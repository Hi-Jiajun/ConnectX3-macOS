/*
 * Part of cx3-mlx4, a macOS IOKit port of the FreeBSD Mellanox driver.
 *
 * Derived from the FreeBSD sys/dev/mlx4 sources vendored under
 * vendor/freebsd-src/; this file cites them by file:line throughout.  Those
 * files are dual-licensed GPL-2.0 / OpenIB.org BSD and this port takes the
 * OpenIB.org BSD branch.  The upstream copyright notices are reproduced in
 * NOTICE and the licence text is in LICENSE, which that branch requires to
 * accompany binary redistribution as well as source.
 *
 * Copyright (c) 2026 Hi-Jiajun and contributors
 */

/*
 * Mellanox ConnectX-3 Pro (PCI 15b3:1007) host register definitions.
 *
 * Provenance: every constant below is carried over from the FreeBSD
 * sys/dev/mlx4 tree vendored at vendor/freebsd-src (41 .c/.h files,
 * dual GPL-2.0 / OpenIB.org BSD).  This file is derived from the OpenIB BSD
 * branch, which permits redistribution in source and binary form provided the
 * copyright notice and licence text are retained.  Line references point at
 * the vendored tree so each value can be re-audited.
 */
#ifndef CX3_MLX4_REGS_H
#define CX3_MLX4_REGS_H

/* ---- BAR0 register blocks ------------------------------------------- */
/* FreeBSD mlx4_core/mlx4.h:71-76 */
#define MLX4_HCR_BASE           0x00080680u  /* mlx4.h:72 command register */
#define MLX4_HCR_SIZE           0x0000001cu  /* mlx4.h:73                  */
#define MLX4_CLR_INT_SIZE       0x00000008u  /* mlx4.h:74                  */

/* ownership bit -- mlx4_core/mlx4_main.c:3059-3060 */
#define MLX4_OWNER_BASE         0x0008069cu
#define MLX4_OWNER_SIZE         0x00000004u

/* HCA reset block -- mlx4_core/mlx4_reset.c:55-63 */
#define MLX4_RESET_BASE         0x000f0000u  /* mlx4_reset.c:55 */
#define MLX4_RESET_SIZE         0x00000400u  /* mlx4_reset.c:56 */
#define MLX4_SEM_OFFSET         0x000003fcu  /* mlx4_reset.c:57 */
#define MLX4_RESET_OFFSET       0x00000010u  /* mlx4_reset.c:58 */
/*
 * mlx4_reset.c:59 is `#define MLX4_RESET_VALUE swab32(1)`, and the value is
 * handed to writel().  On a little-endian host that stores the four bytes
 * 00 00 00 01, i.e. the big-endian number 1.  A native UInt32 store of
 * 0x01000000 on x86_64 produces exactly the same byte sequence.
 */
#define MLX4_RESET_VALUE        0x01000000u

/* ---- HCA command register (HCR) field offsets ---------------------- */
/* FreeBSD mlx4_core/mlx4_cmd.c:101-112 */
#define HCR_IN_PARAM_OFFSET     0x00u   /* u64 BE, 2 dwords */
#define HCR_IN_MODIFIER_OFFSET  0x08u   /* u32 BE, 1 dword  */
#define HCR_OUT_PARAM_OFFSET    0x0cu   /* u64 BE, 2 dwords */
#define HCR_TOKEN_OFFSET        0x14u   /* u32 BE, 1 dword  */
#define HCR_STATUS_OFFSET       0x18u   /* u32 BE, 1 dword  */
#define HCR_OPMOD_SHIFT         12      /* mlx4_cmd.c:108 */
#define HCR_T_BIT               21      /* mlx4_cmd.c:110 */
#define HCR_E_BIT               22      /* mlx4_cmd.c:111 */
#define HCR_GO_BIT              23      /* mlx4_cmd.c:112 */

/* ---- timeouts and sizes ------------------------------------------- */
/*
 * Every "ms" in this section is a real millisecond now (2026-10-12).
 *
 * It was not before, and that is family E in docs/DEFECTS.md: the polls below sleep
 * IOSleep(1) and then count *iterations*, and on this machine one iteration costs about
 * 2.1-2.3 ms -- measured from the kernel log's own timestamps in the 2026-10-12 01:2x
 * hang (5000 iterations = 10.52 s; 60000 iterations = 185.36 s in an earlier one).  So
 * GO_BIT_TIMEOUT_MSECS = 10000 was not 10 s, it was about 22.7 s, and
 * MLX4_CMD_TIME_CLASS_A = 60000 was about 126 s.  The names said milliseconds and the
 * code meant iteration counts, and the hold-the-lock-for-minutes failure mode in
 * docs/LOCK-DESIGN.md rests on exactly that confusion.
 *
 * The waits now run against a deadline computed with clock_get_uptime() (the same real
 * clock abNowNs() already used), so these values mean what they say.  The consequence to
 * be aware of is that every existing budget becomes ~2.1x shorter in wall-clock terms:
 * a command that legitimately took 30 s would now be abandoned at 60 s instead of being
 * waited out for 126 s.  That is the intended trade (docs/LOCK-IMPL.md).
 */
#define GO_BIT_TIMEOUT_MSECS    10000u  /* mlx4_cmd.c:115 -- the reference's real ms,
                                         * kept for the event path only; see
                                         * CX3_CMD_IDLE_WAIT_EVENT_MS below */
#define MLX4_CMD_TIME_CLASS_A   60000u  /* cmd.h:176 */
/*
 * The reference driver allows 60 s for a command.  For M1 the wait happens
 * inside IOService::start(), where a stalled command would block kext loading
 * for that whole minute, so the probe path uses a shorter budget; raise it once
 * the command path is proven.
 */
#define CX3_M1_CMD_TIMEOUT_MS   5000u

/*
 * How long a command may wait *before* it is posted, and how long it may wait for the
 * hardware to complete it.  Both are real milliseconds.
 *
 * Pre-post (the "is the register idle yet" poll):
 *
 *   The reference does not wait at all before a polled command -- poll_sem serialises the
 *   register, so a busy GO bit can only be someone else's (mlx4_cmd.c:456-459).  Here the
 *   same serialisation exists (the transaction lock, then fCmdLock), so the wait only has
 *   to cover the tail of a command that has just returned.  50 ms is that: a small
 *   allowance, not a timeout in which a real command is expected to finish.  Before this
 *   change the same loop waited 10000 iterations, i.e. about 22.7 s *per command*, and
 *   docs/LOCK-DESIGN.md section 1.4 measures those 22.7 s as the main term of the 6 min
 *   07.6 s a single m2dprofile held the transaction lock.
 *
 *   The event path keeps the reference's 10000, because that path can legitimately be
 *   waiting for a completion EQE rather than for the register to clear.
 *
 * Post-post (the completion budget): the command budgets below are what the caller asks
 * for, and each is now a wall-clock deadline rather than an iteration count.
 */
#define CX3_CMD_IDLE_WAIT_MS        50u      /* polled: idle wait before posting */
#define CX3_CMD_IDLE_WAIT_EVENT_MS  10000u   /* event: == GO_BIT_TIMEOUT_MSECS, real ms */
#define CX3_CMD_BUDGET_ICM_MS       5000u    /* ICM/table teardown: the intent behind
                                              * CX3_M1_CMD_TIMEOUT_MS, as real ms */
#define CX3_CMD_BUDGET_FW_MS        60000u   /* firmware/init commands: the reference's
                                              * class A/B/C value (cmd.h:176-178) */
#define MLX4_MAILBOX_SIZE       4096u   /* cmd.h:188 */
#define CMD_POLL_TOKEN          0xffffu /* mlx4_cmd.c:54 */

#define MLX4_SEM_TIMEOUT_MS     10000u  /* 10 * HZ, mlx4_reset.c:61 */
#define MLX4_RESET_TIMEOUT_MS   2000u   /* 2 * HZ,  mlx4_reset.c:62 */

/*
 * Sentinels for "there is no firmware status to report".
 *
 * A firmware status of 0 means "the firmware said ok", so a command that never
 * completed must never be reported as 0.  The first M2b build did exactly that:
 * cmdBox() returned early on timeout without writing outFwStatus, its caller had
 * initialised the word to 0, and so a RUN_FW that never completed printed
 *
 *     RUN_FW : fw status 0x0 ok
 *
 * Two independent source reviews of M2b found this; the RUN_FW "success" that
 * M2b reported was a timeout wearing the success value.
 */
#define CX3_FWSTATUS_NOT_POSTED 0xfffffffeu  /* never written to the HCR at all */
#define CX3_FWSTATUS_TIMEOUT    0xfffffffdu  /* posted, never completed */
#define CX3_FWSTATUS_NO_DEVICE  0xfffffffcu  /* no HCR handle */
/*
 * A fourth state, and the reason it exists rather than reusing CX3_FWSTATUS_TIMEOUT.
 *
 * A command that timed out was *posted*: its bytes may have been read, a reply may still
 * arrive later, and the device is in an unknown state.  A command refused because the
 * mailbox is already poisoned was *not* posted, is not in flight, and the register was not
 * touched at all.  Those are different facts and a caller that cannot tell them apart
 * cannot decide what to do next -- the same reason CX3_FWSTATUS_TIMEOUT is not 0
 * (docs/DEFECTS.md B1).  When this value comes back, the cure is an HCA reset or an FLR:
 * see cmdPoison() in src/Mlx4Hw.cpp.
 */
#define CX3_FWSTATUS_WEDGED     0xfffffffbu  /* not sent: the mailbox is poisoned */

/* ---- ICM page size ------------------------------------------------- */
/* FreeBSD mlx4_core/icm.h:46-47.  The firmware's ICM granularity. */
#define MLX4_ICM_PAGE_SHIFT   12
#define MLX4_ICM_PAGE_SIZE    (1u << MLX4_ICM_PAGE_SHIFT)

/* MLX4_MAILBOX_SIZE / 16: how many (address, alignment) pairs fit, and the
 * batch size mlx4_map_cmd uses.  FreeBSD mlx4_fw.c:1552. */
#define MLX4_MAP_CMD_MAX_ENTRIES  (MLX4_MAILBOX_SIZE / 16)

/* MOD_STAT_CFG input-mailbox byte offsets.  FreeBSD mlx4_fw.c:185-186. */
#define MOD_STAT_CFG_PG_SZ_M_OFFSET  0x02u
#define MOD_STAT_CFG_PG_SZ_OFFSET    0x03u

/* ---- command opcodes ---------------------------------------------- */
/* FreeBSD cmd.h:43-60 */
#define MLX4_CMD_QUERY_DEV_CAP  0x3u
#define MLX4_CMD_QUERY_FW       0x4u
#define MLX4_CMD_INIT_HCA       0x7u     /* M2d */
#define MLX4_CMD_CLOSE_HCA      0x8u     /* cmd.h:56, non-panic HCA close */
#define MLX4_CMD_MOD_STAT_CFG   0x34u    /* cmd.h:48 */
#define MLX4_CMD_SET_ICM_SIZE   0xffdu   /* cmd.h:71, M2d */
#define MLX4_CMD_MAP_ICM        0xffau   /* cmd.h:67, M2d */
#define MLX4_CMD_MAP_ICM_AUX    0xffcu   /* cmd.h:69, M2d */
#define MLX4_CMD_UNMAP_ICM_AUX  0xffbu   /* cmd.h:70 */
#define MLX4_CMD_MAP_FA         0xfffu   /* cmd.h:45 */
#define MLX4_CMD_UNMAP_FA       0xffeu   /* cmd.h:46 */
#define MLX4_CMD_RUN_FW         0xff6u   /* cmd.h:47 */

/* ---- QUERY_FW outbox layout --------------------------------------- */
/* FreeBSD mlx4_core/mlx4_fw.c:1611-1628 */
#define QUERY_FW_OUT_SIZE          0x100u
#define QUERY_FW_VER_OFFSET        0x00u  /* u64 BE */
#define QUERY_FW_PPF_ID            0x09u  /* u8     */
#define QUERY_FW_CMD_IF_REV_OFFSET 0x0au  /* u16 BE */
#define QUERY_FW_MAX_CMD_OFFSET    0x0fu  /* u8, log2 */
#define QUERY_FW_ERR_START_OFFSET  0x30u  /* u64 BE */
#define QUERY_FW_ERR_SIZE_OFFSET   0x38u  /* u32 BE */
#define QUERY_FW_ERR_BAR_OFFSET    0x3cu  /* u32 BE */
/*
 * The FW area size, in 4 KiB pages (u16 BE) -- at offset 0x00, in front of the
 * version field, which the reference reads as the same u64.
 *
 * This port first read it from 0x10, on a note that said "mainline Linux uses
 * 0x10".  That note was wrong.  Mainline Linux has
 *
 *     drivers/net/ethernet/mellanox/mlx4/fw.c:1638
 *         #define QUERY_FW_SIZE_OFFSET  0x00
 *     drivers/net/ethernet/mellanox/mlx4/fw.c:1709
 *         MLX4_GET(fw->fw_pages, outbox, QUERY_FW_SIZE_OFFSET);
 *
 * i.e. byte for byte what FreeBSD has (mlx4_core/mlx4_fw.c:1620, :1691).
 *
 * The reference's own version decode is the proof that 0x00 is a separate
 * field and not the top of the version:
 *
 *     caps = (fw_ver & 0xffff00000000) | ((fw_ver & 0xffff0000) >> 16)
 *                                      | ((fw_ver & 0xffff) << 16);
 *
 * bits 63:48 -- the two bytes at 0x00 -- are dropped entirely, so the version
 * proper is the six bytes at 0x02..0x07.
 *
 * Measured on this card, M1's own outbox dump (`cx3ctl queryfw`):
 *
 *     outbox     06060002 1388002a 00000003 00000004
 *     BE16 @0x00 = 0x0606 = 1542 pages = 6.0 MiB      <- the FW area
 *     bytes 0x02..0x07 decode to version 2.42.5000    <- the version
 *     0x09 ppf id = 0, 0x0a cmd if rev = 3, 0x0f log2 max cmd = 4 -> 16
 *
 * Every field the reference names lands where the reference says it does, and
 * the value this port used to take from 0x10 (5123 pages = 20.0 MiB) is not one
 * of them.  Handing the firmware 3.3x the area it asked for is a plausible
 * cause of the M2b RUN_FW hang, which is why this is the first thing to retest.
 */
#define QUERY_FW_SIZE_OFFSET       0x00u  /* u16 BE, 4 KiB pages */
/* Kept only so a diagnostic can print both candidates side by side. */
#define QUERY_FW_SIZE_OFFSET_WRONG 0x10u
/* Where comm and clock vectors are, needed later; recorded now because they
 * come from the same response and cost nothing to keep. */
#define QUERY_FW_CLR_INT_BASE_OFFSET 0x20u
#define QUERY_FW_CLR_INT_BAR_OFFSET  0x28u
#define QUERY_FW_COMM_BASE_OFFSET    0x40u
#define QUERY_FW_COMM_BAR_OFFSET     0x48u

/* mlx4_fw.c:48-50 */
#define MLX4_COMMAND_INTERFACE_MIN_REV   2
#define MLX4_COMMAND_INTERFACE_MAX_REV   3

