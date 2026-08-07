#!/usr/bin/env python3
"""Bounded DS-04 phase-3 ``state_q15`` data-plane harness.

The protocol/state machine remains independent of ``pyserial`` for deterministic
CI.  A live port is opened only through :func:`run_ds04_on_port`, after both
authorisation flags, exact artifact hashes and the Linux process-ownership
guard pass.  The control-plane parser, artifact types and status parser are
shared with :mod:`stick_phase3_harness`.

The gate is deliberately narrow:

* two independent authorisation flags are required before a write;
* the only data-plane operation is ``state_q15``;
* every command is bounded to a radial Q15 magnitude of 0.20 and TTL 16..80;
* sequence numbers are fresh, strictly increasing uint32 values;
* a non-zero command must be followed by an explicit zero command;
* every accepted command is checked against a successful USB-completion
  timestamp on the board's own clock; and
* ``km.output_mode(mouse)`` is attempted in ``finally`` and the result is
  recorded in an atomic JSON audit transcript.

There is no port discovery or implicit live mode.  The CLI requires an explicit
port, explicit smoke/full plan, both authorisation flags and all artifact hashes.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field, replace
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import tempfile
import time
from typing import Any, Callable, Iterable, Mapping, Protocol, Sequence

try:  # package-style import when tools is imported as a package
    from . import stick_phase3_harness as _p3  # type: ignore
except ImportError:  # direct ``sys.path.insert(.../tools)`` test imports
    import stick_phase3_harness as _p3  # type: ignore


# Public gate constants.  Keep the names explicit so an audit or a test can
# never accidentally inherit a future production cap.
Q15_SCALE = 32767
Q15_MAX_NORM = 0.20
PHASE3_RADIAL_CAP = Q15_MAX_NORM
PHYSICAL_NEUTRAL_MAX_NORM = 0.06
PHYSICAL_NEUTRAL_MAX_Q15 = int(math.floor(PHYSICAL_NEUTRAL_MAX_NORM * Q15_SCALE))
MAX_EMISSION_LATENCY_MS = 2
MIN_TTL_MS = 16
MAX_TTL_MS = 80
UINT32_MAX = 0xFFFFFFFF
STATE_SOURCE = "state_q15"
SECOND_AUTH_FLAG = "--allow-ds04-state-q15"
EXPECTED_FORMAT = _p3.EXPECTED_FORMAT
EXPECTED_CAL = _p3.EXPECTED_CAL

# Q15 magnitude cap.  Commands are integer Q15 values, so compare the square
# in integer space.  The fixed diagonal vector is chosen below to stay inside
# this integer cap after quantisation.
MAX_RADIUS_Q15 = int(math.floor(Q15_MAX_NORM * Q15_SCALE + 1e-9))
MAX_RADIUS_Q15_SQ = MAX_RADIUS_Q15 * MAX_RADIUS_Q15

_U32 = re.compile(r"^(?:0|[1-9][0-9]*)$")
_S32 = re.compile(r"^-?(?:0|[1-9][0-9]*)$")
_TOKEN = re.compile(r"^[A-Za-z0-9_.:+-]+$")
_HEX16 = re.compile(r"^[0-9A-Fa-f]{4}$")


# Re-export the common error/type names.  Hidden/host tests can use either
# module without having to know which layer raised the error.
HarnessError = _p3.HarnessError
AuthorizationError = _p3.AuthorizationError
ArtifactError = _p3.ArtifactError
ProtocolError = _p3.ProtocolError
HarnessTimeout = _p3.HarnessTimeout
Status = _p3.Status
Caps = _p3.Caps
Ack = _p3.Ack
Nack = _p3.Nack
Artifact = _p3.Artifact
ArtifactManifest = _p3.ArtifactManifest


class StateQ15Error(HarnessError):
    """Base for errors raised by the data-plane gate."""


class CapabilityError(StateQ15Error):
    pass


class SessionError(StateQ15Error):
    pass


class StaleSequenceError(StateQ15Error):
    pass


class RadialLimitError(StateQ15Error):
    pass


class TtlError(StateQ15Error):
    pass


class MissingZeroError(StateQ15Error):
    pass


class StatusValidationError(StateQ15Error):
    pass


class MouseTeardownError(StateQ15Error):
    pass


class Transport(Protocol):
    """Minimal offline transport contract used by :func:`run_ds04`.

    A production serial adapter is intentionally not supplied.  Test doubles
    may implement ``readline()`` with or without a timeout argument.
    """

    def write(self, command: str) -> Any: ...

    def readline(self, timeout: float | None = None) -> str | bytes | None: ...

    def close(self) -> Any: ...


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def _strip_wire_line(value: str | bytes) -> str:
    """Use the strict phase-3 framing helper while keeping it public here."""

    return _p3._strip_wire_line(value)


def _parse_uint(token: str, label: str) -> int:
    if not _U32.fullmatch(token):
        raise ProtocolError(f"{label} is not a decimal unsigned integer")
    value = int(token, 10)
    if value > UINT32_MAX:
        raise ProtocolError(f"{label} is outside uint32")
    return value


def _parse_sint(token: str, label: str) -> int:
    if not _S32.fullmatch(token):
        raise ProtocolError(f"{label} is not a decimal signed integer")
    value = int(token, 10)
    if value < -32767 or value > 32767:
        raise ProtocolError(f"{label} is outside Q15 int16 range")
    return value


def q15_from_normalized(value: float) -> int:
    """Convert a finite normalized value using the firmware's Q15 rounding."""

    if not isinstance(value, (int, float)) or not math.isfinite(float(value)):
        raise ValueError("normalized value must be finite")
    value = float(value)
    if value < -1.0 or value > 1.0:
        raise ValueError("normalized value must be in [-1,1]")
    magnitude = math.floor(abs(value) * Q15_SCALE + 0.5)
    result = -magnitude if value < 0 else magnitude
    return max(-Q15_SCALE, min(Q15_SCALE, result))


def normalized_from_q15(value: int) -> float:
    if not isinstance(value, int) or value < -Q15_SCALE or value > Q15_SCALE:
        raise ValueError("value is outside Q15 range")
    return value / Q15_SCALE


