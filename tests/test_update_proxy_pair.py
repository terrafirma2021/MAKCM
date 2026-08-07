from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import update_proxy_pair as updater  # noqa: E402


TEST_IDENTITY = {
    "port": "/dev/ttyACM0", "tty": "ttyACM0",
    "vid": "1a86", "pid": "55d3", "serial": "ch343-test",
    "bus_path": "1-1",
}


def status_v2(role: str, *, state: int, tx: int = 1234, generation: int = 4,
              image_sha256: str | None = None, manifest_sha256: str | None = None,
              active: bool = False, completed: bool = False,
              flags: int = 0) -> dict[str, object]:
    image = image_sha256 or ("c" if role == "left" else "d") * 64
    manifest = manifest_sha256 or ("e" if role == "left" else "f") * 64
    evidence = updater.mkot.STATUS_V2_EVIDENCE_SNAPSHOT_VALID
    if active:
        evidence |= (
            updater.mkot.STATUS_V2_EVIDENCE_ACTIVE
            | updater.mkot.STATUS_V2_EVIDENCE_IDENTITY_VALID
        )
    if completed:
        # Completed/local-validation/pair-commit evidence is deliberately
        # independent from the current state flags.  This lets tests model a
        # malformed or incomplete tombstone (for example a completed record
        # with no local-boot validation) and verify that the host refuses it.
        evidence |= updater.mkot.STATUS_V2_EVIDENCE_COMPLETED
        evidence |= updater.mkot.STATUS_V2_EVIDENCE_IDENTITY_VALID
        if flags & updater.mkot.STATUS_LOCAL_BOOT_VALIDATED:
            evidence |= updater.mkot.STATUS_V2_EVIDENCE_LOCAL_VALIDATED
        if flags & updater.mkot.STATUS_PEER_COMPATIBLE:
            evidence |= updater.mkot.STATUS_V2_EVIDENCE_PAIR_COMMITTED
    return {
        "version": 2,
        "role": updater.mkot.TARGET_LEFT if role == "left" else updater.mkot.TARGET_RIGHT,
        "local_phase": 2 if active else 0,
        "state": state,
        "last_command": updater.mkot.CMD_STATUS,
        "last_response_command": updater.mkot.CMD_STATUS_RESPONSE,
        "last_ota_operation": 0,
        "completed_outcome": 1 if completed else 0,
        "reason": 0,
        "protocol_epoch": 2,
        "status_flags": flags,
        "next_offset": 0,
        "record_transaction_id": tx if active else 0,
        "completed_transaction_id": tx if completed else 0,
        "target_generation": generation if (active or completed) else 0,
        "active_generation": generation,
        "previous_generation": generation - 1,
        "partition_schema": 1,
        "image_size": 400000,
        "record_sequence": 1,
        "last_request_sequence": 2 if active else 0,
        "completed_terminal_sequence": 9 if completed else 0,
        "reset_reason": 1,
        "last_ota_error": 0,
        "last_state_query_error": 0,
        "boot_attempt": 1,
        "evidence_flags": evidence,
        "target_slot": 17 if active else 255,
        "previous_slot": 16,
        "running_slot": 16,
        "boot_slot": 17,
        "image_state": 1,
        "completed_terminal_command": updater.mkot.CMD_CONFIRM if completed else updater.mkot.CMD_HELLO,
        "record_crc32": 0x12345678,
        "image_sha256": image if active else "00" * 32,
        "completed_image_sha256": image if completed else "00" * 32,
        "running_image_sha256": image if (active or completed) else "00" * 32,
        "manifest_sha256": manifest if active else "00" * 32,
        "identity_digest": "33" * 32 if (active or completed) else "00" * 32,
        "previous_image_sha256": "44" * 32 if active else "00" * 32,
        "app_elf_sha256": "55" * 32,
        "hardware_revision": "makcm-s3-r1",
        "project_name": "makcm_left_idf" if role == "left" else "makcm_right_arduino",
        "app_version": f"makcm-g{generation}",
    }


def package(role: str, generation: int = 4, digest: str | None = None) -> updater.PackageRecord:
    return updater.PackageRecord(
        path=f"/{role}.mkota",
        role=role,
        generation=generation,
        min_peer_generation=max(1, generation - 1),
        max_peer_generation=generation,
        package_sha256=digest or (("a" if role == "left" else "b") * 64),
        image_sha256=("c" if role == "left" else "d") * 64,
        image_size=400_000,
        signer_key_id="1" * 32,
        manifest_sha256=("e" if role == "left" else "f") * 64,
        chunk_size=4096,
    )


def args(root: Path, **overrides: object) -> argparse.Namespace:
    values: dict[str, object] = {
        "port": "/dev/ttyACM0",
        "left_package": str(root / "left.mkota"),
        "right_package": str(root / "right.mkota"),
        "public_key": str(root / "production.pub.pem"),
        "public_key_sha256": "",
        "signer_key_id": "1" * 32,
        "journal": str(root / "state.json"),
        "audit": str(root / "audit.json"),
        "runtime_unit": "none",
        "runtime_scope": "system",
        "runtime_settle": 0.0,
        "runtime_stop_timeout": 1.0,
        "transaction_id": 1234,
        "baud": 4_000_000,
        "timeout": 3.0,
        "retries": 5,
        "phase_timeout": 30.0,
        "peer_confirm_timeout": 20.0,
        "peer_confirm_poll": 1.0,
        "peer_settle": 2.0,
        "allow_ota": True,
        "allow_activate": True,
        "allow_confirm": True,
        "controller_detached": True,
    }
    values.update(overrides)
    public = Path(str(values["public_key"]))
    public.parent.mkdir(parents=True, exist_ok=True)
    public.write_text("test public key", encoding="utf-8")
    if not values["public_key_sha256"]:
        values["public_key_sha256"] = updater.sha256_file(public)
    return argparse.Namespace(**values)


