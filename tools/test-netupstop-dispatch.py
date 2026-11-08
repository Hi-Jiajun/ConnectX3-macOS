#!/usr/bin/env python3
"""Exercise the actual selector-46 input boundary without loading a kext.

Regression: 0.44.41 changed input 6 (no attach) to 5 (attach). Compile the
production exportNetUpStop body with only its kernel-dependent callee replaced
by a recording sink. This checks routing, not whether IOKit or attach is safe.
"""
import pathlib
import subprocess
import tempfile


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    source = (root / "src/Cx3Mlx4.cpp").read_text()
    start = source.index("IOReturn Cx3Mlx4::exportNetUpStop(")
    end = source.index("\n}\n", start) + 3
    body = source[start:end]
    harness = r'''
#include <cstdint>
#include <cstdio>
using UInt64 = uint64_t;
using UInt32 = uint32_t;
using IOReturn = int;
constexpr int kNetUpStopInStopAfter = 0;
constexpr IOReturn kIOReturnBadArgument = -1;
constexpr IOReturn kIOReturnSuccess = 0;
#define LOG(...) ((void)0)
class Cx3Mlx4 {
public:
    int calls = 0;
    UInt32 routed = 99;
    IOReturn exportNetUpStop(const UInt64 *, UInt64 *);
    IOReturn netupRun(UInt32 step, UInt64 *) {
        ++calls;
        routed = step;
        return kIOReturnSuccess;
    }
};
'''
    harness += body
    harness += r'''
int main() {
    struct Case { UInt64 input; int route; };
    const Case cases[] = {
        {0, 1}, {1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}, {6, 6},
        {7, -1}, {UINT64_MAX, -1}, {0x100000006ULL, -1}
    };
    int failed = 0;
    for (const auto &c : cases) {
        Cx3Mlx4 driver;
        UInt64 out[6] = {};
        IOReturn rc = driver.exportNetUpStop(&c.input, out);
        bool ok = c.route < 0
            ? rc == kIOReturnBadArgument && driver.calls == 0
            : rc == kIOReturnSuccess && driver.calls == 1 &&
              driver.routed == static_cast<UInt32>(c.route);
        if (!ok) {
            std::printf("FAIL input=%llu expected route=%d, got route=%u calls=%d rc=%d\n",
                (unsigned long long)c.input, c.route, driver.routed, driver.calls, rc);
            ++failed;
        }
    }
    Cx3Mlx4 driver;
    UInt64 out[6] = {};
    if (driver.exportNetUpStop(nullptr, out) != kIOReturnSuccess ||
        driver.routed != 1 || driver.calls != 1) ++failed;
    if (!failed) std::puts("PASS netupstop: 6 reaches self-test; invalid inputs never attach (11 cases)");
    return failed ? 1 : 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="cx3-dispatch-") as tmp:
        binary = str(pathlib.Path(tmp) / "test")
        subprocess.run(["/usr/bin/clang++", "-std=c++11", "-Wall", "-Wextra",
                        "-Werror", "-x", "c++", "-o", binary, "-"],
                       input=harness, text=True, check=True)
        return subprocess.run([binary]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
