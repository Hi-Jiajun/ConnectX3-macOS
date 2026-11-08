#!/usr/bin/env python3
"""Execute production TX setup, context fill, final decode and teardown.

Catches early readiness, freed/lost device-visible records, continued transitions
after bad queries, and teardown ignoring a not-ready owner. Allocator and HCA
transport are synthetic; not a DMA, real concurrency or firmware proof.
"""
from pathlib import Path
import subprocess
import tempfile

SRC = Path(__file__).resolve().parents[1] / 'src'


def method(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


def main():
    source = (SRC / 'Cx3Mlx4.cpp').read_text()
    hw = (SRC / 'Mlx4Hw.cpp').read_text()
    bodies = '\n'.join(method(source, sig) for sig in [
        'static void r4TxQueryAndCompare(', 'static void r4TxPublishScalars(',
        'IOReturn Cx3Mlx4::exportR4Tx(', 'void Cx3Mlx4::txTeardown(',
        'IOReturn Cx3Mlx4::enable('])
    fill = method(hw, 'void Mlx4Hw::qpFillContext(')
    harness = r'''
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
 kIOReturnNoSpace=-6,kIOReturnNotPermitted=-7,kIOReturnExclusiveAccess=-8;
static void logSink(const char*,...) {}
#define LOG(...) logSink(__VA_ARGS__)
static UInt32 mlx4Log2(UInt32 n) { return 31-__builtin_clz(n); }
static UInt32 cx3GetBe32(const void *p) {
 const auto *b=static_cast<const UInt8*>(p);
 return UInt32(b[0])<<24|UInt32(b[1])<<16|UInt32(b[2])<<8|b[3];
}
static UInt64 cx3GetBe64(const void *p) {
 return UInt64(cx3GetBe32(p))<<32|cx3GetBe32(static_cast<const UInt8*>(p)+4);
}
static void mlx4PutBe16(void *p,UInt16 n) { auto *b=static_cast<UInt8*>(p); b[0]=n>>8;b[1]=n; }
static void mlx4PutBe32(void *p,UInt32 n) { auto *b=static_cast<UInt8*>(p); for(unsigned i=0;i<4;++i)b[i]=n>>(24-i*8); }
static void mlx4PutBe64(void *p,UInt64 n) { auto *b=static_cast<UInt8*>(p); for(unsigned i=0;i<8;++i)b[i]=n>>(56-i*8); }
struct Page { UInt64 busAddr=0x1234000; };
struct Mlx4Icm { UInt8 buffer[4096]{}; void *virt=buffer; UInt64 bytes=4096; Page page; Page *pages=&page; };
struct Mlx4Hw {
 Mlx4Icm ring,cqRec,sq,qpRec;
 unsigned allocations=0,frees=0,modifies=0,queries=0,resets=0,cqReturns=0,mttFrees=0,lastQueryPort=0,lastQpn=0,lastCqn=0,lastResetQpn=0;
 unsigned failStep=99,badQuery=99,queryFault=0,postFailureCalls=0;
 UInt32 failStatus=3; bool resetOk=true,wedged=false,earlyReady=false,mttFreeOk=true;
 const bool *ready=nullptr; UInt8 context[QP_CONTEXT_SIZE]{};
 void observe() { if (ready && *ready) earlyReady=true; if(wedged)++postFailureCalls; }
 Mlx4Icm *icmAlloc(unsigned,bool) { ++allocations;return &qpRec; }
 void icmFree(Mlx4Icm*) { ++frees; }
 bool markDmaHandoff(Mlx4Icm *const*,UInt32) { return true; }
 bool cmdWedged() const { return wedged; }
 bool queryPort(unsigned port,Cx3QueryPortResult *r) { lastQueryPort=port; observe();memset(r,0,sizeof(*r)); r->logMaxMacs=7;return true; }
 bool cqCreate(UInt32 cqn,UInt32,UInt8,UInt32,Mlx4Icm **r,UInt32 *pg,UInt32 *off,UInt32 *addr,
               UInt64 *phys,Mlx4Icm **rec,UInt32,UInt32,UInt32 *s) {
  lastCqn=cqn;observe();*r=&ring;*rec=&cqRec;*pg=1;*off=8;*addr=64;*phys=0x12000;*s=0;return true;
 }
 bool cqNextCqe(Mlx4Icm*,UInt32,UInt32,UInt32,UInt8*) { return false; }
 bool sqCreate(UInt32 n,Mlx4Icm **r,UInt32 *pg,UInt32 *off,UInt32 *addr,UInt64 *phys,
               UInt32,UInt32 *stamp,UInt32 *stamped,UInt32 *nonzero) {
  observe();*r=&sq;*pg=4;*off=16;*addr=128;*phys=0x13000;
  *stamp=0xffffffffu;*stamped=n;*nonzero=0;return true;
 }
 void qpFillContext(UInt8*,UInt32,UInt32,UInt32,UInt32,UInt32,UInt32,UInt64,
                    UInt32,UInt32,bool,UInt32,bool,bool,int);
 bool qpModify(UInt16,UInt32 qpn,const UInt8 *p,UInt32 *s) {
  lastQpn=qpn;observe();memcpy(context,p,sizeof(context));
  if(modifies++==failStep){*s=failStatus;wedged=failStatus==CX3_FWSTATUS_TIMEOUT;return false;}
  *s=0;return true;
 }
 bool qpQuery(UInt32,UInt8 *p,UInt32 n,UInt32 *s) {
  observe();memcpy(p,context,n);unsigned call=queries++;*s=0;
  if(call!=badQuery)return true;
  if(queryFault==0){*s=CX3_FWSTATUS_TIMEOUT;wedged=true;return false;}
  if(queryFault==1)p[0]^=0x10;
  if(queryFault==2)p[QP_CONTEXT_LOCAL_QPN_OFFSET+3]^=1;
  if(queryFault==3)p[QP_CONTEXT_MTT_BASE_L_OFFSET+3]^=8;
  if(queryFault==4)p[QP_CONTEXT_CQN_SEND_OFFSET+3]^=1;
  return true;
 }
 bool qpToReset(UInt32 qpn,UInt32 *s) { lastResetQpn=qpn;++resets;*s=resetOk ? 0 : CX3_FWSTATUS_TIMEOUT;return resetOk; }
 bool cqHw2Sw(UInt32,UInt32 *s) { ++cqReturns;*s=0;return true; }
 bool mttFree(UInt32,UInt32,Mlx4Icm*) { ++mttFrees; return mttFreeOk; }
};
FILL
struct IONetworkInterface {};
struct Timer { void wakeAtTime(UInt64) {} void cancelTimeout() {} };
constexpr UInt64 kNetTickNs=1000000;
static UInt64 cx3WakeInTicks(UInt64 n) { return n; }
class Cx3Mlx4 {
public:
    UInt64 fDeviceGeneration=2;
    Cx3AutoNetworkResult fAutoNetwork{}; bool fAutoNetworkUserDisabled=false;
    Cx3PortQueues fPortQueues[2] = { []() { Cx3PortQueues q{}; q.init(1); q.cqUp = true; q.qpUp = true; q.ingressUp = true; q.qpRec = nullptr; q.ingressRec = nullptr; q.qpState = MLX4_QP_STATE_RTS; q.ingressState = MLX4_QP_STATE_RTS; q.qpQpn = 64; q.ingressQpn = 65; q.cqCqn = 128; q.cqUarStart = 1; q.txCqUp = false; q.txCqHwReturned = false; q.txQpUp = false; q.txQpReturned = false; q.txSqUnknownRetained = false; q.txQpn = 0; q.txQpState = 0; q.txCqNent = 0; q.txCqn = 0; q.txCqMttOff = 0; q.txCqMttPages = 0; q.txSqNent = 0; q.txSqPages = 0; q.txSqMttOff = 0; q.txSqMttPages = 0; q.rawTxProd = 0; q.rawTxCons = 0; q.txSqMttAddr = 0; q.txSqPhys = 0; q.txQpRecPhys = 0; q.rawFramePhys = 0; q.rawFrameBytes = 0; q.txQpRec = nullptr; q.txCqRing = nullptr; q.txCqRec = nullptr; q.txSq = nullptr; q.rawFrame = nullptr; return q; }(), []() { Cx3PortQueues q{}; q.init(2); return q; }() };

public:
 Mlx4Hw fHw;
 struct { UInt32 reserved_qps=64,reserved_cqs=64; UInt64 flags=0; } fDevCap;
 struct { UInt32 log_num_qps=10,log_num_cqs=10; } fInitHca;
 bool fHcaInited=true;
 
 
 UInt32 fMrState=1,fMrPdn=1,fEqn=9;
 
 
 
 
 
 
 
 Mlx4TxAdmissionGate fNetTxAdmission{};
 bool fResetCommandsBlocked=false,fStopRetained=false;
 bool fNetEnabled=false,fQuarExhausted=false; Timer *fNetTimer=nullptr;UInt32 fNetUnit=2;
 bool netAttachPoisoned() { return false; }
 void publishLinkStatus(const char*) {}
 bool releaseIcmOrKeep(Mlx4Icm **p,const char*,bool keep) {
  if(*p && !keep)fHw.icmFree(*p);*p=nullptr;return true;
 }
 IOReturn exportR4Tx(const UInt64*,Cx3R4TxResult*,UInt64*,UInt32 port=1);
 void txTeardown(const char*,bool,UInt32 port=1);
 IOReturn enable(IONetworkInterface*);
};
BODIES
static unsigned cases=0,checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){++failures;std::printf("FAIL %s line %u: %s\n",name,__LINE__,#x);}}while(0)
static void setup(Cx3Mlx4 &d,Cx3R4TxResult &r,UInt64 *out) {
 UInt64 in[kR4TxInCount]{}; d.fHw.ready=&d.fPortQueues[0].txQpUp;d.exportR4Tx(in,&r,out);
}
int main() {
 IONetworkInterface iface;
 for(unsigned step=0;step<3;++step)for(UInt32 status : {3u,CX3_FWSTATUS_TIMEOUT,CX3_FWSTATUS_NOT_POSTED,4u,5u,0u}) {
  const char *name="transition failure retains owner, blocks readiness and rebuild";++cases;
  Cx3Mlx4 d;Cx3R4TxResult r;UInt64 out[kR4TxOutCount];d.fHw.failStep=step;d.fHw.failStatus=status;
  setup(d,r,out);
  CHECK(!d.fPortQueues[0].txQpUp && !d.fHw.earlyReady);
  CHECK(d.fPortQueues[0].txQpRec==&d.fHw.qpRec && d.fPortQueues[0].txQpRecPhys==0x1234000);
  CHECK(d.fHw.frees==0 && d.fHw.modifies==step+1 && d.fHw.queries==step);
  CHECK(d.fHw.postFailureCalls==0);
  unsigned allocations=d.fHw.allocations;setup(d,r,out);
  CHECK(d.fHw.allocations==allocations && !d.fPortQueues[0].txQpUp);
  CHECK(d.enable(&iface)==kIOReturnNotPermitted && !d.fNetEnabled);
  d.fHw.resetOk=false;d.txTeardown("unreturned",false);
  CHECK(d.fHw.resets==1 && d.fHw.frees==0 && d.fHw.mttFrees==0 && d.fHw.cqReturns==0);
  CHECK(d.fPortQueues[0].txQpRec==&d.fHw.qpRec && d.fPortQueues[0].txSq && d.fPortQueues[0].txCqRing);
  d.fHw.resetOk=true;d.txTeardown("returned",false);
  CHECK(d.fHw.resets==2 && d.fHw.frees==4 && d.fHw.mttFrees==2 && d.fHw.cqReturns==1);
  CHECK(!d.fPortQueues[0].txQpRec && !d.fPortQueues[0].txSq && !d.fPortQueues[0].txCqUp && !d.fPortQueues[0].txQpUp);
  d.txTeardown("repeat",false);CHECK(d.fHw.resets==2 && d.fHw.frees==4);
 }
 for(unsigned query=0;query<4;++query)for(unsigned fault=0;fault<(query==3 ? 5u : 3u);++fault) {
  const char *name="bad query stops transitions and never enables TX";++cases;
  Cx3Mlx4 d;Cx3R4TxResult r;UInt64 out[kR4TxOutCount];d.fHw.badQuery=query;d.fHw.queryFault=fault;
  setup(d,r,out);
  CHECK(!d.fPortQueues[0].txQpUp && !d.fHw.earlyReady);
  CHECK(d.fPortQueues[0].txQpRec==&d.fHw.qpRec && d.fHw.frees==0);
  CHECK(d.fHw.modifies==(query==3 ? 3u : query+1) && d.fHw.queries==query+1);
  CHECK(d.fHw.postFailureCalls==0);
  CHECK(d.enable(&iface)==kIOReturnNotPermitted && !d.fNetEnabled);
  if(query<3)CHECK(r.statesConfirmed==query);
  d.txTeardown("returned",false);
  CHECK(d.fHw.resets==1 && d.fHw.frees==4 && d.fHw.mttFrees==2);
 }
 {
  const char *name="successful setup marks ready only after all queries";++cases;
  Cx3Mlx4 d;Cx3R4TxResult r;UInt64 out[kR4TxOutCount];setup(d,r,out);
  CHECK(!d.fHw.earlyReady && d.fPortQueues[0].txQpUp && d.fPortQueues[0].txQpRec==&d.fHw.qpRec);
  CHECK(r.statesConfirmed==3 && r.queryOk && r.queryAllFieldsMatch && r.queryMttMatches);
  CHECK(d.fHw.modifies==3 && d.fHw.queries==4 && d.fHw.frees==0);
  CHECK(d.enable(&iface)==kIOReturnSuccess && d.fNetEnabled);
  d.txTeardown("returned",false);CHECK(d.fHw.resets==1 && d.fHw.frees==4);
 }
 {
  const char *name="ready QP loses readiness even when teardown cannot return it";++cases;
  Cx3Mlx4 d;Cx3R4TxResult r;UInt64 out[kR4TxOutCount];setup(d,r,out);
  d.fHw.resetOk=false;d.txTeardown("failed return",false);
  CHECK(!d.fPortQueues[0].txQpUp && d.fPortQueues[0].txQpRec==&d.fHw.qpRec);
  CHECK(d.fHw.resets==1 && d.fHw.frees==0 && d.fHw.cqReturns==0 && d.fHw.mttFrees==0);
  CHECK(d.enable(&iface)==kIOReturnNotPermitted && !d.fNetEnabled);
  d.txTeardown("repeat failed return",false);
  CHECK(d.fHw.resets==2 && d.fHw.frees==0 && d.fPortQueues[0].txQpRec==&d.fHw.qpRec);
  d.fHw.resetOk=true;d.txTeardown("finally returned",false);
  CHECK(d.fHw.resets==3 && d.fHw.frees==4 && !d.fPortQueues[0].txQpRec);
 }
 {
  const char *name="record alone blocks rebuild even without CQ flag";++cases;
  Cx3Mlx4 d;d.fPortQueues[0].txQpRec=&d.fHw.qpRec;Cx3R4TxResult r;UInt64 out[kR4TxOutCount];setup(d,r,out);
  CHECK(d.fHw.allocations==0 && d.fHw.modifies==0 && !d.fPortQueues[0].txQpUp);
 }

 {
  const char *name="secondary TX creates and destroys only its reserved objects";++cases;Cx3Mlx4 d;
  auto&q=d.fPortQueues[1];q=d.fPortQueues[0];q.port=2;q.numbersReserved=true;q.numberGeneration=d.fDeviceGeneration;
  q.numbers={896,66,67,130,131,896,128};q.qpQpn=896;q.ingressQpn=66;q.cqCqn=130;
  Mlx4Icm sentinel;d.fPortQueues[0].txQpRec=&sentinel;d.fPortQueues[0].rawTxProd=71;d.fNetEnabled=true;
  UInt64 in[kR4TxInCount]{},out[kR4TxOutCount]{};Cx3R4TxResult r{};
  d.exportR4Tx(in,&r,out,2);
  CHECK(!out[0] && q.txQpUp && q.txQpn==67 && q.txCqn==131);
  CHECK(d.fHw.lastQueryPort==2 && d.fHw.lastQpn==67 && d.fHw.lastCqn==131);
  CHECK(d.fHw.context[QP_CONTEXT_PRI_SCHED_QUEUE_OFFSET]==0xc3);
  CHECK(d.fNetEnabled && d.fPortQueues[0].txQpRec==&sentinel && d.fPortQueues[0].rawTxProd==71);
  d.txTeardown("secondary",false,2);
  CHECK(d.fHw.lastResetQpn==67 && d.fHw.frees==4 && !q.txQpRec && !q.txSq);
  CHECK(d.fNetEnabled && d.fPortQueues[0].txQpRec==&sentinel && d.fPortQueues[0].rawTxProd==71);
 }
 std::printf("TX setup lifetime: %u cases, %u checks, %u failures (synthetic HCA)\n",cases,checks,failures);
 return failures ? 1 : 0;
}
'''.replace('FILL', fill).replace('BODIES', bodies)
    with tempfile.TemporaryDirectory(prefix='cx3-tx-setup-') as tmp:
        binary = str(Path(tmp) / 'test')
        subprocess.run(['/usr/bin/clang++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-variable', '-Wno-unused-parameter',
                        '-I', str(SRC), '-x', 'c++', '-', '-o', binary],
                       input=harness, text=True, check=True)
        return subprocess.run([binary]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
