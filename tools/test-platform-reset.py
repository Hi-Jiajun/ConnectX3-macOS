#!/usr/bin/env python3
"""Execute production platformFunctionReset; fake only IOPCIDevice boundary.

Asserts:
 - Null fPci returns kIOReturnNotReady with zero device calls.
 - Non-null fPci invokes reset with exact kIOPCIDeviceResetTypeFunctionReset
   and kIOPCIDeviceResetOptionNone (OptionTerminate forbidden).
 - Exactly one call (no retry on failure).
 - Exact status propagation (success, unsupported, error, notready, etc.).
 - No config/BAR reads, sleeps, or poison clearing.

Mutation checks verify the test catches altered type, option, status, null
return code, or retry attempts.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

SRC = Path(__file__).resolve().parents[1] / 'src'


def method(text, signature):
    if signature not in text:
        return None
    start = text.index(signature)
    return text[start:text.index('\n}\n', start) + 3]


HARNESS_TEMPLATE = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#define CX3_HOST_TEST 1
#include "mlx4_pure.h"

using UInt8 = uint8_t;
using UInt16 = uint16_t;
using UInt32 = uint32_t;
using UInt64 = uint64_t;
using IOReturn = int;

constexpr IOReturn kIOReturnSuccess     = 0;
constexpr IOReturn kIOReturnError       = -1;
constexpr IOReturn kIOReturnNotReady    = -2;
constexpr IOReturn kIOReturnUnsupported = -4;
constexpr IOReturn kIOReturnBusy        = -5;
constexpr IOReturn kIOReturnTimeout     = -6;
constexpr IOReturn kIOReturnBadArgument = -7;

typedef enum tIOPCIDeviceResetTypes
{
    kIOPCIDeviceResetTypeHotReset         = 0x00000001,
    kIOPCIDeviceResetTypeWarmReset        = 0x00000002,
    kIOPCIDeviceResetTypeWarmResetDisable = 0x00000004,
    kIOPCIDeviceResetTypeWarmResetEnable  = 0x00000008,
    kIOPCIDeviceResetTypeFunctionReset    = 0x00000010,
} tIOPCIDeviceResetTypes;

typedef enum tIOPCIDeviceResetOptions
{
    kIOPCIDeviceResetOptionNone      = 0x00000000,
    kIOPCIDeviceResetOptionTerminate = 0x00000001,
} tIOPCIDeviceResetOptions;

struct IOPCIDevice {
    unsigned resetCalls = 0;
    tIOPCIDeviceResetTypes lastType = (tIOPCIDeviceResetTypes)0;
    tIOPCIDeviceResetOptions lastOptions = (tIOPCIDeviceResetOptions)0;
    IOReturn returnStatus = kIOReturnSuccess;
    unsigned configReads = 0;
    unsigned configWrites = 0;
    UInt8 bus = 4, device = 3, function = 1;
    UInt16 vendor = 0x15b3, deviceId = 0x1007;
    UInt16 subsystemVendor = 0x15b3, subsystemId = 0x0015;

    IOReturn reset(tIOPCIDeviceResetTypes type,
                   tIOPCIDeviceResetOptions options = kIOPCIDeviceResetOptionNone) {
        ++resetCalls;
        lastType = type;
        lastOptions = options;
        return returnStatus;
    }
    UInt32 configRead32(UInt8) { ++configReads; return 0; }
    void configWrite32(UInt8, UInt32) { ++configWrites; }
    UInt8 getBusNumber() { return bus; }
    UInt8 getDeviceNumber() { return device; }
    UInt8 getFunctionNumber() { return function; }
    UInt16 configRead16(UInt8 off) {
        ++configReads;
        if (off == 0x00) return vendor;
        if (off == 0x02) return deviceId;
        if (off == 0x2c) return subsystemVendor;
        if (off == 0x2e) return subsystemId;
        return 0xffff;
    }
};

static unsigned sleepCalls = 0;
static void IOSleep(unsigned) { ++sleepCalls; }

class Mlx4Hw {
public:
    IOPCIDevice *fPci = nullptr;
    unsigned poisonClears = 0;
    unsigned barReads = 0;
    bool fCmdWedged = false;

    void cmdClearWedged(const char *) { ++poisonClears; }
    UInt32 readRegRaw(UInt32) { ++barReads; return 0; }
    UInt32 readHcrStatus() { ++barReads; return 0; }

    IOReturn platformFunctionReset();
    IOReturn readPciIdentity(Mlx4PciIdentity *out);
};

CODE

static unsigned cases = 0, checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL case %u line %u: %s\n", cases, __LINE__, #x); } } while(0)

int main() {
    {
        ++cases;
        Mlx4Hw hw;
        CHECK(hw.readPciIdentity(nullptr)==kIOReturnBadArgument);
    }
    // Captured identity binds the provider's PCI location and IDs together.
    {
        ++cases;
        Mlx4Hw hw; IOPCIDevice pci; Mlx4PciIdentity id{};
        hw.fPci=&pci;
        CHECK(hw.readPciIdentity(&id)==kIOReturnSuccess);
        CHECK(id.valid());
        CHECK(id.vendor==0x15b3 && id.device==0x1007);
        CHECK(id.subsystemVendor==0x15b3 && id.subsystemId==0x0015);
        CHECK(id.bus==4 && id.deviceNumber==3 && id.function==1);
        CHECK(pci.configReads==4 && pci.resetCalls==0);
    }
    {
        ++cases;
        Mlx4Hw hw; Mlx4PciIdentity id{};
        CHECK(hw.readPciIdentity(&id)==kIOReturnNotReady);
        CHECK(!id.valid());
    }
    for (unsigned mode=0;mode<4;++mode) {
        ++cases;
        Mlx4Hw hw; IOPCIDevice pci; Mlx4PciIdentity id{}; hw.fPci=&pci;
        if(mode==0)pci.vendor=0xffff;
        if(mode==1)pci.deviceId=0xffff;
        if(mode==2)pci.subsystemVendor=0xffff;
        if(mode==3)pci.subsystemId=0xffff;
        CHECK(hw.readPciIdentity(&id)==kIOReturnNotReady);
        CHECK(!id.valid() && pci.resetCalls==0);
    }
    // Case 1: Null fPci returns kIOReturnNotReady with 0 platform reset calls
    {
        ++cases;
        Mlx4Hw hw;
        IOPCIDevice pci;
        hw.fPci = nullptr;
        sleepCalls = 0;

        IOReturn ret = hw.platformFunctionReset();
        CHECK(ret == kIOReturnNotReady);
        CHECK(pci.resetCalls == 0);
        CHECK(pci.configReads == 0 && pci.configWrites == 0);
        CHECK(sleepCalls == 0);
        CHECK(hw.poisonClears == 0 && hw.barReads == 0);
    }

    // Case 2: Exact type (kIOPCIDeviceResetTypeFunctionReset) and options (kIOPCIDeviceResetOptionNone)
    {
        ++cases;
        Mlx4Hw hw;
        IOPCIDevice pci;
        hw.fPci = &pci;
        pci.returnStatus = kIOReturnSuccess;
        sleepCalls = 0;

        IOReturn ret = hw.platformFunctionReset();
        CHECK(ret == kIOReturnSuccess);
        CHECK(pci.resetCalls == 1);
        CHECK(pci.lastType == kIOPCIDeviceResetTypeFunctionReset);
        CHECK(pci.lastOptions == kIOPCIDeviceResetOptionNone);
        CHECK((pci.lastOptions & kIOPCIDeviceResetOptionTerminate) == 0);
        CHECK(pci.configReads == 0 && pci.configWrites == 0);
        CHECK(sleepCalls == 0);
        CHECK(hw.poisonClears == 0 && hw.barReads == 0);
    }

    // Cases 3-9: Exact status propagation (success, unsupported, error, notready, busy, timeout, custom)
    IOReturn testStatuses[] = {
        kIOReturnSuccess,
        kIOReturnUnsupported,
        kIOReturnError,
        kIOReturnNotReady,
        kIOReturnBusy,
        kIOReturnTimeout,
        static_cast<IOReturn>(0x12345678)
    };

    for (IOReturn expectedStatus : testStatuses) {
        ++cases;
        Mlx4Hw hw;
        IOPCIDevice pci;
        hw.fPci = &pci;
        pci.returnStatus = expectedStatus;
        sleepCalls = 0;

        IOReturn ret = hw.platformFunctionReset();
        CHECK(ret == expectedStatus);
        CHECK(pci.resetCalls == 1); // Exactly one call, no retry on error
        CHECK(pci.lastType == kIOPCIDeviceResetTypeFunctionReset);
        CHECK(pci.lastOptions == kIOPCIDeviceResetOptionNone);
        CHECK(pci.configReads == 0 && pci.configWrites == 0);
        CHECK(sleepCalls == 0);
        CHECK(hw.poisonClears == 0 && hw.barReads == 0);
    }

    std::printf("platform reset: %u cases, %u checks, %u failures (production method body; synthetic IOPCIDevice)\n",
                cases, checks, failures);
    return failures ? 1 : 0;
}
'''


def compile_and_run(code_str):
    harness = HARNESS_TEMPLATE.replace('CODE', code_str)
    with tempfile.TemporaryDirectory(prefix='cx3-platform-reset-') as tmp:
        binary = str(Path(tmp) / 'test')
        res = subprocess.run(
            ['/usr/bin/clang++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-function', '-I', str(SRC), '-x', 'c++', '-', '-o', binary],
            input=harness, text=True, capture_output=True)
        if res.returncode != 0:
            return res.returncode, res.stdout, res.stderr
        run = subprocess.run([binary], timeout=10, capture_output=True, text=True)
        return run.returncode, run.stdout, run.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path)
    parser.add_argument('--skip-mutation', action='store_true')
    args = parser.parse_args()

    source_text = (args.source or SRC / 'Mlx4Hw.cpp').read_text()
    reset_code = method(source_text, 'IOReturn Mlx4Hw::platformFunctionReset(')
    identity_code = method(source_text, 'IOReturn Mlx4Hw::readPciIdentity(')

    if reset_code is None or identity_code is None:
        sys.stderr.write("ERROR: required production reset/identity method not found in source\n")
        # Attempt compilation without method body to demonstrate red compile/link failure
        rc, out, err = compile_and_run('')
        sys.stderr.write(err)
        return 1

    code = identity_code + '\n' + reset_code
    rc, out, err = compile_and_run(code)
    sys.stdout.write(out)
    sys.stderr.write(err)
    if rc != 0:
        return rc

    if not args.skip_mutation:
        # Mutation 1: Mutated reset type
        mut1 = code.replace('kIOPCIDeviceResetTypeFunctionReset', 'kIOPCIDeviceResetTypeHotReset')
        rc1, _, _ = compile_and_run(mut1)
        if rc1 == 0:
            sys.stderr.write("MUTATION FAILED: type change to HotReset was not caught!\n")
            return 2

        # Mutation 2: Mutated reset option to OptionTerminate
        mut2 = code.replace('kIOPCIDeviceResetOptionNone', 'kIOPCIDeviceResetOptionTerminate')
        rc2, _, _ = compile_and_run(mut2)
        if rc2 == 0:
            sys.stderr.write("MUTATION FAILED: option change to OptionTerminate was not caught!\n")
            return 2

        # Mutation 3: Return constant success instead of platform status
        mut3 = code.replace('return fPci->reset', 'fPci->reset(kIOPCIDeviceResetTypeFunctionReset, kIOPCIDeviceResetOptionNone); return kIOReturnSuccess; //')
        rc3, _, _ = compile_and_run(mut3)
        if rc3 == 0:
            sys.stderr.write("MUTATION FAILED: constant success return was not caught!\n")
            return 2

        # Mutation 4: Wrong return on null fPci
        mut4 = code.replace('return kIOReturnNotReady;', 'return kIOReturnError;')
        rc4, _, _ = compile_and_run(mut4)
        if rc4 == 0:
            sys.stderr.write("MUTATION FAILED: null return change to kIOReturnError was not caught!\n")
            return 2

        # Mutation 5: Retry on failure
        retry_snippet = (
            'IOReturn r = fPci->reset(kIOPCIDeviceResetTypeFunctionReset, kIOPCIDeviceResetOptionNone);\n'
            '    if (r != 0) r = fPci->reset(kIOPCIDeviceResetTypeFunctionReset, kIOPCIDeviceResetOptionNone);\n'
            '    return r;\n//'
        )
        mut5 = code.replace('return fPci->reset(kIOPCIDeviceResetTypeFunctionReset, kIOPCIDeviceResetOptionNone);', retry_snippet)
        rc5, _, _ = compile_and_run(mut5)
        if rc5 == 0:
            sys.stderr.write("MUTATION FAILED: retry on failure was not caught!\n")
            return 2

        sys.stdout.write("mutations: 5 tested, 5 caught (type, option, status, null-return, retry)\n")

    return 0


if __name__ == '__main__':
    raise SystemExit(main())
