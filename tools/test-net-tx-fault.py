#!/usr/bin/env python3
"""Execute production TX post/reclaim against synthetic CQEs, never hardware."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--provider', type=Path, default=ROOT/'src/Cx3Mlx4.cpp')
    parser.add_argument('--fault-log', type=Path, help='save synthetic production log output for independent decoding')
    args = parser.parse_args()
    provider = args.provider.read_text()
    header = (ROOT/'src/Cx3Mlx4.h').read_text()
    fault_start = header.index('struct NetTxFault {')
    fault_decl = header[fault_start:header.index('} fNetTxFault;', fault_start)+len('} fNetTxFault;')]
    bodies = '\n'.join(method(provider, sig) for sig in
                       ('static inline UInt32 cx3GetBe32(', 'static inline UInt16 cx3GetBe16(',
                        'void Cx3Mlx4::netServiceTx()', 'UInt32 Cx3Mlx4::outputPacket(',
                        'IOReturn Cx3Mlx4::enable(', 'IOReturn Cx3Mlx4::disable('))
    source = r'''
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <libkern/OSByteOrder.h>
#include <vector>
#include <array>
#define CX3_HOST_TEST
#include "mlx4_pure.h"
#include "Cx3PortQueues.h"
struct Packet {unsigned frees=0;UInt8 data[64]={};};
using mbuf_t=Packet*;
static size_t mbuf_pkthdr_len(mbuf_t){return 64;}
static size_t mbuf_len(mbuf_t){return 64;}
static int copies=0;
static int mbuf_copydata(mbuf_t p,unsigned,unsigned n,void*v){++copies;memcpy(v,p->data,n);return 0;}
struct IOLock{bool held=false;};
static bool IOLockTryLock(IOLock*p){if(p->held)return false;p->held=true;return true;}
static void IOLockUnlock(IOLock*p){p->held=false;}
static std::vector<std::string> logs;
static void logSink(const char*f,...){char b[2048];va_list a;va_start(a,f);vsnprintf(b,sizeof(b),f,a);va_end(a);logs.emplace_back(b);}
#define LOG(...) logSink(__VA_ARGS__)
enum {kIOReturnOutputSuccess=0,kIOReturnOutputDropped=1,kIOReturnOutputStall=2,
 kChecksumFamilyTCPIP=1,kChecksumIP=1,kChecksumTCP=2,kChecksumUDP=4,kChecksumTCPIPv6=8,kChecksumUDPIPv6=16};
using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnNotPermitted=-2;
constexpr unsigned kIONetworkLinkValid=1;
constexpr UInt64 kNetTickNs=1000000;
static UInt64 cx3WakeInTicks(UInt64 n){return n;}
struct IONetworkInterface{};
struct Timer{bool armed=true;void wakeAtTime(UInt64){armed=true;}void cancelTimeout(){armed=false;}};
struct Mlx4Icm{void *virt=nullptr;UInt64 bytes=256*MLX4_TXBB_SIZE+SQ_WQE0_OFFSET;};
struct Hardware {
 bool cmdWedged(){return false;}
 std::vector<std::array<UInt8,CQE_LEGACY_SIZE>> cq;
 unsigned next=0,writes=0,bells=0,stamps=0,ci=0;
 bool cqNextCqe(Mlx4Icm*,UInt32,UInt8,UInt32,UInt8*out){if(next==cq.size())return false;memcpy(out,cq[next++].data(),CQE_LEGACY_SIZE);return true;}
 void cqSetCi(volatile UInt32*,UInt32 v){ci=v;}
 UInt64 rqBufferPhys(Mlx4Icm*,UInt32,UInt32){return 0x1000;}
 void sqWriteSendWqe(UInt8*sq,UInt32 slot,UInt32,UInt32,UInt32,UInt32,UInt64,UInt32,UInt32,void*,bool,UInt32){++writes;memset(sq+MLX4_SQ_WQE_OFFSET(slot),0x77,MLX4_TXBB_SIZE);}
 void sqRingSendDoorbell(UInt32,UInt32,bool){++bells;}
 void sqStampTxbb(UInt8*sq,UInt32 slot,UInt32){++stamps;memset(sq+MLX4_SQ_WQE_OFFSET(slot),0xcc,MLX4_TXBB_SIZE);}
 void completion(unsigned idx,unsigned syndrome=0){std::array<UInt8,CQE_LEGACY_SIZE> c{};
  c[CQE_OWNER_SR_OPCODE_OFFSET]=MLX4_CQE_IS_SEND_MASK|(syndrome?MLX4_CQE_OPCODE_ERROR:MLX4_OPCODE_SEND);
  c[CQE_WQE_INDEX_OFFSET]=UInt8(idx>>8);c[CQE_WQE_INDEX_OFFSET+1]=UInt8(idx);
  c[CQE_ERR_SYNDROME_OFFSET]=UInt8(syndrome);c[CQE_ERR_VENDOR_SYNDROME_OFFSET]=syndrome==2?0x6d:0xf9;
  c[3]=0x29;cq.push_back(c);}
};
class Cx3Mlx4 {
public:
    Cx3AutoNetworkResult fAutoNetwork{}; bool fAutoNetworkUserDisabled=false;
    Cx3PortQueues fPortQueues[2] = { [this]() { Cx3PortQueues q{}; q.init(1); q.txSq = &sqRing; q.txCqRing = &cqRing; q.txCqRec = &rec; q.txQpUp = true; q.txSqNent = 256; q.txCqNent = 256; q.rawTxCons = 0; q.rawTxProd = 0; q.txQpn = 0x29; q.cqUarStart = 0; q.qpUp = true; q.ingressUp = true; q.txQpRec = nullptr; q.qpRec = nullptr; q.ingressRec = nullptr; return q; }(), []() { Cx3PortQueues q{}; q.init(2); return q; }() };

 public:
 enum{kNetTxSlots=256,kNetTxPerTick=256,kNetTxMinFrame=14};
 Hardware fHw;Mlx4TxAdmissionGate fNetTxAdmission;IOLock lock;IOLock*fNetTxLock=&lock;
 UInt8 frames[256*128]={},sq[256*MLX4_TXBB_SIZE+SQ_WQE0_OFFSET]={};UInt32 record=0;
 Mlx4Icm frameRing{frames},sqRing{sq},cqRing{nullptr},rec{&record};
 Mlx4Icm *fNetTxRing=&frameRing;
 bool fNetEnabled=true,fNetTxCsum=false;
 UInt32 fNetTxSlotCount=256,fNetTxBufBytes=128;
 UInt32 fMrLkey=0x10000;
 UInt64 fNetTxLockBusy=0,fNetTxCompleted=0,fNetTxErrors=0,fNetTxFrames=0,fNetTxDropped=0;
 UInt64 fNetTxStalled=0,fNetTxCsumGood=0,fNetTxCsumNone=0;
 FAULT_DECLARATION
 UInt32 fNetTxFlushErrors=0;
 mbuf_t fNetTxMbuf[kNetTxSlots]={};UInt8 fNetTxOwner[kNetTxSlots]={};
 bool fResetCommandsBlocked=false,fStopRetained=false,fQuarExhausted=false;
 
 Timer timer;Timer *fNetTimer=&timer;IONetworkInterface iface;IONetworkInterface*fNetIface=&iface;UInt32 fNetUnit=2;
 bool netAttachPoisoned(){return false;}
 void publishLinkStatus(const char*){}
 void setLinkStatus(unsigned,int,UInt64){}
 IOReturn enable(IONetworkInterface*);IOReturn disable(IONetworkInterface*);
 Cx3Mlx4(){fNetTxAdmission.init();memset(&fNetTxFault,0,sizeof(fNetTxFault));}
 void freePacket(mbuf_t p){++p->frees;}
 void getChecksumDemand(mbuf_t,UInt32,UInt32*){}
 void netServiceTx();UInt32 outputPacket(mbuf_t,void*);
};
BODIES
static unsigned checks=0,failures=0,cases=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL %s line %u: %s\n",name,__LINE__,#x);}}while(0)
int main(){
 {const char*name="healthy traffic posts and reclaims";++cases;Cx3Mlx4 d;Packet a,b;
  CHECK(d.outputPacket(&a,nullptr)==kIOReturnOutputSuccess);CHECK(a.frees==0 && d.fHw.bells==1);
  d.fHw.completion(0);d.netServiceTx();CHECK(a.frees==1 && d.fNetTxMbuf[0]==nullptr);
  CHECK(d.outputPacket(&b,nullptr)==kIOReturnOutputSuccess && d.fHw.bells==2);}
 for(unsigned syndrome: {2u,5u}){const char*name="error completion stops new submissions but drains outstanding";++cases;
  Cx3Mlx4 d;Packet a,b,c;logs.clear();copies=0;
  CHECK(d.outputPacket(&a,nullptr)==kIOReturnOutputSuccess);
  CHECK(d.outputPacket(&b,nullptr)==kIOReturnOutputSuccess);
  d.fHw.completion(0,syndrome);d.netServiceTx();
  CHECK(a.frees==1 && b.frees==0 && d.fNetTxMbuf[1]==&b);
  unsigned bells=d.fHw.bells,writes=d.fHw.writes;UInt32 prod=d.fPortQueues[0].rawTxProd;int oldCopies=copies;
  CHECK(d.outputPacket(&c,nullptr)==kIOReturnOutputDropped);
  CHECK(c.frees==1 && d.fHw.bells==bells && d.fHw.writes==writes && copies==oldCopies);
  CHECK(d.fPortQueues[0].rawTxProd==prod && d.fNetTxAdmission.drained() && !d.lock.held);
  d.fHw.completion(1,5);d.netServiceTx();CHECK(b.frees==1 && d.fNetTxMbuf[1]==nullptr);
  CHECK(d.fNetTxCompleted==2 && d.fNetTxErrors==2);
  CHECK(d.fNetTxFlushErrors==(syndrome==5?2u:1u));
 }
 {const char*name="a successful CQE after the fault still reclaims its posted packet";++cases;
  Cx3Mlx4 d;Packet a,b,c;d.outputPacket(&a,nullptr);d.outputPacket(&b,nullptr);
  d.fHw.completion(0,2);d.netServiceTx();auto saved=d.fNetTxFault;
  d.fHw.completion(1);d.netServiceTx();
  CHECK(a.frees==1 && b.frees==1 && d.fNetTxMbuf[1]==nullptr && d.fHw.stamps==2);
  CHECK(d.fNetTxCompleted==2 && d.fNetTxErrors==1 && d.fNetTxFlushErrors==0);
  CHECK(memcmp(&saved,&d.fNetTxFault,sizeof(saved))==0 && d.fNetTxFault.latched);
  CHECK(d.outputPacket(&c,nullptr)==kIOReturnOutputDropped && c.frees==1 && d.fHw.bells==2);
  CHECK(d.fPortQueues[0].rawTxProd==2 && d.fNetTxAdmission.drained() && !d.lock.held);
 }
 {const char*name="first CQE and pre-stamp WQE survive later completions";++cases;Cx3Mlx4 d;Packet a,b;
  d.outputPacket(&a,nullptr);d.outputPacket(&b,nullptr);
  std::array<UInt8,MLX4_TXBB_SIZE> wqe;memcpy(wqe.data(),d.sq+MLX4_SQ_WQE_OFFSET(0),wqe.size());
  d.fHw.completion(0,2);auto cqe=d.fHw.cq.back();d.netServiceTx();
  CHECK(d.fNetTxFault.latched && d.fNetTxFault.wqeValid && d.fNetTxFault.hadMbuf);
  CHECK(d.fNetTxFault.consumer==0 && d.fNetTxFault.producer==2 && d.fNetTxFault.slot==0);
  CHECK(d.fNetTxFault.qpn==d.fPortQueues[0].txQpn && d.fNetTxFault.owner==0);
  CHECK(memcmp(d.fNetTxFault.cqe,cqe.data(),cqe.size())==0);
  CHECK(memcmp(d.fNetTxFault.wqe,wqe.data(),wqe.size())==0);
  CHECK(d.sq[MLX4_SQ_WQE_OFFSET(0)]==0xcc);
  auto saved=d.fNetTxFault;d.fHw.completion(1,5);d.netServiceTx();
  CHECK(memcmp(&saved,&d.fNetTxFault,sizeof(saved))==0 && a.frees==1 && b.frees==1);
 }
 for(UInt32 prod:{65535u,65536u,197208u}){const char*name="absolute WQE counters map to slot and posted owner";++cases;
  Cx3Mlx4 d;Packet a;d.fPortQueues[0].rawTxProd=prod;d.fPortQueues[0].rawTxCons=prod;
  unsigned slot=prod&255u;d.outputPacket(&a,nullptr);d.fHw.completion(prod&65535u,2);d.netServiceTx();
  CHECK(a.frees==1 && d.fNetTxFault.slot==slot && d.fNetTxFault.consumer==prod);
  CHECK(d.fNetTxFault.owner==((prod&256u)?1u:0u) && d.fNetTxFault.producer==prod+1);
 }
 {const char*name="invalid CQE slot does not read WQE or free an unrelated record";++cases;
  Cx3Mlx4 d;Packet a;d.fPortQueues[0].txSqNent=512;d.fNetTxMbuf[0]=&a;
  d.fHw.completion(300,2);d.netServiceTx();
  CHECK(d.fNetTxFault.latched && !d.fNetTxFault.wqeValid && !d.fNetTxFault.hadMbuf);
  CHECK(a.frees==0 && d.fHw.stamps==0);
 }
 {const char*name="undersized SQ is not read for the fault snapshot";++cases;Cx3Mlx4 d;
  d.sqRing.bytes=MLX4_TXBB_SIZE;d.fHw.completion(2,2);d.netServiceTx();
  CHECK(d.fNetTxFault.latched && !d.fNetTxFault.wqeValid);
 }
 {const char*name="lock contention preserves packet and balances callback admission";++cases;
  Cx3Mlx4 d;Packet a;d.lock.held=true;d.fHw.completion(0,2);d.netServiceTx();
  CHECK(d.fHw.next==0 && !d.fNetTxFault.latched);
  CHECK(d.outputPacket(&a,nullptr)==kIOReturnOutputStall && a.frees==0 && d.fNetTxAdmission.drained());
  d.lock.held=false;d.netServiceTx();CHECK(d.fNetTxFault.latched);
  CHECK(d.outputPacket(&a,nullptr)==kIOReturnOutputDropped && a.frees==1 && d.fNetTxAdmission.drained());
 }
 {const char*name="error log flood is bounded";++cases;Cx3Mlx4 d;Packet a;logs.clear();
  d.outputPacket(&a,nullptr);logs.clear();
  for(unsigned i=0;i<40;++i)d.fHw.completion(0,i?5:2);
  d.netServiceTx();CHECK(a.frees==1 && d.fNetTxCompleted==40 && d.fNetTxErrors==40);
  CHECK(logs.size()<=10);}
 {const char*name="interface down/up keeps the fault and still drains posted completions";++cases;
  Cx3Mlx4 d;Packet a,b,c;d.outputPacket(&a,nullptr);d.outputPacket(&b,nullptr);
  d.fHw.completion(0,2);d.netServiceTx();auto saved=d.fNetTxFault;
  CHECK(d.disable(&d.iface)==kIOReturnSuccess && !d.fNetEnabled && !d.timer.armed);
  CHECK(d.enable(&d.iface)==kIOReturnSuccess && d.fNetEnabled && d.timer.armed);
  CHECK(d.outputPacket(&c,nullptr)==kIOReturnOutputDropped && c.frees==1 && d.fHw.bells==2);
  d.fHw.completion(1,5);d.netServiceTx();
  CHECK(a.frees==1 && b.frees==1 && memcmp(&saved,&d.fNetTxFault,sizeof(saved))==0);
 }
 // Emit the production logger's actual format; payload is synthetic, never a device read.
 {Cx3Mlx4 d;Packet a;d.fPortQueues[0].rawTxProd=197208;d.fPortQueues[0].rawTxCons=197208;
  d.outputPacket(&a,nullptr);unsigned slot=197208u&255u;
  unsigned char image[64]={0x00,0x02,0x58,0x0a,0,0,0,2,0,0,0,0x0e,0,0,0,0,
    0,0,0,64,0,1,0,0,0,0,0,1,0x23,0x45,0x60,0};
  memcpy(d.sq+MLX4_SQ_WQE_OFFSET(slot),image,64);
  logs.clear();d.fHw.completion(600,2);d.netServiceTx();
  for(const auto &line:logs) printf("FIXTURE %s\n",line.c_str());}
 printf("TX fault: %u cases, %u checks, %u failures (real methods, synthetic hardware)\n",cases,checks,failures);
 return failures?1:0;
}
'''.replace('FAULT_DECLARATION', fault_decl).replace('BODIES', bodies).replace('#include <vector>', '#include <vector>\n#include <string>')
    with tempfile.TemporaryDirectory(prefix='cx3-net-tx-fault-') as temporary:
        exe = str(Path(temporary)/'test')
        subprocess.run(['/usr/bin/clang++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                        '-I',str(ROOT/'src'),'-x','c++','-','-o',exe],
                       input=source,text=True,check=True,timeout=60)
        result = subprocess.run([exe],text=True,capture_output=True,timeout=20)
        print(result.stdout,end='')
        print(result.stderr,end='')
        if args.fault_log and result.returncode == 0:
            args.fault_log.write_text('\n'.join(line for line in result.stdout.splitlines()
                                              if line.startswith('FIXTURE '))+'\n')
        return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
