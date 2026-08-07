#!/usr/bin/env python3
"""Try Abort then Enter from sticky PendingConfirm (expect gates)."""
from __future__ import annotations

import json
import sys

import paramiko

REMOTE = r'''
cd /home/li/.local/share/makcm-ota/go1-left-gen15
python3 - <<'PY'
import json
from makcm_ota_tool import (
    SerialTransport, OtaClient, TARGET_LEFT, decode_status_payload, OtaNack,
    CMD_ENTER_MAINTENANCE,
)

TX = 2721386941

def emit(**kw):
    print(json.dumps(kw, ensure_ascii=False), flush=True)

t = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
try:
    c = OtaClient(t, target=TARGET_LEFT, timeout=5.0, retries=2, transaction_id=TX)
    st = decode_status_payload(c.hello().payload)
    emit(phase='hello', state=st.state, active=st.active_generation,
         flags=st.status_flags, local=bool(st.status_flags & 0x80))
    try:
        ab = c.abort(allow_ota=True, preflight_status=False)
        a = decode_status_payload(ab.payload)
        emit(phase='abort_ok', state=a.state, flags=a.status_flags,
             reason=getattr(a, 'reason', None))
    except OtaNack as e:
        emit(phase='abort_nack', error=str(e))
    except Exception as e:
        emit(phase='abort_err', error=type(e).__name__ + ': ' + str(e))
    try:
        frame = c.request(CMD_ENTER_MAINTENANCE)
        emit(phase='enter_unexpected_ok', cmd=frame.command)
    except OtaNack as e:
        emit(phase='enter_nack', error=str(e))
    except Exception as e:
        emit(phase='enter_err', error=type(e).__name__ + ': ' + str(e))
finally:
    t.close()
PY
'''


def main() -> int:
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(
        "192.168.137.2",
        username="li",
        password="1234567",
        look_for_keys=False,
        allow_agent=False,
        timeout=10,
    )
    try:
        _, o, e = client.exec_command(REMOTE, timeout=40)
        sys.stdout.write(o.read().decode())
        err = e.read().decode()
        if err.strip():
            sys.stderr.write(err)
        return o.channel.recv_exit_status()
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
