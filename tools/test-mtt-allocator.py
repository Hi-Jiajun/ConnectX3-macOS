#!/usr/bin/env python3
"""Exercise the production MTT bump allocator's out-of-order release policy."""
from pathlib import Path
import subprocess
import tempfile

SRC = Path(__file__).resolve().parents[1] / 'src'


def main():
    harness = r'''
#include <cstdio>
#include <cstring>
#define CX3_HOST_TEST
#include "mlx4_pure.h"

int main() {
    Mlx4MttAllocator a;
    UInt32 eq=0, rq=0, cq=0, tx=0, refused=0;
    a.init(8);
    if (!a.allocate(2, 64, &eq) || !a.allocate(4, 64, &rq) ||
        !a.allocate(3, 64, &cq) || !a.allocate(5, 64, &tx)) return 1;
    if (eq!=8 || rq!=10 || cq!=14 || tx!=17 || a.high!=22) return 2;

    /* Stop order releases the EQ before its later queue allocations. */
    if (!a.release(eq, 2) || a.high!=22 || a.allocationCount!=4) return 3;
    if (!a.release(rq, 4) || a.high!=22 || a.allocationCount!=4) return 4;
    if (!a.release(tx, 5) || a.high!=17) return 5;
    /* Freeing the intervening CQ makes all released tail ranges coalesce. */
    if (!a.release(cq, 3) || a.high!=8 || a.allocationCount!=0) return 6;

    UInt32 reused=0;
    if (!a.allocate(6, 64, &reused) || reused!=8 || a.high!=14) return 7;
    if (a.release(7, 2) || a.high!=14) return 8;    /* reserved prefix */
    if (a.release(8, 0) || a.high!=14) return 9;    /* empty range */
    if (a.release(14, 1) || a.high!=14) return 10;  /* beyond high-water mark */
    if (a.release(9, 5) || a.high!=14) return 11;   /* partial live range */
    if (!a.release(reused, 6) || a.high!=8) return 12;
    if (a.release(reused, 6) || a.high!=8) return 13; /* duplicate / stale range */
    if (a.allocate(57, 64, &reused) || a.high!=8) return 14;

    a.init(0);
    UInt32 first=0, middle=0, last=0;
    if (!a.allocate(2, 16, &first) || !a.allocate(2, 16, &middle) ||
        !a.allocate(2, 16, &last)) return 15;
    if (!a.release(first, 2) || a.allocationCount!=3) return 16;
    if (a.release(first+1, 2) || a.high!=6 || a.allocationCount!=3) return 17;
    if (!a.release(last, 2) || !a.release(middle, 2) || a.high!=0 ||
        a.allocationCount!=0) return 18;

    a.init(0xfffffffeu);
    if (a.allocate(2, 0x1ffffffffull, &refused) || a.high!=0xfffffffeu ||
        a.allocationCount!=0) return 19; /* UInt32 high-water overflow */

    /* Exhausting the exact-allocation ledger refuses, rather than corrupting high. */
    a.init(0);
    UInt32 offsets[Mlx4MttAllocator::kAllocationMax];
    for (UInt32 i=0; i<Mlx4MttAllocator::kAllocationMax; ++i)
        if (!a.allocate(1, 512, &offsets[i])) return 20;
    if (a.allocate(1, 512, &refused) || a.high!=Mlx4MttAllocator::kAllocationMax) return 21;
    for (UInt32 i=0; i<Mlx4MttAllocator::kAllocationMax-1; ++i)
        if (!a.release(offsets[i], 1)) return 22;
    if (a.allocationCount!=Mlx4MttAllocator::kAllocationMax || a.high!=256) return 23;
    if (!a.release(offsets[Mlx4MttAllocator::kAllocationMax-1], 1) || a.high!=0 ||
        a.allocationCount!=0) return 24;

    Mlx4MttAllocator owned;
    int ringA=0, ringB=0;
    UInt32 ringRange=0;
    owned.init(8);
    if (!owned.allocateOwned(3, 64, &ringA, 101, 7, &ringRange) || ringRange!=8 ||
        owned.allocationCount!=1) return 25;
    if (!owned.canMarkOwnerHandoff(&ringA, 101, 7) ||
        owned.canMarkOwnerHandoff(&ringA, 102, 7) ||
        owned.canMarkOwnerHandoff(&ringA, 101, 8)) return 26;
    if (!owned.markOwnerHandoff(&ringA, 101, 7) ||
        owned.releaseOwned(8, 3, &ringB, 101, 7, 7)) return 27;
    /* The wrong identity must not free the allocation; all exact fields are checked. */
    if (owned.releaseOwned(8, 3, &ringB, 101, 7, 7) ||
        owned.releaseOwned(8, 3, &ringA, 102, 7, 7) ||
        owned.releaseOwned(8, 3, &ringA, 101, 8, 7) ||
        owned.releaseOwned(8, 3, &ringA, 101, 7, 0) ||
        owned.high!=11 || owned.allocationCount!=1) return 28;
    if (!owned.releaseOwned(8, 3, &ringA, 101, 7, 7) ||
        owned.high!=8 || owned.allocationCount!=0) return 29;

    /* The A/B experiment owns at most one exact, reusable dummy reservation. */
    Mlx4MttAllocator ab;
    int abRing=0;
    UInt32 dummy=0, dummyAgain=0, abOwned=0;
    ab.init(8);
    if (!ab.canRunAbExperiment(5,9) || !ab.canRunAbExperiment(0,9)) return 30;
    if (!ab.allocateAbReservation(5,64,9,&dummy) || dummy!=8 || ab.high!=13 ||
        ab.allocationCount!=1) return 31;
    if (!ab.canRunAbExperiment(5,9) || !ab.canRunAbExperiment(0,9)) return 32;
    if (!ab.allocateAbReservation(5,64,9,&dummyAgain) || dummyAgain!=dummy ||
        ab.high!=13 || ab.allocationCount!=1) return 33; /* no per-run ledger growth */
    if (ab.allocateAbReservation(6,64,9,&refused) || ab.high!=13 ||
        ab.allocationCount!=1 || ab.canRunAbExperiment(6,9)) return 34; /* one size per table generation */
    if (!ab.allocateOwned(2,64,&abRing,201,9,&abOwned) || abOwned!=13 ||
        ab.canRunAbExperiment(5,9) || ab.canRunAbExperiment(0,9)) return 35;
    if (ab.releaseAbReservationIfUnowned() || ab.high!=15 ||
        ab.allocationCount!=2) return 36; /* keep reservation while a DMA owner exists */
    if (ab.allocateAbReservation(5,64,10,&refused) || ab.high!=15 ||
        ab.allocationCount!=2 || ab.canRunAbExperiment(5,10)) return 37; /* generation drift cannot stack reservations */
    if (!ab.releaseOwned(abOwned,2,&abRing,201,9,0) ||
        !ab.releaseAbReservationIfUnowned() || ab.high!=8 ||
        ab.allocationCount!=0 || !ab.canRunAbExperiment(0,9)) return 38;
    /* Repeated safe A/B runs reclaim the reservation rather than exhausting 256 records. */
    for (UInt32 i=0; i<400; ++i) {
        UInt32 offset=0;
        if (!ab.allocateAbReservation(5,64,9,&offset) || offset!=8 ||
            !ab.releaseAbReservationIfUnowned() || ab.high!=8 ||
            ab.allocationCount!=0) return 39;
    }
    /* A live DMA owner prevents release; once it is gone, table replacement can reclaim it. */
    if (!ab.allocateAbReservation(5,64,9,&dummy) ||
        !ab.allocateOwned(1,64,&abRing,202,9,&abOwned)) return 40;
    if (ab.releaseAbReservationIfUnowned() || ab.high!=14 ||
        ab.allocationCount!=2 || ab.canRunAbExperiment(5,9)) return 41;
    if (!ab.releaseOwned(abOwned,1,&abRing,202,9,0) ||
        !ab.releaseAbReservationIfUnowned() || ab.high!=8 ||
        ab.allocationCount!=0) return 42;

    std::puts("mtt allocator: 42 checks passed (production helper; includes 400 reserve/reclaim cycles)");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='cx3-mtt-allocator-') as tmp:
        binary = str(Path(tmp) / 'test')
        subprocess.run(['/usr/bin/clang++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(SRC), '-x', 'c++', '-', '-o', binary],
                       input=harness, text=True, check=True)
        subprocess.run([binary], check=True)

    hw_source = (SRC / 'Mlx4Hw.cpp').read_text()
    cx_source = (SRC / 'Cx3Mlx4.cpp').read_text()
    dummy_start = hw_source.index('bool Mlx4Hw::mttAllocAbDummy(')
    dummy_alloc = hw_source[dummy_start:hw_source.index('\n}\n', dummy_start) + 3]
    setter_start = hw_source.index('bool Mlx4Hw::setMttTable(')
    setter = hw_source[setter_start:hw_source.index('\n}\n', setter_start) + 3]
    bringup_start = cx_source.index('IOReturn Cx3Mlx4::exportM2dBringUp(')
    bringup = cx_source[bringup_start:cx_source.index('\n}\n', bringup_start) + 3]
    checks = {
        'MTT allocation records bind exact DMA owner and generation':
            all(token in (SRC / 'mlx4_pure.h').read_text() for token in
                ('ownerIdentity', 'ownerAllocationId', 'allocationGeneration', 'ownerGeneration')),
        'MTT release requires exact owner identity and generation':
            'bool releaseOwned(' in (SRC / 'mlx4_pure.h').read_text(),
        'production mttFree returns refusal to its caller':
            'bool Mlx4Hw::mttFree(' in hw_source,
        'handoff propagates generation to the matching MTT owner':
            'fMttAllocator.markOwnerHandoff(' in hw_source,
        'table view setter refuses live allocation owners before replacing the view':
            setter.index('fMttAllocator.allocationCount != 0') < setter.index('fMttBase     = hostBase'),
        'table view setter attempts safe dummy-reservation reclaim before the live-owner guard':
            setter.index('releaseAbReservationIfUnowned()') <
            setter.index('fMttAllocator.allocationCount != 0'),
        'table view setter rejects malformed base/size/reserved prefix':
            'if (!hostBase || bytes < MLX4_MTT_ENTRY_SIZE' in setter,
        'bringup refuses outstanding MTT owners before SET_ICM_SIZE':
            bringup.index('fHw.hasMttAllocations()') < bringup.index('fHw.setIcmSize('),
        'bringup checks table view installation before INIT_HCA':
            bringup.index('fHw.setMttTable(') < bringup.index('fHw.initHca('),
    }
    failed = [label for label, ok in checks.items() if not ok]
    if failed:
        for label in failed:
            print('FAIL:', label)
        raise SystemExit(1)
    print(f"MTT table lifetime source contract: {len(checks)} checks PASS")

    dummy_harness = r'''
