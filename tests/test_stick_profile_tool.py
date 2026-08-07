from __future__ import annotations

import argparse
import contextlib
import csv
import importlib.util
import io
import math
import struct
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "stick_profile_tool", ROOT / "tools" / "stick_profile_tool.py"
)
assert SPEC and SPEC.loader
tool = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = tool
SPEC.loader.exec_module(tool)


def write_trace(path: Path, x: int, y: int, count: int = 16) -> None:
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=("timestamp_s", "endpoint", "report_hex")
        )
        writer.writeheader()
        for index in range(count):
            report = bytes((1, 0x11, 0x22, x, y, index & 0xFF, 0x66, 0x77))
            writer.writerow({
                "timestamp_s": f"{index * 0.004:.6f}",
                "endpoint": "0x84",
                "report_hex": report.hex(),
            })


def write_descriptors(root: Path) -> None:
    device = bytearray(18)
    device[0:2] = bytes((18, 1))
    device[8:10] = (0x054C).to_bytes(2, "little")
    device[10:12] = (0x0DF2).to_bytes(2, "little")
    device[12:14] = (0x0200).to_bytes(2, "little")
    (root / "device.bin").write_bytes(device)
    (root / "config.bin").write_bytes(bytes((9, 2, 9, 0, 1, 1, 0, 0x80, 50)))
    (root / "hid.bin").write_bytes(bytes(range(32)))


def guided_args(root: Path, report_length: int) -> argparse.Namespace:
    return argparse.Namespace(
        format_id="guided-test-v1", vid=0x054C, pid=0x0DF2,
        bcd_device=0x0200, interface=3, endpoint=0x84, report_id=1,
        report_length=report_length, format_version=1, variant=0,
        device_descriptor=root / "device.bin",
        configuration_descriptor=root / "config.bin",
        hid_report_descriptor=root / "hid.bin",
        guided_trace=root / "guided.csv",
    )


def write_guided_trace(path: Path, encoding: str = "uint8", *,
                       duplicate_axes: bool = False,
                       same_circle_direction: bool = False,
                       circle_turns: float = 2.0,
                       omit_marker: str | None = None,
                       axes_are_counters: bool = False) -> int:
    rows: list[tuple[str, float, float]] = []

    def add(marker: str, count: int, point) -> None:
        for index in range(count):
            x, y = point(index, count) if callable(point) else point
            rows.append((marker if index == 0 else "", x, y))

    add("neutral_begin", 16, (0.0, 0.0))
    add("right_begin", 12, lambda i, n: (0.92, 0.12 + ((i % 3) - 1) * .01))
    add("up_anchor", 12, lambda i, n: (0.13 + ((i % 3) - 1) * .01, 0.91))
    add("cw_begin", 80, lambda i, n: (
        math.cos(-2 * math.pi * circle_turns * i / n),
        math.sin(-2 * math.pi * circle_turns * i / n),
    ))
    direction = -1 if same_circle_direction else 1
    add("ccw_begin", 80, lambda i, n: (
        math.cos(direction * 2 * math.pi * circle_turns * i / n),
        math.sin(direction * 2 * math.pi * circle_turns * i / n),
    ))
    add("final_neutral_begin", 16, (0.0, 0.0))

    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=("timestamp_s", "endpoint", "report_hex", "marker")
        )
        writer.writeheader()
        for index, (marker, physical_x, physical_y) in enumerate(rows):
            if marker == omit_marker:
                marker = ""
            noise_x = ((index * 7) % 5) - 2
            noise_y = ((index * 11) % 5) - 2
            if encoding == "uint8":
                x = max(0, min(255, round(128 + physical_x * 112 + noise_x)))
                # Raw Y is deliberately reversed.
                y = max(0, min(255, round(128 - physical_y * 108 + noise_y)))
                if axes_are_counters:
                    x, y = (index * 3) & 0xFF, (index * 5) & 0xFF
                report = bytearray((1, 0x31, x, y, index & 0xFF, 0, 0x44, 0x55, 0x66, 0x77))
                report[5] = (sum(report[:5]) + index * 17) & 0xFF
                if duplicate_axes:
                    report[6], report[7] = x, y
            else:
                x = max(-32768, min(32767, round(physical_x * 29200 + noise_x * 35)))
                y = max(-32768, min(32767, round(physical_y * 28600 + noise_y * 35)))
                if axes_are_counters:
                    x, y = index * 97 - 12000, index * 131 - 16000
                report = bytearray(14)
                report[0:2] = bytes((1, 0x31))
                report[2:4] = struct.pack("<h", x)
                report[4:6] = struct.pack("<h", y)
                report[6:8] = struct.pack("<H", (index * 97) & 0xFFFF)
                report[8] = (sum(report[:8]) + index * 19) & 0xFF
                report[9:] = bytes((0x22, 0x33, 0x44, 0x55, 0x66))
                if duplicate_axes:
                    report[10:12] = struct.pack("<h", x)
                    report[12:14] = struct.pack("<h", y)
            writer.writerow({
                "timestamp_s": f"{index * 0.004:.6f}",
                "endpoint": "0x84",
                "report_hex": report.hex(),
                "marker": marker,
            })
    return 10 if encoding == "uint8" else 14


