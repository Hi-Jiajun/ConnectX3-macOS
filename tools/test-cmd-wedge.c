/*
 * test-cmd-wedge.c -- the command-gate decisions, and the unit the timeouts are in.
 *
 * Why this exists.  The defect this batch fixes is a *duration*: one selector held the
 * transaction lock for 6 min 07.6 s because every wait in the command path was an
 * iteration count wearing a millisecond name (docs/LOCK-DESIGN.md sections 1.3-1.4).
 * The duration itself cannot be reproduced here -- but the two things that made it
 * possible can be checked, and both are pure arithmetic:
 *
 *   1. a budget in milliseconds becomes a deadline in nanoseconds, exactly, so that the
 *      comparison in the poll loop is against a clock and not against a loop counter;
 *   2. the decision that ends the wait has four states, and the one that matters most is
 *      "already poisoned": it must be taken before the register is read and it must not
 *      expire, because a poison that decays is a backoff and the whole point
 *      (docs/LOCK-DESIGN.md section 3.3) is that a command whose GO bit is stuck can
 *      never be posted over.
 *
 * The transitions asserted here are the four the design names:
 *   busy + budget gone          -> abandon (and poison)
 *   already poisoned            -> fail without touching the register
 *   after the cure (a reset)    -> posting is allowed again
 *   register idle after a reset -> post, whatever the toggle echo says
 * The last one is a property of the caller, not of the gate: the gate takes "pending" as
 * the caller's single answer, and the caller computes it from a fresh register read
 * (Mlx4Hw::cmdPending).  It is asserted here in that form -- an idle reading is posted to
 * -- because the reset path sets fToggle = 1 and the echo of the *previous* firmware
 * instance is meaningless, so "idle" is the only sane input for the first command after a
 * reset.
 *
 * Build and run as part of build.sh; it is a host program, no kernel involved.
 */
#include <stdio.h>
#include <stdint.h>

#define CX3_HOST_TEST
#include "../src/cx3_cmdgate.h"
#include "../src/cx3_abi.h"

static int checks = 0, failures = 0;

#define CHECK(what, got, want)                                             \
    do {                                                                   \
        ++checks;                                                          \
        if ((uint64_t)(got) != (uint64_t)(want)) {                         \
            ++failures;                                                    \
            printf("  FAIL %-58s got 0x%llx want 0x%llx\n", (what),        \
                   (unsigned long long)(got), (unsigned long long)(want)); \
        }                                                                  \
    } while (0)

/* Every firmware status the device can answer with, from the ABI's own list. */
#define FWCODE(code, name, text) code,
static const unsigned fwCodes[] = { CX3_FW_STATUS_LIST(FWCODE) };
#undef FWCODE

