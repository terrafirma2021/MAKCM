#!/usr/bin/env python3
"""Drive the gen17 signed pair OTA from the trusted Linux host over CH343.

This is the first full V2-protocol run: every earlier hardware success was
either the gen5->genN V1 bridge or a COM3/COM6 direct flash.  The orchestrator
itself (`tools/update_proxy_pair.py`) owns all policy; this script only ships
it to the trusted host, pins the port and trust anchor, and streams the run
back so the transcript lands in the evidence record.

Preconditions the operator must satisfy:
  * the downstream controller is physically detached from USB3;
  * nothing else is holding the CH343 port.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

import paramiko

HOST, USER = "192.168.137.2", "li"
REMOTE_DIR = "/home/li/.local/share/makcm-ota/gen17-release"
PORT = "/dev/serial/by-id/usb-1a86_USB_Single_Serial_5ACC041332-if00"
PUBLIC_KEY = "/home/li/.config/makcm-ota/signing/production-p256.pub.pem"
PUBLIC_KEY_SHA256 = \
    "ee2378c2eafad90859d63d260cbc373c8f125d8efcfa6427dc8a53536395d399"
SIGNER_KEY_ID = "3dd2425f94e65ad13275fe6d1e2603bd"

ROOT = Path(__file__).resolve().parents[2]
ORCHESTRATOR = ROOT / "tools" / "update_proxy_pair.py"
OTA_TOOL = ROOT / "tools" / "makcm_ota_tool.py"
EVIDENCE = ROOT / "diagnostics" / "ota" / "release_gen17"

# Bump when the signed packages change: the journal stages package bytes at
# transaction creation and keeps reusing that staged copy, so a re-signed
# release must start a new transaction or it silently flashes the old image.
RUN = "gen17j"

COMMAND = f"""
cd {REMOTE_DIR}
python3 update_proxy_pair.py \\
  --port {PORT} \\
  --left-package left-gen17.mkota \\
  --right-package right-gen17.mkota \\
  --public-key {PUBLIC_KEY} \\
  --public-key-sha256 {PUBLIC_KEY_SHA256} \\
  --signer-key-id {SIGNER_KEY_ID} \\
  --journal {RUN}_journal.json \\
  --audit {RUN}_audit.json \\
  --runtime-unit none \\
  --phase-timeout 600 \\
  --controller-detached \\
  --allow-ota --allow-activate --allow-confirm
echo "ORCHESTRATOR_EXIT=$?"
"""


def main() -> int:
    password = os.environ.get("MAKCM_LINUX_PASSWORD")
    if not password:
        raise SystemExit("set MAKCM_LINUX_PASSWORD")

    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(HOST, username=USER, password=password,
                   look_for_keys=False, allow_agent=False, timeout=10)
    try:
        sftp = client.open_sftp()
        try:
            sftp.put(str(ORCHESTRATOR), f"{REMOTE_DIR}/update_proxy_pair.py")
            sftp.put(str(OTA_TOOL), f"{REMOTE_DIR}/makcm_ota_tool.py")
        finally:
            sftp.close()

        # Stream rather than buffer: an OTA that wedges must still show the
        # phase it reached.
        _, stdout, stderr = client.exec_command(COMMAND, timeout=1800,
                                                get_pty=True)
        for line in iter(stdout.readline, ""):
            sys.stdout.write(line)
            sys.stdout.flush()
        code = stdout.channel.recv_exit_status()
        err = stderr.read().decode("utf-8", "replace").strip()
        if err:
            sys.stderr.write(err + "\n")

        EVIDENCE.mkdir(parents=True, exist_ok=True)
        sftp = client.open_sftp()
        try:
            for name in (f"{RUN}_journal.json", f"{RUN}_audit.json"):
                try:
                    sftp.get(f"{REMOTE_DIR}/{name}", str(EVIDENCE / name))
                    print(f"pulled {name}")
                except IOError:
                    print(f"missing {name} (run did not reach that phase)")
        finally:
            sftp.close()
        return code
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