def radial_q15_sq(x: int, y: int) -> int:
    return int(x) * int(x) + int(y) * int(y)


def radial_norm(x: int, y: int) -> float:
    return math.hypot(x, y) / Q15_SCALE


def validate_radial_cap(x: int, y: int) -> None:
    if not isinstance(x, int) or not isinstance(y, int):
        raise RadialLimitError("Q15 coordinates must be integers")
    if x < -Q15_SCALE or x > Q15_SCALE or y < -Q15_SCALE or y > Q15_SCALE:
        raise RadialLimitError("Q15 coordinate is outside [-32767,32767]")
    if radial_q15_sq(x, y) > MAX_RADIUS_Q15_SQ:
        raise RadialLimitError(
            f"radial magnitude exceeds phase-3 cap {Q15_MAX_NORM:.2f}"
        )


def _crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


@dataclass(frozen=True)
class StateQ15Command:
    injection_session: int
    sequence: int
    x: int
    y: int
    ttl_ms: int = 32
    source: str = STATE_SOURCE

    def __post_init__(self) -> None:
        if self.source != STATE_SOURCE:
            raise CapabilityError("DS-04 only permits source=state_q15")
        if not isinstance(self.injection_session, int) or not 0 < self.injection_session <= UINT32_MAX:
            raise SessionError("injection session must be a non-zero uint32")
        if not isinstance(self.sequence, int) or not 0 < self.sequence <= UINT32_MAX:
            raise StaleSequenceError("sequence must be a non-zero uint32")
        if not isinstance(self.ttl_ms, int) or not MIN_TTL_MS <= self.ttl_ms <= MAX_TTL_MS:
            raise TtlError(f"TTL must be in {MIN_TTL_MS}..{MAX_TTL_MS} ms")
        validate_radial_cap(self.x, self.y)

    @property
    def is_zero(self) -> bool:
        return self.x == 0 and self.y == 0

    @property
    def radial(self) -> float:
        return radial_norm(self.x, self.y)

    @property
    def vector(self) -> tuple[int, int]:
        return self.x, self.y

    def wire(self) -> str:
        return encode_state_q15(
            self.injection_session, self.sequence, self.x, self.y, self.ttl_ms
        )


def encode_state_q15(
    injection_session: int,
    sequence: int,
    x: int,
    y: int,
    ttl_ms: int = 32,
) -> str:
    """Encode one strict CRC-protected ``state_q15`` command.

    The function deliberately has no ``rate``, ``move`` or ``lease`` branch;
    callers cannot use it to accidentally widen the DS-04 data plane.
    """

    command = StateQ15Command(injection_session, sequence, x, y, ttl_ms)
    body = (
        f"km.stick.state({command.injection_session},{command.sequence},"
        f"{command.x},{command.y},{command.ttl_ms})"
    )
    return f"{body}*{_crc16_ccitt(body.encode('ascii')):04X}"


def parse_state_q15(line: str | bytes) -> StateQ15Command:
    """Strictly parse one CRC-protected state command.

    ``rate``, ``lease`` and ``move`` commands are explicitly rejected even if
    their CRC and argument shape are valid.
    """

    text = _strip_wire_line(line)
    if not text.startswith("km.stick.state(") or ")*" not in text:
        raise ProtocolError("DS-04 accepts only km.stick.state(...) data commands")
    close = text.find(")*")
    if close == -1 or text.find(")*", close + 2) != -1:
        raise ProtocolError("malformed state_q15 framing")
    body, star, digest = text[:close + 1], text[close + 1], text[close + 2:]
    if star != "*" or not _HEX16.fullmatch(digest):
        raise ProtocolError("state_q15 CRC suffix is malformed")
    expected = _crc16_ccitt(body.encode("ascii"))
    if int(digest, 16) != expected:
        raise ProtocolError("state_q15 CRC mismatch")
    if not body.endswith(")"):
        raise ProtocolError("state_q15 command is missing closing parenthesis")
    args = body[len("km.stick.state("):-1].split(",")
    if len(args) != 5 or any(not arg for arg in args):
        raise ProtocolError("state_q15 argument count mismatch")
    session = _parse_uint(args[0], "injection_session")
    sequence = _parse_uint(args[1], "sequence")
    x = _parse_sint(args[2], "x")
    y = _parse_sint(args[3], "y")
    ttl = _parse_uint(args[4], "ttl_ms")
    try:
        return StateQ15Command(session, sequence, x, y, ttl)
    except (StateQ15Error, ValueError) as exc:
        raise ProtocolError(str(exc)) from exc


# Friendly aliases used by small host tools/tests.
encode_stick_state_q15 = encode_state_q15
parse_stick_state_q15 = parse_state_q15
parse_state = parse_state_q15


@dataclass(frozen=True)
class SafeVector:
    label: str
    x_norm: float
    y_norm: float
    reversal: bool = False

    @property
    def x(self) -> int:
        return q15_from_normalized(self.x_norm)

    @property
    def y(self) -> int:
        return q15_from_normalized(self.y_norm)

    @property
    def is_zero(self) -> bool:
        return self.x == 0 and self.y == 0

    @property
    def radial(self) -> float:
        return math.hypot(self.x_norm, self.y_norm)


