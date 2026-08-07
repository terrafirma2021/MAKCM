#!/usr/bin/env python3
"""Windows class-driver probe for both controller OUT paths.

XInputSetState exercises the XInput vibration OUT report while HidD's output
report API exercises the vendor HID collection (the second OUT endpoint in the
MAKCM profile).  This does not bypass Windows' native drivers or claim the USB
device, so it can run while the normal input observer is active.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wintypes
import json
import time
from pathlib import Path

from windows_hid_snapshot import configure_api, snapshot


class Vibration(ctypes.Structure):
    _fields_ = [
        ("wLeftMotorSpeed", ctypes.c_ushort),
        ("wRightMotorSpeed", ctypes.c_ushort),
    ]


INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x01
FILE_SHARE_WRITE = 0x02
OPEN_EXISTING = 3


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vid", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--pid", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--hz", type=float, default=50.0)
    parser.add_argument("--out", type=Path, required=True)
    return parser.parse_args()


def run(args: argparse.Namespace) -> tuple[dict[str, object], int]:
    hid_state = snapshot(args.vid, args.pid)
    vendor = next(
        (
            item for item in hid_state["devices"]
            if int(item["usage_page"]) >= 0xFF00
            and int(item["output_report_length"]) > 0
        ),
        None,
    )
    setupapi = kernel32 = hid = None
    handle = None
    vendor_errors: list[int] = []
    vendor_calls = 0
    vendor_success = 0
    xinput_errors: list[int] = []
    xinput_calls = 0
    xinput_success = 0

    if vendor is not None:
        _, kernel32, hid = configure_api()
        kernel32.CreateFileW.argtypes = [
            wintypes.LPCWSTR, ctypes.c_uint32, ctypes.c_uint32,
            ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, wintypes.HANDLE,
        ]
        kernel32.CreateFileW.restype = wintypes.HANDLE
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = kernel32.CreateFileW(
            vendor["path"], GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING, 0, None,
        )
        if handle in (None, INVALID_HANDLE_VALUE):
            vendor_errors.append(ctypes.get_last_error())
            handle = None
        else:
            hid.HidD_SetOutputReport.argtypes = [
                wintypes.HANDLE, ctypes.c_void_p, ctypes.c_uint32,
            ]
            hid.HidD_SetOutputReport.restype = wintypes.BOOL

    try:
        try:
            xinput = ctypes.WinDLL("xinput1_4.dll")
        except OSError:
            xinput = ctypes.WinDLL("xinput1_3.dll")
        set_state = xinput.XInputSetState
        set_state.argtypes = [ctypes.c_uint32, ctypes.POINTER(Vibration)]
        set_state.restype = ctypes.c_uint32
    except OSError as error:
        set_state = None
        xinput_errors.append(abs(hash(str(error))) & 0xFFFFFFFF)

    hz = max(args.hz, 1.0)
    deadline = time.monotonic() + max(args.seconds, 0.1)
    next_tick = time.monotonic()
    period = 1.0 / hz
    tick = 0
    try:
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now < next_tick:
                time.sleep(min(next_tick - now, period / 2.0))
                continue
            if set_state is not None:
                vibration = Vibration(
                    0x2000 if tick & 1 else 0,
                    0x1000 if tick & 1 else 0,
                )
                code = int(set_state(0, ctypes.byref(vibration)))
                xinput_calls += 1
                if code == 0:
                    xinput_success += 1
                elif code not in xinput_errors:
                    xinput_errors.append(code)
            if handle is not None and hid is not None:
                length = max(int(vendor["output_report_length"]), 1)
                report = (ctypes.c_ubyte * length)()
                # The vendor collection has no report ID in the current
                # profile, so byte zero remains zero; the rest is a pattern
                # that makes every write distinguishable on a wire trace.
                for index in range(1, length):
                    report[index] = (tick + index) & 0xFF
                vendor_calls += 1
                if hid.HidD_SetOutputReport(handle, report, length):
                    vendor_success += 1
                else:
                    error = ctypes.get_last_error()
                    if error and error not in vendor_errors:
                        vendor_errors.append(error)
            tick += 1
            next_tick += period
            if now > next_tick + period * 3:
                next_tick = now
    finally:
        if set_state is not None:
            zero = Vibration(0, 0)
            set_state(0, ctypes.byref(zero))
        if handle is not None and kernel32 is not None:
            kernel32.CloseHandle(handle)

    failures: list[str] = []
    if vendor is None:
        failures.append("vendor_output_collection_missing")
    if vendor_errors or (vendor_calls and vendor_success != vendor_calls):
        failures.append("vendor_output")
    if xinput_errors or (xinput_calls and xinput_success != xinput_calls):
        failures.append("xinput_output")
    if xinput_calls == 0:
        failures.append("xinput_unavailable")
    if vendor_calls == 0:
        failures.append("vendor_not_attempted")
    result: dict[str, object] = {
        "vid": f"{args.vid:04x}",
        "pid": f"{args.pid:04x}",
        "seconds": args.seconds,
        "hz": hz,
        "vendor_collection": vendor,
        "xinput": {
            "calls": xinput_calls,
            "success": xinput_success,
            "errors": xinput_errors,
        },
        "vendor_output": {
            "calls": vendor_calls,
            "success": vendor_success,
            "errors": vendor_errors,
        },
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
        "summary: xinput=%s/%s vendor=%s/%s failures=%s"
        % (
            result["xinput"]["success"], result["xinput"]["calls"],
            result["vendor_output"]["success"], result["vendor_output"]["calls"],
            result["failures"],
        )
    )
    return code


if __name__ == "__main__":
    raise SystemExit(main())
