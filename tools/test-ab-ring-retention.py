#!/usr/bin/env python3
"""Exercise retained A/B EQ ring cleanup using the production retry method."""
from pathlib import Path
import subprocess
import tempfile

SRC = Path(__file__).resolve().parents[1] / 'src'


def function_source(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for pos in range(opening, len(source)):
        if source[pos] == '{':
            depth += 1
        elif source[pos] == '}':
            depth -= 1
            if depth == 0:
                return source[start:pos + 1]
    raise AssertionError(f'unclosed function: {signature}')


def main():
    hw_source = (SRC / 'Mlx4Hw.cpp').read_text()
    cx_source = (SRC / 'Cx3Mlx4.cpp').read_text()
    retry_start = hw_source.index('bool Mlx4Hw::retryAbRetainedRings()')
    retry = hw_source[retry_start:hw_source.index('\n}\n', retry_start) + 3]
    has_start = hw_source.index('bool Mlx4Hw::hasAbRetainedRings() const')
    has = hw_source[has_start:hw_source.index('\n}\n', has_start) + 3]

    harness = r'''
#include <cstdio>
#include <cstring>
#define CX3_HOST_TEST
#include "mlx4_pure.h"
static void sink(const char*, ...) {}
#define LOG(...) sink(__VA_ARGS__)
struct Mlx4Icm { UInt64 allocationId; Mlx4DmaGeneration generation; };
class Mlx4Hw {
public:
    UInt64 fDeviceGeneration=7;
    Mlx4AbRingRetention fAbRetainedRings[2];
    Mlx4MttAllocator fMttAllocator;
    UInt32 hw2swCalls=0,mttFreeCalls=0,icmFreeCalls=0;
    bool hw2swOk=true,mttFreeOk=true;
    bool hasAbRetainedRings() const;
    bool retryAbRetainedRings();
    bool eqHw2Sw(UInt32,UInt32 *status) {
        ++hw2swCalls; if (status) *status=hw2swOk?0u:0xfffffffdu; return hw2swOk;
    }
    bool mttFree(UInt32,UInt32,Mlx4Icm*) { ++mttFreeCalls; return mttFreeOk; }
    void icmFree(Mlx4Icm*) { ++icmFreeCalls; }
};
HAS_RETAINED
RETRY_RETAINED
static void keep(Mlx4Hw &hw, UInt32 slot, Mlx4Icm *icm, bool mayHold,
                 UInt32 offset, UInt32 pages, UInt32 eqn, UInt64 generation) {
    Mlx4AbRingRetention &r=hw.fAbRetainedRings[slot];
    r.icm=icm; r.mttOffset=offset; r.pages=pages; r.eqn=eqn;
    r.generation=generation; r.fwMayHold=mayHold;
}
int main() {
    Mlx4Hw empty;
    if (!empty.retryAbRetainedRings() || empty.hw2swCalls || empty.mttFreeCalls ||
        empty.icmFreeCalls || empty.hasAbRetainedRings()) return 1;

    Mlx4Hw hw; Mlx4Icm a{};
    keep(hw,0,&a,true,64,2,28,7);
    hw.hw2swOk=false;
    if (hw.retryAbRetainedRings() || hw.hw2swCalls!=1 || hw.mttFreeCalls ||
        hw.icmFreeCalls || !hw.hasAbRetainedRings() ||
        !hw.fAbRetainedRings[0].fwMayHold) return 2;
    /* Successful HW2SW is remembered; an MTT refusal remains retryable. */
    hw.hw2swOk=true; hw.mttFreeOk=false;
    if (hw.retryAbRetainedRings() || hw.hw2swCalls!=2 || hw.mttFreeCalls!=1 ||
        hw.icmFreeCalls || !hw.hasAbRetainedRings() ||
        hw.fAbRetainedRings[0].fwMayHold) return 3;
    hw.mttFreeOk=true;
    if (!hw.retryAbRetainedRings() || hw.hw2swCalls!=2 || hw.mttFreeCalls!=2 ||
        hw.icmFreeCalls!=1 || hw.hasAbRetainedRings() ||
        hw.fAbRetainedRings[0].icm) return 4;

    /* An unknown prior generation must not issue HW2SW or release anything. */
    Mlx4Hw stale; Mlx4Icm c{};
    keep(stale,1,&c,true,80,2,29,6);
    if (stale.retryAbRetainedRings() || stale.hw2swCalls || stale.mttFreeCalls ||
        stale.icmFreeCalls || !stale.hasAbRetainedRings()) return 5;

    /* A pre-handoff MTT refusal retries release without a device command. */
    Mlx4Hw pre; Mlx4Icm d{};
    keep(pre,0,&d,false,96,2,28,7); pre.mttFreeOk=false;
    if (pre.retryAbRetainedRings() || pre.hw2swCalls || pre.mttFreeCalls!=1 ||
        pre.icmFreeCalls || !pre.hasAbRetainedRings()) return 6;
    pre.mttFreeOk=true;
    if (!pre.retryAbRetainedRings() || pre.hw2swCalls || pre.mttFreeCalls!=2 ||
        pre.icmFreeCalls!=1 || pre.hasAbRetainedRings()) return 7;

    /* Retry in reverse allocation order; a failed B does not lose either owner. */
    Mlx4Hw pair; Mlx4Icm e{},f{};
    keep(pair,0,&e,false,64,2,28,7); keep(pair,1,&f,true,66,2,28,7);
    pair.mttFreeOk=false;
    if (pair.retryAbRetainedRings() || pair.hw2swCalls!=1 || pair.mttFreeCalls!=2 ||
        pair.icmFreeCalls || !pair.hasAbRetainedRings()) return 8;
    pair.mttFreeOk=true;
    if (!pair.retryAbRetainedRings() || pair.hw2swCalls!=1 || pair.mttFreeCalls!=4 ||
        pair.icmFreeCalls!=2 || pair.hasAbRetainedRings()) return 9;

    std::puts("A/B retained-ring retry: 9 behavior checks passed (production method)");
    return 0;
}
'''.replace('HAS_RETAINED', has).replace('RETRY_RETAINED', retry)

    with tempfile.TemporaryDirectory(prefix='cx3-ab-ring-retention-') as tmp:
        binary = str(Path(tmp) / 'test')
        subprocess.run(['/usr/bin/clang++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(SRC), '-x', 'c++', '-', '-o', binary],
                       input=harness, text=True, check=True)
        subprocess.run([binary], check=True)

    abring = function_source(hw_source, 'bool Mlx4Hw::eqAbExperiment(')
    stop = function_source(cx_source, 'void Cx3Mlx4::stop(')
    retained_stop_start = stop.index('if (fHw.hasAbRetainedRings()')
    retained_stop = stop[retained_stop_start:]
    retained_stop = retained_stop[:retained_stop.index('\n    }') + 6]
    checks = {
        'A/B retry runs before allocation preflight':
            abring.index('retryAbRetainedRings()') < abring.index('canRunAbExperiment('),
        'both failure cleanup branches retain full ring metadata':
            abring.count('Mlx4AbRingRetention &retained = fAbRetainedRings[k];') == 2 and
            all(token in abring for token in
                ('retained.icm = ring[k]', 'retained.mttOffset = mttOff[k]',
                 'retained.pages = pages', 'retained.eqn = eqn',
                 'retained.generation = ring[k]->generation.allocationGeneration',
                 'retained.fwMayHold = true', 'retained.fwMayHold = false')),
        'stop retains provider before A/B owner teardown can disappear':
            'fHw.hasAbRetainedRings()' in stop and
            stop.index('fHw.hasAbRetainedRings()') < stop.index('teardownEq("driver stopping"'),
        'stop retention pins self and provider':
            retained_stop.count('\n        retain();') == 1 and
            retained_stop.index('\n        retain();') < retained_stop.index('fStopRetained = true') and
            'fStopProvider->retain();' in retained_stop,
    }
    failed = [label for label, ok in checks.items() if not ok]
    if failed:
        for label in failed:
            print('FAIL:', label)
        raise SystemExit(1)
    print(f"A/B owner reachability source contract: {len(checks)} checks PASS")


if __name__ == '__main__':
    main()
