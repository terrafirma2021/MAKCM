#!/usr/bin/env python3
"""Trusted-Linux orchestrator for signed MAKCM pair OTA updates.

This is deliberately a thin policy layer over ``makcm_ota_tool.py``.  The
firmware protocol remains the authority for signature, generation, peer-health
and rollback decisions; this program adds the missing operational pieces:

* verify both signed packages before touching a serial port;
* stop and later restore an optional user systemd runtime that owns CH343;
* pin one explicit Linux serial path and record its USB identity;
* persist one transaction and resume prepare/activate/confirm idempotently;
* emit an atomic JSON audit and never restart the data plane before confirm.

The updater never accepts a private key and never invokes esptool/ROM download.
Bootloader/partition-table recovery remains an explicit rescue operation.
"""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Any, Callable, Sequence
import zlib

try:  # Importable on Windows for offline unit tests; live execution is Linux-only.
    import fcntl  # type: ignore
except ImportError:  # pragma: no cover - exercised by Windows test imports
    fcntl = None  # type: ignore

import makcm_ota_tool as mkot


SCHEMA = "makcm.pair-auto-update.v2"
LEGACY_SCHEMA = "makcm.pair-auto-update.v1"
PHASES = ("new", "prepared", "activated", "confirmed", "maintenance_required")
EXPECTED_USB_VID = "1a86"
EXPECTED_USB_PID = "55d3"
UNIT_RE = re.compile(r"^[A-Za-z0-9_.@-]+$")
# A confirmed journal is allowed to suppress a duplicate update only when it
# is bound to the exact CH343 instance that is attached now.  Keep this list
# explicit: accepting a partial identity would turn an old/unbound journal
# into evidence for an arbitrary replacement adapter.
USB_IDENTITY_FIELDS = ("port", "tty", "vid", "pid", "serial", "bus_path")


class UpdateError(RuntimeError):
    pass


@dataclasses.dataclass(frozen=True)
class PackageRecord:
    path: str
    role: str
    generation: int
    min_peer_generation: int
    max_peer_generation: int
    package_sha256: str
    image_sha256: str
    image_size: int
    signer_key_id: str
    # SHA-256 of the signed 192-byte manifest wire value.  It is distinct from
    # the complete package hash and is required to match Status V2 identity.
    manifest_sha256: str = ""
    chunk_size: int = 0


@dataclasses.dataclass
class Journal:
    schema: str
    transaction_id: int
    phase: str
    target_generation: int
    left: PackageRecord
    right: PackageRecord
    port: str
    usb_identity: dict[str, str]
    runtime_unit: str | None
    runtime_scope: str
    runtime_was_active: bool
    created_at: str
    updated_at: str
    events: list[dict[str, Any]]
    # Per-board durable state.  Each state-changing command has a pending
    # intent until an exact Status V2 fact is persisted.
    boards: dict[str, dict[str, Any]] = dataclasses.field(
        default_factory=lambda: {"left": {}, "right": {}}
    )
    record_seq: int = 0
    record_crc32: str = ""

    @classmethod
    def from_dict(cls, value: dict[str, Any]) -> "Journal":
        try:
            value = dict(value)
            # v1 journals created before runtime_scope was persisted used the
            # system service by default.  Preserve resumability for them.
            value.setdefault("runtime_scope", "system")
            value.setdefault("boards", {"left": {}, "right": {}})
            value.setdefault("record_seq", 0)
            value.setdefault("record_crc32", "")
            value["left"].setdefault("manifest_sha256", "")
            value["right"].setdefault("manifest_sha256", "")
            value["left"].setdefault("chunk_size", 0)
            value["right"].setdefault("chunk_size", 0)
            value["left"] = PackageRecord(**value["left"])
            value["right"] = PackageRecord(**value["right"])
            journal = cls(**value)
        except (KeyError, TypeError, ValueError) as exc:
            raise UpdateError(f"invalid OTA journal: {exc}") from exc
        if journal.schema != SCHEMA or journal.phase not in PHASES:
            raise UpdateError("unsupported OTA journal schema or phase")
        if journal.runtime_scope not in ("system", "user"):
            raise UpdateError("invalid OTA journal runtime scope")
        if not (0 < journal.transaction_id <= 0xFFFFFFFF):
            raise UpdateError("invalid OTA journal transaction id")
        if journal.record_seq < 0:
            raise UpdateError("invalid OTA journal record sequence")
        if not isinstance(journal.boards, dict) or any(
            role not in journal.boards for role in ("left", "right")
        ):
            raise UpdateError("OTA journal is missing per-board state")
        return journal

    def as_dict(self) -> dict[str, Any]:
        return dataclasses.asdict(self)


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_package(path: Path, public_key: Path, signer_key_id: str) -> PackageRecord:
    try:
        package = path.read_bytes()
    except OSError as exc:
        raise UpdateError(f"cannot read OTA package {path}: {exc}") from exc
    if len(package) <= mkot.MANIFEST_SIZE:
        raise UpdateError(f"OTA package has no image: {path}")
    try:
        manifest = mkot.decode_manifest(package[: mkot.MANIFEST_SIZE])
        info = mkot.package_info(package, source=str(path), manifest=manifest)
        role = mkot.role_name(manifest.role)
        if role not in ("left", "right"):
            raise UpdateError(f"OTA package has unsupported role: {role}")
        mkot.validate_intrinsic_identity(
            package[mkot.MANIFEST_SIZE :], role=role,
            generation=manifest.generation, label=f"{path} image",
        )
        valid = mkot.verify_manifest_signature(
            manifest, str(public_key), expected_signer_key_id=signer_key_id
        )
    except Exception as exc:
        raise UpdateError(f"OTA package verification failed for {path}: {exc}") from exc
    if not valid:
        raise UpdateError(f"OTA package signature is invalid: {path}")
    return PackageRecord(
        path=str(path.resolve()),
        role=mkot.role_name(manifest.role),
        generation=manifest.generation,
        min_peer_generation=manifest.min_peer_generation,
        max_peer_generation=manifest.max_peer_generation,
        package_sha256=info["package_sha256"],
        image_sha256=info["image_sha256"],
        image_size=info["image_size"],
        signer_key_id=manifest.signer_key_id.hex(),
        manifest_sha256=hashlib.sha256(package[: mkot.MANIFEST_SIZE]).hexdigest(),
        chunk_size=manifest.chunk_size,
    )


def validate_pair(left: PackageRecord, right: PackageRecord) -> int:
    if left.role != "left" or right.role != "right":
        raise UpdateError("package roles must be left/right respectively")
    if left.generation != right.generation or left.generation == 0:
        raise UpdateError("left/right target generations must match and be nonzero")
    generation = left.generation
    for record in (left, right):
        if not (record.min_peer_generation <= generation <= record.max_peer_generation):
            raise UpdateError(f"{record.role} package rejects target peer generation")
    return generation


def validate_transition(left: PackageRecord, right: PackageRecord,
                        active_generation: int) -> None:
    """Require each new image to tolerate the old peer during rolling activation."""
    if active_generation == left.generation:
        return
    for record in (left, right):
        if not (record.min_peer_generation <= active_generation <=
                record.max_peer_generation):
            raise UpdateError(
                f"{record.role} package rejects current peer generation "
                f"{active_generation} during rolling activation"
            )


