#!/usr/bin/env python3
"""Bounded DS-03 serial handshake harness.

The module intentionally keeps protocol parsing and the handshake state machine
independent of ``pyserial``.  ``pyserial`` is imported only when a live serial
transport (or ``--list-ports``) is requested.  The harness never emits stick
data-plane commands; the arm path is limited to the caps/status queries,
``begin``, and output-mode barriers.  A separate, explicit teardown path
confirms the delayed mouse barrier and a final raw-safe status before it
returns control to its caller.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
from typing import Any, Callable, Iterable, Mapping, Protocol, Sequence


EXPECTED_FORMAT = "xinput_4131_3519_v1_candidate"
EXPECTED_CAL = "none"
DEFAULT_BAUD = 115200
DEFAULT_TIMEOUT = 3.0
DEFAULT_AUDIT_DIR = Path("diagnostics")

# These are process *names*, not arbitrary command-line fragments.  Keeping
# the list small and exact avoids leaking command-line/environment data into a
# transcript while still protecting the known MAKCM Secondary owners.
KNOWN_SECONDARY_PROCESS_NAMES = (
    "ai_secondary",
    "makcm_secondary",
    "makcm-secondary",
    "MAKCM_Aio_Tool",
)


def _is_posix_platform() -> bool:
    """Keep platform branching injectable for pure host-side tests."""
    return os.name == "posix"


def _is_linux_platform() -> bool:
    """Return whether Linux-specific process inspection is available."""
    return sys.platform.startswith("linux")

CAPS_COMMAND = "km.caps()"
STATUS_COMMAND = "km.stick.status()"
OUTPUT_MOUSE_COMMAND = "km.output_mode(mouse)"
OUTPUT_STICK_COMMAND = "km.output_mode(stick)"
PROBE_COMMANDS = frozenset((CAPS_COMMAND, STATUS_COMMAND))
CONTROL_COMMAND_PREFIXES = ("km.caps()", "km.stick.status()",
                             "km.stick.begin(", "km.output_mode(")
DATA_PLANE_PREFIXES = ("km.stick.state(", "km.stick.rate(",
                       "km.stick.lease(", "km.move(")

_TOKEN = re.compile(r"^[A-Za-z0-9_.:+-]+$")
_DECIMAL = re.compile(r"^(?:0|[1-9][0-9]*)$")
_SIGNED_DECIMAL = re.compile(r"^-?(?:0|[1-9][0-9]*)$")
_HEX_DIGEST = re.compile(r"^[0-9a-fA-F]{64}$")


class HarnessError(RuntimeError):
    """Base class for all expected harness failures."""


class AuthorizationError(HarnessError):
    pass


class ArtifactError(HarnessError):
    pass


class ProtocolError(HarnessError):
    pass


class HarnessTimeout(HarnessError):
    pass


class SerialUnavailable(HarnessError):
    pass


class SerialOwnershipError(HarnessError):
    """A known Linux Secondary process may already own the serial port."""


@dataclass(frozen=True)
class Caps:
    version: int
    state_q15: int
    rate: int
    move_compat: int
    game_cal: int
    profiles: int
    max_ttl: int


@dataclass(frozen=True)
class Status:
    physical_session: int
    injection_session: int
    state: str
    source: str
    last_seq: int
    command_age_ms: int
    format_id: str
    cal_id: str
    synthetic_x: int
    synthetic_y: int
    physical_x: int
    physical_y: int
    merged_x: int
    merged_y: int
    override: int
    fault: int
    counters: Mapping[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class Ack:
    op: str
    physical_session: int
    injection_session: int
    mode: str
    format_id: str
    cal_id: str


@dataclass(frozen=True)
class Nack:
    op: str
    reason: str
    state: str


Response = Caps | Status | Ack | Nack


@dataclass(frozen=True)
class Artifact:
    label: str
    path: Path
    expected_sha256: str


@dataclass(frozen=True)
class ArtifactManifest:
    firmware_binary: Artifact
    local_entry: Artifact
    candidate: Artifact
    review: Artifact

    def items(self) -> tuple[Artifact, ...]:
        return (self.firmware_binary, self.local_entry,
                self.candidate, self.review)


@dataclass(frozen=True)
class ProbeResult:
    caps: Caps
    status: Status


@dataclass(frozen=True)
class ArmResult:
    caps: Caps
    status: Status
    physical_session: int
    injection_session: int


@dataclass(frozen=True)
class TeardownResult:
    """Result of the explicit, confirmed mouse teardown handshake."""

    caps: Caps
    status: Status
    output_ack: Ack
    final_status: Status

    @property
    def initial_status(self) -> Status:
        return self.status

    @property
    def ack(self) -> Ack:
        return self.output_ack

    @property
    def pre_status(self) -> Status:
        return self.status

    @property
    def post_status(self) -> Status:
        return self.final_status

    @property
    def mode(self) -> str:
        return self.output_ack.mode


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def _strip_wire_line(value: str | bytes) -> str:
    if isinstance(value, bytes):
        try:
            value = value.decode("ascii")
        except UnicodeDecodeError as exc:
            raise ProtocolError("response is not ASCII") from exc
    if not isinstance(value, str):
        raise ProtocolError("response must be text")
    # The firmware emits CRLF.  Only framing newlines are removed: whitespace
    # in fields is deliberately rejected by the exact parsers below.
    if value.endswith("\n"):
        value = value[:-1]
        if value.endswith("\r"):
            value = value[:-1]
    elif value.endswith("\r"):
        value = value[:-1]
    if not value:
        raise ProtocolError("empty response")
    return value


_PROMPT_PREFIX_BYTES = b">>> "
_PROMPT_PREFIX_TEXT = ">>> "


def _strip_prompt_prefix(value: str | bytes) -> str | bytes:
    """Remove only the firmware's exact, possibly repeated, prompt prefix."""
    prefix = _PROMPT_PREFIX_BYTES if isinstance(value, bytes) else _PROMPT_PREFIX_TEXT
    while value.startswith(prefix):
        value = value[len(prefix):]
    return value