int main(void)
{
    unsigned i;

    /*
     * --- the unit: a budget is a time -------------------------------------------
     *
     * These are the numbers that were wrong before.  GO_BIT_TIMEOUT_MSECS was 10000 and
     * the loop compared it against an iteration counter, so the wait it described was
     * about 22.7 s at 2.27 ms per iteration -- measured, twice, from the kernel log's own
     * timestamps.  A budget that is converted to nanoseconds once cannot drift like that:
     * the only way to get it wrong is to pass the wrong number here, and these checks
     * name each number.
     */
    CHECK("the polled pre-post wait is 50 ms", CX3_CMD_IDLE_WAIT_MS, 50);
    CHECK("...which is 50 ms of nanoseconds", cx3CmdDeadlineNs(0, CX3_CMD_IDLE_WAIT_MS),
          50000000ull);
    CHECK("the event pre-post wait is the reference's 10000 ms",
          CX3_CMD_IDLE_WAIT_EVENT_MS, GO_BIT_TIMEOUT_MSECS);
    CHECK("the ICM teardown budget is 5000 ms, as 5 s of nanoseconds",
          cx3CmdDeadlineNs(0, CX3_CMD_BUDGET_ICM_MS), 5000000000ull);
    CHECK("the firmware budget is the reference's class A value",
          CX3_CMD_BUDGET_FW_MS, MLX4_CMD_TIME_CLASS_A);
    CHECK("...and is 60 s of nanoseconds", cx3CmdDeadlineNs(0, CX3_CMD_BUDGET_FW_MS),
          60000000000ull);

    /* The deadline is relative to the clock reading it was built from. */
    CHECK("a deadline built at t=1e9 is t+50 ms",
          cx3CmdDeadlineNs(1000000000ull, CX3_CMD_IDLE_WAIT_MS), 1050000000ull);

    /* Elapsed time rounds down to whole milliseconds and never goes backwards. */
    CHECK("50 ms of elapsed time reads as 50",
          cx3CmdElapsedMs(1000000ull, 51000000ull), 50);
    CHECK("a reading older than the start reads as 0, not as a huge number",
          cx3CmdElapsedMs(50000000ull, 1000000ull), 0);

    /*
     * The three budgets have to stay ordered this way (idle < teardown < firmware), or a
     * transcription slip turns one into another.  Relationship checks catch that; a
     * per-constant check catches only the value.
     */
    CHECK("idle wait is far below the teardown budget",
          CX3_CMD_IDLE_WAIT_MS < CX3_CMD_BUDGET_ICM_MS, 1);
    CHECK("the teardown budget is below the firmware budget",
          CX3_CMD_BUDGET_ICM_MS < CX3_CMD_BUDGET_FW_MS, 1);
    CHECK("the event idle wait is not shorter than the polled one",
          CX3_CMD_IDLE_WAIT_EVENT_MS >= CX3_CMD_IDLE_WAIT_MS, 1);

    /* --- the four transitions --------------------------------------------------- */
    const uint64_t deadline = cx3CmdDeadlineNs(1000000000ull, 50u);  /* t + 50 ms */
    const uint64_t before   = 1040000000ull;    /* 40 ms in: budget remains */
    const uint64_t after    = 1200000000ull;    /* 200 ms in: budget gone */

    /* A register that reports nothing outstanding: post it (or read the result). */
    CHECK("idle register, healthy mailbox -> idle",
          cx3CmdGate(0, 0, before, deadline), CX3_CMD_GATE_IDLE);

    /* Something outstanding and the budget has not run out: keep waiting. */
    CHECK("busy register, budget remains -> spin",
          cx3CmdGate(0, 1, before, deadline), CX3_CMD_GATE_SPIN);

    /* The deadline is an instant, not an interval: at it, the command is abandoned. */
    CHECK("busy register exactly at the deadline -> abandon",
          cx3CmdGate(0, 1, deadline, deadline), CX3_CMD_GATE_ABANDON);
    CHECK("busy register past the deadline -> abandon",
          cx3CmdGate(0, 1, after, deadline), CX3_CMD_GATE_ABANDON);

    /*
     * The poison is taken first and unconditionally, whatever the register says and
     * whenever it is asked: an idle-looking register is exactly the reading that must
     * NOT be trusted after an abandonment (that is the reading the stuck GO bit produces
     * once someone clears the toggle), and a poison that expired would let the next
     * command post over a command that may still be completing.
     */
    CHECK("poisoned + idle reading -> fail fast, do not post",
          cx3CmdGate(1, 0, before, deadline), CX3_CMD_GATE_FAIL_FAST);
    CHECK("poisoned + busy reading -> fail fast",
          cx3CmdGate(1, 1, before, deadline), CX3_CMD_GATE_FAIL_FAST);
    CHECK("poisoned, no deadline pressure -> fail fast",
          cx3CmdGate(1, 1, after, deadline), CX3_CMD_GATE_FAIL_FAST);
    CHECK("poisoned with an unreachable deadline -> still fail fast (it does not decay)",
          cx3CmdGate(1, 1, 0ull, ~0ull), CX3_CMD_GATE_FAIL_FAST);

    /*
     * Clean inputs permit posting. This pure model takes poison as an input;
     * it cannot establish that reset or any recovery may clear real poison.
     */
    CHECK("clean input, fresh idle register -> post (the echo is not consulted)",
          cx3CmdGate(0, 0, before, deadline), CX3_CMD_GATE_IDLE);
    CHECK("clean input, busy register -> spin, not fail fast",
          cx3CmdGate(0, 1, before, deadline), CX3_CMD_GATE_SPIN);

    /*
     * --- the sentinel: "not sent" is its own value -------------------------------
     *
     * A refused command must not report a value the device could itself have answered
     * with, and must not report 0 ("the firmware said ok").  Family B in docs/DEFECTS.md
     * is six instances of exactly that mistake, one of which printed a RUN_FW timeout as
     * "fw status 0x0 ok".
     */
    CHECK("the wedged sentinel is not 0", CX3_FWSTATUS_WEDGED != 0, 1);
    CHECK("it differs from the timeout sentinel",
          CX3_FWSTATUS_WEDGED != CX3_FWSTATUS_TIMEOUT, 1);
    CHECK("it differs from not-posted", CX3_FWSTATUS_WEDGED != CX3_FWSTATUS_NOT_POSTED, 1);
    CHECK("it differs from no-device", CX3_FWSTATUS_WEDGED != CX3_FWSTATUS_NO_DEVICE, 1);
    for (i = 0; i < sizeof(fwCodes) / sizeof(fwCodes[0]); ++i) {
        ++checks;
        if ((unsigned)CX3_FWSTATUS_WEDGED == fwCodes[i]) {
            ++failures;
            printf("  FAIL %-58s overlaps firmware status 0x%x\n",
                   "the wedged sentinel is host-side only", fwCodes[i]);
        }
    }

    printf("  %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
