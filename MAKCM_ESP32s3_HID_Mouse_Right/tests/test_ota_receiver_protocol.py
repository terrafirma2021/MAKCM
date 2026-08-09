"""Executable model of the right-board four-state MKOT contract."""

from __future__ import annotations

import binascii
import hashlib
import struct
import unittest


MAGIC = b"MKOT"
VERSION = 1
HEADER = 24
MIN_CHUNK = 2048
MAX_CHUNK = 4096
RIGHT = 2


def crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def frame(command: int, txid: int, *, offset: int = 0, sequence: int = 1,
          payload: bytes = b"", flags: int = 0) -> bytes:
    header = struct.pack("<4sBBBBIIHHI", MAGIC, VERSION, RIGHT, command,
                         flags, txid, offset, len(payload), 0, sequence)
    return header + payload + struct.pack("<I", crc32(header[4:] + payload))


class ReceiverModel:
    IDLE, RECEIVING, PREPARED, PENDING_CONFIRM = range(4)

    def __init__(self, *, pending=False, rollback=True):
        self.state = self.PENDING_CONFIRM if pending else self.IDLE
        self.rollback = rollback
        self.maintenance = False
        self.session_tx = 0
        self.image_tx = 0
        self.image = bytearray()
        self.next_offset = 0
        self.expected_sha = b""
        self.local_health = False
        self.peer_health = False
        self.peer_generation = 0
        self.restarted = False
        self.rolled_back = False

    def enter(self, txid: int, peer_generation: int = 19) -> bool:
        if not txid:
            return False
        self.maintenance = True
        self.session_tx = txid
        self.peer_generation = peer_generation
        return True

    def manifest(self, txid: int, image_size: int, digest: bytes) -> str:
        if not self.maintenance or self.state != self.IDLE:
            return "invalid_state"
        if txid != self.session_tx:
            return "wrong_transaction"
        self.image = bytearray(image_size)
        self.image_tx = txid
        self.expected_sha = digest
        self.next_offset = 0
        self.state = self.RECEIVING
        return "ok"

    def data(self, txid: int, offset: int, payload: bytes) -> str:
        if self.state != self.RECEIVING:
            return "invalid_state"
        if txid != self.image_tx:
            return "wrong_transaction"
        if offset != self.next_offset or not payload:
            self.state = self.IDLE
            self.image_tx = 0
            self.next_offset = 0
            return "wrong_offset"
        self.image[offset:offset + len(payload)] = payload
        self.next_offset += len(payload)
        return "ok"

    def prepare(self, txid: int) -> str:
        if txid != self.image_tx or self.state != self.RECEIVING:
            return "invalid_state"
        if self.next_offset != len(self.image):
            return "invalid_state"
        if hashlib.sha256(self.image).digest() != self.expected_sha:
            self.state = self.IDLE
            self.next_offset = 0
            return "image_verification_failed"
        self.state = self.PREPARED
        return "ok"

    def activate(self, txid: int) -> str:
        if txid != self.image_tx or self.state != self.PREPARED:
            return "invalid_state"
        if not self.rollback:
            return "rollback_unavailable"
        self.restarted = True
        return "ack_then_restart"

    def confirm(self, txid: int) -> str:
        if not txid or self.state != self.PENDING_CONFIRM:
            return "invalid_state"
        if (not self.local_health or not self.peer_health or
                not 18 <= self.peer_generation <= 19):
            return "compatibility_rejected"
        self.state = self.IDLE
        self.maintenance = False
        self.session_tx = 0
        return "ok"

    def abort(self, txid: int, *, offset: int = 0,
              payload: bytes = b"") -> str:
        expected_tx = (self.image_tx if self.state in
                       (self.RECEIVING, self.PREPARED) else self.session_tx)
        if not self.maintenance or txid != expected_tx:
            return "wrong_transaction"
        if offset != 0 or payload:
            return "malformed"
        if self.state == self.PENDING_CONFIRM:
            self.rolled_back = True
            return "ack_then_rollback"
        self.state = self.IDLE
        self.image_tx = 0
        self.next_offset = 0
        self.maintenance = False
        self.session_tx = 0
        return "ok"

    def status(self, query_txid: int):
        del query_txid
        if not self.maintenance:
            return None
        transaction = (self.image_tx if self.state in
                       (self.RECEIVING, self.PREPARED) else 0)
        return self.state, transaction

    def frame_error(self) -> None:
        return None

    def exit_maintenance(self, txid: int, *, offset: int = 0,
                         payload: bytes = b"") -> str:
        if not self.maintenance or not txid:
            return "wrong_transaction"
        if offset != 0 or payload:
            return "malformed"
        self.maintenance = False
        self.session_tx = 0
        return "ok"


