#!/usr/bin/env python3
"""GO-1 left-board read-only status over Linux CH343.

READ ONLY. Does not Prepare/Activate/Confirm/Abort/write Flash.
Does NOT use Windows COM download ports (e.g. COM3).
"""

from __future__ import annotations

import json
import sys
import traceback
from datetime import datetime, timezone
from pathlib import Path

import paramiko

HOST = "192.168.137.2"
USER = "li"
PASSWORD = "1234567"
PORT = "/dev/ttyACM0"

REMOTE = r"""
set -e
PORT=/dev/ttyACM0
if [ ! -e "$PORT" ]; then
  echo '{"blocked":true,"reason":"serial_missing","port":"/dev/ttyACM0"}'
  exit 3
fi
export PYTHONPATH="/home/li/.local/share/makcm-ota/go1-left-gen15:/home/li/.local/share/makcm-ota/tools${PYTHONPATH:+:$PYTHONPATH}"
python3 - <<'PY'
import json
from makcm_ota_tool import (
    SerialTransport, OtaClient, TARGET_LEFT, decode_status_payload
)

port = '/dev/ttyACM0'
transport = SerialTransport(port, baudrate=4_000_000, timeout=0.05)
client = OtaClient(transport, target=TARGET_LEFT, timeout=2.0, retries=2)
try:
    hello = client.hello()
    v1 = decode_status_payload(hello.payload)
    out = {
        'mode': 'readonly_status',
        'writes': False,
        'port': port,
        'hello_state': v1.state,
        'active_generation': v1.active_generation,
        'target_generation': v1.target_generation,
        'status_flags': v1.status_flags,
        'maintenance_exclusive': bool(v1.status_flags & 0x20),
        'reason': getattr(v1, 'reason', None),
    }
    try:
        status_v2 = client.status(v2=True)
        v2 = decode_status_payload(status_v2.payload)
        out['v2'] = {
            'state': v2.state,
            'active_generation': v2.active_generation,
            'target_generation': v2.target_generation,
            'status_flags': v2.status_flags,
        }
    except Exception as exc:
        out['v2_error'] = f'{type(exc).__name__}: {exc}'
    print(json.dumps(out, ensure_ascii=False))
finally:
    transport.close()
PY
"""


def main() -> int:
    out_dir = Path(__file__).resolve().parent / "go1_live_readonly"
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    out_path = out_dir / f"left_status_{stamp}.json"

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

    record = {
        "schema": "makcm.go1-readonly-status.v1",
        "captured_utc": datetime.now(timezone.utc).isoformat(),
        "host": HOST,
        "port": PORT,
        "flash_write": False,
        "windows_com_download": False,
        "exit_code": code,
        "stdout": out,
        "stderr": err,
    }
    try:
        for line in out.splitlines():
            line = line.strip()
            if line.startswith("{"):
                record["status"] = json.loads(line)
                break
    except json.JSONDecodeError:
        record["status_parse_error"] = True

    out_path.write_text(json.dumps(record, indent=2, ensure_ascii=False) + "\n",
                        encoding="utf-8")
    print(json.dumps(record, indent=2, ensure_ascii=False))
    print(f"saved {out_path}")
    return code


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception:
        traceback.print_exc()
        raise SystemExit(1)
