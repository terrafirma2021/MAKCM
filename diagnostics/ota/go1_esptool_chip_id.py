#!/usr/bin/env python3
"""Try esptool chip_id with short connect timeout; stream output."""
from __future__ import annotations

import sys
import time

import paramiko

HOST = "192.168.137.2"
PASSWORD = "1234567"

REMOTE = r'''
set +e
fuser -k /dev/ttyACM0 2>/dev/null
sleep 1
python3 -m pip install --user -q esptool >/tmp/esptool_pip.txt 2>&1
export PATH="$HOME/.local/bin:$PATH"
python3 -m esptool version
echo "CONNECTING (hold BOOT)..."
timeout 25 python3 -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 115200 \
  --before default_reset --after no_reset --connect-attempts 5 chip_id
echo EXIT:$?
'''


def main() -> int:
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(
        HOST, username="li", password=PASSWORD,
        look_for_keys=False, allow_agent=False, timeout=10,
    )
    try:
        transport = client.get_transport()
        channel = transport.open_session()
        channel.settimeout(60)
        channel.exec_command(REMOTE)
        deadline = time.time() + 55
        while time.time() < deadline:
            if channel.recv_ready():
                data = channel.recv(4096).decode("utf-8", "replace")
                sys.stdout.write(data)
                sys.stdout.flush()
            if channel.recv_stderr_ready():
                data = channel.recv_stderr(4096).decode("utf-8", "replace")
                sys.stderr.write(data)
                sys.stderr.flush()
            if channel.exit_status_ready():
                break
            time.sleep(0.2)
        while channel.recv_ready():
            sys.stdout.write(channel.recv(4096).decode("utf-8", "replace"))
        if channel.exit_status_ready():
            return channel.recv_exit_status()
        channel.close()
        print("TIMEOUT waiting for esptool", flush=True)
        return 2
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
