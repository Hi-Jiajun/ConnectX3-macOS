#!/usr/bin/env python3
"""Actual runtime/bound CLI functions over deterministic IOKit/sysctl seams."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


class RuntimeClientTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='cx3-runtime-client-')
        cls.exe = str(Path(cls.temp.name)/'test')
        cli = (ROOT/'tools/cx3ctl.c').read_text()
        begin = cli.index('/* CX3_RUNTIME_CLI_BEGIN */')
        block = cli[begin:cli.index('/* CX3_RUNTIME_CLI_END */', begin)]
        helpers = cli[cli.index('static int pcieByteWasRead('):cli.index('static int cmdPcieLink(')]
        fixtures = load('runtime_wire_fixtures', ROOT/'tools/test-runtime-decode.py')
        cls.decoder = fixtures.load('runtime_client_decoder', 'decode-runtime.py')
        identity = fixtures.identity()['rawHex']
        pcie = fixtures.bound()['pcie']['rawHex']
        code = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "cx3_abi.h"
using kern_return_t=int;using io_service_t=unsigned;
enum {KERN_SUCCESS=0,KERN_FAILURE=5};
static int gConn=0,idCalls=0,pciCalls=0,bootCalls=0;
static const char *scenario;
static bool is(const char*s){return strcmp(scenario,s)==0;}
static void bytes(const char*hex,void*out,size_t n){for(size_t i=0;i<n;i++){unsigned x=0;sscanf(hex+2*i,"%2x",&x);static_cast<uint8_t*>(out)[i]=x;}}
static int openService(){if(is("open-fail"))return -1;gConn=1;return 0;}
static kern_return_t IOConnectGetService(int,io_service_t*s){*s=1;return is("getservice-fail")?5:0;}
static kern_return_t IORegistryEntryGetRegistryEntryID(io_service_t,uint64_t*id){*id=is("connected-mismatch")?3:0x1000000000000001ull;return is("registry-fail")?5:0;}
static int IOObjectRelease(io_service_t){return 0;}
static int sysctlbyname(const char*name,void*out,size_t*n,void*,size_t){
  if(strcmp(name,"kern.bootsessionuuid") || is("boot-fail"))return -1;
  const char *boot=(is("boot-drift") && bootCalls)?"7B43565A-082A-4C44-A357-F164DD5EB873":"6B43565A-082A-4C44-A357-F164DD5EB873";
  ++bootCalls;if(*n<37)return -1;memcpy(out,boot,37);*n=37;return 0;
}
static kern_return_t IOConnectCallMethod(int,uint32_t sel,const uint64_t*,uint32_t in,
 const void*,size_t si,uint64_t*,uint32_t*count,void*out,size_t*n){
  if(in || si || !n || !out)return 5;
  if(sel==49){
    ++idCalls;if(*n!=176)return 5;bytes("IDENTITY",out,176);
    auto&r=*static_cast<Cx3RuntimeIdentityResult*>(out);r.sampleUptimeNs+=idCalls-1;
    if(is("transport"))return 5;
    if(is("short"))*n=175;
    if(is("bad-version"))r.version=2;
    if(is("reserved"))r.reservedBytes[0]=1;
    if(is("tag-unterminated"))memset(r.buildTag,'x',sizeof(r.buildTag));
    if(is("fallback"))strcpy(r.buildTag,"unversioned-local");
    if(is("blocked"))r.lifecycleFlags|=1;
    if(is("scalars") && count)*count=1;
    if(idCalls==2){
      if(is("generation-drift"))++r.deviceGeneration;
      if(is("provider-drift"))++r.providerRegistryId;
      if(is("build-drift"))r.buildTag[0]='1';
      if(is("time-backward"))r.sampleUptimeNs=1;
    }
    return 0;
  }
  if(sel==48){
    ++pciCalls;if(*n!=368)return 5;bytes("PCIE",out,368);
    auto&r=*static_cast<Cx3PcieLinkResult*>(out);
    if(is("pci-transport"))return 5;
    if(is("pci-short"))*n=367;
    if(is("pci-generation"))++r.deviceGeneration;
    if(is("pci-identity"))++r.identityBefore.subsystemId;
    if(is("pci-raw"))r.raw[0x20]=0x55;
    return 0;
  }
  return 5;
}
HELPERS
BLOCK
int main(int argc,char**argv){
 if(argc!=3)return 2;scenario=argv[2];
 int rc=!strcmp(argv[1],"identity")?cmdRuntimeIdentity(1):cmdBoundPcieJson();
 fprintf(stderr,"COUNTERS identity=%d pcie=%d boot=%d\n",idCalls,pciCalls,bootCalls);
 return rc?1:0;
}
'''.replace('IDENTITY', identity).replace('PCIE', pcie).replace('HELPERS', helpers).replace('BLOCK', block)
        subprocess.run(['/usr/bin/clang++','-std=c++11','-Wall','-Wextra','-Werror',
                        '-I',str(ROOT/'src'),'-x','c++','-','-o',cls.exe],
                       input=code,text=True,check=True,timeout=60)

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def run_cli(self, mode, scenario):
        return subprocess.run([self.exe, mode, scenario],capture_output=True,text=True,timeout=5)

    def test_actual_identity_json_decodes_without_device_query(self):
        run = self.run_cli('identity','success')
        self.assertEqual(run.returncode,0,run.stderr)
        result = self.decoder.decode_identity(json.loads(run.stdout))
        self.assertTrue(result['complete']); self.assertIn('pcie=0',run.stderr)

    def test_actual_bracketed_capture_decodes(self):
        run = self.run_cli('bound','success')
        self.assertEqual(run.returncode,0,run.stderr)
        result = self.decoder.decode_bound(json.loads(run.stdout))
        self.assertTrue(result['binding_valid'],result['issues'])
        self.assertIn('identity=2 pcie=1',run.stderr)

    def test_preflight_failure_never_reaches_pcie(self):
        for scenario in ('open-fail','getservice-fail','registry-fail','boot-fail',
                         'connected-mismatch','transport','short','bad-version',
                         'reserved','tag-unterminated','fallback','blocked','scalars'):
            with self.subTest(scenario=scenario):
                run = self.run_cli('bound',scenario)
                self.assertNotEqual(run.returncode,0,run.stdout)
                self.assertIn('pcie=0',run.stderr)

    def test_observed_drift_or_query_error_never_passes(self):
        for scenario in ('generation-drift','provider-drift','build-drift','time-backward',
                         'boot-drift','pci-transport','pci-short','pci-generation',
                         'pci-identity','pci-raw'):
            with self.subTest(scenario=scenario):
                run = self.run_cli('bound',scenario)
                self.assertNotEqual(run.returncode,0,run.stdout)
                result = self.decoder.decode_bound(json.loads(run.stdout))
                self.assertFalse(result['binding_valid'])


if __name__ == '__main__': unittest.main(verbosity=2)
