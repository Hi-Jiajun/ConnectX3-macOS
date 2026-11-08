/* Copyright (c) 2026 Hi-Jiajun and contributors. OpenIB BSD; see LICENSE.
 * A native controller is permanently bound to one PF/port/epoch. The PF owns
 * all DMA in Cx3PortQueues; this object owns host callbacks and packet records. */
#ifndef CX3_ETHERNET_PORT_H
#define CX3_ETHERNET_PORT_H
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IONetworkMedium.h>
#include <IOKit/IOTimerEventSource.h>
#include <kern/thread_call.h>
#include "Mlx4Hw.h"
class Cx3Mlx4;
class Cx3EthernetPort:public IOEthernetController {
 OSDeclareDefaultStructors(Cx3EthernetPort)
 typedef IOEthernetController super;
 CX3_STATIC_ASSERT(__is_same(super,IOEthernetController),"native direct superclass");
public:
 bool init(OSDictionary *dict=0) override;
 void free() override;
 bool bind(Cx3Mlx4 *core,UInt32 port,UInt64 generation,const UInt8 *mac);
 bool start(IOService *provider) override;
 void stop(IOService *provider) override;
 bool createWorkLoop() override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn enable(IONetworkInterface*) override;
 IOReturn disable(IONetworkInterface*) override;
 IOReturn getHardwareAddress(IOEthernetAddress*) override;
 IOReturn setHardwareAddress(const IOEthernetAddress*) override;
 UInt32 getFeatures() const override;
 IOReturn getChecksumSupport(UInt32*,UInt32,bool) override;
 IOReturn getMaxPacketSize(UInt32*) const override;
 IOReturn setMaxPacketSize(UInt32) override;
 IOReturn selectMedium(const IONetworkMedium*) override;
 IOReturn setPromiscuousMode(bool) override;
 IOReturn setMulticastMode(bool) override;
 IOReturn setMulticastList(IOEthernetAddress*,UInt32) override;
 UInt32 outputPacket(mbuf_t,void*) override;
 bool attachAndWait();
 bool shutdown();
 void closeAdmission();
 void snapshot(Cx3NativePortResult*);
 bool poisoned() const {return fPoisoned;}
 bool attached() const {return fNetIface && fStarted && !fNetTxAdmission.isClosed();}
 // Called only while the PF selector transaction is held, never on its gate.
 IOReturn applyVlanFilter(const UInt8*);
 IOReturn configureRxChecksum(bool);
private:
 Cx3Mlx4 *fCore=nullptr;
 UInt32 fPort=0;UInt64 fGeneration=0;UInt8 fMac[6]{};
 IOWorkLoop *fWorkLoop=nullptr;
 OSDictionary *fMediaDict=nullptr;IONetworkMedium *fMediumAuto=nullptr,*fMediumActive=nullptr;
 UInt64 fMediumSpeed=0;
 IOLock *fLinkLock=nullptr;thread_call_t fLinkCall=nullptr;
 bool fLinkRef=false,fLinkInFlight=false,fLinkMonitoring=false,fLinkHaveResult=false;
 UInt64 fLinkEpoch=0,fLinkRequestEpoch=0,fLinkNextNs=0,fLinkLastNs=0;
 bool fLinkKnown=false,fLinkUp=false;UInt32 fLinkSpeedMbps=0;
 IOReturn fLinkQueryStatus=kIOReturnNotReady;UInt32 fLinkFwStatus=CX3_FWSTATUS_NOT_POSTED;
 UInt64 fLinkQueries=0,fLinkBusy=0,fLinkFailures=0;
 struct LinkObservation {
  UInt64 epoch=0,stampNs=0;IOReturn status=kIOReturnNotReady;
  UInt32 fwStatus=CX3_FWSTATUS_NOT_POSTED,speedMbps=0;
  bool valid=false,up=false,posted=false;
 } fLinkResult;
 static void linkWorker(thread_call_param_t,thread_call_param_t);
 LinkObservation queryLink(UInt64);
 void startLinkMonitoring();void stopLinkMonitoring();void serviceLink();
 IOLock *fWorkerLock=nullptr;thread_call_t fWorker=nullptr;
 bool fWorkerInFlight=false,fWorkerDone=false,fWorkerOk=false,fWorkerRef=false;
 bool fPoisoned=false,fStarted=false,fDetached=false;UInt32 fWorkerKind=0;
 static void worker(thread_call_param_t,thread_call_param_t);
 static IOReturn attachAction(OSObject*,void*,void*,void*,void*);
 static IOReturn configFaultAction(OSObject*,void*,void*,void*,void*);
 bool runWorker(UInt32);
 enum {kFilterApply=0,kFilterPromisc=1,kFilterMulticast=2,kFilterList=3,kFilterMax=128};
 struct FilterState {
  bool promisc=false,multicast=false,overflow=false;
  UInt32 count=0;UInt8 addresses[kFilterMax][6]{};
 } fFilterDesired,fFilterApplied;
 IOLock *fFilterLock=nullptr;
 IOReturn fFilterStatus=kIOReturnNotReady,fWorkerResult=kIOReturnNotReady;
 UInt32 fFilterFwStatus=CX3_FWSTATUS_NOT_POSTED;UInt64 fFilterPrograms=0;
 bool fFilterPosted=false;
 IOReturn changeFilters(UInt32,bool,IOEthernetAddress*,UInt32,bool=false);
 IOReturn programFilters();
 UInt8 fPendingMac[6]{};
 IOReturn programMac();
 void publishFilterState();
 bool epochValid() const;
 bool netAttachPoisoned(){return fPoisoned;}
 void publishLinkStatus(const char*);
 static void netTimerAction(OSObject*,IOTimerEventSource*);
 bool netServiceStart(const char*);bool netTimerRelease(const char*);
 void netTick();void netServiceRx();void netServiceTx();void netRxRecycle(UInt32);
 bool r4RawRxInFlight(UInt32) const;void r4RawRxSetInFlight(UInt32,bool);bool rqRepostSlot(UInt32);
 enum {kNetTickNs=1000000,kNetRxPerTick=256,kNetTxPerTick=256,kNetTxSlots=256,kNetTxMinFrame=14};
 IONetworkInterface *fNetIface=nullptr;IOTimerEventSource *fNetTimer=nullptr;
 IOLock *fNetTxLock=nullptr;Mlx4TxAdmissionGate fNetTxAdmission;
 bool fNetEnabled=false,fNetInTick=false,fNetRxPrimed=false,fNetRxFault=false,fNetRxCsum=false,fNetTxCsum=false;
 UInt32 fNetUnit=0xffffffffu,fNetTxBufBytes=2048,fNetTxSlotCount=0;
 UInt32 fPacketCapacity=0,fPacketSize=1518; // family units: includes FCS, excludes VLAN
 UInt64 fNetTicks=0,fNetTicksSkipped=0,fNetRxFrames=0,fNetRxBytes=0,fNetRxDropped=0;
 UInt64 fNetRxRefills=0,fNetRxNotOurs=0,fNetRxNotDelivered=0,fNetRxNoRings=0;
 UInt64 fNetRxCsumGood=0,fNetRxCsumNone=0,fNetTxCsumGood=0,fNetTxCsumNone=0;
 UInt64 fNetTxFrames=0,fNetTxCompleted=0,fNetTxDropped=0,fNetTxStalled=0,fNetTxLockBusy=0;
 UInt32 fNetRxErrors=0,fNetTxErrors=0,fNetTxFlushErrors=0;
 mbuf_t fNetTxMbuf[kNetTxSlots]{};UInt8 fNetTxOwner[kNetTxSlots]{};
 struct {bool latched=false,wqeValid=false,hadMbuf=false;UInt8 owner=0;
  UInt32 consumer=0,producer=0,slot=0,qpn=0;UInt8 cqe[CQE_LEGACY_SIZE]{},wqe[MLX4_TXBB_SIZE]{};
 }fNetTxFault;
};
#endif
