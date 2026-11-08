#!/usr/bin/env python3
"""Lock the network stop ordering against outputPacket's asynchronous TX owner.

This is a source-contract check, not a runtime proof that the networking family
has drained callbacks after detachInterface returns.
"""
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / "src" / "Cx3Mlx4.cpp"


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    open_brace = text.index("{", start)
    depth = 0
    for pos in range(open_brace, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace + 1 : pos]
    raise AssertionError(f"unterminated function: {signature}")


def ordered(body: str, *needles: str) -> bool:
    positions = [body.find(needle) for needle in needles]
    return all(pos >= 0 for pos in positions) and positions == sorted(positions)


def main() -> int:
    text = SOURCE.read_text()
    stop = function_body(text, "void Cx3Mlx4::stop(IOService *provider)")
    service_stop = function_body(text, "bool Cx3Mlx4::netServiceStop(const char *why)")
    timer_release = function_body(text, "bool Cx3Mlx4::netTimerRelease(const char *why)")
    output = function_body(text, "UInt32 Cx3Mlx4::outputPacket(mbuf_t pkt, void *param)")
    irq_release = function_body(text, "bool Cx3Mlx4::irqReleaseObjects(")
    irq_remove_failure = irq_release[irq_release.index("if (removeRc != kIOReturnSuccess)") :]
    detach_wait = function_body(text, "bool Cx3Mlx4::netDetachInterfacesAndWait(UInt32 waitMs)")
    detach_worker = function_body(text, "void Cx3Mlx4::netDetachThread(")
    detach_timeout = stop[stop.index(
        "if (disposition.settleAttach() && !netDetachInterfacesAndWait(2000u))"):]
    tx_timeout = stop[stop.index("if (!fNetTxAdmission.drained())"):]
    checks = {
        "close output before synchronous interface detach": ordered(
            stop,
            "fNetEnabled = false;",
            "netDetachInterfacesAndWait(2000u)",
        ),
        "detach interface before stopping/freeing TX path": ordered(
            stop,
            "netDetachInterfacesAndWait(2000u)",
            "while (!fNetTxAdmission.drained() && waitedMs < 2000u)",
            'if (!netServiceStop("driver stopping")',
            'teardownEq("driver stopping", false);',
        ),
        "close callback admission before waiting for selectors": ordered(
            stop,
            "fNetTxAdmission.close()",
            "beginStopAndDrain(65000)",
        ),
        "output callback admission precedes all TX-state reads and has RAII leave": ordered(
            output,
            "if (!pkt)",
            "if (!fNetTxAdmission.enter())",
            "txAdmissionExit = { &fNetTxAdmission };",
            "if (!fNetEnabled || !fNetTxRing",
        ) and "~TxAdmissionExit() { gate->leave(); }" in output,
        "stop detach timeout pins resources before teardown": ordered(
            detach_timeout,
            "if (disposition.settleAttach() && !netDetachInterfacesAndWait(2000u))",
            "retain();",
            "fStopRetained = true;",
            'if (!netServiceStop("driver stopping")',
        ),
        "detach worker performs synchronous family termination off stop thread": (
            "self->detachInterface(ifaces[i], /*sync=*/true);" in detach_worker
            and "thread_call_enter(fNetDetachCall)" in detach_wait
            and ordered(detach_wait,
                        "thread_call_enter(fNetDetachCall)",
                        "IOLockSleepDeadline(fNetAttachLock, &fNetDetachDone",
                        "const bool completed = fNetDetachDone")
        ),
        "detach worker releases interface refs only after synchronous termination": ordered(
            detach_worker,
            "self->detachInterface(ifaces[i], /*sync=*/true);",
            "for (j = 0; j < refs[i]; ++j) ifaces[i]->release();",
            "self->fNetDetachDone = true;",
        ),
        "drain timeout pins self/provider and returns before teardown": (
            "if (!fNetTxAdmission.drained())" in stop
            and ordered(
                tx_timeout,
                "if (!fNetTxAdmission.drained())",
                "retain();",
                "fStopRetained = true;",
                "super::stop(provider);",
            )
        ),
        "device DMA is not inferred from family callback drainage": (
            "This is CPU callback drainage, not proof that device DMA" in stop
            and "Neither this lock nor family termination proves device DMA quiescence." in service_stop
        ),
        "cancel/remove timer before TX drain": ordered(
            timer_release,
            "fNetTimer->cancelTimeout();",
            "fWorkLoop->removeEventSource(fNetTimer)",
            "fNetTimer->release();",
        ) and ordered(service_stop, "netTimerRelease(why)", "IOLockLock(fNetTxLock);"),
        "TX lock held while scanning in-flight mbufs": ordered(
            service_stop,
            "IOLockLock(fNetTxLock);",
            "for (i = 0; i < (UInt32)kNetTxSlots; ++i) if (fNetTxMbuf[i]) ++n;",
            "for (i = 0; i < (UInt32)kNetTxSlots; ++i) {",
        ),
        "TX ring release occurs before unlocking": ordered(
            service_stop,
            "IOLockLock(fNetTxLock);",
            'releaseIcmOrKeep(&fNetTxRing, "network transmit frame ring",',
            "IOLockUnlock(fNetTxLock);",
        ),
        "TX lock is freed only after the drain unlock": ordered(
            service_stop,
            "IOLockUnlock(fNetTxLock);",
            "IOLockFree(fNetTxLock);",
        ),
        "stop removes IRQ source before releasing network and device queues": ordered(
            stop,
            'if (!irqReleaseObjects("driver stopping"))',
            'if (!netServiceStop("driver stopping")',
            'teardownEq("driver stopping", false);',
        ),
        "IRQ removal failure leaves source owner and release state intact": (
            "if (removeRc != kIOReturnSuccess)" in irq_release
            and ordered(irq_remove_failure,
                        "return false;",
                        "fIrqSource->release();",
                        "fIrqSource = 0;",
                        "fIrqObjectsUp = false;")
        ),
    }
    failures = [name for name, passed in checks.items() if not passed]
    for name, passed in checks.items():
        print(f"{'PASS' if passed else 'FAIL'} {name}")
    print(f"{len(checks) - len(failures)}/{len(checks)} source-order checks passed")
    print("LIMIT: interface/IRQ barriers rely on SDK contracts; timer action also uses the stop admission gate; device DMA remains a separate proof")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
