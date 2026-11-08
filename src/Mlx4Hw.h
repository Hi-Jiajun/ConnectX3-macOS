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
 * Mlx4Hw -- hardware access layer for Mellanox ConnectX-3 Pro under macOS.
 *
 * This is the macOS replacement for the bus_dma / ioremap / HCR-handshake
 * layer that FreeBSD's sys/dev/mlx4 relies on (see the handoff document,
 * section 3, for the interface census).  Only two things live here for M1:
 *   - read-only register probes (safe: no device state is modified)
 *   - the HCA reset + HCR command path (writes; the caller gates this)
 *
 * Derived from vendor/freebsd-src under the OpenIB.org BSD licence.
 */
#ifndef CX3_MLX4_HW_H
#define CX3_MLX4_HW_H


#include <IOKit/IOTypes.h>
#include <IOKit/IOReturn.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

#include "mlx4_regs.h"
/*
 * The shared ABI, for two types that belong to both sides: Cx3QueryPortResult (the
 * port answer, whose decode must be the same code the control tool runs) and
 * Cx3AbRingResult (the A/B experiment's whole output).  It is plain C with no IOKit
 * dependencies, so including it here costs nothing.
 */
#include "cx3_abi.h"

class IOBufferMemoryDescriptor;
class IODMACommand;

#include "mlx4_pure.h"

/*
 * One ICM area: a run of bus-addressable host memory whose per-page bus
 * addresses are known.
 *
 * This is what the firmware needs for its own context tables.  It is the
 * generalisation of the single-buffer case that mboxAlloc already handles, and
 * it is deliberately device-inert: allocating one touches no device register.
 *
 * The firmware's rule (mlx4_map_cmd) is that each page handed over must be
 * aligned to its own size and must not straddle a bus segment, so the allocator
 * verifies both rather than assuming them.
 */
struct Mlx4IcmPage {
    UInt64 busAddr;   /* bus address of this 4 KiB page */
    void  *kernVirt;  /* corresponding kernel virtual address */
};

struct Mlx4Icm {
    IOBufferMemoryDescriptor *md;
    IODMACommand             *cmd;
    void                     *virt;
    UInt64                    bytes;
    UInt32                    npages;
    UInt32                    nseg;
    bool                      contiguous;   /* one segment covered everything */
    bool                      pagesAligned; /* every segment a multiple of 4 KiB */
    UInt64                    busLow;       /* lowest bus address in the area */
    UInt64                    busHigh;      /* highest bus address in the area */
    UInt32                    pagesAbove4G; /* how many pages sit at or above 4 GiB */
    Mlx4IcmPage              *pages;
    UInt64                    allocationId; /* unique within this Mlx4Hw lifetime; prevents owner ABA */
    Mlx4DmaGeneration         generation;
};

/*
 * A cheap description of what is actually stored in an ICM area.
 *
 * The device is the only writer of the FW area (no host-side code in the
 * reference writes fw_icm), so "how many bytes are non-zero" is the one piece of
 * evidence that says whether the firmware image was ever DMA'd in.  A checksum
 * makes two samples comparable across a command without dumping megabytes.
 */
struct Mlx4IcmSum {
    UInt64 bytes;        /* size of the area */
    UInt64 nonzero;      /* non-zero bytes */
    UInt32 checksum;     /* additive checksum over the area's dwords */
};

/* The first 32 bytes of a QUERY_EQ answer, for a direct comparison against the
 * context this port wrote. */
struct Mlx4EqContextResult {
    bool   ok;
    UInt32 words[8];
};

class Mlx4Hw {
public:
    Mlx4Hw();

    /* Maps BAR0 (command/reset window) and BAR2.  No device writes. */
    bool  init(IOPCIDevice *pci, IOService *client);
    void  shutdown();
    void  setDeviceGeneration(UInt64 generation) { fDeviceGeneration = generation; }
    /* Stamp a complete set immediately before a command can publish its DMA ownership. */
    bool  markDmaHandoff(Mlx4Icm *const *blocks, UInt32 count);
    /* Stamp a generation-tracked hardware owner that is not backed by an ICM allocation. */
    bool  markResourceHandoff(Mlx4DmaGeneration *owner);

    bool  bar0Mapped() const { return fReg0 != 0; }
    bool  bar2Mapped() const { return fReg2 != 0; }

    /*
     * Warmed register read.
     *
     * Measured on this machine: the first MMIO access that touches a freshly
     * mapped BAR0 page reads back as zero, while the immediately following read
     * of the very same dword returns the live value.  It showed up twice in the
     * first successful boot -- the ownership word and the reset semaphore both
     * read 0x00000000 from the first access and 0x01000000 from the dump a few
     * microseconds later.  Every logic decision therefore goes through this
     * helper, which discards that first sample.
     */
    UInt32 readRegWarm(UInt32 off);
    UInt32 readRegRaw(UInt32 off);

    /* Touch one dword per 4 KiB page so no later read hits a cold page. */
    void   warmBar0();

    /*
     * Reads for the peek/poke selectors.  These go through readRegWarm, not the
     * raw reader, because the cold-read artifact is per address: the first
     * access to an address after a gap returns 0 and the second returns the
     * truth.  Measured directly on this card:
     *     peek 0xf03fc -> 1st 00000000, 2nd 01000000, 3rd 01000000
     *
     * The first version exposed the raw reader here, which made every
     * single-address measurement suspect -- the poke step's "before" read of
     * 0xf03fc reported 0x00000000 while the register actually reads
     * 0x01000000.  A diagnostic path that can report an artifact as data is
     * worse than no path at all.
     */
    UInt32 readBar0(UInt32 off) { return readRegWarm(off); }
    UInt32 readBar2(UInt32 off);

    /* Single 32-bit store, used by the guarded poke selector.  Named so the
     * build-time boot-path check can see it. */
    void   writeBar0(UInt32 off, UInt32 value);
    void   writeBar2(UInt32 off, UInt32 value);

    /* ---- ICM: bus-addressable host memory for the firmware's tables ----
     *
     * Allocating and freeing are device-inert.  Handing the pages to the
     * firmware is a separate step (MAP_FA / MAP_ICM), so an allocator fault can
     * be told apart from a command fault. */
    /*
     * lowmem constrains the area below 4 GiB. The reference asks for GFP_HIGHUSER
     * here, i.e. it does not care, so this is only for testing whether the
     * firmware can fetch from high bus addresses.
     */
    Mlx4Icm *icmAlloc(UInt32 npages, bool lowmem = false, bool contiguous = false);
    void     icmFree(Mlx4Icm *icm);
    /* Verify that every page is 4 KiB aligned and inside one segment. */
    bool     icmCheck(const Mlx4Icm *icm, UInt32 *badPage);

    /* ---- M2b: hand the FW area to the firmware and start it ----
     *
     * The reference order is QUERY_FW, then allocate fw_pages of ICM, then
     * MAP_FA, then RUN_FW, then MOD_STAT_CFG.  QUERY_DEV_CAP only answers after
     * that; sending it earlier returns CMD_STAT_BAD_SYS_STATE, which is exactly
     * what happened before this was understood.
     *
     * Each step reports the firmware's own status code so a failure is
     * attributable to one command. */
    bool mapFa(Mlx4Icm *icm, UInt32 *outEntries, UInt32 *outFwStatus);
    /* The same handover, but for a context table: each block also carries the
     * virtual address the firmware should see it at.  `virt` advances by the
     * block size across the whole list, including across batch boundaries.
     * (FreeBSD mlx4_MAP_ICM, mlx4_icm.c:228.) */
    bool mapIcm(Mlx4Icm *icm, UInt64 virt, UInt32 *outEntries, UInt32 *outFwStatus);
    bool runFw(UInt32 *outFwStatus);
    bool modStatCfg(UInt8 logPgSz, UInt8 logPgSzM, UInt32 *outFwStatus);

