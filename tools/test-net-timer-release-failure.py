#!/usr/bin/env python3
"""Exercise production timer-source removal and fail-closed stop contracts."""
from pathlib import Path
import subprocess
import tempfile


SRC = Path(__file__).resolve().parents[1] / "src"


def body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for pos in range(brace, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[brace + 1 : pos]
    raise AssertionError(f"unterminated function: {signature}")


def ordered(source: str, *needles: str) -> bool:
    positions = [source.find(needle) for needle in needles]
    return all(pos >= 0 for pos in positions) and positions == sorted(positions)


def main() -> int:
    cpp = (SRC / "Cx3Mlx4.cpp").read_text()
    header = (SRC / "Cx3Mlx4.h").read_text()
    signature = "bool Cx3Mlx4::netTimerRelease(const char *why)"
    if signature not in cpp:
        print("FAIL production netTimerRelease helper is missing")
        return 1

    release = body(cpp, signature)
    timer_action = body(cpp, "void Cx3Mlx4::netTimerAction(")
    service = body(cpp, "bool Cx3Mlx4::netServiceStop(const char *why)")
    stop = body(cpp, "void Cx3Mlx4::stop(IOService *provider)")
    stop_timer_failure = stop[stop.index('if (!netServiceStop("driver stopping")') :]
    checks = {
        "timer release API is declared": "netTimerRelease(const char *why);" in header,
        "timer action enters the shared callback gate before netTick": ordered(
            timer_action,
            "if (!me->fNetTxAdmission.enter()) return;",
            "TimerAdmissionExit",
            "me->netTick();",
        ),
        "timer action leaves the shared callback gate on every return":
            "~TimerAdmissionExit() { gate->leave(); }" in timer_action,
        "remove result checked before releasing local timer owner": ordered(
            release,
            "fWorkLoop->removeEventSource(fNetTimer)",
            "if (removeRc != kIOReturnSuccess)",
            "fNetTimer->release();",
            "fNetTimer = 0;",
        ),
        "network stop aborts before TX teardown when timer remains attached": ordered(
            service,
            "if (!netTimerRelease(why))",
            "return false;",
            "IOLockLock(fNetTxLock);",
        ),
        "driver stop pins owners when timer removal is unconfirmed": ordered(
            stop_timer_failure,
            'if (!netServiceStop("driver stopping")',
            "retain();",
            "fStopRetained = true;",
            "super::stop(provider);",
            'teardownEq("driver stopping", false);',
        ),
    }
    failures = [name for name, passed in checks.items() if not passed]
    for name, passed in checks.items():
        print(f"{'PASS' if passed else 'FAIL'} {name}")

    production = signature + " {" + release + "}"
    action_start = cpp.index("void Cx3Mlx4::netTimerAction(")
    action_end = cpp.index("\n}", action_start) + 2
    action_production = cpp[action_start:action_end]
    harness = r'''
#include <cstdio>
#include <cstring>
#define CX3_HOST_TEST
#include "mlx4_pure.h"
using IOReturn = int;
static const IOReturn kIOReturnSuccess = 0;
static unsigned checks=0, failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %u: %s\n", __LINE__, #x); } } while (0)
static void logSink(const char*, ...) {}
#define LOG(...) logSink(__VA_ARGS__)
struct IOTimerEventSource {
    unsigned refs=2, cancels=0, releases=0;
    void cancelTimeout() { ++cancels; }
    void release() { ++releases; --refs; }
};
struct IOWorkLoop {
    IOReturn removeRc=kIOReturnSuccess;
    unsigned removes=0;
    IOReturn removeEventSource(IOTimerEventSource *source) {
        ++removes;
        if (removeRc == kIOReturnSuccess) source->release();
        return removeRc;
    }
};
struct OSObject {};
class Cx3Mlx4 : public OSObject {
public:
    IOTimerEventSource *fNetTimer=nullptr;
    IOWorkLoop *fWorkLoop=nullptr;
    Mlx4TxAdmissionGate fNetTxAdmission;
    unsigned ticks=0;
    bool netTimerRelease(const char *why);
    static void netTimerAction(OSObject *owner, IOTimerEventSource *src);
    void netTick() { ++ticks; }
};
#define OSDynamicCast(type, object) static_cast<type *>(object)
PRODUCTION
ACTION_PRODUCTION
int main() {
    {
        Cx3Mlx4 d; IOTimerEventSource timer;
        d.fNetTxAdmission.init();
        Cx3Mlx4::netTimerAction(&d, &timer);
        CHECK(d.ticks==1 && d.fNetTxAdmission.active()==0);
        d.fNetTxAdmission.close();
        Cx3Mlx4::netTimerAction(&d, &timer);
        CHECK(d.ticks==1 && d.fNetTxAdmission.active()==0);
    }
    {
        Cx3Mlx4 d; IOTimerEventSource timer; IOWorkLoop loop;
        d.fNetTimer=&timer; d.fWorkLoop=&loop; loop.removeRc=1;
        CHECK(!d.netTimerRelease("test"));
        CHECK(timer.cancels==1 && timer.releases==0 && timer.refs==2);
        CHECK(loop.removes==1 && d.fNetTimer==&timer);
    }
    {
        Cx3Mlx4 d; IOTimerEventSource timer;
        d.fNetTimer=&timer; d.fWorkLoop=nullptr;
        CHECK(!d.netTimerRelease("test"));
        CHECK(timer.cancels==1 && timer.releases==0 && timer.refs==2);
        CHECK(d.fNetTimer==&timer);
    }
    {
        Cx3Mlx4 d; IOTimerEventSource timer; IOWorkLoop loop;
        d.fNetTimer=&timer; d.fWorkLoop=&loop;
        CHECK(d.netTimerRelease("test"));
        CHECK(timer.cancels==1 && timer.releases==2 && timer.refs==0);
        CHECK(loop.removes==1 && d.fNetTimer==nullptr);
    }
    {
        Cx3Mlx4 d;
        CHECK(d.netTimerRelease("test"));
    }
    std::printf("timer release behavior: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
'''.replace("ACTION_PRODUCTION", action_production).replace("PRODUCTION", production)
    behavior_rc = 1
    with tempfile.TemporaryDirectory(prefix="cx3-timer-release-") as temp:
        binary = str(Path(temp) / "test")
        subprocess.run(
            ["/usr/bin/clang++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
             "-I", str(SRC), "-x", "c++", "-", "-o", binary],
            input=harness, text=True, check=True,
        )
        behavior_rc = subprocess.run([binary], timeout=10).returncode
    print(f"timer stop contract: {len(checks) - len(failures)}/{len(checks)} checks")
    return 1 if failures or behavior_rc else 0


if __name__ == "__main__":
    raise SystemExit(main())
