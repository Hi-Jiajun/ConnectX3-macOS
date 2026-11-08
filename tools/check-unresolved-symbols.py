#!/usr/bin/env python3
"""Allow only the four measured IONetworkController reserved ABI slots.

Input: one unresolved Mach-O symbol per line, as produced by comm in build.sh.
Dependencies: OK is a bundle dependency check, not proof an arbitrary symbol binds.
"""
import sys

ALLOWED = {
    '__ZN19IONetworkController29_RESERVEDIONetworkController2Ev',
    '__ZN19IONetworkController29_RESERVEDIONetworkController3Ev',
    '__ZN19IONetworkController29_RESERVEDIONetworkController4Ev',
    '__ZN19IONetworkController29_RESERVEDIONetworkController5Ev',
}


def main():
    symbols = {line.strip() for line in sys.stdin if line.strip()}
    unknown = sorted(symbols - ALLOWED)
    for symbol in unknown:
        print('FAIL: unresolvable non-reserved symbol: ' + symbol)
    return 1 if unknown else 0


if __name__ == '__main__':
    raise SystemExit(main())
