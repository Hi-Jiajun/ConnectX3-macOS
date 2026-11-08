/*
 * test-cqe-offsets.c -- the CQE's byte layout, and the send WQE's, as a build-time fact.
 *
 * Why this exists.  Stage 9 is the first stage that READS a completion element and the first
 * that writes a work request, and both layouts have exactly one source in this tree: the
 * FreeBSD reference under vendor/freebsd-src/ (struct mlx4_cqe, cq.h:41-60; struct
 * mlx4_wqe_ctrl_seg / mlx4_wqe_data_seg, qp.h:302-330 and :440-444).  DPDK's CQE was not
 * obtained -- docs/EXTERNAL-REFS.md section 12.3 records that as the gap -- so "the offset is
 * right" cannot be two independent implementations agreeing.  What it can be is a checked
 * fact: this program is compiled with a mirror of the reference's declaration and compares
 * the constants in src/mlx4_regs.h against the layout the C compiler derives from that
 * declaration, so a transcription slip is a build failure rather than a wrong number read
 * off the card.
 *
 * It is not hypothetical.  The stage's own plan (docs/R4-STAGE9-PLAN.md section 3.2) carried
 * status at 0x0e, byte_cnt at 0x12 and wqe_index at 0x16; the real offsets are 0x10, 0x14 and
 * 0x18, because the union that begins at 0x0e contributes its rlid/status halves BEFORE
 * byte_cnt -- the plan had skipped the two bytes of rlid.  Reading byte_cnt at 0x12 on a card
 * where the field is at 0x14 gives a plausible number (the low half of the status word mixed
 * with the byte count) and every conclusion drawn from it would be about that number.  That
 * is this project's D family -- something that looked like a measurement -- and the guard is
 * to compute the layout instead of copying it.
 *
 * The second half of the file is the send side, and there the interesting assertion is the
 * BIT POSITION of the opcode.  mlx4_en_tx.c:682-683 and :761-762 build the control segment's
 * first dword as `MLX4_OPCODE_SEND | owner_bit`, i.e. the opcode in the low eight bits, with
 * the producer count OR'd in at bits 8..23 (:897).  Shifting the opcode by 24 instead -- one
 * keystroke, and what the plan first said -- puts it in the OWNER byte and leaves byte 3 at
 * zero, where the opcode field then reads MLX4_OPCODE_NOP (0x00, device.h:365): the doorbell
 * is rung, a completion can still arrive, and nothing is transmitted.  The byte-level checks
 * below are written so that encoding is a failing build rather than a silent no-op.
 *
 * Build and run as part of build.sh; it is a host program, no kernel involved.
 *
 * The third thing this file holds down, added in 0.43.8, is WHERE WQE 0 IS -- the one place the
 * send queue and the receive ring are not the same shape, and the one place a wrong answer is
 * silent.  The send queue's WQE 0 is at its MTT base (mlx4_en_tx.c:139, :686) and the receive
 * ring's is 64 bytes past its own (mlx4_en_rx.c:406, :482-489).  0.43.6 unified the two shapes,
 * 0.43.7 put the receive ring's header in front of the send queue's descriptors, and the card
 * executed nothing at all -- no completion, no frame, no error, because the hardware read the
 * base and found a stamped "not a descriptor" unit there (docs/DEFECTS.md E13).  Both offsets
 * and the index arithmetic of each queue are asserted below, so re-unifying them is a failing
 * build rather than a silent regression.
 */

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

/* The header under test -- pure #defines, no kernel dependencies. */
#include "../src/mlx4_regs.h"
/* ...and the ABI, which cannot include the header above (both sides include it in either
 * order) so it carries its own literals for the two byte-array widths.  Those literals are
 * checked against this header's constants here, which is the only place that sees both. */
#include "../src/cx3_abi.h"

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

/*
 * A mirror of struct mlx4_cqe (cq.h:41-60), field for field and in the same order, with
 * fixed-width types so the host compiler agrees with the kernel build about widths.  The
 * layout this program checks is therefore the compiler's reading of the reference's
 * declaration, not a third hand-transcription of the offsets.
 */
struct mlx4_cqe_mirror {
    uint32_t vlan_my_qpn;
    uint32_t immed_rss_invalid;
    uint32_t g_mlpath_rqpn;
    uint16_t sl_vid;
    union {
        struct {
            uint16_t rlid;
            uint16_t status;
            uint8_t  ipv6_ext_mask;
            uint8_t  badfcs_enc;
        };
        uint8_t smac[6];
    };
    uint32_t byte_cnt;
    uint16_t wqe_index;
    uint16_t checksum;
    uint8_t  reserved[3];
    uint8_t  owner_sr_opcode;
};

/* struct mlx4_err_cqe (cq.h:63-72): what the SAME 32 bytes mean when the opcode is ERROR.
 * Checked because stage 9 prints the opcode before it interprets anything, and the two
 * structs agree on where my_qpn and wqe_index are -- which is worth knowing rather than
 * assuming when a failure has to be read. */
struct mlx4_err_cqe_mirror {
    uint32_t my_qpn;
    uint32_t reserved1[5];
    uint16_t wqe_index;
    uint8_t  vendor_err_syndrome;
    uint8_t  syndrome;
    uint8_t  reserved2[3];
    uint8_t  owner_sr_opcode;
};

