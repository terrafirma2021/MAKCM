from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import verify_ota_trust_anchor as verifier  # noqa: E402


def anchor_text(*, zero: bool = False) -> str:
    def row(name: str, size: int, seed: int) -> str:
        data = ([0] * size) if zero else [((seed + i) & 0xFF) or 1 for i in range(size)]
        body = ", ".join(f"0x{value:02x}" for value in data)
        return f"constexpr uint8_t {name}[{size}] = {{{body}}};\n"

    return row("kSignerKeyId", 16, 1) + row("kPublicX", 32, 33) + row("kPublicY", 32, 97)


class TrustAnchorVerifierTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_accepts_anchor_embedded_in_both_apps(self) -> None:
        anchor_path = self.root / "anchor.inc"
        anchor_path.write_text(anchor_text(), encoding="ascii")
        anchor = verifier.parse_anchor(anchor_path)
        for name in ("left.bin", "right.bin"):
            image = b"\xe9" + b"\0" * 31 + b"".join(anchor.values())
            path = self.root / name
            path.write_bytes(image)
            result = verifier.verify_application(path, anchor)
            self.assertTrue(all(offset >= 0 for offset in result["anchor_offsets"].values()))

    def test_rejects_zero_anchor(self) -> None:
        path = self.root / "anchor.inc"
        path.write_text(anchor_text(zero=True), encoding="ascii")
        with self.assertRaisesRegex(verifier.AnchorVerificationError, "all zero"):
            verifier.parse_anchor(path)

    def test_rejects_application_without_anchor(self) -> None:
        anchor_path = self.root / "anchor.inc"
        anchor_path.write_text(anchor_text(), encoding="ascii")
        anchor = verifier.parse_anchor(anchor_path)
        image_path = self.root / "left.bin"
        image_path.write_bytes(b"\xe9" + b"\0" * 127)
        with self.assertRaisesRegex(verifier.AnchorVerificationError, "does not embed"):
            verifier.verify_application(image_path, anchor)


if __name__ == "__main__":
    unittest.main()
