#!/usr/bin/env python3
"""Production enable/disable plus real callback gate; no IOKit or device access."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start)+3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--provider', type=Path, default=ROOT/'src/Cx3Mlx4.cpp')
    args = parser.parse_args()
    provider = args.provider.read_text()
    bodies = '\n'.join(method(provider, sig) for sig in
                       ('IOReturn Cx3Mlx4::enable(', 'IOReturn Cx3Mlx4::disable('))
    source = r'''
#include <cstdio>
#include <functional>
#define CX3_HOST_TEST
#include "mlx4_pure.h"
#include "Cx3PortQueues.h"
using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnNotPermitted=-2;
constexpr unsigned kIONetworkLinkValid=1;
constexpr UInt64 kNetTickNs=1000000;
static UInt64 cx3WakeInTicks(UInt64 n){return n;}
static void logSink(const char*,...){}
#define LOG(...) logSink(__VA_ARGS__)
static unsigned checks=0,failures=0,cases=0;
#define CHECK(x) do {++checks;if(!(x)){++failures;std::printf("FAIL %s line %u: %s\n",name,__LINE__,#x);}}while(0)
struct IONetworkInterface{};
struct Timer {
 unsigned wakes=0,cancels=0;bool armed=false;
 std::function<void()> onWake,onCancel;
 void wakeAtTime(UInt64){++wakes;armed=true;if(onWake)onWake();}
 void cancelTimeout(){++cancels;armed=false;if(onCancel)onCancel();}
};
struct Hardware{bool wedged=false;bool cmdWedged(){return wedged;}};
struct Mlx4Icm { int id; };
class Cx3Mlx4 {
public:
    Cx3AutoNetworkResult fAutoNetwork{}; bool fAutoNetworkUserDisabled=false;
    Cx3PortQueues fPortQueues[2] = { []() { Cx3PortQueues q{}; q.init(1); q.txQpUp = true; q.qpUp = true; q.ingressUp = true; return q; }(), []() { Cx3PortQueues q{}; q.init(2); return q; }() };

 public:
 Mlx4TxAdmissionGate fNetTxAdmission;
 Hardware fHw;
 bool fNetEnabled=false,fQuarExhausted=false,fResetCommandsBlocked=false,fStopRetained=false;
 bool poisoned=false;
 Mlx4Icm owner[3]{{1},{2},{3}};
 Timer timer;Timer *fNetTimer=&timer;IONetworkInterface iface;IONetworkInterface *fNetIface=&iface;
 UInt32 fNetUnit=2;unsigned publishes=0,inactive=0;
 std::function<void()> onPublish;
 Cx3Mlx4(){fNetTxAdmission.init();}
 bool netAttachPoisoned(){return poisoned;}
 void publishLinkStatus(const char*){++publishes;if(onPublish)onPublish();}
 void setLinkStatus(unsigned,int,UInt64){++inactive;}
 IOReturn enable(IONetworkInterface*);
 IOReturn disable(IONetworkInterface*);
};
BODIES
int main(){
 {const char *name="100 same-owner interface down/up cycles";++cases;Cx3Mlx4 d;
  d.fPortQueues[0].txQpRec=&d.owner[0];d.fPortQueues[0].qpRec=&d.owner[1];d.fPortQueues[0].ingressRec=&d.owner[2];
  for(unsigned i=0;i<100;++i){
   CHECK(d.enable(&d.iface)==0 && d.fNetEnabled && d.timer.armed);
   CHECK(d.disable(&d.iface)==0 && !d.fNetEnabled && !d.timer.armed);
   CHECK(d.fNetTxAdmission.drained() && !d.fNetTxAdmission.isClosed());
   CHECK(d.fPortQueues[0].txQpRec==&d.owner[0] && d.fPortQueues[0].qpRec==&d.owner[1] && d.fPortQueues[0].ingressRec==&d.owner[2]);
  }
  CHECK(d.timer.wakes==100 && d.timer.cancels==100 && d.inactive==100);
 }
 for(unsigned mode=0;mode<9;++mode){const char *name="rejected enable has no publish/arm and balanced gate";++cases;Cx3Mlx4 d;
  if(mode==0)d.fResetCommandsBlocked=true;
  if(mode==1)d.fStopRetained=true;
  if(mode==2)d.fQuarExhausted=true;
  if(mode==3)d.fHw.wedged=true;
  if(mode==4)d.poisoned=true;
  if(mode==5){d.fPortQueues[0].txQpRec=&d.owner[0];d.fPortQueues[0].txQpUp=false;}
  if(mode==6){d.fPortQueues[0].qpRec=&d.owner[1];d.fPortQueues[0].qpUp=false;}
  if(mode==7){d.fPortQueues[0].ingressRec=&d.owner[2];d.fPortQueues[0].ingressUp=false;}
  if(mode==8)d.fNetTxAdmission.close();
  CHECK(d.enable(&d.iface)==kIOReturnNotPermitted);
  CHECK(!d.fNetEnabled && !d.timer.armed && d.publishes==0 && d.timer.wakes==0);
  CHECK(d.fNetTxAdmission.drained());
 }
 {const char *name="null and repeated callbacks balance admission";++cases;Cx3Mlx4 d;
  CHECK(d.enable(nullptr)==kIOReturnBadArgument);CHECK(d.disable(nullptr)==kIOReturnBadArgument);
  CHECK(d.fNetTxAdmission.drained());CHECK(d.disable(&d.iface)==0);
  CHECK(d.enable(&d.iface)==0);CHECK(d.enable(&d.iface)==0);
  CHECK(d.timer.wakes==1 && d.publishes==2);CHECK(d.disable(&d.iface)==0);
  CHECK(d.disable(&d.iface)==0 && d.timer.cancels==1 && d.fNetTxAdmission.drained());
 }
 for(unsigned mode=0;mode<3;++mode){const char *name="stop overlaps enable: reference retained until callback leaves";++cases;Cx3Mlx4 d;
  auto close=[&](){CHECK(d.fNetTxAdmission.active()==1);CHECK(d.fNetTxAdmission.close()==1);
                 CHECK(!d.fNetTxAdmission.drained());};
  if(mode==0)d.onPublish=close;
  if(mode==1)d.timer.onWake=close;
  if(mode==2){CHECK(d.enable(&d.iface)==0);d.onPublish=close;}
  CHECK(d.enable(&d.iface)==kIOReturnNotPermitted);
  CHECK(!d.fNetEnabled && !d.timer.armed && d.fNetTxAdmission.isClosed() && d.fNetTxAdmission.drained());
  unsigned wakes=d.timer.wakes,pubs=d.publishes,cancels=d.timer.cancels;
  CHECK(d.enable(&d.iface)==kIOReturnNotPermitted);
  CHECK(d.disable(&d.iface)==kIOReturnSuccess);
  CHECK(d.timer.wakes==wakes && d.publishes==pubs && d.timer.cancels==cancels);
 }
 {const char *name="stop during disable counts callback and retains timer";++cases;Cx3Mlx4 d;
  CHECK(d.enable(&d.iface)==0);
  d.timer.onCancel=[&](){CHECK(d.fNetTxAdmission.active()==1);CHECK(d.fNetTxAdmission.close()==1);CHECK(!d.fNetTxAdmission.drained());};
  CHECK(d.disable(&d.iface)==0);CHECK(!d.fNetEnabled && !d.timer.armed && d.fNetTxAdmission.drained());
 }
 {const char *name="reset-blocked disable still stops host delivery without reopening gate";++cases;Cx3Mlx4 d;
  CHECK(d.enable(&d.iface)==0);d.fResetCommandsBlocked=true;
  CHECK(d.disable(&d.iface)==0 && !d.fNetEnabled && !d.timer.armed);
  CHECK(d.enable(&d.iface)==kIOReturnNotPermitted && d.fNetTxAdmission.drained());
 }
 std::printf("network lifecycle: %u cases, %u checks, %u failures (production callbacks / host seams)\n",cases,checks,failures);
 return failures?1:0;
}
'''.replace('BODIES', bodies)
    with tempfile.TemporaryDirectory(prefix='cx3-network-lifecycle-') as temporary:
        exe = str(Path(temporary)/'test')
        subprocess.run(['/usr/bin/clang++','-std=c++17','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                        '-I',str(ROOT/'src'),'-x','c++','-','-o',exe],
                       input=source,text=True,check=True,timeout=60)
        return subprocess.run([exe],timeout=20).returncode


if __name__=='__main__':raise SystemExit(main())
