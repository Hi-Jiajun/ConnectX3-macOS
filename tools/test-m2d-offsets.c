/*
 * test-m2d-offsets.c -- check the M2d offset constants against the reference.
 *
 * Why this exists: every number M2d sends to the HCA is an offset into a
 * big-endian structure, and a typo in one of them is invisible until a command
 * fails -- which costs a kext rebuild and a reboot to see.  The constants were
 * transcribed by hand from the FreeBSD source, so they are exactly the kind of
 * thing that should be checked mechanically rather than by re-reading.
 *
 * What is checked: each constant in src/mlx4_regs.h against the literal value
 * from the reference file it came from.  Deliberately literal on the right-hand
 * side, so that changing the header to match a new reference version is a
 * visible edit here too, not a silent drift.
 *
 * Relationship and consistency checks are included as well (the INIT_HCA
 * sub-offsets are defined relative to their region, the cMPT sub-tables are
 * spaced by entry size << CMPT_SHIFT, and the flags bytes are exactly the 12
 * defined ones), because those are the ones a copied constant protects without
 * being a copy.
 *
 * Build and run as part of build.sh; it is a host program, no kernel involved.
 */
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

/* The header under test.  Pure #defines, no kernel dependencies. */
#include "../src/mlx4_regs.h"

static int checks = 0, failures = 0;

#define CHECK(what, got, want)                                            \
    do {                                                                  \
        ++checks;                                                         \
        if ((uint64_t)(got) != (uint64_t)(want)) {                        \
            ++failures;                                                   \
            printf("  FAIL %-52s got 0x%llx want 0x%llx\n", (what),        \
                   (unsigned long long)(got), (unsigned long long)(want));\
        }                                                                 \
    } while (0)

/* FreeBSD cmd.h:43-71 */
static void test_opcodes(void)
{
    CHECK("MLX4_CMD_QUERY_DEV_CAP", MLX4_CMD_QUERY_DEV_CAP, 0x3);
    CHECK("MLX4_CMD_QUERY_FW",      MLX4_CMD_QUERY_FW,      0x4);
    CHECK("MLX4_CMD_INIT_HCA",      MLX4_CMD_INIT_HCA,      0x7);
    CHECK("MLX4_CMD_MOD_STAT_CFG",  MLX4_CMD_MOD_STAT_CFG,  0x34);
    CHECK("MLX4_CMD_UNMAP_ICM",     MLX4_CMD_UNMAP_ICM,      0xff9);
    CHECK("MLX4_CMD_MAP_ICM",       MLX4_CMD_MAP_ICM,       0xffa);
    CHECK("MLX4_CMD_UNMAP_ICM_AUX", MLX4_CMD_UNMAP_ICM_AUX, 0xffb);
    CHECK("MLX4_CMD_MAP_ICM_AUX",   MLX4_CMD_MAP_ICM_AUX,   0xffc);
    CHECK("MLX4_CMD_SET_ICM_SIZE",  MLX4_CMD_SET_ICM_SIZE,  0xffd);
    CHECK("MLX4_CMD_UNMAP_FA",      MLX4_CMD_UNMAP_FA,      0xffe);
    CHECK("MLX4_CMD_MAP_FA",        MLX4_CMD_MAP_FA,        0xfff);
    CHECK("MLX4_CMD_RUN_FW",        MLX4_CMD_RUN_FW,        0xff6);
}