static void test_cqe_layout(void)
{
    printf("struct mlx4_cqe: %zu bytes (cq.h:41-60)\n",
           sizeof(struct mlx4_cqe_mirror));
    CHECK("sizeof == CQE_LEGACY_SIZE", sizeof(struct mlx4_cqe_mirror), CQE_LEGACY_SIZE);
    CHECK("sizeof == CQE_LEGACY_TOTAL", sizeof(struct mlx4_cqe_mirror), CQE_LEGACY_TOTAL);
    CHECK("sizeof == 32, the reference's literal", sizeof(struct mlx4_cqe_mirror), 32);

    /* Every offset in the header, against the compiler's view of cq.h's declaration. */
    CHECK("vlan_my_qpn",    CQE_VLAN_MY_QPN_OFFSET,
          offsetof(struct mlx4_cqe_mirror, vlan_my_qpn));
    CHECK("immed_rss_invalid", CQE_IMMED_RSS_INVALID_OFFSET,
          offsetof(struct mlx4_cqe_mirror, immed_rss_invalid));
    CHECK("g_mlpath_rqpn",  CQE_G_MLPATH_RQPN_OFFSET,
          offsetof(struct mlx4_cqe_mirror, g_mlpath_rqpn));
    CHECK("sl_vid",         CQE_SL_VID_OFFSET,
          offsetof(struct mlx4_cqe_mirror, sl_vid));
    CHECK("rlid",           CQE_RLID_OFFSET,
          offsetof(struct mlx4_cqe_mirror, rlid));
    CHECK("status",         CQE_STATUS_OFFSET,
          offsetof(struct mlx4_cqe_mirror, status));
    CHECK("ipv6_ext_mask",  CQE_IPV6_EXT_MASK_OFFSET,
          offsetof(struct mlx4_cqe_mirror, ipv6_ext_mask));
    CHECK("badfcs_enc",     CQE_BADFCS_ENC_OFFSET,
          offsetof(struct mlx4_cqe_mirror, badfcs_enc));
    CHECK("byte_cnt",       CQE_BYTE_CNT_OFFSET,
          offsetof(struct mlx4_cqe_mirror, byte_cnt));
    CHECK("wqe_index",      CQE_WQE_INDEX_OFFSET,
          offsetof(struct mlx4_cqe_mirror, wqe_index));
    CHECK("checksum",       CQE_CHECKSUM_OFFSET,
          offsetof(struct mlx4_cqe_mirror, checksum));
    CHECK("owner_sr_opcode", CQE_OWNER_SR_OPCODE_OFFSET,
          offsetof(struct mlx4_cqe_mirror, owner_sr_opcode));
    CHECK("owner byte == CQE_OWNER_OFFSET", CQE_OWNER_SR_OPCODE_OFFSET, CQE_OWNER_OFFSET);

    /*
     * ...and the four literals the plan got wrong.  Written as literals rather than as
     * expressions so that a future edit to cq.h's transcription in mlx4_regs.h is not
     * silently followed here: changing the header has to be a visible edit in this file
     * too, which is the convention test-m2d-offsets.c states.
     */
    CHECK("byte_cnt is at 0x14 (the plan said 0x12)", CQE_BYTE_CNT_OFFSET, 0x14);
    CHECK("wqe_index is at 0x18 (the plan said 0x16)", CQE_WQE_INDEX_OFFSET, 0x18);
    CHECK("status is at 0x10 (the plan said 0x0e)", CQE_STATUS_OFFSET, 0x10);
    CHECK("owner byte is the 32nd", CQE_OWNER_SR_OPCODE_OFFSET, 0x1f);

    /* Every field has to END inside the 32-byte legacy CQE. */
    CHECK("vlan_my_qpn ends inside", CQE_VLAN_MY_QPN_OFFSET + 4 <= CQE_LEGACY_TOTAL, 1);
    CHECK("status ends inside",      CQE_STATUS_OFFSET + 2 <= CQE_LEGACY_TOTAL, 1);
    CHECK("byte_cnt ends inside",    CQE_BYTE_CNT_OFFSET + 4 <= CQE_LEGACY_TOTAL, 1);
    CHECK("wqe_index ends inside",   CQE_WQE_INDEX_OFFSET + 2 <= CQE_LEGACY_TOTAL, 1);
    CHECK("owner ends inside",       CQE_OWNER_SR_OPCODE_OFFSET + 1 <= CQE_LEGACY_TOTAL, 1);

    /* The error CQE: same my_qpn position, same wqe_index position, same opcode byte. */
    CHECK("err sizeof == 32", sizeof(struct mlx4_err_cqe_mirror), 32);
    CHECK("err my_qpn where the good CQE's qpn is",
          offsetof(struct mlx4_err_cqe_mirror, my_qpn), CQE_VLAN_MY_QPN_OFFSET);
    CHECK("err wqe_index where the good CQE's is",
          offsetof(struct mlx4_err_cqe_mirror, wqe_index), CQE_WQE_INDEX_OFFSET);
    CHECK("err opcode byte where the good CQE's owner byte is",
          offsetof(struct mlx4_err_cqe_mirror, owner_sr_opcode),
          CQE_OWNER_SR_OPCODE_OFFSET);
}

static void test_cqe_slot(void)
{
    unsigned factor;

    printf("\n64-byte slots: the real CQE is the second half\n");
    CHECK("slot size == two legacy CQEs", MLX4_CQE_SLOT_SIZE, 2 * CQE_LEGACY_SIZE);
    for (factor = 0; factor <= 1; ++factor) {
        unsigned stride = CQE_LEGACY_SIZE << factor;
        unsigned i;
        /* The slot's stride is the CQE size's own factor, so the arithmetic below is the
         * same one cqNextCqe uses: slot = cons & (nent-1), off = slot*stride + 32*factor. */
        CHECK("stride with factor 1 is the slot size", (factor ? stride : MLX4_CQE_SLOT_SIZE),
              MLX4_CQE_SLOT_SIZE);
        for (i = 0; i < 4; ++i) {
            unsigned off = i * stride + (factor ? CQE_LEGACY_SIZE : 0u);
            /* The owner byte read by the consumer must be the last byte of the legacy CQE
             * inside that slot: with factor 1 the struct sits at +32 and ends at +64. */
            CHECK("owner byte offset in the slot",
                  off + CQE_OWNER_OFFSET,
                  i * stride + (factor ? CQE_LEGACY_SIZE : 0u) + CQE_LEGACY_SIZE - 1);
        }
    }
    CHECK("CQE_SLOT_OWNER_BYTE(1) is the slot's last byte",
          CQE_SLOT_OWNER_BYTE(1) + 1, MLX4_CQE_SLOT_SIZE);
    CHECK("CQE_SLOT_OWNER_BYTE(0) is the first half's last byte",
          CQE_SLOT_OWNER_BYTE(0) + 1, CQE_LEGACY_SIZE);
}

static void test_cqe_opcode(void)
{
    printf("\nCQE owner / opcode byte (cq.h:99-101, device.h:387)\n");
    CHECK("owner mask",     MLX4_CQE_OWNER_MASK, 0x80);
    CHECK("send mask",      MLX4_CQE_IS_SEND_MASK, 0x40);
    CHECK("opcode mask",    MLX4_CQE_OPCODE_MASK, 0x1f);
    CHECK("ERROR opcode",   MLX4_CQE_OPCODE_ERROR, 0x1e);
    CHECK("opcode mask does not overlap the send bit",
          (MLX4_CQE_OPCODE_MASK & MLX4_CQE_IS_SEND_MASK), 0);
    CHECK("nor the owner bit", (MLX4_CQE_OPCODE_MASK & MLX4_CQE_OWNER_MASK), 0);
    CHECK("QPN mask is the low 24 bits", MLX4_CQE_QPN_MASK, 0xffffff);
    CHECK("BAD_FCS is bit 4 of badfcs_enc", MLX4_CQE_BAD_FCS, 1u << 4);
}

