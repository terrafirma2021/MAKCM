#!/usr/bin/env python3
"""MAKCM MKOT v1 host package and maintenance transport tool.

The file format deliberately stays small and boring: a package is exactly the
192-byte ``makcm::ota::Manifest`` wire value followed by the ESP32-S3
application image.  OTA frames use the corresponding ``OtaProtocol.h`` layout
and CRC32.  This module has no port discovery and no implicit activation; the
CLI defaults to inspection/dry-run behaviour.

``cryptography`` is required for ECDSA P-256 signing and verification.  The
dependency is imported lazily so that package inspection and protocol tests can
still run on a machine that does not have the crypto extra installed.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, fields as dataclass_fields
import hashlib
import json
import os
from pathlib import Path
import secrets
import struct
import sys
import time
from typing import Any, Mapping, Protocol
import zlib


class OtaToolError(RuntimeError):
    """An input, protocol, or transport failure."""


try:  # optional for inspect/protocol-only use
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.asymmetric.utils import (
        decode_dss_signature,
        encode_dss_signature,
    )
except ImportError:  # pragma: no cover - exercised by dependency checks
    hashes = serialization = ec = None  # type: ignore[assignment]
    decode_dss_signature = encode_dss_signature = None  # type: ignore[assignment]


# OtaProtocol.h constants.  Keep these literals in one place and test their
# wire representation against the C++ protocol in the host test suite.
WIRE_VERSION = 1
MAGIC = b"MKOT"
HEADER_SIZE = 24
CRC_SIZE = 4
MIN_CHUNK_SIZE = 2048
MAX_CHUNK_SIZE = 4096
MAX_WIRE_SIZE = HEADER_SIZE + MAX_CHUNK_SIZE + CRC_SIZE

TARGET_HOST = 0
TARGET_LEFT = 1
TARGET_RIGHT = 2

CMD_HELLO = 0x01
CMD_ENTER_MAINTENANCE = 0x02
CMD_MANIFEST = 0x03
CMD_DATA = 0x04
CMD_PREPARE = 0x05
CMD_STATUS = 0x06
CMD_ACTIVATE = 0x07
CMD_CONFIRM = 0x08
CMD_ABORT = 0x09
CMD_EXIT_MAINTENANCE = 0x0A
CMD_ACK = 0x80
CMD_NACK = 0x81
CMD_STATUS_RESPONSE = 0x82

VALID_COMMANDS = {
    CMD_HELLO,
    CMD_ENTER_MAINTENANCE,
    CMD_MANIFEST,
    CMD_DATA,
    CMD_PREPARE,
    CMD_STATUS,
    CMD_ACTIVATE,
    CMD_CONFIRM,
    CMD_ABORT,
    CMD_EXIT_MAINTENANCE,
    CMD_ACK,
    CMD_NACK,
    CMD_STATUS_RESPONSE,
}

FLAG_RESPONSE = 0x01
FLAG_RETRANSMIT = 0x02
FLAG_FINAL = 0x04
# Explicit negotiation bit for the sole fixed detailed status payload.
FLAG_STATUS_V2 = 0x08
KNOWN_FRAME_FLAGS = FLAG_RESPONSE | FLAG_RETRANSMIT | FLAG_FINAL | FLAG_STATUS_V2

STATUS_V2_VERSION = 2
STATUS_V2_SIZE = 232
COMMAND_RESPONSE_VERSION = 1
COMMAND_RESPONSE_SIZE = 24

STATUS_ROLLBACK_SUPPORTED = 0x0001
STATUS_TRUST_CONFIGURED = 0x0002
STATUS_PEER_COMPATIBLE = 0x0004
STATUS_MAINTENANCE_EXCLUSIVE = 0x0008
STATUS_HEALTH_PASSED = 0x0010
STATUS_LOCAL_BOOT_VALIDATED = 0x0020
STATUS_V2_KNOWN_STATUS_FLAGS = (
    STATUS_ROLLBACK_SUPPORTED | STATUS_TRUST_CONFIGURED | STATUS_PEER_COMPATIBLE
    | STATUS_MAINTENANCE_EXCLUSIVE | STATUS_HEALTH_PASSED
    | STATUS_LOCAL_BOOT_VALIDATED
)

STATE_IDLE = 0
STATE_RECEIVING = 1
STATE_PREPARED = 2
STATE_PENDING_CONFIRM = 3

# NackReason values from OtaProtocol.h.
NACK_REASONS = {
    0: "none",
    1: "malformed",
    2: "crc_mismatch",
    3: "unsupported_version",
    4: "wrong_target",
    5: "wrong_transaction",
    6: "not_in_maintenance",
    7: "busy",
    8: "manifest_rejected",
    9: "wrong_offset",
    10: "flash_write_failed",
    11: "image_verification_failed",
    12: "signature_verification_failed",
    13: "compatibility_rejected",
    14: "invalid_state",
    15: "timeout",
    16: "rollback_unavailable",
}

MANIFEST_VERSION = 1
IMAGE_KIND_APPLICATION = 1
SIGNATURE_ECDSA_P256_SHA256 = 1
ESP32S3_CHIP_ID = 9
MANIFEST_SIGNED_SIZE = 128
MANIFEST_SIGNATURE_SIZE = 64
MANIFEST_SIZE = 192

PACKAGE_FORMAT = "makcm.mkot-package.v1"
DEFAULT_PROTOCOL_VERSION = 1
DEFAULT_CHUNK_SIZE = MAX_CHUNK_SIZE
BOOT_BAUD = 115_200
FAST_BAUD = 4_000_000
SPEED_SWITCH = bytes((0xDE, 0xAD, 0x05, 0x00, 0xA5, 0x00, 0x09, 0x3D, 0x00))
DEFAULT_UART_REQUEST_TIMEOUT = 5.0
# Re-establish a text-frame boundary after opening the selected baud.  This is
# deliberately harmless to the binary decoder.  It also clears any garbage
# left in the firmware's text line buffer when a repeated invocation sent
# the 115200-baud switch sentinel to a board that was already running at 4 M.
SERIAL_FRAME_BOUNDARY = b"\n"
LEFT_PARTITION_SCHEMA_V1 = 0x4C010001
RIGHT_PARTITION_SCHEMA_V1 = 0x52010001
HARDWARE_REVISION_R1 = "makcm-s3-r1"
LEFT_PROJECT_NAME = "makcm_left_idf"
RIGHT_PROJECT_NAME = "makcm_right_idf"


def _require_crypto() -> None:
    if ec is None or serialization is None or hashes is None:
        raise OtaToolError(
            "cryptography is required for ECDSA P-256 signing/verification; "
            "install it with: python -m pip install cryptography"
        )


def _role_value(role: str | int) -> int:
    if isinstance(role, bool):
        raise OtaToolError("role must be left or right")
    if isinstance(role, int):
        value = role
    else:
        value = {"left": TARGET_LEFT, "right": TARGET_RIGHT}.get(
            role.lower(), -1
        )
    if value not in (TARGET_LEFT, TARGET_RIGHT):
        raise OtaToolError("role must be exactly left or right")
    return value


def role_name(role: int) -> str:
    if role == TARGET_LEFT:
        return "left"
    if role == TARGET_RIGHT:
        return "right"
    return f"target-{role}"


def _u16(value: object, label: str, *, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise OtaToolError(f"{label} must be an integer")
    if value < minimum or value > 0xFFFF:
        raise OtaToolError(f"{label} must be in [{minimum}, 65535]")
    return value


def _u32(value: object, label: str, *, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise OtaToolError(f"{label} must be an integer")
    if value < minimum or value > 0xFFFFFFFF:
        raise OtaToolError(f"{label} must be in [{minimum}, 4294967295]")
    return value


def _fixed_text(value: str, capacity: int, label: str) -> bytes:
    if not isinstance(value, str) or not value:
        raise OtaToolError(f"{label} must be a non-empty string")
    encoded = value.encode("utf-8")
    if b"\x00" in encoded or len(encoded) >= capacity:
        raise OtaToolError(f"{label} must fit in {capacity - 1} UTF-8 bytes")
    return encoded + b"\x00" * (capacity - len(encoded))


def _hex_bytes(value: str | bytes, length: int, label: str) -> bytes:
    if isinstance(value, bytes):
        result = value
    elif isinstance(value, str):
        try:
            result = bytes.fromhex(value)
        except ValueError as exc:
            raise OtaToolError(f"{label} must be hexadecimal") from exc
    else:
        raise OtaToolError(f"{label} must be hexadecimal")
    if len(result) != length:
        raise OtaToolError(f"{label} must contain exactly {length} bytes")
    return result


def _sha256(data: bytes) -> bytes:
    return hashlib.sha256(data).digest()


def _read_path(path: str | Path, label: str) -> bytes:
    try:
        return Path(path).read_bytes()
    except OSError as exc:
        raise OtaToolError(f"cannot read {label} {path}: {exc}") from exc


def _looks_like_merged_image(data: bytes) -> bool:
    """Detect common esptool merged/full-image layouts.

    An ESP application and a bootloader both start with 0xE9, so checking only
    the magic byte is unsafe.  Merged images produced by ``merge-bin`` carry a
    partition-table magic (AA 50) at 0x8000 and generally a second image at
    0x10000.  The checks are intentionally conservative and only reject a
    second image when both header fields look valid.
    """

    for offset in (0x8000, 0x9000, 0x10000):
        if offset + 2 <= len(data) and data[offset : offset + 2] == b"\xAA\x50":
            return True
    if len(data) >= 0x10000 + 4 and data[0x10000] == 0xE9:
        count = data[0x10001]
        chip = data[0x10002]
        if 1 <= count <= 16 and chip in (0x00, 0x02):
            return True
    return False


def validate_application_image(data: bytes, *, label: str = "image") -> None:
    """Reject non-ESP32-S3 application and merged/full images.

    ``esptool merge-bin`` output is rejected even though it also begins with an
    ESP image header.  The host never sends bootloader, partition-table, or
    merged bytes to an OTA application slot.
    """

    if len(data) < 32:
        raise OtaToolError(f"{label} is too short to be an ESP32-S3 application")
    if data[0] != 0xE9:
        raise OtaToolError(
            f"{label} is not an ESP image (expected 0xE9 application header)"
        )
    if data[2] != 0x02:  # ESP32-S3 chip id in the ESP image header
        raise OtaToolError(f"{label} is not an ESP32-S3 application image")
    segment_count = data[1]
    if not 1 <= segment_count <= 16:
        raise OtaToolError(f"{label} has an invalid ESP segment count")
    if _looks_like_merged_image(data):
        raise OtaToolError(
            f"{label} looks like a merged/full flash image; provide the "
            "role-specific application .bin only"
        )


# ESP-IDF's ``esp_app_desc_t`` is embedded in the application bytes that are
# eventually signed.  Manifest metadata alone cannot prove what was compiled
# into an image, so production package creation validates this descriptor.
ESP_APP_DESC_MAGIC = 0xABCD5432
ESP_APP_DESC_MAGIC_WIRE = struct.pack("<I", ESP_APP_DESC_MAGIC)
ESP_APP_DESC_MIN_SIZE = 176
EMBEDDED_IDENTITY_MAGIC = b"MKIMGV1\x00"
EMBEDDED_IDENTITY_TRAILER = b"IDENDV1\x00"
EMBEDDED_IDENTITY_SIZE = 116
EMBEDDED_IDENTITY_VERSION = 1
LEFT_DESCRIPTOR_VERSION = "makcm-g{}"
RIGHT_DESCRIPTOR_IDF_VERSION = "v5.5.4"


@dataclass(frozen=True)
class AppDescriptor:
    version: str
    project_name: str
    idf_version: str
    app_elf_sha256: bytes
    offset: int


@dataclass(frozen=True)
class EmbeddedImageIdentity:
    role: int
    generation: int
    partition_schema: int
    protocol_version: int
    hardware_revision: str
    project_name: str
    app_version: str
    offset: int


def _descriptor_text(data: bytes, offset: int, capacity: int, label: str) -> str:
    raw = data[offset : offset + capacity]
    end = raw.find(b"\x00")
    if end <= 0:
        raise ValueError(f"descriptor {label} is not NUL-terminated")
    try:
        value = raw[:end].decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ValueError(f"descriptor {label} is not UTF-8") from exc
    if any(byte < 0x20 or byte > 0x7E for byte in raw[:end]):
        raise ValueError(f"descriptor {label} contains non-printable bytes")
    return value


def parse_app_descriptor(data: bytes, *, label: str = "image") -> AppDescriptor:
    """Return the intrinsic ESP app descriptor or fail closed.

    Random occurrences of the magic word are ignored unless the identity
    strings and ELF hash are structurally valid.  A package without a
    verifiable descriptor must not be rewrapped under a new generation.
    """

    start = 0
    while True:
        found = data.find(ESP_APP_DESC_MAGIC_WIRE, start)
        if found < 0:
            break
        start = found + 1
        if found + ESP_APP_DESC_MIN_SIZE > len(data):
            continue
        try:
            version = _descriptor_text(data, found + 16, 32, "version")
            project = _descriptor_text(data, found + 48, 32, "project_name")
            idf = _descriptor_text(data, found + 112, 32, "idf_version")
        except ValueError:
            continue
        elf_hash = bytes(data[found + 144 : found + 176])
        if not any(elf_hash):
            continue
        return AppDescriptor(version, project, idf, elf_hash, found)
    raise OtaToolError(
        f"{label} has no valid intrinsic ESP app descriptor; refusing to "
        "sign/repackage it under manifest metadata"
    )


def _parse_embedded_identity_at(
    data: bytes, offset: int
) -> EmbeddedImageIdentity | None:
    if offset + EMBEDDED_IDENTITY_SIZE > len(data):
        return None
    wire = data[offset : offset + EMBEDDED_IDENTITY_SIZE]
    if wire[:8] != EMBEDDED_IDENTITY_MAGIC or wire[108:116] != EMBEDDED_IDENTITY_TRAILER:
        return None
    version, size = struct.unpack_from("<HH", wire, 8)
    if version != EMBEDDED_IDENTITY_VERSION or size != EMBEDDED_IDENTITY_SIZE:
        return None
    if wire[13:16] != b"\0\0\0" or wire[26:28] != b"\0\0":
        return None
    role = wire[12]
    if role not in (TARGET_LEFT, TARGET_RIGHT):
        return None
    generation, partition_schema = struct.unpack_from("<II", wire, 16)
    protocol_version = struct.unpack_from("<H", wire, 24)[0]
    if generation == 0 or partition_schema == 0 or protocol_version == 0:
        return None
    try:
        hardware = _descriptor_text(wire, 28, 16, "hardware_revision")
        project = _descriptor_text(wire, 44, 32, "project_name")
        app_version = _descriptor_text(wire, 76, 32, "app_version")
    except ValueError:
        return None
    return EmbeddedImageIdentity(
        role=role, generation=generation, partition_schema=partition_schema,
        protocol_version=protocol_version, hardware_revision=hardware,
        project_name=project, app_version=app_version, offset=offset,
    )


def parse_embedded_identities(
    data: bytes, *, label: str = "image"
) -> list[EmbeddedImageIdentity]:
    """Find all legal MAKCM embedded identity records in signed image bytes."""

    identities: list[EmbeddedImageIdentity] = []
    start = 0
    while True:
        found = data.find(EMBEDDED_IDENTITY_MAGIC, start)
        if found < 0:
            break
        start = found + 1
        identity = _parse_embedded_identity_at(data, found)
        if identity is not None:
            identities.append(identity)
    if not identities:
        raise OtaToolError(
            f"{label} has no legal MAKCM embedded image identity marker"
        )
    return identities


def validate_intrinsic_identity(
    data: bytes, *, role: str | int, generation: int, label: str = "image"
) -> AppDescriptor:
    """Require role-specific descriptor and embedded package identity."""

    role = _role_value(role)
    descriptor = parse_app_descriptor(data, label=label)
    identities = parse_embedded_identities(data, label=label)
    if len(identities) != 1:
        raise OtaToolError(
            f"{label} has {len(identities)} legal embedded identity markers; "
            "expected exactly one"
        )
    identity = identities[0]
    expected_schema = (
        LEFT_PARTITION_SCHEMA_V1
        if role == TARGET_LEFT else RIGHT_PARTITION_SCHEMA_V1
    )
    expected_project = (
        LEFT_PROJECT_NAME if role == TARGET_LEFT else RIGHT_PROJECT_NAME
    )
    expected_version = LEFT_DESCRIPTOR_VERSION.format(generation)
    if (
        identity.role != role
        or identity.generation != generation
        or identity.partition_schema != expected_schema
        or identity.protocol_version != DEFAULT_PROTOCOL_VERSION
        or identity.hardware_revision != HARDWARE_REVISION_R1
        or identity.project_name != expected_project
        or identity.app_version != expected_version
    ):
        raise OtaToolError(
            f"{label} embedded identity does not exactly match role/generation/"
            "schema/protocol/hardware/project/app version"
        )
    if descriptor.project_name != expected_project:
        raise OtaToolError(
            f"{label} intrinsic project {descriptor.project_name!r} != "
            f"{expected_project!r}"
        )
    if descriptor.version != expected_version:
        raise OtaToolError(
            f"{label} intrinsic version {descriptor.version!r} does not prove "
            f"generation {generation} (expected {expected_version!r})"
        )
    if role == TARGET_RIGHT:
        if descriptor.idf_version != RIGHT_DESCRIPTOR_IDF_VERSION:
            raise OtaToolError(
                f"{label} intrinsic IDF {descriptor.idf_version!r} is not the "
                f"required {RIGHT_DESCRIPTOR_IDF_VERSION}"
            )
    return descriptor


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def _valid_frame_flags(command: int, flags: int) -> bool:
    """Return whether command/flags match the protocol's wire contract."""
    if flags & ~KNOWN_FRAME_FLAGS:
        return False
    if command == CMD_DATA:
        return (flags & ~(FLAG_FINAL | FLAG_RETRANSMIT)) == 0
    if command == CMD_STATUS:
        return flags in (FLAG_STATUS_V2,
                         FLAG_STATUS_V2 | FLAG_RETRANSMIT)
    if command in (CMD_ACK, CMD_NACK):
        return flags == FLAG_RESPONSE
    if command == CMD_STATUS_RESPONSE:
        return flags == (FLAG_RESPONSE | FLAG_STATUS_V2)
    return command in VALID_COMMANDS and flags in (0, FLAG_RETRANSMIT)


