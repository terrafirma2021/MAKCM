#!/usr/bin/env python3
"""Recovery: erase left NVS via CH343 download mode, then OTA gen16.

Board is Fault (validated PendingConfirm after power-cycle on buggy begin).
Protocol Abort returns flash_write_failed; Enter is invalid_state.
Erasing NVS clears the V1 record so next boot is Idle on VALID gen15,
then normal Prepare/Activate can install gen16.

Requires download mode: hold BOOT (GPIO0), then pulse RESET/EN (or
re-plug power while holding BOOT).
"""
from __future__ import annotations

import json
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

import paramiko

HOST = "192.168.137.2"
PASSWORD = "1234567"
PACKAGE_SHA = "5354c9966e438e112eb5106eaa20338dc3ac456d2c96220fe24feb2b3cf13770"
IMAGE_SHA = "c94f56cf2dcbe11e98bef97a430eebcf701098cf1af80a7031a0e96a38273bee"

INSTALL_AND_ERASE = r'''
set -e
python3 -m pip install --user -q esptool 2>/dev/null || pip3 install --user -q esptool
export PATH="$HOME/.local/bin:$PATH"
fuser -k /dev/ttyACM0 2>/dev/null || true
sleep 1
echo "=== trying download-mode chip_id (hold BOOT) ==="
# default_reset pulses DTR/RTS; board may need BOOT held
python3 -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
  --before default_reset --after hard_reset chip_id
echo "=== erase NVS @ 0x9000 len 0x6000 ==="
python3 -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
  --before default_reset --after hard_reset erase_region 0x9000 0x6000
echo "=== erase done ==="
'''

PROBE = r'''
cd /home/li/.local/share/makcm-ota/go1-left-gen16
python3 - <<'PY'
import json, time
from makcm_ota_tool import SerialTransport, OtaClient, TARGET_LEFT, decode_status_payload
time.sleep(2)
t=SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
try:
  c=OtaClient(t, target=TARGET_LEFT, timeout=5.0, retries=3)
  st=decode_status_payload(c.hello().payload)
  print(json.dumps({'ok':True,'state':st.state,'active':st.active_generation,
                    'target':st.target_generation,'flags':st.status_flags,
                    'local_valid':bool(st.status_flags&0x80),
                    'tx':c.transaction_id}))
finally:
  t.close()
PY
'''

