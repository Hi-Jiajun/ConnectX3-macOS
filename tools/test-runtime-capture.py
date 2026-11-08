#!/usr/bin/env python3
"""Candidate/loaded-UUID/hash binding using fake OS/CLI subprocesses only."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import uuid

HERE = Path(__file__).resolve().parent


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, HERE/filename)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


class RuntimeCaptureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.collector = load('runtime_collector', 'capture-runtime.py')
        cls.fixtures = load('runtime_collect_fixtures', 'test-runtime-decode.py')

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='cx3-capture-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.binary = self.root/'saved/Cx3Mlx4.kext/Contents/MacOS/Cx3Mlx4'
        self.binary.parent.mkdir(parents=True)
        self.image_uuid = '11111111-2222-4333-8444-555555555555'
        self.binary.write_bytes(struct.pack('<IiiIIIII',0xfeedfacf,0x1000007,3,11,1,24,0,0)+
            struct.pack('<II16s',0x1b,24,uuid.UUID(self.image_uuid).bytes)+b'fixture')
        self.cli = self.root/'saved/cx3ctl'; self.cli.write_bytes(b'CLI fixture; never executed')
        self.manifest = self.root/'verification.json'
        self.meta = dict(passed=True, buildTag='0.44.123-local',
            identity=dict(passed=True,version='0.44.48',bundleVersion='0.44.48',buildTag='0.44.123-local'),
            artifacts=[dict(path=str(p.relative_to(self.root)),sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in (self.binary,self.cli)])
        self.save_manifest()
        self.calls=[];self.mode='success';self.loaded_calls=0

    def save_manifest(self): self.manifest.write_text(json.dumps(self.meta))

    def runner(self, cmd, **kwargs):
        self.calls.append(cmd)
        if cmd[0]=='/usr/sbin/sysctl':
            text='6B43565A-082A-4C44-A357-F164DD5EB873\n'
        elif cmd[0]=='/usr/bin/kmutil':
            self.loaded_calls+=1
            value=self.image_uuid
            if self.mode=='wrong-loaded' or (self.mode=='loaded-drift' and self.loaded_calls>1): value='22222222-2222-4333-8444-555555555555'
            text=f'126 0 0xffff 0x123 0x123 com.hijiajun.driver.Cx3Mlx4 (0.44.48) {value} <1 2>\n'
            if self.mode=='duplicate-loaded': text+=text
        elif cmd==[str(self.cli),'boundpcie']:
            doc=self.fixtures.bound()
            if self.mode=='runtime-build': doc['after']=self.fixtures.identity(tag=b'other')
            if self.mode=='pcie-forged': self.fixtures.overwrite(doc['pcie'],80,b'\x01')
            if self.mode=='binary-drift': self.binary.write_bytes(self.binary.read_bytes()+b'changed')
            if self.mode=='cli-drift': self.cli.write_bytes(b'changed')
            if self.mode=='manifest-drift': self.manifest.write_text('{}')
            if self.mode=='timeout': raise subprocess.TimeoutExpired(cmd,30,output=b'{"partial":',stderr=b'timed out')
            text=json.dumps(doc)
            if self.mode=='duplicate-json': text='{"schema":"old",'+text[1:]
            if self.mode=='invalid-json': text='{"broken":'
            if self.mode=='decoder-rejection': text='{"schema":"unsupported"}'
            if self.mode=='cli-failure': return subprocess.CompletedProcess(cmd,1,text,'fake failure')
        else: raise AssertionError(f'unexpected external command: {cmd}')
        return subprocess.CompletedProcess(cmd,0,text,'')

    def capture(self):
        out=self.root/'capture'
        ok=self.collector.capture(self.manifest,out,allow_pci_read=True,runner=self.runner)
        return ok,json.loads((out/'receipt.json').read_text())

    def test_success_associates_loaded_image_and_exact_bytes(self):
        ok,r=self.capture(); self.assertTrue(ok,r)
        self.assertTrue(r['validated']);self.assertEqual(r['loaded_uuid'],self.image_uuid.upper())
        self.assertEqual(len(r['capture_file_sha256']),64)
        self.assertEqual(sum(c[0]==str(self.cli) for c in self.calls),1)
        self.assertFalse(r['dma_quiescence_proven'])

    def test_mismatch_or_mutation_is_saved_as_failure(self):
        for mode in ('wrong-loaded','duplicate-loaded','loaded-drift','runtime-build',
                     'pcie-forged','binary-drift','cli-drift','manifest-drift',
                     'timeout','cli-failure','duplicate-json','invalid-json','decoder-rejection'):
            with self.subTest(mode=mode):
                # Each fixture has independent inputs and output directory.
                self.setUp();self.mode=mode
                ok,r=self.capture();self.assertFalse(ok,r);self.assertFalse(r['validated'])
                if mode in ('wrong-loaded','duplicate-loaded'):
                    self.assertFalse(any(c[0]==str(self.cli) for c in self.calls))
                    self.assertNotIn('capture_file_sha256',r)
                else:
                    saved=self.root/'capture/boundpcie.stdout'
                    self.assertEqual(r.get('capture_file_sha256'),hashlib.sha256(saved.read_bytes()).hexdigest())
                    event=next(e for e in r['commands'] if e['stage']=='boundpcie')
                    self.assertEqual(event.get('stdout_sha256'),r['capture_file_sha256'])
                    self.assertEqual(event.get('stderr_sha256'),hashlib.sha256((self.root/'capture/boundpcie.stderr').read_bytes()).hexdigest())
                    if mode=='timeout':
                        self.assertTrue(event['timed_out'])
                        self.assertEqual(saved.read_bytes(),b'{"partial":')

    def test_bad_manifest_or_hash_does_not_invoke_cli(self):
        for change in ('failed','bad-hash','traversal','missing-uuid'):
            with self.subTest(change=change):
                self.setUp()
                if change=='failed': self.meta['passed']=False
                if change=='bad-hash': self.meta['artifacts'][0]['sha256']='0'*64
                if change=='traversal': self.meta['artifacts'][0]['path']='../escape'
                if change=='missing-uuid':
                    self.binary.write_bytes(b'not a Mach-O')
                    self.meta['artifacts'][0]['sha256']=hashlib.sha256(self.binary.read_bytes()).hexdigest()
                self.save_manifest();ok,r=self.capture()
                self.assertFalse(ok,r);self.assertEqual(self.calls,[])

    def test_no_explicit_pci_read_flag_never_starts_collection(self):
        with self.assertRaises(ValueError):
            self.collector.capture(self.manifest,self.root/'capture',allow_pci_read=False,runner=self.runner)
        self.assertEqual(self.calls,[])


if __name__=='__main__': unittest.main(verbosity=2)
