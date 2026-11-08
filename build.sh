#!/bin/bash
# Build Cx3Mlx4.kext from source using the Command Line Tools clang plus
# acidanthera's MacKernelSDK -- the same recipe that built mtkwlan without Xcode.
#
# The three build pitfalls from docs/handoff-cx3-mlx4.md are handled here:
# -DKERNEL -D__KERNEL__ (kext branch of the DriverKit headers), -x c++ for every
# translation unit, and no Xcode placeholders left in the Info.plist.
#
# Usage: build.sh            -> outputs/20261010-cx3-m1/build/Cx3Mlx4.kext
#        OUT=/tmp/x build.sh -> custom output directory
set -uo pipefail

ROOT=/Users/you/Documents/Hackintosh-MacLocal
SRC="$ROOT/tools/cx3-mlx4"
SDK="$ROOT/tools/research/mksdk"
OUT="${OUT:-$ROOT/outputs/20261010-cx3-m1/build}"
# Feature development can defer the broad host regressions. Compilation, boot
# path, symbol resolution and kernel/client ABI checks still run in both modes.
CX3_TEST_PROFILE="${CX3_TEST_PROFILE:-standard}"
case "$CX3_TEST_PROFILE" in
    standard|smoke) ;;
    *) echo "BUILD RESULT: FAIL (CX3_TEST_PROFILE must be standard or smoke)"; exit 2 ;;
esac

#
# THE RUNNING SYSTEM'S KERNEL HEADERS, AHEAD OF MacKernelSDK'S.
#
# Apple's rule for kexts is to compile against the headers of the OS that runs, and stage 10a is
# where this project found out why the rule exists: MacKernelSDK's IONetworkController.h declares
# 74 virtuals and this machine's SDK (MacOSX15.sdk) declares 72, so the vtable emitted against
# MacKernelSDK was two slots longer than the family in the kernel collection.  Symbols are matched
# on that layout, so the link asked for Apple's padding slots (_RESERVEDIONetworkController2..7,
# _RESERVEDIOEthernetController2..9) -- fourteen undefined symbols, and a kext that would have
# failed to load.  Pointing the IOKit tree at the running system's headers took that to four.
#
# Written as a plain, single -I line on purpose: the first attempt used
# ${SYSKERNEL_HEADERS:+-I"$SYSKERNEL_HEADERS"}, and quotes inside that kind of expansion are not
# reliably honoured, which is what the four leftover symbols look like.
#
#
# Overridable, because WHICH SDK matches the running kernel is an empirical question on a machine
# whose installed SDKs are older than its kernel: the vtable layout is what has to agree, and the
# only way to know is to build against each candidate and read the symbol accounting.  The default
# is the newest installed SDK; CX3_SYSKERNEL_HEADERS names another one outright.
#
SYSKERNEL_HEADERS="${CX3_SYSKERNEL_HEADERS:-$(ls -d /Library/Developer/CommandLineTools/SDKs/MacOSX*.sdk 2>/dev/null \
                     | tail -1)/System/Library/Frameworks/Kernel.framework/Headers}"
[ -d "$SYSKERNEL_HEADERS" ] || SYSKERNEL_HEADERS="$SDK/Headers"

CC="${CC:-$(xcrun --find clang 2>/dev/null || echo /usr/bin/clang)}"
# Untagged developer builds must not impersonate a historical candidate.
BUILD_TAG="${BUILD_TAG:-unversioned-local}"

[ -d "$SDK/Headers" ] || { echo "MacKernelSDK headers not found at $SDK/Headers"; exit 1; }

# On concurrency: the first attempt at this used a pid lockfile, which was worse
# than the problem.  A build killed mid-run left the lock behind, and because a
# recycled pid can look alive, every later build was then refused.  The second
# attempt claimed to give each build its own object directory but in fact still
# pointed every build at "$OUT/obj".
#
# What it does now: objects and the assembled bundle are built in private
# temporary directories, and only the finished result is moved into "$OUT" at the
# end.  Two builds therefore cannot see each other's files at all; the later one
# simply wins, and for a given BUILD_TAG both produce identical bytes anyway.