def stage_package(record: PackageRecord, staging_dir: Path) -> PackageRecord:
    """Pin verified bytes in a private transaction directory before flashing."""
    staging_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(staging_dir, 0o700)
    source = Path(record.path)
    destination = staging_dir / f"{record.role}-{record.package_sha256}.mkota"
    if not destination.exists():
        temporary: str | None = None
        try:
            with source.open("rb") as input_file:
                fd, temporary = tempfile.mkstemp(
                    prefix=destination.name + ".", dir=staging_dir
                )
                with os.fdopen(fd, "wb") as output_file:
                    shutil.copyfileobj(input_file, output_file, 1024 * 1024)
                    output_file.flush()
                    os.fsync(output_file.fileno())
            os.chmod(temporary, 0o400)
            os.replace(temporary, destination)
            if os.name != "nt":
                directory_fd = os.open(staging_dir, os.O_RDONLY)
                try:
                    os.fsync(directory_fd)
                finally:
                    os.close(directory_fd)
        except OSError as exc:
            raise UpdateError(f"cannot stage verified {record.role} package: {exc}") from exc
        finally:
            if temporary is not None:
                with contextlib.suppress(FileNotFoundError):
                    os.unlink(temporary)
    actual = sha256_file(destination)
    if actual != record.package_sha256:
        raise UpdateError(f"staged {record.role} package hash mismatch")
    return dataclasses.replace(record, path=str(destination.resolve()))


def verify_staged_package(record: PackageRecord) -> None:
    try:
        actual = sha256_file(Path(record.path))
    except OSError as exc:
        raise UpdateError(f"cannot read staged {record.role} package: {exc}") from exc
    if actual != record.package_sha256:
        raise UpdateError(f"staged {record.role} package changed after verification")


def atomic_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temporary, 0o600)
        os.replace(temporary, path)
        # POSIX can durably fsync the directory entry after os.replace().
        # Windows rejects opening a directory as a normal file descriptor;
        # offline tests still retain file fsync + atomic replacement there.
        if os.name != "nt":
            directory_fd = os.open(path.parent, os.O_RDONLY)
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
    finally:
        with contextlib.suppress(FileNotFoundError):
            os.unlink(temporary)


def _journal_sidecars(path: Path) -> tuple[Path, Path]:
    return (
        path.with_name(path.name + ".a"),
        path.with_name(path.name + ".b"),
    )


def _journal_crc(value: dict[str, Any]) -> str:
    canonical = dict(value)
    canonical["record_crc32"] = ""
    payload = (json.dumps(canonical, sort_keys=True, separators=(",", ":"))
               .encode("utf-8"))
    return f"{zlib.crc32(payload) & 0xFFFFFFFF:08x}"