    /* Shared implementation of MAP_FA and MAP_ICM. */
    bool mapCmd(UInt16 op, Mlx4Icm *icm, bool sendVirt, UInt64 virt,
                UInt32 *outEntries, UInt32 *outFwStatus);

    /* PCI config access for the FLR selector. */
    UInt8  findPcieCapability();
    UInt32 configRead(UInt8 off);
    void   configWrite(UInt8 off, UInt32 value);

    /* FreeBSD mlx4_free_ownership() (mlx4_main.c:3083): writel(0) + msleep(1s). */
    bool   clearOwner();

    /* Log the raw first/second/third sample for the suspect addresses. */
    void   diagnoseFirstRead();

    /* ---- read-only probes ------------------------------------------ */
    /*
     * The ownership gate.
     *
     * readOwnerWordRaw() reads the register exactly once, which is what the
     * reference driver does (FreeBSD/Linux mlx4_get_ownership: ioremap, one
     * readl, iounmap, then !!ret).  That matters, because at this address the
     * first read returns 0 and later reads return 0x01000000 -- measured
     * identically on macOS and on Linux, through two different access paths.
     *
     * So the reference's "do we own the device?" test passes because it samples
     * the first read.  Reading it "properly" and taking the settled value makes
     * the test fail.  The gate is therefore not a meaningful signal on this
     * hardware, and the port has to read it the same way the reference does to
     * behave the same way.
     *
     * readOwnerWord() keeps the settled value for diagnostics, which is what
     * shows the divergence in the log.
     */
    UInt32 readOwnerWordRaw();  /* BAR0 + 0x8069c through the existing mapping */
    UInt32 readOwnerWord();     /* BAR0 + 0x8069c, settled value (diagnostic) */

    /*
     * The ownership check, performed the way the reference performs it.
     *
     * FreeBSD/Linux mlx4_get_ownership() creates a fresh mapping, reads once, and
     * unmaps.  On this platform the first read through a fresh mapping returns 0
     * while every later read returns 0x01000000 -- measured on macOS and on Linux,
     * through two unrelated access paths.  So the reference's check always passes
     * here, on an artifact, and does not actually gate anything.
     *
     * Reproducing it faithfully means creating a mapping for the check rather than
     * reusing the persistent one, which is what this does.  What it returns is
     * therefore the reference's answer, not a "corrected" one.
     */
    UInt32 checkOwnershipAsReference();

    /*
     * Read one BAR0 dword through a freshly created mapping, once.
     *
     * This is how the reference reads every gate it has, because it ioremaps a
     * throwaway window for each check rather than keeping one.  It matters on
     * this hardware: the first read through a fresh mapping returns 0 and later
     * reads return the real value, so the reference's checks pass on the artifact.
     * Reusing a persistent mapping makes them fail.
     *
     * Used for the ownership word and for the reset semaphore, so both gates see
     * what the reference sees.
     */
    UInt32 readBar0Fresh(UInt32 off);
    UInt32 readHcrStatus();     /* BAR0 + 0x80680 + 0x18, host order */
    UInt32 readResetSem();      /* BAR0 + 0xf0000 + 0x3fc, raw */
    void   dump(const char *tag, UInt32 regOff, UInt32 nDwords);

    /*
     * A PCI memory window whose space decoding is disabled floats high, so every
     * read returns 0xffffffff.  That is exactly what the first boot of this kext
     * produced (PCI COMMAND was 0x0000).  Everything that would write to the
     * device is gated on this check, so a disabled window can never be mistaken
     * for a live one.
     */
    bool   windowAlive();
    UInt64 bar0Physical() const;

    /* ---- write path (caller decides whether to run it) ------------- */
    /*
     * Allocate the command mailbox by itself, touching no device registers.
     * This exists as a separate step so that a user-space bisect can tell a DMA
     * allocation problem apart from a register-write problem without rebooting
     * between the two.
     */
    bool  allocMailbox();
    void  freeMailbox() { mboxFree(); }
    UInt64 mboxPhys() const { return fMboxPhys; }
    bool hasMailboxAllocation() const { return fMboxMd || fMboxCmd || fMboxRetained; }
    bool mailboxNeedsRetention() const;
    UInt32 mboxSegCount() const { return fMboxSegCount; }
    UInt64 mboxOffset() const { return fMboxOffset; }

    /*
     * FreeBSD mlx4_reset().
     *
     * bypassSemaphore exists because the semaphore gate carries no information on
     * this hardware. Measured: 0xf03fc reads a constant 0x01000000 and ignores
     * writes (0xa5a5a5a5 written, 0x01000000 read back, repeatedly), as does the
     * ownership word. The reference passes this gate only because its read happens
     * to be the first read of that physical page since boot, which returns 0 as a
     * one-shot; this port's own boot probe consumes that. So the gate is a coin
     * flip on who reads first, not a device property, and the reset has to be able
     * to proceed deliberately past it.
     */
    bool  resetHca(bool bypassSemaphore = false);
    /*
     * macOS IOPCIDevice function-level reset (FLR) adapter.
     *
     * Adapts fPci->reset(kIOPCIDeviceResetTypeFunctionReset, kIOPCIDeviceResetOptionNone).
     * Returns kIOReturnNotReady if fPci is null; otherwise returns the exact platform status.
     * OptionTerminate is forbidden. Does no retries, sleeps, fallbacks, or register reads.
     */
    IOReturn platformFunctionReset();
    /* Read PCI IDs and BDF from the current provider; no MMIO or writes. */
    IOReturn readPciIdentity(Mlx4PciIdentity *out);
    /* Bounded read-only conventional PCI config sample; no MMIO or HCR access. */
    IOReturn queryPcieLink(Cx3PcieLinkResult *out);
    bool  queryFw(Mlx4QueryFwResult *out);  /* FreeBSD mlx4_QUERY_FW() */
    /*
     * The same QUERY_FW, but with the outbox placed at a page of the given ICM
     * area instead of the dedicated mailbox.  Used to prove whether the device
     * can actually DMA into that memory; see kCx3SelFwOutbox in cx3_abi.h.
     */
    bool  queryFwAt(Mlx4Icm *icm, UInt32 page, Mlx4QueryFwResult *out,
                    UInt32 *outFwStatus, bool ignoreBusy);
    /* FreeBSD mlx4_QUERY_DEV_CAP(); bypassBusy is the experiment switch above. */
    bool  queryDevCap(Mlx4QueryDevCapResult *out, bool bypassBusy = false);
    /*
     * QUERY_DEV_CAP again, but decoded into the fields the profile and INIT_HCA
     * need.  Same command shape as queryDevCap(); this one keeps the values.
     */
    bool  parseDevCap(Mlx4DevCap *out, bool bypassBusy = false);
    /*
     * mlx4_make_profile() -- FreeBSD mlx4_profile.c:69.  Packs the requested
     * object counts into the HCA's context memory and returns the total size in
     * bytes (0 on failure, e.g. when the profile does not fit max_icm_sz).
     *
     * tableCapacity and numTablesOut are separate parameters on purpose.  The
     * first version of this took one pointer that meant "how many slots you have"
     * on the way in and "how many I wrote" on the way out, and cleared it before
     * reading it -- so the capacity became 0 and the table list was reported as
     * overflowing.  An in/out count is a trap; two names cannot be confused.
     */
    UInt64 makeProfile(const Mlx4Profile *request, const Mlx4DevCap *cap,
                       Mlx4InitHcaParam *param, Mlx4IcmTable *tables,
                       UInt32 tableCapacity, UInt32 *numTablesOut);
    /* mlx4_SET_ICM_SIZE(): tells the HCA how much context memory to expect and
     * gets back how much auxiliary memory it wants (mlx4_fw.c:2542). */
    bool  setIcmSize(UInt64 icmSize, UInt64 *auxPages, UInt32 *outFwStatus);
    /* mlx4_MAP_ICM_AUX() / mlx4_UNMAP_ICM_AUX() (mlx4_fw.c) */
    bool  mapIcmAux(Mlx4Icm *icm, UInt32 *outFwStatus);
    bool  unmapIcmAux(UInt32 *outFwStatus);
    /* mlx4_UNMAP_ICM() (mlx4_icm.c:305) */
    bool  unmapIcm(UInt32 npages, UInt64 virt, UInt32 *outFwStatus);
    /* mlx4_INIT_HCA() -- FreeBSD mlx4_fw.c:1819. */
    bool  initHca(const Mlx4InitHcaParam *param, const Mlx4DevCap *cap,
                  UInt32 *outFwStatus);
    /* FreeBSD mlx4_CLOSE_HCA(dev, panic=0) -- mlx4_fw.c:2365-2369. */
    bool  closeHca(UInt32 *outFwStatus);
    /*
     * Record where the MTT table is, so the queue code can write entries into it
     * directly.  Called by the M2d bring-up once the table is mapped: the HCA
     * only needs the MTT base address in the EQ context, so without this the
     * queue code would have to look the table up again.
     */
    bool  setMttTable(void *hostBase, UInt64 bytes, UInt32 reservedEntries,
                      UInt64 hcaVirt);
    bool  hasMttAllocations() const { return fMttAllocator.allocationCount != 0; }

