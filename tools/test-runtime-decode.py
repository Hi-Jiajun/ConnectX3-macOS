#!/usr/bin/env python3
"""Saved-wire runtime/capture evidence; never opens a service or device."""
import copy
import importlib.util
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parent


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT/filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def identity(**changes):
    fields = dict(service=0x1000000000000001, provider=0x2000000000000002,
                  uptime=10000, generation=7, flags=16, validity=63,
                  tag=b'0.44.123-local', version=b'0.44.48')
    fields.update(changes)
    raw = struct.pack('<II6QII', 1, 176, 0, fields['validity'], fields['service'],
                      fields['provider'], fields['uptime'], fields['generation'],
                      fields['flags'], 0)
    raw += struct.pack('<4H4B', 0x15b3, 0x1007, 0x15b3, 0x1234, 3, 0, 0, 0)
    raw += fields['version'].ljust(32, b'\0')+fields['tag'].ljust(65, b'\0')+b'\0'*3
    return dict(schema='cx3-runtime-identity-v1', bootSessionUuid='6B43565A-082A-4C44-A357-F164DD5EB873',
                connectedServiceRegistryId=f"0x{fields['service']:016x}", transport=0,
                scalarOutputCount=0, returnedBytes=176, rawHex=raw.hex())


def bound():
    fixture = load('pcie_fixtures_runtime', 'test-pcie-decode.py').fixture()
    return dict(schema='cx3-bound-pcie-v1', before=identity(), after=identity(uptime=10001),
                identityBracketMatched=True,
                pcie=dict(transport=0, returnedBytes=368, rawHex=fixture['structHex']))


def overwrite(capture, offset, data):
    raw = bytearray.fromhex(capture['rawHex'])
    raw[offset:offset+len(data)] = data
    capture['rawHex'] = raw.hex()


class RuntimeDecodeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.decoder = load('runtime_decoder', 'decode-runtime.py')

    def test_valid_identity_preserves_64_bit_ids_and_scope(self):
        r = self.decoder.decode_identity(identity())
        self.assertTrue(r['complete'])
        self.assertEqual(r['service_registry_id'], '0x1000000000000001')
        self.assertEqual(r['build_tag'], '0.44.123-local')
        self.assertEqual(r['device_generation'], '0x0000000000000007')
        self.assertFalse(r['dma_quiescence_proven'])

    def test_complete_capture_uses_independent_pcie_decoder(self):
        r = self.decoder.decode_bound(bound())
        self.assertTrue(r['binding_valid'])
        self.assertEqual(r['pcie']['current_link']['lanes'], 4)
        self.assertEqual(len(r['capture_sha256']), 64)

    def test_malformed_identity_never_becomes_complete(self):
        mutations = [(0, struct.pack('<I', 2)), (4, struct.pack('<I', 175)),
                     (16, struct.pack('<Q', 127)), (56, struct.pack('<I', 32)),
                     (60, b'\x01'), (75, b'\x01'), (173, b'\x01'),
                     (64+9, b'\x20'), (64+10, b'\x08'), (76, b'x'*32),
                     (108, b'x'*65), (86, b'x'), (122, b'\xff')]
        for offset, data in mutations:
            with self.subTest(offset=offset, data=data):
                c = identity(); overwrite(c, offset, data)
                with self.assertRaises(ValueError): self.decoder.decode_identity(c)
        for key, value in [('transport', True), ('returnedBytes', 175),
                           ('scalarOutputCount', 1), ('rawHex', '00'),
                           ('connectedServiceRegistryId', 2**60),
                           ('connectedServiceRegistryId', '0x0000000000000001'),
                           ('bootSessionUuid', 'not-a-boot'), ('schema', 'future')]:
            with self.subTest(key=key):
                c = identity(); c[key] = value
                with self.assertRaises(ValueError): self.decoder.decode_identity(c)

    def test_missing_metadata_and_fallback_are_not_identity_proof(self):
        for kw in [dict(validity=55), dict(service=0), dict(provider=0),
                   dict(generation=0), dict(uptime=0), dict(tag=b'unversioned-local')]:
            with self.subTest(kw=kw):
                self.assertFalse(self.decoder.decode_identity(identity(**kw))['complete'])

    def test_drift_and_refusal_reject_binding(self):
        for field, value in [('service', 3), ('provider', 4), ('generation', 8),
                             ('tag', b'other-build'), ('version', b'0.44.49'),
                             ('uptime', 9999), ('flags', 17)]:
            with self.subTest(field=field):
                c = bound(); c['after'] = identity(**{field:value})
                c['identityBracketMatched'] = False
                self.assertFalse(self.decoder.decode_bound(c)['binding_valid'])
        c = bound(); c['after']['bootSessionUuid'] = '7B43565A-082A-4C44-A357-F164DD5EB873'
        c['identityBracketMatched'] = False
        self.assertFalse(self.decoder.decode_bound(c)['binding_valid'])
        c = bound(); overwrite(c['after'], 64+6, b'\x35\x12')
        c['identityBracketMatched'] = False
        self.assertFalse(self.decoder.decode_bound(c)['binding_valid'])

    def test_pcie_generation_and_raw_forgery_reject_binding(self):
        for offset, data in [(24, struct.pack('<Q', 8)), (56+6, b'\x35\x12'),
                             (80, b'\x01'), (336, b'\x00')]:
            with self.subTest(offset=offset):
                c = bound(); overwrite(c['pcie'], offset, data)
                self.assertFalse(self.decoder.decode_bound(c)['binding_valid'])
        c = bound(); c['pcie']['transport'] = 5; c['identityBracketMatched'] = False
        self.assertFalse(self.decoder.decode_bound(c)['binding_valid'])

    def test_saved_claim_is_never_trusted(self):
        c = bound(); c['after'] = identity(generation=8)
        self.assertFalse(self.decoder.decode_bound(c)['binding_valid'])
        c = bound(); c['identityBracketMatched'] = False
        self.assertFalse(self.decoder.decode_bound(c)['binding_valid'])


if __name__ == '__main__': unittest.main(verbosity=2)
