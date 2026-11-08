#!/usr/bin/env python3
"""Execute production mailbox allocation/free/shutdown and QP consumers.

Regression: poisoned or shutdown-retained mailboxes were returned as usable,
allowing writes before cmdBox rejected the command. Only IOKit object creation,
DMA mapping, BAR release and HCR transport are replaced. No hardware is opened.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile

SRC = Path(__file__).resolve().parents[1] / 'src'


def method(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}\n', start) + 3]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path)
    parser.add_argument('--controller-source', type=Path)
    args = parser.parse_args()
    source = (args.source or SRC / 'Mlx4Hw.cpp').read_text()
    methods = '\n'.join(method(source, s) for s in [
        'bool Mlx4Hw::mboxAlloc(', 'void Mlx4Hw::mboxFree(',
        'void Mlx4Hw::shutdown(', 'bool Mlx4Hw::qpModify(',
        'bool Mlx4Hw::qpQuery(', 'UInt32 Mlx4Hw::qpState(',
        'void Mlx4Hw::cmdPoison(', 'void Mlx4Hw::cmdPoisonInjected(',
        'void Mlx4Hw::cmdClearWedged(', 'bool Mlx4Hw::resetHca('])
    if 'bool Mlx4Hw::mailboxNeedsRetention() const' in source:
        methods += method(source, 'bool Mlx4Hw::mailboxNeedsRetention() const')
    else:
        shutdown = method(source, 'void Mlx4Hw::shutdown(')
        start = shutdown.index('mailbox.init(') + len('mailbox.init(')
        expression = shutdown[start:shutdown.index(');', start)]
        methods += 'bool Mlx4Hw::mailboxNeedsRetention() const { return ' + expression + '; }\n'
    controller = (args.controller_source or SRC / 'Cx3Mlx4.cpp').read_text()
    methods += method(controller, 'IOReturn Cx3Mlx4::exportFault(')
    harness = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
using UInt8=uint8_t; using UInt16=uint16_t; using UInt32=uint32_t; using UInt64=uint64_t;
using IOReturn=int;
#include "cx3_abi.h"
#include "mlx4_regs.h"
#define CX3_HOST_TEST
#include "mlx4_pure.h"
static void logSink(const char*,...) {}
#define LOG(...) logSink(__VA_ARGS__)
static UInt32 mlx4GetBe32(const void *p) {
    const auto *b=static_cast<const UInt8*>(p);
    return UInt32(b[0])<<24 | UInt32(b[1])<<16 | UInt32(b[2])<<8 | b[3];
}
static void mlx4PutBe32(void *p,UInt32 v) {
    auto *b=static_cast<UInt8*>(p);
    for (unsigned i=0;i<4;++i) b[i]=UInt8(v>>(24-i*8));
}
constexpr int kIOReturnSuccess=0, kernel_task=1,kIODirectionInOut=2,
    kIOMemoryKernelUserShared=4,kIODMACommandOutputHost64=8;
constexpr int kIOReturnBadArgument=-1,kIOReturnNotPermitted=-2,kIOReturnNotReady=-3;
static UInt64 nowNs=1000000;
static UInt64 cx3NowNs() { return ++nowNs; }
static void clock_get_uptime(UInt64 *n) { *n=++nowNs; }
static void IOSleep(UInt32 n) { nowNs+=UInt64(n)*1000000; }
static unsigned createMd=0,createCmd=0,releaseMd=0,releaseCmd=0,clearCmd=0;
static unsigned fault=0;
struct IOBufferMemoryDescriptor {
    UInt8 bytes[4096];
    static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(int,int,unsigned,UInt64);
    void *getBytesNoCopy() { return fault==2 ? nullptr : bytes; }
    void release() { ++releaseMd; }
};
static IOBufferMemoryDescriptor md;
IOBufferMemoryDescriptor *IOBufferMemoryDescriptor::inTaskWithPhysicalMask(int,int,unsigned,UInt64) {
    ++createMd; return fault==1 ? nullptr : &md;
}
struct IODMACommand {
    enum { kMapped=1 };
    struct Segment64 { UInt64 fIOVMAddr,fLength; };
    static IODMACommand *withSpecification(int,int,int,int,unsigned,int);
    int setMemoryDescriptor(IOBufferMemoryDescriptor*,bool) { return fault==4 ? -1 : 0; }
    int gen64IOVMSegments(UInt64 *offset,Segment64 *s,UInt32 *count) {
        *offset=fault==7 ? 0 : 4096; *count=fault==6 ? 2 : 1;
        s->fIOVMAddr=fault==9 ? 0x100000000ULL : (fault==10 ? 0x1234001 : 0x1234000);
        s->fLength=fault==8 ? 2048 : 4096; return fault==5 ? -1 : 0;
    }
    void clearMemoryDescriptor(bool) { ++clearCmd; }
    void release() { ++releaseCmd; }
};
static IODMACommand dma;
IODMACommand *IODMACommand::withSpecification(int,int,int,int,unsigned,int) {
    ++createCmd; return fault==3 ? nullptr : &dma;
}
struct Mlx4Icm {};
struct Map { void release() {} };
static void IOLockFree(void*) {}
constexpr unsigned kIOPCIConfigVendorID=0,kIOPCIConfigCommand=4;
struct Pci {
    bool missing=false; unsigned writes=0;
    UInt32 configRead32(unsigned) { return 0; }
    UInt16 configRead16(unsigned) { return missing ? 0xffff : 0x15b3; }
    void configWrite32(unsigned,UInt32) { ++writes; }
    void configWrite16(unsigned,UInt16) { ++writes; }
};
class Mlx4Hw {
public:
    IOBufferMemoryDescriptor *fMboxMd=nullptr;
    IODMACommand *fMboxCmd=nullptr;
    Mlx4Icm *fModuleInbox=nullptr;
    void icmFree(Mlx4Icm*) {}
    void *fMboxVirt=nullptr,*fCmdLock=nullptr;
    UInt64 fMboxPhys=0,fMboxOffset=0;
    UInt32 fMboxSegCount=0; bool fCmdWedged=false,fCmdWedgedPosted=false,fMboxRetained=false;
    UInt16 fCmdWedgedOp=0;
    bool fCmdSyntheticPoison=false,registerBusy=false;
    UInt64 fCmdWedgedAtNs=0; UInt32 fCmdAbandons=0,fCmdRefused=0;
    Map *fMap0=nullptr,*fMap2=nullptr;
    void *fReg0=nullptr,*fReg2=nullptr,*fHcr=nullptr;
    UInt32 resetWords[8]{}; UInt32 *fReset=resetWords;
    Pci pci; Pci *fPci=&pci; unsigned fToggle=1; bool semBusy=false;
    unsigned posts=0;
    bool mboxAlloc(); void mboxFree(); void shutdown();
    bool mailboxNeedsRetention() const;
    bool qpModify(UInt16,UInt32,const UInt8*,UInt32*,UInt8* =nullptr,UInt32=0 OBS_DECL);
    bool qpQuery(UInt32,UInt8*,UInt32,UInt32*,UInt8* =nullptr,UInt32=0 OBS_DECL);
    UInt32 qpState(const UInt8*);
    void cmdPoison(UInt16,const char*,UInt32,UInt64,bool);
    void cmdPoisonInjected(); void cmdClearWedged(const char*);
    bool resetHca(bool);
    UInt8 findPcieCapability() { return 0x40; }
    UInt32 readBar0Fresh(UInt32) { return semBusy ? 1 : 0; }
    UInt32 readResetSem() { return semBusy ? 1 : 0; }
    bool cmdRegisterBusy() { return registerBusy; }
    UInt32 readHcrStatus() { return registerBusy ? 0x800000 : 0; }
    bool cmdWedged() const { return fCmdWedged; }
    UInt16 cmdWedgedOp() const { return fCmdWedgedOp; }
    UInt32 cmdAbandons() const { return fCmdAbandons; }
    UInt32 cmdRefused() const { return fCmdRefused; }
    bool cmdBox(UInt64,UInt64,UInt32,UInt8,UInt16,UInt32,UInt32 *status,
                bool bypassBusy=false,Cx3CommandObservation* =nullptr) {
        if (bypassBusy) return false;
        ++posts; *status=0; return true;
    }
};
class Cx3Mlx4 {
public:
    Mlx4Hw fHw;
    IOReturn exportFault(const UInt64*,UInt64*);
};
METHODS
static unsigned checks=0,failures=0,cases=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %s line %u: %s\n",name,__LINE__,#x); } } while(0)
static void reset() { fault=0;createMd=createCmd=releaseMd=releaseCmd=clearCmd=0; memset(md.bytes,0x5a,sizeof(md.bytes)); }
static void existing(Mlx4Hw &h) {
    h.fMboxMd=&md; h.fMboxCmd=&dma; h.fMboxVirt=md.bytes;
    h.fMboxPhys=0x1234000; h.fMboxOffset=4096; h.fMboxSegCount=1;
}
int main() {
    for (unsigned state=0;state<16;++state) {
        const char *name="mailbox retention decision matches real shutdown"; ++cases;
        reset(); Mlx4Hw h;
        h.fMboxMd=(state&1) ? &md : nullptr;
        h.fMboxCmd=(state&2) ? &dma : nullptr;
        h.fCmdWedged=(state&4)!=0; h.fMboxRetained=(state&8)!=0;
        const bool mustRetain=(state&8) || ((state&4) && (state&3));
        CHECK(h.mailboxNeedsRetention()==mustRetain);
        h.shutdown();
        CHECK(h.fMboxRetained==mustRetain);
        CHECK(releaseMd==(mustRetain ? 0u : unsigned((state&1)!=0)));
        CHECK(releaseCmd==(mustRetain ? 0u : unsigned((state&2)!=0)));
        CHECK(clearCmd==releaseCmd);
        CHECK(h.fMboxMd==(mustRetain && (state&1) ? &md : nullptr));
        CHECK(h.fMboxCmd==(mustRetain && (state&2) ? &dma : nullptr));
        CHECK(h.mailboxNeedsRetention()==mustRetain);
    }
    {
        const char *name="normal allocate/reuse/free"; ++cases; reset(); Mlx4Hw h;
        CHECK(h.mboxAlloc()); CHECK(createMd==1 && createCmd==1);
        CHECK(h.fMboxMd==&md && h.fMboxCmd==&dma && h.fMboxVirt==md.bytes);
        CHECK(h.mboxAlloc()); CHECK(createMd==1 && createCmd==1);
        h.shutdown(); CHECK(releaseMd==1 && releaseCmd==1 && clearCmd==1);
        CHECK(!h.fMboxMd && !h.fMboxCmd && !h.fMboxVirt);
        h.shutdown(); CHECK(releaseMd==1 && releaseCmd==1);
    }
    for (unsigned f=1;f<=10;++f) {
        const char *name="fresh allocation failure"; ++cases; reset(); fault=f; Mlx4Hw h;
        CHECK(!h.mboxAlloc()); CHECK(!h.fMboxMd && !h.fMboxCmd && !h.fMboxVirt);
        CHECK(h.posts==0); CHECK(releaseMd==(f==1 ? 0u : 1u));
        CHECK(releaseCmd==(f<=3 ? 0u : 1u));
    }
    for (bool posted : {false,true}) {
        const char *name="poison refusal before QP writes"; ++cases; reset(); Mlx4Hw h; existing(h);
        h.fCmdWedged=true; h.fCmdWedgedPosted=posted;
        CHECK(!h.mboxAlloc());
        UInt8 context[248]{}; UInt32 status=0;
        CHECK(!h.qpModify(MLX4_CMD_RST2INIT_QP,65,context,&status));
        CHECK(status==CX3_FWSTATUS_NOT_POSTED);
        CHECK(!h.qpQuery(65,context,248,&status));
        CHECK(status==CX3_FWSTATUS_NOT_POSTED);
        CHECK(h.posts==0 && createMd==0 && createCmd==0);
        for (unsigned i=0;i<4096;++i) CHECK(md.bytes[i]==0x5a);
        CHECK(releaseMd==0 && releaseCmd==0 && clearCmd==0);
        h.shutdown(); CHECK(!h.fMboxVirt && h.fMboxMd==&md && h.fMboxCmd==&dma);
        CHECK(!h.mboxAlloc());
        // Merely clearing command poison after a reset must not revive a discarded view.
        h.fCmdWedged=false; CHECK(!h.mboxAlloc());
        h.shutdown(); h.mboxFree();
        CHECK(h.fMboxMd==&md && h.fMboxCmd==&dma);
        CHECK(releaseMd==0 && releaseCmd==0 && clearCmd==0);
    }
    for (unsigned missing=0;missing<5;++missing) {
        const char *name="incomplete retained mapping"; ++cases; reset(); Mlx4Hw h; existing(h);
        if (missing==0) h.fMboxVirt=nullptr;
        if (missing==1) h.fMboxCmd=nullptr;
        if (missing==2) h.fMboxOffset=0;
        if (missing==3) h.fMboxSegCount=0;
        if (missing==4) h.fMboxMd=nullptr;
        CHECK(!h.mboxAlloc());
        CHECK(createMd==0 && createCmd==0 && releaseMd==0 && releaseCmd==0);
    }
    {
        const char *name="poison without mailbox"; ++cases; reset(); Mlx4Hw h; h.fCmdWedged=true;
        CHECK(!h.mboxAlloc()); CHECK(createMd==0 && createCmd==0);
    }
    {
        const char *name="public free on poisoned command"; ++cases; reset(); Mlx4Hw h; existing(h);
        h.fCmdWedged=true; h.mboxFree();
        CHECK(releaseMd==0 && releaseCmd==0 && clearCmd==0);
        CHECK(h.fMboxMd==&md && h.fMboxCmd==&dma);
        h.fCmdWedged=false; CHECK(!h.mboxAlloc());
        h.mboxFree(); CHECK(releaseMd==0 && releaseCmd==0);
    }
    for (unsigned variant=0;variant<7;++variant) {
        const char *name="real abandonment cannot be unwedge-cleared"; ++cases; reset(); Cx3Mlx4 d;
        existing(d.fHw); d.fHw.fCmdRefused=7;
        // A caller-chosen opcode may equal the injection marker: it is not provenance.
        UInt16 op=variant==2 ? CX3_FAULT_OP_MARKER : MLX4_CMD_QUERY_QP;
        d.fHw.cmdPoison(op,"test timeout",1,2,variant!=1);
        auto stamp=d.fHw.fCmdWedgedAtNs;
        if (variant==3) d.fHw.cmdPoisonInjected();
        if (variant==4) d.fHw.fMboxRetained=true;
        if (variant==5) d.fHw.registerBusy=true;
        if (variant==6) {
            d.fHw.cmdClearWedged("reset without an epoch credential");
        } else {
            UInt64 in[kFaultInCount]{},out[kFaultOutCount]{};
            in[kFaultInMode]=kFaultModeUnwedge; in[kFaultInArg]=1;
            CHECK(d.exportFault(in,out)==kIOReturnSuccess);
            CHECK(out[kFaultOutStatus]!=0 && out[kFaultOutWedged]==1);
        }
        CHECK(d.fHw.fCmdWedged && !d.fHw.fCmdSyntheticPoison);
        CHECK(d.fHw.fCmdWedgedOp==op && d.fHw.fCmdWedgedAtNs==stamp);
        CHECK(d.fHw.fCmdAbandons==1 && d.fHw.fCmdRefused==7);
        CHECK(!d.fHw.mboxAlloc());
        CHECK(createMd==0 && createCmd==0 && releaseMd==0 && releaseCmd==0);
        CHECK(md.bytes[0]==0x5a && md.bytes[4095]==0x5a);
        d.fHw.shutdown();
        CHECK(d.fHw.fMboxRetained && d.fHw.fMboxMd==&md && d.fHw.fMboxCmd==&dma);
        CHECK(releaseMd==0 && releaseCmd==0 && clearCmd==0);
    }
    for (unsigned variant=0;variant<6;++variant) {
        const char *name="synthetic poison provenance and refusal"; ++cases; reset(); Cx3Mlx4 d;
        existing(d.fHw);
        UInt64 in[kFaultInCount]{},out[kFaultOutCount]{};
        in[kFaultInMode]=kFaultModeWedge;
        CHECK(d.exportFault(in,out)==kIOReturnSuccess && out[kFaultOutStatus]==0);
        CHECK(d.fHw.fCmdWedged && d.fHw.fCmdSyntheticPoison);
        if (variant==1) d.fHw.registerBusy=true;
        if (variant==2) d.fHw.fMboxRetained=true;
        if (variant==3) d.fHw.cmdPoison(MLX4_CMD_QUERY_QP,"late real failure",1,2,true);
        if (variant==5) d.fHw.fCmdWedgedPosted=true;
        in[kFaultInMode]=kFaultModeUnwedge; in[kFaultInArg]=variant==4 ? 0 : 1;
        CHECK(d.exportFault(in,out)==kIOReturnSuccess);
        bool allowed=variant==0;
        CHECK((out[kFaultOutStatus]==0)==allowed);
        CHECK(d.fHw.fCmdWedged==!allowed);
        if (allowed) {
            CHECK(!d.fHw.fCmdSyntheticPoison && d.fHw.fCmdWedgedOp==0 && d.fHw.fCmdWedgedAtNs==0);
            CHECK(d.fHw.mboxAlloc());
            d.fHw.shutdown(); CHECK(releaseMd==1 && releaseCmd==1 && clearCmd==1);
        } else {
            CHECK(!d.fHw.mboxAlloc());
            d.fHw.shutdown(); CHECK(releaseMd==0 && releaseCmd==0 && clearCmd==0);
        }
    }
    for (unsigned poison=0;poison<4;++poison) for (unsigned result=0;result<3;++result) {
        const char *name="reset outcome is not mailbox recovery proof"; ++cases; reset(); Mlx4Hw h;
        existing(h);
        if (poison==1) h.cmdPoisonInjected();
        if (poison>=2) h.cmdPoison(MLX4_CMD_QUERY_QP,"real timeout",1,2,poison==3);
        bool wedged=h.fCmdWedged,synthetic=h.fCmdSyntheticPoison,posted=h.fCmdWedgedPosted;
        auto op=h.fCmdWedgedOp; auto stamp=h.fCmdWedgedAtNs;
        h.semBusy=result==1; h.pci.missing=result==2;
        CHECK(h.resetHca(false)==(result==0));
        CHECK(h.fCmdWedged==wedged && h.fCmdSyntheticPoison==synthetic);
        CHECK(h.fCmdWedgedPosted==posted && h.fCmdWedgedOp==op && h.fCmdWedgedAtNs==stamp);
        CHECK(h.fMboxMd==&md && h.fMboxCmd==&dma && h.fMboxVirt==md.bytes);
        CHECK(md.bytes[0]==0x5a && md.bytes[4095]==0x5a);
        CHECK(h.mboxAlloc()==!wedged);
        CHECK(releaseMd==0 && releaseCmd==0 && clearCmd==0);
    }
    std::printf("mailbox lifetime: %u cases, %u checks, %u failures (synthetic IOKit/HCR)\n",cases,checks,failures);
    return failures ? 1 : 0;
}
'''.replace('METHODS',methods)
    harness = harness.replace('OBS_DECL', ', Cx3QpCaptureObservation* =nullptr'
                              if 'Cx3QpCaptureObservation *outObservation' in methods else '')
    with tempfile.TemporaryDirectory(prefix='cx3-mailbox-life-') as tmp:
        binary=str(Path(tmp)/'test')
        subprocess.run(['/usr/bin/clang++','-std=c++11','-Wall','-Wextra','-Werror',
                        '-I',str(SRC),'-x','c++','-','-o',binary],input=harness,text=True,check=True)
        return subprocess.run([binary]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