/* ---- QUERY_DEV_CAP outbox layout -------------------------------- */
/* FreeBSD mlx4_core/mlx4_fw.c:735-785.  Byte offsets; sizes vary, see the
 * accessors in Mlx4Hw::queryDevCap for which is 1, 2, 4 or 8 bytes wide. */
#define QUERY_DEV_CAP_MAX_SRQ_SZ_OFFSET   0x10u  /* u8, log2 */
#define QUERY_DEV_CAP_MAX_QP_SZ_OFFSET    0x11u  /* u8, log2 */
#define QUERY_DEV_CAP_RSVD_QP_OFFSET      0x12u  /* u8 */
#define QUERY_DEV_CAP_MAX_QP_OFFSET       0x13u  /* u8, log2 */
#define QUERY_DEV_CAP_RSVD_SRQ_OFFSET     0x14u  /* u8 */
#define QUERY_DEV_CAP_MAX_SRQ_OFFSET      0x15u  /* u8, log2 */
#define QUERY_DEV_CAP_MAX_CQ_SZ_OFFSET    0x19u  /* u8, log2 */
#define QUERY_DEV_CAP_RSVD_CQ_OFFSET      0x1au  /* u8 */
#define QUERY_DEV_CAP_MAX_CQ_OFFSET       0x1bu  /* u8, log2 */
#define QUERY_DEV_CAP_MAX_MPT_OFFSET      0x1du  /* u8, log2 */
#define QUERY_DEV_CAP_RSVD_EQ_OFFSET      0x1eu  /* u8 */
#define QUERY_DEV_CAP_MAX_EQ_OFFSET       0x1fu  /* u8, log2 */
#define QUERY_DEV_CAP_RSVD_MTT_OFFSET     0x20u  /* u8 */
#define QUERY_DEV_CAP_RSVD_MRW_OFFSET     0x22u  /* u8 */
#define QUERY_DEV_CAP_MAX_MTT_SEG_OFFSET  0x23u  /* raw u8; reference names offset only */
#define QUERY_DEV_CAP_NUM_SYS_EQ_OFFSET   0x26u  /* u16 BE, low 12 bits */
#define QUERY_DEV_CAP_MAX_GSO_OFFSET      0x2du  /* u8 */
#define QUERY_DEV_CAP_MTU_WIDTH_OFFSET    0x36u  /* u8 */
#define QUERY_DEV_CAP_VL_PORT_OFFSET      0x37u  /* u8, low 4 bits = num ports */
#define QUERY_DEV_CAP_MAX_MSG_SZ_OFFSET   0x38u  /* u8 */
#define QUERY_DEV_CAP_MAX_GID_OFFSET      0x3bu  /* u8 */
#define QUERY_DEV_CAP_EXT_FLAGS_OFFSET    0x40u  /* u32 BE */
#define QUERY_DEV_CAP_FLAGS_OFFSET        0x44u  /* u64 BE */
#define QUERY_DEV_CAP_RSVD_UAR_OFFSET     0x48u  /* u8 */
#define QUERY_DEV_CAP_UAR_SZ_OFFSET       0x49u  /* u8, log2 pages */
#define QUERY_DEV_CAP_PAGE_SZ_OFFSET      0x4bu  /* u32 BE, mask */

/* ---- PCI identifiers --------------------------------------------- */
#define MLX4_PCI_VENDOR_MELLANOX   0x15b3
#define MLX4_PCI_DEVICE_CX3_PRO    0x1007

/* ==================================================================== */
/* M3, restart 1: event queues, and the event-mode command path         */
/* ==================================================================== */

/* Opcodes.  FreeBSD cmd.h:65,90-93,135. */
#define MLX4_CMD_MAP_EQ         0x12u
#define MLX4_CMD_SW2HW_EQ       0x13u
#define MLX4_CMD_HW2SW_EQ       0x14u
/*
 * Note the asymmetry, from mlx4_eq.c:895-906: SW2HW_EQ is sent with
 * opmodifier 0 and its inbox is the context, while HW2SW_EQ is sent with
 * opmodifier 1 and no inbox at all.  Teardown will need that; nothing here sends
 * HW2SW_EQ yet.
 */
#define MLX4_CMD_QUERY_EQ       0x15u
#define MLX4_CMD_NOP            0x31u
#define MLX4_CMD_QUERY_FUNC     0x56u
#define MLX4_CMD_QUERY_ADAPTER  0x6u    /* cmd.h:54 */

/* ==================================================================== */
/* R3: the port                                                         */
/* ==================================================================== */

/*
 * Port command opcodes, straight from FreeBSD cmd.h:57-63.
 *
 * INIT_PORT's two shapes are worth naming here because the difference is a whole
 * command interface generation and not a detail: with cmd_if_rev < 3 the driver
 * sends a 256-byte mailbox (mlx4_fw.c:2267-2299), with revision 3 and above it
 * sends nothing and in_param is 0 (mlx4_fw.c:2299-2308).  This card reports
 * revision 3 (measured), so only the second shape is implemented -- guessing at
 * the first would be guessing at a driver generation this card is not.
 */
#define MLX4_CMD_INIT_PORT      0x9u
#define MLX4_CMD_CLOSE_PORT     0xau
#define MLX4_CMD_SET_PORT       0xcu
/*
 * CLOSE_PORT -- the other half of the port lifecycle, and the prerequisite for changing anything
 * in the port's configuration (the MTU above all): the reference's change_mtu stops and re-opens
 * the port (mlx4_en/mlx4_en_netdev.c:1839-1840), and CLOSE_PORT is what "stop" means at the
 * device.
 *
 * `mlx4_cmd(dev, 0, port, 0, MLX4_CMD_CLOSE_PORT, MLX4_CMD_TIME_CLASS_A, ...)`
 * (mlx4_core/mlx4_fw.c:2358-2363): no parameter, no mailbox, the port in in_modifier, op_modifier
 * 0, and TIME CLASS A -- which is what this port's other port-level commands use, not the B that
 * the MAC table command takes.
 */
#define MLX4_CMD_CLOSE_PORT     0xau    /* cmd.h:58 */
#define MLX4_CMD_QUERY_PORT     0x43u
#define MLX4_CMD_SENSE_PORT     0x4du
#define MLX4_CMD_QUERY_HCA      0xbu    /* cmd.h:59 -- outbox, in_modifier 0 */

/* ==================================================================== */
/* R4: protection domains and memory regions                            */
/* ==================================================================== */

/* cmd.h:82-84.  SW2HW_MPT takes the MPT entry in a mailbox (in_param) and the
 * hardware index in in_modifier; HW2SW_MPT hands it back the same way. */
#define MLX4_CMD_SW2HW_MPT      0xdu
#define MLX4_CMD_QUERY_MPT      0xeu
#define MLX4_CMD_HW2SW_MPT      0xfu

/* cmd.h:96-97.  SW2HW_CQ takes the CQ context in a mailbox, the CQN in in_modifier. */
#define MLX4_CMD_SW2HW_CQ       0x16u
#define MLX4_CMD_HW2SW_CQ       0x17u

/*
 * cmd.h:108-118.  A QP is not handed over with one command the way a CQ is: it is walked
 * through three state transitions, each of which carries the whole context.  The two extra
 * opcodes are here because they are the ones the verification needs -- 2RST_QP to clear up
 * after a failed experiment, and QUERY_QP, which is the only way to find out whether the
 * state the driver asked for is the state the hardware is in.  "RST2INIT returned 0" and
 * "the QP is in INIT" are different claims (docs/R4-STAGE4-PLAN.md section 7).
 */
#define MLX4_CMD_RST2INIT_QP    0x19u
#define MLX4_CMD_INIT2RTR_QP    0x1au
#define MLX4_CMD_RTR2RTS_QP     0x1bu
#define MLX4_CMD_2RST_QP        0x21u
#define MLX4_CMD_QUERY_QP       0x22u

/*
 * struct mlx4_qp_context (qp.h:196-236), 248 bytes.  Its layout is NOT the CQ context's:
 * QP context lives at mailbox offset +8 and the CQ context at +0 (mlx4_qp.c:177-178 vs
 * mlx4_cq.c:151).  That is the first thing this stage can get wrong, and nothing fails to
 * warn about it -- the command simply returns BAD_PARAM (0x3) or, worse, accepts a context
 * read out of the wrong bytes.
 *
 * The offsets are written out member by member in the struct's own declaration order, the
 * way the EQ and CQ contexts are, so that a member inserted in the middle cannot silently
 * move the ones after it.  Two of them are reached through a nested struct in the
 * reference -- pri_path starts at 0x18 (qp.h:140-160) and its counter_index, ackto and
 * sched_queue are at +4, +8 and +32 inside it.
 */
#define QP_CONTEXT_SIZE                     0xf8u   /* 248 bytes */
#define QP_CONTEXT_FLAGS_OFFSET             0x00u   /* BE32: state in [31:28] */
#define QP_CONTEXT_PD_OFFSET                0x04u   /* BE32 */
#define QP_CONTEXT_MTU_MSGMAX_OFFSET        0x08u   /* u8 */
#define QP_CONTEXT_RQ_SIZE_STRIDE_OFFSET    0x09u   /* u8 */
#define QP_CONTEXT_SQ_SIZE_STRIDE_OFFSET    0x0au   /* u8 */
#define QP_CONTEXT_USR_PAGE_OFFSET          0x0cu   /* BE32: the UAR index */
#define QP_CONTEXT_LOCAL_QPN_OFFSET         0x10u   /* BE32 */
#define QP_CONTEXT_REMOTE_QPN_OFFSET        0x14u   /* BE32 */
#define QP_CONTEXT_PRI_COUNTER_INDEX_OFFSET 0x1cu   /* u8  (pri_path + 4) */
#define QP_CONTEXT_PRI_ACKTO_OFFSET         0x20u   /* u8  (pri_path + 8) */
#define QP_CONTEXT_PRI_SCHED_QUEUE_OFFSET   0x38u   /* u8  (pri_path + 32) */
/*
 * pri_path.feup, at 0x3a -- the byte between sched_queue and fvl_rx, which is the third and
 * last place in the QP context where the reference's SHARED fill produces a different value
 * for a send queue than for a receive queue.  See MLX4_FEUP_FORCE_ETH_UP below.
 *
 * Derivation of the offset, because this one is not quotable from a single line: struct
 * mlx4_qp_context (qp.h:196-236) puts pri_path at 0x18 (its first member after flags/pd/the
 * three size-stride bytes/rlkey_roce_mode/usr_page/local_qpn/remote_qpn), and pri_path
 * (qp.h:140-160) reaches feup after fl, vlan_control, disable_pkey_check, pkey_index,
 * counter_index, grh_mylmc, rlid(BE16), ackto, mgid_index, static_rate, hop_limit,
 * tclass_flowlabel(BE32), rgid[16], sched_queue and vlan_index -- 34 bytes, so 0x18 + 0x22 =
 * 0x3a.  This is NOT a hand-transcription: the same number is computed by the compiler from a
 * mirror of the reference's declaration, and asserted against this constant at build time by
 * tools/test-cqe-offsets.c (the mirror also re-derives ackto 0x20, counter_index 0x1c and
 * sched_queue 0x38, the three offsets this file already relied on, which is what makes the
 * mirror trustworthy rather than a fourth copy).
 */
#define QP_CONTEXT_PRI_FEUP_OFFSET          0x3au   /* u8  (pri_path + 34) */
#define QP_CONTEXT_CQN_SEND_OFFSET          0x7cu   /* BE32 */
/*
 * params2, at 0x90 (qp.h:225).  Named because of one bit in it: MLX4_QP_BIT_FPP (qp.h:103)
 * is cleared for every state except RTR (mlx4_qp.c:915-916).  This port's context is
 * zero from the start, so the clear is a no-op here -- and that is worth saying rather
 * than leaving a reader to wonder whether the reference's line was dropped.
 */
#define QP_CONTEXT_PARAMS2_OFFSET           0x90u   /* BE32 */
#define QP_CONTEXT_CQN_RECV_OFFSET          0x9cu   /* BE32 */
#define QP_CONTEXT_DB_REC_ADDR_OFFSET       0xa0u   /* BE64: the record's DMA address */
#define QP_CONTEXT_RQ_WQE_COUNTER_OFFSET    0xb4u   /* BE16 */
#define QP_CONTEXT_SQ_WQE_COUNTER_OFFSET    0xb6u   /* BE16 */
#define QP_CONTEXT_PARAM3_OFFSET            0xc0u   /* BE32 */
#define QP_CONTEXT_LOG_PAGE_SIZE_OFFSET     0xc8u   /* u8 */
#define QP_CONTEXT_MTT_BASE_H_OFFSET        0xcbu   /* u8 */
#define QP_CONTEXT_MTT_BASE_L_OFFSET        0xccu   /* BE32 */
/*
 * The RSS indirection overlay: struct mlx4_rss_context, which lives INSIDE pri_path
 * inside the QP context (qp.h:117-139) and is reached the way the reference reaches it
 * (mlx4_en_rx.c:1067-1069):
 *
 *     rss_context = (u8 *)&context + offsetof(struct mlx4_qp_context, pri_path)
 *                                + MLX4_RSS_OFFSET_IN_QPC_PRI_PATH
 *
 * pri_path starts at 0x18 in this context (qp.h:140, and this file already depends on
 * that for counter_index at 0x1c) and MLX4_RSS_OFFSET_IN_QPC_PRI_PATH is 0x24
 * (qp.h:117), so the overlay begins at QPC + 0x3c.  That is a field-by-field overlay on
 * top of the context that RST2INIT carries, not a separate command: the RSS context
 * travels inside the same 248 bytes.
 *
 * The struct's members and widths are qp.h:128-137: base_qpn (BE32), default_qpn (BE32),
 * reserved (u16), hash_fn (u8), flags (u8), rss_key[10] (BE32 x 10 = 40 bytes),
 * base_qpn_udp (BE32).
 */
#define QP_CONTEXT_PRI_PATH_OFFSET          0x18u   /* qp.h:140 */
#define MLX4_RSS_OFFSET_IN_QPC_PRI_PATH     0x24u   /* qp.h:117 */
#define QP_CONTEXT_RSS_BASE_QPN_OFFSET \
    (QP_CONTEXT_PRI_PATH_OFFSET + MLX4_RSS_OFFSET_IN_QPC_PRI_PATH)          /* 0x3c */
#define QP_CONTEXT_RSS_DEFAULT_QPN_OFFSET   (QP_CONTEXT_RSS_BASE_QPN_OFFSET + 0x04u) /* 0x40 */
#define QP_CONTEXT_RSS_RESERVED_OFFSET      (QP_CONTEXT_RSS_BASE_QPN_OFFSET + 0x08u) /* 0x44 */
#define QP_CONTEXT_RSS_HASH_FN_OFFSET       (QP_CONTEXT_RSS_BASE_QPN_OFFSET + 0x0au) /* 0x46 */
#define QP_CONTEXT_RSS_FLAGS_OFFSET         (QP_CONTEXT_RSS_BASE_QPN_OFFSET + 0x0bu) /* 0x47 */
#define QP_CONTEXT_RSS_KEY_OFFSET           (QP_CONTEXT_RSS_BASE_QPN_OFFSET + 0x0cu) /* 0x48 */
#define QP_CONTEXT_RSS_BASE_QPN_UDP_OFFSET  (QP_CONTEXT_RSS_BASE_QPN_OFFSET + 0x34u) /* 0x70 */