    /*
     * ------------------------- M3, restart 1 -------------------------
     */

    /*
     * A command posted in event mode: the HCR's E bit is set and the token goes
     * into the token field, and the HCA reports completion by writing a CMD EQE
     * into the event queue instead of by clearing GO alone.
     *
     * This is the difference that matters for M3: with E = 0 (every command so
     * far) no CMD EQE is ever produced, so "the command completed" says nothing
     * about whether the EQ path works.  mlx4_cmd.c:487-495 sets E for the event
     * path, and mlx4_cmd_wait allocates the token it waits for (:349-352).
     */
    bool  cmdEvent(UInt64 inParam, UInt64 outParam, UInt32 inModifier, UInt8 opMod,
                   UInt16 op, UInt16 token, UInt32 timeoutMs, UInt32 *outFwStatus,
                   UInt32 *outHcrOnPost = 0, UInt32 *outHcrAtEnd = 0);
    /*
     * The token a posted command carried, as read back from the HCR's token field
     * (+0x14, high 16 bits).  Worth a separate accessor because the token is NOT in
     * the command dword: that dword's high bits are T, E and opmod.  The tool used
     * to print them as "token field", which was wrong and hid the fact that the
     * real token dword was never being checked.
     */
    UInt32 hcrTokenField() const;

    /* mlx4_QUERY_FUNC() -- FreeBSD mlx4_fw.c:1739.  Outbox command; on a device
     * with system EQs it returns this function's EQ/UAR allocation. */
    bool  queryFunc(UInt32 *outMaxEq, UInt32 *outReservedEq, UInt32 *outReservedUar,
                    UInt32 *outFwStatus);

    /*
     * MTT entries: the table that tells the HCA where a queue's pages are.
     *
     * Only the native (non-multi-function) path is ported: the reference writes
     * the entries straight into the mapped MTT table (mlx4_mr.c:697-721) rather
     * than sending a command, because on a PF the table is host memory.
     *
     * The MTT table's first 256 KiB chunk is mapped in full at mtt_base, which is
     * 32768 entries -- comfortably more than the few pages an EQ needs.  The
     * allocator here hands out from the low end above the reserved prefix and
     * hands them back on release; there is no bitmap because nothing else
     * allocates MTTs yet (the reference's mlx4_alloc_mtt_range is a bitmap over
     * the same table).
     */
    /*
     * outMTTAddr is the byte address the HCA is given for this MTT range.  The
     * EQ context carries it in a 32-bit field plus an 8-bit high byte
     * (mlx4_eq.c:1029-1030), so the value has to fit; the table is at
     * 0x1000c0000 in HCA space, which does.
     */
    bool  mttAlloc(UInt32 npages, Mlx4Icm *owner, UInt32 *outOffsetEntries,
                   UInt32 *outMTTAddr);
    bool  mttAllocAbDummy(UInt32 npages, UInt32 *outOffsetEntries,
                          UInt32 *outMTTAddr);
    bool  mttFree(UInt32 offsetEntries, UInt32 npages, Mlx4Icm *owner);
    bool  mttWrite(UInt32 offsetEntries, UInt32 npages, const UInt64 *busAddrs);

    /*
     * The event queue itself: allocate the ring, point MTT at it, SW2HW_EQ,
     * MAP_EQ the events we care about, and arm it.
     *
     * The ring is allocated as ordinary host memory and its per-page bus
     * addresses go into the MTT, which is what the HCA DMAs into.  The EQ context
     * carries only the MTT base address and the geometry, so the mapping and the
     * context have to agree or the HCA writes somewhere the host is not looking.
     */
    bool  eqCreate(UInt32 nent, UInt8 intr, UInt8 eqeFactor, UInt32 eqn,
                   bool mttAbs, Mlx4Icm **outRing, UInt32 *outPages,
                   UInt32 *outMTTOffset, UInt64 *outMTTAddr,
                   UInt64 *outDoorbellAddr, UInt32 *outFwStatus);
    bool  eqMap(UInt64 eventMask, UInt32 eqn, bool unmap, UInt32 *outFwStatus);
    /*
     * mlx4_HW2SW_EQ() -- FreeBSD mlx4_eq.c:903-907.
     *
     * Takes the queue back from the firmware.  Note the asymmetry with SW2HW_EQ:
     * this one is sent with opmodifier 1 and carries no inbox at all.  It is what
     * makes releasing the ring and its MTT entries safe -- without it the HCA still
     * holds a page list pointing at host memory that is about to be recycled.
     */
    bool  eqHw2Sw(UInt32 eqn, UInt32 *outFwStatus);

    /*
     * The ring-initialisation A/B experiment (M2B-NOTES section 19).
     *
     * The claim under test is a hypothesis, not a measurement: this port observed
     * that after SW2HW_EQ the ring's memory changes (zero after memset, zero just
     * before SW2HW_EQ, 0xcc just after) and that the settled state takes a while to
     * arrive.  That is a correlation.  To turn it into a cause, two identically
     * built rings are allocated, only ONE of them is handed to the firmware, and
     * both are sampled on the same clock -- so the untouched ring is a control that
     * goes through every host-side step the other one does.
     *
     * Two independent reviews of the design changed it in three ways that matter:
     *   - a full 32 KiB snapshot is diffed byte by byte, because "nonzero count +
     *     a head digest" cannot show that the rest of the ring is untouched;
     *   - the sample clock is real wall time (clock_get_uptime) and the command's
     *     own duration is measured, because the figure this project had been
     *     quoting was a loop count and one loop is not one millisecond;
     *   - both rings' addresses and MTT offsets are reported, so "two independent
     *    rings" is evidence rather than an intention.
     */
    bool  eqAbExperiment(bool handOverB, UInt32 eqn, UInt32 nent, UInt8 eqeFactor,
                         UInt32 dummyMtt, Cx3AbRingResult *out);
    bool  hasAbRetainedRings() const;
    bool  retryAbRetainedRings();

    /*
     * R3: the port.  QUERY_PORT is read-only; the other two write port
     * configuration and must only be reached through the gated selector.
     */
    bool  queryPort(UInt32 port, Cx3QueryPortResult *out);
    bool  queryHca(Cx3QueryHcaResult *out);

    /*
     * R4 stage 1: protection domains and one memory region.
     *
     * PDs are software-only on a native PF (mlx4_pd.c:47-56 -- a bitmap, no command),
     * so the only thing that can be wrong is the range: the firmware's own PDs occupy
     * 0..reserved_pds-1 and must not be handed out.
     */
    void  setPdRange(UInt32 reserved);
    bool  pdAlloc(UInt32 *outPdn);
    /* Hand a PD number back.  Only for the case where the MPT step failed and the
     * number was never referenced by anything: this is the bump allocator's only
     * rollback, and it only works if the number was the last one handed out. */
    void  pdFree(UInt32 pdn);

