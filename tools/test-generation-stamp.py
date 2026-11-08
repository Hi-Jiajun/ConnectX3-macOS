#!/usr/bin/env python3
"""Check generation stamps are created and recorded before device handoff."""
from pathlib import Path
import subprocess
import tempfile

SRC = Path(__file__).resolve().parents[1] / 'src'


def method(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


def main():
    hw = (SRC / 'Mlx4Hw.cpp').read_text()
    cx = (SRC / 'Cx3Mlx4.cpp').read_text()
    flr = (SRC / 'flr.inc').read_text()
    alloc = method(hw, 'Mlx4Icm *Mlx4Hw::icmAlloc(')
    mapping = method(hw, 'bool Mlx4Hw::mapCmd(')
    mr_enable = method(hw, 'bool Mlx4Hw::mrEnablePhysical(')
    reset = method(cx, 'IOReturn Cx3Mlx4::exportResetHca(')
    flr_entry = method(flr, 'IOReturn Cx3Mlx4::exportFlr(')
    cq = method(hw, 'bool Mlx4Hw::cqCreate(')
    eq = method(hw, 'bool Mlx4Hw::eqCreate(')
    qp_rx = method(cx, 'IOReturn Cx3Mlx4::exportR4Qp(')
    qp_ingress = method(cx, 'IOReturn Cx3Mlx4::exportR4Ingress(')
    qp_tx = method(cx, 'IOReturn Cx3Mlx4::exportR4Tx(')
    mr = method(cx, 'IOReturn Cx3Mlx4::exportR4Mr(')
    abring = method(hw, 'bool Mlx4Hw::eqAbExperiment(')
    checks = {
        'allocation is stamped from bound generation': 'icm->generation.init(fDeviceGeneration);' in alloc,
        'unbound generation rejected before mailbox allocation': mapping.index('!fDeviceGeneration') < mapping.index('mboxAlloc()'),
        'handoff stamped before first MAP command': mapping.index('markHandoffAttempted') < mapping.index('cmdBox('),
        'HCA reset advances generation before reset attempt': reset.index('++fDeviceGeneration;') < reset.index('fHw.resetHca('),
        'FLR advances generation before platform reset': flr_entry.index('++fDeviceGeneration;') < flr_entry.index('fHw.platformFunctionReset('),
        'quarantine snapshots generation': 'q->generation = icm->generation;' in cx,
        'retained ICM ledger snapshots generation': 'fIcmKept[fIcmKeptCount].generation = icm->generation;' in cx,
        'quarantine overflow snapshots generation': 'fQuarOverflowGeneration[fQuarOverflowCount] = icm->generation;' in cx,
        'CQ ring and record stamped before SW2HW': cq.index('markDmaHandoff') < cq.index('MLX4_CMD_SW2HW_CQ'),
        'EQ ring stamped before SW2HW': eq.index('markDmaHandoff') < eq.index('MLX4_CMD_SW2HW_EQ'),
        'CQ stamp rejection returns before SW2HW': cq.index('if (!markDmaHandoff') < cq.index('MLX4_CMD_SW2HW_CQ'),
        'EQ stamp rejection returns before SW2HW': eq.index('if (!markDmaHandoff') < eq.index('MLX4_CMD_SW2HW_EQ'),
        'RX QP DMA set stamped before qpModify': qp_rx.index('markDmaHandoff') < qp_rx.index('fHw.qpModify('),
        'RSS QP DMA set stamped before qpModify': qp_ingress.index('markDmaHandoff') < qp_ingress.index('fHw.qpModify('),
        'TX QP DMA set stamped before qpModify': qp_tx.index('markDmaHandoff') < qp_tx.index('fHw.qpModify('),
        'RX QP stamp rejection returns before transition': qp_rx.index('if (!fHw.markDmaHandoff') < qp_rx.index('fHw.qpModify('),
        'RSS stamp rejection returns before transition': qp_ingress.index('if (!fHw.markDmaHandoff') < qp_ingress.index('fHw.qpModify('),
        'TX stamp rejection returns before transition': qp_tx.index('if (!fHw.markDmaHandoff') < qp_tx.index('fHw.qpModify('),
    }
    mr_handoff = mr.find('fMrGeneration.init(fDeviceGeneration)')
    mr_command = mr.find('fHw.mrEnablePhysical(')
    mr_reject = mr.find('&fMrGeneration, &fwStatus')
    checks['MR generation stamped before SW2HW_MPT'] = mr_handoff >= 0 and mr_command >= 0 and mr_handoff < mr_command
    checks['MR generation descriptor passed into command adapter'] = mr_reject >= mr_command and mr_command >= 0
    checks['MR stamp is initialized from current provider generation'] = mr.find('fMrGeneration.init(fDeviceGeneration)') >= 0
    checks['MR handoff marker is at MPT command boundary'] = mr_enable.find('markResourceHandoff(owner)') >= 0 and mr_enable.find('markResourceHandoff(owner)') < mr_enable.find('MLX4_CMD_SW2HW_MPT')
    checks['MR refuses MPT command when handoff marker fails'] = mr_enable.find('if (!markResourceHandoff(owner))') >= 0 and mr_enable.find('if (!markResourceHandoff(owner))') < mr_enable.find('MLX4_CMD_SW2HW_MPT')
    ab_handoff = abring.find('markDmaHandoff(handoff, 1)')
    ab_cmd = abring.find('MLX4_CMD_SW2HW_EQ')
    ab_uncertain_owner = abring.find('fwMayHold[handed] = true;')
    checks['experimental EQ ring marked before SW2HW'] = ab_handoff >= 0 and ab_cmd >= 0 and ab_handoff < ab_cmd
    checks['experimental EQ becomes possibly device-owned before SW2HW'] = ab_uncertain_owner >= 0 and ab_cmd >= 0 and ab_uncertain_owner < ab_cmd
    failed = [name for name, ok in checks.items() if not ok]
    if failed:
        for name in failed:
            print('FAIL:', name)
        return 1

    mark_method = method(hw, 'bool Mlx4Hw::markDmaHandoff(')
    resource_method = method(hw, 'bool Mlx4Hw::markResourceHandoff(')
    pure = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#define CX3_HOST_TEST
#include "mlx4_pure.h"
struct Mlx4Icm { UInt64 allocationId=0; Mlx4DmaGeneration generation; };
class Mlx4Hw {
public:
    UInt64 fDeviceGeneration=8;
    Mlx4MttAllocator fMttAllocator;
    bool markDmaHandoff(Mlx4Icm *const*, UInt32);
    bool markResourceHandoff(Mlx4DmaGeneration*);
};
MARK_METHOD
RESOURCE_METHOD
int main() {
    Mlx4DmaGeneration stamp{};
    stamp.init(7);
    if (!stamp.validAllocation() || stamp.mayBeDeviceOwned) return 1;
    if (stamp.markHandoffAttempted(0) || stamp.mayBeDeviceOwned) return 2;
    if (!stamp.markHandoffAttempted(8) || !stamp.matchesOwnerGeneration(8)) return 3;
    if (stamp.matchesOwnerGeneration(7)) return 4;
    Mlx4Hw hw; Mlx4Icm a{}, b{}; a.allocationId=101; b.allocationId=102;
    a.generation.init(2); b.generation.init(3);
    Mlx4Icm *both[]={&a,&b};
    if (!hw.markDmaHandoff(both,2) || !a.generation.matchesOwnerGeneration(8) ||
        !b.generation.matchesOwnerGeneration(8)) return 5;
    Mlx4Icm untouched{}; untouched.allocationId=103; untouched.generation.init(4);
    Mlx4Icm *bad[]={&untouched,nullptr};
    if (hw.markDmaHandoff(bad,2) || untouched.generation.mayBeDeviceOwned ||
        untouched.generation.allocationGeneration != 4) return 6;
    Mlx4DmaGeneration mr{}; mr.init(8);
    if (!hw.markResourceHandoff(&mr) || !mr.matchesOwnerGeneration(8)) return 7;
    Mlx4DmaGeneration invalid{}; invalid.init(0);
    if (hw.markResourceHandoff(&invalid) || invalid.mayBeDeviceOwned) return 8;
    if (hw.markResourceHandoff(nullptr)) return 9;
    hw.fDeviceGeneration=0;
    if (hw.markDmaHandoff(both,2) || hw.markResourceHandoff(&mr)) return 10;
    std::puts("generation stamp model: allocation, handoff and mismatch PASS");
    return 0;
}
'''
    pure = pure.replace('MARK_METHOD', mark_method).replace('RESOURCE_METHOD', resource_method)
    with tempfile.TemporaryDirectory(prefix='cx3-generation-stamp-') as tmp:
        binary = str(Path(tmp) / 'test')
        subprocess.run(['/usr/bin/clang++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(SRC), '-x', 'c++', '-', '-o', binary],
                       input=pure, text=True, check=True)
        subprocess.run([binary], check=True)
    print(f'generation source contract: {len(checks)} checks PASS')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