#include <cstdio>
#include <cstring>
#define CX3_HOST_TEST
#include "mlx4_pure.h"
static void sink(const char*, ...) {}
#define LOG(...) sink(__VA_ARGS__)
class Mlx4Hw {
public:
    UInt64 fDeviceGeneration=9, fMttBytes=512;
    void *fMttBase=(void*)1;
    Mlx4MttAllocator fMttAllocator;
    bool mttAllocAbDummy(UInt32,UInt32*,UInt32*);
};
DUMMY_ALLOC
int main() {
    Mlx4Hw hw; UInt32 offset=0,addr=0,again=0;
    hw.fMttAllocator.init(8);
    if (!hw.mttAllocAbDummy(5,&offset,&addr) || offset!=8 || addr!=64) return 1;
    if (!hw.mttAllocAbDummy(5,&again,&addr) || again!=offset ||
        hw.fMttAllocator.allocationCount!=1 || hw.fMttAllocator.high!=13) return 2;
    if (hw.mttAllocAbDummy(6,&again,&addr) || hw.fMttAllocator.high!=13) return 3;
    hw.fDeviceGeneration=10;
    if (hw.mttAllocAbDummy(5,&again,&addr) || hw.fMttAllocator.high!=13) return 4;
    hw.fMttBytes=0;
    if (hw.mttAllocAbDummy(5,&again,&addr) || hw.fMttAllocator.high!=13) return 5;
    std::puts("production mttAllocAbDummy: reuse and mismatch guards passed");
    return 0;
}
'''.replace('DUMMY_ALLOC', dummy_alloc)
    with tempfile.TemporaryDirectory(prefix='cx3-ab-dummy-') as tmp:
        binary = str(Path(tmp) / 'ab-dummy-test')
        subprocess.run(['/usr/bin/clang++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(SRC), '-x', 'c++', '-', '-o', binary],
                       input=dummy_harness, text=True, check=True)
        subprocess.run([binary], check=True)

    ab_start = hw_source.index('bool Mlx4Hw::eqAbExperiment(')
    abring = hw_source[ab_start:hw_source.index('\n}\n', ab_start) + 3]
    cleanup_start = abring.index('for (r = 2; r > 0; --r)')
    cleanup_reclaim = abring.rfind('releaseAbReservationIfUnowned()')
    if ('mttAllocAbDummy(dummyMtt, &dummyOff, &dummyAddr)' not in abring or
        'releaseAbReservationIfUnowned()' not in abring or
        'canRunAbExperiment(dummyMtt, fDeviceGeneration)' not in abring or
        abring.index('canRunAbExperiment(dummyMtt, fDeviceGeneration)') >
        abring.index('mttAllocAbDummy(dummyMtt, &dummyOff, &dummyAddr)') or
        cleanup_reclaim < cleanup_start):
        raise SystemExit('A/B experiment must preflight an exclusive MTT layout, track its dummy and reclaim only after ring cleanup')
    print('A/B experiment reservation lifecycle source contract: 5 checks PASS')

    mtt_free_start = hw_source.index('bool Mlx4Hw::mttFree(')
    mtt_free = hw_source[mtt_free_start:hw_source.index('\n}\n', mtt_free_start) + 3]
    mtt_free_harness = r'''