def encode_frame(
    *,
    target: int,
    command: int,
    transaction_id: int,
    offset: int = 0,
    sequence: int = 0,
    payload: bytes = b"",
    flags: int = 0,
) -> bytes:
    """Encode one MKOT v1 frame, matching ``OtaProtocol.h`` exactly."""

    if target not in (TARGET_HOST, TARGET_LEFT, TARGET_RIGHT):
        raise OtaToolError("invalid frame target")
    if command not in VALID_COMMANDS:
        raise OtaToolError("invalid frame command")
    if not 0 < transaction_id <= 0xFFFFFFFF:
        raise OtaToolError("transaction_id must be non-zero uint32")
    _u32(offset, "offset")
    _u32(sequence, "sequence")
    if sequence in (0, 0xFFFFFFFF):
        raise OtaToolError("sequence 0 and 0xffffffff are reserved")
    if len(payload) > MAX_CHUNK_SIZE:
        raise OtaToolError(f"frame payload exceeds {MAX_CHUNK_SIZE} bytes")
    if not 0 <= flags <= 0xFF:
        raise OtaToolError("flags must be uint8")
    if not _valid_frame_flags(command, flags):
        raise OtaToolError("invalid flags for frame command")
    header = struct.pack(
        "<4sBBBBIIHHI",
        MAGIC,
        WIRE_VERSION,
        target,
        command,
        flags,
        transaction_id,
        offset,
        len(payload),
        0,
        sequence,
    )
    return header + payload + struct.pack("<I", crc32(header[4:] + payload))