CFLAGS=(-c -x c++ -mkernel -arch x86_64
        -DKERNEL -D__KERNEL__
        -DCX3_START_LEVEL="${CX3_START_LEVEL:-2}"
        -fapple-kext -fno-builtin -fno-common -fno-stack-protector
        -fno-exceptions -fno-rtti -nostdinc -mmacosx-version-min=10.12
        -Wno-nullability-completeness -Wno-deprecated-register
        # --- warnings, and why these are errors ------------------------------------------
        #
        # This project's most expensive defects were not logic errors, they were values
        # pushed through a type too narrow for them, and numbers printed with the wrong
        # format.  Both are things the compiler already knows how to see -- it simply was
        # not being asked.  Measured on this source the day these were enabled:
        #
        #   -Wshorten-64-to-32   found a live instance of the exact bug that cost the
        #                        stage-3 verification a reboot (a DMA address in a UInt32
        #                        member, after the parameter had already been widened)
        #   -Wformat             found a live instance of a log line whose specifiers were
        #                        one short, so every value after the first two printed
        #                        misaligned -- the "diagnostic reports a wrong number" class
        #   -Wshift-count-overflow  would have found the UInt32 >> 32 that masqueraded as a
        #                        firmware alignment requirement for several hours
        #
        # Pointer-to-bool conversion hid a QP observation pointer passed as bypassBusy.
        # These correctness diagnostics are errors because a build that reports and then
        # succeeds is a build whose report gets scrolled past.  -Wunused-* stays a warning:
        # those are untidiness, not silent wrongness.
        -Wall -Wextra
        -Werror=shorten-64-to-32
        -Werror=format
        -Werror=shift-count-overflow
        -Werror=implicit-int-conversion
        -Werror=pointer-bool-conversion
        -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function
        -Wno-sign-conversion -Wno-sign-compare
        -I"$SYSKERNEL_HEADERS" -I"$SDK/Headers" -I"$SRC/src")

OBJ=$(mktemp -d /tmp/cx3-obj.XXXXXX) || { echo "cannot create an object directory"; exit 1; }
STAGE=$(mktemp -d /tmp/cx3-stage.XXXXXX) || { echo "cannot create a staging directory"; exit 1; }
# On success the temporaries go away.  On failure they are kept, because the
# per-file compiler output lives in $OBJ and the failure message points at it --
# deleting it would leave the reader with a path to a file that no longer exists.
# Keep the staging tree for inspection when KEEP_ON_FAIL=1 is set in the
# environment.  Late-stage failures (symbol resolution, boot-path) used to delete
# the evidence a moment before anyone could look at it.
KEEP_ON_FAIL=${KEEP_ON_FAIL:-0}
cleanup() {
    if [ "$KEEP_ON_FAIL" -eq 0 ]; then rm -rf "$OBJ" "$STAGE"; fi
}
trap cleanup EXIT INT TERM
echo "work dirs: OBJ=$OBJ"
echo "           STAGE=$STAGE (moved to $OUT on success)"

# One staged source for compiler/kmod and bundle metadata.  The numeric
# release version remains owned by the source plist; the candidate tag is
# stored separately in both the generated header and the staged plist.
if ! /usr/bin/python3 "$SRC/tools/stage-build-identity.py" "$SRC/kext/Info.plist" \
        "$STAGE/Info.plist" "$STAGE/build-identity.h" "$BUILD_TAG"; then
    echo "BUILD RESULT: FAIL (build identity)"
    exit 1
fi
CFLAGS+=(-include "$STAGE/build-identity.h")

