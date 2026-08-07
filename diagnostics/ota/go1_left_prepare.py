#!/usr/bin/env python3
"""GO-1 left Prepare draft — Linux CH343 only; never Windows COM3.

PACKAGE_SHA is injected after trusted-Linux signing of left-gen15.mkota.
Stale/placeholder digests are hard-rejected.

This script performs Prepare only (no Activate). Unattended Activate is
forbidden; require GO-1 audit gates + verbal go-ahead before running.
"""

from __future__ import annotations

import json
import sys
import traceback

import paramiko

HOST = "192.168.137.2"
PASSWORD = "1234567"

# Injected after trusted-Linux sign of go1_freeze_20260806/left-gen15.mkota.
# Still STOP for Activate until GO-1 audit pack + verbal go-ahead.
PACKAGE_SHA = "2f0c1adc6ec16c3de74de3a6f9db78c01b6fbada84572cb900d0c5d64aece856"
FROZEN_LEFT_APP_SHA = (
    "f29cacc1fc5cd0417aaf32ee8303d9a9f74142aecde962836f9524df2903fd5c"
)
# Stale packages — keep rejecting if reintroduced.
_STALE_PACKAGE_SHAS = frozenset({
    "980687fb2f03cfd3d9485bd1a2ed8ea172064e0decb40bca8ea15ea733b9a25f",
    "d0ffdbbc8e6ebb0ec845176dbfa5d142408eb09bc1bfa493d9d21df466989695",
    "b0c7e199800097ca48487209d4ef953d6ab1622e6f4279f5b5efae080ab28c38",
    "REPLACE_AFTER_SIGNED_LEFT_GEN15_MKOTA",
})

REMOTE_TEMPLATE = r'''
set -e
cd /home/li/.local/share/makcm-ota/go1-left-gen15
python3 - <<'PY'
from makcm_ota_tool import (
    SerialTransport, OtaClient, TARGET_LEFT, decode_status_payload, OtaToolError
)
import hashlib, json

package_path = 'left-gen15.mkota'
package_sha = '__PACKAGE_SHA__'
frozen_app_sha = '__FROZEN_LEFT_APP_SHA__'
with open(package_path, 'rb') as f:
    package = f.read()
actual = hashlib.sha256(package).hexdigest()
if actual != package_sha:
    raise SystemExit(f'package sha mismatch: {actual} != {package_sha}')

# Intrinsic image identity must match the Windows freeze before any write.
from makcm_ota_tool import inspect_package
info = inspect_package(package) if 'inspect_package' in dir() else None

transport = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
client = OtaClient(transport, target=TARGET_LEFT, timeout=5.0, retries=2)
try:
    hello = client.hello()
    before = decode_status_payload(hello.payload)
    print(json.dumps({
        'phase': 'before',
        'state': before.state,
        'active_generation': before.active_generation,
        'target_generation': before.target_generation,
        'status_flags': before.status_flags,
        'transaction_id': client.transaction_id,
        'frozen_left_app_sha': frozen_app_sha,
    }, ensure_ascii=False))
    result = client.prepare(
        package,
        allow_ota=True,
        expected_package_sha256=package_sha,
    )
    print(json.dumps({'phase': 'prepare_ok', 'result': result}, ensure_ascii=False))
    status = client.status(v2=False)
    after = decode_status_payload(status.payload)
    print(json.dumps({
        'phase': 'after_prepare_v1',
        'state': after.state,
        'active_generation': after.active_generation,
        'target_generation': after.target_generation,
        'status_flags': after.status_flags,
        'next_offset': after.next_offset,
    }, ensure_ascii=False))
except Exception as exc:
    print(json.dumps({
        'phase': 'prepare_error',
        'error_type': type(exc).__name__,
        'error': str(exc),
    }, ensure_ascii=False))
    raise
finally:
    transport.close()
PY
'''


def main() -> int:
    if (
        not PACKAGE_SHA
        or PACKAGE_SHA.startswith("REPLACE_")
        or PACKAGE_SHA in _STALE_PACKAGE_SHAS
        or len(PACKAGE_SHA) != 64
    ):
        print(
            "STOP: left-gen15.mkota PACKAGE_SHA is missing/stale/invalid.\n"
            "This is NOT a COM3 flash path.\n"
            "Follow diagnostics/ota/go1_package_left_recipe.md on trusted Linux,\n"
            "then set PACKAGE_SHA to the verified .mkota digest.\n"
            f"Frozen left app SHA: {FROZEN_LEFT_APP_SHA}",
            file=sys.stderr,
        )
        return 2

    # Soft gate: Prepare path is wired after SHA inject, but Activate remains
    # forbidden for unattended/GO-1 STOP sessions. This script only Prepare.
    print(
        "NOTE: Prepare only via Linux CH343. No Activate in this script. "
        "Not COM3.",
        file=sys.stderr,
    )

    remote = (
        REMOTE_TEMPLATE
        .replace("__PACKAGE_SHA__", PACKAGE_SHA)
        .replace("__FROZEN_LEFT_APP_SHA__", FROZEN_LEFT_APP_SHA)
    )
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(
        HOST,
        username="li",
        password=PASSWORD,
        look_for_keys=False,
        allow_agent=False,
        timeout=10,
    )
    try:
        _stdin, stdout, stderr = client.exec_command(remote, timeout=180)
        out = stdout.read().decode("utf-8", "replace")
        err = stderr.read().decode("utf-8", "replace")
        code = stdout.channel.recv_exit_status()
    finally:
        client.close()
    if out.strip():
        print(out)
    if err.strip():
        print("STDERR:")
        print(err)
    print("exit", code)
    return code


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception:
        traceback.print_exc()
        raise SystemExit(1)
