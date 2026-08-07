#!/usr/bin/env python3
"""Deterministic MAKCU ``km.move`` source for proxy pressure tests.

This is intentionally separate from Secondary's vision/arm scheduler.  It
writes one small command at a fixed cadence so a simultaneous XInput + Raw
Input observation can answer whether EP81 and EP83 starve each other.

Run this on the Linux Secondary host after stopping ``ai_secondary`` (the
program refuses to share the serial port by default):

    python3 tests/makcu_move_stress.py --port /dev/ttyACM0 \
      --baud 4000000 --hz 1000 --seconds 30 \
      --out diagnostics/makcu_move_stress.json

The Windows observer should be started before this process.  This tool does
not synthesize controller input; move the physical controller stick while it
runs so XInput packet changes are independently present.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=4_000_000)
    parser.add_argument("--hz", type=float, default=1_000.0)
    parser.add_argument("--seconds", type=float, default=30.0)
    parser.add_argument("--dx", type=int, default=1)
    parser.add_argument("--dy", type=int, default=0)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--max-gap-ms",
        type=float,
        default=20.0,
        help="fail if the source itself pauses longer than this",
    )
    parser.add_argument(
        "--min-write-ratio",
        type=float,
        default=0.95,
        help="minimum writes/(seconds*hz) required for pass",
    )
    parser.add_argument(
        "--status-command",
        default="LEFT_STATUS",
        help="line queried before/after the run; use empty string to disable",
    )
    parser.add_argument(
        "--status-timeout-ms",
        type=int,
        default=500,
        help="time to collect the status response after each query",
    )
    parser.add_argument(
        "--allow-shared-port",
        action="store_true",
        help="skip the ai_secondary ownership guard (unsafe; for diagnosis only)",
    )
    return parser.parse_args()


def secondary_is_running() -> bool:
    if os.name != "posix":
        return False
    try:
        result = subprocess.run(
            ["pgrep", "-x", "ai_secondary"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    except OSError:
        return False
    return result.returncode == 0


def read_window(port: object, timeout_s: float) -> str:
    """Collect text emitted by the board without blocking the pressure loop."""
    end = time.monotonic() + max(0.0, timeout_s)
    chunks: list[bytes] = []
    while time.monotonic() < end:
        try:
            waiting = int(getattr(port, "in_waiting", 0))
            chunk = port.read(max(1, min(waiting, 4096))) if waiting else b""
        except Exception:
            break
        if chunk:
            chunks.append(bytes(chunk))
        else:
            time.sleep(0.005)
    return b"".join(chunks).decode("utf-8", errors="replace")


def query_status(port: object, command: str, timeout_ms: int) -> str:
    if not command:
        return ""
    try:
        port.write((command.rstrip("\r\n") + "\r\n").encode("ascii"))
        port.flush()
    except Exception:
        return ""
    return read_window(port, timeout_ms / 1000.0)


def set_button_stream(port: object, enabled: bool) -> None:
    # The periodic button frames are unrelated to the mouse pressure source.
    # Silence them during a long run so the diagnostic reader cannot fill its
    # receive buffer, then restore the normal Secondary contract on exit.
    line = b"km.buttons(1,8)\r\n" if enabled else b"km.buttons(0,0)\r\n"
    try:
        port.write(line)
        port.flush()
        time.sleep(0.05)
        if int(getattr(port, "in_waiting", 0)):
            port.read(int(port.in_waiting))
    except Exception:
        pass


def percentile(values: list[float], q: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return values[0]
    values = sorted(values)
    # statistics.quantiles uses an interpolated definition and avoids a
    # misleading "nearest observed" jump for a long pressure run.
    if q <= 0.0:
        return values[0]
    if q >= 1.0:
        return values[-1]
    if len(values) < 2:
        return values[0]
    if q == 0.5:
        return values[len(values) // 2]
    index = q * (len(values) - 1)
    lower = int(index)
    upper = min(lower + 1, len(values) - 1)
    fraction = index - lower
    return values[lower] + (values[upper] - values[lower]) * fraction


def run(args: argparse.Namespace) -> tuple[dict[str, object], int]:
    if args.hz <= 0.0 or args.seconds <= 0.0:
        return {"failures": ["invalid_duration_or_rate"], "pass": False}, 2
    if not (-32767 <= args.dx <= 32767 and -32767 <= args.dy <= 32767):
        return {"failures": ["move_out_of_range"], "pass": False}, 2
    if not args.allow_shared_port and secondary_is_running():
        return {
            "port": args.port,
            "failures": ["ai_secondary_running_port_not_exclusive"],
            "pass": False,
        }, 3

    try:
        import serial  # type: ignore
    except ImportError as error:
        return {
            "port": args.port,
            "failures": ["pyserial_missing"],
            "error": str(error),
            "pass": False,
        }, 4

    kwargs: dict[str, object] = {
        "port": args.port,
        "baudrate": args.baud,
        "timeout": 0,
        "write_timeout": 0.25,
    }
    if os.name == "posix":
        # pyserial maps this to flock(LOCK_EX|LOCK_NB) on supported systems.
        kwargs["exclusive"] = True

    stop = False

    def request_stop(_signum: int, _frame: object) -> None:
        nonlocal stop
        stop = True

    old_sigint = signal.signal(signal.SIGINT, request_stop)
    old_sigterm = signal.signal(signal.SIGTERM, request_stop)
    payload = f"km.move({args.dx},{args.dy})\r\n".encode("ascii")
    gaps: list[float] = []
    writes = 0
    write_errors: list[str] = []
    total_bytes = 0
    status_before = ""
    status_after = ""
    first_write: float | None = None
    last_write: float | None = None
    max_write_gap = 0.0
    start = time.monotonic()
    deadline = start + args.seconds
    period = 1.0 / args.hz

    try:
        with serial.Serial(**kwargs) as port:
            port.reset_input_buffer()
            set_button_stream(port, False)
            status_before = query_status(
                port, args.status_command, args.status_timeout_ms
            )
            next_tick = time.monotonic()
            while not stop and time.monotonic() < deadline:
                now = time.monotonic()
                if now < next_tick:
                    time.sleep(min(next_tick - now, period / 2.0))
                    continue
                if now > next_tick + period * 3.0:
                    # Do not replay missed slots as a burst; a burst would
                    # hide a source scheduling/serial stall.
                    next_tick = now
                try:
                    port.write(payload)
                except Exception as error:  # pyserial has platform variants
                    write_errors.append(repr(error))
                    break
                written_at = time.monotonic()
                if first_write is None:
                    first_write = written_at
                if last_write is not None:
                    gap_ms = (written_at - last_write) * 1000.0
                    max_write_gap = max(max_write_gap, gap_ms)
                    if len(gaps) < 1_000_000:
                        gaps.append(gap_ms)
                last_write = written_at
                writes += 1
                total_bytes += len(payload)
                next_tick += period
            # A zero report stops motion without changing the source metrics.
            try:
                port.write(b"km.move(0,0)\r\n")
                port.flush()
            except Exception as error:
                write_errors.append("stop:" + repr(error))
            status_after = query_status(
                port, args.status_command, args.status_timeout_ms
            )
            set_button_stream(port, True)
    except Exception as error:
        write_errors.append(repr(error))
    finally:
        signal.signal(signal.SIGINT, old_sigint)
        signal.signal(signal.SIGTERM, old_sigterm)

    elapsed = max(0.0, (last_write or time.monotonic()) - (first_write or start))
    expected = max(1.0, args.seconds * args.hz)
    ratio = writes / expected
    failures: list[str] = []
    if write_errors:
        failures.append("serial_write")
    if writes == 0:
        failures.append("no_writes")
    if ratio < args.min_write_ratio:
        failures.append("source_rate")
    if max_write_gap > args.max_gap_ms:
        failures.append("source_gap")
    result: dict[str, object] = {
        "port": args.port,
        "baud": args.baud,
        "hz": args.hz,
        "seconds": args.seconds,
        "dx": args.dx,
        "dy": args.dy,
        "payload_bytes": len(payload),
        "writes": writes,
        "expected_writes": expected,
        "write_ratio": round(ratio, 6),
        "elapsed_after_first_write_s": round(elapsed, 6),
        "total_bytes": total_bytes,
        "status_command": args.status_command,
        "status_before": status_before,
        "status_after": status_after,
        "max_write_gap_ms": round(max_write_gap, 3),
        "gap_p50_ms": round(percentile(gaps, 0.50), 3),
        "gap_p95_ms": round(percentile(gaps, 0.95), 3),
        "gap_p99_ms": round(percentile(gaps, 0.99), 3),
        "write_errors": write_errors,
        "failures": failures,
        "pass": not failures,
    }
    return result, 0 if result["pass"] else 5


def main() -> int:
    args = parse_args()
    result, code = run(args)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    print(f"wrote {args.out}")
    print(
        "summary: writes=%s ratio=%.3f max_gap_ms=%s failures=%s"
        % (
            result.get("writes", 0),
            float(result.get("write_ratio", 0.0)),
            result.get("max_write_gap_ms", 0.0),
            result.get("failures", []),
        )
    )
    return code


if __name__ == "__main__":
    raise SystemExit(main())
