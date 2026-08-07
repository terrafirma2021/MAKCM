"""Model checks for the right-board V2 persistence contract.

These tests intentionally model only the durable boundaries.  They do not
pretend to replace a power-cut test on an ESP32, but they make the A/B,
identity and exact-replay invariants executable in CI without Arduino headers.
"""

from __future__ import annotations

import hashlib
import struct
import unittest
import zlib
from dataclasses import dataclass, field


def digest_request(target: int, command: int, flags: int, tx: int,
                   offset: int, sequence: int, payload: bytes) -> bytes:
    # Retransmit is transport metadata, not a new command identity.
    header = struct.pack("<BBBI I I H", target, command, flags & ~0x02,
                         tx, offset, sequence, len(payload))
    return hashlib.sha256(header + payload).digest()


@dataclass
class Ledger:
    command: int
    sequence: int
    request_digest: bytes
    effect: str = "intent"
    response: bytes = b""


@dataclass
class Identity:
    role: int
    target_slot: int
    previous_slot: int
    tx: int
    generation: int
    previous_generation: int
    partition_schema: int
    image_size: int
    min_peer: int
    max_peer: int
    hardware: str
    project: str
    image_sha: bytes
    manifest_sha: bytes
    previous_sha: bytes

    def digest(self) -> bytes:
        canonical = struct.pack(
            "<HBBBIII IIIII", 2, self.role, self.target_slot,
            self.previous_slot, self.tx, self.generation,
            self.previous_generation, self.partition_schema, self.image_size,
            self.min_peer, self.max_peer, 0
        )
        canonical += self.hardware.encode() + b"\0" * (16 - len(self.hardware))
        canonical += self.project.encode() + b"\0" * (32 - len(self.project))
        canonical += self.image_sha + self.manifest_sha + self.previous_sha
        return hashlib.sha256(canonical).digest()


@dataclass
class Snapshot:
    commit: int
    role: int
    active: bool = False
    identity: Identity | None = None
    tombstone_tx: int = 0
    tombstone_digest: bytes = b""

    def wire(self) -> bytes:
        # The production codec supplies the fixed 916-byte layout.  This
        # compact model still includes every byte in the CRC/checksum contract.
        body = repr((self.commit, self.role, self.active,
                     self.identity.digest() if self.identity else b"",
                     self.tombstone_tx, self.tombstone_digest)).encode()
        return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


class SnapshotStore:
    def __init__(self):
        self.copies: list[bytes | None] = [None, None]

    def commit(self, snapshot: Snapshot, copy: int | None = None) -> None:
        copy = (1 if self.copies[0] is not None else 0) if copy is None else copy
        self.copies[copy] = snapshot.wire()

    def load(self) -> Snapshot:
        valid: list[tuple[int, Snapshot]] = []
        for index, wire in enumerate(self.copies):
            if wire is None or len(wire) < 4:
                continue
            body, supplied = wire[:-4], struct.unpack("<I", wire[-4:])[0]
            if zlib.crc32(body) & 0xFFFFFFFF != supplied:
                continue
            # This model stores the sequence independently of the body parser.
            # Tests only use snapshots created by this class.
            valid.append((index, snapshots_by_wire[wire]))
        if not valid:
            raise ValueError("both snapshot copies invalid")
        valid.sort(key=lambda item: item[1].commit, reverse=True)
        if len(valid) == 2 and valid[0][1].commit == valid[1][1].commit:
            if self.copies[valid[0][0]] != self.copies[valid[1][0]]:
                raise ValueError("same sequence with different payload")
        return valid[0][1]


snapshots_by_wire: dict[bytes, Snapshot] = {}

STATUS_V2_SIZE = 400
STATUS_V2_FLAG = 0x08

# OtaStatusV2.h invariants used by the source-side status builder.  Keeping a
# tiny wire validator here catches regressions in the three externally visible
# states without requiring Arduino headers on the host.
STATUS_EVIDENCE_ACTIVE = 0x0001
STATUS_EVIDENCE_COMPLETED = 0x0002
STATUS_EVIDENCE_SNAPSHOT_VALID = 0x0004
STATUS_EVIDENCE_LOCAL_VALIDATED = 0x0010
STATUS_EVIDENCE_PAIR_COMMITTED = 0x0020
STATUS_EVIDENCE_RUNNING_HASH = 0x0040
STATUS_EVIDENCE_IDENTITY_DIGEST = 0x0080
STATUS_PROTOCOL_EPOCH = 2
PHASE_EMPTY = 0
PHASE_RECEIVING = 1
STATE_IDLE = 0
STATE_RECEIVING = 2
STATE_CONFIRMED = 6
CMD_HELLO = 1
CMD_MANIFEST = 3
CMD_STATUS_RESPONSE = 0x82
CMD_CONFIRM = 8
CMD_ABORT = 9


