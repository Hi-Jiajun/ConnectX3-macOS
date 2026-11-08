#!/usr/bin/env python3
"""Run the production CLOSE_HCA command wrapper over a recording cmdBox seam."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


def harness(red=False):
    hw_source = (ROOT / "src/Mlx4Hw.cpp").read_text()
    if red or "bool Mlx4Hw::closeHca(" not in hw_source:
        impl = "bool Mlx4Hw::closeHca(UInt32 *outFwStatus) { (void)outFwStatus; return false; }"
    else:
        impl = function(hw_source, "bool Mlx4Hw::closeHca(")
    return r'''#include <cstdint>
#include <cstdio>
#include "mlx4_regs.h"
using UInt8=uint8_t; using UInt16=uint16_t; using UInt32=uint32_t; using UInt64=uint64_t;
class Mlx4Hw {
 public:
  bool result=true; UInt32 fwStatus=0; unsigned calls=0;
  UInt64 inParam=99,outParam=99; UInt32 inModifier=99,timeout=0; UInt8 opMod=99; UInt16 op=0;
  bool statusPtrSeen=false;
  bool cmdBox(UInt64 in,UInt64 out,UInt32 modifier,UInt8 modifierOp,UInt16 opcode,
              UInt32 wait,UInt32 *status) {
    ++calls; inParam=in;outParam=out;inModifier=modifier;opMod=modifierOp;
    op=opcode;timeout=wait;statusPtrSeen=status!=nullptr;
    if(status)*status=fwStatus;
    return result;
  }
  bool closeHca(UInt32 *);
};
IMPLEMENTATION
static unsigned checks=0,failures=0;
#define CHECK(label,condition) do{++checks;if(!(condition)){++failures;std::printf("FAIL %s\n",label);}}while(0)
static void shape(Mlx4Hw &h) {
 CHECK("one command only",h.calls==1);
 CHECK("zero params and nonpanic opmod",h.inParam==0 && h.outParam==0 && h.inModifier==0 && h.opMod==0);
 CHECK("opcode and class C timeout",h.op==MLX4_CMD_CLOSE_HCA && h.timeout==MLX4_CMD_TIME_CLASS_C);
 CHECK("status output provided to cmdBox",h.statusPtrSeen);
}
int main() {
 UInt32 status=0;
 { Mlx4Hw h; h.result=true;h.fwStatus=0; CHECK("success accepted",h.closeHca(&status));
   CHECK("success status copied",status==0);shape(h); }
 { Mlx4Hw h; h.result=false;h.fwStatus=CX3_FWSTATUS_TIMEOUT;status=0;
   CHECK("bool failure rejected",!h.closeHca(&status));CHECK("failure status preserved",status==CX3_FWSTATUS_TIMEOUT);shape(h); }
 { Mlx4Hw h; h.result=true;h.fwStatus=0x1234;status=0;
   CHECK("true plus fw error rejected",!h.closeHca(&status));CHECK("nonzero fw status preserved",status==0x1234);shape(h); }
 { Mlx4Hw h; h.result=false;h.fwStatus=0;status=0xaaaa;
   CHECK("false plus zero rejected",!h.closeHca(&status));CHECK("zero result copied",status==0);shape(h); }
 { Mlx4Hw h; h.result=false;h.fwStatus=CX3_FWSTATUS_NOT_POSTED;status=0;
   CHECK("not posted rejected",!h.closeHca(&status));CHECK("not posted preserved",status==CX3_FWSTATUS_NOT_POSTED);shape(h); }
 { Mlx4Hw h; h.result=true;h.fwStatus=0x5678;
   CHECK("null caller output still uses command status",!h.closeHca(nullptr));shape(h); }
 std::printf("CLOSE_HCA command: %u checks, %u failures\n",checks,failures);
 return failures?1:0;
}
'''.replace("IMPLEMENTATION", impl)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--red", action="store_true", help="force the absent-endpoint behavior baseline")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="cx3-close-hca-command-") as temp:
        binary = str(Path(temp) / "test")
        built = subprocess.run(["/usr/bin/clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                "-I", str(ROOT / "src"), "-x", "c++", "-", "-o", binary],
                               input=harness(args.red), capture_output=True, text=True)
        if built.returncode:
            print(built.stderr, end="")
            return built.returncode
        run = subprocess.run([binary], capture_output=True, text=True, timeout=10)
        print(run.stdout, end="")
        print(run.stderr, end="")
        return run.returncode


if __name__ == "__main__":
    raise SystemExit(main())
