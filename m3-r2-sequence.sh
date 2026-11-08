#!/bin/bash
#
# M3 restart 2: the same completion, delivered by an interrupt instead of a poll.
#
# R1 proved the queue's DMA path with polling.  This proves the notification path:
# a workloop event source attached to a provider interrupt, with a callback that
# consumes the queue in order and re-arms it.
#
# The reviews' acceptance table for this stage, and where each item is answered:
#   interrupt delivered            -> m3irqstatus: "matched YES" with the right token
#   repeated re-arm                -> three tests in a row, all three matched
#   two or more phase wraps        -> left to the interrupt-driven soak, not here
#   mixed events do not impersonate-> a PORT_CHANGE arrives before the completion and
#                                     must not be reported as it (the callback records
#                                     "does not match the pending" instead)
#   teardown leaves no late callback -> m3irqdown then m3irqstatus
#
# ⚠ Same rule as always: after the HCA reset, nothing may read BAR0 0xf03fc until
# RUN_FW has completed -- which is why there is no probe after step 1.
#
# Usage:  sudo tools/cx3-mlx4/m3-r2-sequence.sh [source-index]
#         source-index defaults to 0; the EQ context's intr field is 0, so 0 is the
#         value that means "MSI-X table entry 0".

set -uo pipefail
CTL="${CX3CTL:-/usr/local/bin/cx3ctl}"
SRC="${1:-0}"
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
[ -x "$CTL" ] || { echo "$CTL not found"; exit 1; }

hr() { printf '%s\n' "============================================================"; }
step() { echo; hr; echo "  $*"; hr; }

step "1/10  read-only baseline (before the reset, where reading is safe)"
"$CTL" probe || { echo "probe failed; stopping"; exit 1; }

step "2/10  HCA reset"
"$CTL" reset force || { echo "reset failed; stopping"; exit 1; }

step "3/10  QUERY_FW"
QFW=$("$CTL" queryfw force 2>&1)
PAGES=$(printf '%s\n' "$QFW" | sed -n 's/^ *fw pages *: *\([0-9][0-9]*\).*/\1/p' | head -1)
echo "$QFW" | grep -E "fw pages|fw status"
[ -n "$PAGES" ] || { echo "could not read fw_pages; stopping"; exit 1; }

step "4/10  fwload $PAGES  (FW area, MAP_FA, RUN_FW, MOD_STAT_CFG)"
"$CTL" fwload "$PAGES" | grep -E "MAP_FA|RUN_FW|MOD_STAT" || exit 1

step "5/10  m2dprofile + m2dbringup  (context memory, 14 tables, INIT_HCA)"
"$CTL" m2dprofile > /dev/null || { echo "profile failed"; exit 1; }
"$CTL" m2dbringup | grep -E "ICM tables|INIT_HCA" || exit 1

step "6/10  m3equp async  (QUERY_FUNC, ring, MTT, SW2HW_EQ, MAP_EQ, armed)"
"$CTL" m3equp async | grep -E "QUERY_FUNC|EQ  |MTT addr|arm wait|MAP_EQ" || exit 1

step "7/10  m3clrint  (the interrupt-clear register: the prerequisite)"
echo "The source this provider gives us is legacy, and a legacy level interrupt has to"
echo "be acknowledged at the device or it storms -- enabling one without this is what"
echo "wedged the machine.  This sets up the clear and m3irqup refuses without it."
echo
"$CTL" m3clrint || { echo "the clear register could not be set up; stopping"; exit 1; }

step "8/10  m3irqup $SRC  (workloop + interrupt event source, enabled)"
"$CTL" m3irqup "$SRC" || { echo "the interrupt session did not open"; exit 1; }

