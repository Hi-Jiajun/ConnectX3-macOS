#!/bin/bash
#
# Add one partition to a USB stick's MBR for the diagnostic payload, without
# touching the partitions already on it.
#
# Written because the stick in hand already carries someone else's payload on
# partition 1: this adds a new partition in the unused space, backs up the
# partition table sector first, and verifies afterwards that the existing
# entries are byte-for-byte unchanged.  Nothing outside the new partition is
# altered, and the table can be put back from the backup.
#
# Prints what it would do unless --apply is given.
#
# Usage:
#   prepare-usb-partition.sh /dev/disk2 2G            # dry run
#   sudo prepare-usb-partition.sh /dev/disk2 2G --apply

set -uo pipefail

DISK="${1:-}"
SIZE="${2:-2G}"
APPLY="${3:-}"

die() { echo "error: $*" >&2; exit 1; }

[ -n "$DISK" ] || die "usage: $0 /dev/diskX <size, e.g. 2G> [--apply]"
[ -b "$DISK" ] || die "$DISK is not a block device"
[ "$APPLY" = "--apply" ] && [ "$(id -u)" -ne 0 ] && die "run with sudo to apply"

# Size: accept 2G style.
case "$SIZE" in
    *G|*g) SECTORS=$(( ${SIZE%[Gg]} * 1024 * 1024 * 1024 / 512 )) ;;
    *M|*m) SECTORS=$(( ${SIZE%[Mm]} * 1024 * 1024 / 512 )) ;;
    *[0-9]) SECTORS="$SIZE" ;;                       # already in sectors
    *) die "cannot parse size '$SIZE' (use 2G or a sector count)" ;;
esac

echo "disk:    $DISK"
echo "new:     $SIZE  ($SECTORS sectors)"
echo

# A block device reports st_size 0 on macOS, so the device capacity comes from
# diskutil instead of from stat().
TOTAL_BYTES=$(diskutil info -plist "$DISK" 2>/dev/null | plutil -extract TotalSize raw -o - - 2>/dev/null)
[ -n "${TOTAL_BYTES:-}" ] || die "could not read the size of $DISK"
TOTAL_SECTORS=$(( TOTAL_BYTES / 512 ))
echo "device total: $TOTAL_SECTORS sectors"
echo

python3 - "$DISK" "$SECTORS" "$APPLY" "$TOTAL_SECTORS" <<'PY'
import os, struct, sys, subprocess, time

disk, size_s, apply_s, total = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4])
apply = (apply_s == "--apply")

SECTOR = 512

# ---- read the MBR ---------------------------------------------------------
with open(disk, "rb") as f:
    mbr = bytearray(f.read(4096))

if mbr[510:512] != b"\x55\xaa":
    sys.exit("error: %s does not start with an MBR signature" % disk)

def entry(i):
    o = 446 + i * 16
    return mbr[o:o + 16]

def parse(e):
    ptype = e[4]
    start = struct.unpack("<I", e[8:12])[0]
    count = struct.unpack("<I", e[12:16])[0]
    bootable = e[0] == 0x80
    return ptype, start, count, bootable

print("existing partition table:")
free_slot = None
end_max = 0
for i in range(4):
    e = entry(i)
    ptype, start, count, bootable = parse(e)
    if ptype == 0 or count == 0:
        if free_slot is None:
            free_slot = i
        print("  slot %d: empty" % (i + 1))
        continue
    print("  slot %d: type 0x%02X start %d size %d%s"
          % (i + 1, ptype, start, count, "  (bootable)" if bootable else ""))
    end_max = max(end_max, start + count)

if free_slot is None:
    sys.exit("error: no free primary partition slot; a logical partition would be needed")

print()
print("device sectors    : %d" % total)
print("first free sector : %d" % end_max)
print("free sectors      : %d" % (total - end_max))

# Align the new partition up to 1 MiB, which is what every other tool does.
align = 2048
start = ((end_max + align - 1) // align) * align
if start + size_s > total:
    sys.exit("error: not enough room (need %d sectors from %d, device ends at %d)"
             % (size_s, start, total))

print("new partition     : slot %d, type 0x0C, start %d, size %d"
      % (free_slot + 1, start, size_s))
print("  covers sectors %d..%d (%.1f GB of a %.1f GB device)"
      % (start, start + size_s - 1, size_s * SECTOR / 1e9, total * SECTOR / 1e9))
print()

if not apply:
    print("dry run -- nothing written.  Re-run with --apply to commit.")
    sys.exit(0)

# ---- back up the table ----------------------------------------------------
backup = "/tmp/%s-mbr-backup-%s.bin" % (os.path.basename(disk), time.strftime("%Y%m%d-%H%M%S"))
with open(backup, "wb") as f:
    f.write(mbr[:512])
print("partition table backed up to %s" % backup)

# ---- build the new entry --------------------------------------------------
# CHS fields are filled with the maximum values, which is the conventional
# "use LBA, CHS is out of range" marker for partitions past the 1024-cylinder
# limit.  Firmware on this machine already boots a type-0x0C partition, so the
# same type is used rather than an EFI System type.
new = bytearray(16)
new[0] = 0x00
new[1:4] = b"\xfe\xff\xff"                     # start CHS
new[4] = 0x0C                                  # FAT32 LBA
new[5:8] = b"\xfe\xff\xff"                     # end CHS
new[8:12] = struct.pack("<I", start)
new[12:16] = struct.pack("<I", size_s)

orig_entries = [bytes(entry(i)) for i in range(4)]
mbr[446 + free_slot * 16: 446 + (free_slot + 1) * 16] = new
mbr[510:512] = b"\x55\xaa"

with open(disk, "r+b") as f:
    f.write(mbr[:512])
    f.flush()
    os.fsync(f.fileno())
time.sleep(1)

# ---- verify ---------------------------------------------------------------
with open(disk, "rb") as f:
    back = f.read(512)

ok = True
for i in range(4):
    if i == free_slot:
        continue
    if bytes(back[446 + i * 16: 446 + (i + 1) * 16]) != orig_entries[i]:
        print("  MISMATCH in slot %d" % (i + 1))
        ok = False

print()
print("verification:")
print("  signature 0xAA55 : %s" % ("present" if back[510:512] == b"\x55\xaa" else "MISSING"))
print("  other entries    : %s" % ("unchanged" if ok else "CHANGED -- restore %s" % backup))
if not ok:
    sys.exit(1)

ptype, s, c, b = parse(bytes(back[446 + free_slot * 16: 446 + (free_slot + 1) * 16]))
print("  new entry        : slot %d type 0x%02X start %d size %d" % (free_slot + 1, ptype, s, c))
print()
print("Restore the old table with:")
print("    sudo dd if=%s of=%s bs=512 count=1" % (backup, disk))
PY

echo
if [ "$APPLY" = "--apply" ]; then
    echo "next: format the new partition and copy the payload, e.g."
    echo "    diskutil list $DISK"
    echo "    sudo newfs_msdos -F 32 -v CX3DIAG ${DISK}s2"
else
    echo "dry run only."
fi
