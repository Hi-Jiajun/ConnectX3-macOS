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

#ifndef CX3_ABI_H
#define CX3_ABI_H

#include <stddef.h>
#include <stdint.h>

/*
 * Shared ABI between the Cx3Mlx4 kext and its user-space control tool.
 * Plain C, no IOKit dependencies, so both sides can include it.
 *
 * Design rule for this driver, learned the hard way on 2026-10-10: nothing in
 * IOKit start() may write to the device or allocate DMA.  A hang there costs
 * the whole boot, and because the card keeps its wedged state across warm
 * resets it can cost the following boots too.  All of that work therefore lives
 * behind these selectors, so the machine stays bootable and each step can be
 * bisected without rebooting.
 *
 * Layout of the boot-to-command decoupling:
 *   start()      -- PCI enable, BAR map, read-only register probe.  No writes,
 *                   no DMA.  This is the configuration already proven to boot
 *                   (0.2.0 booted twice with exactly this work).
 *   selectors    -- everything else, run from user space on a live system.
 */

enum {
    kCx3SelProbe    = 0,  /* read-only: registers + gates. 	             */
    kCx3SelDma      = 1,  /* allocate the command mailbox only. NO device access */
    kCx3SelQueryFw  = 2,  /* HCR handshake + QUERY_FW.  writes the HCR.        */
    kCx3SelResetHca = 3,  /* HCA reset.  can require a power cycle.            */
    kCx3SelPeek     = 4,  /* read-only: dump any register range.               */
    kCx3SelClearOwner = 5,/* write 0 to the ownership word (mlx4_free_ownership) */
    kCx3SelPoke     = 6,  /* single guarded 32-bit store + read-back.           */
    kCx3SelFlr      = 7,  /* PCIe Function Level Reset -- hardware reset.       */
    kCx3SelQueryDevCap = 8,/* QUERY_DEV_CAP: the device's capability set.        */
    kCx3SelIcmAlloc = 9,  /* allocate the FW/ICM area, or free it.              */
    kCx3SelFwLoad   = 10, /* M2b: allocate the FW area, MAP_FA, RUN_FW, MOD_STAT_CFG */
    kCx3SelFwArea   = 11, /* read-only: what is actually IN the FW area */
    kCx3SelFwOutbox = 12, /* QUERY_FW whose outbox lives in the FW area */
    kCx3SelM2dProfile = 13, /* QUERY_DEV_CAP decode + mlx4_make_profile */
    kCx3SelM2dBringUp = 14, /* SET_ICM_SIZE, ICM tables, INIT_HCA */
    kCx3SelM3EqUp   = 15, /* QUERY_FUNC + one EQ: MTT, SW2HW_EQ, MAP_EQ, arm */
    kCx3SelM3EqPoll = 16, /* poll the EQ ring, and optionally fire a test NOP */
    kCx3SelM3EqRing = 17, /* QUERY_FW whose outbox is an EQ ring page */
    kCx3SelM3EqInfo = 18, /* MTT readback + QUERY_EQ */
    kCx3SelM3IrqUp  = 19, /* open an interrupt session (workloop + event source) */
    kCx3SelM3IrqTest = 20,/* register a token and post the event-mode NOP */
    kCx3SelM3IrqStatus = 21,/* what the interrupt path has seen */
    kCx3SelM3IrqDown = 22, /* close the session */
    kCx3SelM3ClrInt = 23, /* set up the legacy interrupt-clear register */
    kCx3SelM3QueryPort = 24, /* R3: QUERY_PORT, read-only, raw + decoded */
    kCx3SelM3PortUp   = 25, /* R3: the gated SET_PORT general + INIT_PORT sequence */
    kCx3SelM3AbRing   = 26, /* the ring-initialisation A/B experiment */
    kCx3SelM3QueryHca = 27, /* R4 entrance condition: what mode did INIT_HCA actually set? */
    kCx3SelR4Mr       = 28, /* R4 stage 1: a PD and one physical memory region */
    kCx3SelR4Cq       = 29, /* R4 stage 2: one completion queue */
    kCx3SelR4Rq       = 30, /* R4 stage 3: the receive ring's buffers and WQEs */
    kCx3SelHcr        = 31, /* the general-purpose command runner (gated) */
    kCx3SelSurvey     = 32, /* one call, the whole current state */
    /*
     * The command-timeout mechanism's own fault injector (docs/LOCK-IMPL.md).
     *
     * It exists because the failure it tests cannot be produced on demand: the 2026-10-12
     * hang that held the transaction lock for 6 min 07.6 s happened once, by accident, and
     * cost a boot.  The two things that need to be observable -- "a poisoned mailbox makes
     * a command fail at once instead of waiting" and "the read-only selectors are still
     * answerable while a selector holds the lock" -- are otherwise only testable by
     * breaking the card again.
     *
     * It cannot make the device do anything it was not already doing: mode 1 stops commands
     * (the safe direction), mode 2 only clears that, and mode 3 sleeps inside a selector to
     * hold the lock.  Mode 2 refuses while the command register still reads busy, because
     * then the poison is not an injection and clearing it would post over a stuck command.
     */
    kCx3SelFault      = 33,
    /* R4 stage 4: the RX QP -- RST2INIT, INIT2RTR, RTR2RTS, then the producer publish. */
    kCx3SelR4Qp       = 34,
    /*
     * R4 stage 5: the port's MAC table and the single-ring RSS indirection QP.  Two halves
     * that share one selector because they share one verdict -- "packets can now be steered
     * to the queue stage 4 built" -- and neither half is useful alone: a registered MAC with
     * no hash target, or a hash target for an address the firmware does not recognise.
     */
    kCx3SelR4Ingress  = 35,
    /*
     * R4 stage 6: the transmit side's resources -- a completion queue, a send QP walked to
     * RTS, and the send queue's memory.  Deliberately NOT a transmit: no frame is posted and
     * no doorbell is rung, so every judgement here is about resources that EXIST and about
     * numbers that are where they were put.
     *
     * The number space is the part of this stage with a constraint outside itself.  A0 maps
     * MAC table index i to `base_qpn + i` (mlx4_en_netdev.c:573-577), the base is a parameter
     * this driver will hand the firmware with SET_PORT qpn_calc (mlx4_port.c:1578-1600), and
     * this project's non-promiscuous plan puts that base at `num_qps - (1 << log_num_macs)`
     * so that the 128 MAC-derived QPNs land on the TOP of the space instead of on our queues
     * (docs/M2B-NOTES.md section 41.5, option R).  So this stage must not place anything at or
     * above that line, or the option is dead before it is tried: `topReservedStart` is that
     * line, computed from two runtime values, and both of this stage's numbers are checked
     * against it.
     */
    kCx3SelR4Tx       = 36,
    /*
     * R4 stage 9: the first selector that makes a byte move.
     *
     * Everything before it measured resources and their state; this one posts a work request,
     * rings the send doorbell, and reads completion elements -- on the transmit side and on
     * the receive side, which is why its scalars are in two blocks.  See the definition below
     * for the six judgements and for why a failure of one half must not hide the other.
     */
    kCx3SelR4Raw      = 37,
    /*
     * kCx3SelTxProbe -- the three discriminators of docs/M2B-NOTES.md section 52, which
     * exist because every field of the send path and every way of delivering the send
     * doorbell had been checked or falsified and the transmit still did nothing.
     *
     * It is a SEPARATE selector rather than part of r4raw for two reasons.  First, r4raw's
     * scalar block is full at sixteen (docs/DEFECTS.md F13 is that cap), and a verifier that
     * decodes its own ABI structures is one more place to be wrong -- so these facts have to
     * be scalars.  Second, r4raw is the one step in the chain that FAILS, and its behaviour
     * is what the last four boots were read against: a probe inside it could only make the
     * comparison harder.
     *
     * One call makes three measurements, in this order, and stops as soon as one of them
     * answers:
     *
     *   1. NOP versus SEND.  The same send queue, the same doorbell, the same wait, the same
     *      32-byte descriptor -- only the opcode byte differs (0x00 against 0x0a).  A NOP
     *      carries no data and still generates a completion, so if the NOP completes, the
     *      hardware IS fetching and executing this queue's descriptors and the defect is
     *      specific to the SEND data path; if it does not, the hardware is not executing
     *      anything from this queue and no field of the descriptor can be the cause.
     *   2. The receive completion queue's arm doorbell, through UAR page 128 at
     *      MLX4_CQ_DOORBELL (0x20, an 8-byte write, mlx4_cq_arm at cq.h:138-161).  Its
     *      success is observable without an interrupt session: the hardware writes a COMP
     *      EQE (device.h:293) into the host-memory EQ ring, which this driver can poll.  This
     *      is what separates "the doorbell never reaches the device" from "the doorbell
     *      arrives and the send queue is what says no" -- the two halves of section 51.5.
     *   3. The doorbell record.  The public PRM's posting procedure (section 7.4.3.1) updates
     *      it between the WQE write and the doorbell, and notes that the receive and send
     *      records are CONSECUTIVE in memory; no implementation writes the send half for a
     *      send queue (section 51.4).  Four variants are tried -- nothing, word 0, word 1,
     *      both -- and the first one whose post completes is reported.
     *
     * What it does NOT do, on purpose: it sends no firmware command, so it cannot poison the
     * mailbox (docs/LOCK-IMPL.md) and it cannot leave the device needing a reset.  It rings
     * doorbells, writes the queue's own stamps and its own record, and consumes at most one
     * EQE.
     */
    kCx3SelTxProbe    = 38,
    /*
     * kCx3SelNetUp -- stage 10a: publish an IOEthernetController interface for a device the
     * chain has already brought up, so that the card becomes something the system can see
     * (`ifconfig`), with the MAC the firmware reports and the link state we set.
     *
     * It is a selector rather than work in start() because the bring-up itself is driven from
     * user space by this project's own tooling, and start() is held read-only by a build gate:
     * no device write, no DMA allocation, nothing that can wedge a boot.  Attaching here keeps
     * that property and makes the interface appear exactly when the device is ready for it.
     *
     * This is 10a: it wires the metadata and the link, and deliberately does NOT touch mbufs.
     * The receive and transmit data paths of the plan's section 5 are 10b.
     */
    kCx3SelNetUp      = 39,
    /*
     * kCx3SelNetStat -- what the network data path has actually done.
     *
     * Read-only, and it exists because of what the first version of that path could not do:
     * its counters were visible only in the kernel log (a once-a-second heartbeat line and the
     * first frames), which means every question about it -- did frames arrive, did they reach
     * the stack, how many were dropped and by which check, is the timer even firing -- cost a
     * log scrape whose window is as long as you happened to start `log stream`.  Numbers a
     * caller can ask for on demand are what makes the next iteration cheap, which is the whole
     * reason this project has selectors at all.
     */
    kCx3SelNetStat    = 40,
    /*
     * kCx3SelNetLink -- the media table and the link reading (M5-1).
     *
     * Its own selector rather than work in netup, for one concrete reason: publishing a media
     * dictionary changes the link report the M4 acceptance depends on, and M4 has not been
     * verified on hardware yet.  A mode input makes that a choice instead of a bet -- the
     * M4 test runs with the driver's behaviour unchanged, and `cx3ctl netlink publish` turns the
     * dictionary on afterwards WITHOUT another reboot.  Same reasoning as cx3raw/cx3quarfree,
     * except that this one does not need a boot argument because it changes nothing until asked.
     */
    kCx3SelNetLink    = 41,
    /*
     * kCx3SelNetOff -- the offload switches (M5-3 onwards), same runtime-gate reasoning as
     * netlink: the receive path's checksum marking is per frame, so a wrong interpretation
     * would corrupt or drop frames in the very test M4 has not run yet.  Default OFF means the
     * stack computes every checksum itself, which is exactly today's behaviour.
     */
    kCx3SelNetOff     = 42,
    /*
     * kCx3SelNetArm -- the arm experiment done properly (M5-2's prerequisite).
     *
     * Stage 9 armed the receive CQ and measured ZERO EQEs, but the experiment was then shown to
     * be invalid for a reason of its own: the queue was never empty when it was armed, and the
     * arm's semantics are "notify when something NEW arrives" (docs/M5-REFERENCE.md section 5,
     * mlx4's own usage is drain-then-arm).  So the one link in the receive chain that this
     * project has never proved -- CQ arm -> COMP EQE -> event queue -- has no reading at all,
     * neither positive nor negative.
     *
     * This selector is that reading: drain the receive CQ (the completions are consumed and their
     * slots re-posted, exactly as the data path does), ARM it, and then look at the event queue
     * for a COMP event naming this CQ.  Everything it reports is about that: how many
     * completions were drained and whether the queue was empty when the arm happened (the arm is
     * only interpretable if it was), whether an EQE arrived at all, and what it named.
     */
    kCx3SelNetArm     = 43,
    /*
     * kCx3SelNetFilt -- the hardware receive filters (M5-8): the port's multicast table, and
     * the promise that was recorded but not kept.
     *
     * Today setMulticastList() accepts, records and says in the log that the hardware was NOT
     * told.  That was honest and it is a functional gap: macOS enables IPv6 by default, and
     * without the group addresses programmed, no IPv6 frame is ever delivered.
     *
     * What made this small enough to do is a fact that was checked rather than assumed:
     * mlx4_multicast_attach() does nothing at all for Ethernet under A0 steering
     * (mlx4_core/mlx4_mcg.c:1408-1417), and this card is in A0 (the driver reads QUERY_HCA and
     * says so).  So the whole mechanism is the port's multicast filter table, programmed with
     * SET_MCAST_FLTR, and the hash is the firmware's job.
     *
     * Runtime-gated like the others, with one difference: turning it ON replays the addresses
     * the stack has already asked for, because the stack only calls setMulticastList when its
     * list CHANGES -- so without a replay the switch would appear to do nothing until the user
     * changed something unrelated.
     */
    kCx3SelNetFilt    = 44,
    /*
     * kCx3SelNetPort -- stopping the port, which is the first half of any configuration change
     * that the port has to be re-opened for.
     *
     * The reference's own change_mtu is "stop the port, then start it" (mlx4_en_netdev.c:1839-
     * 1840), and this is the stop.  The start already exists here as the chain's port-up step,
     * which takes the MTU as an input -- so the missing piece for jumbo was one command, not a
     * second port bring-up.
     *
     * It refuses while the network data path is enabled: closing a port under a live interface
     * stops traffic, and a tool that did that to a running interface without saying so would be
     * the kind of silent state change this project keeps having to write rules against.
     */
    kCx3SelNetPort    = 45,
    /*
     * kCx3SelNetUpStop -- the interface step, cut short after its first step.
     *
     * This exists to answer one question with one run, and it is a separate selector rather than
     * a flag on netup so that netup's own contract is not touched by an experiment (the same
     * reason kCx3SelNetArm is separate from the interrupt session calls):
     *
     *   alive  -> everything netup does AFTER reading the port's address is what the machine
     *             cannot survive, and the per-step markers in netup then say which step;
     *   dead   -> entering the netup path at all is fatal, and the death is not in the attach
     *             or the timer or the frame ring.
     *
     * It reads nothing from the device: the port address must already be held (published by
     * m3portup or the standalone queryport selector), and if it is not, this refuses exactly as
     * netup does.  So a run of it cannot be confounded with the mailbox question.
     */
    kCx3SelNetUpStop  = 46,
    kCx3SelQueryAdapter = 47, /* QUERY_ADAPTER: full 256-byte outbox + decoded ID */
    kCx3SelPcieLink   = 48, /* bounded read-only conventional PCIe link sample */
    kCx3SelRuntimeIdentity = 49, /* host-only service/build/generation identity */
    kCx3SelModuleRead = 50, /* fixed MAD GET, page 0, EEPROM flat offsets 0..511 */
    kCx3SelAutoNetwork = 51, /* one bound transaction; no boot-time auto start */
    kCx3SelPortInfo = 52, /* per-port cached observation or fixed QUERY_PORT refresh */
    kCx3SelPortQueue = 53, /* per-port real queue setup/disposal */
    kCx3SelNativePort = 54, /* fixed native controller per physical port */
    kCx3SelNativeConfig = 55, /* port-local VLAN whitelist and checksum policy */
    kCx3SelCount = 56,
};


/* --------------------------------------------------------------------- */
/* scalar output layouts, one block per selector                          */
/* --------------------------------------------------------------------- */

/* kCx3SelProbe -- no inputs */
enum {
    kProbeOutStatus   = 0,  /* kIOReturn, 0 = ok */
    kProbeOutWindow   = 1,  /* 1 = the BAR0 window reads back live */
    kProbeOutOwner    = 2,
    kProbeOutHcrStat  = 3,
    kProbeOutResetSem = 4,
    kProbeOutPciCmd   = 5,
    kProbeOutBar0Phys = 6,
    kProbeOutMapFlags = 7,  /* bit0 = BAR0 mapped, bit1 = BAR2 mapped */
    /*
     * 1 = the command mailbox is poisoned, i.e. some command was abandoned and nothing is
     * posted until a reset.  It belongs in *probe* specifically: probe is the one call the
     * recovery path always makes first, and the reason a command just failed is the first
     * thing its reader needs (docs/LOCK-IMPL.md).
     */
    kProbeOutCmdWedged = 8,
    kProbeOutCount    = 9,
};

/* kCx3SelDma -- no inputs */
enum {
    kDmaOutStatus   = 0,
    kDmaOutPhys     = 1,
    kDmaOutSegCount = 2,
    kDmaOutLow4G    = 3,  /* 1 = address is below 4 GiB */
    kDmaOutOffset   = 4,
    kDmaOutCount    = 5,
};

/* kCx3SelQueryFw */
enum {
    kFwInForce = 0,   /* nonzero: bypass the ownership gate (see below) */
    kFwInCount = 1,
};
enum {
    kFwOutStatus    = 0,   /* kIOReturn, 0 = ok */
    kFwOutFwStatus  = 1,   /* status byte the HCA returned, 0 = success */
    kFwOutFwVer     = 2,   /* dev->caps.fw_ver, packed as in mlx4_fw.c */
    kFwOutCmdIfRev  = 3,
    kFwOutMaxCmds   = 4,
    kFwOutPpfId     = 5,
    kFwOutCatasOff  = 6,
    kFwOutCatasSize = 7,
    kFwOutCatasBar  = 8,
    kFwOutRaw0      = 9,   /* first four big-endian dwords of the outbox */
    kFwOutRaw1      = 10,
    kFwOutRaw2      = 11,
    kFwOutRaw3      = 12,
    kFwOutFwPages   = 13,  /* u16, in 4 KiB pages: the FW area to allocate */
    /*
     * 14 values, and the headroom matters: the raw outbox dwords 4..7 used to sit
     * here too, which made the count 18 and failed every QUERY_FW with
     * kIOReturnBadArgument, because the kernel caps scalar outputs at 16. They
     * existed only to eyeball offset 0x10, and fwPages now parses that offset
     * properly, so they are gone and four values of headroom remain.
     */
    kFwOutCount     = 14,
};

/* kCx3SelResetHca */
enum {
    kRstInConfirm = 0,   /* must be 1; guards an irreversible action */
    kRstInBypassSem = 1, /* 1 = skip the semaphore gate (see resetHca) */
    kRstInCount   = 2,
};
enum {
    kRstOutStatus      = 0,
    kRstOutOwnerBefore = 1,
    kRstOutOwnerAfter  = 2,
    /*
     * 1 = the mailbox was poisoned *before* this reset ran.  Reported from the reset's own
     * output rather than left to be inferred from the log, because it is the fact that
     * says whether this reset was also a recovery (docs/LOCK-IMPL.md, question 3).
     */
    kRstOutCmdWedged   = 3,
    kRstOutCount       = 4,
};

/*
 * kCx3SelPeek -- read-only window onto the whole address space.
 *
 * This exists so that register exploration stops costing a reboot each time.
 * Everything else needs a new kext binary (new cdHash) and therefore a reboot,
 * so the ability to read any offset interactively is the highest-leverage thing
 * to have in the build.
 */
enum {
    kPeekSpaceBar0 = 0,
    kPeekSpaceBar2 = 1,
    kPeekSpaceConfig = 2,
};
enum {
    kPeekInSpace = 0,
    kPeekInOffset = 1,
    kPeekInCount = 2,    /* dwords to read, 1..kPeekMaxDwords */
    kPeekInCount_ = 3,
};
enum {
    kPeekMaxDwords = 14,
    kPeekOutStatus = 0,  /* kIOReturn, 0 = ok */
    kPeekOutCount = 1,   /* dwords actually returned */
    kPeekOutData0 = 2,   /* raw dwords, no byte swapping */
    kPeekOutCount_ = kPeekOutData0 + kPeekMaxDwords,
};

/*
 * kCx3SelClearOwner -- clears the ownership word.
 *
 * This is byte-for-byte what FreeBSD mlx4_free_ownership() does
 * (mlx4_main.c:3083): writel(0, owner); msleep(1000); and it is called by the
 * reference in three "hand the device back" paths (SRIOV handover, load-failure
 * cleanup, unload).  It is a single 32-bit store to one register, no handshake
 * and no polling, which is why it is safe to offer before the command path.
 *
 * Note on risk: posted MMIO writes complete without waiting for the device, so
 * a write is intrinsically less able to hang the CPU than a read.  Reads to this
 * device already complete normally (the boot probe reads every 4 KiB page of
 * BAR0), which is evidence the device answers.
 */
enum {
    kClrInConfirm = 0,   /* must be 1 */
    kClrInCount = 1,
};
enum {
    kClrOutStatus = 0,
    kClrOutBefore = 1,
    kClrOutAfter = 2,
    kClrOutCount = 3,
};

/*
 * kCx3SelPoke -- one 32-bit store with read-back, for determining whether BAR0
 * writes reach the device at all.
 *
 * Needed because clearOwner() (a write of 0 to the ownership word) had no
 * effect, and that has two very different explanations: the address is
 * read-only, or none of our writes are landing.  A second address settles it.
 *
 * Refused by the kext, at any privilege level:
 *   0x00f010  the HCA reset trigger (writing the reset value reboots the HCA)
 *   0x080698  the HCR control dword (setting the GO bit posts a firmware command)
 * Those two turn a diagnostic into an action, so they stay out of this selector.
 */
enum {
    kPokeInSpace = 0,
    kPokeInOffset = 1,
    kPokeInValue = 2,
    kPokeInConfirm = 3,   /* must be 1 */
    kPokeInCount = 4,
};
enum {
    kPokeOutStatus = 0,
    kPokeOutBefore = 1,
    kPokeOutAfter = 2,
    kPokeOutValue = 3,
    kPokeOutCount = 4,
};

/*
 * kCx3SelFlr -- PCIe Function Level Reset.
 *
 * Why this and not the vendor reset at 0xf0010: the vendor reset is guarded by a
 * firmware semaphore that this card holds permanently, and overriding that guard
 * is a firmware-level protocol violation.  FLR is the mechanism the PCIe
 * specification defines for exactly this situation -- a function whose driver is
 * stuck -- it is executed by the hardware, and this card advertises support for
 * it (Device Capabilities bit 28 is set: 0x11d08e02).
 *
 * Safety measures, because an FLR can invalidate our BAR mapping:
 *   - platform FunctionReset with OptionNone replaces a hand-written FLR bit
 *   - config identity/BAR checks are diagnostic, never a release credential
 *   - no old MMIO mapping is used; owner is the unread sentinel 0xffffffff
 *   - commands remain blocked after an attempted platform reset; no recovery
 *     generation/scope credential is emitted by this legacy result
 *   - the FLR support bit is checked before anything is written
 *   - the caller must pass the confirmation scalar
 */
enum {
    kFlrInConfirm = 0,     /* must be 1 */
    kFlrInCount = 1,
};
enum {
    kFlrOutStatus     = 0,
    kFlrOutDevCaps    = 1,   /* Device Capabilities as read */
    kFlrOutSupported  = 2,   /* 1 if bit 28 was set */
    kFlrOutDevCtlPre  = 3,
    kFlrOutBar0Before = 4,
    kFlrOutBar0After  = 5,
    kFlrOutBarStable  = 6,   /* 1 if both BAR0 config dwords survived the reset */
    kFlrOutVendorId   = 7,   /* re-read after the reset, to see it came back */
    kFlrOutDeviceId   = 8,
    kFlrOutOwner      = 9,   /* 0xffffffff = not read; BAR stability is not MMIO trust */
    kFlrOutCount      = 10,
};

/*
 * kCx3SelQueryDevCap -- QUERY_DEV_CAP (0x3), the device's capability set.
 *
 * The raw 4 KiB outbox comes back rather than parsed fields.  Two reasons: the
 * capability layout is where all the downstream decisions live (page size, how
 * many QPs/CQs/EQs, ICM parameters), so it is worth being able to see all of it
 * without another round trip; and returning raw bytes means adding a field later
 * does not change this ABI, and an ABI change costs a reboot here because a new
 * kext binary means a new cdHash and a rebuilt kernel collection.
 *
 * The tool parses it using the same offset constants the kext uses, from
 * src/mlx4_regs.h.
 */
enum {
    kDevCapInForce = 0,     /* nonzero: bypass the ownership gate */
    /*
     * nonzero: post the command even when the HCR still looks busy.
     *
     * This exists because "the register says a command is outstanding" and "the
     * device is still working on one" are not the same statement, and during the
     * M2b RUN_FW hang the difference is the whole question.  Deliberately named
     * for what it does rather than for a hypothesis; it is an experiment switch,
     * not a normal path.
     */
    kDevCapInIgnoreBusy = 1,
    kDevCapInCount = 2,
};
enum {
    kDevCapOutStatus = 0,   /* kIOReturn, 0 = ok */
    kDevCapOutFwStatus = 1, /* status byte the HCA returned */
    kDevCapOutCount = 2,
};

/*
 * The outbox travels as a structure, not as scalars.
 *
 * The scalar path is capped at kIOExternalMethodScalarOutputCountMax = 16, and
 * this result needs 24 dwords of outbox plus two status words.  Sending it as
 * scalars failed with kIOReturnBadArgument (0xe00002c2), which is how the limit
 * was discovered.  IOExternalMethodDispatch has checkStructureOutputSize for
 * exactly this, so bulk data goes there.
 *
 * Kept as a plain C struct so both the kext and the tool agree on the layout
 * without either one depending on the other's headers.
 */
/*
 * 64 dwords (256 bytes) covers the whole meaningful QUERY_DEV_CAP record: the
 * profile needs the entry sizes at 0x80-0x92 and max_icm_sz at 0xa0, which the
 * previous 24-word (96-byte) window silently cut off.  This travels as a
 * structure, so widening it is not the 16-value scalar limit.
 */
enum { kDevCapWords = 64 };
struct Cx3DevCapResult {
    uint64_t status;            /* kIOReturn, 0 = ok */
    uint64_t fwStatus;          /* status byte the HCA returned; 0xffffffff = not attempted */
    uint32_t words[kDevCapWords];  /* raw outbox dwords, host byte order */
};

/* QUERY_ADAPTER's complete outbox and the board-ID candidate decoded from it. */
enum { CX3_ADAPTER_RESULT_VERSION = 1, CX3_ADAPTER_RAW_BYTES = 256 };
struct Cx3AdapterResult {
    uint32_t version;
    uint32_t structBytes;
    uint64_t status;              /* kIOReturn payload; transport may still succeed */
    uint32_t fwStatus;            /* CX3_FWSTATUS_NOT_POSTED unless the command ran */
    uint32_t rawBytes;            /* 256 only after successful firmware completion */
    uint64_t deviceGeneration;    /* provider-local generation, not run/boot identity */
    uint32_t intaPin;             /* zero-extended byte, 0xff until command succeeds */
    uint32_t reserved;
    uint8_t boardId[64];          /* decoded board ID / PSID candidate, not confirmed PSID */
    uint8_t raw[CX3_ADAPTER_RAW_BYTES];
};