/*
 * The send WQE's control segment: the one place where a single wrong shift produces a
 * descriptor that completes and transmits nothing.
 */
static void test_tx_wqe(const uint32_t ownerBit, const uint32_t prod, uint8_t b[4])
{
    uint32_t dword = (uint32_t)MLX4_OPCODE_SEND |
                     ((prod & MLX4_TX_PROD_MASK) << MLX4_TX_PROD_SHIFT) |
                     (ownerBit << MLX4_TX_OWNER_SHIFT);

    /* Big-endian on the wire, which is why byte 3 is where the opcode has to be. */
    b[0] = (uint8_t)((dword >> 24) & 0xffu);
    b[1] = (uint8_t)((dword >> 16) & 0xffu);
    b[2] = (uint8_t)((dword >>  8) & 0xffu);
    b[3] = (uint8_t)( dword        & 0xffu);
}

static void test_tx_wqe_layout(void)
{
    uint8_t b[4];
    uint32_t prod, owner;

    printf("\nstruct mlx4_wqe_ctrl_seg + one data segment (qp.h:302-330, :440-444)\n");
    CHECK("ctrl seg size", TX_WQE_CTRL_SIZE, 16);
    CHECK("data seg size", TX_WQE_DATA_SIZE, 16);
    CHECK("data seg begins where the ctrl seg ends", TX_WQE_BYTE_COUNT_OFFSET,
          TX_WQE_CTRL_SIZE);
    CHECK("a posted WQE is ctrl + data", TX_WQE_SEND_SIZE,
          TX_WQE_CTRL_SIZE + TX_WQE_DATA_SIZE);
    CHECK("...and fits in one TXBB", TX_WQE_SEND_SIZE <= MLX4_TXBB_SIZE, 1);
    CHECK("byte_count offset", TX_WQE_BYTE_COUNT_OFFSET, 0x10);
    CHECK("lkey offset",       TX_WQE_LKEY_OFFSET, 0x14);
    CHECK("addr offset",       TX_WQE_ADDR_OFFSET, 0x18);
    CHECK("addr ends inside the WQE", TX_WQE_ADDR_OFFSET + 8 <= TX_WQE_SEND_SIZE, 1);
    CHECK("imm offset",        TX_WQE_IMM_OFFSET, 0x0c);
    CHECK("srcrb_flags offset", TX_WQE_SRCRB_FLAGS_OFFSET, 0x08);
    CHECK("fence_size is the last byte of the ctrl seg's second dword",
          TX_WQE_FENCE_SIZE_OFFSET, 0x07);
    /*
     * fence_size is the descriptor's size IN 16-BYTE UNITS INCLUDING THE CONTROL SEGMENT, and
     * this project wrote 1 for four builds because it took FreeBSD's `ds_cnt` for a count of
     * data segments (docs/M2B-NOTES.md section 54).  Pinned three ways: the value the writer
     * derives for this WQE, the formula, and the two things the formula must not be -- the
     * segment count, and the control segment's own size divided by sixteen.
     */
    CHECK("a ctrl + 1 data segment WQE declares two 16-byte units",
          (uint32_t)TX_WQE_SEND_FENCE_SIZE, 2u);
    CHECK("...which is the whole descriptor over sixteen",
          (uint32_t)TX_WQE_SEND_FENCE_SIZE,
          (uint32_t)((TX_WQE_CTRL_SIZE + TX_WQE_DATA_SIZE) / 16u));
    CHECK("...and NOT the number of data segments",
          (uint32_t)TX_WQE_SEND_FENCE_SIZE != 1u, 1u);
    CHECK("...and NOT just the control segment",
          (uint32_t)TX_WQE_SEND_FENCE_SIZE != (uint32_t)(TX_WQE_CTRL_SIZE / 16u), 1u);
    CHECK("the formula counts the control segment plus each data segment",
          (uint32_t)TX_WQE_FENCE_SIZE(3u), 4u);

    /* The CTRL_FLAGS the reference sends: C (bits [3:2]) and SE (bit 1). */
    CHECK("CQ_UPDATE is the 2-bit field 3 << 2", MLX4_WQE_CTRL_CQ_UPDATE, 0x0c);
    CHECK("SOLICITED is 1 << 1", MLX4_WQE_CTRL_SOLICITED, 0x02);
    CHECK("the reference's CTRL_FLAGS", MLX4_WQE_CTRL_CQ_UPDATE | MLX4_WQE_CTRL_SOLICITED,
          0x0e);
    CHECK("...and its bits [3:2] are non-zero, i.e. a completion is asked for",
          ((MLX4_WQE_CTRL_CQ_UPDATE | MLX4_WQE_CTRL_SOLICITED) >> 2) & 0x3u, 3);
    CHECK("the owner bit is bit 31", MLX4_WQE_CTRL_OWN, 0x80000000u);
    CHECK("FORCE_LOOPBACK exists and is NOT part of the flags we send",
          MLX4_WQE_CTRL_FORCE_LOOPBACK & (MLX4_WQE_CTRL_CQ_UPDATE | MLX4_WQE_CTRL_SOLICITED),
          0);

    printf("the control dword, byte for byte (big-endian)\n");
    /* The two phases the ring walks through: prod 0 is the first lap (owner bit 0), prod
     * 256 is the second (owner bit 1) -- mlx4_en_tx.c:682 `ring->prod & ring->size`. */
    for (prod = 0; prod < 600; prod += 256) {
        owner = (prod & 256u) ? 1u : 0u;
        test_tx_wqe(owner, prod, b);
        /* The opcode must be in the LAST byte: the big-endian low byte of the word. */
        CHECK("opcode in byte 3", b[3], MLX4_OPCODE_SEND);
        CHECK("...and not NOP there", b[3] != MLX4_OPCODE_NOP, 1);
        /* The owner bit is bit 7 of byte 0. */
        CHECK("owner bit in byte 0, bit 7", (b[0] & 0x80u) ? 1u : 0u, owner);
        /* The producer count occupies bits 8..23, i.e. all of byte 2 and the low 7 bits
         * of byte 1 (byte 1's top bit belongs to the owner). */
        CHECK("producer low byte in byte 2", b[2], prod & 0xffu);
        CHECK("producer high bits in byte 1", b[1], (prod >> 8) & 0x7fu);
        CHECK("byte 0's low 7 bits are unused", b[0] & 0x7fu, 0);
    }

    /*
     * And the mistake, made explicitly, so that the build fails if anyone re-introduces it:
     * the shifted form would put 0x0a in byte 0 and 0x00 in byte 3 -- i.e. MLX4_OPCODE_NOP
     * in the field the hardware reads, which is a descriptor that completes and sends
     * nothing.
     */
    printf("the shifted encoding the plan first specified (must NOT be what we send)\n");
    {
        uint32_t wrong = ((uint32_t)MLX4_OPCODE_SEND << 24);
        printf("  MLX4_OPCODE_SEND << 24 -> bytes %02x %02x %02x %02x\n",
               (wrong >> 24) & 0xffu, (wrong >> 16) & 0xffu, (wrong >> 8) & 0xffu,
               wrong & 0xffu);
        /* Byte 3 of a big-endian word is its LOW byte: that is the opcode field, and the
         * shifted encoding leaves it zero, i.e. MLX4_OPCODE_NOP. */
        CHECK("the shifted form's opcode byte (byte 3) is NOP", wrong & 0xffu,
              MLX4_OPCODE_NOP);
        CHECK("...while byte 0 -- the owner byte -- wrongly carries the opcode",
              (wrong >> 24) & 0xffu, MLX4_OPCODE_SEND);
        test_tx_wqe(0, 0, b);
        CHECK("the encoding we send is NOT that", b[3] == MLX4_OPCODE_SEND &&
              b[0] != MLX4_OPCODE_SEND, 1);
        CHECK("NOP and SEND are different values, which is what makes it detectable",
              MLX4_OPCODE_NOP != MLX4_OPCODE_SEND, 1);
    }
}

