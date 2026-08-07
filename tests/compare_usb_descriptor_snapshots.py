#!/usr/bin/env python3
"""Compare two JSON files produced by usb_descriptor_snapshot.py."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    args = parser.parse_args()
    reference = json.loads(args.reference.read_text(encoding="utf-8"))
    candidate = json.loads(args.candidate.read_text(encoding="utf-8"))
    keys = sorted(set(reference) | set(candidate))
    mismatches = []
    for key in keys:
        if reference.get(key) != candidate.get(key):
            mismatches.append(key)
            print(f"DIFF {key}")
    if mismatches:
        print(f"FAIL: {len(mismatches)} descriptor groups differ")
        return 1
    print("PASS: raw descriptor groups match")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
