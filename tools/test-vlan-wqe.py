#!/usr/bin/python3
"""Compile the production send encoder; synthetic memory/barriers, no device."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/Mlx4Hw.cpp').read_text()
start = source.index('void Mlx4Hw::sqWriteSendWqe(')
body = source[start:source.index('\n}\n', start) + 3]
start2=source.index('bool Mlx4Hw::setVlanFilter(')
filter_body=source[start2:source.index('\n}\n', start2)+3]
code = r'''
#include <cstdio>
#include <cstring>
#define CX3_HOST_TEST
#include "mlx4_pure.h"
#define LOG(...) ((void)0)
static unsigned barriers=0,checks=0,failures=0;
static UInt8 firstBarrier[TX_WQE_SEND_SIZE],*observed=nullptr;
#define CHECK(x) do{++checks;if(!(x)){++failures;printf("FAIL %u: %s\n",__LINE__,#x);}}while(0)
static void mlx4PutBe32(UInt8*p,UInt32 v){for(unsigned i=0;i<4;++i)p[i]=UInt8(v>>(24-i*8));}
static void mlx4PutBe64(UInt8*p,UInt64 v){for(unsigned i=0;i<8;++i)p[i]=UInt8(v>>(56-i*8));}
static void mlx4Wmb(){if(!barriers&&observed)memcpy(firstBarrier,observed,TX_WQE_SEND_SIZE);++barriers;}
struct Mlx4Hw{void sqWriteSendWqe(UInt8*,UInt32,UInt32,UInt32,UInt32,UInt32,UInt64,UInt32,UInt32,UInt8*,bool,UInt32=0,bool=false,UInt16=0);};
METHOD
struct VlanHardware{
 UInt8 inbox[MLX4_MAILBOX_SIZE]{};void*fMboxVirt=inbox;UInt64 fMboxPhys=0x12345000;bool allocated=true,accepted=true;
 unsigned commands=0;UInt32 port=0,op=0;UInt8 opmod=1;UInt64 address=0;
 bool mboxAlloc(){return allocated;}
 bool cmdBox(UInt64 in,UInt64,UInt32 p,UInt8 m,UInt32 code,UInt32,UInt32*s){++commands;address=in;port=p;op=code;opmod=m;*s=accepted?0:3;return accepted;}
 bool setVlanFilter(UInt32,const UInt8*,UInt32*);
};
FILTER_METHOD
int main(){
 Mlx4Hw hw;UInt8 ring[2*MLX4_TXBB_SIZE],image[TX_WQE_SEND_SIZE];memset(ring,0xff,sizeof(ring));
 observed=ring+MLX4_SQ_WQE_OFFSET(1);
 hw.sqWriteSendWqe(ring,1,257,1,1514,0x12345678,0x123456789abcdef0ull,1,MLX4_OPCODE_SEND,image,false,MLX4_WQE_CTRL_TCP_UDP_CSUM,true,0xa123);
 CHECK(barriers==2&&firstBarrier[0]==0xff&&firstBarrier[3]==0xff);
 CHECK(firstBarrier[4]==0xa1&&firstBarrier[5]==0x23&&firstBarrier[6]==0x40&&firstBarrier[7]==2);
 CHECK(!memcmp(observed,image,sizeof(image))&&observed[0]==0x80&&observed[3]==MLX4_OPCODE_SEND);
 CHECK(observed[TX_WQE_SRCRB_FLAGS_OFFSET+3]==(MLX4_WQE_CTRL_CQ_UPDATE|MLX4_WQE_CTRL_SOLICITED|MLX4_WQE_CTRL_TCP_UDP_CSUM));
 CHECK(observed[TX_WQE_BYTE_COUNT_OFFSET+2]==5&&observed[TX_WQE_BYTE_COUNT_OFFSET+3]==0xea);
 CHECK(observed[TX_WQE_ADDR_OFFSET]==0x12&&observed[TX_WQE_ADDR_OFFSET+7]==0xf0&&ring[4]==0xff);
 barriers=0;hw.sqWriteSendWqe(ring,1,258,0,64,1,4096,1,MLX4_OPCODE_SEND,nullptr,false);
 CHECK(barriers==2&&!observed[4]&&!observed[5]&&!observed[6]&&observed[7]==2&&!observed[0]);
 CHECK(firstBarrier[4]==0&&firstBarrier[5]==0&&firstBarrier[6]==0);
 hw.sqWriteSendWqe(ring,1,259,0,64,1,4096,1,MLX4_OPCODE_SEND,nullptr,false,0,true,0xe000);
 CHECK(observed[4]==0xe0&&observed[5]==0&&observed[6]==0x40);
 auto count=barriers;hw.sqWriteSendWqe(nullptr,0,0,0,0,0,0,0,0,nullptr,false);CHECK(barriers==count);
 VlanHardware vlan;UInt32 status=0;UInt8 bits[512]{};bits[0]=1;bits[4]=0x20;bits[511]=0x40;
 CHECK(vlan.setVlanFilter(2,bits,&status)&&!status&&vlan.commands==1&&vlan.port==2&&vlan.op==0x47&&vlan.opmod==0&&vlan.address==vlan.fMboxPhys);
 CHECK(vlan.inbox[511]==1&&vlan.inbox[507]==0x20&&vlan.inbox[0]==0x40&&vlan.inbox[1]==0);
 CHECK(vlan.setVlanFilter(1,nullptr,&status)&&vlan.inbox[0]==0xff&&vlan.inbox[511]==0xff&&vlan.inbox[512]==0);
 CHECK(!vlan.setVlanFilter(3,bits,&status)&&status==CX3_FWSTATUS_NOT_POSTED&&vlan.commands==2);
 vlan.allocated=false;CHECK(!vlan.setVlanFilter(1,bits,&status)&&vlan.commands==2&&status==CX3_FWSTATUS_NOT_POSTED);
 vlan.allocated=true;vlan.accepted=false;CHECK(!vlan.setVlanFilter(1,bits,&status)&&status==3&&vlan.commands==3);
 printf("VLAN WQE/filter: %u checks, %u failures (production encoder, synthetic memory/barriers)\n",checks,failures);
 return failures?1:0;
}
'''.replace('FILTER_METHOD',filter_body.replace('Mlx4Hw::','VlanHardware::')).replace('METHOD', body)
with tempfile.TemporaryDirectory(prefix='cx3-vlan-wqe-') as directory:
    exe = str(Path(directory) / 'test')
    subprocess.run(['/usr/bin/clang++', '-std=c++17', '-Wall', '-Wextra',
                    '-Wno-unused-parameter', '-I' + str(ROOT / 'src'),
                    '-x', 'c++', '-', '-o', exe], input=code, text=True, check=True)
    raise SystemExit(subprocess.run([exe], timeout=10).returncode)
