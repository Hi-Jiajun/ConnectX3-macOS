#!/usr/bin/env python3
"""Require IRQ event-source removal failures to retain teardown owners."""
from pathlib import Path
import re
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
    release = body(cpp, "bool Cx3Mlx4::irqReleaseObjects(")
    stop = body(cpp, "void Cx3Mlx4::stop(IOService *provider)")
    irq_down = body(cpp, "IOReturn Cx3Mlx4::exportM3IrqDown(UInt64 *out)")
    profile = body(cpp, "IOReturn Cx3Mlx4::exportM2dProfile(")
    strip_comments = lambda value: re.sub(r"/\*.*?\*/|//[^\n]*", "", value, flags=re.S)
    release, stop, irq_down, profile = map(strip_comments, (release, stop, irq_down, profile))
    remove_failure = release[release.index("if (removeRc != kIOReturnSuccess)") :]
    release_success_tail = release[release.index("fIrqSource->release();") :]
    stop_irq_failure = stop[stop.index('if (!irqReleaseObjects("driver stopping"))') :]
    profile_irq_failure = profile[profile.index(
        'if (!irqReleaseObjects("a new profile replaces the tables"))'
    ) :]

    checks = {
        "release API reports failure": "bool     irqReleaseObjects(const char *why);" in header,
        "remove result is checked before local source release": ordered(
            release,
            "const IOReturn removeRc = fWorkLoop->removeEventSource(fIrqSource);",
            "if (removeRc != kIOReturnSuccess)",
            "fIrqSource->release();",
        ) and "return false;" in remove_failure[:remove_failure.index("fIrqSource->release();")],
        "missing workloop does not release a still-attached source": (
            "if (!fWorkLoop)" in release and "return false;" in release
        ),
        "release state is cleared only after successful removal": ordered(
            release_success_tail,
            "fIrqSource->release();",
            "fIrqSource = 0;",
            "fIrqObjectsUp = false;",
            "return true;",
        ),
        "stop retains provider and returns before EQ/device teardown": ordered(
            stop_irq_failure,
            "fStopRetained = true;",
            "super::stop(provider);",
            'teardownEq("driver stopping", false);',
        ),
        "stop confirms IRQ removal before network/resource teardown": ordered(
            stop,
            'if (!irqReleaseObjects("driver stopping"))',
            'if (!netServiceStop("driver stopping")',
            'teardownEq("driver stopping", false);',
        ),
        "IRQ-down reports failure and does not hand back EQ": ordered(
            irq_down,
            'if (!irqReleaseObjects("m3irqdown"))',
            "kIrqDownOutStatus",
            "return kIOReturnSuccess;",
            "fHw.eqHw2Sw(fEqn, &hw2sw)",
        ),
        "profile replacement aborts before tearing down current resources": ordered(
            profile_irq_failure,
            "res->status =",
            "return kIOReturnSuccess;",
            'teardownEq("a new profile replaces the tables", false);',
        ) and profile.find("fProfile = requestedProfile;") > profile.find(
            'if (!irqReleaseObjects("a new profile replaces the tables"))'
        ),
    }
    for name, passed in checks.items():
        print(f"{'PASS' if passed else 'FAIL'} {name}")
    failures = sum(not passed for passed in checks.values())
    print(f"IRQ release failure contract: {len(checks) - failures}/{len(checks)} checks")
    production = "bool Cx3Mlx4::irqReleaseObjects(const char *why) {" + body(
        cpp, "bool Cx3Mlx4::irqReleaseObjects("
    ) + "}"
    harness = r'''
#include <cstdio>
using IOReturn = int;
static const IOReturn kIOReturnSuccess = 0;
static unsigned checks=0, failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line %u: %s\n", __LINE__, #x); } } while (0)
static void logSink(const char*, ...) {}
#define LOG(...) logSink(__VA_ARGS__)
struct IOEventSource {
    unsigned refs=2, disables=0, releases=0;
    void disable() { ++disables; }
    void release() { ++releases; --refs; }
};
struct IOWorkLoop {
    IOReturn removeRc=kIOReturnSuccess;
    unsigned removes=0;
    IOReturn removeEventSource(IOEventSource *source) {
        ++removes;
        if (removeRc == kIOReturnSuccess) source->release(); // release workloop retain
        return removeRc;
    }
};
class Cx3Mlx4 {
public:
    IOEventSource *fIrqSource=nullptr;
    IOWorkLoop *fWorkLoop=nullptr;
    bool fIrqObjectsUp=true, fIrqEnabled=true, fIrqStormTripped=false;
    unsigned long long fIrqCount=0, fIrqCallbackCount=0, fIrqEqeCount=0, fIrqSkipped=0;
    bool irqReleaseObjects(const char *why);
};
PRODUCTION
int main() {
    {
        Cx3Mlx4 d; IOEventSource source; IOWorkLoop loop;
        d.fIrqSource=&source; d.fWorkLoop=&loop;
        loop.removeRc=1;
        CHECK(!d.irqReleaseObjects("test"));
        CHECK(source.disables==1 && source.releases==0 && source.refs==2);
        CHECK(loop.removes==1 && d.fIrqSource==&source);
        CHECK(d.fIrqObjectsUp && !d.fIrqEnabled);
    }
    {
        Cx3Mlx4 d; IOEventSource source;
        d.fIrqSource=&source; d.fWorkLoop=nullptr;
        CHECK(!d.irqReleaseObjects("test"));
        CHECK(source.disables==1 && source.releases==0 && source.refs==2);
        CHECK(d.fIrqSource==&source && d.fIrqObjectsUp && !d.fIrqEnabled);
    }
    {
        Cx3Mlx4 d; IOEventSource source; IOWorkLoop loop;
        d.fIrqSource=&source; d.fWorkLoop=&loop;
        CHECK(d.irqReleaseObjects("test"));
        CHECK(source.disables==1 && source.releases==2 && source.refs==0);
        CHECK(loop.removes==1 && d.fIrqSource==nullptr);
        CHECK(!d.fIrqObjectsUp && !d.fIrqEnabled);
    }
    std::printf("IRQ release behavior: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
'''.replace('PRODUCTION', production)
    with tempfile.TemporaryDirectory(prefix="cx3-irq-release-") as temp:
        binary = str(Path(temp) / "test")
        subprocess.run(["/usr/bin/clang++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                        "-x", "c++", "-", "-o", binary], input=harness,
                       text=True, check=True)
        behavior_rc = subprocess.run([binary], timeout=10).returncode
    return 1 if failures or behavior_rc else 0


if __name__ == "__main__":
    raise SystemExit(main())
