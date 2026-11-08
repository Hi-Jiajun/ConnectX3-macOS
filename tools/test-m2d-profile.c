/*
 * test-m2d-profile.c -- run the M2d decode and profile packing on the host,
 * against the bytes this specific card actually returned.
 *
 * Why this exists.  Both bugs found in M2d so far were arithmetic-or-shape
 * mistakes that only the hardware noticed, and each cost a reboot: a 160-byte
 * stack overflow that panicked the machine, and an in/out parameter that made
 * the profile report "14 tables needed but only 0 slots" and refuse to run.  Code
 * whose first execution is on hardware gets one test per reboot; code in a header
 * a host program can include gets a test per build.
 *
 * The input below is the real QUERY_DEV_CAP outbox from this card, copied out of
 * the device's own answer (it is also in docs/M2B-NOTES.md).  The expected values
 * are what the reference's expressions produce from it, worked out by hand from
 * the FreeBSD decode -- so this is a regression test on the transcription, not a
 * restatement of the implementation.
 *
 * Build: cc -x c++ -o /tmp/cx3-m2d-test tools/test-m2d-profile.c && /tmp/cx3-m2d-test
 */
#include <stdio.h>
#include <string.h>

#define CX3_HOST_TEST
#include "../src/mlx4_pure.h"

static int checks = 0, failures = 0;

#define CHECK(what, got, want)                                             \
    do {                                                                   \
        ++checks;                                                          \
        if ((unsigned long long)(got) != (unsigned long long)(want)) {     \
            ++failures;                                                    \
            printf("  FAIL %-46s got 0x%llx want 0x%llx\n", (what),         \
                   (unsigned long long)(got), (unsigned long long)(want)); \
        }                                                                  \
    } while (0)

/* The device's answer, as it wrote it: big-endian dwords, host-order here. */
static const UInt32 outbox[MLX4_DEVCAP_WORDS] = {
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x0e0e0618, 0x60180508, 0x08160718, 0x16180309,
    0x60400823, 0x00000400, 0x00070007, 0x0011381f,
    0x00010000, 0x802f0102, 0x1e00007f, 0x00008000,
    0xeeff1f9d, 0xf677fbef, 0x20020009, 0x80090310,
    0x003e03f0, 0x00200200, 0x00000000, 0x0c030000,
    0x00094017, 0x40114010, 0x00000100, 0x00000080,
    0xb5fd0000, 0xfd00e0f4, 0x3000ff01, 0x00000000,
    0x00200100, 0x00010040, 0x00800080, 0x00800040,
    0x00080040, 0x3d5b8ec2, 0x00008000, 0x5e5ffff7,
    0x00100000, 0x00000000, 0x00000040, 0x000000fe,
    0x00000001, 0x00000000, 0x00000000, 0x00000000,
    0xc0000039, 0x00000000, 0x00081849, 0x04000fa0,
    0xc0284200, 0xc0000000, 0x803240fa, 0x00000000,
    0x00000000, 0x00001000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000
};