_DEFAULT_RIGHT_CURRENT = object()


class FakeRunner:
    def __init__(self, *, current: int = 3, fail: str | None = None,
                 unit_active: bool = False, target: int = 0, state: int = 0,
                 left_flags: int = 0, confirm_tombstone: bool = True,
                  right_current: int | None | object = _DEFAULT_RIGHT_CURRENT,
                  right_target: int = 0,
                 right_state: int = updater.mkot.STATE_MAINTENANCE,
                 right_flags: int = 0, right_error: str | None = None):
        self.current = current
        self.target = target
        self.state = state
        self.left_flags = left_flags
        self.confirm_tombstone = confirm_tombstone
        self.right_current = (
            current if right_current is _DEFAULT_RIGHT_CURRENT else right_current
        )
        self.right_target = right_target
        self.right_state = right_state
        self.right_flags = right_flags
        self.right_error = right_error
        self.fail = fail
        self.unit_active = unit_active
        self.commands: list[list[str]] = []
        self.unit_actions: list[tuple[str, str]] = []

    def run_json(self, command: list[str], timeout: float) -> dict[str, object]:
        del timeout
        self.commands.append(command)
        verb = command[0]
        if verb == self.fail:
            raise updater.UpdateError(f"forced {verb} failure")
        if verb == "status":
            target = command[command.index("--target") + 1]
            tx = int(command[command.index("--transaction-id") + 1])
            if target == "right":
                if self.right_error:
                    raise updater.UpdateError(self.right_error)
                if self.right_current is None:
                    raise updater.UpdateError("right status unavailable")
                right_completed = self.right_state == updater.mkot.STATE_CONFIRMED
                right_active = self.right_target != 0 or self.right_state in (
                    updater.mkot.STATE_PREPARED,
                    updater.mkot.STATE_ACTIVATION_PENDING,
                    updater.mkot.STATE_PENDING_CONFIRM,
                )
                return {
                    "response": {
                        "status": status_v2(
                            "right", state=self.right_state, tx=tx,
                            generation=self.right_target or self.right_current,
                            active=right_active, completed=right_completed,
                            flags=self.right_flags,
                        )
                    }
                }
            left_completed = self.state == updater.mkot.STATE_CONFIRMED
            left_active = self.target != 0 or self.state in (
                updater.mkot.STATE_PREPARED,
                updater.mkot.STATE_ACTIVATION_PENDING,
                updater.mkot.STATE_PENDING_CONFIRM,
            )
            return {
                "response": {
                    "status": status_v2(
                        "left", state=self.state, tx=tx,
                        generation=self.target or self.current,
                        active=left_active, completed=left_completed,
                        flags=self.left_flags,
                    )
                }
            }
        if verb == "pair-prepare":
            tx = int(command[command.index("--transaction-id") + 1])
            self.right_current = self.current
            self.right_target = 4
            self.right_state = updater.mkot.STATE_PREPARED
            self.right_flags = updater.mkot.STATUS_PREPARED
            self.current = self.current
            self.target = 4
            self.state = updater.mkot.STATE_PREPARED
            return {
                "activated": False,
                "prepared_order": ["right", "left"],
                "prepared_facts": {
                    "right": status_v2(
                        "right", state=updater.mkot.STATE_PREPARED, tx=tx,
                        generation=4, active=True,
                        flags=updater.mkot.STATUS_PREPARED,
                    ),
                    "left": status_v2(
                        "left", state=updater.mkot.STATE_PREPARED, tx=tx,
                        generation=4, active=True,
                        flags=updater.mkot.STATUS_PREPARED,
                    ),
                },
            }
        if verb == "pair-activate":
            self.right_current = 4
            self.right_target = 4
            self.right_state = updater.mkot.STATE_PENDING_CONFIRM
            self.right_flags = updater.mkot.STATUS_LOCAL_BOOT_VALIDATED | updater.mkot.STATUS_PEER_COMPATIBLE | updater.mkot.STATUS_HEALTH_PASSED
            self.current = 4
            self.target = 4
            self.state = 5
            return {
                "activation_order": ["right", "left"],
                "peer_confirmation": {"active_generation": 4},
            }
        if verb == "pair-confirm":
            self.current = 4
            if self.confirm_tombstone:
                self.target = 4
                self.state = updater.mkot.STATE_CONFIRMED
                self.left_flags = (
                    updater.mkot.STATUS_LOCAL_BOOT_VALIDATED
                    | updater.mkot.STATUS_PEER_COMPATIBLE
                    | updater.mkot.STATUS_HEALTH_PASSED
                )
            else:
                self.target = 0
                self.state = 0
            self.right_current = 4
            self.right_target = 4 if self.confirm_tombstone else 0
            self.right_state = updater.mkot.STATE_CONFIRMED if self.confirm_tombstone else updater.mkot.STATE_IDLE
            self.right_flags = (
                updater.mkot.STATUS_LOCAL_BOOT_VALIDATED
                | updater.mkot.STATUS_PEER_COMPATIBLE
                | updater.mkot.STATUS_HEALTH_PASSED
                if self.confirm_tombstone else 0
            )
            return {"pair_health_confirmed": True, "active_generation": 4}
        raise AssertionError(verb)

    def unit_state(self, unit: str, scope: str) -> tuple[str, str]:
        del unit, scope
        return "loaded", "active" if self.unit_active else "inactive"

    def systemctl(self, unit: str, action: str, scope: str) -> None:
        self.unit_actions.append((unit, action, scope))
        if action == "stop":
            self.unit_active = False
        elif action == "start":
            self.unit_active = True


