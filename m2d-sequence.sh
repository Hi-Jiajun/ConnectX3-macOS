#!/bin/bash
#
# M2d in one run: firmware up, then the HCA's own context memory, then INIT_HCA.
#
# ⚠ THE ONE RULE, and it is why this is a script rather than a list of commands:
# after the HCA reset, do not read BAR0 0xf03fc until RUN_FW has completed.  One
# read of that word stops RUN_FW from ever finishing (measured; docs/M2B-NOTES.md
# section 9).  That is why there is no probe after step 1 here -- "cx3ctl probe"
# reads exactly that word.
#
# Order of operations, and why it is this order:
#   1  probe                       read-only, BEFORE the reset (safe)
#   2  reset force                 the reference resets before touching anything
#   3  queryfw force               gives fw_pages
#   4  fwload <pages>              firmware starts running
#   5  m2dprofile                  QUERY_DEV_CAP + mlx4_make_profile
#   6  m2dbringup                  SET_ICM_SIZE, MAP_ICM each table, INIT_HCA
#
# Usage:  sudo tools/cx3-mlx4/m2d-sequence.sh [profile knobs, passed to m2dprofile]
#         e.g.  sudo tools/cx3-mlx4/m2d-sequence.sh qp 512 cq 128
#
# A failure does not require a reboot: re-run the script, which resets first.
# A failure of RUN_FW leaves the command register busy, and only the reset clears
# that -- which is why step 2 is unconditional.

set -uo pipefail

CTL="${CX3CTL:-/usr/local/bin/cx3ctl}"
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
[ -x "$CTL" ] || { echo "$CTL not found"; exit 1; }

hr() { printf '%s\n' "============================================================"; }
step() { echo; hr; echo "  $*"; hr; }

step "1/6  read-only baseline (before the reset, where reading is safe)"
"$CTL" probe || { echo "probe failed; stopping before anything writes"; exit 1; }
echo
echo "  NOTE: no probe is run again after the reset.  It reads 0xf03fc, and that"
echo "        read is what stops RUN_FW from completing."

step "2/6  HCA reset"
"$CTL" reset force || { echo "reset failed; stopping"; exit 1; }

step "3/6  QUERY_FW"
QFW=$("$CTL" queryfw force 2>&1)
echo "$QFW"
PAGES=$(printf '%s\n' "$QFW" | sed -n 's/^ *fw pages *: *\([0-9][0-9]*\).*/\1/p' | head -1)
[ -n "$PAGES" ] || { echo "could not read fw_pages; stopping"; exit 1; }

step "4/6  fwload $PAGES  (allocate the FW area, MAP_FA, RUN_FW, MOD_STAT_CFG)"
"$CTL" fwload "$PAGES" || { echo "fwload failed; stopping before M2d"; exit 1; }

step "5/6  m2dprofile  (QUERY_DEV_CAP + the context-memory profile)"
"$CTL" m2dprofile "$@" || { echo "profile failed; stopping"; exit 1; }

step "6/6  m2dbringup  (SET_ICM_SIZE -> MAP_ICM_AUX -> MAP_ICM each table -> INIT_HCA)"
"$CTL" m2dbringup
rc=$?

echo
hr
if [ "$rc" -eq 0 ]; then
    echo "  M2d complete. The next milestone is M3: EQ / CQ / QP and interrupts,"
    echo "  built inside the context memory that INIT_HCA has just been told about."
else
    echo "  M2d did not complete. The step that failed is named in the output"
    echo "  above; the kernel log has the per-table detail:"
    echo "      /usr/bin/log show --last 2m --predicate 'eventMessage CONTAINS \"Cx3Mlx4\"'"
    echo "  Re-running this script is safe: it resets first."
fi
hr
exit "$rc"
