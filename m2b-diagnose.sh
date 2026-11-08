#!/bin/bash
#
# M2b diagnosis in one run: ask the three questions that decide what to fix next,
# in the order that makes each answer usable by the next.
#
# The questions, and why this order:
#
#   1. Can the device write into the FW area at all?   (fwoutbox)
#      A QUERY_FW whose outbox must land inside the FW area.  Known-good command,
#      self-checking answer, exercises exactly the DMA path RUN_FW needs.
#
#   2. Does a second command still complete after that? (implied by 1 succeeding)
#      If fwoutbox times out, GO stays set and nothing else can be posted, so the
#      run has to stop there and the next attempt needs a reboot.
#
#   3. What does the area hold after the full sequence?  (fwload fill + fwarea)
#      With a pattern written first, the contents afterwards say whether the
#      device ever touched the memory.
#
# Usage:  sudo tools/cx3-mlx4/m2b-diagnose.sh [--lowmem] [--no-fill]
#
# ⚠ Do not read BAR0 offset 0xf03fc (the reset semaphore) between the reset and
# RUN_FW: one read makes RUN_FW never complete.  The single probe below is before
# the reset on purpose.  See docs/M2B-NOTES.md section 9.

set -uo pipefail

CTL="${CX3CTL:-/usr/local/bin/cx3ctl}"
LOWMEM=0
FILL=1

for a in "$@"; do
    case "$a" in
        --lowmem)  LOWMEM=1 ;;
        --no-fill) FILL=0 ;;
        *) echo "unknown option: $a"; exit 2 ;;
    esac
done

[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
[ -x "$CTL" ] || { echo "$CTL not found"; exit 1; }

hr() { printf '%s\n' "============================================================"; }
step() { echo; hr; echo "  $*"; hr; }

step "1/6  read-only baseline"
"$CTL" probe || { echo "probe failed; stopping"; exit 1; }

step "2/6  HCA reset (the reference's first act, semaphore gate bypassed)"
echo "A stuck RUN_FW leaves the command register busy, and only a reset clears"
echo "that, so this run starts from a fresh device."
"$CTL" reset force || { echo "reset failed; stopping"; exit 1; }

step "3/6  QUERY_FW"
QFW=$("$CTL" queryfw force 2>&1)
echo "$QFW"
PAGES=$(printf '%s\n' "$QFW" | sed -n 's/^ *fw pages *: *\([0-9][0-9]*\).*/\1/p' | head -1)
[ -n "$PAGES" ] || { echo "could not read fw_pages; stopping"; exit 1; }

step "4/6  allocate the FW area ($PAGES pages) so it can be used as a target"
"$CTL" icmalloc "$PAGES" || { echo "allocation failed; stopping"; exit 1; }

step "5/6  QUESTION 1: can the device write into that memory?"
echo "QUERY_FW is sent with its outbox placed inside the FW area. If the answer"
echo "lands there, the memory is reachable and RUN_FW's failure is about content,"
echo "not about the DMA path."
echo
if ! "$CTL" fwoutbox 0 8; then
    echo
    echo "The command did not complete. GO is now set and nothing more can be"
    echo "posted this boot: reboot, then run this script again."
    exit 1
fi

# Release the area again before the real load.  fwload would free it anyway, but
# it does so by unmapping first (mlx4_close_fw does, mlx4_main.c:1913), and this
# area was never mapped -- so freeing it here keeps the sequence free of a command
# the reference never sends in this order.  'icmalloc 0' touches no device.
"$CTL" icmalloc 0 > /dev/null 2>&1

step "6/6  QUESTION 2: the full sequence, with a known pattern in the area"
FWOPTS=""
[ "$LOWMEM" = 1 ] && FWOPTS="$FWOPTS lowmem"
[ "$FILL" = 1 ]   && FWOPTS="$FWOPTS fill"
"$CTL" fwload "$PAGES" $FWOPTS

echo
hr
echo "  then read the area back (only meaningful if the sequence got that far):"
echo "      sudo $CTL fwarea 0 8"
hr
