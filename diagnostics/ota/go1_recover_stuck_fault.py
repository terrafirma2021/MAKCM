#!/usr/bin/env python3
"""Recover stuck gen15 Fault board: abort with old tx, aggressive RTS/DTR, poll."""
from __future__ import annotations

import json
import sys

import paramiko

REMOTE = r'''
set +e
fuser -k /dev/ttyACM0 2>/dev/null
sleep 1
cd /home/li/.local/share/makcm-ota/go1-left-gen15
python3 - <<'PY'
import json, time, subprocess
import serial
from makcm_ota_tool import (
    SerialTransport, OtaClient, TARGET_LEFT, decode_status_payload, OtaNack
)

def emit(**kw):
    print(json.dumps(kw, ensure_ascii=False), flush=True)

TX = 70811770

# 1) Status + Abort with sticky transaction
t = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
try:
    c = OtaClient(t, target=TARGET_LEFT, timeout=5.0, retries=2, transaction_id=TX)
    st = decode_status_payload(c.hello().payload)
    emit(phase='before', state=st.state, active=st.active_generation,
         target=st.target_generation, flags=st.status_flags,
         local_valid=bool(st.status_flags & 0x80), reason=getattr(st, 'reason', None))
    try:
        abort = c.abort(allow_ota=True)
        ab = decode_status_payload(abort.payload)
        emit(phase='abort_ack', state=ab.state, active=ab.active_generation,
             target=ab.target_generation, flags=ab.status_flags,
             reason=getattr(ab, 'reason', None))
    except OtaNack as e:
        emit(phase='abort_nack', error=str(e),
             status=getattr(e, 'status', None) and {
                 'state': e.status.state if e.status else None,
                 'reason': getattr(e.status, 'reason', None) if e.status else None,
             })
    except Exception as e:
        emit(phase='abort_err', error=type(e).__name__ + ': ' + str(e))
finally:
    t.close()

# 2) Hard serial reset pulses (DTR+RTS permutations)
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.2)
patterns = [
    (False, True), (True, False), (False, False), (True, True),
]
for i in range(6):
    for dtr, rts in patterns:
        s.setDTR(dtr); s.setRTS(rts); time.sleep(0.05)
    s.setDTR(False); s.setRTS(False); time.sleep(0.15)
s.close()
emit(phase='reset_pulses_done')
subprocess.run(['fuser', '-k', '/dev/ttyACM0'], capture_output=True)
time.sleep(6)

# 3) Poll both txs
for i in range(15):
    for label, tx in (('old', TX), ('fresh', None)):
        try:
            t2 = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
            try:
                kwargs = {} if tx is None else {'transaction_id': tx}
                c2 = OtaClient(t2, target=TARGET_LEFT, timeout=3.0, retries=1, **kwargs)
                st = decode_status_payload(c2.hello().payload)
                emit(phase='poll', poll=i, label=label, state=st.state,
                     active=st.active_generation, target=st.target_generation,
                     flags=st.status_flags, local_valid=bool(st.status_flags & 0x80))
                if st.active_generation == 5 and st.state == 0:
                    emit(phase='RECOVERED_GEN5')
                    raise SystemExit(0)
                if st.active_generation == 15 and (st.status_flags & 0x80):
                    emit(phase='GEN15_VALID')
                    raise SystemExit(0)
                break
            finally:
                t2.close()
        except SystemExit:
            raise
        except Exception as e:
            emit(phase='poll_err', poll=i, label=label,
                 error=type(e).__name__ + ': ' + str(e))
    time.sleep(1.2)

emit(phase='STILL_STUCK')
raise SystemExit(2)
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
        _, o, e = client.exec_command(REMOTE, timeout=120)
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


if __name__ == "__main__":
    raise SystemExit(main())
