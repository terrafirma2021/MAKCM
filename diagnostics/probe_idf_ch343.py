#!/usr/bin/env python3
"""Probe IDF left-board CH343 bring-up path."""

from __future__ import annotations

import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("NO_PYSERIAL")
    sys.exit(2)

SPEED_SWITCH = bytes([0xDE, 0xAD, 0x05, 0x00, 0xA5, 0x00, 0x09, 0x3D, 0x00])


def list_ports_now() -> None:
    ports = list(list_ports.comports())
    if not ports:
        print("NO_PORTS")
        return
    for item in ports:
        print(f"PORT\t{item.device}\t{item.description}\t{item.hwid}")


def read_for(ser: serial.Serial, seconds: float) -> bytes:
    deadline = time.time() + seconds
    buf = bytearray()
    while time.time() < deadline:
        chunk = ser.read(1024)
        if chunk:
            buf.extend(chunk)
        else:
            time.sleep(0.02)
    return bytes(buf)


def open_port(port: str, baud: int) -> serial.Serial:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.05
    ser.write_timeout = 1
    # Avoid holding the ESP32 in reset / download via CH343 handshake lines.
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.dtr = False
    ser.rts = False
    return ser


def pulse_reset(ser: serial.Serial) -> None:
    # Common CH343 wiring: RTS/DTR can reset ESP32-S3.
    ser.rts = True
    ser.dtr = True
    time.sleep(0.05)
    ser.rts = False
    ser.dtr = False
    time.sleep(0.05)


def probe_once(port: str, baud: int, reset: bool, label: str) -> None:
    print(f"=== {label}: {port} @ {baud} reset={reset} ===")
    try:
        ser = open_port(port, baud)
    except Exception as exc:  # noqa: BLE001
        print(f"OPEN_FAIL: {exc}")
        return

    if reset:
        pulse_reset(ser)
        boot = read_for(ser, 2.5)
        print(f"BOOT[{len(boot)}]: {boot!r}")
        if boot:
            print("BOOT_TXT:", boot.decode("utf-8", errors="replace"))
    else:
        time.sleep(0.1)
        drained = ser.read(8192)
        if drained:
            print(f"DRAIN[{len(drained)}]: {drained!r}")

    for cmd in (b"km.version()\r\n", b"km.echo(0)\r\n"):
        ser.reset_input_buffer()
        ser.write(cmd)
        ser.flush()
        print(f">> {cmd!r}")
        reply = read_for(ser, 1.0)
        print(f"<< {reply!r}")
        if reply:
            print("TXT:", reply.decode("utf-8", errors="replace"))

    print("--- passive 4s ---")
    passive = read_for(ser, 4.0)
    print(f"PASSIVE[{len(passive)}]: {passive!r}")
    if passive:
        print("TXT:", passive.decode("utf-8", errors="replace"))
    ser.close()


def probe_speed_switch(port: str) -> None:
    print(f"=== SPEED SWITCH then 4Mbps cmds: {port} ===")
    try:
        ser = open_port(port, 115200)
    except Exception as exc:  # noqa: BLE001
        print(f"OPEN_FAIL: {exc}")
        return
    ser.write(SPEED_SWITCH)
    ser.flush()
    time.sleep(0.2)
    ser.close()

    try:
        ser = open_port(port, 4000000)
    except Exception as exc:  # noqa: BLE001
        print(f"REOPEN_FAIL: {exc}")
        return
    for cmd in (b"km.version()\r\n", b"km.echo(0)\r\n"):
        ser.write(cmd)
        ser.flush()
        print(f">> {cmd!r}")
        reply = read_for(ser, 1.0)
        print(f"<< {reply!r}")
        if reply:
            print("TXT:", reply.decode("utf-8", errors="replace"))
    print("--- passive 4s ---")
    passive = read_for(ser, 4.0)
    print(f"PASSIVE[{len(passive)}]: {passive!r}")
    if passive:
        print("TXT:", passive.decode("utf-8", errors="replace"))
    ser.close()


def main() -> int:
    port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
    print("=== PORTS ===")
    list_ports_now()
    print()
    probe_once(port, 115200, reset=False, label="NO_RESET")
    print()
    probe_once(port, 115200, reset=True, label="WITH_RESET")
    print()
    probe_once(port, 4000000, reset=False, label="FAST_NO_RESET")
    print()
    probe_speed_switch(port)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
