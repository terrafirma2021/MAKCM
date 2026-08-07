from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import promote_stick_profile as promote  # noqa: E402


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fixture(root: Path) -> tuple[Path, Path, Path]:
    device = bytearray(18)
    device[0:2] = bytes((18, 1))
    device[8:10] = (0x4131).to_bytes(2, "little")
    device[10:12] = (0x3519).to_bytes(2, "little")
    device[12:14] = (0x0100).to_bytes(2, "little")
    device_path = root / "device.bin"
    device_path.write_bytes(device)
    configuration = bytes((
        9, 2, 42, 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 1, 0xFF, 0x5D, 1, 0,
        17, 0x21, 0, 1, 1, 0x25, 0x81, 0x14, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        7, 5, 0x81, 3, 32, 0, 1,
    ))
    config_path = root / "configuration.bin"
    config_path.write_bytes(configuration)
    trace = root / "guided.csv"
    trace.write_text("evidence\n", encoding="utf-8")
    descriptor_digest = hashlib.sha256(device + configuration).hexdigest()
    candidate = {
        "schema": "makcm.controller-stick-adapter-candidate.v1",
        "format_id": "xinput-test-v1",
        "identity": {
            "vid": 0x4131,
            "pid": 0x3519,
            "bcd_device": 0x0100,
            "descriptor_bundle_sha256": descriptor_digest,
            "descriptor_hash32": int(descriptor_digest[:8], 16),
            "hid_report_present": False,
            "hid_report_sha256": None,
            "hid_report_hash32": 0,
            "interface_number": 0,
            "endpoint_address": 0x81,
            "report_id": 0,
            "report_length": 20,
            "format_version": 1,
            "variant": 0,
        },
        "layout": {
            "encoding": "int16_le", "x_offset": 10, "x_sign": 1,
            "y_offset": 12, "y_sign": 1,
        },
        "axes": {
            "x": {
                "offset": 10, "encoding": "int16_le", "sign": 1,
                "protocol_constants_proposed": {
                    "minimum": -32768, "center": 0, "maximum": 32767,
                },
            },
            "y": {
                "offset": 12, "encoding": "int16_le", "sign": 1,
                "protocol_constants_proposed": {
                    "minimum": -32768, "center": 0, "maximum": 32767,
                },
            },
        },
        "evidence": {
            "device_descriptor": str(device_path),
            "configuration_descriptor": str(config_path),
            "hid_report_descriptor": None,
            "guided_trace": str(trace),
            "guided_trace_sha256": sha(trace),
        },
        "qualification": {"decision_confidence": 0.96},
        "publication": {
            "state": "CANDIDATE_ONLY_REVIEW_REQUIRED",
            "review_required": True,
            "firmware_cpp_emitted": False,
        },
    }
    candidate["profile_sha256"] = hashlib.sha256(json.dumps(
        candidate, sort_keys=True, separators=(",", ":")
    ).encode()).hexdigest()
    candidate_path = root / "candidate.json"
    write_json(candidate_path, candidate)
    review = {
        "schema": "makcm.controller-stick-adapter-review.v1",
        "candidate": {
            "path": str(candidate_path),
            "file_sha256": sha(candidate_path),
            "profile_sha256": candidate["profile_sha256"],
            "publication_state": "CANDIDATE_ONLY_REVIEW_REQUIRED",
        },
        "identity": {
            "vid": "0x4131", "pid": "0x3519", "bcd_device": "0x0100",
            "target_interface": 0, "target_interface_class": "0xff",
            "endpoint": "0x81", "report_id": 0,
            "report_length": 20, "hid_report_present": False,
        },
        "layout_decision": {
            "encoding": "int16_le", "x_offset": 10, "x_sign": 1,
            "y_offset": 12, "y_sign": 1, "protocol_min": -32768,
            "protocol_center": 0, "protocol_max": 32767,
        },
        "protocol_constant_review": {
            "decision": "CANONICAL_SIGNED_INT16_CONTRACT_ACCEPTED",
        },
        "integrity_review": {
            "decision": "NO_RECOMPUTED_INTEGRITY_FIELD_OBSERVED_COPY_THROUGH",
            "stage3_confirmation_required": True,
        },
        "evidence": {
            "guided_csv": {"path": str(trace), "sha256": sha(trace)},
        },
        "decision": {
            "result": "CONDITIONAL_ACCEPT_FOR_PHASE3_LIMIT_0_20",
            "local_phase3_cpp_generation": True,
            "production_allow_list_publication": False,
        },
        "promotion": {
            "format_id": "xinput-test-v1", "cal_id": "none",
            "format_version": 1, "variant": 0,
        },
    }
    review_path = root / "review.json"
    write_json(review_path, review)
    return candidate_path, review_path, trace


class PromoteStickProfileTest(unittest.TestCase):
    def test_reviewed_no_hid_candidate_emits_deterministic_entry(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            candidate, review, _ = fixture(Path(folder))
            first = promote.build_entry(candidate, review)
            second = promote.build_entry(candidate, review)
            self.assertEqual(first, second)
            self.assertIn('"xinput-test-v1", "none"', first)
            self.assertIn("CodecFieldEncoding::Int16LE", first)
            self.assertIn("{10, 0, -32768, 32767, 1}", first)
            self.assertIn("{12, 0, -32768, 32767, 1}", first)
            self.assertTrue(first.rstrip().endswith("},"))
            self.assertIn("\n    false,\n},", first)

    def test_candidate_tamper_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            candidate, review, _ = fixture(Path(folder))
            value = json.loads(candidate.read_text(encoding="utf-8"))
            value["layout"]["x_offset"] = 9
            write_json(candidate, value)
            with self.assertRaisesRegex(promote.PromotionError,
                                        "canonical profile_sha256 mismatch"):
                promote.build_entry(candidate, review)

    def test_evidence_tamper_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            candidate, review, trace = fixture(Path(folder))
            trace.write_text("tampered\n", encoding="utf-8")
            with self.assertRaisesRegex(promote.PromotionError,
                                        "evidence hash mismatch"):
                promote.build_entry(candidate, review)

    def test_unresolved_review_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            candidate, review, _ = fixture(Path(folder))
            value = json.loads(review.read_text(encoding="utf-8"))
            value["integrity_review"]["decision"] = "UNRESOLVED"
            write_json(review, value)
            with self.assertRaisesRegex(promote.PromotionError,
                                        "integrity decision mismatch"):
                promote.build_entry(candidate, review)

    def test_no_hid_nonzero_hash_and_hid_interface_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            candidate, review, _ = fixture(root)
            value = json.loads(candidate.read_text(encoding="utf-8"))
            value["identity"]["hid_report_hash32"] = 1
            unsigned = dict(value)
            unsigned.pop("profile_sha256")
            value["profile_sha256"] = hashlib.sha256(json.dumps(
                unsigned, sort_keys=True, separators=(",", ":")
            ).encode()).hexdigest()
            write_json(candidate, value)
            review_value = json.loads(review.read_text(encoding="utf-8"))
            review_value["candidate"]["file_sha256"] = sha(candidate)
            review_value["candidate"]["profile_sha256"] = value["profile_sha256"]
            write_json(review, review_value)
            with self.assertRaisesRegex(promote.PromotionError,
                                        "no-HID hash32 mismatch"):
                promote.build_entry(candidate, review)


if __name__ == "__main__":
    unittest.main()
