#!/usr/bin/env python3
"""Probe UART1 recovery after COM6 flash: LEFT_STATUS, READY, Enter, Abort."""
from __future__ import annotations

import json
import sys

import paramiko

HOST = "192.168.137.2"
PASSWORD = "1234567"

REMOTE = r"""
export PATH=/usr/bin:/bin
fuser -k /dev/ttyACM0 2>/dev/null || true
sleep 0.6
export PYTHONPATH=/home/li/.local/share/makcm-ota/go1-left-gen15:/home/li/.local/share/makcm-ota/tools
/usr/bin/python3 - <<'PY'
import serial, time, json
from makcm_ota_tool import (
    SerialTransport, OtaClient, TARGET_LEFT, TARGET_RIGHT,
    decode_status_payload, SPEED_SWITCH, CMD_ENTER_MAINTENANCE, OtaNack,
)

PORT = '/dev/ttyACM0'

def drain(s, secs=0.8):
    end = time.time() + secs
    b = bytearray()
    while time.time() < end:
        c = s.read(4096)
        if c:
            b.extend(c)
    return b

def text_cmd(s, cmd, wait=1.2):
    s.reset_input_buffer()
    payload = cmd if cmd.endswith(b'\n') else cmd + b'\n'
    s.write(payload)
    s.flush()
    return drain(s, wait)

out = {'phases': []}

s = serial.Serial(PORT, 115200, timeout=0.05)
s.write(SPEED_SWITCH)
s.flush()
time.sleep(0.15)
s.close()
s = serial.Serial(PORT, 4_000_000, timeout=0.05)
time.sleep(0.1)
bootish = drain(s, 0.4)
out['boot_noise'] = bootish.decode('utf-8', 'replace')[:400]

for cmd in (b'LEFT_STATUS', b'READY', b'km.version()'):
    wait = 1.5 if cmd == b'READY' else 1.0
    rx = text_cmd(s, cmd, wait)
    text = rx.decode('utf-8', 'replace')
    out['phases'].append({
        'cmd': cmd.decode(),
        'rx_len': len(rx),
        'rx': text[:800],
        'has_RIGHT': ('RIGHT:' in text),
        'has_LEFT_STATUS': ('LEFT_STATUS' in text),
    })
s.close()

try:
    t = SerialTransport(PORT, baudrate=4_000_000, timeout=0.05)
    left = OtaClient(t, target=TARGET_LEFT, timeout=3.0, retries=2)
    st = decode_status_payload(left.hello().payload)
    out['left_hello'] = {
        'ok': True,
        'state': st.state,
        'active': st.active_generation,
        'target': st.target_generation,
        'flags': st.status_flags,
        'local_valid': bool(st.status_flags & 0x80),
        'maint': bool(st.status_flags & 0x20),
    }
    right = OtaClient(t, target=TARGET_RIGHT, timeout=8.0, retries=1)
    try:
        frame = right.request(CMD_ENTER_MAINTENANCE)
        rst = decode_status_payload(frame.payload)
        out['right_enter'] = {
            'ok': True,
            'state': rst.state,
            'active': rst.active_generation,
            'target': rst.target_generation,
            'flags': rst.status_flags,
            'tx': right.transaction_id,
        }
        try:
            ab = decode_status_payload(right.abort(allow_ota=True).payload)
            out['right_abort'] = {
                'ok': True,
                'state': ab.state,
                'active': ab.active_generation,
                'target': ab.target_generation,
                'flags': ab.status_flags,
            }
        except Exception as e:
            out['right_abort'] = {
                'ok': False,
                'error': type(e).__name__ + ': ' + str(e),
            }
    except OtaNack as e:
        out['right_enter'] = {'ok': False, 'nack': str(e)}
    except Exception as e:
        out['right_enter'] = {
            'ok': False,
            'error': type(e).__name__ + ': ' + str(e),
        }
    t.close()
except Exception as e:
    out['left_hello'] = {
        'ok': False,
        'error': type(e).__name__ + ': ' + str(e),
    }

print(json.dumps(out, ensure_ascii=False, indent=2))
PY
"""


def main() -> int:
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(
        HOST,
        username="li",
        password=PASSWORD,
        look_for_keys=False,
        allow_agent=False,
        timeout=10,
    )
    try:
        _, o, e = client.exec_command(REMOTE, timeout=60)
        sys.stdout.write(o.read().decode("utf-8", "replace"))
        err = e.read().decode("utf-8", "replace")
        if err.strip():
            sys.stderr.write("STDERR:\n" + err)
        return o.channel.recv_exit_status()
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
