#!/bin/bash
#
# M3 restart 3: the port, and the ring-initialisation A/B experiment.
#
# Two things happen here and they are deliberately in this order:
#
#   1. The A/B experiment.  It hands a ring to SW2HW_EQ on EQN 28 -- the same number
#      m3equp uses -- so it has to run BEFORE the production EQ exists.  Its job is to
#      turn "the ring changed after SW2HW_EQ" from a correlation into a measurement,
#      by comparing the ring that was handed over against one that was not.
#
#   2. The port.  QUERY_PORT first (read-only, and it is also the gate), then the
#      gated SET_PORT general + INIT_PORT, then QUERY_PORT again to read the link bit.
#      INIT_PORT succeeding is not the acceptance criterion; the link bit is.
#
# The two are independent, so if the port stage is not wanted, pass `noport` and only
# the first one runs.
#
# ⚠ Same rule as always: after the HCA reset, nothing may read BAR0 0xf03fc until
# RUN_FW has completed -- which is why there is no probe between steps 2 and 4.
#
# Usage:  sudo tools/cx3-mlx4/m3-r3-sequence.sh [noport]

set -uo pipefail
CTL="${CX3CTL:-/usr/local/bin/cx3ctl}"
WANT_PORT="${1:-port}"

[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
[ -x "$CTL" ] || { echo "$CTL not found"; exit 1; }

hr() { printf '%s\n' "============================================================"; }
step() { echo; hr; echo "  $*"; hr; }

step "1/8  read-only baseline (before the reset, where reading is safe)"
"$CTL" probe || { echo "probe failed; stopping"; exit 1; }

step "2/8  HCA reset"
"$CTL" reset force || { echo "reset failed; stopping"; exit 1; }

step "3/8  QUERY_FW + fwload (FW area, MAP_FA, RUN_FW, MOD_STAT_CFG)"
QFW=$("$CTL" queryfw force 2>&1)
PAGES=$(printf '%s\n' "$QFW" | sed -n 's/^ *fw pages *: *\([0-9][0-9]*\).*/\1/p' | head -1)
echo "$QFW" | grep -E "fw pages|cmd_if_rev" || true
[ -n "$PAGES" ] || { echo "could not read fw_pages; stopping"; exit 1; }
"$CTL" fwload "$PAGES" | grep -E "MAP_FA|RUN_FW|MOD_STAT_CFG" || exit 1

step "4/8  m2dprofile + m2dbringup  (context memory, 14 tables, INIT_HCA)"
"$CTL" m2dprofile > /dev/null || { echo "profile failed"; exit 1; }
"$CTL" m2dbringup | grep -E "ICM tables|INIT_HCA" || exit 1

step "5/8  A/B ring experiment, round 1: hand ring A over, B is the control"
echo "Both rings are built the same way and diffed byte-for-byte against their own"
echo "baseline at every sample.  The control ring goes through every host-side step"
echo "and never sees the firmware, so 'the ring changed' can be attributed."
echo
"$CTL" m3abring a

step "6/8  A/B ring experiment, round 2: hand ring B over, A is the control"
echo "The roles are swapped.  If the change follows the ring that was handed over"
echo "rather than the ring at a particular bus address, the two rounds disagree about"
echo "which ring changed -- and that is the whole point of doing it twice."
echo
"$CTL" m3abring b

if [ "$WANT_PORT" = "noport" ]; then
    step "7/8, 8/8  skipped (noport)"
    echo "Only the A/B experiment was asked for."
    exit 0
fi

step "7/8  QUERY_PORT both ports (read-only) -- this is also the gate"
for P in 1 2; do
    echo
    "$CTL" queryport "$P"
done

step "8/8  m3portup: SET_PORT general (Ethernet) + INIT_PORT, gated on that answer"
echo "Order is the reference's own start path: general then INIT_PORT.  No CLOSE_PORT"
echo "-- that belongs to the stop and type-change paths, and the link bit must not be"
echo "used to decide whether one is needed."
echo
"$CTL" m3portup 1
RC=$?
echo
echo "--- and a fresh look at the port afterwards ---"
"$CTL" queryport 1 | grep -E "link_state|link_speed|supported_types|eth_mtu"

echo
hr
case "$RC" in
    0) echo "  R3: LINK UP on port 1." ;;
    2) echo "  R3: STOPPED AT THE GATE -- the port does not advertise Ethernet." ;;
    3) echo "  R3: the commands went through but the link bit is 0.  That is DOWN and"
       echo "      it does not say why: cable, far end, or an untrained port are all"
       echo "      still open.  Compare with the Linux run, which reported 'eth0: Link"
       echo "      Up' on this card -- so the port itself can do it." ;;
    *) echo "  R3: did not complete; read the statuses above and the kernel log." ;;
esac
echo
echo "  Kernel log: /usr/bin/log show --last 5m --predicate 'eventMessage BEGINSWITH \"Cx3Mlx4: \"'"
hr
exit 0