def _button_stream_frame(value: str | bytes) -> tuple[int, bool] | None:
    """Return ``(mask, needs_crlf_tail)`` for an exact binary button frame.

    The legacy stream is ``km.`` + one binary mask byte + CRLF.  A mask of
    0x0a is itself pyserial's line delimiter, so ``readline`` returns the
    prefix through that byte and leaves the framing CRLF for the next read.
    No other ``km.*`` or binary line is tolerated here.
    """
    value = _strip_prompt_prefix(value)
    if isinstance(value, str):
        try:
            raw = value.encode("latin-1")
        except UnicodeEncodeError:
            return None
    elif isinstance(value, bytes):
        raw = value
    else:
        return None

    if raw == b"km.\n":
        return 0x0A, True
    if len(raw) == 6 and raw[:3] == b"km." and raw[4:] == b"\r\n":
        mask = raw[3]
        return (mask, False) if mask <= 0x1F else None
    if len(raw) == 5 and raw[:3] == b"km." and raw[4:] == b"\n":
        mask = raw[3]
        return (mask, False) if mask <= 0x1F else None
    return None


def _body(line: str, prefix: str) -> str:
    if not (line.startswith(prefix) and line.endswith(")")):
        raise ProtocolError(f"malformed {prefix.rstrip('(')} line")
    body = line[len(prefix):-1]
    if not body:
        raise ProtocolError(f"empty {prefix.rstrip('(')} body")
    return body


def _parse_uint(token: str, label: str) -> int:
    if not _DECIMAL.fullmatch(token):
        raise ProtocolError(f"{label} is not a decimal unsigned integer")
    return int(token, 10)


def _parse_sint(token: str, label: str) -> int:
    if not _SIGNED_DECIMAL.fullmatch(token):
        raise ProtocolError(f"{label} is not a decimal signed integer")
    return int(token, 10)


def _parse_token(token: str, label: str) -> str:
    if not _TOKEN.fullmatch(token):
        raise ProtocolError(f"{label} is malformed")
    return token


def parse_caps(line: str | bytes) -> Caps:
    """Parse the exact firmware ``STICK_CAPS`` response."""
    text = _strip_wire_line(line)
    fields = _body(text, "STICK_CAPS(").split(",")
    legacy = ("v", "state_q15", "rate", "move_compat", "profiles", "max_ttl")
    current = (
        "v", "state_q15", "rate", "move_compat", "game_cal", "profiles", "max_ttl",
    )
    expected = legacy if len(fields) == len(legacy) else current
    if len(fields) != len(expected):
        raise ProtocolError("STICK_CAPS field count mismatch")
    values: list[int] = []
    for item, key in zip(fields, expected):
        name, sep, value = item.partition("=")
        if sep != "=" or name != key:
            raise ProtocolError(f"STICK_CAPS expected {key}=...")
        values.append(_parse_uint(value, key))
    if expected == legacy:
        if values[0] != 1:
            raise ProtocolError("legacy STICK_CAPS shape requires v=1")
        values.insert(4, 0)  # v1 predates game calibration and implies disabled.
    elif values[0] != 2:
        raise ProtocolError("current STICK_CAPS shape requires v=2")
    return Caps(*values)


_STATUS_KEYS = (
    "cmd", "crc", "seq", "ttl", "profile", "modified", "center",
    "override", "sat", "guard", "decode", "stall", "fifo", "mode_switch",
    "reversal", "wrong", "source_conflict", "frame", "barrier", "target",
    "timing", "awaiting",
)
_STATES = frozenset(("raw_safe", "stick_arming", "stick_idle", "stick_active",
                     "physical_override", "expired", "fault", "hotplug"))
_SOURCES = frozenset(("none", "state_q15", "rate", "move_compat"))
_MODES = frozenset(("off", "mouse", "stick"))


def _split_pair(token: str, key: str, count: int, *, signed: bool = False) -> tuple[int, ...]:
    name, sep, raw = token.partition("=")
    if sep != "=" or name != key:
        raise ProtocolError(f"STICK_STATUS expected {key}=...")
    values = raw.split("/")
    if len(values) != count:
        raise ProtocolError(f"STICK_STATUS {key} arity mismatch")
    parse = _parse_sint if signed else _parse_uint
    return tuple(parse(item, f"{key}[{index}]") for index, item in enumerate(values))


def parse_status(line: str | bytes) -> Status:
    """Parse the complete, positional-plus-counter ``STICK_STATUS`` frame."""
    text = _strip_wire_line(line)
    fields = _body(text, "STICK_STATUS(").split(",")
    # v1 has 21 counters; v2 adds decode= after guard=.  Both layouts remain
    # exact so an OTA transition can inspect/teardown the old board without
    # accepting a truncated or shifted frame.
    if len(fields) not in (37, 38):
        raise ProtocolError(f"STICK_STATUS field count mismatch: {len(fields)}")
    physical = _parse_uint(fields[0], "physical_session")
    injection = _parse_uint(fields[1], "injection_session")
    state = _parse_token(fields[2], "state")
    source = _parse_token(fields[3], "source")
    if state not in _STATES:
        raise ProtocolError(f"unknown STICK_STATUS state {state!r}")
    if source not in _SOURCES:
        raise ProtocolError(f"unknown STICK_STATUS source {source!r}")
    last_seq = _parse_uint(fields[4], "last_seq")
    age = _parse_uint(fields[5], "command_age_ms")
    format_id = _parse_token(fields[6], "format_id")
    cal_id = _parse_token(fields[7], "cal_id")
    signed_values = [_parse_sint(fields[index], f"status[{index}]")
                     for index in range(8, 14)]
    override = _parse_uint(fields[14], "override")
    fault = _parse_uint(fields[15], "fault")
    if override not in (0, 1) or fault not in (0, 1):
        raise ProtocolError("STICK_STATUS override/fault must be 0 or 1")

    counters: dict[str, Any] = {}
    counters["cmd"] = _split_pair(fields[16], "cmd", 2)
    counters["crc"] = _parse_pair_single(fields[17], "crc")
    counters["seq"] = _split_pair(fields[18], "seq", 3)
    counters["ttl"] = _parse_pair_single(fields[19], "ttl")
    profile_name, profile_mismatch = _split_name_uint(fields[20], "profile")
    counters["profile"] = (profile_name, profile_mismatch)
    counters["modified"] = _parse_pair_single(fields[21], "modified")
    counters["center"] = _parse_pair_single(fields[22], "center")
    counters["override"] = _split_pair(fields[23], "override", 3)
    counters["sat"] = _parse_pair_single(fields[24], "sat")
    counters["guard"] = _parse_pair_single(fields[25], "guard")
    cursor = 26
    if len(fields) == 38:
        counters["decode"] = _parse_pair_single(fields[cursor], "decode")
        cursor += 1
    else:
        counters["decode"] = 0
    counters["stall"] = _parse_pair_single(fields[cursor], "stall")
    counters["fifo"] = _split_pair(fields[cursor + 1], "fifo", 2)
    counters["mode_switch"] = _parse_pair_single(fields[cursor + 2], "mode_switch")
    counters["reversal"] = _parse_pair_single(fields[cursor + 3], "reversal")
    counters["wrong"] = _split_pair(fields[cursor + 4], "wrong", 2)
    counters["source_conflict"] = _parse_pair_single(fields[cursor + 5], "source_conflict")
    counters["frame"] = _parse_pair_single(fields[cursor + 6], "frame")
    counters["barrier"] = _split_pair(fields[cursor + 7], "barrier", 3)
    counters["target"] = _split_pair(fields[cursor + 8], "target", 4)
    counters["timing"] = _split_pair(fields[cursor + 9], "timing", 5)
    counters["awaiting"] = _split_pair(fields[cursor + 10], "awaiting", 4)
    return Status(physical, injection, state, source, last_seq, age, format_id,
                  cal_id, *signed_values, override, fault, counters)