/* Module EEPROM via mlx4_port.c:mlx4_get_module_info. Only fixed GETs, never
 * arbitrary MAD/I2C writes. Offset 0..255 selects 0x50, 256..511 selects 0x51;
 * page number is zero. Missing/high-page-unsupported is not a zero DOM sample.
 * The MAD response is retained on MAD-level rejection; data is valid only with
 * status/fwStatus/madStatus all zero and returnedBytes == requestedBytes. */
enum { CX3_MODULE_RESULT_VERSION = 1, CX3_MODULE_RAW_BYTES = 256,
       CX3_MODULE_MAX_READ = 48, CX3_MODULE_MAD_NOT_RETURNED = 0xffff };
static inline int cx3ModuleRangeValid(uint64_t port, uint64_t offset, uint64_t length)
{
    return port >= 1 && port <= 2 && offset < 512 &&
           length >= 1 && length <= CX3_MODULE_MAX_READ &&
           length <= 256 - (offset & 255);
}
struct Cx3ModuleResult {
    uint32_t version, structBytes;
    uint64_t status;
    uint32_t fwStatus, madStatus;
    uint64_t deviceGeneration;
    uint32_t port, offset, requestedBytes, returnedBytes;
    uint32_t i2cAddress, page, wireOffset, rawBytes;
    uint8_t raw[CX3_MODULE_RAW_BYTES];
    uint8_t data[CX3_MODULE_MAX_READ];
};

enum { CX3_PCIE_RESULT_VERSION = 1, CX3_PCIE_RAW_BYTES = 256, CX3_PCIE_VALID_BYTES = 32 };
enum {
    CX3_PCIE_REASON_OK = 0, CX3_PCIE_REASON_NOT_READY = 1,
    CX3_PCIE_REASON_RESET_BLOCKED = 2, CX3_PCIE_REASON_RETENTION_BLOCKED = 3,
    CX3_PCIE_REASON_READ_UNAVAILABLE = 4, CX3_PCIE_REASON_UNSUPPORTED_HEADER = 5,
    CX3_PCIE_REASON_NO_PCIE_CAP = 6, CX3_PCIE_REASON_MALFORMED_LIST = 7,
    CX3_PCIE_REASON_CYCLE = 8, CX3_PCIE_REASON_TRUNCATED_CAP = 9,
    CX3_PCIE_REASON_UNSUPPORTED_PCIE = 10, CX3_PCIE_REASON_INCONSISTENT = 11
};
enum {
    CX3_PCIE_VALID_IDENTITY_BEFORE = 1u << 0,
    CX3_PCIE_VALID_IDENTITY_AFTER = 1u << 1,
    CX3_PCIE_VALID_CONSISTENT = 1u << 2,
    CX3_PCIE_VALID_LINK_FIELDS = 1u << 3,
    CX3_PCIE_VALID_LNKCAP2 = 1u << 4
};
struct Cx3PcieIdentity {
    uint16_t vendor, device, subsystemVendor, subsystemId;
    uint8_t bus, deviceNumber, function, reserved;
};
struct Cx3PcieLinkResult {
    uint32_t version, structBytes;
    uint64_t status;
    uint32_t reason, validity;
    uint64_t deviceGeneration;
    uint32_t capOffset, walkCount, pcieFlags, lnkCap, lnkSta, lnkCap2;
    struct Cx3PcieIdentity identityBefore, identityAfter;
    /* First observed bytes, including all-ones failure observations. readValid
     * means sampled, not semantically valid. Unread bytes remain zero. */
    uint8_t raw[CX3_PCIE_RAW_BYTES], readValid[CX3_PCIE_VALID_BYTES];
};

enum { CX3_RUNTIME_RESULT_VERSION = 1 };
enum {
    CX3_RUNTIME_VALID_SERVICE = 1u << 0,
    CX3_RUNTIME_VALID_PROVIDER = 1u << 1,
    CX3_RUNTIME_VALID_PCI_SNAPSHOT = 1u << 2,
    CX3_RUNTIME_VALID_BUILD = 1u << 3,
    CX3_RUNTIME_VALID_GENERATION = 1u << 4,
    CX3_RUNTIME_VALID_UPTIME = 1u << 5
};
enum {
    CX3_RUNTIME_FLAG_RESET_BLOCKED = 1u << 0,
    CX3_RUNTIME_FLAG_STOP_RETAINED = 1u << 1,
    CX3_RUNTIME_FLAG_CMD_WEDGED = 1u << 2,
    CX3_RUNTIME_FLAG_QUAR_EXHAUSTED = 1u << 3,
    CX3_RUNTIME_FLAG_FW_RUN_ATTEMPTED = 1u << 4
};
/* Single-client network control. The provider transaction lock spans the entire
 * initialization, including all existing export methods. Status never touches
 * hardware. Enable is once per empty service instance; failures are sticky.
 * Disable pauses host delivery, not a DMA-revocation or hardware-close proof.
 * Only port 1 / standard MTU is supported by this first product entry. */
#define CX3_AUTO_NETWORK_VERSION 1u
#define CX3_AUTO_NETWORK_ALL_STAGES 0xffffull
#define CX3_AUTO_NET_ENABLED 1u
#define CX3_AUTO_NET_LINK_UP 2u
#define CX3_AUTO_NET_USER_DISABLED 4u
#define CX3_AUTO_NET_TX_FAULT 8u
#define CX3_AUTO_NET_LIFECYCLE_BLOCKED 16u
enum { kCx3AutoStatus=0, kCx3AutoEnable=1, kCx3AutoDisable=2 };
enum { kCx3AutoIdle=0, kCx3AutoInitializing=1, kCx3AutoReady=2,
       kCx3AutoDisabled=3, kCx3AutoFailed=4 };
struct Cx3AutoNetworkRequest {
    uint32_t version, structBytes, action, port;
    uint64_t expectedServiceId, expectedProviderId, expectedGeneration;
    char expectedBuildTag[65];
    uint8_t reserved[7];
};
struct Cx3AutoNetworkResult {
    uint32_t version, structBytes;
    uint64_t status;
    uint32_t state, action, stage, completedStage, failedStage, lastSelector;
    uint32_t interfaceUnit, port;
    uint64_t deviceGeneration, completedMask, lastStageStatus;
    uint32_t flags, reserved;
    uint64_t lastScalars[16]; /* sentinels for unused/not-returned slots */
};

struct Cx3RuntimeIdentityResult {
    uint32_t version, structBytes;
    uint64_t status, validity, serviceRegistryId, providerRegistryId;
    uint64_t sampleUptimeNs, deviceGeneration;
    uint32_t lifecycleFlags, reserved;
    struct Cx3PcieIdentity pciIdentity;
    char bundleVersion[32], buildTag[65];
    uint8_t reservedBytes[3];
};

/*
 * Compile-time guard on the scalar limits.
 *
 * The scalar cap is not enforced anywhere the compiler can see, so exceeding it
 * turns into a runtime kIOReturnBadArgument that reads like a permissions or
 * argument-count mistake rather than "this selector is too wide".  These
 * assertions make an over-wide selector a build failure instead.
 *
 * THE CAP IS THE KERNEL'S, NOT OUR PARAMETER.  It is
 * `kIOExternalMethodScalarOutputCountMax` (and the input one is
 * `kIOExternalMethodScalarInputCountMax`), declared in
 * Kernel.framework/Headers/IOKit/IOUserClient.h:
 *
 *   /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/System/Library/Frameworks/
 *   Kernel.framework/Headers/IOKit/IOUserClient.h:116-119
 *   ...and the copy this build actually includes, because build.sh passes
 *   -nostdinc -I$SDK/Headers with SDK=tools/research/mksdk:
 *   tools/research/mksdk/Headers/IOKit/IOUserClient.h:150-153
 *
 *   enum {
 *       kIOExternalMethodScalarInputCountMax  = 16,
 *       kIOExternalMethodScalarOutputCountMax = 16,
 *   };
 *
 * Both copies are 16, and a declared width above it is refused with kIOReturnBadArgument
 * (0xe00002c2) before any of the selector's code runs -- measured on 0.43.9, where every
 * combination of input count, output count and structure size failed the same way while the
 * selectors under the cap worked in the same boot (docs/DEFECTS.md F13).  WHERE the refusal
 * happens is not pinned here: the header's own comment calls these constants "documentary
 * since the scalarInputCount, scalarOutputCount fields reflect the actual number passed", the
 * dispatch path in XNU's IOUserClient::externalMethod compares the caller's counts against
 * the DECLARED ones (both 19, so it would pass), and the in-band scalar arrays in the
 * io_connect_method message are the 16 these constants size -- so the practical rule is the
 * one that matters, and the exact site is F13's open question.  Nothing in this project can
 * widen the limit: a selector whose declared width exceeds it is not "wide", it is
 * unreachable, and every argument-count and size combination a caller can try fails alike.
 *
 * 0.43.9 RAISED THIS TO 20 FOR `r4tx`, WHICH IS THE DEFECT THIS COMMENT NOW RECORDS
 * (docs/DEFECTS.md F13).  r4tx had filled all sixteen, three feup facts wanted scalars,
 * and the note argued that "the cap was the thing to move rather than the facts to
 * compress" -- but the cap is not a knob, so every `r4tx` call in 0.43.9 returned
 * kIOReturnBadArgument and the feup question was never measured.  Raising the number also
 * silently disabled the only guard: the per-selector
 * `CX3_STATIC_ASSERT(kR4TxOutCount <= CX3_SCALAR_OUT_MAX)` stayed true because both sides
 * moved together.  Three guards now hold it:
 *
 *   1. the identity below is asserted where the kernel's constant is visible
 *      (src/Cx3Mlx4UserClient.cpp, which includes <IOKit/IOUserClient.h>), so changing
 *      this number away from the kernel's is a build failure, not a runtime refusal;
 *   2. every selector's declared widths are asserted against the cap -- outputs for all 38,
 *      inputs for all that declare any (the four with none are left alone: 0 <= 16 is not a
 *      check) -- beside the enum or in the block below, wherever this file already kept that
 *      selector's assert.  Adding a scalar past the cap therefore fails the build at the
 *      selector that did it;
 *   3. tools/verify.sh compares every selector's declared output width with the cap it reads
 *      out of this header, so a run says "unreachable" before it touches the device.
 *
 * The number is an ABI width: the tool sizes its output array from it and the kernel writes
 * into that array, so `cx3ctl` and the kext must be installed together, exactly as the
 * CQE/OEE structure changes already required.  A selector that runs out of scalars has to
 * move a fact into its structure (there is no 16-value limit there), not a limit here --
 * `r4tx` is the worked example, in its enum below.
 */
#define CX3_SCALAR_IN_MAX  16
#define CX3_SCALAR_OUT_MAX 16




/*
 * Firmware command status codes, from FreeBSD mlx4_core/mlx4_cmd.c CMD_STAT_*.
 *
 * Worth naming because the number alone is not interpretable and the mapping to
 * a meaning is the whole diagnosis.  CMD_STAT_BAD_SYS_STATE in particular means
 * "the device is not in a state where this command can run", which is how
 * QUERY_DEV_CAP answered before the firmware had been started.
 */
#define CX3_FW_STATUS_LIST(X)                     \
    X(0x00, OK,              "ok")                \
    X(0x01, INTERNAL_ERR,    "internal error")    \
    X(0x02, BAD_OP,          "bad opcode")        \
    X(0x03, BAD_PARAM,       "bad parameter")     \
    X(0x04, BAD_SYS_STATE,   "wrong system state") \
    X(0x05, BAD_RESOURCE,    "bad resource")      \
    X(0x06, RESOURCE_BUSY,   "resource busy")     \
    X(0x08, EXCEED_LIM,      "exceeded limit")    \
    X(0x09, BAD_RES_STATE,   "bad resource state") \
    X(0x0a, BAD_INDEX,       "bad index")         \
    X(0x0b, BAD_NVMEM,       "bad NVMEM")         \
    X(0x0c, ICM_ERROR,       "ICM error")         \
    X(0x10, BAD_QP_STATE,    "bad QP state")      \
    X(0x20, BAD_SEG_PARAM,   "bad segment parameter") \
    X(0x21, REG_BOUND,       "MR has MWs bound")  \
    X(0x22, LAM_NOT_PRE,     "LAM not prepared")  \
    X(0x30, BAD_PKT,         "bad packet")        \
    X(0x40, BAD_SIZE,        "bad size")          \
    X(0x50, MULTI_FUNC_REQ,  "multi-function required")

#define CX3_FW_STATUS_DECL(code, name, text) CX3_FW_STATUS_##name = code,
enum { CX3_FW_STATUS_LIST(CX3_FW_STATUS_DECL) CX3_FW_STATUS__COUNT = 0x51 };
#undef CX3_FW_STATUS_DECL

/*
 * kCx3SelIcmAlloc -- allocate or free the area the firmware will use for its own
 * tables, and report what came back.
 *
 * Deliberately device-inert: no register is touched, no command is sent.  The ICM
 * layer is where the reference does most of its DMA work, so it is worth being
 * able to exercise the allocation on its own, before any of it is handed to the
 * firmware over MAP_FA.
 *
 * inputs:  pages  > 0 allocate that many 4 KiB pages; 0 frees what is held.
 * outputs: a summary, not the whole page list, because the scalar path caps at 16
 *          values.  Enough to confirm the two invariants that matter: every page
 *          is aligned, and bus segments are whole numbers of pages.
 */
enum {
    kIcmInPages = 0,
    kIcmInCount = 1,
};
enum {
    kIcmOutStatus     = 0,   /* kIOReturn, 0 = ok */
    kIcmOutPages      = 1,
    kIcmOutBytes      = 2,
    kIcmOutFirstBus   = 3,
    kIcmOutLastBus    = 4,
    kIcmOutAligned    = 5,   /* 1 = icmCheck passed */
    kIcmOutSegments   = 6,   /* how many bus segments the area spans */
    kIcmOutContiguous = 7,   /* 1 = a single segment */
    kIcmOutChecksum   = 8,   /* sum of all page bus addresses, for comparison */
    kIcmOutCount      = 9,
};


/*
 * kCx3SelFwLoad -- M2b, the whole firmware handover, in one call.
 *
 * The reference order is QUERY_FW, allocate fw_pages of ICM, MAP_FA, RUN_FW,
 * MOD_STAT_CFG.  Only after that does QUERY_DEV_CAP answer; sending it earlier
 * returns CMD_STAT_BAD_SYS_STATE, which is how this ordering was discovered.
 *
 * One selector rather than four, because the steps are useless apart and each one
 * is reported separately in the output, so a failure is still attributable.
 * Every firmware status code comes back, with 0xffffffff meaning "never reached".
 */
enum {
    kFwLoadInPages = 0,     /* fw_pages, straight from QUERY_FW */
    kFwLoadInLowmem = 1,    /* 1 = constrain the FW area below 4 GiB */
    /*
     * 1 = load the page count anyway, even though it disagrees with the
     * QUERY_FW answer.  The check exists because the first M2b build took
     * fw_pages from the wrong outbox offset and handed the firmware 3.3x the
     * area it asked for; an override keeps the deliberate experiment possible
     * without leaving the default path able to repeat that mistake.
     */
    kFwLoadInForce = 2,
    /*
     * 1 = fill the FW area with a recognisable pattern before MAP_FA.
     *
     * This is what makes the "is there a firmware image in there?" reading
     * decisive.  Freshly allocated kernel pages can hold old data, so an
     * untouched area is not necessarily all zeros and "0 non-zero bytes" is a
     * weaker statement than it looks.  With a known pattern written first, the
     * contents afterwards have three distinguishable readings: our pattern is
     * still intact (the device never wrote), the pattern is gone (the device
     * wrote its own data), or it reads all zeros (neither).
     */
    kFwLoadInFill = 3,
    kFwLoadInCount = 4,
};
enum {
    kFwLoadOutStatus      = 0,   /* kIOReturn, 0 = the whole sequence ran */
    kFwLoadOutPages       = 1,   /* pages the firmware asked for */
    kFwLoadOutEntries     = 2,   /* MAP_FA entries actually sent */
    kFwLoadOutMapFaStatus = 3,   /* fw status from MAP_FA */
    kFwLoadOutRunFwStatus = 4,   /* fw status from RUN_FW */
    kFwLoadOutModStatus   = 5,   /* fw status from MOD_STAT_CFG */
    kFwLoadOutFirstBus    = 6,
    kFwLoadOutLastBus     = 7,
    /*
     * How much of the FW area actually holds data, sampled after the sequence
     * ends.  The device is what fills this area in (nothing in the host driver
     * writes it -- searched: no reference code writes fw_icm), so a zero here
     * after RUN_FW says the firmware image never arrived, while a populated area
     * says the load happened and the hang is elsewhere.
     */
    kFwLoadOutNonzero     = 8,   /* non-zero bytes in the whole FW area */
    kFwLoadOutChecksum    = 9,   /* additive checksum over its dwords */
    kFwLoadOutCount       = 10,
};

/*
 * kCx3SelFwArea -- read-only inspection of the FW area handed to the device.
 *
 * Exists because "RUN_FW timed out" has two very different explanations that no
 * register can tell apart: the device never wrote its firmware image into the
 * area (so there was nothing to run), or it wrote the image and the handover
 * still did not complete.  The only place that distinction is visible is the
 * area's contents, and until this selector existed the contents were not
 * reachable from user space at all.
 */
enum {
    kFwAreaInOffset = 0,   /* byte offset to start a dump from */
    kFwAreaInCount  = 1,   /* dwords to return, 0 = summary only */
    kFwAreaInCount_ = 2,
};
enum {
    kFwAreaOutStatus   = 0,   /* kIOReturn, 0 = ok */
    kFwAreaOutHeld     = 1,   /* 1 if a FW area is currently held */
    kFwAreaOutPages    = 2,
    kFwAreaOutNonzero  = 3,   /* non-zero bytes in the whole area */
    kFwAreaOutChecksum = 4,
    kFwAreaOutData0    = 5,
    kFwAreaMaxDwords   = 10,
    kFwAreaOutCount    = kFwAreaOutData0 + kFwAreaMaxDwords,   /* 15 */
};

/*
 * kCx3SelFwOutbox -- ask the device to DMA into a page of the FW area.
 *
 * This is the cleanest available test of "can the firmware write into the memory
 * we mapped for it?", and it needs no new hardware access path: a command whose
 * outbox address is one of the FW area's pages must, if it completes, have the
 * device write its answer into that page.  QUERY_FW is the obvious candidate --
 * it is known to complete on this card, it writes a fixed 4 KiB answer, and its
 * contents are self-verifying (version, cmd_if_rev, fw_pages).
 *
 * Why it matters: RUN_FW never completing could mean "the device cannot reach
 * this memory" or "the device has nothing to run".  A QUERY_FW whose answer
 * lands in the FW area separates those two, because it exercises exactly the
 * DMA path RUN_FW would need while removing every other variable.
 *
 * Only meaningful while an area is held (after fwload or icmalloc), and on this
 * hardware only after a fresh reset, because a stuck RUN_FW leaves the command
 * register busy; ignoreBusy is offered for the case where it is deliberately
 * run afterwards anyway.
 */
enum {
    kFwOutboxInPage      = 0,  /* which page of the FW area to use as outbox */
    kFwOutboxInDwords    = 1,  /* how many dwords of that page to return */
    kFwOutboxInIgnoreBusy = 2, /* 1 = post even if the command register looks busy */
    kFwOutboxInCount     = 3,
};
enum {
    kFwOutboxOutStatus     = 0,  /* kIOReturn, 0 = ok */
    kFwOutboxOutPage       = 1,  /* the page actually used */
    kFwOutboxOutBusAddr    = 2,  /* its bus address, as handed to the device */
    kFwOutboxOutFwStatus   = 3,  /* the device's answer: status byte */
    kFwOutboxOutFwVer      = 4,
    kFwOutboxOutChanged    = 5,  /* dwords of that page that differ from before */
    kFwOutboxOutNonzero    = 6,  /* non-zero dwords of that page after the command */
    kFwOutboxOutData0      = 7,
    kFwOutboxMaxDwords     = 8,
    kFwOutboxOutCount      = kFwOutboxOutData0 + kFwOutboxMaxDwords,   /* 15 */
};


/*
 * Compile-time guard on the scalar limits.
 *
 * The first version used #if tests and they never worked: #if is evaluated by the
 * preprocessor, which cannot see enum constants, so `#if kFwOutCount > 16`
 * evaluated the name to 0 and passed. QUERY_FW then shipped with 18 scalar outputs
 * and failed at run time with kIOReturnBadArgument (0xe00002c2) -- the second time
 * this particular limit cost a test cycle.
 *
 * _Static_assert is evaluated by the compiler, which does know the enum values.
 * Verified by construction rather than assumed: setting kFwOutCount back to 18
 * stops the build here.
 */
#if defined(__cplusplus)
#define CX3_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define CX3_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

CX3_STATIC_ASSERT(sizeof(struct Cx3AdapterResult) == 360, "adapter result ABI size");
CX3_STATIC_ASSERT(offsetof(struct Cx3AdapterResult, boardId) == 40, "adapter board ID offset");
CX3_STATIC_ASSERT(offsetof(struct Cx3AdapterResult, raw) == 104, "adapter raw offset");
CX3_STATIC_ASSERT(sizeof(struct Cx3PcieIdentity) == 12, "PCIe identity ABI size");
CX3_STATIC_ASSERT(sizeof(struct Cx3PcieLinkResult) == 368, "PCIe link ABI size");
CX3_STATIC_ASSERT(offsetof(struct Cx3PcieLinkResult, identityBefore) == 56, "PCIe before identity offset");
CX3_STATIC_ASSERT(offsetof(struct Cx3PcieLinkResult, identityAfter) == 68, "PCIe after identity offset");
CX3_STATIC_ASSERT(offsetof(struct Cx3PcieLinkResult, raw) == 80, "PCIe raw offset");
CX3_STATIC_ASSERT(offsetof(struct Cx3PcieLinkResult, readValid) == 336, "PCIe validity map offset");
CX3_STATIC_ASSERT(sizeof(struct Cx3AutoNetworkRequest)==112, "auto network request ABI");
CX3_STATIC_ASSERT(sizeof(struct Cx3AutoNetworkResult)==208, "auto network result ABI");
CX3_STATIC_ASSERT(offsetof(struct Cx3AutoNetworkResult,lastScalars)==80, "auto network scalars ABI");
CX3_STATIC_ASSERT(sizeof(struct Cx3RuntimeIdentityResult) == 176, "runtime identity ABI size");
CX3_STATIC_ASSERT(offsetof(struct Cx3RuntimeIdentityResult, version) == 0, "runtime version offset");
CX3_STATIC_ASSERT(offsetof(struct Cx3RuntimeIdentityResult, structBytes) == 4, "runtime size offset");
CX3_STATIC_ASSERT(offsetof(struct Cx3RuntimeIdentityResult, status) == 8, "runtime status offset");
CX3_STATIC_ASSERT(offsetof(struct Cx3RuntimeIdentityResult, validity) == 16, "runtime validity offset");
CX3_STATIC_ASSERT(offsetof(struct Cx3RuntimeIdentityResult, serviceRegistryId) == 24, "runtime service ID offset");
CX3_STATIC_ASSERT(offsetof(struct Cx3RuntimeIdentityResult, providerRegistryId) == 32, "runtime provider ID offset");
CX3_STATIC_ASSERT(offsetof(struct Cx3RuntimeIdentityResult, sampleUptimeNs) == 40, "runtime uptime offset");
CX3_STATIC_ASSERT(offsetof(struct Cx3RuntimeIdentityResult, deviceGeneration) == 48, "runtime generation offset");
CX3_STATIC_ASSERT(offsetof(struct Cx3RuntimeIdentityResult, lifecycleFlags) == 56, "runtime flags offset");
CX3_STATIC_ASSERT(offsetof(struct Cx3RuntimeIdentityResult, reserved) == 60, "runtime reserved word offset");
CX3_STATIC_ASSERT(offsetof(struct Cx3RuntimeIdentityResult, pciIdentity) == 64, "runtime PCI offset");
CX3_STATIC_ASSERT(offsetof(struct Cx3RuntimeIdentityResult, bundleVersion) == 76, "runtime version offset");
CX3_STATIC_ASSERT(offsetof(struct Cx3RuntimeIdentityResult, buildTag) == 108, "runtime tag offset");
CX3_STATIC_ASSERT(offsetof(struct Cx3RuntimeIdentityResult, reservedBytes) == 173, "runtime reserved offset");

CX3_STATIC_ASSERT(kProbeOutCount    <= CX3_SCALAR_OUT_MAX, "probe: outputs over the scalar cap");
CX3_STATIC_ASSERT(kDmaOutCount      <= CX3_SCALAR_OUT_MAX, "dma: outputs over the scalar cap");
CX3_STATIC_ASSERT(kFwInCount        <= CX3_SCALAR_IN_MAX,  "queryfw: inputs over the scalar cap");
CX3_STATIC_ASSERT(kFwOutCount       <= CX3_SCALAR_OUT_MAX, "queryfw: outputs over the scalar cap");
CX3_STATIC_ASSERT(kRstInCount       <= CX3_SCALAR_IN_MAX,  "reset: inputs over the scalar cap");
CX3_STATIC_ASSERT(kRstOutCount      <= CX3_SCALAR_OUT_MAX, "reset: outputs over the scalar cap");
/*
 * kPeekInCount_ -- with the underscore -- is this selector's scalar-input WIDTH (index 2 is
 * kPeekInCount, which is a value the caller passes, not a width).  The first version of this
 * assert named the value, so it checked a number that has nothing to do with the cap while
 * its message said it did; found by auditing all 38 selectors against the kernel's 16 in
 * 0.43.10 (docs/DEFECTS.md F13), which is also when the literal 3 in the dispatch table and
 * in tools/cx3ctl.c's `sizes` table was replaced by this name.
 */
CX3_STATIC_ASSERT(kPeekInCount_     <= CX3_SCALAR_IN_MAX,  "peek: inputs over the scalar cap");
CX3_STATIC_ASSERT(kPeekOutCount_    <= CX3_SCALAR_OUT_MAX, "peek: outputs over the scalar cap");
CX3_STATIC_ASSERT(kClrInCount       <= CX3_SCALAR_IN_MAX,  "clearowner: inputs over the scalar cap");
CX3_STATIC_ASSERT(kClrOutCount      <= CX3_SCALAR_OUT_MAX, "clearowner: outputs over the scalar cap");
CX3_STATIC_ASSERT(kPokeInCount      <= CX3_SCALAR_IN_MAX,  "poke: inputs over the scalar cap");
CX3_STATIC_ASSERT(kPokeOutCount     <= CX3_SCALAR_OUT_MAX, "poke: outputs over the scalar cap");
CX3_STATIC_ASSERT(kFlrInCount       <= CX3_SCALAR_IN_MAX,  "flr: inputs over the scalar cap");
CX3_STATIC_ASSERT(kFlrOutCount      <= CX3_SCALAR_OUT_MAX, "flr: outputs over the scalar cap");
CX3_STATIC_ASSERT(kDevCapInCount    <= CX3_SCALAR_IN_MAX,  "querydevcap: inputs over the scalar cap");
CX3_STATIC_ASSERT(kDevCapOutCount   <= CX3_SCALAR_OUT_MAX, "querydevcap: outputs over the scalar cap");
CX3_STATIC_ASSERT(kIcmInCount       <= CX3_SCALAR_IN_MAX,  "icmalloc: inputs over the scalar cap");
CX3_STATIC_ASSERT(kIcmOutCount      <= CX3_SCALAR_OUT_MAX, "icmalloc: outputs over the scalar cap");
CX3_STATIC_ASSERT(kFwLoadInCount    <= CX3_SCALAR_IN_MAX,  "fwload: inputs over the scalar cap");
CX3_STATIC_ASSERT(kFwLoadOutCount   <= CX3_SCALAR_OUT_MAX, "fwload: outputs over the scalar cap");
CX3_STATIC_ASSERT(kFwAreaInCount_   <= CX3_SCALAR_IN_MAX,  "fwarea: inputs over the scalar cap");
CX3_STATIC_ASSERT(kFwAreaOutCount   <= CX3_SCALAR_OUT_MAX, "fwarea: outputs over the scalar cap");
CX3_STATIC_ASSERT(kFwAreaMaxDwords  <= 10,                 "fwarea: dump width over budget");
CX3_STATIC_ASSERT(kFwOutboxInCount  <= CX3_SCALAR_IN_MAX,  "fwoutbox: inputs over the scalar cap");
CX3_STATIC_ASSERT(kFwOutboxOutCount <= CX3_SCALAR_OUT_MAX, "fwoutbox: outputs over the scalar cap");
CX3_STATIC_ASSERT(kFwOutboxMaxDwords <= 8,                 "fwoutbox: dump width over budget");

