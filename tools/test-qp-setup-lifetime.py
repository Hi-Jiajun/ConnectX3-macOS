#!/usr/bin/env python3
"""Run complete production RX/RSS setup exports against a synthetic HCA.

Catches record free/lost owner on transition failure and rollback failure, retry
overwriting an unresolved QP, query failure still publishing producer, and enable
of a partial QP. Context builders and final comparisons are production methods;
only HCA transport, allocator and IOKit boundaries are synthetic.
"""
from pathlib import Path
import subprocess
import tempfile

SRC=Path(__file__).resolve().parents[1]/'src'


def method(text, signature):
    start=text.index(signature)
    while text.index(';',start)<text.index('{',start):
        start=text.index(signature,start+len(signature))
    return text[start:text.index('\n}\n',start)+3]


def main():
    source=(SRC/'Cx3Mlx4.cpp').read_text()
    hw=(SRC/'Mlx4Hw.cpp').read_text()
    bodies='\n'.join(method(source,s) for s in [
        'static void r4QpQueryAndCompare(', 'static void r4IngressQueryAndCompare(',
        'static void r4QpPublishScalars(', 'static void r4IngressPublishScalars(',
        'static void r4IngressMacEntry(', 'static void r4IngressBuildKey(',
        'IOReturn Cx3Mlx4::exportR4Qp(', 'IOReturn Cx3Mlx4::exportR4Ingress(',
        'void Cx3Mlx4::qpTeardown(', 'void Cx3Mlx4::ingressTeardown(',
        'void Cx3Mlx4::rqTeardown(', 'void Cx3Mlx4::cqTeardown(',
        'void Cx3Mlx4::mrTeardown(',
        'IOReturn Cx3Mlx4::enable('])
    bodies='\n'.join(method(hw,s) for s in [
        'void Mlx4Hw::qpFillContext(', 'void Mlx4Hw::qpFillRssContext('])+'\n'+bodies
    harness=r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#define CX3_HOST_TEST
#include "mlx4_pure.h"
using UInt8=uint8_t; using UInt16=uint16_t; using UInt32=uint32_t; using UInt64=uint64_t;
using IOReturn=int;
#include "cx3_abi.h"
#include "Cx3PortQueues.h"
#include "mlx4_regs.h"
constexpr int kIOReturnSuccess=0,kIOReturnError=-1,kIOReturnNotReady=-2,
    kIOReturnBadArgument=-3,kIOReturnNoMemory=-4,kIOReturnUnsupported=-5,
    kIOReturnNoSpace=-6,kIOReturnNotPermitted=-7;
