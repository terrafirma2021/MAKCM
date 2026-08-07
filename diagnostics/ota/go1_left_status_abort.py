#!/usr/bin/env python3
"""Read left-board V1 status over Linux CH343 and abort leftover maintenance."""

from __future__ import annotations

import json
import sys

import paramiko

HOST = "192.168.137.2"
USER = "li"
PASSWORD = "1234567"
REMOTE = r"""
cd /home/li/.local/share/makcm-ota/go1-left-gen15 && python3 - <<'PY'
from makcm_ota_tool import SerialTransport, OtaClient, TARGET_LEFT, decode_status_payload
import json

transport = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
client = OtaClient(transport, target=TARGET_LEFT, timeout=2.0, retries=2)
try:
    hello = client.hello()
    status = decode_status_payload(hello.payload)
    print(json.dumps({
        'hello_state': status.state,
        'active_generation': status.active_generation,
        'target_generation': status.target_generation,
        'status_flags': status.status_flags,
        'maintenance_exclusive': bool(status.status_flags & 0x20),
    }, ensure_ascii=False))
    if status.state != 0:
        abort = client.abort(allow_ota=True)
        abort_status = decode_status_payload(abort.payload)
        print(json.dumps({
            'abort_state': abort_status.state,
            'abort_flags': abort_status.status_flags,
            'abort_reason': abort_status.reason,
        }, ensure_ascii=False))
finally:
    transport.close()
PY
"""


def main() -> int:
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(
        HOST,
        username=USER,
        password=PASSWORD,
        look_for_keys=False,
        allow_agent=False,
        timeout=10,
    )
    try:
        _stdin, stdout, stderr = client.exec_command(REMOTE, timeout=30)
        out = stdout.read().decode("utf-8", "replace")
        err = stderr.read().decode("utf-8", "replace")
        code = stdout.channel.recv_exit_status()
    finally:
        client.close()
    if out:
        sys.stdout.write(out)
        if not out.endswith("\n"):
            sys.stdout.write("\n")
    if err.strip():
        sys.stderr.write(err)
        if not err.endswith("\n"):
            sys.stderr.write("\n")
    print(f"exit {code}")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
