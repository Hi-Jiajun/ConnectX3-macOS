#!/usr/bin/env python3
"""Decode saved CX3 selector-50 module EEPROM captures; fully offline.

Input files are cx3ctl rawjson records. Saved reads are not an atomic EEPROM
snapshot, and neither their bytes nor deviceGeneration authenticate hardware.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

STRUCT_BYTES = 368
RAW_BYTES = 256
DATA_BYTES = 48
MAD_NOT_RETURNED = 0xffff


def integer(obj, key):
    value = obj.get(key)
    if type(value) is not int:
        raise ValueError(f"{key} must be an integer")
    return value


def scalar(value, key):
    if isinstance(value, str) and re.fullmatch(r"0[xX][0-9a-fA-F]+", value):
        return int(value, 16)
    if type(value) is int:
        return value
    raise ValueError(f"input {key} must be an integer or hexadecimal scalar")


def parse_capture(obj, source="capture"):
    if not isinstance(obj, dict):
        raise ValueError("rawjson record must be a JSON object")
    for key, expected in (("selector", 50), ("call", 0), ("inCount", 3),
                          ("outCount", 0), ("structLen", STRUCT_BYTES)):
        if integer(obj, key) != expected:
            raise ValueError(f"{key} must be {expected}")
    if obj.get("out") != []:
        raise ValueError("selector 50 requires an empty scalar output array")
    inputs = obj.get("in")
    if not isinstance(inputs, list) or len(inputs) != 3:
        raise ValueError("selector 50 requires three scalar inputs")
    port_in, offset_in, length_in = [scalar(v, k) for v, k in zip(inputs, ("port", "offset", "length"))]
    encoded = obj.get("structHex")
    if not isinstance(encoded, str) or not re.fullmatch(r"[0-9a-fA-F]{736}", encoded):
        raise ValueError("structHex must encode exactly 368 bytes")
    payload = bytes.fromhex(encoded)
    version, size, status, fw_status, mad_status, generation = struct.unpack_from("<IIQIIQ", payload)
    fields = struct.unpack_from("<8I", payload, 32)
    port, offset, requested, returned, i2c, page, wire_offset, raw_bytes = fields
    raw = payload[64:320]
    data = payload[320:368]
    if version != 1 or size != STRUCT_BYTES:
        raise ValueError("unsupported module result version or structBytes")
    if (port_in, offset_in, length_in) != (port, offset, requested):
        raise ValueError("selector inputs disagree with the returned request fields")
    if not (1 <= port <= 2 and 0 <= offset < 512 and 1 <= requested <= DATA_BYTES):
        raise ValueError("port/offset/requestedBytes outside selector-50 range")
    if requested > 256 - (offset & 255):
        raise ValueError("requested range crosses a 256-byte page")
    if (i2c, page, wire_offset) != (0x50 if offset < 256 else 0x51, 0, offset & 255):
        raise ValueError("I2C address/page/wireOffset disagree with flat offset")
    if raw_bytes not in (0, RAW_BYTES):
        raise ValueError("rawBytes must be 0 or 256")
    if raw_bytes == 0 and any(raw):
        raise ValueError("raw response bytes present while rawBytes is zero")
    result = dict(source=source, port=port, offset=offset, requested=requested,
                  generation=generation, status=status, fw_status=fw_status,
                  mad_status=mad_status, raw_bytes=raw_bytes,
                  capture_sha256=hashlib.sha256(payload).hexdigest())
    if status != 0:
        if returned != 0 or any(data):
            raise ValueError("failed read carries purported returned EEPROM data")
        if mad_status != MAD_NOT_RETURNED and raw_bytes != RAW_BYTES:
            raise ValueError("MAD status reported without a captured MAD response")
        if raw_bytes == RAW_BYTES and int.from_bytes(raw[4:6], "big") != mad_status:
            raise ValueError("MAD status disagrees with retained response header")
        result["diagnostic"] = (f"read failed: status={status}, fwStatus={fw_status}, "
                                f"madStatus=0x{mad_status:04x}")
        result["success"] = False
        return result, None
    if fw_status != 0 or mad_status != 0 or raw_bytes != RAW_BYTES or returned != requested:
        raise ValueError("successful read requires fw/mad status 0, rawBytes 256 and returned=requested")
    if generation == 0:
        raise ValueError("successful read requires a nonzero deviceGeneration")
    if int.from_bytes(raw[4:6], "big") != 0:
        raise ValueError("MAD response header reports failure for a successful result")
    if data[:requested] != raw[80:80 + requested]:
        raise ValueError("data disagrees with MAD response payload raw[80:80+n]")
    if any(data[requested:]):
        raise ValueError("unused data tail must be zero-filled")
    result["success"] = True
    result["returned"] = returned
    result["data_hex"] = data[:returned].hex()
    return result, (port, generation, offset, data[:returned])


def printable(field):
    field = field.split(b"\0", 1)[0].rstrip(b" ")
    if not field:
        return None
    if all(32 <= c <= 126 for c in field):
        return field.decode("ascii")
    return None


def decode_dom(memory):
    def byte(offset):
        return memory[offset] if 0 <= offset < 512 else None

    def be16(offset):
        a, b = byte(offset), byte(offset + 1)
        return ((a << 8) | b) if a is not None and b is not None else None

    identifier = byte(0)
    names = {0x03: "SFP", 0x0c: "QSFP", 0x0d: "QSFP+", 0x11: "QSFP28"}
    module = names.get(identifier, "unknown")
    result = dict(identifier=identifier, module_type=module, fields={}, limitations=[])
    if module == "SFP":
        for key, start, end in (("vendor", 20, 36), ("part_number", 40, 56), ("serial", 68, 84)):
            part = bytes(byte(i) or 0 for i in range(start, end)) if all(byte(i) is not None for i in range(start, end)) else None
            result["fields"][key] = printable(part) if part is not None else None
        options = byte(92)
        result["fields"]["a0_dom_options"] = options
        result["fields"]["dom_supported"] = bool(options & 0x40) if options is not None else None
        result["fields"]["internal_calibration"] = bool(options & 0x20) if options is not None else None
        result["fields"]["external_calibration"] = bool(options & 0x10) if options is not None else None
        if options is not None and options & 0x40 and options & 0x20 and not options & 0x10:
            values = (("temperature_c", 352, lambda x: struct.unpack(">h", x.to_bytes(2, "big"))[0] / 256),
                      ("voltage_v", 354, lambda x: x / 10000),
                      ("tx_bias_ma", 356, lambda x: x * 0.002),
                      ("tx_power_mw", 358, lambda x: x * 0.0001),
                      ("rx_power_mw", 360, lambda x: x * 0.0001))
            for name, offset, convert in values:
                raw_value = be16(offset)
                result["fields"][name] = convert(raw_value) if raw_value is not None else None
            result["limitations"].append("SFP engineering values decoded only for declared internal calibration; missing bytes remain null")
        else:
            result["limitations"].append("SFP DOM engineering values withheld unless A0 byte 92 declares DOM and internal, non-external calibration")
    elif module in ("QSFP", "QSFP+", "QSFP28"):
        fields = result["fields"]
        status = byte(2)
        fields["data_ready"] = not bool(status & 1) if status is not None else None
        if status is None or status & 1:
            result["limitations"].append("QSFP engineering values withheld: Data_Not_Ready is set or status byte 2 is missing")
            return result
        raw_t, raw_v = be16(22), be16(26)
        fields["temperature_c"] = struct.unpack(">h", raw_t.to_bytes(2, "big"))[0] / 256 if raw_t is not None else None
        fields["voltage_v"] = raw_v / 10000 if raw_v is not None else None
        fields["rx_power_mw"] = [be16(34 + 2 * i) / 10000 if be16(34 + 2 * i) is not None else None for i in range(4)]
        fields["tx_bias_ma"] = [be16(42 + 2 * i) * 0.002 if be16(42 + 2 * i) is not None else None for i in range(4)]
        fields["tx_power_mw"] = [be16(50 + 2 * i) / 10000 if be16(50 + 2 * i) is not None else None for i in range(4)]
        result["limitations"].append("QSFP fields use the specified lower-memory byte offsets/scales; availability is per-byte and no hardware DOM support is asserted")
    else:
        result["limitations"].append("unknown identifier; no DOM interpretation attempted")
    return result


def assemble(captures, intervals, diagnostics):
    """Join validated intervals and decode only the EEPROM bytes actually present."""
    successful = list(intervals)
    if not successful:
        return dict(schema="cx3-module-eeprom-v1", complete=False, captures=captures,
                    diagnostics=diagnostics or ["no successful EEPROM bytes"], dom=None,
                    hardware_verified=False, snapshot_atomic=False)
    ports, generations = {x[0] for x in successful}, {x[1] for x in successful}
    if len(ports) != 1 or len(generations) != 1:
        diagnostics.append("successful reads have mixed port or deviceGeneration; refusing to combine")
        return dict(schema="cx3-module-eeprom-v1", complete=False, captures=captures,
                    diagnostics=diagnostics, dom=None, hardware_verified=False, snapshot_atomic=False)
    memory = {}
    for port, generation, offset, data in successful:
        for index, value in enumerate(data, offset):
            if index in memory and memory[index] != value:
                diagnostics.append(f"conflicting overlap at EEPROM offset {index}")
                return dict(schema="cx3-module-eeprom-v1", complete=False, captures=captures,
                            diagnostics=diagnostics, dom=None, hardware_verified=False, snapshot_atomic=False)
            memory[index] = value
    view = [memory.get(i) for i in range(512)]
    dom = decode_dom(view)
    result = dict(schema="cx3-module-eeprom-v1", complete=(len(memory) == 512 and not diagnostics),
                  capture_set_valid=not diagnostics,
                  port=next(iter(ports)), device_generation=next(iter(generations)),
                  bytes_present=len(memory), intervals=[], captures=captures,
                  diagnostics=diagnostics, dom=dom, hardware_verified=False,
                  hardware_provenance="offline saved input; deviceGeneration is not hardware authentication",
                  snapshot_atomic=False,
                  snapshot_note="Reads may have been captured at different times; this is not an atomic EEPROM snapshot.")
    indices = sorted(memory)
    if indices:
        start = prev = indices[0]
        for index in indices[1:] + [None]:
            if index is not None and index == prev + 1:
                prev = index
                continue
            result["intervals"].append(dict(start=start, end_exclusive=prev + 1, length=prev - start + 1))
            if index is not None:
                start = prev = index
    return result


def decode_files(paths):
    diagnostics, captures, intervals = [], [], []
    for path in paths:
        try:
            raw = Path(path).read_bytes()
            if len(raw) > 1024 * 1024:
                raise ValueError("capture exceeds 1 MiB")
            record, interval = parse_capture(json.loads(raw.decode("utf-8")), str(path))
            captures.append(record)
            if interval:
                intervals.append(interval)
            else:
                diagnostics.append(record["diagnostic"] + f" ({path})")
        except (OSError, ValueError, UnicodeError, json.JSONDecodeError) as exc:
            diagnostics.append(f"{path}: {exc}")
    return assemble(captures, intervals, diagnostics)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", nargs="+", help="one or more saved selector-50 rawjson files")
    args = parser.parse_args()
    result = decode_files(args.capture)
    print(json.dumps(result, indent=2, ensure_ascii=True))
    return 0 if result.get("dom") is not None and result.get("capture_set_valid") else 1


if __name__ == "__main__":
    raise SystemExit(main())
