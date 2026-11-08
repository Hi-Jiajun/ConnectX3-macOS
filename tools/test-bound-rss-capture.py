#!/usr/bin/env python3
"""Frozen-candidate RSS collection with real CLI output over a host transport."""
import copy
import hashlib
import importlib.util
import inspect
import json
from pathlib import Path
import subprocess
import unittest

HERE=Path(__file__).resolve().parent


def load(name,filename):
    spec=importlib.util.spec_from_file_location(name,HERE/filename)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module


base=load('rss_candidate_base','test-runtime-capture.py')


class RssCapture(unittest.TestCase):
    save_manifest=base.RuntimeCaptureTests.save_manifest

    @classmethod
    def setUpClass(cls):
        cls.collector=load('rss_collector','capture-runtime.py')
        cls.fixtures=load('rss_collect_ids','test-runtime-decode.py')
        client=load('rss_capture_client','test-bound-rss-client.py').BoundRssClient
        client.setUpClass();cls.addClassCleanup(client.tearDownClass)
        cls.normal=json.loads(subprocess.check_output([client.exe,'rss','success'],text=True,stderr=subprocess.DEVNULL))
        r=subprocess.run([client.exe,'rss','device-fail'],capture_output=True,text=True,check=False)
        cls.failed=json.loads(r.stdout)
        r=subprocess.run([client.exe,'rss','selector-refused'],capture_output=True,text=True,check=False)
        cls.refused=json.loads(r.stdout)

    def setUp(self):
        base.RuntimeCaptureTests.setUp(self)
        self.meta['buildTag']=self.meta['identity']['buildTag']='0.44.128-local';self.save_manifest()

    def runner(self,cmd,**kwargs):
        if cmd!=[str(self.cli),'boundrss','--allow-rss-experiment']:
            return base.RuntimeCaptureTests.runner(self,cmd,**kwargs)
        self.calls.append(cmd);document=copy.deepcopy(self.normal)
        if self.mode=='timeout':raise subprocess.TimeoutExpired(cmd,30,output=b'{"partial":',stderr=b'timed out')
        if self.mode=='runtime-build':document['after']=self.fixtures.identity(tag=b'other')
        if self.mode=='rss-time':
            raw=bytearray.fromhex(document['rss']['structHex']);raw[18120:18128]=(1001).to_bytes(8,'little');document['rss']['structHex']=raw.hex()
        if self.mode=='cli-drift':self.cli.write_bytes(b'changed')
        if self.mode in ('device-fail','device-fail-zero-exit'):document=copy.deepcopy(self.failed)
        if self.mode=='selector-refused-zero-exit':document=copy.deepcopy(self.refused)
        return subprocess.CompletedProcess(cmd,1 if self.mode=='device-fail' else 0,json.dumps(document),'')

    def capture(self,allow=True):
        self.assertIn('observation',inspect.signature(self.collector.capture).parameters,'RSS collector mode is missing')
        output=self.root/'capture'
        ok=self.collector.capture(self.manifest,output,observation='rss',allow_rss_experiment=allow,runner=self.runner)
        return ok,json.loads((output/'receipt.json').read_text())

    def test_success_binds_actual_cli_result_to_frozen_candidate(self):
        ok,r=self.capture();self.assertTrue(ok,r)
        self.assertEqual(r['observation'],'rss');self.assertTrue(r['operation_succeeded'])
        self.assertTrue(r['identity_binding_valid']);self.assertFalse(r['dma_quiescence_proven'])
        self.assertEqual(sum(c[0]==str(self.cli) for c in self.calls),1)
        self.assertEqual(r['capture_file_sha256'],hashlib.sha256((self.root/'capture/boundrss.stdout').read_bytes()).hexdigest())

    def test_wrong_image_never_invokes_experiment(self):
        self.mode='wrong-loaded';ok,r=self.capture();self.assertFalse(ok,r)
        self.assertFalse(any(c[0]==str(self.cli) for c in self.calls))

    def test_failure_drift_timeout_never_retries_and_preserves_bytes(self):
        for mode in ('runtime-build','rss-time','cli-drift','loaded-drift','timeout'):
            with self.subTest(mode=mode):
                self.setUp();self.mode=mode;ok,r=self.capture();self.assertFalse(ok,r)
                self.assertEqual(sum(c[0]==str(self.cli) for c in self.calls),1)
                self.assertEqual(r['capture_file_sha256'],hashlib.sha256((self.root/'capture/boundrss.stdout').read_bytes()).hexdigest())

    def test_device_failure_keeps_binding_without_claiming_success(self):
        for mode in ('device-fail','device-fail-zero-exit','selector-refused-zero-exit'):
            with self.subTest(mode=mode):
                self.setUp();self.mode=mode;ok,r=self.capture();self.assertFalse(ok,r)
                self.assertTrue(r['identity_binding_valid']);self.assertFalse(r['operation_succeeded'])
                self.assertTrue((self.root/'capture/decoded.json').exists())

    def test_missing_optin_and_unknown_observation_have_no_effects(self):
        self.assertIn('observation',inspect.signature(self.collector.capture).parameters,'RSS collector mode is missing')
        for kw in (dict(observation='rss',allow_pci_read=True),dict(observation='arbitrary',allow_rss_experiment=True)):
            with self.subTest(kw=kw):
                with self.assertRaises(ValueError):self.collector.capture(self.manifest,self.root/'capture',runner=self.runner,**kw)
                self.assertEqual(self.calls,[]);self.assertFalse((self.root/'capture').exists())


if __name__=='__main__':unittest.main(verbosity=2)
