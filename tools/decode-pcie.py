#!/usr/bin/env python3
"""Decode saved selector-48 x86_64 PCIe captures without accessing a device.

Re-derive the capability walk and negotiated/max link values from sampled PCI
configuration bytes, not producer summaries. Saved data does not authenticate
hardware; bounded repeat reads cannot establish an atomic or historical state.
"""
import argparse
import hashlib
import json
import re
import struct
import sys

REASONS = ("ok", "not_ready", "reset_blocked", "retention_blocked",
           "read_unavailable", "unsupported_header", "no_pcie_capability",
           "malformed_list", "cycle", "truncated_capability",
           "unsupported_pcie", "inconsistent")
# PCI_EXP_LNKSTA_CLS / NLW, pci_regs.h. Widths are raw lane counts, not log2.
SPEEDS = {1: 2.5, 2: 5, 3: 8, 4: 16, 5: 32, 6: 64}
WIDTHS = {1, 2, 4, 8, 12, 16, 32}


def _integer(document, key):
    value = document.get(key)
    if type(value) is not int:
        raise ValueError(f"{key} must be an integer")
    return value


def _identity(payload, offset):
    values = struct.unpack_from("<4H4B", payload, offset)
    if values[-1] != 0:
        raise ValueError("identity reserved byte is nonzero")
    return values[:-1]


def _link(value):
    speed, width = value & 15, (value >> 4) & 63
    return {"speed_code": speed, "gt_per_second": SPEEDS.get(speed),
            "width_raw": width, "lanes": width if width in WIDTHS else None}


