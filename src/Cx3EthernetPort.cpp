/* Copyright (c) 2026 Hi-Jiajun and contributors. OpenIB BSD; see LICENSE.
 * Native per-port RX/TX follows the existing PF network data path and the same
 * Mlx4Hw primitives. Fixed fPort is never switched to dispatch a callback. */
#include "Cx3EthernetPort.h"
#include "Cx3Mlx4.h"
#include "Cx3Debug.h"
#include <IOKit/IOWorkLoop.h>
#include <IOKit/network/IOOutputQueue.h>
#include <IOKit/network/IONetworkMedium.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <libkern/OSByteOrder.h>
#include <sys/kpi_mbuf.h>
OSDefineMetaClassAndStructors(Cx3EthernetPort,IOEthernetController)
static UInt32 cx3GetBe32(const void*p){const UInt8*b=(const UInt8*)p;return UInt32(b[0])<<24|UInt32(b[1])<<16|UInt32(b[2])<<8|b[3];}
static UInt16 cx3GetBe16(const void*p){const UInt8*b=(const UInt8*)p;return UInt16(UInt16(b[0])<<8|b[1]);}
static UInt64 cx3WakeInTicks(UInt64 ns){UInt64 ticks=0,now=0;nanoseconds_to_absolutetime(ns,&ticks);clock_get_uptime(&now);return now+ticks;}
static UInt64 cx3PortNowNs(){UInt64 ticks=0,ns=0;clock_get_uptime(&ticks);absolutetime_to_nanoseconds(ticks,&ns);return ns;}
bool Cx3EthernetPort::init(OSDictionary*d){fNetTxAdmission.init();return super::init(d);}
bool Cx3EthernetPort::bind(Cx3Mlx4*c,UInt32 p,UInt64 gen,const UInt8*mac){
 if(fCore||!c||p<1||p>2||!gen||!mac)return false;
 fCore=c;c->retain();fPort=p;fGeneration=gen;fNetTxSlotCount=c->fPortQueues[p-1].txSqNent;memcpy(fMac,mac,6);
 fNetTxBufBytes=c->fPortQueues[p-1].rqBufBytes;
 UInt32 wire=c->fPorts[p-1].frameMtu;
 if(wire<1522u||fNetTxBufBytes<wire+MLX4_NET_IP_ALIGN)return false;
 fPacketCapacity=wire-MLX4_ETH_VLAN_LEN;
 // mlx4_en_netdev.c:2247: Ethernet HWCSUM/HWCSUM_IPV6; the existing WQE
 // and validated RX CQE paths below implement the advertised family contract.
 fNetRxCsum=c->fPortQueues[p-1].nativeRxCsum;
 fNetTxCsum=c->fPortQueues[p-1].nativeTxCsum;return true;
}
bool Cx3EthernetPort::epochValid()const{
 return fCore&&fGeneration==fCore->fDeviceGeneration&&!fCore->selectorStopping()&&
  !fCore->fResetCommandsBlocked&&!fCore->fStopRetained&&fCore->fPortQueues[fPort-1].setupReady;
}
IOWorkLoop*Cx3EthernetPort::getWorkLoop()const{return fWorkLoop;}
bool Cx3EthernetPort::createWorkLoop(){if(!fWorkLoop)fWorkLoop=IOWorkLoop::workLoop();return fWorkLoop!=nullptr;}
bool Cx3EthernetPort::start(IOService*p){
 if(!fCore||p!=fCore||!super::start(p))return false;
 fStarted=true;fWorkerLock=IOLockAlloc();fNetTxLock=IOLockAlloc();fFilterLock=IOLockAlloc();
 fWorker=thread_call_allocate(&Cx3EthernetPort::worker,this);
 fLinkLock=IOLockAlloc();fLinkCall=thread_call_allocate(&Cx3EthernetPort::linkWorker,this);
 if(!fWorkerLock||!fNetTxLock||!fFilterLock||!fWorker||!fLinkLock||!fLinkCall)return false;
 retain();fLinkRef=true; // pinned until cancel/drain AND thread_call_free succeed
 fMediaDict=OSDictionary::withCapacity(1);
 fMediumAuto=IONetworkMedium::medium((IOMediumType)kIOMediumEthernetAuto,0,0,0,nullptr);
 if(!fMediaDict||!fMediumAuto||!IONetworkMedium::addMedium(fMediaDict,fMediumAuto)||
    !publishMediumDictionary(fMediaDict)||!setSelectedMedium(fMediumAuto))return false;
 setProperty("cx3PhysicalPort",fPort,32);setProperty("cx3DeviceGeneration",fGeneration,64);
 if(!netServiceStart("native port"))return false;
 return true;
}
void Cx3EthernetPort::stop(IOService*p){
 closeAdmission();if(fStarted){fStarted=false;super::stop(p);}
}
void Cx3EthernetPort::closeAdmission(){fNetTxAdmission.close();fNetEnabled=false;stopLinkMonitoring();}
IOReturn Cx3EthernetPort::getHardwareAddress(IOEthernetAddress*a){if(!a||!fCore)return kIOReturnNotReady;memcpy(a->bytes,fMac,6);return 0;}
UInt32 Cx3EthernetPort::getFeatures()const{
 // Both CQE-stripped and inline C-tags are normalized to family metadata.
 return super::getFeatures()|kIONetworkFeatureHardwareVlan;
}
IOReturn Cx3EthernetPort::getChecksumSupport(UInt32*m,UInt32 family,bool output){
 if(!m)return kIOReturnBadArgument;*m=0;
 if(family!=kChecksumFamilyTCPIP)return kIOReturnUnsupported;
 // RX policy controls marking only, so retain its capability across toggles.
 if(!output||fNetTxCsum)*m=kChecksumIP|kChecksumTCP|kChecksumUDP|kChecksumTCPIPv6|kChecksumUDPIPv6;
 return kIOReturnSuccess;
}
IOReturn Cx3EthernetPort::getMaxPacketSize(UInt32*size)const{
 if(!size)return kIOReturnBadArgument;
 *size=fPacketCapacity;return fPacketCapacity?kIOReturnSuccess:kIOReturnNotReady;
}
IOReturn Cx3EthernetPort::setMaxPacketSize(UInt32 size){
 if(size<64u||size>fPacketCapacity)return kIOReturnBadArgument;
 if(!fNetTxAdmission.enter())return kIOReturnNotPermitted;
 struct Exit{Mlx4TxAdmissionGate*g;~Exit(){g->leave();}}exit{&fNetTxAdmission};
 if(!epochValid()||fPoisoned)return kIOReturnNotReady;
 if(!fNetTxLock||!IOLockTryLock(fNetTxLock))return kIOReturnBusy;
 // Hardware ceiling and DMA geometry were established before attach. Both
 // paths enforce this current size; no command/resize/reset runs on the gate.
 fPacketSize=size;IOLockUnlock(fNetTxLock);
 setProperty("cx3CurrentPacketSize",size,32);
 return kIOReturnSuccess;
}
IOReturn Cx3EthernetPort::selectMedium(const IONetworkMedium*m){
 if(!m||(m!=fMediumAuto&&m!=fMediumActive))return kIOReturnBadArgument;
 if(!fNetTxAdmission.enter())return kIOReturnNotPermitted;
 // auto-current is an observation, never a request to force a guessed PHY mode.
 bool ok=epochValid()&&setSelectedMedium(fMediumAuto);fNetTxAdmission.leave();
 return ok?kIOReturnSuccess:kIOReturnNotReady;
}
void Cx3EthernetPort::publishLinkStatus(const char*){
 if(!fNetIface)return;
 const bool valid=fNetEnabled&&fLinkKnown&&!fPoisoned&&!fNetTxAdmission.isClosed();
 const bool active=valid&&fLinkUp;
 const UInt64 speed=active?UInt64(fLinkSpeedMbps)*1000000ull:0;
 IONetworkMedium*oldMedium=nullptr;OSDictionary*oldDict=nullptr;
 if(speed&&(!fMediumActive||fMediumSpeed!=speed)){
  auto*medium=IONetworkMedium::medium((IOMediumType)kIOMediumEthernetAuto,speed,0,1,"auto-current");
  auto*dict=medium?OSDictionary::withCapacity(2):nullptr;
  if(dict&&IONetworkMedium::addMedium(dict,fMediumAuto)&&IONetworkMedium::addMedium(dict,medium)&&publishMediumDictionary(dict)){
   oldMedium=fMediumActive;oldDict=fMediaDict;fMediumActive=medium;fMediaDict=dict;fMediumSpeed=speed;
  }else{if(dict)dict->release();if(medium)medium->release();}
 }
 const auto*medium=speed&&fMediumActive&&fMediumSpeed==speed?fMediumActive:fMediumAuto;
 setLinkStatus((valid?kIONetworkLinkValid:0)|(active?kIONetworkLinkActive:0),medium,speed);
 if(oldMedium)oldMedium->release();if(oldDict)oldDict->release();
}

