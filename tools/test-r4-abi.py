#!/usr/bin/env python3
"""Execute new user-client R4 size guards and rawjson with a synthetic transport.

Catches legacy/oversized buffers reaching device exports, and rawjson silently
truncating large captures. Does not call IOKit or the live driver.
"""
from pathlib import Path
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}\n', start) + 3]


def compile_run(tmp, name, source, args=()):
    binary = str(Path(tmp) / name)
    subprocess.run(['/usr/bin/clang++', '-std=c++11', '-Wall', '-Wextra',
                    '-Wno-unused-parameter', '-Wno-unused-const-variable',
                    '-Wno-unused-function', '-Werror', '-I', str(ROOT / 'src'),
                    '-x', 'c++', '-', '-o', binary], input=source, text=True, check=True)
    return subprocess.run([binary, *args], text=True, capture_output=True)


def main():
    client = (ROOT / 'src/Cx3Mlx4UserClient.cpp').read_text()
    tool = (ROOT / 'tools/cx3ctl.c').read_text()
    handlers = '\n'.join(method(client, 'IOReturn Cx3Mlx4UserClient::doR4' + s + '(')
                         for s in ['Qp', 'Ingress'])
    preamble = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <initializer_list>
#include "cx3_abi.h"
#include "mlx4_regs.h"
using UInt64=uint64_t; using IOReturn=int;
constexpr int kIOReturnNotAttached=-1, kIOReturnBadArgument=-2, kIOReturnSuccess=0;
constexpr int kIOReturnNoMemory=-3,kIOReturnError=-4;
static void logSink(const char*, ...) {}
#define LOG(...) logSink(__VA_ARGS__)
static bool failAllocation=false;
static unsigned liveAllocations=0;
static void *IOMalloc(size_t n) {
    if (failAllocation) return nullptr;
    void *p=std::malloc(n); if (p) ++liveAllocations; return p;
}
static void IOFree(void *p,size_t) { if (p) { --liveAllocations; std::free(p); } }
struct IOMemoryDescriptor {
    uint8_t *bytes; size_t length; bool shortWrite;
    size_t getLength() const { return length; }
    size_t writeBytes(size_t offset,const void *p,size_t size) {
        if (shortWrite) return 0;
        if (offset+size>length) return 0;
        memcpy(bytes+offset,p,size); return size;
    }
};
struct IOExternalMethodArguments {
    void *structureOutput; size_t structureOutputSize;
    unsigned scalarInputCount; UInt64 *scalarInput, *scalarOutput;
    IOMemoryDescriptor *structureOutputDescriptor=nullptr;
    uint32_t structureOutputDescriptorSize=0;
    IOExternalMethodArguments(void *p,size_t n,unsigned count,UInt64 *in,UInt64 *out)
        : structureOutput(p),structureOutputSize(n),scalarInputCount(count),
          scalarInput(in),scalarOutput(out) {}
};
struct Provider {
    unsigned calls=0;
    int exportR4Qp(const UInt64*, Cx3R4QpResult *r, UInt64*) {
        ++calls; memset(r,0,sizeof(*r)); return 0;
    }
    int exportR4Ingress(Cx3R4IngressResult *r, UInt64*) {
        ++calls; memset(r,0,sizeof(*r)); return 0;
    }
};
class Cx3Mlx4UserClient {
public:
    Provider *fProvider=nullptr;
    int doR4Qp(IOExternalMethodArguments*);
    int doR4Ingress(IOExternalMethodArguments*);
};
'''
    main_code = r'''
