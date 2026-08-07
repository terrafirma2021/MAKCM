#!/usr/bin/env python3
"""GO-1 left-only Prepare+Activate via Linux CH343. Never COM3 / right / Confirm."""

from __future__ import annotations

import json
import sys
import time
import traceback
from datetime import datetime, timezone
from pathlib import Path

import paramiko

HOST = "192.168.137.2"
USER = "li"
PASSWORD = "1234567"
PACKAGE_SHA = "2f0c1adc6ec16c3de74de3a6f9db78c01b6fbada84572cb900d0c5d64aece856"
FROZEN_LEFT_APP_SHA = (
    "f29cacc1fc5cd0417aaf32ee8303d9a9f74142aecde962836f9524df2903fd5c"
)

REMOTE = r'''
set -e
cd /home/li/.local/share/makcm-ota/go1-left-gen15
python3 - <<'PY'
import hashlib, json, time, traceback
from makcm_ota_tool import (
    SerialTransport, OtaClient, TARGET_LEFT, decode_status_payload, OtaToolError
)

package_path = 'left-gen15.mkota'
package_sha = '2f0c1adc6ec16c3de74de3a6f9db78c01b6fbada84572cb900d0c5d64aece856'
frozen_app_sha = 'f29cacc1fc5cd0417aaf32ee8303d9a9f74142aecde962836f9524df2903fd5c'

def emit(phase, **kwargs):
    row = {'phase': phase, **kwargs}
    print(json.dumps(row, ensure_ascii=False), flush=True)

with open(package_path, 'rb') as f:
    package = f.read()
actual = hashlib.sha256(package).hexdigest()
if actual != package_sha:
    raise SystemExit(f'package sha mismatch: {actual} != {package_sha}')
emit('package_ok', package_sha=actual, package_bytes=len(package),
     frozen_left_app_sha=frozen_app_sha)

transport = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
client = OtaClient(transport, target=TARGET_LEFT, timeout=8.0, retries=3)
try:
    hello = client.hello()
    before = decode_status_payload(hello.payload)
    emit('before',
         state=before.state,
         active_generation=before.active_generation,
         target_generation=before.target_generation,
         status_flags=before.status_flags,
         transaction_id=client.transaction_id)

    # If leftover maintenance, abort first so Prepare starts clean.
    if before.state != 0 or before.target_generation != 0:
        abort = client.abort(allow_ota=True)
        aborted = decode_status_payload(abort.payload)
        emit('pre_abort',
             state=aborted.state,
             active_generation=aborted.active_generation,
             target_generation=aborted.target_generation,
             status_flags=aborted.status_flags,
             reason=getattr(aborted, 'reason', None))

    prep = client.prepare(
        package,
        allow_ota=True,
        expected_package_sha256=package_sha,
    )
    emit('prepare_ok', result=str(prep))
    after_prep = decode_status_payload(client.status(v2=False).payload)
    emit('after_prepare',
         state=after_prep.state,
         active_generation=after_prep.active_generation,
         target_generation=after_prep.target_generation,
         status_flags=after_prep.status_flags,
         next_offset=getattr(after_prep, 'next_offset', None))

    # Activate — board will reboot; ACK may be lost.
    try:
        # gen5 Status V2 preflight times out; keep sticky tx and skip V2.
        act = client.activate(allow_activate=True, preflight_status=False)
        act_status = decode_status_payload(act.payload)
        emit('activate_ack',
             state=act_status.state,
             active_generation=act_status.active_generation,
             target_generation=act_status.target_generation,
             status_flags=act_status.status_flags)
    except Exception as exc:
        emit('activate_ack_ambiguous',
             error_type=type(exc).__name__,
             error=str(exc))

    # Close and reopen after reboot window.
    transport.close()
    time.sleep(4.0)

    outcomes = []
    for i in range(20):
        try:
            t2 = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
            c2 = OtaClient(t2, target=TARGET_LEFT, timeout=3.0, retries=1)
            try:
                st = decode_status_payload(c2.hello().payload)
                row = {
                    'poll': i,
                    'state': st.state,
                    'active_generation': st.active_generation,
                    'target_generation': st.target_generation,
                    'status_flags': st.status_flags,
                    'maintenance_exclusive': bool(st.status_flags & 0x20),
                    'local_boot_validated': bool(st.status_flags & 0x80),
                }
                outcomes.append(row)
                emit('post_activate_poll', **row)
                # Success: running gen15 and local validated bit, or at least gen15 idle/pendingconfirm
                if st.active_generation == 15 and (st.status_flags & 0x80):
                    emit('GO1_PASS', **row)
                    break
                if st.active_generation == 5 and st.state != 0:
                    emit('GO1_FAIL_ROLLBACK', **row)
                    break
            finally:
                t2.close()
        except Exception as exc:
            emit('post_activate_poll_error', poll=i,
                 error_type=type(exc).__name__, error=str(exc))
        time.sleep(1.5)
    else:
        emit('GO1_INCONCLUSIVE', last=outcomes[-1] if outcomes else None)
except Exception as exc:
    emit('fatal', error_type=type(exc).__name__, error=str(exc),
         traceback=traceback.format_exc())
    raise
PY
'''


def main() -> int:
    out_dir = Path(__file__).resolve().parent / "go1_live_readonly"
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    out_path = out_dir / f"go1_left_prepare_activate_{stamp}.jsonl"

    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(
        HOST, username=USER, password=PASSWORD,
        look_for_keys=False, allow_agent=False, timeout=10,
    )
    try:
        _stdin, stdout, stderr = client.exec_command(REMOTE, timeout=600)
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

    summary = {
        "schema": "makcm.go1-left-prepare-activate.v1",
        "captured_utc": datetime.now(timezone.utc).isoformat(),
        "package_sha": PACKAGE_SHA,
        "frozen_left_app_sha": FROZEN_LEFT_APP_SHA,
        "exit_code": code,
        "log_path": str(out_path),
        "stderr": err,
        "phases": [],
    }
    for line in lines:
        line = line.strip()
        if line.startswith("{"):
            try:
                summary["phases"].append(json.loads(line))
            except json.JSONDecodeError:
                pass
    verdict = "INCONCLUSIVE"
    for p in summary["phases"]:
        if p.get("phase") == "GO1_PASS":
            verdict = "PASS"
        elif p.get("phase") == "GO1_FAIL_ROLLBACK":
            verdict = "FAIL_ROLLBACK"
        elif p.get("phase") == "GO1_INCONCLUSIVE":
            verdict = "INCONCLUSIVE"
        elif p.get("phase") == "fatal":
            verdict = "FATAL"
    summary["verdict"] = verdict
    summary_path = out_dir / f"go1_left_prepare_activate_{stamp}.summary.json"
    summary_path.write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n",
                            encoding="utf-8")
    print(f"\nSUMMARY verdict={verdict} exit={code}")
    print(f"saved {out_path}")
    print(f"saved {summary_path}")
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception:
        traceback.print_exc()
        raise SystemExit(1)
