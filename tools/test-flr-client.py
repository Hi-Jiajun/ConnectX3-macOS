#!/usr/bin/env python3
"""Execute cmdFlr: failed reset must fail CLI; unread owner is not 'still set'."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'tools/cx3ctl.c').read_text()
start = source.index('static int cmdFlr(void)')
body = source[start:source.index('\n}\n', start) + 3]
code = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CX3_HOST_TEST
#include "cx3_abi.h"
static int mode;
static int call(unsigned,const uint64_t*,unsigned,uint64_t *out,uint32_t*) {
    out[kFlrOutStatus]=mode==1 ? 42 : mode==2 ? 43 : 0;
    out[kFlrOutSupported]=mode!=2;
    out[kFlrOutBarStable]=mode!=2;
    out[kFlrOutOwner]=0xffffffffull;
    return 0;
}
BODY
int main(int argc,char**) { mode=argc-1;return cmdFlr()==0 ? 0 : 1; }
'''.replace('BODY', body)
with tempfile.TemporaryDirectory(prefix='cx3-flr-client-') as temp:
    binary = str(Path(temp) / 'test')
    subprocess.run(['/usr/bin/clang++', '-std=c++11', '-I', str(ROOT / 'src'),
                    '-x', 'c++', '-', '-o', binary], input=code, text=True, check=True)
    checks = 0
    for mode in range(3):
        result = subprocess.run([binary] + ['x'] * mode, text=True, capture_output=True)
        assert result.returncode == (0 if mode == 0 else 1), (mode, result.stdout)
        checks += 1
        if mode == 0:
            assert 'not read' in result.stdout and 'still set' not in result.stdout, result.stdout
            assert 'blocked' in result.stdout, result.stdout
            checks += 2
    print(f'FLR client: {checks} behavior checks PASS (real cmdFlr, synthetic transport)')