/*
 * kCx3SelM2dProfile -- decode QUERY_DEV_CAP and run mlx4_make_profile.
 *
 * The profile knobs are inputs rather than constants because they decide how much
 * context memory the HCA is asked to reserve, and this driver does not need the
 * reference's default (2^18 QPs is an IB-stack-sized table).  Everything the
 * follow-up steps need is kept in the provider, so this call is the only one that
 * takes them.
 *
 * Both results travel as a structure: the decoded capability set and the packed
 * layout together are far more than 16 values, and truncating either would make
 * the numbers unreportable exactly when they matter.
 */
enum {
    kM2dInNumQp        = 0,
    kM2dInRdmarcPerQp  = 1,
    kM2dInNumSrq       = 2,
    kM2dInNumCq        = 3,
    kM2dInNumMcg       = 4,
    kM2dInNumMpt       = 5,
    kM2dInNumMtt       = 6,   /* in segments */
    kM2dInCount        = 7,
};
enum {
    kProfOutStatus   = 0,     /* kIOReturn, 0 = ok */
    kProfOutFwStatus = 1,
    kProfOutCount    = 2,
};
struct Cx3ProfileResult {
    uint64_t status;
    uint64_t fwStatus;
    uint64_t icmSize;         /* bytes the profile needs */
    uint64_t maxIcmSz;        /* what the HCA offers */
    uint64_t qpcBase, srqcBase, cqcBase, eqcBase, altcBase, auxcBase;
    uint64_t rdmarcBase, dmptBase, cmptBase, mttBase, mcBase;
    uint32_t logNumQps, logNumSrqs, logNumCqs, logNumEqs, logRdPerQp;
    uint32_t logMptSz, logMcEntrySz, logMcTableSz, logMcHashSz;
    uint32_t numSysEqs, logUarSz, uarPageSz, mwEnabled;
    uint32_t numTables;
    /* decoded capability values the profile was built from */
    uint32_t qpcEntrySz, cqcEntrySz, eqcEntrySz, srqEntrySz;
    uint32_t dmptEntrySz, cmptEntrySz, mttEntrySz, rdmarcEntrySz;
    uint32_t auxEntrySz, altcEntrySz;
    uint32_t reservedQps, reservedCqs, reservedSrqs, reservedEqs;
    uint32_t reservedMtts, reservedMrws, numPorts;
    uint32_t flags2;
};

/*
 * kCx3SelM2dBringUp -- the rest of M2d, in the reference's order:
 * SET_ICM_SIZE -> allocate + MAP_ICM_AUX -> MAP_ICM each table -> INIT_HCA.
 *
 * One selector because the steps are useless apart and each one's status is
 * reported separately, so a failure still names the step that failed.
 */
enum { kBringUpInCount = 0 };
enum {
    kBringUpOutStatus      = 0,   /* kIOReturn, 0 = the sequence ran */
    kBringUpOutIcmSize     = 1,
    kBringUpOutAuxUnits    = 2,   /* SET_ICM_SIZE's answer, in 1 MiB units */
    kBringUpOutAuxPages    = 3,   /* what we allocated for it, in 4 KiB pages */
    kBringUpOutSetIcmStat  = 4,
    kBringUpOutMapAuxStat  = 5,
    kBringUpOutTablesDone  = 6,
    kBringUpOutTablesTotal = 7,
    kBringUpOutTableMask   = 8,   /* bit i set = table i mapped */
    kBringUpOutInitHcaStat = 9,
    kBringUpOutCount       = 10,
};

/*
 * ------------------------------------------------------------------ */
/* M3 restart 2: interrupts                                             */
/* ------------------------------------------------------------------ */

/*
 * kCx3SelM3IrqUp -- open an interrupt session on one provider interrupt source.
 *
 * The source index is an input rather than a constant on purpose: which provider
 * index corresponds to MSI-X table entry 0 is not something this project has
 * measured, and the EQ context's intr field is already committed to table entry 0
 * (that is what the reference fills in for the asynchronous EQ).  The call reports
 * getInterruptType's answer so the mapping can be settled by measurement instead of
 * by assumption.
 */
enum {
    kIrqUpInSource = 0,
    /*
     * 1 = allow a non-messaged (legacy) source anyway.  Only meaningful once this
     * port can clear the interrupt at the device; until then a legacy source storms,
     * which is why the default is to refuse.  The override exists so the reason can
     * be recorded rather than the option being silently absent.
     */
    kIrqUpInForceLegacy = 1,
    kIrqUpInCount = 2,
};
enum {
    kIrqUpOutStatus    = 0,   /* kIOReturn, 0 = session open */
    kIrqUpOutSource    = 1,   /* the provider interrupt index used */
    kIrqUpOutIntType   = 2,   /* getInterruptType's answer */
    kIrqUpOutGetTypeRc = 3,
    kIrqUpOutEnabled   = 4,
    kIrqUpOutEqn       = 5,
    kIrqUpOutEqIntr    = 6,   /* what the EQ context was built with */
    kIrqUpOutCount     = 7,
};

/* kCx3SelM3IrqTest -- register the token to wait for, post the NOP, return. */
enum { kIrqTestInToken = 0, kIrqTestInCount = 1 };
enum {
    kIrqTestOutStatus    = 0,
    kIrqTestOutFwStatus  = 1,
    kIrqTestOutToken     = 2,
    kIrqTestOutHcrOnPost = 3,   /* the command dword as the hardware was handed it */
    kIrqTestOutCount     = 4,
};

/*
 * kCx3SelM3IrqStatus -- read the completion later, from another thread.
 *
 * This separation is the reviews' point: the posting call must not wait for a
 * completion that the interrupt path has to deliver, because if the waiter ran on
 * the workloop the workloop would be waiting for itself.
 */
enum {
    /*
     * The scalar output path is capped at 16 values and this selector needs more
     * facts than that, so the five booleans are packed into one flags word and the
     * counters, which the reviews asked to be kept separate, each get their own slot.
     */
    kIrqStatusOutFlags         = 0,    /* bit0 enabled, bit1 objects exist,
                                        * bit2 storm tripped, bit3 legacy source,
                                        * bit4 clear register ready */
    kIrqStatusOutIrqCount      = 1,    /* action invocations (not interrupts) */
    kIrqStatusOutCallbackCount = 2,    /* sum of the callback's count argument */
    kIrqStatusOutEqeCount      = 3,    /* EQEs consumed inside the action */
    kIrqStatusOutPollEqeCount  = 4,    /* EQEs the poll selector consumed instead */
    kIrqStatusOutClrWrites     = 5,    /* interrupt acknowledgements written */
    kIrqStatusOutErrors        = 6,    /* ready slots that carried nothing usable */
    kIrqStatusOutSkipped       = 7,    /* invocations that consumed nothing */
    kIrqStatusOutPending       = 8,
    kIrqStatusOutWantToken     = 9,
    kIrqStatusOutMatched       = 10,
    kIrqStatusOutMatchedToken  = 11,
    kIrqStatusOutMatchedStatus = 12,
    kIrqStatusOutLastType      = 13,
    kIrqStatusOutConsIndex     = 14,
    kIrqStatusOutZeroStreak    = 15,   /* consecutive invocations that consumed nothing */
    kIrqStatusOutCount         = 16,
};

/* Bits of kIrqStatusOutFlags. */
#define CX3_IRQ_F_ENABLED      (1ull << 0)
#define CX3_IRQ_F_OBJECTS      (1ull << 1)
#define CX3_IRQ_F_STORM        (1ull << 2)
#define CX3_IRQ_F_LEGACY       (1ull << 3)
#define CX3_IRQ_F_CLR_READY    (1ull << 4)

/*
 * ------------------------------------------------------------------ */
/* R3: the port                                                         */
/* ------------------------------------------------------------------ */

/*
 * QUERY_PORT's output, decoded ONCE, in a header both sides include.
 *
 * There is a reason this is shared rather than decoded in the tool: the first
 * version of this project's device-capability decode existed twice, and the two
 * copies disagreed about a field for a while without anything noticing.  A
 * decoder in the ABI header means the kernel and the control tool cannot have
 * different opinions about an offset -- there is only one opinion.
 *
 * Offsets are the reference's, from mlx4_fw.c's new-port-commands branch
 * (mlx4_fw.c:1202-1243) and the Ethernet view in mlx4_en/en_port.h:89-102.  Both
 * views are the SAME response: the core reads 0x18 as a BE32 whose top byte is
 * trans_type, and the Ethernet code reads that same byte as `transceiver`.
 *
 * Widths come from the destination types in struct mlx4_port_cap (fw.h:46-62):
 * MLX4_GET reads sizeof(destination) bytes, big-endian.  wavelength is a u16 and
 * trans_code a u64 -- not, as an earlier reading of this port's own draft had it,
 * a u32 each.
 */
#define CX3_QUERY_PORT_RAW_LEN  64

/*
 * "There is no output slot for the raw owner byte" -- passed to reportOwnerMap by
 * selectors whose output block is full.  It exists because an output index was once
 * written by an implicit "+1" convention and landed on top of another field; see
 * Cx3Mlx4::reportOwnerMap.
 */
#define kCx3OwnerNoByteSlot 0xffffffffu

/* supported_port_types is a bitfield, not an enum: 1 = IB, 2 = Ethernet. */
#define CX3_PORT_TYPE_IB   1
#define CX3_PORT_TYPE_ETH  2

/*
 * Two kIOReturn values, as they arrive in a scalar output.
 *
 * The selectors write "(UInt64)kIOReturnX".  IOReturn is a signed 32-bit type and
 * these values have bit 31 set, so the cast sign-extends: the caller sees
 * 0xffffffffe00002c7, not 0xe00002c7.  Named here because the comparison has to be
 * against the exact 64-bit pattern the kernel produced, and spelling the prefix out at
 * every call site is how that gets got wrong.
 */
#define CX3_IORETURN_UNSUPPORTED 0xffffffffe00002c7ull   /* kIOReturnUnsupported */
#define CX3_IORETURN_NOT_READY   0xffffffffe00002d8ull   /* kIOReturnNotReady */

/* The link-speed encoding, byte 0x05 & 0x6f (en_port.h:78-86).  0x40 is 40G and
 * 0x20 is 56G -- the two are adjacent and easy to swap, so they are named. */
#define CX3_SPEED_10G_XAUI  0x00
#define CX3_SPEED_10G_XFI   0x01
#define CX3_SPEED_1G        0x02
#define CX3_SPEED_100M      0x04
#define CX3_SPEED_20G       0x08
#define CX3_SPEED_OTHER     0x0f
#define CX3_SPEED_56G       0x20
#define CX3_SPEED_40G       0x40

typedef struct {
    uint32_t status;               /* kIOReturn for the selector itself */
    uint32_t fwStatus;
    uint32_t port;
    uint32_t rawLen;
    uint8_t  raw[CX3_QUERY_PORT_RAW_LEN];
    /* decoded */
    uint32_t linkState;            /* 0x00 bit7 -- this is the link-up bit */
    uint32_t supportedTypes;       /* 0x00 bits1:0 */
    uint32_t suggestedType;        /* 0x00 bit3 */
    uint32_t defaultSense;         /* 0x00 bit4 */
    uint32_t dmfsOptimized;        /* 0x00 bit5 */
    uint32_t autoneg;              /* 0x01 bit7 */
    uint32_t ibMtu;                /* 0x01 & 0xf */
    uint32_t ethMtu;               /* BE16 0x02 */
    uint32_t linkSpeed;            /* 0x05 & 0x6f */
    uint32_t maxPortWidth;         /* 0x06 & 0xf */
    uint32_t maxGids;              /* 1 << (0x07 >> 4) */
    uint32_t maxPkeys;             /* 1 << (0x07 & 0xf) */
    uint32_t logMaxMacs;           /* 0x0a & 0xf */
    uint32_t logMaxVlans;          /* 0x0a >> 4 */
    uint32_t maxVl;                /* 0x0b & 0xf */
    uint32_t maxTcEth;             /* 0x0b >> 4 */
    uint8_t  defMac[8];            /* BE64 0x10, as it comes off the wire */
    uint32_t transType;            /* BE32 0x18 >> 24 */
    uint32_t vendorOui;            /* BE32 0x18 & 0xffffff */
    uint32_t wavelength;           /* BE16 0x1c */
    uint64_t transCode;            /* BE64 0x20 */
} Cx3QueryPortResult;

/* The one decode, used by the kext and by cx3ctl.  `outbox` is the raw mailbox. */
static inline void cx3QueryPortDecode(const uint8_t *b, Cx3QueryPortResult *r)
{
    const uint32_t w32 = ((uint32_t)b[0x18] << 24) | ((uint32_t)b[0x19] << 16) |
                         ((uint32_t)b[0x1a] << 8)  |  (uint32_t)b[0x1b];
    uint32_t i;

    r->linkState      = (b[0x00] & 0x80) >> 7;
    r->supportedTypes = b[0x00] & 0x03;
    r->suggestedType  = (b[0x00] >> 3) & 1;
    r->defaultSense   = (b[0x00] >> 4) & 1;
    r->dmfsOptimized  = (b[0x00] >> 5) & 1;
    r->autoneg        = (b[0x01] & 0x80) >> 7;
    r->ibMtu          = b[0x01] & 0x0f;
    r->ethMtu         = ((uint32_t)b[0x02] << 8) | (uint32_t)b[0x03];
    r->linkSpeed      = b[0x05] & 0x6f;
    r->maxPortWidth   = b[0x06] & 0x0f;
    r->maxGids        = 1u << ((b[0x07] >> 4) & 0xf);
    r->maxPkeys       = 1u << (b[0x07] & 0x0f);
    r->logMaxMacs     = b[0x0a] & 0x0f;
    r->logMaxVlans    = (b[0x0a] >> 4) & 0x0f;
    r->maxVl          = b[0x0b] & 0x0f;
    r->maxTcEth       = (b[0x0b] >> 4) & 0x0f;
    for (i = 0; i < 8; ++i) r->defMac[i] = b[0x10 + i];
    r->transType      = (w32 >> 24) & 0xff;
    r->vendorOui      = w32 & 0xffffff;
    r->wavelength     = ((uint32_t)b[0x1c] << 8) | (uint32_t)b[0x1d];
    r->transCode      = 0;
    for (i = 0; i < 8; ++i)
        r->transCode = (r->transCode << 8) | (uint64_t)b[0x20 + i];
}

/* Speed in Mb/s, or 0 for an encoding the reference itself reports as unknown
 * (mlx4_en_port.c:95-116 returns -1; 0 is that, in a uint32_t). */
static inline uint32_t cx3LinkSpeedMbps(uint32_t raw)
{
    switch (raw & 0x6f) {
    case CX3_SPEED_100M:     return 100;
    case CX3_SPEED_1G:       return 1000;
    case CX3_SPEED_10G_XAUI: return 10000;
    case CX3_SPEED_10G_XFI:  return 10000;
    case CX3_SPEED_20G:      return 20000;
    case CX3_SPEED_40G:      return 40000;
    case CX3_SPEED_56G:      return 56000;
    default:                 return 0;
    }
}

/*
 * kCx3SelM3QueryPort -- QUERY_PORT for one port.  Read-only.
 *
 * The capability answer decides whether the R3 write path may run at all, so it
 * is a separate selector from the one that acts on it.
 */
enum { kQueryPortInPort = 0, kQueryPortInCount = 1 };
enum {
    kQueryPortOutStatus = 0,
    kQueryPortOutCount  = 1,
};
CX3_STATIC_ASSERT(kQueryPortInCount  <= CX3_SCALAR_IN_MAX,  "queryport: inputs over the scalar cap");
CX3_STATIC_ASSERT(kQueryPortOutCount <= CX3_SCALAR_OUT_MAX, "queryport: outputs over the scalar cap");

/*
 * kCx3SelM3PortUp -- bring one port up, in the reference's order.
 *
 * The reference's normal Ethernet start path is general -> INIT_PORT
 * (mlx4_en/mlx4_en_netdev.c:2312-2325); CLOSE_PORT belongs to the stop and
 * type-change paths, not to a first initialisation, which is why this sequence
 * does not send it.
 *
 * The command's parameters are NOT inputs.  Both reviews pointed out that
 * op_modifier 0 is the IB opcode and 1 is Ethernet (cmd.h:194-195), and that the
 * high bits of in_modifier select a different Ethernet sub-command (cmd.h:200-210)
 * -- so a wrong value is not an invalid value, it is a *different operation*.
 * Nothing here is reachable from user space except the port number.
 *
 * The sequence is the reference's own five steps since 0.43.6: the gate, SET_PORT
 * general, SET_PORT qpn_calc (stage 7 -- under A0 this is where the driver TELLS the
 * firmware what the MAC table's base QPN is; it has to be in before INIT_PORT), and
 * INIT_PORT.  Each step's firmware answer has its own scalar, because "all three
 * commands succeeded" is a claim that cannot be checked against a chain that only
 * reports two of them.
 */
enum {
    kPortUpInPort = 0,
    kPortUpInMtu  = 1,   /* 0 = use the reference's own default (see below) */
    kPortUpInCount = 2,
};
CX3_STATIC_ASSERT(kPortUpInCount <= CX3_SCALAR_IN_MAX, "m3portup: inputs over the scalar cap");
enum {
    kPortUpOutStatus     = 0,
    kPortUpOutSupported  = 1,   /* from the gating QUERY_PORT */
    kPortUpOutQpFwStatus = 2,
    kPortUpOutSetFwStatus = 3,
    kPortUpOutInitFwStatus = 4,
    kPortUpOutLinkBefore = 5,
    kPortUpOutLinkAfter  = 6,
    kPortUpOutSpeedAfter = 7,
    /*
     * How far the chain got, and the numbers are the STEPS of the sequence above -- named in
     * the kPortUpStep* enum below, which is where the count lives so the tool and the verifier
     * do not each carry a copy of it:
     *
     *   0 = stopped before any write (the gate refused: not Ethernet, or QUERY_PORT
     *       did not answer)
     *   1 = the gate passed and the port is Ethernet
     *   2 = SET_PORT general answered (its status is in kPortUpOutSetFwStatus)
     *   3 = SET_PORT qpn_calc was ATTEMPTED (its status is in kPortUpOutQpnCalcFwStatus,
     *       and it is the step that can legitimately be a failure: see below)
     *   4 = INIT_PORT was attempted (status in kPortUpOutInitFwStatus)
     *   5 = every write succeeded and the link was read back
     *
     * So a failure at step N means step N was attempted and did not work, while a value
     * BELOW the step in question means it was never reached -- which is the difference
     * tools/verify.sh needs to tell "qpn_calc was refused" apart from "qpn_calc never
     * ran", and neither of those from "it ran and nobody recorded the answer".
     */
    kPortUpOutStep       = 8,
    kPortUpOutMtu        = 9,   /* what was actually sent */
    /*
     * The qpn_calc command's own answer (stage 7).  Before the call it holds
     * CX3_FWSTATUS_NOT_POSTED, so a zeroed buffer cannot read as "accepted"; and because
     * this scalar is what verify.sh reads to judge the step, a future edit that forgets
     * to write it produces the sentinel -- a visibly different answer from either 0x0 or
     * a rejection.
     */
    kPortUpOutQpnCalcFwStatus = 10,
    kPortUpOutCount      = 11,
};

CX3_STATIC_ASSERT(kPortUpOutCount <= CX3_SCALAR_OUT_MAX,
                  "m3portup: outputs over the scalar cap");

/*
 * The steps the sequence above is made of, named.  They exist because the number of steps is
 * now written down in three places -- the driver that produces it, the tool that prints "N of
 * M", and the verifier that has to tell "refused" apart from "never reached" -- and before
 * stage 7 those three were a single literal 4 in two of them.  Adding a step then made the
 * tool claim "of 4" while the device reported 5, which is the project's oldest defect shape
 * playing out in a sentence rather than in a register (docs/DEFECTS.md F11).  One home for the
 * number is the fix; the tool and the verifier both read these names.
 */
enum {
    kPortUpStepGate     = 1,   /* the gating QUERY_PORT passed and the port is Ethernet */
    kPortUpStepGeneral  = 2,   /* SET_PORT general answered */
    kPortUpStepQpnCalc  = 3,   /* SET_PORT qpn_calc was ATTEMPTED (stage 7) */
    kPortUpStepInitPort = 4,   /* INIT_PORT was ATTEMPTED */
    kPortUpSteps        = 5,   /* every write succeeded and the link was read back */
};

/*
 * kCx3SelM3AbRing -- the ring-initialisation A/B experiment.
 *
 * The input picks which of two identically-built rings is handed to SW2HW_EQ, so
 * the same experiment can be run twice with the roles swapped.  Sample times are
 * REAL wall-clock intervals measured with clock_get_uptime, not loop counts: the
 * "238 ms" this project previously recorded for the armed wait was 238 iterations
 * of {memory read + IOSleep(1)}, and one iteration is measurably not 1 ms
 * (measured: 60000 iterations of the command poll took 185.36 s, ~3.09 ms each).
 */
enum {
    kAbRingInHandOverB = 0,
    /*
     * Which EQN to hand the ring to.  0 = the default, 28, the asynchronous EQ.
     *
     * This input exists because of a measurement that needs explaining: handing a
     * ring to EQN 28 wrote the whole ring (0xcc in each slot's first half, 0xee in
     * the second), but handing a *different* ring to the *same* EQN 28 immediately
     * afterwards wrote nothing at all.  Two readings of one EQN cannot tell "the
     * firmware initialises each EQN once" apart from "it initialises once per boot"
     * -- that needs a second, fresh EQN in the same boot.  The reference's EQ bitmap
     * is seeded with the reserved range (mlx4_eq.c:1262-1277 reserves
     * reserved_eqs + num_comp_vectors + ... entries before any EQ is created), and
     * this card reports max_eq 156 with reserved_eqs 28, so numbers below 28 are not
     * the host's to use and are refused by the selector.
     */
    kAbRingInEqn = 1,
    /*
     * Allocate and deliberately LEAK this many MTT entries before building the two
     * rings, which moves them further up the MTT table (8 entries per ring, so 8 here
     * puts ring A at offset 72 instead of 64).
     *
     * This exists because of a measurement that is currently unexplained: handing a
     * ring over whose MTT entries start at offset 64 has written the ring every single
     * time, and handing one whose entries start at 72 has written nothing, nine times
     * out of nine -- including when the *same* ring identity was handed over twice in
     * a row, where the write happened both times.  So the variable is the ring, and
     * with only offsets 64 and 72 in play, "which ring was allocated first" and "which
     * MTT offset" cannot be told apart.  Moving the first ring to 72 does tell them
     * apart, and it costs one input instead of one guess.
     */
    kAbRingInDummyMtt = 2,
    kAbRingInCount = 3,
};
enum {
    kAbRingOutStatus      = 0,
    kAbRingOutSw2hwStat   = 1,
    kAbRingOutHw2swStat   = 2,
    kAbRingOutUnknown     = 3,   /* 1 = a ring was left unfreed, hardware still may hold it */
    kAbRingOutCmdUs       = 4,   /* how long SW2HW_EQ itself took */
    kAbRingOutQueryEq0    = 5,   /* QUERY_EQ fw status right after SW2HW_EQ */
    kAbRingOutQueryEq1    = 6,   /* QUERY_EQ fw status right after HW2SW_EQ */
    kAbRingOutCount       = 7,
};
CX3_STATIC_ASSERT(kAbRingInCount  <= CX3_SCALAR_IN_MAX,  "m3abring: inputs over the scalar cap");
CX3_STATIC_ASSERT(kAbRingOutCount <= CX3_SCALAR_OUT_MAX, "m3abring: outputs over the scalar cap");

/* Per-sample and per-ring detail, as a structure. */
#define CX3_AB_SAMPLES 8
#define CX3_AB_PAGES   8
#define CX3_AB_HEAD    64

typedef struct {
    uint32_t handedOver;                 /* 0 = ring A was, 1 = ring B was */
    uint32_t samples;
    uint32_t nent;
    uint32_t pages;
    uint32_t sw2hwStatus;                /* fw status of the SW2HW_EQ that ran */
    uint32_t hw2swStatus;                /* fw status of the HW2SW_EQ afterwards */
    uint32_t unknown;                    /* 1 = a ring was left unfreed on purpose */
    uint32_t cmdUs;                      /* how long SW2HW_EQ itself took, real us */
    uint32_t dummyMtt;                   /* MTT entries leaked before the rings */
    /*
     * QUERY_EQ's answer for the EQN that was just handed a ring.
     *
     * One review asked for this so that "the firmware accepted the context" can be
     * checked against "the firmware recorded what we sent" -- two different claims, and
     * this is the only observation that separates them.  [0] is taken right after
     * SW2HW_EQ, [1] right after HW2SW_EQ.
     */
    uint32_t queryEqStatus[2];           /* fw status of each QUERY_EQ */
    uint32_t queryEqWords[2][8];         /* raw context words as the firmware reported */
    uint32_t mttFirstEntry[2];           /* MTT entry values read back for each ring */
    uint32_t mttLastEntry[2];
    /*
     * The mtt_base_addr bytes actually present in the command inbox before posting.
     *
     * Read out of the buffer rather than recomputed from the expression that filled
     * it: an earlier version of this experiment inferred "the high byte is zero" from
     * the address being small, and that inference was exactly what was wrong.  A byte
     * read from the buffer cannot be wrong about what the buffer holds.
     */
    uint32_t inboxMttH;
    uint32_t inboxMttL;
    /* no-overlap evidence: the two rings' own addresses */
    uint64_t busFirst[2], busLast[2];
    uint64_t mttEntryOffset[2];
    uint64_t ringVirt[2];
    /* per ring: baseline, first post-command, and final */
    uint32_t nonzeroBase[2], nonzeroFirst[2], nonzeroFinal[2];
    uint32_t changedFirst[2], changedFinal[2];   /* bytes differing from baseline */
    uint32_t changedOffsetLo[2], changedOffsetHi[2];
    uint32_t pagesChanged[2];                    /* bitmap, CX3_AB_PAGES bits */
    uint32_t ownerSetBase[2], ownerSetFinal[2];  /* slots whose owner bit reads 1 */
    uint32_t firstChangeUs[2], lastChangeUs[2];  /* relative to command return;
                                                  * 0xffffffff = never changed */
    uint32_t sampleUs[CX3_AB_SAMPLES];
    uint32_t changedPerSample[2][CX3_AB_SAMPLES];
    /*
     * Difference between each sample and the one before it.  The baseline diff alone
     * cannot show that nothing changed in between -- this can, and it is what makes
     * "last change at +X us" a statement about a change rather than about still
     * differing from the original bytes.
     */
    uint32_t changedVsPrev[2][CX3_AB_SAMPLES];
    uint32_t nonzeroPerSample[2][CX3_AB_SAMPLES];
    uint32_t ownerSetPerSample[2][CX3_AB_SAMPLES];
    uint32_t changedPerPage[2][CX3_AB_PAGES];
    uint32_t ownerSetPerPage[2][CX3_AB_PAGES];
    uint8_t  headBefore[2][CX3_AB_HEAD];
    uint8_t  headFirst[2][CX3_AB_HEAD];
    uint8_t  headAfter[2][CX3_AB_HEAD];
} Cx3AbRingResult;

