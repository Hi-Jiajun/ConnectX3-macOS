#!/usr/bin/env python3
"""Actual bound-RSS CLI over recording transport; compiler-laid-out result ABI."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module


def harness():
    cli=(ROOT/'tools/cx3ctl.c').read_text()
    start=cli.index('/* CX3_RUNTIME_CLI_BEGIN */')
    runtime=cli[start:cli.index('/* CX3_RUNTIME_CLI_END */',start)]
    helpers=cli[cli.index('static int pcieByteWasRead('):cli.index('static int cmdPcieLink(')]
    begin='/* CX3_BOUND_RSS_CLI_BEGIN */'
    if begin in cli:
        start=cli.index(begin);rss=cli[start:cli.index('/* CX3_BOUND_RSS_CLI_END */',start)]
    else:
        rss='static int cmdBoundRssJson(int){fputs("missing boundrss endpoint\\n",stderr);return 2;}'
    code=r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "cx3_abi.h"
using kern_return_t=int;using io_service_t=unsigned;
enum {KERN_SUCCESS=0,KERN_FAILURE=5};
static int gConn=0,idCalls=0,rssCalls=0,bootCalls=0;
static const char *scenario;
static bool is(const char*s){return strcmp(scenario,s)==0;}
static int openService(){if(is("open-fail"))return -1;gConn=1;return 0;}
static kern_return_t IOConnectGetService(int,io_service_t*s){*s=1;return 0;}
static kern_return_t IORegistryEntryGetRegistryEntryID(io_service_t,uint64_t*id){*id=0x1000000000000001ull;return 0;}
static int IOObjectRelease(io_service_t){return 0;}
static int sysctlbyname(const char*,void*out,size_t*n,void*,size_t){
 ++bootCalls;if(*n<37)return -1;
 memcpy(out,is("boot-drift") && bootCalls>1?"7B43565A-082A-4C44-A357-F164DD5EB873":"6B43565A-082A-4C44-A357-F164DD5EB873",37);*n=37;return 0;
}
static void observation(Cx3QpCaptureObservation *o,unsigned size){
 o->abiVersion=1;o->abiBytes=64;o->preparedBytes=size;o->capturedBytes=size;
 o->command.abiVersion=1;o->command.abiBytes=40;o->command.flags=7;
 o->command.startNs=100;o->command.postedNs=200;o->command.endNs=300;
}
static kern_return_t IOConnectCallMethod(int conn,uint32_t sel,const uint64_t*,uint32_t in,
 const void*,size_t si,uint64_t*out,uint32_t*count,void*bytes,size_t*n){
 if(conn!=1 || in || si || !n || !bytes)return 5;
 if(sel==49){
  ++idCalls;if(*n!=176 || !count || *count!=0)return 5;
  auto&r=*static_cast<Cx3RuntimeIdentityResult*>(bytes);memset(&r,0,sizeof(r));
  r.version=1;r.structBytes=176;r.validity=63;r.serviceRegistryId=0x1000000000000001ull;
  r.providerRegistryId=0x2000000000000002ull;r.deviceGeneration=7;r.sampleUptimeNs=idCalls==1?10:1000;
  r.lifecycleFlags=16;r.pciIdentity={0x15b3,0x1007,0x15b3,0x1234,3,0,0,0};
  strcpy(r.bundleVersion,"0.44.48");strcpy(r.buildTag,"0.44.128-local");
  if(is("blocked"))r.lifecycleFlags|=1;
  if(is("identity-fail"))return 5;
  if(idCalls==2){if(is("generation-drift"))++r.deviceGeneration;
   if(is("provider-drift"))++r.providerRegistryId;
   if(is("build-drift")){memset(r.buildTag,0,sizeof(r.buildTag));strcpy(r.buildTag,"changed");}}
  return 0;
 }
 if(sel==35){
  ++rssCalls;if(*n!=sizeof(Cx3R4IngressResult) || !out || !count || *count!=15)return 5;
  auto&r=*static_cast<Cx3R4IngressResult*>(bytes);memset(&r,0,sizeof(r));
  r.abiVersion=CX3_R4INGRESS_ABI;r.abiBytes=sizeof(r);r.qpn=65;
  observation(&r.modifyCapture[0],264);observation(&r.queryCapture[0],4096);
  observation(&r.finalQueryCapture,4096);
  r.queryOk=1;r.queryState=3;
  r.rawSentMailboxStep[0][8]=0x30;r.rawQueryMailboxStep[0][27]=0x41;
  r.rawQueryMailboxFinal[8]=0x30;r.rawQueryMailboxFinal[27]=0x41;
  if(is("selector-refused")){
   r.modifyCapture[0].fwStatus=3;r.modifyCapture[0].command.fwStatus=3;
   memset(&r.queryCapture[0],0,sizeof(r.queryCapture[0]));memset(r.rawQueryMailboxStep[0],0,4096);
   memset(&r.finalQueryCapture,0,sizeof(r.finalQueryCapture));memset(r.rawQueryMailboxFinal,0,4096);
   r.queryOk=0;r.queryState=0;
  }
  if(is("device-fail"))r.status=0xe00002d8;
  out[0]=r.status;out[5]=r.qpn;out[8]=r.queryOk;out[9]=r.queryState;
  if(is("transport-fail"))return 5;
  if(is("short"))*n=18511;
  if(is("scalars"))*count=14;
  if(is("bad-abi"))r.abiVersion=2;
  if(is("scalar-mismatch"))++out[0];
  if(is("time-outside"))r.modifyCapture[0].command.endNs=1001;
  return 0;
 }
 return 5;
}
HELPERS
RUNTIME
RSS
int main(int argc,char**argv){
 if(argc!=3)return 2;scenario=argv[2];int rc;
 if(!strcmp(argv[1],"identity"))rc=cmdRuntimeIdentity(1);
 else if(!strcmp(argv[1],"pcie"))rc=cmdBoundPcieJson();
 else rc=cmdBoundRssJson(strcmp(argv[1],"no-optin")!=0);
 fprintf(stderr,"COUNTERS identity=%d rss=%d boot=%d\n",idCalls,rssCalls,bootCalls);
 return rc?1:0;
}
'''
    return code.replace('HELPERS',helpers).replace('RUNTIME',runtime).replace('RSS',rss)


class BoundRssClient(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='cx3-bound-rss-');cls.exe=str(Path(cls.temp.name)/'test')
        cls.decoder=load('rss_client_decoder',ROOT/'tools/decode-runtime.py')
        subprocess.run(['/usr/bin/clang++','-std=c++17','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I',str(ROOT/'src'),
                        '-x','c++','-','-o',cls.exe],input=harness(),text=True,check=True)

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def run_cli(self,scenario='success',mode='rss'):
        return subprocess.run([self.exe,mode,scenario],capture_output=True,text=True,timeout=10)

    def test_actual_cli_one_connection_one_operation_and_independent_decode(self):
        r=self.run_cli();self.assertEqual(r.returncode,0,r.stderr)
        d=self.decoder.decode_bound_rss(json.loads(r.stdout));self.assertTrue(d['binding_valid'],d['issues'])
        self.assertTrue(d['operation_succeeded']);self.assertEqual(d['rss']['qpn'],65)
        self.assertIn('identity=2 rss=1',r.stderr)

    def test_no_optin_or_bad_identity_never_reaches_rss(self):
        for scenario,mode in (('success','no-optin'),('open-fail','rss'),('identity-fail','rss'),('blocked','rss')):
            with self.subTest(scenario=scenario,mode=mode):
                r=self.run_cli(scenario,mode);self.assertNotEqual(r.returncode,0)
                self.assertIn('rss=0',r.stderr)

    def test_failure_and_drift_never_retries_or_claims_binding(self):
        for scenario in ('generation-drift','provider-drift','build-drift','boot-drift','transport-fail',
                         'short','scalars','bad-abi','scalar-mismatch'):
            with self.subTest(scenario=scenario):
                r=self.run_cli(scenario);self.assertNotEqual(r.returncode,0,r.stdout)
                self.assertTrue(r.stdout.strip().startswith('{'),r.stderr)
                d=self.decoder.decode_bound_rss(json.loads(r.stdout));self.assertFalse(d['binding_valid'])
                self.assertIn('identity=2 rss=1',r.stderr)

    def test_device_error_preserves_bound_diagnostics_but_returns_failure(self):
        r=self.run_cli('device-fail');self.assertNotEqual(r.returncode,0)
        self.assertTrue(r.stdout.strip().startswith('{'),r.stderr)
        d=self.decoder.decode_bound_rss(json.loads(r.stdout));self.assertTrue(d['binding_valid'],d['issues'])
        self.assertFalse(d['operation_succeeded']);self.assertEqual(len(d['rss']['entries']),7)

    def test_real_selector_refusal_status_zero_is_not_operation_success(self):
        r=self.run_cli('selector-refused');self.assertNotEqual(r.returncode,0,r.stdout)
        d=self.decoder.decode_bound_rss(json.loads(r.stdout));self.assertTrue(d['binding_valid'],d['issues'])
        self.assertEqual(d['rss']['result_status'],0);self.assertFalse(d['operation_succeeded'])

    def test_consumer_rejects_outside_bracket_command_clock(self):
        r=self.run_cli('time-outside');self.assertTrue(r.stdout.strip().startswith('{'),r.stderr)
        d=self.decoder.decode_bound_rss(json.loads(r.stdout))
        self.assertFalse(d['binding_valid'])


if __name__=='__main__':unittest.main(verbosity=2)
