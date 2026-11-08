#!/usr/bin/env python3
"""Run real init and all command wrappers with lock allocation/refusal faults.

Break caught: a missing lock still posts or reads HCR, or a command entry
allocates/replaces a shared lock. Only IOKit allocation/maps and the HCR core
are substituted. This does not prove shutdown/callback drainage.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

SRC = Path(__file__).resolve().parents[1] / 'src'


def method(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}\n', start) + 3]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path)
    args = parser.parse_args()
    source = (args.source or SRC / 'Mlx4Hw.cpp').read_text()
    code = '\n'.join(method(source, sig) for sig in [
        'bool Mlx4Hw::init(', 'bool Mlx4Hw::cmdBox(',
        'bool Mlx4Hw::cmdBoxImm(', 'bool Mlx4Hw::cmdEvent('])
    harness = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
using UInt8=uint8_t;using UInt16=uint16_t;using UInt32=uint32_t;using UInt64=uint64_t;
#define CX3_HOST_TEST
#include "cx3_abi.h"
#include "mlx4_regs.h"
struct IOService {};
static unsigned allocs=0,frees=0,locks=0,unlocks=0,depth=0;
static bool failAlloc=false;
struct IOLock {};
static IOLock lock;
static IOLock *IOLockAlloc() { ++allocs;return failAlloc ? nullptr : &lock; }
static void IOLockFree(IOLock*) { ++frees; }
static void IOLockLock(IOLock*) { ++locks;++depth; }
static void IOLockUnlock(IOLock*) { ++unlocks;--depth; }
static void logSink(const char*,...) {}
#define LOG(...) logSink(__VA_ARGS__)
constexpr unsigned kIOPCIConfigBaseAddress0=0x10,kIOPCIConfigBaseAddress2=0x18,kIOMapAnywhere=0;
struct Map {
    UInt32 registers[0x110000/4]{};
    bool nullAddress=false;
    unsigned releases=0;
    void release() { ++releases; }
    uintptr_t getVirtualAddress() { return nullAddress ? 0 : reinterpret_cast<uintptr_t>(registers); }
};
static Map mapping;
struct IOPCIDevice {
    unsigned maps=0; bool failBar0=false;
    Map *mapDeviceMemoryWithRegister(unsigned reg,unsigned) {
        ++maps;return reg==kIOPCIConfigBaseAddress0 && failBar0 ? nullptr : &mapping;
    }
};
static UInt32 OSSwapBigToHostInt32(UInt32 n) { return __builtin_bswap32(n); }
class Mlx4Hw {
public:
    IOPCIDevice *fPci=nullptr;IOService *fClient=nullptr;
    Map *fMap0=nullptr,*fMap2=nullptr;
    volatile UInt32 *fReg0=nullptr,*fReg2=nullptr,*fHcr=nullptr,*fReset=nullptr;
    UInt32 fToggle=1; IOLock *fCmdLock=nullptr;
    unsigned calls=0,reads=0,unlockedCalls=0;
    bool coreOK=true;
    bool init(IOPCIDevice*,IOService*);
    bool cmdBox(UInt64,UInt64,UInt32,UInt8,UInt16,UInt32,UInt32*,bool,Cx3CommandObservation*);
    bool cmdBoxImm(UInt64,UInt64,UInt8,UInt16,UInt32,UInt64*,UInt32*,bool);
    bool cmdEvent(UInt64,UInt64,UInt32,UInt8,UInt16,UInt16,UInt32,UInt32*,UInt32*,UInt32*);
    bool cmdBoxLocked(UInt64,UInt64,UInt32,UInt8,UInt16,UInt32,UInt32 *s,bool,
                      UInt64 *imm,UInt16,bool,Cx3CommandObservation *o=nullptr) {
        ++calls;if(depth!=1)++unlockedCalls;
        if(s)*s=coreOK ? 0 : CX3_FWSTATUS_TIMEOUT;
        if(imm)*imm=0xabcdef;
        if(o) { memset(o,0,sizeof(*o));o->abiVersion=1;o->abiBytes=40;o->flags=1; }
        return coreOK;
    }
    UInt32 readRegRaw(UInt32) { ++reads;return 0; }
    UInt32 readHcrStatus() { ++reads;return 0; }
};
CODE
static unsigned cases=0,checks=0,failures=0;
#define CHECK(x) do { ++checks;if(!(x)) { ++failures;std::printf("FAIL case %u line %u: %s\n",cases,__LINE__,#x); } } while(0)
int main() {
    for(unsigned mode=0;mode<4;++mode) {
        ++cases;Mlx4Hw h;IOPCIDevice p;IOService s;
        allocs=frees=mapping.releases=0;failAlloc=mode==1;p.failBar0=mode==2;mapping.nullAddress=mode==3;
        bool ok=h.init(&p,&s);
        CHECK(ok==(mode==0));
        CHECK(allocs==1);
        CHECK(mode==1 ? p.maps==0 : p.maps>=1);
        CHECK(!ok || h.fCmdLock!=nullptr);
        CHECK(ok || h.fCmdLock==nullptr);
        CHECK(frees==unsigned(mode>=2));
        CHECK(mapping.releases==unsigned(mode==3));
        CHECK(ok || (h.fMap0==nullptr && h.fHcr==nullptr));
    }
    mapping.nullAddress=false;
    for(unsigned wrapper=0;wrapper<3;++wrapper) for(unsigned mode=0;mode<3;++mode) {
        ++cases;Mlx4Hw h;UInt32 status=0xdeadbeef,post=77,end=88;
        UInt64 imm=99;Cx3CommandObservation obs;memset(&obs,0xcc,sizeof(obs));
        h.fHcr=mapping.registers;h.fCmdLock=mode==0 ? nullptr : &lock;
        h.coreOK=mode!=2;allocs=locks=unlocks=depth=0;failAlloc=true;
        bool ok=wrapper==0 ? h.cmdBox(0,0,0,0,1,1,&status,false,&obs) :
                wrapper==1 ? h.cmdBoxImm(0,0,0,1,1,&imm,&status,false) :
                             h.cmdEvent(0,0,0,0,1,9,1,&status,&post,&end);
        CHECK(ok==(mode==1));CHECK(allocs==0);
        CHECK(h.calls==unsigned(mode!=0));CHECK(h.unlockedCalls==0);
        CHECK(locks==unsigned(mode!=0) && locks==unlocks && depth==0);
        CHECK(status==(mode==0 ? CX3_FWSTATUS_NOT_POSTED : mode==1 ? 0 : CX3_FWSTATUS_TIMEOUT));
        if(mode==0) {
            CHECK(h.reads==0);
            if(wrapper==0) { Cx3CommandObservation zero{};CHECK(!memcmp(&obs,&zero,sizeof(obs))); }
            if(wrapper==1) CHECK(imm==0);
            if(wrapper==2) CHECK(post==0 && end==0);
        }
        // Missing optional outputs must also refuse without touching HCR/core.
        if(mode==0) {
            CHECK(!h.cmdBox(0,0,0,0,1,1,nullptr,false,nullptr));
            CHECK(!h.cmdBoxImm(0,0,0,1,1,nullptr,nullptr,false));
            CHECK(!h.cmdEvent(0,0,0,0,1,9,1,nullptr,nullptr,nullptr));
            CHECK(h.calls==0 && h.reads==0 && allocs==0);
        }
    }
    std::printf("command lock: %u cases, %u checks, %u failures (production init/wrappers; synthetic IOKit/core)\n",cases,checks,failures);
    return failures ? 1 : 0;
}
'''.replace('CODE', code)
    with tempfile.TemporaryDirectory(prefix='cx3-command-lock-') as tmp:
        binary = str(Path(tmp) / 'test')
        subprocess.run(['/usr/bin/clang++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function',
                        '-I', str(SRC), '-x', 'c++', '-', '-o', binary],
                       input=harness, text=True, check=True)
        return subprocess.run([binary], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
