#!/usr/bin/env python3
"""Run the production externalMethod and admission methods at capacity boundaries.

Break caught: an exhausted ledger still admits/dispatches a selector, including
one admitted before exhaustion but only acquiring the transaction lock later.
The superclass/locks are recording boundaries, not a real IOKit concurrency test.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile

SRC = Path(__file__).resolve().parents[1] / 'src'


def method(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}\n', start) + 3]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--client-source', type=Path)
    args = parser.parse_args()
    source = (SRC / 'Cx3Mlx4.cpp').read_text()
    client = (args.client_source or SRC / 'Cx3Mlx4UserClient.cpp').read_text()
    methods = '\n'.join(method(source, sig) for sig in [
        'bool Cx3Mlx4::selectorEnter(', 'void Cx3Mlx4::selectorExit(',
        'bool Cx3Mlx4::selectorStopping('])
    methods += method(client, 'IOReturn Cx3Mlx4UserClient::externalMethod(')
    if 'bool Cx3Mlx4::resetAdmissionAllows(' in source:
        methods += method(source, 'bool Cx3Mlx4::resetAdmissionAllows(')
    methods += method(client, 'void Cx3Mlx4UserClient::stop(')
    if 'bool Cx3Mlx4UserClient::clientReady(' in client:
        methods += method(client, 'bool Cx3Mlx4UserClient::clientReady(')
    guard_start = client.index('class Cx3SelectorGuard {')
    guard = client[guard_start:client.index('\n};', guard_start) + 3]
    policy_start = client.index('static const UInt8 kXactClass[]')
    policy = client[policy_start:client.index('\n};', policy_start) + 3]
    harness = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#define CX3_HOST_TEST
#include "mlx4_pure.h"
#include "cx3_abi.h"
using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnNotPermitted=-1,kIOReturnBusy=-2,kIOReturnNotAttached=-3;
static void logSink(const char*,...) {}
#define LOG(...) logSink(__VA_ARGS__)
struct OSObject {};
using IOService=OSObject;
struct IOLock {};
static void IOLockLock(IOLock*) {}
static void IOLockUnlock(IOLock*) {}
struct IOExternalMethodArguments { unsigned scalarInputCount=0,scalarOutputCount=0; };
struct IOExternalMethodDispatch {};
struct Base : OSObject {
    unsigned dispatches=0;
    void stop(IOService*) {}
    IOReturn externalMethod(uint32_t,IOExternalMethodArguments*,IOExternalMethodDispatch*,OSObject*,void*) {
        ++dispatches; return kIOReturnSuccess;
    }
};
class Cx3Mlx4 {
public:
 bool nativeActive=false;bool hasNativePorts()const{return nativeActive;}
    enum { kCx3ClientPhaseEntered,kCx3ClientPhaseBusy,kCx3ClientPhaseSweepIn,
        kCx3ClientPhaseSweepOut,kCx3ClientPhaseDispatchIn,kCx3ClientPhaseDispatchOut,
        kCx3ClientPhaseReleased,kCx3ClientPhaseWaitLock,kCx3ClientPhaseLocked };
    Mlx4LifecycleGate fGate{};
    IOLock admission,transaction;
    IOLock *fAdmissionLock=&admission,*fXactLock=&transaction;
    bool fQuarExhausted=false,exhaustAtLock=false,denyTry=false,stopAtLock=false,failLock=false;
    bool fResetCommandsBlocked=false,resetAtLock=false;
    int lockDepth=0; unsigned lockCalls=0,sweeps=0;
    bool retentionExhausted() const { return fQuarExhausted; }
    bool resetAdmissionAllows(UInt32) const;
    bool netAttachPoisoned() { return false; }
    void userClientMark(uint32_t,unsigned) {}
    bool transactionEnter() {
        if (failLock) return false;
        ++lockDepth; ++lockCalls;
        if (exhaustAtLock && fGate.active) fQuarExhausted=true;
        if (stopAtLock && fGate.active) fGate.beginStop();
        if (resetAtLock && fGate.active) fResetCommandsBlocked=true;
        return true;
    }
    bool transactionTryEnter() { if (denyTry) return false; return transactionEnter(); }
    void transactionExit() { --lockDepth; }
    void freezeInstrumentsSweep(const char*) { ++sweeps; }
    bool selectorEnter(); void selectorExit(); bool selectorStopping();
};
GUARD
enum { kXactLocked=0,kXactTryLock=1,kXactNoLock=2 };
POLICY
class Cx3Mlx4UserClient : public Base {
public:
    using super=Base;
    Cx3Mlx4 *fProvider=nullptr;
    IOLock clientLock; IOLock *fClientLock=&clientLock; bool fClientStopping=false;
    void stop(IOService*); bool clientReady();
    static const IOExternalMethodDispatch sMethods[kCx3SelCount];
    IOReturn externalMethod(uint32_t,IOExternalMethodArguments*,IOExternalMethodDispatch*,OSObject*,void*);
};
const IOExternalMethodDispatch Cx3Mlx4UserClient::sMethods[kCx3SelCount]{};
METHODS
static unsigned cases=0,checks=0,failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %s line %u: %s\n",name,__LINE__,#x); } } while(0)
int main() {
    for (uint32_t selector : {0u,13u,(uint32_t)kCx3SelRuntimeIdentity,(uint32_t)kCx3SelNativePort}) for (unsigned mode=0;mode<8;++mode) {
        const char *name="selector retention boundary"; ++cases;
        Cx3Mlx4 d; d.fGate.init(); Cx3Mlx4UserClient c; c.fProvider=&d;
        d.fQuarExhausted=mode==1; d.exhaustAtLock=mode==2; d.denyTry=mode==3;
        d.stopAtLock=mode==4; d.failLock=mode==5;
        if (mode==6) d.fGate.beginStop();
        if (mode==7) c.stop(nullptr);
        IOExternalMethodArguments args;
        int rc=c.externalMethod(selector,&args,nullptr,nullptr,nullptr);
        const bool busy=(mode==3 || mode==5) && kXactClass[selector]==kXactTryLock;
        const bool hostIdentity=selector==kCx3SelRuntimeIdentity || selector==kCx3SelNativePort;
        const bool refused=(mode==1 || mode==2) && !hostIdentity;
        const bool mustRefuse=refused || mode==4 || mode==6 || (mode==5 && !busy);
        CHECK(rc==(mode==7 ? kIOReturnNotAttached : mustRefuse ? kIOReturnNotPermitted : busy ? kIOReturnBusy : kIOReturnSuccess));
        CHECK(c.dispatches==unsigned(mode!=7 && !mustRefuse && !busy));
        CHECK(d.sweeps==unsigned(mode!=7 && mode!=6 && mode!=3 && mode!=5 &&
                                 selector!=kCx3SelRuntimeIdentity));
        CHECK(d.fGate.active==0 && d.lockDepth==0);
    }
    // Every current selector, including a waiter admitted before reset.
    for(uint32_t selector=0;selector<kCx3SelCount;++selector) for(unsigned mode=0;mode<3;++mode) {
        const char *name="reset selector admission";++cases;
        Cx3Mlx4 d;d.fGate.init();Cx3Mlx4UserClient c;c.fProvider=&d;
        d.fResetCommandsBlocked=mode==1;d.resetAtLock=mode==2;
        IOExternalMethodArguments args;
        int rc=c.externalMethod(selector,&args,nullptr,nullptr,nullptr);
        bool allowed=mode==0 || selector==kCx3SelNetStat || selector==kCx3SelRuntimeIdentity || selector==kCx3SelAutoNetwork || selector==kCx3SelNativePort;
        CHECK(rc==(allowed ? kIOReturnSuccess : kIOReturnNotPermitted));
        CHECK(c.dispatches==unsigned(allowed));
        CHECK(d.fGate.active==0 && d.lockDepth==0);
    }
    std::printf("selector retention: %u cases, %u checks, %u failures (synthetic IOKit/locks)\n",cases,checks,failures);
    return failures ? 1 : 0;
}
'''.replace('GUARD', guard).replace('POLICY', policy).replace('METHODS', methods)
    with tempfile.TemporaryDirectory(prefix='cx3-selector-retention-') as tmp:
        binary = str(Path(tmp) / 'test')
        subprocess.run(['/usr/bin/clang++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(SRC), '-x', 'c++', '-', '-o', binary],
                       input=harness, text=True, check=True)
        return subprocess.run([binary]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
