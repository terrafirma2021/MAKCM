#!/usr/bin/env python3
"""Sign one MAKCM pair release on the trusted Linux host.

The release generation is read from ``common/OtaGeneration.h`` so a package can
never be cut for a generation the firmware was not built at.

The private key never leaves the trusted Linux host: this program uploads the
two unsigned application images plus ``makcm_ota_tool.py``, signs and verifies
remotely, then pulls the signed packages back for the evidence record.  It
never touches a serial port -- flashing is ``tools/update_proxy_pair.py``.
"""
from __future__ import annotations

import argparse
import getpass
import hashlib
import json
import os
import re
import sys
from pathlib import Path

import paramiko

sys.path.insert(0, str(Path(__file__).resolve().parent))
from makcm_ota_tool import validate_intrinsic_identity  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
GENERATION_HEADER = ROOT / "common" / "OtaGeneration.h"
OTA_TOOL = ROOT / "tools" / "makcm_ota_tool.py"
LEFT_IMAGE = (ROOT / "MAKCM_ESP32s3_Device_Mouse_Left_IDF" / "build"
              / "makcm_left_idf.bin")
RIGHT_IMAGE = (ROOT / "MAKCM_ESP32s3_HID_Mouse_Right" / "build"
               / "makcm_right_idf.bin")

REMOTE_SIGNING = "/home/li/.config/makcm-ota/signing"
PRIVATE_KEY = f"{REMOTE_SIGNING}/production-p256.pem"
PUBLIC_KEY = f"{REMOTE_SIGNING}/production-p256.pub.pem"
SIGNER_KEY_ID = "3dd2425f94e65ad13275fe6d1e2603bd"


def read_generation() -> int:
    text = GENERATION_HEADER.read_text(encoding="utf-8")
    match = re.search(r"^#define\s+MAKCM_OTA_PAIR_GENERATION\s+(\d+)\s*$",
                      text, re.MULTILINE)
    if match is None:
        raise SystemExit(f"cannot parse generation from {GENERATION_HEADER}")
    generation = int(match.group(1))
    text_match = re.search(
        r'^#define\s+MAKCM_OTA_PAIR_GENERATION_TEXT\s+"(\d+)"\s*$',
        text, re.MULTILINE)
    if text_match is None or int(text_match.group(1)) != generation:
        raise SystemExit("numeric and text generation macros disagree")
    return generation


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def assert_image_identity(image: Path, role: str, generation: int) -> None:
    """Fail before upload if the build is stale.

    This is the same descriptor check the packager enforces remotely; running
    it locally turns a stale build into an immediate, readable error instead
    of an SSH round trip that fails at the signing step.
    """
    try:
        data = image.read_bytes()
    except FileNotFoundError as exc:
        raise SystemExit(f"missing {role} image: {image}") from exc
    validate_intrinsic_identity(
        data, role=role, generation=generation,
        label=f"{role} image {image.name}")


def run(client: paramiko.SSHClient, command: str, timeout: int = 180) -> str:
    _, stdout, stderr = client.exec_command(command, timeout=timeout)
    out = stdout.read().decode("utf-8", "replace")
    err = stderr.read().decode("utf-8", "replace")
    code = stdout.channel.recv_exit_status()
    sys.stdout.write(out)
    if err.strip():
        sys.stderr.write(err)
    if code != 0:
        raise SystemExit(f"remote command failed exit={code}")
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.137.2")
    parser.add_argument("--user", default="li")
    parser.add_argument(
        "--min-peer-generation", type=int, default=None,
        help="default: this generation minus one (the pre-update peer)")
    parser.add_argument(
        "--out", type=Path, default=None,
        help="local evidence directory; default diagnostics/ota/release_gen<N>")
    args = parser.parse_args()

    generation = read_generation()
    min_peer = (generation - 1 if args.min_peer_generation is None
                else args.min_peer_generation)
    out_dir = args.out or (ROOT / "diagnostics" / "ota"
                           / f"release_gen{generation}")
    remote_dir = f"/home/li/.local/share/makcm-ota/gen{generation}-release"

    boards = (("left", LEFT_IMAGE), ("right", RIGHT_IMAGE))
    for role, image in boards:
        assert_image_identity(image, role, generation)

    print(f"generation={generation} min_peer={min_peer} max_peer={generation}")
    local_sha = {role: sha256(image) for role, image in boards}
    for role, image in boards:
        print(f"{role}_image={image}")
        print(f"{role}_image_sha256={local_sha[role]}")
        print(f"{role}_image_bytes={image.stat().st_size}")

    password = os.environ.get("MAKCM_LINUX_PASSWORD") or getpass.getpass(
        f"password for {args.user}@{args.host}: ")

    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(args.host, username=args.user, password=password,
                   look_for_keys=False, allow_agent=False, timeout=10)
    try:
        run(client, f"mkdir -p {remote_dir}")
        sftp = client.open_sftp()
        try:
            sftp.put(str(OTA_TOOL), f"{remote_dir}/makcm_ota_tool.py")
            for role, image in boards:
                sftp.put(str(image), f"{remote_dir}/{role}_app_g{generation}.bin")
        finally:
            sftp.close()

        for role, _ in boards:
            stem = f"{role}-gen{generation}"
            run(client, f"""
set -e
cd {remote_dir}
python3 makcm_ota_tool.py package \\
  --image {role}_app_g{generation}.bin \\
  --output {stem}.mkota \\
  --private-key {PRIVATE_KEY} \\
  --signer-key-id {SIGNER_KEY_ID} \\
  --role {role} \\
  --generation {generation} \\
  --min-peer-generation {min_peer} \\
  --max-peer-generation {generation}
python3 makcm_ota_tool.py inspect --package {stem}.mkota
python3 makcm_ota_tool.py verify --package {stem}.mkota --public-key {PUBLIC_KEY}
sha256sum {stem}.mkota {role}_app_g{generation}.bin | tee {stem}.mkota.sha256.txt
""")

        out_dir.mkdir(parents=True, exist_ok=True)
        sftp = client.open_sftp()
        try:
            for role, _ in boards:
                stem = f"{role}-gen{generation}"
                for name in (f"{stem}.mkota", f"{stem}.mkota.sha256.txt",
                             f"{role}_app_g{generation}.bin"):
                    sftp.get(f"{remote_dir}/{name}", str(out_dir / name))
        finally:
            sftp.close()

        summary = {
            "generation": generation,
            "min_peer_generation": min_peer,
            "max_peer_generation": generation,
            "signer_key_id": SIGNER_KEY_ID,
            "remote_dir": remote_dir,
            "boards": {},
        }
        for role, image in boards:
            stem = f"{role}-gen{generation}"
            pulled = sha256(out_dir / f"{role}_app_g{generation}.bin")
            if pulled != local_sha[role]:
                raise SystemExit(f"{role} image sha mismatch after round trip")
            summary["boards"][role] = {
                "image_sha256": local_sha[role],
                "image_bytes": image.stat().st_size,
                "package_sha256": sha256(out_dir / f"{stem}.mkota"),
                "package_bytes": (out_dir / f"{stem}.mkota").stat().st_size,
                "remote_package": f"{remote_dir}/{stem}.mkota",
            }
        (out_dir / f"release_gen{generation}.summary.json").write_text(
            json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(summary, indent=2))
        return 0
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