OTA = r'''
set -e
cd /home/li/.local/share/makcm-ota/go1-left-gen16
fuser -k /dev/ttyACM0 2>/dev/null || true
sleep 1
python3 - <<'PY'
import hashlib, json, time
from makcm_ota_tool import SerialTransport, OtaClient, TARGET_LEFT, decode_status_payload, OtaNack

package_sha = '5354c9966e438e112eb5106eaa20338dc3ac456d2c96220fe24feb2b3cf13770'
image_sha = 'c94f56cf2dcbe11e98bef97a430eebcf701098cf1af80a7031a0e96a38273bee'

def emit(phase, **kw):
    print(json.dumps({'phase': phase, **kw}, ensure_ascii=False), flush=True)

package = open('left-gen16.mkota','rb').read()
assert hashlib.sha256(package).hexdigest() == package_sha
emit('package_ok', package_sha=package_sha)

t = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
c = OtaClient(t, target=TARGET_LEFT, timeout=8.0, retries=3)
try:
    before = decode_status_payload(c.hello().payload)
    emit('before', state=before.state, active=before.active_generation,
         target=before.target_generation, flags=before.status_flags,
         local=bool(before.status_flags & 0x80), tx=c.transaction_id)
    if before.state != 0 or before.target_generation != 0:
        try:
            ab = c.abort(allow_ota=True, preflight_status=False)
            a = decode_status_payload(ab.payload)
            emit('abort', state=a.state, flags=a.status_flags)
        except Exception as e:
            emit('abort_err', error=str(e))
            # fresh client
            t.close()
            time.sleep(0.5)
            t = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
            c = OtaClient(t, target=TARGET_LEFT, timeout=8.0, retries=3)
            before = decode_status_payload(c.hello().payload)
            emit('rehello', state=before.state, active=before.active_generation,
                 flags=before.status_flags, tx=c.transaction_id)
            if before.state != 0:
                raise SystemExit('still not idle')

    sticky = c.transaction_id
    emit('prepare_start', tx=sticky)
    c.prepare(package, allow_ota=True, expected_package_sha256=package_sha)
    after = decode_status_payload(c.status(v2=False).payload)
    sticky = c.transaction_id
    emit('after_prepare', state=after.state, active=after.active_generation,
         target=after.target_generation, flags=after.status_flags, tx=sticky)
    try:
        act = c.activate(allow_activate=True, preflight_status=False)
        a = decode_status_payload(act.payload)
        emit('activate_ack', state=a.state, flags=a.status_flags)
    except Exception as e:
        emit('activate_ambiguous', error=type(e).__name__+': '+str(e))
    t.close()
    time.sleep(5)
    for i in range(25):
        t2 = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
        try:
            for label, tx in (('sticky', sticky), ('fresh', None)):
                try:
                    kw = {} if tx is None else {'transaction_id': tx}
                    c2 = OtaClient(t2, target=TARGET_LEFT, timeout=3.0, retries=1, **kw)
                    st = decode_status_payload(c2.hello().payload)
                    row = {'poll': i, 'label': label, 'state': st.state,
                           'active': st.active_generation, 'target': st.target_generation,
                           'flags': st.status_flags,
                           'local_valid': bool(st.status_flags & 0x80)}
                    emit('poll', **row)
                    if st.active_generation == 16 and (st.status_flags & 0x80):
                        emit('GO1_GEN16_PASS', **row)
                        raise SystemExit(0)
                    break
                except SystemExit:
                    raise
                except Exception as e:
                    emit('probe_err', poll=i, label=label, error=str(e))
        finally:
            t2.close()
        time.sleep(1.5)
    emit('INCONCLUSIVE')
    raise SystemExit(2)
finally:
    try:
        t.close()
    except Exception:
        pass
PY
'''


def ssh(cmd: str, timeout: int = 120) -> tuple[int, str, str]:
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(HOST, username='li', password=PASSWORD,
                   look_for_keys=False, allow_agent=False, timeout=10)
    try:
        _, o, e = client.exec_command(cmd, timeout=timeout)
        # stream
        out_chunks = []
        while True:
            line = o.readline()
            if not line:
                break
            sys.stdout.write(line)
            sys.stdout.flush()
            out_chunks.append(line)
        err = e.read().decode('utf-8', 'replace')
        code = o.channel.recv_exit_status()
        return code, ''.join(out_chunks), err
    finally:
        client.close()


def main() -> int:
    print('STEP1: erase NVS (hold BOOT on left board now, keep held until chip_id succeeds)')
    code, out, err = ssh(INSTALL_AND_ERASE, timeout=180)
    if err.strip():
        sys.stderr.write(err)
    if code != 0:
        print('ERASE_FAILED — hold BOOT + RESET, then re-run')
        return code

    print('STEP2: probe after erase/reset')
    time.sleep(3)
    code, out, err = ssh(PROBE, timeout=40)
    print(out)
    if err.strip():
        sys.stderr.write(err)

    print('STEP3: gen16 Prepare+Activate')
    code, out, err = ssh(OTA, timeout=900)
    if err.strip():
        sys.stderr.write(err[-2000:])
    verdict = 'PASS' if 'GO1_GEN16_PASS' in out else 'FAIL'
    stamp = datetime.now(timezone.utc).strftime('%Y%m%d_%H%M%S')
    path = Path(__file__).resolve().parent / 'go1_live_readonly' / f'go1_nvs_erase_gen16_{stamp}.jsonl'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(out, encoding='utf-8')
    print(f'SUMMARY verdict={verdict} exit={code} saved={path}')
    return 0 if verdict == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