#include <cstdio>
#include <cstring>
#define CX3_HOST_TEST
#include "mlx4_pure.h"
static void sink(const char*, ...) {}
#define LOG(...) sink(__VA_ARGS__)
struct Mlx4Icm { UInt64 allocationId; Mlx4DmaGeneration generation; };
class Mlx4Hw {
public:
    UInt32 fMttDummy=0;
    Mlx4MttAllocator fMttAllocator;
    bool mttFree(UInt32,UInt32,Mlx4Icm*);
};
MTT_FREE
int main() {
    Mlx4Hw hw; Mlx4Icm a{},b{}; UInt32 off=0;
    a.allocationId=101; a.generation.init(7);
    b.allocationId=102; b.generation.init(7);
    hw.fMttAllocator.init(8);
    if (!hw.fMttAllocator.allocateOwned(3,64,&a,a.allocationId,7,&off) || off!=8) return 1;
    if (!hw.fMttAllocator.markOwnerHandoff(&a,a.allocationId,7) ||
        !a.generation.markHandoffAttempted(7)) return 2;
    if (hw.mttFree(off,3,&b) || hw.fMttAllocator.high!=11 ||
        hw.fMttAllocator.allocationCount!=1) return 3;
    a.allocationId=103;
    if (hw.mttFree(off,3,&a) || hw.fMttAllocator.high!=11) return 4;
    a.allocationId=101;
    a.generation.ownerGeneration=8;
    if (hw.mttFree(off,3,&a) || hw.fMttAllocator.high!=11) return 5;
    a.generation.ownerGeneration=7;
    if (!hw.mttFree(off,3,&a) || hw.fMttAllocator.high!=8 ||
        hw.fMttAllocator.allocationCount!=0) return 6;
    std::puts("production mttFree: exact owner/generation refusal and release passed");
    return 0;
}
'''.replace('MTT_FREE', mtt_free)
    with tempfile.TemporaryDirectory(prefix='cx3-mtt-free-') as tmp:
        binary = str(Path(tmp) / 'mtt-free-test')
        subprocess.run(['/usr/bin/clang++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-I', str(SRC), '-x', 'c++', '-', '-o', binary],
                       input=mtt_free_harness, text=True, check=True)
        subprocess.run([binary], check=True)

    setter_harness = r'''
