#!/bin/bash
#
# M2 in one run: reset the HCA, bring the firmware up, then ask it for the
# capability set.
#
# ⚠ ONE RULE, AND IT IS NOT OPTIONAL: after the HCA reset, do not read BAR0
# offset 0xf03fc (the reset semaphore) until RUN_FW has completed.  Measured on
# this card: a single read of that word between the reset and RUN_FW makes
# RUN_FW never complete (GO stays set, 60 s x ~3 ms later the command times out).
# That is why this script no longer runs the read-only probe after step 2 -- the
# probe reads that register, and it was the reason every run of this script
# failed while the same sequence without a probe succeeded.  See
# docs/M2B-NOTES.md section 9.
#
# Why this is a script rather than four commands typed by hand: each step's
# outcome decides whether the next one is meaningful, `fw_pages` has to be carried
# from step 2 into step 3, and a failure needs its evidence captured in place. It
# also prints what it is about to do, because two of these steps write the device
# and one of them can require a power cycle.
#
# Usage:  sudo tools/cx3-mlx4/m2-sequence.sh [--lowmem] [--no-reset] [--fill]
#
#   --no-reset   skip step 1 (useful once the reset has already been done this boot)
#   --lowmem     constrain the FW area below 4 GiB, for testing whether the
#                firmware can fetch from high bus addresses
#   --fill       write a known pattern into the FW area before MAP_FA, so that
#                what is in it afterwards is interpretable (see docs/M2B-NOTES.md)

set -uo pipefail

CTL="${CX3CTL:-/usr/local/bin/cx3ctl}"
LOWMEM=0
DO_RESET=1
FILL=0

for a in "$@"; do
    case "$a" in
        --lowmem)   LOWMEM=1 ;;
        --no-reset) DO_RESET=0 ;;
        --fill)     FILL=1 ;;
        *) echo "unknown option: $a"; exit 2 ;;
    esac
done

[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
[ -x "$CTL" ] || { echo "$CTL not found; install 0.21.0 or newer"; exit 1; }

hr() { printf '%s\n' "------------------------------------------------------------"; }
step() { echo; hr; echo "  $*"; hr; }

echo "M2 sequence"
echo "  controller : $CTL"
echo "  reset      : $([ "$DO_RESET" = 1 ] && echo "yes (semaphore gate bypassed)" || echo "skipped")"
echo "  fw area    : $([ "$LOWMEM" = 1 ] && echo "below 4 GiB" || echo "anywhere")"
echo "  fill       : $([ "$FILL" = 1 ] && echo "yes (known pattern before MAP_FA)" || echo "no")"

# ---------------------------------------------------------------------------
step "1/4  read-only baseline"
if ! "$CTL" probe; then
    echo "probe failed; stopping before anything writes to the device"
    exit 1
fi
echo
echo "NOTE: this probe happens BEFORE the reset, which is safe. Reading the"
echo "      semaphore after the reset (see the warning at the top) is not."

# ---------------------------------------------------------------------------
if [ "$DO_RESET" = 1 ]; then
    step "2/4  HCA reset"
    echo "The reference driver resets the HCA before any firmware command, because"
    echo "a boot ROM may have left it in an undefined state. This port never has,"
    echo "and RUN_FW timed out without it."
    echo
    echo "This writes 0xf0010 and re-zeros the PCI header. If it fails part way, the"
    echo "card may need a power cycle; a warm reboot has been enough so far."
    echo
    if ! "$CTL" reset force; then
        echo "reset failed; stopping"
        exit 1
    fi
    # No probe here on purpose: it reads 0xf03fc, and that read is exactly what
    # stops RUN_FW from completing.  A read-only health check is not worth
    # breaking the thing being tested.
else
    step "2/4  HCA reset  --  skipped on request"
fi

# ---------------------------------------------------------------------------
step "3/4  QUERY_FW  (gives fw_pages, the size of the area to allocate)"
QFW=$("$CTL" queryfw force 2>&1)
echo "$QFW"

# "  fw pages    : 1542 (0x606) -> 6168 KiB of FW area to allocate"
# 1542 is what the reference's offset (0x00) yields on this card.  Until 0.21.0
# this tool read offset 0x10 instead and printed 5123, which is why the FW area
# was 3.3x too large; fwload now refuses a count that disagrees with QUERY_FW.
PAGES=$(printf '%s\n' "$QFW" | sed -n 's/^ *fw pages *: *\([0-9][0-9]*\).*/\1/p' | head -1)
if [ -z "$PAGES" ]; then
    echo
    echo "could not read fw_pages from the output above; stopping"
    exit 1
fi
echo "  -> will allocate $PAGES pages ($((PAGES * 4)) KiB)"

# ---------------------------------------------------------------------------
step "4/4  fwload $PAGES  (allocate + MAP_FA + RUN_FW + MOD_STAT_CFG)"
FWOPTS=""
[ "$LOWMEM" = 1 ] && FWOPTS="$FWOPTS lowmem"
[ "$FILL" = 1 ]   && FWOPTS="$FWOPTS fill"
"$CTL" fwload "$PAGES" $FWOPTS

# ---------------------------------------------------------------------------
step "then: QUERY_DEV_CAP"
echo "If the four steps above are all ok, the firmware is running and this should"
echo "answer instead of returning CMD_STAT_BAD_SYS_STATE:"
echo
echo "    sudo $CTL querydevcap force"
echo
echo "The number that matters most is min page size: it decides whether the ICM"
echo "layer in M2d can go on assuming 4 KiB."
