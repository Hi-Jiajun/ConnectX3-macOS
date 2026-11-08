#!/usr/bin/env python3
"""Focused offline tests for decode-module.py."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("decode_module", HERE / "decode-module.py")
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)


def capture(offset, data, *, port=1, generation=0x1234, status=0, fw=0, mad=0):
    length = len(data)
    raw = bytearray(256)
    raw_bytes = 256 if status == 0 or mad != 0xffff else 0
    if raw_bytes:
        raw[4:6] = mad.to_bytes(2, "big")
        if status == 0:
            raw[80:80 + length] = data
    result = bytearray(368)
    struct.pack_into("<IIQIIQ", result, 0, 1, 368, status, fw, mad, generation)
    struct.pack_into("<8I", result, 32, port, offset, length, length if status == 0 else 0,
                     0x50 if offset < 256 else 0x51, 0, offset & 255,
                     raw_bytes)
    result[64:320] = raw
    if status == 0:
        result[320:320 + length] = data
    return dict(selector=50, call=0, inCount=3,
                **{"in": [hex(port), hex(offset), hex(length)]},
                outCount=0, out=[], structLen=368, structHex=result.hex())


def place(samples, start, content):
    return samples.append(capture(start, content))


class ModuleDecoderTests(unittest.TestCase):
    def test_unready_qsfp_has_no_engineering_values(self):
        parsed = decoder.parse_capture(capture(0, b"\x0d\0\x01" + bytes(29)))
        result = decoder.assemble([parsed[0]], [parsed[1]], [])
        self.assertFalse(result["dom"]["fields"]["data_ready"])
        self.assertNotIn("temperature_c", result["dom"]["fields"])

    def test_internal_calibrated_sfp_dom(self):
        a0 = bytearray(96)
        a0[0] = 0x03
        a0[20:36] = b"ACME OPTICS".ljust(16)
        a0[40:56] = b"SFP-10G-SR".ljust(16)
        a0[68:84] = b"SN123456".ljust(16)
        a0[92] = 0x60  # DOM present, internal calibration, no external calibration
        a2 = bytearray(8)
        struct.pack_into(">hHHH", a2, 0, 25 * 256, 33000, 1000, 12000)
        # RX power starts at flat EEPROM offset 360, after this first 8-byte chunk.
        samples = []
        place(samples, 0, a0[:48])
        place(samples, 48, a0[48:96])
        place(samples, 352, bytes(a2))
        parsed = [decoder.parse_capture(item) for item in samples]
        result = decoder.assemble([r for r, _ in parsed], [i for _, i in parsed], [])
        self.assertIsNotNone(result["dom"])
        dom = result["dom"]
        self.assertEqual(dom["module_type"], "SFP")
        self.assertTrue(dom["fields"]["dom_supported"])
        self.assertEqual(dom["fields"]["temperature_c"], 25.0)
        self.assertEqual(dom["fields"]["voltage_v"], 3.3)
        self.assertEqual(dom["fields"]["tx_bias_ma"], 2.0)
        self.assertEqual(dom["fields"]["tx_power_mw"], 1.2)
        self.assertIsNone(dom["fields"]["rx_power_mw"])

    def test_external_calibration_withholds_engineering_values(self):
        a0 = bytearray(96); a0[0] = 3; a0[92] = 0x50
        samples = [decoder.parse_capture(capture(0, a0[:48])),
                   decoder.parse_capture(capture(48, a0[48:]))]
        merged = decoder.assemble([r for r, _ in samples], [i for _, i in samples], [])
        dom = merged["dom"]
        self.assertNotIn("temperature_c", dom["fields"])
        self.assertTrue(any("withheld" in item for item in dom["limitations"]))

    def test_rejects_bad_abi_and_data_disagreement(self):
        item = capture(0, bytes(range(16)))
        broken = dict(item); broken["structLen"] = 367
        with self.assertRaisesRegex(ValueError, "structLen"):
            decoder.parse_capture(broken)
        payload = bytearray.fromhex(item["structHex"]); payload[320] ^= 1
        broken = dict(item, structHex=payload.hex())
        with self.assertRaisesRegex(ValueError, "disagrees"):
            decoder.parse_capture(broken)

    def test_rejects_conflicting_overlapping_samples(self):
        parsed = [decoder.parse_capture(capture(0, b"abcd")),
                  decoder.parse_capture(capture(2, b"XY"))]
        result = decoder.assemble([r for r, _ in parsed], [i for _, i in parsed], [])
        self.assertIsNone(result["dom"])
        self.assertIn("conflicting overlap at EEPROM offset 2", result["diagnostics"])

    def test_failed_read_has_diagnostic_and_no_bytes(self):
        item = capture(0, b"x", status=0xe00002c7, fw=0xffffffff, mad=0xffff)
        record, interval = decoder.parse_capture(item)
        self.assertFalse(record["success"])
        self.assertIsNone(interval)
        self.assertIn("read failed", record["diagnostic"])
        result = decoder.assemble([record], [], [record["diagnostic"]])
        self.assertIsNone(result["dom"])
        self.assertTrue(result["diagnostics"])

    def test_qsfp_lower_page_fields_and_per_field_gaps(self):
        lower = bytearray(58); lower[0] = 0x0d
        struct.pack_into(">h", lower, 22, -256)
        struct.pack_into(">H", lower, 26, 33000)
        struct.pack_into(">HHHH", lower, 34, 10000, 20000, 30000, 40000)
        struct.pack_into(">HHHH", lower, 42, 100, 200, 300, 400)
        struct.pack_into(">HHHH", lower, 50, 5000, 6000, 7000, 8000)
        parts = [decoder.parse_capture(capture(0, lower[:48])),
                 decoder.parse_capture(capture(48, lower[48:]))]
        result = decoder.assemble([r for r, _ in parts], [i for _, i in parts], [])
        fields = result["dom"]["fields"]
        self.assertEqual(result["dom"]["module_type"], "QSFP+")
        self.assertEqual(fields["temperature_c"], -1.0)
        self.assertEqual(fields["voltage_v"], 3.3)
        self.assertEqual(fields["rx_power_mw"], [1.0, 2.0, 3.0, 4.0])
        self.assertEqual(fields["tx_bias_ma"], [0.2, 0.4, 0.6, 0.8])
        self.assertTrue(any("no hardware DOM support" in x for x in result["dom"]["limitations"]))

    def test_multiple_rawjson_files_cli_input_path(self):
        with tempfile.TemporaryDirectory(prefix="cx3-module-decode-") as folder:
            paths = []
            for index, data in enumerate((b"\x03" + bytes(47), bytes(48))):
                path = Path(folder) / f"read-{index}.json"
                path.write_text(json.dumps(capture(index * 48, data)), encoding="utf-8")
                paths.append(path)
            result = decoder.decode_files(paths)
        self.assertEqual(result["bytes_present"], 96)
        self.assertEqual(result["dom"]["module_type"], "SFP")
        self.assertFalse(result["hardware_verified"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
