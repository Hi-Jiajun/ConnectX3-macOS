#!/bin/bash
#
# Which MTT address values does the firmware actually accept?
#
# M2B-NOTES section 22 records the anomaly: handing a ring over whose MTT entries start
# at byte address 0x200 writes the whole ring, and 0x240 writes nothing, with the same
# ring identity, the same EQN and the same cycle -- so the discriminator is the MTT
# address, not the ring.
#
# This walks the address up in steps that make the competing explanations disagree.
# Ring A's entries start at (64 + dummyMtt), and its byte address is that x 8:
#
#   dummy  entries  byte addr  256-aligned  512-aligned  power of 2  observed
#     0       64     0x200         yes          yes          yes      WRITES
#     8       72     0x240         no           no           no       nothing
#    24       88     0x2C0         no           no           no       ?
#    32       96     0x300         yes          no           no       ?  <- discriminator
#    64      128     0x400         yes          yes          yes      ?
#   128      192     0x600         yes          yes          no       ?  <- 512 vs pow2
#
#   "aligned value, 256 bytes"  -> 24 no, 32 YES, 64 yes
#   "aligned value, 512 bytes"  -> 24 no, 32 no,  64 yes, 128 yes
#   "power of two only"         -> 24 no, 32 no,  64 yes, 128 no
#   "only the first allocation" -> every dummy > 0 is no
#
# Usage:  sudo tools/cx3-mlx4/m3-abring-alignment.sh [dummy ...]

set -uo pipefail
CTL="${CX3CTL:-/usr/local/bin/cx3ctl}"
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
[ -x "$CTL" ] || { echo "$CTL not found"; exit 1; }

DUMMIES=("$@")
[ "${#DUMMIES[@]}" -gt 0 ] || DUMMIES=(0 8 24 32 64 128)

fresh() {
    "$CTL" reset force > /dev/null 2>&1 || return 1
    "$CTL" queryfw force > /dev/null 2>&1 || return 1
    "$CTL" fwload 1542 > /dev/null 2>&1 || return 1
    "$CTL" m2dprofile > /dev/null 2>&1 || return 1
    "$CTL" m2dbringup > /dev/null 2>&1 || return 1
    return 0
}

printf '%-7s %-9s %-11s %-14s %s\n' dummy entries byteaddr "A changed" verdict
printf '%s\n' "--------------------------------------------------------------------------"

for D in "${DUMMIES[@]}"; do
    E=$((64 + D))
    A=$(printf '0x%x' $((E * 8)))
    fresh || { echo "  fresh cycle failed for dummy=$D"; continue; }

    OUT=$("$CTL" m3abring a 28 "$D" 2>&1)
    # "ring A: 32510 byte(s) differ from baseline" or "ring A: 0 byte(s) differ"
    CHANGED=$(printf '%s\n' "$OUT" | sed -n 's/^ *ring A: \([0-9][0-9]*\) byte(s) differ from baseline.*/\1/p' | head -1)
    [ -n "$CHANGED" ] || CHANGED="?"

    if [ "$CHANGED" = "?" ]; then
        VERDICT="could not read the result"
    elif [ "$CHANGED" -gt 0 ]; then
        VERDICT="WRITTEN"
    else
        VERDICT="nothing"
    fi
    printf '%-7s %-9s %-11s %-14s %s\n' "$D" "$E" "$A" "$CHANGED" "$VERDICT"
done

echo
echo "MTT readback for the last point (host memory, no device involved):"
/usr/bin/log show --last 90s --predicate 'eventMessage BEGINSWITH "Cx3Mlx4: "' 2>/dev/null |
    sed 's/^.*Cx3Mlx4: //' | grep -E "abring: MTT readback for ring A" | tail -1
echo
echo "Kernel log for the whole run:"
echo "  log show --last 10m --predicate 'eventMessage BEGINSWITH \"Cx3Mlx4: \"' | grep abring:"
