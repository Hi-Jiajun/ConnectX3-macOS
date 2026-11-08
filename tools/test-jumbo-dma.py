#!/usr/bin/python3
"""Exercise production DMA address arithmetic on synthetic IOVA page lists."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/Mlx4Hw.cpp').read_text()
start = source.index('UInt64 Mlx4Hw::rqBufferPhys(')
body = source[start:source.index('\n}\n', start) + 3]
code = r'''
#include <cstdint>
#include <cstdio>
using UInt32=uint32_t;using UInt64=uint64_t;
#define MLX4_ICM_PAGE_SIZE 4096u
#define MLX4_ICM_PAGE_SHIFT 12u
struct Mlx4IcmPage{UInt64 busAddr;void*kernVirt;};
struct Mlx4Icm{UInt64 bytes;UInt32 npages;Mlx4IcmPage*pages;};
struct Mlx4Hw{UInt64 rqBufferPhys(const Mlx4Icm*,UInt32,UInt32)const;};
METHOD
static unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL %u: %s\n",__LINE__,#x);}}while(0)
int main(){
 Mlx4Hw hw;Mlx4IcmPage pages[8]{};Mlx4Icm b{8*4096,8,pages};
 for(unsigned i=0;i<8;++i)pages[i].busAddr=0x123450000ull+UInt64(i)*4096;
 CHECK(hw.rqBufferPhys(&b,16384,0)==pages[0].busAddr);
 CHECK(hw.rqBufferPhys(&b,16384,1)==pages[4].busAddr);
 CHECK(hw.rqBufferPhys(&b,2048,3)==pages[1].busAddr+2048);
 pages[3].busAddr+=4096;
 CHECK(!hw.rqBufferPhys(&b,16384,0)); // single-segment WQE must not span a hole
 CHECK(hw.rqBufferPhys(&b,16384,1)==pages[4].busAddr); // independent slot survives
 pages[3].busAddr-=4096;
 CHECK(!hw.rqBufferPhys(&b,16384,2));
 CHECK(!hw.rqBufferPhys(&b,768,0));
 CHECK(!hw.rqBufferPhys(&b,16384,0xffffffffu));
 b.bytes=16383;CHECK(!hw.rqBufferPhys(&b,16384,0));b.bytes=32768;
 pages[1].busAddr|=1;CHECK(!hw.rqBufferPhys(&b,16384,0));pages[1].busAddr&=~1ull;
 b.pages=nullptr;CHECK(!hw.rqBufferPhys(&b,16384,0));
 printf("jumbo DMA: %u checks, %u failures (real arithmetic, synthetic IOVA)\n",checks,failures);
 return failures?1:0;
}
'''.replace('METHOD', body)
with tempfile.TemporaryDirectory(prefix='cx3-jumbo-dma-') as directory:
    exe = str(Path(directory) / 'test')
    subprocess.run(['/usr/bin/clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-x', 'c++', '-', '-o', exe], input=code, text=True, check=True)
    raise SystemExit(subprocess.run([exe], timeout=10).returncode)
