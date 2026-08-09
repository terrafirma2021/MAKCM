#!/usr/bin/env python3
"""Trusted-Linux wrapper for the single-process MAKCM pair OTA flow."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from typing import Any, Callable, Sequence

try:
    import fcntl  # type: ignore
except ImportError:  # pragma: no cover - importable for Windows unit tests
    fcntl = None  # type: ignore

import makcm_ota_tool as mkot


AUDIT_SCHEMA = "makcm.pair-update.success.v1"
EXPECTED_USB_VID = "1a86"
EXPECTED_USB_PID = "55d3"
SYSTEMCTL_TIMEOUT_S = 30
UNIT_RE = re.compile(r"^[A-Za-z0-9_.@-]+$")


class UpdateError(RuntimeError):
    pass


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds")


def atomic_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary: str | None = None
    try:
        fd, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as output:
            json.dump(value, output, indent=2, sort_keys=True)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
        temporary = None
        if os.name != "nt":
            directory_fd = os.open(path.parent, os.O_RDONLY)
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
    finally:
        if temporary is not None:
            try:
                os.unlink(temporary)
            except FileNotFoundError:
                pass


def usb_identity(port: Path) -> dict[str, str]:
    if sys.platform != "linux":
        raise UpdateError("pair OTA is supported only on trusted Linux")
    resolved = port.resolve(strict=True)
    tty = resolved.name
    device = Path("/sys/class/tty") / tty / "device"
    if not device.exists():
        raise UpdateError(f"serial port has no sysfs identity: {resolved}")
    vendor = product = serial = bus_path = ""
    current = device.resolve()
    for parent in (current, *current.parents):
        vendor_file = parent / "idVendor"
        product_file = parent / "idProduct"
        if vendor_file.exists() and product_file.exists():
            vendor = vendor_file.read_text(encoding="ascii").strip().lower()
            product = product_file.read_text(encoding="ascii").strip().lower()
            serial_file = parent / "serial"
            if serial_file.exists():
                serial = serial_file.read_text(
                    encoding="utf-8", errors="replace"
                ).strip()
            bus_path = parent.name
            break
    if (vendor, product) != (EXPECTED_USB_VID, EXPECTED_USB_PID):
        raise UpdateError(
            "explicit port is not pinned CH343 "
            f"{EXPECTED_USB_VID}:{EXPECTED_USB_PID}: "
            f"{vendor or 'unknown'}:{product or 'unknown'}"
        )
    return {
        "port": str(resolved), "tty": tty, "vid": vendor, "pid": product,
        "serial": serial, "bus_path": bus_path,
    }


def validate_unit(unit: str | None) -> str | None:
    if unit in (None, "", "none"):
        return None
    if not UNIT_RE.fullmatch(unit):
        raise UpdateError("runtime unit contains unsupported characters")
    return unit


class Systemd:
    @staticmethod
    def unit_active(unit: str, scope: str) -> bool:
        command = ["systemctl"]
        if scope == "user":
            command.append("--user")
        command.extend(["is-active", "--quiet", unit])
        try:
            result = subprocess.run(
                command, check=False, timeout=SYSTEMCTL_TIMEOUT_S,
            )
        except subprocess.TimeoutExpired as exc:
            raise UpdateError(
                f"systemctl {scope} is-active {unit} timed out after "
                f"{SYSTEMCTL_TIMEOUT_S}s"
            ) from exc
        return result.returncode == 0

    @staticmethod
    def action(unit: str, action: str, scope: str) -> None:
        command = ["systemctl"]
        if scope == "user":
            command.append("--user")
        command.extend([action, unit])
        try:
            result = subprocess.run(
                command, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, check=False,
                timeout=SYSTEMCTL_TIMEOUT_S,
            )
        except subprocess.TimeoutExpired as exc:
            raise UpdateError(
                f"systemctl {scope} {action} {unit} timed out after "
                f"{SYSTEMCTL_TIMEOUT_S}s"
            ) from exc
        if result.returncode != 0:
            raise UpdateError(
                f"systemctl {scope} {action} {unit} failed: "
                f"{result.stderr.strip() or result.stdout.strip()}"
            )


def _read_once(path: Path, label: str) -> bytes:
    try:
        return path.read_bytes()
    except OSError as exc:
        raise UpdateError(f"cannot read {label} {path}: {exc}") from exc


def _validate_package(package: bytes, *, path: Path, role: int,
                      public_key: Path, signer_key_id: str) -> mkot.Manifest:
    try:
        if len(package) <= mkot.MANIFEST_SIZE:
            raise mkot.OtaToolError("package has no image")
        manifest = mkot.decode_manifest(package[:mkot.MANIFEST_SIZE])
        mkot.package_info(package, source=str(path), manifest=manifest)
        expected_project = (mkot.LEFT_PROJECT_NAME if role == mkot.TARGET_LEFT
                            else mkot.RIGHT_PROJECT_NAME)
        expected_schema = (mkot.LEFT_PARTITION_SCHEMA_V1
                           if role == mkot.TARGET_LEFT
                           else mkot.RIGHT_PARTITION_SCHEMA_V1)
        if manifest.role != role:
            raise mkot.OtaToolError("package role does not match filename argument")
        if manifest.hardware_revision != mkot.HARDWARE_REVISION_R1:
            raise mkot.OtaToolError("package hardware revision is not current")
        if manifest.project_name != expected_project:
            raise mkot.OtaToolError("package project name is not current")
        if manifest.partition_schema != expected_schema:
            raise mkot.OtaToolError("package partition schema is not current")
        if manifest.protocol_version != mkot.DEFAULT_PROTOCOL_VERSION:
            raise mkot.OtaToolError("package protocol version is not current")
        mkot.validate_intrinsic_identity(
            package[mkot.MANIFEST_SIZE:], role=role,
            generation=manifest.generation, label=f"{path} image",
        )
        if not mkot.verify_manifest_signature(
            manifest, public_key,
            expected_signer_key_id=signer_key_id,
        ):
            raise mkot.OtaToolError("manifest signature is invalid")
        return manifest
    except Exception as exc:
        raise UpdateError(f"OTA package verification failed for {path}: {exc}") from exc


def preflight_packages(*, left_path: Path, right_path: Path,
                       public_key: Path, public_key_sha256: str,
                       signer_key_id: str) -> tuple[bytes, bytes,
                                                    mkot.Manifest,
                                                    mkot.Manifest]:
    left_package = _read_once(left_path, "left package")
    right_package = _read_once(right_path, "right package")
    key_bytes = _read_once(public_key, "public key")
    expected_key_hash = public_key_sha256.lower()
    if len(expected_key_hash) != 64 or any(
        value not in "0123456789abcdef" for value in expected_key_hash
    ):
        raise UpdateError("public key SHA-256 must be 64 hexadecimal characters")
    if hashlib.sha256(key_bytes).hexdigest() != expected_key_hash:
        raise UpdateError("public key file does not match pinned SHA-256")
    left = _validate_package(
        left_package, path=left_path, role=mkot.TARGET_LEFT,
        public_key=public_key, signer_key_id=signer_key_id,
    )
    right = _validate_package(
        right_package, path=right_path, role=mkot.TARGET_RIGHT,
        public_key=public_key, signer_key_id=signer_key_id,
    )
    if left.generation != right.generation:
        raise UpdateError("left/right package generations must match")
    for manifest, peer in ((left, right), (right, left)):
        if not manifest.min_peer_generation <= peer.generation <= \
                manifest.max_peer_generation:
            raise UpdateError("package peer generation range rejects this pair")
    return left_package, right_package, left, right


def _lock_path(port: str) -> Path:
    lock_root = Path(os.environ.get("XDG_RUNTIME_DIR", "/run/lock"))
    if not lock_root.is_dir():
        raise UpdateError(f"trusted per-port lock directory is unavailable: {lock_root}")
    digest = hashlib.sha256(os.path.realpath(port).encode("utf-8")).hexdigest()[:24]
    return lock_root / f"makcm-ota-{digest}.lock"


def update(args: argparse.Namespace, *, systemd: Any = Systemd,
           identity_reader: Callable[[Path], dict[str, str]] = usb_identity,
           transport_factory: Callable[[], mkot.Transport] | None = None,
           progress: Callable[[str], None] | None = None) -> dict[str, Any]:
    if args.controller_detached is not True:
        raise UpdateError("--controller-detached is required before any write")
    if args.request_timeout <= 0 or args.boot_timeout <= 0 or args.retries < 0:
        raise UpdateError("timeouts must be positive and retries non-negative")
    unit = validate_unit(args.runtime_unit)
    left_package, right_package, left, right = preflight_packages(
        left_path=Path(args.left_package), right_path=Path(args.right_package),
        public_key=Path(args.public_key),
        public_key_sha256=args.public_key_sha256,
        signer_key_id=args.signer_key_id,
    )
    identity = identity_reader(Path(args.port))
    was_active = bool(unit and systemd.unit_active(unit, args.runtime_scope))
    if was_active:
        systemd.action(unit, "stop", args.runtime_scope)
    factory = transport_factory or (
        lambda: mkot.SerialTransport(
            args.port, baudrate=mkot.FAST_BAUD,
            timeout=min(args.request_timeout, 0.05),
        )
    )
    result = mkot.update_pair(
        transport_factory=factory, right_package=right_package,
        left_package=left_package, request_timeout=args.request_timeout,
        retries=args.retries, boot_timeout=args.boot_timeout,
        progress=progress, controller_detached=True,
    )
    if was_active:
        systemd.action(unit, "start", args.runtime_scope)
    audit = {
        "schema": AUDIT_SCHEMA, "completed_at": utc_now(),
        "generation": left.generation,
        "left_package_sha256": hashlib.sha256(left_package).hexdigest(),
        "right_package_sha256": hashlib.sha256(right_package).hexdigest(),
        "signer_key_id": left.signer_key_id.hex(),
        "usb_identity": identity, "result": result,
    }
    atomic_json(Path(args.audit), audit)
    return audit


def parser() -> argparse.ArgumentParser:
    value = argparse.ArgumentParser(description=__doc__)
    value.add_argument("--port", required=True)
    value.add_argument("--left-package", required=True)
    value.add_argument("--right-package", required=True)
    value.add_argument("--public-key", required=True)
    value.add_argument("--public-key-sha256", required=True)
    value.add_argument("--signer-key-id", required=True)
    value.add_argument("--audit", required=True)
    value.add_argument("--controller-detached", action="store_true", required=True)
    value.add_argument("--request-timeout", type=float, default=5.0)
    value.add_argument("--boot-timeout", type=float, default=45.0)
    value.add_argument("--retries", type=int, default=5)
    value.add_argument("--runtime-unit", default="ai-secondary.service")
    value.add_argument("--runtime-scope", choices=("system", "user"),
                       default="system")
    return value


def main(argv: Sequence[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        if fcntl is None:
            raise UpdateError("pair OTA requires trusted-Linux flock")
        lock_path = _lock_path(args.port)
        with lock_path.open("a+b") as lock:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = update(args, progress=lambda stage: print(
                stage, file=sys.stderr, flush=True
            ))
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    except (UpdateError, mkot.OtaToolError, OSError,
            subprocess.SubprocessError) as exc:
        print(f"update_proxy_pair: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