static void logSink(const char*,...) {}
#define LOG(...) logSink(__VA_ARGS__)
static UInt32 cx3GetBe32(const void *p) {
    const auto *b=static_cast<const UInt8*>(p);
    return UInt32(b[0])<<24 | UInt32(b[1])<<16 | UInt32(b[2])<<8 | b[3];
}
static UInt32 OSSwapBigToHostInt32(UInt32 v) { return __builtin_bswap32(v); }
static UInt64 cx3GetBe64(const void *p) {
    return UInt64(cx3GetBe32(p))<<32|cx3GetBe32(static_cast<const UInt8*>(p)+4);
}
static UInt32 mlx4GetBe32(const void *p) { return cx3GetBe32(p); }
static void mlx4PutBe16(void *p,UInt16 n) { auto *b=static_cast<UInt8*>(p); b[0]=n>>8;b[1]=n; }
static void mlx4PutBe32(void *p,UInt32 n) { auto *b=static_cast<UInt8*>(p); for(unsigned i=0;i<4;++i)b[i]=n>>(24-i*8); }
static void mlx4PutBe64(void *p,UInt64 n) { auto *b=static_cast<UInt8*>(p); for(unsigned i=0;i<8;++i)b[i]=n>>(56-i*8); }
static unsigned mlx4Log2(unsigned v) { return 31-__builtin_clz(v); }
struct Page { UInt64 busAddr=0x1234000; };
struct Mlx4Icm {
    UInt8 buffer[4096]{}; void *virt=buffer; UInt64 bytes=4096;
    Page page; Page *pages=&page;
};
struct Mlx4Hw {
    Mlx4Icm allocation;
    unsigned allocations=0,frees=0,modifies=0,queries=0,resets=0,producers=0,mttFrees=0;
    unsigned cqReturns=0,mrReturns=0,lastQueryPort=0,lastMacPort=0,lastResetQpn=0;
    bool markDmaHandoff(Mlx4Icm *const*,UInt32) { return true; }
    unsigned failStep=0; UInt32 failStatus=3,state=0,qpn=0;
    unsigned badQuery=99,queryFault=0,postFailureCalls=0;
    UInt8 context[QP_CONTEXT_SIZE]{};
    bool resetOk=true,producerOk=true,wedged=false,cqReturnOk=true,mrReturnOk=true,mttFreeOk=true;
    Mlx4Icm *icmAlloc(unsigned,bool) { ++allocations; return &allocation; }
    void icmFree(Mlx4Icm*) { ++frees; }
    void qpFillContext(UInt8*,UInt32,UInt32,UInt32,UInt32,UInt32,UInt32,UInt64,
                       UInt32,UInt32,bool,UInt32,bool,bool,int);
    void qpFillRssContext(UInt8*,UInt32,UInt32,UInt32,bool,const UInt8*,UInt32);
    void capture(Cx3QpCaptureObservation *o,UInt32 status,UInt32 bytes,UInt64 stamp) {
        if (!o) return;
        memset(o,0,sizeof(*o));o->abiVersion=1;o->abiBytes=64;
        o->preparedBytes=bytes;o->capturedBytes=bytes;o->fwStatus=status;
        o->command.abiVersion=1;o->command.abiBytes=40;
        o->command.flags=7;o->command.fwStatus=status;o->command.startNs=stamp;
    }
    bool qpModify(UInt16,UInt32 number,const UInt8 *p,UInt32 *status,UInt8*,UInt32,
                   Cx3QpCaptureObservation *o=nullptr) {
        qpn=number;
        if(wedged)++postFailureCalls;
        if (modifies++==failStep) { *status=failStatus; capture(o,*status,264,1000+modifies);wedged=failStatus==CX3_FWSTATUS_TIMEOUT; return false; }
        capture(o,0,264,1000+modifies);
        memcpy(context,p,sizeof(context));state=cx3GetBe32(p)>>28; *status=0; return true;
    }
    bool qpQuery(UInt32 number,UInt8 *p,UInt32 size,UInt32 *status,UInt8*,UInt32,
                  Cx3QpCaptureObservation *o=nullptr) {
        if(wedged)++postFailureCalls;
        unsigned call=queries++;memcpy(p,context,size);
        capture(o,0,4096,2000+queries);
        if(call==badQuery) {
            if(queryFault==0){*status=CX3_FWSTATUS_TIMEOUT;capture(o,*status,4096,2000+queries);o && (o->capturedBytes=0);wedged=true;return false;}
            if(queryFault==1)p[0]^=0x10;
            if(queryFault==2)p[QP_CONTEXT_LOCAL_QPN_OFFSET+3]^=1;
            if(queryFault==3)p[QP_CONTEXT_MTT_BASE_L_OFFSET+3]^=8;
            if(queryFault==4)p[QP_CONTEXT_CQN_SEND_OFFSET+3]^=1;
            if(queryFault==5)p[QP_CONTEXT_RSS_KEY_OFFSET]^=1;
            if(queryFault==6)p[QP_CONTEXT_RQ_SIZE_STRIDE_OFFSET]^=1;
            if(queryFault==7)mlx4PutBe32(p+QP_CONTEXT_FLAGS_OFFSET,
                cx3GetBe32(p+QP_CONTEXT_FLAGS_OFFSET)&~(1u<<MLX4_RSS_QPC_FLAG_OFFSET));
            if(queryFault==8)p[QP_CONTEXT_LOG_PAGE_SIZE_OFFSET]^=1;
        }
        *status=0; return true;
    }
    bool qpToReset(UInt32 n,UInt32 *status) { lastResetQpn=n; ++resets; *status=resetOk ? 0 : CX3_FWSTATUS_TIMEOUT; return resetOk; }
    void rqSetProducer(volatile UInt32 *p,UInt32 prod) { ++producers; *p=__builtin_bswap32(producerOk ? prod : prod+1); }
    bool queryPort(unsigned port,Cx3QueryPortResult *r) { lastQueryPort=port; memset(r,0,sizeof(*r)); r->defMac[7]=1; return true; }
    bool setPortMacTableWrite(unsigned port,const UInt8*,unsigned,UInt32 *status) { lastMacPort=port; *status=0; return true; }
    bool cmdWedged() const { return wedged; }
    bool mttFree(UInt32,UInt32,Mlx4Icm*) { ++mttFrees; return mttFreeOk; }
    bool cqHw2Sw(UInt32,UInt32 *status) { ++cqReturns; *status=cqReturnOk ? 0 : CX3_FWSTATUS_TIMEOUT; return cqReturnOk; }
    bool mrHw2Sw(UInt32,UInt32 *status) { ++mrReturns; *status=mrReturnOk ? 0 : CX3_FWSTATUS_TIMEOUT; return mrReturnOk; }
};
struct IONetworkInterface {};
struct Timer { void wakeAtTime(UInt64) {} void cancelTimeout() {} };
constexpr UInt64 kNetTickNs=1000000;
static UInt64 cx3WakeInTicks(UInt64 n) { return n; }
class Cx3Mlx4 {
public:
    UInt64 fDeviceGeneration=2;
    Cx3AutoNetworkResult fAutoNetwork{}; bool fAutoNetworkUserDisabled=false;
    Cx3PortQueues fPortQueues[2] = { []() { Cx3PortQueues q{}; q.init(1); q.cqUp = true; q.cqUnknownRetained = false; q.cqHwReturned = false; q.cqMayBeDeviceOwned = true; q.rqUp = true; q.rqUnknownRetained = false; q.qpUp = false; q.ingressUp = false; q.macUp = false; q.cqCqn = 128; q.cqUarStart = 1; q.rqNent = 128; q.rqMttOff = 0; q.rqMttPages = 0; q.cqMttOff = 2; q.cqMttPages = 1; q.cqNent = 128; q.cqConsIndex = 0; q.cqRecPhys = 0; q.cqRing = nullptr; q.cqRec = nullptr; q.txQpUp = false; q.txQpReturned = false; q.txQpRec = nullptr; q.rqMttAddr = 0x8000; q.rqBufs = nullptr; q.rqRing = nullptr; q.rqPages = 1; q.rawRxProd = 0; q.rawRxCons = 0; q.rawRxRecyclesTotal = 0; q.qpQpn = 64; q.qpState = 0; q.qpProd = 0; q.qpRecPhys = 0; q.qpRec = nullptr; q.ingressQpn = 65; q.ingressState = 0; q.ingressProd = 0; q.ingressRecPhys = 0; q.ingressRec = nullptr; q.macValue = 0; q.macSlot = 0; return q; }(), []() { Cx3PortQueues q{}; q.init(2); return q; }() };

public:
    Mlx4Hw fHw;
    struct { UInt32 reserved_qps=64,bmme_flags=0; UInt64 flags=0; } fDevCap;
    struct { UInt32 log_num_qps=10; } fInitHca;
    bool fHcaInited=true;
    UInt32 fMrPdn=1,fMrState=1;
    
