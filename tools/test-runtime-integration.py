#!/usr/bin/env python3
"""Real identity/PCIe exporters, handlers and CLI composed over host seams.

Uses the existing actual PCIe-query harness; only OS/IOKit/config/clock transport
is synthetic. The independent decoder consumes actual CLI-emitted bytes.
"""
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
    fixture=load('runtime_pcie_fixture',ROOT/'tools/test-pcie-integration.py')
    source=fixture.harness()
    provider=(ROOT/'src/Cx3Mlx4.cpp').read_text()
    client=(ROOT/'src/Cx3Mlx4UserClient.cpp').read_text()
    cli=(ROOT/'tools/cx3ctl.c').read_text()
    additions=r'''
#define CX3_BUNDLE_VERSION "0.44.48"
#define CX3_BUILD_TAG "0.44.123-local"
static UInt64 sampleClock=1000;
static void clock_get_uptime(UInt64*v){*v=sampleClock++;}
static void absolutetime_to_nanoseconds(UInt64 v,UInt64*n){*n=v*3;}
struct CachedPci {
 UInt16 vendor=0x15b3,device=0x1007,subsystemVendor=0x15b3,subsystemId=0x1234;
 UInt8 bus=3,deviceNumber=2,function=1;
 bool valid(){return vendor && device && deviceNumber<32 && function<8;}
};
'''
    replacements=[
      ('struct IOPCIDevice {',additions+'\nstruct IOPCIDevice {\n  UInt64 getRegistryEntryID(){return 0x2000000000000002ull;}'),
      ('IOReturn queryPcieLink(Cx3PcieLinkResult*);','bool cmdWedged()const{return false;} IOReturn queryPcieLink(Cx3PcieLinkResult*);'),
      ('  Mlx4Hw fHw; IOPCIDevice *fPci;','  Mlx4Hw fHw; IOPCIDevice *fPci;\n  CachedPci fStartPciIdentity; bool fStartPciIdentityValid=true;\n  bool fStopRetained=false,fQuarExhausted=false,fFwRunAttempted=true;\n  UInt64 getRegistryEntryID(){return 0x1000000000000001ull;}\n  IOReturn exportRuntimeIdentity(Cx3RuntimeIdentityResult*);'),
      ('struct IOExternalMethodArguments {',fixture.function(provider,'static bool cx3CopyBuildIdentity(')+'\n'+fixture.function(provider,'IOReturn Cx3Mlx4::exportRuntimeIdentity(')+'\nstruct IOExternalMethodArguments {'),
      ('  IOReturn doPcieLink(IOExternalMethodArguments*);','  IOReturn doPcieLink(IOExternalMethodArguments*);\n  IOReturn doRuntimeIdentity(IOExternalMethodArguments*);'),
      ('typedef int kern_return_t;',fixture.function(client,'IOReturn Cx3Mlx4UserClient::doRuntimeIdentity(')+'\ntypedef int kern_return_t;'),
      ('  if(sel!=48) return KERN_FAILURE;','  if(sel!=48 && sel!=49) return KERN_FAILURE;'),
      ('  int ret=client->doPcieLink(&a); *sos=a.structureOutputSize;',
       '  int ret=sel==49?client->doRuntimeIdentity(&a):client->doPcieLink(&a); *sos=a.structureOutputSize;\n  if(sel==49 && oc)*oc=a.scalarOutputCount;\n  if(sel==48 && !strcmp(scenario,"generation-drift"))++client->fProvider->fDeviceGeneration;\n  if(sel==48 && !strcmp(scenario,"cached-drift"))++client->fProvider->fStartPciIdentity.subsystemId;'),
      ('int main(int argc,char **argv)', 'int pcieFixtureMain(int argc,char **argv)')]
    for old,new in replacements:
        if source.count(old)!=1: raise AssertionError(f'integration seam changed: {old}')
        source=source.replace(old,new)
    # The reused production export sets generation before invoking actual query.
    begin=cli.index('/* CX3_RUNTIME_CLI_BEGIN */')
    block=cli[begin:cli.index('/* CX3_RUNTIME_CLI_END */',begin)]
    source+=r'''
using io_service_t=unsigned;
static kern_return_t IOConnectGetService(int,io_service_t*s){*s=1;return 0;}
static kern_return_t IORegistryEntryGetRegistryEntryID(io_service_t,uint64_t*id){*id=client->fProvider->getRegistryEntryID();return 0;}
static int IOObjectRelease(io_service_t){return 0;}
static int sysctlbyname(const char*,void*out,size_t*n,void*,size_t){if(*n<37)return -1;memcpy(out,"6B43565A-082A-4C44-A357-F164DD5EB873",37);*n=37;return 0;}
'''+block+r'''
int main(int argc,char**argv){
 if(argc!=3)return 2;scenario=argv[2];IOPCIDevice pci=valid();
 if(!strcmp(scenario,"allones"))put(pci,6,0xffff,2);
 Cx3Mlx4 p(&pci);Cx3Mlx4UserClient c;c.fProvider=&p;client=&c;
 if(!strcmp(scenario,"reset"))p.fResetCommandsBlocked=true;
 int result=!strcmp(argv[1],"identity")?cmdRuntimeIdentity(1):cmdBoundPcieJson();
 fprintf(stderr,"config reads=%zu\n",pci.reads.size());return result?1:0;
}
'''
    return source


class RuntimeIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='cx3-runtime-integration-')
        cls.exe=str(Path(cls.temp.name)/'test')
        cls.decoder=load('runtime_integration_decoder',ROOT/'tools/decode-runtime.py')
        subprocess.run(['/usr/bin/clang++','-std=c++17','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                        '-I',str(ROOT/'src'),'-x','c++','-','-o',cls.exe],
                       input=harness(),text=True,check=True,timeout=60)

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def test_pure_host_identity_uses_real_export_and_handler(self):
        run=subprocess.run([self.exe,'identity','normal'],capture_output=True,text=True,timeout=10)
        self.assertEqual(run.returncode,0,run.stderr)
        r=self.decoder.decode_identity(json.loads(run.stdout));self.assertTrue(r['complete'])
        self.assertIn('config reads=0',run.stderr)

    def test_actual_query_export_handler_cli_and_decoder(self):
        run=subprocess.run([self.exe,'bound','normal'],capture_output=True,text=True,timeout=10)
        self.assertEqual(run.returncode,0,run.stderr)
        r=self.decoder.decode_bound(json.loads(run.stdout));self.assertTrue(r['binding_valid'],r['issues'])
        self.assertEqual(r['pcie']['current_link']['lanes'],1)
        self.assertNotIn('config reads=0',run.stderr)

    def test_production_refusal_and_changed_owner_fail_binding(self):
        for scenario in ('reset','allones','generation-drift','cached-drift'):
            with self.subTest(scenario=scenario):
                run=subprocess.run([self.exe,'bound',scenario],capture_output=True,text=True,timeout=10)
                self.assertNotEqual(run.returncode,0,run.stdout)
                self.assertFalse(self.decoder.decode_bound(json.loads(run.stdout))['binding_valid'])
                if scenario=='reset':self.assertIn('config reads=0',run.stderr)


if __name__=='__main__':unittest.main(verbosity=2)