    /*
     * The "physical" memory region the Ethernet driver uses for everything.
     *
     * mlx4_en_main.c:211-219 allocates exactly one MR with iova 0, size ~0, LOCAL_READ
     * | LOCAL_WRITE, npages 0.  With npages 0 the MTT has order < 0, and
     * mlx4_mr_enable (mlx4_mr.c:655-670) then sets MLX4_MPT_FLAG_PHYSICAL and
     * mtt_addr = 0 -- there is no page list at all, the region is physical.  That MR's
     * key is what every RX and TX descriptor carries as its lkey.
     */
    bool  mrEnablePhysical(UInt32 mptIndex, UInt32 pdn, UInt64 iova, UInt64 size,
                           UInt32 access, Mlx4DmaGeneration *owner, UInt32 *outFwStatus);

    /*
     * The dMPT table's host view, handed in by the provider the same way the MTT
     * table is.  Used for one thing: reading an MPT entry back after SW2HW_MPT to
     * see what the firmware did with it, rather than repeating what was sent.
     */
    void  setMptTable(void *hostBase, UInt64 bytes, UInt32 objSize);
    bool  mptEntryRead(UInt32 mptIndex, UInt8 *out, UInt32 need);

    /*
     * QUERY_MPT -- the firmware's own view of the entry.
     *
     * Needed as a second opinion on the host-memory readback, because the two are not
     * the same claim.  The table is host memory the driver can read
     * (mlx4_mr_hw_get_mpt reads it directly on a non-mfunc device, mlx4_mr.c:318-323),
     * but it does not follow that the firmware writes the entry's status back there: in
     * the fast-register path the DRIVER maintains that byte itself, writing
     * MLX4_MPT_STATUS_SW and then MLX4_MPT_STATUS_HW around the update
     * (mlx4_mr.c:1018,1040).  So "the status nibble is still 0xf" could mean the entry
     * was not taken OR that nothing writes it back, and only asking the firmware
     * separates those.
     */
    bool  queryMpt(UInt32 mptIndex, UInt8 *out, UInt32 need, UInt32 *outFwStatus);

    /*
     * Take an MPT entry back from the hardware -- HW2SW_MPT (mlx4_mr.c:290-295).
     *
     * op_modifier 1 with no outbox is the "no output mailbox" form the reference uses
     * when it does not want the entry back.  It has to happen before the dMPT table is
     * released: an entry the hardware still owns points into that table, and freeing
     * the table underneath it is the same mistake teardownEq refuses to make with an EQ
     * ring.
     */
    bool  mrHw2Sw(UInt32 mptIndex, UInt32 *outFwStatus);

    /*
     * R4 stage 2: a completion queue.
     *
     * cqCreate makes the ring, points the CQ context's MTT at it, and hands it over with
     * SW2HW_CQ.  The context is built here rather than by the caller because every field
     * in it has a specific encoding and two of them are the ones both reviews of this
     * stage called out: the UAR index inside logsize_usrpage, and db_rec_addr being the
     * DMA BYTE ADDRESS of the record (mlx4_cq.c:284) rather than an offset or a UAR
     * address.  Getting either wrong produces a CQ whose completions never arrive.
     */
    bool  cqCreate(UInt32 cqn, UInt32 nent, UInt8 eqn, UInt32 uarIndex,
                   Mlx4Icm **outRing, UInt32 *outPages, UInt32 *outMttOffset,
                   UInt32 *outMttAddr, UInt64 *outDbRecordPhys, Mlx4Icm **outRec,
                   UInt32 *outOwnerBefore, UInt32 *outOwnerAfter,
                   UInt32 *outFwStatus);
    bool  cqHw2Sw(UInt32 cqn, UInt32 *outFwStatus);

    /*
     * The completion-index record and the ring's doorbell.
     *
     * Two addresses that must not be confused: the RECORD is host memory the driver and
     * the hardware both write (the CI the driver has consumed), and the DOORBELL is MMIO
     * that tells the hardware there is something to read.  mlx4_cq_set_ci writes the
     * record only (cq.h:163-166); the doorbell is a separate 8-byte write.
     */
    void  cqSetCi(volatile UInt32 *record, UInt32 consIndex);
    /*
     * mlx4_cq_arm's first step (cq.h:149): the ARM WORD into the record's SECOND dword,
     * before any MMIO.  The CQ's record is a pair -- set_ci_db at word 0 and arm_db at word 1
     * (mlx4_en_cq.c:122-123) -- so writing the arm word over word 0 would address a different
     * register entirely, and the mistake would be silent in both directions.
     *
     * The value is [sn | cmd | ci], the CONSUMER INDEX, while the doorbell below carries
     * [sn | cmd | cqn] and the index separately (cq.h:149 against :157-159).
     */
    void  cqSetArmDb(volatile UInt32 *record, UInt32 armSn, UInt32 cmd, UInt32 consIndex);
    /*
     * The UAR page is a PARAMETER since 0.43.11: the arm probe of docs/M2B-NOTES.md section 52
     * has to be able to knock on the same page the QPs were built with (the runtime index in
     * their contexts) and on a reserved one, and those two answers mean different things.
     * Before that this function hard-coded MLX4_NUM_RESERVED_UARS, which happens to be the
     * same number on this card -- "happens to be" is what section 51.4 is a list of.
     */
    void  cqRingDoorbell(UInt32 cqn, UInt32 consIndex, UInt32 armSn, bool arm, UInt32 uarIndex);

    /*
     * The SEND queue's doorbell record (the QP context's db_rec_addr, host memory).
     *
     * Two 32-bit words, and which word is which is the open question this exists to ask: the
     * public PRM says the receive and send records are "consecutive in memory" and that the
     * send one holds `sq_wqebb_counter`, the count of WQEs posted since creation, pointing at
     * the first EMPTY WQEBB (section 7.4.2, quoted in docs/EXTERNAL-REFS.md section 2) --
     * while the posting procedure's step 2 requires updating it before the doorbell rings
     * (section 7.4.3.1).  No implementation in the four this project checked writes it for a
     * send queue (docs/M2B-NOTES.md section 51.4), so which word the hardware reads, if
     * either, is not known here: the caller writes one, both, or neither and the answer is
     * the measurement.
     */
    void  sqSetDoorbellRecord(volatile UInt32 *record, UInt32 word0, UInt32 word1);

    /* Read one CQE out of a CQ ring, using the reference's readiness rule.  Same shape
     * as eqNextEqe: the owner bit is compared against the consumer index's phase. */
    bool  cqNextCqe(const Mlx4Icm *ring, UInt32 nent, UInt8 cqeFactor,
                    UInt32 consIndex, UInt8 *outCqeLegacy);

    /*
     * R4 stage 3: the receive ring's WQEs.
     *
     * Allocates the ring (with the 64-byte header in front of the WQEs), a buffer per WQE
     * from the same page-list allocator, writes each WQE's data segment, and points the
     * context's MTT at the ring.  It does NOT hand the ring to the hardware: that is
     * rqEnable, and keeping the two apart means "the memory is built correctly" and "the
     * hardware took it" stay separable -- the same split that made stage 1 and stage 2
     * debuggable.
     */
    bool  rqCreate(UInt32 nent, UInt32 bufBytes, UInt32 lkey, Mlx4Icm **outRing,
                   Mlx4Icm **outBufs, UInt32 *outPages, UInt32 *outMttOffset,
                   UInt32 *outMttAddr, UInt64 *outBufPhysFirst, UInt32 *outBufPhysCount,
                   UInt64 *outRingPhysFirst, UInt32 *outRingPhysCount);