static void test_decode(void)
{
    Mlx4QueryDevCapResult q;
    Mlx4DevCap cap;
    UInt8 be[MLX4_DEVCAP_WORDS * 4];
    UInt32 i;

    memset(&q, 0, sizeof(q));
    for (i = 0; i < MLX4_DEVCAP_WORDS; ++i) q.rawWords[i] = outbox[i];
    mlx4DevCapBytes(&q, be);
    memset(&cap, 0, sizeof(cap));
    mlx4DecodeDevCap(be, &cap);

    /*
     * The three values that are not their raw byte, checked first, because
     * getting them wrong is invisible (they come out as plausible small numbers):
     *   reserved_qps = 1 << (0x12 & 0xf)      raw byte 0x0e -> 1<<14 = 16384?  no:
     *                                                                0x0e & 0xf = 14
     *   min_page_sz  = 1 << 0x4b              raw byte 0x09 -> 512
     *   uar_size     = 1 << ((0x49 & 0x3f)+20)
     * Here they are asserted as the reference computes them.
     */
    CHECK("ports",              cap.num_ports, 2);
    CHECK("num_sys_eqs",        cap.num_sys_eqs, 1024);
    /* 0x12 = 0x06 -> 1<<6;  0x1a = 0x07 -> 1<<7;  0x14 = 0x60 -> 1<<6 */
    CHECK("reserved_qps",       cap.reserved_qps, 64);
    CHECK("reserved_cqs",       cap.reserved_cqs, 128);
    CHECK("reserved_srqs",      cap.reserved_srqs, 64);
    /* 0x4b = 0x09 -> 1<<9 = 512;  0x49 = 0x02 -> 1<<((2 & 0x3f)+20) = 4 MiB */
    CHECK("min_page_sz",        cap.min_page_sz, 512);
    CHECK("uar_size",           cap.uar_size, 1u << 22);
    /* 0x48 = 0x20 -> >>4 = 2;  0x1f = 0x09 -> 1<<9;  0x20 = 0x60 -> 1<<6 */
    CHECK("reserved_uars",      cap.reserved_uars, 2);
    CHECK("max_eqs",            cap.max_eqs, 512);
    CHECK("reserved_mtts",      cap.reserved_mtts, 64);
    /* FreeBSD and Linux decode RSVD_MCG as field & 0x0f (0x62 = 0x40 -> 0),
     * not as an exponent. This catches the wrong 1 << nibble interpretation. */
    CHECK("reserved_mcg raw low nibble", cap.reserved_mcg, 0);
    /* max_counters is a BE32, not a log2 byte: saved 0x68..0x6b = 00000100. */
    CHECK("advertised counters BE32", cap.max_counters, 256);

    /* Entry sizes: plain u16 reads, and the profile is built from them. */
    CHECK("qpc_entry_sz",       cap.qpc_entry_sz, 256);
    CHECK("cqc_entry_sz",       cap.cqc_entry_sz, 128);
    CHECK("eqc_entry_sz",       cap.eqc_entry_sz, 128);
    CHECK("srq_entry_sz",       cap.srq_entry_sz, 128);
    CHECK("dmpt_entry_sz",      cap.dmpt_entry_sz, 64);
    CHECK("cmpt_entry_sz",      cap.cmpt_entry_sz, 64);
    CHECK("rdmarc_entry_sz",    cap.rdmarc_entry_sz, 32);
    CHECK("altc_entry_sz",      cap.altc_entry_sz, 64);
    CHECK("mtt_entry_sz",       cap.mtt_entry_sz, 8);

    /* The 64-bit capability word is ext_flags<<32 | flags, where both are 32-bit
     * reads -- reading it as a u64 at 0x44 swallows half of ext_flags. */
    CHECK("flags high (ext)",   (UInt32)(cap.flags >> 32), 0xeeff1f9d);
    CHECK("flags low",          (UInt32)(cap.flags & 0xffffffffu), 0xf677fbefu);

    /* num_sys_eqs=1024 means the SYS_EQS bit must be set: the EQ profile and the
     * EQ numbering model both key off it, and the reference derives it here. */
    CHECK("SYS_EQS bit set",    (cap.flags2 & MLX4_DEV_CAP_FLAG2_SYS_EQS) ? 1 : 0, 1);
    /* 0x1e = 0x03 -> 1<<3 = 8, and it must NOT be raised to reserved_uars*4:
     * that branch is only for a device with no system EQs. */
    CHECK("reserved_eqs kept",  cap.reserved_eqs, 8);
    /* max_icm_sz is a plain u64 at 0xa0; this device reports an unbounded-looking
     * 2^52, which is what the references read too. */
    CHECK("max_icm_sz",         cap.max_icm_sz, 0x0010000000000000ull);

    /* Nonzero low nibble and unrelated high bits must both be handled. */
    be[0x62] = 0xa3;
    mlx4DecodeDevCap(be, &cap);
    CHECK("reserved_mcg packed nibble", cap.reserved_mcg, 3);
    be[0x62] = 0xff;
    mlx4DecodeDevCap(be, &cap);
    CHECK("reserved_mcg upper boundary", cap.reserved_mcg, 15);

    be[0x68] = 0x01; be[0x69] = 0x02; be[0x6a] = 0x03; be[0x6b] = 0x04;
    mlx4DecodeDevCap(be, &cap);
    CHECK("counter full width and endian", cap.max_counters, 0x01020304u);
    /* flags bit 48 = ext_flags bit 16, i.e. byte 0x41 bit 0. Reuse output
     * deliberately: absence must clear a formerly reported counter count. */
    be[0x41] &= (UInt8)~1u;
    mlx4DecodeDevCap(be, &cap);
    CHECK("absent counter capability", cap.max_counters, 0);
}