@dataclass(frozen=True)
class Frame:
    target: int
    command: int
    flags: int
    transaction_id: int
    offset: int
    sequence: int
    payload: bytes


@dataclass(frozen=True)
class CommandResponsePayload:
    """The fixed 24-byte ACK/NACK command result from OtaProtocol.h."""

    state: int
    last_command: int
    reason: int
    next_offset: int
    target_generation: int
    active_generation: int
    status_flags: int


def decode_command_response_payload(payload: bytes) -> CommandResponsePayload:
    """Decode the compact ACK/NACK result; Status uses StatusPayloadV2."""

    if len(payload) != COMMAND_RESPONSE_SIZE:
        raise OtaToolError(
            f"OTA command response must be exactly {COMMAND_RESPONSE_SIZE} bytes"
        )
    if (payload[0] != COMMAND_RESPONSE_VERSION or payload[3] != 0 or
            any(payload[6:8])):
        raise OtaToolError("invalid OTA command response header/reserved bytes")
    state = payload[1]
    last_command = payload[2]
    reason = struct.unpack_from("<H", payload, 4)[0]
    status_flags = struct.unpack_from("<I", payload, 20)[0]
    if state not in (STATE_IDLE, STATE_RECEIVING, STATE_PREPARED,
                     STATE_PENDING_CONFIRM):
        raise OtaToolError("invalid OTA command response state")
    if last_command not in VALID_COMMANDS:
        raise OtaToolError("invalid OTA command response last command")
    if reason > 16:
        raise OtaToolError("invalid OTA command response reason")
    if status_flags & ~STATUS_V2_KNOWN_STATUS_FLAGS:
        raise OtaToolError("unknown OTA command response status flag bits")
    return CommandResponsePayload(
        state=state,
        last_command=last_command,
        reason=reason,
        next_offset=struct.unpack_from("<I", payload, 8)[0],
        target_generation=struct.unpack_from("<I", payload, 12)[0],
        active_generation=struct.unpack_from("<I", payload, 16)[0],
        status_flags=status_flags,
    )


def _command_response_semantics(
    response_command: int, response: CommandResponsePayload
) -> None:
    if response_command not in (CMD_ACK, CMD_NACK):
        raise OtaToolError("compact payload requires an ACK or NACK frame")
    if (response_command == CMD_NACK) != (response.reason != 0):
        raise OtaToolError("OTA ACK/NACK command and reason disagree")


@dataclass(frozen=True)
class StatusPayloadV2:
    """Strict 232-byte projection of ``common/OtaStatusV2.h``."""

    role: int
    state: int
    last_command: int
    last_response_command: int
    reason: int
    status_flags: int
    next_offset: int
    transaction_id: int
    target_generation: int
    running_generation: int
    partition_schema: int
    image_size: int
    reset_reason: int
    last_ota_error: int
    last_state_query_error: int
    running_slot: int
    boot_slot: int
    target_slot: int
    image_state: int
    target_image_sha256: bytes
    running_image_sha256: bytes
    manifest_sha256: bytes
    hardware_revision: str
    project_name: str
    app_version: str


def _status_text(payload: bytes, offset: int, capacity: int, label: str) -> str:
    raw = payload[offset : offset + capacity]
    end = raw.find(b"\x00")
    if end <= 0 or any(raw[end + 1 :]):
        raise OtaToolError(f"Status V2 {label} field is not canonical text")
    try:
        return raw[:end].decode("utf-8")
    except UnicodeDecodeError as exc:
        raise OtaToolError(f"Status V2 {label} is not UTF-8") from exc


def _status_semantics(status: StatusPayloadV2) -> None:
    if status.role not in (TARGET_LEFT, TARGET_RIGHT):
        raise OtaToolError("invalid Status V2 board role")
    if status.state not in (STATE_IDLE, STATE_RECEIVING, STATE_PREPARED,
                            STATE_PENDING_CONFIRM):
        raise OtaToolError("invalid Status V2 state")
    if status.last_command not in VALID_COMMANDS:
        raise OtaToolError("invalid Status V2 last command")
    if status.last_response_command not in (CMD_ACK, CMD_NACK,
                                             CMD_STATUS_RESPONSE):
        raise OtaToolError("invalid Status V2 response command")
    if not 0 <= status.reason <= 16:
        raise OtaToolError("invalid Status V2 reason")
    if (status.last_response_command == CMD_NACK) != (status.reason != 0):
        raise OtaToolError("Status V2 response/reason disagree")
    if status.status_flags & ~STATUS_V2_KNOWN_STATUS_FLAGS:
        raise OtaToolError("unknown Status V2 status flag bits")
    if status.running_generation == 0 or status.partition_schema == 0:
        raise OtaToolError("Status V2 running identity is incomplete")
    if not any(status.running_image_sha256):
        raise OtaToolError("Status V2 running image hash is empty")
    if status.last_state_query_error == 0:
        if status.running_slot == 0xFF or status.boot_slot == 0xFF or \
                status.image_state > 5:
            raise OtaToolError("Status V2 partition state is incomplete")
    elif (status.running_slot != 0xFF or status.boot_slot != 0xFF or
          status.image_state != 0xFF):
        raise OtaToolError("Status V2 failed partition query has stale facts")
    transient = status.state in (STATE_RECEIVING, STATE_PREPARED)
    if transient:
        if (status.transaction_id == 0 or status.target_generation == 0 or
                status.image_size == 0 or status.next_offset > status.image_size or
                status.target_slot == 0xFF or
                not any(status.target_image_sha256) or
                not any(status.manifest_sha256)):
            raise OtaToolError("Status V2 transaction identity is incomplete")
        if status.state == STATE_PREPARED and \
                status.next_offset != status.image_size:
            raise OtaToolError("prepared Status V2 has incomplete image")
    elif (status.transaction_id != 0 or status.target_generation != 0 or
          status.image_size != 0 or status.next_offset != 0 or
          status.target_slot != 0xFF or any(status.target_image_sha256) or
          any(status.manifest_sha256)):
        raise OtaToolError("non-transient Status V2 carries transaction facts")


def decode_status_v2(payload: bytes) -> StatusPayloadV2:
    if len(payload) != STATUS_V2_SIZE:
        raise OtaToolError(
            f"OTA Status V2 payload must be exactly {STATUS_V2_SIZE} bytes"
        )
    if payload[0] != STATUS_V2_VERSION or any(payload[9:12]) or any(payload[14:16]):
        raise OtaToolError("invalid OTA Status V2 header/reserved bytes")
    status = StatusPayloadV2(
        role=payload[1], state=payload[2], last_command=payload[3],
        last_response_command=payload[4], running_slot=payload[5],
        boot_slot=payload[6], target_slot=payload[7], image_state=payload[8],
        reason=struct.unpack_from("<H", payload, 12)[0],
        status_flags=struct.unpack_from("<I", payload, 16)[0],
        next_offset=struct.unpack_from("<I", payload, 20)[0],
        transaction_id=struct.unpack_from("<I", payload, 24)[0],
        target_generation=struct.unpack_from("<I", payload, 28)[0],
        running_generation=struct.unpack_from("<I", payload, 32)[0],
        partition_schema=struct.unpack_from("<I", payload, 36)[0],
        image_size=struct.unpack_from("<I", payload, 40)[0],
        reset_reason=struct.unpack_from("<I", payload, 44)[0],
        last_ota_error=struct.unpack_from("<i", payload, 48)[0],
        last_state_query_error=struct.unpack_from("<i", payload, 52)[0],
        target_image_sha256=bytes(payload[56:88]),
        running_image_sha256=bytes(payload[88:120]),
        manifest_sha256=bytes(payload[120:152]),
        hardware_revision=_status_text(payload, 152, 16, "hardware_revision"),
        project_name=_status_text(payload, 168, 32, "project_name"),
        app_version=_status_text(payload, 200, 32, "app_version"),
    )
    _status_semantics(status)
    return status


