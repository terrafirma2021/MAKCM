from __future__ import annotations

import hashlib
import inspect
from pathlib import Path
import re
import struct
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import makcm_ota_tool as ota  # noqa: E402
import update_proxy_pair as updater  # noqa: E402


HASH_A = bytes.fromhex("11" * 32)
HASH_B = bytes.fromhex("22" * 32)


def intrinsic_image(role: int, generation: int = 7) -> bytes:
    project = (ota.LEFT_PROJECT_NAME if role == ota.TARGET_LEFT
               else ota.RIGHT_PROJECT_NAME)
    schema = (ota.LEFT_PARTITION_SCHEMA_V1 if role == ota.TARGET_LEFT
              else ota.RIGHT_PARTITION_SCHEMA_V1)
    image = bytearray(4096)
    image[:4] = bytes((0xE9, 1, 0x02, 0))
    descriptor = 0x20
    struct.pack_into("<I", image, descriptor, ota.ESP_APP_DESC_MAGIC)

    def put_text(offset: int, capacity: int, value: str) -> None:
        encoded = value.encode("ascii")
        if len(encoded) >= capacity:
            raise AssertionError(value)
        image[offset:offset + len(encoded)] = encoded

    version = ota.LEFT_DESCRIPTOR_VERSION.format(generation)
    put_text(descriptor + 16, 32, version)
    put_text(descriptor + 48, 32, project)
    put_text(descriptor + 112, 32,
             ota.RIGHT_DESCRIPTOR_IDF_VERSION if role == ota.TARGET_RIGHT
             else "v5.5.4")
    image[descriptor + 144:descriptor + 176] = bytes(range(1, 33))

    marker_offset = 0x180
    marker = bytearray(ota.EMBEDDED_IDENTITY_SIZE)
    marker[:8] = ota.EMBEDDED_IDENTITY_MAGIC
    struct.pack_into("<HH", marker, 8, ota.EMBEDDED_IDENTITY_VERSION,
                     ota.EMBEDDED_IDENTITY_SIZE)
    marker[12] = role
    struct.pack_into("<IIH", marker, 16, generation, schema,
                     ota.DEFAULT_PROTOCOL_VERSION)

    def put_marker(offset: int, capacity: int, value: str) -> None:
        encoded = value.encode("ascii")
        if len(encoded) >= capacity:
            raise AssertionError(value)
        marker[offset:offset + len(encoded)] = encoded

    put_marker(28, 16, ota.HARDWARE_REVISION_R1)
    put_marker(44, 32, project)
    put_marker(76, 32, version)
    marker[108:116] = ota.EMBEDDED_IDENTITY_TRAILER
    image[marker_offset:marker_offset + len(marker)] = marker
    return bytes(image)


def package(role: int, generation: int = 7) -> bytes:
    image = intrinsic_image(role, generation)
    value = manifest(role)
    value = ota.Manifest(
        **{
            **value.__dict__,
            "generation": generation,
            "image_size": len(image),
            "image_sha256": hashlib.sha256(image).digest(),
        }
    )
    return value.wire + image


def status(*, role: int = ota.TARGET_RIGHT, state: int = ota.STATE_IDLE,
           tx: int = 0, generation: int = 7, target_generation: int = 0,
           running_hash: bytes = HASH_A, target_hash: bytes = bytes(32),
           manifest_hash: bytes = bytes(32), image_size: int = 0,
           next_offset: int = 0, flags: int | None = None,
           project: str | None = None, schema: int | None = None
           ) -> ota.StatusPayloadV2:
    if flags is None:
        flags = (ota.STATUS_PEER_COMPATIBLE | ota.STATUS_HEALTH_PASSED |
                 ota.STATUS_LOCAL_BOOT_VALIDATED)
    if project is None:
        project = (ota.RIGHT_PROJECT_NAME
                   if role == ota.TARGET_RIGHT else ota.LEFT_PROJECT_NAME)
    if schema is None:
        schema = (ota.RIGHT_PARTITION_SCHEMA_V1
                  if role == ota.TARGET_RIGHT else ota.LEFT_PARTITION_SCHEMA_V1)
    return ota.StatusPayloadV2(
        role=role, state=state, last_command=ota.CMD_STATUS,
        last_response_command=ota.CMD_STATUS_RESPONSE, reason=0,
        status_flags=flags, next_offset=next_offset, transaction_id=tx,
        target_generation=target_generation, running_generation=generation,
        partition_schema=schema, image_size=image_size, reset_reason=1,
        last_ota_error=0, last_state_query_error=0, running_slot=0,
        boot_slot=0, target_slot=(1 if state in (
            ota.STATE_RECEIVING, ota.STATE_PREPARED) else 0xFF), image_state=2,
        target_image_sha256=target_hash,
        running_image_sha256=running_hash,
        manifest_sha256=manifest_hash, hardware_revision=ota.HARDWARE_REVISION_R1,
        project_name=project, app_version="makcm-g7",
    )


