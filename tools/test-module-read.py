#!/usr/bin/python3
"""Brief host-only smoke of production module GET, export, handler and CLI.
No IOKit transport/device access; full failure/combination suite is deferred.
"""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
def method(file,signature):
 s=(ROOT/file).read_text();i=s.index(signature);return s[i:s.index('\n}\n',i)+3]
methods={
 'HW':method('src/Mlx4Hw.cpp','bool Mlx4Hw::readModule('),
 'FREE':method('src/Mlx4Hw.cpp','void Mlx4Hw::mboxFree('),
 'RETAIN':method('src/Mlx4Hw.cpp','bool Mlx4Hw::mailboxNeedsRetention() const'),
 'EXPORT':method('src/Cx3Mlx4.cpp','IOReturn Cx3Mlx4::exportModuleRead('),
 'HANDLER':method('src/Cx3Mlx4UserClient.cpp','IOReturn Cx3Mlx4UserClient::doModuleRead('),
 'CLI':method('tools/cx3ctl.c','static int cmdModuleRead(')}
code=r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#define CX3_HOST_TEST
#include "mlx4_pure.h"
#include "cx3_abi.h"
using IOReturn=int; using kern_return_t=int;
enum {kIOReturnSuccess=0,KERN_SUCCESS=0,kIOReturnBadArgument=-1,kIOReturnNoMemory=-2,
 kIOReturnNotPermitted=-3,kIOReturnUnsupported=-4,kIOReturnError=-5,kIOReturnOffline=-6,
 kIOReturnNotAttached=-7,kIOReturnNotReady=-8};
