#!/usr/bin/env python3
"""Compare two saved MAKCM status windows and check error-counter deltas.

The input files may contain arbitrary CH343 text logs.  The newest
``LEFT_STATUS`` line and ``RIGHT:ESPLOG_STATUS`` line are extracted from each
file.  Slash-separated compact fields are expanded, so a pressure run can be
judged without manually subtracting counters.

Example::

    python tests/compare_status_window.py before.txt after.txt \
        --out diagnostics/status_window.json --require-progress
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


TOKEN = re.compile(r"(?P<key>[A-Za-z_][A-Za-z0-9_]*)=(?P<value>[^\s]+)")

LEFT_SLASH_FIELDS = {
    "ep": ["endpoint_submit", "endpoint_complete", "endpoint_retry",
            "endpoint_drop", "endpoint_stale"],
    "ctrl": ["control_setups", "control_forwards", "control_responses",
              "control_stalls"],
    "sec": ["secondary_commands", "secondary_parse_errors",
            "secondary_tx_errors", "secondary_rx_overflows", "fast_baud"],
    "inj": ["injection_moves_accepted", "injection_moves_rejected"],
    "btn": ["injection_buttons_accepted", "injection_buttons_rejected"],
    "seq": ["sequence_frames", "sequence_gap_frames", "sequence_gap_events",
            "sequence_duplicates", "sequence_out_of_order",
            "sequence_session_changes"],
}

RIGHT_SLASH_FIELDS = {
    "stream": ["stream_pending", "stream_retries"],
}


def scalar(value: str) -> Any:
    try:
        return int(value, 0)
    except ValueError:
        return value


def parse_status_line(line: str) -> dict[str, Any]:
    fields: dict[str, Any] = {"raw": line.rstrip("\r\n")}
    kind = "left" if "LEFT_STATUS" in line else "right"
    fields["kind"] = kind
    for match in TOKEN.finditer(line):
        key = match.group("key")
        value = match.group("value")
        if "/" not in value:
            fields[key] = scalar(value)
            continue
        parts = value.split("/")
        if not all(part and re.fullmatch(r"(?:0[xX])?[0-9A-Fa-f]+", part)
                   for part in parts):
            fields[key] = value
            continue
        values = [scalar(part) for part in parts]
        fields[key] = values
        names = (LEFT_SLASH_FIELDS if kind == "left" else RIGHT_SLASH_FIELDS).get(
            key, []
        )
        for name, item in zip(names, values):
            fields[name] = item
    return fields


def newest(lines: list[str], marker: str) -> dict[str, Any] | None:
    matches = [line for line in lines if marker in line]
    return parse_status_line(matches[-1]) if matches else None


def load(path: Path) -> dict[str, Any]:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    return {
        "left": newest(lines, "LEFT_STATUS"),
        "right": newest(lines, "ESPLOG_STATUS"),
    }


def numeric_delta(before: dict[str, Any], after: dict[str, Any]) -> dict[str, int]:
    result: dict[str, int] = {}
    for key, value in after.items():
        if key in {"raw", "kind"} or not isinstance(value, int):
            continue
        old = before.get(key)
        if isinstance(old, int):
            result[key] = value - old
    return result


def evaluate(before: dict[str, Any], after: dict[str, Any],
             require_progress: bool) -> dict[str, Any]:
    result: dict[str, Any] = {"before": before, "after": after,
                              "delta": {}, "failures": []}
    if before is None or after is None:
        result["failures"].append("missing_status_line")
        return result
    delta = numeric_delta(before, after)
    result["delta"] = delta

    # These are monotonic error counters.  A pressure window must not add any
    # of them; historical values before the window are intentionally ignored.
    error_fields = (
        "rxerr", "txerr", "reject", "endpoint_retry", "endpoint_drop",
        "endpoint_stale", "control_stalls", "secondary_parse_errors",
        "secondary_tx_errors", "secondary_rx_overflows", "injection_moves_rejected",
        "injection_buttons_rejected", "sequence_gap_frames",
        "sequence_gap_events", "sequence_duplicates", "sequence_out_of_order",
        "sequence_session_changes", "overwrite", "rxerr", "cleanerr",
    )
    for key in error_fields:
        if key in delta and delta[key] != 0:
            result["failures"].append(f"{key}_delta={delta[key]}")
    if require_progress:
        progress = 0
        for key in ("sequence_frames", "input", "forward", "endpoint_complete"):
            progress += max(0, int(delta.get(key, 0)))
        if progress == 0:
            result["failures"].append("no_progress")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--require-progress", action="store_true")
    args = parser.parse_args()

    before = load(args.before)
    after = load(args.after)
    report = {
        "left": evaluate(before["left"], after["left"],
                          args.require_progress),
        "right": evaluate(before["right"], after["right"],
                           args.require_progress),
    }
    report["failures"] = report["left"]["failures"] + report["right"]["failures"]
    report["pass"] = not report["failures"]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    print(f"wrote {args.out}")
    if report["failures"]:
        print("FAIL: " + ", ".join(report["failures"]))
        return 5
    print("PASS: status counter deltas are clean")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
