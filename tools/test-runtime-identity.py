#!/usr/bin/env python3
"""Exercise the production runtime identity export and UserClient handler."""
from pathlib import Path
import contextlib
import importlib.util
import io
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


def harness():
    provider = (ROOT / "src/Cx3Mlx4.cpp").read_text()
    client = (ROOT / "src/Cx3Mlx4UserClient.cpp").read_text()
    source = r'''#include <cstdint>
#include <cstdio>
#include <cstring>
#include "cx3_abi.h"
using UInt8=uint8_t; using UInt16=uint16_t; using UInt32=uint32_t; using UInt64=uint64_t; using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnNotAttached=-2;
#define CX3_BUNDLE_VERSION "0.2.0"
#define CX3_BUILD_TAG "candidate-123"
static UInt64 gTestTicks=987654321;
static void clock_get_uptime(UInt64 *v) { *v=gTestTicks; }
void absolutetime_to_nanoseconds(UInt64 ticks,UInt64 *v) { *v=ticks*3; }
static void logSink(const char*,...) {}
#define LOG(...) logSink(__VA_ARGS__)
struct Mlx4PciIdentity {
  UInt16 vendor=0x15b3,device=0x1007,subsystemVendor=0x15b3,subsystemId=0x0015;
  UInt8 bus=4,deviceNumber=3,function=1;
  bool valid() const { return vendor && device && deviceNumber<32 && function<8; }
};
struct Provider { UInt64 id=0xabc; UInt64 getRegistryEntryID() const { return id; } };
struct Hw { unsigned reads=0; bool wedged=false; bool cmdWedged() { ++reads; return wedged; } };
class Cx3Mlx4 {
 public:
  Mlx4PciIdentity fStartPciIdentity{}; bool fStartPciIdentityValid=true;
  UInt64 fDeviceGeneration=0x1122334455667788ull; bool fResetCommandsBlocked=false;
  bool fStopRetained=false,fQuarExhausted=false,fFwRunAttempted=false;
  Provider *fPci=nullptr; Hw fHw; UInt64 id=0x123;
  UInt64 getRegistryEntryID() const { return id; }
  IOReturn exportRuntimeIdentity(Cx3RuntimeIdentityResult*);
};
struct IOExternalMethodArguments {
 UInt32 scalarInputCount=0,scalarOutputCount=0; const void *structureInput=nullptr;
 size_t structureInputSize=0; void *structureOutput=nullptr; size_t structureOutputSize=0;
};
class Cx3Mlx4UserClient {
 public: Cx3Mlx4 *fProvider=nullptr; IOReturn doRuntimeIdentity(IOExternalMethodArguments*);
};
HELPER
EXPORT
HANDLER
static unsigned failures=0;
#define CHECK(label,expr) do{if(!(expr)){std::printf("FAIL %s\n",label);++failures;}}while(0)
int main() {
 Cx3Mlx4 p; Provider pci; p.fPci=&pci; Cx3Mlx4UserClient c; c.fProvider=&p;
 Cx3RuntimeIdentityResult r; IOExternalMethodArguments a;
 a.structureOutput=&r; a.structureOutputSize=sizeof(r);
 CHECK("normal handler success",c.doRuntimeIdentity(&a)==0);
 CHECK("fixed ABI",sizeof(r)==176 && r.version==1 && r.structBytes==176);
 CHECK("registry and generation",r.serviceRegistryId==0x123 && r.providerRegistryId==0xabc &&
       r.deviceGeneration==0x1122334455667788ull && r.sampleUptimeNs==2962962963ull);
 CHECK("valid bits",(r.validity&0x3f)==0x3f);
 CHECK("PCI snapshot",r.pciIdentity.vendor==0x15b3 && r.pciIdentity.bus==4 && r.pciIdentity.deviceNumber==3);
 CHECK("metadata",!std::strcmp(r.bundleVersion,"0.2.0") && !std::strcmp(r.buildTag,"candidate-123"));
 CHECK("zero reserved",r.reserved==0 && r.reservedBytes[0]==0 && r.reservedBytes[1]==0 && r.reservedBytes[2]==0);
 CHECK("hardware boundary",p.fHw.reads==1);
 char bad[16]{};
 CHECK("dev build metadata rejected",!cx3CopyBuildIdentity(bad,sizeof(bad),"dev"));
 CHECK("unversioned metadata rejected",!cx3CopyBuildIdentity(bad,sizeof(bad),"unversioned"));
 char tiny[3]{}; CHECK("truncated metadata rejected",!cx3CopyBuildIdentity(tiny,sizeof(tiny),"4.2"));
 p.fDeviceGeneration++; Cx3RuntimeIdentityResult next{}; p.exportRuntimeIdentity(&next);
 CHECK("stable service identity across generation",next.serviceRegistryId==r.serviceRegistryId &&
       next.deviceGeneration==r.deviceGeneration+1);
 p.fResetCommandsBlocked=p.fStopRetained=p.fQuarExhausted=p.fFwRunAttempted=true; p.fHw.wedged=true;
 p.exportRuntimeIdentity(&next);
 CHECK("lifecycle flags",next.lifecycleFlags==31);
 p.fStartPciIdentityValid=false; p.exportRuntimeIdentity(&next);
 CHECK("unknown PCI omitted",!(next.validity&CX3_RUNTIME_VALID_PCI_SNAPSHOT) && next.pciIdentity.vendor==0);
 p.fDeviceGeneration=0; p.exportRuntimeIdentity(&next);
 CHECK("unknown generation omitted",!(next.validity&CX3_RUNTIME_VALID_GENERATION));
 gTestTicks=0;p.exportRuntimeIdentity(&next);
 CHECK("unavailable clock is not marked valid",!(next.validity&CX3_RUNTIME_VALID_UPTIME));
 gTestTicks=987654321;
 p.fDeviceGeneration=0x1122334455667789ull;
 p.fStartPciIdentityValid=true;
 a.structureOutputSize--; CHECK("short output",c.doRuntimeIdentity(&a)==kIOReturnBadArgument);
 a.structureOutputSize+=2; CHECK("long output",c.doRuntimeIdentity(&a)==kIOReturnBadArgument);
 a.structureOutputSize=sizeof(r); a.structureOutput=nullptr; CHECK("null output",c.doRuntimeIdentity(&a)==kIOReturnBadArgument);
 a.structureOutput=&r; a.scalarInputCount=1; CHECK("scalar input",c.doRuntimeIdentity(&a)==kIOReturnBadArgument);
 a.scalarInputCount=0; a.scalarOutputCount=1; CHECK("scalar output",c.doRuntimeIdentity(&a)==kIOReturnBadArgument);
 a.scalarOutputCount=0; a.structureInput=&r; CHECK("structure input pointer",c.doRuntimeIdentity(&a)==kIOReturnBadArgument);
 a.structureInput=nullptr; a.structureInputSize=1; CHECK("structure input size",c.doRuntimeIdentity(&a)==kIOReturnBadArgument);
 CHECK("null arguments",c.doRuntimeIdentity(nullptr)==kIOReturnBadArgument);
 a.structureInputSize=0; c.fProvider=nullptr; CHECK("detached",c.doRuntimeIdentity(&a)==kIOReturnNotAttached);
 std::printf("runtime identity: %u failures\n",failures); return failures?1:0;
}
'''
    return (source.replace("HELPER", function(provider, "static bool cx3CopyBuildIdentity("))
            .replace("EXPORT", function(provider, "IOReturn Cx3Mlx4::exportRuntimeIdentity("))
            .replace("HANDLER", function(client, "IOReturn Cx3Mlx4UserClient::doRuntimeIdentity(")))