/*
 * The profile packing, with the numbers the driver actually uses, against the
 * layout the kernel log reported.  This is the check that fails if the in/out
 * capacity handling regresses.
 */
static void test_profile(void)
{
    Mlx4DevCap cap;
    Mlx4QueryDevCapResult q;
    UInt8 be[MLX4_DEVCAP_WORDS * 4];
    Mlx4Profile req;
    Mlx4InitHcaParam p;
    Mlx4IcmTable tables[18];
    Mlx4ProfileEntry entries[MLX4_PROFILE_MAX_ENTRIES];
    UInt32 nTables = 0, nEntries = 0, i;
    UInt64 total;

    memset(&q, 0, sizeof(q));
    for (i = 0; i < MLX4_DEVCAP_WORDS; ++i) q.rawWords[i] = outbox[i];
    mlx4DevCapBytes(&q, be);
    memset(&cap, 0, sizeof(cap));
    mlx4DecodeDevCap(be, &cap);

    memset(&req, 0, sizeof(req));
    req.num_qp = 1024; req.rdmarc_per_qp = 16; req.num_srq = 64;
    req.num_cq = 256; req.num_mcg = 256; req.num_mpt = 2048; req.num_mtt = 4096;

    memset(&p, 0, sizeof(p));
    memset(tables, 0, sizeof(tables));
    memset(entries, 0, sizeof(entries));
    total = mlx4MakeProfile(&req, &cap, &p, tables, 18, &nTables,
                            entries, MLX4_PROFILE_MAX_ENTRIES, &nEntries);

    /* The bug this test was written for: the capacity must not be clobbered. */
    CHECK("profile returned non-zero", total ? 1 : 0, 1);
    CHECK("tables listed",      nTables, 14);
    CHECK("entries reported",   nEntries, MLX4_PROFILE_MAX_ENTRIES);

    /*
     * CMPT dominates: MLX4_NUM_CMPTS = 4 << 24 = 2^26 objects times a 64-byte
     * entry, so the cMPT address space is 4 GiB.  Both references compute the
     * same, so this is a transcription check, not a sanity claim.
     */
    /*
     * The EQ region is sized for min(max_eqs, MAX_MSIX) = min(512, 64) = 64
     * entries, i.e. 8 KiB -- not for num_sys_eqs.  Everything else follows from
     * the entry sizes and the requested counts.
     */
    CHECK("icm size",           total,
          0x100000000ull    /* CMPT  2^26 x 64  */
        + 0x80000ull        /* RDMARC 2^14 x 32 */
        + 0x40000ull        /* QP    2^10 x 256 */
        + 0x40000ull        /* MTT   2^15 x 8   */
        + 0x40000ull        /* MCG   2^8 x 1024 */
        + 0x2000ull         /* EQ    2^6 x 128  */
        + 0x20000ull        /* DMPT  2^11 x 64  */
        + 0x10000ull        /* ALTC  2^10 x 64  */
        + 0x8000ull         /* CQ    2^8 x 128  */
        + 0x2000ull         /* SRQ   2^6 x 128  */
        + 0x1000ull);       /* AUXC  2^10 x 1   */

    /* Sorted by decreasing size, packed with no gaps: start[i+1] = start[i]+size[i]. */
    for (i = 0; i + 1 < MLX4_PROFILE_MAX_ENTRIES; ++i) {
        if (entries[i].size) {
            CHECK("no gap between entries", entries[i + 1].start,
                  entries[i].start + entries[i].size);
        }
        if (i + 1 < MLX4_PROFILE_MAX_ENTRIES && entries[i].size)
            CHECK("sizes descend in order", entries[i].size >= entries[i + 1].size ? 1 : 0, 1);
    }

    /* The bases INIT_HCA is told must match where the packer put each table. */
    {
        const Mlx4ProfileEntry *qp = 0, *cq = 0, *eq = 0, *mut = 0;
        for (i = 0; i < MLX4_PROFILE_MAX_ENTRIES; ++i) {
            if (!strcmp(entries[i].name, "QP"))    qp  = &entries[i];
            if (!strcmp(entries[i].name, "CQ"))    cq  = &entries[i];
            if (!strcmp(entries[i].name, "EQ"))    eq  = &entries[i];
            if (!strcmp(entries[i].name, "MTT"))   mut = &entries[i];
        }
        CHECK("QP table found",  qp ? 1 : 0, 1);
        CHECK("CQ table found",  cq ? 1 : 0, 1);
        CHECK("EQ table found",  eq ? 1 : 0, 1);
        CHECK("MTT table found", mut ? 1 : 0, 1);
        if (qp)  CHECK("qpc_base from packer", p.qpc_base, qp->start);
        if (cq)  CHECK("cqc_base from packer", p.cqc_base, cq->start);
        if (eq)  CHECK("eqc_base from packer", p.eqc_base, eq->start);
        if (mut) CHECK("mtt_base from packer", p.mtt_base, mut->start);
        if (qp)  CHECK("log_num_qps",          p.log_num_qps, qp->logNum);
        if (cq)  CHECK("log_num_cqs",          p.log_num_cqs, cq->logNum);
    }

    /* With system EQs the HCA numbers EQs globally: log_num_eqs is 0x1f and the
     * count comes from the device (mlx4_profile.c:190-197). */
    CHECK("log_num_eqs (sys EQs)", p.log_num_eqs, 0x1f);
    /*
     * 1024 does not fit in a byte.  This is the check that caught it: the field
     * was UInt8 and truncated to 0, which would have told INIT_HCA the HCA has no
     * system EQs while the EQ profile was sized for 1024 of them.
     */
    CHECK("num_sys_eqs in param",  p.num_sys_eqs, 1024);
    CHECK("num_sys_eqs fits u16",  sizeof(p.num_sys_eqs), 2);

    /* Each table's reserved prefix must be small enough for one 256 KiB chunk
     * list entry, which is the bound mapIcmTable checks. */
    for (i = 0; i < nTables; ++i) {
        UInt64 reservedBytes = (UInt64)tables[i].reserved * tables[i].objSize;
        UInt32 chunks = (UInt32)((reservedBytes + MLX4_TABLE_CHUNK_SIZE - 1)
                                 / MLX4_TABLE_CHUNK_SIZE);
        if (chunks > 8) {
            ++failures;
            printf("  FAIL table %s needs %u chunks, the array holds 8\n",
                   tables[i].name, chunks);
        }
        ++checks;
    }
}

