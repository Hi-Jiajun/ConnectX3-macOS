#!/usr/bin/env python3
"""Independent RSS wire fixtures; no device/IOKit, no production QPC builder."""
import copy
import importlib.util
from pathlib import Path
import struct
import unittest

HERE=Path(__file__).resolve().parent


def load(name, filename):
    spec=importlib.util.spec_from_file_location(name,HERE/filename)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module


def fixture(status=0):
    identities=load('rss_bound_identity_fixtures','test-runtime-decode.py')
    raw=bytearray(18512)
    struct.pack_into('<III',raw,0,3,18512,status)
    struct.pack_into('<I',raw,68,65)
    # One captured modify, one captured query; all other records uncalled/zero.
    # Offsets are an independent explicit x86_64 ABI fixture, not a builder.
    for offset,size in ((18064,264),(18256,4096),(18448,4096)):
        struct.pack_into('<6I4I3Q',raw,offset,1,64,size,size,0,0,1,40,7,0,100,200,300)
    raw[896]=0x30;raw[1704:1708]=b'\x00\x00\x00\x41'
    raw[13976]=0x30;raw[13992:13996]=b'\x00\x00\x00\x41'
    observation=dict(selector=35,call=0,inCount=0,**{'in':[]},
                     outCount=15,out=['0x0']*15,structLen=18512,structHex=raw.hex())
    observation['out'][0]=hex(status);observation['out'][5]='0x41'
    observation['out'][8]='0x1';observation['out'][9]='0x3'
    return dict(schema='cx3-bound-rss-v1',before=identities.identity(uptime=10),
                after=identities.identity(uptime=1000),identityBracketMatched=True,rss=observation)


class BoundRssDecode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):cls.decoder=load('bound_rss_decoder','decode-runtime.py')

    def decode(self,c):
        self.assertTrue(hasattr(self.decoder,'decode_bound_rss'),'RSS runtime binding consumer is missing')
        return self.decoder.decode_bound_rss(c)

    def test_complete_binding_keeps_exact_mailboxes_and_scope(self):
        r=self.decode(fixture());self.assertTrue(r['binding_valid'],r['issues'])
        self.assertTrue(r['operation_succeeded']);self.assertEqual(r['rss']['qpn'],65)
        self.assertEqual(r['rss']['entries'][0]['decoded']['state'],3)
        self.assertEqual(r['rss']['entries'][1]['decoded']['local_qpn'],65)
        self.assertFalse(r['atomic_snapshot']);self.assertFalse(r['dma_quiescence_proven'])

    def test_device_failure_is_bound_diagnostic_not_feature_success(self):
        r=self.decode(fixture(0xe00002d8));self.assertTrue(r['binding_valid'],r['issues'])
        self.assertFalse(r['operation_succeeded']);self.assertEqual(r['rss']['result_status'],0xe00002d8)

    def test_selector_zero_without_final_query_does_not_mean_operation_success(self):
        c=fixture();raw=bytearray.fromhex(c['rss']['structHex'])
        raw[13968:18064]=bytes(4096);raw[18448:18512]=bytes(64)
        c['rss']['structHex']=raw.hex();r=self.decode(c)
        self.assertTrue(r['binding_valid'],r['issues']);self.assertFalse(r['operation_succeeded'])

    def test_drift_rejected_even_when_producer_claims_success(self):
        identities=load('rss_drift_fixtures','test-runtime-decode.py')
        for changes in (dict(generation=8),dict(provider=4),dict(service=3),dict(tag=b'other'),
                        dict(uptime=9),dict(flags=17)):
            with self.subTest(changes=changes):
                c=fixture();c['after']=identities.identity(**changes)
                self.assertFalse(self.decode(c)['binding_valid'])
        c=fixture();c['identityBracketMatched']=False;self.assertFalse(self.decode(c)['binding_valid'])

    def test_record_time_must_lie_inside_identity_bracket(self):
        for start,end in ((1,300),(100,1001)):
            c=fixture();raw=bytearray.fromhex(c['rss']['structHex'])
            struct.pack_into('<Q',raw,18064+40,start);struct.pack_into('<Q',raw,18064+56,end)
            c['rss']['structHex']=raw.hex();self.assertFalse(self.decode(c)['binding_valid'])

    def test_wrong_selector_dimensions_and_stale_mailbox_rejected(self):
        mutations=(('selector',34),('call',5),('inCount',1),('outCount',14),
                   ('structLen',18511),('out',['0x0']*14),('in',['0x0']))
        for key,value in mutations:
            with self.subTest(key=key):
                c=fixture();c['rss'][key]=value
                self.assertFalse(self.decode(c)['binding_valid'])
        c=fixture();raw=bytearray.fromhex(c['rss']['structHex']);raw[5776]=1
        c['rss']['structHex']=raw.hex();self.assertFalse(self.decode(c)['binding_valid'])
        c=fixture();c['rss']['out'][0]='0x1';self.assertFalse(self.decode(c)['binding_valid'])
        c=fixture();c['rss']['out'][5]='0x42';self.assertFalse(self.decode(c)['binding_valid'])

    def test_raw_result_and_canonical_capture_hashes_bind_bytes(self):
        import hashlib,json
        c=fixture();r=self.decode(c)
        self.assertEqual(r['rss']['result_sha256'],hashlib.sha256(bytes.fromhex(c['rss']['structHex'])).hexdigest())
        canonical=json.dumps(c,sort_keys=True,separators=(',',':'),ensure_ascii=True).encode()
        self.assertEqual(r['capture_sha256'],hashlib.sha256(canonical).hexdigest())


if __name__=='__main__':unittest.main(verbosity=2)
