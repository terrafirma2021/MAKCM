from __future__ import annotations

import hashlib
from pathlib import Path
import re
import sys
import tempfile
import unittest
import struct
from unittest import mock

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import makcm_ota_tool as ota  # noqa: E402


HEALTHY_OTA_FLAGS = (
    ota.STATUS_PEER_COMPATIBLE
    | ota.STATUS_HEALTH_PASSED
    | ota.STATUS_LOCAL_BOOT_VALIDATED
)


def app_image(size: int = 8192) -> bytes:
    image = bytearray((i * 31 + 7) & 0xFF for i in range(size))
    image[0:4] = bytes((0xE9, 2, 2, 0x2F))
    # Keep synthetic app data away from merged-image partition magic offsets.
    if size > 0x8000:
        image[0x8000:0x8002] = b"\x00\x00"
    return bytes(image)


def intrinsic_image(role: str, generation: int, *, marker_count: int = 1) -> bytes:
    """Build the minimum structurally valid ESP/MAKCM identity fixture."""

    image = bytearray(1024)
    image[0:4] = bytes((0xE9, 2, 2, 0x2F))
    descriptor_offset = 0x20
    struct.pack_into("<I", image, descriptor_offset, ota.ESP_APP_DESC_MAGIC)

    def put_text(offset: int, capacity: int, value: str) -> None:
        encoded = value.encode("ascii")
        if len(encoded) >= capacity:
            raise AssertionError(value)
        image[offset : offset + len(encoded)] = encoded
        image[offset + len(encoded)] = 0

    if role == "left":
        descriptor_version = f"makcm-g{generation}"
        descriptor_project = ota.LEFT_DESCRIPTOR_PROJECT
        descriptor_idf = "v5.5.5"
        marker_role = ota.TARGET_LEFT
        schema = ota.LEFT_PARTITION_SCHEMA_V1
        marker_project = ota.LEFT_PROJECT_NAME
    else:
        descriptor_version = "esp-idf: v4.4.7 38eeba213a"
        descriptor_project = ota.RIGHT_DESCRIPTOR_PROJECT
        descriptor_idf = "v4.4.7-dirty"
        marker_role = ota.TARGET_RIGHT
        schema = ota.RIGHT_PARTITION_SCHEMA_V1
        marker_project = ota.RIGHT_MANIFEST_PROJECT_NAME
    put_text(descriptor_offset + 16, 32, descriptor_version)
    put_text(descriptor_offset + 48, 32, descriptor_project)
    put_text(descriptor_offset + 112, 32, descriptor_idf)
    image[descriptor_offset + 144 : descriptor_offset + 176] = bytes(range(1, 33))

    for index in range(marker_count):
        offset = 0x180 + index * ota.EMBEDDED_IDENTITY_SIZE
        marker = bytearray(ota.EMBEDDED_IDENTITY_SIZE)
        marker[:8] = ota.EMBEDDED_IDENTITY_MAGIC
        struct.pack_into("<HH", marker, 8, ota.EMBEDDED_IDENTITY_VERSION,
                         ota.EMBEDDED_IDENTITY_SIZE)
        marker[12] = marker_role
        struct.pack_into("<IIH", marker, 16, generation, schema,
                         ota.DEFAULT_PROTOCOL_VERSION)
        def marker_text(start: int, capacity: int, value: str) -> None:
            encoded = value.encode("ascii")
            if len(encoded) >= capacity:
                raise AssertionError(value)
            marker[start : start + len(encoded)] = encoded
            marker[start + len(encoded)] = 0
        marker_text(28, 16, ota.HARDWARE_REVISION_R1)
        marker_text(44, 32, marker_project)
        marker_text(76, 32, f"makcm-g{generation}")
        marker[108:116] = ota.EMBEDDED_IDENTITY_TRAILER
        image[offset : offset + len(marker)] = marker
    return bytes(image)


def status_payload(*, state: int = 2, last_command: int, reason: int = 0,
                   next_offset: int = 0,
                   target_generation: int = 0, active_generation: int = 0,
                   flags: int = 0) -> bytes:
    payload = bytearray(24)
    payload[0:4] = bytes((ota.WIRE_VERSION, state, last_command, 0))
    payload[4:6] = reason.to_bytes(2, "little")
    payload[8:12] = next_offset.to_bytes(4, "little")
    payload[12:16] = target_generation.to_bytes(4, "little")
    payload[16:20] = active_generation.to_bytes(4, "little")
    payload[20:24] = flags.to_bytes(4, "little")
    return bytes(payload)


STATUS_IMAGE_SHA = bytes.fromhex("11" * 32)
STATUS_MANIFEST_SHA = bytes.fromhex("22" * 32)


