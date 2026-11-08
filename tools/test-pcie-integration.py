#!/usr/bin/env python3
"""Run actual PCIe query/export/handler/CLI functions over synthetic config.

The transport shim replaces IOConnectCallMethod, not the production functions.
The independently implemented decoder consumes actual cmdRawJson output.
This tests software composition; no IOKit, device, or hardware is accessed.
"""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


query = load("pcie_query_fixture", ROOT / "tools/test-pcie-query.py")
decoder = load("pcie_decoder", ROOT / "tools/decode-pcie.py")


def function(source, signature):
    start = source.index(signature)
    # All extracted functions end with an unindented brace.
    return source[start:source.index("\n}", start) + 2]


def harness():
    source = query.HARNESS.replace('"ABI_H"', '"cx3_abi.h"')
    source = source.replace("METHOD", query.method_source())
    source = source.replace("int main(){", "int queryFixtureMain(){")
    provider = (ROOT / "src/Cx3Mlx4.cpp").read_text()
    client = (ROOT / "src/Cx3Mlx4UserClient.cpp").read_text()
    cli = (ROOT / "tools/cx3ctl.c").read_text()
    source += r'''
#include <stdlib.h>
enum {kIOReturnNotPermitted=-10, kIOReturnNoSpace=-11, kIOReturnNotAttached=-12};
class Cx3Mlx4 {
public:
  Mlx4Hw fHw; IOPCIDevice *fPci;
  UInt64 fDeviceGeneration=0x1122334455667788ull;
  bool fResetCommandsBlocked=false, exhausted=false;
  Cx3Mlx4(IOPCIDevice *p):fHw(p),fPci(p){}
  bool retentionExhausted(){return exhausted;}
  IOReturn exportPcieLink(Cx3PcieLinkResult*);
};
EXPORT
struct IOExternalMethodArguments {
  UInt32 scalarInputCount=0, scalarOutputCount=0;
  const void *structureInput=nullptr; size_t structureInputSize=0;
  void *structureOutput=nullptr; size_t structureOutputSize=0;
};
#define LOG(...) ((void)0)
class Cx3Mlx4UserClient {
public:
  Cx3Mlx4 *fProvider=nullptr;
  IOReturn doPcieLink(IOExternalMethodArguments*);
};
HANDLER
typedef int kern_return_t;
enum { KERN_SUCCESS=0, KERN_FAILURE=5 };
static Cx3Mlx4UserClient *client;
static int gConn;
static const char *scenario;
static int openService(){return 0;}
static void breadcrumb(const char*,...) {}
static kern_return_t IOConnectCallMethod(int,unsigned sel,const uint64_t*,uint32_t ic,
 const void* si,size_t sis,uint64_t*,uint32_t* oc,void* so,size_t* sos) {
  if(sel!=48) return KERN_FAILURE;
  IOExternalMethodArguments a;
  a.scalarInputCount=ic; a.scalarOutputCount=oc?*oc:0;
  a.structureInput=si; a.structureInputSize=sis;
  a.structureOutput=so; a.structureOutputSize=*sos;
  int ret=client->doPcieLink(&a); *sos=a.structureOutputSize;
  if(ret==0 && so && *sos==368) {
    Cx3PcieLinkResult *r=(Cx3PcieLinkResult*)so;
    if(!strcmp(scenario,"spoof-capid"))r->raw[0x40]=1;
    if(!strcmp(scenario,"spoof-walk"))r->walkCount=0;
    if(!strcmp(scenario,"spoof-unsampled"))r->raw[0x20]=0x55;
    if(!strcmp(scenario,"spoof-header"))r->raw[0xe]=1;
    if(!strcmp(scenario,"spoof-bdf"))r->identityBefore.deviceNumber=r->identityAfter.deviceNumber=32;
  }
  return ret;
}
CLI_HELPERS
RAWJSON
static int boundaries(Cx3Mlx4 &p, IOPCIDevice &pci, Cx3Mlx4UserClient &c) {
  unsigned failures=0;
#define CHECK(name, expr) do{if(!(expr)){fprintf(stderr,"FAIL: %s\n",name);++failures;}}while(0)
  Cx3PcieLinkResult r; IOExternalMethodArguments a;
  a.structureOutput=&r; a.structureOutputSize=sizeof(r);
  p.fResetCommandsBlocked=true; memset(&r,0xcc,sizeof(r));
  CHECK("reset refusal is complete and before reads",c.doPcieLink(&a)==0 && r.reason==2 && pci.reads.empty() && !r.validity && r.deviceGeneration==0x1122334455667788ull);
  for(unsigned i=0;i<256;i++)CHECK("reset refusal zero raw",r.raw[i]==0);
  p.fResetCommandsBlocked=false;p.exhausted=true;
  CHECK("retention refusal before reads",c.doPcieLink(&a)==0 && r.reason==3 && pci.reads.empty());
  p.exhausted=false;p.fPci=nullptr;
  CHECK("not ready before reads",c.doPcieLink(&a)==0 && r.reason==1 && pci.reads.empty());
  p.fPci=&pci;
  a.structureOutputSize=367; CHECK("short output refused",c.doPcieLink(&a)==kIOReturnBadArgument && pci.reads.empty());
  a.structureOutputSize=369; CHECK("long output refused",c.doPcieLink(&a)==kIOReturnBadArgument && pci.reads.empty());
  a.structureOutputSize=368; a.structureOutput=nullptr;
  CHECK("null output refused",c.doPcieLink(&a)==kIOReturnBadArgument && pci.reads.empty());
  a.structureOutput=&r; a.scalarInputCount=1;
  CHECK("scalar input refused",c.doPcieLink(&a)==kIOReturnBadArgument && pci.reads.empty());
  a.scalarInputCount=0;a.scalarOutputCount=1;
  CHECK("scalar output refused",c.doPcieLink(&a)==kIOReturnBadArgument && pci.reads.empty());
  a.scalarOutputCount=0;a.structureInput=&r;
  CHECK("input pointer refused",c.doPcieLink(&a)==kIOReturnBadArgument && pci.reads.empty());
  a.structureInput=nullptr;a.structureInputSize=1;
  CHECK("input length refused",c.doPcieLink(&a)==kIOReturnBadArgument && pci.reads.empty());
  a.structureInputSize=0;c.fProvider=nullptr;
  CHECK("detached refused",c.doPcieLink(&a)==kIOReturnNotAttached && pci.reads.empty());
  printf("PCIe boundary failures: %u\n",failures);return failures?1:0;
}
int main(int argc,char **argv) {
  if(argc!=3)return 2;scenario=argv[2];
  IOPCIDevice pci=valid(!strcmp(scenario,"v2")?2:1,
    !strcmp(scenario,"unknown")?0x2077:!strcmp(scenario,"v2")?0x2843:0x2011);
  if(!strcmp(scenario,"self"))put(pci,0x40,0x4010,2);
  if(!strcmp(scenario,"back")){put(pci,0x40,0x4401,2);put(pci,0x44,0x4010,2);put(pci,0x46,1,2);put(pci,0x50,0x100083,4);put(pci,0x56,0x2011,2);}
  if(!strcmp(scenario,"low"))put(pci,0x40,0x2010,2);
  if(!strcmp(scenario,"allones"))put(pci,6,0xffff,2);
  if(!strcmp(scenario,"no-pcie"))put(pci,0x40,1,2);
  if(!strcmp(scenario,"changed"))pci.mutate[0]=2;
  if(!strcmp(scenario,"flags-changed"))pci.mutate[0x42]=2;
  if(!strcmp(scenario,"link-changed"))pci.mutate[0x52]=2;
  Cx3Mlx4 provider(&pci);Cx3Mlx4UserClient c;c.fProvider=&provider;client=&c;
  if(!strcmp(scenario,"reset"))provider.fResetCommandsBlocked=true;
  if(!strcmp(argv[1],"boundaries"))return boundaries(provider,pci,c);
  if(!strcmp(argv[1],"cli"))return cmdPcieLink()?1:0;
  char selector[]="48", out[]="out", zero[]="0", st[]="struct", len[]="368";
  char *raw[]={selector,out,zero,st,len};
  return cmdRawJson(5,raw,0);
}
'''
    return source.replace("EXPORT", function(provider, "IOReturn Cx3Mlx4::exportPcieLink(")) \
        .replace("HANDLER", function(client, "IOReturn Cx3Mlx4UserClient::doPcieLink(")) \
        .replace("CLI_HELPERS", cli[cli.index("static const char *pcieSpeed("):cli.index("\n\n/*", cli.index("static int cmdPcieLink("))]) \
        .replace("RAWJSON", function(cli, "static int cmdRawJson("))


class PcieIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="cx3-pcie-integration-")
        cls.exe = str(Path(cls.temp.name) / "test")
        built = subprocess.run(["/usr/bin/clang++", "-std=c++17", "-Wall", "-Wextra",
                                "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                "-I", str(ROOT / "src"), "-x", "c++", "-", "-o", cls.exe],
                               input=harness(), capture_output=True, text=True)
        if built.returncode:
            raise RuntimeError(built.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_fixture(self, mode, scenario):
        return subprocess.run([self.exe, mode, scenario], capture_output=True, text=True, timeout=10)

    def test_production_capture_decodes_current_and_max_separately(self):
        for scenario, current, width in (("v1", 2.5, 1), ("v2", 8, 4), ("unknown", None, None)):
            with self.subTest(scenario=scenario):
                run = self.run_fixture("raw", scenario)
                self.assertEqual(run.returncode, 0, run.stderr)
                decoded = decoder.decode_capture(json.loads(run.stdout))
                self.assertEqual(decoded["current_link"]["gt_per_second"], current)
                self.assertEqual(decoded["current_link"]["lanes"], width)
                self.assertEqual(decoded["advertised_max"]["lanes"], 8)
                self.assertEqual(decoded["provider_generation"], 0x1122334455667788)
                self.assertIn("unverified", decoded["hardware_provenance"])

    def test_production_rejects_matching_cap_bad_next_pointer(self):
        for scenario, reason in (("self", "cycle"), ("back", "cycle"), ("low", "malformed_list")):
            with self.subTest(scenario=scenario):
                run = self.run_fixture("raw", scenario)
                self.assertEqual(run.returncode, 0, run.stderr)
                capture = json.loads(run.stdout)
                payload = bytes.fromhex(capture["structHex"])
                self.assertEqual(int.from_bytes(payload[16:20], "little"),
                                 8 if reason == "cycle" else 7,
                                 "production query reported malformed list as success")
                decoded = decoder.decode_capture(capture)
                self.assertEqual(decoded["capture_state"], "failed")
                self.assertEqual(decoded["reason"], reason)
                self.assertIsNone(decoded["current_link"])

    def test_failure_capture_never_publishes_current_link(self):
        for scenario in ("reset", "changed", "flags-changed", "link-changed"):
            run = self.run_fixture("raw", scenario)
            self.assertEqual(run.returncode, 0, run.stderr)
            decoded = decoder.decode_capture(json.loads(run.stdout))
            self.assertEqual(decoded["capture_state"], "failed")
            self.assertIsNone(decoded["current_link"])

    def test_allones_observation_is_preserved_without_link_semantics(self):
        run = self.run_fixture("raw", "allones")
        self.assertEqual(run.returncode, 0, run.stderr)
        capture = json.loads(run.stdout)
        payload = bytes.fromhex(capture["structHex"])
        self.assertEqual(payload[80+6:80+8], b"\xff\xff", "retain actual all-ones observation")
        self.assertEqual(payload[336] & 0xc0, 0xc0, "bitmap means sampled, not semantically valid")
        decoded = decoder.decode_capture(capture)
        self.assertEqual(decoded["reason"], "read_unavailable")
        self.assertIsNone(decoded["current_link"])

    def test_consistent_no_pcie_diagnostic_roundtrip(self):
        run = self.run_fixture("raw", "no-pcie")
        self.assertEqual(run.returncode, 0, run.stderr)
        try:
            decoded = decoder.decode_capture(json.loads(run.stdout))
        except ValueError as error:
            self.fail("decoder rejected legitimate no-cap diagnostic: " + str(error))
        self.assertEqual(decoded["reason"], "no_pcie_capability")
        self.assertEqual(decoded["validity"], 7)
        self.assertIsNone(decoded["current_link"])

    def test_actual_export_and_handler_refuse_before_reads(self):
        run = self.run_fixture("boundaries", "v1")
        self.assertEqual(run.returncode, 0, run.stderr)

    def test_actual_cli_valid_sample(self):
        run = self.run_fixture("cli", "v1")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn("2.5 GT/s x1", run.stdout)
        self.assertIn("maximum: 8 GT/s x8", run.stdout)

    def test_cli_refuses_corrupt_capture_before_printing_link(self):
        for scenario in ("spoof-capid", "spoof-walk", "spoof-unsampled", "spoof-header", "spoof-bdf"):
            with self.subTest(scenario=scenario):
                run = self.run_fixture("cli", scenario)
                self.assertNotEqual(run.returncode, 0, scenario + " accepted: " + run.stdout)
                self.assertNotIn("negotiated current", run.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