def encode_status_v2(status: StatusPayloadV2) -> bytes:
    _status_semantics(status)
    wire = bytearray(STATUS_V2_SIZE)
    wire[0:9] = bytes((
        STATUS_V2_VERSION, status.role, status.state, status.last_command,
        status.last_response_command, status.running_slot, status.boot_slot,
        status.target_slot, status.image_state,
    ))
    struct.pack_into("<H", wire, 12, status.reason)
    for offset, value in (
        (16, status.status_flags), (20, status.next_offset),
        (24, status.transaction_id), (28, status.target_generation),
        (32, status.running_generation), (36, status.partition_schema),
        (40, status.image_size), (44, status.reset_reason),
    ):
        struct.pack_into("<I", wire, offset, value)
    struct.pack_into("<i", wire, 48, status.last_ota_error)
    struct.pack_into("<i", wire, 52, status.last_state_query_error)
    wire[56:88] = _hex_bytes(status.target_image_sha256, 32,
                             "target_image_sha256")
    wire[88:120] = _hex_bytes(status.running_image_sha256, 32,
                              "running_image_sha256")
    wire[120:152] = _hex_bytes(status.manifest_sha256, 32, "manifest_sha256")
    wire[152:168] = _fixed_text(status.hardware_revision, 16,
                                "hardware_revision")
    wire[168:200] = _fixed_text(status.project_name, 32, "project_name")
    wire[200:232] = _fixed_text(status.app_version, 32, "app_version")
    return bytes(wire)



def decode_frame(wire: bytes) -> Frame:
    if len(wire) < HEADER_SIZE + CRC_SIZE:
        raise OtaToolError("short MKOT frame")
    if wire[:4] != MAGIC or wire[4] != WIRE_VERSION:
        raise OtaToolError("bad MKOT magic/version")
    (
        _magic,
        _version,
        target,
        command,
        flags,
        transaction_id,
        offset,
        length,
        reserved,
        sequence,
    ) = struct.unpack_from("<4sBBBBIIHHI", wire, 0)
    if target not in (TARGET_HOST, TARGET_LEFT, TARGET_RIGHT):
        raise OtaToolError("invalid frame target")
    if command not in VALID_COMMANDS:
        raise OtaToolError("invalid frame command")
    if transaction_id == 0 or sequence in (0, 0xFFFFFFFF) or reserved != 0:
        raise OtaToolError("invalid frame transaction/reserved fields")
    if not _valid_frame_flags(command, flags):
        raise OtaToolError("invalid flags for frame command")
    expected_len = HEADER_SIZE + length + CRC_SIZE
    if length > MAX_CHUNK_SIZE or len(wire) != expected_len:
        raise OtaToolError("invalid frame payload length")
    expected_crc = struct.unpack_from("<I", wire, HEADER_SIZE + length)[0]
    actual_crc = crc32(wire[4 : HEADER_SIZE] + wire[HEADER_SIZE : HEADER_SIZE + length])
    if expected_crc != actual_crc:
        raise OtaToolError("MKOT frame CRC mismatch")
    return Frame(
        target=target,
        command=command,
        flags=flags,
        transaction_id=transaction_id,
        offset=offset,
        sequence=sequence,
        payload=wire[HEADER_SIZE : HEADER_SIZE + length],
    )


class FrameDecoder:
    """Streaming decoder with MKOT magic resynchronisation."""

    def __init__(self) -> None:
        self._buffer = bytearray()
        self._expected: int | None = None

    def feed(self, data: bytes) -> list[Frame]:
        self._buffer.extend(data)
        frames: list[Frame] = []
        while True:
            start = self._buffer.find(MAGIC)
            if start < 0:
                # Keep a possible partial magic prefix.
                self._buffer[:] = self._buffer[-(len(MAGIC) - 1) :]
                break
            if start:
                del self._buffer[:start]
            if len(self._buffer) < HEADER_SIZE:
                break
            length = struct.unpack_from("<H", self._buffer, 16)[0]
            expected = HEADER_SIZE + length + CRC_SIZE
            if length > MAX_CHUNK_SIZE:
                del self._buffer[0]
                continue
            if len(self._buffer) < expected:
                break
            wire = bytes(self._buffer[:expected])
            del self._buffer[:expected]
            try:
                frames.append(decode_frame(wire))
            except OtaToolError:
                # A CRC/header failure must not poison later valid frames.
                continue
        return frames


@dataclass(frozen=True)
class Manifest:
    version: int
    role: int
    image_kind: int
    protocol_version: int
    chip_id: int
    generation: int
    min_peer_generation: int
    max_peer_generation: int
    partition_schema: int
    image_size: int
    chunk_size: int
    signature_algorithm: int
    hardware_revision: str
    project_name: str
    image_sha256: bytes
    signer_key_id: bytes
    signature: bytes

    @property
    def signed_bytes(self) -> bytes:
        return encode_manifest(self, include_signature=False)

    @property
    def wire(self) -> bytes:
        return encode_manifest(self, include_signature=True)


def _decode_text(raw: bytes, label: str, capacity: int) -> str:
    end = raw.find(b"\x00")
    if end <= 0:
        raise OtaToolError(f"manifest {label} is not NUL-terminated")
    if any(raw[end + 1 :]):
        # C++ accepts arbitrary padding, but strict package generation avoids
        # non-deterministic trailing bytes and catches corrupt packages.
        raise OtaToolError(f"manifest {label} has non-zero padding")
    try:
        value = raw[:end].decode("utf-8")
    except UnicodeDecodeError as exc:
        raise OtaToolError(f"manifest {label} is not UTF-8") from exc
    if len(raw) != capacity or not value:
        raise OtaToolError(f"manifest {label} is malformed")
    return value


def encode_manifest(manifest: Manifest, *, include_signature: bool = True) -> bytes:
    role = _role_value(manifest.role)
    if manifest.version != MANIFEST_VERSION:
        raise OtaToolError("unsupported manifest version")
    if manifest.image_kind != IMAGE_KIND_APPLICATION:
        raise OtaToolError("manifest image_kind must be application")
    _u16(manifest.protocol_version, "protocol_version")
    if manifest.chip_id != ESP32S3_CHIP_ID:
        raise OtaToolError("manifest chip_id must be ESP32-S3 (9)")
    generation = _u32(manifest.generation, "generation", minimum=1)
    min_peer = _u32(manifest.min_peer_generation, "min_peer_generation")
    max_peer = _u32(manifest.max_peer_generation, "max_peer_generation")
    if min_peer > max_peer:
        raise OtaToolError("min_peer_generation must not exceed max_peer_generation")
    partition_schema = _u32(manifest.partition_schema, "partition_schema", minimum=1)
    image_size = _u32(manifest.image_size, "image_size", minimum=1)
    chunk_size = _u16(manifest.chunk_size, "chunk_size")
    if not MIN_CHUNK_SIZE <= chunk_size <= MAX_CHUNK_SIZE:
        raise OtaToolError(f"chunk_size must be in [{MIN_CHUNK_SIZE}, {MAX_CHUNK_SIZE}]")
    if manifest.signature_algorithm != SIGNATURE_ECDSA_P256_SHA256:
        raise OtaToolError("manifest signature_algorithm must be ECDSA P-256 SHA-256")
    hardware = _fixed_text(manifest.hardware_revision, 16, "hardware_revision")
    project = _fixed_text(manifest.project_name, 32, "project_name")
    image_hash = _hex_bytes(manifest.image_sha256, 32, "image_sha256")
    signer_id = _hex_bytes(manifest.signer_key_id, 16, "signer_key_id")
    if signer_id == b"\x00" * 16:
        raise OtaToolError("signer_key_id must not be all zero")
    signature = _hex_bytes(manifest.signature, MANIFEST_SIGNATURE_SIZE, "signature")
    signed = struct.pack(
        "<HBBHHIIIIIHBB",
        MANIFEST_VERSION,
        role,
        IMAGE_KIND_APPLICATION,
        manifest.protocol_version,
        ESP32S3_CHIP_ID,
        generation,
        min_peer,
        max_peer,
        partition_schema,
        image_size,
        chunk_size,
        SIGNATURE_ECDSA_P256_SHA256,
        0,
    ) + hardware + project + image_hash + signer_id
    if len(signed) != MANIFEST_SIGNED_SIZE:
        raise AssertionError(f"manifest signed prefix is {len(signed)} bytes")
    return signed + (signature if include_signature else b"" )


