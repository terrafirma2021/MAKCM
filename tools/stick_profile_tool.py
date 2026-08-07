#!/usr/bin/env python3
"""Build or qualify a fail-closed controller stick report adapter.

Input report CSV schema (header required):

    timestamp_s,endpoint,report_hex

``endpoint`` accepts decimal or 0x-prefixed values. Every row must contain the
complete USB interrupt report, including Report ID when the device uses one.
The legacy mode accepts five single-axis traces and remains available for
bring-up replay.  ``--guided-trace`` selects the production qualification
analyzer: neutral, physical-right/up anchors, clockwise/counter-clockwise
circles, and final neutral are captured in one marker-bearing CSV.  Guided
mode emits an auditable JSON candidate only; it never emits firmware C++.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import statistics
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


class ProfileError(ValueError):
    pass


@dataclass(frozen=True)
class Report:
    timestamp_s: float
    endpoint: int
    data: bytes


@dataclass(frozen=True)
class GuidedReport:
    timestamp_s: float
    endpoint: int
    data: bytes
    marker: str


def parse_int(value: str) -> int:
    return int(value, 0)


def percentile(values: Iterable[float], q: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    index = max(0, min(len(ordered) - 1, math.ceil(q * len(ordered)) - 1))
    return float(ordered[index])


def load_reports(path: Path, endpoint: int, report_id: int,
                 report_length: int) -> list[Report]:
    reports: list[Report] = []
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        required = {"timestamp_s", "endpoint", "report_hex"}
        if not reader.fieldnames or not required.issubset(reader.fieldnames):
            raise ProfileError(f"{path}: required columns are {sorted(required)}")
        for row_number, row in enumerate(reader, 2):
            try:
                row_endpoint = parse_int(row["endpoint"].strip())
                report_hex = row["report_hex"]
                for separator in (" ", ":", "-", ","):
                    report_hex = report_hex.replace(separator, "")
                data = bytes.fromhex(report_hex)
                timestamp = float(row["timestamp_s"])
            except (TypeError, ValueError) as exc:
                raise ProfileError(f"{path}:{row_number}: invalid row: {exc}") from exc
            if row_endpoint != endpoint:
                continue
            if len(data) != report_length:
                raise ProfileError(
                    f"{path}:{row_number}: report length {len(data)} != {report_length}"
                )
            if report_id and data[0] != report_id:
                raise ProfileError(
                    f"{path}:{row_number}: report ID 0x{data[0]:02X} != 0x{report_id:02X}"
                )
            reports.append(Report(timestamp, row_endpoint, data))
    if len(reports) < 8:
        raise ProfileError(f"{path}: need at least 8 qualifying reports, got {len(reports)}")
    reports.sort(key=lambda item: item.timestamp_s)
    return reports


_GUIDED_MARKERS = {
    "neutral_begin": "neutral_begin",
    "neutral": "neutral_begin",
    "neutral_end": "neutral_end",
    "right_begin": "right_begin",
    "right_anchor": "right_begin",
    "physical_right": "right_begin",
    "physical_right_anchor": "right_begin",
    "right_end": "right_end",
    "up_begin": "up_begin",
    "up_anchor": "up_begin",
    "physical_up": "up_begin",
    "physical_up_anchor": "up_begin",
    "up_end": "up_end",
    "cw_begin": "cw_begin",
    "cw": "cw_begin",
    "clockwise": "cw_begin",
    "clockwise_begin": "cw_begin",
    "cw_end": "cw_end",
    "ccw_begin": "ccw_begin",
    "ccw": "ccw_begin",
    "counter_clockwise": "ccw_begin",
    "counter_clockwise_begin": "ccw_begin",
    "counterclockwise": "ccw_begin",
    "counterclockwise_begin": "ccw_begin",
    "ccw_end": "ccw_end",
    "final_neutral_begin": "final_neutral_begin",
    "neutral_final_begin": "final_neutral_begin",
    "neutral_final": "final_neutral_begin",
    "final_neutral": "final_neutral_begin",
    "final_neutral_end": "final_neutral_end",
    "neutral_final_end": "final_neutral_end",
}


def _validate_report_contract(endpoint: int, report_id: int,
                              report_length: int) -> None:
    if not 0 <= endpoint <= 0xFF or not endpoint & 0x80:
        raise ProfileError("endpoint must be an IN endpoint address (0x80..0xFF)")
    if not 0 <= report_id <= 0xFF:
        raise ProfileError("report ID must fit in one byte")
    if report_length < 2 or report_length > 1024:
        raise ProfileError("report length must be between 2 and 1024 bytes")


def load_guided_reports(path: Path, endpoint: int, report_id: int,
                        report_length: int) -> list[GuidedReport]:
    """Load a guided trace without silently dropping malformed completions."""
    _validate_report_contract(endpoint, report_id, report_length)
    reports: list[GuidedReport] = []
    seen_markers: set[str] = set()
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        required = {"timestamp_s", "endpoint", "report_hex", "marker"}
        if not reader.fieldnames or not required.issubset(reader.fieldnames):
            raise ProfileError(f"{path}: required columns are {sorted(required)}")
        for row_number, row in enumerate(reader, 2):
            try:
                row_endpoint = parse_int(row["endpoint"].strip())
                report_hex = row["report_hex"]
                for separator in (" ", ":", "-", ","):
                    report_hex = report_hex.replace(separator, "")
                data = bytes.fromhex(report_hex)
                timestamp = float(row["timestamp_s"])
            except (AttributeError, TypeError, ValueError) as exc:
                raise ProfileError(f"{path}:{row_number}: invalid row: {exc}") from exc
            if not math.isfinite(timestamp) or timestamp < 0:
                raise ProfileError(f"{path}:{row_number}: timestamp must be finite and non-negative")
            if reports and timestamp <= reports[-1].timestamp_s:
                raise ProfileError(f"{path}:{row_number}: timestamps must be strictly increasing")
            if row_endpoint != endpoint:
                raise ProfileError(
                    f"{path}:{row_number}: endpoint 0x{row_endpoint:02X} != 0x{endpoint:02X}"
                )
            if len(data) != report_length:
                raise ProfileError(
                    f"{path}:{row_number}: report length {len(data)} != {report_length}"
                )
            if report_id and data[0] != report_id:
                raise ProfileError(
                    f"{path}:{row_number}: report ID 0x{data[0]:02X} != 0x{report_id:02X}"
                )
            raw_marker = (row.get("marker") or "").strip().lower()
            if raw_marker:
                if raw_marker not in _GUIDED_MARKERS:
                    raise ProfileError(f"{path}:{row_number}: unknown marker {raw_marker!r}")
                marker = _GUIDED_MARKERS[raw_marker]
                if marker in seen_markers:
                    raise ProfileError(f"{path}:{row_number}: duplicate marker {marker!r}")
                seen_markers.add(marker)
            else:
                marker = ""
            reports.append(GuidedReport(timestamp, row_endpoint, data, marker))
    if len(reports) < 48:
        raise ProfileError(f"{path}: guided trace needs at least 48 reports, got {len(reports)}")
    return reports


def _guided_segments(reports: list[GuidedReport]) -> dict[str, list[GuidedReport]]:
    positions = {item.marker: index for index, item in enumerate(reports) if item.marker}
    starts = ("neutral_begin", "right_begin", "up_begin", "cw_begin",
              "ccw_begin", "final_neutral_begin")
    missing = [name for name in starts if name not in positions]
    if missing:
        raise ProfileError("guided trace missing required markers: " + ", ".join(missing))
    ordered = [positions[name] for name in starts]
    if ordered != sorted(ordered) or len(set(ordered)) != len(ordered):
        raise ProfileError("guided markers are out of order")

    def section(start: str, explicit_end: str, next_start: str | None) -> list[GuidedReport]:
        first = positions[start]
        if explicit_end in positions:
            last = positions[explicit_end] + 1
            if last <= first or (next_start and last > positions[next_start]):
                raise ProfileError(f"guided marker {explicit_end} is out of order")
        elif next_start:
            last = positions[next_start]
        else:
            last = len(reports)
        return reports[first:last]

    segments = {
        "neutral_initial": section("neutral_begin", "neutral_end", "right_begin"),
        "right": section("right_begin", "right_end", "up_begin"),
        "up": section("up_begin", "up_end", "cw_begin"),
        "cw": section("cw_begin", "cw_end", "ccw_begin"),
        "ccw": section("ccw_begin", "ccw_end", "final_neutral_begin"),
        "neutral_final": section("final_neutral_begin", "final_neutral_end", None),
    }
    minimum = {"neutral_initial": 8, "right": 4, "up": 4, "cw": 12,
               "ccw": 12, "neutral_final": 8}
    too_short = [f"{name}={len(segments[name])}<{count}"
                 for name, count in minimum.items() if len(segments[name]) < count]
    if too_short:
        raise ProfileError("guided segments have insufficient samples: " + ", ".join(too_short))
    return segments


def read_axis(data: bytes, offset: int, encoding: str) -> int:
    width = 2 if encoding == "int16_le" else 1
    if offset < 0 or offset + width > len(data):
        raise ProfileError(f"axis offset {offset} exceeds report length {len(data)}")
    if encoding == "uint8":
        return data[offset]
    return int.from_bytes(data[offset:offset + 2], "little", signed=True)


def axis_values(reports: list[Report], offset: int, encoding: str) -> list[int]:
    return [read_axis(item.data, offset, encoding) for item in reports]


def median(values: list[int]) -> int:
    return int(round(statistics.median(values)))


def sha256(path: Path) -> bytes:
    return hashlib.sha256(path.read_bytes()).digest()


def descriptor_bundle_sha(device: Path, configuration: Path) -> bytes:
    digest = hashlib.sha256()
    digest.update(device.read_bytes())
    digest.update(configuration.read_bytes())
    return digest.digest()


def compact_hash(digest: bytes) -> int:
    return int.from_bytes(digest[:4], "big")


def cadence(reports: list[Report]) -> dict[str, float | int]:
    gaps = [
        (reports[index].timestamp_s - reports[index - 1].timestamp_s) * 1000.0
        for index in range(1, len(reports))
    ]
    if any(gap <= 0 for gap in gaps):
        raise ProfileError("timestamps must be strictly increasing")
    return {
        "samples": len(reports),
        "gap_p50_ms": percentile(gaps, 0.50),
        "gap_p95_ms": percentile(gaps, 0.95),
        "gap_p99_ms": percentile(gaps, 0.99),
        "gap_max_ms": max(gaps),
    }


def analyze_axis(name: str, neutral: list[int], negative: list[int],
                 positive: list[int], encoding: str) -> dict[str, int | float]:
    center = median(neutral)
    neg = median(negative)
    pos = median(positive)
    neg_delta = neg - center
    pos_delta = pos - center
    minimum_span = 32 if encoding == "uint8" else 4096
    if abs(neg_delta) < minimum_span or abs(pos_delta) < minimum_span:
        raise ProfileError(
            f"{name}: endpoint span too small: center={center} neg={neg} pos={pos}"
        )
    if neg_delta * pos_delta >= 0:
        raise ProfileError(
            f"{name}: negative/positive sweeps do not straddle center: "
            f"center={center} neg={neg} pos={pos}"
        )
    # Internal positive must follow the capture labelled positive. A reversed
    # raw axis is represented by sign=-1 instead of swapping endpoints.
    sign = 1 if pos > center else -1
    all_values = neutral + negative + positive
    raw_min = min(all_values)
    raw_max = max(all_values)
    if not raw_min < center < raw_max:
        raise ProfileError(f"{name}: measured range does not contain center")
    jitter = [abs(value - center) for value in neutral]
    return {
        "center": center,
        "minimum": raw_min,
        "maximum": raw_max,
        "sign": sign,
        "negative_median": neg,
        "positive_median": pos,
        "neutral_jitter_p99": percentile(jitter, 0.99),
        "neutral_jitter_max": max(jitter),
    }


def verify_axis_isolation(
    center_x: int, center_y: int,
    x_negative_x: list[int], x_negative_y: list[int],
    x_positive_x: list[int], x_positive_y: list[int],
    y_negative_x: list[int], y_negative_y: list[int],
    y_positive_x: list[int], y_positive_y: list[int],
) -> dict[str, float]:
    x_intended = min(
        abs(median(x_negative_x) - center_x),
        abs(median(x_positive_x) - center_x),
    )
    y_intended = min(
        abs(median(y_negative_y) - center_y),
        abs(median(y_positive_y) - center_y),
    )
    x_cross = max(
        abs(median(x_negative_y) - center_y),
        abs(median(x_positive_y) - center_y),
    )
    y_cross = max(
        abs(median(y_negative_x) - center_x),
        abs(median(y_positive_x) - center_x),
    )
    x_ratio = x_cross / max(1, x_intended)
    y_ratio = y_cross / max(1, y_intended)
    if x_ratio > 0.10 or y_ratio > 0.10:
        raise ProfileError(
            f"axis isolation failed: X cross={x_ratio:.3f}, Y cross={y_ratio:.3f}; "
            "verify offsets and perform true single-axis sweeps"
        )
    return {"x_cross_ratio": x_ratio, "y_cross_ratio": y_ratio}


def bytes_list(data: bytes) -> list[int]:
    return list(data)


def canonical_hash(profile: dict) -> str:
    encoded = json.dumps(profile, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def _round(value: float) -> float:
    return round(float(value), 6)


def _mad(values: list[int], center: float) -> float:
    return float(statistics.median(abs(value - center) for value in values))


def _change_rate(values: list[int]) -> float:
    if len(values) < 2:
        return 0.0
    return sum(a != b for a, b in zip(values, values[1:])) / (len(values) - 1)


def _increment_consistency(values: list[int], encoding: str) -> float:
    if len(values) < 4:
        return 0.0
    modulus = 256 if encoding == "uint8" else 65536
    half = modulus // 2
    deltas = [((b - a + half) % modulus) - half for a, b in zip(values, values[1:])]
    nonzero = [delta for delta in deltas if delta]
    if not nonzero:
        return 0.0
    return Counter(nonzero).most_common(1)[0][1] / len(nonzero)


def _field_metrics(offset: int, encoding: str,
                   segments: dict[str, list[GuidedReport]],
                   all_reports: list[GuidedReport]) -> dict:
    values = {
        name: [read_axis(item.data, offset, encoding) for item in reports]
        for name, reports in segments.items()
    }
    neutral_initial = values["neutral_initial"]
    neutral_final = values["neutral_final"]
    neutral = neutral_initial + neutral_final
    circle = values["cw"] + values["ccw"]
    center_f = float(statistics.median(neutral))
    center = int(round(center_f))
    deviations = [abs(value - center_f) for value in neutral]
    low = percentile(circle, 0.01)
    high = percentile(circle, 0.99)
    span = high - low
    right = float(statistics.median(values["right"]))
    up = float(statistics.median(values["up"]))
    all_values = [read_axis(item.data, offset, encoding) for item in all_reports]
    width = 2 if encoding == "int16_le" else 1
    reasons: list[str] = []
    minimum_span = 160 if encoding == "uint8" else 36000
    if span < minimum_span:
        reasons.append(f"insufficient_circle_span:{span:.1f}<{minimum_span}")
    if len(set(circle)) < 8:
        reasons.append("button_or_discrete_field:fewer_than_8_circle_values")
    jitter_p99 = percentile(deviations, 0.99)
    jitter_p999 = percentile(deviations, 0.999)
    jitter_limit = max(3.0 if encoding == "uint8" else 700.0, span * 0.08)
    if jitter_p999 > jitter_limit:
        reasons.append(f"neutral_not_stable:p99.9={jitter_p999:.1f}>{jitter_limit:.1f}")
    drift = abs(statistics.median(neutral_initial) - statistics.median(neutral_final))
    drift_limit = max(3.0 if encoding == "uint8" else 700.0, span * 0.06)
    if drift > drift_limit:
        reasons.append(f"neutral_drift:{drift:.1f}>{drift_limit:.1f}")
    cw_span = percentile(values["cw"], 0.99) - percentile(values["cw"], 0.01)
    ccw_span = percentile(values["ccw"], 0.99) - percentile(values["ccw"], 0.01)
    if span and min(cw_span, ccw_span) / span < 0.62:
        reasons.append("bad_circle:one_direction_does_not_cover_envelope")
    counter_score = _increment_consistency(all_values, encoding)
    if counter_score >= 0.72:
        reasons.append(f"counter_like:increment_consistency={counter_score:.3f}")
    neutral_change = _change_rate(neutral_initial) + _change_rate(neutral_final)
    neutral_change /= 2.0
    neutral_range = max(neutral) - min(neutral)
    if neutral_change > 0.85 and span and neutral_range / span > 0.12:
        reasons.append(
            f"checksum_or_timestamp_like:neutral_change_rate={neutral_change:.3f}"
        )
    anchor_max = max(abs(right - center_f), abs(up - center_f))
    if span and anchor_max / span < 0.25:
        reasons.append("anchor_unresponsive")

    return {
        "offset": offset,
        "width": width,
        "encoding": encoding,
        "observed_center": center,
        "neutral": {
            "median": _round(center_f),
            "mad": _round(_mad(neutral, center_f)),
            "jitter_p99": _round(jitter_p99),
            "jitter_p99_9": _round(jitter_p999),
            "initial_median": _round(statistics.median(neutral_initial)),
            "final_median": _round(statistics.median(neutral_final)),
            "drift": _round(drift),
        },
        "anchors": {
            "right_median": _round(right),
            "right_delta": _round(right - center_f),
            "up_median": _round(up),
            "up_delta": _round(up - center_f),
        },
        "circle_envelope": {
            "minimum_observed": min(circle),
            "maximum_observed": max(circle),
            "q01": _round(low),
            "q99": _round(high),
            "robust_span": _round(span),
            "cw_robust_span": _round(cw_span),
            "ccw_robust_span": _round(ccw_span),
        },
        "classifier": {
            "unique_circle_values": len(set(circle)),
            "neutral_change_rate": _round(neutral_change),
            "increment_consistency": _round(counter_score),
        },
        "rejection_reasons": reasons,
        "_values": values,
    }


# The winding gate below ignores samples inside this normalized radius and
# requires at least this many turns in each direction.  Keep the area gate in
# the same physical units: normalized x/y are divided by the robust envelope
# spans, so a circle at the outer-sample radius has signed shoelace area
# ``pi*r*r*turns``.  This is a lower bound for a qualifying outer rotation,
# not a sample-rate-dependent per-report threshold.
_CIRCLE_OUTER_RADIUS_MIN = 0.16
_CIRCLE_MIN_TURNS = 1.25
_MIN_NORMALIZED_CIRCLE_AREA = (
    math.pi * _CIRCLE_OUTER_RADIUS_MIN ** 2 * _CIRCLE_MIN_TURNS
)


def _signed_circle_area(x_values: list[int], y_values: list[int],
                        center_x: float, center_y: float,
                        span_x: float, span_y: float) -> float:
    if len(x_values) != len(y_values) or len(x_values) < 3 or not span_x or not span_y:
        return 0.0
    points = [((x - center_x) / span_x, (y - center_y) / span_y)
              for x, y in zip(x_values, y_values)]
    total = sum(
        x1 * y2 - x2 * y1
        for (x1, y1), (x2, y2) in zip(points, points[1:] + points[:1])
    )
    # Standard signed shoelace area.  Do not divide by the number of samples:
    # doing so made the same physical circle fail as report cadence increased.
    return total / 2.0


def _winding_metrics(x_values: list[int], y_values: list[int],
                     center_x: float, center_y: float,
                     span_x: float, span_y: float,
                     angular_bins: int = 16) -> dict[str, float | int]:
    """Measure outer-circle winding while ignoring near-center transitions."""
    if len(x_values) != len(y_values) or len(x_values) < 3 or not span_x or not span_y:
        return {"signed_turns": 0.0, "absolute_turns": 0.0,
                "directional_consistency": 0.0, "angular_bins_covered": 0,
                "angular_bin_coverage": 0.0, "outer_samples": 0}
    angles: list[float] = []
    covered: set[int] = set()
    for x, y in zip(x_values, y_values):
        nx = (x - center_x) / span_x
        ny = (y - center_y) / span_y
        # An ideal outer circle has normalized radius about 0.5.  Excluding
        # near-center samples removes return-to-center/marker latency without
        # asking the operator to draw isolated axes or a laboratory-perfect
        # circle.
        if math.hypot(nx, ny) < _CIRCLE_OUTER_RADIUS_MIN:
            continue
        angle = math.atan2(ny, nx)
        angles.append(angle)
        covered.add(min(angular_bins - 1, int((angle + math.pi) * angular_bins /
                                               (2.0 * math.pi))))
    deltas: list[float] = []
    for previous, current in zip(angles, angles[1:]):
        delta = (current - previous + math.pi) % (2.0 * math.pi) - math.pi
        deltas.append(delta)
    signed_radians = sum(deltas)
    absolute_radians = sum(abs(delta) for delta in deltas)
    return {
        "signed_turns": _round(signed_radians / (2.0 * math.pi)),
        "absolute_turns": _round(absolute_radians / (2.0 * math.pi)),
        "directional_consistency": _round(
            abs(signed_radians) / absolute_radians if absolute_radians else 0.0
        ),
        "angular_bins_covered": len(covered),
        "angular_bin_coverage": _round(len(covered) / angular_bins),
        "outer_samples": len(angles),
    }


def _correlation(x_values: list[int], y_values: list[int]) -> float:
    if len(x_values) != len(y_values) or len(x_values) < 2:
        return 0.0
    mean_x = statistics.fmean(x_values)
    mean_y = statistics.fmean(y_values)
    xx = sum((value - mean_x) ** 2 for value in x_values)
    yy = sum((value - mean_y) ** 2 for value in y_values)
    if not xx or not yy:
        return 0.0
    xy = sum((x - mean_x) * (y - mean_y) for x, y in zip(x_values, y_values))
    return xy / math.sqrt(xx * yy)


def _pair_candidate(x: dict, y: dict) -> tuple[dict | None, str | None]:
    if x["encoding"] != y["encoding"]:
        return None, "mixed_encoding"
    x_bytes = set(range(x["offset"], x["offset"] + x["width"]))
    y_bytes = set(range(y["offset"], y["offset"] + y["width"]))
    if x_bytes & y_bytes:
        return None, "overlapping_fields"
    if x["rejection_reasons"] or y["rejection_reasons"]:
        return None, "field_rejected"
    span_x = x["circle_envelope"]["robust_span"]
    span_y = y["circle_envelope"]["robust_span"]
    xr = abs(x["anchors"]["right_delta"]) / span_x
    xu = abs(x["anchors"]["up_delta"]) / span_x
    yu = abs(y["anchors"]["up_delta"]) / span_y
    yr = abs(y["anchors"]["right_delta"]) / span_y
    if xr < 0.25 or yu < 0.25:
        return None, "anchor_motion_too_small"
    # Cross-axis motion is permitted.  Anchors must identify a dominant axis,
    # but there is deliberately no legacy 10% isolation threshold here.
    if xr < xu * 1.25 or yu < yr * 1.25:
        return None, "anchors_do_not_unambiguously_assign_axes"

    xv = x["_values"]
    yv = y["_values"]
    cw_area = _signed_circle_area(
        xv["cw"], yv["cw"], x["observed_center"], y["observed_center"],
        span_x, span_y,
    )
    ccw_area = _signed_circle_area(
        xv["ccw"], yv["ccw"], x["observed_center"], y["observed_center"],
        span_x, span_y,
    )
    cw_winding = _winding_metrics(
        xv["cw"], yv["cw"], x["observed_center"], y["observed_center"],
        span_x, span_y,
    )
    ccw_winding = _winding_metrics(
        xv["ccw"], yv["ccw"], x["observed_center"], y["observed_center"],
        span_x, span_y,
    )
    if (abs(cw_area) < _MIN_NORMALIZED_CIRCLE_AREA
            or abs(ccw_area) < _MIN_NORMALIZED_CIRCLE_AREA
            or cw_area * ccw_area >= 0):
        return None, "bad_circle:directions_are_not_opposite_closed_rotations"
    if (abs(cw_winding["signed_turns"]) < _CIRCLE_MIN_TURNS
            or abs(ccw_winding["signed_turns"]) < _CIRCLE_MIN_TURNS):
        return None, "bad_circle:each_direction_requires_at_least_1.25_outer_turns"
    if cw_winding["signed_turns"] * ccw_winding["signed_turns"] >= 0:
        return None, "bad_circle:winding_directions_are_not_opposite"
    if (cw_winding["angular_bin_coverage"] < 0.75
            or ccw_winding["angular_bin_coverage"] < 0.75):
        return None, "bad_circle:angular_bin_coverage_below_0.75"
    if (cw_winding["directional_consistency"] < 0.65
            or ccw_winding["directional_consistency"] < 0.65):
        return None, "bad_circle:winding_directional_consistency_below_0.65"
    circle_x = xv["cw"] + xv["ccw"]
    circle_y = yv["cw"] + yv["ccw"]
    ellipse_ratio = min(span_x, span_y) / max(span_x, span_y)
    if ellipse_ratio < 0.45:
        return None, "bad_circle:ellipse_axis_ratio_below_0.45"

    specificity_x = xr / max(1e-9, xr + xu)
    specificity_y = yu / max(1e-9, yu + yr)
    coverage_x = min(1.0, span_x / (220.0 if x["encoding"] == "uint8" else 56000.0))
    coverage_y = min(1.0, span_y / (220.0 if y["encoding"] == "uint8" else 56000.0))
    neutral_x = 1.0 - min(1.0, x["neutral"]["jitter_p99_9"] / max(1.0, span_x * .08))
    neutral_y = 1.0 - min(1.0, y["neutral"]["jitter_p99_9"] / max(1.0, span_y * .08))
    orientation = min(
        1.0,
        min(abs(cw_area), abs(ccw_area)) / _MIN_NORMALIZED_CIRCLE_AREA,
    )
    confidence = (
        0.25 * min(specificity_x, specificity_y)
        + 0.20 * min(coverage_x, coverage_y)
        + 0.20 * min(neutral_x, neutral_y)
        + 0.20 * orientation
        + 0.15 * ellipse_ratio
    )
    return {
        "x_offset": x["offset"],
        "y_offset": y["offset"],
        "encoding": x["encoding"],
        "confidence": _round(confidence),
        "anchor_specificity": {
            "x": _round(specificity_x), "y": _round(specificity_y),
            "x_cross_ratio": _round(xu / max(xr, 1e-9)),
            "y_cross_ratio": _round(yr / max(yu, 1e-9)),
        },
        "circle_geometry": {
            "cw_signed_area": _round(cw_area),
            "ccw_signed_area": _round(ccw_area),
            "cw_winding": cw_winding,
            "ccw_winding": ccw_winding,
            "ellipse_axis_span_ratio": _round(ellipse_ratio),
            "axis_correlation": _round(_correlation(circle_x, circle_y)),
        },
        "_x": x,
        "_y": y,
    }, None


def _public_field(field: dict) -> dict:
    return {key: value for key, value in field.items() if not key.startswith("_")}


def _validate_descriptors(args: argparse.Namespace) -> tuple[bytes, bytes | None]:
    paths = [args.device_descriptor, args.configuration_descriptor]
    for path in paths:
        if not path.is_file():
            raise ProfileError(f"missing descriptor file: {path}")
    hid_path = getattr(args, "hid_report_descriptor", None)
    no_hid = bool(getattr(args, "no_hid_report_descriptor", False))
    if (hid_path is None) == (not no_hid):
        raise ProfileError(
            "guided mode requires exactly one of a HID report descriptor "
            "or explicit no-HID declaration"
        )
    if hid_path is not None and not hid_path.is_file():
        raise ProfileError(f"missing descriptor file: {hid_path}")
    device_bytes = args.device_descriptor.read_bytes()
    if len(device_bytes) != 18 or device_bytes[1] != 1:
        raise ProfileError("device descriptor must be the exact 18-byte USB descriptor")
    captured = (
        int.from_bytes(device_bytes[8:10], "little"),
        int.from_bytes(device_bytes[10:12], "little"),
        int.from_bytes(device_bytes[12:14], "little"),
    )
    if captured != (args.vid, args.pid, args.bcd_device):
        raise ProfileError("CLI VID/PID/bcdDevice do not match the captured device descriptor")
    return (
        descriptor_bundle_sha(args.device_descriptor, args.configuration_descriptor),
        sha256(hid_path) if hid_path is not None else None,
    )


def build_guided_candidate(args: argparse.Namespace) -> dict:
    """Analyze one guided trace and return a review-only adapter candidate."""
    descriptor_digest, hid_digest = _validate_descriptors(args)
    reports = load_guided_reports(
        args.guided_trace, args.endpoint, args.report_id, args.report_length
    )
    segments = _guided_segments(reports)
    first_offset = 1 if args.report_id else 0
    fields: list[dict] = []
    for offset in range(first_offset, args.report_length):
        fields.append(_field_metrics(offset, "uint8", segments, reports))
    for offset in range(first_offset, args.report_length - 1):
        fields.append(_field_metrics(offset, "int16_le", segments, reports))

    viable = [field for field in fields if not field["rejection_reasons"]]
    pairs: list[dict] = []
    pair_rejections = Counter()
    for x in viable:
        for y in viable:
            if x is y:
                continue
            pair, reason = _pair_candidate(x, y)
            if pair:
                pairs.append(pair)
            elif reason:
                pair_rejections[reason] += 1
    pairs.sort(key=lambda pair: (-pair["confidence"], pair["encoding"],
                                 pair["x_offset"], pair["y_offset"]))
    if not pairs:
        reasons = ", ".join(f"{key}={value}" for key, value in sorted(pair_rejections.items()))
        raise ProfileError("no high-confidence X/Y field pair; " + (reasons or "all fields rejected"))
    best = pairs[0]
    if best["confidence"] < 0.72:
        raise ProfileError(f"best axis pair confidence {best['confidence']:.3f} is below 0.720")
    if len(pairs) > 1 and best["confidence"] - pairs[1]["confidence"] < 0.035:
        other = pairs[1]
        raise ProfileError(
            "ambiguous axes: top candidates are too close "
            f"({best['encoding']} {best['x_offset']}/{best['y_offset']}={best['confidence']:.3f}, "
            f"{other['encoding']} {other['x_offset']}/{other['y_offset']}={other['confidence']:.3f})"
        )

    x = best["_x"]
    y = best["_y"]
    encoding = best["encoding"]
    protocol_min, protocol_center, protocol_max = (
        (0, 128, 255) if encoding == "uint8" else (-32768, 0, 32767)
    )
    center_tolerance = 8 if encoding == "uint8" else 1800
    if abs(x["observed_center"] - protocol_center) > center_tolerance or \
            abs(y["observed_center"] - protocol_center) > center_tolerance:
        raise ProfileError(
            "observed neutral does not support canonical protocol center; "
            "per-device center correction is forbidden"
        )
    if encoding == "uint8":
        envelope_supported = all(
            field["circle_envelope"]["q01"] <= 40
            and field["circle_envelope"]["q99"] >= 215
            for field in (x, y)
        )
    else:
        envelope_supported = all(
            field["circle_envelope"]["q01"] <= -23000
            and field["circle_envelope"]["q99"] >= 23000
            for field in (x, y)
        )
    if not envelope_supported:
        raise ProfileError(
            "insufficient outer-circle coverage to propose protocol min/max constants"
        )

    numeric_span = float(protocol_max - protocol_min)

    def protocol_metrics(field: dict) -> dict[str, float]:
        center_agreement = max(
            0.0, 1.0 - abs(field["observed_center"] - protocol_center) / center_tolerance
        )
        envelope_coverage = min(
            1.0, field["circle_envelope"]["robust_span"] / numeric_span
        )
        boundary_agreement = max(
            0.0,
            1.0 - (
                abs(field["circle_envelope"]["q01"] - protocol_min)
                + abs(protocol_max - field["circle_envelope"]["q99"])
            ) / numeric_span,
        )
        confidence = min(0.90, 0.45 * center_agreement + 0.55 * envelope_coverage)
        return {
            "canonical_center_agreement": _round(center_agreement),
            "observed_numeric_envelope_coverage": _round(envelope_coverage),
            "numeric_boundary_agreement": _round(boundary_agreement),
            "hypothesis_confidence_capped_at_0_90": _round(confidence),
        }

    protocol_evidence = {"x": protocol_metrics(x), "y": protocol_metrics(y)}
    protocol_confidence = min(
        protocol_evidence["x"]["hypothesis_confidence_capped_at_0_90"],
        protocol_evidence["y"]["hypothesis_confidence_capped_at_0_90"],
    )
    cw_winding = best["circle_geometry"]["cw_winding"]
    ccw_winding = best["circle_geometry"]["ccw_winding"]
    circle_confidence = _round(
        0.40 * min(1.0, min(abs(cw_winding["signed_turns"]),
                            abs(ccw_winding["signed_turns"])) / 1.75)
        + 0.30 * min(cw_winding["angular_bin_coverage"],
                     ccw_winding["angular_bin_coverage"])
        + 0.30 * min(cw_winding["directional_consistency"],
                     ccw_winding["directional_consistency"])
    )

    def axis(name: str, field: dict) -> dict:
        anchor_delta = field["anchors"]["right_delta" if name == "x" else "up_delta"]
        return {
            "offset": field["offset"],
            "encoding": encoding,
            "sign": 1 if anchor_delta > 0 else -1,
            "protocol_constants_proposed": {
                "center": protocol_center,
                "minimum": protocol_min,
                "maximum": protocol_max,
                "source": "encoding_contract_hypothesis_requires_review",
                "hypothesis_evidence": protocol_evidence[name],
                "runtime_per_device_center_correction": False,
            },
            "observed_device_evidence": {
                "center": field["observed_center"],
                "neutral": field["neutral"],
                "circle_envelope": field["circle_envelope"],
                "anchors": field["anchors"],
            },
        }

    alternate_pairs = [
        {key: value for key, value in pair.items() if not key.startswith("_")}
        for pair in pairs[1:9]
    ]
    rejected_fields = [_public_field(field) for field in fields if field["rejection_reasons"]]
    candidate = {
        "schema": "makcm.controller-stick-adapter-candidate.v1",
        "format_id": args.format_id,
        "adapter_version": args.format_version,
        "publication": {
            "state": "CANDIDATE_ONLY_REVIEW_REQUIRED",
            "firmware_cpp_emitted": False,
            "review_required": True,
        },
        "identity": {
            "vid": args.vid,
            "pid": args.pid,
            "bcd_device": args.bcd_device,
            "descriptor_bundle_sha256": descriptor_digest.hex(),
            "descriptor_hash32": compact_hash(descriptor_digest),
            "hid_report_present": hid_digest is not None,
            "hid_report_sha256": hid_digest.hex() if hid_digest is not None else None,
            "hid_report_hash32": compact_hash(hid_digest) if hid_digest is not None else 0,
            "interface_number": args.interface,
            "endpoint_address": args.endpoint,
            "report_id": args.report_id,
            "report_length": args.report_length,
            "format_version": args.format_version,
            "variant": args.variant,
        },
        "layout": {
            "encoding": encoding,
            "x_offset": x["offset"],
            "y_offset": y["offset"],
            "x_sign": 1 if x["anchors"]["right_delta"] > 0 else -1,
            "y_sign": 1 if y["anchors"]["up_delta"] > 0 else -1,
        },
        "axes": {"x": axis("x", x), "y": axis("y", y)},
        "qualification": {
            "gate": "HIGH_CONFIDENCE_CANDIDATE",
            "decision_confidence": best["confidence"],
            "decisions": {
                "axis_identity_and_sign": _round(min(
                    best["anchor_specificity"]["x"], best["anchor_specificity"]["y"]
                )),
                "encoding_and_offsets": best["confidence"],
                "bidirectional_circle": circle_confidence,
                "protocol_constants": protocol_confidence,
            },
            "protocol_constant_evidence": {
                "status": "ENCODING_HYPOTHESIS_REVIEW_REQUIRED",
                "confidence_cap": 0.90,
                "metrics": protocol_evidence,
            },
            "anchor_evidence": best["anchor_specificity"],
            "ellipse_and_coupling_evidence": best["circle_geometry"],
            "segment_counts": {name: len(rows) for name, rows in segments.items()},
            "cadence": cadence([Report(item.timestamp_s, item.endpoint, item.data)
                                for item in reports]),
            "alternate_pairs": alternate_pairs,
            "rejected_fields": rejected_fields,
            "pair_rejection_summary": dict(sorted(pair_rejections.items())),
        },
        "integrity": {
            "update_rule": "unresolved_requires_manual_review",
            "suspected_counter_checksum_or_timestamp_fields": [
                {"offset": field["offset"], "encoding": field["encoding"],
                 "reasons": field["rejection_reasons"]}
                for field in fields
                if any("counter" in reason or "checksum" in reason or "timestamp" in reason
                       for reason in field["rejection_reasons"])
            ],
        },
        "evidence": {
            "device_descriptor": str(args.device_descriptor),
            "configuration_descriptor": str(args.configuration_descriptor),
            "hid_report_descriptor": (
                str(args.hid_report_descriptor) if hid_digest is not None else None
            ),
            "hid_report_present": hid_digest is not None,
            "guided_trace": str(args.guided_trace),
            "guided_trace_sha256": sha256(args.guided_trace).hex(),
        },
    }
    candidate["profile_sha256"] = canonical_hash(candidate)
    return candidate


def build_profile(args: argparse.Namespace) -> dict:
    if getattr(args, "hid_report_descriptor", None) is None:
        raise ProfileError("legacy mode requires a real HID report descriptor")
    paths = [args.device_descriptor, args.configuration_descriptor,
             args.hid_report_descriptor]
    for path in paths:
        if not path.is_file():
            raise ProfileError(f"missing descriptor file: {path}")
    device_bytes = args.device_descriptor.read_bytes()
    if len(device_bytes) != 18 or device_bytes[1] != 1:
        raise ProfileError("device descriptor must be the exact 18-byte USB descriptor")
    captured_vid = int.from_bytes(device_bytes[8:10], "little")
    captured_pid = int.from_bytes(device_bytes[10:12], "little")
    captured_bcd = int.from_bytes(device_bytes[12:14], "little")
    if (captured_vid, captured_pid, captured_bcd) != (args.vid, args.pid, args.bcd_device):
        raise ProfileError(
            "CLI VID/PID/bcdDevice do not match the captured device descriptor"
        )

    captures = {
        name: load_reports(path, args.endpoint, args.report_id, args.report_length)
        for name, path in {
            "neutral": args.neutral,
            "x_negative": args.x_negative,
            "x_positive": args.x_positive,
            "y_negative": args.y_negative,
            "y_positive": args.y_positive,
        }.items()
    }
    x = {name: axis_values(rows, args.x_offset, args.encoding)
         for name, rows in captures.items()}
    y = {name: axis_values(rows, args.y_offset, args.encoding)
         for name, rows in captures.items()}
    x_axis = analyze_axis("x", x["neutral"], x["x_negative"],
                          x["x_positive"], args.encoding)
    y_axis = analyze_axis("y", y["neutral"], y["y_negative"],
                          y["y_positive"], args.encoding)
    isolation = verify_axis_isolation(
        int(x_axis["center"]), int(y_axis["center"]),
        x["x_negative"], y["x_negative"],
        x["x_positive"], y["x_positive"],
        x["y_negative"], y["y_negative"],
        x["y_positive"], y["y_positive"],
    )

    descriptor_digest = descriptor_bundle_sha(
        args.device_descriptor, args.configuration_descriptor)
    hid_digest = sha256(args.hid_report_descriptor)
    profile = {
        "schema": "makcm.controller-stick-format.v1",
        "format_id": args.format_id,
        "cal_id": args.cal_id,
        "identity": {
            "vid": args.vid,
            "pid": args.pid,
            "bcd_device": args.bcd_device,
            "descriptor_bundle_sha256": descriptor_digest.hex(),
            "descriptor_hash32": compact_hash(descriptor_digest),
            "hid_report_sha256": hid_digest.hex(),
            "hid_report_hash32": compact_hash(hid_digest),
            "interface_number": args.interface,
            "endpoint_address": args.endpoint,
            "report_id": args.report_id,
            "report_length": args.report_length,
            "format_version": args.format_version,
            "variant": args.variant,
        },
        "axes": {
            "x": {"offset": args.x_offset, "encoding": args.encoding, **x_axis},
            "y": {"offset": args.y_offset, "encoding": args.encoding, **y_axis},
        },
        "quality": {
            "axis_isolation": isolation,
            "cadence": cadence(captures["neutral"]),
            "capture_counts": {name: len(rows) for name, rows in captures.items()},
            "integrity_fields": "not_enabled",
            "gate": "PASS",
        },
        "evidence": {
            "device_descriptor": str(args.device_descriptor),
            "configuration_descriptor": str(args.configuration_descriptor),
            "hid_report_descriptor": str(args.hid_report_descriptor),
            "reports": {
                name: str(getattr(args, name)) for name in captures
            },
        },
    }
    profile["profile_sha256"] = canonical_hash(profile)
    return profile


def cpp_array(data: bytes) -> str:
    return "{" + ",".join(f"0x{byte:02X}" for byte in data) + "}"


def write_cpp_entry(profile: dict, path: Path) -> None:
    identity = profile["identity"]
    x = profile["axes"]["x"]
    y = profile["axes"]["y"]
    descriptor_sha = bytes.fromhex(identity["descriptor_bundle_sha256"])
    hid_sha = bytes.fromhex(identity["hid_report_sha256"])
    encoding = "CodecFieldEncoding::Int16LE" if x["encoding"] == "int16_le" \
        else "CodecFieldEncoding::UInt8"
    text = f"""// Generated by tools/stick_profile_tool.py; do not hand-edit.
{{
    \"{profile['format_id']}\", \"{profile['cal_id']}\",
    0x{identity['vid']:04X}, 0x{identity['pid']:04X}, 0x{identity['bcd_device']:04X},
    0x{identity['descriptor_hash32']:08X}, 0x{identity['hid_report_hash32']:08X},
    {cpp_array(descriptor_sha)}, {cpp_array(hid_sha)},
    {identity['interface_number']}, 0x{identity['endpoint_address']:02X},
    0x{identity['report_id']:02X}, {identity['report_length']},
    {identity['format_version']}, {identity['variant']}, {encoding},
    {{{x['offset']}, {x['center']}, {x['minimum']}, {x['maximum']}, {x['sign']}}},
    {{{y['offset']}, {y['center']}, {y['minimum']}, {y['maximum']}, {y['sign']}}},
}},
"""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--format-id", required=True)
    result.add_argument("--cal-id", default="none")
    result.add_argument("--vid", type=parse_int, required=True)
    result.add_argument("--pid", type=parse_int, required=True)
    result.add_argument("--bcd-device", type=parse_int, required=True)
    result.add_argument("--interface", type=parse_int, required=True)
    result.add_argument("--endpoint", type=parse_int, required=True)
    result.add_argument("--report-id", type=parse_int, default=0)
    result.add_argument("--report-length", type=parse_int, required=True)
    result.add_argument("--format-version", type=parse_int, default=1)
    result.add_argument("--variant", type=parse_int, default=0)
    result.add_argument("--encoding", choices=("uint8", "int16_le"))
    result.add_argument("--x-offset", type=parse_int)
    result.add_argument("--y-offset", type=parse_int)
    result.add_argument("--device-descriptor", type=Path, required=True)
    result.add_argument("--configuration-descriptor", type=Path, required=True)
    hid = result.add_mutually_exclusive_group()
    hid.add_argument("--hid-report-descriptor", type=Path)
    hid.add_argument("--no-hid-report-descriptor", action="store_true",
                     help="guided vendor interface has no HID Report Descriptor")
    result.add_argument("--guided-trace", type=Path,
                        help="marker-bearing guided CSV; emits JSON candidate only")
    result.add_argument("--neutral", type=Path)
    result.add_argument("--x-negative", type=Path)
    result.add_argument("--x-positive", type=Path)
    result.add_argument("--y-negative", type=Path)
    result.add_argument("--y-positive", type=Path)
    result.add_argument("--out-json", type=Path, required=True)
    result.add_argument("--out-cpp", type=Path)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.guided_trace:
            if (args.hid_report_descriptor is None) == (not args.no_hid_report_descriptor):
                raise ProfileError(
                    "guided mode requires exactly one of --hid-report-descriptor "
                    "or --no-hid-report-descriptor"
                )
            if args.out_cpp:
                raise ProfileError("guided mode never emits firmware C++; omit --out-cpp")
            profile = build_guided_candidate(args)
        else:
            required = ("encoding", "x_offset", "y_offset", "neutral", "x_negative",
                        "x_positive", "y_negative", "y_positive", "out_cpp",
                        "hid_report_descriptor")
            missing = [name.replace("_", "-") for name in required
                       if getattr(args, name) is None]
            if missing:
                raise ProfileError("legacy mode missing required arguments: " + ", ".join(missing))
            if args.no_hid_report_descriptor:
                raise ProfileError("legacy mode requires a real HID report descriptor")
            profile = build_profile(args)
        args.out_json.parent.mkdir(parents=True, exist_ok=True)
        args.out_json.write_text(
            json.dumps(profile, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        if not args.guided_trace:
            write_cpp_entry(profile, args.out_cpp)
    except ProfileError as exc:
        print(f"FAIL: {exc}")
        return 2
    print(f"PASS: {args.out_json} sha256={profile['profile_sha256']}")
    if args.guided_trace:
        print("candidate only: review required; no firmware C++ emitted")
    else:
        print(f"generated: {args.out_cpp}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
