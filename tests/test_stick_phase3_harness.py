from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import stick_phase3_harness as harness  # noqa: E402


STATUS = (
    "STICK_STATUS(42,0,raw_safe,none,0,0,xinput_4131_3519_v1_candidate,none,0,0,0,0,0,0,0,0,"
    "cmd=0/0,crc=0,seq=0/0/0,ttl=0,profile=hip/0,modified=0,center=0,"
    "override=0/0/0,sat=0,guard=0,decode=0,stall=0,fifo=0/0,mode_switch=0,"
    "reversal=0,wrong=0/0,source_conflict=0,frame=0,barrier=0/0/0,"
    "target=0/0/0/0,timing=0/0/0/0/0,"
    "awaiting=0/0/0/0)"
)
CAPS = "STICK_CAPS(v=2,state_q15=1,rate=0,move_compat=0,game_cal=0,profiles=1,max_ttl=80)"
ARMED_STATUS = STATUS.replace(
    "42,0,raw_safe,none,0,0,", "42,99,stick_idle,state_q15,0,0,"
)
RAW_STATUS = STATUS


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


class SerialDouble:
    """Closed pyserial stand-in that records setup order for pure tests."""

    def __init__(self, events, *, fail_at=None, **kwargs):
        self.events = events
        self.fail_at = fail_at
        self.kwargs = kwargs
        self._port = kwargs["port"]
        self._dtr = None
        self._rts = None
        self.closed = False

    def _event(self, name, value=None):
        self.events.append((name, value) if value is not None else (name,))
        if self.fail_at == name:
            raise OSError(f"{name} failed")

    @property
    def dtr(self):
        return self._dtr

    @dtr.setter
    def dtr(self, value):
        self._event("dtr", value)
        self._dtr = value

    @property
    def rts(self):
        return self._rts

    @rts.setter
    def rts(self, value):
        self._event("rts", value)
        self._rts = value

    @property
    def port(self):
        return self._port

    @port.setter
    def port(self, value):
        self._event("port", value)
        self._port = value

    def open(self):
        self._event("open")

    def reset_input_buffer(self):
        self._event("reset_input_buffer")

    def reset_output_buffer(self):
        self._event("reset_output_buffer")

    def close(self):
        self._event("close")
        self.closed = True


def ack(op="begin", physical=42, injection=99, mode="mouse",
        fmt=harness.EXPECTED_FORMAT, cal="none"):
    return f"STICK_ACK({op},{physical},{injection},{mode},{fmt},{cal})"


def manifest(root: Path, *, mutate: str | None = None) -> harness.ArtifactManifest:
    values = {}
    for label in ("firmware_binary", "local_entry", "candidate", "review"):
        path = root / f"{label}.bin"
        path.write_bytes(label.encode("ascii"))
        values[label] = harness.Artifact(label, path,
                                         hashlib.sha256(path.read_bytes()).hexdigest())
    if mutate:
        values[mutate] = harness.Artifact(mutate, values[mutate].path, "0" * 64)
    return harness.ArtifactManifest(values["firmware_binary"], values["local_entry"],
                                    values["candidate"], values["review"])