#define LOG(...) ((void)0)
struct Obj {void clearMemoryDescriptor(bool){} void release(){}};
struct Mlx4IcmPage {UInt64 busAddr=0x2000;};
struct Mlx4Icm {UInt8 bytesStorage[4096];void *virt=bytesStorage;Mlx4IcmPage page;Mlx4IcmPage *pages=&page;UInt32 npages=1;UInt64 bytes=4096;};
class Mlx4Hw {
public:
 Mlx4Icm inbox; Mlx4Icm *fModuleInbox=nullptr; Obj *fMboxMd=nullptr,*fMboxCmd=nullptr;
 UInt8 mailbox[4096],reply[4096];void *fMboxVirt=mailbox;
 UInt64 fMboxPhys=0x1000,fMboxOffset=4096,fDeviceGeneration=2;UInt32 fMboxSegCount=1;
 bool fCmdWedged=false,fMboxRetained=false,owner=false,alive=true;
 unsigned calls=0,frees=0;int mode=0;UInt32 expectedPort=1,expectedOffset=4,expectedLength=3;
 Mlx4Hw(){memset(mailbox,0xee,sizeof(mailbox));memset(reply,0,sizeof(reply));reply[80]=0x12;reply[81]=0x34;reply[82]=0x56;}
 bool mboxAlloc(){return !fCmdWedged&&!fMboxRetained;}
 Mlx4Icm *icmAlloc(UInt32 n,bool low){return n==1&&low?&inbox:nullptr;}
 void icmFree(Mlx4Icm*){++frees;}
 bool icmCheck(Mlx4Icm*,UInt32*){return true;}
 UInt32 checkOwnershipAsReference(){return owner?1:0;} bool windowAlive(){return alive;}
 bool cmdBox(UInt64 in,UInt64 out,UInt32 port,UInt8 opmod,UInt16 op,UInt32 wait,UInt32 *status){
  ++calls;UInt8 expected[4096]={};expected[0]=expected[1]=expected[2]=expected[3]=1;
  expected[16]=0xff;expected[17]=0x60;expected[64]=expectedOffset<256?0x50:0x51;
  expected[67]=expectedOffset&255;expected[71]=expectedLength;
  if(in!=0x2000||out!=0x1000||port!=expectedPort||opmod!=3||op!=0x24||wait!=60000||memcmp(inbox.virt,expected,4096))return false;
  *status=0;
  if(mode==3){fCmdWedged=true;return false;}
  if(mode==4){*status=7;return false;}
  memcpy(mailbox,reply,4096);return true;
 }
 bool readModule(UInt32,UInt32,UInt32,Cx3ModuleResult*);
 void mboxFree();bool mailboxNeedsRetention() const;
};
__HW__
__FREE__
__RETAIN__
class Cx3Mlx4 {
public:
 Mlx4Hw fHw;UInt64 fDeviceGeneration=2;bool fResetCommandsBlocked=false,exhausted=false;
 bool fIcmMapped=true,fHcaInited=true;
 bool retentionExhausted(){return exhausted;}
 IOReturn exportModuleRead(UInt32,UInt32,UInt32,Cx3ModuleResult*);
};
__EXPORT__
struct IOExternalMethodArguments {
 UInt32 scalarInputCount=3,scalarOutputCount=0;UInt64 *scalarInput=nullptr;
 void *structureInput=nullptr;size_t structureInputSize=0;
 void *structureOutput=nullptr;size_t structureOutputSize=sizeof(Cx3ModuleResult);
};
class Cx3Mlx4UserClient {public:Cx3Mlx4 *fProvider=nullptr;IOReturn doModuleRead(IOExternalMethodArguments*);};
__HANDLER__
static Cx3Mlx4 *driver=nullptr;static unsigned opens=0;
static int gConn=1;
static int openService(){++opens;return 0;}
static int IOConnectCallMethod(int,unsigned sel,const UInt64 *inputs,unsigned count,const void*,size_t,UInt64*,UInt32*,void *out,size_t *size){
 if(sel!=50||count!=3||*size!=sizeof(Cx3ModuleResult))return -1;
 return driver->exportModuleRead((UInt32)inputs[0],(UInt32)inputs[1],(UInt32)inputs[2],(Cx3ModuleResult*)out);
}
__CLI__
static unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL line %d: %s\n",__LINE__,#x);}}while(0)
int main(){
 Cx3ModuleResult r;
 {Mlx4Hw h;CHECK(h.readModule(1,4,3,&r));CHECK(r.returnedBytes==3&&r.rawBytes==256&&r.deviceGeneration==2&&r.data[0]==0x12&&r.data[2]==0x56&&h.calls==1);h.mboxFree();CHECK(h.frees==1&&h.fModuleInbox==nullptr);}
 {Mlx4Hw h;h.expectedOffset=352;CHECK(h.readModule(1,352,3,&r));CHECK(r.i2cAddress==0x51&&r.wireOffset==96);}
 {Mlx4Hw h;CHECK(!h.readModule(1,250,12,&r));CHECK(h.calls==0&&h.fModuleInbox==nullptr&&r.returnedBytes==0);h.fCmdWedged=true;CHECK(!h.readModule(1,4,3,&r));CHECK(h.calls==0&&h.mailbox[0]==0xee);}
 {Mlx4Hw h;h.reply[4]=7;h.expectedOffset=352;CHECK(!h.readModule(1,352,3,&r));CHECK(r.status==(UInt64)kIOReturnUnsupported&&r.madStatus==0x700&&r.returnedBytes==0&&r.data[0]==0&&r.rawBytes==256);}
 {Mlx4Hw h;h.mode=3;CHECK(!h.readModule(1,4,3,&r));CHECK(h.mailboxNeedsRetention());h.mboxFree();CHECK(h.frees==0&&h.fModuleInbox==&h.inbox&&h.fMboxRetained);CHECK(!h.readModule(1,4,3,&r)&&h.calls==1);}
 {Cx3Mlx4 d;Cx3Mlx4UserClient c;c.fProvider=&d;UInt64 in[]={1,4,3};IOExternalMethodArguments a;a.scalarInput=in;a.structureOutput=&r;CHECK(c.doModuleRead(&a)==0&&d.fHw.calls==1);a.structureOutputSize--;CHECK(c.doModuleRead(&a)==kIOReturnBadArgument&&d.fHw.calls==1);}
 {Cx3Mlx4 d;d.fHcaInited=false;CHECK(d.exportModuleRead(1,4,3,&r)==0&&r.status==(UInt64)kIOReturnNotReady&&d.fHw.calls==0);}
 {Cx3Mlx4 d;driver=&d;CHECK(cmdModuleRead(1,4,3,1)==0);unsigned before=opens;CHECK(cmdModuleRead(1,255,3,0)==2&&opens==before);d.fHw.mode=4;CHECK(cmdModuleRead(1,4,3,1)==1);}
 printf("module smoke: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
'''
for name,value in methods.items():code=code.replace('__'+name+'__',value)
with tempfile.TemporaryDirectory(prefix='cx3-module-smoke-') as folder:
 p=Path(folder);(p/'smoke.cpp').write_text(code)
 subprocess.run(['/usr/bin/clang++','-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=undefined','-I',str(ROOT/'src'),str(p/'smoke.cpp'),'-o',str(p/'smoke')],check=True)
 subprocess.run([str(p/'smoke')],check=True)