/*
 * The two numbers that describe the indirection QP's own shape, both from qp.h:107-126.
 *
 * MLX4_RSS_QPC_FLAG_OFFSET (13) is the bit in the context's flags word that says "this QP
 * is an RSS indirection QP": `flags = 7 << 16 | rss << MLX4_RSS_QPC_FLAG_OFFSET`
 * (mlx4_en_resources.c:49).  MLX4_RSS_HASH_TOP is the Toeplitz hash (XOR is the other
 * choice, 0, and the reference does not use it here: mlx4_en_rx.c:1074).
 *
 * MLX4_RSS_QPN_HIGH_SHIFT is the encoding of the ring count in base_qpn's top byte:
 * `cpu_to_be32(ilog2(rss_rings) << 24 | base_qpn)` (mlx4_en_rx.c:1070-1071).  With one
 * ring ilog2(1) is 0, which is why this stage cannot verify that encoding -- see
 * docs/R4-STAGE5-PLAN.md section 5, decision (3).
 */
#define MLX4_RSS_QPC_FLAG_OFFSET            13u     /* qp.h:125 */
#define MLX4_RSS_HASH_TOP                   1u      /* qp.h:109 */
#define MLX4_RSS_QPN_HIGH_SHIFT             24u

/*
 * The RSS input-flag byte (mlx4_en_rx.c:1005-1013): which protocols are hashed.  The
 * reference's mlx4_en_get_rss_mask sets IPV4 | TCP_IPV4 | IPV6 | TCP_IPV6 and adds the two
 * UDP bits only when the profile asks for UDP RSS.  The values are qp.h:110-115.
 */
#define MLX4_RSS_UDP_IPV6                    (1u << 0)
#define MLX4_RSS_UDP_IPV4                    (1u << 1)
#define MLX4_RSS_TCP_IPV6                    (1u << 2)
#define MLX4_RSS_IPV6                        (1u << 3)
#define MLX4_RSS_TCP_IPV4                    (1u << 4)
#define MLX4_RSS_IPV4                        (1u << 5)
/* MLX4_EN_RSS_KEY_SIZE (qp.h:127) is 40 bytes, i.e. ten BE32 words. */
#define MLX4_EN_RSS_KEY_SIZE                40u
/*
 * Where the context sits inside the command mailbox, and what the optpar word at +0 is.
 * mlx4_qp.c:177-178 writes optpar at +0 and memcpy's the context to +8; the same +8 is
 * where QUERY_QP's answer lands (:892).  optpar is this port's 0 -- it is the bitmask of
 * "which optional fields are present", and 0 means "the mandatory context only", which is
 * exactly what these three commands send.
 */
#define QP_CONTEXT_MAILBOX_OFFSET           8u
#define QP_OPTPAR_DEFAULT                   0u

/*
 * flags[31:28] is the QP's state, and the values are the ones the command opcodes are
 * named after (qp.h:67-77).  They are also literally what the reference writes
 * (mlx4_qp.c:912-914 masks the nibble and ORs the new state in), so the three transitions
 * are "same context, four bits different".
 */
#define MLX4_QP_STATE_RST       0u
#define MLX4_QP_STATE_INIT      1u
#define MLX4_QP_STATE_RTR       2u
#define MLX4_QP_STATE_RTS       3u
#define MLX4_QP_STATE_NIBBLE_SHIFT 28u

/*
 * The non-state bits of the context's flags word: 7 << 16 (mlx4_en_resources.c:49).  The
 * RSS bit (MLX4_RSS_QPC_FLAG_OFFSET, :49's `rss << ...`) is not set -- 0 means the
 * firmware applies the default for our single-value `port`.  One field on this card can
 * hold 1.  BIT_FPP (qp.h:103) is a send-side bit the reference clears for every state
 * except RTR (mlx4_qp.c:915-916); ours is zero from the start and is cleared explicitly
 * anyway, so the field is written as the reference writes it.
 */
#define MLX4_QP_FLAGS_FIXED                 (7u << 16)
#define MLX4_QP_BIT_FPP                     (1u << 3)

/*
 * The scheduler queue a software Ethernet QP uses: 0x83 | (port - 1) << 6
 * (mlx4_en_resources.c:61).  The low bits select the "no VLAN, no priority" shape of the
 * Ethernet schedule queue, and the shift picks the port.  A wrong value here does not fail;
 * it makes the QP belong to the other port or to no traffic class, and the symptom is
 * frames that never arrive.
 */
#define MLX4_QP_SCHED_QUEUE_ETH             0x83u
#define MLX4_QP_SCHED_QUEUE_PORT_SHIFT      6u
/*
 * The priority the reference ORs into sched_queue when its fill is given a non-negative
 * user_prio: `context->pri_path.sched_queue |= user_prio << 3` (mlx4_en_resources.c:63).
 * It sits in the SAME condition as MLX4_FEUP_FORCE_ETH_UP, which is why the two are written
 * from one place here.  The transmit ring the reference activates passes `i /
 * num_tx_rings_p_up` (mlx4_en_netdev.c:1362-1363), i.e. 0 for the first ring on the port,
 * so the priority field is 0 on this driver's send queue and the byte stays 0x83.
 */
#define MLX4_QP_SCHED_QUEUE_PRIO_SHIFT      3u
/*
 * The port this stage's QP belongs to.
 *
 * Not a device-sourced value and so not a read-back candidate: it is the port the
 * verification chain activates (`m3portup 1`), i.e. a choice this driver makes, and it is
 * named once here because it appears in two places (the context's sched_queue and, later, the
 * steering that has to point at this QP).  The R3 selectors take a port as input because the
 * reference has a port parameter there; this stage does not, and pinning it to a named
 * constant is what keeps "the QP is on port 1" from being a literal in three files.
 */
#define MLX4_QP_PORT_ETH                    1u

/*
 * rq_size_stride / sq_size_stride: ilog2(entries) << 3 | (ilog2(stride) - 4)
 * (mlx4_en_resources.c:53-57).  The stride is the WQE size -- 16 bytes for both this
 * port's RQ (one data segment, qp.h:440-444) and the reference's RX ring
 * (`sizeof(struct mlx4_en_rx_desc)`), so ilog2(16) - 4 == 0 and the low nibble is zero.
 * The dummy SQ is 64 bytes because TXBB_SIZE is 64 (en.h:84).
 *
 * mtu_msgmax is 0xff for Ethernet (mlx4_en_resources.c:51): 255 * 16 = 4080 bytes, which
 * is the largest frame this card will hand over.
 */
#define MLX4_QP_RQ_STRIDE_SHIFT             3u    /* ilog2(entries) << 3 */
#define MLX4_QP_STRIDE_SUB                  4u    /* - 4, from ilog2(stride) */
#define MLX4_QP_WQE_SIZE_RQ                 16u   /* one data segment */
#define MLX4_QP_WQE_SIZE_DUMMY_SQ           64u   /* TXBB_SIZE, en.h:84 */
#define MLX4_QP_MTU_MSGMAX_ETH              0xffu

/*
 * param3's FCS_KEEP bit (mlx4_en_rx.c:949-953):
 *
 *     if (dev->caps.flags & MLX4_DEV_CAP_FLAG_FCS_KEEP)
 *             context->param3 |= cpu_to_be32(1 << 29);
 *
 * and MLX4_DEV_CAP_FLAG_FCS_KEEP is bit 34 of the 64-bit flags word (device.h:177).
 * Setting it asks the firmware to keep the 4-byte Ethernet FCS on received frames; not
 * setting it changes every frame's length by 4 bytes, which is exactly the kind of silent
 * difference stage 9 would spend a day on.  `caps.flags` in this port is already
 * `ext_flags << 32 | flags` (mlx4_pure.h:369-370), so the bit is directly readable.
 */
#define MLX4_DEV_CAP_FLAG_FCS_KEEP          (1ull << 34)
#define MLX4_QP_PARAM3_FCS_KEEP             (1u << 29)

/*
 * pri_path.feup's FORCE_ETH_UP bit -- the one field where the reference's SHARED fill gives a
 * SEND queue a value it does not give a receive queue, and the one this port did not copy.
 *
 *     mlx4_en_resources.c:62-65 -- the shared fill's only assignment to this field
 *         if (user_prio >= 0) {
 *                 context->pri_path.sched_queue |= user_prio << 3;
 *                 context->pri_path.feup = 1 << 6;      <- MLX4_FEUP_FORCE_ETH_UP (qp.h:187)
 *         }
 *
 * and the two callers pass opposite signs, which is what makes this asymmetric:
 *
 *     TRANSMIT  mlx4_en_tx.c:233-234 passes `user_prio` down, and its caller
 *               (mlx4_en_netdev.c:1362-1363) passes `i / priv->num_tx_rings_p_up`, which is
 *               a non-negative index and therefore >= 0 -- so a TX ring gets 0x40 here;
 *     RECEIVE   mlx4_en_rx.c:944-945 passes -1, so the condition is false and the field
 *               stays at the value the memset left (0x00).
 *
 * The port's RX QP copied the -1 instantiation faithfully (feup 0) and that queue WORKS on
 * this card (stage 9 measured 51..60 completions on a live LAN), while the port's TX QP copied
 * the same shared fill the same way -- and a send queue is exactly the instantiation that needs
 * the bit.  That is the same shape as docs/DEFECTS.md E13 (one shared shape for two queues
 * whose hardware conventions differ), one field up: not a memory offset but a context byte.
 *
 * Where the bit is set outside mlx4_en, for the same reason (a software-made Ethernet queue
 * that will SEND):
 *
 *     drivers/infiniband/hw/mlx4/qp.c:1805   path->feup |= MLX4_FEUP_FORCE_ETH_UP;   (RoCE)
 *     mlx4_core/mlx4_resource_tracker.c:821  qpc->pri_path.feup |= ... (SDN/QoS update path)
 *     mlx4_core/mlx4_resource_tracker.c:5344  same, on the UPDATE_QP path
 *
 * so every implementation in reach that hand-builds an Ethernet QP context which has to
 * transmit sets it.  DPDK is not a counter-example: it does not build a QP context at all --
 * it goes through verbs (mlx4_txq.c:431 create_qp, :442/:455/:467 ibv_modify_qp), and the
 * context is filled by the kernel driver above.
 *
 * WHAT IS NOT KNOWN, stated rather than papered over: the word "force Eth UP" is the comment in
 * qp.h:187 and nothing more.  CX-3's PRM is NDA and the public CX-4 PRM does not document this
 * field, so what the firmware does with the bit -- force the port's Ethernet state, or the QP's
 * own vport state -- is not established here.  The port is already up and this card receives
 * fine, so "the link is down" is NOT the hypothesis; the hypothesis is that a send queue whose
 * context does not claim Ethernet-up never has its descriptors executed, which fits the
 * symptom exactly (doorbell rung, no completion, no error completion, no frame on the wire).
 *
 * THE FALSIFICATION CONDITION, written before the run so the run can settle it: this is a
 * one-byte change to the context and it is judged by stage 9's own six criteria.  If a TX
 * completion arrives and a frame appears on the wire, the bit was the cause.  If the WQE still
 * goes out with the doorbell rung and NOTHING happens -- no completion, no error completion, no
 * frame -- then this field is not the gate, and the strongest remaining candidate is the
 * TRANSPORT the reference uses and this port does not: mlx4_en on CX-3 has BlueFlame
 * (bf_reg_size > 0 is what mlx4_bf_alloc needs, mlx4_en_tx.c:157-166) and so takes the
 * BlueFlame branch unconditionally (:890), i.e. the shipper may never exercise the plain
 * memory+doorbell path this port walks.  `db_rec_addr` (0xa0, the record) is NOT a candidate in
 * either case: four implementations (mlx4_en, rdma-core, DPDK, and the shipping driver) write
 * the receive record and none writes a send one.
 */
#define MLX4_FEUP_FORCE_ETH_UP              (1u << 6)

/*
 * VEP_MC_STEER, bit 42 of the same 64-bit flags word (device.h:182), and the one place it is
 * read: SET_PORT's qpn_calc sub-command picks its multicast form from it.
 *
 *     mlx4_port.c:1585-1586
 *         u32 m_promisc = (dev->caps.flags & MLX4_DEV_CAP_FLAG_VEP_MC_STEER) ?
 *                 MCAST_DIRECT : MCAST_DEFAULT;
 *
 * Measured on this card: `caps.flags = ext_flags 0xeeff1f9d << 32 | flags 0xf677fbef`, so bit
 * 42 is SET and the reference's branch takes MCAST_DIRECT (1), which is what this port sends
 * (docs/R4-STAGE7-PLAN.md section 3).  This is the third capability bit that was already in
 * the two dwords this port had read all along -- log_max_macs and FCS_KEEP are the other two
 * -- which is why the code reads it out of fDevCap rather than deciding it here.
 */
#define MLX4_DEV_CAP_FLAG_VEP_MC_STEER      (1ull << 42)

/*
 * struct mlx4_cq_context (mlx4.h:344-363), 128 bytes -- which is this card's
 * cqc_entry_sz, checked against the profile's own log ("cqc 0x100170000 log 8" with
 * "entry sizes ... cq 128") rather than assumed.
 *
 * Note what this is NOT: 128 is the size of the CQ CONTEXT entry, not of a CQE.  The
 * CQE is 64 bytes, because INIT_HCA's eqe/cqe word 0x60000000 sets both 64-byte flags
 * (mlx4_fw.c:1917-1931).  Reading cqc_entry_sz as the CQE size would put every CQ
 * descriptor at the wrong stride.
 */
#define CQ_CONTEXT_FLAGS_OFFSET          0x00u   /* BE32 */
#define CQ_CONTEXT_LOGSIZE_USRPAGE_OFFSET 0x0cu  /* BE32: logsize << 24 | hw uar index */
#define CQ_CONTEXT_CQ_PERIOD_OFFSET      0x10u   /* BE16 */
#define CQ_CONTEXT_CQ_MAX_COUNT_OFFSET   0x12u   /* BE16 */
#define CQ_CONTEXT_COMP_EQN_OFFSET       0x17u   /* u8: the EQ NUMBER, not a vector */
#define CQ_CONTEXT_LOG_PAGE_SIZE_OFFSET  0x18u   /* u8 */
#define CQ_CONTEXT_MTT_BASE_H_OFFSET     0x1bu   /* u8 */
#define CQ_CONTEXT_MTT_BASE_L_OFFSET     0x1cu   /* BE32 */
#define CQ_CONTEXT_LAST_NOTIFIED_OFFSET  0x20u   /* BE32 */
#define CQ_CONTEXT_SOLICIT_PROD_OFFSET   0x24u   /* BE32 */
#define CQ_CONTEXT_CONSUMER_INDEX_OFFSET 0x28u   /* BE32 */
#define CQ_CONTEXT_PRODUCER_INDEX_OFFSET 0x2cu   /* BE32 */
#define CQ_CONTEXT_DB_REC_ADDR_OFFSET    0x38u   /* BE64: the record's DMA byte address */

