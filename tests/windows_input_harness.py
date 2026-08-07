#!/usr/bin/env python3
"""Short Windows-layer concurrency probe for XInput + HID mouse.

The test intentionally uses the class drivers (XInputGetState and hid.dll)
because libusb cannot claim the native Windows interfaces.  A timeout on the
mouse read is normal when the mouse is stationary; an actual read or a
non-timeout error is recorded separately.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wintypes
import json
import time
from pathlib import Path

from windows_hid_snapshot import snapshot


class Gamepad(ctypes.Structure):
    _fields_ = [
        ("wButtons", ctypes.c_ushort),
        ("bLeftTrigger", ctypes.c_ubyte),
        ("bRightTrigger", ctypes.c_ubyte),
        ("sThumbLX", ctypes.c_short),
        ("sThumbLY", ctypes.c_short),
        ("sThumbRX", ctypes.c_short),
        ("sThumbRY", ctypes.c_short),
    ]


class XInputState(ctypes.Structure):
    _fields_ = [("packet", ctypes.c_uint32), ("gamepad", Gamepad)]


class Overlapped(ctypes.Structure):
    _fields_ = [
        ("Internal", ctypes.c_size_t),
        ("InternalHigh", ctypes.c_size_t),
        ("Pointer", ctypes.c_void_p),
        ("hEvent", wintypes.HANDLE),
    ]


ERROR_IO_PENDING = 997
ERROR_OPERATION_ABORTED = 995
WAIT_OBJECT_0 = 0
WAIT_TIMEOUT = 258
FILE_FLAG_OVERLAPPED = 0x40000000
GENERIC_READ = 0x80000000
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--vid", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--pid", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--out", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    hid_state = snapshot(args.vid, args.pid)
    mouse = next((item for item in hid_state["devices"]
                  if item["usage_page"] == 0x01 and item["usage"] == 0x02),
                 None)
    if mouse is None:
        data = {"xinput": {"error": "no XInput HID mouse collection"},
                "mouse": {"reads": 0, "timeouts": 0,
                           "errors": ["mouse collection not found"]}}
        args.out.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        return 3

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateFileW.argtypes = [
        wintypes.LPCWSTR, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
        ctypes.c_uint32, ctypes.c_uint32, wintypes.HANDLE
    ]
    kernel32.CreateFileW.restype = wintypes.HANDLE
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CreateEventW.argtypes = [ctypes.c_void_p, wintypes.BOOL,
                                      wintypes.BOOL, wintypes.LPCWSTR]
    kernel32.CreateEventW.restype = wintypes.HANDLE
    kernel32.ResetEvent.argtypes = [wintypes.HANDLE]
    kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel32.WaitForSingleObject.restype = wintypes.DWORD
    kernel32.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p,
                                  wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                                  ctypes.POINTER(Overlapped)]
    kernel32.ReadFile.restype = wintypes.BOOL
    kernel32.GetOverlappedResult.argtypes = [wintypes.HANDLE,
                                             ctypes.POINTER(Overlapped),
                                             ctypes.POINTER(wintypes.DWORD),
                                             wintypes.BOOL]
    kernel32.GetOverlappedResult.restype = wintypes.BOOL
    kernel32.CancelIoEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(Overlapped)]
    kernel32.CancelIoEx.restype = wintypes.BOOL

    handle = kernel32.CreateFileW(
        mouse["path"], GENERIC_READ, 0x03, None, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, None
    )
    if handle in (None, INVALID_HANDLE_VALUE):
        data = {"xinput": {}, "mouse": {"reads": 0, "timeouts": 0,
                                          "errors": [ctypes.get_last_error()]}}
        args.out.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        return 4

    xinput = ctypes.WinDLL("xinput1_4.dll")
    get_state = xinput.XInputGetState
    get_state.argtypes = [ctypes.c_uint32, ctypes.POINTER(XInputState)]
    get_state.restype = ctypes.c_uint32
    xinput_info = {"success": 0, "errors": [], "packet_changes": 0,
                   "last": None}
    mouse_info = {"reads": 0, "timeouts": 0, "errors": [], "samples": []}
    event = kernel32.CreateEventW(None, True, False, None)
    deadline = time.monotonic() + max(args.seconds, 0.1)
    last_packet = None
    try:
        while time.monotonic() < deadline:
            state = XInputState()
            code = int(get_state(0, ctypes.byref(state)))
            if code == 0:
                xinput_info["success"] += 1
                if last_packet is not None and state.packet != last_packet:
                    xinput_info["packet_changes"] += 1
                last_packet = state.packet
                xinput_info["last"] = {
                    "packet": state.packet,
                    "buttons": state.gamepad.wButtons,
                    "left_trigger": state.gamepad.bLeftTrigger,
                    "right_trigger": state.gamepad.bRightTrigger,
                    "lx": state.gamepad.sThumbLX,
                    "ly": state.gamepad.sThumbLY,
                    "rx": state.gamepad.sThumbRX,
                    "ry": state.gamepad.sThumbRY,
                }
            elif code not in xinput_info["errors"]:
                xinput_info["errors"].append(code)

            kernel32.ResetEvent(event)
            buffer = ctypes.create_string_buffer(
                max(int(mouse["input_report_length"]), 1))
            overlapped = Overlapped()
            overlapped.hEvent = event
            transferred = wintypes.DWORD()
            ok = kernel32.ReadFile(handle, buffer, len(buffer),
                                   ctypes.byref(transferred),
                                   ctypes.byref(overlapped))
            if not ok:
                error = ctypes.get_last_error()
                if error != ERROR_IO_PENDING:
                    if error != ERROR_OPERATION_ABORTED:
                        mouse_info["errors"].append(error)
                else:
                    wait = kernel32.WaitForSingleObject(event, 20)
                    if wait == WAIT_OBJECT_0:
                        if kernel32.GetOverlappedResult(
                                handle, ctypes.byref(overlapped),
                                ctypes.byref(transferred), False):
                            mouse_info["reads"] += 1
                            if len(mouse_info["samples"]) < 5:
                                mouse_info["samples"].append(
                                    bytes(buffer[:transferred.value]).hex())
                    elif wait == WAIT_TIMEOUT:
                        mouse_info["timeouts"] += 1
                        kernel32.CancelIoEx(handle, ctypes.byref(overlapped))
            else:
                mouse_info["reads"] += 1
                if len(mouse_info["samples"]) < 5:
                    mouse_info["samples"].append(
                        bytes(buffer[:transferred.value]).hex())
    finally:
        kernel32.CancelIoEx(handle, None)
        kernel32.CloseHandle(event)
        kernel32.CloseHandle(handle)

    data = {"xinput": xinput_info, "mouse": mouse_info,
            "hid_collection": mouse}
    args.out.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    print(f"wrote {args.out}")
    return 0 if xinput_info["success"] and not mouse_info["errors"] else 5


if __name__ == "__main__":
    raise SystemExit(main())
