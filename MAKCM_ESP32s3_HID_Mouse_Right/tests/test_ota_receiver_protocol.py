"""Host-side checks for the right-board MKOT maintenance contract.

The Arduino receiver is exercised on hardware, while this small model keeps
the framing/state safety properties testable without an ESP32 or flash chip.
It intentionally mirrors common/OtaProtocol.h rather than inventing a second
wire format.
"""

from __future__ import annotations

import hashlib
import struct
import unittest


MAGIC = b"MKOT"
VERSION = 1
HEADER = 24
MIN_CHUNK = 2048
MAX_CHUNK = 4096
RIGHT = 2
MANIFEST_SIZE = 192


def crc32(data: bytes) -> int:
    return __import__("binascii").crc32(data) & 0xFFFFFFFF


def frame(command: int, txid: int, *, target: int = RIGHT, offset: int = 0,
          sequence: int = 0, payload: bytes = b"", flags: int = 0) -> bytes:
    assert txid
    assert len(payload) <= MAX_CHUNK
    header = struct.pack("<4sBBBBIIHHI", MAGIC, VERSION, target, command,
                         flags, txid, offset, len(payload), 0, sequence)
    return header + payload + struct.pack("<I", crc32(header[4:] + payload))


class ReceiverModel:
    IDLE, RECEIVING, PREPARED, ACTIVATED, AWAIT_CONFIRM, FAULT = range(6)

    def __init__(self, rollback=False):
        self.state = self.IDLE
        self.rollback = rollback
        self.txid = 0
        self.image = bytearray()
        self.expected = 0
        self.sha = b""
        self.maintenance = False
        self.persistent = False
        self.ack_before_reboot = False
        self.rollback_requested = False

    def bootstrap(self, txid: int):
        if txid == 0:
            raise ValueError("zero transaction")
        self.maintenance = True
        self.txid = txid
        self.persistent = True

    def manifest(self, txid: int, *, role=RIGHT, chip=9,
                 image_kind=1, chunk=4096, image_size=4096,
                 signature=True):
        if not self.maintenance or txid != self.txid:
            return "wrong_transaction"
        if role != RIGHT or chip != 9 or image_kind != 1:
            return "manifest_rejected"
        if not MIN_CHUNK <= chunk <= MAX_CHUNK:
            return "manifest_rejected"
        if not signature:
            return "signature_rejected"
        self.image = bytearray(image_size)
        self.expected = 0
        self.sha = b""
        self.state = self.RECEIVING
        return "ok"

    def data(self, txid: int, offset: int, payload: bytes):
        if self.state != self.RECEIVING or txid != self.txid:
            return "invalid_state"
        if not payload or len(payload) > MAX_CHUNK:
            return "wrong_offset"
        if offset + len(payload) > len(self.image):
            return "wrong_offset"
        if offset < self.expected:
            end = offset + len(payload)
            return "ok" if bytes(self.image[offset:end]) == payload else "wrong_offset"
        if offset != self.expected:
            return "wrong_offset"
        self.image[offset:offset + len(payload)] = payload
        self.expected += len(payload)
        return "ok"

    def prepare(self, expected_sha: bytes):
        if self.state != self.RECEIVING or self.expected != len(self.image):
            return "invalid_state"
        if hashlib.sha256(self.image).digest() != expected_sha:
            self.state = self.FAULT
            return "image_verification_failed"
        # No key/verifier is linked into the Arduino image yet.
        self.state = self.FAULT
        return "signature_rejected"

    def activate(self):
        return "ok" if self.rollback else "rollback_unavailable"

    def abort(self):
        if self.state in (self.ACTIVATED, self.AWAIT_CONFIRM):
            if not self.rollback:
                return "rollback_unavailable"
            # The production receiver emits ACK while this durable evidence
            # still exists, then asks the bootloader to roll back.  It must
            # not clear the record and reboot the same pending slot.
            self.ack_before_reboot = True
            self.rollback_requested = True
            return "ack_then_rollback"
        self.state = self.FAULT
        self.persistent = False
        self.maintenance = False
        self.txid = 0
        return "ok"


