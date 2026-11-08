#!/usr/bin/env python3
"""Decode one saved .129 FIRST ERROR block. Offline; not proof of hardware cause."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

SUMMARY = re.compile(
    r'net tx: FIRST ERROR \([^\n]*?\): syndrome 0x([0-9a-fA-F]{2}) vendor 0x([0-9a-fA-F]{2}), '
    r'consumer (\d+) producer (\d+) wqe_index (\d+) slot (\d+) posted_owner ([01]) '
    r'mbuf ([01]) WQE_valid ([01]) configured_qpn 0x([0-9a-fA-F]+) cqe_qpn 0x([0-9a-fA-F]+);')
ROW = re.compile(r'net tx: FIRST (CQE|WQE) \+([0-9a-fA-F]{2}): '
                 r'([0-9a-fA-F]{8}) ([0-9a-fA-F]{8}) ([0-9a-fA-F]{8}) ([0-9a-fA-F]{8})(?![0-9a-fA-F])')


def decode(text, entries):
    if type(entries) is not int or entries < 1 or entries > 65536 or entries & (entries-1):
        raise ValueError('SQ entries must be a power of two in 1..65536')
    summaries = list(SUMMARY.finditer(text))
    if text.count('net tx: FIRST ERROR') != 1 or len(summaries) != 1:
        raise ValueError('select exactly one complete FIRST ERROR block from one boot/service')
    m = summaries[0]
    syndrome, vendor = [int(v, 16) for v in m.groups()[:2]]
    consumer, producer, index, slot, owner, mbuf, valid = map(int, m.groups()[2:9])
    configured, reported = [int(v, 16) for v in m.groups()[9:]]
    if max(consumer, producer) > 0xffffffff or index > 0xffff or max(configured, reported) > 0xffffff:
        raise ValueError('summary integer out of range')
    if slot != index & (entries-1):
        raise ValueError('SQ entries/summary slot does not match CQE index')
    rows = {'CQE': {}, 'WQE': {}}
    for line in text.splitlines():
        if 'net tx: FIRST CQE' not in line and 'net tx: FIRST WQE' not in line:
            continue
        row = ROW.search(line)
        if not row or text.find(line) < m.start():
            raise ValueError('malformed or unassociated raw row')
        kind, offset = row[1], int(row[2], 16)
        limit = 32 if kind == 'CQE' else 64
        if offset % 16 or offset >= limit or offset in rows[kind]:
            raise ValueError('duplicate/out-of-range raw row')
        rows[kind][offset] = bytes.fromhex(''.join(row.groups()[2:]))
    if set(rows['CQE']) != {0, 16} or set(rows['WQE']) != ({0, 16, 32, 48} if valid else set()):
        raise ValueError('incomplete capture or WQE_valid conflicts with raw rows')
    cqe = b''.join(rows['CQE'][i] for i in (0, 16))
    if (cqe[31] & 31) != 30 or (cqe[26], cqe[27]) != (vendor, syndrome):
        raise ValueError('summary and raw CQE opcode/syndromes disagree')
    if struct.unpack_from('>H', cqe, 24)[0] != index or struct.unpack_from('>I', cqe)[0] & 0xffffff != reported:
        raise ValueError('summary and raw CQE index/QPN disagree')
    issues = []
    if configured != reported: issues.append('CQE QPN differs from configured TX QPN')
    if not mbuf: issues.append('no posted mbuf record at capture; slot history is uncertain')
    distance = (producer - index) & 0xffff
    inferred = (producer - distance) & 0xffffffff if mbuf and 1 <= distance <= entries else None
    if inferred is None: issues.append('cannot infer an outstanding post counter from producer/index')
    elif owner != bool(inferred & entries): issues.append('recorded owner differs from inferred ring phase')
    result = dict(schema='cx3-tx-first-fault-v1', complete=True, hardware_root_cause_proven=False,
                  runtime_identity_verified=False, sq_entries=entries, consumer=consumer,
                  producer_at_fault=producer, wqe_index=index, slot=slot, recorded_owner=owner,
                  posted_mbuf=bool(mbuf), configured_qpn=configured, cqe_qpn=reported,
                  syndrome=syndrome, syndrome_name={2:'LOCAL_QP_OP_ERR', 5:'WR_FLUSH_ERR'}.get(syndrome, 'UNMAPPED'),
                  vendor_syndrome=vendor, vendor_meaning='unknown', cqe_hex=cqe.hex(), wqe=None,
                  inferred_post_counter=inferred, counter_inference_is_exact_record=False, issues=issues)
    if valid:
        wqe = b''.join(rows['WQE'][i] for i in (0,16,32,48))
        control, = struct.unpack_from('>I', wqe)
        count, lkey, address = struct.unpack_from('>IIQ', wqe, 16)
        result['wqe'] = dict(raw_hex=wqe.hex(), owner=control >> 31, opcode=control & 255,
                             producer_field=(control >> 8) & 0xffff, size_16bytes=wqe[7] & 63,
                             byte_count=count, lkey=lkey, dma_address=f'0x{address:016x}')
        if control >> 31 != owner: issues.append('WQE owner differs from posted owner record')
        if control & 255 != 10: issues.append('WQE opcode is not SEND')
        if wqe[7] & 63 != 2: issues.append('WQE size differs from ctrl plus one data segment')
        if inferred is not None and (control >> 8) & 0xffff != inferred & 0xffff:
            issues.append('WQE producer field differs from inferred post counter; normal MMIO reference omits this field')
        if not count or count & 0x80000000: issues.append('empty or inline data segment; not the expected non-inline SEND')
        if not address: issues.append('DMA address is zero')
    else: issues.append('WQE snapshot unavailable')
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path)
    p.add_argument('--sq-entries', required=True, type=int, help='from the same run, not guessed from index')
    args = p.parse_args()
    raw = args.log.read_bytes()
    try:
        result = decode(raw.decode('utf-8', errors='strict'), args.sq_entries)
        result['input_sha256'] = hashlib.sha256(raw).hexdigest()
    except (ValueError, UnicodeError) as e:
        print(json.dumps(dict(complete=False, error=str(e)))); return 1
    print(json.dumps(result, indent=2)); return 0


if __name__ == '__main__':
    raise SystemExit(main())