class OtaReceiverProtocolTests(unittest.TestCase):
    def test_mkot_wire_shape_and_crc(self):
        wire = frame(0x04, 0x1234, offset=4096, sequence=9,
                     payload=bytes(range(64)))
        self.assertEqual(wire[:4], MAGIC)
        self.assertEqual(len(wire), HEADER + 64 + 4)
        damaged = bytearray(wire)
        damaged[HEADER + 3] ^= 0x40
        self.assertNotEqual(struct.unpack_from("<I", damaged, HEADER + 64)[0],
                            crc32(damaged[4:HEADER + 64]))

    def test_boot_uses_only_pending_verify_as_pending_confirm(self):
        self.assertEqual(ReceiverModel(pending=False).state, ReceiverModel.IDLE)
        self.assertEqual(ReceiverModel(pending=True).state,
                         ReceiverModel.PENDING_CONFIRM)

    def test_manifest_requires_idle_maintenance_and_exact_transaction(self):
        receiver = ReceiverModel()
        digest = hashlib.sha256(b"x" * MIN_CHUNK).digest()
        self.assertEqual(receiver.manifest(7, MIN_CHUNK, digest), "invalid_state")
        self.assertTrue(receiver.enter(7))
        self.assertEqual(receiver.manifest(8, MIN_CHUNK, digest),
                         "wrong_transaction")
        self.assertEqual(receiver.manifest(7, MIN_CHUNK, digest), "ok")

    def test_data_is_exactly_sequential(self):
        receiver = ReceiverModel()
        image = b"a" * (2 * MIN_CHUNK)
        receiver.enter(9)
        receiver.manifest(9, len(image), hashlib.sha256(image).digest())
        self.assertEqual(receiver.data(9, 0, image[:MIN_CHUNK]), "ok")
        self.assertEqual(receiver.data(9, MIN_CHUNK, image[MIN_CHUNK:]), "ok")

    def test_bad_data_for_current_image_fails_closed(self):
        receiver = ReceiverModel()
        receiver.enter(9)
        receiver.manifest(9, MIN_CHUNK, hashlib.sha256(b"a" * MIN_CHUNK).digest())
        self.assertEqual(receiver.data(9, 1, b"a"), "wrong_offset")
        self.assertEqual(receiver.state, receiver.IDLE)

    def test_wrong_transaction_data_and_frame_error_preserve_transfer(self):
        receiver = ReceiverModel()
        receiver.enter(9)
        receiver.manifest(9, MIN_CHUNK, hashlib.sha256(b"a" * MIN_CHUNK).digest())
        self.assertEqual(receiver.data(10, 0, b"a"), "wrong_transaction")
        receiver.frame_error()
        self.assertEqual(receiver.state, receiver.RECEIVING)
        self.assertEqual(receiver.image_tx, 9)

    def test_prepare_hash_failure_aborts_transient_state_to_idle(self):
        receiver = ReceiverModel()
        receiver.enter(11)
        receiver.manifest(11, MIN_CHUNK, b"z" * 32)
        receiver.data(11, 0, b"a" * MIN_CHUNK)
        self.assertEqual(receiver.prepare(11), "image_verification_failed")
        self.assertEqual(receiver.state, receiver.IDLE)

    def test_activate_acknowledges_before_restart_and_requires_rollback(self):
        receiver = ReceiverModel(rollback=True)
        receiver.enter(13)
        receiver.state = receiver.PREPARED
        receiver.image_tx = 13
        self.assertEqual(receiver.activate(13), "ack_then_restart")
        self.assertTrue(receiver.restarted)
        receiver.rollback = False
        self.assertEqual(receiver.activate(13), "rollback_unavailable")

    def test_pending_confirm_accepts_a_new_tx_and_requires_both_health_checks(self):
        receiver = ReceiverModel(pending=True)
        self.assertTrue(receiver.enter(99, peer_generation=18))
        self.assertEqual(receiver.confirm(99), "compatibility_rejected")
        receiver.local_health = receiver.peer_health = True
        self.assertEqual(receiver.confirm(99), "ok")
        self.assertFalse(receiver.maintenance)
        too_old = ReceiverModel(pending=True)
        too_old.enter(100, peer_generation=17)
        too_old.local_health = too_old.peer_health = True
        self.assertEqual(too_old.confirm(100), "compatibility_rejected")

    def test_abort_releases_idle_or_transient_and_rolls_back_pending(self):
        transient = ReceiverModel()
        transient.enter(17)
        transient.state = transient.RECEIVING
        transient.image_tx = 17
        self.assertEqual(transient.abort(17), "ok")
        self.assertEqual(transient.state, transient.IDLE)
        self.assertFalse(transient.maintenance)
        pending = ReceiverModel(pending=True)
        pending.enter(18)
        self.assertEqual(pending.abort(18), "ack_then_rollback")
        self.assertTrue(pending.rolled_back)

    def test_new_session_aborts_preserved_transfer_only_with_image_transaction(self):
        old_image_tx = 0x10203040
        new_session_tx = 0x50607080
        for state in (ReceiverModel.RECEIVING, ReceiverModel.PREPARED):
            with self.subTest(state=state):
                receiver = ReceiverModel()
                receiver.enter(old_image_tx)
                receiver.image_tx = old_image_tx
                receiver.state = state
                receiver.next_offset = 77
                self.assertEqual(receiver.exit_maintenance(old_image_tx), "ok")
                self.assertTrue(receiver.enter(new_session_tx))

                self.assertEqual(receiver.abort(new_session_tx),
                                 "wrong_transaction")
                self.assertEqual(receiver.state, state)
                self.assertEqual(receiver.next_offset, 77)
                self.assertTrue(receiver.maintenance)

                self.assertEqual(receiver.abort(old_image_tx), "ok")
                self.assertEqual(receiver.state, ReceiverModel.IDLE)
                self.assertFalse(receiver.maintenance)

    def test_pending_abort_still_uses_current_session_transaction(self):
        receiver = ReceiverModel(pending=True)
        receiver.enter(0x33445566)
        receiver.image_tx = 0x11223344
        self.assertEqual(receiver.abort(receiver.image_tx), "wrong_transaction")
        self.assertFalse(receiver.rolled_back)
        self.assertEqual(receiver.abort(receiver.session_tx),
                         "ack_then_rollback")
        self.assertTrue(receiver.rolled_back)

    def test_exit_maintenance_preserves_every_state_and_transaction(self):
        for state in (ReceiverModel.IDLE, ReceiverModel.RECEIVING,
                      ReceiverModel.PREPARED, ReceiverModel.PENDING_CONFIRM):
            with self.subTest(state=state):
                receiver = ReceiverModel()
                receiver.enter(27)
                receiver.state = state
                receiver.image_tx = 27 if state in (
                    ReceiverModel.RECEIVING, ReceiverModel.PREPARED) else 0
                receiver.next_offset = 123
                self.assertEqual(receiver.exit_maintenance(27), "ok")
                self.assertEqual(receiver.state, state)
                self.assertEqual(receiver.next_offset, 123)
                self.assertFalse(receiver.maintenance)

    def test_status_query_transaction_can_release_channel_without_changing_image(self):
        old_session_tx = 0x10203040
        new_query_tx = 0x50607080
        receiver = ReceiverModel()
        receiver.enter(old_session_tx)
        receiver.state = receiver.RECEIVING
        receiver.image_tx = old_session_tx
        receiver.next_offset = 77

        self.assertEqual(receiver.status(new_query_tx),
                         (receiver.RECEIVING, old_session_tx))
        self.assertEqual(receiver.exit_maintenance(new_query_tx), "ok")
        self.assertEqual(receiver.state, receiver.RECEIVING)
        self.assertEqual(receiver.image_tx, old_session_tx)
        self.assertEqual(receiver.next_offset, 77)

    def test_malformed_exit_does_not_abort_transient_state(self):
        receiver = ReceiverModel()
        receiver.enter(31)
        receiver.state = receiver.RECEIVING
        receiver.next_offset = 77
        self.assertEqual(receiver.exit_maintenance(31, offset=1), "malformed")
        self.assertEqual(receiver.state, receiver.RECEIVING)
        self.assertEqual(receiver.next_offset, 77)
        self.assertTrue(receiver.maintenance)


if __name__ == "__main__":
    unittest.main()
