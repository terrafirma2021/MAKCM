#!/usr/bin/env python3
"""Capture or convert guided controller USB evidence for stick qualification.

Live capture parses classic pcap/USBPcap records directly.  tshark is used only
as a secondary readability/version check because USB class sub-dissectors may
consume payload bytes and USBPcap records do not expose ``usb.urb_type``.
The legacy offline converter still accepts deterministic tshark TSV input.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import importlib
import io
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
from bisect import bisect_left
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Callable, Iterable


class CaptureError(ValueError):
    pass


TSHARK_FIELDS = (
    "frame.time_epoch",
    "usb.urb_type",
    "usb.bus_id",
    "usb.device_address",
    "usb.endpoint_address",
    "usb.urb_id",
    "usb.data_len",
    "usb.capdata",
)

REQUIRED_MARKERS = (
    "neutral_begin",
    "right_begin",
    "up_begin",
    "cw_begin",
    "ccw_begin",
    "final_neutral_begin",
)

MARKER_ORDER = (
    "neutral_begin", "neutral_end",
    "right_begin", "right_end",
    "up_begin", "up_end",
    "cw_begin", "cw_end",
    "ccw_begin", "ccw_end",
    "final_neutral_begin", "final_neutral_end",
)

ACTION_PROMPTS = (
    ("neutral_begin", "完全松开右摇杆并保持回中；按 Enter 开始约 2 秒的中心段。"),
    ("neutral_end", "继续完全不碰右摇杆；保持约 2 秒后按 Enter 结束中心段。"),
    ("right_begin", "把右摇杆推到物理最右（少量上下偏差没关系）；按 Enter 开始右锚点。"),
    ("right_end", "继续保持最右约 1 秒，然后按 Enter 结束右锚点。"),
    ("up_begin", "把右摇杆推到物理最上（少量左右偏差没关系）；按 Enter 开始上锚点。"),
    ("up_end", "继续保持最上约 1 秒，然后按 Enter 结束上锚点。"),
    ("cw_begin", "把右摇杆推到外圈边缘；按 Enter 后沿外圈顺时针转约两圈。"),
    ("cw_end", "顺时针两圈完成后停住并按 Enter 结束；速度均匀即可，不必画得完美。"),
    ("ccw_begin", "仍在外圈边缘；按 Enter 后沿外圈逆时针转约两圈。"),
    ("ccw_end", "逆时针两圈完成后停住并按 Enter 结束；速度均匀即可，不必画得完美。"),
    ("final_neutral_begin", "完全松开右摇杆让它自行回中；按 Enter 开始最终中心段。"),
    ("final_neutral_end", "继续完全不碰右摇杆；保持约 2 秒后按 Enter 结束并等待处理。"),
)

PREFLIGHT_READY = 0
PREFLIGHT_CAPTURE_MISSING = 3
PREFLIGHT_DESCRIPTOR_MISSING = 4
PREFLIGHT_BOTH_MISSING = 5
USBPCAP_MIN_VERSION = (1, 5, 4, 0)
USBPCAP_LINKTYPE = 249
USBPCAP_BASE_HEADER_LEN = 27
USBPCAP_TRANSFER_INTERRUPT = 1
USBPCAP_INFO_PDO_TO_FDO = 0x01
USBPCAP_PARSER_SCHEMA = "makcm.classic-pcap-usbpcap.v1"
DISCOVERY_DURATION_TOLERANCE_S = Decimal("0.25")
USBPCAP_READINESS_TIMEOUT_S = 15.0
USBPCAP_READINESS_POLL_S = 0.02
USBPCAP_MIN_OPERATIONAL_SIZE = 24 + 16 + USBPCAP_BASE_HEADER_LEN


@dataclass(frozen=True)
class Completion:
    timestamp: Decimal
    timestamp_text: str
    endpoint: int
    urb_id: int
    payload: bytes


@dataclass(frozen=True)
class Marker:
    name: str
    epoch: Decimal


@dataclass(frozen=True)
class USBPcapRecord:
    record_number: int
    timestamp: Decimal
    timestamp_text: str
    irp_id: int
    status: int
    function: int
    info: int
    bus: int
    device: int
    endpoint: int
    transfer: int
    header_len: int
    data_length: int
    payload: bytes

    @property
    def is_completion(self) -> bool:
        return bool(self.info & USBPCAP_INFO_PDO_TO_FDO)


@dataclass(frozen=True)
class USBPcapCapture:
    path: Path
    snaplen: int
    records: tuple[USBPcapRecord, ...]
    earliest: Decimal
    latest: Decimal
    sha256: str

    @property
    def observed_duration(self) -> Decimal:
        return self.latest - self.earliest


@dataclass(frozen=True)
class CaptureReadiness:
    method: str
    validation_deferred: bool
    observed_size_bytes: int
    first_record: USBPcapCapture | None


def parse_int(value: str | int, field: str) -> int:
    if isinstance(value, int):
        return value
    text = str(value).strip()
    if not text:
        raise CaptureError(f"{field} is empty")
    try:
        return int(text, 0)
    except ValueError as exc:
        if text.isdecimal():
            return int(text, 10)
        raise CaptureError(f"invalid {field}: {value!r}") from exc


def parse_decimal(value: object, field: str) -> Decimal:
    try:
        result = Decimal(str(value).strip())
    except (InvalidOperation, AttributeError) as exc:
        raise CaptureError(f"invalid {field}: {value!r}") from exc
    if not result.is_finite() or result < 0:
        raise CaptureError(f"{field} must be finite and non-negative")
    return result


def normalize_capdata(value: str) -> bytes:
    text = value.strip()
    for separator in (":", " ", "-", ","):
        text = text.replace(separator, "")
    if not text:
        return b""
    if len(text) % 2:
        raise CaptureError("usb.capdata contains an odd number of hex digits")
    try:
        return bytes.fromhex(text)
    except ValueError as exc:
        raise CaptureError(f"malformed usb.capdata: {value!r}") from exc


def classify_urb_type(value: str) -> str:
    text = value.strip().upper()
    textual = {
        "C": "complete", "'C'": "complete", '"C"': "complete",
        "URB_COMPLETE": "complete",
        "S": "submit", "'S'": "submit", '"S"': "submit",
        "URB_SUBMIT": "submit",
        "E": "error", "'E'": "error", '"E"': "error",
        "URB_ERROR": "error",
    }
    if text in textual:
        return textual[text]
    try:
        numeric = int(text, 0)
    except ValueError as exc:
        raise CaptureError(f"invalid usb.urb_type: {value!r}") from exc
    numeric_types = {0x43: "complete", 0x53: "submit", 0x45: "error"}
    if numeric not in numeric_types:
        raise CaptureError(f"unsupported usb.urb_type: {value!r}")
    return numeric_types[numeric]


def is_urb_complete(value: str) -> bool:
    return classify_urb_type(value) == "complete"


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_capture_selection(bus: int, device: int, endpoint: int) -> None:
    if not 1 <= bus <= 0xFFFF:
        raise CaptureError("bus must be in the range 1..65535")
    if not 1 <= device <= 127:
        raise CaptureError("USB device address must be in the range 1..127")
    if not 0x81 <= endpoint <= 0x8F:
        raise CaptureError("endpoint must be an interrupt IN endpoint (0x81..0x8F)")


def _validate_classic_pcap_global_header(data: bytes, path: Path) -> int:
    if len(data) < 24:
        raise CaptureError(f"{path}: truncated classic pcap global header")
    magic = data[:4]
    if magic != b"\xd4\xc3\xb2\xa1":
        if magic in (b"\xa1\xb2\xc3\xd4", b"\x4d\x3c\xb2\xa1", b"\xa1\xb2\x3c\x4d"):
            raise CaptureError(
                f"{path}: unsupported pcap byte order/timestamp precision; "
                "USBPcap little-endian microsecond classic pcap is required"
            )
        raise CaptureError(f"{path}: invalid classic pcap magic {magic.hex()}")
    major, minor, thiszone, sigfigs, snaplen, linktype = struct.unpack_from(
        "<HHiIII", data, 4
    )
    if (major, minor) != (2, 4):
        raise CaptureError(f"{path}: unsupported pcap version {major}.{minor}")
    if thiszone != 0 or sigfigs != 0:
        raise CaptureError(f"{path}: noncanonical pcap timezone/sigfigs")
    if not 1 <= snaplen <= 128 * 1024 * 1024:
        raise CaptureError(f"{path}: invalid pcap snaplen {snaplen}")
    if linktype != USBPCAP_LINKTYPE:
        raise CaptureError(
            f"{path}: linktype {linktype} is not USBPcap ({USBPCAP_LINKTYPE})"
        )
    return snaplen


def _parse_classic_usbpcap_data(data: bytes, path: Path) -> USBPcapCapture:
    snaplen = _validate_classic_pcap_global_header(data, path)

    records: list[USBPcapRecord] = []
    offset = 24
    record_number = 0
    while offset < len(data):
        record_number += 1
        if len(data) - offset < 16:
            raise CaptureError(f"{path}: truncated pcap record header #{record_number}")
        seconds, micros, captured_len, original_len = struct.unpack_from("<IIII", data, offset)
        offset += 16
        if micros >= 1_000_000:
            raise CaptureError(f"{path}: record #{record_number} has invalid microseconds")
        if captured_len == 0:
            raise CaptureError(f"{path}: record #{record_number} is empty")
        if captured_len > snaplen:
            raise CaptureError(
                f"{path}: record #{record_number} captured length {captured_len} exceeds snaplen"
            )
        if captured_len != original_len:
            raise CaptureError(
                f"{path}: record #{record_number} is truncated "
                f"({captured_len} captured, {original_len} original)"
            )
        if captured_len > len(data) - offset:
            raise CaptureError(f"{path}: truncated pcap record data #{record_number}")
        packet = data[offset:offset + captured_len]
        offset += captured_len
        if len(packet) < USBPCAP_BASE_HEADER_LEN:
            raise CaptureError(
                f"{path}: record #{record_number} is shorter than the USBPcap header"
            )
        header_len = struct.unpack_from("<H", packet, 0)[0]
        if not USBPCAP_BASE_HEADER_LEN <= header_len <= len(packet):
            raise CaptureError(
                f"{path}: record #{record_number} has invalid USBPcap headerLen {header_len}"
            )
        irp_id = struct.unpack_from("<Q", packet, 2)[0]
        status = struct.unpack_from("<I", packet, 10)[0]
        function = struct.unpack_from("<H", packet, 14)[0]
        info = packet[16]
        if info & ~USBPCAP_INFO_PDO_TO_FDO:
            raise CaptureError(
                f"{path}: record #{record_number} has reserved irp_info bits 0x{info:02X}"
            )
        bus = struct.unpack_from("<H", packet, 17)[0]
        device = struct.unpack_from("<H", packet, 19)[0]
        endpoint = packet[21]
        transfer = packet[22]
        data_length = struct.unpack_from("<I", packet, 23)[0]
        if transfer == USBPCAP_TRANSFER_INTERRUPT and header_len != USBPCAP_BASE_HEADER_LEN:
            raise CaptureError(
                f"{path}: record #{record_number} interrupt transfer headerLen "
                f"{header_len} != {USBPCAP_BASE_HEADER_LEN}"
            )
        payload = packet[header_len:]
        if data_length != len(payload):
            raise CaptureError(
                f"{path}: record #{record_number} dataLength {data_length} "
                f"!= bytes after headerLen {len(payload)}"
            )
        if bus == 0 or device > 127:
            raise CaptureError(
                f"{path}: record #{record_number} has invalid bus/device {bus}/{device}"
            )
        timestamp_text = f"{seconds}.{micros:06d}"
        records.append(USBPcapRecord(
            record_number=record_number,
            timestamp=Decimal(seconds) + (Decimal(micros) / Decimal(1_000_000)),
            timestamp_text=timestamp_text,
            irp_id=irp_id,
            status=status,
            function=function,
            info=info,
            bus=bus,
            device=device,
            endpoint=endpoint,
            transfer=transfer,
            header_len=header_len,
            data_length=data_length,
            payload=payload,
        ))
    if not records:
        raise CaptureError(f"{path}: pcap contains no records")
    timestamps = [record.timestamp for record in records]
    for previous, current in zip(timestamps, timestamps[1:]):
        if current < previous:
            raise CaptureError(f"{path}: pcap record timestamps are not monotonic")
    return USBPcapCapture(
        path=path,
        snaplen=snaplen,
        records=tuple(records),
        earliest=timestamps[0],
        latest=timestamps[-1],
        sha256=hashlib.sha256(data).hexdigest(),
    )


def parse_classic_usbpcap(path: Path) -> USBPcapCapture:
    """Strictly parse USBPcap's little-endian, microsecond classic pcap."""
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise CaptureError(f"cannot read raw capture {path}: {exc}") from exc
    return _parse_classic_usbpcap_data(data, path)


