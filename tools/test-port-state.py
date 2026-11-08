#!/usr/bin/python3
"""Directed host smoke of real per-port observation, bring-up and query/CLI.
Firmware/IOKit transport is synthetic; this is not two-port hardware acceptance.
"""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
def method(file,sig):
 s=(ROOT/file).read_text();a=s.index(sig);return s[a:s.index('\n}\n',a)+3]
code=r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#define CX3_HOST_TEST
#include "mlx4_pure.h"
#include "Cx3PortQueues.h"
#include "Cx3PortState.h"
using IOReturn=int;using kern_return_t=int;
enum{kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnNotReady=-2,kIOReturnError=-3,kIOReturnUnsupported=-4,kIOReturnNotAttached=-5};
#define LOG(...) ((void)0)
static void IOSleep(unsigned){}
static void clock_get_uptime(UInt64*p){static UInt64 n=100;*p=++n;}
static void absolutetime_to_nanoseconds(UInt64 t,UInt64*p){*p=t*100;}
struct Hw {
 unsigned queries=0,sets=0,inits=0;bool failSet=false,failQuery=false,failAfterInit=false,badMac=false;
 bool cmdWedged(){return false;}
 bool queryPort(UInt32 port,Cx3QueryPortResult*q){
  ++queries;memset(q,0,sizeof(*q));q->port=port;q->fwStatus=0;
  if(failQuery||(failAfterInit&&inits)){q->status=(UInt32)kIOReturnError;q->fwStatus=9;return false;}
  q->rawLen=64;q->raw[0]=port==1?0x82:2;q->raw[2]=0x26;q->raw[3]=0xac;
  q->raw[5]=port==1?CX3_SPEED_10G_XFI:CX3_SPEED_40G;
  q->raw[10]=7;q->raw[18]=0xec;q->raw[19]=0x0d;q->raw[20]=0x9a;q->raw[21]=0x2a;q->raw[22]=0x15;q->raw[23]=(UInt8)(0x10+port);
  if(badMac)q->raw[18]=1;
  cx3QueryPortDecode(q->raw,q);return true;
 }
 bool setPortGeneralEth(UInt32,UInt16,bool,UInt32*s){++sets;*s=failSet?0xffffffffu:0;return !failSet;}
 bool setPortQpnCalc(UInt32,UInt32,UInt32,UInt32,UInt32,UInt32*s){*s=0;return true;}
 bool initPort(UInt32,UInt32*s){++inits;*s=0;return true;}
};
class Cx3Mlx4 {
public:
 void *fNativePorts[2]{};
    Cx3PortQueues fPortQueues[2] = { []() { Cx3PortQueues q{}; q.init(1); q.qpUp = false; q.qpQpn = 64; return q; }(), []() { Cx3PortQueues q{}; q.init(2); return q; }() };

public:
 Cx3PortState fPorts[2];Hw fHw;UInt64 fDeviceGeneration=2;
 bool fHcaInited=true,fIcmMapped=true,fResetCommandsBlocked=false,fStopRetained=false;
 bool fEqRing=false,fIrqObjectsUp=false;
 struct{UInt32 bmme_flags=0,reserved_qps=64;UInt64 flags=0;}fDevCap;
 struct{UInt32 log_num_qps=10;}fInitHca;
 Cx3Mlx4(){fPorts[0].init(1,2);fPorts[1].init(2,2);}
 bool retentionExhausted(){return false;}
 bool recordPortQuery(UInt32,const Cx3QueryPortResult&);
 IOReturn exportPortInfo(UInt32,bool,Cx3PortInfoResult*);
 IOReturn exportM3PortUp(const UInt64*,UInt64*);
};
__RECORD__
__EXPORT__
__BRINGUP__
struct IOExternalMethodArguments {UInt32 scalarInputCount=0,scalarOutputCount=0;const UInt64*scalarInput=0;const void*structureInput=0;size_t structureInputSize=0;void*structureOutput=0;size_t structureOutputSize=0;};
struct Cx3Mlx4UserClient {Cx3Mlx4*fProvider;IOReturn doPortInfo(IOExternalMethodArguments*);};
__HANDLER__
static Cx3Mlx4 *driver;static int gConn=1,opens=0;
static int openService(){++opens;return 0;}
static int IOConnectCallMethod(int,unsigned sel,const UInt64*in,unsigned count,const void*,size_t,UInt64*,UInt32*outCount,void*out,size_t*bytes){
 if(sel!=52||count!=2||*bytes!=152)return -1;*outCount=0;
 return driver->exportPortInfo((UInt32)in[0],in[1],(Cx3PortInfoResult*)out);
}
__CLI__
static unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL %d %s\n",__LINE__,#x);}}while(0)
int main(){Cx3PortInfoResult r;UInt64 out[16];
 {Cx3Mlx4 d;driver=&d;CHECK(!d.exportPortInfo(1,false,&r)&&r.status&&r.rawBytes==0&&d.fHw.queries==0);CHECK(!d.exportPortInfo(1,true,&r)&&!r.status&&r.linkUp&&r.speedMbps==10000&&r.mac[5]==0x11&&r.rawBytes==64);auto before=d.fPorts[0];CHECK(!d.exportPortInfo(2,true,&r)&&!r.status&&!r.linkUp&&r.speedMbps==40000&&r.mac[5]==0x12);CHECK(!memcmp(&before,&d.fPorts[0],sizeof(before)));CHECK(!cmdPortInfo(2,0,0)&&d.fHw.queries==2);}
 {Cx3Mlx4 d;UInt64 a[]={1,1522},b[]={2,9022};CHECK(!d.exportM3PortUp(a,out)&&out[0]==0&&d.fPorts[0].mtuConfigured);auto before=d.fPorts[0];CHECK(!d.exportM3PortUp(b,out)&&out[0]==0&&d.fPorts[1].frameMtu==9022&&d.fPorts[1].initAccepted);CHECK(!memcmp(&before,&d.fPorts[0],sizeof(before)));}
 {Cx3Mlx4 d;UInt64 in[]={2,12000};CHECK(!d.exportM3PortUp(in,out)&&out[0]!=0&&d.fHw.sets==0&&d.fHw.inits==0&&d.fPorts[1].frameMtu==0);in[1]=0x100000000ull;unsigned n=d.fHw.queries;CHECK(!d.exportM3PortUp(in,out)&&out[0]!=0&&d.fHw.queries==n);}
 {Cx3Mlx4 d;d.fHw.badMac=true;UInt64 in[]={2,1522};CHECK(!d.exportM3PortUp(in,out)&&out[0]==0&&!d.fPorts[1].macValid);}
 {Cx3Mlx4 d;d.fPorts[1].frameMtu=1522;d.fPorts[1].mtuConfigured=true;d.fHw.failSet=true;UInt64 in[]={2,9022};CHECK(!d.exportM3PortUp(in,out)&&out[0]!=0&&!d.fPorts[1].mtuConfigured&&d.fPorts[1].frameMtu==0&&d.fHw.inits==0);}
 {Cx3Mlx4 d;d.fHw.failAfterInit=true;UInt64 in[]={2,1522};CHECK(!d.exportM3PortUp(in,out)&&out[0]!=0&&d.fPorts[1].initAccepted&&!d.fPorts[1].linkKnown&&d.fPorts[1].lastQueryFailed);}
 {Cx3Mlx4 d;driver=&d;CHECK(!d.exportPortInfo(1,true,&r)&&!r.status);auto previous=r;d.fHw.failQuery=true;CHECK(!d.exportPortInfo(1,true,&r)&&r.status&&(r.flags&128)&&r.queryUptimeNs==previous.queryUptimeNs&&!memcmp(r.raw,previous.raw,64));CHECK(cmdPortInfo(1,0,0)==1);d.fPorts[0].newGeneration(3);CHECK(!d.fPorts[0].queryValid&&!d.fPorts[0].macValid&&!d.fPorts[0].mtuConfigured&&!d.fPorts[0].closeConfirmed);}
 {Cx3Mlx4 d;d.fResetCommandsBlocked=true;CHECK(!d.exportPortInfo(1,true,&r)&&r.status&&d.fHw.queries==0);Cx3Mlx4UserClient uc{&d};UInt64 in[]={0x100000001ull,1};IOExternalMethodArguments a;a.scalarInputCount=2;a.scalarInput=in;a.structureOutput=&r;a.structureOutputSize=152;CHECK(uc.doPortInfo(&a)==kIOReturnBadArgument&&d.fHw.queries==0);}
 printf("%u checks, %u failures (host fake transport; not dual-port hardware)\n",checks,failures);return failures?1:0;
}
'''
for key,file,sig in [('RECORD','src/Cx3Mlx4.cpp','bool Cx3Mlx4::recordPortQuery('),('EXPORT','src/Cx3Mlx4.cpp','IOReturn Cx3Mlx4::exportPortInfo('),('BRINGUP','src/Cx3Mlx4.cpp','IOReturn Cx3Mlx4::exportM3PortUp('),('HANDLER','src/Cx3Mlx4UserClient.cpp','IOReturn Cx3Mlx4UserClient::doPortInfo('),('CLI','tools/cx3ctl.c','static int cmdPortInfo(')]:code=code.replace('__'+key+'__',method(file,sig))
with tempfile.TemporaryDirectory(prefix='cx3-port-state-') as t:
 p=Path(t);(p/'main.cpp').write_text(code)
 subprocess.run(['c++','-std=c++14','-Wall','-Wextra','-Wno-unused-variable','-I'+str(ROOT/'src'),str(p/'main.cpp'),'-o',str(p/'smoke')],check=True)
 subprocess.run([str(p/'smoke')],check=True)
