#!/bin/bash
#
# Build the payload for the Cx3Mlx4 Linux diagnostic USB stick, and optionally
# write it onto a prepared FAT32 partition.
#
# The payload is an Alpine Linux live system plus:
#   - an apkovl that enables one extra boot service, which runs cx3diag-run.sh
#   - a grub.cfg with that service's parameters and no "quiet"
#   - lspci and mstflint, which Alpine's own media does not carry
#   - a marker file and an out/ directory, so the live system can find the stick
#     after boot and write its findings there
#
# Usage:
#   make-usb.sh                        build the payload, print where it is
#   sudo make-usb.sh --write /dev/diskXsY
#                                      also copy it onto that FAT32 partition
#
# No destructive command is used: every run builds into a fresh temporary
# directory, and --write only copies files into an existing partition named
# explicitly on the command line.

set -uo pipefail

ISO_VER=3.20.10
ISO_NAME="alpine-standard-${ISO_VER}-x86_64.iso"
ISO_URL="https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/${ISO_NAME}"
ISO_SHA=f4c54c81aa7ba8b1c14f92c8b1d5456d4b4c1aa76d0be699e24b7b2e29c5bd0a

ALP_MAIN="https://dl-cdn.alpinelinux.org/alpine/v3.20/main/x86_64"
ALP_EDGE_TESTING="https://dl-cdn.alpinelinux.org/alpine/edge/testing/x86_64"
ALP_COMMUNITY="https://dl-cdn.alpinelinux.org/alpine/v3.20/community/x86_64"

HERE=$(cd "$(dirname "$0")" && pwd)
WORK="${WORK:-$(mktemp -d /tmp/cx3-usb.XXXXXX)}"

die() { echo "error: $*" >&2; exit 1; }
step() { echo; echo "=== $* ==="; }

# ---------------------------------------------------------------------------
step "workspace: $WORK"
mkdir -p "$WORK"

ISO="$WORK/$ISO_NAME"
if [ ! -f "$ISO" ]; then
    step "downloading Alpine $ISO_VER"
    curl -sS -L --fail -m 900 -o "$ISO" "$ISO_URL" || die "download failed"
fi

step "verifying the ISO"
got=$(shasum -a 256 "$ISO" | awk '{print $1}')
if [ "$got" != "$ISO_SHA" ]; then
    die "ISO checksum mismatch
    expected $ISO_SHA
    got      $got"
fi
echo "  sha256 OK, $(wc -c < "$ISO" | tr -d ' ') bytes"

# ---------------------------------------------------------------------------
step "extracting the ISO"
STAGE="$WORK/stage"
mkdir -p "$STAGE"
# A previous run leaves the ISO's read-only directories in place, and tar cannot
# replace files inside a directory it may not write to.  Grant write access
# first so a re-run overwrites cleanly, with nothing deleted.
chmod -R u+w "$STAGE" 2>/dev/null
( cd "$STAGE" && tar -xf "$ISO" ) || die "could not extract the ISO"
[ -f "$STAGE/boot/vmlinuz-lts" ] || die "extraction looks wrong: no boot/vmlinuz-lts"
# The ISO's directories come out read-only, which would stop the boot
# configuration being installed over the ISO's own copies further down.  The
# FAT filesystem the payload ends up on carries no Unix permissions anyway.
chmod -R u+w "$STAGE"
echo "  $(du -sh "$STAGE" | cut -f1) extracted"

# ---------------------------------------------------------------------------
step "downloading the packages Alpine's media lacks"
PKG="$WORK/pkgs"
mkdir -p "$PKG"

fetch() {
    local url="$1/$2"
    if [ ! -s "$PKG/$2" ]; then
        curl -sS -L --fail -m 300 -o "$PKG/$2" "$url" || die "download failed: $url"
    fi
    printf '  %-48s %9s bytes\n' "$2" "$(wc -c < "$PKG/$2" | tr -d ' ')"
}

