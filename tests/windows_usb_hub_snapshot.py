#!/usr/bin/env python3
"""Read a live USB device descriptor through the Windows hub driver.

Native HID/XInput drivers can make a libusb control request return a partial
or unusable view.  IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION is a
read-only hub-driver path and therefore provides the authoritative descriptor
bytes for the device currently attached to a physical root-hub port.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wintypes
import json
import struct
from pathlib import Path

from usb_descriptor_snapshot import configuration_topology


class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", ctypes.c_uint32),
        ("Data2", ctypes.c_uint16),
        ("Data3", ctypes.c_uint16),
        ("Data4", ctypes.c_ubyte * 8),
    ]


class DeviceInterfaceData(ctypes.Structure):
    _fields_ = [
        ("cbSize", ctypes.c_uint32),
        ("InterfaceClassGuid", GUID),
        ("Flags", ctypes.c_uint32),
        ("Reserved", ctypes.c_void_p),
    ]


USB_HUB_GUID = GUID(
    0xF18A0E88,
    0xC30C,
    0x11D0,
    (ctypes.c_ubyte * 8)(0x88, 0x15, 0, 0xA0, 0xC9, 0x06, 0xBE, 0xD8),
)
USB_DESCRIPTOR_IOCTL = 0x220410  # FILE_DEVICE_UNKNOWN, function 260
USB_CONNECTION_INFO_EX_IOCTL = 0x220448  # FILE_DEVICE_UNKNOWN, function 274
ERROR_NO_MORE_ITEMS = 259
INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--vid", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--pid", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--out", type=Path, required=True)
    return parser.parse_args()


def configure_api():
    setupapi = ctypes.WinDLL("setupapi", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    setupapi.SetupDiGetClassDevsW.argtypes = [
        ctypes.POINTER(GUID), wintypes.LPCWSTR, wintypes.HWND, ctypes.c_uint32
    ]
    setupapi.SetupDiGetClassDevsW.restype = wintypes.HANDLE
    setupapi.SetupDiEnumDeviceInterfaces.argtypes = [
        wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(GUID),
        ctypes.c_uint32, ctypes.POINTER(DeviceInterfaceData)
    ]
    setupapi.SetupDiEnumDeviceInterfaces.restype = wintypes.BOOL
    setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(DeviceInterfaceData), ctypes.c_void_p,
        ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p
    ]
    setupapi.SetupDiGetDeviceInterfaceDetailW.restype = wintypes.BOOL
    setupapi.SetupDiDestroyDeviceInfoList.argtypes = [wintypes.HANDLE]
    kernel32.CreateFileW.argtypes = [
        wintypes.LPCWSTR, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
        ctypes.c_uint32, ctypes.c_uint32, wintypes.HANDLE
    ]
    kernel32.CreateFileW.restype = wintypes.HANDLE
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.DeviceIoControl.argtypes = [
        wintypes.HANDLE, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32,
        ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32),
        ctypes.c_void_p
    ]
    kernel32.DeviceIoControl.restype = wintypes.BOOL
    return setupapi, kernel32


def hub_paths(setupapi):
    info = setupapi.SetupDiGetClassDevsW(
        ctypes.byref(USB_HUB_GUID), None, None, 0x12
    )
    if info in (None, INVALID_HANDLE_VALUE):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        for index in range(1024):
            interface = DeviceInterfaceData()
            interface.cbSize = ctypes.sizeof(interface)
            if not setupapi.SetupDiEnumDeviceInterfaces(
                    info, None, ctypes.byref(USB_HUB_GUID), index,
                    ctypes.byref(interface)):
                if ctypes.get_last_error() == ERROR_NO_MORE_ITEMS:
                    break
                continue
            required = ctypes.c_uint32()
            setupapi.SetupDiGetDeviceInterfaceDetailW(
                info, ctypes.byref(interface), None, 0,
                ctypes.byref(required), None)
            if required.value == 0:
                continue
            detail = ctypes.create_string_buffer(required.value)
            ctypes.cast(detail, ctypes.POINTER(ctypes.c_uint32))[0] = 8
            if setupapi.SetupDiGetDeviceInterfaceDetailW(
                    info, ctypes.byref(interface), detail, required.value,
                    ctypes.byref(required), None):
                # DevicePath is at byte offset four despite cbSize=8 on x64.
                yield ctypes.wstring_at(ctypes.addressof(detail) + 4)
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(info)


def ioctl(kernel32, handle, code, request, output_size):
    output = ctypes.create_string_buffer(output_size)
    returned = ctypes.c_uint32()
    ok = kernel32.DeviceIoControl(
        handle, code, request, len(request), output, output_size,
        ctypes.byref(returned), None
    )
    if not ok:
        return None
    return bytes(output[:returned.value])


def descriptor(kernel32, handle, port, descriptor_type, length):
    # USB_DESCRIPTOR_REQUEST is packed: ULONG + USB setup packet (8 bytes).
    request = struct.pack(
        "<IBBHHH", port, 0x80, 0x06, descriptor_type << 8, 0, length
    )
    response = ioctl(kernel32, handle, USB_DESCRIPTOR_IOCTL,
                     request, 12 + length)
    if response is None or len(response) < 12:
        return None
    return response[12:]


def snapshot(vid: int, pid: int) -> dict:
    setupapi, kernel32 = configure_api()
    for hub in hub_paths(setupapi):
        handle = kernel32.CreateFileW(hub, 0, 0x03, None, 3, 0, None)
        if handle in (None, INVALID_HANDLE_VALUE):
            continue
        try:
            for port in range(1, 65):
                request = struct.pack("<I", port)
                info = ioctl(kernel32, handle, USB_CONNECTION_INFO_EX_IOCTL,
                             request, 256)
                if info is None or len(info) < 22:
                    continue
                device = info[4:22]
                if len(device) < 18:
                    continue
                found_vid = device[8] | (device[9] << 8)
                found_pid = device[10] | (device[11] << 8)
                if found_vid != vid or found_pid != pid:
                    continue
                config_header = descriptor(kernel32, handle, port, 2, 9)
                if config_header is None or len(config_header) < 9:
                    continue
                total = config_header[2] | (config_header[3] << 8)
                configuration = descriptor(kernel32, handle, port, 2, total)
                if configuration is None or len(configuration) != total:
                    continue
                return {
                    "hub": hub,
                    "port": port,
                    "vid": f"{vid:04x}",
                    "pid": f"{pid:04x}",
                    "device": device.hex(),
                    "configuration": configuration.hex(),
                    "topology": configuration_topology(configuration),
                    # Keep the same snapshot schema consumed by
                    # guided_usb_capture.py. The hub IOCTL path is used for
                    # the authoritative device/configuration identity; it
                    # does not retrieve class-specific HID report
                    # descriptors or optional string/BOS/qualifier data.
                    "strings": {},
                    "hid_reports": {},
                    "bos": None,
                    "qualifier": None,
                    "errors": [],
                }
        finally:
            kernel32.CloseHandle(handle)
    raise RuntimeError(f"live USB device {vid:04x}:{pid:04x} not found")


def main() -> int:
    args = parse_args()
    data = snapshot(args.vid, args.pid)
    args.out.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    print(f"wrote {args.out}: hub port {data['port']}, "
          f"configuration {data['topology']['raw_length']} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