def status_v2_wire(*, role: int, state: int, last_command: int,
                   transaction_id: int, target_generation: int,
                   active_generation: int, flags: int = 0,
                   next_offset: int = 0, completed: bool = False,
                   image_sha: bytes = STATUS_IMAGE_SHA,
                   manifest_sha: bytes = STATUS_MANIFEST_SHA) -> bytes:
    evidence = ota.STATUS_V2_EVIDENCE_SNAPSHOT_VALID
    active = bool(transaction_id and target_generation and not completed)
    locally_validated = bool(flags & ota.STATUS_LOCAL_BOOT_VALIDATED)
    if active:
        evidence |= (
            ota.STATUS_V2_EVIDENCE_ACTIVE
            | ota.STATUS_V2_EVIDENCE_IDENTITY_VALID
        )
    if locally_validated:
        evidence |= ota.STATUS_V2_EVIDENCE_LOCAL_VALIDATED
    if completed:
        evidence |= (
            ota.STATUS_V2_EVIDENCE_COMPLETED
            | ota.STATUS_V2_EVIDENCE_LOCAL_VALIDATED
            | ota.STATUS_V2_EVIDENCE_PAIR_COMMITTED
            | ota.STATUS_V2_EVIDENCE_IDENTITY_VALID
        )
    status = ota.StatusPayloadV2(
        role=role,
        local_phase=(6 if active and locally_validated else (2 if active else 0)),
        state=state, last_command=last_command,
        last_response_command=ota.CMD_STATUS_RESPONSE,
        last_ota_operation=0, completed_outcome=1 if completed else 0,
        reason=0, protocol_epoch=2, status_flags=flags,
        next_offset=next_offset,
        record_transaction_id=transaction_id if active else 0,
        completed_transaction_id=transaction_id if completed else 0,
        target_generation=target_generation if active else (target_generation if completed else 0),
        active_generation=active_generation,
        previous_generation=max(1, target_generation - 1),
        partition_schema=1, image_size=8192, record_sequence=1,
        last_request_sequence=1 if active else 0,
        completed_terminal_sequence=9 if completed else 0,
        reset_reason=1, last_ota_error=0, last_state_query_error=0,
        boot_attempt=1, evidence_flags=evidence, running_slot=16,
        boot_slot=17, target_slot=17 if active else 0xFF,
        previous_slot=16, image_state=1,
        completed_terminal_command=ota.CMD_CONFIRM if completed else ota.CMD_HELLO,
        record_crc32=0x12345678, image_sha256=image_sha,
        completed_image_sha256=image_sha if completed else bytes(32),
        running_image_sha256=image_sha, manifest_sha256=manifest_sha,
        identity_digest=bytes.fromhex("33" * 32),
        previous_image_sha256=bytes.fromhex("44" * 32),
        app_elf_sha256=bytes.fromhex("55" * 32),
        hardware_revision="makcm-s3-r1", project_name="makcm-test",
        app_version="makcm-g7",
    )
    return ota.encode_status_v2(status)


class MockTransport:
    def __init__(self, *, fail_manifest: bool = False, wrong_offset_once: bool = False,
                 mismatch_status: bool = False, status_generation: int = 0,
                 active_generation: int = 0, status_state: int = 2,
                 status_flags: int = 0, skip_data_offset_once: bool = False):
        self.incoming = bytearray()
        self.writes: list[ota.Frame] = []
        self.closed = False
        self.fail_manifest = fail_manifest
        self.wrong_offset_once = wrong_offset_once
        self.mismatch_status = mismatch_status
        self.status_generation = status_generation
        self.active_generation = active_generation
        self.status_state = status_state
        self.status_flags = status_flags
        self.skip_data_offset_once = skip_data_offset_once
        self.record_tx = 0
        self.record_generation = status_generation
        self.record_image_sha = STATUS_IMAGE_SHA
        self.record_manifest_sha = STATUS_MANIFEST_SHA
        self.last_accepted_offset = 0

    def write(self, data: bytes) -> int:
        frame = ota.decode_frame(data)
        self.writes.append(frame)
        if frame.command == ota.CMD_MANIFEST:
            manifest = ota.decode_manifest(frame.payload)
            self.record_tx = frame.transaction_id
            self.record_generation = manifest.generation
            self.record_image_sha = manifest.image_sha256
            self.record_manifest_sha = hashlib.sha256(frame.payload).digest()
        if self.fail_manifest and frame.command == ota.CMD_MANIFEST:
            self.incoming += ota.encode_frame(
                target=frame.target,
                command=ota.CMD_NACK,
                transaction_id=frame.transaction_id,
                offset=0,
                sequence=frame.sequence,
                payload=status_payload(last_command=frame.command, reason=8),
                flags=ota.FLAG_RESPONSE,
            )
            return len(data)
        if self.wrong_offset_once and frame.command == ota.CMD_DATA:
            self.wrong_offset_once = False
            # Model an ACK-lost duplicate: the receiver already committed the
            # chunk and its exact Status V2 therefore advances the offset.
            self.last_accepted_offset = frame.offset + len(frame.payload)
            self.incoming += ota.encode_frame(
                target=frame.target,
                command=ota.CMD_NACK,
                transaction_id=frame.transaction_id,
                offset=0,
                sequence=frame.sequence,
                payload=status_payload(last_command=frame.command, reason=9),
                flags=ota.FLAG_RESPONSE,
            )
            return len(data)
        if frame.command == ota.CMD_STATUS:
            generation = self.status_generation or self.record_generation
            # A board with no active transaction still runs some image, so its
            # active generation is never zero even when target_generation is.
            active = self.active_generation or generation or 1
            active_tx = self.record_tx or frame.transaction_id if generation else 0
            payload = status_v2_wire(
                role=frame.target,
                state=self.status_state,
                last_command=frame.command,
                transaction_id=active_tx,
                target_generation=generation,
                active_generation=active,
                flags=self.status_flags,
                next_offset=self.last_accepted_offset,
                image_sha=self.record_image_sha,
                manifest_sha=self.record_manifest_sha,
            )
            self.incoming += ota.encode_frame(
                target=frame.target,
                command=ota.CMD_STATUS_RESPONSE,
                transaction_id=frame.transaction_id,
                offset=self.last_accepted_offset,
                sequence=frame.sequence,
                payload=payload,
                flags=ota.FLAG_RESPONSE | ota.FLAG_STATUS_V2,
            )
            return len(data)
        accepted = frame.offset + len(frame.payload) if frame.command == ota.CMD_DATA else frame.offset
        if frame.command == ota.CMD_DATA and self.skip_data_offset_once:
            self.skip_data_offset_once = False
            accepted += 1
        if frame.command == ota.CMD_DATA:
            self.last_accepted_offset = accepted
        response_command = (
            ota.CMD_STATUS_RESPONSE if frame.command == ota.CMD_STATUS else ota.CMD_ACK
        )
        response_payload = status_payload(
            state=self.status_state,
            last_command=frame.command,
            next_offset=accepted,
            target_generation=self.status_generation,
            active_generation=self.active_generation,
            flags=self.status_flags,
        )
        response_offset = frame.offset if self.mismatch_status else accepted
        self.incoming += ota.encode_frame(
            target=frame.target,
            command=response_command,
            transaction_id=frame.transaction_id,
            offset=response_offset,
            sequence=frame.sequence,
            payload=response_payload,
            flags=ota.FLAG_RESPONSE,
        )
        return len(data)

    def read(self, size: int = 1) -> bytes:
        if not self.incoming:
            return b""
        result = bytes(self.incoming[:size])
        del self.incoming[:size]
        return result

    def close(self) -> None:
        self.closed = True


