#!/usr/bin/python3
"""Small smoke: real namespace, per-port orchestrator, UC handler and bound CLI.
Bottom-level queue exports/IOKit are fake; this does not validate device semantics.
Real RX/RSS/TX port-2 context construction runs in the lifetime tests.
"""
from pathlib import Path
import subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
def method(path, signature):
    s=(ROOT/path).read_text();a=s.index(signature)
    return s[a:s.index('\n}\n',a)+3]
code=r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "cx3_abi.h"
#include "mlx4_regs.h"
#include "Cx3PortQueues.h"
#include "Cx3PortState.h"
using UInt32=uint32_t;using UInt64=uint64_t;using IOReturn=int;
enum {kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnNotReady=-2,
 kIOReturnError=-3,kIOReturnNoMemory=-4,kIOReturnNotAttached=-5,KERN_FAILURE=-6};
struct Mlx4Icm {};
static bool oom=false;static unsigned allocs=0,frees=0;
void*IOMalloc(size_t n){if(oom)return nullptr;++allocs;return malloc(n);}
void IOFree(void*p,size_t){++frees;free(p);}
struct Hw {
 bool wedged=false,closeOk=true;unsigned queries=0,closes=0,lastPort=0;
 bool cmdWedged(){return wedged;}
 bool queryPort(unsigned p,Cx3QueryPortResult*q){++queries;lastPort=p;q->logMaxMacs=7;return true;}
 bool closePort(unsigned p,UInt32*s){++closes;lastPort=p;*s=closeOk?0:3;return closeOk;}
};
class Cx3Mlx4 {
public:
 void *fNativePorts[2]{};
 Cx3PortQueues fPortQueues[2]{};Cx3PortState fPorts[2]{};Cx3QueueNamespace fQueueNamespace{};
 UInt64 fDeviceGeneration=2;bool fHcaInited=true,fIcmMapped=true,fEqUnknownRetained=false;
 bool fResetCommandsBlocked=false,fStopRetained=false,fNetEnabled=false,stopping=false;
 void*fNetIface=nullptr,*fNetTimer=nullptr,*fNetTxRing=nullptr;Mlx4Icm eq;Mlx4Icm*fEqRing=&eq;
 unsigned fMrState=1,calls=0,failStage=0,semanticFail=0,poisonAfter=0,teardowns=0;
 bool retain=false;Hw fHw;Mlx4Icm owners[2];
 struct {unsigned num_ports=2,reserved_qps=64,reserved_cqs=128;}fDevCap;
 struct {unsigned log_num_qps=10,log_num_cqs=10;}fInitHca;
 Cx3Mlx4(){fPortQueues[0].init(1);fPortQueues[1].init(2);fQueueNamespace.init();}
 bool retentionExhausted(){return false;}bool selectorStopping(){return stopping;}
 bool recordPortQuery(unsigned,Cx3QueryPortResult&){return true;}
 bool base(unsigned n,unsigned p,UInt64*s){++calls;if(p<1||p>2)abort();if(poisonAfter==n)stopping=true;s[0]=failStage==n?3:0;return !s[0];}
 IOReturn exportR4Cq(const UInt64*,Cx3R4CqResult*r,UInt64*s,unsigned p){if(base(1,p,s)){auto&q=fPortQueues[p-1];q.cqUp=true;q.cqCqn=q.numbers.rxCqn;q.cqRing=&owners[p-1];}r->fwStatus=0;return 0;}
 IOReturn exportR4Rq(const UInt64*,Cx3R4RqResult*r,UInt64*s,unsigned p){if(base(2,p,s)){auto&q=fPortQueues[p-1];q.rqUp=true;q.rqRing=&owners[p-1];}r->wqe0Matches=r->wqeLastMatches=1;return 0;}
 IOReturn exportR4Qp(const UInt64*,Cx3R4QpResult*r,UInt64*s,unsigned p){if(base(3,p,s)){auto&q=fPortQueues[p-1];q.qpUp=true;q.qpRec=&owners[p-1];q.qpQpn=q.numbers.rxQpn;}r->stepsAccepted=r->statesConfirmed=r->queryState=3;r->queryOk=r->qpnConfirmed=r->queryAllFieldsMatch=r->queryMttMatches=r->producerMatches=1;return 0;}
 IOReturn exportR4Ingress(Cx3R4IngressResult*r,UInt64*s,unsigned p){if(base(4,p,s)){auto&q=fPortQueues[p-1];q.ingressUp=q.macUp=true;q.ingressRec=&owners[p-1];q.ingressQpn=q.numbers.rssQpn;}r->stepsAccepted=r->statesConfirmed=r->queryState=3;r->queryOk=r->queryAllFieldsMatch=r->queryMttMatches=r->producerMatches=1;r->queryRssMatches=semanticFail!=4;return 0;}
 IOReturn exportR4Tx(const UInt64*,Cx3R4TxResult*r,UInt64*s,unsigned p){if(base(5,p,s)){auto&q=fPortQueues[p-1];q.txQpUp=q.txCqUp=true;q.txQpRec=&owners[p-1];q.txQpn=q.numbers.txQpn;q.txCqn=q.numbers.txCqn;}r->numberSafe=r->queryOk=r->queryAllFieldsMatch=r->queryMttMatches=r->qpnConfirmed=r->sqStampMatches=1;r->stepsAccepted=r->statesConfirmed=r->queryState=3;return 0;}
 void ingressTeardown(const char*,bool,unsigned p){++teardowns;if(!retain){auto&q=fPortQueues[p-1];q.ingressRec=nullptr;q.ingressUp=q.macUp=false;}}
 void txTeardown(const char*,bool,unsigned p){++teardowns;if(!retain){auto&q=fPortQueues[p-1];q.txQpRec=nullptr;q.txQpUp=q.txCqUp=false;}}
 void qpTeardown(const char*,bool,unsigned p){++teardowns;if(!retain){auto&q=fPortQueues[p-1];q.qpRec=nullptr;q.qpUp=false;}}
 void cqTeardown(const char*,bool,unsigned p){++teardowns;if(!retain){auto&q=fPortQueues[p-1];q.cqRing=nullptr;q.cqUp=false;}}
 void rqTeardown(const char*,bool,unsigned p){++teardowns;if(!retain){auto&q=fPortQueues[p-1];q.rqRing=nullptr;q.rqUp=false;}}
 bool preparePortQueueNumbers();IOReturn exportPortQueue(UInt32,UInt32,UInt64,Cx3PortQueueResult*);
};
__PREPARE__
__EXPORT__
struct IOExternalMethodArguments{UInt32 scalarInputCount=3,scalarOutputCount=0;UInt64*scalarInput=nullptr;const void*structureInput=nullptr;size_t structureInputSize=0;void*structureOutput=nullptr;size_t structureOutputSize=96;};
struct Cx3Mlx4UserClient{Cx3Mlx4*fProvider;IOReturn doPortQueue(IOExternalMethodArguments*);};
__HANDLER__
struct Cx3RuntimeSample{Cx3RuntimeIdentityResult result;uint64_t connectedServiceRegistryId;char bootSessionUuid[37];uint32_t transport,scalarOutputCount;size_t returnedBytes;};
__META_UUID__
__META_TAG__
__META_SHAPE__
__META_HEX__
__META_EMIT__
__CLI_BIND__
static Cx3Mlx4*driver=nullptr;static unsigned samples=0,transports=0;static int drift=0,gConn=1;
static int readRuntimeSample(Cx3RuntimeSample*s){memset(s,0,sizeof(*s));++samples;s->connectedServiceRegistryId=100;s->returnedBytes=sizeof(s->result);strcpy(s->bootSessionUuid,"00000001-0000-0000-0000-000000000001");auto&r=s->result;r.version=CX3_RUNTIME_RESULT_VERSION;r.structBytes=sizeof(r);r.validity=63;r.serviceRegistryId=100;r.providerRegistryId=drift==1&&samples>1?201:200;r.sampleUptimeNs=samples;r.deviceGeneration=driver->fDeviceGeneration+(drift==2&&samples>1);r.lifecycleFlags=drift==3?1:16;r.pciIdentity.vendor=0x15b3;r.pciIdentity.device=0x1007;r.pciIdentity.subsystemVendor=0x15b3;r.pciIdentity.subsystemId=1;strcpy(r.bundleVersion,"0.44.48");strcpy(r.buildTag,"queue-smoke");return 0;}
static int IOConnectCallMethod(int,unsigned sel,const uint64_t*in,unsigned n,const void*,size_t,uint64_t*,uint32_t*outCount,void*out,size_t*bytes){++transports;if(sel!=53||n!=3||*bytes!=96)return -1;*outCount=0;return driver->exportPortQueue(in[0],in[1],in[2],(Cx3PortQueueResult*)out);}
__CLI__
static unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL %d %s\n",__LINE__,#x);}}while(0)
int main(){
 Cx3PortQueueResult r{};
 {Cx3QueueNamespace n{};n.init();CHECK(n.configure(10,10,64,128,2,7,2));auto&a=n.numbers[0];auto&b=n.numbers[1];CHECK(a.rxQpn==768&&a.rssQpn==64&&a.txQpn==65&&a.rxCqn==128&&a.txCqn==129&&a.macSlots==128);CHECK(b.rxQpn==896&&b.rssQpn==66&&b.txQpn==67&&b.rxCqn==130&&b.txCqn==131&&a.macBase+a.macSlots==b.macBase);CHECK(!n.configure(10,10,64,128,2,7,2));}
 {Cx3QueueNamespace n{};n.init();CHECK(!n.configure(8,10,64,128,2,7,2));CHECK(!n.configure(25,10,64,128,2,7,2));CHECK(!n.configure(10,7,64,128,2,7,2));CHECK(!n.configure(10,10,0xffffffffu,128,2,7,2));CHECK(!n.configure(10,10,64,128,2,8,2));CHECK(!n.configure(10,10,64,128,2,7,0));}
 {Cx3Mlx4 d;CHECK(!d.exportPortQueue(2,1,2,&r)&&!r.status&&r.completedMask==31&&(r.flags&2)&&r.rxQpn==896&&r.rssQpn==66&&r.txQpn==67&&d.calls==5);CHECK(!d.fPortQueues[0].hasOwners());CHECK(!d.exportPortQueue(2,1,2,&r)&&!r.status&&d.calls==5);CHECK(!d.exportPortQueue(1,1,2,&r)&&!r.status&&r.rxQpn==768&&d.calls==10);auto primary=d.fPortQueues[0].qpRec;CHECK(!d.exportPortQueue(2,2,2,&r)&&!r.status&&r.flags==1&&!r.ownerMask&&!r.completedMask);CHECK(d.fPortQueues[0].qpRec==primary&&d.fMrState==1&&d.fEqRing==&d.eq);CHECK(!d.exportPortQueue(2,1,2,&r)&&!r.status&&d.calls==15);}
 {Cx3Mlx4 d;d.semanticFail=4;CHECK(!d.exportPortQueue(2,1,2,&r)&&r.status&&r.failedStage==4&&r.completedMask==7&&d.calls==4&&(r.flags&4));CHECK(!d.exportPortQueue(2,1,2,&r)&&r.status&&d.calls==4);CHECK(!d.exportPortQueue(2,2,2,&r)&&!r.status&&r.flags==1);}
 {Cx3Mlx4 d;d.poisonAfter=2;CHECK(!d.exportPortQueue(2,1,2,&r)&&r.status&&r.failedStage==3&&r.completedMask==3&&d.calls==2&&r.fwStatus==CX3_FWSTATUS_NOT_POSTED);}
 {Cx3Mlx4 d;CHECK(!d.exportPortQueue(2,1,1,&r)&&r.status&&d.calls==0&&d.fHw.queries==0);d.fNetIface=&d;CHECK(!d.exportPortQueue(2,1,2,&r)&&r.status&&d.calls==0&&d.fHw.queries==0);CHECK(!d.exportPortQueue(1,2,2,&r)&&r.status&&d.teardowns==0);}
 {Cx3Mlx4 d;CHECK(!d.exportPortQueue(2,1,2,&r)&&!r.status);d.fPorts[1].initAttempted=true;d.fHw.closeOk=false;CHECK(!d.exportPortQueue(2,2,2,&r)&&r.status&&!(r.flags&2)&&d.teardowns==0);d.fHw.closeOk=true;d.retain=true;CHECK(!d.exportPortQueue(2,2,2,&r)&&r.status&&(r.flags&8)&&d.fHw.lastPort==2);d.retain=false;CHECK(!d.exportPortQueue(2,2,2,&r)&&!r.status&&d.fPorts[1].closeConfirmed);}
 {Cx3Mlx4 d;CHECK(!d.exportPortQueue(2,1,2,&r));d.fDeviceGeneration=3;CHECK(!d.exportPortQueue(2,0,3,&r)&&r.status&&!(r.flags&2));CHECK(!d.exportPortQueue(2,1,3,&r)&&r.status&&d.calls==5);}
 {Cx3Mlx4 d;oom=true;CHECK(!d.exportPortQueue(2,1,2,&r)&&r.status&&!d.fPortQueues[1].setupAttempted&&!d.calls);oom=false;Cx3Mlx4UserClient uc{&d};UInt64 in[]={2,1,2};IOExternalMethodArguments a;a.scalarInput=in;a.structureOutput=&r;a.scalarInput[0]=(1ull<<32)+2;CHECK(uc.doPortQueue(&a)==kIOReturnBadArgument&&!d.calls);in[0]=2;CHECK(!uc.doPortQueue(&a)&&!r.status&&d.calls==5);}
 {Cx3Mlx4 d;driver=&d;samples=transports=0;CHECK(!cmdPortQueue(2,1,"queue-smoke",0)&&transports==1&&d.calls==5);CHECK(!cmdPortQueue(2,0,"queue-smoke",1));CHECK(!cmdPortQueue(2,2,"queue-smoke",0));}
 for(int change: {1,2,3}){Cx3Mlx4 d;driver=&d;drift=change;samples=transports=0;CHECK(cmdPortQueue(2,0,"queue-smoke",0)==1);CHECK(transports==(change==3?0u:1u));}drift=0;
 {Cx3Mlx4 d;driver=&d;samples=transports=0;CHECK(cmdPortQueue(2,1,"wrong",0)==1&&!transports&&!d.calls);}
 CHECK(allocs==frees);printf("port queue: %u checks, %u failures (synthetic transport; no hardware)\n",checks,failures);return failures?1:0;
}
'''.replace('#include <cstdlib>','#include <cstdlib>\n#include <initializer_list>')
for key,path,sig in [('PREPARE','src/Cx3Mlx4.cpp','bool Cx3Mlx4::preparePortQueueNumbers('),('EXPORT','src/Cx3Mlx4.cpp','IOReturn Cx3Mlx4::exportPortQueue('),('HANDLER','src/Cx3Mlx4UserClient.cpp','IOReturn Cx3Mlx4UserClient::doPortQueue(')]:
    code=code.replace('__'+key+'__',method(path,sig))
for key,sig in [('META_UUID','static int runtimeUuidValid('),('META_TAG','static int runtimeMetadataValid('),('META_SHAPE','static int runtimeIdentityShapeValid('),('META_HEX','static void runtimeEmitHex('),('META_EMIT','static void emitRuntimeSample('),('CLI_BIND','static int runtimeSameInstance('),('CLI','static int cmdPortQueue(')]:
    code=code.replace('__'+key+'__',method('tools/cx3ctl.c',sig))
with tempfile.TemporaryDirectory(prefix='cx3-port-queue-') as t:
    p=Path(t);(p/'main.cpp').write_text(code)
    subprocess.run(['/usr/bin/clang++','-std=c++14','-Wall','-Wextra','-I'+str(ROOT/'src'),str(p/'main.cpp'),'-o',str(p/'smoke')],check=True)
    subprocess.run([str(p/'smoke')],check=True)
