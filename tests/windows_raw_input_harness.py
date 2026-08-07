#!/usr/bin/env python3
"""Concurrent Windows user-mode pressure probe for the MAKCM composite.

This harness deliberately uses the Windows class interfaces instead of
opening the HID files directly.  XInput and the HID class driver therefore
remain owned by Windows while a hidden window receives Raw Input mouse
reports.  It is suitable for a high-rate run with Secondary producing
``km.move`` at the same time as a physical controller is being exercised.

The test does not synthesize USB traffic.  It records what the PC actually
observes: XInput poll/error/packet-change counts, target Raw Input mouse
events and deltas, event gaps, and the target HID device paths.  A run with
no physical movement is valid as a connectivity probe but must not be called
an input-throughput pass; use ``--require-target-activity`` and
``--require-xinput-change`` for the pressure gate.

Example (PowerShell):

    python tests\\windows_raw_input_harness.py --vid 0x4131 --pid 0x3519 \\
      --seconds 60 --out diagnostics\\raw_pressure.json \\
      --require-target-activity --require-xinput-change

Start this first, then hold/move the controller and run the Secondary
1000-Hz ``km.move`` generator.  The output is intentionally independent of
the generator implementation so it can also be used with a physical mouse.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wintypes
import json
import re
import threading
import time
from collections import Counter
from pathlib import Path


ULONG_PTR = ctypes.c_ulonglong if ctypes.sizeof(ctypes.c_void_p) == 8 else ctypes.c_ulong
LPARAM = ctypes.c_ssize_t
LRESULT = ctypes.c_ssize_t
HCURSOR = wintypes.HANDLE
HWND_MESSAGE = ctypes.c_void_p(-3)

WM_INPUT = 0x00FF
WM_CLOSE = 0x0010
WM_DESTROY = 0x0002
RID_INPUT = 0x10000003
RIDI_DEVICENAME = 0x20000007
RIM_TYPEMOUSE = 0
RIDEV_INPUTSINK = 0x00000100

ERROR_CLASS_ALREADY_EXISTS = 1410
WAIT_OBJECT_0 = 0


class XInputGamepad(ctypes.Structure):
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
    _fields_ = [("packet", ctypes.c_uint32), ("gamepad", XInputGamepad)]


class RawInputDevice(ctypes.Structure):
    _fields_ = [
        ("usUsagePage", ctypes.c_ushort),
        ("usUsage", ctypes.c_ushort),
        ("dwFlags", ctypes.c_uint32),
        ("hwndTarget", wintypes.HWND),
    ]


class RawInputHeader(ctypes.Structure):
    _fields_ = [
        ("dwType", ctypes.c_uint32),
        ("dwSize", ctypes.c_uint32),
        ("hDevice", wintypes.HANDLE),
        ("wParam", ULONG_PTR),
    ]


class RawMouse(ctypes.Structure):
    _fields_ = [
        ("usFlags", ctypes.c_ushort),
        ("_padding", ctypes.c_ushort),
        # RAWMOUSE's button union is at the aligned offset immediately after
        # usFlags.  We only need its two 16-bit views; declaring ulButtons
        # followed by them (instead of an overlay) shifts every field after
        # the union by four bytes and makes lLastX appear to be zero.
        ("usButtonFlags", ctypes.c_ushort),
        ("usButtonData", ctypes.c_short),
        ("ulRawButtons", ctypes.c_uint32),
        ("lLastX", ctypes.c_long),
        ("lLastY", ctypes.c_long),
        ("ulExtraInformation", ctypes.c_uint32),
    ]


WNDPROC = ctypes.WINFUNCTYPE(
    LRESULT, wintypes.HWND, wintypes.UINT, ULONG_PTR, LPARAM
)


class WndClass(ctypes.Structure):
    _fields_ = [
        ("style", ctypes.c_uint32),
        ("lpfnWndProc", WNDPROC),
        ("cbClsExtra", ctypes.c_int),
        ("cbWndExtra", ctypes.c_int),
        ("hInstance", wintypes.HINSTANCE),
        ("hIcon", wintypes.HICON),
        ("hCursor", HCURSOR),
        ("hbrBackground", wintypes.HBRUSH),
        ("lpszMenuName", wintypes.LPCWSTR),
        ("lpszClassName", wintypes.LPCWSTR),
    ]


class Msg(ctypes.Structure):
    _fields_ = [
        ("hwnd", wintypes.HWND),
        ("message", wintypes.UINT),
        ("wParam", ULONG_PTR),
        ("lParam", LPARAM),
        ("time", wintypes.DWORD),
        ("pt_x", ctypes.c_long),
        ("pt_y", ctypes.c_long),
    ]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vid", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--pid", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--user", type=int, default=0, help="XInput user index")
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--poll-ms", type=float, default=1.0)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--require-target-activity",
        action="store_true",
        help="fail unless at least one target Raw Input mouse report arrives",
    )
    parser.add_argument(
        "--require-xinput-change",
        action="store_true",
        help="fail unless XInput packet changes at least once",
    )
    parser.add_argument(
        "--max-target-gap-ms",
        type=float,
        default=0.0,
        help="optional maximum allowed target-mouse inter-report gap",
    )
    parser.add_argument(
        "--min-target-events",
        type=int,
        default=0,
        help="optional minimum number of target Raw Input reports",
    )
    parser.add_argument(
        "--sample-limit",
        type=int,
        default=32,
        help="maximum number of raw report samples kept in JSON",
    )
    return parser.parse_args()


def _target_pattern(vid: int, pid: int) -> re.Pattern[str]:
    return re.compile(
        rf"VID_{vid:04X}&PID_{pid:04X}", re.IGNORECASE
    )


class RawProbe:
    def __init__(self, user32: ctypes.WinDLL, target: re.Pattern[str], sample_limit: int):
        self.user32 = user32
        self.target = target
        self.sample_limit = max(0, sample_limit)
        self.lock = threading.Lock()
        self.raw_total = 0
        self.target_events = 0
        self.non_target_events = 0
        self.raw_errors: list[int] = []
        self.device_errors: list[int] = []
        self.target_dx = 0
        self.target_dy = 0
        self.target_wheel = 0
        self.target_button_events = 0
        self.target_paths: Counter[str] = Counter()
        self.samples: list[dict[str, object]] = []
        self.last_target_time: float | None = None
        self.max_target_gap_ms = 0.0
        self.gaps_over_20ms = 0
        self.gaps_over_50ms = 0
        self.last_error = 0

        self.user32.GetRawInputData.argtypes = [
            wintypes.HANDLE,
            wintypes.UINT,
            ctypes.c_void_p,
            ctypes.POINTER(wintypes.UINT),
            wintypes.UINT,
        ]
        self.user32.GetRawInputData.restype = wintypes.UINT
        self.user32.GetRawInputDeviceInfoW.argtypes = [
            wintypes.HANDLE,
            wintypes.UINT,
            ctypes.c_void_p,
            ctypes.POINTER(wintypes.UINT),
        ]
        self.user32.GetRawInputDeviceInfoW.restype = ctypes.c_int

    def device_path(self, handle: wintypes.HANDLE) -> str:
        if not handle:
            return ""
        length = wintypes.UINT(0)
        result = self.user32.GetRawInputDeviceInfoW(
            handle, RIDI_DEVICENAME, None, ctypes.byref(length)
        )
        if result == -1 or length.value == 0:
            error = ctypes.get_last_error()
            if error and error not in self.device_errors:
                self.device_errors.append(error)
            return ""
        buffer = ctypes.create_unicode_buffer(length.value + 1)
        capacity = wintypes.UINT(length.value + 1)
        result = self.user32.GetRawInputDeviceInfoW(
            handle, RIDI_DEVICENAME, buffer, ctypes.byref(capacity)
        )
        if result == -1:
            error = ctypes.get_last_error()
            if error and error not in self.device_errors:
                self.device_errors.append(error)
            return ""
        return buffer.value

    def consume(self, raw_handle: LPARAM) -> None:
        raw_handle_value = getattr(raw_handle, "value", raw_handle)
        required = wintypes.UINT(0)
        header_size = ctypes.sizeof(RawInputHeader)
        result = self.user32.GetRawInputData(
            wintypes.HANDLE(int(raw_handle_value)), RID_INPUT, None,
            ctypes.byref(required), header_size
        )
        if result == 0xFFFFFFFF or required.value < header_size:
            error = ctypes.get_last_error()
            if error and error not in self.raw_errors:
                self.raw_errors.append(error)
            return
        buffer = ctypes.create_string_buffer(required.value)
        result = self.user32.GetRawInputData(
            wintypes.HANDLE(int(raw_handle_value)), RID_INPUT, buffer,
            ctypes.byref(required), header_size
        )
        if result == 0xFFFFFFFF or result < header_size:
            error = ctypes.get_last_error()
            if error and error not in self.raw_errors:
                self.raw_errors.append(error)
            return
        header = ctypes.cast(buffer, ctypes.POINTER(RawInputHeader)).contents
        with self.lock:
            self.raw_total += 1
        if header.dwType != RIM_TYPEMOUSE:
            return
        mouse = ctypes.cast(
            ctypes.addressof(buffer) + header_size,
            ctypes.POINTER(RawMouse),
        ).contents
        path = self.device_path(header.hDevice)
        now = time.monotonic()
        is_target = bool(self.target.search(path))
        with self.lock:
            if not is_target:
                self.non_target_events += 1
                return
            self.target_events += 1
            self.target_paths[path] += 1
            self.target_dx += int(mouse.lLastX)
            self.target_dy += int(mouse.lLastY)
            if mouse.usButtonFlags & 0x0400:  # RI_MOUSE_WHEEL
                self.target_wheel += int(mouse.usButtonData)
            if mouse.usButtonFlags:
                self.target_button_events += 1
            if self.last_target_time is not None:
                gap_ms = (now - self.last_target_time) * 1000.0
                self.max_target_gap_ms = max(self.max_target_gap_ms, gap_ms)
                if gap_ms > 20.0:
                    self.gaps_over_20ms += 1
                if gap_ms > 50.0:
                    self.gaps_over_50ms += 1
            self.last_target_time = now
            if len(self.samples) < self.sample_limit:
                self.samples.append({
                    "t_ms": round(now * 1000.0, 3),
                    "path": path,
                    "raw_hex": bytes(buffer[header_size:]).hex(),
                    "flags": int(mouse.usFlags),
                    "button_flags": int(mouse.usButtonFlags),
                    "button_data": int(mouse.usButtonData),
                    "dx": int(mouse.lLastX),
                    "dy": int(mouse.lLastY),
                })

    def snapshot(self) -> dict[str, object]:
        with self.lock:
            return {
                "raw_total": self.raw_total,
                "target_events": self.target_events,
                "non_target_events": self.non_target_events,
                "target_dx": self.target_dx,
                "target_dy": self.target_dy,
                "target_wheel": self.target_wheel,
                "target_button_events": self.target_button_events,
                "target_paths": dict(self.target_paths),
                "max_target_gap_ms": round(self.max_target_gap_ms, 3),
                "gaps_over_20ms": self.gaps_over_20ms,
                "gaps_over_50ms": self.gaps_over_50ms,
                "samples": list(self.samples),
                "errors": list(self.raw_errors),
                "device_info_errors": list(self.device_errors),
            }


def xinput_worker(
    xinput_info: dict[str, object], user: int, poll_ms: float,
    deadline: float, stop: threading.Event,
) -> None:
    try:
        xinput = ctypes.WinDLL("xinput1_4.dll")
    except OSError:
        try:
            xinput = ctypes.WinDLL("xinput1_3.dll")
        except OSError as error:
            xinput_info["load_error"] = str(error)
            return
    get_state = xinput.XInputGetState
    get_state.argtypes = [ctypes.c_uint32, ctypes.POINTER(XInputState)]
    get_state.restype = ctypes.c_uint32
    errors: list[int] = []
    xinput_info["errors"] = errors
    last_packet: int | None = None
    last_change: float | None = None
    max_change_gap = 0.0
    interval = max(poll_ms, 0.1) / 1000.0
    next_report = time.monotonic() + 1.0
    while not stop.is_set() and time.monotonic() < deadline:
        started = time.monotonic()
        state = XInputState()
        code = int(get_state(user, ctypes.byref(state)))
        xinput_info["polls"] = int(xinput_info.get("polls", 0)) + 1
        if code == 0:
            xinput_info["success"] = int(xinput_info.get("success", 0)) + 1
            packet = int(state.packet)
            if last_packet is not None and packet != last_packet:
                xinput_info["packet_changes"] = int(
                    xinput_info.get("packet_changes", 0)
                ) + 1
                now = time.monotonic()
                if last_change is not None:
                    max_change_gap = max(max_change_gap, (now - last_change) * 1000.0)
                last_change = now
            last_packet = packet
            xinput_info["last"] = {
                "packet": packet,
                "buttons": int(state.gamepad.wButtons),
                "left_trigger": int(state.gamepad.bLeftTrigger),
                "right_trigger": int(state.gamepad.bRightTrigger),
                "lx": int(state.gamepad.sThumbLX),
                "ly": int(state.gamepad.sThumbLY),
                "rx": int(state.gamepad.sThumbRX),
                "ry": int(state.gamepad.sThumbRY),
            }
        elif code not in errors:
            errors.append(code)
        now = time.monotonic()
        if now >= next_report:
            print(
                "progress: xinput=%d/%d changes=%d"
                % (
                    int(xinput_info.get("success", 0)),
                    int(xinput_info.get("polls", 0)),
                    int(xinput_info.get("packet_changes", 0)),
                )
            )
            next_report = now + 1.0
        remaining = interval - (time.monotonic() - started)
        if remaining > 0:
            stop.wait(remaining)
    xinput_info["max_packet_change_gap_ms"] = round(max_change_gap, 3)


def run(args: argparse.Namespace) -> tuple[dict[str, object], int]:
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    user32.RegisterRawInputDevices.argtypes = [
        ctypes.POINTER(RawInputDevice), wintypes.UINT, ctypes.c_uint
    ]
    user32.RegisterRawInputDevices.restype = wintypes.BOOL
    user32.RegisterClassW.argtypes = [ctypes.POINTER(WndClass)]
    user32.RegisterClassW.restype = wintypes.ATOM
    user32.CreateWindowExW.argtypes = [
        wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,
        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
        wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, ctypes.c_void_p,
    ]
    user32.CreateWindowExW.restype = wintypes.HWND
    user32.DefWindowProcW.argtypes = [wintypes.HWND, wintypes.UINT, ULONG_PTR, LPARAM]
    user32.DefWindowProcW.restype = LRESULT
    user32.DestroyWindow.argtypes = [wintypes.HWND]
    user32.DestroyWindow.restype = wintypes.BOOL
    user32.PostQuitMessage.argtypes = [ctypes.c_int]
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, ULONG_PTR, LPARAM]
    user32.PostMessageW.restype = wintypes.BOOL
    user32.GetMessageW.argtypes = [ctypes.POINTER(Msg), wintypes.HWND, wintypes.UINT, wintypes.UINT]
    user32.GetMessageW.restype = ctypes.c_int
    user32.TranslateMessage.argtypes = [ctypes.POINTER(Msg)]
    user32.DispatchMessageW.argtypes = [ctypes.POINTER(Msg)]
    kernel32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
    kernel32.GetModuleHandleW.restype = wintypes.HINSTANCE

    target = _target_pattern(args.vid, args.pid)
    probe = RawProbe(user32, target, args.sample_limit)
    xinput_info: dict[str, object] = {
        "user": args.user,
        "polls": 0,
        "success": 0,
        "packet_changes": 0,
        "errors": [],
    }
    stop = threading.Event()
    class_name = f"MAKCMRawInput_{os_process_id()}_{id(probe):x}"
    callback_holder: dict[str, WNDPROC] = {}

    @WNDPROC
    def window_proc(hwnd: wintypes.HWND, message: wintypes.UINT,
                   wparam: ULONG_PTR, lparam: LPARAM) -> LRESULT:
        if int(message) == WM_INPUT:
            probe.consume(lparam)
            return 0
        if int(message) == WM_CLOSE:
            user32.DestroyWindow(wintypes.HWND(getattr(hwnd, "value", hwnd)))
            return 0
        if int(message) == WM_DESTROY:
            user32.PostQuitMessage(0)
            return 0
        return int(user32.DefWindowProcW(
            wintypes.HWND(getattr(hwnd, "value", hwnd)), message, wparam, lparam
        ))

    callback_holder["wndproc"] = window_proc
    hinstance = kernel32.GetModuleHandleW(None)
    wnd_class = WndClass(
        0, window_proc, 0, 0, hinstance, None, None, None, None, class_name
    )
    atom = user32.RegisterClassW(ctypes.byref(wnd_class))
    if not atom:
        error = ctypes.get_last_error()
        if error != ERROR_CLASS_ALREADY_EXISTS:
            return {"error": "RegisterClassW", "winerror": error}, 4
    hwnd = user32.CreateWindowExW(
        0, class_name, class_name, 0, 0, 0, 0, 0, None, None, hinstance, None
    )
    if not hwnd:
        return {"error": "CreateWindowExW", "winerror": ctypes.get_last_error()}, 4

    devices = (RawInputDevice * 1)(RawInputDevice(
        0x01, 0x02, RIDEV_INPUTSINK, hwnd
    ))
    if not user32.RegisterRawInputDevices(
        devices, 1, ctypes.sizeof(RawInputDevice)
    ):
        error = ctypes.get_last_error()
        user32.DestroyWindow(hwnd)
        return {"error": "RegisterRawInputDevices", "winerror": error}, 4

    deadline = time.monotonic() + max(args.seconds, 0.1)
    worker = threading.Thread(
        target=xinput_worker,
        args=(xinput_info, args.user, args.poll_ms, deadline, stop),
        daemon=True,
    )
    worker.start()

    def close_window() -> None:
        remaining = max(0.0, deadline - time.monotonic())
        if remaining:
            time.sleep(remaining)
        user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)

    closer = threading.Thread(target=close_window, daemon=True)
    closer.start()
    msg = Msg()
    while True:
        result = user32.GetMessageW(ctypes.byref(msg), None, 0, 0)
        if result <= 0:
            break
        user32.TranslateMessage(ctypes.byref(msg))
        user32.DispatchMessageW(ctypes.byref(msg))
    stop.set()
    worker.join(timeout=2.0)
    raw = probe.snapshot()
    result: dict[str, object] = {
        "vid": f"{args.vid:04x}",
        "pid": f"{args.pid:04x}",
        "seconds": args.seconds,
        "target_pattern": target.pattern,
        "xinput": xinput_info,
        "raw_mouse": raw,
    }
    failures: list[str] = []
    if xinput_info.get("load_error"):
        failures.append("xinput_load")
    if raw["errors"]:
        failures.append("raw_input")
    if args.require_target_activity and not raw["target_events"]:
        failures.append("no_target_mouse_activity")
    if args.min_target_events > 0 and int(raw["target_events"]) < args.min_target_events:
        failures.append("target_event_count")
    if args.require_xinput_change and not xinput_info.get("packet_changes"):
        failures.append("no_xinput_packet_change")
    if args.max_target_gap_ms > 0 and raw["target_events"] >= 2:
        if float(raw["max_target_gap_ms"]) > args.max_target_gap_ms:
            failures.append("target_gap")
    result["failures"] = failures
    result["pass"] = not failures and int(xinput_info.get("success", 0)) > 0
    return result, 0 if result["pass"] else 5


def os_process_id() -> int:
    # Avoid importing os solely for a unique class name; a monotonic fragment
    # is enough because the class is registered and destroyed in one process.
    return int(time.monotonic_ns() & 0xFFFFFFFF)


def main() -> int:
    args = parse_args()
    result, code = run(args)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    print(f"wrote {args.out}")
    raw = result.get("raw_mouse", {})
    xinput = result.get("xinput", {})
    print(
        "summary: xinput=%s/%s changes=%s target_raw=%s max_gap_ms=%s failures=%s"
        % (
            xinput.get("success", 0), xinput.get("polls", 0),
            xinput.get("packet_changes", 0), raw.get("target_events", 0),
            raw.get("max_target_gap_ms", 0), result.get("failures", []),
        )
    )
    return code


if __name__ == "__main__":
    raise SystemExit(main())
