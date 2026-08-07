#!/usr/bin/env python3
"""Small libusb/PyUSB smoke harness for the native left proxy.

It deliberately does not assume a single XInput report size.  After the device
enumerates it starts one reader per interrupt IN endpoint, then optionally
writes a bounded packet to each OUT endpoint.  The output makes it obvious
whether EP81 and EP83 progress independently instead of one queue starving the
other.

Example (Linux):
    python3 tests/usb_host_harness.py --vid 0x4131 --pid 0x3519 --seconds 10

PyUSB/libusb is an optional host-side dependency; the script reports a clear
installation hint when it is not available.
"""

from __future__ import annotations

import argparse
import threading
import time
from collections import Counter


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--vid", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--pid", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--out-byte", type=lambda value: int(value, 0), default=0)
    parser.add_argument(
        "--expect-in", type=int, default=0,
        help="require at least this many interrupt IN endpoints (0 disables)",
    )
    parser.add_argument(
        "--expect-out", type=int, default=0,
        help="require at least this many interrupt OUT endpoints (0 disables)",
    )
    parser.add_argument(
        "--out-hz", type=float, default=125.0,
        help="per-OUT-endpoint write rate used by the pressure test",
    )
    return parser.parse_args()


def main() -> int:
    try:
        import usb.core  # type: ignore
        import usb.util  # type: ignore
        import libusb_package  # type: ignore
    except ImportError:
        print("PyUSB/libusb is required: python3 -m pip install pyusb libusb-package")
        return 2

    args = parse_args()
    # PyUSB does not automatically discover the bundled Windows DLL supplied
    # by libusb-package.  Passing the backend explicitly makes the same
    # harness usable on Windows and Linux, and avoids silently reporting that
    # no backend exists after PyUSB itself was installed successfully.
    backend = libusb_package.get_libusb1_backend()
    if backend is None:
        print("libusb-1.0 backend could not be loaded")
        return 2
    try:
        device = usb.core.find(idVendor=args.vid, idProduct=args.pid, backend=backend)
    except usb.core.NoBackendError as error:
        print(f"libusb backend unavailable: {error}")
        return 2
    if device is None:
        print(f"device {args.vid:04x}:{args.pid:04x} not found")
        return 3
    device.set_configuration()
    configuration = device.get_active_configuration()
    readers: list[tuple[int, int]] = []
    writers: list[tuple[int, int]] = []
    for interface in configuration:
        for endpoint in interface:
            address = int(endpoint.bEndpointAddress)
            direction = usb.util.endpoint_direction(address)
            transfer = usb.util.endpoint_type(endpoint.bmAttributes)
            if transfer != usb.util.ENDPOINT_TYPE_INTR:
                continue
            if direction == usb.util.ENDPOINT_IN:
                readers.append((address, int(endpoint.wMaxPacketSize)))
            else:
                writers.append((address, int(endpoint.wMaxPacketSize)))
    print(f"interfaces={configuration.bNumInterfaces} IN={readers} OUT={writers}")
    if args.expect_in and len(readers) < args.expect_in:
        print(f"FAIL: expected at least {args.expect_in} IN endpoints")
        return 4
    if args.expect_out and len(writers) < args.expect_out:
        print(f"FAIL: expected at least {args.expect_out} OUT endpoints")
        return 4

    counts = Counter()
    write_counts = Counter()
    errors = Counter()
    last_read = {}
    max_gap = Counter()
    lock = threading.Lock()
    stop = threading.Event()

    def read_loop(address: int, packet_size: int) -> None:
        while not stop.is_set():
            try:
                data = device.read(address, packet_size, timeout=250)
                now = time.monotonic()
                with lock:
                    counts[address] += 1
                    previous = last_read.get(address)
                    if previous is not None:
                        max_gap[address] = max(max_gap[address], now - previous)
                    last_read[address] = now
                    count = counts[address]
                if count <= 3:
                    print(f"EP{address:02x} len={len(data)} {bytes(data).hex()}")
            except usb.core.USBTimeoutError:
                continue
            except usb.core.USBError as error:
                print(f"EP{address:02x} read error: {error}")
                errors[address] += 1
                break

    threads = [
        threading.Thread(target=read_loop, args=endpoint, daemon=True)
        for endpoint in readers
    ]
    for thread in threads:
        thread.start()

    def write_loop(address: int, packet_size: int) -> None:
        interval = 1.0 / max(args.out_hz, 1.0)
        payload = bytes([args.out_byte & 0xFF]) * min(packet_size, 1)
        while not stop.is_set():
            started = time.monotonic()
            try:
                device.write(address, payload, timeout=250)
                with lock:
                    write_counts[address] += 1
            except usb.core.USBError as error:
                print(f"EP{address:02x} write error: {error}")
                errors[address] += 1
                break
            remaining = interval - (time.monotonic() - started)
            if remaining > 0:
                stop.wait(remaining)

    writer_threads = [
        threading.Thread(target=write_loop, args=endpoint, daemon=True)
        for endpoint in writers
    ]
    for thread in writer_threads:
        thread.start()

    deadline = time.monotonic() + max(args.seconds, 0.1)
    while time.monotonic() < deadline:
        time.sleep(0.05)
    stop.set()
    for thread in threads:
        thread.join(timeout=1.0)
    for thread in writer_threads:
        thread.join(timeout=1.0)
    print("counts=" + ", ".join(f"EP{k:02x}:{v}" for k, v in sorted(counts.items())))
    print("writes=" + ", ".join(f"EP{k:02x}:{v}" for k, v in sorted(write_counts.items())))
    print("max_gap_ms=" + ", ".join(
        f"EP{k:02x}:{v * 1000.0:.3f}" for k, v in sorted(max_gap.items())))
    if readers and any(counts[address] == 0 for address, _ in readers):
        print("FAIL: at least one interrupt IN endpoint made no progress")
        return 4
    if writers and any(write_counts[address] == 0 for address, _ in writers):
        print("FAIL: at least one interrupt OUT endpoint made no progress")
        return 4
    if errors:
        print("FAIL: USB transfer errors=" + repr(dict(errors)))
        return 4
    print("PASS: independent IN endpoint progress observed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
