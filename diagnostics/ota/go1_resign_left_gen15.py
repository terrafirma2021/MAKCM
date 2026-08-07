#!/usr/bin/env python3
"""Upload rebuilt left bin to Linux, resign left-gen15.mkota, pull artifacts back."""
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

import paramiko

HOST = "192.168.137.2"
USER = "li"
PASSWORD = "1234567"
REMOTE_DIR = "/home/li/.local/share/makcm-ota/go1-left-gen15"
KEY = "/home/li/.config/makcm-ota/signing/production-p256.pem"
SIGNER_KEY_ID = "3dd2425f94e65ad13275fe6d1e2603bd"

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "build" / "makcm_left_idf.bin"
FREEZE = ROOT / "diagnostics" / "ota" / "go1_freeze_20260806"
TOOL = ROOT / "tools" / "makcm_ota_tool.py"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sftp_put(sftp, local: Path, remote: str) -> None:
    sftp.put(str(local), remote)


def sftp_get(sftp, remote: str, local: Path) -> None:
    local.parent.mkdir(parents=True, exist_ok=True)
    sftp.get(remote, str(local))


def main() -> int:
    if not BIN.is_file():
        raise SystemExit(f"missing bin: {BIN}")
    image_sha = sha256(BIN)
    print(f"local_bin={BIN}")
    print(f"image_sha={image_sha}")
    print(f"image_bytes={BIN.stat().st_size}")

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
        client.exec_command(f"mkdir -p {REMOTE_DIR}", timeout=10)[1].channel.recv_exit_status()
        sftp = client.open_sftp()
        try:
            sftp_put(sftp, BIN, f"{REMOTE_DIR}/left_app_gen15.bin")
            sftp_put(sftp, TOOL, f"{REMOTE_DIR}/makcm_ota_tool.py")
        finally:
            sftp.close()

        remote = f"""
set -e
cd {REMOTE_DIR}
python3 makcm_ota_tool.py package \\
  --image left_app_gen15.bin \\
  --output left-gen15.mkota \\
  --private-key {KEY} \\
  --signer-key-id {SIGNER_KEY_ID} \\
  --role left \\
  --generation 15 \\
  --min-peer-generation 5 \\
  --max-peer-generation 15
python3 makcm_ota_tool.py inspect --package left-gen15.mkota
python3 makcm_ota_tool.py verify --package left-gen15.mkota --public-key /home/li/.config/makcm-ota/signing/production-p256.pub.pem
sha256sum left-gen15.mkota left_app_gen15.bin | tee left-gen15.mkota.sha256.txt
python3 - <<'PY'
import hashlib, json
from pathlib import Path
pkg = Path('left-gen15.mkota').read_bytes()
img = Path('left_app_gen15.bin').read_bytes()
print(json.dumps({{
  'package_sha256': hashlib.sha256(pkg).hexdigest(),
  'image_sha256': hashlib.sha256(img).hexdigest(),
  'package_bytes': len(pkg),
  'image_bytes': len(img),
}}))
PY
"""
        _, stdout, stderr = client.exec_command(remote, timeout=120)
        out = stdout.read().decode("utf-8", "replace")
        err = stderr.read().decode("utf-8", "replace")
        code = stdout.channel.recv_exit_status()
        sys.stdout.write(out)
        if err.strip():
            sys.stderr.write(err)
        if code != 0:
            raise SystemExit(f"remote sign failed exit={code}")

        FREEZE.mkdir(parents=True, exist_ok=True)
        sftp = client.open_sftp()
        try:
            sftp_get(sftp, f"{REMOTE_DIR}/left-gen15.mkota", FREEZE / "left-gen15.mkota")
            sftp_get(
                sftp,
                f"{REMOTE_DIR}/left-gen15.mkota.sha256.txt",
                FREEZE / "left-gen15.mkota.sha256.txt",
            )
            sftp_get(sftp, f"{REMOTE_DIR}/left_app_gen15.bin", FREEZE / "left_app_gen15.bin")
        finally:
            sftp.close()

        package_sha = sha256(FREEZE / "left-gen15.mkota")
        pulled_image_sha = sha256(FREEZE / "left_app_gen15.bin")
        if pulled_image_sha != image_sha:
            raise SystemExit("pulled image sha mismatch")
        summary = {
            "image_sha256": image_sha,
            "package_sha256": package_sha,
            "package_bytes": (FREEZE / "left-gen15.mkota").stat().st_size,
            "image_bytes": BIN.stat().st_size,
            "signer_key_id": SIGNER_KEY_ID,
            "remote_dir": REMOTE_DIR,
        }
        (FREEZE / "left-gen15.resign.summary.json").write_text(
            json.dumps(summary, indent=2) + "\n", encoding="utf-8"
        )
        print(json.dumps(summary, indent=2))
        print(f"saved {FREEZE / 'left-gen15.mkota'}")
        return 0
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
