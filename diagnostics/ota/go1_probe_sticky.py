#!/usr/bin/env python3
"""Probe left with known sticky txs then fresh; no fuser-kill mid-session."""
from __future__ import annotations

import json
import sys

import paramiko

REMOTE = r'''
cd /home/li/.local/share/makcm-ota/go1-left-gen15
python3 - <<'PY'
import json
from makcm_ota_tool import SerialTransport, OtaClient, TARGET_LEFT, decode_status_payload

def emit(**kw):
    print(json.dumps(kw, ensure_ascii=False), flush=True)

TXS = [2721386941, 630861179, 863439654, 70811770, None]
t = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
try:
    for tx in TXS:
        try:
            kwargs = {} if tx is None else {'transaction_id': tx}
            c = OtaClient(t, target=TARGET_LEFT, timeout=3.0, retries=1, **kwargs)
            st = decode_status_payload(c.hello().payload)
            emit(ok=True, tx=tx, state=st.state, active=st.active_generation,
                 target=st.target_generation, flags=st.status_flags,
                 local_valid=bool(st.status_flags & 0x80),
                 maint=bool(st.status_flags & 0x20))
            raise SystemExit(0)
        except SystemExit:
            raise
        except Exception as e:
            emit(ok=False, tx=tx, error=type(e).__name__+': '+str(e))
finally:
    t.close()
raise SystemExit(1)
PY
'''

def main() -> int:
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect('192.168.137.2', username='li', password='1234567',
              look_for_keys=False, allow_agent=False, timeout=10)
    try:
        _, o, e = c.exec_command(REMOTE, timeout=40)
        sys.stdout.write(o.read().decode())
        err = e.read().decode()
        if err.strip():
            sys.stderr.write(err)
        return o.channel.recv_exit_status()
    finally:
        c.close()

if __name__ == '__main__':
    raise SystemExit(main())
