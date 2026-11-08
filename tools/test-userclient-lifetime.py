#!/usr/bin/env python3
"""Run real client init/start/stop/free and a resumed production handler.

Breaks caught: stop clears a provider still used by an admitted call, provider
ownership is never retained, repeated stop releases too soon, missing gate lock
admits a client. Host IOKit/refcounts are dependency substitutes, not kernel proof.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile


def method(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}\n', start) + 3]


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--source', type=Path)
    args = p.parse_args()
    src = Path(__file__).resolve().parents[1] / 'src'
    text = (args.source or src / 'Cx3Mlx4UserClient.cpp').read_text()
    signatures = ['bool Cx3Mlx4UserClient::initWithTask(',
                  'bool Cx3Mlx4UserClient::start(', 'void Cx3Mlx4UserClient::stop(',
                  'IOReturn Cx3Mlx4UserClient::doProbe(']
    methods = '\n'.join(method(text, sig) for sig in signatures)
    for sig, fallback in [
        ('void Cx3Mlx4UserClient::free(', 'void Cx3Mlx4UserClient::free() { super::free(); }'),
        ('bool Cx3Mlx4UserClient::clientReady(',
         'bool Cx3Mlx4UserClient::clientReady() { return fProvider != nullptr; }')]:
        methods += method(text, sig) if sig in text else fallback
    harness = r'''
#include <cstdint>
#include <cstdio>
#include <mutex>
using UInt32=uint32_t; using task_t=void*; using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnNotAttached=-1;
struct OSDictionary {};
static bool allocationFails=false,root=true;
static unsigned allocated=0,freed=0;
struct IOLock { std::mutex mutex; };
IOLock* IOLockAlloc() { if (allocationFails) return nullptr; ++allocated; return new IOLock; }
void IOLockFree(IOLock* l) { ++freed; delete l; }
void IOLockLock(IOLock* l) { l->mutex.lock(); }
void IOLockUnlock(IOLock* l) { l->mutex.unlock(); }
struct IOService { virtual ~IOService() {};
    unsigned refs=1;
    void retain() { ++refs; } void release() { --refs; }
};
struct Cx3Mlx4 : IOService {
    unsigned probes=0;
    void exportProbe(uint64_t*) { ++probes; }
};
#define OSDynamicCast(type,p) dynamic_cast<type*>(p)
static void logSink(const char*,...) {}
#define LOG(...) logSink(__VA_ARGS__)
struct IOExternalMethodArguments { uint64_t values[8]{}; uint64_t* scalarOutput=values; };
struct Base : IOService {
    bool startOK=true; unsigned stops=0,frees=0;
    bool initWithTask(task_t,void*,UInt32,OSDictionary*) { return true; }
    bool start(IOService*) { return startOK; }
    void stop(IOService*) { ++stops; }
    void free() { ++frees; }
    int clientHasPrivilege(void*,const char*) { return root ? 0 : -1; }
};
struct Cx3Mlx4UserClient : Base {
    using super=Base;
    task_t fTask=nullptr; Cx3Mlx4* fProvider=nullptr;
    IOLock* fClientLock=nullptr; bool fClientStopping=false;
    bool initWithTask(task_t,void*,UInt32,OSDictionary*);
    bool start(IOService*); void stop(IOService*); void free(); bool clientReady();
    IOReturn doProbe(IOExternalMethodArguments*);
};
METHODS
static unsigned checks=0,failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %u: %s\n",__LINE__,#x); } } while(0)
int main() {
    {
        Cx3Mlx4 d; Cx3Mlx4UserClient c;
        CHECK(c.initWithTask(nullptr,nullptr,0,nullptr)); CHECK(c.start(&d));
        CHECK(c.clientReady()); CHECK(d.refs==2);
        c.stop(&d); CHECK(!c.clientReady()); CHECK(c.fProvider==&d && d.refs==2);
        // Resume only the handler of a call admitted BEFORE stop. New external
        // calls must be refused separately by externalMethod's client gate.
        IOExternalMethodArguments a;
        CHECK(c.doProbe(&a)==kIOReturnSuccess); CHECK(d.probes==1);
        c.stop(&d); CHECK(d.refs==2);
        c.free(); CHECK(d.refs==1 && c.fProvider==nullptr); CHECK(c.frees==1);
    }
    {
        Cx3Mlx4UserClient c; allocationFails=true;
        CHECK(!c.initWithTask(nullptr,nullptr,0,nullptr));
        CHECK(!c.clientReady()); c.free(); allocationFails=false;
    }
    {
        Cx3Mlx4 d; Cx3Mlx4UserClient c;
        CHECK(c.initWithTask(nullptr,nullptr,0,nullptr)); c.startOK=false;
        CHECK(!c.start(&d)); CHECK(d.refs==1 && c.fProvider==nullptr); c.free();
    }
    {
        Cx3Mlx4UserClient c; IOService wrong;
        CHECK(c.initWithTask(nullptr,nullptr,0,nullptr)); CHECK(!c.start(&wrong));
        CHECK(c.fProvider==nullptr); c.free();
    }
    {
        Cx3Mlx4UserClient c; root=false;
        CHECK(!c.initWithTask(nullptr,nullptr,0,nullptr)); CHECK(!c.clientReady());
        c.free(); root=true;
    }
    CHECK(allocated==freed);
    std::printf("userclient lifetime: %u checks, %u failures (production methods / host IOKit)\n",checks,failures);
    return failures ? 1 : 0;
}
'''.replace('METHODS', methods)
    with tempfile.TemporaryDirectory(prefix='cx3-userclient-lifetime-') as temp:
        binary = str(Path(temp) / 'test')
        subprocess.run(['/usr/bin/clang++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        '-x', 'c++', '-', '-o', binary], input=harness, text=True, check=True)
        return subprocess.run([binary], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