def _fixed_safe_vectors() -> tuple[SafeVector, ...]:
    """Return the deterministic phase-3 vector plan.

    Each non-zero vector is immediately followed by ``zero``.  Opposite signs
    are therefore separated by an explicit center command (the ``reversal``
    marker is retained as useful audit metadata).  Diagonal components are
    normalized from radius 0.20, rather than using 0.20 on each axis.
    """

    vectors: list[SafeVector] = [SafeVector("zero", 0.0, 0.0)]
    axis = (
        ("+x", 1.0, 0.0), ("-x", -1.0, 0.0),
        ("+y", 0.0, 1.0), ("-y", 0.0, -1.0),
    )
    for axis_name, sx, sy in axis:
        for amount in (0.05, 0.10, 0.20):
            label = f"{axis_name}_{amount:.2f}"
            vectors.extend(
                (SafeVector(label, sx * amount, sy * amount),
                 SafeVector("zero", 0.0, 0.0, reversal=amount == 0.20))
            )
    # Derive the component in integer space and round down.  Rounding the
    # normalized float independently can produce 4634/4634, whose integer
    # radius is one LSB above the 6553 cap.
    diagonal_component = (
        math.floor(MAX_RADIUS_Q15 / math.sqrt(2.0)) / Q15_SCALE
    )
    for name, sx, sy in (
        ("diag_++", 1.0, 1.0), ("diag_+-", 1.0, -1.0),
        ("diag_-+", -1.0, 1.0), ("diag_--", -1.0, -1.0),
    ):
        vectors.extend(
            (SafeVector(name, sx * diagonal_component, sy * diagonal_component),
             SafeVector("zero", 0.0, 0.0, reversal=True))
        )
    # Validate the plan at import time.  This protects future edits from
    # accidentally emitting an axis 0.20+0.20 diagonal.
    for item in vectors:
        validate_radial_cap(item.x, item.y)
    for index, item in enumerate(vectors[:-1]):
        if not item.is_zero and not vectors[index + 1].is_zero:
            raise AssertionError("fixed safe vector plan omitted explicit zero")
    return tuple(vectors)


FIXED_SAFE_VECTORS = _fixed_safe_vectors()
SAFE_VECTORS = FIXED_SAFE_VECTORS
SMOKE_SAFE_VECTORS = (
    SafeVector("zero", 0.0, 0.0),
    SafeVector("+x_0.05", 0.05, 0.0),
    SafeVector("zero", 0.0, 0.0),
)


class SequenceAllocator:
    """Strict non-wrapping uint32 sequence allocator."""

    def __init__(self, initial: int = 0) -> None:
        if not isinstance(initial, int) or initial < 0 or initial > UINT32_MAX:
            raise ValueError("initial sequence must be a uint32")
        self.last = initial

    def next(self) -> int:
        if self.last >= UINT32_MAX:
            raise StaleSequenceError("uint32 sequence exhausted; refusing wrap")
        self.last += 1
        return self.last