void Cx3EthernetPort::startLinkMonitoring()
{
 if(!fLinkLock||!fLinkCall)return;
 IOLockLock(fLinkLock);++fLinkEpoch;fLinkMonitoring=true;fLinkHaveResult=false;fLinkNextNs=0;
 fLinkQueryStatus=kIOReturnNotReady;fLinkFwStatus=CX3_FWSTATUS_NOT_POSTED;IOLockUnlock(fLinkLock);
 fLinkKnown=fLinkUp=false;fLinkSpeedMbps=0;fLinkLastNs=0;
}
void Cx3EthernetPort::stopLinkMonitoring()
{
 if(!fLinkLock)return;
 IOLockLock(fLinkLock);fLinkMonitoring=false;++fLinkEpoch;fLinkHaveResult=false;IOLockUnlock(fLinkLock);
 // Non-waiting cancellation: an executing query retains its pin and PF admission.
 // Its old epoch cannot update the cache or publish after down/up/stop.
 if(fLinkCall&&thread_call_cancel(fLinkCall)){
  IOLockLock(fLinkLock);fLinkInFlight=false;IOLockUnlock(fLinkLock);
 }
}
Cx3EthernetPort::LinkObservation Cx3EthernetPort::queryLink(UInt64 epoch)
{
 LinkObservation r;r.epoch=epoch;
 if(!fNetTxAdmission.enter())return r;
 struct Host{Mlx4TxAdmissionGate*g;~Host(){g->leave();}}host{&fNetTxAdmission};
 if(!fCore->selectorEnter())return r;
 struct Admission{Cx3Mlx4*c;~Admission(){c->selectorExit();}}admission{fCore};
 if(!fCore->transactionTryEnter()){r.status=kIOReturnBusy;return r;}
 struct Transaction{Cx3Mlx4*c;~Transaction(){c->transactionExit();}}transaction{fCore};
 IOLockLock(fLinkLock);bool current=fLinkMonitoring&&epoch==fLinkEpoch;IOLockUnlock(fLinkLock);
 if(!current||!epochValid()||fPoisoned||fNetTxAdmission.isClosed()||fCore->fQuarExhausted||fCore->fHw.cmdWedged())return r;
 Cx3QueryPortResult q{};r.posted=true;const bool ok=fCore->fHw.queryPort(fPort,&q);
 r.stampNs=cx3PortNowNs();r.fwStatus=q.fwStatus;r.status=kIOReturnError;
 IOLockLock(fLinkLock);
 current=fLinkMonitoring&&epoch==fLinkEpoch&&!fNetTxAdmission.isClosed()&&epochValid();
 if(current){
  // recordPortQuery requires the same PF transaction as selectors. It rejects
  // bad rawLen/status; a previous successful cache is not a new observation.
  if(!ok&&!q.status)q.status=(UInt32)kIOReturnError;
  const bool observed=fCore->recordPortQuery(fPort,q);
  if(ok&&observed){r.valid=true;r.up=q.linkState!=0;r.speedMbps=cx3LinkSpeedMbps(q.linkSpeed);r.status=kIOReturnSuccess;}
 }
 IOLockUnlock(fLinkLock);return r;
}
void Cx3EthernetPort::linkWorker(thread_call_param_t a,thread_call_param_t)
{
 auto*self=(Cx3EthernetPort*)a;
 IOLockLock(self->fLinkLock);const UInt64 epoch=self->fLinkRequestEpoch;IOLockUnlock(self->fLinkLock);
 const LinkObservation r=self->queryLink(epoch);
 IOLockLock(self->fLinkLock);
 if(epoch==self->fLinkEpoch&&self->fLinkMonitoring&&!self->fNetTxAdmission.isClosed()){
  self->fLinkQueryStatus=r.status;self->fLinkFwStatus=r.fwStatus;
  if(r.posted)++self->fLinkQueries;
  if(r.status==kIOReturnBusy)++self->fLinkBusy;
  else{
   self->fLinkResult=r;self->fLinkHaveResult=true;
   if(r.status){++self->fLinkFailures;self->fLinkMonitoring=false;}
  }
 }
 self->fLinkInFlight=false; // last local-resource access is the lock release below
 IOLockUnlock(self->fLinkLock);
}
void Cx3EthernetPort::serviceLink()
{
 if(!fLinkLock||!fLinkCall)return;
 const UInt64 now=cx3PortNowNs();LinkObservation r;bool have=false,queue=false,running=false,inFlight=false;
 IOReturn status;UInt32 fw;UInt64 queries,busy,failures,epoch;
 IOLockLock(fLinkLock);
 if(fLinkHaveResult){r=fLinkResult;fLinkHaveResult=false;have=r.epoch==fLinkEpoch;}
 if(fLinkMonitoring&&fNetEnabled&&!fLinkInFlight&&!fNetTxAdmission.isClosed()&&epochValid()&&now>=fLinkNextNs){
  fLinkInFlight=true;fLinkRequestEpoch=fLinkEpoch;fLinkNextNs=now+1000000000ull;queue=true;
 }
 running=fLinkMonitoring;inFlight=fLinkInFlight;status=fLinkQueryStatus;fw=fLinkFwStatus;
 queries=fLinkQueries;busy=fLinkBusy;failures=fLinkFailures;epoch=fLinkEpoch;IOLockUnlock(fLinkLock);
 bool changed=false;
 if(have){
  fLinkKnown=r.valid&&!r.status;fLinkUp=fLinkKnown&&r.up;fLinkSpeedMbps=fLinkKnown?r.speedMbps:0;
  if(fLinkKnown)fLinkLastNs=r.stampNs;changed=true;
 }
 if(fLinkKnown&&(!epochValid()||fNetTxAdmission.isClosed()||now-fLinkLastNs>=3000000000ull)){
  fLinkKnown=fLinkUp=false;fLinkSpeedMbps=0;changed=true;
 }
 if(changed)publishLinkStatus("periodic observation");
 // CQ polling is 1kHz; registry allocation/notification is only on an observed
 // change or the 1Hz query cycle, never twelve property writes per packet tick.
 if(queue||have||changed){
  setProperty("cx3LinkObservationValid",fLinkKnown);setProperty("cx3LinkObservedUp",fLinkKnown&&fLinkUp);
  setProperty("cx3LinkObservedSpeedMbps",fLinkKnown?fLinkSpeedMbps:0,32);setProperty("cx3LinkObservationUptimeNs",fLinkLastNs,64);
  setProperty("cx3LinkObservationEpoch",epoch,64);setProperty("cx3LinkQueryStatus",(UInt64)status,64);
  setProperty("cx3LinkQueryFwStatus",fw,32);setProperty("cx3LinkQueries",queries,64);
  setProperty("cx3LinkBusy",busy,64);setProperty("cx3LinkFailures",failures,64);
  setProperty("cx3LinkMonitorRunning",running);setProperty("cx3LinkQueryInFlight",inFlight);
 }
 if(queue)thread_call_enter(fLinkCall); // never wait for firmware on the workloop
}
IOReturn Cx3EthernetPort::attachAction(OSObject*o,void*a,void*,void*,void*){
 auto*self=OSDynamicCast(Cx3EthernetPort,o);if(!self||!a)return kIOReturnBadArgument;
 bool*ok=(bool*)a;*ok=self->attachInterface(&self->fNetIface,false);return *ok?0:kIOReturnError;
}
void Cx3EthernetPort::worker(thread_call_param_t a,thread_call_param_t){
 auto*self=(Cx3EthernetPort*)a;bool ok=false;
 if(self->fWorkerKind==1){
  auto*loop=self->getWorkLoop();if(loop)loop->runAction(&Cx3EthernetPort::attachAction,self,&ok);
  if(ok&&self->fNetIface){self->fNetIface->registerService();self->fNetUnit=self->fNetIface->getUnitNumber();}
 }else if(self->fWorkerKind==2){
  if(self->fNetIface){self->detachInterface(self->fNetIface,true);self->fNetIface=nullptr;}ok=true;
 }else if(self->fWorkerKind==3){
  self->fWorkerResult=self->programFilters();ok=self->fWorkerResult==kIOReturnSuccess;
  if(!ok&&self->fFilterPosted)self->fPoisoned=true;
 }else if(self->fWorkerKind==4){
  self->fWorkerResult=self->programMac();ok=self->fWorkerResult==kIOReturnSuccess;
  if(!ok&&self->fFilterPosted)self->fPoisoned=true;
 }
 IOLockLock(self->fWorkerLock);self->fWorkerOk=ok;self->fWorkerDone=true;self->fWorkerInFlight=false;IOLockUnlock(self->fWorkerLock);
}
bool Cx3EthernetPort::runWorker(UInt32 kind){
 if(!fWorker||!fWorkerLock)return false;
 IOLockLock(fWorkerLock);
 if(fWorkerInFlight){IOLockUnlock(fWorkerLock);return false;}
 fWorkerKind=kind;fWorkerDone=fWorkerOk=false;fWorkerInFlight=true;
 if(!fWorkerRef){retain();fWorkerRef=true;}
 IOLockUnlock(fWorkerLock);
 if(thread_call_enter(fWorker)){fPoisoned=true;return false;}
 for(UInt32 ms=0;ms<2000;++ms){
  IOLockLock(fWorkerLock);bool done=fWorkerDone,ok=fWorkerOk;IOLockUnlock(fWorkerLock);
  if(done){if(!ok&&kind!=3&&kind!=4)fPoisoned=true;return ok;}IOSleep(1);
 }
 fPoisoned=true;closeAdmission();return false;
}
bool Cx3EthernetPort::attachAndWait(){return !fPoisoned&&runWorker(1);}
bool Cx3EthernetPort::shutdown(){
 if(fDetached)return true;fPoisoned=true;closeAdmission();
 for(UInt32 ms=0;!fNetTxAdmission.drained()&&ms<2000;++ms)IOSleep(1);
 if(!fNetTxAdmission.drained())return false;
 if(fLinkLock){IOLockLock(fLinkLock);bool busy=fLinkInFlight;IOLockUnlock(fLinkLock);if(busy)return false;}
 if(fLinkCall&&!thread_call_free(fLinkCall))return false;fLinkCall=nullptr;
 if(!netTimerRelease("native shutdown"))return false;
 if(fWorkerLock){IOLockLock(fWorkerLock);bool busy=fWorkerInFlight;IOLockUnlock(fWorkerLock);if(busy)return false;}
 if(fNetIface&&!runWorker(2))return false;
 if(fWorker&&!thread_call_free(fWorker))return false;fWorker=nullptr;
 if(fWorkerRef){fWorkerRef=false;release();}
 if(fLinkRef){fLinkRef=false;release();}
 // All retained mbufs have already been copied into PF-owned TX frame storage.
 // Releasing source mbufs after host drain says nothing about the DMA buffer.
 for(UInt32 i=0;i<kNetTxSlots;++i)if(fNetTxMbuf[i]){freePacket(fNetTxMbuf[i]);fNetTxMbuf[i]=nullptr;}
 fDetached=true;return true;
}
void Cx3EthernetPort::free(){
 // A non-quiesced instance remains owned by its PF; free is never a DMA release path.
 if(fWorkerLock)IOLockFree(fWorkerLock);if(fNetTxLock)IOLockFree(fNetTxLock);
 if(fFilterLock)IOLockFree(fFilterLock);
 if(fLinkLock)IOLockFree(fLinkLock);
 if(fMediumActive)fMediumActive->release();if(fMediumAuto)fMediumAuto->release();if(fMediaDict)fMediaDict->release();
 auto*core=fCore;auto*loop=fWorkLoop;super::free();if(loop)loop->release();if(core)core->release();
}
void Cx3EthernetPort::snapshot(Cx3NativePortResult*r){
 if(fNetTxLock)IOLockLock(fNetTxLock);
 r->flags=CX3_NATIVE_PRESENT|(fNetIface?CX3_NATIVE_ATTACHED:0)|(fNetEnabled?CX3_NATIVE_ENABLED:0)|
  (fPoisoned?CX3_NATIVE_POISONED:0)|(fNetTxFault.latched?CX3_NATIVE_TX_FAULT:0)|
  (fNetRxFault?CX3_NATIVE_RX_FAULT:0);
 r->interfaceUnit=fNetUnit;r->rxFrames=fNetRxFrames;r->rxBytes=fNetRxBytes;r->rxDropped=fNetRxDropped;
 r->txFrames=fNetTxFrames;r->txCompleted=fNetTxCompleted;r->txDropped=fNetTxDropped;
 r->rxErrors=fNetRxErrors;r->txErrors=fNetTxErrors;r->ticks=fNetTicks;r->skipped=fNetTicksSkipped;
 memcpy(r->mac,fMac,6);if(fNetTxLock)IOLockUnlock(fNetTxLock);
}

