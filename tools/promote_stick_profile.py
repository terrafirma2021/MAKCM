#!/usr/bin/env python3
"""Promote a reviewed guided adapter candidate into one generated C++ entry.

This is deliberately separate from ``stick_profile_tool.py``: guided analysis
always emits a review-only candidate, while this command requires an external
review document that is cryptographically bound to the candidate and every
referenced evidence file.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile


class PromotionError(RuntimeError):
    pass


SAFE_ID = re.compile(r"^[A-Za-z0-9_.:+-]{1,63}$")


def sha256_file(path: Path) -> str:
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError as exc:
        raise PromotionError(f"cannot read evidence {path}: {exc}") from exc


def read_bytes(path: Path, label: str) -> bytes:
    try:
        return path.read_bytes()
    except OSError as exc:
        raise PromotionError(f"cannot read {label} {path}: {exc}") from exc


def load_object(path: Path, label: str) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PromotionError(f"cannot load {label} {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise PromotionError(f"{label} must be a JSON object")
    return value


def canonical_candidate_hash(candidate: dict) -> str:
    unsigned = dict(candidate)
    claimed = unsigned.pop("profile_sha256", None)
    if not isinstance(claimed, str) or len(claimed) != 64:
        raise PromotionError("candidate profile_sha256 is missing or malformed")
    encoded = json.dumps(unsigned, sort_keys=True, separators=(",", ":")).encode()
    actual = hashlib.sha256(encoded).hexdigest()
    if actual != claimed:
        raise PromotionError("candidate canonical profile_sha256 mismatch")
    return actual


def integer(value: object, label: str, minimum: int, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise PromotionError(f"{label} must be an integer")
    if value < minimum or value > maximum:
        raise PromotionError(f"{label} outside [{minimum}, {maximum}]")
    return value


def exact(value: object, expected: object, label: str) -> None:
    if value != expected:
        raise PromotionError(f"{label} mismatch: {value!r} != {expected!r}")


def descriptor_stream(data: bytes):
    offset = 0
    while offset < len(data):
        if offset + 2 > len(data):
            raise PromotionError(f"truncated descriptor at offset {offset}")
        length = data[offset]
        if length < 2 or offset + length > len(data):
            raise PromotionError(f"invalid descriptor length at offset {offset}")
        yield data[offset:offset + length]
        offset += length


def validate_topology(configuration: bytes, interface_number: int,
                      endpoint_address: int, hid_present: bool) -> int:
    if len(configuration) < 9 or configuration[1] != 2:
        raise PromotionError("configuration descriptor is malformed")
    total = configuration[2] | (configuration[3] << 8)
    if total != len(configuration):
        raise PromotionError("configuration descriptor total length mismatch")
    current: int | None = None
    current_class: int | None = None
    target_class: int | None = None
    endpoint_found = False
    target_has_hid_descriptor = False
    for descriptor in descriptor_stream(configuration):
        if descriptor[1] == 4 and len(descriptor) >= 9:
            current = descriptor[2]
            current_class = descriptor[5]
            if current == interface_number:
                target_class = current_class
        elif (descriptor[1] == 0x21 and current == interface_number
              and current_class == 0x03):
            target_has_hid_descriptor = True
        elif descriptor[1] == 5 and len(descriptor) >= 7 and current == interface_number:
            if descriptor[2] == endpoint_address:
                if descriptor[3] & 0x03 != 0x03 or endpoint_address & 0x80 == 0:
                    raise PromotionError("target endpoint is not interrupt IN")
                endpoint_found = True
    if target_class is None or not endpoint_found:
        raise PromotionError("reviewed interface/endpoint not found in configuration")
    if hid_present:
        if target_class != 0x03 or not target_has_hid_descriptor:
            raise PromotionError("HID-present candidate does not target an HID interface")
    elif target_class == 0x03 or target_has_hid_descriptor:
        raise PromotionError("no-HID candidate targets an HID interface")
    return target_class


def cpp_array(hex_digest: str, label: str) -> str:
    try:
        data = bytes.fromhex(hex_digest)
    except ValueError as exc:
        raise PromotionError(f"{label} is not hex") from exc
    if len(data) != 32:
        raise PromotionError(f"{label} must contain 32 bytes")
    return "{" + ",".join(f"0x{byte:02X}" for byte in data) + "}"


def resolve_workspace_path(value: object, label: str) -> Path:
    if not isinstance(value, str) or not value:
        raise PromotionError(f"{label}.path must be a non-empty string")
    return Path(value)


def validate_evidence(review: dict) -> None:
    evidence = review.get("evidence")
    if not isinstance(evidence, dict) or not evidence:
        raise PromotionError("review evidence must be a non-empty object")
    for name, item in evidence.items():
        if not isinstance(item, dict):
            raise PromotionError(f"review evidence {name} must be an object")
        path = resolve_workspace_path(item.get("path"), f"evidence.{name}")
        digest = item.get("sha256")
        if not isinstance(digest, str) or len(digest) != 64:
            raise PromotionError(f"evidence.{name}.sha256 is malformed")
        if sha256_file(path) != digest.lower():
            raise PromotionError(f"evidence hash mismatch: {name}")


def axis_spec(candidate_axis: dict, review: dict, name: str,
              report_length: int) -> tuple[int, int, int, int, int]:
    if not isinstance(candidate_axis, dict):
        raise PromotionError(f"candidate axis {name} missing")
    proposed = candidate_axis.get("protocol_constants_proposed")
    if not isinstance(proposed, dict):
        raise PromotionError(f"candidate axis {name} lacks protocol constants")
    offset = integer(candidate_axis.get("offset"), f"{name}.offset", 0,
                     report_length - 1)
    center = integer(proposed.get("center"), f"{name}.center", -32768, 32767)
    minimum = integer(proposed.get("minimum"), f"{name}.minimum", -32768, 32767)
    maximum = integer(proposed.get("maximum"), f"{name}.maximum", -32768, 32767)
    sign = integer(candidate_axis.get("sign"), f"{name}.sign", -1, 1)
    if sign == 0 or not minimum < center < maximum:
        raise PromotionError(f"candidate axis {name} constants are invalid")
    exact(review.get(f"{name}_offset"), offset, f"review {name}_offset")
    exact(review.get(f"{name}_sign"), sign, f"review {name}_sign")
    exact(review.get("protocol_min"), minimum, "review protocol_min")
    exact(review.get("protocol_center"), center, "review protocol_center")
    exact(review.get("protocol_max"), maximum, "review protocol_max")
    return offset, center, minimum, maximum, sign


def build_entry(candidate_path: Path, review_path: Path) -> str:
    candidate = load_object(candidate_path, "candidate")
    review = load_object(review_path, "review")
    exact(candidate.get("schema"), "makcm.controller-stick-adapter-candidate.v1",
          "candidate schema")
    exact(review.get("schema"), "makcm.controller-stick-adapter-review.v1",
          "review schema")
    publication = candidate.get("publication")
    if not isinstance(publication, dict):
        raise PromotionError("candidate publication block missing")
    exact(publication.get("state"), "CANDIDATE_ONLY_REVIEW_REQUIRED",
          "candidate publication state")
    exact(publication.get("review_required"), True, "candidate review_required")
    exact(publication.get("firmware_cpp_emitted"), False,
          "candidate firmware_cpp_emitted")
    profile_sha = canonical_candidate_hash(candidate)

    review_candidate = review.get("candidate")
    if not isinstance(review_candidate, dict):
        raise PromotionError("review candidate binding missing")
    exact(Path(str(review_candidate.get("path"))).resolve(), candidate_path.resolve(),
          "review candidate path")
    exact(review_candidate.get("file_sha256"), sha256_file(candidate_path),
          "review candidate file hash")
    exact(review_candidate.get("profile_sha256"), profile_sha,
          "review candidate profile hash")

    decision = review.get("decision")
    if not isinstance(decision, dict):
        raise PromotionError("review decision missing")
    exact(decision.get("result"), "CONDITIONAL_ACCEPT_FOR_PHASE3_LIMIT_0_20",
          "review result")
    exact(decision.get("local_phase3_cpp_generation"), True,
          "review local phase3 generation flag")
    exact(decision.get("production_allow_list_publication"), False,
          "review production publication flag")
    integrity = review.get("integrity_review")
    protocol = review.get("protocol_constant_review")
    if not isinstance(integrity, dict) or not isinstance(protocol, dict):
        raise PromotionError("protocol/integrity review blocks are required")
    exact(integrity.get("decision"),
          "NO_RECOMPUTED_INTEGRITY_FIELD_OBSERVED_COPY_THROUGH",
          "integrity decision")
    exact(integrity.get("stage3_confirmation_required"), True,
          "integrity stage3 confirmation")
    exact(protocol.get("decision"), "CANONICAL_SIGNED_INT16_CONTRACT_ACCEPTED",
          "protocol constant decision")
    validate_evidence(review)

    qualification = candidate.get("qualification")
    if not isinstance(qualification, dict) or not isinstance(
            qualification.get("decision_confidence"), (int, float)):
        raise PromotionError("candidate qualification confidence missing")
    if float(qualification["decision_confidence"]) < 0.90:
        raise PromotionError("candidate decision confidence below promotion floor 0.90")

    identity = candidate.get("identity")
    layout = candidate.get("layout")
    axes = candidate.get("axes")
    reviewed_identity = review.get("identity")
    reviewed_layout = review.get("layout_decision")
    promotion = review.get("promotion")
    if not all(isinstance(value, dict) for value in
               (identity, layout, axes, reviewed_identity, reviewed_layout, promotion)):
        raise PromotionError("candidate/review identity or layout block missing")

    format_id = candidate.get("format_id")
    cal_id = promotion.get("cal_id")
    if not isinstance(format_id, str) or not SAFE_ID.fullmatch(format_id):
        raise PromotionError("unsafe candidate format_id")
    if not isinstance(cal_id, str) or not SAFE_ID.fullmatch(cal_id):
        raise PromotionError("unsafe promotion cal_id")
    exact(promotion.get("format_id"), format_id, "promotion format_id")

    vid = integer(identity.get("vid"), "identity.vid", 0, 0xFFFF)
    pid = integer(identity.get("pid"), "identity.pid", 0, 0xFFFF)
    bcd = integer(identity.get("bcd_device"), "identity.bcd_device", 0, 0xFFFF)
    interface = integer(identity.get("interface_number"), "identity.interface", 0, 255)
    endpoint = integer(identity.get("endpoint_address"), "identity.endpoint", 0, 255)
    report_id = integer(identity.get("report_id"), "identity.report_id", 0, 255)
    report_length = integer(identity.get("report_length"), "identity.report_length", 1, 1024)
    format_version = integer(identity.get("format_version"), "identity.format_version", 1, 0xFFFF)
    variant = integer(identity.get("variant"), "identity.variant", 0, 0xFFFF)
    exact(promotion.get("format_version"), format_version, "promotion format_version")
    exact(promotion.get("variant"), variant, "promotion variant")
    exact(reviewed_identity.get("target_interface"), interface, "review target interface")
    exact(reviewed_identity.get("endpoint"), f"0x{endpoint:02x}", "review endpoint")
    exact(reviewed_identity.get("report_id"), report_id, "review report_id")
    exact(reviewed_identity.get("report_length"), report_length, "review report_length")

    descriptor_hex = identity.get("descriptor_bundle_sha256")
    if not isinstance(descriptor_hex, str):
        raise PromotionError("descriptor bundle hash missing")
    descriptor_hash32 = integer(identity.get("descriptor_hash32"),
                                "descriptor_hash32", 0, 0xFFFFFFFF)
    exact(descriptor_hash32, int(descriptor_hex[:8], 16), "descriptor hash32")

    evidence = candidate.get("evidence")
    if not isinstance(evidence, dict):
        raise PromotionError("candidate evidence missing")
    device_path = Path(str(evidence.get("device_descriptor")))
    config_path = Path(str(evidence.get("configuration_descriptor")))
    device = read_bytes(device_path, "device descriptor")
    configuration = read_bytes(config_path, "configuration descriptor")
    descriptor_digest = hashlib.sha256(device + configuration).hexdigest()
    exact(descriptor_digest, descriptor_hex, "descriptor bundle evidence hash")
    if len(device) != 18 or device[1] != 1:
        raise PromotionError("device descriptor is malformed")
    exact(int.from_bytes(device[8:10], "little"), vid, "device VID")
    exact(int.from_bytes(device[10:12], "little"), pid, "device PID")
    exact(int.from_bytes(device[12:14], "little"), bcd, "device bcdDevice")
    exact(reviewed_identity.get("vid"), f"0x{vid:04x}", "review VID")
    exact(reviewed_identity.get("pid"), f"0x{pid:04x}", "review PID")
    exact(reviewed_identity.get("bcd_device"), f"0x{bcd:04x}",
          "review bcdDevice")

    guided_path = Path(str(evidence.get("guided_trace")))
    guided_digest = sha256_file(guided_path)
    exact(evidence.get("guided_trace_sha256"), guided_digest,
          "candidate guided trace hash")
    reviewed_guided = review["evidence"].get("guided_csv")
    if not isinstance(reviewed_guided, dict):
        raise PromotionError("review guided_csv evidence missing")
    exact(Path(str(reviewed_guided.get("path"))).resolve(), guided_path.resolve(),
          "review guided trace path")
    exact(reviewed_guided.get("sha256"), guided_digest,
          "review guided trace hash")

    hid_present = identity.get("hid_report_present")
    if not isinstance(hid_present, bool):
        raise PromotionError("identity hid_report_present must be boolean")
    exact(reviewed_identity.get("hid_report_present"), hid_present,
          "review hid_report_present")
    target_class = validate_topology(configuration, interface, endpoint, hid_present)
    exact(reviewed_identity.get("target_interface_class"),
          f"0x{target_class:02x}", "review target interface class")
    if hid_present:
        hid_hex = identity.get("hid_report_sha256")
        hid_hash32 = integer(identity.get("hid_report_hash32"), "hid hash32", 0, 0xFFFFFFFF)
        hid_path = Path(str(evidence.get("hid_report_descriptor")))
        exact(sha256_file(hid_path), hid_hex, "HID report evidence hash")
        exact(hid_hash32, int(str(hid_hex)[:8], 16), "HID report hash32")
    else:
        exact(identity.get("hid_report_sha256"), None, "no-HID SHA")
        exact(identity.get("hid_report_hash32"), 0, "no-HID hash32")
        exact(evidence.get("hid_report_descriptor"), None, "no-HID evidence path")
        hid_hex = "00" * 32
        hid_hash32 = 0

    encoding = layout.get("encoding")
    if encoding not in ("uint8", "int16_le"):
        raise PromotionError("unsupported layout encoding")
    exact(reviewed_layout.get("encoding"), encoding, "review encoding")
    x = axis_spec(axes.get("x"), reviewed_layout, "x", report_length)
    y = axis_spec(axes.get("y"), reviewed_layout, "y", report_length)
    width = 2 if encoding == "int16_le" else 1
    if x[0] + width > report_length or y[0] + width > report_length:
        raise PromotionError("axis field exceeds report length")
    if set(range(x[0], x[0] + width)) & set(range(y[0], y[0] + width)):
        raise PromotionError("axis fields overlap")

    codec = "CodecFieldEncoding::Int16LE" if encoding == "int16_le" \
        else "CodecFieldEncoding::UInt8"
    return f'''// Generated by tools/promote_stick_profile.py; do not hand-edit.
{{
    {json.dumps(format_id)}, {json.dumps(cal_id)},
    0x{vid:04X}, 0x{pid:04X}, 0x{bcd:04X},
    0x{descriptor_hash32:08X}, 0x{hid_hash32:08X},
    {cpp_array(descriptor_hex, "descriptor SHA")}, {cpp_array(str(hid_hex), "HID SHA")},
    {interface}, 0x{endpoint:02X}, 0x{report_id:02X}, {report_length},
    {format_version}, {variant}, {codec},
    {{{x[0]}, {x[1]}, {x[2]}, {x[3]}, {x[4]}}},
    {{{y[0]}, {y[1]}, {y[2]}, {y[3]}, {y[4]}}},
    {str(hid_present).lower()},
}},
'''


def write_atomic(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", newline="\n",
                                     dir=path.parent, delete=False) as handle:
        temporary = Path(handle.name)
        handle.write(text)
    try:
        os.replace(temporary, path)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--review", type=Path, required=True)
    parser.add_argument("--out-cpp", type=Path, required=True)
    args = parser.parse_args()
    try:
        entry = build_entry(args.candidate, args.review)
        write_atomic(args.out_cpp, entry)
    except PromotionError as exc:
        print(f"FAIL: {exc}")
        return 1
    print(f"PASS: generated reviewed profile entry {args.out_cpp}")
    print(f"sha256={sha256_file(args.out_cpp)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
