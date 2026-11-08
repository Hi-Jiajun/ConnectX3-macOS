#!/usr/bin/env python3
"""Fixture tests for the offline QPC decoder. No device access."""
import importlib.util
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
DECODER = Path(os.environ.get('CX3_QPC_DECODER', HERE / 'decode-qpc.py')).resolve()
spec = importlib.util.spec_from_file_location('qpc', DECODER)
qpc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qpc)

# Hand-written wire image, not a builder/decoder round trip. Each row is 16 bytes
# except the final 8. A nonzero last hardware word catches truncating the QPC.
LITERAL_QPC = bytes.fromhex('''
30000001 01020304 a1b2c300 05060708
00123456 00654321 00000000 7f000000
9a000000 00000000 00000000 00000000
00000000 00000000 82008300 02123456
00abcdef 0000010f 40414243 44454647
48494a4b 4c4d4e4f 50515253 54555657
58595a5b 5c5d5e5f 60616263 64656667
03fedcba 00000000 00000000 00112233
00000000 00000000 00000000 00000000
10203040 00000000 00000000 00445566
11223344 55667788 00000000 00000000
00000000 1234abcd 00000000 00000000
02000000 00000000 070000ab 89abcdef
00000000 00000000 00000000 00000000
00000000 00000000 00000000 00000000
00000000 deadbeef
''')
PREFIX = bytes.fromhex('00000040 00000000')
PADDING = bytes.fromhex('f1f2f3f4 f5f6f7f8')


def put(raw, off, value, size):
    raw[off:off + size] = value.to_bytes(size, 'big')