class RuntimeIdentity(unittest.TestCase):
    def test_selector_checker_counts_every_row_and_abi_total(self):
        spec = importlib.util.spec_from_file_location('selector_checker', ROOT/'tools/check-selector-tables.py')
        checker = importlib.util.module_from_spec(spec); spec.loader.exec_module(checker)
        original = (ROOT/'src/Cx3Mlx4UserClient.cpp').read_text()
        cases = {
            'complete': (original, 0),
            'missing-both-labels': (original.replace('/* runtime identity */', '').replace('/* 49 runtime identity */', ''), 1),
            'wrong-final-number': (original.replace('/* 49 runtime identity */', '/* 48 runtime identity */'), 1),
            'wrong-handler-label': (original.replace('/* runtime identity */', '/* unrelated */'), 1),
        }
        with tempfile.TemporaryDirectory(prefix='cx3-selector-check-') as tmp:
            checker.SRC = Path(tmp)/'Cx3Mlx4UserClient.cpp'
            for name, (source, expected) in cases.items():
                with self.subTest(name=name):
                    checker.SRC.write_text(source)
                    output = io.StringIO()
                    with contextlib.redirect_stdout(output): result = checker.main()
                    self.assertEqual(result, expected, output.getvalue())
                    if expected == 0:
                        self.assertIn('dispatch rows 50, lock-class rows 50', output.getvalue())

    def test_actual_export_and_handler(self):
        provider = (ROOT / "src/Cx3Mlx4.cpp").read_text()
        client = (ROOT / "src/Cx3Mlx4UserClient.cpp").read_text()
        self.assertIn("static bool cx3CopyBuildIdentity(", provider)
        self.assertIn("if (selector != (uint32_t)kCx3SelRuntimeIdentity)\n            fProvider->freezeInstrumentsSweep", client)
        helper = provider[provider.index("static bool cx3CopyBuildIdentity("):provider.index("\n}\n", provider.index("static bool cx3CopyBuildIdentity(")) + 3]
        src = harness().replace("HELPER", helper)
        src = src.replace('UInt8=uint8_t; using UInt32=uint32_t; using UInt64=uint64_t;',
                          'UInt8=uint8_t; using UInt16=uint16_t; using UInt32=uint32_t; using UInt64=uint64_t;')
        with tempfile.TemporaryDirectory(prefix="cx3-runtime-identity-") as tmp:
            binary = str(Path(tmp) / "test")
            built = subprocess.run(["/usr/bin/clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                    "-I", str(ROOT / "src"), "-x", "c++", "-", "-o", binary],
                                   input=src, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            run = subprocess.run([binary], capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
