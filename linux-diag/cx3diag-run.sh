#!/bin/sh
#
# Cx3Mlx4 Linux register readback -- minimal and read-only.
#
# Answers one question and nothing else: what does the ConnectX-3 Pro report at
# the BAR0 offsets that macOS reports as 0x01000000?
#
# Why this is so small.  The previous version tried to do four things: read the
# registers with the driver loaded, unload the driver, read again, reload.  It
# hung the machine twice, and the cause is now clear from the runs that did
# complete: it performed raw /dev/mem reads of the HCR command block
# (0x80680-0x8069c) while mlx4 owned the device, and then unloaded the driver.
# Unloading requires firmware commands (0xf, HW2SW_MPT during MPT teardown) and
# the firmware never cleared the GO bit:
#
#     mlx4_core 0000:07:00.0: command 0xf timed out (go bit not cleared)
#
# The driver has a mutex serialising HCR access; an outside reader ignores it, so
# the reads can corrupt an in-flight command.  Two earlier runs that did the same
# unload but no raw reads completed cleanly, which is what points at the reads.
#
# So this version never loads the driver at all.  mlx4 is blocked on the kernel
# command line (see grub.cfg), which leaves the device in exactly the state macOS
# sees: present, unclaimed, no commands in flight.  Nothing is unloaded, nothing
# is reloaded, and no in-flight command can be disturbed.
#
# Everything here is a read.  The one config-space write is the memory-space
# enable, which is what any driver does on bind and which is required for the
# device to answer BAR reads at all.
#
# Results go to <USB>/out/run-<timestamp>/ and the machine reboots.

PATH=/sbin:/bin:/usr/sbin:/usr/bin
export PATH

MARK=CX3DIAG.MARK
LOG=/tmp/cx3diag.log
: > "$LOG" 2>/dev/null || true

say() {
    echo "$*" >> "$LOG" 2>/dev/null || true
    echo "$*" > /dev/console 2>/dev/null || true
}

banner() {
    say ""
    say "============================================================"
    say "  $*"
    say "============================================================"
}

# cap <file> <label> <cmd...>
cap() {
    _f="$1"; shift
    _l="$1"; shift
    {
        printf '\n===== %s =====\n' "$_l"
        printf '$ %s\n' "$*"
        "$@" 2>&1
        printf '(exit status %d)\n' "$?"
    } >> "$OUTDIR/$_f"
    say "    collected: $_l"
}

banner "Cx3Mlx4 register readback starting"
say "  date:   $(date 2>/dev/null)"
say "  kernel: $(uname -r 2>/dev/null)"

# ---------------------------------------------------------------------------
# Locate the stick.  nlplug-findfs may have mounted it read-only.
# ---------------------------------------------------------------------------
MEDIA=""
for d in /media/* /mnt/* /run/media/*/*; do
    [ -f "$d/$MARK" ] && { MEDIA="$d"; break; }
done
if [ -z "$MEDIA" ]; then
    for dev in $(blkid -o device 2>/dev/null); do
        if [ "$(blkid -o value -s LABEL "$dev" 2>/dev/null)" = "CX3DIAG" ]; then
            mkdir -p /mnt/cx3diag
            mount -t vfat "$dev" /mnt/cx3diag 2>/dev/null && { MEDIA=/mnt/cx3diag; break; }
        fi
    done
fi

STAMP=$(date +%Y%m%d-%H%M%S 2>/dev/null)
OUTDIR=""
if [ -n "$MEDIA" ]; then
    OUTDIR="$MEDIA/out/run-$STAMP"
    if ! mkdir -p "$OUTDIR" 2>/dev/null; then
        DEV=$(awk -v m="$MEDIA" '$2==m{print $1}' /proc/mounts 2>/dev/null | head -1)
        [ -n "$DEV" ] && mount -o remount,rw "$DEV" "$MEDIA" 2>/dev/null
        mkdir -p "$OUTDIR" 2>/dev/null
    fi
fi
if [ ! -d "$OUTDIR" ]; then
    OUTDIR=/tmp/cx3diag-out
    mkdir -p "$OUTDIR"
    say "[!] cannot write to the stick; results only in $OUTDIR"
fi
say "  media:  ${MEDIA:-NOT FOUND}"
say "  output: $OUTDIR"

# ---------------------------------------------------------------------------
# The device.
# ---------------------------------------------------------------------------
banner "Device"