def _parse_pair_single(token: str, key: str) -> int:
    name, sep, raw = token.partition("=")
    if sep != "=" or name != key:
        raise ProtocolError(f"STICK_STATUS expected {key}=...")
    return _parse_uint(raw, key)


def _split_name_uint(token: str, key: str) -> tuple[str, int]:
    name, sep, raw = token.partition("=")
    if sep != "=" or name != key:
        raise ProtocolError(f"STICK_STATUS expected {key}=...")
    value = raw.split("/")
    if len(value) != 2:
        raise ProtocolError(f"STICK_STATUS {key} arity mismatch")
    profile = _parse_token(value[0], "profile name")
    return profile, _parse_uint(value[1], "profile mismatch")


def parse_ack(line: str | bytes) -> Ack:
    text = _strip_wire_line(line)
    fields = _body(text, "STICK_ACK(").split(",")
    if len(fields) != 6:
        raise ProtocolError("STICK_ACK field count mismatch")
    op = _parse_token(fields[0], "ACK operation")
    physical = _parse_uint(fields[1], "ACK physical_session")
    injection = _parse_uint(fields[2], "ACK injection_session")
    mode = _parse_token(fields[3], "ACK mode")
    if mode not in _MODES:
        raise ProtocolError(f"unknown ACK mode {mode!r}")
    format_id = _parse_token(fields[4], "ACK format")
    cal_id = _parse_token(fields[5], "ACK calibration")
    return Ack(op, physical, injection, mode, format_id, cal_id)


def parse_nack(line: str | bytes) -> Nack:
    text = _strip_wire_line(line)
    fields = _body(text, "STICK_NACK(").split(",")
    if len(fields) != 3:
        raise ProtocolError("STICK_NACK field count mismatch")
    op = _parse_token(fields[0], "NACK operation")
    reason = _parse_token(fields[1], "NACK reason")
    state = _parse_token(fields[2], "NACK state")
    if state not in _STATES:
        raise ProtocolError(f"unknown NACK state {state!r}")
    return Nack(op, reason, state)


def parse_response(line: str | bytes) -> Response:
    """Classify and strictly parse one response line."""
    text = _strip_wire_line(line)
    if text.startswith("STICK_CAPS("):
        return parse_caps(text)
    if text.startswith("STICK_STATUS("):
        return parse_status(text)
    if text.startswith("STICK_ACK("):
        return parse_ack(text)
    if text.startswith("STICK_NACK("):
        return parse_nack(text)
    raise ProtocolError(f"unexpected response line: {text!r}")


# Descriptive aliases keep the pure parser convenient for small host tests and
# make the wire envelope names explicit at call sites.
parse_stick_caps = parse_caps
parse_stick_status = parse_status
parse_stick_ack = parse_ack
parse_stick_nack = parse_nack
parse_line = parse_response


def is_control_command(command: str) -> bool:
    """Return whether *command* is a supported control-plane command."""
    if not isinstance(command, str) or command != command.strip() or not command:
        return False
    if command in (CAPS_COMMAND, STATUS_COMMAND, OUTPUT_MOUSE_COMMAND,
                   OUTPUT_STICK_COMMAND):
        return True
    if command.startswith("km.stick.begin(") and command.endswith(")"):
        # The state machine applies the exact format/cal/source checks; this
        # helper only distinguishes control from data-plane traffic.
        return True
    return False


def assert_control_command(command: str) -> None:
    if any(command.startswith(prefix) for prefix in DATA_PLANE_PREFIXES):
        raise ProtocolError("data-plane commands are forbidden by DS-03 harness")
    if not is_control_command(command):
        raise ProtocolError(f"unsupported harness command: {command!r}")


class Transport(Protocol):
    def write(self, command: str) -> Any: ...

    def readline(self, timeout: float | None = None) -> str | bytes | None: ...

    def close(self) -> Any: ...


