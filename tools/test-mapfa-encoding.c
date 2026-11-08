/*
 * Host-side check of the MAP_FA mailbox encoding.
 *
 * The mailbox layout is easy to get subtly wrong (entry stride, which slot holds
 * the address, byte order) and getting it wrong means telling the firmware to use
 * pages it should not. Rather than discover that after a reboot, this replicates
 * the encoding from Mlx4Hw::mapFa and checks it against what mlx4_map_cmd
 * (FreeBSD mlx4_core/mlx4_fw.c:1501) is documented to produce.
 *
 * It does not run the driver and does not touch hardware; it only proves the bytes
 * are laid out the way the firmware expects.
 *
 * Build and run:
 *   cc -o /tmp/t tools/test-mapfa-encoding.c && /tmp/t
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#define MLX4_MAILBOX_SIZE        4096u
#define MLX4_ICM_PAGE_SHIFT      12
#define MLX4_MAP_CMD_MAX_ENTRIES (MLX4_MAILBOX_SIZE / 16)

static void put_be64(void *p, uint64_t v)
{
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)(v >> 56); b[1] = (uint8_t)(v >> 48);
    b[2] = (uint8_t)(v >> 40); b[3] = (uint8_t)(v >> 32);
    b[4] = (uint8_t)(v >> 24); b[5] = (uint8_t)(v >> 16);
    b[6] = (uint8_t)(v >> 8);  b[7] = (uint8_t)(v);
}

static uint64_t get_be64(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; ++i) v = (v << 8) | b[i];
    return v;
}

/*
 * Models the real mailbox lifecycle: fill entries, send a batch when the mailbox
 * is full, clear it, carry on. Each batch is copied out as it is "sent", so the
 * contents of every batch can be inspected -- an earlier version cleared in place
 * and could therefore only see the last one, which made three correct assertions
 * look like failures.
 */
#define MAX_BATCHES 8

static uint32_t encode_batches(uint64_t *busAddrs, uint32_t npages,
                               uint8_t batches[MAX_BATCHES][MLX4_MAILBOX_SIZE],
                               uint32_t *outBatches,
                               int sendVirt, uint64_t virt)
{
    uint8_t  mbox[MLX4_MAILBOX_SIZE];
    uint32_t nent = 0, total = 0, nb = 0, i;

    memset(mbox, 0, sizeof(mbox));
    for (i = 0; i < npages; ++i) {
        if (sendVirt) {
            put_be64(mbox + nent * 16 + 0, virt);
            virt += (1u << MLX4_ICM_PAGE_SHIFT);
        }
        put_be64(mbox + nent * 16 + 8,
                 busAddrs[i] | (uint64_t)(MLX4_ICM_PAGE_SHIFT - MLX4_ICM_PAGE_SHIFT));
        ++nent;
        ++total;

        if (nent == MLX4_MAP_CMD_MAX_ENTRIES) {
            if (nb < MAX_BATCHES) memcpy(batches[nb], mbox, MLX4_MAILBOX_SIZE);
            nb++;
            nent = 0;
            memset(mbox, 0, sizeof(mbox));
        }
    }
    if (nent) {
        if (nb < MAX_BATCHES) memcpy(batches[nb], mbox, MLX4_MAILBOX_SIZE);
        nb++;
    }
    *outBatches = nb;
    return total;
}

static int failures = 0;