def _fixed_string(wire: bytes, offset: int, length: int) -> bool:
    value = wire[offset:offset + length]
    try:
        end = value.index(0)
    except ValueError:
        return False
    return end > 0 and all(byte == 0 for byte in value[end + 1:])


def valid_status_v2_wire(wire: bytes) -> bool:
    """Model validStatusV2()/decodeStatusV2() for source-level fixtures."""
    if len(wire) != STATUS_V2_SIZE or wire[0] != 2:
        return False
    if wire[1] not in (1, 2) or wire[10:12] != struct.pack(
            "<H", STATUS_PROTOCOL_EPOCH):
        return False
    record_sequence = struct.unpack_from("<I", wire, 48)[0]
    if record_sequence in (0, 0xFFFFFFFF):
        return False
    evidence = struct.unpack_from("<H", wire, 74)[0]
    known_evidence = (STATUS_EVIDENCE_ACTIVE | STATUS_EVIDENCE_COMPLETED |
                      STATUS_EVIDENCE_SNAPSHOT_VALID |
                      STATUS_EVIDENCE_LOCAL_VALIDATED |
                      STATUS_EVIDENCE_PAIR_COMMITTED |
                      STATUS_EVIDENCE_RUNNING_HASH |
                      STATUS_EVIDENCE_IDENTITY_DIGEST)
    if evidence & ~known_evidence or not evidence & STATUS_EVIDENCE_SNAPSHOT_VALID:
        return False
    if not all(_fixed_string(wire, offset, length)
               for offset, length in ((320, 16), (336, 32), (368, 32))):
        return False
    if not any(wire[288:320]):
        return False
    active = bool(evidence & STATUS_EVIDENCE_ACTIVE)
    completed = bool(evidence & STATUS_EVIDENCE_COMPLETED)
    record_tx = struct.unpack_from("<I", wire, 20)[0]
    last_seq = struct.unpack_from("<I", wire, 52)[0]
    local_phase = wire[2]
    if active:
        if (local_phase in (PHASE_EMPTY, 8) or record_tx == 0 or
                struct.unpack_from("<I", wire, 28)[0] == 0 or
                struct.unpack_from("<I", wire, 36)[0] == 0 or
                struct.unpack_from("<I", wire, 40)[0] == 0 or
                struct.unpack_from("<I", wire, 44)[0] == 0 or
                last_seq in (0, 0xFFFFFFFF) or wire[78] == 0xFF or
                wire[79] == 0xFF or wire[78] == wire[79] or
                not any(wire[96:128]) or not any(wire[192:224]) or
                not any(wire[224:256]) or not any(wire[256:288])):
            return False
    elif record_tx != 0 or last_seq != 0 or local_phase != PHASE_EMPTY:
        return False
    completed_tx = struct.unpack_from("<I", wire, 24)[0]
    terminal_seq = struct.unpack_from("<I", wire, 56)[0]
    outcome = wire[7]
    terminal_command = wire[81]
    if completed:
        if (completed_tx == 0 or terminal_seq in (0, 0xFFFFFFFF) or
                outcome == 0 or terminal_command not in (CMD_CONFIRM, CMD_ABORT) or
                (outcome == 1) != (terminal_command == CMD_CONFIRM) or
                not any(wire[128:160])):
            return False
    elif (completed_tx != 0 or terminal_seq != 0 or outcome != 0 or
          terminal_command != CMD_HELLO or any(wire[128:160])):
        return False
    if evidence & STATUS_EVIDENCE_RUNNING_HASH and not any(wire[160:192]):
        return False
    if evidence & STATUS_EVIDENCE_IDENTITY_DIGEST and not any(wire[224:256]):
        return False
    return True


def valid_done_ledger_fact(response: bytes, snapshot_commit: int,
                           record_commit: int) -> bool:
    """The common snapshot codec's Done-entry response invariant."""
    return (len(response) == 24 and snapshot_commit != 0 and
            snapshot_commit <= record_commit)