    /* Write one WQE's data segment.  Exposed so the selector can rewrite a WQE after a
     * buffer is recycled, and so a test can check the encoding without a queue. */
    /*
     * bufPhys is UInt64, and that is a fix rather than a style choice: this card's DMA
     * addresses routinely exceed 4 GiB -- the allocation logs show bus ranges into the
     * tens of GiB and count 'pages at or above 4 GiB' -- so a UInt32 parameter here
     * silently put only the LOW HALF of the address into the WQE.  Nothing failed; the
     * descriptor just pointed somewhere else.  Caught by reading the WQE back.
     */
    void  rqWriteWqe(UInt8 *ringVirt, UInt32 stride, UInt32 index, UInt64 bufPhys,
                     UInt32 byteCount, UInt32 lkey);

    /* The producer index record (mlx4_en_rx.c:250-254). */
    void  rqSetProducer(volatile UInt32 *record, UInt32 prod);

    /*
     * The bus address of receive buffer `index`, from the same arithmetic rqCreate used to
     * write that buffer's WQE. Small buffers divide a page. Jumbo buffers span pages,
     * and every page of that one buffer must have a contiguous DMA bus address;
     * physical contiguity alone is not sufficient. It is a function rather than a copy
     * because stage 9 re-posts WQEs by index: "the address the refill writes" and "the address
     * the first fill wrote" have to be the same number, and two copies of the arithmetic are
     * how they stop being (docs/DEFECTS.md D12).
     */
    UInt64 rqBufferPhys(const Mlx4Icm *bufs, UInt32 bufBytes, UInt32 index) const;

    /*
     * R4 stage 9: one send WQE, and the doorbell that hands it to the hardware.
     *
     * The write order is the reference's (mlx4_en_tx.c:897-919) and every step of it is
     * load-bearing:
     *
     *   the control segment's other fields and the data segment are written first;
     *   a barrier, so the descriptor is in memory before the owner bit says it is the
     *       hardware's;
     *   the owner_opcode WORD last, because that word is what transfers ownership;
     *   a second barrier;
     *   and only then does the caller ring the doorbell.
     *
     * The first dword is built as `MLX4_OPCODE_SEND | (prod & 0xffff) << 8 | owner << 31` --
     * the opcode in the LOW EIGHT BITS (mlx4_en_tx.c:682-683, :761-762, :897; see the comment
     * on those constants, and tools/test-cqe-offsets.c, for why the shift matters).
     *
     * `outImage32`, when not null, receives the 32 bytes as they stand in memory AFTER the
     * owner word is written -- the caller reads them back for the stage's first criterion
     * instead of trusting that the writer and the reader agree.
     */
    /*
     * `opcode` is a parameter since 0.43.11 and it is the ONLY thing the NOP-versus-SEND
     * experiment moves (docs/M2B-NOTES.md section 52).  It reaches the queue through the same
     * single write of the owner_opcode word, after the same two barriers, as before -- the
     * alternative, patching the byte afterwards, would put a second store into the word that
     * transfers ownership, which is the one ordering this writer exists to get right.
     *
     * `nDataSegs` is the number of 16-byte data segments the descriptor carries, and the
     * fence_size byte is DERIVED from it (TX_WQE_FENCE_SIZE) rather than passed in, because
     * passing it in as a literal is how this port wrote 1 for four builds: the field is the
     * descriptor's size in 16-byte units including the control segment, not the segment count
     * (mlx4_regs.h has the derivation and the three implementations behind it).  With
     * nDataSegs = 1 the byte is 2.
     */
    void  sqWriteSendWqe(UInt8 *sqVirt, UInt32 index, UInt32 prod, UInt32 ownerBit,
                         UInt32 byteCount, UInt32 lkey, UInt64 bufPhys, UInt32 nDataSegs,
                         UInt32 opcode, UInt8 *outImage32, bool verbose,
                         UInt32 csumFlags = 0, bool insertVlan = false, UInt16 vlanTag = 0);

    /*
     * Stamp a reclaimed 64-byte TXBB with the transmit convention, exactly as
     * mlx4_en_stamp_wqe does (mlx4_en_tx.c:271-287): the first dword of every 64-byte unit of
     * the descriptor becomes `STAMP_VAL | owner << STAMP_SHIFT`, i.e. an opcode field of 0xff
     * -- a value that is not a work request -- carrying the owner phase the slot will be
     * posted in next.  "Free" and "posted" are therefore distinguishable in memory, which is
     * the property stage 6's C12 fix restored at creation time.
     */
    void  sqStampTxbb(UInt8 *sqVirt, UInt32 index, UInt32 owner);

    /*
     * The send doorbell: BE32(qpn << 8) into the UAR page at MLX4_SEND_DOORBELL (0x14).
     *
     * Two things this must not get wrong, both of them silent: the QPN is SHIFTED LEFT by
     * eight (mlx4_en_tx.c:231 -- the register carries a QPN, not a producer index), and the
     * offset is 0x14, not the CQ's 0x20 (doorbell.h:41-42).  The UAR page comes from the QP
     * context's own usr_page, the runtime index, exactly as the CQ's doorbell does.
     */
    void  sqRingSendDoorbell(UInt32 qpn, UInt32 uarIndex, bool verbose);

    /*
     * R4 stage 6: the send queue's memory.
     *
     * One allocation holding the WQE array, an MTT entry per page, and nothing else -- no
     * descriptors are written, because nothing is being transmitted yet (that is stage 9).
     * The layout does NOT mirror rqCreate's, and that asymmetry is the hardware's: the receive
     * ring keeps a 64-byte unit in front of its descriptors (mlx4_en_rx.c:406, :482-489) and
     * the transmit ring keeps nothing (mlx4_en_tx.c:139, :686), so WQE i of this queue is at
     * the allocation's base + i * 64 -- the base itself, with no header to add.  Getting that
     * wrong is docs/DEFECTS.md E13.
     *
     * Every 64-byte unit's first dword is stamped with MLX4_SQ_INIT_OWNER_BIT ("software owns",
     * 0xffffffff) -- the reference does that to its whole TX ring right after the clear
     * (mlx4_en_netdev.c:1373-1375, docs/DEFECTS.md C12) and it is the state the send path's
     * reclaim scan reads.  `outStampValue` is WQE 0's first dword read back out of the
     * memory, `outTxbbsStamped` counts how many of the WQE array's TXBBs carry the same stamp
     * (wanted: all of them), and `outWqeNonZeroBytes` counts bytes in the WQE array BEYOND
     * those stamps -- which is the actual "was anything posted" question, and must be 0.
     *
     * The MTT entries are the SQ's own -- mlx4_en_tx.c:238-239 hands the TX ring's
     * wqres.mtt to mlx4_qp_to_ready -- which is this function's reason for existing before
     * any QP does: the QP context's mtt_base_addr_l has to name them.
     */
    bool  sqCreate(UInt32 nent, Mlx4Icm **outRing, UInt32 *outPages, UInt32 *outMttOffset,
                   UInt32 *outMttAddr, UInt64 *outRingPhysFirst, UInt32 *outRingPhysCount,
                   UInt32 *outStampValue, UInt32 *outTxbbsStamped,
                   UInt32 *outWqeNonZeroBytes);