/*
 * The flag bits and the slot count have to agree, and this selector is at the hard
 * cap so a new field cannot simply be appended.  Both numbers were got wrong once in
 * this very enum -- two fields were given indices that another two already held --
 * so the relation is asserted rather than eyeballed.
 */
CX3_STATIC_ASSERT((CX3_IRQ_F_ENABLED | CX3_IRQ_F_OBJECTS | CX3_IRQ_F_STORM |
                   CX3_IRQ_F_LEGACY | CX3_IRQ_F_CLR_READY) == 0x1full,
                  "the flags word and its bit definitions disagree");

enum {
    kIrqDownOutStatus   = 0,
    kIrqDownOutIrqCount = 1,
    kIrqDownOutEqeCount = 2,
    kIrqDownOutSkipped  = 3,
    kIrqDownOutTripped  = 4,
    kIrqDownOutCount    = 5,
};

CX3_STATIC_ASSERT(kIrqUpInCount     <= CX3_SCALAR_IN_MAX,  "m3irqup: inputs over the scalar cap");
CX3_STATIC_ASSERT(kIrqUpOutCount    <= CX3_SCALAR_OUT_MAX, "m3irqup: outputs over the scalar cap");
CX3_STATIC_ASSERT(kIrqTestInCount   <= CX3_SCALAR_IN_MAX,  "m3irqtest: inputs over the scalar cap");
CX3_STATIC_ASSERT(kIrqTestOutCount  <= CX3_SCALAR_OUT_MAX, "m3irqtest: outputs over the scalar cap");
CX3_STATIC_ASSERT(kIrqStatusOutCount <= CX3_SCALAR_OUT_MAX, "m3irqstatus: outputs over the scalar cap");
CX3_STATIC_ASSERT(kIrqDownOutCount  <= CX3_SCALAR_OUT_MAX, "m3irqdown: outputs over the scalar cap");

/*
 * kCx3SelM3ClrInt -- set up the register that acknowledges a legacy interrupt.
 *
 * Runs QUERY_ADAPTER for inta_pin (the one field the mask and offset are computed
 * from), takes clr_int_base/clr_int_bar from the QUERY_FW answer this port already
 * holds, maps the region and reports the exact address and value it will write.
 *
 * Deliberately separate from opening the interrupt session: the reviews' rule is
 * that a legacy source may only be enabled once a real clear path exists, so this is
 * the prerequisite that has to succeed first, and m3irqup checks it.
 */
enum { kClrIntInCount = 0 };
enum {
    kClrIntOutStatus   = 0,
    kClrIntOutFwStatus = 1,   /* QUERY_ADAPTER's status */
    kClrIntOutIntaPin  = 2,
    kClrIntOutBar      = 3,   /* clr_int_bar, already (v>>6)*2 */
    kClrIntOutBase     = 4,   /* clr_int_base */
    kClrIntOutMask     = 5,   /* swab32(1 << (pin & 31)) */
    kClrIntOutOffset   = 6,   /* base + (pin < 32 ? 4 : 0) */
    kClrIntOutReady    = 7,
    kClrIntOutBoard0   = 8,   /* board id bytes 0..3, for the record */
    kClrIntOutBoard1   = 9,
    kClrIntOutCount    = 10,
};
CX3_STATIC_ASSERT(kClrIntOutCount <= CX3_SCALAR_OUT_MAX, "m3clrint: outputs over the scalar cap");

/* Compile-time guard on the M2d selectors' widths. */
CX3_STATIC_ASSERT(kM2dInCount     <= CX3_SCALAR_IN_MAX,  "m2dprofile: inputs over the scalar cap");
CX3_STATIC_ASSERT(kProfOutCount   <= CX3_SCALAR_OUT_MAX, "m2dprofile: outputs over the scalar cap");
CX3_STATIC_ASSERT(kBringUpOutCount <= CX3_SCALAR_OUT_MAX, "m2dbringup: outputs over the scalar cap");

/* ------------------------------------------------------------------ */
/* M3 restart 1: the event queue                                        */
/* ------------------------------------------------------------------ */

/*
 * kCx3SelM3EqUp -- measure the function's EQ allocation, then build one EQ and
 * map the events to it.
 *
 * The order is the reference's: QUERY_FUNC first (it decides which EQ numbers
 * exist and overrides what QUERY_DEV_CAP said), then the ring and its MTT, then
 * SW2HW_EQ, then MAP_EQ, then arm by writing the consumer index to the doorbell.
 *
 * inputs: nentLog2  0 means "the async EQ size, 256 entries"; otherwise 1 << n
 *         intr      the EQ's interrupt vector field.  0 is NOT "no interrupt": it
 *                   is the MSI-X table entry the reference points its asynchronous
 *                   EQ at (mlx4_main.c:2860-2874 gives the async EQ the vector for
 *                   entry 0, and mlx4_eq.c:1200-1202 creates it with intr = 0).  A
 *                   provider index, an OS IRQ number and an EQN are three different
 *                   things; only the MSI-X table entry belongs here.
 *         eqeFactor 0 = 32-byte EQEs, 1 = 64-byte (this device advertises 64)
 *         useAsyncSize 1 = use MLX4_NUM_ASYNC_EQE + spare, as the reference does
 */
enum {
    kEqUpInNentLog2    = 0,
    kEqUpInIntr        = 1,
    kEqUpInEqeFactor   = 2,
    kEqUpInUseAsync    = 3,
    /*
     * 1 = write the EQ context's mtt_base_addr as an absolute context-memory
     * address (table base + offset) instead of the byte offset inside the table
     * that the reference passes.  Only one of the two can be right, the material
     * here cannot settle which, and each attempt costs one reset-and-reinit loop
     * -- so both are reachable from one build.
     */
    kEqUpInMttAbs      = 4,
    kEqUpInCount       = 5,
};
enum {
    kEqUpOutStatus      = 0,
    kEqUpOutFwStatus    = 1,
    kEqUpOutMaxEq       = 2,   /* from QUERY_FUNC */
    kEqUpOutReservedEq  = 3,
    kEqUpOutReservedUar = 4,
    kEqUpOutEqn         = 5,
    kEqUpOutNent        = 6,
    kEqUpOutPages       = 7,
    kEqUpOutMTTOffset   = 8,
    kEqUpOutMTTAddr     = 9,
    kEqUpOutDoorbell    = 10,
    kEqUpOutRingBus     = 11,
    kEqUpOutMapEqStatus = 12,
    kEqUpOutMttAbs      = 13,
    /*
     * The ring's owner state right after arming.  These exist because "armed" is
     * not instantaneous: measured, the owner bytes are zero immediately and set a
     * moment later, and a poll in between consumes an unwritten slot.  Recording
     * it here means the arming delay is visible in the same call that arms.
     */
    kEqUpOutOwnerSet32  = 14,
    /*
     * How long the firmware took to report the ring armed.  This replaced the
     * "slot 0 owner byte" field: the wait supersedes it, because a poll that runs
     * before arming completes consumes slots that were never written, and that is
     * what hid a correct CMD EQE that had already arrived in slot 0.
     */
    kEqUpOutArmWaitMs   = 15,
    kEqUpOutCount       = 16,   /* at the scalar output cap: do not add fields */
};

/*
 * kCx3SelM3EqPoll -- the acceptance test for restart 1.
 *
 * With `fireNop` set it first posts a NOP in *event mode* (E bit set, token in
 * the token field).  That is the point of the whole step: with E clear -- which
 * is how every command before M3 was sent -- the HCA never writes a CMD EQE, so
 * a completed command proves nothing about the EQ.  With E set, completion is
 * delivered as a CMD EQE into this EQ, and finding it by polling the ring proves
 * the whole path: HCR event request, event routing, MTT, EQ DMA write, and the
 * software's own ownership/consumer-index accounting.
 *
 * inputs:  fireNop 0/1, token, consIndex to start polling from, dwords to dump
 */
enum {
    kEqPollInFireNop    = 0,
    kEqPollInToken      = 1,
    kEqPollInConsIndex  = 2,
    kEqPollInDumpDwords = 3,
    /*
     * How long to keep re-scanning the ring for the event, in ms.
     *
     * Needed because a command completing and its CMD EQE being visible are not
     * the same instant: cmdEvent() returns when the HCR status says complete, and
     * the EQE is written by the HCA's own DMA afterwards.  A single sweep would
     * then report "no event" for an event that arrives a microsecond later, which
     * is a false negative that looks exactly like a broken EQ.
     */
    kEqPollInWaitMs     = 4,
    /*
     * 1 = read the head of the queue without consuming it or moving the consumer
     * index.  In diagnostics a "look at the slot again" must not be able to lose an
     * event, and the previous version's forward scan did exactly that: it searched
     * for any ready slot and then advanced the index past everything before it.
     */
    kEqPollInPeek       = 5,
    kEqPollInCount      = 6,
};
enum {
    kEqPollOutStatus    = 0,
    kEqPollOutNopStatus = 1,   /* fw status of the event-mode NOP */
    kEqPollOutFound     = 2,   /* 1 if an EQE the software owns was read */
    kEqPollOutFoundType = 3,
    kEqPollOutFoundToken = 4,
    kEqPollOutCmdStatus = 5,
    kEqPollOutConsIndex = 6,   /* the index the EQE was found at */
    kEqPollOutNextCons  = 7,
    kEqPollOutData0     = 8,
    /* Four dwords is enough for the EQE header (type, subtype, token, status) and
     * the scalar outputs are at their hard cap of 16. */
    kEqPollMaxDwords    = 4,
    /*
     * The ring's owner map as the host sees it after arming.
     *
     * This exists because the initial state of the owner bits is hardware
     * behaviour, not something the source states: the reference zeroes the ring
     * and sets cons_index to 0, and its own readiness expression would then call
     * every slot ready -- so the driver must be relying on the HCA writing the
     * owner bits when SW2HW_EQ arms the queue.  A negative poll result is
     * uninterpretable without this: "the HCA owns every slot" (expected, armed,
     * nothing delivered) and "the host is reading the wrong memory" look the
     * same from the symptom alone.
     *
     * ownerSet32 counts the first 32 slots that carry the owner bit; ownerNext8
     * is the raw owner byte of the first eight slots, so the pattern is visible
     * rather than summarised.
     */
    kEqPollOutOwnerSet32 = kEqPollOutData0 + kEqPollMaxDwords,       /* 12 */
    kEqPollOutOwner0     = kEqPollOutOwnerSet32 + 1,                 /* 13 */
    /*
     * What the command dword actually held when it was posted.
     *
     * This is the cheapest way to settle "did our E bit reach the hardware?".
     * Read back immediately after the store: if E had been dropped -- a wrong
     * shift, a wrong dword -- the HCA would complete the command by clearing GO
     * and never write a completion EQE, which is exactly the symptom being
     * chased.  Bit 22 should be set for an event-mode command, and the token
     * should be visible in bits 31:16.
     */
    kEqPollOutHcrOnPost = 14,
    /*
     * The first "ready" slot that carried no event type, +1, or 0 if there was
     * none.  Replaced the "register value when the wait ended" field, which always
     * matched the on-post reading anyway; a slot that is ready but empty is the
     * condition that caused the walk-past, so it is worth a slot of its own.
     */
    kEqPollOutZeroSlot  = 15,
    kEqPollOutCount     = 16,   /* at the scalar output cap: do not add fields */
};

CX3_STATIC_ASSERT(kEqUpInCount   <= CX3_SCALAR_IN_MAX,  "m3equp: inputs over the scalar cap");
CX3_STATIC_ASSERT(kEqUpOutCount  <= CX3_SCALAR_OUT_MAX, "m3equp: outputs over the scalar cap");
CX3_STATIC_ASSERT(kEqPollInCount <= CX3_SCALAR_IN_MAX,  "m3eqpoll: inputs over the scalar cap");
CX3_STATIC_ASSERT(kEqPollOutCount <= CX3_SCALAR_OUT_MAX, "m3eqpoll: outputs over the scalar cap");

/* kCx3SelM3EqRing -- the ring reachability probe. */
enum {
    kEqRingInPage   = 0,
    /* 1 = intend the destructive part (see m3eqring's refusal note). */
    kEqRingInForce  = 1,
    kEqRingInCount  = 2,
};
enum {
    kEqRingOutStatus   = 0,
    kEqRingOutFwStatus = 1,
    kEqRingOutFwVer    = 2,
    kEqRingOutChanged  = 3,   /* bytes of the first 64 that differ from 0xa5 */
    kEqRingOutData0    = 4,
    kEqRingMaxDwords   = 8,   /* 32 bytes, enough for the QUERY_FW fields */
    kEqRingOutCount    = kEqRingOutData0 + kEqRingMaxDwords,   /* 12 */
};
CX3_STATIC_ASSERT(kEqRingInCount  <= CX3_SCALAR_IN_MAX,  "m3eqring: inputs over the scalar cap");
CX3_STATIC_ASSERT(kEqRingOutCount <= CX3_SCALAR_OUT_MAX, "m3eqring: outputs over the scalar cap");

/*
 * kCx3SelM3EqInfo -- read-only: what the HCA would follow for this EQ, and what
 * it thinks the EQ is.
 *
 * The ring probe showed the HCA can write into the ring's memory, but it handed a
 * bus address over directly instead of going through the MTT -- which is not the
 * path a real event takes.  So the MTT entries are read back and compared against
 * the ring's page addresses, and QUERY_EQ is attempted for the HCA's own view.
 *
 * The two arrays travel as structures: 8+8 entries plus the context exceeds the
 * 16-scalar output cap, and truncating them would remove exactly the comparison
 * this exists for.
 */
enum { kEqInfoMaxEntries = 8 };
enum {
    kEqInfoOutStatus      = 0,
    kEqInfoOutFwStatus    = 1,   /* QUERY_EQ's status, 0xffffffff = not attempted */
    kEqInfoOutRingPages   = 2,
    kEqInfoOutMTTOffset   = 3,
    kEqInfoOutMatch       = 4,   /* 1 = every MTT entry matches its page address */
    kEqInfoOutQueryEqOk   = 5,   /* 1 = the HCA answered QUERY_EQ */
    kEqInfoOutEqcBase     = 6,   /* the EQC table's HCA virtual base */
    kEqInfoOutEqcOffset   = 7,   /* byte offset of this EQN's entry inside it */
    kEqInfoOutEqcNonzero  = 8,   /* non-zero bytes in that entry */
    kEqInfoOutCount       = 9,
};
enum { kEqInfoCtxWords = 16 };   /* 64 bytes: the whole EQ context entry */
struct Cx3EqInfoResult {
    uint32_t ringBus[kEqInfoMaxEntries];    /* the ring's page bus addresses */
    uint32_t mttEntry[kEqInfoMaxEntries];   /* what the HCA would read for them */
    /*
     * One verdict per page: 1 = this page's MTT entry equals its bus address with the
     * present bit set.
     *
     * A separate array on purpose, and the reason is measured rather than stylistic.
     * The verdict used to be bit 31 of mttEntry[] itself, and bit 31 of an MTT entry is
     * also an address bit: on this card the ring's first page read as bus 0xafae4000,
     * so correct entries reported "DOES NOT MATCH", and the tool masked the same bit off
     * before printing -- showing a value the kernel never read.  The user-space tool now
     * prints mttEntry[] whole and takes the verdict from here (docs/DEFECTS.md F7,
     * docs/EQ-ORDERING.md section 4.5).
     */
    uint32_t mttMatch[kEqInfoMaxEntries];
    /*
     * The HCA's own copy of the EQ context, read straight out of the EQC table in
     * host memory.  The HCA copies the context into the table at SW2HW_EQ
     * (init_hca->eqc_base + eqn * eqc_entry_size), and the whole table is mapped,
     * so this is readable without asking the firmware anything -- which makes it
     * the most direct answer to "did SW2HW_EQ store what we sent?".
     *
     * If these bytes match the context this port built, the context is not the
     * problem; if they are zero or differ, SW2HW_EQ did not store it and nothing
     * downstream can work.
     */
    uint32_t eqcEntry[kEqInfoCtxWords];
    uint32_t eqContext[kEqInfoMaxEntries];  /* QUERY_EQ's first 32 bytes, if it answers */
};
CX3_STATIC_ASSERT(kEqInfoOutCount <= CX3_SCALAR_OUT_MAX, "m3eqinfo: outputs over the scalar cap");
/*
 * The verdict array and the value array it judges are the same length, so a page can
 * never have a verdict without a value.  This is the one property of the pair that a
 * later edit could break without the compiler noticing (adding a ninth page to one
 * array and not the other).
 */
/*
 * Written with the elaborated type specifier (`struct Cx3EqInfoResult *`) because this
 * header is also included by C translation units (tools/test-cmd-wedge.c), where the bare
 * tag name is not a type.  Both languages accept the elaborated form.
 */
CX3_STATIC_ASSERT(sizeof(((struct Cx3EqInfoResult *)0)->mttMatch) ==
                  sizeof(((struct Cx3EqInfoResult *)0)->mttEntry),
                  "m3eqinfo: mttMatch[] and mttEntry[] must stay in step");

/*
 * ------------------------------------------------------------------ */
/* R4 entrance condition: which steering mode is the HCA actually in?  */
/* ------------------------------------------------------------------ */

/*
 * QUERY_HCA -- opcode 0x0b, no input, outbox, in_modifier 0, op_modifier 0
 * (cmd.h:59; the decode is mlx4_fw.c:2097-2105).
 *
 * This exists because of a real omission found while planning R4: this port's
 * INIT_HCA writes the MC base, entry size, hash size and table size, but it never
 * wrote INIT_HCA_UC_STEERING_OFFSET -- the byte that tells the HCA "B0".  Whatever
 * the port intended, the device was told A0, and A0 is the mode where
 * mlx4_SET_PORT_qpn_calc() actually sends a command and where a unicast MAC's QPN is
 * base_qpn + mac_index.  R4's receive steering depends on knowing which mode is in
 * force, so it is read from the device rather than assumed from the capability bits.
 */
/*
 * 256 bytes, not 64, and the reason is a bug this comment exists to prevent.
 *
 * The QUERY_HCA answer puts fields out to 0x0db (log_mc_table_sz), and the first
 * version of this had a 64-byte snapshot while the decoder read 0x57, 0xc0..0xc7 and
 * 0xd2..0xdb.  For a struct member that is a read past the end of the array: b[0x57]
 * landed in the padding after it and read as zero, b[0xd8] read past the struct
 * entirely.  So the tool reported "byte 0x0d8 = 0x00, therefore A0" from a byte it had
 * never actually looked at -- a fabricated measurement, and worse than no measurement
 * because it looked like evidence.
 *
 * 256 covers every offset this decoder touches with room to spare; the decoder now
 * reads from the full command mailbox in the kernel, and this snapshot is the copy the
 * tool can inspect afterwards.
 */
#define CX3_QUERY_HCA_RAW_LEN 256

/*
 * The highest byte cx3QueryHcaDecode reads, named so the buffer size can be tied to
 * it at compile time instead of being eyeballed.  That is the fix for the class of
 * mistake described above: the two numbers were maintained separately and drifted,
 * and the symptom was a plausible-looking zero rather than a crash.
 */
#define CX3_QUERY_HCA_DECODE_MAX_OFFSET 0x0dbu
CX3_STATIC_ASSERT(CX3_QUERY_HCA_RAW_LEN > CX3_QUERY_HCA_DECODE_MAX_OFFSET,
                  "the QUERY_HCA snapshot must be wider than the decoder's last offset");

/* What Cx3QueryHcaResult.steeringMode reports.  Defined here rather than in
 * mlx4_regs.h because this header is the shared ABI and has to stand alone. */
#define CX3_STEER_A0               0u
#define CX3_STEER_B0               1u
#define CX3_STEER_DEVICE_MANAGED   2u

typedef struct {
    uint32_t status;                     /* kIOReturn for the selector itself */
    uint32_t fwStatus;
    uint32_t rawLen;
    uint8_t  raw[CX3_QUERY_HCA_RAW_LEN];
    /* decoded */
    uint32_t steeringMode;               /* CX3_STEER_* */
    uint32_t flagsDword;                 /* BE32 0x014 as the firmware reports it */
    uint32_t deviceManagedBit;           /* bit 6 of the above */
    uint32_t ucSteeringByte;             /* 0x0d8: bit 3 means B0 */
    /* the MC table fields, so the A0/B0 branch's inputs can be compared */
    uint64_t mcBase;                     /* BE64 0x0c0 */
    uint32_t logMcEntrySz;               /* 0x0d2 */
    uint32_t logMcHashSz;                /* 0x0d6 */
    uint32_t logMcTableSz;               /* 0x0db */
    uint32_t logNumQps;                  /* from the QPC block */
    uint32_t logNumCqs;
} Cx3QueryHcaResult;

/* One decode, shared by the kext and the tool. */
static inline void cx3QueryHcaDecode(const uint8_t *b, Cx3QueryHcaResult *r)
{
    uint32_t i;
    r->flagsDword = ((uint32_t)b[0x14] << 24) | ((uint32_t)b[0x15] << 16) |
                    ((uint32_t)b[0x16] << 8)  |  (uint32_t)b[0x17];
    r->deviceManagedBit = (r->flagsDword >> 6) & 1u;
    r->ucSteeringByte   = b[0xd8];
    if (r->deviceManagedBit)
        r->steeringMode = CX3_STEER_DEVICE_MANAGED;
    else if (r->ucSteeringByte & 0x08u)
        r->steeringMode = CX3_STEER_B0;
    else
        r->steeringMode = CX3_STEER_A0;
    r->mcBase = 0;
    for (i = 0; i < 8; ++i) r->mcBase = (r->mcBase << 8) | (uint64_t)b[0xc0 + i];
    r->logMcEntrySz = ((uint32_t)b[0xd2] << 8) | (uint32_t)b[0xd3];
    r->logMcHashSz  = ((uint32_t)b[0xd6] << 8) | (uint32_t)b[0xd7];
    r->logMcTableSz = b[0xdb];
    /*
     * log_num_qps is the byte at QPC+0x17 = 0x37 and log_num_cqs at CQC+0x37 = 0x57
     * (mlx4_regs.h INIT_HCA_LOG_QP_OFFSET / INIT_HCA_LOG_CQ_OFFSET, which match the
     * reference's arithmetic at mlx4_fw.c:1838-1844).
     */
    r->logNumQps    = b[0x37];
    r->logNumCqs    = b[0x57];
}

enum { kQueryHcaInCount = 0 };
enum { kQueryHcaOutStatus = 0, kQueryHcaOutCount = 1 };
/* No input assert: this selector declares no scalars in (0 is trivially within the cap). */
CX3_STATIC_ASSERT(kQueryHcaOutCount <= CX3_SCALAR_OUT_MAX, "queryhca: outputs over the scalar cap");

/* Steering mode as a word, for printing. */
static inline const char *cx3SteerName(uint32_t m)
{
    switch (m) {
    case CX3_STEER_A0:             return "A0";
    case CX3_STEER_B0:             return "B0";
    case CX3_STEER_DEVICE_MANAGED: return "DEVICE_MANAGED (DMFS)";
    default:                       return "unknown";
    }
}

/*
 * ------------------------------------------------------------------ */
/* R4 stage 1: a PD and one physical memory region                       */
/* ------------------------------------------------------------------ */

/*
 * kCx3SelR4Mr -- allocate a protection domain and enable the single "physical" memory
 * region the Ethernet driver uses for everything.
 *
 * The MR is the one from mlx4_en_main.c:211-219: iova 0, size ~0, LOCAL_READ |
 * LOCAL_WRITE, zero pages, which makes it a physical region with no page list.  Its
 * key is what every RX and TX descriptor carries as its lkey (mlx4_en_rx.c:59,
 * mlx4_en_tx.c:873), so getting the key transformation or the PD wrong produces a
 * descriptor the hardware silently cannot resolve.
 *
 * The result reports the entry as it was sent AND as it reads back out of the mapped
 * dMPT table, because "SW2HW_MPT returned 0" and "the hardware stored what was sent"
 * are different claims.
 */
typedef struct {
    uint32_t status;                     /* kIOReturn for the selector itself */
    uint32_t fwStatus;                   /* SW2HW_MPT's firmware status */
    uint32_t pdn;
    uint32_t mptIndex;
    uint32_t keySent;                    /* the MPT entry's key field as written */
    uint32_t lkey;                       /* hw_index_to_key(index): what a WQE carries */
    uint64_t iovaSent;
    uint64_t sizeSent;
    uint32_t flagsSent;
    uint32_t pdFlagsSent;
    uint32_t entitySizeSent;
    /* read back out of the mapped dMPT table, host memory, no command */
    uint32_t flagsReadback;
    uint32_t keyReadback;
    uint32_t pdFlagsReadback;
    uint32_t entitySizeReadback;
    uint64_t mttAddrReadback;
    uint32_t statusNibble;               /* flagsReadback >> 28: 0 = hardware owns it */
    uint32_t readbackOk;                 /* 1 = the entry could be read at all */
    uint32_t pdReserved;                 /* how many PDs the firmware owns */
    uint32_t mptCount;                   /* entries the profile CONFIGURED (1<<log_mpt_sz) */
    uint32_t mptMaxCap;                  /* the card's capability ceiling, for contrast */
    /*
     * QUERY_MPT's view of the same entry -- a second opinion, not a repeat.
     *
     * The host-memory readback above shows what is sitting in the table; this shows
     * what the firmware says it stored.  They are not the same claim: the fast-register
     * path has the DRIVER maintain the status byte itself (mlx4_mr.c:1018,1040), so a
     * readback that still says SW_OWNS is ambiguous between "not taken" and "nothing
     * writes it back".  Asking the firmware separates those.
     */
    uint32_t queryMptStatus;
    uint32_t queryMptOk;
    uint32_t queryFlags;
    uint32_t queryKey;
    uint32_t queryPdFlags;
    uint32_t queryEntitySize;
    uint64_t queryMttAddr;
    uint32_t queryStatusNibble;
} Cx3R4MrResult;

enum {
    kR4MrInIova = 0,        /* 0 = the reference's own value (0) */
    kR4MrInSize = 1,        /* 0 = ~0, the reference's own value */
    kR4MrInCount = 2,
};
enum {
    kR4MrOutStatus = 0,
    kR4MrOutCount  = 1,
};
CX3_STATIC_ASSERT(kR4MrInCount  <= CX3_SCALAR_IN_MAX,  "r4mr: inputs over the scalar cap");
CX3_STATIC_ASSERT(kR4MrOutCount <= CX3_SCALAR_OUT_MAX, "r4mr: outputs over the scalar cap");

/*
 * ------------------------------------------------------------------ */
/* R4 stage 2: one completion queue                                     */
/* ------------------------------------------------------------------ */

/*
 * kCx3SelR4Cq -- create the RX completion queue and hand it to the hardware.
 *
 * Two values in the context are the ones both reviews of this stage named as the places
 * where a mistake shows up as "completions never arrive" rather than as an error:
 *
 *   logsize_usrpage carries the UAR index, and the first 128 UARs belong to the EQ
 *   doorbells -- so a CQ placed there would sit on the doorbell that already works.
 *
 *   db_rec_addr is the DMA BYTE ADDRESS of the completion-index record
 *   (mlx4_cq.c:284), not an offset and not a UAR address.
 *
 * A third thing is not in the context at all and matters as much: the owner byte of
 * every slot has to be preset to 0x80 by the driver before the CQ is used.  Nothing else
 * does it for a CQ -- the EQ ring is written by the firmware, which is why that path
 * needs no such step.
 */