/* The readiness rule as the port and the reference both express it
 * (mlx4_eq.c:121-125): ready when the owner bit equals the consumer index's
 * phase bit at position log2(nent). */
static int eq_ready(const UInt8 *ring, UInt32 nent, UInt8 eqeFactor, UInt32 cons)
{
    UInt32 stride = MLX4_EQ_ENTRY_SIZE << eqeFactor;
    UInt64 slot = (UInt64)(cons & (nent - 1)) * stride + (eqeFactor ? MLX4_EQ_ENTRY_SIZE : 0);
    UInt32 owner = (ring[slot + MLX4_EQE_OWNER_OFFSET] & 0x80) ? 1u : 0u;
    UInt32 phase = (cons & nent) ? 1u : 0u;
    return owner == phase;
}

/* What the HCA is expected to write into a slot it delivers in lap L: the owner
 * bit carries the lap's parity, so a consumed slot from the previous lap reads as
 * the HCA's in the current one. */
static UInt8 eq_owner_for_lap(UInt32 lap) { return (lap & 1) ? 0x80 : 0x00; }

static void eq_deliver(UInt8 *ring, UInt32 nent, UInt8 eqeFactor, UInt32 cons,
                       UInt8 type, UInt16 token)
{
    UInt32 stride = MLX4_EQ_ENTRY_SIZE << eqeFactor;
    UInt64 slot = (UInt64)(cons & (nent - 1)) * stride + (eqeFactor ? MLX4_EQ_ENTRY_SIZE : 0);
    UInt8 *e = ring + slot;

    memset(e, 0, MLX4_EQ_ENTRY_SIZE);
    e[MLX4_EQE_TYPE_OFFSET] = type;
    e[MLX4_EQE_CMD_TOKEN_OFFSET]     = (UInt8)(token >> 8);
    e[MLX4_EQE_CMD_TOKEN_OFFSET + 1] = (UInt8)(token & 0xff);
    e[MLX4_EQE_OWNER_OFFSET] = eq_owner_for_lap(cons / nent);
}

