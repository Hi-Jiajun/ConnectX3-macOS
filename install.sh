#!/bin/bash
# Stage Cx3Mlx4 into the auxiliary kernel collection.
#
# Requires root and a reboot (macOS 15 has no live kext loading: kextutil -n is
# gone, and kmutil load only stages for the next boot).  This script:
#   1. runs the offline validation,
#   2. backs up the three persistent stores that kmutil load touches,
#   3. stages the kext.
#
# Run as:  sudo tools/cx3-mlx4/install.sh [path/to/Cx3Mlx4.kext]
set -uo pipefail

ROOT=/Users/you/Documents/Hackintosh-MacLocal
SRC_KEXT="${1:-$ROOT/outputs/20261010-cx3-m1/build/Cx3Mlx4.kext}"
KEXT=/Library/Extensions/Cx3Mlx4.kext
ID=com.hijiajun.driver.Cx3Mlx4
STAMP=$(date +%Y%m%d-%H%M%S)
BACKUP="$ROOT/outputs/20261010-cx3-m1/state-backup-$STAMP"

[ -d "$SRC_KEXT" ] || { echo "built kext not found: $SRC_KEXT"; exit 1; }
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }

echo "=== bundle ==="
plutil -p "$SRC_KEXT/Contents/Info.plist" | grep -E 'CFBundleIdentifier|IOPCIPrimaryMatch|IOProbeScore|IOMatchCategory'

echo
echo "=== 1. offline symbol check (authoritative) ==="
kmutil libraries -r "$(dirname "$SRC_KEXT")" -p "$SRC_KEXT" > /dev/null 2>&1
rc=$?
echo "kmutil libraries rc=$rc"
[ "$rc" -eq 0 ] || { echo "refusing to install: unresolved symbols"; exit 1; }

echo
echo "=== 2. backup persistent state -> $BACKUP ==="
mkdir -p "$BACKUP"
/usr/bin/ditto /var/db/KernelExtensionManagement "$BACKUP/KernelExtensionManagement" \
    && echo "  saved KernelExtensionManagement/"
[ -f /private/var/db/SystemPolicyConfiguration/UAKL.plist ] && \
    /bin/cp /private/var/db/SystemPolicyConfiguration/UAKL.plist "$BACKUP/UAKL.plist" && \
    echo "  saved UAKL.plist"
[ -f /var/db/SystemPolicyConfiguration/KextPolicy ] && \
    /bin/cp /var/db/SystemPolicyConfiguration/KextPolicy "$BACKUP/KextPolicy" && \
    echo "  saved KextPolicy"
/usr/bin/ditto /Library/KernelCollections "$BACKUP/KernelCollections" 2>/dev/null && \
    echo "  saved Library/KernelCollections/"

echo
echo "=== 3. install to $KEXT (root:wheel, 0755) ==="
# kmutil load validates ownership: a bundle owned by the invoking user is
# rejected with KMErrorDomain Code=30 "Invalid ownership (501:20) should be
# (0:0)".  /Library/Extensions is also where this machine's other third-party
# kexts live, and it is a stable path for the auxiliary collection build.
rm -rf "$KEXT"
/usr/bin/ditto "$SRC_KEXT" "$KEXT"
chown -R root:wheel "$KEXT"
chmod -R go-w "$KEXT"
ls -ld "$KEXT" "$KEXT/Contents" "$KEXT/Contents/MacOS" "$KEXT/Contents/MacOS/Cx3Mlx4"

echo
echo "=== 4. stage the kext ==="
kmutil load -z -p "$KEXT"
rc=$?
echo "kmutil load rc=$rc"
case "$rc" in
    0|28) echo "  staged (rc 28 = 'requires a reboot' is the expected success path)" ;;
    *)    echo "  FAILED; roll back with: sudo tools/cx3-mlx4/rollback.sh" ;;
esac

echo
echo "=== staged state ==="
echo "--- instructions.plist ---"
plutil -p /var/db/KernelExtensionManagement/AuxKC/CurrentAuxKC/com.apple.kcgen.instructions.plist \
    | grep -A3 -i "$ID" || echo "  (identifier not present - check kmutil load output)"
echo "--- UAKL ---"
plutil -p /var/db/KernelExtensionManagement/AuxKC/CurrentAuxKC/com.apple.kcgen.uakl.plist

echo
echo "Next: reboot, then collect evidence with"
echo "  ioreg -l -p IOService | grep -A40 'pcidebug\" = \"7:0:0'"
echo "  /usr/bin/log show --last 3m --predicate 'eventMessage CONTAINS \"Cx3Mlx4\"'"

echo
echo "=== 5. install the user-space control tool ==="
# Nothing that writes to the device runs at boot any more; it is all reached
# through this tool, on a live system, as root.
TOOL_SRC="$(dirname "$SRC_KEXT")/cx3ctl"
if [ -x "$TOOL_SRC" ]; then
    install -m 0755 -o root -g wheel "$TOOL_SRC" /usr/local/bin/cx3ctl 2>/dev/null \
        || { mkdir -p /usr/local/bin && cp "$TOOL_SRC" /usr/local/bin/cx3ctl && \
             chown root:wheel /usr/local/bin/cx3ctl && chmod 755 /usr/local/bin/cx3ctl; }
    echo "  installed /usr/local/bin/cx3ctl"
else
    echo "  cx3ctl not found next to the bundle; build it with build.sh"
fi

echo
echo "After the reboot, in this order:"
echo "  sudo cx3ctl probe      # read-only; confirms the device still binds"
echo "  sudo cx3ctl dma        # DMA allocation only, no device access"
echo "  sudo cx3ctl queryfw    # the step that previously hung the machine"
echo "  sudo cx3ctl reset      # only if queryfw shows a reset is needed"