typedef struct {
    uint32_t status;                 /* kIOReturn for the selector itself */
    uint32_t fwStatus;               /* SW2HW_CQ's firmware status */
    uint32_t cqn;
    uint32_t nent;
    uint32_t pages;
    uint32_t mttOffset;
    uint32_t mttAddr;                /* byte offset inside the MTT table */
    uint32_t uarIndex;
    uint32_t hwUarIndex;             /* what went into logsize_usrpage's low 24 bits */
    uint64_t uarPageOffset;          /* BAR2 + this = the UAR page */
    uint64_t doorbellOffset;         /* BAR2 + this = the CQ doorbell */
    uint64_t dbRecordPhys;           /* the record's DMA byte address, as sent */
    uint32_t logsizeUsrpageSent;     /* the dword as it was encoded */
    uint32_t eqnSent;                /* comp_eqn, which is an EQ number and not a vector */
    /*
     * The ring as it stood immediately after creation: every slot's owner byte should be
     * 0x80, and a scan of all of them says whether the preset actually covered the ring.
     * This is the check for the one defect in this stage that produces a false completion
     * instead of an error.
     */
    uint32_t ownerPresetCount;       /* slots whose owner byte reads 0x80 */
    uint32_t ownerNotPresetCount;    /* slots whose owner byte does not */
    uint32_t firstNotPresetSlot;     /* 0xffffffff if none */
    /* A peek at the first few slots, so "the CQEs are at +32 in each slot" can be seen
     * rather than assumed. */
    uint8_t  slot0First[16];
    uint8_t  slot0Second[16];
    /*
     * The owner byte's exact values across the whole ring, before and after the driver's
     * preset.  [0] = slots holding 0x80, [1] = 0xcc, [2] = 0xee, [3] = 0x00,
     * [4] = anything else.  After the preset only the first two fields are meaningful:
     * [0] = 0x80 and [1] = everything else.
     *
     * This exists because "slots with bit 7 set" cannot tell the driver's preset from the
     * firmware's fill -- every fill byte seen so far has bit 7 set too.  The distinct
     * values can.
     */
    uint32_t ownerBefore[5];
    uint32_t ownerAfter[2];
    uint32_t ownerByteOffsetInSlot;   /* where the owner byte is, so it can be checked */
    /*
     * How many slots the consumer's own readiness rule finds right now, from
     * cons_index 0, without consuming anything.
     *
     * Expected: 0.  A freshly enabled CQ holds no completions, and the rule says a slot is
     * ready when its owner bit equals the consumer index's phase (mlx4_en_rx.c:768 via
     * XNOR).  With owner = 0x80 and phase = 0 that is "not ready".  Anything other than 0
     * here means the rule is inverted or the initial owner state is wrong -- and this is
     * the single measurement that stage 3 onward depends on, because a consumer that
     * starts by walking slots nobody wrote reports zeroed memory as a completion.
     */
    uint32_t readyAtStart;
} Cx3R4CqResult;

#define CX3_R4CQ_DEFAULT_NENT 512
#define CX3_R4CQ_DEFAULT_CQN  128

enum {
    kR4CqInNent = 0,        /* 0 = 512 */
    kR4CqInCqn  = 1,        /* 0 = 128, the first CQN above the firmware's reserved share */
    kR4CqInCount = 2,
};
enum {
    kR4CqOutStatus = 0,
    kR4CqOutCount  = 1,
};
CX3_STATIC_ASSERT(kR4CqInCount  <= CX3_SCALAR_IN_MAX,  "r4cq: inputs over the scalar cap");
CX3_STATIC_ASSERT(kR4CqOutCount <= CX3_SCALAR_OUT_MAX, "r4cq: outputs over the scalar cap");

/*
 * ------------------------------------------------------------------ */
/* R4 stage 3: the receive ring's buffers and WQEs                      */
/* ------------------------------------------------------------------ */

/*
 * kCx3SelR4Rq -- build the receive ring: one buffer per WQE, each WQE pointing at its
 * buffer, plus the MTT entries for the ring.
 *
 * This stage does NOT hand the ring to the hardware.  rqCreate builds memory and writes
 * descriptors; enabling the WQEs (with the producer index that tells the hardware how many
 * there are) is stage 4, together with the QP.  The split is the same one that made stages
 * 1 and 2 debuggable: "the memory is right" and "the hardware took it" stay separable, and
 * a failure names which one it was.
 *
 * Three fields decide whether a frame can ever land, and all three are silent when wrong:
 * the WQE's lkey must be the TRANSFORMED key (0x00010000 from stage 1, not the raw index
 * 256); byte_count is the buffer size the hardware may fill, not a frame length; and every
 * buffer's address must be a single bus address, i.e. no buffer straddles a page.
 *
 * This port's buffers are plain 2048-byte DMA pages rather than mbufs.  mlx4_en reaches
 * into an mbuf cluster and compensates with a 2-byte alignment at the protocol-stack
 * boundary; stage 3 is about "can a frame arrive at all", so the target is page-aligned
 * and the count is 2048 - 2.  Copying a frame out is a later decision.
 */
typedef struct {
    uint32_t status;
    uint32_t nent;
    uint32_t ringPages;
    uint32_t ringBytes;
    uint32_t mttOffset;
    uint32_t mttAddr;
    uint32_t lkey;                  /* the transformed key that went into every WQE */
    uint32_t lkeyRawIndex;          /* the MPT index it was derived from, for contrast */
    uint32_t bufBytes;
    uint32_t byteCountSent;         /* bufBytes - IP align: what the WQE says */
    uint32_t ipAlign;
    uint32_t wqeSize;               /* bytes per WQE: one data segment */
    uint32_t ringHeaderBytes;       /* the dummy section in front of WQE 0 */
    uint64_t bufPhysFirst;
    uint64_t bufPhysLast;
    uint64_t ringPhysFirst;
    uint32_t ringPhysCount;
    uint32_t mtu_msgmax;            /* the QP context field stage 4 will need */
    /* WQE 0 as it reads back out of the ring */
    uint32_t wqe0ByteCount;
    uint32_t wqe0Lkey;
    uint64_t wqe0Addr;
    uint32_t wqe0Matches;           /* 1 = all three fields are what they should be */
    /* and the last WQE, because an off-by-one in the address step is invisible in WQE 0 */
    uint32_t wqeLastByteCount;
    uint32_t wqeLastLkey;
    uint64_t wqeLastAddr;
    uint32_t wqeLastMatches;
    uint32_t bufsInOneAlloc;        /* 1 = every buffer came from one page-list allocation */
} Cx3R4RqResult;

#define CX3_R4RQ_DEFAULT_NENT 256
#define CX3_R4RQ_DEFAULT_BUF  2048

enum {
    kR4RqInNent     = 0,
    kR4RqInBufBytes = 1,   /* 0 = 2048 */
    kR4RqInCount    = 2,
};
enum { kR4RqOutStatus = 0, kR4RqOutCount = 1 };
CX3_STATIC_ASSERT(kR4RqInCount  <= CX3_SCALAR_IN_MAX,  "r4rq: inputs over the scalar cap");
CX3_STATIC_ASSERT(kR4RqOutCount <= CX3_SCALAR_OUT_MAX, "r4rq: outputs over the scalar cap");

/*
 * ------------------------------------------------------------------ */
/* R4 stage 4: the RX QP, and the receive-queue producer index          */
/* ------------------------------------------------------------------ */

/* Command-path evidence, not a device quiescence or firmware-acceptance proof.
 * Posted means the host completed its HCR GO store and write barrier; it does
 * not prove the PCIe request reached the device. Replied means the normal HCR
 * completion/status-read path was reached (including a firmware error).
 * Times are monotonic nanoseconds, meaningful only within the same boot.
 * An unattempted capture has flags=0; status is valid only with Entered set. */
#define CX3_COMMAND_OBSERVATION_ABI 1u
enum {
    kCx3CmdObservedEntered = 1u,
    kCx3CmdObservedPosted = 2u,
    kCx3CmdObservedReplied = 4u,
};
typedef struct {
    uint32_t abiVersion;
    uint32_t abiBytes;
    uint32_t flags;
    uint32_t fwStatus;
    uint64_t startNs;
    uint64_t postedNs;
    uint64_t endNs;
} Cx3CommandObservation;
CX3_STATIC_ASSERT(sizeof(Cx3CommandObservation) == 40, "command observation ABI size");

/* One qpModify/qpQuery invocation. abiVersion=0 means never invoked; an invoked
 * method stamps this header even on validation/allocation failure. preparedBytes
 * is the host mailbox region prepared for the operation (264 send / 4096 query),
 * not evidence of posting. capturedBytes is the number copied into the caller's
 * raw capture (query only on success). command preserves actual HCR-path evidence;
 * it remains all zero if cmdBox was never called. fwStatus is the method status,
 * including NOT_POSTED on early rejection. These are not quiescence credentials. */
#define CX3_QP_CAPTURE_OBSERVATION_ABI 1u
#define CX3_R4QP_ABI 1u
typedef struct {
    uint32_t abiVersion;
    uint32_t abiBytes;
    uint32_t preparedBytes;
    uint32_t capturedBytes;
    uint32_t fwStatus;
    uint32_t reserved;
    Cx3CommandObservation command;
} Cx3QpCaptureObservation;
CX3_STATIC_ASSERT(sizeof(Cx3QpCaptureObservation) == 64, "QP capture observation ABI size");

/*
 * kCx3SelR4Qp -- take one QP from RST to RTS and publish the RQ producer index.
 *
 * A QP is not handed over the way a CQ is.  There is no SW2HW_QP: it is three state
 * transitions, each carrying the whole 248-byte context, with only the state nibble
 * changing between them (mlx4_qp.c:912-926).  Two things about that shape are what this
 * stage can get wrong and neither fails to warn:
 *
 *   the context lives at mailbox +8, while the CQ context this project already sends
 *   lives at +0 (mlx4_qp.c:177-178 vs mlx4_cq.c:151).  Sending it at +0 leaves the
 *   hardware reading optpar and the first four bytes of the context as the context.
 *
 *   the MTT address and the page-size byte belong to the RST->INIT transition ONLY
 *   (mlx4_qp.c:164-169) and have to survive the other two, because all three share one
 *   buffer.  That is exactly what "one context, three states" is testing.
 *
 * The three layers below are separately readable because "the command returned 0", "the
 * state is the one asked for" and "the receive queue knows how many WQEs it has" are three
 * different claims, and a stage that reports only the first would call a rejected context
 * a success.
 */
typedef struct {
    uint32_t status;                 /* kIOReturn for the selector itself */
    /*
     * The QP number, and where it came from.  This project's rule is that a value the
     * device can be asked is read back rather than written down, so the number is derived
     * from the firmware's own reserved share (the same rule the CQN follows) and then
     * CONFIRMED by the query below: `queryLocalQpn` is what the firmware says this QP is
     * called, and `qpnConfirmed` is whether that equals what was written.
     */
    uint32_t qpn;
    uint32_t qpnRule;                /* the firmware's reserved share: the rule's input */
    uint32_t qpnProfileMax;          /* 1 << log_num_qps, the configured ceiling */
    uint32_t qpnConfirmed;           /* 1 = QUERY_QP's local_qpn is what was written */
    uint32_t pdn;
    uint32_t uarIndex;
    uint32_t cqn;
    uint32_t nent;                   /* receive WQEs, i.e. what the producer publishes */
    /* what went into the context, so the readback can be compared against it */
    uint32_t rqSizeStrideSent;
    uint32_t sqSizeStrideSent;
    uint32_t mtuMsgmaxSent;
    uint32_t counterIndexSent;
    uint32_t schedQueueSent;
    uint32_t param3Sent;
    uint32_t fcsKeep;                /* caps.flags bit 34: the input to param3's bit 29 */
    uint32_t roceAdvertised;         /* bmme_flags bit 19: logged, deliberately not used */
    uint64_t dbRecordPhys;           /* the record's DMA byte address, as sent */
    uint32_t dbRecordPage;           /* which ICM page allocation it came from */
    uint32_t mttOffset;              /* MTT entry index of the ring's first page */
    uint64_t mttAddr;                /* byte address inside the MTT table (index x 8) */
    /*
     * Layer 1: the three commands, and what the state was AFTER each one.
     *
     * "The command succeeded" and "the hardware is in that state" are different, and only
     * the query separates them -- a context whose flags nibble was overwritten between
     * steps can be accepted by all three commands in a row.  So each step is followed by
     * its own QUERY_QP rather than one query at the end.
     */
    uint32_t stepOpcode[3];          /* 0x19, 0x1a, 0x1b */
    uint32_t stepFwStatus[3];
    uint32_t stepStateWanted[3];     /* INIT (1), RTR (2), RTS (3) */
    uint32_t stepStateAfter[3];      /* the query's flags[31:28] after that step */
    uint32_t stepQueryOk[3];         /* 1 = the query itself answered */
    uint32_t stepsAccepted;          /* how many of the three returned fw status 0 */
    uint32_t statesConfirmed;        /* how many queries read back the wanted state */
    /*
     * Layer 2: the last query, field by field against what was written.  One read is doing
     * two jobs here -- is this the QP we wrote (local_qpn), and did the firmware keep the
     * fields (the queue parameters) -- and both are needed before the producer publish
     * means anything.
     */
    uint32_t queryFwStatus;
    uint32_t queryOk;
    uint32_t queryFlagsRaw;
    uint32_t queryState;             /* flags[31:28] */
    uint32_t queryLocalQpn;
    uint32_t queryRqSizeStride;
    uint32_t querySqSizeStride;
    uint32_t queryMtuMsgmax;
    uint32_t queryCqnSend;
    uint32_t queryCqnRecv;
    uint64_t queryDbRecAddr;
    uint32_t queryMttAddrH;
    uint64_t queryMttAddrL;
    uint32_t queryLogPageSize;
    uint32_t queryFieldMask;         /* bit i set = field i matched; see the mask names */
    uint32_t queryAllFieldsMatch;    /* 1 = every named field matched */
    /*
     * The MTT address and the page-size byte are reported apart from the eight named fields
     * because they are not part of the same claim: they are written by the RST->INIT
     * transition only, so a mismatch here says the shared context was disturbed between
     * steps, which is a different defect from a wrong queue parameter.
     */
    uint32_t queryMttMatches;
    /*
     * Two more fields of the context as the FIRMWARE holds it, reported on both paths.
     *
     * They are apart from the eight compared ones because the plan fixes what layer 2
     * compares (rq/sq size-stride, cqn_send, cqn_recv, db_rec_addr, mtu_msgmax, local_qpn,
     * state), and because these two have a second job: param3 carries the FCS_KEEP bit and
     * sched_queue carries the port, so seeing the firmware's copy of them is how "the
     * context this driver built is the context the hardware has" is checked for the two
     * fields whose wrong value is silent -- a frame 4 bytes long, or the other port.
     */
    uint32_t queryParam3;
    uint32_t querySchedQueue;
    /*
     * Layer 3: the delivery action of this stage.
     *
     * Publishing the RQ producer is NOT an MMIO doorbell: it is a 32-bit write into an
     * ordinary host-memory record, `*ring->wqres.db.db = cpu_to_be32(ring->prod & 0xffff)`
     * (mlx4_en_rx.c:250-254), whose DMA address is the one in the context's db_rec_addr.
     * The value is the number of WQEs the hardware may use, and it is read BACK out of
     * that memory afterwards, because a write that nobody can observe is a claim.
     */
    uint32_t producerValue;
    uint32_t producerReadback;       /* read back out of the record, big-endian decoded */
    uint32_t producerMatches;
    uint32_t producerWidth;          /* the field's width: 16 bits, not the CQ's 24 */
    uint32_t producerBefore;         /* what the record held before the write */
    /* Exact mailbox bytes for the same offline first-layer analysis as selector 35. */
    uint8_t  rawSentMailboxStep[3][264];
    uint8_t  rawQueryMailboxStep[3][4096];
    uint8_t  rawQueryMailboxFinal[4096];
    /* Versioned capture extension; old sizes are rejected before export. */
    uint32_t abiVersion;
    uint32_t abiBytes;
    Cx3QpCaptureObservation modifyCapture[3];
    Cx3QpCaptureObservation queryCapture[3];
    Cx3QpCaptureObservation finalQueryCapture;
} Cx3R4QpResult;

/* Which field failed, when queryAllFieldsMatch is 0.  Named rather than numbered so a
 * reader of the tool's output does not have to count bits. */
enum {
    kR4QpFieldState      = 0,
    kR4QpFieldLocalQpn   = 1,
    kR4QpFieldRqSize     = 2,
    kR4QpFieldSqSize     = 3,
    kR4QpFieldCqnSend    = 4,
    kR4QpFieldCqnRecv    = 5,
    kR4QpFieldDbRecAddr  = 6,
    kR4QpFieldMtuMsgmax  = 7,
};

enum {
    kR4QpInQpn = 0,      /* 0 = the rule's value (the firmware's reserved share) */
    kR4QpInCount = 1,
};
/*
 * The scalars, and why every layer has one.
 *
 * docs/R4-STAGE4-PLAN.md section 7 asks for the three layers to be readable as VALUES, and
 * the verifier reads values out of the scalar path -- deliberately, because a harness that
 * decodes its own ABI structures is one more place to be wrong (that is why `fault` reports
 * its poison bit through `probe` rather than through `survey`).  So each layer gets a
 * scalar that can be asserted without parsing anything, and the structure carries the
 * detail a human reads when a scalar says no.
 */
enum {
    kR4QpOutStatus          = 0,  /* kIOReturn, 0 = the selector ran */
    kR4QpOutQpn             = 1,
    kR4QpOutStepsAccepted   = 2,  /* how many of the three commands returned fw status 0 */
    kR4QpOutStatesConfirmed = 3,  /* how many per-step queries read back the wanted state */
    kR4QpOutQueryOk         = 4,  /* the last QUERY_QP answered at all */
    kR4QpOutQueryState      = 5,  /* its flags[31:28]; RTS is 3 */
    kR4QpOutQpnConfirmed    = 6,  /* its local_qpn equals the QPN written */
    kR4QpOutFieldsMatch     = 7,  /* all eight compared context fields matched */
    kR4QpOutMttMatches      = 8,  /* the RST->INIT-only fields survived the other two steps */
    kR4QpOutProducerValue   = 9,  /* the value written into the record */
    kR4QpOutProducerReadback = 10,/* what reading the record back gives */
    kR4QpOutProducerMatches = 11, /* those two are equal */
    kR4QpOutCount           = 12,
};
/*
 * "This layer was never reached", written into every measured scalar before anything is
 * measured.  Zero would be the wrong default for the same reason it is everywhere else in
 * this file: a call that failed before it asked the device leaves the structure zeroed, and
 * a chain reading these has to be able to tell "measured NO" from "never measured".  It
 * matters here in particular because two of the assertions are equalities -- producerValue
 * against producerReadback -- and two zeros satisfy them.
 */
#define CX3_R4QP_NOT_MEASURED 0xffffffffu

/*
 * ------------------------------------------------------------------ */
/* R4 stage 5: the MAC table, and the RSS indirection QP                */
/* ------------------------------------------------------------------ */

#define CX3_R4INGRESS_ABI 3
/* 8-byte prefix, 248-byte hardware QPC, plus 8 cleared padding bytes. */
#define CX3_QP_MODIFY_CAPTURE_BYTES 264u

/*
 * kCx3SelR4Ingress -- two independent things that together make received traffic reach the
 * queue stage 4 put in RTS:
 *
 *   1. the port's MAC table, so the firmware recognises this card's own address; and
 *   2. an RSS indirection QP, whose hash target is stage 4's RX QP.
 *
 * Neither one produces a completion event, which is what makes this stage different from the
 * ones before it: "frames are arriving" cannot be the evidence here, because nothing in this
 * step is supposed to make a frame arrive yet.  So both halves are read back:
 *
 *   the MAC table  the reference never reads it back -- it keeps its own copy in software
 *                  (mlx4_port.c:182-340) -- so the read direction is this port's own idea:
 *                  the same SET_PORT command with in_param 0 asks the firmware to put the
 *                  table it holds into the outbox.  Whether that works is a measurement, and
 *                  if it does not the field says so rather than pretending: a failed
 *                  read-back is `macReadbackOk = 0`, and the stage's evidence degrades to the
 *                  command status.  It is deliberately NOT reported as "the MAC was not
 *                  registered", which is a claim this call cannot establish.
 *
 *   the indirection QP  three state transitions again (RST->INIT->RTR->RTS, the same
 *                  mlx4_qp_to_ready walk stage 4 used), each followed by its own QUERY_QP,
 *                  and then the one field that carries the stage's whole point: the
 *                  firmware's `default_qpn` has to be the QPN of the queue that actually
 *                  receives.  `base_qpn` has to agree with it too -- with a single ring its
 *                  high byte is ilog2(1) = 0, so the two fields coincide by construction
 *                  (decision 3 in docs/R4-STAGE5-PLAN.md), which is why this stage can check
 *                  the high byte but NOT the high-byte encoding.
 *
 * The failure this exists to catch is "the indirection QP points at itself": a QP whose
 * default_qpn is its own number is accepted by every command in the sequence, reaches RTS,
 * and steers hashed traffic into a queue with no receive ring at all.  `pointsAtAnotherQp`
 * is that question as one number.
 */
typedef struct {
    /*
     * Structure ABI: 2 adds the exact mailbox captures.  The user client stamps both fields
     * after the selector returns, so a new tool against an old kext sees zero and refuses
     * instead of reading zeros as measurements.  A new kext against an old tool is rejected
     * by the existing structureOutputSize check.
     */
    uint32_t abiVersion;
    uint32_t abiBytes;
    uint32_t status;                 /* kIOReturn for the selector itself */
    /* --- the MAC table --- */
    uint32_t macFwStatus;            /* the write direction's fw status */
    /*
     * The slot the entry was written into, MEASURED out of the table that was sent (the first
     * non-empty entry of its 128), not restated from the driver's decision.  The number the
     * verifier reads is therefore the number the firmware was handed: the revision that had a
     * "find the first empty slot" scan running after the address had been placed reported 1
     * while the command carried the address in slot 1 and the driver believed 0
     * (docs/DEFECTS.md D12).
     *
     * What this field does NOT say: which QPN the entry selects.  The firmware's rule is
     * `base_qpn + mac_index` with base_qpn = mlx4_get_base_qpn() = the ETH_ADDR region's base
     * (mlx4_port.c:372-375), which is computed from devcap counts this port does not decode
     * (mlx4_qp.c:777-794, mlx4_main.c:545-548).  "Slot 0 selects the RX QP of stage 4" is not
     * established; see docs/R4-STAGE5-RESULT.md for what that leaves to stage 7.
     */
    uint32_t macSlot;
    uint32_t macTableBytes;          /* 1024: MLX4_MAC_TABLE_SIZE, all of it, in the mailbox */
    uint32_t macEntries;             /* 128: MLX4_MAX_MAC_NUM */
    uint8_t  mac[8];                 /* the entry as sent: BE64, the valid bit in byte 0 */
    uint64_t macValue;               /* the same entry's low 48 bits, i.e. the address */
    uint8_t  macReadRaw[8];          /* the read direction's 8 bytes, zero when it did not answer */
    uint32_t macReadFwStatus;
    /*
     * 1 = the read direction returned content that can be compared.  0 covers BOTH ways it can
     * fail to be usable, and macReadFwStatus says which: no answer at all (a non-zero fw status),
     * or an answer whose outbox held only zeros (fw status 0).  The second is what this card did
     * on 2026-10-12 07:16 -- and an empty outbox is not a table, so neither case supports the
     * claim "the firmware holds a different table".
     */
    uint32_t macReadbackOk;
    uint32_t macReadbackMatches;     /* only meaningful when macReadbackOk is 1 */
    /* --- the indirection QP --- */
    uint32_t qpn;                    /* the entry QP's own number (derived, then confirmed) */
    uint32_t qpnRule;                /* the firmware's reserved share: the rule's input */
    uint32_t qpnProfileMax;          /* 1 << log_num_qps */
    uint32_t rxQpn;                  /* stage 4's RX QP: what the hash must target */
    uint32_t pdn;
    uint32_t uarIndex;
    uint32_t cqn;
    uint32_t rssRings;               /* 1: single-ring RSS for this stage */
    uint32_t hashFnSent;             /* MLX4_RSS_HASH_TOP */
    uint32_t rssFlagsSent;           /* IPV4|TCP_IPV4|IPV6|TCP_IPV6 */
    uint32_t rssKeyBytes;            /* 40 */
    uint32_t baseQpnSent;            /* ilog2(rings) << 24 | rxQpn */
    uint32_t defaultQpnSent;         /* rxQpn */
    uint32_t baseQpnUdpSent;         /* 0: the profile does not ask for UDP RSS */
    uint32_t rqSizeStrideSent;       /* 0 for an indirection QP: the fill skips it */
    uint32_t sqSizeStrideSent;
    uint32_t mtuMsgmaxSent;
    uint32_t counterIndexSent;
    uint32_t schedQueueSent;
    uint32_t param3Sent;
    uint32_t mttOffset;
    uint64_t mttAddr;
    /*
     * The producer record, and the one field of this stage that is a judgement call rather
     * than a measurement.
     *
     * The indirection QP carries a db_rec_addr because the fill always writes it
     * (mlx4_en_resources.c:78) -- but not the value the RX ring's context carries: the fill
     * writes `res.db.dma << 2` and nothing overwrites it for this QP, while the RX ring's own
     * line overwrites it with the unshifted address (mlx4_en_rx.c:947).  Both facts are
     * recorded rather than reconciled: the shifted form is what the shipping reference sends,
     * and the field's semantics are not documented anywhere in the tree.  This QP produces no
     * completions, so the field is expected not to be read at all; the record is still
     * allocated, cleared and filled with the same producer value, so that whichever form the
     * firmware were to follow points at memory this driver owns.  The falsification condition
     * is in docs/R4-STAGE5-RESULT.md: if the entry QP cannot reach RTS and this is the only
     * suspicious field, send the unshifted address instead.
     */
    uint64_t dbRecordPhys;           /* the record's real DMA byte address */
    uint64_t dbRecAddrSent;          /* what went into the context: dbRecordPhys << 2 */
    uint32_t dbRecAddrShifted;       /* 1 = the shifted form was sent (the reference's) */
    uint32_t producerValue;
    uint32_t producerReadback;
    uint32_t producerMatches;
    /* --- layer 1: the three transitions, each followed by its own QUERY_QP --- */
    uint32_t stepOpcode[3];
    uint32_t stepFwStatus[3];
    uint32_t stepStateWanted[3];
    uint32_t stepStateAfter[3];
    uint32_t stepQueryOk[3];
    uint32_t stepsAccepted;
    uint32_t statesConfirmed;
    /* --- layer 2: the last query, field by field --- */
    uint32_t queryFwStatus;
    uint32_t queryOk;
    uint32_t queryFlagsRaw;
    uint32_t queryState;             /* flags[31:28]; RTS is 3 */
    uint32_t queryLocalQpn;
    uint32_t queryRssHighByte;       /* base_qpn's top byte: ilog2(rings), 0 for one ring */
    uint32_t queryBaseQpn;
    uint32_t queryDefaultQpn;
    uint32_t queryBaseQpnUdp;
    uint32_t queryHashFn;
    uint32_t queryRssFlags;
    uint32_t queryReserved;          /* the overlay's u16 at 0x44 */
    uint32_t queryRssKeyMatches;     /* 1 = the 40 bytes came back as sent */
    uint32_t queryBaseQpnMatches;    /* 1 = base_qpn is rxQpn with the high byte 0 */
    uint32_t queryDefaultQpnMatches; /* 1 = default_qpn is rxQpn */
    uint32_t queryRssMatches;        /* 1 = all of the above: the overlay survived */
    uint32_t queryRqSizeStride;
    uint32_t querySqSizeStride;
    uint32_t queryMtuMsgmax;
    uint32_t queryCqnSend;
    uint32_t queryCqnRecv;
    uint64_t queryDbRecAddr;         /* the firmware's copy of this QP's db_rec_addr */
    uint32_t queryFieldMask;         /* bit i set = field i matched; see the mask names */
    uint32_t queryAllFieldsMatch;
    uint32_t queryMttMatches;
    uint32_t queryParam3;
    uint32_t querySchedQueue;
    uint32_t pointsAtAnotherQp;      /* 1 = the entry QPN is not its own default_qpn */
    /*
     * Raw bytes, for offline analysis (T03/P1-R).
     *
     * These legacy 256-byte slots hold the 248-byte hardware context plus 8 zero padding
     * bytes. They are NOT a verbatim 256-byte hardware context; use full mailboxes for
     * byte-level evidence including bytes outside the context.
     * rawSentQpc is the context this driver handed to qpModify, before the three state
     * transitions.  It is all-zero on the idempotent path, which sends nothing and therefore
     * has no "what went in" to report -- the caller zeroes the whole result first, and this
     * field is left alone there.
     *
     * rawQueriedQpc is the last QUERY_QP answer verbatim, in device byte order.  Both are
     * copied only after the operation they describe succeeded; a failed query leaves the
     * queried block zero, which is distinguishable from a real all-zero context by queryOk.
     */
    uint8_t  rawSentQpc[256];
    uint8_t  rawQueriedQpc[256];
    /*
     * Exact command buffers, not reconstructions from the QPC.  Each transition's outbound
     * mailbox is captured after construction and before cmdBox; each successful query's whole
     * 4 KiB answer is captured before the 248-byte context at +8 is decoded. These blocks close the
     * gap between "the context we think we sent" and "the bytes the HCR was handed", and
     * between "the context field" and "the firmware's whole reply".  A failed query leaves
     * its answer zeroed; stepFwStatus/queryOk distinguish that from a real zero answer.
     */
    uint8_t  rawSentMailboxStep[3][264];   /* prefix + 248-byte QPC + 8 zero padding */
    uint8_t  rawQueryMailboxStep[3][4096];
    uint8_t  rawQueryMailboxFinal[4096];
    Cx3QpCaptureObservation modifyCapture[3];
    Cx3QpCaptureObservation queryCapture[3];
    Cx3QpCaptureObservation finalQueryCapture;
} Cx3R4IngressResult;

