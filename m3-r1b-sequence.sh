#!/bin/bash
#
# M3 restart 1, attempt B: two MTT addressing forms in ONE boot.
#
# Why this exists, and why it is a script rather than a list of commands:
#
# The R1 run got every step accepted -- QUERY_FUNC, SW2HW_EQ, MAP_EQ, and an
# event-mode NOP all returned fw status 0 -- and yet the EQ ring was untouched:
# every owner byte zero and the one slot read back of type 0x00, i.e. never
# written.  So the HCA is not storing event descriptors in this ring.
#
# The one link in that chain which is neither source-provable nor already
# exercised is the MTT: an event is written through the MTT entries the EQ context
# points at, and the *units* of that pointer could not be settled from the
# material at hand -- mlx4_buddy.c, which would say what mtt->offset counts in, is
# not among the 41 vendored files.  The reference passes
# mlx4_mtt_addr() = offset * entry_size, a byte offset inside the table; the other
# possibility is an absolute context-memory address.
#
# Both forms are attempted here, with a full reset and re-initialisation between
# them, because an HCA reset makes teardownEq() rewind the host-side state and the
# whole INIT_HCA chain is scripted and takes a few seconds.  Each attempt is one
# pass through the same sequence, so a difference between them is attributable.
#
# ⚠ Same rule as M2: after the HCA reset, do not read BAR0 0xf03fc until RUN_FW
# has completed -- which is why there is no probe after step 1.
#
# Usage:  sudo tools/cx3-mlx4/m3-r1b-sequence.sh

set -uo pipefail
CTL="${CX3CTL:-/usr/local/bin/cx3ctl}"
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
[ -x "$CTL" ] || { echo "$CTL not found"; exit 1; }

hr() { printf '%s\n' "============================================================"; }
step() { echo; hr; echo "  $*"; hr; }

# One full pass: reset, bring the firmware and context memory up, build the EQ
# with the requested MTT addressing form, then look at what the HCA stored and
# what it delivered.
attempt() {
    local mttabs="$1" label="$2"

    step "$label: reset and re-initialise"
    "$CTL" reset force >/dev/null || { echo "reset failed"; return 1; }
    local qfw pages
    qfw=$("$CTL" queryfw force 2>&1)
    pages=$(printf '%s\n' "$qfw" | sed -n 's/^ *fw pages *: *\([0-9][0-9]*\).*/\1/p' | head -1)
    [ -n "$pages" ] || { echo "could not read fw_pages"; return 1; }
    "$CTL" fwload "$pages" >/dev/null 2>&1 || { echo "fwload failed"; return 1; }
    "$CTL" m2dprofile >/dev/null 2>&1 || { echo "m2dprofile failed"; return 1; }
    "$CTL" m2dbringup >/dev/null 2>&1 || { echo "m2dbringup failed"; return 1; }
    echo "  firmware running, 14 context tables mapped, INIT_HCA accepted"

    step "$label: EQ up with $label MTT addressing"
    "$CTL" m3equp async $mttabs || { echo "m3equp failed"; return 1; }

    step "$label: what the HCA stored and what it would follow"
    "$CTL" m3eqinfo

    step "$label: event-mode NOP, then poll"
    "$CTL" m3eqpoll nop 0x1234 dump 4 wait 100
    return $?
}

step "1/2  baseline: MTT address as the reference passes it (offset inside the table)"
attempt "" "offset"
rc_a=$?
echo "  attempt A exit: $rc_a"

step "2/2  experiment: MTT address as an absolute context-memory address"
attempt "mttabs" "absolute"
rc_b=$?
echo "  attempt B exit: $rc_b"

echo
hr
echo "  How to read this:"
echo "    - 'EQC entry ... non-zero' shows whether SW2HW_EQ stored the context at"
echo "      all.  It is zeroed before the command, so a zero entry means it did not."
echo "    - 'cmd dword ... E bit SET' shows the hardware received an event-mode"
echo "      command; E clear would explain everything by itself."
echo "    - If one attempt delivers a CMD EQE and the other does not, the MTT"
echo "      addressing form is the answer, and the notes should say which."
echo "    - If neither delivers, the EQ configuration or the event routing is next,"
echo "      and the EQC bytes above are what to compare against."
hr
exit 0