class DataPlaneStateMachine:
    """Pure DS-04 command/status gate used by the transport runner."""

    def __init__(
        self,
        physical_session: int,
        injection_session: int,
        *,
        expected_format: str = EXPECTED_FORMAT,
        expected_cal: str = EXPECTED_CAL,
        max_emission_latency_ms: int = MAX_EMISSION_LATENCY_MS,
    ) -> None:
        if not isinstance(physical_session, int) or not 0 < physical_session <= UINT32_MAX:
            raise SessionError("physical session must be a non-zero uint32")
        if not isinstance(injection_session, int) or not 0 < injection_session <= UINT32_MAX:
            raise SessionError("injection session must be a non-zero uint32")
        self.physical_session = physical_session
        self.injection_session = injection_session
        self.expected_format = expected_format
        self.expected_cal = expected_cal
        if not isinstance(max_emission_latency_ms, int) or max_emission_latency_ms < 0:
            raise ValueError("max_emission_latency_ms must be a non-negative integer")
        self.max_emission_latency_ms = max_emission_latency_ms
        self.last_sequence = 0
        self.last_status_sequence = 0
        self.previous_nonzero = False
        self.statuses: list[Status] = []
        self.commands: list[StateQ15Command] = []
        self.emission_latencies_ms: list[int] = []
        self._counter_baseline: dict[str, Any] | None = None

    @property
    def phase(self) -> str:
        return "awaiting_zero" if self.previous_nonzero else "ready"

    def send(self, command: StateQ15Command) -> str:
        if not isinstance(command, StateQ15Command):
            raise ProtocolError("DS-04 state machine accepts StateQ15Command only")
        if command.injection_session != self.injection_session:
            raise SessionError(
                f"wrong injection session {command.injection_session}; "
                f"expected {self.injection_session}"
            )
        if command.sequence <= self.last_sequence:
            raise StaleSequenceError(
                f"stale/non-increasing sequence {command.sequence}; "
                f"last sent {self.last_sequence}"
            )
        if self.previous_nonzero and not command.is_zero:
            raise MissingZeroError("every non-zero state must be followed by explicit zero")
        self.last_sequence = command.sequence
        self.previous_nonzero = not command.is_zero
        self.commands.append(command)
        return command.wire()

    def _counter(self, status: Status, key: str, default: Any = 0) -> Any:
        return status.counters.get(key, default)

    @staticmethod
    def _pair(value: Any, key: str, arity: int) -> tuple[int, ...]:
        if not isinstance(value, (tuple, list)) or len(value) != arity or any(
            not isinstance(item, int) or item < 0 for item in value
        ):
            raise StatusValidationError(f"status counter {key} is malformed")
        return tuple(value)

    def _validate_counters(self, status: Status, command: StateQ15Command) -> None:
        # The strict parser guarantees these keys exist, but checking shape and
        # monotonicity here catches a stale/shifted status mock as well.
        cmd = self._pair(self._counter(status, "cmd"), "cmd", 2)
        seq = self._pair(self._counter(status, "seq"), "seq", 3)
        override = self._pair(self._counter(status, "override"), "override", 3)
        wrong = self._pair(self._counter(status, "wrong"), "wrong", 2)
        fifo = self._pair(self._counter(status, "fifo"), "fifo", 2)
        barrier = self._pair(self._counter(status, "barrier"), "barrier", 3)
        target = self._pair(self._counter(status, "target"), "target", 4)
        awaiting = self._pair(self._counter(status, "awaiting"), "awaiting", 4)
        timing = self._pair(self._counter(status, "timing"), "timing", 5)
        scalar_keys = (
            "crc", "ttl", "modified", "center", "sat", "guard", "decode", "stall",
            "mode_switch", "reversal", "source_conflict", "frame",
        )
        scalars: dict[str, int] = {}
        for key in scalar_keys:
            value = self._counter(status, key)
            if not isinstance(value, int) or value < 0:
                raise StatusValidationError(f"status counter {key} is malformed")
            scalars[key] = value
        profile = self._counter(status, "profile")
        if (
            not isinstance(profile, tuple) or len(profile) != 2 or
            not isinstance(profile[0], str) or not isinstance(profile[1], int) or
            profile[1] < 0
        ):
            raise StatusValidationError("status counter profile is malformed")

        current: dict[str, Any] = {
            "cmd": cmd, "seq": seq, "override": override, "wrong": wrong,
            "fifo": fifo, "barrier": barrier, "target": target,
            "awaiting": awaiting, "profile": profile, **scalars,
        }
        current["timing"] = timing
        old = self._counter_baseline
        if old is not None:
            # Counters are monotonically increasing.  A lower value means this
            # status belongs to an older session/frame and must be rejected.
            for key, value in current.items():
                previous = old[key]
                if isinstance(value, tuple):
                    if any(now < before for now, before in zip(value, previous)):
                        raise StatusValidationError(f"status counter {key} regressed")
                elif isinstance(value, int):
                    if value < previous:
                        raise StatusValidationError(f"status counter {key} regressed")
                elif isinstance(value, tuple) and isinstance(previous, tuple):
                    pass
            # An accepted sequence must not report stale, duplicate, wrong
            # session, CRC, source conflict, guard or frame errors.
            for key in ("crc", "ttl", "sat", "guard", "decode", "stall", "source_conflict", "frame"):
                if current[key] != old[key]:
                    raise StatusValidationError(f"status counter {key} changed")
            for key in ("seq", "wrong"):
                if current[key] != old[key]:
                    raise StatusValidationError(f"status counter {key} changed")
            if current["cmd"][1] != old["cmd"][1]:
                raise StatusValidationError("invalid command counter changed")
            if current["cmd"][0] <= old["cmd"][0]:
                raise StatusValidationError("accepted command counter did not advance")
            old_timing = old["timing"]
            assert isinstance(old_timing, tuple)
            if current["timing"][0] < old_timing[0]:
                raise StatusValidationError("status timing current sequence regressed")
        else:
            # Fresh harness state must not start with evidence of parser or
            # session errors.  A non-zero existing ``center`` is allowed (it
            # can represent an earlier fail-closed report), but all safety
            # error counters must be zero.
            for key in ("crc", "ttl", "sat", "guard", "decode", "stall", "source_conflict", "frame"):
                if current[key] != 0:
                    raise StatusValidationError(f"status counter {key} is non-zero")
            if any(seq):
                raise StatusValidationError("status sequence error counters are non-zero")
            if any(wrong):
                raise StatusValidationError("status wrong-session counters are non-zero")
        # A fresh accepted state should increment valid command count.  Some
        # firmware status snapshots are taken before the async report, so we
        # require only monotonicity on later frames; the first snapshot is
        # required to show at least one valid command.
        if old is None and cmd[0] < 1:
            raise StatusValidationError("status does not record accepted command")
        if timing[0] != command.sequence:
            raise StatusValidationError("status timing current sequence mismatch")
        if timing[2] != command.sequence:
            raise StatusValidationError("status timing emitted sequence mismatch")
        if timing[1] != timing[3] or timing[3] > timing[4]:
            raise StatusValidationError("status timing timestamps are not monotonic")
        latency = timing[4] - timing[3]
        if latency > self.max_emission_latency_ms:
            raise StatusValidationError(
                f"accepted-to-emitted latency {latency}ms exceeds "
                f"{self.max_emission_latency_ms}ms"
            )
        self.emission_latencies_ms.append(latency)
        self._counter_baseline = current

    def accept_status(self, status: Status, command: StateQ15Command) -> Status:
        if not isinstance(status, Status):
            raise ProtocolError("DS-04 requires a parsed STICK_STATUS frame")
        failures: list[str] = []
        if status.physical_session != self.physical_session:
            failures.append("physical session mismatch")
        if status.injection_session != self.injection_session:
            failures.append("injection session mismatch")
        if status.source != STATE_SOURCE:
            failures.append(f"source={status.source}")
        if status.last_seq != command.sequence:
            failures.append(
                f"accepted seq={status.last_seq} expected {command.sequence}"
            )
        if status.format_id != self.expected_format or status.cal_id != self.expected_cal:
            failures.append("profile/calibration mismatch")
        if status.synthetic_x != command.x or status.synthetic_y != command.y:
            failures.append("synthetic Q15 does not match accepted command")
        physical_sq = radial_q15_sq(status.physical_x, status.physical_y)
        if physical_sq > PHYSICAL_NEUTRAL_MAX_Q15 * PHYSICAL_NEUTRAL_MAX_Q15:
            failures.append("physical Q15 is outside neutral-only gate")
        if command.is_zero:
            if (status.merged_x, status.merged_y) != (
                status.physical_x, status.physical_y
            ):
                failures.append("zero command did not preserve physical center report")
        elif status.merged_x != command.x or status.merged_y != command.y:
            failures.append("merged Q15 does not match emitted command")
        if status.override != 0 or status.fault != 0:
            failures.append("override/fault is active")
        if status.command_age_ms > command.ttl_ms:
            failures.append("status command age exceeds command TTL")
        expected_state = "stick_idle" if command.is_zero else "stick_active"
        if status.state != expected_state:
            failures.append(f"state={status.state} expected {expected_state}")
        if failures:
            raise StatusValidationError("invalid accepted STICK_STATUS: " + "; ".join(failures))
        self._validate_counters(status, command)
        self.last_status_sequence = command.sequence
        self.statuses.append(status)
        return status

    def require_explicit_zero(self) -> None:
        if self.previous_nonzero:
            raise MissingZeroError("data-plane sequence ended without explicit zero")

    # Commonly useful spelling for host tests.
    validate_status = accept_status


StateQ15StateMachine = DataPlaneStateMachine


