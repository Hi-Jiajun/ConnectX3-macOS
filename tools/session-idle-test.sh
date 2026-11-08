#!/bin/bash
#
# session-idle-test.sh -- open an interrupt session, sit still, close it, sit still again.
#
# Why this and not the whole chain.  The chain has dozens of steps and several of them can
# hang the machine, so a death inside it says only "somewhere in these thirty steps".  This
# runs the SHORTEST sequence that reaches an open session -- nine calls, no port, no QP, no
# queues -- and then does nothing at all.  If the machine dies in the idle, the session
# itself is sufficient and the mechanism does not need the device being busy.  If it
# survives, the suspicion moves to what the chain does WITH a session open, which is what
# docs/HANG-AUDIT.md 10.5 step 3 bisects.
#
# The second idle is the other half, and it is the half 0.44.21 changed: until that build the
# close removed the event source and left the QR with the firmware, so the card could assert a
# level-triggered line for the rest of the boot with nothing to acknowledge it (DEFECTS.md
# F17/F18).  Surviving an idle AFTER the close is therefore a reading about the fix, not a
# repeat of the first one.
#
# Every line carries a wall-clock stamp, and the idles print one every 10 s: if the machine
# hard-dies, the last line of this output is the time it died (to 10 s), on a box where the
# only other clock is another session's file.
#
# Usage:  sudo tools/session-idle-test.sh [open-idle-seconds] [closed-idle-seconds]
#         defaults: 120 and 60.  Use 0 to skip either phase.
#
# Reads nothing from the device that it did not just create: the sequence is reset ->
# queryfw(force) -> fwload -> querydevcap -> m2dprofile -> INIT_HCA -> m3equp -> m3clrint ->
# m3irqup.  `probe` is deliberately NOT used anywhere in it: after a reset, probe reads
# 0xf03fc and wedges the mailbox for 60 s (docs/M2B-NOTES section 9).

set -uo pipefail

C=${CX3CTL:-/usr/local/bin/cx3ctl}
SIZES=$("$C" sizes)

stamp() { date '+%H:%M:%S'; }

call() {
    local sel=$1; shift
    local row nin nout ssz name vals i out st
    row=$(printf '%s\n' "$SIZES" | awk -v s="$sel" '$1==s {print $2" "$3" "$4" "$5}')
    read -r nin nout ssz name <<< "$row"
    vals=$(IFS=,; echo "$*")
    i=$(printf '%s' "$vals" | awk -F, '{print ($0=="")?0:NF}')
    #
    # A call with NO arguments sends `printf '%s' ""`, which writes no bytes and no newline,
    # so awk reads nothing and prints nothing and `i` comes out EMPTY -- not 0.  `[ "" -lt n ]`
    # is then a shell error, and the loop below silently does not run.  It happened to give the
    # right answer here (an empty value list is what a no-argument call needs), but a noisy
    # "integer expression expected" on the line before a selector runs is exactly the kind of
    # message that trains a reader to skip messages.  Default it explicitly.
    #
    [ -n "$i" ] || i=0
    while [ "$i" -lt "$nin" ]; do vals="${vals}${vals:+,}0"; i=$((i+1)); done
    out=$("$C" rawjson "$sel" in "$vals" out "$nout" struct "$ssz" 2>&1 | tail -1)
    st=$(printf '%s' "$out" | sed -n 's/.*"out":\["\(0x[0-9a-f]*\)".*/\1/p')
    printf '[%s] %-12s call=%s status=%s\n' "$(stamp)" "$name" \
      "$(printf '%s' "$out" | sed -n 's/.*"call":\([-0-9]*\).*/\1/p')" "$st"
}

idle() {
    local secs=$1 label=$2 left=$1
    [ "$secs" -gt 0 ] || { printf '[%s] idle: %s skipped (0 s)\n' "$(stamp)" "$label"; return; }
    printf '[%s] idle: %s for %s s -- a death between this line and the next is a death with the\n' \
        "$(stamp)" "$label" "$secs"
    printf '        card doing NOTHING.  Console stopped?  That is the reading.\n'
    while [ "$left" -gt 0 ]; do
        sleep 10
        left=$((left - 10))
        [ "$left" -lt 0 ] && left=0
        printf '[%s]   still alive, %s s of this idle to go\n' "$(stamp)" "$left"
    done
}

OPEN_IDLE="${1:-120}"
CLOSED_IDLE="${2:-60}"

printf '[%s] === session-idle-test: opening a session ===\n' "$(stamp)"

call 3  1 1              # reset (factory state; also clears any previous session's state)
call 2  1                # queryfw force=1 -- m3clrint needs a HELD QUERY_FW result
call 10 1542 0 0 0       # fwload (MAP_FA + RUN_FW)
call 8  1 0              # querydevcap
call 13 1024 16 64 256 256 2048 4096   # m2dprofile
call 14                  # INIT_HCA
call 15 0 0 1 1          # EQ: the ring, MTT, SW2HW_EQ, MAP_EQ, arm
call 23                  # m3clrint: the legacy clear path, without which irqup refuses
call 19 0 0              # m3irqup: THE ONE VARIABLE

idle "$OPEN_IDLE" "session OPEN"

printf '[%s] === closing the session (this is the 0.44.21 path: ack, drain, hand the EQ back) ===\n' \
    "$(stamp)"
call 22                  # m3irqdown

idle "$CLOSED_IDLE" "session CLOSED"

printf '[%s] === session-idle-test: both idles survived ===\n' "$(stamp)"
printf 'Now read the kernel log for the two lines that carry the readings:\n'
printf '  /usr/bin/log show --last 5m --predicate %s | grep -E "m3irqdown|HW2SW_EQ|EQ teardown"\n' \
    "'eventMessage CONTAINS \"Cx3Mlx4\"'"
