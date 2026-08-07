"""Left-board GO-1 fault-injection contracts (model only).

These tests freeze the recovery decisions required before a single left-only
Prepare/Activate attempt.  They do not flash hardware and do not claim ESP32
power-cut fidelity; they make ACK-loss / power-window / NVS-fail behavior
executable in CI so GO-1 cannot silently regress to safe_abort or fail-open.
"""

from __future__ import annotations

import unittest
from dataclasses import dataclass, replace


VALID = "VALID"
PENDING_VERIFY = "PENDING_VERIFY"
ABORTED = "ABORTED"
PREPARED = "PREPARED"
LOCAL_VALIDATED = "LOCAL_VALIDATED"
MAINTENANCE = "MAINTENANCE"
IDLE = "IDLE"
FAULT = "FAULT"


@dataclass(frozen=True)
class SlotState:
    subtype: int
    image_state: str
    generation: int
    image_sha: str


@dataclass
class LeftBoard:
    running: SlotState
    boot: SlotState
    other: SlotState
    logical: str = IDLE
    local_validated: bool = False
    record_tx: int = 0
    record_phase: str = "none"
    record_image_sha: str = ""
    usb1_data_plane: str = "product"  # product | fail_close
    last_persist_ok: bool = True
    mark_valid_ok: bool | None = None

    def status_fact(self) -> dict[str, object]:
        return {
            "running_gen": self.running.generation,
            "boot_gen": self.boot.generation,
            "running_state": self.running.image_state,
            "boot_state": self.boot.image_state,
            "logical": self.logical,
            "local_validated": self.local_validated,
            "record_tx": self.record_tx,
            "record_phase": self.record_phase,
            "record_image_sha": self.record_image_sha,
            "usb1_data_plane": self.usb1_data_plane,
        }


def baseline_gen5() -> LeftBoard:
    old = SlotState(0, VALID, 5, "sha-gen5")
    empty = SlotState(1, "EMPTY", 0, "")
    return LeftBoard(running=old, boot=old, other=empty)


def host_after_lost_ack(
    board: LeftBoard,
    *,
    command: str,
    expected_tx: int,
    expected_sha: str,
) -> str:
    """Host policy when the write ACK is lost: never safe_abort; reconcile."""
    fact = board.status_fact()
    if command == "prepare":
        if (
            fact["record_tx"] == expected_tx
            and fact["record_phase"] == PREPARED
            and fact["record_image_sha"] == expected_sha
            and fact["boot_gen"] == 5
        ):
            return "advance_prepared"
        return "hold_prepare_intent"
    if command == "activate":
        if (
            fact["local_validated"]
            and fact["running_gen"] == 15
            and fact["boot_gen"] == 15
            and fact["running_state"] == VALID
            and fact["record_image_sha"] == expected_sha
        ):
            return "advance_local_validated"
        if fact["boot_gen"] == 15 and fact["running_state"] == PENDING_VERIFY:
            return "hold_activate_intent_pending_verify"
        return "hold_activate_intent_ambiguous"
    raise AssertionError(f"unknown command {command}")


def apply_prepare(board: LeftBoard, tx: int, image_sha: str, persist_ok: bool) -> LeftBoard:
    next_board = replace(board)
    next_board.usb1_data_plane = "fail_close"
    if not persist_ok:
        next_board.last_persist_ok = False
        next_board.logical = FAULT
        next_board.record_phase = "none"
        return next_board
    next_board.last_persist_ok = True
    next_board.logical = MAINTENANCE
    next_board.record_tx = tx
    next_board.record_phase = PREPARED
    next_board.record_image_sha = image_sha
    # Prepare must not flip boot slot.
    next_board.boot = board.boot
    next_board.other = SlotState(1, PREPARED, 15, image_sha)
    return next_board


def apply_activate_set_boot(board: LeftBoard, persist_ok: bool) -> LeftBoard:
    next_board = replace(board)
    if board.record_phase != PREPARED:
        next_board.logical = FAULT
        return next_board
    if not persist_ok:
        next_board.last_persist_ok = False
        next_board.logical = FAULT
        next_board.usb1_data_plane = "fail_close"
        return next_board
    next_board.boot = SlotState(1, PENDING_VERIFY, 15, board.record_image_sha)
    next_board.logical = MAINTENANCE
    next_board.record_phase = "activate_intent"
    return next_board


def first_boot_pending(board: LeftBoard) -> LeftBoard:
    """Bootloader selected the pending slot; app has not marked valid yet."""
    next_board = replace(board)
    next_board.running = SlotState(1, PENDING_VERIFY, 15, board.record_image_sha)
    next_board.boot = next_board.running
    next_board.other = SlotState(0, VALID, 5, "sha-gen5")
    next_board.local_validated = False
    next_board.logical = MAINTENANCE
    next_board.usb1_data_plane = "fail_close"
    next_board.mark_valid_ok = None
    return next_board


def power_cut_before_mark(board: LeftBoard) -> LeftBoard:
    """Unsigned reset while still PENDING_VERIFY -> next boot ABORTS new slot."""
    next_board = replace(board)
    next_board.running = SlotState(0, VALID, 5, "sha-gen5")
    next_board.boot = next_board.running
    next_board.other = SlotState(1, ABORTED, 15, board.record_image_sha)
    next_board.local_validated = False
    next_board.logical = FAULT
    next_board.usb1_data_plane = "fail_close"
    next_board.mark_valid_ok = False
    return next_board