/*
 * Which field failed, when queryAllFieldsMatch is 0.  The overlay's own fields are judged by
 * queryRssMatches and its two halves instead, because "the queue parameters came back" and
 * "the hash target came back" are different claims with different consequences.
 *
 * rq_size_stride is deliberately NOT in this list: for an indirection QP the fill does not
 * write it at all (mlx4_en_resources.c:52), so what the firmware holds there is not something
 * this driver sent and therefore not something it can compare.
 */
enum {
    kR4IngressFieldState      = 0,
    kR4IngressFieldLocalQpn   = 1,
    kR4IngressFieldSqSize     = 2,
    kR4IngressFieldCqnSend    = 3,
    kR4IngressFieldCqnRecv    = 4,
    kR4IngressFieldMtuMsgmax  = 5,
};

enum {
    kR4IngressInCount = 0,      /* no inputs: everything comes from the staged state */
};

/*
 * The scalars.  Every judged claim of this stage is one of these, so the verifier decides
 * from values without decoding the structure -- the rule every other stage follows, and the
 * reason `fault` reports its poison bit through `probe`.
 *
 * The two directions of the MAC command get separate scalars because they are two commands
 * and either can be the one that failed.  `kR4IngressOutMacReadbackOk` is what keeps the
 * read-back's absence from being read as a mismatch: 0 there means "not measured", which the
 * verifier reports as SKIP rather than as a failed judgement.  0 covers both the silent command
 * and the answered-with-an-all-zero-outbox case, and the kernel log says which; what it must
 * never do is let "the outbox carried nothing" be reported as "the firmware holds something
 * else", which is what the verifier did until 0.43.4 (docs/DEFECTS.md D12).
 */
enum {
    kR4IngressOutStatus            = 0,  /* kIOReturn, 0 = the selector ran */
    kR4IngressOutMacStatus         = 1,  /* the MAC write's fw status (0 = accepted) */
    kR4IngressOutMacSlot           = 2,  /* the slot used; 0 is the expected answer */
    kR4IngressOutMacReadbackOk     = 3,  /* 1 = the read direction answered at all */
    kR4IngressOutMacReadbackMatch  = 4,  /* 1 = its slot equals what was written */
    kR4IngressOutQpn               = 5,  /* the indirection QP's number */
    kR4IngressOutStepsAccepted     = 6,
    kR4IngressOutStatesConfirmed   = 7,
    kR4IngressOutQueryOk           = 8,
    kR4IngressOutQueryState        = 9,  /* RTS is 3 */
    kR4IngressOutRssDefaultMatches = 10, /* the firmware's default_qpn is the RX QPN */
    kR4IngressOutRssBaseMatches    = 11, /* its base_qpn is too, with the high byte 0 */
    kR4IngressOutRxQpn             = 12, /* stage 4's RX QPN: both of the above are this */
    kR4IngressOutQueryDefaultQpn   = 13, /* what the firmware says, for the log reader */
    kR4IngressOutQueryBaseQpn      = 14,
    kR4IngressOutCount             = 15,
};
/*
 * "This was never reached", for the same reason stage 4 has its own: a call that refuses
 * before asking the device leaves zeros behind, and several of these assertions are
 * equalities that zeros would satisfy.
 */
#define CX3_R4INGRESS_NOT_MEASURED 0xffffffffu

/*
 * kCx3SelR4Tx -- R4 stage 6: the transmit side's resources, and the space they must stay
 * out of.
 *
 * What this stage is NOT: a successful transmission.  Nothing is posted to the send queue,
 * no doorbell is rung, and no frame leaves the card.  The whole claim is that three resources
 * exist and are measurable -- a TX completion queue the firmware accepted, a send QP that
 * went RST -> INIT -> RTR -> RTS with each step read back, and a send queue whose memory the
 * QP's context names.
 *
 * Three things the plan names as "check yourself first", and where each is answered:
 *
 *   sq_size_stride = 0x42.  The reference's encoding is ilog2(entries) << 3 |
 *       (ilog2(stride) - 4) (mlx4_en_resources.c:54-57) and a send queue's unit is the 64-byte
 *       TXBB (en.h:84), so 256 TXBBs give 0x42 -- not the 0x02 a receive QP carries for its
 *       dummy SQ, and not 0x40 (the RQ's 256 WQEs of 16 bytes).  sqSizeStride is read out of
 *       the context image that was sent and the readback is compared against it.
 *   the UAR.  usrPage is the same runtime index the CQ uses (reservedUarStart(), which
 *       derives it from uar_page_sz and reserved_uars and logs both inputs), never a literal.
 *   number collisions.  numberSafe is one bit over four separate comparisons: both numbers
 *       below topReservedStart, and the QPN different from the RX QP's and the indirection
 *       QP's, and the CQN different from the RX CQN's.  Each of the four is also reported on
 *       its own, so a failure says which one it was.
 *
 * Two decisions are visible in the field list rather than buried:
 *
 *   db_rec_addr is the UNSHIFTED record address, exactly as stage 4 sends for the RX QP
 *       (mlx4_en_rx.c:947) and deliberately not the << 2 form the reference's shared fill
 *       writes for anything else (mlx4_en_resources.c:78, which is what stage 5 copies for
 *       the indirection QP).  dbRecAddrShifted is therefore 0.  Note the divergence honestly:
 *       the reference's TX ring DOES receive the shifted form -- nothing overwrites it on
 *       that path the way mlx4_en_rx.c:947 overwrites it for the RX ring.
 *       docs/R4-STAGE6-RESULT.md records it with a falsification condition.
 *   param3's FCS_KEEP bit is NOT set.  The reference sets it in mlx4_en_config_rx_qp
 *       (mlx4_en_rx.c:950-951), not in the shared fill, and the bit is about received frames.
 *       param3Sent is 0 here and the log says why, so "the FCS bit is missing" cannot be read
 *       as an omission.
 */

typedef struct {
    uint32_t status;                 /* kIOReturn for the selector itself */
    /* --- the send CQ --- */
    uint32_t cqn;                    /* the number this stage chose, below the top line */
    uint32_t cqNent;                 /* entries this driver asked for */
    uint32_t cqPages;
    uint32_t cqMttOffset;
    uint32_t cqFwStatus;             /* SW2HW_CQ's answer */
    /*
     * What the consumer's own readiness rule finds on a freshly created queue.  Zero is the
     * only correct answer: a slot whose owner byte reads as the consumer's phase would be a
     * completion out of memory nobody wrote, and this number is how "the ring is empty" is
     * measured rather than assumed.
     */
    uint32_t cqReadyAtStart;
    uint64_t cqMttAddr;
    uint64_t cqDbRecordPhys;         /* the CQ's own completion-index record */
    /* --- the two numbers, and the space they must stay out of --- */
    uint32_t qpn;                    /* the send QP's number (derived, then confirmed) */
    uint32_t qpnRule;                /* the firmware's reserved share: the rule's input */
    uint32_t qpnProfileMax;          /* 1 << log_num_qps */
    uint32_t numQps;                 /* = qpnProfileMax, named for the arithmetic below */
    uint32_t logMaxMacs;             /* QUERY_PORT outbox 0x0a, low nibble */
    uint32_t topReservedStart;       /* numQps - (1 << logMaxMacs): where the top 128 begin */
    uint32_t qpnBelowTop;            /* 1 = qpn < topReservedStart */
    uint32_t cqnBelowTop;            /* 1 = cqn < topReservedStart */
    uint32_t qpnCollidesRx;          /* 1 = it IS the RX QP's number (a collision) */
    uint32_t qpnCollidesIngress;     /* 1 = it IS the indirection QP's number */
    uint32_t cqnCollidesRx;          /* 1 = it IS the RX CQ's number */
    uint32_t numberSafe;             /* 1 = no collision and both numbers fit below the top */
    uint32_t rxQpn;                  /* stage 4's RX QPN: one of the four comparisons */
    uint32_t ingressQpn;             /* stage 5's indirection QPN */
    uint32_t rxCqn;                  /* stage 2's CQN */
    /* --- the send queue's memory --- */
    uint32_t sqNent;                 /* TXBBs in the WQE array (every one of them a slot) */
    uint32_t sqStride;               /* MLX4_TXBB_SIZE, 64 */
    /*
     * 0 since 0.43.8, and a fact rather than a placeholder: a send queue's WQE 0 is at its
     * MTT base (mlx4_en_tx.c:139, :686) where the receive ring's is 64 bytes past its own
     * (mlx4_en_rx.c:406, :482-489).  0.43.7 built this queue with the receive ring's 64 and
     * the hardware executed nothing -- docs/DEFECTS.md E13.
     */
    uint32_t sqHeaderBytes;          /* SQ_WQE0_OFFSET, 0: no unit in front of the WQEs */
    uint32_t sqBytes;                /* nent * stride: the header used to be added here */
    uint32_t sqPages;
    uint32_t sqMttOffset;
    uint64_t sqMttAddr;              /* index x MLX4_MTT_ENTRY_SIZE, as the context wants it */
    uint64_t sqPhysFirst;
    /*
     * The initial ownership state of the send queue's memory, which since 0.43.6 is the
     * reference's: EVERY 64-byte TXBB's first dword carries the transmit stamp with owner = 1
     * -- `STAMP_VAL | (1 << STAMP_SHIFT)` = 0xffffffff, "software owns this slot"
     * (mlx4_en_netdev.c:1373-1375, en.h:95-99).  Zero would say bit 31 = 0 = "hardware owns",
     * i.e. every slot in flight (docs/DEFECTS.md C12).
     *
     * Three numbers rather than one, because a single "the stamp is right" bit cannot say
     * whether the whole array was stamped or only the header:
     */
    uint32_t sqStampValue;           /* WQE 0's first dword (the allocation's first 4 bytes),
                                      * READ OUT of memory */
    uint32_t sqStampMatches;         /* 1 = that value is MLX4_SQ_INIT_OWNER_BIT (0xffffffff) */
    uint32_t sqTxbbsStamped;         /* how many of the sqNent WQE TXBBs carry it: wanted sqNent */
    /*
     * Bytes in the WQE array that are neither the ownership stamp nor part of one: i.e.
     * non-zero bytes EXCLUDING each TXBB's first dword.  This is the "was a descriptor posted"
     * question, and 0 is the right answer -- the stamps are 4 bytes of every 64 and are not
     * counted, which is what the pre-0.43.6 "the array is all zero" check could no longer mean
     * once the array is stamped the way the reference stamps it.
     */
    uint32_t sqWqeNonZeroBytes;
    uint32_t sqWqeUntouched;         /* 1 = sqWqeNonZeroBytes == 0, i.e. no descriptor body */
    /* --- the context, as written --- */
    uint32_t pdn;
    uint32_t uarIndex;               /* the runtime UAR index (shared with the CQs) */
    uint32_t rqSizeStrideSent;       /* 0 for a send QP: the fill does not write it */
    uint32_t sqSizeStrideSent;       /* 0x42 for 256 TXBBs of 64 bytes */
    uint32_t mtuMsgmaxSent;
    uint32_t counterIndexSent;       /* 0xFF: the reference's "no counter" */
    uint32_t schedQueueSent;
    uint32_t param3Sent;             /* 0: FCS_KEEP belongs to the receive side */
    uint64_t dbRecordPhys;           /* the record's real DMA byte address */
    uint64_t dbRecAddrSent;          /* what went into the context: the UNSHIFTED address */
    uint32_t dbRecAddrShifted;       /* 0 -- unlike the indirection QP of stage 5 */
    uint32_t isTxSent;               /* 1: the record of which fill branch was taken */
    /* --- layer 1: the three transitions, each followed by its own QUERY_QP --- */
    uint32_t stepOpcode[3];
    uint32_t stepFwStatus[3];
    uint32_t stepStateWanted[3];
    uint32_t stepStateAfter[3];
    uint32_t stepQueryOk[3];
    uint32_t stepsAccepted;
    uint32_t statesConfirmed;
    /* --- layer 2: the last query, field by field --- */
    uint32_t queryFwStatus;
    uint32_t queryOk;
    uint32_t queryFlagsRaw;
    uint32_t queryState;             /* flags[31:28]; RTS is 3 */
    uint32_t queryLocalQpn;
    uint32_t queryRqSizeStride;      /* the firmware's copy: expected 0 */
    uint32_t querySqSizeStride;      /* the firmware's copy: expected 0x42 */
    uint32_t queryMtuMsgmax;
    uint32_t queryCqnSend;
    uint32_t queryCqnRecv;
    uint64_t queryDbRecAddr;         /* the firmware's copy of this QP's db_rec_addr */
    uint32_t queryLogPageSize;
    uint32_t queryMttAddrH;
    uint32_t queryMttAddrL;
    uint32_t queryFieldMask;         /* bit i set = field i matched; see the mask names */
    uint32_t queryAllFieldsMatch;
    uint32_t queryMttMatches;
    uint32_t qpnConfirmed;           /* 1 = the firmware's local_qpn is the number written */
    uint32_t queryParam3;            /* reported, not judged: no reference to compare with */
    uint32_t querySchedQueue;        /* same */
    /*
     * pri_path.feup (QPC + 0x3a), the field this batch added to the fill, and the ONE field on
     * the send side where the reference's shared fill gives a send queue something it does not
     * give a receive queue (mlx4_en_resources.c:62-65).  Three numbers rather than one,
     * because "we wrote it" and "the firmware kept it" are different claims and this project
     * has already measured one of them being false on this very context: param3 was written as
     * 0x20000000 and QUERY_QP answered 0x0 (docs/DEFECTS.md F10, stage 4's run).  If the
     * firmware treats pri_path the same way, the value below is where that shows.
     *
     * REPORTED, NOT JUDGED -- not part of queryFieldMask, for the same reason param3 and
     * sched_queue are not: no reference line reads this field back, so there is nothing to
     * compare a readback against, and failing on an unknown would report the unknown as known.
     */
    uint32_t feupSent;               /* the byte the fill put in the context image */
    uint32_t queryFeup;              /* the firmware's copy after RTS (see queryOk) */
    uint32_t feupSurvived;           /* 1 = the firmware's copy equals what was written */
} Cx3R4TxResult;

/*
 * Which field failed, when queryAllFieldsMatch is 0.
 *
 * The eight are the ones the fill writes (or deliberately leaves) and that the reference's
 * semantics require to survive: the state, the QP's own number, both size/stride bytes, the
 * MTU, both CQNs and the record address.  param3 and sched_queue are NOT here, for the reason
 * stage 4 recorded after measuring it: the reference never reads either back, so there is
 * nothing to compare them against, and failing on an unknown would report the unknown as
 * known.  Both are printed beside the firmware's copy instead.
 */
enum {
    kR4TxFieldState      = 0,
    kR4TxFieldLocalQpn   = 1,
    kR4TxFieldRqSize     = 2,
    kR4TxFieldSqSize     = 3,
    kR4TxFieldMtuMsgmax  = 4,
    kR4TxFieldCqnSend    = 5,
    kR4TxFieldCqnRecv    = 6,
    kR4TxFieldDbRecAddr  = 7,
};

enum {
    kR4TxInCqNent = 0,   /* 0 = this driver's default (CX3_R4TX_DEFAULT_CQ_NENT) */
    kR4TxInSqNent = 1,   /* 0 = this driver's default (CX3_R4TX_DEFAULT_SQ_TXBB) */
    kR4TxInCount  = 2,
};

/*
 * The scalar block, filled to the kernel's 16 (see the note beside CX3_SCALAR_OUT_MAX): and
 * the three facts of this stage's own experiment are still three separate values.
 *
 * WHY THIS LIST CHANGED IN 0.43.10, and what it costs.  In 0.43.9 the cap was raised to 20 to
 * make room for the feup trio, which made every call to this selector fail with
 * kIOReturnBadArgument before it reached the driver (docs/DEFECTS.md F13).  Sixteen is the
 * ceiling, so three entries had to go somewhere, and the order of preference is the rule this
 * project already uses: a JUDGED claim keeps its own scalar; a number that is only context for
 * a message travels in the structure, which the kernel hands over whole and `cx3ctl r4tx`
 * prints.  So:
 *
 *   - `cqn` and `topReservedStart` are no longer scalars.  They are the two numbers the CQN and
 *     the top-reserved-line messages are about: context, not verdicts, and their verdict
 *     (`kR4TxOutNumberSafe`) stays a scalar and still decides.  Both remain in Cx3R4TxResult,
 *     printed by `cx3ctl r4tx` and by the kernel log line that states the arithmetic.  The one
 *     assertion the verifier made on a number rather than on a verdict -- "the QPN is not 0" --
 *     is on kR4TxOutQpn, which stays here.
 *
 *   - `stepsAccepted` and `statesConfirmed` share one scalar (steps low, states high, shift
 *     kR4TxOutStatesShift).  This is the one packing this stage does and it is LOSSLESS: two
 *     counts of 0..3, each in its own half of a 64-bit value, so both numbers are read unchanged
 *     and neither can be mistaken for the other.  They are always read together -- the
 *     verifier's own line is "3/3 commands accepted, 3/3 states read back after each step" --
 *     and they are measured by the same loop, so no state can be lost by putting them side by
 *     side.  When the loop never ran the whole scalar is CX3_R4TX_NOT_MEASURED.
 *
 * WHY THE 0/1 VERDICTS WERE NOT THE ONES PACKED (the alternative that looks cheaper: a bitfield
 * holding cqReady / numberSafe / queryOk / sqStampMatches, which would have freed the three
 * slots on its own).  Every one of those four is published as 0, 1, or -- when the call refused
 * before the fact was measured -- CX3_R4TX_NOT_MEASURED, and the sentinel is PER-FACT: a refusal
 * after the CQ is built publishes cqReady and numberSafe as measured while the layer-2 facts are
 * still sentinel.  A bit cannot hold three states, so packing them would make "nothing was
 * measured here" read as "the measurement came out wrong" -- docs/DEFECTS.md D10, and the same
 * rule that makes the three layer-2 equalities below keep their own scalars.
 */
enum {
    kR4TxOutStatus          = 0,  /* kIOReturn, 0 = the selector ran */
    kR4TxOutCqFwStatus      = 1,  /* SW2HW_CQ's answer (0 = accepted) */
    kR4TxOutCqReady         = 2,  /* readable slots on the fresh CQ: 0 is the only right answer */
    kR4TxOutQpn             = 3,  /* the TX QP's number */
    kR4TxOutNumberSafe      = 4,  /* 1 = both numbers are below the top-reserved line and collide
                                   * with nothing that exists.  The line itself
                                   * (numQps - (1 << logMaxMacs)) and the CQN are in
                                   * Cx3R4TxResult, printed by `cx3ctl r4tx` */
    kR4TxOutStepsStates     = 5,  /* stepsAccepted | statesConfirmed << kR4TxOutStatesShift */
    kR4TxOutQueryOk         = 6,
    kR4TxOutQueryState      = 7,  /* RTS is 3 */
    kR4TxOutQpnConfirmed    = 8,  /* the firmware's local_qpn is the number we wrote */
    kR4TxOutFieldsMatch     = 9,  /* all eight compared fields */
    kR4TxOutMttMatches      = 10, /* the MTT address survived RST->INIT and the other two */
    kR4TxOutSqSizeStride    = 11, /* the byte as sent: 0x42 for 256 TXBBs of 64 bytes */
    kR4TxOutSqStampMatches  = 12, /* WQE 0's first dword came back as MLX4_SQ_INIT_OWNER_BIT
                                   * (0xffffffff) AND every WQE TXBB carries it -- a
                                   * conjunction since 0.43.6, and since 0.43.8 the memory it
                                   * covers IS the WQE array (no 64-byte header in front of
                                   * it: docs/DEFECTS.md E13) */
    /*
     * The three feup numbers, added in 0.43.9 (the cap they needed is in the note beside
     * CX3_SCALAR_OUT_MAX).  They are separate scalars rather than a conjunction because the
     * QUESTION they answer has three outcomes that must not be confused: the firmware may keep
     * the byte (the change was made and the hypothesis is live), may drop it (the change
     * cannot help and this line of investigation ends here -- which is exactly what stage 4
     * measured for param3), or QUERY_QP may not answer at all (nothing is known either way).
     * A single scalar would have to compress those into one number, which is the shape
     * docs/DEFECTS.md F7 records: a value carrying its own verdict.
     */
    kR4TxOutFeupSent        = 13, /* 0x40 for a send queue, 0x00 for a receive one */
    kR4TxOutFeupQuery       = 14, /* QUERY_QP's copy; CX3_R4TX_NOT_MEASURED if it did not answer */
    kR4TxOutFeupSurvived    = 15, /* 1 = the two are equal; NOT_MEASURED if no query */
    kR4TxOutCount           = 16,
};

/*
 * The shift that puts statesConfirmed in the upper half of kR4TxOutStepsStates.  A named
 * constant rather than a literal in the two places that use it (the driver's publish and the
 * verifier's decode), because a shift written twice is one more number that can disagree with
 * itself -- and because `abi()` in tools/verify.sh resolves this name out of this header, so a
 * mismatch is a lookup failure rather than a number read out of the wrong half.
 *
 * The not-measured case, spelled out because this scalar carries two numbers: the value is the
 * 32-bit sentinel CX3_R4TX_NOT_MEASURED, whose upper half is then zero, so a reader must compare
 * the WHOLE value against the sentinel before splitting it -- docs/DEFECTS.md D10.  The driver
 * never publishes a half-sentinel.
 */
enum { kR4TxOutStatesShift = 32 };

/* "This was never reached", the same sentinel the two stages before this one use. */
#define CX3_R4TX_NOT_MEASURED 0xffffffffu

/*
 * The queue sizes this stage CHOOSES, named once each rather than written into the selector:
 * the CQ's entry count (a power of two, as the CQ's phase rule requires) and the send queue's
 * TXBB count (whose ilog2 is what makes sq_size_stride 0x42).  Both are inputs, so a caller can
 * ask for something else without a rebuild.
 */
#define CX3_R4TX_DEFAULT_CQ_NENT 256u
#define CX3_R4TX_DEFAULT_SQ_TXBB 256u

/*
 * ------------------------------------------------------------------ */
/* R4 stage 9: r4Raw -- post WQEs, ring the send doorbell, read CQEs    */
/* ------------------------------------------------------------------ */

/*
 * kCx3SelR4Raw -- the first stage where a byte actually moves.
 *
 * Everything in stages 1..6 measured resources that exist and are in the right state.  This
 * call POSTS a work request, RINGS the send doorbell, and CONSUMES completion elements -- both
 * for the send queue stage 6 built and for the receive side stages 3..5 built.  It is also the
 * first selector whose effect leaves the machine: the frames it sends are on the wire.
 *
 * What it deliberately is NOT (plan section 6): no ARP, no IP, no protocol stack, no
 * IOEthernetController, no multiple queues, no offload.  The CQE's checksum and status fields
 * are recorded and not judged, because this stage offloads nothing.
 *
 * ------------------------------------------------------------------ *
 * The six judgements, each from a VALUE, and each failing in its own words
 * ------------------------------------------------------------------ *
 *
 *   1. the TX WQE was written correctly.  The 32 bytes are read back OUT of the queue memory
 *      after the write, and every field is compared with what the encoder intended -- owner
 *      bit, opcode, producer, byte_count, lkey, and the frame's bus address.  The owner bit of
 *      the first WQE is 0, which is "the hardware owns it" (mlx4_en_tx.c:682: the bit is
 *      `prod & ring->size`, so it is 0 on the first lap).
 *   2. the TX completion arrived.  A CQE is consumed from the TX CQ with stage 2's readiness
 *      rule, its opcode is checked against MLX4_CQE_OPCODE_ERROR (the reference's own error
 *      test, mlx4_en_tx.c:381-382), and its byte_cnt/wqe_index are reported.  No completion at
 *      all is a different failure from a completion in error, and the two say different things
 *      about which of the encoding's fields to look at.
 *   3. the TX ring wrapped TWICE.  The posts run past 2 x nent, and the owner bit each lap was
 *      computed from the lap phase and then read back out of memory.  A ring that only turns
 *      once cannot distinguish "the owner bit is right" from "it happens to be 0 everywhere",
 *      which is exactly what stage 6's C12 was.
 *   4. the RX side received something.  Every consumed CQE's `vlan_my_qpn` low 24 bits must be
 *      THIS driver's RX QPN (stage 4's number).  WHEN NOT ONE FRAME ARRIVES, the first thing to
 *      suspect is stage 7's `mcast`/`base_qpn` -- under A0 the firmware decides where broadcast
 *      goes, and it is the `mcast` field of SET_PORT qpn_calc that names it
 *      (docs/EXTERNAL-REFS.md section 13) -- and NOT "stage 8's filter is not configured".
 *      That sentence is in the kernel log and in the verifier's FAIL text on purpose.
 *   5. the RX content is readable.  byte_cnt >= 60 (a minimum-size frame; this card's FCS_KEEP
 *      adds 4 to what the CQE reports), and the frame's bytes are copied out of the receive
 *      buffer so they can be printed.  A frame carrying this driver's own 0x88B5 pattern is
 *      reported separately: on a switched segment a broadcast frame does not come back to the
 *      port it left, so `rxOurs` may legitimately be 0 on a run where frames arrived.
 *   6. the refill is correct.  Each consumed CQE names a slot (`wqe_index`); that slot's WQE is
 *      rewritten and the producer count published again.  Two things are checked: the slot is
 *      the one the ring's cycle position predicts, and it was not already in flight.  Then the
 *      count of slots refilled at least twice is reported -- that is the criterion's "two laps"
 *      as a number rather than as a claim, and on a quiet segment it will be short, which the
 *      verifier reports as SKIP (not enough traffic), not as a refill failure.
 *
 * ------------------------------------------------------------------ *
 * One selector, two halves, and the receive half stands alone
 * ------------------------------------------------------------------ *
 *
 * Plan decision 4.  The TX half can fail for reasons that have nothing to do with the RX half
 * (the WQE encoding, the send doorbell, the CQ, the MR's region), and vice versa.  So a TX
 * failure never stops the RX work: `txRefused` says why the transmit half did not run, and the
 * receive scalars are measured anyway.  The only thing that stops everything is a missing
 * RECEIVE prerequisite (stages 2/3/4), because then there is nothing to observe at all; that
 * is reported as kIOReturnNotReady.
 *
 * ------------------------------------------------------------------ *
 * The first run is one WQE and one frame; the wrap is a second phase
 * ------------------------------------------------------------------ *
 *
 * Plan section 1.  The call is in two phases: post ONE WQE, ring the doorbell, WAIT for its
 * completion; and only if that succeeded, continue posting (one at a time, each completion
 * consumed before the next post) past two laps of the ring.  So a wrong encoding is met on one
 * frame rather than on five hundred, and the wrap -- the part where a mistake would send the
 * hardware after a slot nobody posted -- only happens after a frame has demonstrably gone
 * through the whole path.  `in[kR4RawInTxPosts] = 1` stops after the single frame, which is
 * what a cautious first run wants.
 */

