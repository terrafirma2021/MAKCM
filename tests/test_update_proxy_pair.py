from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import update_proxy_pair as updater  # noqa: E402


def arguments(root: Path, **overrides: object) -> argparse.Namespace:
    values: dict[str, object] = {
        "port": "/dev/ttyACM0",
        "left_package": str(root / "left.mkota"),
        "right_package": str(root / "right.mkota"),
        "public_key": str(root / "public.pem"),
        "public_key_sha256": "a" * 64,
        "signer_key_id": "11" * 16,
        "audit": str(root / "audit.json"),
        "controller_detached": True,
        "request_timeout": 1.0,
        "boot_timeout": 2.0,
        "retries": 1,
        "runtime_unit": "ai-secondary.service",
        "runtime_scope": "system",
    }
    values.update(overrides)
    return argparse.Namespace(**values)


def manifests() -> tuple[updater.mkot.Manifest, updater.mkot.Manifest]:
    def one(role: int) -> updater.mkot.Manifest:
        return updater.mkot.Manifest(
            version=1, role=role, image_kind=1, protocol_version=1, chip_id=9,
            generation=7, min_peer_generation=1, max_peer_generation=9,
            partition_schema=(updater.mkot.LEFT_PARTITION_SCHEMA_V1
                              if role == updater.mkot.TARGET_LEFT
                              else updater.mkot.RIGHT_PARTITION_SCHEMA_V1),
            image_size=4096, chunk_size=4096, signature_algorithm=1,
            hardware_revision=updater.mkot.HARDWARE_REVISION_R1,
            project_name=(updater.mkot.LEFT_PROJECT_NAME
                          if role == updater.mkot.TARGET_LEFT
                          else updater.mkot.RIGHT_PROJECT_NAME),
            image_sha256=b"i" * 32, signer_key_id=bytes.fromhex("11" * 16),
            signature=b"s" * 64,
        )
    return one(updater.mkot.TARGET_LEFT), one(updater.mkot.TARGET_RIGHT)


class FakeSystemd:
    def __init__(self, active: bool = True) -> None:
        self.active = active
        self.actions: list[str] = []

    def unit_active(self, unit: str, scope: str) -> bool:
        return self.active

    def action(self, unit: str, action: str, scope: str) -> None:
        self.actions.append(action)