void Cx3EthernetPort::publishFilterState()
{
    const bool pending=!fFilterPrograms || fFilterDesired.promisc!=fFilterApplied.promisc ||
        fFilterDesired.multicast!=fFilterApplied.multicast || fFilterDesired.overflow!=fFilterApplied.overflow ||
        fFilterDesired.count!=fFilterApplied.count ||
        memcmp(fFilterDesired.addresses,fFilterApplied.addresses,sizeof(fFilterDesired.addresses));
    setProperty("cx3FilterPending",pending);
    setProperty("cx3FilterStatus",(UInt64)fFilterStatus,64);
    setProperty("cx3FilterFwStatus",fFilterFwStatus,32);
    setProperty("cx3FilterAppliedRevision",fFilterPrograms,64);
    setProperty("cx3FilterRequestedMulticastCount",fFilterDesired.count,32);
    setProperty("cx3FilterAppliedMulticastCount",fFilterApplied.count,32);
    setProperty("cx3FilterAppliedPromiscuous",fFilterApplied.promisc);
    setProperty("cx3FilterAppliedAllMulticast",fFilterApplied.multicast&&fFilterApplied.overflow);
}

IOReturn Cx3EthernetPort::changeFilters(UInt32 operation,bool active,IOEthernetAddress*addresses,
                                      UInt32 count,bool force)
{
    if(operation>kFilterList || (operation==kFilterList&&count&&!addresses))return kIOReturnBadArgument;
    if(!fNetTxAdmission.enter())return kIOReturnNotPermitted;
    struct Exit {Mlx4TxAdmissionGate*g;~Exit(){g->leave();}} exit{&fNetTxAdmission};
    if(!epochValid()||fPoisoned)return kIOReturnNotReady;
    if(!fFilterLock||!IOLockTryLock(fFilterLock))return kIOReturnBusy;
    struct Unlock {IOLock*l;~Unlock(){IOLockUnlock(l);}} unlock{fFilterLock};
    FilterState previous=fFilterDesired;
    if(operation==kFilterPromisc)fFilterDesired.promisc=active;
    if(operation==kFilterMulticast)fFilterDesired.multicast=active;
    if(operation==kFilterList){
        // A finite hardware table may accept all groups when membership exceeds
        // capacity (IOEthernetController's documented last-resort behavior).
        if(count<=kFilterMax)for(UInt32 i=0;i<count;++i)
            if(!(addresses[i].bytes[0]&1u))return kIOReturnBadArgument;
        fFilterDesired.count=count;fFilterDesired.overflow=count>kFilterMax;
        memset(fFilterDesired.addresses,0,sizeof(fFilterDesired.addresses));
        if(!fFilterDesired.overflow)for(UInt32 i=0;i<count;++i)memcpy(fFilterDesired.addresses[i],addresses[i].bytes,6);
    }
    if(!fNetEnabled&&!force){fFilterStatus=kIOReturnSuccess;publishFilterState();return kIOReturnSuccess;}
    fFilterPosted=false;fWorkerResult=kIOReturnNotReady;
    const bool ok=runWorker(3);
    // A timeout leaves a real worker alive. Do not sample its partially written
    // applied state/firmware status or overwrite its desired input on that path.
    IOLockLock(fWorkerLock);const bool done=fWorkerDone;
    const IOReturn result=done?fWorkerResult:kIOReturnBusy;
    const bool posted=done&&fFilterPosted;IOLockUnlock(fWorkerLock);
    fFilterStatus=ok?kIOReturnSuccess:(!done&&fPoisoned?kIOReturnTimeout:result);
    if(!ok&&!fPoisoned&&!posted&&fFilterStatus==kIOReturnNotReady)fFilterStatus=kIOReturnBusy;
    if(!ok&&!posted&&!fPoisoned)fFilterDesired=previous;
    if(fPoisoned){closeAdmission();if(fNetTimer)fNetTimer->cancelTimeout();
        if(fNetIface)setLinkStatus(kIONetworkLinkValid,nullptr,0);}
    if(done||!fPoisoned)publishFilterState();
    else {setProperty("cx3FilterPending",true);setProperty("cx3FilterStatus",(UInt64)fFilterStatus,64);}
    return fFilterStatus;
}

IOReturn Cx3EthernetPort::setPromiscuousMode(bool active)
{
    return changeFilters(kFilterPromisc,active,nullptr,0);
}
IOReturn Cx3EthernetPort::setMulticastMode(bool active)
{
    return changeFilters(kFilterMulticast,active,nullptr,0);
}
IOReturn Cx3EthernetPort::setMulticastList(IOEthernetAddress*addresses,UInt32 count)
{
    return changeFilters(kFilterList,false,addresses,count);
}

