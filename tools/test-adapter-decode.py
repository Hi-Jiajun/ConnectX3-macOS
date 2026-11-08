#!/usr/bin/env python3
"""Check saved adapter captures against independent literal wire/ABI fixtures.

Catches accepting failed/short/foreign captures as board identity, using the
VSD header as ID, ignoring one Topspin signature, and trusting decoded fields
that disagree with the raw firmware reply. Never opens the driver.
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

SCRIPT = Path(__file__).with_name("decode-adapter.py")
spec = importlib.util.spec_from_file_location("decode_adapter", SCRIPT)
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)


def capture(*, board=None, raw=None, status=0, fw=0, length=256, pin=11):
    if raw is None:
        raw = bytearray(256)
        raw[0x10] = 11
        raw[0x20:0x24] = b"VSD!"  # deliberate decoy
        raw[0xf0:0x100] = b"1_TM54329876DCBA"
    if board is None:
        board = b"MT_123456789ABCD" + bytes(48)
    payload = struct.pack("<IIQIIQII64s256s", 1, 360, status, fw,
                          length, 7, pin, 0, board, bytes(raw))
    return {"selector": 47, "call": 0, "inCount": 0, "in": [],
            "outCount": 0, "out": [], "structLen": 360,
            "structHex": payload.hex()}


class AdapterDecodeTests(unittest.TestCase):
    def decode(self, document):
        result = decoder.decode_capture(document)
        self.assertIsInstance(result, dict)
        self.assertTrue({"capture_state", "board_id", "board_id_hex", "identity_state",
                         "raw_sha256", "provider_generation"}.issubset(result),
                        "adapter decoder must return a validated capture record")
        return result

    def test_complete_mellanox_identity_is_verified_against_wire(self):
        document = capture()
        result = self.decode(document)
        self.assertEqual(result["capture_state"], "captured")
        self.assertEqual(result["board_id"], "MT_123456789ABCD")
        self.assertEqual(result["board_layout"], "mellanox")
        self.assertEqual(result["inta_pin"], 11)
        self.assertEqual(result["provider_generation"], 7)
        self.assertEqual(result["raw_sha256"],
                         "06d917ed6e9165d405e89bd0da35135310e4183e5aad4a95c9f2bcb33aad8ca8")
        self.assertIn("unverified", result["hardware_provenance"])
        self.assertEqual(len(result["board_id_hex"]), 128)

    def test_topspin_both_signatures_and_bounded_string(self):
        raw = bytearray(256)
        raw[0x10] = 5
        raw[0x20:0x22] = raw[0xfe:0x100] = b"\x05\xad"
        raw[0x40:0x80] = b"T" * 64  # destination must terminate at byte63
        result = self.decode(capture(raw=raw, board=b"T"*63+b"\0", pin=5))
        self.assertEqual(result["board_id"], "T" * 63)
        self.assertEqual(result["board_layout"], "topspin")
        for offset in (0x20, 0xfe):
            missing_signature = bytearray(raw)
            missing_signature[offset:offset+2] = b"\0\0"
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                decoder.decode_capture(capture(raw=missing_signature, board=b"T"*63+b"\0", pin=5))

    def test_decoded_fields_cannot_disagree_with_raw(self):
        for field, value in (("board", b"FAKE" + bytes(60)), ("pin", 12)):
            with self.subTest(field=field), self.assertRaises(ValueError):
                decoder.decode_capture(capture(**{field: value}))

    def test_failed_and_not_posted_have_no_identity(self):
        for fw, state in ((7, "failed"), (0xffffffff, "not_attempted")):
            document = capture(board=bytes(64), raw=bytes(256),
                               status=0xe00002bc, fw=fw, length=0, pin=255)
            result = self.decode(document)
            self.assertEqual(result["capture_state"], state)
            self.assertIsNone(result["board_id"])
            self.assertIsNone(result["raw_sha256"])
        with self.assertRaises(ValueError):
            decoder.decode_capture(capture(status=0xe00002bc, fw=7, length=0))

    def test_empty_success_is_not_invented_identity(self):
        result = self.decode(capture(raw=bytes(256), board=bytes(64), pin=0))
        self.assertEqual(result["capture_state"], "captured")
        self.assertIsNone(result["board_id"])
        self.assertEqual(result["identity_state"], "unresolved")

    def test_binary_identity_is_preserved_without_fabricating_text(self):
        raw = bytearray(256)
        raw[0x10] = 11
        raw[0xf0:0x100] = bytes(range(16))
        board = bytes((3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)) + bytes(48)
        result = self.decode(capture(raw=raw, board=board))
        self.assertIsNone(result["board_id"])
        self.assertEqual(result["board_id_hex"], board.hex())
        self.assertEqual(result["identity_state"], "unresolved")

    def test_rejects_bad_transport_shape_and_version(self):
        mutations = {"selector": 23, "call": 1, "structLen": 359,
                     "inCount": 1, "outCount": 1, "in": [0], "out": [0],
                     "structHex": "00", "selector_bool": True}
        for field, value in mutations.items():
            document = capture()
            document["selector" if field == "selector_bool" else field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                decoder.decode_capture(document)
        for offset, value in ((0, 2), (4, 359), (20, 255), (32, 256), (36, 1)):
            document = capture()
            payload = bytearray.fromhex(document["structHex"])
            struct.pack_into("<I", payload, offset, value)
            document["structHex"] = payload.hex()
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                decoder.decode_capture(document)

    def test_cli_consumes_saved_json_without_device_access(self):
        with tempfile.TemporaryDirectory(prefix="cx3-adapter-decode-") as temp:
            path = Path(temp) / "capture.json"
            path.write_text(json.dumps(capture()))
            result = subprocess.run([sys.executable, str(SCRIPT), str(path)],
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(result.stdout.strip(), "saved capture CLI must emit its record")
            self.assertEqual(json.loads(result.stdout)["board_id"], "MT_123456789ABCD")
            document = copy.deepcopy(capture())
            document["call"] = 9
            path.write_text(json.dumps(document))
            result = subprocess.run([sys.executable, str(SCRIPT), str(path)],
                                    capture_output=True, text=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main(verbosity=2)
