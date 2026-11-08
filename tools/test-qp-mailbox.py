#!/usr/bin/env python3
"""Execute production QP mailbox methods. Only allocator/HCR are substituted.

Catches truncated captures, stale failed-query bytes, capture overruns and copying
after the HCR has reused the mailbox. Does not simulate firmware or DMA safety.
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
    parser.add_argument('--source', type=Path, default=SRC / 'Mlx4Hw.cpp')
    args = parser.parse_args()
    text = args.source.read_text()
    state_source = (SRC / 'Mlx4Hw.cpp').read_text()
    methods = '\n'.join(method(text, s) for s in
                        ['bool Mlx4Hw::qpModify(', 'bool Mlx4Hw::qpQuery('])
    methods += '\n' + method(state_source, 'UInt32 Mlx4Hw::qpState(')
    observed = 'Cx3QpCaptureObservation *outObservation' in methods
    harness = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
using UInt8=uint8_t; using UInt16=uint16_t; using UInt32=uint32_t; using UInt64=uint64_t;
#include "cx3_abi.h"
#include "mlx4_regs.h"
#ifndef CX3_QP_CAPTURE_OBSERVATION_ABI
// Test-first contract for old source replay. Not a replacement production method.
#define CX3_QP_CAPTURE_OBSERVATION_ABI 1u
struct Cx3QpCaptureObservation {
    uint32_t abiVersion, abiBytes, preparedBytes, capturedBytes, fwStatus, reserved;
    Cx3CommandObservation command;
};
#endif
static void logSink(const char*, ...) {}
#define LOG(...) logSink(__VA_ARGS__)
static UInt32 mlx4GetBe32(const void *p) {
    const auto *b=static_cast<const UInt8*>(p);
    return UInt32(b[0])<<24 | UInt32(b[1])<<16 | UInt32(b[2])<<8 | b[3];
}
static void mlx4PutBe32(void *p, UInt32 v) {
    auto *b=static_cast<UInt8*>(p);
    for (unsigned i=0;i<4;++i) b[i]=UInt8(v >> (24-i*8));
}
class Mlx4Hw {
public:
    UInt8 mailbox[4096]{}; void *fMboxVirt=mailbox; UInt64 fMboxPhys=0x1234000;
    bool allocOk=true, commandOk=true; UInt32 result=0, calls=0, allocations=0;
    UInt8 atPost[4096]{};
    bool mboxAlloc() { ++allocations; return allocOk; }
    bool cmdBox(UInt64 in, UInt64 out, UInt32 qpn, UInt8 mod, UInt16 op,
                UInt32, UInt32 *status, bool bypassBusy=false,
                Cx3CommandObservation *observation=nullptr) {
        if (bypassBusy) return false; // Ordinary QP paths must never bypass busy.
        ++calls;
        if (qpn!=65 || mod!=0 ||
            (op==MLX4_CMD_QUERY_QP ? (in!=0 || out!=fMboxPhys) :
                                   (in!=fMboxPhys || out!=0))) return false;
        memcpy(atPost, mailbox, sizeof(mailbox));
        // Reuse/overwrite the whole mailbox after posting: an outbound capture must
        // already have happened; a successful query must copy this response instead.
        for (unsigned i=0;i<sizeof(mailbox);++i) mailbox[i]=UInt8(i*7+3);
        *status=result;
        if (observation) {
            memset(observation,0,sizeof(*observation));
            observation->abiVersion=CX3_COMMAND_OBSERVATION_ABI;
            observation->abiBytes=sizeof(*observation);
            observation->flags=result==CX3_FWSTATUS_TIMEOUT ? 3u : 7u;
            observation->fwStatus=result;
            observation->startNs=100; observation->postedNs=200; observation->endNs=300;
        }
        return commandOk;
    }
    bool qpModify(UInt16, UInt32, const UInt8*, UInt32*, UInt8*=nullptr, UInt32=0 OBS_DECL);
    bool qpQuery(UInt32, UInt8*, UInt32, UInt32*, UInt8*=nullptr, UInt32=0 OBS_DECL);
    UInt32 qpState(const UInt8*);
};
METHODS
static bool observedModify(Mlx4Hw &h, const UInt8 *context, UInt32 *status,
                           UInt8 *bytes, UInt32 size, Cx3QpCaptureObservation *capture) {
    (void)capture;
    return h.qpModify(MLX4_CMD_RST2INIT_QP,65,context,status,bytes,size OBS_ARG);
}
static bool observedQuery(Mlx4Hw &h, UInt8 *context, UInt32 *status,
                          UInt8 *bytes, UInt32 size, Cx3QpCaptureObservation *capture) {
    (void)capture;
    return h.qpQuery(65,context,248,status,bytes,size OBS_ARG);
}
static unsigned checks=0, failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %u: %s\n",__LINE__,#x); } } while(0)
int main() {
    UInt8 context[256]; for (unsigned i=0;i<256;++i) context[i]=UInt8(i^0xa5);
    struct Guarded { UInt8 pre[8], bytes[4104], post[8]; } capture;
    UInt32 status;
    // Malformed capture requests must fail before allocation or command posting.
    for (bool query : {false,true}) {
        for (unsigned size : {0u,1u,263u, query ? 4095u : 263u}) {
            Mlx4Hw h; memset(&capture,0xcc,sizeof(capture)); UInt8 q[256]{};
            bool ok=query ? h.qpQuery(65,q,248,&status,capture.bytes,size) :
                            h.qpModify(MLX4_CMD_RST2INIT_QP,65,context,&status,capture.bytes,size);
            CHECK(!ok); CHECK(h.calls==0); CHECK(h.allocations==0);
            CHECK(status==CX3_FWSTATUS_NOT_POSTED);
        }
    }
    for (unsigned outcome=0;outcome<4;++outcome) {
        Mlx4Hw h; memset(&capture,0xcc,sizeof(capture));
        h.allocOk=outcome!=1; h.commandOk=outcome==0;
        h.result=outcome==2 ? CX3_FWSTATUS_TIMEOUT : 3;
        bool ok=h.qpModify(MLX4_CMD_RST2INIT_QP,65,context,&status,capture.bytes,264);
        CHECK(ok==(outcome==0)); CHECK(capture.pre[0]==0xcc && capture.bytes[264]==0xcc);
        if (outcome==1) { for (unsigned i=0;i<264;++i) CHECK(capture.bytes[i]==0); }
        else {
            CHECK(memcmp(capture.bytes,h.atPost,264)==0);
            CHECK(memcmp(capture.bytes+8,context,248)==0);
            for (unsigned i=256;i<264;++i) CHECK(capture.bytes[i]==0);
            CHECK(mlx4GetBe32(capture.bytes)==QP_OPTPAR_DEFAULT);
            CHECK(memcmp(capture.bytes,h.mailbox,264)!=0);
        }
    }
    for (unsigned outcome=0;outcome<4;++outcome) {
        Mlx4Hw h; memset(&capture,0xcc,sizeof(capture)); UInt8 q[256]; memset(q,0xee,sizeof(q));
        h.allocOk=outcome!=1; h.commandOk=outcome==0;
        h.result=outcome==2 ? CX3_FWSTATUS_TIMEOUT : 3;
        bool ok=h.qpQuery(65,q,248,&status,capture.bytes,4096);
        CHECK(ok==(outcome==0)); CHECK(capture.pre[0]==0xcc && capture.bytes[4096]==0xcc);
        if (outcome==0) {
            CHECK(memcmp(capture.bytes,h.mailbox,4096)==0);
            CHECK(memcmp(q,h.mailbox+8,248)==0);
            CHECK(capture.bytes[4095]==UInt8(4095*7+3));
        } else {
            for (unsigned i=0;i<4096;++i) CHECK(capture.bytes[i]==0);
            for (unsigned i=0;i<248;++i) CHECK(q[i]==0);
        }
        CHECK(q[248]==0xee && q[255]==0xee);
    }
    // Oversized destination capacity is not a request to copy unrelated bytes.
    Mlx4Hw h; memset(&capture,0xcc,sizeof(capture));
    CHECK(h.qpModify(MLX4_CMD_RST2INIT_QP,65,context,&status,capture.bytes,4104));
    CHECK(capture.bytes[264]==0xcc && capture.bytes[4096]==0xcc);
    // Observation must survive every return path and be reset when reused.
    // cmdBox is a recording transport here; real HCR flag/timing correctness is
    // separately exercised by test-command-observation.py.
    for (bool query : {false,true}) {
        for (unsigned outcome=0;outcome<6;++outcome) {
            Mlx4Hw hw; UInt8 q[248]{}; UInt8 bytes[4096]{};
            Cx3QpCaptureObservation observation; memset(&observation,0xcc,sizeof(observation));
            hw.allocOk=outcome!=1;
            hw.commandOk=outcome==0 || outcome==4 || outcome==5;
            hw.result=outcome==2 ? CX3_FWSTATUS_TIMEOUT : outcome==3 ? 3u : 0u;
            UInt32 capacity=outcome==5 ? 1u : query ? 4096u : 264u;
            UInt8 *destination=outcome==4 ? nullptr : bytes;
            if (outcome==4) capacity=0;
            bool ok=query ? observedQuery(hw,q,&status,destination,capacity,&observation) :
                            observedModify(hw,context,&status,destination,capacity,&observation);
            CHECK(ok==(outcome==0 || outcome==4));
            CHECK(observation.abiVersion==1); CHECK(observation.abiBytes==64);
            CHECK(observation.reserved==0);
            const bool prepared=outcome!=1 && outcome!=5;
            CHECK(observation.preparedBytes==(prepared ? (query ? 4096u : 264u) : 0u));
            CHECK(observation.capturedBytes==
                  (prepared && outcome!=4 && (!query || ok) ? (query ? 4096u : 264u) : 0u));
            CHECK(observation.fwStatus==status);
            if (!prepared) {
                CHECK(observation.command.flags==0);
                CHECK(observation.command.startNs==0 && observation.command.endNs==0);
                CHECK(hw.calls==0);
            } else {
                CHECK(observation.command.flags==(outcome==2 ? 3u : 7u));
                CHECK(observation.command.fwStatus==status);
                CHECK(observation.command.startNs==100 && observation.command.postedNs==200 &&
                      observation.command.endNs==300);
            }
            // A malformed request must erase the previous command's evidence.
            if (query) observedQuery(hw,q,nullptr,bytes,1,&observation);
            else observedModify(hw,nullptr,nullptr,bytes,264,&observation);
            CHECK(observation.preparedBytes==0 && observation.capturedBytes==0);
            CHECK(observation.command.flags==0 && observation.command.postedNs==0);
            CHECK(observation.fwStatus==CX3_FWSTATUS_NOT_POSTED);
        }
    }
    std::printf("QP mailbox: %u checks, %u failures (synthetic HCR only)\n",checks,failures);
    return failures ? 1 : 0;
}
'''.replace('METHODS', methods).replace('UInt8*=', 'UInt8* =')
    harness = harness.replace('OBS_DECL', ', Cx3QpCaptureObservation* =nullptr' if observed else '')
    harness = harness.replace('OBS_ARG', ', capture' if observed else '')
    harness = harness.replace('#include <cstring>', '#include <cstring>\n#include <initializer_list>')
    with tempfile.TemporaryDirectory(prefix='cx3-qp-mailbox-') as tmp:
        binary = str(Path(tmp) / 'test')
        subprocess.run(['/usr/bin/clang++', '-std=c++11', '-Wall', '-Wextra',
                        '-Werror', '-I', str(SRC), '-x', 'c++', '-', '-o', binary],
                       input=harness, text=True, check=True)
        return subprocess.run([binary]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