IOReturn Cx3EthernetPort::programFilters()
{
    // This worker is a real PF lifecycle participant. stop must observe it even
    // though no UserClient selector started the operation.
    fFilterFwStatus=CX3_FWSTATUS_NOT_POSTED;
    if(!fCore->selectorEnter())return kIOReturnNotReady;
    struct Admission {Cx3Mlx4*c;~Admission(){c->selectorExit();}} admission{fCore};
    if(!fCore->transactionTryEnter())return kIOReturnBusy;
    struct Transaction {Cx3Mlx4*c;~Transaction(){c->transactionExit();}} transaction{fCore};
    if(!epochValid()||fPoisoned||fNetTxAdmission.isClosed()||fCore->fQuarExhausted||fCore->fHw.cmdWedged())
        return kIOReturnNotReady;
    auto&q=fCore->fPortQueues[fPort-1];Cx3QueryPortResult port{};
    if(!q.macUp||!q.numbersReserved||q.qpQpn!=q.numbers.rxQpn)return kIOReturnNotReady;
    if(!fCore->fHw.queryPort(fPort,&port)){fFilterFwStatus=port.fwStatus;return kIOReturnError;}
    fFilterFwStatus=port.fwStatus;
    if(port.fwStatus||port.logMaxMacs>=24u||(1u<<port.logMaxMacs)!=q.numbers.macSlots)
        return kIOReturnNotReady;
    // A0 steering: mlx4_en_netdev.c:741-936 and mlx4_port.c:1578-1612.
    const UInt32 multicast=(fCore->fDevCap.flags&MLX4_DEV_CAP_FLAG_VEP_MC_STEER)?MCAST_DIRECT:MCAST_DEFAULT;
    fFilterPosted=true;
    if(!fCore->fHw.setPortQpnCalc(fPort,q.numbers.macBase,fFilterDesired.promisc?1u:0u,
        port.logMaxMacs,multicast,&fFilterFwStatus)||fFilterFwStatus)return kIOReturnError;
    if(!fCore->fHw.setMcastFltr(fPort,0,false,MLX4_MCAST_DISABLE,&fFilterFwStatus)||fFilterFwStatus)
        return kIOReturnError;
    if(!fFilterDesired.promisc&&!(fFilterDesired.multicast&&fFilterDesired.overflow)){
        if(!fCore->fHw.setMcastFltr(fPort,0xffffffffffffull,true,MLX4_MCAST_CONFIG,&fFilterFwStatus)||fFilterFwStatus)
            return kIOReturnError;
        if(fFilterDesired.multicast)for(UInt32 i=0;i<fFilterDesired.count;++i){
            UInt64 mac=0;for(UInt32 k=0;k<6;++k)mac=(mac<<8)|fFilterDesired.addresses[i][k];
            if(!fCore->fHw.setMcastFltr(fPort,mac,false,MLX4_MCAST_CONFIG,&fFilterFwStatus)||fFilterFwStatus)
                return kIOReturnError;
        }
        if(!fCore->fHw.setMcastFltr(fPort,0,false,MLX4_MCAST_ENABLE,&fFilterFwStatus)||fFilterFwStatus)
            return kIOReturnError;
    }
    if(!fCore->fHw.setVlanFilter(fPort,!fFilterDesired.promisc&&q.vlanFilter?q.vlanBits:nullptr,
        &q.vlanFwStatus)||q.vlanFwStatus){fFilterFwStatus=q.vlanFwStatus;q.vlanApplied=false;return kIOReturnError;}
    q.vlanApplied=true;++q.vlanRevision;
    fFilterApplied=fFilterDesired;++fFilterPrograms;
    return kIOReturnSuccess;
}

IOReturn Cx3EthernetPort::configureRxChecksum(bool on)
{
    if(!fNetTxAdmission.enter())return kIOReturnNotPermitted;
    struct Exit{Mlx4TxAdmissionGate*g;~Exit(){g->leave();}}exit{&fNetTxAdmission};
    if(!epochValid()||fPoisoned)return kIOReturnNotReady;
    // RX timer holds the same PF transaction, so no CQE is interpreted midway.
    fNetRxCsum=on;return kIOReturnSuccess;
}

IOReturn Cx3EthernetPort::configFaultAction(OSObject*o,void*,void*,void*,void*)
{
    auto*self=OSDynamicCast(Cx3EthernetPort,o);if(!self)return kIOReturnBadArgument;
    if(self->fNetTimer)self->fNetTimer->cancelTimeout();
    if(self->fNetIface)self->setLinkStatus(kIONetworkLinkValid,nullptr,0);
    return kIOReturnSuccess;
}

IOReturn Cx3EthernetPort::applyVlanFilter(const UInt8*bits)
{
    if(!fNetTxAdmission.enter())return kIOReturnNotPermitted;
    struct Exit{Mlx4TxAdmissionGate*g;~Exit(){g->leave();}}exit{&fNetTxAdmission};
    if(!epochValid()||fPoisoned)return kIOReturnNotReady;
    if(!fFilterLock||!IOLockTryLock(fFilterLock))return kIOReturnBusy;
    struct Unlock{IOLock*l;~Unlock(){IOLockUnlock(l);}}unlock{fFilterLock};
    auto&q=fCore->fPortQueues[fPort-1];
    if(!fCore->fHw.setVlanFilter(fPort,fFilterDesired.promisc?nullptr:bits,&q.vlanFwStatus)||q.vlanFwStatus){
        q.vlanApplied=false;fPoisoned=true;closeAdmission();
        // Only host notification runs on the gate; never a firmware command.
        if(fWorkLoop)fWorkLoop->runAction(&Cx3EthernetPort::configFaultAction,this,nullptr);
        return kIOReturnError;
    }
    q.vlanApplied=true;++q.vlanRevision;return kIOReturnSuccess;
}

IOReturn Cx3EthernetPort::setHardwareAddress(const IOEthernetAddress*address)
{
    if(!address)return kIOReturnBadArgument;
    UInt8 any=0;for(UInt32 i=0;i<6;++i)any|=address->bytes[i];
    if(!any||(address->bytes[0]&1u))return kIOReturnBadArgument;
    if(!fNetTxAdmission.enter())return kIOReturnNotPermitted;
    struct Exit{Mlx4TxAdmissionGate*g;~Exit(){g->leave();}}exit{&fNetTxAdmission};
    if(!epochValid()||fPoisoned)return kIOReturnNotReady;
    if(!fFilterLock||!IOLockTryLock(fFilterLock))return kIOReturnBusy;
    struct Unlock{IOLock*l;~Unlock(){IOLockUnlock(l);}}unlock{fFilterLock};
    if(!memcmp(fMac,address->bytes,6))return kIOReturnSuccess;
    memcpy(fPendingMac,address->bytes,6);fFilterPosted=false;fWorkerResult=kIOReturnNotReady;
    const bool ok=runWorker(4);
    IOLockLock(fWorkerLock);const bool done=fWorkerDone;
    IOReturn result=done?fWorkerResult:kIOReturnTimeout;IOLockUnlock(fWorkerLock);
    if(fPoisoned){closeAdmission();if(fNetTimer)fNetTimer->cancelTimeout();
        if(fNetIface)setLinkStatus(kIONetworkLinkValid,nullptr,0);}
    setProperty("cx3MacStatus",(UInt64)result,64);
    return ok?kIOReturnSuccess:result;
}

IOReturn Cx3EthernetPort::programMac()
{
    fFilterFwStatus=CX3_FWSTATUS_NOT_POSTED;
    if(!fCore->selectorEnter())return kIOReturnNotReady;
    struct Admission{Cx3Mlx4*c;~Admission(){c->selectorExit();}}admission{fCore};
    if(!fCore->transactionTryEnter())return kIOReturnBusy;
    struct Transaction{Cx3Mlx4*c;~Transaction(){c->transactionExit();}}transaction{fCore};
    if(!epochValid()||fPoisoned||fNetTxAdmission.isClosed()||fCore->fQuarExhausted||fCore->fHw.cmdWedged())
        return kIOReturnNotReady;
    auto&q=fCore->fPortQueues[fPort-1];
    // This native A0 path owns slot zero and its existing QPN. Adding MACs at
    // other indices also needs steering QPs; a second bare table entry is unsafe.
    if(!q.macUp||!q.numbersReserved||q.macSlot!=0||q.qpQpn!=q.numbers.macBase||
       q.numbers.macSlots!=MLX4_MAX_MAC_NUM)return kIOReturnNotReady;
    UInt8 table[MLX4_MAC_TABLE_SIZE]{};table[0]=0x80;
    memcpy(table+2,fPendingMac,6);
    fFilterPosted=true;
    if(!fCore->fHw.setPortMacTableWrite(fPort,table,sizeof(table),&fFilterFwStatus)||fFilterFwStatus)
        return kIOReturnError;
    IOLockLock(fNetTxLock);
    memcpy(fMac,fPendingMac,6);memcpy(q.mac,table,8);q.macValue=0;
    for(UInt32 i=0;i<6;++i)q.macValue=(q.macValue<<8)|fMac[i];
    IOLockUnlock(fNetTxLock);
    return kIOReturnSuccess;
}

