#!/usr/bin/env python3
"""Flash left gen16 app via Windows COM3 download mode without kicking the port mid-way.

Uses --before no-reset --after no-reset. After success, power-cycle the board
(COM3 will disappear on reset — that is expected).
"""
from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

BIN = Path(__file__).resolve().parents[2] / (
    "diagnostics/ota/go1_freeze_20260806/left_app_gen16.bin"
)
# Running slot from prior Status V2 was ota_0 @ 0x20000.
OTA0_OFFSET = "0x20000"
PORT = "COM3"


def com3_present() -> bool:
    try:
        import serial.tools.list_ports
        return any(p.device.upper() == PORT for p in serial.tools.list_ports.comports())
    except Exception:
        # fallback powershell
        r = subprocess.run(
            [
                "powershell",
                "-NoProfile",
                "-Command",
                "([bool](Get-CimInstance Win32_SerialPort | "
                "Where-Object DeviceID -eq 'COM3'))",
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )
        return "True" in (r.stdout or "")


def main() -> int:
    if not BIN.is_file():
        raise SystemExit(f"missing {BIN}")
    print(f"bin={BIN} ({BIN.stat().st_size} bytes)", flush=True)
    print("Waiting for COM3 download mode (hold BOOT)...", flush=True)
    deadline = time.time() + 180
    while time.time() < deadline:
        if com3_present():
            print("COM3 present — flashing ota_0 @ 0x20000 (no-reset)", flush=True)
            break
        time.sleep(0.5)
    else:
        print("TIMEOUT: COM3 not found", flush=True)
        return 2

    cmd = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        "esp32s3",
        "-p",
        PORT,
        "-b",
        "460800",
        "--before",
        "no-reset",
        "--after",
        "no-reset",
        "write-flash",
        "--flash-mode",
        "dio",
        "--flash-freq",
        "80m",
        "--flash-size",
        "detect",
        OTA0_OFFSET,
        str(BIN),
    ]
    print(" ".join(cmd), flush=True)
    rc = subprocess.call(cmd)
    print(f"write_flash_exit={rc}", flush=True)
    if rc != 0:
        return rc
    print(
        "FLASH_OK — stay in bootloader. Power-cycle LEFT board now "
        "(COM3 will drop; that is normal).",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
