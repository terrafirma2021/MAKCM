#!/usr/bin/env python3
"""Prove that a generated OTA trust anchor is embedded in both applications."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
from typing import Dict


class AnchorVerificationError(RuntimeError):
    pass


_EXPECTED = {"kSignerKeyId": 16, "kPublicX": 32, "kPublicY": 32}
_ARRAY = re.compile(
    r"constexpr\s+uint8_t\s+"
    r"(kSignerKeyId|kPublicX|kPublicY)\s*"
    r"\[\s*(\d+)\s*\]\s*=\s*\{([^}]*)\}\s*;",
    re.MULTILINE,
)


def parse_anchor(path: Path) -> Dict[str, bytes]:
    try:
        text = path.read_text(encoding="ascii")
    except OSError as exc:
        raise AnchorVerificationError(f"cannot read trust anchor: {path}") from exc
    values: Dict[str, bytes] = {}
    for match in _ARRAY.finditer(text):
        name, declared_text, body = match.groups()
        if name in values:
            raise AnchorVerificationError(f"duplicate trust-anchor array: {name}")
        declared = int(declared_text)
        expected = _EXPECTED[name]
        if declared != expected:
            raise AnchorVerificationError(
                f"{name} declares {declared} bytes, expected {expected}"
            )
        tokens = [token.strip() for token in body.split(",") if token.strip()]
        try:
            raw = bytes(int(token, 0) for token in tokens)
        except (ValueError, OverflowError) as exc:
            raise AnchorVerificationError(f"invalid byte in {name}") from exc
        if len(raw) != expected:
            raise AnchorVerificationError(
                f"{name} contains {len(raw)} bytes, expected {expected}"
            )
        if not any(raw):
            raise AnchorVerificationError(f"{name} is all zero")
        values[name] = raw
    missing = sorted(set(_EXPECTED) - set(values))
    if missing:
        raise AnchorVerificationError(
            "missing trust-anchor arrays: " + ", ".join(missing)
        )
    return values


def verify_application(path: Path, anchor: Dict[str, bytes]) -> dict:
    try:
        image = path.read_bytes()
    except OSError as exc:
        raise AnchorVerificationError(f"cannot read application: {path}") from exc
    if len(image) < 32 or image[0] != 0xE9:
        raise AnchorVerificationError(f"not an ESP application image: {path}")
    offsets = {name: image.find(raw) for name, raw in anchor.items()}
    missing = [name for name, offset in offsets.items() if offset < 0]
    if missing:
        raise AnchorVerificationError(
            f"application does not embed {', '.join(missing)}: {path}"
        )
    return {
        "path": str(path),
        "bytes": len(image),
        "sha256": hashlib.sha256(image).hexdigest(),
        "anchor_offsets": offsets,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--anchor", required=True, type=Path)
    parser.add_argument("--left-app", required=True, type=Path)
    parser.add_argument("--right-app", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        anchor = parse_anchor(args.anchor)
        result = {
            "anchor": str(args.anchor),
            "key_id": anchor["kSignerKeyId"].hex(),
            "anchor_sha256": hashlib.sha256(args.anchor.read_bytes()).hexdigest(),
            "left": verify_application(args.left_app, anchor),
            "right": verify_application(args.right_app, anchor),
        }
    except AnchorVerificationError as exc:
        parser.error(str(exc))
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
