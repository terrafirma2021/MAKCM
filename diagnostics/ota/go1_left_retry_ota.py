#!/usr/bin/env python3
"""Abort leftover Receiving/Prepared on left, then Prepare+Activate once.

Do NOT probe with fuser-kill while this runs.
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
PACKAGE_SHA = "2f0c1adc6ec16c3de74de3a6f9db78c01b6fbada84572cb900d0c5d64aece856"
IMAGE_SHA = "f29cacc1fc5cd0417aaf32ee8303d9a9f74142aecde962836f9524df2903fd5c"

REMOTE = r'''
set -e
cd /home/li/.local/share/makcm-ota/go1-left-gen15
# Only clear stale holders; do not keep killing during transfer.
fuser -k /dev/ttyACM0 2>/dev/null || true
sleep 1
python3 - <<'PY'
import hashlib, json, time, traceback
from makcm_ota_tool import (
    SerialTransport, OtaClient, TARGET_LEFT, decode_status_payload, OtaNack
)

package_path = 'left-gen15.mkota'
package_sha = '2f0c1adc6ec16c3de74de3a6f9db78c01b6fbada84572cb900d0c5d64aece856'
image_sha = 'f29cacc1fc5cd0417aaf32ee8303d9a9f74142aecde962836f9524df2903fd5c'
CANDIDATE_TX = [630861179, 863439654, 70811770]

def emit(phase, **kwargs):
    print(json.dumps({'phase': phase, **kwargs}, ensure_ascii=False), flush=True)

with open(package_path, 'rb') as f:
    package = f.read()
actual = hashlib.sha256(package).hexdigest()
if actual != package_sha:
    raise SystemExit(f'package sha mismatch: {actual}')
emit('package_ok', package_sha=actual, package_bytes=len(package), image_sha=image_sha)

transport = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
client = None
before = None
last_err = None
for tx in [None] + CANDIDATE_TX:
    try:
        kwargs = {} if tx is None else {'transaction_id': tx}
        client = OtaClient(transport, target=TARGET_LEFT, timeout=8.0, retries=2, **kwargs)
        hello = client.hello()
        before = decode_status_payload(hello.payload)
        emit('before', tx_try=tx, state=before.state,
             active_generation=before.active_generation,
             target_generation=before.target_generation,
             status_flags=before.status_flags,
             transaction_id=client.transaction_id)
        break
    except Exception as exc:
        last_err = type(exc).__name__ + ': ' + str(exc)
        emit('before_err', tx_try=tx, error=last_err)
        client = None
if client is None or before is None:
    raise SystemExit('hello failed: ' + str(last_err))

# Clear leftover maintenance / receiving / prepared.
if before.state != 0 or before.target_generation != 0:
    try:
        abort = client.abort(allow_ota=True, preflight_status=False)
        ab = decode_status_payload(abort.payload)
        emit('pre_abort', state=ab.state, active_generation=ab.active_generation,
             target_generation=ab.target_generation, status_flags=ab.status_flags,
             reason=getattr(ab, 'reason', None))
    except OtaNack as exc:
        emit('pre_abort_nack', error=str(exc))
        # Fresh enter after nack may still work from Fault/Idle; continue if Idle-ish
    except Exception as exc:
        emit('pre_abort_err', error=type(exc).__name__ + ': ' + str(exc))

# Re-hello on sticky client after abort.
st2 = decode_status_payload(client.status(v2=False).payload)
emit('after_abort_status', state=st2.state,
     active_generation=st2.active_generation,
     target_generation=st2.target_generation,
     status_flags=st2.status_flags)

# If still not Idle, open a fresh transaction client.
if st2.state != 0 or st2.target_generation != 0:
    transport.close()
    time.sleep(0.5)
    transport = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
    client = OtaClient(transport, target=TARGET_LEFT, timeout=8.0, retries=3)
    hello = client.hello()
    before = decode_status_payload(hello.payload)
    emit('rehello', state=before.state,
         active_generation=before.active_generation,
         target_generation=before.target_generation,
         status_flags=before.status_flags,
         transaction_id=client.transaction_id)
    if before.state != 0 or before.target_generation != 0:
        abort = client.abort(allow_ota=True, preflight_status=False)
        ab = decode_status_payload(abort.payload)
        emit('second_abort', state=ab.state,
             active_generation=ab.active_generation,
             target_generation=ab.target_generation,
             status_flags=ab.status_flags)

sticky_tx = client.transaction_id
emit('prepare_start', transaction_id=sticky_tx)
prep = client.prepare(
    package,
    allow_ota=True,
    expected_package_sha256=package_sha,
)
emit('prepare_ok', result=str(prep))
after_prep = decode_status_payload(client.status(v2=False).payload)
sticky_tx = client.transaction_id
emit('after_prepare',
     state=after_prep.state,
     active_generation=after_prep.active_generation,
     target_generation=after_prep.target_generation,
     status_flags=after_prep.status_flags,
     next_offset=getattr(after_prep, 'next_offset', None),
     transaction_id=sticky_tx)

try:
    act = client.activate(allow_activate=True, preflight_status=False)
    act_status = decode_status_payload(act.payload)
    emit('activate_ack',
         state=act_status.state,
         active_generation=act_status.active_generation,
         target_generation=act_status.target_generation,
         status_flags=act_status.status_flags)
except Exception as exc:
    emit('activate_ack_ambiguous', error_type=type(exc).__name__, error=str(exc))

transport.close()
time.sleep(5.0)

outcomes = []
for i in range(30):
    try:
        t2 = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
        try:
            # Prefer sticky tx first for PendingConfirm/Fault, then fresh.
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
                    outcomes.append(row)
                    emit('post_activate_poll', **row)
                    if st.active_generation == 15 and (st.status_flags & 0x80):
                        emit('GO1_PASS', **row)
                        raise SystemExit(0)
                    if st.active_generation == 5 and st.state == 0 and i > 3:
                        # Settled rollback idle
                        emit('GO1_FAIL_ROLLBACK', **row)
                        raise SystemExit(3)
                    break
                except SystemExit:
                    raise
                except Exception as exc:
                    emit('post_activate_probe_err', poll=i, label=label,
                         error=type(exc).__name__ + ': ' + str(exc))
        finally:
            t2.close()
    except SystemExit:
        raise
    except Exception as exc:
        emit('post_activate_poll_error', poll=i,
             error_type=type(exc).__name__, error=str(exc))
    time.sleep(1.5)

emit('GO1_INCONCLUSIVE', last=outcomes[-1] if outcomes else None)
raise SystemExit(2)
PY
'''


def main() -> int:
    out_dir = Path(__file__).resolve().parent / "go1_live_readonly"
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    out_path = out_dir / f"go1_left_retry_{stamp}.jsonl"

    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(
        HOST, username="li", password=PASSWORD,
        look_for_keys=False, allow_agent=False, timeout=10,
    )
    try:
        # Long timeout: full image transfer at 4Mbaud still needs headroom.
        _, stdout, stderr = client.exec_command(REMOTE, timeout=900)
        lines = []
        while True:
            line = stdout.readline()
            if not line:
                break
            sys.stdout.write(line)
            sys.stdout.flush()
            lines.append(line)
            out_path.write_text("".join(lines), encoding="utf-8")
        err = stderr.read().decode("utf-8", "replace")
        code = stdout.channel.recv_exit_status()
    finally:
        client.close()

    verdict = "INCONCLUSIVE"
    phases = []
    for line in lines:
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError:
            continue
        phases.append(row)
        if row.get("phase") == "GO1_PASS":
            verdict = "PASS"
        elif row.get("phase") == "GO1_FAIL_ROLLBACK":
            verdict = "FAIL_ROLLBACK"
        elif row.get("phase") == "GO1_INCONCLUSIVE":
            verdict = "INCONCLUSIVE"
    summary = {
        "schema": "makcm.go1-left-retry.v1",
        "captured_utc": datetime.now(timezone.utc).isoformat(),
        "package_sha": PACKAGE_SHA,
        "image_sha": IMAGE_SHA,
        "exit_code": code,
        "verdict": verdict,
        "log_path": str(out_path),
        "stderr_tail": err[-2000:] if err else "",
        "phases": phases,
    }
    summary_path = out_dir / f"go1_left_retry_{stamp}.summary.json"
    summary_path.write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n",
                            encoding="utf-8")
    print(f"\nSUMMARY verdict={verdict} exit={code}")
    print(f"saved {out_path}")
    print(f"saved {summary_path}")
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
