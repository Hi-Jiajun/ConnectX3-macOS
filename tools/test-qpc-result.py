#!/usr/bin/env python3
"""Actual rawjson producer -> independent result consumer, no IOKit/device.

The transport fills real C result structs, so the fixture's compiler layout is
independent of the Python consumer's explicit ABI offsets. No context builder is
used. Failures must not turn unattempted/failed query bytes into valid contexts.
"""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
SRC = HERE.parent
DECODER = Path(os.environ.get('CX3_QPC_DECODER', HERE / 'decode-qpc.py')).resolve()


class ResultDecode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='cx3-result-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / 'producer'
        source = (SRC / 'tools/cx3ctl.c').read_text()
        start = source.index('static int cmdRawJson(')
        producer = source[start:source.index('\n}\n', start) + 3]
        harness = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cx3_abi.h"
#include "mlx4_regs.h"
typedef int kern_return_t;
const int KERN_SUCCESS=0,KERN_FAILURE=5;
static int gConn=0;
static int openService() { return 0; }
static void breadcrumb(const char*,...) {}
static void observation(Cx3QpCaptureObservation *o,unsigned n,unsigned flags,unsigned status) {
    o->abiVersion=1;o->abiBytes=sizeof(*o);o->preparedBytes=n;
    o->capturedBytes=n;o->fwStatus=status;
    o->command.abiVersion=1;o->command.abiBytes=40;o->command.flags=flags;
    o->command.fwStatus=status;o->command.startNs=100;
    o->command.postedNs=200;o->command.endNs=300;
}
template<class T> static void fill(T *r) {
    memset(r,0,sizeof(*r));r->abiBytes=sizeof(*r);r->qpn=0x123456;
    r->status=0xe00002d8; // Device-level failure must remain inspectable.
    observation(&r->modifyCapture[0],264,7,0);
    observation(&r->queryCapture[0],4096,7,0);
    observation(&r->modifyCapture[1],264,3,CX3_FWSTATUS_TIMEOUT);
    observation(&r->finalQueryCapture,4096,3,CX3_FWSTATUS_TIMEOUT);
    r->finalQueryCapture.capturedBytes=0;
    r->rawSentMailboxStep[0][8]=0x30;
    r->rawSentMailboxStep[0][24]=0x00;r->rawSentMailboxStep[0][25]=0x12;
    r->rawSentMailboxStep[0][26]=0x34;r->rawSentMailboxStep[0][27]=0x56;
    memcpy(r->rawSentMailboxStep[1],r->rawSentMailboxStep[0],264);
    memcpy(r->rawQueryMailboxStep[0],r->rawSentMailboxStep[0],264);
    r->rawQueryMailboxStep[0][4095]=0x5a;
}
static int IOConnectCallMethod(int,unsigned selector,const uint64_t*,uint32_t,
    const void*,size_t,uint64_t*,uint32_t*,void *p,size_t *n) {
    if(selector==34 && *n==sizeof(Cx3R4QpResult)) {
        auto *r=(Cx3R4QpResult*)p;fill(r);r->abiVersion=CX3_R4QP_ABI;
    } else if(selector==35 && *n==sizeof(Cx3R4IngressResult)) {
        auto *r=(Cx3R4IngressResult*)p;fill(r);r->abiVersion=CX3_R4INGRESS_ABI;
    } else return 5;
    return 0;
}
PRODUCER
int main(int argc,char **argv) { return cmdRawJson(argc,argv,1); }
'''.replace('PRODUCER', producer)
        subprocess.run(['/usr/bin/clang++', '-std=c++11', '-Wall', '-Wextra',
                        '-Werror', '-I', str(SRC / 'src'), '-x', 'c++', '-',
                        '-o', str(cls.binary)], input=harness, text=True, check=True)

    def document(self, selector=34):
        return json.loads(subprocess.check_output([str(self.binary), str(selector),
                         'struct', '17912' if selector==34 else '18512'], text=True))

    def consume(self, document):
        path = Path(self.tmp.name) / 'input.json'
        path.write_text(json.dumps(document))
        return subprocess.run([sys.executable, str(DECODER), '--rawjson', str(path)],
                              text=True, capture_output=True)

    def decoded(self, document):
        result = self.consume(document)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def rejected(self, document):
        result = self.consume(document)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, '')
        self.assertNotIn('Traceback', result.stderr)

    def mutate(self, document, offset, value, count=4):
        raw = bytearray.fromhex(document['structHex'])
        raw[offset:offset+count] = value.to_bytes(count, 'little')
        document['structHex'] = raw.hex()
        return document

    def test_actual_producer_both_result_layouts_and_device_failure(self):
        for selector in (34, 35):
            with self.subTest(selector=selector):
                d = self.decoded(self.document(selector))
                self.assertEqual(d['selector'], selector)
                self.assertEqual(d['qpn'], 0x123456)
                self.assertEqual(d['result_status'], 0xe00002d8)
                self.assertEqual(len(d['entries']), 7)
                self.assertEqual(d['entries'][0]['decoded']['state'], 3)
                self.assertEqual(d['entries'][1]['decoded']['local_qpn'], 0x123456)
                self.assertTrue(d['entries'][1]['decoded']['capture']['suffix_hex'].endswith('5a'))

    def test_unattempted_and_failed_query_are_not_decoded(self):
        d = self.decoded(self.document())
        self.assertEqual(d['entries'][2]['observation']['command']['flags'], 3)
        self.assertIsNotNone(d['entries'][2]['decoded']) # Prepared send, even on timeout.
        self.assertIsNone(d['entries'][3]['decoded'])
        self.assertFalse(d['entries'][3]['observation']['invoked'])
        self.assertIsNone(d['entries'][6]['decoded'])
        self.assertTrue(d['entries'][6]['observation']['invoked'])
        self.assertEqual(d['entries'][6]['observation']['captured_bytes'], 0)
        self.assertEqual(d['identity'], 'not-established-by-this-result-ABI')

    def test_full_result_and_mailbox_hashes_bind_exact_bytes(self):
        import hashlib
        document = self.document()
        d = self.decoded(document)
        raw = bytes.fromhex(document['structHex'])
        self.assertEqual(d['result_sha256'], hashlib.sha256(raw).hexdigest())
        self.assertEqual(d['entries'][0]['mailbox_sha256'], hashlib.sha256(raw[276:540]).hexdigest())
        self.assertEqual(d['entries'][1]['mailbox_sha256'], hashlib.sha256(raw[1068:5164]).hexdigest())

    def test_successful_all_zero_query_is_still_copied(self):
        document=self.document();raw=bytearray.fromhex(document['structHex'])
        raw[1068:5164]=bytes(4096);document['structHex']=raw.hex()
        entry=self.decoded(document)['entries'][1]
        self.assertEqual(entry['observation']['captured_bytes'],4096)
        self.assertEqual(entry['decoded']['state'],0)

    def test_envelope_call_size_selector_type_and_hex_fail_closed(self):
        for key,value in [('call',5),('call',True),('selector',36),('selector',True),
                          ('structLen',17911),('structLen',True),('structHex','zz')]:
            with self.subTest(key=key,value=value):
                document=self.document();document[key]=value;self.rejected(document)
        self.rejected({})
        self.rejected([])

    def test_wrong_result_version_and_legacy_or_truncated_lengths(self):
        for offset,value in [(17452,0),(17452,2),(17456,17911)]:
            self.rejected(self.mutate(self.document(),offset,value))
        for size in (17456,17911,17913):
            document=self.document();document['structHex']=document['structHex'][:size*2]
            if size==17913:document['structHex']+='00'
            document['structLen']=size;self.rejected(document)

    def test_inconsistent_observation_lengths_headers_or_flags_are_rejected(self):
        # QP modify observation starts at 17464; command subrecord starts +24.
        for relative,value in [(0,2),(4,63),(8,263),(12,263),(20,1),
                               (24,2),(28,39),(32,8),(32,2),(32,5)]:
            with self.subTest(relative=relative,value=value):
                self.rejected(self.mutate(self.document(),17464+relative,value))

    def test_failed_query_cannot_claim_copied_bytes_or_leave_stale_bytes(self):
        self.rejected(self.mutate(self.document(),17848+12,4096))
        self.rejected(self.mutate(self.document(),13356,1))
        self.rejected(self.mutate(self.document(),17656+16,3))

    def test_uncalled_record_or_command_cannot_carry_old_evidence(self):
        self.rejected(self.mutate(self.document(),17656+64+12,4096))
        self.rejected(self.mutate(self.document(),17464+32,0))

    def test_out_of_order_timestamps_are_rejected(self):
        self.rejected(self.mutate(self.document(),17464+24+24,99,8))
        self.rejected(self.mutate(self.document(),17464+24+32,199,8))

    def test_command_cannot_exist_before_mailbox_preparation(self):
        document=self.mutate(self.document(),17464+8,0)
        self.mutate(document,17464+12,0)
        raw=bytearray.fromhex(document['structHex']);raw[276:540]=bytes(264)
        document['structHex']=raw.hex();self.rejected(document)


if __name__=='__main__':
    unittest.main(verbosity=2)
