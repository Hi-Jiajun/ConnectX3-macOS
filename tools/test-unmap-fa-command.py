#!/usr/bin/env python3
"""Execute the production UNMAP_FA wrapper over a recording command boundary."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path)
    args = parser.parse_args()
    source = (args.source or ROOT/'src/Mlx4Hw.cpp').read_text()
    start = source.index('bool Mlx4Hw::unmapFa(')
    impl = source[start:source.index('\n}', start)+2]
    code = r'''
#include <cstdint>
#include <cstdio>
#include "mlx4_regs.h"
using UInt8=uint8_t;using UInt16=uint16_t;using UInt32=uint32_t;using UInt64=uint64_t;
static void sink(const char*,...){}
#define LOG(...) sink(__VA_ARGS__)
struct Mlx4Hw {
  bool result=true,writeStatus=true;UInt32 reply=0;unsigned calls=0;
  bool shape=true;
  bool cmdBox(UInt64 in,UInt64 out,UInt32 modifier,UInt8 opmod,UInt16 op,UInt32 timeout,UInt32*s){
    ++calls;shape=in==0 && out==0 && modifier==0 && opmod==0 &&
      op==MLX4_CMD_UNMAP_FA && timeout==MLX4_CMD_TIME_CLASS_B && s;
    if(s && writeStatus)*s=reply;
    return result;
  }
  bool unmapFa(UInt32*);
};
IMPLEMENTATION
static unsigned checks=0,failures=0;
#define CHECK(label,e) do{++checks;if(!(e)){++failures;std::printf("FAIL %s\n",label);}}while(0)
int main(){
  for(unsigned mode=0;mode<7;++mode){
    Mlx4Hw h;UInt32 status=123;
    h.result=mode==0 || mode==2 || mode==5 || mode==6;
    h.reply=mode==2?9:mode==3?CX3_FWSTATUS_TIMEOUT:mode==4?CX3_FWSTATUS_NOT_POSTED:0;
    h.writeStatus=mode!=5;
    bool ok=h.unmapFa(mode==6?nullptr:&status);
    CHECK("only command success and zero FW status authorize unmap",ok==(mode==0 || mode==6));
    CHECK("one native zero-argument class B command",h.calls==1 && h.shape);
    if(mode!=6)CHECK("status including unposted/unknown is preserved",status==(mode==5?CX3_FWSTATUS_NOT_POSTED:h.reply));
  }
  std::printf("UNMAP_FA command: %u checks, %u failures\n",checks,failures);
  return failures?1:0;
}
'''.replace('IMPLEMENTATION', impl)
    with tempfile.TemporaryDirectory(prefix='cx3-unmap-fa-command-') as temp:
        binary = str(Path(temp)/'test')
        subprocess.run(['/usr/bin/clang++','-std=c++17','-Wall','-Wextra','-Werror',
                        '-I',str(ROOT/'src'),'-x','c++','-','-o',binary],
                       input=code,text=True,check=True)
        return subprocess.run([binary],timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
