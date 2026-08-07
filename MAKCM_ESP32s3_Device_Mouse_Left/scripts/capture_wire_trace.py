#!/usr/bin/env python3
"""Dump and decode the left MCU's bounded MAKCU wire capture."""

from __future__ import annotations

import argparse
import binascii
import csv
import re
import struct
import time
from pathlib import Path

import serial


BEGIN_RE = re.compile(
    rb"BRIDGE_TRACE_BEGIN version=(\d+) bytes=(\d+) records=(\d+) "
    rb"overflow=(\d+) dropped=(\d+) auto=(\d+) crc32=([0-9A-Fa-f]{8})"
)
HEADER = struct.Struct("<IHBB")


def receive_dump(port: str, baud: int, timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    captured = bytearray()
    with serial.Serial(port, baud, timeout=0.1, write_timeout=1) as device:
        # Secondary leaves km.buttons(1,8) enabled when it closes. Stop those
        # binary frames before printing the hex dump or they can interleave
        # with its ASCII lines. This happens after capture and does not alter
        # the saved trace buffer.
        device.reset_input_buffer()
        device.write(b"km.buttons(0)\r\n")
        device.flush()
        time.sleep(0.1)
        device.reset_input_buffer()
        device.write(b"BRIDGE_TRACE_DUMP\r\n")
        device.flush()
        while time.monotonic() < deadline:
            block = device.read(device.in_waiting or 1)
            if block:
                captured.extend(block)
                if b"BRIDGE_TRACE_END" in captured:
                    return bytes(captured)
    raise TimeoutError("trace dump did not reach BRIDGE_TRACE_END")


def parse_dump(dump: bytes) -> tuple[dict[str, int], bytes]:
    match = BEGIN_RE.search(dump)
    if not match:
        raise ValueError("BRIDGE_TRACE_BEGIN header not found")
    names = ("version", "bytes", "records", "overflow", "dropped", "auto")
    metadata = {name: int(value) for name, value in zip(names, match.groups()[:6])}
    metadata["crc32"] = int(match.group(7), 16)
    payload_start = dump.find(b"\n", match.end()) + 1
    payload_end = dump.find(b"BRIDGE_TRACE_END", payload_start)
    if payload_start == 0 or payload_end < 0:
        raise ValueError("incomplete trace dump")
    # Each firmware line contains at most 64 bytes / 128 hex characters.
    # Runtime binary diagnostics can interleave after Print::print(line) and
    # before Print::println() writes CRLF. Keep the fixed hex prefix from each
    # physical line; the final CRC still rejects any mid-line corruption.
    hex_parts: list[bytes] = []
    for line in dump[payload_start:payload_end].splitlines():
        prefix = re.match(rb"[0-9A-Fa-f]+", line)
        if prefix:
            hex_parts.append(prefix.group(0)[:128])
    hex_payload = b"".join(hex_parts)
    payload = bytes.fromhex(hex_payload.decode("ascii"))
    if len(payload) != metadata["bytes"]:
        raise ValueError(
            f"length mismatch: header={metadata['bytes']} decoded={len(payload)}"
        )
    checksum = binascii.crc32(payload) & 0xFFFFFFFF
    if checksum != metadata["crc32"]:
        raise ValueError(
            f"CRC mismatch: header={metadata['crc32']:08X} decoded={checksum:08X}"
        )
    return metadata, payload


def decode_records(payload: bytes) -> list[tuple[int, str, bytes]]:
    records: list[tuple[int, str, bytes]] = []
    offset = 0
    while offset < len(payload):
        if len(payload) - offset < HEADER.size:
            raise ValueError(f"truncated record header at offset {offset}")
        timestamp_us, length, direction, _ = HEADER.unpack_from(payload, offset)
        offset += HEADER.size
        end = offset + length
        if end > len(payload):
            raise ValueError(f"truncated record payload at offset {offset}")
        label = "RX" if direction == 0 else "TX" if direction == 1 else f"DIR{direction}"
        records.append((timestamp_us, label, payload[offset:end]))
        offset = end
    return records


def printable(data: bytes) -> str:
    return "".join(chr(value) if 32 <= value < 127 else "." for value in data)


def write_outputs(out_dir: Path, dump: bytes, metadata: dict[str, int],
                  payload: bytes, records: list[tuple[int, str, bytes]]) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "makcu_wire_dump.txt").write_bytes(dump)
    (out_dir / "makcu_wire_trace.bin").write_bytes(payload)
    (out_dir / "makcu_rx.bin").write_bytes(
        b"".join(data for _, direction, data in records if direction == "RX")
    )
    (out_dir / "makcu_tx.bin").write_bytes(
        b"".join(data for _, direction, data in records if direction == "TX")
    )
    with (out_dir / "makcu_wire_trace.csv").open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(("timestamp_us", "direction", "length", "hex", "ascii"))
        for timestamp_us, direction, data in records:
            writer.writerow((timestamp_us, direction, len(data), data.hex(" "), printable(data)))
    with (out_dir / "makcu_wire_metadata.txt").open("w", encoding="utf-8") as file:
        for key, value in metadata.items():
            rendered = f"{value:08X}" if key == "crc32" else str(value)
            file.write(f"{key}={rendered}\n")


def summarize(records: list[tuple[int, str, bytes]]) -> str:
    rx_bytes = sum(len(data) for _, direction, data in records if direction == "RX")
    tx_bytes = sum(len(data) for _, direction, data in records if direction == "TX")
    move_times = [
        timestamp for timestamp, direction, data in records
        if direction == "RX" and b"km.move" in data
    ]
    max_gap_ms = 0.0
    if len(move_times) > 1:
        max_gap_ms = max(b - a for a, b in zip(move_times, move_times[1:])) / 1000.0
    return (
        f"records={len(records)} rx_bytes={rx_bytes} tx_bytes={tx_bytes} "
        f"km.move_chunks={len(move_times)} max_move_gap_ms={max_gap_ms:.3f}"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True, help="CH343 port, for example COM5")
    parser.add_argument("--baud", type=int, default=4_000_000)
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--out", type=Path, default=Path("wire_trace"))
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    dump = receive_dump(args.port, args.baud, args.timeout)
    # Keep the original bytes even when validation fails. Repeated read-only
    # dumps can then recover a line affected by unrelated UART output/noise.
    (args.out / "makcu_wire_dump_unparsed.bin").write_bytes(dump)
    metadata, payload = parse_dump(dump)
    records = decode_records(payload)
    if len(records) != metadata["records"]:
        raise ValueError(
            f"record mismatch: header={metadata['records']} decoded={len(records)}"
        )
    write_outputs(args.out, dump, metadata, payload, records)
    print(summarize(records))
    print(f"saved={args.out.resolve()}")


if __name__ == "__main__":
    main()