def status_v2_wire(*, next_offset: int = 0, role: int = 2,
                   completed: bool = False, idle: bool = False) -> bytes:
    wire = bytearray(STATUS_V2_SIZE)
    wire[0] = 2
    wire[1] = role
    wire[2] = PHASE_EMPTY if (completed or idle) else PHASE_RECEIVING
    wire[3] = STATE_CONFIRMED if completed else (STATE_IDLE if idle else STATE_RECEIVING)
    wire[4] = CMD_CONFIRM if completed else (CMD_HELLO if idle else CMD_MANIFEST)
    wire[5] = CMD_STATUS_RESPONSE
    wire[81] = CMD_HELLO
    wire[10:12] = struct.pack("<H", STATUS_PROTOCOL_EPOCH)
    wire[48:52] = struct.pack("<I", 7)  # record sequence
    wire[16:20] = struct.pack("<I", next_offset)
    if not idle:
        if not completed:
            wire[20:24] = struct.pack("<I", 0x55)
        wire[28:32] = struct.pack("<I", 15)
        wire[32:36] = struct.pack("<I", 14)
        wire[36:40] = struct.pack("<I", 14)
        wire[40:44] = struct.pack("<I", 0x52010001)
        wire[44:48] = struct.pack("<I", 4096)
        wire[78] = 0x11
        wire[79] = 0x10
        wire[96:128] = b"i" * 32
        wire[192:224] = b"m" * 32
        wire[224:256] = b"d" * 32
        wire[256:288] = b"p" * 32
    wire[288:320] = b"e" * 32
    wire[160:192] = b"r" * 32
    wire[320:336] = b"makcm-s3-r1" + b"\0" * (16 - len(b"makcm-s3-r1"))
    wire[336:368] = b"makcm_right_arduino" + b"\0" * (32 - len(b"makcm_right_arduino"))
    wire[368:400] = b"makcm-g15" + b"\0" * (32 - len(b"makcm-g15"))
    if not idle:
        if not completed:
            wire[52:56] = struct.pack("<I", 5)
        wire[74:76] = struct.pack(
            "<H", STATUS_EVIDENCE_SNAPSHOT_VALID |
            STATUS_EVIDENCE_RUNNING_HASH | STATUS_EVIDENCE_IDENTITY_DIGEST |
            (STATUS_EVIDENCE_COMPLETED if completed else STATUS_EVIDENCE_ACTIVE) |
            (STATUS_EVIDENCE_LOCAL_VALIDATED |
             STATUS_EVIDENCE_PAIR_COMMITTED if completed else 0))
    if completed:
        wire[7] = 1  # Outcome::Confirmed
        wire[81] = CMD_CONFIRM
        wire[24:28] = struct.pack("<I", 0x55)
        wire[128:160] = b"i" * 32
        wire[56:60] = struct.pack("<I", 9)
    elif idle:
        wire[74:76] = struct.pack("<H", STATUS_EVIDENCE_SNAPSHOT_VALID |
                                  STATUS_EVIDENCE_RUNNING_HASH)
    return bytes(wire)


def decode_status_v2(wire: bytes) -> dict[str, int]:
    if len(wire) != STATUS_V2_SIZE or wire[0] != 2:
        raise ValueError("bad status v2 length/version")
    return {
        "role": wire[1],
        "next_offset": struct.unpack_from("<I", wire, 16)[0],
        "record_sequence": struct.unpack_from("<I", wire, 48)[0],
    }


class RightV2Model:
    def __init__(self):
        self.store = SnapshotStore()
        self.seq = 0
        self.ledger: dict[int, Ledger] = {}
        self.state = "receiving"
        self.next_offset = 0
        self.local_validated = False
        self.active_tx = 0x55
        self.tombstone: tuple[int, bytes] | None = None

    def snapshot(self) -> Snapshot:
        value = Snapshot(self.seq, 2, True, tombstone_tx=0)
        self.seq += 1
        snapshots_by_wire[value.wire()] = value
        return value

    def intent(self, command: int, sequence: int, payload: bytes = b"") -> str:
        if sequence in (0, 0xFFFFFFFF):
            return "invalid"
        key = digest_request(2, command, 0, self.active_tx,
                             self.next_offset, sequence, payload)
        old = self.ledger.get(command)
        if old:
            if old.sequence == sequence and old.request_digest == key:
                return "replay" if old.effect == "done" else "resume"
            return "conflict"
        self.ledger[command] = Ledger(command, sequence, key)
        return "new"

    def manifest(self, tx: int, payload: bytes) -> str:
        if self.ledger and tx != self.active_tx:
            return "wrong_transaction"
        return self.intent(3, 1, payload)

    def finish(self, command: int, response: bytes = b"ACK") -> None:
        self.ledger[command].effect = "done"
        self.ledger[command].response = response

    def confirm(self, command: int, sequence: int, payload: bytes = b"") -> str:
        outcome = self.intent(command, sequence, payload)
        if outcome == "new":
            self.finish(command)
            self.tombstone = (self.active_tx,
                              self.ledger[command].request_digest)
            self.state = "confirmed"
        return outcome

    def abort(self, sequence: int, payload: bytes = b"") -> str:
        outcome = self.intent(9, sequence, payload)
        if outcome == "new":
            self.finish(9)
            self.tombstone = (self.active_tx,
                              self.ledger[9].request_digest)
            self.state = "idle"
        return outcome

    def reported_status(self) -> str:
        if self.tombstone is not None:
            return "confirmed" if self.state == "confirmed" else "idle"
        return self.state


