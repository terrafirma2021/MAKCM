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
from typing import Any, BinaryIO, Iterable, Mapping, Protocol
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
TARGET_PAIR = 3

CMD_HELLO = 0x01
CMD_ENTER_MAINTENANCE = 0x02
CMD_MANIFEST = 0x03
CMD_DATA = 0x04
CMD_PREPARE = 0x05
CMD_STATUS = 0x06
CMD_ACTIVATE = 0x07
CMD_CONFIRM = 0x08
CMD_ABORT = 0x09
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
    CMD_ACK,
    CMD_NACK,
    CMD_STATUS_RESPONSE,
}

FLAG_RESPONSE = 0x01
FLAG_RETRANSMIT = 0x02
FLAG_FINAL = 0x04
# Explicit negotiation bit for the fixed 400-byte Status V2 payload.  A V2
# status request/reply must carry this bit; V1 remains available only through
# the explicit diagnostic path below.
FLAG_STATUS_V2 = 0x08
KNOWN_FRAME_FLAGS = FLAG_RESPONSE | FLAG_RETRANSMIT | FLAG_FINAL | FLAG_STATUS_V2

STATUS_V2_VERSION = 2
STATUS_V2_SIZE = 400
STATUS_V2_PROTOCOL_EPOCH = 2

# StatusPayloadV2 wire offsets (mirrors common/OtaStatusV2.h).  Keeping the
# offsets named here makes the Python codec auditable against the C++ golden
# vector without relying on native struct packing.
STATUS_V2_EVIDENCE_ACTIVE = 0x0001
STATUS_V2_EVIDENCE_COMPLETED = 0x0002
STATUS_V2_EVIDENCE_SNAPSHOT_VALID = 0x0004
STATUS_V2_EVIDENCE_SNAPSHOT_COPY_ERROR = 0x0008
STATUS_V2_EVIDENCE_LOCAL_VALIDATED = 0x0010
STATUS_V2_EVIDENCE_PAIR_COMMITTED = 0x0020
STATUS_V2_EVIDENCE_RUNNING_HASH_VALID = 0x0040
STATUS_V2_EVIDENCE_IDENTITY_VALID = 0x0080
STATUS_V2_KNOWN_EVIDENCE = (
    STATUS_V2_EVIDENCE_ACTIVE | STATUS_V2_EVIDENCE_COMPLETED
    | STATUS_V2_EVIDENCE_SNAPSHOT_VALID | STATUS_V2_EVIDENCE_SNAPSHOT_COPY_ERROR
    | STATUS_V2_EVIDENCE_LOCAL_VALIDATED | STATUS_V2_EVIDENCE_PAIR_COMMITTED
    | STATUS_V2_EVIDENCE_RUNNING_HASH_VALID | STATUS_V2_EVIDENCE_IDENTITY_VALID
)
# OtaSnapshotV2 enum values used by the fixed status wire.
STATUS_V2_MAX_PHASE = 11
STATUS_V2_MAX_OUTCOME = 3

# ``StatusFlags::kStatusPeerCompatible``.  A pair activation must not infer
# peer health from a successful UART ACK; the receiver has to assert this bit.
STATUS_PEER_COMPATIBLE = 0x0010
STATUS_HEALTH_PASSED = 0x0040
STATUS_LOCAL_BOOT_VALIDATED = 0x0080
STATUS_PENDING_VERIFY = 0x0002
STATUS_PREPARED = 0x0001
STATUS_ROLLBACK_SUPPORTED = 0x0004
STATUS_TRUST_CONFIGURED = 0x0008
STATUS_MAINTENANCE_EXCLUSIVE = 0x0020
STATUS_V2_KNOWN_STATUS_FLAGS = (
    STATUS_PREPARED | STATUS_PENDING_VERIFY | STATUS_ROLLBACK_SUPPORTED
    | STATUS_TRUST_CONFIGURED | STATUS_PEER_COMPATIBLE
    | STATUS_MAINTENANCE_EXCLUSIVE | STATUS_HEALTH_PASSED
    | STATUS_LOCAL_BOOT_VALIDATED
)