    /*
     * R4 stage 5, first half: the port's MAC table.
     *
     * The command carries the WHOLE table (1024 bytes = 128 entries) in its mailbox, and is
     * sent again in full for every add or remove (mlx4_port.c:127-146, :299-301).  This port
     * has one address, so the table it sends is "one entry set, 127 clear".
     *
     * The two directions are separate functions because they are separate evidence: the
     * write takes the mailbox (in_param = its DMA address) and the read sends the same
     * command with in_param 0, asking the firmware to put the table it holds into the
     * outbox.  "Accepted" and "stored" are different claims, and the reference never reads
     * this table back, so the read direction is a measurement rather than a copy of one.
     *
     * The write takes the WHOLE TABLE and copies it byte for byte, so there is no slot
     * parameter: "the table that was sent" and "the table the caller built" are the same
     * bytes, and the position of the address inside them has exactly one representation --
     * the caller's.  The revision before this one took "one entry + a slot number" and built
     * the table inside the hardware layer, which let the caller report slot 0 while the
     * command carried slot 1 (docs/DEFECTS.md D12).  `tableBytes` must be the whole
     * MLX4_MAC_TABLE_SIZE, because the command replaces the firmware's table rather than
     * editing one entry of it.
     */
    bool  setPortMacTableWrite(UInt32 port, const UInt8 *table, UInt32 tableBytes,
                               UInt32 *outFwStatus);
    bool  setVlanFilter(UInt32 port,const UInt8 *vidBits,UInt32 *outFwStatus);
    /*
     * Disabled compatibility entrypoint: no documented MAC-table read command is implemented.
     * Returns NOT_POSTED/false without touching the hardware.
     */
    bool  setPortMacTableRead(UInt32 port, UInt32 entryIndex, UInt8 *outEntry,
                              UInt32 entryBytes, UInt32 *outFwStatus);

    /*
     * R4 stage 4: the RX QP, walked from RST to RTS.
     *
     * qpModify builds the 248-byte context (at mailbox +8, NOT +0 -- that is the CQ's
     * habit and the first thing this stage gets wrong) and sends one state-transition
     * command.  The caller keeps one context between the three calls, which is what the
     * reference does (mlx4_qp.c:912-926) and what makes the `flags` nibble the only thing
     * that differs between them.
     *
     * The context travels as raw bytes rather than as a struct for the same reason the CQ
     * and EQ contexts are built from offsets: the layout is a list of big-endian fields
     * with nested paths, and a struct here would be a second place for the offsets to
     * live.  The offsets are in mlx4_regs.h beside the other context layouts, and every
     * field of the buffer is written here, in one place, so there is nothing left for the
     * caller to encode.
     *
     * `size` and `stride` describe the ring being configured and their MEANING depends on
     * `isTx`, exactly as in the reference's single pair of arguments
     * (mlx4_en_fill_qp_context(priv, size, stride, is_tx, rss, ...), mlx4_en_resources.c:46):
     * for a receive QP they are the RQ's WQE count and WQE size, for a send QP the SQ's
     * TXBB count and TXBB size.  `isTx` also decides the three lines that differ between
     * the two kinds of queue -- which of the two size/stride bytes is written, and whether
     * param3 carries FCS_KEEP (the receive path's, mlx4_en_rx.c:950-951).
     *
     * `userPrio` is the reference's fourth-from-last argument and is passed through rather
     * than derived, because it is the ONE argument whose SIGN changes the context: the
     * reference's fill sets pri_path.feup = MLX4_FEUP_FORCE_ETH_UP (and ORs the priority into
     * sched_queue) only when `user_prio >= 0` (mlx4_en_resources.c:62-65).  Its transmit
     * caller passes a queue index -- always >= 0 -- and BOTH of its receive-side callers pass
     * -1 (mlx4_en_rx.c:946 for the ring, :1057 for the RSS indirection QP), so the sign is
     * the reference's own way of saying "this queue sends" / "this queue only receives".  It
     * is a parameter here so that those two constants have to be written down at each call
     * site, where a reader can compare them with the reference's line, instead of being
     * inferred from `isTx` (which happens to agree today and is a different statement).
     */
    void  qpFillContext(UInt8 *context, UInt32 pdn, UInt32 qpn, UInt32 uarIndex,
                        UInt32 size, UInt32 stride, UInt32 cqn, UInt64 dbRecAddr,
                        UInt32 mtuMsgmax, UInt32 port, bool fcsKeep, UInt32 counterIndex,
                        bool rss, bool isTx, int userPrio);
    /*
     * The RSS overlay on top of a context this function just filled, at QPC + 0x3c
     * (mlx4_en_rx.c:1067-1075).  Separated from the fill for the same reason the
     * reference separates them: the fill is `mlx4_en_fill_qp_context` and the overlay is
     * `mlx4_en_config_rss_steer`'s next few lines, and the overlay is what makes a QP an
     * indirection QP rather than a queue.
     *
     * `baseQpn` is the number the firmware hashes INTO (the RX ring's QPN here) and
     * `rssRings` is how many rings it may choose among -- the count is encoded in
     * base_qpn's top byte, so it is a parameter rather than a constant.
     */
    void  qpFillRssContext(UInt8 *context, UInt32 baseQpn, UInt32 rssRings,
                           UInt32 baseQpnUdp, bool udpRss, const UInt8 *key,
                           UInt32 keyBytes);
    /* One state transition.  `context` is the 248-byte image and is what the command's
     * mailbox carries, at offset QP_CONTEXT_MAILBOX_OFFSET. */
    bool  qpModify(UInt16 op, UInt32 qpn, const UInt8 *context, UInt32 *outFwStatus,
                   UInt8 *outMailbox = 0, UInt32 outBytes = 0,
                   Cx3QpCaptureObservation *outObservation = 0);

    /*
     * mlx4_qp_query (mlx4_qp.c:884-898): QUERY_QP, whose answer is at mailbox +8.
     *
     * This is the only thing that separates "the command returned 0" from "the hardware is
     * in the state asked for".  Its inbox is empty -- cmd_box(in=0, out=mailbox, ...) --
     * and the response is the context as the FIRMWARE sees it, which is also how a field
     * the firmware rewrites (local_qpn, the MTT address) gets checked rather than restated.
     */
    bool  qpQuery(UInt32 qpn, UInt8 *outContext, UInt32 need, UInt32 *outFwStatus,
                  UInt8 *outMailbox = 0, UInt32 outBytes = 0,
                  Cx3QpCaptureObservation *outObservation = 0);

    /* The state nibble of a context's flags word, as a plain value: flags[31:28]. */
    static UInt32 qpState(const UInt8 *context);

    /*
     * mlx4_qp_remove + the 2RST half of mlx4_qp_modify: put a QP back to RST.
     *
     * op_modifier 2 and in_param 0 is the "no mailbox" shape the reference uses for a QP
     * that is being torn down (mlx4_qp.c:145-147).  Needed for the rollback path, because
     * a QP left in RTS holds an MTT pointing at a ring the driver is about to release.
     */
    bool  qpToReset(UInt32 qpn, UInt32 *outFwStatus);

