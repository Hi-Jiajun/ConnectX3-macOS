#!/usr/bin/python3
"""Small host smoke of the real automatic-network orchestrator and UC adapter.
Typed exports below are a fake transport, NOT device/IOKit validation.
"""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
def method(path,signature):
 s=(ROOT/path).read_text();a=s.index(signature);return s[a:s.index('\n}\n',a)+3]
code=r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "cx3_abi.h"
#include "Cx3PortState.h"
using UInt32=uint32_t;using UInt64=uint64_t;using IOReturn=int;
enum {kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnNotPermitted=-2,
      kIOReturnNotReady=-3,kIOReturnNoMemory=-4,kIOReturnError=-5,kIOReturnNotAttached=-6};
#define CX3_BUILD_TAG "auto-smoke"
#define LOG(...) ((void)0)
#define OSDynamicCast(t,o) static_cast<t*>(o)
struct OSObject {};
struct Registry {UInt64 getRegistryEntryID(){return 200;}};
struct Iface {UInt32 getUnitNumber(){return 2;}};
struct Loop {using Action=IOReturn(*)(OSObject*,void*,void*,void*,void*);
 IOReturn runAction(Action action,OSObject*owner,void*arg){return action(owner,arg,0,0,0);}};
using IOWorkLoop=Loop;
void IOLockLock(void*){} void IOLockUnlock(void*){}
static bool oom=false;static unsigned allocs=0,frees=0;
void *IOMalloc(size_t n){if(oom)return 0;++allocs;return malloc(n);}
void IOFree(void*p,size_t){++frees;free(p);}
class Cx3Mlx4:public OSObject {
public:
 Cx3AutoNetworkResult fAutoNetwork{};bool fAutoNetworkUserDisabled=false;
 Registry pci;Registry *fPci=&pci;UInt64 fDeviceGeneration=1;
 Cx3PortState fPorts[2]{};bool fNetEnabled=false,fResetCommandsBlocked=false,fStopRetained=false;
 bool fQuarExhausted=false,fFwRunAttempted=false,fResetUnproven=false,fHcaInited=false,fIcmMapped=false;
 Iface iface;Iface *fNetIface=nullptr;void *fNetTimer=nullptr,*fNetTxRing=nullptr,*fNetTxLock=nullptr;
 struct {bool latched=false;} fNetTxFault;
 struct {bool wedged=false;bool cmdWedged(){return wedged;}} fHw;
 Loop loop;unsigned calls=0,last=0,fail=0,semanticFail=0,gatedCalls=0;bool empty=true;
 Cx3Mlx4(){fAutoNetwork.version=1;fAutoNetwork.structBytes=sizeof(fAutoNetwork);fAutoNetwork.port=1;fAutoNetwork.interfaceUnit=0xffffffffu;}
 UInt64 getRegistryEntryID(){return 100;} bool selectorStopping(){return false;}
 bool hasStopRetentionOwners(){return false;}bool resetFreshStartEligible(){return empty;}
 Loop *getWorkLoop(){return &loop;}void setProperty(const char*,UInt32,int){}
 IOReturn enable(Iface*){++gatedCalls;fNetEnabled=true;return 0;}
 IOReturn disable(Iface*){++gatedCalls;fNetEnabled=false;return 0;}
 void base(unsigned sel,UInt64*s){++calls;last=sel;s[0]=fail==sel?(UInt64)kIOReturnError:0;}
 IOReturn exportResetHca(bool b,UInt64*s){if(!b)abort();base(3,s);fDeviceGeneration=2;fResetUnproven=true;return 0;}
 IOReturn exportQueryFw(bool b,UInt64*s){if(!b)abort();base(2,s);s[kFwOutFwStatus]=0;s[kFwOutFwPages]=1542;return 0;}
 IOReturn exportFwLoad(UInt32 pages,bool low,bool force,bool fill,UInt64*s){if(pages!=1542||low||force||fill)abort();base(10,s);s[kFwLoadOutMapFaStatus]=s[kFwLoadOutRunFwStatus]=0;fFwRunAttempted=true;return 0;}
 IOReturn exportQueryDevCap(bool a,bool b,Cx3DevCapResult*r,UInt64*s){if(!a||b)abort();base(8,s);r->status=r->fwStatus=0;return 0;}
 IOReturn exportM2dProfile(const UInt64*i,Cx3ProfileResult*r,UInt64*s){if(i[0]!=1024)abort();base(13,s);r->numTables=14;return 0;}
 IOReturn exportM2dBringUp(UInt64*s){base(14,s);fHcaInited=fIcmMapped=true;s[kBringUpOutInitHcaStat]=0;s[kBringUpOutTablesDone]=s[kBringUpOutTablesTotal]=14;return 0;}
 IOReturn exportM3QueryPort(const UInt64*i,Cx3QueryPortResult*r,UInt64*s){if(i[0]!=1)abort();base(24,s);r->supportedTypes=2;return 0;}
 IOReturn exportM3PortUp(const UInt64*i,UInt64*s){if(i[0]!=1||i[1])abort();base(25,s);s[kPortUpOutStep]=kPortUpSteps;s[kPortUpOutSetFwStatus]=s[kPortUpOutQpnCalcFwStatus]=s[kPortUpOutInitFwStatus]=0;return 0;}
 IOReturn exportM3EqUp(const UInt64*i,UInt64*s){if(i[2]!=1||i[3]!=1)abort();base(15,s);s[kEqUpOutFwStatus]=s[kEqUpOutMapEqStatus]=0;return 0;}
 IOReturn exportR4Mr(const UInt64*,Cx3R4MrResult*r,UInt64*s){base(28,s);r->queryMptOk=1;return 0;}
 IOReturn exportR4Cq(const UInt64*i,Cx3R4CqResult*,UInt64*s){if(i[0]!=512||i[1]!=128)abort();base(29,s);return 0;}
 IOReturn exportR4Rq(const UInt64*i,Cx3R4RqResult*r,UInt64*s){if(i[0]!=256||i[1]!=2048)abort();base(30,s);r->wqe0Matches=r->wqeLastMatches=1;return 0;}
 IOReturn exportR4Qp(const UInt64*,Cx3R4QpResult*r,UInt64*s){base(34,s);r->stepsAccepted=r->statesConfirmed=r->queryState=3;r->queryOk=r->qpnConfirmed=r->queryAllFieldsMatch=r->queryMttMatches=r->producerMatches=1;return 0;}
 IOReturn exportR4Ingress(Cx3R4IngressResult*r,UInt64*s){base(35,s);r->stepsAccepted=r->statesConfirmed=r->queryState=3;r->macReadbackOk=r->macReadbackMatches=r->queryOk=r->queryAllFieldsMatch=r->queryMttMatches=r->producerMatches=1;r->queryRssMatches=semanticFail!=35;return 0;}
 IOReturn exportR4Tx(const UInt64*,Cx3R4TxResult*r,UInt64*s){base(36,s);r->numberSafe=r->queryOk=r->queryAllFieldsMatch=r->queryMttMatches=r->qpnConfirmed=r->sqStampMatches=1;r->stepsAccepted=r->statesConfirmed=r->queryState=3;return 0;}
 IOReturn exportNetUp(const UInt64*,UInt64*s){base(39,s);s[kNetUpOutRefused]=0;s[kNetUpOutMacOk]=1;fNetIface=&iface;fNetTimer=fNetTxRing=this;return 0;}
 IOReturn exportAutoNetwork(const Cx3AutoNetworkRequest*,Cx3AutoNetworkResult*);
 static IOReturn autoNetworkOnWorkLoop(OSObject*,void*,void*,void*,void*);
};
__GATE__
__EXPORT__
struct IOExternalMethodArguments {UInt32 scalarInputCount=0,scalarOutputCount=0;const void*structureInput=0;size_t structureInputSize=0;void*structureOutput=0;size_t structureOutputSize=0;};
struct Cx3Mlx4UserClient {Cx3Mlx4*fProvider;IOReturn doAutoNetwork(IOExternalMethodArguments*);};
__HANDLER__

