#!/usr/bin/env python3
"""Execute production retention and queue teardown paths with a bounded ledger.

Breaks caught: full ledger loses the caller's owner, invalid CPU view authorises
free, and refusal still clears queue metadata or returns its MTT allocation.
IOKit/HCR and hashing are substituted; no real DMA, concurrency or reset proof.
"""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile

SRC = Path(__file__).resolve().parents[1] / 'src'


def method(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path)
    args = parser.parse_args()
    source = (args.source or SRC / 'Cx3Mlx4.cpp').read_text()
    header = (SRC / 'Cx3Mlx4.h').read_text()
    bodies = '\n'.join(method(source, sig) for sig in [
        'bool Cx3Mlx4::releaseIcmOrKeep(', 'bool Cx3Mlx4::quarantineTake(',
        'void Cx3Mlx4::qpTeardown(', 'void Cx3Mlx4::ingressTeardown(',
        'void Cx3Mlx4::rqTeardown(', 'void Cx3Mlx4::cqTeardown(',
        'void Cx3Mlx4::txTeardown(', 'void Cx3Mlx4::teardownEq(',
        'bool Cx3Mlx4::hasContextOwners() const'])
    harness = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
using UInt8=uint8_t; using UInt32=uint32_t; using UInt64=uint64_t;
#define CX3_HOST_TEST
#include "mlx4_pure.h"
#include "cx3_abi.h"
#include "Cx3PortQueues.h"
#include "mlx4_regs.h"
static void logSink(const char*,...) {}
#define LOG(...) logSink(__VA_ARGS__)
struct Mlx4Icm {
    UInt32 data[4]{0x11223344,0x55667788,0,0};
    void *virt=data; UInt64 bytes=16,busLow=0x100000; UInt32 npages=1;
    Mlx4DmaGeneration generation{};
};
struct Mlx4IcmSum { UInt64 bytes=0,nonzero=0; UInt32 checksum=0; };
struct Hw {
    unsigned frees=0,mttFrees=0,returns=0;
    UInt32 mttFreeOffset=0,mttFreePages=0;
    Mlx4Icm *mttFreeOwner=nullptr;
    bool cqReturnOk=true,eqReturnOk=true,mttFreeOk=true;
    bool hasMttAllocations() const { return false; } // no live MTT ledger in these owner cases
    void icmFree(Mlx4Icm*) { ++frees; }
    void icmSum(Mlx4Icm*,Mlx4IcmSum*) {}
    bool mttFree(UInt32 off,UInt32 pages,Mlx4Icm *owner) {
        ++mttFrees; mttFreeOffset=off; mttFreePages=pages; mttFreeOwner=owner; return mttFreeOk;
    }
    bool qpToReset(UInt32,UInt32 *s) { ++returns; *s=0; return true; }
    bool cqHw2Sw(UInt32,UInt32 *s) { ++returns; *s=cqReturnOk ? 0 : CX3_FWSTATUS_TIMEOUT; return cqReturnOk; }
    bool eqHw2Sw(UInt32,UInt32 *s) { ++returns; *s=eqReturnOk ? 0 : CX3_FWSTATUS_TIMEOUT; return eqReturnOk; }
};
class Cx3Mlx4 {
public:
    Cx3PortQueues fPortQueues[2] = { []() { Cx3PortQueues q{}; q.init(1); q.qpUp = false; q.ingressUp = false; q.macUp = false; q.rqUp = false; q.cqUp = false; q.rqUnknownRetained = false; q.txSqUnknownRetained = false; q.qpQpn = 64; q.qpProd = 5; q.qpState = MLX4_QP_STATE_RTS; q.qpRec = nullptr; q.qpRecPhys = 0x12000; q.ingressQpn = 65; q.ingressProd = 6; q.ingressState = MLX4_QP_STATE_RTS; q.ingressRec = nullptr; q.ingressRecPhys = 0x13000; q.macValue = 1; q.macSlot = 1; q.rqBufs = nullptr; q.rqRing = nullptr; q.rqNent = 128; q.rqPages = 2; q.rqMttOff = 8; q.rqMttPages = 2; q.rqMttAddr = 0x40; q.rawRxProd = 4; q.rawRxCons = 3; q.rawRxRecyclesTotal = 2; q.cqCqn = 128; q.cqMttOff = 10; q.cqMttPages = 2; q.cqNent = 128; q.cqRing = nullptr; q.cqRec = nullptr; q.cqUnknownRetained = false; q.cqHwReturned = false; q.cqMayBeDeviceOwned = true; q.cqRecPhys = 0x14000; q.cqConsIndex = 3; q.txQpUp = false; q.txCqUp = false; q.txQpReturned = false; q.txCqHwReturned = false; q.txQpn = 66; q.txQpState = MLX4_QP_STATE_RTS; q.txQpRec = nullptr; q.txSq = nullptr; q.rawFrame = nullptr; q.txQpRecPhys = 0x15000; q.txSqMttAddr = 0x80; q.txSqPhys = 0x16000; q.rawFramePhys = 0x17000; q.rawFrameBytes = 64; q.rawTxProd = 2; q.rawTxCons = 1; q.txSqMttOff = 16; q.txSqPages = 2; q.txSqMttPages = 2; q.txCqn = 129; q.txCqMttOff = 18; q.txCqMttPages = 2; q.txCqNent = 128; q.txCqRing = nullptr; q.txCqRec = nullptr; return q; }(), []() { Cx3PortQueues q{}; q.init(2); return q; }() };

public:
    enum { kQuarantineMax=QUAR_MAX,kQuarantineOverflowMax=OVERFLOW_MAX,
           kQuarantineMaxBytes=64*1024*1024 };
    struct QuarantineBlock {
        Mlx4Icm *icm; const char *tag; UInt64 busLow;
        UInt32 bytes,pages,txPosted,rxPosted,qpsUp,ownerWord;
        UInt32 baselineSum,baselineNonzero,lastSum,lastNonzero;
        Mlx4DmaGeneration generation;
    };
    Hw fHw;
    UInt32 fMrState=0;
    QuarantineBlock fQuar[kQuarantineMax]{};
    Mlx4Icm *fQuarOverflow[kQuarantineOverflowMax]{};
    Mlx4DmaGeneration fQuarOverflowGeneration[OVERFLOW_MAX]{};
    UInt32 fQuarCount=0,fQuarOverflowCount=0,fQuarRefused=0,fQuarOverflowUntracked=0;
    UInt64 fQuarBytes=0; bool fQuarExhausted=false,fNetEnabled=false;
    UInt32 fResetPreTxPosted=0,fResetPreRxPosted=0,fResetPreQpsUp=0,fResetPreOwner=0;
    
    
    
     
    
     
      
    
    
    
    
    
    