class UpdateProxyPairTest(unittest.TestCase):
    def test_parser_has_only_single_flow_options(self) -> None:
        actions = {option for action in updater.parser()._actions
                   for option in action.option_strings}
        for removed in ("--journal", "--legacy", "--staging-dir",
                        "--transaction-id", "--allow-ota",
                        "--allow-activate", "--allow-confirm"):
            self.assertNotIn(removed, actions)
        self.assertIn("--controller-detached", actions)

    def test_wrong_package_fails_before_serial_or_runtime(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            systemd = FakeSystemd()
            identity = mock.Mock()
            transport = mock.Mock()
            with mock.patch.object(
                updater, "preflight_packages",
                side_effect=updater.UpdateError("bad package"),
            ):
                with self.assertRaisesRegex(updater.UpdateError, "bad package"):
                    updater.update(
                        arguments(root), systemd=systemd,
                        identity_reader=identity, transport_factory=transport,
                    )
            identity.assert_not_called()
            transport.assert_not_called()
            self.assertEqual(systemd.actions, [])

    def test_systemd_checks_and_actions_have_bounded_timeouts(self) -> None:
        timeout = subprocess.TimeoutExpired(["systemctl"], 30)
        with mock.patch.object(updater.subprocess, "run", side_effect=timeout) as run:
            with self.assertRaisesRegex(updater.UpdateError, "is-active.*timed out"):
                updater.Systemd.unit_active("ai-secondary.service", "system")
            self.assertEqual(run.call_args.kwargs["timeout"],
                             updater.SYSTEMCTL_TIMEOUT_S)

        with mock.patch.object(updater.subprocess, "run", side_effect=timeout) as run:
            with self.assertRaisesRegex(updater.UpdateError, "stop.*timed out"):
                updater.Systemd.action(
                    "ai-secondary.service", "stop", "system"
                )
            self.assertEqual(run.call_args.kwargs["timeout"],
                             updater.SYSTEMCTL_TIMEOUT_S)

    def test_runtime_restarts_only_after_success_and_audit_is_atomic(self) -> None:
        left, right = manifests()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            systemd = FakeSystemd()
            params = arguments(root)
            with mock.patch.object(
                updater, "preflight_packages",
                return_value=(b"left", b"right", left, right),
            ), mock.patch.object(
                updater.mkot, "update_pair",
                return_value={"generation": 7, "right": {}, "left": {}},
            ) as pair:
                result = updater.update(
                    params, systemd=systemd,
                    identity_reader=lambda _path: {
                        "port": "/dev/ttyACM0", "tty": "ttyACM0",
                        "vid": "1a86", "pid": "55d3", "serial": "abc",
                        "bus_path": "1-2",
                    }, transport_factory=mock.Mock(),
                )
            self.assertEqual(systemd.actions, ["stop", "start"])
            pair.assert_called_once()
            audit = json.loads(Path(params.audit).read_text(encoding="utf-8"))
            self.assertEqual(audit, result)
            self.assertEqual(audit["schema"], updater.AUDIT_SCHEMA)

    def test_runtime_stays_stopped_and_no_audit_on_failure(self) -> None:
        left, right = manifests()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            params = arguments(root)
            systemd = FakeSystemd()
            with mock.patch.object(
                updater, "preflight_packages",
                return_value=(b"left", b"right", left, right),
            ), mock.patch.object(
                updater.mkot, "update_pair",
                side_effect=updater.mkot.OtaToolError("wire failed"),
            ):
                with self.assertRaisesRegex(updater.mkot.OtaToolError, "wire"):
                    updater.update(
                        params, systemd=systemd,
                        identity_reader=lambda _path: {},
                        transport_factory=mock.Mock(),
                    )
            self.assertEqual(systemd.actions, ["stop"])
            self.assertFalse(Path(params.audit).exists())

    def test_controller_detached_fails_before_package_read(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with mock.patch.object(updater, "preflight_packages") as preflight:
                with self.assertRaisesRegex(updater.UpdateError,
                                            "controller-detached"):
                    updater.update(arguments(root, controller_detached=False))
            preflight.assert_not_called()

    def test_usb_identity_requires_ch343(self) -> None:
        with mock.patch.object(sys, "platform", "linux"), \
                mock.patch.object(Path, "resolve", return_value=Path("/dev/ttyX")), \
                mock.patch.object(Path, "exists", return_value=False):
            with self.assertRaisesRegex(updater.UpdateError, "sysfs identity"):
                updater.usb_identity(Path("/dev/ttyX"))

    def test_per_port_lock_is_stable_and_port_specific(self) -> None:
        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.dict(os.environ, {"XDG_RUNTIME_DIR": temporary}):
            first = updater._lock_path("/dev/ttyACM0")
            again = updater._lock_path("/dev/ttyACM0")
            other = updater._lock_path("/dev/ttyACM1")
        self.assertEqual(first, again)
        self.assertNotEqual(first, other)
        self.assertTrue(first.name.startswith("makcm-ota-"))

    def test_package_files_are_read_once(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            left_path, right_path, key_path = (
                root / "left", root / "right", root / "key"
            )
            left_path.write_bytes(b"l")
            right_path.write_bytes(b"r")
            key_path.write_bytes(b"key")
            left, right = manifests()
            reads: list[Path] = []
            original = updater._read_once

            def tracked(path: Path, label: str) -> bytes:
                reads.append(path)
                return original(path, label)

            with mock.patch.object(updater, "_read_once", side_effect=tracked), \
                    mock.patch.object(updater, "_validate_package",
                                      side_effect=[left, right]):
                updater.preflight_packages(
                    left_path=left_path, right_path=right_path,
                    public_key=key_path,
                    public_key_sha256=hashlib.sha256(b"key").hexdigest(),
                    signer_key_id="11" * 16,
                )
            self.assertEqual(reads.count(left_path), 1)
            self.assertEqual(reads.count(right_path), 1)


if __name__ == "__main__":
    unittest.main()
