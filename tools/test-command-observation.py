#!/usr/bin/env python3
"""Observe real cmdBox/Locked transitions; only PCI registers/clock/locks are fake.

Catches marking preparation as posting, dropping the posted fact on timeout/FW
error, stale reused observations, and observations that invent a reply on refusal.
The host register array cannot prove the device received a PCIe write.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

SRC = Path(__file__).resolve().parents[1] / 'src'


def method(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}\n', start) + 3]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path)
    parser.add_argument('--qp-source', type=Path)
    args = parser.parse_args()
    source = (args.source or SRC / 'Mlx4Hw.cpp').read_text()
    code = '\n'.join(method(source, signature) for signature in [
        'bool Mlx4Hw::cmdBox(', 'bool Mlx4Hw::cmdBoxLocked(',
        'void Mlx4Hw::cmdPoison('])
    # Baseline has no observation argument: execute its unchanged methods and
    # test that it fails to produce the required output, rather than faking it.
    traced = 'Cx3CommandObservation *outObservation' in code
    integration = not args.source
    if integration:
        qp_source = (args.qp_source or SRC / 'Mlx4Hw.cpp').read_text()
        code += '\n' + '\n'.join(method(qp_source, signature) for signature in [
            'bool Mlx4Hw::qpModify(', 'bool Mlx4Hw::qpQuery(', 'UInt32 Mlx4Hw::qpState('])
    harness = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#define CX3_HOST_TEST
#include "cx3_abi.h"
#include "cx3_cmdgate.h"
#include "mlx4_regs.h"
#ifndef CX3_COMMAND_OBSERVATION_ABI
struct Cx3CommandObservation {
    uint32_t abiVersion,abiBytes,flags,fwStatus;
    uint64_t startNs,postedNs,endNs;
};
#endif
static UInt64 nowNs=1000000;
static UInt64 cx3NowNs() { nowNs+=1000000; return nowNs; }
static void IOSleep(UInt32 n) { nowNs+=UInt64(n)*1000000; }
static UInt32 OSSwapHostToBigInt32(UInt32 n) { return __builtin_bswap32(n); }
static UInt32 OSSwapBigToHostInt32(UInt32 n) { return __builtin_bswap32(n); }
static UInt32 mlx4GetBe32(const void *p) { UInt32 n;memcpy(&n,p,4);return __builtin_bswap32(n); }
static void mlx4PutBe32(void *p,UInt32 n) { n=__builtin_bswap32(n);memcpy(p,&n,4); }
static Cx3CommandObservation *currentObservation=nullptr;
static unsigned prematurePosted=0;
static void mlx4Wmb() {
    // The Posted fact may only be published AFTER the GO store's barrier.
    if (currentObservation && (currentObservation->flags & 2u)) ++prematurePosted;
    __sync_synchronize();
}
static void logSink(const char*,...) {}
#define LOG(...) logSink(__VA_ARGS__)
struct IOLock {};
static IOLock lock;
static unsigned locks=0,unlocks=0;
static IOLock* IOLockAlloc() { return &lock; }
static void IOLockLock(IOLock*) { ++locks; }
static void IOLockUnlock(IOLock*) { ++unlocks; }
class Mlx4Hw {
public:
    UInt32 words[7]{}; UInt32 *fHcr=words; IOLock *fCmdLock=&lock;
    UInt8 mailbox[4096]{};void *fMboxVirt=mailbox;UInt64 fMboxPhys=0x1234000;
    bool mboxAlloc() { return true; }
    bool qpModify(UInt16,UInt32,const UInt8*,UInt32*,UInt8*,UInt32,Cx3QpCaptureObservation*);
    bool qpQuery(UInt32,UInt8*,UInt32,UInt32*,UInt8*,UInt32,Cx3QpCaptureObservation*);
    UInt32 qpState(const UInt8*);
    bool fCmdWedged=false,fCmdWedgedPosted=false,fCmdSyntheticPoison=false;
    UInt16 fCmdWedgedOp=0; UInt64 fCmdWedgedAtNs=0;
    UInt32 fCmdAbandons=0,fCmdRefused=0,fToggle=1;
    bool preBusy=false,postBusy=false,seenPost=false; unsigned reads=0;
    UInt32 replyStatus=0;
    bool cmdPending() {
        ++reads;
        if (words[6] & OSSwapHostToBigInt32(1u<<HCR_GO_BIT)) seenPost=true;
        if (!seenPost) return preBusy;
        if (postBusy) return true;
        words[6]=OSSwapHostToBigInt32(replyStatus<<24);
        return false;
    }
    UInt32 readHcrStatus() { ++reads; return seenPost && !postBusy ? replyStatus<<24 : 0; }
    UInt32 readRegWarm(UInt32) { ++reads; return 0; }
    void cmdPoison(UInt16,const char*,UInt32,UInt64,bool);
    bool cmdBox(UInt64,UInt64,UInt32,UInt8,UInt16,UInt32,UInt32*,bool OBS_DECL);
    bool cmdBoxLocked(UInt64,UInt64,UInt32,UInt8,UInt16,UInt32,UInt32*,bool,
                      UInt64*,UInt16,bool OBS_DECL);
};
CODE
static unsigned cases=0,checks=0,failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL case %u line %u: %s\n",cases,__LINE__,#x); } } while(0)
int main() {
    for (unsigned mode=0;mode<7;++mode) {
        ++cases; Mlx4Hw h; UInt32 status=0xdeadbeef;
        Cx3CommandObservation obs; memset(&obs,0xcc,sizeof(obs));
        currentObservation=&obs;
        if (mode==1) h.fHcr=nullptr;
        if (mode==2) h.fCmdWedged=true;
        if (mode==3) h.preBusy=true;
        if (mode==4) h.postBusy=true;
        if (mode==5) h.replyStatus=3;
        bool ok=h.cmdBox(0x1122334455667788ULL,0x1234000,65,0,
                         MLX4_CMD_QUERY_QP,2,mode==6 ? nullptr : &status,false OBS_ARG);
        bool posted=mode==0 || mode>=4;
        bool replied=mode==0 || mode>=5;
        CHECK(ok==(mode==0 || mode==6));
        CHECK(obs.abiVersion==1 && obs.abiBytes==sizeof(obs));
        CHECK(obs.flags==(1u | (posted ? 2u : 0u) | (replied ? 4u : 0u)));
        CHECK(obs.fwStatus==(mode==1 ? CX3_FWSTATUS_NO_DEVICE :
                            mode==2 || mode==3 ? CX3_FWSTATUS_WEDGED :
                            mode==4 ? CX3_FWSTATUS_TIMEOUT : mode==5 ? 3u : 0u));
        CHECK(obs.startNs>0 && obs.endNs>=obs.startNs);
        CHECK(posted ? (obs.postedNs>=obs.startNs && obs.postedNs<=obs.endNs) : obs.postedNs==0);
        if (posted) {
            CHECK(h.words[0]==0x44332211 && h.words[1]==0x88776655);
            CHECK(h.fToggle==0);
        } else {
            CHECK(h.words[0]==0 && h.words[1]==0 && h.words[6]==0);
            CHECK(h.fToggle==1);
        }
        if (mode==1 || mode==2) CHECK(h.reads==0);
        CHECK(locks==unlocks);
        CHECK(prematurePosted==0);
        // Reuse the exact output after an attempted command. Refusal must clear
        // its old Posted/Replied/timestamp facts even if firmware status is null.
        h.fCmdWedged=true;
        h.cmdBox(0,0,65,0,MLX4_CMD_QUERY_QP,2,nullptr,false OBS_ARG);
        CHECK(obs.flags==1 && obs.postedNs==0 &&
              obs.fwStatus==(mode==1 ? CX3_FWSTATUS_NO_DEVICE : CX3_FWSTATUS_WEDGED));
        CHECK(obs.abiVersion==1 && obs.endNs>=obs.startNs);
    }
INTEGRATION
    std::printf("command observation: %u cases, %u checks, %u failures (production HCR path / host registers)\n",cases,checks,failures);
    return failures ? 1 : 0;
}
'''
    declaration = ',Cx3CommandObservation* =nullptr' if traced else ''
    if integration:
        # Use the production declarations verbatim. A copied declaration once
        # omitted bypassBusy and hid a pointer-to-bool argument wiring defect.
        header = (SRC / 'Mlx4Hw.h').read_text()
        begin = header.index('    bool  cmdBox(')
        end = header.index(';', header.index('    bool  cmdBoxLocked(', begin)) + 1
        begin_fake = harness.index('    bool cmdBox(')
        end_fake = harness.index(';', harness.index('    bool cmdBoxLocked(', begin_fake)) + 1
        harness = harness[:begin_fake] + header[begin:end] + harness[end_fake:]
    harness = harness.replace('OBS_DECL', declaration).replace('OBS_ARG', ',&obs' if traced else '')
    harness = harness.replace('INTEGRATION', r'''
    // Real QP -> real cmdBox -> real cmdBoxLocked. In particular the bool
    // bypassBusy BEFORE the observation pointer must remain false.
    currentObservation=nullptr;
    for (bool query : {false,true}) for (unsigned mode=0;mode<5;++mode) {
        ++cases;Mlx4Hw h;UInt8 context[248]{},capture[4096]{};
        Cx3QpCaptureObservation o{};UInt32 status=0xdeadbeef;
        if(mode==1)h.preBusy=true;
        if(mode==2)h.postBusy=true;
        if(mode==3)h.replyStatus=3;
        if(mode==4)h.fHcr=nullptr;
        bool ok=query ? h.qpQuery(65,context,248,&status,capture,4096,&o) :
                        h.qpModify(MLX4_CMD_RST2INIT_QP,65,context,&status,capture,264,&o);
        CHECK(ok==(mode==0));
        CHECK(o.preparedBytes==(query ? 4096u : 264u));
        CHECK(o.capturedBytes==(query ? (ok ? 4096u : 0u) : 264u));
        CHECK(o.command.abiVersion==1 && o.command.abiBytes==40);
        CHECK(o.command.flags==(mode==1 || mode==4 ? 1u : mode==2 ? 3u : 7u));
        CHECK(o.command.fwStatus==status && o.fwStatus==status);
        CHECK(o.command.startNs>0 && o.command.endNs>=o.command.startNs);
        if(mode==1 || mode==4) {
            CHECK(!h.seenPost && h.words[0]==0 && h.words[6]==0 && h.fToggle==1);
            CHECK(o.command.postedNs==0);
        } else {
            CHECK(h.seenPost && h.fToggle==0);
            CHECK(o.command.postedNs>=o.command.startNs && o.command.postedNs<=o.command.endNs);
        }
        CHECK(status==(mode==0 ? 0u : mode==1 ? CX3_FWSTATUS_WEDGED :
                       mode==2 ? CX3_FWSTATUS_TIMEOUT : mode==3 ? 3u : CX3_FWSTATUS_NO_DEVICE));
    }
''' if integration else '')
    harness = harness.replace('CODE', code)
    with tempfile.TemporaryDirectory(prefix='cx3-command-observation-') as temp:
        binary = str(Path(temp) / 'test')
        if integration and not args.qp_source:
            # Recreate the exact historical argument omission. The public
            # declaration must reject it at compile time, not silently choose
            # the bool parameter. No production file is modified for this.
            faulty = harness.replace(
                'false, outObservation ? &outObservation->command : 0',
                'outObservation ? &outObservation->command : 0')
            if faulty == harness:
                raise RuntimeError('negative wiring fixture did not mutate a call')
            variable = harness + r'''
void pointerVariableMustNotBypass(Mlx4Hw &h, Cx3CommandObservation *p) {
    h.cmdBox(0,0,0,0,0,0,nullptr,p);
}
'''
            for label, negative in [('historical QP calls', faulty), ('pointer variable', variable)]:
                rejected = subprocess.run(
                    ['/usr/bin/clang++', '-std=c++11', '-fsyntax-only',
                     '-I', str(SRC), '-x', 'c++', '-'],
                    input=negative, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                if rejected.returncode == 0 or 'deleted' not in rejected.stderr:
                    print('FAIL:', label, 'not rejected by type contract')
                    print(rejected.stderr)
                    return 1
            print('type contract: 2 pointer-as-bool fixtures rejected (deleted overload, no Werror)')
        subprocess.run(['/usr/bin/clang++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-Wno-pointer-bool-conversion',
                        '-I', str(SRC), '-x', 'c++', '-', '-o', binary],
                       input=harness, text=True, check=True)
        return subprocess.run([binary], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
