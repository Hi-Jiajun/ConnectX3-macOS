#!/usr/bin/env python3
"""Exercise production QUERY_ADAPTER board-ID decoding with a fake transport.

The FreeBSD get_board_id reference selects Topspin vs Mellanox VSD layout.
Literal wire bytes below catch copying the VSD header as an ID, missing a
signature check, a wrong absolute offset, missing word swaps, or missing NUL.
No IOKit/device calls are linked or made.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/Mlx4Hw.cpp').read_text()
start = source.index('bool Mlx4Hw::queryAdapter(')
method = source[start:source.index('\n}\n', start) + 3]
code = r'''
#include <cstdio>
#include <cstring>
#define CX3_HOST_TEST
#include "mlx4_pure.h"
#include "cx3_abi.h"
static void sink(const char*, ...) {}
#define LOG(...) sink(__VA_ARGS__)
class Mlx4Hw {
public:
    UInt8 mailbox[MLX4_MAILBOX_SIZE], reply[MLX4_MAILBOX_SIZE];
    void *fMboxVirt = mailbox;
    UInt64 fMboxPhys = 0x12345000;
    bool allocOk = true, commandOk = true;
    unsigned commandCalls = 0;
    Mlx4Hw() {
        memset(mailbox, 0xcc, sizeof(mailbox));
        memset(reply, 0, sizeof(reply));
        reply[0x10] = 63;
        memset(reply + 0x20, 0x5b, 64);  // decoy VSD header, not board ID
        memcpy(reply + 0xf0, "1_TM54329876DCBA", 16);
    }
    bool mboxAlloc() { return allocOk; }
    bool cmdBox(UInt64 in, UInt64 out, UInt32 modifier, UInt8 opmod,
                UInt16 op, UInt32 timeout, UInt32 *status) {
        ++commandCalls;
        if (in || out != fMboxPhys || modifier || opmod || op != 0x6 ||
            timeout != 60000) return false;
        *status = commandOk ? 0 : 7;
        if (!commandOk) return false;
        memcpy(mailbox, reply, sizeof(reply));
        return true;
    }
    bool queryAdapter(UInt8*, UInt8*, UInt32*, UInt8* = nullptr);
};
METHOD
static unsigned checks = 0, failures = 0;
#define CHECK(label, expr) do { ++checks; if (!(expr)) { \
    ++failures; std::printf("FAIL %s\n", label); } } while (0)
static bool allZero(const UInt8 *p, unsigned n) {
    for (unsigned i = 0; i < n; ++i) if (p[i]) return false;
    return true;
}
static void testMellanox(unsigned signatureMask) {
    Mlx4Hw hw;
    if (signatureMask & 1) { hw.reply[0x20] = 5; hw.reply[0x21] = 0xad; }
    if (signatureMask & 2) { hw.reply[0xfe] = 5; hw.reply[0xff] = 0xad; }
    UInt8 pin = 0, storage[66]; UInt32 status = 999;
    memset(storage, 0xcc, sizeof(storage));
    CHECK("Mellanox QUERY_ADAPTER succeeds", hw.queryAdapter(&pin, storage+1, &status));
    CHECK("Mellanox inta/status/one command", pin == 63 && status == 0 && hw.commandCalls == 1);
    // Normal and only-first-signature fixtures have the same literal wire ID.
    if (signatureMask != 2)
        CHECK("Mellanox full ID with dword swaps", memcmp(storage+1, "MT_123456789ABCD", 16) == 0);
    else {
        const UInt8 expected[] = {'M','T','_','1','2','3','4','5','6','7','8','9',0xad,5,'C','D'};
        CHECK("second signature alone remains Mellanox", memcmp(storage+1, expected, 16) == 0);
    }
    CHECK("Mellanox zero padding", allZero(storage+17, 48));
    CHECK("Mellanox output canaries", storage[0] == 0xcc && storage[65] == 0xcc);
    UInt8 raw[258]; memset(raw, 0xcc, sizeof(raw));
    CHECK("four-argument raw query succeeds", hw.queryAdapter(nullptr, nullptr, nullptr, raw+1));
    CHECK("raw output is the exact first 256 mailbox bytes", memcmp(raw+1, hw.reply, 256) == 0);
    CHECK("raw output canaries", raw[0] == 0xcc && raw[257] == 0xcc);
}
int main() {
    testMellanox(0); testMellanox(1); testMellanox(2);
    {
        Mlx4Hw hw;
        hw.reply[0x20] = hw.reply[0xfe] = 5;
        hw.reply[0x21] = hw.reply[0xff] = 0xad;
        memset(hw.reply+0x40, 0, 64);
        memcpy(hw.reply+0x40, "TS-VSD-BOARD-X", 14);
        UInt8 output[66]; memset(output, 0xcc, sizeof(output));
        CHECK("Topspin QUERY_ADAPTER succeeds", hw.queryAdapter(nullptr, output+1, nullptr));
        CHECK("Topspin ID uses nested +0x20 offset without swapping",
              memcmp(output+1, "TS-VSD-BOARD-X", 14) == 0);
        CHECK("Topspin zero padding", allZero(output+15, 50));
        CHECK("Topspin output canaries", output[0] == 0xcc && output[65] == 0xcc);
        memset(hw.reply+0x40, 'T', 64); // no NUL within the source field
        CHECK("Topspin maximum length succeeds", hw.queryAdapter(nullptr, output+1, nullptr));
        unsigned ts = 0; for (unsigned i = 1; i <= 63; ++i) if (output[i] == 'T') ++ts;
        CHECK("Topspin truncation terminates within 64", ts == 63 && output[64] == 0);
        CHECK("Topspin bounded output", output[0] == 0xcc && output[65] == 0xcc);
    }
    {
        Mlx4Hw hw; UInt8 pin = 0; UInt32 status = 0;
        CHECK("optional board output omitted", hw.queryAdapter(&pin, nullptr, &status));
        CHECK("optional output retains inta pin", pin == 63 && status == 0);
        hw.allocOk = false;
        CHECK("mailbox allocation failure", !hw.queryAdapter(&pin, nullptr, &status));
        CHECK("allocation failure does not issue command", hw.commandCalls == 1 && pin == 0xff && status == CX3_FWSTATUS_NOT_POSTED);
        UInt8 raw[256]; memset(raw, 0xcc, sizeof(raw));
        CHECK("raw output zeroed before allocation failure", !hw.queryAdapter(nullptr, nullptr, nullptr, raw) && allZero(raw, sizeof(raw)));
        hw.allocOk = true; hw.commandOk = false;
        CHECK("firmware failure", !hw.queryAdapter(&pin, nullptr, &status));
        CHECK("firmware status preserved", status == 7 && pin == 0xff);
        memset(raw, 0xcc, sizeof(raw));
        CHECK("raw output zeroed before firmware failure", !hw.queryAdapter(nullptr, nullptr, nullptr, raw) && allZero(raw, sizeof(raw)));
    }
    std::printf("QUERY_ADAPTER: %u checks, %u failures (production method, synthetic VSD)\n", checks, failures);
    return failures ? 1 : 0;
}
'''.replace('METHOD', method)
with tempfile.TemporaryDirectory(prefix='cx3-adapter-query-') as temp:
    binary = str(Path(temp) / 'test')
    subprocess.run(['/usr/bin/clang++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    '-I', str(ROOT / 'src'), '-x', 'c++', '-', '-o', binary],
                   input=code, text=True, check=True)
    raise SystemExit(subprocess.run([binary], timeout=10).returncode)