def manifest(role: int = ota.TARGET_RIGHT) -> ota.Manifest:
    return ota.Manifest(
        version=1, role=role, image_kind=1, protocol_version=1, chip_id=9,
        generation=7, min_peer_generation=1, max_peer_generation=9,
        partition_schema=(ota.RIGHT_PARTITION_SCHEMA_V1
                          if role == ota.TARGET_RIGHT
                          else ota.LEFT_PARTITION_SCHEMA_V1),
        image_size=4096, chunk_size=4096, signature_algorithm=1,
        hardware_revision=ota.HARDWARE_REVISION_R1,
        project_name=(ota.RIGHT_PROJECT_NAME
                      if role == ota.TARGET_RIGHT else ota.LEFT_PROJECT_NAME),
        image_sha256=HASH_A, signer_key_id=b"k" * 16,
        signature=b"s" * 64,
    )


class ClosedTransport:
    def __init__(self) -> None:
        self.closed = False

    def close(self) -> None:
        self.closed = True


class SerialDouble:
    def __init__(self, **kwargs: object) -> None:
        self.kwargs = kwargs
        self.port = kwargs.get("port")
        self.dtr = self.rts = None
        self.writes: list[bytes] = []
        self.flushed = self.input_reset = self.output_reset = False

    def open(self) -> None: pass
    def close(self) -> None: pass
    def read(self, size: int = 1) -> bytes: return b""

    def write(self, data: bytes) -> int:
        self.writes.append(bytes(data))
        return len(data)

    def flush(self) -> None: self.flushed = True
    def reset_input_buffer(self) -> None: self.input_reset = True
    def reset_output_buffer(self) -> None: self.output_reset = True


