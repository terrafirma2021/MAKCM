#!/usr/bin/env python3
"""Capture Windows USB1 (left-board Device) descriptors for GO-1 G-08.

READ ONLY. No OTA, no Flash, no COM download port (not COM3).

Usage:
  python diagnostics/ota/go1_capture_usb1_descriptor.py --list
  python diagnostics/ota/go1_capture_usb1_descriptor.py --vid 0x4131 --pid 0x3519
  python diagnostics/ota/go1_capture_usb1_descriptor.py --vid 0x4131 --pid 0x3519 --via hub
"""

from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TESTS = ROOT / "tests"
if str(TESTS) not in sys.path:
    sys.path.insert(0, str(TESTS))

MAINTENANCE_CLASSES = {
    0x02: "CDC",
    0x08: "MSC",
    0x0A: "CDC_DATA",
    0xFE: "APP_SPECIFIC_OR_DFU_CANDIDATE",
}


def analyze_topology(topology: dict) -> dict:
    findings: list[dict] = []
    for iface in topology.get("interfaces") or []:
        cls = int(iface.get("class") or 0)
        if cls in MAINTENANCE_CLASSES:
            findings.append({
                "interface": iface.get("number"),
                "class": cls,
                "label": MAINTENANCE_CLASSES[cls],
                "subclass": iface.get("subclass"),
                "protocol": iface.get("protocol"),
            })
    return {
        "maintenance_like_interfaces": findings,
        "g08_static_pass": len(findings) == 0,
    }


def capture_libusb(vid: int, pid: int) -> dict:
    from usb_descriptor_snapshot import snapshot as libusb_snapshot

    data = libusb_snapshot(argparse.Namespace(vid=vid, pid=pid))
    data["capture_path"] = "libusb_control"
    data["analysis"] = analyze_topology(data.get("topology") or {})
    return data


def capture_hub(vid: int, pid: int) -> dict:
    import windows_usb_hub_snapshot as hub

    data = hub.snapshot(vid, pid)
    data["capture_path"] = "windows_hub"
    data["analysis"] = analyze_topology(data.get("topology") or {})
    return data


def try_list_devices() -> list[dict]:
    try:
        import libusb_package  # type: ignore
        import usb.core  # type: ignore
    except ImportError:
        return []
    backend = libusb_package.get_libusb1_backend()
    devices = []
    for dev in usb.core.find(find_all=True, backend=backend) or []:
        try:
            devices.append({
                "vid": f"{int(dev.idVendor):04x}",
                "pid": f"{int(dev.idProduct):04x}",
                "bus": getattr(dev, "bus", None),
                "address": getattr(dev, "address", None),
            })
        except Exception:
            continue
    return devices


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--vid", type=lambda x: int(x, 0))
    parser.add_argument("--pid", type=lambda x: int(x, 0))
    parser.add_argument("--via", choices=("libusb", "hub"), default="hub",
                        help="hub IOCTL is preferred on Windows with HID claimed")
    parser.add_argument("--label", default="usb1_idle")
    args = parser.parse_args()

    out_dir = Path(__file__).resolve().parent / "go1_live_readonly"
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.list:
        devices = try_list_devices()
        print(json.dumps({"devices": devices, "count": len(devices)}, indent=2))
        return 0 if devices else 1

    if args.vid is None or args.pid is None:
        parser.error("provide --list, or both --vid and --pid")

    if args.via == "hub":
        data = capture_hub(args.vid, args.pid)
    else:
        data = capture_libusb(args.vid, args.pid)

    stamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    record = {
        "schema": "makcm.go1-usb1-descriptor.v1",
        "captured_utc": datetime.now(timezone.utc).isoformat(),
        "flash_write": False,
        "ota_commands": False,
        "windows_com_download": False,
        "label": args.label,
        "snapshot": data,
    }
    path = out_dir / f"{args.label}_{stamp}.json"
    path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    analysis = data.get("analysis") or {}
    print(json.dumps({
        "saved": str(path),
        "g08_static_pass": analysis.get("g08_static_pass"),
        "maintenance_like_interfaces": analysis.get(
            "maintenance_like_interfaces"),
        "vid": data.get("vid"),
        "pid": data.get("pid"),
        "hub_port": data.get("port"),
    }, indent=2))
    return 0 if analysis.get("g08_static_pass") else 2


if __name__ == "__main__":
    raise SystemExit(main())