class HandshakeStateMachine:
    """Pure command/response state machine used by both probe and arm.

    ``send`` records the exact command that the transport is about to emit;
    ``receive`` parses and validates the next response.  A response kind may
    be consumed only once, and a response for a later/earlier phase is
    rejected as stale or unexpected.
    """

    def __init__(self, mode: str, *, expected_format: str = EXPECTED_FORMAT,
                 expected_cal: str = EXPECTED_CAL) -> None:
        if mode not in ("probe", "arm", "teardown"):
            raise ValueError("mode must be probe, arm, or teardown")
        self.mode = mode
        self.expected_format = expected_format
        self.expected_cal = expected_cal
        self.phase = "start"
        # Caps/status are singleton frames.  ACKs are keyed by operation so a
        # valid begin ACK followed by a valid output_mode ACK is not mistaken
        # for a duplicate merely because both share the ACK envelope.
        self.seen: set[tuple[str, str]] = set()
        self.caps: Caps | None = None
        self.status: Status | None = None
        self.begin_ack: Ack | None = None
        self.output_ack: Ack | None = None
        self.final_status: Status | None = None

    def send(self, command: str) -> None:
        assert_control_command(command)
        if self.phase == "start" and command == CAPS_COMMAND:
            self.phase = "wait_caps"
        elif self.phase == "caps_ready" and command == STATUS_COMMAND:
            self.phase = "wait_status"
        elif self.mode == "arm" and self.phase == "status_ready":
            expected = f"km.stick.begin({self.expected_format},{self.expected_cal},state_q15)"
            if command != expected:
                raise ProtocolError("arm begin command does not match exact profile")
            self.phase = "wait_begin"
        elif self.mode == "arm" and self.phase == "begin_ready" and command == OUTPUT_STICK_COMMAND:
            self.phase = "wait_output"
        elif (self.mode == "teardown" and self.phase == "status_ready" and
              command == OUTPUT_MOUSE_COMMAND):
            self.phase = "wait_output"
        elif (self.mode == "teardown" and self.phase == "output_ready" and
              command == STATUS_COMMAND):
            self.phase = "wait_final_status"
        elif self.phase in ("complete", "failed"):
            raise ProtocolError("handshake is already complete")
        else:
            raise ProtocolError(f"unexpected command {command!r} in phase {self.phase}")

    def receive(self, line: str | bytes) -> Response:
        response = parse_response(line)
        kind = type(response).__name__
        # A teardown intentionally consumes two status frames.  Keep duplicate
        # detection phase-aware for that mode while preserving the singleton
        # semantics used by the probe/arm handshakes.
        if self.mode == "teardown" and isinstance(response, Status):
            if self.phase in ("wait_status", "status_ready"):
                status_phase = "initial"
            elif self.phase in ("wait_final_status", "complete"):
                status_phase = "final"
            else:
                status_phase = self.phase
            identity = (kind, status_phase)
        else:
            identity = (kind, response.op if isinstance(response, (Ack, Nack)) else kind)
        if identity in self.seen:
            raise ProtocolError(f"duplicate/stale {kind} response")
        self.seen.add(identity)
        if isinstance(response, Nack):
            self.phase = "failed"
            raise ProtocolError(
                f"device NACK {response.op}: {response.reason} ({response.state})"
            )
        if self.phase == "wait_caps" and isinstance(response, Caps):
            self.caps = response
            self.phase = "caps_ready"
            return response
        if self.phase == "wait_status" and isinstance(response, Status):
            self.status = response
            if self.mode == "probe":
                self.phase = "complete"
            else:
                self.phase = "status_ready"
            return response
        if self.phase == "wait_begin" and isinstance(response, Ack):
            if response.op != "begin":
                raise ProtocolError(f"unexpected ACK operation {response.op!r} while waiting begin")
            self.begin_ack = response
            self.phase = "begin_ready"
            return response
        if self.phase == "wait_output" and isinstance(response, Ack):
            if response.op != "output_mode":
                raise ProtocolError(f"unexpected ACK operation {response.op!r} while waiting output_mode")
            self.output_ack = response
            self.phase = "output_ready" if self.mode == "teardown" else "complete"
            return response
        if (self.mode == "teardown" and self.phase == "wait_final_status" and
                isinstance(response, Status)):
            self.final_status = response
            self.phase = "complete"
            return response
        self.phase = "failed"
        raise ProtocolError(f"unexpected {kind} response in phase {self.phase}")