    bool fEqUnknownRetained=false;
    bool fEqHwReturned=false,fEqMayBeDeviceOwned=true;
     
    
    
    
    
     
    
    
    
    Mlx4Icm *fEqRing=nullptr; UInt32 fEqNent=128,fEqeFactor=1,fEqn=9;
    UInt32 fEqMTTOffset=20,fEqMttPages=2,fEqConsIndex=4; UInt64 fEqDoorbell=0x18000;
    bool releaseIcmOrKeep(Mlx4Icm**,const char*,bool);
    bool quarantineTake(Mlx4Icm*,const char*);
    bool hasContextOwners() const;
    void qpTeardown(const char*,bool,UInt32 port=1); void ingressTeardown(const char*,bool,UInt32 port=1);
    void rqTeardown(const char*,bool,UInt32 port=1); void cqTeardown(const char*,bool,UInt32 port=1);
    void txTeardown(const char*,bool,UInt32 port=1); void teardownEq(const char*,bool);
};
BODIES
static unsigned cases=0,checks=0,failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %s line %u: %s\n",name,__LINE__,#x); } } while(0)
static void fill(Cx3Mlx4 &d,Mlx4Icm *blocks,unsigned count=Cx3Mlx4::kQuarantineMax+Cx3Mlx4::kQuarantineOverflowMax) {
    for (unsigned i=0;i<count;++i)
        d.quarantineTake(&blocks[i],"existing owner");
}
int main() {
    {
        const char *name="quarantine preserves generation stamp"; ++cases;
        Cx3Mlx4 d; Mlx4Icm block; Mlx4Icm *slot=&block;
        block.generation.init(11); block.generation.markHandoffAttempted(12);
        CHECK(d.releaseIcmOrKeep(&slot,"generation",true));
        CHECK(slot==nullptr && d.fQuarCount==1);
        CHECK(d.fQuar[0].generation.allocationGeneration==11);
        CHECK(d.fQuar[0].generation.matchesOwnerGeneration(12));
    }
    constexpr unsigned capacity=Cx3Mlx4::kQuarantineMax+Cx3Mlx4::kQuarantineOverflowMax;
    Mlx4Icm blocks[capacity];
    {
        const char *name="full ledger retains caller owner"; ++cases; Cx3Mlx4 d;
        fill(d,blocks); Mlx4Icm extra; Mlx4Icm *p=&extra;
        CHECK(!d.releaseIcmOrKeep(&p,"no slot",true));
        CHECK(p==&extra && d.fHw.frees==0 && d.fQuarOverflowUntracked==0);
        CHECK(d.fQuarCount==Cx3Mlx4::kQuarantineMax && d.fQuarOverflowCount==Cx3Mlx4::kQuarantineOverflowMax);
        CHECK(extra.data[0]==0x11223344 && extra.data[1]==0x55667788);
    }
    for (unsigned form=0;form<3;++form) {
        const char *name="missing CPU view is not ownership proof"; ++cases;
        Cx3Mlx4 d; Mlx4Icm extra; Mlx4Icm *p=&extra;
        if (form==0) extra.virt=nullptr; else extra.bytes=form==1 ? 0 : 3;
        CHECK(!d.releaseIcmOrKeep(&p,"unreadable",true));
        CHECK(p==&extra && d.fHw.frees==0 && d.fQuarExhausted);
    }
    {
        const char *name="capacity stops network submission"; ++cases;
        Cx3Mlx4 d; d.fNetEnabled=true; fill(d,blocks);
        CHECK(d.fQuarExhausted && !d.fNetEnabled);
    }
    for (unsigned endpoint=0;endpoint<7;++endpoint) for (bool full : {false,true}) {
        const char *name="teardown retention contract"; ++cases; Cx3Mlx4 d;
        Mlx4Icm a,b,c; if (full) fill(d,blocks);
        switch (endpoint) {
            case 0: d.fNetEnabled=true; d.fPortQueues[0].qpUp=true; d.fPortQueues[0].qpRec=&a; d.qpTeardown("test",true);
                CHECK(d.fPortQueues[0].qpRec==(full ? &a : nullptr));
                CHECK(!d.fPortQueues[0].qpUp && !d.fNetEnabled && d.fPortQueues[0].qpRecPhys==(full ? 0x12000u : 0u));
                CHECK(d.fPortQueues[0].qpProd==(full ? 5u : 0u)); break;
            case 1: d.fNetEnabled=true; d.fPortQueues[0].ingressUp=d.fPortQueues[0].macUp=true; d.fPortQueues[0].ingressRec=&a;
                d.ingressTeardown("test",true);
                CHECK(d.fPortQueues[0].ingressRec==(full ? &a : nullptr));
                CHECK(!d.fPortQueues[0].ingressUp && !d.fNetEnabled && d.fPortQueues[0].macUp==full);
                CHECK(d.fPortQueues[0].ingressRecPhys==(full ? 0x13000u : 0u)); break;
            case 2: d.fPortQueues[0].rqUp=true; d.fPortQueues[0].rqBufs=&a; d.fPortQueues[0].rqRing=&b;
                d.rqTeardown("test",true);
                CHECK(d.fPortQueues[0].rqBufs==(full ? &a : nullptr) && d.fPortQueues[0].rqRing==(full ? &b : nullptr));
                CHECK(d.fPortQueues[0].rqUp==full && d.fPortQueues[0].rqMttOff==(full ? 8u : 0u));
                CHECK(d.fPortQueues[0].rqPages==(full ? 2u : 0u)); break;
            case 3: d.fPortQueues[0].cqUp=true; d.fPortQueues[0].cqRing=&a; d.fPortQueues[0].cqRec=&b;
                d.cqTeardown("test",true);
                CHECK(d.fPortQueues[0].cqRing==(full ? &a : nullptr) && d.fPortQueues[0].cqRec==(full ? &b : nullptr));
                CHECK(d.fPortQueues[0].cqUp==false && d.fPortQueues[0].cqMttOff==(full ? 10u : 0u));
                CHECK(d.fPortQueues[0].cqRecPhys==(full ? 0x14000u : 0u)); break;
            case 4: d.fPortQueues[0].txQpUp=true; d.fPortQueues[0].txQpRec=&a; d.fPortQueues[0].txSq=&b; d.fPortQueues[0].rawFrame=&c;
                d.txTeardown("test",true);
                CHECK(d.fPortQueues[0].txQpRec==(full ? &a : nullptr) && d.fPortQueues[0].txSq==(full ? &b : nullptr));
                CHECK(d.fPortQueues[0].rawFrame==(full ? &c : nullptr));
                CHECK(d.fPortQueues[0].txSqMttOff==(full ? 16u : 0u) && d.fPortQueues[0].txSqPages==(full ? 2u : 0u)); break;
            case 5: d.fPortQueues[0].txCqUp=true; d.fPortQueues[0].txCqRing=&a; d.fPortQueues[0].txCqRec=&b;
                d.fPortQueues[0].txSqMttOff=d.fPortQueues[0].txSqPages=0;
                d.txTeardown("test",true);
                CHECK(d.fPortQueues[0].txCqRing==(full ? &a : nullptr) && d.fPortQueues[0].txCqRec==(full ? &b : nullptr));
                CHECK(d.fPortQueues[0].txCqUp==full && d.fPortQueues[0].txCqMttOff==(full ? 18u : 0u));
                CHECK(d.fPortQueues[0].txCqMttPages==(full ? 2u : 0u)); break;
            case 6: d.fEqRing=&a; d.teardownEq("test",true);
                CHECK(d.fEqRing==(full ? &a : nullptr));
                CHECK(d.fEqNent==(full ? 128u : 0u) && d.fEqn==(full ? 9u : 0u));
                CHECK(d.fEqMTTOffset==20u && d.fEqMttPages==2u); break;
        }
        CHECK(d.fHw.frees==0 && d.fHw.returns==0);
        CHECK(d.fHw.mttFrees==0); /* reset-only retention never reuses MTT indices */
        if (full) CHECK(d.fHw.mttFrees==0 && d.fQuarOverflowUntracked==0);
    }
    for (unsigned endpoint=0;endpoint<4;++endpoint) {
        const char *name="second allocation refusal keeps dependency metadata"; ++cases;
        Cx3Mlx4 d; Mlx4Icm a,b,c; fill(d,blocks,capacity-1);
        switch (endpoint) {
            case 0: d.fPortQueues[0].rqUp=true; d.fPortQueues[0].rqBufs=&a; d.fPortQueues[0].rqRing=&b;
                d.rqTeardown("partial",true); d.rqTeardown("repeat partial",true);
                CHECK(!d.fPortQueues[0].rqBufs && d.fPortQueues[0].rqRing==&b && d.fPortQueues[0].rqUp && d.fPortQueues[0].rqPages==2); break;
            case 1: d.fPortQueues[0].cqUp=true; d.fPortQueues[0].cqRing=&a; d.fPortQueues[0].cqRec=&b;
                d.cqTeardown("partial",true); d.cqTeardown("repeat partial",true);
                CHECK(!d.fPortQueues[0].cqRing && d.fPortQueues[0].cqRec==&b && !d.fPortQueues[0].cqUp && d.fPortQueues[0].cqMttPages==2); break;
            case 2: d.fPortQueues[0].txQpUp=true; d.fPortQueues[0].txQpRec=&a; d.fPortQueues[0].txSq=&b; d.fPortQueues[0].rawFrame=&c;
                d.txTeardown("partial",true); d.txTeardown("repeat partial",true);
                CHECK(!d.fPortQueues[0].txQpRec && d.fPortQueues[0].txSq==&b && d.fPortQueues[0].rawFrame==&c && d.fPortQueues[0].txSqPages==2); break;
            case 3: d.fPortQueues[0].txCqUp=true; d.fPortQueues[0].txCqRing=&a; d.fPortQueues[0].txCqRec=&b;
                d.fPortQueues[0].txSqMttOff=d.fPortQueues[0].txSqPages=0;
                d.txTeardown("partial",true); d.txTeardown("repeat partial",true);
                CHECK(!d.fPortQueues[0].txCqRing && d.fPortQueues[0].txCqRec==&b && d.fPortQueues[0].txCqUp && d.fPortQueues[0].txCqMttPages==2); break;
        }
        CHECK(d.fQuarOverflow[ Cx3Mlx4::kQuarantineOverflowMax-1 ]==&a);
        CHECK(d.fHw.frees==0 && d.fHw.mttFrees==0 && d.fQuarOverflowUntracked==0);
    }
    {
        const char *name="EQ hand-back refusal survives a repeated teardown"; ++cases;
        Cx3Mlx4 d; Mlx4Icm ring; d.fEqRing=&ring; d.fEqUnknownRetained=true; d.fHw.eqReturnOk=false;
        d.teardownEq("failed",false); d.teardownEq("failed again",false);
        CHECK(d.fEqRing==&ring && d.fEqn==9 && d.fEqNent==128 && d.fEqMTTOffset==20);
        CHECK(d.fEqUnknownRetained);
        CHECK(d.fHw.frees==0 && d.fHw.mttFrees==0 && d.fHw.returns==2);
        d.fHw.eqReturnOk=true; d.teardownEq("successful return",false);
        CHECK(!d.fEqRing && d.fEqn==0 && d.fHw.frees==1 && d.fHw.returns==3);
        CHECK(!d.fEqUnknownRetained);
        CHECK(d.fHw.mttFrees==1 && d.fHw.mttFreeOffset==20 && d.fHw.mttFreePages==2);
    }
    {
        const char *name="CQ unknown retained owner can retry hand-back"; ++cases;
        Cx3Mlx4 d; Mlx4Icm ring,rec;
        d.fPortQueues[0].cqUp=false; d.fPortQueues[0].cqRing=&ring; d.fPortQueues[0].cqRec=&rec;
        d.fPortQueues[0].cqUnknownRetained=true;
        d.fPortQueues[0].cqCqn=128; d.fPortQueues[0].cqMttOff=24; d.fPortQueues[0].cqMttPages=3;
        d.fPortQueues[0].cqNent=128; d.fPortQueues[0].cqRecPhys=0x24000;
        d.fHw.cqReturnOk=false;
        d.cqTeardown("unknown retained",false);
        CHECK(d.fHw.returns==1 && d.fPortQueues[0].cqRing==&ring && d.fPortQueues[0].cqRec==&rec);
        CHECK(d.fPortQueues[0].cqUnknownRetained);
        CHECK(d.fPortQueues[0].cqMttOff==24 && d.fPortQueues[0].cqMttPages==3 && d.fHw.frees==0 && d.fHw.mttFrees==0);
        d.fHw.cqReturnOk=true;
        d.cqTeardown("unknown retained retry",false);
        CHECK(d.fHw.returns==2 && !d.fPortQueues[0].cqRing && !d.fPortQueues[0].cqRec);
        CHECK(!d.fPortQueues[0].cqUnknownRetained);
        CHECK(d.fPortQueues[0].cqMttOff==0 && d.fPortQueues[0].cqMttPages==0 && d.fHw.frees==2);
        CHECK(d.fHw.mttFrees==1 && d.fHw.mttFreeOffset==24 && d.fHw.mttFreePages==3);
    }
    {
        const char *name="CQ MTT refusal preserves owner and retries without duplicate HW2SW"; ++cases;
        Cx3Mlx4 d; Mlx4Icm ring,rec; d.fPortQueues[0].cqRing=&ring; d.fPortQueues[0].cqRec=&rec;
        d.fPortQueues[0].cqUp=true; d.fPortQueues[0].cqMttOff=30; d.fPortQueues[0].cqMttPages=2; d.fHw.mttFreeOk=false;
        d.cqTeardown("MTT refusal",false);
        CHECK(d.fHw.returns==1 && d.fHw.mttFrees==1 && d.fPortQueues[0].cqHwReturned && !d.fPortQueues[0].cqUp);
        CHECK(d.fPortQueues[0].cqRing==&ring && d.fPortQueues[0].cqRec==&rec && d.fPortQueues[0].cqMttOff==30 && d.fPortQueues[0].cqMttPages==2);
        CHECK(d.fHw.frees==0);
        d.fHw.mttFreeOk=true; d.cqTeardown("MTT retry",false);
        CHECK(d.fHw.returns==1 && d.fHw.mttFrees==2 && d.fHw.frees==2);
        CHECK(!d.fPortQueues[0].cqRing && !d.fPortQueues[0].cqRec && d.fPortQueues[0].cqMttPages==0 && !d.fPortQueues[0].cqHwReturned);
    }
    {
        const char *name="pre-handoff CQ MTT refusal retries without issuing HW2SW"; ++cases;
        Cx3Mlx4 d; Mlx4Icm ring,rec; d.fPortQueues[0].cqRing=&ring; d.fPortQueues[0].cqRec=&rec;
        d.fPortQueues[0].cqUp=false; d.fPortQueues[0].cqUnknownRetained=true; d.fPortQueues[0].cqMayBeDeviceOwned=false;
        d.fHw.mttFreeOk=false;
        d.cqTeardown("pre-handoff MTT refusal",false);
        CHECK(d.fHw.returns==0 && d.fHw.mttFrees==1 && d.fHw.frees==0);
        CHECK(d.fPortQueues[0].cqRing==&ring && d.fPortQueues[0].cqRec==&rec && d.fPortQueues[0].cqUnknownRetained);
        d.fHw.mttFreeOk=true; d.cqTeardown("pre-handoff MTT retry",false);
        CHECK(d.fHw.returns==0 && d.fHw.mttFrees==2 && d.fHw.frees==2);
        CHECK(!d.fPortQueues[0].cqRing && !d.fPortQueues[0].cqRec && !d.fPortQueues[0].cqUnknownRetained && !d.fPortQueues[0].cqMayBeDeviceOwned);
    }
    {
        const char *name="EQ MTT refusal preserves owner and retries without duplicate HW2SW"; ++cases;
        Cx3Mlx4 d; Mlx4Icm ring; d.fEqRing=&ring; d.fHw.mttFreeOk=false;
        d.teardownEq("MTT refusal",false);
        CHECK(d.fHw.returns==1 && d.fHw.mttFrees==1 && d.fEqHwReturned && d.fEqUnknownRetained);
        CHECK(d.fEqRing==&ring && d.fEqMTTOffset==20 && d.fEqMttPages==2 && d.fHw.frees==0);
        d.fHw.mttFreeOk=true; d.teardownEq("MTT retry",false);
        CHECK(d.fHw.returns==1 && d.fHw.mttFrees==2 && d.fHw.frees==1);
        CHECK(!d.fEqRing && d.fEqMttPages==0 && !d.fEqHwReturned && !d.fEqUnknownRetained);
    }
    {
        const char *name="pre-handoff EQ MTT refusal retries without issuing HW2SW"; ++cases;
        Cx3Mlx4 d; Mlx4Icm ring; d.fEqRing=&ring; d.fEqUnknownRetained=true;
        d.fEqMayBeDeviceOwned=false; d.fHw.mttFreeOk=false;
        d.teardownEq("pre-handoff MTT refusal",false);
        CHECK(d.fHw.returns==0 && d.fHw.mttFrees==1 && d.fHw.frees==0);
        CHECK(d.fEqRing==&ring && d.fEqUnknownRetained && !d.fEqMayBeDeviceOwned);
        d.fHw.mttFreeOk=true; d.teardownEq("pre-handoff MTT retry",false);
        CHECK(d.fHw.returns==0 && d.fHw.mttFrees==2 && d.fHw.frees==1);
        CHECK(!d.fEqRing && !d.fEqUnknownRetained && !d.fEqMayBeDeviceOwned);
    }
    {
        const char *name="RQ MTT refusal preserves ring and buffer owner fields"; ++cases;
        Cx3Mlx4 d; Mlx4Icm ring,bufs; d.fPortQueues[0].rqUp=true; d.fPortQueues[0].rqRing=&ring; d.fPortQueues[0].rqBufs=&bufs;
        d.fHw.mttFreeOk=false; d.rqTeardown("MTT refusal",false);
        CHECK(d.fHw.mttFrees==1 && d.fHw.frees==0 && d.fPortQueues[0].rqRing==&ring && d.fPortQueues[0].rqBufs==&bufs);
        CHECK(d.fPortQueues[0].rqMttOff==8 && d.fPortQueues[0].rqMttPages==2 && d.fPortQueues[0].rqUp);
        d.fHw.mttFreeOk=true; d.rqTeardown("MTT retry",false);
        CHECK(d.fHw.mttFrees==2 && d.fHw.frees==2 && !d.fPortQueues[0].rqRing && !d.fPortQueues[0].rqBufs);
        CHECK(d.fPortQueues[0].rqMttPages==0 && !d.fPortQueues[0].rqUp);
    }
    {
        const char *name="pre-handoff RQ owner remains reachable after MTT refusal"; ++cases;
        Cx3Mlx4 d; Mlx4Icm ring,bufs; d.fPortQueues[0].rqRing=&ring; d.fPortQueues[0].rqBufs=&bufs;
        d.fPortQueues[0].rqUnknownRetained=true; d.fPortQueues[0].rqUp=false; d.fHw.mttFreeOk=false;
        d.rqTeardown("create rollback refusal",false);
        CHECK(d.fHw.mttFrees==1 && d.fHw.frees==0 && d.fPortQueues[0].rqRing==&ring && d.fPortQueues[0].rqBufs==&bufs);
        CHECK(d.fPortQueues[0].rqUnknownRetained && !d.fPortQueues[0].rqUp && d.fPortQueues[0].rqMttOff==8 && d.fPortQueues[0].rqMttPages==2);
        d.fHw.mttFreeOk=true; d.rqTeardown("create rollback retry",false);
        CHECK(d.fHw.mttFrees==2 && d.fHw.frees==2 && !d.fPortQueues[0].rqRing && !d.fPortQueues[0].rqBufs);
        CHECK(!d.fPortQueues[0].rqUnknownRetained && !d.fPortQueues[0].rqUp && d.fPortQueues[0].rqMttPages==0);
    }
    {
        const char *name="TX SQ MTT refusal keeps QP return and retries without duplicate reset"; ++cases;
        Cx3Mlx4 d; Mlx4Icm qp,sq,frame; d.fPortQueues[0].txQpUp=true; d.fPortQueues[0].txQpRec=&qp;
        d.fPortQueues[0].txSq=&sq; d.fPortQueues[0].rawFrame=&frame; d.fHw.mttFreeOk=false;
        d.txTeardown("MTT refusal",false);
        CHECK(d.fHw.returns==1 && d.fHw.mttFrees==1 && d.fPortQueues[0].txQpReturned && d.fHw.frees==0);
        CHECK(d.fPortQueues[0].txQpRec==&qp && d.fPortQueues[0].txSq==&sq && d.fPortQueues[0].txSqMttPages==2);
        d.fHw.mttFreeOk=true; d.txTeardown("MTT retry",false);
        CHECK(d.fHw.returns==1 && d.fHw.mttFrees==2 && d.fHw.frees==3);
        CHECK(!d.fPortQueues[0].txQpRec && !d.fPortQueues[0].txSq && d.fPortQueues[0].txSqMttPages==0 && !d.fPortQueues[0].txQpReturned);
    }
    {
        const char *name="pre-handoff SQ owner retries MTT release without QP command"; ++cases;
        Cx3Mlx4 d; Mlx4Icm sq; d.fPortQueues[0].txSq=&sq; d.fPortQueues[0].txSqUnknownRetained=true;
        d.fPortQueues[0].txSqMttOff=22; d.fPortQueues[0].txSqMttPages=3; d.fPortQueues[0].txSqPages=3;
        d.fPortQueues[0].txQpUp=false; d.fPortQueues[0].txQpRec=nullptr; d.fHw.mttFreeOk=false;
        d.txTeardown("create rollback refusal",false);
        CHECK(d.fHw.returns==0 && d.fHw.mttFrees==1 && d.fHw.frees==0);
        CHECK(d.fPortQueues[0].txSq==&sq && d.fPortQueues[0].txSqUnknownRetained && d.fPortQueues[0].txSqMttOff==22);
        d.fHw.mttFreeOk=true; d.txTeardown("create rollback retry",false);
        CHECK(d.fHw.returns==0 && d.fHw.mttFrees==2 && d.fHw.frees==1);
        CHECK(!d.fPortQueues[0].txSq && !d.fPortQueues[0].txSqUnknownRetained && d.fPortQueues[0].txSqMttPages==0);
    }
    {
        const char *name="TX CQ MTT refusal retries after one queue return"; ++cases;
        Cx3Mlx4 d; Mlx4Icm ring,rec; d.fPortQueues[0].txCqUp=true; d.fPortQueues[0].txCqRing=&ring; d.fPortQueues[0].txCqRec=&rec;
        d.fPortQueues[0].txSqMttOff=d.fPortQueues[0].txSqMttPages=0;
        d.fHw.mttFreeOk=false; d.txTeardown("MTT refusal",false);
        CHECK(d.fHw.returns==1 && d.fHw.mttFrees==1 && d.fPortQueues[0].txCqHwReturned && d.fHw.frees==0);
        CHECK(d.fPortQueues[0].txCqRing==&ring && d.fPortQueues[0].txCqRec==&rec && d.fPortQueues[0].txCqMttPages==2);
        d.fHw.mttFreeOk=true; d.txTeardown("MTT retry",false);
        CHECK(d.fHw.returns==1 && d.fHw.mttFrees==2 && d.fHw.frees==2);
        CHECK(!d.fPortQueues[0].txCqRing && !d.fPortQueues[0].txCqRec && d.fPortQueues[0].txCqMttPages==0 && !d.fPortQueues[0].txCqHwReturned);
    }
    for (bool afterReset : {true,false}) {
        const char *name="raw frame owner after partial TX retention"; ++cases;
        Cx3Mlx4 d;d.fPortQueues[0].cqMayBeDeviceOwned=false; /* fixture has no CQ handoff in this case */ Mlx4Icm qp,sq,frame;
        d.fEqNent=0; // all other context-owner predicates are absent
        d.fPortQueues[0].txQpUp=true; d.fPortQueues[0].txQpRec=&qp;
        d.fPortQueues[0].txSq=&sq; d.fPortQueues[0].rawFrame=&frame;
        // Two transfers fit; the raw frame is the first allocation refused.
        fill(d,blocks,capacity-2);
        d.txTeardown("partial raw-frame retention",afterReset);
        CHECK(!d.fPortQueues[0].txQpRec && !d.fPortQueues[0].txSq);
        if (afterReset) {
            CHECK(d.fPortQueues[0].rawFrame==&frame && d.fHw.frees==0 && d.fQuarExhausted);
            CHECK(d.fQuarCount+d.fQuarOverflowCount==capacity);
            CHECK(d.hasContextOwners());
            d.txTeardown("repeat partial raw-frame retention",true);
            CHECK(d.fPortQueues[0].rawFrame==&frame && d.hasContextOwners() && d.fHw.frees==0);
            CHECK(d.fPortQueues[0].rawFramePhys==0x17000 && d.fPortQueues[0].rawFrameBytes==64);
        } else {
            // Ordinary stop does not transfer these blocks to quarantine;
            // capacity alone cannot manufacture this refusal on that path.
            CHECK(!d.fPortQueues[0].rawFrame && d.fHw.frees==3 && !d.hasContextOwners());
            CHECK(d.fHw.returns==1 && d.fHw.mttFrees==1 &&
                  d.fQuarCount+d.fQuarOverflowCount==capacity-2);
            CHECK(d.fPortQueues[0].rawFramePhys==0 && d.fPortQueues[0].rawFrameBytes==0);
        }
    }
    std::printf("retention teardown: %u cases, %u checks, %u failures (synthetic IOKit/HCR)\n",cases,checks,failures);
    return failures ? 1 : 0;
}
'''.replace('BODIES', bodies)
    for placeholder, name in [('QUAR_MAX', 'kQuarantineMax'), ('OVERFLOW_MAX', 'kQuarantineOverflowMax')]:
        harness = harness.replace(placeholder, re.search(r'\b' + name + r'\s*=\s*(\d+)', header).group(1))
    with tempfile.TemporaryDirectory(prefix='cx3-retention-') as tmp:
        binary = str(Path(tmp) / 'test')
        subprocess.run(['/usr/bin/clang++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-I', str(SRC), '-x', 'c++', '-', '-o', binary],
                       input=harness, text=True, check=True)
        return subprocess.run([binary]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