using kern_return_t=int;
enum {KERN_FAILURE=-100};
struct Cx3RuntimeSample {Cx3RuntimeIdentityResult result;uint64_t connectedServiceRegistryId;char bootSessionUuid[37];uint32_t transport,scalarOutputCount;size_t returnedBytes;};
__META_UUID__
__META_TAG__
__META_SHAPE__
__META_HEX__
__META_EMIT__
static Cx3Mlx4 *driver=nullptr;static unsigned samples=0,transports=0;static bool drift=false;
static int gConn=1;
static int readRuntimeSample(Cx3RuntimeSample*s){
 memset(s,0,sizeof(*s));++samples;s->connectedServiceRegistryId=100;s->returnedBytes=sizeof(s->result);
 strcpy(s->bootSessionUuid,"00000001-0000-0000-0000-000000000001");
 auto&r=s->result;r.version=CX3_RUNTIME_RESULT_VERSION;r.structBytes=sizeof(r);r.validity=63;
 r.serviceRegistryId=100;r.providerRegistryId=drift&&samples>1?201:200;r.sampleUptimeNs=samples;
 r.deviceGeneration=driver->fDeviceGeneration;r.lifecycleFlags=driver->fFwRunAttempted?16:0;
 r.pciIdentity.vendor=0x15b3;r.pciIdentity.device=0x1007;r.pciIdentity.subsystemVendor=0x15b3;r.pciIdentity.subsystemId=1;
 strcpy(r.bundleVersion,"0.44.48");strcpy(r.buildTag,CX3_BUILD_TAG);return runtimeIdentityShapeValid(s)?0:1;
}
static int IOConnectCallMethod(int,unsigned sel,const uint64_t*,unsigned,const void*in,size_t inBytes,uint64_t*,uint32_t*outCount,void*out,size_t*outBytes){
 ++transports;if(sel!=51||inBytes!=112||*outBytes!=208)return -1;*outCount=0;
 return driver->exportAutoNetwork((const Cx3AutoNetworkRequest*)in,(Cx3AutoNetworkResult*)out);
}
__CLI_BIND__
__CLI__

static unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL %d %s\n",__LINE__,#x);}}while(0)
Cx3AutoNetworkRequest request(Cx3Mlx4&d,unsigned action=1){Cx3AutoNetworkRequest q{};q.version=1;q.structBytes=sizeof(q);q.action=action;q.port=1;q.expectedServiceId=100;q.expectedProviderId=200;q.expectedGeneration=d.fDeviceGeneration;strcpy(q.expectedBuildTag,CX3_BUILD_TAG);return q;}
int main(){Cx3AutoNetworkResult r;
 {Cx3Mlx4 d;auto q=request(d);CHECK(!d.exportAutoNetwork(&q,&r));CHECK(!r.status&&r.state==kCx3AutoReady&&r.completedMask==0xffff&&d.calls==16&&r.deviceGeneration==2);q=request(d);CHECK(!d.exportAutoNetwork(&q,&r)&&!r.status&&d.calls==16);q=request(d,2);CHECK(!d.exportAutoNetwork(&q,&r)&&r.state==kCx3AutoDisabled&&d.fAutoNetworkUserDisabled&&!d.fNetEnabled);q=request(d);CHECK(!d.exportAutoNetwork(&q,&r)&&!r.status&&d.calls==16&&d.fNetEnabled&&!d.fAutoNetworkUserDisabled);}
 {Cx3Mlx4 d;d.semanticFail=35;auto q=request(d);CHECK(!d.exportAutoNetwork(&q,&r));CHECK(r.status&&r.state==kCx3AutoFailed&&r.failedStage==14&&d.calls==14&&d.last==35&&r.lastScalars[0]==0);q=request(d);CHECK(!d.exportAutoNetwork(&q,&r)&&r.status&&d.calls==14);q=request(d,0);CHECK(!d.exportAutoNetwork(&q,&r)&&r.failedStage==14&&d.calls==14);}
 {Cx3Mlx4 d;d.fail=10;auto q=request(d);CHECK(!d.exportAutoNetwork(&q,&r)&&r.status&&r.failedStage==3&&d.calls==3);}
 {Cx3Mlx4 d;auto q=request(d);q.expectedGeneration=99;CHECK(d.exportAutoNetwork(&q,&r)==kIOReturnNotPermitted&&d.calls==0);q=request(d);q.port=2;CHECK(d.exportAutoNetwork(&q,&r)==kIOReturnBadArgument&&d.calls==0);q=request(d);strcpy(q.expectedBuildTag,"other");CHECK(d.exportAutoNetwork(&q,&r)==kIOReturnNotPermitted&&d.calls==0);}
 {Cx3Mlx4 d;d.empty=false;auto q=request(d);CHECK(!d.exportAutoNetwork(&q,&r)&&r.status&&d.calls==0);}
 {Cx3Mlx4 d;auto q=request(d,2);CHECK(!d.exportAutoNetwork(&q,&r)&&r.state==kCx3AutoDisabled&&d.calls==0);q=request(d);CHECK(!d.exportAutoNetwork(&q,&r)&&!r.status&&r.state==kCx3AutoReady&&d.calls==16&&!d.fAutoNetworkUserDisabled);}
 {Cx3Mlx4 d;d.fResetCommandsBlocked=true;auto q=request(d,0);CHECK(!d.exportAutoNetwork(&q,&r)&&r.status&&(r.flags&16)&&d.calls==0);q=request(d);CHECK(!d.exportAutoNetwork(&q,&r)&&r.status&&d.calls==0);}
 {Cx3Mlx4 d;d.fNetTxLock=&d;d.fNetTxFault.latched=true;auto q=request(d);CHECK(!d.exportAutoNetwork(&q,&r)&&r.status&&(r.flags&8)&&d.calls==0);}
 {Cx3Mlx4 d;oom=true;auto q=request(d);CHECK(!d.exportAutoNetwork(&q,&r)&&r.status&&d.calls==0&&d.fAutoNetwork.state==0);oom=false;Cx3Mlx4UserClient uc{&d};IOExternalMethodArguments a;a.structureInput=&q;a.structureInputSize=sizeof(q)-1;a.structureOutput=&r;a.structureOutputSize=sizeof(r);CHECK(uc.doAutoNetwork(&a)==kIOReturnBadArgument&&d.calls==0);a.structureInputSize=sizeof(q);CHECK(!uc.doAutoNetwork(&a)&&!r.status&&d.calls==16);}

 {Cx3Mlx4 d;driver=&d;samples=transports=0;CHECK(!cmdAutoNetwork(kCx3AutoEnable,CX3_BUILD_TAG,0)&&transports==1&&d.calls==16);CHECK(!cmdAutoNetwork(kCx3AutoDisable,CX3_BUILD_TAG,0)&&d.calls==16);}
 {Cx3Mlx4 d;driver=&d;samples=transports=0;CHECK(cmdAutoNetwork(kCx3AutoEnable,"wrong-build",0)==1&&transports==0&&d.calls==0);}
 {Cx3Mlx4 d;driver=&d;d.semanticFail=35;samples=transports=0;CHECK(cmdAutoNetwork(kCx3AutoEnable,CX3_BUILD_TAG,0)==1&&d.calls==14);}
 {Cx3Mlx4 d;driver=&d;samples=transports=0;drift=true;CHECK(cmdAutoNetwork(kCx3AutoStatus,CX3_BUILD_TAG,0)==1);drift=false;}
 CHECK(allocs==frees);
 printf("%u checks, %u failures (host fake transport; not hardware)\n",checks,failures);return failures?1:0;
}
'''
for key,path,sig in [('GATE','src/Cx3Mlx4.cpp','IOReturn Cx3Mlx4::autoNetworkOnWorkLoop('),('EXPORT','src/Cx3Mlx4.cpp','IOReturn Cx3Mlx4::exportAutoNetwork('),('HANDLER','src/Cx3Mlx4UserClient.cpp','IOReturn Cx3Mlx4UserClient::doAutoNetwork(')]:
 code=code.replace('__'+key+'__',method(path,sig))
for key,sig in [('META_UUID','static int runtimeUuidValid('),('META_TAG','static int runtimeMetadataValid('),('META_SHAPE','static int runtimeIdentityShapeValid('),('META_HEX','static void runtimeEmitHex('),('META_EMIT','static void emitRuntimeSample('),('CLI_BIND','static int autoNetworkSameProvider('),('CLI','static int cmdAutoNetwork(')]:
 code=code.replace('__'+key+'__',method('tools/cx3ctl.c',sig))
with tempfile.TemporaryDirectory(prefix='cx3-auto-smoke-') as t:
 p=Path(t);(p/'main.cpp').write_text(code)
 subprocess.run(['c++','-std=c++14','-Wall','-Wextra','-I'+str(ROOT/'src'),str(p/'main.cpp'),'-o',str(p/'smoke')],check=True)
 subprocess.run([str(p/'smoke')],check=True)