#define MLX4_CQ_CONTEXT_SIZE             0x80u

/* struct mlx4_cqe (cq.h:41-60) is 32 bytes and its owner byte is the last of those 32
 * (cq.h:101, MLX4_CQE_OWNER_MASK).  This card runs 64-byte CQEs, and the layout follows
 * the EQE convention: the real CQE is the SECOND 32-byte half of the slot. */
#define CQE_LEGACY_SIZE                  0x20u
#define CQE_OWNER_OFFSET                 0x1fu
#define MLX4_CQE_OWNER_MASK              0x80u
/* Byte offset of the owner byte inside slot i, for a 64-byte CQE card. */
#define CQE_SLOT_OWNER_BYTE(factor) \
    (((factor) ? CQE_LEGACY_SIZE : 0u) + CQE_OWNER_OFFSET)

/* CQ doorbell: an 8-byte doorbell inside the UAR page (doorbell.h:42, cq.h:138-170). */
#define MLX4_CQ_DOORBELL                 0x20u

/* The request-notification command that goes in the doorbell (cq.h:168-172).  Zero means
 * "update the index only", which is what the poll path does. */
#define MLX4_CQ_DB_REQ_NOT_SOL           0x01000000u   /* 1 << 24 */
#define MLX4_CQ_DB_REQ_NOT               0x02000000u   /* 2 << 24 */

/* ==================================================================== */
/* R4 stage 3: receive buffers and their WQEs                           */
/* ==================================================================== */

/*
 * The RQ WQE is a chain of data segments (qp.h:440-444):
 *
 *     __be32 byte_count;   __be32 lkey;   __be64 addr;
 *
 * and this port uses ONE segment per WQE -- MLX4_EN_MAX_RX_SEGS is 1 in this reference
 * (en.h:86-87) and the code is written around the single-fragment case -- so a WQE is 16
 * bytes.  Two consequences that are easy to miss:
 *
 *   - `rx_desc->data[0].lkey` is `mr.key` (mlx4_en_rx.c:59): the TRANSFORMED lkey that
 *     stage 1 reported as 0x00010000, not the raw MPT index.  Putting the index here
 *     would be a descriptor the hardware cannot resolve, with no command failing.
 *
 *   - `byte_count` is `rx_mb_size - MLX4_NET_IP_ALIGN` (mlx4_en_rx.c:58): the size of the
 *     buffer the hardware may fill, NOT the size of the frame that will arrive.
 *     MLX4_NET_IP_ALIGN is 2 (en.h:80).
 *
 * This port's buffer is a deliberately plain 2048 bytes rather than an mbuf.  mlx4_en
 * reaches into an mbuf cluster and compensates with a 2-byte alignment at the
 * protocol-stack boundary; stage 3 is about whether a frame can land in DMA memory this
 * driver owns and be reported by a CQE, so the target is page-aligned and the count is
 * 2048 - 2.  Copying the frame out afterwards is a separate, later decision, and taking
 * an unaligned pointer here would silently break both the count and the alignment.
 */
#define RX_WQE_SIZE                      0x10u   /* one 16-byte data segment */
#define RX_WQE_BYTE_COUNT_OFFSET         0x00u   /* BE32 */
#define RX_WQE_LKEY_OFFSET               0x04u   /* BE32 */
#define RX_WQE_ADDR_OFFSET               0x08u   /* BE64 */

#define MLX4_NET_IP_ALIGN                2u

/*
 * The Ethernet frame overhead, which is what turns an L3 MTU into a wire size.
 *
 * The reference states the convention once, in the clamp it puts on SET_PORT general's mtu
 * field -- and the field carries a WIRE size, not an MTU:
 *
 *     mtu = min(mtu, eth_mtu_cap[port] + ETH_HLEN + VLAN_HLEN + ETH_FCS_LEN)
 *                                                        mlx4_port.c:1306-1308
 *
 * with `eth_mtu_cap` being the port's L3 capability (device.h:532 -- the field is literally
 * named *_cap, which is what makes it a capability and not a configuration).  For a 1500 MTU
 * that is 1500 + 14 + 4 + 4 = 1522, which is exactly what m3portup already programs, and the
 * family's own constant agrees from the other end: kIOEthernetMaxPacketSize is 1518
 * (IOEthernetController.h:45) = header + payload + FCS with no VLAN tag.
 *
 * Named here rather than written at the use sites because these are protocol constants, and
 * tools/check-hardcoded.py is explicit that the header defining a value is where it belongs
 * (it refuses a bare 1518/1522 anywhere else in src/).
 */
#define MLX4_ETH_HLEN                    14u
#define MLX4_ETH_VLAN_LEN                4u
#define MLX4_ETH_FCS_LEN                 4u

/*
 * TXBB_SIZE is 64 (en.h:84), and the receive ring reserves that much in FRONT of its WQEs
 * (mlx4_en_rx.c:406 adds it to buf_size; :482-488 skips past it before initialising any
 * descriptor).  So the ring's first 64 bytes are not WQEs and WQE i lives at
 * ring + 64 + i * stride -- the "dummy SQ" section 27.4 warned about, now with its number.
 */
#define RX_RING_HEADER_SIZE              64u

/*
 * The receive producer index goes into an ordinary doorbell RECORD in host DMA memory,
 * not to the UAR (mlx4_en_rx.c:250-254):
 *
 *     *ring->wqres.db.db = cpu_to_be32(ring->prod & 0xffff);
 *
 * A 16-bit value rather than the CQ's 24, and the destination is the record.
 */
#define RX_PROD_DB_WIDTH                 0xffffu

/* ==================================================================== */
/* R4 stage 6: the send queue's memory                                  */
/* ==================================================================== */

/*
 * The SQ's unit is the TXBB: 64 bytes (en.h:84), and that is the stride the QP context's
 * sq_size_stride has to describe -- `ilog2(size) << 3 | (ilog2(stride) - 4)` with 256
 * entries and a 64-byte stride is 0x42 (mlx4_en_resources.c:54-57, section 27.4).  The
 * receive ring measured the same 64 bytes the other way round: its WQEs are 16 bytes and
 * the 64-byte unit appears as the dummy SQ in front of them (RX_RING_HEADER_SIZE).
 */
#define MLX4_TXBB_SIZE                   64u

/*
 * The send-wqe stamp: the one encoding the TRANSMIT side uses, for every 64-byte TXBB of the
 * send queue's memory -- and every one of those units IS a descriptor slot, because nothing
 * is in front of them (SQ_WQE0_OFFSET below).
 *
 * STAMP_SHIFT is 31 and STAMP_VAL is 0x7fffffff (en.h:95-99), so a TXBB is stamped
 * `STAMP_VAL | owner << STAMP_SHIFT` (mlx4_en_stamp_wqe, mlx4_en_tx.c:271-287, and DPDK's
 * mlx4 independently:
 * `stamp = MLX4_SQ_STAMP_VAL | (owner << MLX4_SQ_OWNER_BIT)`, docs/EXTERNAL-REFS.md section
 * 4):
 *
 *     owner = 1 -> 0xffffffff   "software owns this slot" (free, not in flight)
 *     owner = 0 -> 0x7fffffff   "hardware owns it" (posted, valid to execute)
 *
 * The reference's port-activation path stamps EVERY 64-byte TXBB with owner = 1 before
 * anything is transmitted (mlx4_en_netdev.c:1373-1375, `*((u32 *) (tx_ring->buf + j)) =
 * INIT_OWNER_BIT` over the whole buffer, right after the memset at mlx4_en_tx.c:227).  Its
 * meaning is the one stage 9 depends on: zero would say bit 31 = 0 = "hardware owns", so an
 * all-zero WQE array means every slot looks in flight to the driver's own reclaim scan.
 *
 * STAGE 6 WROTE THE RECEIVE RING'S MARKER HERE INSTEAD -- `1 << STAMP_SHIFT` = 0x80000000,
 * which is `cpu_to_be32(1 << STAMP_SHIFT)` in the RX ring's header (mlx4_en_rx.c:482-488) and
 * agrees with the transmit encoding on the ownership bit and on nothing else -- and left the
 * WQE array all zero.  Both are docs/DEFECTS.md C12, fixed in 0.43.6 by writing
 * MLX4_SQ_INIT_OWNER_BIT into every 64-byte unit of the send queue's memory, exactly as the
 * loop above does.  Since 0.43.8 those units ARE the WQE array -- the 64-byte header this
 * port used to put in front of it is gone (SQ_WQE0_OFFSET, docs/DEFECTS.md E13) -- so the
 * range the reference's loop covers and the range the context's sq_size_stride describes are
 * now the same range, and a stamp left out anywhere is a slot the reclaim scan reads as in
 * flight.
 */
#define MLX4_STAMP_SHIFT                 31u          /* en.h:98 */
#define MLX4_SQ_STAMP_VAL                0x7fffffffu  /* en.h:96 */
#define MLX4_SQ_INIT_OWNER_BIT           (MLX4_SQ_STAMP_VAL | (1u << MLX4_STAMP_SHIFT))

/*
 * WHERE WQE 0 IS -- the one place the send queue and the receive ring disagree, and the
 * disagreement is the hardware's convention rather than a style choice: the same queue index
 * means a different byte offset on each of the two rings.
 *
 *   RQ: WQE 0 at the MTT base + 64.  The receive ring reserves a 64-byte unit in FRONT of its
 *   descriptors, stamps it, and then skips it: `ring->buf_size = size *
 *   sizeof(struct mlx4_en_rx_desc) + TXBB_SIZE` (mlx4_en_rx.c:406), and after the stamp
 *   `ring->buf += TXBB_SIZE` (:482-489), so every descriptor the hardware is given is 64
 *   bytes past the base.  MEASURED: this port builds the RQ the same way and the receive path
 *   works -- 2026-10-12, 51 completions naming our RX QPN, each frame's bytes read out of the
 *   buffer its CQE named (docs/R4-STAGE9-RESULT.md section 12).
 *
 *   SQ: WQE 0 at the MTT base + 0.  There is NO header on the transmit ring: its allocation
 *   is `ALIGN(size * ring->stride, MLX4_EN_PAGE_SIZE)` (mlx4_en_tx.c:123) with `ring->buf =
 *   ring->wqres.buf.direct.buf` (:139), and every descriptor is written at `ring->buf + index
 *   * TXBB_SIZE` (:686).  MEASURED the other way, at the price of a bug: with a 64-byte
 *   header in front of the WQEs, WQE 0 landed at +64 while the hardware read the base -- the
 *   all-`0xffffffff` ("software owns", i.e. not a descriptor) unit this port had stamped there
 *   -- and executed nothing.  No completion, no frame on the wire, no error: 0.43.7's failure
 *   (docs/DEFECTS.md E13, docs/R4-STAGE9-RESULT.md section 12).
 *
 * Both offsets live here, one home each, because the thing this port got wrong was having a
 * single shape for two queues -- and a host test can now assert the asymmetry instead of a
 * reader having to notice it (tools/test-cqe-offsets.c).
 */
#define SQ_WQE0_OFFSET                   0u   /* no header -- mlx4_en_tx.c:139, :686 */
#define RQ_WQE0_OFFSET                   RX_RING_HEADER_SIZE /* 64 -- mlx4_en_rx.c:406, :482-489 */

/*
 * Where WQE `index` lives in the send queue's allocation, as a byte offset from the MTT base.
 *
 * The receive ring deliberately does NOT get a macro of this shape: its offset is
 * RX_RING_HEADER_SIZE + index * stride, and folding two different conventions into one macro
 * is exactly what produced E13.  Its arithmetic stays visible at its own call sites, where the
 * number is not zero and a reader can see it.
 */
#define MLX4_SQ_WQE_OFFSET(index) \
    ((unsigned long long)SQ_WQE0_OFFSET + (unsigned long long)(index) * MLX4_TXBB_SIZE)

/*
 * The UAR index a software queue must use, and the two lines that settle it.
 *
 * device.h:1576-1579
 *     mlx4_get_num_reserved_uar() = 128 >> (PAGE_SHIFT - uar_page_shift)
 *     ("The first 128 UARs are used for EQ doorbells")
 * mlx4_main.c:184-193
 *     dev->caps.reserved_uars = max(that, devcap->reserved_uars /
 *                                   (1 << (PAGE_SHIFT - uar_page_shift)))
 *
 * The max is the part that matters: QUERY_DEV_CAP's reserved_uars does NOT decide the
 * starting index, it is one input to a maximum that 128 always wins on a card whose
 * UAR page size equals the system page size.  On this card that is exactly the case --
 * this port sends uar_page_sz 0 in INIT_HCA, so uar_page_shift = 0 + 12 = 12 = PAGE_SHIFT
 * (mlx4_main.c:856) and both terms collapse to 128 and 2.
 *
 * Using 2 here would put a CQ's UAR inside the 128 pages the EQ doorbells own, i.e. on
 * top of the doorbell that is already known to work -- and the symptom would not be an
 * error, it would be a CQ whose completions never arrive.
 *
 * The same arithmetic explains the EQ doorbell this port already uses:
 * ((eqn / 4) << uar_page_shift) + 0x800 + 8 * (eqn % 4) with EQN 28 gives
 * (7 << 12) + 0x800 = 0x7800, which is exactly what the working EQ logs show.
 */
#define MLX4_UAR_PAGE_SHIFT              12u
#define MLX4_EQ_UAR_DOORBELL_BASE        0x800u
#define MLX4_NUM_RESERVED_UARS           128u

/* mlx4_to_hw_uar_index() (device.h:1571-1573): index << (PAGE_SHIFT - uar_page_shift),
 * which is the identity when the two shifts are equal. */
static inline unsigned int mlx4ToHwUarIndex(unsigned int index)
{
    return index << (12 - MLX4_UAR_PAGE_SHIFT);
}

/* The UAR page's byte offset inside BAR2 (mlx4_uar_alloc: uar->pfn is the BAR2 start
 * PFN plus the index, mlx4_pd.c:158-160). */
static inline unsigned long long mlx4UarPageOffset(unsigned int index)
{
    return (unsigned long long)index << MLX4_UAR_PAGE_SHIFT;
}

/*
 * struct mlx4_mpt_entry (mlx4.h:303-320), 64 bytes -- which is this card's
 * dmpt_entry_sz, checked against the profile's own log rather than assumed:
 * "dmpt 0x100140000 log 11" together with "entry sizes ... dmpt 64".
 *
 * __packed in the reference because mtt_addr is 64 bits but only 4-byte aligned.  The
 * offsets are written out the way the EQ and CQ contexts are, so a member added to the
 * middle of the struct cannot silently move them.
 */
#define MPT_ENTRY_SIZE                  0x40u
#define MPT_ENTRY_FLAGS_OFFSET          0x00u   /* BE32 */
#define MPT_ENTRY_QPN_OFFSET            0x04u   /* BE32 */
#define MPT_ENTRY_KEY_OFFSET            0x08u   /* BE32 */
#define MPT_ENTRY_PD_FLAGS_OFFSET       0x0cu   /* BE32 */
#define MPT_ENTRY_START_OFFSET          0x10u   /* BE64 */
#define MPT_ENTRY_LENGTH_OFFSET         0x18u   /* BE64 */
#define MPT_ENTRY_LKEY_OFFSET           0x20u   /* BE32 */
#define MPT_ENTRY_WIN_CNT_OFFSET        0x24u   /* BE32 */
#define MPT_ENTRY_MTT_REP_OFFSET        0x2bu   /* u8, after 3 reserved bytes */
#define MPT_ENTRY_MTT_ADDR_OFFSET       0x2cu   /* BE64 */
#define MPT_ENTRY_MTT_SZ_OFFSET         0x34u   /* BE32 */
#define MPT_ENTRY_ENTITY_SIZE_OFFSET    0x38u   /* BE32 */
#define MPT_ENTRY_FIRST_BYTE_OFFSET     0x3cu   /* BE32 */