def _decode_journal_candidate(path: Path) -> Journal:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise UpdateError(f"cannot read OTA journal {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise UpdateError("OTA journal root must be an object")
    schema = value.get("schema")
    if schema == LEGACY_SCHEMA:
        raise UpdateError(
            "legacy v1 OTA journal is not resumable automatically; archive it "
            "as diagnostic evidence and start a fresh v2 transaction"
        )
    journal = Journal.from_dict(value)
    if journal.record_seq <= 0 or not re.fullmatch(r"[0-9a-f]{8}", journal.record_crc32):
        raise UpdateError("OTA journal lacks v2 record sequence/CRC")
    if _journal_crc(value) != journal.record_crc32:
        raise UpdateError(f"OTA journal CRC mismatch: {path}")
    return journal


def read_journal(path: Path) -> Journal | None:
    candidates = [path, *_journal_sidecars(path)]
    existing = [candidate for candidate in candidates if candidate.exists()]
    if not existing:
        return None
    valid: list[tuple[Path, Journal, bytes]] = []
    errors: list[str] = []
    for candidate in existing:
        try:
            journal = _decode_journal_candidate(candidate)
            canonical = json.dumps(
                journal.as_dict(), sort_keys=True, separators=(",", ":")
            ).encode("utf-8")
            valid.append((candidate, journal, canonical))
        except UpdateError as exc:
            errors.append(str(exc))
    if not valid:
        raise UpdateError(
            "no valid OTA journal copy remains; refusing state-changing recovery: "
            + "; ".join(errors)
        )
    newest_sequence = max(item[1].record_seq for item in valid)
    newest = [item for item in valid if item[1].record_seq == newest_sequence]
    if any(item[2] != newest[0][2] for item in newest[1:]):
        names = ", ".join(str(item[0]) for item in newest)
        raise UpdateError(
            "OTA journal split-brain: equal record sequence has divergent "
            f"valid contents ({names})"
        )
    return newest[0][1]


def save_journal(path: Path, journal: Journal) -> None:
    journal.updated_at = utc_now()
    journal.record_seq = max(0, int(journal.record_seq)) + 1
    value = journal.as_dict()
    value["record_crc32"] = _journal_crc(value)
    journal.record_crc32 = value["record_crc32"]
    # Write both durable copies before the human-friendly primary.  A crash at
    # any point leaves at least one complete copy for read-only reconciliation.
    for copy_path in (*_journal_sidecars(path), path):
        atomic_json(copy_path, value)


def archive_legacy_confirmed(path: Path) -> bool:
    """Archive a legacy v1 confirmed journal as diagnostic-only evidence.

    Unfinished v1 journals are intentionally not touched; callers must stop
    and require an explicit human recovery decision.
    """

    existing = [
        candidate for candidate in (path, *_journal_sidecars(path))
        if candidate.exists()
    ]
    if not existing:
        return False
    decoded: list[tuple[Path, dict[str, Any], bytes]] = []
    for candidate in existing:
        try:
            value = json.loads(candidate.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise UpdateError(
                "legacy journal copies are mixed/corrupt; preserving every "
                f"copy for manual recovery: {candidate}: {exc}"
            ) from exc
        if not isinstance(value, dict) or value.get("schema") != LEGACY_SCHEMA:
            raise UpdateError(
                "legacy journal copies are mixed with another schema; "
                "preserving every copy for manual recovery"
            )
        if value.get("phase") != "confirmed":
            raise UpdateError(
                "unfinished v1 OTA journal cannot be resumed automatically; "
                "preserve it for manual board-fact reconciliation"
            )
        canonical = json.dumps(value, sort_keys=True,
                               separators=(",", ":")).encode("utf-8")
        decoded.append((candidate, value, canonical))
    if any(item[2] != decoded[0][2] for item in decoded[1:]):
        raise UpdateError(
            "legacy confirmed journal copies disagree; preserving every "
            "copy for manual recovery"
        )
    value = decoded[0][1]
    archive = path.with_name(
        f"{path.stem}.legacy-gen{value.get('target_generation', 'unknown')}."
        f"tx{value.get('transaction_id', 'unknown')}.confirmed.json"
    )
    atomic_json(archive, value)
    for copy_path in existing:
        copy_path.unlink()
    return True


def record_event(journal: Journal, event: str, **fields: Any) -> None:
    journal.events.append({"at": utc_now(), "event": event, **fields})


def usb_identity(port: Path) -> dict[str, str]:
    if sys.platform != "linux":
        raise UpdateError("automatic pair OTA is supported only on trusted Linux")
    resolved = port.resolve(strict=True)
    tty = resolved.name
    node = Path("/sys/class/tty") / tty / "device"
    if not node.exists():
        raise UpdateError(f"serial port has no sysfs identity: {resolved}")
    current = node.resolve()
    vendor = product = serial = bus_path = ""
    for parent in (current, *current.parents):
        vendor_file = parent / "idVendor"
        product_file = parent / "idProduct"
        if vendor_file.exists() and product_file.exists():
            vendor = vendor_file.read_text(encoding="ascii").strip().lower()
            product = product_file.read_text(encoding="ascii").strip().lower()
            serial_file = parent / "serial"
            if serial_file.exists():
                serial = serial_file.read_text(encoding="utf-8", errors="replace").strip()
            bus_path = parent.name
            break
    if vendor != EXPECTED_USB_VID or product != EXPECTED_USB_PID:
        raise UpdateError(
            f"explicit port is not pinned CH343 {EXPECTED_USB_VID}:{EXPECTED_USB_PID}: "
            f"{vendor or 'unknown'}:{product or 'unknown'}"
        )
    return {
        "port": str(resolved),
        "tty": tty,
        "vid": vendor,
        "pid": product,
        "serial": serial,
        "bus_path": bus_path,
    }


def validate_unit(unit: str | None) -> str | None:
    if unit in (None, "", "none"):
        return None
    if not UNIT_RE.fullmatch(unit):
        raise UpdateError("runtime unit contains unsupported characters")
    return unit


class Runner:
    def __init__(self, *, python: str, ota_tool: Path):
        self.python = python
        self.ota_tool = ota_tool.resolve()

    def run_json(self, args: Sequence[str], timeout: float) -> dict[str, Any]:
        command = [self.python, str(self.ota_tool), *args]
        result = subprocess.run(
            command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=timeout, check=False,
        )
        if result.returncode != 0:
            detail = result.stderr.strip() or result.stdout.strip()
            raise UpdateError(f"MKOT command failed ({result.returncode}): {detail}")
        try:
            value = json.loads(result.stdout)
        except json.JSONDecodeError as exc:
            raise UpdateError("MKOT command returned non-JSON output") from exc
        if not isinstance(value, dict):
            raise UpdateError("MKOT command JSON root is not an object")
        return value

    @staticmethod
    def systemctl(unit: str, action: str, scope: str) -> None:
        command = ["systemctl"]
        if scope == "user":
            command.append("--user")
        command.extend([action, unit])
        result = subprocess.run(
            command, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        if result.returncode != 0:
            raise UpdateError(
                f"systemctl {scope} {action} {unit} failed: "
                f"{result.stderr.strip() or result.stdout.strip()}"
            )

    @staticmethod
    def unit_state(unit: str, scope: str) -> tuple[str, str]:
        command = ["systemctl"]
        if scope == "user":
            command.append("--user")
        command.extend([
            "show", unit, "--property=LoadState", "--property=ActiveState", "--value"
        ])
        result = subprocess.run(
            command,
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        if result.returncode != 0:
            raise UpdateError(
                f"systemctl {scope} show {unit} failed: "
                f"{result.stderr.strip() or result.stdout.strip()}"
            )
        values = [line.strip() for line in result.stdout.splitlines()]
        load = values[0] if values else "not-found"
        active = values[1] if len(values) > 1 else "inactive"
        return load, active


def command_common(args: argparse.Namespace, tx: int) -> list[str]:
    return [
        "--port", args.port,
        "--baud", str(args.baud),
        "--timeout", str(args.timeout),
        "--retries", str(args.retries),
        "--transaction-id", str(tx),
    ]


def status_left(runner: Runner, args: argparse.Namespace, tx: int) -> dict[str, Any]:
    return runner.run_json(
        ["status", *command_common(args, tx), "--target", "left"], timeout=30
    )


def status_right(runner: Runner, args: argparse.Namespace, tx: int) -> dict[str, Any]:
    """Read right-board status through the same transaction-scoped bridge."""
    return runner.run_json(
        ["status", *command_common(args, tx), "--target", "right"], timeout=30
    )


STATUS_V2_REQUIRED_FIELDS = (
    "version", "role", "local_phase", "state", "last_command",
    "last_response_command", "last_ota_operation", "completed_outcome",
    "reason", "protocol_epoch", "next_offset", "record_transaction_id",
    "completed_transaction_id", "target_generation", "active_generation",
    "previous_generation", "partition_schema", "image_size",
    "record_sequence", "last_request_sequence",
    "completed_terminal_sequence", "target_slot", "previous_slot",
    "running_slot", "boot_slot", "image_state",
    "completed_terminal_command", "evidence_flags",
    "status_flags", "image_sha256", "completed_image_sha256",
    "running_image_sha256", "manifest_sha256", "identity_digest",
    "previous_image_sha256", "app_elf_sha256", "hardware_revision",
    "project_name", "app_version", "reset_reason", "boot_attempt",
    "last_ota_error",
    "last_state_query_error", "record_crc32",
)


def status_v2_payload(status: dict[str, Any], *, role: str) -> dict[str, Any]:
    """Extract a complete Status V2 board fact; never accept V1 implicitly."""

    try:
        payload = status["response"]["status"]
    except (KeyError, TypeError) as exc:
        raise UpdateError(f"{role} status response has no status payload") from exc
    if not isinstance(payload, dict) or payload.get("version") != 2:
        raise UpdateError(
            f"{role} status is not Status V2; automatic updater refuses V1"
        )
    missing = [field for field in STATUS_V2_REQUIRED_FIELDS if field not in payload]
    if missing:
        raise UpdateError(
            f"{role} Status V2 is missing exact board facts: {','.join(missing)}"
        )
    expected_role = mkot.TARGET_LEFT if role == "left" else mkot.TARGET_RIGHT
    try:
        if int(payload.get("role")) != expected_role:
            raise UpdateError(f"{role} Status V2 role does not identify this board")
    except (TypeError, ValueError) as exc:
        raise UpdateError(f"{role} Status V2 role is malformed") from exc
    return payload


def status_values(status: dict[str, Any], *, role: str) -> tuple[int, int, int, int]:
    payload = status_v2_payload(status, role=role)
    try:
        return (
            int(payload["active_generation"]), int(payload["target_generation"]),
            int(payload["state"]), int(payload["status_flags"]),
        )
    except (TypeError, ValueError) as exc:
        raise UpdateError(f"{role} Status V2 has invalid numeric facts") from exc


def active_generation(status: dict[str, Any]) -> tuple[int, int, int]:
    active, target, state, _flags = status_values(status, role="left")
    return active, target, state


def _digest_field(payload: dict[str, Any], field: str, *, role: str) -> str:
    value = payload.get(field)
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", value):
        raise UpdateError(f"{role} Status V2 {field} is not a 32-byte SHA-256")
    return value.lower()


def fact_matches_active(
    status: dict[str, Any], *, role: str, tx: int, generation: int,
    package: PackageRecord,
) -> bool:
    payload = status_v2_payload(status, role=role)
    return (
        int(payload["record_transaction_id"]) == tx
        and int(payload["target_generation"]) == generation
        and int(payload["active_generation"]) <= generation
        and int(payload["evidence_flags"]) & mkot.STATUS_V2_EVIDENCE_ACTIVE
        and _digest_field(payload, "image_sha256", role=role) == package.image_sha256.lower()
        and _digest_field(payload, "manifest_sha256", role=role)
        == package.manifest_sha256.lower()
    )


def fact_matches_completed(
    status: dict[str, Any], *, role: str, tx: int, generation: int,
    package: PackageRecord,
) -> bool:
    payload = status_v2_payload(status, role=role)
    return (
        int(payload["completed_transaction_id"]) == tx
        and int(payload["target_generation"]) == generation
        and int(payload["active_generation"]) == generation
        and int(payload["completed_terminal_sequence"]) != 0
        and int(payload["completed_outcome"]) == 1
        and int(payload["completed_terminal_command"]) == mkot.CMD_CONFIRM
        and int(payload["evidence_flags"]) & mkot.STATUS_V2_EVIDENCE_COMPLETED
        and int(payload["evidence_flags"]) & mkot.STATUS_V2_EVIDENCE_LOCAL_VALIDATED
        and int(payload["evidence_flags"]) & mkot.STATUS_V2_EVIDENCE_PAIR_COMMITTED
        and int(payload["evidence_flags"]) & mkot.STATUS_V2_EVIDENCE_IDENTITY_VALID
        and _digest_field(payload, "completed_image_sha256", role=role)
        == package.image_sha256.lower()
        and _digest_field(payload, "manifest_sha256", role=role)
        == package.manifest_sha256.lower()
    )


def record_board_fact(
    journal: Journal, role: str, status: dict[str, Any], *, command: str,
) -> None:
    payload = status_v2_payload(status, role=role)
    board = journal.boards.setdefault(role, {})
    board["fact"] = {
        "observed_at": utc_now(),
        "command": command,
        "response": status.get("response", {}),
        "status": payload,
    }
    board["pending_action"] = None


def record_board_intent(
    journal: Journal, role: str, command: str, request_digest: str,
    *, sequence: int = 0, package: PackageRecord | None = None,
) -> None:
    if sequence in (0xFFFFFFFF,):
        raise UpdateError("request sequence 0xffffffff is reserved")
    # The pair CLI owns wire sequence allocation.  Unknown sequence is stored
    # as null rather than the wire-reserved zero; reconciliation must then
    # stop until an exact board fact supplies the durable sequence.
    persisted_sequence: int | None = sequence if sequence else None
    journal.boards.setdefault(role, {})["pending_action"] = {
        "command": command,
        "sequence": persisted_sequence,
        "request_digest": request_digest,
        "payload_hash": package.image_sha256 if package else "",
        "transaction_id": journal.transaction_id,
        "generation": journal.target_generation,
        "image_sha256": package.image_sha256 if package else "",
        "manifest_sha256": package.manifest_sha256 if package else "",
        "created_at": utc_now(),
    }
    record_event(
        journal, f"{role}_{command}_intent", request_digest=request_digest,
        sequence=persisted_sequence,
    )


def clear_board_intent(journal: Journal, role: str) -> None:
    journal.boards.setdefault(role, {})["pending_action"] = None


def command_digest(command: Sequence[str]) -> str:
    canonical = "\x00".join(command).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def prepare_command_sequence(package: PackageRecord, role: str) -> int:
    """Predict the state-changing PREPARE request sequence for pair CLI.

    OtaClient allocates one sequence per request and retries reuse it.  The
    pair prepare path has a fixed preamble (right: Enter+Manifest; left:
    Hello+Enter+Manifest) followed by the manifest-declared DATA chunks.
    """

    if package.chunk_size <= 0:
        raise UpdateError(f"{role} package has no chunk-size identity")
    chunks = (package.image_size + package.chunk_size - 1) // package.chunk_size
    return (3 + chunks) if role == "right" else (4 + chunks)


def next_board_command_sequence(
    journal: Journal, role: str, *, intervening_statuses: int = 0,
) -> int:
    """Derive the next wire command sequence from a stored exact board fact."""

    stored = stored_board_status(journal, role)
    if stored is None:
        raise UpdateError(f"{role} has no exact Status V2 fact for sequencing")
    payload = status_v2_payload(stored, role=role)
    try:
        last = int(payload["last_request_sequence"])
    except (TypeError, ValueError) as exc:
        raise UpdateError(f"{role} durable request sequence is malformed") from exc
    sequence = last + intervening_statuses + 1
    if last <= 0 or sequence >= 0xFFFFFFFF:
        raise UpdateError(f"{role} durable request sequence cannot advance safely")
    return sequence


def stored_board_status(journal: Journal, role: str) -> dict[str, Any] | None:
    fact = journal.boards.get(role, {}).get("fact")
    if not isinstance(fact, dict):
        return None
    status = fact.get("status")
    if not isinstance(status, dict):
        return None
    return {"response": {"status": status}}


def reconcile_pending_board(
    journal: Journal, role: str, status: dict[str, Any], package: PackageRecord,
    *, tx: int, generation: int,
) -> bool:
    """Consume a pending host intent only after an exact Status V2 fact.

    ``False`` means the board did not prove the intent's effect; callers must
    stop in maintenance rather than resend or abort an ambiguous operation.
    """

    board = journal.boards.setdefault(role, {})
    pending = board.get("pending_action")
    if not pending:
        return False
    if int(pending.get("transaction_id", 0)) != tx:
        raise UpdateError(f"{role} pending intent transaction differs from journal")
    if pending.get("sequence") in (None, 0, 0xFFFFFFFF):
        raise UpdateError(
            f"{role} pending intent has no durable non-reserved request sequence"
        )
    command = str(pending.get("command", ""))
    payload = status_v2_payload(status, role=role)
    exact_active = fact_matches_active(
        status, role=role, tx=tx, generation=generation, package=package
    )
    exact_completed = fact_matches_completed(
        status, role=role, tx=tx, generation=generation, package=package
    )
    expected = {
        "prepare": (
            exact_active
            and int(payload["state"]) == mkot.STATE_PREPARED
            and int(payload["last_request_sequence"]) ==
                int(pending["sequence"])
        ),
        "activate": (
            exact_active
            and int(payload["state"])
            in (mkot.STATE_ACTIVATION_PENDING, mkot.STATE_PENDING_CONFIRM,
                mkot.STATE_CONFIRMED)
            and int(payload["last_request_sequence"]) ==
                int(pending["sequence"])
        ),
        "confirm": (
            exact_completed
            and int(payload["completed_terminal_sequence"]) ==
                int(pending["sequence"])
        ),
    }.get(command)
    if expected is None or not expected:
        raise UpdateError(
            f"{role} pending {command} has no exact Status V2 fact; "
            "leaving journal pending and refusing state-changing recovery"
        )
    record_board_fact(journal, role, status, command=command)
    record_event(journal, f"{role}_{command}_reconciled")
    return True


def assert_right_recovery_status(
    status: dict[str, Any], generation: int, *, tx: int,
    package: PackageRecord,
) -> None:
    """Require exact right-board health before reconciling ``activated``.

    This is deliberately stricter than the normal activation poll: the ACK
    for ``pair-confirm`` may have been lost, so the status read is the only
    evidence available.  It must be logically idle, at the requested
    generation, and explicitly report both peer-compatible and health-passed
    flags.  Any malformed/NACK/timeout response has already raised from
    ``status_right`` and leaves the journal untouched.
    """
    payload = status_v2_payload(status, role="right")
    active, target, state, flags = status_values(status, role="right")
    required_flags = mkot.STATUS_PEER_COMPATIBLE | mkot.STATUS_HEALTH_PASSED
    if active != generation:
        raise UpdateError(
            f"right recovery status generation {active} != target {generation}"
        )
    completed_tombstone = fact_matches_completed(
        status, role="right", tx=tx, generation=generation, package=package
    )
    if not completed_tombstone:
        raise UpdateError(
            "right recovery status is not an exact completed Status V2 tombstone"
        )
    if (flags & required_flags) != required_flags:
        raise UpdateError(
            "right recovery status lacks peer-compatible and health-passed flags"
        )


def assert_left_post_confirm_status(
    status: dict[str, Any], generation: int, *, tx: int,
    package: PackageRecord,
) -> None:
    """Require a durable completed Status V2 tombstone."""
    active, target, state, flags = status_values(status, role="left")
    if active != generation:
        raise UpdateError(
            f"post-confirm left generation {active} != target {generation}"
        )
    completed_tombstone = fact_matches_completed(
        status, role="left", tx=tx, generation=generation, package=package
    )
    if not completed_tombstone:
        raise UpdateError(
            "post-confirm left status is not an exact completed Status V2 tombstone"
        )


def assert_device_identity(port: str, expected: dict[str, str]) -> None:
    current = usb_identity(Path(port))
    for field in ("vid", "pid", "serial", "bus_path"):
        pinned = expected.get(field, "")
        if pinned and current.get(field, "") != pinned:
            raise UpdateError(f"CH343 identity changed at {field}")


def ensure_authorized(args: argparse.Namespace) -> None:
    missing = [
        name for name in ("allow_ota", "allow_activate", "allow_confirm")
        if not getattr(args, name)
    ]
    if missing:
        flags = ", ".join("--" + name.replace("_", "-") for name in missing)
        raise UpdateError(f"automatic update requires all independent authorizations: {flags}")


def validate_arguments(args: argparse.Namespace) -> None:
    """Reject malformed policy/transport values before any external side effect."""

    ensure_authorized(args)
    if args.transaction_id is not None and not (
        0 < int(args.transaction_id) <= 0xFFFFFFFF
    ):
        raise UpdateError("transaction id must be uint32 and nonzero")
    if int(args.baud) <= 0 or float(args.timeout) <= 0 or int(args.retries) < 0:
        raise UpdateError("serial baud/timeout must be positive and retries nonnegative")
    positive = (
        "runtime_stop_timeout", "phase_timeout", "peer_confirm_timeout",
        "peer_confirm_poll",
    )
    if any(float(getattr(args, name)) <= 0 for name in positive) or \
       float(args.runtime_settle) < 0 or float(args.peer_settle) < 0:
        raise UpdateError("runtime/phase/peer timeouts must be positive")
    if not re.fullmatch(r"[0-9a-fA-F]{32}", str(args.signer_key_id)):
        raise UpdateError("signer key id must be exactly 16 bytes of hexadecimal")
    trusted_tool = Path(__file__).with_name("makcm_ota_tool.py").resolve()
    selected_tool = Path(getattr(args, "ota_tool", trusted_tool)).resolve()
    if selected_tool != trusted_tool:
        raise UpdateError("production updater refuses an alternate OTA tool")
    selected_python = os.path.realpath(str(getattr(args, "python", sys.executable)))
    if selected_python != os.path.realpath(sys.executable):
        raise UpdateError("production updater refuses an alternate Python interpreter")


def assert_same_release(journal: Journal, left: PackageRecord,
                        right: PackageRecord, port: str,
                        runtime_unit: str | None, runtime_scope: str,
                        current_identity: dict[str, str]) -> None:
    if journal.target_generation != left.generation:
        raise UpdateError("journal target generation differs from requested release")
    if journal.left.package_sha256 != left.package_sha256 or \
       journal.right.package_sha256 != right.package_sha256:
        raise UpdateError("journal package hashes differ from requested release")
    if journal.left.manifest_sha256 != left.manifest_sha256:
        raise UpdateError("journal left manifest identity differs from release")
    if journal.right.manifest_sha256 != right.manifest_sha256:
        raise UpdateError("journal right manifest identity differs from release")
    if os.path.realpath(journal.port) != os.path.realpath(port):
        raise UpdateError("journal serial port differs from requested port")
    if journal.runtime_unit != runtime_unit or journal.runtime_scope != runtime_scope:
        raise UpdateError("journal runtime service configuration differs from request")
    for field in ("vid", "pid", "serial", "bus_path"):
        expected = journal.usb_identity.get(field, "")
        if expected and current_identity.get(field, "") != expected:
            raise UpdateError(f"journal CH343 identity differs at {field}")


def assert_complete_journal_identity(journal: Journal, port: str,
                                     current_identity: dict[str, str]) -> None:
    """Require a complete journal-to-device CH343 identity match.

    Missing fields are deliberately not treated as wildcards.  That remains
    safe for ordinary resume checks (which may use old synthetic fixtures),
    but is unsafe for either pair-health proof or activated recovery.
    """
    if os.path.realpath(journal.port) != os.path.realpath(port):
        raise UpdateError("journal serial port does not match current port")
    missing = [field for field in USB_IDENTITY_FIELDS
               if field not in journal.usb_identity]
    if missing:
        raise UpdateError(
            "confirmed journal does not contain complete CH343 identity: "
            + ",".join(missing)
        )
    mismatched = [field for field in USB_IDENTITY_FIELDS
                  if journal.usb_identity.get(field) != current_identity.get(field)]
    if mismatched:
        raise UpdateError(
            "confirmed journal CH343 identity does not match current device at "
            + ",".join(mismatched)
        )


def assert_confirmed_journal_identity(journal: Journal, port: str,
                                      current_identity: dict[str, str]) -> None:
    """Require a complete identity match before trusting ``confirmed``.

    A journal with ``phase=confirmed`` is the only local evidence we accept
    for a no-op when the left board is already at the requested generation.
    """
    if journal.phase != "confirmed":
        raise UpdateError(
            "left board is already current but journal phase does not prove "
            "that right-first pair confirmation was reached"
        )
    assert_complete_journal_identity(journal, port, current_identity)


def same_release(journal: Journal, left: PackageRecord,
                 right: PackageRecord, port: str) -> bool:
    return (
        journal.target_generation == left.generation
        and journal.left.package_sha256 == left.package_sha256
        and journal.right.package_sha256 == right.package_sha256
        and bool(journal.left.manifest_sha256)
        and journal.left.manifest_sha256 == left.manifest_sha256
        and bool(journal.right.manifest_sha256)
        and journal.right.manifest_sha256 == right.manifest_sha256
        and os.path.realpath(journal.port) == os.path.realpath(port)
    )


def update(args: argparse.Namespace, runner: Runner) -> dict[str, Any]:
    validate_arguments(args)
    public_key = Path(args.public_key).expanduser().resolve(strict=True)
    expected_public_hash = args.public_key_sha256.lower()
    if not re.fullmatch(r"[0-9a-f]{64}", expected_public_hash) or \
       sha256_file(public_key) != expected_public_hash:
        raise UpdateError("production public-key SHA-256 does not match pinned policy")

    journal_path = Path(args.journal).expanduser()
    try:
        journal = read_journal(journal_path)
    except UpdateError:
        # A confirmed v1 record is diagnostic evidence only; archive it and
        # start a new v2 transaction.  Any unfinished/corrupt-only v1 record
        # remains a hard stop.
        if archive_legacy_confirmed(journal_path):
            journal = None
        else:
            raise
    # An unfinished transaction resumes exclusively from its private staged
    # copies.  Moving the original release directory must not strand recovery.
    if journal is not None and journal.phase != "confirmed":
        left = verify_package(Path(journal.left.path), public_key, args.signer_key_id)
        right = verify_package(Path(journal.right.path), public_key, args.signer_key_id)
    else:
        left = verify_package(Path(args.left_package).expanduser(), public_key,
                              args.signer_key_id)
        right = verify_package(Path(args.right_package).expanduser(), public_key,
                               args.signer_key_id)
    generation = validate_pair(left, right)
    if not left.manifest_sha256 or not right.manifest_sha256:
        raise UpdateError(
            "package manifest identity is missing; automatic OTA requires "
            "signed manifest SHA-256 for Status V2 reconciliation"
        )
    identity = usb_identity(Path(args.port))

    unit = validate_unit(args.runtime_unit)
    if journal is not None and not same_release(journal, left, right, args.port):
        if journal.phase != "confirmed":
            raise UpdateError(
                "an unfinished journal belongs to a different release; resume or "
                "recover it before selecting new packages"
            )
        archive = journal_path.with_name(
            f"{journal_path.stem}.gen{journal.target_generation}."
            f"tx{journal.transaction_id}.confirmed.json"
        )
        atomic_json(archive, journal.as_dict())
        journal_path.unlink()
        journal = None
    if journal is None:
        tx = args.transaction_id or secrets.randbelow(0xFFFFFFFF - 1) + 1
        staging_dir = journal_path.parent / f"{journal_path.stem}.tx{tx}.packages"
        left = stage_package(left, staging_dir)
        right = stage_package(right, staging_dir)
        now = utc_now()
        journal = Journal(
            schema=SCHEMA, transaction_id=tx, phase="new",
            target_generation=generation, left=left, right=right,
            port=str(Path(args.port).resolve()), usb_identity=identity,
            runtime_unit=unit, runtime_scope=args.runtime_scope,
            runtime_was_active=False,
            created_at=now, updated_at=now, events=[],
            boards={"left": {}, "right": {}},
        )
        record_event(journal, "journal_created")
        save_journal(journal_path, journal)
    else:
        assert_same_release(
            journal, left, right, args.port, unit, args.runtime_scope, identity
        )
        # Validate the binding before stopping the optional runtime service.
        # Confirmed evidence and activated recovery are both bound to this
        # exact CH343 instance; an incomplete legacy identity must fail closed
        # without side effects.
        if journal.phase == "confirmed":
            assert_confirmed_journal_identity(journal, args.port, identity)
        else:
            assert_complete_journal_identity(journal, args.port, identity)
        left, right = journal.left, journal.right
        tx = journal.transaction_id
        record_event(journal, "journal_resumed", phase=journal.phase)
        save_journal(journal_path, journal)

    if journal.phase != "confirmed" and not args.controller_detached:
        raise UpdateError(
            "an update is required; detach the downstream controller and pass "
            "--controller-detached before stopping runtime or entering maintenance"
        )

    if unit:
        load, active = runner.unit_state(unit, args.runtime_scope)
        record_event(journal, "runtime_observed", unit=unit,
                     scope=args.runtime_scope, load=load, active=active)
        if load != "not-found" and active == "active":
            journal.runtime_was_active = True
            record_event(journal, "runtime_stop_intent", unit=unit)
            save_journal(journal_path, journal)
            runner.systemctl(unit, "stop", args.runtime_scope)
            record_event(journal, "runtime_stopped", unit=unit)
            save_journal(journal_path, journal)
            deadline = time.monotonic() + args.runtime_stop_timeout
            while True:
                stopped_load, stopped_active = runner.unit_state(
                    unit, args.runtime_scope
                )
                if stopped_load == "not-found" or stopped_active in ("inactive", "failed"):
                    break
                if time.monotonic() >= deadline:
                    raise UpdateError("runtime unit did not become inactive")
                time.sleep(min(0.1, max(0.0, deadline - time.monotonic())))
            time.sleep(args.runtime_settle)
        elif load != "not-found" and active not in ("inactive", "failed"):
            raise UpdateError(f"runtime unit is in unsafe transient state: {active}")

    # Before any new write, make downgrade/no-op decisions using complete
    # Status V2 board facts.  Generation/state/flags alone are never evidence
    # for this transaction.
    assert_device_identity(args.port, journal.usb_identity)
    status = status_left(runner, args, tx)
    right_status = status_right(runner, args, tx)
    left_payload = status_v2_payload(status, role="left")
    right_payload = status_v2_payload(right_status, role="right")
    active, target, state, left_flags = status_values(status, role="left")
    right_active_generation, right_target, right_state, _right_flags = \
        status_values(right_status, role="right")
    record_event(journal, "left_preflight", active_generation=active,
                 target_generation=target, state=state)
    record_event(journal, "right_preflight",
                 active_generation=right_active_generation,
                 target_generation=right_target, state=right_state)
    save_journal(journal_path, journal)
    if active > generation or right_active_generation > generation:
        raise UpdateError("refusing firmware generation downgrade")
    validate_transition(left, right, active)
    validate_transition(left, right, right_active_generation)
    left_exact_active = fact_matches_active(
        status, role="left", tx=tx, generation=generation, package=left
    )
    left_exact_completed = fact_matches_completed(
        status, role="left", tx=tx, generation=generation, package=left
    )
    right_exact_active = fact_matches_active(
        right_status, role="right", tx=tx, generation=generation, package=right
    )
    right_exact_completed = fact_matches_completed(
        right_status, role="right", tx=tx, generation=generation, package=right
    )

    # If a previous process died, consume only pending actions whose exact
    # board fact is present.  Any ambiguity stops before a new write.
    left_pending = journal.boards.get("left", {}).get("pending_action")
    if (
        isinstance(left_pending, dict)
        and left_pending.get("command") == "confirm"
        and right_exact_completed
        and left_exact_active
        and left_pending.get("sequence") not in (None, 0, 0xFFFFFFFF)
        and int(left_payload["last_request_sequence"]) <
            int(left_pending["sequence"])
    ):
        # Right committed first, but the exact left watermark proves the
        # predicted left CONFIRM was never observed. Retire only that unsent
        # intent; equal/greater sequence remains ambiguous and fails below.
        clear_board_intent(journal, "left")
        record_board_fact(journal, "left", status, command="preflight")
        record_event(journal, "left_confirm_intent_not_observed")
        save_journal(journal_path, journal)

    left_reconciled = False
    for role, package in (("left", left),):
        pending = journal.boards.get(role, {}).get("pending_action")
        if pending:
            reconcile_pending_board(
                journal, role, status, package, tx=tx, generation=generation
            )
            left_reconciled = True
            save_journal(journal_path, journal)

    right_reconciled = False
    right_pending = journal.boards.get("right", {}).get("pending_action")
    if right_pending:
        reconcile_pending_board(
            journal, "right", right_status, right, tx=tx, generation=generation
        )
        right_reconciled = True
        save_journal(journal_path, journal)

    # Fresh Status V2 facts are the sequencing watermark.  Resume journals
    # created before per-board facts existed may advance only after both boards
    # have been queried and exactly match this transaction/package.
    if not journal.boards.get("left", {}).get("pending_action") and (
        left_exact_active or left_exact_completed
    ):
        record_board_fact(journal, "left", status, command="preflight")
    if not journal.boards.get("right", {}).get("pending_action") and (
        right_exact_active or right_exact_completed
    ):
        record_board_fact(journal, "right", right_status, command="preflight")
    save_journal(journal_path, journal)

    if (
        left_reconciled
        and left_exact_active
        and state in (mkot.STATE_ACTIVATION_PENDING, mkot.STATE_PENDING_CONFIRM,
                      mkot.STATE_CONFIRMED)
        and journal.phase in ("new", "prepared")
    ):
        journal.phase = "activated"
        record_event(journal, "device_progress_reconciled", phase="activated")
        save_journal(journal_path, journal)

    # A process may die after both board facts are durable but before the
    # corresponding host phase save.  Promote only from the stored exact
    # facts, never from generation/state alone.
    stored_left = stored_board_status(journal, "left")
    stored_right = stored_board_status(journal, "right")
    if (
        journal.phase == "new"
        and not journal.boards.get("left", {}).get("pending_action")
        and not journal.boards.get("right", {}).get("pending_action")
        and stored_left is not None and stored_right is not None
        and int(status_v2_payload(stored_left, role="left")["state"]) == mkot.STATE_PREPARED
        and int(status_v2_payload(stored_right, role="right")["state"]) == mkot.STATE_PREPARED
        and fact_matches_active(stored_left, role="left", tx=tx, generation=generation, package=left)
        and fact_matches_active(stored_right, role="right", tx=tx, generation=generation, package=right)
    ):
        journal.phase = "prepared"
        record_event(journal, "device_progress_reconciled", phase="prepared")
        save_journal(journal_path, journal)
    if (
        journal.phase == "prepared"
        and not journal.boards.get("left", {}).get("pending_action")
        and not journal.boards.get("right", {}).get("pending_action")
        and stored_left is not None and stored_right is not None
        and int(status_v2_payload(stored_left, role="left")["state"])
            in (mkot.STATE_ACTIVATION_PENDING, mkot.STATE_PENDING_CONFIRM,
                mkot.STATE_CONFIRMED)
        and int(status_v2_payload(stored_right, role="right")["state"])
            in (mkot.STATE_ACTIVATION_PENDING, mkot.STATE_PENDING_CONFIRM,
                mkot.STATE_CONFIRMED)
        and fact_matches_active(stored_left, role="left", tx=tx, generation=generation, package=left)
        and fact_matches_active(stored_right, role="right", tx=tx, generation=generation, package=right)
    ):
        journal.phase = "activated"
        record_event(journal, "device_progress_reconciled", phase="activated")
        save_journal(journal_path, journal)

    if journal.phase == "activated":
        if not (left_exact_active or left_exact_completed):
            raise UpdateError(
                "activated recovery lacks an exact left Status V2 fact"
            )
        if right_exact_completed:
            pass
        elif right_exact_active:
            required = (
                mkot.STATUS_LOCAL_BOOT_VALIDATED |
                mkot.STATUS_PEER_COMPATIBLE |
                mkot.STATUS_HEALTH_PASSED
            )
            if right_state not in (
                mkot.STATE_PENDING_CONFIRM, mkot.STATE_CONFIRMED
            ) or (_right_flags & required) != required:
                raise UpdateError(
                    "activated recovery right Status V2 lacks local/peer/health proof"
                )
        else:
            raise UpdateError(
                "activated recovery lacks an exact right Status V2 fact or tombstone"
            )

    if left_exact_completed:
        if journal.phase == "activated":
            if not right_exact_completed:
                raise UpdateError(
                    "left completed tombstone is not paired with an exact right tombstone"
                )
            assert_right_recovery_status(
                right_status, generation, tx=tx, package=right
            )
            record_board_fact(journal, "right", right_status, command="confirm")
            journal.phase = "confirmed"
            record_event(journal, "device_progress_reconciled", phase="confirmed")
            save_journal(journal_path, journal)
        elif journal.phase != "confirmed":
            raise UpdateError(
                "left board has an exact completed tombstone but pair journal "
                "does not prove right completion"
            )
        else:
            if not right_exact_completed:
                raise UpdateError(
                    "confirmed journal no-op lacks exact right completed tombstone"
                )
            record_event(journal, "already_current")
            save_journal(journal_path, journal)
    elif active == generation and not left_exact_active:
        raise UpdateError(
            "left board is already at target generation without an exact "
            "transaction/image tombstone; refusing generation-only promotion"
        )
    elif journal.phase == "confirmed":
        raise UpdateError("confirmed journal no longer matches left Status V2 facts")

    common = command_common(args, tx)
    if journal.phase == "new":
        assert_device_identity(args.port, journal.usb_identity)
        verify_staged_package(left)
        verify_staged_package(right)
        prepare_command = [
            "pair-prepare", *common,
            "--right-package", right.path,
            "--left-package", left.path,
            "--right-package-sha256", right.package_sha256,
            "--left-package-sha256", left.package_sha256,
            "--allow-ota",
        ]
        digest = command_digest(prepare_command)
        record_board_intent(
            journal, "right", "prepare", digest,
            sequence=prepare_command_sequence(right, "right"), package=right,
        )
        record_board_intent(
            journal, "left", "prepare", digest,
            sequence=prepare_command_sequence(left, "left"), package=left,
        )
        save_journal(journal_path, journal)
        result = runner.run_json(prepare_command, timeout=args.phase_timeout)
        if result.get("activated") is not False or result.get("prepared_order") != ["right", "left"]:
            raise UpdateError("pair-prepare did not prove both boards prepared")
        prepared_facts = result.get("prepared_facts")
        if not isinstance(prepared_facts, dict):
            raise UpdateError("pair-prepare did not return per-board Status V2 facts")
        for role, package in (("right", right), ("left", left)):
            fact = prepared_facts.get(role)
            if not isinstance(fact, dict):
                raise UpdateError(f"pair-prepare has no {role} Status V2 fact")
            if not fact_matches_active(
                {"response": {"status": fact}},
                role=role, tx=tx, generation=generation, package=package,
            ) or int(fact.get("state", -1)) != mkot.STATE_PREPARED:
                raise UpdateError(f"pair-prepare {role} fact identity mismatch")
            record_board_fact(
                journal, role, {"response": {"status": fact}}, command="prepare"
            )
        journal.phase = "prepared"
        record_event(journal, "prepared", result=result)
        save_journal(journal_path, journal)

    if journal.phase == "prepared":
        assert_device_identity(args.port, journal.usb_identity)
        activate_command = [
            "pair-activate", *common,
            "--target-generation", str(generation),
            "--right-image-sha256", right.image_sha256,
            "--left-image-sha256", left.image_sha256,
            "--right-manifest-sha256", right.manifest_sha256,
            "--left-manifest-sha256", left.manifest_sha256,
            "--peer-confirm-timeout", str(args.peer_confirm_timeout),
            "--peer-confirm-poll", str(args.peer_confirm_poll),
            "--peer-settle", str(args.peer_settle),
            "--allow-activate",
        ]
        digest = command_digest(activate_command)
        record_board_intent(
            journal, "right", "activate", digest,
            sequence=next_board_command_sequence(journal, "right"),
            package=right,
        )
        record_board_intent(
            journal, "left", "activate", digest,
            sequence=next_board_command_sequence(journal, "left"),
            package=left,
        )
        save_journal(journal_path, journal)
        result = runner.run_json(activate_command, timeout=args.phase_timeout)
        peer = result.get("peer_confirmation", {})
        if result.get("activation_order") != ["right", "left"] or \
           int(peer.get("active_generation", -1)) != generation:
            raise UpdateError("pair-activate did not prove right-first target health")
        # The peer fact is necessary but not sufficient for the left board;
        # fetch and persist both exact Status V2 facts before advancing phase.
        right_status = status_right(runner, args, tx)
        left_status = status_left(runner, args, tx)
        if not fact_matches_active(
            right_status, role="right", tx=tx, generation=generation, package=right
        ) or int(status_v2_payload(right_status, role="right")["state"]) not in (
            mkot.STATE_ACTIVATION_PENDING, mkot.STATE_PENDING_CONFIRM,
            mkot.STATE_CONFIRMED,
        ):
            raise UpdateError("pair-activate right Status V2 fact mismatch")
        if not fact_matches_active(
            left_status, role="left", tx=tx, generation=generation, package=left
        ) or int(status_v2_payload(left_status, role="left")["state"]) not in (
            mkot.STATE_ACTIVATION_PENDING, mkot.STATE_PENDING_CONFIRM,
            mkot.STATE_CONFIRMED,
        ):
            raise UpdateError("pair-activate left Status V2 fact mismatch")
        record_board_fact(journal, "right", right_status, command="activate")
        record_board_fact(journal, "left", left_status, command="activate")
        journal.phase = "activated"
        record_event(journal, "activated", result=result)
        save_journal(journal_path, journal)

    if journal.phase == "activated":
        assert_device_identity(args.port, journal.usb_identity)
        confirm_command = [
            "pair-confirm", *common,
            "--target-generation", str(generation),
            "--right-image-sha256", right.image_sha256,
            "--left-image-sha256", left.image_sha256,
            "--right-manifest-sha256", right.manifest_sha256,
            "--left-manifest-sha256", left.manifest_sha256,
            "--peer-confirm-timeout", str(args.peer_confirm_timeout),
            "--peer-confirm-poll", str(args.peer_confirm_poll),
            "--peer-settle", str(args.peer_settle),
            "--allow-confirm",
        ]
        digest = command_digest(confirm_command)
        right_already_completed = fact_matches_completed(
            stored_board_status(journal, "right") or {}, role="right", tx=tx,
            generation=generation, package=right,
        ) if stored_board_status(journal, "right") is not None else False
        if not right_already_completed:
            record_board_intent(
                journal, "right", "confirm", digest,
                sequence=next_board_command_sequence(journal, "right"),
                package=right,
            )
        else:
            record_event(journal, "right_confirm_already_completed")
        record_board_intent(
            journal, "left", "confirm", digest,
            # pair-confirm performs one final left STATUS immediately before
            # the left write to avoid racing an already-completed transaction.
            sequence=next_board_command_sequence(
                journal, "left", intervening_statuses=1
            ),
            package=left,
        )
        save_journal(journal_path, journal)
        result = runner.run_json(confirm_command, timeout=args.phase_timeout)
        if result.get("pair_health_confirmed") is not True or \
           int(result.get("active_generation", -1)) != generation:
            raise UpdateError("pair-confirm did not prove pair health")
        right_status = status_right(runner, args, tx)
        left_status = status_left(runner, args, tx)
        if not fact_matches_completed(
            right_status, role="right", tx=tx, generation=generation, package=right
        ):
            raise UpdateError("pair-confirm right Status V2 tombstone mismatch")
        if not fact_matches_completed(
            left_status, role="left", tx=tx, generation=generation, package=left
        ):
            raise UpdateError("pair-confirm left Status V2 tombstone mismatch")
        record_board_fact(journal, "right", right_status, command="confirm")
        record_board_fact(journal, "left", left_status, command="confirm")
        journal.phase = "confirmed"
        record_event(journal, "confirmed", result=result)
        save_journal(journal_path, journal)

    assert_device_identity(args.port, journal.usb_identity)
    final_status = status_left(runner, args, tx)
    final_right_status = status_right(runner, args, tx)
    active, _target, _state, _flags = status_values(final_status, role="left")
    assert_left_post_confirm_status(
        final_status, generation, tx=tx, package=left
    )
    assert_right_recovery_status(
        final_right_status, generation, tx=tx, package=right
    )
    record_event(journal, "post_confirm_verified", active_generation=active)

    if unit and journal.runtime_was_active:
        runner.systemctl(unit, "start", args.runtime_scope)
        record_event(journal, "runtime_started", unit=unit)
        journal.runtime_was_active = False

    save_journal(journal_path, journal)
    audit_path = Path(args.audit).expanduser()
    atomic_json(audit_path, journal.as_dict())
    return {
        "schema": SCHEMA,
        "result": "confirmed",
        "transaction_id": tx,
        "active_generation": generation,
        "journal": str(journal_path.resolve()),
        "audit": str(audit_path.resolve()),
        "usb_identity": identity,
    }


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", required=True, help="explicit trusted-Linux CH343 port")
    p.add_argument("--left-package", required=True)
    p.add_argument("--right-package", required=True)
    p.add_argument("--public-key", required=True, help="public production trust key only")
    p.add_argument("--public-key-sha256", required=True,
                   help="pinned SHA-256 of the production public-key file")
    p.add_argument("--signer-key-id", required=True)
    p.add_argument("--journal", required=True)
    p.add_argument("--audit", required=True)
    p.add_argument("--runtime-unit", default="ai-secondary.service",
                   help="systemd unit, or 'none'; restarted only after confirm")
    p.add_argument("--runtime-scope", choices=("system", "user"), default="system")
    p.add_argument("--runtime-settle", type=float, default=1.0)
    p.add_argument("--runtime-stop-timeout", type=float, default=10.0)
    p.add_argument("--transaction-id", type=int)
    p.add_argument("--baud", type=int, default=4_000_000)
    p.add_argument("--timeout", type=float, default=3.0)
    p.add_argument("--retries", type=int, default=5)
    p.add_argument("--phase-timeout", type=float, default=180.0)
    p.add_argument("--peer-confirm-timeout", type=float, default=45.0)
    p.add_argument("--peer-confirm-poll", type=float, default=1.0)
    p.add_argument("--peer-settle", type=float, default=2.0)
    p.add_argument("--ota-tool", default=str(Path(__file__).with_name("makcm_ota_tool.py")))
    p.add_argument("--python", default=sys.executable)
    p.add_argument("--allow-ota", action="store_true")
    p.add_argument("--allow-activate", action="store_true")
    p.add_argument("--allow-confirm", action="store_true")
    p.add_argument("--controller-detached", action="store_true",
                   help="confirm downstream controller is detached (required before prepare, not for an already-current no-op)")
    return p


def main(argv: Sequence[str] | None = None,
         runner_factory: Callable[..., Runner] = Runner) -> int:
    args = parser().parse_args(argv)
    try:
        validate_arguments(args)
        if fcntl is None:
            raise UpdateError("automatic pair OTA lock requires trusted Linux")
        lock_path = Path(args.journal).expanduser().with_suffix(".lock")
        lock_path.parent.mkdir(parents=True, exist_ok=True)
        canonical_port = os.path.realpath(args.port)
        port_lock_name = hashlib.sha256(
            canonical_port.encode("utf-8")
        ).hexdigest()[:24]
        lock_root = Path(os.environ.get("XDG_RUNTIME_DIR", "/run/lock"))
        if not lock_root.is_dir():
            raise UpdateError(f"trusted per-port lock directory is unavailable: {lock_root}")
        port_lock_path = lock_root / f"makcm-ota-{port_lock_name}.lock"
        with lock_path.open("a+b") as lock, port_lock_path.open("a+b") as port_lock:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            fcntl.flock(port_lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = update(
                args,
                runner_factory(python=args.python, ota_tool=Path(args.ota_tool)),
            )
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    except (UpdateError, OSError, subprocess.SubprocessError) as exc:
        print(f"update_proxy_pair: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