class AuditTranscript:
    """Timestamped JSON transcript with atomic final publication."""

    def __init__(self, path: Path | None, *, port: str | None, baud: int,
                 artifacts: ArtifactManifest | None = None) -> None:
        if path is None:
            stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            path = DEFAULT_AUDIT_DIR / f"stick_phase3_{stamp}.json"
        self.path = Path(path)
        self.started_at = _utc_now()
        self.port = port
        self.baud = baud
        self.artifacts = artifacts
        self.events: list[dict[str, Any]] = []
        self.outcome: str = "in_progress"
        self.error: str | None = None

    def record(self, direction: str, line: str, *, parsed: str | None = None) -> None:
        if direction not in ("command", "response", "event"):
            raise ValueError("invalid transcript direction")
        event: dict[str, Any] = {
            "timestamp_utc": _utc_now(),
            "direction": direction,
            "line": str(line),
        }
        if parsed is not None:
            event["parsed"] = parsed
        self.events.append(event)

    def finish(self, outcome: str, error: str | None = None) -> None:
        self.outcome = outcome
        self.error = error
        self.write_atomic()

    def payload(self) -> dict[str, Any]:
        artifact_data: dict[str, Any] = {}
        if self.artifacts is not None:
            for artifact in self.artifacts.items():
                artifact_data[artifact.label] = {
                    "path": str(artifact.path),
                    "expected_sha256": artifact.expected_sha256.lower(),
                }
        result: dict[str, Any] = {
            "schema": "makcm.stick-phase3-transcript.v1",
            "started_at_utc": self.started_at,
            "finished_at_utc": _utc_now(),
            "port": self.port,
            "baud": self.baud,
            "artifacts": artifact_data,
            "events": self.events,
            "outcome": self.outcome,
        }
        if self.error:
            result["error"] = self.error
        return result

    def write_atomic(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        payload = json.dumps(self.payload(), indent=2, sort_keys=True) + "\n"
        temporary: Path | None = None
        try:
            with tempfile.NamedTemporaryFile(
                "w", encoding="utf-8", newline="\n", dir=self.path.parent,
                prefix=f".{self.path.name}.", suffix=".tmp", delete=False,
            ) as handle:
                temporary = Path(handle.name)
                handle.write(payload)
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(temporary, self.path)
        except BaseException:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
            raise


def _running_secondary_processes() -> tuple[tuple[str, int | None], ...]:
    """Return known MAKCM Secondary processes currently present on Linux.

    ``pgrep -x`` gives us the executable *comm* name and PID without reading
    command-line arguments or environment variables.  A non-zero result other
    than the normal "no match" code is treated as an inability to establish
    ownership and therefore fails closed for callers that do not explicitly
    opt into the manual override.
    """
    if not _is_linux_platform():
        # Windows has no Linux process-ownership signal; do not pretend that
        # this check applies there.
        return ()

    found: list[tuple[str, int | None]] = []
    for process_name in KNOWN_SECONDARY_PROCESS_NAMES:
        try:
            result = subprocess.run(
                ["pgrep", "-x", process_name],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                check=False,
                text=True,
            )
        except OSError as exc:
            raise SerialOwnershipError(
                "cannot inspect Linux Secondary process ownership"
            ) from exc

        return_code = getattr(result, "returncode", None)
        if return_code == 1:
            continue  # pgrep's normal no-match result.
        if return_code != 0:
            raise SerialOwnershipError(
                "Linux Secondary process ownership check failed"
            )

        raw_output = getattr(result, "stdout", "") or ""
        if isinstance(raw_output, bytes):
            raw_output = raw_output.decode("ascii", errors="ignore")
        pids: list[int] = []
        for token in str(raw_output).split():
            # Only numeric PIDs are retained; arbitrary subprocess output is
            # deliberately never copied into the audit transcript.
            if token.isdecimal():
                pids.append(int(token, 10))
        if pids:
            found.extend((process_name, pid) for pid in pids)
        else:
            # A process can exit between pgrep's match and stdout collection.
            # Keep the name as a conservative ownership hit while avoiding a
            # fabricated PID.
            found.append((process_name, None))
    return tuple(found)


def _serial_process_guard(
    transcript: AuditTranscript,
    *,
    allow_active_secondary_process: bool,
) -> None:
    """Guard a real Linux serial open against known Secondary owners."""
    if not _is_linux_platform():
        if allow_active_secondary_process:
            transcript.record(
                "event",
                "serial_process_guard:manual_override "
                "flag=--allow-active-secondary-process "
                "skipped_non_linux=true no_exclusivity_guarantee=true",
            )
            return
        transcript.record("event", "serial_process_guard:skipped_non_posix")
        return

    try:
        processes = _running_secondary_processes()
    except SerialOwnershipError as exc:
        if allow_active_secondary_process:
            transcript.record(
                "event",
                "serial_process_guard:manual_override "
                "flag=--allow-active-secondary-process "
                "detection_unavailable=true no_exclusivity_guarantee=true",
            )
            return
        transcript.record(
            "event",
            "serial_process_guard:blocked detection_unavailable=true",
        )
        raise

    if not processes:
        transcript.record("event", "serial_process_guard:clear")
        return

    details = ",".join(
        f"{name}(pid={pid if pid is not None else 'unknown'})"
        for name, pid in processes
    )
    if allow_active_secondary_process:
        transcript.record(
            "event",
            "serial_process_guard:manual_override "
            "flag=--allow-active-secondary-process "
            f"processes={details} no_exclusivity_guarantee=true",
        )
        return

    error = SerialOwnershipError(
        "serial ownership guard blocked known MAKCM Secondary process(es): "
        f"{details}; stop them or pass --allow-active-secondary-process "
        "for a manually controlled diagnostic run"
    )
    transcript.record(
        "event", f"serial_process_guard:blocked processes={details}"
    )
    raise error


def _normalise_digest(value: str, label: str) -> str:
    if not isinstance(value, str) or not _HEX_DIGEST.fullmatch(value):
        raise ArtifactError(f"{label} must be a 64-character SHA-256 digest")
    return value.lower()


def verify_artifact(artifact: Artifact) -> str:
    expected = _normalise_digest(artifact.expected_sha256, f"{artifact.label} hash")
    try:
        digest = hashlib.sha256(artifact.path.read_bytes()).hexdigest()
    except OSError as exc:
        raise ArtifactError(f"cannot read {artifact.label} {artifact.path}: {exc}") from exc
    if digest != expected:
        raise ArtifactError(
            f"{artifact.label} hash mismatch: expected {expected}, got {digest}"
        )
    return digest


def verify_artifacts(manifest: ArtifactManifest) -> dict[str, str]:
    """Verify every DS-03 artifact before a serial transport is opened."""
    result: dict[str, str] = {}
    for artifact in manifest.items():
        result[artifact.label] = verify_artifact(artifact)
    return result


def _safe_status(status: Status) -> None:
    failures: list[str] = []
    if status.physical_session == 0:
        failures.append("physical session is zero")
    if status.injection_session != 0:
        failures.append("injection session is already active")
    if status.state != "raw_safe":
        failures.append(f"state={status.state}")
    if status.source != "none":
        failures.append(f"source={status.source}")
    # The reviewed local profile is installed before the begin barrier.  A
    # pre-arm status carrying ``none`` would mean the board is not configured
    # for this exact candidate and must not be armed.
    if (status.format_id != EXPECTED_FORMAT or
            status.cal_id != EXPECTED_CAL):
        failures.append("profile/calibration does not match expected candidate")
    if status.synthetic_x != 0 or status.synthetic_y != 0:
        failures.append("synthetic state is non-zero")
    if status.merged_x != 0 or status.merged_y != 0:
        failures.append("merged state is non-zero")
    if status.override != 0 or status.fault != 0:
        failures.append("override/fault is active")
    if failures:
        raise ProtocolError("unsafe STICK_STATUS: " + "; ".join(failures))


validate_safe_status = _safe_status


def _validate_caps(caps: Caps) -> None:
    """Require the exact DS-03 control-plane capability profile."""
    if (caps.version not in (1, 2) or caps.state_q15 != 1 or caps.rate != 0 or
            caps.move_compat != 0 or caps.game_cal != 0 or caps.profiles != 1):
        raise ProtocolError(
            "caps mismatch: require v=1|2,state_q15=1,rate=0,"
            "move_compat=0,game_cal=0,profiles=1"
        )


def _teardown_initial_status(status: Status) -> None:
    """Validate identity/profile fields before a mouse teardown write."""
    failures: list[str] = []
    if status.physical_session == 0:
        failures.append("physical session is zero")
    if (status.format_id != EXPECTED_FORMAT or
            status.cal_id != EXPECTED_CAL):
        failures.append("profile/calibration does not match expected candidate")
    if failures:
        raise ProtocolError("unsafe teardown pre-status: " + "; ".join(failures))


def _teardown_final_status(status: Status, *, physical_session: int) -> None:
    """Require proof that the mouse barrier left the board raw and safe."""
    failures: list[str] = []
    if status.physical_session != physical_session or physical_session == 0:
        failures.append("physical session does not match teardown ACK")
    if status.injection_session != 0:
        failures.append("injection session is still active")
    if status.state != "raw_safe":
        failures.append(f"state={status.state}")
    if status.source != "none":
        failures.append(f"source={status.source}")
    if (status.format_id != EXPECTED_FORMAT or
            status.cal_id != EXPECTED_CAL):
        failures.append("profile/calibration does not match expected candidate")
    if status.synthetic_x != 0 or status.synthetic_y != 0:
        failures.append("synthetic state is non-zero")
    if status.merged_x != 0 or status.merged_y != 0:
        failures.append("merged state is non-zero")
    if status.override != 0 or status.fault != 0:
        failures.append("override/fault is active")
    if failures:
        raise ProtocolError("unsafe teardown final status: " + "; ".join(failures))


def _write(transport: Transport, command: str, transcript: AuditTranscript) -> None:
    assert_control_command(command)
    transcript.record("command", command)
    transport.write(command)


def _readline(transport: Transport, timeout: float) -> str | bytes | None:
    try:
        return transport.readline(timeout)
    except TypeError:
        # Small test doubles commonly expose readline() without a timeout.
        return transport.readline()  # type: ignore[call-arg]


def _wait_response(transport: Transport, machine: HandshakeStateMachine,
                   transcript: AuditTranscript, timeout: float) -> Response:
    deadline = time.monotonic() + max(0.0, timeout)
    swallow_button_crlf_tail = False
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise HarnessTimeout(f"timeout waiting in phase {machine.phase}")
        raw = _readline(transport, remaining)
        if raw is None or raw == b"" or raw == "":
            # ``None``/empty bytes are timeout indications from common mocks and
            # pyserial.  Keep waiting until the bounded deadline.
            time.sleep(min(0.005, max(0.0, remaining)))
            continue
        if swallow_button_crlf_tail and raw in (b"\r\n", "\r\n"):
            transcript.record("event", "button_stream:ignored trailing_crlf")
            swallow_button_crlf_tail = False
            continue
        swallow_button_crlf_tail = False
        button_frame = _button_stream_frame(raw)
        if button_frame is not None:
            mask, swallow_button_crlf_tail = button_frame
            transcript.record("event", f"button_stream:ignored mask={mask}")
            continue
        stripped = _strip_prompt_prefix(raw)
        if stripped in (b"", "") and stripped != raw:
            transcript.record("event", "button_stream:ignored prompt")
            continue
        try:
            text = _strip_wire_line(stripped)
        except ProtocolError:
            transcript.record("response", repr(raw))
            raise
        transcript.record("response", text)
        response = machine.receive(text)
        # Add a small parsed tag without duplicating response contents.
        transcript.events[-1]["parsed"] = type(response).__name__
        return response


def _exchange(transport: Transport, machine: HandshakeStateMachine,
              command: str, transcript: AuditTranscript, timeout: float) -> Response:
    machine.send(command)
    _write(transport, command, transcript)
    return _wait_response(transport, machine, transcript, timeout)


def run_probe(transport: Transport, *, port: str | None = None,
              baud: int = DEFAULT_BAUD, timeout: float = DEFAULT_TIMEOUT,
              audit_path: Path | None = None,
              transcript: AuditTranscript | None = None) -> ProbeResult:
    """Run the read-only probe over an already-created transport."""
    transcript = transcript or AuditTranscript(audit_path, port=port, baud=baud)
    machine = HandshakeStateMachine("probe")
    try:
        caps = _exchange(transport, machine, CAPS_COMMAND, transcript, timeout)
        status = _exchange(transport, machine, STATUS_COMMAND, transcript, timeout)
        assert isinstance(caps, Caps) and isinstance(status, Status)
        result = ProbeResult(caps, status)
        transcript.finish("success")
        return result
    except Exception as exc:
        transcript.finish("failure", str(exc))
        if isinstance(exc, HarnessError):
            raise
        raise HarnessError(str(exc)) from exc


def run_arm(transport: Transport, *, allow_stick_output: bool,
            artifacts: ArtifactManifest | None = None, port: str | None = None,
            baud: int = DEFAULT_BAUD, timeout: float = DEFAULT_TIMEOUT,
            audit_path: Path | None = None,
            artifacts_verified: bool = False,
            transcript: AuditTranscript | None = None) -> ArmResult:
    """Run the bounded DS-03 arm handshake over an existing transport.

    Callers that create the transport themselves must verify artifacts before
    opening it; ``artifacts_verified`` is an explicit assertion for that case.
    The CLI uses :func:`run_arm_on_port`, which enforces this ordering itself.
    """
    transcript = transcript or AuditTranscript(audit_path, port=port, baud=baud,
                                               artifacts=artifacts)
    machine = HandshakeStateMachine("arm")
    if not allow_stick_output:
        error = AuthorizationError("arm requires --allow-stick-output")
        transcript.finish("failure", str(error))
        raise error
    if artifacts is not None and not artifacts_verified:
        try:
            verify_artifacts(artifacts)
        except Exception as exc:
            transcript.finish("failure", str(exc))
            raise
    try:
        caps = _exchange(transport, machine, CAPS_COMMAND, transcript, timeout)
        assert isinstance(caps, Caps)
        _validate_caps(caps)

        status = _exchange(transport, machine, STATUS_COMMAND, transcript, timeout)
        assert isinstance(status, Status)
        _safe_status(status)

        begin_command = f"km.stick.begin({EXPECTED_FORMAT},{EXPECTED_CAL},state_q15)"
        begin = _exchange(transport, machine, begin_command, transcript, timeout)
        assert isinstance(begin, Ack)
        if (begin.op != "begin" or begin.physical_session != status.physical_session or
                begin.physical_session == 0 or begin.injection_session == 0 or
                begin.mode != "mouse" or begin.format_id != EXPECTED_FORMAT or
                begin.cal_id != EXPECTED_CAL):
            raise ProtocolError("begin ACK session/mode/format mismatch")

        output = _exchange(transport, machine, OUTPUT_STICK_COMMAND,
                           transcript, timeout)
        assert isinstance(output, Ack)
        if (output.op != "output_mode" or
                output.physical_session != begin.physical_session or
                output.injection_session != begin.injection_session or
                output.physical_session == 0 or output.injection_session == 0 or
                output.mode != "stick" or output.format_id != EXPECTED_FORMAT or
                output.cal_id != EXPECTED_CAL):
            raise ProtocolError("output_mode ACK session/mode/format mismatch")

        result = ArmResult(caps, status, begin.physical_session,
                           begin.injection_session)
        transcript.finish("success")
        return result
    except Exception as exc:
        # Once serial is open, always attempt to leave the board in mouse mode
        # on every arm failure.  This write is deliberately not awaited: the
        # rollback itself must not turn a bounded failure into an unbounded one.
        try:
            _write(transport, OUTPUT_MOUSE_COMMAND, transcript)
        except Exception as rollback_exc:
            transcript.record("event", f"rollback_failed:{rollback_exc}")
        transcript.finish("failure", str(exc))
        if isinstance(exc, HarnessError):
            raise
        raise HarnessError(str(exc)) from exc


def run_teardown(transport: Transport, *, port: str | None = None,
                 baud: int = DEFAULT_BAUD, timeout: float = DEFAULT_TIMEOUT,
                 audit_path: Path | None = None,
                 transcript: AuditTranscript | None = None) -> TeardownResult:
    """Explicitly return an armed board to confirmed mouse/raw-safe mode.

    This path is intentionally independent from :func:`run_arm`: it never
    sends a best-effort rollback, and it never emits a data-plane command.  A
    successful result requires the exact delayed ``output_mode(mouse)`` ACK
    followed by a fresh status frame proving that injection has disarmed.
    """
    transcript = transcript or AuditTranscript(audit_path, port=port, baud=baud)
    machine = HandshakeStateMachine("teardown")
    try:
        caps = _exchange(transport, machine, CAPS_COMMAND, transcript, timeout)
        assert isinstance(caps, Caps)
        _validate_caps(caps)

        status = _exchange(transport, machine, STATUS_COMMAND, transcript, timeout)
        assert isinstance(status, Status)
        _teardown_initial_status(status)

        output = _exchange(transport, machine, OUTPUT_MOUSE_COMMAND,
                           transcript, timeout)
        assert isinstance(output, Ack)
        if (output.op != "output_mode" or
                output.physical_session != status.physical_session or
                output.physical_session == 0 or output.injection_session != 0 or
                output.mode != "mouse" or
                output.format_id != EXPECTED_FORMAT or
                output.cal_id != EXPECTED_CAL):
            raise ProtocolError("output_mode(mouse) ACK session/mode/format mismatch")

        final_status = _exchange(transport, machine, STATUS_COMMAND,
                                 transcript, timeout)
        assert isinstance(final_status, Status)
        _teardown_final_status(
            final_status, physical_session=output.physical_session,
        )

        result = TeardownResult(caps, status, output, final_status)
        transcript.finish("success")
        return result
    except Exception as exc:
        # There is deliberately no automatic retry or rollback here.  The
        # command was already the fail-safe transition; an ACK/status failure
        # must be surfaced and audited instead of being hidden by a
        # best-effort write that could itself be stale.
        transcript.finish("failure", str(exc))
        if isinstance(exc, HarnessError):
            raise
        raise HarnessError(str(exc)) from exc


class SerialTransport:
    """Small text-line adapter around an optional pyserial instance."""

    def __init__(self, port: str, baud: int, timeout: float) -> None:
        try:
            import serial  # type: ignore
        except ImportError as exc:
            raise SerialUnavailable(
                "pyserial is required for live serial operations"
            ) from exc
        kwargs: dict[str, Any] = {
            # Construct closed so the modem-control lines can be forced low
            # before opening the device.  Opening in Serial(...) would let
            # pyserial toggle DTR/RTS as part of its constructor.
            "port": None,
            "baudrate": baud,
            "timeout": timeout,
        }
        if _is_posix_platform():
            # pyserial maps this to a non-blocking flock where supported.  It
            # prevents a second Linux process from silently interleaving wire
            # traffic with the bounded phase-3 handshake.
            kwargs["exclusive"] = True
        serial_instance: Any | None = None
        try:
            serial_instance = serial.Serial(**kwargs)
            # These assignments happen while the port is still closed.  Keep
            # them before assigning ``port``/calling ``open`` so no modem
            # control transition can leak into the live device.
            serial_instance.dtr = False
            serial_instance.rts = False
            serial_instance.port = port
            serial_instance.open()
            serial_instance.reset_input_buffer()
            serial_instance.reset_output_buffer()
        except Exception as exc:
            # A partially configured/open transport must never escape.  Close
            # best-effort, preserving the original failure as the diagnostic.
            if serial_instance is not None:
                try:
                    serial_instance.close()
                except Exception:
                    pass
            raise HarnessError(f"cannot open serial port {port!r}: {exc}") from exc
        self._serial = serial_instance

    def write(self, command: str) -> int:
        # Firmware's line reader accepts CRLF; the pure transport contract
        # intentionally exposes bare command strings to test doubles.
        return int(self._serial.write((command + "\r\n").encode("ascii")))

    def readline(self, timeout: float | None = None) -> bytes:
        if timeout is None:
            return self._serial.readline()
        # pyserial's timeout is mutable and this avoids a second reader thread.
        previous = getattr(self._serial, "timeout", None)
        try:
            self._serial.timeout = timeout
            return self._serial.readline()
        finally:
            try:
                self._serial.timeout = previous
            except Exception:
                pass

    def close(self) -> None:
        self._serial.close()


def _serial_factory(port: str, baud: int, timeout: float) -> Transport:
    return SerialTransport(port, baud, timeout)


def run_probe_on_port(port: str, *, baud: int = DEFAULT_BAUD,
                      timeout: float = DEFAULT_TIMEOUT,
                      audit_path: Path | None = None,
                      transport_factory: Callable[[str, int, float], Transport] = _serial_factory,
                      allow_active_secondary_process: bool = False) -> ProbeResult:
    if not port:
        raise HarnessError("a serial port is required; no auto-selection is performed")
    transcript = AuditTranscript(audit_path, port=port, baud=baud)
    try:
        _serial_process_guard(
            transcript,
            allow_active_secondary_process=allow_active_secondary_process,
        )
        transport = transport_factory(port, baud, timeout)
    except Exception as exc:
        transcript.finish("failure", str(exc))
        raise
    try:
        return run_probe(transport, port=port, baud=baud, timeout=timeout,
                         audit_path=audit_path, transcript=transcript)
    finally:
        try:
            transport.close()
        except Exception:
            pass


def run_arm_on_port(port: str, *, allow_stick_output: bool,
                    artifacts: ArtifactManifest, baud: int = DEFAULT_BAUD,
                    timeout: float = DEFAULT_TIMEOUT,
                    audit_path: Path | None = None,
                    transport_factory: Callable[[str, int, float], Transport] = _serial_factory,
                    allow_active_secondary_process: bool = False) -> ArmResult:
    if not port:
        raise HarnessError("a serial port is required; no auto-selection is performed")
    transcript = AuditTranscript(audit_path, port=port, baud=baud,
                                 artifacts=artifacts)
    if not allow_stick_output:
        # Authorization is checked before both artifact work and transport
        # creation; an accidental invocation cannot touch a board.
        error = AuthorizationError("arm requires --allow-stick-output")
        transcript.finish("failure", str(error))
        raise error
    # This is intentionally before transport_factory: a hash mismatch cannot
    # even open the serial device.
    try:
        verify_artifacts(artifacts)
        _serial_process_guard(
            transcript,
            allow_active_secondary_process=allow_active_secondary_process,
        )
        transport = transport_factory(port, baud, timeout)
    except Exception as exc:
        transcript.finish("failure", str(exc))
        raise
    try:
        return run_arm(transport, allow_stick_output=allow_stick_output,
                       artifacts=artifacts, port=port, baud=baud,
                       timeout=timeout, audit_path=audit_path,
                       artifacts_verified=True, transcript=transcript)
    finally:
        try:
            transport.close()
        except Exception:
            pass


def run_teardown_on_port(
    port: str, *, baud: int = DEFAULT_BAUD,
    timeout: float = DEFAULT_TIMEOUT, audit_path: Path | None = None,
    transport_factory: Callable[[str, int, float], Transport] = _serial_factory,
    allow_active_secondary_process: bool = False,
) -> TeardownResult:
    """Run the explicit teardown while retaining the normal port guards."""
    if not port:
        raise HarnessError("a serial port is required; no auto-selection is performed")
    transcript = AuditTranscript(audit_path, port=port, baud=baud)
    try:
        _serial_process_guard(
            transcript,
            allow_active_secondary_process=allow_active_secondary_process,
        )
        transport = transport_factory(port, baud, timeout)
    except Exception as exc:
        transcript.finish("failure", str(exc))
        raise
    try:
        return run_teardown(transport, port=port, baud=baud, timeout=timeout,
                            audit_path=audit_path, transcript=transcript)
    finally:
        try:
            transport.close()
        except Exception:
            pass


def list_ports() -> list[str]:
    try:
        from serial.tools import list_ports as serial_list_ports  # type: ignore
    except ImportError as exc:
        raise SerialUnavailable("pyserial is required for --list-ports") from exc
    return [str(item.device) for item in serial_list_ports.comports()]


def _artifact_from_args(args: argparse.Namespace, name: str, path_name: str,
                        hash_name: str) -> Artifact:
    path = getattr(args, path_name, None)
    digest = getattr(args, hash_name, None)
    if path is None or digest is None:
        raise ArtifactError(f"--{path_name.replace('_', '-')} and --{hash_name.replace('_', '-')} are required")
    return Artifact(name, Path(path), str(digest))


def _manifest_from_args(args: argparse.Namespace) -> ArtifactManifest:
    return ArtifactManifest(
        _artifact_from_args(args, "firmware_binary", "firmware_binary", "firmware_sha256"),
        _artifact_from_args(args, "local_entry", "local_entry", "local_entry_sha256"),
        _artifact_from_args(args, "candidate", "candidate", "candidate_sha256"),
        _artifact_from_args(args, "review", "review", "review_sha256"),
    )


def _add_common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--port", help="explicit serial port (never auto-selected)")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument("--audit", "--audit-path", "--transcript", "--transcript-path",
                        dest="audit_path", type=Path)
    parser.add_argument(
        "--allow-active-secondary-process",
        action="store_true",
        help=(
            "MANUAL/UNSAFE: bypass the Linux MAKCM Secondary ownership guard; "
            "this does not guarantee serial exclusivity and is only for a "
            "controlled diagnostic run (recorded in the audit transcript)"
        ),
    )


