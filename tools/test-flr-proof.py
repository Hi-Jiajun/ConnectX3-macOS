#!/usr/bin/env python3
"""Host test for the six independent conditions of an FLR release credential."""
import subprocess
import tempfile
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / 'src'
SOURCE = r'''
#include <cstdio>
#include <cstring>
#define CX3_HOST_TEST
#include "mlx4_pure.h"

static int checks, failures;
#define CHECK(label, expression) do { ++checks; if (!(expression)) { \
    ++failures; std::printf("FAIL %s\n", label); } } while (0)
static Mlx4FlrProof make(bool supported, bool pending, bool cpu, bool issued,
                         bool readable, bool scope) {
    Mlx4FlrProof p; p.init();
    p.supported=supported; p.pendingCleared=pending; p.cpuDrained=cpu;
    p.resetIssued=issued; p.readyReadable=readable; p.scopeAccounted=scope;
    return p;
}
int main() {
    CHECK("fresh is not proof", !make(false,false,false,false,false,false).credential());
    CHECK("only complete evidence is proof", make(true,true,true,true,true,true).credential());
    CHECK("unsupported", !make(false,true,true,true,true,true).credential());
    CHECK("transactions pending", !make(true,false,true,true,true,true).credential());
    CHECK("CPU callback still alive", !make(true,true,false,true,true,true).credential());
    CHECK("reset not issued", !make(true,true,true,false,true,true).credential());
    CHECK("device not readable", !make(true,true,true,true,false,true).credential());
    CHECK("peer function unaccounted", !make(true,true,true,true,true,false).credential());
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='flr-proof-') as temp:
        cpp = Path(temp) / 'test.cpp'
        binary = Path(temp) / 'test'
        cpp.write_text(SOURCE)
        build = subprocess.run(['/usr/bin/clang++', '-std=c++11', '-Wall', '-Wextra',
                                '-Werror', '-I', str(SRC), str(cpp), '-o', str(binary)],
                               capture_output=True, text=True, timeout=60)
        if build.returncode:
            print(build.stderr, end='')
            return build.returncode
        return subprocess.run([str(binary)], text=True, timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