    /*
     * Run one HCR command with parameters the caller chooses.
     *
     * Why this exists: this project's cost is reboots, and the pattern that produced them
     * was "try a command, find out, edit the kext, add a selector, reboot".  A selector per
     * experiment cannot be avoided when the code itself is wrong, but most experiments are
     * not code changes -- they are "what does the firmware do if this field is that value".
     * Those become user-space calls with this.
     *
     * It is gated (cx3rawBootArg() -- see Cx3Debug.h) because it can send ANY command,
     * including ones that make the hardware start referring to host memory.  That is the
     * right capability for developing a driver and the wrong one for a released driver, so
     * it is a switch rather than a compile-time decision: turning it on does not require
     * editing the kext or rebuilding.
     *
     * Every call is logged with its parameters, so "what did I send" is always answerable
     * afterwards -- the same principle as the WQE readback and the MPT entry readback.
     */
    bool  hcrRun(UInt16 op, UInt32 inModifier, UInt8 opMod, UInt32 flags, UInt32 timeoutMs,
                 const UInt8 *inbox, UInt32 inboxLen, UInt8 *outbox, UInt32 outboxLen,
                 UInt32 *outFwStatus, UInt32 *outHcrPost, UInt32 *outHcrFinal,
                 UInt64 *outImm);
    bool  setPortGeneralEth(UInt32 port, UInt16 mtu, bool roceV1V2, UInt32 *outFwStatus);
    /*
     * R4 stage 7: SET_PORT, the qpn_calc sub-command -- mlx4_SET_PORT_qpn_calc
     * (mlx4_port.c:1578-1612).  Under A0 this is how the DRIVER tells the firmware where the
     * MAC-derived QPN range starts; the firmware does not check the value against a region of
     * its own, so the only way to know it was taken is the command's own answer.
     *
     * Every field is a parameter because every one of them is a device answer the caller
     * reads: `baseQpn` is the number this port's RX QP has (or will take), `nMacLog` is
     * QUERY_PORT's log_max_macs, and `mPromisc` comes from the device's VEP_MC_STEER
     * capability bit (MLX4_DEV_CAP_FLAG_VEP_MC_STEER).  Nothing here is a literal except the
     * two VLAN indices, which are the reference's own enumerators
     * (MLX4_NO_VLAN_IDX / MLX4_VLAN_MISS_IDX, device.h:424-425) and NOT both zero.
     */
    bool  setPortQpnCalc(UInt32 port, UInt32 baseQpn, UInt32 promisc, UInt32 nMacLog,
                         UInt32 mPromisc, UInt32 *outFwStatus);
    bool  initPort(UInt32 port, UInt32 *outFwStatus);
    /*
     * SET_MCAST_FLTR -- one entry of the port's multicast filter table.
     *
     * Under A0 steering (which this card is in, verified through QUERY_HCA) this is the WHOLE
     * mechanism for receiving group addresses: mlx4_multicast_attach() is a no-op for Ethernet
     * under A0 (mlx4_mcg.c:1408-1417).  The address is packed big-endian into a 64-bit value --
     * mlx4_mac_to_u64, driver.h:92-100, addr[0] in bits 47..40 -- and `clear` rides in bit 63,
     * so "flush the table and seed it with this one address" is one command, which is exactly
     * how the reference starts (mlx4_en_netdev.c:912-917: broadcast with clear, then enable).
     */
    bool  setMcastFltr(UInt32 port, UInt64 mac, bool clear, UInt32 mode,
                       UInt32 *outFwStatus);
    /*
     * CLOSE_PORT -- stop the port at the device.  The queues are NOT touched (they are the
     * bring-up's objects, and closing a port does not remove a QP), so this is the "stop" half of
     * a configuration change: the other half is the port-up sequence the chain already runs
     * (SET_PORT general with the new MTU, qpn_calc, INIT_PORT).
     */
    bool  closePort(UInt32 port, UInt32 *outFwStatus);
    /* Read back the MTT entries a queue would use (host memory, plain loads). */
    bool   mttReadback(UInt32 offsetEntries, UInt32 n, const Mlx4Icm *icm, UInt64 *out,
                       void *res);
    UInt64 mttEntryValue(UInt32 entryIndex);
    /* mlx4_QUERY_EQ (0x15): the HCA's own view of an EQ.  May be unsupported. */
    bool   queryEq(UInt32 eqn, Mlx4EqContextResult *out, UInt32 *outFwStatus);
    /*
     * mlx4_QUERY_ADAPTER() -- FreeBSD mlx4_fw.c:1789.
     *
     * Needed for exactly one field on this port: inta_pin (outbox 0x10, a u8), which
     * is what the interrupt-clear register's mask and offset are computed from.  The
     * board id comes along for free and is logged.
     */
    bool   queryAdapter(UInt8 *outIntaPin, UInt8 *outBoardId,
                        UInt32 *outFwStatus, UInt8 *outRaw = nullptr);
    bool   readModule(UInt32 port, UInt32 offset, UInt32 length, Cx3ModuleResult *out);
    /*
     * Set up the interrupt-clear register so a legacy interrupt can be acknowledged.
     *
     * mlx4_eq.c:1114-1126 maps it, and :1184-1187 computes the two values that
     * matter:
     *     clr_mask = swab32(1 << (inta_pin & 31));
     *     clr_int  = clr_base + (inta_pin < 32 ? 4 : 0);
     *
     * The 4-byte offset for pins below 32 is the reference's, not a guess -- the
     * register region is 8 bytes (MLX4_CLR_INT_SIZE) and the two halves serve the two
     * pin ranges.
     *
     * The BAR is reached through the mapping this port already holds when it can be
     * (0 and 2), so the common case adds no new mapping and no new failure mode.
     */
    bool   clrIntSetup(UInt8 clrBar, UInt64 clrBase, UInt8 intaPin,
                       UInt32 *outMask, UInt32 *outOffset, UInt32 *outVia);
    /* Write the clear register.  This is the step whose absence wedged the machine. */
    bool   clrIntWrite();
    bool   clrIntReady() const { return fClrIntPtr != 0; }
    void   clrIntTearDown();
    /*
     * The QUERY_FW fields the clear register needs.  Kept in the hardware layer
     * rather than re-read, because QUERY_FW is only sent once per load and its answer
     * is already held -- re-running it would be a second command for no reason.
     */
    bool   clrIntFieldsFromQueryFw(Mlx4QueryFwResult *out) const;
    /* The last QUERY_FW answer, kept so the clear setup does not need to re-send it. */
    void   rememberQueryFw(const Mlx4QueryFwResult *fw);
    /* Write the consumer index (and the "request notification" bit) to the
     * doorbell.  Same arithmetic the reference's eq_set_ci uses. */
    void  eqSetConsumerIndex(UInt64 doorbellAddr, UInt32 consIndex, bool reqNot);
    /*
     * Poll the ring for the next EQE the software owns.
     *
     * Readiness is the reference's expression, not an invention:
     *     ready  <=>  !!(eqe->owner & 0x80) == !!(eq->cons_index & eq->nent)
     * (mlx4_eq.c:121-125 and the diagnostic at :804-808).  The consumer index
     * participates as a phase bit, because the owner bit alternates on every
     * pass over the ring -- so an empty ring does not look ready.
     */
    bool  eqNextEqe(const Mlx4Icm *ring, UInt32 nent, UInt8 eqeFactor,
                    UInt32 consIndex, UInt8 *outEqe32);
    /*
     * mlx4_UNMAP_FA() -- FreeBSD mlx4_fw.c:1585, mlx4_main.c:1497 and :1916.
     * The reference always unmaps the FW area before freeing it; without this
     * the firmware keeps a page list that points at memory the host is about to
     * recycle, which is how a second load can fail differently from the first.
     */
    bool  unmapFa(UInt32 *outFwStatus);

