#!/usr/bin/env python3
"""Execute production stop + EQ + TX stop with injected cleanup refusals.

Actual stop, netServiceStop, teardownEq, releaseIcmOrKeep and owner predicates
are extracted unchanged. IOKit/hardware operations and other individually
tested child teardowns are controlled seams. Proves software owner survival
and stop composition, never DMA quiescence or real framework behavior.
"""
from pathlib import Path
import argparse
import re
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
    args = parser.parse_args()
    source = (args.source or ROOT/'src/Cx3Mlx4.cpp').read_text()
    init = method(source, 'bool Cx3Mlx4::init(')
    table_init = init[init.index('    fNumTables = 0;'):init.index('    fIcmSize = 0;')]
    methods = '\n'.join(method(source, signature) for signature in (
        'void Cx3Mlx4::stop(IOService *provider)',
        'void Cx3Mlx4::teardownEq(',
        'bool Cx3Mlx4::netServiceStop(',
        'bool Cx3Mlx4::releaseIcmOrKeep(',
        'bool Cx3Mlx4::hasContextOwners() const',
        'void Cx3Mlx4::netTimerAction('))
    # Optional new production predicate: missing endpoint is an observed false
    # outcome, so the red run fails assertions rather than compilation.
    signature = 'bool Cx3Mlx4::hasIcmTableAllocations() const'
    methods += '\n' + (method(source, signature) if signature in source else
                        signature + ' { return false; }\n')
    signature = 'bool Cx3Mlx4::hasStopRetentionOwners() const'
    methods += '\n' + (method(source, signature) if signature in source else
                        signature + ' { return false; }\n')
    for signature in ('bool Cx3Mlx4::closePortsForStop(const char *why)',
                      'bool Cx3Mlx4::closeHcaForStop(const char *why)',
                      'bool Cx3Mlx4::closeFwForStop(const char *why)'):
        methods += '\n' + (method(source, signature) if signature in source else
                            signature + ' { (void)why; return false; }\n')
    hw_source = (args.hw_source or ROOT/'src/Mlx4Hw.cpp').read_text()
    for signature in ('bool Mlx4Hw::closePort(UInt32 port, UInt32 *outFwStatus)',
                      'bool Mlx4Hw::closeHca(UInt32 *outFwStatus)',
                      'bool Mlx4Hw::initPort(UInt32 port, UInt32 *outFwStatus)',
                      'bool Mlx4Hw::unmapFa(UInt32 *outFwStatus)'):
        methods += '\n' + (method(hw_source, signature) if signature in hw_source else
                            signature + ' { (void)outFwStatus; return false; }\n')
    stage_start = source.index('    /* 4. INIT_PORT */', source.index('IOReturn Cx3Mlx4::exportM3PortUp('))
    stage_end = source.index('    /*\n     * 5. Read the link state.', stage_start)
    init_port_stage = source[stage_start:stage_end]
    signature = 'bool Mlx4Hw::mailboxNeedsRetention() const'
    if signature in hw_source:
        methods += '\n' + method(hw_source, signature)
    else:
        # Execute the old shutdown decision verbatim for a pre-helper replay.
        shutdown = method(hw_source, 'void Mlx4Hw::shutdown(')
        expression = re.search(r'mailbox\.init\(([^;]+)\);', shutdown).group(1)
        methods += '\n' + signature + ' { return ' + expression + '; }\n'
    code = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#define CX3_HOST_TEST
