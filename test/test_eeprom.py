"""Offline EEPROM regression tests. Never imports PyUSB or touches hardware."""

import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("pakon_eeprom", ROOT / "tools/pakon_eeprom.py")
eeprom = importlib.util.module_from_spec(spec)
spec.loader.exec_module(eeprom)
FIXTURES = ROOT / "test/fixtures/eeprom"


def fixture(unit="F135-2233"):
    return {name: (FIXTURES / unit / eeprom.filename(name)).read_bytes()
            for name in eeprom.SECTIONS}


def with_crc(data):
    data = bytearray(data)
    struct.pack_into("<I", data, 4, zlib.crc32(data[8:]) & 0xFFFFFFFF)
    return bytes(data)


class FakeDevice:
    """Reject anything except the exact OEM select/read sequence."""

    def __init__(self, raw=None, fail_call=None, short_call=None, interrupt_call=None):
        self.calls = []
        self.memory = bytearray(0xA24)
        for name, data in (raw or fixture()).items():
            offset, length = eeprom.SECTIONS[name]
            self.memory[offset:offset + length] = data
        self.selected = False
        self.fail_call = fail_call
        self.short_call = short_call
        self.interrupt_call = interrupt_call

    def ctrl_transfer(self, kind, request, value, index, payload, timeout):
        self.calls.append((kind, request, value, index, payload, timeout))
        number = len(self.calls)
        if number == self.interrupt_call:
            raise KeyboardInterrupt()
        if number == self.fail_call:
            raise OSError("injected disconnect")
        if index != 0x1234 or timeout <= 0:
            raise AssertionError("invalid index/timeout")
        if kind == 0x40 and request == 0xA4:
            if value != 0xA5 or payload != b"" or self.selected:
                raise AssertionError("invalid read select")
            self.selected = True
            return 0
        if kind == 0xC0 and request == 0xA9:
            if not self.selected or not 1 <= payload <= 32:
                raise AssertionError("read must follow select; maximum chunk is 32")
            self.selected = False
            if not any(offset <= value and value + payload <= offset + length
                       for offset, length in eeprom.SECTIONS.values()):
                raise AssertionError("read outside documented sections")
            result = bytes(self.memory[value:value + payload])
            return result[:-1] if number == self.short_call else result
        raise AssertionError("unexpected USB operation")


class DecodeTests(unittest.TestCase):
    def test_real_base_unit(self):
        report = eeprom.decode_sections(fixture())
        self.assertTrue(report["decodable"])
        self.assertTrue(report["all_copies_valid"])
        self.assertEqual(report["copies_equal"], {"A": True, "B": True})
        decoded = report["decoded"]
        self.assertEqual((decoded["serial"], decoded["scanner_type"]), (2233, 1350))
        self.assertEqual(decoded["model"], "F-135")
        self.assertEqual(decoded["resolution_bases"]["4"],
                         {"offset": 27, "motor_speed": 8162, "motor_speed_ir": 6119})
        self.assertEqual(decoded["motor_adjustments"]["4"]["ir"], 1008)
        self.assertEqual(decoded["positive_matrix"][0][0], 0.25)
        self.assertAlmostEqual(decoded["negative_matrix"][0][0], 0.27680, places=4)

    def test_real_corrupt_primary_falls_back(self):
        report = eeprom.decode_sections(fixture("F135plus-16402"))
        self.assertTrue(report["decodable"])
        self.assertFalse(report["all_copies_valid"])
        self.assertEqual(report["selected"]["A"], "sectionA_backup")
        self.assertEqual(report["decoded"]["serial"], 16402)
        self.assertEqual(report["decoded"]["model"], "F-135+")
        self.assertEqual(report["decoded"]["positive_matrix"][0][1], 0.0)
        self.assertFalse(report["copies_equal"]["A"])
        self.assertEqual(report["warnings"],
                         ["Section A: sectionA_primary invalid; sectionA_backup selected"])

    def test_one_warning_per_section_problem(self):
        raw = fixture()
        for name in eeprom.SECTIONS:
            raw[name] = b"\xff" * len(raw[name])
        report = eeprom.decode_sections(raw)
        self.assertEqual(report["warnings"], ["Section A: neither copy validates",
                                              "Section B: neither copy validates"])

    def test_missing_section_has_no_file_or_hash(self):
        raw = fixture()
        del raw["sectionB_backup"]
        report = eeprom.decode_sections(raw)
        entry = report["sections"]["sectionB_backup"]
        self.assertTrue(entry["missing"])
        self.assertIsNone(entry["file"])
        self.assertIsNone(entry["sha256"])
        self.assertFalse(entry["valid"])
        self.assertIsNone(report["copies_equal"]["B"])
        self.assertEqual(report["selected"]["B"], "sectionB_primary")
        self.assertFalse(report["sections"]["sectionB_primary"]["missing"])

    def test_truncated_oversized_and_corrupt_length(self):
        original = fixture()["sectionA_primary"]
        for data in (b"", original[:7], original[:-1], original + b"x",
                     b"\xff\xff\xff\xff" + original[4:]):
            with self.subTest(length=len(data)):
                self.assertFalse(eeprom.inspect_section(data, 398)["valid"])

    def test_crc_payload_only(self):
        data = fixture()["sectionB_primary"]
        self.assertTrue(eeprom.inspect_section(data, 36)["valid"])
        changed = bytearray(data)
        changed[-1] ^= 1
        self.assertFalse(eeprom.inspect_section(changed, 36)["valid"])

    def test_backup_fallback_for_section_b(self):
        raw = fixture()
        raw["sectionB_primary"] = b"\x00" * 36
        report = eeprom.decode_sections(raw)
        self.assertTrue(report["decodable"])
        self.assertEqual(report["selected"]["B"], "sectionB_backup")

    def test_both_bad_never_decodes_that_section(self):
        for section in ("A", "B"):
            raw = fixture()
            for suffix in ("primary", "backup"):
                name = f"section{section}_{suffix}"
                raw[name] = b"\xff" * len(raw[name])
            report = eeprom.decode_sections(raw)
            self.assertFalse(report["decodable"])
            self.assertIsNone(report["selected"][section])
            self.assertNotIn("serial" if section == "A" else "motor_adjustments",
                             report["decoded"])

    def test_valid_but_different_copies(self):
        raw = fixture()
        data = bytearray(raw["sectionA_backup"])
        struct.pack_into("<I", data, 0x10, 999)
        raw["sectionA_backup"] = with_crc(data)
        report = eeprom.decode_sections(raw)
        self.assertTrue(report["all_copies_valid"])
        self.assertEqual(report["selected"]["A"], "sectionA_primary")
        self.assertTrue(any("valid copies differ" in w for w in report["warnings"]))

    def test_unknown_model_and_nonfinite_matrix(self):
        raw = fixture()
        data = bytearray(raw["sectionA_primary"])
        struct.pack_into("<I", data, 0x0C, 9999)
        struct.pack_into("<f", data, 0x26, float("nan"))
        raw["sectionA_primary"] = with_crc(data)
        report = eeprom.decode_sections(raw)
        self.assertFalse(report["decodable"])
        self.assertEqual(report["decoded"]["model"], "unknown")
        self.assertIsNone(report["decoded"]["negative_matrix"][0][0])
        json.dumps(report, allow_nan=False)