static void test_doorbell_and_frame(void)
{
    printf("\nthe send doorbell (doorbell.h:41) and the frame this stage sends\n");
    CHECK("MLX4_SEND_DOORBELL", MLX4_SEND_DOORBELL, 0x14);
    CHECK("MLX4_CQ_DOORBELL is a different register", MLX4_CQ_DOORBELL, 0x20);
    CHECK("the two differ by 0xC -- the mistake this constant exists to prevent",
          MLX4_CQ_DOORBELL - MLX4_SEND_DOORBELL, 0xc);
    CHECK("the QPN is shifted left by 8", MLX4_SEND_DB_QPN_SHIFT, 8);
    /* The doorbell value is a QPN, not a producer index: for QPN 66 it is 16896. */
    CHECK("the doorway value for a QPN", (uint32_t)(66u << MLX4_SEND_DB_QPN_SHIFT), 16896);
    CHECK("the frame is 60 bytes without the FCS", CX3_R4RAW_FRAME_BYTES, 60);
    CHECK("...which is a legal minimum once the FCS is added",
          CX3_R4RAW_FRAME_BYTES + 4, 64);
    CHECK("ethertype is Local Experimental 1", CX3_R4RAW_ETHERTYPE, 0x88b5);
    CHECK("ethertype sits after the two MAC addresses", CX3_R4RAW_ETHERTYPE_OFFSET, 12);
    CHECK("payload starts after the ethertype", CX3_R4RAW_PAYLOAD_OFFSET, 14);
    CHECK("payload is long enough for the pattern",
          CX3_R4RAW_PAYLOAD_BYTES >= 32, 1);
    CHECK("payload ends exactly at the frame length",
          CX3_R4RAW_PAYLOAD_OFFSET + CX3_R4RAW_PAYLOAD_BYTES, CX3_R4RAW_FRAME_BYTES);
}

/*
 * The ABI's own copies of these sizes.  Two homes for one number, checked rather than trusted:
 * docs/DEFECTS.md F11 is the row for a number that lived in two places and disagreed, and the
 * way to keep that from happening here is to make the disagreement a build failure.
 */
