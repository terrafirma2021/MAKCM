#!/usr/bin/env python3
"""Capture raw USB descriptors for direct-vs-proxy comparison.

Example:
    python tests/usb_descriptor_snapshot.py --vid 0x4131 --pid 0x3519 \
        --out direct.json

The script intentionally uses standard control GET_DESCRIPTOR requests rather
than class-specific parsers.  A failed optional BOS/Qualifier request is
recorded as absent.  Referenced string/HID failures are recorded in the JSON
and make the process exit with status 2 after the partial snapshot is written;
this keeps driver-claimed Windows interfaces diagnosable instead of dropping
the evidence.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Iterable


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--vid", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--pid", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--out", type=Path, required=True)
    return parser.parse_args()


def raw_control(device, request_type: int, value: int, index: int,
                length: int) -> bytes:
    data = device.ctrl_transfer(request_type, 6, value, index, length)
    return bytes(int(byte) & 0xFF for byte in data)


def walk_descriptors(data: bytes) -> Iterable[tuple[int, bytes]]:
    offset = 0
    while offset < len(data):
        if offset + 2 > len(data):
            raise ValueError(f"truncated descriptor header at {offset}")
        length = data[offset]
        if length < 2 or offset + length > len(data):
            raise ValueError(f"invalid descriptor length at {offset}")
        yield offset, data[offset:offset + length]
        offset += length


def referenced_string_indices(device: bytes, configuration: bytes) -> list[int]:
    indices: set[int] = set()
    if len(device) >= 17:
        indices.update(device[14:17])
    for _, descriptor in walk_descriptors(configuration):
        if descriptor[1] == 2 and len(descriptor) >= 9:
            indices.add(descriptor[6])
        elif descriptor[1] == 4 and len(descriptor) >= 9:
            indices.add(descriptor[8])
        elif descriptor[1] == 11 and len(descriptor) >= 8:
            indices.add(descriptor[7])
    indices.discard(0)
    indices.discard(0xEE)
    return sorted(indices)


def configuration_topology(configuration: bytes) -> dict:
    """Return a compact, class-agnostic view of the raw config blob."""
    if len(configuration) < 9:
        raise ValueError("short configuration descriptor")
    interfaces: list[dict] = []
    endpoints: list[dict] = []
    current: dict | None = None
    for _, descriptor in walk_descriptors(configuration):
        dtype = descriptor[1]
        if dtype == 4 and len(descriptor) >= 9:
            current = {
                "number": descriptor[2],
                "alternate": descriptor[3],
                "class": descriptor[5],
                "subclass": descriptor[6],
                "protocol": descriptor[7],
                "endpoints": [],
            }
            interfaces.append(current)
        elif dtype == 5 and len(descriptor) >= 7:
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
    return {
        "declared_total": configuration[2] | (configuration[3] << 8),
        "raw_length": len(configuration),
        "declared_interfaces": configuration[4],
        "interfaces": interfaces,
        "endpoints": endpoints,
    }


def hid_reports(device, configuration: bytes,
                errors: list[str]) -> dict[str, str]:
    reports: dict[str, str] = {}
    current_interface: int | None = None
    current_class: int | None = None
    for _, descriptor in walk_descriptors(configuration):
        if descriptor[1] == 4 and len(descriptor) >= 9:
            current_interface = descriptor[2]
            current_class = descriptor[5]
        # Descriptor type 0x21 is also used by vendor/XInput interfaces.  It
        # is an HID descriptor only when the owning interface declares class
        # 0x03; otherwise requesting 0x22 would turn a valid vendor blob into
        # a false snapshot failure.
        elif (descriptor[1] == 0x21 and current_interface is not None and
              current_class == 0x03):
            if len(descriptor) < 9:
                raise ValueError("short HID descriptor")
            report_type = descriptor[6]
            if report_type != 0x22:
                continue
            length = descriptor[7] | (descriptor[8] << 8)
            try:
                report = raw_control(device, 0x81, 0x2200, current_interface,
                                     length)
            except Exception as error:  # pragma: no cover - host dependent
                errors.append(
                    f"hid_report:{current_interface}:{error}")
                continue
            if len(report) != length:
                errors.append(
                    f"hid_report_length:{current_interface}:"
                    f"{len(report)} != {length}"
                )
                continue
            reports[str(current_interface)] = report.hex()
    return reports


def snapshot(args: argparse.Namespace) -> dict:
    try:
        import libusb_package  # type: ignore
        import usb.core  # type: ignore
    except ImportError as error:
        raise SystemExit(
            "PyUSB/libusb is required: python -m pip install pyusb "
            "libusb-package"
        ) from error
    backend = libusb_package.get_libusb1_backend()
    if backend is None:
        raise SystemExit("libusb-1.0 backend could not be loaded")
    device = usb.core.find(idVendor=args.vid, idProduct=args.pid,
                           backend=backend)
    if device is None:
        raise SystemExit(f"device {args.vid:04x}:{args.pid:04x} not found")

    device_descriptor = raw_control(device, 0x80, 0x0100, 0, 18)
    if len(device_descriptor) < 18:
        raise ValueError("short device descriptor")
    config_header = raw_control(device, 0x80, 0x0200, 0, 9)
    if len(config_header) < 9:
        raise ValueError("short configuration descriptor")
    total = config_header[2] | (config_header[3] << 8)
    configuration = raw_control(device, 0x80, 0x0200, 0, total)
    if len(configuration) != total:
        raise ValueError(f"configuration length {len(configuration)} != {total}")

    errors: list[str] = []
    result: dict = {
        "vid": f"{args.vid:04x}",
        "pid": f"{args.pid:04x}",
        "device": device_descriptor.hex(),
        "configuration": configuration.hex(),
        "topology": configuration_topology(configuration),
        "strings": {},
        "hid_reports": hid_reports(device, configuration, errors),
        "bos": None,
        "qualifier": None,
        "errors": errors,
    }
    try:
        bos = raw_control(device, 0x80, 0x0F00, 0, 256)
        result["bos"] = bos.hex()
    except Exception:
        pass
    try:
        qualifier = raw_control(device, 0x80, 0x0600, 0, 10)
        if len(qualifier) >= 10:
            result["qualifier"] = qualifier.hex()
    except Exception:
        pass

    langids: list[int] = []
    try:
        lang_descriptor = raw_control(device, 0x80, 0x0300, 0, 255)
        for offset in range(2, len(lang_descriptor) - 1, 2):
            langids.append(lang_descriptor[offset] |
                           (lang_descriptor[offset + 1] << 8))
        result["strings"]["0:0000"] = lang_descriptor.hex()
    except Exception:
        langids = [0x0409]
    for index in referenced_string_indices(device_descriptor, configuration):
        for langid in langids or [0x0409]:
            try:
                data = raw_control(device, 0x80, 0x0300 | index, langid, 255)
                result["strings"][f"{index}:{langid:04x}"] = data.hex()
            except Exception as error:  # pragma: no cover - host dependent
                errors.append(f"string:{index}:{langid:04x}:{error}")
    # Microsoft OS string 0xEE is optional but useful for the XInput census.
    try:
        data = raw_control(device, 0x80, 0x03EE, 0, 255)
        result["strings"]["238:0000"] = data.hex()
    except Exception:
        pass
    return result


def main() -> int:
    args = parse_args()
    data = snapshot(args)
    args.out.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    print(f"wrote {args.out}")
    if data.get("errors"):
        print(f"snapshot captured with {len(data['errors'])} descriptor errors")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