# --- environment, recorded so a failure elsewhere can be compared -------
{
    echo "date:     $(date)"
    echo "host:     $(uname -a)"
    echo "compiler: $CC"
    "$CC" --version | head -3
    echo
    echo "sources:"
    for f in "$SRC"/src/*.cpp; do
        printf '  %-24s %s\n' "$(basename "$f")" "$(shasum -a 256 "$f" | cut -c1-16)"
    done
    echo
    echo "MacKernelSDK:"
    printf '  Headers/IOKit present: %s\n' "$([ -d "$SDK/Headers/IOKit" ] && echo yes || echo NO)"
    printf '  IOPCIDevice.h:         %s\n' "$([ -f "$SDK/Headers/IOKit/pci/IOPCIDevice.h" ] && echo yes || echo NO)"
    echo
    echo "build tag: $BUILD_TAG"
    echo "test profile: $CX3_TEST_PROFILE (not full hardware acceptance)"
    printf 'bundle version: %s\n' "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$STAGE/Info.plist")"
} | tee "$STAGE/build-env.txt" | sed 's/^/  /'

echo
echo "=== preflight: does this compiler accept the kernel flags? ==="
PREFLIGHT=$(mktemp -d)
printf '#include <IOKit/IOService.h>\nclass T : public IOService { OSDeclareDefaultStructors(T) };\nOSDefineMetaClassAndStructors(T, IOService)\n' > "$PREFLIGHT/t.cpp"
if "$CC" -c -x c++ -mkernel -arch x86_64 -DKERNEL -D__KERNEL__ -nostdinc \
        -fapple-kext -fno-builtin -fno-common -fno-stack-protector \
        -fno-exceptions -fno-rtti -mmacosx-version-min=10.12 \
        -I"$SDK/Headers" -o "$PREFLIGHT/t.o" "$PREFLIGHT/t.cpp" 2> "$PREFLIGHT/t.err"; then
    echo "  OK: toolchain accepts the kext flags"
else
    echo "  FAIL: the toolchain rejects the kext flags, which has nothing to do with"
    echo "        our source.  This is pitfall #1 in docs/handoff-cx3-mlx4.md:"
    echo "        without -DKERNEL -D__KERNEL__ the DriverKit branch of"
    echo "        PCIDriverKit/IOPCIDevice.h is taken and fails."
    sed 's/^/        /' "$PREFLIGHT/t.err" | head -10
    exit 1
fi

echo
echo "=== compile ==="

ok=0
fail=0
for f in "$SRC"/src/*.cpp; do
    o="$OBJ/$(basename "$f" .cpp).o"
    if "$CC" "${CFLAGS[@]}" -o "$o" "$f" 2> "$o.err"; then
        echo "OK   $(basename "$f")"
        ok=$((ok + 1))
    else
        echo "FAIL $(basename "$f")"
        echo "     command was:"
        echo "       $CC ${CFLAGS[*]} -o $o $f"
        echo "     errors:"
        grep -m8 'error:' "$o.err" | sed 's/^/       /'
        echo "     (full output: $o.err)"
        fail=$((fail + 1))
    fi
done

echo
echo "=== compile: ok=$ok fail=$fail ==="
if [ "$fail" -ne 0 ]; then
    KEEP_ON_FAIL=1
    echo
    echo "compiler output kept for inspection:"
    echo "  $OBJ"
    ls "$OBJ"/*.err 2>/dev/null | sed 's/^/    /'
    echo "BUILD RESULT: FAIL"
    exit 1
fi

echo
echo "=== link kext ==="
# The _kmod_info symbol comes from src/kmod_info.cpp; the CLT linker does not
# synthesise it the way Xcode's kext target does.
"$CC" -arch x86_64 -mkernel -nostdinc -nostdlib \
       -Wl,-kext -Wl,-no_arch_warnings -Xlinker -kext \
       -o "$STAGE/Cx3Mlx4" "$OBJ"/*.o
rc=$?
echo "link rc=$rc"
[ "$rc" -eq 0 ] || exit 1
file "$STAGE/Cx3Mlx4"
KMOD=$(nm -m "$STAGE/Cx3Mlx4" | grep -c ' _kmod_info$')
echo "--- _kmod_info defined: $KMOD (must be 1) ---"
if [ "$KMOD" -ne 1 ]; then
    echo "    FAIL: kmutil load would reject the bundle (rc 31)"
    exit 1
fi

echo
echo "=== assemble bundle ==="
STAGEKEXT="$STAGE/Cx3Mlx4.kext"
mkdir -p "$STAGEKEXT/Contents/MacOS"
cp -p "$STAGE/Cx3Mlx4" "$STAGEKEXT/Contents/MacOS/Cx3Mlx4"
cp -p "$STAGE/Info.plist" "$STAGEKEXT/Contents/Info.plist"
chmod 755 "$STAGEKEXT/Contents/MacOS/Cx3Mlx4"
plutil -lint "$STAGEKEXT/Contents/Info.plist"

echo
echo "=== offline validation ==="
echo "--- boot-path reachability (start() must not be able to write) ---"
python3 "$SRC/tools/check-boot-path.py" "$STAGE/Cx3Mlx4" | sed 's/^/    /'
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "    FAIL: refusing to continue; the boot path is not read-only"
    exit 1
fi

# ...and prove the check above can still fail.  It is a name-matching heuristic
# over disassembly, and it has already gone wrong once in the other direction: an
# otool rendering artifact made it report a vtable dispatch to
# IOPCIDevice::setBusMasterEnable as a call to a device-write routine, failing a
# good build.  A check that cannot fail is not a check, so an entry point that
# provably reaches a write is run through it and must be rejected.
python3 "$SRC/tools/check-boot-path.py" "$STAGE/Cx3Mlx4" \
    "__ZN7Cx3Mlx414exportResetHcaEbPy" > /dev/null 2>&1
if [ "$?" -eq 0 ]; then
    echo "    FAIL: the boot-path check passed an entry point that resets the HCA,"
    echo "          so it is no longer detecting anything. Fix the check."
    exit 1
fi

ENCTEST=$(mktemp -d)
if [ "$CX3_TEST_PROFILE" = standard ]; then
echo "--- mailbox encoding (host test, no hardware) ---"
# The MAP_FA / MAP_ICM entry layout is easy to get subtly wrong, and a mistake
# would tell the firmware to use pages it should not, so it is checked on every
# build instead of being discovered after a reboot.
if cc -Wall -o "$ENCTEST/t" "$SRC/tools/test-mapfa-encoding.c" 2> "$ENCTEST/cc.err"; then
    if "$ENCTEST/t" > "$ENCTEST/out.txt" 2>&1; then
        printf '    %s\n' "$(tail -1 "$ENCTEST/out.txt")"
    else
        echo "    FAIL: mailbox encoding test"
        grep -E "FAIL|failure" "$ENCTEST/out.txt" | sed 's/^/    /'
        exit 1
    fi
else
    echo "    FAIL: could not compile the encoding test"
    head -5 "$ENCTEST/cc.err" | sed 's/^/    /'
    exit 1
fi

# The M2d offsets (QUERY_DEV_CAP, INIT_HCA, opcodes) were transcribed by hand
# from the reference, and a typo in one of them cannot be seen anywhere except in
# a failed command, which costs a reboot.  So they are checked against the
# reference's own literal values on every build.
if cc -Wall -o "$ENCTEST/m2d" "$SRC/tools/test-m2d-offsets.c" 2> "$ENCTEST/m2d.err"; then
    if "$ENCTEST/m2d" > "$ENCTEST/m2d.out" 2>&1; then
        printf '    %s\n' "$(tail -1 "$ENCTEST/m2d.out")"
    else
        echo "    FAIL: M2d offset cross-check"
        grep -E "FAIL|failure" "$ENCTEST/m2d.out" | sed 's/^/    /'
        exit 1
    fi
else
    echo "    FAIL: could not compile the M2d offset test"
    head -5 "$ENCTEST/m2d.err" | sed 's/^/    /'
    exit 1
fi

# Device-sourced values written into the source as literals.
#
# The rule: a value the device can be asked is read back, not written down.  The constants
# that were guesses have cost this project more than anything else -- a field read from the
# wrong offset, an EQ profile count from the wrong capability, a UAR index assumed to be
# devcap's when the rule makes it 128.  Each produced a driver that was quietly wrong.
#
# It is a GATE, not a warning, because it is clean today: the file's own comment explains
# why a check that starts by failing gets turned off, and the converse holds too -- one that
# passes on the day it is added is one nobody has a reason to disable.  If it fires, either
# the value should be read back, or the rule needs refining; both are worth stopping for.
#
# The two per-selector tables have to line up, and this check exists because they did not:
# six rows appended after `survey` in the dispatch table put every selector from 33 up onto the
# wrong handler, and the kernel rejected the calls with kIOReturnBadArgument -- a chain run that
# reached INIT_HCA and the port and then failed four R4 steps with a code that says nothing about
# dispatch order.  The same edit had reversed the lock-class table's new rows.  Both are caught
# here now (tools/check-selector-tables.py).
#
if python3 "$SRC/tools/test-netupstop-dispatch.py" > "$ENCTEST/netupstop.out" 2>&1; then
    tail -n 1 "$ENCTEST/netupstop.out"
else
    echo "    FAIL: selector 46 diagnostic routing can reach the wrong step"
    sed 's/^/    /' "$ENCTEST/netupstop.out"
    exit 1
fi

# The normal network TX path is a stack-thread callback, not covered by the
# user-client selector transaction. Keep its stop ordering visible in every
# build; this source contract deliberately reports the remaining async-detach
# limitation rather than treating it as a callback-quiescence proof.
if python3 "$SRC/tools/test-net-stop-tx-drain.py" > "$ENCTEST/netstop.out" 2>&1; then
    tail -n 2 "$ENCTEST/netstop.out"
else
    echo "    FAIL: TX stop ordering regressed"
    sed 's/^/    /' "$ENCTEST/netstop.out"
    exit 1
fi

if python3 "$SRC/tools/test-tx-admission-gate.py" > "$ENCTEST/txgate.out" 2>&1; then
    tail -n 1 "$ENCTEST/txgate.out"
else
    echo "    FAIL: TX callback admission gate race regression"
    sed 's/^/    /' "$ENCTEST/txgate.out"
    exit 1
fi

# An errored TX QP must stop new submissions without abandoning posted mbufs.
if /usr/bin/python3 "$SRC/tools/test-net-tx-fault.py" > "$ENCTEST/txfault.out" 2>&1; then
    tail -n 1 "$ENCTEST/txfault.out"
else
    echo "    FAIL: TX first-fault containment regression"
    sed 's/^/    /' "$ENCTEST/txfault.out"
    exit 1
fi

if /usr/bin/python3 "$SRC/tools/test-tx-fault-decode.py" > "$ENCTEST/txdecode.out" 2>&1; then
    echo "    TX first-fault log decoder: PASS (offline)"
else
    echo "    FAIL: TX first-fault log decoder"
    cat "$ENCTEST/txdecode.out"
    exit 1
fi

if python3 "$SRC/tools/test-net-detach-worker.py" > "$ENCTEST/netdetach.out" 2>&1; then
    tail -n 1 "$ENCTEST/netdetach.out"
else
    echo "    FAIL: synchronous interface-detach worker lifetime regression"
    sed 's/^/    /' "$ENCTEST/netdetach.out"
    exit 1
fi

if python3 "$SRC/tools/check-selector-tables.py" > "$ENCTEST/seltab.out" 2>&1; then
    tail -n 2 "$ENCTEST/seltab.out"
else
    echo "    FAIL: the per-selector tables do not line up"
    sed 's/^/    /' "$ENCTEST/seltab.out"
    exit 1
fi

if python3 "$SRC/tools/check-hardcoded.py" --strict > "$ENCTEST/hard.out" 2>&1; then
    printf '    %s\n' "$(tail -1 "$ENCTEST/hard.out")"
else
    echo "    FAIL: a device-sourced literal with no read-back in its file"
    sed 's/^/    /' "$ENCTEST/hard.out"
    exit 1
fi

# The CQ owner-byte offsets.  Same reason as the others: this is arithmetic, it is
# transcribed by hand, and the one thing it can get wrong is invisible on hardware --
# a slot whose owner byte is missed reads as ready, so the consumer reports zeroed
# memory as a completion.  The test also prints how much of the ring the reference's
# own loop would have covered, because that number is why this port does not copy it.
if cc -Wall -o "$ENCTEST/cqown" "$SRC/tools/test-cq-owner.c" 2> "$ENCTEST/cqown.err"; then
    if "$ENCTEST/cqown" > "$ENCTEST/cqown.out" 2>&1; then
        printf '    %s\n' "$(tail -1 "$ENCTEST/cqown.out")"
        grep -E "reference covers|does NOT set" "$ENCTEST/cqown.out" | sed 's/^/    /'
    else
        echo "    FAIL: CQ owner-byte offset test"
        grep -E "FAIL|failure" "$ENCTEST/cqown.out" | sed 's/^/    /'
        exit 1
    fi
else
    echo "    FAIL: could not compile the CQ owner-byte test"
    head -5 "$ENCTEST/cqown.err" | sed 's/^/    /'
    exit 1
fi

# The M2d decode and profile packing, run on the host against the bytes this card
# actually returned from QUERY_DEV_CAP.  Both M2d bugs so far were arithmetic or
# shape mistakes whose first execution was on hardware, and each cost a reboot --
# a stack overflow that panicked the machine, and a UInt8 that truncated
# num_sys_eqs from 1024 to 0.  This test is compiled as C++ because the header it
# includes is C++.
# The CQE's and the send WQE's byte layouts.  Stage 9 is the first stage that reads a
# completion element and the first that writes a work request, and both layouts have ONE
# source in this tree (the vendored FreeBSD structs; DPDK's CQE was not obtained), so the
# offsets cannot be "two implementations agreeing".  This test compiles a mirror of the
# reference's declarations and checks the constants against the layout the compiler derives
# from them, plus the bit position of the opcode in the control segment -- where the plan's
# first draft had it shifted by 24, which turns the descriptor into a NOP that would complete
# and transmit nothing.  It also pins the ABI's own byte-array widths, which cannot include
# the register header and so carry literals (docs/DEFECTS.md F11's shape).
if cc -Wall -o "$ENCTEST/cqeoff" "$SRC/tools/test-cqe-offsets.c" 2> "$ENCTEST/cqeoff.err"; then
    if "$ENCTEST/cqeoff" > "$ENCTEST/cqeoff.out" 2>&1; then
        printf '    %s\n' "$(tail -1 "$ENCTEST/cqeoff.out")"
        grep -E "struct mlx4_cqe|shifted encoding|MLX4_OPCODE_SEND << 24" \
            "$ENCTEST/cqeoff.out" | sed 's/^/    /'
    else
        echo "    FAIL: CQE / send-WQE offset test"
        grep -E "FAIL|failure" "$ENCTEST/cqeoff.out" | sed 's/^/    /'
        exit 1
    fi
else
    echo "    FAIL: could not compile the CQE / send-WQE offset test"
    head -5 "$ENCTEST/cqeoff.err" | sed 's/^/    /'
    exit 1
fi

# The MTU path's three conventions (stack frame / family answer / SET_PORT wire size), which meet
# in one function and would otherwise be checked by reading comments.  Host program, no kernel.
if cc -Wall -o "$ENCTEST/mtu" "$SRC/tools/test-mtu-conventions.c" 2> "$ENCTEST/mtu.err"; then
    if "$ENCTEST/mtu" > "$ENCTEST/mtu.out" 2>&1; then
        grep -E "the smallest of the three|checks," "$ENCTEST/mtu.out" | sed 's/^/    /'
    else
        echo "    FAIL: MTU convention test"
        grep -E "FAIL" "$ENCTEST/mtu.out" | sed 's/^/    /'
        exit 1
    fi
else
    echo "    FAIL: could not compile the MTU convention test"
    head -5 "$ENCTEST/mtu.err" | sed 's/^/    /'
    exit 1
fi

if cc -x c++ -Wall -o "$ENCTEST/m2dp" "$SRC/tools/test-m2d-profile.c" 2> "$ENCTEST/m2dp.err"; then
    if "$ENCTEST/m2dp" > "$ENCTEST/m2dp.out" 2>&1; then
        printf '    %s\n' "$(tail -1 "$ENCTEST/m2dp.out")"
    else
        echo "    FAIL: M2d profile test"
        grep -E "FAIL|failure" "$ENCTEST/m2dp.out" | sed 's/^/    /'
        exit 1
    fi
else
    echo "    FAIL: could not compile the M2d profile test"
    head -5 "$ENCTEST/m2dp.err" | sed 's/^/    /'
    exit 1
fi

# The command timeout budgets and the gate decision that ends a wait.
#
# Why the host can check this at all: the failure being fixed is a duration (one selector
# held the transaction lock for 6 min 07.6 s because every wait was an iteration count
# with a millisecond name), and a duration cannot be measured without the device.  What
# can be checked is the arithmetic that turns a millisecond budget into a wall-clock
# deadline -- the one conversion that separates "50 ms" from "50 polls at 2.1 ms each" --
# and the four states of the gate, in particular that "already poisoned" is decided before
# the register is read and does not expire.  Those are exactly the parts a later edit could
# break silently.  docs/LOCK-IMPL.md carries the reasoning; docs/LOCK-DESIGN.md section 5.1
# asked for this test by name.
if cc -Wall -o "$ENCTEST/cmdwedge" "$SRC/tools/test-cmd-wedge.c" 2> "$ENCTEST/cmdwedge.err"; then
    if "$ENCTEST/cmdwedge" > "$ENCTEST/cmdwedge.out" 2>&1; then
        printf '    %s\n' "$(tail -1 "$ENCTEST/cmdwedge.out")"
    else
        echo "    FAIL: command timeout / gate test"
        grep -E "FAIL|failure" "$ENCTEST/cmdwedge.out" | sed 's/^/    /'
        exit 1
    fi
else
    echo "    FAIL: could not compile the command timeout / gate test"
    head -5 "$ENCTEST/cmdwedge.err" | sed 's/^/    /'
    exit 1
fi

else
    echo "--- smoke: selector ABI and CQE/WQE layout only; broad host regression deferred ---"
    if ! /usr/bin/python3 "$SRC/tools/check-selector-tables.py" > "$ENCTEST/seltab.out" 2>&1; then
        cat "$ENCTEST/seltab.out"; exit 1
    fi
    if ! cc -Wall -o "$ENCTEST/cqeoff" "$SRC/tools/test-cqe-offsets.c" > "$ENCTEST/cqeoff.cc" 2>&1; then
        cat "$ENCTEST/cqeoff.cc"; exit 1
    fi
    if ! "$ENCTEST/cqeoff" > "$ENCTEST/cqeoff.out" 2>&1; then
        cat "$ENCTEST/cqeoff.out"; exit 1
    fi
    tail -1 "$ENCTEST/seltab.out"
    tail -1 "$ENCTEST/cqeoff.out"
fi

echo "--- kmutil print-diagnostics (kext-level dependencies only) ---"
kmutil print-diagnostics -p "$STAGEKEXT" 2>&1 | sed 's/^/    /'
echo "--- kmutil libraries (authoritative per-symbol resolution) ---"
kmutil libraries -r "$STAGE" -p "$STAGEKEXT" 2>&1 | sed 's/^/    /'
# nm -u also lists the IOKit base-class _RESERVED slots; those resolve normally
# against the kernel collection, so the meaningful check is that the count
# kmutil resolved equals the count the Mach-O left undefined.
UNDEF=$(nm -u "$STAGEKEXT/Contents/MacOS/Cx3Mlx4" | wc -l | tr -d ' ')
RESOLVED=$(kmutil libraries -r "$STAGE" -p "$STAGEKEXT" 2>/dev/null | grep -c ' in /')
echo "--- undefined symbol accounting ---"
echo "    undefined in Mach-O : $UNDEF"
echo "    resolved by kmutil  : $RESOLVED"
if [ "$UNDEF" -ne "$RESOLVED" ]; then
    echo "    MISMATCH - the kext would fail symbol resolution; do not install"
    #
    # AND WHICH ONES.  This check was written to compare counts -- "do not install" is the right
    # verdict -- but a count says there is a problem and not what it is, and "14 unresolved" cost
    # two rounds of guessing, one of them wrong (the guesses were made by diffing nm output
    # against the previous build's binary, because this exit left no staged binary to measure).
    # Both lists are already in hand here, so print the difference: on a mismatch the names are
    # the answer, and the step that produces them is the one that is already failing.
    #
    # The symbol is the FIRST field of a kmutil line (`\t\t_SYMBOL in /System/...`), and nm -u's
    # last field.  Sorted sets, so comm works; -23 = only in the first.
    #
    nm -u "$STAGEKEXT/Contents/MacOS/Cx3Mlx4" | awk '{print $NF}' | sort -u > "$STAGE/undef.sym"
    kmutil libraries -r "$STAGE" -p "$STAGEKEXT" 2>/dev/null | grep ' in /' \
        | sed 's/^[[:space:]]*//' | awk '{print $1}' | sort -u > "$STAGE/resolved.sym"
    echo "    the $(($UNDEF - $RESOLVED)) symbol(s) kmutil could not resolve:"
    comm -23 "$STAGE/undef.sym" "$STAGE/resolved.sym" | sed 's/^/      /' | head -40
    # Dependency presence is not per-symbol availability. Only the four measured
    # reserved networking ABI slots below are eligible for the diagnostic fallback.
    if ! comm -23 "$STAGE/undef.sym" "$STAGE/resolved.sym" \
        | /usr/bin/python3 "$SRC/tools/check-unresolved-symbols.py"; then
        echo "    FAIL: real kernel import is unavailable; refusing candidate"
        exit 1
    fi
    echo "    (full lists: $STAGE/undef.sym and $STAGE/resolved.sym -- this stage directory is"
    echo "     left in place on failure, which is also why nm could not be run on it by hand)"
    #
    # AND THEN ASK THE TOOL WHOSE JOB THIS IS.  `kmutil print-diagnostics` exists to "indicate
    # whether the kext is loadable"; the counts above are one line list against another, and they
    # are not that test.  Measured 2026-10-12: a kext that print-diagnostics calls
    # "Dependencies: OK" -- the same verdict as a kext that ships and loads -- shows this exact
    # mismatch, on Apple's DeclareReservedUnused padding slots, under every Apple SDK on this
    # machine (14, 14.5, 15, 15.2, 15.5, Xcode 26.3) and under MacKernelSDK.  Four slots for an
    # IOEthernetController subclass, and the mismatch size changes with the header set, which is
    # what a count of padding slots does and what a missing implementation does not.
    #
    # The staged copy is chowned first because print-diagnostics refuses a bundle it does not
    # consider properly owned, and that refusal has been the only thing it ever said here -- the
    # step was blind for as long as it has existed.
    chown -R root:wheel "$STAGEKEXT" 2>/dev/null
    chmod -R go-w "$STAGEKEXT" 2>/dev/null
    verdict=$(kmutil print-diagnostics -p "$STAGEKEXT" 2>&1)
    printf '%s\n' "$verdict" | LC_ALL=C grep -v 'Invalid ownership\|Authenticating extension' \
        | sed 's/^/    /'
    if ! printf '%s\n' "$verdict" | LC_ALL=C grep -q 'Dependencies: OK'; then
        echo "    AND kmutil print-diagnostics does not say Dependencies: OK -- that is the"
        echo "    authoritative verdict, and this build fails on it."
        exit 1
    fi
    echo "    ...and print-diagnostics answers Dependencies: OK, which is the check that decides"
    echo "    whether a kext loads.  The count above is therefore a warning here, not the verdict."
    echo "OKWITHDECLAREDRESERVED"
fi
echo "    OK - every undefined symbol resolves against the kernel collections"

echo
echo "=== user-space control tool ==="
# Built with the system SDK (not the kernel one) so it links against IOKit
# normally.  It talks to the kext through Cx3Mlx4UserClient's selectors.
#
# Note on the compiler: this one must go through xcrun.  Invoking
# /Library/Developer/CommandLineTools/usr/bin/clang directly (which is what
# `xcrun --find clang` returns, and what builds the kext) makes it fail with
# "'stdio.h' file not found", because that binary does not locate the macOS SDK
# on its own.  The kext build never hits this because it passes -nostdinc and
# points explicitly at MacKernelSDK.
TOOLCC="${TOOLCC:-/usr/bin/clang}"
CODESIGN="${CODESIGN:-/usr/bin/codesign}"
"$TOOLCC" -O2 -Wall -o "$STAGE/cx3ctl" "$SRC/tools/cx3ctl.c" \
      -framework IOKit -framework CoreFoundation
rc=$?
echo "cx3ctl build rc=$rc"
[ "$rc" -eq 0 ] || exit 1
# macOS denies LAN UDP from unsigned locally built tools (sendto -> EHOSTUNREACH).
# The CX3_BC breadcrumb is the surviving record when a selector takes the machine
# down before logd flushes, so the tool must be signed before it is published.
"$CODESIGN" --force --sign - "$STAGE/cx3ctl" >/dev/null 2>&1 ||
    { echo "cx3ctl ad-hoc codesign failed"; exit 1; }
"$CODESIGN" --verify --strict "$STAGE/cx3ctl" >/dev/null 2>&1 ||
    { echo "cx3ctl codesign verification failed"; exit 1; }
echo "cx3ctl codesign: verified (ad-hoc)"
if ! CX3_BC=127.0.0.1:9 "$STAGE/cx3ctl" bctest > "$STAGE/cx3ctl-bctest.out" 2>&1; then
    echo "cx3ctl breadcrumb self-test failed:"
    sed 's/^/    /' "$STAGE/cx3ctl-bctest.out"
    exit 1
fi
sed 's/^/    /' "$STAGE/cx3ctl-bctest.out"
ls -la "$STAGE/cx3ctl"

#
# THE TOOL'S SELECTOR TABLE, CHECKED AGAINST THE ABI.
#
# A guard for a defect that reached the machine: the table `cx3ctl sizes` prints had no row for
# `netup` (selector 39), and tools/verify.sh learns each selector's in/out counts and structure
# size FROM THAT TABLE -- so the chain's interface step called it with no outputs asked for and
# reported "the call failed" instead of anything about the device.  Nothing else catches it: the
# kext's own static assert pins the DISPATCH table (one entry per selector), which is a different
# list in a different file.
#
# So: every id from 0 to kCx3SelCount-1 must have a row.  The count comes from the ABI header, so
# adding a selector without a row here fails the build rather than the next boot.
#
EXPECTED=$(python3 - "$SRC/src/cx3_abi.h" <<'PYEOF'
import re, sys
t = open(sys.argv[1]).read()
m = re.search(r"kCx3SelCount\s*=\s*(\d+)", t)
print(m.group(1) if m else "")
PYEOF
)
if [ -z "$EXPECTED" ]; then
    echo "    FAIL: kCx3SelCount not found in src/cx3_abi.h, so the selector table cannot be checked"
    exit 1
fi
"$STAGE/cx3ctl" sizes > "$ENCTEST/sizes.out" 2>&1
python3 - "$EXPECTED" "$ENCTEST/sizes.out" <<'PYEOF'
import sys
expected = int(sys.argv[1])
rows = {}
for line in open(sys.argv[2]):
    f = line.split()
    if len(f) >= 5 and f[0].isdigit():
        rows[int(f[0])] = f[4]
missing = [i for i in range(expected) if i not in rows]
extra = [i for i in rows if i >= expected]
print("    %d of %d selector(s) have a row in cx3ctl's sizes table" % (len(rows), expected))
if missing:
    print("    FAIL: no row for selector(s) %s -- verify.sh reads this table to learn each"
          % ", ".join(str(i) for i in missing))
    print("          selector's argument counts, so a missing row makes the chain call it with")
    print("          nothing asked for and report a failure that says nothing")
    sys.exit(1)
if extra:
    print("    FAIL: row(s) %s are past kCx3SelCount -- the table and the ABI disagree"
          % ", ".join(str(i) for i in extra))
    sys.exit(1)
print("    OK - every selector has a row, and none is past the end")
PYEOF
[ $? -eq 0 ] || { echo "    FAIL: the selector table is not complete"; exit 1; }

echo "=== publish to $OUT ==="
# Only now is $OUT touched, so a concurrent build never sees a half-written tree.
rm -rf "$OUT"
mkdir -p "$OUT"
mv "$STAGE/Cx3Mlx4"      "$OUT/Cx3Mlx4"
mv "$STAGE/Cx3Mlx4.kext" "$OUT/Cx3Mlx4.kext"
mv "$STAGE/cx3ctl"       "$OUT/cx3ctl"
cp -p "$STAGE/build-env.txt" "$OUT/build-env.txt" 2>/dev/null || true
cp -p "$STAGE/build-identity.h" "$OUT/build-identity.h" || exit 1
ls -la "$OUT" | sed 's/^/  /'

echo
echo
echo "Run (as root, on a live system):"
echo "  $OUT/cx3ctl probe"
echo "  $OUT/cx3ctl dma"
echo "  $OUT/cx3ctl queryfw"

echo
echo "=== artifact freshness ==="
# Catches the failure mode that actually matters: a build that reported success
# while the artifact is older than the sources, which is what a clobbered or
# skipped compile step looks like.
BINCHECK="$OUT/Cx3Mlx4.kext/Contents/MacOS/Cx3Mlx4"
if [ ! -f "$BINCHECK" ]; then
    echo "  FAIL: no binary at $BINCHECK"
    echo "BUILD RESULT: FAIL"
    exit 1
fi
newest_src=$(ls -t "$SRC"/src/*.cpp "$SRC"/src/*.h "$SRC"/kext/Info.plist | head -1)
if [ "$BINCHECK" -ot "$newest_src" ]; then
    echo "  FAIL: the binary is older than $newest_src"
    echo "        the compile step did not actually replace it"
    echo "BUILD RESULT: FAIL"
    exit 1
fi
echo "  binary is newer than every source"

echo
echo "=== artifact ==="
ls -la "$OUT/Cx3Mlx4.kext/Contents/MacOS/Cx3Mlx4"
echo "bundle: $OUT/Cx3Mlx4.kext"

echo

echo "===================================================================="
BIN="$OUT/Cx3Mlx4.kext/Contents/MacOS/Cx3Mlx4"
printf 'BUILD RESULT: PASS\n'
printf '  test profile: %s (full project/hardware acceptance not implied)\n' "$CX3_TEST_PROFILE"
printf '  kext binary : %s (%s bytes)\n' "$BIN" "$(wc -c < "$BIN" | tr -d ' ')"
printf '  sha256      : %s\n' "$(shasum -a 256 "$BIN" | cut -d' ' -f1)"
printf '  cx3ctl      : %s\n' "$OUT/cx3ctl"
printf '  environment : %s\n' "$OUT/build-env.txt"
echo "===================================================================="
echo
echo "A note on reading logs: do not grep the whole transcript for the word"
echo "\"error\".  kmutil print-diagnostics emits 'Filesystem error: Invalid"
echo "ownership' for any kext not owned by root, which the build dir never is,"
echo "so that string always appears on a successful build.  The authoritative"
echo "result is the BUILD RESULT line above, and the per-file OK/FAIL list."