IOReturn Cx3EthernetPort::enable(IONetworkInterface *iface)
{
    if (!iface || iface!=fNetIface) return kIOReturnBadArgument;

    if (!fNetTxAdmission.enter()) return kIOReturnNotPermitted;
    struct NetworkAdmissionExit {
        Mlx4TxAdmissionGate *gate;
        ~NetworkAdmissionExit() { gate->leave(); }
    } networkAdmissionExit = { &fNetTxAdmission };
    if (
        fCore->fResetCommandsBlocked || fCore->fStopRetained) {
        LOG("enable: refused -- reset recovery is blocked or driver stop retained its owners");
        return kIOReturnNotPermitted;
    }

    const auto finishEnable = [this]() -> IOReturn {
        if (!fNetTxAdmission.isClosed()) return kIOReturnSuccess;
        fNetEnabled = false;
        if (fNetTimer) fNetTimer->cancelTimeout();
        return kIOReturnNotPermitted;
    };

    if (!epochValid() || fCore->fQuarExhausted || fCore->fHw.cmdWedged() ||
        (fCore->fPortQueues[fPort-1].txQpRec && !fCore->fPortQueues[fPort-1].txQpUp) ||
        (fCore->fPortQueues[fPort-1].qpRec && !fCore->fPortQueues[fPort-1].qpUp) ||
        (fCore->fPortQueues[fPort-1].ingressRec && !fCore->fPortQueues[fPort-1].ingressUp)) {
        LOG("enable: refused -- retention exhausted, or command/QP ownership unresolved");
        return kIOReturnNotPermitted;
    }

    if (netAttachPoisoned()) {
        LOG("enable: refused -- the deferred attach worker is poisoned (timeout/unknown "
            "state); the interface must not be brought up on a driver that is stopping");
        return kIOReturnNotPermitted;
    }

    if (fNetEnabled) {
        LOG("enable: the interface is already enabled; re-publishing the link and returning "
            "success rather than starting anything twice");
        publishLinkStatus("enable (repeat)");
        return finishEnable();
    }

    // No firmware command runs on this workloop. The worker uses try-only PF
    // locking; an attach/detach selector holding that lock cannot deadlock us.
    IOReturn filterRc=changeFilters(kFilterApply,false,nullptr,0,true);
    if(filterRc!=kIOReturnSuccess)return filterRc;
    if(fNetTxAdmission.isClosed())return kIOReturnNotPermitted;
    fNetEnabled = true;
    startLinkMonitoring();
    publishLinkStatus("enable");
    
    if (fNetTimer) {
        fNetTimer->wakeAtTime(cx3WakeInTicks(kNetTickNs));
        LOG("enable: en%u is up, and the receive path is running: the data path's timer polls "
            "the receive CQ every %llu us and hands frames to the stack; outputPacket posts into "
            "the send queue and the same tick reclaims what completed. Both halves are live for "
            "the first time in this build, so this is the line after which a ping can work -- "
            "and if it does not, the log says which half was silent.",
            (unsigned)fNetUnit, (unsigned long long)(kNetTickNs / 1000ull));
    } else {
        LOG("enable: en%u is up (which is what this callback has to prove -- the family's own "
            "enable() answers kIOReturnUnsupported), but there is NO data path timer: netup did "
            "not manage to create one, so no frame will be delivered. The kernel log says why",
            (unsigned)fNetUnit);
    }
    return finishEnable();
}

IOReturn Cx3EthernetPort::disable(IONetworkInterface *iface)
{
    if (!iface || iface!=fNetIface) return kIOReturnBadArgument;
    
    if (!fNetTxAdmission.enter()) return kIOReturnSuccess;
    struct NetworkAdmissionExit {
        Mlx4TxAdmissionGate *gate;
        ~NetworkAdmissionExit() { gate->leave(); }
    } networkAdmissionExit = { &fNetTxAdmission };

    if (!fNetEnabled) {
        LOG("disable: the interface was not enabled; nothing to stop");
        return kIOReturnSuccess;
    }
    fNetEnabled = false;
    stopLinkMonitoring();
    fLinkKnown=fLinkUp=false;fLinkSpeedMbps=0;
    setProperty("cx3LinkObservationValid",false);setProperty("cx3LinkObservedUp",false);
    setProperty("cx3LinkMonitorRunning",false);setProperty("cx3LinkObservedSpeedMbps",0u,32);
    
    if (fNetTimer) fNetTimer->cancelTimeout();
    if (fNetIface) setLinkStatus(kIONetworkLinkValid, 0, 0ull);
    LOG("disable: en%u is down; the link is published as valid but not active and the receive "
        "timer is cancelled. No device state was touched: the queues and the port stay exactly "
        "as the chain left them, so bringing the interface back up resumes on the same rings",
        (unsigned)fNetUnit);
    return kIOReturnSuccess;
}

void Cx3EthernetPort::netTimerAction(OSObject *owner, IOTimerEventSource *src)
{
    Cx3EthernetPort *me = OSDynamicCast(Cx3EthernetPort, owner);
    if (!me || src!=me->fNetTimer) return;
    
    if (!me->fNetTxAdmission.enter()) return;
    struct TimerAdmissionExit {
        Mlx4TxAdmissionGate *gate;
        ~TimerAdmissionExit() { gate->leave(); }
    } timerAdmissionExit = { &me->fNetTxAdmission };
    me->netTick();
}

bool Cx3EthernetPort::netServiceStart(const char *why)
{
    if (fNetTimer) return true;
    if (!fWorkLoop) {
        LOG("net (%s): there is no workloop to put the data path's timer on, so no frame will "
            "be delivered. getWorkLoop() creates it on demand and the family's start() needs it, "
            "so this should be unreachable", why);
        return false;
    }

    fNetTimer = IOTimerEventSource::timerEventSource(this, &Cx3EthernetPort::netTimerAction);
    if (!fNetTimer) {
        LOG("net (%s): could not create the timer event source", why);
        return false;
    }
    if (fWorkLoop->addEventSource(fNetTimer) != kIOReturnSuccess) {
        LOG("net (%s): could not add the timer to the workloop, so the data path stays off",
            why);
        fNetTimer->release();
        fNetTimer = 0;
        return false;
    }
    LOG("net (%s): the data path's timer is on the workloop. It POLLS the receive CQ; nothing "
        "is armed at the device, because the CQ arm -> COMP EQE -> EQ link is the one link "
        "stage 9 never proved (docs/10B-PRIMITIVES.md 1.6)", why);
    return true;
}

bool Cx3EthernetPort::netTimerRelease(const char *why)
{
    if (!fNetTimer) return true;

    fNetTimer->cancelTimeout();
    if (!fWorkLoop) {
        LOG("net (%s): timer retained because its workloop is unavailable; no TX or device "
            "resource teardown may proceed", why);
        return false;
    }
    const IOReturn removeRc = fWorkLoop->removeEventSource(fNetTimer);
    if (removeRc != kIOReturnSuccess) {
        LOG("net (%s): timer retained because removeEventSource returned 0x%x; callback "
            "source ownership is uncertain", why, (unsigned)removeRc);
        return false;
    }
    fNetTimer->release();
    fNetTimer = 0;
    return true;
}

void Cx3EthernetPort::netTick()
{
    if (!fNetTimer) return;
    ++fNetTicks;

    if (fNetInTick) {
        
        LOG("net: the timer action is re-entering itself; the tick is skipped. This should not "
            "be possible on a serial workloop, so it is a finding about the workloop, not about "
            "the network");
        return;
    }
    fNetInTick = true;
    serviceLink();
    if (epochValid() && fNetEnabled && !fNetTxAdmission.isClosed()) {
        if (fCore->transactionTryEnter()) {
            netServiceRx();
            
            netServiceTx();
            fCore->transactionExit();
        } else {
            ++fNetTicksSkipped;
        }
    }
    fNetInTick = false;

    if ((fNetTicks % 1024ull) == 0ull) {
        LOG("net: %llu tick(s), %llu skipped (a selector held the transaction lock), %llu "
            "frame(s) delivered / %llu byte(s), %llu dropped, %llu refill(s), %llu not-ours, "
            "%llu taken-but-not-submitted, "
            "%u ring error(s), %llu tick(s) with no receive objects (a reset ran under the "
            "interface -- re-run the chain to put them back); transmit: %llu posted, %llu "
            "completed, %llu stalled (ring full: the stack keeps those), %llu dropped, %u "
            "error(s), %llu frame(s) handed back on lock contention; enabled %s",
            (unsigned long long)fNetTicks, (unsigned long long)fNetTicksSkipped,
            (unsigned long long)fNetRxFrames, (unsigned long long)fNetRxBytes,
            (unsigned long long)fNetRxDropped, (unsigned long long)fNetRxRefills,
            (unsigned long long)fNetRxNotOurs, (unsigned long long)fNetRxNotDelivered,
            (unsigned)fNetRxErrors,
            (unsigned long long)fNetRxNoRings, (unsigned long long)fNetTxFrames,
            (unsigned long long)fNetTxCompleted, (unsigned long long)fNetTxStalled,
            (unsigned long long)fNetTxDropped, (unsigned)fNetTxErrors,
            (unsigned long long)fNetTxLockBusy, fNetEnabled ? "yes" : "no");
    }

    if (epochValid() && fNetEnabled && !fNetTxAdmission.isClosed()) fNetTimer->wakeAtTime(cx3WakeInTicks(kNetTickNs));
}