/*
 * mlx4.h:275-282.  The top nibble of flags is the entry's status: 0x0 means the
 * hardware owns it, 0xf that software does.  That is what makes reading the mapped
 * dMPT table back a real check -- if SW2HW_MPT was accepted, the firmware should have
 * replaced SW_OWNS with its own value, and the entry in host memory says so.
 */
#define MLX4_MPT_FLAG_SW_OWNS       (0xfu << 28)
#define MLX4_MPT_FLAG_FREE          (0x3u << 28)
#define MLX4_MPT_FLAG_MIO           (1u << 17)
#define MLX4_MPT_FLAG_BIND_ENABLE   (1u << 15)
#define MLX4_MPT_FLAG_PHYSICAL      (1u << 9)
#define MLX4_MPT_FLAG_REGION        (1u << 8)
#define MLX4_MPT_STATUS_NIBBLE_MASK 0xf0000000u
#define MLX4_MPT_STATUS_HW          0x00000000u

#define MLX4_MPT_PD_MASK            0x1ffffu
#define MLX4_MPT_PD_FLAG_FAST_REG   (1u << 27)
#define MLX4_MPT_PD_FLAG_RAE        (1u << 28)
#define MLX4_MPT_PD_FLAG_EN_INV     (3u << 24)

/* device.h:355-356 -- the access bits inside the flags word. */
#define MLX4_PERM_LOCAL_READ        (1u << 10)
#define MLX4_PERM_LOCAL_WRITE       (1u << 11)

/*
 * How many protection domains the card has and where the driver's start.
 *
 * mlx4.h:91 has MLX4_NUM_PDS = 1 << 15, and the allocator begins at
 * dev->caps.reserved_pds (mlx4_pd.c:122-124).  PDs are software-only on a native PF --
 * there is no SW2HW_PD -- so this is a bitmap in the driver and the only thing that has
 * to be right is which numbers belong to the firmware.
 */
#define MLX4_NUM_PDS                (1u << 15)

/*
 * The key transformation.  The two are inverses (plain `unsigned int` rather than
 * UInt32: this header is included by the control tool as well as the kext, and only the
 * kext has the IOKit types):
 *
 *     hw_index_to_key(ind) = (ind >> 24) | (ind << 8)
 *     key_to_hw_index(key) = (key << 24) | (key >> 8)
 *
 * The MPT entry's key field and SW2HW_MPT's in_modifier take the RAW index, while the
 * lkey that goes into a WQE is the transformed one (mlx4_en_rx.c:59 and
 * mlx4_en_tx.c:873 both put mr.key in the descriptor).  Mixing the two up yields a
 * descriptor the hardware cannot resolve, and no command fails to warn about it.
 */
static inline unsigned int mlx4HwIndexToKey(unsigned int ind)
{
    return (ind >> 24) | (ind << 8);
}

static inline unsigned int mlx4KeyToHwIndex(unsigned int key)
{
    return (key << 24) | (key >> 8);
}

/*
 * SET_PORT's two enums, which are DIFFERENT HCR FIELDS with OVERLAPPING VALUES
 * (cmd.h:192-208).  Read this before using any of them, because the values collide and a
 * wrong choice is not an invalid value the firmware rejects -- it is a different operation:
 *
 *   group 1 -- the OP_MODIFIER (cmd.h:192-197).  This is the command's third field:
 *
 *       MLX4_SET_PORT_IB_OPCODE    = 0x0
 *       MLX4_SET_PORT_ETH_OPCODE   = 0x1     <- what the MAC-table command sends
 *       MLX4_SET_PORT_BEACON_OPCODE= 0x4
 *
 *   group 2 -- the HIGH BYTE OF in_modifier, i.e. the Ethernet sub-command (cmd.h:199-208).
 *   This is `value << 8 | port`:
 *
 *       MLX4_SET_PORT_GENERAL    = 0x0   <- m3portup's SET_PORT general (in_modifier = port)
 *       MLX4_SET_PORT_RQP_CALC   = 0x1   <- stage 7's A0 qpn_calc
 *       MLX4_SET_PORT_MAC_TABLE  = 0x2   <- this stage's MAC table (in_modifier = 0x201)
 *       ... VLAN_TABLE 0x3, PRIO_MAP 0x4, GID_TABLE 0x5, ...
 *
 * The collision is exact and easy to misread: 0x1 is BOTH "Ethernet" (an op_modifier) and
 * "RQP_CALC" (a sub-command byte), and 0x0 is both "IB" and "GENERAL".  Two of this port's
 * call sites now use one group each -- `m3portup` sends GENERAL as the high byte with
 * ETH_OPCODE as the op_modifier, and the MAC-table command sends MAC_TABLE as the high byte
 * with the same op_modifier -- which is precisely the situation in which the two groups get
 * conflated.  Recorded as docs/DEFECTS.md E9.
 *
 * One more reason this pair matters: op_modifier 0 is the IB form of SET_PORT.  Sending it by
 * accident is not a rejected command, it is a different operation on the same port.
 */
#define MLX4_SET_PORT_IB_OPCODE   0x0u   /* group 1: op_modifier */
#define MLX4_SET_PORT_ETH_OPCODE  0x1u   /* group 1: op_modifier */
#define MLX4_SET_PORT_GENERAL     0x0u   /* group 2: in_modifier's high byte */
#define MLX4_SET_PORT_RQP_CALC    0x1u   /* group 2: in_modifier's high byte (stage 7) */

/*
 * The MAC table (mlx4_regs.h's group 2 above: MLX4_SET_PORT_MAC_TABLE = 2, cmd.h:203), and
 * the one thing about this
 * command that is easy to get wrong:
 *
 *   in_param is the MAILBOX's DMA address, and the mailbox holds the WHOLE TABLE -- 128
 *   entries of 8 bytes, not one address (mlx4_port.c:127-140: it memcpy's
 *   MLX4_MAC_TABLE_SIZE bytes into the mailbox and hands the mailbox's dma in).
 *
 * So "register one MAC" is implemented as "clear the table, put one entry in it, send
 * 1024 bytes".  Each entry is cpu_to_be64(mac | MLX4_MAC_VALID) (mlx4_port.c:299), where
 * MLX4_MAC_VALID is bit 63 (mlx4_port.c:48) and the address itself lives in the low 48
 * bits (MLX4_MAC_MASK, driver.h:40).  A slot whose valid bit is clear is an empty slot,
 * which is what `mlx4_is_mac_valid`-style tests read (mlx4_port.c:155-160).
 *
 * Op modifier is MLX4_SET_PORT_ETH_OPCODE (1) and the timeout is
 * MLX4_CMD_TIME_CLASS_B -- not A, which is what this port's other port commands use.
 */
#define MLX4_SET_PORT_MAC_TABLE   0x2u                        /* cmd.h:203 */
#define MLX4_MAX_MAC_NUM          128u                        /* driver.h:732 */
#define MLX4_MAC_ENTRY_SIZE       8u
/* driver.h:733 -- the byte count the command's mailbox carries. */
#define MLX4_MAC_TABLE_SIZE       (MLX4_MAX_MAC_NUM * MLX4_MAC_ENTRY_SIZE)
#define MLX4_MAC_VALID            (1ull << 63)                /* mlx4_port.c:48 */
#define MLX4_MAC_MASK             0xffffffffffffull            /* driver.h:40 */

/* ==================================================================== */
/* R4 stage 7: SET_PORT qpn_calc, the command that tells the firmware    */
/* where the A0 MAC-derived QPN range starts                            */
/* ==================================================================== */

/*
 * Why there is a command at all, and why it has to come before INIT_PORT.
 *
 * Under A0 steering the MAC table is not consulted by the firmware at receive time; the
 * table's INDEX becomes a QPN: `*qpn = base_qpn + index` (mlx4_en_netdev.c:573-576).  The
 * base is not something the driver can look up -- it PUSHES it, with this command
 * (mlx4_port.c:1595, `context->base_qpn = cpu_to_be32(base_qpn)`), and the firmware does not
 * check that the base lands in some region of its own.  The reference's order is
 * general -> qpn_calc -> INIT_PORT (mlx4_en_netdev.c:1388, :1394, :1403), and it has to be
 * that order: the base is what a frame arriving after INIT_PORT is steered by.
 *
 * The command is A0-only in the reference -- mlx4_SET_PORT_qpn_calc returns 0 immediately
 * when the steering mode is not A0 (mlx4_port.c:1588-1589), i.e. in B0 it is a no-op rather
 * than something else.  This port's INIT_HCA never writes the B0 steering byte
 * (INIT_HCA_UC_STEERING_OFFSET, the block below), so the HCA was told A0 and this command is
 * the one the reference would send; the selector says so in the log and the falsification
 * condition is the firmware's own answer (BAD_PARAM 0x3 / BAD_RESOURCE 0x5 => it does check
 * the base, and the base has to move to num_qps - (1 << log_max_macs)).
 *
 * Its encoding is the one E9 warns about, and this is the first call site to use both groups
 * at once:
 *
 *   op_modifier = MLX4_SET_PORT_ETH_OPCODE  = 0x1   (group 1)
 *   in_modifier = MLX4_SET_PORT_RQP_CALC << 8 | port = 0x101 for port 1   (group 2)
 *   in_param    = the mailbox's DMA address (mlx4_port.c:1591-1594): the context goes in the
 *                 mailbox, not in a register
 *   timeout     = MLX4_CMD_TIME_CLASS_B (mlx4_port.c:1607) -- the same class the other two
 *                 port commands use, and a different name from the class this port's QP and
 *                 CQ commands use
 */

/*
 * struct mlx4_set_port_rqp_calc_context (mlx4.h:798-813, spelled `rererved` for the byte at
 * 0x04 in the reference source).  The reference fills FIVE of these fields and leaves the
 * rest to the zeroed mailbox; the offsets are listed so the ones it does not fill are visibly
 * deliberate rather than forgotten.
 */
#define RQP_CALC_BASE_QPN_OFFSET      0x00u  /* __be32 */
#define RQP_CALC_RESERVED_OFFSET      0x04u  /* u8,   mlx4.h:799 (the reference's typo) */
#define RQP_CALC_N_MAC_OFFSET         0x05u  /* u8,   mlx4_port.c:1596 */
#define RQP_CALC_N_VLAN_OFFSET        0x06u  /* u8,   left 0 */
#define RQP_CALC_N_PRIO_OFFSET        0x07u  /* u8,   left 0 */
#define RQP_CALC_MAC_MISS_OFFSET      0x0bu  /* u8,   left 0 */
#define RQP_CALC_INTRA_NO_VLAN_OFFSET 0x0cu  /* u8,   set to 0 explicitly (:1599) */
#define RQP_CALC_NO_VLAN_OFFSET       0x0du  /* u8,   MLX4_NO_VLAN_IDX (:1600) */
#define RQP_CALC_INTRA_VLAN_MISS_OFFSET 0x0eu /* u8,  set to 0 explicitly (:1601) */
#define RQP_CALC_VLAN_MISS_OFFSET     0x0fu  /* u8,   MLX4_VLAN_MISS_IDX (:1602) */
#define RQP_CALC_NO_VLAN_PRIO_OFFSET  0x13u  /* u8,   left 0 */
#define RQP_CALC_PROMISC_OFFSET       0x14u  /* __be32, promisc << 31 | base (:1597-1598) */
#define RQP_CALC_MCAST_OFFSET         0x18u  /* __be32, m_promisc << 30 | base (:1603-1604) */

/*
 * The two shift amounts, and the two VLAN indices.  Neither index is zero by accident:
 * MLX4_VLAN_MISS_IDX is 1 (device.h:424-425 -- it is the second enumerator, so it is the
 * predecessor of these constants plus one), and the reference assigns it explicitly
 * (mlx4_port.c:1602) while assigning no_vlan from MLX4_NO_VLAN_IDX (0, :1600).  Writing
 * `vlan_miss = 0` here would look like "leave it at the clear value" and would be a
 * different table.
 */
#define SET_PORT_PROMISC_SHIFT        31u    /* en_port.h:39 / mlx4.h:769 */
#define SET_PORT_MC_PROMISC_SHIFT     30u    /* en_port.h:40 / mlx4.h:770 */
#define MLX4_NO_VLAN_IDX              0u     /* device.h:424 */
#define MLX4_VLAN_MISS_IDX            1u     /* device.h:425 */

/*
 * The mcast field's high bits, from SET_PORT's own parameter (mlx4.h:772-774):
 * MCAST_DIRECT means the firmware sends multicast to the QPN in this context, which is what
 * this port wants (index 0's neighbour, the RX QP).  Which of the two the reference picks is
 * a capability read -- see MLX4_DEV_CAP_FLAG_VEP_MC_STEER above.
 */
#define MCAST_DIRECT_ONLY             0u
#define MCAST_DIRECT                  1u
#define MCAST_DEFAULT                 2u

/*
 * The RoCE V1/V2 capability, and why the check is against bmme_flags.
 *
 * The reference derives MLX4_DEV_CAP_FLAG2_ROCE_V1_V2 (bit 33 of the 64-bit flags
 * word) from a bit in QUERY_DEV_CAP's bmme_flags -- mlx4_fw.c:1015-1016:
 *
 *     if (dev_cap->bmme_flags & MLX4_FLAG_ROCE_V1_V2)
 *             dev_cap->flags2 |= MLX4_DEV_CAP_FLAG2_ROCE_V1_V2;
 *
 * and MLX4_FLAG_ROCE_V1_V2 is MLX4_BMME_FLAG_ROCE_V1_V2 = 1 << 19 (device.h:282).
 * This port already reads bmme_flags, but it never collected that flag2 bit, so a
 * test of "flags2 & (1 << 33)" would silently always be false -- which is the right
 * answer for the wrong reason.  SET_PORT general needs this bit: with it, the
 * reference sets flags bit 4 AND the roce_mode byte together (mlx4_port.c:1562-1566),
 * and one without the other is a combination it never sends.
 */
#define MLX4_BMME_FLAG_ROCE_V1_V2 (1u << 19)

/* ==================================================================== */
/* R4 stage 9: the send WQE, the send doorbell, and the completion      */
/* element's fields                                                     */
/* ==================================================================== */