/*
 * The inputs.  Both are budgets/limits rather than device values, so defaults are a decision
 * and are named here once instead of appearing as literals in the selector.
 */
#define CX3_R4RAW_DEFAULT_TX_POST_WAIT_MS   100u   /* per post, for its completion */
#define CX3_R4RAW_DEFAULT_TX_WRAP_BUDGET_MS 5000u  /* for the whole wrap phase */
#define CX3_R4RAW_DEFAULT_RX_BUDGET_MS      3000u  /* waiting for frames to arrive */

/*
 * The largest receive ring this selector will walk its own bookkeeping for.  The arrays behind
 * criterion 6 are per slot (how many times a slot has been refilled, and whether it is in
 * flight), and a ring larger than the arrays would make this stage report numbers about slots
 * it is not tracking -- so a larger ring is refused with a message rather than served badly.
 * 1024 is eight times the ring the chain builds; the check is what matters, not the value.
 */
#define CX3_R4RAW_MAX_RX_SLOTS 1024u

enum {
    kR4RawInTxPosts    = 0,  /* 0 = the default (2 x SQ TXBBs + 1, i.e. two laps and one more);
                              * 1 = the single-frame phase only, no wrap; N = N posts */
    kR4RawInRxBudgetMs = 1,  /* 0 = the default; how long to keep polling the receive CQ */
    kR4RawInCount      = 2,
};

/* Why the transmit half did not run.  A number rather than a sentence, because the kernel
 * logs the sentence and the tool/verifier read the number. */
#define CX3_R4RAW_TX_RAN          0u
#define CX3_R4RAW_TX_NO_STAGE6    1u   /* r4tx has not built the send side in this boot */
#define CX3_R4RAW_TX_NO_MAC       2u   /* no registered address: the frame would have to
                                        * invent a source, and a fabricated source is a worse
                                        * test frame than none at all */
#define CX3_R4RAW_TX_NO_FRAME     3u   /* the frame buffer could not be allocated */

typedef struct {
    uint32_t status;                 /* kIOReturn for the selector itself */
    /* --- the transmit half --- */
    uint32_t txRefused;              /* CX3_R4RAW_TX_*: 0 means it ran */
    uint32_t txQpn;                  /* the send QP's number (stage 6's, read out of state) */
    uint32_t txUarIndex;             /* the UAR page the doorbell went to, from the context */
    uint32_t txSqNent;               /* TXBBs in the send queue's WQE array */
    uint32_t txSqHeaderBytes;        /* SQ_WQE0_OFFSET: 0, i.e. no header in front of the array */
    uint32_t txPosts;                /* how many WQEs were posted in this call */
    uint32_t txProdAfter;            /* the producer count after them, in TXBBs */
    uint32_t txLapsCovered;          /* how many distinct laps the posts touched */
    uint32_t txOwnerLapsOk;          /* laps whose owner phase matched the computed one */
    uint32_t txOwnerBitsSeen;        /* bit mask: bit k set = an owner bit of k was observed */
    uint32_t txWqeOk;                /* 1 = every post's readback matched the encoding */
    uint32_t txWqeChecked;           /* how many posts were compared with their image */
    /*
     * The first WQE, as it stands in the queue's memory after the owner word was written --
     * read back with a load, not restated from the encoder.  The whole 32 bytes are here
     * because the plan's decision 3 asks for the bytes as the fallback when an offset is
     * wrong: a hex dump of a descriptor whose opcode landed in the wrong byte is unmistakable
     * once the number is in front of the reader.
     */
    uint8_t  txWqeImage[32];    /* CQE_LEGACY_SIZE: 3 * 0x20 + 1 bytes of hex, or the
                              * 32 bytes themselves; tools/test-cqe-offsets.c pins
                              * this against mlx4_regs.h's own constant */
    uint32_t txWqeIndex;             /* which TXBB of the array it went into */
    uint32_t txWqeDword;             /* its owner_opcode word, decoded from the bytes */
    uint32_t txWqeOwnerBit;          /* the owner bit as read back (0 = handed over) */
    uint32_t txWqeOwnerWanted;       /* the lap phase it was computed from */
    uint32_t txWqeNonZeroBytes;      /* bytes outside the 32-byte WQE image in that TXBB */
    uint32_t txFrameBytes;           /* 60: what byte_count carries (no FCS) */
    uint32_t txFramePayloadBytes;
    uint64_t txFramePhys;            /* the frame buffer's bus address */
    uint32_t txFrameEthertype;       /* 0x88b5, as written into the frame */
    uint32_t txDoorbellValue;        /* BE32(qpn << 8), the 32-bit value written */
    uint64_t txDoorbellOffset;       /* BAR2 + the UAR page + 0x14 */
    /* --- the transmit completion --- */
    uint32_t txCompletions;          /* CQEs consumed from the TX CQ */
    uint32_t txCqeOk;                /* 1 = at least one, well formed, not in error */
    uint32_t txErrors;               /* completions whose opcode is ERROR */
    uint32_t txTimeouts;             /* posts with no completion inside their budget */
    uint32_t txCqeRawConsIndex;      /* the consumer index that read the first one */
    uint8_t  txCqeRaw[32];           /* the first completion, all 32 bytes */
    uint32_t txCqeQpn;               /* vlan_my_qpn, low 24 bits */
    uint32_t txCqeByteCnt;
    uint32_t txCqeWqeIndex;
    uint32_t txCqeOwnerSrOpcode;
    uint32_t txCqeOpcode;            /* its low 5 bits */
    uint32_t txCqeIsSend;            /* bit 6 of the opcode byte -- 1 for a send completion */
    uint32_t txCqeStatus;            /* the status word: RECORDED, not judged */
    uint32_t txCqeChecksum;          /* same */
    uint32_t txWrapOk;               /* > 2 x nent posts and every lap phase as computed */
    uint32_t txWrapMs;               /* how long the wrap phase took */
    /* --- the receive half --- */
    uint32_t rxQpn;                  /* stage 4's RX QPN: what every CQE must name */
    uint32_t rxNent;                 /* WQEs in the receive ring */
    uint32_t rxStride;               /* bytes per WQE: one 16-byte data segment */
    uint32_t rxFrames;               /* CQEs consumed in this call */
    uint32_t rxQpnMatches;           /* those whose low 24 bits were our QPN */
    uint32_t rxQpnOk;                /* 1 = every one of them */
    uint32_t rxErrorCqes;            /* completions with the ERROR opcode */
    uint32_t rxMinByteCnt;           /* smallest byte_cnt among non-error completions */
    uint32_t rxContentOk;            /* 1 = one frame with byte_cnt >= the minimum was read */
    uint32_t rxOurs;                 /* 1 = one of those carried THIS driver's pattern */
    uint32_t rxFirstRawConsIndex;
    uint8_t  rxFirstRaw[32];         /* the first receive completion, all 32 bytes */
    uint32_t rxFirstQpn;
    uint32_t rxFirstByteCnt;
    uint32_t rxFirstWqeIndex;
    uint32_t rxFirstExpectedSlot;    /* what the ring's cycle position predicts */
    uint32_t rxFirstSlotMatches;     /* 1 = the CQE's slot is the predicted one */
    uint32_t rxFirstOpcode;
    uint32_t rxFirstIsSend;          /* 0 for a receive completion */
    uint32_t rxFirstOwnerSrOpcode;
    uint32_t rxFirstStatus;          /* recorded, not judged */
    uint32_t rxFirstChecksum;        /* recorded, not judged */
    uint32_t rxFirstBadfcsEnc;       /* recorded, not judged */
    uint32_t rxFirstBytesLen;        /* how many bytes of it were copied out */
    uint8_t  rxFirstBytes[64];       /* CX3_R4RAW_RX_KEEP_BYTES: how much of a frame is
                                  * copied out, pinned by the same test */
    uint32_t rxOursBytesLen;         /* the first frame that was ours, if any */
    uint8_t  rxOursBytes[64];        /* the same, for the first frame that was ours */
    /* --- the refill --- */
    uint32_t rxRecycles;             /* WQEs re-posted in this call */
    uint32_t rxRecycleOk;            /* 1 = every one to the right slot, none in flight */
    uint32_t rxRecycleViolations;
    uint32_t rxViolationSlot;        /* which slot, when one was seen */
    uint32_t rxViolationCons;        /* and the consumer index that named it */
    uint32_t rxSlotsRecycledTwice;   /* criterion 6's number: slots refilled >= 2 times */
    uint32_t rxLaps;                 /* whole laps of the receive ring refilled so far */
    uint32_t rxCqConsAfter;          /* the receive CQ's consumer index after this call */
    uint32_t rxProducerBefore;       /* the producer record's value at entry */
    uint32_t rxProducerAfter;        /* and after the refills */
    uint32_t rxInFlightAtExit;       /* slots posted and not yet completed */
    uint32_t rxBudgetMs;
    /* --- the memory region the WQEs' lkey belongs to, read back from the firmware --- */
    uint32_t mrQueryOk;
    uint32_t mrFwStatus;
    uint32_t mrMptIndex;
    uint64_t mrStart;                /* the region as the FIRMWARE holds it */
    uint64_t mrLength;
    uint32_t frameInRegion;          /* 1 = the frame buffer lies inside it */
    uint32_t calls;                  /* how many times this selector has run this boot */
} Cx3R4RawResult;

enum {
    kR4RawOutStatus            = 0,  /* kIOReturn, 0 = the selector ran */
    kR4RawOutTxRefused         = 1,  /* 0 = the transmit half ran; see CX3_R4RAW_TX_* */
    kR4RawOutTxWqeOk           = 2,  /* criterion 1: every post's readback matched */
    kR4RawOutTxPosts           = 3,
    kR4RawOutTxCompletions     = 4,  /* criterion 2's count: 0 means not one arrived */
    kR4RawOutTxCqeOk           = 5,  /* 1 = the first completion is well formed and not ERROR */
    kR4RawOutTxErrors          = 6,  /* completions in error -- a different failure from none */
    kR4RawOutTxWrapOk          = 7,  /* criterion 3 */
    kR4RawOutTxOwnerLapsOk     = 8,  /* laps whose owner phase was as computed */
    kR4RawOutRxFrames          = 9,  /* criterion 4's count: 0 means nothing arrived */
    kR4RawOutRxQpnOk           = 10, /* 1 = every completion named OUR RX QPN */
    kR4RawOutRxQpn             = 11, /* the number they had to name, so the assertion has a
                                      * visible referent rather than an implied one */
    kR4RawOutRxMinByteCnt      = 12, /* criterion 5's size test */
    kR4RawOutRxContentOk       = 13, /* 1 = a frame of at least the minimum was read out */
    kR4RawOutRxLaps            = 14, /* criterion 6's second half: whole laps of the receive
                                      * ring refilled so far.  Two laps means every slot was
                                      * refilled at least twice, which is the criterion's own
                                      * wording; the per-slot counts are in the structure for
                                      * the log.  A run with too little inbound traffic gets
                                      * fewer, and that is a statement about the segment, not
                                      * about the refill -- the verifier reports it as SKIP and
                                      * names stage 7's mcast as the first thing to check. */
    kR4RawOutRxRecycleOk       = 15, /* criterion 6 */
    kR4RawOutCount             = 16,
};

/*
 * "This was never reached", the sentinel all three R4 stages use: a call that refuses before
 * it asks the device leaves the structure zeroed, and several of these assertions are
 * equalities or counts that a zero would satisfy.  Note that the RX slots are full (16 = the
 * cap) -- adding a seventeenth judgement means merging two or dropping one, and saying so here
 * is cheaper than the next person finding out from a static assertion.
 */
#define CX3_R4RAW_NOT_MEASURED 0xffffffffu

/*
 * ------------------------------------------------------------------ */
/* The general-purpose command runner                                   */
/* ------------------------------------------------------------------ */

/*
 * kCx3SelHcr -- send one HCR command with parameters the caller chooses.
 *
 * The reason this exists is the shape of this project's cost.  Every kernel change needs a
 * reboot, and the pattern that produced them was "try a command, learn something, edit the
 * kext, add a selector, reboot".  Some of that is unavoidable -- a wrong line of code has to
 * be fixed -- but most experiments are not code: they are "what does the firmware do when
 * this field holds that value".  Those become one user-space call.
 *
 * It can send ANY command, including ones that make the hardware start referring to host
 * memory, so it is gated on the boot argument cx3raw=1.  A published build is then safe by
 * default rather than safe by remembering, and turning it on does not require a rebuild.
 *
 * Nothing is interpreted on the way through: the caller supplies the inbox bytes and
 * receives the outbox bytes.  That is the point of a probe.
 */
#define CX3_HCR_MAILBOX_SIZE 4096

enum {
    kHcrInOpcode     = 0,   /* the HCR opcode */
    kHcrInInModifier = 1,
    kHcrInOpModifier = 2,
    kHcrInFlags      = 3,   /* CX3_HCR_F_* */
    kHcrInTimeoutMs  = 4,   /* 0 = class A */
    kHcrInInboxLen   = 5,   /* bytes of structureInput to copy into the mailbox */
    kHcrInOutboxLen  = 6,   /* bytes of the mailbox to copy back out */
    kHcrInCount      = 7,
};

enum {
    kHcrOutStatus    = 0,   /* kIOReturn for the selector itself */
    kHcrOutCompleted = 1,   /* 1 = the command completed, 0 = it did not */
    kHcrOutFwStatus  = 2,   /* the firmware's status byte */
    kHcrOutHcrFinal  = 3,   /* the HCR status register after the command */
    kHcrOutImmLsb    = 4,   /* immediate return, low 32 bits, when requested */
    kHcrOutImmMsb    = 5,
    kHcrOutAllowed   = 6,   /* 1 = cx3raw=1 was in effect */
    kHcrOutCount     = 7,
};
CX3_STATIC_ASSERT(kHcrInCount  <= CX3_SCALAR_IN_MAX,  "hcr: inputs over the scalar cap");
CX3_STATIC_ASSERT(kHcrOutCount <= CX3_SCALAR_OUT_MAX, "hcr: outputs over the scalar cap");

#define CX3_HCR_F_USE_MAILBOX   (1u << 0)   /* in_param = the mailbox's DMA address */
#define CX3_HCR_F_FILL_INBOX    (1u << 1)   /* copy structureInput into the mailbox */
#define CX3_HCR_F_BYPASS_BUSY   (1u << 2)   /* post even if the command register is busy */
#define CX3_HCR_F_WANT_IMM      (1u << 3)   /* read the immediate return value */

/*
 * kCx3SelSurvey -- everything the driver currently knows, in one call.

 * The other half of "stop needing a reboot to ask a question": when something is wrong, the
 * first useful thing is not one more experiment but the whole current state.  This gathers
 * what is cheap and independent -- the mapped BARs' identity, which tables are mapped, which
 * queues exist, and each queue's own context as the firmware reports it -- so a single call
 * either shows the inconsistency or rules out the obvious ones.
 */
#define CX3_SURVEY_MAX_ENTRIES 40

/*
 * Each item is numbers only.  The kext has no runtime string formatting (snprintf is not in
 * the kext KPI -- the build's symbol accounting refused it once already), and more
 * importantly this project's convention is that the kernel returns values and the tool
 * explains them: that is how the port decoder works, and it keeps the interpretation in one
 * place where it can be read.  The `id` values are named below so the tool has something
 * stable to key on.
 */
typedef struct {
    uint32_t kind;      /* CX3_SURVEY_KIND_* */
    uint32_t id;        /* meaning depends on kind; see the enums below */
    uint32_t value0;
    uint32_t value1;
    uint64_t value64;
} Cx3SurveyItem;

typedef struct {
    uint32_t entries;
    Cx3SurveyItem item[CX3_SURVEY_MAX_ENTRIES];
} Cx3SurveyResult;

#define CX3_SURVEY_KIND_FACT   1u
#define CX3_SURVEY_KIND_TABLE  2u
#define CX3_SURVEY_KIND_QUEUE  3u

/* kind == FACT */
enum {
    kSurveyFactPciHeld      = 0,
    kSurveyFactBars         = 1,
    kSurveyFactHcaState     = 2,   /* v0 = hca initialised, v1 = icm mapped */
    kSurveyFactReservedEqUar = 3,
    kSurveyFactCqCounts     = 4,   /* v0 = reserved, v1 = configured */
    kSurveyFactPdCounts     = 5,   /* v0 = reserved, v1 = total */
    kSurveyFactMptCounts    = 6,   /* v0 = reserved, v1 = configured */
    /*
     * The command-timeout state, which is the part of this batch that cannot be seen
     * anywhere else.  Three facts because three questions are asked of it: is the mailbox
     * poisoned right now, which command did it, and what budgets are in force.
     *
     *   CmdWedged    v0 = 1 if poisoned (every command is now refused before it is
     *                     posted), v1 = how many commands were given up on since boot,
     *                     v64 = how many were refused while a poison held.
     *   CmdWedgedOp  v0 = the opcode whose abandonment set the poison (0 = none),
     *                     v1 = 1 if the offset of the abandonment was "before posting"
     *                          (the register never went idle) and 0 if it was "after"
     *                          (posted, never completed), v64 = uptime in ns when it
     *                          happened.  Those two cases need different next steps, so
     *                          they are separate numbers and not one flag.
     *   CmdBudget    v0 = the pre-post idle wait for a polled command, in ms,
     *                     v1 = the same for an event-mode command, in ms,
     *                     v64 = (firmware budget ms << 32) | (ICM teardown budget ms).
     *                     Reading these back is how a running kext can be shown to have
     *                     the bounded timeouts in force rather than the old iteration
     *                     counts -- a check on the *loaded* binary, which no build-time
     *                     test can make.
     */
    kSurveyFactCmdWedged    = 7,
    kSurveyFactCmdWedgedOp  = 8,
    kSurveyFactCmdBudget    = 9,
    kSurveyFactInconsistent = 99,  /* v0 = a code; see kSurveyIncons_* */
};
enum {
    kSurveyInconsCqWithoutEq  = 1,
    kSurveyInconsMrWithoutIcm = 2,
    kSurveyInconsRqWithoutMr  = 3,
};
/* kind == QUEUE */
enum { kSurveyQueueEq = 0, kSurveyQueueMr = 1, kSurveyQueueCq = 2, kSurveyQueueRq = 3 };

enum { kSurveyInCount = 0 };
enum { kSurveyOutStatus = 0, kSurveyOutEntries = 1, kSurveyOutCount = 2 };
CX3_STATIC_ASSERT(kSurveyOutCount <= CX3_SCALAR_OUT_MAX, "survey: outputs over the scalar cap");

/*
 * kCx3SelFault -- inject and clear the command-timeout states on purpose.
 *
 * The selector number and the two scalars are the whole interface; the meanings are here
 * rather than in the tool so that the tool cannot drift from the kext.
 */
enum {
    kFaultInMode    = 0,   /* kFaultMode* below */
    kFaultInArg     = 1,   /* mode 2: must be 1 (a confirmation); mode 3: milliseconds */
    kFaultInCount   = 2,
};
enum {
    kFaultModeWedge      = 1,  /* set the poison as if a command had been abandoned */
    kFaultModeUnwedge    = 2,  /* clear it; kFaultInArg must be 1 */
    kFaultModeHold       = 3,  /* hold the transaction lock for kFaultInArg ms */
};
/*
 * The longest a selector may be told to hold the transaction lock.  Bounded because this
 * hook exists to prove that *other* selectors stay answerable while the lock is held, and a
 * hook that can hold it indefinitely would be the very defect the batch is fixing.
 */
#define CX3_FAULT_HOLD_MAX_MS 30000u

/*
 * The opcode an injected poison records in its place of "the command that was abandoned".
 *
 * Not a firmware opcode, and not one this driver sends: the commands it sends are 0x3-0x56
 * and 0xff6-0xfff (src/mlx4_regs.h).  It exists so that a poison set on purpose cannot be
 * mistaken for one the hardware caused -- in the log line, in the survey fact, or in the
 * next reader's head.
 */
#define CX3_FAULT_OP_MARKER 0x5ffu

enum {
    kFaultOutStatus    = 0,  /* kIOReturn, 0 = ok */
    kFaultOutWedged    = 1,  /* the poison state after this call */
    kFaultOutWedgedOp  = 2,  /* the opcode that set it, 0 if none */
    kFaultOutAbandons  = 3,  /* the cumulative abandonment count */
    kFaultOutRefused   = 4,  /* the cumulative count of commands refused while poisoned */
    kFaultOutHeldMs    = 5,  /* mode 3: how long the lock was actually held */
    kFaultOutCount     = 6,
};
CX3_STATIC_ASSERT(kFaultInCount  <= CX3_SCALAR_IN_MAX,  "fault: inputs over the scalar cap");
CX3_STATIC_ASSERT(kFaultOutCount <= CX3_SCALAR_OUT_MAX, "fault: outputs over the scalar cap");

CX3_STATIC_ASSERT(kR4QpInCount   <= CX3_SCALAR_IN_MAX,  "r4qp: inputs over the scalar cap");
CX3_STATIC_ASSERT(kR4QpOutCount  <= CX3_SCALAR_OUT_MAX, "r4qp: outputs over the scalar cap");
/*
 * The per-step arrays are indexed by the step number, and the code writes exactly the
 * three transitions RST->INIT->RTR->RTS.  A fourth state array bound would let a loop go
 * one step too far without the compiler saying anything.
 */
CX3_STATIC_ASSERT(sizeof(((Cx3R4QpResult *)0)->stepFwStatus) /
                  sizeof(((Cx3R4QpResult *)0)->stepFwStatus[0]) == 3,
                  "r4qp: the step arrays must have one slot per state transition");

CX3_STATIC_ASSERT(kR4IngressInCount  <= CX3_SCALAR_IN_MAX,
                  "r4ingress: inputs over the scalar cap");
CX3_STATIC_ASSERT(kR4IngressOutCount <= CX3_SCALAR_OUT_MAX,
                  "r4ingress: outputs over the scalar cap");
CX3_STATIC_ASSERT(sizeof(((Cx3R4IngressResult *)0)->stepFwStatus) /
                  sizeof(((Cx3R4IngressResult *)0)->stepFwStatus[0]) == 3,
                  "r4ingress: the step arrays must have one slot per state transition");
/*
 * The MAC entry is an 8-byte BE64 and the read-back is the same 8 bytes.  Two arrays that are
 * supposed to be the same width are exactly what a later edit gets wrong silently (the
 * per-page MTT verdict arrays in Cx3EqInfoResult needed the same pin), and a comparison over
 * mismatched widths would show a mismatch that is an artefact of the comparison.
 */
CX3_STATIC_ASSERT(sizeof(((Cx3R4IngressResult *)0)->mac) ==
                  sizeof(((Cx3R4IngressResult *)0)->macReadRaw),
                  "r4ingress: the MAC entry and its read-back must be the same width");

CX3_STATIC_ASSERT(kR4TxInCount  <= CX3_SCALAR_IN_MAX,  "r4tx: inputs over the scalar cap");
CX3_STATIC_ASSERT(kR4TxOutCount <= CX3_SCALAR_OUT_MAX, "r4tx: outputs over the scalar cap");
CX3_STATIC_ASSERT(sizeof(((Cx3R4TxResult *)0)->stepFwStatus) /
                  sizeof(((Cx3R4TxResult *)0)->stepFwStatus[0]) == 3,
                  "r4tx: the step arrays must have one slot per state transition");

/*
 * R4 stage 9.
 *
 * The two byte-array sizes in the structure are LITERALS, because this header cannot see
 * mlx4_regs.h (it is included by both the kernel and the tool, in either order, and it must
 * stay free of that dependency).  They are therefore pinned against mlx4_regs.h's own
 * constants at build time instead -- tools/test-cqe-offsets.c includes both headers and checks
 * these four widths and the slot-bitmap division.  A number that has two homes and no check is
 * the shape docs/DEFECTS.md F11 records; this is the check.
 */
CX3_STATIC_ASSERT(kR4RawInCount  <= CX3_SCALAR_IN_MAX,  "r4raw: inputs over the scalar cap");
CX3_STATIC_ASSERT(kR4RawOutCount <= CX3_SCALAR_OUT_MAX, "r4raw: outputs over the scalar cap");
CX3_STATIC_ASSERT(CX3_R4RAW_MAX_RX_SLOTS % 8u == 0u,
                  "r4raw: the in-flight bitmap needs a whole number of bytes per slot count");

/* ------------------------------------------------------------------ */
/* kCx3SelTxProbe -- the three discriminators (docs/M2B-NOTES.md 52)    */
/* ------------------------------------------------------------------ */

/*
 * Why this selector exists at all, and why it is not part of r4raw, is written beside
 * kCx3SelTxProbe above.  The short version: r4raw's sixteen scalars are all in use, and
 * "does the hardware execute ANY descriptor from this send queue" is a question that has to
 * be answered from a value rather than from a structure.
 */
enum {
    kTxProbeInNopWaitMs = 0,   /* per post, for its completion; 0 = the default (150 ms).
                                * Long enough for any completion the hardware would ever
                                * write (the working receive path completes in microseconds),
                                * short enough that four variants and two opcodes stay inside
                                * a second and a half. */
    kTxProbeInArmWaitMs = 1,   /* how long to poll the EQ ring for the CQ event the arm
                                * should produce; 0 = the default (2000 ms).  Sized against
                                * the segment rather than the device: this LAN delivers an
                                * ARP request every second or so, and the event can only fire
                                * when a completion lands while the CQ is armed. */
    kTxProbeInCount     = 2,
};

/*
 * The values that mean "this driver could not run that experiment", kept apart from the
 * values that mean "it ran and the answer was no".  docs/DEFECTS.md D10 is the reason this
 * distinction is a sentinel rather than a zero: a field that was never written and a field
 * the hardware answered 0 to are different answers, and only one of them is a measurement.
 */
#define CX3_TXPROBE_NOT_RUN      0xffffffffu  /* never reached */
#define CX3_TXPROBE_RAN          0u
#define CX3_TXPROBE_NO_TX_CQ     1u  /* r4tx has not built a send CQ in this boot */
#define CX3_TXPROBE_NO_TX_SQ     2u  /* no send-queue memory to post into */
#define CX3_TXPROBE_NO_TX_QP     3u  /* no QPN to name in the doorbell */
/*
 * 5: the network data path owns the send queue.
 *
 * Its own code since 0.44.8, and separate from 2 on purpose: "there is no send queue" and
 * "the send queue is in use by the interface" call for opposite responses (run the chain /
 * bring the interface down), and a report that cannot tell them apart sends a reader to the
 * wrong one. The numbering skips 4, which the arm probe took (CX3_TXPROBE_NO_RX_CQ).
 */
#define CX3_TXPROBE_IFACE_UP     5u
#define CX3_TXPROBE_NO_RX_CQ     4u  /* no receive CQ record for the arm probe -- that
                                      * experiment is skipped, the others still run; a
                                      * refusal code is only used when NOTHING could run */

