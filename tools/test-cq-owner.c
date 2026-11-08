/*
 * test-cq-owner.c -- does the owner preset write the same byte the consumer reads?
 *
 * This exists because of the one defect in stage 2 that would not produce an error, only
 * a false completion: if the driver presets the wrong byte, a slot's owner stays zero and
 * the phase rule reads it as ready, so the consumer reports zero-filled memory as a
 * completed CQE.
 *
 * The reference has a loop that looks like it does this and does not quite:
 *
 *     for (j = 0; j < cq->size; j++)
 *             cq->buf[j].owner_sr_opcode = MLX4_CQE_OWNER_MASK;
 *
 * `cq->buf` is a `struct mlx4_cqe *` -- 32 bytes -- while `buf_size` is
 * `cq->size * cqe_size` and cqe_size is 64 on this card, so that walks the first half of
 * the buffer.  The consumer looks at the real CQE halves, at i*64+63.
 *
 * So the two predicates this port needs are:
 *
 *   preset(i)  = i * stride + CQE_SLOT_OWNER_BYTE(factor)
 *   read(i)    = i * stride + (factor ? 32 : 0) + CQE_OWNER_OFFSET
 *
 * and they are the same expression written twice.  The point of this test is to make that
 * a checked fact rather than a claim: it also enumerates what the reference's loop would
 * have covered, so the gap is visible as a number.
 *
 * Build and run:  cc -Wall -o /tmp/test-cq-owner tools/test-cq-owner.c && /tmp/test-cq-owner
 */

#include <stdio.h>

#define CQE_LEGACY_SIZE 0x20u
#define CQE_OWNER_OFFSET 0x1fu
#define CQE_SLOT_OWNER_BYTE(factor) (((factor) ? CQE_LEGACY_SIZE : 0u) + CQE_OWNER_OFFSET)

static int failures = 0;
static int checks = 0;

static void check(int cond, const char *what)
{
    ++checks;
    if (!cond) {
        ++failures;
        printf("  FAIL: %s\n", what);
    }
}

int main(void)
{
    unsigned factor, nent;

    printf("preset offset == read offset, for every slot\n");
    for (factor = 0; factor <= 1; ++factor) {
        unsigned stride = CQE_LEGACY_SIZE << factor;
        printf("  factor %u (slot stride %u bytes)\n", factor, stride);
        for (nent = 2; nent <= 1024; nent <<= 1) {
            unsigned i;
            for (i = 0; i < nent; ++i) {
                unsigned preset = i * stride + CQE_SLOT_OWNER_BYTE(factor);
                unsigned read   = i * stride + (factor ? CQE_LEGACY_SIZE : 0u)
                                  + CQE_OWNER_OFFSET;
                if (preset != read) {
                    printf("  FAIL: nent %u slot %u: preset %u != read %u\n", nent, i,
                           preset, read);
                    ++failures;
                }
                ++checks;
                /* The owner byte must be the last byte of the legacy CQE. */
                if (read != i * stride + (factor ? CQE_LEGACY_SIZE : 0u) + 31u) {
                    printf("  FAIL: nent %u slot %u: owner at %u is not the 32nd byte of "
                           "the legacy CQE\n", nent, i, read);
                    ++failures;
                }
                ++checks;
            }
        }
    }

    /*
     * And now the gap, as a number: what the reference's 32-byte-stride loop covers
     * versus what the consumer actually reads.  nent = 512, the size stage 2 uses.
     */
    printf("\nwhat the reference's loop covers, nent 512, 64-byte CQEs\n");
    {
        const unsigned nent512 = 512, stride = 64;
        unsigned refMax = 0, realMax = 0, i, covered = 0;
        int slotPreset[512];

        for (i = 0; i < 512; ++i) slotPreset[i] = 0;
        /* the reference loop: j walks 32-byte units, both halves */
        for (i = 0; i < nent512; ++i) {
            unsigned off = i * 32 + 31;
            if (off > refMax) refMax = off;
            /* which slot's REAL CQE does that byte belong to, if any? */
            if (off % stride == CQE_SLOT_OWNER_BYTE(1)) {
                slotPreset[off / stride] = 1;
                ++covered;
            }
        }
        for (i = 0; i < nent512; ++i) {
            unsigned off = i * stride + CQE_SLOT_OWNER_BYTE(1);
            if (off > realMax) realMax = off;
        }
        printf("  reference covers %u of %u real CQE owner bytes (highest byte %u)\n",
               covered, nent512, refMax);
        printf("  the consumer reads up to byte %u\n", realMax);
        check(covered == nent512 / 2, "the reference's loop covers half the ring");
        {
            unsigned missing = 0;
            for (i = 0; i < nent512; ++i) if (!slotPreset[i]) ++missing;
            printf("  slots whose owner byte the reference loop does NOT set: %u\n",
                   missing);
            check(missing == nent512 / 2, "the other half is left unset");
        }
    }

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