#include "mlx4_pure.h"
#include "cx3_abi.h"
#include "Cx3PortQueues.h"
#include "Cx3PortState.h"
constexpr int kIOReturnSuccess=0;
using UInt8=uint8_t;using UInt16=uint16_t;using UInt32=uint32_t;using UInt64=uint64_t;
struct IOService {
  unsigned refs=1,stopCalls=0,releaseCalls=0;
  void retain(){++refs;}void release(){--refs;++releaseCalls;}
  void stop(IOService*){++stopCalls;}
};
using OSObject=IOService;
struct IOTimerEventSource {};
#define OSDynamicCast(type,value) static_cast<type*>(value)
struct IOPCIDevice:IOService {
  unsigned disables=0,closes=0;
  bool setBusMasterEnable(bool){++disables;return true;}
  UInt16 configRead16(unsigned){return 0;}
  void close(void*){++closes;}
};
struct IOLock { bool locked=false,freed=false; };
static void IOLockLock(IOLock*l){l->locked=true;}
static void IOLockUnlock(IOLock*l){l->locked=false;}
static void IOLockFree(IOLock*l){l->freed=true;}
static bool thread_call_cancel(void*){return true;}
static bool thread_call_free(void*){return true;}
static void IOSleep(unsigned){}
static void sink(const char*,...){}
#define LOG(...) sink(__VA_ARGS__)
struct Mlx4Icm { unsigned id; UInt64 allocationId=1; Mlx4DmaGeneration generation{}; };
struct InitSliceFixture {
  Mlx4IcmTable fTables[3]; UInt32 fNumTables;
  void initialize(){TABLE_INIT}
};
enum {kIOPCIConfigCommand=4,kCx3QuiesceStopping=1};
struct Mlx4Hw {
  unsigned shutdownCalls=0,eqCalls=0,mttCalls=0,freeCalls=0,mptClearCalls=0;
  unsigned doorbells=0,postShutdownWrites=0;
  IOPCIDevice *providerView=nullptr;bool abRetained=false;
  bool eqOk=true,mttOk=true,mttLive=false;
  unsigned portCalls=0,hcaCalls=0;int portFail=0;
  bool hcaOk=true,portErrorWithTrue=false,commandShapeValid=true,initOk=true;UInt32 hcaStatus=0;
  unsigned initCalls=0,faCalls=0;bool faOk=true,poisonOnFa=false;UInt32 faStatus=0;
  std::vector<std::string> order;
  bool fCmdWedged=false,fMboxRetained=false;
  bool fHaveLastFw=true;struct {UInt32 cmdIfRev=3;} fLastFw;
  void *fMboxMd=nullptr,*fMboxCmd=nullptr,*fModuleInbox=nullptr;
  bool mailboxNeedsRetention()const;
  bool cmdWedged()const{return fCmdWedged;}
  bool closePort(UInt32,UInt32*);bool closeHca(UInt32*);
  bool initPort(UInt32,UInt32*);bool unmapFa(UInt32*);
  bool cmdBox(UInt64 in,UInt64 out,UInt32 modifier,UInt8 opMod,UInt16 op,UInt32 timeout,UInt32*s){
    commandShapeValid=commandShapeValid && in==0 && out==0 && opMod==0 && s &&
      (!providerView || (providerView->disables==0 && providerView->closes==0));
    if(op==MLX4_CMD_CLOSE_PORT){
      ++portCalls;order.push_back("port"+std::to_string(modifier));
      commandShapeValid=commandShapeValid && modifier>=1 && modifier<=2 && timeout==MLX4_CMD_TIME_CLASS_A;
      *s=portErrorWithTrue?9:(int)modifier==portFail?7:0;return (int)modifier!=portFail;
    }
    if(op==MLX4_CMD_CLOSE_HCA){
      ++hcaCalls;order.push_back("hca");*s=hcaStatus;
      commandShapeValid=commandShapeValid && modifier==0 && timeout==MLX4_CMD_TIME_CLASS_C;return hcaOk;
    }
    if(op==MLX4_CMD_INIT_PORT){
      ++initCalls;*s=initOk?0:0xffffffffu;
      commandShapeValid=commandShapeValid && modifier>=1 && modifier<=2 && timeout==MLX4_CMD_TIME_CLASS_A;return initOk;
    }
    if(op==MLX4_CMD_UNMAP_FA){
      ++faCalls;order.push_back("fa");*s=faStatus;if(poisonOnFa)fCmdWedged=true;
      commandShapeValid=commandShapeValid && modifier==0 && timeout==MLX4_CMD_TIME_CLASS_B && shutdownCalls==0;
      return faOk;
    }
    commandShapeValid=false;return false;
  }
  std::vector<unsigned> freed;
  bool hasAbRetainedRings()const{return abRetained;}
  bool hasMttAllocations()const{return mttLive;}
  bool eqHw2Sw(UInt32,UInt32*s){++eqCalls;*s=eqOk?0:7;return eqOk;}
  bool mttFree(UInt32,UInt32,Mlx4Icm*){++mttCalls;if(mttOk)mttLive=false;return mttOk;}
  void icmFree(Mlx4Icm*p){++freeCalls;freed.push_back(p->id);order.push_back("free"+std::to_string(p->id));}
  void setMptTable(void*,UInt64,UInt64){++mptClearCalls;}
  void shutdown(){++shutdownCalls;order.push_back("shutdown");}
  void doorbell(){if(shutdownCalls)++postShutdownWrites;else ++doorbells;}
};
struct NativeChild {void closeAdmission(){}};
class Cx3Mlx4:public IOService {
public:
    Cx3PortQueues fPortQueues[2] = { []() { Cx3PortQueues q{}; q.init(1); q.qpUp = false; q.ingressUp = false; q.txQpUp = false; q.cqUp = false; q.rqUp = false; q.txCqUp = false; q.qpRec = nullptr; q.ingressRec = nullptr; q.txQpRec = nullptr; q.cqRing = nullptr; q.cqRec = nullptr; q.rqRing = nullptr; q.rqBufs = nullptr; q.txCqRing = nullptr; q.txCqRec = nullptr; q.txSq = nullptr; q.rawFrame = nullptr; return q; }(), []() { Cx3PortQueues q{}; q.init(2); return q; }() };

public:
  NativeChild *fNativePorts[2]{};bool stopNativePorts(){return true;}
  using super=IOService;
  enum{kCx3AttachWorkerPoisoned=1,kCx3AttachWorkerIdle=0,kNetTxSlots=4,kMaxIcmTables=3};
  Mlx4Hw fHw;
  bool fStopRetained=false,fNetEnabled=true,fNetAttachInFlight=false;
  bool fHcaInited=false,fIcmHcaHolds=false,fIcmMapped=false,fResetCommandsBlocked=false,fFwRunAttempted=false;
  UInt64 fDeviceGeneration=1;
  Cx3PortState fPorts[2]{};
  struct {UInt32 num_ports=2;} fDevCap;
  IOService *fStopProvider=nullptr;IOPCIDevice *fPci=nullptr;
  IOLock attachLock,txLock;IOLock *fNetAttachLock=&attachLock,*fNetTxLock=&txLock;
  void *fNetAttachCall=nullptr;UInt32 fNetAttachRetainCount=0;
  Mlx4TxAdmissionGate fNetTxAdmission;
  IOService media,medium,workloop;IOService *fMediaDict=&media,*fMediumAuto=&medium,*fWorkLoop=&workloop;
  bool fMediaPublished=true,fPciOpen=true;
  Mlx4IcmTable fTables[kMaxIcmTables]={};UInt32 fNumTables=0;
  Mlx4Icm *fAuxIcm=nullptr,*fFwIcm=nullptr,*fNetTxRing=nullptr;
  struct LedgerSlot {Mlx4Icm *icm=nullptr;};
  LedgerSlot fQuar[3],fIcmKept[3];Mlx4Icm *fQuarOverflow[3]={};
  UInt32 fQuarCount=0,fQuarOverflowCount=0,fIcmKeptCount=0;
  UInt32 fQuarOverflowUntracked=0,fIcmKeptOverflow=0;
  UInt32 fNetTxSlotCount=4,fNetTxInFlightAtStop=0;
  void *fNetTxMbuf[kNetTxSlots]={};
  UInt64 fNetTicks=0,fNetRxFrames=0,fNetRxDropped=0,fNetRxRefills=0,fNetRxErrors=0;
  UInt64 fNetTxFrames=0,fNetTxCompleted=0,fNetTxStalled=0,fNetTxDropped=0,fNetTxErrors=0;
  bool fEqMayBeDeviceOwned=false,fEqHwReturned=false,fEqUnknownRetained=false;
  Mlx4Icm *fEqRing=nullptr;UInt32 fEqNent=0,fEqn=0,fEqMTTOffset=0,fEqMttPages=0;
  UInt32 fEqeFactor=0,fEqDoorbell=0,fEqConsIndex=0;
  UInt32 fMrState=0;
  
  
  
  
  bool irqOk=true,timerOk=true,retentionOk=true,tableOk=true,tableKeep=false;
  unsigned childCalls=0,tableCalls=0,drainCalls=0,mbufFrees=0,retainedBlocks=0;
  unsigned timerTicks=0;
  bool stopInsideTick=false,lateTimerDuringDetach=false;
  std::string fail;
  Cx3Mlx4(){fNetTxAdmission.init();for(unsigned i=0;i<2;++i){fPorts[i].init(i+1,1);fPorts[i].linkUp=true;fPorts[i].speedMbps=10000;}}
  bool beginStopAndDrain(UInt32){return true;}
  void netAttachWorkerMark(UInt32){}
  bool netDetachInterfacesAndWait(UInt32){if(lateTimerDuringDetach)netTimerAction(this,nullptr);return true;}
  bool irqReleaseObjects(const char*){return irqOk;}
  bool netTimerRelease(const char*){return timerOk;}
  bool quarantineTake(Mlx4Icm*p,const char*){if(retentionOk){++retainedBlocks;fQuar[fQuarCount++].icm=p;}return retentionOk;}
  void quarantineDrain(const char*,int){++drainCalls;}
  void freePacket(void*){++mbufFrees;}
  void ingressTeardown(const char*,bool,UInt32 port=1){if(port==2 && !fPortQueues[1].hasOwners())return;++childCalls;fHw.order.push_back("child");if(fail!="ingress")fPortQueues[port-1].ingressRec=nullptr;}
  void txTeardown(const char*,bool,UInt32 port=1){if(port==2 && !fPortQueues[1].hasOwners())return;++childCalls;fHw.order.push_back("child");if(fail!="tx")fPortQueues[port-1].txSq=nullptr;if(fail!="rawframe")fPortQueues[port-1].rawFrame=nullptr;}
  void qpTeardown(const char*,bool,UInt32 port=1){if(port==2 && !fPortQueues[1].hasOwners())return;++childCalls;fHw.order.push_back("child");if(fail!="qp")fPortQueues[port-1].qpRec=nullptr;}
  void mrTeardown(const char*,bool){++childCalls;fHw.order.push_back("child");if(fail!="mr")fMrState=0;}
  void cqTeardown(const char*,bool,UInt32 port=1){if(port==2 && !fPortQueues[1].hasOwners())return;++childCalls;fHw.order.push_back("child");if(fail!="cq")fPortQueues[port-1].cqRing=nullptr;}
  void rqTeardown(const char*,bool,UInt32 port=1){if(port==2 && !fPortQueues[1].hasOwners())return;++childCalls;fHw.order.push_back("child");if(fail!="rq")fPortQueues[port-1].rqRing=nullptr;}
  void freeIcmTables(){++tableCalls;fHw.order.push_back(fHcaInited?"unsafe-tables":"tables");if(tableOk){for(auto&t:fTables)for(auto&p:t.icm){if(p && tableKeep)fIcmKept[fIcmKeptCount++].icm=p;p=nullptr;}if(fAuxIcm && tableKeep)fIcmKept[fIcmKeptCount++].icm=fAuxIcm;fAuxIcm=nullptr;fIcmMapped=false;fIcmHcaHolds=false;}}
  void stop(IOService*);void teardownEq(const char*,bool);bool netServiceStop(const char*);
  bool releaseIcmOrKeep(Mlx4Icm**,const char*,bool);bool hasContextOwners()const;
  bool hasIcmTableAllocations()const;
  bool hasStopRetentionOwners()const;
  bool closePortsForStop(const char*);bool closeHcaForStop(const char*);bool closeFwForStop(const char*);
  int initializePortStage(UInt32 port){
    Cx3PortState &portState=fPorts[port-1];
    UInt32 initStatus=CX3_FWSTATUS_NOT_POSTED;UInt64 out[kPortUpOutCount]{};
    INIT_PORT_STAGE
    (void)out;return kIOReturnSuccess;
  }
  static void netTimerAction(OSObject*,IOTimerEventSource*);
  void netTick(){++timerTicks;if(stopInsideTick)stop(fPci);fHw.doorbell();}
};
METHODS
static unsigned checks=0,failures=0,cases=0;
#define CHECK(name,e) do{++checks;if(!(e)){++failures;printf("FAIL case %u: %s\n",cases,name);}}while(0)
static void retained(Cx3Mlx4 &d,IOPCIDevice &pci) {
  CHECK("incomplete stop pins self and provider",d.fStopRetained && d.fStopProvider==&pci && d.refs==2 && pci.refs==2);
  CHECK("incomplete owner keeps hardware/provider access",d.fHw.shutdownCalls==0 && pci.closes==0 && d.fPci==&pci && d.fPciOpen);
  CHECK("incomplete owner preserves MPT and workloop/media references",d.fHw.mptClearCalls==0 && d.fWorkLoop && d.fMediaDict && d.fMediumAuto);
  unsigned child=d.childCalls,eq=d.fHw.eqCalls,mtt=d.fHw.mttCalls,refs=d.refs,pr=pci.refs;
  d.stop(&pci);
  CHECK("incomplete stop is sticky without retry or another retain",d.refs==refs && pci.refs==pr && d.childCalls==child && d.fHw.eqCalls==eq && d.fHw.mttCalls==mtt && d.stopCalls==1);
}
int main(){
  Mlx4Icm ring={1},record={2},frames={3},table={4},aux={5},fw={6};
  {
    ++cases;InitSliceFixture f;memset(f.fTables,0xa5,sizeof(f.fTables));f.fNumTables=123;
    f.initialize();bool empty=true;for(unsigned char b:reinterpret_cast<unsigned char(&)[sizeof(f.fTables)]>(f.fTables))if(b)empty=false;
    CHECK("production init clears poisoned full table storage",empty);
    CHECK("production init sets active profile count to zero",f.fNumTables==0);
  }
  for(unsigned failure=0;failure<2;failure++){
    ++cases;Cx3Mlx4 d;IOPCIDevice pci;d.fPci=&pci;d.fHw.providerView=&pci;
    d.fEqRing=&ring;d.fEqNent=64;d.fEqn=9;d.fEqMayBeDeviceOwned=true;
    d.fEqMttPages=1;d.fEqMTTOffset=64;d.fHw.mttLive=true;
    d.fHw.eqOk=failure!=0;d.fHw.mttOk=failure!=1;
    d.fNumTables=1;d.fTables[0].icm[0]=&table;
    d.stop(&pci);retained(d,pci);
    CHECK("failed EQ keeps exact ring/MTT owner",d.fEqRing==&ring && d.fEqMttPages==1 && d.fEqn==9);
    CHECK("failed EQ skips tables and ring frees",d.tableCalls==0 && d.fHw.freeCalls==0 && d.fTables[0].icm[0]==&table);
  }
  for(const char *stage:{"ingress","tx","qp","mr","cq","rq","rawframe"}){
    ++cases;Cx3Mlx4 d;IOPCIDevice pci;d.fPci=&pci;d.fHw.providerView=&pci;d.fail=stage;
    d.fPortQueues[0].ingressRec=&record;d.fPortQueues[0].txSq=&ring;d.fPortQueues[0].qpRec=&record;d.fMrState=1;
    d.fPortQueues[0].cqRing=&ring;d.fPortQueues[0].rqRing=&ring;d.fPortQueues[0].rawFrame=&frames;d.fNumTables=1;d.fTables[0].icm[0]=&table;
    d.stop(&pci);retained(d,pci);
    CHECK("failed child prevents dependent table teardown",d.tableCalls==0 && d.fTables[0].icm[0]==&table);
  }
  {
    ++cases;Cx3Mlx4 d;IOPCIDevice pci;d.fPci=&pci;d.fHw.providerView=&pci;d.fNetTxRing=&frames;
    d.fNetTxMbuf[0]=&frames;d.retentionOk=false;
    d.stop(&pci);retained(d,pci);
    CHECK("TX transfer refusal keeps frame owner/metadata/lock",d.fNetTxRing==&frames && d.fNetTxSlotCount==4 && d.fNetTxLock==&d.txLock && !d.txLock.freed && !d.txLock.locked);
    CHECK("TX transfer refusal skips all child teardown",d.childCalls==0 && d.fHw.eqCalls==0 && d.tableCalls==0);
  }
  for(unsigned failure=0;failure<3;failure++){
    ++cases;Cx3Mlx4 d;IOPCIDevice pci;d.fPci=&pci;d.fHw.providerView=&pci;d.tableOk=false;
    d.fNumTables=2;
    if(failure<2)d.fTables[1].icm[failure+1]=&table;
    else d.fAuxIcm=&aux;
    d.stop(&pci);retained(d,pci);
    CHECK("table refusal prevents FW/quarantine progression",d.drainCalls==0 && d.tableCalls==1);
    CHECK("table owner predicate reflects live slots despite profile counts",d.hasIcmTableAllocations());
  }
  {
    ++cases;Cx3Mlx4 d;IOPCIDevice pci;d.fPci=&pci;d.fHw.providerView=&pci;
    // An allocation beyond the active profile count must not disappear from
    // the bounded owner scan merely because that profile count was cleared.
    d.fTables[d.kMaxIcmTables-1].icm[sizeof(d.fTables[0].icm)/sizeof(d.fTables[0].icm[0])-1]=&table;
    d.stop(&pci);retained(d,pci);
    CHECK("parked table owner found even with zero profile count",d.tableCalls==0 && d.hasIcmTableAllocations());
  }
  {
    ++cases;Cx3Mlx4 d;IOPCIDevice pci;d.fPci=&pci;d.fHw.providerView=&pci;d.fFwIcm=&fw;d.retentionOk=false;
    d.stop(&pci);retained(d,pci);
    CHECK("FW refusal keeps original owner",d.fFwIcm==&fw && d.drainCalls==0);
  }
  {
    ++cases;Cx3Mlx4 d;IOPCIDevice pci;d.fPci=&pci;d.fHw.providerView=&pci;
    d.fEqRing=&ring;d.fEqNent=64;d.fEqn=9;d.fEqMayBeDeviceOwned=true;d.fEqMttPages=1;
    d.fHw.mttLive=true;d.fNumTables=2;d.fTables[1].icm[2]=&table;d.fAuxIcm=&aux;
    d.stop(&pci);
    CHECK("successful stop releases exact EQ owner",d.fHw.eqCalls==1 && d.fHw.mttCalls==1 && d.fHw.freeCalls==1 && !d.fEqRing);
    CHECK("successful stop reaches provider close and shutdown",!d.fStopRetained && d.refs==1 && pci.refs==1 && d.fHw.shutdownCalls==1 && pci.closes==1 && !d.fPci && d.stopCalls==1);
    CHECK("successful stop tolerates nonzero profile count with empty allocation slots",d.fNumTables==2 && !d.hasIcmTableAllocations());
    CHECK("successful stop releases media/workloop",!d.fMediaDict && !d.fMediumAuto && !d.fWorkLoop);
  }
  {
    ++cases;Cx3Mlx4 d;IOPCIDevice pci;d.fPci=&pci;d.fHw.providerView=&pci;
    d.stopInsideTick=true;Cx3Mlx4::netTimerAction(&d,nullptr);
    CHECK("timer entered before stop completes against retained mapping",d.timerTicks==1 && d.fHw.doorbells==1 && d.fHw.postShutdownWrites==0);
    CHECK("admitted timer keeps stop from IRQ/queue/BAR teardown",d.fStopRetained && d.childCalls==0 && d.fHw.shutdownCalls==0 && d.fPci==&pci && d.fNetTxAdmission.drained());
    Cx3Mlx4::netTimerAction(&d,nullptr);
    CHECK("timer arriving after retained stop is refused before MMIO",d.timerTicks==1 && d.fHw.doorbells==1 && d.fHw.postShutdownWrites==0);
  }
  {
    ++cases;Cx3Mlx4 d;IOPCIDevice pci;d.fPci=&pci;d.fHw.providerView=&pci;d.lateTimerDuringDetach=true;
    d.stop(&pci);Cx3Mlx4::netTimerAction(&d,nullptr);
    CHECK("queued timer after close and after shutdown cannot enter tick",d.timerTicks==0 && d.fHw.doorbells==0 && d.fHw.postShutdownWrites==0);
    CHECK("late refused timer does not prevent a clean stop",!d.fStopRetained && d.fHw.shutdownCalls==1);
  }
  for(unsigned owner=0;owner<14;owner++){
    ++cases;Cx3Mlx4 d;IOPCIDevice pci;d.fPci=&pci;d.fHw.providerView=&pci;
    switch(owner){
      case 0:d.fQuar[0].icm=&frames;d.fQuarCount=1;break;
      case 1:d.fQuarOverflow[0]=&frames;d.fQuarOverflowCount=1;break;
      case 2:d.fIcmKept[0].icm=&table;d.fIcmKeptCount=1;break;
      case 3:d.fQuarOverflowUntracked=1;break;
      case 4:d.fIcmKeptOverflow=1;break;
      case 5:d.fQuar[2].icm=&frames;break;
      case 6:d.fQuarOverflow[2]=&frames;break;
      case 7:d.fIcmKept[2].icm=&table;break;
      case 8:d.fHw.fCmdWedged=true;d.fHw.fMboxMd=&frames;break;
      case 9:d.fHw.fCmdWedged=true;d.fHw.fMboxCmd=&frames;break;
      case 10:d.fHw.fMboxRetained=true;break;
      case 11:d.fQuarCount=1;break;
      case 12:d.fQuarOverflowCount=1;break;
      case 13:d.fIcmKeptCount=1;break;
    }
    d.stop(&pci);retained(d,pci);
    CHECK("retained ledger blocks final drain/close",d.drainCalls==0 && d.hasStopRetentionOwners());
    CHECK("preexisting retained owner stops before any child/table teardown",d.childCalls==0 && d.fHw.eqCalls==0 && d.tableCalls==0);
    d.release();pci.release();
    CHECK("local self/provider pin outlives framework reference drop",d.refs==1 && pci.refs==1);
  }
  for(unsigned transfer=0;transfer<4;transfer++){
    ++cases;Cx3Mlx4 d;IOPCIDevice pci;d.fPci=&pci;d.fHw.providerView=&pci;
    if(transfer==0)d.fFwIcm=&fw;
    if(transfer==1){d.fNetTxRing=&frames;d.fNetTxMbuf[0]=&frames;
      d.fNumTables=1;d.fTables[0].icm[0]=&table;d.fFwIcm=&fw;}
    if(transfer>=2){d.tableKeep=true;d.fNumTables=1;
      d.fFwIcm=&fw;if(transfer==2)d.fTables[0].icm[0]=&table;else d.fAuxIcm=&aux;}
    d.stop(&pci);retained(d,pci);
    CHECK("unresolved FW or transferred child retains an addressable owner",
          d.drainCalls==0 && (transfer==0 ? d.fFwIcm==&fw && !d.hasStopRetentionOwners() :
          d.hasStopRetentionOwners() && (transfer==1 ? d.fQuar[0].icm==&frames :
                        d.fIcmKept[0].icm==(transfer==2?&table:&aux))));
    CHECK("original or ledger slot survives without losing owner",
          !d.fNetTxRing && !d.fAuxIcm && d.fFwIcm==&fw &&
          (transfer==1 ? d.fTables[0].icm[0]==&table : !d.fTables[0].icm[0]));
    if(transfer==1)CHECK("frame retention stops before queue/table dependencies",d.childCalls==0 && d.fHw.eqCalls==0 && d.tableCalls==0);
  }
  for(unsigned mailbox=0;mailbox<4;mailbox++){
    ++cases;Cx3Mlx4 d;IOPCIDevice pci;d.fPci=&pci;d.fHw.providerView=&pci;
    if(mailbox&1)d.fHw.fMboxMd=&frames;
    if(mailbox&2)d.fHw.fMboxCmd=&frames;
    if(mailbox==0)d.fHw.fCmdWedged=true; // poison with no allocation is not a memory owner
    d.stop(&pci);
    CHECK("healthy mailbox or poison without allocation permits clean stop",
          !d.fStopRetained && d.fHw.shutdownCalls==1 && pci.closes==1 && !d.hasStopRetentionOwners());
  }
  for(unsigned ports=1;ports<=2;++ports){
    ++cases;Cx3Mlx4 d;IOPCIDevice pci;d.fPci=&pci;d.fHw.providerView=&pci;d.fHcaInited=true;d.fIcmHcaHolds=true;
    Mlx4Icm activeFw{6};activeFw.generation.init(1);activeFw.generation.markHandoffAttempted(1);
    d.fFwRunAttempted=true;d.fIcmMapped=true;
    d.fDevCap.num_ports=ports;d.fNumTables=1;d.fTables[0].icm[0]=&table;d.fFwIcm=&activeFw;
    d.stop(&pci);
    std::vector<std::string> expected={"port1"};if(ports==2)expected.push_back("port2");
    for(unsigned i=0;i<6;++i)expected.push_back("child");expected.push_back("hca");expected.push_back("tables");
    expected.push_back("fa");expected.push_back("free6");expected.push_back("shutdown");
    CHECK("normal close orders ports/children/HCA/tables/FW before command shutdown",d.fHw.order==expected);
    CHECK("confirmed normal FW close reaches clean provider shutdown",!d.fStopRetained && d.fHw.faCalls==1 && d.fHw.shutdownCalls==1 && pci.closes==1 && !d.fFwIcm);
    CHECK("FW close preserves RUN_FW sticky and only clears closed HCA state",!d.fHcaInited && d.fFwRunAttempted && d.fPorts[0].closeConfirmed && !d.fPorts[0].linkUp && d.fQuarCount==0);
    CHECK("one close per HCA and declared port with exact command shape",d.fHw.portCalls==ports && d.fHw.hcaCalls==1 && d.fHw.commandShapeValid);
  }
  // Composition boundary: network frame ownership is decided before any
  // port/HCA/table/FW shutdown. Other child teardowns remain explicit seams;
  // this tests stop's dependence on their postconditions, not their DMA proof.
  for(unsigned ports=1;ports<=2;++ports)for(unsigned pending:{0u,1u,4u}){
    ++cases;Cx3Mlx4 d;IOPCIDevice pci;d.fPci=&pci;d.fHw.providerView=&pci;
    Mlx4Icm activeFw{6};activeFw.generation.init(1);activeFw.generation.markHandoffAttempted(1);
    d.fHcaInited=true;d.fIcmHcaHolds=true;d.fIcmMapped=true;d.fFwRunAttempted=true;
    d.fDevCap.num_ports=ports;d.fNumTables=1;d.fTables[0].icm[0]=&table;d.fAuxIcm=&aux;d.fFwIcm=&activeFw;
    d.fNetTxRing=&frames;d.fPortQueues[0].ingressRec=&record;d.fPortQueues[0].txSq=&ring;d.fPortQueues[0].qpRec=&record;
    d.fMrState=1;d.fPortQueues[0].cqRing=&ring;d.fPortQueues[0].rqRing=&ring;d.fPortQueues[0].rawFrame=&frames;
    // The single pending frame is in the LAST slot, not always slot zero.
    for(unsigned i=0;i<pending;++i)d.fNetTxMbuf[d.kNetTxSlots-1-i]=&frames;
    d.stop(&pci);
    CHECK("network stop counts pending frames before clearing every host mbuf",d.fNetTxInFlightAtStop==pending && d.mbufFrees==pending);
    bool mbufsEmpty=true;for(auto*p:d.fNetTxMbuf)if(p)mbufsEmpty=false;
    CHECK("network mbufs are no longer host-owned after drain",mbufsEmpty && !d.fNetTxRing && !d.fNetTxLock);
    if(pending){
      retained(d,pci);
      CHECK("pending network TX survives in the exact retention slot",d.fQuarCount==1 && d.fQuar[0].icm==&frames && d.fHw.freeCalls==0);
      CHECK("pending TX blocks every port/child/HCA/table/FW teardown",d.fHw.portCalls==0 && d.childCalls==0 && d.fHw.hcaCalls==0 && d.tableCalls==0 && d.fHw.faCalls==0);
      CHECK("pending TX preserves all dependent lifecycle state",d.fHcaInited && d.fIcmHcaHolds && d.fTables[0].icm[0]==&table && d.fAuxIcm==&aux && d.fFwIcm==&activeFw);
    }else{
      CHECK("idle full owner set reaches normal stop",!d.fStopRetained && !d.hasContextOwners() && !d.hasIcmTableAllocations() && !d.hasStopRetentionOwners() && !d.fFwIcm && !d.fHcaInited);
      CHECK("idle frame and FW exact allocations are released once",d.fHw.freed==std::vector<unsigned>({3,6}));
      CHECK("idle network proceeds through declared ports and core teardown",d.fHw.portCalls==ports && d.childCalls==6 && d.fHw.hcaCalls==1 && d.tableCalls==1 && d.fHw.faCalls==1 && d.fHw.shutdownCalls==1 && pci.closes==1);
      CHECK("completed stop leaves callback gate closed and lifecycle sticky",d.fNetTxAdmission.isClosed() && d.fNetTxAdmission.drained() && d.fFwRunAttempted && d.fDeviceGeneration==1);
    }
  }
  for(unsigned mode=0;mode<3;++mode){
    ++cases;Cx3Mlx4 d;IOPCIDevice pci;d.fPci=&pci;d.fHw.providerView=&pci;d.fHcaInited=true;d.fIcmHcaHolds=true;
    d.fNumTables=1;d.fTables[0].icm[0]=&table;d.fAuxIcm=&aux;d.fFwIcm=&fw;
    d.fHw.hcaOk=mode==1;d.fHw.hcaStatus=mode==0?7:mode==1?9:0xffffffffu;
    d.stop(&pci);retained(d,pci);
    CHECK("failed/unknown/inconsistent close preserves initialized and mapping flags",d.fHcaInited && d.fIcmHcaHolds);
    CHECK("HCA close failure blocks all table/FW operations",d.fHw.hcaCalls==1 && d.tableCalls==0 && d.fTables[0].icm[0]==&table && d.fAuxIcm==&aux && d.fFwIcm==&fw && d.fHw.freeCalls==0 && d.retainedBlocks==0);
  }
  for(unsigned mode=0;mode<7;++mode){
    ++cases;Cx3Mlx4 d;IOPCIDevice pci;d.fPci=&pci;d.fHw.providerView=&pci;d.fHcaInited=true;
    d.fNumTables=1;d.fTables[0].icm[0]=&table;d.fFwIcm=&fw;
    if(mode<2)d.fHw.portFail=mode+1;
    if(mode==2)d.fDevCap.num_ports=0;
    if(mode==3)d.fDevCap.num_ports=3;
    if(mode==4)d.fResetCommandsBlocked=true;
    if(mode==5)d.fHw.fCmdWedged=true;
    if(mode==6)d.fHw.portErrorWithTrue=true;
    d.stop(&pci);retained(d,pci);
    CHECK("port/metadata/admission failure blocks children/HCA/tables",d.childCalls==0 && d.fHw.hcaCalls==0 && d.tableCalls==0 && d.fFwIcm==&fw && d.fTables[0].icm[0]==&table);
    CHECK("only permitted prefix of port commands ran",d.fHw.portCalls==(mode<2?mode+1:mode==6?1:0));
    CHECK("failed port1 is not advertised closed",(mode!=0 && mode!=6) || !d.fPorts[0].closeConfirmed);
  }
  {
    ++cases;Cx3Mlx4 d;IOPCIDevice pci;d.fPci=&pci;d.fHw.providerView=&pci;d.fHcaInited=true;d.fPorts[0].closeConfirmed=true;
    d.fPortQueues[0].qpRec=&record;d.fail="qp";d.stop(&pci);retained(d,pci);
    CHECK("already closed first port skipped; live child blocks CLOSE_HCA",d.fHw.portCalls==1 && d.fHw.order[0]=="port2" && d.fHw.hcaCalls==0 && d.fHcaInited);
  }
  for(unsigned mode=0;mode<3;++mode){
    ++cases;Cx3Mlx4 d;d.fHcaInited=true;d.fIcmHcaHolds=true;d.fPorts[0].closeConfirmed=d.fPorts[1].closeConfirmed=true;d.fNetEnabled=false;
    if(mode<2)d.fNetTxAdmission.close();
    if(mode==1){d.fNetTxAdmission.init();d.fNetTxAdmission.enter();d.fNetTxAdmission.close();}
    bool result=d.closeHcaForStop("isolated helper");
    CHECK("HCA close requires closed and drained callback gate",result==(mode==0) && d.fHw.hcaCalls==(mode==0?1u:0u));
    CHECK("close does not erase current-instance mapping knowledge",d.fIcmHcaHolds && d.tableCalls==0 && d.fHw.freeCalls==0);
  }
  {
    ++cases;Cx3Mlx4 d;d.fHcaInited=true;d.fNetEnabled=false;d.fNetTxAdmission.close();
    d.fPorts[0].closeConfirmed=true;
    CHECK("port2 missing close receipt independently blocks shared HCA",!d.closeHcaForStop("missing port2") && d.fHw.hcaCalls==0);
  }
  for(unsigned port=1;port<=2;++port)for(bool accepted:{false,true}){
    ++cases;Cx3Mlx4 d;d.fPorts[0].closeConfirmed=d.fPorts[1].closeConfirmed=true;d.fHw.initOk=accepted;
    d.initializePortStage(port);
    CHECK("INIT_PORT attempt invalidates only that port's close receipt even on unknown failure",d.fPorts[0].closeConfirmed==(port!=1) && d.fPorts[1].closeConfirmed==(port!=2));
    CHECK("production INIT_PORT stage used actual wrapper exactly once",d.fHw.initCalls==1 && d.fHw.commandShapeValid);
  }
  for(unsigned mode=0;mode<5;++mode){
    ++cases;Cx3Mlx4 d;IOPCIDevice pci;d.fPci=&pci;d.fHw.providerView=&pci;Mlx4Icm activeFw{6};
    activeFw.generation.init(1);activeFw.generation.markHandoffAttempted(1);
    d.fFwIcm=&activeFw;d.fFwRunAttempted=true;d.fHw.faOk=mode==1 || mode==4;
    d.fHw.faStatus=mode==0?7:mode==1?9:mode==2?CX3_FWSTATUS_TIMEOUT:mode==3?0:CX3_FWSTATUS_NOT_POSTED;
    if(mode==2){d.fHw.poisonOnFa=true;d.fHw.fMboxMd=&frames;}
    d.stop(&pci);retained(d,pci);
    CHECK("failed/unknown/inconsistent FW close preserves exact slot and sticky state",d.fFwIcm==&activeFw && d.fFwRunAttempted && activeFw.generation.matchesOwnerGeneration(1));
    CHECK("failed FW close neither frees nor transfers owner",d.fHw.faCalls==1 && d.fHw.freeCalls==0 && d.retainedBlocks==0 && d.drainCalls==0);
    CHECK("FW command ran before hardware shutdown with exact shape",d.fHw.commandShapeValid && d.fHw.shutdownCalls==0);
    if(mode==2)CHECK("FW timeout preserves command poison and mailbox dependency",d.fHw.fCmdWedged && d.fHw.fMboxMd==&frames);
  }
  for(unsigned mode=0;mode<3;++mode){
    ++cases;Cx3Mlx4 d;IOPCIDevice pci;d.fPci=&pci;d.fHw.providerView=&pci;Mlx4Icm activeFw{6};
    activeFw.generation.init(1);if(mode)activeFw.generation.markHandoffAttempted(1);
    d.fFwRunAttempted=mode==2;d.fFwIcm=&activeFw;d.stop(&pci);
    CHECK("host-only/mapped/running FW all reach clean stop",!d.fStopRetained && !d.fFwIcm && d.fHw.shutdownCalls==1 && pci.closes==1 && d.fHw.freeCalls==1 && d.fHw.freed[0]==6);
    CHECK("only handed-off FW sends UNMAP_FA",d.fHw.faCalls==(mode?1u:0u) && d.retainedBlocks==0 && d.fHw.commandShapeValid);
    CHECK("normal FW close does not authorize same-controller RUN_FW reuse",d.fFwRunAttempted==(mode==2) && d.fDeviceGeneration==1 && d.fNetTxAdmission.isClosed());
  }
  for(unsigned mode=0;mode<20;++mode){
    ++cases;Cx3Mlx4 d;Mlx4Icm activeFw{6};activeFw.generation.init(1);activeFw.generation.markHandoffAttempted(1);
    d.fFwIcm=&activeFw;d.fNetEnabled=false;d.fNetTxAdmission.close();
    switch(mode){
      case 0:d.fHcaInited=true;break;case 1:d.fIcmMapped=true;break;
      case 2:d.fIcmHcaHolds=true;break;case 3:d.fPortQueues[0].qpRec=&record;break;
      case 4:d.fTables[2].icm[2]=&table;break;case 5:d.fAuxIcm=&aux;break;
      case 6:d.fQuar[2].icm=&frames;break;case 7:d.fResetCommandsBlocked=true;break;
      case 8:d.fHw.fCmdWedged=true;break;case 9:activeFw.allocationId=0;break;
      case 10:activeFw.generation.allocationGeneration=0;break;
      case 11:activeFw.generation.allocationGeneration=2;break;
      case 12:activeFw.generation.ownerGeneration=2;break;
      case 13:activeFw.generation.ownerGeneration=0;break;
      case 14:d.fNetTxAdmission.init();break;
      case 15:d.fNetTxAdmission.init();d.fNetTxAdmission.enter();d.fNetTxAdmission.close();break;
      case 16:d.fNetEnabled=true;break;
      case 17:activeFw.generation.init(1);d.fFwRunAttempted=true;break;
      case 18:activeFw.generation.init(1);activeFw.generation.ownerGeneration=1;break;
      case 19:d.fHw.abRetained=true;break;
    }
    CHECK("FW close refuses unresolved dependency or lifecycle metadata",!d.closeFwForStop("guard"));
    CHECK("guard refusal has no command/free/owner transfer",d.fHw.faCalls==0 && d.fHw.freeCalls==0 && d.fFwIcm==&activeFw && d.retainedBlocks==0);
  }
  {
    ++cases;Cx3Mlx4 d;CHECK("empty FW slot is a no-op",d.closeFwForStop("empty") && d.fHw.faCalls==0 && d.fHw.freeCalls==0);
  }
  printf("stop composition: %u cases, %u checks, %u failures (production stop/EQ/TX stop over host seams)\n",cases,checks,failures);
  return failures?1:0;
}
'''.replace('METHODS', methods).replace('TABLE_INIT',table_init).replace('INIT_PORT_STAGE',init_port_stage)
    with tempfile.TemporaryDirectory(prefix='cx3-stop-composition-') as temporary:
        exe = str(Path(temporary)/'test')
        subprocess.run(['/usr/bin/clang++','-std=c++17','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                        '-I',str(ROOT/'src'),'-x','c++','-','-o',exe],
                       input=code,text=True,check=True)
        return subprocess.run([exe],timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
