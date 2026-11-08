#!/usr/bin/env python3
"""Execute the production synchronous-detach worker with host IOKit doubles."""
from pathlib import Path
import subprocess
import tempfile


SOURCE = Path(__file__).resolve().parents[1] / "src" / "Cx3Mlx4.cpp"


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 0
    for pos in range(opening, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[opening + 1:pos]
    raise AssertionError(f"unterminated function: {signature}")


def main() -> int:
    text = SOURCE.read_text()
    method = function_body(text, "bool Cx3Mlx4::netDetachInterfacesAndWait(UInt32 waitMs)")
    worker = function_body(text, "void Cx3Mlx4::netDetachThread(")
    harness = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
using UInt32=uint32_t; using UInt64=uint64_t; using wait_result_t=int;
using AbsoluteTime=uint64_t;
static const int THREAD_UNINT=0, THREAD_AWAKENED=0;
static const int THREAD_TIMED_OUT=1;
static const UInt32 kNanosecondScale=1;
static bool runWorkerInline=true, sleepTimesOut=false, permitCallFree=true;
static bool returnAlreadyPending=false;
struct OSObject { virtual ~OSObject() {} };
struct IOLock { std::recursive_mutex mutex; };
struct IONetworkInterface : OSObject {
    unsigned refs=1, releases=0;
    void release() { --refs; ++releases; }
};
struct ThreadCall;
using thread_call_t=ThreadCall*;
using thread_call_param_t=void*;
using thread_call_func_t=void(*)(thread_call_param_t, thread_call_param_t);
struct ThreadCall {
    thread_call_func_t fn; void *arg; bool freed;
    ThreadCall(thread_call_func_t f, void *a) : fn(f), arg(a), freed(false) {}
};
static void IOLockLock(IOLock *l) { l->mutex.lock(); }
static void IOLockUnlock(IOLock *l) { l->mutex.unlock(); }
static void IOLockWakeup(IOLock*, void*, bool) {}
static void clock_interval_to_deadline(UInt32 interval, UInt32, AbsoluteTime *out) { *out=interval; }
static wait_result_t IOLockSleepDeadline(IOLock*, void*, AbsoluteTime, UInt32) {
    return sleepTimesOut ? THREAD_TIMED_OUT : THREAD_AWAKENED;
}
static bool thread_call_free(thread_call_t call) {
    if (!call || call->freed || !permitCallFree) return false;
    call->freed=true; return true;
}
static bool thread_call_enter(thread_call_t call) {
    if (returnAlreadyPending) return true;
    if (runWorkerInline) call->fn(call->arg, nullptr);
    return false;
}
static void logSink(const char*,...) {}
#define LOG(...) logSink(__VA_ARGS__)
#define OSDynamicCast(type, value) dynamic_cast<type*>(value)
class Cx3Mlx4 : public OSObject {
public:
    IOLock *fNetAttachLock=nullptr;
    IONetworkInterface *fNetIface=nullptr, *fNetAttachIface=nullptr;
    thread_call_t fNetDetachCall=nullptr;
    bool fNetDetachInFlight=false, fNetDetachDone=false, fNetDetachCallFreeFailed=false;
    UInt32 fNetDetachIfaceCount=0, fNetDetachRefCounts[2]={0,0};
    IONetworkInterface *fNetDetachIfaces[2]={nullptr,nullptr};
    unsigned refs=1, detachCalls=0, nonsyncCalls=0;
    bool allSync=true;
    void retain() { ++refs; }
    void release() { --refs; }
    void detachInterface(IONetworkInterface*, bool sync) { ++detachCalls; allSync &= sync; }
    bool netDetachInterfacesAndWait(UInt32);
    static void netDetachThread(thread_call_param_t, thread_call_param_t);
};
METHOD
WORKER
static unsigned cases=0, checks=0, failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL case %u line %u: %s\n",cases,__LINE__,#x); } } while(0)
static ThreadCall makeCall(Cx3Mlx4 *d) { return ThreadCall(&Cx3Mlx4::netDetachThread,d); }
int main() {
    {
        ++cases; Cx3Mlx4 d; IOLock lock; IONetworkInterface iface; ThreadCall call=makeCall(&d);
        d.fNetAttachLock=&lock; d.fNetDetachCall=&call; d.fNetIface=&iface;
        CHECK(d.netDetachInterfacesAndWait(2000));
        CHECK(d.detachCalls==1 && d.allSync);
        CHECK(iface.refs==0 && iface.releases==1);
        CHECK(d.fNetIface==nullptr && d.fNetAttachIface==nullptr);
        CHECK(d.fNetDetachDone && !d.fNetDetachInFlight && d.fNetDetachCall==nullptr);
        CHECK(d.refs==1 && call.freed);
    }
    {
        ++cases; Cx3Mlx4 d; IOLock lock; IONetworkInterface iface; iface.refs=2; ThreadCall call=makeCall(&d);
        d.fNetAttachLock=&lock; d.fNetDetachCall=&call;
        d.fNetIface=&iface; d.fNetAttachIface=&iface;
        CHECK(d.netDetachInterfacesAndWait(2000));
        CHECK(d.detachCalls==1 && d.allSync);
        CHECK(iface.refs==0 && iface.releases==2);
    }
    {
        ++cases; Cx3Mlx4 d; IOLock lock; IONetworkInterface a,b;
        ThreadCall call=makeCall(&d); d.fNetAttachLock=&lock; d.fNetDetachCall=&call;
        d.fNetIface=&a; d.fNetAttachIface=&b;
        CHECK(d.netDetachInterfacesAndWait(2000));
        CHECK(d.detachCalls==2 && d.allSync);
        CHECK(a.refs==0 && b.refs==0 && a.releases==1 && b.releases==1);
    }
    {
        ++cases; Cx3Mlx4 d; IOLock lock; ThreadCall call=makeCall(&d);
        d.fNetAttachLock=&lock; d.fNetDetachCall=&call;
        CHECK(d.netDetachInterfacesAndWait(2000));
        CHECK(call.freed && d.fNetDetachCall==nullptr && d.detachCalls==0);
    }
    {
        ++cases; Cx3Mlx4 d; IOLock lock; IONetworkInterface iface;
        d.fNetAttachLock=&lock; d.fNetIface=&iface; d.fNetDetachCall=nullptr;
        CHECK(!d.netDetachInterfacesAndWait(2000));
        CHECK(d.fNetIface==&iface && iface.refs==1 && d.detachCalls==0);
    }
    {
        ++cases; Cx3Mlx4 d; IOLock lock; IONetworkInterface iface; ThreadCall call=makeCall(&d);
        runWorkerInline=false; sleepTimesOut=true;
        d.fNetAttachLock=&lock; d.fNetDetachCall=&call; d.fNetIface=&iface;
        CHECK(!d.netDetachInterfacesAndWait(2000));
        CHECK(d.fNetIface==nullptr && d.fNetDetachIfaces[0]==&iface);
        CHECK(iface.refs==1 && d.refs==2 && d.fNetDetachInFlight);
        CHECK(!call.freed && !d.fNetDetachDone);
        runWorkerInline=true; sleepTimesOut=false;
    }
    {
        ++cases; Cx3Mlx4 d; IOLock lock; IONetworkInterface iface; ThreadCall call=makeCall(&d);
        permitCallFree=false;
        d.fNetAttachLock=&lock; d.fNetDetachCall=&call; d.fNetIface=&iface;
        CHECK(!d.netDetachInterfacesAndWait(2000));
        CHECK(d.detachCalls==1 && iface.refs==0);
        CHECK(d.fNetDetachDone && d.fNetDetachCallFreeFailed && d.fNetDetachCall==&call);
        CHECK(d.refs==1 && !call.freed);
        permitCallFree=true;
    }
    {
        ++cases; Cx3Mlx4 d; IOLock lock; IONetworkInterface iface; ThreadCall call=makeCall(&d);
        d.retain(); /* the pre-existing pending invocation owns this reference */
        runWorkerInline=false; returnAlreadyPending=true;
        d.fNetAttachLock=&lock; d.fNetDetachCall=&call; d.fNetIface=&iface;
        CHECK(!d.netDetachInterfacesAndWait(2000));
        CHECK(d.refs==2 && d.fNetDetachInFlight);
        CHECK(d.fNetIface==nullptr && d.fNetAttachIface==nullptr);
        CHECK(iface.refs==1 && d.detachCalls==0);
        call.fn(call.arg, nullptr);
        CHECK(d.refs==1 && iface.refs==0 && iface.releases==1);
        CHECK(d.fNetDetachDone && !d.fNetDetachInFlight && call.freed);
        runWorkerInline=true; returnAlreadyPending=false;
    }
    std::printf("TX detach worker: %u cases, %u checks, %u failures (production helper/worker)\n",cases,checks,failures);
    return failures ? 1 : 0;
}
'''.replace("METHOD", "bool Cx3Mlx4::netDetachInterfacesAndWait(UInt32 waitMs) {\n" + method + "\n}") \
       .replace("WORKER", "void Cx3Mlx4::netDetachThread(thread_call_param_t param0, thread_call_param_t param1) {\n" + worker + "\n}")
    with tempfile.TemporaryDirectory(prefix="cx3-net-detach-") as temp:
        binary = str(Path(temp) / "test")
        subprocess.run(
            ["/usr/bin/clang++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-parameter", "-Wno-unused-variable", "-I",
             str(SOURCE.parent), "-x", "c++", "-", "-o", binary],
            input=harness, text=True, check=True,
        )
        return subprocess.run([binary], timeout=10).returncode


if __name__ == "__main__":
    raise SystemExit(main())