/*
 * The send WQE: a 16-byte control segment followed by a chain of data segments
 * (struct mlx4_wqe_ctrl_seg / struct mlx4_wqe_data_seg, qp.h:302-330 and :440-444).
 *
 * This port posts exactly ONE data segment, so the WQE is 32 bytes and fits in one 64-byte
 * TXBB (MLX4_TXBB_SIZE) with room to spare -- which is the shape sq_size_stride 0x42
 * describes and stage 6 measured.
 *
 * The three fields whose position matters and whose wrong value is silent:
 *
 *   owner_opcode.  Bit 31 is the owner bit (mlx4_wqe_ctrl_seg, and MLX4_WQE_CTRL_OWN =
 *   1 << 31, qp.h:286), and the OPCODE IS IN THE LOW EIGHT BITS --
 *   `opcode = cpu_to_be32(MLX4_OPCODE_SEND) | owner_bit` (mlx4_en_tx.c:682-683, :761-762),
 *   with bits 8..23 carrying a producer count in TXBB units
 *   (`opcode |= cpu_to_be32((bf_prod & 0xffff) << 8)`, mlx4_en_tx.c:897).
 *
 *   THE PRODUCER BITS ARE A BLUEFLAME CONVENTION, and this port writes them anyway: the
 *   reference ORs them in only on the blueflame branch (:890-905), where the hardware needs to
 *   know where in the blueflame buffer the descriptors are, while the memory path (:906-919)
 *   writes the opcode and the owner bit and nothing else.  That is a deviation from the path
 *   this port is on, and it is recorded rather than silently kept: MLX4_TX_PROD_SHIFT/_MASK and
 *   the WQE readback check both use it, and on the FIRST post of a lap the producer is 0, so
 *   the encoding this port sends and the reference's memory-path encoding are the same 4 bytes
 *   there.  If a transmit completion comes back for post 1 but the wrap phase misbehaves, this
 *   field is the first thing to drop (docs/R4-STAGE9-RESULT.md section 12).
 *
 *   WHAT GOES WRONG IF THE OPCODE IS SHIFTED, because this is the stage's one mistake that
 *   fails SILENTLY: with `MLX4_OPCODE_SEND << 24` the opcode would land in byte 0 of the
 *   big-endian word -- the owner byte -- and byte 3 would be zero, so the descriptor's opcode
 *   field reads MLX4_OPCODE_NOP (0x00, device.h:365).  The doorbell would still be rung, a
 *   completion would probably still arrive, and not one byte would leave the card: a failure
 *   represented as a non-failure (docs/DEFECTS.md family B) reaching stage 10 as "ping does
 *   not work".  tools/test-cqe-offsets.c asserts the byte position and the selector logs the
 *   WQE's 32 bytes in hex, so a first run can be read.
 *
 *   srcrb_flags.  The reference's own CTRL_FLAGS is MLX4_WQE_CTRL_CQ_UPDATE |
 *   MLX4_WQE_CTRL_SOLICITED (mlx4_en_tx.c:635-636): bits [3:2] (C) generating the completion
 *   event, and bit 1 (SE) the solicited event.  MLX4_WQE_CTRL_CQ_UPDATE is the 2-bit field
 *   3 << 2 (qp.h:292), not 1 << 2.
 *
 *   byte_count / lkey / addr.  One data segment (qp.h:440-444), and byte_count is the FRAME
 *   length WITHOUT the FCS: the reference's expression is
 *   `max(m_pkthdr.len, ETHER_MIN_LEN - ETHER_CRC_LEN)` (mlx4_en_tx.c:764-765), whose 64-byte
 *   minimum minus the 4-byte CRC is 60.  The RECEIVE side is asymmetric -- FCS_KEEP means a
 *   received CQE's byte_cnt includes those 4 bytes -- and "fixing" the asymmetry is what
 *   would be wrong (docs/R4-STAGE9-PLAN.md section 4, decision 2).
 */
#define MLX4_OPCODE_NOP                  0x00u   /* device.h:365 -- what a shifted opcode
                                                  * silently becomes */
#define MLX4_OPCODE_SEND                 0x0au   /* device.h:369 */
#define MLX4_EN_BIT_DESC_OWN             0x80000000u /* en.h:247 */
#define MLX4_TX_OWNER_SHIFT              31u     /* qp.h:286, MLX4_WQE_CTRL_OWN */
#define MLX4_TX_PROD_SHIFT               8u      /* mlx4_en_tx.c:897 */
#define MLX4_TX_PROD_MASK                0xffffu /* mlx4_en_tx.c:897 */

#define MLX4_WQE_CTRL_OWN                (1u << 31)  /* qp.h:286 */
#define MLX4_WQE_CTRL_CQ_UPDATE          (3u << 2)   /* qp.h:292 -- the 2-bit field C */
#define MLX4_WQE_CTRL_SOLICITED          (1u << 1)   /* qp.h:293 */
/*
 * The two checksum-request bits in the same word (qp.h:294-295), and what asking for them
 * means is the whole of the model: the STACK has already written the pseudo-header value into
 * the frame's checksum field, and these two bits tell the hardware to finish it.  The other two
 * bits the descriptor has for this (MLX4_WQE_CTRL_IIP 1<<28 and MLX4_WQE_CTRL_ILP 1<<27,
 * qp.h:289-290, "the hardware computes the pseudo-header too") are NEVER set by the reference
 * implementation (docs/M5-REFERENCE.md section 1.5 checked every use in the tree), and this
 * port does not set them either -- that would be a second, unverified model of the same thing.
 */
#define MLX4_WQE_CTRL_IP_CSUM            (1u << 4)   /* qp.h:294 */
#define MLX4_WQE_CTRL_TCP_UDP_CSUM       (1u << 5)   /* qp.h:295 */
#define MLX4_WQE_CTRL_FORCE_LOOPBACK     (1u << 0)   /* qp.h:299 -- DELIBERATELY NOT SET:
                                                      * it would loop this frame back into
                                                      * the receive path, and stage 9 is
                                                      * measuring the wire */

/* struct mlx4_wqe_ctrl_seg (qp.h:302-330), 16 bytes. */
#define TX_WQE_OWNER_OPCODE_OFFSET       0x00u  /* BE32: owner | prod[8..23] | opcode[0..7] */
#define TX_WQE_VLAN_TAG_OFFSET           0x04u  /* union with bf_qpn */
#define TX_WQE_INS_VLAN_OFFSET           0x06u
#define MLX4_WQE_CTRL_INS_CVLAN          (1u << 6) /* qp.h:296; ins_vlan byte */
#define TX_WQE_FENCE_SIZE_OFFSET         0x07u  /* the descriptor's size in 16-byte units */
#define TX_WQE_SRCRB_FLAGS_OFFSET        0x08u  /* BE32: high 24 bits SRC rb, low 8 flags */
#define TX_WQE_IMM_OFFSET                0x0cu  /* BE32 */
#define TX_WQE_CTRL_SIZE                 0x10u

/* struct mlx4_wqe_data_seg (qp.h:440-444), 16 bytes -- the one this stage posts. */
#define TX_WQE_BYTE_COUNT_OFFSET         0x10u
#define TX_WQE_LKEY_OFFSET               0x14u
#define TX_WQE_ADDR_OFFSET               0x18u
#define TX_WQE_DATA_SIZE                 0x10u
#define TX_WQE_SEND_SIZE                 0x20u  /* ctrl + one data segment */

/*
 * TX_WQE_FENCE_SIZE -- what the ctrl segment's fence_size byte holds: THE DESCRIPTOR'S SIZE IN
 * 16-BYTE UNITS, COUNTING THE CONTROL SEGMENT ITSELF.  It is not the number of data segments.
 *
 * This cost four builds of a transmit that never happened.  The first version of this port read
 * FreeBSD's line -- `tx_desc->ctrl.fence_size = (ds_cnt & 0x3f)` (mlx4_en_tx.c:842) -- as "ds_cnt
 * is the data segment count", and it is not: ds_cnt is computed on the line above as
 * `(dseg - (struct mlx4_wqe_data_seg *)tx_desc) + nr_segs` (mlx4_en_tx.c:808), and dseg STARTS AT
 * `&tx_desc->data` (mlx4_en_tx.c:688), i.e. at unit 1 -- so ds_cnt = 1 + the segment count.
 *
 * The other two independent implementations agree, and each says it in its own way:
 *
 *   Linux mlx4_en   real_size = CTRL_SIZE + nr_frags * DS_SIZE (en_tx.c:648), then
 *                   `real_size = (real_size / 16) & 0x3f` (:1075): bytes over sixteen.  The
 *                   BlueFlame branch writes the SAME number into the same byte as
 *                   `qpn_vlan.bf_qpn = ring->doorbell_qpn | cpu_to_be32(real_size)` (:1080),
 *                   which is what makes the two branches one encoder instead of two.
 *   libmlx4         `int size = sizeof *ctrl / 16;` (providers/mlx4/qp.c:269, = 1) then
 *                   `size += wr->num_sge * (sizeof *seg / 16)` (:446) and
 *                   `ctrl->fence_size = (FENCE ? ... : 0) | size` (:449).  Its own consumer
 *                   agrees in the same file: `int ds = ((struct mlx4_wqe_ctrl_seg *)wqe)->
 *                   fence_size << 2` (:78) is a count of BYTES, and the stamping loop walks
 *                   from offset 16 up to it.
 *
 * A value of 1 for a ctrl-plus-one-data-segment WQE declares a 16-byte descriptor: a control
 * segment with no data segment at all, i.e. a SEND with nothing to send.  That is silent -- no
 * frame, no completion, no error -- which is exactly what this port saw for four builds.
 */
#define TX_WQE_FENCE_SIZE(ds) \
    (((TX_WQE_CTRL_SIZE + ((ds) * TX_WQE_DATA_SIZE)) / 16u) & 0x3fu)

/* One control segment and one data segment: 32 bytes, so two 16-byte units. */
#define TX_WQE_SEND_FENCE_SIZE           TX_WQE_FENCE_SIZE(1u)

/*
 * The send doorbell, and the one offset in stage 9 that has to be checked twice.
 *
 *     ring->doorbell_qpn = ring->qp.qpn << 8;                       (mlx4_en_tx.c:231)
 *     writel(cpu_to_be32(ring->doorbell_qpn),
 *            ((u8 *)ring->bf.uar->map) + MLX4_SEND_DOORBELL);       (:918-919)
 *
 * So the value carries the QPN in bits 8..31 -- the QPN SHIFTED LEFT, not a producer index --
 * and the register is 0x14 inside the UAR page.  The CQ's doorbell is 0x20 (doorbell.h:42)
 * and the two differ by 0xC: writing the CQ's offset here would knock on a door that is not
 * this one, and nothing would say so.
 */
#define MLX4_SEND_DOORBELL               0x14u  /* doorbell.h:41 */
#define MLX4_SEND_DB_QPN_SHIFT           8u     /* mlx4_en_tx.c:231 */

/*
 * struct mlx4_cqe (cq.h:41-60) -- 32 bytes, and this card runs 64-byte CQEs, so the struct
 * lives in the SECOND half of each slot (CQE_SLOT_OWNER_BYTE above, and cqNextCqe's
 * cqeFactor).  The offsets below are the struct's own, and tools/test-cqe-offsets.c checks
 * them against the C compiler's view of the same declaration instead of against a second
 * hand-copy of them.
 *
 * WHICH OF THEM THIS STAGE USES, and nothing beyond that (plan section 4, decision 3): the
 * CQE's byte layout has only ONE source in this tree -- DPDK's was not obtained, see
 * docs/EXTERNAL-REFS.md section 12.3 -- so the four fields the criteria need are the four
 * this stage reads:
 *
 *   vlan_my_qpn      the QPN that received the frame, in the low 24 bits (MLX4_CQE_QPN_MASK);
 *                    the high bits carry the VLAN and L2-tunnel flags (cq.h:91-92), which is
 *                    why the mask is not optional.
 *   byte_cnt         the frame length, FCS included because this card has FCS_KEEP set.
 *   wqe_index        which receive WQE the frame used -- what the refill is driven from.
 *   owner_sr_opcode  bit 7 the owner phase, bit 6 "this is a send completion"
 *                    (MLX4_CQE_IS_SEND_MASK) and bits [4:0] the opcode, whose ERROR value
 *                    (0x1e, device.h:387) is how the reference detects a completion in error
 *                    (mlx4_en_tx.c:381-382).  The error CQE is a DIFFERENT 32-byte struct in
 *                    which the same offsets hold my_qpn / wqe_index / syndrome (cq.h:63-72),
 *                    which is why the opcode is read before anything else is interpreted.
 *
 * The checksum field and the status word are read and NOT judged: they carry the receive-side
 * ptype and checksum flags (cq.h:131-138) and this stage does no offload.
 */
#define CQE_VLAN_MY_QPN_OFFSET           0x00u  /* BE32 */
#define MLX4_CQE_QPN_MASK                0xffffffu
#define MLX4_CQE_CVLAN_PRESENT_MASK      (1u << 29) /* cq.h:90; sl_vid has full TCI */
#define CQE_IMMED_RSS_INVALID_OFFSET     0x04u  /* BE32 */
#define CQE_G_MLPATH_RQPN_OFFSET         0x08u  /* BE32 */
#define CQE_SL_VID_OFFSET                0x0cu  /* BE16 */
#define CQE_RLID_OFFSET                  0x0eu  /* BE16 */
#define CQE_STATUS_OFFSET                0x10u  /* BE16: ptype / checksum flags (cq.h:131) */
#define CQE_IPV6_EXT_MASK_OFFSET         0x12u  /* u8 */
#define CQE_BADFCS_ENC_OFFSET            0x13u  /* u8: bit 4 = MLX4_CQE_BAD_FCS (cq.h:141) */
#define CQE_BYTE_CNT_OFFSET              0x14u  /* BE32: the frame length, FCS included */
#define CQE_WQE_INDEX_OFFSET             0x18u  /* BE16 */
#define CQE_CHECKSUM_OFFSET              0x1au  /* BE16 */
#define CQE_OWNER_SR_OPCODE_OFFSET       0x1fu  /* u8 -- the byte CQE_OWNER_OFFSET names */
/* The error CQE (struct mlx4_err_cqe, cq.h:63-72) keeps my_qpn at 0x00 and wqe_index at 0x18
 * -- which is why the refill can still be driven from an error completion -- and puts the two
 * syndrome bytes at 0x1a and 0x1b, i.e. where the good CQE has checksum/reserved. */
#define CQE_ERR_VENDOR_SYNDROME_OFFSET   0x1au  /* u8 */
#define CQE_ERR_SYNDROME_OFFSET          0x1bu  /* u8 */
#define CQE_LEGACY_TOTAL                 0x20u

#define MLX4_CQE_IS_SEND_MASK            0x40u  /* cq.h:100 */
#define MLX4_CQE_OPCODE_MASK             0x1fu  /* cq.h:101 */
#define MLX4_CQE_OPCODE_ERROR            0x1eu  /* device.h:387 */
#define MLX4_CQE_BAD_FCS                 (1u << 4)  /* cq.h:141 */

/*
 * The protocol type and checksum verdict the CQE's status word carries (cq.h:122-130).
 *
 * Received frames are checksummed by the hardware whether or not the driver asked for it, and
 * this word is how it says so: IPOK is "the IP and L4 checksums were verified", and the
 * per-protocol bits say which of them the frame carried.  The verdict itself is the pair
 * (IPOK set, CQE_CHECKSUM_OFFSET == MLX4_CQE_CHECKSUM_VALID) -- see mlx4_en_rx.c:810-818,
 * where the driver's own test is exactly that conjunction and the 0xffff is a SENTINEL meaning
 * "verified", not a checksum.
 */
