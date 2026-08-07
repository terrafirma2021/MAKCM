#!/usr/bin/env python3
"""Push IDF-env esptool + deps to Linux for download-mode recovery."""
from __future__ import annotations

import io
import sys
import zipfile
from pathlib import Path

import paramiko

HOST = "192.168.137.2"
PASSWORD = "1234567"
IDF_SITE = Path(
    r"C:\Users\li\Documents\MAKCM\.tools\espressif\python_env"
    r"\idf5.5_py3.13_env\Lib\site-packages"
)
WIN_SITE = Path(
    r"C:\Users\li\AppData\Local\Programs\Python\Python313\Lib\site-packages"
)

# Prefer IDF copies; fall back to system Python for reedsolo.py etc.
NAMES = [
    "esptool",
    "intelhex",
    "ecdsa",
    "serial",
    "yaml",
    "rich",
    "click",
    "markdown_it",
    "mdurl",
    "pygments",
    "linkify_it",
    "uc_micro",
    "reedsolo.py",
    "bitstring",
    "bitarray",
]


def add_tree(zf: zipfile.ZipFile, src: Path, arc_prefix: str) -> None:
    if src.is_file():
        zf.write(src, arc_prefix.replace("\\", "/"))
        return
    for path in src.rglob("*"):
        if not path.is_file():
            continue
        if "__pycache__" in path.parts:
            continue
        # skip native .pyd/.dll that won't run on Linux
        if path.suffix.lower() in {".pyd", ".dll", ".exe"}:
            continue
        rel = path.relative_to(src).as_posix()
        zf.write(path, f"{arc_prefix}/{rel}")


def resolve(name: str) -> Path | None:
    for site in (IDF_SITE, WIN_SITE):
        p = site / name
        if p.exists():
            return p
    return None


def main() -> int:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        for name in NAMES:
            src = resolve(name)
            if src is None:
                print(f"skip {name}")
                continue
            arc = name[:-3] if name.endswith(".py") and src.is_file() else name
            if name.endswith(".py") and src.is_file():
                zf.write(src, name)
                print(f"packed file {name}")
            else:
                add_tree(zf, src, name if src.is_dir() else arc)
                print(f"packed {src}")
    payload = buf.getvalue()
    print(f"zip_bytes={len(payload)}")

    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(
        HOST, username="li", password=PASSWORD,
        look_for_keys=False, allow_agent=False, timeout=10,
    )
    try:
        sftp = client.open_sftp()
        with sftp.file("/tmp/esptool_bundle.zip", "wb") as rf:
            rf.write(payload)
        sftp.close()
        cmd = r"""
set -e
python3 - <<'PY'
import sys, zipfile, pathlib
target = pathlib.Path.home()/'.local'/'lib'/f'python{sys.version_info[0]}.{sys.version_info[1]}'/'site-packages'
target.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile('/tmp/esptool_bundle.zip') as zf:
    zf.extractall(target)
print('extracted_to', target)
import esptool
print('esptool_ok', getattr(esptool, '__version__', '?'))
PY
"""
        _, o, e = client.exec_command(cmd, timeout=60)
        sys.stdout.write(o.read().decode())
        err = e.read().decode()
        if err.strip():
            sys.stderr.write(err)
        return o.channel.recv_exit_status()
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