static void test_abi_widths(void)
{
    printf("\nthe ABI's byte arrays, against the constants above\n");
    CHECK("txWqeImage is one legacy CQE wide",
          sizeof(((Cx3R4RawResult *)0)->txWqeImage), CQE_LEGACY_TOTAL);
    CHECK("txCqeRaw is one legacy CQE wide",
          sizeof(((Cx3R4RawResult *)0)->txCqeRaw), CQE_LEGACY_TOTAL);
    CHECK("rxFirstRaw is one legacy CQE wide",
          sizeof(((Cx3R4RawResult *)0)->rxFirstRaw), CQE_LEGACY_TOTAL);
    CHECK("rxFirstBytes holds the frame bytes that are kept",
          sizeof(((Cx3R4RawResult *)0)->rxFirstBytes), CX3_R4RAW_RX_KEEP_BYTES);
    CHECK("rxOursBytes too",
          sizeof(((Cx3R4RawResult *)0)->rxOursBytes), CX3_R4RAW_RX_KEEP_BYTES);
    CHECK("the slot bitmap is a whole number of bytes",
          CX3_R4RAW_MAX_RX_SLOTS % 8u, 0);
    CHECK("the scalar block fits the kernel's cap",
          kR4RawOutCount <= CX3_SCALAR_OUT_MAX, 1);
    /*
     * The selector block is closed, and the check is written against the LAST one so that
     * adding a selector has to come here and move this line on purpose.  0.43.11 is the first
     * time it moved: txprobe was added after r4raw because r4raw's scalar block is full at
     * sixteen and its behaviour is what four boots of transmit failures were read against.
     * 0.44.10 is the second: netstat was added after netup, because the network data path's
     * counters could only be read from the kernel log and every question about it therefore
     * cost a log window that had to have been started at the right moment.
     * 0.44.11 is the third: netlink (the media table and the link re-reading) was added after
     * netstat, because publishing a media dictionary changes the link report the M4 acceptance
     * depends on and M4 has not run yet -- so it is reachable at runtime instead.
     * 0.44.12 is the fourth: netoff (the offload switches) after netlink.  The receive path's
     * checksum marking is per frame, so a wrong verdict would corrupt or drop frames in the very
     * test M4 has not run yet -- the switch defaults to off and is reachable at runtime.
     * 0.44.14 is the fifth: netarm (the drain-then-arm experiment) after netoff.  It is a
     * MEASUREMENT rather than a feature, and it is here because stage 9's arm measurement could
     * not be interpreted (the queue was not empty), which left the receive chain's last link
     * unproved in both directions.
     * 0.44.15 is the sixth: netfilt (the hardware receive filters) after netarm.  It programmes
     * the port's multicast table, which is the whole mechanism for group addresses on this card
     * because it is in A0 steering -- mlx4_multicast_attach() does nothing for Ethernet under
     * A0 (mlx4_mcg.c:1408-1417).
     * 0.44.17 is the seventh: netport (stop the port) after netfilt.  It is the first half of
     * every port reconfiguration -- the reference's change_mtu is stop-then-start -- and the
     * second half already existed as the chain's port-up step, which takes the MTU as an input.
     *
     * Two selectors, one number each: the count must be exactly one past the highest, and the
     * highest must be the one named here.  A gap (a selector id with no dispatch entry) is
     * caught by the user client's own pinned table sizes at build time; this catches the other
     * direction, an id added past the end of the block.
     */
    /*
     * 0.44.30 is the eighth: netupstop (46), which is netup's first step and nothing else.  It
     * exists to separate "the steps after the address" from "entering this path at all" -- five
     * machine deaths are inside the netup call and one of them produced no driver log at all.
     *
     * queryadapter (47) and pcielink (48) stay fixed; runtime identity (49) appends
     * host-only service/build/generation metadata.
     * Keep all prior IDs stable and pin the new selector and wire layout as well as checking
     * the new end of the block. Future additions must be acknowledged
     * here rather than silently weakening the closed-selector guard.
     */
    CHECK("netupstop retains selector 46", kCx3SelNetUpStop, 46);
    CHECK("netupstop follows netport",
          (uint32_t)kCx3SelNetUpStop - (uint32_t)kCx3SelNetPort, 1);
    CHECK("queryadapter retains selector 47", kCx3SelQueryAdapter, 47);
    CHECK("queryadapter follows netupstop",
          (uint32_t)kCx3SelQueryAdapter - (uint32_t)kCx3SelNetUpStop, 1);
    CHECK("pcielink retains selector 48", kCx3SelPcieLink, 48);
    CHECK("pcielink follows queryadapter",
          (uint32_t)kCx3SelPcieLink - (uint32_t)kCx3SelQueryAdapter, 1);
    CHECK("runtime identity retains selector 49", kCx3SelRuntimeIdentity, 49);
    CHECK("runtime identity follows pcielink",
          (uint32_t)kCx3SelRuntimeIdentity - (uint32_t)kCx3SelPcieLink, 1);
    CHECK("module read appends selector 50", kCx3SelModuleRead, 50);
    CHECK("auto network appends selector 51", kCx3SelAutoNetwork, 51);
    CHECK("port info appends selector 52", kCx3SelPortInfo, 52);
    CHECK("native config is the last selector",
          (uint32_t)kCx3SelCount - (uint32_t)kCx3SelNativeConfig, 1);
    CHECK("native config selector", kCx3SelNativeConfig, 55);
    CHECK("native config result ABI", sizeof(struct Cx3NativeConfigResult), 56);
    CHECK("native port selector", kCx3SelNativePort, 54);
    CHECK("native port result ABI", sizeof(struct Cx3NativePortResult), 120);
    CHECK("port queue selector", kCx3SelPortQueue, 53);
    CHECK("port queue result ABI", sizeof(struct Cx3PortQueueResult), 96);
    CHECK("port info result ABI", sizeof(struct Cx3PortInfoResult), 152);
    CHECK("auto network result ABI", sizeof(struct Cx3AutoNetworkResult), 208);
    CHECK("auto network request ABI", sizeof(struct Cx3AutoNetworkRequest), 112);
    CHECK("module result size", sizeof(struct Cx3ModuleResult), 368);
    CHECK("module raw offset", offsetof(struct Cx3ModuleResult, raw), 64);
    CHECK("module data offset", offsetof(struct Cx3ModuleResult, data), 320);
    CHECK("runtime identity ABI version", CX3_RUNTIME_RESULT_VERSION, 1);
    CHECK("runtime identity size", sizeof(struct Cx3RuntimeIdentityResult), 176);
    CHECK("runtime service ID offset", offsetof(struct Cx3RuntimeIdentityResult, serviceRegistryId), 24);
    CHECK("runtime PCI snapshot offset", offsetof(struct Cx3RuntimeIdentityResult, pciIdentity), 64);
    CHECK("runtime build tag offset", offsetof(struct Cx3RuntimeIdentityResult, buildTag), 108);
    CHECK("PCIe result ABI version", CX3_PCIE_RESULT_VERSION, 1);
    CHECK("PCIe result size", sizeof(struct Cx3PcieLinkResult), 368);
    CHECK("PCIe identity before offset", offsetof(struct Cx3PcieLinkResult, identityBefore), 56);
    CHECK("PCIe identity after offset", offsetof(struct Cx3PcieLinkResult, identityAfter), 68);
    CHECK("PCIe raw offset", offsetof(struct Cx3PcieLinkResult, raw), 80);
    CHECK("PCIe validity bitmap offset", offsetof(struct Cx3PcieLinkResult, readValid), 336);
    CHECK("queryadapter ABI version", CX3_ADAPTER_RESULT_VERSION, 1);
    CHECK("queryadapter result bytes", sizeof(struct Cx3AdapterResult), 360);
    CHECK("queryadapter ID offset", offsetof(struct Cx3AdapterResult, boardId), 40);
    CHECK("queryadapter raw offset", offsetof(struct Cx3AdapterResult, raw), 104);
    CHECK("queryadapter raw bytes", sizeof(((struct Cx3AdapterResult *)0)->raw), 256);
    /*
     * Its outputs ARE netup's (same layout, so a reader can compare a stopped run with a complete
     * one), and its one input is the stop point.  Both are checked here so that the shape cannot
     * drift away from netup's without a line changing in this file.
     */
    CHECK("the cut-short selector reports the same slots as netup",
          kNetUpOutCount <= CX3_SCALAR_OUT_MAX, 1);
    CHECK("the cut-short selector's stop point is an input", kNetUpStopInCount, 1);
    CHECK("netport follows netfilt",
          (uint32_t)kCx3SelNetPort - (uint32_t)kCx3SelNetFilt, 1);
    CHECK("the port block fits the kernel's cap",
          kNetPortOutCount <= CX3_SCALAR_OUT_MAX, 1);
    CHECK("netfilt follows netarm",
          (uint32_t)kCx3SelNetFilt - (uint32_t)kCx3SelNetArm, 1);
    CHECK("the filter block fits the kernel's cap",
          kNetFiltOutCount <= CX3_SCALAR_OUT_MAX, 1);
    CHECK("netarm follows netoff",
          (uint32_t)kCx3SelNetArm - (uint32_t)kCx3SelNetOff, 1);
    CHECK("the arm experiment's block fits the kernel's cap",
          kNetArmOutCount <= CX3_SCALAR_OUT_MAX, 1);
    CHECK("netoff follows netlink",
          (uint32_t)kCx3SelNetOff - (uint32_t)kCx3SelNetLink, 1);
    CHECK("the offload block fits the kernel's cap",
          kNetOffOutCount <= CX3_SCALAR_OUT_MAX, 1);
    CHECK("netlink follows netstat",
          (uint32_t)kCx3SelNetLink - (uint32_t)kCx3SelNetStat, 1);
    CHECK("the media/link block fits the kernel's cap",
          kNetLinkOutCount <= CX3_SCALAR_OUT_MAX, 1);
    CHECK("netstat follows netup",
          (uint32_t)kCx3SelNetStat - (uint32_t)kCx3SelNetUp, 1);
    CHECK("the counters' scalar block fits the kernel's cap",
          kNetStatOutCount <= CX3_SCALAR_OUT_MAX, 1);
    CHECK("netup follows txprobe",
          (uint32_t)kCx3SelNetUp - (uint32_t)kCx3SelTxProbe, 1);
    CHECK("the interface selector's block fits the kernel's cap",
          kNetUpOutCount <= CX3_SCALAR_OUT_MAX, 1);
    CHECK("txprobe follows r4raw",
          (uint32_t)kCx3SelTxProbe - (uint32_t)kCx3SelR4Raw, 1);
    CHECK("the probe's scalar block fits the kernel's cap",
          kTxProbeOutCount <= CX3_SCALAR_OUT_MAX, 1);
    /* The frame fits in the page the driver allocates for it, and in one WQE's buffer size. */
    CHECK("the frame fits in one 4 KiB page", CX3_R4RAW_FRAME_BYTES <= 4096u, 1);

    /*
     * ...and the one hex-dump buffer size has to hold the LARGEST of the four things this stage
     * formats into it: the two 32-byte structures, the 60-byte frame that is sent, and the up to
     * 64 bytes of a received frame that are kept.  docs/DEFECTS.md E12 is the draft that sized
     * a buffer for the first and formatted the last into it.
     */
    printf("\nthe one hex-dump buffer size, against the four things it holds\n");
    CHECK("holds a legacy CQE", CX3_R4RAW_HEX_BUF_BYTES >= 3u * CQE_LEGACY_TOTAL + 1u, 1);
    CHECK("holds a send WQE", CX3_R4RAW_HEX_BUF_BYTES >= 3u * TX_WQE_SEND_SIZE + 1u, 1);
    CHECK("holds the frame this stage sends",
          CX3_R4RAW_HEX_BUF_BYTES >= 3u * CX3_R4RAW_FRAME_BYTES + 1u, 1);
    CHECK("holds the most of a received frame that is kept",
          CX3_R4RAW_HEX_BUF_BYTES >= 3u * CX3_R4RAW_RX_KEEP_BYTES + 1u, 1);
}

