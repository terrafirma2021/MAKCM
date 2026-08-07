#!/usr/bin/env python3
"""Upload right gen16 bin, sign right-gen16.mkota on trusted Linux, pull back.

Mirrors go1_resign_left_gen16.py.  The private key never leaves the trusted
Linux side; this host only uploads the unsigned application image and pulls
the signed package plus its hashes back for the freeze record.
"""
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

import paramiko

HOST = "192.168.137.2"
USER = "li"
PASSWORD = "1234567"
REMOTE_DIR = "/home/li/.local/share/makcm-ota/go1-right-gen16"
KEY = "/home/li/.config/makcm-ota/signing/production-p256.pem"
PUB = "/home/li/.config/makcm-ota/signing/production-p256.pub.pem"
SIGNER_KEY_ID = "3dd2425f94e65ad13275fe6d1e2603bd"

ROOT = Path(__file__).resolve().parents[2]
BIN = (ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / ".pio" / "build" / "RIGHT"
       / "firmware.bin")
FREEZE = ROOT / "diagnostics" / "ota" / "go1_freeze_20260806"
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
    print(f"local_bin={BIN}")
    print(f"image_sha={image_sha}")
    print(f"image_bytes={BIN.stat().st_size}")

    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(
        HOST, username=USER, password=PASSWORD,
        look_for_keys=False, allow_agent=False, timeout=10,
    )
    try:
        client.exec_command(f"mkdir -p {REMOTE_DIR}", timeout=10)[1].channel.recv_exit_status()
        sftp = client.open_sftp()
        try:
            sftp.put(str(BIN), f"{REMOTE_DIR}/right_app_gen16.bin")
            sftp.put(str(TOOL), f"{REMOTE_DIR}/makcm_ota_tool.py")
        finally:
            sftp.close()

        # The peer for this update is the left board, already running gen16.
        # min-peer stays permissive so an older left can still bridge a
        # recovery; max-peer is the current release.
        remote = f"""
set -e
cd {REMOTE_DIR}
python3 makcm_ota_tool.py package \\
  --image right_app_gen16.bin \\
  --output right-gen16.mkota \\
  --private-key {KEY} \\
  --signer-key-id {SIGNER_KEY_ID} \\
  --role right \\
  --generation 16 \\
  --min-peer-generation 5 \\
  --max-peer-generation 16
python3 makcm_ota_tool.py inspect --package right-gen16.mkota
python3 makcm_ota_tool.py verify --package right-gen16.mkota --public-key {PUB}
sha256sum right-gen16.mkota right_app_gen16.bin | tee right-gen16.mkota.sha256.txt
python3 - <<'PY'
import hashlib, json
from pathlib import Path
pkg = Path('right-gen16.mkota').read_bytes()
img = Path('right_app_gen16.bin').read_bytes()
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
            for name in (
                "right-gen16.mkota",
                "right-gen16.mkota.sha256.txt",
                "right_app_gen16.bin",
            ):
                sftp.get(f"{REMOTE_DIR}/{name}", str(FREEZE / name))
        finally:
            sftp.close()

        package_sha = sha256(FREEZE / "right-gen16.mkota")
        pulled = sha256(FREEZE / "right_app_gen16.bin")
        if pulled != image_sha:
            raise SystemExit("pulled image sha mismatch")
        summary = {
            "role": "right",
            "generation": 16,
            "image_sha256": image_sha,
            "package_sha256": package_sha,
            "package_bytes": (FREEZE / "right-gen16.mkota").stat().st_size,
            "image_bytes": BIN.stat().st_size,
            "signer_key_id": SIGNER_KEY_ID,
            "remote_dir": REMOTE_DIR,
        }
        (FREEZE / "right-gen16.resign.summary.json").write_text(
            json.dumps(summary, indent=2) + "\n", encoding="utf-8"
        )
        print(json.dumps(summary, indent=2))
        return 0
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
