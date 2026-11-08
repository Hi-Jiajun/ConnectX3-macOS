#!/bin/bash
#
# M3 restart 1 in one run: firmware up, context memory up, then one event queue,
# then the acceptance test.
#
# ⚠ Same rule as M2: after the HCA reset, do not read BAR0 0xf03fc until RUN_FW
# has completed.  That is why there is no probe after step 1 -- "cx3ctl probe"
# reads exactly that word, and one read stops RUN_FW from ever finishing
# (docs/M2B-NOTES.md section 9).
#
# Chain, and why each link is needed:
#   1  probe                          read-only, before the reset (safe)
#   2  reset force                    the reference resets before touching anything
#   3  queryfw force                  fw_pages, and proves the command path
#   4  fwload <pages>                 firmware runs: MAP_FA, RUN_FW
#   5  m2dprofile                     QUERY_DEV_CAP + the context-memory profile
#   6  m2dbringup                     SET_ICM_SIZE, 14 tables, INIT_HCA
#   7  m3equp async                   QUERY_FUNC + EQ + MAP_EQ + arm
#   8  m3eqring 0                    QUERY_FW whose outbox is EQ ring page 0: can
#                                    the HCA write into the ring's memory at all?
#   9  m3eqinfo                     MTT readback and QUERY_EQ (read-only)
#  10  m3eqpoll nop 0x1234 dump 6 wait 100
#                                    event-mode NOP, then poll for its CMD EQE
#
# Step 8 is the acceptance test for this restart.  If it passes, the event queue's
# DMA path is proven and restart 2 (an interrupt vector) has something to attach to.
#
# Usage:  sudo tools/cx3-mlx4/m3-r1-sequence.sh [--no-reset]

set -uo pipefail

CTL="${CX3CTL:-/usr/local/bin/cx3ctl}"
DO_RESET=1

for a in "$@"; do
    case "$a" in
        --no-reset) DO_RESET=0 ;;
        *) echo "unknown option: $a"; exit 2 ;;
    esac
done

[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
[ -x "$CTL" ] || { echo "$CTL not found"; exit 1; }

hr() { printf '%s\n' "============================================================"; }
step() { echo; hr; echo "  $*"; hr; }

step "1/10  read-only baseline (before the reset, where reading is safe)"
"$CTL" probe || { echo "probe failed; stopping before anything writes"; exit 1; }
echo
echo "  NOTE: no probe after the reset.  It reads 0xf03fc, and that read is what"
echo "        stops RUN_FW from completing."

if [ "$DO_RESET" = 1 ]; then
    step "2/10  HCA reset"
    "$CTL" reset force || { echo "reset failed; stopping"; exit 1; }
else
    step "2/10  HCA reset  --  skipped on request"
fi

step "3/10  QUERY_FW"
QFW=$("$CTL" queryfw force 2>&1)
echo "$QFW"
PAGES=$(printf '%s\n' "$QFW" | sed -n 's/^ *fw pages *: *\([0-9][0-9]*\).*/\1/p' | head -1)
[ -n "$PAGES" ] || { echo "could not read fw_pages; stopping"; exit 1; }

step "4/10  fwload $PAGES  (FW area, MAP_FA, RUN_FW, MOD_STAT_CFG)"
"$CTL" fwload "$PAGES" || { echo "fwload failed; stopping"; exit 1; }

step "5/10  m2dprofile  (QUERY_DEV_CAP + context-memory profile)"
"$CTL" m2dprofile || { echo "profile failed; stopping"; exit 1; }

step "6/10  m2dbringup  (SET_ICM_SIZE, 14 ICM tables, INIT_HCA)"
"$CTL" m2dbringup || { echo "bring-up failed; stopping before the EQ"; exit 1; }

step "7/10  m3equp  (QUERY_FUNC, EQ ring, MTT, SW2HW_EQ, MAP_EQ, arm)"
"$CTL" m3equp async || { echo "EQ bring-up failed; stopping"; exit 1; }

step "8/10  m3eqring  (can the HCA write into the EQ ring at all?)"
echo "This is the discriminator a negative poll needs.  The same instrument that"
echo "proved the FW area reachable -- a QUERY_FW whose outbox points into the target"
echo "memory -- aimed at the ring.  If the answer lands in the ring, the memory is"
echo "fine and any failure is in the EQ's configuration; if not, it is the ring's"
echo "bus addresses."
echo
"$CTL" m3eqring 0 || echo "  (ring probe did not complete)"
echo

step "9/10  m3eqinfo  (what the HCA would follow, and what it thinks the EQ is)"
"$CTL" m3eqinfo
echo

step "10/10  m3eqpoll  (event-mode NOP, then poll the EQ for its CMD EQE)"
"$CTL" m3eqpoll nop 0x1234 dump 6 wait 100
rc=$?

echo
hr
if [ "$rc" -eq 0 ]; then
    echo "  R1 PASSED: an event-mode command's completion arrived as a CMD EQE in"
    echo "  the EQ we built, found by polling.  HCR event request -> routing ->"
    echo "  MTT -> EQ DMA -> software consumption is proven, with no interrupt."
    echo
    echo "  Next restart: attach one MSI-X vector to this EQ and get the same EQE"
    echo "  through a callback instead of a poll."
else
    echo "  R1 did not pass.  Order of reading:"
    echo "    1. m3eqinfo  -- do the MTT entries match the ring's page addresses?"
    echo "                    (a mismatch is the fault; the ring probe could not see"
    echo "                     this because it bypassed the MTT)"
    echo "    2. m3eqinfo's QUERY_EQ -- what geometry does the HCA think it holds?"
    echo "    3. owner map -- 0/32 vs 32/32, as below"
    echo
    echo "  Read the owner map next -- it separates the two cases:"
    echo "    - 0 of 32 slots owned  -> the HCA never took ownership at SW2HW_EQ, so"
    echo "                              the poll cannot be trusted yet"
    echo "    - 32 of 32 owned       -> armed and empty: the event was not delivered,"
    echo "                              or the HCA wrote to pages the host is not reading"
    echo "  The kernel log has the ring, MTT and doorbell values:"
    echo "      /usr/bin/log show --last 3m --predicate 'eventMessage CONTAINS \"Cx3Mlx4\"'"
    echo
    echo "  A rerun needs a reset first (INIT_HCA is once per load):"
    echo "      sudo $CTL reset force   then re-run this script with --no-reset"
fi
hr
exit "$rc"
