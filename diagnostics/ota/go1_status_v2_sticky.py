#!/usr/bin/env python3
"""Read left Status V1+V2 with sticky tx after failed mark-valid."""
from __future__ import annotations

import json
import sys

import paramiko

REMOTE = r'''
cd /home/li/.local/share/makcm-ota/go1-left-gen15
python3 - <<'PY'
import json
from makcm_ota_tool import (
    SerialTransport, OtaClient, TARGET_LEFT, decode_status_payload
)

TX = 630861179

def emit(phase, **kw):
    print(json.dumps({'phase': phase, **kw}, ensure_ascii=False, default=str), flush=True)

t = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
try:
    c = OtaClient(t, target=TARGET_LEFT, timeout=5.0, retries=2, transaction_id=TX)
    v1 = decode_status_payload(c.hello().payload)
    emit('v1', state=v1.state, active=v1.active_generation, target=v1.target_generation,
         flags=v1.status_flags, local_valid=bool(v1.status_flags & 0x80),
         reason=getattr(v1, 'reason', None),
         next_offset=getattr(v1, 'next_offset', None))
    # Prefer raw status v2 if available
    try:
        frame = c.status(v2=True)
        # Try structured decode first
        try:
            v2 = decode_status_payload(frame.payload)
            d = getattr(v2, '__dict__', None) or {}
            emit('v2_decoded', **{k: d[k] for k in d})
        except Exception as e:
            emit('v2_decode_err', error=type(e).__name__+': '+str(e),
                 payload_hex=frame.payload.hex() if frame.payload else None,
                 payload_len=len(frame.payload) if frame.payload else 0)
    except Exception as e:
        emit('v2_err', error=type(e).__name__+': '+str(e))
finally:
    t.close()
PY
'''

def main() -> int:
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect('192.168.137.2', username='li', password='1234567',
                   look_for_keys=False, allow_agent=False, timeout=10)
    try:
        _, o, e = client.exec_command(REMOTE, timeout=60)
        sys.stdout.write(o.read().decode())
        err = e.read().decode()
        if err.strip():
            sys.stderr.write(err)
        return o.channel.recv_exit_status()
    finally:
        client.close()

if __name__ == '__main__':
    raise SystemExit(main())
