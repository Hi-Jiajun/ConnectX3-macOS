#!/usr/bin/env python3
"""Execute production reset and preflight; no HCA command on insufficient budget.

Hardware/reset/teardown are substituted boundaries. The real entry and inventory
must refuse before them. This is not a global reservation or concurrency proof.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile


def method(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}\n', start) + 3]


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--source', type=Path)
    p.add_argument('--flr-source', type=Path)
    args = p.parse_args()
    src = Path(__file__).resolve().parents[1] / 'src'
    source = (args.source or src / 'Cx3Mlx4.cpp').read_text()
    code = method(source, 'IOReturn Cx3Mlx4::exportResetHca(')
    if 'bool Cx3Mlx4::resetRetentionPreflight(' in source:
        code += method(source, 'bool Cx3Mlx4::resetRetentionPreflight(')
    if 'bool Cx3Mlx4::resetFreshStartEligible(' in source:
        code += method(source, 'bool Cx3Mlx4::resetFreshStartEligible(')
    code += (args.flr_source or src / 'flr.inc').read_text()
    harness = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
using UInt8=uint8_t; using UInt32=uint32_t; using UInt64=uint64_t; using IOReturn=int;
#define CX3_HOST_TEST 1
#include "mlx4_pure.h"
constexpr int kIOReturnSuccess=0,kIOReturnError=-1,kIOReturnNotReady=-2,kIOReturnNoSpace=-3;
constexpr int kIOReturnUnsupported=-4;
constexpr int kIOReturnBadArgument=-7;
constexpr unsigned kIOPCIConfigBaseAddress0=0x10,kIOPCIConfigVendorID=0,kIOPCIConfigCommand=4;
static void IOSleep(unsigned) {}
#include "cx3_abi.h"
#include "Cx3PortQueues.h"
#include "Cx3PortState.h"
struct IOLock {};
static void **clearOnLock=nullptr;
static void IOLockLock(IOLock*) { if (clearOnLock) *clearOnLock=nullptr; }
static void IOLockUnlock(IOLock*) {}
static void logSink(const char*,...) {}
#define LOG(...) logSink(__VA_ARGS__)
struct Mlx4Icm { UInt32 data=0x12345678; void* virt=&data; UInt64 bytes=4096; };
struct Table { Mlx4Icm *icm[8]{}; };
struct Hw {
    unsigned resets=0,clear=0,ownerReads=0; bool resetOK=true,mailbox=false,wedged=false,mttOwners=false;
    bool abRetained=false;
    unsigned configWrites=0,platformCalls=0,poisonClears=0,postConfigReads=0; bool flrSupported=true,hasCap=true;
    UInt32 postId=0x100715b3,postBar=0x80000000,postBarHi=0,postCommand=2;
    UInt32 preId=0x100715b3,preBar=0x80000000,preBarHi=0,preCommand=2;
    IOReturn platformStatus=kIOReturnSuccess;
    UInt64 currentGeneration=1;
    void setDeviceGeneration(UInt64 generation) { currentGeneration=generation; }
    Mlx4PciIdentity preIdentity{0x15b3,0x1007,0x15b3,0x0015,4,3,1};
    Mlx4PciIdentity postIdentity{0x15b3,0x1007,0x15b3,0x0015,4,3,1};
    bool preIdentityReadable=true,postIdentityReadable=true;
    IOReturn platformFunctionReset() { ++platformCalls;return platformStatus; }
    IOReturn readPciIdentity(Mlx4PciIdentity *out) {
        bool after=platformCalls!=0;
        if (!out) return kIOReturnBadArgument;
        if (!(after ? postIdentityReadable : preIdentityReadable)) { out->clear(); return kIOReturnNotReady; }
        *out=after ? postIdentity : preIdentity;
        return out->valid() ? kIOReturnSuccess : kIOReturnNotReady;
    }
    UInt8 findPcieCapability() { return hasCap ? 0x40 : 0; }
    UInt32 configRead(unsigned reg) {
        if (platformCalls) {
            ++postConfigReads;
            if (reg==0) return postId;
            if (reg==0x10) return postBar;
            if (reg==0x14) return postBarHi;
        if (reg==4) return postCommand;
        }
        if (reg==0x44) return flrSupported ? (1u<<28) : 0;
        if (reg==0x10) return preBar;
        if (reg==0x14) return preBarHi;
        if (reg==0) return preId;
        if (reg==4) return preCommand;
        return 0;
    }
    void configWrite(unsigned,UInt32) { ++configWrites; }
    void cmdClearWedged(const char*) { ++poisonClears; }
    UInt32 readOwnerWord() { ++ownerReads;return 0x1234; }
    bool cmdWedged() const { return wedged; }
    bool hasMailboxAllocation() const { return mailbox; }
    bool hasMttAllocations() const { return mttOwners; }
    bool hasAbRetainedRings() const { return abRetained; }
    bool resetHca(bool) { ++resets; return resetOK; }
    void clrIntTearDown() { ++clear; }
};
class Cx3Mlx4 {
public:
    Cx3PortQueues fPortQueues[2] = { []() { Cx3PortQueues q{}; q.init(1); q.qpRec = nullptr; q.cqRing = nullptr; q.cqRec = nullptr; q.rqRing = nullptr; q.rqBufs = nullptr; q.ingressRec = nullptr; q.txQpRec = nullptr; q.txSq = nullptr; q.rawFrame = nullptr; q.txCqRing = nullptr; q.txCqRec = nullptr; return q; }(), []() { Cx3PortQueues q{}; q.init(2); return q; }() };

public:
    enum { kQuarantineMax=64,kQuarantineMaxBytes=64*1024*1024,kQuarantineOverflowMax=32,
           kIcmKeptMax=256,kMaxIcmTables=18,kMaxIcmChunksPerTable=8 };
    Hw fHw; Mlx4PciIdentity fStartPciIdentity{0x15b3,0x1007,0x15b3,0x0015,4,3,1};
    UInt64 fDeviceGeneration=1;
    Cx3PortState fPorts[2]{};
    Cx3Mlx4() { fPorts[0].init(1,1); fPorts[1].init(2,1); }
    bool fStartPciIdentityValid=true;
    bool fQuarExhausted=false,fNetEnabled=false,fResetUnproven=false,fResetCommandsBlocked=false;
    UInt32 fQuarCount=0,fQuarOverflowCount=0,fIcmKeptCount=0;
    UInt32 fQuarOverflowUntracked=0,fIcmKeptOverflow=0;
    UInt64 fQuarBytes=0;
    void *fIrqSource=nullptr,*fWorkLoop=nullptr,*fNetTimer=nullptr,*fNetIface=nullptr;
    void *fNetAttachIface=nullptr,*fNetAttachCall=nullptr;
    IOLock lock; IOLock *fNetAttachLock=&lock;
    bool fNetAttachInFlight=false,fNetAttachPoisoned=false;
    bool fIrqObjectsUp=false,fIrqEnabled=false;
    bool fHcaInited=true,fIcmMapped=true,fIcmHcaHolds=true,fClrIntReady=true,fClrIntLegacy=true;
    UInt32 fResetSilenceChanges=0,fQuarRefused=0;
    Mlx4Icm *fEqRing=nullptr;
    
    
    Mlx4Icm *fFwIcm=nullptr,*fAuxIcm=nullptr;
    Mlx4Icm *fNetTxRing=nullptr; Table fTables[kMaxIcmTables]{}; UInt32 fNumTables=0;
    unsigned teardown=0,silence=0;
    void resetSilenceCheck() { ++silence; }
    void teardownEq(const char*,bool) { ++teardown; }
    void ingressTeardown(const char*,bool,UInt32 port=1) { if(port==1) ++teardown; }
    void txTeardown(const char*,bool,UInt32 port=1) { if(port==1) ++teardown; }
    void qpTeardown(const char*,bool,UInt32 port=1) { if(port==1) ++teardown; }
    void cqTeardown(const char*,bool,UInt32 port=1) { if(port==1) ++teardown; }
    void rqTeardown(const char*,bool,UInt32 port=1) { if(port==1) ++teardown; }
    void mrTeardown(const char*,bool) { ++teardown; }
    bool resetRetentionPreflight(); IOReturn exportResetHca(bool,UInt64*);
    bool resetFreshStartEligible() const;
    IOReturn exportFlr(UInt64*);
};
CODE
static unsigned cases=0,checks=0,failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL case %u line %u: %s\n",cases,__LINE__,#x); } } while(0)
int main() {
    {
        ++cases; Mlx4DmaGeneration stamp; stamp.init(7);
        CHECK(stamp.validAllocation() && !stamp.mayBeDeviceOwned);
        CHECK(!stamp.markHandoffAttempted(0));
        CHECK(stamp.markHandoffAttempted(8));
        CHECK(stamp.matchesOwnerGeneration(8) && !stamp.matchesOwnerGeneration(7));
    }
    for (unsigned mode=0;mode<34;++mode) {
        ++cases; Cx3Mlx4 d; Mlx4Icm a,b; UInt64 out[kRstOutCount]{};
        d.fEqRing=&a;
        clearOnLock=nullptr;
        Mlx4Icm chunks[18*8+1];
        switch(mode) {
        case 1: d.fQuarCount=1; break;
        case 2: d.fQuarOverflowCount=1; break;
        case 3: d.fIcmKeptCount=1; break;
        case 4: d.fQuarExhausted=true; break;
        case 5: a.bytes=64ULL*1024*1024+1; break;
        case 6: a.bytes=UINT64_MAX; break;
        case 7: d.fPortQueues[0].qpRec=&a; break;
        case 8: d.fNumTables=1; d.fTables[0].icm[0]=&a; break;
        case 9: d.fNetTimer=reinterpret_cast<void*>(1); break;
        case 10: d.fNetEnabled=true; break;
        case 11: d.fNetAttachInFlight=true; break;
        case 12: d.fNetAttachPoisoned=true; break;
        case 13: d.fNumTables=19; break;
        case 14: a.virt=nullptr; break;
        case 15: d.fAuxIcm=&b; d.fNumTables=1; d.fTables[0].icm[0]=&b; break;
        case 16: a.bytes=64ULL*1024*1024; break;
        case 17: d.fPortQueues[0].qpRec=&b; a.bytes=64ULL*1024*1024-4096; break;
        case 18: d.fPortQueues[0].qpRec=&b; a.bytes=64ULL*1024*1024-4095; break;
        case 19: d.fResetUnproven=true; break;
        case 20: d.fHw.resetOK=false; break;
        case 21: case 22:
            d.fNumTables=18;
            for (unsigned i=0;i<18;++i) for (unsigned j=0;j<8;++j) d.fTables[i].icm[j]=&chunks[i*8+j];
            d.fAuxIcm=&chunks[144];
            if (mode==22) d.fTables[17].icm[7]=d.fTables[16].icm[7];
            break;
        case 23: d.fNetIface=reinterpret_cast<void*>(1); break;
        case 24: d.fNetAttachIface=reinterpret_cast<void*>(1); break;
        case 25: d.fNetTxRing=&b; break;
        case 26: d.fNetAttachLock=nullptr; d.fNetAttachCall=reinterpret_cast<void*>(1); break;
        case 27: d.fQuarBytes=4; break;
        case 28: d.fQuarOverflowUntracked=1; break;
        case 29: d.fIcmKeptOverflow=1; break;
        case 30: a.bytes=4; break;
        case 31: a.bytes=3; break;
        case 32: d.fNetAttachIface=reinterpret_cast<void*>(1); clearOnLock=&d.fNetAttachIface; break;
        case 33: d.fHw.abRetained=true; break;
        }
        const bool allowed=mode==0 || mode==16 || mode==17 || mode==20 || mode==21 || mode==30 || mode==32;
        const bool succeeded=allowed && mode!=20;
        CHECK(d.exportResetHca(false,out)==kIOReturnSuccess);
        CHECK(out[kRstOutStatus]==(allowed ? (succeeded ? 0 : (UInt64)kIOReturnError) : (UInt64)kIOReturnNoSpace));
        CHECK(d.fHw.resets==unsigned(allowed));
        CHECK(d.fDeviceGeneration==(allowed ? 2u : 1u));
        CHECK(d.fHw.currentGeneration==d.fDeviceGeneration);
        CHECK(d.teardown==(succeeded ? 7u : 0u));
        CHECK(d.silence==unsigned(succeeded));
        CHECK(d.fEqRing==&a && a.data==0x12345678);
        if (!succeeded) CHECK(d.fHcaInited && d.fIcmMapped && d.fIcmHcaHolds && d.fClrIntReady);
        if (allowed) {
            CHECK(d.fResetUnproven);
            CHECK(d.exportResetHca(false,out)==kIOReturnSuccess);
            CHECK(out[kRstOutStatus]==(UInt64)kIOReturnNoSpace);
            CHECK(d.fHw.resets==1);
        }
        clearOnLock=nullptr;
    }
    // Fresh initial reset may proceed to initialization. A returned reset bool
    // is never a credential for replacing a prior device-owned allocation.
    for (unsigned mode=0;mode<11;++mode) {
        ++cases;Cx3Mlx4 d;Mlx4Icm a;UInt64 out[kRstOutCount]{};
        d.fHcaInited=d.fIcmMapped=d.fIcmHcaHolds=d.fClrIntReady=d.fClrIntLegacy=false;
        if(mode==1)d.fHw.resetOK=false;
        if(mode==2)d.fFwIcm=&a;
        if(mode==3)d.fHcaInited=true;
        if(mode==4)d.fHw.mailbox=true;
        if(mode==5)d.fHw.wedged=true;
        if(mode==6)d.fAuxIcm=&a;
        if(mode==7){d.fNumTables=1;d.fTables[0].icm[0]=&a;}
        if(mode==8)d.fClrIntReady=true;
        if(mode==9)d.fTables[17].icm[7]=&a; // inconsistent inactive slot still is an owner
        if(mode==10)d.fHw.mttOwners=true;
        d.exportResetHca(false,out);
        CHECK(d.fHw.resets==1 && d.fResetUnproven);
        CHECK(d.fResetCommandsBlocked==(mode!=0));
        if(mode==1)CHECK(d.fHw.ownerReads==1); // no MMIO read after failed reset
        unsigned reads=d.fHw.ownerReads;
        d.exportResetHca(false,out);
        CHECK(out[kRstOutStatus]!=0 && d.fHw.resets==1);
        CHECK(d.fHw.ownerReads==reads); // repeated reset refuses before MMIO
    }
    Mlx4Icm*& (*owners[])(Cx3Mlx4&)={
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fEqRing;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fFwIcm;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fPortQueues[0].qpRec;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fPortQueues[0].cqRing;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fPortQueues[0].cqRec;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fPortQueues[0].rqRing;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fPortQueues[0].rqBufs;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fPortQueues[0].ingressRec;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fPortQueues[0].txQpRec;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fPortQueues[0].txSq;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fPortQueues[0].rawFrame;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fPortQueues[0].txCqRing;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fPortQueues[0].txCqRec;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fPortQueues[1].qpRec;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fPortQueues[1].cqRing;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fPortQueues[1].cqRec;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fPortQueues[1].rqRing;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fPortQueues[1].rqBufs;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fPortQueues[1].ingressRec;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fPortQueues[1].txQpRec;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fPortQueues[1].txSq;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fPortQueues[1].rawFrame;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fPortQueues[1].txCqRing;},
        [](Cx3Mlx4& d)->Mlx4Icm*& {return d.fPortQueues[1].txCqRec;}
    };
    for (auto slot : owners) {
        ++cases; Cx3Mlx4 d; Mlx4Icm a; a.bytes=64ULL*1024*1024+1; slot(d)=&a;
        UInt64 out[kRstOutCount]{}; d.exportResetHca(false,out);
        CHECK(out[kRstOutStatus]==(UInt64)kIOReturnNoSpace);
        CHECK(d.fHw.resets==0 && d.teardown==0);
        CHECK(slot(d)==&a && !d.fResetUnproven);
    }
    for (auto slot : owners) {
        ++cases;Cx3Mlx4 d;Mlx4Icm a;UInt64 out[kRstOutCount]{};
        d.fHcaInited=d.fIcmMapped=d.fIcmHcaHolds=d.fClrIntReady=d.fClrIntLegacy=false;
        slot(d)=&a;d.exportResetHca(false,out);
        CHECK(d.fHw.resets==1);
        CHECK(d.fResetCommandsBlocked); // every device-facing owner defeats fresh startup
    }
    for (unsigned mode=0; mode<12; ++mode) {
        ++cases; Cx3Mlx4 d; Mlx4Icm a; UInt64 out[kFlrOutCount]{};
        switch (mode) {
        case 1: d.fResetUnproven=true; break;
        case 2: d.fNetEnabled=true; break;
        case 3: d.fNetAttachInFlight=true; break;
        case 4: d.fQuarCount=1; break;
        case 5: d.fIcmKeptCount=1; break;
        case 6: d.fIrqObjectsUp=true; break;
        case 7: d.fIrqSource=reinterpret_cast<void*>(1); break;
        case 8: d.fEqRing=&a; a.bytes=UINT64_MAX; break;
        case 9: d.fHw.flrSupported=false; break;
        case 10: d.fHw.hasCap=false; break;
        case 11: d.fHw.resetOK=false; {
            UInt64 rst[kRstOutCount]{}; d.exportResetHca(false,rst);
            CHECK(d.fResetUnproven && d.fHw.resets==1);
        } break;
        }
        bool permitted=mode==0;
        bool priorReset=d.fResetUnproven;
        CHECK(d.exportFlr(out)==kIOReturnSuccess);
        CHECK((out[kFlrOutStatus]==0)==permitted);
        CHECK(d.fDeviceGeneration==((permitted || mode==11) ? 2u : 1u));
        CHECK(d.fHw.currentGeneration==d.fDeviceGeneration);
        CHECK(d.fHw.configWrites==0 && d.fHw.platformCalls==unsigned(permitted));
        CHECK(d.fHw.poisonClears==0);
        CHECK(d.fIcmHcaHolds); // platform return is not a release credential
        CHECK(d.fHw.ownerReads==unsigned(mode==11)); // never use the old BAR after FLR
        CHECK(d.fResetUnproven==(priorReset || permitted));
        CHECK(d.fResetCommandsBlocked==(permitted || mode==11));
        CHECK(d.teardown==0 && d.silence==0);
        if (permitted) {
            // FLR then FLR and FLR then reset both refuse without another write/reset.
            CHECK(d.exportFlr(out)==kIOReturnSuccess);
            CHECK(out[kFlrOutStatus]!=0 && d.fHw.configWrites==0 && d.fHw.platformCalls==1);
            UInt64 rst[kRstOutCount]{}; d.exportResetHca(false,rst);
            CHECK(rst[kRstOutStatus]!=0 && d.fHw.resets==0);
        }
    }
    for (unsigned mode=0;mode<3;++mode) {
        ++cases;Cx3Mlx4 d;UInt64 out[kFlrOutCount]{};
        d.fHw.platformStatus=mode==0 ? kIOReturnUnsupported : mode==1 ? kIOReturnError : kIOReturnNotReady;
        unsigned reads=d.fHw.ownerReads;
        d.exportFlr(out);
        CHECK(out[kFlrOutStatus]==(UInt64)d.fHw.platformStatus);
        CHECK(d.fHw.platformCalls==1 && d.fHw.configWrites==0);
        CHECK(d.fResetUnproven && d.fResetCommandsBlocked);
        CHECK(d.fHw.ownerReads==reads && d.fHw.poisonClears==0);
        CHECK(d.fHw.postConfigReads==0 && d.fIcmHcaHolds);
        CHECK(d.teardown==0 && d.silence==0);
        d.exportFlr(out);
        CHECK(out[kFlrOutStatus]!=0 && d.fHw.platformCalls==1);
    }
    // A successful platform return is not sufficient if configuration/identity
    // is unavailable or changed. No old MMIO access even on the good branch.
    for (unsigned mode=0;mode<11;++mode) {
        ++cases;Cx3Mlx4 d;UInt64 out[kFlrOutCount]{};
        if(mode==1)d.fHw.postId=0xffffffffu;
        if(mode==2)d.fHw.postId=0;
        if(mode==3)d.fHw.postId=0x100815b3;
        if(mode==4)d.fHw.postBar=0x90000000;
        if(mode==5)d.fHw.postBarHi=1;
        if(mode==6)d.fHw.postCommand=0xffff;
        if(mode==7)d.fHw.postCommand=0;
        if(mode==8)d.fHw.postIdentity.bus=5;
        if(mode==9)d.fHw.postIdentity.deviceNumber=4;
        if(mode==10)d.fHw.postIdentityReadable=false;
        d.exportFlr(out);
        CHECK((out[kFlrOutStatus]==0)==(mode==0));
        CHECK(d.fHw.platformCalls==1 && d.fHw.configWrites==0);
        CHECK(d.fHw.ownerReads==0 && out[kFlrOutOwner]==0xffffffffull);
        CHECK(d.fResetCommandsBlocked && d.fResetUnproven && d.fIcmHcaHolds);
        CHECK(d.teardown==0 && d.silence==0 && d.fHw.poisonClears==0);
    }
    // Refuse a stale/disconnected or malformed pre-reset config snapshot
    // before asking the platform to reset a device whose identity is unknown.
    for (unsigned mode=0;mode<7;++mode) {
        ++cases;Cx3Mlx4 d;UInt64 out[kFlrOutCount]{};
        if(mode==0)d.fHw.preId=0xffffffffu;
        if(mode==1)d.fHw.preId=0;
        if(mode==2)d.fHw.preBar=0xffffffffu;
        if(mode==3)d.fHw.preBar=0;
        if(mode==4)d.fHw.preIdentity.bus=5;
        if(mode==5)d.fHw.preIdentityReadable=false;
        if(mode==6)d.fStartPciIdentityValid=false;
        d.exportFlr(out);
        CHECK(out[kFlrOutStatus]==(UInt64)(mode==6 ? kIOReturnNoSpace : kIOReturnNotReady));
        CHECK(d.fHw.platformCalls==0 && d.fHw.configWrites==0);
        CHECK(!d.fResetUnproven && !d.fResetCommandsBlocked);
        CHECK(d.fHw.ownerReads==0 && d.fHw.postConfigReads==0);
        CHECK(d.fHw.poisonClears==0 && d.teardown==0 && d.silence==0);
    }
    {
        ++cases; Cx3Mlx4 d; UInt64 out[kFlrOutCount]{};
        d.fDeviceGeneration=~0ull;
        d.exportFlr(out);
        CHECK(out[kFlrOutStatus]==(UInt64)kIOReturnNoSpace);
        CHECK(d.fHw.platformCalls==0 && d.fDeviceGeneration==~0ull);
    }
    std::printf("reset/FLR preflight: %u cases, %u checks, %u failures (production entries / synthetic HCA)\n",cases,checks,failures);
    return failures ? 1 : 0;
}
'''.replace('CODE', code)
    with tempfile.TemporaryDirectory(prefix='cx3-reset-preflight-') as temp:
        binary = str(Path(temp) / 'test')
        subprocess.run(['/usr/bin/clang++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-I', str(src), '-x', 'c++', '-', '-o', binary],
                       input=harness, text=True, check=True)
        return subprocess.run([binary], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