void Cx3EthernetPort::netServiceRx()
{
    UInt32 nent = fCore->fPortQueues[fPort-1].rqNent;
    UInt32 served = 0;
    UInt32 i;
    UInt8 cqe[CQE_LEGACY_SIZE];

    if (!epochValid() || !fNetIface || fNetRxFault) return;
    if (!fCore->fPortQueues[fPort-1].cqUp || !fCore->fPortQueues[fPort-1].cqRing || !fCore->fPortQueues[fPort-1].rqRing || !fCore->fPortQueues[fPort-1].rqBufs || !fCore->fPortQueues[fPort-1].qpRec) {
        
        ++fNetRxNoRings;
        return;
    }
    
    if (!fNetRxPrimed) {
        UInt32 prodNow = OSSwapBigToHostInt32(((volatile UInt32 *)fCore->fPortQueues[fPort-1].qpRec->virt)[0]);
        if (prodNow == 0u) {
            ++fNetRxErrors;
            fNetRxFault = true;
            LOG("net rx: the receive producer record reads 0, so the hardware was never told "
                "that any WQE exists; refusing to invent an in-flight set and stopping the "
                "receive path");
            return;
        }
        fCore->fPortQueues[fPort-1].rawRxProd = prodNow;
        for (i = 0; i < fCore->fPortQueues[fPort-1].rqNent; ++i) r4RawRxSetInFlight(i, true);
        fNetRxPrimed = true;
        LOG("net rx: first service for this ring: the producer record reads %u (the %u WQEs "
            "stage 3 filled and stage 4 published), so every slot is in flight until a "
            "completion says otherwise; completions already in the CQ are from those frames",
            (unsigned)prodNow, (unsigned)fCore->fPortQueues[fPort-1].rqNent);
    }

    while (served < kNetRxPerTick) {
        UInt32 cons = fCore->fPortQueues[fPort-1].rawRxCons, slot, cqeIdx, qpn, byteCnt, len;
        UInt8 *frame;
        mbuf_t m;

        memset(cqe, 0, sizeof(cqe));
        if (!fCore->fHw.cqNextCqe(fCore->fPortQueues[fPort-1].cqRing, fCore->fPortQueues[fPort-1].cqNent, 1, cons, cqe)) break;

        fCore->fPortQueues[fPort-1].rawRxCons = cons + 1;
        if (fCore->fPortQueues[fPort-1].cqRec) fCore->fHw.cqSetCi((volatile UInt32 *)fCore->fPortQueues[fPort-1].cqRec->virt, fCore->fPortQueues[fPort-1].rawRxCons);
        ++served;

        qpn     = cx3GetBe32(cqe + CQE_VLAN_MY_QPN_OFFSET) & MLX4_CQE_QPN_MASK;
        
        slot    = cons & (nent - 1u);
        cqeIdx  = (UInt32)((cqe[CQE_WQE_INDEX_OFFSET] << 8) | cqe[CQE_WQE_INDEX_OFFSET + 1]);
        byteCnt = cx3GetBe32(cqe + CQE_BYTE_CNT_OFFSET);

        // Error CQEs do not carry the normal receive byte-count/status contract.
        // Preserve the queue owners and stop this port's RX without guessing
        // which slot can be re-posted. TX and the other port remain independent.
        if ((cqe[CQE_OWNER_SR_OPCODE_OFFSET] & MLX4_CQE_OPCODE_MASK) == MLX4_CQE_OPCODE_ERROR ||
            (cqe[CQE_OWNER_SR_OPCODE_OFFSET] & MLX4_CQE_IS_SEND_MASK)) {
            ++fNetRxErrors;
            fNetRxFault = true;
            break;
        }

        if (qpn != fCore->fPortQueues[fPort-1].qpQpn) {
            
            ++fNetRxNotOurs;
            continue;
        }
        if (slot >= nent || cqeIdx != (cons & 0xffffu) || !r4RawRxInFlight(slot)) {
            ++fNetRxErrors;
            fNetRxFault = true;
            LOG("net rx: the completion at consumer index %u carries wqe_index %u, and this ring "
                "(%u entries) has slot %u%s. The refill stops here rather than continue on an "
                "assumption -- a buffer handed back while a frame can still land in it is the "
                "defect this check exists for. Nothing is lost that was already read.",
                (unsigned)cons, (unsigned)cqeIdx, (unsigned)nent, (unsigned)slot,
                r4RawRxInFlight(slot) ? "" : ", and it was NOT in flight");
            break;
        }
        if (byteCnt < kNetTxMinFrame + 4u || byteCnt > fCore->fPortQueues[fPort-1].rqBufBytes - MLX4_NET_IP_ALIGN) {
            
            ++fNetRxErrors;
            LOG("net rx: the completion at consumer index %u reports %u byte(s), and this ring's "
                "buffers are %u byte(s): nothing is delivered for it and the slot is re-posted",
                (unsigned)cons, (unsigned)byteCnt, (unsigned)fCore->fPortQueues[fPort-1].rqBufBytes);
            netRxRecycle(slot);
            continue;
        }

        len   = byteCnt - 4u;
        frame = (UInt8 *)fCore->fPortQueues[fPort-1].rqBufs->virt + (UInt64)slot * (UInt64)fCore->fPortQueues[fPort-1].rqBufBytes;

        const bool stripped=(cx3GetBe32(cqe+CQE_VLAN_MY_QPN_OFFSET)&MLX4_CQE_CVLAN_PRESENT_MASK)!=0;
        const bool inlineTag=len>=18u&&frame[12]==0x81u&&frame[13]==0;
        // One C-tag only. A QPC may leave the tag inline; normalize that case
        // too, because HardwareVlan requires all delivered C-tags out of band.
        const UInt16 vlanTag=stripped?cx3GetBe16(cqe+CQE_SL_VID_OFFSET):
            (inlineTag?cx3GetBe16(frame+14):0);
        const bool nested=inlineTag&&len>=22u&&frame[16]==0x81u&&frame[17]==0;
        if((stripped&&inlineTag)||nested||((stripped||inlineTag)&&(vlanTag&0xfffu)==0xfffu)||
           byteCnt>fPacketSize+(inlineTag?MLX4_ETH_VLAN_LEN:0u)){
            ++fNetRxDropped;netRxRecycle(slot);continue;
        }
        if(inlineTag)len-=MLX4_ETH_VLAN_LEN;

        m = allocatePacketNoWait(len);
        if (!m) {
            ++fNetRxDropped;
        } else {
            UInt32 submitted;
            if(inlineTag){
                bcopy(frame,mbuf_data(m),12);
                bcopy(frame+16,(UInt8*)mbuf_data(m)+12,len-12);
            }else bcopy(frame,mbuf_data(m),len);
            if(stripped||inlineTag)setVlanTag(m,vlanTag);
            
            if (fNetRxCsum) {
                UInt16 st = (UInt16)cx3GetBe16(cqe + CQE_STATUS_OFFSET);
                UInt16 cs = (UInt16)cx3GetBe16(cqe + CQE_CHECKSUM_OFFSET);
                UInt32 mask = 0;
                bool isV6 = (st & MLX4_CQE_STATUS_IPV6) != 0;

                if ((st & MLX4_CQE_STATUS_IPOK) && cs == (UInt16)MLX4_CQE_CHECKSUM_VALID &&
                    (st & (MLX4_CQE_STATUS_IPV4 | MLX4_CQE_STATUS_IPV6))) {
                    if (!isV6) mask |= kChecksumIP;   
                    if (st & MLX4_CQE_STATUS_TCP)
                        mask |= isV6 ? kChecksumTCPIPv6 : kChecksumTCP;
                    if (st & MLX4_CQE_STATUS_UDP)
                        mask |= isV6 ? kChecksumUDPIPv6 : kChecksumUDP;
                    if (mask) {
                        
                        setChecksumResult(m, kChecksumFamilyTCPIP, mask, mask);
                        ++fNetRxCsumGood;
                    } else {
                        ++fNetRxCsumNone;
                    }
                } else {
                    ++fNetRxCsumNone;
                }
            }
            
            submitted = fNetIface->inputPacket(m, len, 0);
            if (submitted == 0u) {
                ++fNetRxNotDelivered;
                if (fNetRxNotDelivered <= 8ull) {
                    LOG("net rx: inputPacket took %u byte(s) and submitted nothing (returned 0), "
                        "so this frame did not reach the stack -- the interface is down, or its "
                        "input path refused it. The mbuf belongs to the interface either way",
                        (unsigned)len);
                }
            } else {
                ++fNetRxFrames;
                fNetRxBytes += len;
            }
            if (fNetRxFrames <= 8 || (fNetRxFrames % 64ull) == 0ull) {
                LOG("net rx: frame %llu delivered: %u byte(s) after the FCS was removed, from "
                    "slot %u (consumer %u, buffer bus 0x%llx), %02x:%02x:%02x:%02x:%02x:%02x -> "
                    "%02x:%02x:%02x:%02x:%02x:%02x ethertype 0x%02x%02x",
                    (unsigned long long)fNetRxFrames, (unsigned)len, (unsigned)slot,
                    (unsigned)cons,
                    (unsigned long long)fCore->fHw.rqBufferPhys(fCore->fPortQueues[fPort-1].rqBufs, fCore->fPortQueues[fPort-1].rqBufBytes, slot),
                    frame[6], frame[7], frame[8], frame[9], frame[10], frame[11],
                    frame[0], frame[1], frame[2], frame[3], frame[4], frame[5],
                    frame[12], frame[13]);
            }
        }

        netRxRecycle(slot);
    }
}

