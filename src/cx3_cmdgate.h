/*
 * Part of cx3-mlx4, a macOS IOKit port of the FreeBSD Mellanox driver.
 *
 * Original work in this repository.  It cites no upstream file:line and is
 * derived from none; the licence in LICENSE governs it.
 *
 * Copyright (c) 2026 Hi-Jiajun and contributors
 */

/*
 * cx3_cmdgate.h -- the decision a command path makes at every step of a wait, as a
 * pure function of its inputs.
 *
 * Why this is a header and not three lines inside Mlx4Hw.cpp: the failure this
 * separates out is "a command was abandoned and the mailbox was poisoned", and it is
 * the first thing in this driver whose *timing* is the defect (docs/LOCK-DESIGN.md
 * section 1.4: one selector held the transaction lock for 6 min 07.6 s).  A timing bug
 * cannot be reproduced on the host, but the *decision* that ends the wait can be, and
 * tools/test-cmd-wedge.c asserts all four transitions on every build.  Code that only
 * ever runs against hardware gets tested once per reboot.
 *
 * Nothing here touches IOKit, allocates, or reads a clock: the caller supplies "now".
 *
 * The three states the gate distinguishes, and why they must not be conflated:
 *
 *   IDLE       the register reports nothing outstanding.  For the pre-post poll that
 *              means "post the command"; for the completion poll it means "it finished".
 *   SPIN       something is still outstanding and the deadline has not passed.
 *   ABANDON    the deadline passed while the register still reported an outstanding
 *              command.  The caller gives up AND poisons the mailbox, because a command
 *              whose GO bit is set may still be being read by the device and its
 *              completion may still arrive (docs/LOCK-DESIGN.md section 3.3).
 *   FAIL_FAST  the mailbox is already poisoned: do not read the register, do not fill the
 *              mailbox, do not post.  This is a *state*, not an outcome of waiting, and
 *              it is checked before anything else so that a poisoned driver fails in
 *              microseconds instead of posting over a stuck command.
 */
#ifndef CX3_CMD_GATE_H
#define CX3_CMD_GATE_H

#ifdef CX3_HOST_TEST
/* Host build: the same widths, without the kernel headers. */
#include <stdint.h>
typedef uint8_t  UInt8;
typedef uint16_t UInt16;
typedef uint32_t UInt32;
typedef uint64_t UInt64;
#else
#include <IOKit/IOTypes.h>
#endif

#include "mlx4_regs.h"

enum {
    CX3_CMD_GATE_FAIL_FAST = 0,   /* poisoned: refuse without touching the register */
    CX3_CMD_GATE_IDLE      = 1,   /* nothing outstanding: post, or the command is done */
    CX3_CMD_GATE_SPIN      = 2,   /* outstanding, budget remains */
    CX3_CMD_GATE_ABANDON   = 3    /* outstanding, budget gone: abandon and poison */
};

/*
 * The deadline, in nanoseconds, that a budget of `ms` milliseconds from `nowNs` lands on.
 *
 * This one-line function is the whole difference between the old timeouts and these:
 * the old poll counted iterations of {register read + IOSleep(1)} and compared that count
 * against a constant named "MSECS", and one iteration costs about 2.1-2.3 ms on this
 * machine (measured, docs/LOCK-DESIGN.md section 1.3).  A budget that is a *time* is
 * converted once, here, and then compared against a clock that measures time.
 */
static inline UInt64 cx3CmdDeadlineNs(UInt64 nowNs, UInt32 budgetMs)
{
    return nowNs + (UInt64)budgetMs * 1000000ull;
}

/* Milliseconds between two readings of the same clock, for the log lines. */
static inline UInt32 cx3CmdElapsedMs(UInt64 startNs, UInt64 nowNs)
{
    if (nowNs < startNs) return 0u;
    return (UInt32)((nowNs - startNs) / 1000000ull);
}

/*
 * The decision.  All four arguments are facts the caller already has; no register is
 * read here, so "what do we do next" can be tested without a device.
 *
 * `wedged` is the mailbox poison flag and is checked first and unconditionally: it does
 * not expire, it is not cleared by waiting, and only a device reset clears it (that is
 * what makes it a poison rather than a backoff).
 */
static inline UInt32 cx3CmdGate(UInt32 wedged, UInt32 pending,
                                UInt64 nowNs, UInt64 deadlineNs)
{
    if (wedged)                  return CX3_CMD_GATE_FAIL_FAST;
    if (!pending)                return CX3_CMD_GATE_IDLE;
    if (nowNs >= deadlineNs)     return CX3_CMD_GATE_ABANDON;
    return CX3_CMD_GATE_SPIN;
}

#endif /* CX3_CMD_GATE_H */
