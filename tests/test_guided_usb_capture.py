from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import json
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "guided_usb_capture", ROOT / "tools" / "guided_usb_capture.py"
)
assert SPEC and SPEC.loader
capture = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = capture
SPEC.loader.exec_module(capture)


BASE = 1_700_000_000
MARKER_INDICES = (0, 2, 4, 6, 8, 10)


def tshark_row(index: int, *, urb_type: str = "C", bus: str = "1",
               device: str = "7", endpoint: str = "0x84",
               urb_id: str | None = None, data_len: str = "8",
               capdata: str | None = None, timestamp: str | None = None) -> dict[str, str]:
    payload = bytes((1, 0x20, 0x80 + index, 0x80 - index, index, 0x55, 0x66, 0x77))
    return {
        "frame.time_epoch": timestamp or f"{BASE}.{10 + index * 10:03d}",
        "usb.urb_type": urb_type,
        "usb.bus_id": bus,
        "usb.device_address": device,
        "usb.endpoint_address": endpoint,
        "usb.urb_id": urb_id or f"0x{0x1000 + index:X}",
        "usb.data_len": data_len,
        "usb.capdata": payload.hex(":") if capdata is None else capdata,
    }


def write_tsv(path: Path, rows: list[dict[str, str]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=capture.TSHARK_FIELDS, delimiter="\t",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(rows)


def usbpcap_packet(
    irp_id: int,
    *,
    info: int = 1,
    bus: int = 1,
    device: int = 7,
    endpoint: int = 0x84,
    transfer: int = 1,
    payload: bytes = b"\x01\x20\x80\x80\x00\x55\x66\x77",
    extra_header: bytes = b"",
    data_length: int | None = None,
    header_len: int | None = None,
) -> bytes:
    actual_header_len = 27 + len(extra_header) if header_len is None else header_len
    base = struct.pack(
        "<HQIHBHHBBI",
        actual_header_len,
        irp_id,
        0,
        9,
        info,
        bus,
        device,
        endpoint,
        transfer,
        len(payload) if data_length is None else data_length,
    )
    return base + extra_header + payload


def write_pcap(
    path: Path,
    packets: list[tuple[int, int, bytes]],
    *,
    magic: bytes = b"\xd4\xc3\xb2\xa1",
    linktype: int = 249,
    snaplen: int = 65535,
    captured_adjust: int = 0,
    original_adjust: int = 0,
) -> None:
    global_header = magic + struct.pack("<HHiIII", 2, 4, 0, 0, snaplen, linktype)
    data = bytearray(global_header)
    for seconds, micros, packet in packets:
        captured = len(packet) + captured_adjust
        original = len(packet) + original_adjust
        data.extend(struct.pack("<IIII", seconds, micros, captured, original))
        data.extend(packet)
    path.write_bytes(bytes(data))


def guided_packets(
    *,
    bus: int = 1,
    device: int = 7,
    endpoint: int = 0x84,
    count: int = 300,
) -> list[tuple[int, int, bytes]]:
    packets = []
    for index in range(count):
        micros = index * 1000
        packets.append((
            BASE + micros // 1_000_000,
            micros % 1_000_000,
            usbpcap_packet(
                irp_id=0x1000 + index,
                bus=bus,
                device=device,
                endpoint=endpoint,
                payload=bytes((1, 0x20, index & 0xFF, 0x80, 0, 0x55, 0x66, 0x77)),
            ),
        ))
    return packets


def discovery_packets(observed_s: float = 9.8) -> list[tuple[int, int, bytes]]:
    packets = []
    for index in range(101):
        point = observed_s * index / 100
        seconds = BASE + int(point)
        micros = int(round((point - int(point)) * 1_000_000))
        if micros == 1_000_000:
            seconds += 1
            micros = 0
        packets.append((
            seconds,
            micros,
            usbpcap_packet(
                irp_id=0x8000 + index,
                payload=bytes((index & 0xFF, 1, 2, 3, 4, 5, 6, 7)),
            ),
        ))
    return packets


def target_runner(command, **kwargs):
    if "--extcap-version" in command:
        return subprocess.CompletedProcess(command, 0, "extcap {version=1.5.4.0}\n", "")
    if "--extcap-interfaces" in command:
        return subprocess.CompletedProcess(
            command, 0, "interface {value=\\\\.\\USBPcap1}{display=USBPcap1}\n", ""
        )
    if "--extcap-config" in command:
        return subprocess.CompletedProcess(
            command, 0,
            "value {arg=5}{value=7}{display=[7] Controller}{enabled=true}\n", "",
        )
    if "--version" in command:
        return subprocess.CompletedProcess(command, 0, "TShark 4.6.7\n", "")
    return subprocess.CompletedProcess(command, 0, b"", b"")


class FakeCaptureProcess:
    def __init__(self, pid: int = 4242) -> None:
        self.pid = pid
        self.returncode = None

    def poll(self):
        return self.returncode

    def wait(self, timeout=None):
        deadline = None if timeout is None else time.monotonic() + timeout
        while self.returncode is None:
            if deadline is not None and time.monotonic() >= deadline:
                raise subprocess.TimeoutExpired(["USBPcapCMD"], timeout)
            time.sleep(0.001)
        return self.returncode

    def terminate(self):
        self.returncode = -15


def valid_rows(count: int = 12) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for index in range(count):
        rows.append(tshark_row(index, urb_type="S", capdata=""))
        rows.append(tshark_row(index, urb_type="0x43" if index % 2 else "C"))
    rows.append(dict(rows[7]))  # repeated exported completion for URB 0x1003
    rows.append(tshark_row(20, device="8"))
    rows.append(tshark_row(21, endpoint="0x83"))
    return rows


def marker_mapping() -> dict[str, str]:
    return {
        name: f"{BASE}.{9 + index * 10:03d}"
        for name, index in zip(capture.REQUIRED_MARKERS, MARKER_INDICES)
    }


def write_markers(path: Path, markers=None) -> None:
    path.write_text(json.dumps(markers if markers is not None else marker_mapping()),
                    encoding="utf-8")


def inspection_rows() -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []

    def add_group(bus: int, device: int, endpoint: int, length: int,
                  count: int, urb_base: int, variable_offset: int) -> None:
        for index in range(count):
            payload = bytearray([0x40 + offset for offset in range(length)])
            payload[variable_offset] += index
            row = tshark_row(
                index, bus=str(bus), device=str(device), endpoint=f"0x{endpoint:02X}",
                urb_id=f"0x{urb_base + index:X}",
                timestamp=f"{BASE}.{10 + index * 10:03d}",
            )
            row["usb.data_len"] = str(length)
            row["usb.capdata"] = payload.hex(":")
            rows.append(row)

    add_group(1, 7, 0x84, 8, 5, 0x2000, 2)
    add_group(1, 7, 0x82, 4, 3, 0x3000, 1)
    add_group(1, 8, 0x84, 6, 4, 0x4000, 4)
    rows.append(tshark_row(30, urb_type="S", capdata="", urb_id="0x5000"))
    rows.append(tshark_row(31, urb_type="E", capdata="", urb_id="0x5001"))
    rows.append(tshark_row(32, data_len="0", capdata="", urb_id="0x5002"))
    rows.append(tshark_row(33, endpoint="0x04", urb_id="0x5003"))
    rows.append(dict(rows[0]))  # exact duplicate completion
    conflict = dict(rows[0])
    conflict["usb.capdata"] = "FF:" + conflict["usb.capdata"][3:]
    rows.append(conflict)
    malformed = tshark_row(34, urb_id="0x5004")
    malformed["usb.device_address"] = ""
    rows.append(malformed)
    return rows


def descriptor_snapshot(interface_reports: dict[int, bytes] | None = None,
                        *, vendor_only: bool = False,
                        vendor_interfaces: tuple[int, ...] = ()) -> dict:
    device = bytearray(18)
    device[0:2] = bytes((18, 1))
    device[8:10] = (0x4131).to_bytes(2, "little")
    device[10:12] = (0x3519).to_bytes(2, "little")
    device[12:14] = (0x0100).to_bytes(2, "little")
    reports = interface_reports or {}
    if vendor_only:
        vendor_interfaces = tuple(sorted(set(vendor_interfaces) | {0}))
    interface_numbers = sorted(set(reports) | set(vendor_interfaces))
    configuration = bytearray((9, 2, 0, 0, len(interface_numbers), 1, 0, 0x80, 50))
    for interface in interface_numbers:
        interface_class = 0x03 if interface in reports else 0xFF
        configuration.extend((9, 4, interface, 0, 1, interface_class, 0, 0, 0))
        if interface in reports:
            report_length = len(reports[interface])
            configuration.extend((
                9, 0x21, 0x11, 0x01, 0, 1, 0x22,
                report_length & 0xFF, report_length >> 8,
            ))
        configuration.extend((7, 5, 0x80 | (interface + 1), 3, 64, 0, 4))
    configuration[2:4] = len(configuration).to_bytes(2, "little")
    return {
        "vid": "4131",
        "pid": "3519",
        "device": device.hex(),
        "configuration": configuration.hex(),
        "topology": capture._configuration_topology(bytes(configuration)),
        "strings": {},
        "hid_reports": {str(interface): report.hex()
                        for interface, report in reports.items()},
        "bos": None,
        "qualifier": None,
        "errors": [],
    }


def write_snapshot(path: Path, document: dict) -> None:
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def preflight_args(root: Path) -> argparse.Namespace:
    return argparse.Namespace(
        usbpcapcmd=None,
        tshark=None,
        snapshot_script=root / "usb_descriptor_snapshot.py",
        out_json=root / "preflight.json",
    )


def ready_preflight_dependencies(root: Path):
    args = preflight_args(root)
    args.snapshot_script.write_text("# read-only snapshot helper\n", encoding="utf-8")
    usbpcapcmd = Path(r"C:\Program Files\USBPcap\USBPcapCMD.exe")
    tshark = Path(r"C:\Program Files\Wireshark\tshark.exe")
    calls: list[list[str]] = []

    def finder(name, explicit):
        return usbpcapcmd if name == "usbpcapcmd" else tshark

    def runner(command, **kwargs):
        calls.append(command)
        if command[-1] == "--extcap-interfaces":
            return subprocess.CompletedProcess(
                command, 0,
                "interface {value=\\\\.\\USBPcap2}{display=USBPcap2}\n", "",
            )
        if command[-1] == "--extcap-version":
            return subprocess.CompletedProcess(command, 0, "extcap {version=1.5.4.0}\n", "")
        name = Path(command[0]).stem
        return subprocess.CompletedProcess(command, 0, f"{name} 4.4.1\n", "")

    modules = {
        "usb.core": SimpleNamespace(),
        "libusb_package": SimpleNamespace(get_libusb1_backend=lambda: object()),
    }

    def importer(name):
        return modules[name]

    return args, finder, runner, importer, calls


class GuidedUsbCaptureTest(unittest.TestCase):
    def test_usbpcapcmd_version_uses_extcap_protocol(self) -> None:
        executable = Path(r"C:\USBPcap\USBPcapCMD.exe")
        completed = subprocess.CompletedProcess(
            [str(executable), "--extcap-version"],
            0,
            "extcap {version=1.5.4.0}{help=http://desowin.org/usbpcap/}\n",
            "",
        )
        with mock.patch.object(capture.subprocess, "run", return_value=completed) as run:
            self.assertEqual(capture.usbpcapcmd_version(executable), completed.stdout.strip())
        run.assert_called_once_with(
            [str(executable), "--extcap-version"],
            check=True,
            capture_output=True,
            text=True,
        )
        old = subprocess.CompletedProcess(
            [str(executable), "--extcap-version"],
            0,
            "extcap {version=1.5.3.0}\n",
            "",
        )
        with mock.patch.object(capture.subprocess, "run", return_value=old), \
                self.assertRaisesRegex(capture.CaptureError, "require >= 1.5.4.0"):
            capture.usbpcapcmd_version(executable)

    def test_preflight_all_ready_is_read_only_and_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            args, finder, runner, importer, calls = ready_preflight_dependencies(root)
            first = capture.run_preflight(
                args, finder=finder, runner=runner, importer=importer
            )
            second = capture.run_preflight(
                args, finder=finder, runner=runner, importer=importer
            )
            self.assertEqual(first, second)
            self.assertEqual(first["status"], "READY")
            self.assertEqual(first["exit_code"], capture.PREFLIGHT_READY)
            self.assertEqual(first["required_for_capture"]["status"], "PASS")
            self.assertEqual(first["required_for_descriptor"]["status"], "PASS")
            self.assertEqual(
                first["required_for_capture"]["checks"][2]["interfaces"],
                [r"\\.\USBPcap2"],
            )
            self.assertEqual(first["policy"], {
                "read_only": True,
                "auto_install_performed": False,
                "device_selected": None,
                "endpoint_selected": None,
                "next_step_automatic": False,
            })
            self.assertTrue(all(
                command[-1] in ("--version", "--extcap-version", "--extcap-interfaces")
                for command in calls
            ))
            self.assertFalse(any(
                "install" in argument.lower() for command in calls for argument in command
            ))
            summary = capture.render_preflight_summary(first)
            self.assertIn("Required for live capture: PASS", summary)
            self.assertIn("no installation", summary)

    def test_preflight_missing_capture_dependencies_is_never_live_pass(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            args = preflight_args(root)
            args.snapshot_script.write_text("# helper\n", encoding="utf-8")
            modules = {
                "usb.core": SimpleNamespace(),
                "libusb_package": SimpleNamespace(get_libusb1_backend=lambda: object()),
            }
            runner = mock.Mock()
            document = capture.run_preflight(
                args,
                finder=lambda name, explicit: (_ for _ in ()).throw(
                    capture.CaptureError(f"{name} missing")
                ),
                runner=runner,
                importer=lambda name: modules[name],
            )
            self.assertEqual(document["status"], "CAPTURE_REQUIREMENTS_MISSING")
            self.assertEqual(document["required_for_capture"]["status"], "FAIL")
            self.assertEqual(document["required_for_descriptor"]["status"], "PASS")
            self.assertEqual(document["exit_code"], capture.PREFLIGHT_CAPTURE_MISSING)
            runner.assert_not_called()

    def test_preflight_no_usbpcap_interface_fails_capture_group(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            args, finder, _, importer, _ = ready_preflight_dependencies(root)

            def runner(command, **kwargs):
                if command[-1] == "--extcap-interfaces":
                    output = ""
                elif command[-1] == "--extcap-version":
                    output = "extcap {version=1.5.4.0}\n"
                else:
                    output = "Wireshark 4.4.1\n"
                return subprocess.CompletedProcess(command, 0, output, "")

            document = capture.run_preflight(
                args, finder=finder, runner=runner, importer=importer
            )
            interface = document["required_for_capture"]["checks"][2]
            self.assertEqual(interface["status"], "FAIL")
            self.assertEqual(interface["interfaces"], [])
            self.assertIn("USBPcap", interface["error"])
            self.assertEqual(document["exit_code"], capture.PREFLIGHT_CAPTURE_MISSING)

    def test_preflight_descriptor_missing_and_both_missing_exit_codes(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            args, finder, runner, _, _ = ready_preflight_dependencies(root)

            def missing_import(name):
                raise ModuleNotFoundError(f"{name} unavailable")

            descriptor_missing = capture.run_preflight(
                args, finder=finder, runner=runner, importer=missing_import
            )
            self.assertEqual(
                descriptor_missing["status"], "DESCRIPTOR_REQUIREMENTS_MISSING"
            )
            self.assertEqual(
                descriptor_missing["exit_code"], capture.PREFLIGHT_DESCRIPTOR_MISSING
            )

            args.snapshot_script = root / "missing_snapshot.py"
            both = capture.run_preflight(
                args,
                finder=lambda name, explicit: (_ for _ in ()).throw(
                    capture.CaptureError(f"{name} unavailable")
                ),
                runner=mock.Mock(),
                importer=missing_import,
            )
            self.assertEqual(
                both["status"], "CAPTURE_AND_DESCRIPTOR_REQUIREMENTS_MISSING"
            )
            self.assertEqual(both["exit_code"], capture.PREFLIGHT_BOTH_MISSING)

    def test_doctor_alias_writes_json_and_returns_structured_nonzero_code(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            args, finder, runner, importer, _ = ready_preflight_dependencies(root)
            document = capture.run_preflight(
                args, finder=finder, runner=runner, importer=importer
            )
            document["required_for_descriptor"]["status"] = "FAIL"
            document["status"] = "DESCRIPTOR_REQUIREMENTS_MISSING"
            document["exit_code"] = capture.PREFLIGHT_DESCRIPTOR_MISSING
            output = root / "doctor.json"
            with mock.patch.object(capture, "run_preflight", return_value=document):
                result = capture.main([
                    "doctor", "--snapshot-script", str(args.snapshot_script),
                    "--out-json", str(output),
                ])
            self.assertEqual(result, capture.PREFLIGHT_DESCRIPTOR_MISSING)
            self.assertEqual(json.loads(output.read_text(encoding="utf-8")), document)

    def test_descriptor_unpack_one_hid_and_deterministic_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            snapshot = descriptor_snapshot({0: bytes((0x05, 0x01, 0x09, 0x05))})
            write_snapshot(root / "snapshot.json", snapshot)
            first = capture.unpack_descriptor_snapshot(
                root / "snapshot.json", root / "out-a", target_interface=0
            )
            second = capture.unpack_descriptor_snapshot(
                root / "snapshot.json", root / "out-b", target_interface=0
            )
            self.assertTrue(first["hid_report_present"])
            self.assertEqual(first["hid_interface"], 0)
            self.assertEqual(first, second)
            self.assertEqual(
                (root / "out-a" / "manifest.json").read_bytes(),
                (root / "out-b" / "manifest.json").read_bytes(),
            )
            self.assertEqual(
                (root / "out-a" / "device.bin").read_bytes(),
                bytes.fromhex(snapshot["device"]),
            )
            self.assertEqual(
                (root / "out-a" / "configuration.bin").read_bytes(),
                bytes.fromhex(snapshot["configuration"]),
            )
            self.assertEqual(
                (root / "out-a" / "hid_report_if0.bin").read_bytes(),
                bytes.fromhex(snapshot["hid_reports"]["0"]),
            )

    def test_descriptor_unpack_multiple_hid_uses_explicit_target_interface(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            snapshot = descriptor_snapshot({
                0: bytes((0x05, 0x01, 0x09, 0x05)),
                1: bytes((0x06, 0x00, 0xFF, 0x09, 0x01)),
            })
            write_snapshot(root / "snapshot.json", snapshot)
            manifest = capture.unpack_descriptor_snapshot(
                root / "snapshot.json", root / "selected", target_interface=1
            )
            self.assertEqual(manifest["available_hid_interfaces"], [0, 1])
            self.assertEqual(manifest["hid_interface"], 1)
            self.assertTrue((root / "selected" / "hid_report_if1.bin").exists())
            self.assertFalse((root / "selected" / "hid_report_if0.bin").exists())

    def test_descriptor_unpack_no_hid_vendor_snapshot_emits_no_fake_file(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            snapshot = descriptor_snapshot(vendor_only=True)
            write_snapshot(root / "snapshot.json", snapshot)
            manifest = capture.unpack_descriptor_snapshot(
                root / "snapshot.json", root / "xbox-style", target_interface=0
            )
            self.assertFalse(manifest["hid_report_present"])
            self.assertIsNone(manifest["hid_interface"])
            self.assertEqual(manifest["available_hid_interfaces"], [])
            self.assertIsNone(manifest["files"]["hid_report"])
            self.assertEqual(list((root / "xbox-style").glob("hid_report*.bin")), [])

    def test_descriptor_unpack_malformed_hex_length_and_topology_are_atomic(self) -> None:
        for case in ("hex", "length", "topology"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                snapshot = descriptor_snapshot({0: bytes((0x05, 0x01, 0x09, 0x05))})
                if case == "hex":
                    snapshot["device"] = "GG"
                    expected = "hexadecimal"
                elif case == "length":
                    configuration = bytearray.fromhex(snapshot["configuration"])
                    configuration[2:4] = (len(configuration) + 1).to_bytes(2, "little")
                    snapshot["configuration"] = configuration.hex()
                    expected = "declared total"
                else:
                    snapshot["topology"]["declared_interfaces"] = 99
                    expected = "topology"
                write_snapshot(root / "snapshot.json", snapshot)
                with self.assertRaisesRegex(capture.CaptureError, expected):
                    capture.unpack_descriptor_snapshot(
                        root / "snapshot.json", root / "output", target_interface=0
                    )
                self.assertFalse((root / "output").exists())

    def test_descriptor_composite_vendor_target_never_binds_mouse_hid(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            mouse_report = bytes((0x05, 0x01, 0x09, 0x02, 0xA1, 0x01))
            snapshot = descriptor_snapshot({1: mouse_report}, vendor_interfaces=(0,))
            write_snapshot(root / "snapshot.json", snapshot)
            vendor = capture.unpack_descriptor_snapshot(
                root / "snapshot.json", root / "vendor-target", target_interface=0
            )
            self.assertEqual(vendor["target_interface"], 0)
            self.assertEqual(vendor["target_interface_class"], 0xFF)
            self.assertFalse(vendor["hid_report_present"])
            self.assertEqual(vendor["non_target_hid_interfaces"], [1])
            self.assertEqual(vendor["non_target_hid_reports"], [
                {"interface": 1, "present": True, "length": len(mouse_report)}
            ])
            self.assertIsNone(vendor["files"]["hid_report"])
            self.assertEqual(list((root / "vendor-target").glob("hid_report*.bin")), [])

            mouse = capture.unpack_descriptor_snapshot(
                root / "snapshot.json", root / "mouse-target", target_interface=1
            )
            self.assertEqual(mouse["target_interface_class"], 0x03)
            self.assertTrue(mouse["hid_report_present"])
            self.assertEqual(
                (root / "mouse-target" / "hid_report_if1.bin").read_bytes(), mouse_report
            )

    def test_descriptor_target_hid_missing_error_and_nonexistent_interface_fail(self) -> None:
        for case in ("missing", "error", "nonexistent"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                report = bytes((0x05, 0x01, 0x09, 0x02))
                snapshot = descriptor_snapshot({1: report}, vendor_interfaces=(0,))
                target = 9 if case == "nonexistent" else 1
                if case in ("missing", "error"):
                    snapshot["hid_reports"] = {}
                if case == "error":
                    snapshot["errors"] = ["hid_report:1:control transfer failed"]
                write_snapshot(root / "snapshot.json", snapshot)
                expected = {
                    "missing": "report bytes are missing",
                    "error": "retrieval failed",
                    "nonexistent": "does not exist",
                }[case]
                with self.assertRaisesRegex(capture.CaptureError, expected):
                    capture.unpack_descriptor_snapshot(
                        root / "snapshot.json", root / "output", target_interface=target
                    )
                self.assertFalse((root / "output").exists())

    def test_inspect_ranks_groups_and_reports_variation_cadence_and_exclusions(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            write_tsv(root / "inspect.tsv", inspection_rows())
            document = capture.inspect_tshark_tsv(root / "inspect.tsv")
            self.assertEqual(document["selection"], "NONE_HUMAN_REVIEW_REQUIRED")
            self.assertEqual(len(document["candidates"]), 3)
            first = document["candidates"][0]
            self.assertEqual(
                (first["bus"], first["device"], first["endpoint"],
                 first["report_length"], first["report_count"]),
        (1, 7, 0x84, 8, 5),
            )
            self.assertEqual(first["cadence_ms"], {
                "samples": 4, "p50": 10.0, "p95": 10.0, "maximum": 10.0,
            })
            self.assertEqual(first["unique_payload_count"], 5)
            self.assertEqual(first["variable_byte_count"], 1)
            self.assertEqual(first["byte_variation"][2]["range"], 4)
            self.assertEqual(document["counts"]["submit_rows"], 1)
            self.assertEqual(document["counts"]["error_rows"], 1)
            self.assertEqual(document["counts"]["no_payload_completions"], 1)
            self.assertEqual(document["counts"]["out_payload_completions"], 1)
            self.assertEqual(document["counts"]["deduplicated_completions"], 1)
            self.assertEqual(document["counts"]["conflicting_urb_rows"], 1)
            self.assertEqual(document["counts"]["malformed_rows"], 1)
            summary = capture.render_inspection_summary(document)
            self.assertIn("no endpoint or axis was selected", summary)
            self.assertIn("device 8", summary)

    def test_inspection_json_is_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            write_tsv(root / "inspect.tsv", inspection_rows())
            first = capture.inspect_tshark_tsv(root / "inspect.tsv")
            second = capture.inspect_tshark_tsv(root / "inspect.tsv")
            capture.write_inspection_json(first, root / "a.json")
            capture.write_inspection_json(second, root / "b.json")
            self.assertEqual(first, second)
            self.assertEqual((root / "a.json").read_bytes(), (root / "b.json").read_bytes())

    def test_valid_completion_only_conversion_dedup_and_filtering(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            write_tsv(root / "capture.tsv", valid_rows())
            write_markers(root / "markers.json")
            audit = capture.convert_tshark_tsv(
                root / "capture.tsv", root / "markers.json", bus=1, device=7,
                endpoint=0x84, out_csv=root / "guided.csv",
                audit_json=root / "audit.json", max_marker_lag_ms=5,
            )
            self.assertEqual(audit["selection"], {"bus": 1, "device": 7, "endpoint": 0x84})
            self.assertEqual(audit["counts"]["source_rows"], 27)
            self.assertEqual(audit["counts"]["kept_completions"], 12)
            self.assertEqual(audit["counts"]["deduplicated_completions"], 1)
            self.assertEqual(audit["counts"]["non_complete"], 12)
            self.assertEqual(audit["counts"]["filtered_selection"], 2)
            with (root / "guided.csv").open("r", encoding="utf-8", newline="") as handle:
                reports = list(csv.DictReader(handle))
            self.assertEqual(len(reports), 12)
            self.assertEqual(reports[0]["timestamp_s"], "0.000")
            self.assertEqual(reports[1]["timestamp_s"], "0.010")
            self.assertTrue(all(row["endpoint"] == "0x84" for row in reports))
            self.assertEqual(
                [reports[index]["marker"] for index in MARKER_INDICES],
                list(capture.REQUIRED_MARKERS),
            )
            self.assertTrue(all(value == 1.0 for value in audit["marker_lag_ms"].values()))
            self.assertEqual(audit["capture_epoch_origin_s"], f"{BASE}.010")

    def test_marker_lag_collision_and_no_following_report_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            write_tsv(root / "capture.tsv", valid_rows())
            reports, _ = capture.parse_tshark_tsv(root / "capture.tsv", 1, 7, 0x84)
            write_markers(root / "markers.json")
            markers = capture.load_markers(root / "markers.json")
            with self.assertRaisesRegex(capture.CaptureError, "exceeds"):
                capture.merge_markers(reports, markers, .5)

            collision = list(markers)
            collision[1] = capture.Marker(collision[1].name, reports[2].timestamp - capture.Decimal(".0005"))
            collision[0] = capture.Marker(collision[0].name, reports[2].timestamp - capture.Decimal(".001"))
            with self.assertRaisesRegex(capture.CaptureError, "collide"):
                capture.merge_markers(reports, collision, 5)

            no_following = list(markers)
            no_following[-1] = capture.Marker(no_following[-1].name,
                                                reports[-1].timestamp + capture.Decimal(".001"))
            with self.assertRaisesRegex(capture.CaptureError, "no following"):
                capture.merge_markers(reports, no_following, 5)

    def test_malformed_capdata_data_len_and_time_rejected(self) -> None:
        for case in ("capdata", "data_len", "time"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                rows = valid_rows()
                target = next(row for row in rows if row["usb.urb_type"] == "C")
                if case == "capdata":
                    target["usb.capdata"] = "01:GG"
                    expected = "capdata"
                elif case == "data_len":
                    target["usb.data_len"] = "9"
                    expected = "data_len"
                else:
                    target["frame.time_epoch"] = "NaN"
                    expected = "finite"
                write_tsv(root / "capture.tsv", rows)
                with self.assertRaisesRegex(capture.CaptureError, expected):
                    capture.parse_tshark_tsv(root / "capture.tsv", 1, 7, 0x84)

    def test_missing_duplicate_and_out_of_order_markers_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            missing = marker_mapping()
            missing.pop("cw_begin")
            write_markers(root / "missing.json", missing)
            with self.assertRaisesRegex(capture.CaptureError, "missing required"):
                capture.load_markers(root / "missing.json")

            duplicate = [
                {"marker": name, "epoch_ns": int(float(value) * 1_000_000_000)}
                for name, value in marker_mapping().items()
            ]
            duplicate.append(dict(duplicate[0]))
            write_markers(root / "duplicate.json", {"clock": "unix_epoch_ns",
                                                     "markers": duplicate})
            with self.assertRaisesRegex(capture.CaptureError, "duplicate"):
                capture.load_markers(root / "duplicate.json")

            unordered = marker_mapping()
            unordered["right_begin"], unordered["up_begin"] = (
                unordered["up_begin"], unordered["right_begin"]
            )
            write_markers(root / "unordered.json", unordered)
            with self.assertRaisesRegex(capture.CaptureError, "out of order"):
                capture.load_markers(root / "unordered.json")

    def test_csv_and_audit_hashes_are_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            write_tsv(root / "capture.tsv", valid_rows())
            write_markers(root / "markers.json")
            audits = []
            for suffix in ("a", "b"):
                audits.append(capture.convert_tshark_tsv(
                    root / "capture.tsv", root / "markers.json", bus=1, device=7,
                    endpoint=0x84, out_csv=root / f"{suffix}.csv",
                    audit_json=root / f"{suffix}.json", max_marker_lag_ms=5,
                    tshark_command=("tshark", "-r", "capture.pcap"),
                    tshark_version="TShark 4.4.0",
                ))
            self.assertEqual((root / "a.csv").read_bytes(), (root / "b.csv").read_bytes())
            self.assertEqual((root / "a.json").read_bytes(), (root / "b.json").read_bytes())
            self.assertEqual(audits[0], audits[1])
            self.assertEqual(
                audits[0]["output_csv_sha256"], hashlib.sha256((root / "a.csv").read_bytes()).hexdigest()
            )

    def test_live_command_construction_and_interface_listing_are_mocked(self) -> None:
        usbpcapcmd = Path(r"C:\Program Files\USBPcap\USBPcapCMD.exe")
        tshark = Path(r"C:\Program Files\Wireshark\tshark.exe")
        command = capture.build_usbpcapcmd_capture_command(
            usbpcapcmd, r"\\.\USBPcap1", Path("raw.pcap"), 180, device=7
        )
        self.assertEqual(
            command,
            [str(usbpcapcmd), "-d", r"\\.\USBPcap1", "-o", "raw.pcap",
             "--devices", "7", "--inject-descriptors"],
        )
        self.assertNotIn("dumpcap", " ".join(command).lower())
        export = capture.build_tshark_export_command(
            tshark, Path("raw.pcap"), bus=1, device=7, endpoint=0x84
        )
        self.assertEqual(export.count("-e"), len(capture.TSHARK_FIELDS))
        self.assertIn("usb.capdata", export)
        display_filter = export[export.index("-Y") + 1]
        self.assertIn("usb.irp_info.direction == 1", display_filter)
        self.assertIn("usb.transfer_type == 1", display_filter)
        self.assertIn("usb.bus_id == 1", display_filter)
        self.assertIn("usb.device_address == 7", display_filter)
        self.assertIn("usb.endpoint_address == 0x84", display_filter)
        self.assertIn("usb.data_len > 0", display_filter)

        completed = subprocess.CompletedProcess(
            [str(usbpcapcmd), "--extcap-interfaces"],
            0,
            "interface {value=\\\\.\\USBPcap1}{display=USBPcap1}\n",
            "",
        )
        with mock.patch.object(capture.subprocess, "run", return_value=completed) as run:
            self.assertIn("USBPcap1", capture.list_capture_interfaces(usbpcapcmd))
            run.assert_called_once_with(
                [str(usbpcapcmd), "--extcap-interfaces"],
                check=True,
                capture_output=True,
                text=True,
            )

    def _legacy_live_wrapper_uses_mocked_dumpcap_and_tshark(self) -> None:
        class FakeProcess:
            def __init__(self) -> None:
                self.returncode = None

            def poll(self):
                return self.returncode

            def terminate(self) -> None:
                self.returncode = -15

            def wait(self, timeout=None):
                return self.returncode

            def kill(self) -> None:
                self.returncode = -9

        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source_tsv = root / "source.tsv"
            write_tsv(source_tsv, valid_rows(24))
            raw_capture = root / "raw.pcap"
            args = argparse.Namespace(
                usbpcapcmd=None, tshark=None, list_interfaces=False,
                interface=r"\\.\USBPcap1", bus="1", device="7", endpoint="0x84",
                duration_s=180, raw_capture=raw_capture, markers_json=root / "markers.json",
                tsv=root / "export.tsv", out_csv=root / "guided.csv",
                audit_json=root / "audit.json", max_marker_lag_ms=5.0,
            )
            process = FakeProcess()
            calls: list[list[str]] = []

            def fake_run(command, **kwargs):
                calls.append(command)
                if "--version" in command:
                    return subprocess.CompletedProcess(command, 0, "TShark 4.4.0\n", "")
                kwargs["stdout"].write(source_tsv.read_bytes())
                return subprocess.CompletedProcess(command, 0, "", "")

            epoch_values = iter(
                BASE * 1_000_000_000 + (9 + index * 10) * 1_000_000
                for index in (0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 23)
            )
            usbpcapcmd = Path(r"C:\USBPcap\USBPcapCMD.exe")
            tshark = Path(r"C:\Wireshark\tshark.exe")
            with mock.patch.object(
                capture, "find_usbpcapcmd_executable", return_value=usbpcapcmd,
            ), mock.patch.object(
                capture, "find_wireshark_executable", return_value=tshark,
            ), mock.patch.object(capture.subprocess, "Popen", return_value=process) as popen, \
                    mock.patch.object(capture.subprocess, "run", side_effect=fake_run):
                audit = capture.run_live(
                    args, input_fn=lambda prompt: "", time_ns_fn=lambda: next(epoch_values)
                )
            self.assertIsNotNone(audit)
            self.assertTrue(raw_capture.exists())
            self.assertTrue(args.markers_json.exists())
            self.assertTrue(args.out_csv.exists())
            self.assertEqual(audit["tshark"]["version"], "TShark 4.4.0")
            self.assertEqual(audit["input"]["raw_capture_path"], str(raw_capture))
            self.assertEqual(
                audit["input"]["raw_capture_sha256"],
                hashlib.sha256(raw_capture.read_bytes()).hexdigest(),
            )
            marker_document = json.loads(args.markers_json.read_text(encoding="utf-8"))
            self.assertEqual(
                [item["marker"] for item in marker_document["markers"]],
                list(capture.MARKER_ORDER),
            )
            with args.out_csv.open("r", encoding="utf-8", newline="") as handle:
                reports = list(csv.DictReader(handle))
            marker_rows = {row["marker"]: index for index, row in enumerate(reports) if row["marker"]}
            self.assertGreaterEqual(
                marker_rows["final_neutral_end"] - marker_rows["final_neutral_begin"], 3
            )
            popen_command = popen.call_args.args[0]
            self.assertIn(r"\\.\USBPcap1", popen_command)
            self.assertEqual(calls[0].count("-e"), len(capture.TSHARK_FIELDS))
            self.assertIn("-Y", calls[0])

    def _legacy_aborted_live_capture_retains_raw_and_markers_without_derived_outputs(self) -> None:
        class FakeProcess:
            returncode = None

            def poll(self):
                return self.returncode

            def terminate(self):
                self.returncode = -15

            def wait(self, timeout=None):
                return self.returncode

            def kill(self):
                self.returncode = -9

        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            raw_capture = root / "aborted.pcap"
            args = argparse.Namespace(
                usbpcapcmd=None, tshark=None, list_interfaces=False,
                interface=r"\\.\USBPcap1", bus="1", device="7", endpoint="0x84",
                duration_s=180, raw_capture=raw_capture, markers_json=root / "markers.json",
                tsv=root / "export.tsv", out_csv=root / "guided.csv",
                audit_json=root / "audit.json", max_marker_lag_ms=5.0,
            )
            prompts = 0

            def abort_on_second_prompt(prompt):
                nonlocal prompts
                prompts += 1
                if prompts == 2:
                    raise KeyboardInterrupt
                return ""

            with mock.patch.object(
                capture, "find_usbpcapcmd_executable",
                return_value=Path(r"C:\USBPcap\USBPcapCMD.exe"),
            ), mock.patch.object(
                capture, "find_wireshark_executable",
                return_value=Path(r"C:\Wireshark\tshark.exe"),
            ), mock.patch.object(capture.subprocess, "Popen", return_value=FakeProcess()), \
                    mock.patch.object(capture.subprocess, "run") as run:
                with self.assertRaises(KeyboardInterrupt):
                    capture.run_live(
                        args, input_fn=abort_on_second_prompt,
                        time_ns_fn=lambda: BASE * 1_000_000_000,
                    )
            self.assertTrue(raw_capture.exists())
            self.assertTrue(args.markers_json.exists())
            markers = json.loads(args.markers_json.read_text(encoding="utf-8"))["markers"]
            self.assertEqual([item["marker"] for item in markers], ["neutral_begin"])
            self.assertFalse(args.tsv.exists())
            self.assertFalse(args.out_csv.exists())
            self.assertFalse(args.audit_json.exists())
            run.assert_not_called()

    def _legacy_discovery_command_and_abort_retain_raw_without_json(self) -> None:
        usbpcapcmd = Path(r"C:\USBPcap\USBPcapCMD.exe")
        discovery_command = capture.build_discovery_usbpcapcmd_command(
            usbpcapcmd, r"\\.\USBPcap1", Path("discover.pcap"), 8
        )
        self.assertEqual(
            discovery_command,
            [str(usbpcapcmd), "-d", r"\\.\USBPcap1", "-o", "discover.pcap",
             "-A", "--inject-descriptors"],
        )
        command = capture.build_discovery_tshark_command(
            Path(r"C:\Wireshark\tshark.exe"), Path("discover.pcap")
        )
        display_filter = command[command.index("-Y") + 1]
        self.assertIn("usb.irp_info.direction == 1", display_filter)
        self.assertIn("usb.data_len > 0", display_filter)
        self.assertNotIn("usb.bus_id ==", display_filter)
        self.assertNotIn("usb.device_address ==", display_filter)
        self.assertEqual(command.count("-e"), len(capture.TSHARK_FIELDS))

        class AbortingProcess:
            def __init__(self):
                self.returncode = None
                self.wait_calls = 0

            def wait(self, timeout=None):
                self.wait_calls += 1
                if self.wait_calls == 1:
                    raise KeyboardInterrupt
                return self.returncode

            def poll(self):
                return self.returncode

            def terminate(self):
                self.returncode = -15

            def kill(self):
                self.returncode = -9

        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            raw_capture = root / "discover.pcap"
            args = argparse.Namespace(
                usbpcapcmd=None, tshark=None, interface=r"\\.\USBPcap1", duration_s=8,
                raw_capture=raw_capture, tsv=root / "discover.tsv",
                out_json=root / "discover.json",
            )
            with mock.patch.object(
                capture, "find_usbpcapcmd_executable",
                return_value=usbpcapcmd,
            ), mock.patch.object(
                capture, "find_wireshark_executable",
                return_value=Path(r"C:\Wireshark\tshark.exe"),
            ), mock.patch.object(
                capture.subprocess, "Popen", return_value=AbortingProcess()
            ), mock.patch.object(capture.subprocess, "run") as run:
                with self.assertRaises(KeyboardInterrupt):
                    capture.run_discover(args, input_fn=lambda prompt: "")
            self.assertTrue(raw_capture.exists())
            self.assertFalse(args.tsv.exists())
            self.assertFalse(args.out_json.exists())
            run.assert_not_called()

    def _legacy_mocked_discovery_writes_hashed_read_only_report(self) -> None:
        class FinishedProcess:
            returncode = None

            def wait(self, timeout=None):
                self.returncode = 0
                return 0

            def poll(self):
                return self.returncode

            def terminate(self):
                self.returncode = -15

            def kill(self):
                self.returncode = -9

        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source_tsv = root / "source.tsv"
            write_tsv(source_tsv, inspection_rows())
            raw_capture = root / "discover.pcap"
            args = argparse.Namespace(
                usbpcapcmd=None, tshark=None, interface=r"\\.\USBPcap2", duration_s=10,
                raw_capture=raw_capture, tsv=root / "discover.tsv",
                out_json=root / "discover.json",
            )
            calls: list[list[str]] = []

            def fake_run(command, **kwargs):
                calls.append(command)
                if "--version" in command:
                    return subprocess.CompletedProcess(command, 0, "TShark 4.4.1\n", "")
                kwargs["stdout"].write(source_tsv.read_bytes())
                return subprocess.CompletedProcess(command, 0, "", "")

            with mock.patch.object(
                capture, "find_usbpcapcmd_executable",
                return_value=Path(r"C:\USBPcap\USBPcapCMD.exe"),
            ), mock.patch.object(
                capture, "find_wireshark_executable",
                return_value=Path(r"C:\Wireshark\tshark.exe"),
            ), mock.patch.object(
                capture.subprocess, "Popen", return_value=FinishedProcess()
            ) as popen, mock.patch.object(capture.subprocess, "run", side_effect=fake_run):
                document = capture.run_discover(args, input_fn=lambda prompt: "")
            self.assertTrue(args.out_json.exists())
            self.assertEqual(document["selection"], "NONE_HUMAN_REVIEW_REQUIRED")
            self.assertEqual(len(document["candidates"]), 3)
            self.assertEqual(
                document["capture"]["raw_capture_sha256"],
                hashlib.sha256(raw_capture.read_bytes()).hexdigest(),
            )
            self.assertEqual(
                document["capture"]["tshark_tsv_sha256"],
                hashlib.sha256(args.tsv.read_bytes()).hexdigest(),
            )
            self.assertEqual(document["capture"]["tshark_version"], "TShark 4.4.1")
            self.assertIn("-Y", document["capture"]["tshark_command"])
            self.assertIn("-A", popen.call_args.args[0])
            self.assertEqual(calls[0].count("-e"), len(capture.TSHARK_FIELDS))

    def test_usbpcap_parser_selects_completion_interrupt_payload_without_dissector(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            raw = root / "direct.pcap"
            protocol_payload = bytes.fromhex("01020304AABBCCDDEEFF")
            packets = [
                (BASE, 1, usbpcap_packet(
                    irp_id=1, info=0, payload=protocol_payload
                )),
                (BASE, 2, usbpcap_packet(
                    irp_id=1, info=1, payload=protocol_payload
                )),
                (BASE, 3, usbpcap_packet(
                    irp_id=2, info=1, transfer=2, payload=b"control"
                )),
                (BASE, 4, usbpcap_packet(
                    irp_id=3, info=1, endpoint=0x04, payload=b"out"
                )),
            ]
            write_pcap(raw, packets)
            parsed = capture.parse_classic_usbpcap(raw)
            selected = capture.payload_completions(
                parsed, bus=1, device=7, endpoint=0x84
            )
            self.assertEqual(len(selected), 1)
            self.assertEqual(selected[0].payload, protocol_payload)
            self.assertEqual(selected[0].irp_id, 1)
            self.assertEqual(parsed.observed_duration, capture.Decimal("0.000003"))

    def test_usbpcap_parser_rejects_magic_linktype_truncation_and_length_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            packet = usbpcap_packet(irp_id=1)
            cases: list[tuple[str, dict, str]] = [
                ("big.pcap", {"magic": b"\xa1\xb2\xc3\xd4"}, "byte order"),
                ("wrong-link.pcap", {"linktype": 1}, "linktype"),
                (
                    "data-mismatch.pcap",
                    {"packets": [(BASE, 0, usbpcap_packet(irp_id=1, data_length=9))]},
                    "dataLength",
                ),
                (
                    "header-mismatch.pcap",
                    {"packets": [(BASE, 0, usbpcap_packet(irp_id=1, header_len=40))]},
                    "headerLen",
                ),
            ]
            for name, options, expected in cases:
                path = root / name
                packets = options.pop("packets", [(BASE, 0, packet)])
                write_pcap(path, packets, **options)
                with self.subTest(name=name), self.assertRaisesRegex(
                    capture.CaptureError, expected
                ):
                    capture.parse_classic_usbpcap(path)

            truncated = root / "truncated.pcap"
            write_pcap(truncated, [(BASE, 0, packet)])
            truncated.write_bytes(truncated.read_bytes()[:-1])
            with self.assertRaisesRegex(capture.CaptureError, "truncated"):
                capture.parse_classic_usbpcap(truncated)

            record_mismatch = root / "record-mismatch.pcap"
            write_pcap(
                record_mismatch,
                [(BASE, 0, packet)],
                captured_adjust=0,
                original_adjust=1,
            )
            with self.assertRaisesRegex(capture.CaptureError, "is truncated"):
                capture.parse_classic_usbpcap(record_mismatch)

    def test_version_interface_device_and_selection_ranges_fail_closed(self) -> None:
        with self.assertRaisesRegex(capture.CaptureError, "require >= 1.5.4.0"):
            capture.require_usbpcap_version("extcap {version=1.5.3.0}")
        self.assertEqual(
            capture.require_usbpcap_version("extcap {version=1.5.4.0}"),
            (1, 5, 4, 0),
        )
        for selection in ((0, 7, 0x84), (1, 0, 0x84), (1, 128, 0x84),
                          (1, 7, 0x80), (1, 7, 0x90)):
            with self.subTest(selection=selection), self.assertRaises(capture.CaptureError):
                capture.validate_capture_selection(*selection)

        executable = Path(r"C:\USBPcap\USBPcapCMD.exe")

        def missing_device_runner(command, **kwargs):
            result = target_runner(command, **kwargs)
            if "--extcap-config" in command:
                return subprocess.CompletedProcess(
                    command, 0,
                    "value {arg=5}{value=8}{display=[8] Other}{enabled=true}\n", "",
                )
            return result

        with self.assertRaisesRegex(capture.CaptureError, "not attached"):
            capture.verify_capture_target(
                executable, r"\\.\USBPcap1", device=7, runner=missing_device_runner
            )

        def missing_interface_runner(command, **kwargs):
            result = target_runner(command, **kwargs)
            if "--extcap-interfaces" in command:
                return subprocess.CompletedProcess(command, 0, "", "")
            return result

        with self.assertRaisesRegex(capture.CaptureError, "not present"):
            capture.verify_capture_target(
                executable, r"\\.\USBPcap1", device=7, runner=missing_interface_runner
            )

    def test_cli_accepts_only_pcap_and_stale_output_is_rejected(self) -> None:
        parser = capture.build_parser()
        args = parser.parse_args(["live", "--pcap", "fresh.pcap"])
        self.assertEqual(args.raw_capture, Path("fresh.pcap"))
        with self.assertRaises(SystemExit):
            parser.parse_args(["live", "--pcapng", "wrong.pcapng"])
        with tempfile.TemporaryDirectory() as folder:
            raw = Path(folder) / "stale.pcap"
            raw.write_bytes(b"old evidence")
            with self.assertRaisesRegex(capture.CaptureError, "already exists"):
                capture._prepare_raw_capture(raw)
            with self.assertRaisesRegex(capture.CaptureError, ".pcap extension"):
                capture._prepare_raw_capture(Path(folder) / "wrong.pcapng")

    def test_live_direct_parser_controlled_stop_and_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            raw = root / "guided.pcap"
            args = argparse.Namespace(
                usbpcapcmd=None, tshark=None, list_interfaces=False,
                interface=r"\\.\USBPcap1", bus="1", device="7", endpoint="0x84",
                duration_s=180, raw_capture=raw, markers_json=root / "markers.json",
                tsv=root / "guided.tsv", out_csv=root / "guided.csv",
                audit_json=root / "guided.audit.json", max_marker_lag_ms=5.0,
            )
            process = FakeCaptureProcess()

            def popen(command):
                self.assertFalse(raw.exists())
                write_pcap(raw, guided_packets())
                return process

            def stop(proc):
                proc.returncode = 1
                return {
                    "controlled": True,
                    "method": "taskkill /PID /T /F",
                    "tree_pids": [proc.pid, proc.pid + 1],
                    "exit_code": 1,
                }

            epochs = iter([
                (BASE - 1) * 1_000_000_000,
                *(
                    BASE * 1_000_000_000 + (9 + index * 20) * 1_000_000
                    for index in range(12)
                ),
            ])
            monotonic = iter((10.0, 10.5))
            with mock.patch.object(
                capture, "find_usbpcapcmd_executable",
                return_value=Path(r"C:\USBPcap\USBPcapCMD.exe"),
            ), mock.patch.object(
                capture, "find_wireshark_executable",
                return_value=Path(r"C:\Wireshark\tshark.exe"),
            ):
                audit = capture.run_live(
                    args,
                    input_fn=lambda prompt: "",
                    time_ns_fn=lambda: next(epochs),
                    monotonic_fn=lambda: next(monotonic),
                    runner=target_runner,
                    popen_factory=popen,
                    stop_fn=stop,
                    snapshot_fn=lambda: {},
                    post_marker_grace_s=0,
                )
            self.assertEqual(audit["capture"]["parser_schema"], capture.USBPCAP_PARSER_SCHEMA)
            self.assertTrue(audit["capture"]["controlled_stop"])
            self.assertEqual(audit["capture"]["exit_code"], 1)
            self.assertEqual(audit["capture"]["device"], 7)
            self.assertEqual(audit["capture"]["filter"]["endpoint"], "0x84")
            self.assertEqual(audit["capture"]["usbpcapcmd_version"],
                             "extcap {version=1.5.4.0}")
            self.assertEqual(audit["input"]["raw_capture_sha256"], hashlib.sha256(
                raw.read_bytes()).hexdigest())
            self.assertEqual(audit["tshark"]["command"][-2:], ["-c", "1"])

    def test_live_natural_failure_and_blocked_input_watchdog_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)

            def args_for(name):
                return argparse.Namespace(
                    usbpcapcmd=None, tshark=None, list_interfaces=False,
                    interface=r"\\.\USBPcap1", bus="1", device="7", endpoint="0x84",
                    duration_s=180, raw_capture=root / f"{name}.pcap",
                    markers_json=root / f"{name}.markers.json",
                    tsv=root / f"{name}.tsv", out_csv=root / f"{name}.csv",
                    audit_json=root / f"{name}.json", max_marker_lag_ms=5.0,
                )

            with mock.patch.object(
                capture, "find_usbpcapcmd_executable",
                return_value=Path(r"C:\USBPcap\USBPcapCMD.exe"),
            ), mock.patch.object(
                capture, "find_wireshark_executable",
                return_value=Path(r"C:\Wireshark\tshark.exe"),
            ):
                natural = FakeCaptureProcess()
                natural.returncode = 1
                with self.assertRaisesRegex(capture.CaptureError, "exited early"):
                    capture.run_live(
                        args_for("natural"),
                        runner=target_runner,
                        popen_factory=lambda command: natural,
                        snapshot_fn=lambda: {},
                        post_marker_grace_s=0,
                    )

                watchdog_args = args_for("watchdog")
                stopped = threading.Event()
                watchdog_process = FakeCaptureProcess()

                def popen(command):
                    write_pcap(watchdog_args.raw_capture, guided_packets())
                    return watchdog_process

                def stop(proc):
                    proc.returncode = 1
                    stopped.set()
                    return {
                        "controlled": True, "method": "taskkill /T /F",
                        "tree_pids": [proc.pid], "exit_code": 1,
                    }

                started = time.monotonic()
                with self.assertRaises(capture.CaptureError):
                    capture.run_live(
                        watchdog_args,
                        input_fn=lambda prompt: stopped.wait(2),
                        runner=target_runner,
                        popen_factory=popen,
                        stop_fn=stop,
                        snapshot_fn=lambda: {},
                        watchdog_timeout_s=0.05,
                        post_marker_grace_s=0,
                    )
                self.assertLess(time.monotonic() - started, 1.0)
                self.assertTrue(stopped.is_set())
                self.assertFalse(watchdog_args.out_csv.exists())

    def test_discovery_controlled_duration_gate_and_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)

            def run_case(name: str, observed_s: float, expect_success: bool):
                raw = root / f"{name}.pcap"
                args = argparse.Namespace(
                    usbpcapcmd=None, tshark=None, interface=r"\\.\USBPcap1",
                    duration_s=10, raw_capture=raw, tsv=root / f"{name}.tsv",
                    out_json=root / f"{name}.json",
                )
                process = FakeCaptureProcess(pid=5000 if expect_success else 5001)

                def popen(command):
                    write_pcap(raw, discovery_packets(observed_s))
                    return process

                def stop(proc):
                    proc.returncode = 1
                    return {
                        "controlled": True, "method": "taskkill /PID /T /F",
                        "tree_pids": [proc.pid], "exit_code": 1,
                    }

                kwargs = dict(
                    input_fn=lambda prompt: "",
                    time_ns_fn=lambda: (BASE - 1) * 1_000_000_000,
                    monotonic_fn=iter((1.0, 1.1)).__next__,
                    runner=target_runner,
                    popen_factory=popen,
                    stop_fn=stop,
                    snapshot_fn=lambda: {},
                    watchdog_timeout_s=0.02,
                )
                if expect_success:
                    return capture.run_discover(args, **kwargs)
                with self.assertRaisesRegex(capture.CaptureError, "covers only"):
                    capture.run_discover(args, **kwargs)
                return None

            with mock.patch.object(
                capture, "find_usbpcapcmd_executable",
                return_value=Path(r"C:\USBPcap\USBPcapCMD.exe"),
            ), mock.patch.object(
                capture, "find_wireshark_executable",
                return_value=Path(r"C:\Wireshark\tshark.exe"),
            ):
                document = run_case("complete", 9.8, True)
                run_case("short", 1.49, False)
            self.assertEqual(document["capture"]["status"], "PASS")
            self.assertTrue(document["capture"]["controlled_stop"])
            self.assertEqual(document["capture"]["exit_code"], 1)
            self.assertEqual(document["capture"]["pcap_observed_duration_s"], "9.8")
            self.assertGreater(document["candidates"][0]["report_count"], 100)

    def test_discovery_readiness_delays_running_prompt_and_duration_watchdog(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            raw = root / "delayed.pcap"
            source = root / "source.pcap"
            write_pcap(source, discovery_packets())
            full_pcap = source.read_bytes()
            source.unlink()
            args = argparse.Namespace(
                usbpcapcmd=None, tshark=None, interface=r"\\.\USBPcap1",
                duration_s=10, raw_capture=raw, tsv=root / "delayed.tsv",
                out_json=root / "delayed.json",
            )
            process = FakeCaptureProcess(pid=5100)
            writer_threads = []
            ready_seen = threading.Event()
            ready_times = []
            stop_times = []
            printed = []
            real_ready = capture._ready_usbpcap_prefix

            def observed_ready(path):
                result = real_ready(path)
                if result is not None and not ready_seen.is_set():
                    ready_times.append(time.monotonic())
                    ready_seen.set()
                return result

            def popen(command):
                # A valid global header and partial first record are not readiness.
                raw.write_bytes(full_pcap[:44])

                def finish_first_record():
                    time.sleep(0.04)
                    raw.write_bytes(full_pcap)

                thread = threading.Thread(target=finish_first_record)
                thread.start()
                writer_threads.append(thread)
                return process

            def stop(proc):
                stop_times.append(time.monotonic())
                proc.returncode = 1
                return {
                    "controlled": True, "method": "taskkill /PID /T /F",
                    "tree_pids": [proc.pid], "exit_code": 1,
                }

            def record_print(*values, **kwargs):
                printed.append((time.monotonic(), " ".join(str(value) for value in values)))

            with mock.patch.object(
                capture, "find_usbpcapcmd_executable",
                return_value=Path(r"C:\USBPcap\USBPcapCMD.exe"),
            ), mock.patch.object(
                capture, "find_wireshark_executable",
                return_value=Path(r"C:\Wireshark\tshark.exe"),
            ), mock.patch.object(
                capture, "_ready_usbpcap_prefix", side_effect=observed_ready,
            ), mock.patch("builtins.print", side_effect=record_print):
                document = capture.run_discover(
                    args,
                    input_fn=lambda prompt: "",
                    time_ns_fn=lambda: (BASE - 1) * 1_000_000_000,
                    monotonic_fn=iter((1.0, 1.2)).__next__,
                    runner=target_runner,
                    popen_factory=popen,
                    stop_fn=stop,
                    snapshot_fn=lambda: {},
                    watchdog_timeout_s=0.04,
                    readiness_timeout_s=0.5,
                )
            for thread in writer_threads:
                thread.join(timeout=1)
            running_times = [
                at for at, text in printed if text.startswith("Discovery capture running:")
            ]
            self.assertTrue(ready_seen.is_set())
            self.assertEqual(len(running_times), 1)
            self.assertGreaterEqual(running_times[0], ready_times[0])
            self.assertGreaterEqual(stop_times[0] - ready_times[0], 0.02)
            self.assertEqual(document["capture"]["status"], "PASS")
            self.assertEqual(
                document["capture"]["readiness_first_record_epoch_s"],
                f"{BASE}.000000".rstrip("0").rstrip("."),
            )

    def test_live_does_not_accept_markers_before_pcap_readiness(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            raw = root / "delayed-guided.pcap"
            source = root / "source.pcap"
            write_pcap(source, guided_packets())
            full_pcap = source.read_bytes()
            source.unlink()
            args = argparse.Namespace(
                usbpcapcmd=None, tshark=None, list_interfaces=False,
                interface=r"\\.\USBPcap1", bus="1", device="7", endpoint="0x84",
                duration_s=180, raw_capture=raw,
                markers_json=root / "markers.json", tsv=root / "guided.tsv",
                out_csv=root / "guided.csv", audit_json=root / "guided.json",
                max_marker_lag_ms=5.0,
            )
            process = FakeCaptureProcess(pid=5200)
            ready_seen = threading.Event()
            writer_threads = []
            input_readiness = []
            running_readiness = []
            real_ready = capture._ready_usbpcap_prefix

            def observed_ready(path):
                result = real_ready(path)
                if result is not None:
                    ready_seen.set()
                return result

            def popen(command):
                raw.write_bytes(full_pcap[:44])

                def finish_first_record():
                    time.sleep(0.04)
                    raw.write_bytes(full_pcap)

                thread = threading.Thread(target=finish_first_record)
                thread.start()
                writer_threads.append(thread)
                return process

            def guided_input(prompt):
                input_readiness.append(ready_seen.is_set())
                return ""

            def record_print(*values, **kwargs):
                if values and str(values[0]).startswith("Guided capture running:"):
                    running_readiness.append(ready_seen.is_set())

            def stop(proc):
                proc.returncode = 1
                return {
                    "controlled": True, "method": "taskkill /PID /T /F",
                    "tree_pids": [proc.pid], "exit_code": 1,
                }

            epochs = iter([
                (BASE - 1) * 1_000_000_000,
                *(
                    BASE * 1_000_000_000 + (9 + index * 20) * 1_000_000
                    for index in range(12)
                ),
            ])
            with mock.patch.object(
                capture, "find_usbpcapcmd_executable",
                return_value=Path(r"C:\USBPcap\USBPcapCMD.exe"),
            ), mock.patch.object(
                capture, "find_wireshark_executable",
                return_value=Path(r"C:\Wireshark\tshark.exe"),
            ), mock.patch.object(
                capture, "_ready_usbpcap_prefix", side_effect=observed_ready,
            ), mock.patch("builtins.print", side_effect=record_print):
                capture.run_live(
                    args,
                    input_fn=guided_input,
                    time_ns_fn=lambda: next(epochs),
                    monotonic_fn=iter((1.0, 1.2)).__next__,
                    runner=target_runner,
                    popen_factory=popen,
                    stop_fn=stop,
                    snapshot_fn=lambda: {},
                    watchdog_timeout_s=0.5,
                    readiness_timeout_s=0.5,
                    post_marker_grace_s=0,
                )
            for thread in writer_threads:
                thread.join(timeout=1)
            self.assertEqual(running_readiness, [True])
            self.assertEqual(input_readiness, [True] * len(capture.ACTION_PROMPTS))

    def test_readiness_timeout_and_early_exit_fail_before_running_prompt(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)

            def args_for(name):
                return argparse.Namespace(
                    usbpcapcmd=None, tshark=None, interface=r"\\.\USBPcap1",
                    duration_s=10, raw_capture=root / f"{name}.pcap",
                    tsv=root / f"{name}.tsv", out_json=root / f"{name}.json",
                )

            stopped = threading.Event()
            timeout_process = FakeCaptureProcess(pid=5300)

            def stop(proc):
                stopped.set()
                proc.returncode = 1
                return {
                    "controlled": True, "method": "taskkill /PID /T /F",
                    "tree_pids": [proc.pid], "exit_code": 1,
                }

            printed = []
            with mock.patch.object(
                capture, "find_usbpcapcmd_executable",
                return_value=Path(r"C:\USBPcap\USBPcapCMD.exe"),
            ), mock.patch.object(
                capture, "find_wireshark_executable",
                return_value=Path(r"C:\Wireshark\tshark.exe"),
            ), mock.patch("builtins.print", side_effect=lambda *v, **k: printed.append(v)):
                with self.assertRaisesRegex(capture.CaptureError, "readiness timed out"):
                    capture.run_discover(
                        args_for("timeout"),
                        input_fn=lambda prompt: "",
                        runner=target_runner,
                        popen_factory=lambda command: timeout_process,
                        stop_fn=stop,
                        snapshot_fn=lambda: {},
                        readiness_timeout_s=0.03,
                    )
                early_process = FakeCaptureProcess(pid=5301)
                early_process.returncode = 2
                with self.assertRaisesRegex(
                    capture.CaptureError, "exited early before pcap readiness"
                ):
                    capture.run_discover(
                        args_for("early"),
                        input_fn=lambda prompt: "",
                        runner=target_runner,
                        popen_factory=lambda command: early_process,
                        stop_fn=stop,
                        snapshot_fn=lambda: {},
                        readiness_timeout_s=0.1,
                    )
            self.assertTrue(stopped.is_set())
            self.assertFalse(any(
                values and str(values[0]).startswith("Discovery capture running:")
                for values in printed
            ))
            self.assertFalse((root / "timeout.tsv").exists())
            self.assertFalse((root / "early.json").exists())

    def test_locked_but_growing_capture_becomes_operationally_ready(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            raw = root / "locked-growing.pcap"
            source = root / "source.pcap"
            write_pcap(source, discovery_packets())
            full_pcap = source.read_bytes()
            source.unlink()
            args = argparse.Namespace(
                usbpcapcmd=None, tshark=None, interface=r"\\.\USBPcap1",
                duration_s=10, raw_capture=raw, tsv=root / "capture.tsv",
                out_json=root / "capture.json",
            )
            process = FakeCaptureProcess(pid=5400)
            writers = []

            def popen(command):
                raw.write_bytes(full_pcap[:capture.USBPCAP_MIN_OPERATIONAL_SIZE])

                def grow_locked_file():
                    time.sleep(0.04)
                    raw.write_bytes(full_pcap)

                thread = threading.Thread(target=grow_locked_file)
                thread.start()
                writers.append(thread)
                return process

            def stop(proc):
                proc.returncode = 1
                return {
                    "controlled": True, "method": "taskkill /PID /T /F",
                    "tree_pids": [proc.pid], "exit_code": 1,
                }

            with mock.patch.object(
                capture, "find_usbpcapcmd_executable",
                return_value=Path(r"C:\USBPcap\USBPcapCMD.exe"),
            ), mock.patch.object(
                capture, "find_wireshark_executable",
                return_value=Path(r"C:\Wireshark\tshark.exe"),
            ), mock.patch.object(
                capture, "_ready_usbpcap_prefix",
                side_effect=PermissionError(13, "sharing violation"),
            ):
                document = capture.run_discover(
                    args,
                    input_fn=lambda prompt: "",
                    time_ns_fn=lambda: (BASE - 1) * 1_000_000_000,
                    monotonic_fn=iter((1.0, 1.2)).__next__,
                    runner=target_runner,
                    popen_factory=popen,
                    stop_fn=stop,
                    snapshot_fn=lambda: {},
                    watchdog_timeout_s=0.03,
                    readiness_timeout_s=0.5,
                )
            for thread in writers:
                thread.join(timeout=1)
            provenance = document["capture"]
            self.assertEqual(
                provenance["readiness_method"], "locked_file_size_growth"
            )
            self.assertTrue(provenance["readiness_validation_deferred"])
            self.assertTrue(provenance["post_stop_strict_validation"])
            self.assertGreater(
                provenance["readiness_observed_size_bytes"],
                capture.USBPCAP_MIN_OPERATIONAL_SIZE,
            )
            self.assertEqual(provenance["status"], "PASS")

    def test_locked_capture_without_growth_times_out_without_format_error(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            raw = root / "locked-static.pcap"
            source = root / "source.pcap"
            write_pcap(source, discovery_packets())
            minimum = source.read_bytes()[:capture.USBPCAP_MIN_OPERATIONAL_SIZE]
            source.unlink()
            args = argparse.Namespace(
                usbpcapcmd=None, tshark=None, interface=r"\\.\USBPcap1",
                duration_s=10, raw_capture=raw, tsv=root / "capture.tsv",
                out_json=root / "capture.json",
            )
            process = FakeCaptureProcess(pid=5401)
            stopped = threading.Event()

            def popen(command):
                raw.write_bytes(minimum)
                return process

            def stop(proc):
                stopped.set()
                proc.returncode = 1
                return {
                    "controlled": True, "method": "taskkill /PID /T /F",
                    "tree_pids": [proc.pid], "exit_code": 1,
                }

            with mock.patch.object(
                capture, "find_usbpcapcmd_executable",
                return_value=Path(r"C:\USBPcap\USBPcapCMD.exe"),
            ), mock.patch.object(
                capture, "find_wireshark_executable",
                return_value=Path(r"C:\Wireshark\tshark.exe"),
            ), mock.patch.object(
                capture, "_ready_usbpcap_prefix",
                side_effect=PermissionError(13, "sharing violation"),
            ):
                with self.assertRaisesRegex(
                    capture.CaptureError, "pcap readiness timed out"
                ):
                    capture.run_discover(
                        args,
                        input_fn=lambda prompt: "",
                        runner=target_runner,
                        popen_factory=popen,
                        stop_fn=stop,
                        snapshot_fn=lambda: {},
                        readiness_timeout_s=0.04,
                    )
            self.assertTrue(stopped.is_set())
            self.assertFalse(args.tsv.exists())
            self.assertFalse(args.out_json.exists())

    def test_locked_operational_readiness_still_rejects_malformed_after_stop(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            raw = root / "locked-malformed.pcap"
            args = argparse.Namespace(
                usbpcapcmd=None, tshark=None, interface=r"\\.\USBPcap1",
                duration_s=10, raw_capture=raw, tsv=root / "capture.tsv",
                out_json=root / "capture.json",
            )
            process = FakeCaptureProcess(pid=5402)
            writers = []

            def popen(command):
                raw.write_bytes(b"X" * capture.USBPCAP_MIN_OPERATIONAL_SIZE)

                def grow_malformed_file():
                    time.sleep(0.04)
                    raw.write_bytes(b"X" * (capture.USBPCAP_MIN_OPERATIONAL_SIZE + 32))

                thread = threading.Thread(target=grow_malformed_file)
                thread.start()
                writers.append(thread)
                return process

            def stop(proc):
                proc.returncode = 1
                return {
                    "controlled": True, "method": "taskkill /PID /T /F",
                    "tree_pids": [proc.pid], "exit_code": 1,
                }

            with mock.patch.object(
                capture, "find_usbpcapcmd_executable",
                return_value=Path(r"C:\USBPcap\USBPcapCMD.exe"),
            ), mock.patch.object(
                capture, "find_wireshark_executable",
                return_value=Path(r"C:\Wireshark\tshark.exe"),
            ), mock.patch.object(
                capture, "_ready_usbpcap_prefix",
                side_effect=PermissionError(13, "sharing violation"),
            ):
                with self.assertRaisesRegex(
                    capture.CaptureError, "invalid classic pcap magic"
                ):
                    capture.run_discover(
                        args,
                        input_fn=lambda prompt: "",
                        time_ns_fn=lambda: (BASE - 1) * 1_000_000_000,
                        runner=target_runner,
                        popen_factory=popen,
                        stop_fn=stop,
                        snapshot_fn=lambda: {},
                        watchdog_timeout_s=0.03,
                        readiness_timeout_s=0.5,
                    )
            for thread in writers:
                thread.join(timeout=1)
            self.assertFalse(args.tsv.exists())
            self.assertFalse(args.out_json.exists())

    def test_existing_process_rejected_and_tree_membership_is_scoped(self) -> None:
        snapshot = {
            10: (1, "USBPcapCMD.exe"),
            11: (10, "USBPcapCMD.exe"),
            12: (11, "conhost.exe"),
            99: (1, "unrelated.exe"),
        }
        with self.assertRaisesRegex(capture.CaptureError, "another USBPcapCMD"):
            capture.reject_existing_usbpcapcmd(lambda: snapshot)
        self.assertEqual(capture._process_tree_pids(10, snapshot), {10, 11, 12})

        process = FakeCaptureProcess(pid=10)
        snapshots = iter((snapshot, {99: (1, "unrelated.exe")}))
        commands = []

        def taskkill(command, **kwargs):
            commands.append(command)
            process.returncode = 1
            return subprocess.CompletedProcess(command, 0, "", "")

        with mock.patch.object(capture.os, "name", "nt"):
            stopped = capture.stop_windows_process_tree(
                process, runner=taskkill, snapshot_fn=lambda: next(snapshots)
            )
        self.assertTrue(stopped["controlled"])
        self.assertEqual(stopped["tree_pids"], [10, 11, 12])
        self.assertEqual(commands, [["taskkill", "/PID", "10", "/T", "/F"]])

        survivor = FakeCaptureProcess(pid=10)
        survivor_snapshots = iter((snapshot, {12: (11, "conhost.exe")}))

        def taskkill_with_survivor(command, **kwargs):
            survivor.returncode = 1
            return subprocess.CompletedProcess(command, 0, "", "")

        with mock.patch.object(capture.os, "name", "nt"), self.assertRaisesRegex(
            capture.CaptureError, "surviving PIDs"
        ):
            capture.stop_windows_process_tree(
                survivor,
                runner=taskkill_with_survivor,
                snapshot_fn=lambda: next(survivor_snapshots),
            )

    def test_real_diagnostic_replay_is_nonempty_and_explicitly_partial(self) -> None:
        raw = ROOT / "diagnostics" / "ds02_4131" / "discovery.pcap"
        self.assertTrue(raw.is_file())
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            document = capture.inspect_usbpcap_replay(
                raw, root / "replay.tsv", root / "replay.json",
                expected_duration_s=10,
            )
        self.assertEqual(document["capture"]["status"], "PARTIAL_SHORT_CAPTURE")
        self.assertEqual(document["capture"]["pcap_observed_duration_s"], "1.486727")
        self.assertGreater(len(document["candidates"]), 0)
        self.assertGreater(document["candidates"][0]["report_count"], 0)


if __name__ == "__main__":
    unittest.main()