def mark_valid(board: LeftBoard, persist_ok: bool) -> LeftBoard:
    next_board = replace(board)
    next_board.usb1_data_plane = "fail_close"
    if board.running.image_state != PENDING_VERIFY:
        next_board.logical = FAULT
        next_board.mark_valid_ok = False
        return next_board
    if not persist_ok:
        next_board.last_persist_ok = False
        next_board.logical = FAULT
        next_board.local_validated = False
        next_board.mark_valid_ok = False
        # Image state may still be PENDING_VERIFY; must not claim VALID.
        return next_board
    next_board.running = SlotState(1, VALID, 15, board.record_image_sha)
    next_board.boot = next_board.running
    next_board.local_validated = True
    next_board.logical = MAINTENANCE
    next_board.record_phase = LOCAL_VALIDATED
    next_board.mark_valid_ok = True
    return next_board


def power_cut_after_mark(board: LeftBoard) -> LeftBoard:
    next_board = replace(board)
    # Confirm is intentionally absent for GO-1; VALID must stick.
    next_board.running = SlotState(1, VALID, 15, board.record_image_sha)
    next_board.boot = next_board.running
    next_board.other = SlotState(0, VALID, 5, "sha-gen5")
    next_board.local_validated = True
    next_board.logical = MAINTENANCE
    next_board.usb1_data_plane = "fail_close"
    return next_board


class Go1LeftFaultInjectionTests(unittest.TestCase):
    def test_prepare_ack_loss_reconciles_without_safe_abort(self) -> None:
        board = apply_prepare(baseline_gen5(), tx=42, image_sha="sha-gen15", persist_ok=True)
        decision = host_after_lost_ack(
            board, command="prepare", expected_tx=42, expected_sha="sha-gen15"
        )
        self.assertEqual(decision, "advance_prepared")
        self.assertNotEqual(decision, "safe_abort")
        self.assertEqual(board.boot.generation, 5)
        self.assertEqual(board.usb1_data_plane, "fail_close")

    def test_activate_ack_loss_while_pending_verify_holds_intent(self) -> None:
        board = apply_prepare(baseline_gen5(), tx=7, image_sha="sha-gen15", persist_ok=True)
        board = apply_activate_set_boot(board, persist_ok=True)
        board = first_boot_pending(board)
        decision = host_after_lost_ack(
            board, command="activate", expected_tx=7, expected_sha="sha-gen15"
        )
        self.assertEqual(decision, "hold_activate_intent_pending_verify")
        self.assertNotIn("abort", decision)

    def test_activate_ack_loss_after_local_validated_advances(self) -> None:
        board = apply_prepare(baseline_gen5(), tx=7, image_sha="sha-gen15", persist_ok=True)
        board = apply_activate_set_boot(board, persist_ok=True)
        board = first_boot_pending(board)
        board = mark_valid(board, persist_ok=True)
        decision = host_after_lost_ack(
            board, command="activate", expected_tx=7, expected_sha="sha-gen15"
        )
        self.assertEqual(decision, "advance_local_validated")

    def test_power_cut_before_mark_returns_old_valid_new_aborted(self) -> None:
        board = apply_prepare(baseline_gen5(), tx=9, image_sha="sha-gen15", persist_ok=True)
        board = apply_activate_set_boot(board, persist_ok=True)
        board = first_boot_pending(board)
        board = power_cut_before_mark(board)
        self.assertEqual(board.running.generation, 5)
        self.assertEqual(board.running.image_state, VALID)
        self.assertEqual(board.other.image_state, ABORTED)
        self.assertFalse(board.local_validated)
        self.assertEqual(board.usb1_data_plane, "fail_close")

    def test_power_cut_after_mark_keeps_valid_without_pair_confirm(self) -> None:
        board = apply_prepare(baseline_gen5(), tx=9, image_sha="sha-gen15", persist_ok=True)
        board = apply_activate_set_boot(board, persist_ok=True)
        board = first_boot_pending(board)
        board = mark_valid(board, persist_ok=True)
        board = power_cut_after_mark(board)
        self.assertEqual(board.running.generation, 15)
        self.assertEqual(board.running.image_state, VALID)
        self.assertTrue(board.local_validated)
        self.assertEqual(board.logical, MAINTENANCE)
        self.assertEqual(board.usb1_data_plane, "fail_close")

    def test_prepared_survives_reboot_without_activate(self) -> None:
        board = apply_prepare(baseline_gen5(), tx=3, image_sha="sha-gen15", persist_ok=True)
        rebooted = replace(board)
        # Cold boot without Activate: still old boot slot.
        self.assertEqual(rebooted.boot.generation, 5)
        self.assertEqual(rebooted.record_phase, PREPARED)
        self.assertEqual(rebooted.record_tx, 3)

    def test_nvs_prepare_persist_failure_fail_closes(self) -> None:
        board = apply_prepare(baseline_gen5(), tx=1, image_sha="sha-gen15", persist_ok=False)
        self.assertEqual(board.logical, FAULT)
        self.assertEqual(board.record_phase, "none")
        self.assertEqual(board.boot.generation, 5)
        self.assertEqual(board.usb1_data_plane, "fail_close")
        self.assertFalse(board.local_validated)

    def test_nvs_mark_persist_failure_does_not_claim_valid(self) -> None:
        board = apply_prepare(baseline_gen5(), tx=1, image_sha="sha-gen15", persist_ok=True)
        board = apply_activate_set_boot(board, persist_ok=True)
        board = first_boot_pending(board)
        board = mark_valid(board, persist_ok=False)
        self.assertFalse(board.local_validated)
        self.assertNotEqual(board.running.image_state, VALID)
        self.assertEqual(board.logical, FAULT)
        self.assertEqual(board.usb1_data_plane, "fail_close")
        self.assertIs(board.mark_valid_ok, False)


if __name__ == "__main__":
    unittest.main()