def decode_manifest(wire: bytes) -> Manifest:
    if len(wire) != MANIFEST_SIZE:
        raise OtaToolError("manifest must be exactly 192 bytes")
    (
        version,
        role,
        image_kind,
        protocol_version,
        chip_id,
        generation,
        min_peer_generation,
        max_peer_generation,
        partition_schema,
        image_size,
        chunk_size,
        signature_algorithm,
        reserved,
    ) = struct.unpack_from("<HBBHHIIIIIHBB", wire, 0)
    if reserved != 0:
        raise OtaToolError("manifest reserved byte must be zero")
    if version != MANIFEST_VERSION:
        raise OtaToolError("unsupported manifest version")
    role = _role_value(role)
    if image_kind != IMAGE_KIND_APPLICATION:
        raise OtaToolError("manifest is not an application image")
    if chip_id != ESP32S3_CHIP_ID:
        raise OtaToolError("manifest chip_id is not ESP32-S3")
    if generation == 0 or min_peer_generation > max_peer_generation:
        raise OtaToolError("manifest generation/peer range is invalid")
    if partition_schema == 0 or image_size == 0:
        raise OtaToolError("manifest partition_schema/image_size is invalid")
    if not MIN_CHUNK_SIZE <= chunk_size <= MAX_CHUNK_SIZE:
        raise OtaToolError("manifest chunk_size is outside protocol limits")
    if signature_algorithm != SIGNATURE_ECDSA_P256_SHA256:
        raise OtaToolError("manifest signature algorithm is unsupported")
    hardware = _decode_text(wire[32:48], "hardware_revision", 16)
    project = _decode_text(wire[48:80], "project_name", 32)
    signature = wire[MANIFEST_SIGNED_SIZE:MANIFEST_SIZE]
    if signature == b"\x00" * MANIFEST_SIGNATURE_SIZE:
        raise OtaToolError("manifest signature is empty")
    return Manifest(
        version=version,
        role=role,
        image_kind=image_kind,
        protocol_version=protocol_version,
        chip_id=chip_id,
        generation=generation,
        min_peer_generation=min_peer_generation,
        max_peer_generation=max_peer_generation,
        partition_schema=partition_schema,
        image_size=image_size,
        chunk_size=chunk_size,
        signature_algorithm=signature_algorithm,
        hardware_revision=hardware,
        project_name=project,
        image_sha256=wire[80:112],
        signer_key_id=wire[112:128],
        signature=signature,
    )


def _load_private_key(path: str | Path):
    _require_crypto()
    try:
        key = serialization.load_pem_private_key(_read_path(path, "private key"), password=None)
    except Exception as exc:  # cryptography has several loader exceptions
        raise OtaToolError(f"cannot load ECDSA private key {path}: {exc}") from exc
    if not isinstance(key, ec.EllipticCurvePrivateKey) or not isinstance(
        key.curve, ec.SECP256R1
    ):
        raise OtaToolError("private key must be an ECDSA P-256 key")
    return key


def _load_public_key(path: str | Path):
    _require_crypto()
    try:
        key = serialization.load_pem_public_key(_read_path(path, "public key"))
    except Exception as exc:
        raise OtaToolError(f"cannot load ECDSA public key {path}: {exc}") from exc
    if not isinstance(key, ec.EllipticCurvePublicKey) or not isinstance(
        key.curve, ec.SECP256R1
    ):
        raise OtaToolError("public key must be an ECDSA P-256 key")
    return key


def write_trust_anchor(
    public_key_path: str | Path,
    output_path: str | Path,
    *,
    signer_key_id: str | bytes,
    overwrite: bool = False,
) -> dict[str, str]:
    """Generate the public-only firmware include consumed by both boards."""
    key = _load_public_key(public_key_path)
    key_id = _hex_bytes(signer_key_id, 16, "signer_key_id")
    if key_id == b"\x00" * 16:
        raise OtaToolError("signer_key_id must not be all zero")
    numbers = key.public_numbers()
    public_x = numbers.x.to_bytes(32, "big")
    public_y = numbers.y.to_bytes(32, "big")
    output = Path(output_path)
    if output.exists() and not overwrite:
        raise OtaToolError(
            f"trust anchor already exists: {output}; pass --force to replace it"
        )

    def declaration(name: str, value: bytes) -> str:
        body = ", ".join(f"0x{byte:02x}" for byte in value)
        return f"constexpr uint8_t {name}[{len(value)}] = {{{body}}};"

    contents = (
        "// Generated public trust anchor. Do not place a private key here.\n"
        + declaration("kSignerKeyId", key_id)
        + "\n"
        + declaration("kPublicX", public_x)
        + "\n"
        + declaration("kPublicY", public_y)
        + "\n"
    )
    try:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(contents, encoding="ascii", newline="\n")
    except OSError as exc:
        raise OtaToolError(f"cannot write trust anchor {output}: {exc}") from exc
    return {
        "output": str(output),
        "signer_key_id": key_id.hex(),
        "public_key_sha256": hashlib.sha256(
            key.public_bytes(
                serialization.Encoding.DER,
                serialization.PublicFormat.SubjectPublicKeyInfo,
            )
        ).hexdigest(),
    }


