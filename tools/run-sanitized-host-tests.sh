#!/bin/bash
# Run host tests with the CLT Clang runtime.
#
# On this Darwin 25 host, Xcode's Clang 17 ASan runtime crashes
# during dyld introspection. CLT Clang 21 uses a compatible runtime. This
# wrapper changes DEVELOPER_DIR only for its child processes; it does not
# modify xcode-select, shell profiles, or project configuration.

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
export DEVELOPER_DIR=/Library/Developer/CommandLineTools

if [[ ! -d "$DEVELOPER_DIR" ]]; then
    echo "FAIL: missing command-line tools at $DEVELOPER_DIR" >&2
    exit 2
fi

compiler=$(/usr/bin/xcrun --find clang++)
version=$(/usr/bin/clang++ -dumpversion)
major=${version%%.*}
if [[ ! "$major" =~ ^[0-9]+$ ]] || (( major < 21 )); then
    echo "FAIL: expected Clang >= 21 for the Darwin 25 ASan runtime; got $version" >&2
    echo "resolved compiler: $compiler" >&2
    exit 2
fi

probe_dir=$(/usr/bin/mktemp -d "${TMPDIR:-/tmp}/cx3-sanitizer.XXXXXX")
trap '/bin/rm -f "$probe_dir/probe.cpp" "$probe_dir/probe"; /bin/rmdir "$probe_dir" 2>/dev/null || true' EXIT
probe_src="$probe_dir/probe.cpp"
probe_bin="$probe_dir/probe"
printf '%s\n' '#include <cstdio>' 'int main() { std::puts("sanitizer preflight"); return 0; }' > "$probe_src"

if ! /usr/bin/clang++ -std=c++17 -fsanitize=address,undefined \
    -fno-sanitize-recover=all "$probe_src" -o "$probe_bin"; then
    echo "FAIL: sanitizer preflight compile failed with $compiler" >&2
    exit 2
fi
if ! "$probe_bin" >/dev/null; then
    echo "FAIL: sanitizer preflight did not reach main with $compiler" >&2
    exit 2
fi

echo "sanitizer toolchain: $compiler (Clang $version)"
if (( $# == 0 )); then
    set -- \
        "$ROOT/tools/cx3-mlx4/tools/test-start-admission.py" \
        "$ROOT/tools/cx3-mlx4/tools/test-net-tx-fault.py"
fi

for test in "$@"; do
    echo "RUN: $test"
    /usr/bin/python3 "$test"
done
