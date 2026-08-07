from __future__ import annotations

import json
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import stick_phase3_state_harness as harness  # noqa: E402


CAPS = "STICK_CAPS(v=2,state_q15=1,rate=0,move_compat=0,game_cal=0,profiles=1,max_ttl=80)"


def status(
    *,
    physical: int = 42,
    injection: int = 0,
    state: str = "raw_safe",
    source: str = "none",
    seq: int = 0,
    x: int = 0,
    y: int = 0,
    valid: int = 0,
    age: int = 0,
    center: int = 0,
    timing: str = "0/0/0/0/0",
    physical_x: int = 0,
    physical_y: int = 0,
    merged_x: int | None = None,
    merged_y: int | None = None,
) -> str:
    # This is the exact 38-field frame accepted by the shared phase-3 parser.
    if merged_x is None:
        merged_x = x
    if merged_y is None:
        merged_y = y
    return (
        f"STICK_STATUS({physical},{injection},{state},{source},{seq},{age},"
        f"{harness.EXPECTED_FORMAT},none,{x},{y},{physical_x},{physical_y},"
        f"{merged_x},{merged_y},0,0,"
        f"cmd={valid}/0,crc=0,seq=0/0/0,ttl=0,profile=hip/0,modified=0,"
        f"center={center},override=0/0/0,sat=0,guard=0,decode=0,stall=0,fifo=0/0,"
        "mode_switch=1,reversal=0,wrong=0/0,source_conflict=0,frame=0,"
        f"barrier=0/0/0,target=0/0/0/0,timing={timing},awaiting=0/0/0/0)"
    )


def accepted_status(seq: int, x: int, y: int, valid: int) -> str:
    return status(
        injection=99,
        state="stick_active" if (x or y) else "stick_idle",
        source="state_q15",
        seq=seq,
        x=x,
        y=y,
        valid=valid,
        timing=f"{seq}/0/{seq}/0/0",
    )


def begin_ack() -> str:
    return f"STICK_ACK(begin,42,99,mouse,{harness.EXPECTED_FORMAT},none)"


def stick_ack() -> str:
    return f"STICK_ACK(output_mode,42,99,stick,{harness.EXPECTED_FORMAT},none)"


def mouse_ack() -> str:
    return f"STICK_ACK(output_mode,42,0,mouse,{harness.EXPECTED_FORMAT},none)"


class MockTransport:
    def __init__(self, responses=()):
        self.responses = list(responses)
        self.writes: list[str] = []
        self.closed = False

    def write(self, command: str):
        self.writes.append(command)

    def readline(self, timeout=None):
        if self.responses:
            return self.responses.pop(0)
        return None

    def close(self):
        self.closed = True


def manifest(root: Path) -> harness.ArtifactManifest:
    artifacts = []
    for label in ("firmware_binary", "local_entry", "candidate", "review"):
        path = root / f"{label}.bin"
        path.write_bytes(label.encode("ascii"))
        artifacts.append(harness.Artifact(
            label, path, hashlib.sha256(path.read_bytes()).hexdigest()
        ))
    return harness.ArtifactManifest(*artifacts)


