#!/usr/bin/env python3
"""Host test for what stop() may release in each worker-drain outcome."""
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
static void verify(bool selector, bool worker, bool complete, bool poison) {
    Mlx4StopDisposition d;
    d.init(selector, worker);
    CHECK("selector outcome preserved", d.selectorDrained == selector);
    CHECK("worker outcome preserved", d.workerQuiesced == worker);
    CHECK("whole device teardown only when both drained and quiescent", d.complete() == complete);
    CHECK("worker failure poisons attach", d.poisoned() == poison);
    CHECK("selector lock retained only while selector active", d.retainSelectorLock() == !selector);
    CHECK("attach completion applied only to drained worker", d.settleAttach() == worker);
}
int main() {
    verify(false, false, false, true);
    verify(false, true, false, false);
    verify(true, false, false, true);
    verify(true, true, true, false);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='stop-disposition-') as temp:
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
