#!/usr/bin/env python3
"""Run the real QUERY_DEV_CAP CLI against a synthetic transport, never IOKit.

Catches treating the unresolved MRW/MTT raw byte as a proven byte count or
shifting by its unchecked value. Expectations are independent literal offsets;
the checked-in FreeBSD/Linux decoders only name these two offsets.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DevcapClientTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / 'tools/cx3ctl.c').read_text()
        start = source.index('static int cmdQueryDevCap(int force, int ignoreBusy)')
        body = source[start:source.index('\n}\n', start) + 3]
        code = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cx3_abi.h"
#include "mlx4_regs.h"
typedef int kern_return_t;
#define KERN_SUCCESS 0
static int gConn = 123;
static unsigned mrwByte = 0x40, mttByte = 0x23;
static unsigned transportCalls;
static int openService(void) { return 0; }
static const char *fwStatusName(uint64_t s) { return s ? "error" : "success"; }
static kern_return_t IOConnectCallMethod(int connection, unsigned selector,
        const uint64_t *in, unsigned inCount, const void *input, size_t inputSize,
        uint64_t *out, uint32_t *outCount, void *result, size_t *size) {
    if (connection != 123 || selector != 8 || inCount != kDevCapInCount ||
        in[0] != 0 || in[1] != 0 || input || inputSize || !out ||
        *outCount != kDevCapOutCount || *size != sizeof(struct Cx3DevCapResult))
        return -1;
    struct Cx3DevCapResult *res = result;
    unsigned char raw[256] = {0};
    /* Literal byte offsets, not the formatter's macros or decoder helper. */
    raw[0x11] = 0x0e; raw[0x13] = 0x18; /* QP WQEs and count */
    raw[0x1d] = 0x18; raw[0x22] = 8;    /* MPT count and reserved MRWs */
    raw[0x20] = 0x60; raw[0x37] = 2;   /* reserved MTTs and ports */
    raw[0x21] = mrwByte; raw[0x23] = mttByte;
    memset(res, 0, sizeof(*res));
    for (unsigned i = 0; i < 64; ++i)
        res->words[i] = ((uint32_t)raw[4*i] << 24) |
                        ((uint32_t)raw[4*i+1] << 16) |
                        ((uint32_t)raw[4*i+2] << 8) | raw[4*i+3];
    ++transportCalls;
    return KERN_SUCCESS;
}
BODY
int main(int argc, char **argv) {
    if (argc == 3) {
        mrwByte = (unsigned)strtoul(argv[1], NULL, 0);
        mttByte = (unsigned)strtoul(argv[2], NULL, 0);
    }
    int rc = cmdQueryDevCap(0, 0);
    return rc == 0 && transportCalls == 1 ? 0 : 1;
}
'''.replace('BODY', body)
        cls.temp = tempfile.TemporaryDirectory(prefix='cx3-devcap-client-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = str(Path(cls.temp.name) / 'test')
        subprocess.run(['/usr/bin/clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        '-I', str(ROOT / 'src'), '-x', 'c', '-', '-o', cls.binary],
                       input=code, text=True, check=True)

    def run_client(self, mrw=64, mtt=35):
        result = subprocess.run([self.binary, str(mrw), str(mtt)],
                                text=True, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, '')
        return result.stdout

    def test_saved_raw_bytes_are_not_claimed_as_limits(self):
        output = self.run_client()
        self.assertRegex(output, r'MTT segment.*raw 0x23.*unresolved')
        self.assertRegex(output, r'MRW size.*raw 0x40.*unresolved')
        self.assertNotIn('34359738368 B', output)
        self.assertRegex(output, r'QPs\s*: max\s+16777216\s+reserved\s+1\s+max WQEs/QP 16384')
        self.assertRegex(output, r'MPTs\s*: max\s+16777216\s+reserved MTTs\s+64')
        self.assertRegex(output, r'MRWs\s*: reserved 256')

    def test_all_unknown_bytes_preserved_without_undefined_shifts(self):
        for value in range(256):
            with self.subTest(raw=value):
                output = self.run_client(value, value)
                self.assertRegex(output, rf'MTT segment.*raw 0x{value:02x}.*unresolved')
                self.assertRegex(output, rf'MRW size.*raw 0x{value:02x}.*unresolved')


if __name__ == '__main__':
    unittest.main(verbosity=2)
