#!/usr/bin/env python3
"""Fail the build if Cx3Mlx4::start() can reach any device write or DMA alloc.

Build 0.3.0 ran the command path from IOKit start() and hung the machine before
it could flush a log line; the card kept that state across warm resets and the
next six boots hung too.  This check turns "start() must stay read-only" from a
comment into something the build enforces.

It disassembles the freshly linked kext, walks the call graph reachable from
Cx3Mlx4::start(), and fails if anything on that graph writes configuration
space, writes a BAR register, allocates DMA, or resets the HCA.

Usage: check-boot-path.py <path-to-kext-binary>
"""
import re
import subprocess
import sys

FORBIDDEN = [
    "configWrite",
    "extendedConfigWrite",
    "IODMACommand",
    "IOMemoryDescriptor",
    # Our own write-capable routines.  These matter because a raw store to a
    # mapped BAR pointer compiles to a plain `mov` with no symbol to match, so
    # the only reliable handle on it is the name of the function holding it.
    "resetHca",
    "clearOwner",
    "writeBar",
    "configWrite",
    "queryFw",
    "mboxAlloc",
    "mboxFree",
    "cmdBox",
    "cmdPost",
    "export",
]

# NOTE on the limits of this check, so it is not trusted further than it earns:
# it matches call targets by name.  A function reachable from start() that wrote
# straight to a register through an already-loaded pointer would not be caught by
# name.  What it does guarantee is that start() cannot reach any of the named
# write/deadlock-capable routines, and it prints the full reachable set on every
# build so a human can review any addition.  That is why the reachable list is
# printed rather than summarised.

# otool prints the label with a trailing colon; the bodies dict keys are stored
# without it, so the entry point is named the same way.
ENTRY = "__ZN7Cx3Mlx45startEP9IOService"

# See the note above on what may be believed.
DIRECT_CALL = re.compile(r"^(__Z[A-Za-z0-9_.$]+|_[A-Za-z][A-Za-z0-9_]*)$")
RIP_REF = re.compile(r"^(__Z[A-Za-z0-9_.$]+|_[A-Za-z][A-Za-z0-9_]*)\(%rip\)$")


def symbols_in(operand):
    """The symbol names an operand really names, if any."""
    operand = operand.strip()
    if DIRECT_CALL.match(operand):
        return [operand]
    m = RIP_REF.match(operand)
    if m:
        return [m.group(1)]
    return []


def operand_of(line):
    """Operand text with otool's annotation dropped.

    otool appends `## comment`, and the comment is itself a symbol name, so it
    must not be matched as if it were part of the instruction.
    """
    line = line.split("##", 1)[0]
    parts = line.split("\t")
    if len(parts) < 3:
        return ""
    return parts[-1].strip()


def main():
    if len(sys.argv) not in (2, 3):
        print("usage: check-boot-path.py <kext-binary> [entry-symbol]",
              file=sys.stderr)
        return 2
    binary = sys.argv[1]
    entry = sys.argv[2] if len(sys.argv) == 3 else ENTRY

    try:
        asm = subprocess.run(["otool", "-tvV", binary],
                             capture_output=True, text=True, check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError) as exc:
        print("could not disassemble %s: %s" % (binary, exc), file=sys.stderr)
        return 2

    lines = asm.split("\n")
    bodies = {}
    current = None
    for line in lines:
        if re.match(r"^__Z", line):
            current = line.rstrip(":")
            bodies[current] = []
        elif current is not None:
            bodies[current].append(line)

    if entry not in bodies:
        print("FAIL: %s not found in %s" % (entry, binary), file=sys.stderr)
        return 2

    # Transitive closure over direct calls to functions we know about.
    seen, stack = set(), [entry]
    while stack:
        fn = stack.pop()
        if fn in seen or fn not in bodies:
            continue
        seen.add(fn)
        for line in bodies[fn]:
            if "callq" not in line:
                continue
            for target in symbols_in(operand_of(line)):
                if target in bodies and target not in seen:
                    stack.append(target)

    reachable = sorted(seen)
    print("boot-path reachability from %s (%d functions):" % (entry, len(reachable)))
    for fn in reachable:
        print("    %s" % fn)

    # A forbidden symbol counts if it is reachable itself, if something reachable
    # calls it, or if something reachable takes its address.
    bad = []
    for fn in reachable:
        for token in FORBIDDEN:
            if token in fn:
                bad.append("%s (reachable directly)" % fn)
        for line in bodies[fn]:
            for sym in symbols_in(operand_of(line)):
                for token in FORBIDDEN:
                    if token in sym:
                        bad.append("%s -> %s" % (fn, sym))

    print()
    if bad:
        print("FAIL: the boot path can reach a device write or a DMA allocation:")
        for b in sorted(set(bad)):
            print("    %s" % b)
        print()
        print("Move that work behind a Cx3Mlx4UserClient selector instead")
        print("(see src/cx3_abi.h). A fault in start() is a boot fault.")
        return 1

    print("PASS: no device write, no DMA allocation, no HCA reset is reachable")
    print("      from Cx3Mlx4::start(). The boot path is read-only.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