static void check(int cond, const char *what)
{
    printf("  %-58s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) failures++;
}

int main(void)
{
    static uint64_t pages[512];
    static uint8_t  batch[MAX_BATCHES][MLX4_MAILBOX_SIZE];
    uint32_t nent, nb = 0, i;

    printf("MAP_FA / MAP_ICM mailbox encoding\n\n");
    printf("batch size (MLX4_MAILBOX_SIZE / 16) = %u entries -> %u bytes\n",
           MLX4_MAP_CMD_MAX_ENTRIES, MLX4_MAP_CMD_MAX_ENTRIES * 16);

    printf("\nlayout:\n");
    check(MLX4_MAP_CMD_MAX_ENTRIES == 256, "batch size is 256 entries");
    check(256 * 16 == MLX4_MAILBOX_SIZE, "256 entries exactly fill the 4 KiB mailbox");

    /* ---- a small MAP_FA, the shape the real call uses ---- */
    pages[0] = 0x0000000030114000ULL;
    pages[1] = 0x0000000030115000ULL;
    pages[2] = 0x0000000100000000ULL;      /* above 4 GiB, to catch truncation */

    nent = encode_batches(pages, 3, batch, &nb, 0, 0);
    printf("\nMAP_FA, 3 pages -> %u entr(ies) in %u batch(es)\n", nent, nb);
    for (i = 0; i < nent; ++i) {
        printf("    entry %u: virt slot = 0x%016llx   bus slot = 0x%016llx\n",
               i, (unsigned long long)get_be64(batch[0] + i * 16 + 0),
               (unsigned long long)get_be64(batch[0] + i * 16 + 8));
    }

    printf("\nchecks:\n");
    check(nb == 1, "3 pages go out as one batch");
    check(get_be64(batch[0] + 0 * 16 + 8) == pages[0], "entry 0 bus slot = page 0 address");
    check(get_be64(batch[0] + 1 * 16 + 8) == pages[1], "entry 1 bus slot = page 1 address");
    check(get_be64(batch[0] + 2 * 16 + 8) == pages[2], "entry 2 bus slot = page 2 address");
    check(get_be64(batch[0] + 2 * 16 + 8) == 0x100000000ULL,
          "an address above 4 GiB survives intact");
    check((get_be64(batch[0] + 0 * 16 + 8) & 0xfffull) == 0,
          "page 0 address is 4 KiB aligned, so the alignment field ORs in nothing");
    check(get_be64(batch[0] + 1 * 16 + 0) == 0 &&
          get_be64(batch[0] + 2 * 16 + 0) == 0,
          "virt slots are zero for MAP_FA (no virtual side)");
    check(batch[0][1 * 16 + 8] == 0x00 && batch[0][1 * 16 + 11] == 0x00 &&
          batch[0][1 * 16 + 12] == 0x30 && batch[0][1 * 16 + 13] == 0x11 &&
          batch[0][1 * 16 + 14] == 0x50 && batch[0][1 * 16 + 15] == 0x00,
          "big-endian on the wire (0x30115000 -> 00 00 00 00 30 11 50 00)");

    /* ---- batching ---- */
    for (i = 0; i < 512; ++i) pages[i] = 0x100000000000ULL + (uint64_t)i * 0x1000ULL;

    nent = encode_batches(pages, 256, batch, &nb, 0, 0);
    check(nent == 256 && nb == 1, "256 pages: one full batch");
    check(get_be64(batch[0] + 255 * 16 + 8) == pages[255],
          "last entry of a full batch holds the right address");

    nent = encode_batches(pages, 300, batch, &nb, 0, 0);
    check(nent == 300 && nb == 2, "300 pages: two batches");
    check(get_be64(batch[1] + 0 * 16 + 8) == pages[256],
          "first entry of batch 2 is page 256");
    /* 300 - 256 = 44 entries in batch 2, so its indices are 0..43. */
    check(get_be64(batch[1] + 43 * 16 + 8) == pages[299],
          "last entry of batch 2 (index 43) is page 299");
    check(get_be64(batch[1] + 44 * 16 + 8) == 0,
          "batch 2 is zeroed at index 44, past its 44 entries (no leakage)");

    /* ---- MAP_ICM: virtual slots, and the counter must not reset per batch ---- */
    nent = encode_batches(pages, 300, batch, &nb, 1, 0x2000ULL);
    check(nent == 300 && nb == 2, "MAP_ICM: 300 pages, two batches");
    check(get_be64(batch[0] + 0 * 16 + 0) == 0x2000ULL,
          "MAP_ICM: entry 0 virt = the starting virtual address");
    check(get_be64(batch[0] + 1 * 16 + 0) == 0x3000ULL,
          "MAP_ICM: entry 1 virt advances by exactly one page");
    check(get_be64(batch[0] + 255 * 16 + 0) == 0x2000ULL + 255 * 0x1000ULL,
          "MAP_ICM: last entry of batch 1 has the right virt");
    check(get_be64(batch[1] + 0 * 16 + 0) == 0x2000ULL + 256 * 0x1000ULL,
          "MAP_ICM: batch 2 continues the count -- virt is NOT reset per batch");
    check(get_be64(batch[1] + 43 * 16 + 0) == 0x2000ULL + 299 * 0x1000ULL,
          "MAP_ICM: final entry virt = start + 299 pages");

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all checks passed",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