def sign_manifest(manifest: Manifest, private_key_path: str | Path) -> Manifest:
    key = _load_private_key(private_key_path)
    der = key.sign(manifest.signed_bytes, ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(der)
    signature = r.to_bytes(32, "big") + s.to_bytes(32, "big")
    return Manifest(**{**manifest.__dict__, "signature": signature})


def verify_manifest_signature(
    manifest: Manifest,
    public_key_path: str | Path,
    *,
    expected_signer_key_id: str | bytes | None = None,
) -> bool:
    key = _load_public_key(public_key_path)
    if expected_signer_key_id is not None and manifest.signer_key_id != _hex_bytes(
        expected_signer_key_id, 16, "expected_signer_key_id"
    ):
        return False
    if len(manifest.signature) != 64:
        return False
    r = int.from_bytes(manifest.signature[:32], "big")
    s = int.from_bytes(manifest.signature[32:], "big")
    if not r or not s:
        return False
    try:
        key.verify(
            encode_dss_signature(r, s),
            manifest.signed_bytes,
            ec.ECDSA(hashes.SHA256()),
        )
    except Exception:
        return False
    return True


def make_manifest(
    image: bytes,
    *,
    role: str | int,
    generation: int,
    min_peer_generation: int,
    max_peer_generation: int,
    partition_schema: int,
    hardware_revision: str,
    project_name: str,
    signer_key_id: str | bytes,
    protocol_version: int = DEFAULT_PROTOCOL_VERSION,
    chunk_size: int = DEFAULT_CHUNK_SIZE,
) -> Manifest:
    validate_application_image(image)
    if not min_peer_generation <= max_peer_generation:
        raise OtaToolError("min_peer_generation must not exceed max_peer_generation")
    return Manifest(
        version=MANIFEST_VERSION,
        role=_role_value(role),
        image_kind=IMAGE_KIND_APPLICATION,
        protocol_version=_u16(protocol_version, "protocol_version"),
        chip_id=ESP32S3_CHIP_ID,
        generation=_u32(generation, "generation", minimum=1),
        min_peer_generation=_u32(min_peer_generation, "min_peer_generation"),
        max_peer_generation=_u32(max_peer_generation, "max_peer_generation"),
        partition_schema=_u32(partition_schema, "partition_schema", minimum=1),
        image_size=_u32(len(image), "image_size", minimum=1),
        chunk_size=_u16(chunk_size, "chunk_size"),
        signature_algorithm=SIGNATURE_ECDSA_P256_SHA256,
        hardware_revision=hardware_revision,
        project_name=project_name,
        image_sha256=_sha256(image),
        signer_key_id=_hex_bytes(signer_key_id, 16, "signer_key_id"),
        signature=b"\x01" * MANIFEST_SIGNATURE_SIZE,
    )


def build_package(
    image: bytes,
    *,
    private_key_path: str | Path,
    **manifest_kwargs: Any,
) -> tuple[bytes, Manifest]:
    manifest = make_manifest(image, **manifest_kwargs)
    manifest = sign_manifest(manifest, private_key_path)
    package = manifest.wire + image
    if len(package) != MANIFEST_SIZE + len(image):
        raise AssertionError("package length mismatch")
    return package, manifest


def write_package(
    image_path: str | Path,
    output_path: str | Path,
    *,
    private_key_path: str | Path,
    **manifest_kwargs: Any,
) -> dict[str, Any]:
    image = _read_path(image_path, "application image")
    package, manifest = build_package(
        image, private_key_path=private_key_path, **manifest_kwargs
    )
    try:
        Path(output_path).write_bytes(package)
    except OSError as exc:
        raise OtaToolError(f"cannot write package {output_path}: {exc}") from exc
    return package_info(package, source=str(output_path), manifest=manifest)


def package_info(
    package: bytes,
    *,
    source: str | None = None,
    manifest: Manifest | None = None,
) -> dict[str, Any]:
    if len(package) < MANIFEST_SIZE:
        raise OtaToolError("package is shorter than the 192-byte manifest")
    manifest = manifest or decode_manifest(package[:MANIFEST_SIZE])
    image = package[MANIFEST_SIZE:]
    validate_application_image(image, label="package image")
    if manifest.image_size != len(image):
        raise OtaToolError("package image_size does not match payload length")
    image_hash = _sha256(image)
    if image_hash != manifest.image_sha256:
        raise OtaToolError("package image SHA-256 does not match manifest")
    return {
        "format": PACKAGE_FORMAT,
        "source": source,
        "package_size": len(package),
        "package_sha256": hashlib.sha256(package).hexdigest(),
        "image_size": len(image),
        "image_sha256": image_hash.hex(),
        "manifest": {
            "version": manifest.version,
            "role": role_name(manifest.role),
            "generation": manifest.generation,
            "min_peer_generation": manifest.min_peer_generation,
            "max_peer_generation": manifest.max_peer_generation,
            "partition_schema": manifest.partition_schema,
            "protocol_version": manifest.protocol_version,
            "chip_id": manifest.chip_id,
            "chunk_size": manifest.chunk_size,
            "hardware_revision": manifest.hardware_revision,
            "project_name": manifest.project_name,
            "signer_key_id": manifest.signer_key_id.hex(),
            "signature_algorithm": "ecdsa-p256-sha256",
            "signature_raw_r_s": manifest.signature.hex(),
        },
    }


class Transport(Protocol):
    def write(self, data: bytes) -> int | None: ...

    def read(self, size: int = 1) -> bytes: ...

    def close(self) -> None: ...


class SerialTransport:
    """Explicit-port pyserial transport; no discovery or fallback."""

    def __init__(self, port: str, *, baudrate: int = 4_000_000, timeout: float = 0.05):
        if not port:
            raise OtaToolError("serial port must be explicitly provided")
        if baudrate < BOOT_BAUD:
            raise OtaToolError(f"baudrate must be at least {BOOT_BAUD}")
        self._assert_port_not_owned(port)
        try:
            import serial  # type: ignore
        except ImportError as exc:
            raise OtaToolError(
                "pyserial is required for UART OTA; install with: "
                "python -m pip install pyserial"
            ) from exc
        self._serial = None
        try:
            if baudrate == FAST_BAUD:
                boot = self._open_serial(serial, port, BOOT_BAUD, timeout)
                try:
                    written = int(boot.write(SPEED_SWITCH))
                    if written != len(SPEED_SWITCH):
                        raise OtaToolError("short write during 4M baud switch")
                    boot.flush()
                finally:
                    boot.close()
                time.sleep(0.12)
            self._serial = self._open_serial(serial, port, baudrate, timeout)
            self._serial.reset_input_buffer()
            self._serial.reset_output_buffer()
            written = int(self._serial.write(SERIAL_FRAME_BOUNDARY))
            if written != len(SERIAL_FRAME_BOUNDARY):
                raise OtaToolError("short write while restoring OTA frame boundary")
            self._serial.flush()
            time.sleep(0.02)
            self._serial.reset_input_buffer()
        except Exception as exc:
            if self._serial is not None:
                self._serial.close()
            if isinstance(exc, OtaToolError):
                raise
            raise OtaToolError(f"cannot open explicit OTA port {port}: {exc}") from exc

    @staticmethod
    def _open_serial(serial_module: Any, port: str, baudrate: int,
                     timeout: float):
        kwargs: dict[str, Any] = {
            "port": None,
            "baudrate": baudrate,
            "timeout": timeout,
        }
        if sys.platform.startswith("linux"):
            kwargs["exclusive"] = True
        handle = serial_module.Serial(**kwargs)
        try:
            handle.dtr = False
            handle.rts = False
            handle.port = port
            handle.open()
            return handle
        except Exception:
            handle.close()
            raise

    @staticmethod
    def _assert_port_not_owned(port: str) -> None:
        if not sys.platform.startswith("linux"):
            return
        target = os.path.realpath(port)
        owners: list[str] = []
        for entry in Path("/proc").glob("[0-9]*/fd"):
            if entry.parent.name == str(os.getpid()):
                continue
            try:
                if any(os.path.realpath(fd) == target for fd in entry.iterdir()):
                    cmdline = (entry.parent / "cmdline").read_bytes().replace(
                        b"\x00", b" "
                    ).decode("utf-8", "replace").strip()
                    owners.append(f"pid={entry.parent.name} {cmdline}")
            except (OSError, PermissionError):
                continue
        if owners:
            raise OtaToolError(
                f"explicit OTA port {port} is already owned: " + "; ".join(owners)
            )

    def write(self, data: bytes) -> int:
        if self._serial is None:
            raise OtaToolError("serial transport is closed")
        return int(self._serial.write(data))

    def read(self, size: int = 1) -> bytes:
        if self._serial is None:
            raise OtaToolError("serial transport is closed")
        return bytes(self._serial.read(size))

    def close(self) -> None:
        if self._serial is not None:
            self._serial.close()
            self._serial = None


class OtaNack(OtaToolError):
    def __init__(self, frame: Frame,
                 status: CommandResponsePayload | None = None):
        status = status or decode_command_response_payload(frame.payload)
        reason = status.reason
        self.reason = reason
        self.status = status
        self.frame = frame
        super().__init__(f"OTA NACK reason={reason} ({NACK_REASONS.get(reason, 'unknown')})")


class OtaClient:
    """Reliable in-process request client for one board transaction."""

    def __init__(self, transport: Transport, *, target: int,
                 transaction_id: int | None = None, timeout: float = 1.0,
                 retries: int = 3) -> None:
        if target not in (TARGET_LEFT, TARGET_RIGHT):
            raise OtaToolError("OTA client target must be left or right")
        transaction_id = transaction_id or (secrets.randbelow(0xFFFFFFFF) or 1)
        if not 0 < transaction_id <= 0xFFFFFFFF:
            raise OtaToolError("transaction_id must be non-zero uint32")
        if timeout <= 0 or retries < 0:
            raise OtaToolError("timeout must be positive and retries non-negative")
        self.transport = transport
        self.target = target
        self.transaction_id = transaction_id
        self.timeout = timeout
        self.retries = retries
        self._sequence = 0
        self._decoder = FrameDecoder()

    def _read_response(self, *, sequence: int, command: int,
                       detailed: bool) -> Frame:
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            for frame in self._decoder.feed(self.transport.read(256)):
                if (frame.transaction_id != self.transaction_id or
                        frame.target not in (self.target, TARGET_HOST) or
                        frame.sequence != sequence or
                        not (frame.flags & FLAG_RESPONSE) or
                        frame.flags & ~(FLAG_RESPONSE | FLAG_STATUS_V2) or
                        frame.command not in (CMD_ACK, CMD_NACK,
                                              CMD_STATUS_RESPONSE)):
                    continue
                if detailed != bool(frame.flags & FLAG_STATUS_V2):
                    if frame.command == CMD_NACK and \
                            not (frame.flags & FLAG_STATUS_V2):
                        try:
                            status = decode_command_response_payload(frame.payload)
                            _command_response_semantics(frame.command, status)
                        except OtaToolError:
                            continue
                        if status.last_command == command:
                            raise OtaNack(frame, status)
                    continue
                try:
                    if detailed:
                        observed = decode_status_v2(frame.payload).last_command
                    else:
                        compact = decode_command_response_payload(frame.payload)
                        _command_response_semantics(frame.command, compact)
                        observed = compact.last_command
                except OtaToolError:
                    continue
                if observed == command:
                    return frame
        raise OtaToolError("timed out waiting for OTA response")

    def request(self, command: int, *, payload: bytes = b"", offset: int = 0,
                flags: int = 0, expected: tuple[int, ...] = (CMD_ACK,),
                retries: int | None = None, detailed: bool = False) -> Frame:
        retries = self.retries if retries is None else retries
        if self._sequence >= 0xFFFFFFFE:
            raise OtaToolError("request sequence exhausted")
        self._sequence += 1
        sequence = self._sequence
        for attempt in range(retries + 1):
            wire = encode_frame(
                target=self.target, command=command,
                transaction_id=self.transaction_id, offset=offset,
                sequence=sequence, payload=payload,
                flags=flags | (FLAG_RETRANSMIT if attempt else 0),
            )
            try:
                written = self.transport.write(wire)
                if written is not None and written != len(wire):
                    raise OtaToolError(f"short serial write {written}/{len(wire)} bytes")
                frame = self._read_response(
                    sequence=sequence, command=command, detailed=detailed
                )
            except (OtaToolError, OSError):
                if attempt == retries:
                    raise
                continue
            if detailed:
                next_offset = decode_status_v2(frame.payload).next_offset
            else:
                status = decode_command_response_payload(frame.payload)
                _command_response_semantics(frame.command, status)
                next_offset = status.next_offset
                if frame.command == CMD_NACK:
                    raise OtaNack(frame, status)
            if frame.offset != next_offset:
                raise OtaToolError(
                    "OTA response frame.offset disagrees with status next_offset"
                )
            if frame.command not in expected:
                raise OtaToolError(
                    f"unexpected OTA response command 0x{frame.command:02x}"
                )
            return frame
        raise AssertionError("unreachable")

    def hello(self) -> Frame:
        return self.request(CMD_HELLO)

    def enter_maintenance(self) -> Frame:
        return self.request(CMD_ENTER_MAINTENANCE)

    def status(self) -> StatusPayloadV2:
        frame = self.request(
            CMD_STATUS, flags=FLAG_STATUS_V2,
            expected=(CMD_STATUS_RESPONSE,), detailed=True,
        )
        return decode_status_v2(frame.payload)

    def abort(self) -> Frame:
        return self.request(CMD_ABORT)

    def exit_maintenance(self) -> Frame:
        return self.request(CMD_EXIT_MAINTENANCE)

    def send_manifest(self, manifest: Manifest) -> Frame:
        if manifest.role != self.target:
            raise OtaToolError("package role does not match selected OTA target")
        return self.request(CMD_MANIFEST, payload=manifest.wire)

    def send_data(self, image: bytes, *, manifest: Manifest) -> int:
        if manifest.image_size != len(image) or manifest.image_sha256 != _sha256(image):
            raise OtaToolError("image does not match manifest")
        offset = 0
        while offset < len(image):
            chunk = image[offset : offset + manifest.chunk_size]
            frame = self.request(
                CMD_DATA, payload=chunk, offset=offset,
                flags=FLAG_FINAL if offset + len(chunk) == len(image) else 0,
            )
            accepted = decode_command_response_payload(
                frame.payload).next_offset
            expected = offset + len(chunk)
            if accepted != expected:
                raise OtaToolError(
                    f"receiver accepted next offset {accepted}, expected {expected}"
                )
            offset = accepted
        return offset

    def prepare(self, package: bytes) -> dict[str, Any]:
        manifest = decode_manifest(package[:MANIFEST_SIZE])
        image = package[MANIFEST_SIZE:]
        info = package_info(package, manifest=manifest)
        if self.target == TARGET_LEFT:
            self.hello()
        self.enter_maintenance()
        self.send_manifest(manifest)
        self.send_data(image, manifest=manifest)
        self.request(CMD_PREPARE)
        return info

    def activate(self) -> Frame:
        return self.request(CMD_ACTIVATE)

    def confirm(self) -> Frame:
        return self.request(CMD_CONFIRM)


def _package_bytes(package: bytes, role: int) -> tuple[Manifest, bytes, bytes]:
    if not isinstance(package, bytes) or len(package) <= MANIFEST_SIZE:
        raise OtaToolError("OTA package must contain a manifest and image")
    manifest = decode_manifest(package[:MANIFEST_SIZE])
    package_info(package, manifest=manifest)
    if manifest.role != role:
        raise OtaToolError(f"{role_name(role)} package has the wrong role")
    validate_intrinsic_identity(
        package[MANIFEST_SIZE:], role=role, generation=manifest.generation,
        label=f"{role_name(role)} package image",
    )
    return manifest, manifest.image_sha256, _sha256(manifest.wire)


def _current_and_healthy(status: StatusPayloadV2, manifest: Manifest) -> bool:
    health = STATUS_PEER_COMPATIBLE | STATUS_HEALTH_PASSED | \
        STATUS_LOCAL_BOOT_VALIDATED
    return (status.role == manifest.role and status.state == STATE_IDLE and
            status.running_generation == manifest.generation and
            status.running_image_sha256 == manifest.image_sha256 and
            status.partition_schema == manifest.partition_schema and
            status.hardware_revision == manifest.hardware_revision and
            status.project_name == manifest.project_name and
            (status.status_flags & health) == health)


def _pending_image_exact(status: StatusPayloadV2, manifest: Manifest) -> bool:
    return (status.role == manifest.role and
            status.state == STATE_PENDING_CONFIRM and
            status.running_generation == manifest.generation and
            status.running_image_sha256 == manifest.image_sha256 and
            status.partition_schema == manifest.partition_schema and
            status.hardware_revision == manifest.hardware_revision and
            status.project_name == manifest.project_name)


def _pending_exact(status: StatusPayloadV2, manifest: Manifest) -> bool:
    health = STATUS_PEER_COMPATIBLE | STATUS_HEALTH_PASSED | \
        STATUS_LOCAL_BOOT_VALIDATED
    return (_pending_image_exact(status, manifest) and
            (status.status_flags & health) == health)


def _prepared_exact(status: StatusPayloadV2, manifest: Manifest,
                    transaction_id: int) -> bool:
    return (status.role == manifest.role and status.state == STATE_PREPARED and
            status.transaction_id == transaction_id and
            status.target_generation == manifest.generation and
            status.target_image_sha256 == manifest.image_sha256 and
            status.manifest_sha256 == _sha256(manifest.wire) and
            status.next_offset == manifest.image_size and
            status.partition_schema == manifest.partition_schema and
            status.hardware_revision == manifest.hardware_revision and
            status.project_name == manifest.project_name)


def _wait_status(client: OtaClient, predicate: Any, *, timeout: float,
                 label: str) -> StatusPayloadV2:
    deadline = time.monotonic() + timeout
    last: StatusPayloadV2 | None = None
    while time.monotonic() < deadline:
        try:
            last = client.status()
            if predicate(last):
                return last
        except (OtaToolError, OSError):
            pass
        time.sleep(min(0.1, max(0.0, deadline - time.monotonic())))
    detail = "no status" if last is None else f"state={last.state}"
    raise OtaToolError(f"timed out waiting for {label}: {detail}")


def _confirm_until_healthy(client: OtaClient, manifest: Manifest, *,
                           timeout: float, label: str,
                           initial: StatusPayloadV2 | None = None
                           ) -> StatusPayloadV2:
    """Drive PendingConfirm to healthy Idle using observed board facts."""
    deadline = time.monotonic() + timeout
    last = initial
    last_observed = initial
    while time.monotonic() < deadline:
        if last is None:
            try:
                last = client.status()
                last_observed = last
            except (OtaToolError, OSError):
                last = None
        if last is not None:
            if _current_and_healthy(last, manifest):
                return last
            if _pending_exact(last, manifest):
                try:
                    client.confirm()
                except (OtaToolError, OSError):
                    pass
        last = None
        time.sleep(min(0.1, max(0.0, deadline - time.monotonic())))
    detail = ("no status" if last_observed is None
              else f"state={last_observed.state}")
    raise OtaToolError(f"timed out waiting for {label}: {detail}")


def _preflight_board(transport: Transport, *, target: int,
                     manifest: Manifest, request_timeout: float,
                     retries: int, boot_timeout: float) -> tuple[OtaClient, str,
                                                                  StatusPayloadV2]:
    client = OtaClient(transport, target=target, timeout=request_timeout,
                       retries=retries)
    status = client.status()
    if status.role != target:
        raise OtaToolError("Status V2 role does not match target")
    if status.hardware_revision != manifest.hardware_revision or \
            status.project_name != manifest.project_name or \
            status.partition_schema != manifest.partition_schema:
        raise OtaToolError("board identity does not match signed package")
    if _current_and_healthy(status, manifest):
        if target == TARGET_RIGHT:
            client.exit_maintenance()
        return client, "skipped", status
    if status.state == STATE_PENDING_CONFIRM:
        if not _pending_image_exact(status, manifest):
            raise OtaToolError("PendingConfirm running image is not the package")
        final = _confirm_until_healthy(
            client, manifest, timeout=boot_timeout,
            label="confirmed Idle image", initial=status,
        )
        if target == TARGET_RIGHT:
            client.exit_maintenance()
        return client, "confirmed", final
    if status.state in (STATE_RECEIVING, STATE_PREPARED):
        active = OtaClient(
            transport, target=target, transaction_id=status.transaction_id,
            timeout=request_timeout, retries=retries,
        )
        active.abort()
        client = OtaClient(transport, target=target, timeout=request_timeout,
                           retries=retries)
    elif status.state != STATE_IDLE:
        raise OtaToolError(f"unsupported board state {status.state}")
    return client, "update", status


def _update_right(transport: Transport, package: bytes, manifest: Manifest, *,
                  request_timeout: float, retries: int,
                  boot_timeout: float) -> tuple[str, StatusPayloadV2]:
    client, action, status = _preflight_board(
        transport, target=TARGET_RIGHT, manifest=manifest,
        request_timeout=request_timeout, retries=retries,
        boot_timeout=boot_timeout,
    )
    if action != "update":
        return action, status
    client.prepare(package)
    prepared = client.status()
    if not _prepared_exact(prepared, manifest, client.transaction_id):
        raise OtaToolError("right board did not report exact Prepared state")
    try:
        client.activate()
    except (OtaToolError, OSError):
        # ACTIVATE may have taken effect even when its ACK was lost.  Do not
        # replay a boot-selection mutation blindly; the new image facts below
        # distinguish PendingConfirm/Idle from a still-Prepared old image.
        pass
    final = _confirm_until_healthy(
        client, manifest, timeout=boot_timeout, label="right Idle health",
    )
    client.exit_maintenance()
    return "updated", final


def _update_left(transport: Transport, transport_factory: Any, package: bytes,
                 manifest: Manifest, *, request_timeout: float, retries: int,
                 boot_timeout: float) -> tuple[Transport, str, StatusPayloadV2]:
    client, action, status = _preflight_board(
        transport, target=TARGET_LEFT, manifest=manifest,
        request_timeout=request_timeout, retries=retries,
        boot_timeout=boot_timeout,
    )
    if action != "update":
        return transport, action, status
    client.prepare(package)
    prepared = client.status()
    if not _prepared_exact(prepared, manifest, client.transaction_id):
        raise OtaToolError("left board did not report exact Prepared state")
    try:
        client.activate()
    except (OtaToolError, OSError):
        # USB reconnect plus Status is the only reliable observation after an
        # ACK-loss/reboot race; never infer that ACTIVATE failed from silence.
        pass
    transport.close()
    transport = transport_factory()
    client = OtaClient(transport, target=TARGET_LEFT, timeout=request_timeout,
                       retries=retries)
    final = _confirm_until_healthy(
        client, manifest, timeout=boot_timeout, label="left Idle health",
    )
    return transport, "updated", final


def update_pair(*, transport_factory: Any, right_package: bytes,
                left_package: bytes, request_timeout: float, retries: int,
                boot_timeout: float, progress: Any,
                controller_detached: bool = True) -> dict[str, Any]:
    """Update right then left using only current board facts."""
    if controller_detached is not True:
        raise OtaToolError("controller_detached=True is required before serial I/O")
    if request_timeout <= 0 or retries < 0 or boot_timeout <= 0:
        raise OtaToolError("timeouts must be positive and retries non-negative")
    right, _, _ = _package_bytes(right_package, TARGET_RIGHT)
    left, _, _ = _package_bytes(left_package, TARGET_LEFT)
    if right.generation != left.generation:
        raise OtaToolError("left/right package generations must match")
    for manifest in (right, left):
        peer = left.generation if manifest is right else right.generation
        if not manifest.min_peer_generation <= peer <= manifest.max_peer_generation:
            raise OtaToolError("package rejects target peer generation")
    emit = progress if progress is not None else (lambda _stage: None)
    transport: Transport | None = None
    try:
        emit("preflight")
        transport = transport_factory()
        emit("right")
        right_action, right_status = _update_right(
            transport, right_package, right, request_timeout=request_timeout,
            retries=retries, boot_timeout=boot_timeout,
        )
        emit("left")
        transport, left_action, left_status = _update_left(
            transport, transport_factory, left_package, left,
            request_timeout=request_timeout, retries=retries,
            boot_timeout=boot_timeout,
        )
        emit("complete")
        return {
            "generation": left.generation,
            "right": {"action": right_action,
                      "status": status_v2_json(right_status)},
            "left": {"action": left_action,
                     "status": status_v2_json(left_status)},
        }
    finally:
        if transport is not None:
            transport.close()


def _load_package(path: str | Path) -> tuple[bytes, Manifest]:
    package = _read_path(path, "package")
    if len(package) <= MANIFEST_SIZE:
        raise OtaToolError("package contains no application payload")
    manifest = decode_manifest(package[:MANIFEST_SIZE])
    package_info(package, manifest=manifest)
    return package, manifest


def _open_client(
    port: str,
    target: int,
    args: argparse.Namespace,
    *,
    transaction_id: int | None = None,
) -> OtaClient:
    # Port is deliberately required by argparse and checked again in code.
    if not port:
        raise OtaToolError("an explicit --port is required; auto-discovery is disabled")
    transport = SerialTransport(port, baudrate=args.baud, timeout=min(args.timeout, 0.05))
    return OtaClient(
        transport,
        target=target,
        transaction_id=transaction_id,
        timeout=args.timeout,
        retries=args.retries,
    )


def _close_client(client: OtaClient | None) -> None:
    if client is not None:
        try:
            client.transport.close()
        except Exception:
            pass


def _print(value: Mapping[str, Any] | Any) -> None:
    print(json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False))


