#!/usr/bin/env python3
"""Exercise the production TX admission gate's CAS/close race on the host."""
from pathlib import Path
import subprocess
import tempfile


def main() -> int:
    src = Path(__file__).resolve().parents[1] / "src"
    harness = r'''
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
#define CX3_HOST_TEST
#include "mlx4_pure.h"

static std::atomic<unsigned> checks(0), failures(0);
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %u: %s\n", __LINE__, #x); } } while (0)

int main() {
    {
        Mlx4TxAdmissionGate gate; gate.init();
        CHECK(gate.enter());
        CHECK(gate.active() == 1u);
        CHECK(gate.close() == 1u);
        CHECK(gate.isClosed());
        CHECK(!gate.enter());
        CHECK(gate.leave());
        CHECK(gate.drained());
        CHECK(gate.close() == 0u);
        CHECK(!gate.leave());
    }
    for (unsigned round = 0; round < 100; ++round) {
        Mlx4TxAdmissionGate gate; gate.init();
        std::atomic<bool> start(false);
        std::atomic<unsigned> accepted(0);
        std::vector<std::thread> workers;
        for (unsigned i = 0; i < 8; ++i) {
            workers.emplace_back([&]() {
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                for (unsigned n = 0; n < 500; ++n) {
                    if (gate.enter()) {
                        accepted.fetch_add(1, std::memory_order_relaxed);
                        std::this_thread::yield();
                        CHECK(gate.leave());
                    }
                }
            });
        }
        std::thread stopper([&]() {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            std::this_thread::yield();
            (void)gate.close();
        });
        start.store(true, std::memory_order_release);
        for (auto &worker : workers) worker.join();
        stopper.join();
        CHECK(gate.isClosed());
        CHECK(gate.drained());
        CHECK(!gate.enter());
        CHECK(accepted.load() <= 8u * 500u);
    }
    std::printf("TX admission gate: %u checks, %u failures (CAS race stress)\n",
                checks.load(), failures.load());
    return failures.load() ? 1 : 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="cx3-tx-admission-") as temp:
        binary = str(Path(temp) / "test")
        subprocess.run(
            ["/usr/bin/clang++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
             "-pthread", "-I", str(src), "-x", "c++", "-", "-o", binary],
            input=harness, text=True, check=True,
        )
        return subprocess.run([binary], timeout=30).returncode


if __name__ == "__main__":
    raise SystemExit(main())