class AutoUpdateTests(unittest.TestCase):
    def invoke(self, root: Path, runner: FakeRunner,
               **overrides: object) -> dict[str, object]:
        parameters = args(root, **overrides)
        with mock.patch.object(updater, "verify_package", side_effect=[package("left"), package("right")]), \
             mock.patch.object(updater, "stage_package", side_effect=lambda record, _directory: record), \
             mock.patch.object(updater, "verify_staged_package"), \
             mock.patch.object(updater, "usb_identity", return_value=TEST_IDENTITY.copy()):
            return updater.update(parameters, runner)

    def test_full_update_is_right_first_and_persists_confirmed_audit(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runner = FakeRunner()
            result = self.invoke(root, runner)
            self.assertEqual(
                [command[0] for command in runner.commands],
                [
                    "status", "status", "pair-prepare", "pair-activate",
                    "status", "status", "pair-confirm", "status", "status",
                    "status", "status",
                ],
            )
            self.assertEqual(result["result"], "confirmed")
            journal = json.loads((root / "state.json").read_text(encoding="utf-8"))
            audit = json.loads((root / "audit.json").read_text(encoding="utf-8"))
            self.assertEqual(journal["phase"], "confirmed")
            self.assertEqual(audit["phase"], "confirmed")
            prepare = runner.commands[2]
            self.assertIn("--allow-ota", prepare)
            self.assertIn("--right-package-sha256", prepare)
            self.assertLess(prepare.index("--right-package"), prepare.index("--left-package"))

    def test_full_update_accepts_durable_left_confirmed_tombstone(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runner = FakeRunner(confirm_tombstone=True)
            result = self.invoke(root, runner)
            self.assertEqual(result["result"], "confirmed")
            final = json.loads((root / "state.json").read_text(encoding="utf-8"))
            self.assertEqual(final["phase"], "confirmed")

    def test_post_confirm_tombstone_requires_local_validation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parameters = args(root)
            now = updater.utc_now()
            updater.save_journal(
                Path(parameters.journal),
                updater.Journal(
                    schema=updater.SCHEMA, transaction_id=1234, phase="confirmed",
                    target_generation=4, left=package("left"), right=package("right"),
                    port="/dev/ttyACM0", usb_identity=TEST_IDENTITY.copy(),
                    runtime_unit=None, runtime_scope="system",
                    runtime_was_active=False, created_at=now, updated_at=now,
                    events=[],
                ),
            )
            runner = FakeRunner(
                current=4, target=4, state=updater.mkot.STATE_CONFIRMED,
                left_flags=0,
            )
            with self.assertRaisesRegex(updater.UpdateError, "tombstone|Status V2 facts"):
                self.invoke(root, runner)

    def test_resume_from_prepared_skips_flash_download(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parameters = args(root)
            now = updater.utc_now()
            journal = updater.Journal(
                schema=updater.SCHEMA, transaction_id=1234, phase="prepared",
                target_generation=4, left=package("left"), right=package("right"),
                port="/dev/ttyACM0", usb_identity=TEST_IDENTITY.copy(), runtime_unit=None,
                runtime_scope="system", runtime_was_active=False,
                created_at=now, updated_at=now, events=[],
            )
            updater.save_journal(Path(parameters.journal), journal)
            runner = FakeRunner(
                current=3, target=4, state=updater.mkot.STATE_PREPARED,
                right_current=3, right_target=4,
                right_state=updater.mkot.STATE_PREPARED,
            )
            with mock.patch.object(updater, "verify_package", side_effect=[package("left"), package("right")]), \
                 mock.patch.object(updater, "usb_identity", return_value=TEST_IDENTITY.copy()):
                updater.update(parameters, runner)
            verbs = [command[0] for command in runner.commands]
            self.assertEqual(
                verbs,
                [
                    "status", "status", "pair-activate", "status", "status",
                    "pair-confirm", "status", "status", "status", "status",
                ],
            )
            self.assertNotIn("pair-prepare", verbs)

    def test_device_pending_confirm_advances_stale_prepared_journal(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parameters = args(root)
            now = updater.utc_now()
            journal = updater.Journal(
                schema=updater.SCHEMA, transaction_id=1234, phase="prepared",
                target_generation=4, left=package("left"), right=package("right"),
                port="/dev/ttyACM0", usb_identity=TEST_IDENTITY.copy(), runtime_unit=None,
                runtime_scope="system", runtime_was_active=False,
                created_at=now, updated_at=now, events=[],
            )
            updater.save_journal(Path(parameters.journal), journal)
            runner = FakeRunner(
                current=4, target=4, state=updater.mkot.STATE_PENDING_CONFIRM,
                left_flags=updater.mkot.STATUS_LOCAL_BOOT_VALIDATED,
                right_current=4, right_target=4,
                right_state=updater.mkot.STATE_PENDING_CONFIRM,
                right_flags=(updater.mkot.STATUS_LOCAL_BOOT_VALIDATED |
                             updater.mkot.STATUS_PEER_COMPATIBLE |
                             updater.mkot.STATUS_HEALTH_PASSED),
            )
            with mock.patch.object(
                updater, "verify_package", side_effect=[package("left"), package("right")]
            ), mock.patch.object(updater, "usb_identity", return_value=TEST_IDENTITY.copy()):
                updater.update(parameters, runner)
            self.assertEqual(
                [command[0] for command in runner.commands],
                [
                    "status", "status", "pair-confirm", "status", "status",
                    "status", "status",
                ],
            )
            saved = json.loads(Path(parameters.journal).read_text(encoding="utf-8"))
            self.assertIn("confirmed", [e["event"] for e in saved["events"]])

    def test_unfinished_resume_verifies_staged_paths_not_missing_sources(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parameters = args(
                root,
                left_package=str(root / "deleted-left.mkota"),
                right_package=str(root / "deleted-right.mkota"),
            )
            now = updater.utc_now()
            journal = updater.Journal(
                schema=updater.SCHEMA, transaction_id=1234, phase="prepared",
                target_generation=4,
                left=updater.dataclasses.replace(package("left"), path="/staged/left.mkota"),
                right=updater.dataclasses.replace(package("right"), path="/staged/right.mkota"),
                port="/dev/ttyACM0", usb_identity=TEST_IDENTITY.copy(), runtime_unit=None,
                runtime_scope="system", runtime_was_active=False,
                created_at=now, updated_at=now, events=[],
            )
            updater.save_journal(Path(parameters.journal), journal)
            seen: list[str] = []
            def verify(path: Path, _key: Path, _signer: str) -> updater.PackageRecord:
                seen.append(str(path))
                return journal.left if "left" in str(path) else journal.right
            with mock.patch.object(updater, "verify_package", side_effect=verify), \
                 mock.patch.object(updater, "usb_identity", return_value=TEST_IDENTITY.copy()):
                updater.update(
                    parameters,
                    FakeRunner(
                        current=3, target=4,
                        state=updater.mkot.STATE_PREPARED,
                        right_current=3, right_target=4,
                        right_state=updater.mkot.STATE_PREPARED,
                    ),
                )
            self.assertEqual(
                seen, [str(Path("/staged/left.mkota")), str(Path("/staged/right.mkota"))]
            )

    def test_activate_failure_leaves_prepared_and_never_writes_final_audit(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runner = FakeRunner(fail="pair-activate")
            with self.assertRaisesRegex(updater.UpdateError, "forced pair-activate"):
                self.invoke(root, runner)
            journal = json.loads((root / "state.json").read_text(encoding="utf-8"))
            self.assertEqual(journal["phase"], "prepared")
            self.assertFalse((root / "audit.json").exists())

    def test_already_current_performs_no_write_command(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parameters = args(root)
            now = updater.utc_now()
            journal = updater.Journal(
                schema=updater.SCHEMA, transaction_id=1234, phase="confirmed",
                target_generation=4, left=package("left"), right=package("right"),
                port="/dev/ttyACM0", usb_identity=TEST_IDENTITY.copy(), runtime_unit=None,
                runtime_scope="system", runtime_was_active=False,
                created_at=now, updated_at=now, events=[],
            )
            updater.save_journal(Path(parameters.journal), journal)
            runner = FakeRunner(
                current=4, target=4, state=updater.mkot.STATE_CONFIRMED,
                left_flags=(updater.mkot.STATUS_LOCAL_BOOT_VALIDATED
                            | updater.mkot.STATUS_PEER_COMPATIBLE
                            | updater.mkot.STATUS_HEALTH_PASSED),
                right_current=4, right_target=4,
                right_state=updater.mkot.STATE_CONFIRMED,
                right_flags=(updater.mkot.STATUS_LOCAL_BOOT_VALIDATED
                             | updater.mkot.STATUS_PEER_COMPATIBLE
                             | updater.mkot.STATUS_HEALTH_PASSED),
            )
            result = self.invoke(root, runner)
            self.assertEqual(
                [command[0] for command in runner.commands],
                ["status", "status", "status", "status"],
            )
            self.assertEqual(result["active_generation"], 4)

    def test_already_current_without_pair_journal_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(updater.UpdateError, "generation without"):
                self.invoke(Path(temporary), FakeRunner(current=4))

    def test_prepared_journal_cannot_claim_already_confirmed_pair(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parameters = args(root)
            now = updater.utc_now()
            updater.save_journal(
                Path(parameters.journal),
                updater.Journal(
                    schema=updater.SCHEMA, transaction_id=1234, phase="prepared",
                    target_generation=4, left=package("left"), right=package("right"),
                    port="/dev/ttyACM0", usb_identity=TEST_IDENTITY.copy(), runtime_unit=None,
                    runtime_scope="system", runtime_was_active=False,
                    created_at=now, updated_at=now, events=[],
                ),
            )
            with mock.patch.object(
                updater, "verify_package", side_effect=[package("left"), package("right")]
            ), mock.patch.object(updater, "usb_identity", return_value=TEST_IDENTITY.copy()):
                with self.assertRaisesRegex(updater.UpdateError, "generation without"):
                    updater.update(parameters, FakeRunner(current=4))

    def test_every_unconfirmed_phase_fails_when_only_left_is_current(self) -> None:
        """A left Idle/current report cannot stand in for right-board proof."""
        for phase in ("new", "prepared"):
            with self.subTest(phase=phase), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                parameters = args(root)
                now = updater.utc_now()
                updater.save_journal(
                    Path(parameters.journal),
                    updater.Journal(
                        schema=updater.SCHEMA, transaction_id=1234, phase=phase,
                        target_generation=4, left=package("left"), right=package("right"),
                        port="/dev/ttyACM0", usb_identity=TEST_IDENTITY.copy(),
                        runtime_unit=None, runtime_scope="system",
                        runtime_was_active=False, created_at=now, updated_at=now,
                        events=[],
                    ),
                )
                with self.assertRaisesRegex(updater.UpdateError, "generation without"):
                    self.invoke(root, FakeRunner(current=4))

    def _write_activated_journal(self, root: Path) -> argparse.Namespace:
        parameters = args(root)
        now = updater.utc_now()
        updater.save_journal(
            Path(parameters.journal),
            updater.Journal(
                schema=updater.SCHEMA, transaction_id=1234, phase="activated",
                target_generation=4, left=package("left"), right=package("right"),
                port="/dev/ttyACM0", usb_identity=TEST_IDENTITY.copy(),
                runtime_unit=None, runtime_scope="system", runtime_was_active=False,
                created_at=now, updated_at=now, events=[],
            ),
        )
        return parameters

    def test_activated_ack_loss_reconciles_with_exact_right_health(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._write_activated_journal(root)
            flags = updater.mkot.STATUS_PEER_COMPATIBLE | updater.mkot.STATUS_HEALTH_PASSED
            runner = FakeRunner(
                current=4, target=4, state=updater.mkot.STATE_PENDING_CONFIRM,
                left_flags=updater.mkot.STATUS_LOCAL_BOOT_VALIDATED,
                right_current=4,
                right_target=4,
                right_state=updater.mkot.STATE_PENDING_CONFIRM,
                right_flags=flags | updater.mkot.STATUS_LOCAL_BOOT_VALIDATED,
            )
            result = self.invoke(root, runner)
            self.assertEqual(result["result"], "confirmed")
            self.assertEqual(
                [command[0] for command in runner.commands],
                ["status", "status", "pair-confirm", "status", "status", "status", "status"],
            )
            journal = json.loads((root / "state.json").read_text(encoding="utf-8"))
            self.assertEqual(journal["phase"], "confirmed")

    def test_activated_ack_loss_accepts_exact_right_confirmed_tombstone(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._write_activated_journal(root)
            flags = (
                updater.mkot.STATUS_PEER_COMPATIBLE
                | updater.mkot.STATUS_HEALTH_PASSED
                | updater.mkot.STATUS_LOCAL_BOOT_VALIDATED
            )
            runner = FakeRunner(
                current=4,
                target=4,
                state=updater.mkot.STATE_CONFIRMED,
                left_flags=updater.mkot.STATUS_LOCAL_BOOT_VALIDATED,
                right_current=4,
                right_target=4,
                right_state=updater.mkot.STATE_CONFIRMED,
                right_flags=flags,
            )
            result = self.invoke(root, runner)
            self.assertEqual(result["result"], "confirmed")

    def test_right_completed_left_unsent_confirm_intent_resumes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parameters = self._write_activated_journal(root)
            journal = updater.read_journal(Path(parameters.journal))
            self.assertIsNotNone(journal)
            updater.record_board_intent(
                journal, "right", "confirm", "a" * 64,
                sequence=9, package=journal.right,
            )
            updater.record_board_intent(
                journal, "left", "confirm", "b" * 64,
                sequence=4, package=journal.left,
            )
            updater.save_journal(Path(parameters.journal), journal)
            flags = (
                updater.mkot.STATUS_LOCAL_BOOT_VALIDATED |
                updater.mkot.STATUS_PEER_COMPATIBLE |
                updater.mkot.STATUS_HEALTH_PASSED
            )
            runner = FakeRunner(
                current=4, target=4,
                state=updater.mkot.STATE_PENDING_CONFIRM,
                left_flags=updater.mkot.STATUS_LOCAL_BOOT_VALIDATED,
                right_current=4, right_target=4,
                right_state=updater.mkot.STATE_CONFIRMED,
                right_flags=flags,
            )
            result = self.invoke(root, runner)
            self.assertEqual(result["result"], "confirmed")
            saved = json.loads(Path(parameters.journal).read_text(encoding="utf-8"))
            events = [event["event"] for event in saved["events"]]
            self.assertIn("left_confirm_intent_not_observed", events)
            self.assertIn("right_confirm_already_completed", events)

    def test_left_confirm_equal_sequence_without_tombstone_is_ambiguous(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parameters = self._write_activated_journal(root)
            journal = updater.read_journal(Path(parameters.journal))
            self.assertIsNotNone(journal)
            updater.record_board_intent(
                journal, "right", "confirm", "a" * 64,
                sequence=9, package=journal.right,
            )
            updater.record_board_intent(
                journal, "left", "confirm", "b" * 64,
                sequence=2, package=journal.left,
            )
            updater.save_journal(Path(parameters.journal), journal)
            flags = (
                updater.mkot.STATUS_LOCAL_BOOT_VALIDATED |
                updater.mkot.STATUS_PEER_COMPATIBLE |
                updater.mkot.STATUS_HEALTH_PASSED
            )
            runner = FakeRunner(
                current=4, target=4,
                state=updater.mkot.STATE_PENDING_CONFIRM,
                left_flags=updater.mkot.STATUS_LOCAL_BOOT_VALIDATED,
                right_current=4, right_target=4,
                right_state=updater.mkot.STATE_CONFIRMED,
                right_flags=flags,
            )
            with self.assertRaisesRegex(updater.UpdateError, "left pending confirm"):
                self.invoke(root, runner)

    def test_activated_recovery_right_unknown_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._write_activated_journal(root)
            runner = FakeRunner(
                current=4, target=4, state=updater.mkot.STATE_PENDING_CONFIRM,
                left_flags=updater.mkot.STATUS_LOCAL_BOOT_VALIDATED,
            )
            with self.assertRaisesRegex(updater.UpdateError, "exact right Status V2"):
                self.invoke(root, runner)
            journal = json.loads((root / "state.json").read_text(encoding="utf-8"))
            self.assertEqual(journal["phase"], "activated")

    def test_activated_recovery_rejects_right_generation_or_flags(self) -> None:
        cases = (
            {"right_current": 3, "right_flags": updater.mkot.STATUS_PEER_COMPATIBLE |
             updater.mkot.STATUS_HEALTH_PASSED, "message": "generation"},
            {"right_current": 4, "right_flags": updater.mkot.STATUS_PEER_COMPATIBLE,
             "message": "flags"},
            {"right_current": 4, "right_flags": updater.mkot.STATUS_HEALTH_PASSED,
             "message": "flags"},
        )
        for case in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                self._write_activated_journal(root)
                runner = FakeRunner(
                    current=4, target=4, state=updater.mkot.STATE_PENDING_CONFIRM,
                    left_flags=updater.mkot.STATUS_LOCAL_BOOT_VALIDATED,
                    right_current=case["right_current"],
                    right_target=(4 if case["right_current"] == 4 else 0),
                    right_state=(updater.mkot.STATE_CONFIRMED
                                 if case["right_current"] == 4
                                 else updater.mkot.STATE_MAINTENANCE),
                    right_flags=case["right_flags"],
                    confirm_tombstone=False,
                )
                with self.assertRaisesRegex(
                    updater.UpdateError, "right Status V2|right.*tombstone"
                ):
                    self.invoke(root, runner)
                journal = json.loads((root / "state.json").read_text(encoding="utf-8"))
                self.assertEqual(journal["phase"], "activated")

    def test_confirmed_journal_identity_mismatch_cannot_prove_pair(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parameters = args(root)
            now = updater.utc_now()
            mismatched = TEST_IDENTITY.copy()
            mismatched["serial"] = "different-adapter"
            updater.save_journal(
                Path(parameters.journal),
                updater.Journal(
                    schema=updater.SCHEMA, transaction_id=1234, phase="confirmed",
                    target_generation=4, left=package("left"), right=package("right"),
                    port="/dev/ttyACM0", usb_identity=mismatched,
                    runtime_unit=None, runtime_scope="system",
                    runtime_was_active=False, created_at=now, updated_at=now,
                    events=[],
                ),
            )
            with self.assertRaisesRegex(updater.UpdateError, "identity differs"):
                self.invoke(root, FakeRunner(current=4))

    def test_confirmed_journal_without_complete_identity_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parameters = args(root)
            now = updater.utc_now()
            incomplete = TEST_IDENTITY.copy()
            del incomplete["bus_path"]
            updater.save_journal(
                Path(parameters.journal),
                updater.Journal(
                    schema=updater.SCHEMA, transaction_id=1234, phase="confirmed",
                    target_generation=4, left=package("left"), right=package("right"),
                    port="/dev/ttyACM0", usb_identity=incomplete,
                    runtime_unit=None, runtime_scope="system",
                    runtime_was_active=False, created_at=now, updated_at=now,
                    events=[],
                ),
            )
            with self.assertRaisesRegex(updater.UpdateError, "complete CH343 identity"):
                self.invoke(root, FakeRunner(current=4))

    def test_runtime_is_restarted_only_after_confirm(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runner = FakeRunner(unit_active=True)
            self.invoke(root, runner, runtime_unit="ai-secondary.service")
            self.assertEqual(
                runner.unit_actions,
                [("ai-secondary.service", "stop", "system"),
                 ("ai-secondary.service", "start", "system")],
            )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runner = FakeRunner(unit_active=True, fail="pair-activate")
            with self.assertRaises(updater.UpdateError):
                self.invoke(root, runner, runtime_unit="ai-secondary.service")
            self.assertEqual(
                runner.unit_actions,
                [("ai-secondary.service", "stop", "system")]
            )

    def test_all_independent_authorizations_are_required(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for field in ("allow_ota", "allow_activate", "allow_confirm"):
                parameters = args(root, **{field: False})
                with self.assertRaises(updater.UpdateError):
                    updater.ensure_authorized(parameters)

    def test_detach_is_required_only_when_prepare_is_needed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaisesRegex(updater.UpdateError, "detach"):
                self.invoke(root, FakeRunner(current=3), controller_detached=False)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parameters = args(root)
            now = updater.utc_now()
            updater.save_journal(
                Path(parameters.journal),
                updater.Journal(
                    schema=updater.SCHEMA, transaction_id=1234, phase="confirmed",
                    target_generation=4, left=package("left"), right=package("right"),
                    port="/dev/ttyACM0", usb_identity=TEST_IDENTITY.copy(), runtime_unit=None,
                    runtime_scope="system", runtime_was_active=False,
                    created_at=now, updated_at=now, events=[],
                ),
            )
            result = self.invoke(
                root,
                FakeRunner(
                    current=4, target=4, state=updater.mkot.STATE_CONFIRMED,
                    left_flags=(updater.mkot.STATUS_LOCAL_BOOT_VALIDATED
                                | updater.mkot.STATUS_PEER_COMPATIBLE
                                | updater.mkot.STATUS_HEALTH_PASSED),
                    right_current=4, right_target=4,
                    right_state=updater.mkot.STATE_CONFIRMED,
                    right_flags=(updater.mkot.STATUS_LOCAL_BOOT_VALIDATED
                                 | updater.mkot.STATUS_PEER_COMPATIBLE
                                 | updater.mkot.STATUS_HEALTH_PASSED),
                ),
                controller_detached=False,
            )
            self.assertEqual(result["active_generation"], 4)

    def test_pair_validation_rejects_role_and_generation_mismatch(self) -> None:
        with self.assertRaises(updater.UpdateError):
            updater.validate_pair(package("right"), package("left"))
        with self.assertRaises(updater.UpdateError):
            updater.validate_pair(package("left", 4), package("right", 5))

    def test_confirmed_old_release_is_archived_but_unfinished_is_not_overwritten(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parameters = args(root, transaction_id=5678)
            now = updater.utc_now()
            old = updater.Journal(
                schema=updater.SCHEMA, transaction_id=1234, phase="confirmed",
                target_generation=3, left=package("left", 3, "e" * 64),
                right=package("right", 3, "f" * 64), port="/dev/ttyACM0",
                usb_identity={}, runtime_unit=None, runtime_was_active=False,
                runtime_scope="system",
                created_at=now, updated_at=now, events=[],
            )
            updater.save_journal(Path(parameters.journal), old)
            runner = FakeRunner(current=3)
            with mock.patch.object(updater, "verify_package", side_effect=[package("left"), package("right")]), \
                 mock.patch.object(updater, "stage_package", side_effect=lambda record, _directory: record), \
                 mock.patch.object(updater, "verify_staged_package"), \
                 mock.patch.object(updater, "usb_identity", return_value={}):
                updater.update(parameters, runner)
            self.assertTrue((root / "state.gen3.tx1234.confirmed.json").exists())
            current = json.loads((root / "state.json").read_text(encoding="utf-8"))
            self.assertEqual(current["target_generation"], 4)
            self.assertEqual(current["transaction_id"], 5678)

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parameters = args(root)
            now = updater.utc_now()
            unfinished = updater.Journal(
                schema=updater.SCHEMA, transaction_id=1234, phase="prepared",
                target_generation=3, left=package("left", 3, "e" * 64),
                right=package("right", 3, "f" * 64), port="/dev/ttyACM0",
                usb_identity={}, runtime_unit=None, runtime_was_active=False,
                runtime_scope="system",
                created_at=now, updated_at=now, events=[],
            )
            updater.save_journal(Path(parameters.journal), unfinished)
            with mock.patch.object(updater, "verify_package", side_effect=[package("left"), package("right")]), \
                 mock.patch.object(updater, "usb_identity", return_value={}):
                with self.assertRaisesRegex(updater.UpdateError, "unfinished journal"):
                    updater.update(parameters, FakeRunner(current=3))
            retained = json.loads((root / "state.json").read_text(encoding="utf-8"))
            self.assertEqual(retained["phase"], "prepared")

    def test_source_has_no_private_key_or_rom_flash_interface(self) -> None:
        source = (ROOT / "tools" / "update_proxy_pair.py").read_text(encoding="utf-8")
        self.assertNotIn("--private-key", source)
        self.assertNotIn("esptool.py", source)
        self.assertIn('"pair-prepare"', source)
        self.assertIn('"pair-activate"', source)
        self.assertIn('"pair-confirm"', source)

    def test_transition_rejects_package_that_cannot_run_with_current_peer(self) -> None:
        left = package("left")
        right = package("right")
        updater.validate_transition(left, right, 3)
        incompatible = updater.dataclasses.replace(left, min_peer_generation=4)
        with self.assertRaisesRegex(updater.UpdateError, "current peer generation"):
            updater.validate_transition(incompatible, right, 3)

    def test_staging_pins_verified_bytes_and_detects_mutation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "left.mkota"
            source.write_bytes(b"verified package bytes")
            record = updater.dataclasses.replace(
                package("left"), path=str(source),
                package_sha256=updater.sha256_file(source),
            )
            staged = updater.stage_package(record, root / "private")
            self.assertNotEqual(staged.path, str(source))
            updater.verify_staged_package(staged)
            Path(staged.path).chmod(0o600)
            Path(staged.path).write_bytes(b"changed")
            with self.assertRaisesRegex(updater.UpdateError, "changed"):
                updater.verify_staged_package(staged)

    def test_resume_rejects_runtime_scope_change(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parameters = args(root)
            now = updater.utc_now()
            journal = updater.Journal(
                schema=updater.SCHEMA, transaction_id=1234, phase="prepared",
                target_generation=4, left=package("left"), right=package("right"),
                port="/dev/ttyACM0", usb_identity={}, runtime_unit=None,
                runtime_scope="user", runtime_was_active=False,
                created_at=now, updated_at=now, events=[],
            )
            updater.save_journal(Path(parameters.journal), journal)
            with mock.patch.object(
                updater, "verify_package", side_effect=[package("left"), package("right")]
            ), mock.patch.object(updater, "usb_identity", return_value={}):
                with self.assertRaisesRegex(updater.UpdateError, "runtime service"):
                    updater.update(parameters, FakeRunner())

    def test_resume_rejects_changed_ch343_identity(self) -> None:
        now = updater.utc_now()
        journal = updater.Journal(
            schema=updater.SCHEMA, transaction_id=1234, phase="prepared",
            target_generation=4, left=package("left"), right=package("right"),
            port="/dev/ttyACM0",
            usb_identity={"vid": "1a86", "pid": "55d3", "serial": "original", "bus_path": "1-7"},
            runtime_unit=None, runtime_scope="system", runtime_was_active=False,
            created_at=now, updated_at=now, events=[],
        )
        with self.assertRaisesRegex(updater.UpdateError, "identity differs"):
            updater.assert_same_release(
                journal, package("left"), package("right"), "/dev/ttyACM0",
                None, "system",
                {"vid": "1a86", "pid": "55d3", "serial": "replacement", "bus_path": "1-7"},
            )

    def test_journal_a_b_recovery_chooses_highest_valid_copy(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "state.json"
            now = updater.utc_now()
            journal = updater.Journal(
                schema=updater.SCHEMA, transaction_id=1234, phase="new",
                target_generation=4, left=package("left"), right=package("right"),
                port="/dev/ttyACM0", usb_identity={}, runtime_unit=None,
                runtime_scope="system", runtime_was_active=False,
                created_at=now, updated_at=now, events=[],
            )
            updater.save_journal(path, journal)
            journal.phase = "prepared"
            updater.save_journal(path, journal)
            path.write_text("{corrupt", encoding="utf-8")
            recovered = updater.read_journal(path)
            self.assertIsNotNone(recovered)
            self.assertEqual(recovered.record_seq, journal.record_seq)
            self.assertEqual(recovered.phase, "prepared")

    def test_journal_all_copies_corrupt_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "state.json"
            now = updater.utc_now()
            journal = updater.Journal(
                schema=updater.SCHEMA, transaction_id=1234, phase="new",
                target_generation=4, left=package("left"), right=package("right"),
                port="/dev/ttyACM0", usb_identity={}, runtime_unit=None,
                runtime_scope="system", runtime_was_active=False,
                created_at=now, updated_at=now, events=[],
            )
            updater.save_journal(path, journal)
            for copy in (path, path.with_name(path.name + ".a"),
                         path.with_name(path.name + ".b")):
                copy.write_text("{corrupt", encoding="utf-8")
            with self.assertRaisesRegex(updater.UpdateError, "no valid OTA journal"):
                updater.read_journal(path)

    def test_journal_equal_sequence_divergence_is_split_brain(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "state.json"
            now = updater.utc_now()
            journal = updater.Journal(
                schema=updater.SCHEMA, transaction_id=1234, phase="new",
                target_generation=4, left=package("left"), right=package("right"),
                port="/dev/ttyACM0", usb_identity=TEST_IDENTITY.copy(),
                runtime_unit=None, runtime_scope="system",
                runtime_was_active=False, created_at=now, updated_at=now,
                events=[],
            )
            updater.save_journal(path, journal)
            side_a, side_b = updater._journal_sidecars(path)
            divergent = json.loads(side_a.read_text(encoding="utf-8"))
            divergent["phase"] = "prepared"
            divergent["record_crc32"] = updater._journal_crc(divergent)
            side_b.write_text(
                json.dumps(divergent, sort_keys=True), encoding="utf-8"
            )
            with self.assertRaisesRegex(updater.UpdateError, "split-brain"):
                updater.read_journal(path)

    def test_mixed_legacy_copies_are_preserved(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "state.json"
            confirmed = {
                "schema": updater.LEGACY_SCHEMA, "phase": "confirmed",
                "transaction_id": 1234, "target_generation": 4,
            }
            unfinished = dict(confirmed, phase="prepared")
            side_a, _side_b = updater._journal_sidecars(path)
            path.write_text(json.dumps(confirmed), encoding="utf-8")
            side_a.write_text(json.dumps(unfinished), encoding="utf-8")
            with self.assertRaisesRegex(updater.UpdateError, "unfinished v1"):
                updater.archive_legacy_confirmed(path)
            self.assertTrue(path.exists())
            self.assertTrue(side_a.exists())

    def test_reserved_sequence_intent_is_never_persisted(self) -> None:
        now = updater.utc_now()
        journal = updater.Journal(
            schema=updater.SCHEMA, transaction_id=1234, phase="new",
            target_generation=4, left=package("left"), right=package("right"),
            port="/dev/ttyACM0", usb_identity={}, runtime_unit=None,
            runtime_scope="system", runtime_was_active=False,
            created_at=now, updated_at=now, events=[],
        )
        with self.assertRaisesRegex(updater.UpdateError, "reserved"):
            updater.record_board_intent(
                journal, "left", "prepare", "a" * 64,
                sequence=0xFFFFFFFF, package=journal.left,
            )
        updater.record_board_intent(
            journal, "left", "prepare", "a" * 64,
            sequence=0, package=journal.left,
        )
        self.assertIsNone(journal.boards["left"]["pending_action"]["sequence"])
        with self.assertRaisesRegex(updater.UpdateError, "no durable"):
            updater.reconcile_pending_board(
                journal, "left",
                {"response": {"status": status_v2(
                    "left", state=updater.mkot.STATE_PREPARED,
                    tx=1234, generation=4, active=True,
                    flags=updater.mkot.STATUS_PREPARED,
                )}},
                journal.left, tx=1234, generation=4,
            )


if __name__ == "__main__":
    unittest.main()