class StickPhase3HarnessTest(unittest.TestCase):
    def test_serial_open_is_posix_exclusive_but_windows_unchanged(self):
        serial_module = mock.Mock()
        serial_module.Serial.side_effect = lambda **kwargs: SerialDouble([], **kwargs)
        with mock.patch.dict(sys.modules, {"serial": serial_module}):
            with mock.patch.object(harness, "_is_posix_platform", return_value=True):
                harness.SerialTransport("/dev/ttyACM0", 115200, 1.0)
            serial_module.Serial.assert_called_once_with(
                port=None, baudrate=115200, timeout=1.0,
                exclusive=True,
            )

            serial_module.Serial.reset_mock()
            with mock.patch.object(harness, "_is_posix_platform", return_value=False):
                harness.SerialTransport("COM3", 115200, 1.0)
            serial_module.Serial.assert_called_once_with(
                port=None, baudrate=115200, timeout=1.0,
            )

    def test_serial_setup_order_forces_lines_low_and_flushes_buffers(self):
        events = []
        serial_module = mock.Mock()
        serial_module.Serial.side_effect = lambda **kwargs: SerialDouble(events, **kwargs)
        with mock.patch.dict(sys.modules, {"serial": serial_module}), \
                mock.patch.object(harness, "_is_posix_platform", return_value=True):
            transport = harness.SerialTransport("/dev/ttyACM0", 115200, 1.0)

        self.assertEqual(events, [
            ("dtr", False), ("rts", False),
            ("port", "/dev/ttyACM0"), ("open",),
            ("reset_input_buffer",), ("reset_output_buffer",),
        ])
        self.assertIsNotNone(transport._serial)

    def test_serial_setup_failure_closes_and_fails_closed(self):
        for fail_at in ("dtr", "rts", "reset_input_buffer", "reset_output_buffer"):
            with self.subTest(fail_at=fail_at):
                events = []
                serial_module = mock.Mock()
                instance = SerialDouble(events, fail_at=fail_at, port=None,
                                        baudrate=115200, timeout=1.0)
                serial_module.Serial.return_value = instance
                with mock.patch.dict(sys.modules, {"serial": serial_module}), \
                        mock.patch.object(harness, "_is_posix_platform", return_value=True), \
                        self.assertRaises(harness.HarnessError):
                    harness.SerialTransport("/dev/ttyACM0", 115200, 1.0)
                self.assertTrue(instance.closed)
                self.assertEqual(events[-1], ("close",))

    def test_linux_secondary_guard_blocks_probe_before_transport(self):
        factory = mock.Mock(side_effect=AssertionError("serial must not open"))
        with tempfile.TemporaryDirectory() as folder:
            audit = Path(folder) / "guard.json"
            with mock.patch.object(harness, "_is_linux_platform", return_value=True), \
                    mock.patch.object(
                        harness,
                        "_running_secondary_processes",
                        return_value=(("ai_secondary", 1234),),
                    ), self.assertRaises(harness.SerialOwnershipError):
                harness.run_probe_on_port(
                    "/dev/ttyACM0", audit_path=audit,
                    transport_factory=factory,
                )
            factory.assert_not_called()
            payload = json.loads(audit.read_text(encoding="utf-8"))
            self.assertEqual(payload["outcome"], "failure")
            self.assertTrue(any(
                event["direction"] == "event" and
                "blocked" in event["line"] and
                "ai_secondary(pid=1234)" in event["line"]
                for event in payload["events"]
            ))

    def test_linux_process_probe_keeps_only_name_and_pid(self):
        calls = []

        def fake_run(command, **kwargs):
            calls.append((command, kwargs))
            if command[-1] == "ai_secondary":
                return mock.Mock(returncode=0, stdout="321\n")
            return mock.Mock(returncode=1, stdout="")

        with mock.patch.object(harness, "_is_linux_platform", return_value=True), \
                mock.patch.object(harness.subprocess, "run", side_effect=fake_run):
            processes = harness._running_secondary_processes()
        self.assertEqual(processes, (("ai_secondary", 321),))
        self.assertTrue(all(kwargs["text"] is True for _, kwargs in calls))
        self.assertTrue(all("shell" not in kwargs for _, kwargs in calls))

    def test_linux_secondary_guard_override_is_audited_and_probe_runs(self):
        transport = MockTransport([CAPS, STATUS])
        factory = mock.Mock(return_value=transport)
        with tempfile.TemporaryDirectory() as folder:
            audit = Path(folder) / "override.json"
            with mock.patch.object(harness, "_is_linux_platform", return_value=True), \
                    mock.patch.object(
                        harness,
                        "_running_secondary_processes",
                        return_value=(("ai_secondary", 4321),),
                    ):
                result = harness.run_probe_on_port(
                    "/dev/ttyACM0", audit_path=audit,
                    transport_factory=factory,
                    allow_active_secondary_process=True,
                )
            self.assertEqual(result.status.physical_session, 42)
            factory.assert_called_once()
            payload = json.loads(audit.read_text(encoding="utf-8"))
            self.assertTrue(any(
                event["direction"] == "event" and
                "manual_override" in event["line"] and
                "ai_secondary(pid=4321)" in event["line"] and
                "no_exclusivity_guarantee=true" in event["line"]
                for event in payload["events"]
            ))

    def test_linux_secondary_guard_applies_to_arm_before_transport(self):
        factory = mock.Mock(side_effect=AssertionError("serial must not open"))
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            audit = root / "arm_guard.json"
            with mock.patch.object(harness, "_is_linux_platform", return_value=True), \
                    mock.patch.object(
                        harness,
                        "_running_secondary_processes",
                        return_value=(("makcm_secondary", 98),),
                    ), self.assertRaises(harness.SerialOwnershipError):
                harness.run_arm_on_port(
                    "/dev/ttyACM0", allow_stick_output=True,
                    artifacts=manifest(root), audit_path=audit,
                    transport_factory=factory,
                )
            factory.assert_not_called()
            payload = json.loads(audit.read_text(encoding="utf-8"))
            self.assertEqual(payload["outcome"], "failure")
            self.assertIn("makcm_secondary(pid=98)",
                          "\n".join(event["line"] for event in payload["events"]))

    def test_windows_skips_linux_process_detection(self):
        transport = MockTransport([CAPS, STATUS])
        with tempfile.TemporaryDirectory() as folder:
            audit = Path(folder) / "windows.json"
            with mock.patch.object(harness, "_is_linux_platform", return_value=False), \
                    mock.patch.object(
                        harness,
                        "_running_secondary_processes",
                        side_effect=AssertionError("must not inspect Linux processes"),
                    ):
                harness.run_probe_on_port(
                    "COM3", audit_path=audit,
                    transport_factory=lambda _port, _baud, _timeout: transport,
                )
            payload = json.loads(audit.read_text(encoding="utf-8"))
            self.assertTrue(any(
                "skipped_non_posix" in event["line"]
                for event in payload["events"]
            ))

    def test_exact_parser_and_rejects_malformed(self):
        caps = harness.parse_caps(CAPS + "\r\n")
        self.assertEqual(caps.version, 2)
        self.assertEqual(caps.game_cal, 0)
        self.assertEqual(caps.profiles, 1)
        legacy = harness.parse_caps(
            "STICK_CAPS(v=1,state_q15=1,rate=0,move_compat=0,profiles=1,max_ttl=80)"
        )
        self.assertEqual((legacy.version, legacy.game_cal), (1, 0))
        self.assertEqual(harness.parse_status(STATUS).physical_session, 42)
        legacy_status = STATUS.replace(",decode=0", "")
        self.assertEqual(harness.parse_status(legacy_status).counters["decode"], 0)
        self.assertEqual(harness.parse_ack(ack()).injection_session, 99)
        self.assertEqual(
            harness.parse_nack("STICK_NACK(begin,profile_mismatch,raw_safe)").reason,
            "profile_mismatch",
        )
        malformed = (
            "STICK_CAPS(v=1,state_q15=1,rate=0,move_compat=0,profiles=1)",
            STATUS.replace("raw_safe", "RAW_SAFE"),
            "STICK_ACK(begin,42,99,mouse,format,none,extra)",
            "STICK_NACK(begin,profile_mismatch,not_a_state)",
        )
        for line in malformed:
            with self.subTest(line=line), self.assertRaises(harness.ProtocolError):
                harness.parse_response(line)

    def test_state_machine_rejects_duplicate_stale_and_unexpected(self):
        machine = harness.HandshakeStateMachine("probe")
        machine.send(harness.CAPS_COMMAND)
        machine.receive(CAPS)
        with self.assertRaisesRegex(harness.ProtocolError, "duplicate"):
            machine.receive(CAPS)
        # A status response is stale while caps is still expected on a fresh
        # machine; the exact phase is checked rather than silently skipped.
        stale = harness.HandshakeStateMachine("probe")
        stale.send(harness.CAPS_COMMAND)
        with self.assertRaises(harness.ProtocolError):
            stale.receive(STATUS)

    def test_success_delayed_ack_and_no_data_plane(self):
        transport = MockTransport([
            CAPS, STATUS, None, None,
            ack(),
            ack("output_mode", 42, 99, "stick"),
        ])
        with tempfile.TemporaryDirectory() as folder:
            result = harness.run_arm(
                transport, allow_stick_output=True,
                artifacts=manifest(Path(folder)), artifacts_verified=True,
                timeout=0.05, audit_path=Path(folder) / "audit.json",
            )
            self.assertEqual((result.physical_session, result.injection_session), (42, 99))
            self.assertEqual(transport.writes, [
                "km.caps()", "km.stick.status()",
                "km.stick.begin(xinput_4131_3519_v1_candidate,none,state_q15)",
                "km.output_mode(stick)",
            ])
        self.assertFalse(any(any(command.startswith(prefix)
                                   for prefix in harness.DATA_PLANE_PREFIXES)
                                for command in transport.writes))

    def test_teardown_waits_for_mouse_ack_then_confirms_raw_status(self):
        transport = MockTransport([
            CAPS, ARMED_STATUS, None,
            ack("output_mode", 42, 0, "mouse"), RAW_STATUS,
        ])
        with tempfile.TemporaryDirectory() as folder:
            result = harness.run_teardown(
                transport, port="COM3", timeout=0.05,
                audit_path=Path(folder) / "teardown.json",
            )
            self.assertEqual(result.output_ack.mode, "mouse")
            self.assertEqual(result.final_status.state, "raw_safe")
            self.assertEqual(result.final_status.injection_session, 0)
        self.assertEqual(transport.writes, [
            "km.caps()", "km.stick.status()", "km.output_mode(mouse)",
            "km.stick.status()",
        ])
        self.assertFalse(any(any(command.startswith(prefix)
                               for prefix in harness.DATA_PLANE_PREFIXES)
                             for command in transport.writes))

    def test_teardown_ack_mismatch_fails_closed_without_final_status(self):
        transport = MockTransport([
            CAPS, ARMED_STATUS, ack("output_mode", 42, 99, "mouse"),
        ])
        with tempfile.TemporaryDirectory() as folder:
            audit = Path(folder) / "teardown_ack_failure.json"
            with self.assertRaisesRegex(harness.ProtocolError, "ACK"):
                harness.run_teardown(
                    transport, timeout=0.05, audit_path=audit,
                )
            payload = json.loads(audit.read_text(encoding="utf-8"))
            self.assertEqual(payload["outcome"], "failure")
        self.assertEqual(transport.writes, [
            "km.caps()", "km.stick.status()", "km.output_mode(mouse)",
        ])

    def test_teardown_timeout_is_not_best_effort(self):
        transport = MockTransport([CAPS, ARMED_STATUS])
        with tempfile.TemporaryDirectory() as folder:
            audit = Path(folder) / "teardown_timeout.json"
            with self.assertRaises(harness.HarnessTimeout):
                harness.run_teardown(
                    transport, timeout=0.01, audit_path=audit,
                )
            payload = json.loads(audit.read_text(encoding="utf-8"))
            self.assertEqual(payload["outcome"], "failure")
        self.assertEqual(transport.writes, [
            "km.caps()", "km.stick.status()", "km.output_mode(mouse)",
        ])

    def test_teardown_final_status_must_be_raw_safe_and_zeroed(self):
        unsafe = RAW_STATUS.replace("raw_safe,none", "stick_idle,state_q15")
        transport = MockTransport([
            CAPS, ARMED_STATUS, ack("output_mode", 42, 0, "mouse"), unsafe,
        ])
        with tempfile.TemporaryDirectory() as folder, \
                self.assertRaisesRegex(harness.ProtocolError, "final status"):
            harness.run_teardown(
                transport, timeout=0.05,
                audit_path=Path(folder) / "teardown_status_failure.json",
            )
        self.assertEqual(transport.writes, [
            "km.caps()", "km.stick.status()", "km.output_mode(mouse)",
            "km.stick.status()",
        ])

    def test_teardown_cli_is_an_independent_subcommand(self):
        transport = MockTransport([
            CAPS, ARMED_STATUS, ack("output_mode", 42, 0, "mouse"), RAW_STATUS,
        ])
        with tempfile.TemporaryDirectory() as folder, \
                mock.patch.object(
                    harness, "run_teardown_on_port",
                    return_value=harness.TeardownResult(
                        harness.parse_caps(CAPS),
                        harness.parse_status(ARMED_STATUS),
                        harness.parse_ack(ack("output_mode", 42, 0, "mouse")),
                        harness.parse_status(RAW_STATUS),
                    ),
                ) as teardown:
            with mock.patch("builtins.print") as printer:
                self.assertEqual(harness.main([
                    "teardown", "--port", "COM3", "--timeout", "0.05",
                    "--audit", str(Path(folder) / "cli.json"),
                ]), 0)
            teardown.assert_called_once_with(
                "COM3", baud=115200, timeout=0.05,
                audit_path=Path(folder) / "cli.json",
                allow_active_secondary_process=False,
            )
            rendered = printer.call_args.args[0]
            self.assertEqual(json.loads(rendered)["mode"], "mouse")
        self.assertEqual(transport.writes, [])

    def test_missing_authorization_does_not_write(self):
        transport = MockTransport()
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(harness.AuthorizationError):
                harness.run_arm(
                    transport, allow_stick_output=False,
                    artifacts=manifest(Path(folder)), artifacts_verified=True,
                    audit_path=Path(folder) / "audit.json",
                )
        self.assertEqual(transport.writes, [])

    def test_caps_mismatch_rolls_back(self):
        transport = MockTransport([
            CAPS.replace("state_q15=1", "state_q15=0"),
        ])
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(harness.ProtocolError):
                harness.run_arm(
                    transport, allow_stick_output=True,
                    artifacts=manifest(Path(folder)), artifacts_verified=True,
                    timeout=0.05, audit_path=Path(folder) / "audit.json",
                )
        self.assertEqual(transport.writes, ["km.caps()", "km.output_mode(mouse)"])

    def test_nack_rolls_back(self):
        transport = MockTransport([CAPS, STATUS,
                                   "STICK_NACK(begin,profile_mismatch,raw_safe)"])
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaisesRegex(harness.ProtocolError, "NACK"):
                harness.run_arm(
                    transport, allow_stick_output=True,
                    artifacts=manifest(Path(folder)), artifacts_verified=True,
                    timeout=0.05, audit_path=Path(folder) / "audit.json",
                )
        self.assertEqual(transport.writes[-1], "km.output_mode(mouse)")

    def test_timeout_rolls_back(self):
        transport = MockTransport([CAPS, STATUS])
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(harness.HarnessTimeout):
                harness.run_arm(
                    transport, allow_stick_output=True,
                    artifacts=manifest(Path(folder)), artifacts_verified=True,
                    timeout=0.01, audit_path=Path(folder) / "audit.json",
                )
        self.assertEqual(transport.writes[-1], "km.output_mode(mouse)")

    def test_wrong_session_or_format_rolls_back(self):
        transport = MockTransport([
            CAPS, STATUS,
            ack(physical=43),
        ])
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(harness.ProtocolError):
                harness.run_arm(
                    transport, allow_stick_output=True,
                    artifacts=manifest(Path(folder)), artifacts_verified=True,
                    timeout=0.05, audit_path=Path(folder) / "audit.json",
                )
        self.assertEqual(transport.writes[-1], "km.output_mode(mouse)")

    def test_output_ack_wrong_format_rolls_back(self):
        transport = MockTransport([
            CAPS, STATUS, ack(),
            ack("output_mode", 42, 99, "stick", "wrong_format"),
        ])
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(harness.ProtocolError):
                harness.run_arm(
                    transport, allow_stick_output=True,
                    artifacts=manifest(Path(folder)), artifacts_verified=True,
                    timeout=0.05, audit_path=Path(folder) / "audit.json",
                )
        self.assertEqual(transport.writes[-1], "km.output_mode(mouse)")

    def test_artifact_hash_mismatch_before_transport_factory(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            bad = manifest(root, mutate="candidate")
            factory = mock.Mock(side_effect=AssertionError("must not open serial"))
            with self.assertRaises(harness.ArtifactError):
                harness.run_arm_on_port(
                    "COM3", allow_stick_output=True, artifacts=bad,
                    audit_path=root / "audit.json", transport_factory=factory,
                )
            factory.assert_not_called()
            self.assertTrue((root / "audit.json").exists())

    def test_probe_command_whitelist_and_read_only(self):
        transport = MockTransport([CAPS, STATUS])
        with tempfile.TemporaryDirectory() as folder:
            result = harness.run_probe(
                transport, port="COM3", timeout=0.05,
                audit_path=Path(folder) / "probe.json",
            )
            self.assertEqual(result.caps.version, 2)
        self.assertEqual(transport.writes, ["km.caps()", "km.stick.status()"])
        for command in ("km.stick.state(1,1,0,0,32)", "km.stick.rate(1,1,1,32)"):
            with self.assertRaises(harness.ProtocolError):
                harness.assert_control_command(command)

    def test_probe_ignores_only_exact_button_stream_and_prompt_prefix(self):
        transport = MockTransport([
            b"km.\x00\r\n", b">>> " + CAPS.encode("ascii") + b"\r\n",
            b">>> km.\x1f\r\n", b">>> " + STATUS.encode("ascii") + b"\r\n",
        ])
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "probe.json"
            result = harness.run_probe(
                transport, port="COM3", timeout=0.05, audit_path=path,
            )
            payload = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(result.status.state, "raw_safe")
        ignored = [
            event["line"] for event in payload["events"]
            if event["direction"] == "event"
        ]
        self.assertIn("button_stream:ignored mask=0", ignored)
        self.assertIn("button_stream:ignored mask=31", ignored)

    def test_probe_handles_newline_button_mask_and_exact_tail(self):
        transport = MockTransport([
            b"km.\n", b"\r\n", b">>> " + CAPS.encode("ascii") + b"\r\n",
            STATUS,
        ])
        with tempfile.TemporaryDirectory() as folder:
            result = harness.run_probe(
                transport, timeout=0.05, audit_path=Path(folder) / "probe.json",
            )
            self.assertEqual(result.caps.version, 2)

    def test_probe_rejects_malformed_button_stream(self):
        for malformed in (b"km.\x20\r\n", b"km.bad\r\n"):
            with self.subTest(malformed=malformed):
                transport = MockTransport([malformed])
                with tempfile.TemporaryDirectory() as folder, \
                        self.assertRaises(harness.ProtocolError):
                    harness.run_probe(
                        transport, timeout=0.05,
                        audit_path=Path(folder) / "probe.json",
                    )

    def test_audit_contains_directions_and_is_atomic_result(self):
        transport = MockTransport([CAPS, STATUS])
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "audit.json"
            harness.run_probe(transport, port="COM3", baud=115200,
                              timeout=0.05, audit_path=path)
            data = path.read_text(encoding="utf-8")
            self.assertIn('"direction": "command"', data)
            self.assertIn('"direction": "response"', data)
            self.assertNotIn("secret", data)


if __name__ == "__main__":
    unittest.main()