    UInt32 fMrMptIndex=7;
    
    
     
    
    
    
    
    
     
    
     
      
    Mlx4TxAdmissionGate fNetTxAdmission{};
    bool fResetCommandsBlocked=false,fStopRetained=false;
    bool fNetEnabled=false,fQuarExhausted=false; Timer *fNetTimer=nullptr; UInt32 fNetUnit=2;
    bool netAttachPoisoned() { return false; }
    void publishLinkStatus(const char*) {}
    bool releaseIcmOrKeep(Mlx4Icm **p,const char*,bool keep) {
        if (!keep && *p) fHw.icmFree(*p); *p=nullptr; return true;
    }
    IOReturn exportR4Qp(const UInt64*,Cx3R4QpResult*,UInt64*,UInt32 port=1);
    IOReturn exportR4Ingress(Cx3R4IngressResult*,UInt64*,UInt32 port=1);
    void qpTeardown(const char*,bool,UInt32 port=1);
    void ingressTeardown(const char*,bool,UInt32 port=1);
    void rqTeardown(const char*,bool,UInt32 port=1);
    void cqTeardown(const char*,bool,UInt32 port=1);
    void mrTeardown(const char*,bool);
    IOReturn enable(IONetworkInterface*);
};
BODIES
static unsigned checks=0,failures=0,cases=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %s line %u: %s\n",name,__LINE__,#x); } } while(0)
int main() {
    for (bool ingress : {false,true}) {
        const char *name="per-call capture output remains bound to each RX/RSS step"; ++cases;
        Cx3Mlx4 d; d.fHw.failStep=99;
        if(ingress){d.fPortQueues[0].qpUp=true;d.fPortQueues[0].qpState=MLX4_QP_STATE_RTS;}
        UInt64 in[kR4QpInCount]{},out[16]{};Cx3R4QpResult qp{};Cx3R4IngressResult rss{};
        if(ingress)d.exportR4Ingress(&rss,out);else d.exportR4Qp(in,&qp,out);
#ifdef CX3_R4QP_ABI
        const auto *sent=ingress ? rss.modifyCapture : qp.modifyCapture;
        const auto *queried=ingress ? rss.queryCapture : qp.queryCapture;
        for(unsigned step=0;step<3;++step) {
            CHECK(sent[step].abiVersion==1 && sent[step].capturedBytes==264);
            CHECK(sent[step].command.startNs==1001+step);
            CHECK(queried[step].abiVersion==1 && queried[step].capturedBytes==4096);
            CHECK(queried[step].command.startNs==2001+step);
        }
        CHECK((ingress ? rss.finalQueryCapture : qp.finalQueryCapture).command.startNs==2004);
        if(ingress)d.exportR4Ingress(&rss,out);else d.exportR4Qp(in,&qp,out);
        sent=ingress ? rss.modifyCapture : qp.modifyCapture;
        queried=ingress ? rss.queryCapture : qp.queryCapture;
        for(unsigned step=0;step<3;++step) {
            CHECK(sent[step].abiVersion==0 && sent[step].command.flags==0);
            CHECK(queried[step].abiVersion==0 && queried[step].command.flags==0);
        }
        CHECK((ingress ? rss.finalQueryCapture : qp.finalQueryCapture).command.startNs==2005);
#else
        CHECK(false); // Existing results cannot carry any per-command capture evidence.
#endif
    }
    for(bool ingress : {false,true}) {
        const char *name="ready RX/RSS loses readiness before failed teardown"; ++cases;
        Cx3Mlx4 d;d.fHw.failStep=99;
        if(ingress){d.fPortQueues[0].qpUp=true;d.fPortQueues[0].qpState=MLX4_QP_STATE_RTS;}
        UInt64 in[kR4QpInCount]{},out[16]{};Cx3R4QpResult qp{};Cx3R4IngressResult rss{};
        if(ingress)d.exportR4Ingress(&rss,out);else d.exportR4Qp(in,&qp,out);
        CHECK((ingress ? d.fPortQueues[0].ingressUp : d.fPortQueues[0].qpUp) && d.fHw.producers==1);
        d.fNetEnabled=true;d.fHw.resetOk=false;
        if(ingress)d.ingressTeardown("return failed",false);else d.qpTeardown("return failed",false);
        CHECK(!(ingress ? d.fPortQueues[0].ingressUp : d.fPortQueues[0].qpUp) && !d.fNetEnabled);
        CHECK((ingress ? d.fPortQueues[0].ingressRec : d.fPortQueues[0].qpRec)==&d.fHw.allocation && d.fHw.frees==0);
        CHECK((ingress ? d.fPortQueues[0].ingressState : d.fPortQueues[0].qpState)!=MLX4_QP_STATE_RTS);
        CHECK(d.fHw.resets==1 && d.fHw.mttFrees==0);
        IONetworkInterface iface;CHECK(d.enable(&iface)==kIOReturnNotPermitted && !d.fNetEnabled);
        unsigned queries=d.fHw.queries;
        if(ingress)d.exportR4Ingress(&rss,out);else d.exportR4Qp(in,&qp,out);
        CHECK(d.fHw.allocations==1 && d.fHw.queries==queries && d.fHw.producers==1);
        if(ingress)d.ingressTeardown("repeat failure",false);else d.qpTeardown("repeat failure",false);
        CHECK(d.fHw.resets==2 && d.fHw.frees==0 && (ingress ? d.fPortQueues[0].ingressRec : d.fPortQueues[0].qpRec)==&d.fHw.allocation);
        d.fHw.resetOk=true;
        if(ingress)d.ingressTeardown("returned",false);else d.qpTeardown("returned",false);
        CHECK(d.fHw.resets==3 && d.fHw.frees==1 && !(ingress ? d.fPortQueues[0].ingressRec : d.fPortQueues[0].qpRec));
        CHECK(!(ingress ? d.fPortQueues[0].ingressUp : d.fPortQueues[0].qpUp) && !d.fNetEnabled);
    }
    for (bool ingress : {false,true}) for(unsigned query=0;query<4;++query)
    for(unsigned fault : {0u,1u,2u,3u,4u,5u,7u,8u}) {
        if((query<3 && fault>2) || (!ingress && fault==7))continue;
        const char *name=ingress ? "RSS bad query must not publish" : "RX bad query must not publish"; ++cases;
        Cx3Mlx4 d; d.fHw.failStep=99;d.fHw.badQuery=query;
        d.fHw.queryFault=(!ingress && fault==5) ? 6 : fault;
        d.fNetEnabled=true;
        if(ingress){d.fPortQueues[0].qpUp=true;d.fPortQueues[0].qpState=MLX4_QP_STATE_RTS;}
        UInt64 in[kR4QpInCount]{},out[16]{};Cx3R4QpResult qp{};Cx3R4IngressResult rss{};
        if(ingress)d.exportR4Ingress(&rss,out);else d.exportR4Qp(in,&qp,out);
        CHECK(d.fHw.modifies==(query==3 ? 3u : query+1) && d.fHw.queries==query+1);
        CHECK(d.fHw.producers==0 && d.fHw.frees==0 && d.fHw.postFailureCalls==0);
        CHECK(cx3GetBe32(d.fHw.allocation.virt)==0);
        CHECK(!(ingress ? d.fPortQueues[0].ingressUp : d.fPortQueues[0].qpUp) && !d.fNetEnabled);
        CHECK((ingress ? d.fPortQueues[0].ingressRec : d.fPortQueues[0].qpRec)==&d.fHw.allocation);
        CHECK((ingress ? d.fPortQueues[0].ingressState : d.fPortQueues[0].qpState)!=MLX4_QP_STATE_RTS);
        CHECK((ingress ? rss.status : qp.status)==UInt32(kIOReturnNotReady));
        unsigned alloc=d.fHw.allocations,modify=d.fHw.modifies,queries=d.fHw.queries;
        if(ingress)d.exportR4Ingress(&rss,out);else d.exportR4Qp(in,&qp,out);
        CHECK(d.fHw.allocations==alloc && d.fHw.modifies==modify && d.fHw.queries==queries);
        IONetworkInterface iface;CHECK(d.enable(&iface)==kIOReturnNotPermitted && !d.fNetEnabled);
        d.fHw.resetOk=false;
        if(ingress)d.ingressTeardown("query failed",false);else d.qpTeardown("query failed",false);
        CHECK(d.fHw.frees==0 && (ingress ? d.fPortQueues[0].ingressRec : d.fPortQueues[0].qpRec)==&d.fHw.allocation);
        d.fHw.resetOk=true;
        if(ingress)d.ingressTeardown("returned",false);else d.qpTeardown("returned",false);
        CHECK(d.fHw.frees==1 && d.fHw.resets==2);
    }
    for(bool ingress : {false,true})for(unsigned fault=0;fault<9;++fault) {
        if(!ingress && fault==7)continue;
        const char *name="ready QP remeasurement revokes readiness on query/producer disagreement"; ++cases;
        Cx3Mlx4 d;d.fHw.failStep=99;
        if(ingress){d.fPortQueues[0].qpUp=true;d.fPortQueues[0].qpState=MLX4_QP_STATE_RTS;}
        UInt64 in[kR4QpInCount]{},out[16]{};Cx3R4QpResult qp{};Cx3R4IngressResult rss{};
        if(ingress)d.exportR4Ingress(&rss,out);else d.exportR4Qp(in,&qp,out);
        CHECK((ingress ? d.fPortQueues[0].ingressUp : d.fPortQueues[0].qpUp) && d.fHw.producers==1);
        d.fNetEnabled=true;
        if(fault==6)static_cast<UInt32*>(d.fHw.allocation.virt)[0]=0;
        else {d.fHw.badQuery=d.fHw.queries;d.fHw.queryFault=(!ingress && fault==5) ? 6 : fault;}
        if(ingress)d.exportR4Ingress(&rss,out);else d.exportR4Qp(in,&qp,out);
        CHECK(!(ingress ? d.fPortQueues[0].ingressUp : d.fPortQueues[0].qpUp) && !d.fNetEnabled);
        CHECK((ingress ? d.fPortQueues[0].ingressRec : d.fPortQueues[0].qpRec)==&d.fHw.allocation && d.fHw.frees==0);
        CHECK(d.fHw.producers==1 && d.fHw.modifies==3 && d.fHw.queries==5);
        CHECK((ingress ? rss.status : qp.status)==UInt32(kIOReturnNotReady));
        IONetworkInterface iface;CHECK(d.enable(&iface)==kIOReturnNotPermitted && !d.fNetEnabled);
        unsigned queries=d.fHw.queries;
        if(ingress)d.exportR4Ingress(&rss,out);else d.exportR4Qp(in,&qp,out);
        CHECK(d.fHw.queries==queries && d.fHw.allocations==1 && d.fHw.producers==1);
    }
    {
        const char *name="exhausted retention refuses network enable"; ++cases; Cx3Mlx4 d;
        d.fQuarExhausted=true; IONetworkInterface iface;
        CHECK(d.enable(&iface)==kIOReturnNotPermitted && !d.fNetEnabled);
    }
    for (bool ingress : {false,true}) for (UInt32 status : {
        0u,3u,CX3_FWSTATUS_NOT_POSTED,CX3_FWSTATUS_TIMEOUT,CX3_FWSTATUS_WEDGED,CX3_FWSTATUS_NO_DEVICE})
    for (unsigned step=0;step<3;++step) {
        const char *name=ingress ? "RSS transition failure" : "RX transition failure"; ++cases;
        Cx3Mlx4 d; d.fHw.failStep=step; d.fHw.failStatus=status;
        d.fNetEnabled=true;
        if (ingress) { d.fPortQueues[0].qpUp=true; d.fPortQueues[0].qpState=MLX4_QP_STATE_RTS; }
        UInt64 in[kR4QpInCount]{},out[16]{};
        Cx3R4QpResult qp{}; Cx3R4IngressResult rss{};
        if (ingress) d.exportR4Ingress(&rss,out); else d.exportR4Qp(in,&qp,out);
        // A completed command with FW error is not itself proof of QP ownership return.
        const bool exposed=step>0 || (status!=CX3_FWSTATUS_NOT_POSTED &&
            status!=CX3_FWSTATUS_WEDGED && status!=CX3_FWSTATUS_NO_DEVICE);
        CHECK(d.fHw.allocations==1); CHECK(d.fHw.modifies==step+1); CHECK(d.fHw.producers==0);
        CHECK(d.fHw.frees==(exposed ? 0u : 1u));
        Mlx4Icm *rec=ingress ? d.fPortQueues[0].ingressRec : d.fPortQueues[0].qpRec;
        // Readiness must remain false even when resource ownership is retained.
        CHECK(!(ingress ? d.fPortQueues[0].ingressUp : d.fPortQueues[0].qpUp));
        CHECK(!d.fNetEnabled);
        CHECK(rec==(exposed ? &d.fHw.allocation : nullptr));
        if (exposed) {
            CHECK((ingress ? d.fPortQueues[0].ingressQpn : d.fPortQueues[0].qpQpn)==(ingress ? 65u : 64u));
            CHECK((ingress ? d.fPortQueues[0].ingressState : d.fPortQueues[0].qpState)!=MLX4_QP_STATE_RTS);
            unsigned oldAlloc=d.fHw.allocations,oldModify=d.fHw.modifies;
            if (ingress) d.exportR4Ingress(&rss,out); else d.exportR4Qp(in,&qp,out);
            CHECK(d.fHw.allocations==oldAlloc && d.fHw.modifies==oldModify);
            CHECK((ingress ? rss.status : qp.status)==UInt32(kIOReturnNotReady));
            IONetworkInterface iface;
            CHECK(d.enable(&iface)==kIOReturnNotPermitted && !d.fNetEnabled);
            d.fHw.resetOk=false;
            if (ingress) d.ingressTeardown("fault test",false); else d.qpTeardown("fault test",false);
            CHECK(d.fHw.frees==0);
            CHECK((ingress ? d.fPortQueues[0].ingressRec : d.fPortQueues[0].qpRec)==&d.fHw.allocation);
            // QP -> CQN and lkey/PD dependencies cannot be revoked while QP reset failed.
            d.cqTeardown("failed QP dependency",false);
            d.mrTeardown("failed QP dependency",false);
            CHECK(d.fPortQueues[0].cqUp && d.fHw.cqReturns==0);
            CHECK(d.fMrState==1 && d.fHw.mrReturns==0);
            if (!ingress) {
                d.rqTeardown("failed QP still owns dependency",false);
                CHECK(d.fPortQueues[0].rqUp && d.fHw.mttFrees==0 && d.fHw.frees==0);
            }
            d.fHw.resetOk=true;
            if (ingress) d.ingressTeardown("return ownership",false); else d.qpTeardown("return ownership",false);
            CHECK(d.fHw.frees==1);
            CHECK(!(ingress ? d.fPortQueues[0].ingressUp : d.fPortQueues[0].qpUp));
        }
    }
    for (bool ingress : {false,true}) for (bool resetOk : {false,true}) {
        const char *name="producer mismatch rollback"; ++cases; Cx3Mlx4 d;
        d.fHw.failStep=99; d.fHw.producerOk=false; d.fHw.resetOk=resetOk;
        d.fNetEnabled=true;
        if (ingress) { d.fPortQueues[0].qpUp=true; d.fPortQueues[0].qpState=MLX4_QP_STATE_RTS; }
        UInt64 in[kR4QpInCount]{},out[16]{}; Cx3R4QpResult qp{}; Cx3R4IngressResult rss{};
        if (ingress) d.exportR4Ingress(&rss,out); else d.exportR4Qp(in,&qp,out);
        CHECK(d.fHw.resets==1); CHECK(d.fHw.frees==(resetOk ? 1u : 0u));
        CHECK(!(ingress ? d.fPortQueues[0].ingressUp : d.fPortQueues[0].qpUp));
        CHECK(!d.fNetEnabled);
        CHECK((ingress ? d.fPortQueues[0].ingressRec : d.fPortQueues[0].qpRec)==(resetOk ? nullptr : &d.fHw.allocation));
    }
    for (bool ingress : {false,true}) {
        const char *name="successful setup and ownership return"; ++cases; Cx3Mlx4 d;
        d.fHw.failStep=99;
        if (ingress) { d.fPortQueues[0].qpUp=true; d.fPortQueues[0].qpState=MLX4_QP_STATE_RTS; }
        UInt64 in[kR4QpInCount]{},out[16]{}; Cx3R4QpResult qp{}; Cx3R4IngressResult rss{};
        if (ingress) d.exportR4Ingress(&rss,out); else d.exportR4Qp(in,&qp,out);
        CHECK(d.fHw.modifies==3 && d.fHw.frees==0);
        CHECK(d.fHw.producers==1 && d.fHw.queries==4);
        CHECK((ingress ? d.fPortQueues[0].ingressState : d.fPortQueues[0].qpState)==MLX4_QP_STATE_RTS);
        CHECK((ingress ? d.fPortQueues[0].ingressRec : d.fPortQueues[0].qpRec)==&d.fHw.allocation);
        if(ingress)d.exportR4Ingress(&rss,out);else d.exportR4Qp(in,&qp,out);
        CHECK((ingress ? d.fPortQueues[0].ingressUp : d.fPortQueues[0].qpUp) && d.fHw.allocations==1);
        CHECK(d.fHw.modifies==3 && d.fHw.queries==5 && d.fHw.producers==1);
        if (ingress) d.ingressTeardown("normal",false); else d.qpTeardown("normal",false);
        CHECK(d.fHw.frees==1 && d.fHw.resets==1);
        if (ingress) d.ingressTeardown("repeat",false); else d.qpTeardown("repeat",false);
        CHECK(d.fHw.frees==1 && d.fHw.resets==1);
    }
    for (unsigned owner=0;owner<6;++owner) {
        const char *name="shared MR with unresolved QP owner"; ++cases; Cx3Mlx4 d;
        // Ready and partial RX/RSS/TX owners each keep the shared MR/PD alive.
        if (owner==0) d.fPortQueues[0].qpUp=true;
        if (owner==1) d.fPortQueues[0].qpRec=&d.fHw.allocation;
        if (owner==2) d.fPortQueues[0].ingressUp=true;
        if (owner==3) d.fPortQueues[0].ingressRec=&d.fHw.allocation;
        if (owner==4) d.fPortQueues[0].txQpUp=true;
        if (owner==5) d.fPortQueues[0].txQpRec=&d.fHw.allocation;
        d.mrTeardown("QP outstanding",false);
        CHECK(d.fMrState==1 && d.fHw.mrReturns==0);
        d.fPortQueues[0].qpUp=d.fPortQueues[0].ingressUp=d.fPortQueues[0].txQpUp=false;
        d.fPortQueues[0].qpRec=d.fPortQueues[0].ingressRec=d.fPortQueues[0].txQpRec=nullptr;
        d.mrTeardown("all QPs returned",false);
        CHECK(d.fMrState==0 && d.fHw.mrReturns==1);
        d.mrTeardown("repeat",false);
        CHECK(d.fHw.mrReturns==1);
    }
    {
        const char *name="CQ after actual RX ownership return"; ++cases; Cx3Mlx4 d;
        Mlx4Icm cqRing,cqRec; d.fPortQueues[0].cqRing=&cqRing; d.fPortQueues[0].cqRec=&cqRec;
        d.fPortQueues[0].qpRec=&d.fHw.allocation; d.fHw.resetOk=false;
        d.qpTeardown("failed return",false); d.cqTeardown("must keep",false);
        CHECK(d.fPortQueues[0].cqUp && d.fPortQueues[0].cqRing==&cqRing && d.fPortQueues[0].cqRec==&cqRec);
        CHECK(d.fHw.cqReturns==0 && d.fHw.mttFrees==0 && d.fHw.frees==0);
        d.fHw.resetOk=true;
        d.qpTeardown("successful return",false); d.cqTeardown("now safe locally",false);
        CHECK(!d.fPortQueues[0].cqUp && !d.fPortQueues[0].cqRing && !d.fPortQueues[0].cqRec);
        CHECK(d.fHw.cqReturns==1 && d.fHw.mttFrees==1 && d.fHw.frees==3);
        d.cqTeardown("repeat",false);
        CHECK(d.fHw.cqReturns==1 && d.fHw.frees==3);
    }
    {
        const char *name="CQ hand-back itself fails"; ++cases; Cx3Mlx4 d;
        Mlx4Icm ring,rec; d.fPortQueues[0].cqRing=&ring; d.fPortQueues[0].cqRec=&rec; d.fHw.cqReturnOk=false;
        d.cqTeardown("timeout",false);
        CHECK(d.fPortQueues[0].cqUp && d.fPortQueues[0].cqRing==&ring && d.fPortQueues[0].cqRec==&rec);
        CHECK(d.fHw.cqReturns==1 && d.fHw.frees==0 && d.fHw.mttFrees==0);
        d.fHw.cqReturnOk=true; d.cqTeardown("returned",false);
        CHECK(!d.fPortQueues[0].cqUp && d.fHw.frees==2 && d.fHw.mttFrees==1);
    }
    for (unsigned state : {1u,2u}) {
        const char *name="MR hand-back itself fails"; ++cases; Cx3Mlx4 d;
        d.fMrState=state; d.fHw.mrReturnOk=false;
        d.mrTeardown("timeout",false);
        CHECK(d.fMrState==2 && d.fHw.mrReturns==1 && d.fMrMptIndex==7 && d.fMrPdn==1);
        d.fHw.mrReturnOk=true; d.mrTeardown("returned",false);
        CHECK(d.fMrState==0 && d.fHw.mrReturns==2);
    }
    {
        const char *name="primary setup leaves secondary owner intact"; ++cases; Cx3Mlx4 d; d.fHw.failStep=99;
        Mlx4Icm other; d.fPortQueues[1].qpRec=&other; d.fPortQueues[1].qpQpn=512;
        d.fPortQueues[1].rawTxProd=91; d.fPortQueues[1].rawRxRecycles[0]=17;
        UInt64 in[kR4QpInCount]{},out[16]{}; Cx3R4QpResult qp{};
        d.exportR4Qp(in,&qp,out);
        CHECK(qp.status==0 && d.fPortQueues[0].qpUp && d.fPortQueues[0].qpRec);
        CHECK(d.fPortQueues[1].qpRec==&other && d.fPortQueues[1].qpQpn==512 &&
              d.fPortQueues[1].rawTxProd==91 && d.fPortQueues[1].rawRxRecycles[0]==17);
    }
    {
        const char *name="shared MR waits for secondary QP"; ++cases; Cx3Mlx4 d;
        Mlx4Icm other; d.fPortQueues[1].qpRec=&other;
        d.mrTeardown("secondary still owns",false);
        CHECK(d.fMrState==1 && d.fHw.mrReturns==0 && d.fPortQueues[1].qpRec==&other);
        d.fPortQueues[1].qpRec=nullptr; d.mrTeardown("both returned",false);
        CHECK(d.fMrState==0 && d.fHw.mrReturns==1);
    }

    for (bool ingress : {false,true}) {
        const char *name="managed secondary RX/RSS uses port 2 and its reserved namespace"; ++cases; Cx3Mlx4 d;
        auto &q=d.fPortQueues[1]; q=d.fPortQueues[0]; q.port=2; q.numbersReserved=true;
        q.numberGeneration=d.fDeviceGeneration; q.numbers={896,66,67,130,131,896,128};
        q.cqCqn=130; q.qpQpn=896; q.ingressQpn=66;
        if(ingress) {q.qpUp=true;q.qpState=MLX4_QP_STATE_RTS;}
        Mlx4Icm sentinel; d.fPortQueues[0].qpRec=&sentinel;
        d.fPortQueues[0].rawTxProd=99; d.fNetEnabled=true;d.fHw.failStep=99;
        UInt64 in[kR4QpInCount]{},out[16]{};Cx3R4QpResult qp{};Cx3R4IngressResult rss{};
        if(ingress)d.exportR4Ingress(&rss,out,2);else d.exportR4Qp(in,&qp,out,2);
        CHECK(!out[0] && (ingress ? q.ingressUp:q.qpUp));
        CHECK(d.fHw.qpn==(ingress ? 66u:896u));
        CHECK(d.fHw.context[QP_CONTEXT_PRI_SCHED_QUEUE_OFFSET]==0xc3);
        CHECK(d.fNetEnabled && d.fPortQueues[0].qpRec==&sentinel && d.fPortQueues[0].rawTxProd==99);
        if(ingress)CHECK(d.fHw.lastQueryPort==2 && d.fHw.lastMacPort==2);
        if(ingress)d.ingressTeardown("secondary",false,2);else d.qpTeardown("secondary",false,2);
        CHECK(d.fHw.lastResetQpn==(ingress ? 66u:896u));
        CHECK(d.fNetEnabled && d.fPortQueues[0].qpRec==&sentinel && !(ingress?q.ingressRec:q.qpRec));
    }
    {
        const char *name="secondary unreserved, stale and conflicting requests stop before allocation"; ++cases; Cx3Mlx4 d;
        UInt64 in[kR4QpInCount]{},out[16]{};Cx3R4QpResult qp{};
        d.exportR4Qp(in,&qp,out,2);CHECK(out[0] && !d.fHw.allocations && !d.fHw.modifies);
        auto&q=d.fPortQueues[1];q=d.fPortQueues[0];q.port=2;q.numbersReserved=true;
        q.numbers={896,66,67,130,131,896,128};q.numberGeneration=1;
        d.exportR4Qp(in,&qp,out,2);CHECK(out[0] && !d.fHw.allocations);
        q.numberGeneration=d.fDeviceGeneration;in[0]=64;
        d.exportR4Qp(in,&qp,out,2);CHECK(out[0] && !d.fHw.allocations);
        in[0]=0;d.fNetEnabled=true;d.exportR4Qp(in,&qp,out,2);
        CHECK(qp.stepsAccepted==0 && qp.stepFwStatus[0]==3 && d.fNetEnabled); // payload reports device refusal
    }
    std::printf("QP setup lifetime: %u cases, %u checks, %u failures (synthetic HCA)\n",cases,checks,failures);
    return failures ? 1 : 0;
}
'''.replace('BODIES',bodies)
    with tempfile.TemporaryDirectory(prefix='cx3-qp-setup-') as tmp:
        binary=str(Path(tmp)/'test')
        subprocess.run(['/usr/bin/clang++','-std=c++11','-Wall','-Wextra','-Werror',
                        '-Wno-unused-variable','-Wno-unused-parameter',
                        '-I',str(SRC),'-x','c++','-','-o',binary],input=harness,text=True,check=True)
        return subprocess.run([binary]).returncode


if __name__=='__main__':
    raise SystemExit(main())