class DropResponseTransport(MockTransport):
    """Drop the response to one state-changing command after recording it."""

    def __init__(self, dropped_command: int):
        super().__init__()
        self.dropped_command = dropped_command

    def write(self, data: bytes) -> int:
        frame = ota.decode_frame(data)
        if frame.command == self.dropped_command:
            self.writes.append(frame)
            return len(data)
        return super().write(data)


class PairConfirmTransport:
    """Scripted exact-target status/confirm transport for the pair CLI."""

    def __init__(self, *, right_statuses, left_statuses):
        self.right_statuses = list(right_statuses)
        self.left_statuses = list(left_statuses)
        self.right_status_index = 0
        self.left_status_index = 0
        self.incoming = bytearray()
        self.writes: list[ota.Frame] = []
        self.closed = False

    @staticmethod
    def _wire_status(spec, command, transaction_id, target):
        state, target_generation, active_generation, flags = spec
        completed = state == ota.STATE_CONFIRMED
        return status_v2_wire(
            role=target,
            state=state,
            last_command=command,
            transaction_id=transaction_id if target_generation else (transaction_id if completed else 0),
            target_generation=target_generation,
            active_generation=active_generation,
            flags=flags,
            completed=completed,
        )

    def _next_status(self, target):
        if target == ota.TARGET_RIGHT:
            values = self.right_statuses
            index = self.right_status_index
            self.right_status_index += 1
        else:
            values = self.left_statuses
            index = self.left_status_index
            self.left_status_index += 1
        if not values:
            raise AssertionError("missing scripted status")
        return values[min(index, len(values) - 1)]

    def write(self, data: bytes) -> int:
        frame = ota.decode_frame(data)
        self.writes.append(frame)
        if frame.command == ota.CMD_STATUS:
            spec = self._next_status(frame.target)
            response_command = ota.CMD_STATUS_RESPONSE
        elif frame.command == ota.CMD_CONFIRM:
            if frame.target == ota.TARGET_RIGHT:
                spec = (
                    ota.STATE_CONFIRMED,
                    5,
                    5,
                    HEALTHY_OTA_FLAGS,
                )
            else:
                spec = (ota.STATE_CONFIRMED, 0, 5, 0)
            response_command = ota.CMD_ACK
        else:
            spec = (ota.STATE_PENDING_CONFIRM, 5, 5, 0)
            response_command = ota.CMD_ACK
        if frame.command == ota.CMD_STATUS:
            payload = self._wire_status(
                spec, frame.command, frame.transaction_id, frame.target
            )
            response_flags = ota.FLAG_RESPONSE | ota.FLAG_STATUS_V2
        else:
            state, target_generation, active_generation, flags = spec
            payload = status_payload(
                state=state, last_command=frame.command,
                target_generation=target_generation,
                active_generation=active_generation, flags=flags,
            )
            response_flags = ota.FLAG_RESPONSE
        self.incoming += ota.encode_frame(
            target=frame.target,
            command=response_command,
            transaction_id=frame.transaction_id,
            offset=0,
            sequence=frame.sequence,
            payload=payload,
            flags=response_flags,
        )
        return len(data)

    def read(self, size: int = 1) -> bytes:
        if not self.incoming:
            return b""
        result = bytes(self.incoming[:size])
        del self.incoming[:size]
        return result

    def close(self) -> None:
        self.closed = True


class SerialDouble:
    def __init__(self, **kwargs):
        self.kwargs = kwargs
        self.port = kwargs.get("port")
        self.dtr = None
        self.rts = None
        self.writes: list[bytes] = []
        self.opened = False
        self.closed = False
        self.flushed = False
        self.input_reset = False
        self.output_reset = False

    def open(self):
        self.opened = True

    def write(self, data):
        self.writes.append(bytes(data))
        return len(data)

    def flush(self):
        self.flushed = True

    def reset_input_buffer(self):
        self.input_reset = True

    def reset_output_buffer(self):
        self.output_reset = True

    def read(self, size=1):
        return b""

    def close(self):
        self.closed = True


class OtaToolTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.image = app_image()
        self.key = ec.generate_private_key(ec.SECP256R1())
        self.private_path = self.root / "signer.pem"
        self.public_path = self.root / "signer.pub.pem"
        self.private_path.write_bytes(
            self.key.private_bytes(
                serialization.Encoding.PEM,
                serialization.PrivateFormat.PKCS8,
                serialization.NoEncryption(),
            )
        )
        self.public_path.write_bytes(
            self.key.public_key().public_bytes(
                serialization.Encoding.PEM,
                serialization.PublicFormat.SubjectPublicKeyInfo,
            )
        )

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def manifest(self, image: bytes | None = None) -> ota.Manifest:
        return ota.make_manifest(
            image or self.image,
            role="left",
            generation=7,
            min_peer_generation=6,
            max_peer_generation=8,
            partition_schema=0xAABBCCDD,
            hardware_revision="s3-r1",
            project_name="makcm-left",
            signer_key_id="00112233445566778899aabbccddeeff",
        )

    def test_frame_matches_cpp_layout_and_crc(self) -> None:
        payload = bytes(range(64))
        wire = ota.encode_frame(
            target=ota.TARGET_RIGHT,
            command=ota.CMD_DATA,
            transaction_id=0x12345678,
            offset=4096,
            sequence=7,
            payload=payload,
            flags=ota.FLAG_RETRANSMIT,
        )
        frame = ota.decode_frame(wire)
        self.assertEqual(frame.target, ota.TARGET_RIGHT)
        self.assertEqual(frame.offset, 4096)
        self.assertEqual(frame.payload, payload)
        self.assertEqual(len(wire), ota.HEADER_SIZE + len(payload) + ota.CRC_SIZE)
        with self.assertRaises(ota.OtaToolError):
            ota.encode_frame(
                target=ota.TARGET_RIGHT,
                command=0x7F,
                transaction_id=1,
            )

    def test_shared_cpp_python_golden_frame(self) -> None:
        contract = (ROOT / "tests" / "ota_protocol_golden.inc").read_text(
            encoding="ascii"
        )
        match = re.search(
            r'MAKCM_OTA_GOLDEN_FRAME_HEX\s*\\\s*"([0-9a-f]+)"',
            contract,
        )
        self.assertIsNotNone(match)
        expected = bytes.fromhex(match.group(1))
        actual = ota.encode_frame(
            target=ota.TARGET_RIGHT,
            command=ota.CMD_DATA,
            flags=ota.FLAG_RETRANSMIT,
            transaction_id=0x12345678,
            offset=0x10203040,
            sequence=0x01020304,
            payload=bytes((0x00, 0x01, 0x7F, 0x80, 0xFF)),
        )
        self.assertEqual(actual, expected)
        decoded = ota.decode_frame(expected)
        self.assertEqual(decoded.sequence, 0x01020304)
        self.assertEqual(decoded.payload, bytes((0x00, 0x01, 0x7F, 0x80, 0xFF)))

    def test_frame_rejects_reserved_sequence_zero_and_max(self) -> None:
        for sequence in (0, 0xFFFFFFFF):
            with self.subTest(sequence=sequence), self.assertRaisesRegex(
                ota.OtaToolError, "sequence"
            ):
                ota.encode_frame(
                    target=ota.TARGET_LEFT,
                    command=ota.CMD_HELLO,
                    transaction_id=1,
                    sequence=sequence,
                )

    def test_client_stops_before_sequence_wrap(self) -> None:
        transport = MockTransport()
        client = ota.OtaClient(
            transport, target=ota.TARGET_LEFT, transaction_id=91, timeout=0.01,
        )
        client._sequence = 0xFFFFFFFD
        client.request(ota.CMD_HELLO)
        self.assertEqual(client._sequence, 0xFFFFFFFE)
        with self.assertRaisesRegex(ota.OtaToolError, "sequence exhausted"):
            client.request(ota.CMD_HELLO)

    def test_status_v2_rejects_reserved_and_unknown_fields(self) -> None:
        wire = bytearray(status_v2_wire(
            role=ota.TARGET_LEFT, state=ota.STATE_PREPARED,
            last_command=ota.CMD_STATUS, transaction_id=91,
            target_generation=7, active_generation=6,
        ))
        for description, mutate in (
            ("record sequence", lambda value: value.__setitem__(48, 0)),
            ("protocol epoch", lambda value: value.__setitem__(10, 3)),
            ("unknown evidence", lambda value: value.__setitem__(74, 0x80)),
        ):
            with self.subTest(description=description):
                bad = bytearray(wire)
                mutate(bad)
                with self.assertRaises(ota.OtaToolError):
                    ota.decode_status_v2(bytes(bad))

    def test_activation_disposition_does_not_treat_prepared_as_activated(self) -> None:
        prepared = ota.decode_status_v2(status_v2_wire(
            role=ota.TARGET_RIGHT,
            state=ota.STATE_PREPARED,
            last_command=ota.CMD_STATUS,
            transaction_id=91,
            target_generation=7,
            active_generation=6,
        ))
        self.assertEqual(
            ota.activation_disposition(
                prepared,
                role=ota.TARGET_RIGHT,
                transaction_id=91,
                generation=7,
                image_sha256=STATUS_IMAGE_SHA,
                manifest_sha256=STATUS_MANIFEST_SHA,
            ),
            "activate",
        )

        pending = ota.decode_status_v2(status_v2_wire(
            role=ota.TARGET_RIGHT,
            state=ota.STATE_PENDING_CONFIRM,
            last_command=ota.CMD_STATUS,
            transaction_id=91,
            target_generation=7,
            active_generation=7,
        ))
        self.assertEqual(
            ota.activation_disposition(
                pending,
                role=ota.TARGET_RIGHT,
                transaction_id=91,
                generation=7,
                image_sha256=STATUS_IMAGE_SHA,
                manifest_sha256=STATUS_MANIFEST_SHA,
            ),
            "already_activated",
        )

    def test_data_ack_that_skips_offset_is_rejected(self) -> None:
        transport = MockTransport(skip_data_offset_once=True)
        client = ota.OtaClient(
            transport, target=ota.TARGET_LEFT, transaction_id=92, timeout=0.01,
        )
        manifest = ota.sign_manifest(self.manifest(), self.private_path)
        with self.assertRaisesRegex(ota.OtaToolError, "expected"):
            client.send_data(self.image, manifest=manifest)

    def test_explicit_transport_performs_boot_to_4m_switch(self) -> None:
        instances: list[SerialDouble] = []
        serial_module = mock.Mock()

        def factory(**kwargs):
            instance = SerialDouble(**kwargs)
            instances.append(instance)
            return instance

        serial_module.Serial.side_effect = factory
        with mock.patch.dict(sys.modules, {"serial": serial_module}), \
                mock.patch.object(sys, "platform", "win32"), \
                mock.patch.object(ota.time, "sleep"):
            transport = ota.SerialTransport("COM3", baudrate=ota.FAST_BAUD)
            transport.close()
        self.assertEqual([item.kwargs["baudrate"] for item in instances],
                         [ota.BOOT_BAUD, ota.FAST_BAUD])
        self.assertEqual(instances[0].writes, [ota.SPEED_SWITCH])
        self.assertTrue(instances[0].flushed)
        self.assertEqual(instances[1].writes, [ota.SERIAL_FRAME_BOUNDARY])
        self.assertTrue(instances[1].flushed)
        self.assertTrue(instances[1].input_reset)
        self.assertTrue(instances[1].output_reset)
        self.assertFalse(instances[0].dtr)
        self.assertFalse(instances[0].rts)

    def test_115200_transport_also_restores_frame_boundary(self) -> None:
        serial_module = mock.Mock()
        serial_module.Serial.side_effect = lambda **kwargs: SerialDouble(**kwargs)
        with mock.patch.dict(sys.modules, {"serial": serial_module}), \
                mock.patch.object(sys, "platform", "win32"), \
                mock.patch.object(ota.time, "sleep"):
            transport = ota.SerialTransport("COM3", baudrate=ota.BOOT_BAUD)
            instance = transport._serial
            transport.close()
        self.assertEqual(instance.writes, [ota.SERIAL_FRAME_BOUNDARY])
        self.assertTrue(instance.flushed)

    def test_uart_commands_default_above_right_cleanup_budget(self) -> None:
        parser = ota._parser()
        status = parser.parse_args([
            "status", "--port", "COM3", "--target", "left",
        ])
        pair = parser.parse_args([
            "pair-prepare", "--port", "COM3",
            "--right-package", "right.mkota",
            "--left-package", "left.mkota",
            "--right-package-sha256", "0" * 64,
            "--left-package-sha256", "1" * 64,
        ])
        self.assertGreater(status.timeout, 4.0)
        self.assertEqual(status.timeout, ota.DEFAULT_UART_REQUEST_TIMEOUT)
        self.assertEqual(pair.timeout, ota.DEFAULT_UART_REQUEST_TIMEOUT)

    def test_status_payload_matches_cpp_wire(self) -> None:
        payload = bytearray(24)
        payload[0:4] = bytes((ota.WIRE_VERSION, 2, ota.CMD_DATA, 0))
        payload[4:6] = (0).to_bytes(2, "little")
        payload[8:12] = (4096).to_bytes(4, "little")
        payload[12:16] = (7).to_bytes(4, "little")
        status = ota.decode_status_payload(bytes(payload))
        self.assertEqual(status.next_offset, 4096)
        self.assertEqual(status.target_generation, 7)

    def test_manifest_sign_verify_and_raw_signature(self) -> None:
        signed = ota.sign_manifest(self.manifest(), self.private_path)
        self.assertEqual(len(signed.wire), ota.MANIFEST_SIZE)
        self.assertEqual(len(signed.signature), 64)
        self.assertTrue(ota.verify_manifest_signature(signed, self.public_path))
        tampered = ota.Manifest(**{**signed.__dict__, "project_name": "tampered"})
        self.assertFalse(ota.verify_manifest_signature(tampered, self.public_path))

    def test_public_trust_anchor_generation_is_explicit_and_public_only(self) -> None:
        output = self.root / "ota_trust_anchor.local.inc"
        result = ota.write_trust_anchor(
            self.public_path,
            output,
            signer_key_id="00112233445566778899aabbccddeeff",
        )
        text = output.read_text(encoding="ascii")
        self.assertIn("constexpr uint8_t kSignerKeyId[16]", text)
        self.assertIn("constexpr uint8_t kPublicX[32]", text)
        self.assertIn("constexpr uint8_t kPublicY[32]", text)
        self.assertNotIn("PRIVATE KEY", text)
        self.assertEqual(result["signer_key_id"],
                         "00112233445566778899aabbccddeeff")
        with self.assertRaises(ota.OtaToolError):
            ota.write_trust_anchor(
                self.public_path,
                output,
                signer_key_id="00112233445566778899aabbccddeeff",
            )

    def test_package_is_manifest_plus_app_and_rejects_merged(self) -> None:
        package, manifest = ota.build_package(
            self.image,
            private_key_path=self.private_path,
            signer_key_id="00112233445566778899aabbccddeeff",
            role="left",
            generation=7,
            min_peer_generation=6,
            max_peer_generation=8,
            partition_schema=1,
            hardware_revision="s3-r1",
            project_name="makcm-left",
        )
        self.assertEqual(package[: ota.MANIFEST_SIZE], manifest.wire)
        info = ota.package_info(package)
        self.assertEqual(info["package_size"], len(package))
        self.assertEqual(info["image_sha256"], hashlib.sha256(self.image).hexdigest())
        merged = bytearray(self.image + b"\0" * (0x8000 - len(self.image)))
        merged[0x8000:0x8002] = b"\xAA\x50"
        with self.assertRaises(ota.OtaToolError):
            ota.validate_application_image(bytes(merged))

    def test_intrinsic_identity_proves_generation_without_manifest_guessing(self) -> None:
        left = intrinsic_image("left", 15)
        descriptor = ota.validate_intrinsic_identity(left, role="left", generation=15)
        self.assertEqual(descriptor.version, "makcm-g15")
        with self.assertRaisesRegex(ota.OtaToolError, "generation"):
            ota.validate_intrinsic_identity(left, role="left", generation=14)

        right = intrinsic_image("right", 15)
        descriptor = ota.validate_intrinsic_identity(right, role="right", generation=15)
        self.assertEqual(descriptor.project_name, ota.RIGHT_DESCRIPTOR_PROJECT)

        with self.assertRaisesRegex(ota.OtaToolError, "exactly one"):
            ota.validate_intrinsic_identity(
                intrinsic_image("right", 15, marker_count=2),
                role="right", generation=15,
            )

    def test_actual_right_candidate_identity_is_read_from_marker(self) -> None:
        path = ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / ".pio" / "build" / "RIGHT" / "firmware.bin"
        if not path.exists():
            self.skipTest("right candidate firmware.bin is not present")
        image = path.read_bytes()
        identities = ota.parse_embedded_identities(image, label=str(path))
        self.assertEqual(len(identities), 1)
        generation = identities[0].generation
        descriptor = ota.validate_intrinsic_identity(
            image, role="right", generation=generation, label=str(path)
        )
        self.assertTrue(descriptor.idf_version.startswith("v4.4.7"))

    def test_prepare_retries_resume_and_never_activates(self) -> None:
        transport = MockTransport(wrong_offset_once=True)
        client = ota.OtaClient(transport, target=ota.TARGET_LEFT, transaction_id=9, timeout=0.01)
        manifest = ota.sign_manifest(self.manifest(), self.private_path)
        package = manifest.wire + self.image
        result = client.prepare(package, allow_ota=True)
        self.assertEqual(result["image_size"], len(self.image))
        commands = [frame.command for frame in transport.writes]
        self.assertIn(ota.CMD_PREPARE, commands)
        self.assertNotIn(ota.CMD_ACTIVATE, commands)
        self.assertEqual(transport.writes[-1].command, ota.CMD_PREPARE)

    def test_right_prepare_enters_maintenance_without_discovery_hello(self) -> None:
        transport = MockTransport()
        client = ota.OtaClient(
            transport, target=ota.TARGET_RIGHT, transaction_id=10, timeout=0.01
        )
        manifest = ota.sign_manifest(
            ota.make_manifest(
                self.image,
                role="right",
                generation=7,
                min_peer_generation=6,
                max_peer_generation=8,
                partition_schema=0xAABBCCDD,
                hardware_revision="s3-r1",
                project_name="makcm-right",
                signer_key_id="00112233445566778899aabbccddeeff",
            ),
            self.private_path,
        )
        client.prepare(manifest.wire + self.image, allow_ota=True)
        commands = [frame.command for frame in transport.writes]
        self.assertEqual(commands[0], ota.CMD_ENTER_MAINTENANCE)
        self.assertNotIn(ota.CMD_HELLO, commands)

    def test_left_prepare_keeps_local_preflight_hello(self) -> None:
        transport = MockTransport()
        client = ota.OtaClient(
            transport, target=ota.TARGET_LEFT, transaction_id=10, timeout=0.01
        )
        manifest = ota.sign_manifest(self.manifest(), self.private_path)
        client.prepare(manifest.wire + self.image, allow_ota=True)
        commands = [frame.command for frame in transport.writes]
        self.assertEqual(commands[:2], [ota.CMD_HELLO, ota.CMD_ENTER_MAINTENANCE])

    def test_status_offset_mismatch_fails_closed(self) -> None:
        transport = MockTransport(mismatch_status=True)
        client = ota.OtaClient(transport, target=ota.TARGET_LEFT, transaction_id=11,
                               timeout=0.01)
        with self.assertRaises(ota.OtaToolError):
            client.request(ota.CMD_DATA, payload=b"x" * ota.MIN_CHUNK_SIZE)

    def test_confirm_timeout_never_aborts_ambiguous_write(self) -> None:
        transport = DropResponseTransport(ota.CMD_CONFIRM)
        client = ota.OtaClient(
            transport, target=ota.TARGET_LEFT, transaction_id=21,
            timeout=0.001, retries=0,
        )
        with self.assertRaises(ota.OtaToolError):
            client.confirm(allow_confirm=True)
        commands = [frame.command for frame in transport.writes]
        self.assertEqual(commands, [ota.CMD_STATUS, ota.CMD_CONFIRM, ota.CMD_STATUS])
        self.assertNotIn(ota.CMD_ABORT, commands)

    def test_activate_timeout_never_aborts_ambiguous_write(self) -> None:
        transport = DropResponseTransport(ota.CMD_ACTIVATE)
        client = ota.OtaClient(
            transport, target=ota.TARGET_RIGHT, transaction_id=22,
            timeout=0.001, retries=0,
        )
        with self.assertRaises(ota.OtaToolError):
            client.activate(allow_activate=True)
        commands = [frame.command for frame in transport.writes]
        self.assertEqual(commands, [ota.CMD_STATUS, ota.CMD_ACTIVATE, ota.CMD_STATUS])
        self.assertNotIn(ota.CMD_ABORT, commands)

    def test_peer_confirmation_requires_explicit_flag(self) -> None:
        transport = MockTransport(status_generation=3, status_flags=0)
        client = ota.OtaClient(transport, target=ota.TARGET_RIGHT, transaction_id=12,
                               timeout=0.01)
        with self.assertRaisesRegex(ota.OtaToolError, "peer confirmation"):
            client.wait_peer_confirmation(expected_generation=3, timeout=0.02,
                                          poll_interval=0.001)
        good = MockTransport(
            status_generation=3,
            active_generation=3,
            status_state=ota.STATE_PENDING_CONFIRM,
            status_flags=HEALTHY_OTA_FLAGS,
        )
        good_client = ota.OtaClient(good, target=ota.TARGET_RIGHT, transaction_id=12,
                                    timeout=0.01)
        status = good_client.wait_peer_confirmation(expected_generation=3,
                                                    timeout=0.02, poll_interval=0.001)
        self.assertEqual(status.target_generation, 3)

        # A logically idle/healthy response without a completed tombstone is
        # deliberately insufficient for automatic pair confirmation.
        confirmed = MockTransport(
            active_generation=3,
            status_state=0,
            status_flags=HEALTHY_OTA_FLAGS,
        )
        confirmed_client = ota.OtaClient(
            confirmed, target=ota.TARGET_RIGHT, transaction_id=12, timeout=0.01
        )
        with self.assertRaisesRegex(ota.OtaToolError, "identity/tombstone"):
            confirmed_client.wait_confirmed_peer(
                expected_generation=3, timeout=0.02, poll_interval=0.001
            )

    def test_activation_recovery_requires_active_generation(self) -> None:
        flags = HEALTHY_OTA_FLAGS
        stale = MockTransport(
            status_generation=3,
            active_generation=2,
            status_state=ota.STATE_PENDING_CONFIRM,
            status_flags=flags,
        )
        client = ota.OtaClient(
            stale, target=ota.TARGET_RIGHT, transaction_id=12, timeout=0.01
        )
        with self.assertRaisesRegex(ota.OtaToolError, "active generation"):
            client.wait_peer_confirmation(
                expected_generation=3, timeout=0.02, poll_interval=0.001
            )

    def test_confirm_recovery_rejects_logical_idle_maintenance_without_tombstone(self) -> None:
        confirmed = MockTransport(
            active_generation=3,
            status_state=ota.STATE_MAINTENANCE,
            status_flags=HEALTHY_OTA_FLAGS,
        )
        client = ota.OtaClient(
            confirmed, target=ota.TARGET_RIGHT, transaction_id=12, timeout=0.01
        )
        with self.assertRaisesRegex(ota.OtaToolError, "identity/tombstone"):
            client.wait_confirmed_peer(
                expected_generation=3, timeout=0.02, poll_interval=0.001
            )

    def _run_pair_confirm(self, transport: PairConfirmTransport) -> int:
        argv = [
            "pair-confirm", "--port", "COM3", "--allow-confirm",
            "--target-generation", "5", "--peer-confirm-timeout", "0.02",
            "--peer-confirm-poll", "0.001", "--peer-settle", "0",
            "--right-image-sha256", STATUS_IMAGE_SHA.hex(),
            "--left-image-sha256", STATUS_IMAGE_SHA.hex(),
            "--right-manifest-sha256", STATUS_MANIFEST_SHA.hex(),
            "--left-manifest-sha256", STATUS_MANIFEST_SHA.hex(),
            "--timeout", "0.01", "--retries", "0",
            "--transaction-id", "1234",
        ]
        printed = []
        with mock.patch.object(ota, "SerialTransport", return_value=transport), \
                mock.patch.object(ota.time, "sleep"), \
                mock.patch.object(ota, "_print", side_effect=printed.append):
            result = ota.main(argv)
        self.last_pair_result = printed[0] if printed else None
        return result

    def test_pair_confirm_skips_left_confirm_after_lost_ack_idle_rerun(self) -> None:
        # This is the observed recovery state: the first run confirmed both
        # boards, but the host lost the final response before saving the
        # journal.  A retry must be read-only and must not send Confirm to the
        # already-idle left manager.
        transport = PairConfirmTransport(
            right_statuses=[
                (ota.STATE_MAINTENANCE, 0, 5, HEALTHY_OTA_FLAGS),
            ],
            left_statuses=[(ota.STATE_IDLE, 0, 5, 0)],
        )
        self.assertEqual(self._run_pair_confirm(transport), 2)
        self.assertNotIn(ota.CMD_CONFIRM, [frame.command for frame in transport.writes])

    def test_pair_confirm_accepts_exact_completed_tombstones(self) -> None:
        transport = PairConfirmTransport(
            right_statuses=[
                (ota.STATE_CONFIRMED, 5, 5, HEALTHY_OTA_FLAGS),
            ],
            left_statuses=[
                (ota.STATE_CONFIRMED, 5, 5, HEALTHY_OTA_FLAGS),
            ],
        )
        self.assertEqual(self._run_pair_confirm(transport), 0)
        self.assertEqual(self.last_pair_result["confirm_order"], [])
        self.assertEqual(self.last_pair_result["confirm_skipped"], ["right", "left"])
        self.assertNotIn(ota.CMD_CONFIRM, [frame.command for frame in transport.writes])

    def test_pair_confirm_rejects_wrong_left_generation_or_state_before_write(self) -> None:
        wrong_generation = PairConfirmTransport(
            right_statuses=[
                (ota.STATE_CONFIRMED, 5, 5, HEALTHY_OTA_FLAGS),
            ],
            left_statuses=[(ota.STATE_PENDING_CONFIRM, 4, 5, 0)],
        )
        self.assertEqual(self._run_pair_confirm(wrong_generation), 2)
        self.assertNotIn(ota.CMD_CONFIRM,
                          [frame.command for frame in wrong_generation.writes])

        activation_pending = PairConfirmTransport(
            right_statuses=[
                (ota.STATE_MAINTENANCE, 0, 5, HEALTHY_OTA_FLAGS),
            ],
            left_statuses=[(ota.STATE_ACTIVATION_PENDING, 5, 5, 0)],
        )
        self.assertEqual(self._run_pair_confirm(activation_pending), 2)
        self.assertNotIn(ota.CMD_CONFIRM,
                          [frame.command for frame in activation_pending.writes])

    def test_pair_confirm_normal_pending_state_confirms_both_in_order(self) -> None:
        transport = PairConfirmTransport(
            right_statuses=[
                (ota.STATE_PENDING_CONFIRM, 5, 5, 0),
                (ota.STATE_CONFIRMED, 5, 5, HEALTHY_OTA_FLAGS),
            ],
            # Initial preflight, recheck immediately before left.confirm, and
            # the status inside OtaClient.confirm.
            left_statuses=[
                (ota.STATE_PENDING_CONFIRM, 5, 5, 0),
                (ota.STATE_PENDING_CONFIRM, 5, 5, 0),
                (ota.STATE_PENDING_CONFIRM, 5, 5, 0),
            ],
        )
        self.assertEqual(self._run_pair_confirm(transport), 0)
        self.assertEqual(self.last_pair_result["confirm_order"], ["right", "left"])
        self.assertEqual(self.last_pair_result["confirm_skipped"], [])
        confirms = [
            frame.target for frame in transport.writes
            if frame.command == ota.CMD_CONFIRM
        ]
        self.assertEqual(confirms, [ota.TARGET_RIGHT, ota.TARGET_LEFT])

    def test_pair_confirm_reports_mixed_confirmed_and_pending_roles(self) -> None:
        transport = PairConfirmTransport(
            right_statuses=[
                (ota.STATE_CONFIRMED, 5, 5, HEALTHY_OTA_FLAGS),
            ],
            left_statuses=[
                (ota.STATE_PENDING_CONFIRM, 5, 5, 0),
                (ota.STATE_PENDING_CONFIRM, 5, 5, 0),
                (ota.STATE_PENDING_CONFIRM, 5, 5, 0),
            ],
        )
        self.assertEqual(self._run_pair_confirm(transport), 0)
        self.assertEqual(self.last_pair_result["confirm_order"], ["left"])
        self.assertEqual(self.last_pair_result["confirm_skipped"], ["right"])

    def test_pair_parser_has_one_explicit_port(self) -> None:
        parser = ota._parser()
        pair = next(action for action in parser._subparsers._group_actions[0].choices.values()
                     if action.prog.endswith("pair-prepare"))
        names = {option for action in pair._actions for option in action.option_strings}
        self.assertIn("--port", names)
        self.assertNotIn("--left-port", names)
        self.assertNotIn("--right-port", names)

    def test_prepare_failure_attempts_abort_and_status(self) -> None:
        transport = MockTransport(fail_manifest=True)
        client = ota.OtaClient(transport, target=ota.TARGET_LEFT, transaction_id=10, timeout=0.01)
        manifest = ota.sign_manifest(self.manifest(), self.private_path)
        with self.assertRaises(ota.OtaNack):
            client.prepare(manifest.wire + self.image, allow_ota=True)
        commands = [frame.command for frame in transport.writes]
        self.assertIn(ota.CMD_ABORT, commands)
        self.assertIn(ota.CMD_STATUS, commands)

    def test_prepare_failure_never_aborts_ambiguous_activated_state(self) -> None:
        transport = MockTransport(
            fail_manifest=True,
            status_state=ota.STATE_ACTIVATION_PENDING,
            status_flags=ota.STATUS_PENDING_VERIFY,
        )
        client = ota.OtaClient(
            transport, target=ota.TARGET_LEFT, transaction_id=10, timeout=0.01
        )
        manifest = ota.sign_manifest(self.manifest(), self.private_path)
        with self.assertRaises(ota.OtaNack):
            client.prepare(manifest.wire + self.image, allow_ota=True)
        commands = [frame.command for frame in transport.writes]
        self.assertIn(ota.CMD_STATUS, commands)
        self.assertNotIn(ota.CMD_ABORT, commands)

    def test_authorizations_are_independent(self) -> None:
        transport = MockTransport()
        client = ota.OtaClient(transport, target=ota.TARGET_LEFT)
        with self.assertRaises(ota.OtaToolError):
            client.prepare(b"", allow_ota=False)
        with self.assertRaises(ota.OtaToolError):
            client.activate(allow_activate=False)
        with self.assertRaises(ota.OtaToolError):
            client.confirm(allow_confirm=False)
        self.assertEqual(transport.writes, [])

    def test_explicit_package_hash_is_stable(self) -> None:
        manifest = ota.sign_manifest(self.manifest(), self.private_path)
        package = manifest.wire + self.image
        first = ota.package_info(package)["package_sha256"]
        second = ota.package_info(package)["package_sha256"]
        self.assertEqual(first, second)


