#!/usr/bin/env python3
"""Execute actual start() at the exclusive-provider boundary.

Catches continuing into PCI writes/BAR initialization after provider open is
denied (including a provider still owned by a retained old controller). The
IOKit superclass, PCI open/config and HW init are explicit host seams. This
does not prove real provider exclusion, DMA quiescence or a hardware reload.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path)
    args = parser.parse_args()
    source = (args.source or ROOT/'src/Cx3Mlx4.cpp').read_text()
    start = source.index('bool Cx3Mlx4::start(IOService *provider)')
    body = source[start:source.index('\n}\n', start)+3]
    code = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#define CX3_HOST_TEST
#include "mlx4_pure.h"
#include "Cx3PortState.h"
#define CX3_START_LEVEL 2
#define CX3_BUILD_TAG "host-start-fixture"
struct IOService {
  virtual ~IOService()=default;
  bool superOk=true;unsigned superStarts=0,registrations=0;
  bool start(IOService*){++superStarts;return superOk;}
  void registerService(){++registrations;}
};
#define OSDynamicCast(type,value) dynamic_cast<type*>(value)
static bool bootOff=false;
static bool cx3BootOff(){return bootOff;}
static void sink(const char*,...){}
#define LOG(...) sink(__VA_ARGS__)
#define LOGBOOT(...) sink(__VA_ARGS__)
enum {kIOPCIConfigVendorID=0,kIOPCIConfigDeviceID=2,kIOPCIConfigCommand=4,
      kIOPCIConfigRevisionID=8,kIOPCIConfigSubSystemVendorID=44,kIOPCIConfigSubSystemID=46};
struct IOPCIDevice:IOService {
  void *owner=nullptr;bool deny=false;UInt16 vendor=0x15b3,device=0x1007;
  unsigned opens=0,closes=0,reads=0,writes=0;UInt8 bus=1,slot=0,function=0;
  bool open(void*client){++opens;if(deny || (owner && owner!=client))return false;owner=client;return true;}
  void close(void*client){++closes;if(owner==client)owner=nullptr;}
  UInt16 configRead16(unsigned offset){++reads;switch(offset){
    case kIOPCIConfigVendorID:return vendor;case kIOPCIConfigDeviceID:return device;
    case kIOPCIConfigCommand:return 6;case kIOPCIConfigSubSystemVendorID:return 0x15b3;
    case kIOPCIConfigSubSystemID:return 0x0015;default:return 0;}}
  UInt32 configRead32(unsigned){++reads;return 0x020000a1;}
  UInt8 getBusNumber(){return bus;}UInt8 getDeviceNumber(){return slot;}UInt8 getFunctionNumber(){return function;}
  bool setBusMasterEnable(bool){++writes;return true;}
  bool setIOEnable(bool){++writes;return true;}
  bool setMemoryEnable(bool){++writes;return true;}
};
struct IOLock{};
static IOLock attachLock;
static unsigned lockAllocs=0,callAllocs=0;
static IOLock *IOLockAlloc(){++lockAllocs;return &attachLock;}
static void IOLockFree(IOLock*){}
static void *thread_call_allocate(void(*)(void*,void*),void*){++callAllocs;return &callAllocs;}
static bool thread_call_free(void*){return true;}
struct Mlx4Hw {
  bool ok=true;unsigned inits=0;UInt64 generation=0;
  bool init(IOPCIDevice*,IOService*){++inits;return ok;}
  void setDeviceGeneration(UInt64 g){generation=g;}
  bool bar0Mapped(){return true;}bool bar2Mapped(){return true;}
};
struct Cx3Mlx4:IOService {
  using super=IOService;
  enum {kCx3AttachWorkerIdle=0};
  IOPCIDevice *fPci=nullptr;bool fPciOpen=false,fStartPciIdentityValid=false,fStopRetained=false;
  Mlx4PciIdentity fStartPciIdentity{};UInt64 fDeviceGeneration=0;
  Cx3PortState fPorts[2]{};
  UInt16 fPciCommandAfterEnable=0;UInt32 fBarMapFlags=0;
  Mlx4Hw fHw;IOLock *fNetAttachLock=nullptr;void *fNetAttachCall=nullptr,*fNetDetachCall=nullptr;
  unsigned probes=0;
  static void netAttachThread(void*,void*){}static void netDetachThread(void*,void*){}
  void netAttachWorkerMark(unsigned){}
  void readOnlyProbe(){++probes;}
  bool start(IOService*);
};
METHOD
static unsigned cases=0,checks=0,failures=0;
#define CHECK(name,e) do{++checks;if(!(e)){++failures;printf("FAIL case %u: %s\n",cases,name);}}while(0)
static void denied(Cx3Mlx4 &driver,IOPCIDevice &pci){
  unsigned locks=lockAllocs,calls=callAllocs;
  CHECK("provider refusal returns failure",!driver.start(&pci));
  CHECK("provider refusal performs no PCI enable or hardware-layer initialization",pci.writes==0 && driver.fHw.inits==0 && driver.probes==0);
  CHECK("provider refusal publishes no service or attach/detach workers",driver.registrations==0 && lockAllocs==locks && callAllocs==calls);
  CHECK("provider refusal does not close another owner's device",pci.closes==0);
  CHECK("provider refusal leaves no actionable PCI handle or accepted identity",!driver.fPci && !driver.fPciOpen && !driver.fStartPciIdentityValid && driver.fDeviceGeneration==0);
}
int main(){
  {
    ++cases;Cx3Mlx4 driver;IOPCIDevice pci;pci.deny=true;
    denied(driver,pci);CHECK("exclusive open attempted once",pci.opens==1);
  }
  {
    ++cases;Cx3Mlx4 oldOwner,newOwner;IOPCIDevice pci;pci.owner=&oldOwner;
    denied(newOwner,pci);
    CHECK("old retained owner's exclusive provider survives rejected fresh start",pci.owner==&oldOwner);
  }
  {
    ++cases;Cx3Mlx4 driver;IOPCIDevice pci;
    CHECK("clean fresh start succeeds",driver.start(&pci));
    CHECK("successful start holds provider and publishes once",pci.owner==&driver && pci.opens==1 && pci.closes==0 && driver.fPciOpen && driver.registrations==1);
    CHECK("only successful open reaches three PCI enables and hardware init",pci.writes==3 && driver.fHw.inits==1 && driver.probes==1);
    CHECK("fresh instance establishes generation and worker context",driver.fDeviceGeneration==1 && driver.fHw.generation==1 && driver.fNetAttachLock && driver.fNetAttachCall && driver.fNetDetachCall);
  }
  {
    ++cases;Cx3Mlx4 driver;IOPCIDevice pci;bootOff=true;
    CHECK("boot-off returns before superclass/config/open",!driver.start(&pci) && driver.superStarts==0 && pci.reads==0 && pci.opens==0 && pci.writes==0 && driver.fHw.inits==0);
    bootOff=false;
  }
  {
    ++cases;Cx3Mlx4 driver;IOPCIDevice pci;driver.superOk=false;
    CHECK("superclass failure returns before PCI access",!driver.start(&pci) && driver.superStarts==1 && pci.reads==0 && pci.opens==0 && pci.writes==0 && driver.fHw.inits==0);
  }
  {
    ++cases;Cx3Mlx4 driver;IOService other;
    CHECK("non-PCI provider never initializes hardware",!driver.start(&other) && driver.fHw.inits==0 && driver.registrations==0);
  }
  {
    ++cases;Cx3Mlx4 driver;IOPCIDevice pci;pci.device=0xffff;
    CHECK("wrong device refuses before open and PCI enable",!driver.start(&pci) && pci.opens==0 && pci.writes==0 && driver.fHw.inits==0 && driver.registrations==0);
  }
  {
    ++cases;Cx3Mlx4 driver;IOPCIDevice pci;driver.fHw.ok=false;
    CHECK("hardware init failure returns without publication",!driver.start(&pci) && driver.registrations==0);
    CHECK("hardware init failure closes only successfully opened provider",pci.opens==1 && pci.closes==1 && !pci.owner && !driver.fPciOpen && driver.fDeviceGeneration==0);
  }
  for(unsigned mode=0;mode<3;++mode){
    ++cases;Cx3Mlx4 driver;IOPCIDevice oldPci,newPci;
    driver.fPci=mode<2?&oldPci:nullptr;
    if(mode==0)driver.fStopRetained=true;
    if(mode==1){driver.fPciOpen=true;oldPci.owner=&driver;}
    if(mode==2){driver.fDeviceGeneration=7;driver.fHw.generation=7;}
    const auto old=driver.fPci;const auto generation=driver.fDeviceGeneration;
    CHECK("retained/open/previous-generation instance cannot be restarted",!driver.start(&newPci));
    CHECK("same-instance restart refuses before superclass and provider access",driver.superStarts==0 && newPci.reads==0 && newPci.opens==0 && newPci.writes==0 && driver.fHw.inits==0);
    CHECK("restart refusal preserves original provider and generation",driver.fPci==old && driver.fDeviceGeneration==generation);
    CHECK("restart refusal publishes no new service or worker context",driver.registrations==0 && !driver.fNetAttachCall && !driver.fNetDetachCall);
  }
  {
    ++cases;Cx3Mlx4 driver;IOPCIDevice pci;
    CHECK("control fresh instance starts",driver.start(&pci));
    unsigned reads=pci.reads,opens=pci.opens,writes=pci.writes,calls=callAllocs,locks=lockAllocs;
    CHECK("duplicate start on live same-client owner is rejected",!driver.start(&pci));
    CHECK("duplicate start does not overwrite BAR/worker ownership",driver.fHw.inits==1 && driver.superStarts==1 && driver.registrations==1 && pci.reads==reads && pci.opens==opens && pci.writes==writes && callAllocs==calls && lockAllocs==locks);
  }
  printf("start admission: %u cases, %u checks, %u failures (actual start over host provider/HW seams)\n",cases,checks,failures);
  return failures?1:0;
}
'''.replace('METHOD', body)
    with tempfile.TemporaryDirectory(prefix='cx3-start-admission-') as temporary:
        exe = str(Path(temporary)/'test')
        subprocess.run(['/usr/bin/clang++','-std=c++17','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                        '-I',str(ROOT/'src'),'-x','c++','-','-o',exe],
                       input=code,text=True,check=True)
        return subprocess.run([exe],timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
