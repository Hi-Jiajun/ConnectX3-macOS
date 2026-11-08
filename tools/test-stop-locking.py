#!/usr/bin/env python3
"""Exercise production admission/drain with real host threads, not a gate copy.

Breaks caught: stop queues behind a firmware transaction; an admitted waiter
dispatches after stop; sleep-count budgets ignore oversleep; absent locks admit
unserialized work. IOKit locks/time are host substitutes, not kernel validation.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile


def method(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}\n', start) + 3]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path)
    args = parser.parse_args()
    src = Path(__file__).resolve().parents[1] / 'src'
    source = (args.source or src / 'Cx3Mlx4.cpp').read_text()
    signatures = ['void Cx3Mlx4::transactionEnter(' if 'void Cx3Mlx4::transactionEnter(' in source
                  else 'bool Cx3Mlx4::transactionEnter(',
                  'void Cx3Mlx4::transactionExit(', 'bool Cx3Mlx4::transactionTryEnter(',
                  'bool Cx3Mlx4::selectorEnter(', 'void Cx3Mlx4::selectorExit(',
                  'UInt32 Cx3Mlx4::selectorActive(', 'bool Cx3Mlx4::beginStopAndDrain(']
    extra = 'bool Cx3Mlx4::selectorStopping('
    if extra in source:
        signatures.append(extra)
    methods = '\n'.join(method(source, s) for s in signatures)
    enter_type = 'void' if signatures[0].startswith('void') else 'bool'
    harness = r'''
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>
#define CX3_HOST_TEST
#include "mlx4_pure.h"
using UInt32=uint32_t; using UInt64=uint64_t;
struct IOLock { std::mutex mutex; };
static std::atomic<unsigned> allocations{0},sleeps{0};
static std::atomic<bool> failAllocation{false},virtualTime{false};
static uint64_t fakeTicks=0;
static IOLock* IOLockAlloc() { ++allocations; return failAllocation ? nullptr : new IOLock; }
static void IOLockLock(IOLock* l) { l->mutex.lock(); }
static void IOLockUnlock(IOLock* l) { l->mutex.unlock(); }
static bool IOLockTryLock(IOLock* l) { return l->mutex.try_lock(); }
// Deliberately non-unit timebase: one tick is 3 ns.
static void clock_get_uptime(UInt64* t) {
    *t=virtualTime ? fakeTicks : std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()/3;
}
static void absolutetime_to_nanoseconds(UInt64 t,UInt64* ns) { *ns=t*3; }
static void IOSleep(unsigned ms) {
    ++sleeps;
    if (virtualTime) fakeTicks+=10000000; // 30 ms per nominal 1 ms sleep
    else std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
static void logSink(const char*,...) {}
#define LOG(...) logSink(__VA_ARGS__)
class Cx3Mlx4 {
public:
    IOLock *fXactLock=nullptr,*fAdmissionLock=nullptr;
    Mlx4LifecycleGate fGate{}; bool fQuarExhausted=false;
    ENTER_TYPE transactionEnter(); void transactionExit(); bool transactionTryEnter();
    bool selectorEnter(); void selectorExit(); UInt32 selectorActive();
    bool beginStopAndDrain(UInt32);
    bool selectorStopping() STOPPING_BODY
};
METHODS
static unsigned checks=0,failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %u: %s\n",__LINE__,#x); } } while(0)
int main() {
    {
        Cx3Mlx4 d; IOLock transaction,admission;
        d.fXactLock=&transaction; d.fAdmissionLock=&admission; d.fGate.init();
        CHECK(d.selectorEnter());
        transaction.mutex.lock();
        std::atomic<bool> done{false}; bool drained=true;
        std::thread stopper([&] { drained=d.beginStopAndDrain(15); done=true; });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // Assert before releasing the long transaction, so the old implementation
        // fails deterministically instead of hanging the test indefinitely.
        CHECK(done.load());
        if (done) { CHECK(d.selectorStopping()); CHECK(!d.selectorEnter()); }
        transaction.mutex.unlock(); stopper.join();
        CHECK(!drained);
        d.selectorExit(); CHECK(d.selectorActive()==0);
        CHECK(d.beginStopAndDrain(0)); CHECK(!d.selectorEnter());
    }
    {
        Cx3Mlx4 d; IOLock transaction,admission;
        d.fXactLock=&transaction; d.fAdmissionLock=&admission; d.fGate.init();
        CHECK(d.selectorEnter());
        virtualTime=true; fakeTicks=0; sleeps=0;
        CHECK(!d.beginStopAndDrain(10));
        CHECK(sleeps==1); // elapsed 30 ms, not ten iterations (300 ms)
        d.selectorExit(); CHECK(d.beginStopAndDrain(0)); virtualTime=false;
    }
    {
        Cx3Mlx4 d; d.fGate.init(); failAllocation=true;
        unsigned before=allocations;
        CHECK(!d.transactionTryEnter()); CHECK(!d.selectorEnter());
        CHECK(!d.beginStopAndDrain(0)); CHECK(allocations==before);
    }
    std::printf("stop locking: %u checks, %u failures (host threads / synthetic IOKit timebase)\n",checks,failures);
    return failures ? 1 : 0;
}
'''.replace('ENTER_TYPE', enter_type).replace('STOPPING_BODY', ';' if extra in source else '{ return false; }').replace('METHODS', methods)
    with tempfile.TemporaryDirectory(prefix='cx3-stop-locking-') as tmp:
        binary = str(Path(tmp) / 'test')
        subprocess.run(['/usr/bin/clang++', '-std=c++11', '-pthread', '-Wall', '-Wextra',
                        '-Wno-unused-function', '-Werror', '-I', str(src), '-x', 'c++', '-', '-o', binary],
                       input=harness, text=True, check=True)
        return subprocess.run([binary], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