class AuditTranscript:
    """Small atomic JSON audit writer for offline DS-04 runs."""

    schema = "makcm.ds04-state-q15-transcript.v1"

    def __init__(
        self,
        path: Path | None,
        *,
        artifacts: ArtifactManifest | None = None,
        authorizations: Mapping[str, bool] | None = None,
    ) -> None:
        if path is None:
            stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            path = Path("diagnostics") / f"ds04_state_q15_{stamp}.json"
        self.path = Path(path)
        self.started_at = _utc_now()
        self.artifacts = artifacts
        self.authorizations = dict(authorizations or {})
        self.events: list[dict[str, Any]] = []
        self.outcome = "in_progress"
        self.error: str | None = None

    def record(self, direction: str, line: str, *, parsed: str | None = None) -> None:
        if direction not in ("command", "response", "event"):
            raise ValueError("invalid audit direction")
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
        artifacts: dict[str, Any] = {}
        if self.artifacts is not None:
            for artifact in self.artifacts.items():
                artifacts[artifact.label] = {
                    "path": str(artifact.path),
                    "expected_sha256": str(artifact.expected_sha256).lower(),
                }
        result: dict[str, Any] = {
            "schema": self.schema,
            "started_at_utc": self.started_at,
            "finished_at_utc": _utc_now(),
            "authorizations": self.authorizations,
            "cap": Q15_MAX_NORM,
            "ttl_ms": {"min": MIN_TTL_MS, "max": MAX_TTL_MS},
            "source": STATE_SOURCE,
            "artifacts": artifacts,
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
                import os
                os.fsync(handle.fileno())
            import os
            os.replace(temporary, self.path)
        except BaseException:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
            raise


def _verify_artifacts(artifacts: ArtifactManifest | None) -> dict[str, str]:
    if artifacts is None:
        return {}
    # Keep the existing parser/manifest's exact SHA-256 rules.  This function
    # is called before any transport write.
    return _p3.verify_artifacts(artifacts)


def _readline(transport: Transport, timeout: float) -> str | bytes | None:
    try:
        return transport.readline(timeout)
    except TypeError:
        return transport.readline()  # type: ignore[call-arg]


def _wait_line(transport: Transport, audit: AuditTranscript, timeout: float) -> str:
    deadline = time.monotonic() + max(0.0, timeout)
    swallow_button_crlf_tail = False
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise HarnessTimeout("timeout waiting for DS-04 response")
        raw = _readline(transport, remaining)
        if raw is None or raw == "" or raw == b"":
            time.sleep(min(0.002, max(0.0, remaining)))
            continue
        if swallow_button_crlf_tail and raw in (b"\r\n", "\r\n"):
            audit.record("event", "button_stream:ignored trailing_crlf")
            swallow_button_crlf_tail = False
            continue
        swallow_button_crlf_tail = False
        button_frame = _p3._button_stream_frame(raw)
        if button_frame is not None:
            mask, swallow_button_crlf_tail = button_frame
            audit.record("event", f"button_stream:ignored mask={mask}")
            continue
        stripped = _p3._strip_prompt_prefix(raw)
        if stripped in (b"", "") and stripped != raw:
            audit.record("event", "button_stream:ignored prompt")
            continue
        try:
            text = _strip_wire_line(stripped)
        except Exception:
            audit.record("response", repr(raw))
            raise
        audit.record("response", text)
        return text


def _exchange_control(
    transport: Transport,
    audit: AuditTranscript,
    command: str,
    timeout: float,
) -> _p3.Response:
    # Existing control guard ensures we never accidentally put an unframed
    # data command on the control path.
    _p3.assert_control_command(command)
    audit.record("command", command)
    transport.write(command)
    text = _wait_line(transport, audit, timeout)
    response = _p3.parse_response(text)
    audit.events[-1]["parsed"] = type(response).__name__
    if isinstance(response, Nack):
        raise ProtocolError(f"device NACK {response.op}: {response.reason} ({response.state})")
    return response


def _exchange_status(
    transport: Transport,
    audit: AuditTranscript,
    timeout: float,
) -> Status:
    response = _exchange_control(transport, audit, _p3.STATUS_COMMAND, timeout)
    if not isinstance(response, Status):
        raise ProtocolError("expected STICK_STATUS response")
    return response


def _wait_emitted_status(
    transport: Transport,
    audit: AuditTranscript,
    machine: DataPlaneStateMachine,
    command: StateQ15Command,
    timeout: float,
) -> Status:
    """Wait until the accepted command has a successful USB completion.

    A UART write and even ``last_seq`` acceptance are insufficient evidence:
    the board's same-clock timing tuple must name this sequence as emitted.
    Pending status frames are retained in the audit but are not treated as a
    failure unless they expose a session/fault/sequence contradiction.
    """

    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise HarnessTimeout(
                f"timeout waiting for emitted sequence {command.sequence}"
            )
        status = _exchange_status(transport, audit, remaining)
        if status.physical_session != machine.physical_session:
            raise StatusValidationError("physical session changed while awaiting emission")
        if status.injection_session != machine.injection_session:
            raise StatusValidationError("injection session changed while awaiting emission")
        if status.fault != 0 or status.override != 0:
            raise StatusValidationError("fault/override while awaiting emission")
        if status.source != STATE_SOURCE:
            raise StatusValidationError("source changed while awaiting emission")
        timing = status.counters.get("timing")
        if not isinstance(timing, (tuple, list)) or len(timing) != 5:
            raise StatusValidationError("same-board timing telemetry is required")
        accepted_seq, _accepted_ms, emitted_seq, _emitted_accept_ms, _emitted_ms = timing
        if (
            status.last_seq == command.sequence and
            accepted_seq == command.sequence and
            emitted_seq == command.sequence
        ):
            return status
        previous = machine.last_status_sequence
        if status.last_seq not in (previous, command.sequence):
            raise StatusValidationError("unexpected accepted sequence while awaiting emission")
        if accepted_seq not in (previous, command.sequence):
            raise StatusValidationError("unexpected timing sequence while awaiting emission")
        if emitted_seq not in (previous, command.sequence):
            raise StatusValidationError("unexpected emitted sequence while awaiting emission")
        audit.record(
            "event",
            f"emission_pending requested={command.sequence} "
            f"accepted={status.last_seq} emitted={emitted_seq}",
        )
        time.sleep(min(0.001, max(0.0, deadline - time.monotonic())))


def _write_state(
    transport: Transport,
    audit: AuditTranscript,
    command: StateQ15Command,
) -> None:
    wire = command.wire()
    audit.record("command", wire)
    transport.write(wire)


def _validate_begin_ack(ack: Ack, status: Status) -> int:
    if (
        ack.op != "begin" or ack.physical_session != status.physical_session or
        ack.physical_session == 0 or ack.injection_session == 0 or
        ack.mode != "mouse" or ack.format_id != EXPECTED_FORMAT or ack.cal_id != EXPECTED_CAL
    ):
        raise SessionError("begin ACK session/mode/format mismatch")
    return ack.injection_session


def _validate_output_stick_ack(ack: Ack, physical: int, injection: int) -> None:
    if (
        ack.op != "output_mode" or ack.physical_session != physical or
        ack.injection_session != injection or ack.mode != "stick" or
        ack.format_id != EXPECTED_FORMAT or ack.cal_id != EXPECTED_CAL
    ):
        raise SessionError("output_mode(stick) ACK session/mode/format mismatch")


def _validate_mouse_ack(ack: Ack, physical: int) -> None:
    if (
        ack.op != "output_mode" or ack.physical_session != physical or
        ack.physical_session == 0 or ack.injection_session != 0 or
        ack.mode != "mouse" or ack.format_id != EXPECTED_FORMAT or ack.cal_id != EXPECTED_CAL
    ):
        raise MouseTeardownError("output_mode(mouse) ACK session/mode/format mismatch")


def _teardown_mouse(
    transport: Transport,
    audit: AuditTranscript,
    physical_session: int | None,
    *,
    timeout: float,
) -> Status:
    """Best-effort write with strict confirmation when a response is present.

    It is called from ``finally``.  If the main operation already failed, its
    exception remains primary; the teardown failure is an audit event.  On a
    successful run, failure to confirm the mouse barrier is raised.
    """

    command = _p3.OUTPUT_MOUSE_COMMAND
    try:
        _p3.assert_control_command(command)
        audit.record("command", command)
        transport.write(command)
        text = _wait_line(transport, audit, timeout)
        response = _p3.parse_response(text)
        audit.events[-1]["parsed"] = type(response).__name__
        if isinstance(response, Nack):
            raise MouseTeardownError(
                f"device NACK {response.op}: {response.reason} ({response.state})"
            )
        if not isinstance(response, Ack):
            raise MouseTeardownError("expected mouse output ACK")
        if physical_session is None:
            physical_session = response.physical_session
        _validate_mouse_ack(response, physical_session)
        # A fresh final status is mandatory for a successful teardown.  This
        # catches stale ACKs and proves synthetic/merged state returned center.
        final = _exchange_status(transport, audit, timeout)
        failures: list[str] = []
        if final.physical_session != physical_session:
            failures.append("physical session mismatch")
        if final.injection_session != 0:
            failures.append("injection session remains active")
        if final.state != "raw_safe" or final.source != "none":
            failures.append("final state/source is not raw_safe/none")
        if final.synthetic_x != 0 or final.synthetic_y != 0:
            failures.append("final synthetic state is non-zero")
        if final.merged_x != 0 or final.merged_y != 0:
            failures.append("final merged state is non-zero")
        if final.override != 0 or final.fault != 0:
            failures.append("final override/fault is active")
        if final.format_id != EXPECTED_FORMAT or final.cal_id != EXPECTED_CAL:
            failures.append("final profile/calibration mismatch")
        if failures:
            raise MouseTeardownError("unsafe final mouse status: " + "; ".join(failures))
        audit.record("event", "mouse_teardown:confirmed")
        return final
    except Exception as exc:
        audit.record("event", f"mouse_teardown:failed:{type(exc).__name__}:{exc}")
        raise


@dataclass(frozen=True)
class DS04Result:
    caps: Caps
    initial_status: Status
    physical_session: int
    injection_session: int
    commands: tuple[StateQ15Command, ...]
    statuses: tuple[Status, ...]
    final_status: Status | None = None

    @property
    def vectors(self) -> tuple[tuple[int, int], ...]:
        return tuple(item.vector for item in self.commands)

    @property
    def emission_latencies_ms(self) -> tuple[int, ...]:
        values: list[int] = []
        for status in self.statuses:
            timing = status.counters.get("timing")
            if isinstance(timing, (tuple, list)) and len(timing) == 5:
                values.append(int(timing[4]) - int(timing[3]))
        return tuple(values)


def run_ds04(
    transport: Transport,
    *,
    allow_stick_output: bool = False,
    allow_ds04_state_q15: bool = False,
    # Friendly alias accepted for callers that use the shorter flag name.
    allow_state_q15: bool | None = None,
    artifacts: ArtifactManifest | None = None,
    timeout: float = 3.0,
    audit_path: Path | None = None,
    initial_sequence: int = 0,
    vectors: Sequence[SafeVector] | None = None,
    transcript: AuditTranscript | None = None,
    artifacts_verified: bool = False,
) -> DS04Result:
    """Run the offline DS-04 state-Q15 gate over a mock transport.

    Both authorisation flags must be true.  The alias ``allow_state_q15`` is
    deliberately folded into the second flag but is still recorded under both
    names in the audit.  No command is written before authorisation and
    artifact verification succeed.
    """

    if allow_state_q15 is not None:
        allow_ds04_state_q15 = bool(allow_state_q15)
    auth = {
        "allow_stick_output": bool(allow_stick_output),
        "allow_ds04_state_q15": bool(allow_ds04_state_q15),
        "allow_state_q15_alias": bool(allow_state_q15) if allow_state_q15 is not None else False,
    }
    audit = transcript or AuditTranscript(
        audit_path, artifacts=artifacts, authorizations=auth
    )
    primary_error: Exception | None = None
    physical_session: int | None = None
    injection_session_for_teardown: int | None = None
    last_data_command: StateQ15Command | None = None
    result: DS04Result | None = None
    final_status: Status | None = None
    board_touched = False
    try:
        if not allow_stick_output:
            raise AuthorizationError("DS-04 requires --allow-stick-output")
        if not allow_ds04_state_q15:
            raise AuthorizationError(f"DS-04 requires explicit {SECOND_AUTH_FLAG}")
        if timeout <= 0:
            raise ValueError("timeout must be positive")
        if not artifacts_verified:
            _verify_artifacts(artifacts)
        audit.record("event", "authorization:second_gate_granted")
        audit.record("event", "artifacts:verified")

        # Control-plane preflight/arm uses the strict DS-03 parser and exact
        # profile checks.  It remains offline because the caller owns the mock
        # transport and this module never imports pyserial.
        # Mark before the first write.  If a test transport raises from
        # ``write`` the board may still have observed part of the command and
        # the fail-safe mouse attempt is still required.
        board_touched = True
        caps_response = _exchange_control(transport, audit, _p3.CAPS_COMMAND, timeout)
        if not isinstance(caps_response, Caps):
            raise ProtocolError("expected STICK_CAPS response")
        _p3._validate_caps(caps_response)
        if (caps_response.state_q15 != 1 or caps_response.rate != 0 or
                caps_response.move_compat != 0 or caps_response.game_cal != 0):
            raise CapabilityError("only state_q15 capability is allowed")

        initial = _exchange_status(transport, audit, timeout)
        _p3._safe_status(initial)
        physical_session = initial.physical_session

        begin_command = f"km.stick.begin({EXPECTED_FORMAT},{EXPECTED_CAL},state_q15)"
        begin_response = _exchange_control(transport, audit, begin_command, timeout)
        if not isinstance(begin_response, Ack):
            raise ProtocolError("expected begin ACK")
        injection_session = _validate_begin_ack(begin_response, initial)
        injection_session_for_teardown = injection_session

        output_response = _exchange_control(transport, audit, _p3.OUTPUT_STICK_COMMAND, timeout)
        if not isinstance(output_response, Ack):
            raise ProtocolError("expected output_mode(stick) ACK")
        _validate_output_stick_ack(output_response, physical_session, injection_session)

        machine = DataPlaneStateMachine(
            physical_session, injection_session,
            expected_format=EXPECTED_FORMAT, expected_cal=EXPECTED_CAL,
        )
        allocator = SequenceAllocator(initial_sequence)
        plan = tuple(vectors) if vectors is not None else FIXED_SAFE_VECTORS
        if not plan:
            raise ProtocolError("DS-04 vector plan cannot be empty")
        for vector in plan:
            if not isinstance(vector, SafeVector):
                raise ProtocolError("vector plan contains a non-SafeVector item")
            command = StateQ15Command(
                injection_session, allocator.next(), vector.x, vector.y,
                # 32 ms is inside the mandatory TTL range and deterministic.
                32,
            )
            last_data_command = command
            machine.send(command)
            # Validate zero-boundary and fresh sequence *before* putting the
            # command on the wire.  A rejected next vector must never become
            # an accidental data-plane write.
            _write_state(transport, audit, command)
            accepted = _wait_emitted_status(
                transport, audit, machine, command, timeout
            )
            machine.accept_status(accepted, command)
        machine.require_explicit_zero()
        # The fixed plan always ends at center; custom plans cannot bypass the
        # final explicit-zero requirement.
        result = DS04Result(
            caps_response, initial, physical_session, injection_session,
            tuple(machine.commands), tuple(machine.statuses), None,
        )
        audit.record("event", f"vectors:complete count={len(machine.commands)}")
    except Exception as exc:
        primary_error = exc
        audit.record("event", f"failure:{type(exc).__name__}:{exc}")
        raise
    finally:
        # The mouse barrier is attempted even when caps, begin, NACK, timeout,
        # status validation or vector generation fails.  On early failures the
        # physical session is unknown; the ACK itself supplies it.
        teardown_error: Exception | None = None
        if board_touched:
            try:
                # If an exception interrupted a non-zero command/status pair,
                # put one explicit center command on the wire before the mode
                # barrier.  It is intentionally not awaited: mouse teardown
                # remains the final fail-safe and data-plane responses are
                # normally telemetry-only.
                if (
                    last_data_command is not None and
                    not last_data_command.is_zero and
                    injection_session_for_teardown is not None
                ):
                    if last_data_command.sequence >= UINT32_MAX:
                        audit.record("event", "explicit_zero:sequence_exhausted")
                    else:
                        zero = StateQ15Command(
                            injection_session_for_teardown,
                            last_data_command.sequence + 1,
                            0, 0, last_data_command.ttl_ms,
                        )
                        _write_state(transport, audit, zero)
                        audit.record("event", "explicit_zero:finally")
                final_status = _teardown_mouse(
                    transport, audit, physical_session, timeout=timeout
                )
            except Exception as exc:
                teardown_error = exc
        else:
            audit.record("event", "mouse_teardown:skipped_board_not_touched")
        try:
            transport.close()
        except Exception as exc:
            audit.record("event", f"transport_close:failed:{type(exc).__name__}:{exc}")
        # ``finally`` cannot replace an active exception.  For a successful
        # main run, however, an unconfirmed teardown makes the whole gate fail.
        if primary_error is None and teardown_error is not None:
            audit.finish("failure", str(teardown_error))
            raise teardown_error
        if primary_error is not None:
            audit.finish("failure", str(primary_error))
        elif teardown_error is None:
            audit.finish("success")

    if result is None or final_status is None:
        raise HarnessError("DS-04 completed without a confirmed result")
    return replace(result, final_status=final_status)


def run_ds04_on_port(
    port: str,
    *,
    allow_stick_output: bool,
    allow_ds04_state_q15: bool,
    artifacts: ArtifactManifest,
    baud: int = _p3.DEFAULT_BAUD,
    timeout: float = _p3.DEFAULT_TIMEOUT,
    audit_path: Path | None = None,
    vectors: Sequence[SafeVector] = SMOKE_SAFE_VECTORS,
    allow_active_secondary_process: bool = False,
    transport_factory: Callable[[str, int, float], Transport] = _p3._serial_factory,
) -> DS04Result:
    """Open one explicit live port after every fail-closed precondition.

    There is deliberately no auto-selection.  Authorization and all artifact
    hashes are checked before process ownership and before the serial factory
    is called.  The normal ``run_ds04`` finally path owns confirmed mouse
    teardown and transport closure once the port has been opened.
    """

    auth = {
        "allow_stick_output": bool(allow_stick_output),
        "allow_ds04_state_q15": bool(allow_ds04_state_q15),
        "allow_state_q15_alias": False,
    }
    audit = AuditTranscript(
        audit_path, artifacts=artifacts, authorizations=auth
    )
    try:
        if not allow_stick_output:
            raise AuthorizationError("DS-04 live run requires --allow-stick-output")
        if not allow_ds04_state_q15:
            raise AuthorizationError(
                f"DS-04 live run requires explicit {SECOND_AUTH_FLAG}"
            )
        if not port:
            raise ProtocolError("an explicit serial port is required")
        if baud <= 0 or timeout <= 0:
            raise ValueError("baud and timeout must be positive")
        if artifacts is None:
            raise ArtifactError("DS-04 live run requires the exact artifact manifest")
        _verify_artifacts(artifacts)
        audit.record("event", "artifacts:verified_before_serial_open")
        _p3._serial_process_guard(
            audit,
            allow_active_secondary_process=allow_active_secondary_process,
        )
        transport = transport_factory(port, baud, timeout)
    except Exception as exc:
        audit.record("event", f"live_open:failed:{type(exc).__name__}:{exc}")
        audit.finish("failure", str(exc))
        raise
    return run_ds04(
        transport,
        allow_stick_output=allow_stick_output,
        allow_ds04_state_q15=allow_ds04_state_q15,
        artifacts=artifacts,
        timeout=timeout,
        audit_path=audit_path,
        vectors=vectors,
        transcript=audit,
        artifacts_verified=True,
    )


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True,
                        help="explicit serial port; never auto-selected")
    parser.add_argument("--baud", type=int, default=_p3.DEFAULT_BAUD)
    parser.add_argument("--timeout", type=float, default=_p3.DEFAULT_TIMEOUT)
    parser.add_argument("--audit", dest="audit_path", type=Path, required=True)
    parser.add_argument("--plan", choices=("smoke", "full"), required=True)
    parser.add_argument("--allow-stick-output", action="store_true")
    parser.add_argument(SECOND_AUTH_FLAG, dest="allow_ds04_state_q15",
                        action="store_true")
    parser.add_argument(
        "--allow-active-secondary-process", action="store_true",
        help="MANUAL/UNSAFE ownership-guard bypass; recorded in the audit",
    )
    _p3._add_artifacts(parser)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)
    try:
        artifacts = _p3._manifest_from_args(args)
        vectors = SMOKE_SAFE_VECTORS if args.plan == "smoke" else FIXED_SAFE_VECTORS
        result = run_ds04_on_port(
            args.port,
            allow_stick_output=args.allow_stick_output,
            allow_ds04_state_q15=args.allow_ds04_state_q15,
            artifacts=artifacts,
            baud=args.baud,
            timeout=args.timeout,
            audit_path=args.audit_path,
            vectors=vectors,
            allow_active_secondary_process=args.allow_active_secondary_process,
        )
        print(json.dumps({
            "physical_session": result.physical_session,
            "injection_session": result.injection_session,
            "vectors": len(result.commands),
            "emission_latencies_ms": result.emission_latencies_ms,
            "final_state": result.final_status.state if result.final_status else None,
            "final_source": result.final_status.source if result.final_status else None,
        }, sort_keys=True))
        return 0
    except HarnessError as exc:
        print(f"FAIL: {exc}")
        return 1