def payload_completions(
    capture: USBPcapCapture,
    *,
    bus: int | None = None,
    device: int | None = None,
    endpoint: int | None = None,
) -> list[USBPcapRecord]:
    if any(value is not None for value in (bus, device, endpoint)):
        if None in (bus, device, endpoint):
            raise CaptureError("bus/device/endpoint selection must be complete")
        validate_capture_selection(int(bus), int(device), int(endpoint))
    selected: list[USBPcapRecord] = []
    for record in capture.records:
        if not record.is_completion:
            continue
        if record.transfer != USBPCAP_TRANSFER_INTERRUPT:
            continue
        if not 0x81 <= record.endpoint <= 0x8F or record.data_length == 0:
            continue
        if bus is not None and (
            record.bus, record.device, record.endpoint
        ) != (bus, device, endpoint):
            continue
        selected.append(record)
    return selected


def write_usbpcap_tsv(records: Iterable[USBPcapRecord], path: Path) -> None:
    output = io.StringIO(newline="")
    writer = csv.DictWriter(
        output, fieldnames=TSHARK_FIELDS, delimiter="\t", lineterminator="\n"
    )
    writer.writeheader()
    for record in records:
        writer.writerow({
            "frame.time_epoch": record.timestamp_text,
            "usb.urb_type": "C",
            "usb.bus_id": record.bus,
            "usb.device_address": record.device,
            "usb.endpoint_address": f"0x{record.endpoint:02X}",
            "usb.urb_id": f"0x{record.irp_id:X}",
            "usb.data_len": record.data_length,
            "usb.capdata": record.payload.hex(),
        })
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(output.getvalue(), encoding="utf-8", newline="")


def _marker_epoch(value: object, clock: str | None) -> Decimal:
    if isinstance(value, dict):
        if "epoch_ns" in value:
            return parse_decimal(value["epoch_ns"], "marker epoch_ns") / Decimal(1_000_000_000)
        for key in ("timestamp_epoch_s", "epoch_s", "timestamp_s"):
            if key in value:
                return parse_decimal(value[key], f"marker {key}")
        raise CaptureError("marker object requires epoch_ns or timestamp_epoch_s")
    parsed = parse_decimal(value, "marker timestamp")
    if clock == "unix_epoch_ns" or (clock is None and parsed >= Decimal("1e15")):
        return parsed / Decimal(1_000_000_000)
    return parsed


def load_markers(path: Path) -> list[Marker]:
    try:
        document = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise CaptureError(f"cannot read marker JSON {path}: {exc}") from exc
    clock: str | None = None
    raw: object = document
    if isinstance(document, dict) and "markers" in document:
        clock = document.get("clock")
        raw = document["markers"]

    items: list[tuple[str, object]] = []
    if isinstance(raw, dict):
        items = list(raw.items())
    elif isinstance(raw, list):
        for index, item in enumerate(raw):
            if not isinstance(item, dict) or "marker" not in item:
                raise CaptureError(f"marker entry {index} requires a marker name")
            if "epoch_ns" in item:
                value: object = {"epoch_ns": item["epoch_ns"]}
            elif "timestamp_epoch_s" in item:
                value = {"timestamp_epoch_s": item["timestamp_epoch_s"]}
            else:
                raise CaptureError(f"marker entry {index} requires epoch_ns")
            items.append((str(item["marker"]), value))
    else:
        raise CaptureError("marker JSON must contain a marker mapping or list")

    allowed = set(MARKER_ORDER)
    seen: set[str] = set()
    markers: list[Marker] = []
    for raw_name, value in items:
        name = raw_name.strip().lower()
        if name not in allowed:
            raise CaptureError(f"unknown marker {name!r}")
        if name in seen:
            raise CaptureError(f"duplicate marker {name!r}")
        seen.add(name)
        markers.append(Marker(name, _marker_epoch(value, clock)))
    missing = [name for name in REQUIRED_MARKERS if name not in seen]
    if missing:
        raise CaptureError("missing required markers: " + ", ".join(missing))

    by_name = {item.name: item for item in markers}
    semantic = [by_name[name] for name in MARKER_ORDER if name in by_name]
    for previous, current in zip(semantic, semantic[1:]):
        if current.epoch <= previous.epoch:
            raise CaptureError(
                f"markers are out of order: {previous.name}={previous.epoch} "
                f"then {current.name}={current.epoch}"
            )
    return semantic


def parse_tshark_tsv(path: Path, bus: int, device: int,
                     endpoint: int) -> tuple[list[Completion], dict[str, int]]:
    validate_capture_selection(bus, device, endpoint)
    counts = {
        "source_rows": 0,
        "kept_completions": 0,
        "deduplicated_completions": 0,
        "rejected_rows": 0,
        "filtered_selection": 0,
        "non_complete": 0,
        "no_payload": 0,
    }
    kept_by_urb: dict[tuple[int, Decimal], Completion] = {}
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle, delimiter="\t")
        if not reader.fieldnames:
            raise CaptureError(f"{path}: missing TSV header")
        missing = [field for field in TSHARK_FIELDS if field not in reader.fieldnames]
        if missing:
            raise CaptureError(f"{path}: missing tshark fields: {', '.join(missing)}")
        for row_number, row in enumerate(reader, 2):
            counts["source_rows"] += 1
            try:
                timestamp = parse_decimal(row["frame.time_epoch"], "frame.time_epoch")
                row_bus = parse_int(row["usb.bus_id"], "usb.bus_id")
                row_device = parse_int(row["usb.device_address"], "usb.device_address")
                row_endpoint = parse_int(row["usb.endpoint_address"], "usb.endpoint_address")
                urb_id = parse_int(row["usb.urb_id"], "usb.urb_id")
                data_len = parse_int(row["usb.data_len"], "usb.data_len")
                if data_len < 0:
                    raise CaptureError("usb.data_len must be non-negative")
            except CaptureError as exc:
                raise CaptureError(f"{path}:{row_number}: {exc}") from exc

            if (row_bus, row_device, row_endpoint) != (bus, device, endpoint):
                counts["filtered_selection"] += 1
                counts["rejected_rows"] += 1
                continue
            try:
                urb_type = classify_urb_type(row["usb.urb_type"])
            except CaptureError as exc:
                raise CaptureError(f"{path}:{row_number}: {exc}") from exc
            if urb_type != "complete":
                counts["non_complete"] += 1
                counts["rejected_rows"] += 1
                continue
            try:
                payload = normalize_capdata(row["usb.capdata"])
            except CaptureError as exc:
                raise CaptureError(f"{path}:{row_number}: {exc}") from exc
            if not payload or data_len == 0:
                if payload or data_len:
                    raise CaptureError(
                        f"{path}:{row_number}: usb.data_len/capdata disagree "
                        f"({data_len} versus {len(payload)})"
                    )
                counts["no_payload"] += 1
                counts["rejected_rows"] += 1
                continue
            if len(payload) != data_len:
                raise CaptureError(
                    f"{path}:{row_number}: usb.data_len {data_len} != capdata length {len(payload)}"
                )
            completion = Completion(
                timestamp, str(row["frame.time_epoch"]).strip(), row_endpoint, urb_id, payload
            )
            urb_key = (urb_id, timestamp)
            previous = kept_by_urb.get(urb_key)
            if previous is not None:
                if previous != completion:
                    raise CaptureError(
                        f"{path}:{row_number}: duplicate URB id 0x{urb_id:X} has conflicting data"
                    )
                counts["deduplicated_completions"] += 1
                continue
            kept_by_urb[urb_key] = completion

    completions = sorted(kept_by_urb.values(), key=lambda item: item.timestamp)
    if not completions:
        raise CaptureError("no payload-bearing selected URB_COMPLETE reports")
    for previous, current in zip(completions, completions[1:]):
        if current.timestamp < previous.timestamp:
            raise CaptureError("kept completion timestamps must be monotonic")
    counts["kept_completions"] = len(completions)
    return completions, counts


def merge_markers(completions: list[Completion], markers: list[Marker],
                  max_marker_lag_ms: float) -> tuple[dict[int, str], dict[str, float]]:
    if not math.isfinite(max_marker_lag_ms) or max_marker_lag_ms < 0:
        raise CaptureError("max marker lag must be finite and non-negative")
    timestamps = [item.timestamp for item in completions]
    assignments: dict[int, str] = {}
    lags: dict[str, float] = {}
    limit = Decimal(str(max_marker_lag_ms)) / Decimal(1000)
    for marker in markers:
        index = bisect_left(timestamps, marker.epoch)
        if index >= len(completions):
            raise CaptureError(f"marker {marker.name} has no following completion")
        if index in assignments:
            raise CaptureError(
                f"markers {assignments[index]} and {marker.name} collide on one report"
            )
        lag = completions[index].timestamp - marker.epoch
        if lag > limit:
            raise CaptureError(
                f"marker {marker.name} lag {_decimal_milliseconds(lag):.6f} ms "
                f"exceeds {max_marker_lag_ms:.6f} ms"
            )
        assignments[index] = marker.name
        lags[marker.name] = _decimal_milliseconds(lag)
    return assignments, lags


def _decimal_milliseconds(value: Decimal) -> float:
    return round(float(value * Decimal(1000)), 6)


def _percentile_decimal(values: list[Decimal], q: float) -> Decimal:
    if not values:
        return Decimal(0)
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(q * len(ordered)) - 1))
    return ordered[index]


