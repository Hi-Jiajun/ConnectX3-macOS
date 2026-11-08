#!/usr/bin/env python3
"""Exercise the production QUERY_ADAPTER raw export and user-client handler.

Named breaks: truncating the mailbox copy, returning uninitialised ABI bytes,
querying after a safety refusal, accepting a non-exact structure size, or
introducing interrupt/reset/port side effects. The initial missing-endpoint
stub makes RED a behavioral assertion rather than a missing-symbol compile.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
abi = (ROOT / 'src/cx3_abi.h').read_text()
provider = (ROOT / 'src/Cx3Mlx4.cpp').read_text()
client = (ROOT / 'src/Cx3Mlx4UserClient.cpp').read_text()
cli = (ROOT / 'tools/cx3ctl.c').read_text()

fallback_abi = r'''
struct Cx3AdapterResult {
    uint32_t version, structBytes; uint64_t status; uint32_t fwStatus, rawBytes;
    uint64_t deviceGeneration; uint32_t intaPin, reserved;
    uint8_t boardId[64], raw[256];
};
enum { CX3_ADAPTER_RESULT_VERSION = 1, CX3_ADAPTER_RAW_BYTES = 256 };
'''
abi_def = '' if 'struct Cx3AdapterResult' in abi else fallback_abi

def method(text, signature, fallback=''):
    if signature not in text:
        return fallback
    start = text.index(signature)
    return text[start:text.index('\n}\n', start) + 3]

missing_export = r'''
IOReturn Cx3Mlx4::exportQueryAdapter(Cx3AdapterResult *r) {
    memset(r, 0, sizeof(*r)); return kIOReturnUnsupported;
}
'''
export = method(provider, 'IOReturn Cx3Mlx4::exportQueryAdapter(', missing_export)
missing_do = r'''
IOReturn Cx3Mlx4UserClient::doQueryAdapter(IOExternalMethodArguments *a) {
    (void)a; return kIOReturnUnsupported;
}
'''
do = method(client, 'IOReturn Cx3Mlx4UserClient::doQueryAdapter(', missing_do)

code = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#define CX3_HOST_TEST
#include "mlx4_pure.h"
#include "cx3_abi.h"
__ABI__
using IOReturn=int;
enum { kIOReturnSuccess=0, kIOReturnError=-1, kIOReturnNotPermitted=-2,
       kIOReturnOffline=-3, kIOReturnBadArgument=-4, kIOReturnNotAttached=-5,
       kIOReturnUnsupported=-6 };
static void sink(const char*,...) __attribute__((unused));
static void sink(const char*,...) {}
#define LOG(...) sink(__VA_ARGS__)
class Mlx4Hw {
public:
    unsigned ownerCalls=0, aliveCalls=0, queryCalls=0;
    bool alive=true, queryOk=true;
    UInt32 owner=0;
    UInt32 fw=0;
    UInt8 pin=0x3f, board[64], raw[256];
    UInt32 checkOwnershipAsReference() { ++ownerCalls; return owner; }
    bool windowAlive() { ++aliveCalls; return alive; }
    bool queryAdapter(UInt8 *p, UInt8 *b, UInt32 *f, UInt8 *r) {
        ++queryCalls; *f=fw; if (!queryOk) return false;
        *p=pin; memcpy(b,board,64); memcpy(r,raw,256); return true;
    }
};
class Cx3Mlx4 {
public:
    Mlx4Hw fHw; UInt64 fDeviceGeneration=0x1122334455667788ull;
    bool fResetCommandsBlocked=false, exhausted=false;
    bool retentionExhausted() const { return exhausted; }
    IOReturn exportQueryAdapter(Cx3AdapterResult *);
};
__EXPORT_METHOD__
struct IOExternalMethodArguments {
    void *structureOutput=nullptr; size_t structureOutputSize=0;
    const void *structureInput=nullptr; size_t structureInputSize=0;
    UInt32 scalarInputCount=0; UInt64 *scalarInput=nullptr;
    UInt32 scalarOutputCount=0; UInt64 *scalarOutput=nullptr;
};
class Cx3Mlx4UserClient {
public:
    Cx3Mlx4 *fProvider=nullptr;
    IOReturn doQueryAdapter(IOExternalMethodArguments *);
};
__DO_METHOD__
static unsigned checks=0, failures=0;
#define CHECK(n,e) do { ++checks; if (!(e)) { ++failures; std::printf("FAIL %s\n",n); } } while(0)
static bool zero(const UInt8 *p, unsigned n) { for(unsigned i=0;i<n;i++) if(p[i]) return false; return true; }
static void testExport() {
    Cx3Mlx4 p; Cx3AdapterResult r; memset(p.fHw.board,'B',64);
    for(unsigned i=0;i<256;i++) p.fHw.raw[i]=(UInt8)(i ^ 0xa5);
    memset(&r,0xcc,sizeof(r));
    CHECK("raw query succeeds", p.exportQueryAdapter(&r)==kIOReturnSuccess);
    CHECK("exact raw bytes and board ID copied", r.rawBytes==256 && !memcmp(r.raw,p.fHw.raw,256) && !memcmp(r.boardId,p.fHw.board,64));
    CHECK("adapter metadata initialized", r.version==1 && r.structBytes==360 && r.status==0 && r.fwStatus==0 && r.deviceGeneration==0x1122334455667788ull && r.intaPin==0x3f && r.reserved==0);
    CHECK("single ownership/window/query observation", p.fHw.ownerCalls==1 && p.fHw.aliveCalls==1 && p.fHw.queryCalls==1);
    memset(&r,0xcc,sizeof(r)); p.fResetCommandsBlocked=true;
    CHECK("reset-blocked refusal", p.exportQueryAdapter(&r)==kIOReturnSuccess && r.status==(UInt64)kIOReturnNotPermitted && r.fwStatus==CX3_FWSTATUS_NOT_POSTED && r.intaPin==0xff && r.rawBytes==0 && zero(r.raw,256) && zero(r.boardId,64));
    CHECK("reset refusal observes no hardware", p.fHw.ownerCalls==1 && p.fHw.aliveCalls==1 && p.fHw.queryCalls==1);
    p.fResetCommandsBlocked=false; p.exhausted=true;
    CHECK("retention refusal observes no hardware", p.exportQueryAdapter(&r)==kIOReturnSuccess && r.status==(UInt64)kIOReturnNotPermitted && p.fHw.queryCalls==1);
    p.exhausted=false; p.fHw.alive=false;
    CHECK("offline refusal has clean payload", p.exportQueryAdapter(&r)==kIOReturnSuccess && r.status==(UInt64)kIOReturnOffline && r.fwStatus==CX3_FWSTATUS_NOT_POSTED && r.rawBytes==0 && zero(r.raw,256) && zero(r.boardId,64));
    CHECK("offline refusal stops before query", p.fHw.queryCalls==1);
    p.fHw.alive=true; p.fHw.owner=1;
    CHECK("ownership refusal is clean and skips query", p.exportQueryAdapter(&r)==kIOReturnSuccess && r.status==(UInt64)kIOReturnNotPermitted && r.fwStatus==CX3_FWSTATUS_NOT_POSTED && r.intaPin==0xff && r.rawBytes==0 && zero(r.raw,256) && zero(r.boardId,64) && p.fHw.queryCalls==1 && p.fHw.aliveCalls==2);
    p.fHw.owner=0;
    p.fHw.alive=true; p.fHw.queryOk=false; p.fHw.fw=7;
    CHECK("firmware failure keeps attempted status", p.exportQueryAdapter(&r)==kIOReturnSuccess && r.status==(UInt64)kIOReturnError && r.fwStatus==7 && r.intaPin==0xff && r.rawBytes==0 && zero(r.raw,256) && zero(r.boardId,64));
}
static void testHandler() {
    Cx3Mlx4 p; Cx3Mlx4UserClient c; c.fProvider=&p;
    Cx3AdapterResult r; memset(&r,0xcc,sizeof(r));
    IOExternalMethodArguments a; a.structureOutput=&r; a.structureOutputSize=sizeof(r);
    CHECK("exact handler output size accepted", c.doQueryAdapter(&a)==kIOReturnSuccess && r.rawBytes==256);
    a.structureOutputSize=sizeof(r)-1;
    unsigned before=p.fHw.queryCalls;
    CHECK("short handler output rejected before query", c.doQueryAdapter(&a)==kIOReturnBadArgument && p.fHw.queryCalls==before);
    a.structureOutputSize=sizeof(r)+1;
    CHECK("oversized handler output rejected before query", c.doQueryAdapter(&a)==kIOReturnBadArgument && p.fHw.queryCalls==before);
    a.structureOutput=nullptr; a.structureOutputSize=sizeof(r);
    CHECK("missing handler output rejected before query", c.doQueryAdapter(&a)==kIOReturnBadArgument && p.fHw.queryCalls==before);
    a.structureOutput=&r; a.structureOutputSize=sizeof(r); a.scalarInputCount=1;
    CHECK("handler rejects scalar input before query", c.doQueryAdapter(&a)==kIOReturnBadArgument && p.fHw.queryCalls==before);
    a.scalarInputCount=0; a.scalarOutputCount=1;
    CHECK("handler rejects scalar output before query", c.doQueryAdapter(&a)==kIOReturnBadArgument && p.fHw.queryCalls==before);
    a.scalarOutputCount=0; a.structureInput=&r; a.structureInputSize=1;
    CHECK("handler rejects structure input before query", c.doQueryAdapter(&a)==kIOReturnBadArgument && p.fHw.queryCalls==before);
    a.structureInput=nullptr; a.structureInputSize=0;
    c.fProvider=nullptr; a.structureOutput=&r;
    CHECK("detached handler rejected", c.doQueryAdapter(&a)==kIOReturnNotAttached);
}
int main(){ testExport(); testHandler(); std::printf("QUERY_ADAPTER_EXPORT: %u checks, %u failures\n",checks,failures); return failures?1:0; }
'''.replace('__EXPORT_METHOD__', export).replace('__DO_METHOD__', do).replace('__ABI__', abi_def)

with tempfile.TemporaryDirectory(prefix='cx3-adapter-export-') as temp:
    binary = str(Path(temp) / 'test')
    subprocess.run(['/usr/bin/clang++','-std=c++11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                    '-I',str(ROOT/'src'),'-x','c++','-','-o',binary],
                   input=code,text=True,check=True)
    result = subprocess.run([binary],timeout=10)
    if result.returncode:
        raise SystemExit(result.returncode)

start = cli.index('static int cmdRawJson(')
open_brace = cli.index('{', start)
depth = 0
for end in range(open_brace, len(cli)):
    if cli[end] == '{':
        depth += 1
    elif cli[end] == '}':
        depth -= 1
        if depth == 0:
            break
rawjson = cli[start:end + 1]
rawjson_harness = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int kern_return_t; typedef unsigned int io_connect_t;
enum { KERN_SUCCESS=0, KERN_FAILURE=5, CX3_SCALAR_IN_MAX=16,
       CX3_SCALAR_OUT_MAX=16, CX3_HCR_MAILBOX_SIZE=4096 };
static io_connect_t gConn;
static unsigned seenSelector=999, seenIn=999, seenOut=999;
static size_t seenStruct=0; static unsigned calls=0;
static void put32(unsigned char *p,unsigned v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void put64(unsigned char *p,uint64_t v) { put32(p,(unsigned)v); put32(p+4,(unsigned)(v>>32)); }
static int openService(void) { gConn=7; return 0; }
static void breadcrumb(const char *fmt,...) { (void)fmt; }
kern_return_t IOConnectCallMethod(io_connect_t c, unsigned selector,
    const uint64_t *in, unsigned inCount, const void *si, size_t siLen,
    uint64_t *out, unsigned *outCount, void *so, size_t *soSize) {
    (void)c; (void)in; (void)si; (void)siLen; (void)out;
    ++calls; seenSelector=selector; seenIn=inCount; seenOut=outCount?*outCount:999;
    seenStruct=soSize?*soSize:0;
    if (selector!=47 || inCount!=0 || !outCount || *outCount!=0 || !so || !soSize || *soSize!=360) return 99;
    unsigned char *p=(unsigned char*)so; unsigned char raw[256]={0};
    const unsigned char id[16]={'P','S','I','D','T','E','S','T','0','0','0','0','0','0','0','1'};
    memset(so,0,360); raw[0x10]=63;
    memcpy(raw+0xf0,"DISPTSET00001000",16); /* four reversed BE dwords */
    put32(p,1); put32(p+4,360); put64(p+8,0); put32(p+16,0); put32(p+20,256);
    put64(p+24,0x1122334455667788ull); put32(p+32,63); put32(p+36,0);
    memcpy(p+40,id,16); memcpy(p+104,raw,256); *soSize=360; return KERN_SUCCESS;
}
FUNCTION
int main(void) {
    char *argv[]={(char*)"cx3ctl",(char*)"47",(char*)"out",(char*)"0",(char*)"struct",(char*)"360"};
    int rc=cmdRawJson(6,argv,1);
    if(rc || calls!=1 || seenSelector!=47 || seenIn!=0 || seenOut!=0 || seenStruct!=360) {
        fprintf(stderr,"FAIL rawjson 47 out 0 struct 360 rc=%d calls=%u selector=%u in=%u out=%u struct=%zu\n",
                rc,calls,seenSelector,seenIn,seenOut,seenStruct); return 1;
    }
    fprintf(stderr,"RAWJSON_ADAPTER: synthetic transport accepted selector47 out0 struct360\n");
    return 0;
}
'''.replace('FUNCTION', rawjson)
with tempfile.TemporaryDirectory(prefix='cx3-rawjson-adapter-') as temp:
    binary = str(Path(temp) / 'rawjson-test')
    subprocess.run(['/usr/bin/clang','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                    '-x','c','-','-o',binary],input=rawjson_harness,text=True,check=True)
    result = subprocess.run([binary],timeout=10,text=True,capture_output=True)
    print(result.stderr,end='')
    if result.returncode:
        print(result.stdout,end='')
        raise SystemExit(result.returncode)
    capture_dir = ROOT.parent.parent / 'outputs/20261024-cx3-adapter-export'
    capture_dir.mkdir(parents=True,exist_ok=True)
    (capture_dir/'synthetic-adapter-capture.json').write_text(result.stdout)
    print('SYNTHETIC_CAPTURE: outputs/20261024-cx3-adapter-export/synthetic-adapter-capture.json')

start = cli.index('static int cmdQueryAdapter(')
open_brace = cli.index('{', start)
depth = 0
for end in range(open_brace, len(cli)):
    if cli[end] == '{':
        depth += 1
    elif cli[end] == '}':
        depth -= 1
        if depth == 0:
            break
query_cli = cli[start:end + 1]
cli_harness = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int kern_return_t; typedef unsigned int io_connect_t;
enum { KERN_SUCCESS=0, KERN_FAILURE=5, kCx3SelQueryAdapter=47,
       CX3_ADAPTER_RESULT_VERSION=1, CX3_ADAPTER_RAW_BYTES=256 };
struct Cx3AdapterResult { uint32_t version,structBytes; uint64_t status;
    uint32_t fwStatus,rawBytes; uint64_t deviceGeneration; uint32_t intaPin,reserved;
    uint8_t boardId[64],raw[256]; };
static io_connect_t gConn; static unsigned scenario=0;
static int openService(void) { gConn=1; return 0; }
kern_return_t IOConnectCallMethod(io_connect_t c,unsigned sel,const uint64_t *in,unsigned ni,
    const void *si,size_t sil,uint64_t *out,unsigned *no,void *so,size_t *ns) {
    struct Cx3AdapterResult *r=(struct Cx3AdapterResult*)so;
    (void)c;(void)in;(void)ni;(void)si;(void)sil;(void)out;(void)no;
    if(sel!=47 || !ns || *ns!=360) return 99;
    if(scenario==1) return KERN_FAILURE;
    memset(r,0,sizeof(*r)); r->version=1;r->structBytes=360;r->fwStatus=0;r->rawBytes=256;r->intaPin=63;
    memset(r->boardId,'X',64); memset(r->raw,0x5a,256);
    switch(scenario) {
    case 2:*ns=359;break; case 3:r->version=2;break; case 4:r->structBytes=359;break;
    case 5:r->status=0xe00002c2ull;break; case 6:r->fwStatus=7;break;
    case 7:r->rawBytes=255;break; case 8:r->intaPin=256;break; case 9:r->reserved=1;break;
    default:break;
    }
    return KERN_SUCCESS;
}
FUNCTION
int main(void) {
    unsigned i; int failures=0;
    for(i=0;i<10;i++) { scenario=i; int rc=cmdQueryAdapter();
        if((i==0 && rc!=0)||(i!=0 && rc==0)) { fprintf(stderr,"FAIL CLI validation scenario %u rc=%d\n",i,rc); ++failures; }
    }
    if(failures) return 1;
    fprintf(stderr,"QUERY_ADAPTER_CLI: valid response accepted; transport/short/ABI/status/fw/raw/pin/reserved rejected\n");
    return 0;
}
'''.replace('FUNCTION', query_cli)
with tempfile.TemporaryDirectory(prefix='cx3-adapter-cli-') as temp:
    binary = str(Path(temp) / 'queryadapter-test')
    subprocess.run(['/usr/bin/clang','-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                    '-x','c','-','-o',binary],input=cli_harness,text=True,check=True)
    raise SystemExit(subprocess.run([binary],timeout=10,stdout=subprocess.DEVNULL).returncode)