/*
 * WHERE WQE 0 IS: the send queue and the receive ring disagree, and the disagreement is the
 * hardware's.
 *
 * This is the one asymmetry in this project that a wrong answer to which is SILENT: with the
 * receive ring's 64-byte header in front of them, a send queue's descriptors land at +64 while
 * the hardware reads the MTT base -- where the driver has stamped 0xffffffff ("software owns",
 * i.e. not a descriptor).  The result is a doorbell that was rung, a descriptor nobody
 * executed, no completion, no frame and no error message.  Measured on 0.43.7:
 * docs/DEFECTS.md E13, docs/R4-STAGE9-RESULT.md section 12.
 *
 * So both offsets are asserted here rather than left to a reader noticing them, including the
 * index arithmetic each side uses (mlx4_en_tx.c:139/:686 against mlx4_en_rx.c:406/:482-489).
 */
static void test_sq_rq_header_asymmetry(void)
{
    unsigned i;

    printf("\nWQE 0: the send queue has no header, the receive ring has 64 bytes of one\n");
    CHECK("SQ_WQE0_OFFSET is 0 -- no unit in front of the WQEs", SQ_WQE0_OFFSET, 0);
    CHECK("RQ_WQE0_OFFSET is 64 -- TXBB_SIZE, the receive ring's reserved unit",
          RQ_WQE0_OFFSET, 64);
    CHECK("...and that is MLX4_TXBB_SIZE, not a second number with the same value",
          RQ_WQE0_OFFSET, MLX4_TXBB_SIZE);
    CHECK("...and it is RX_RING_HEADER_SIZE (one number, one home)",
          RQ_WQE0_OFFSET, RX_RING_HEADER_SIZE);
    CHECK("the two queues' WQE 0 are NOT at the same offset (this is the whole point)",
          SQ_WQE0_OFFSET != RQ_WQE0_OFFSET, 1);

    /* The SQ's ring is the WQE array alone: no header added to the allocation. */
    CHECK("a 256-TXBB SQ ring is exactly nent * TXBB_SIZE bytes",
          256u * MLX4_TXBB_SIZE, 16384u);
    CHECK("...which is mlx4_en_tx.c:123's ALIGN(size * stride, PAGE_SIZE) rounded up",
          ((256u * MLX4_TXBB_SIZE) + 4095u) / 4096u, 4u);

    /*
     * WQE i of each queue, as the two references index them -- note that the RQ's stride is
     * its 16-byte data segment (RX_WQE_SIZE) while the SQ's is the 64-byte TXBB, so the two
     * queues differ in BOTH terms: the additive 64 bytes and the multiplier.
     */
    for (i = 0; i < 4; ++i) {
        CHECK("WQE i of the SQ is at i * TXBB_SIZE from the base (+0)",
              MLX4_SQ_WQE_OFFSET(i), (uint64_t)i * (uint64_t)MLX4_TXBB_SIZE);
        CHECK("WQE i of the RQ is at 64 + i * 16 (its stride is the data segment)",
              RQ_WQE0_OFFSET + (uint64_t)i * (uint64_t)RX_WQE_SIZE,
              (uint64_t)RQ_WQE0_OFFSET + (uint64_t)i * 16u);
    }
    /* The first WQE of each: this is the number that was wrong. */
    CHECK("SQ: the first post (TXBB 0) is at offset 0, where the hardware reads",
          MLX4_SQ_WQE_OFFSET(0), 0);
    CHECK("RQ: the first fill (slot 0) is at offset 64, where the hardware reads",
          RQ_WQE0_OFFSET + 0u, 64);

    /*
     * ...and the consequence in the other direction: because the SQ's offset is
     * index * TXBB_SIZE with no additive term, a doorbell's TXBB index and the byte offset of
     * that WQE agree by construction (offset / TXBB_SIZE == index).  A header breaks the
     * identity, and the 0.43.7 defect was exactly that: the doorbell said TXBB 0 while the
     * descriptor was at the second unit.
     */
    printf("the doorbell's index and the SQ's byte offset, which a header would decouple\n");
    for (i = 0; i < 4; ++i) {
        CHECK("SQ: offset / TXBB_SIZE is the index again",
              MLX4_SQ_WQE_OFFSET(i) / MLX4_TXBB_SIZE, i);
    }
    CHECK("RQ: the same identity does NOT hold there (64 + i*16 is not i*16)",
          (RQ_WQE0_OFFSET + 3u * RX_WQE_SIZE) / MLX4_TXBB_SIZE, 1);
}