class StateQ15HarnessTest(unittest.TestCase):
    def test_codec_rejects_forbidden_source_and_overlimit(self):
        with self.assertRaises(harness.RadialLimitError):
            harness.StateQ15Command(99, 1, 6553, 6553)
        with self.assertRaises(harness.CapabilityError):
            harness.assert_ds04_command("km.stick.rate(99,1,1,1,32)")
        with self.assertRaises(harness.ProtocolError):
            harness.parse_state_q15("km.stick.state(99,1,0,0,32)*0000")

    def test_codec_round_trip_and_ttl_bounds(self):
        wire = harness.encode_state_q15(99, 1, 0, 6553, 16)
        parsed = harness.parse_state_q15(wire)
        self.assertEqual(parsed.vector, (0, 6553))
        self.assertEqual(parsed.ttl_ms, 16)
        with self.assertRaises(harness.TtlError):
            harness.StateQ15Command(99, 1, 0, 0, 15)
        with self.assertRaises(harness.TtlError):
            harness.StateQ15Command(99, 1, 0, 0, 81)

    def test_state_machine_rejects_wrong_session_stale_and_missing_zero(self):
        machine = harness.DataPlaneStateMachine(42, 99)
        first = harness.StateQ15Command(99, 1, 1, 0)
        machine.send(first)
        with self.assertRaises(harness.SessionError):
            machine.send(harness.StateQ15Command(100, 2, 0, 0))
        with self.assertRaises(harness.MissingZeroError):
            machine.send(harness.StateQ15Command(99, 2, 2, 0))
        with self.assertRaises(harness.StaleSequenceError):
            machine.send(harness.StateQ15Command(99, 1, 0, 0))
        machine.send(harness.StateQ15Command(99, 3, 0, 0))
        machine.require_explicit_zero()

    def _happy_transport(self):
        # Two vectors keep this mock test quick while still exercising the
        # mandatory nonzero -> explicit zero boundary.
        vectors = (
            harness.SafeVector("zero", 0, 0),
            harness.SafeVector("x", 0.05, 0),
            harness.SafeVector("zero", 0, 0),
        )
        responses = [CAPS, status() , begin_ack(), stick_ack()]
        for index, vector in enumerate(vectors, 1):
            x, y = vector.x, vector.y
            responses.append(accepted_status(index, x, y, index))
        responses.extend([mouse_ack(), status()])
        return MockTransport(responses), vectors

    def test_mock_run_generates_audit_and_mouse_teardown(self):
        transport, vectors = self._happy_transport()
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "audit.json"
            result = harness.run_ds04(
                transport,
                allow_stick_output=True,
                allow_ds04_state_q15=True,
                timeout=0.05,
                audit_path=path,
                vectors=vectors,
            )
            payload = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(result.injection_session, 99)
        self.assertTrue(transport.closed)
        self.assertEqual(payload["schema"], "makcm.ds04-state-q15-transcript.v1")
        self.assertEqual(payload["outcome"], "success")
        commands = [e["line"] for e in payload["events"] if e["direction"] == "command"]
        self.assertIn("km.output_mode(mouse)", commands)
        self.assertTrue(any(line.startswith("km.stick.state(") for line in commands))

    def test_state_run_ignores_button_stream_between_control_responses(self):
        transport, vectors = self._happy_transport()
        transport.responses.insert(0, b"km.\x00\r\n")
        transport.responses.insert(2, b">>> km.\x01\r\n")
        transport.responses[3] = b">>> " + transport.responses[3].encode("ascii") + b"\r\n"
        with tempfile.TemporaryDirectory() as folder:
            result = harness.run_ds04(
                transport,
                allow_stick_output=True,
                allow_ds04_state_q15=True,
                timeout=0.05,
                audit_path=Path(folder) / "buttons.json",
                vectors=vectors,
            )
        self.assertEqual(result.final_status.state, "raw_safe")

    def test_second_authorization_blocks_all_writes(self):
        transport = MockTransport()
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(harness.AuthorizationError):
                harness.run_ds04(
                    transport,
                    allow_stick_output=True,
                    allow_ds04_state_q15=False,
                    timeout=0.01,
                    audit_path=Path(folder) / "auth.json",
                )
        self.assertEqual(transport.writes, [])

    def test_nack_and_timeout_still_attempt_mouse_teardown(self):
        nack = MockTransport([CAPS, status(), "STICK_NACK(begin,profile_mismatch,raw_safe)", mouse_ack(), status()])
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(harness.ProtocolError):
                harness.run_ds04(
                    nack, allow_stick_output=True, allow_ds04_state_q15=True,
                    timeout=0.01, audit_path=Path(folder) / "nack.json",
                )
        self.assertIn("km.output_mode(mouse)", nack.writes)

        timeout = MockTransport([CAPS, status()])
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(harness.HarnessTimeout):
                harness.run_ds04(
                    timeout, allow_stick_output=True, allow_ds04_state_q15=True,
                    timeout=0.005, audit_path=Path(folder) / "timeout.json",
                )
        self.assertIn("km.output_mode(mouse)", timeout.writes)

    def test_final_teardown_status_is_required(self):
        transport, vectors = self._happy_transport()
        # Keep ACK but return an armed status: successful vectors must fail
        # closed rather than trusting the mouse ACK alone.
        transport.responses[-1] = accepted_status(3, 0, 0, 3)
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(harness.MouseTeardownError):
                harness.run_ds04(
                    transport,
                    allow_stick_output=True,
                    allow_ds04_state_q15=True,
                    timeout=0.05,
                    audit_path=Path(folder) / "final.json",
                    vectors=vectors,
                )
        self.assertTrue(transport.closed)

    def test_live_semantics_allow_neutral_jitter_and_require_board_timing(self):
        machine = harness.DataPlaneStateMachine(42, 99)
        zero = harness.StateQ15Command(99, 1, 0, 0)
        machine.send(zero)
        zero_status = harness._p3.parse_status(status(
            injection=99, state="stick_idle", source="state_q15", seq=1,
            valid=1, physical_x=-128, physical_y=0,
            merged_x=-128, merged_y=0, timing="1/10/1/10/11",
        ))
        machine.accept_status(zero_status, zero)
        self.assertEqual(machine.emission_latencies_ms, [1])

        nonzero = harness.StateQ15Command(99, 2, 1638, 0)
        machine.send(nonzero)
        active = harness._p3.parse_status(status(
            injection=99, state="stick_active", source="state_q15", seq=2,
            x=1638, valid=2, physical_x=-128, physical_y=0,
            merged_x=1638, merged_y=0, timing="2/20/2/20/21",
        ))
        machine.accept_status(active, nonzero)
        with self.assertRaises(harness.StatusValidationError):
            late = harness.DataPlaneStateMachine(42, 99)
            command = harness.StateQ15Command(99, 1, 1638, 0)
            late.send(command)
            late.accept_status(harness._p3.parse_status(status(
                injection=99, state="stick_active", source="state_q15", seq=1,
                x=1638, valid=1, timing="1/10/1/10/13",
            )), command)

    def test_pending_emission_is_polled_not_mistaken_for_success(self):
        vectors = (harness.SafeVector("zero", 0, 0),)
        pending = status(
            injection=99, state="stick_idle", source="state_q15", seq=1,
            valid=1, timing="1/10/0/0/0",
        )
        emitted = accepted_status(1, 0, 0, 1)
        transport = MockTransport([
            CAPS, status(), begin_ack(), stick_ack(), pending, emitted,
            mouse_ack(), status(),
        ])
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "pending.json"
            result = harness.run_ds04(
                transport, allow_stick_output=True,
                allow_ds04_state_q15=True, timeout=0.05,
                audit_path=path, vectors=vectors,
            )
            payload = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(result.emission_latencies_ms, (0,))
        self.assertTrue(any(
            event["direction"] == "event" and
            event["line"].startswith("emission_pending")
            for event in payload["events"]
        ))
        self.assertEqual(transport.writes.count("km.stick.status()"), 4)

    def test_live_wrapper_checks_auth_and_artifacts_before_open(self):
        opened = []

        def factory(*args):
            opened.append(args)
            return MockTransport()

        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            exact = manifest(root)
            with self.assertRaises(harness.AuthorizationError):
                harness.run_ds04_on_port(
                    "/dev/ttyACM0", allow_stick_output=True,
                    allow_ds04_state_q15=False, artifacts=exact,
                    audit_path=root / "auth-live.json",
                    transport_factory=factory,
                )
            self.assertEqual(opened, [])
            bad = harness.ArtifactManifest(
                harness.Artifact(
                    "firmware_binary", exact.firmware_binary.path, "0" * 64
                ),
                exact.local_entry, exact.candidate, exact.review,
            )
            with self.assertRaises(harness.ArtifactError):
                harness.run_ds04_on_port(
                    "/dev/ttyACM0", allow_stick_output=True,
                    allow_ds04_state_q15=True, artifacts=bad,
                    audit_path=root / "hash-live.json",
                    transport_factory=factory,
                )
            self.assertEqual(opened, [])

    def test_live_wrapper_uses_explicit_factory_and_confirms_teardown(self):
        transport, vectors = self._happy_transport()
        opened = []

        def factory(port, baud, timeout):
            opened.append((port, baud, timeout))
            return transport

        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            result = harness.run_ds04_on_port(
                "/dev/ttyACM0", allow_stick_output=True,
                allow_ds04_state_q15=True, artifacts=manifest(root),
                timeout=0.05, audit_path=root / "live.json",
                vectors=vectors, allow_active_secondary_process=True,
                transport_factory=factory,
            )
        self.assertEqual(opened, [("/dev/ttyACM0", 115200, 0.05)])
        self.assertIsNotNone(result.final_status)
        self.assertEqual(result.final_status.state, "raw_safe")
        self.assertTrue(transport.closed)


if __name__ == "__main__":
    unittest.main()