#include <cstdio>
#include <cstring>
#define CX3_HOST_TEST
#include "mlx4_pure.h"
static void sink(const char*, ...) {}
#define LOG(...) sink(__VA_ARGS__)
class Mlx4Hw {
public:
    void *fMttBase=nullptr; UInt64 fMttBytes=0,fMttHcaVirt=0;
    UInt32 fMttReserved=0; Mlx4MttAllocator fMttAllocator;
    bool setMttTable(void*,UInt64,UInt32,UInt64);
};
SETTER
int main() {
    UInt8 oldTable[4096]{},newTable[4096]{}; Mlx4Hw hw; UInt32 off=0;
    if (!hw.setMttTable(oldTable,sizeof(oldTable),8,0x1000) || hw.fMttAllocator.high!=8) return 1;
    if (!hw.fMttAllocator.allocate(2,512,&off) || off!=8) return 2;
    if (hw.setMttTable(newTable,sizeof(newTable),4,0x2000) || hw.fMttBase!=oldTable ||
        hw.fMttAllocator.high!=10) return 3;
    if (!hw.fMttAllocator.release(off,2) ||
        !hw.setMttTable(newTable,sizeof(newTable),4,0x2000) || hw.fMttBase!=newTable ||
        hw.fMttAllocator.high!=4) return 4;
    if (hw.setMttTable(nullptr,sizeof(newTable),4,0x3000) || hw.fMttBase!=newTable) return 5;
    hw.fMttAllocator.init(4);
    if (!hw.fMttAllocator.allocateAbReservation(3,512,9,&off) || off!=4) return 6;
    if (!hw.setMttTable(newTable,sizeof(newTable),6,0x3000) ||
        hw.fMttBase!=newTable || hw.fMttAllocator.high!=6 ||
        hw.fMttAllocator.allocationCount!=0) return 7;
    std::puts("MTT table setter: 7 behavior checks passed");
    return 0;
}
'''
    setter_harness = setter_harness.replace('SETTER', setter)
    with tempfile.TemporaryDirectory(prefix='cx3-mtt-setter-') as tmp:
        binary = str(Path(tmp) / 'setter-test')
        subprocess.run(['/usr/bin/clang++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(SRC), '-x', 'c++', '-', '-o', binary],
                       input=setter_harness, text=True, check=True)
        subprocess.run([binary], check=True)


if __name__ == '__main__':
    main()