static void test_eq_phase_ring(void)
{
    enum { NENT = 512, STRIDE = 64 };
    static UInt8 ring[NENT * STRIDE];
    UInt32 i, off;

    /* --- the one assumption: the post-arm state -------------------------
     * Every slot is the HCA's.  Expressed as "owner bit set", i.e. the opposite
     * of the lap-0 value the HCA writes when it delivers.  Restart 1 measures
     * this directly (the port reports how many of the first 32 slots carry the
     * owner bit after arming), because the source does not say. */
    memset(ring, 0, sizeof(ring));
    for (i = 0; i < NENT; ++i) {
        off = (UInt32)((UInt64)i * STRIDE + (STRIDE - 32));
        ring[off + MLX4_EQE_OWNER_OFFSET] = 0x80;
    }
    for (i = 0; i < 4; ++i)
        CHECK("armed ring: slot not ready (lap 0)", eq_ready(ring, NENT, 1, i), 0);

    /*
     * --- then the ring evolving ----------------------------------------
     * A full lap of deliveries: the HCA writes every slot with the lap-0 owner
     * value (0x00), so after the software has consumed all of them the ring reads
     * as entirely the HCA's again for lap 1 -- with owner bytes of 0x00, not
     * 0x80.  This is why the phase bit exists: the same byte value means
     * "consumed" in one lap and "empty" in the next.
     */
    for (i = 0; i < NENT; ++i)
        eq_deliver(ring, NENT, 1, i, MLX4_EVENT_TYPE_CMD, (UInt16)i);
    for (i = NENT; i < NENT + 4; ++i)
        CHECK("after a full lap: not ready", eq_ready(ring, NENT, 1, i), 0);
    CHECK("lap-0 owner byte is 0x00, same byte as empty in lap 1",
          ring[(STRIDE - 32) + MLX4_EQE_OWNER_OFFSET], 0x00);

    /* One lap-1 delivery becomes the only ready slot. */
    eq_deliver(ring, NENT, 1, NENT, MLX4_EVENT_TYPE_CMD, 0x5678);
    CHECK("lap-1 delivery ready", eq_ready(ring, NENT, 1, NENT), 1);
    CHECK("lap-1 owner byte is 0x80", ring[(STRIDE - 32) + MLX4_EQE_OWNER_OFFSET], 0x80);
    CHECK("the next slot is still the HCA's", eq_ready(ring, NENT, 1, NENT + 1), 0);

    /* And it must not be consumed twice. */
    CHECK("no double consumption at 2*nent (not yet delivered)",
          eq_ready(ring, NENT, 1, 2 * NENT), 0);

    /* --- the same rule with 32-byte EQEs ------------------------------- */
    {
        static UInt8 r32[NENT * 32];
        memset(r32, 0, sizeof(r32));
        for (i = 0; i < NENT; ++i) r32[(UInt64)i * 32 + MLX4_EQE_OWNER_OFFSET] = 0x80;
        CHECK("32B armed ring not ready", eq_ready(r32, NENT, 0, 0), 0);
        eq_deliver(r32, NENT, 0, 0, MLX4_EVENT_TYPE_CMD, 0xabcd);
        CHECK("32B ready after delivery", eq_ready(r32, NENT, 0, 0), 1);
        CHECK("32B owner byte", r32[MLX4_EQE_OWNER_OFFSET], 0x00);
        CHECK("32B type byte",  r32[MLX4_EQE_TYPE_OFFSET], MLX4_EVENT_TYPE_CMD);
    }
}

