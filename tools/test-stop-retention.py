#!/usr/bin/env python3
"""Run production stop through its incomplete/success boundary with host IOKit.

Breaks caught: incomplete stop loses self/provider ownership or detaches an
interface while a selector is live; repeated stop retries cleanup or pins again.
Only the complete-stop tail (after the failure return) is replaced by a marker.
This proves local reference/branch behavior, not IOKit termination semantics.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path)
    args = parser.parse_args()
    src = Path(__file__).resolve().parents[1] / 'src'
    text = (args.source or src / 'Cx3Mlx4.cpp').read_text()
    start = text.index('void Cx3Mlx4::stop(IOService *provider)')
    tail = text.index('    /*\n     * Move both an ordinary attached interface and any late orphan', start)
    production = text[start:tail] + '    ++tailReached;\n}\n'
    worker = text.index('void Cx3Mlx4::netAttachThread(')
    completion = text.index('    IOLockLock(self->fNetAttachLock);', worker)
    completion = text[completion:text.index('\n}\n', completion)]
    harness = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#define CX3_HOST_TEST
#include "mlx4_pure.h"
using UInt32=uint32_t;
struct IOService {
    unsigned refs=1,stopCalls=0;
    void retain() { ++refs; }
    void release() { --refs; }
    void stop(IOService*) { ++stopCalls; }
};
struct IONetworkInterface : IOService {};
struct IOLock {};
static void IOLockLock(IOLock*) {}
static void IOLockUnlock(IOLock*);
static void IOLockWakeup(IOLock*,bool*,bool) {}
static bool workerQuiet=true;
static bool freeAllowed=true;
static unsigned cancelCalls=0,freeCalls=0;
static bool thread_call_cancel(void*) { ++cancelCalls; return workerQuiet; }
static bool thread_call_free(void*) { ++freeCalls; return freeAllowed; }
bool thread_call_isactive(void*) { return !workerQuiet; }
static void logSink(const char*,...) {}
#define LOG(...) logSink(__VA_ARGS__)
struct TestHw {
    bool retained=false;
    bool hasAbRetainedRings() const { return retained; }
};
class Cx3Mlx4 : public IOService {
public:
    using super=IOService;
    enum { kCx3AttachWorkerPoisoned=1,kCx3AttachWorkerIdle=0,
           kCx3AttachWorkerLateReturn=2,kCx3AttachWorkerReturned=3,kCx3AttachWorkerFailed=4 };
    bool drained=true,fStopRetained=false;
    bool fNetEnabled=false;
    Mlx4TxAdmissionGate fNetTxAdmission;
    TestHw fHw;
    IOService *fStopProvider=nullptr,*fPci=nullptr;
    IOLock lock; IOLock *fNetAttachLock=&lock;
    void *fNetAttachCall=reinterpret_cast<void*>(1);
    UInt32 fNetAttachRetainCount=1;
    bool fNetAttachInFlight=false,fNetAttachPoisoned=false,fNetAttachSuccess=false,fNetAttachDone=false;
    IONetworkInterface *fNetAttachIface=nullptr;
    unsigned tailReached=0,detaches=0,completionMarks=0;
    Cx3Mlx4() { fNetTxAdmission.init(); }
    bool beginStopAndDrain(UInt32) { return drained; }
    // Prefix-only fixture has no ledger/mailbox owner; full composition tests
    // exercise the real retention predicate and those additional stop states.
    bool hasStopRetentionOwners() const { return false; }
    bool stopNativePorts() { return true; } // native lifecycle has its own real-method smoke
    void netAttachWorkerMark(UInt32 phase) { if (phase>=2) ++completionMarks; }
    void detachInterface(IONetworkInterface*,bool) { ++detaches; }
    void stop(IOService*);
    static void completeWorker(Cx3Mlx4*);
};
static Cx3Mlx4 *completionTarget=nullptr;
static bool markedBeforeFence=false;
static void IOLockUnlock(IOLock*) {
    if (completionTarget && !completionTarget->fNetAttachInFlight)
        markedBeforeFence=completionTarget->completionMarks!=0;
}
PRODUCTION
void Cx3Mlx4::completeWorker(Cx3Mlx4 *self) {
    bool late,attached=true; IONetworkInterface* netif=nullptr;
COMPLETION
}
static unsigned checks=0,failures=0,cases=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL case %u line %u: %s\n",cases,__LINE__,#x); } } while(0)
int main() {
    for (unsigned mode=0;mode<5;++mode) {
        ++cases;
        Cx3Mlx4 d; IOService provider; IONetworkInterface iface;
        d.fPci=&provider; d.fNetAttachIface=&iface;
        d.drained=(mode&1)!=0 || mode==4; workerQuiet=(mode&2)!=0 || mode==4;
        d.fNetAttachInFlight=!workerQuiet;
        freeAllowed=mode!=4;
        cancelCalls=freeCalls=0;
        // One queued worker reference in addition to the framework's reference.
        d.retain();
        const bool complete=mode==3;
        d.stop(&provider);
        CHECK(cancelCalls==unsigned(d.drained));
        CHECK(freeCalls==unsigned(d.drained && workerQuiet));
        CHECK(d.fNetAttachCall==((d.drained && workerQuiet && freeAllowed) ? nullptr : reinterpret_cast<void*>(1)));
        CHECK(d.stopCalls==unsigned(!complete)); // complete tail is intentionally not executed
        CHECK(d.tailReached==unsigned(complete));
        // Interface termination moved to the bounded synchronous worker after this prefix.
        CHECK(d.detaches==0);
        CHECK(d.fNetAttachIface==&iface);
        CHECK(iface.refs==1u);
        CHECK(provider.refs==(complete ? 1u : 2u));
        CHECK(d.refs==(complete ? 1u : 3u));
        CHECK(d.fNetAttachRetainCount==(complete ? 0u : 1u));
        CHECK(d.fStopRetained==!complete);
        CHECK(d.fStopProvider==(complete ? nullptr : &provider));
        if (!complete) {
            // A later worker exit cannot release the boot-long owner pin.
            d.release(); CHECK(d.refs==2);
            unsigned refs=d.refs,providerRefs=provider.refs;
            d.drained=true; workerQuiet=true;
            d.stop(&provider);
            CHECK(d.refs==refs && provider.refs==providerRefs);
            CHECK(d.tailReached==0 && d.detaches==0);
            CHECK(d.stopCalls==1);
        }
    }
    {
        ++cases;
        Cx3Mlx4 d; IOService provider;
        d.fPci=&provider;
        d.fHw.retained=true;
        d.fNetAttachRetainCount=0;
        d.stop(&provider);
        CHECK(d.fStopRetained && d.fStopProvider==&provider);
        CHECK(d.refs==2 && provider.refs==2);
        CHECK(d.stopCalls==1 && d.tailReached==0);
        CHECK(d.fHw.hasAbRetainedRings());
        CHECK(d.fNetAttachRetainCount==0);
    }
    {
        ++cases;
        Cx3Mlx4 d; d.retain(); d.fNetAttachInFlight=true;
        completionTarget=&d; Cx3Mlx4::completeWorker(&d); completionTarget=nullptr;
        CHECK(markedBeforeFence); // no final worker property access after publication
        CHECK(!d.fNetAttachInFlight && d.fNetAttachDone);
        CHECK(d.refs==1 && d.fNetAttachRetainCount==0);
    }
    {
        ++cases;
        Cx3Mlx4 d; IOService fallback;
        d.drained=false; d.fNetAttachCall=nullptr; d.fNetAttachRetainCount=0;
        d.stop(&fallback);
        CHECK(d.fStopProvider==&fallback && fallback.refs==2);
        CHECK(d.refs==2 && d.fStopRetained);
    }
    std::printf("stop retention: %u cases, %u checks, %u failures (production failure prefix / host IOKit)\n",cases,checks,failures);
    return failures ? 1 : 0;
}
'''.replace('PRODUCTION', production).replace('COMPLETION', completion)
    with tempfile.TemporaryDirectory(prefix='cx3-stop-retention-') as temp:
        binary = str(Path(temp) / 'test')
        subprocess.run(['/usr/bin/clang++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(src), '-x', 'c++', '-', '-o', binary],
                       input=harness, text=True, check=True)
        return subprocess.run([binary], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