class BootstrapModel:
    """Small model of EspUsbHost's asynchronous text bootstrap.

    The production RX task must continue dispatching bridge frames while the
    USB client task performs teardown.  In particular, DeviceGoneAccepted
    cannot be consumed if the command handler waits synchronously.
    """

    ACTIVE, OPENING, EMPTY, CLEANING, FAULT = range(5)

    def __init__(self, state=ACTIVE, wait_ms=3500):
        self.state = state
        self.wait_ms = wait_ms
        self.pending = None
        self.started_ms = 0

    def request(self, txid, peer=4):
        if not txid or self.state == self.FAULT:
            return "OTA_UNAVAILABLE"
        if self.pending is not None:
            return "OTA_PENDING" if self.pending == (txid, peer) else "OTA_UNAVAILABLE"
        if self.state in (self.ACTIVE, self.OPENING, self.CLEANING):
            self.state = self.CLEANING
            self.pending = (txid, peer)
            self.started_ms = 0
            return "OTA_PENDING"
        if self.state == self.EMPTY:
            return "OTA_READY"
        return "OTA_UNAVAILABLE"

    def service(self, now_ms):
        if self.pending is None:
            return None
        if self.state == self.EMPTY:
            self.pending = None
            return "OTA_READY"
        if self.state == self.FAULT or now_ms - self.started_ms >= self.wait_ms:
            self.pending = None
            return "OTA_UNAVAILABLE"
        return None


class OtaReceiverProtocolTests(unittest.TestCase):
    def test_mkot_wire_shape_and_crc(self):
        wire = frame(0x04, 0x1234, offset=4096, sequence=9,
                     payload=bytes(range(64)))
        self.assertEqual(wire[:4], MAGIC)
        self.assertEqual(len(wire), HEADER + 64 + 4)
        tampered = bytearray(wire)
        tampered[HEADER + 3] ^= 0x40
        supplied = struct.unpack_from("<I", tampered, HEADER + 64)[0]
        self.assertNotEqual(supplied, crc32(tampered[4:HEADER + 64]))

    def test_bootstrap_manifest_role_and_sequential_idempotence(self):
        receiver = ReceiverModel()
        receiver.bootstrap(7)
        self.assertEqual(receiver.manifest(7, role=1), "manifest_rejected")
        self.assertEqual(receiver.manifest(7), "ok")
        chunk = b"x" * MIN_CHUNK
        self.assertEqual(receiver.data(7, 0, chunk), "ok")
        self.assertEqual(receiver.data(7, 0, chunk), "ok")
        self.assertEqual(receiver.data(7, MIN_CHUNK + 1, chunk), "wrong_offset")
        self.assertEqual(receiver.data(7, MIN_CHUNK, chunk), "ok")
        self.assertEqual(receiver.data(7, 4096, chunk), "wrong_offset")

    def test_hash_and_signature_are_fail_closed(self):
        receiver = ReceiverModel()
        receiver.bootstrap(9)
        self.assertEqual(receiver.manifest(9, image_size=MIN_CHUNK), "ok")
        payload = b"a" * MIN_CHUNK
        self.assertEqual(receiver.data(9, 0, payload), "ok")
        self.assertEqual(receiver.prepare(hashlib.sha256(payload).digest()),
                         "signature_rejected")

    def test_activation_requires_bootloader_rollback(self):
        self.assertEqual(ReceiverModel(rollback=False).activate(),
                         "rollback_unavailable")
        self.assertEqual(ReceiverModel(rollback=True).activate(), "ok")

    def test_await_confirm_abort_ack_then_bootloader_rollback(self):
        receiver = ReceiverModel(rollback=True)
        receiver.bootstrap(17)
        receiver.state = receiver.AWAIT_CONFIRM
        self.assertEqual(receiver.abort(), "ack_then_rollback")
        self.assertTrue(receiver.ack_before_reboot)
        self.assertTrue(receiver.rollback_requested)
        self.assertTrue(receiver.persistent)
        self.assertTrue(receiver.maintenance)

    def test_receiving_or_prepared_abort_clears_download_state(self):
        receiver = ReceiverModel(rollback=True)
        receiver.bootstrap(18)
        self.assertEqual(receiver.manifest(18), "ok")
        self.assertEqual(receiver.abort(), "ok")
        self.assertEqual(receiver.state, receiver.FAULT)
        self.assertFalse(receiver.persistent)
        self.assertFalse(receiver.maintenance)

    def test_bootstrap_keeps_rx_alive_for_cleanup_ack(self):
        receiver = BootstrapModel()
        self.assertEqual(receiver.request(7), "OTA_PENDING")
        # Retransmitting the same request does not restart teardown or reject
        # the transaction while DeviceGoneAccepted is still in flight.
        self.assertEqual(receiver.request(7), "OTA_PENDING")
        receiver.state = BootstrapModel.EMPTY
        self.assertEqual(receiver.service(100), "OTA_READY")
        self.assertIsNone(receiver.pending)

    def test_bootstrap_fault_and_timeout_are_explicit(self):
        failed = BootstrapModel(state=BootstrapModel.FAULT)
        self.assertEqual(failed.request(9), "OTA_UNAVAILABLE")

        timed = BootstrapModel()
        self.assertEqual(timed.request(9), "OTA_PENDING")
        self.assertEqual(timed.service(3499), None)
        self.assertEqual(timed.service(3500), "OTA_UNAVAILABLE")


if __name__ == "__main__":
    unittest.main()