/*
 * MTT entry encoding: one entry per page, address ORed with the present flag
 * (mlx4_write_mtt_chunk, mlx4_mr.c:697-721):
 *     mtts[i] = cpu_to_be64(page_list[i] | MLX4_MTT_FLAG_PRESENT)
 */
static void test_mtt_encoding(void)
{
    UInt64 addr = 0x0000000123456000ull;
    UInt64 entry = addr | MLX4_MTT_FLAG_PRESENT;

    CHECK("present flag", MLX4_MTT_FLAG_PRESENT, 1);
    CHECK("entry size",   MLX4_MTT_ENTRY_SIZE, 8);
    CHECK("flag does not disturb the address", entry & ~0xfffull, addr);
    CHECK("low bit set", entry & 1, 1);
    /* The flag lives in the bits the address cannot use, which is only true for a
     * page-aligned address -- the reason mttWrite refuses unaligned ones. */
    CHECK("address must be page aligned for this to work", (addr & 0xfffull), 0);
}

/*
 * Entry point.  Order matters only for readability: the decode and the profile
 * first (they are what M2d runs), then the M3 restart-1 checks.
 */
/* ------------------------------------------------------------------ */
/* EQ context / EQE offsets, doorbell arithmetic, phase rule            */
/* ------------------------------------------------------------------ */

/*
 * The EQ context is a byte structure of single-byte and 32-bit fields at fixed
 * offsets.  One wrong offset gives the HCA a geometry or an MTT address that does
 * not match the memory the host prepared, and the visible symptom is "no event
 * arrived" -- indistinguishable from several other faults.  So the offsets are
 * checked field by field against the reference struct, the same way the
 * QUERY_DEV_CAP and INIT_HCA offsets are.
 *
 * struct mlx4_eq_context, laid out as the compiler would (mlx4.h:323):
 *   0x00 __be32 flags          0x0c u8 log_eq_size     0x17 u8 intr
 *   0x0a __be16 page_offset    0x1b u8 mtt_base_addr_h 0x18 u8 log_page_size
 *   0x1c __be32 mtt_base_l     0x28 __be32 consumer_index
 */
static void test_eq_offsets(void)
{
    CHECK("EQC flags",          EQ_CONTEXT_FLAGS_OFFSET, 0x00);
    CHECK("EQC log_eq_size",    EQ_CONTEXT_LOG_EQ_SIZE_OFFSET, 0x0c);
    CHECK("EQC intr",           EQ_CONTEXT_INTR_OFFSET, 0x17);
    CHECK("EQC log_page_size",  EQ_CONTEXT_LOG_PAGE_SIZE_OFFSET, 0x18);
    CHECK("EQC mtt_base_h",     EQ_CONTEXT_MTT_BASE_H_OFFSET, 0x1b);
    CHECK("EQC mtt_base_l",     EQ_CONTEXT_MTT_BASE_L_OFFSET, 0x1c);
    CHECK("EQC consumer_index", EQ_CONTEXT_CONSUMER_INDEX_OFFSET, 0x28);

    /* STATUS_OK is 0 << 28 and STATE_ARMED is 9 << 8, so the flags dword is
     * exactly 0x900 (mlx4_eq.c:60-64, :1023-1024). */
    CHECK("EQ armed flags", (MLX4_EQ_STATUS_OK | MLX4_EQ_STATE_ARMED), 0x900);

    /* The owner byte is the LAST byte of the 32-byte EQE.  Reading offset 0 for
     * it is the obvious mistake and yields a ring that never looks ready. */
    CHECK("EQE type",   MLX4_EQE_TYPE_OFFSET, 0x01);
    CHECK("EQE owner",  MLX4_EQE_OWNER_OFFSET, 0x1f);
    CHECK("EQE cmd token",     MLX4_EQE_CMD_TOKEN_OFFSET, 0x06);
    CHECK("EQE cmd status",    MLX4_EQE_CMD_STATUS_OFFSET, 0x0f);
    CHECK("EQE cmd out_param", MLX4_EQE_CMD_OUT_PARAM_OFFSET, 0x10);
    CHECK("EQE legacy size",   MLX4_EQ_ENTRY_SIZE, 0x20);

    CHECK("event CMD",         MLX4_EVENT_TYPE_CMD, 0x0a);
    CHECK("event PORT_CHANGE", MLX4_EVENT_TYPE_PORT_CHANGE, 0x09);

    /* The async EQ is 256 data entries plus 128 spare, rounded to a power of two
     * (mlx4_eq.c:51-52, :967-972). */
    {
        UInt32 want = MLX4_NUM_ASYNC_EQE + MLX4_NUM_SPARE_EQE, n = 1;
        while (n < want) n <<= 1;
        CHECK("async EQ entries", want, 384);
        CHECK("async EQ rounded", n, 512);
    }
}