BDF=""
for d in /sys/bus/pci/devices/*; do
    [ "$(cat "$d/vendor" 2>/dev/null)" = "0x15b3" ] && { BDF=$(basename "$d"); break; }
done
SHORT=$(echo "$BDF" | cut -d: -f2-)
say "  Mellanox: ${BDF:-none found}"

cap 00-summary.txt "environment and device" sh -c "
    echo \"uname:   \$(uname -a)\"
    echo \"cmdline: \$(cat /proc/cmdline)\"
    echo
    echo \"Mellanox BDF: ${BDF:-none}\"
    echo
    echo 'driver bound to it (must be none for this test):'
    readlink /sys/bus/pci/devices/${BDF:-none}/driver 2>/dev/null || echo '  (none - correct)'
"

cap 01-config.txt "PCI config space" sh -c \
    'dd if=/sys/bus/pci/devices/'"${BDF:-none}"'/config bs=256 count=1 2>/dev/null | hexdump -Cv'

cap 02-lspci.txt "lspci -nnvvv" lspci -nnvvv -s "$SHORT"

cap 03-modules.txt "mlx4 modules must NOT be loaded" sh -c \
    'lsmod | grep -i mlx4 || echo "  (no mlx4 module loaded - this is what we want)"'

cap 04-iomem.txt "kernel view of the BAR ranges" sh -c \
    'grep -A1 -iE "0000:07:00" /proc/iomem || echo "(none)"'

# ---------------------------------------------------------------------------
# Enable memory space, the same bit any driver sets on bind.  Without it the
# device does not claim BAR accesses and every read comes back as all-ones.
# ---------------------------------------------------------------------------
banner "Enabling memory space"
cap 05-setpci.txt "setpci: enable memory space and bus master" sh -c \
    'setpci -s '"$SHORT"' 4.w; setpci -s '"$SHORT"' 4.w=0x0006; echo "after:"; setpci -s '"$SHORT"' 4.w'

# ---------------------------------------------------------------------------
# The readback.
#
# devmem2 output looks like:
#     /dev/mem opened.
#     Memory mapped at address 0x7f... .
#     Read at address  0x00001000 (0x7f...): 0x00001000
# Note there is no "Value = " anywhere in it.  An earlier version matched on that
# string, so every read was recorded as FAIL even though the reads worked.
#
# Each key register is read three times in a row.  macOS shows a first-read-zero
# artifact, and repeating the read here shows whether Linux sees it too, which
# separates a device property from a macOS mapping property.
# ---------------------------------------------------------------------------
banner "BAR0 readback"

BAR0BASE=""
[ -n "$BDF" ] && BAR0BASE=$(head -1 "/sys/bus/pci/devices/$BDF/resource" 2>/dev/null | awk '{print $1}')
say "  BAR0 physical base: ${BAR0BASE:-unknown}"

# Hard stop.  Reading the HCR block from outside while a driver owns the device
# is what hung this machine twice: the driver serialises access to that register
# with a mutex, an outside reader ignores it, and the interrupted command then
# never completes.  If a driver has bound despite the blacklist, the readback
# does not run at all.  Skipping is the correct outcome, not a failure.
BOUND=$(readlink "/sys/bus/pci/devices/$BDF/driver" 2>/dev/null)
if [ -n "$BOUND" ]; then
    say ""
    say "  [!] REFUSING to read registers: a driver is bound to $BDF"
    say "      ($BOUND)"
    say "      External reads of the HCR block while a driver is active are what"
    say "      wedged this machine on 2026-10-10 (command 0xf timed out)."
    say "      No registers were read.  Rebooting."
    {
        echo "readback SKIPPED: a driver was bound to $BDF"
        echo "driver: $BOUND"
        echo
        echo "Refusing to read BAR0 registers with a driver active.  See the"
        echo "header of cx3diag-run.sh: that is what wedged the machine twice."
    } >> "$OUTDIR/09-skipped.txt"
    [ -n "$MEDIA" ] && echo "skipped" > "$MEDIA/CX3DIAG.DONE" 2>/dev/null
    sync; sleep 3
    reboot -f 2>/dev/null || echo b > /proc/sysrq-trigger 2>/dev/null || /sbin/reboot -f
    exit 0
fi
say "  no driver bound: safe to read"

note() {
    # capture one dword, and say whether the read itself worked
    if [ -z "$BAR0BASE" ]; then echo "NO-BAR0"; return; fi
    phys=$((BAR0BASE + $1))
    out=$(devmem2 "$(printf '0x%x' "$phys")" w 2>&1)
    v=$(echo "$out" | sed -n 's/.*): \(0x[0-9A-Fa-f]*\)[[:space:]]*$/\1/p' | tail -1)
    if [ -z "$v" ]; then
        # no value: keep the reason, it distinguishes tool failure from silence
        r=$(echo "$out" | tr '\n' ' ' | cut -c1-90)
        echo "FAIL($r)"
    else
        echo "$v"
    fi
}

{
    echo "===== BAR0 readback, no driver bound ====="
    echo "date:      $(date 2>/dev/null)"
    echo "BDF:       $BDF"
    echo "BAR0 base: ${BAR0BASE:-unknown}"
    echo "driver:    $(readlink /sys/bus/pci/devices/$BDF/driver 2>/dev/null || echo none)"
    echo "method:    devmem2 (mmap of /dev/mem, iomem=relaxed)"
    echo
    echo "macOS, in the same unclaimed state, reports for these offsets:"
    echo "    0x8069c = 0x01000000   (the ownership word across 10 boots)"
    echo "    0xf03fc = 0x01000000   (the reset semaphore)"
    echo "    0x80698 = 0x63144526   (HCR status, raw)"
    echo "    0x6e000 and 0x77000    whole pages of 0x01000000"
    echo
    echo "Each of the three key registers is read three times in a row, because"
    echo "macOS shows a first-read-zero artifact at these addresses."
    echo
    echo "--- 0x8069c ownership word, three consecutive reads ---"
    for i in 1 2 3; do printf '  read %d: %s\n' "$i" "$(note 0x8069c)"; done
    echo
    echo "--- 0xf03fc reset semaphore, three consecutive reads ---"
    for i in 1 2 3; do printf '  read %d: %s\n' "$i" "$(note 0xf03fc)"; done
    echo
    echo "--- 0x80698 HCR status, three consecutive reads ---"
    for i in 1 2 3; do printf '  read %d: %s\n' "$i" "$(note 0x80698)"; done
    echo
    echo "--- the HCR block, one read each (no driver, so nothing is in flight) ---"
    for o in 0x80680 0x80684 0x80688 0x8068c 0x80690 0x80694 0x806a0; do
        printf '  +%s  %s\n' "$o" "$(note "$o")"
    done
    echo
    echo "--- the reset block ---"
    for o in 0xf0000 0xf0004 0xf0008 0xf000c 0xf0010 0xf0014 0xf0018 0xf001c \
             0xf03f0 0xf03f4 0xf03f8; do
        printf '  +%s  %s\n' "$o" "$(note "$o")"
    done
    echo
    echo "--- whole pages that read 0x01000000 on macOS ---"
    for o in 0x6e000 0x6e004 0x6e008 0x6e00c 0x77000 0x77004 0x77008 0x7700c; do
        printf '  +%s  %s\n' "$o" "$(note "$o")"
    done
    echo
    echo "--- other macOS readings, for reference ---"
    for o in 0x0 0x1000 0x40000 0x40004 0x59000 0x70000 0x81000; do
        printf '  +%s  %s\n' "$o" "$(note "$o")"
    done
} >> "$OUTDIR/10-bar0-readback.txt"
say "    wrote 10-bar0-readback.txt"

# A second pass immediately after, to see whether anything moved.
{
    echo "===== second pass, same offsets, a second later ====="
    for o in 0x8069c 0xf03fc 0x80698 0x80680 0xf0010; do
        printf '  +%s  %s\n' "$o" "$(note "$o")"
    done
} >> "$OUTDIR/11-bar0-second-pass.txt"
say "    wrote 11-bar0-second-pass.txt"

# ---------------------------------------------------------------------------
# mstflint, with no driver bound, which is how its documentation assumes the
# device is accessed.  The earlier failure named 0xf0014, a register in the
# reset block that mlx4 also touches, so a clean run here would also settle
# whether that failure was the driver or the card.
# ---------------------------------------------------------------------------
banner "mstflint"
[ -n "$BDF" ] && {
    cap 20-mstflint.txt "mstflint -d <bdf> q (no driver bound)" mstflint -d "$BDF" q
    cap 21-mstconfig.txt "mstconfig -d <bdf> q (no driver bound)" mstconfig -d "$BDF" q
}
cap 22-mstflint-version.txt "mstflint --version" mstflint --version

# ---------------------------------------------------------------------------
# Done.
# ---------------------------------------------------------------------------
banner "Finished"
say "  results directory: $OUTDIR"
for f in "$OUTDIR"/*.txt; do
    [ -f "$f" ] && say "    $(basename "$f")  ($(wc -c < "$f" 2>/dev/null) bytes)"
done
{
    echo "run finished: $(date 2>/dev/null)"
    echo "media: $MEDIA"
    echo "outdir: $OUTDIR"
} >> "$OUTDIR/99-done.txt" 2>/dev/null
[ -n "$MEDIA" ] && echo "readback completed $(date 2>/dev/null)" > "$MEDIA/CX3DIAG.DONE" 2>/dev/null

sync
sleep 3
say ""
say "  rebooting in 5 seconds -- back to the normal boot."
say "  then boot macOS and read <USB>/out/run-*/ in Finder."
say ""
sleep 5
sync
reboot -f 2>/dev/null || echo b > /proc/sysrq-trigger 2>/dev/null || /sbin/reboot -f