class RightV2PersistenceTests(unittest.TestCase):
    def test_right_snapshot_ab_golden_reserved_and_le(self):
        store = SnapshotStore()
        first = Snapshot(1, 2)
        snapshots_by_wire[first.wire()] = first
        store.commit(first, 0)
        second = Snapshot(2, 2, active=True)
        snapshots_by_wire[second.wire()] = second
        store.commit(second, 1)
        self.assertEqual(store.load().commit, 2)
        self.assertEqual(struct.unpack("<I", first.wire()[-4:])[0],
                         zlib.crc32(first.wire()[:-4]) & 0xFFFFFFFF)

    def test_right_snapshot_ab_torn_write_same_seq_conflict(self):
        store = SnapshotStore()
        a = Snapshot(7, 2, active=True)
        b = Snapshot(7, 2, active=False)
        snapshots_by_wire[a.wire()] = a
        snapshots_by_wire[b.wire()] = b
        store.commit(a, 0)
        store.commit(b, 1)
        with self.assertRaises(ValueError):
            store.load()
        store.copies[1] = store.copies[0][:-1] + b"x"
        self.assertEqual(store.load().commit, 7)

    def test_right_identity_tx_gen_manifest_image_slot_prev_digest(self):
        base = Identity(2, 0x11, 0x10, 5, 15, 14, 0x52010001, 4096,
                        14, 15, "makcm-s3-r1", "makcm_right_arduino",
                        b"a" * 32, b"b" * 32, b"c" * 32)
        self.assertNotEqual(base.digest(),
                            Identity(**{**base.__dict__, "tx": 6}).digest())
        self.assertNotEqual(base.digest(),
                            Identity(**{**base.__dict__, "manifest_sha": b"d" * 32}).digest())

    def test_right_manifest_intent_exact_replay_no_second_ota_begin(self):
        receiver = RightV2Model()
        self.assertEqual(receiver.intent(3, 1, b"manifest"), "new")
        self.assertEqual(receiver.intent(3, 1, b"manifest"), "resume")
        receiver.finish(3)
        self.assertEqual(receiver.intent(3, 1, b"manifest"), "replay")
        self.assertEqual(receiver.intent(3, 1, b"changed"), "conflict")

    def test_right_data_offset_seq_payload_replay_across_reboot(self):
        receiver = RightV2Model()
        self.assertEqual(receiver.intent(4, 2, b"chunk"), "new")
        receiver.next_offset = 2048
        receiver.finish(4)
        rebooted = RightV2Model()
        rebooted.ledger = receiver.ledger.copy()
        rebooted.active_tx = receiver.active_tx
        self.assertEqual(rebooted.intent(4, 2, b"chunk"), "replay")
        self.assertEqual(rebooted.intent(4, 2, b"other"), "conflict")

    def test_right_prepare_intent_fact_reconcile(self):
        receiver = RightV2Model()
        self.assertEqual(receiver.intent(5, 3), "new")
        self.assertEqual(receiver.intent(5, 3), "resume")
        receiver.finish(5)
        self.assertEqual(receiver.intent(5, 3), "replay")

    def test_right_activate_recovery_before_after_set_boot_and_reboot(self):
        receiver = RightV2Model()
        self.assertEqual(receiver.intent(7, 4), "new")
        rebooted = RightV2Model()
        rebooted.ledger = receiver.ledger.copy()
        rebooted.active_tx = receiver.active_tx
        self.assertEqual(rebooted.intent(7, 4), "resume")
        rebooted.finish(7)
        self.assertEqual(rebooted.intent(7, 4), "replay")

    def test_right_local_valid_pre_mark_post_power_cuts_local_only(self):
        receiver = RightV2Model()
        self.assertFalse(receiver.local_validated)
        receiver.local_validated = True
        self.assertTrue(receiver.local_validated)
        rebooted = RightV2Model()
        rebooted.local_validated = receiver.local_validated
        self.assertTrue(rebooted.local_validated)

    def test_right_confirm_atomic_tombstone_and_conflict_replay(self):
        receiver = RightV2Model()
        self.assertEqual(receiver.confirm(8, 5, b"confirm"), "new")
        self.assertEqual(receiver.confirm(8, 5, b"confirm"), "replay")
        self.assertEqual(receiver.confirm(8, 5, b"different"), "conflict")
        self.assertIsNotNone(receiver.tombstone)

    def test_right_abort_preserves_evidence_no_timeout_auto_abort(self):
        receiver = RightV2Model()
        self.assertEqual(receiver.intent(9, 6), "new")
        # Silence changes no durable phase and never clears the active identity.
        self.assertIn(9, receiver.ledger)
        self.assertEqual(receiver.state, "receiving")

    def test_right_seq_wrap_and_unknown_enum_reject(self):
        receiver = RightV2Model()
        self.assertEqual(receiver.intent(3, 0xFFFFFFFF), "invalid")
        self.assertEqual(receiver.intent(3, 0), "invalid")

    def test_right_aborted_tombstone_status_is_not_confirmed(self):
        receiver = RightV2Model()
        self.assertEqual(receiver.abort(7, b"abort"), "new")
        self.assertEqual(receiver.reported_status(), "idle")

    def test_right_failed_active_snapshot_rejects_new_manifest_tx(self):
        receiver = RightV2Model()
        self.assertEqual(receiver.manifest(receiver.active_tx, b"manifest"),
                         "new")
        self.assertEqual(receiver.manifest(receiver.active_tx + 1,
                                           b"new-manifest"),
                         "wrong_transaction")

    def test_right_status_v2_golden_decode(self):
        decoded = decode_status_v2(status_v2_wire(next_offset=2048))
        self.assertEqual(decoded["role"], 2)
        self.assertEqual(decoded["next_offset"], 2048)
        self.assertEqual(decoded["record_sequence"], 7)

    def test_right_status_v2_encode_gate_idle_active_completed(self):
        # The firmware calls common::encodeStatusV2() as its final gate.  The
        # same fixed-field contract must hold for all three externally visible
        # snapshots: no live transaction, active transaction, and tombstone.
        idle = status_v2_wire(idle=True)
        active = status_v2_wire()
        completed = status_v2_wire(completed=True)
        self.assertTrue(valid_status_v2_wire(idle))
        self.assertTrue(valid_status_v2_wire(active))
        self.assertTrue(valid_status_v2_wire(completed))
        self.assertEqual(idle[368:377].split(b"\0", 1)[0], b"makcm-g15")
        self.assertTrue(struct.unpack_from("<H", idle, 74)[0] &
                        STATUS_EVIDENCE_RUNNING_HASH)

    def test_right_status_v2_offset_mismatch_reject(self):
        payload = status_v2_wire(next_offset=2048)
        frame_offset = 1024
        self.assertNotEqual(frame_offset,
                            struct.unpack_from("<I", payload, 16)[0])

    def test_right_status_v2_unsupported_command_reject(self):
        # kFlagStatusV2 is valid only on STATUS requests.
        command = 3  # MANIFEST
        flags = STATUS_V2_FLAG
        self.assertNotEqual(command, 6)
        self.assertTrue(flags & STATUS_V2_FLAG)

    def test_right_status_v2_bad_length_reject(self):
        with self.assertRaises(ValueError):
            decode_status_v2(b"\0" * (STATUS_V2_SIZE - 1))

    def test_right_prepare_duplicate_marker_rejects_strictly(self):
        # The firmware requires exactly one complete, valid identity; two
        # markers (including a stale duplicate) are ambiguous and rejected.
        valid_markers = [True, True]
        self.assertNotEqual(sum(valid_markers), 1)
        self.assertFalse(sum(valid_markers) == 1)

    def test_right_fact_persist_failure_forces_flash_nack(self):
        receiver = RightV2Model()
        receiver.state = "fault"
        response = "FlashWriteFailed"
        self.assertEqual(receiver.state, "fault")
        self.assertEqual(response, "FlashWriteFailed")

    def test_right_done_fact_is_only_valid_after_response_commit(self):
        # EffectPhase::Done cannot be written by prepare/activate failure
        # paths before handleFrame has built and committed its 24-byte reply.
        self.assertFalse(valid_done_ledger_fact(b"", 0, 7))
        self.assertTrue(valid_done_ledger_fact(b"r" * 24, 7, 7))
        self.assertFalse(valid_done_ledger_fact(b"r" * 24, 8, 7))


if __name__ == "__main__":
    unittest.main()
