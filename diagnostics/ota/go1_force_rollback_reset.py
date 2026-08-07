#!/usr/bin/env python3
"""Force CH343 DTR reset so PENDING_VERIFY gen15 rolls back to gen5."""
from __future__ import annotations

import json
import sys
import time

import paramiko

REMOTE = r'''
set +e
fuser -k /dev/ttyACM0 2>/dev/null
sleep 1
python3 - <<'PY'
import json, time, subprocess
import serial
from pathlib import Path

def emit(**kw):
    print(json.dumps(kw, ensure_ascii=False), flush=True)

# Aggressive DTR/RTS reset pulse
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.2)
for _ in range(3):
    s.setDTR(False); s.setRTS(True); time.sleep(0.1)
    s.setDTR(True); s.setRTS(False); time.sleep(0.1)
    s.setDTR(False); s.setRTS(False); time.sleep(0.2)
s.close()
emit(phase='reset_pulses_done')
time.sleep(1)
subprocess.run(['fuser','-k','/dev/ttyACM0'], capture_output=True)
time.sleep(5)

import sys
sys.path.insert(0, '/home/li/.local/share/makcm-ota/go1-left-gen15')
from makcm_ota_tool import SerialTransport, OtaClient, TARGET_LEFT, decode_status_payload

for i in range(20):
    try:
        t = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
        try:
            # Prefer old tx first (gen15 fault), then fresh (gen5 idle)
            for label, tx in (('old', 70811770), ('fresh', None)):
                try:
                    kwargs = {} if tx is None else {'transaction_id': tx}
                    c = OtaClient(t, target=TARGET_LEFT, timeout=3.0, retries=1, **kwargs)
                    st = decode_status_payload(c.hello().payload)
                    emit(phase='poll', poll=i, label=label, state=st.state,
                         active=st.active_generation, target=st.target_generation,
                         flags=st.status_flags, local_valid=bool(st.status_flags & 0x80))
                    if st.active_generation == 5:
                        emit(phase='ROLLBACK_TO_GEN5')
                        raise SystemExit(0)
                    if st.active_generation == 15 and (st.status_flags & 0x80):
                        emit(phase='GEN15_VALID')
                        raise SystemExit(0)
                    break
                except SystemExit:
                    raise
                except Exception as e:
                    emit(phase='probe_err', poll=i, label=label, error=type(e).__name__+': '+str(e))
        finally:
            t.close()
    except SystemExit:
        raise
    except Exception as e:
        emit(phase='open_err', poll=i, error=type(e).__name__+': '+str(e))
    time.sleep(1.5)
emit(phase='STILL_PENDING')
raise SystemExit(2)
PY
'''


def main() -> int:
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect('192.168.137.2', username='li', password='1234567',
                   look_for_keys=False, allow_agent=False, timeout=10)
    try:
        _i, o, e = client.exec_command(REMOTE, timeout=120)
        while True:
            line = o.readline()
            if not line:
                break
            sys.stdout.write(line)
            sys.stdout.flush()
        err = e.read().decode()
        if err.strip():
            sys.stderr.write(err)
        return o.channel.recv_exit_status()
    finally:
        client.close()


if __name__ == '__main__':
    raise SystemExit(main())
