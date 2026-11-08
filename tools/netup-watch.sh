#!/bin/bash
#
# netup-watch.sh -- sample the CX3 client/netup/attach phase every 10 ms and push it OFF the
# machine.
#
# The previous shell loop launched `ioreg` for every sample. Under the load produced by the CX3
# bring-up chain it fell about three seconds behind and missed the fatal selector call. The native
# helper resolves the Cx3Mlx4 service once and reads six registry properties directly.
#
# Usage:  tools/netup-watch.sh [seconds]        (default 120; Ctrl-C also fine)
#         CX3_BC_HOST=host:port overrides the recorder (default the forensics VM at ...:9999)
#         CX3_WATCH_REQUIRE_UDP=0 allows a local-only run; the default is 1.
# The generated helper is ad-hoc signed because macOS currently rejects LAN UDP from an
# unsigned locally built binary with EHOSTUNREACH.

set -uo pipefail

SRC=/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4
SECS="${1:-120}"
OUT="$SRC/outputs/netup-watch-$(date +%Y%m%d-%H%M%S).log"
VC="${CX3_BC_HOST:-192.0.2.60:9999}"
BIN="${CX3_PHASE_WATCH_BIN:-$SRC/outputs/cx3-phase-watch}"
SOURCE="$SRC/tools/cx3-phase-watch.c"
CC="${CC:-/usr/bin/clang}"
CODESIGN="${CODESIGN:-/usr/bin/codesign}"
SDKROOT="${SDKROOT:-$(/usr/bin/xcrun --sdk macosx --show-sdk-path)}"
REQUIRE_UDP="${CX3_WATCH_REQUIRE_UDP:-1}"

case "$SECS" in
    ''|*[!0-9]*|0) echo "netup-watch: duration must be a positive integer" >&2; exit 2 ;;
esac
case "$VC" in
    *:*) ;;
    *) echo "netup-watch: CX3_BC_HOST must be host:port" >&2; exit 2 ;;
esac
host=${VC%:*}
port=${VC#*:}
case "$host" in
    ''|*[!A-Za-z0-9._:-]*) echo "netup-watch: invalid UDP host: $host" >&2; exit 2 ;;
esac
case "$port" in
    ''|*[!0-9]*) echo "netup-watch: invalid UDP port: $port" >&2; exit 2 ;;
esac
[ "$port" -ge 1 ] && [ "$port" -le 65535 ] ||
    { echo "netup-watch: UDP port out of range: $port" >&2; exit 2; }
[ -x "$CC" ] || { echo "netup-watch: compiler not executable: $CC" >&2; exit 2; }
[ -x "$CODESIGN" ] || { echo "netup-watch: codesign not executable: $CODESIGN" >&2; exit 2; }
[ -n "$SDKROOT" ] && [ -d "$SDKROOT" ] ||
    { echo "netup-watch: macOS SDK path is unavailable: ${SDKROOT:-empty}" >&2; exit 2; }
case "$REQUIRE_UDP" in
    0|1) ;;
    *) echo "netup-watch: CX3_WATCH_REQUIRE_UDP must be 0 or 1" >&2; exit 2 ;;
esac

mkdir -p "$SRC/outputs"
if [[ ! -x "$BIN" || "$SOURCE" -nt "$BIN" ]] ||
   ! "$CODESIGN" --verify --strict "$BIN" >/dev/null 2>&1; then
    TMP="$BIN.tmp.$$"
    "$CC" -O2 -Wall -Wextra -Werror -isysroot "$SDKROOT" \
        -framework IOKit -framework CoreFoundation \
        -o "$TMP" "$SOURCE" || { rm -f "$TMP"; exit 1; }
    # A locally built, unsigned helper is denied LAN UDP by the current
    # macOS policy (sendto -> EHOSTUNREACH); Apple-signed tools such as nc and
    # python3 are not. An ad-hoc signature is sufficient for this local probe.
    "$CODESIGN" --force --sign - "$TMP" >/dev/null 2>&1 ||
        { echo "netup-watch: ad-hoc codesign failed for $TMP" >&2; rm -f "$TMP"; exit 1; }
    "$CODESIGN" --verify --strict "$TMP" >/dev/null 2>&1 ||
        { echo "netup-watch: codesign verification failed for $TMP" >&2; rm -f "$TMP"; exit 1; }
    mv "$TMP" "$BIN"
fi

printf 'netup-watch: native sampler, 10ms interval, %ss -> %s (require_udp=%s)\n' \
    "$SECS" "$OUT" "$REQUIRE_UDP"
CX3_WATCH_REQUIRE_UDP="$REQUIRE_UDP" \
    exec "$BIN" "$SECS" "$host" "$port" "$OUT"