/* FreeBSD mlx4_fw.c:736-815 -- QUERY_DEV_CAP outbox layout */
static void test_devcap_offsets(void)
{
    CHECK("DEV_CAP_MAX_SRQ_SZ",   QUERY_DEV_CAP_MAX_SRQ_SZ_OFFSET,   0x10);
    CHECK("DEV_CAP_MAX_QP_SZ",    QUERY_DEV_CAP_MAX_QP_SZ_OFFSET,    0x11);
    CHECK("DEV_CAP_RSVD_QP",      QUERY_DEV_CAP_RSVD_QP_OFFSET,      0x12);
    CHECK("DEV_CAP_MAX_QP",       QUERY_DEV_CAP_MAX_QP_OFFSET,       0x13);
    CHECK("DEV_CAP_RSVD_SRQ",     QUERY_DEV_CAP_RSVD_SRQ_OFFSET,     0x14);
    CHECK("DEV_CAP_MAX_SRQ",      QUERY_DEV_CAP_MAX_SRQ_OFFSET,      0x15);
    CHECK("DEV_CAP_RSVD_EEC",     QUERY_DEV_CAP_RSVD_EEC_OFFSET,     0x16);
    CHECK("DEV_CAP_MAX_EEC",      QUERY_DEV_CAP_MAX_EEC_OFFSET,      0x17);
    CHECK("DEV_CAP_MAX_CQ_SZ",    QUERY_DEV_CAP_MAX_CQ_SZ_OFFSET,    0x19);
    CHECK("DEV_CAP_RSVD_CQ",      QUERY_DEV_CAP_RSVD_CQ_OFFSET,      0x1a);
    CHECK("DEV_CAP_MAX_CQ",       QUERY_DEV_CAP_MAX_CQ_OFFSET,       0x1b);
    CHECK("DEV_CAP_MAX_MPT",      QUERY_DEV_CAP_MAX_MPT_OFFSET,      0x1d);
    CHECK("DEV_CAP_RSVD_EQ",      QUERY_DEV_CAP_RSVD_EQ_OFFSET,      0x1e);
    CHECK("DEV_CAP_MAX_EQ",       QUERY_DEV_CAP_MAX_EQ_OFFSET,       0x1f);
    CHECK("DEV_CAP_RSVD_MTT",     QUERY_DEV_CAP_RSVD_MTT_OFFSET,     0x20);
    CHECK("DEV_CAP_MAX_MRW_SZ",   QUERY_DEV_CAP_MAX_MRW_SZ_OFFSET,   0x21);
    CHECK("DEV_CAP_RSVD_MRW",     QUERY_DEV_CAP_RSVD_MRW_OFFSET,     0x22);
    CHECK("DEV_CAP_MAX_MTT_SEG",  QUERY_DEV_CAP_MAX_MTT_SEG_OFFSET,  0x23);
    CHECK("DEV_CAP_NUM_SYS_EQ",   QUERY_DEV_CAP_NUM_SYS_EQ_OFFSET,   0x26);
    CHECK("DEV_CAP_MAX_AV",       QUERY_DEV_CAP_MAX_AV_OFFSET,       0x27);
    CHECK("DEV_CAP_MAX_REQ_QP",   QUERY_DEV_CAP_MAX_REQ_QP_OFFSET,   0x29);
    CHECK("DEV_CAP_MAX_RES_QP",   QUERY_DEV_CAP_MAX_RES_QP_OFFSET,   0x2b);
    CHECK("DEV_CAP_MAX_GSO",      QUERY_DEV_CAP_MAX_GSO_OFFSET,      0x2d);
    CHECK("DEV_CAP_RSS",          QUERY_DEV_CAP_RSS_OFFSET,          0x2e);
    CHECK("DEV_CAP_MAX_RDMA",     QUERY_DEV_CAP_MAX_RDMA_OFFSET,     0x2f);
    CHECK("DEV_CAP_RSZ_SRQ",      QUERY_DEV_CAP_RSZ_SRQ_OFFSET,      0x33);
    CHECK("DEV_CAP_PORT_BEACON",  QUERY_DEV_CAP_PORT_BEACON_OFFSET,  0x34);
    CHECK("DEV_CAP_ACK_DELAY",    QUERY_DEV_CAP_ACK_DELAY_OFFSET,    0x35);
    CHECK("DEV_CAP_MTU_WIDTH",    QUERY_DEV_CAP_MTU_WIDTH_OFFSET,    0x36);
    CHECK("DEV_CAP_VL_PORT",      QUERY_DEV_CAP_VL_PORT_OFFSET,      0x37);
    CHECK("DEV_CAP_MAX_MSG_SZ",   QUERY_DEV_CAP_MAX_MSG_SZ_OFFSET,   0x38);
    CHECK("DEV_CAP_MAX_GID",      QUERY_DEV_CAP_MAX_GID_OFFSET,      0x3b);
    CHECK("DEV_CAP_RATE_SUPPORT", QUERY_DEV_CAP_RATE_SUPPORT_OFFSET, 0x3c);
    CHECK("DEV_CAP_CQ_TS_SUPPORT",QUERY_DEV_CAP_CQ_TS_SUPPORT_OFFSET,0x3e);
    CHECK("DEV_CAP_MAX_PKEY",     QUERY_DEV_CAP_MAX_PKEY_OFFSET,     0x3f);
    CHECK("DEV_CAP_EXT_FLAGS",    QUERY_DEV_CAP_EXT_FLAGS_OFFSET,    0x40);
    CHECK("DEV_CAP_FLAGS",        QUERY_DEV_CAP_FLAGS_OFFSET,        0x44);
    CHECK("DEV_CAP_RSVD_UAR",     QUERY_DEV_CAP_RSVD_UAR_OFFSET,     0x48);
    CHECK("DEV_CAP_UAR_SZ",       QUERY_DEV_CAP_UAR_SZ_OFFSET,       0x49);
    CHECK("DEV_CAP_PAGE_SZ",      QUERY_DEV_CAP_PAGE_SZ_OFFSET,      0x4b);
    CHECK("DEV_CAP_BF",           QUERY_DEV_CAP_BF_OFFSET,           0x4c);
    CHECK("DEV_CAP_LOG_BF_REG_SZ",QUERY_DEV_CAP_LOG_BF_REG_SZ_OFFSET,0x4d);
    CHECK("DEV_CAP_LOG_MAX_BF_REGS",
          QUERY_DEV_CAP_LOG_MAX_BF_REGS_PER_PAGE_OFFSET,             0x4e);
    CHECK("DEV_CAP_LOG_MAX_BF_PAGES",
          QUERY_DEV_CAP_LOG_MAX_BF_PAGES_OFFSET,                     0x4f);
    CHECK("DEV_CAP_MAX_SG_SQ",    QUERY_DEV_CAP_MAX_SG_SQ_OFFSET,    0x51);
    CHECK("DEV_CAP_MAX_DESC_SZ_SQ",QUERY_DEV_CAP_MAX_DESC_SZ_SQ_OFFSET,0x52);
    CHECK("DEV_CAP_MAX_SG_RQ",    QUERY_DEV_CAP_MAX_SG_RQ_OFFSET,    0x55);
    CHECK("DEV_CAP_MAX_DESC_SZ_RQ",QUERY_DEV_CAP_MAX_DESC_SZ_RQ_OFFSET,0x56);
    CHECK("DEV_CAP_SVLAN_BY_QP",  QUERY_DEV_CAP_SVLAN_BY_QP_OFFSET,  0x5d);
    CHECK("DEV_CAP_MAX_QP_MCG",   QUERY_DEV_CAP_MAX_QP_MCG_OFFSET,   0x61);
    CHECK("DEV_CAP_RSVD_MCG",     QUERY_DEV_CAP_RSVD_MCG_OFFSET,     0x62);
    CHECK("DEV_CAP_MAX_MCG",      QUERY_DEV_CAP_MAX_MCG_OFFSET,      0x63);
    CHECK("DEV_CAP_RSVD_PD",      QUERY_DEV_CAP_RSVD_PD_OFFSET,      0x64);
    CHECK("DEV_CAP_MAX_PD",       QUERY_DEV_CAP_MAX_PD_OFFSET,       0x65);
    CHECK("DEV_CAP_RSVD_XRC",     QUERY_DEV_CAP_RSVD_XRC_OFFSET,     0x66);
    CHECK("DEV_CAP_MAX_XRC",      QUERY_DEV_CAP_MAX_XRC_OFFSET,      0x67);
    CHECK("DEV_CAP_MAX_COUNTERS", QUERY_DEV_CAP_MAX_COUNTERS_OFFSET, 0x68);
    CHECK("DEV_CAP_EXT_2_FLAGS",  QUERY_DEV_CAP_EXT_2_FLAGS_OFFSET,  0x70);
    CHECK("DEV_CAP_FS_IPOIB",     QUERY_DEV_CAP_FLOW_STEERING_IPOIB_OFFSET, 0x74);
    CHECK("DEV_CAP_FS_RANGE_EN",  QUERY_DEV_CAP_FLOW_STEERING_RANGE_EN_OFFSET, 0x76);
    CHECK("DEV_CAP_FS_MAX_QP",    QUERY_DEV_CAP_FLOW_STEERING_MAX_QP_OFFSET, 0x77);
    CHECK("DEV_CAP_SL2VL_EVENT",  QUERY_DEV_CAP_SL2VL_EVENT_OFFSET,  0x78);
    CHECK("DEV_CAP_ECN_QCN_VER",  QUERY_DEV_CAP_ECN_QCN_VER_OFFSET,  0x7b);

    /* Entry sizes and the limits: the ones the profile is computed from. */
    CHECK("DEV_CAP_RDMARC_ENTRY_SZ", QUERY_DEV_CAP_RDMARC_ENTRY_SZ_OFFSET, 0x80);
    CHECK("DEV_CAP_QPC_ENTRY_SZ",    QUERY_DEV_CAP_QPC_ENTRY_SZ_OFFSET,    0x82);
    CHECK("DEV_CAP_AUX_ENTRY_SZ",    QUERY_DEV_CAP_AUX_ENTRY_SZ_OFFSET,    0x84);
    CHECK("DEV_CAP_ALTC_ENTRY_SZ",   QUERY_DEV_CAP_ALTC_ENTRY_SZ_OFFSET,   0x86);
    CHECK("DEV_CAP_EQC_ENTRY_SZ",    QUERY_DEV_CAP_EQC_ENTRY_SZ_OFFSET,    0x88);
    CHECK("DEV_CAP_CQC_ENTRY_SZ",    QUERY_DEV_CAP_CQC_ENTRY_SZ_OFFSET,    0x8a);
    CHECK("DEV_CAP_SRQ_ENTRY_SZ",    QUERY_DEV_CAP_SRQ_ENTRY_SZ_OFFSET,    0x8c);
    CHECK("DEV_CAP_C_MPT_ENTRY_SZ",  QUERY_DEV_CAP_C_MPT_ENTRY_SZ_OFFSET,  0x8e);
    CHECK("DEV_CAP_MTT_ENTRY_SZ",    QUERY_DEV_CAP_MTT_ENTRY_SZ_OFFSET,    0x90);
    CHECK("DEV_CAP_D_MPT_ENTRY_SZ",  QUERY_DEV_CAP_D_MPT_ENTRY_SZ_OFFSET,  0x92);
    CHECK("DEV_CAP_BMME_FLAGS",      QUERY_DEV_CAP_BMME_FLAGS_OFFSET,      0x94);
    CHECK("DEV_CAP_MAX_ICM_SZ",      QUERY_DEV_CAP_MAX_ICM_SZ_OFFSET,      0xa0);
}