void Cx3EthernetPort::netServiceTx()
{
    UInt32 served = 0, nent = fCore->fPortQueues[fPort-1].txSqNent;
    UInt8 cqe[CQE_LEGACY_SIZE];

    if (!fCore->fPortQueues[fPort-1].txCqRing || !fCore->fPortQueues[fPort-1].txSq || fNetTxSlotCount == 0 || nent == 0) return;
    
    if (!fNetTxLock || !IOLockTryLock(fNetTxLock)) {
        ++fNetTxLockBusy;
        return;
    }

    while (served < (UInt32)kNetTxPerTick) {
        UInt32 cons = fCore->fPortQueues[fPort-1].rawTxCons, opcode, err, wqeIdx, isSend;
        mbuf_t m;

        memset(cqe, 0, sizeof(cqe));
        if (!fCore->fHw.cqNextCqe(fCore->fPortQueues[fPort-1].txCqRing, fCore->fPortQueues[fPort-1].txCqNent, 1, cons, cqe)) break;

        opcode = cqe[CQE_OWNER_SR_OPCODE_OFFSET];
        err    = ((opcode & MLX4_CQE_OPCODE_MASK) == MLX4_CQE_OPCODE_ERROR) ? 1u : 0u;
        isSend = (opcode & MLX4_CQE_IS_SEND_MASK) ? 1u : 0u;
        wqeIdx = cx3GetBe16(cqe + CQE_WQE_INDEX_OFFSET);

        fCore->fPortQueues[fPort-1].rawTxCons = cons + 1u;
        if (fCore->fPortQueues[fPort-1].txCqRec) fCore->fHw.cqSetCi((volatile UInt32 *)fCore->fPortQueues[fPort-1].txCqRec->virt, fCore->fPortQueues[fPort-1].rawTxCons);
        ++served;
        ++fNetTxCompleted;

        if (err) {
            ++fNetTxErrors;
            
            const bool flush = cqe[CQE_ERR_SYNDROME_OFFSET] == 0x05u;
            if (flush) ++fNetTxFlushErrors;
            if (!fNetTxFault.latched) {
                fNetTxFault.latched = true;
                fNetTxFault.consumer = cons;
                fNetTxFault.producer = fCore->fPortQueues[fPort-1].rawTxProd;
                fNetTxFault.slot = wqeIdx & (nent - 1u);
                fNetTxFault.qpn = fCore->fPortQueues[fPort-1].txQpn;
                memcpy(fNetTxFault.cqe, cqe, sizeof(fNetTxFault.cqe));
                const UInt32 slot = fNetTxFault.slot;
                if (slot < fNetTxSlotCount && slot < (UInt32)kNetTxSlots) {
                    fNetTxFault.owner = fNetTxOwner[slot];
                    fNetTxFault.hadMbuf = fNetTxMbuf[slot] != 0;
                    const UInt64 offset = MLX4_SQ_WQE_OFFSET(slot);
                    if (fCore->fPortQueues[fPort-1].txSq->virt && offset + MLX4_TXBB_SIZE <= fCore->fPortQueues[fPort-1].txSq->bytes) {
                        memcpy(fNetTxFault.wqe, (UInt8 *)fCore->fPortQueues[fPort-1].txSq->virt + offset,
                               sizeof(fNetTxFault.wqe));
                        fNetTxFault.wqeValid = true;
                    }
                }
                LOG("net tx: FIRST ERROR (%s): syndrome 0x%02x vendor 0x%02x, consumer %u "
                    "producer %u wqe_index %u slot %u posted_owner %u mbuf %u WQE_valid %u "
                    "configured_qpn 0x%x cqe_qpn 0x%x; new TX blocked for controller lifetime; "
                    "CQ draining continues, root cause unconfirmed",
                    flush ? "WR_FLUSH_ERR" : "non-flush completion error",
                    (unsigned)cqe[CQE_ERR_SYNDROME_OFFSET],
                    (unsigned)cqe[CQE_ERR_VENDOR_SYNDROME_OFFSET],
                    (unsigned)cons, (unsigned)fNetTxFault.producer, (unsigned)wqeIdx,
                    (unsigned)slot, (unsigned)fNetTxFault.owner, (unsigned)fNetTxFault.hadMbuf,
                    (unsigned)fNetTxFault.wqeValid, (unsigned)fNetTxFault.qpn,
                    (unsigned)(cx3GetBe32(cqe + CQE_VLAN_MY_QPN_OFFSET) & MLX4_CQE_QPN_MASK));
                
                for (UInt32 row = 0; row < CQE_LEGACY_SIZE; row += 16u) {
                    const UInt8 *p = fNetTxFault.cqe + row;
                    LOG("net tx: FIRST CQE +%02x: %08x %08x %08x %08x", (unsigned)row,
                        (unsigned)cx3GetBe32(p), (unsigned)cx3GetBe32(p + 4),
                        (unsigned)cx3GetBe32(p + 8), (unsigned)cx3GetBe32(p + 12));
                }
                if (fNetTxFault.wqeValid) {
                    for (UInt32 row = 0; row < MLX4_TXBB_SIZE; row += 16u) {
                        const UInt8 *p = fNetTxFault.wqe + row;
                        LOG("net tx: FIRST WQE +%02x: %08x %08x %08x %08x", (unsigned)row,
                            (unsigned)cx3GetBe32(p), (unsigned)cx3GetBe32(p + 4),
                            (unsigned)cx3GetBe32(p + 8), (unsigned)cx3GetBe32(p + 12));
                    }
                }
            }
        }
        if (!isSend && !err) {
            
            continue;
        }
        
        wqeIdx &= (nent - 1u);
        if (wqeIdx >= nent || wqeIdx >= (UInt32)kNetTxSlots) {
            ++fNetTxErrors;
            if (!fNetTxFault.latched) LOG("net tx: the completion at consumer index %u names TXBB %u, and this send queue "
                "has %u entries, so no frame record matches it. Nothing is freed on a guess: a "
                "record freed for the wrong TXBB is a frame freed while the hardware may still "
                "be reading it", (unsigned)cons, (unsigned)wqeIdx, (unsigned)nent);
            continue;
        }

        m = fNetTxMbuf[wqeIdx];
        fNetTxMbuf[wqeIdx] = 0;
        if (m) freePacket(m);
        else if (!fNetTxFault.latched &&
                 (fNetTxCompleted <= 8ull || (fNetTxCompleted % 64ull) == 0ull)) {
            
            LOG("net tx: the completion for TXBB %u has no mbuf recorded, so nothing was freed "
                "-- either the frame was posted by the selector path (txprobe/r4raw share this "
                "queue) or the record was already cleared", (unsigned)wqeIdx);
        }
        fCore->fHw.sqStampTxbb((UInt8 *)fCore->fPortQueues[fPort-1].txSq->virt, wqeIdx, fNetTxOwner[wqeIdx]);
    }
    IOLockUnlock(fNetTxLock);
}

void Cx3EthernetPort::netRxRecycle(UInt32 slot)
{
    if (!rqRepostSlot(slot)) {
        ++fNetRxErrors;
        LOG("net rx: slot %u has no address from the buffer allocation, so its WQE was NOT "
            "re-posted: a WQE naming address zero is one the hardware would fill somewhere else. "
            "The ring runs short instead, and every later completion will fail the ring checks "
            "and say so", (unsigned)slot);
        return;
    }
    ++fNetRxRefills;
    if (slot < CX3_R4RAW_MAX_RX_SLOTS) ++fCore->fPortQueues[fPort-1].rawRxRecycles[slot];
}