#define MLX4_CQE_STATUS_IPV4             (1u << 6)   /* cq.h:123 */
#define MLX4_CQE_STATUS_IPV4F            (1u << 7)   /* cq.h:124 -- a fragment */
#define MLX4_CQE_STATUS_IPV6             (1u << 8)   /* cq.h:125 */
#define MLX4_CQE_STATUS_IPV4OPT          (1u << 9)   /* cq.h:126 -- carries IP options */
#define MLX4_CQE_STATUS_TCP              (1u << 10)  /* cq.h:127 */
#define MLX4_CQE_STATUS_UDP              (1u << 11)  /* cq.h:128 */
#define MLX4_CQE_STATUS_IPOK             (1u << 12)  /* cq.h:129 */

/* The value at CQE_CHECKSUM_OFFSET that means "the hardware verified it" (mlx4_en_rx.c:813). */
#define MLX4_CQE_CHECKSUM_VALID          0xffffu

/*
 * The 64-byte CQE slot, named so that the two halves are one arithmetic instead of two: the
 * owner byte is the last byte of the second half.  CQE_LEGACY_SIZE / CQE_OWNER_OFFSET above
 * are the 32-byte view; this is the slot's.
 */
#define MLX4_CQE_SLOT_SIZE               0x40u

/*
 * The frame stage 9 puts on the wire (plan section 4, decision 1).  Every field of it is a
 * CHOICE rather than a device answer, which is why the numbers live here with the reason
 * beside them instead of inside the selector:
 *
 *   destination  broadcast (ff:ff:ff:ff:ff:ff), so nobody has to be known and the frame is
 *                unambiguously a test frame.
 *   ethertype    0x88B5, the IEEE "Local Experimental Ethertype 1": nothing on a normal LAN
 *                interprets it, so the frame cannot be mistaken for ARP, DHCP or mDNS and
 *                cannot make a peer do anything.  ARP is stage 10 -- it needs a legal source
 *                protocol address and would actually query the network.
 *   source       this port's own registered address (stage 5's MAC entry), read out of the
 *                driver at run time; a frame with a fabricated source would be a worse test
 *                frame, not a better one.
 *   length       60 bytes WITHOUT the FCS -- 64 with the 4 bytes the hardware appends -- so
 *                the frame is a legal minimum-size Ethernet frame rather than something a
 *                switch may drop or pad (hence ETHER_MIN_LEN - ETHER_CRC_LEN, the reference's
 *                own expression for what byte_count carries).
 *   payload      a fixed pattern, so a frame that comes back can be identified byte for byte
 *                with no protocol stack at all (this is a raw-frame experiment, not a
 *                protocol implementation).
 */
#define CX3_R4RAW_FRAME_BYTES            60u     /* ETHER_MIN_LEN (64) - ETHER_CRC_LEN (4) */
#define CX3_R4RAW_ETHERTYPE              0x88b5u
#define CX3_R4RAW_MAC_BYTES              6u
#define CX3_R4RAW_ETHERTYPE_OFFSET       (CX3_R4RAW_MAC_BYTES * 2u)         /* 12 */
#define CX3_R4RAW_PAYLOAD_OFFSET         (CX3_R4RAW_ETHERTYPE_OFFSET + 2u)  /* 14 */
#define CX3_R4RAW_PAYLOAD_BYTES          (CX3_R4RAW_FRAME_BYTES - CX3_R4RAW_PAYLOAD_OFFSET)
/* The payload's first four bytes are the tag 'C' 'X' '3' 'R', which is how a frame that comes
 * back is recognised as this driver's own without a protocol stack.  Named one byte at a time
 * rather than as a 32-bit constant because the bytes go into the frame in this order and a
 * value would make the reader work out the byte order. */
#define CX3_R4RAW_TAG_0                  0x43u   /* 'C' */
#define CX3_R4RAW_TAG_1                  0x58u   /* 'X' */
#define CX3_R4RAW_TAG_2                  0x33u   /* '3' */
#define CX3_R4RAW_TAG_3                  0x52u   /* 'R' */
#define CX3_R4RAW_RX_KEEP_BYTES          64u     /* how much of a received frame is copied out */

/*
 * The ONE size for every hex dump this stage logs -- the CQE's 32 bytes, the send WQE's 32,
 * the frame's 60, the received frame's up to 64 -- so that "the buffer is big enough" is a
 * property of the constant rather than of each call site.
 *
 * This is not hypothetical.  The first draft sized the receive poll's buffer for a CQE
 * (3 x 32 + 1) and then formatted a received frame into it (up to 3 x 64 + 1): 95 bytes written
 * past the end of a stack buffer, in the kernel, on the first run that received a frame of 32
 * bytes or more.  Found in review of that draft, and the guard is the same shape as D12's --
 * make the two numbers one number instead of checking that they agree.  `n` bytes need 3n for
 * "xx " per byte plus one for the terminator.
 * (docs/DEFECTS.md E12.)
 */
#define CX3_R4RAW_HEX_BUF_BYTES          (3u * CX3_R4RAW_RX_KEEP_BYTES + 1u)

/*
 * QUERY_ADAPTER outbox (mlx4_fw.c:1795-1797).  inta_pin is the one field this port
 * needs: the mask and offset of the interrupt-clear register are computed from it
 * (mlx4_eq.c:1184-1187).
 */
#define QUERY_ADAPTER_OUT_SIZE            0x100u
#define QUERY_ADAPTER_INTA_PIN_OFFSET     0x10u  /* u8  */
#define QUERY_ADAPTER_VSD_OFFSET          0x20u  /* vendor-specific data, not the ID itself */
#define MLX4_BOARD_ID_LEN                 64u    /* device.h:101 */
/* Offsets relative to VSD: mlx4_fw.c:get_board_id (1751-1787). */
#define MLX4_VSD_SIG1_OFFSET              0x00u
#define MLX4_VSD_SIG2_OFFSET              0xdeu
#define MLX4_VSD_MLX_BOARD_ID_OFFSET      0xd0u
#define MLX4_VSD_TS_BOARD_ID_OFFSET       0x20u
#define MLX4_VSD_SIGNATURE_TOPSPIN        0x05adu

/*
 * QUERY_FUNC outbox (mlx4_fw.c:1742-1750).  On a device with system EQs this is
 * how the driver learns how many EQs and UARs its function actually has:
 *
 *     dev_cap->max_eqs      = func.max_eq;
 *     dev_cap->reserved_eqs = func.rsvd_eqs;
 *     dev_cap->reserved_uars = func.rsvd_uars;
 *
 * and those three values decide which EQ numbers the driver may use, so EQ
 * numbering depends on this command (mlx4_main.c:2289-2297).
 */
#define QUERY_FUNC_RSVD_EQS_OFFSET   0x04u  /* u16 BE */
#define QUERY_FUNC_MAX_EQ_OFFSET     0x06u  /* u16 BE */
#define QUERY_FUNC_RSVD_UARS_OFFSET  0x0bu  /* u8 */

/*
 * EQ geometry.  mlx4_eq.c:51-65 and device.h:891.
 *
 * The asynchronous EQ is created with MLX4_NUM_SPARE_EQE entries more than the
 * events it expects, because the HCA thinks the queue has overflowed if the
 * consumer index is not advanced often enough (mlx4_eq.c:815-823).
 */
#define MLX4_NUM_ASYNC_EQE      0x100u
#define MLX4_NUM_SPARE_EQE      0x80u
#define MLX4_EQ_ENTRY_SIZE      0x20u   /* the legacy EQE payload size */

/*
 * EQ context flags/state (mlx4_eq.c:60-64).  Note that STATUS_OK and OWNER_SW are
 * the *zero* values, so the flags dword this port writes is exactly the armed state
 * shifted into place: MLX4_EQ_STATE_ARMED = 9 << 8 = 0x900.  An earlier comment in
 * the code said "ARMED is zero", which was wrong -- the value written was right and
 * the explanation underneath it was not.
 */
#define MLX4_EQ_STATUS_OK       (0u  << 28)
#define MLX4_EQ_OWNER_SW        (0u  << 24)
#define MLX4_EQ_STATE_ARMED     (9u  <<  8)
#define MLX4_EQ_STATE_FIRED     (10u <<  8)

/*
 * struct mlx4_eq_context byte offsets (mlx4.h:323).  Only the fields this port
 * fills are named; the rest stay zero, which is what the reference leaves them.
 */
#define EQ_CONTEXT_FLAGS_OFFSET           0x00u
#define EQ_CONTEXT_LOG_EQ_SIZE_OFFSET     0x0cu
#define EQ_CONTEXT_INTR_OFFSET            0x17u
#define EQ_CONTEXT_LOG_PAGE_SIZE_OFFSET   0x18u
#define EQ_CONTEXT_MTT_BASE_H_OFFSET      0x1bu
#define EQ_CONTEXT_MTT_BASE_L_OFFSET      0x1cu
#define EQ_CONTEXT_CONSUMER_INDEX_OFFSET  0x28u

/*
 * struct mlx4_eqe byte offsets (device.h:891).  Two of these are easy to get
 * wrong and both matter:
 *
 *   - the owner byte is the LAST byte of the 32-byte EQE (0x1f), not the first;
 *   - with 64-byte EQEs the legacy 32-byte EQE sits at offset +0x20 inside the
 *     slot ("the first 32 bytes in the 64 byte EQE are reserved",
 *     mlx4_eq.c:110-116).
 */
#define MLX4_EQE_TYPE_OFFSET          0x01u
#define MLX4_EQE_OWNER_OFFSET         0x1fu
/*
 * Where inside a slot the legacy 32-byte EQE starts, and where its owner byte is.
 *
 * Two call sites need this: the consumer in the hardware layer, and the armed-wait
 * in the provider.  They disagreed for a while because one of them computed the
 * owner position as "half the stride" -- which happens to be right for 64-byte
 * EQEs (stride 64, legacy payload at +32) and is 16 bytes wrong for 32-byte ones
 * (stride 32, legacy payload at +0).  This hardware uses 64-byte EQEs, so the
 * mistake was invisible here; it would have been a false "the ring never settles"
 * on a card that uses the 32-byte format.  One definition, used by both.
 */
#define MLX4_EQE_LEGACY_OFFSET(factor) ((factor) ? MLX4_EQ_ENTRY_SIZE : 0u)
#define MLX4_EQE_OWNER_BYTE(factor) \
    (MLX4_EQE_LEGACY_OFFSET(factor) + MLX4_EQE_OWNER_OFFSET)
/* Inside the cmd union member, which starts at 0x04. */
#define MLX4_EQE_CMD_TOKEN_OFFSET     0x06u   /* u16 BE */
#define MLX4_EQE_CMD_STATUS_OFFSET    0x0fu   /* u8 */
#define MLX4_EQE_CMD_OUT_PARAM_OFFSET 0x10u   /* u64 BE */
/*
 * The completion ("COMP") event and the queue number it names.
 *
 * mlx4_eqe is `reserved1, type, reserved2, subtype` (offsets 0..3) and then the payload union
 * at offset 4, whose `comp` member is a single __be32 cqn (device.h:893-900).  Type 0x00 is
 * MLX4_EVENT_TYPE_COMP (device.h:293) -- a CQ event, not an empty slot, which is the mistake
 * the code beside m3eqpoll records: stepping over type 0 as if it were nothing meant a
 * completion from one command was reported as another's.
 *
 * This is the EQE the completion queue's arm doorbell produces, so it is the one fact that
 * separates "the arm reached the device" from "the doorbell went nowhere" without needing an
 * interrupt session: the hardware writes it into the host-memory EQ ring, which this driver
 * can poll (docs/M2B-NOTES.md section 52, experiment 1).
 */
#define MLX4_EVENT_TYPE_COMP          0x00u   /* device.h:293 */
#define MLX4_EQE_COMP_CQN_OFFSET      0x04u   /* BE32, event.comp.cqn */

#define MLX4_EVENT_TYPE_PORT_CHANGE   0x09u   /* device.h:307 */
#define MLX4_EVENT_TYPE_CMD           0x0au   /* device.h:310 */

/* MTT entries (device.h:403, mlx4_mr.c:697-721). */
#define MLX4_MTT_FLAG_PRESENT   1ull
#define MLX4_MTT_ENTRY_SIZE     8u

/*
 * UAR doorbell arithmetic (mlx4_eq.c:920-939).  Each UAR page holds four EQ
 * doorbells, and the EQ doorbell for EQN n is at
 *
 *     BAR2 + ((n / 4) << uar_page_shift) + 0x800 + 8 * (n % 4)
 *
 * uar_page_shift is PAGE_SHIFT unless the 4 KiB-UAR build option is on
 * (mlx4_main.c:434-440).
 */
#define MLX4_UAR_PAGE_SHIFT        12u
#define MLX4_EQ_DOORBELL_OFFSET    0x800u

/* ==================================================================== */
/* M2d: context memory (ICM) and INIT_HCA                               */
/* ==================================================================== */

/*
 * Opcodes and sizes.  FreeBSD cmd.h:55-71, mlx4.h:91-104, mlx4_icm.c:52.
 * MAP_ICM/MAP_FA/RUN_FW/SET_ICM_SIZE are already defined above.
 */
#define MLX4_CMD_INIT_HCA           0x7u    /* cmd.h:55 */
#define MLX4_CMD_UNMAP_ICM          0xff9u  /* cmd.h:68 */
#define MLX4_CMD_UNMAP_ICM_AUX      0xffbu  /* cmd.h:70 */
/*
 * SET_MCAST_FLTR (0x48) -- the port's multicast filter table, and the only mechanism that
 * matters under A0 steering.
 *
 * That last part was checked rather than assumed: mlx4_multicast_attach() returns 0 WITHOUT
 * SENDING ANYTHING when the steering mode is A0 and the protocol is Ethernet
 * (mlx4_core/mlx4_mcg.c:1408-1417), so on this card -- which the driver verifies is in A0 via
 * QUERY_HCA -- the group addresses are programmed only through this command.  The hash is the
 * FIRMWARE's job: the driver hands over a MAC address and the mode.
 *
 * The parameter is `mac | (clear << 63)` and the mode goes in the op_modifier with the port in
 * the in_modifier (mlx4_core/mlx4_port.c:1716-1722, the wrapper the Ethernet driver calls).
 */
#define MLX4_CMD_SET_MCAST_FLTR     0x48u   /* cmd.h:142 */
#define MLX4_CMD_SET_VLAN_FLTR      0x47u   /* cmd.h:141 */
#define MLX4_MCAST_CONFIG           0u      /* en_port.h:51 -- add an address to the table */
#define MLX4_MCAST_DISABLE          1u      /* en_port.h:52 -- stop filtering through it */
#define MLX4_MCAST_ENABLE           2u      /* en_port.h:53 -- start filtering through it */
/* clear is bit 63 of the parameter (mlx4_port.c:1719). */
#define MLX4_MCAST_CLEAR_BIT        63u

#define MLX4_CMD_TIME_CLASS_B       60000u  /* cmd.h:177 */
#define MLX4_CMD_TIME_CLASS_C       60000u  /* cmd.h:178 */