class StatusV2JsonTest(unittest.TestCase):
    """The JSON projection must not drop decoded board facts.

    A hand-maintained field list in ``_frame_info`` once omitted seven decoded
    fields, so ``update_proxy_pair.py`` rejected every board's status as
    incomplete and no V2 update could get past discovery.  Deriving the keys
    from the dataclass makes that class of drift impossible; this test pins the
    contract to the orchestrator's own requirement list.
    """

    def status(self) -> ota.StatusPayloadV2:
        wire = status_v2_wire(
            role=ota.TARGET_LEFT, state=ota.STATE_IDLE,
            last_command=ota.CMD_HELLO, transaction_id=91,
            target_generation=7, active_generation=7,
            flags=HEALTHY_OTA_FLAGS, completed=True,
        )
        return ota.decode_status_v2(wire)

    def test_projection_covers_every_dataclass_field(self) -> None:
        status = self.status()
        payload = ota.status_v2_json(status)
        for field in ota.dataclass_fields(status):
            self.assertIn(field.name, payload)
        self.assertEqual(payload["version"], 2)

    def test_projection_satisfies_orchestrator_requirements(self) -> None:
        sys.path.insert(0, str(ROOT / "tools"))
        import update_proxy_pair as orchestrator  # noqa: PLC0415

        payload = ota.status_v2_json(self.status())
        missing = [name for name in orchestrator.STATUS_V2_REQUIRED_FIELDS
                   if name not in payload]
        self.assertEqual(missing, [])

    def test_zero_active_generation_is_rejected(self) -> None:
        """A board that answers is running an image, so it knows its own gen.

        The right board's V2 builder omitted active_generation entirely while
        its V1 builder set it, and neither validator noticed.  The host then
        read a healthy gen16 right board as "peer generation 0" and refused
        the rolling-activation check.
        """
        wire = bytearray(status_v2_wire(
            role=ota.TARGET_RIGHT, state=ota.STATE_IDLE,
            last_command=ota.CMD_HELLO, transaction_id=91,
            target_generation=7, active_generation=7,
            flags=HEALTHY_OTA_FLAGS, completed=True,
        ))
        self.assertIsNotNone(ota.decode_status_v2(bytes(wire)))
        struct.pack_into("<I", wire, 32, 0)  # active_generation
        with self.assertRaises(ota.OtaToolError):
            ota.decode_status_v2(bytes(wire))

    def test_byte_facts_are_hex_and_commands_stay_formatted(self) -> None:
        payload = ota.status_v2_json(self.status())
        self.assertEqual(payload["image_sha256"], STATUS_IMAGE_SHA.hex())
        self.assertEqual(payload["app_elf_sha256"], "55" * 32)
        self.assertEqual(payload["previous_image_sha256"], "44" * 32)
        self.assertEqual(payload["last_command"], f"0x{ota.CMD_HELLO:02x}")
        self.assertTrue(payload["local_validated"])


if __name__ == "__main__":
    unittest.main()
