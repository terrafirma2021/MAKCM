#!/usr/bin/env python3
"""GO-1 gen16: wait until board leaves validated PendingConfirm, then OTA.

Current gen15 (wrong begin intercept) cannot Enter/Abort while
PendingConfirm+local_valid. Power-cycle first: buggy begin → Fault, then
Abort→Idle→Prepare→Activate gen16 (fixed begin). Do not fuser-kill mid-transfer.
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
STICKY_TXS = [2721386941, 630861179, 863439654, 70811770]

REMOTE = r'''
set -e
cd /home/li/.local/share/makcm-ota/go1-left-gen16
fuser -k /dev/ttyACM0 2>/dev/null || true
sleep 1
python3 - <<'PY'
import hashlib, json, time
from makcm_ota_tool import (
    SerialTransport, OtaClient, TARGET_LEFT, decode_status_payload, OtaNack,
)

package_path = 'left-gen16.mkota'
package_sha = '__PACKAGE_SHA__'
image_sha = '__IMAGE_SHA__'
STICKY = [2721386941, 630861179, 863439654, 70811770]

def emit(phase, **kw):
    print(json.dumps({'phase': phase, **kw}, ensure_ascii=False), flush=True)

with open(package_path, 'rb') as f:
    package = f.read()
actual = hashlib.sha256(package).hexdigest()
if actual != package_sha:
    raise SystemExit(f'package sha mismatch: {actual}')
emit('package_ok', package_sha=actual, image_sha=image_sha, package_bytes=len(package))

transport = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
client = None
before = None
last_err = None
for tx in STICKY + [None]:
    try:
        kwargs = {} if tx is None else {'transaction_id': tx}
        client = OtaClient(transport, target=TARGET_LEFT, timeout=8.0, retries=2, **kwargs)
        before = decode_status_payload(client.hello().payload)
        emit('before', tx_try=tx, state=before.state,
             active_generation=before.active_generation,
             target_generation=before.target_generation,
             status_flags=before.status_flags,
             local_valid=bool(before.status_flags & 0x80),
             transaction_id=client.transaction_id)
        break
    except Exception as exc:
        last_err = type(exc).__name__ + ': ' + str(exc)
        emit('before_err', tx_try=tx, error=last_err)
        client = None
if client is None or before is None:
    raise SystemExit('hello failed: ' + str(last_err))

# Validated PendingConfirm cannot Abort/Enter — host must power-cycle first.
if before.state == 5 and (before.status_flags & 0x80):
    emit('NEED_POWER_CYCLE', reason='validated_pending_confirm_blocks_enter_abort')
    raise SystemExit(10)

# Fault / leftover maintenance → Abort to Idle.
if before.state != 0 or before.target_generation != 0:
    try:
        abort = client.abort(allow_ota=True, preflight_status=False)
        ab = decode_status_payload(abort.payload)
        emit('pre_abort', state=ab.state, active_generation=ab.active_generation,
             target_generation=ab.target_generation, status_flags=ab.status_flags,
             reason=getattr(ab, 'reason', None))
    except OtaNack as exc:
        emit('pre_abort_nack', error=str(exc))
    except Exception as exc:
        emit('pre_abort_err', error=type(exc).__name__ + ': ' + str(exc))

st2 = decode_status_payload(client.status(v2=False).payload)
emit('after_abort_status', state=st2.state,
     active_generation=st2.active_generation,
     target_generation=st2.target_generation,
     status_flags=st2.status_flags)

if st2.state != 0 or st2.target_generation != 0:
    transport.close()
    time.sleep(0.5)
    transport = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
    client = OtaClient(transport, target=TARGET_LEFT, timeout=8.0, retries=3)
    before = decode_status_payload(client.hello().payload)
    emit('rehello', state=before.state, active_generation=before.active_generation,
         target_generation=before.target_generation, status_flags=before.status_flags,
         transaction_id=client.transaction_id)
    if before.state != 0 or before.target_generation != 0:
        abort = client.abort(allow_ota=True, preflight_status=False)
        ab = decode_status_payload(abort.payload)
        emit('second_abort', state=ab.state, active_generation=ab.active_generation,
             target_generation=ab.target_generation, status_flags=ab.status_flags)

sticky_tx = client.transaction_id
emit('prepare_start', transaction_id=sticky_tx)
prep = client.prepare(package, allow_ota=True, expected_package_sha256=package_sha)
emit('prepare_ok', result=str(prep))
after_prep = decode_status_payload(client.status(v2=False).payload)
sticky_tx = client.transaction_id
emit('after_prepare', state=after_prep.state,
     active_generation=after_prep.active_generation,
     target_generation=after_prep.target_generation,
     status_flags=after_prep.status_flags,
     next_offset=getattr(after_prep, 'next_offset', None),
     transaction_id=sticky_tx)

try:
    act = client.activate(allow_activate=True, preflight_status=False)
    act_status = decode_status_payload(act.payload)
    emit('activate_ack', state=act_status.state,
         active_generation=act_status.active_generation,
         target_generation=act_status.target_generation,
         status_flags=act_status.status_flags)
except Exception as exc:
    emit('activate_ack_ambiguous', error_type=type(exc).__name__, error=str(exc))

transport.close()
time.sleep(5.0)

for i in range(30):
    try:
        t2 = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
        try:
            for label, tx in (('sticky', sticky_tx), ('fresh', None)):
                try:
                    kwargs = {} if tx is None else {'transaction_id': tx}
                    c2 = OtaClient(t2, target=TARGET_LEFT, timeout=3.0, retries=1, **kwargs)
                    st = decode_status_payload(c2.hello().payload)
                    row = {
                        'poll': i, 'label': label, 'state': st.state,
                        'active_generation': st.active_generation,
                        'target_generation': st.target_generation,
                        'status_flags': st.status_flags,
                        'local_boot_validated': bool(st.status_flags & 0x80),
                    }
                    emit('post_activate_poll', **row)
                    if st.active_generation == 16 and (st.status_flags & 0x80):
                        emit('GO1_GEN16_PASS', **row)
                        raise SystemExit(0)
                    if st.active_generation == 15 and st.state == 0 and i > 3:
                        emit('GO1_GEN16_FAIL_ROLLBACK', **row)
                        raise SystemExit(3)
                    break
                except SystemExit:
                    raise
                except Exception as exc:
                    emit('probe_err', poll=i, label=label,
                         error=type(exc).__name__ + ': ' + str(exc))
        finally:
            t2.close()
    except SystemExit:
        raise
    except Exception as exc:
        emit('poll_err', poll=i, error=type(exc).__name__ + ': ' + str(exc))
    time.sleep(1.5)

emit('GO1_GEN16_INCONCLUSIVE')
raise SystemExit(2)
PY
'''.replace('__PACKAGE_SHA__', PACKAGE_SHA).replace('__IMAGE_SHA__', IMAGE_SHA)


def probe_once() -> dict:
    remote = r'''
cd /home/li/.local/share/makcm-ota/go1-left-gen16 2>/dev/null || cd /home/li/.local/share/makcm-ota/go1-left-gen15
python3 - <<'PY'
import json
from makcm_ota_tool import SerialTransport, OtaClient, TARGET_LEFT, decode_status_payload
TXS=[2721386941,630861179,863439654,70811770,None]
t=SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
try:
  for tx in TXS:
    try:
      kwargs={} if tx is None else {'transaction_id': tx}
      c=OtaClient(t, target=TARGET_LEFT, timeout=3.0, retries=1, **kwargs)
      st=decode_status_payload(c.hello().payload)
      print(json.dumps({'ok':True,'tx':tx,'state':st.state,'active':st.active_generation,
                        'target':st.target_generation,'flags':st.status_flags,
                        'local_valid':bool(st.status_flags&0x80)}))
      raise SystemExit(0)
    except SystemExit:
      raise
    except Exception as e:
      print(json.dumps({'ok':False,'tx':tx,'error':type(e).__name__+': '+str(e)}))
finally:
  t.close()
raise SystemExit(1)
PY
'''
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(HOST, username='li', password=PASSWORD,
                   look_for_keys=False, allow_agent=False, timeout=10)
    try:
        _, o, e = client.exec_command(remote, timeout=40)
        out = o.read().decode()
        rows = []
        for line in out.splitlines():
            if line.startswith('{'):
                try:
                    rows.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
        return {'rows': rows, 'stdout': out, 'exit': o.channel.recv_exit_status()}
    finally:
        client.close()


def ready_for_ota(rows: list[dict]) -> bool:
    for row in rows:
        if not row.get('ok'):
            continue
        # Fault (7) or Idle (0) — not validated PendingConfirm
        if row.get('state') == 5 and row.get('local_valid'):
            return False
        if row.get('state') in (0, 7) or (
            row.get('active') in (5, 15) and not row.get('local_valid')
        ):
            return True
    return False


def run_ota() -> int:
    if 'REPLACE' in PACKAGE_SHA:
        raise SystemExit('PACKAGE_SHA not injected')
    out_dir = Path(__file__).resolve().parent / 'go1_live_readonly'
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime('%Y%m%d_%H%M%S')
    out_path = out_dir / f'go1_left_gen16_{stamp}.jsonl'

    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(HOST, username='li', password=PASSWORD,
                   look_for_keys=False, allow_agent=False, timeout=10)
    try:
        _, stdout, stderr = client.exec_command(REMOTE, timeout=900)
        lines = []
        while True:
            line = stdout.readline()
            if not line:
                break
            sys.stdout.write(line)
            sys.stdout.flush()
            lines.append(line)
            out_path.write_text(''.join(lines), encoding='utf-8')
        err = stderr.read().decode('utf-8', 'replace')
        code = stdout.channel.recv_exit_status()
    finally:
        client.close()

    verdict = 'INCONCLUSIVE'
    for line in lines:
        if '"GO1_GEN16_PASS"' in line:
            verdict = 'PASS'
        elif '"GO1_GEN16_FAIL_ROLLBACK"' in line:
            verdict = 'FAIL_ROLLBACK'
        elif '"NEED_POWER_CYCLE"' in line:
            verdict = 'NEED_POWER_CYCLE'
    print(f'\nSUMMARY verdict={verdict} exit={code}')
    print(f'saved {out_path}')
    if err.strip():
        print(err[-1500:])
    return 0 if verdict == 'PASS' else (10 if verdict == 'NEED_POWER_CYCLE' else 1)


def main() -> int:
    print('Waiting for power-cycle out of validated PendingConfirm '
          '(Abort/Enter blocked until then)...', flush=True)
    deadline = time.time() + 30 * 60
    poll = 0
    while time.time() < deadline:
        result = probe_once()
        summary = result['rows'][-1] if result['rows'] else {'ok': False}
        print(f'poll={poll} {summary}', flush=True)
        if ready_for_ota(result['rows']):
            print('RECOVERED — launching gen16 OTA', flush=True)
            return run_ota()
        # Still validated PendingConfirm
        if any(r.get('ok') and r.get('state') == 5 and r.get('local_valid')
               for r in result['rows']):
            print('STILL validated PendingConfirm — power-cycle LEFT board now',
                  flush=True)
        poll += 1
        time.sleep(3.0)
    print('TIMEOUT waiting for power-cycle', flush=True)
    return 2


if __name__ == '__main__':
    raise SystemExit(main())
