#!/usr/bin/env python3
"""Download-mode NVS erase via CH343. Hold BOOT while connecting."""
from __future__ import annotations

import sys
import time

import paramiko

HOST = "192.168.137.2"
PASSWORD = "1234567"

REMOTE = r'''
set +e
export PYTHONUNBUFFERED=1
fuser -k /dev/ttyACM0 2>/dev/null
sleep 1
echo "=== chip_id (HOLD BOOT) ==="
timeout 35 python3 -u -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 115200 \
  --before default_reset --after no_reset --connect-attempts 10 chip_id
ec=$?
echo CHIP_EXIT:$ec
if [ "$ec" -ne 0 ]; then
  exit $ec
fi
echo "=== erase NVS @ 0x9000 len 0x6000 ==="
timeout 60 python3 -u -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
  --before no_reset --after hard_reset erase_region 0x9000 0x6000
ec=$?
echo ERASE_EXIT:$ec
exit $ec
'''


def main() -> int:
    print(">>> HOLD BOOT on LEFT board NOW — keep holding until CHIP_EXIT:0 <<<", flush=True)
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(
        HOST, username="li", password=PASSWORD,
        look_for_keys=False, allow_agent=False, timeout=10,
    )
    try:
        chan = client.get_transport().open_session()
        chan.get_pty()
        chan.exec_command(REMOTE)
        deadline = time.time() + 100
        buf = ""
        while time.time() < deadline:
            if chan.recv_ready():
                chunk = chan.recv(4096).decode("utf-8", "replace")
                buf += chunk
                sys.stdout.write(chunk)
                sys.stdout.flush()
            if chan.recv_stderr_ready():
                chunk = chan.recv_stderr(4096).decode("utf-8", "replace")
                buf += chunk
                sys.stderr.write(chunk)
                sys.stderr.flush()
            if chan.exit_status_ready():
                break
            time.sleep(0.15)
        while chan.recv_ready():
            chunk = chan.recv(4096).decode("utf-8", "replace")
            buf += chunk
            sys.stdout.write(chunk)
        code = chan.recv_exit_status() if chan.exit_status_ready() else 2
        print(f"remote_exit={code}", flush=True)
        return 0 if "ERASE_EXIT:0" in buf else code
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