/*
 * The QP context's path fields, and in particular pri_path.feup at 0x3a -- the byte 0.43.9
 * started writing.
 *
 * Why a mirror and not a table: the feup offset cannot be quoted from one line of the
 * reference.  It is reached by two levels of nesting -- pri_path starts 0x18 bytes into struct
 * mlx4_qp_context (qp.h:196-236, after flags/pd/the three size-stride bytes/rlkey_roce_mode/
 * usr_page/local_qpn/remote_qpn) and feup is 34 bytes into struct mlx4_qp_path
 * (qp.h:140-160: fl, vlan_control, disable_pkey_check, pkey_index, counter_index, grh_mylmc,
 * rlid(BE16), ackto, mgid_index, static_rate, hop_limit, tclass_flowlabel(BE32), rgid[16],
 * sched_queue, vlan_index) -- and 0 in a wrong byte here is a context field silently left at
 * the memset's value, which is exactly the defect this batch fixes (docs/DEFECTS.md E14).
 *
 * The mirror is written field for field and in the declaration's order, with fixed-width types,
 * and it is checked TWO ways: against the reference's own names here, and against the three
 * offsets src/mlx4_regs.h already relied on before this batch (ackto, counter_index,
 * sched_queue).  If the transcription were wrong, those three would fail too -- which is what
 * makes the new one credible rather than a fourth hand-copy.
 */
typedef struct {
    uint8_t  fl;
    uint8_t  vlan_control;
    uint8_t  disable_pkey_check;
    uint8_t  pkey_index;
    uint8_t  counter_index;
    uint8_t  grh_mylmc;
    uint16_t rlid;
    uint8_t  ackto;
    uint8_t  mgid_index;
    uint8_t  static_rate;
    uint8_t  hop_limit;
    uint32_t tclass_flowlabel;
    uint8_t  rgid[16];
    uint8_t  sched_queue;
    uint8_t  vlan_index;
    uint8_t  feup;
    uint8_t  fvl_rx;
    uint8_t  reserved4[2];
    uint8_t  dmac[6];
} mlx4_qp_path_mirror;

typedef struct {
    uint32_t flags;
    uint32_t pd;
    uint8_t  mtu_msgmax;
    uint8_t  rq_size_stride;
    uint8_t  sq_size_stride;
    uint8_t  rlkey_roce_mode;
    uint32_t usr_page;
    uint32_t local_qpn;
    uint32_t remote_qpn;
    mlx4_qp_path_mirror pri_path;
    mlx4_qp_path_mirror alt_path;
    uint32_t params1;
    uint32_t reserved1;
    uint32_t next_send_psn;
    uint32_t cqn_send;
    uint16_t roce_entropy;
    uint16_t reserved2[3];
    uint32_t last_acked_psn;
    uint32_t ssn;
    uint32_t params2;
    uint32_t rnr_nextrecvpsn;
    uint32_t xrcd;
    uint32_t cqn_recv;
    uint64_t db_rec_addr;
    uint32_t qkey;
    uint32_t srqn;
    uint32_t msn;
    uint16_t rq_wqe_counter;
    uint16_t sq_wqe_counter;
    uint32_t reserved3;
    uint16_t rate_limit_params;
    uint8_t  reserved4;
    uint8_t  qos_vport;
    uint32_t param3;
    uint32_t nummmcpeers_basemkey;
    uint8_t  log_page_size;
    uint8_t  reserved5[2];
    uint8_t  mtt_base_addr_h;
    uint32_t mtt_base_addr_l;
    uint32_t reserved6[10];
} mlx4_qp_context_mirror;

/* The byte offset of a pri_path member inside the whole context, from the compiler. */
#define QPC_PRI_PATH_OFF(field) \
    (offsetof(mlx4_qp_context_mirror, pri_path) + \
     offsetof(mlx4_qp_path_mirror, field))

