#!/usr/bin/env python3
"""Read raw CH343 bytes from Linux (boot log / any traffic)."""
from __future__ import annotations

import sys

import paramiko

REMOTE = r'''
set +e
fuser -k /dev/ttyACM0 2>/dev/null
sleep 1
python3 - <<'PY'
import serial, time, sys
for baud in (115200, 4_000_000):
    print(f'=== open {baud} ===', flush=True)
    try:
        s = serial.Serial('/dev/ttyACM0', baud, timeout=0.2)
    except Exception as e:
        print('open_err', e, flush=True)
        continue
    # DTR pulse attempt
    try:
        s.setDTR(False); time.sleep(0.05); s.setDTR(True); time.sleep(0.05); s.setDTR(False)
    except Exception as e:
        print('dtr_err', e, flush=True)
    end = time.time() + 3.0
    buf = bytearray()
    while time.time() < end:
        chunk = s.read(4096)
        if chunk:
            buf.extend(chunk)
    s.close()
    print(f'got {len(buf)} bytes @ {baud}', flush=True)
    if buf:
        # printable preview
        text = buf.decode('utf-8', 'replace')
        print(text[-2000:], flush=True)
        print('hex_head', buf[:64].hex(), flush=True)
PY
'''

def main() -> int:
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(
        "192.168.137.2",
        username="li",
        password="1234567",
        look_for_keys=False,
        allow_agent=False,
        timeout=10,
    )
    try:
        _, o, e = c.exec_command(REMOTE, timeout=40)
        sys.stdout.write(o.read().decode("utf-8", "replace"))
        err = e.read().decode("utf-8", "replace")
        if err.strip():
            sys.stderr.write(err)
        return o.channel.recv_exit_status()
    finally:
        c.close()

if __name__ == "__main__":
    raise SystemExit(main())
