#!/usr/bin/env python3
"""Decode a mlx4 QPC or captured mailbox (BE on the wire), offline only.

Offsets come from tools/cx3-mlx4/src/mlx4_regs.h (QP_CONTEXT_*). This is an analysis tool
for captured raw contexts; it never touches a device. Byte decoding does not establish
whether a command was attempted, posted, completed or semantically accepted.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

QP_CONTEXT_SIZE = 248
# Exact sizes of the four existing capture shapes, not guesses from field values.
# Legacy slots and modify captures have 8 bytes beyond the real hardware QPC.
LAYOUTS = {
    'qpc': (248, 0, 'none'),
    'legacy-slot': (256, 0, 'legacy-slot-padding'),
    'modify-mailbox': (264, 8, 'modify-capture-padding'),
    'query-mailbox': (4096, 8, 'query-mailbox-remainder'),
}

# Explicit x86_64 little-endian result ABI, separate from BE hardware mailboxes.
# Tests execute actual C rawjson with compiler-laid-out result structs; a header
# change must be deliberately versioned here, never guessed from a partial file.
RESULT_ABIS = {
    34: dict(size=17912, version=1, version_offset=17452, bytes_offset=17456,
             qpn=4, status=0, sent=276, query=1068, final=13356,
             sent_meta=17464, query_meta=17656, final_meta=17848),
    35: dict(size=18512, version=3, version_offset=0, bytes_offset=4,
             qpn=68, status=8, sent=888, query=1680, final=13968,
             sent_meta=18064, query_meta=18256, final_meta=18448),
}

# (name, offset, kind)
FIELDS = [
    ('flags',            0x00, 'u32'),   # state in bits 31:28
    ('pd',               0x04, 'u32'),
    ('mtu_msgmax',       0x08, 'u8'),
    ('rq_size_stride',   0x09, 'u8'),
    ('sq_size_stride',   0x0a, 'u8'),
    ('usr_page',         0x0c, 'u32'),
    ('local_qpn',        0x10, 'u32'),
    ('remote_qpn',       0x14, 'u32'),
    ('pri_counter_index',0x1c, 'u8'),
    ('pri_ackto',        0x20, 'u8'),
    ('pri_sched_queue',  0x38, 'u8'),
    ('pri_feup',         0x3a, 'u8'),
    ('rss_base_qpn',     0x3c, 'u32'),
    ('rss_default_qpn',  0x40, 'u32'),
    ('rss_reserved',     0x44, 'u16'),
    ('rss_hash_fn',      0x46, 'u8'),
    ('rss_flags',        0x47, 'u8'),
    ('rss_base_qpn_udp', 0x70, 'u32'),
    ('cqn_send',         0x7c, 'u32'),
    ('cqn_recv',         0x9c, 'u32'),
    ('db_rec_addr',      0xa0, 'u64'),
    ('rq_wqe_counter',   0xb4, 'u16'),
    ('sq_wqe_counter',   0xb6, 'u16'),
    ('param3',           0xc0, 'u32'),
    ('log_page_size',    0xc8, 'u8'),
    ('mtt_base_h',       0xcb, 'u8'),
    ('mtt_base_l',       0xcc, 'u32'),
]


def decode(raw, layout='auto'):
    if layout == 'auto':
        layout = next((name for name, (size, _, _) in LAYOUTS.items()
                       if len(raw) == size), None)
        if layout is None:
            raise ValueError('capture must be exactly 248, 256, 264 or 4096 bytes, got %d' % len(raw))
    if layout not in LAYOUTS:
        raise ValueError('unknown capture format: %s' % layout)
    size, offset, suffix_role = LAYOUTS[layout]
    if len(raw) != size:
        raise ValueError('%s must be exactly %d bytes, got %d' % (layout, size, len(raw)))
    context = raw[offset:offset + QP_CONTEXT_SIZE]
    prefix, suffix = raw[:offset], raw[offset + QP_CONTEXT_SIZE:]
    out = {}
    for name, off, kind in FIELDS:
        size = {'u8': 1, 'u16': 2, 'u32': 4, 'u64': 8}[kind]
        out[name] = int.from_bytes(context[off:off + size], 'big')
    out['state'] = (out['flags'] >> 28) & 0xf
    out['rss_key_hex'] = context[0x48:0x70].hex()
    out['rss_high_byte'] = (out['rss_base_qpn'] >> 24) & 0xff
    warnings = []
    if layout in ('legacy-slot', 'modify-mailbox') and any(suffix):
        warnings.append('nonzero capture padding; preserved separately, not part of the 248-byte QPC')
    out['capture'] = {
        'format': layout,
        'input_bytes': len(raw),
        'context_offset': offset,
        'context_bytes': QP_CONTEXT_SIZE,
        'input_sha256': hashlib.sha256(raw).hexdigest(),
        'context_sha256': hashlib.sha256(context).hexdigest(),
        'prefix_hex': prefix.hex(),
        'context_hex': context.hex(),
        'suffix_hex': suffix.hex(),
        'suffix_role': suffix_role,
        'suffix_nonzero': bool(any(suffix)),
        'validity': 'not-established-by-bytes',
        'warnings': warnings,
    }
    return out


def decode_result(document):
    if not isinstance(document, dict):
        raise ValueError('rawjson envelope must be an object')
    for field in ('selector', 'call', 'structLen'):
        if type(document.get(field)) is not int:
            raise ValueError('%s must be an integer' % field)
    if document['call'] != 0:
        raise ValueError('rawjson transport did not succeed')
    abi = RESULT_ABIS.get(document['selector'])
    if abi is None:
        raise ValueError('only selector 34/35 result ABIs are supported')
    if not isinstance(document.get('structHex'), str):
        raise ValueError('structHex must be a hex string')
    raw = bytes.fromhex(document['structHex'])
    if document['structLen'] != len(raw) or len(raw) != abi['size']:
        raise ValueError('rawjson/result length mismatch or unsupported ABI size')

    def u32(offset):
        return int.from_bytes(raw[offset:offset+4], 'little')

    if u32(abi['version_offset']) != abi['version'] or u32(abi['bytes_offset']) != len(raw):
        raise ValueError('result version/length stamp mismatch')
    entries = []
    spans = []
    for step in range(3):
        spans += [('modify-%d' % (step+1), abi['sent']+step*264,
                   abi['sent_meta']+step*64, 'modify-mailbox'),
                  ('query-%d' % (step+1), abi['query']+step*4096,
                   abi['query_meta']+step*64, 'query-mailbox')]
    spans.append(('query-final', abi['final'], abi['final_meta'], 'query-mailbox'))
    for label, offset, meta_offset, layout in spans:
        size = LAYOUTS[layout][0]
        mailbox = raw[offset:offset+size]
        meta = raw[meta_offset:meta_offset+64]
        version, count, prepared, captured, status, reserved = struct.unpack_from('<6I', meta)
        cv, cb, flags, cs, start, posted, end = struct.unpack_from('<4I3Q', meta, 24)
        if version == 0:
            if any(meta):
                raise ValueError('%s: uncalled capture contains stale evidence' % label)
        elif version != 1 or count != 64 or reserved != 0:
            raise ValueError('%s: invalid capture ABI' % label)
        if prepared not in (0, size) or captured not in (0, size) or captured > prepared:
            raise ValueError('%s: inconsistent capture lengths' % label)
        if flags == 0:
            if any(meta[24:]):
                raise ValueError('%s: unentered command contains evidence' % label)
        else:
            if cv != 1 or cb != 40 or flags not in (1, 3, 7) or cs != status:
                raise ValueError('%s: invalid command observation' % label)
            if prepared != size:
                raise ValueError('%s: command without a prepared mailbox' % label)
            if start > end or (flags & 2 and not start <= posted <= end) or (not flags & 2 and posted):
                raise ValueError('%s: inconsistent command timestamps' % label)
        if not captured and any(mailbox):
            raise ValueError('%s: uncaptured mailbox contains stale bytes' % label)
        if layout == 'query-mailbox' and captured and (flags != 7 or status != 0):
            raise ValueError('%s: failed/unreplied query claims copied bytes' % label)
        entries.append({
            'label': label, 'mailbox_offset': offset, 'mailbox_bytes': size,
            'observation_offset': meta_offset,
            'mailbox_sha256': hashlib.sha256(mailbox).hexdigest(),
            'observation': {'invoked': version != 0, 'prepared_bytes': prepared,
                            'captured_bytes': captured, 'fw_status': status,
                            'command': {'flags': flags, 'fw_status': cs,
                                        'start_ns': start, 'posted_ns': posted, 'end_ns': end}},
            'decoded': decode(mailbox, layout) if captured else None,
        })
    return {'selector': document['selector'], 'abi_version': abi['version'],
            'abi_bytes': len(raw), 'qpn': u32(abi['qpn']),
            'result_status': u32(abi['status']),
            'result_sha256': hashlib.sha256(raw).hexdigest(),
            'identity': 'not-established-by-this-result-ABI',
            'evidence': 'untrusted saved capture; hashes bind bytes, not origin or device acceptance',
            'entries': entries}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    source = ap.add_mutually_exclusive_group(required=True)
    source.add_argument('--hex', help='raw capture as hex')
    source.add_argument('--file', help='file containing hex (whitespace ignored)')
    source.add_argument('--rawjson', help='saved cx3ctl rawjson 34/35 JSON; offline only')
    ap.add_argument('--format', choices=['auto', *LAYOUTS], default='auto',
                    help='capture layout; auto selects by exact byte count only')
    args = ap.parse_args()
    try:
        if args.rawjson:
            if args.format != 'auto':
                raise ValueError('--format is only for standalone hex captures')
            saved = Path(args.rawjson).read_bytes()
            decoded = decode_result(json.loads(saved.decode('utf-8')))
            decoded['file_sha256'] = hashlib.sha256(saved).hexdigest()
        else:
            text = args.hex if args.hex is not None else Path(args.file).read_text()
            raw = bytes.fromhex(''.join(text.split()))
            decoded = decode(raw, args.format)
    except (OSError, UnicodeError, ValueError) as error:
        ap.error(str(error))
    print(json.dumps(decoded, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