def inspect_tshark_tsv(input_tsv: Path) -> dict:
    """Enumerate USB IN report candidates without selecting or interpreting axes."""
    counts = {
        "source_rows": 0,
        "candidate_reports": 0,
        "deduplicated_completions": 0,
        "submit_rows": 0,
        "error_rows": 0,
        "no_payload_completions": 0,
        "out_payload_completions": 0,
        "malformed_rows": 0,
        "conflicting_urb_rows": 0,
    }
    malformed: list[dict[str, object]] = []
    conflicts: list[dict[str, object]] = []
    seen_urbs: dict[tuple[int, int, int, Decimal], Completion] = {}
    groups: dict[tuple[int, int, int, int], list[Completion]] = {}
    with input_tsv.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle, delimiter="\t")
        if not reader.fieldnames:
            raise CaptureError(f"{input_tsv}: missing TSV header")
        missing = [field for field in TSHARK_FIELDS if field not in reader.fieldnames]
        if missing:
            raise CaptureError(
                f"{input_tsv}: missing tshark fields: {', '.join(missing)}"
            )
        for row_number, row in enumerate(reader, 2):
            counts["source_rows"] += 1
            try:
                timestamp = parse_decimal(row["frame.time_epoch"], "frame.time_epoch")
                bus = parse_int(row["usb.bus_id"], "usb.bus_id")
                device = parse_int(row["usb.device_address"], "usb.device_address")
                endpoint = parse_int(row["usb.endpoint_address"], "usb.endpoint_address")
                urb_id = parse_int(row["usb.urb_id"], "usb.urb_id")
                data_len = parse_int(row["usb.data_len"], "usb.data_len")
                if data_len < 0:
                    raise CaptureError("usb.data_len must be non-negative")
                urb_type = classify_urb_type(row["usb.urb_type"])
            except CaptureError as exc:
                counts["malformed_rows"] += 1
                malformed.append({"row": row_number, "reason": str(exc)})
                continue
            if urb_type == "submit":
                counts["submit_rows"] += 1
                continue
            if urb_type == "error":
                counts["error_rows"] += 1
                continue
            try:
                payload = normalize_capdata(row["usb.capdata"])
            except CaptureError as exc:
                counts["malformed_rows"] += 1
                malformed.append({"row": row_number, "reason": str(exc)})
                continue
            if not payload or data_len == 0:
                if payload or data_len:
                    counts["malformed_rows"] += 1
                    malformed.append({
                        "row": row_number,
                        "reason": f"usb.data_len/capdata disagree ({data_len} versus {len(payload)})",
                    })
                else:
                    counts["no_payload_completions"] += 1
                continue
            if len(payload) != data_len:
                counts["malformed_rows"] += 1
                malformed.append({
                    "row": row_number,
                    "reason": f"usb.data_len {data_len} != capdata length {len(payload)}",
                })
                continue
            completion = Completion(
                timestamp, str(row["frame.time_epoch"]).strip(), endpoint, urb_id, payload
            )
            # Windows can reuse the same IRP address after a completion.  Only
            # rows with the same IRP ID *and timestamp* are duplicates.
            urb_key = (bus, device, urb_id, timestamp)
            previous = seen_urbs.get(urb_key)
            if previous is not None:
                if previous == completion:
                    counts["deduplicated_completions"] += 1
                else:
                    counts["conflicting_urb_rows"] += 1
                    conflicts.append({
                        "row": row_number,
                        "bus": bus,
                        "device": device,
                        "urb_id": f"0x{urb_id:X}",
                        "previous_epoch": format(previous.timestamp, "f"),
                        "current_epoch": format(timestamp, "f"),
                        "reason": "same URB id has conflicting completion evidence",
                    })
                continue
            seen_urbs[urb_key] = completion
            if not endpoint & 0x80:
                counts["out_payload_completions"] += 1
                continue
            groups.setdefault((bus, device, endpoint, data_len), []).append(completion)
            counts["candidate_reports"] += 1

    candidates: list[dict] = []
    for (bus, device, endpoint, report_length), reports in groups.items():
        reports.sort(key=lambda item: (item.timestamp, item.urb_id))
        gaps = [current.timestamp - previous.timestamp
                for previous, current in zip(reports, reports[1:])
                if current.timestamp > previous.timestamp]
        columns = [[report.payload[offset] for report in reports]
                   for offset in range(report_length)]
        variation = [
            {
                "offset": offset,
                "minimum": min(values),
                "maximum": max(values),
                "range": max(values) - min(values),
                "unique_values": len(set(values)),
                "varies": len(set(values)) > 1,
            }
            for offset, values in enumerate(columns)
        ]
        candidates.append({
            "bus": bus,
            "device": device,
            "endpoint": endpoint,
            "report_length": report_length,
            "report_count": len(reports),
            "first_epoch_s": format(reports[0].timestamp, "f"),
            "last_epoch_s": format(reports[-1].timestamp, "f"),
            "cadence_ms": {
                "samples": len(gaps),
                "p50": _decimal_milliseconds(_percentile_decimal(gaps, .50)) if gaps else None,
                "p95": _decimal_milliseconds(_percentile_decimal(gaps, .95)) if gaps else None,
                "maximum": _decimal_milliseconds(max(gaps)) if gaps else None,
            },
            "unique_payload_count": len({report.payload for report in reports}),
            "variable_byte_count": sum(item["varies"] for item in variation),
            "byte_variation": variation,
        })
    candidates.sort(key=lambda item: (
        -item["report_count"], -item["variable_byte_count"], item["bus"],
        item["device"], item["endpoint"], item["report_length"],
    ))
    for rank, candidate in enumerate(candidates, 1):
        candidate["rank"] = rank
    return {
        "schema": "makcm.usb-report-discovery.v1",
        "selection": "NONE_HUMAN_REVIEW_REQUIRED",
        "input": {
            "tshark_tsv_path": str(input_tsv),
            "tshark_tsv_sha256": sha256_file(input_tsv),
        },
        "counts": counts,
        "candidates": candidates,
        "malformed_evidence": malformed,
        "conflicting_urb_evidence": conflicts,
    }


def render_inspection_summary(document: dict) -> str:
    lines = [
        "USB report candidates (read-only; no endpoint or axis was selected):"
    ]
    for item in document["candidates"]:
        cadence = item["cadence_ms"]
        cadence_text = "n/a" if cadence["p50"] is None else (
            f"p50={cadence['p50']:.3f}/p95={cadence['p95']:.3f}/max={cadence['maximum']:.3f} ms"
        )
        lines.append(
            f"  {item['rank']}. bus {item['bus']} device {item['device']} "
            f"EP 0x{item['endpoint']:02X} len {item['report_length']}: "
            f"reports={item['report_count']} unique={item['unique_payload_count']} "
            f"variable_bytes={item['variable_byte_count']} cadence {cadence_text}"
        )
    if not document["candidates"]:
        lines.append("  no payload-bearing IN completion candidates")
    counts = document["counts"]
    lines.append(
        f"Excluded: submits={counts['submit_rows']} errors={counts['error_rows']} "
        f"no_payload={counts['no_payload_completions']} OUT={counts['out_payload_completions']} "
        f"malformed={counts['malformed_rows']} conflicts={counts['conflicting_urb_rows']}"
    )
    return "\n".join(lines) + "\n"