int main() {
    unsigned checks=0, failures=0;
    for (bool ingress : {false,true}) {
        const size_t need=ingress ? sizeof(Cx3R4IngressResult) : sizeof(Cx3R4QpResult);
        // Heap storage deliberately: these diagnostics are too large for a kernel stack.
        auto *buffer=static_cast<uint8_t*>(std::calloc(1,need+16));
        for (size_t bytes : {size_t(0),size_t(4096),need-1,need,need+8}) {
            Provider provider; Cx3Mlx4UserClient c; c.fProvider=&provider;
            UInt64 in[16]{},out[32]{};
            IOExternalMethodArguments a={buffer,bytes,
                unsigned(ingress ? kR4IngressInCount : kR4QpInCount),in,out};
            const int rc=ingress ? c.doR4Ingress(&a) : c.doR4Qp(&a);
            const bool ok=bytes==need ? (rc==0 && provider.calls==1) :
                                       (rc==kIOReturnBadArgument && provider.calls==0);
            ++checks; if (!ok) { ++failures; std::printf("FAIL ABI ingress=%u bytes=%zu calls=%u rc=%d\n",ingress,bytes,provider.calls,rc); }
            if (bytes==need && ingress) {
                const auto *r=reinterpret_cast<Cx3R4IngressResult*>(buffer);
                ++checks;
                if (r->abiVersion!=CX3_R4INGRESS_ABI || r->abiBytes!=need) ++failures;
            }
            if (bytes==need && !ingress) {
                ++checks;
#ifdef CX3_R4QP_ABI
                const auto *r=reinterpret_cast<Cx3R4QpResult*>(buffer);
                if (r->abiVersion!=CX3_R4QP_ABI || r->abiBytes!=need) ++failures;
#else
                ++failures;
#endif
            }
        }
        // Mirror XNU: out-of-line results have a descriptor and zero in-band size.
        for (unsigned scenario=0;scenario<6;++scenario) {
            Provider provider; Cx3Mlx4UserClient c; c.fProvider=&provider;
            UInt64 in[16]{},out[32]{};
            IOMemoryDescriptor md={buffer,need,false};
            IOExternalMethodArguments a(nullptr,0,
                unsigned(ingress ? kR4IngressInCount : kR4QpInCount),in,out);
            a.structureOutputDescriptor=&md;
            a.structureOutputDescriptorSize=uint32_t(need);
            if (scenario==1) --a.structureOutputDescriptorSize;
            if (scenario==2) --md.length;
            if (scenario==3) { a.structureOutput=buffer; a.structureOutputSize=need; }
            failAllocation=scenario==4;
            md.shortWrite=scenario==5;
            const int rc=ingress ? c.doR4Ingress(&a) : c.doR4Qp(&a);
            failAllocation=false;
            const bool ok=scenario==0 ? (rc==0 && provider.calls==1) :
                          scenario==4 ? (rc==kIOReturnNoMemory && provider.calls==0) :
                          scenario==5 ? (rc==kIOReturnError && provider.calls==1) :
                                        (rc==kIOReturnBadArgument && provider.calls==0);
            ++checks;
            if (!ok) { ++failures; std::printf("FAIL OOL ingress=%u scenario=%u calls=%u rc=%d\n",ingress,scenario,provider.calls,rc); }
            ++checks; if (liveAllocations) { ++failures; std::puts("FAIL leaked output allocation"); }
        }
        std::free(buffer);
        std::printf("size %s = %zu\n",ingress ? "ingress" : "qp",need);
    }
    std::printf("R4 client ABI: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
'''
    transport = r'''
using kern_return_t=int;
constexpr int KERN_SUCCESS=0,KERN_FAILURE=5;
static int gConn=0, openCalls=0, transportCalls=0;
static int openService() { ++openCalls; return 0; }
static void breadcrumb(const char*, ...) {}
static int IOConnectCallMethod(int, unsigned, const uint64_t*, uint32_t,
    const void*,size_t,uint64_t*,uint32_t*,void *p,size_t *size) {
    ++transportCalls; memset(p,0xab,*size); return 0;
}
'''
    rawjson = method(tool, 'static int cmdRawJson(')
    with tempfile.TemporaryDirectory(prefix='cx3-r4-abi-') as tmp:
        results = []
        if 'static void printQpCapture(' in tool:
            formatter = method(tool, 'static void printQpCapture(')
        else:
            formatter = 'static void printQpCapture(const char*, const Cx3QpCaptureObservation*) {}'
        capturemain = r'''
int main() {
    Cx3QpCaptureObservation o{};
    printQpCapture("unused",&o);
    o.abiVersion=1;o.abiBytes=64;o.preparedBytes=264;o.capturedBytes=264;
    o.fwStatus=CX3_FWSTATUS_NOT_POSTED;
    printQpCapture("prepared",&o);
    o.command.abiVersion=1;o.command.abiBytes=40;o.command.flags=7;
    o.command.fwStatus=3;o.command.startNs=100;o.command.postedNs=200;o.command.endNs=300;
    o.fwStatus=3;o.preparedBytes=4096;o.capturedBytes=0;
    printQpCapture("fw-error",&o);
    o.abiBytes=1;printQpCapture("bad-version",&o);
}
'''
        proc = compile_run(tmp, 'capture-print', preamble + formatter + capturemain)
        lines = proc.stdout.splitlines()
        printed = (proc.returncode==0 and len(lines)==4 and
                   'not called' in lines[0] and
                   'prepared=264 captured=264' in lines[1] and 'HCR not entered' in lines[1] and
                   'prepared=4096 captured=0' in lines[2] and 'flags=0x7' in lines[2] and
                   '100/200/300' in lines[2] and 'invalid capture ABI' in lines[3])
        print('capture formatter actual output:', 'PASS' if printed else 'FAIL')
        results.append(printed)
        output_helper = ROOT / 'src/Cx3R4Output.h'
        helper = output_helper.read_text() if output_helper.exists() else ''
        proc = compile_run(tmp, 'client', preamble + helper + handlers + main_code)
        print(proc.stdout, end=''); print(proc.stderr, end=''); results.append(proc.returncode==0)
        toolmain = r'''
int main(int argc,char **argv) {
    const int rc=cmdRawJson(argc,argv,1);
    std::fprintf(stderr,"opens=%d transport=%d\n",openCalls,transportCalls);
    return rc;
}
'''
        # Use production ABI to obtain current diagnostic length, not a copied constant.
        sizeprog = compile_run(tmp, 'size', preamble +
            'int main(){std::printf("%zu",sizeof(Cx3R4IngressResult));}')
        need = int(sizeprog.stdout)
        proc = compile_run(tmp, 'json', preamble + transport + rawjson + toolmain,
                           ['35', 'out', '16', 'struct', str(need)])
        data = json.loads(proc.stdout)
        ok = proc.returncode==0 and data['structLen']==need and data['structHex']=='ab'*need
        print(f'rawjson full capture ({need} bytes): {"PASS" if ok else "FAIL"}')
        results.append(ok)
        proc = subprocess.run([str(Path(tmp)/'json'),'35','struct','999999'],
                              capture_output=True,text=True)
        ok=proc.returncode==2 and 'opens=0 transport=0' in proc.stderr
        print(f'rawjson oversize rejected before transport: {"PASS" if ok else "FAIL"}')
        results.append(ok)
        return 0 if all(results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
