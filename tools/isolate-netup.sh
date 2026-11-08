#!/bin/bash
#
# isolate-netup.sh -- the one experiment that separates "netup's QUERY_PORT is fatal" from
# "the state the steps before it leave behind is fatal".
#
# Why this shape, and not "call netup first thing after a clean boot":
#
#   * netup's first act is a mailbox command, and this project has already measured what an
#     unauthorised mailbox/register touch does BEFORE the HCA is initialised: probe reading
#     0xf03fc after a reset leaves RUN_FW incomplete for 60 s and poisons the mailbox
#     (docs/M2B-NOTES section 9).  A netup-as-first-command that died would therefore die for a
#     reason already known, and would say nothing about the question.
#   * so the prefix is the SMALLEST sequence that makes the mailbox legal: reset, QUERY_FW,
#     fwload (MAP_FA + RUN_FW + MOD_STAT_CFG), QUERY_DEV_CAP, the context tables + INIT_HCA.
#     It builds NO EQ, sets up NO interrupt-clear register, opens NO interrupt session, and runs
#     NONE of the R4 stages -- so nothing in it arms a queue or leaves a descriptor ring.
#   * then the SAME command the fatal selector runs, from the same driver code path
#     (Mlx4Hw::queryPort -> cmdBox -> polled), is issued STANDALONE first: `queryport` (24).
#     That is the control.  If 24 answers and 39 dies one line later, "the command is bad" is
#     excluded and what is left is the difference between the two selectors -- not the command.
#
# Read the result as:
#   alive after 39  -> the fatal ingredient is in the state the chain's later steps leave
#                      (EQ / cleart register / interrupt source / R4 rings / the CQ arm), and the
#                      bisect continues from there;
#   dead at 39      -> the ingredient is in the interaction between that minimal state and the
#                      netup selector, and 24-vs-39 becomes the whole question.
#
# Everything is tee'd into the repo (a hard death clears /tmp on this machine, and the unified
# log keeps nothing from the seconds before it), so the file survives the reboot.
#
# Usage:  sudo tools/isolate-netup.sh

set -uo pipefail

SRC=/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4
OUT="$SRC/outputs/isolate-netup-$(date +%Y%m%d-%H%M%S).log"
C=${CX3CTL:-/usr/local/bin/cx3ctl}
SIZES=$("$C" sizes)

#
# Everything below is written twice: once to the terminal and once, line by line, to $OUT.
#
# The first version of this script used `exec > >(tee "$OUT")`, and that was WRONG in exactly the
# way this project keeps finding: `tee` writes to a FILE through stdio, which is block-buffered, so
# the file only receives what the 4 KiB buffer releases.  verify.sh's logs looked fine because a
# chain produces kilobytes; this script produces about two, and the measured result was a
# 147-byte file for a run that had completed the whole chain -- a "record" that would have said
# "nothing happened" about a run that did everything.  (Found by the parallel forensics session,
# 2026-10-13.)  Bash's own `printf` with an append redirection is a write() per call, so this
# cannot buffer.  Use it, not tee.
#
log() { printf '%s\n' "$*" >> "$OUT"; printf '%s\n' "$*"; }

#
# A breadcrumb that leaves the machine on a channel that is neither logd nor this disk.
#
# The recorder on the LAN VM listens on UDP 9999 and timestamps whatever arrives, so "which tool
# call was in flight when the machine stopped" survives a death that takes the unified log with it
# -- which is the shape of every one of today's deaths.  Failure is ignored on purpose: a machine
# without that route must still be able to run the experiment.
#
bc() { printf '%s %s\n' "$(date '+%H:%M:%S')" "$*" | /usr/bin/nc -u -w1 192.0.2.60 9999 >/dev/null 2>&1 || true; }

stamp() { date '+%T'; }

blackbox() {
    log "[$(stamp)] black box: $(curl -sS -m 3 http://127.0.0.1:8099/state 2>&1 || printf 'no answer')"
}

call() {
    local sel=$1; shift
    local row nin nout ssz name vals i out st
    row=$(printf '%s\n' "$SIZES" | awk -v s="$sel" '$1==s {print $2" "$3" "$4" "$5}')
    read -r nin nout ssz name <<< "$row"
    vals=$(IFS=,; echo "$*")
    i=$(printf '%s' "$vals" | awk -F, '{print ($0=="")?0:NF}')
    [ -n "$i" ] || i=0
    while [ "$i" -lt "$nin" ]; do vals="${vals}${vals:+,}0"; i=$((i+1)); done
    bc "pre rawjson $sel $name $*"
    out=$("$C" rawjson "$sel" in "$vals" out "$nout" struct "$ssz" 2>&1 | tail -1)
    bc "post rawjson $sel $name $* rc"
    st=$(printf '%s' "$out" | sed -n 's/.*"out":\["\(0x[0-9a-f]*\)".*/\1/p')
    log "$(printf '[%s] %-12s call=%s status=%s' "$(stamp)" "$name" \
      "$(printf '%s' "$out" | sed -n 's/.*"call":\([-0-9]*\).*/\1/p')" "$st")"
}

log "=== isolate-netup: $OUT ==="
bc "start isolate-netup"
blackbox

log ''
log '--- the smallest state that makes a mailbox command legal (NO EQ, NO interrupts, NO R4) ---'
call 3  1 1
call 2  1
call 10 1542 0 0 0
call 8  1 0
call 13 1024 16 64 256 256 2048 4096
call 14
log ''
blackbox

log ''
log '--- CONTROL: the same command, standalone (selector 24, Mlx4Hw::queryPort) ---'
call 24 1

#
# THE BISECT, in one boot.  Selector 46 runs the SAME implementation as netup (netupRun) and
# returns once the step named by its input is done:
#
#   46 in 1 -> read the address and return            (measured clean, twice)
#   46 in 2 -> also attachInterface
#   46 in 3 -> also publish the link
#   46 in 4 -> also create the receive timer
#   39      -> the whole thing, including the frame ring and the registration
#
# If one of these stops the machine, the LAST BREADCRUMB names the stop point that did it -- and
# the breadcrumbs go to the LAN VM over UDP, which is the only record that survives a death that
# takes the unified log with it.  The stop point is in the breadcrumb text for exactly that reason.
#
# Only 1 is known good; the rest are the experiment.  They run in order, so the first death is the
# answer and the boot ends there.
#
for stop in 1 2 3 4; do
    log ''
    log "--- selector 46 with stopAfter=$stop ---"
    call 46 "$stop"
    log ''
    blackbox
done

log ''
log '--- THE WHOLE THING: selector 39 (netup incl. the frame ring and the registration) ---'
log "[$(stamp)] calling 39 now; if this is the last line, the machine stopped while it was in flight"
call 39

log ''
log '--- survived ---'
bc "pre netup report"
"$C" netup 2>&1 | while IFS= read -r l; do log "    $l"; done
bc "post netup report"
blackbox