/* FreeBSD mlx4_fw.c:1834-1878 -- INIT_HCA inbox layout */
static void test_inithca_offsets(void)
{
    CHECK("INIT_HCA_IN_SIZE",       INIT_HCA_IN_SIZE,            0x200);
    CHECK("INIT_HCA_VERSION",       INIT_HCA_VERSION,            2);
    CHECK("INIT_HCA_VERSION_OFF",   INIT_HCA_VERSION_OFFSET,     0x000);
    CHECK("INIT_HCA_CACHELINE",     INIT_HCA_CACHELINE_SZ_OFFSET,0x00e);
    CHECK("INIT_HCA_FLAGS",         INIT_HCA_FLAGS_OFFSET,       0x014);
    CHECK("INIT_HCA_QPC",           INIT_HCA_QPC_OFFSET,         0x020);
    CHECK("INIT_HCA_QPC_BASE",      INIT_HCA_QPC_BASE_OFFSET,    0x030);
    CHECK("INIT_HCA_LOG_QP",        INIT_HCA_LOG_QP_OFFSET,      0x037);
    CHECK("INIT_HCA_SRQC_BASE",     INIT_HCA_SRQC_BASE_OFFSET,   0x048);
    CHECK("INIT_HCA_LOG_SRQ",       INIT_HCA_LOG_SRQ_OFFSET,     0x04f);
    CHECK("INIT_HCA_CQC_BASE",      INIT_HCA_CQC_BASE_OFFSET,    0x050);
    CHECK("INIT_HCA_LOG_CQ",        INIT_HCA_LOG_CQ_OFFSET,      0x057);
    CHECK("INIT_HCA_EQE_CQE",       INIT_HCA_EQE_CQE_OFFSETS,    0x058);
    CHECK("INIT_HCA_EQE_CQE_STRIDE",INIT_HCA_EQE_CQE_STRIDE_OFFSET, 0x05b);
    CHECK("INIT_HCA_ALTC_BASE",     INIT_HCA_ALTC_BASE_OFFSET,   0x060);
    CHECK("INIT_HCA_AUXC_BASE",     INIT_HCA_AUXC_BASE_OFFSET,   0x070);
    CHECK("INIT_HCA_EQC_BASE",      INIT_HCA_EQC_BASE_OFFSET,    0x080);
    CHECK("INIT_HCA_LOG_EQ",        INIT_HCA_LOG_EQ_OFFSET,      0x087);
    CHECK("INIT_HCA_NUM_SYS_EQS",   INIT_HCA_NUM_SYS_EQS_OFFSET, 0x08a);
    CHECK("INIT_HCA_RDMARC_BASE",   INIT_HCA_RDMARC_BASE_OFFSET, 0x090);
    CHECK("INIT_HCA_LOG_RD",        INIT_HCA_LOG_RD_OFFSET,      0x097);
    CHECK("INIT_HCA_MCAST",         INIT_HCA_MCAST_OFFSET,       0x0c0);
    CHECK("INIT_HCA_MC_BASE",       INIT_HCA_MC_BASE_OFFSET,     0x0c0);
    CHECK("INIT_HCA_LOG_MC_ENTRY",  INIT_HCA_LOG_MC_ENTRY_SZ_OFFSET, 0x0d2);
    CHECK("INIT_HCA_LOG_MC_HASH",   INIT_HCA_LOG_MC_HASH_SZ_OFFSET,  0x0d6);
    CHECK("INIT_HCA_UC_STEERING",   INIT_HCA_UC_STEERING_OFFSET, 0x0d8);
    CHECK("INIT_HCA_LOG_MC_TABLE",  INIT_HCA_LOG_MC_TABLE_SZ_OFFSET, 0x0db);
    CHECK("INIT_HCA_TPT",           INIT_HCA_TPT_OFFSET,         0x0f0);
    CHECK("INIT_HCA_DMPT_BASE",     INIT_HCA_DMPT_BASE_OFFSET,   0x0f0);
    CHECK("INIT_HCA_TPT_MW",        INIT_HCA_TPT_MW_OFFSET,      0x0f8);
    CHECK("INIT_HCA_LOG_MPT_SZ",    INIT_HCA_LOG_MPT_SZ_OFFSET,  0x0fb);
    CHECK("INIT_HCA_MTT_BASE",      INIT_HCA_MTT_BASE_OFFSET,    0x100);
    CHECK("INIT_HCA_CMPT_BASE",     INIT_HCA_CMPT_BASE_OFFSET,   0x108);
    CHECK("INIT_HCA_UAR",           INIT_HCA_UAR_OFFSET,         0x120);
    CHECK("INIT_HCA_LOG_UAR_SZ",    INIT_HCA_LOG_UAR_SZ_OFFSET,  0x12a);
    CHECK("INIT_HCA_UAR_PAGE_SZ",   INIT_HCA_UAR_PAGE_SZ_OFFSET, 0x12b);
}