/* One opcode value per post, so the completion's opcode field can be compared with what
 * was asked for instead of being read as a number. */
#define CX3_TXPROBE_OPCODE_NONE  0xffu  /* no completion arrived, so there is no opcode */

/* The two waits, so the driver and the tool's own help text cannot disagree about them. */
#define CX3_TXPROBE_DEFAULT_POST_WAIT_MS  150u
#define CX3_TXPROBE_DEFAULT_ARM_WAIT_MS   2000u

enum {
    kTxProbeOutStatus      = 0,  /* kIOReturn, 0 = the selector ran */
    kTxProbeOutRefused     = 1,  /* CX3_TXPROBE_*: 0 = the experiments ran */
    /* --- 1. NOP: is this send queue executed at all? --- */
    kTxProbeOutNopPosts    = 2,  /* posts made with the NOP opcode (1, or 0 if refused) */
    kTxProbeOutNopWqeOk    = 3,  /* 1 = the 32 bytes in the queue read back as encoded,
                                  * with opcode 0x00 -- i.e. the descriptor is there to be
                                  * executed, not merely written */
    kTxProbeOutNopComps    = 4,  /* completions after it.  ANY opcode, ANY status: an error
                                  * completion still proves the descriptor was executed,
                                  * which is the whole question here */
    kTxProbeOutNopOpcode   = 5,  /* the first completion's opcode (CX3_TXPROBE_OPCODE_NONE
                                  * when none arrived) */
    kTxProbeOutNopStatus   = 6,  /* and its status word, recorded rather than judged */
    /* --- 2. the same post with SEND, in the same call --- */
    kTxProbeOutSendComps   = 7,
    kTxProbeOutSendOpcode  = 8,
    /* --- 3. the arm doorbell through UAR page 128 --- */
    kTxProbeOutArmRan      = 9,  /* 1 = the arm probe ran (the receive CQ exists) */
    kTxProbeOutArmEqeFound = 10, /* 1 = an EQE was in the EQ ring within the budget */
    kTxProbeOutArmEqeType  = 11, /* its type byte: 0x00 is a COMP event (device.h:293) */
    kTxProbeOutArmEqeCqn   = 12, /* the CQN it named (EQE offset 4, mlx4_eqe's comp.cqn) */
    kTxProbeOutArmEqeOk    = 13, /* 1 = type 0x00 AND that CQN is our receive CQ's.  Only
                                  * this value says the arm reached the device and did what
                                  * it is for; the three above say what arrived instead */
    /* --- 4. the send doorbell record, the PRM's posting step 2 --- */
    kTxProbeOutRecVariants = 14, /* variants tried: 4 when the other experiments ran */
    kTxProbeOutRecWinner   = 15, /* 0 = none of them produced a completion.  Otherwise the
                                  * variant number PLUS ONE, so that variant 0 -- the plain SEND
                                  * with no record written, which is a variant like any other --
                                  * is 1 and is not confused with "none".  The first version of
                                  * this field used 0 for both and read as "no variant worked"
                                  * when the first one had. */
    kTxProbeOutCount       = 16,
};

CX3_STATIC_ASSERT(kTxProbeInCount  <= CX3_SCALAR_IN_MAX,  "txprobe: inputs over the scalar cap");
/*
 * Sixteen is the kernel's limit (kIOExternalMethodScalarOutputCountMax, and docs/DEFECTS.md
 * F13 is what happens when this side's idea of it is larger).  The assert is against that
 * cap rather than against a copy of it, so the next addition to this block fails the BUILD
 * instead of the call.
 */
CX3_STATIC_ASSERT(kTxProbeOutCount <= CX3_SCALAR_OUT_MAX, "txprobe: outputs over the scalar cap");

/* ------------------------------------------------------------------ */
/* kCx3SelNetUp -- stage 10a: the interface (docs/R4-STAGE10-PLAN.md)   */
/* ------------------------------------------------------------------ */

enum {
    kNetUpInCount = 0,   /* nothing to ask for: it attaches what the chain built */
};

#define CX3_NETUP_RAN        0u
#define CX3_NETUP_NO_DEVICE  1u  /* no port address is held: no port-up step has read one in
                              * this boot, so there is nothing to give an interface.  Since
                              * 0.44.28 this selector sends no command of its own -- the address
                              * is published by m3portup (or the standalone queryport selector)
                              * from the same QUERY_PORT that programmed the steering table.
                              * A statement about which stages have run, not about hardware. */
#define CX3_NETUP_NO_IFACE   2u  /* the interface object could not be created or attached;
                              * the kernel log says which of the two, and why */

enum {
    kNetUpOutStatus   = 0,  /* kIOReturn, 0 = the selector ran */
    kNetUpOutRefused  = 1,  /* CX3_NETUP_*: 0 = the interface was attached */
    kNetUpOutUnit     = 2,  /* the unit number the kernel gave it: the interface is en<unit>,
                             * which is what `ifconfig` and ping will use */
    kNetUpOutMacOk    = 3,  /* 1 = getHardwareAddress answered with the MAC the firmware
                             * reports for this port (def_mac, from QUERY_PORT) */
    kNetUpOutLinkUp   = 4,  /* 1 = setLinkStatus was called with VALID|ACTIVE at 10 Gb/s */
    /*
     * The address itself, packed as a 48-bit number (byte 0 in the top 8 bits), so a caller
     * can compare it against what `ifconfig` shows without a second call: "the interface
     * advertises the card's address" is a check, not a sentence, only if something can compare
     * the two.  Added 0.44.6, together with the chain step that does the comparison.
     */
    kNetUpOutMacBits  = 5,
    kNetUpOutCount    = 6,
};

CX3_STATIC_ASSERT(kNetUpInCount  <= CX3_SCALAR_IN_MAX,  "netup: inputs over the scalar cap");
CX3_STATIC_ASSERT(kNetUpOutCount <= CX3_SCALAR_OUT_MAX, "netup: outputs over the scalar cap");

/*
 * kCx3SelNetUpStop (46) -- the interface step, stopped after a chosen step.
 *
 * Input 0 is the step to stop after: 1 = read the address and return (the first form of this
 * experiment), 2 = also attach, 3 = also publish the link, 4 = also create the timer, 5 = also
 * allocate the frame ring (which is the whole of netup).  0 is read as 1, so the original
 * cut-short behaviour is what a caller that passes zero gets. 6 = test work-loop
 * gate entry without attaching; values above 6 are rejected, never clamped to 5.
 *
 * The OUTPUT LAYOUT IS NETUP'S, deliberately: this selector is netup with an early exit, so a
 * caller reads the same slots and the two can be compared directly.  (The first version gave it
 * its own four-slot layout, which meant the same function would have had to write into two
 * different shapes -- a mismatch waiting to happen, and the kind this project has paid for.)
 */
enum {
    kNetUpStopInStopAfter = 0,
    kNetUpStopInCount     = 1,
};
CX3_STATIC_ASSERT(kNetUpStopInCount <= CX3_SCALAR_IN_MAX,
                  "netupstop: inputs over the scalar cap");

/*
 * kCx3SelNetStat -- the network data path's own counters, one call, no device access.
 *
 * Numbers only, and the tool explains them: the kext has no runtime string formatting (snprintf
 * is not in the kext KPI) and this project's rule is that the kernel returns values while the
 * interpretation lives where it can be read (the survey selector's comment says the same thing).
 *
 * What each field is for, in the order a reader needs them:
 *
 *   enabled      did the stack ever bring the interface up (without it nothing below ran)
 *   ticks        is the timer firing at all -- a data path that delivers nothing because the
 *                timer is dead looks exactly like one that delivers nothing because no frame
 *                arrived, and this is the field that tells them apart
 *   ticksSkipped a selector held the transaction lock, so that tick did nothing.  Expected to be
 *                small; a large number means the chain was running while the interface was up
 *   noRings      ticks that found the receive objects missing, i.e. a reset ran under the
 *                interface.  Any of these means: re-run the chain
 *   rxFrames / rxBytes      frames this driver SUBMITTED to the stack (see rxNotDelivered)
 *   rxNotDelivered          inputPacket took the mbuf and returned 0: not submitted.  Counted
 *                           apart from rxFrames because "the driver got frames" and "the stack
 *                           got frames" are different claims
 *   rxDropped / rxErrors    refused by this driver: no mbuf (DONTWAIT), or a ring check failed
 *   rxRefills               slots re-posted (should track rxFrames plus what is still in flight)
 *   rxNotOurs               completions naming another QPN: not this interface's traffic
 *   txFrames / txCompleted  posted and reclaimed
 *   txStalled / txDropped   handed back to the queue (ring full) / dropped by this driver
 *   txErrors                ERROR completions, or a completion naming a TXBB with no record
 *   txLockBusy              a frame handed back because the transmit lock was held
 *   linkUp / speedMbps      what the port-up step measured and netup published
 *   unit / macBits          the interface the system gave this controller, and its address
 *   mcastCount / mcastMode / promiscMode   what the filter callbacks were asked for and did not
 *                           program (docs/R4-STAGE10-PLAN.md section 12.6)
 */
typedef struct {
    uint32_t unit;
    uint32_t enabled;         /* 0/1 */
    uint32_t linkUp;          /* 0/1, from the port-up measurement */
    uint32_t speedMbps;
    uint32_t macValid;
    uint32_t mcastMode;       /* 0/1 as requested */
    uint32_t promiscMode;     /* 0/1 as requested */
    uint32_t mcastCount;      /* addresses in the last list the stack asked for */
    uint32_t rxErrors;
    uint32_t txErrors;
    uint32_t timerCreated;    /* 0/1: netup managed to build the data path's timer */
    uint32_t txRingPresent;   /* 0/1: netup managed to allocate the frame ring */
    uint64_t macBits;         /* the address, packed as in kNetUpOutMacBits */
    uint64_t ticks;
    uint64_t ticksSkipped;
    uint64_t noRings;
    uint64_t rxFrames;
    uint64_t rxBytes;
    uint64_t rxNotDelivered;
    uint64_t rxDropped;
    uint64_t rxRefills;
    uint64_t rxNotOurs;
    uint64_t txFrames;
    uint64_t txCompleted;
    uint64_t txStalled;
    uint64_t txDropped;
    uint64_t txLockBusy;
} Cx3NetStatResult;

enum {
    kNetStatOutStatus = 0,
    /*
     * The interface's unit number, as a scalar as well as in the structure.
     *
     * A harness that wants to talk to the interface by name (tools/verify.sh's M5 mode) has no
     * other way to learn it: the tool prints it, but a script that parsed the tool's prose would
     * be deciding from text, which is the thing this project's chain is careful not to do.
     */
    kNetStatOutUnit   = 1,
    /*
     * Which step of the INTERFACE step is in progress, sampled by an off-machine poller.
     *
     * Why this exists, measured 2026-10-13: the machine went silent less than 0.35 s after a
     * call to selector 39, and in that window the driver produced no line at all -- the earlier
     * deaths, which took 1.5-3.2 s, delivered theirs.  So the lines were not "never produced",
     * they were "never got out", and no amount of adding log lines can say which step was
     * running.  A counter that a NON-BLOCKING selector can read (netstat is kXactTryLock and
     * touches no device state) can be sampled from another process, and the sample pushed off the
     * machine, so the last value that arrives names the step -- even when every other record is
     * lost.  Values: 0 = not in the interface step, 1..5 = the step it is inside of,
     * 6 = it returned.
     */
    kNetStatOutNetupStep = 2,
    kNetStatOutCount  = 3
};
CX3_STATIC_ASSERT(kNetStatOutCount <= CX3_SCALAR_OUT_MAX, "netstat: outputs over the scalar cap");


/*
 * kCx3SelNetLink -- media table, selected medium, and one reading of the link.
 *
 * Modes (input 0):
 *   REPORT    read-only: what has been published, what is selected, what the link says
 *   PUBLISH   build the media dictionary, publish it, and select the one entry in it
 *   REQUERY   send QUERY_PORT, compare with what was last reported, republish if it changed
 *
 * What the dictionary contains, and why only that: one entry, `autoselect`.  A richer list
 * would be a claim about the PHY (10GBase-SR vs 10GBase-CR4 vs 10GBase-T), and the sources this
 * port has do not map QUERY_PORT's transceiver type to those names -- so publishing one would be
 * a guess with `ifconfig` as its evidence.  The actual speed is not in the media list anyway:
 * setLinkStatus carries it, and it is measured.
 */
enum { kNetLinkInMode = 0, kNetLinkInCount = 1 };

#define CX3_NETLINK_REPORT   0u
#define CX3_NETLINK_PUBLISH  1u
#define CX3_NETLINK_REQUERY  2u

enum {
    kNetLinkOutStatus     = 0,  /* kIOReturn for the selector itself */
    kNetLinkOutRefused    = 1,  /* CX3_NETLINK_* mode actually performed, or 0xffffffff */
    kNetLinkOutMediaCount = 2,  /* entries in the published dictionary; 0 = nothing published */
    kNetLinkOutSelected   = 3,  /* 1 = getSelectedMedium() answers with our object */
    kNetLinkOutLinkUp     = 4,  /* 1 = the port reported the link up */
    kNetLinkOutSpeedMbps  = 5,  /* what QUERY_PORT's speed code decoded to */
    kNetLinkOutChanged    = 6,  /* REQUERY: 1 = the reading differed and was republished */
    kNetLinkOutCount      = 7
};
CX3_STATIC_ASSERT(kNetLinkOutCount <= CX3_SCALAR_OUT_MAX, "netlink: outputs over the scalar cap");

/*
 * kCx3SelNetOff -- what this driver offloads, and the switches for it.
 *
 * The report is read-only and says both things a reader needs kept apart: whether the switch is
 * on, and what the hardware has actually reported since.  RX is implemented first (M5-3): the
 * receive side needs no device change at all -- the hardware checksums unconditionally and the
 * driver's job is to read the CQE's verdict and mark the mbuf -- while the transmit side means
 * writing new bits into the send WQE this project's transmit path depends on, which waits until
 * M4 has been verified.  getChecksumSupport() advertises RX by default, and the OUTPUT path's answer follows the TX
 * switch -- with one property that has to be said out loud: the family asks it ONCE, when the
 * interface is attached (netup).  Turning the TX switch on after that changes nothing until the
 * next attach, which is why the verification recipe sets it BEFORE the chain's netup step
 * (tools/verify.sh reads CX3_TXCSUM for exactly that).  The receive side has no such constraint:
 * marking a frame is this driver's own decision, taken per frame, so flipping it works at once.
 */
enum { kNetOffInMode = 0, kNetOffInCount = 1 };

#define CX3_NETOFF_REPORT  0u
#define CX3_NETOFF_RX_ON   1u
#define CX3_NETOFF_RX_OFF  2u
#define CX3_NETOFF_TX_ON   3u
#define CX3_NETOFF_TX_OFF  4u

enum {
    kNetOffOutStatus   = 0,  /* kIOReturn for the selector itself */
    kNetOffOutMode     = 1,  /* the mode that was performed */
    kNetOffOutRxOn     = 2,  /* 1 = received frames are being marked */
    kNetOffOutRxGood   = 3,  /* frames the hardware reported as verified AND we marked */
    kNetOffOutRxNone   = 4,  /* frames with no verdict: nothing marked, the stack computes */
    kNetOffOutTxOn     = 5,  /* 1 = the OUTPUT path advertises offload to the family */
    kNetOffOutTxGood   = 6,  /* frames posted with an offload request in the WQE */
    kNetOffOutTxNone   = 7,  /* frames posted with no request (the stack inserted it) */
    kNetOffOutCount    = 8
};
CX3_STATIC_ASSERT(kNetOffOutCount <= CX3_SCALAR_OUT_MAX, "netoff: outputs over the scalar cap");


/*
 * kCx3SelNetArm -- "drain, then arm, then see whether a COMP event arrives".
 *
 * The input is the wait budget in milliseconds (0 = the driver's default).  The outputs are
 * deliberately ordered so that a reader can judge whether the experiment was VALID before
 * looking at what it found: drained/empty first, then the arm, then the event.
 */
enum { kNetArmInWaitMs = 0, kNetArmInCount = 1 };

enum {
    kNetArmOutStatus      = 0,  /* kIOReturn for the selector itself */
    kNetArmOutRefused     = 1,  /* CX3_NETARM_*: 0 = the experiment ran */
    kNetArmOutDrained     = 2,  /* completions consumed while emptying the receive CQ */
    kNetArmOutCqEmpty     = 3,  /* 1 = the CQ WAS empty when the arm was rung; 0 = it was not,
                                 * and then the arm result says nothing (this is the defect that
                                 * invalidated stage 9's measurement) */
    kNetArmOutEqeBefore   = 4,  /* events consumed from the EQ before arming, so the one that
                                 * follows is attributable to this arm */
    kNetArmOutArmed       = 5,  /* 1 = the arm doorbell was rung */
    kNetArmOutEqeFound    = 6,  /* 1 = an EQE arrived within the budget */
    kNetArmOutEqeType     = 7,  /* its type byte (0xff = none arrived); 0x00 is COMP */
    kNetArmOutEqeCqn      = 8,  /* the CQN it named */
    kNetArmOutEqeOk       = 9,  /* 1 = COMP AND naming THIS CQ -- the link proved */
    kNetArmOutWaitMs      = 10, /* how long the driver actually waited */
    kNetArmOutCount       = 11
};
CX3_STATIC_ASSERT(kNetArmOutCount <= CX3_SCALAR_OUT_MAX, "netarm: outputs over the scalar cap");

#define CX3_NETARM_RAN          0u
#define CX3_NETARM_NO_CQ        1u  /* no receive CQ built in this boot (r4cq) */
#define CX3_NETARM_NO_EQ        2u  /* no event queue (m3equp) */
#define CX3_NETARM_IFACE_UP     3u  /* the network data path is enabled and owns this CQ:
                                     * bring the interface down first */
#define CX3_NETARM_IRQ_UP       4u  /* an interrupt session is open and would consume the very
                                     * EQE this experiment waits for: run m3irqdown first */

/*
 * How long the event queue is watched, and how many completions the drain will consume.
 *
 * The drain bound exists because the precondition ("the CQ is empty when the arm is rung") cannot
 * be assumed on a live LAN: broadcast frames keep arriving.  A drain that hits the bound is
 * REPORTED as "not empty", and in that state the arm has no meaning -- which is exactly the
 * distinction stage 9's measurement lacked.
 */
#define CX3_NETARM_DEFAULT_WAIT_MS 2000u
#define CX3_NETARM_MAX_DRAIN       512u


/*
 * kCx3SelNetFilt -- report the switch, turn it on (which replays the remembered list), or turn
 * it off.
 *
 * Turning it OFF does not rewrite the table: the addresses stay in the device until something
 * programs it again (a power cycle, or a chain run, which re-does the port bring-up).  Said in
 * the log rather than implied, because "the switch is off" and "the device has no group
 * addresses" are different states and only one of them is true.
 */
enum { kNetFiltInMode = 0, kNetFiltInCount = 1 };

/* How many group addresses the driver remembers.  IPv6 needs a handful (solicited-node plus
 * all-nodes); 32 is far above that, and the count of anything that does not fit is reported
 * rather than dropped silently. */
#define CX3_NETFILT_MAX_ADDRS 32u

#define CX3_NETFILT_REPORT      0u
#define CX3_NETFILT_MCAST_ON    1u
#define CX3_NETFILT_MCAST_OFF   2u
/*
 * Promiscuous mode, and under A0 it is one bit of a command this driver already sends: the
 * reference's A0 branch is `mlx4_SET_PORT_qpn_calc(port, base_qpn, 1)`
 * (mlx4_en/mlx4_en_netdev.c:780-786), the same command the port bring-up sends with 0.  The
 * reference also disables the port's multicast filter whenever promiscuous mode goes ON
 * (:790-795, unconditionally), and re-programs it afterwards when it goes off -- which is the
 * caller's next step there and this selector's here.
 *
 * It is the one filter switch that touches the UNICAST table, which is why it comes after the
 * multicast one and why turning it off re-sends the same command with the bit clear (a working
 * undo, not a hope).
 */
#define CX3_NETFILT_PROMISC_ON  3u
#define CX3_NETFILT_PROMISC_OFF 4u

enum {
    kNetFiltOutStatus    = 0,  /* kIOReturn for the selector itself */
    kNetFiltOutMode      = 1,  /* the mode that was performed */
    kNetFiltOutMcastOn   = 2,  /* 1 = group addresses are being programmed into the device */
    kNetFiltOutStored    = 3,  /* how many addresses the driver has remembered */
    kNetFiltOutPrograms  = 4,  /* how many times the table has been written */
    kNetFiltOutLastFw    = 5,  /* fw status of the last command of the last programming */
    kNetFiltOutTruncated = 6,  /* addresses the driver could not remember (its own list cap) */
    kNetFiltOutPromiscOn = 7,  /* 1 = the unicast filter is off (qpn_calc's promisc bit) */
    kNetFiltOutPromiscFw = 8,  /* fw status of the last promiscuous-mode command */
    kNetFiltOutCount     = 9
};
CX3_STATIC_ASSERT(kNetFiltOutCount <= CX3_SCALAR_OUT_MAX, "netfilt: outputs over the scalar cap");


/*
 * kCx3SelNetPort -- report the port's state, or stop it.
 *
 * There is no "up" mode, and that is deliberate rather than an omission: re-opening a port is the
 * port-up sequence the chain already runs, with the MTU as one of ITS inputs (kPortUpInMtu), so a
 * second copy of it here would be a second place for the three-step order to be got wrong.  What
 * this selector adds is the stop, and the state that says the port is stopped.
 */
enum { kNetPortInMode = 0, kNetPortInCount = 1 };

#define CX3_NETPORT_REPORT 0u
#define CX3_NETPORT_DOWN   1u

enum {
    kNetPortOutStatus  = 0,  /* kIOReturn for the selector itself */
    kNetPortOutMode    = 1,  /* the mode that was performed */
    kNetPortOutDown    = 2,  /* 1 = this driver stopped the port and has not re-opened it */
    kNetPortOutFw      = 3,  /* fw status of the last CLOSE_PORT */
    kNetPortOutLinkUp  = 4,  /* what the driver last knew about the link (0 after a stop) */
    kNetPortOutCount   = 5
};
CX3_STATIC_ASSERT(kNetPortOutCount <= CX3_SCALAR_OUT_MAX, "netport: outputs over the scalar cap");

/* Per-port observation/management receipts, not queue/DMA ownership proof.
 * raw is the most recent successful QUERY_PORT in this device generation.
 * LAST_QUERY_FAILED marks a failed newer query; do not present raw as current.
 * frameMtu is a WIRE size accepted by SET_PORT; maxPayloadMtu is L3 capability. */
#define CX3_PORT_INFO_VERSION 1u
#define CX3_PORT_QUERY_VALID 1u
#define CX3_PORT_MAC_VALID 2u
#define CX3_PORT_LINK_VALID 4u
#define CX3_PORT_MTU_CONFIGURED 8u
/* Conservative history across generations; ACCEPTED/CLOSE are current receipts. */
#define CX3_PORT_INIT_ATTEMPTED 16u
#define CX3_PORT_INIT_ACCEPTED 32u
#define CX3_PORT_CLOSE_CONFIRMED 64u
#define CX3_PORT_LAST_QUERY_FAILED 128u
struct Cx3PortInfoResult {
    uint32_t version, structBytes;
    uint64_t status, deviceGeneration;
    uint32_t port, flags;
    uint32_t queryFwStatus, initFwStatus, closeFwStatus, mtuFwStatus;
    uint32_t frameMtu, maxPayloadMtu, speedMbps, linkUp;
    uint8_t mac[6], reserved[2];
    uint64_t queryUptimeNs;
    uint32_t rawBytes, reservedWord;
    uint8_t raw[CX3_QUERY_PORT_RAW_LEN];
};
CX3_STATIC_ASSERT(sizeof(struct Cx3PortInfoResult)==152, "port info ABI");
CX3_STATIC_ASSERT(offsetof(struct Cx3PortInfoResult,raw)==88, "port info raw ABI");

/* This control does not attach a native interface, post TX frames or reset
 * the HCA. CREATE uses already-initialized shared HCA/MR/EQ. DESTROY refuses
 * an attached native primary interface and never revokes the shared MR/EQ. */
#define CX3_PORT_QUEUE_VERSION 1u
#define CX3_PORT_QUEUE_STATUS 0u
#define CX3_PORT_QUEUE_CREATE 1u
#define CX3_PORT_QUEUE_DESTROY 2u
#define CX3_PORT_QUEUE_RESERVED 1u
#define CX3_PORT_QUEUE_READY 2u
#define CX3_PORT_QUEUE_FAILED 4u
#define CX3_PORT_QUEUE_OWNERS 8u
#define CX3_PORT_QUEUE_STAGES 31u
struct Cx3PortQueueResult {
    uint32_t version, structBytes;
    uint64_t status, deviceGeneration;
    uint32_t port, action, flags, stage, failedStage, fwStatus;
    uint32_t rxQpn, rssQpn, txQpn, rxCqn, txCqn, macBase, macSlots, numPorts;
    uint64_t ownerMask, completedMask;
};
CX3_STATIC_ASSERT(sizeof(struct Cx3PortQueueResult)==96, "port queue result ABI");
CX3_STATIC_ASSERT(offsetof(struct Cx3PortQueueResult,ownerMask)==80, "port queue owners ABI");


#define CX3_NATIVE_VERSION 1u
#define CX3_NATIVE_STATUS 0u
#define CX3_NATIVE_ATTACH 1u
#define CX3_NATIVE_DETACH 2u
#define CX3_NATIVE_PRESENT 1u
#define CX3_NATIVE_ATTACHED 2u
#define CX3_NATIVE_ENABLED 4u
#define CX3_NATIVE_POISONED 8u
#define CX3_NATIVE_TX_FAULT 16u
#define CX3_NATIVE_RX_FAULT 32u
/* Three scalar inputs: port, action, expectedGeneration. Host detach does not
 * revoke device queues; destroy/create before attaching another controller. */
struct Cx3NativePortResult {
 uint32_t version,structBytes;
 uint64_t status,deviceGeneration;
 uint32_t port,action,flags,interfaceUnit;
 uint64_t rxFrames,rxBytes,rxDropped,txFrames,txCompleted,txDropped;
 uint64_t ticks,skipped;
 uint32_t rxErrors,txErrors;
 uint8_t mac[6],reserved[2];
};
CX3_STATIC_ASSERT(sizeof(struct Cx3NativePortResult)==120,"native port ABI");

/* Scalars: port, action, expectedGeneration, value. VLAN IDs 0..4094;
 * ALL/CLEAR/STATUS require value=0; RX/TX checksum take 0/1.
 * TX changes require no native controller (the family caches capabilities). */
#define CX3_NATIVE_CONFIG_VERSION 1u
#define CX3_NCFG_STATUS 0u
#define CX3_NCFG_VLAN_ALL 1u
#define CX3_NCFG_VLAN_CLEAR 2u
#define CX3_NCFG_VLAN_ADD 3u
#define CX3_NCFG_VLAN_DEL 4u
#define CX3_NCFG_RXCSUM 5u
#define CX3_NCFG_TXCSUM 6u
#define CX3_NCFG_TX_ON 1u
#define CX3_NCFG_RX_ON 2u
#define CX3_NCFG_VLAN_FILTER 4u
#define CX3_NCFG_VLAN_APPLIED 8u
struct Cx3NativeConfigResult {
 uint32_t version,structBytes;
 uint64_t status,deviceGeneration;
 uint32_t port,action,value,flags;
 uint32_t vlanCount,fwStatus;
 uint64_t vlanRevision;
};
CX3_STATIC_ASSERT(sizeof(struct Cx3NativeConfigResult)==56,"native config ABI");

#endif /* CX3_ABI_H */