UInt32 Cx3EthernetPort::outputPacket(mbuf_t pkt, void *param)
{
    UInt32 nent, slot, prod, ownerBit, len;
    UInt64 bufPhys;
    UInt8 *bufVirt;

    (void)param;
    if (!pkt) return kIOReturnOutputDropped;
    if (!fNetTxAdmission.enter()) return kIOReturnOutputStall;
    struct TxAdmissionExit {
        Mlx4TxAdmissionGate *gate;
        ~TxAdmissionExit() { gate->leave(); }
    } txAdmissionExit = { &fNetTxAdmission };

    if (!epochValid() || !fNetEnabled || !fCore->fPortQueues[fPort-1].rawFrame || !fCore->fPortQueues[fPort-1].txSq || !fCore->fPortQueues[fPort-1].txCqRing || !fCore->fPortQueues[fPort-1].txQpUp) {
        ++fNetTxDropped;
        freePacket(pkt);
        return kIOReturnOutputDropped;
    }
    
    if (!fNetTxLock || !IOLockTryLock(fNetTxLock)) {
        ++fNetTxLockBusy;
        return kIOReturnOutputStall;    
    }

    if (fNetTxFault.latched) {
        ++fNetTxDropped;
        IOLockUnlock(fNetTxLock);
        freePacket(pkt);
        return kIOReturnOutputDropped;
    }

    len = (UInt32)mbuf_pkthdr_len(pkt);
    if (len == 0) len = (UInt32)mbuf_len(pkt);
    if (len < (UInt32)kNetTxMinFrame || len > fPacketSize ||
        len > (fNetTxBufBytes - (UInt32)MLX4_NET_IP_ALIGN)) {
        ++fNetTxDropped;
        if (fNetTxDropped <= 8) {
            LOG("net tx: a %u byte frame is not something this driver can put on the wire "
                "(%u..%u), so it is dropped and counted. A zero here means the packet carries "
                "neither a pkthdr length nor a length in its single mbuf",
                (unsigned)len, (unsigned)kNetTxMinFrame,
                (unsigned)(fNetTxBufBytes - (UInt32)MLX4_NET_IP_ALIGN));
        }
        IOLockUnlock(fNetTxLock);
        freePacket(pkt);
        return kIOReturnOutputDropped;
    }

    nent = fCore->fPortQueues[fPort-1].txSqNent;
    prod = fCore->fPortQueues[fPort-1].rawTxProd;
    slot = prod & (nent - 1u);
    ownerBit = (prod & nent) ? 1u : 0u;

    if (fNetTxMbuf[slot] != 0) {
        
        ++fNetTxStalled;
        IOLockUnlock(fNetTxLock);
        return kIOReturnOutputStall;
    }

    bufPhys = fCore->fHw.rqBufferPhys(fCore->fPortQueues[fPort-1].rawFrame, fNetTxBufBytes, slot);
    if (bufPhys == 0) {
        ++fNetTxErrors;
        LOG("net tx: TXBB %u has no address in the frame ring, so no WQE was written and the "
            "frame is dropped rather than sent from an address of zero", (unsigned)slot);
        IOLockUnlock(fNetTxLock);
        freePacket(pkt);
        return kIOReturnOutputDropped;
    }
    bufVirt = (UInt8 *)fCore->fPortQueues[fPort-1].rawFrame->virt + (UInt64)slot * (UInt64)fNetTxBufBytes;

    if (mbuf_copydata(pkt, 0, len, bufVirt) != 0) {
        ++fNetTxErrors;
        LOG("net tx: mbuf_copydata could not produce %u contiguous byte(s) for TXBB %u, so the "
            "frame is dropped rather than sent with a hole in it", (unsigned)len,
            (unsigned)slot);
        IOLockUnlock(fNetTxLock);
        freePacket(pkt);
        return kIOReturnOutputDropped;
    }

    const bool tagged=len>=18u&&bufVirt[12]==0x81u&&bufVirt[13]==0;
    UInt32 vlanTag=0;const bool insertVlan=getVlanTagDemand(pkt,&vlanTag);
    if((insertVlan&&(tagged||(bufVirt[12]==0x88u&&bufVirt[13]==0xa8u)||(vlanTag&0xfffu)==0xfffu))||
       len>fPacketSize-MLX4_ETH_FCS_LEN+(tagged?MLX4_ETH_VLAN_LEN:0u)){
        ++fNetTxDropped;IOLockUnlock(fNetTxLock);freePacket(pkt);return kIOReturnOutputDropped;
    }

    {
        UInt32 demand = 0;
        UInt32 csumFlags = 0;

        if (fNetTxCsum) {
            getChecksumDemand(pkt, kChecksumFamilyTCPIP, &demand);
            if (demand & kChecksumIP) csumFlags |= (UInt32)MLX4_WQE_CTRL_IP_CSUM;
            if (demand & (kChecksumTCP | kChecksumUDP |
                          kChecksumTCPIPv6 | kChecksumUDPIPv6))
                csumFlags |= (UInt32)MLX4_WQE_CTRL_TCP_UDP_CSUM;
        }
        if (csumFlags) {
            ++fNetTxCsumGood;
            if (fNetTxCsumGood <= 8ull || (fNetTxCsumGood % 64ull) == 0ull) {
                LOG("net tx: frame %llu asks for hardware checksums (demand 0x%x -> srcrb_flags "
                    "0x%x): the IP and/or L4 checksum field already holds the pseudo-header value "
                    "and the device completes it", (unsigned long long)fNetTxCsumGood,
                    (unsigned)demand, (unsigned)csumFlags);
            }
        } else {
            ++fNetTxCsumNone;
        }
        fCore->fHw.sqWriteSendWqe((UInt8 *)fCore->fPortQueues[fPort-1].txSq->virt, slot, prod, ownerBit, len, fCore->fMrLkey, bufPhys,
                           1u, MLX4_OPCODE_SEND, 0,
                           false, csumFlags,insertVlan,(UInt16)vlanTag);
    }
    
    fNetTxMbuf[slot] = pkt;
    fNetTxOwner[slot] = (UInt8)ownerBit;
    fCore->fPortQueues[fPort-1].rawTxProd = prod + 1u;
    fCore->fHw.sqRingSendDoorbell(fCore->fPortQueues[fPort-1].txQpn, fCore->fPortQueues[fPort-1].cqUarStart, false);

    ++fNetTxFrames;
    IOLockUnlock(fNetTxLock);
    if (fNetTxFrames <= 8ull || (fNetTxFrames % 64ull) == 0ull) {
        UInt8 *f = bufVirt;
        LOG("net tx: frame %llu posted: %u byte(s) in TXBB %u (prod %u, owner bit %u, buffer "
            "bus 0x%llx), %02x:%02x:%02x:%02x:%02x:%02x -> %02x:%02x:%02x:%02x:%02x:%02x "
            "ethertype 0x%02x%02x. The WQE is NOT waited for: the completion frees the mbuf",
            (unsigned long long)fNetTxFrames, (unsigned)len, (unsigned)slot, (unsigned)prod,
            (unsigned)ownerBit, (unsigned long long)bufPhys,
            f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11],
            f[12], f[13]);
    }
    return kIOReturnOutputSuccess;
}

bool Cx3EthernetPort::r4RawRxInFlight(UInt32 slot) const
{
    if (slot >= CX3_R4RAW_MAX_RX_SLOTS) return false;
    return (fCore->fPortQueues[fPort-1].rawRxInFlight[slot >> 3] & (UInt8)(1u << (slot & 7u))) ? true : false;
}

void Cx3EthernetPort::r4RawRxSetInFlight(UInt32 slot, bool inFlight)
{
    UInt8 bit;

    if (slot >= CX3_R4RAW_MAX_RX_SLOTS) return;
    bit = (UInt8)(1u << (slot & 7u));
    if (inFlight) fCore->fPortQueues[fPort-1].rawRxInFlight[slot >> 3] |= bit;
    else          fCore->fPortQueues[fPort-1].rawRxInFlight[slot >> 3] &= (UInt8)(~bit);
}

bool Cx3EthernetPort::rqRepostSlot(UInt32 slot)
{
    UInt64 phys = fCore->fHw.rqBufferPhys(fCore->fPortQueues[fPort-1].rqBufs, fCore->fPortQueues[fPort-1].rqBufBytes, slot);
    UInt32 byteCount = fCore->fPortQueues[fPort-1].rqBufBytes - MLX4_NET_IP_ALIGN;

    if (phys == 0) return false;

    fCore->fHw.rqWriteWqe((UInt8 *)fCore->fPortQueues[fPort-1].rqRing->virt, RX_WQE_SIZE, slot, phys, byteCount, fCore->fPortQueues[fPort-1].rqLkey);
    ++fCore->fPortQueues[fPort-1].rawRxProd;
    ++fCore->fPortQueues[fPort-1].rawRxRecyclesTotal;
    fCore->fHw.rqSetProducer((volatile UInt32 *)fCore->fPortQueues[fPort-1].qpRec->virt, fCore->fPortQueues[fPort-1].rawRxProd);
    r4RawRxSetInFlight(slot, true);
    return true;
}