def _add_artifacts(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--firmware-binary", "--firmware", "--firmware-bin",
                        dest="firmware_binary", type=Path)
    parser.add_argument("--firmware-sha256", "--firmware-sha", "--firmware-hash", "--firmware-bin-sha256",
                        dest="firmware_sha256")
    parser.add_argument("--local-entry", "--local-entry-file", "--entry",
                        dest="local_entry", type=Path)
    parser.add_argument("--local-entry-sha256", "--local-entry-sha", "--local-entry-hash", "--entry-sha256",
                        dest="local_entry_sha256")
    parser.add_argument("--candidate", "--candidate-json", dest="candidate", type=Path)
    parser.add_argument("--candidate-sha256", "--candidate-sha", "--candidate-hash", "--candidate-json-sha256",
                        dest="candidate_sha256")
    parser.add_argument("--review", "--review-json", dest="review", type=Path)
    parser.add_argument("--review-sha256", "--review-sha", "--review-hash", "--review-json-sha256",
                        dest="review_sha256")


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list-ports", action="store_true",
                        help="list serial ports and exit")
    subparsers = parser.add_subparsers(dest="command")
    probe = subparsers.add_parser("probe", help="read-only caps/status probe")
    _add_common(probe)
    arm = subparsers.add_parser("arm", help="bounded phase-3 arm handshake")
    _add_common(arm)
    arm.add_argument("--allow-stick-output", action="store_true",
                     help="explicitly authorize switching output_mode(stick)")
    _add_artifacts(arm)
    teardown = subparsers.add_parser(
        "teardown", help="confirmed output_mode(mouse) teardown"
    )
    _add_common(teardown)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)
    if args.list_ports:
        try:
            for port in list_ports():
                print(port)
            return 0
        except HarnessError as exc:
            print(f"FAIL: {exc}")
            return 2
    if args.command is None:
        parser.error("choose probe, arm, teardown, or --list-ports")
    if not args.port:
        print("FAIL: --port is required; no auto-selection is performed")
        return 2
    if args.baud <= 0 or args.timeout <= 0:
        print("FAIL: baud and timeout must be positive")
        return 2
    try:
        if args.command == "probe":
            result = run_probe_on_port(args.port, baud=args.baud,
                                       timeout=args.timeout,
                                       audit_path=args.audit_path,
                                       allow_active_secondary_process=(
                                           args.allow_active_secondary_process
                                       ))
            print(json.dumps({"caps": result.caps.__dict__,
                              "status": result.status.__dict__},
                             sort_keys=True))
            return 0
        if args.command == "teardown":
            result = run_teardown_on_port(
                args.port, baud=args.baud, timeout=args.timeout,
                audit_path=args.audit_path,
                allow_active_secondary_process=(
                    args.allow_active_secondary_process
                ),
            )
            print(json.dumps({
                "physical_session": result.final_status.physical_session,
                "injection_session": result.final_status.injection_session,
                "mode": result.output_ack.mode,
                "state": result.final_status.state,
                "source": result.final_status.source,
                "format": result.final_status.format_id,
                "cal": result.final_status.cal_id,
            }, sort_keys=True))
            return 0
        if not args.allow_stick_output:
            print("FAIL: arm requires --allow-stick-output")
            return 2
        manifest = _manifest_from_args(args)
        result = run_arm_on_port(
            args.port, allow_stick_output=args.allow_stick_output,
            artifacts=manifest, baud=args.baud, timeout=args.timeout,
            audit_path=args.audit_path,
            allow_active_secondary_process=args.allow_active_secondary_process,
        )
        print(json.dumps({"physical_session": result.physical_session,
                          "injection_session": result.injection_session,
                          "format": EXPECTED_FORMAT, "cal": EXPECTED_CAL},
                         sort_keys=True))
        return 0
    except HarnessError as exc:
        print(f"FAIL: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
