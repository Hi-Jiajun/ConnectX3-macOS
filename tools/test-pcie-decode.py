#!/usr/bin/env python3
"""Literal saved-wire fixtures catch max/current confusion and forged captures.

Production breaks caught: reading flags at cap+0 instead of cap+2, trusting
side fields over raw, decoding unsampled bytes, accepting malformed/cyclic
lists or inconsistent identity, silently treating unknown speeds/training as
known, and publishing a current link from a failed query. No driver access.
"""
from pathlib import Path
import copy
import importlib.util
import json
import struct
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("decode-pcie.py")
spec = importlib.util.spec_from_file_location("decode_pcie", SCRIPT)
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)
IDENTITY = struct.pack("<4H4B", 0x15b3, 0x1007, 0x15b3, 0x1234, 3, 0, 0, 0)


def fixture(*, version=1, current=0x2043, maximum=0x100083,
            cap2=0x0e, status=0, reason=0):
    raw, valid = bytearray(256), bytearray(32)

    def put(offset, value):
        raw[offset:offset+len(value)] = value
        for index in range(offset, offset+len(value)):
            valid[index // 8] |= 1 << (index % 8)

    put(0, b"\xb3\x15\x07\x10")
    put(0x2c, b"\xb3\x15\x34\x12")
    put(6, b"\x10\x00")
    put(0x0e, b"\x80")  # multifunction bit is not a bridge header type
    put(0x34, b"\x40")
    put(0x40, b"\x10\x00")
    put(0x42, struct.pack("<H", version))
    put(0x4c, struct.pack("<I", maximum))
    put(0x52, struct.pack("<H", current))
    if version == 2:
        put(0x6c, struct.pack("<I", cap2))
    payload = struct.pack("<IIQIIQ6I", 1, 368, status, reason,
                          31 if version == 2 else 15, 7, 0x40, 1,
                          version, maximum, current, cap2 if version == 2 else 0)
    payload += IDENTITY * 2 + raw + valid
    return {"selector": 48, "call": 0, "inCount": 0, "in": [],
            "outCount": 0, "out": [], "structLen": 368,
            "structHex": payload.hex()}


def change(document, offset, value, fmt="<I"):
    altered = copy.deepcopy(document)
    payload = bytearray.fromhex(altered["structHex"])
    struct.pack_into(fmt, payload, offset, value)
    altered["structHex"] = payload.hex()
    return altered


class PcieDecodeTests(unittest.TestCase):
    def decoded(self, document):
        result = decoder.decode_capture(document)
        self.assertIsInstance(result, dict)
        self.assertTrue({"capture_state", "current_link", "advertised_max",
                         "raw_hex", "read_valid_hex", "hardware_provenance",
                         "provider_generation"}.issubset(result),
                        "decoder must emit a checked PCIe record")
        return result

    def test_current_gen3_x4_is_not_advertised_gen3_x8(self):
        result = self.decoded(fixture())
        self.assertEqual(result.get("capture_state"), "captured")
        self.assertEqual(result.get("current_link"),
                         {"speed_code": 3, "gt_per_second": 8, "width_raw": 4, "lanes": 4})
        self.assertEqual(result.get("advertised_max"),
                         {"speed_code": 3, "gt_per_second": 8, "width_raw": 8, "lanes": 8})
        self.assertEqual(result.get("provider_generation"), 7)
        self.assertIn("unverified", result.get("hardware_provenance", ""))
        self.assertIs(result.get("atomic_snapshot"), False)

    def test_gen1_x1_vs_max_gen3_x8_and_cap2_vector_stay_separate(self):
        result = self.decoded(fixture(version=2, current=0x2011, cap2=0x0e))
        self.assertEqual(result.get("current_link", {}).get("gt_per_second"), 2.5)
        self.assertEqual(result.get("current_link", {}).get("lanes"), 1)
        self.assertEqual(result.get("advertised_max", {}).get("lanes"), 8)
        self.assertEqual(result.get("supported_speed_vector_raw"), 0x0e)

    def test_training_is_raw_only_and_dll_validity_is_conditional(self):
        result = self.decoded(fixture(current=0x2843))
        self.assertEqual(result.get("training_raw"), 1)
        self.assertIsNone(result.get("training_interpretation"))
        self.assertIs(result.get("dll_active"), True)
        result = self.decoded(fixture(maximum=0x83, current=0x0043))
        self.assertIsNone(result.get("dll_active"))

    def test_unknown_speed_and_reserved_width_are_not_fabricated(self):
        result = self.decoded(fixture(current=0x2037))
        self.assertEqual(result.get("current_link", {}).get("speed_code"), 7)
        self.assertEqual(result.get("current_link", {}).get("width_raw"), 3)
        self.assertIsNone(result.get("current_link", {}).get("gt_per_second"))
        self.assertIsNone(result.get("current_link", {}).get("lanes"))

    def test_side_fields_or_identity_cannot_disagree_with_sampled_raw(self):
        # ABI flags/current/capOffset/identity mismatch must not be trusted.
        for offset, value in ((40, 2), (48, 0x2083), (32, 0x44), (36, 2),
                              (68, 0x100715b4), (72, 0x123415b4), (76, 4)):
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                decoder.decode_capture(change(fixture(), offset, value))

    def test_missing_read_bits_unsampled_data_and_cycles_are_rejected(self):
        for offset, value, fmt in ((336+0x52//8, 0, "<B"),
                                   (80+0x80, 1, "<B"),
                                   (80+0x41, 0x40, "<B"),
                                   (80+0x34, 0xfd, "<B"),
                                   (80+6, 0, "<B")):
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                decoder.decode_capture(change(fixture(), offset, value, fmt))

    def test_failed_diagnostic_retains_raw_but_publishes_no_link(self):
        # Inconsistent sample may retain all successfully-read bytes, but no
        # complete/consistent validity flag may survive a failed observation.
        document = change(fixture(status=0xe00002bc, reason=11), 20, 3)
        result = self.decoded(document)
        self.assertEqual(result.get("capture_state"), "failed")
        self.assertIsNone(result.get("current_link"))
        self.assertIsNone(result.get("advertised_max"))
        self.assertEqual(len(result.get("raw_hex", "")), 512)
        for offset, value in ((16, 0), (20, 15)):
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                decoder.decode_capture(change(document, offset, value))

    def test_transport_abi_reserved_and_unknown_reason_are_rejected(self):
        for key, value in (("selector", 47), ("selector", True), ("call", 5),
                           ("inCount", 1), ("outCount", 1), ("in", [0]),
                           ("out", [0]), ("structLen", 367), ("structHex", "00")):
            document = fixture()
            document[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                decoder.decode_capture(document)
        for offset, value in ((0, 2), (4, 360), (16, 12), (20, 0x8f), (40, 3)):
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                decoder.decode_capture(change(fixture(), offset, value))
        for offset in (67, 79):
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                decoder.decode_capture(change(fixture(), offset, 1, "<B"))

    def test_saved_file_cli_has_no_hardware_dependency(self):
        with tempfile.TemporaryDirectory(prefix="cx3-pcie-decode-") as temporary:
            path = Path(temporary) / "capture.json"
            path.write_text(json.dumps(fixture()))
            result = subprocess.run([sys.executable, str(SCRIPT), str(path)],
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(result.stdout.strip(), "CLI must emit decoded saved record")
            if result.stdout.strip():
                self.assertEqual(json.loads(result.stdout)["current_link"]["lanes"], 4)
            path.write_text(json.dumps(change(fixture(), 48, 0x2083)))
            result = subprocess.run([sys.executable, str(SCRIPT), str(path)],
                                    capture_output=True, text=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main(verbosity=2)