# Intentional aliases make the bounded operation discoverable without adding a
# second implementation (and preserve a stable API for host-side scripts).
run_state_q15 = run_ds04
run_data_plane = run_ds04
run_phase3_state = run_ds04
DS04StateQ15Harness = DataPlaneStateMachine


def assert_ds04_command(command: str) -> None:
    """Reject forbidden rate/move/lease commands and malformed data lines."""

    if not isinstance(command, str) or command != command.strip() or not command:
        raise ProtocolError("command must be a non-empty framed string")
    if command.startswith(("km.stick.rate(", "km.stick.lease(", "km.move(", "move(")):
        raise CapabilityError("DS-04 forbids rate/move/lease data-plane commands")
    if command.startswith("km.stick.state("):
        parse_state_q15(command)
        return
    if command in (
        _p3.CAPS_COMMAND, _p3.STATUS_COMMAND,
        _p3.OUTPUT_STICK_COMMAND, _p3.OUTPUT_MOUSE_COMMAND,
    ) or command.startswith("km.stick.begin("):
        _p3.assert_control_command(command)
        return
    raise ProtocolError(f"unsupported DS-04 command: {command!r}")


__all__ = [
    "Ack", "Artifact", "ArtifactManifest", "AuditTranscript", "Caps",
    "CapabilityError", "DataPlaneStateMachine", "DS04Result", "DS04StateQ15Harness",
    "EXPECTED_CAL", "EXPECTED_FORMAT", "FIXED_SAFE_VECTORS", "HarnessError",
    "HarnessTimeout", "MAX_EMISSION_LATENCY_MS", "MAX_RADIUS_Q15",
    "MAX_TTL_MS", "MIN_TTL_MS",
    "MissingZeroError", "MouseTeardownError", "Nack", "PHASE3_RADIAL_CAP",
    "PHYSICAL_NEUTRAL_MAX_NORM", "ProtocolError", "Q15_MAX_NORM",
    "RadialLimitError", "SAFE_VECTORS", "SMOKE_SAFE_VECTORS",
    "SECOND_AUTH_FLAG", "STATE_SOURCE", "SafeVector", "SequenceAllocator", "SessionError",
    "StateQ15Command", "StateQ15Error", "StateQ15StateMachine", "Status",
    "StatusValidationError", "StaleSequenceError", "Transport", "TtlError",
    "assert_ds04_command", "encode_state_q15", "encode_stick_state_q15",
    "normalized_from_q15", "parse_state", "parse_state_q15", "parse_stick_state_q15",
    "build_arg_parser", "main", "q15_from_normalized", "radial_norm",
    "radial_q15_sq", "run_data_plane", "run_ds04", "run_ds04_on_port",
    "run_phase3_state", "run_state_q15", "validate_radial_cap",
]


if __name__ == "__main__":
    raise SystemExit(main())