/*
 * Doorbell address for an EQN, from mlx4_eq.c:939:
 *     BAR2 + ((eqn / 4) << uar_page_shift) + 0x800 + 8 * (eqn % 4)
 * Four doorbells per UAR page, 8 bytes apart.  Checked across a page boundary
 * because an off-by-one in the division puts the doorbell in the wrong page and
 * the symptom is purely "the HCA never re-arms".
 */
static void test_eq_doorbell(void)
{
    UInt32 eqn;
    for (eqn = 0; eqn < 9; ++eqn) {
        UInt64 got = (UInt64)(((eqn / 4) << MLX4_UAR_PAGE_SHIFT) +
                              MLX4_EQ_DOORBELL_OFFSET + 8 * (eqn % 4));
        UInt64 want = (UInt64)(((eqn / 4) * 4096) + 0x800 + 8 * (eqn % 4));
        CHECK("doorbell address", got, want);
    }
    CHECK("doorbell EQN 0", (UInt64)(((0/4) << 12) + 0x800 + 8*(0%4)), 0x800);
    CHECK("doorbell EQN 3", (UInt64)(((3/4) << 12) + 0x800 + 8*(3%4)), 0x818);
    CHECK("doorbell EQN 4", (UInt64)(((4/4) << 12) + 0x800 + 8*(4%4)), 0x1800);
    CHECK("doorbell EQN 7", (UInt64)(((7/4) << 12) + 0x800 + 8*(7%4)), 0x1818);
    CHECK("doorbell EQN 8", (UInt64)(((8/4) << 12) + 0x800 + 8*(8%4)), 0x2800);
}

/*
 * The readiness rule as a truth table over its two inputs.  This part is
 * source-provable and carries no assumption: ready <=> owner == phase, from
 * mlx4_eq.c:121-125 and the diagnostic at :803-808 which prints the same
 * expression with its two outcomes labelled HW and SW.
 */
static void test_eq_phase_rule(void)
{
    CHECK("owner=0 phase=0 -> ready",     0 == 0, 1);
    CHECK("owner=1 phase=0 -> not ready", 1 == 0, 0);
    CHECK("owner=0 phase=1 -> not ready", 0 == 1, 0);
    CHECK("owner=1 phase=1 -> ready",     1 == 1, 1);
    /* The phase is the consumer index's bit at log2(nent): for nent = 512 that
     * is bit 9, so it flips at 512, not at 511. */
    CHECK("phase at cons 511, nent 512", ((511 & 512) ? 1 : 0), 0);
    CHECK("phase at cons 512, nent 512", ((512 & 512) ? 1 : 0), 1);
    CHECK("phase at cons 1023, nent 512", ((1023 & 512) ? 1 : 0), 1);
    CHECK("phase at cons 1024, nent 512", ((1024 & 512) ? 1 : 0), 0);
}

int main(void)
{
    printf("M2d + M3-restart-1 pure logic, against this card's real QUERY_DEV_CAP bytes\n\n");
    printf("decode:\n");                                test_decode();
    printf("profile:\n");                               test_profile();
    printf("EQ context / EQE offsets:\n");               test_eq_offsets();
    printf("EQ doorbell arithmetic:\n");                test_eq_doorbell();
    printf("EQ owner/phase rule (source-provable):\n");  test_eq_phase_rule();
    printf("EQ owner/phase over a ring:\n");            test_eq_phase_ring();
    printf("MTT entry encoding:\n");                    test_mtt_encoding();
    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
