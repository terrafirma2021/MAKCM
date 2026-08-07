#!/usr/bin/env python3
"""Poll CH343 until left board rolls back to gen5 Idle, then run Prepare+Activate.

Blocks until physical power-cycle recovers the stuck PENDING_VERIFY gen15 Fault
image. Never uses Windows COM download ports / right board / Confirm.
"""

from __future__ import annotations

import json
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

import paramiko

HOST = "192.168.137.2"
PASSWORD = "1234567"
POLL_REMOTE = r'''
set +e
fuser -k /dev/ttyACM0 2>/dev/null
sleep 0.5
cd /home/li/.local/share/makcm-ota/go1-left-gen15
python3 - <<'PY'
import json
from makcm_ota_tool import SerialTransport, OtaClient, TARGET_LEFT, decode_status_payload

def emit(**kw):
    print(json.dumps(kw, ensure_ascii=False), flush=True)

TX_OLD = 70811770
for label, tx in (('old', TX_OLD), ('fresh', None)):
    try:
        t = SerialTransport('/dev/ttyACM0', baudrate=4_000_000, timeout=0.05)
        try:
            kwargs = {} if tx is None else {'transaction_id': tx}
            c = OtaClient(t, target=TARGET_LEFT, timeout=3.0, retries=1, **kwargs)
            st = decode_status_payload(c.hello().payload)
            emit(ok=True, label=label, state=st.state,
                 active=st.active_generation, target=st.target_generation,
                 flags=st.status_flags, local_valid=bool(st.status_flags & 0x80))
            raise SystemExit(0)
        finally:
            t.close()
    except SystemExit:
        raise
    except Exception as e:
        emit(ok=False, label=label, error=type(e).__name__ + ': ' + str(e))
raise SystemExit(1)
PY
'''


def probe_once() -> dict:
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
        _, o, e = client.exec_command(POLL_REMOTE, timeout=40)
        out = o.read().decode("utf-8", "replace")
        err = e.read().decode("utf-8", "replace")
        code = o.channel.recv_exit_status()
    finally:
        client.close()
    rows = []
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return {"exit_code": code, "rows": rows, "stderr": err, "stdout": out}


def recovered(rows: list[dict]) -> bool:
    for row in rows:
        if not row.get("ok"):
            continue
        if row.get("active") == 5 and row.get("state") == 0:
            return True
    return False


def main() -> int:
    out_dir = Path(__file__).resolve().parent / "go1_live_readonly"
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    log_path = out_dir / f"go1_wait_gen5_{stamp}.jsonl"
    print(
        "WAITING: power-cycle LEFT board now "
        "(CH343/DTR cannot reset; need physical power cut).",
        flush=True,
    )
    print(f"log={log_path}", flush=True)
    deadline = time.time() + 30 * 60
    poll = 0
    while time.time() < deadline:
        result = probe_once()
        row = {
            "utc": datetime.now(timezone.utc).isoformat(),
            "poll": poll,
            **result,
        }
        with log_path.open("a", encoding="utf-8") as f:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")
        summary = result["rows"][-1] if result["rows"] else {"ok": False}
        print(f"poll={poll} {summary}", flush=True)
        if recovered(result["rows"]):
            print("RECOVERED_GEN5 — launching Prepare+Activate", flush=True)
            script = Path(__file__).resolve().parent / "go1_left_retry_ota.py"
            rc = subprocess.call([sys.executable, str(script)])
            return rc
        poll += 1
        time.sleep(3.0)
    print("TIMEOUT waiting for gen5 after power-cycle", flush=True)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
