#!/usr/bin/env python3
"""Capture Windows HID logical collections for one VID/PID.

PyUSB can be denied by the native Windows HID/XInput drivers even while the
logical HID interfaces are active.  This read-only probe uses SetupAPI and
hid.dll instead, and records the report lengths/usages that Windows actually
opened.  It is complementary to usb_descriptor_snapshot.py: it cannot prove
raw USB descriptor bytes, but it can prove whether Windows has live mouse,
gamepad, keyboard, and vendor HID collections.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wintypes
import json
from pathlib import Path


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


class HidAttributes(ctypes.Structure):
    _fields_ = [
        ("Size", ctypes.c_uint32),
        ("VendorID", ctypes.c_ushort),
        ("ProductID", ctypes.c_ushort),
        ("VersionNumber", ctypes.c_ushort),
    ]


class HidCaps(ctypes.Structure):
    _fields_ = [
        ("Usage", ctypes.c_ushort),
        ("UsagePage", ctypes.c_ushort),
        ("InputReportByteLength", ctypes.c_ushort),
        ("OutputReportByteLength", ctypes.c_ushort),
        ("FeatureReportByteLength", ctypes.c_ushort),
        ("Reserved", ctypes.c_ushort * 17),
        ("NumberLinkCollectionNodes", ctypes.c_ushort),
        ("NumberInputButtonCaps", ctypes.c_ushort),
        ("NumberInputValueCaps", ctypes.c_ushort),
        ("NumberInputDataIndices", ctypes.c_ushort),
        ("NumberOutputButtonCaps", ctypes.c_ushort),
        ("NumberOutputValueCaps", ctypes.c_ushort),
        ("NumberOutputDataIndices", ctypes.c_ushort),
        ("NumberFeatureButtonCaps", ctypes.c_ushort),
        ("NumberFeatureValueCaps", ctypes.c_ushort),
        ("NumberFeatureDataIndices", ctypes.c_ushort),
    ]


HID_INTERFACE_GUID = GUID(
    0x4D1E55B2,
    0xF16F,
    0x11CF,
    (ctypes.c_ubyte * 8)(0x88, 0xCB, 0, 0x11, 0x11, 0, 0, 0x30),
)
DIGCF_PRESENT = 0x02
DIGCF_DEVICEINTERFACE = 0x10
ERROR_NO_MORE_ITEMS = 259
INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value


def configure_api():
    setupapi = ctypes.WinDLL("setupapi", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    hid = ctypes.WinDLL("hid", use_last_error=True)

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

    hid.HidD_GetAttributes.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(HidAttributes)
    ]
    hid.HidD_GetAttributes.restype = wintypes.BOOL
    hid.HidD_GetPreparsedData.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(ctypes.c_void_p)
    ]
    hid.HidD_GetPreparsedData.restype = wintypes.BOOL
    hid.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]
    hid.HidP_GetCaps.argtypes = [ctypes.c_void_p, ctypes.POINTER(HidCaps)]
    hid.HidP_GetCaps.restype = ctypes.c_long
    return setupapi, kernel32, hid


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--vid", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--pid", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--out", type=Path, required=True)
    return parser.parse_args()


def snapshot(vid: int, pid: int) -> dict:
    setupapi, kernel32, hid = configure_api()
    info = setupapi.SetupDiGetClassDevsW(
        ctypes.byref(HID_INTERFACE_GUID), None, None,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE,
    )
    if info in (None, INVALID_HANDLE_VALUE):
        raise ctypes.WinError(ctypes.get_last_error())

    devices: list[dict] = []
    try:
        for index in range(1024):
            interface = DeviceInterfaceData()
            interface.cbSize = ctypes.sizeof(interface)
            if not setupapi.SetupDiEnumDeviceInterfaces(
                    info, None, ctypes.byref(HID_INTERFACE_GUID), index,
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
            # The Windows structure's cbSize is 8 on x64, but DevicePath is
            # located immediately after the DWORD at byte offset 4.
            ctypes.cast(detail, ctypes.POINTER(ctypes.c_uint32))[0] = 8
            if not setupapi.SetupDiGetDeviceInterfaceDetailW(
                    info, ctypes.byref(interface), detail, required.value,
                    ctypes.byref(required), None):
                continue
            path = ctypes.wstring_at(ctypes.addressof(detail) + 4)
            handle = kernel32.CreateFileW(
                path, 0, 0x03, None, 3, 0, None
            )
            if handle in (None, INVALID_HANDLE_VALUE):
                continue
            try:
                attributes = HidAttributes()
                attributes.Size = ctypes.sizeof(attributes)
                if not hid.HidD_GetAttributes(handle,
                                              ctypes.byref(attributes)):
                    continue
                if attributes.VendorID != vid or attributes.ProductID != pid:
                    continue
                preparsed = ctypes.c_void_p()
                if not hid.HidD_GetPreparsedData(
                        handle, ctypes.byref(preparsed)):
                    continue
                try:
                    caps = HidCaps()
                    status = hid.HidP_GetCaps(
                        preparsed, ctypes.byref(caps)
                    ) & 0xFFFFFFFF
                    devices.append({
                        "path": path,
                        "vid": f"{attributes.VendorID:04x}",
                        "pid": f"{attributes.ProductID:04x}",
                        "usage_page": caps.UsagePage,
                        "usage": caps.Usage,
                        "input_report_length": caps.InputReportByteLength,
                        "output_report_length": caps.OutputReportByteLength,
                        "feature_report_length": caps.FeatureReportByteLength,
                        "link_collection_nodes": caps.NumberLinkCollectionNodes,
                        "input_value_caps": caps.NumberInputValueCaps,
                        "output_value_caps": caps.NumberOutputValueCaps,
                        "hidp_status": f"0x{status:08x}",
                    })
                finally:
                    hid.HidD_FreePreparsedData(preparsed)
            finally:
                kernel32.CloseHandle(handle)
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(info)
    return {"vid": f"{vid:04x}", "pid": f"{pid:04x}", "devices": devices}


def main() -> int:
    args = parse_args()
    data = snapshot(args.vid, args.pid)
    args.out.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    print(f"wrote {args.out} ({len(data['devices'])} HID collections)")
    return 0 if data["devices"] else 3


if __name__ == "__main__":
    raise SystemExit(main())