/*
 * The HCA's context memory is divided into tables, and the host maps them in
 * 256 KiB chunks (MLX4_TABLE_CHUNK_SIZE, mlx4_icm.c:52).  A chunk is what one
 * MAP_ICM call describes.
 */
#define MLX4_TABLE_CHUNK_SIZE       (1u << 18)
#define MLX4_NUM_PDS                (1u << 15)   /* mlx4.h:91 */

/* cMPT layout: four sub-tables inside the cMPT region (mlx4.h:95-104). */
#define MLX4_CMPT_TYPE_QP           0
#define MLX4_CMPT_TYPE_SRQ          1
#define MLX4_CMPT_TYPE_CQ           2
#define MLX4_CMPT_TYPE_EQ           3
#define MLX4_CMPT_NUM_TYPE          4    /* mlx4.h:99-104: the type count */
#define MLX4_CMPT_SHIFT             24

/* MTT entries per segment (mlx4.h:87); the profile multiplies by 1<<this. */
#define MLX4_MTT_ENTRY_PER_SEG      8

/*
 * QUERY_DEV_CAP offsets this port did not previously name.  FreeBSD
 * mlx4_core/mlx4_fw.c:736-815 -- same values as mainline fw.c, and the ones the
 * tool already prints decode coherently, which is the cross-check that the
 * outbox is being read at the right offsets.
 */
#define QUERY_DEV_CAP_RSVD_EEC_OFFSET           0x16u  /* u8, small count */
#define QUERY_DEV_CAP_MAX_EEC_OFFSET            0x17u  /* u8, log2 */
#define QUERY_DEV_CAP_MAX_MRW_SZ_OFFSET         0x21u  /* raw u8; reference names offset only */
#define QUERY_DEV_CAP_MAX_AV_OFFSET             0x27u  /* u8, log2 */
#define QUERY_DEV_CAP_MAX_REQ_QP_OFFSET         0x29u  /* u8, log2 */
#define QUERY_DEV_CAP_MAX_RES_QP_OFFSET         0x2bu  /* u8, log2 */
#define QUERY_DEV_CAP_RSS_OFFSET                0x2eu  /* u8 */
#define QUERY_DEV_CAP_MAX_RDMA_OFFSET           0x2fu  /* u8, log2 */
#define QUERY_DEV_CAP_RSZ_SRQ_OFFSET            0x33u  /* u8 */
#define QUERY_DEV_CAP_PORT_BEACON_OFFSET        0x34u  /* u8 */
#define QUERY_DEV_CAP_ACK_DELAY_OFFSET          0x35u  /* u8 */
#define QUERY_DEV_CAP_RATE_SUPPORT_OFFSET       0x3cu  /* u16 BE */
#define QUERY_DEV_CAP_CQ_TS_SUPPORT_OFFSET      0x3eu  /* u8 */
#define QUERY_DEV_CAP_MAX_PKEY_OFFSET           0x3fu  /* u8 */
#define QUERY_DEV_CAP_BF_OFFSET                 0x4cu  /* u8 */
#define QUERY_DEV_CAP_LOG_BF_REG_SZ_OFFSET      0x4du  /* u8 */
#define QUERY_DEV_CAP_LOG_MAX_BF_REGS_PER_PAGE_OFFSET 0x4eu
#define QUERY_DEV_CAP_LOG_MAX_BF_PAGES_OFFSET   0x4fu
#define QUERY_DEV_CAP_MAX_SG_SQ_OFFSET          0x51u  /* u8 */
#define QUERY_DEV_CAP_MAX_DESC_SZ_SQ_OFFSET     0x52u  /* u16 BE */
#define QUERY_DEV_CAP_MAX_SG_RQ_OFFSET          0x55u  /* u8 */
#define QUERY_DEV_CAP_MAX_DESC_SZ_RQ_OFFSET     0x56u  /* u16 BE */
#define QUERY_DEV_CAP_SVLAN_BY_QP_OFFSET        0x5du  /* u8 */
#define QUERY_DEV_CAP_MAX_QP_MCG_OFFSET         0x61u  /* u8, log2 */
#define QUERY_DEV_CAP_RSVD_MCG_OFFSET           0x62u  /* u8 */
#define QUERY_DEV_CAP_MAX_MCG_OFFSET            0x63u  /* u8, log2 */
#define QUERY_DEV_CAP_RSVD_PD_OFFSET            0x64u  /* u8 */
#define QUERY_DEV_CAP_MAX_PD_OFFSET             0x65u  /* u8, log2 */
#define QUERY_DEV_CAP_RSVD_XRC_OFFSET           0x66u  /* u8 */
#define QUERY_DEV_CAP_MAX_XRC_OFFSET            0x67u  /* u8, log2 */
#define QUERY_DEV_CAP_MAX_COUNTERS_OFFSET       0x68u  /* u32 BE, gated by COUNTERS */
#define QUERY_DEV_CAP_EXT_2_FLAGS_OFFSET        0x70u  /* u32 BE */
#define QUERY_DEV_CAP_FLOW_STEERING_IPOIB_OFFSET 0x74u
#define QUERY_DEV_CAP_FLOW_STEERING_RANGE_EN_OFFSET 0x76u
#define QUERY_DEV_CAP_FLOW_STEERING_MAX_QP_OFFSET 0x77u
#define QUERY_DEV_CAP_SL2VL_EVENT_OFFSET        0x78u
#define QUERY_DEV_CAP_ECN_QCN_VER_OFFSET        0x7bu

/* Entry sizes: u16 BE each (mlx4_fw.c:801-810).  These are what the ICM
 * profiles are computed from, so a wrong one silently sizes every table. */
#define QUERY_DEV_CAP_RDMARC_ENTRY_SZ_OFFSET    0x80u
#define QUERY_DEV_CAP_QPC_ENTRY_SZ_OFFSET       0x82u
#define QUERY_DEV_CAP_AUX_ENTRY_SZ_OFFSET       0x84u
#define QUERY_DEV_CAP_ALTC_ENTRY_SZ_OFFSET      0x86u
#define QUERY_DEV_CAP_EQC_ENTRY_SZ_OFFSET       0x88u
#define QUERY_DEV_CAP_CQC_ENTRY_SZ_OFFSET       0x8au
#define QUERY_DEV_CAP_SRQ_ENTRY_SZ_OFFSET       0x8cu
#define QUERY_DEV_CAP_C_MPT_ENTRY_SZ_OFFSET     0x8eu
#define QUERY_DEV_CAP_MTT_ENTRY_SZ_OFFSET       0x90u
#define QUERY_DEV_CAP_D_MPT_ENTRY_SZ_OFFSET     0x92u
#define QUERY_DEV_CAP_BMME_FLAGS_OFFSET         0x94u  /* u32 BE */
#define QUERY_DEV_CAP_ETH_BACKPL_OFFSET         0x9cu  /* u32 BE */
#define QUERY_DEV_CAP_MAX_ICM_SZ_OFFSET         0xa0u  /* u64 BE, bytes */
#define QUERY_DEV_CAP_VXLAN                     0x9eu
#define QUERY_DEV_CAP_FW_REASSIGN_MAC           0x9du

/* flags2 bits this port needs (FreeBSD device.h:193-210). */
#define MLX4_DEV_CAP_FLAG2_RSS                  (1ull << 0)
#define MLX4_DEV_CAP_FLAG2_TS                   (1ull << 5)
#define MLX4_DEV_CAP_FLAG2_VXLAN_OFFLOADS       (1ull << 10)
#define MLX4_DEV_CAP_FLAG2_SYS_EQS              (1ull << 17)
#define MLX4_DEV_CAP_FLAG2_RECOVERABLE_ERROR_EVENT (1ull << 20)
/* flags bit for memory windows (device.h:169). */
#define MLX4_DEV_CAP_FLAG_MEM_WINDOW            (1ull << 16)

/*
 * INIT_HCA input mailbox (0x200 bytes).  FreeBSD mlx4_fw.c:1834-1878.  The
 * offsets are byte offsets into the inbox and every value is big-endian; most
 * are single bytes, which is why this is written with a byte-store helper
 * rather than by filling a struct.
 */
#define INIT_HCA_IN_SIZE                    0x200u
#define INIT_HCA_VERSION_OFFSET             0x000u
#define INIT_HCA_VERSION                    2u
#define INIT_HCA_CACHELINE_SZ_OFFSET        0x00eu
#define INIT_HCA_RECOVERABLE_ERROR_EVENT_OFFSET 0x018u
#define INIT_HCA_FLAGS_OFFSET               0x014u
#define INIT_HCA_QPC_OFFSET                 0x020u
#define INIT_HCA_QPC_BASE_OFFSET            (INIT_HCA_QPC_OFFSET + 0x10)
#define INIT_HCA_LOG_QP_OFFSET              (INIT_HCA_QPC_OFFSET + 0x17)
#define INIT_HCA_SRQC_BASE_OFFSET           (INIT_HCA_QPC_OFFSET + 0x28)
#define INIT_HCA_LOG_SRQ_OFFSET             (INIT_HCA_QPC_OFFSET + 0x2f)
#define INIT_HCA_CQC_BASE_OFFSET            (INIT_HCA_QPC_OFFSET + 0x30)
#define INIT_HCA_LOG_CQ_OFFSET              (INIT_HCA_QPC_OFFSET + 0x37)
#define INIT_HCA_EQE_CQE_OFFSETS            (INIT_HCA_QPC_OFFSET + 0x38)
#define INIT_HCA_EQE_CQE_STRIDE_OFFSET      (INIT_HCA_QPC_OFFSET + 0x3b)
#define INIT_HCA_ALTC_BASE_OFFSET           (INIT_HCA_QPC_OFFSET + 0x40)
#define INIT_HCA_AUXC_BASE_OFFSET           (INIT_HCA_QPC_OFFSET + 0x50)
#define INIT_HCA_EQC_BASE_OFFSET            (INIT_HCA_QPC_OFFSET + 0x60)
#define INIT_HCA_LOG_EQ_OFFSET              (INIT_HCA_QPC_OFFSET + 0x67)
#define INIT_HCA_NUM_SYS_EQS_OFFSET         (INIT_HCA_QPC_OFFSET + 0x6a)
#define INIT_HCA_RDMARC_BASE_OFFSET         (INIT_HCA_QPC_OFFSET + 0x70)
#define INIT_HCA_LOG_RD_OFFSET              (INIT_HCA_QPC_OFFSET + 0x77)
#define INIT_HCA_MCAST_OFFSET               0x0c0u
#define INIT_HCA_MC_BASE_OFFSET             (INIT_HCA_MCAST_OFFSET + 0x00)
#define INIT_HCA_LOG_MC_ENTRY_SZ_OFFSET     (INIT_HCA_MCAST_OFFSET + 0x12)
#define INIT_HCA_LOG_MC_HASH_SZ_OFFSET      (INIT_HCA_MCAST_OFFSET + 0x16)
#define INIT_HCA_UC_STEERING_OFFSET         (INIT_HCA_MCAST_OFFSET + 0x18)
#define INIT_HCA_LOG_MC_TABLE_SZ_OFFSET     (INIT_HCA_MCAST_OFFSET + 0x1b)

/*
 * The steering-mode bit, and why the port has to look at it.
 *
 * For B0 the reference writes (1 << 3) into the byte at MCAST+0x18 -- mlx4_fw.c:2008-2010:
 *
 *     if (dev->caps.steering_mode == MLX4_STEERING_MODE_B0)
 *             MLX4_PUT(inbox, (u8)(1 << 3), INIT_HCA_UC_STEERING_OFFSET);
 *
 * and mlx4_QUERY_HCA reads the mode back from exactly that bit, after first checking
 * the device-managed flag (mlx4_fw.c:2097-2105):
 *
 *     if (flags & (1 << INIT_HCA_DEVICE_MANAGED_FLOW_STEERING_EN))
 *             DEVICE_MANAGED;
 *     else if (outbox[INIT_HCA_UC_STEERING_OFFSET] & 0x8)
 *             B0;
 *     else
 *             A0;
 *
 * This port wrote the MC base, entry size, hash size and table size but never that
 * byte, so whatever it intended, the HCA was told A0 -- and A0 is the mode where
 * mlx4_SET_PORT_qpn_calc() actually sends a command (mlx4_port.c:1588 returns early
 * for anything else) and where a unicast MAC's QPN is base_qpn + mac_index.  Getting
 * this wrong does not fail loudly; it makes the receive steering quietly not match.
 */
#define INIT_HCA_UC_STEERING_B0_BIT         0x08u
#define INIT_HCA_DEVICE_MANAGED_FLOW_STEERING_EN 0x6u

#define INIT_HCA_TPT_OFFSET                 0x0f0u
#define INIT_HCA_DMPT_BASE_OFFSET           (INIT_HCA_TPT_OFFSET + 0x00)
#define INIT_HCA_TPT_MW_OFFSET              (INIT_HCA_TPT_OFFSET + 0x08)
#define INIT_HCA_LOG_MPT_SZ_OFFSET          (INIT_HCA_TPT_OFFSET + 0x0b)
#define INIT_HCA_MTT_BASE_OFFSET            (INIT_HCA_TPT_OFFSET + 0x10)
#define INIT_HCA_CMPT_BASE_OFFSET           (INIT_HCA_TPT_OFFSET + 0x18)
#define INIT_HCA_UAR_OFFSET                 0x120u
#define INIT_HCA_LOG_UAR_SZ_OFFSET          (INIT_HCA_UAR_OFFSET + 0x0a)
#define INIT_HCA_UAR_PAGE_SZ_OFFSET         (INIT_HCA_UAR_OFFSET + 0x0b)

/* MLX4_MAX_EQ_NUM / MAX_MSIX: device.h:51-52 and mlx4.h. */
#define MLX4_MAX_MSIX                       64u
#define MLX4_MAX_EQ_NUM                     1024u   /* device.h:109 */

/* dev_cap->flags bits INIT_HCA consults (device.h:164-189). */
#define MLX4_DEV_CAP_FLAG_IPOIB_CSUM        (1ull << 7)
#define MLX4_DEV_CAP_FLAG_COUNTERS          (1ull << 48)
#define MLX4_DEV_CAP_FLAG_RSS_IP_FRAG       (1ull << 52)
#define MLX4_DEV_CAP_FLAG_64B_EQE           (1ull << 61)
#define MLX4_DEV_CAP_FLAG_64B_CQE           (1ull << 62)
/* BMME flags (device.h:279) and the memory-window enable (mlx4.h:67). */
#define MLX4_BMME_FLAG_TYPE_2_WIN           (1u << 9)
#define INIT_HCA_TPT_MW_ENABLE              (1u << 7)

/* Steering modes (device.h:123-125) and the multicast entry size (mlx4.h:83). */
#define MLX4_STEERING_MODE_A0               0
#define MLX4_STEERING_MODE_B0               1
#define MLX4_STEERING_MODE_DEVICE_MANAGED   2
#define MLX4_DEFAULT_MGM_LOG_ENTRY_SIZE     10      /* 1024-byte MCG entries */

/* ---- PCI Express capability register offsets inside the cap block -- */
#define PCI_CAP_ID_EXP          0x10
#define PCI_EXP_DEVCTL          0x08
#define PCI_EXP_LNKCTL          0x10

#endif /* CX3_MLX4_REGS_H */