step "9/10  three tests in a row: post, then read the result separately"
echo "Each test registers its own token and returns without waiting; the completion"
echo "has to arrive on its own, through the interrupt."
rc=0
for T in 0x1234 0xabcd 0x55aa; do
    echo
    echo "--- token $T ---"
    "$CTL" m3irqtest "$T" | grep -E "status|NOP fw|token|cmd dword"
    # Give the interrupt a moment; the action runs on the workloop, asynchronously.
    sleep 1
    if "$CTL" m3irqstatus | grep -q "PASS"; then
        echo "    PASS for token $T"
    else
        echo "    did NOT match token $T"
        "$CTL" m3irqstatus | sed 's/^/    /'
        rc=1
    fi
done

step "9b/10  a completion that is NOT the pending one must not be reported as it"
echo "The matching key is the token, so the question is what happens to an EQE that"
echo "is a real completion but belongs to a different command.  Two completions are"
echo "put in flight with different tokens, one through the interrupt path and one"
echo "through the poll selector, while only the first token is registered as the"
echo "test.  Two things have to hold:"
echo
echo "  - the test still reports the token it registered, and only that one"
echo "  - the other completion is consumed and recorded as a non-match, not"
echo "    reported as the test's completion"
echo
echo "This is the same code path a PORT_CHANGE would take (a non-CMD, or a CMD"
echo "with somebody else's token, arriving while a test is pending).  A real"
echo "PORT_CHANGE needs the port configured, which is a later stage -- so a real"
echo "PORT_CHANGE is deliberately NOT claimed here."
echo
"$CTL" m3irqtest 0xb001 | grep -E "posted|status" | sed 's/^/  /'
echo "  The poll selector posts the second command but is given no time to wait for"
echo "  it (wait 0), so the second completion is left for the interrupt path's"
echo "  consumer to find.  That is the point: it must be the interrupt path that has"
echo "  to deal with a completion that is not the pending one.  With a non-zero wait"
echo "  the poll selector could consume it first and the mismatch branch would not be"
echo "  exercised at all."
"$CTL" m3eqpoll nop 0xb002 wait 0 | grep -E "token|NOP|EQE|not this" | sed 's/^/  /'
sleep 1
echo
echo "  --- what the interrupt path reports now ---"
"$CTL" m3irqstatus | sed 's/^/  /'
if "$CTL" m3irqstatus | grep -q "token 0xb001"; then
    echo "    the test's own token is what it reported (0xb001)"
else
    echo "    the reported token is NOT the registered one -- read the status above"
    rc=1
fi
echo
echo "  --- the kernel log's own account of the foreign completion ---"
/usr/bin/log show --last 2m --predicate 'eventMessage CONTAINS "Cx3Mlx4"' 2>/dev/null |
    grep -E "does not match the pending|not a CMD completion" | tail -4 | sed 's/^/    /' ||
    echo "    (no such line: the foreign completion may not have been consumed yet)"
echo
echo "  A line naming a token that is not 0xb001 as 'does not match the pending' is"
echo "  the evidence that a foreign completion is not reported as the test's."
echo

step "10/10  m3irqdown, then confirm the session is closed"
"$CTL" m3irqdown
"$CTL" m3irqstatus | grep -E "session|interrupts" | sed 's/^/  /'

echo
hr
if [ "$rc" -eq 0 ]; then
    echo "  R2 PASSED: an event-mode command's completion reached the driver through"
    echo "  the interrupt path, three times in a row, each with its own token -- so the"
    echo "  notification works and the queue re-arms itself between deliveries."
else
    echo "  R2 did not pass.  Read 'm3irqstatus' carefully:"
    echo "    acks 0 while a LEGACY source is enabled"
    echo "                            -> the clear never happened; that is the state"
    echo "                               that stormed before. Stop and fix that first."
    echo "    interrupts 0            -> the notification is not reaching the handler;"
    echo "                               the source index is likely not the vector the"
    echo "                               EQ is on (its context says intr = 0)"
    echo "    interrupts > 0, no match-> it is reaching the handler but the queue did"
    echo "                               not produce the expected completion"
    echo "  Kernel log: /usr/bin/log show --last 3m --predicate 'eventMessage CONTAINS \"Cx3Mlx4\"'"
fi
hr
exit "$rc"