# Derive the projection from the dataclass so every fixed board fact is present.
def status_v2_json(status: StatusPayloadV2) -> dict[str, Any]:
    """Project every fixed Status V2 board fact without lossy formatting."""

    payload: dict[str, Any] = {"version": 2}
    for field in dataclass_fields(status):
        value = getattr(status, field.name)
        payload[field.name] = value.hex() if isinstance(value, bytes) else value
    payload["last_command"] = f"0x{status.last_command:02x}"
    payload["last_response_command"] = f"0x{status.last_response_command:02x}"
    payload["reason_name"] = NACK_REASONS.get(status.reason, "unknown")
    return payload


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command")

    package = sub.add_parser("package", help="sign an application .bin into .mkota")
    package.add_argument("--image", required=True)
    package.add_argument("--output", required=True)
    package.add_argument("--private-key", required=True)
    package.add_argument("--signer-key-id", required=True)
    package.add_argument("--role", choices=("left", "right"), required=True)
    package.add_argument("--generation", type=int, required=True)
    package.add_argument("--min-peer-generation", type=int, required=True)
    package.add_argument("--max-peer-generation", type=int, required=True)
    package.add_argument("--partition-schema", type=lambda x: int(x, 0),
                         help="defaults to the role's checked-in v1 schema")
    package.add_argument("--hardware-revision",
                         help="defaults to the checked-in MAKCM R1 revision")
    package.add_argument("--project-name",
                         help="defaults to the role's signed logical project name")
    package.add_argument("--protocol-version", type=int, default=DEFAULT_PROTOCOL_VERSION)
    package.add_argument("--chunk-size", type=int, default=DEFAULT_CHUNK_SIZE)

    inspect = sub.add_parser("inspect", help="inspect a package (no serial I/O)")
    inspect.add_argument("--package", required=True)
    verify = sub.add_parser("verify", help="verify package signature offline")
    verify.add_argument("--package", required=True)
    verify.add_argument("--public-key", required=True)
    verify.add_argument("--signer-key-id")
    anchor = sub.add_parser(
        "anchor", help="generate the public-only ota_trust_anchor.local.inc"
    )
    anchor.add_argument("--public-key", required=True)
    anchor.add_argument("--signer-key-id", required=True)
    anchor.add_argument("--output", required=True)
    anchor.add_argument("--force", action="store_true")

    def serial_parser(name: str) -> argparse.ArgumentParser:
        command = sub.add_parser(name)
        command.add_argument("--port", required=True, help="explicit maintenance UART port")
        command.add_argument("--target", choices=("left", "right"), required=True)
        command.add_argument("--baud", type=int, default=4_000_000)
        command.add_argument("--timeout", type=float, default=DEFAULT_UART_REQUEST_TIMEOUT)
        command.add_argument("--retries", type=int, default=3)
        return command

    serial_parser("status")

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _parser()
    args = parser.parse_args(argv)
    if not args.command:
        # Default operation is inspect/dry-run only; without a package there is
        # nothing to inspect and, importantly, no serial side effect.
        parser.print_help()
        return 0
    try:
        if args.command == "package":
            role_defaults = {
                "left": (LEFT_PARTITION_SCHEMA_V1, LEFT_PROJECT_NAME),
                "right": (RIGHT_PARTITION_SCHEMA_V1,
                          RIGHT_PROJECT_NAME),
            }
            expected_schema, expected_project = role_defaults[args.role]
            partition_schema = (expected_schema if args.partition_schema is None
                                else args.partition_schema)
            hardware_revision = (HARDWARE_REVISION_R1
                                 if args.hardware_revision is None
                                 else args.hardware_revision)
            project_name = (expected_project if args.project_name is None
                            else args.project_name)
            if partition_schema != expected_schema or \
                    hardware_revision != HARDWARE_REVISION_R1 or \
                    project_name != expected_project:
                raise OtaToolError(
                    "package metadata does not match the checked-in role/hardware/schema"
                )
            image_bytes = _read_path(args.image, "application image")
            validate_application_image(image_bytes, label=args.image)
            descriptor = validate_intrinsic_identity(
                image_bytes, role=args.role, generation=args.generation,
                label=args.image,
            )
            _print(
                {
                    **write_package(
                    args.image,
                    args.output,
                    private_key_path=args.private_key,
                    signer_key_id=args.signer_key_id,
                    role=args.role,
                    generation=args.generation,
                    min_peer_generation=args.min_peer_generation,
                    max_peer_generation=args.max_peer_generation,
                    partition_schema=partition_schema,
                    hardware_revision=hardware_revision,
                    project_name=project_name,
                    protocol_version=args.protocol_version,
                    chunk_size=args.chunk_size,
                    ),
                    "intrinsic_descriptor": {
                        "offset": descriptor.offset,
                        "version": descriptor.version,
                        "project_name": descriptor.project_name,
                        "idf_version": descriptor.idf_version,
                        "app_elf_sha256": descriptor.app_elf_sha256.hex(),
                    },
                }
            )
            return 0
        if args.command == "anchor":
            _print(write_trust_anchor(
                args.public_key,
                args.output,
                signer_key_id=args.signer_key_id,
                overwrite=args.force,
            ))
            return 0
        if args.command in ("inspect", "verify"):
            package, manifest = _load_package(args.package)
            info = package_info(package, source=args.package, manifest=manifest)
            if args.command == "verify":
                info["signature_valid"] = verify_manifest_signature(
                    manifest, args.public_key, expected_signer_key_id=args.signer_key_id
                )
                if not info["signature_valid"]:
                    raise OtaToolError("manifest signature verification failed")
            _print(info)
            return 0
        if args.command == "status":
            client = _open_client(args.port, _role_value(args.target), args)
            try:
                status = client.status()
                if args.target == "right":
                    client.exit_maintenance()
                _print({"command": "status", "status": status_v2_json(status)})
            finally:
                _close_client(client)
            return 0
        raise OtaToolError(f"unknown command {args.command}")
    except OtaToolError as exc:
        print(f"makcm_ota_tool: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
