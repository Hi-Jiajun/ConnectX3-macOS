#!/usr/bin/env python3
"""Host-run fixtures for the production PCIe query method (no IOKit/device)."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/Mlx4Hw.cpp").read_text()
HEADER = ROOT / "src/cx3_abi.h"

def method_source():
    start = SOURCE.find("IOReturn Mlx4Hw::queryPcieLink(")
    if start < 0:
        return "IOReturn Mlx4Hw::queryPcieLink(Cx3PcieLinkResult *out) { (void)out; return 0; }"
    opening = SOURCE.find("{", start)
    depth = 0
    for pos in range(opening, len(SOURCE)):
        if SOURCE[pos] == "{": depth += 1
        elif SOURCE[pos] == "}":
            depth -= 1
            if depth == 0: return SOURCE[start:pos + 1]
    raise AssertionError("production method has unclosed body")

HARNESS = r'''#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <vector>
#include <map>
#include <utility>
#include "ABI_H"
typedef uint32_t UInt32; typedef uint16_t UInt16; typedef uint8_t UInt8; typedef uint64_t UInt64;
typedef unsigned long IOByteCount;
typedef int IOReturn;
enum { kIOReturnSuccess=0, kIOReturnBadArgument=-1, kIOReturnNotReady=-2,
       kIOReturnOffline=-3, kIOReturnUnsupported=-4, kIOReturnNotFound=-5,
       kIOReturnNoResources=-6, kIOReturnBusy=-7 };
struct IOPCIDevice {
  std::map<unsigned,unsigned> b;
  std::map<unsigned,unsigned> counts;
  std::map<unsigned,unsigned> mutate;
  std::vector<unsigned> reads;
  unsigned getBusNumber(){return 3;} unsigned getDeviceNumber(){return 2;} unsigned getFunctionNumber(){return 1;}
  unsigned value(unsigned o,unsigned n){reads.push_back(o); unsigned c=++counts[o]; unsigned v=0; for(unsigned i=0;i<n;i++)v|=(b[o+i]&255)<<(8*i); if(mutate[o] && c>=mutate[o])v^=1; return v;}
  UInt8 configRead8(unsigned o){ return value(o,1)&255; }
  UInt16 configRead16(unsigned o){ return value(o,2)&65535; }
  UInt32 configRead32(unsigned o){ return value(o,4); }
};
class Mlx4Hw { public: IOPCIDevice *fPci; Mlx4Hw(IOPCIDevice*p):fPci(p){} IOReturn queryPcieLink(Cx3PcieLinkResult*); };
METHOD
static void put(IOPCIDevice &p,unsigned o,unsigned v,unsigned n){for(unsigned i=0;i<n;i++)p.b[o+i]=(v>>(8*i))&255;}
static IOPCIDevice valid(unsigned ver=1,unsigned sta=0x2011,unsigned cap=0x100083){IOPCIDevice p; put(p,0,0x100715b3,4);put(p,0x2c,0x123415b3,4);put(p,6,0x10,2);put(p,0xe,0x80,1);put(p,0x34,0x40,1);put(p,0x40,0x10,2);put(p,0x42,ver,2);put(p,0x4c,cap,4);put(p,0x52,sta,2);if(ver==2)put(p,0x6c,0xe,4);return p;}
#define C(name,cond) do{if(!(cond)){fprintf(stderr,"FAIL: %s\n",name);fails++;}}while(0)
int main(){int fails=0; Cx3PcieLinkResult r={};
  auto p=valid(1,0x2011,0x100083); Mlx4Hw h(&p); C("v1 transport",h.queryPcieLink(&r)==0);
  C("v1 success and validity",r.reason==0 && r.status==0 && r.validity==15);
  C("current Gen1 x1 remains separate from max Gen3 x8",(r.lnkSta&15)==1 && ((r.lnkSta>>4)&63)==1 && (r.lnkCap&15)==3 && ((r.lnkCap>>4)&63)==8);
  C("raw bytes and validity recorded",r.raw[0x52]==0x11 && (r.readValid[0x52/8]&(1u<<(0x52%8))));
  auto p2=valid(2,0x2843,0x100083); Mlx4Hw h2(&p2); h2.queryPcieLink(&r);
  C("v2 cap2 and training raw",r.validity==31 && r.lnkCap2==0xe && ((r.lnkSta>>11)&1)==1);
  auto unknown=valid(1,0x2077,0x100083); Mlx4Hw hu(&unknown); hu.queryPcieLink(&r);
  C("unknown speed preserved raw",(r.lnkSta&15)==7 && r.reason==0);
  Mlx4Hw no(nullptr); no.queryPcieLink(&r); C("no provider rejected before reads",r.reason==CX3_PCIE_REASON_NOT_READY && r.readValid[0]==0);
  auto nocap=valid(); put(nocap,6,0,2); Mlx4Hw hn(&nocap); hn.queryPcieLink(&r);
  C("no capability avoids pointer read",r.reason==CX3_PCIE_REASON_NO_PCIE_CAP && r.readValid[0x34/8]==0);
  auto bad=valid(); put(bad,0x34,0x3c,1); Mlx4Hw hb(&bad); hb.queryPcieLink(&r);
  C("bad pointer rejected",r.reason==CX3_PCIE_REASON_MALFORMED_LIST);
  auto cycle=valid(); put(cycle,0x40,0x4040,2); Mlx4Hw hc(&cycle); hc.queryPcieLink(&r);
  C("self cycle rejected",r.reason==CX3_PCIE_REASON_CYCLE && r.walkCount==1);
  auto unsupported=valid(3); Mlx4Hw hup(&unsupported); hup.queryPcieLink(&r);
  C("future version rejected",r.reason==CX3_PCIE_REASON_UNSUPPORTED_PCIE);
  auto unsupportedType=valid(); put(unsupportedType,0xe,1,1); Mlx4Hw hut(&unsupportedType); hut.queryPcieLink(&r);
  C("bridge header rejected",r.reason==CX3_PCIE_REASON_UNSUPPORTED_HEADER);
  auto unaligned=valid(); put(unaligned,0x34,0x42,1); Mlx4Hw hua(&unaligned); hua.queryPcieLink(&r);
  C("unaligned pointer rejected",r.reason==CX3_PCIE_REASON_MALFORMED_LIST);
  auto truncated=valid(); put(truncated,0x34,0xfc,1); put(truncated,0xfc,0x10,2); put(truncated,0xfe,1,2); Mlx4Hw ht(&truncated); ht.queryPcieLink(&r);
  C("capability near end is truncated",r.reason==CX3_PCIE_REASON_TRUNCATED_CAP);
  auto allones=valid(); put(allones,6,0xffff,2); Mlx4Hw hall(&allones); hall.queryPcieLink(&r);
  C("all-ones status is read failure",r.reason==CX3_PCIE_REASON_READ_UNAVAILABLE && allones.reads.back()==6);
  auto flagsFail=valid(); put(flagsFail,0x42,0xffff,2); Mlx4Hw hf(&flagsFail); hf.queryPcieLink(&r);
  C("all-ones flags stop before link fields",r.reason==CX3_PCIE_REASON_READ_UNAVAILABLE && flagsFail.reads.back()==0x42);
  auto headerFail=valid(); put(headerFail,0xe,0xff,1); Mlx4Hw hhead(&headerFail); hhead.queryPcieLink(&r);
  C("all-ones header type stops",r.reason==CX3_PCIE_REASON_READ_UNAVAILABLE && headerFail.reads.back()==0xe);
  auto ptrFail=valid(); put(ptrFail,0x34,0xff,1); Mlx4Hw hp(&ptrFail); hp.queryPcieLink(&r);
  C("all-ones capability pointer stops",r.reason==CX3_PCIE_REASON_READ_UNAVAILABLE && ptrFail.reads.back()==0x34);
  auto nodeFail=valid(); put(nodeFail,0x40,0xffff,2); Mlx4Hw hnode(&nodeFail); hnode.queryPcieLink(&r);
  C("all-ones cap header stops",r.reason==CX3_PCIE_REASON_READ_UNAVAILABLE && nodeFail.reads.back()==0x40);
  auto lnkFail=valid(); put(lnkFail,0x4c,0xffffffffu,4); Mlx4Hw hl(&lnkFail); hl.queryPcieLink(&r);
  C("all-ones LnkCap stops before recheck",r.reason==CX3_PCIE_REASON_READ_UNAVAILABLE && lnkFail.reads.back()==0x4c);
  auto staFail=valid(); put(staFail,0x52,0xffff,2); Mlx4Hw hs(&staFail); hs.queryPcieLink(&r);
  C("all-ones LnkSta stops before recheck",r.reason==CX3_PCIE_REASON_READ_UNAVAILABLE && staFail.reads.back()==0x52);
  auto cap2Fail=valid(2); put(cap2Fail,0x6c,0xffffffffu,4); Mlx4Hw hcap2(&cap2Fail); hcap2.queryPcieLink(&r);
  C("all-ones LnkCap2 stops before recheck",r.reason==CX3_PCIE_REASON_READ_UNAVAILABLE && cap2Fail.reads.back()==0x6c);
  auto changed=valid(); changed.mutate[0]=2; Mlx4Hw hchanged(&changed); hchanged.queryPcieLink(&r);
  C("identity change rejected without overwriting first sample",r.reason==CX3_PCIE_REASON_INCONSISTENT && r.raw[0]==0xb3);
  auto chain=valid(); put(chain,0x34,0x40,1); for(unsigned i=0;i<48;i++)put(chain,0x40+4*i,((i==47?0:(0x44+4*i))<<8)|1,2);
  Mlx4Hw hchain(&chain); hchain.queryPcieLink(&r);
  C("48-node unique conventional chain is bounded",r.reason==CX3_PCIE_REASON_NO_PCIE_CAP && r.walkCount==48);
  printf("pcie query: %d focused assertions failed\n",fails); return fails?1:0;}
'''

def main():
    start = SOURCE.find("IOReturn Mlx4Hw::queryPcieLink(")
    if start >= 0:
        body = method_source()
        for forbidden in ("configWrite", "writeBar", "cmdBox", "resetHca", "icmAlloc"):
            if forbidden in body:
                print("FAIL: PCIe query has forbidden hardware surface: " + forbidden)
                return 1
    source = HARNESS.replace('"ABI_H"', '"' + str(HEADER) + '"').replace("METHOD", method_source())
    with tempfile.TemporaryDirectory(prefix="cx3-pcie-query-") as td:
        cpp, exe = Path(td)/"query.cpp", Path(td)/"query"
        cpp.write_text(source)
        built = subprocess.run(["c++", "-std=c++17", "-I", str(ROOT/"src"), str(cpp), "-o", str(exe)], capture_output=True, text=True)
        if built.returncode:
            print(built.stderr, end="")
            return 2
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        print(run.stdout, end=""); print(run.stderr, end="")
        return run.returncode

if __name__ == "__main__": raise SystemExit(main())
