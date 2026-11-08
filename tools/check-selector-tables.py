#!/usr/bin/env python3
"""
check-selector-tables.py -- the two per-selector tables have to line up.

`Cx3Mlx4UserClient` has two lists that both have exactly one row per selector, and both are
POSITIONAL: `sMethods[selector]` is the dispatch, `kXactClass[selector]` is the lock policy.
A row inserted anywhere but its own index shifts every later selector onto the wrong handler,
and what that looks like from outside is the kernel rejecting the call with
kIOReturnBadArgument -- with the ROW COUNT still correct, so the count assert in the source
cannot see it.

Measured 2026-10-13: six rows (netstat..netport) were appended to the dispatch table after
`survey` (index 32) instead of at 40..45.  The chain ran to INIT_HCA and the port and then
failed four R4 steps with `kern_return 0xe00002c2`, which says nothing about dispatch order.

`kXactClass` writes its selector number in every row's comment, so it is the ground truth here;
the dispatch table's labels are compared to it row by row.  A shift in either list fails.

Usage: tools/check-selector-tables.py        # exit 0 when the two tables agree
"""

import pathlib
import re
import sys

SRC = pathlib.Path(__file__).resolve().parent.parent / "src" / "Cx3Mlx4UserClient.cpp"
ABI = SRC.with_name("cx3_abi.h")


def region(text: str, start: str, end: str) -> str:
    i = text.find(start)
    if i < 0:
        raise SystemExit("    FAIL: could not find %r in %s" % (start[:40], SRC.name))
    j = text.find(end, i)
    if j < 0:
        raise SystemExit("    FAIL: could not find the end of the region after %r" % start[:40])
    return text[i:j]


def main() -> int:
    src = SRC.read_text()

    disp = region(src,
                  "const IOExternalMethodDispatch Cx3Mlx4UserClient::sMethods[] = {",
                  "/*\n * Which selectors take the transaction lock")
    klass = region(src,
                   "static const UInt8 kXactClass[] = {",
                   "CX3_STATIC_ASSERT(sizeof(kXactClass)")

    # One label per dispatch row.  The rows carry `/* name */`; multi-line initialisers do not,
    # so the labels come out one per row.
    label = r"([A-Za-z0-9_]+(?:[ \t]+[A-Za-z0-9_]+)*)"
    drops = re.findall(r"/\*\s*" + label + r"\s*\*/\s*\{\s*&Cx3Mlx4UserClient::", disp)
    # One (number, label) per lock-class row.
    krows = re.findall(r"/\*\s*(\d+)\s+" + label + r"\s*\*/\s*kXact\w+\s*,", klass)

    print("    dispatch rows %d, lock-class rows %d" % (len(drops), len(krows)))

    counts = re.findall(r"\bkCx3SelCount\s*=\s*(\d+)\b", ABI.read_text())
    dispatch_initializers = len(re.findall(r"\{\s*&Cx3Mlx4UserClient::", disp))
    class_initializers = len(re.findall(r"\*/\s*kXact\w+\s*,", klass))
    if len(counts) != 1 or any(n != int(counts[0]) for n in
            (len(drops), len(krows), dispatch_initializers, class_initializers)):
        print("    FAIL: every initializer needs a label and both tables must match kCx3SelCount")
        return 1

    if len(drops) != len(krows):
        print("    FAIL: the two tables have different row counts, so one of them has a row the")
        print("          other does not -- and both are positional")
        return 1

    for i, (num, name) in enumerate(krows):
        if int(num) != i:
            print("    FAIL: the lock-class row at index %d is numbered %s: that table is out of"
                  % (i, num))
            print("          order, so every row from it down is on the wrong selector")
            return 1
        # The two tables abbreviate some names differently -- `clearown` against `clearowner`,
        # `r4ingr` against `r4ingress` -- so the comparison is "one is a prefix of the other"
        # rather than equality.  That is enough for what this checks: a SHIFT moves a genuinely
        # different name into the slot, and no two selectors' names are prefixes of each other.
        a, b = (re.sub(r"\s+", "", value.lower()) for value in (drops[i], name))
        if not (a.startswith(b) or b.startswith(a)):
            print("    FAIL: index %d is '%s' in the dispatch table and '%s' in the lock-class"
                  % (i, drops[i], name))
            print("          table.  The rows are positional, so a shift here calls the wrong")
            print("          handler -- and the kernel answers kIOReturnBadArgument, which says")
            print("          nothing about the reason")
            return 1

    print("    OK - both tables have one row per selector, in the same order")
    return 0


if __name__ == "__main__":
    sys.exit(main())