# State values from ``OtaProtocol.h``.  The right receiver reports
# ``STATE_MAINTENANCE`` while its internal updater is logically idle but the
# UART is held in the exclusive MKOT channel.  That is a valid post-confirm
# state as long as the active generation and both health flags also match.
STATE_IDLE = 0
STATE_MAINTENANCE = 1
STATE_RECEIVING = 2
STATE_PREPARED = 3
STATE_ACTIVATION_PENDING = 4
STATE_PENDING_CONFIRM = 5
STATE_CONFIRMED = 6
STATE_FAULT = 7

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
# left in the firmware's legacy line buffer when a repeated invocation sent
# the 115200-baud switch sentinel to a board that was already running at 4 M.
SERIAL_FRAME_BOUNDARY = b"\n"
LEFT_PARTITION_SCHEMA_V1 = 0x4C010001
RIGHT_PARTITION_SCHEMA_V1 = 0x52010001
HARDWARE_REVISION_R1 = "makcm-s3-r1"
LEFT_PROJECT_NAME = "makcm_left_idf"
RIGHT_MANIFEST_PROJECT_NAME = "makcm_right_arduino"


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
LEFT_DESCRIPTOR_PROJECT = "makcm_left_idf"
LEFT_DESCRIPTOR_VERSION = "makcm-g{}"
RIGHT_DESCRIPTOR_PROJECT = "arduino-lib-builder"
RIGHT_DESCRIPTOR_IDF_PREFIX = "v4.4.7"


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
    data: bytes, *, role: str, generation: int, label: str = "image"
) -> AppDescriptor:
    """Require role-specific descriptor identity before package signing.

    The right Arduino image currently has a fixed upstream descriptor and no
    generation in that descriptor.  It must therefore carry an explicit
    ``makcm-g<generation>`` marker elsewhere in the signed app bytes; absent
    that marker package creation reports a hard production block.
    """

    descriptor = parse_app_descriptor(data, label=label)
    identities = parse_embedded_identities(data, label=label)
    if len(identities) != 1:
        raise OtaToolError(
            f"{label} has {len(identities)} legal embedded identity markers; "
            "expected exactly one"
        )
    identity = identities[0]
    expected_schema = (
        LEFT_PARTITION_SCHEMA_V1 if role == "left" else RIGHT_PARTITION_SCHEMA_V1
    )
    expected_project = (
        LEFT_PROJECT_NAME if role == "left" else RIGHT_MANIFEST_PROJECT_NAME
    )
    expected_version = LEFT_DESCRIPTOR_VERSION.format(generation)
    if (
        identity.role != (TARGET_LEFT if role == "left" else TARGET_RIGHT)
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
    if role == "left":
        expected_version = LEFT_DESCRIPTOR_VERSION.format(generation)
        if descriptor.project_name != LEFT_DESCRIPTOR_PROJECT:
            raise OtaToolError(
                f"{label} intrinsic project {descriptor.project_name!r} != "
                f"{LEFT_DESCRIPTOR_PROJECT!r}"
            )
        if descriptor.version != expected_version:
            raise OtaToolError(
                f"{label} intrinsic version {descriptor.version!r} does not "
                f"prove generation {generation} (expected {expected_version!r})"
            )
    elif role == "right":
        if descriptor.project_name != RIGHT_DESCRIPTOR_PROJECT:
            raise OtaToolError(
                f"{label} intrinsic project {descriptor.project_name!r} != "
                f"{RIGHT_DESCRIPTOR_PROJECT!r}"
            )
        if not descriptor.idf_version.startswith(RIGHT_DESCRIPTOR_IDF_PREFIX):
            raise OtaToolError(
                f"{label} intrinsic IDF {descriptor.idf_version!r} is not the "
                f"supported {RIGHT_DESCRIPTOR_IDF_PREFIX} line"
            )
    else:
        raise OtaToolError("role must be left or right")
    return descriptor


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


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

    if target not in (TARGET_HOST, TARGET_LEFT, TARGET_RIGHT, TARGET_PAIR):
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
    if flags & ~KNOWN_FRAME_FLAGS:
        raise OtaToolError("frame contains unknown flag bits")
    if flags & FLAG_STATUS_V2 and command not in (CMD_STATUS, CMD_STATUS_RESPONSE):
        raise OtaToolError("Status V2 flag is valid only on STATUS frames")
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
class StatusPayload:
    """The fixed 24-byte ACK/NAK/status payload from OtaProtocol.h."""

    state: int
    last_command: int
    reason: int
    next_offset: int
    target_generation: int
    active_generation: int
    status_flags: int


@dataclass(frozen=True)
class StatusPayloadV2:
    """The fixed 400-byte Status V2 payload from ``OtaStatusV2.h``.

    All integer fields are decoded explicitly as little-endian values.  The
    byte arrays are retained as immutable ``bytes`` so callers can compare an
    observed board fact to the staged manifest without lossy formatting.
    """

    role: int
    local_phase: int
    state: int
    last_command: int
    last_response_command: int
    last_ota_operation: int
    completed_outcome: int
    reason: int
    protocol_epoch: int
    status_flags: int
    next_offset: int
    record_transaction_id: int
    completed_transaction_id: int
    target_generation: int
    active_generation: int
    previous_generation: int
    partition_schema: int
    image_size: int
    record_sequence: int
    last_request_sequence: int
    completed_terminal_sequence: int
    reset_reason: int
    last_ota_error: int
    last_state_query_error: int
    boot_attempt: int
    evidence_flags: int
    running_slot: int
    boot_slot: int
    target_slot: int
    previous_slot: int
    image_state: int
    completed_terminal_command: int
    record_crc32: int
    image_sha256: bytes
    completed_image_sha256: bytes
    running_image_sha256: bytes
    manifest_sha256: bytes
    identity_digest: bytes
    previous_image_sha256: bytes
    app_elf_sha256: bytes
    hardware_revision: str
    project_name: str
    app_version: str

    @property
    def active_present(self) -> bool:
        return bool(self.evidence_flags & STATUS_V2_EVIDENCE_ACTIVE)

    @property
    def completed_present(self) -> bool:
        return bool(self.evidence_flags & STATUS_V2_EVIDENCE_COMPLETED)

    @property
    def local_validated(self) -> bool:
        return bool(self.evidence_flags & STATUS_V2_EVIDENCE_LOCAL_VALIDATED)

    @property
    def pair_committed(self) -> bool:
        return bool(self.evidence_flags & STATUS_V2_EVIDENCE_PAIR_COMMITTED)

    @property
    def identity_valid(self) -> bool:
        return bool(self.evidence_flags & STATUS_V2_EVIDENCE_IDENTITY_VALID)


def _status_v2_text(wire: bytes, offset: int, capacity: int, label: str) -> str:
    raw = wire[offset : offset + capacity]
    end = raw.find(b"\x00")
    if end <= 0 or any(raw[end + 1 :]):
        raise OtaToolError(f"Status V2 {label} field is not canonical text")
    try:
        value = raw[:end].decode("utf-8")
    except UnicodeDecodeError as exc:
        raise OtaToolError(f"Status V2 {label} is not UTF-8") from exc
    return value


def _status_v2_u16(wire: bytes, offset: int) -> int:
    return struct.unpack_from("<H", wire, offset)[0]


def _status_v2_u32(wire: bytes, offset: int) -> int:
    return struct.unpack_from("<I", wire, offset)[0]


def decode_status_v2(payload: bytes) -> StatusPayloadV2:
    """Decode and validate the exact Status V2 wire value.

    The decoder deliberately rejects unknown/reserved bytes and all semantic
    combinations that ``validStatusV2`` rejects in the shared C++ codec.  A
    malformed V2 response is never downgraded to a V1 status by automatic
    update paths.
    """

    if len(payload) != STATUS_V2_SIZE:
        raise OtaToolError(
            f"OTA Status V2 payload must be exactly {STATUS_V2_SIZE} bytes"
        )
    if payload[0] != STATUS_V2_VERSION or payload[82] != 0 or payload[83] != 0:
        raise OtaToolError("invalid OTA Status V2 header/reserved bytes")
    if any(payload[88:96]):
        raise OtaToolError("invalid OTA Status V2 reserved bytes")
    role = payload[1]
    local_phase = payload[2]
    state = payload[3]
    last_command = payload[4]
    last_response_command = payload[5]
    completed_outcome = payload[7]
    reason = _status_v2_u16(payload, 8)
    protocol_epoch = _status_v2_u16(payload, 10)
    status_flags = _status_v2_u32(payload, 12)
    next_offset = _status_v2_u32(payload, 16)
    record_transaction_id = _status_v2_u32(payload, 20)
    completed_transaction_id = _status_v2_u32(payload, 24)
    target_generation = _status_v2_u32(payload, 28)
    active_generation = _status_v2_u32(payload, 32)
    previous_generation = _status_v2_u32(payload, 36)
    partition_schema = _status_v2_u32(payload, 40)
    image_size = _status_v2_u32(payload, 44)
    record_sequence = _status_v2_u32(payload, 48)
    last_request_sequence = _status_v2_u32(payload, 52)
    completed_terminal_sequence = _status_v2_u32(payload, 56)
    reset_reason = _status_v2_u32(payload, 60)
    last_ota_error = struct.unpack_from("<i", payload, 64)[0]
    last_state_query_error = struct.unpack_from("<i", payload, 68)[0]
    boot_attempt = _status_v2_u16(payload, 72)
    evidence_flags = _status_v2_u16(payload, 74)
    running_slot, boot_slot, target_slot, previous_slot = payload[76:80]
    image_state = payload[80]
    completed_terminal_command = payload[81]
    record_crc32 = _status_v2_u32(payload, 84)
    image_sha256 = bytes(payload[96:128])
    completed_image_sha256 = bytes(payload[128:160])
    running_image_sha256 = bytes(payload[160:192])
    manifest_sha256 = bytes(payload[192:224])
    identity_digest = bytes(payload[224:256])
    previous_image_sha256 = bytes(payload[256:288])
    app_elf_sha256 = bytes(payload[288:320])
    hardware_revision = _status_v2_text(payload, 320, 16, "hardware_revision")
    project_name = _status_v2_text(payload, 336, 32, "project_name")
    app_version = _status_v2_text(payload, 368, 32, "app_version")

    if role not in (TARGET_LEFT, TARGET_RIGHT):
        raise OtaToolError("invalid Status V2 board role")
    if local_phase > STATUS_V2_MAX_PHASE or state > STATE_FAULT:
        raise OtaToolError("invalid Status V2 phase/state")
    if last_command not in VALID_COMMANDS:
        raise OtaToolError("invalid Status V2 last command")
    if last_response_command not in (CMD_ACK, CMD_NACK, CMD_STATUS_RESPONSE):
        raise OtaToolError("invalid Status V2 last response command")
    if completed_outcome > STATUS_V2_MAX_OUTCOME or reason > 16:
        raise OtaToolError("invalid Status V2 outcome/reason")
    if protocol_epoch != STATUS_V2_PROTOCOL_EPOCH or record_sequence in (0, 0xFFFFFFFF):
        raise OtaToolError("invalid Status V2 protocol/record sequence")
    # Mirrors common/OtaStatusV2.h: a board that can answer is running an
    # image, so it always knows its own generation.
    if active_generation == 0:
        raise OtaToolError("Status V2 active generation is missing")
    if status_flags & ~STATUS_V2_KNOWN_STATUS_FLAGS:
        raise OtaToolError("unknown Status V2 status flag bits")
    if evidence_flags & ~STATUS_V2_KNOWN_EVIDENCE:
        raise OtaToolError("unknown Status V2 evidence flag bits")
    if not (evidence_flags & STATUS_V2_EVIDENCE_SNAPSHOT_VALID):
        raise OtaToolError("Status V2 snapshot-valid evidence is missing")
    if not any(app_elf_sha256):
        raise OtaToolError("Status V2 app ELF identity hash is missing")

    active = bool(evidence_flags & STATUS_V2_EVIDENCE_ACTIVE)
    completed = bool(evidence_flags & STATUS_V2_EVIDENCE_COMPLETED)
    if active and (
        record_transaction_id == 0
        or local_phase in (0, 8)
        or target_generation == 0
        or previous_generation == 0
        or partition_schema == 0
        or image_size == 0
        or last_request_sequence in (0, 0xFFFFFFFF)
        or target_slot == 0xFF
        or previous_slot == 0xFF
        or target_slot == previous_slot
        or not any(image_sha256)
        or not any(manifest_sha256)
        or not any(identity_digest)
        or not any(previous_image_sha256)
    ):
        raise OtaToolError("Status V2 active evidence is incomplete")
    if completed and (
        completed_transaction_id == 0
        or completed_terminal_sequence == 0
        or completed_terminal_sequence == 0xFFFFFFFF
        or completed_outcome == 0
        or completed_terminal_command not in (CMD_CONFIRM, CMD_ABORT)
        or ((completed_outcome == 1) != (completed_terminal_command == CMD_CONFIRM))
        or not any(completed_image_sha256)
    ):
        raise OtaToolError("Status V2 completed evidence is incomplete")
    if not active and (
        record_transaction_id != 0 or last_request_sequence != 0 or local_phase != 0
    ):
        raise OtaToolError("Status V2 has active fields without active evidence")
    if not completed and (
        completed_transaction_id != 0
        or completed_terminal_sequence != 0
        or completed_outcome != 0
        or completed_terminal_command != CMD_HELLO
        or any(completed_image_sha256)
    ):
        raise OtaToolError("Status V2 has completed fields without tombstone evidence")
    if evidence_flags & STATUS_V2_EVIDENCE_RUNNING_HASH_VALID and not any(running_image_sha256):
        raise OtaToolError("Status V2 running hash evidence is empty")
    if evidence_flags & STATUS_V2_EVIDENCE_IDENTITY_VALID and not any(identity_digest):
        raise OtaToolError("Status V2 identity evidence is empty")

    return StatusPayloadV2(
        role=role, local_phase=local_phase, state=state,
        last_command=last_command, last_response_command=last_response_command,
        last_ota_operation=payload[6], completed_outcome=completed_outcome,
        reason=reason, protocol_epoch=protocol_epoch, status_flags=status_flags,
        next_offset=next_offset, record_transaction_id=record_transaction_id,
        completed_transaction_id=completed_transaction_id,
        target_generation=target_generation, active_generation=active_generation,
        previous_generation=previous_generation, partition_schema=partition_schema,
        image_size=image_size, record_sequence=record_sequence,
        last_request_sequence=last_request_sequence,
        completed_terminal_sequence=completed_terminal_sequence,
        reset_reason=reset_reason, last_ota_error=last_ota_error,
        last_state_query_error=last_state_query_error, boot_attempt=boot_attempt,
        evidence_flags=evidence_flags, running_slot=running_slot, boot_slot=boot_slot,
        target_slot=target_slot, previous_slot=previous_slot, image_state=image_state,
        completed_terminal_command=completed_terminal_command,
        record_crc32=record_crc32, image_sha256=image_sha256,
        completed_image_sha256=completed_image_sha256,
        running_image_sha256=running_image_sha256,
        manifest_sha256=manifest_sha256, identity_digest=identity_digest,
        previous_image_sha256=previous_image_sha256,
        app_elf_sha256=app_elf_sha256, hardware_revision=hardware_revision,
        project_name=project_name, app_version=app_version,
    )


def encode_status_v2(status: StatusPayloadV2) -> bytes:
    """Encode a validated Status V2 value for golden vectors/mocks."""

    wire = bytearray(STATUS_V2_SIZE)
    wire[0] = STATUS_V2_VERSION
    wire[1] = status.role
    wire[2] = status.local_phase
    wire[3] = status.state
    wire[4] = status.last_command
    wire[5] = status.last_response_command
    wire[6] = status.last_ota_operation
    wire[7] = status.completed_outcome
    struct.pack_into("<H", wire, 8, status.reason)
    struct.pack_into("<H", wire, 10, status.protocol_epoch)
    for offset, value in (
        (12, status.status_flags), (16, status.next_offset),
        (20, status.record_transaction_id), (24, status.completed_transaction_id),
        (28, status.target_generation), (32, status.active_generation),
        (36, status.previous_generation), (40, status.partition_schema),
        (44, status.image_size), (48, status.record_sequence),
        (52, status.last_request_sequence), (56, status.completed_terminal_sequence),
        (60, status.reset_reason), (84, status.record_crc32),
    ):
        struct.pack_into("<I", wire, offset, value & 0xFFFFFFFF)
    struct.pack_into("<i", wire, 64, status.last_ota_error)
    struct.pack_into("<i", wire, 68, status.last_state_query_error)
    struct.pack_into("<H", wire, 72, status.boot_attempt)
    struct.pack_into("<H", wire, 74, status.evidence_flags)
    wire[76:81] = bytes((status.running_slot, status.boot_slot,
                          status.target_slot, status.previous_slot,
                          status.image_state))
    wire[81] = status.completed_terminal_command
    for offset, value in (
        (96, status.image_sha256), (128, status.completed_image_sha256),
        (160, status.running_image_sha256), (192, status.manifest_sha256),
        (224, status.identity_digest), (256, status.previous_image_sha256),
        (288, status.app_elf_sha256),
    ):
        if len(value) != 32:
            raise OtaToolError("Status V2 digest must be exactly 32 bytes")
        wire[offset : offset + 32] = value
    for offset, value, capacity, label in (
        (320, status.hardware_revision, 16, "hardware_revision"),
        (336, status.project_name, 32, "project_name"),
        (368, status.app_version, 32, "app_version"),
    ):
        encoded = value.encode("utf-8")
        if not value or len(encoded) >= capacity:
            raise OtaToolError(f"Status V2 {label} does not fit fixed field")
        wire[offset : offset + len(encoded)] = encoded
    # Decode performs the same semantic/reserved-byte validation as C++.
    decode_status_v2(bytes(wire))
    return bytes(wire)


def decode_status_payload(payload: bytes) -> StatusPayload:
    if len(payload) != 24:
        raise OtaToolError("OTA status payload must be exactly 24 bytes")
    version, state, last_command, reserved = struct.unpack_from("<BBBB", payload, 0)
    reason, reserved2 = struct.unpack_from("<HH", payload, 4)
    if version != WIRE_VERSION or reserved != 0 or reserved2 != 0:
        raise OtaToolError("invalid OTA status payload header")
    if state > 7 or last_command not in VALID_COMMANDS:
        raise OtaToolError("invalid OTA status state/command")
    next_offset, target_generation, active_generation, status_flags = struct.unpack_from(
        "<IIII", payload, 8
    )
    return StatusPayload(
        state=state,
        last_command=last_command,
        reason=reason,
        next_offset=next_offset,
        target_generation=target_generation,
        active_generation=active_generation,
        status_flags=status_flags,
    )


def status_v2_matches_active(
    status: StatusPayloadV2, *, role: int, transaction_id: int,
    generation: int, image_sha256: bytes | None = None,
    manifest_sha256: bytes | None = None,
) -> bool:
    """Exact active-record predicate used by all automatic pair decisions."""

    if status.role != role or not status.active_present or not status.identity_valid:
        return False
    if (
        status.record_transaction_id != transaction_id
        or status.target_generation != generation
    ):
        return False
    if image_sha256 is not None and status.image_sha256 != image_sha256:
        return False
    if manifest_sha256 is not None and status.manifest_sha256 != manifest_sha256:
        return False
    return True


def status_v2_matches_completed(
    status: StatusPayloadV2, *, role: int, transaction_id: int, generation: int,
    image_sha256: bytes | None = None,
    manifest_sha256: bytes | None = None,
) -> bool:
    """Exact completed-tombstone predicate; generation alone is insufficient."""

    if status.role != role or not status.completed_present:
        return False
    if status.completed_transaction_id != transaction_id:
        return False
    if status.target_generation != generation:
        return False
    if status.completed_outcome != 1 or status.completed_terminal_command != CMD_CONFIRM:
        return False
    if image_sha256 is not None and status.completed_image_sha256 != image_sha256:
        return False
    if manifest_sha256 is not None and status.manifest_sha256 != manifest_sha256:
        return False
    if not (status.evidence_flags & STATUS_V2_EVIDENCE_IDENTITY_VALID):
        return False
    # A completed identity carries the target generation in the active fields
    # when retained, or in the board's current active generation after cleanup.
    return status.active_generation == generation


def activation_disposition(
    status: StatusPayloadV2, *, role: int, transaction_id: int, generation: int,
    image_sha256: bytes, manifest_sha256: bytes,
) -> str:
    """Classify an exact pair member before ACTIVATE.

    ``status_v2_matches_active`` proves identity, not activation.  A Prepared
    record is still active in the snapshot sense and must not be mistaken for
    an already activated image.
    """

    if status_v2_matches_completed(
        status,
        role=role,
        transaction_id=transaction_id,
        generation=generation,
        image_sha256=image_sha256,
        manifest_sha256=manifest_sha256,
    ):
        return "completed"
    if not status_v2_matches_active(
        status,
        role=role,
        transaction_id=transaction_id,
        generation=generation,
        image_sha256=image_sha256,
        manifest_sha256=manifest_sha256,
    ):
        raise OtaToolError("board lacks exact Status V2 activation identity")
    if status.state == STATE_PREPARED:
        return "activate"
    if status.state in (
        STATE_ACTIVATION_PENDING, STATE_PENDING_CONFIRM, STATE_CONFIRMED,
    ):
        return "already_activated"
    raise OtaToolError(
        f"exact board identity is in non-activatable state {status.state}"
    )


def _status_v2_summary(status: StatusPayloadV2) -> dict[str, Any]:
    """JSON-safe complete fact included in pair command results/journals."""

    return {
        "version": STATUS_V2_VERSION,
        "role": status.role,
        "local_phase": status.local_phase,
        "state": status.state,
        "last_command": status.last_command,
        "last_response_command": status.last_response_command,
        "last_ota_operation": status.last_ota_operation,
        "completed_outcome": status.completed_outcome,
        "reason": status.reason,
        "protocol_epoch": status.protocol_epoch,
        "record_transaction_id": status.record_transaction_id,
        "completed_transaction_id": status.completed_transaction_id,
        "target_generation": status.target_generation,
        "active_generation": status.active_generation,
        "previous_generation": status.previous_generation,
        "partition_schema": status.partition_schema,
        "image_size": status.image_size,
        "target_slot": status.target_slot,
        "previous_slot": status.previous_slot,
        "running_slot": status.running_slot,
        "boot_slot": status.boot_slot,
        "image_state": status.image_state,
        "record_sequence": status.record_sequence,
        "last_request_sequence": status.last_request_sequence,
        "completed_terminal_sequence": status.completed_terminal_sequence,
        "completed_terminal_command": status.completed_terminal_command,
        "reset_reason": status.reset_reason,
        "last_ota_error": status.last_ota_error,
        "last_state_query_error": status.last_state_query_error,
        "boot_attempt": status.boot_attempt,
        "evidence_flags": status.evidence_flags,
        "status_flags": status.status_flags,
        "record_crc32": status.record_crc32,
        "image_sha256": status.image_sha256.hex(),
        "completed_image_sha256": status.completed_image_sha256.hex(),
        "running_image_sha256": status.running_image_sha256.hex(),
        "manifest_sha256": status.manifest_sha256.hex(),
        "identity_digest": status.identity_digest.hex(),
        "previous_image_sha256": status.previous_image_sha256.hex(),
        "app_elf_sha256": status.app_elf_sha256.hex(),
        "hardware_revision": status.hardware_revision,
        "project_name": status.project_name,
        "app_version": status.app_version,
    }


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
    if target not in (TARGET_HOST, TARGET_LEFT, TARGET_RIGHT, TARGET_PAIR):
        raise OtaToolError("invalid frame target")
    if command not in VALID_COMMANDS:
        raise OtaToolError("invalid frame command")
    if transaction_id == 0 or sequence in (0, 0xFFFFFFFF) or reserved != 0:
        raise OtaToolError("invalid frame transaction/reserved fields")
    if flags & ~KNOWN_FRAME_FLAGS:
        raise OtaToolError("frame contains unknown flag bits")
    if flags & FLAG_STATUS_V2 and command not in (CMD_STATUS, CMD_STATUS_RESPONSE):
        raise OtaToolError("Status V2 flag is valid only on STATUS frames")
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
    def __init__(self, frame: Frame, status: StatusPayload | None = None):
        status = status or decode_status_payload(frame.payload)
        reason = status.reason
        self.reason = reason
        self.status = status
        self.frame = frame
        super().__init__(f"OTA NACK reason={reason} ({NACK_REASONS.get(reason, 'unknown')})")


class OtaClient:
    """Reliable request/response client over an exclusive maintenance UART."""

    def __init__(
        self,
        transport: Transport,
        *,
        target: int,
        transaction_id: int | None = None,
        timeout: float = 1.0,
        retries: int = 3,
    ) -> None:
        if target not in (TARGET_LEFT, TARGET_RIGHT):
            raise OtaToolError("OTA client target must be left or right")
        if transaction_id is None:
            transaction_id = secrets.randbelow(0xFFFFFFFF) or 1
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
        # Set by prepare() after the signed manifest is decoded.  These are
        # required to prove that a read-only STATUS belongs to this exact
        # transaction before any best-effort pre-activation Abort is sent.
        self._expected_image_sha256: bytes | None = None
        self._expected_manifest_sha256: bytes | None = None

    def _read_response(
        self, *, expected_sequence: int, expected_command: int,
        require_status_v2: bool = False,
    ) -> Frame:
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            chunk = self.transport.read(256)
            if not chunk:
                continue
            for frame in self._decoder.feed(chunk):
                if frame.transaction_id != self.transaction_id:
                    continue
                if frame.target not in (self.target, TARGET_HOST):
                    continue
                if frame.sequence != expected_sequence:
                    continue
                if not (frame.flags & FLAG_RESPONSE):
                    continue
                if frame.flags & ~(FLAG_RESPONSE | FLAG_STATUS_V2):
                    continue
                if frame.command not in (CMD_ACK, CMD_NACK, CMD_STATUS_RESPONSE):
                    continue
                if require_status_v2 and not (frame.flags & FLAG_STATUS_V2):
                    # A correlated V1 NACK here is the bridge refusing this
                    # exact request (Busy / NotInMaintenance / ...).  Skipping
                    # it turns every refusal into a blind timeout.
                    if frame.command == CMD_NACK:
                        try:
                            nack_status = decode_status_payload(frame.payload)
                        except OtaToolError:
                            continue
                        if nack_status.last_command != expected_command:
                            continue
                        raise OtaNack(frame, nack_status)
                    continue
                if not require_status_v2 and (frame.flags & FLAG_STATUS_V2):
                    continue
                try:
                    if require_status_v2:
                        status_v2 = decode_status_v2(frame.payload)
                        if status_v2.last_response_command != frame.command:
                            continue
                        last_command = status_v2.last_command
                    else:
                        status = decode_status_payload(frame.payload)
                        last_command = status.last_command
                except OtaToolError:
                    continue
                if last_command != expected_command:
                    continue
                return frame
        raise OtaToolError("timed out waiting for OTA response")

    def request(
        self,
        command: int,
        *,
        payload: bytes = b"",
        offset: int = 0,
        flags: int = 0,
        expected: tuple[int, ...] = (CMD_ACK, CMD_STATUS_RESPONSE),
        retries: int | None = None,
        require_status_v2: bool = False,
    ) -> Frame:
        if retries is None:
            retries = self.retries
        if self._sequence >= 0xFFFFFFFE:
            raise OtaToolError(
                "request sequence exhausted before reserved boundary; start a new transaction"
            )
        self._sequence += 1
        sequence = self._sequence
        for attempt in range(retries + 1):
            attempt_flags = flags | (FLAG_RETRANSMIT if attempt else 0)
            wire = encode_frame(
                target=self.target,
                command=command,
                transaction_id=self.transaction_id,
                offset=offset,
                sequence=sequence,
                payload=payload,
                flags=attempt_flags,
            )
            try:
                written = self.transport.write(wire)
                if written is not None and written != len(wire):
                    raise OtaToolError(
                        f"short serial write {written}/{len(wire)} bytes"
                    )
                frame = self._read_response(
                    expected_sequence=sequence,
                    expected_command=command,
                    require_status_v2=require_status_v2,
                )
            except (OtaToolError, OSError):
                if attempt >= retries:
                    raise
                continue
            if frame.command not in (CMD_ACK, CMD_NACK, CMD_STATUS_RESPONSE):
                raise OtaToolError("unsupported OTA response command")
            # Status V2 is required for automatic STATUS/reconciliation.  ACK
            # responses to state-changing commands remain the compact V1
            # response; callers must issue an exact V2 STATUS before acting on
            # the board fact.
            status_next_offset = (
                decode_status_v2(frame.payload).next_offset
                if require_status_v2 else decode_status_payload(frame.payload).next_offset
            )
            if frame.offset != status_next_offset:
                raise OtaToolError(
                    "OTA response frame.offset disagrees with "
                    "status next_offset"
                )
            if frame.command == CMD_NACK:
                if require_status_v2:
                    raise OtaToolError("Status V2 NACK is not valid for automatic status")
                raise OtaNack(frame, decode_status_payload(frame.payload))
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

    def send_manifest(self, manifest: Manifest) -> Frame:
        if manifest.role != self.target:
            raise OtaToolError("package role does not match selected OTA target")
        return self.request(CMD_MANIFEST, payload=manifest.wire)

    def send_data(self, image: bytes, *, manifest: Manifest) -> int:
        if manifest.image_size != len(image) or manifest.image_sha256 != _sha256(image):
            raise OtaToolError("image does not match manifest")
        chunk_size = manifest.chunk_size
        offset = 0
        resume_recoveries = 0
        max_resume_recoveries = max(1, self.retries + 1)
        while offset < len(image):
            chunk = image[offset : offset + chunk_size]
            final = offset + len(chunk) == len(image)
            try:
                frame = self.request(
                    CMD_DATA,
                    payload=chunk,
                    offset=offset,
                    flags=FLAG_FINAL if final else 0,
                )
            except OtaNack as exc:
                # A reconnect/resume or an idempotent duplicate can report the
                # receiver's next offset.  Never guess from local state: only
                # accept a bounded, forward-compatible offset from the frame.
                if exc.reason != 9:  # NackReason::WrongOffset
                    raise
                try:
                    # A WrongOffset response is intentionally not trusted for
                    # its echoed request offset.  Query STATUS V2 for the
                    # durable next offset; if that evidence is unavailable,
                    # stop rather than guessing from a response envelope.
                    status_frame = self.status()
                    status_v2 = decode_status_v2(status_frame.payload)
                    if not status_v2_matches_active(
                        status_v2,
                        role=self.target,
                        transaction_id=self.transaction_id,
                        generation=manifest.generation,
                        image_sha256=manifest.image_sha256,
                        manifest_sha256=_sha256(manifest.wire),
                    ):
                        raise OtaToolError(
                            "receiver Status V2 does not identify this exact image"
                        )
                    resume = status_v2.next_offset
                except OtaToolError as status_error:
                    raise OtaToolError(
                        "cannot resume DATA without durable Status V2 next_offset"
                    ) from status_error
                if not 0 <= resume <= len(image):
                    raise OtaToolError("receiver returned an invalid resume offset")
                if resume != len(image) and resume % chunk_size != 0:
                    raise OtaToolError(
                        "receiver returned an unaligned resume offset"
                    )
                if resume <= offset:
                    raise OtaToolError(
                        "receiver made no forward DATA resume progress"
                    )
                resume_recoveries += 1
                if resume_recoveries > max_resume_recoveries:
                    raise OtaToolError(
                        "receiver exceeded the bounded DATA resume budget"
                    )
                offset = resume
                continue
            # Receiver returns the next expected offset in the frame offset.
            accepted = decode_status_payload(frame.payload).next_offset
            expected = offset + len(chunk)
            if accepted != expected:
                raise OtaToolError(
                    f"receiver accepted next offset {accepted}, expected {expected}"
                )
            offset = accepted
        return offset

    def prepare(
        self,
        package: bytes,
        *,
        allow_ota: bool = False,
        expected_package_sha256: str | None = None,
    ) -> dict[str, Any]:
        if not allow_ota:
            raise OtaToolError("prepare requires explicit --allow-ota authorization")
        manifest = decode_manifest(package[:MANIFEST_SIZE])
        image = package[MANIFEST_SIZE:]
        info = package_info(package, manifest=manifest)
        self._expected_image_sha256 = manifest.image_sha256
        self._expected_manifest_sha256 = _sha256(manifest.wire)
        if expected_package_sha256 is not None:
            expected = expected_package_sha256.lower()
            if len(expected) != 64 or any(c not in "0123456789abcdef" for c in expected):
                raise OtaToolError("expected package SHA-256 must be 64 hexadecimal characters")
            if info["package_sha256"] != expected:
                raise OtaToolError("package SHA-256 does not match explicit expected hash")
        try:
            # A right-board HELLO is not a harmless discovery request: the
            # left proxy must first bootstrap the right board's exclusive
            # UART1 maintenance channel.  Sending HELLO and then ENTER caused
            # two bootstrap-sensitive round trips during the one-time legacy
            # migration; if the first OTA_READY text response was lost, the
            # old receiver remained in maintenance and rejected ENTER as
            # Busy.  ENTER already returns the common status payload, so make
            # it the first and only bootstrap request for the right target.
            # Keep the left HELLO for its local preflight, where it does not
            # cross the inter-board maintenance boundary.
            if self.target == TARGET_LEFT:
                self.hello()
            self.enter_maintenance()
            self.send_manifest(manifest)
            self.send_data(image, manifest=manifest)
            self.request(CMD_PREPARE)
            return info
        except Exception as exc:
            self.safe_abort_pre_activation()
            raise exc

    def status(self, *, v2: bool = True) -> Frame:
        """Read board facts; V2 is mandatory unless explicitly diagnostic."""
        if v2:
            frame = self.request(
                CMD_STATUS,
                flags=FLAG_STATUS_V2,
                expected=(CMD_STATUS_RESPONSE,),
                require_status_v2=True,
            )
            status = decode_status_v2(frame.payload)
            # A new CLI process starts its volatile counter at zero, while the
            # board retains the last durable state-changing sequence.  Adopt
            # that watermark after the exact Status V2 response so ACTIVATE,
            # CONFIRM or ABORT cannot be rejected as a stale sequence.
            durable_sequence = 0
            if (
                status.active_present
                and status.record_transaction_id == self.transaction_id
            ):
                durable_sequence = status.last_request_sequence
            elif (
                status.completed_present
                and status.completed_transaction_id == self.transaction_id
            ):
                durable_sequence = status.completed_terminal_sequence
            if durable_sequence in (0xFFFFFFFF,):
                raise OtaToolError("board reports a reserved durable sequence")
            self._sequence = max(self._sequence, durable_sequence)
            return frame
        return self.request(CMD_STATUS, expected=(CMD_STATUS_RESPONSE, CMD_ACK))

    def safe_status(self) -> Frame | None:
        try:
            return self.status()
        except Exception:
            return None

    def safe_abort_pre_activation(self) -> Frame | None:
        """Abort only when an exact status proves boot selection is untouched.

        A lost PREPARE/ACTIVATE response is ambiguous.  Never convert a
        timeout into an unconditional rollback or evidence erase.
        """
        if self._expected_image_sha256 is None or self._expected_manifest_sha256 is None:
            # No signed package identity has been established in this client;
            # a status-only response is never enough to authorize Abort.
            return None
        frame = self.safe_status()
        if frame is None:
            return None
        try:
            status = decode_status_v2(frame.payload)
        except OtaToolError:
            return None
        if (
            status.record_transaction_id != self.transaction_id
            or not status.active_present
            or status.image_sha256 != self._expected_image_sha256
            or status.manifest_sha256 != self._expected_manifest_sha256
        ):
            return None
        if status.state not in (
            STATE_MAINTENANCE,
            STATE_RECEIVING,
        ):
            return None
        if status.status_flags & STATUS_PENDING_VERIFY:
            return None
        return self.safe_abort()

    def wait_peer_confirmation(
        self,
        *,
        expected_generation: int,
        expected_image_sha256: bytes | None = None,
        expected_manifest_sha256: bytes | None = None,
        timeout: float = 3.0,
        poll_interval: float = 0.1,
    ) -> StatusPayload:
        """Wait for an explicit receiver assertion that its peer is healthy.

        A successful status response alone is not enough after the right board
        reboots: the current protocol only conveys peer health through
        ``kStatusPeerCompatible`` and ``kStatusHealthPassed``. Missing or
        malformed evidence fails closed and callers must not activate left.
        """

        expected_generation = _u32(expected_generation, "expected_generation", minimum=1)
        if timeout <= 0 or poll_interval <= 0:
            raise OtaToolError("peer confirmation timeout/poll interval must be positive")
        deadline = time.monotonic() + timeout
        last_error = "missing peer confirmation"
        while time.monotonic() < deadline:
            try:
                frame = self.status()
                status = decode_status_v2(frame.payload)
                if status.active_generation != expected_generation:
                    last_error = (
                        "missing peer confirmation: active generation "
                        f"{status.active_generation} != {expected_generation}"
                    )
                elif not status_v2_matches_active(
                    status,
                    role=self.target,
                    transaction_id=self.transaction_id,
                    generation=expected_generation,
                    image_sha256=expected_image_sha256,
                    manifest_sha256=expected_manifest_sha256,
                ):
                    last_error = (
                        "missing peer confirmation: board active identity does not "
                        "match transaction/generation/image"
                    )
                elif status.state not in (
                    STATE_MAINTENANCE,
                    STATE_ACTIVATION_PENDING,
                    STATE_PENDING_CONFIRM,
                    STATE_IDLE,
                    STATE_CONFIRMED,
                ):
                    last_error = (
                        "missing peer confirmation: receiver is not in an "
                        f"activated state ({status.state})"
                    )
                elif status.target_generation not in (0, expected_generation):
                    last_error = (
                        "missing peer confirmation: target generation "
                        f"{status.target_generation} != {expected_generation}"
                    )
                elif not (status.status_flags & STATUS_PEER_COMPATIBLE):
                    last_error = (
                        "missing peer confirmation: receiver did not assert "
                        "peer-compatible"
                    )
                elif not (status.status_flags & STATUS_HEALTH_PASSED):
                    last_error = (
                        "missing peer confirmation: receiver health check not passed"
                    )
                elif not (
                    status.status_flags & STATUS_LOCAL_BOOT_VALIDATED
                ):
                    last_error = (
                        "missing peer confirmation: local boot not validated"
                    )
                elif not status.local_validated:
                    last_error = (
                        "missing peer confirmation: local validation is not "
                        "durable in Status V2"
                    )
                else:
                    return status
            except (OtaToolError, OSError) as exc:
                last_error = f"missing peer confirmation: {exc}"
            time.sleep(min(poll_interval, max(0.0, deadline - time.monotonic())))
        raise OtaToolError(last_error)

    def wait_confirmed_peer(
        self,
        *,
        expected_generation: int,
        expected_image_sha256: bytes | None = None,
        expected_manifest_sha256: bytes | None = None,
        timeout: float = 3.0,
        poll_interval: float = 0.1,
    ) -> StatusPayload:
        """Require the peer's active image, idle state and health evidence."""
        expected_generation = _u32(
            expected_generation, "expected_generation", minimum=1
        )
        if timeout <= 0 or poll_interval <= 0:
            raise OtaToolError("confirmed-peer timeout/poll interval must be positive")
        deadline = time.monotonic() + timeout
        last_error = "missing confirmed peer health"
        while time.monotonic() < deadline:
            try:
                status = decode_status_v2(self.status().payload)
                active_match = status_v2_matches_active(
                    status,
                    role=self.target,
                    transaction_id=self.transaction_id,
                    generation=expected_generation,
                    image_sha256=expected_image_sha256,
                    manifest_sha256=expected_manifest_sha256,
                )
                completed_match = status_v2_matches_completed(
                    status,
                    role=self.target,
                    transaction_id=self.transaction_id,
                    generation=expected_generation,
                    image_sha256=expected_image_sha256,
                    manifest_sha256=expected_manifest_sha256,
                )
                if status.active_generation != expected_generation:
                    active_match = False
                if not active_match and not completed_match:
                    last_error = (
                        "missing confirmed peer health: board identity/tombstone "
                        "does not match transaction/generation/image"
                    )
                elif status.state not in (
                    STATE_IDLE, STATE_MAINTENANCE, STATE_CONFIRMED
                ):
                    last_error = (
                        "missing confirmed peer health: state "
                        f"{status.state} is not logically idle"
                    )
                elif (status.status_flags & STATUS_PEER_COMPATIBLE) == 0:
                    last_error = "missing confirmed peer health: incompatible peer"
                elif (status.status_flags & STATUS_HEALTH_PASSED) == 0:
                    last_error = "missing confirmed peer health: health check not passed"
                elif (
                    status.status_flags & STATUS_LOCAL_BOOT_VALIDATED
                ) == 0:
                    last_error = (
                        "missing confirmed peer health: local boot not validated"
                    )
                elif not status.local_validated:
                    last_error = (
                        "missing confirmed peer health: local validation is "
                        "not durable in Status V2"
                    )
                elif completed_match and not status.pair_committed:
                    last_error = (
                        "missing confirmed peer health: pair commit is not "
                        "durable in Status V2"
                    )
                else:
                    return status
            except (OtaToolError, OSError) as exc:
                last_error = f"missing confirmed peer health: {exc}"
            time.sleep(min(poll_interval, max(0.0, deadline - time.monotonic())))
        raise OtaToolError(last_error)

    def abort(self, *, allow_ota: bool = False,
              preflight_status: bool = True) -> Frame:
        if not allow_ota:
            raise OtaToolError("abort requires explicit --allow-ota authorization")
        if preflight_status:
            self.status()
        return self.request(CMD_ABORT)

    def safe_abort(self) -> Frame | None:
        try:
            return self.request(CMD_ABORT, retries=0)
        except Exception:
            return None

    def activate(self, *, allow_activate: bool = False,
                 preflight_status: bool = True) -> Frame:
        if not allow_activate:
            raise OtaToolError(
                "activation requires independent explicit --allow-activate authorization"
        )
        try:
            if preflight_status:
                self.status()
            return self.request(CMD_ACTIVATE)
        except Exception:
            # ACTIVATE changes boot selection before its ACK is observed.  A
            # timeout is therefore ambiguous: sending ABORT here can erase
            # the durable ActivationPending evidence or roll the peer back
            # after the write already took effect.  Reconcile with a
            # read-only STATUS only; the orchestrator owns recovery.
            self.safe_status()
            raise

    def confirm(self, *, allow_confirm: bool = False,
                preflight_status: bool = True) -> Frame:
        if not allow_confirm:
            raise OtaToolError(
                "confirmation requires independent explicit --allow-confirm authorization"
        )
        try:
            # For a forwarded right-board confirmation this read-only status
            # gives the left manager a chance to re-bootstrap UART1.
            if preflight_status:
                self.status()
            return self.request(CMD_CONFIRM)
        except Exception:
            # CONFIRM is a state-changing command and an absent ACK does not
            # prove that it failed.  Never turn that ambiguity into an ABORT;
            # preserve the board's durable evidence for exact-tx reconcile.
            self.safe_status()
            raise


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
        transaction_id=args.transaction_id if transaction_id is None else transaction_id,
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


def _frame_info(frame: Frame) -> dict[str, Any]:
    info: dict[str, Any] = {
        "command": f"0x{frame.command:02x}",
        "transaction_id": frame.transaction_id,
        "offset": frame.offset,
        "sequence": frame.sequence,
        "payload_hex": frame.payload.hex(),
    }
    try:
        status_v2 = decode_status_v2(frame.payload)
    except OtaToolError:
        try:
            status = decode_status_payload(frame.payload)
        except OtaToolError:
            return info
        info["status"] = {
            "version": 1,
            "state": status.state,
            "last_command": f"0x{status.last_command:02x}",
            "reason": status.reason,
            "reason_name": NACK_REASONS.get(status.reason, "unknown"),
            "next_offset": status.next_offset,
            "target_generation": status.target_generation,
            "active_generation": status.active_generation,
            "status_flags": status.status_flags,
        }
        return info
    info["status"] = status_v2_json(status_v2)
    return info


# Consumers (notably update_proxy_pair.py) require the *complete* board fact,
# so this projection is derived from the dataclass rather than hand-listed.  A
# hand-maintained list silently dropped seven decoded fields, which made every
# board's status look incomplete and blocked the orchestrator before prepare.
def status_v2_json(status: StatusPayloadV2) -> dict[str, Any]:
    """Project a decoded Status V2 into JSON without losing any board fact."""

    payload: dict[str, Any] = {"version": 2}
    for field in dataclass_fields(status):
        value = getattr(status, field.name)
        payload[field.name] = value.hex() if isinstance(value, bytes) else value
    # Derived conveniences and the historical display formats callers parse.
    payload["last_command"] = f"0x{status.last_command:02x}"
    payload["last_response_command"] = f"0x{status.last_response_command:02x}"
    payload["reason_name"] = NACK_REASONS.get(status.reason, "unknown")
    payload["local_validated"] = status.local_validated
    payload["pair_committed"] = status.pair_committed
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

    def serial_parser(name: str, *, package_arg: bool = False) -> argparse.ArgumentParser:
        command = sub.add_parser(name)
        command.add_argument("--port", required=True, help="explicit maintenance UART port")
        command.add_argument("--target", choices=("left", "right"), required=True)
        command.add_argument("--baud", type=int, default=4_000_000)
        command.add_argument("--timeout", type=float, default=DEFAULT_UART_REQUEST_TIMEOUT)
        command.add_argument("--retries", type=int, default=3)
        command.add_argument("--transaction-id", type=lambda x: int(x, 0))
        if package_arg:
            command.add_argument("--package", required=True)
        return command

    status_command = serial_parser("status")
    status_command.add_argument(
        "--legacy-v1", action="store_true",
        help="explicit diagnostic compatibility; never used by automatic updater",
    )
    abort = serial_parser("abort")
    abort.add_argument("--allow-ota", action="store_true")
    prepare = serial_parser("prepare", package_arg=True)
    prepare.add_argument("--allow-ota", action="store_true")
    prepare.add_argument("--package-sha256", required=True,
                         help="exact SHA-256 of the complete manifest+image package")
    activate = serial_parser("activate")
    activate.add_argument("--allow-activate", action="store_true")
    confirm = serial_parser("confirm")
    confirm.add_argument("--allow-confirm", action="store_true")

    pair_prepare = sub.add_parser(
        "pair-prepare",
        help="prepare right then left through one explicit left-board port; never activate",
    )
    pair_prepare.add_argument("--port", required=True,
                              help="the one explicit CH343/left-board maintenance port")
    pair_prepare.add_argument("--right-package", required=True)
    pair_prepare.add_argument("--left-package", required=True)
    pair_prepare.add_argument("--allow-ota", action="store_true")
    pair_prepare.add_argument("--right-package-sha256", required=True)
    pair_prepare.add_argument("--left-package-sha256", required=True)
    pair_prepare.add_argument("--baud", type=int, default=4_000_000)
    pair_prepare.add_argument("--timeout", type=float, default=DEFAULT_UART_REQUEST_TIMEOUT)
    pair_prepare.add_argument("--retries", type=int, default=3)
    pair_prepare.add_argument("--transaction-id", type=lambda x: int(x, 0))

    pair_activate = sub.add_parser(
        "pair-activate",
        help="activate right then left through one port after peer confirmation",
    )
    pair_activate.add_argument("--port", required=True,
                               help="the one explicit CH343/left-board maintenance port")
    pair_activate.add_argument("--allow-activate", action="store_true")
    pair_activate.add_argument("--target-generation", type=int, required=True,
                               help="expected right-board generation after reboot")
    pair_activate.add_argument(
        "--right-image-sha256",
        help="signed right image SHA-256 required for exact Status V2 reconcile",
    )
    pair_activate.add_argument(
        "--left-image-sha256",
        help="signed left image SHA-256 required for exact Status V2 reconcile",
    )
    pair_activate.add_argument(
        "--right-manifest-sha256",
        help="right manifest wire SHA-256 required for exact Status V2 reconcile",
    )
    pair_activate.add_argument(
        "--left-manifest-sha256",
        help="left manifest wire SHA-256 required for exact Status V2 reconcile",
    )
    pair_activate.add_argument("--peer-confirm-timeout", type=float, default=3.0)
    pair_activate.add_argument("--peer-confirm-poll", type=float, default=0.1)
    pair_activate.add_argument("--peer-settle", type=float, default=0.25)
    pair_activate.add_argument("--baud", type=int, default=4_000_000)
    pair_activate.add_argument("--timeout", type=float, default=DEFAULT_UART_REQUEST_TIMEOUT)
    pair_activate.add_argument("--retries", type=int, default=3)
    pair_activate.add_argument("--transaction-id", type=lambda x: int(x, 0), required=True,
                               help="the same transaction id printed by pair-prepare")
    pair_confirm = sub.add_parser(
        "pair-confirm",
        help="confirm right then left after both boot and pass pair health checks",
    )
    pair_confirm.add_argument("--port", required=True)
    pair_confirm.add_argument("--allow-confirm", action="store_true")
    pair_confirm.add_argument("--target-generation", type=int, required=True)
    pair_confirm.add_argument("--right-image-sha256")
    pair_confirm.add_argument("--left-image-sha256")
    pair_confirm.add_argument("--right-manifest-sha256")
    pair_confirm.add_argument("--left-manifest-sha256")
    pair_confirm.add_argument("--peer-confirm-timeout", type=float, default=3.0)
    pair_confirm.add_argument("--peer-confirm-poll", type=float, default=0.1)
    pair_confirm.add_argument("--peer-settle", type=float, default=0.25)
    pair_confirm.add_argument("--baud", type=int, default=4_000_000)
    pair_confirm.add_argument("--timeout", type=float, default=DEFAULT_UART_REQUEST_TIMEOUT)
    pair_confirm.add_argument("--retries", type=int, default=3)
    pair_confirm.add_argument("--transaction-id", type=lambda x: int(x, 0),
                              required=True)
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
                          RIGHT_MANIFEST_PROJECT_NAME),
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
                frame = client.status(v2=not args.legacy_v1)
                _print({"command": "status", "response": _frame_info(frame)})
            finally:
                _close_client(client)
            return 0
        if args.command == "abort":
            client = _open_client(args.port, _role_value(args.target), args)
            try:
                frame = client.abort(allow_ota=args.allow_ota)
                _print({"command": "abort", "response": _frame_info(frame)})
            finally:
                _close_client(client)
            return 0
        if args.command == "prepare":
            package, _manifest = _load_package(args.package)
            client = _open_client(args.port, _manifest.role, args)
            try:
                _print(client.prepare(
                    package,
                    allow_ota=args.allow_ota,
                    expected_package_sha256=args.package_sha256,
                ))
            finally:
                _close_client(client)
            return 0
        if args.command in ("activate", "confirm"):
            client = _open_client(args.port, _role_value(args.target), args)
            try:
                frame = client.activate(allow_activate=args.allow_activate) \
                    if args.command == "activate" else \
                    client.confirm(allow_confirm=args.allow_confirm)
                _print({"command": args.command, "response": _frame_info(frame)})
            finally:
                _close_client(client)
            return 0
        if args.command == "pair-prepare":
            if not args.allow_ota:
                raise OtaToolError("pair-prepare requires explicit --allow-ota authorization")
            right_package, right_manifest = _load_package(args.right_package)
            left_package, left_manifest = _load_package(args.left_package)
            if right_manifest.role != TARGET_RIGHT or left_manifest.role != TARGET_LEFT:
                raise OtaToolError("pair package roles must be right and left respectively")
            tx = args.transaction_id or (secrets.randbelow(0xFFFFFFFF) or 1)
            transport = SerialTransport(
                args.port, baudrate=args.baud, timeout=min(args.timeout, 0.05)
            )
            right = OtaClient(
                transport, target=TARGET_RIGHT, transaction_id=tx,
                timeout=args.timeout, retries=args.retries,
            )
            left = OtaClient(
                transport, target=TARGET_LEFT, transaction_id=tx,
                timeout=args.timeout, retries=args.retries,
            )
            try:
                right.prepare(
                    right_package,
                    allow_ota=True,
                    expected_package_sha256=args.right_package_sha256,
                )
                left.prepare(
                    left_package,
                    allow_ota=True,
                    expected_package_sha256=args.left_package_sha256,
                )
                right_fact = decode_status_v2(right.status().payload)
                left_fact = decode_status_v2(left.status().payload)
                right_manifest_sha = _sha256(right_manifest.wire)
                left_manifest_sha = _sha256(left_manifest.wire)
                if (
                    right_fact.state != STATE_PREPARED
                    or not status_v2_matches_active(
                        right_fact,
                        role=TARGET_RIGHT,
                        transaction_id=tx,
                        generation=right_manifest.generation,
                        image_sha256=right_manifest.image_sha256,
                        manifest_sha256=right_manifest_sha,
                    )
                ):
                    raise OtaToolError(
                        "right PREPARE lacks exact Status V2 transaction/image fact"
                    )
                if (
                    left_fact.state != STATE_PREPARED
                    or not status_v2_matches_active(
                        left_fact,
                        role=TARGET_LEFT,
                        transaction_id=tx,
                        generation=left_manifest.generation,
                        image_sha256=left_manifest.image_sha256,
                        manifest_sha256=left_manifest_sha,
                    )
                ):
                    raise OtaToolError(
                        "left PREPARE lacks exact Status V2 transaction/image fact"
                    )
                _print(
                    {
                        "command": "pair-prepare",
                        "prepared_order": ["right", "left"],
                        "activated": False,
                        "transaction_id": right.transaction_id,
                        "prepared_facts": {
                            "right": _status_v2_summary(right_fact),
                            "left": _status_v2_summary(left_fact),
                        },
                    }
                )
            except Exception:
                right.safe_abort_pre_activation()
                if left is not None:
                    left.safe_abort_pre_activation()
                raise
            finally:
                _close_client(right)
            return 0
        if args.command == "pair-activate":
            if not args.allow_activate:
                raise OtaToolError(
                    "pair-activate requires independent explicit --allow-activate authorization"
                )
            tx = args.transaction_id or (secrets.randbelow(0xFFFFFFFF) or 1)
            transport = SerialTransport(
                args.port, baudrate=args.baud, timeout=min(args.timeout, 0.05)
            )
            right = OtaClient(
                transport, target=TARGET_RIGHT, transaction_id=tx,
                timeout=args.timeout, retries=args.retries,
            )
            left = OtaClient(
                transport, target=TARGET_LEFT, transaction_id=tx,
                timeout=args.timeout, retries=args.retries,
            )
            try:
                right_image_sha = _hex_bytes(
                    args.right_image_sha256, 32, "right_image_sha256"
                )
                left_image_sha = _hex_bytes(
                    args.left_image_sha256, 32, "left_image_sha256"
                )
                right_manifest_sha = _hex_bytes(
                    args.right_manifest_sha256, 32, "right_manifest_sha256"
                )
                left_manifest_sha = _hex_bytes(
                    args.left_manifest_sha256, 32, "left_manifest_sha256"
                )
                before = decode_status_v2(right.status().payload)
                right_disposition = activation_disposition(
                    before,
                    role=TARGET_RIGHT,
                    transaction_id=tx,
                    generation=args.target_generation,
                    image_sha256=right_image_sha,
                    manifest_sha256=right_manifest_sha,
                )
                if right_disposition == "activate":
                    right.activate(allow_activate=True, preflight_status=False)
                    # The right board reboots after ACTIVATE.  Wait for the
                    # left manager to re-establish UART1 before polling it.
                    time.sleep(max(0.0, args.peer_settle))
                right_after = OtaClient(
                    transport, target=TARGET_RIGHT, transaction_id=tx,
                    timeout=args.timeout, retries=args.retries,
                )
                if right_disposition == "completed":
                    peer = right_after.wait_confirmed_peer(
                        expected_generation=args.target_generation,
                        expected_image_sha256=right_image_sha,
                        expected_manifest_sha256=right_manifest_sha,
                        timeout=args.peer_confirm_timeout,
                        poll_interval=args.peer_confirm_poll,
                    )
                else:
                    peer = right_after.wait_peer_confirmation(
                        expected_generation=args.target_generation,
                        expected_image_sha256=right_image_sha,
                        expected_manifest_sha256=right_manifest_sha,
                        timeout=args.peer_confirm_timeout,
                        poll_interval=args.peer_confirm_poll,
                    )
                left_before = decode_status_v2(left.status().payload)
                left_disposition = activation_disposition(
                    left_before,
                    role=TARGET_LEFT,
                    transaction_id=tx,
                    generation=args.target_generation,
                    image_sha256=left_image_sha,
                    manifest_sha256=left_manifest_sha,
                )
                if left_disposition == "activate":
                    left.activate(allow_activate=True, preflight_status=False)
                _print(
                    {
                        "command": "pair-activate",
                        "activation_order": ["right", "left"],
                        "atomic": False,
                        "peer_confirmation": {
                            "target_generation": peer.target_generation,
                            "active_generation": peer.active_generation,
                            "status_flags": peer.status_flags,
                            "fact": _status_v2_summary(peer),
                        },
                    }
                )
            except Exception:
                right.safe_status()
                left.safe_status()
                raise
            finally:
                _close_client(right)
            return 0
        if args.command == "pair-confirm":
            if not args.allow_confirm:
                raise OtaToolError(
                    "pair-confirm requires independent explicit --allow-confirm authorization"
                )
            tx = args.transaction_id
            transport = SerialTransport(
                args.port, baudrate=args.baud, timeout=min(args.timeout, 0.05)
            )
            right = OtaClient(
                transport, target=TARGET_RIGHT, transaction_id=tx,
                timeout=args.timeout, retries=args.retries,
            )
            left = OtaClient(
                transport, target=TARGET_LEFT, transaction_id=tx,
                timeout=args.timeout, retries=args.retries,
            )
            try:
                # Read both boards on the exact transaction before issuing
                # either CONFIRM write.  This is important after a lost host
                # response: the boards may already be confirmed even though
                # the local journal is still at ``activated``.
                right_image_sha = _hex_bytes(
                    args.right_image_sha256, 32, "right_image_sha256"
                )
                left_image_sha = _hex_bytes(
                    args.left_image_sha256, 32, "left_image_sha256"
                )
                right_manifest_sha = _hex_bytes(
                    args.right_manifest_sha256, 32, "right_manifest_sha256"
                )
                left_manifest_sha = _hex_bytes(
                    args.left_manifest_sha256, 32, "left_manifest_sha256"
                )
                before_right = decode_status_v2(right.status().payload)
                before_left = decode_status_v2(left.status().payload)
                right_already_confirmed = (
                    status_v2_matches_completed(
                        before_right,
                        role=TARGET_RIGHT,
                        transaction_id=tx,
                        generation=args.target_generation,
                        image_sha256=right_image_sha,
                        manifest_sha256=right_manifest_sha,
                    )
                    and before_right.local_validated
                    and before_right.pair_committed
                    and (before_right.status_flags & STATUS_PEER_COMPATIBLE) != 0
                    and (before_right.status_flags & STATUS_HEALTH_PASSED) != 0
                )
                left_already_confirmed = (
                    status_v2_matches_completed(
                        before_left,
                        role=TARGET_LEFT,
                        transaction_id=tx,
                        generation=args.target_generation,
                        image_sha256=left_image_sha,
                        manifest_sha256=left_manifest_sha,
                    )
                    and before_left.local_validated
                    and before_left.pair_committed
                    and (before_left.status_flags &
                         STATUS_LOCAL_BOOT_VALIDATED) != 0
                    and (before_left.status_flags & STATUS_PEER_COMPATIBLE) != 0
                    and (before_left.status_flags & STATUS_HEALTH_PASSED) != 0
                )
                left_pending_confirm = (
                    before_left.state == STATE_PENDING_CONFIRM
                    and status_v2_matches_active(
                        before_left,
                        role=TARGET_LEFT,
                        transaction_id=tx,
                        generation=args.target_generation,
                        image_sha256=left_image_sha,
                        manifest_sha256=left_manifest_sha,
                    )
                )
                if not left_already_confirmed and not left_pending_confirm:
                    # ActivationPending is a valid pre-reboot marker, but the
                    # left state machine accepts CONFIRM only in
                    # PendingConfirm.  Fail closed without issuing a write.
                    raise OtaToolError(
                        "left board is neither already confirmed nor pending "
                        "confirmation at the requested generation"
                    )

                confirm_order: list[str] = []
                confirm_skipped: list[str] = []

                if not right_already_confirmed:
                    if (
                        before_right.state not in (
                            STATE_ACTIVATION_PENDING, STATE_PENDING_CONFIRM
                        )
                        or not status_v2_matches_active(
                            before_right,
                            role=TARGET_RIGHT,
                            transaction_id=tx,
                            generation=args.target_generation,
                            image_sha256=right_image_sha,
                            manifest_sha256=right_manifest_sha,
                        )
                    ):
                        raise OtaToolError(
                            "right board lacks exact pending Status V2 identity"
                        )
                    right.confirm(allow_confirm=True, preflight_status=False)
                    confirm_order.append("right")
                    time.sleep(max(0.0, args.peer_settle))
                else:
                    confirm_skipped.append("right")
                right_after = OtaClient(
                    transport, target=TARGET_RIGHT, transaction_id=tx,
                    timeout=args.timeout, retries=args.retries,
                )
                peer = right_after.wait_confirmed_peer(
                    expected_generation=args.target_generation,
                    expected_image_sha256=right_image_sha,
                    expected_manifest_sha256=right_manifest_sha,
                    timeout=args.peer_confirm_timeout,
                    poll_interval=args.peer_confirm_poll,
                )

                if not left_already_confirmed:
                    # Re-read immediately before the second write so a
                    # concurrent/retried host cannot turn an already-idle
                    # left board into an InvalidState NACK.  Only
                    # PendingConfirm is writable; ActivationPending and all
                    # other states fail closed without a CONFIRM frame.
                    left_now = decode_status_v2(left.status().payload)
                    if (
                        left_now.state != STATE_PENDING_CONFIRM
                        or not status_v2_matches_active(
                            left_now,
                            role=TARGET_LEFT,
                            transaction_id=tx,
                            generation=args.target_generation,
                            image_sha256=left_image_sha,
                            manifest_sha256=left_manifest_sha,
                        )
                    ):
                        raise OtaToolError(
                            "left board is no longer pending confirmation at "
                            "the requested generation"
                        )
                    left.confirm(allow_confirm=True, preflight_status=False)
                    confirm_order.append("left")
                else:
                    confirm_skipped.append("left")
                _print({
                    "command": "pair-confirm",
                    "confirm_order": confirm_order,
                    "confirm_skipped": confirm_skipped,
                    "transaction_id": tx,
                    "active_generation": peer.active_generation,
                    "right_fact": _status_v2_summary(peer),
                    "pair_health_confirmed": True,
                })
            except Exception:
                right.safe_status()
                left.safe_status()
                raise
            finally:
                _close_client(right)
            return 0
        raise OtaToolError(f"unknown command {args.command}")
    except OtaToolError as exc:
        print(f"makcm_ota_tool: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
