#!/usr/bin/env python3
"""Re-sign left-gen17.mkota from the current build on the trusted Linux box.

The originally signed gen17 left image predates the makcm_bridge_rx stack fix
(handoff 3.6.5).  Shipping it would OTA the left board back onto a build that
reboots the moment the right board answers, so the gen17 package must be
rebuilt from current source before any pair update runs.

Replaces the package in the gen17 release directory in place -- there is no
reason to keep the broken one around.
"""
from __future__ import annotations

import hashlib
import json
import os
import sys
from pathlib import Path

import paramiko

HOST = "192.168.137.2"
USER = "li"
PASSWORD = os.environ.get("MAKCM_LINUX_PASSWORD", "1234567")
REMOTE_DIR = "/home/li/.local/share/makcm-ota/gen17-release"
KEY = "/home/li/.config/makcm-ota/signing/production-p256.pem"
PUB = "/home/li/.config/makcm-ota/signing/production-p256.pub.pem"
SIGNER_KEY_ID = "3dd2425f94e65ad13275fe6d1e2603bd"

GENERATION = 17
MIN_PEER = 16
MAX_PEER = 17

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "build" / "makcm_left_idf.bin"
RELEASE = ROOT / "diagnostics" / "ota" / "release_gen17"
TOOL = ROOT / "tools" / "makcm_ota_tool.py"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    if not BIN.is_file():
        raise SystemExit(f"missing bin: {BIN}")
    image_sha = sha256(BIN)
    image_bytes = BIN.stat().st_size
    print(f"local_bin={BIN}")
    print(f"image_sha={image_sha}")
    print(f"image_bytes={image_bytes}")

    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(HOST, username=USER, password=PASSWORD,
                   look_for_keys=False, allow_agent=False, timeout=10)
    try:
        client.exec_command(f"mkdir -p {REMOTE_DIR}",
                            timeout=10)[1].channel.recv_exit_status()
        sftp = client.open_sftp()
        try:
            sftp.put(str(BIN), f"{REMOTE_DIR}/left_app_g17.bin")
            sftp.put(str(TOOL), f"{REMOTE_DIR}/makcm_ota_tool.py")
        finally:
            sftp.close()

        remote = f"""
set -e
cd {REMOTE_DIR}
rm -f left-gen17.mkota
python3 makcm_ota_tool.py package \\
  --image left_app_g17.bin \\
  --output left-gen17.mkota \\
  --private-key {KEY} \\
  --signer-key-id {SIGNER_KEY_ID} \\
  --role left \\
  --generation {GENERATION} \\
  --min-peer-generation {MIN_PEER} \\
  --max-peer-generation {MAX_PEER}
python3 makcm_ota_tool.py inspect --package left-gen17.mkota
python3 makcm_ota_tool.py verify --package left-gen17.mkota --public-key {PUB}
sha256sum left-gen17.mkota left_app_g17.bin | tee left-gen17.mkota.sha256.txt
"""
        _, stdout, stderr = client.exec_command(remote, timeout=180)
        out = stdout.read().decode("utf-8", "replace")
        err = stderr.read().decode("utf-8", "replace")
        code = stdout.channel.recv_exit_status()
        sys.stdout.write(out)
        if err.strip():
            sys.stderr.write(err)
        if code != 0:
            raise SystemExit(f"remote sign failed exit={code}")

        RELEASE.mkdir(parents=True, exist_ok=True)
        sftp = client.open_sftp()
        try:
            for name in ("left-gen17.mkota", "left-gen17.mkota.sha256.txt",
                         "left_app_g17.bin"):
                sftp.get(f"{REMOTE_DIR}/{name}", str(RELEASE / name))
        finally:
            sftp.close()

        if sha256(RELEASE / "left_app_g17.bin") != image_sha:
            raise SystemExit("pulled image sha mismatch")

        summary_path = RELEASE / "release_gen17.summary.json"
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        summary["boards"]["left"] = {
            "image_sha256": image_sha,
            "image_bytes": image_bytes,
            "package_sha256": sha256(RELEASE / "left-gen17.mkota"),
            "package_bytes": (RELEASE / "left-gen17.mkota").stat().st_size,
            "remote_package": f"{REMOTE_DIR}/left-gen17.mkota",
        }
        summary_path.write_text(json.dumps(summary, indent=2) + "\n",
                                encoding="utf-8")
        print(json.dumps(summary["boards"]["left"], indent=2))
        return 0
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