class BackupTests(unittest.TestCase):
    def test_exact_read_sequence_and_archive(self):
        device = FakeDevice()
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp) / "backup"
            report, code = eeprom.backup(device, directory)
            self.assertEqual(code, 0)
            self.assertTrue(report["acquisition_complete"])
            self.assertEqual(eeprom.read_archive(directory)["decoded"], report["decoded"])
            for name, data in fixture().items():
                self.assertEqual((directory / eeprom.filename(name)).read_bytes(), data)
            self.assertEqual(len(device.calls), 64)  # (14+14+2+2) chunks * 2
            for name, (offset, length) in eeprom.SECTIONS.items():
                reads = [c for c in device.calls if c[1] == 0xA9 and offset <= c[2] < offset + length]
                self.assertEqual(reads[0][2:5], (offset, 0x1234, 8))
                self.assertEqual(sum(c[4] for c in reads), length)
            for line in (directory / "SHA256SUMS").read_text().splitlines():
                digest, name = line.split("  ")
                self.assertEqual(digest, hashlib.sha256((directory / name).read_bytes()).hexdigest())

    def test_no_overwrite_and_no_io(self):
        device = FakeDevice()
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(FileExistsError):
                eeprom.backup(device, Path(temp))
        self.assertFalse(device.calls)

    def test_transfer_failure_preserves_partial_no_retry(self):
        # Select failure, IN failure, short IN, interruption after the header.
        cases = ({"fail_call": 3}, {"fail_call": 4},
                 {"short_call": 4}, {"interrupt_call": 4})
        for options in cases:
            with self.subTest(options=options), tempfile.TemporaryDirectory() as temp:
                device = FakeDevice(**options)
                directory = Path(temp) / "backup"
                report, code = eeprom.backup(device, directory)
                self.assertNotEqual(code, 0)
                self.assertFalse(report["acquisition_complete"])
                self.assertTrue(report["acquisition_error"])
                self.assertLessEqual(len(device.calls), 4)
                size = 39 if "short_call" in options else 8
                self.assertEqual((directory / eeprom.filename("sectionA_primary")).stat().st_size, size)
                self.assertFalse(eeprom.read_archive(directory)["decodable"])
                # Only sections actually written are reported as files.
                for name, entry in report["sections"].items():
                    self.assertEqual(entry["missing"], name != "sectionA_primary")
                    if entry["file"] is None:
                        self.assertIsNone(entry["sha256"])
                        self.assertFalse((directory / eeprom.filename(name)).exists())
                    else:
                        self.assertEqual(entry["sha256"], hashlib.sha256(
                            (directory / entry["file"]).read_bytes()).hexdigest())

    def test_unexpected_select_result_prevents_read(self):
        class BadSelect:
            def ctrl_transfer(self, *args, **kwargs):
                self.asserted = args
                return 1
        device = BadSelect()
        with self.assertRaises(OSError):
            eeprom.read_section(device, 0, 398, 2000, bytearray())
        self.assertEqual(device.asserted[1], 0xA4)

    def test_invalid_extent_never_sends(self):
        device = FakeDevice()
        with self.assertRaises(ValueError):
            eeprom.read_section(device, 0, 8192, 2000, bytearray())
        self.assertFalse(device.calls)

    def test_tamper_detected(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp) / "backup"
            eeprom.backup(FakeDevice(), directory)
            (directory / eeprom.filename("sectionA_primary")).write_bytes(b"tampered")
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                eeprom.read_archive(directory)

    def test_decode_cli_needs_no_pyusb(self):
        # -S excludes site-packages, including any installed PyUSB.
        run = subprocess.run([sys.executable, "-S", str(ROOT / "tools/pakon_eeprom.py"),
                              "decode", str(FIXTURES / "F135-2233")],
                             capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(json.loads(run.stdout)["decoded"]["serial"], 2233)


if __name__ == "__main__":
    unittest.main()
