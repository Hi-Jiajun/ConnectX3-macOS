#!/usr/bin/env python3
"""Execute real ICM release/retention methods with a fake firmware and allocator.

Catches the bug where a reset plus a quiet observation frees device-owned pages.
Firmware UNMAP responses and allocator side effects are the only substituted
boundaries. This is not DMA simulation or hardware acceptance.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

SRC = Path(__file__).resolve().parents[1] / 'src'


def method(source, name):
    prefix = 'IOReturn' if name == 'exportM2dProfile' else ('bool' if name == 'hasContextOwners' or
        ('bool Cx3Mlx4::' + name + '(') in source else 'void')
    start = source.index(prefix + ' Cx3Mlx4::' + name + '(')
    return source[start:source.index('\n}\n', start) + 3]


def struct(source, name):
    start = source.index('struct ' + name + ' {')
    return source[start:source.index('\n};', start) + 3]


HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
using UInt8=uint8_t; using UInt16=uint16_t;
using UInt32=uint32_t; using UInt64=uint64_t;
using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnError=-1,kIOReturnOffline=-2,
    kIOReturnBadArgument=-3,kIOReturnNoSpace=-4,kIOReturnNotReady=-5;
#include "cx3_abi.h"
#include "Cx3PortQueues.h"
#define CX3_HOST_TEST
#include "mlx4_pure.h"
class IOBufferMemoryDescriptor;
class IODMACommand;
ICM_STRUCTS
static void testLog(const char*, ...) {}
#define LOG(...) testLog(__VA_ARGS__)

struct Hw {
    bool tableOk=true, auxOk=true, mttOwners=false;
    UInt32 tableCalls=0, auxCalls=0;
    std::vector<Mlx4Icm*> freed;
    UInt32 capCalls=0,profileCalls=0;
    bool windowAlive() { return true; }
    bool hasMttAllocations() const { return mttOwners; }
    bool parseDevCap(Mlx4DevCap *cap) { ++capCalls; cap->uar_size=4096; return true; }
    UInt64 makeProfile(Mlx4Profile*,Mlx4DevCap*,Mlx4InitHcaParam*,Mlx4IcmTable *tables,
                       UInt32,UInt32 *count) {
        ++profileCalls; tables[0].icm[0]=nullptr; *count=1; return 4096;
    }
    bool unmapIcm(UInt32, UInt64, UInt32 *status) {
        ++tableCalls; *status=tableOk ? 0 : 1; return tableOk;
    }
    bool unmapIcmAux(UInt32 *status) {
        ++auxCalls; *status=auxOk ? 0 : 1; return auxOk;
    }
    void icmFree(Mlx4Icm *p) { freed.push_back(p); }
};
class Cx3Mlx4 {
public:
    Cx3PortQueues fPortQueues[2] = { []() { Cx3PortQueues q{}; q.init(1); q.qpUp = false; q.ingressUp = false; q.txQpUp = false; q.cqUp = false; q.rqUp = false; q.txCqUp = false; q.qpRec = nullptr; q.ingressRec = nullptr; q.txQpRec = nullptr; q.cqRing = nullptr; q.cqRec = nullptr; q.rqRing = nullptr; q.rqBufs = nullptr; q.txSq = nullptr; q.rawFrame = nullptr; q.txCqRing = nullptr; q.txCqRec = nullptr; return q; }(), []() { Cx3PortQueues q{}; q.init(2); return q; }() };

public:
    enum { kMaxIcmTables=TABLE_MAX, kIcmKeptMax=KEPT_MAX };
    struct IcmKeptBlock { Mlx4Icm *icm; UInt32 pages; const char *tag; Mlx4DmaGeneration generation; };
    Hw fHw;
    Mlx4IcmTable fTables[kMaxIcmTables]{};
    Mlx4Icm *fAuxIcm=nullptr;
    UInt32 fNumTables=0, fLostIcmPages=0;
    UInt32 fResetSilenceRuns=0, fResetSilenceChanges=0;
    bool fIcmMapped=false, fIcmHcaHolds=false;
    bool fHcaInited=false,fNetEnabled=false,fQuarExhausted=false;
    
    
    
    
    Mlx4Icm *fEqRing=nullptr;
    UInt32 fMrState=0,fEqNent=0,teardownCalls=0;
    Mlx4Profile fProfile{}; Mlx4DevCap fDevCap{}; Mlx4InitHcaParam fInitHca{};
    UInt64 fIcmSize=0;
    IcmKeptBlock fIcmKept[kIcmKeptMax]{};
    UInt32 fIcmKeptCount=0, fIcmKeptOverflow=0;
    KEEP_RETURN keepIcmForQuiesce(Mlx4Icm*, const char*);
    void freeIcmTables();
    bool hasContextOwners() const;
    IOReturn exportM2dProfile(const UInt64*,Cx3ProfileResult*,UInt64*);
    // These commands cannot run on the rejected profile path. Empty-state positive
    // control does not assert their semantics; actual QP/CQ/MR return is tested elsewhere.
    bool irqReleaseObjects(const char*) { ++teardownCalls; return true; }
    void teardownEq(const char*,bool) { ++teardownCalls; }
    void ingressTeardown(const char*,bool,UInt32 port=1) { if(port==1) ++teardownCalls; }
    void txTeardown(const char*,bool,UInt32 port=1) { if(port==1) ++teardownCalls; }
    void qpTeardown(const char*,bool,UInt32 port=1) { if(port==1) ++teardownCalls; }
    void mrTeardown(const char*,bool) { ++teardownCalls; }
    void cqTeardown(const char*,bool,UInt32 port=1) { if(port==1) ++teardownCalls; }
    void rqTeardown(const char*,bool,UInt32 port=1) { if(port==1) ++teardownCalls; }
};
METHODS
static unsigned checks=0, failures=0, cases=0;
#define CHECK(label, expression) do { ++checks; if (!(expression)) { \
    ++failures; std::printf("FAIL %s: %s\n", caseName, label); } } while(0)

static void reset_case(const char *caseName, UInt32 runs, UInt32 changes, int form) {
    ++cases;
    Cx3Mlx4 d;
    Mlx4Icm a{}, b{}, aux{};
    UInt32 words[3]={0x11223344,0x55667788,0xabcdef01};
    a.generation.init(5); a.generation.markHandoffAttempted(6);
    b.generation.init(5); b.generation.markHandoffAttempted(6);
    aux.generation.init(5); aux.generation.markHandoffAttempted(6);
    a.npages=2; a.virt=&words[0];
    b.npages=3; b.virt=&words[1];
    aux.npages=5; aux.virt=&words[2];
    d.fResetSilenceRuns=runs; d.fResetSilenceChanges=changes;
    d.fIcmMapped=true; d.fIcmHcaHolds=false;
    d.fNumTables=(form & 1) ? 1 : 0;
    if (form & 1) {
        d.fTables[0].name="QPC";
        d.fTables[0].icm[0]=&a;
        d.fTables[0].icm[7]=&b;
    }
    if (form & 2) d.fAuxIcm=&aux;
    d.freeIcmTables();
    const UInt32 wantCount=form==1 ? 2 : (form==2 ? 1 : 3);
    const UInt32 wantPages=form==3 ? 10 : 5;
    CHECK("reset without proof must not free", d.fHw.freed.empty());
    CHECK("old instance must not receive unmap", d.fHw.tableCalls==0 && d.fHw.auxCalls==0);
    CHECK("every block retained in ledger", d.fIcmKeptCount==wantCount);
    CHECK("retained pages accounted", d.fLostIcmPages==wantPages);
    if (form & 1) {
        CHECK("table slots relinquished", d.fTables[0].icm[0]==nullptr && d.fTables[0].icm[7]==nullptr);
        CHECK("first block address retained", d.fIcmKept[0].icm==&a);
        CHECK("last table block address retained", d.fIcmKept[1].icm==&b);
        CHECK("table owner generation retained", d.fIcmKept[0].generation.matchesOwnerGeneration(6) &&
              d.fIcmKept[1].generation.matchesOwnerGeneration(6));
    }
    if (form & 2) {
        CHECK("aux address retained", d.fIcmKept[wantCount-1].icm==&aux);
        CHECK("aux owner generation retained", d.fIcmKept[wantCount-1].generation.matchesOwnerGeneration(6));
    }
    CHECK("aux slot cleared", d.fAuxIcm==nullptr);
    CHECK("mapping flags cleared", !d.fIcmMapped && !d.fIcmHcaHolds);
    CHECK("readout unchanged", d.fResetSilenceRuns==runs && d.fResetSilenceChanges==changes);
    CHECK("payload read only", words[0]==0x11223344 && words[1]==0x55667788 && words[2]==0xabcdef01);
    d.freeIcmTables();
    CHECK("repeat cleanup cannot free retained pages", d.fHw.freed.empty());
    CHECK("repeat cleanup cannot duplicate retention", d.fIcmKeptCount==wantCount && d.fLostIcmPages==wantPages);
}

static void unmap_case(const char *caseName, bool tableOk, bool auxOk) {
    ++cases;
    Cx3Mlx4 d;
    Mlx4Icm table{}, aux{};
    table.npages=2; aux.npages=5;
    d.fNumTables=1; d.fTables[0].name="QPC";
    d.fTables[0].virt=0x80000; d.fTables[0].icm[0]=&table;
    d.fAuxIcm=&aux; d.fIcmHcaHolds=true; d.fIcmMapped=true;
    d.fHw.tableOk=tableOk; d.fHw.auxOk=auxOk;
    d.freeIcmTables();
    CHECK("one unmap per object", d.fHw.tableCalls==1 && d.fHw.auxCalls==1);
    CHECK("free only after confirmed unmap", d.fHw.freed.size()==unsigned(tableOk)+unsigned(auxOk));
    CHECK("table free outcome", (std::find(d.fHw.freed.begin(),d.fHw.freed.end(),&table)!=d.fHw.freed.end())==tableOk);
    CHECK("aux free outcome", (std::find(d.fHw.freed.begin(),d.fHw.freed.end(),&aux)!=d.fHw.freed.end())==auxOk);
    CHECK("failed unmaps retained", d.fIcmKeptCount==unsigned(!tableOk)+unsigned(!auxOk));
    CHECK("failed unmap pages counted", d.fLostIcmPages==unsigned(tableOk ? 0 : 2)+unsigned(auxOk ? 0 : 5));
    CHECK("old slots cleared", d.fTables[0].icm[0]==nullptr && d.fAuxIcm==nullptr);
    d.freeIcmTables();
    CHECK("no double unmap", d.fHw.tableCalls==1 && d.fHw.auxCalls==1);
    CHECK("no double free", d.fHw.freed.size()==unsigned(tableOk)+unsigned(auxOk));
}

int main() {
    {
        const char *caseName="initialized HCA requires normal close"; ++cases;
        Cx3Mlx4 d; Mlx4Icm table{}, aux{};table.npages=1;aux.npages=1;
        d.fHcaInited=true;d.fIcmMapped=true;d.fIcmHcaHolds=true;
        d.fNumTables=1;d.fTables[0].icm[0]=&table;d.fAuxIcm=&aux;
        d.freeIcmTables();
        CHECK("initialized HCA prevents table/AUX unmap",d.fHw.tableCalls==0 && d.fHw.auxCalls==0);
        CHECK("initialized HCA prevents free and retention transfer",d.fHw.freed.empty() && d.fIcmKeptCount==0);
        CHECK("initialized HCA preserves exact owners and mapping flags",d.fTables[0].icm[0]==&table && d.fAuxIcm==&aux && d.fIcmMapped && d.fIcmHcaHolds);
    }
    {
        const char *caseName="secondary CQ keeps PF tables mapped"; ++cases;
        Cx3Mlx4 d; Mlx4Icm table{},child{}; d.fNumTables=1; d.fTables[0].icm[0]=&table;
        d.fIcmMapped=d.fIcmHcaHolds=true; d.fPortQueues[1].cqRing=&child;
        d.freeIcmTables();
        CHECK("secondary owner blocks shared table release",d.fHw.tableCalls==0 &&
              d.fHw.freed.empty() && d.fTables[0].icm[0]==&table && d.fPortQueues[1].cqRing==&child);
    }
    reset_case("quiet tables", 1,0,1);
    reset_case("quiet aux", 1,0,2);
    reset_case("quiet tables and aux", 1,0,3);
    reset_case("long quiet tables and aux", 0xffffffffu,0,3);
    reset_case("changed tables", 1,1,1);
    reset_case("changed aux", 1,1,2);
    reset_case("changed tables and aux", 1,1,3);
    reset_case("unsampled tables", 0,0,1);
    reset_case("unsampled aux", 0,0,2);
    reset_case("unsampled tables and aux", 0,0,3);
    unmap_case("both returned",true,true);
    unmap_case("table refusal",false,true);
    unmap_case("aux refusal",true,false);
    unmap_case("both refused",false,false);
    for (unsigned owner=0;owner<22;++owner) {
        const char *caseName="context dependency retained in place"; ++cases;
        Cx3Mlx4 d; Mlx4Icm table{},aux{},child{}; table.npages=2; aux.npages=5;
        d.fNumTables=1; d.fTables[0].name="QPC"; d.fTables[0].icm[0]=&table;
        d.fAuxIcm=&aux; d.fIcmMapped=d.fIcmHcaHolds=true;
        switch (owner) {
            case 0: d.fPortQueues[0].qpUp=true; break; case 1: d.fPortQueues[0].qpRec=&child; break;
            case 2: d.fPortQueues[0].ingressUp=true; break; case 3: d.fPortQueues[0].ingressRec=&child; break;
            case 4: d.fPortQueues[0].txQpUp=true; break; case 5: d.fPortQueues[0].txQpRec=&child; break;
            case 6: d.fPortQueues[0].cqUp=true; break; case 7: d.fPortQueues[0].cqRing=&child; break;
            case 8: d.fPortQueues[0].cqRec=&child; break; case 9: d.fPortQueues[0].rqUp=true; break;
            case 10: d.fPortQueues[0].rqRing=&child; break; case 11: d.fPortQueues[0].rqBufs=&child; break;
            case 12: d.fPortQueues[0].txCqUp=true; break; case 13: d.fPortQueues[0].txCqRing=&child; break;
            case 14: d.fPortQueues[0].txCqRec=&child; break; case 15: d.fPortQueues[0].txSq=&child; break;
            case 16: d.fEqRing=&child; break; case 17: d.fEqNent=128; break;
            case 18: d.fMrState=1; break; case 19: d.fMrState=2; break;
            case 20: d.fHw.mttOwners=true; break;
            case 21: d.fPortQueues[0].rawFrame=&child; break;
        }
        d.freeIcmTables();
        CHECK("no UNMAP under child owner", d.fHw.tableCalls==0 && d.fHw.auxCalls==0);
        CHECK("no free or owner transfer", d.fHw.freed.empty() && d.fIcmKeptCount==0);
        CHECK("table and aux remain addressable", d.fTables[0].icm[0]==&table && d.fAuxIcm==&aux);
        CHECK("mapping metadata preserved", d.fIcmMapped && d.fIcmHcaHolds && d.fLostIcmPages==0);
        UInt64 in[kM2dInCount]{}; in[kM2dInNumQp]=128; in[kM2dInNumMtt]=128;
        UInt64 out[kProfOutCount]{}; Cx3ProfileResult result{}; d.fProfile.num_qp=77;
        d.exportM2dProfile(in,&result,out);
        CHECK("profile reports refusal", result.status==UInt64(kIOReturnNotReady) && out[kProfOutStatus]==result.status);
        CHECK("refused profile has no command/teardown/layout effects",
              d.fHw.capCalls==0 && d.fHw.profileCalls==0 && d.teardownCalls==0);
        CHECK("old profile and pointers preserved", d.fProfile.num_qp==77 &&
              d.fTables[0].icm[0]==&table && d.fAuxIcm==&aux);
    }
    for (unsigned mode : {0u,1u,2u}) {
        const char *caseName="profile admission control"; ++cases; Cx3Mlx4 d;
        d.fHcaInited=mode==1; d.fNetEnabled=mode==2; UInt64 in[kM2dInCount]{};
        in[kM2dInNumQp]=128; in[kM2dInNumMtt]=128;
        UInt64 out[kProfOutCount]{}; Cx3ProfileResult result{};
        d.exportM2dProfile(in,&result,out);
        CHECK("initialized HCA/enabled network cannot repack live tables", result.status==UInt64(mode ? kIOReturnNotReady : 0));
        CHECK("layout only on empty uninitialized state", d.fHw.profileCalls==(mode ? 0u : 1u));
    }
    for (bool oneSlot : {false,true}) {
        const char *caseName="full ICM retention ledger"; ++cases; Cx3Mlx4 d;
        Mlx4Icm held[Cx3Mlx4::kIcmKeptMax]{},table{},aux{};
        for (unsigned i=0;i<Cx3Mlx4::kIcmKeptMax-unsigned(oneSlot);++i)
            d.keepIcmForQuiesce(&held[i],"existing");
        table.npages=2; aux.npages=5;
        d.fNumTables=1; d.fTables[0].name="QPC"; d.fTables[0].icm[0]=&table;
        d.fAuxIcm=&aux; d.fIcmMapped=d.fIcmHcaHolds=true;
        d.fHw.tableOk=d.fHw.auxOk=false; d.fNetEnabled=true;
        d.freeIcmTables();
        CHECK("table pointer moved only if ledger owns it", d.fTables[0].icm[0]==(oneSlot ? nullptr : &table));
        CHECK("untracked aux stays addressable", d.fAuxIcm==&aux);
        CHECK("no counted but unaddressable allocation", d.fIcmKeptOverflow==0);
        CHECK("failed retention blocks work and preserves mapping", d.fQuarExhausted && !d.fNetEnabled && d.fIcmMapped && d.fIcmHcaHolds);
        CHECK("account only actual owner transfers", d.fLostIcmPages==(oneSlot ? 2u : 0u));
        CHECK("no free on failure", d.fHw.freed.empty());
        UInt64 in[kM2dInCount]{}; in[kM2dInNumQp]=128; in[kM2dInNumMtt]=128;
        UInt64 out[kProfOutCount]{}; Cx3ProfileResult result{};
        d.fNetEnabled=false; d.exportM2dProfile(in,&result,out);
        CHECK("exhausted retention cannot repack table slots", result.status==UInt64(kIOReturnNotReady) && d.fHw.profileCalls==0);
        CHECK("aux survives attempted profile", d.fAuxIcm==&aux);
    }
    {
        const char *caseName="retention exhausts inside profile teardown"; ++cases;
        Cx3Mlx4 d; Mlx4Icm held[Cx3Mlx4::kIcmKeptMax]{},table{},aux{};
        for (unsigned i=0;i<Cx3Mlx4::kIcmKeptMax-1;++i) d.keepIcmForQuiesce(&held[i],"existing");
        table.npages=2; aux.npages=5; d.fNumTables=1;
        d.fTables[0].name="QPC"; d.fTables[0].icm[0]=&table; d.fAuxIcm=&aux;
        d.fIcmMapped=d.fIcmHcaHolds=true; d.fHw.tableOk=d.fHw.auxOk=false;
        UInt64 in[kM2dInCount]{}; in[kM2dInNumQp]=128; in[kM2dInNumMtt]=128;
        UInt64 out[kProfOutCount]{}; Cx3ProfileResult result{};
        d.exportM2dProfile(in,&result,out);
        CHECK("new exhaustion aborts profile before layout", result.status==UInt64(kIOReturnNotReady) &&
              out[kProfOutStatus]==result.status && d.fHw.profileCalls==0);
        CHECK("failed AUX stays mapped and addressable", d.fAuxIcm==&aux && d.fIcmMapped && d.fIcmHcaHolds);
        CHECK("prior transfer remains in ledger", d.fIcmKept[d.kIcmKeptMax-1].icm==&table && d.fLostIcmPages==2);
    }
    std::printf("%u cases, %u checks, %u failures\n",cases,checks,failures);
    return failures ? 1 : 0;
}
'''


def run(folder):
    source = (SRC / 'Cx3Mlx4.cpp').read_text()
    header = (SRC / 'Cx3Mlx4.h').read_text()
    hw = (SRC / 'Mlx4Hw.h').read_text()
    harness = HARNESS.replace('ICM_STRUCTS', struct(hw, 'Mlx4IcmPage') + '\n' + struct(hw, 'Mlx4Icm'))
    harness = harness.replace('KEEP_RETURN', 'bool' if 'bool Cx3Mlx4::keepIcmForQuiesce(' in source else 'void')
    for placeholder, name in [('TABLE_MAX', 'kMaxIcmTables'), ('KEPT_MAX', 'kIcmKeptMax')]:
        harness = harness.replace(placeholder, re.search(r'\b' + name + r'\s*=\s*(\d+)', header).group(1))
    # Before the production guard is introduced, omit the not-yet-existent helper
    # so RED exercises the old release/profile behavior rather than a compile error.
    methods = ['keepIcmForQuiesce', 'freeIcmTables', 'exportM2dProfile']
    if 'bool Cx3Mlx4::hasContextOwners(' in source:
        methods.insert(0, 'hasContextOwners')
    harness = harness.replace('METHODS', '\n'.join(method(source, name) for name in methods))
    cpp, binary = folder / 'test.cpp', folder / 'test'
    cpp.write_text(harness)
    build = subprocess.run(['/usr/bin/clang++', '-std=c++11', '-Wall', '-Wextra',
                            '-Werror', '-Wno-unused-variable', '-I', str(SRC),
                            str(cpp), '-o', str(binary)],
                           capture_output=True, text=True, timeout=60)
    (folder / 'build.log').write_text(build.stdout + build.stderr)
    if build.returncode:
        print(build.stdout + build.stderr, end='')
        return build.returncode
    test = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    (folder / 'run.log').write_text(test.stdout + test.stderr)
    print(test.stdout + test.stderr, end='')
    return test.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifacts', type=Path, help='new directory for exact compiled harness and logs')
    args = parser.parse_args()
    if args.artifacts:
        args.artifacts.mkdir(parents=True, exist_ok=False)
        return run(args.artifacts)
    with tempfile.TemporaryDirectory(prefix='cx3-icm-release-') as temp:
        return run(Path(temp))


if __name__ == '__main__':
    raise SystemExit(main())