class QpcDecode(unittest.TestCase):
    def cli(self, *args):
        return subprocess.run([sys.executable, str(DECODER), *args],
                              capture_output=True, text=True)

    def decoded(self, raw, layout='auto'):
        args = ['--hex', raw.hex()]
        if layout != 'auto':
            args += ['--format', layout]
        result = self.cli(*args)
        self.assertEqual(result.returncode, 0, result.stderr)
        decoded = json.loads(result.stdout)
        self.assertIn('capture', decoded, 'decoder omitted byte provenance')
        return decoded

    def test_literal_hardware_context_uses_248_bytes_and_big_endian(self):
        d = self.decoded(LITERAL_QPC)
        expected = {
            'state': 3, 'flags': 0x30000001, 'pd': 0x01020304,
            'local_qpn': 0x123456, 'remote_qpn': 0x654321,
            'pri_sched_queue': 0x82, 'pri_feup': 0x83,
            'rss_base_qpn': 0x02123456, 'rss_high_byte': 2,
            'rss_default_qpn': 0x00abcdef, 'rss_hash_fn': 1,
            'rss_flags': 0x0f, 'rss_base_qpn_udp': 0x03fedcba,
            'cqn_send': 0x00112233, 'cqn_recv': 0x00445566,
            'db_rec_addr': 0x1122334455667788,
            'rq_wqe_counter': 0x1234, 'sq_wqe_counter': 0xabcd,
            'param3': 0x02000000, 'log_page_size': 7,
            'mtt_base_h': 0xab, 'mtt_base_l': 0x89abcdef,
            'rss_key_hex': '404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f6061626364656667',
        }
        for key, value in expected.items():
            with self.subTest(field=key):
                self.assertEqual(d[key], value)
        self.assertEqual(d['capture']['context_hex'][-8:], 'deadbeef')

    def test_all_four_formats_preserve_every_input_byte(self):
        cases = [
            ('qpc', LITERAL_QPC, 0, b'', b''),
            ('legacy-slot', LITERAL_QPC + PADDING, 0, b'', PADDING),
            ('modify-mailbox', PREFIX + LITERAL_QPC + PADDING, 8, PREFIX, PADDING),
            ('query-mailbox', PREFIX + LITERAL_QPC + bytes.fromhex('a5') * 3840,
             8, PREFIX, bytes.fromhex('a5') * 3840),
        ]
        for layout, raw, offset, prefix, suffix in cases:
            for selected in ('auto', layout):
                with self.subTest(layout=layout, selected=selected):
                    d = self.decoded(raw, selected)
                    self.assertEqual(d['local_qpn'], 0x123456)
                    c = d['capture']
                    self.assertEqual(c['format'], layout)
                    self.assertEqual(c['input_bytes'], len(raw))
                    self.assertEqual(c['context_offset'], offset)
                    self.assertEqual(c['context_bytes'], 248)
                    self.assertEqual(c['prefix_hex'], prefix.hex())
                    self.assertEqual(c['suffix_hex'], suffix.hex())
                    self.assertEqual(c['context_hex'], LITERAL_QPC.hex())
                    self.assertEqual(bytes.fromhex(c['prefix_hex'] + c['context_hex'] + c['suffix_hex']), raw)
                    self.assertEqual(c['input_sha256'], hashlib.sha256(raw).hexdigest())
                    self.assertEqual(c['context_sha256'], hashlib.sha256(LITERAL_QPC).hexdigest())

    def test_padding_and_query_remainder_are_not_hardware_context(self):
        for layout, raw, role in (
            ('legacy-slot', LITERAL_QPC + PADDING, 'legacy-slot-padding'),
            ('modify-mailbox', PREFIX + LITERAL_QPC + PADDING, 'modify-capture-padding'),
            ('query-mailbox', PREFIX + LITERAL_QPC + b'\xa5' * 3840, 'query-mailbox-remainder'),
        ):
            with self.subTest(layout=layout):
                c = self.decoded(raw, layout)['capture']
                self.assertEqual(c['suffix_role'], role)
                self.assertTrue(c['suffix_nonzero'])
                self.assertEqual(bool(c['warnings']), layout != 'query-mailbox')

    def test_zero_capture_does_not_claim_firmware_success_or_validity(self):
        for length in (248, 256, 264, 4096):
            with self.subTest(length=length):
                d = self.decoded(bytes(length))
                self.assertEqual(d['state'], 0)
                self.assertEqual(d['capture']['validity'], 'not-established-by-bytes')
                self.assertFalse(d['capture']['suffix_nonzero'])

    def test_explicit_format_rejects_every_other_size(self):
        for layout, size in (('qpc', 248), ('legacy-slot', 256),
                             ('modify-mailbox', 264), ('query-mailbox', 4096)):
            for wrong in (248, 256, 264, 4096):
                if wrong == size:
                    continue
                with self.subTest(layout=layout, wrong=wrong):
                    result = self.cli('--hex', bytes(wrong).hex(), '--format', layout)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(result.stdout, '')
                    self.assertNotIn('Traceback', result.stderr)

    def test_cli_rejects_bad_input_without_partial_json_or_traceback(self):
        for args in ([], ['--hex', 'zz'], ['--hex', '0'], ['--hex', ''],
                     ['--hex', '00'], ['--hex', '00' * 247],
                     ['--hex', '00' * 249], ['--hex', '00' * 4095],
                     ['--hex', '00' * 4097],
                     ['--hex', '00' * 248, '--format', 'typo'],
                     ['--hex', '00', '--file', 'irrelevant']):
            with self.subTest(option=args[0] if args else 'none',
                              text_bytes=len(args[1]) if len(args) > 1 else 0):
                result = self.cli(*args)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, '')
                self.assertNotIn('Traceback', result.stderr)

    def test_text_file_cli_accepts_whitespace_and_handles_io_errors(self):
        with tempfile.TemporaryDirectory(prefix='cx3-qpc-test-') as directory:
            path = Path(directory) / 'capture.hex'
            path.write_text(' \n'.join(LITERAL_QPC.hex()[i:i + 32]
                                       for i in range(0, 496, 32)))
            result = self.cli('--file', str(path), '--format', 'qpc')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)['local_qpn'], 0x123456)
            result = self.cli('--file', str(Path(directory) / 'absent'))
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn('Traceback', result.stderr)

    def test_round_trip_of_a_synthetic_rss_context(self):
        raw = bytearray(256)
        put(raw, 0x00, 0x30000000, 4)          # RTS in flags[31:28]
        put(raw, 0x04, 5, 4)                   # pd
        raw[0x08] = 0xff                       # mtu_msgmax
        raw[0x09] = 0x00                       # rss QP: no own RQ
        raw[0x0a] = 0x02
        put(raw, 0x0c, 128, 4)                 # usr_page
        put(raw, 0x10, 65, 4)                  # local qpn
        raw[0x38] = 0x82                       # sched queue
        put(raw, 0x3c, 65, 4)                  # rss base qpn (1 ring)
        put(raw, 0x40, 64, 4)                  # default qpn -> RX QP
        raw[0x46] = 1                          # hash fn TOP
        raw[0x47] = 0x0f                       # ipv4/6 + tcp
        raw[0x48:0x70] = bytes((0x40 + i) & 0xff for i in range(40))
        put(raw, 0x7c, 128, 4)                 # cqn_send
        put(raw, 0x9c, 128, 4)                 # cqn_recv
        put(raw, 0xa0, 0x404040414240, 8)      # db_rec_addr
        put(raw, 0xc0, 0x02000000, 4)          # param3
        d = qpc.decode(bytes(raw))
        self.assertEqual(d['state'], 3)
        self.assertEqual(d['local_qpn'], 65)
        self.assertEqual(d['rss_base_qpn'], 65)
        self.assertEqual(d['rss_default_qpn'], 64)
        self.assertEqual(d['rss_high_byte'], 0)
        self.assertEqual(d['cqn_send'], 128)
        self.assertEqual(d['cqn_recv'], 128)
        self.assertEqual(d['db_rec_addr'], 0x404040414240)
        self.assertEqual(d['param3'], 0x02000000)
        self.assertEqual(d['rss_key_hex'][:6], '404142')
        self.assertEqual(d['rss_key_hex'][-6:], '656667')   # key bytes 0x40+i -> last three 0x65,0x66,0x67

    def test_length_is_enforced(self):
        for bad in (b'', bytes(255), bytes(257)):
            with self.subTest(n=len(bad)), self.assertRaises(ValueError):
                qpc.decode(bad)

    def test_zero_context_does_not_invent_a_state(self):
        d = qpc.decode(bytes(256))
        self.assertEqual(d['state'], 0)
        self.assertEqual(d['local_qpn'], 0)
        self.assertEqual(d['rss_key_hex'], '00' * 40)


if __name__ == '__main__':
    unittest.main(verbosity=2)
