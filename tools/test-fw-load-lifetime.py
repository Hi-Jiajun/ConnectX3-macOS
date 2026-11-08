#!/usr/bin/env python3
"""Production FW load -> UNMAP helper and CLI over synthetic command/allocator seams.

Checks refused handback never loses/frees the previous FW allocation; live or
retained dependencies reject before any hardware read/command, even with force.
First-load controls and actual CLI error exit are exercised. No device, real
DMA lifetime, concurrency or IOKit execution is represented by these host seams.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start)+3]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path)
    parser.add_argument('--hw-source', type=Path)
    parser.add_argument('--cli-source', type=Path)
    args = parser.parse_args()
    source = (args.source or ROOT/'src/Cx3Mlx4.cpp').read_text()
    hw = (args.hw_source or ROOT/'src/Mlx4Hw.cpp').read_text()
    cli = (args.cli_source or ROOT/'tools/cx3ctl.c').read_text()
    methods = '\n'.join((method(source, 'bool Cx3Mlx4::fwReplacementAllowed(')
                         if 'bool Cx3Mlx4::fwReplacementAllowed(' in source else '',
                         method(source, 'IOReturn Cx3Mlx4::exportIcmAlloc('),
                         method(source, 'IOReturn Cx3Mlx4::exportFwLoad('),
                         method(hw, 'bool Mlx4Hw::unmapFa(')))
    client = method(cli, 'static int cmdFwLoad(') + method(cli, 'static int cmdIcmAlloc(')
    code = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
using UInt8=uint8_t;using UInt16=uint16_t;using UInt32=uint32_t;using UInt64=uint64_t;
using IOReturn=int;
#define CX3_HOST_TEST
#include "mlx4_pure.h"
#include "cx3_abi.h"
#include "Cx3PortQueues.h"
#include "mlx4_regs.h"
constexpr IOReturn kIOReturnSuccess=0,kIOReturnError=-1,kIOReturnNotReady=-2,
                  kIOReturnBadArgument=-3,kIOReturnOffline=-4,kIOReturnNoMemory=-5;
static void sink(const char*,...){}
#define LOG(...) sink(__VA_ARGS__)
static unsigned lockDepth=0;
void IOLockLock(void*){++lockDepth;}
void IOLockUnlock(void*){--lockDepth;}
struct Mlx4IcmPage { UInt64 busAddr=0x100000; };
struct Mlx4Icm {
  UInt32 npages=2,pagesAbove4G=0,nseg=1;bool contiguous=true;
  UInt64 bytes=8192,busLow=0x100000,busHigh=0x101000;
  Mlx4IcmPage backing[2];Mlx4IcmPage *pages=backing;
  UInt32 data[4]={0x11223344,0x55667788,0x99aabbcc,0xaabbccdd};
  Mlx4DmaGeneration generation;
  Mlx4Icm(){backing[1].busAddr=0x101000;generation.init(7);}
};
class Mlx4Hw {
public:
  Mlx4Icm fresh;
  bool window=true,allocOK=true,unmapOK=true,mapOK=true,runOK=true,modOK=true,wedged=false,ab=false;
  UInt32 unmapStatus=0,mapStatus=0,runStatus=0,modStatus=0;
  unsigned reads=0,allocs=0,unmaps=0,frees=0,maps=0,runs=0,mods=0,fills=0;
  Mlx4Icm *freed=nullptr,*filled=nullptr;
  bool windowAlive(){++reads;return window;}
  bool cmdWedged()const{return wedged;}
  bool hasAbRetainedRings()const{return ab;}
  bool cmdBox(UInt64 a,UInt64 b,UInt32 c,UInt8 d,UInt16 op,UInt32 timeout,UInt32*s){
    if(a || b || c || d || op!=MLX4_CMD_UNMAP_FA || !timeout)return false;
    ++unmaps;*s=unmapStatus;return unmapOK;
  }
  bool unmapFa(UInt32*);
  Mlx4Icm *icmAlloc(UInt32,bool=false){++allocs;fresh=Mlx4Icm();fresh.pages=fresh.backing;return allocOK?&fresh:nullptr;}
  bool icmCheck(Mlx4Icm*,UInt32*){return true;}
  void icmFree(Mlx4Icm*p){++frees;freed=p;}
  void icmFill(Mlx4Icm*p){++fills;filled=p;memset(p->data,0xa5,sizeof(p->data));}
  bool mapFa(Mlx4Icm*p,UInt32*entries,UInt32*s){++maps;p->generation.markHandoffAttempted(7);*entries=mapOK?2:1;*s=mapStatus;return mapOK;}
  bool runFw(UInt32*s){++runs;*s=runStatus;return runOK;}
  bool modStatCfg(UInt32,UInt32,UInt32*s){++mods;*s=modStatus;return modOK;}
};
class Cx3Mlx4 {
public:
    Cx3PortQueues fPortQueues[2] = { []() { Cx3PortQueues q{}; q.init(1); q.macUp = false; return q; }(), []() { Cx3PortQueues q{}; q.init(2); return q; }() };

public:
  Mlx4Hw fHw;Mlx4Icm *fFwIcm=nullptr;
  UInt32 fQueriedFwPages=2;UInt64 fDeviceGeneration=7;
  bool fHcaInited=false,fIcmMapped=false,fIcmHcaHolds=false,fNetEnabled=false;
  bool fQuarExhausted=false,fResetCommandsBlocked=false,fStopRetained=false;
  bool fResetUnproven=false;
  bool fClrIntReady=false;
  bool fFwRunAttempted=false,fIrqObjectsUp=false,fIrqEnabled=false;
  void *fNetIface=nullptr,*fNetTimer=nullptr,*fNetTxRing=nullptr,*fIrqSource=nullptr;
  bool context=false,tables=false,retention=false,stopping=false;
  void *fNetAttachLock=this,*fNetAttachCall=nullptr,*fNetAttachIface=nullptr;
  bool fNetAttachInFlight=false,fNetAttachPoisoned=false;
  unsigned samples=0;
  bool hasContextOwners()const{return context;}
  bool hasIcmTableAllocations()const{return tables;}
  bool hasStopRetentionOwners()const{return retention;}
  bool selectorStopping()const{return stopping;}
  bool fwReplacementAllowed();
  void fillFwAreaSummary(const char*,UInt64*out){++samples;out[kFwLoadOutNonzero]=16;out[kFwLoadOutChecksum]=0x12345678;}
  IOReturn exportFwLoad(UInt32,bool,bool,bool,UInt64*);
  IOReturn exportIcmAlloc(UInt32,UInt64*);
};
METHODS
static Cx3Mlx4 *endpoint=nullptr;
static bool shortReply=false,transportFail=false;
static int call(uint32_t selector,const uint64_t*in,uint32_t count,uint64_t*out,uint32_t*n){
  if(transportFail)return -1;
  if(selector==kCx3SelIcmAlloc && count==kIcmInCount && *n==kIcmOutCount){
    int result=endpoint->exportIcmAlloc(in[0],out);
    if(shortReply)*n=kIcmOutCount-1;
    return result;
  }
  if(selector!=kCx3SelFwLoad || count!=kFwLoadInCount || *n!=kFwLoadOutCount)return -1;
  int result=endpoint->exportFwLoad(in[kFwLoadInPages],in[kFwLoadInLowmem],
                                   in[kFwLoadInForce],in[kFwLoadInFill],out);
  if(shortReply)*n=kFwLoadOutCount-1;
  return result;
}
static const char *fwStatusName(uint64_t status){return status?"error":"success";}
CLIENT
static unsigned cases=0,checks=0,failures=0;
#define CHECK(name,e) do{++checks;if(!(e)){++failures;printf("FAIL case %u: %s\n",cases,name);}}while(0)
static void no_new_work(Cx3Mlx4 &d,Mlx4Icm &old){
  CHECK("exact old owner and bytes survive refusal",d.fFwIcm==&old && old.data[0]==0x11223344 && old.data[3]==0xaabbccdd);
  CHECK("refusal has no free/alloc/MAP/RUN/MOD/fill/sample",d.fHw.frees==0 && d.fHw.allocs==0 && d.fHw.maps==0 && d.fHw.runs==0 && d.fHw.mods==0 && d.fHw.fills==0 && d.samples==0);
}
int main(){
  for(UInt32 status : {1u,CX3_FWSTATUS_TIMEOUT,CX3_FWSTATUS_NOT_POSTED})for(bool force:{false,true}){
    ++cases;Cx3Mlx4 d;Mlx4Icm old;old.generation.markHandoffAttempted(7);d.fFwIcm=&old;d.fHw.unmapOK=false;d.fHw.unmapStatus=status;
    UInt64 out[kFwLoadOutCount];memset(out,0xa5,sizeof(out));
    CHECK("dispatch returns result envelope",d.exportFwLoad(2,false,force,true,out)==0);
    CHECK("failed UNMAP reported as failure",out[kFwLoadOutStatus]!=0);
    CHECK("failed UNMAP reached exactly once",d.fHw.unmaps==1);
    no_new_work(d,old);
    CHECK("new stages retain not-posted sentinel",out[kFwLoadOutMapFaStatus]==0xffffffffull && out[kFwLoadOutRunFwStatus]==0xffffffffull && out[kFwLoadOutModStatus]==0xffffffffull);
    d.exportFwLoad(2,false,force,true,out);
    no_new_work(d,old);
    CHECK("repeated rejected UNMAP never compounds allocation",d.fHw.unmaps==2);
  }
  for(unsigned live=0;live<27;live++)for(bool force:{false,true}){
    ++cases;Cx3Mlx4 d;Mlx4Icm old;d.fFwIcm=&old;
    switch(live){
      case 0:d.fHcaInited=true;break;case 1:d.fIcmMapped=true;break;
      case 2:d.fIcmHcaHolds=true;break;case 3:d.fNetEnabled=true;break;
      case 4:d.context=true;break;case 5:d.tables=true;break;case 6:d.retention=true;break;
      case 7:d.fNetIface=&old;break;case 8:d.fNetTimer=&old;break;
      case 9:d.fNetTxRing=&old;break;case 10:d.fIrqSource=&old;break;
      case 11:d.fQuarExhausted=true;break;case 12:d.fResetCommandsBlocked=true;break;
      case 13:d.fStopRetained=true;break;case 14:d.fHw.wedged=true;break;
      case 15:d.fPortQueues[0].macUp=true;break;case 16:d.fClrIntReady=true;break;
      case 17:d.stopping=true;break;
      case 18:d.fHw.ab=true;break;case 19:d.fNetAttachInFlight=true;break;
      case 20:d.fNetAttachPoisoned=true;break;case 21:d.fNetAttachIface=&old;break;
      case 22:d.fNetAttachLock=nullptr;d.fNetAttachCall=&old;break;
      case 23:d.fNetAttachLock=nullptr;d.fNetAttachIface=&old;break;
      case 24:d.fFwRunAttempted=true;break;case 25:d.fIrqObjectsUp=true;break;
      case 26:d.fIrqEnabled=true;break;
    }
    UInt64 out[kFwLoadOutCount];
    d.exportFwLoad(2,false,force,true,out);
    CHECK("live/unknown dependency rejected",out[kFwLoadOutStatus]==UInt64(kIOReturnNotReady));
    CHECK("admission refusal precedes hardware reads and UNMAP",d.fHw.reads==0 && d.fHw.unmaps==0);
    no_new_work(d,old);
    UInt64 icm[kIcmOutCount];
    for(UInt32 pages:{0u,2u}){
      d.exportIcmAlloc(pages,icm);
      CHECK("icmalloc cannot bypass lifecycle refusal",icm[kIcmOutStatus]==UInt64(kIOReturnNotReady));
      no_new_work(d,old);
    }
    CHECK("attach snapshot releases its lock",lockDepth==0);
  }
  for(unsigned state=0;state<5;state++){
    ++cases;Cx3Mlx4 d;Mlx4Icm old;d.fFwIcm=&old;UInt64 out[kFwLoadOutCount],icm[kIcmOutCount];
    if(state==0)old.generation.init(0);
    if(state==1)old.generation.init(6);
    if(state==2){old.generation.mayBeDeviceOwned=true;old.generation.ownerGeneration=0;}
    if(state==3){old.generation.mayBeDeviceOwned=true;old.generation.ownerGeneration=6;}
    if(state==4)old.generation.ownerGeneration=7;
    d.exportFwLoad(2,false,true,true,out);
    CHECK("invalid/stale owner refuses fwload",out[kFwLoadOutStatus]==UInt64(kIOReturnNotReady));
    for(UInt32 pages:{0u,2u}){
      d.exportIcmAlloc(pages,icm);
      CHECK("invalid/stale owner refuses icmalloc",icm[kIcmOutStatus]==UInt64(kIOReturnNotReady));
    }
    CHECK("owner refusal is device inert",d.fHw.reads==0 && d.fHw.unmaps==0);
    no_new_work(d,old);
  }
  for(unsigned mode=0;mode<3;mode++){
    ++cases;Cx3Mlx4 d;Mlx4Icm old;d.fFwIcm=&old;
    if(mode==1)old.generation.markHandoffAttempted(7);
    UInt64 out[kFwLoadOutCount];
    if(mode==2)d.fQueriedFwPages=3;
    d.exportFwLoad(2,false,mode==2,true,out);
    CHECK("host-only or confirmed scoped replacement succeeds",out[kFwLoadOutStatus]==0 && d.fFwIcm==&d.fHw.fresh);
    CHECK("only prior device handoff requires UNMAP",d.fHw.unmaps==unsigned(mode==1));
    CHECK("replacement frees only the old owner then performs new work",d.fHw.frees==1 && d.fHw.freed==&old && d.fHw.allocs==1 && d.fHw.maps==1 && d.fHw.runs==1 && d.fHw.fills==1);
  }
  for(UInt32 pages:{0u,2u}){
    ++cases;Cx3Mlx4 d;Mlx4Icm old;d.fFwIcm=&old;UInt64 out[kIcmOutCount];
    d.exportIcmAlloc(pages,out);
    CHECK("host-only diagnostic free/replacement remains usable",out[kIcmOutStatus]==0 && d.fHw.frees==1 && d.fHw.freed==&old && d.fFwIcm==(pages?&d.fHw.fresh:nullptr));
    CHECK("host-only operation has no device reads or commands",d.fHw.reads==0 && d.fHw.unmaps==0 && d.fHw.maps==0 && d.fHw.runs==0);
  }
  for(unsigned fail=0;fail<3;fail++){
    ++cases;Cx3Mlx4 d;UInt64 fw[kFwLoadOutCount],icm[kIcmOutCount];
    d.fHw.mapOK=fail!=1;d.fHw.runOK=fail!=2;
    d.exportFwLoad(2,false,false,false,fw);
    Mlx4Icm *held=d.fFwIcm;unsigned allocs=d.fHw.allocs,maps=d.fHw.maps,runs=d.fHw.runs;
    for(UInt32 pages:{0u,2u}){
      d.exportIcmAlloc(pages,icm);
      CHECK("actual fwload handoff blocks cross-selector free and replacement",icm[kIcmOutStatus]==UInt64(kIOReturnNotReady) && d.fFwIcm==held && d.fHw.frees==0 && d.fHw.allocs==allocs && d.fHw.maps==maps && d.fHw.runs==runs);
    }
  }
  for(unsigned failure=0;failure<5;failure++){
    ++cases;Cx3Mlx4 d;UInt64 out[kFwLoadOutCount];
    d.fHw.allocOK=failure!=1;d.fHw.mapOK=failure!=2;d.fHw.mapStatus=failure==2?1:0;
    d.fHw.runOK=failure!=3;d.fHw.runStatus=failure==3?CX3_FWSTATUS_TIMEOUT:0;
    d.fHw.modOK=failure!=4;d.fHw.modStatus=failure==4?1:0;
    d.exportFwLoad(2,true,false,false,out);
    CHECK("first load never unmaps or frees a nonexistent old owner",d.fHw.unmaps==0 && d.fHw.frees==0);
    CHECK("first load reaches intended failure boundary",d.fHw.allocs==1 && d.fHw.maps==unsigned(failure!=1) && d.fHw.runs==unsigned(failure!=1 && failure!=2) && d.fHw.mods==unsigned(failure==0 || failure==4));
    CHECK("first load status and non-fatal MOD policy preserved",(out[kFwLoadOutStatus]==0)==(failure==0 || failure==4));
    CHECK("partial MAP/RUN retains exact new allocation",d.fFwIcm==(failure==1?nullptr:&d.fHw.fresh));
    CHECK("initial fill is opt-in",d.fHw.fills==0);
    CHECK("RUN_FW attempt remains recorded even on an uncertain result",d.fFwRunAttempted==(failure!=1 && failure!=2));
    if(failure==0 || failure==3 || failure==4){
      Mlx4Icm *held=d.fFwIcm;unsigned reads=d.fHw.reads,allocs=d.fHw.allocs,maps=d.fHw.maps,runs=d.fHw.runs;
      d.exportFwLoad(2,false,true,true,out);
      CHECK("live or uncertain RUN_FW cannot be replaced without recovery contract",out[kFwLoadOutStatus]==UInt64(kIOReturnNotReady) && d.fFwIcm==held && d.fHw.reads==reads && d.fHw.allocs==allocs && d.fHw.maps==maps && d.fHw.runs==runs && d.fHw.unmaps==0 && d.fHw.frees==0);
    }
  }
  for(unsigned invalid=0;invalid<3;invalid++){
    ++cases;Cx3Mlx4 d;Mlx4Icm old;d.fFwIcm=&old;UInt64 out[kFwLoadOutCount];
    d.exportFwLoad(invalid==0?0:(invalid==1?(1u<<20)+1:3),false,false,true,out);
    CHECK("invalid count rejected before hardware",out[kFwLoadOutStatus]==UInt64(kIOReturnBadArgument) && d.fHw.reads==0 && d.fHw.unmaps==0);
    no_new_work(d,old);
  }
  {
    ++cases;Cx3Mlx4 d;d.fResetUnproven=true;UInt64 out[kFwLoadOutCount];
    d.exportFwLoad(2,false,false,false,out);
    CHECK("clean first-reset bookkeeping does not block initial FW load",out[kFwLoadOutStatus]==0 && d.fFwIcm==&d.fHw.fresh && d.fHw.maps==1 && d.fHw.runs==1 && d.fHw.unmaps==0 && d.fHw.frees==0);
  }
  {
    ++cases;Cx3Mlx4 d;Mlx4Icm old;d.fFwIcm=&old;d.fHw.window=false;UInt64 out[kFwLoadOutCount];
    d.exportFwLoad(2,false,false,true,out);
    CHECK("offline window rejects without UNMAP",out[kFwLoadOutStatus]==UInt64(kIOReturnOffline) && d.fHw.reads==1 && d.fHw.unmaps==0);
    no_new_work(d,old);
  }
  for(unsigned mode=0;mode<6;mode++){
    ++cases;Cx3Mlx4 d;Mlx4Icm old;endpoint=&d;shortReply=mode==4;transportFail=mode==5;
    if(mode==1){old.generation.markHandoffAttempted(7);d.fFwIcm=&old;d.fHw.unmapOK=false;d.fHw.unmapStatus=1;}
    if(mode==2)d.fHcaInited=true;
    if(mode==3){d.fHw.runOK=false;d.fHw.runStatus=CX3_FWSTATUS_TIMEOUT;}
    int result=cmdFwLoad(2,0,0,0);
    CHECK("actual CLI returns nonzero for any rejected/incomplete/short/transport result",(result==0)==(mode==0));
  }
  for(unsigned mode=0;mode<4;mode++){
    ++cases;Cx3Mlx4 d;Mlx4Icm old;endpoint=&d;shortReply=mode==2;transportFail=mode==3;
    if(mode==1){old.generation.markHandoffAttempted(7);d.fFwIcm=&old;}
    CHECK("icmalloc CLI returns nonzero for rejection/short/transport",(cmdIcmAlloc(0)==0)==(mode==0));
  }
  printf("FW load lifetime: %u cases, %u checks, %u failures (production controller/UNMAP/CLI; host seams)\n",cases,checks,failures);
  return failures?1:0;
}
'''.replace('METHODS', methods).replace('CLIENT', client)
    with tempfile.TemporaryDirectory(prefix='cx3-fw-load-') as temp:
        binary = str(Path(temp)/'test')
        subprocess.run(['/usr/bin/clang++','-std=c++11','-Wall','-Wextra','-Werror',
                        '-I',str(ROOT/'src'),'-x','c++','-','-o',binary],
                       input=code,text=True,check=True,timeout=60)
        return subprocess.run([binary],timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