class MakcmOtaToolTest(unittest.TestCase):
    def test_frame_matches_cpp_golden_layout_and_crc(self) -> None:
        wire = ota.encode_frame(
            target=ota.TARGET_RIGHT, command=ota.CMD_DATA,
            transaction_id=0x12345678, offset=0x20, sequence=9,
            payload=b"abc", flags=ota.FLAG_FINAL,
        )
        self.assertEqual(wire[:4], b"MKOT")
        self.assertEqual(
            struct.unpack_from("<BBBBIIHHI", wire, 4),
            (1, ota.TARGET_RIGHT, ota.CMD_DATA, ota.FLAG_FINAL,
             0x12345678, 0x20, 3, 0, 9),
        )
        self.assertEqual(struct.unpack_from("<I", wire, 27)[0],
                         ota.crc32(wire[4:27]))
        self.assertEqual(ota.decode_frame(wire).payload, b"abc")

    def test_shared_cpp_python_golden_frame(self) -> None:
        contract = (ROOT / "tests" / "ota_protocol_golden.inc").read_text(
            encoding="ascii"
        )
        match = re.search(
            r'MAKCM_OTA_GOLDEN_FRAME_HEX\s*\\\s*"([0-9a-f]+)"', contract
        )
        self.assertIsNotNone(match)
        expected = bytes.fromhex(match.group(1))
        actual = ota.encode_frame(
            target=ota.TARGET_RIGHT, command=ota.CMD_DATA,
            flags=ota.FLAG_RETRANSMIT, transaction_id=0x12345678,
            offset=0x10203040, sequence=0x01020304,
            payload=bytes((0x00, 0x01, 0x7F, 0x80, 0xFF)),
        )
        self.assertEqual(actual, expected)

    def test_frame_flags_reject_invalid_command_combinations(self) -> None:
        invalid = (
            (ota.CMD_HELLO, ota.FLAG_FINAL),
            (ota.CMD_HELLO, ota.FLAG_RESPONSE),
            (ota.CMD_DATA, ota.FLAG_STATUS_V2),
            (ota.CMD_STATUS, 0),
            (ota.CMD_STATUS, ota.FLAG_STATUS_V2 | ota.FLAG_FINAL),
            (ota.CMD_ACK, 0),
            (ota.CMD_ACK, ota.FLAG_RESPONSE | ota.FLAG_RETRANSMIT),
            (ota.CMD_NACK, ota.FLAG_RESPONSE | ota.FLAG_STATUS_V2),
            (ota.CMD_STATUS_RESPONSE, ota.FLAG_RESPONSE),
            (ota.CMD_STATUS_RESPONSE,
             ota.FLAG_RESPONSE | ota.FLAG_STATUS_V2 | ota.FLAG_RETRANSMIT),
        )
        for command, flags in invalid:
            with self.subTest(command=command, flags=flags), \
                    self.assertRaises(ota.OtaToolError):
                ota.encode_frame(
                    target=ota.TARGET_RIGHT, command=command,
                    transaction_id=1, sequence=1, flags=flags,
                )

    def test_decode_rejects_crc_valid_invalid_flag_combinations(self) -> None:
        valid = ota.encode_frame(
            target=ota.TARGET_RIGHT, command=ota.CMD_DATA,
            transaction_id=1, sequence=1,
        )
        invalid = (
            (ota.CMD_HELLO, ota.FLAG_FINAL),
            (ota.CMD_STATUS, 0),
            (ota.CMD_ACK, ota.FLAG_RESPONSE | ota.FLAG_RETRANSMIT),
            (ota.CMD_STATUS_RESPONSE, ota.FLAG_RESPONSE),
        )
        for command, flags in invalid:
            with self.subTest(command=command, flags=flags):
                wire = bytearray(valid)
                wire[6] = command
                wire[7] = flags
                struct.pack_into("<I", wire, len(wire) - ota.CRC_SIZE,
                                 ota.crc32(wire[4:-ota.CRC_SIZE]))
                with self.assertRaises(ota.OtaToolError):
                    ota.decode_frame(bytes(wire))

    def test_explicit_serial_switches_115200_to_4m_without_control_lines(self) -> None:
        instances: list[SerialDouble] = []
        serial_module = mock.Mock()

        def factory(**kwargs: object) -> SerialDouble:
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
        self.assertEqual(instances[1].writes, [ota.SERIAL_FRAME_BOUNDARY])
        self.assertIs(instances[0].dtr, False)
        self.assertIs(instances[0].rts, False)

    def test_status_v2_is_strict_fixed_232_byte_wire(self) -> None:
        value = status()
        wire = ota.encode_status_v2(value)
        self.assertEqual(len(wire), 232)
        self.assertEqual(ota.decode_status_v2(wire), value)
        for malformed in (wire[:-1], wire + b"\0"):
            with self.assertRaisesRegex(ota.OtaToolError, "exactly 232"):
                ota.decode_status_v2(malformed)
        reserved = bytearray(wire)
        reserved[10] = 1
        with self.assertRaisesRegex(ota.OtaToolError, "reserved"):
                ota.decode_status_v2(bytes(reserved))

    def test_command_response_ack_and_nack_goldens_are_not_status_v2(self) -> None:
        ack = bytes.fromhex(
            "0102050000000000403020100e0000000d00000002000000"
        )
        nack = bytes.fromhex(
            "01010a000e00000044332211090000000800000008000000"
        )
        self.assertEqual(len(ack), ota.COMMAND_RESPONSE_SIZE)
        self.assertEqual(
            ota.decode_command_response_payload(ack).last_command,
            ota.CMD_PREPARE,
        )
        decoded_nack = ota.decode_command_response_payload(nack)
        self.assertEqual(decoded_nack.last_command, ota.CMD_EXIT_MAINTENANCE)
        self.assertEqual(decoded_nack.reason, 14)
        with self.assertRaisesRegex(ota.OtaToolError, "exactly 232"):
            ota.decode_status_v2(ack)

        with self.assertRaisesRegex(ota.OtaToolError, "reason disagree"):
            ota._command_response_semantics(
                ota.CMD_NACK, ota.decode_command_response_payload(ack)
            )
        with self.assertRaisesRegex(ota.OtaToolError, "reason disagree"):
            ota._command_response_semantics(
                ota.CMD_ACK, ota.decode_command_response_payload(nack)
            )

    def test_manifest_ecdsa_signature_round_trip(self) -> None:
        if ota.ec is None:
            self.skipTest("cryptography is unavailable")
        from cryptography.hazmat.primitives import serialization

        key = ota.ec.generate_private_key(ota.ec.SECP256R1())
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            private_path = root / "private.pem"
            public_path = root / "public.pem"
            private_path.write_bytes(key.private_bytes(
                serialization.Encoding.PEM,
                serialization.PrivateFormat.PKCS8,
                serialization.NoEncryption(),
            ))
            public_path.write_bytes(key.public_key().public_bytes(
                serialization.Encoding.PEM,
                serialization.PublicFormat.SubjectPublicKeyInfo,
            ))
            signed = ota.sign_manifest(manifest(), private_path)
            self.assertTrue(ota.verify_manifest_signature(
                signed, public_path,
                expected_signer_key_id=signed.signer_key_id,
            ))
            self.assertEqual(len(signed.signature), 64)

    def test_status_v2_rejects_old_snapshot_fields(self) -> None:
        names = {field.name for field in ota.dataclass_fields(ota.StatusPayloadV2)}
        self.assertNotIn("record_transaction_id", names)
        self.assertNotIn("completed_transaction_id", names)
        self.assertNotIn("evidence_flags", names)
        self.assertEqual(ota.STATUS_V2_SIZE, 232)

    def test_library_has_no_authorization_or_resume_api(self) -> None:
        for method in (ota.OtaClient.prepare, ota.OtaClient.activate,
                       ota.OtaClient.confirm, ota.OtaClient.abort):
            self.assertFalse(any(
                name.startswith("allow")
                for name in inspect.signature(method).parameters
            ))
        self.assertFalse(hasattr(ota, "status_v2_matches_active"))
        self.assertFalse(hasattr(ota, "status_v2_matches_completed"))
        commands = ota._parser()._subparsers._group_actions[0].choices
        self.assertEqual(set(commands), {"package", "inspect", "verify",
                                         "anchor", "status"})

    def test_right_gate_completes_before_left_and_progress_is_fixed(self) -> None:
        right_package = package(ota.TARGET_RIGHT)
        left_package = package(ota.TARGET_LEFT)
        events: list[str] = []
        transport = ClosedTransport()
        with mock.patch.object(
            ota, "_update_right",
            side_effect=lambda *args, **kwargs: (
                events.append("right-done") or ("updated", status())
            ),
        ), mock.patch.object(
            ota, "_update_left",
            side_effect=lambda *args, **kwargs: (
                events.append("left-start") or
                (transport, "updated", status(role=ota.TARGET_LEFT))
            ),
        ):
            result = ota.update_pair(
                transport_factory=lambda: transport, right_package=right_package,
                left_package=left_package, request_timeout=1, retries=1,
                boot_timeout=1, progress=events.append,
            )
        self.assertLess(events.index("right-done"), events.index("left-start"))
        self.assertEqual([item for item in events if item in (
            "preflight", "right", "left", "complete")],
            ["preflight", "right", "left", "complete"])
        self.assertEqual(result["generation"], 7)

    def test_package_bytes_accepts_real_intrinsic_identity_for_numeric_roles(self) -> None:
        for role in (ota.TARGET_LEFT, ota.TARGET_RIGHT):
            with self.subTest(role=role):
                decoded, image_hash, _ = ota._package_bytes(package(role), role)
                self.assertEqual(decoded.role, role)
                self.assertEqual(image_hash, hashlib.sha256(
                    intrinsic_image(role)
                ).digest())

    def test_intrinsic_identity_accepts_role_names_and_rejects_invalid_roles(self) -> None:
        for name, role in (("left", ota.TARGET_LEFT),
                           ("right", ota.TARGET_RIGHT)):
            with self.subTest(role=name):
                ota.validate_intrinsic_identity(
                    intrinsic_image(role), role=name, generation=7,
                )
        for invalid in (0, 3, True, "host"):
            with self.subTest(invalid=invalid), self.assertRaisesRegex(
                ota.OtaToolError, r"role must be (?:exactly )?left or right"
            ):
                ota.validate_intrinsic_identity(
                    b"not an image", role=invalid, generation=7,
                )

    def test_proxy_preflight_accepts_real_intrinsic_identities(self) -> None:
        if ota.ec is None:
            self.skipTest("cryptography is unavailable")
        from cryptography.hazmat.primitives import serialization

        key = ota.ec.generate_private_key(ota.ec.SECP256R1())
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            private_path = root / "private.pem"
            public_path = root / "public.pem"
            private_path.write_bytes(key.private_bytes(
                serialization.Encoding.PEM,
                serialization.PrivateFormat.PKCS8,
                serialization.NoEncryption(),
            ))
            public_bytes = key.public_key().public_bytes(
                serialization.Encoding.PEM,
                serialization.PublicFormat.SubjectPublicKeyInfo,
            )
            public_path.write_bytes(public_bytes)
            package_paths: dict[int, Path] = {}
            signer_id: str | None = None
            for role, name in ((ota.TARGET_LEFT, "left.mkota"),
                               (ota.TARGET_RIGHT, "right.mkota")):
                unsigned_package = package(role)
                signed = ota.sign_manifest(
                    ota.decode_manifest(unsigned_package[:ota.MANIFEST_SIZE]),
                    private_path,
                )
                signer_id = signed.signer_key_id.hex()
                package_paths[role] = root / name
                package_paths[role].write_bytes(
                    signed.wire + unsigned_package[ota.MANIFEST_SIZE:]
                )

            self.assertIsNotNone(signer_id)
            _, _, left, right = updater.preflight_packages(
                left_path=package_paths[ota.TARGET_LEFT],
                right_path=package_paths[ota.TARGET_RIGHT],
                public_key=public_path,
                public_key_sha256=hashlib.sha256(public_bytes).hexdigest(),
                signer_key_id=signer_id,
            )
        self.assertEqual(left.role, ota.TARGET_LEFT)
        self.assertEqual(right.role, ota.TARGET_RIGHT)

    def test_fresh_updates_wait_for_confirm_readiness_on_both_boards(self) -> None:
        for role in (ota.TARGET_RIGHT, ota.TARGET_LEFT):
            with self.subTest(role=role):
                board_manifest = manifest(role)
                client = mock.Mock(transaction_id=41)
                preflight_status = status(role=role)
                prepared = status(
                    role=role, state=ota.STATE_PREPARED, tx=41,
                    target_generation=7, target_hash=HASH_A,
                    manifest_hash=ota._sha256(board_manifest.wire),
                    image_size=4096, next_offset=4096,
                )
                client.status.return_value = prepared
                unready = status(role=role, state=ota.STATE_PENDING_CONFIRM,
                                 flags=0)
                ready = status(role=role, state=ota.STATE_PENDING_CONFIRM)
                idle = status(role=role)
                client.status.side_effect = [prepared, unready, ready, idle]

                with mock.patch.object(
                    ota, "_preflight_board",
                    return_value=(client, "update", preflight_status),
                ), mock.patch.object(ota.time, "sleep"):
                    if role == ota.TARGET_RIGHT:
                        ota._update_right(
                            ClosedTransport(), b"package", board_manifest,
                            request_timeout=1, retries=0, boot_timeout=1,
                        )
                    else:
                        first, reopened = ClosedTransport(), ClosedTransport()
                        with mock.patch.object(ota, "OtaClient", return_value=client):
                            ota._update_left(
                                first, lambda: reopened, b"package", board_manifest,
                                request_timeout=1, retries=0, boot_timeout=1,
                            )
                client.confirm.assert_called_once_with()

    def test_reruns_wait_for_confirm_readiness_on_both_boards(self) -> None:
        for role in (ota.TARGET_RIGHT, ota.TARGET_LEFT):
            with self.subTest(role=role):
                board_manifest = manifest(role)
                client = mock.Mock()
                unready = status(role=role, state=ota.STATE_PENDING_CONFIRM,
                                 flags=0)
                ready = status(role=role, state=ota.STATE_PENDING_CONFIRM)
                idle = status(role=role)
                client.status.side_effect = [unready, ready, idle]

                with mock.patch.object(ota, "OtaClient", return_value=client), \
                        mock.patch.object(ota.time, "sleep"):
                    _, action, final = ota._preflight_board(
                        ClosedTransport(), target=role, manifest=board_manifest,
                        request_timeout=1, retries=0, boot_timeout=1,
                    )
                self.assertEqual(action, "confirmed")
                self.assertEqual(final, idle)
                client.confirm.assert_called_once_with()

    def test_left_activate_closes_and_reopens_transport(self) -> None:
        first, second = ClosedTransport(), ClosedTransport()
        clients: list[mock.Mock] = []

        def make_client(*args: object, **kwargs: object) -> mock.Mock:
            client = mock.Mock()
            client.transaction_id = 41
            client.status.side_effect = [
                status(role=ota.TARGET_LEFT,
                       state=ota.STATE_PENDING_CONFIRM),
                status(role=ota.TARGET_LEFT),
            ]
            clients.append(client)
            return client

        preflight_client = mock.Mock(transaction_id=41)
        with mock.patch.object(
            ota, "_preflight_board", return_value=(preflight_client, "update",
                                                    status(role=ota.TARGET_LEFT)),
        ), mock.patch.object(ota, "OtaClient", side_effect=make_client), \
                mock.patch.object(ota, "_prepared_exact", return_value=True), \
                mock.patch.object(ota.time, "sleep"):
            reopened, action, _ = ota._update_left(
                first, lambda: second, b"package", manifest(ota.TARGET_LEFT),
                request_timeout=1, retries=0, boot_timeout=1,
            )
        self.assertTrue(first.closed)
        self.assertIs(reopened, second)
        self.assertEqual(action, "updated")
        preflight_client.activate.assert_called_once_with()

    def test_rerun_uses_idle_pending_and_transient_board_facts(self) -> None:
        board_manifest = manifest()
        idle_client = mock.Mock()
        idle_client.status.return_value = status()
        with mock.patch.object(ota, "OtaClient", return_value=idle_client):
            _, action, _ = ota._preflight_board(
                ClosedTransport(), target=ota.TARGET_RIGHT,
                manifest=board_manifest, request_timeout=1, retries=0,
                boot_timeout=1,
            )
        self.assertEqual(action, "skipped")
        idle_client.exit_maintenance.assert_called_once_with()
        idle_client.abort.assert_not_called()

        pending_client = mock.Mock()
        pending_client.status.side_effect = [
            status(state=ota.STATE_PENDING_CONFIRM), status()
        ]
        with mock.patch.object(ota, "OtaClient", return_value=pending_client), \
                mock.patch.object(ota.time, "sleep"):
            _, action, _ = ota._preflight_board(
                ClosedTransport(), target=ota.TARGET_RIGHT,
                manifest=board_manifest, request_timeout=1, retries=0,
                boot_timeout=1,
            )
        self.assertEqual(action, "confirmed")
        pending_client.confirm.assert_called_once_with()
        pending_client.exit_maintenance.assert_called_once_with()
        pending_client.abort.assert_not_called()

        status_client, abort_client, fresh_client = mock.Mock(), mock.Mock(), mock.Mock()
        status_client.status.return_value = status(
            state=ota.STATE_RECEIVING, tx=99, target_generation=7,
            target_hash=HASH_A, manifest_hash=HASH_B, image_size=4096,
        )
        with mock.patch.object(
            ota, "OtaClient", side_effect=[status_client, abort_client, fresh_client]
        ) as constructor:
            selected, action, _ = ota._preflight_board(
                ClosedTransport(), target=ota.TARGET_RIGHT,
                manifest=board_manifest, request_timeout=1, retries=0,
                boot_timeout=1,
            )
        self.assertIs(selected, fresh_client)
        self.assertEqual(action, "update")
        self.assertEqual(constructor.call_args_list[1].kwargs["transaction_id"], 99)
        abort_client.abort.assert_called_once_with()

    def test_confirm_ack_loss_is_reconciled_by_idle_fact(self) -> None:
        client = mock.Mock(transaction_id=77)
        client.confirm.side_effect = ota.OtaToolError("lost ACK")
        prepared = status(state=ota.STATE_PREPARED, tx=77,
                          target_generation=7, target_hash=HASH_A,
                          manifest_hash=HASH_B, image_size=4096,
                          next_offset=4096)
        client.status.side_effect = [prepared,
                                     status(state=ota.STATE_PENDING_CONFIRM),
                                     status()]
        with mock.patch.object(
            ota, "_preflight_board", return_value=(client, "update", status())
        ), mock.patch.object(ota, "_prepared_exact", return_value=True), \
                mock.patch.object(ota.time, "sleep"):
            action, final = ota._update_right(
                ClosedTransport(), b"package", manifest(), request_timeout=1,
                retries=0, boot_timeout=1,
            )
        self.assertEqual(action, "updated")
        self.assertEqual(final.state, ota.STATE_IDLE)
        client.exit_maintenance.assert_called_once_with()
        client.abort.assert_not_called()

    def test_activate_ack_loss_continues_from_observed_board_facts(self) -> None:
        board_manifest = manifest()
        client = mock.Mock(transaction_id=77)
        client.activate.side_effect = ota.OtaToolError("lost ACTIVATE ACK")
        client.status.side_effect = [
            status(state=ota.STATE_PREPARED, tx=77,
                   target_generation=7, target_hash=HASH_A,
                   manifest_hash=ota._sha256(board_manifest.wire),
                   image_size=4096, next_offset=4096),
            status(state=ota.STATE_PENDING_CONFIRM),
            status(),
        ]
        with mock.patch.object(
            ota, "_preflight_board", return_value=(client, "update", status())
        ), mock.patch.object(ota.time, "sleep"):
            action, final = ota._update_right(
                ClosedTransport(), b"package", board_manifest,
                request_timeout=1, retries=0, boot_timeout=1,
            )
        self.assertEqual(action, "updated")
        self.assertEqual(final.state, ota.STATE_IDLE)
        client.confirm.assert_called_once_with()
        client.exit_maintenance.assert_called_once_with()

    def test_left_activate_ack_loss_reopens_and_uses_status_facts(self) -> None:
        board_manifest = manifest(ota.TARGET_LEFT)
        first, reopened = ClosedTransport(), ClosedTransport()
        activating = mock.Mock(transaction_id=77)
        activating.activate.side_effect = ota.OtaToolError("lost ACTIVATE ACK")
        activating.status.return_value = status(
            role=ota.TARGET_LEFT, state=ota.STATE_PREPARED, tx=77,
            target_generation=7, target_hash=HASH_A,
            manifest_hash=ota._sha256(board_manifest.wire),
            image_size=4096, next_offset=4096,
        )
        after_reboot = mock.Mock()
        after_reboot.status.side_effect = [
            status(role=ota.TARGET_LEFT, state=ota.STATE_PENDING_CONFIRM),
            status(role=ota.TARGET_LEFT),
        ]
        with mock.patch.object(
            ota, "_preflight_board",
            return_value=(activating, "update", status(role=ota.TARGET_LEFT)),
        ), mock.patch.object(ota, "OtaClient", return_value=after_reboot), \
                mock.patch.object(ota.time, "sleep"):
            transport, action, final = ota._update_left(
                first, lambda: reopened, b"package", board_manifest,
                request_timeout=1, retries=0, boot_timeout=1,
            )
        self.assertTrue(first.closed)
        self.assertIs(transport, reopened)
        self.assertEqual(action, "updated")
        self.assertEqual(final.state, ota.STATE_IDLE)
        after_reboot.confirm.assert_called_once_with()

    def test_confirm_request_loss_retries_from_pending_confirm_fact(self) -> None:
        client = mock.Mock(transaction_id=77)
        pending = status(state=ota.STATE_PENDING_CONFIRM)
        healthy = status()
        client.status.side_effect = [pending, pending, pending, healthy]

        with mock.patch.object(
            ota, "_preflight_board", return_value=(client, "update", status())
        ), mock.patch.object(ota, "_prepared_exact", return_value=True):
            action, final = ota._update_right(
                ClosedTransport(), b"package", manifest(), request_timeout=1,
                retries=0, boot_timeout=1,
            )

        self.assertEqual(action, "updated")
        self.assertEqual(final, healthy)
        self.assertEqual(client.confirm.call_count, 2)
        client.exit_maintenance.assert_called_once_with()

    def test_confirm_ack_loss_converges_when_status_is_idle(self) -> None:
        client = mock.Mock()
        client.confirm.side_effect = ota.OtaToolError("lost ACK")
        pending = status(state=ota.STATE_PENDING_CONFIRM)
        healthy = status()
        client.status.side_effect = [pending, healthy]

        with mock.patch.object(ota, "OtaClient", return_value=client), \
                mock.patch.object(ota.time, "sleep"):
            _, action, final = ota._preflight_board(
                ClosedTransport(), target=ota.TARGET_RIGHT,
                manifest=manifest(), request_timeout=1, retries=0,
                boot_timeout=1,
            )

        self.assertEqual(action, "confirmed")
        self.assertEqual(final, healthy)
        client.confirm.assert_called_once_with()
        client.exit_maintenance.assert_called_once_with()

    def test_status_command_exits_right_without_aborting_any_observed_state(self) -> None:
        for state_value in (ota.STATE_RECEIVING, ota.STATE_PREPARED,
                            ota.STATE_PENDING_CONFIRM):
            with self.subTest(state=state_value):
                client = mock.Mock()
                observed = status(
                    state=state_value,
                    tx=17 if state_value != ota.STATE_PENDING_CONFIRM else 0,
                    target_generation=(7 if state_value !=
                                       ota.STATE_PENDING_CONFIRM else 0),
                    target_hash=(HASH_A if state_value !=
                                 ota.STATE_PENDING_CONFIRM else bytes(32)),
                    manifest_hash=(HASH_B if state_value !=
                                   ota.STATE_PENDING_CONFIRM else bytes(32)),
                    image_size=(4096 if state_value !=
                                ota.STATE_PENDING_CONFIRM else 0),
                    next_offset=(4096 if state_value == ota.STATE_PREPARED else 0),
                )
                client.status.return_value = observed
                with mock.patch.object(ota, "_open_client", return_value=client), \
                        mock.patch.object(ota, "_print"):
                    self.assertEqual(
                        ota.main(["status", "--port", "COM1", "--target", "right"]),
                        0,
                    )
                client.exit_maintenance.assert_called_once_with()
                client.abort.assert_not_called()
                self.assertEqual(observed.state, state_value)

    def test_status_command_on_left_has_no_terminal_command(self) -> None:
        client = mock.Mock()
        client.status.return_value = status(role=ota.TARGET_LEFT)
        with mock.patch.object(ota, "_open_client", return_value=client), \
                mock.patch.object(ota, "_print"):
            self.assertEqual(
                ota.main(["status", "--port", "COM1", "--target", "left"]), 0
            )
        client.abort.assert_not_called()
        client.exit_maintenance.assert_not_called()

    def test_controller_detached_fails_before_transport(self) -> None:
        factory = mock.Mock()
        with self.assertRaisesRegex(ota.OtaToolError, "controller_detached"):
            ota.update_pair(
                transport_factory=factory, right_package=b"x",
                left_package=b"y", request_timeout=1, retries=0,
                boot_timeout=1, progress=None, controller_detached=False,
            )
        factory.assert_not_called()


if __name__ == "__main__":
    unittest.main()