/*
 * Consistency: the derived values, which are the ones a copied constant does not
 * protect.  These are the relationships the code relies on, checked as
 * relationships rather than as literals.
 */
static void test_derived(void)
{
    /* cMPT sub-tables are spaced entry_size << CMPT_SHIFT apart, and the number
     * of objects in the cMPT region is the type count shifted by the same amount
     * (mlx4.h:95-104). */
    CHECK("CMPT_SHIFT",       MLX4_CMPT_SHIFT, 24);
    CHECK("CMPT type QP",     MLX4_CMPT_TYPE_QP,  0);
    CHECK("CMPT type SRQ",    MLX4_CMPT_TYPE_SRQ, 1);
    CHECK("CMPT type CQ",     MLX4_CMPT_TYPE_CQ,  2);
    CHECK("CMPT type EQ",     MLX4_CMPT_TYPE_EQ,  3);
    CHECK("CMPT num types",   4, 4);
    /* The three values the INIT_HCA flags dword is built from must not collide
     * with the endianness bit or with each other. */
    CHECK("FLAG2 RSS bit",    MLX4_DEV_CAP_FLAG2_RSS,           1ull << 0);
    CHECK("FLAG2 TS bit",     MLX4_DEV_CAP_FLAG2_TS,            1ull << 5);
    CHECK("FLAG2 VXLAN bit",  MLX4_DEV_CAP_FLAG2_VXLAN_OFFLOADS,1ull << 10);
    CHECK("FLAG2 SYS_EQS bit",MLX4_DEV_CAP_FLAG2_SYS_EQS,       1ull << 17);
    CHECK("MEM_WINDOW bit",   MLX4_DEV_CAP_FLAG_MEM_WINDOW,     1ull << 16);
    CHECK("MW enable value",  INIT_HCA_TPT_MW_ENABLE,           1u << 7);
    CHECK("BMME type2 win",   MLX4_BMME_FLAG_TYPE_2_WIN,        1u << 9);
    CHECK("EQE 64B flag",     MLX4_DEV_CAP_FLAG_64B_EQE,        1ull << 61);
    CHECK("CQE 64B flag",     MLX4_DEV_CAP_FLAG_64B_CQE,        1ull << 62);
    /* The ICM chunk is 256 KiB and the two sizes must be consistent with the
     * page size the allocator works in. */
    CHECK("TABLE_CHUNK_SIZE", MLX4_TABLE_CHUNK_SIZE, 1u << 18);
    CHECK("ICM page size",    MLX4_ICM_PAGE_SIZE,    1u << MLX4_ICM_PAGE_SHIFT);
    CHECK("chunk in pages",   MLX4_TABLE_CHUNK_SIZE / MLX4_ICM_PAGE_SIZE, 64);
    CHECK("MAX_MSIX",         MLX4_MAX_MSIX,         64);
    CHECK("MAX_EQ_NUM",       MLX4_MAX_EQ_NUM,       1024);
    CHECK("DEFAULT MGM log",  MLX4_DEFAULT_MGM_LOG_ENTRY_SIZE, 10);
    CHECK("steering A0",      MLX4_STEERING_MODE_A0, 0);
    CHECK("NUM_PDS",          MLX4_NUM_PDS,          1u << 15);
}

int main(void)
{
    printf("M2d offset cross-check against the FreeBSD reference\n\n");
    printf("opcodes:\n");              test_opcodes();
    printf("QUERY_DEV_CAP offsets:\n"); test_devcap_offsets();
    printf("INIT_HCA offsets:\n");      test_inithca_offsets();
    printf("derived values:\n");        test_derived();
    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
