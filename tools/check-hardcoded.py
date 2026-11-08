#!/usr/bin/env python3
"""
check-hardcoded.py -- find device-sourced values written into the source as literals, and
barriers that are not barriers.

The rule this enforces: **a value the device can be asked is read back, not written down.**

It is not a style preference.  The constants that were guesses have cost this project more
time than anything else: a field read from the wrong offset, an EQ profile count taken from
the phys-capability number instead of the reference's formula, a UAR index assumed to be
devcap's reserved count when the rule makes it 128.  Each produced a plausible-looking
driver that did the wrong thing quietly.

What it checks, and how it avoids being noise:

  - only literals in *value* positions, never in comments, and never inside the header that
    defines the value being checked against;
  - each rule names its own alibi: the expression that is the CORRECT way to obtain that
    value.  A literal is reported only where that alibi is absent from the file;
  - exit status is a warning budget, not a gate, until the list is empty.  A check that
    fails the build on the day it is added gets disabled.

The second rule is the one 0.43.9 added, and it is here rather than in a checker of its own
because this file already does the hard part -- stripping comments and string literals, so a
name mentioned in PROSE is not a finding.  The name is OSSynchronizeIO:

    MacOSX.sdk/.../Kernel.framework/Headers/libkern/OSAtomic.h:549-556

        #if defined(__arm__) || defined(__arm64__)
        extern void OSSynchronizeIO(void);
        #else
        static __inline__ void OSSynchronizeIO(void) { }
        #endif

This port is built x86_64 only, so every call to it compiled to nothing -- while three of this
project's own comments said it was "Linux wmb() / mmiowb()".  A no-op write barrier in front of
a doorbell is exactly the class of thing this project keeps finding by reading, and the fix is
`mlx4Wmb()` in src/Mlx4Hw.cpp, which is `__sync_synchronize()` (a real fence, MFENCE on this
architecture, defined unconditionally rather than only under an ARM test).  The rule below is
what keeps the next edit from reaching for the familiar name again: `src/` may not call it
outside a comment, and every call site has to say `mlx4Wmb()`.

Note what this rule does NOT claim: that the missing barrier caused any particular failure.  On
x86-64 the TSO model orders a WB descriptor store ahead of a later UC doorbell store anyway, so
the honest severity is "the code claimed a guarantee it did not have" -- see docs/DEFECTS.md
D14, which is where the reasoning and the falsification live.

Usage:
  tools/check-hardcoded.py            # report, exit 0 unless --strict
  tools/check-hardcoded.py --strict   # report, exit 1 if anything is found
"""

import re
import sys
import pathlib

SRC = pathlib.Path(__file__).resolve().parent.parent / "src"

# (literal, what it is, the correct way to obtain it, files where it is legitimate)
RULES = [
    (
        # Not a bare \b28\b: the first version of this rule reported "(sn << 28) | cmd" in
        # the CQ doorbell encoding -- a bit position, not an EQN.  That is family F (the
        # checker itself was wrong), and the fix is here rather than in the correct code:
        # "fix the code until the check is happy" is how a checker teaches the wrong lesson.
        # So a shift amount is excluded, and so is a hex/byte context.
        r"(?<![<])(?<!0x)\b28\b(?![xX0-9a-fA-F])",
        "28 -- EQN 28 is this card's asynchronous EQ, which is a device answer",
        "fFuncReservedEq",
    ),
    (
        r"\b128\b(?!\s*\))",
        "128 -- the first UAR index, which is max(128, devcap) at this page size",
        "reservedUarStart",
    ),
    (
        r"\b1518\b|\b1522\b",
        "the MTU to send, which the reference derives from the interface MTU",
        "MLX4_NET_IP_ALIGN",
    ),
    (
        r"\b512\b",
        "the EQ/CQ entry count, which this port chooses and should name once",
        "kEqUpOutNent",
    ),
]

# The header that defines the constants is where literals belong, and the tools directory
# is a different program (the control tool may legitimately mention numbers in usage text).
EXEMPT_FILES = {"mlx4_regs.h", "cx3_abi.h"}

# Names that must not appear in a VALUE position in src/: calling one is a defect, mentioning
# one in a comment is documentation (this file's comment stripper is what makes that hold).
FORBIDDEN_CALLS = [
    (
        r"\bOSSynchronizeIO\b\s*\(",
        "OSSynchronizeIO -- an EMPTY INLINE on x86_64 (OSAtomic.h:549-556), so it is not a "
        "barrier at all",
        "mlx4Wmb",
    ),
]


def strip_comments_and_strings(text: str) -> str:
    """Blanking comments and string literals, so a number in prose is not a finding."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            end = n if j < 0 else j + 2
            # Keep the NEWLINES.  Blanking a multi-line comment without them shifts every
            # line number after it, so the report would point at the wrong line -- a
            # diagnostic that misdirects, which is the class of thing this project keeps
            # deciding is worse than reporting nothing.
            out.append("\n" * text.count("\n", i, end))
            i = end
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
            out.append(" ")
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            out.append('""')
        else:
            out.append(c)
            i += 1
    return "".join(out)


def main() -> int:
    strict = "--strict" in sys.argv
    findings = []

    for path in sorted(SRC.glob("*.cpp")) + sorted(SRC.glob("*.h")):
        if path.name in EXEMPT_FILES:
            continue
        code = strip_comments_and_strings(path.read_text())
        # A shift amount is not a device value.  Done textually because the lookbehind for
        # "<<" with optional space is awkward to express and easy to get subtly wrong.
        code = re.sub(r"<<\s*\d+", "<< N", code)
        code = re.sub(r">>\s*\d+", ">> N", code)
        for pattern, what, alibi in RULES:
            for m in re.finditer(pattern, code):
                line = code.count("\n", 0, m.start()) + 1
                # The alibi is searched file-wide rather than per line: a file that reads
                # the value from the device somewhere is doing the right thing, and the
                # literal elsewhere is then a deliberate comparison against it.
                if alibi and alibi in code:
                    continue
                findings.append((path.name, line, what, alibi))

        for pattern, what, alibi in FORBIDDEN_CALLS:
            for m in re.finditer(pattern, code):
                line = code.count("\n", 0, m.start()) + 1
                findings.append((path.name, line, what, alibi))

    if not findings:
        print("  no device-sourced literal outside the headers that define them")
        print("  and no call to a name that is not a barrier on this architecture "
              f"({', '.join(w.split(' --')[0] for _, w, _ in FORBIDDEN_CALLS)})")
        return 0

    print(f"  {len(findings)} device-sourced literal(s) with no read-back in the same file:")
    for name, line, what, alibi in findings:
        print(f"    {name}:{line}: {what}")
        print(f"        should come from: {alibi}")
    print()
    print("  This is a warning, not a build gate, until the list is empty.  A check that")
    print("  starts by failing gets turned off, and then it protects nothing.")
    return 1 if strict else 0


if __name__ == "__main__":
    sys.exit(main())
