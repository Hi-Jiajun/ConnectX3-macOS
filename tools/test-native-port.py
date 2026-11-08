#!/usr/bin/python3
"""B2c smoke: compile the real native controller, PF manager, UC and CLI.

IOKit, scheduling and device primitives are synthetic. Tests establish port
binding and host ownership only; they do not establish real framework/DMA or
network acceptance. No kext or hardware entry point is called.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(path, signature):
    s = (ROOT / path).read_text()
    a = s.index(signature)
    return s[a:s.index('\n}\n', a) + 3]


COMMON = r'''
#pragma once
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <strings.h>
#include <vector>
#include <array>
#include <map>
#include <string>
#include <libkern/OSByteOrder.h>
#define CX3_HOST_TEST
#include "mlx4_pure.h"
#include "Cx3PortQueues.h"
#include "Cx3PortState.h"
#define OSDeclareDefaultStructors(x)
#define OSDefineMetaClassAndStructors(x,y)
#define OSDynamicCast(x,p) dynamic_cast<x*>(p)
#define LOG(...) ((void)0)
using IOReturn=int;
enum {kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnNotReady=-2,kIOReturnError=-3,
 kIOReturnNoMemory=-4,kIOReturnNotAttached=-5,kIOReturnNotPermitted=-6,KERN_FAILURE=-7,kIOReturnBusy=-8,kIOReturnUnsupported=-9,kIOReturnTimeout=-10,
 kIOReturnOutputSuccess=0,kIOReturnOutputDropped=1,kIOReturnOutputStall=2,
 kChecksumFamilyTCPIP=1,kChecksumIP=1,kChecksumTCP=2,kChecksumUDP=4,kChecksumTCPIPv6=8,kChecksumUDPIPv6=16,
 kIONetworkFeatureHardwareVlan=2,kIONetworkLinkValid=1,kIONetworkLinkActive=2};
struct Packet {unsigned frees=0;UInt32 demand=0,checksumMask=0,length=64;bool hasVlan=false;UInt32 vlan=0;UInt8 data[16384]{};};using mbuf_t=Packet*;
static size_t mbuf_pkthdr_len(mbuf_t p){return p->length;}
static size_t mbuf_len(mbuf_t p){return p->length;}
static int mbuf_copydata(mbuf_t p,unsigned,unsigned n,void*v){memcpy(v,p->data,n);return 0;}
static void*mbuf_data(mbuf_t p){return p->data;}
struct IOLock{bool held=false;};
static IOLock*IOLockAlloc(){return new IOLock;}
static void IOLockFree(IOLock*p){delete p;}
static void IOLockLock(IOLock*p){if(p->held)abort();p->held=true;}
static bool IOLockTryLock(IOLock*p){if(p->held)return false;p->held=true;return true;}
static void IOLockUnlock(IOLock*p){p->held=false;}
struct OSObject{unsigned refs=1;virtual ~OSObject(){};void retain(){++refs;}void release(){if(!refs)abort();--refs;}};
struct OSDictionary:OSObject{static OSDictionary*withCapacity(unsigned){return new OSDictionary;}};
using IOMediumType=unsigned;constexpr unsigned kIOMediumEthernetAuto=1;
struct IONetworkMedium:OSObject{
 UInt64 speed=0;static bool fail;
 static IONetworkMedium*medium(IOMediumType,UInt64 speed,unsigned,unsigned,const char*){if(fail)return nullptr;auto*m=new IONetworkMedium;m->speed=speed;return m;}
 static bool addMedium(OSDictionary*,IONetworkMedium*){return true;}
};bool IONetworkMedium::fail=false;
struct IOService:OSObject{
 IOService*provider=nullptr;virtual bool init(OSDictionary* =nullptr){return true;}
 virtual bool start(IOService*){return true;}virtual void stop(IOService*){}virtual void free(){}
 bool attach(IOService*p){provider=p;return true;}void detach(IOService*){provider=nullptr;}
 unsigned properties=0;
 IOService*getProvider(){return provider;}void setProperty(const char*,UInt64,unsigned){++properties;}void setProperty(const char*,bool){++properties;}
};
struct IONetworkInterface:IOService{
 unsigned unit=0,received=0;Packet last{};
 void registerService(){}unsigned getUnitNumber(){return unit;}
 unsigned inputPacket(mbuf_t p,unsigned,unsigned){last=*p;++received;++p->frees;return 1;}
};
using IOEthernetInterface=IONetworkInterface;
struct IOEthernetAddress{UInt8 bytes[6];};
struct IOWorkLoop:OSObject{
 using Action=IOReturn(*)(OSObject*,void*,void*,void*,void*);
 static bool removeOk;static IOWorkLoop*workLoop(){return new IOWorkLoop;}
 IOReturn runAction(Action a,OSObject*o,void*p){return a(o,p,nullptr,nullptr,nullptr);}
 int addEventSource(void*){return 0;}int removeEventSource(void*){return removeOk?0:-3;}
};bool IOWorkLoop::removeOk=true;
static bool attachOk=true,startOk=true;static unsigned unitNext=10;
struct IOEthernetController:IOService{
 virtual bool createWorkLoop(){return true;}virtual IOWorkLoop*getWorkLoop()const{return nullptr;}
 bool start(IOService*)override{return startOk&&createWorkLoop();}
 virtual IOReturn enable(IONetworkInterface*){return 0;}virtual IOReturn disable(IONetworkInterface*){return 0;}
 virtual IOReturn getHardwareAddress(IOEthernetAddress*){return 0;}
 virtual IOReturn setHardwareAddress(const IOEthernetAddress*){return 0;}
 virtual UInt32 getFeatures()const{return 0;}
 bool getVlanTagDemand(mbuf_t p,UInt32*v){*v=p->vlan;return p->hasVlan;}
 void setVlanTag(mbuf_t p,UInt32 v){p->vlan=v;p->hasVlan=true;}
 virtual IOReturn getChecksumSupport(UInt32*,UInt32,bool){return 0;}
 virtual IOReturn getMaxPacketSize(UInt32*)const{return 0;}virtual IOReturn setMaxPacketSize(UInt32){return 0;}
 virtual IOReturn selectMedium(const IONetworkMedium*){return 0;}
 virtual IOReturn setPromiscuousMode(bool){return 0;}virtual IOReturn setMulticastMode(bool){return 0;}
 virtual IOReturn setMulticastList(IOEthernetAddress*,UInt32){return 0;}
 bool publishMediumDictionary(OSDictionary*){return true;}bool setSelectedMedium(const IONetworkMedium*){return true;}
 virtual UInt32 outputPacket(mbuf_t,void*){return 0;}
 bool attachInterface(IONetworkInterface**i,bool){if(!attachOk)return false;*i=new IONetworkInterface;(*i)->unit=unitNext++;return true;}
 void detachInterface(IONetworkInterface*i,bool){i->release();}
 unsigned linkFlags=0,linkCalls=0;UInt64 linkBps=0;const IONetworkMedium*activeMedium=nullptr;
 void setLinkStatus(unsigned flags,const IONetworkMedium*m,UInt64 speed){linkFlags=flags;linkBps=speed;activeMedium=m;++linkCalls;}
 void freePacket(mbuf_t p){++p->frees;}mbuf_t allocatePacketNoWait(unsigned n){auto*p=new Packet;p->length=n;return p;}
 void getChecksumDemand(mbuf_t p,UInt32,UInt32*m){*m=p->demand;}
 void setChecksumResult(mbuf_t p,UInt32,UInt32 m,UInt32){p->checksumMask=m;}
};
struct IOTimerEventSource:OSObject{
 bool armed=false;unsigned wakes=0;
 static IOTimerEventSource*timerEventSource(OSObject*,void(*)(OSObject*,IOTimerEventSource*)){return new IOTimerEventSource;}
 void cancelTimeout(){armed=false;}void wakeAtTime(UInt64){armed=true;++wakes;}
};
using thread_call_param_t=void*;
struct Call {void(*fn)(void*,void*);void*arg;};using thread_call_t=Call*;
static bool workerHold=false,workerFreeOk=true;static Call*pending=nullptr;
static thread_call_t thread_call_allocate(void(*f)(void*,void*),void*a){return new Call{f,a};}
static bool thread_call_enter(Call*c){if(workerHold)pending=c;else c->fn(c->arg,nullptr);return false;}
static bool thread_call_cancel(Call*c){if(pending==c){pending=nullptr;return true;}return false;}
static bool thread_call_free(Call*c){if(!workerFreeOk||pending==c)return false;delete c;return true;}
static void IOSleep(unsigned){} // deterministic bounded wait, no real sleep
static void nanoseconds_to_absolutetime(UInt64 n,UInt64*t){*t=n;}
static UInt64 mockNowNs=1;
static void clock_get_uptime(UInt64*t){*t=mockNowNs;}
static void absolutetime_to_nanoseconds(UInt64 t,UInt64*n){*n=t;}
struct Mlx4Icm{void*virt=nullptr;UInt64 bytes=0;};
struct Hardware{
 bool wedged=false,allocOk=true,handoffOk=true;unsigned allocs=0,handoffs=0,bells=0,writes=0,stamps=0,lastQpn=0;
 unsigned queryPortNumber=0,configPort=0,baseQpn=0,promisc=0,commands=0,failCommand=0,lastChecksum=0;
 std::array<UInt8,512> vlanBits{};unsigned vlanPort=0;
 bool setVlanFilter(unsigned p,const UInt8*bits,UInt32*s){vlanPort=p;*s=++commands==failCommand?3:0;if(*s)return false;if(bits)memcpy(vlanBits.data(),bits,512);else vlanBits.fill(0xff);return true;}
 UInt16 lastVlan=0;bool insertedVlan=false;std::array<UInt8,MLX4_MAC_TABLE_SIZE> macTable{};
 bool setPortMacTableWrite(unsigned p,const UInt8*t,unsigned n,UInt32*s){configPort=p;++commands;*s=commands==failCommand?3:0;if(*s||n!=macTable.size())return false;memcpy(macTable.data(),t,n);return true;}
 struct Mcast {unsigned port,mode;UInt64 mac;bool clear;};std::vector<Mcast> mcasts;
 bool queryOk=true,queryBadLength=false;UInt32 linkSpeed[2]={CX3_SPEED_10G_XFI,CX3_SPEED_40G};bool linkUp[2]={true,true};unsigned queries=0;
 void(*queryHook)(void*)=nullptr;void*queryHookArg=nullptr;
 bool queryPort(unsigned p,Cx3QueryPortResult*r){++queries;queryPortNumber=p;r->port=p;r->logMaxMacs=7;r->fwStatus=queryOk?0:3;
  r->rawLen=queryBadLength?0:CX3_QUERY_PORT_RAW_LEN;r->linkSpeed=linkSpeed[p-1];r->linkState=linkUp[p-1];
  if(queryHook){auto fn=queryHook;queryHook=nullptr;fn(queryHookArg);}return queryOk;}
 bool setPortQpnCalc(unsigned p,unsigned q,unsigned on,unsigned,unsigned,UInt32*s){configPort=p;baseQpn=q;promisc=on;*s=++commands==failCommand?3:0;return !*s;}
 bool setMcastFltr(unsigned p,UInt64 mac,bool clear,unsigned mode,UInt32*s){mcasts.push_back({p,mode,mac,clear});*s=++commands==failCommand?3:0;return !*s;}
 void*lastSq=nullptr,*lastRq=nullptr;Mlx4Icm*lastCq=nullptr;
 std::map<Mlx4Icm*,std::vector<std::array<UInt8,CQE_LEGACY_SIZE>>> completions;
 bool cmdWedged(){return wedged;}
 Mlx4Icm*icmAlloc(unsigned p,bool,bool=false){++allocs;if(!allocOk)return nullptr;return new Mlx4Icm{calloc(p,4096),UInt64(p)*4096};}
 bool markDmaHandoff(Mlx4Icm**,unsigned){++handoffs;return handoffOk;}
 bool cqNextCqe(Mlx4Icm*r,UInt32,UInt8,UInt32 c,UInt8*out){lastCq=r;auto&v=completions[r];if(c>=v.size())return false;memcpy(out,v[c].data(),CQE_LEGACY_SIZE);return true;}
 void cqSetCi(volatile UInt32*p,UInt32 v){*p=OSSwapHostToBigInt32(v);}
 UInt64 rqBufferPhys(Mlx4Icm*r,UInt32 stride,UInt32 slot){return r&&UInt64(slot)*stride<r->bytes?0x1000+UInt64(slot)*stride:0;}
 void sqWriteSendWqe(UInt8*s,UInt32 slot,UInt32,UInt32,UInt32,UInt32,UInt64,UInt32,UInt32,void*,bool,UInt32 csum,bool insert=false,UInt16 vlan=0){++writes;lastSq=s;lastChecksum=csum;insertedVlan=insert;lastVlan=vlan;memset(s+MLX4_SQ_WQE_OFFSET(slot),0x77,MLX4_TXBB_SIZE);}
 void sqRingSendDoorbell(UInt32 q,UInt32,bool){++bells;lastQpn=q;}
 void sqStampTxbb(UInt8*s,UInt32 slot,UInt32){++stamps;memset(s+MLX4_SQ_WQE_OFFSET(slot),0xcc,MLX4_TXBB_SIZE);}
 void rqWriteWqe(UInt8*r,UInt32,UInt32,UInt64,UInt32,UInt32){lastRq=r;}
 void rqSetProducer(volatile UInt32*p,UInt32 v){*p=OSSwapHostToBigInt32(v);}
 void completion(Mlx4Icm*r,unsigned q,unsigned idx,bool tx,bool err=false,unsigned bytes=68){
  std::array<UInt8,CQE_LEGACY_SIZE> c{};c[3]=UInt8(q);c[2]=UInt8(q>>8);
  c[CQE_OWNER_SR_OPCODE_OFFSET]=(tx?MLX4_CQE_IS_SEND_MASK:0)|(err?MLX4_CQE_OPCODE_ERROR:MLX4_OPCODE_SEND);
  c[CQE_WQE_INDEX_OFFSET]=UInt8(idx>>8);c[CQE_WQE_INDEX_OFFSET+1]=UInt8(idx);
  for(unsigned i=0;i<4;++i)c[CQE_BYTE_CNT_OFFSET+i]=UInt8(bytes>>(24u-8u*i));
  c[CQE_ERR_SYNDROME_OFFSET]=err?2:0;completions[r].push_back(c);
 }
};
class Cx3EthernetPort;
class Cx3Mlx4:public IOService{
public:
 Hardware fHw;Cx3EthernetPort*fNativePorts[2]{};Cx3PortQueues fPortQueues[2]{};Cx3PortState fPorts[2]{};
 struct{unsigned num_ports=2;UInt64 flags=MLX4_DEV_CAP_FLAG_VEP_MC_STEER;}fDevCap;UInt64 fDeviceGeneration=2;
 bool fResetCommandsBlocked=false,fStopRetained=false,fQuarExhausted=false,stopping=false;
 void*fNetIface=nullptr,*fNetTimer=nullptr,*fNetTxRing=nullptr;bool fNetEnabled=false;UInt32 fMrLkey=0x10000;
 bool retentionExhausted(){return fQuarExhausted;}bool selectorStopping(){return stopping;}
 bool transactionAvailable=true;unsigned selectorActive=0,transactionDepth=0;
 bool selectorEnter(){if(stopping)return false;++selectorActive;return true;}void selectorExit(){--selectorActive;}
 bool transactionTryEnter(){if(!transactionAvailable)return false;++transactionDepth;return true;}void transactionExit(){--transactionDepth;}
 bool hasNativePorts()const{return fNativePorts[0]||fNativePorts[1];}
 bool resetAdmissionAllows(UInt32)const;bool recordPortQuery(UInt32,const Cx3QueryPortResult&);
 IOReturn exportNativePort(UInt32,UInt32,UInt64,Cx3NativePortResult*);
 IOReturn exportNativeConfig(UInt32,UInt32,UInt64,UInt32,Cx3NativeConfigResult*);bool stopNativePorts();
 Cx3Mlx4(){for(unsigned p=0;p<2;++p){auto&q=fPortQueues[p];q.init(p+1);q.setupReady=q.numbersReserved=q.qpUp=q.ingressUp=q.txQpUp=q.cqUp=q.macUp=true;
   q.numberGeneration=2;q.txSqNent=q.rqNent=q.txCqNent=q.cqNent=256;q.rqBufBytes=2048;q.txQpn=65+p*2;q.qpQpn=768+p*128;
   q.numbers.macBase=q.numbers.rxQpn=q.qpQpn;q.numbers.macSlots=128;
   q.mac[2]=2;q.mac[7]=UInt8(p+1);q.txSq=new Mlx4Icm{calloc(1,256*MLX4_TXBB_SIZE+SQ_WQE0_OFFSET),256*MLX4_TXBB_SIZE+SQ_WQE0_OFFSET};
   q.rqRing=new Mlx4Icm{calloc(256,RX_WQE_SIZE),256*RX_WQE_SIZE};q.rqBufs=new Mlx4Icm{calloc(256,2048),256*2048};
   q.qpRec=new Mlx4Icm{calloc(1,8),8};*(UInt32*)q.qpRec->virt=OSSwapHostToBigInt32(256);
   q.cqRing=new Mlx4Icm;q.txCqRing=new Mlx4Icm;q.cqRec=new Mlx4Icm{calloc(1,8),8};q.txCqRec=new Mlx4Icm{calloc(1,8),8};
   fPorts[p].init(p+1,2);fPorts[p].initAccepted=fPorts[p].mtuConfigured=true;fPorts[p].frameMtu=1522;
  }}
};
'''

TESTS = r'''
static unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL %d: %s\n",__LINE__,#x);}}while(0)
static void attach(Cx3Mlx4&d,unsigned p){Cx3NativePortResult r{};CHECK(!d.exportNativePort(p,1,2,&r)&&!r.status&&(r.flags&2));}
int main(){
 Cx3NativePortResult r{};
 {Cx3Mlx4 d;auto&q=d.fPortQueues[1];q.rqBufBytes=16384;
  q.rqBufs=new Mlx4Icm{calloc(256,16384),256*16384};d.fPorts[1].frameMtu=9022;
  attach(d,2);auto*c=d.fNativePorts[1];UInt32 capacity=0;
  CHECK(!c->getMaxPacketSize(&capacity)&&capacity==9018&&c->fNetTxBufBytes==16384);
  CHECK(!c->setMaxPacketSize(9018)&&c->fPacketSize==9018&&!c->enable(c->fNetIface));
  Packet jumbo;jumbo.length=9014;jumbo.data[0]=0x33;
  CHECK(!c->outputPacket(&jumbo,nullptr)&&d.fHw.bells==1&&((UInt8*)q.rawFrame->virt)[0]==0x33);
  d.fHw.completion(q.txCqRing,q.txQpn,0,true);c->netServiceTx();CHECK(jumbo.frees==1);
  ((UInt8*)q.rqBufs->virt)[0]=0x44;d.fHw.completion(q.cqRing,q.qpQpn,0,false,false,9018);c->netServiceRx();
  CHECK(c->fNetIface->received==1&&c->fNetIface->last.length==9014&&c->fNetIface->last.data[0]==0x44);
  CHECK(c->setMaxPacketSize(9019)==kIOReturnBadArgument&&c->fPacketSize==9018);
  CHECK(!c->setMaxPacketSize(1518));Packet large;large.length=9014;
  CHECK(c->outputPacket(&large,nullptr)==kIOReturnOutputDropped&&large.frees==1&&d.fHw.bells==1);
  Packet boundary;boundary.length=1515;
  CHECK(c->outputPacket(&boundary,nullptr)==kIOReturnOutputDropped&&d.fHw.bells==1);
  Packet tag;tag.length=1518;tag.data[12]=0x81;CHECK(!c->outputPacket(&tag,nullptr)&&d.fHw.bells==2);
  d.fHw.completion(q.cqRing,q.qpQpn,1,false,false,9018);c->netServiceRx();
  CHECK(c->fNetIface->received==1&&c->fNetRxDropped==1&&q.rawRxProd==258);
  attach(d,1);auto*a=d.fNativePorts[0];CHECK(a->setMaxPacketSize(9018)==kIOReturnBadArgument&&a->fPacketSize==1518);
  c->fNetTxLock->held=true;CHECK(c->setMaxPacketSize(9018)==kIOReturnBusy&&c->fPacketSize==1518);c->fNetTxLock->held=false;
  CHECK(d.stopNativePorts());
 }
 {Cx3Mlx4 d;attach(d,1);attach(d,2);auto*a=d.fNativePorts[0];auto*b=d.fNativePorts[1];
  CHECK(a!=b&&a->getWorkLoop()!=b->getWorkLoop()&&a->fNetTimer!=b->fNetTimer&&a->fNetTxLock!=b->fNetTxLock);
  CHECK(a->fPort==1&&b->fPort==2&&a->fGeneration==2&&b->fGeneration==2&&a->fNetUnit!=b->fNetUnit);
  CHECK(!a->bind(&d,2,2,b->fMac));CHECK(!a->enable(a->fNetIface)&&!b->enable(b->fNetIface));
  Packet x,y,z;x.data[0]=0x11;y.data[0]=0x22;
  CHECK(!a->outputPacket(&x,nullptr)&&!b->outputPacket(&y,nullptr));
  CHECK(d.fHw.lastQpn==d.fPortQueues[1].txQpn&&d.fHw.lastSq==d.fPortQueues[1].txSq->virt);
  CHECK(((UInt8*)d.fPortQueues[0].rawFrame->virt)[0]==0x11&&((UInt8*)d.fPortQueues[1].rawFrame->virt)[0]==0x22);
  CHECK(a->fNetTxMbuf[0]==&x&&b->fNetTxMbuf[0]==&y&&d.fPortQueues[0].rawTxProd==1&&d.fPortQueues[1].rawTxProd==1);
  d.fHw.completion(d.fPortQueues[1].txCqRing,d.fPortQueues[1].txQpn,0,true,true);b->netServiceTx();
  CHECK(y.frees==1&&!x.frees&&b->fNetTxFault.latched&&!a->fNetTxFault.latched&&d.fPortQueues[0].rawTxCons==0);
  CHECK(b->outputPacket(&z,nullptr)==kIOReturnOutputDropped&&z.frees==1&&d.fHw.bells==2);
  auto&q=d.fPortQueues[1];((UInt8*)q.rqBufs->virt)[0]=0x42;
  d.fHw.completion(q.cqRing,q.qpQpn,0,false);b->netServiceRx();
  CHECK(b->fNetRxFrames==1&&b->fNetIface->last.data[0]==0x42&&!a->fNetRxFrames);
  CHECK(q.rawRxCons==1&&q.rawRxProd==257&&d.fPortQueues[0].rawRxProd==0&&d.fPortQueues[0].rawRxCons==0);
  CHECK(d.fHw.lastRq==q.rqRing->virt&&q.rawRxRecycles[0]==1&&!d.fPortQueues[0].rawRxRecycles[0]);
  d.fHw.completion(q.cqRing,q.qpQpn,1,false,true);b->netServiceRx();auto cons=q.rawRxCons;
  b->netServiceRx();CHECK(b->fNetRxFault&&q.rawRxCons==cons&&q.rawRxProd==257&&b->fNetRxFrames==1);
  b->snapshot(&r);CHECK((r.flags&CX3_NATIVE_RX_FAULT)&&(r.flags&CX3_NATIVE_TX_FAULT));
  auto*frames=q.rawFrame;d.fHw.wedged=true;d.fQuarExhausted=true;CHECK(!d.exportNativePort(2,2,2,&r)&&!r.status&&!r.flags);
  CHECK(d.fNativePorts[0]==a&&a->fNetEnabled&&d.fPortQueues[1].rawFrame==frames&&a->fNetTxMbuf[0]==&x);
  d.fHw.wedged=false;d.fQuarExhausted=false;CHECK(!d.exportNativePort(2,1,2,&r)&&r.status&&!d.fNativePorts[1]); // explicit queue rebuild required
  d.fDeviceGeneration=3;auto bells=d.fHw.bells;Packet stale;
  CHECK(a->outputPacket(&stale,nullptr)==kIOReturnOutputDropped&&stale.frees==1&&d.fHw.bells==bells);
  auto wakes=a->fNetTimer->wakes;a->closeAdmission();a->netTick();CHECK(a->fNetTimer->wakes==wakes);
  Packet closed;CHECK(a->outputPacket(&closed,nullptr)==kIOReturnOutputStall&&!closed.frees);
  d.fDeviceGeneration=2;CHECK(d.stopNativePorts()&&!d.hasNativePorts()&&x.frees==1&&d.fPortQueues[0].rawFrame);
 }
 {Cx3Mlx4 d;d.fNetIface=&d;CHECK(!d.exportNativePort(1,1,2,&r)&&r.status&&!d.fNativePorts[0]);
  CHECK(!d.exportNativePort(2,1,1,&r)&&r.status&&!d.fNativePorts[1]&&!d.fHw.allocs);
  d.fNetIface=nullptr;d.fPortQueues[1].txSqNent=257;CHECK(!d.exportNativePort(2,1,2,&r)&&r.status&&!d.fHw.allocs);}
 {Cx3Mlx4 d;startOk=false;CHECK(!d.exportNativePort(2,1,2,&r)&&r.status&&d.fNativePorts[1]);
  CHECK(!d.fHw.allocs&&!d.fPortQueues[1].rawFrame);startOk=true;CHECK(!d.exportNativePort(2,2,2,&r)&&!r.status&&!d.hasNativePorts());}
 {Cx3Mlx4 d;attachOk=false;CHECK(!d.exportNativePort(2,1,2,&r)&&r.status&&d.fNativePorts[1]&&d.fPortQueues[1].rawFrame);
  CHECK(d.fNativePorts[1]->poisoned());attachOk=true;auto allocs=d.fHw.allocs;
  CHECK(!d.exportNativePort(2,1,2,&r)&&r.status&&d.fHw.allocs==allocs);CHECK(d.stopNativePorts());}
 {Cx3Mlx4 d;workerHold=true;CHECK(!d.exportNativePort(2,1,2,&r)&&r.status&&d.fNativePorts[1]&&pending);
  auto*c=d.fNativePorts[1];auto*frame=d.fPortQueues[1].rawFrame;CHECK(!d.stopNativePorts()&&d.fNativePorts[1]==c&&d.fPortQueues[1].rawFrame==frame);
  auto*call=pending;pending=nullptr;workerHold=false;call->fn(call->arg,nullptr); // late completion remains owned
  CHECK(c->enable(c->fNetIface)==kIOReturnNotPermitted);
  CHECK(d.stopNativePorts()&&!d.hasNativePorts()&&d.fPortQueues[1].rawFrame==frame);}
 {Cx3Mlx4 d;attach(d,1);attach(d,2);IOWorkLoop::removeOk=false;auto*c=d.fNativePorts[0];
  CHECK(!d.stopNativePorts()&&d.fNativePorts[0]==c&&d.fNativePorts[1]&&c->fNetTimer&&c->fNetTxAdmission.isClosed()&&d.fNativePorts[1]->fNetTxAdmission.isClosed());
  IOWorkLoop::removeOk=true;workerFreeOk=false;CHECK(!d.stopNativePorts()&&d.fNativePorts[0]==c&&c->fWorker);
  workerFreeOk=true;CHECK(d.stopNativePorts());}
 {Cx3Mlx4 d;attach(d,2);CHECK(d.resetAdmissionAllows(kCx3SelNativePort)&&d.resetAdmissionAllows(kCx3SelPortInfo));
  CHECK(!d.resetAdmissionAllows(kCx3SelResetHca)&&!d.resetAdmissionAllows(kCx3SelAutoNetwork));CHECK(d.stopNativePorts());}
 {Cx3Mlx4 d;Cx3Mlx4UserClient uc{&d};UInt64 in[]={2,1,2};IOExternalMethodArguments a;a.scalarInput=in;a.structureOutput=&r;
  CHECK(!uc.doNativePort(&a)&&!r.status&&d.fNativePorts[1]);in[0]=(1ull<<32)+2;CHECK(uc.doNativePort(&a)==kIOReturnBadArgument);
  CHECK(d.stopNativePorts());}
 {Cx3Mlx4 d;driver=&d;samples=transports=0;drift=0;CHECK(!cmdNativePort(2,1,"native-smoke",0)&&transports==1);
  CHECK(!cmdNativePort(2,0,"native-smoke",1));CHECK(!cmdNativePort(2,2,"native-smoke",0));}
 for(int change:{1,2,3}){Cx3Mlx4 d;driver=&d;drift=change;samples=transports=0;CHECK(cmdNativePort(2,0,"native-smoke",0)==1);CHECK(transports==(change==3?0u:1u));}drift=0;
 {Cx3Mlx4 d;driver=&d;samples=transports=0;CHECK(cmdNativePort(2,1,"wrong",0)==1&&!transports&&!d.fHw.allocs);}
 {Cx3Mlx4 d;attach(d,1);attach(d,2);auto*a=d.fNativePorts[0];auto*b=d.fNativePorts[1];
  IOEthernetAddress group{{1,0,0x5e,0,0,1}},bad{{2,0,0,0,0,1}};UInt32 support=0;
  CHECK(!b->getChecksumSupport(&support,kChecksumFamilyTCPIP,true)&&support==31);
  CHECK(b->getChecksumSupport(&support,99,true)==kIOReturnUnsupported&&!support);
  CHECK(!b->setMulticastList(&group,1)&&!b->setMulticastMode(true)&&!d.fHw.commands&&!b->fFilterPrograms);
  d.transactionAvailable=false;CHECK(b->enable(b->fNetIface)==kIOReturnBusy&&!b->poisoned()&&!d.fHw.commands);
  CHECK(!d.selectorActive&&!d.transactionDepth&&b->fNetTxAdmission.drained());d.transactionAvailable=true;
  CHECK(!b->enable(b->fNetIface)&&b->fFilterPrograms==1&&d.fHw.queryPortNumber==2&&d.fHw.configPort==2&&d.fHw.baseQpn==896);
  CHECK(!a->fFilterPrograms&&d.fHw.mcasts.size()==4&&d.fHw.mcasts[1].mac==0xffffffffffffull&&d.fHw.mcasts[1].clear);
  CHECK(d.fHw.mcasts[2].mac==0x01005e000001ull&&d.fHw.mcasts[2].port==2&&d.fHw.mcasts.back().mode==MLX4_MCAST_ENABLE);
  auto revision=b->fFilterPrograms;auto commands=d.fHw.commands;
  CHECK(b->setMulticastList(&bad,1)==kIOReturnBadArgument&&b->fFilterPrograms==revision&&d.fHw.commands==commands);
  CHECK(!b->setPromiscuousMode(true)&&d.fHw.promisc==1&&d.fHw.mcasts.back().mode==MLX4_MCAST_DISABLE);
  CHECK(!b->setPromiscuousMode(false)&&d.fHw.promisc==0&&d.fHw.mcasts.back().mode==MLX4_MCAST_ENABLE);
  IOEthernetAddress many[129]{};CHECK(!b->setMulticastList(many,129)&&b->fFilterApplied.overflow&&d.fHw.mcasts.back().mode==MLX4_MCAST_DISABLE);
  CHECK(!b->setMulticastList(&group,1)&&!b->fFilterApplied.overflow&&d.fHw.mcasts.back().mode==MLX4_MCAST_ENABLE);
  d.transactionAvailable=false;CHECK(b->setPromiscuousMode(true)==kIOReturnBusy&&!b->fFilterDesired.promisc&&!b->poisoned());d.transactionAvailable=true;
  Packet tx;tx.demand=kChecksumIP|kChecksumTCPIPv6;CHECK(!b->outputPacket(&tx,nullptr)&&d.fHw.lastChecksum==(MLX4_WQE_CTRL_IP_CSUM|MLX4_WQE_CTRL_TCP_UDP_CSUM));
  auto&q=d.fPortQueues[1];d.fHw.completion(q.cqRing,q.qpQpn,0,false);auto&c=d.fHw.completions[q.cqRing][0];
  UInt16 status=MLX4_CQE_STATUS_IPV4|MLX4_CQE_STATUS_TCP|MLX4_CQE_STATUS_IPOK;
  c[CQE_STATUS_OFFSET]=status>>8;c[CQE_STATUS_OFFSET+1]=status;c[CQE_CHECKSUM_OFFSET]=c[CQE_CHECKSUM_OFFSET+1]=0xff;
  b->netServiceRx();CHECK(b->fNetIface->last.checksumMask==(kChecksumIP|kChecksumTCP)&&b->fNetRxCsumGood==1&&!a->fNetRxCsumGood);
  CHECK(!d.selectorActive&&!d.transactionDepth&&d.stopNativePorts());}
 {Cx3Mlx4 d;attach(d,1);attach(d,2);auto*c=d.fNativePorts[1];auto*frames=d.fPortQueues[1].rawFrame;
  d.fHw.failCommand=2;CHECK(c->enable(c->fNetIface)==kIOReturnError&&c->poisoned()&&c->fNetTxAdmission.isClosed()&&!c->fNetEnabled);
  CHECK(!c->fFilterPrograms&&c->fFilterFwStatus==3&&d.fPortQueues[1].rawFrame==frames&&!d.fNativePorts[0]->poisoned());
  CHECK(!d.selectorActive&&!d.transactionDepth&&d.stopNativePorts());}
 {Cx3Mlx4 d;attach(d,2);auto*c=d.fNativePorts[1];workerHold=true;
  CHECK(c->enable(c->fNetIface)==kIOReturnTimeout&&c->poisoned()&&pending&&!d.fHw.commands);
  CHECK(!d.stopNativePorts()&&d.fNativePorts[1]==c);auto*call=pending;pending=nullptr;workerHold=false;call->fn(call->arg,nullptr);
  CHECK(!d.fHw.commands&&!d.selectorActive&&!d.transactionDepth&&d.stopNativePorts());}

 {Cx3Mlx4 d;attach(d,1);attach(d,2);auto*a=d.fNativePorts[0];auto*c=d.fNativePorts[1];auto&q=d.fPortQueues[1];
  CHECK(c->getFeatures()&kIONetworkFeatureHardwareVlan);CHECK(!c->enable(c->fNetIface));
  Packet tx;tx.hasVlan=true;tx.vlan=0xa123;tx.length=1514;tx.demand=kChecksumTCP;
  CHECK(!c->outputPacket(&tx,nullptr)&&d.fHw.insertedVlan&&d.fHw.lastVlan==0xa123&&d.fHw.lastChecksum==MLX4_WQE_CTRL_TCP_UDP_CSUM);
  auto bells=d.fHw.bells;Packet duplicate;duplicate.hasVlan=true;duplicate.vlan=1;duplicate.data[12]=0x81;
  CHECK(c->outputPacket(&duplicate,nullptr)==kIOReturnOutputDropped&&duplicate.frees==1&&d.fHw.bells==bells);
  Packet invalid;invalid.hasVlan=true;invalid.vlan=4095;
  CHECK(c->outputPacket(&invalid,nullptr)==kIOReturnOutputDropped&&invalid.frees==1&&d.fHw.bells==bells);
  Packet big;big.hasVlan=true;big.vlan=7;big.length=1515;
  CHECK(c->outputPacket(&big,nullptr)==kIOReturnOutputDropped&&d.fHw.bells==bells);
  Packet priority;priority.hasVlan=true;priority.vlan=0xe000;
  CHECK(!c->outputPacket(&priority,nullptr)&&d.fHw.insertedVlan&&d.fHw.lastVlan==0xe000);
  d.fHw.completion(q.cqRing,q.qpQpn,0,false,false,1518);auto&rx=d.fHw.completions[q.cqRing][0];
  rx[0]|=0x20;rx[CQE_SL_VID_OFFSET]=0xa1;rx[CQE_SL_VID_OFFSET+1]=0x23;
  c->netServiceRx();CHECK(c->fNetIface->received==1&&c->fNetIface->last.length==1514&&c->fNetIface->last.hasVlan&&c->fNetIface->last.vlan==0xa123);
  auto*frame=(UInt8*)q.rqBufs->virt+q.rqBufBytes;frame[12]=0x81;frame[14]=0xe0;frame[15]=7;frame[16]=8;frame[17]=0;frame[18]=0x45;
  d.fHw.completion(q.cqRing,q.qpQpn,1,false,false,1522);c->netServiceRx();
  auto&last=c->fNetIface->last;CHECK(c->fNetIface->received==2&&last.hasVlan&&last.vlan==0xe007&&last.length==1514&&last.data[12]==8&&last.data[13]==0&&last.data[14]==0x45);
  auto*untag=(UInt8*)q.rqBufs->virt+2*q.rqBufBytes;untag[12]=8;
  d.fHw.completion(q.cqRing,q.qpQpn,2,false);c->netServiceRx();CHECK(c->fNetIface->received==3&&!c->fNetIface->last.hasVlan);
  d.fHw.completion(q.cqRing,q.qpQpn,3,false,false,1519);auto&over=d.fHw.completions[q.cqRing][3];over[0]|=0x20;
  c->netServiceRx();CHECK(c->fNetIface->received==3&&c->fNetRxDropped==1&&q.rawRxProd==260);
  IOEthernetAddress mac{{2,0x44,0x55,0x66,0x77,0x88}},bad{{1,0,0,0,0,1}},zero{};UInt8 old[6];memcpy(old,c->fMac,6);
  auto commands=d.fHw.commands;CHECK(c->setHardwareAddress(&bad)==kIOReturnBadArgument&&c->setHardwareAddress(&zero)==kIOReturnBadArgument&&d.fHw.commands==commands);
  d.transactionAvailable=false;CHECK(c->setHardwareAddress(&mac)==kIOReturnBusy&&!memcmp(old,c->fMac,6)&&!c->poisoned());d.transactionAvailable=true;
  CHECK(!c->setHardwareAddress(&mac)&&d.fHw.configPort==2&&d.fHw.macTable[0]==0x80&&!memcmp(d.fHw.macTable.data()+2,mac.bytes,6));
  CHECK(!memcmp(c->fMac,mac.bytes,6)&&!memcmp(q.mac+2,mac.bytes,6)&&q.macValue==0x024455667788ull&&memcmp(a->fMac,mac.bytes,6));
  commands=d.fHw.commands;CHECK(!c->setHardwareAddress(&mac)&&d.fHw.commands==commands);CHECK(d.stopNativePorts());
 }
 {Cx3Mlx4 d;attach(d,2);auto*c=d.fNativePorts[1];UInt8 old[6];memcpy(old,c->fMac,6);IOEthernetAddress mac{{2,3,4,5,6,7}};
  d.fHw.failCommand=1;auto*owner=d.fPortQueues[1].rawFrame;
  CHECK(c->setHardwareAddress(&mac)==kIOReturnError&&c->poisoned()&&c->fNetTxAdmission.isClosed()&&!memcmp(old,c->fMac,6)&&d.fPortQueues[1].rawFrame==owner);
  CHECK(d.stopNativePorts());
 }
 {Cx3Mlx4 d;attach(d,2);auto*c=d.fNativePorts[1];IOEthernetAddress mac{{2,3,4,5,6,7}};workerHold=true;
  CHECK(c->setHardwareAddress(&mac)==kIOReturnTimeout&&c->poisoned()&&pending&&!d.fHw.commands);
  CHECK(!d.stopNativePorts());auto*call=pending;pending=nullptr;workerHold=false;call->fn(call->arg,nullptr);
  CHECK(!d.fHw.commands&&d.stopNativePorts());
 }

 {Cx3Mlx4 d;Cx3NativeConfigResult config{};auto&q=d.fPortQueues[1];
  CHECK(!d.exportNativeConfig(2,CX3_NCFG_TXCSUM,2,0,&config)&&!config.status&&!q.nativeTxCsum&&!d.fHw.commands);
  CHECK(!d.exportNativeConfig(2,CX3_NCFG_VLAN_CLEAR,2,0,&config)&&!config.status&&!config.vlanCount);
  CHECK(!d.exportNativeConfig(2,CX3_NCFG_VLAN_ADD,2,0,&config)&&!config.status&&config.vlanCount==1);
  CHECK(!d.exportNativeConfig(2,CX3_NCFG_VLAN_ADD,2,4094,&config)&&!config.status&&config.vlanCount==2&&!d.fHw.commands);
  CHECK(d.exportNativeConfig(2,CX3_NCFG_VLAN_ADD,2,4095,&config)==kIOReturnBadArgument);
  CHECK(!d.exportNativeConfig(2,CX3_NCFG_VLAN_CLEAR,1,0,&config)&&config.status&&q.vlanBits[0]==1);
  attach(d,2);auto*c=d.fNativePorts[1];UInt32 mask=99;
  CHECK(!c->getChecksumSupport(&mask,kChecksumFamilyTCPIP,true)&&mask==0);
  CHECK(!c->enable(c->fNetIface)&&d.fHw.vlanPort==2&&d.fHw.vlanBits[0]==1&&d.fHw.vlanBits[511]==0x40&&q.vlanApplied&&q.vlanRevision==1);
  CHECK(!d.exportNativeConfig(2,CX3_NCFG_TXCSUM,2,1,&config)&&config.status==UInt64(kIOReturnBusy)&&!q.nativeTxCsum);
  CHECK(!d.exportNativeConfig(2,CX3_NCFG_VLAN_ADD,2,37,&config)&&!config.status&&config.vlanCount==3&&d.fHw.vlanBits[4]==0x20);
  CHECK(!d.exportNativeConfig(2,CX3_NCFG_VLAN_DEL,2,37,&config)&&!config.status&&config.vlanCount==2&&!d.fHw.vlanBits[4]);
  CHECK(!c->setPromiscuousMode(true)&&d.fHw.vlanBits[4]==0xff);
  CHECK(!c->setPromiscuousMode(false)&&d.fHw.vlanBits[0]==1&&d.fHw.vlanBits[4]==0);
  auto commands=d.fHw.commands;c->fFilterLock->held=true;
  CHECK(!d.exportNativeConfig(2,CX3_NCFG_VLAN_CLEAR,2,0,&config)&&config.status==UInt64(kIOReturnBusy)&&config.vlanCount==2&&d.fHw.commands==commands);c->fFilterLock->held=false;
  CHECK(!d.exportNativeConfig(2,CX3_NCFG_RXCSUM,2,0,&config)&&!config.status&&!c->fNetRxCsum);
  CHECK(!c->getChecksumSupport(&mask,kChecksumFamilyTCPIP,false)&&mask==31);
  d.fHw.completion(q.cqRing,q.qpQpn,0,false);auto&rx=d.fHw.completions[q.cqRing][0];
  UInt16 status=MLX4_CQE_STATUS_IPV4|MLX4_CQE_STATUS_TCP|MLX4_CQE_STATUS_IPOK;
  rx[CQE_STATUS_OFFSET]=status>>8;rx[CQE_STATUS_OFFSET+1]=status;rx[CQE_CHECKSUM_OFFSET]=rx[CQE_CHECKSUM_OFFSET+1]=0xff;
  c->netServiceRx();CHECK(c->fNetIface->received==1&&!c->fNetIface->last.checksumMask);
  CHECK(!d.exportNativeConfig(2,CX3_NCFG_VLAN_ALL,2,0,&config)&&!config.status&&config.vlanCount==4095&&!(config.flags&CX3_NCFG_VLAN_FILTER)&&d.fHw.vlanBits[0]==0xff);
  CHECK(!d.exportNativeConfig(2,CX3_NCFG_VLAN_ADD,2,1,&config)&&config.status==UInt64(kIOReturnBadArgument));CHECK(d.stopNativePorts());
 }
 {Cx3Mlx4 d;attach(d,1);attach(d,2);Cx3NativeConfigResult config{};auto*c=d.fNativePorts[1];auto*q=&d.fPortQueues[1];auto*owner=q->rawFrame;
  d.fHw.failCommand=1;CHECK(!d.exportNativeConfig(2,CX3_NCFG_VLAN_CLEAR,2,0,&config)&&config.status&&c->poisoned()&&!q->vlanFilter&&!q->vlanApplied&&q->rawFrame==owner&&!d.fNativePorts[0]->poisoned());
  CHECK(d.stopNativePorts());
 }
 {Cx3Mlx4 d;Cx3NativeConfigResult config{};Cx3Mlx4UserClient uc{&d};UInt64 in[]={2,CX3_NCFG_VLAN_ADD,2,1ull<<32};IOExternalMethodArguments a;
  a.scalarInputCount=4;a.scalarInput=in;a.structureOutput=&config;a.structureOutputSize=sizeof(config);
  CHECK(uc.doNativeConfig(&a)==kIOReturnBadArgument&&!d.fHw.commands);in[3]=4095;CHECK(uc.doNativeConfig(&a)==kIOReturnBadArgument);
  in[1]=CX3_NCFG_RXCSUM;in[3]=0;CHECK(!uc.doNativeConfig(&a)&&!config.status&&!d.fPortQueues[1].nativeRxCsum);
 }
 {Cx3Mlx4 d;driver=&d;samples=transports=0;drift=0;CHECK(!cmdNativeConfig(2,CX3_NCFG_VLAN_CLEAR,0,"native-smoke",0)&&transports==1);
  CHECK(!cmdNativeConfig(2,CX3_NCFG_VLAN_ADD,37,"native-smoke",1)&&d.fPortQueues[1].vlanBits[4]==0x20);
  CHECK(cmdNativeConfig(2,CX3_NCFG_VLAN_ADD,4095,"native-smoke",0)==2);CHECK(cmdNativeConfig(2,0,0,"wrong",0)==1);
 }
 for(int change:{1,2,3}){Cx3Mlx4 d;driver=&d;drift=change;samples=transports=0;CHECK(cmdNativeConfig(2,0,0,"native-smoke",0)==1&&transports==(change==3?0u:1u));}drift=0;

 {mockNowNs=10000000000ull;Cx3Mlx4 d;attach(d,1);attach(d,2);auto*a=d.fNativePorts[0];auto*c=d.fNativePorts[1];
  CHECK(!a->enable(a->fNetIface)&&!c->enable(c->fNetIface)&&a->linkFlags==0&&c->linkFlags==0);
  a->serviceLink();a->serviceLink();c->serviceLink();c->serviceLink();
  CHECK(a->fLinkKnown&&c->fLinkKnown&&a->linkBps==10000000000ull&&c->linkBps==40000000000ull&&a->linkFlags==3&&c->linkFlags==3);
  CHECK(c->fMediumActive&&c->fMediumActive->speed==40000000000ull&&c->activeMedium==c->fMediumActive&&d.fPorts[1].speedMbps==40000);
  auto queries=d.fHw.queries;auto properties=c->properties;auto*owner=d.fPortQueues[1].rawFrame;
  c->serviceLink();CHECK(d.fHw.queries==queries&&c->properties==properties&&!d.selectorActive&&!d.transactionDepth);
  mockNowNs+=1000000000ull;d.fHw.linkSpeed[1]=CX3_SPEED_20G;c->serviceLink();c->serviceLink();
  CHECK(c->linkBps==20000000000ull&&c->fMediumActive->speed==20000000000ull&&a->linkBps==10000000000ull);
  mockNowNs+=1000000000ull;d.fHw.linkUp[1]=false;c->serviceLink();c->serviceLink();
  CHECK(c->fLinkKnown&&!c->fLinkUp&&c->linkFlags==1&&!c->linkBps&&a->linkFlags==3);
  mockNowNs+=1000000000ull;d.fHw.linkUp[1]=true;d.fHw.linkSpeed[1]=0x6f;c->serviceLink();c->serviceLink();
  CHECK(c->fLinkKnown&&c->fLinkUp&&c->linkFlags==3&&!c->linkBps&&c->activeMedium==c->fMediumAuto);
  mockNowNs+=1000000000ull;d.fHw.linkSpeed[1]=CX3_SPEED_40G;IONetworkMedium::fail=true;c->serviceLink();c->serviceLink();
  CHECK(c->fLinkKnown&&c->linkBps==40000000000ull&&c->activeMedium==c->fMediumAuto&&c->fMediumSpeed==20000000000ull);IONetworkMedium::fail=false;
  mockNowNs+=1000000000ull;c->serviceLink();c->serviceLink();CHECK(c->activeMedium==c->fMediumActive&&c->fMediumSpeed==40000000000ull);
  d.transactionAvailable=false;queries=d.fHw.queries;mockNowNs+=1000000000ull;c->serviceLink();c->serviceLink();
  CHECK(c->fLinkKnown&&c->fLinkBusy==1&&d.fHw.queries==queries);
  mockNowNs+=2100000000ull;c->serviceLink();CHECK(!c->fLinkKnown&&c->linkFlags==0&&!c->linkBps&&d.fHw.queries==queries);
  d.transactionAvailable=true;mockNowNs+=1000000000ull;c->serviceLink();c->serviceLink();CHECK(c->fLinkKnown&&c->linkFlags==3);
  mockNowNs+=1000000000ull;d.fHw.queryBadLength=true;c->serviceLink();c->serviceLink();
  CHECK(!c->fLinkKnown&&c->linkFlags==0&&!c->fLinkMonitoring&&c->fLinkFailures==1&&d.fPorts[1].lastQueryFailed);
  queries=d.fHw.queries;mockNowNs+=10000000000ull;c->serviceLink();CHECK(d.fHw.queries==queries);
  d.fHw.queryBadLength=false;CHECK(!c->disable(c->fNetIface)&&!c->enable(c->fNetIface));c->serviceLink();c->serviceLink();
  CHECK(c->fLinkKnown&&c->fLinkMonitoring&&d.fPortQueues[1].rawFrame==owner&&!d.selectorActive&&!d.transactionDepth);
  CHECK(d.stopNativePorts());
 }
 {mockNowNs+=1000000000ull;Cx3Mlx4 d;attach(d,2);auto*c=d.fNativePorts[1];CHECK(!c->enable(c->fNetIface));
  auto before=d.fHw.queries;workerHold=true;c->serviceLink();CHECK(pending==c->fLinkCall&&c->fLinkInFlight&&d.fHw.queries==before);
  auto epoch=c->fLinkEpoch;CHECK(!c->disable(c->fNetIface)&&!pending&&!c->fLinkInFlight&&c->fLinkEpoch>epoch);
  c->serviceLink();CHECK(d.fHw.queries==before&&!c->fLinkKnown);workerHold=false;
  CHECK(!c->enable(c->fNetIface));c->serviceLink();c->serviceLink();CHECK(c->fLinkKnown&&d.stopNativePorts());
 }
 {mockNowNs+=1000000000ull;Cx3Mlx4 d;attach(d,2);auto*c=d.fNativePorts[1];CHECK(!c->enable(c->fNetIface));
  auto oldCache=d.fPorts[1].queryUptimeNs;
  d.fHw.queryHook=[](void*p){auto*c=(Cx3EthernetPort*)p;c->disable(c->fNetIface);};d.fHw.queryHookArg=c;
  c->serviceLink();c->serviceLink();CHECK(!c->fNetEnabled&&!c->fLinkInFlight&&!c->fLinkHaveResult&&!c->fLinkKnown&&d.fPorts[1].queryUptimeNs==oldCache&&c->linkFlags==1);
  CHECK(!c->enable(c->fNetIface));c->serviceLink();c->serviceLink();CHECK(c->fLinkKnown&&c->fLinkUp&&d.stopNativePorts());
 }
 {mockNowNs+=1000000000ull;Cx3Mlx4 d;attach(d,2);auto*c=d.fNativePorts[1];CHECK(!c->enable(c->fNetIface));auto*owner=d.fPortQueues[1].rawFrame;
  struct Attempt{Cx3EthernetPort*c;bool completed;};Attempt attempt{c,true};
  d.fHw.queryHook=[](void*p){auto*a=(Attempt*)p;a->completed=a->c->shutdown();};d.fHw.queryHookArg=&attempt;
  c->serviceLink();CHECK(!attempt.completed&&c->fNetTxAdmission.isClosed()&&c->fLinkRef&&c->fLinkCall&&!c->fLinkInFlight&&!c->fLinkHaveResult&&d.fPortQueues[1].rawFrame==owner);
  CHECK(c->enable(c->fNetIface)==kIOReturnNotPermitted&&!d.selectorActive&&!d.transactionDepth&&d.stopNativePorts());
 }
 {mockNowNs+=1000000000ull;Cx3Mlx4 d;attach(d,2);auto*c=d.fNativePorts[1];CHECK(!c->enable(c->fNetIface));
  d.fHw.queryHook=[](void*p){++((Cx3Mlx4*)p)->fDeviceGeneration;};d.fHw.queryHookArg=&d;
  c->serviceLink();c->serviceLink();CHECK(!c->fLinkKnown&&!d.fPorts[1].queryValid&&c->linkFlags==0&&d.stopNativePorts());
 }
 {mockNowNs+=1000000000ull;Cx3Mlx4 d;attach(d,2);auto*c=d.fNativePorts[1];CHECK(!c->enable(c->fNetIface));
  Packet held;CHECK(!c->outputPacket(&held,nullptr));auto prod=d.fPortQueues[1].rawTxProd;
  CHECK(!c->disable(c->fNetIface)&&!held.frees&&d.fPortQueues[1].rawTxProd==prod);
  CHECK(!c->enable(c->fNetIface)&&d.fPortQueues[1].rawTxProd==prod);
  d.fHw.completion(d.fPortQueues[1].txCqRing,d.fPortQueues[1].txQpn,0,true);c->netServiceTx();CHECK(held.frees==1&&d.fPortQueues[1].rawTxCons==1&&d.stopNativePorts());
 }
 printf("native port: %u checks, %u failures (real controller/PF/UC/CLI; synthetic IOKit/HW/scheduler)\n",checks,failures);return failures?1:0;
}
'''

UC = r'''
struct IOExternalMethodArguments{UInt32 scalarInputCount=3,scalarOutputCount=0;UInt64*scalarInput=nullptr;const void*structureInput=nullptr;size_t structureInputSize=0;void*structureOutput=nullptr;size_t structureOutputSize=120;};
struct Cx3Mlx4UserClient{Cx3Mlx4*fProvider;IOReturn doNativePort(IOExternalMethodArguments*);IOReturn doNativeConfig(IOExternalMethodArguments*);};
'''
TRANSPORT = r'''
static Cx3Mlx4*driver=nullptr;static unsigned samples=0,transports=0;static int drift=0,gConn=1;
static int readRuntimeSample(Cx3RuntimeSample*s){memset(s,0,sizeof(*s));++samples;s->connectedServiceRegistryId=100;s->returnedBytes=sizeof(s->result);strcpy(s->bootSessionUuid,"00000001-0000-0000-0000-000000000001");auto&r=s->result;r.version=CX3_RUNTIME_RESULT_VERSION;r.structBytes=sizeof(r);r.validity=63;r.serviceRegistryId=100;r.providerRegistryId=drift==1&&samples>1?201:200;r.sampleUptimeNs=samples;r.deviceGeneration=driver->fDeviceGeneration+(drift==2&&samples>1);r.lifecycleFlags=drift==3?1:16;r.pciIdentity.vendor=0x15b3;r.pciIdentity.device=0x1007;r.pciIdentity.subsystemVendor=0x15b3;r.pciIdentity.subsystemId=1;strcpy(r.bundleVersion,"0.44.48");strcpy(r.buildTag,"native-smoke");return 0;}
static int IOConnectCallMethod(int,unsigned sel,const uint64_t*in,unsigned n,const void*,size_t,uint64_t*,uint32_t*outCount,void*out,size_t*bytes){++transports;if(sel==55&&n==4&&*bytes==56){*outCount=0;return driver->exportNativeConfig(in[0],in[1],in[2],in[3],(Cx3NativeConfigResult*)out);}if(sel!=54||n!=3||*bytes!=120)return -1;*outCount=0;return driver->exportNativePort(in[0],in[1],in[2],(Cx3NativePortResult*)out);}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='cx3-native-port-') as directory:
        p = Path(directory)
        (p / 'common.h').write_text(COMMON)
        for path in ('IOKit/network/IOEthernetController.h', 'IOKit/network/IOEthernetInterface.h',
                     'IOKit/IOTimerEventSource.h', 'IOKit/IOWorkLoop.h', 'IOKit/network/IOOutputQueue.h',
                     'IOKit/network/IONetworkMedium.h', 'IOKit/IOLib.h', 'kern/thread_call.h',
                     'kern/clock.h', 'sys/kpi_mbuf.h', 'Mlx4Hw.h', 'Cx3Mlx4.h', 'Cx3Debug.h'):
            f = p / path
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_text('#include "common.h"\n')
        # Change access for host assertions only; every method body is verbatim.
        (p / 'Cx3EthernetPort.h').write_text((ROOT / 'src/Cx3EthernetPort.h').read_text().replace('private:', 'public:'))
        (p / 'Cx3EthernetPort.cpp').write_text((ROOT / 'src/Cx3EthernetPort.cpp').read_text())
        code = '#include "Cx3EthernetPort.cpp"\n'
        for sig in ('IOReturn Cx3Mlx4::exportNativePort(', 'bool Cx3Mlx4::stopNativePorts()',
                    'bool Cx3Mlx4::resetAdmissionAllows(', 'IOReturn Cx3Mlx4::exportNativeConfig(',
                    'bool Cx3Mlx4::recordPortQuery('):
            code += method('src/Cx3Mlx4.cpp', sig)
        code += UC + method('src/Cx3Mlx4UserClient.cpp', 'IOReturn Cx3Mlx4UserClient::doNativePort(')
        code += method('src/Cx3Mlx4UserClient.cpp', 'IOReturn Cx3Mlx4UserClient::doNativeConfig(')
        code += '\nstruct Cx3RuntimeSample{Cx3RuntimeIdentityResult result;uint64_t connectedServiceRegistryId;char bootSessionUuid[37];uint32_t transport,scalarOutputCount;size_t returnedBytes;};\n'
        for sig in ('static int runtimeUuidValid(', 'static int runtimeMetadataValid(',
                    'static int runtimeIdentityShapeValid(', 'static void runtimeEmitHex(',
                    'static void emitRuntimeSample(', 'static int runtimeSameInstance('):
            code += method('tools/cx3ctl.c', sig)
        code += TRANSPORT + method('tools/cx3ctl.c', 'static int cmdNativePort(') + method('tools/cx3ctl.c', 'static int cmdNativeConfig(') + TESTS
        (p / 'main.cpp').write_text(code)
        subprocess.run(['/usr/bin/clang++', '-std=c++14', '-Wall', '-Wextra', '-Wno-unused-function',
                        '-Wno-unused-variable', '-Wno-unused-parameter', '-I' + str(p), '-I' + str(ROOT / 'src'),
                        str(p / 'main.cpp'), '-o', str(p / 'smoke')], check=True)
        subprocess.run([str(p / 'smoke')], check=True)


if __name__ == '__main__':
    main()