static void test_qpc_path_offsets(void)
{
    printf("\nstruct mlx4_qp_context / mlx4_qp_path: %zu bytes (qp.h:140-236)\n",
           sizeof(mlx4_qp_context_mirror));
    CHECK("sizeof == QP_CONTEXT_SIZE", sizeof(mlx4_qp_context_mirror), QP_CONTEXT_SIZE);
    CHECK("sizeof == 248, the reference's literal", sizeof(mlx4_qp_context_mirror), 248);

    /*
     * The three offsets this file's siblings in src/ already used BEFORE this batch, checked
     * first: they are the mirror's own correctness condition, not the new claim.
     */
    CHECK("pri_path itself",  offsetof(mlx4_qp_context_mirror, pri_path), 0x18);
    CHECK("pri_path.counter_index", QPC_PRI_PATH_OFF(counter_index),
          QP_CONTEXT_PRI_COUNTER_INDEX_OFFSET);
    CHECK("pri_path.ackto",         QPC_PRI_PATH_OFF(ackto), QP_CONTEXT_PRI_ACKTO_OFFSET);
    CHECK("pri_path.sched_queue",   QPC_PRI_PATH_OFF(sched_queue),
          QP_CONTEXT_PRI_SCHED_QUEUE_OFFSET);

    /* ...and the field this batch added. */
    CHECK("pri_path.feup",          QPC_PRI_PATH_OFF(feup), QP_CONTEXT_PRI_FEUP_OFFSET);
    CHECK("pri_path.feup is 0x3a, the literal derived by hand",
          QP_CONTEXT_PRI_FEUP_OFFSET, 0x3a);
    /*
     * It is NOT the neighbours, which is the mistake that matters: fvl_rx is the next byte and
     * sched_queue the one before, so an off-by-one here writes a field the firmware reads for a
     * different purpose and reports success.
     */
    CHECK("feup is not sched_queue", QP_CONTEXT_PRI_FEUP_OFFSET !=
          QP_CONTEXT_PRI_SCHED_QUEUE_OFFSET, 1);
    CHECK("feup is one past sched_queue",
          QP_CONTEXT_PRI_FEUP_OFFSET - QP_CONTEXT_PRI_SCHED_QUEUE_OFFSET, 2);
    CHECK("feup is one before fvl_rx (vlan_index is the byte in between)",
          QPC_PRI_PATH_OFF(fvl_rx) - QP_CONTEXT_PRI_FEUP_OFFSET, 1);
    CHECK("vlan_index sits between sched_queue and feup",
          QPC_PRI_PATH_OFF(vlan_index) - QP_CONTEXT_PRI_SCHED_QUEUE_OFFSET, 1);
    /* And it is in the PRI path, not the alt path: the two are 44 bytes apart (qp.h:141-160). */
    CHECK("alt_path.feup is 44 bytes later",
          (offsetof(mlx4_qp_context_mirror, alt_path) + offsetof(mlx4_qp_path_mirror, feup)) -
          QP_CONTEXT_PRI_FEUP_OFFSET, sizeof(mlx4_qp_path_mirror));

    /*
     * The other QPC offsets src/mlx4_regs.h names, so that a transcription slip in the mirror
     * cannot make the checks above pass by moving everything together.
     */
    CHECK("mtu_msgmax",   offsetof(mlx4_qp_context_mirror, mtu_msgmax),
          QP_CONTEXT_MTU_MSGMAX_OFFSET);
    CHECK("rq_size_stride", offsetof(mlx4_qp_context_mirror, rq_size_stride),
          QP_CONTEXT_RQ_SIZE_STRIDE_OFFSET);
    CHECK("sq_size_stride", offsetof(mlx4_qp_context_mirror, sq_size_stride),
          QP_CONTEXT_SQ_SIZE_STRIDE_OFFSET);
    CHECK("usr_page",     offsetof(mlx4_qp_context_mirror, usr_page),
          QP_CONTEXT_USR_PAGE_OFFSET);
    CHECK("local_qpn",    offsetof(mlx4_qp_context_mirror, local_qpn),
          QP_CONTEXT_LOCAL_QPN_OFFSET);
    CHECK("cqn_send",     offsetof(mlx4_qp_context_mirror, cqn_send),
          QP_CONTEXT_CQN_SEND_OFFSET);
    CHECK("cqn_recv",     offsetof(mlx4_qp_context_mirror, cqn_recv),
          QP_CONTEXT_CQN_RECV_OFFSET);
    CHECK("db_rec_addr",  offsetof(mlx4_qp_context_mirror, db_rec_addr),
          QP_CONTEXT_DB_REC_ADDR_OFFSET);
    CHECK("params2",      offsetof(mlx4_qp_context_mirror, params2),
          QP_CONTEXT_PARAMS2_OFFSET);
    CHECK("param3",       offsetof(mlx4_qp_context_mirror, param3),
          QP_CONTEXT_PARAM3_OFFSET);
    CHECK("log_page_size", offsetof(mlx4_qp_context_mirror, log_page_size),
          QP_CONTEXT_LOG_PAGE_SIZE_OFFSET);
    CHECK("mtt_base_addr_h", offsetof(mlx4_qp_context_mirror, mtt_base_addr_h),
          QP_CONTEXT_MTT_BASE_H_OFFSET);
    CHECK("mtt_base_addr_l", offsetof(mlx4_qp_context_mirror, mtt_base_addr_l),
          QP_CONTEXT_MTT_BASE_L_OFFSET);

    /*
     * The bit itself, against qp.h:186-190 and the one line that sets it.  Written as a literal
     * because that is what the reference writes (`context->pri_path.feup = 1 << 6`,
     * mlx4_en_resources.c:64) -- and because a shifted form here would be the same class of
     * silent mistake the opcode-shift check above exists for.
     */
    CHECK("MLX4_FEUP_FORCE_ETH_UP is 1 << 6", MLX4_FEUP_FORCE_ETH_UP, 1u << 6);
    CHECK("MLX4_FEUP_FORCE_ETH_UP is 0x40", MLX4_FEUP_FORCE_ETH_UP, 0x40);
    CHECK("the priority shift is 3, from `user_prio << 3`",
          MLX4_QP_SCHED_QUEUE_PRIO_SHIFT, 3);
    /*
     * The two live in ONE condition in the reference and now in one condition in the fill, so
     * the check that matters is that they cannot be confused for each other: 0x40 in the
     * priority field would be a priority of 8 (3 bits, so it would truncate) and 8 in feup
     * (1 << 3) is the VLAN bit, a different claim entirely.
     */
    CHECK("the feup bit is NOT the priority shift",
          MLX4_FEUP_FORCE_ETH_UP != (1u << MLX4_QP_SCHED_QUEUE_PRIO_SHIFT), 1);
    CHECK("feup does not touch sched_queue's low bits",
          (MLX4_QP_SCHED_QUEUE_ETH & MLX4_FEUP_FORCE_ETH_UP), 0x00);
}

int main(void)
{
    test_cqe_layout();
    test_cqe_slot();
    test_cqe_opcode();
    test_tx_wqe_layout();
    test_doorbell_and_frame();
    test_sq_rq_header_asymmetry();
    test_qpc_path_offsets();
    test_abi_widths();

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