def decode_capture(document):
    if not isinstance(document, dict):
        raise ValueError("capture must be a JSON object")
    for key, expected in (("selector", 48), ("call", 0), ("inCount", 0),
                          ("outCount", 0), ("structLen", 368)):
        if _integer(document, key) != expected:
            raise ValueError(f"{key} must be {expected}")
    if document.get("in") != [] or document.get("out") != []:
        raise ValueError("PCIe query requires empty scalar input/output arrays")
    encoded = document.get("structHex")
    if not isinstance(encoded, str) or not re.fullmatch(r"[0-9a-fA-F]{736}", encoded):
        raise ValueError("structHex must encode exactly 368 bytes")
    payload = bytes.fromhex(encoded)
    (version, size, status, reason, validity, generation, cap_offset,
     walk_count, flags, capability, link_status, capability2) = \
        struct.unpack_from("<IIQIIQ6I", payload)
    before, after = _identity(payload, 56), _identity(payload, 68)
    raw, sampled = payload[80:336], payload[336:368]
    if version != 1 or size != 368 or reason >= len(REASONS) or validity & ~31:
        raise ValueError("unsupported PCIe ABI, reason or validity bits")
    for index, value in enumerate(raw):
        if value and not sampled[index // 8] & (1 << (index % 8)):
            raise ValueError("unread raw bytes must be zero")

    def read(offset, count):
        if offset < 0 or offset + count > 256:
            raise ValueError("configuration field extends beyond conventional space")
        for index in range(offset, offset + count):
            if not sampled[index // 8] & (1 << (index % 8)):
                raise ValueError("required configuration field was not sampled")
        value = int.from_bytes(raw[offset:offset+count], "little")
        if value == (1 << (8 * count)) - 1:
            raise ValueError("all-ones observation is unavailable, not success")
        return value

    result = {
        "schema": "cx3-pcie-capture-v1", "capture_state": "failed",
        "status": status, "reason": REASONS[reason], "validity": validity,
        "provider_generation": generation, "atomic_snapshot": False,
        "hardware_provenance": "unverified saved input; hashes bind bytes only",
        "capture_sha256": hashlib.sha256(payload).hexdigest(),
        "raw_hex": raw.hex(), "read_valid_hex": sampled.hex(),
        "current_link": None, "advertised_max": None,
        "supported_speed_vector_raw": None, "training_raw": None,
        "training_interpretation": None, "dll_active": None,
    }
    if status != 0:
        if reason == 0 or validity & 24:
            raise ValueError("failed query claims success or complete link fields")
        if validity & 4:
            # A stable, exhausted non-PCIe list is a valid diagnostic failure.
            # Re-derive its limited consistency claim; never publish link data.
            if reason != 6 or validity != 7 or before != after or \
                    any(value == 0xffff for value in before[:4]) or \
                    before[5] >= 32 or before[6] >= 8 or \
                    any((cap_offset, flags, capability, link_status, capability2)):
                raise ValueError("invalid consistent no-PCIe diagnostic")
            if (read(0, 2), read(2, 2), read(0x2c, 2), read(0x2e, 2)) != before[:4] or \
                    read(0x0e, 1) & 0x7f or not read(6, 2) & 0x10:
                raise ValueError("no-PCIe diagnostic lacks consistent endpoint identity")
            pointer, seen = read(0x34, 1), set()
            while pointer:
                if pointer < 0x40 or pointer > 0xfc or pointer & 3 or \
                        pointer in seen or len(seen) >= 48:
                    raise ValueError("invalid no-PCIe capability list")
                seen.add(pointer)
                header = read(pointer, 2)
                if header & 255 == 0x10:
                    raise ValueError("no-PCIe diagnostic contains PCIe capability")
                pointer = header >> 8
            if len(seen) != walk_count:
                raise ValueError("no-PCIe node count disagrees with raw walk")
        if reason in (1, 2, 3) and (validity or any(raw) or any(sampled) or
                                  any(before) or any(after) or cap_offset or
                                  walk_count or flags or capability or
                                  link_status or capability2):
            raise ValueError("pre-observation refusal carries sampled state")
        return result
    if reason != 0 or validity not in (15, 31):
        raise ValueError("success requires complete, consistent identity/link samples")
    if before != after or any(value == 0xffff for value in before[:4]) or \
            before[5] >= 32 or before[6] >= 8:
        raise ValueError("identity is invalid or changed during observation")

    if (read(0, 2), read(2, 2), read(0x2c, 2), read(0x2e, 2)) != before[:4]:
        raise ValueError("PCI identity disagrees with sampled configuration")
    if read(0x0e, 1) & 0x7f or not read(6, 2) & 0x10:
        raise ValueError("success lacks an endpoint capability list")
    pointer, seen = read(0x34, 1), set()
    while pointer:
        if pointer < 0x40 or pointer > 0xfc or pointer & 3:
            raise ValueError("malformed capability pointer")
        if pointer in seen or len(seen) >= 48:
            raise ValueError("cyclic or unbounded capability list")
        seen.add(pointer)
        header = read(pointer, 2)
        next_pointer = header >> 8
        if next_pointer and (next_pointer < 0x40 or next_pointer > 0xfc or
                             next_pointer & 3 or next_pointer in seen):
            raise ValueError("malformed or cyclic next capability pointer")
        if header & 255 == 0x10:
            break
        pointer = next_pointer
    else:
        raise ValueError("success contains no PCI Express capability")
    if pointer != cap_offset or len(seen) != walk_count:
        raise ValueError("producer capability offset/count disagrees with raw walk")
    wire_flags = read(pointer + 2, 2)
    pcie_version, port_type = wire_flags & 15, (wire_flags >> 4) & 15
    if pcie_version not in (1, 2) or port_type not in (0, 1):
        raise ValueError("unsupported PCIe capability version or endpoint type")
    wire_capability, wire_status = read(pointer + 0x0c, 4), read(pointer + 0x12, 2)
    wire_capability2 = read(pointer + 0x2c, 4) if pcie_version == 2 else 0
    if (flags, capability, link_status, capability2) != \
            (wire_flags, wire_capability, wire_status, wire_capability2):
        raise ValueError("producer PCIe fields disagree with sampled configuration")
    if validity != (31 if pcie_version == 2 else 15):
        raise ValueError("PCIe version and capability2 validity disagree")
    result.update(
        capture_state="captured", cap_offset=pointer, walk_count=len(seen),
        current_link=_link(wire_status), advertised_max=_link(wire_capability),
        supported_speed_vector_raw=(wire_capability2 & 0xfe) if pcie_version == 2 else None,
        training_raw=(wire_status >> 11) & 1,
        dll_active=bool(wire_status & 0x2000) if wire_capability & 0x100000 else None,
        pcie_version=pcie_version, device_port_type=port_type,
    )
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", help="saved rawjson selector-48 result")
    args = parser.parse_args()
    try:
        with open(args.capture, "rb") as stream:
            saved = stream.read(1024 * 1024 + 1)
        if len(saved) > 1024 * 1024:
            raise ValueError("capture exceeds 1 MiB")
        result = decode_capture(json.loads(saved))
    except (OSError, ValueError, UnicodeError) as error:
        print(f"decode-pcie: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, ensure_ascii=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