def write_inspection_json(document: dict, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _strict_snapshot_hex(value: object, field: str) -> bytes:
    if not isinstance(value, str) or not value or len(value) % 2:
        raise CaptureError(f"snapshot {field} must be non-empty even-length hexadecimal")
    if any(character not in "0123456789abcdefABCDEF" for character in value):
        raise CaptureError(f"snapshot {field} contains malformed hexadecimal")
    return bytes.fromhex(value)


def _walk_usb_descriptors(data: bytes) -> list[bytes]:
    descriptors: list[bytes] = []
    offset = 0
    while offset < len(data):
        if offset + 2 > len(data):
            raise CaptureError(f"truncated descriptor header at configuration offset {offset}")
        length = data[offset]
        if length < 2 or offset + length > len(data):
            raise CaptureError(f"invalid descriptor length at configuration offset {offset}")
        descriptors.append(data[offset:offset + length])
        offset += length
    return descriptors


def _configuration_topology(configuration: bytes) -> dict:
    if len(configuration) < 9 or configuration[0] != 9 or configuration[1] != 2:
        raise CaptureError("configuration must begin with an exact 9-byte descriptor")
    declared_total = int.from_bytes(configuration[2:4], "little")
    if declared_total != len(configuration):
        raise CaptureError(
            f"configuration declared total {declared_total} != raw length {len(configuration)}"
        )
    interfaces: list[dict] = []
    endpoints: list[dict] = []
    current: dict | None = None
    for descriptor in _walk_usb_descriptors(configuration):
        dtype = descriptor[1]
        if dtype == 4:
            if len(descriptor) < 9:
                raise CaptureError("short interface descriptor")
            current = {
                "number": descriptor[2],
                "alternate": descriptor[3],
                "class": descriptor[5],
                "subclass": descriptor[6],
                "protocol": descriptor[7],
                "endpoints": [],
            }
            interfaces.append(current)
        elif dtype == 5:
            if len(descriptor) < 7:
                raise CaptureError("short endpoint descriptor")
            endpoint = {
                "address": descriptor[2],
                "attributes": descriptor[3],
                "packet_size": descriptor[4] | (descriptor[5] << 8),
                "interval": descriptor[6],
                "interface": current["number"] if current else None,
            }
            endpoints.append(endpoint)
            if current is not None:
                current["endpoints"].append(endpoint["address"])
    if configuration[4] != len({item["number"] for item in interfaces}):
        raise CaptureError(
            f"configuration declares {configuration[4]} interfaces but topology has "
            f"{len({item['number'] for item in interfaces})}"
        )
    return {
        "declared_total": declared_total,
        "raw_length": len(configuration),
        "declared_interfaces": configuration[4],
        "interfaces": interfaces,
        "endpoints": endpoints,
    }


def _hid_report_lengths(configuration: bytes) -> dict[int, int]:
    result: dict[int, int] = {}
    current_interface: int | None = None
    current_class: int | None = None
    for descriptor in _walk_usb_descriptors(configuration):
        if descriptor[1] == 4 and len(descriptor) >= 9:
            current_interface = descriptor[2]
            current_class = descriptor[5]
        elif descriptor[1] == 0x21 and current_class == 0x03:
            if len(descriptor) < 9:
                raise CaptureError("short HID descriptor")
            if descriptor[6] != 0x22:
                continue
            length = descriptor[7] | descriptor[8] << 8
            if current_interface is None:
                raise CaptureError("HID descriptor has no owning interface")
            if current_interface in result and result[current_interface] != length:
                raise CaptureError(
                    f"conflicting HID report lengths for interface {current_interface}"
                )
            result[current_interface] = length
    return result


def unpack_descriptor_snapshot(snapshot_path: Path, out_dir: Path,
                               target_interface: int) -> dict:
    """Validate and atomically unpack usb_descriptor_snapshot.py evidence."""
    try:
        snapshot = json.loads(snapshot_path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise CaptureError(f"cannot read descriptor snapshot {snapshot_path}: {exc}") from exc
    if not isinstance(snapshot, dict):
        raise CaptureError("descriptor snapshot root must be an object")
    device = _strict_snapshot_hex(snapshot.get("device"), "device")
    configuration = _strict_snapshot_hex(snapshot.get("configuration"), "configuration")
    if len(device) != 18 or device[0] != 18 or device[1] != 1:
        raise CaptureError("snapshot device must be the exact 18-byte USB Device Descriptor")
    topology = _configuration_topology(configuration)
    declared_topology = snapshot.get("topology")
    if not isinstance(declared_topology, dict) or declared_topology != topology:
        raise CaptureError("snapshot declared topology does not match configuration bytes")
    target_entries = [item for item in topology["interfaces"]
                      if item["number"] == target_interface]
    if not target_entries:
        raise CaptureError(f"target interface {target_interface} does not exist")
    target_classes = {item["class"] for item in target_entries}
    if len(target_classes) != 1:
        raise CaptureError(
            f"target interface {target_interface} has ambiguous classes across alternates"
        )
    target_class = next(iter(target_classes))
    try:
        declared_vid = int(str(snapshot["vid"]), 16)
        declared_pid = int(str(snapshot["pid"]), 16)
    except (KeyError, TypeError, ValueError) as exc:
        raise CaptureError("snapshot requires hexadecimal vid and pid declarations") from exc
    captured_vid = int.from_bytes(device[8:10], "little")
    captured_pid = int.from_bytes(device[10:12], "little")
    if (declared_vid, declared_pid) != (captured_vid, captured_pid):
        raise CaptureError("snapshot VID/PID declarations do not match device bytes")

    raw_reports = snapshot.get("hid_reports")
    if not isinstance(raw_reports, dict):
        raise CaptureError("snapshot hid_reports must be an object")
    declared_hid_lengths = _hid_report_lengths(configuration)
    reports: dict[int, bytes] = {}
    for raw_interface, raw_hex in raw_reports.items():
        try:
            interface = int(str(raw_interface), 10)
        except ValueError as exc:
            raise CaptureError(f"invalid HID interface key {raw_interface!r}") from exc
        if interface in reports:
            raise CaptureError(f"duplicate HID interface {interface}")
        report = _strict_snapshot_hex(raw_hex, f"hid_reports[{interface}]")
        if interface not in declared_hid_lengths:
            raise CaptureError(
                f"HID report interface {interface} is not declared as HID in configuration"
            )
        if len(report) != declared_hid_lengths[interface]:
            raise CaptureError(
                f"HID report interface {interface} length {len(report)} != "
                f"declared {declared_hid_lengths[interface]}"
            )
        reports[interface] = report
    selected_interface: int | None = None
    selected_report: bytes | None = None
    if target_class == 0x03:
        errors = snapshot.get("errors", [])
        if not isinstance(errors, list) or any(not isinstance(item, str) for item in errors):
            raise CaptureError("snapshot errors must be a list of strings")
        target_error_prefixes = (
            f"hid_report:{target_interface}:",
            f"hid_report_length:{target_interface}:",
        )
        matching_errors = [item for item in errors if item.startswith(target_error_prefixes)]
        if matching_errors:
            raise CaptureError(
                f"target HID interface {target_interface} retrieval failed: {matching_errors[0]}"
            )
        if target_interface not in declared_hid_lengths:
            raise CaptureError(
                f"target HID interface {target_interface} has no declared report length"
            )
        if target_interface not in reports:
            raise CaptureError(
                f"target HID interface {target_interface} report bytes are missing"
            )
        selected_interface = target_interface
        selected_report = reports[target_interface]

    file_manifest: dict[str, dict | None] = {
        "device": {
            "file": "device.bin", "length": len(device),
            "sha256": hashlib.sha256(device).hexdigest(),
        },
        "configuration": {
            "file": "configuration.bin", "length": len(configuration),
            "sha256": hashlib.sha256(configuration).hexdigest(),
        },
        "hid_report": None,
    }
    if selected_report is not None:
        file_manifest["hid_report"] = {
            "file": f"hid_report_if{selected_interface}.bin",
            "interface": selected_interface,
            "length": len(selected_report),
            "sha256": hashlib.sha256(selected_report).hexdigest(),
        }
    manifest = {
        "schema": "makcm.usb-descriptor-files.v1",
        "snapshot_sha256": sha256_file(snapshot_path),
        "vid": captured_vid,
        "pid": captured_pid,
        "topology": topology,
        "target_interface": target_interface,
        "target_interface_class": target_class,
        "hid_report_present": selected_report is not None,
        "hid_interface": selected_interface,
        "declared_hid_interfaces": sorted(declared_hid_lengths),
        "available_hid_interfaces": sorted(reports),
        "non_target_hid_interfaces": sorted(
            interface for interface in declared_hid_lengths if interface != target_interface
        ),
        "non_target_hid_reports": [
            {"interface": interface, "present": True, "length": len(report)}
            for interface, report in sorted(reports.items())
            if interface != target_interface
        ],
        "files": file_manifest,
    }

    # All semantic validation above precedes filesystem mutation.  A staged
    # sibling directory is renamed only after every artifact is complete.
    if out_dir.exists() and any(out_dir.iterdir()):
        raise CaptureError(f"descriptor output directory must be absent or empty: {out_dir}")
    out_dir.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{out_dir.name}-", dir=out_dir.parent))
    try:
        (stage / "device.bin").write_bytes(device)
        (stage / "configuration.bin").write_bytes(configuration)
        if selected_report is not None:
            (stage / f"hid_report_if{selected_interface}.bin").write_bytes(selected_report)
        (stage / "manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        if out_dir.exists():
            out_dir.rmdir()
        os.replace(stage, out_dir)
    except Exception:
        if stage.exists():
            shutil.rmtree(stage)
        raise
    return manifest


def render_guided_csv(completions: list[Completion], assignments: dict[int, str]) -> bytes:
    output = io.StringIO(newline="")
    writer = csv.DictWriter(
        output,
        fieldnames=("timestamp_s", "endpoint", "report_hex", "marker"),
        lineterminator="\n",
    )
    writer.writeheader()
    origin = completions[0].timestamp
    for index, item in enumerate(completions):
        writer.writerow({
            "timestamp_s": format(item.timestamp - origin, "f"),
            "endpoint": f"0x{item.endpoint:02X}",
            "report_hex": item.payload.hex(),
            "marker": assignments.get(index, ""),
        })
    return output.getvalue().encode("utf-8")


def convert_tshark_tsv(input_tsv: Path, markers_json: Path, *, bus: int,
                       device: int, endpoint: int, out_csv: Path,
                       audit_json: Path, max_marker_lag_ms: float = 100.0,
                       tshark_command: Iterable[str] | None = None,
                       tshark_version: str | None = None,
                       raw_capture: Path | None = None,
                       capture_provenance: dict | None = None) -> dict:
    completions, counts = parse_tshark_tsv(input_tsv, bus, device, endpoint)
    markers = load_markers(markers_json)
    assignments, lags = merge_markers(completions, markers, max_marker_lag_ms)
    csv_bytes = render_guided_csv(completions, assignments)
    audit = {
        "schema": "makcm.guided-usb-capture-audit.v1",
        "input": {
            "tshark_tsv_path": str(input_tsv),
            "tshark_tsv_sha256": sha256_file(input_tsv),
            "marker_json_path": str(markers_json),
            "marker_json_sha256": sha256_file(markers_json),
            "raw_capture_path": str(raw_capture) if raw_capture else None,
            "raw_capture_sha256": sha256_file(raw_capture) if raw_capture else None,
        },
        "selection": {"bus": bus, "device": device, "endpoint": endpoint},
        "counts": counts,
        "marker_lag_ms": lags,
        "max_marker_lag_ms": max_marker_lag_ms,
        "capture_epoch_origin_s": format(completions[0].timestamp, "f"),
        "tshark": {
            "command": list(tshark_command) if tshark_command else None,
            "version": tshark_version,
        },
        "output_csv_sha256": hashlib.sha256(csv_bytes).hexdigest(),
    }
    if capture_provenance is not None:
        audit["capture"] = capture_provenance
    audit_bytes = (json.dumps(audit, indent=2, sort_keys=True) + "\n").encode("utf-8")
    out_csv.parent.mkdir(parents=True, exist_ok=True)
    audit_json.parent.mkdir(parents=True, exist_ok=True)
    out_csv.write_bytes(csv_bytes)
    audit_json.write_bytes(audit_bytes)
    return audit


def find_wireshark_executable(name: str, explicit: Path | None = None) -> Path:
    if explicit is not None:
        if explicit.is_file():
            return explicit
        raise CaptureError(f"configured {name} executable does not exist: {explicit}")
    found = shutil.which(name)
    if found:
        return Path(found)
    candidates = (
        Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Wireshark" / f"{name}.exe",
        Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
        / "Wireshark" / f"{name}.exe",
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise CaptureError(
        f"{name} was not found; install Wireshark with USBPcap externally or pass --{name}"
    )


def find_usbpcapcmd_executable(explicit: Path | None = None) -> Path:
    """Find the USBPcap user-space capture executable.

    USBPcapCMD is the capture engine.  It is intentionally resolved separately
    from Wireshark's dumpcap: dumpcap/libpcap/Npcap cannot enumerate USBPcap
    root hubs on their own.
    """
    if explicit is not None:
        if explicit.is_file():
            return explicit
        raise CaptureError(f"configured USBPcapCMD executable does not exist: {explicit}")
    for name in ("USBPcapCMD", "USBPcapCMD.exe"):
        found = shutil.which(name)
        if found:
            return Path(found)
    roots = (
        Path(os.environ.get("ProgramFiles", r"C:\Program Files")),
        Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")),
    )
    for root in roots:
        candidate = root / "USBPcap" / "USBPcapCMD.exe"
        if candidate.is_file():
            return candidate
    raise CaptureError(
        "USBPcapCMD.exe was not found; install USBPcap externally or pass --usbpcapcmd"
    )


USBPCAP_INTERFACE_RE = re.compile(r"^\\\\\.\\USBPcap[0-9]+$", re.IGNORECASE)


def validate_usbpcap_interface(interface: str, *, context: str = "capture") -> str:
    """Validate an explicitly selected USBPcap root-hub control device."""
    value = interface.strip()
    if not value:
        raise CaptureError(f"{context} requires an explicitly selected USBPcap root-hub interface")
    if not USBPCAP_INTERFACE_RE.fullmatch(value):
        raise CaptureError(
            f"{context} interface must match \\\\.\\USBPcapN; automatic interface selection is disabled"
        )
    return value


def parse_usbpcap_interfaces(output: str) -> list[str]:
    """Extract canonical USBPcap root-hub values from USBPcapCMD output.

    USBPcapCMD's extcap output is of the form
    ``interface {value=\\.\\USBPcap1}{display=USBPcap1}``.  A small fallback
    parser accepts legacy dumpcap-style lines so old evidence can still be
    inspected, but no dumpcap invocation is made by the live path.
    """
    interfaces: set[str] = set()
    for raw_line in output.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        match = re.search(r"interface\s+\{value=([^}]*)\}", line, re.IGNORECASE)
        candidate = match.group(1).strip() if match else None
        if candidate is None:
            legacy = re.search(r"(\\\\\.\\USBPcap[0-9]+)", line, re.IGNORECASE)
            candidate = legacy.group(1) if legacy else None
        if candidate is not None and USBPCAP_INTERFACE_RE.fullmatch(candidate):
            interfaces.add(candidate)
    return sorted(interfaces, key=lambda value: (value.lower(), value))


def parse_usbpcap_version(output: str) -> tuple[int, int, int, int]:
    match = re.search(r"extcap\s*\{version=([0-9]+(?:\.[0-9]+){1,3})\}", output)
    if not match:
        raise CaptureError("USBPcapCMD --extcap-version returned an invalid version line")
    parts = [int(part) for part in match.group(1).split(".")]
    parts.extend([0] * (4 - len(parts)))
    return tuple(parts)  # type: ignore[return-value]


def require_usbpcap_version(output: str) -> tuple[int, int, int, int]:
    version = parse_usbpcap_version(output)
    if version < USBPCAP_MIN_VERSION:
        required = ".".join(str(part) for part in USBPCAP_MIN_VERSION)
        actual = ".".join(str(part) for part in version)
        raise CaptureError(f"USBPcapCMD {actual} is unsupported; require >= {required}")
    return version


def parse_usbpcap_devices(output: str) -> dict[int, str]:
    """Parse enabled physical-device values from USBPcap extcap config."""
    devices: dict[int, str] = {}
    for line in output.splitlines():
        if not line.startswith("value "):
            continue
        value_match = re.search(r"\{value=([0-9]+)\}", line)
        enabled_match = re.search(r"\{enabled=(true|false)\}", line, re.IGNORECASE)
        if value_match is None or (
            enabled_match is not None and enabled_match.group(1).lower() != "true"
        ):
            continue
        address = int(value_match.group(1))
        if not 1 <= address <= 127:
            continue
        display_match = re.search(r"\{display=([^}]*)\}", line)
        devices[address] = display_match.group(1) if display_match else ""
    return devices


def _first_version_line(result: subprocess.CompletedProcess) -> str:
    text = (result.stdout or result.stderr or "").strip()
    return text.splitlines()[0] if text else "version command returned no text"


def usbpcapcmd_version(usbpcapcmd: Path) -> str:
    """Return USBPcapCMD's extcap version line after a checked invocation."""
    result = subprocess.run(
        [str(usbpcapcmd), "--extcap-version"],
        check=True,
        capture_output=True,
        text=True,
    )
    line = _first_version_line(result)
    require_usbpcap_version(line)
    return line


def verify_capture_target(
    usbpcapcmd: Path,
    interface: str,
    *,
    device: int | None,
    runner: Callable[..., subprocess.CompletedProcess] = subprocess.run,
) -> dict:
    """Re-enumerate version/interface/device immediately before capture."""
    interface = validate_usbpcap_interface(interface)
    version_result = runner(
        [str(usbpcapcmd), "--extcap-version"],
        check=True,
        capture_output=True,
        text=True,
    )
    version_line = _first_version_line(version_result)
    version = require_usbpcap_version(version_line)
    interface_result = runner(
        [str(usbpcapcmd), "--extcap-interfaces"],
        check=True,
        capture_output=True,
        text=True,
    )
    interfaces = parse_usbpcap_interfaces(interface_result.stdout or "")
    if interface not in interfaces:
        raise CaptureError(
            f"selected interface {interface} is not present in the current USBPcap enumeration"
        )
    config_result = runner(
        [
            str(usbpcapcmd),
            "--extcap-config",
            "--extcap-interface",
            interface,
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    devices = parse_usbpcap_devices(config_result.stdout or "")
    if device is not None:
        if not 1 <= device <= 127:
            raise CaptureError("USB device address must be in the range 1..127")
        if device not in devices:
            raise CaptureError(
                f"USB device address {device} is not attached to {interface}; "
                "re-run discovery after reconnect/re-enumeration"
            )
    return {
        "version_line": version_line,
        "version": list(version),
        "interfaces": interfaces,
        "devices": {str(key): devices[key] for key in sorted(devices)},
    }


def preflight_exit_code(document: dict) -> int:
    capture_ready = document["required_for_capture"]["status"] == "PASS"
    descriptor_ready = document["required_for_descriptor"]["status"] == "PASS"
    if capture_ready and descriptor_ready:
        return PREFLIGHT_READY
    if not capture_ready and not descriptor_ready:
        return PREFLIGHT_BOTH_MISSING
    if not capture_ready:
        return PREFLIGHT_CAPTURE_MISSING
    return PREFLIGHT_DESCRIPTOR_MISSING


def run_preflight(
    args: argparse.Namespace,
    *,
    finder: Callable[[str, Path | None], Path] | None = None,
    runner: Callable[..., subprocess.CompletedProcess] | None = None,
    importer: Callable[[str], object] | None = None,
) -> dict:
    """Audit Windows capture prerequisites without installation or USB selection."""
    finder = finder or (
        lambda name, explicit: (
            find_usbpcapcmd_executable(explicit)
            if name == "usbpcapcmd"
            else find_wireshark_executable(name, explicit)
        )
    )
    runner = runner or subprocess.run
    importer = importer or importlib.import_module
    capture_checks: list[dict] = []
    tool_paths: dict[str, Path] = {}

    # USBPcapCMD is the capture engine.  Its extcap interface list is the
    # authoritative read-only root-hub enumeration; dumpcap/Npcap is not a
    # prerequisite for USBPcap live capture.
    for name, version_args in (
        ("usbpcapcmd", ("--extcap-version",)),
        ("tshark", ("--version",)),
    ):
        explicit = getattr(args, name, None)
        try:
            path = finder(name, explicit)
            result = runner(
                [str(path), *version_args],
                check=True,
                capture_output=True,
                text=True,
            )
            version_line = _first_version_line(result)
            if name == "usbpcapcmd":
                require_usbpcap_version(version_line)
            tool_paths[name] = path
            capture_checks.append({
                "id": name,
                "status": "PASS",
                "required": True,
                "path": str(path),
                "version": version_line,
                "error": None,
            })
        except (CaptureError, OSError, subprocess.SubprocessError) as exc:
            capture_checks.append({
                "id": name,
                "status": "FAIL",
                "required": True,
                "path": str(explicit) if explicit else None,
                "version": None,
                "error": str(exc),
            })

    usbpcapcmd = tool_paths.get("usbpcapcmd")
    if usbpcapcmd is None:
        capture_checks.append({
            "id": "usbpcap_interface",
            "status": "FAIL",
            "required": True,
            "interfaces": [],
            "error": "USBPcapCMD is unavailable, so root-hub interfaces cannot be listed",
        })
    else:
        try:
            result = runner(
                [str(usbpcapcmd), "--extcap-interfaces"],
                check=True,
                capture_output=True,
                text=True,
            )
            interfaces = parse_usbpcap_interfaces(result.stdout or "")
            capture_checks.append({
                "id": "usbpcap_interface",
                "status": "PASS" if interfaces else "FAIL",
                "required": True,
                "interfaces": interfaces,
                "error": None if interfaces else (
                    "USBPcapCMD reported no USBPcap root-hub interface; "
                    "restart USB devices or reboot after driver installation"
                ),
            })
        except (CaptureError, OSError, subprocess.SubprocessError) as exc:
            capture_checks.append({
                "id": "usbpcap_interface",
                "status": "FAIL",
                "required": True,
                "interfaces": [],
                "error": str(exc),
            })

    descriptor_checks: list[dict] = []
    snapshot_script = args.snapshot_script
    descriptor_checks.append({
        "id": "descriptor_snapshot_script",
        "status": "PASS" if snapshot_script.is_file() else "FAIL",
        "required": True,
        "path": str(snapshot_script),
        "error": None if snapshot_script.is_file() else "descriptor snapshot script is missing",
    })
    imported: dict[str, object] = {}
    for check_id, module_name in (("pyusb", "usb.core"),
                                  ("libusb_package", "libusb_package")):
        try:
            imported[module_name] = importer(module_name)
            descriptor_checks.append({
                "id": check_id,
                "status": "PASS",
                "required": True,
                "module": module_name,
                "error": None,
            })
        except (ImportError, ModuleNotFoundError, OSError) as exc:
            descriptor_checks.append({
                "id": check_id,
                "status": "FAIL",
                "required": True,
                "module": module_name,
                "error": str(exc),
            })
    libusb_module = imported.get("libusb_package")
    if libusb_module is None:
        descriptor_checks.append({
            "id": "libusb_backend",
            "status": "FAIL",
            "required": True,
            "error": "libusb_package is unavailable",
        })
    else:
        try:
            backend = libusb_module.get_libusb1_backend()  # type: ignore[attr-defined]
            descriptor_checks.append({
                "id": "libusb_backend",
                "status": "PASS" if backend is not None else "FAIL",
                "required": True,
                "error": None if backend is not None else "libusb-1.0 backend was not found",
            })
        except Exception as exc:  # backend loader errors vary by packaged libusb
            descriptor_checks.append({
                "id": "libusb_backend",
                "status": "FAIL",
                "required": True,
                "error": str(exc),
            })

    capture_status = "PASS" if all(
        item["status"] == "PASS" for item in capture_checks
    ) else "FAIL"
    descriptor_status = "PASS" if all(
        item["status"] == "PASS" for item in descriptor_checks
    ) else "FAIL"
    if capture_status == descriptor_status == "PASS":
        status = "READY"
    elif capture_status == descriptor_status == "FAIL":
        status = "CAPTURE_AND_DESCRIPTOR_REQUIREMENTS_MISSING"
    elif capture_status == "FAIL":
        status = "CAPTURE_REQUIREMENTS_MISSING"
    else:
        status = "DESCRIPTOR_REQUIREMENTS_MISSING"
    document = {
        "schema": "makcm.windows-usb-capture-preflight.v1",
        "status": status,
        "required_for_capture": {"status": capture_status, "checks": capture_checks},
        "required_for_descriptor": {
            "status": descriptor_status,
            "checks": descriptor_checks,
        },
        "policy": {
            "read_only": True,
            "auto_install_performed": False,
            "device_selected": None,
            "endpoint_selected": None,
            "next_step_automatic": False,
        },
    }
    document["exit_code"] = preflight_exit_code(document)
    return document


def render_preflight_summary(document: dict) -> str:
    lines = [
        f"Windows USB capture preflight: {document['status']} "
        f"(exit {document['exit_code']})",
    ]
    for group_key, label in (
        ("required_for_capture", "Required for live capture"),
        ("required_for_descriptor", "Required for descriptor snapshot"),
    ):
        group = document[group_key]
        lines.append(f"{label}: {group['status']}")
        for check in group["checks"]:
            detail = check.get("version") or check.get("path")
            if check.get("interfaces"):
                detail = "; ".join(check["interfaces"])
            if check["status"] == "FAIL":
                detail = check.get("error") or "unavailable"
            lines.append(
                f"  [{check['status']}] {check['id']}"
                + (f": {detail}" if detail else "")
            )
    lines.append(
        "Read-only audit only: no installation, USB device selection, or endpoint selection was performed."
    )
    return "\n".join(lines) + "\n"


def build_usbpcapcmd_capture_command(
    usbpcapcmd: Path,
    interface: str,
    raw_capture: Path,
    duration_s: int,
    *,
    device: int | None = None,
    capture_all: bool = False,
) -> list[str]:
    """Build a direct USBPcapCMD capture command.

    USBPcapCMD 1.5.4.0 writes classic pcap (not pcapng) via ``-o`` and has no
    duration switch.  The caller enforces the duration by waiting and then
    terminating the process.  Guided capture supplies an explicit USB device
    address; discovery deliberately captures all devices on the selected root
    hub for human review.
    """
    validate_usbpcap_interface(interface)
    if not 10 <= duration_s <= 3600:
        raise CaptureError("capture duration must be between 10 and 3600 seconds")
    if device is not None and not 1 <= device <= 127:
        raise CaptureError("USB device address must be in the range 1..127")
    if device is None and not capture_all:
        raise CaptureError(
            "capture requires an explicit USB device or capture_all=True; automatic device selection is disabled"
        )
    command = [
        str(usbpcapcmd),
        "-d",
        interface,
        "-o",
        str(raw_capture),
    ]
    if device is not None:
        command.extend(("--devices", str(device)))
    if capture_all:
        command.append("-A")
    command.append("--inject-descriptors")
    return command


def build_tshark_export_command(tshark: Path, raw_capture: Path, *, bus: int,
                                device: int, endpoint: int) -> list[str]:
    """Legacy TSV export command; live capture does not rely on its payload."""
    validate_capture_selection(bus, device, endpoint)
    display_filter = (
        f"usb.irp_info.direction == 1 && usb.bus_id == {bus} "
        f"&& usb.device_address == {device} "
        f"&& usb.endpoint_address == 0x{endpoint:02x} "
        "&& usb.transfer_type == 1 && usb.data_len > 0"
    )
    command = [str(tshark), "-r", str(raw_capture), "-Y", display_filter, "-T", "fields",
               "-E", "header=y", "-E", "separator=/t", "-E", "quote=d",
               "-E", "occurrence=f"]
    for field in TSHARK_FIELDS:
        command.extend(("-e", field))
    return command


def build_discovery_usbpcapcmd_command(usbpcapcmd: Path, interface: str,
                                       raw_capture: Path, duration_s: int) -> list[str]:
    if not 5 <= duration_s <= 15:
        raise CaptureError("discovery duration must be between 5 and 15 seconds")
    return build_usbpcapcmd_capture_command(
        usbpcapcmd,
        interface,
        raw_capture,
        # The shared builder validates guided durations; discovery has its own
        # narrower 5-15 second range and is bounded by run_discover's wait.
        max(10, duration_s),
        capture_all=True,
    )


def build_discovery_tshark_command(tshark: Path, raw_capture: Path) -> list[str]:
    display_filter = (
        "usb.irp_info.direction == 1 && usb.transfer_type == 1 "
        "&& usb.endpoint_address >= 0x81 && usb.endpoint_address <= 0x8f "
        "&& usb.data_len > 0"
    )
    command = [str(tshark), "-r", str(raw_capture), "-Y", display_filter,
               "-T", "fields", "-E", "header=y", "-E", "separator=/t",
               "-E", "quote=d", "-E", "occurrence=f"]
    for field in TSHARK_FIELDS:
        command.extend(("-e", field))
    return command


def build_tshark_readability_command(tshark: Path, raw_capture: Path) -> list[str]:
    return [str(tshark), "-r", str(raw_capture), "-c", "1"]


def tshark_version(tshark: Path) -> str:
    result = subprocess.run(
        [str(tshark), "--version"], check=True, capture_output=True, text=True
    )
    return result.stdout.splitlines()[0].strip() if result.stdout else "unknown"


def list_capture_interfaces(usbpcapcmd: Path) -> str:
    result = subprocess.run(
        [str(usbpcapcmd), "--extcap-interfaces"],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout


def write_live_markers(path: Path, entries: list[tuple[str, int]]) -> None:
    document = {
        "schema": "makcm.guided-marker-epochs.v1",
        "clock": "unix_epoch_ns",
        "markers": [{"marker": name, "epoch_ns": epoch_ns}
                    for name, epoch_ns in entries],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


def windows_process_snapshot() -> dict[int, tuple[int, str]]:
    """Return pid -> (parent pid, executable) without mutating process state."""
    if os.name != "nt":
        return {}
    import ctypes
    from ctypes import wintypes

    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [
            ("dwSize", wintypes.DWORD),
            ("cntUsage", wintypes.DWORD),
            ("th32ProcessID", wintypes.DWORD),
            ("th32DefaultHeapID", ctypes.c_size_t),
            ("th32ModuleID", wintypes.DWORD),
            ("cntThreads", wintypes.DWORD),
            ("th32ParentProcessID", wintypes.DWORD),
            ("pcPriClassBase", wintypes.LONG),
            ("dwFlags", wintypes.DWORD),
            ("szExeFile", wintypes.WCHAR * 260),
        ]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateToolhelp32Snapshot.argtypes = (wintypes.DWORD, wintypes.DWORD)
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.Process32FirstW.argtypes = (
        wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)
    )
    kernel32.Process32FirstW.restype = wintypes.BOOL
    kernel32.Process32NextW.argtypes = (
        wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)
    )
    kernel32.Process32NextW.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel32.CloseHandle.restype = wintypes.BOOL
    snapshot = kernel32.CreateToolhelp32Snapshot(0x00000002, 0)
    invalid_handle = ctypes.c_void_p(-1).value
    if snapshot == invalid_handle:
        raise CaptureError(
            f"CreateToolhelp32Snapshot failed with Windows error {ctypes.get_last_error()}"
        )
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    processes: dict[int, tuple[int, str]] = {}
    try:
        success = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
        while success:
            processes[int(entry.th32ProcessID)] = (
                int(entry.th32ParentProcessID),
                entry.szExeFile,
            )
            success = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snapshot)
    return processes


def reject_existing_usbpcapcmd(
    snapshot_fn: Callable[[], dict[int, tuple[int, str]]] = windows_process_snapshot,
) -> None:
    existing = sorted(
        pid for pid, (_, name) in snapshot_fn().items()
        if name.lower() == "usbpcapcmd.exe"
    )
    if existing:
        raise CaptureError(
            "refusing capture while another USBPcapCMD process exists: "
            + ", ".join(str(pid) for pid in existing)
        )


def _process_tree_pids(
    root_pid: int, snapshot: dict[int, tuple[int, str]]
) -> set[int]:
    result = {root_pid}
    changed = True
    while changed:
        changed = False
        for pid, (parent, _) in snapshot.items():
            if parent in result and pid not in result:
                result.add(pid)
                changed = True
    return result


def stop_windows_process_tree(
    process: subprocess.Popen,
    *,
    runner: Callable[..., subprocess.CompletedProcess] = subprocess.run,
    snapshot_fn: Callable[[], dict[int, tuple[int, str]]] = windows_process_snapshot,
) -> dict:
    """Force-stop only the process tree rooted at this capture's PID."""
    if process.poll() is not None:
        return {
            "controlled": False,
            "method": None,
            "tree_pids": [process.pid],
            "exit_code": process.returncode,
        }
    before = snapshot_fn()
    tree_pids = _process_tree_pids(process.pid, before)
    if os.name == "nt":
        result = runner(
            ["taskkill", "/PID", str(process.pid), "/T", "/F"],
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0 and process.poll() is None:
            raise CaptureError(
                f"taskkill failed for capture PID {process.pid}: "
                f"{(result.stderr or result.stdout or '').strip()}"
            )
    else:
        # Non-Windows is used only by deterministic unit tests.
        process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired as exc:
        raise CaptureError(f"capture process tree PID {process.pid} did not stop") from exc
    after = snapshot_fn()
    survivors = sorted(pid for pid in tree_pids if pid in after)
    if survivors:
        raise CaptureError(
            "capture process tree still has surviving PIDs: "
            + ", ".join(str(pid) for pid in survivors)
        )
    return {
        "controlled": True,
        "method": "taskkill /PID /T /F" if os.name == "nt" else "terminate(test)",
        "tree_pids": sorted(tree_pids),
        "exit_code": process.returncode,
    }


def _validate_controlled_exit(process: subprocess.Popen, stop: dict) -> None:
    if stop.get("controlled"):
        allowed = (0, 1) if os.name == "nt" else (0, 1, -15)
        if process.returncode not in allowed:
            raise CaptureError(
                f"controlled USBPcapCMD stop returned unexpected status {process.returncode}"
            )
        return
    if process.returncode != 0:
        raise CaptureError(f"USBPcapCMD failed naturally with status {process.returncode}")


def _prepare_raw_capture(path: Path) -> None:
    if path.suffix.lower() != ".pcap":
        raise CaptureError("USBPcapCMD raw output must use the .pcap extension")
    if path.exists():
        raise CaptureError(
            f"raw capture already exists; USBPcap CREATE_NEW requires a fresh path: {path}"
        )
    path.parent.mkdir(parents=True, exist_ok=True)


def _validate_created_capture(path: Path, start_epoch_ns: int) -> USBPcapCapture:
    if not path.is_file():
        raise CaptureError(f"USBPcapCMD did not produce raw capture: {path}")
    stat = path.stat()
    if stat.st_size <= 24:
        raise CaptureError(f"USBPcapCMD raw capture is empty: {path}")
    if stat.st_ctime_ns < start_epoch_ns:
        raise CaptureError(f"raw capture predates this capture start: {path}")
    return parse_classic_usbpcap(path)


def _ready_usbpcap_prefix(path: Path) -> USBPcapCapture | None:
    """Return a strictly parsed first record, or None while it is incomplete."""
    try:
        data = path.read_bytes()
    except FileNotFoundError:
        return None
    except PermissionError:
        # Windows USBPcapCMD may open the output without read sharing.  The
        # caller has a bounded, stat-only operational-readiness fallback.
        raise
    except OSError as exc:
        raise CaptureError(f"cannot read raw capture {path}: {exc}") from exc
    if len(data) < 24:
        return None
    snaplen = _validate_classic_pcap_global_header(data, path)
    if len(data) < 40:
        return None
    seconds, micros, captured_len, original_len = struct.unpack_from("<IIII", data, 24)
    if micros >= 1_000_000:
        raise CaptureError(f"{path}: record #1 has invalid microseconds")
    if captured_len == 0:
        raise CaptureError(f"{path}: record #1 is empty")
    if captured_len > snaplen:
        raise CaptureError(
            f"{path}: record #1 captured length {captured_len} exceeds snaplen"
        )
    if captured_len != original_len:
        raise CaptureError(
            f"{path}: record #1 is truncated "
            f"({captured_len} captured, {original_len} original)"
        )
    first_record_end = 40 + captured_len
    if len(data) < first_record_end:
        return None
    return _parse_classic_usbpcap_data(data[:first_record_end], path)


def _wait_for_capture_ready(
    path: Path,
    process: subprocess.Popen,
    *,
    timeout_s: float = USBPCAP_READINESS_TIMEOUT_S,
    poll_s: float = USBPCAP_READINESS_POLL_S,
    monotonic_fn: Callable[[], float] = time.monotonic,
    sleep_fn: Callable[[float], None] = time.sleep,
) -> CaptureReadiness:
    """Wait for strict first-record validation or locked-file growth."""
    if timeout_s <= 0:
        raise CaptureError("USBPcap readiness timeout must be positive")
    if poll_s <= 0:
        raise CaptureError("USBPcap readiness poll interval must be positive")
    deadline = monotonic_fn() + timeout_s
    locked_previous_size: int | None = None
    while True:
        returncode = process.poll()
        if returncode is not None:
            raise CaptureError(
                f"USBPcapCMD exited early before pcap readiness with status {returncode}"
            )
        try:
            ready = _ready_usbpcap_prefix(path)
        except PermissionError:
            try:
                current_size = path.stat().st_size
            except (FileNotFoundError, PermissionError):
                current_size = None
            except OSError as exc:
                raise CaptureError(
                    f"cannot stat locked raw capture {path}: {exc}"
                ) from exc
            if current_size is not None:
                grew = (
                    locked_previous_size is not None
                    and current_size > locked_previous_size
                )
                if current_size >= USBPCAP_MIN_OPERATIONAL_SIZE and grew:
                    returncode = process.poll()
                    if returncode is not None:
                        raise CaptureError(
                            "USBPcapCMD exited early before pcap readiness "
                            f"with status {returncode}"
                        )
                    return CaptureReadiness(
                        method="locked_file_size_growth",
                        validation_deferred=True,
                        observed_size_bytes=current_size,
                        first_record=None,
                    )
                locked_previous_size = current_size
            ready = None
        if ready is not None:
            returncode = process.poll()
            if returncode is not None:
                raise CaptureError(
                    f"USBPcapCMD exited early before pcap readiness with status {returncode}"
                )
            return CaptureReadiness(
                method="strict_first_record_parse",
                validation_deferred=False,
                observed_size_bytes=40 + len(ready.records[0].payload)
                + ready.records[0].header_len,
                first_record=ready,
            )
        remaining = deadline - monotonic_fn()
        if remaining <= 0:
            raise CaptureError(
                f"USBPcapCMD pcap readiness timed out after {timeout_s:g} s"
            )
        sleep_fn(min(poll_s, remaining))


def _run_tshark_readability_check(
    tshark: Path,
    raw_capture: Path,
    runner: Callable[..., subprocess.CompletedProcess] = subprocess.run,
) -> tuple[list[str], str]:
    command = build_tshark_readability_command(tshark, raw_capture)
    runner(command, check=True, capture_output=True)
    version_result = runner(
        [str(tshark), "--version"], check=True, capture_output=True, text=True
    )
    version = (
        version_result.stdout.splitlines()[0].strip()
        if version_result.stdout else "unknown"
    )
    return command, version


def _input_worker(
    input_fn: Callable[[str], str],
    prompt: str,
    completed: threading.Event,
    outcome: list[BaseException | None],
) -> None:
    try:
        input_fn(prompt)
        outcome.append(None)
    except BaseException as exc:
        outcome.append(exc)
    finally:
        completed.set()


def _wait_for_guided_input(
    input_fn: Callable[[str], str],
    prompt: str,
    watchdog_done: threading.Event,
    watchdog_state: dict,
) -> None:
    completed = threading.Event()
    outcome: list[BaseException | None] = []
    threading.Thread(
        target=_input_worker,
        args=(input_fn, prompt, completed, outcome),
        daemon=True,
    ).start()
    while not completed.wait(0.02):
        if watchdog_done.is_set() and watchdog_state.get("expired"):
            error = watchdog_state.get("error")
            if error is not None:
                raise CaptureError(f"capture watchdog stop failed: {error}") from error
            raise CaptureError("USBPcapCMD capture duration expired during guided input")
    if outcome and outcome[0] is not None:
        raise outcome[0]


def _start_watchdog(
    process: subprocess.Popen,
    timeout_s: float,
    stop_fn: Callable[[subprocess.Popen], dict],
) -> tuple[threading.Event, threading.Event, dict, threading.Thread]:
    cancel = threading.Event()
    done = threading.Event()
    state: dict = {"expired": False, "stop": None, "error": None}

    def watchdog() -> None:
        try:
            if not cancel.wait(timeout_s):
                state["expired"] = True
                state["stop"] = stop_fn(process)
        except BaseException as exc:
            state["error"] = exc
        finally:
            done.set()

    thread = threading.Thread(target=watchdog, daemon=True)
    thread.start()
    return cancel, done, state, thread


def run_live(args: argparse.Namespace, input_fn: Callable[[str], str] = input,
             time_ns_fn: Callable[[], int] = time.time_ns,
             monotonic_fn: Callable[[], float] = time.monotonic,
             runner: Callable[..., subprocess.CompletedProcess] = subprocess.run,
             popen_factory: Callable[..., subprocess.Popen] = subprocess.Popen,
             stop_fn: Callable[[subprocess.Popen], dict] = stop_windows_process_tree,
             snapshot_fn: Callable[[], dict[int, tuple[int, str]]] = windows_process_snapshot,
             watchdog_timeout_s: float | None = None,
             readiness_timeout_s: float = USBPCAP_READINESS_TIMEOUT_S,
             post_marker_grace_s: float = 0.05) -> dict | None:
    usbpcapcmd = find_usbpcapcmd_executable(getattr(args, "usbpcapcmd", None))
    if args.list_interfaces:
        print(list_capture_interfaces(usbpcapcmd), end="")
        return None
    tshark = find_wireshark_executable("tshark", getattr(args, "tshark", None))
    if args.interface is None or args.bus is None or args.device is None or args.endpoint is None:
        raise CaptureError("live capture requires --interface, --bus, --device, and --endpoint")
    endpoint = parse_int(args.endpoint, "endpoint")
    bus = parse_int(args.bus, "bus")
    device = parse_int(args.device, "device")
    validate_capture_selection(bus, device, endpoint)
    target = verify_capture_target(
        usbpcapcmd, args.interface, device=device, runner=runner
    )
    reject_existing_usbpcapcmd(snapshot_fn)
    command = build_usbpcapcmd_capture_command(
        usbpcapcmd,
        args.interface,
        args.raw_capture,
        args.duration_s,
        device=device,
    )
    _prepare_raw_capture(args.raw_capture)
    marker_entries: list[tuple[str, int]] = []
    write_live_markers(args.markers_json, marker_entries)
    start_epoch_ns = time_ns_fn()
    start_monotonic = monotonic_fn()
    process = popen_factory(command)
    timeout_s = (
        float(args.duration_s) if watchdog_timeout_s is None else watchdog_timeout_s
    )
    cancel: threading.Event | None = None
    watchdog_done = threading.Event()
    watchdog_state: dict = {"expired": False, "stop": None, "error": None}
    watchdog_thread: threading.Thread | None = None
    readiness: CaptureReadiness | None = None
    stop: dict = {"controlled": False, "method": None, "tree_pids": [process.pid]}
    try:
        readiness = _wait_for_capture_ready(
            args.raw_capture, process, timeout_s=readiness_timeout_s
        )
        print("Guided capture running: follow each right-stick prompt.")
        cancel, watchdog_done, watchdog_state, watchdog_thread = _start_watchdog(
            process, timeout_s, stop_fn
        )
        for name, prompt in ACTION_PROMPTS:
            if process.poll() is not None:
                raise CaptureError(f"USBPcapCMD exited early with status {process.returncode}")
            _wait_for_guided_input(
                input_fn, prompt + " ", watchdog_done, watchdog_state
            )
            marker_entries.append((name, time_ns_fn()))
            write_live_markers(args.markers_json, marker_entries)
        if post_marker_grace_s > 0:
            time.sleep(post_marker_grace_s)
    finally:
        write_live_markers(args.markers_json, marker_entries)
        if cancel is not None:
            cancel.set()
        if watchdog_thread is not None:
            watchdog_thread.join(timeout=11)
        if watchdog_state.get("stop") is not None:
            stop = watchdog_state["stop"]
        elif process.poll() is None:
            stop = stop_fn(process)
        else:
            stop["exit_code"] = process.returncode
    if watchdog_state.get("error") is not None:
        raise CaptureError(
            f"capture watchdog failed: {watchdog_state['error']}"
        ) from watchdog_state["error"]
    _validate_controlled_exit(process, stop)
    if not stop.get("controlled"):
        raise CaptureError("guided capture did not finish through a controlled process-tree stop")
    if len(marker_entries) != len(MARKER_ORDER):
        raise CaptureError("capture stopped before all guided markers were recorded")
    capture = _validate_created_capture(args.raw_capture, start_epoch_ns)
    selected = payload_completions(
        capture, bus=bus, device=device, endpoint=endpoint
    )
    if not selected:
        raise CaptureError("no payload-bearing selected interrupt IN completions")
    marker_epochs = [
        Decimal(epoch_ns) / Decimal(1_000_000_000) for _, epoch_ns in marker_entries
    ]
    if capture.earliest > marker_epochs[0] or capture.latest < marker_epochs[-1]:
        raise CaptureError(
            "raw pcap timestamps do not cover every guided marker epoch"
        )
    if not any(record.timestamp > marker_epochs[-1] for record in selected):
        raise CaptureError("final marker has no following selected interrupt IN report")
    write_usbpcap_tsv(selected, args.tsv)
    tshark_command, version = _run_tshark_readability_check(
        tshark, args.raw_capture, runner
    )
    elapsed = monotonic_fn() - start_monotonic
    provenance = {
        "mode": "GUIDED_EXPLICIT_SELECTION",
        "parser_schema": USBPCAP_PARSER_SCHEMA,
        "usbpcapcmd_command": command,
        "usbpcapcmd_version": target["version_line"],
        "interface": args.interface,
        "device": device,
        "filter": {
            "bus": bus,
            "device": device,
            "endpoint": f"0x{endpoint:02X}",
            "transfer": "INTERRUPT",
            "direction": "IN",
            "irp_info_direction": 1,
        },
        "enumerated_devices": target["devices"],
        "pid": process.pid,
        "exit_code": process.returncode,
        "controlled_stop": bool(stop.get("controlled")),
        "stop_method": stop.get("method"),
        "tree_pids": stop.get("tree_pids", [process.pid]),
        "elapsed_s": elapsed,
        "start_epoch_ns": start_epoch_ns,
        "readiness_timeout_s": readiness_timeout_s,
        "readiness_method": readiness.method if readiness is not None else None,
        "readiness_validation_deferred": (
            readiness.validation_deferred if readiness is not None else None
        ),
        "readiness_observed_size_bytes": (
            readiness.observed_size_bytes if readiness is not None else None
        ),
        "readiness_first_record_epoch_s": format(capture.earliest, "f"),
        "post_stop_strict_validation": True,
        "pcap_earliest_epoch_s": format(capture.earliest, "f"),
        "pcap_latest_epoch_s": format(capture.latest, "f"),
        "pcap_observed_duration_s": format(capture.observed_duration, "f"),
        "raw_capture_sha256": capture.sha256,
    }
    return convert_tshark_tsv(
        args.tsv, args.markers_json, bus=bus, device=device, endpoint=endpoint,
        out_csv=args.out_csv, audit_json=args.audit_json,
        max_marker_lag_ms=args.max_marker_lag_ms,
        tshark_command=tshark_command, tshark_version=version,
        raw_capture=args.raw_capture, capture_provenance=provenance,
    )


def run_discover(args: argparse.Namespace,
                 input_fn: Callable[[str], str] = input,
                 time_ns_fn: Callable[[], int] = time.time_ns,
                 monotonic_fn: Callable[[], float] = time.monotonic,
                 runner: Callable[..., subprocess.CompletedProcess] = subprocess.run,
                 popen_factory: Callable[..., subprocess.Popen] = subprocess.Popen,
                 stop_fn: Callable[[subprocess.Popen], dict] = stop_windows_process_tree,
                 snapshot_fn: Callable[[], dict[int, tuple[int, str]]] = windows_process_snapshot,
                 watchdog_timeout_s: float | None = None,
                 readiness_timeout_s: float = USBPCAP_READINESS_TIMEOUT_S) -> dict:
    usbpcapcmd = find_usbpcapcmd_executable(getattr(args, "usbpcapcmd", None))
    tshark = find_wireshark_executable("tshark", getattr(args, "tshark", None))
    if args.interface is None:
        raise CaptureError("discover requires --interface from live --list-interfaces")
    target = verify_capture_target(
        usbpcapcmd, args.interface, device=None, runner=runner
    )
    reject_existing_usbpcapcmd(snapshot_fn)
    command = build_discovery_usbpcapcmd_command(
        usbpcapcmd, args.interface, args.raw_capture, args.duration_s
    )
    input_fn(
        f"Press Enter to prepare a {args.duration_s} s read-only discovery. "
        "Wait for the capture-running message before moving the physical right stick. "
    )
    _prepare_raw_capture(args.raw_capture)
    start_epoch_ns = time_ns_fn()
    start_monotonic = monotonic_fn()
    process = popen_factory(command)
    timeout_s = (
        float(args.duration_s) if watchdog_timeout_s is None else watchdog_timeout_s
    )
    cancel: threading.Event | None = None
    watchdog_done = threading.Event()
    watchdog_state: dict = {"expired": False, "stop": None, "error": None}
    watchdog_thread: threading.Thread | None = None
    readiness: CaptureReadiness | None = None
    stop: dict = {"controlled": False, "method": None, "tree_pids": [process.pid]}
    try:
        readiness = _wait_for_capture_ready(
            args.raw_capture, process, timeout_s=readiness_timeout_s
        )
        print("Discovery capture running: move only the right stick; no endpoint is auto-selected.")
        cancel, watchdog_done, watchdog_state, watchdog_thread = _start_watchdog(
            process, timeout_s, stop_fn
        )
        process.wait(timeout=timeout_s + 11)
        if not watchdog_state.get("expired"):
            raise CaptureError(
                f"discovery USBPcapCMD exited naturally before the requested duration "
                f"with status {process.returncode}"
            )
    finally:
        if cancel is not None:
            cancel.set()
        if watchdog_thread is not None:
            watchdog_thread.join(timeout=11)
        if watchdog_state.get("stop") is not None:
            stop = watchdog_state["stop"]
        elif process.poll() is None:
            stop = stop_fn(process)
        else:
            stop["exit_code"] = process.returncode
    if watchdog_state.get("error") is not None:
        raise CaptureError(
            f"capture watchdog failed: {watchdog_state['error']}"
        ) from watchdog_state["error"]
    if not watchdog_state.get("expired") or not stop.get("controlled"):
        raise CaptureError("discovery did not finish through the controlled duration watchdog")
    _validate_controlled_exit(process, stop)
    capture = _validate_created_capture(args.raw_capture, start_epoch_ns)
    minimum_duration = Decimal(str(args.duration_s)) - DISCOVERY_DURATION_TOLERANCE_S
    if capture.observed_duration < minimum_duration:
        raise CaptureError(
            f"discovery pcap covers only {capture.observed_duration} s; "
            f"require at least {minimum_duration} s"
        )
    selected = payload_completions(capture)
    if not selected:
        raise CaptureError("discovery contains no payload-bearing interrupt IN completions")
    write_usbpcap_tsv(selected, args.tsv)
    tshark_command, version = _run_tshark_readability_check(
        tshark, args.raw_capture, runner
    )
    document = inspect_tshark_tsv(args.tsv)
    document["capture"] = {
        "mode": "READ_ONLY_DISCOVERY_NO_AUTO_SELECTION",
        "status": "PASS",
        "parser_schema": USBPCAP_PARSER_SCHEMA,
        "raw_capture_path": str(args.raw_capture),
        "raw_capture_sha256": capture.sha256,
        "tshark_tsv_path": str(args.tsv),
        "tshark_tsv_sha256": sha256_file(args.tsv),
        "usbpcapcmd_command": command,
        "usbpcapcmd_version": target["version_line"],
        "tshark_command": tshark_command,
        "tshark_version": version,
        "duration_s": args.duration_s,
        "pcap_observed_duration_s": format(capture.observed_duration, "f"),
        "pcap_earliest_epoch_s": format(capture.earliest, "f"),
        "pcap_latest_epoch_s": format(capture.latest, "f"),
        "start_epoch_ns": start_epoch_ns,
        "readiness_timeout_s": readiness_timeout_s,
        "readiness_method": readiness.method if readiness is not None else None,
        "readiness_validation_deferred": (
            readiness.validation_deferred if readiness is not None else None
        ),
        "readiness_observed_size_bytes": (
            readiness.observed_size_bytes if readiness is not None else None
        ),
        "readiness_first_record_epoch_s": format(capture.earliest, "f"),
        "post_stop_strict_validation": True,
        "elapsed_s": monotonic_fn() - start_monotonic,
        "pid": process.pid,
        "exit_code": process.returncode,
        "controlled_stop": True,
        "stop_method": stop.get("method"),
        "tree_pids": stop.get("tree_pids", [process.pid]),
        "interface": args.interface,
        "device": None,
        "filter": {
            "capture": "ALL_DEVICES_ON_EXPLICIT_ROOT_HUB",
            "analysis": "PDO_TO_FDO_INTERRUPT_IN_DATA_LENGTH_GT_0",
        },
        "enumerated_devices": target["devices"],
    }
    write_inspection_json(document, args.out_json)
    print(render_inspection_summary(document), end="")
    return document


def inspect_usbpcap_replay(
    raw_capture: Path,
    tsv: Path,
    out_json: Path,
    *,
    expected_duration_s: float | None = None,
) -> dict:
    """Offline parser replay.  It can never certify a formal live discovery."""
    capture = parse_classic_usbpcap(raw_capture)
    selected = payload_completions(capture)
    if not selected:
        raise CaptureError("pcap contains no payload-bearing interrupt IN completions")
    write_usbpcap_tsv(selected, tsv)
    document = inspect_tshark_tsv(tsv)
    if (
        expected_duration_s is not None
        and capture.observed_duration
        < Decimal(str(expected_duration_s)) - DISCOVERY_DURATION_TOLERANCE_S
    ):
        status = "PARTIAL_SHORT_CAPTURE"
    else:
        status = "PARTIAL_OFFLINE_REPLAY"
    document["capture"] = {
        "mode": "OFFLINE_USBPCAP_REPLAY_NO_AUTO_SELECTION",
        "status": status,
        "parser_schema": USBPCAP_PARSER_SCHEMA,
        "raw_capture_path": str(raw_capture),
        "raw_capture_sha256": capture.sha256,
        "tshark_tsv_path": str(tsv),
        "tshark_tsv_sha256": sha256_file(tsv),
        "pcap_earliest_epoch_s": format(capture.earliest, "f"),
        "pcap_latest_epoch_s": format(capture.latest, "f"),
        "pcap_observed_duration_s": format(capture.observed_duration, "f"),
        "expected_duration_s": expected_duration_s,
    }
    write_inspection_json(document, out_json)
    return document


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    offline = subparsers.add_parser("offline", help="convert deterministic tshark TSV")
    offline.add_argument("--input-tsv", type=Path, required=True)
    offline.add_argument("--markers-json", type=Path, required=True)
    offline.add_argument("--bus", type=lambda value: parse_int(value, "bus"), required=True)
    offline.add_argument("--device", type=lambda value: parse_int(value, "device"), required=True)
    offline.add_argument("--endpoint", type=lambda value: parse_int(value, "endpoint"), required=True)
    offline.add_argument("--out-csv", type=Path, required=True)
    offline.add_argument("--audit-json", type=Path, required=True)
    offline.add_argument("--max-marker-lag-ms", type=float, default=100.0)

    inspect = subparsers.add_parser(
        "inspect", help="rank unselected payload-bearing IN report groups for human review"
    )
    inspect.add_argument("--input-tsv", type=Path, required=True)
    inspect.add_argument("--out-json", type=Path, required=True)

    inspect_pcap = subparsers.add_parser(
        "inspect-pcap",
        help="strict offline classic pcap/USBPcap replay (always PARTIAL)",
    )
    inspect_pcap.add_argument("--pcap", dest="raw_capture", type=Path, required=True)
    inspect_pcap.add_argument("--tsv", type=Path, required=True)
    inspect_pcap.add_argument("--out-json", type=Path, required=True)
    inspect_pcap.add_argument("--expected-duration-s", type=float)

    live = subparsers.add_parser(
        "live", help="capture using an existing Wireshark/USBPcap installation"
    )
    live.add_argument("--usbpcapcmd", type=Path,
                      help="USBPcapCMD.exe path (defaults to the installed USBPcap path)")
    live.add_argument("--tshark", type=Path)
    live.add_argument("--list-interfaces", action="store_true")
    live.add_argument("--interface", help="explicit USBPcap root-hub value from USBPcapCMD --extcap-interfaces")
    live.add_argument("--bus")
    live.add_argument("--device")
    live.add_argument("--endpoint")
    live.add_argument("--duration-s", type=int, default=180)
    live.add_argument("--pcap", dest="raw_capture", type=Path,
                      default=Path("guided_capture.pcap"),
                      help="raw USBPcapCMD classic pcap output path")
    live.add_argument("--markers-json", type=Path, default=Path("guided_markers.json"))
    live.add_argument("--tsv", type=Path, default=Path("guided_capture.tsv"))
    live.add_argument("--out-csv", type=Path, default=Path("guided_trace.csv"))
    live.add_argument("--audit-json", type=Path, default=Path("guided_capture.audit.json"))
    live.add_argument("--max-marker-lag-ms", type=float, default=100.0)

    discover = subparsers.add_parser(
        "discover", help="record 5-15 s of unselected USB reports for read-only inspection"
    )
    discover.add_argument("--usbpcapcmd", type=Path)
    discover.add_argument("--tshark", type=Path)
    discover.add_argument("--interface", required=True,
                          help="explicit USBPcap root-hub value from live --list-interfaces")
    discover.add_argument("--duration-s", type=int, default=10)
    discover.add_argument("--pcap", dest="raw_capture", type=Path,
                          default=Path("usb_discovery.pcap"),
                          help="raw USBPcapCMD classic pcap output path")
    discover.add_argument("--tsv", type=Path, default=Path("usb_discovery.tsv"))
    discover.add_argument("--out-json", type=Path, default=Path("usb_discovery.json"))

    descriptors = subparsers.add_parser(
        "descriptors", help="validate and unpack usb_descriptor_snapshot.py JSON"
    )
    descriptors.add_argument("--snapshot", type=Path, required=True)
    descriptors.add_argument("--out-dir", type=Path, required=True)
    descriptors.add_argument(
        "--target-interface", type=lambda value: parse_int(value, "target interface"),
        required=True,
        help="controller interface number; must match the guided analyzer --interface",
    )

    preflight = subparsers.add_parser(
        "preflight", aliases=("doctor",),
        help="read-only Windows Wireshark/USBPcap and descriptor dependency audit",
    )
    preflight.add_argument("--usbpcapcmd", type=Path)
    preflight.add_argument("--tshark", type=Path)
    preflight.add_argument(
        "--snapshot-script", type=Path,
        default=Path(__file__).resolve().parents[1] / "tests" / "usb_descriptor_snapshot.py",
    )
    preflight.add_argument("--out-json", type=Path, default=Path("usb_preflight.json"))
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command in ("preflight", "doctor"):
            audit = run_preflight(args)
            write_inspection_json(audit, args.out_json)
            print(render_preflight_summary(audit), end="")
            return audit["exit_code"]
        if args.command == "offline":
            audit = convert_tshark_tsv(
                args.input_tsv, args.markers_json, bus=args.bus, device=args.device,
                endpoint=args.endpoint, out_csv=args.out_csv, audit_json=args.audit_json,
                max_marker_lag_ms=args.max_marker_lag_ms,
            )
        elif args.command == "inspect":
            audit = inspect_tshark_tsv(args.input_tsv)
            write_inspection_json(audit, args.out_json)
            print(render_inspection_summary(audit), end="")
        elif args.command == "inspect-pcap":
            audit = inspect_usbpcap_replay(
                args.raw_capture,
                args.tsv,
                args.out_json,
                expected_duration_s=args.expected_duration_s,
            )
            print(render_inspection_summary(audit), end="")
        elif args.command == "live":
            audit = run_live(args)
        elif args.command == "discover":
            audit = run_discover(args)
        else:
            audit = unpack_descriptor_snapshot(
                args.snapshot, args.out_dir, target_interface=args.target_interface
            )
    except (CaptureError, OSError, subprocess.SubprocessError) as exc:
        print(f"FAIL: {exc}")
        return 2
    if audit is not None:
        digest = audit.get("output_csv_sha256") or audit.get("snapshot_sha256")
        print(f"PASS: {digest or 'read-only inspection complete'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
