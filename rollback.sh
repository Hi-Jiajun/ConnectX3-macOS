#!/bin/bash
# Remove Cx3Mlx4 from the auxiliary kernel collection and restore the persistent
# state backed up by install.sh.
#
# Deliberately does NOT use `kmutil clear-staging`: that would also drop the
# NullMoth / HighPoint kexts staged on this machine.
#
# Run as:  sudo tools/cx3-mlx4/rollback.sh [path/to/state-backup-YYYYmmdd-HHMMSS]
set -uo pipefail

ROOT=/Users/you/Documents/Hackintosh-MacLocal
KEXT=/Library/Extensions/Cx3Mlx4.kext
ID=com.hijiajun.driver.Cx3Mlx4
INSTR=/var/db/KernelExtensionManagement/AuxKC/CurrentAuxKC/com.apple.kcgen.instructions.plist
UAKL=/var/db/KernelExtensionManagement/AuxKC/CurrentAuxKC/com.apple.kcgen.uakl.plist
PURGE="${1:-}"

[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }

echo "=== current staged entries for $ID ==="
grep -c "$ID" "$INSTR" 2>/dev/null || echo "  instructions.plist: not present"
grep -c "$ID" "$UAKL"   2>/dev/null || echo "  uakl.plist: not present"

echo
echo "=== 1. kmutil unload ==="
if [ -d "$KEXT" ]; then
    kmutil unload -p "$KEXT" 2>&1 | sed 's/^/  /'
else
    echo "  bundle missing at $KEXT, skipping unload"
fi

echo
echo "=== 2. verify no residue ==="
for f in "$INSTR" "$UAKL"; do
    if grep -q "$ID" "$f" 2>/dev/null; then
        echo "  RESIDUE in $f"
    else
        echo "  clean: $f"
    fi
done

echo
echo "=== 3. rebuild the auxiliary kernel collection ==="
kmutil rebuild 2>&1 | sed 's/^/  /'

echo
echo "=== 4. remove the installed bundle ==="
if [ "$PURGE" = "--purge" ]; then
    if [ -d "$KEXT" ]; then
        rm -rf "$KEXT" && echo "  removed $KEXT"
    else
        echo "  nothing to remove"
    fi
else
    echo "  kept $KEXT (pass --purge to delete it as well)"
fi

echo
echo "If any RESIDUE line appeared above, restore the matching backup with:"
echo "  /usr/bin/ditto <state-backup-dir>/KernelExtensionManagement /var/db/KernelExtensionManagement"
echo "Only remove the entries belonging to $ID; never blanket-clear the staging area."
echo "A reboot is required for the removal to take effect."
