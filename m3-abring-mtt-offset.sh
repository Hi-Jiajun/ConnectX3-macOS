#!/bin/bash
#
# Four points that separate two explanations for the ring-write anomaly.
#
# M2B-NOTES section 22 records the measurement: handing a ring over whose MTT entries
# start at offset 64 has written the ring every time, and handing one whose entries
# start at 72 has written nothing, nine times out of nine -- including handing the
# *same* ring identity over twice in a row, where the write happened both times.  So
# the variable is the ring, and with only offsets 64 and 72 ever produced, "the ring
# that was allocated first" and "MTT offset 64" cannot be told apart.
#
# Passing a non-zero dummyMtt leaks that many MTT entries before the two rings are
# built, which moves them up the table.  With dummy=8 the first ring sits at 72, and
# that is what separates the two explanations:
#
#   3rd point WRITES  -> the discriminator is allocation order, not the offset
#   3rd point does not -> the discriminator is the MTT base value in the context
#
# No reset between the points: the whole question is what the firmware does with a
# *second* handover, and resetting would hide exactly that.
#
# Usage:  sudo tools/cx3-mlx4/m3-abring-mtt-offset.sh

set -uo pipefail
CTL="${CX3CTL:-/usr/local/bin/cx3ctl}"
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
[ -x "$CTL" ] || { echo "$CTL not found"; exit 1; }

hr() { printf '%s\n' "============================================================"; }

hr; echo "  fresh cycle: reset, then bring the HCA up from scratch"; hr
"$CTL" reset force | grep -E "^  status" || exit 1
"$CTL" queryfw force | grep -E "fw pages" || exit 1
PAGES=$("$CTL" queryfw force 2>/dev/null | sed -n 's/^ *fw pages *: *\([0-9]*\).*/\1/p' | head -1)
"$CTL" fwload "${PAGES:-1542}" | grep -E "RUN_FW" || exit 1
"$CTL" m2dprofile > /dev/null || exit 1
"$CTL" m2dbringup | grep -E "INIT_HCA" || exit 1

run() {
    echo
    hr; echo "  $1"; hr
    "$CTL" m3abring "$2" "$3" "$4"
    echo
    echo "  --- kernel log for this point ---"
    /usr/bin/log show --last 40s --predicate 'eventMessage BEGINSWITH "Cx3Mlx4: "' 2>/dev/null |
        sed 's/^.*Cx3Mlx4: //' |
        grep -E "abring: (ring [AB] virt|handing|dummy|QUERY_EQ|MTT readback|SW2HW_EQ returned|HW2SW_EQ|ring [AB] (final|NEVER|first))" |
        sed 's/^/    /'
}

run "point 1: hand A, no dummy MTT  (A lands at offset 64 -- the case that writes)" a 28 0
run "point 2: hand B, no dummy MTT  (B lands at offset 72 -- the case that does not)" b 28 0
run "point 3: hand A, tracked 8-entry dummy reservation  (A now lands at 72)" a 28 8
run "point 4: hand B, reuse the same 8-entry reservation (B now lands at 80)" b 28 8

echo
hr
cat <<'EOF'
  How to read the four points

    point 1 writes and point 2 does not     -> reproduces the anomaly
    point 3 WRITES                          -> the discriminator is "the ring allocated
                                              first", not the MTT offset; look at
                                              icmAlloc/mtt allocation for the second ring
    point 3 does NOT write                  -> the discriminator is the MTT base value the
                                              context carries (64 works, 72 and above do
                                              not); that is a real constraint to design
                                              around, and R4's many rings would hit it

  In both cases, read the QUERY_EQ and MTT readback lines: if the firmware reports a
  different mtt_base than the one sent, the context is not being stored as built, and
  that is a third explanation which needs no firmware expectation about offsets at all.
EOF
hr