    /*
     * The command mailbox's poison state, and why it is a state rather than a return code.
     *
     * When a command cannot be waited for any longer -- the register never went idle
     * before the command was posted, or it never cleared after -- the honest options are
     * "keep waiting forever" and "stop using this mailbox until the device is reset".
     * The first is what held the transaction lock for 6 min 07.6 s; the second is what
     * mlx4_cmd_reset_flow does in the reference (mlx4_cmd.c:634-638 ->
     * mlx4_catas.c:169-200, with mlx4_internal_err_reset defaulting to 1 in
     * mlx4_catas.c:47), where every later command is refused at the entrance.
     *
     * The reason it has to be a state and not just "return an error this time": once the
     * GO bit is stuck, the mailbox may still be being read, a reply may still arrive, and
     * the only thing standing between a late reply and the *next* command reading it as
     * its own answer is the GO/T-bit check -- which is precisely what is broken.  So the
     * safe direction is to stop posting, and the cost is that the device is unusable
     * until proven recovery. Raw reset success is not that proof.
     */
    bool  cmdWedged() const { return fCmdWedged; }
    UInt16 cmdWedgedOp() const { return fCmdWedgedOp; }
    /*
     * Whether the command that set the poison had been *posted* when it was abandoned.
     * The two cases are not equivalent: "posted, never completed" means the device may have
     * begun acting on that mailbox and may still complete it, while "never went idle" means
     * the command was never handed over -- and the arm of the failure that a reader should
     * distrust is different in each.
     */
    bool  cmdWedgedPosted() const { return fCmdWedgedPosted; }
    UInt32 cmdAbandons() const { return fCmdAbandons; }   /* commands given up on */
    UInt32 cmdRefused() const { return fCmdRefused; }     /* commands refused fast */
    UInt64 cmdWedgedAt() const { return fCmdWedgedAtNs; }
    /*
     * What the register says right now, in the command path's own terms (GO set, or the
     * toggle echo still matching).  Read-only, and exposed for two callers: the fault
     * injector's refusal to un-poison a device that is genuinely busy, and any future
     * diagnostic that needs the same reading.  It is deliberately NOT used by the poison
     * check in cmdBoxLocked, which must not read the register at all.
     */
    bool  cmdRegisterBusy() { return cmdPending(); }
    /*
     * Poison the mailbox.  Called at the first abandonment and at every later one
     * (there should not be a later one: the poison makes commands fail before they can
     * reach a deadline; real recovery needs an epoch credential). Logs with the opcode, the reason,
     * the budget and the time actually spent, because those four together are what makes
     * the log line a measurement rather than an assertion.
     */
    void  cmdPoison(UInt16 op, const char *why, UInt32 budgetMs, UInt64 waitedMs,
                    bool posted);
    /*
     * Poison the mailbox on purpose, for kCx3SelFault.  Separate from cmdPoison() because
     * the abandonment counter has to keep meaning "commands this driver gave up on": an
     * injection is not an abandonment, and a counter that mixed the two would make an
     * injected run look exactly like a real one in the numbers.
     */
    void  cmdPoisonInjected();
    /*
     * Revoke only a synthetic injection (never a real abandonment), and only
     * while its mailbox has not been retained. This is not a reset credential.
     */
    void  cmdClearWedged(const char *why);

    /* Read-only views of an ICM area, for diagnostics. */
    void  icmSum(const Mlx4Icm *icm, Mlx4IcmSum *out);
    bool  icmDump(const Mlx4Icm *icm, UInt32 byteOffset, UInt32 count, UInt32 *out);
    /* Write a recognisable pattern into an ICM area.  Host memory only. */
    void  icmFill(Mlx4Icm *icm);

private:
    bool  mboxAlloc();
    void  mboxFree();
    bool  cmdPending();
    /*
     * cmdBox() plus the immediate output parameter.
     *
     * After a command completes, the HCR's out_param dwords hold whatever the
     * firmware left there, and some commands return their answer that way
     * instead of through a mailbox (mlx4_cmd_poll's out_is_imm branch,
     * mlx4_cmd.c:640-645).  SET_ICM_SIZE is one of them: it returns the number
     * of auxiliary pages in the register, not in memory, so without this the
     * answer is unreachable.
     */
    bool  cmdBoxImm(UInt64 inParam, UInt64 inModifier, UInt8 opMod, UInt16 op,
                    UInt32 timeoutMs, UInt64 *outImm, UInt32 *outFwStatus,
                    bool bypassBusy);
    bool  cmdBox(UInt64 inParam, UInt64 outParam, UInt32 inModifier, UInt8 opMod,
                 UInt16 op, UInt32 timeoutMs, UInt32 *outFwStatus,
                 bool bypassBusy = false, Cx3CommandObservation *outObservation = 0);
    /* A pointer in slot 8 must never silently become bypassBusy=true. */
    template <typename T>
    bool cmdBox(UInt64, UInt64, UInt32, UInt8, UInt16, UInt32, UInt32 *, T *) = delete;
    bool  cmdBoxLocked(UInt64 inParam, UInt64 outParam, UInt32 inModifier,
                       UInt8 opMod, UInt16 op, UInt32 timeoutMs, UInt32 *outFwStatus,
                       bool bypassBusy, UInt64 *outImm, UInt16 eventToken,
                       bool useEvent, Cx3CommandObservation *outObservation = 0);

    IOPCIDevice *             fPci;
    IOService *               fClient;
    IOMemoryMap *             fMap0;
    IOMemoryMap *             fMap2;
    volatile UInt32 *         fReg0;
    volatile UInt32 *         fReg2;
    volatile UInt32 *         fHcr;      /* fReg0 + 0x80680 */
    volatile UInt32 *         fReset;    /* fReg0 + 0xf0000 */
    UInt32                    fToggle;   /* HCR toggle bit, starts at 1 */
    UInt64                    fDeviceGeneration; /* zero means identity/generation not bound */
    UInt64                    fIcmAllocationSequence; /* never reset across HCA generations */
    IOBufferMemoryDescriptor *fMboxMd;
    IODMACommand *            fMboxCmd;
    UInt64                    fMboxPhys;
    UInt64                    fMboxOffset;
    UInt32                    fMboxSegCount;
    void *                    fMboxVirt;
    bool                      fMboxRetained; /* sticky until reboot; poison clearing is not release proof */
    Mlx4Icm *                 fModuleInbox; /* separate MAD inbox; same poison lifetime as command outbox */
    /*
     * The reference serialises the command register: cmd_poll() takes poll_sem
     * and cmd_post() takes the device state mutex (mlx4_cmd.c:441, :592).  This
     * port shares one mailbox and one toggle across every caller, and more than
     * one user client can exist, so without a lock two commands can interleave
     * their HCR stores and their mailbox writes.  Allocated lazily so that the
     * read-only boot path never allocates anything.
     */
    IOLock *                  fCmdLock;
    /*
     * The mailbox poison state (see cmdWedged() above).  All of it is initialised in the
     * constructor's list, longhand, for the reason recorded there: this class comes from
     * an allocator that does not promise zeroed memory, and fCmdWedged is tested before
     * anything assigns it.
     */
    bool                      fCmdWedged;    /* stop touching the HCR and the mailbox */
    UInt16                    fCmdWedgedOp;  /* the opcode that was abandoned */
    bool                      fCmdWedgedPosted; /* was it posted when abandoned? */
    bool                      fCmdSyntheticPoison; /* explicit injection provenance, not opcode */
    UInt64                    fCmdWedgedAtNs;/* when it was abandoned (uptime) */
    UInt32                    fCmdAbandons;  /* how many commands were given up on */
    UInt32                    fCmdRefused;   /* how many were refused while poisoned */
    /*
     * MTT table view for the queue code: host base of the mapped table, its
     * byte length, the reserved prefix (which must not be handed out), and a
     * bump pointer for the rest.
     */
    void *                    fMttBase;
    UInt64                    fMttBytes;
    /*
     * The MTT table's HCA virtual base, as announced by INIT_HCA.  It is needed
     * only for the experiment below: whether a queue context's mtt_base_addr is a
     * byte offset inside the table (what the reference passes) or an absolute
     * address in the HCA's context-memory space.  The reference's own arithmetic
     * (mlx4_mr.c:266-268 returns offset * entry_size, a small number) says
     * "offset", but /tmp/.../mlx4_buddy.c -- the file that would settle what
     * mtt->offset actually counts in -- is NOT in the vendored 41 files, so the
     * units are not derivable here and are measured instead.
     */
    UInt64                    fMttHcaVirt;
    UInt32                    fMttReserved;
    /* PD allocation state: the firmware's share, and the next number to hand out. */
    UInt32                    fPdReserved;
    UInt32                    fPdNext;
    /* The dMPT table's host view, for reading MPT entries back. */
    void *                    fMptBase;
    UInt64                    fMptBytes;
    UInt32                    fMptObjSize;
    Mlx4MttAllocator          fMttAllocator;
    Mlx4AbRingRetention       fAbRetainedRings[2];
    /*
     * The interrupt-clear register: where it is, what to write, and (when it needed
     * its own mapping) the mapping to release.
     */
    volatile UInt32 *         fClrIntPtr;
    Mlx4QueryFwResult         fLastFw;
    bool                      fHaveLastFw;
    UInt32                    fClrMask;
    UInt32                    fClrOffset;
    IOMemoryMap *             fClrMap;
};

#endif /* CX3_MLX4_HW_H */
