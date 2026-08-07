#!/usr/bin/env python3
"""Release CH343 lock, probe left status, optional DTR reset."""
from __future__ import annotations

import json
import sys
import time

import paramiko

HOST = "192.168.137.2"
PASSWORD = "1234567"

REMOTE = r'''
set +e
fuser -k /dev/ttyACM0 2>/dev/null
sleep 2
cd /home/li/.local/share/makcm-ota/go1-left-gen15
python3 - <<'PY'
import json, time
from makcm_ota_tool import SerialTransport, OtaClient, TARGET_LEFT, decode_status_payload

def emit(**kw):
    print(json.dumps(kw, ensure_ascii=False), flush=True)

def probe(tx=None, label='fresh'):
    t = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
    try:
        kwargs = {} if tx is None else {'transaction_id': tx}
        c = OtaClient(t, target=TARGET_LEFT, timeout=5.0, retries=2, **kwargs)
        st = decode_status_payload(c.hello().payload)
        emit(phase='probe', label=label, state=st.state,
             active=st.active_generation, target=st.target_generation,
             flags=st.status_flags, local_valid=bool(st.status_flags & 0x80))
        return st.active_generation, st.state, st.status_flags
    finally:
        t.close()

try:
    active, state, flags = probe(70811770, 'old')
except Exception as e:
    emit(phase='old_err', error=type(e).__name__ + ': ' + str(e))
    try:
        active, state, flags = probe(None, 'fresh')
    except Exception as e2:
        emit(phase='fresh_err', error=type(e2).__name__ + ': ' + str(e2))
        # DTR reset then retry
        import serial
        s = serial.Serial('/dev/ttyACM0', 115200)
        s.dtr = False; time.sleep(0.05); s.dtr = True; time.sleep(0.05); s.dtr = False
        s.close()
        emit(phase='dtr_reset')
        time.sleep(5)
        fuser = __import__('subprocess').run(['fuser','-k','/dev/ttyACM0'], capture_output=True)
        time.sleep(1)
        active, state, flags = probe(None, 'after_reset')

emit(phase='done')
PY
'''


def main() -> int:
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(HOST, username='li', password=PASSWORD,
                   look_for_keys=False, allow_agent=False, timeout=10)
    try:
        _i, o, e = client.exec_command(REMOTE, timeout=90)
        sys.stdout.write(o.read().decode())
        err = e.read().decode()
        if err.strip():
            sys.stderr.write(err)
        return o.channel.recv_exit_status()
    finally:
        client.close()


if __name__ == '__main__':
    raise SystemExit(main())
