#!/usr/bin/env python3
"""Host test for the mailbox release decision after a command timeout."""
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
static void verify(bool wedged) {
    Mlx4MailboxDisposition d;
    d.init(wedged);
    CHECK("timeout state preserved", d.wedged == wedged);
    CHECK("poisoned mailbox retained", d.retain() == wedged);
    CHECK("ordinary mailbox released", d.release() == !wedged);
}
int main() {
    verify(false);
    verify(true);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='mailbox-disposition-') as temp:
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