def upsample_guided_trace(source: Path, target: Path, factor: int = 4) -> None:
    """Repeat each 250 Hz report at 1/factor cadence without moving markers."""
    with source.open("r", encoding="utf-8", newline="") as handle:
        rows = list(csv.DictReader(handle))
    fieldnames = ("timestamp_s", "endpoint", "report_hex", "marker")
    base_step = 0.004
    with target.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            timestamp = float(row["timestamp_s"])
            for repeat in range(factor):
                upsampled = dict(row)
                upsampled["timestamp_s"] = f"{timestamp + base_step * repeat / factor:.6f}"
                if repeat:
                    upsampled["marker"] = ""
                writer.writerow(upsampled)


class ProfileToolTest(unittest.TestCase):
    def test_build_exact_uint8_profile(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            device = bytearray(18)
            device[0:2] = bytes((18, 1))
            device[8:10] = (0x054C).to_bytes(2, "little")
            device[10:12] = (0x0DF2).to_bytes(2, "little")
            device[12:14] = (0x0200).to_bytes(2, "little")
            (root / "device.bin").write_bytes(device)
            (root / "config.bin").write_bytes(bytes((9, 2, 9, 0, 1, 1, 0, 0x80, 50)))
            (root / "hid.bin").write_bytes(bytes(range(32)))
            write_trace(root / "neutral.csv", 128, 128)
            write_trace(root / "x_negative.csv", 0, 128)
            write_trace(root / "x_positive.csv", 255, 128)
            write_trace(root / "y_negative.csv", 128, 255)  # raw Y is reversed
            write_trace(root / "y_positive.csv", 128, 0)
            args = argparse.Namespace(
                format_id="dse-test-v1", cal_id="none", vid=0x054C,
                pid=0x0DF2, bcd_device=0x0200, interface=3,
                endpoint=0x84, report_id=1, report_length=8,
                format_version=1, variant=0, encoding="uint8",
                x_offset=3, y_offset=4,
                device_descriptor=root / "device.bin",
                configuration_descriptor=root / "config.bin",
                hid_report_descriptor=root / "hid.bin",
                neutral=root / "neutral.csv",
                x_negative=root / "x_negative.csv",
                x_positive=root / "x_positive.csv",
                y_negative=root / "y_negative.csv",
                y_positive=root / "y_positive.csv",
            )
            profile = tool.build_profile(args)
            self.assertEqual(profile["axes"]["x"]["sign"], 1)
            self.assertEqual(profile["axes"]["y"]["sign"], -1)
            self.assertEqual(profile["quality"]["gate"], "PASS")
            self.assertAlmostEqual(
                profile["quality"]["cadence"]["gap_p99_ms"], 4.0, places=6
            )
            tool.write_cpp_entry(profile, root / "profile.inc")
            self.assertIn("dse-test-v1", (root / "profile.inc").read_text())

    def test_guided_uint8_noisy_cross_axis_reversed_y(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            write_descriptors(root)
            length = write_guided_trace(root / "guided.csv")
            candidate = tool.build_guided_candidate(guided_args(root, length))
            self.assertEqual(candidate["layout"], {
                "encoding": "uint8", "x_offset": 2, "y_offset": 3,
                "x_sign": 1, "y_sign": -1,
            })
            self.assertEqual(
                candidate["publication"]["state"], "CANDIDATE_ONLY_REVIEW_REQUIRED"
            )
            self.assertFalse(candidate["publication"]["firmware_cpp_emitted"])
            self.assertFalse(
                candidate["axes"]["x"]["protocol_constants_proposed"]
                ["runtime_per_device_center_correction"]
            )
            self.assertGreater(
                candidate["qualification"]["anchor_evidence"]["x_cross_ratio"], .10
            )
            integrity_offsets = {
                item["offset"] for item in candidate["integrity"]
                ["suspected_counter_checksum_or_timestamp_fields"]
                if item["encoding"] == "uint8"
            }
            self.assertTrue({4, 5}.issubset(integrity_offsets))
            rejected = {
                item["offset"]: item["rejection_reasons"]
                for item in candidate["qualification"]["rejected_fields"]
                if item["encoding"] == "uint8"
            }
            self.assertTrue(any("counter_like" in reason for reason in rejected[4]))
            self.assertTrue(any("checksum_or_timestamp_like" in reason
                                for reason in rejected[5]))
            winding = candidate["qualification"]["ellipse_and_coupling_evidence"]
            self.assertGreater(abs(winding["cw_winding"]["signed_turns"]), 1.8)
            self.assertGreater(abs(winding["ccw_winding"]["signed_turns"]), 1.8)
            protocol = candidate["qualification"]["protocol_constant_evidence"]
            self.assertEqual(protocol["status"], "ENCODING_HYPOTHESIS_REVIEW_REQUIRED")
            self.assertLessEqual(
                candidate["qualification"]["decisions"]["protocol_constants"], .90
            )
            self.assertEqual(len(candidate["profile_sha256"]), 64)

    def test_guided_int16_le(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            write_descriptors(root)
            length = write_guided_trace(root / "guided.csv", "int16_le")
            candidate = tool.build_guided_candidate(guided_args(root, length))
            self.assertEqual(candidate["layout"]["encoding"], "int16_le")
            self.assertEqual(candidate["layout"]["x_offset"], 2)
            self.assertEqual(candidate["layout"]["y_offset"], 4)
            self.assertEqual(candidate["axes"]["x"]["protocol_constants_proposed"]["center"], 0)
            self.assertLess(candidate["axes"]["x"]["observed_device_evidence"]["center"], 100)

    def test_guided_circle_area_is_sample_rate_invariant(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            write_descriptors(root)
            base = root / "guided_250hz.csv"
            dense = root / "guided_1000hz.csv"
            length = write_guided_trace(base)
            upsample_guided_trace(base, dense)

            base_args = guided_args(root, length)
            base_args.guided_trace = base
            dense_args = guided_args(root, length)
            dense_args.guided_trace = dense
            base_candidate = tool.build_guided_candidate(base_args)
            dense_candidate = tool.build_guided_candidate(dense_args)

            self.assertEqual(base_candidate["layout"], dense_candidate["layout"])
            base_geometry = base_candidate["qualification"]["ellipse_and_coupling_evidence"]
            dense_geometry = dense_candidate["qualification"]["ellipse_and_coupling_evidence"]
            self.assertAlmostEqual(
                base_geometry["cw_signed_area"],
                dense_geometry["cw_signed_area"],
                places=3,
            )
            self.assertAlmostEqual(
                base_geometry["ccw_signed_area"],
                dense_geometry["ccw_signed_area"],
                places=3,
            )

    def test_guided_rejects_same_direction_circles(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            write_descriptors(root)
            length = write_guided_trace(
                root / "guided.csv", same_circle_direction=True
            )
            with self.assertRaisesRegex(tool.ProfileError, "no high-confidence"):
                tool.build_guided_candidate(guided_args(root, length))

    def test_guided_rejects_less_than_minimum_circle_turns(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            write_descriptors(root)
            length = write_guided_trace(root / "guided.csv", circle_turns=1.0)
            with self.assertRaisesRegex(tool.ProfileError, "no high-confidence"):
                tool.build_guided_candidate(guided_args(root, length))

    def test_guided_explicit_no_hid_report_descriptor(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            write_descriptors(root)
            length = write_guided_trace(root / "guided.csv")
            args = guided_args(root, length)
            args.hid_report_descriptor = None
            args.no_hid_report_descriptor = True
            candidate = tool.build_guided_candidate(args)
            self.assertFalse(candidate["identity"]["hid_report_present"])
            self.assertIsNone(candidate["identity"]["hid_report_sha256"])
            self.assertEqual(candidate["identity"]["hid_report_hash32"], 0)
            self.assertFalse(candidate["evidence"]["hid_report_present"])
            self.assertIsNone(candidate["evidence"]["hid_report_descriptor"])
            self.assertEqual(
                candidate["publication"]["state"], "CANDIDATE_ONLY_REVIEW_REQUIRED"
            )

    def test_legacy_mode_rejects_no_hid_report_descriptor(self) -> None:
        with self.assertRaisesRegex(tool.ProfileError, "legacy mode requires"):
            tool.build_profile(argparse.Namespace(hid_report_descriptor=None))

    def test_guided_output_and_hash_are_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            write_descriptors(root)
            length = write_guided_trace(root / "guided.csv")
            args = guided_args(root, length)
            first = tool.build_guided_candidate(args)
            second = tool.build_guided_candidate(args)
            self.assertEqual(first, second)
            unsigned = dict(first)
            digest = unsigned.pop("profile_sha256")
            self.assertEqual(digest, tool.canonical_hash(unsigned))

    def test_guided_rejects_counter_checksum_trace(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            write_descriptors(root)
            length = write_guided_trace(root / "guided.csv", axes_are_counters=True)
            with self.assertRaisesRegex(tool.ProfileError, "no high-confidence"):
                tool.build_guided_candidate(guided_args(root, length))

    def test_guided_rejects_ambiguous_duplicate_axes(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            write_descriptors(root)
            length = write_guided_trace(root / "guided.csv", duplicate_axes=True)
            with self.assertRaisesRegex(tool.ProfileError, "ambiguous axes"):
                tool.build_guided_candidate(guided_args(root, length))

    def test_guided_rejects_missing_marker_and_bad_circle(self) -> None:
        for case in ("missing", "bad_circle", "incomplete_circle"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                write_descriptors(root)
                length = write_guided_trace(
                    root / "guided.csv",
                    omit_marker="ccw_begin" if case == "missing" else None,
                    same_circle_direction=case == "bad_circle",
                    circle_turns=1.0 if case == "incomplete_circle" else 2.0,
                )
                expected = "missing required markers" if case == "missing" else "no high-confidence"
                with self.assertRaisesRegex(tool.ProfileError, expected):
                    tool.build_guided_candidate(guided_args(root, length))

    def test_guided_cli_rejects_out_cpp_without_emitting_files(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            write_descriptors(root)
            length = write_guided_trace(root / "guided.csv")
            out_json = root / "candidate.json"
            out_cpp = root / "profile.inc"
            argv = [
                "stick_profile_tool.py", "--format-id", "guided-test-v1",
                "--vid", "0x054C", "--pid", "0x0DF2", "--bcd-device", "0x0200",
                "--interface", "3", "--endpoint", "0x84", "--report-id", "1",
                "--report-length", str(length), "--device-descriptor", str(root / "device.bin"),
                "--configuration-descriptor", str(root / "config.bin"),
                "--hid-report-descriptor", str(root / "hid.bin"),
                "--guided-trace", str(root / "guided.csv"), "--out-json", str(out_json),
                "--out-cpp", str(out_cpp),
            ]
            output = io.StringIO()
            with mock.patch.object(sys, "argv", argv), contextlib.redirect_stdout(output):
                self.assertEqual(tool.main(), 2)
            self.assertIn("guided mode never emits firmware C++", output.getvalue())
            self.assertFalse(out_json.exists())
            self.assertFalse(out_cpp.exists())

    def test_guided_rejects_malformed_report_id_length_and_timestamp(self) -> None:
        for case in ("report_id", "length", "timestamp"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                write_descriptors(root)
                length = write_guided_trace(root / "guided.csv")
                with (root / "guided.csv").open("r", encoding="utf-8", newline="") as handle:
                    rows = list(csv.DictReader(handle))
                if case == "report_id":
                    data = bytearray.fromhex(rows[0]["report_hex"])
                    data[0] = 2
                    rows[0]["report_hex"] = data.hex()
                    expected = "report ID"
                elif case == "length":
                    rows[0]["report_hex"] = rows[0]["report_hex"][:-2]
                    expected = "report length"
                else:
                    rows[1]["timestamp_s"] = rows[0]["timestamp_s"]
                    expected = "strictly increasing"
                with (root / "guided.csv").open("w", encoding="utf-8", newline="") as handle:
                    writer = csv.DictWriter(
                        handle,
                        fieldnames=("timestamp_s", "endpoint", "report_hex", "marker"),
                    )
                    writer.writeheader()
                    writer.writerows(rows)
                with self.assertRaisesRegex(tool.ProfileError, expected):
                    tool.build_guided_candidate(guided_args(root, length))


if __name__ == "__main__":
    unittest.main()