# lspci and the library it needs
fetch "$ALP_MAIN" pciutils-3.12.0-r1.apk
fetch "$ALP_MAIN" pciutils-libs-3.12.0-r1.apk
fetch "$ALP_MAIN" hwdata-pci-0.382-r0.apk

# mstflint is musl-native but lives in Alpine's edge/testing tree, and needs the
# C++ and unwind runtimes that the standard ISO does not ship.
fetch "$ALP_EDGE_TESTING" mstflint-4.35.0.1-r0.apk
fetch "$ALP_MAIN" libstdc++-13.2.1_git20240309-r1.apk
fetch "$ALP_MAIN" libgcc-13.2.1_git20240309-r1.apk

# devmem2: the only way to read a device register from user space here.  The
# sysfs resource0 file only supports mmap, not read, and Alpine's busybox has no
# devmem applet, so both of the first attempts failed.  devmem2 mmaps /dev/mem.
fetch "$ALP_COMMUNITY" devmem2-0.2-r0.apk

# ---------------------------------------------------------------------------
step "building the apkovl"
OVL_ROOT="$WORK/apkovl-root"
mkdir -p "$OVL_ROOT/etc/local.d" "$OVL_ROOT/etc/runlevels/default"

# This marker is what makes Alpine add its default boot services, which include
# modloop -- the service that makes kernel modules (mlx4 among them) available.
# Ship an apkovl without it and that setup is silently skipped.
: > "$OVL_ROOT/etc/.default_boot_services"
echo "  etc/.default_boot_services (marker)"

install -m 0755 "$HERE/cx3diag-run.sh" "$OVL_ROOT/etc/local.d/zz-cx3diag.start"
echo "  etc/local.d/zz-cx3diag.start"

# Enable the local service.  alpine-baselayout ships no runlevels entries, so
# this symlink cannot be clobbered when apk installs the base packages.
ln -sf /etc/init.d/local "$OVL_ROOT/etc/runlevels/default/local"
echo "  etc/runlevels/default/local -> /etc/init.d/local"

