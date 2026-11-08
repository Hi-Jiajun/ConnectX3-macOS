#!/usr/bin/env python3
"""Decode saved selector-47 v1 x86_64 captures without opening the driver.

Usage: /usr/bin/python3 tools/decode-adapter.py saved-rawjson.json
Checks the producer's board ID and INTA pin against the original BE mailbox.
Hashes bind the saved bytes; the input itself does not authenticate hardware.
"""
import argparse
import hashlib
import json
import re
import struct
import sys


def _integer(document, key):
    value = document.get(key)
    if type(value) is not int:
        raise ValueError(f"{key} must be an integer")
    return value


def decode_capture(document):
    if not isinstance(document, dict):
        raise ValueError("capture must be a JSON object")
    for key, expected in (("selector", 47), ("call", 0), ("inCount", 0),
                          ("outCount", 0), ("structLen", 360)):
        if _integer(document, key) != expected:
            raise ValueError(f"{key} must be {expected}")
    if document.get("in") != [] or document.get("out") != []:
        raise ValueError("adapter query requires empty scalar input/output arrays")
    encoded = document.get("structHex")
    if not isinstance(encoded, str) or not re.fullmatch(r"[0-9a-fA-F]{720}", encoded):
        raise ValueError("structHex must encode exactly 360 bytes")
    payload = bytes.fromhex(encoded)
    version, size, status, fw, raw_bytes, generation, pin, reserved, board, raw = \
        struct.unpack("<IIQIIQII64s256s", payload)
    if version != 1 or size != 360 or reserved != 0 or pin > 255:
        raise ValueError("unsupported adapter ABI, reserved bits or INTA pin")
    result = {
        "schema": "cx3-adapter-capture-v1",
        "status": status, "fw_status": fw, "provider_generation": generation,
        "capture_sha256": hashlib.sha256(payload).hexdigest(),
        "hardware_provenance": "unverified saved input; hashes bind bytes only",
        "capture_state": "captured", "identity_state": "unresolved",
        "board_id": None, "board_id_hex": None, "board_layout": None,
        "inta_pin": None, "raw_sha256": None,
    }
    if status != 0:
        if raw_bytes != 0 or any(board) or any(raw) or pin != 255:
            raise ValueError("failed query carries a purported identity or raw response")
        result["capture_state"] = "not_attempted" if fw == 0xffffffff else "failed"
        return result
    if fw != 0 or raw_bytes != 256:
        raise ValueError("success requires firmware status zero and 256 captured bytes")
    if pin != raw[0x10]:
        raise ValueError("INTA pin disagrees with the original mailbox")
    if raw[0x20:0x22] == b"\x05\xad" and raw[0xfe:0x100] == b"\x05\xad":
        layout = "topspin"
        field = raw[0x40:0x7f].split(b"\0", 1)[0]
        expected_board = field.ljust(64, b"\0")
    else:
        layout = "mellanox"
        field = b"".join(raw[start:start+4][::-1] for start in range(0xf0, 0x100, 4))
        expected_board = field + bytes(48)
    if board != expected_board:
        raise ValueError("board ID disagrees with the original mailbox layout")
    # Keep binary/empty IDs as bytes, rather than making a plausible PSID string.
    text, _, padding = board.partition(b"\0")
    if text and all(32 <= byte <= 126 for byte in text) and not any(padding):
        result["board_id"] = text.decode("ascii")
        result["identity_state"] = "candidate"
    result.update(board_id_hex=board.hex(), board_layout=layout, inta_pin=pin,
                  raw_sha256=hashlib.sha256(raw).hexdigest())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", help="saved rawjson selector-47 result")
    args = parser.parse_args()
    try:
        with open(args.capture, "rb") as stream:
            saved = stream.read(1024 * 1024 + 1)
        if len(saved) > 1024 * 1024:
            raise ValueError("capture exceeds 1 MiB")
        result = decode_capture(json.loads(saved))
    except (OSError, ValueError, UnicodeError) as error:
        print(f"decode-adapter: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, ensure_ascii=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
