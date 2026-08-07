#!/usr/bin/env python3
"""Right-only gen16 Prepare→Activate→Confirm via Linux CH343→left→UART1.

Does NOT touch Windows COM3. Does NOT Confirm left. Does NOT pair OTA.

Stops after Confirm returns Idle so the operator can power-cycle and recheck
V2 Confirmed-tombstone restore (restoreFromSnapshotV2).
"""

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
REMOTE_DIR = "/home/li/.local/share/makcm-ota/go1-right-gen16"
PACKAGE_SHA = "31a90668bd4fec0c3e96ddc6c353aa36e799c8a56823d68934b734e8e2a8f0a4"
IMAGE_SHA = "86f8c63f09d26a2ee1c2d2aee6bef605baa99526ffdfd33dd9ad1c31c14e435a"

REMOTE = r'''
set -e
cd __REMOTE_DIR__
export PATH=/usr/bin:/bin:$PATH
export PYTHONPATH="__REMOTE_DIR__:${PYTHONPATH:-}"
/usr/bin/python3 - <<'PY'
import hashlib, json, time, traceback
from makcm_ota_tool import (
    SerialTransport, OtaClient, TARGET_LEFT, TARGET_RIGHT,
    decode_status_payload, OtaToolError,
)

package_path = 'right-gen16.mkota'
package_sha = '__PACKAGE_SHA__'
image_sha = '__IMAGE_SHA__'

def emit(phase, **kwargs):
    print(json.dumps({'phase': phase, **kwargs}, ensure_ascii=False), flush=True)

def flags_bits(flags):
    return {
        'flags': flags,
        'local_valid': bool(flags & 0x80),
        'maint': bool(flags & 0x20),
        'health': bool(flags & 0x10),
        'trust': bool(flags & 0x08),
        'rollback': bool(flags & 0x04),
    }

def summarize(label, st, **extra):
    emit(label,
         state=st.state,
         active=st.active_generation,
         target=st.target_generation,
         reason=getattr(st, 'reason', None),
         **flags_bits(st.status_flags),
         **extra)

with open(package_path, 'rb') as f:
    package = f.read()
actual = hashlib.sha256(package).hexdigest()
if actual != package_sha:
    raise SystemExit(f'package sha mismatch: {actual} != {package_sha}')
emit('package_ok', package_sha=actual, package_bytes=len(package),
     image_sha=image_sha)

transport = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
left = OtaClient(transport, target=TARGET_LEFT, timeout=8.0, retries=3)
right = OtaClient(transport, target=TARGET_RIGHT, timeout=12.0, retries=3,
                  transaction_id=left.transaction_id)
try:
    left_before = decode_status_payload(left.hello().payload)
    summarize('left_before', left_before)
    if left_before.state != 0 or left_before.active_generation != 16:
        raise SystemExit(
            f'left must be Idle gen16 before right OTA; got state={left_before.state} '
            f'active={left_before.active_generation}')
    if left_before.status_flags & 0x20:
        raise SystemExit('left is in maintenance_exclusive; abort/release first')

    # Right HELLO is not a discovery probe.  prepare() for TARGET_RIGHT
    # already starts with ENTER_MAINTENANCE as the only UART1 bootstrap.
    # Do not Enter beforehand — a second Enter while left is already
    # quiesced times out waiting for another OTA_READY.
    # Controller must be unplugged from the right USB host so otaQuiesce
    # can finish teardown.
    prep = right.prepare(
        package,
        allow_ota=True,
        expected_package_sha256=package_sha,
    )
    emit('prepare_ok', result=str(prep))
    after_prep = decode_status_payload(right.status(v2=False).payload)
    summarize('right_after_prepare', after_prep)

    try:
        # Right may reboot before ACK; keep sticky tx and skip V2 preflight.
        act = right.activate(allow_activate=True, preflight_status=False)
        summarize('right_activate_ack', decode_status_payload(act.payload))
    except Exception as exc:
        emit('right_activate_ack_ambiguous',
             error_type=type(exc).__name__, error=str(exc))

    transport.close()
    emit('wait_reboot', seconds=8)
    time.sleep(8)

    transport = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
    left = OtaClient(transport, target=TARGET_LEFT, timeout=8.0, retries=3,
                     transaction_id=right.transaction_id)
    right = OtaClient(transport, target=TARGET_RIGHT, timeout=12.0, retries=4,
                      transaction_id=right.transaction_id)

    # Re-bootstrap left then poll right until active=16 + local_valid.
    summarize('left_after_reboot', decode_status_payload(left.hello().payload))
    deadline = time.time() + 45
    post = None
    while time.time() < deadline:
        try:
            post = decode_status_payload(right.hello().payload)
            summarize('right_after_reboot_probe', post)
            if post.active_generation == 16 and (post.status_flags & 0x80):
                break
        except Exception as exc:
            emit('right_after_reboot_wait',
                 error_type=type(exc).__name__, error=str(exc))
        time.sleep(2)
    else:
        raise SystemExit('right did not reach active=16 + local_boot_validated')

    conf = right.confirm(allow_confirm=True, preflight_status=False)
    after_confirm = decode_status_payload(conf.payload)
    summarize('right_confirm_ack', after_confirm)
    final = decode_status_payload(right.hello().payload)
    summarize('right_final', final)
    if final.state != 0 or final.active_generation != 16:
        raise SystemExit(
            f'expected Idle gen16 after confirm; got state={final.state} '
            f'active={final.active_generation}')
    emit('RIGHT_GEN16_CONFIRM_PASS',
         active=final.active_generation,
         local_valid=bool(final.status_flags & 0x80),
         flags=final.status_flags)
    emit('STOP_FOR_POWER_CYCLE',
         next='power-cycle whole unit, then recheck right active=16 Idle + controller')
except Exception:
    emit('FAIL', traceback=traceback.format_exc())
    raise
finally:
    try:
        transport.close()
    except Exception:
        pass
PY
'''


def main() -> int:
    out_dir = Path(__file__).resolve().parent / "go1_live_readonly"
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    out_path = out_dir / f"right_gen16_pac_{stamp}.json"

    remote = (
        REMOTE.replace("__REMOTE_DIR__", REMOTE_DIR)
        .replace("__PACKAGE_SHA__", PACKAGE_SHA)
        .replace("__IMAGE_SHA__", IMAGE_SHA)
    )

    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(
        HOST, username=USER, password=PASSWORD,
        look_for_keys=False, allow_agent=False, timeout=10,
    )
    try:
        _, stdout, stderr = client.exec_command(remote, timeout=300)
        out = stdout.read().decode("utf-8", "replace")
        err = stderr.read().decode("utf-8", "replace")
        code = stdout.channel.recv_exit_status()
    finally:
        client.close()

    record = {
        "schema": "makcm.go1-right-gen16-pac.v1",
        "captured_utc": datetime.now(timezone.utc).isoformat(),
        "package_sha256": PACKAGE_SHA,
        "image_sha256": IMAGE_SHA,
        "exit_code": code,
        "stdout": out,
        "stderr": err,
        "events": [],
    }
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                record["events"].append(json.loads(line))
            except json.JSONDecodeError:
                pass
    out_path.write_text(json.dumps(record, indent=2, ensure_ascii=False) + "\n",
                        encoding="utf-8")
    print(out)
    if err.strip():
        print(err, file=sys.stderr)
    print(f"saved {out_path}")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