# An apk is a tar archive with a signature and .PKGINFO ahead of the payload, so
# extracting all of it and dropping the metadata yields exactly the files to
# install.  No apk database is involved, so this works with no network.
for a in "$PKG"/*.apk; do
    tar -xzf "$a" -C "$OVL_ROOT" \
        --exclude='.PKGINFO' --exclude='.SIGN*' \
        --exclude='usr/include' --exclude='usr/include/*' 2>/dev/null \
        || echo "  warning: partial extract from $(basename "$a")"
done
echo "  bundled:"
for t in usr/bin/lspci usr/bin/mstflint usr/bin/mstconfig usr/bin/devmem2; do
    [ -e "$OVL_ROOT/$t" ] && printf '    %-24s %9s bytes\n' "$t" "$(wc -c < "$OVL_ROOT/$t" | tr -d ' ')"
done
for l in usr/lib/libpci.so.3 usr/lib/libstdc++.so.6 usr/lib/libgcc_s.so.1; do
    [ -e "$OVL_ROOT/$l" ] && printf '    %-24s present\n' "$l"
done

OVL="$STAGE/cx3diag.apkovl.tar.gz"
( cd "$OVL_ROOT" && COPYFILE_DISABLE=1 tar --no-mac-metadata \
      --uid 0 --gid 0 --uname root --gname root -czf "$OVL" etc usr ) \
    || die "could not build the apkovl"
echo "  $(basename "$OVL"): $(wc -c < "$OVL" | tr -d ' ') bytes"

step "apkovl contents"
tar -tzf "$OVL" | sed 's/^/    /' | head -26

# ---------------------------------------------------------------------------
step "installing the boot configuration"
install -m 0644 "$HERE/grub.cfg" "$STAGE/boot/grub/grub.cfg"
# Belt and braces: if GRUB's compiled-in prefix points elsewhere, a config next
# to the loader is picked up instead.
install -m 0644 "$HERE/grub.cfg" "$STAGE/efi/boot/grub.cfg"
printf '%s\n' \
  'TIMEOUT 50' \
  'DEFAULT lts' \
  'LABEL lts' \
  '  MENU LABEL Cx3Mlx4 Linux diagnostic' \
  '  KERNEL /boot/vmlinuz-lts' \
  '  INITRD /boot/initramfs-lts' \
  '  APPEND modules=loop,squashfs,sd-mod,usb-storage console=tty0' \
  > "$STAGE/boot/syslinux/syslinux.cfg"
echo "  boot/grub/grub.cfg, efi/boot/grub.cfg, boot/syslinux/syslinux.cfg"

step "verifying the boot configuration landed"
grep -H "apkovl\|vmlinuz-lts\|timeout" \
     "$STAGE/boot/grub/grub.cfg" "$STAGE/efi/boot/grub.cfg" 2>/dev/null | sed 's/^/    /'

# ---------------------------------------------------------------------------
step "adding the stick's own markers"
: > "$STAGE/CX3DIAG.MARK"
mkdir -p "$STAGE/out"
printf '%s\n' \
  'Cx3Mlx4 Linux diagnostic stick' \
  '' \
  'Boot this stick from the firmware boot menu. It will:' \
  '' \
  '  1. start Alpine Linux from this stick (nothing is installed anywhere)' \
  '  2. load the reference mlx4 driver and record what it reports' \
  '  3. read the card identity with lspci and mstflint' \
  '  4. write everything under out/run-<timestamp>/ on THIS stick' \
  '  5. reboot on its own, so the machine returns to its normal boot' \
  '' \
  'Then boot macOS as usual and read out/run-<timestamp>/ in Finder.' \
  'The files are plain text.' \
  '' \
  'Nothing here writes to any disk other than this stick.' \
  > "$STAGE/README-ON-STICK.txt"
echo "  CX3DIAG.MARK, out/, README-ON-STICK.txt"

# ---------------------------------------------------------------------------
step "payload ready"
echo "  path:  $STAGE"
echo "  size:  $(du -sh "$STAGE" | cut -f1)"
echo "  files: $(find "$STAGE" -type f | wc -l | tr -d ' ')"
du -sh "$STAGE"/boot/modloop-lts "$STAGE"/boot/initramfs-lts "$STAGE"/boot/vmlinuz-lts 2>/dev/null | sed 's/^/    /'

# ---------------------------------------------------------------------------
if [ "${1:-}" = "--write" ]; then
    DEV="${2:-}"
    [ -n "$DEV" ] || die "--write needs a device, e.g. --write /dev/disk4s2"
    [ -b "$DEV" ] || die "$DEV is not a block device"
    [ "$(id -u)" -eq 0 ] || die "run with sudo to write to $DEV"

    step "target check: $DEV"
    info=$(diskutil info "$DEV" 2>/dev/null)
    echo "$info" | grep -E "Volume Name|File System Personality|Mount Point|Disk Size" | sed 's/^/  /'
    personality=$(echo "$info" | awk -F': *' '/File System Personality/{print $2}')
    case "$personality" in
        *FAT*) : ;;
        *) die "refusing: $DEV reports '$personality', not FAT" ;;
    esac

    MP=$(echo "$info" | awk -F': *' '/Mount Point/{print $2}')
    if [ -z "$MP" ] || [ "$MP" = "Not mounted" ]; then
        diskutil mount "$DEV" >/dev/null || die "could not mount $DEV"
        MP=$(diskutil info "$DEV" | awk -F': *' '/Mount Point/{print $2}')
    fi
    [ -d "$MP" ] || die "no mount point for $DEV"
    echo "  mount point: $MP"
    echo "  copying..."
    COPYFILE_DISABLE=1 /usr/bin/ditto "$STAGE" "$MP" || die "copy failed"
    sync
    echo "  done: $(find "$MP" -type f | wc -l | tr -d ' ') files on the stick"
else
    echo
    echo "  To write it onto a prepared FAT32 partition:"
    echo "      sudo $0 --write /dev/diskXsY"
fi
