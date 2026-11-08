#!/usr/bin/env python3
"""Source contracts for retaining CQ/EQ owners after ambiguous create timeouts."""
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / 'src'


def body(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 0
    for pos in range(opening, len(text)):
        if text[pos] == '{':
            depth += 1
        elif text[pos] == '}':
            depth -= 1
            if depth == 0:
                return text[opening + 1:pos]
    raise AssertionError(f'unclosed function: {signature}')


def timeout_branch(text, command):
    marker = f'if (status == CX3_FWSTATUS_TIMEOUT)'
    start = text.index(marker, text.index(command))
    opening = text.index('{', start)
    depth = 0
    for pos in range(opening, len(text)):
        if text[pos] == '{':
            depth += 1
        elif text[pos] == '}':
            depth -= 1
            if depth == 0:
                return text[opening + 1:pos]
    raise AssertionError(f'unclosed timeout branch: {command}')


def failure_branches(text, marker):
    branches = []
    pos = 0
    while True:
        start = text.find(marker, pos)
        if start < 0:
            return branches
        opening = text.index('{', start)
        depth = 0
        for end in range(opening, len(text)):
            if text[end] == '{':
                depth += 1
            elif text[end] == '}':
                depth -= 1
                if depth == 0:
                    branches.append(text[opening + 1:end])
                    pos = end + 1
                    break
        else:
            raise AssertionError(f'unclosed branch: {marker}')


def main():
    hw = (SRC / 'Mlx4Hw.cpp').read_text()
    ctl = (SRC / 'Cx3Mlx4.cpp').read_text()
    failures = []

    expected = {
        'cqCreate': ['exposeRetainedOwner();'],
        'eqCreate': ['exposeRetainedOwner();'],
    }
    for name, assignments in expected.items():
        branch = timeout_branch(body(hw, f'bool Mlx4Hw::{name}('),
                                'MLX4_CMD_SW2HW_CQ' if name == 'cqCreate' else 'MLX4_CMD_SW2HW_EQ')
        for assignment in assignments:
            if assignment not in branch:
                failures.append(f'{name} timeout does not return retained owner field {assignment}')

    cq = body(ctl, 'IOReturn Cx3Mlx4::exportR4Cq(')
    if 'queue.cqUnknownRetained = true' not in cq or 'queue.cqMttOff = mttOffset' not in cq:
        failures.append('CQ timeout does not persist the retained owner and MTT metadata')
    if 'if (queue.cqUnknownRetained || queue.cqRing || queue.cqRec)' not in cq:
        failures.append('CQ creation does not refuse an existing unknown/retained owner')
    if 'if (ring && rec && pages != 0u)' not in cq or 'queue.cqMayBeDeviceOwned = fwStatus == CX3_FWSTATUS_TIMEOUT' not in cq:
        failures.append('CQ create failure does not persist returned owner and ownership state')
    cq_teardown = body(ctl, 'void Cx3Mlx4::cqTeardown(')
    if 'queue.cqMayBeDeviceOwned && !queue.cqHwReturned' not in cq_teardown:
        failures.append('CQ teardown may issue HW2SW_CQ when SW2HW_CQ was never issued')
    cq_create = body(hw, 'bool Mlx4Hw::cqCreate(')
    if cq_create.count('exposeRetainedOwner(); return false;') != 5:
        failures.append('CQ create rollback MTT refusals do not all return a reachable owner')

    eq = body(ctl, 'IOReturn Cx3Mlx4::exportM3EqUp(')
    if 'fEqUnknownRetained = true' not in eq or 'fEqMTTOffset = mttOffset' not in eq:
        failures.append('EQ timeout does not persist the retained owner and MTT metadata')
    if 'if (fEqRing && pages != 0u)' not in eq or 'fEqMayBeDeviceOwned = fwStatus == CX3_FWSTATUS_TIMEOUT' not in eq:
        failures.append('EQ create failure does not retain returned pre-handoff owners with their device-ownership state')
    eq_teardown = body(ctl, 'void Cx3Mlx4::teardownEq(')
    if 'fEqMayBeDeviceOwned && fEqn && !fEqHwReturned' not in eq_teardown:
        failures.append('EQ teardown may issue HW2SW_EQ when SW2HW_EQ was never issued')
    eq_create = body(hw, 'bool Mlx4Hw::eqCreate(')
    if eq_create.count('exposeRetainedOwner(); return false;') != 5:
        failures.append('EQ create rollback MTT refusals do not all return a reachable owner')
    if 'const auto exposeRetainedOwner' not in eq_create:
        failures.append('EQ create does not define retained-owner output helper')

    rq_create = body(hw, 'bool Mlx4Hw::rqCreate(')
    rq_release_refusals = failure_branches(rq_create, 'if (!mttFree(mttOffset, pages, ring))')
    if len(rq_release_refusals) != 4 or any(
            '*outRing = ring' not in branch or '*outBufs = bufs' not in branch or
            '*outPages = pages' not in branch or '*outMttOffset = mttOffset' not in branch
            for branch in rq_release_refusals):
        failures.append('RQ create MTT refusal does not return complete ring/buffer owner metadata')
    rq = body(ctl, 'IOReturn Cx3Mlx4::exportR4Rq(')
    if 'queue.rqUnknownRetained = true' not in rq or 'queue.rqMttOff = mttOffset' not in rq:
        failures.append('RQ create rollback does not persist its returned pre-handoff owner')
    if 'if (queue.rqUnknownRetained || queue.rqRing || queue.rqBufs)' not in rq:
        failures.append('RQ create does not gate reuse while an unknown owner is retained')
    rq_teardown = body(ctl, 'void Cx3Mlx4::rqTeardown(')
    if 'if (!queue.rqUp && !queue.rqUnknownRetained) return;' not in rq_teardown:
        failures.append('RQ teardown cannot retry a pre-handoff create rollback owner')

    sq_create = body(hw, 'bool Mlx4Hw::sqCreate(')
    sq_release_refusals = failure_branches(sq_create, 'if (!mttFree(mttOffset, pages, ring))')
    if len(sq_release_refusals) != 2 or any(
            '*outRing = ring' not in branch or '*outPages = pages' not in branch or
            '*outMttOffset = mttOffset' not in branch for branch in sq_release_refusals):
        failures.append('SQ create MTT refusal does not return complete ring owner metadata')
    tx = body(ctl, 'IOReturn Cx3Mlx4::exportR4Tx(')
    if 'queue.txSqUnknownRetained = true' not in tx or 'queue.txSqMttOff = sqMttOff' not in tx:
        failures.append('SQ create rollback does not persist its returned pre-handoff owner')
    if 'if (queue.txSqUnknownRetained)' not in tx:
        failures.append('TX setup does not reject an unavailable retained SQ owner')
    for signature in ['IOReturn Cx3Mlx4::exportM3EqRing(',
                      'IOReturn Cx3Mlx4::exportM3EqInfo(',
                      'IOReturn Cx3Mlx4::exportM3EqPoll(',
                      'IOReturn Cx3Mlx4::exportM3IrqUp(',
                      'IOReturn Cx3Mlx4::exportNetArm(']:
        if 'fEqUnknownRetained' not in body(ctl, signature):
            failures.append(f'{signature} does not refuse access to an unknown EQ owner')
    if 'if (fEqUnknownRetained)' not in cq:
        failures.append('CQ creation does not reject a potentially owned unknown EQ')

    if failures:
        for failure in failures:
            print('FAIL', failure)
        print(f'unknown owner path: {len(failures)} failure(s)')
        return 1
    print('unknown owner path: CQ/EQ timeout and RQ/SQ rollback retention contracts PASS')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
