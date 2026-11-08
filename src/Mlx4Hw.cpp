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
 * Mlx4Hw implementation.  Derived from vendor/freebsd-src (OpenIB BSD).
 * Each routine names the FreeBSD function it mirrors so the port stays
 * diffable against the original.
 */
#include "Mlx4Hw.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <libkern/OSByteOrder.h>
/*
 * clock_get_uptime, for the one thing in this file that needs real elapsed time
 * rather than a loop count: the A/B ring experiment's sample clock.  Declared in
 * kern/clock.h; the build's symbol accounting is what proves it is actually in the
 * kernel's exported set rather than merely declared in a header.
 */
#include <kern/clock.h>

#include "Cx3Debug.h"   /* LOG / LOGBOOT / boot-arg gates */
/*
 * The command gate: the decision that ends a wait, as a pure function, so that
 * tools/test-cmd-wedge.c can assert its four states on every build instead of leaving them
 * to be discovered on hardware.  See the header for why the states must not be conflated.
 */
#include "cx3_cmdgate.h"

/* ---- big-endian field helpers (FreeBSD MLX4_GET / MLX4_PUT) -------- */
static inline UInt16 mlx4GetBe16(const void *p)
{
    return OSSwapBigToHostInt16(*(const UInt16 *)p);
}
static inline UInt32 mlx4GetBe32(const void *p)
{
    return OSSwapBigToHostInt32(*(const UInt32 *)p);
}
static inline UInt64 mlx4GetBe64(const void *p)
{
    return OSSwapBigToHostInt64(*(const UInt64 *)p);
}
static inline void mlx4PutBe64(void *p, UInt64 v)
{
    *(UInt64 *)p = OSSwapHostToBigInt64(v);
}

/*
 * mlx4Wmb -- the write barrier this file used to believe OSSynchronizeIO was.
 *
 * The reference and every other implementation put a `wmb()` at these same points, and this
 * port's comments said so in as many words ("Linux wmb() / mmiowb()").  What was actually
 * called could not be one:
 *
 *     MacOSX.sdk/System/Library/Frameworks/Kernel.framework/Headers/libkern/OSAtomic.h:549-556
 *
 *         #if defined(__arm__) || defined(__arm64__)
 *         extern void OSSynchronizeIO(void);
 *         #else
 *         static __inline__ void
 *         OSSynchronizeIO(void)
 *         {
 *         }
 *         #endif
 *
 * This port is built x86_64 only (build.sh: -arch x86_64), so EVERY call compiled to nothing
 * -- not a barrier, not even a compiler barrier.  The declaration's own doc comment ("ensures
 * orderly load and store operations to noncached memory mapped I/O devices ... executes the
 * eieio instruction on PowerPC") is about the architectures that have a real definition; on
 * the architecture this driver runs on there is none.
 *
 * Why this is not merely cosmetic, and why it is not the whole story either: on x86-64 the TSO
 * memory model orders ordinary stores against later stores, so a WB descriptor write followed
 * by a UC doorbell write cannot be reordered by the hardware, and the compiler very nearly
 * cannot reorder them across an opaque call either.  That is the honest reason the missing
 * barrier is UNLIKELY to be what stops the send path -- it is recorded as its own defect
 * because the code claimed a guarantee it did not have, which is how a later edit (a compiler
 * that inlines harder, or a store the compiler can prove is dead) becomes a real failure with
 * no line to point at.
 *
 * The barrier is `__sync_synchronize()` -- a full fence, the strongest of the three candidate
 * alignments and the one MFENCE gives on this architecture.  Full rather than the weaker
 * "release" form on purpose: the sites below bracket MMIO against ordinary memory, and the
 * cost of an MFENCE at a handful of queue-post sites is not what this driver's latency is made
 * of.
 */
static inline void mlx4Wmb(void)
{
    __sync_synchronize();
}

/*
 * Defined further down, in the EQ section, where its four-snapshot purpose is explained.
 * Declared here because the CQ path also needs it: "the ring holds 0xcc" needs the same
 * before/after treatment on either kind of queue.
 */
static void mlx4DumpBytes(const char *label, const UInt8 *p, UInt32 n);

Mlx4Hw::Mlx4Hw()
    : fPci(0), fClient(0), fMap0(0), fMap2(0), fReg0(0), fReg2(0),
      fHcr(0), fReset(0), fToggle(1), fDeviceGeneration(0), fIcmAllocationSequence(0),
      fMboxMd(0), fMboxCmd(0),
      fMboxPhys(0), fMboxOffset(0), fMboxSegCount(0), fMboxVirt(0), fMboxRetained(false), fModuleInbox(0), fCmdLock(0),
      /*
       * The poison state, initialised here for the reason this list exists at all: the
       * object comes from an allocator that does not promise zeroed memory, and
       * fCmdWedged is read by the first command that runs.
       */
      fCmdWedged(false), fCmdWedgedOp(0), fCmdWedgedPosted(false), fCmdSyntheticPoison(false), fCmdWedgedAtNs(0),
      fCmdAbandons(0), fCmdRefused(0),
      fMttBase(0), fMttBytes(0), fMttHcaVirt(0), fMttReserved(0),
      /*
       * Every member, and this line is a fix rather than housekeeping.
       *
       * The R4 additions were declared after this list and left out of it, so fPdNext
       * was read by setPdRange() before anything wrote it, and fMptBase was tested
       * against 0 by mptEntryRead() and then used as a pointer.  The object comes from
       * an allocator that does not promise zeroed memory, so "the constructor happens
       * to give me zeros" was never a safe assumption -- and a wild fMptBase is a
       * kernel read of an arbitrary address.
       */
      fPdReserved(0), fPdNext(0),
      fMptBase(0), fMptBytes(0), fMptObjSize(0)
{
}

bool Mlx4Hw::init(IOPCIDevice *pci, IOService *client)
{
    fPci    = pci;
    fClient = client;

    /* Allocate once before the provider can publish command entry points.
     * A host lock is not a DMA allocation or a device write. Command paths
     * never lazily publish/replace this shared pointer. */
    if (!fCmdLock) fCmdLock = IOLockAlloc();
    if (!fCmdLock) {
        LOG("command lock allocation failed; hardware layer not initialized");
        return false;
    }

    /* FreeBSD reaches these with ioremap(pci_resource_start(pdev, 0) + off). */
    fMap0 = fPci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapAnywhere);
    if (!fMap0) {
        LOG("mapDeviceMemoryWithRegister(BAR0) failed");
        IOLockFree(fCmdLock); fCmdLock = 0;
        return false;
    }
    fReg0 = (volatile UInt32 *)fMap0->getVirtualAddress();
    if (!fReg0) {
        LOG("BAR0 virtual address is NULL");
        fMap0->release(); fMap0 = 0;
        IOLockFree(fCmdLock); fCmdLock = 0;
        return false;
    }

    fMap2 = fPci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2, kIOMapAnywhere);
    fReg2 = fMap2 ? (volatile UInt32 *)fMap2->getVirtualAddress() : 0;

    fHcr    = fReg0 + (MLX4_HCR_BASE   / 4);
    fReset  = fReg0 + (MLX4_RESET_BASE / 4);
    fToggle = 1;                        /* mlx4_cmd_init(): cmd.toggle = 1 */
    return true;
}

bool Mlx4Hw::mailboxNeedsRetention() const
{
    return fMboxRetained || (fCmdWedged && (fMboxMd || fMboxCmd || fModuleInbox));
}

void Mlx4Hw::shutdown()
{
    Mlx4MailboxDisposition mailbox;
    mailbox.init(mailboxNeedsRetention());
    if (mailbox.release()) {
        mboxFree();
    } else if (mailbox.retain()) {
        fMboxRetained = true;
        /*
         * A poisoned command may still read this inbox or DMA its answer into the outbox
         * after stop() returns.  Releasing the descriptor would release the IOMMU mapping
         * underneath the device.  Keep both objects (and therefore the pages) retained for
         * this boot; publication arrives only after the body below has cleared their raw
         * view.
         */
        LOG("mailbox RETAINED for reboot: command op 0x%x was abandoned (posted=%u), so "
            "the device may still touch virt %p phys 0x%llx; not freeing its mapping",
            (unsigned)fCmdWedgedOp, (unsigned)fCmdWedgedPosted, fMboxVirt,
            (unsigned long long)fMboxPhys);
        fMboxVirt = 0;
        fMboxPhys = 0;
        fMboxOffset = 0;
        fMboxSegCount = 0;
    }
    if (fCmdLock) { IOLockFree(fCmdLock); fCmdLock = 0; }
    if (fMap2) { fMap2->release(); fMap2 = 0; }
    if (fMap0) { fMap0->release(); fMap0 = 0; }
    fReg0 = 0; fReg2 = 0; fHcr = 0; fReset = 0;
}

bool Mlx4Hw::markDmaHandoff(Mlx4Icm *const *blocks, UInt32 count)
{
    UInt32 i;
    if (!blocks || count == 0 || !fDeviceGeneration) return false;
    /* Validate the whole set first, so a malformed later entry cannot leave a partial stamp. */
    for (i = 0; i < count; ++i) {
        if (!blocks[i] || !blocks[i]->allocationId ||
            !blocks[i]->generation.validAllocation() ||
            !fMttAllocator.canMarkOwnerHandoff(blocks[i], blocks[i]->allocationId,
                                               blocks[i]->generation.allocationGeneration))
            return false;
    }
    for (i = 0; i < count; ++i) {
        if (!blocks[i]->generation.markHandoffAttempted(fDeviceGeneration)) return false;
        if (!fMttAllocator.markOwnerHandoff(blocks[i], blocks[i]->allocationId,
                                            fDeviceGeneration)) return false;
    }
    return true;
}

bool Mlx4Hw::markResourceHandoff(Mlx4DmaGeneration *owner)
{
    if (!owner || !owner->validAllocation() || !fDeviceGeneration) return false;
    return owner->markHandoffAttempted(fDeviceGeneration);
}

/* ------------------------------------------------------------------ */
/* read-only probes                                                    */
/* ------------------------------------------------------------------ */

UInt32 Mlx4Hw::readRegRaw(UInt32 off)
{
    if (!fReg0) return 0xffffffffu;
    return fReg0[off / 4];
}

UInt32 Mlx4Hw::readRegWarm(UInt32 off)
{
    UInt32 a, b, c;

    if (!fReg0) return 0xffffffffu;

    a = fReg0[off / 4];     /* may be the cold-page artifact: discard */
    b = fReg0[off / 4];
    if (a == b) return b;

    /* Still settling (or a genuinely live register that just changed); take a
     * third sample so the caller sees a value that is at least stable across two
     * consecutive reads. */
    c = fReg0[off / 4];
    return c;
}

void Mlx4Hw::warmBar0()
{
    UInt32 off;
    UInt32 touched = 0;

    if (!fReg0) return;
    for (off = 0; off < 0x100000u; off += 0x1000u) {
        (void)fReg0[off / 4];
        touched++;
    }
    LOG("warmBar0: touched %u pages of BAR0", (unsigned)touched);
}

void Mlx4Hw::diagnoseFirstRead()
{
    struct { const char *name; UInt32 off; } probe[] = {
        { "owner(0x8069c)",      MLX4_OWNER_BASE },
        { "hcrstatus(0x80698)",  MLX4_HCR_BASE + HCR_STATUS_OFFSET },
        { "resetsem(0xf03fc)",   MLX4_RESET_BASE + MLX4_SEM_OFFSET },
    };
    unsigned i;

    if (!fReg0 || !fHcr) { LOG("diagnoseFirstRead: BAR0 not mapped"); return; }

    for (i = 0; i < sizeof(probe) / sizeof(probe[0]); ++i) {
        UInt32 a = readRegRaw(probe[i].off);
        UInt32 b = readRegRaw(probe[i].off);
        UInt32 c = readRegRaw(probe[i].off);
        LOG("cold-read probe %-18s 1st=0x%08x 2nd=0x%08x 3rd=0x%08x%s",
            probe[i].name, (unsigned)a, (unsigned)b, (unsigned)c,
            (a != b) ? "   <- first sample differed, confirming the artifact"
                     : "");
    }
}

/*
 * Mirrors mlx4_get_ownership(): map, read once, unmap.
 *
 * Note that this deliberately does NOT reuse fReg0.  The reference maps a
 * throwaway window each time it makes this check, and on this hardware the first
 * read through a fresh window returns 0, which is what makes the check pass.  A
 * read through an already-used mapping returns 0x01000000, which would fail it.
 * Matching the reference's access pattern is the point.
 */
UInt32 Mlx4Hw::readBar0Fresh(UInt32 off)
{
    IOMemoryMap *tmp;
    UInt32       v = 0xffffffffu;

    if (!fPci) return v;

    tmp = fPci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapAnywhere);
    if (!tmp) {
        LOG("readBar0Fresh(0x%05x): could not create a mapping", (unsigned)off);
        return v;
    }
    {
        volatile UInt32 *base = (volatile UInt32 *)tmp->getVirtualAddress();
        if (base) v = base[off / 4];      /* one read, exactly as readl() does */
    }
    tmp->release();
    return v;
}

UInt32 Mlx4Hw::checkOwnershipAsReference()
{
    return readBar0Fresh(MLX4_OWNER_BASE);
}

UInt32 Mlx4Hw::readOwnerWordRaw()
{
    /* One read, no retry: byte-for-byte the reference's behaviour. */
    return readRegRaw(MLX4_OWNER_BASE);
}

UInt32 Mlx4Hw::readOwnerWord()
{
    return readRegWarm(MLX4_OWNER_BASE);
}

UInt32 Mlx4Hw::readHcrStatus()
{
    if (!fHcr) return 0xffffffffu;
    return OSSwapBigToHostInt32(readRegWarm(MLX4_HCR_BASE + HCR_STATUS_OFFSET));
}

UInt32 Mlx4Hw::readResetSem()
{
    return readRegWarm(MLX4_RESET_BASE + MLX4_SEM_OFFSET);
}

void Mlx4Hw::dump(const char *tag, UInt32 regOff, UInt32 nDwords)
{
    UInt32 i;
    if (!fReg0) { LOG("%s: BAR0 not mapped", tag); return; }
    for (i = 0; i + 4 <= nDwords; i += 4) {
        UInt32 o = regOff + i * 4;
        LOG("%s+0x%05x = %08x %08x %08x %08x", tag,
            (unsigned)o,
            (unsigned)readRegWarm(o + 0), (unsigned)readRegWarm(o + 4),
            (unsigned)readRegWarm(o + 8), (unsigned)readRegWarm(o + 12));
    }
}

/*
 * Reads four independent, unrelated registers.  A disabled memory window forces
 * all of them to 0xffffffff; a live window will not agree on that value, even if
 * the HCA happens to be sitting in reset.
 */
bool Mlx4Hw::windowAlive()
{
    if (!fReg0 || !fHcr) return false;
    /*
     * Read order matters here, and not for the usual reason.
     *
     * The last test reads BAR0 0xf03fc, and a read of that word between the HCA
     * reset and RUN_FW stops RUN_FW from ever completing (measured; see
     * docs/M2B-NOTES.md section 9).  The HCR word comes first, so in every
     * working state this returns before the semaphore is touched -- which is the
     * only reason fwload's own window check does not break the load it guards.
     * Do not reorder these tests, and do not add a window check anywhere between
     * the reset and RUN_FW.
     */
    if (fHcr[0]                      != 0xffffffffu) return true;
    if (fHcr[1]                      != 0xffffffffu) return true;
    if (fReg0[MLX4_OWNER_BASE / 4]   != 0xffffffffu) return true;
    if (fReset[MLX4_SEM_OFFSET / 4]  != 0xffffffffu) return true;
    return false;
}

UInt64 Mlx4Hw::bar0Physical() const
{
    return fMap0 ? (UInt64)fMap0->getPhysicalAddress() : 0;
}

UInt32 Mlx4Hw::readBar2(UInt32 off)
{
    UInt32 a, b, c;

    if (!fReg2) return 0xffffffffu;

    /* Same cold-read handling as readRegWarm, for BAR2. */
    a = fReg2[off / 4];
    b = fReg2[off / 4];
    if (a == b) return b;
    c = fReg2[off / 4];
    return c;
}

void Mlx4Hw::writeBar0(UInt32 off, UInt32 value)
{
    if (!fReg0) return;
    fReg0[off / 4] = value;
    mlx4Wmb();
}

void Mlx4Hw::writeBar2(UInt32 off, UInt32 value)
{
    if (!fReg2) return;
    fReg2[off / 4] = value;
    mlx4Wmb();
}

/* Linux pci_pcie_cap(): walk the capability list looking for id 0x10. */
UInt8 Mlx4Hw::findPcieCapability()
{
    UInt8 cap;
    int   guard;

    if (!fPci) return 0;
    cap = (UInt8)(fPci->configRead32(0x34) & 0xfc);
    for (guard = 0; cap && guard < 48; ++guard) {
        UInt16 hdr = fPci->configRead16(cap);
        if ((hdr & 0xff) == PCI_CAP_ID_EXP) return cap;
        cap = (UInt8)((hdr >> 8) & 0xfc);
    }
    return 0;
}

UInt32 Mlx4Hw::configRead(UInt8 off)
{
    return fPci ? fPci->configRead32(off) : 0xffffffffu;
}

void Mlx4Hw::configWrite(UInt8 off, UInt32 value)
{
    if (fPci) fPci->configWrite32(off, value);
}

/*
 * mlx4_free_ownership() -- the reference's own "give the device back" store.
 * Recorded values before and after so the effect is visible rather than assumed.
 */
bool Mlx4Hw::clearOwner()
{
    if (!fReg0) return false;
    fReg0[MLX4_OWNER_BASE / 4] = 0;
    IOSleep(1000);                      /* mlx4_reset.c-style settle */
    return true;
}

/* ------------------------------------------------------------------ */
/* HCA reset -- mirrors FreeBSD mlx4_reset() (mlx4_reset.c:41)         */
/* ------------------------------------------------------------------ */

bool Mlx4Hw::resetHca(bool bypassSemaphore)
{
    UInt32 saved[64];
    UInt8  pcieCap = 0;
    UInt16 devctl = 0, linkctl = 0, vendor = 0xffff;
    UInt32 sem = 0;
    int    i;

    pcieCap = findPcieCapability();

    /* Save the PCI header; config offsets 0x58/0x5c (dwords 22/23) are
     * skipped because they have a special meaning (mlx4_reset.c:76-88). */
    for (i = 0; i < 64; ++i) {
        if (i == 22 || i == 23) { saved[i] = 0; continue; }
        saved[i] = fPci->configRead32(i * 4);
    }

    /*
     * Grab the HW semaphore to lock out flash updates (mlx4_reset.c:104).
     *
     * Read it the way the reference does -- through a freshly created mapping,
     * once per attempt.  The reference ioremaps a throwaway window for this and
     * calls readl() on it, so its first read is the first read through a new
     * mapping.  On this hardware that read returns 0, which is why the reference
     * proceeds.  Reading it through the persistent mapping instead returns
     * 0x01000000 and this would refuse, leaving the HCA unreset -- which is
     * exactly the difference that has kept MAP_FA/RUN_FW from matching the
     * reference's behaviour.
     */
    if (bypassSemaphore) {
        LOG("reset: semaphore gate bypassed on request (the register reads a "
            "constant 0x%08x and ignores writes, so it gates nothing)",
            (unsigned)readResetSem());
    } else {
        for (i = 0; i < (int)MLX4_SEM_TIMEOUT_MS; ++i) {
            sem = readBar0Fresh(MLX4_RESET_BASE + MLX4_SEM_OFFSET);
            if (!sem) break;
            IOSleep(1);
        }
        LOG("reset: semaphore after waiting = 0x%08x (settled value through the "
            "persistent mapping is 0x%08x)", (unsigned)sem, (unsigned)readResetSem());
        if (sem) {
            LOG("reset: HW semaphore busy (0x%08x), aborting", (unsigned)sem);
            return false;
        }
    }

    /* Actually hit reset (mlx4_reset.c:120).  After this write BAR0 must not be
     * touched until the PCI header has been restored below. */
    fReset[MLX4_RESET_OFFSET / 4] = MLX4_RESET_VALUE;
    IOSleep(1000);                      /* "one second before accessing device" */

    /* Wait for config space to come back (mlx4_reset.c:126). */
    for (i = 0; i < (int)MLX4_RESET_TIMEOUT_MS; ++i) {
        vendor = fPci->configRead16(kIOPCIConfigVendorID);
        if (vendor != 0xffff) break;
        IOSleep(1);
    }
    if (vendor == 0xffff) {
        LOG("reset: device did not come back from reset, aborting");
        return false;
    }

    /* Restore PCIe devctl/linkctl, then dwords 0..15 with COMMAND last
     * (mlx4_reset.c:143-172). */
    if (pcieCap) {
        devctl  = (UInt16)(saved[(pcieCap + PCI_EXP_DEVCTL) / 4] & 0xffff);
        linkctl = (UInt16)(saved[(pcieCap + PCI_EXP_LNKCTL) / 4] & 0xffff);
        fPci->configWrite16(pcieCap + PCI_EXP_DEVCTL,  devctl);
        fPci->configWrite16(pcieCap + PCI_EXP_LNKCTL, linkctl);
    }
    for (i = 0; i < 16; ++i) {
        if (i * 4 == kIOPCIConfigCommand) continue;
        fPci->configWrite32(i * 4, saved[i]);
    }
    fPci->configWrite32(kIOPCIConfigCommand, saved[kIOPCIConfigCommand / 4]);

    LOG("reset: done (pcie cap 0x%02x, devctl 0x%04x, linkctl 0x%04x)",
        (unsigned)pcieCap, (unsigned)devctl, (unsigned)linkctl);
    fToggle = 1;

    /* Restored configuration and a fresh toggle are not an epoch/scope DMA
     * credential. Do not make a poisoned mailbox reusable here. Clean initial
     * startup remains clean; recovery of a real abandonment belongs to R06. */
    return true;
}

IOReturn Mlx4Hw::platformFunctionReset()
{
    if (!fPci) {
        return kIOReturnNotReady;
    }
    return fPci->reset(kIOPCIDeviceResetTypeFunctionReset, kIOPCIDeviceResetOptionNone);
}

IOReturn Mlx4Hw::readPciIdentity(Mlx4PciIdentity *out)
{
    if (!out) return kIOReturnBadArgument;
    out->clear();
    if (!fPci) return kIOReturnNotReady;

    Mlx4PciIdentity identity;
    identity.vendor = fPci->configRead16(0x00);
    identity.device = fPci->configRead16(0x02);
    identity.subsystemVendor = fPci->configRead16(0x2c);
    identity.subsystemId = fPci->configRead16(0x2e);
    identity.bus = fPci->getBusNumber();
    identity.deviceNumber = fPci->getDeviceNumber();
    identity.function = fPci->getFunctionNumber();
    if (!identity.valid()) return kIOReturnNotReady;

    *out = identity;
    return kIOReturnSuccess;
}

/*
 * A single bounded PCI configuration sample.  Every first observation is copied
 * into the ABI byte image; the consistency pass compares later values without
 * replacing those bytes.  IOPCIDevice's config reads have no error return, so
 * all-ones is treated as unavailable and terminates the pass.
 */
IOReturn Mlx4Hw::queryPcieLink(Cx3PcieLinkResult *out)
{
    if (!out) return kIOReturnBadArgument;
    const UInt64 generation = out->deviceGeneration;
    memset(out, 0, sizeof(*out));
    out->version = CX3_PCIE_RESULT_VERSION;
    out->structBytes = sizeof(*out);
    out->deviceGeneration = generation;
    out->status = (UInt64)kIOReturnSuccess;
    out->reason = CX3_PCIE_REASON_OK;
    if (!fPci) {
        out->reason = CX3_PCIE_REASON_NOT_READY;
        out->status = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }

    bool sampled[256] = {};
    auto save = [&](UInt32 off, UInt32 bytes, UInt32 value) {
        for (UInt32 i = 0; i < bytes; ++i) {
            UInt8 b = (UInt8)(value >> (8 * i));
            if (!sampled[off + i]) {
                out->raw[off + i] = b;
                out->readValid[(off + i) / 8] |= (UInt8)(1u << ((off + i) % 8));
                sampled[off + i] = true;
            }
        }
    };
    auto read8 = [&](UInt32 off, UInt8 *v) {
        if (off > 0xff) return false;
        UInt8 x = fPci->configRead8((IOByteCount)off);
        save(off, 1, x);
        if (x == 0xff) return false;
        *v = x; return true;
    };
    auto read16 = [&](UInt32 off, UInt16 *v) {
        if ((off & 1) || off + 1 > 0xff) return false;
        UInt16 x = fPci->configRead16((IOByteCount)off);
        save(off, 2, x);
        if (x == 0xffff) return false;
        *v = x; return true;
    };
    auto read32 = [&](UInt32 off, UInt32 *v) {
        if ((off & 3) || off + 3 > 0xff) return false;
        UInt32 x = fPci->configRead32((IOByteCount)off);
        save(off, 4, x);
        if (x == 0xffffffffu) return false;
        *v = x; return true;
    };
    auto fail = [&](UInt32 reason, IOReturn status) {
        out->reason = reason; out->status = (UInt64)status; return kIOReturnSuccess;
    };

    UInt16 vendor, device, subven, subid;
    if (!read16(0, &vendor) || !read16(2, &device) || !read16(0x2c, &subven) ||
        !read16(0x2e, &subid)) return fail(CX3_PCIE_REASON_READ_UNAVAILABLE, kIOReturnOffline);
    out->identityBefore.vendor = vendor; out->identityBefore.device = device;
    out->identityBefore.subsystemVendor = subven; out->identityBefore.subsystemId = subid;
    out->identityBefore.bus = fPci->getBusNumber();
    out->identityBefore.deviceNumber = fPci->getDeviceNumber();
    out->identityBefore.function = fPci->getFunctionNumber();
    out->validity |= CX3_PCIE_VALID_IDENTITY_BEFORE;

    UInt8 header;
    UInt16 pciStatus;
    if (!read8(0x0e, &header) || !read16(0x06, &pciStatus))
        return fail(CX3_PCIE_REASON_READ_UNAVAILABLE, kIOReturnOffline);
    if ((header & 0x7f) != 0)
        return fail(CX3_PCIE_REASON_UNSUPPORTED_HEADER, kIOReturnUnsupported);
    if (!(pciStatus & (1u << 4)))
        return fail(CX3_PCIE_REASON_NO_PCIE_CAP, kIOReturnNotFound);
    UInt8 firstPointer;
    if (!read8(0x34, &firstPointer))
        return fail(CX3_PCIE_REASON_READ_UNAVAILABLE, kIOReturnOffline);
    if (firstPointer == 0) return fail(CX3_PCIE_REASON_NO_PCIE_CAP, kIOReturnNotFound);

    bool visited[256] = {};
    UInt8 nodes[48]; UInt16 nodeHeaders[48];
    UInt32 nodeCount = 0; UInt8 cap = firstPointer; UInt16 capHeader = 0;
    for (;;) {
        if (cap < 0x40 || cap > 0xfc || (cap & 3))
            return fail(CX3_PCIE_REASON_MALFORMED_LIST, kIOReturnBadArgument);
        if (visited[cap]) return fail(CX3_PCIE_REASON_CYCLE, kIOReturnBadArgument);
        if (nodeCount >= 48) return fail(CX3_PCIE_REASON_MALFORMED_LIST, kIOReturnNoResources);
        visited[cap] = true; nodes[nodeCount] = cap;
        if (!read16(cap, &capHeader))
            return fail(CX3_PCIE_REASON_READ_UNAVAILABLE, kIOReturnOffline);
        nodeHeaders[nodeCount] = capHeader;
        ++nodeCount; out->walkCount = nodeCount;
        UInt8 next = (UInt8)(capHeader >> 8);
        if (next && (next < 0x40 || next > 0xfc || (next & 3)))
            return fail(CX3_PCIE_REASON_MALFORMED_LIST, kIOReturnBadArgument);
        if (next && visited[next]) return fail(CX3_PCIE_REASON_CYCLE, kIOReturnBadArgument);
        if ((capHeader & 0xff) == 0x10) { out->capOffset = cap; break; }
        if (!next) {
            /* A well-formed non-PCIe chain still gets a bounded final check. */
            UInt16 av, ad, asv, asi;
            if (!read16(0, &av) || !read16(2, &ad) || !read16(0x2c, &asv) ||
                !read16(0x2e, &asi))
                return fail(CX3_PCIE_REASON_INCONSISTENT, kIOReturnOffline);
            out->identityAfter.vendor = av; out->identityAfter.device = ad;
            out->identityAfter.subsystemVendor = asv; out->identityAfter.subsystemId = asi;
            out->identityAfter.bus = fPci->getBusNumber();
            out->identityAfter.deviceNumber = fPci->getDeviceNumber();
            out->identityAfter.function = fPci->getFunctionNumber();
            out->validity |= CX3_PCIE_VALID_IDENTITY_AFTER;
            UInt8 ha, pa; UInt16 sa;
            if (memcmp(&out->identityBefore, &out->identityAfter, sizeof(out->identityBefore)) ||
                !read8(0x0e, &ha) || !read16(6, &sa) || !read8(0x34, &pa) ||
                (ha & 0x7f) != (header & 0x7f) ||
                !!(sa & (1u << 4)) != !!(pciStatus & (1u << 4)) || pa != firstPointer)
                return fail(CX3_PCIE_REASON_INCONSISTENT, kIOReturnBusy);
            for (UInt32 i = 0; i < nodeCount; ++i) {
                UInt16 again;
                if (!read16(nodes[i], &again) || again != nodeHeaders[i])
                    return fail(CX3_PCIE_REASON_INCONSISTENT, kIOReturnBusy);
            }
            out->validity |= CX3_PCIE_VALID_CONSISTENT;
            return fail(CX3_PCIE_REASON_NO_PCIE_CAP, kIOReturnNotFound);
        }
        cap = next;
    }

    UInt16 flags;
    if ((UInt32)cap + 3 > 0xff || !read16((UInt32)cap + 2, &flags))
        return fail((UInt32)cap + 3 > 0xff ? CX3_PCIE_REASON_TRUNCATED_CAP :
                    CX3_PCIE_REASON_READ_UNAVAILABLE,
                    (UInt32)cap + 3 > 0xff ? kIOReturnBadArgument : kIOReturnOffline);
    out->pcieFlags = flags;
    UInt32 version = flags & 0xf, portType = (flags >> 4) & 0xf;
    if ((version != 1 && version != 2) || (portType != 0 && portType != 1))
        return fail(CX3_PCIE_REASON_UNSUPPORTED_PCIE, kIOReturnUnsupported);
    if ((UInt32)cap + 0x0f > 0xff || (UInt32)cap + 0x13 > 0xff)
        return fail(CX3_PCIE_REASON_TRUNCATED_CAP, kIOReturnBadArgument);
    UInt16 initialLinkStatus;
    if (!read32((UInt32)cap + 0x0c, &out->lnkCap) ||
        !read16((UInt32)cap + 0x12, &initialLinkStatus))
        return fail(CX3_PCIE_REASON_READ_UNAVAILABLE, kIOReturnOffline);
    out->lnkSta = initialLinkStatus;
    if (version == 2) {
        if ((UInt32)cap + 0x2f > 0xff)
            return fail(CX3_PCIE_REASON_TRUNCATED_CAP, kIOReturnBadArgument);
        if (!read32((UInt32)cap + 0x2c, &out->lnkCap2))
            return fail(CX3_PCIE_REASON_READ_UNAVAILABLE, kIOReturnOffline);
    }

    /* Final identity and structure pass.  Nothing sampled above is overwritten. */
    if (!read16(0, &vendor) || !read16(2, &device) || !read16(0x2c, &subven) ||
        !read16(0x2e, &subid)) return fail(CX3_PCIE_REASON_INCONSISTENT, kIOReturnOffline);
    out->identityAfter.vendor = vendor; out->identityAfter.device = device;
    out->identityAfter.subsystemVendor = subven; out->identityAfter.subsystemId = subid;
    out->identityAfter.bus = fPci->getBusNumber();
    out->identityAfter.deviceNumber = fPci->getDeviceNumber();
    out->identityAfter.function = fPci->getFunctionNumber();
    out->validity |= CX3_PCIE_VALID_IDENTITY_AFTER;
    if (memcmp(&out->identityBefore, &out->identityAfter, sizeof(out->identityBefore)) != 0)
        return fail(CX3_PCIE_REASON_INCONSISTENT, kIOReturnBusy);
    UInt8 headerAfter, pointerAfter; UInt16 statusAfter;
    if (!read8(0x0e, &headerAfter) || !read16(6, &statusAfter) ||
        !read8(0x34, &pointerAfter)) return fail(CX3_PCIE_REASON_INCONSISTENT, kIOReturnOffline);
    if ((headerAfter & 0x7f) != (header & 0x7f) ||
        !!(statusAfter & (1u << 4)) != !!(pciStatus & (1u << 4)) || pointerAfter != firstPointer)
        return fail(CX3_PCIE_REASON_INCONSISTENT, kIOReturnBusy);
    for (UInt32 i = 0; i < nodeCount; ++i) {
        UInt16 again;
        if (!read16(nodes[i], &again) || again != nodeHeaders[i])
            return fail(CX3_PCIE_REASON_INCONSISTENT, kIOReturnBusy);
        if (nodes[i] == cap) {
            UInt16 flagsAgain;
            if (!read16((UInt32)cap + 2, &flagsAgain) || flagsAgain != flags)
                return fail(CX3_PCIE_REASON_INCONSISTENT, kIOReturnBusy);
        }
    }
    UInt16 staAgain;
    if (!read16((UInt32)cap + 0x12, &staAgain) || staAgain != (UInt16)out->lnkSta)
        return fail(CX3_PCIE_REASON_INCONSISTENT, kIOReturnBusy);
    out->validity |= CX3_PCIE_VALID_CONSISTENT | CX3_PCIE_VALID_LINK_FIELDS;
    if (version == 2) out->validity |= CX3_PCIE_VALID_LNKCAP2;
    return kIOReturnSuccess;
}

/* ------------------------------------------------------------------ */
/* HCR command path -- mirrors mlx4_cmd.c cmd_pending / cmd_post / poll */
/* ------------------------------------------------------------------ */

/*
 * Real wall-clock time, in nanoseconds, for the command waits.
 *
 * This is the same clock abNowNs() uses further down, and it is here rather than there
 * because the waits need it: the whole point of this batch is that a timeout becomes a
 * deadline on a clock instead of a count of iterations.  clock_get_uptime is already in
 * this binary (declared in kern/clock.h, and build.sh's symbol accounting resolves it), so
 * this adds no new dependency.
 */
static UInt64 cx3NowNs()
{
    UInt64 t = 0;
    clock_get_uptime(&t);
    return t;
}

bool Mlx4Hw::cmdPending()
{
    UInt32 status = readHcrStatus();
    UInt32 go = (status & (1u << HCR_GO_BIT)) ? 1u : 0u;
    UInt32 tb = (status & (1u << HCR_T_BIT))  ? 1u : 0u;
    /*
     * FreeBSD/Linux mlx4_cmd.c cmd_pending():
     *     return (status & GO_BIT) || (cmd->toggle == !!(status & T_BIT));
     * The comparison is equality, not inequality: the HCA echoes the toggle bit
     * of the command it has finished, so while the next toggle value still
     * matches the echo the command is outstanding.  Getting this backwards
     * makes the register look permanently busy before the first command and
     * look instantly idle after one, which is exactly the wrong pair of
     * mistakes for a poll loop.
     */
    return (go != 0) || (fToggle == tb);
}

/*
 * Give up on a command, and poison the mailbox so that nothing else is posted into it.
 *
 * Called from exactly two places, both in cmdBoxLocked: the pre-post wait ran out, and the
 * completion wait ran out.  Both mean the same thing -- the register reported an
 * outstanding command for longer than the budget allowed -- and neither can be undone.
 *
 * What is NOT safe (the short version; docs/LOCK-IMPL.md has the source-level argument):
 * returning an error and letting the next command proceed.  The mailbox is one 4 KiB
 * buffer; the next command's first act is to clear it, and the device may still be reading
 * the abandoned command's bytes out of it.  The firmware's own answer, if it ever comes,
 * lands in the status word and the outbox, where the next command would read it as its
 * own.  Both of those produce a *plausible wrong answer* rather than a failure, which is
 * the class of defect this project has paid the most for (docs/DEFECTS.md, family D).
 *
 * So: abandon means poison, and the poison means "no command is posted and no HCR word is
 * touched until a reset".  That is the reference's own semantic
 * (mlx4_cmd.c:634-638 -> mlx4_catas.c:169-200, mlx4_internal_err_reset defaulting to 1).
 *
 * The trade this makes, stated plainly: a command that was merely slow is now a device
 * that is unusable until reset.  The alternative was waiting without an upper bound, which
 * is what made the recovery path (reset) unreachable for six minutes -- the failure this
 * whole batch exists for.  A bounded loss of the device beats an unbounded loss of the
 * driver.
 */
void Mlx4Hw::cmdPoison(UInt16 op, const char *why, UInt32 budgetMs, UInt64 waitedMs,
                       bool posted)
{
    ++fCmdAbandons;
    if (!fCmdWedged || fCmdSyntheticPoison) {
        fCmdWedged       = true;
        fCmdWedgedOp     = op;
        fCmdWedgedPosted = posted;
        fCmdWedgedAtNs   = cx3NowNs();
    }
    /* A real abandonment always dominates an earlier synthetic injection. */
    fCmdSyntheticPoison = false;
    LOG("cmd 0x%x: ABANDONED %s -- %s. Budget %u ms, spent %llu ms. The command mailbox "
        "is now POISONED: every later command fails immediately with fw status 0x%x and "
        "does not touch the HCR, because a late reply to this one would otherwise be read "
        "as the next command's answer. Recovery needs an epoch-bound device/CPU "
        "quiescence credential; raw reset, FLR or an idle HCR cannot clear this poison. Abandonment %u.",
        (unsigned)op, posted ? "after posting" : "before posting", why,
        (unsigned)budgetMs, (unsigned long long)waitedMs,
        (unsigned)CX3_FWSTATUS_WEDGED, (unsigned)fCmdAbandons);
}

/*
 * Undo a host-only injection. Explicit provenance, not the marker opcode or
 * the current HCR idle bit, proves that this poison did not abandon a command.
 * Real recovery requires a separate generation/scope credential, not this API.
 */
void Mlx4Hw::cmdClearWedged(const char *why)
{
    if (!fCmdWedged) {
        LOG("command mailbox: not poisoned, nothing to clear (%s)", why);
        return;
    }
    if (!fCmdSyntheticPoison || fCmdWedgedPosted || fMboxRetained) {
        LOG("command mailbox: refusing poison clear (%s): not an unretained synthetic "
            "injection; op 0x%x posted=%u retained=%u remain unchanged", why,
            (unsigned)fCmdWedgedOp, (unsigned)fCmdWedgedPosted, (unsigned)fMboxRetained);
        return;
    }
    LOG("command mailbox: the poison is cleared by %s. It was set by op 0x%x after %u "
        "abandonment(s); %u command(s) have been refused while a poison held (both counts "
        "are cumulative since this boot and are kept). The next command will post again -- "
        "if the register is still stuck it will be abandoned and the mailbox poisoned "
        "again, with its own log line.",
        why, (unsigned)fCmdWedgedOp, (unsigned)fCmdAbandons, (unsigned)fCmdRefused);
    fCmdWedged     = false;
    fCmdWedgedOp   = 0;
    fCmdWedgedPosted = false;
    fCmdSyntheticPoison = false;
    fCmdWedgedAtNs = 0;
}

/*
 * The same state, set on purpose.
 *
 * The marker opcode and the log line are the point: an injected poison has to be
 * distinguishable from one the hardware caused, in the log and in survey, because the
 * whole value of the injector is that it lets the recovery path be exercised *without*
 * having broken the card.
 */
void Mlx4Hw::cmdPoisonInjected()
{
    if (fCmdWedged) {
        LOG("fault injection: the mailbox is already poisoned (by op 0x%x); nothing "
            "changed. Abandonments so far: %u.",
            (unsigned)fCmdWedgedOp, (unsigned)fCmdAbandons);
        return;
    }
    fCmdWedged       = true;
    fCmdWedgedOp     = (UInt16)CX3_FAULT_OP_MARKER;
    fCmdWedgedPosted = false;
    fCmdSyntheticPoison = true;
    fCmdWedgedAtNs   = cx3NowNs();
    LOG("command mailbox: POISONED ON PURPOSE by the fault injector (kCx3SelFault), "
        "recorded as op 0x%x. No command was abandoned, so the abandonment count stays 0 "
        "and no register was touched. Every command fails at once with fw status 0x%x from "
        "here; 'fault unwedge 1' can revoke this synthetic injection only while the "
        "command register is idle and the mailbox is not retained.",
        (unsigned)CX3_FAULT_OP_MARKER, (unsigned)CX3_FWSTATUS_WEDGED);
}

/*
 * Serialise the command register, the way the reference does.
 *
 * cmd_poll() holds poll_sem across post-and-wait and cmd_post() holds the device
 * state mutex (mlx4_cmd.c:441, :592).  Everything here shares one mailbox buffer,
 * one toggle bit and one set of HCR registers, and the user client is not
 * single-instanced, so two concurrent callers can interleave HCR stores or
 * overwrite each other's mailbox between the memset and the GO bit.  That failure
 * mode produces a legally-formed command with a wrong page list, which is
 * indistinguishable from a hardware problem from the outside.
 *
 * The lock is allocated by init(), before command entry points are published.
 * The boot contract forbids device writes/DMA, not host-only locks. Missing
 * initialization or shutdown must refuse, never allocate or run unlocked.
 */
bool Mlx4Hw::cmdBox(UInt64 inParam, UInt64 outParam, UInt32 inModifier, UInt8 opMod,
                    UInt16 op, UInt32 timeoutMs, UInt32 *outFwStatus, bool bypassBusy,
                    Cx3CommandObservation *outObservation)
{
    bool ok;

    if (!fCmdLock) {
        if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
        if (outObservation) memset(outObservation, 0, sizeof(*outObservation));
        return false;
    }
    IOLockLock(fCmdLock);
    ok = cmdBoxLocked(inParam, outParam, inModifier, opMod, op, timeoutMs, outFwStatus,
                      bypassBusy, 0, 0, false, outObservation);
    IOLockUnlock(fCmdLock);
    return ok;
}

/*
 * A command whose answer comes back in the command register rather than in a
 * mailbox.
 *
 * The reference calls this the "imm" (immediate) form: after the command
 * completes, the two out_param dwords of the HCR hold the value
 * (mlx4_cmd_poll's out_is_imm branch, mlx4_cmd.c:640-645).  SET_ICM_SIZE uses
 * it to report how much auxiliary context memory the HCA wants, so without this
 * path that answer cannot be read at all.
 */
bool Mlx4Hw::cmdBoxImm(UInt64 inParam, UInt64 inModifier, UInt8 opMod, UInt16 op,
                       UInt32 timeoutMs, UInt64 *outImm, UInt32 *outFwStatus,
                       bool bypassBusy)
{
    bool ok;

    if (outImm) *outImm = 0;
    if (!fCmdLock) {
        if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
        return false;
    }
    IOLockLock(fCmdLock);
    /*
     * in_modifier is a 32-bit field in the command register, so narrowing it here is
     * intended rather than accidental -- but it used to be silent, which is the same shape
     * as the truncations that have cost this project time. -Wshorten-64-to-32 asks for the
     * cast, and the cast is where the intent lives.
     */
    ok = cmdBoxLocked(inParam, 0, (UInt32)inModifier, opMod, op, timeoutMs, outFwStatus,
                      bypassBusy, outImm, 0, false);
    IOLockUnlock(fCmdLock);
    return ok;
}

bool Mlx4Hw::cmdBoxLocked(UInt64 inParam, UInt64 outParam, UInt32 inModifier, UInt8 opMod,
                          UInt16 op, UInt32 timeoutMs, UInt32 *outFwStatus, bool bypassBusy,
                          UInt64 *outImm, UInt16 eventToken, bool useEvent,
                          Cx3CommandObservation *outObservation)
{
    UInt32 status;
    UInt32 gate;
    UInt64 startNs, deadlineNs;
    UInt32 shadowStatus = CX3_FWSTATUS_NOT_POSTED;
    if (!outFwStatus) outFwStatus = &shadowStatus;
    /* Stack-only, per invocation. No last-command cache that another command
     * could replace between unlocking and a QP caller reading the result. */
    struct ObservationScope {
        Cx3CommandObservation *out;
        UInt32 *fw;
        ObservationScope(Cx3CommandObservation *o, UInt32 *s) : out(o), fw(s) {
            if (!out) return;
            memset(out, 0, sizeof(*out));
            out->abiVersion = CX3_COMMAND_OBSERVATION_ABI;
            out->abiBytes = sizeof(*out);
            out->flags = kCx3CmdObservedEntered;
            out->startNs = cx3NowNs();
        }
        ~ObservationScope() {
            if (!out) return;
            out->fwStatus = *fw;
            out->endNs = cx3NowNs();
        }
    } observation(outObservation, outFwStatus);

    /*
     * Every early return writes a "there is no firmware status" sentinel.
     *
     * A firmware status of 0 means "the firmware said ok", so leaving the word
     * untouched makes a timeout indistinguishable from success at the caller.
     * That is not hypothetical: M2b reported
     *
     *     RUN_FW : fw status 0x0 ok
     *
     * for a command that never completed, because this function returned before
     * writing outFwStatus and the caller's local started at 0.  Two independent
     * reviews of M2b found the same defect.
     */
    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    if (!fHcr) {
        LOG("cmd 0x%x: no HCR handle, command not posted", (unsigned)op);
        if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NO_DEVICE;
        return false;
    }

    /*
     * The poison check, and the reason it is the first thing after the "no device" check:
     * a poisoned mailbox must cost microseconds and must not read the command register.
     *
     * Reading the register here would be worse than useless.  The reading that a stuck
     * command produces is not a fact about whether the device is idle -- it is the thing
     * that is broken -- and acting on it (posting because it looked idle) is exactly how a
     * late reply to the abandoned command gets claimed by the next one.  So this returns
     * before touching anything the device can see.
     *
     * This also covers bypassBusy on purpose.  That flag exists to post *over* a stuck GO
     * bit for diagnosis ("does the device still answer anything?"), and once the mailbox is
     * poisoned that question is not allowed to be asked from here: the answer would be
     * indistinguishable from a stale reply, and the sanctioned way to ask it is to reset
     * first. Raw reset/FLR remain separate diagnostic paths but do not clear
     * real poison or authorize mailbox reuse without a recovery credential.
     */
    if (fCmdWedged) {
        ++fCmdRefused;
        if (fCmdRefused == 1) {
            LOG("cmd 0x%x: refused before posting -- the command mailbox is poisoned by "
                "op 0x%x. No register is read and no byte of the mailbox is written. "
                "Every later command will be refused the same way; count them in survey "
                "(kSurveyFactCmdWedged) or with 'cx3ctl probe'.",
                (unsigned)op, (unsigned)fCmdWedgedOp);
        }
        if (outFwStatus) *outFwStatus = CX3_FWSTATUS_WEDGED;
        return false;
    }

    /*
     * Once per boot, say what the timeouts now are.
     *
     * This is the line asked for by docs/LOCK-IMPL.md's "independently observable" rule: a
     * reader of the log has to be able to tell that the bounded-timeout-and-poison
     * mechanism is the one that is running, and what values are in force, without reading
     * the binary.  It is printed at the first command rather than at load time because
     * start() must stay read-only and allocation-free.
     */
    {
        static bool sLoggedBudgets = false;
        if (!sLoggedBudgets) {
            sLoggedBudgets = true;
            LOG("command timeouts are wall-clock deadlines now, measured with "
                "clock_get_uptime, not counts of {HCR read + IOSleep(1)}: pre-post idle "
                "wait %u ms (polled) / %u ms (event), firmware budget %u ms, ICM teardown "
                "budget %u ms. Exceeding either wait abandons the command and poisons the "
                "mailbox: after that every command fails at once with fw status 0x%x and "
                "the HCR is not touched until proven recovery (raw reset/FLR do not clear it). "
                "(docs/LOCK-IMPL.md)",
                (unsigned)CX3_CMD_IDLE_WAIT_MS, (unsigned)CX3_CMD_IDLE_WAIT_EVENT_MS,
                (unsigned)CX3_CMD_BUDGET_FW_MS, (unsigned)CX3_CMD_BUDGET_ICM_MS,
                (unsigned)CX3_FWSTATUS_WEDGED);
        }
    }
    startNs = cx3NowNs();

    /*
     * cmd_post(): wait for the register to be idle first (mlx4_cmd.c:462).
     *
     * bypassBusy skips this on request.  It exists because the M2b RUN_FW hang
     * leaves GO set forever, and after that no command can be posted at all --
     * so the one question left ("does the device still answer anything?") cannot
     * be asked.  Posting over a stuck GO is not a normal path; it is how that
     * question gets answered.
     */
    if (bypassBusy) {
        LOG("cmd 0x%x: bypassBusy -- posting even though the register reads 0x%08x",
            (unsigned)op, (unsigned)readHcrStatus());
    } else {
        /*
         * Bounded, and short: 50 ms for a polled command, the reference's 10000 ms for an
         * event-mode one.  This is the wait that cost 22.7 s per command and 6 min 07.6 s
         * per selector when it counted iterations against a constant named MSECS.
         *
         * Note the reading is taken once per pass through the loop, and the decision comes
         * from cx3CmdGate() -- a pure function, so the four transitions are asserted on
         * every build by tools/test-cmd-wedge.c instead of being discovered on hardware.
         */
        UInt32 idleBudgetMs = useEvent ? CX3_CMD_IDLE_WAIT_EVENT_MS : CX3_CMD_IDLE_WAIT_MS;
        UInt64 idleStartNs = cx3NowNs();
        UInt64 idleEndNs   = cx3CmdDeadlineNs(idleStartNs, idleBudgetMs);

        for (;;) {
            gate = cx3CmdGate(fCmdWedged ? 1u : 0u, cmdPending() ? 1u : 0u,
                              cx3NowNs(), idleEndNs);
            if (gate != CX3_CMD_GATE_SPIN) break;
            IOSleep(1);
        }
        if (gate == CX3_CMD_GATE_ABANDON) {
            UInt64 waited = cx3CmdElapsedMs(idleStartNs, cx3NowNs());
            cmdPoison(op, "the command register never went idle before the command was "
                          "posted, so the device was already busy with a command of "
                          "unknown age and unknown ownership",
                      idleBudgetMs, waited, false);
            if (outFwStatus) *outFwStatus = CX3_FWSTATUS_WEDGED;
            return false;
        }
        /* FAIL_FAST cannot be reached here (checked above, and fCmdLock is held), but if
         * it ever were, refusing is the only safe answer. */
        if (gate == CX3_CMD_GATE_FAIL_FAST) {
            if (outFwStatus) *outFwStatus = CX3_FWSTATUS_WEDGED;
            return false;
        }
    }

    /* Writes of less than 32 bits to the HCR do not work, so every field is a
     * full 32-bit store (mlx4_cmd.c:474-499).  out_param carries the DMA
     * address of the command mailbox for the box variant. */
    /*
     * "writing the registers" and "posted" are separate lines on purpose: the difference
     * between a death during the MMIO writes and a death during the poll that follows is the
     * difference between a stuck access and a command that never completes, and the next hard
     * death can only tell them apart if both states are named.
     */
    /*
     * Three markers across the write sequence rather than one: if the poll heartbeat below never
     * appears, "died while writing the registers" is still several stores wide, and these split
     * it into the parameter dwords, the token dword and the GO word.  Cheap (3 lines per
     * command) and bounded.
     */
    LOG("cmd 0x%x: idle; writing HCR[0..4] (in/out params and modifiers)", (unsigned)op);
    fHcr[0] = OSSwapHostToBigInt32((UInt32)(inParam >> 32));
    fHcr[1] = OSSwapHostToBigInt32((UInt32)(inParam & 0xffffffffull));
    fHcr[2] = OSSwapHostToBigInt32(inModifier);
    fHcr[3] = OSSwapHostToBigInt32((UInt32)(outParam >> 32));
    fHcr[4] = OSSwapHostToBigInt32((UInt32)(outParam & 0xffffffffull));
    LOG("cmd 0x%x: HCR[0..4] stored; writing the token dword", (unsigned)op);
    /*
     * Token.  A polled command sends CMD_POLL_TOKEN (0xffff); an event-mode
     * command sends its own small token, which the HCA echoes back in the CMD
     * EQE so the waiter can tell its completion from someone else's
     * (mlx4_cmd.c:487, and :349-352 where the token is allocated).
     */
    fHcr[5] = OSSwapHostToBigInt32((UInt32)(useEvent ? eventToken : CMD_POLL_TOKEN) << 16);
    mlx4Wmb();                          /* the reference's wmb(): keep the field
                                         * stores ahead of the GO bit.  The name
                                         * used to be OSSynchronizeIO, which is an
                                         * empty inline on x86_64 -- see mlx4Wmb's
                                         * definition at the top of this file */
    LOG("cmd 0x%x: token stored; writing the GO word (this is the hand-over)", (unsigned)op);

    fHcr[6] = OSSwapHostToBigInt32(
        (1u << HCR_GO_BIT) |
        ((fToggle ? 1u : 0u) << HCR_T_BIT) |
        /*
         * The E bit.  Setting it is what makes the HCA answer with a CMD EQE
         * instead of only clearing GO -- mlx4_cmd.c:494.  Every command before
         * M3 left it clear, which is exactly why a completed command so far
         * proved nothing about the event path.
         */
        (useEvent ? (1u << HCR_E_BIT) : 0u) |
        (((UInt32)opMod) << HCR_OPMOD_SHIFT) |
        (UInt32)(op & 0xfffu));
    mlx4Wmb();

    if (outObservation) {
        outObservation->flags |= kCx3CmdObservedPosted;
        outObservation->postedNs = cx3NowNs();
    }
    fToggle ^= 1;                       /* mlx4_cmd.c:503 */
    LOG("cmd 0x%x: posted (GO set); polling for it to clear, budget %u ms",
        (unsigned)op, (unsigned)timeoutMs);

    /*
     * mlx4_cmd_poll(): spin until the GO bit clears (mlx4_cmd.c:616).
     *
     * On budget units: this used to count iterations of {HCR read + IOSleep(1)} and
     * compare the count against timeoutMs, so the "60 s" budget was about 126 s of wall
     * clock and the "5 s" one was about 10.5 s (measured twice from the kernel log's own
     * timestamps: 5000 iterations = 10.52 s, 60000 = 185.36 s).  The budget is a real
     * millisecond count now and it is spent against clock_get_uptime, so a reader can
     * compare it with any other timestamp in the log.
     *
     * When it runs out the command is abandoned AND the mailbox is poisoned -- there is no
     * third option that lets the driver carry on safely; the argument is at cmdPoison().
     */
    deadlineNs = cx3CmdDeadlineNs(cx3NowNs(), timeoutMs);
    {
        /*
         * Bounded logging inside the poll: the first pass, then once a second.  A hard death
         * here leaves "still outstanding after N ms" as the last line, which is a measurement
         * of how long the register answered GO before the machine stopped -- and it is what
         * separates "the poll never saw it clear" from "the reads themselves stopped working"
         * (the line prints the HCR value it just read).
         */
        UInt64 lastLogNs = cx3NowNs();
        unsigned passes = 0, heartbeats = 0;

        for (;;) {
            gate = cx3CmdGate(fCmdWedged ? 1u : 0u, cmdPending() ? 1u : 0u,
                              cx3NowNs(), deadlineNs);
            if (gate != CX3_CMD_GATE_SPIN) break;
            ++passes;
            /*
             * A heartbeat every 250 ms, CAPPED at four lines, and it carries the fields that say
             * what the register answered rather than just that time passed:
             *
             *   GO still set at every heartbeat  -> the command was posted and the device never
             *                                       completed it (a device/firmware statement);
             *   GO cleared at a heartbeat, then the machine dies
             *                                    -> the death is after completion, in the driver's
             *                                       own reads/copy/decode (deadlineNs and the
             *                                       gate would have exited the loop, so this
             *                                       combination means the read after the gate);
             *   one heartbeat or none at all     -> the fatal event is a register access itself.
             *
             * The cap exists so that a 60-second budget cannot fill the log: this line is for
             * the death that follows, not for the seven hundred heartbeats a stuck command
             * produces.  (The parallel forensics session asked for exactly this shape, and it is
             * the only channel that survives a hard death: nothing outside the machine can pull
             * a stack once every CPU is wedged -- spindump only writes its report when it
             * finishes, which is proof that the outside-the-machine route is closed.)
             */
            if (heartbeats < 4u &&
                (passes == 1u || cx3CmdElapsedMs(lastLogNs, cx3NowNs()) >= 250ull)) {
                UInt32 hcr = readHcrStatus();
                LOG("cmd 0x%x: heartbeat %u -- %llu ms after the post, HCR 0x%08x (GO=%u T=%u), "
                    "fToggle=%u, %u pass(es)", (unsigned)op, heartbeats + 1u,
                    (unsigned long long)cx3CmdElapsedMs(startNs, cx3NowNs()),
                    (unsigned)hcr, (unsigned)((hcr >> HCR_GO_BIT) & 1u),
                    (unsigned)((hcr >> HCR_T_BIT) & 1u), (unsigned)(fToggle ? 1u : 0u), passes);
                ++heartbeats;
                lastLogNs = cx3NowNs();
            }
            IOSleep(1);
        }
    }
    if (gate == CX3_CMD_GATE_ABANDON) {
        UInt64 waited = cx3CmdElapsedMs(startNs, cx3NowNs());
        LOG("command 0x%x did not complete: the budget is %u ms measured from the moment it "
            "was posted, and %llu ms have passed since this call started (the pre-post wait "
            "included). Status 0x%08x, so the device never completed it.",
            (unsigned)op, (unsigned)timeoutMs, (unsigned long long)waited,
            (unsigned)readHcrStatus());
        cmdPoison(op, "the command was posted and the register still reported it "
                      "outstanding when the budget ran out",
                  timeoutMs, waited, true);
        /*
         * TIMEOUT, not WEDGED, and the difference is not cosmetic: this command *was*
         * posted, so "it may have been seen by the device and may still complete" is the
         * fact, while WEDGED means "not sent at all".  The poison is set either way -- the
         * two are independent -- and the next command is the one that reports WEDGED.
         * Collapsing the two would throw away the only word that says whether the device
         * could have acted on this command's mailbox.
         */
        if (outFwStatus) *outFwStatus = CX3_FWSTATUS_TIMEOUT;
        return false;
    }
    if (gate != CX3_CMD_GATE_IDLE) {
        /* Defensive: unreachable while fCmdLock is held, and refusing is the safe answer. */
        if (outFwStatus) *outFwStatus = CX3_FWSTATUS_WEDGED;
        return false;
    }

    status = readHcrStatus();
    if (outObservation) outObservation->flags |= kCx3CmdObservedReplied;
    /*
     * Read the immediate output parameter, if the caller wants it, before the
     * status is interpreted -- and read it through the same warm accessor as
     * everything else.  Both dwords are big-endian on the wire.
     */
    if (outImm) {
        UInt64 hi = OSSwapBigToHostInt32(readRegWarm(MLX4_HCR_BASE + 0x0c));
        UInt64 lo = OSSwapBigToHostInt32(readRegWarm(MLX4_HCR_BASE + 0x10));
        *outImm = (hi << 32) | lo;
    }
    if (outFwStatus) *outFwStatus = status >> 24;   /* mlx4_cmd.c:645 */
    if ((status >> 24) != 0) {
        LOG("command 0x%x failed: fw status = 0x%x",
            (unsigned)op, (unsigned)(status >> 24));
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* command mailbox (replaces pci_pool_create + dma_alloc_coherent)     */
/* ------------------------------------------------------------------ */

bool Mlx4Hw::mboxAlloc()
{
    IODMACommand::Segment64 seg;
    UInt32 nseg = 1;
    UInt64 offset = 0;
    IOReturn segRc;

    /* Callers clear/fill the mailbox BEFORE cmdBox checks command poison. */
    if (fCmdWedged || fMboxRetained) {
        LOG("mailbox unavailable: command poisoned or mapping retained; not reused");
        return false;
    }
    if (fMboxMd) {
        /* Descriptor ownership alone is not a usable CPU/DMA view (e.g. after shutdown). */
        return fMboxCmd && fMboxVirt && fMboxSegCount == 1 &&
               fMboxOffset == MLX4_MAILBOX_SIZE;
    }
    if (fMboxCmd || fMboxVirt || fMboxPhys || fMboxOffset || fMboxSegCount) {
        LOG("mailbox unavailable: incomplete mapping retained in place");
        return false;
    }

    fMboxSegCount = 0;
    fMboxOffset = 0;
    memset(&seg, 0, sizeof(seg));

    /*
     * Page aligned so the 4 KiB outbox is a single DMA segment, and kept below
     * 4 GiB on purpose.
     *
     * The outbox address is handed to the firmware as `out_param`, i.e. the card
     * will DMA its answer into it.  If that address were ever mis-traslated (a
     * 32-bit DMA window, a missing IOMMU mapping), the failure mode would be the
     * card writing to an unrelated physical page.  Constraining the buffer to
     * the low 4 GiB removes the truncation half of that risk outright; the
     * address is re-checked in queryFw() before it is used.
     */
    const UInt64 kMboxPhysMask = 0x00000000fffff000ull;
    fMboxMd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIODirectionInOut | kIOMemoryKernelUserShared,
        MLX4_MAILBOX_SIZE,
        kMboxPhysMask);
    if (!fMboxMd) {
        LOG("mailbox IOBufferMemoryDescriptor allocation failed");
        return false;
    }
    fMboxVirt = fMboxMd->getBytesNoCopy();
    if (!fMboxVirt) {
        LOG("mailbox has no CPU view");
        mboxFree();
        return false;
    }

    fMboxCmd = IODMACommand::withSpecification(kIODMACommandOutputHost64, 64, 0,
                                              IODMACommand::kMapped,
                                              MLX4_MAILBOX_SIZE, 1);
    if (!fMboxCmd) {
        LOG("mailbox IODMACommand allocation failed");
        mboxFree();
        return false;
    }
    if (fMboxCmd->setMemoryDescriptor(fMboxMd, true) != kIOReturnSuccess) {
        LOG("mailbox setMemoryDescriptor failed");
        mboxFree();
        return false;
    }

    /*
     * gen64IOVMSegments takes `offset` as an in/out parameter: on the way in it
     * is where to start, on the way out it is where generation stopped.  So a
     * fully consumed 4096-byte descriptor legitimately comes back with
     * offset == 4096, which is "the whole buffer was covered by one segment" --
     * the success case.
     *
     * The first version of this check tested `offset != 0` and therefore
     * rejected every successful allocation:
     *     "mailbox is not one contiguous DMA segment (nseg=1 off=4096)"
     * The contiguity property we actually care about is nseg == 1; the offset
     * only confirms the whole descriptor was consumed.
     */
    segRc = fMboxCmd->gen64IOVMSegments(&offset, &seg, &nseg);
    if (segRc != kIOReturnSuccess || nseg != 1 || offset != MLX4_MAILBOX_SIZE ||
        seg.fLength != MLX4_MAILBOX_SIZE || seg.fIOVMAddr > kMboxPhysMask ||
        (seg.fIOVMAddr & (MLX4_MAILBOX_SIZE - 1u))) {
        LOG("mailbox is not one contiguous DMA segment "
            "(rc=0x%x nseg=%u off=%llu want off=%u addr=0x%llx)",
            segRc, (unsigned)nseg, (unsigned long long)offset,
            (unsigned)MLX4_MAILBOX_SIZE, (unsigned long long)seg.fIOVMAddr);
        mboxFree();
        return false;
    }
    fMboxPhys = seg.fIOVMAddr;
    fMboxOffset = offset;
    fMboxSegCount = nseg;
    LOG("mailbox: virt %p phys 0x%llx len %u (low-4GiB constraint %s)",
        fMboxVirt, (unsigned long long)fMboxPhys, (unsigned)MLX4_MAILBOX_SIZE,
        (fMboxPhys < 0x100000000ull) ? "satisfied" : "VIOLATED");
    return true;
}

void Mlx4Hw::mboxFree()
{
    if ((fCmdWedged || fMboxRetained) && (fMboxMd || fMboxCmd || fModuleInbox)) {
        /* freeMailbox() is public too: no caller may bypass shutdown's retention. */
        fMboxRetained = true;
        fMboxVirt = 0;
        fMboxPhys = 0;
        fMboxOffset = 0;
        fMboxSegCount = 0;
        LOG("mailbox free refused: retaining descriptor and DMA mapping until reboot");
        return;
    }
    if (fModuleInbox) { icmFree(fModuleInbox); fModuleInbox = 0; }
    if (fMboxCmd) {
        fMboxCmd->clearMemoryDescriptor(true);
        fMboxCmd->release();
        fMboxCmd = 0;
    }
    if (fMboxMd) {
        fMboxMd->release();
        fMboxMd = 0;
    }
    fMboxVirt = 0;
    fMboxPhys = 0;
    fMboxOffset = 0;
    fMboxSegCount = 0;
}

/*
 * Public wrapper used by the user client's DMA-only bisect step.  Deliberately
 * does nothing else: no register read, no register write, no command.
 */
bool Mlx4Hw::allocMailbox()
{
    return mboxAlloc();
}


/* ------------------------------------------------------------------ */
/* ICM: bus-addressable memory for the firmware's context tables        */
/* ------------------------------------------------------------------ */

/*
 * Allocate npages of 4 KiB, page aligned, and record each page's bus address.
 *
 * Device-inert: no register is touched.  That matters because the ICM layer is
 * where the reference does most of its DMA work, and it is worth being able to
 * exercise the allocation on its own before any of it is handed to the firmware.
 *
 * Two properties mlx4_map_cmd depends on are checked rather than assumed:
 *   - every page is aligned to its own size (guaranteed by the physical mask)
 *   - no page straddles a bus segment (checked by requiring every segment to be
 *     a whole number of pages)
 * The physical mask asks the allocator for page-aligned memory, but it does not
 * promise contiguity, so a multi-page area can legitimately come back as several
 * segments.  Each is fine on its own; a segment ending mid-page is not, because
 * that page would have two bus addresses.
 */
Mlx4Icm *Mlx4Hw::icmAlloc(UInt32 npages, bool lowmem, bool contiguous)
{
    Mlx4Icm *icm;
    UInt64   bytes;
    UInt64   offset = 0;
    UInt32   nseg, i, page;
    UInt64   covered;

    if (npages == 0 || npages > (1u << 20) || fIcmAllocationSequence == ~0ull) return 0;

    bytes = (UInt64)npages * 4096ull;
    icm = (Mlx4Icm *)IOMalloc(sizeof(Mlx4Icm));
    if (!icm) return 0;
    memset(icm, 0, sizeof(*icm));
    icm->allocationId = ++fIcmAllocationSequence;
    icm->generation.init(fDeviceGeneration);
    icm->bytes  = bytes;
    icm->npages = npages;

    icm->md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared |
            (contiguous ? kIOMemoryPhysicallyContiguous : 0),
        (IOByteCount)bytes,
        lowmem ? 0x00000000fffff000ull : 0xfffffffffffff000ull);
    if (!icm->md) {
        LOG("icm: IOBufferMemoryDescriptor(%llu bytes) failed",
            (unsigned long long)bytes);
        icmFree(icm);
        return 0;
    }
    icm->virt = icm->md->getBytesNoCopy();

    icm->cmd = IODMACommand::withSpecification(kIODMACommandOutputHost64, 64, 0,
                                              IODMACommand::kMapped, bytes, 1);
    if (!icm->cmd || icm->cmd->setMemoryDescriptor(icm->md, true) != kIOReturnSuccess) {
        LOG("icm: IODMACommand setup failed");
        icmFree(icm);
        return 0;
    }

    /* Walk the bus segments, emitting one entry per whole 4 KiB page. */
    icm->pages = (Mlx4IcmPage *)IOMalloc(sizeof(Mlx4IcmPage) * npages);
    if (!icm->pages) { icmFree(icm); return 0; }
    memset(icm->pages, 0, sizeof(Mlx4IcmPage) * npages);

    page = 0;
    covered = 0;
    icm->pagesAligned = true;
    for (i = 0; i < npages; ++i) {          /* upper bound on segment count */
        IODMACommand::Segment64 seg;
        UInt32 n = 1;
        UInt64 segPages, k;

        memset(&seg, 0, sizeof(seg));
        if (icm->cmd->gen64IOVMSegments(&offset, &seg, &n) != kIOReturnSuccess) break;
        if (n == 0 || seg.fLength == 0) break;

        if (seg.fLength & 0xfffull) {
            /* A partial page: that page would have two bus addresses, which the
             * firmware's page list cannot express.  Refuse the whole thing rather
             * than hand over something subtly wrong. */
            LOG("icm: segment length 0x%llx is not a whole number of 4 KiB pages",
                (unsigned long long)seg.fLength);
            icm->pagesAligned = false;
            break;
        }
        segPages = seg.fLength >> 12;
        for (k = 0; k < segPages && page < npages; ++k, ++page) {
            icm->pages[page].busAddr  = seg.fIOVMAddr + (k << 12);
            icm->pages[page].kernVirt = (void *)((UInt64)icm->virt + covered + (k << 12));
        }
        covered += seg.fLength;
        if (covered >= bytes) break;
    }
    icm->nseg = i + 1;
    icm->contiguous = (icm->nseg == 1);

    if (page != npages) {
        LOG("icm: only %u of %u pages got bus addresses (segments did not cover "
            "the buffer)", (unsigned)page, (unsigned)npages);
        icmFree(icm);
        return 0;
    }

    /*
     * The bus-address range, not just the first address.
     *
     * Whether the firmware can reach every page of the area is the open question
     * behind the M2b RUN_FW hang, and "the first address is low" does not answer
     * it: the area spans many segments and can cross 4 GiB partway through.  Cost
     * of measuring it here is one pass over the page array, so it is measured on
     * every allocation rather than reconstructed by hand from the first address.
     */
    {
        UInt64 lo = ~0ull, hi = 0;
        UInt32 above4g = 0;
        for (page = 0; page < npages; ++page) {
            UInt64 a = icm->pages[page].busAddr;
            if (a < lo) lo = a;
            if (a > hi) hi = a;
            if (a >= 0x100000000ull) ++above4g;
        }
        icm->busLow = lo;
        icm->busHigh = hi;
        icm->pagesAbove4G = above4g;
    }

    LOG("icm: %u pages / %llu bytes, %u segment(s), %s, pages aligned: %s",
        (unsigned)icm->npages, (unsigned long long)icm->bytes, (unsigned)icm->nseg,
        icm->contiguous ? "contiguous" : "scattered",
        icm->pagesAligned ? "yes" : "NO");
    LOG("icm: bus range 0x%llx .. 0x%llx (span %llu MiB), %u page(s) at or above 4 GiB",
        (unsigned long long)icm->busLow, (unsigned long long)icm->busHigh,
        (unsigned long long)((icm->busHigh - icm->busLow + 4095) >> 20),
        (unsigned)icm->pagesAbove4G);
    return icm;
}

void Mlx4Hw::icmFree(Mlx4Icm *icm)
{
    if (!icm) return;
    if (icm->pages) { IOFree(icm->pages, sizeof(Mlx4IcmPage) * icm->npages); icm->pages = 0; }
    if (icm->cmd) {
        icm->cmd->clearMemoryDescriptor(true);
        icm->cmd->release();
        icm->cmd = 0;
    }
    if (icm->md) { icm->md->release(); icm->md = 0; }
    IOFree(icm, sizeof(Mlx4Icm));
}

/*
 * Summarise an ICM area: how many bytes are non-zero, and a checksum.
 *
 * Deliberately a full scan rather than a sample.  A firmware image that was
 * partially DMA'd in would look exactly like a complete one if only the head
 * were checked, and the whole point of this reading is to distinguish "the
 * device filled the area" from "the device never touched it".
 */
void Mlx4Hw::icmSum(const Mlx4Icm *icm, Mlx4IcmSum *out)
{
    const UInt8 *p;
    UInt64 i;

    if (!out) return;
    out->bytes = 0;
    out->nonzero = 0;
    out->checksum = 0;
    if (!icm || !icm->virt || !icm->bytes) return;

    p = (const UInt8 *)icm->virt;
    out->bytes = icm->bytes;
    for (i = 0; i < icm->bytes; ++i) if (p[i]) ++out->nonzero;
    /*
     * Big-endian assembly, so the checksum is computed over the same dword values
     * that icmDump prints.  Two different views of the same memory would make two
     * samples impossible to compare by eye.
     */
    for (i = 0; i + 4 <= icm->bytes; i += 4) {
        UInt32 w = ((UInt32)p[i] << 24) | ((UInt32)p[i + 1] << 16) |
                   ((UInt32)p[i + 2] << 8) | (UInt32)p[i + 3];
        out->checksum += w;
    }
}

/*
 * Read `count` dwords at `byteOffset` out of an ICM area, in host byte order.
 * Returns false (and leaves the outputs alone) if the range is outside the area.
 */
bool Mlx4Hw::icmDump(const Mlx4Icm *icm, UInt32 byteOffset, UInt32 count, UInt32 *out)
{
    const UInt8 *p;
    UInt32 i;

    if (!icm || !icm->virt || !out || count == 0) return false;
    if ((UInt64)byteOffset + (UInt64)count * 4 > icm->bytes) return false;

    p = (const UInt8 *)icm->virt + byteOffset;
    /* Big-endian, matching how the device writes outboxes: the dumped words can
     * then be compared directly against the kernel log's QUERY_FW lines. */
    for (i = 0; i < count; ++i) {
        out[i] = ((UInt32)p[i * 4] << 24) | ((UInt32)p[i * 4 + 1] << 16) |
                 ((UInt32)p[i * 4 + 2] << 8) | (UInt32)p[i * 4 + 3];
    }
    return true;
}

/*
 * Fill an ICM area with a pattern that cannot be mistaken for either zeros or a
 * firmware image: dword i = 0xF00D0000 | (i & 0xffff).
 *
 * Host memory only -- no device access.  Its purpose is to make the later
 * reading of the area unambiguous, because freshly allocated kernel pages can
 * hold old data, so "the area is not all zeros" would otherwise prove nothing.
 * See kFwLoadInFill in cx3_abi.h.
 */
void Mlx4Hw::icmFill(Mlx4Icm *icm)
{
    UInt32 *p;
    UInt32  dwords, i;

    if (!icm || !icm->virt || icm->bytes < 4) return;
    p = (UInt32 *)icm->virt;
    dwords = (UInt32)(icm->bytes / 4);
    for (i = 0; i < dwords; ++i) p[i] = 0xf00d0000u | (i & 0xffffu);
}

/*
 * Independent verification of the two invariants the firmware's page list needs.
 * Returns false and the index of the first offending page.
 */
bool Mlx4Hw::icmCheck(const Mlx4Icm *icm, UInt32 *badPage)
{
    UInt32 i;
    if (badPage) *badPage = 0xffffffffu;
    if (!icm || !icm->pages || icm->npages == 0) return false;

    for (i = 0; i < icm->npages; ++i) {
        if (icm->pages[i].busAddr & 0xfffull) {
            if (badPage) *badPage = i;
            LOG("icmCheck: page %u bus address 0x%llx is not 4 KiB aligned",
                (unsigned)i, (unsigned long long)icm->pages[i].busAddr);
            return false;
        }
        if (icm->pages[i].kernVirt == 0) {
            if (badPage) *badPage = i;
            LOG("icmCheck: page %u has no kernel address", (unsigned)i);
            return false;
        }
    }
    if (!icm->pagesAligned) {
        LOG("icmCheck: a bus segment was not a whole number of pages");
        return false;
    }
    return true;
}


/* ------------------------------------------------------------------ */
/* M2b: MAP_FA, RUN_FW, MOD_STAT_CFG                                    */
/* ------------------------------------------------------------------ */

/*
 * mlx4_map_cmd() -- FreeBSD mlx4_core/mlx4_fw.c:1501.
 *
 * The firmware keeps its own context tables in host RAM, so the host has to tell
 * it which pages to use.  The mailbox holds 64-bit pairs:
 *
 *   [0]  the address the firmware should see this block at (MAP_ICM), or nothing
 *        at all when the caller passes virt = -1 (MAP_FA maps by bus address
 *        into a fixed region, so there is no virtual side to send)
 *   [1]  the block's bus address, ORed with (lg - 12), where lg is the least
 *        significant set bit of (address | size) -- that is, the log2 of the
 *        largest power of two dividing both, i.e. how well aligned the block is
 *
 * The command's in_param is the mailbox's DMA address and in_modifier is the
 * entry count.  The list is sent in batches of MLX4_MAILBOX_SIZE / 16 = 256.
 *
 * This port emits one entry per 4 KiB page with the alignment field set to 0,
 * rather than computing lg per scatterlist entry as the reference does.  Both are
 * valid; the reference form is more compact because it can describe a whole
 * aligned block in one entry, and the difference is efficiency, not correctness.
 */
/*
 * One implementation for both MAP_FA and MAP_ICM, because the only difference is
 * whether the virtual-address slot is filled in.  Two copies of this loop would be
 * two places to get the layout wrong.
 *
 * MAP_FA passes sendVirt = false (mlx4_map_cmd's virt = -1): the FW area is mapped
 * by bus address into a fixed region, so the even 64-bit slot stays zero.
 * MAP_ICM passes true and a starting virtual address, which advances by a page per
 * entry and keeps advancing across batch boundaries.
 */
bool Mlx4Hw::mapFa(Mlx4Icm *icm, UInt32 *outEntries, UInt32 *outFwStatus)
{
    return mapCmd(MLX4_CMD_MAP_FA, icm, false, 0, outEntries, outFwStatus);
}

bool Mlx4Hw::mapIcm(Mlx4Icm *icm, UInt64 virt, UInt32 *outEntries, UInt32 *outFwStatus)
{
    return mapCmd(MLX4_CMD_MAP_ICM, icm, true, virt, outEntries, outFwStatus);
}

bool Mlx4Hw::mapCmd(UInt16 op, Mlx4Icm *icm, bool sendVirt, UInt64 virt,
                    UInt32 *outEntries, UInt32 *outFwStatus)
{
    UInt8  *b;
    UInt32  nent = 0, total = 0, status = 0, i, badPage = 0;

    if (outEntries)   *outEntries = 0;
    if (outFwStatus)  *outFwStatus = 0xffffffffu;
    if (!icm || !icm->pages || icm->npages == 0) {
        LOG("map op 0x%x: nothing to map", (unsigned)op);
        return false;
    }
    if (!icmCheck(icm, &badPage)) {
        LOG("map op 0x%x: refusing, page %u failed the alignment check",
            (unsigned)op, (unsigned)badPage);
        return false;
    }
    if (!fDeviceGeneration || !icm->generation.validAllocation()) {
        LOG("map op 0x%x: refusing an unbound allocation/device generation", (unsigned)op);
        return false;
    }
    if (!mboxAlloc()) return false;
    /* A timeout or partial batch is ambiguous; stamp before the first MAP post. */
    if (!icm->generation.markHandoffAttempted(fDeviceGeneration)) {
        LOG("map op 0x%x: refusing because device generation is unbound", (unsigned)op);
        return false;
    }

    b = (UInt8 *)fMboxVirt;
    memset(b, 0, MLX4_MAILBOX_SIZE);

    for (i = 0; i < icm->npages; ++i) {
        /*
         * icmCheck has verified every page is 4 KiB aligned, so lg is 12 and the
         * alignment field (lg - MLX4_ICM_PAGE_SHIFT) is 0.  Big-endian on the
         * wire: the firmware reads these as __be64 whatever the host order is.
         */
        if (sendVirt) {
            mlx4PutBe64(b + nent * 16 + 0, virt);
            virt += MLX4_ICM_PAGE_SIZE;
        }
        mlx4PutBe64(b + nent * 16 + 8,
                    icm->pages[i].busAddr | (UInt64)(MLX4_ICM_PAGE_SHIFT - MLX4_ICM_PAGE_SHIFT));
        ++nent;
        ++total;

        if (nent == MLX4_MAP_CMD_MAX_ENTRIES) {
            if (!cmdBox(fMboxPhys, 0, nent, 0, op,
                        CX3_M1_CMD_TIMEOUT_MS, &status)) {
                LOG("map op 0x%x: batch failed after %u entries (fw status 0x%x)",
                    (unsigned)op, (unsigned)(total - nent), (unsigned)status);
                if (outEntries)  *outEntries = total - nent;
                if (outFwStatus) *outFwStatus = status;
                return false;
            }
            nent = 0;
            memset(b, 0, MLX4_MAILBOX_SIZE);
        }
    }

    if (nent) {
        if (!cmdBox(fMboxPhys, 0, nent, 0, op, CX3_M1_CMD_TIMEOUT_MS, &status)) {
            LOG("map op 0x%x: final batch of %u failed (fw status 0x%x)",
                (unsigned)op, (unsigned)nent, (unsigned)status);
            if (outEntries)  *outEntries = total - nent;
            if (outFwStatus) *outFwStatus = status;
            return false;
        }
    }

    if (outEntries)  *outEntries = total;
    if (outFwStatus) *outFwStatus = status;
    LOG("map op 0x%x: %u page(s) / %llu bytes in %u batch(es), first bus 0x%llx, "
        "virt slot %s",
        (unsigned)op, (unsigned)total, (unsigned long long)icm->bytes,
        (unsigned)((total + MLX4_MAP_CMD_MAX_ENTRIES - 1) / MLX4_MAP_CMD_MAX_ENTRIES),
        (unsigned long long)icm->pages[0].busAddr,
        sendVirt ? "filled" : "zero (no virtual side for this op)");
    return true;
}

/*
 * mlx4_RUN_FW() -- FreeBSD mlx4_fw.c:1594.  No parameters at all: it tells the
 * firmware to leave its idle state and start running from the area MAP_FA mapped.
 */
bool Mlx4Hw::runFw(UInt32 *outFwStatus)
{
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    bool ok;

    if (outFwStatus) *outFwStatus = 0xffffffffu;
    ok = cmdBox(0, 0, 0, 0, MLX4_CMD_RUN_FW, MLX4_CMD_TIME_CLASS_A, &status);
    if (outFwStatus) *outFwStatus = status;
    if (!ok) {
        LOG("RUN_FW failed (fw status 0x%x)", (unsigned)status);
        return false;
    }
    LOG("RUN_FW: firmware started (fw status 0x%x)", (unsigned)status);
    return true;
}

/*
 * mlx4_UNMAP_FA() -- FreeBSD mlx4_fw.c:1585-1590 and mlx4_main.c:1496-1500.
 *
 * No mailbox and no parameters: it tells the firmware to forget the FW-area page
 * list.  The reference always does this before freeing the area (mlx4_close_fw,
 * mlx4_main.c:1913) and on its load-failure path, so that a later load does not
 * start from a page list pointing at memory the host has already recycled.
 */
bool Mlx4Hw::unmapFa(UInt32 *outFwStatus)
{
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    bool ok;

    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    ok = cmdBox(0, 0, 0, 0, MLX4_CMD_UNMAP_FA, MLX4_CMD_TIME_CLASS_B, &status);
    if (outFwStatus) *outFwStatus = status;
    if (!ok || status != 0) {
        LOG("UNMAP_FA failed (fw status 0x%x)", (unsigned)status);
        return false;
    }
    LOG("UNMAP_FA: firmware released the FW-area page list (fw status 0x%x)",
        (unsigned)status);
    return true;
}

/*
 * mlx4_MOD_STAT_CFG() -- FreeBSD mlx4_fw.c:177.
 *
 * Note the direction: this one takes an INBOX, so the mailbox address goes in
 * in_param with no outbox.  The reference only warns when this fails, because the
 * page-size override is an optimisation: the defaults still work.
 */
bool Mlx4Hw::modStatCfg(UInt8 logPgSz, UInt8 logPgSzM, UInt32 *outFwStatus)
{
    UInt8  *b;
    UInt32  status = 0;
    bool    ok;

    if (outFwStatus) *outFwStatus = 0xffffffffu;
    if (!mboxAlloc()) return false;

    b = (UInt8 *)fMboxVirt;
    memset(b, 0, MLX4_MAILBOX_SIZE);
    b[MOD_STAT_CFG_PG_SZ_OFFSET]   = logPgSz;
    b[MOD_STAT_CFG_PG_SZ_M_OFFSET] = logPgSzM;

    ok = cmdBox(fMboxPhys, 0, 0, 0, MLX4_CMD_MOD_STAT_CFG,
                MLX4_CMD_TIME_CLASS_A, &status);
    if (outFwStatus) *outFwStatus = status;
    LOG("MOD_STAT_CFG(log_pg_sz=%u, log_pg_sz_m=%u): %s (fw status 0x%x)",
        (unsigned)logPgSz, (unsigned)logPgSzM, ok ? "ok" : "failed",
        (unsigned)status);
    return ok;
}

/* ------------------------------------------------------------------ */
/* QUERY_FW -- mirrors FreeBSD mlx4_QUERY_FW() (mlx4_fw.c:1600)        */
/* ------------------------------------------------------------------ */

/*
 * Parse a QUERY_FW outbox.  Shared by the mailbox version and the version whose
 * outbox sits in the FW area, so the two can never disagree about the layout.
 */
static void mlx4ParseQueryFwOutbox(const UInt8 *b, Mlx4QueryFwResult *out)
{
    UInt64 fwVer;
    int i;

    /* QUERY_FW's own window, which is narrow on purpose -- see the note on
     * MLX4_QUERY_FW_WORDS.  Only the first four dwords are ever used. */
    for (i = 0; i < MLX4_QUERY_FW_WORDS; ++i)
        out->rawWords[i] = mlx4GetBe32(b + i * 4);

    /* mlx4_fw.c:1641-1649 (the subminor version sits above the minor version,
     * so the two halves are swapped). */
    fwVer = mlx4GetBe64(b + QUERY_FW_VER_OFFSET);
    out->fwVer = (fwVer & 0xffff00000000ull) |
                 ((fwVer & 0xffff0000ull) >> 16) |
                 ((fwVer & 0x0000ffffull) << 16);
    out->functionId  = b[QUERY_FW_PPF_ID];
    out->cmdIfRev    = mlx4GetBe16(b + QUERY_FW_CMD_IF_REV_OFFSET);
    out->maxCmdLog2  = b[QUERY_FW_MAX_CMD_OFFSET];
    out->maxCmds     = 1u << out->maxCmdLog2;
    out->catasOffset = mlx4GetBe64(b + QUERY_FW_ERR_START_OFFSET);
    out->catasSize   = mlx4GetBe32(b + QUERY_FW_ERR_SIZE_OFFSET);
    out->catasBar    = (mlx4GetBe32(b + QUERY_FW_ERR_BAR_OFFSET) >> 6) * 2;
    /*
     * The interrupt-clear register, for the legacy interrupt path
     * (mlx4_fw.c:1692-1694).  Same (v >> 6) * 2 BAR encoding as catas above -- the
     * reference does this conversion for both, so it is a property of the field and
     * not an accident of one call site.
     */
    out->clrIntBase  = mlx4GetBe64(b + QUERY_FW_CLR_INT_BASE_OFFSET);
    out->clrIntBar   = (mlx4GetBe32(b + QUERY_FW_CLR_INT_BAR_OFFSET) >> 6) * 2;
    /*
     * Offset 0x00, as both references read it -- see the long note on
     * QUERY_FW_SIZE_OFFSET in mlx4_regs.h.  The other candidate this port used to
     * use (0x10) is printed alongside so that the next reader can see the two
     * numbers disagree instead of having to take this one on faith.
     */
    out->fwPages  = mlx4GetBe16(b + QUERY_FW_SIZE_OFFSET);
    out->fwPages0x10 = mlx4GetBe16(b + QUERY_FW_SIZE_OFFSET_WRONG);
    LOG("QUERY_FW: fw_pages=%u pages (0x%04x) = %u KiB from offset 0x%02x "
        "[reference; the 0x%02x candidate reads %u pages and is NOT the FW size]",
        (unsigned)out->fwPages, (unsigned)out->fwPages,
        (unsigned)out->fwPages * (MLX4_ICM_PAGE_SIZE / 1024),
        (unsigned)QUERY_FW_SIZE_OFFSET,
        (unsigned)QUERY_FW_SIZE_OFFSET_WRONG, (unsigned)out->fwPages0x10);
    out->ok = true;
}

bool Mlx4Hw::queryFw(Mlx4QueryFwResult *out)
{
    UInt8  *b;
    UInt64  fwVer;
    UInt32  fwStatus = 0;
    int     i;

    if (!out) return false;
    memset(out, 0, sizeof(*out));

    if (!mboxAlloc()) return false;
    memset(fMboxVirt, 0, MLX4_MAILBOX_SIZE);

    /*
     * Last line of defence before a DMA address leaves this driver: never hand
     * the firmware an outbox address outside the low 4 GiB window (see
     * mboxAlloc for why).  Refusing here costs one failed query; skipping the
     * check could cost an unrelated page of kernel memory.
     */
    if (fMboxPhys == 0 || fMboxPhys >= 0x100000000ull) {
        LOG("QUERY_FW aborted: outbox DMA address 0x%llx is not in the low 4 GiB "
            "window, refusing to hand it to the firmware",
            (unsigned long long)fMboxPhys);
        return false;
    }

    /* mlx4_fw.c:1637:
     *   mlx4_cmd_box(dev, 0, mailbox->dma, 0, 0, MLX4_CMD_QUERY_FW,
     *                MLX4_CMD_TIME_CLASS_A, MLX4_CMD_NATIVE)  */
    if (!cmdBox(0, fMboxPhys, 0, 0, MLX4_CMD_QUERY_FW,
                CX3_M1_CMD_TIMEOUT_MS, &fwStatus)) {
        out->fwStatus = fwStatus;
        out->ok = false;
        return false;
    }
    out->fwStatus = fwStatus;

    b = (UInt8 *)fMboxVirt;
    mlx4ParseQueryFwOutbox(b, out);
    fLastFw = *out;
    fHaveLastFw = true;
    return true;
}

void Mlx4Hw::rememberQueryFw(const Mlx4QueryFwResult *fw)
{
    if (!fw) return;
    fLastFw = *fw;
    fHaveLastFw = true;
}

bool Mlx4Hw::clrIntFieldsFromQueryFw(Mlx4QueryFwResult *out) const
{
    if (!fHaveLastFw) return false;
    if (out) *out = fLastFw;
    return true;
}

/*
 * QUERY_FW, but with the outbox inside a page of the given ICM area.
 *
 * Identical wire format to queryFw() above: the only change is the address handed
 * to the device as out_param.  That makes it a direct test of whether the device
 * can write into the memory we mapped, which is the question behind the RUN_FW
 * hang; the answer's fields are self-checking, so a garbage reply is obvious.
 */
bool Mlx4Hw::queryFwAt(Mlx4Icm *icm, UInt32 page, Mlx4QueryFwResult *out,
                       UInt32 *outFwStatus, bool ignoreBusy)
{
    UInt8  *b;
    UInt64  bus;
    UInt32  fwStatus = CX3_FWSTATUS_NOT_POSTED;

    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    if (!out || !icm || !icm->virt || page >= icm->npages) {
        LOG("queryFwAt: bad arguments");
        return false;
    }
    memset(out, 0, sizeof(*out));

    bus = icm->pages[page].busAddr;
    b   = (UInt8 *)icm->virt + ((UInt64)page << MLX4_ICM_PAGE_SHIFT);
    memset(b, 0, MLX4_MAILBOX_SIZE);

    if (!cmdBox(0, bus, 0, 0, MLX4_CMD_QUERY_FW, CX3_M1_CMD_TIMEOUT_MS,
                &fwStatus, ignoreBusy)) {
        out->fwStatus = fwStatus;
        out->ok = false;
        if (outFwStatus) *outFwStatus = fwStatus;
        return false;
    }
    out->fwStatus = fwStatus;
    if (outFwStatus) *outFwStatus = fwStatus;
    mlx4ParseQueryFwOutbox(b, out);
    return true;
}

/*
 * mlx4_QUERY_DEV_CAP() -- FreeBSD mlx4_core/mlx4_fw.c:724.
 *
 * Identical command shape to QUERY_FW (4 KiB outbox, no inbox); the work is in
 * the field extraction.  Two details are easy to get wrong:
 *
 *   - the offsets in mlx4_fw.c are BYTE offsets, unlike the dword indices the
 *     HCR uses, so the outbox has to be walked as bytes
 *   - several fields are one byte wide and are packed next to each other, so
 *     they must not be read as dwords
 *
 * The minimum page size is derived the same way the reference derives it:
 * page_size_cap is a mask, and ~mask + 1 is the size.
 */
bool Mlx4Hw::queryDevCap(Mlx4QueryDevCapResult *out, bool bypassBusy)
{
    UInt8  *b;
    UInt32  fwStatus = 0;
    int     i;

    if (!out) return false;
    memset(out, 0, sizeof(*out));

    if (!mboxAlloc()) return false;
    memset(fMboxVirt, 0, MLX4_MAILBOX_SIZE);

    /* mlx4_fw.c:835:
     *   mlx4_cmd_box(dev, 0, mailbox->dma, 0, 0, MLX4_CMD_QUERY_DEV_CAP,
     *                MLX4_CMD_TIME_CLASS_A, MLX4_CMD_NATIVE)  */
    if (!cmdBox(0, fMboxPhys, 0, 0, MLX4_CMD_QUERY_DEV_CAP,
                CX3_M1_CMD_TIMEOUT_MS, &fwStatus, bypassBusy)) {
        out->fwStatus = fwStatus;
        out->ok = false;
        return false;
    }
    out->fwStatus = fwStatus;

    b = (UInt8 *)fMboxVirt;
    for (i = 0; i < MLX4_DEVCAP_WORDS; ++i)
        out->rawWords[i] = mlx4GetBe32(b + i * 4);

    out->maxSrqSzLog2   = b[QUERY_DEV_CAP_MAX_SRQ_SZ_OFFSET];
    out->maxQpSzLog2    = b[QUERY_DEV_CAP_MAX_QP_SZ_OFFSET];
    out->reservedQps    = b[QUERY_DEV_CAP_RSVD_QP_OFFSET];
    out->maxQpLog2      = b[QUERY_DEV_CAP_MAX_QP_OFFSET];
    out->reservedSrqs   = b[QUERY_DEV_CAP_RSVD_SRQ_OFFSET];
    out->maxSrqLog2     = b[QUERY_DEV_CAP_MAX_SRQ_OFFSET];
    out->maxCqSzLog2    = b[QUERY_DEV_CAP_MAX_CQ_SZ_OFFSET];
    out->reservedCqs    = b[QUERY_DEV_CAP_RSVD_CQ_OFFSET];
    out->maxCqLog2      = b[QUERY_DEV_CAP_MAX_CQ_OFFSET];
    out->maxMptLog2     = b[QUERY_DEV_CAP_MAX_MPT_OFFSET];
    out->reservedEqs    = b[QUERY_DEV_CAP_RSVD_EQ_OFFSET];
    out->maxEqLog2      = b[QUERY_DEV_CAP_MAX_EQ_OFFSET];
    out->reservedMtts   = b[QUERY_DEV_CAP_RSVD_MTT_OFFSET];
    out->maxMttSegLog2  = b[QUERY_DEV_CAP_MAX_MTT_SEG_OFFSET];
    out->maxGso         = b[QUERY_DEV_CAP_MAX_GSO_OFFSET];
    out->mtuWidth       = b[QUERY_DEV_CAP_MTU_WIDTH_OFFSET];
    out->maxMsgSzLog2   = b[QUERY_DEV_CAP_MAX_MSG_SZ_OFFSET];
    out->maxGidLog2     = b[QUERY_DEV_CAP_MAX_GID_OFFSET];
    out->reservedUars   = b[QUERY_DEV_CAP_RSVD_UAR_OFFSET];
    out->uarSzLog2      = b[QUERY_DEV_CAP_UAR_SZ_OFFSET];

    out->extFlags    = mlx4GetBe32(b + QUERY_DEV_CAP_EXT_FLAGS_OFFSET);
    out->flags       = mlx4GetBe64(b + QUERY_DEV_CAP_FLAGS_OFFSET);
    out->pageSizeCap = mlx4GetBe32(b + QUERY_DEV_CAP_PAGE_SZ_OFFSET);

    /*
     * min_page_sz is the log2 at 0x4b, not a mask-derived size:
     *     mlx4_fw.c:936   dev_cap->min_page_sz = 1 << field;
     * The version here used to invert page_size_cap, copied from a different
     * driver, and reported a value in the billions.  page_size_cap is still
     * carried because it is what the historical page-size negotiation used.
     */
    out->minPageSize = 1u << b[QUERY_DEV_CAP_PAGE_SZ_OFFSET];

    out->ok = true;
    return true;
}

/* ==================================================================== */
/* M2d: device capabilities, the context-memory profile, INIT_HCA       */
/* ==================================================================== */
/*
 * Everything below is the port of the reference's "bring the HCA's own context
 * memory up, then INIT_HCA" sequence:
 *
 *   mlx4_QUERY_DEV_CAP   mlx4_fw.c:812      -> the capability set
 *   mlx4_make_profile    mlx4_profile.c:69  -> icm_size + where each table goes
 *   mlx4_SET_ICM_SIZE    mlx4_fw.c:2542     -> how much aux memory the HCA wants
 *   mlx4_init_icm        mlx4_main.c:1563   -> allocate and MAP_ICM each table
 *   mlx4_INIT_HCA        mlx4_fw.c:1819     -> hand the layout to the HCA
 *
 * The decode and the profile arithmetic are transcribed rather than
 * re-derived: every one of these numbers ends up in a command, and a plausible
 * wrong value here fails far away from its cause.
 */

/* MLX4_PUT, as byte stores (mlx4_fw.c:72-90 writes big-endian fields). */
static inline void mlx4PutBe16(void *p, UInt16 v)
{
    ((UInt8 *)p)[0] = (UInt8)(v >> 8);
    ((UInt8 *)p)[1] = (UInt8)v;
}
static inline void mlx4PutBe32(void *p, UInt32 v)
{
    ((UInt8 *)p)[0] = (UInt8)(v >> 24);
    ((UInt8 *)p)[1] = (UInt8)(v >> 16);
    ((UInt8 *)p)[2] = (UInt8)(v >> 8);
    ((UInt8 *)p)[3] = (UInt8)v;
}

bool Mlx4Hw::parseDevCap(Mlx4DevCap *out, bool bypassBusy)
{
    Mlx4QueryDevCapResult q;
    UInt8 be[MLX4_DEVCAP_WORDS * 4];

    if (!out) return false;
    memset(out, 0, sizeof(*out));

    /* The command itself, then the pure decode (mlx4_pure.h).  Keeping the decode
     * out of this file is what lets tools/test-m2d-profile.c run it against real
     * outbox bytes on the host, instead of it only ever executing on hardware. */
    if (!queryDevCap(&q, bypassBusy)) return false;
    mlx4DevCapBytes(&q, be);
    mlx4DecodeDevCap(be, out);

    LOG("QUERY_DEV_CAP decoded: %u ports, %u sys EQs, max_icm %llu KiB, "
        "entry sizes qp %u cq %u eq %u srq %u mtt %u dmpt %u cmpt %u rdmarc %u "
        "aux %u altc %u",
        (unsigned)out->num_ports, (unsigned)out->num_sys_eqs,
        (unsigned long long)(out->max_icm_sz >> 10),
        (unsigned)out->qpc_entry_sz, (unsigned)out->cqc_entry_sz,
        (unsigned)out->eqc_entry_sz, (unsigned)out->srq_entry_sz,
        (unsigned)out->mtt_entry_sz, (unsigned)out->dmpt_entry_sz,
        (unsigned)out->cmpt_entry_sz, (unsigned)out->rdmarc_entry_sz,
        (unsigned)out->aux_entry_sz, (unsigned)out->altc_entry_sz);
    LOG("QUERY_DEV_CAP reserved: qps %u cqs %u srqs %u eqs %u mtts %u mrws %u "
        "mgms %u | uar_size %u reserved %u | min_page %u | flags 0x%llx flags2 0x%x",
        (unsigned)out->reserved_qps, (unsigned)out->reserved_cqs,
        (unsigned)out->reserved_srqs, (unsigned)out->reserved_eqs,
        (unsigned)out->reserved_mtts, (unsigned)out->reserved_mrws,
        (unsigned)out->reserved_mcg, (unsigned)out->uar_size,
        (unsigned)out->reserved_uars, (unsigned)out->min_page_sz,
        (unsigned long long)out->flags, (unsigned)out->flags2);
    return true;
}

/*
 * mlx4_make_profile() -- FreeBSD mlx4_profile.c:69.
 *
 * Packs the requested object counts into the HCA's context memory.  The
 * reference's method is worth keeping exactly, because the layout is not
 * arbitrary: every resource's size is rounded up to a power of two, the
 * resources are sorted by decreasing size, and then packed end to end with no
 * gaps.  That is what makes "each table is aligned to its own size" true for
 * free, and INIT_HCA then simply receives the resulting offsets.
 *
 * Returns the total size in bytes, or 0 if the profile cannot be satisfied.
 */
UInt64 Mlx4Hw::makeProfile(const Mlx4Profile *request, const Mlx4DevCap *cap,
                           Mlx4InitHcaParam *param, Mlx4IcmTable *tables,
                           UInt32 tableCapacity, UInt32 *numTablesOut)
{
    Mlx4ProfileEntry entries[MLX4_PROFILE_MAX_ENTRIES];
    UInt32 nEntries = 0, i;
    UInt64 total;

    memset(entries, 0, sizeof(entries));
    total = mlx4MakeProfile(request, cap, param, tables, tableCapacity,
                            numTablesOut, entries, MLX4_PROFILE_MAX_ENTRIES,
                            &nEntries);
    for (i = 0; i < nEntries; ++i) {
        if (!entries[i].size) continue;
        LOG("profile[%2u] %-6s: 2^%02u entries @ 0x%09llx, size 0x%09llx",
            (unsigned)i, entries[i].name, (unsigned)entries[i].logNum,
            (unsigned long long)entries[i].start,
            (unsigned long long)entries[i].size);
    }
    if (!total)
        LOG("profile: does not fit the HCA's context memory (max_icm_sz %llu), or "
            "the table list needs more slots than were offered",
            (unsigned long long)(cap ? cap->max_icm_sz : 0));
    else
        LOG("profile: %llu KiB of HCA context memory, %u table(s)",
            (unsigned long long)(total >> 10), (unsigned)(numTablesOut ? *numTablesOut : 0));
    return total;
}

/*
 * mlx4_SET_ICM_SIZE() -- FreeBSD mlx4_fw.c:2542.
 *
 * Tells the HCA how large the context-memory profile is and receives back how
 * much *auxiliary* memory it wants, in 1 MiB units.  The answer arrives in the
 * command register, not in a mailbox, which is why this needs cmdBoxImm.
 */
bool Mlx4Hw::setIcmSize(UInt64 icmSize, UInt64 *auxPages, UInt32 *outFwStatus)
{
    UInt64 imm = 0;
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    bool ok;

    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    if (auxPages) *auxPages = 0;

    ok = cmdBoxImm(icmSize, 0, 0, MLX4_CMD_SET_ICM_SIZE, MLX4_CMD_TIME_CLASS_A,
                   &imm, &status, false);
    if (outFwStatus) *outFwStatus = status;
    if (!ok) {
        LOG("SET_ICM_SIZE(%llu bytes): failed, fw status 0x%x",
            (unsigned long long)icmSize, (unsigned)status);
        return false;
    }
    if (auxPages) *auxPages = imm;
    /*
     * The answer is a count of 4 KiB pages, already in the units the allocator
     * wants -- it used to be reported here as "1 MiB units", which made the
     * caller allocate 256x too much.  Evidence: the reference passes it directly
     * to mlx4_alloc_icm(dev, aux_pages, ...), whose parameter is npages in system
     * pages (mlx4_icm.c:128-200), and prints aux_pages << 2 as KB
     * (mlx4_main.c:1577-1579).  The #if MLX4_ICM_PAGE_SIZE < PAGE_SIZE fixup in
     * mlx4_fw.c:2550-2557 does not apply here, since both are 4 KiB.
     */
    LOG("SET_ICM_SIZE: %llu KiB of context memory needs %llu aux page(s) "
        "(= %llu KiB) [fw status 0x%x]",
        (unsigned long long)(icmSize >> 10), (unsigned long long)imm,
        (unsigned long long)(imm << 2), (unsigned)status);
    return true;
}

/* mlx4_MAP_ICM_AUX / mlx4_UNMAP_ICM_AUX (mlx4_fw.c). */
bool Mlx4Hw::mapIcmAux(Mlx4Icm *icm, UInt32 *outFwStatus)
{
    UInt32 entries = 0;
    bool ok = mapCmd(MLX4_CMD_MAP_ICM_AUX, icm, false, 0, &entries, outFwStatus);
    LOG("MAP_ICM_AUX: %s (%u entries, fw status 0x%x)", ok ? "ok" : "failed",
        (unsigned)entries, (unsigned)(outFwStatus ? *outFwStatus : 0xffffffffu));
    return ok;
}

bool Mlx4Hw::unmapIcmAux(UInt32 *outFwStatus)
{
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    /*
     * The teardown class, not the firmware class: an UNMAP is not a command that may
     * legitimately take a minute, and this is the loop that made one selector hold the
     * transaction lock for 6 min 07.6 s.  CX3_CMD_BUDGET_ICM_MS is the intent behind the
     * old CX3_M1_CMD_TIMEOUT_MS expressed as real milliseconds (5000, not 10.5 s).
     */
    bool ok = cmdBox(0, 0, 0, 0, MLX4_CMD_UNMAP_ICM_AUX, CX3_CMD_BUDGET_ICM_MS, &status);
    if (outFwStatus) *outFwStatus = status;
    LOG("UNMAP_ICM_AUX: %s (fw status 0x%x)", ok ? "ok" : "failed", (unsigned)status);
    return ok;
}

/*
 * mlx4_UNMAP_ICM() -- FreeBSD mlx4_icm.c:305.
 *
 * The virtual address and the size are the command's parameters: in_param is the
 * virtual base and in_modifier the number of 4 KiB ICM pages.
 */
bool Mlx4Hw::unmapIcm(UInt32 npages, UInt64 virt, UInt32 *outFwStatus)
{
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    bool ok = cmdBox(virt, 0, npages, 0, MLX4_CMD_UNMAP_ICM, CX3_CMD_BUDGET_ICM_MS,
                     &status);
    if (outFwStatus) *outFwStatus = status;
    LOG("UNMAP_ICM(virt 0x%llx, %u pages): %s (fw status 0x%x)",
        (unsigned long long)virt, (unsigned)npages, ok ? "ok" : "failed",
        (unsigned)status);
    return ok;
}

/*
 * mlx4_INIT_HCA() -- FreeBSD mlx4_fw.c:1819.
 *
 * The inbox is 0x200 bytes and describes where every context table lives plus
 * which optional features are on.  Two details are easy to get wrong and both
 * matter:
 *
 *   - the flags dword at 0x014 has a host-endianness bit (bit 1) plus bit 0,
 *     and the optional feature bits are ORed into the same dword;
 *   - the EQE/CQE size selection is not a field of its own: it is bit 29 / bit 30
 *     of the dword at 0x058, set only when the HCA advertises 64-byte EQE/CQE.
 *
 * Cache-line size goes in as (ilog2(linesize) - 4) << 5; on x86_64 that is 64.
 */
bool Mlx4Hw::initHca(const Mlx4InitHcaParam *param, const Mlx4DevCap *cap,
                     UInt32 *outFwStatus)
{
    UInt8 *b;
    UInt32 flags = 0, eqeCqe = 0;
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    bool ok;

    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    if (!param || !cap) return false;
    if (!mboxAlloc()) return false;

    b = (UInt8 *)fMboxVirt;
    memset(b, 0, MLX4_MAILBOX_SIZE);
    /* The inbox is 0x200 bytes; refuse rather than silently truncate. */
    if (MLX4_MAILBOX_SIZE < INIT_HCA_IN_SIZE) {
        LOG("INIT_HCA: mailbox is %u bytes, the inbox needs %u",
            (unsigned)MLX4_MAILBOX_SIZE, (unsigned)INIT_HCA_IN_SIZE);
        return false;
    }

    b[INIT_HCA_VERSION_OFFSET] = INIT_HCA_VERSION;
    /* mlx4_fw.c:1889-1890: (ilog2(cache_line_size()) - 4) << 5 */
    b[INIT_HCA_CACHELINE_SZ_OFFSET] = (UInt8)((6 - 4) << 5);   /* 64-byte line */

    /* mlx4_fw.c:1892-1900: little-endian hosts clear bit 1 and set bit 0. */
    flags &= ~(1u << 1);
    flags |= 1u;                                  /* "check UD address vector" */
    if (cap->flags & MLX4_DEV_CAP_FLAG_IPOIB_CSUM)  flags |= (1u << 3);
    if (cap->flags & MLX4_DEV_CAP_FLAG_COUNTERS)    flags |= (1u << 4);
    if (cap->flags & MLX4_DEV_CAP_FLAG_RSS_IP_FRAG) flags |= (1u << 13);
    mlx4PutBe32(b + INIT_HCA_FLAGS_OFFSET, flags);

    /* mlx4_fw.c:1914-1933: 64-byte EQE and CQE are bits in this one dword. */
    if (cap->flags & MLX4_DEV_CAP_FLAG_64B_EQE) eqeCqe |= (1u << 29);
    if (cap->flags & MLX4_DEV_CAP_FLAG_64B_CQE) eqeCqe |= (1u << 30);
    mlx4PutBe32(b + INIT_HCA_EQE_CQE_OFFSETS, eqeCqe);
    /* mlx4_fw.c:1952: a recoverable-error event is bit 31 of the dword at 0x18. */
    if (cap->flags2 & MLX4_DEV_CAP_FLAG2_RECOVERABLE_ERROR_EVENT)
        mlx4PutBe32(b + INIT_HCA_RECOVERABLE_ERROR_EVENT_OFFSET,
                    (1u << 31));

    /* QPC / SRQC / CQC / ALTC / AUXC / EQC / RDMARC. */
    mlx4PutBe64(b + INIT_HCA_QPC_BASE_OFFSET,  param->qpc_base);
    b[INIT_HCA_LOG_QP_OFFSET]    = param->log_num_qps;
    mlx4PutBe64(b + INIT_HCA_SRQC_BASE_OFFSET, param->srqc_base);
    b[INIT_HCA_LOG_SRQ_OFFSET]   = param->log_num_srqs;
    mlx4PutBe64(b + INIT_HCA_CQC_BASE_OFFSET,  param->cqc_base);
    b[INIT_HCA_LOG_CQ_OFFSET]    = param->log_num_cqs;
    mlx4PutBe64(b + INIT_HCA_ALTC_BASE_OFFSET, param->altc_base);
    mlx4PutBe64(b + INIT_HCA_AUXC_BASE_OFFSET, param->auxc_base);
    mlx4PutBe64(b + INIT_HCA_EQC_BASE_OFFSET,  param->eqc_base);
    b[INIT_HCA_LOG_EQ_OFFSET]    = param->log_num_eqs;
    /* u16 on the wire, and it really can exceed 255 (1024 on this card). */
    mlx4PutBe16(b + INIT_HCA_NUM_SYS_EQS_OFFSET, param->num_sys_eqs);
    mlx4PutBe64(b + INIT_HCA_RDMARC_BASE_OFFSET, param->rdmarc_base);
    b[INIT_HCA_LOG_RD_OFFSET]    = param->log_rd_per_qp;

    /* Multicast.  A0 steering: base, entry size, hash half, whole table. */
    mlx4PutBe64(b + INIT_HCA_MC_BASE_OFFSET, param->mc_base);
    mlx4PutBe16(b + INIT_HCA_LOG_MC_ENTRY_SZ_OFFSET, param->log_mc_entry_sz);
    mlx4PutBe16(b + INIT_HCA_LOG_MC_HASH_SZ_OFFSET,  param->log_mc_hash_sz);
    b[INIT_HCA_LOG_MC_TABLE_SZ_OFFSET] = param->log_mc_table_sz;

    /* TPT: dMPT, memory windows, MPT count, MTT, cMPT. */
    mlx4PutBe64(b + INIT_HCA_DMPT_BASE_OFFSET, param->dmpt_base);
    b[INIT_HCA_TPT_MW_OFFSET] = param->mw_enabled;
    b[INIT_HCA_LOG_MPT_SZ_OFFSET] = param->log_mpt_sz;
    mlx4PutBe64(b + INIT_HCA_MTT_BASE_OFFSET,  param->mtt_base);
    mlx4PutBe64(b + INIT_HCA_CMPT_BASE_OFFSET, param->cmpt_base);

    /* UAR. */
    b[INIT_HCA_UAR_PAGE_SZ_OFFSET] = param->uar_page_sz;
    b[INIT_HCA_LOG_UAR_SZ_OFFSET]  = param->log_uar_sz;

    LOG("INIT_HCA inbox: version %u cacheline %u flags 0x%08x eqe/cqe 0x%08x | "
        "qpc 0x%llx/%u srqc 0x%llx/%u cqc 0x%llx/%u eqc 0x%llx/%u sys_eqs %u | "
        "dmpt 0x%llx mtt 0x%llx cmpt 0x%llx | mc 0x%llx/%u/%u/%u | "
        "uar page %u log %u",
        (unsigned)b[INIT_HCA_VERSION_OFFSET], (unsigned)b[INIT_HCA_CACHELINE_SZ_OFFSET],
        (unsigned)flags, (unsigned)eqeCqe,
        (unsigned long long)param->qpc_base, (unsigned)param->log_num_qps,
        (unsigned long long)param->srqc_base, (unsigned)param->log_num_srqs,
        (unsigned long long)param->cqc_base, (unsigned)param->log_num_cqs,
        (unsigned long long)param->eqc_base, (unsigned)param->log_num_eqs,
        (unsigned)param->num_sys_eqs,
        (unsigned long long)param->dmpt_base, (unsigned long long)param->mtt_base,
        (unsigned long long)param->cmpt_base,
        (unsigned long long)param->mc_base, (unsigned)param->log_mc_entry_sz,
        (unsigned)param->log_mc_hash_sz, (unsigned)param->log_mc_table_sz,
        (unsigned)param->uar_page_sz, (unsigned)param->log_uar_sz);

    ok = cmdBox(fMboxPhys, 0, 0, 0, MLX4_CMD_INIT_HCA, MLX4_CMD_TIME_CLASS_C, &status);
    if (outFwStatus) *outFwStatus = status;
    LOG("INIT_HCA: %s (fw status 0x%x)", ok ? "ok" : "failed", (unsigned)status);
    return ok;
}

/* FreeBSD mlx4_CLOSE_HCA(dev, panic=0): no mailbox or input/output parameters. */
bool Mlx4Hw::closeHca(UInt32 *outFwStatus)
{
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    const bool commandOk = cmdBox(0, 0, 0, 0, MLX4_CMD_CLOSE_HCA,
                                  MLX4_CMD_TIME_CLASS_C, &status);
    if (outFwStatus) *outFwStatus = status;
    return commandOk && status == 0;
}

/* ==================================================================== */
/* M3, restart 1: event mode, QUERY_FUNC, MTT, and the event queue       */
/* ==================================================================== */

/*
 * A command in event mode.
 *
 * Same register layout as cmdBox, with two differences: the E bit is set, and
 * the token field carries `token` instead of the poll sentinel.  The HCA then
 * reports completion by writing a CMD EQE into whatever EQ has the CMD event
 * mapped -- which is the whole point, because it makes the completion observable
 * through the EQ DMA path rather than only through the command register.
 */
bool Mlx4Hw::cmdEvent(UInt64 inParam, UInt64 outParam, UInt32 inModifier, UInt8 opMod,
                      UInt16 op, UInt16 token, UInt32 timeoutMs, UInt32 *outFwStatus,
                      UInt32 *outHcrOnPost, UInt32 *outHcrAtEnd)
{
    bool ok;

    if (outHcrOnPost) *outHcrOnPost = 0;
    if (outHcrAtEnd)  *outHcrAtEnd  = 0;
    if (!fCmdLock) {
        if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
        return false;
    }
    IOLockLock(fCmdLock);
    ok = cmdBoxLocked(inParam, outParam, inModifier, opMod, op, timeoutMs, outFwStatus,
                      false, 0, token, true);
    /*
     * Read the command dword straight back.  It is the only way to see what the
     * hardware was actually handed: a wrong shift or a wrong dword would leave E
     * clear, and the HCA would then complete the command by clearing GO without
     * ever writing a completion EQE -- indistinguishable, from the outside, from
     * an EQ that cannot receive.
     */
    if (fHcr) {
        UInt32 onPost = OSSwapBigToHostInt32(readRegRaw(MLX4_HCR_BASE + HCR_STATUS_OFFSET));
        if (outHcrOnPost) *outHcrOnPost = onPost;
        if (outHcrAtEnd)  *outHcrAtEnd  = readHcrStatus();
    }
    IOLockUnlock(fCmdLock);
    LOG("cmdEvent 0x%x token 0x%x: %s (fw status 0x%x); command dword read back = "
        "0x%08x (E bit %s, token field 0x%04x), at end 0x%08x",
        (unsigned)op, (unsigned)token, ok ? "completed" : "did not complete",
        (unsigned)(outFwStatus ? *outFwStatus : 0xffffffffu),
        (unsigned)(outHcrOnPost ? *outHcrOnPost : 0),
        ((outHcrOnPost ? *outHcrOnPost : 0) & (1u << HCR_E_BIT)) ? "SET" : "CLEAR",
        (unsigned)(((outHcrOnPost ? *outHcrOnPost : 0) >> 16) & 0xffff),
        (unsigned)(outHcrAtEnd ? *outHcrAtEnd : 0));
    return ok;
}

/*
 * mlx4_QUERY_FUNC() -- FreeBSD mlx4_fw.c:1739.
 *
 * On a device with system EQs this is how the function learns its own EQ count
 * and UAR allocation, and the reference overwrites what QUERY_DEV_CAP said with
 * these values (mlx4_main.c:2289-2297).  The EQ numbers the driver may use start
 * after reserved_eqs, so this has to be asked before any EQ is created.
 */
bool Mlx4Hw::queryFunc(UInt32 *outMaxEq, UInt32 *outReservedEq, UInt32 *outReservedUar,
                       UInt32 *outFwStatus)
{
    UInt8 *b;
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    bool ok;

    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    if (!mboxAlloc()) return false;
    b = (UInt8 *)fMboxVirt;
    memset(b, 0, MLX4_MAILBOX_SIZE);

    /* mlx4_fw.c:1752: mlx4_cmd_box(dev, 0, mailbox->dma, in_modifier, 0, …) with
     * in_modifier = slave = 0 for a physical function. */
    ok = cmdBox(0, fMboxPhys, 0, 0, MLX4_CMD_QUERY_FUNC, MLX4_CMD_TIME_CLASS_A, &status);
    if (outFwStatus) *outFwStatus = status;
    if (!ok) {
        LOG("QUERY_FUNC: failed (fw status 0x%x)", (unsigned)status);
        return false;
    }

    if (outMaxEq)      *outMaxEq      = mlx4GetBe16(b + QUERY_FUNC_MAX_EQ_OFFSET);
    if (outReservedEq) *outReservedEq = mlx4GetBe16(b + QUERY_FUNC_RSVD_EQS_OFFSET);
    if (outReservedUar) *outReservedUar = b[QUERY_FUNC_RSVD_UARS_OFFSET];

    LOG("QUERY_FUNC: max_eq %u, reserved_eqs %u, reserved_uars %u (fw status 0x%x)",
        (unsigned)(outMaxEq ? *outMaxEq : 0), (unsigned)(outReservedEq ? *outReservedEq : 0),
        (unsigned)(outReservedUar ? *outReservedUar : 0), (unsigned)status);
    return true;
}

/* ------------------------------------------------------------------ */
/* MTT entries                                                          */
/* ------------------------------------------------------------------ */

/*
 * Hand a PD number back.
 *
 * Only meaningful for the failure path of a single allocation, and only because the
 * allocator is a bump pointer: if the number being returned is not the last one handed
 * out, giving it back would hand the same number out twice.  The check says so instead
 * of doing it.
 */
void Mlx4Hw::pdFree(UInt32 pdn)
{
    if (pdn + 1 != fPdNext) {
        LOG("pdFree: PD %u is not the most recent allocation (next would be %u); not "
            "reclaiming it -- a PD number is a software number, so recycling one that "
            "something may still name is worse than burning it",
            (unsigned)pdn, (unsigned)fPdNext);
        return;
    }
    fPdNext = pdn;
    LOG("pdFree: PD %u returned (the MPT step that followed it failed)", (unsigned)pdn);
}

/*
 * HW2SW_MPT -- opcode 0x0f, in_modifier is the hardware index, op_modifier 1 for the
 * form with no output mailbox (mlx4_mr.c:290-295).
 *
 * A failure here is NOT a clean "the entry is ours again": it means the hardware may
 * still own the entry, and the only correct response is to leave the table alone.  The
 * caller distinguishes the two, which is why this returns a bool and the firmware
 * status separately.
 */
bool Mlx4Hw::mrHw2Sw(UInt32 mptIndex, UInt32 *outFwStatus)
{
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    bool ok;

    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    ok = cmdBox(0, 0, mptIndex, 1, MLX4_CMD_HW2SW_MPT, MLX4_CMD_TIME_CLASS_B, &status);
    if (outFwStatus) *outFwStatus = status;
    LOG("mrHw2Sw: HW2SW_MPT %u: %s (fw status 0x%x)", (unsigned)mptIndex,
        ok ? "the entry is back in software's hands" : "FAILED", (unsigned)status);
    return ok;
}

/* ------------------------------------------------------------------ */
/* R4 stage 1: PDs and the physical memory region                       */
/* ------------------------------------------------------------------ */

/*
 * A protection domain is a number, and on a native PF nothing else.
 *
 * mlx4_pd_alloc (mlx4_pd.c:47-56) is a bitmap allocation with no command behind it;
 * the bitmap's reserved prefix is dev->caps.reserved_pds (mlx4_pd.c:122-124), which is
 * the firmware's own share.  So the two ways to get this wrong are handing out a
 * reserved number, or handing out the same number twice -- both silent, both producing
 * a QP that the hardware refuses to resolve.  The range is therefore set from
 * QUERY_DEV_CAP rather than assumed, and a bump pointer is used the same way mttAlloc
 * uses one, with the same caveat: nothing is recycled.
 */
void Mlx4Hw::setPdRange(UInt32 reserved)
{
    fPdReserved = reserved;
    if (fPdNext < reserved) fPdNext = reserved;
    LOG("PD range: the firmware owns 0..%u, this port hands out from %u upward "
        "(MLX4_NUM_PDS %u)", (unsigned)(reserved ? reserved - 1 : 0),
        (unsigned)fPdNext, (unsigned)MLX4_NUM_PDS);
}

bool Mlx4Hw::pdAlloc(UInt32 *outPdn)
{
    if (!outPdn) return false;
    *outPdn = 0;
    if (fPdNext >= MLX4_NUM_PDS) {
        LOG("pdAlloc: no PD left (next would be %u, the card has %u)",
            (unsigned)fPdNext, (unsigned)MLX4_NUM_PDS);
        return false;
    }
    *outPdn = fPdNext++;
    LOG("pdAlloc: PD %u (a software number -- there is no SW2HW_PD on a native PF)",
        (unsigned)*outPdn);
    return true;
}

/*
 * The physical memory region.
 *
 * mlx4_en_main.c:211-219 builds exactly one MR for the whole driver: iova 0, size ~0,
 * LOCAL_READ | LOCAL_WRITE, npages 0, page_shift 0.  With npages 0 the MTT comes back
 * with order < 0 and mlx4_mr_enable (mlx4_mr.c:648-670) sets MLX4_MPT_FLAG_PHYSICAL
 * with mtt_addr = 0: there is no page list, the region IS physical memory, and the
 * entity size is still written from the MTT's page_shift (12).  That is why no MTT
 * entries are needed here and why this function takes no mtt argument.
 *
 * The fields, from mlx4_mr.c:648-670 and the struct at mlx4.h:303-320:
 *
 *   flags     = MIO | REGION | access | PHYSICAL | SW_OWNS
 *   key       = key_to_hw_index(mr->key)      == the raw hardware index
 *   pd_flags  = pd | PD_FLAG_EN_INV
 *   start     = iova, length = size
 *   entity_size = page_shift (12), not 4096
 *   mtt_addr  = 0 for a physical region
 *
 * SW2HW_MPT's in_modifier is the hardware index masked to the table size
 * (mlx4_mr.c:671-673), and in_param is the mailbox holding this entry.
 */
bool Mlx4Hw::mrEnablePhysical(UInt32 mptIndex, UInt32 pdn, UInt64 iova, UInt64 size,
                              UInt32 access, Mlx4DmaGeneration *owner, UInt32 *outFwStatus)
{
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    UInt32 flags;
    UInt8 *b;

    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    if (!mboxAlloc()) { LOG("mrEnable: no command mailbox"); return false; }

    b = (UInt8 *)fMboxVirt;
    memset(b, 0, MLX4_MAILBOX_SIZE);

    flags = MLX4_MPT_FLAG_MIO | MLX4_MPT_FLAG_REGION | access |
            MLX4_MPT_FLAG_PHYSICAL | MLX4_MPT_FLAG_SW_OWNS;
    mlx4PutBe32(b + MPT_ENTRY_FLAGS_OFFSET, flags);
    mlx4PutBe32(b + MPT_ENTRY_KEY_OFFSET, mptIndex);
    mlx4PutBe32(b + MPT_ENTRY_PD_FLAGS_OFFSET, (pdn & MLX4_MPT_PD_MASK) |
                                               MLX4_MPT_PD_FLAG_EN_INV);
    mlx4PutBe64(b + MPT_ENTRY_START_OFFSET, iova);
    mlx4PutBe64(b + MPT_ENTRY_LENGTH_OFFSET, size);
    mlx4PutBe32(b + MPT_ENTRY_ENTITY_SIZE_OFFSET, MLX4_ICM_PAGE_SHIFT);
    mlx4PutBe64(b + MPT_ENTRY_MTT_ADDR_OFFSET, 0);      /* physical: no page list */

    LOG("mrEnable: MPT %u (key 0x%08x, lkey for a WQE 0x%08x): flags 0x%08x "
        "(MIO|REGION|access 0x%x|PHYSICAL|SW_OWNS), pd_flags 0x%08x (pd %u | EN_INV), "
        "iova 0x%llx, size 0x%llx, entity_size %u, mtt_addr 0 (physical)",
        (unsigned)mptIndex, (unsigned)mptIndex, (unsigned)mlx4HwIndexToKey(mptIndex),
        (unsigned)flags, (unsigned)access,
        (unsigned)((pdn & MLX4_MPT_PD_MASK) | MLX4_MPT_PD_FLAG_EN_INV), (unsigned)pdn,
        (unsigned long long)iova, (unsigned long long)size,
        (unsigned)MLX4_ICM_PAGE_SHIFT);

    if (!markResourceHandoff(owner)) {
        LOG("mrEnable: refusing SW2HW_MPT %u because its owner has no valid provider generation",
            (unsigned)mptIndex);
        return false;
    }
    if (!cmdBox(fMboxPhys, 0, mptIndex, 0, MLX4_CMD_SW2HW_MPT,
                MLX4_CMD_TIME_CLASS_B, &status)) {
        if (outFwStatus) *outFwStatus = status;
        LOG("mrEnable: SW2HW_MPT %u failed (fw status 0x%x)", (unsigned)mptIndex,
            (unsigned)status);
        return false;
    }
    if (outFwStatus) *outFwStatus = status;
    LOG("mrEnable: SW2HW_MPT %u accepted (fw status 0x%x)", (unsigned)mptIndex,
        (unsigned)status);
    return true;
}

/*
 * Read an MPT entry back out of the dMPT table.
 *
 * The table is ICM memory this port already mapped (the M2d profile's 14 tables include
 * "dMPT"), so this is a plain host load and needs no command.  It is the same kind of
 * check as the MTT readback in the EQ work -- it shows what the hardware made of what
 * was sent instead of repeating what was sent.  The specific thing to look for is the
 * status nibble of flags: the driver writes SW_OWNS (0xf), and a firmware that accepted
 * the entry replaces it with its own value.
 *
 * Only chunk 0 is used, and the whole table has to fit in it.  That is true for this
 * card -- 2048 entries of 64 bytes is 128 KiB against a 256 KiB chunk -- but it is
 * asserted rather than assumed, because reading the wrong chunk's memory would look
 * exactly like a valid answer.
 */
void Mlx4Hw::setMptTable(void *hostBase, UInt64 bytes, UInt32 objSize)
{
    fMptBase    = hostBase;
    fMptBytes   = bytes;
    fMptObjSize = objSize;
    LOG("dMPT table view: base %p, %llu bytes, %u bytes per entry (%llu entries)%s",
        hostBase, (unsigned long long)bytes, (unsigned)objSize,
        (unsigned long long)(objSize ? bytes / objSize : 0),
        (bytes > (UInt64)MLX4_TABLE_CHUNK_SIZE)
            ? " -- WARNING: spans more than one 256 KiB chunk, and the readback below "
              "only handles the first" : "");
}

bool Mlx4Hw::mptEntryRead(UInt32 mptIndex, UInt8 *out, UInt32 need)
{
    UInt64 off;

    if (!out || need == 0 || fMptObjSize == 0) return false;
    if (fMptBase == 0) {
        LOG("mptEntryRead: the dMPT table is not mapped (run m2dprofile/m2dbringup)");
        return false;
    }
    if (fMptBytes > (UInt64)MLX4_TABLE_CHUNK_SIZE) {
        LOG("mptEntryRead: the dMPT table is %llu bytes, past the one chunk this "
            "readback covers", (unsigned long long)fMptBytes);
        return false;
    }
    off = (UInt64)mptIndex * fMptObjSize;
    if (off + need > fMptBytes) {
        LOG("mptEntryRead: entry %u (%llu bytes) is past the table's %llu bytes",
            (unsigned)mptIndex, (unsigned long long)off, (unsigned long long)fMptBytes);
        return false;
    }
    memcpy(out, (const UInt8 *)fMptBase + off, need);
    return true;
}

/*
 * QUERY_MPT -- opcode 0x0e, outbox, no inbox, in_modifier is the hardware index
 * (cmd.h:83; the shape is corroborated by mlx4_mr_hw_get_mpt at mlx4_mr.c:323-330 and
 * by the command table entry at mlx4_cmd.c:1229-1236: has_inbox false, has_outbox true).
 *
 * The reference only sends it for fast-register MRs and for slaves, so its answer is
 * not something the production path here depends on -- but it is the only way to ask
 * the firmware what it thinks it stored, as opposed to what happens to be sitting in
 * the host-memory copy of the table.
 */
bool Mlx4Hw::queryMpt(UInt32 mptIndex, UInt8 *out, UInt32 need, UInt32 *outFwStatus)
{
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;

    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    if (!out || need == 0 || need > MPT_ENTRY_SIZE) return false;
    if (!mboxAlloc()) { LOG("queryMpt: no command mailbox"); return false; }

    memset(fMboxVirt, 0, MLX4_MAILBOX_SIZE);
    if (!cmdBox(0, fMboxPhys, mptIndex, 0, MLX4_CMD_QUERY_MPT,
                MLX4_CMD_TIME_CLASS_B, &status)) {
        if (outFwStatus) *outFwStatus = status;
        LOG("queryMpt: MPT %u: no answer (fw status 0x%x)", (unsigned)mptIndex,
            (unsigned)status);
        return false;
    }
    if (outFwStatus) *outFwStatus = status;
    memcpy(out, fMboxVirt, need);
    LOG("queryMpt: MPT %u: fw status 0x%x, first 4 bytes %02x %02x %02x %02x",
        (unsigned)mptIndex, (unsigned)status, out[0], out[1], out[2], out[3]);
    return true;
}


bool Mlx4Hw::setMttTable(void *hostBase, UInt64 bytes, UInt32 reservedEntries,
                         UInt64 hcaVirt)
{
    /* A/B's synthetic offset reservation is never exposed to the device. Reclaim
     * it when it is the only remaining MTT range so a later INIT_HCA/table view
     * can proceed; live DMA owners still make the ordinary guard below fail. */
    if (fMttAllocator.abReservationTracked &&
        !fMttAllocator.releaseAbReservationIfUnowned()) {
        LOG("MTT table view: A/B dummy reservation remains pinned behind live MTT owners");
    }
    if (fMttAllocator.allocationCount != 0) {
        LOG("MTT table view refused: %u allocation owner record(s) remain; resetting the "
            "allocator would make old indices reusable", (unsigned)fMttAllocator.allocationCount);
        return false;
    }
    if (!hostBase || bytes < MLX4_MTT_ENTRY_SIZE ||
        reservedEntries > bytes / MLX4_MTT_ENTRY_SIZE) {
        LOG("MTT table view refused: invalid base/size/reserved prefix (%p, %llu bytes, %u entries)",
            hostBase, (unsigned long long)bytes, (unsigned)reservedEntries);
        return false;
    }
    fMttBase     = hostBase;
    fMttBytes    = bytes;
    fMttHcaVirt  = hcaVirt;
    fMttReserved = reservedEntries;
    /* Allocation starts after the reserved prefix, which the firmware owns
     * (mlx4_main.c:1612-1620 rounds it up to a cache line for exactly this
     * reason). */
    fMttAllocator.init(reservedEntries);
    LOG("MTT table view: base %p, %llu bytes (%llu entries), reserved prefix %u entries",
        hostBase, (unsigned long long)bytes,
        (unsigned long long)(bytes / MLX4_MTT_ENTRY_SIZE), (unsigned)reservedEntries);
    return true;
}

/*
 * Hand out `npages` consecutive MTT entries and report the byte address the HCA
 * should be given (offset * entry size, mlx4_mr.c mlx4_mtt_addr).
 *
 * The first chunk of the MTT table -- 256 KiB, 32768 entries -- is mapped in
 * full by the M2d bring-up, so the entries are ordinary host memory and the
 * write below is a store, not a command (mlx4_mr.c:697-721 is the native path).
 * Allocation starts above the reserved prefix and is handed back on release.
 */
bool Mlx4Hw::mttAlloc(UInt32 npages, Mlx4Icm *owner, UInt32 *outOffsetEntries,
                      UInt32 *outMTTAddr)
{
    UInt32 offset;

    if (outOffsetEntries) *outOffsetEntries = 0;
    if (outMTTAddr) *outMTTAddr = 0;
    if (npages == 0) return false;
    if (fMttBase == 0 || fMttBytes == 0) {
        LOG("mttAlloc: the MTT table is not mapped (run m2dprofile/m2dbringup first)");
        return false;
    }

    if (owner) {
        if (!owner->allocationId || !owner->generation.validAllocation() ||
            owner->generation.allocationGeneration != fDeviceGeneration ||
            !fMttAllocator.allocateOwned(npages, fMttBytes / MLX4_MTT_ENTRY_SIZE, owner,
                                         owner->allocationId,
                                         owner->generation.allocationGeneration, &offset)) {
            LOG("mttAlloc: refusing owner with missing/stale allocation identity or no table room");
            return false;
        }
    } else if (!fMttAllocator.allocateReservation(npages,
                                                  fMttBytes / MLX4_MTT_ENTRY_SIZE,
                                                  fDeviceGeneration, &offset)) {
        if (fMttAllocator.allocationCount == Mlx4MttAllocator::kAllocationMax) {
            LOG("mttAlloc: the exact owner ledger is full (%u allocations); refusing %u more "
                "entries without changing the high-water mark",
                (unsigned)fMttAllocator.allocationCount, (unsigned)npages);
        } else {
            LOG("mttAlloc: %u entries from %u would pass the mapped %llu bytes of MTT table "
                "(reserved prefix is %u)",
                (unsigned)npages, (unsigned)fMttAllocator.high,
                (unsigned long long)fMttBytes, (unsigned)fMttReserved);
        }
        return false;
    }

    if (outOffsetEntries) *outOffsetEntries = offset;
    /*
     * The address handed to a queue context is offset * entry size, and the context carries it
     * as mtt_base_addr_h (a byte) plus mtt_base_addr_l (32 bits) -- so returning it in 32 bits
     * is by construction, not a loss.  The cast states that; without it this was another
     * silent narrowing that -Wshorten-64-to-32 flags.
     */
    if (outMTTAddr) *outMTTAddr = (UInt32)((UInt64)offset * MLX4_MTT_ENTRY_SIZE);
    LOG("mttAlloc: %u entries at offset %u (HCA address 0x%llx), table 0x%llx + %llu bytes",
        (unsigned)npages, (unsigned)offset,
        (unsigned long long)((UInt64)offset * MLX4_MTT_ENTRY_SIZE),
        (unsigned long long)fMttBase, (unsigned long long)fMttBytes);
    return true;
}

bool Mlx4Hw::mttFree(UInt32 offsetEntries, UInt32 npages, Mlx4Icm *owner)
{
    if (!owner || !owner->allocationId || !owner->generation.validAllocation() ||
        !fMttAllocator.releaseOwned(offsetEntries, npages, owner, owner->allocationId,
                                    owner->generation.allocationGeneration,
                                    owner->generation.ownerGeneration)) {
        LOG("mttFree: refusing invalid, stale, wrong-owner, duplicate, partial, or untracked "
            "free for offset %u pages %u owner %p id %llu; high-water mark remains %u",
            (unsigned)offsetEntries, (unsigned)npages, owner,
            (unsigned long long)(owner ? owner->allocationId : 0),
            (unsigned)fMttAllocator.high);
        return false;
    }
    return true;
}

bool Mlx4Hw::mttAllocAbDummy(UInt32 npages, UInt32 *outOffsetEntries,
                             UInt32 *outMTTAddr)
{
    UInt32 offset = 0;
    if (outOffsetEntries) *outOffsetEntries = 0;
    if (outMTTAddr) *outMTTAddr = 0;
    if (!npages || !fDeviceGeneration || !fMttBase || !fMttBytes ||
        !fMttAllocator.canRunAbExperiment(npages, fDeviceGeneration) ||
        !fMttAllocator.allocateAbReservation(npages,
                                             fMttBytes / MLX4_MTT_ENTRY_SIZE,
                                             fDeviceGeneration, &offset)) {
        LOG("mttAllocAbDummy: refusing unbound, stale, mismatched, or out-of-range table reservation"
            " (%u entries, generation %llu)", (unsigned)npages,
            (unsigned long long)fDeviceGeneration);
        return false;
    }
    if (outOffsetEntries) *outOffsetEntries = offset;
    if (outMTTAddr) *outMTTAddr = (UInt32)((UInt64)offset * MLX4_MTT_ENTRY_SIZE);
    LOG("mttAllocAbDummy: tracked %u-entry reusable A/B reservation at offset %u, generation %llu",
        (unsigned)npages, (unsigned)offset, (unsigned long long)fDeviceGeneration);
    return true;
}

bool Mlx4Hw::hasAbRetainedRings() const
{
    return fAbRetainedRings[0].valid() || fAbRetainedRings[1].valid();
}

/*
 * Retry only the A/B owners that this Mlx4Hw instance still owns. A ring that may
 * have reached the HCA is returned before its MTT range or ICM allocation is
 * released. A successful HW2SW is remembered so a later MTT-ledger refusal does
 * not repeat the device command. Generation drift is fail-closed: the old EQN is
 * not sent to a different HCA generation.
 */
bool Mlx4Hw::retryAbRetainedRings()
{
    bool releasedAll = true;
    for (int slot = 1; slot >= 0; --slot) {
        Mlx4AbRingRetention &retained = fAbRetainedRings[slot];
        if (!retained.valid()) continue;
        if (!retained.pages || !retained.generation ||
            retained.generation != fDeviceGeneration) {
            LOG("abring retry: retaining slot %d owner %p; saved generation %llu does not "
                "match current generation %llu", slot, retained.icm,
                (unsigned long long)retained.generation,
                (unsigned long long)fDeviceGeneration);
            releasedAll = false;
            continue;
        }
        if (retained.fwMayHold) {
            UInt32 status = CX3_FWSTATUS_NOT_POSTED;
            if (!eqHw2Sw(retained.eqn, &status)) {
                LOG("abring retry: HW2SW_EQ failed for retained slot %d EQN %u (status 0x%x); "
                    "owner and MTT range remain pinned", slot, (unsigned)retained.eqn,
                    (unsigned)status);
                releasedAll = false;
                continue;
            }
            retained.fwMayHold = false;
            LOG("abring retry: HW2SW_EQ succeeded for retained slot %d EQN %u; retrying "
                "exact MTT release without repeating handback", slot,
                (unsigned)retained.eqn);
        }
        Mlx4Icm *ring = (Mlx4Icm *)retained.icm;
        if (!mttFree(retained.mttOffset, retained.pages, ring)) {
            LOG("abring retry: exact MTT release refused for retained slot %d offset %u pages "
                "%u; ICM owner remains reachable", slot, (unsigned)retained.mttOffset,
                (unsigned)retained.pages);
            releasedAll = false;
            continue;
        }
        icmFree(ring);
        retained.clear();
    }
    if (!hasAbRetainedRings()) fMttAllocator.releaseAbReservationIfUnowned();
    return releasedAll && !hasAbRetainedRings();
}

/*
 * Write the page list into the MTT table, one entry per page, with the "present"
 * flag in the low bit -- mlx4_write_mtt_chunk (mlx4_mr.c:697-721):
 *
 *     mtts[i] = cpu_to_be64(page_list[i] | MLX4_MTT_FLAG_PRESENT);
 */
bool Mlx4Hw::mttWrite(UInt32 offsetEntries, UInt32 npages, const UInt64 *busAddrs)
{
    UInt8 *base;
    UInt32 i;

    if (!busAddrs || npages == 0) return false;
    if (fMttBase == 0) {
        LOG("mttWrite: the MTT table is not mapped");
        return false;
    }
    if ((UInt64)(offsetEntries + npages) * MLX4_MTT_ENTRY_SIZE > fMttBytes) {
        LOG("mttWrite: entries %u..%u are past the mapped MTT table",
            (unsigned)offsetEntries, (unsigned)(offsetEntries + npages));
        return false;
    }

    base = (UInt8 *)fMttBase;
    for (i = 0; i < npages; ++i) {
        /*
         * The bits below 12 are ORed with the present flag, so the bus address
         * has to be page aligned for the two to combine -- which icmAlloc
         * already guarantees, and which the flag's position requires.
         */
        if (busAddrs[i] & 0xfffull) {
            LOG("mttWrite: page %u bus address 0x%llx is not 4 KiB aligned",
                (unsigned)i, (unsigned long long)busAddrs[i]);
            return false;
        }
        mlx4PutBe64(base + (UInt64)(offsetEntries + i) * MLX4_MTT_ENTRY_SIZE,
                    busAddrs[i] | MLX4_MTT_FLAG_PRESENT);
    }
    LOG("mttWrite: %u entries at MTT offset %u (first 0x%llx, last 0x%llx)",
        (unsigned)npages, (unsigned)offsetEntries,
        (unsigned long long)busAddrs[0],
        (unsigned long long)busAddrs[npages - 1]);
    return true;
}

/* ------------------------------------------------------------------ */
/* R4 stage 2: the completion queue                                     */
/* ------------------------------------------------------------------ */

/*
 * cqNextCqe -- the readiness rule, expressed exactly as the reference's consumer does.
 *
 * mlx4_en_rx.c:768:
 *
 *     while (XNOR(cqe->owner_sr_opcode & MLX4_CQE_OWNER_MASK, cons_index & size))
 *
 * with XNOR(a,b) = (!a == !b) (en.h:236) and size = the ring's entry count.  So a slot
 * is ready when the owner bit equals the phase bit, exactly as the EQ side does it --
 * and, as there, "the owner says hardware" on its own would report a freshly enabled
 * ring as full.
 *
 * The slot's real CQE is the second 32-byte half, hence CQE_SLOT_OWNER_BYTE, and the
 * consumer only ever looks at the slot the persistent index names.
 */
bool Mlx4Hw::cqNextCqe(const Mlx4Icm *ring, UInt32 nent, UInt8 cqeFactor,
                       UInt32 consIndex, UInt8 *outCqeLegacy)
{
    UInt32 stride = CQE_LEGACY_SIZE << cqeFactor;      /* 32 or 64 */
    UInt32 slot   = consIndex & (nent - 1);
    UInt64 off    = (UInt64)slot * stride + (cqeFactor ? CQE_LEGACY_SIZE : 0u);
    const UInt8 *cqe;
    UInt32 owner, phase;

    if (!ring || !ring->virt || !outCqeLegacy) return false;
    if (off + CQE_LEGACY_SIZE > ring->bytes) return false;

    cqe   = (const UInt8 *)ring->virt + off;
    owner = (cqe[CQE_OWNER_OFFSET] & MLX4_CQE_OWNER_MASK) ? 1u : 0u;
    phase = (consIndex & nent) ? 1u : 0u;

    if (owner != phase) return false;
    /* Same barrier reasoning as eqNextEqe: the hardware writes the payload and the
     * owner byte as separate stores and only the owner byte is the handshake. */
    __asm__ __volatile__("" ::: "memory");
    memcpy(outCqeLegacy, cqe, CQE_LEGACY_SIZE);
    return true;
}

/*
 * cqCreate -- mlx4_cq_alloc (mlx4_cq.c:237-291) reduced to what one RX CQ needs.
 *
 * Two fields in the context are the ones to get right, and both reviews of this stage
 * named them as the places where a mistake produces "completions never arrive" rather
 * than an error:
 *
 *   logsize_usrpage  = (ilog2(nent) << 24) | mlx4_to_hw_uar_index(uar->index)
 *                      (mlx4_cq.c:274-276).  The index is the 128th UAR and up; the
 *                      first 128 belong to the EQ doorbells, so putting a CQ in there
 *                      would land it on the doorbell that is already known to work.
 *
 *   db_rec_addr      = the DMA BYTE ADDRESS of the completion-index record
 *                      (mlx4_cq.c:284).  Not an offset, not a UAR address, not shifted.
 *
 * And one thing outside the context: the OWNER BYTE OF EVERY SLOT has to be preset to
 * 0x80 by the driver before the CQ is used (mlx4_en_netdev.c:1306-1307).  Unlike the EQ
 * ring -- whose owner bits the firmware sets while it accepts the queue, measured in
 * section 21.3 -- nothing does this for a CQ.  See the note at the preset loop for why
 * this port writes every slot rather than copying the reference's loop.
 */
bool Mlx4Hw::cqCreate(UInt32 cqn, UInt32 nent, UInt8 eqn, UInt32 uarIndex,
                      Mlx4Icm **outRing, UInt32 *outPages, UInt32 *outMttOffset,
                      UInt32 *outMttAddr, UInt64 *outDbRecordPhys, Mlx4Icm **outRec,
                      UInt32 *outOwnerBefore, UInt32 *outOwnerAfter,
                      UInt32 *outFwStatus)
{
    const UInt8 cqeFactor = 1;                      /* 64-byte CQEs on this card */
    const UInt32 stride   = CQE_LEGACY_SIZE << cqeFactor;
    UInt32 pages, mttOffset = 0, mttAddr = 0, status = CX3_FWSTATUS_NOT_POSTED;
    UInt64 *list = 0;
    Mlx4Icm *ring = 0, *rec = 0;
    UInt8 *ctx;
    UInt32 i;
    bool ok = false;
    const auto exposeRetainedOwner = [&]() {
        if (outRing) *outRing = ring;
        if (outPages) *outPages = pages;
        if (outMttOffset) *outMttOffset = mttOffset;
        if (outMttAddr) *outMttAddr = mttAddr;
        if (outDbRecordPhys) *outDbRecordPhys = rec ? rec->pages[0].busAddr : 0;
        if (outRec) *outRec = rec;
    };

    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    if (outRing) *outRing = 0;
    if (outPages) *outPages = 0;
    if (outMttOffset) *outMttOffset = 0;
    if (outMttAddr) *outMttAddr = 0;
    if (outDbRecordPhys) *outDbRecordPhys = 0;
    if (outRec) *outRec = 0;

    /* nent must be a power of two: readiness uses `cons_index & nent` as the phase. */
    { UInt32 n = 1; while (n < nent) n <<= 1; nent = n; }
    pages = (nent * stride + (MLX4_ICM_PAGE_SIZE - 1)) >> MLX4_ICM_PAGE_SHIFT;
    if (pages == 0) return false;

    /* 1. the ring */
    ring = icmAlloc(pages, false);
    if (!ring) {
        LOG("cqCreate: could not allocate %u pages for a %u-entry CQ ring",
            (unsigned)pages, (unsigned)nent);
        return false;
    }
    /*
     * The clear, then the owner preset -- and the preset is the part that is easy to get
     * subtly wrong.
     *
     * The reference memsets the buffer (mlx4_en_cq.c:126) and then presets owners with
     *
     *     for (j = 0; j < cq->size; j++)
     *             cq->buf[j].owner_sr_opcode = MLX4_CQE_OWNER_MASK;
     *
     * and `cq->buf` is a `struct mlx4_cqe *`, i.e. 32 bytes, while `buf_size` is
     * `cq->size * cqe_size` = size * 64 on this card.  So that loop walks the first HALF
     * of the buffer while the reader looks at the real CQE halves, which sit at
     * i * 64 + 63; the two coincide only for i < size/2.
     *
     * Two corrections to what this comment used to say, both from measurement:
     *
     * 1. WHERE the preset goes.  The reference clears in mlx4_en_activate_cq
     *    (mlx4_en_cq.c:126), allocates the CQ -- which is where SW2HW_CQ happens --
     *    at :157, and only then, back in the caller, presets owners
     *    (mlx4_en_netdev.c:1306-1307).  So its order is clear -> hand over -> preset.
     *    This port did clear -> preset -> hand over, and the firmware writes the ring
     *    while it accepts the queue (measured: 0xcc throughout), so the preset was
     *    being overwritten and did nothing.  The preset now runs after the handover,
     *    where it survives and where the reference puts it.
     *
     * 2. WHY the missed half is not the disaster this comment used to predict.  It said
     *    the second half would keep the clear's zeros and read as ready.  It does not:
     *    the firmware fills the ring, and every fill byte seen so far has bit 7 set
     *    (0xcc, 0xee), which is the same state the preset writes.  So the reference's
     *    half-covering loop is incomplete in intent but harmless in practice -- on a
     *    firmware that fills the ring.  That is a reason to write every slot rather than
     *    depend on the fill, not a reason to call the reference broken.
     *
     * The failure this guards against is still worth guarding against: a slot whose owner
     * byte ends up zero reads as ready on the first lap, so the consumer walks slots
     * nobody wrote and reports zeroed memory as a completion.
     */
    memset(ring->virt, 0, (size_t)ring->bytes);
    mlx4DumpBytes("cqCreate: right after the clear, before SW2HW_CQ",
                  (const UInt8 *)ring->virt, 32);

    /* 2. the completion-index record: two adjacent 32-bit words of host DMA memory
     *    (mlx4_alloc.c:717-726 allocates order 1 for exactly this). */
    rec = icmAlloc(1, false);
    if (!rec) { LOG("cqCreate: could not allocate the CI record"); icmFree(ring); return false; }
    memset(rec->virt, 0, (size_t)rec->bytes);

    /* 3. MTT entries for the ring's pages */
    if (!mttAlloc(pages, ring, &mttOffset, &mttAddr)) {
        LOG("cqCreate: no MTT entries for %u pages", (unsigned)pages);
        icmFree(rec); icmFree(ring);
        return false;
    }
    list = (UInt64 *)IOMalloc(sizeof(UInt64) * pages);
    if (!list) {
        if (!mttFree(mttOffset, pages, ring)) { exposeRetainedOwner(); return false; }
        icmFree(rec); icmFree(ring); return false;
    }
    for (i = 0; i < pages; ++i) list[i] = ring->pages[i].busAddr;
    if (!mttWrite(mttOffset, pages, list)) {
        LOG("cqCreate: the MTT write failed");
        IOFree(list, sizeof(UInt64) * pages);
        if (!mttFree(mttOffset, pages, ring)) { exposeRetainedOwner(); return false; }
        icmFree(rec); icmFree(ring);
        return false;
    }
    IOFree(list, sizeof(UInt64) * pages);
    list = 0;

    /* 4. the context */
    if (!mboxAlloc()) {
        if (!mttFree(mttOffset, pages, ring)) { exposeRetainedOwner(); return false; }
        icmFree(rec); icmFree(ring); return false;
    }
    ctx = (UInt8 *)fMboxVirt;
    memset(ctx, 0, MLX4_MAILBOX_SIZE);
    mlx4PutBe32(ctx + CQ_CONTEXT_LOGSIZE_USRPAGE_OFFSET,
                (mlx4Log2(nent) << 24) | mlx4ToHwUarIndex(uarIndex));
    ctx[CQ_CONTEXT_COMP_EQN_OFFSET]      = eqn;
    ctx[CQ_CONTEXT_LOG_PAGE_SIZE_OFFSET] = 0;       /* host page == ICM page */
    ctx[CQ_CONTEXT_MTT_BASE_H_OFFSET]    = (UInt8)((UInt64)mttAddr >> 32);
    mlx4PutBe32(ctx + CQ_CONTEXT_MTT_BASE_L_OFFSET, mttAddr & 0xffffffffu);
    mlx4PutBe64(ctx + CQ_CONTEXT_DB_REC_ADDR_OFFSET, rec->pages[0].busAddr);

    LOG("cqCreate: CQN %u: context logsize_usrpage 0x%08x (logsize %u, hw uar index %u), "
        "comp_eqn %u, mtt_base 0x%x, db_rec_addr 0x%llx (the record's DMA BYTE address), "
        "uar page at BAR2+0x%llx, CQ doorbell at BAR2+0x%llx",
        (unsigned)cqn, (unsigned)((mlx4Log2(nent) << 24) | mlx4ToHwUarIndex(uarIndex)),
        (unsigned)mlx4Log2(nent), (unsigned)mlx4ToHwUarIndex(uarIndex), (unsigned)eqn,
        (unsigned)mttAddr, (unsigned long long)rec->pages[0].busAddr,
        mlx4UarPageOffset(uarIndex),
        mlx4UarPageOffset(uarIndex) + MLX4_CQ_DOORBELL);

    {
        Mlx4Icm *handoff[2] = { ring, rec };
        if (!markDmaHandoff(handoff, 2)) {
            LOG("cqCreate: refusing SW2HW_CQ because CQ ring/record generation is unbound");
            if (!mttFree(mttOffset, pages, ring)) { exposeRetainedOwner(); return false; }
            icmFree(rec); icmFree(ring);
            if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
            return false;
        }
    }
    ok = cmdBox(fMboxPhys, 0, cqn, 0, MLX4_CMD_SW2HW_CQ, MLX4_CMD_TIME_CLASS_A, &status);
    if (outFwStatus) *outFwStatus = status;
    if (!ok) {
        if (status == CX3_FWSTATUS_TIMEOUT) {
            LOG("cqCreate: SW2HW_CQ for CQN %u TIMED OUT -- the hardware may have taken "
                "the context, so nothing is freed (fw status 0x%x)", (unsigned)cqn,
                (unsigned)status);
            /* Return ownership to the controller even though creation did not
             * complete.  A timeout is ambiguous, so teardown must be able to
             * retry HW2SW_CQ; these outputs are ownership metadata, not a usable CQ. */
            exposeRetainedOwner();
            return false;                            /* deliberately leaks, as eqCreate does */
        }
        LOG("cqCreate: SW2HW_CQ failed for CQN %u (fw status 0x%x)", (unsigned)cqn,
            (unsigned)status);
        if (!mttFree(mttOffset, pages, ring)) { exposeRetainedOwner(); return false; }
        icmFree(rec);
        icmFree(ring);
        return false;
    }

    /*
     * What the ring holds BEFORE the preset, by exact value.
     *
     * A count of "slots with bit 7 set" cannot answer "did my preset run", because every
     * fill byte this firmware has been seen to use has bit 7 set (0xcc, and 0xee on the EQ
     * side) -- the same caveat the EQ owner scan already carries.  Counting the distinct
     * values can: a ring full of 0xcc means the fill is what set the bit, and a ring full
     * of 0x80 means the preset did.
     */
    {
        UInt32 v80 = 0, vCC = 0, vEE = 0, v00 = 0, vOther = 0;
        const UInt8 *p = (const UInt8 *)ring->virt;
        for (i = 0; i < nent; ++i) {
            UInt8 v = p[(UInt64)i * stride + CQE_SLOT_OWNER_BYTE(cqeFactor)];
            if (v == 0x80) ++v80; else if (v == 0xcc) ++vCC; else if (v == 0xee) ++vEE;
            else if (v == 0x00) ++v00; else ++vOther;
        }
        if (outOwnerBefore) {
            outOwnerBefore[0] = v80; outOwnerBefore[1] = vCC; outOwnerBefore[2] = vEE;
            outOwnerBefore[3] = v00; outOwnerBefore[4] = vOther;
        }
        LOG("cqCreate: owner byte across the ring BEFORE the preset: 0x80 x%u, 0xcc x%u, "
            "0xee x%u, 0x00 x%u, other x%u", (unsigned)v80, (unsigned)vCC, (unsigned)vEE,
            (unsigned)v00, (unsigned)vOther);
    }

    /*
     * The preset, now that the hardware has taken the queue and written whatever it
     * writes.  Every slot, not half -- see the note at the clear above for why the
     * reference's loop is narrower and why this does not copy it.
     */
    for (i = 0; i < nent; ++i)
        ((UInt8 *)ring->virt)[(UInt64)i * stride + CQE_SLOT_OWNER_BYTE(cqeFactor)] =
            MLX4_CQE_OWNER_MASK;
    {
        UInt32 v80 = 0, vOther = 0;
        const UInt8 *p = (const UInt8 *)ring->virt;
        for (i = 0; i < nent; ++i) {
            UInt8 v = p[(UInt64)i * stride + CQE_SLOT_OWNER_BYTE(cqeFactor)];
            if (v == 0x80) ++v80; else ++vOther;
        }
        if (outOwnerAfter) { outOwnerAfter[0] = v80; outOwnerAfter[1] = vOther; }
        LOG("cqCreate: owner byte AFTER the preset: 0x80 x%u, anything else x%u%s",
            (unsigned)v80, (unsigned)vOther,
            vOther ? " -- the preset did NOT cover the ring" : "");
    }
    LOG("cqCreate: ring %u entries of %u bytes (factor %u); cleared, handed to SW2HW_CQ, "
        "then the owner byte of all %u slots set to 0x80 at offset i*%u+%u",
        (unsigned)nent, (unsigned)stride, (unsigned)cqeFactor, (unsigned)nent,
        (unsigned)stride, (unsigned)CQE_SLOT_OWNER_BYTE(cqeFactor));

    if (outRing) *outRing = ring;
    if (outPages) *outPages = pages;
    if (outMttOffset) *outMttOffset = mttOffset;
    if (outMttAddr) *outMttAddr = mttAddr;
    if (outDbRecordPhys) *outDbRecordPhys = rec->pages[0].busAddr;
    if (outRec) *outRec = rec;
    LOG("cqCreate: CQN %u ready (fw status 0x%x); ring %u pages, record at 0x%llx",
        (unsigned)cqn, (unsigned)status, (unsigned)pages,
        (unsigned long long)rec->pages[0].busAddr);
    return true;
}

bool Mlx4Hw::cqHw2Sw(UInt32 cqn, UInt32 *outFwStatus)
{
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    bool ok = cmdBox(0, 0, cqn, 1, MLX4_CMD_HW2SW_CQ, MLX4_CMD_TIME_CLASS_A, &status);
    if (outFwStatus) *outFwStatus = status;
    LOG("cqHw2Sw: CQN %u: %s (fw status 0x%x)", (unsigned)cqn,
        ok ? "given back" : "FAILED", (unsigned)status);
    return ok;
}

/*
 * Tell the hardware how far the software has consumed.
 *
 * Two different destinations, and conflating them is a classic way to make a queue look
 * dead (cq.h:163-166 vs :138-158):
 *
 *   mlx4_cq_set_ci writes the CI into the host RECORD only -- no MMIO at all.
 *   mlx4_cq_arm  writes the arm word into the record's second dword, then does an 8-byte
 *                MMIO write to UAR + MLX4_CQ_DOORBELL carrying [(sn<<28)|cmd|cqn, ci].
 */
void Mlx4Hw::cqSetCi(volatile UInt32 *record, UInt32 consIndex)
{
    if (!record) return;
    record[0] = OSSwapHostToBigInt32(consIndex & 0xffffffu);
    mlx4Wmb();
}

void Mlx4Hw::cqSetArmDb(volatile UInt32 *record, UInt32 armSn, UInt32 cmd, UInt32 consIndex)
{
    if (!record) return;
    record[1] = OSSwapHostToBigInt32(((armSn & 3u) << 28) | cmd | (consIndex & 0xffffffu));
    mlx4Wmb();
}

void Mlx4Hw::cqRingDoorbell(UInt32 cqn, UInt32 consIndex, UInt32 armSn, bool arm,
                            UInt32 uarIndex)
{
    UInt64 off = mlx4UarPageOffset(uarIndex) + MLX4_CQ_DOORBELL;
    UInt32 sn  = armSn & 3u;
    UInt32 ci  = consIndex & 0xffffffu;
    UInt32 cmd = arm ? MLX4_CQ_DB_REQ_NOT : 0u;
    UInt32 d0  = (sn << 28) | cmd | (cqn & 0xffffffu);

    if (!fReg2) { LOG("cqRingDoorbell: BAR2 is not mapped"); return; }
    if (off + 8 > 0x800000ull) {
        LOG("cqRingDoorbell: 0x%llx is outside the mapped BAR2 window",
            (unsigned long long)off);
        return;
    }
    fReg2[off / 4]       = OSSwapHostToBigInt32(d0);
    fReg2[off / 4 + 1]   = OSSwapHostToBigInt32(ci);
    mlx4Wmb();
    LOG("CQ doorbell: wrote [0x%08x, 0x%08x] to BAR2+0x%llx (UAR index %u, CQN %u, ci %u, "
        "arm_sn %u, arm %u)", (unsigned)d0, (unsigned)ci, (unsigned long long)off,
        (unsigned)uarIndex, (unsigned)cqn, (unsigned)ci, (unsigned)sn, arm ? 1u : 0u);
}

void Mlx4Hw::sqSetDoorbellRecord(volatile UInt32 *record, UInt32 word0, UInt32 word1)
{
    if (!record) return;
    /*
     * Both words are always written, including the zero case: "left as it was" and "set to
     * zero" are the same thing only while nothing else writes this record, and a variant that
     * relied on that would silently change meaning the first time something did.
     */
    record[0] = OSSwapHostToBigInt32(word0);
    record[1] = OSSwapHostToBigInt32(word1);
    /* The record is in host memory and the doorbell is MMIO: the record has to land before
     * the hardware can be told to look (mlx4_cq_arm does the same, cq.h:152-156). */
    mlx4Wmb();
}


/* ------------------------------------------------------------------ */
/* R4 stage 3: receive buffers and their WQEs                           */
/* ------------------------------------------------------------------ */

/*
 * rqWriteWqe -- one data segment, exactly as mlx4_en_init_rx_desc writes it
 * (mlx4_en_rx.c:49-70).
 *
 *   byte_count = the size the hardware may fill: the buffer size minus the 2-byte IP
 *                alignment the reference reserves.  NOT the frame length.
 *   lkey       = mr.key, the transformed key.  The raw MPT index here would be a
 *                descriptor the hardware cannot resolve, and nothing would fail loudly.
 */
void Mlx4Hw::rqWriteWqe(UInt8 *ringVirt, UInt32 stride, UInt32 index, UInt64 bufPhys,
                        UInt32 byteCount, UInt32 lkey)
{
    UInt8 *wqe;

    if (!ringVirt) return;
    wqe = ringVirt + RX_RING_HEADER_SIZE + (UInt64)index * stride;
    mlx4PutBe32(wqe + RX_WQE_BYTE_COUNT_OFFSET, byteCount);
    mlx4PutBe32(wqe + RX_WQE_LKEY_OFFSET, lkey);
    mlx4PutBe64(wqe + RX_WQE_ADDR_OFFSET, bufPhys);
}

/*
 * rqSetProducer -- the producer index, into the record.
 *
 * 16 bits rather than the CQ's 24, and host memory rather than the UAR.  The reference's
 * own line is `*ring->wqres.db.db = cpu_to_be32(ring->prod & 0xffff)`
 * (mlx4_en_rx.c:250-254).  A different width would leave the hardware reading an index it
 * was never told about, and the symptom would be "no frames arrive".
 */
void Mlx4Hw::rqSetProducer(volatile UInt32 *record, UInt32 prod)
{
    if (!record) return;
    record[0] = OSSwapHostToBigInt32(prod & RX_PROD_DB_WIDTH);
    mlx4Wmb();
}

/*
 * rqBufferPhys -- where the WQE for buffer `index` points.
 *
 * Same arithmetic rqCreate does while it writes the first fill (page = i / perPage, offset
 * within the page = (i % perPage) * bufBytes), moved into one place because stage 9 re-posts
 * these WQEs: "the address the refill writes" and "the address the first fill wrote" being the
 * same number is the whole correctness condition of recycling, and two copies of three lines
 * is how two copies stop agreeing.
 */
UInt64 Mlx4Hw::rqBufferPhys(const Mlx4Icm *bufs, UInt32 bufBytes, UInt32 index) const
{
    if (!bufs || !bufs->pages || bufBytes<128u || bufBytes>16384u ||
        (bufBytes & (bufBytes-1u))) return 0;
    const UInt64 offset=UInt64(index)*bufBytes;
    if (offset>bufs->bytes || bufBytes>bufs->bytes-offset) return 0;
    const UInt64 first=offset>>MLX4_ICM_PAGE_SHIFT;
    const UInt64 last=(offset+bufBytes-1u)>>MLX4_ICM_PAGE_SHIFT;
    if (last>=bufs->npages) return 0;
    const UInt64 base=bufs->pages[first].busAddr;
    if (!base || (base & (MLX4_ICM_PAGE_SIZE-1u))) return 0;
    const UInt64 span=(last-first+1u)*MLX4_ICM_PAGE_SIZE;
    if (base>~UInt64(0)-(span-1u)) return 0;
    for (UInt64 page=first;page<=last;++page)
        if (bufs->pages[page].busAddr!=base+(page-first)*MLX4_ICM_PAGE_SIZE) return 0;
    return base+(offset & (MLX4_ICM_PAGE_SIZE-1u));
}

/* ------------------------------------------------------------------ */
/* R4 stage 9: the send path -- one WQE, and the doorbell that runs it */
/* ------------------------------------------------------------------ */

/*
 * sqWriteSendWqe -- one send WQE: the 16-byte control segment and ONE 16-byte data segment,
 * written in the order the reference writes them (mlx4_en_tx.c:800-919).
 *
 * The order and the two barriers are the reference's, not a style choice:
 *
 *     (data segment and the control segment's other fields)
 *     wmb();
 *     tx_desc->ctrl.owner_opcode = opcode;    <- owner bit LAST
 *     wmb();
 *     writel(... MLX4_SEND_DOORBELL ...)      <- the caller does this
 *
 * and everything before the owner word is a field the hardware reads only after that word
 * says the descriptor is its own.  Getting this backwards is a descriptor the hardware can
 * read while it is still being written, which is a class of failure with no error message.
 */
void Mlx4Hw::sqWriteSendWqe(UInt8 *sqVirt, UInt32 index, UInt32 prod, UInt32 ownerBit,
                            UInt32 byteCount, UInt32 lkey, UInt64 bufPhys,
                            UInt32 nDataSegs, UInt32 opcode, UInt8 *outImage32, bool verbose,
                            UInt32 csumFlags, bool insertVlan, UInt16 vlanTag)
{
    UInt8 *wqe;
    UInt32 dword, fenceSize;

    if (!sqVirt) return;

    /*
     * The WQE array starts AT the queue's MTT base: the send queue has no header in front of
     * WQE 0, unlike the receive ring, and the two conventions are hardware fact rather than
     * preference (`mlx4_en_tx.c:139/:686` against `mlx4_en_rx.c:406/:482-489`).  This is the
     * one place where "the driver's index" and "the memory offset" are reconciled, and
     * docs/DEFECTS.md E13 is what a 64-byte offset here costs: the hardware reads the base,
     * finds a stamped unit that is not a descriptor, and executes nothing at all -- no
     * completion, no frame, no error.  The offset is a macro so that the asymmetry is asserted
     * at build time (tools/test-cqe-offsets.c) rather than repeated as arithmetic.
     */
    wqe = sqVirt + MLX4_SQ_WQE_OFFSET(index);

    /* ctrl: vlan_tag / ins_vlan / fence_size in one dword.  fence_size is the DESCRIPTOR'S
     * SIZE in 16-byte units counting the control segment -- derived here from the number of
     * data segments, never passed in as a literal; TX_WQE_FENCE_SIZE in mlx4_regs.h carries the
     * derivation and the three implementations that agree on it, and docs/M2B-NOTES.md section
     * 54 is what passing 1 instead of 2 for a ctrl-plus-one-segment WQE cost: a descriptor
     * declaring itself 16 bytes long, i.e. a SEND with no data segment, i.e. silence.
     * VLAN metadata is written before the owner word, just like the reference
     * mlx4_en_tx.c:710-716. Defaults keep legacy callers byte-identical. */
    wqe[TX_WQE_VLAN_TAG_OFFSET + 0] = insertVlan ? UInt8(vlanTag >> 8) : 0;
    wqe[TX_WQE_VLAN_TAG_OFFSET + 1] = insertVlan ? UInt8(vlanTag) : 0;
    wqe[TX_WQE_INS_VLAN_OFFSET]     = insertVlan ? MLX4_WQE_CTRL_INS_CVLAN : 0;
    fenceSize = TX_WQE_FENCE_SIZE(nDataSegs);
    wqe[TX_WQE_FENCE_SIZE_OFFSET]   = (UInt8)fenceSize;
    /*
     * ctrl: srcrb_flags.  The reference's CTRL_FLAGS -- C (a completion is generated) and SE --
     * plus whatever the caller asks for.  csumFlags defaults to 0, so the two callers that
     * transmit for the stage tests (r4raw and txprobe) write the byte-identical descriptor they
     * always did; the network data path is the only caller that can pass anything, and only
     * when it read a checksum request out of the stack's mbuf.
     */
    mlx4PutBe32(wqe + TX_WQE_SRCRB_FLAGS_OFFSET,
                MLX4_WQE_CTRL_CQ_UPDATE | MLX4_WQE_CTRL_SOLICITED |
                (csumFlags & (MLX4_WQE_CTRL_IP_CSUM | MLX4_WQE_CTRL_TCP_UDP_CSUM)));
    mlx4PutBe32(wqe + TX_WQE_IMM_OFFSET, 0);

    /*
     * data: the frame's length, the key that resolves it, and where it is -- and nothing at all
     * when the descriptor carries no data segment.  A NOP with a data segment written behind it
     * is not a NOP descriptor, and the first version of the probe's NOP-versus-SEND experiment
     * wrote one: its silence then said nothing about the hardware (docs/M2B-NOTES.md 55).
     */
    if (nDataSegs > 0u) {
        mlx4PutBe32(wqe + TX_WQE_BYTE_COUNT_OFFSET, byteCount);
        mlx4PutBe32(wqe + TX_WQE_LKEY_OFFSET, lkey);
        mlx4PutBe64(wqe + TX_WQE_ADDR_OFFSET, bufPhys);
    }

    /* (a) the descriptor reaches memory before the ownership transfer below. */
    mlx4Wmb();

    /*
     * ctrl: owner_opcode LAST, and note what its bits hold: the opcode in the low eight, the
     * producer count (in TXBBs) in 8..23, the owner bit at 31.  The owner value is the ring's
     * LAP PHASE -- `(ring->prod & ring->size)`, mlx4_en_tx.c:682 -- so it alternates every
     * time the producer walks once around the ring.  That alternation is what lets the
     * hardware and the driver agree about which lap a slot's contents belong to, and it is
     * why a ring that only ever turns once can be wrong in a way that shows up later.
     */
    dword = (opcode & 0xffu) |
            ((prod & MLX4_TX_PROD_MASK) << MLX4_TX_PROD_SHIFT) |
            ((ownerBit & 1u) << MLX4_TX_OWNER_SHIFT);
    mlx4PutBe32(wqe + TX_WQE_OWNER_OPCODE_OFFSET, dword);

    /* (b) and the ownership change is complete before anyone rings a doorbell. */
    mlx4Wmb();

    if (outImage32) memcpy(outImage32, wqe, TX_WQE_SEND_SIZE);

    /*
     * Only the interesting WQEs are logged: the selector posts hundreds of them in the wrap
     * phase and each one is a line, so the callers pass verbose = true for the FIRST post of
     * each lap (and for any post whose readback did not match), which is where a wrong encoding
     * would show.  The line is worth having because the encoding's one silent mistake -- the
     * opcode landing in the owner byte and byte 3 reading NOP -- is visible in it.
     */
    if (verbose) {
        LOG("sqWriteSendWqe: WQE %u at SQ+0x%llx = 0x%08x owner %u prod %u opcode 0x%02x "
            "fence %u, byte_count %u lkey 0x%08x addr 0x%llx; srcrb_flags 0x%08x (C and SE), "
            "vlan 0, imm 0. The opcode is in the LOW byte of the big-endian word -- shifted by "
            "24 it would become NOP (0x00) and the descriptor would complete and transmit "
            "nothing",
            (unsigned)index,
            (unsigned long long)MLX4_SQ_WQE_OFFSET(index),
            (unsigned)dword, (unsigned)((dword >> MLX4_TX_OWNER_SHIFT) & 1u),
            (unsigned)((dword >> MLX4_TX_PROD_SHIFT) & MLX4_TX_PROD_MASK),
            (unsigned)(dword & 0xffu), (unsigned)fenceSize, (unsigned)byteCount,
            (unsigned)lkey, (unsigned long long)bufPhys,
            (unsigned)(MLX4_WQE_CTRL_CQ_UPDATE | MLX4_WQE_CTRL_SOLICITED));
    }
}

/*
 * sqStampTxbb -- the reference's mlx4_en_stamp_wqe (mlx4_en_tx.c:271-287).
 *
 * A 32-byte WQE occupies one 64-byte TXBB here, so "every STAMP_STRIDE (64 bytes)" is the
 * unit itself: its first dword becomes STAMP_VAL | owner << 31.  Nothing reads a stamped unit
 * -- the point is that a reclaimed slot no longer LOOKS like a posted descriptor, and that its
 * owner bit carries the phase it will next be posted in.
 */
void Mlx4Hw::sqStampTxbb(UInt8 *sqVirt, UInt32 index, UInt32 owner)
{
    UInt32 stamp;

    if (!sqVirt) return;
    stamp = MLX4_SQ_STAMP_VAL | ((owner & 1u) << MLX4_STAMP_SHIFT);
    mlx4PutBe32(sqVirt + MLX4_SQ_WQE_OFFSET(index), stamp);
    mlx4Wmb();
}

/*
 * sqRingSendDoorbell -- the second MMIO write this project has ever made from a queue path,
 * and the first that makes the hardware send something.
 *
 * The value is the QPN in bits 8..31 (mlx4_en_tx.c:231) and the address is the UAR page of the
 * index in the QP's own context (usr_page), plus MLX4_SEND_DOORBELL.  Passed in rather than
 * re-derived: the number in the context and the page that is knocked on have to be the same
 * one, and this is the one place where they can disagree.
 */
void Mlx4Hw::sqRingSendDoorbell(UInt32 qpn, UInt32 uarIndex, bool verbose)
{
    UInt64 off = mlx4UarPageOffset(uarIndex) + MLX4_SEND_DOORBELL;
    UInt32 val = (qpn & MLX4_CQE_QPN_MASK) << MLX4_SEND_DB_QPN_SHIFT;

    if (!fReg2) {
        LOG("sqRingSendDoorbell: BAR2 is not mapped, so the send doorbell was NOT rung "
            "(the WQE is in memory and the hardware has not been told)");
        return;
    }
    if (off + 4 > 0x800000ull) {
        LOG("sqRingSendDoorbell: 0x%llx is outside the mapped BAR2 window, so the doorbell "
            "was NOT rung", (unsigned long long)off);
        return;
    }
    fReg2[off / 4] = OSSwapHostToBigInt32(val);
    mlx4Wmb();
    if (verbose) {
        LOG("SEND doorbell: wrote 0x%08x (QPN %u << %u) to BAR2+0x%llx -- the UAR page of "
            "index %u, offset 0x%x. The CQ's doorbell is 0x%x, 0xC away; knocking there "
            "instead would write into a register that is not this one",
            (unsigned)val, (unsigned)qpn, (unsigned)MLX4_SEND_DB_QPN_SHIFT,
            (unsigned long long)off, (unsigned)uarIndex, (unsigned)MLX4_SEND_DOORBELL,
            (unsigned)MLX4_CQ_DOORBELL);
    }
}

/*
 * rqCreate -- the receive ring's memory, built but not yet handed over.
 *
 * Shape follows mlx4_en_rx.c:396-500: one allocation for the ring including the 64-byte
 * header in front of the WQEs, one allocation holding all the receive buffers, an MTT
 * entry per ring page, and a WQE per entry pointing at its buffer.
 *
 * The buffers live in ONE allocation rather than one per WQE, because the page-list
 * allocator hands back whole pages and 256 buffers of 2048 bytes is exactly 128 pages: a
 * buffer never straddles a page boundary, so its bus address is its page's plus a
 * multiple of bufBytes.  That property is enforced below rather than assumed -- a buffer
 * spanning two pages would have two bus addresses, which this WQE format cannot express.
 */
bool Mlx4Hw::rqCreate(UInt32 nent, UInt32 bufBytes, UInt32 lkey, Mlx4Icm **outRing,
                      Mlx4Icm **outBufs, UInt32 *outPages, UInt32 *outMttOffset,
                      UInt32 *outMttAddr, UInt64 *outBufPhysFirst, UInt32 *outBufPhysCount,
                      UInt64 *outRingPhysFirst, UInt32 *outRingPhysCount)
{
    const UInt32 stride = RX_WQE_SIZE;
    UInt32 ringBytes, bufPages, pages, mttOffset = 0, mttAddr = 0;
    Mlx4Icm *ring = 0, *bufs = 0;
    UInt64 *list = 0;
    UInt32 i;

    if (outRing) *outRing = 0;
    if (outBufs) *outBufs = 0;
    if (outPages) *outPages = 0;
    if (outMttOffset) *outMttOffset = 0;
    if (outMttAddr) *outMttAddr = 0;
    if (outBufPhysFirst) *outBufPhysFirst = 0;
    if (outBufPhysCount) *outBufPhysCount = 0;
    if (outRingPhysFirst) *outRingPhysFirst = 0;
    if (outRingPhysCount) *outRingPhysCount = 0;

    if (!nent || nent>65536u) return false; // bound rounding and byte-count arithmetic
    { UInt32 n = 1; while (n < nent) n <<= 1; nent = n; }
    if (bufBytes<128u || bufBytes>16384u || (bufBytes & (bufBytes-1u))) {
        LOG("rqCreate refused: buffer size %u must be a power of two in 128..16384",
            (unsigned)bufBytes);
        return false;
    }

    ringBytes = RX_RING_HEADER_SIZE + nent * stride;
    pages = (ringBytes + (MLX4_ICM_PAGE_SIZE - 1)) >> MLX4_ICM_PAGE_SHIFT;
    ring  = icmAlloc(pages, false);
    if (!ring) {
        LOG("rqCreate: could not allocate %u pages for a %u-entry RQ ring",
            (unsigned)pages, (unsigned)nent);
        return false;
    }
    memset(ring->virt, 0, (size_t)ring->bytes);

    bufPages = (nent * bufBytes + (MLX4_ICM_PAGE_SIZE - 1)) >> MLX4_ICM_PAGE_SHIFT;
    bufs = icmAlloc(bufPages, false, bufBytes>MLX4_ICM_PAGE_SIZE);
    if (!bufs) {
        LOG("rqCreate: could not allocate %u pages for %u buffers of %u bytes",
            (unsigned)bufPages, (unsigned)nent, (unsigned)bufBytes);
        icmFree(ring);
        return false;
    }
    memset(bufs->virt, 0, (size_t)bufs->bytes);

    // A jumbo WQE still has one data segment. Refuse non-contiguous IOVA
    // spans before allocating/writing an MTT or publishing a hardware owner.
    for (i=0;i<nent;++i) if (!rqBufferPhys(bufs,bufBytes,i)) {
        LOG("rqCreate: buffer %u is not one complete contiguous DMA span",(unsigned)i);
        icmFree(bufs);icmFree(ring);return false;
    }

    if (!mttAlloc(pages, ring, &mttOffset, &mttAddr)) {
        LOG("rqCreate: no MTT entries for %u ring pages", (unsigned)pages);
        icmFree(bufs); icmFree(ring);
        return false;
    }
    list = (UInt64 *)IOMalloc(sizeof(UInt64) * pages);
    if (!list) {
        if (!mttFree(mttOffset, pages, ring)) {
            if (outRing) *outRing = ring;
            if (outBufs) *outBufs = bufs;
            if (outPages) *outPages = pages;
            if (outMttOffset) *outMttOffset = mttOffset;
            if (outMttAddr) *outMttAddr = mttAddr;
            if (outBufPhysFirst) *outBufPhysFirst = bufs->pages[0].busAddr;
            if (outBufPhysCount) *outBufPhysCount = nent;
            if (outRingPhysFirst) *outRingPhysFirst = ring->pages[0].busAddr;
            if (outRingPhysCount) *outRingPhysCount = pages;
            return false;
        }
        icmFree(bufs); icmFree(ring); return false;
    }
    for (i = 0; i < pages; ++i) list[i] = ring->pages[i].busAddr;
    if (!mttWrite(mttOffset, pages, list)) {
        IOFree(list, sizeof(UInt64) * pages);
        if (!mttFree(mttOffset, pages, ring)) {
            if (outRing) *outRing = ring;
            if (outBufs) *outBufs = bufs;
            if (outPages) *outPages = pages;
            if (outMttOffset) *outMttOffset = mttOffset;
            if (outMttAddr) *outMttAddr = mttAddr;
            if (outBufPhysFirst) *outBufPhysFirst = bufs->pages[0].busAddr;
            if (outBufPhysCount) *outBufPhysCount = nent;
            if (outRingPhysFirst) *outRingPhysFirst = ring->pages[0].busAddr;
            if (outRingPhysCount) *outRingPhysCount = pages;
            return false;
        }
        icmFree(bufs); icmFree(ring);
        return false;
    }
    IOFree(list, sizeof(UInt64) * pages);
    list = 0;

    {
        UInt32 byteCount = bufBytes - MLX4_NET_IP_ALIGN;
        UInt32 perPage   = bufBytes<=MLX4_ICM_PAGE_SIZE ? MLX4_ICM_PAGE_SIZE/bufBytes : 1u;
        for (i = 0; i < nent; ++i) {
            UInt64 phys;
            if ((i / perPage) >= bufPages) {
                LOG("rqCreate: buffer %u needs page %u but only %u exist",
                    (unsigned)i, (unsigned)(i / perPage), (unsigned)bufPages);
                if (!mttFree(mttOffset, pages, ring)) {
                    if (outRing) *outRing = ring;
                    if (outBufs) *outBufs = bufs;
                    if (outPages) *outPages = pages;
                    if (outMttOffset) *outMttOffset = mttOffset;
                    if (outMttAddr) *outMttAddr = mttAddr;
                    if (outBufPhysFirst) *outBufPhysFirst = bufs->pages[0].busAddr;
                    if (outBufPhysCount) *outBufPhysCount = nent;
                    if (outRingPhysFirst) *outRingPhysFirst = ring->pages[0].busAddr;
                    if (outRingPhysCount) *outRingPhysCount = pages;
                    return false;
                }
                icmFree(bufs); icmFree(ring);
                return false;
            }
            /*
             * The address comes from rqBufferPhys -- the same function stage 9's refill uses
             * when it re-posts a WQE after a frame.  One home for the arithmetic, because a
             * refill that computes a different address than the first fill is a WQE pointing
             * into a buffer nobody read (the shape docs/DEFECTS.md D12 records).
             */
            phys = rqBufferPhys(bufs, bufBytes, i);
            if (phys == 0) {
                LOG("rqCreate: buffer %u has no address (page %u of %u)", (unsigned)i,
                    (unsigned)(i / perPage), (unsigned)bufPages);
                if (!mttFree(mttOffset, pages, ring)) {
                    if (outRing) *outRing = ring;
                    if (outBufs) *outBufs = bufs;
                    if (outPages) *outPages = pages;
                    if (outMttOffset) *outMttOffset = mttOffset;
                    if (outMttAddr) *outMttAddr = mttAddr;
                    if (outBufPhysFirst) *outBufPhysFirst = bufs->pages[0].busAddr;
                    if (outBufPhysCount) *outBufPhysCount = nent;
                    if (outRingPhysFirst) *outRingPhysFirst = ring->pages[0].busAddr;
                    if (outRingPhysCount) *outRingPhysCount = pages;
                    return false;
                }
                icmFree(bufs); icmFree(ring);
                return false;
            }
            rqWriteWqe((UInt8 *)ring->virt, stride, i, phys, byteCount, lkey);
        }
        LOG("rqCreate: %u WQEs written, each a 16-byte segment: byte_count %u (buffer %u "
            "minus the %u-byte IP align, not the frame length), lkey 0x%08x, buffers from "
            "0x%llx stepping by %u",
            (unsigned)nent, (unsigned)byteCount, (unsigned)bufBytes,
            (unsigned)MLX4_NET_IP_ALIGN, (unsigned)lkey,
            (unsigned long long)bufs->pages[0].busAddr, (unsigned)bufBytes);
    }

    if (outRing) *outRing = ring;
    if (outBufs) *outBufs = bufs;
    if (outPages) *outPages = pages;
    if (outMttOffset) *outMttOffset = mttOffset;
    if (outMttAddr) *outMttAddr = mttAddr;
    if (outBufPhysFirst) *outBufPhysFirst = bufs->pages[0].busAddr;
    if (outBufPhysCount) *outBufPhysCount = nent;
    if (outRingPhysFirst) *outRingPhysFirst = ring->pages[0].busAddr;
    if (outRingPhysCount) *outRingPhysCount = pages;

    /*
     * WQE 0 read back.  "The loop ran" and "the bytes are right" are different claims, and
     * every field here is one that a mistake leaves silent until a frame fails to arrive.
     */
    {
        const UInt8 *w = (const UInt8 *)ring->virt + RX_RING_HEADER_SIZE;
        UInt32 bc = 0, lk = 0, k;
        UInt64 ad = 0;
        for (k = 0; k < 4; ++k) bc = (bc << 8) | w[RX_WQE_BYTE_COUNT_OFFSET + k];
        for (k = 0; k < 4; ++k) lk = (lk << 8) | w[RX_WQE_LKEY_OFFSET + k];
        for (k = 0; k < 8; ++k) ad = (ad << 8) | (UInt64)w[RX_WQE_ADDR_OFFSET + k];
        LOG("rqCreate: WQE 0 read back: byte_count %u, lkey 0x%08x, addr 0x%llx "
            "(expected %u / 0x%08x / 0x%llx) -- %s",
            (unsigned)bc, (unsigned)lk, (unsigned long long)ad,
            (unsigned)(bufBytes - MLX4_NET_IP_ALIGN), (unsigned)lkey,
            (unsigned long long)bufs->pages[0].busAddr,
            (bc == bufBytes - MLX4_NET_IP_ALIGN && lk == lkey &&
             ad == bufs->pages[0].busAddr) ? "all three match" : "MISMATCH");
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* R4 stage 6: the send queue's memory                                  */
/* ------------------------------------------------------------------ */

/*
 * sqCreate -- the send queue's memory: the WQE array and its MTT entries.
 *
 * Built but not handed to anything, which is the same split stage 3 kept for the RQ: "the
 * memory is where the context will say it is" and "the hardware took it" are separate
 * claims, and the latter happens when the QP's RST->INIT step carries the MTT address
 * (mlx4_qp.c:164-169).  No WQE is posted here, because this stage sends no frames (that is
 * stage 9) -- but "no WQE posted" is NOT "the memory is all zero", and reading the reference's
 * activation path as far as the clear got this wrong once (docs/DEFECTS.md C12):
 *
 *     memset(ring->buf, 0, ring->buf_size);                       mlx4_en_tx.c:227
 *     for (j = 0; j < tx_ring->buf_size; j += STAMP_STRIDE)       mlx4_en_netdev.c:1373-1375
 *             *((u32 *) (tx_ring->buf + j)) = INIT_OWNER_BIT;     -- 0xffffffff: SW owns
 *
 * The second step is the one that decides what stage 9's reclaim scan sees.  A zero dword has
 * bit 31 clear, and bit 31 is the OWNER bit -- 0 means "the hardware owns this slot", i.e.
 * every slot looks in flight.  So every 64-byte unit of this memory gets the transmit stamp
 * with owner = 1 (MLX4_SQ_INIT_OWNER_BIT = STAMP_VAL | 1 << 31 = 0xffffffff): every unit the
 * range covers is a descriptor slot, and the stamp's meaning there is "this slot is free".
 *
 * What that costs the two measurements below is worth stating, because it is why the previous
 * revision's check could not catch C12: reading a value back out of memory proves only that
 * THIS DRIVER's write is there ("the numbers are the ones we put in"), and says nothing about
 * how the hardware interprets it.  So the check is split into the two halves that can be
 * measured and named: how many of the WQE array's TXBBs carry the ownership stamp (wanted: all
 * of them), and how many bytes outside those stamps are non-zero (wanted: 0, i.e. no descriptor
 * body was written).
 *
 * THE LAYOUT: nent 64-byte TXBBs, WQE 0 at offset 0 of the allocation, and nothing in front of
 * it.
 *
 * That is the reference's transmit ring, not a shape transferred from the receive ring:
 * `ring->buf_size = ALIGN(size * ring->stride, MLX4_EN_PAGE_SIZE)` (mlx4_en_tx.c:123) with
 * `ring->buf` used directly as TXBB 0 (`ring->buf + index * TXBB_SIZE`, :686) -- so the
 * driver's index and the memory offset are the same number.  The unit is 64 bytes because
 * TXBB_SIZE is (en.h:84), and the context's sq_size_stride says so (0x42 for 256 of them,
 * mlx4_en_resources.c:54-57).
 *
 * 0.43.6 built this allocation as `[header][WQE array]` instead -- the receive ring's shape
 * (one 64-byte unit in front of the WQEs, mlx4_en_rx.c:406/:482-489) applied to a ring whose
 * hardware convention has no header.  That is docs/DEFECTS.md E13, and what it looked like on
 * the card is worth keeping next to the code: with WQE 0 written at +64 the hardware read the
 * stamped 0xffffffff ("software owns", therefore not a descriptor) unit at the base, executed
 * nothing, and produced no completion, no frame and no error.  The offset is a named constant
 * and a build-time assertion now (SQ_WQE0_OFFSET / MLX4_SQ_WQE_OFFSET,
 * tools/test-cqe-offsets.c) so that the two conventions cannot be re-unified by arithmetic.
 *
 * The MTT entries are the SQ's OWN -- the reference passes this ring's wqres.mtt to
 * mlx4_qp_to_ready (mlx4_en_tx.c:238-239) -- one per page of the allocation, written before
 * the QP exists so that the RST->INIT step has an address to name.
 */
bool Mlx4Hw::sqCreate(UInt32 nent, Mlx4Icm **outRing, UInt32 *outPages, UInt32 *outMttOffset,
                      UInt32 *outMttAddr, UInt64 *outRingPhysFirst, UInt32 *outRingPhysCount,
                      UInt32 *outStampValue, UInt32 *outTxbbsStamped,
                      UInt32 *outWqeNonZeroBytes)
{
    const UInt32 stride = MLX4_TXBB_SIZE;
    UInt32 ringBytes, pages, mttOffset = 0, mttAddr = 0, i, j, nonZero = 0, stamp = 0;
    UInt32 stamped = 0;
    Mlx4Icm *ring = 0;
    UInt64 *list = 0;
    UInt8 *sq;

    if (outRing) *outRing = 0;
    if (outPages) *outPages = 0;
    if (outMttOffset) *outMttOffset = 0;
    if (outMttAddr) *outMttAddr = 0;
    if (outRingPhysFirst) *outRingPhysFirst = 0;
    if (outRingPhysCount) *outRingPhysCount = 0;
    if (outStampValue) *outStampValue = 0;
    if (outTxbbsStamped) *outTxbbsStamped = 0;
    if (outWqeNonZeroBytes) *outWqeNonZeroBytes = 0;

    /*
     * The count is rounded up to a power of two, as the CQ's and the RQ's are: the size field
     * encodes ilog2(nent), so a count that is not a power of two cannot be described at all.
     * Rounding is the reference's own shape (its callers pass a power of two,
     * mlx4_en_main.c:93), and it is done here rather than assumed so that a caller passing
     * 1000 gets 1024 and reads 1024 back instead of a truncated encoding.
     */
    { UInt32 n = 1; while (n < nent) n <<= 1; nent = n; }
    if (nent == 0) nent = 1;

    /*
     * No header: the send queue's memory IS its WQE array, so this is the reference's own
     * `ALIGN(size * stride, PAGE_SIZE)` rounded to ICM pages (mlx4_en_tx.c:123) with nothing
     * added in front: any `+ 64` (or any header constant) in this expression is
     * docs/DEFECTS.md E13, and the receive ring's own `+ RX_RING_HEADER_SIZE` -- correct
     * there, wrong here -- is why the difference is asserted at build time rather than
     * remembered (tools/test-cqe-offsets.c).
     */
    ringBytes = nent * stride;
    pages = (ringBytes + (MLX4_ICM_PAGE_SIZE - 1)) >> MLX4_ICM_PAGE_SHIFT;
    ring = icmAlloc(pages, false);
    if (!ring) {
        LOG("sqCreate: could not allocate %u pages for a %u-TXBB send ring (%u bytes)",
            (unsigned)pages, (unsigned)nent, (unsigned)ringBytes);
        return false;
    }
    memset(ring->virt, 0, (size_t)ring->bytes);
    sq = (UInt8 *)ring->virt;

    /*
     * The initial ownership stamp: every 64-byte unit's first dword, over the whole allocation
     * -- the reference's own loop (mlx4_en_netdev.c:1373-1375), which covers every TXBB of the
     * ring because the ring IS the allocation.  The value is the transmit stamp with owner = 1
     * (0xffffffff, en.h:95-99), written big-endian into each unit's first dword; the rest of
     * every unit stays as the clear left it, which is what "nothing has been posted" has to
     * mean.
     *
     * One difference from the reference's loop, stated because it IS one: it steps over
     * `tx_ring->buf_size`, which is the size rounded up to a whole page, while this loop covers
     * exactly the `nent` units the context's sq_size_stride describes.  Bytes past
     * `nent * TXBB_SIZE` sit inside the allocation's last page but are not part of the ring, so
     * they are not slots and nothing reads them.
     */
    for (j = 0; j + MLX4_TXBB_SIZE <= ringBytes; j += MLX4_TXBB_SIZE) {
        sq[j + 0] = (UInt8)((MLX4_SQ_INIT_OWNER_BIT >> 24) & 0xffu);
        sq[j + 1] = (UInt8)((MLX4_SQ_INIT_OWNER_BIT >> 16) & 0xffu);
        sq[j + 2] = (UInt8)((MLX4_SQ_INIT_OWNER_BIT >>  8) & 0xffu);
        sq[j + 3] = (UInt8)(MLX4_SQ_INIT_OWNER_BIT & 0xffu);
    }

    if (!mttAlloc(pages, ring, &mttOffset, &mttAddr)) {
        LOG("sqCreate: no MTT entries for the send ring's %u pages", (unsigned)pages);
        icmFree(ring);
        return false;
    }
    list = (UInt64 *)IOMalloc(sizeof(UInt64) * pages);
    if (!list) {
        if (!mttFree(mttOffset, pages, ring)) {
            if (outRing) *outRing = ring;
            if (outPages) *outPages = pages;
            if (outMttOffset) *outMttOffset = mttOffset;
            if (outMttAddr) *outMttAddr = mttAddr;
            if (outRingPhysFirst) *outRingPhysFirst = ring->pages[0].busAddr;
            if (outRingPhysCount) *outRingPhysCount = pages;
            return false;
        }
        icmFree(ring); return false;
    }
    for (i = 0; i < pages; ++i) list[i] = ring->pages[i].busAddr;
    if (!mttWrite(mttOffset, pages, list)) {
        LOG("sqCreate: the MTT write failed for the send ring's %u pages", (unsigned)pages);
        IOFree(list, sizeof(UInt64) * pages);
        if (!mttFree(mttOffset, pages, ring)) {
            if (outRing) *outRing = ring;
            if (outPages) *outPages = pages;
            if (outMttOffset) *outMttOffset = mttOffset;
            if (outMttAddr) *outMttAddr = mttAddr;
            if (outRingPhysFirst) *outRingPhysFirst = ring->pages[0].busAddr;
            if (outRingPhysCount) *outRingPhysCount = pages;
            return false;
        }
        icmFree(ring);
        return false;
    }
    IOFree(list, sizeof(UInt64) * pages);
    list = 0;

    /*
     * The three measurements, all read OUT of the memory rather than reported as the constants
     * that were written into it:
     *
     *   stamp    the allocation's first dword, i.e. WQE 0 -- which is a descriptor slot, not a
     *            header, and is the same unit the count below starts its walk at;
     *   stamped  how many of the nent WQE TXBBs carry the ownership stamp.  All of them is the
     *            claim, and a partial count is exactly the failure a whole-array statement
     *            would hide -- writing the loop for the header and forgetting the array (or
     *            stopping early) would otherwise read as "the memory is stamped";
     *   nonZero  non-zero bytes in the WQE array BEYOND each TXBB's first dword.  This is the
     *            one that answers "was a descriptor posted": the stamps are 4 bytes of every
     *            64, and everything else in a TXBB belongs to a WQE body.
     *
     * None of the three says anything about how the HARDWARE reads the memory -- they measure
     * this driver's own writes.  That limit is the point of C12: a check that reads back what
     * it just wrote passes whether or not the value means what the writer thought.
     */
    stamp = ((UInt32)sq[0] << 24) | ((UInt32)sq[1] << 16) | ((UInt32)sq[2] << 8) |
            (UInt32)sq[3];
    for (i = 0; i < nent; ++i) {
        const UInt64 off = MLX4_SQ_WQE_OFFSET(i);
        UInt32 dword;
        if (off + stride > ringBytes) break;
        dword = ((UInt32)sq[off + 0] << 24) | ((UInt32)sq[off + 1] << 16) |
                ((UInt32)sq[off + 2] << 8) | (UInt32)sq[off + 3];
        if (dword == MLX4_SQ_INIT_OWNER_BIT) ++stamped;
        for (j = 4; j < stride; ++j) {
            if (sq[off + j] != 0) ++nonZero;
        }
    }

    if (outRing) *outRing = ring;
    if (outPages) *outPages = pages;
    if (outMttOffset) *outMttOffset = mttOffset;
    if (outMttAddr) *outMttAddr = mttAddr;
    if (outRingPhysFirst) *outRingPhysFirst = ring->pages[0].busAddr;
    if (outRingPhysCount) *outRingPhysCount = pages;
    if (outStampValue) *outStampValue = stamp;
    if (outTxbbsStamped) *outTxbbsStamped = stamped;
    if (outWqeNonZeroBytes) *outWqeNonZeroBytes = nonZero;

    LOG("sqCreate: %u TXBBs of %u bytes with WQE 0 at offset %u of the allocation (%u bytes "
        "total, %u pages) -- no header in front of the WQEs: the send queue's WQE i is at the "
        "MTT base + i * %u (mlx4_en_tx.c:139/:686), where the receive ring's is at +%u "
        "(mlx4_en_rx.c:406/:482-489). Every 64-byte unit's first dword is stamped with the "
        "transmit stamp STAMP_VAL(0x%08x) | owner 1 << %u = 0x%08x, i.e. 'software owns it' -- "
        "measured back out of the memory, not reported from the constant: WQE 0's dword reads "
        "0x%08x and %u of %u WQE TXBBs carry the same value. The WQE array holds %u non-zero "
        "byte(s) "
        "OUTSIDE those stamps -- this stage posts nothing, so 0 is the right answer and the "
        "stamps themselves are not counted (they are 4 bytes of every 64). MTT entry %u -> byte "
        "address 0x%x (index x %u), the ring's first page at 0x%llx",
        (unsigned)nent, (unsigned)stride, (unsigned)SQ_WQE0_OFFSET, (unsigned)ringBytes,
        (unsigned)pages, (unsigned)MLX4_TXBB_SIZE, (unsigned)RQ_WQE0_OFFSET,
        (unsigned)MLX4_SQ_STAMP_VAL, (unsigned)MLX4_STAMP_SHIFT,
        (unsigned)MLX4_SQ_INIT_OWNER_BIT, (unsigned)stamp, (unsigned)stamped, (unsigned)nent,
        (unsigned)nonZero, (unsigned)mttOffset,
        (unsigned)mttAddr, (unsigned)MLX4_MTT_ENTRY_SIZE,
        (unsigned long long)ring->pages[0].busAddr);
    return true;
}

/* ------------------------------------------------------------------ */
/* R4 stage 5: the port's MAC table, as one 1024-byte command           */
/* ------------------------------------------------------------------ */

/*
 * setPortMacTableWrite -- mlx4_set_port_mac_table (mlx4_port.c:127-146).
 *
 * Read that function before changing anything here, because its shape is the trap: the
 * mailbox it hands to the command does not carry ONE mac address, it carries the WHOLE
 * TABLE -- 128 entries of 8 bytes (MLX4_MAC_TABLE_SIZE, driver.h:733) -- and the command is
 * not per-index.  The reference builds that table in software (`entries[]`/`refs[]`/`total`,
 * mlx4_port.c:182-340) and re-sends the lot on every add and every remove; this port has one
 * address, so the table it sends is "everything clear except the one slot", which is the
 * same thing the reference would send with one entry registered.
 *
 * Four details, each of which is a way to send a command that succeeds and registers
 * nothing:
 *
 *   in_param    = the MAILBOX's DMA address (:138), not 0 and not the table's own address.
 *   in_modifier = MLX4_SET_PORT_MAC_TABLE << 8 | port (:140) -- 0x201 for port 1.  The high
 *                 byte is the sub-command; a wrong one is a different operation, not an
 *                 error the firmware reports.
 *   op_modifier = MLX4_SET_PORT_ETH_OPCODE (:141), the Ethernet form.
 *   timeout     = MLX4_CMD_TIME_CLASS_B (:142).  All three classes happen to be 60000 ms in
 *                 this port's headers, so a wrong choice would not show up as a different
 *                 duration today -- which is exactly why the right name is written down
 *                 rather than a number.
 *
 * The entries themselves: cpu_to_be64(mac | MLX4_MAC_VALID) (:299) with the valid bit at
 * 1ull << 63 (:48).  Where "mac" comes from is the caller's business -- this function takes
 * the entry KEY already assembled as 8 big-endian bytes, so the only code that knows the
 * byte layout is the one place that also reads the device.
 *
 * THE TABLE IS THE CALLER'S, BYTE FOR BYTE.  This function takes the whole table and copies
 * `tableBytes` of it into the mailbox; it does not know which entry holds the address, and it
 * has no slot parameter that could be given an answer different from the caller's.  That is a
 * fix rather than a simplification, and it is worth the sentences: the previous revision took
 * "one entry + a slot number" and built the table HERE, so the table the caller built and the
 * table that went on the wire were two objects that could disagree -- and did.  The caller
 * wrote the address into slot 0 of its own table and then scanned that table for the first
 * EMPTY slot, which returned 1 (slot 0 was no longer empty: the address was in it), so it
 * reported 1 and this function put the address in slot 1.  One address, two slots, and the
 * output agreed with neither the caller's intent nor its own comment
 * (docs/DEFECTS.md D12).  With the whole table passed in, "what was sent" and "what the caller
 * built" are the same bytes by construction, and the slot cannot be a third opinion.
 *
 * The bytes are assembled by the caller as they go on the wire, rather than passed as a host
 * UInt64: the quantity IS big-endian, and assembling it in a host integer first would mean
 * getting the byte order right in two places instead of one.
 */
bool Mlx4Hw::setPortMacTableWrite(UInt32 port, const UInt8 *table, UInt32 tableBytes,
                                 UInt32 *outFwStatus)
{
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    UInt32 inMod, i, k, nonEmpty = 0, firstNonEmpty = MLX4_MAX_MAC_NUM;

    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    if (port < 1 || port > 2) {
        LOG("setPortMacTable: refused, port %u", (unsigned)port);
        return false;
    }
    if (!table) {
        LOG("setPortMacTable: no table to send");
        return false;
    }
    /*
     * The whole table or nothing.  This command REPLACES what the firmware holds, so a short
     * one is not a shorter table: the entries past the caller's bytes would keep whatever the
     * previous command put there, which is a different operation rather than a smaller one.
     */
    if (tableBytes != MLX4_MAC_TABLE_SIZE) {
        LOG("setPortMacTable: refused, %u bytes; this command carries the whole %u-byte %u-entry "
            "table", (unsigned)tableBytes, (unsigned)MLX4_MAC_TABLE_SIZE,
            (unsigned)MLX4_MAX_MAC_NUM);
        return false;
    }
    if (!mboxAlloc()) { LOG("setPortMacTable: no command mailbox"); return false; }

    /*
     * The mailbox is cleared and the caller's bytes then copied in whole.  Clearing first is
     * what makes an entry the caller left at zero an EMPTY slot rather than a second address:
     * MLX4_MAC_VALID is the top bit of the entry (driver.h:40, mlx4_port.c:48), so an entry of
     * eight zero bytes is a hole.  The state this command writes is therefore exactly the
     * caller's table, which for this port is "one MAC, 127 holes" -- the same thing the
     * reference's software table would hold with one address registered.
     */
    memset(fMboxVirt, 0, MLX4_MAILBOX_SIZE);
    memcpy(fMboxVirt, table, tableBytes);

    inMod = (MLX4_SET_PORT_MAC_TABLE << 8) | port;
    LOG("setPortMacTable: port %u: in_param = the mailbox 0x%llx, in_modifier 0x%x "
        "(MAC_TABLE %u << 8 | port %u), op_modifier %u (ETH), timeout class B; the mailbox "
        "holds the caller's WHOLE table, copied byte for byte: %u bytes, %u entries of %u",
        (unsigned)port, (unsigned long long)fMboxPhys, (unsigned)inMod,
        (unsigned)MLX4_SET_PORT_MAC_TABLE, (unsigned)port,
        (unsigned)MLX4_SET_PORT_ETH_OPCODE, (unsigned)tableBytes,
        (unsigned)MLX4_MAX_MAC_NUM, (unsigned)MLX4_MAC_ENTRY_SIZE);

    /*
     * Where the address actually is, measured out of the bytes about to be sent.  It is
     * computed here rather than taken as a parameter because a parameter is precisely what
     * went wrong (D12): a number derived from the mailbox cannot disagree with the mailbox,
     * and the caller measures the same bytes the same way, so the two numbers are checkable
     * against each other instead of against somebody's prose.
     */
    for (i = 0; i < MLX4_MAX_MAC_NUM; ++i) {
        bool entryHasContent = false;
        for (k = 0; k < MLX4_MAC_ENTRY_SIZE; ++k) {
            if (table[(UInt64)i * MLX4_MAC_ENTRY_SIZE + k] != 0) {
                entryHasContent = true;
                break;
            }
        }
        if (entryHasContent) {
            ++nonEmpty;
            if (firstNonEmpty == MLX4_MAX_MAC_NUM) firstNonEmpty = i;
        }
    }
    LOG("setPortMacTable: port %u: the table in the mailbox holds %u non-empty entries, the "
        "first at index %u (%u = none at all). Measured from the bytes being sent, not passed "
        "in beside them -- the previous revision was handed the slot as an argument, and the "
        "argument said 1 while the caller's table held the address in slot 0",
        (unsigned)port, (unsigned)nonEmpty, (unsigned)firstNonEmpty,
        (unsigned)MLX4_MAX_MAC_NUM);
    if (firstNonEmpty < MLX4_MAX_MAC_NUM) {
        const UInt8 *e = table + (UInt64)firstNonEmpty * MLX4_MAC_ENTRY_SIZE;
        LOG("setPortMacTable: entry %u bytes (BE64, as they go on the wire): "
            "%02x %02x %02x %02x %02x %02x %02x %02x",
            (unsigned)firstNonEmpty, e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
    }

    if (!cmdBox(fMboxPhys, 0, inMod, MLX4_SET_PORT_ETH_OPCODE, MLX4_CMD_SET_PORT,
                MLX4_CMD_TIME_CLASS_B, &status)) {
        if (outFwStatus) *outFwStatus = status;
        LOG("setPortMacTable: port %u: SET_PORT MAC_TABLE failed (fw status 0x%x)",
            (unsigned)port, (unsigned)status);
        return false;
    }
    if (outFwStatus) *outFwStatus = status;
    LOG("setPortMacTable: port %u: accepted (fw status 0x%x); no documented readback is "
        "available, so addressed packet reception must verify steering",
        (unsigned)port, (unsigned)status);
    return true;
}

/*
 * No supported read operation is known for SET_PORT_MAC_TABLE. Earlier builds guessed that
 * in_param=0 turned SET_PORT into a read; it remains a write opcode and may alter the table.
 * Keep this entrypoint fail-closed so a future caller cannot reintroduce that experiment.
 */
bool Mlx4Hw::setPortMacTableRead(UInt32 port, UInt32 entryIndex, UInt8 *outEntry,
                                 UInt32 entryBytes, UInt32 *outFwStatus)
{
    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    if (outEntry && entryBytes <= MLX4_MAC_ENTRY_SIZE) memset(outEntry, 0, entryBytes);
    LOG("setPortMacTableRead: DISABLED for port %u entry %u: no documented read operation; "
        "SET_PORT_MAC_TABLE is a write even when in_param is zero. No command was posted",
        (unsigned)port, (unsigned)entryIndex);
    return false;
}

/* ------------------------------------------------------------------ */
/* R4 stage 4: the RX QP, walked from RST to RTS                        */
/* ------------------------------------------------------------------ */

/*
 * qpFillContext -- every field of the 248-byte QP context, in one place.
 *
 * The values are the reference's, field for field (mlx4_en_fill_qp_context,
 * mlx4_en_resources.c:46-82), with the two places mlx4_en_config_rss_qp overrides them
 * afterwards already applied -- that is the real RX ring's context
 * (mlx4_en_rx.c:945-953), not the steering QP's:
 *
 *   db_rec_addr = ring->wqres.db.dma, NOT `<< 2`.  The `<< 2` in mlx4_en_resources.c:78
 *                belongs to the indirection QP, which produces no completions; the RX
 *                ring's own line overwrites it.  Those are the two candidates and the
 *                later write is the one that runs.
 *
 *   param3      = 1 << 29 when the device advertises FCS_KEEP.  Not a nicety: it decides
 *                whether a received frame is 4 bytes longer, and that shows up as a
 *                length that is off by ETH_FCS_LEN in every later stage.
 *
 * Everything this card can be asked is passed in rather than written down: pdn, qpn, the
 * UAR index, the CQN, the MTT address and the FCS bit all come from the caller, which took
 * them from the device.  The parameters that remain are the ones this driver CHOOSES (the
 * queue sizes) or that are constants of the Ethernet QP shape (mtu_msgmax, the scheduling
 * queue byte).
 */
void Mlx4Hw::qpFillContext(UInt8 *context, UInt32 pdn, UInt32 qpn, UInt32 uarIndex,
                           UInt32 size, UInt32 stride, UInt32 cqn,
                           UInt64 dbRecAddr, UInt32 mtuMsgmax, UInt32 port, bool fcsKeep,
                           UInt32 counterIndex, bool rss, bool isTx, int userPrio)
{
    UInt32 rqSizeStride = 0, sqSizeStride;
    UInt32 param3 = 0;
    UInt32 schedQueue;
    UInt32 feup = 0;
    UInt32 flags;

    if (!context) return;

    /*
     * The two size/stride bytes.  ilog2(entries) << 3 | (ilog2(stride) - 4), and the
     * subtraction is computed rather than written down so a card with a different WQE size
     * cannot silently get an offset byte: ilog2(16) - 4 is 0, and ilog2(64) - 4 is 2.
     *
     * Three lines of the reference (mlx4_en_resources.c:52-57), one of which is a
     * condition rather than a value:
     *
     *     if (!is_tx && !rss) rq_size_stride = ilog2(size) << 3 | (ilog2(stride) - 4);
     *     if (is_tx)          sq_size_stride = ilog2(size) << 3 | (ilog2(stride) - 4);
     *     else                sq_size_stride = ilog2(TXBB_SIZE) - 4;
     *
     * A send QP therefore leaves rq_size_stride at the zero the clear leaves -- the byte is
     * still in the mailbox, but no receive geometry is claimed by it -- and describes its
     * OWN ring in sq_size_stride (256 TXBBs of 64 bytes: 0x42).  The dummy-SQ form
     * (ilog2(64) - 4 = 0x02) is what the receive QP and the indirection QP carry, and it
     * describes the 64-byte unit in front of their WQEs.
     */
    if (!isTx && !rss) {
        rqSizeStride = (mlx4Log2(size) << MLX4_QP_RQ_STRIDE_SHIFT) |
                       (mlx4Log2(stride) - MLX4_QP_STRIDE_SUB);
    }
    sqSizeStride = isTx ? ((mlx4Log2(size) << MLX4_QP_RQ_STRIDE_SHIFT) |
                           (mlx4Log2(stride) - MLX4_QP_STRIDE_SUB))
                        : (mlx4Log2(MLX4_QP_WQE_SIZE_DUMMY_SQ) - MLX4_QP_STRIDE_SUB);

    memset(context, 0, QP_CONTEXT_SIZE);
    /*
     * FCS_KEEP is set by the RECEIVE path only.  The reference's bit lives in
     * mlx4_en_config_rx_qp (mlx4_en_rx.c:950-951), not in mlx4_en_fill_qp_context, and the
     * name says why: it asks the firmware to keep the 4-byte Ethernet FCS on frames this
     * queue RECEIVES, so a send queue setting it would claim something about a direction it
     * does not have.  `fcsKeep` is the device's capability either way -- the caller passes
     * the same value for both kinds of queue -- and this line is where the reference's
     * asymmetry is applied.
     */
    if (fcsKeep && !isTx) param3 |= MLX4_QP_PARAM3_FCS_KEEP;

    /*
     * flags: the fixed bits, with the state nibble left at RST -- the three transitions are
     * the only thing that changes it, exactly as the reference does it (mlx4_qp.c:912-914).
     *
     * The rss bit is `rss << MLX4_RSS_QPC_FLAG_OFFSET` (mlx4_en_resources.c:49), and it is
     * the one bit in this word that says what KIND of QP this is: set, the firmware reads the
     * RSS overlay further down the same context and steers hashed traffic through this QP;
     * clear, it is an ordinary queue. An indirection QP with this bit clear is not an error
     * the firmware reports -- it is a queue nobody hashes into.
     */
    flags = MLX4_QP_FLAGS_FIXED | ((rss ? 1u : 0u) << MLX4_RSS_QPC_FLAG_OFFSET);
    mlx4PutBe32(context + QP_CONTEXT_FLAGS_OFFSET, flags);
    mlx4PutBe32(context + QP_CONTEXT_PD_OFFSET, pdn);
    context[QP_CONTEXT_MTU_MSGMAX_OFFSET]     = (UInt8)mtuMsgmax;
    /*
     * rq_size_stride is written ONLY when `!is_tx && !rss` in the reference
     * (mlx4_en_resources.c:52), so two kinds of QP leave it ZERO -- which is not stage 4's
     * 0x40, and that difference is a deliberate part of what this copies:
     *
     *   an RSS indirection QP has no receive queue of its own: nothing is posted to it and it
     *   never produces a completion, so writing the real RX ring's size here would claim a
     *   queue that does not exist (stage 5);
     *
     *   a SEND QP has no receive queue either, and the byte it must NOT carry is any receive
     *   geometry at all -- its ring is described by sq_size_stride instead (stage 6).
     */
    if (!rss && !isTx) context[QP_CONTEXT_RQ_SIZE_STRIDE_OFFSET] = (UInt8)rqSizeStride;
    context[QP_CONTEXT_SQ_SIZE_STRIDE_OFFSET] = (UInt8)sqSizeStride;
    mlx4PutBe32(context + QP_CONTEXT_USR_PAGE_OFFSET, uarIndex);
    mlx4PutBe32(context + QP_CONTEXT_LOCAL_QPN_OFFSET, qpn);
    /*
     * remote_qpn stays 0.  The reference leaves it 0 too for an RX QP (mlx4_qp_modify sets
     * only local_qpn, mlx4_qp.c:180-181), and the entropy hash that would read it belongs
     * to the RoCE send path -- see the note about roce_entropy in the selector.
     */
    /*
     * rlkey_roce_mode (0x0b) stays 0, and it arrives that way by design rather than by
     * omission: the field is declared in qp.h:202 and no line in the vendored reference ever
     * writes it, so there is nothing to copy.  The memset above is the whole story for it.
     *
     * usr_page is the software UAR index, not the hardware one -- the reference writes
     * `mdev->priv_uar.index` here (mlx4_en_resources.c:58) while the CQ's logsize_usrpage
     * carries mlx4_to_hw_uar_index() of the same number (mlx4_cq.c:274-276).  The two are
     * equal on this card because uar_page_sz is 0, and the caller passes the same UAR the CQ
     * uses, which is what the reference does: a netdev has one priv_uar for all its queues.
     */
    context[QP_CONTEXT_PRI_COUNTER_INDEX_OFFSET] = (UInt8)counterIndex;
    context[QP_CONTEXT_PRI_ACKTO_OFFSET]         = 1;
    /*
     * sched_queue and feup are written TOGETHER because the reference writes them together,
     * inside one condition (mlx4_en_resources.c:61-65):
     *
     *     context->pri_path.sched_queue = 0x83 | (priv->port - 1) << 6;
     *     if (user_prio >= 0) {
     *             context->pri_path.sched_queue |= user_prio << 3;
     *             context->pri_path.feup = 1 << 6;          <- MLX4_FEUP_FORCE_ETH_UP
     *     }
     *
     * and this is the line the port did not have.  The prefix of sched_queue was already
     * right (0x83 | (port-1) << 6, and the priority field left zero, which is what
     * `user_prio << 3` contributes when the transmit caller passes the queue index 0); the
     * bit the send side also carries was missing entirely, so a SEND queue's context claimed
     * the same "not Ethernet-up" state as a receive queue's while still being expected to
     * transmit.  See MLX4_FEUP_FORCE_ETH_UP in mlx4_regs.h for the two callers' signs, the
     * other three implementations that set it, and the falsification condition for this run.
     *
     * Both writes come from the ONE argument, so "the priority went in" and "the bit went in"
     * cannot drift apart: the reference's condition is exactly `userPrio >= 0`.
     */
    schedQueue = MLX4_QP_SCHED_QUEUE_ETH |
                 ((port ? (port - 1u) : 0u) << MLX4_QP_SCHED_QUEUE_PORT_SHIFT);
    if (userPrio >= 0) {
        schedQueue |= (UInt32)(userPrio & 0x7) << MLX4_QP_SCHED_QUEUE_PRIO_SHIFT;
        feup        = MLX4_FEUP_FORCE_ETH_UP;
    }
    context[QP_CONTEXT_PRI_SCHED_QUEUE_OFFSET] = (UInt8)schedQueue;
    context[QP_CONTEXT_PRI_FEUP_OFFSET]        = (UInt8)feup;
    mlx4PutBe32(context + QP_CONTEXT_CQN_SEND_OFFSET, cqn);
    mlx4PutBe32(context + QP_CONTEXT_CQN_RECV_OFFSET, cqn);
    mlx4PutBe64(context + QP_CONTEXT_DB_REC_ADDR_OFFSET, dbRecAddr);
    mlx4PutBe16(context + QP_CONTEXT_RQ_WQE_COUNTER_OFFSET, 0);
    mlx4PutBe16(context + QP_CONTEXT_SQ_WQE_COUNTER_OFFSET, 0);
    mlx4PutBe32(context + QP_CONTEXT_PARAM3_OFFSET, param3);
    /*
     * The MTT and the page-size byte are NOT set here: only the RST->INIT transition
     * carries them (mlx4_qp.c:164-169), and they have to survive unchanged through the
     * other two.  Setting them in the shared image would make that distinction
     * untestable, which is exactly what "one context, three states" is checking.
     */
    context[QP_CONTEXT_LOG_PAGE_SIZE_OFFSET] = 0;
    context[QP_CONTEXT_MTT_BASE_H_OFFSET]    = 0;
    mlx4PutBe32(context + QP_CONTEXT_MTT_BASE_L_OFFSET, 0);

    LOG("qpFillContext: pd %u, qpn %u, usr_page %u, mtu_msgmax 0x%02x, flags 0x%08x "
        "(rss bit %u: %s), rq_size_stride 0x%02x (%s), "
        "sq_size_stride 0x%02x, cqn_send/recv %u, db_rec_addr 0x%llx, counter_index 0x%02x, "
        "sched_queue 0x%02x, feup 0x%02x, param3 0x%08x%s",
        (unsigned)pdn, (unsigned)qpn, (unsigned)uarIndex, (unsigned)mtuMsgmax,
        (unsigned)flags, rss ? 1u : 0u,
        rss ? "an indirection QP, so the overlay at 0x3c is what the firmware reads"
            : (isTx ? "a send QP, so sq_size_stride describes its own ring"
                    : "an ordinary receive queue"),
        (unsigned)context[QP_CONTEXT_RQ_SIZE_STRIDE_OFFSET],
        (!rss && !isTx)
            ? "ilog2(entries) << 3 | (ilog2(stride) - 4), written"
            : "left at zero: the reference writes it only when !is_tx && !rss",
        (unsigned)sqSizeStride,
        (unsigned)cqn, (unsigned long long)dbRecAddr, (unsigned)counterIndex,
        (unsigned)context[QP_CONTEXT_PRI_SCHED_QUEUE_OFFSET],
        (unsigned)context[QP_CONTEXT_PRI_FEUP_OFFSET], (unsigned)param3,
        isTx ? " (a send QP: FCS_KEEP is not set, the reference sets it on the receive side "
               "only -- mlx4_en_rx.c:950-951)"
             : (fcsKeep ? " (FCS kept: the device advertises FCS_KEEP)" : ""));
    /*
     * The feup line, on its own because it is this batch's subject and because the number that
     * matters is the CONTRAST: userPrio >= 0 is the reference's transmit instantiation of the
     * shared fill, and -1 is both receive-side ones (mlx4_en_rx.c:946, :1057).
     */
    LOG("qpFillContext: user_prio %d -> pri_path.feup 0x%02x (%s) at QPC+0x%x, "
        "sched_queue 0x%02x. MLX4_FEUP_FORCE_ETH_UP (0x40) is set for a SEND queue only: the "
        "reference's fill sets it when user_prio >= 0 (mlx4_en_resources.c:62-65), the "
        "transmit ring passes a queue index (>= 0, mlx4_en_tx.c:233-234 with "
        "mlx4_en_netdev.c:1362-1363) and BOTH receive-side callers pass -1 "
        "(mlx4_en_rx.c:946 for the ring, :1057 for the RSS indirection QP). The receive QP "
        "of stage 4 is the -1 instantiation and it works on this card; this is the other "
        "one. Written 0x%02x, and what the firmware keeps is read back by QUERY_QP",
        userPrio, (unsigned)context[QP_CONTEXT_PRI_FEUP_OFFSET],
        (userPrio >= 0) ? "MLX4_FEUP_FORCE_ETH_UP: this queue is expected to transmit"
                        : "left at zero: this queue only receives",
        (unsigned)QP_CONTEXT_PRI_FEUP_OFFSET,
        (unsigned)context[QP_CONTEXT_PRI_SCHED_QUEUE_OFFSET],
        (unsigned)context[QP_CONTEXT_PRI_FEUP_OFFSET]);
    LOG("qpFillContext: the two size/stride bytes as they stand in the mailbox: "
        "rq_size_stride 0x%02x, sq_size_stride 0x%02x (from %s %u of stride %u)%s",
        (unsigned)context[QP_CONTEXT_RQ_SIZE_STRIDE_OFFSET],
        (unsigned)context[QP_CONTEXT_SQ_SIZE_STRIDE_OFFSET],
        isTx ? "the SEND ring:" : "the receive ring:", (unsigned)size, (unsigned)stride,
        isTx ? " -- a send QP's sq_size_stride is its own ring, not the dummy 0x02 a receive "
               "QP carries" : "");
    if (rss) {
        /*
         * Said out loud, because this is the half of the fill that changes behaviour and the
         * half a reader would otherwise have to infer from a parameter: with rss set, the
         * rq_size_stride byte was deliberately NOT written and is 0 -- not stage 4's 0x40.
         */
        LOG("qpFillContext: rq_size_stride left at 0x%02x because rss is set -- the "
            "reference's condition is `!is_tx && !rss` (mlx4_en_resources.c:52), so an "
            "indirection QP sends no receive-queue size at all.",
            (unsigned)context[QP_CONTEXT_RQ_SIZE_STRIDE_OFFSET]);
    }
    if (isTx) {
        /*
         * The other half of the same asymmetry, said out loud for the same reason: on this
         * path BOTH size/stride bytes differ from what stage 4 sends -- rq_size_stride is 0
         * where the RX QP has 0x40, and sq_size_stride is the send ring's own 0x42 where the
         * RX QP has the dummy 0x02.  A reader comparing this context with stage 4's would
         * otherwise see two "wrong" bytes and no sentence explaining them.
         */
        LOG("qpFillContext: this is the send side, so both size/stride bytes differ from an "
            "RX context on purpose: rq_size_stride 0x%02x (0x40 for the RX QP -- a send QP "
            "claims no receive geometry) and sq_size_stride 0x%02x (0x02 for the RX QP -- a "
            "send QP describes its own %u TXBBs of %u bytes, not the dummy SQ)",
            (unsigned)context[QP_CONTEXT_RQ_SIZE_STRIDE_OFFSET],
            (unsigned)context[QP_CONTEXT_SQ_SIZE_STRIDE_OFFSET], (unsigned)size,
            (unsigned)stride);
    }
}

/*
 * qpFillRssContext -- the RSS overlay, at QPC + 0x3c (mlx4_en_rx.c:1067-1075).
 *
 * This is the second layer of an indirection QP, and it is a separate function because the
 * reference separates it: mlx4_en_fill_qp_context fills the whole 248-byte context, and then
 * mlx4_en_config_rss_steer reinterprets part of pri_path as a `struct mlx4_rss_context`
 * (`ptr = (u8 *)&context + offsetof(struct mlx4_qp_context, pri_path) +
 * MLX4_RSS_OFFSET_IN_QPC_PRI_PATH`, :1067-1069).  pri_path is at 0x18 (qp.h:140 -- this port
 * already relies on that for counter_index at 0x1c and sched_queue at 0x38) and the RSS
 * offset inside it is 0x24 (qp.h:117), so the overlay begins at QPC + 0x3c: two fresh fields
 * (0x3c/0x40), not aliases of anything the fill wrote.
 *
 * Each field, and what a wrong value costs -- note that all of these are silent: the three
 * commands still return fw status 0 whatever they say.
 *
 *   base_qpn     the first ring the hash may select, with the ring COUNT encoded in its top
 *                byte: `ilog2(rss_rings) << 24 | base_qpn` (:1070-1071).  With one ring
 *                ilog2(1) is 0, so the high byte is 0 and this field comes out EQUAL to
 *                default_qpn -- which makes this stage's read-back a clean two-value check
 *                (both equal the real RX QPN) and means the high-byte encoding is NOT
 *                exercised here.  That is decision (3) in docs/R4-STAGE5-PLAN.md, and it
 *                belongs to the multi-ring milestone.
 *   default_qpn  where traffic goes when the hash did not pick a ring -- the real RX ring's
 *                QPN.  This is the field the stage's verdict rests on: it is what makes the
 *                indirection QP point at the queue that actually receives.
 *   hash_fn      the hash function: MLX4_RSS_HASH_TOP (1); XOR is the other value (:1074
 *                uses TOP, qp.h:108-109 names both).
 *   flags        which protocols are hashed (mlx4_en_get_rss_mask, :1005-1013).
 *   rss_key      40 bytes of Toeplitz key (:1075, MLX4_EN_RSS_KEY_SIZE at qp.h:127).
 *   base_qpn_udp filled only when the profile asks for UDP RSS (:1072-1073).  This port's
 *                profile does not, so the caller passes udpRss = false and the field keeps
 *                the zero the clear left -- which is what the reference does, rather than a
 *                value of its own.
 *
 * rss_key arrives as bytes because where a key comes from is policy, not layout: the
 * reference asks its stack (`rss_getkey`, mlx4_en_rx.c:1003) and this port has no stack to
 * ask, so the caller builds one and says where in the log.
 */
void Mlx4Hw::qpFillRssContext(UInt8 *context, UInt32 baseQpn, UInt32 rssRings,
                              UInt32 baseQpnUdp, bool udpRss, const UInt8 *key,
                              UInt32 keyBytes)
{
    UInt8  mask;
    UInt32 rings = rssRings ? rssRings : 1u;
    UInt32 high = mlx4Log2(rings);

    if (!context) return;

    if (high > 0xffu) {
        /*
         * The ring count lives in ONE byte (ilog2(rss_rings) << 24), so more than 255 rings
         * cannot be encoded at all.  Truncating silently would tell the firmware to hash into
         * a range of QPs nobody reserved, so a later milestone with many rings hits this
         * refusal instead of a wrong number.
         */
        LOG("qpFillRssContext: %u rings needs ilog2 = %u, which does not fit in the "
            "encoding's single byte -- refusing to truncate it", (unsigned)rings,
            (unsigned)high);
        return;
    }

    mlx4PutBe32(context + QP_CONTEXT_RSS_BASE_QPN_OFFSET,
                (high << MLX4_RSS_QPN_HIGH_SHIFT) | (baseQpn & 0x00ffffffu));
    mlx4PutBe32(context + QP_CONTEXT_RSS_DEFAULT_QPN_OFFSET, baseQpn & 0x00ffffffu);
    mlx4PutBe16(context + QP_CONTEXT_RSS_RESERVED_OFFSET, 0);
    context[QP_CONTEXT_RSS_HASH_FN_OFFSET] = (UInt8)MLX4_RSS_HASH_TOP;

    mask = (UInt8)(MLX4_RSS_IPV4 | MLX4_RSS_TCP_IPV4 | MLX4_RSS_IPV6 | MLX4_RSS_TCP_IPV6);
    if (udpRss) mask |= (UInt8)(MLX4_RSS_UDP_IPV4 | MLX4_RSS_UDP_IPV6);
    context[QP_CONTEXT_RSS_FLAGS_OFFSET] = mask;

    if (key && keyBytes) {
        UInt32 n = (keyBytes > MLX4_EN_RSS_KEY_SIZE) ? MLX4_EN_RSS_KEY_SIZE : keyBytes;
        memcpy(context + QP_CONTEXT_RSS_KEY_OFFSET, key, n);
        /* The remainder of the 40-byte field keeps the zero the clear left. */
    }
    if (udpRss) {
        mlx4PutBe32(context + QP_CONTEXT_RSS_BASE_QPN_UDP_OFFSET,
                    baseQpnUdp & 0x00ffffffu);
    }

    LOG("qpFillRssContext: at QPC + 0x%02x (pri_path 0x%02x + the RSS offset 0x%02x): "
        "base_qpn 0x%08x (ilog2(%u) << %u | %u), default_qpn 0x%08x, hash_fn %u (TOP), "
        "flags 0x%02x%s, rss_key %u byte(s), base_qpn_udp %s",
        (unsigned)QP_CONTEXT_RSS_BASE_QPN_OFFSET, (unsigned)QP_CONTEXT_PRI_PATH_OFFSET,
        (unsigned)MLX4_RSS_OFFSET_IN_QPC_PRI_PATH,
        (unsigned)mlx4GetBe32(context + QP_CONTEXT_RSS_BASE_QPN_OFFSET),
        (unsigned)rings, (unsigned)MLX4_RSS_QPN_HIGH_SHIFT, (unsigned)(baseQpn & 0x00ffffffu),
        (unsigned)mlx4GetBe32(context + QP_CONTEXT_RSS_DEFAULT_QPN_OFFSET),
        (unsigned)context[QP_CONTEXT_RSS_HASH_FN_OFFSET], (unsigned)mask,
        udpRss ? " (with the UDP bits: the profile asks for UDP RSS)" : "",
        (unsigned)(key ? keyBytes : 0u),
        udpRss ? "filled (:1072-1073)" : "left 0 -- the profile does not ask for UDP RSS");
    if (high == 0) {
        LOG("qpFillRssContext: one ring, so ilog2(rings) is 0 and base_qpn's high byte is 0 "
            "-- base_qpn and default_qpn are EQUAL by construction here, which is why the "
            "high-byte encoding is not exercised by this stage (decision 3).");
    }
}

/*
 * qpModify -- one of the three state transitions, with the reference's parameter shapes
 * written out rather than reproduced from memory (mlx4_qp.c:177-186):
 *
 *   in_param     = the command mailbox's DMA address  (:183)
 *   in_modifier  = qpn | (sqd_event << 31); this port has no sqd_event, so it is the qpn
 *                  (:184)
 *   op_modifier  = 2 for a transition INTO reset, 0 otherwise (:185) -- and 2 is refused
 *                  here, because the two-RST path has no mailbox at all: see qpToReset.
 *
 * The context sits at mailbox +8.  That is the whole first half of this stage's risk: the
 * CQ context this project already sends lives at +0, and both are "the context in the
 * mailbox" in prose.
 *
 * One line of the reference is folded into the caller rather than repeated here: it writes
 * local_qpn into the mailbox copy just before the command (mlx4_qp.c:180-181), i.e. it
 * asserts that the QP number the command is told and the number in the context agree.  This
 * port's qpFillContext writes that field into the single context image the caller owns, so
 * the two cannot disagree -- which is the property that line exists to guarantee.
 */
bool Mlx4Hw::qpModify(UInt16 op, UInt32 qpn, const UInt8 *context, UInt32 *outFwStatus,
                      UInt8 *outMailbox, UInt32 outBytes,
                      Cx3QpCaptureObservation *outObservation)
{
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;

    if (outObservation) {
        memset(outObservation, 0, sizeof(*outObservation));
        outObservation->abiVersion = CX3_QP_CAPTURE_OBSERVATION_ABI;
        outObservation->abiBytes = sizeof(*outObservation);
        outObservation->fwStatus = CX3_FWSTATUS_NOT_POSTED;
    }
    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    /* Capture includes the cleared padding after the 248-byte hardware QPC. */
    if ((!outMailbox && outBytes) ||
        (outMailbox && outBytes < CX3_QP_MODIFY_CAPTURE_BYTES)) return false;
    if (outMailbox) memset(outMailbox, 0, CX3_QP_MODIFY_CAPTURE_BYTES);
    if (!context) return false;
    if (op == MLX4_CMD_2RST_QP) {
        LOG("qpModify refused: 2RST_QP carries no mailbox -- use qpToReset, which sends it "
            "with in_param 0 and op_modifier 2 (mlx4_qp.c:145-147)");
        return false;
    }
    if (!mboxAlloc()) { LOG("qpModify: no command mailbox"); return false; }

    memset(fMboxVirt, 0, MLX4_MAILBOX_SIZE);
    mlx4PutBe32(fMboxVirt, QP_OPTPAR_DEFAULT);
    memcpy((UInt8 *)fMboxVirt + QP_CONTEXT_MAILBOX_OFFSET, context, QP_CONTEXT_SIZE);
    if (outObservation) outObservation->preparedBytes = CX3_QP_MODIFY_CAPTURE_BYTES;
    if (outMailbox) {
        memcpy(outMailbox, fMboxVirt, CX3_QP_MODIFY_CAPTURE_BYTES);
        if (outObservation) outObservation->capturedBytes = CX3_QP_MODIFY_CAPTURE_BYTES;
    }

    LOG("qpModify: opcode 0x%02x, qpn %u, in_param (the mailbox) 0x%llx, "
        "in_modifier %u, op_modifier 0, context at mailbox +%u; flags 0x%08x "
        "(state %u), mtt_base_h 0x%02x, mtt_base_l 0x%08x, log_page_size %u",
        (unsigned)op, (unsigned)qpn, (unsigned long long)fMboxPhys, (unsigned)qpn,
        (unsigned)QP_CONTEXT_MAILBOX_OFFSET,
        (unsigned)mlx4GetBe32(context + QP_CONTEXT_FLAGS_OFFSET),
        (unsigned)qpState(context),
        (unsigned)context[QP_CONTEXT_MTT_BASE_H_OFFSET],
        (unsigned)mlx4GetBe32(context + QP_CONTEXT_MTT_BASE_L_OFFSET),
        (unsigned)context[QP_CONTEXT_LOG_PAGE_SIZE_OFFSET]);

    const bool commandOk = cmdBox(fMboxPhys, 0, qpn, 0, op, MLX4_CMD_TIME_CLASS_C, &status,
                                  false, outObservation ? &outObservation->command : 0);
    if (outObservation) outObservation->fwStatus = status;
    if (!commandOk) {
        if (outFwStatus) *outFwStatus = status;
        if (status == CX3_FWSTATUS_TIMEOUT) {
            LOG("qpModify: opcode 0x%02x for QPN %u TIMED OUT after being POSTED -- the "
                "hardware may have taken the context, so the QP's state is UNKNOWN and "
                "nothing is assumed about it (fw status 0x%x)",
                (unsigned)op, (unsigned)qpn, (unsigned)status);
        } else {
            LOG("qpModify: opcode 0x%02x failed for QPN %u (fw status 0x%x)",
                (unsigned)op, (unsigned)qpn, (unsigned)status);
        }
        return false;
    }
    if (outFwStatus) *outFwStatus = status;
    return true;
}

/*
 * qpQuery -- mlx4_qp_query (mlx4_qp.c:884-898), and the reason this stage can be said to
 * have finished rather than merely to have been sent.
 *
 * The command shape is the one the reference uses: cmd_box(in=0, out=mailbox, qpn, 0,
 * QUERY_QP) -- no inbox, because the question is the in_modifier -- and the answer is the
 * context at mailbox +8 (:892).  The context that comes back is what the FIRMWARE holds,
 * not an echo of what this port last wrote, which is what makes it evidence for the two
 * things this stage has to distinguish:
 *
 *   - is this the QP that was written?  local_qpn is set by mlx4_qp_modify itself
 *     (mlx4_qp.c:180-181), so a QP created by anyone else would say so here;
 *   - did the firmware keep the fields?  the state nibble and the queue parameters are
 *     read back field by field by the caller.
 *
 * `need` is a byte count and is checked against the context size, so this cannot be asked
 * to copy more than the caller's buffer holds.
 */
bool Mlx4Hw::qpQuery(UInt32 qpn, UInt8 *outContext, UInt32 need, UInt32 *outFwStatus,
                     UInt8 *outMailbox, UInt32 outBytes,
                     Cx3QpCaptureObservation *outObservation)
{
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;

    if (outObservation) {
        memset(outObservation, 0, sizeof(*outObservation));
        outObservation->abiVersion = CX3_QP_CAPTURE_OBSERVATION_ABI;
        outObservation->abiBytes = sizeof(*outObservation);
        outObservation->fwStatus = CX3_FWSTATUS_NOT_POSTED;
    }
    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    if ((!outMailbox && outBytes) || (outMailbox && outBytes < MLX4_MAILBOX_SIZE)) return false;
    if (outMailbox) memset(outMailbox, 0, MLX4_MAILBOX_SIZE);
    if (!outContext || need == 0 || need > QP_CONTEXT_SIZE) return false;
    memset(outContext, 0, need);
    if (!mboxAlloc()) { LOG("qpQuery: no command mailbox"); return false; }

    /*
     * The mailbox is cleared before the command rather than after: a query whose answer
     * does not arrive would otherwise leave the PREVIOUS command's bytes in the buffer and
     * the caller would read them as this QP's context.  That is the same shape as the MPT
     * readback defect this project already recorded (a byte reported that nobody wrote).
     */
    memset(fMboxVirt, 0, MLX4_MAILBOX_SIZE);
    if (outObservation) outObservation->preparedBytes = MLX4_MAILBOX_SIZE;
    const bool commandOk = cmdBox(0, fMboxPhys, qpn, 0, MLX4_CMD_QUERY_QP,
                                  MLX4_CMD_TIME_CLASS_A, &status,
                                  false, outObservation ? &outObservation->command : 0);
    if (outObservation) outObservation->fwStatus = status;
    if (!commandOk) {
        if (outFwStatus) *outFwStatus = status;
        LOG("qpQuery: QPN %u: no answer (fw status 0x%x)", (unsigned)qpn, (unsigned)status);
        return false;
    }
    if (outMailbox) {
        memcpy(outMailbox, fMboxVirt, MLX4_MAILBOX_SIZE);
        if (outObservation) outObservation->capturedBytes = MLX4_MAILBOX_SIZE;
    }
    if (outFwStatus) *outFwStatus = status;
    memcpy(outContext, (const UInt8 *)fMboxVirt + QP_CONTEXT_MAILBOX_OFFSET, need);
    LOG("qpQuery: QPN %u: fw status 0x%x, answer at mailbox +%u: flags 0x%08x (state %u), "
        "local_qpn %u", (unsigned)qpn, (unsigned)status,
        (unsigned)QP_CONTEXT_MAILBOX_OFFSET,
        (unsigned)mlx4GetBe32(outContext + QP_CONTEXT_FLAGS_OFFSET),
        (unsigned)qpState(outContext),
        (unsigned)mlx4GetBe32(outContext + QP_CONTEXT_LOCAL_QPN_OFFSET));
    return true;
}

/*
 * The state nibble, read the way the reference writes it: flags[31:28]
 * (mlx4_qp.c:912-914), so the value is one of MLX4_QP_STATE_* and printable as itself.
 */
UInt32 Mlx4Hw::qpState(const UInt8 *context)
{
    if (!context) return 0xffffffffu;
    return (mlx4GetBe32(context + QP_CONTEXT_FLAGS_OFFSET) >>
            MLX4_QP_STATE_NIBBLE_SHIFT) & 0xfu;
}

/*
 * qpToReset -- the rollback path: 2RST_QP with no mailbox.
 *
 * The reference's shape is different from the other three transitions and the difference
 * is the point (mlx4_qp.c:145-147): in_param 0 and op_modifier 2, with no context at all.
 * Sending the four-bit state change through qpModify would post a mailbox the firmware is
 * not expecting, and a QP left in RTS while the driver releases the memory its MTT points
 * at is how a later command finds the HCA writing into recycled pages.
 */
bool Mlx4Hw::qpToReset(UInt32 qpn, UInt32 *outFwStatus)
{
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;

    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    /*
     * Class A, not class C: this one goes through the reference's early "into reset" path
     * (mlx4_qp.c:145-147), and that path asks for MLX4_CMD_TIME_CLASS_A while the three
     * context-carrying transitions ask for class C (:186).  Both are 60000 in this port's
     * headers, so the value is the same -- the difference is which source line this
     * corresponds to, which is what a future reader would need.
     */
    if (!cmdBox(0, 0, qpn, 2, MLX4_CMD_2RST_QP, MLX4_CMD_TIME_CLASS_A, &status)) {
        if (outFwStatus) *outFwStatus = status;
        LOG("qpToReset: QPN %u: 2RST_QP failed (fw status 0x%x)", (unsigned)qpn,
            (unsigned)status);
        return false;
    }
    if (outFwStatus) *outFwStatus = status;
    LOG("qpToReset: QPN %u: put back to RST (2RST_QP, in_param 0, op_modifier 2)", 
        (unsigned)qpn);
    return true;
}



/*
 * hcrRun -- one command, with the caller's parameters, and the mailbox read back.
 *
 * The shape is deliberately the most literal one available: fill the mailbox (optionally
 * from a caller-supplied buffer), hand in_param/out_param/in_modifier/op_modifier to the
 * command path exactly as given, then copy the mailbox back out.  Nothing is interpreted
 * here, because interpreting is what a general-purpose probe must not do -- the caller
 * decides what the bytes mean, and the bytes are what come back.
 *
 * Two details that are easy to get wrong and are handled here rather than by callers:
 *
 *   - in_param is the mailbox's DMA address when CX3_HCR_F_USE_MAILBOX is set, and 0
 *     otherwise.  The reference's asymmetry comes from which command is being sent, so it
 *     cannot be inferred; it is a flag.
 *
 *   - the mailbox is cleared before an inbox is copied in, and the outbox is copied out
 *     AFTER the command returns and BEFORE anything else uses the mailbox -- the same
 *     ordering bug that made an earlier MPT readback report a byte nobody had written
 *     (section 26.3), here avoided structurally: there is exactly one mailbox and every
 *     path copies out of it immediately.
 */
bool Mlx4Hw::hcrRun(UInt16 op, UInt32 inModifier, UInt8 opMod, UInt32 flags, UInt32 timeoutMs,
                    const UInt8 *inbox, UInt32 inboxLen, UInt8 *outbox, UInt32 outboxLen,
                    UInt32 *outFwStatus, UInt32 *outHcrPost, UInt32 *outHcrFinal,
                    UInt64 *outImm)
{
    UInt32 fwStatus = CX3_FWSTATUS_NOT_POSTED;
    UInt64 inParam = 0, outParam = 0, imm = 0;
    bool wantMailbox = (flags & (1u << 0)) != 0;      /* CX3_HCR_F_USE_MAILBOX */
    bool fillInbox   = (flags & (1u << 1)) != 0;      /* CX3_HCR_F_FILL_INBOX */
    bool bypassBusy  = (flags & (1u << 2)) != 0;      /* CX3_HCR_F_BYPASS_BUSY */
    bool wantImm     = (flags & (1u << 3)) != 0;      /* CX3_HCR_F_WANT_IMM */
    /*
     * The default budget for a command that did not name one: the reference's class A
     * value, as real milliseconds (docs/LOCK-DESIGN.md section 3.2 keeps the reference's
     * 60 s for firmware/init commands and shortens only the teardown class).
     */
    UInt32 timeout   = timeoutMs ? timeoutMs : CX3_CMD_BUDGET_FW_MS;
    bool ok;

    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    if (outImm) *outImm = 0;
    if (outbox && outboxLen) memset(outbox, 0, outboxLen);

    /*
     * Refused before the mailbox is touched, not after.
     *
     * cmdBoxLocked() would refuse this command anyway, but by then the four lines below
     * have already cleared the 4 KiB inbox -- and the one thing that must not happen after
     * an abandonment is that the bytes of the abandoned command move under a device that
     * may still be reading them.  This is the path the operator drives by hand
     * ('cx3ctl hcr'), so it is the path where an unnecessary clear is most likely to
     * happen.
     *
     * This does not make the rule universal: the per-command helpers each clear the
     * mailbox before calling cmdBox() as well, and they are not guarded here.  What is
     * guaranteed after a poison is "no command is posted and the HCR is not touched"; what
     * is not guaranteed is that the mailbox's *contents* are frozen.  docs/LOCK-IMPL.md
     * states that boundary rather than implying more.
     */
    if (fCmdWedged) {
        ++fCmdRefused;
        LOG("hcrRun: op 0x%x refused -- the command mailbox is poisoned by op 0x%x; the "
            "inbox was not cleared and no register was read (fw status 0x%x)",
            (unsigned)op, (unsigned)fCmdWedgedOp, (unsigned)CX3_FWSTATUS_WEDGED);
        if (outFwStatus) *outFwStatus = CX3_FWSTATUS_WEDGED;
        return false;
    }

    if (!mboxAlloc()) {
        LOG("hcrRun: no command mailbox");
        return false;
    }
    if (inboxLen > MLX4_MAILBOX_SIZE || outboxLen > MLX4_MAILBOX_SIZE) {
        LOG("hcrRun: inbox %u or outbox %u exceeds the %u-byte mailbox",
            (unsigned)inboxLen, (unsigned)outboxLen, (unsigned)MLX4_MAILBOX_SIZE);
        return false;
    }

    /*
     * Always clear, then fill if asked.  Clearing unconditionally means a caller who sends
     * no inbox does not inherit whatever the previous command left, which matters more here
     * than anywhere else: this is the path where commands are being tried in sequence and a
     * stale mailbox would look like the firmware's answer.
     */
    memset(fMboxVirt, 0, MLX4_MAILBOX_SIZE);
    if (fillInbox && inbox && inboxLen) memcpy(fMboxVirt, inbox, inboxLen);

    if (wantMailbox) inParam = fMboxPhys;

    /*
     * What is actually in the mailbox, before the command goes out, taken from the buffer
     * rather than from the caller's pointer.  This is the fix for the whole class of mistake
     * section 26.3 was: "the bytes are what I think I put there" is a claim, and a byte read
     * out of the buffer is a fact.  For the first 32 bytes only, because the console echo is
     * not free and most contexts live there.
     */
    {
        const UInt8 *b = (const UInt8 *)fMboxVirt;
        LOG("hcrRun: op 0x%x, in_modifier 0x%x, op_modifier %u, flags 0x%x, %s, "
            "inbox %u byte(s), first 16: %02x %02x %02x %02x %02x %02x %02x %02x "
            "%02x %02x %02x %02x %02x %02x %02x %02x",
            (unsigned)op, (unsigned)inModifier, (unsigned)opMod, (unsigned)flags,
            wantMailbox ? "in_param = mailbox DMA" : "in_param = 0",
            (unsigned)inboxLen, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
            b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    }

    if (wantImm) {
        ok = cmdBoxImm(inParam, inModifier, opMod, op, timeout, &imm, &fwStatus, bypassBusy);
        if (outImm) *outImm = imm;
    } else {
        ok = cmdBox(inParam, outParam, inModifier, opMod, op, timeout, &fwStatus, bypassBusy);
    }

    if (outFwStatus) *outFwStatus = fwStatus;
    if (outHcrFinal) *outHcrFinal = readHcrStatus();

    /*
     * Copy the mailbox out whether or not the command succeeded: on failure the outbox is
     * where the firmware's own explanation lives, and reading it only on success is how a
     * failure ends up described as "it did not work".
     */
    if (outbox && outboxLen) memcpy(outbox, fMboxVirt, outboxLen);
    if (outbox && outboxLen >= 16) {
        const UInt8 *b = (const UInt8 *)outbox;
        LOG("hcrRun: op 0x%x %s (fw status 0x%x); outbox first 16: %02x %02x %02x %02x "
            "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
            (unsigned)op, ok ? "completed" : "did not complete", (unsigned)fwStatus,
            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
            b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* The event queue                                                      */
/* ------------------------------------------------------------------ */

/*
 * mlx4_create_eq() -- FreeBSD mlx4_eq.c:952, reduced to what restart 1 needs:
 * a ring, MTT entries pointing at it, SW2HW_EQ, and the doorbell address.
 *
 * nent is rounded up to a power of two and the page count follows from the EQE
 * size, exactly as the reference does (mlx4_eq.c:967-1003).
 */
/*
 * Log a few bytes of a buffer, with a label saying when they were read.
 *
 * Exists because "the ring holds 0xcc/0xee" has three possible explanations that
 * look identical from a single reading -- the host's clear never took effect,
 * something on the host rewrote the buffer, or the device wrote it -- and only a
 * sequence of readings taken between the steps can tell them apart.  Measured
 * once already with two snapshots placed too late, which is why there are four
 * here, two of them inside eqCreate() itself.
 */
static void mlx4DumpBytes(const char *label, const UInt8 *p, UInt32 n)
{
    static const char hexd[] = "0123456789abcdef";
    char line[2 * 32 + 1];
    UInt32 i;

    if (!p || n == 0) return;
    if (n > 32) n = 32;
    for (i = 0; i < n; ++i) {
        line[i * 2]     = hexd[(p[i] >> 4) & 0xf];
        line[i * 2 + 1] = hexd[p[i] & 0xf];
    }
    line[n * 2] = 0;
    LOG("ring bytes [%s]: %s", label, line);
}

bool Mlx4Hw::eqCreate(UInt32 nent, UInt8 intr, UInt8 eqeFactor, UInt32 eqn,
                      bool mttAbs, Mlx4Icm **outRing, UInt32 *outPages,
                      UInt32 *outMTTOffset, UInt64 *outMTTAddr,
                      UInt64 *outDoorbellAddr, UInt32 *outFwStatus)
{
    UInt32 eqeSize = MLX4_EQ_ENTRY_SIZE << eqeFactor;   /* 32 or 64 */
    UInt32 n = 1, pages, mttOffset = 0, status = CX3_FWSTATUS_NOT_POSTED;
    UInt64 mttAddr = 0, *list = 0;
    UInt32 mttAddr32 = 0;
    UInt8 *ctx;
    Mlx4Icm *ring;
    UInt32 i;
    bool ok = false;
    const auto exposeRetainedOwner = [&]() {
        if (outRing) *outRing = ring;
        if (outPages) *outPages = pages;
        if (outMTTOffset) *outMTTOffset = mttOffset;
        if (outMTTAddr) *outMTTAddr = mttAddr;
        if (outDoorbellAddr)
            *outDoorbellAddr = (UInt64)(((eqn / 4) << MLX4_UAR_PAGE_SHIFT) +
                                        MLX4_EQ_DOORBELL_OFFSET + 8 * (eqn % 4));
    };

    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    if (outRing) *outRing = 0;
    if (outPages) *outPages = 0;
    if (outMTTOffset) *outMTTOffset = 0;
    if (outMTTAddr) *outMTTAddr = 0;
    if (outDoorbellAddr) *outDoorbellAddr = 0;

    if (nent < 2) nent = 2;
    while (n < nent) n <<= 1;                 /* roundup_pow_of_two */
    nent = n;

    pages = (nent * eqeSize + 4095u) >> 12;
    if (pages == 0) return false;

    /* 1. the ring: 4 KiB pages whose bus addresses are known individually. */
    ring = icmAlloc(pages, false);
    if (!ring) {
        LOG("eqCreate: could not allocate %u pages for a %u-entry EQ ring",
            (unsigned)pages, (unsigned)nent);
        return false;
    }
    memset(ring->virt, 0, (size_t)ring->bytes);
    /*
     * The clear, checked immediately.  If these bytes are not zero the problem is
     * the buffer or the pointer; if they are zero and a later reading is not, the
     * problem is whatever writes in between, and the remaining three dumps say
     * which step did it.
     */
    mlx4DumpBytes("eqCreate: right after memset(0) of the whole ring",
                  (const UInt8 *)ring->virt, 32);
    /*
     * A write/read self-check on the ring's own kernel address.  The ring is the
     * one buffer here whose contents are in question, so instead of trusting that
     * a store to this address lands, it is verified: pattern, read back, restore.
     */
    {
        UInt8 *r = (UInt8 *)ring->virt;
        UInt32 i;
        bool same = true;
        for (i = 0; i < 32; ++i) r[i] = 0x5a;
        for (i = 0; i < 32; ++i) if (r[i] != 0x5a) { same = false; break; }
        mlx4DumpBytes("eqCreate: after writing 0x5a to the first 32 bytes", r, 32);
        LOG("eqCreate: ring write/read self-check: %s",
            same ? "a store to this address reads back unchanged"
                 : "A STORE DID NOT READ BACK -- the ring address is not coherent");
        memset(r, 0, (size_t)ring->bytes);
        mlx4DumpBytes("eqCreate: after restoring the clear", r, 32);
    }

    /* 2. MTT entries for those pages. */
    if (!mttAlloc(pages, ring, &mttOffset, &mttAddr32)) {
        LOG("eqCreate: no MTT entries for %u pages", (unsigned)pages);
        icmFree(ring);
        return false;
    }
    mttAddr = mttAddr32;
    list = (UInt64 *)IOMalloc(sizeof(UInt64) * pages);
    if (!list) {
        if (!mttFree(mttOffset, pages, ring)) { exposeRetainedOwner(); return false; }
        icmFree(ring);
        return false;
    }
    for (i = 0; i < pages; ++i) list[i] = ring->pages[i].busAddr;
    if (!mttWrite(mttOffset, pages, list)) {
        IOFree(list, sizeof(UInt64) * pages);
        if (!mttFree(mttOffset, pages, ring)) { exposeRetainedOwner(); return false; }
        icmFree(ring);
        return false;
    }
    IOFree(list, sizeof(UInt64) * pages);

    /* 3. the EQ context, which is the SW2HW_EQ inbox. */
    if (!mboxAlloc()) {
        if (!mttFree(mttOffset, pages, ring)) { exposeRetainedOwner(); return false; }
        icmFree(ring);
        return false;
    }
    ctx = (UInt8 *)fMboxVirt;
    memset(ctx, 0, MLX4_MAILBOX_SIZE);
    /* mlx4_eq.c:1023-1031.  STATUS_OK and STATE_ARMED are both zero in the high
     * bits of the flags dword, so writing the state field is what arms it. */
    mlx4PutBe32(ctx + EQ_CONTEXT_FLAGS_OFFSET, MLX4_EQ_STATUS_OK | MLX4_EQ_STATE_ARMED);
    ctx[EQ_CONTEXT_LOG_EQ_SIZE_OFFSET]  = (UInt8)mlx4Log2(nent);
    ctx[EQ_CONTEXT_INTR_OFFSET]         = intr;
    /*
     * log_page_size = PAGE_SHIFT - MLX4_ICM_PAGE_SHIFT, i.e. 0 when the HCA's ICM
     * page size equals the host page size (mlx4_eq.c:1027).
     */
    ctx[EQ_CONTEXT_LOG_PAGE_SIZE_OFFSET] = (UInt8)(12 - MLX4_ICM_PAGE_SHIFT);
    /*
     * The one field whose units are not derivable from the material we have.
     *
     * The reference passes mlx4_mtt_addr() = offset * mtt_entry_sz, which is a
     * byte offset inside the MTT table -- for this port, 64 * 8 = 0x200.  If the
     * firmware instead expects an address in the HCA's context-memory space, the
     * right value is the table's own base plus that offset, which INIT_HCA
     * announced (0x1000c0000 in this profile).  Both forms are one call apart and
     * only one of them can be right, so the caller picks and the value is logged.
     */
    {
        UInt64 ctxMtt = mttAddr;
        if (mttAbs) ctxMtt = fMttHcaVirt + mttAddr;
        LOG("eqCreate: EQ context mtt_base_addr = 0x%llx (%s form: %s)",
            (unsigned long long)ctxMtt, mttAbs ? "absolute" : "offset",
            mttAbs ? "table base + offset" : "offset inside the MTT table");
        ctx[EQ_CONTEXT_MTT_BASE_H_OFFSET] = (UInt8)(ctxMtt >> 32);
        mlx4PutBe32(ctx + EQ_CONTEXT_MTT_BASE_L_OFFSET, (UInt32)(ctxMtt & 0xffffffffu));
    }

    LOG("eqCreate: nent %u, %u pages, EQE size %u (factor %u), MTT offset %u addr 0x%llx, "
        "context flags 0x%08x log_eq_size %u intr %u log_page_size %u",
        (unsigned)nent, (unsigned)pages, (unsigned)eqeSize, (unsigned)eqeFactor,
        (unsigned)mttOffset, (unsigned long long)mttAddr,
        (unsigned)(MLX4_EQ_STATUS_OK | MLX4_EQ_STATE_ARMED),
        (unsigned)mlx4Log2(nent), (unsigned)intr,
        (unsigned)(12 - MLX4_ICM_PAGE_SHIFT));

    /* 4. SW2HW_EQ: in_param is the context mailbox, in_modifier the EQ number
     * (mlx4_eq.c:895-900). */
    mlx4DumpBytes("eqCreate: just before SW2HW_EQ", (const UInt8 *)ring->virt, 32);
    {
        Mlx4Icm *handoff[1] = { ring };
        if (!markDmaHandoff(handoff, 1)) {
            LOG("eqCreate: refusing SW2HW_EQ because the EQ ring generation is unbound");
            if (!mttFree(mttOffset, pages, ring)) { exposeRetainedOwner(); return false; }
            icmFree(ring);
            if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
            return false;
        }
    }
    ok = cmdBox(fMboxPhys, 0, eqn, 0, MLX4_CMD_SW2HW_EQ, MLX4_CMD_TIME_CLASS_A, &status);
    if (outFwStatus) *outFwStatus = status;
    if (!ok) {
        /*
         * A timeout is NOT a rejection, and this used to treat the two the same.
         *
         * One source review caught it: every SW2HW_EQ failure fell through to the
         * same "free the ring and the MTT entries" path, including the timeout whose
         * GO bit never cleared.  A timeout means the command did not complete -- it
         * says nothing about whether the hardware accepted the context and the page
         * list first.  Freeing host memory the device may still be translating
         * through is exactly the failure this file refuses to risk elsewhere
         * (teardownEq keeps a ring it cannot hand back for the same reason).
         *
         * So: a firmware status that is really a rejection still frees, because the
         * device said no.  A timeout leaks the ring and the MTT entries on purpose
         * and says so.
         */
        if (status == CX3_FWSTATUS_TIMEOUT) {
            LOG("eqCreate: SW2HW_EQ for EQN %u TIMED OUT -- the HCA may have taken the "
                "context before the command never completed, so the %u-page ring and its "
                "MTT entries are deliberately NOT freed (fw status 0x%x). Only a reboot "
                "or a reset reclaims them.", (unsigned)eqn, (unsigned)pages,
                (unsigned)status);
            /* Preserve a retryable host owner.  The outputs describe retained
             * resources only; the EQ is not established or safe to consume. */
            exposeRetainedOwner();
            return false;
        }
        LOG("eqCreate: SW2HW_EQ failed for EQN %u (fw status 0x%x)",
            (unsigned)eqn, (unsigned)status);
        if (!mttFree(mttOffset, pages, ring)) { exposeRetainedOwner(); return false; }
        icmFree(ring);
        return false;
    }
    /*
     * The moment that matters: if the clear survived everything the host did, and
     * this reading differs, then the HCA wrote the ring while accepting the queue
     * -- which would be an answer, not a mystery.
     */
    mlx4DumpBytes("eqCreate: right after SW2HW_EQ succeeded",
                  (const UInt8 *)ring->virt, 32);

    if (outRing)  *outRing = ring;
    if (outPages) *outPages = pages;
    if (outMTTOffset) *outMTTOffset = mttOffset;
    if (outMTTAddr) *outMTTAddr = mttAddr;
    if (outDoorbellAddr)
        *outDoorbellAddr = /* mlx4_eq.c:939 */
            (UInt64)(((eqn / 4) << MLX4_UAR_PAGE_SHIFT) + MLX4_EQ_DOORBELL_OFFSET +
                     8 * (eqn % 4));
    LOG("eqCreate: EQN %u ready (fw status 0x%x); doorbell at BAR2+0x%llx",
        (unsigned)eqn, (unsigned)status,
        (unsigned long long)(outDoorbellAddr ? *outDoorbellAddr : 0ull));
    return true;
}

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* M3 R3: is the ring initialisation done BY the firmware, for THIS ring? */
/* ------------------------------------------------------------------ */

static UInt32 abNonzero(const UInt8 *p, UInt64 n)
{
    UInt64 i;
    UInt32 c = 0;
    for (i = 0; i < n; ++i) if (p[i]) ++c;
    return c;
}

/* Slots whose owner bit reads 1.  The owner byte's position comes from the same
 * definition the consumer uses, so this cannot drift away from it. */
static UInt32 abOwnerSet(const UInt8 *p, UInt32 nent, UInt8 eqeFactor)
{
    UInt32 stride = MLX4_EQ_ENTRY_SIZE << eqeFactor;
    UInt64 off    = MLX4_EQE_OWNER_BYTE(eqeFactor);
    UInt32 i, c = 0;
    for (i = 0; i < nent; ++i)
        if (p[(UInt64)i * stride + off] & 0x80) ++c;
    return c;
}

/*
 * A byte-by-byte diff against a stored baseline snapshot.
 *
 * Both reviews asked for this instead of a digest or a nonzero count: a digest says
 * "something changed" but not how much or where, and a nonzero count cannot tell
 * "untouched" from "written with the same pattern".  A diff gives the count, the
 * range and the per-page account, and both sides of the comparison are copies taken
 * at one instant rather than a live buffer read twice.
 */
static UInt32 abDiff(const UInt8 *now, const UInt8 *base, UInt64 bytes,
                     UInt32 *outLo, UInt32 *outHi, UInt32 *outPerPage)
{
    UInt64 i;
    UInt32 changed = 0;
    UInt32 lo = 0xffffffffu, hi = 0xffffffffu;
    UInt32 pp[CX3_AB_PAGES];
    UInt32 k;

    for (k = 0; k < CX3_AB_PAGES; ++k) pp[k] = 0;
    for (i = 0; i < bytes; ++i) {
        if (now[i] == base[i]) continue;
        ++changed;
        if (lo == 0xffffffffu) lo = (UInt32)i;
        hi = (UInt32)i;
        k = (UInt32)(i >> MLX4_ICM_PAGE_SHIFT);
        if (k < CX3_AB_PAGES) ++pp[k];
    }
    if (outLo) *outLo = lo;
    if (outHi) *outHi = hi;
    if (outPerPage) for (k = 0; k < CX3_AB_PAGES; ++k) outPerPage[k] = pp[k];
    return changed;
}

/*
 * Real wall time, in nanoseconds.
 *
 * The figure this project had been quoting ("238 ms until the ring settled") was
 * 238 iterations of {memory read + IOSleep(1)} -- and the command poll's own
 * calibration shows one iteration is not one millisecond (60000 iterations took
 * 185.36 s, about 3.09 ms each).  A number that is going to be quoted as a
 * duration has to be measured as one.
 */
static UInt64 abNowNs()
{
    return cx3NowNs();
}

/*
 * The A/B ring-initialisation experiment (M2B-NOTES section 19).
 *
 * What it is for
 * --------------
 * This port measured that a freshly cleared EQ ring is no longer clear after
 * SW2HW_EQ accepts it (zero -> 0xcc).  That reading is real, and it does not show
 * who wrote: the host writes into that memory too, and a single ring has no control.
 * A hypothesis that explains an observation is not evidence for itself.
 *
 * What it does
 * ------------
 * Two rings, built by the same code, on the same clock:
 *
 *   one is handed to SW2HW_EQ;  the other is not, and never is.
 *
 * Both are diffed against their own baseline at every sample, so the untouched ring
 * is the control for everything the host does -- allocation, MTT writes, elapsed
 * time, the command mailbox being used at all.  The input chooses WHICH ring is
 * handed over, so the experiment can be run twice with the roles swapped: if the
 * change follows the ring that was handed over rather than the ring at a particular
 * address, the two runs disagree about which ring changed.
 *
 * Deliberately not done here: MAP_EQ, the consumer doorbell, and any event.  The
 * reference does those as separate steps after SW2HW_EQ (mlx4_eq.c:1015-1033 then
 * :1270-1277), and mixing them in would leave "the ring changed" ambiguous between
 * queue acceptance and event routing.
 *
 * Known limit, stated rather than papered over: SW2HW_EQ is synchronous, so the
 * samples begin when it returns.  A change that happens *while* the command is being
 * processed shows up as "already changed at the first sample".  That is still a
 * usable answer -- it places the write at the command -- but the process inside the
 * command cannot be observed from here; that would need a second thread, which is a
 * bigger change than this diagnostic earns.
 */
bool Mlx4Hw::eqAbExperiment(bool handOverB, UInt32 eqn, UInt32 nent, UInt8 eqeFactor,
                            UInt32 dummyMtt, Cx3AbRingResult *out)
{
    /* Sampled from the moment the command returned, in real microseconds. */
    static const UInt32 kSampleUs[CX3_AB_SAMPLES] =
        { 0, 5000, 20000, 60000, 150000, 300000, 600000, 1000000 };
    const UInt32 eqeSize = MLX4_EQ_ENTRY_SIZE << eqeFactor;
    const UInt32 handed  = handOverB ? 1u : 0u;
    const UInt32 control = handed ^ 1u;

    Mlx4Icm *ring[2] = { 0, 0 };
    UInt32   mttOff[2] = { 0, 0 };
    bool     mttAllocated[2] = { false, false };
    /*
     * UInt64, not UInt32, and that is the whole bug this comment exists to prevent
     * from coming back.
     *
     * These were UInt32 and the context then did `(UInt8)(mttAddr[handed] >> 32)`.
     * Shifting a 32-bit value by 32 is undefined behaviour; on this machine clang
     * emits a shift whose count the hardware masks to 5 bits, so `>> 32` behaved as
     * `>> 0` and the "high byte" written was really the address's LOW byte.
     *
     * The consequence looked like a hardware rule for several hours: address 0x200
     * has low byte 0x00, so it encoded correctly and the ring was written; 0x240 has
     * low byte 0x40, so the context got mtt_base = 0x4000000240 and the ring was
     * never written; 0x300 and 0x400 have low byte 0x00 and worked again.  Ten runs
     * fitted "the MTT address must be a multiple of 256 bytes" perfectly -- because
     * "low byte is zero" and "multiple of 256" are the same predicate, and only the
     * first is causal.  The QUERY_EQ readback settled it: the firmware stored H=0x40
     * for the 0x240 case, which is exactly the corrupted encoding, not a normalised
     * address as an alignment rule would have produced.
     *
     * The production path never had this: eqCreate() declares `UInt64 ctxMtt` first
     * and shifts that.  It was the diagnostic that used the narrower type, which is
     * why only the diagnostic was wrong.
     */
    UInt64   mttAddr[2] = { 0, 0 };
    UInt32   n = 1, pages, i, r, s, status = CX3_FWSTATUS_NOT_POSTED;
    UInt64  *list = 0;
    UInt8   *base[2] = { 0, 0 };        /* baseline snapshots, one per ring */
    UInt8   *snap = 0;                  /* the working full copy, reused */
    UInt8   *prevSnap[2] = { 0, 0 };    /* the previous sample, for adjacent diffs */
    UInt8   *ctx;
    bool     ok = false;
    /*
     * Set once the firmware has been handed a ring, cleared only by a successful
     * HW2SW_EQ.  It gates the free below for the same reason the production teardown
     * does: freeing memory a device still holds a page list for is how the device
     * ends up writing into whatever the host puts there next.  Leaking a ring is the
     * cheaper failure.
     */
    bool     fwMayHold[2] = { false, false };
    UInt64   tBefore, tAfter, tZero;

    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->handedOver  = handed;
    out->dummyMtt    = dummyMtt;
    out->firstChangeUs[0] = out->firstChangeUs[1] = 0xffffffffu;
    out->lastChangeUs[0]  = out->lastChangeUs[1]  = 0xffffffffu;
    out->changedOffsetLo[0] = out->changedOffsetLo[1] = 0xffffffffu;
    out->changedOffsetHi[0] = out->changedOffsetHi[1] = 0xffffffffu;
    for (s = 0; s < CX3_AB_SAMPLES; ++s) out->sampleUs[s] = kSampleUs[s];

    while (n < nent) n <<= 1;
    nent  = n;
    pages = (nent * eqeSize + (MLX4_ICM_PAGE_SIZE - 1)) >> MLX4_ICM_PAGE_SHIFT;
    if (pages == 0 || pages > CX3_AB_PAGES) {
        LOG("abring: %u pages is outside the %u-page window this experiment samples",
            (unsigned)pages, (unsigned)CX3_AB_PAGES);
        return false;
    }
    out->nent    = nent;
    out->pages   = pages;
    out->samples = CX3_AB_SAMPLES;

    if (!retryAbRetainedRings()) {
        LOG("abring: refusing a new experiment because a previous ring owner could not be "
            "returned and released; retained owner metadata is preserved for retry");
        out->unknown = 1;
        return false;
    }

    if (!fMttAllocator.canRunAbExperiment(dummyMtt, fDeviceGeneration)) {
        LOG("abring: refusing an ambiguous MTT layout; the A/B rings require an otherwise idle "
            "MTT allocator and a reservation anchored at its first usable entry");
        goto done;
    }
    if (!dummyMtt && !fMttAllocator.releaseAbReservationIfUnowned()) {
        LOG("abring: refusing the no-dummy variant while the prior reservation is pinned by "
            "another live MTT owner");
        goto done;
    }

    /*
     * 0. Optional, generation-bound dummy MTT reservation.
     *
     * It exists to move the two rings up the table so their offsets are not 64 and
     * 72 -- see kAbRingInDummyMtt for why that matters. It is reused across A/B
     * runs and reclaimed only after no other live MTT owner can depend on the table.
     */
    if (dummyMtt) {
        UInt32 dummyOff = 0, dummyAddr = 0;
        if (!mttAllocAbDummy(dummyMtt, &dummyOff, &dummyAddr)) {
            LOG("abring: could not allocate %u dummy MTT entries", (unsigned)dummyMtt);
            goto done;
        }
        LOG("abring: %u tracked dummy MTT entries at offset %u (HCA 0x%x) place the two "
            "rings at offset %u instead of 64",
            (unsigned)dummyMtt, (unsigned)dummyOff, (unsigned)dummyAddr,
            (unsigned)(dummyOff + dummyMtt));
    }

    /* 1. Two rings, built identically.  icmAlloc hands back zeroed memory, but the
     *    clear is made explicit because "the ring starts at zero" is the baseline
     *    this whole experiment is measured against. */
    for (r = 0; r < 2; ++r) {
        ring[r] = icmAlloc(pages, false);
        if (!ring[r]) {
            LOG("abring: could not allocate ring %c (%u pages)", r ? 'B' : 'A',
                (unsigned)pages);
            goto done;
        }
        memset(ring[r]->virt, 0, (size_t)ring[r]->bytes);
        out->ringVirt[r] = (UInt64)(uintptr_t)ring[r]->virt;
        out->busFirst[r] = ring[r]->pages[0].busAddr;
        out->busLast[r]  = ring[r]->pages[pages - 1].busAddr;
        /*
         * mttAlloc hands back a 32-bit address; it is widened into the UInt64 here,
         * which is where the width bug above needs to be prevented -- the shift that
         * reads the high byte must be applied to the 64-bit value.
         */
        UInt32 mttAddr32 = 0;
        if (!mttAlloc(pages, ring[r], &mttOff[r], &mttAddr32)) {
            LOG("abring: no MTT entries for %u pages (ring %c)", (unsigned)pages,
                r ? 'B' : 'A');
            goto done;
        }
        mttAllocated[r] = true;
        mttAddr[r] = mttAddr32;
        out->mttEntryOffset[r] = (UInt64)mttOff[r] * MLX4_MTT_ENTRY_SIZE;
    }
    list = (UInt64 *)IOMalloc(sizeof(UInt64) * pages);
    if (!list) { LOG("abring: no room for the page-address list"); goto done; }
    for (r = 0; r < 2; ++r) {
        for (i = 0; i < pages; ++i) list[i] = ring[r]->pages[i].busAddr;
        if (!mttWrite(mttOff[r], pages, list)) {
            LOG("abring: MTT write failed for ring %c", r ? 'B' : 'A');
            goto done;
        }
    }
    IOFree(list, sizeof(UInt64) * pages);
    list = 0;

    /*
     * The two rings must not share a page or an MTT entry, or "the control ring did
     * not change" would mean nothing.  Checked, not assumed.
     */
    if (mttOff[0] == mttOff[1]) {
        LOG("abring: the two rings were given the same MTT entries (%u) -- stopping",
            (unsigned)mttOff[0]);
        goto done;
    }
    if (ring[0]->bytes != ring[1]->bytes) {
        LOG("abring: the two rings are not the same size; stopping");
        goto done;
    }
    /*
     * The isolation check both reviews asked to be completed.
     *
     * It used to compare only A[i] with B[i] and the two MTT start offsets -- which
     * cannot see an alias between A[i] and B[j] for i != j, nor an overlap between
     * the two MTT entry *ranges* when their starts differ, nor a duplicate page
     * inside one ring.  Any of those would make "the control ring did not change"
     * meaningless, so all three are checked over the full sets rather than at the
     * diagonal.
     */
    for (i = 0; i < pages; ++i) {
        UInt32 j;
        for (j = 0; j < pages; ++j) {
            if (ring[0]->pages[i].busAddr == ring[1]->pages[j].busAddr) {
                LOG("abring: ring A page %u and ring B page %u share bus address "
                    "0x%llx -- alias, stopping", (unsigned)i, (unsigned)j,
                    (unsigned long long)ring[0]->pages[i].busAddr);
                goto done;
            }
        }
        for (j = i + 1; j < pages; ++j) {
            if (ring[0]->pages[i].busAddr == ring[0]->pages[j].busAddr) {
                LOG("abring: ring A itself repeats bus address 0x%llx at pages %u and %u "
                    "-- stopping", (unsigned long long)ring[0]->pages[i].busAddr,
                    (unsigned)i, (unsigned)j);
                goto done;
            }
        }
    }
    /* The MTT entry ranges must not intersect, not merely differ at their start. */
    {
        UInt32 loA = mttOff[0], hiA = mttOff[0] + pages;
        UInt32 loB = mttOff[1], hiB = mttOff[1] + pages;
        if (loA < hiB && loB < hiA) {
            LOG("abring: the two MTT entry ranges overlap (A %u..%u, B %u..%u) -- "
                "stopping", (unsigned)loA, (unsigned)hiA, (unsigned)loB, (unsigned)hiB);
            goto done;
        }
    }
    LOG("abring: ring A virt 0x%llx pages 0x%llx..0x%llx MTT +0x%llx; ring B virt 0x%llx "
        "pages 0x%llx..0x%llx MTT +0x%llx -- no shared page, no shared MTT entry",
        (unsigned long long)out->ringVirt[0],
        (unsigned long long)out->busFirst[0], (unsigned long long)out->busLast[0],
        (unsigned long long)out->mttEntryOffset[0],
        (unsigned long long)out->ringVirt[1],
        (unsigned long long)out->busFirst[1], (unsigned long long)out->busLast[1],
        (unsigned long long)out->mttEntryOffset[1]);

    /* 2. Baselines.  Full copies, not digests. */
    for (r = 0; r < 2; ++r) {
        base[r] = (UInt8 *)IOMalloc((size_t)ring[r]->bytes);
        if (!base[r]) {
            LOG("abring: no room for ring %c's baseline snapshot", r ? 'B' : 'A');
            goto done;
        }
        memcpy(base[r], ring[r]->virt, (size_t)ring[r]->bytes);
        out->nonzeroBase[r]  = abNonzero(base[r], ring[r]->bytes);
        out->ownerSetBase[r] = abOwnerSet(base[r], nent, eqeFactor);
        memcpy(out->headBefore[r], base[r], CX3_AB_HEAD);
        LOG("abring: ring %c baseline: %u nonzero byte(s) of %llu, %u of %u slots owner=1",
            r ? 'B' : 'A', (unsigned)out->nonzeroBase[r],
            (unsigned long long)ring[r]->bytes, (unsigned)out->ownerSetBase[r],
            (unsigned)nent);
    }
    snap = (UInt8 *)IOMalloc((size_t)ring[0]->bytes);
    if (!snap) { LOG("abring: no room for the working snapshot"); goto done; }
    for (r = 0; r < 2; ++r) {
        prevSnap[r] = (UInt8 *)IOMalloc((size_t)ring[r]->bytes);
        if (!prevSnap[r]) {
            LOG("abring: no room for ring %c's previous-sample snapshot", r ? 'B' : 'A');
            goto done;
        }
        /* Sample 0's "previous" is the baseline: at that point nothing has run
         * except what the baseline already captured. */
        memcpy(prevSnap[r], base[r], (size_t)ring[r]->bytes);
    }

    /* 3. The EQ context, built exactly as eqCreate builds it -- same fields, same
     *    units.  A context that differed would make the experiment measure the
     *    difference instead. */
    if (!mboxAlloc()) { LOG("abring: no command mailbox"); goto done; }
    ctx = (UInt8 *)fMboxVirt;
    memset(ctx, 0, MLX4_MAILBOX_SIZE);
    mlx4PutBe32(ctx + EQ_CONTEXT_FLAGS_OFFSET, MLX4_EQ_STATUS_OK | MLX4_EQ_STATE_ARMED);
    ctx[EQ_CONTEXT_LOG_EQ_SIZE_OFFSET]   = (UInt8)mlx4Log2(nent);
    ctx[EQ_CONTEXT_INTR_OFFSET]          = 0;   /* the asynchronous EQ's intr */
    ctx[EQ_CONTEXT_LOG_PAGE_SIZE_OFFSET] = 0;   /* host page == ICM page */
    ctx[EQ_CONTEXT_MTT_BASE_H_OFFSET]    = (UInt8)(mttAddr[handed] >> 32);
    mlx4PutBe32(ctx + EQ_CONTEXT_MTT_BASE_L_OFFSET,
                (UInt32)(mttAddr[handed] & 0xffffffffu));

    /*
     * Log the bytes that are actually in the inbox, not the expression that filled
     * them.
     *
     * Both reviews of this experiment asked for this, and the reason is the bug above:
     * "the high byte is zero" was an inference from `mttAddr < 2^32`, and that
     * inference was what went wrong.  A value read back out of the buffer cannot be
     * wrong about what the buffer contains.  It is read before the command is posted,
     * because QUERY_EQ reuses the same mailbox and would overwrite it.
     */
    out->inboxMttH = ctx[EQ_CONTEXT_MTT_BASE_H_OFFSET];
    out->inboxMttL = ((UInt32)ctx[EQ_CONTEXT_MTT_BASE_L_OFFSET] << 24) |
                     ((UInt32)ctx[EQ_CONTEXT_MTT_BASE_L_OFFSET + 1] << 16) |
                     ((UInt32)ctx[EQ_CONTEXT_MTT_BASE_L_OFFSET + 2] << 8) |
                      (UInt32)ctx[EQ_CONTEXT_MTT_BASE_L_OFFSET + 3];
    LOG("abring: encoding ring %c's mtt_base: address 0x%llx -> inbox h=0x%02x "
        "l=0x%08x (reassembled 0x%llx; must equal the address)",
        handed ? 'B' : 'A', (unsigned long long)mttAddr[handed],
        (unsigned)out->inboxMttH, (unsigned)out->inboxMttL,
        (unsigned long long)(((UInt64)out->inboxMttH << 32) | out->inboxMttL));

    LOG("abring: handing ring %c (MTT offset %u -> 0x%08x) to SW2HW_EQ as EQN %u; "
        "ring %c stays in the host's hands as the control",
        handed ? 'B' : 'A', (unsigned)mttOff[handed], (unsigned)mttAddr[handed],
        (unsigned)eqn, control ? 'B' : 'A');

    {
        Mlx4Icm *handoff[1] = { ring[handed] };
        if (!markDmaHandoff(handoff, 1)) {
            LOG("abring: refusing SW2HW_EQ because the selected ring has no valid generation");
            goto done;
        }
    }
    /* A timeout or failed reply does not establish that firmware rejected the ring. */
    fwMayHold[handed] = true;
    tBefore = abNowNs();
    ok = cmdBox(fMboxPhys, 0, eqn, 0, MLX4_CMD_SW2HW_EQ, MLX4_CMD_TIME_CLASS_A, &status);
    tAfter  = abNowNs();
    out->sw2hwStatus = status;
    out->cmdUs = (UInt32)((tAfter - tBefore) / 1000ull);
    LOG("abring: SW2HW_EQ returned after %u us (fw status 0x%x)",
        (unsigned)out->cmdUs, (unsigned)status);
    if (!ok) {
        LOG("abring: SW2HW_EQ failed (fw status 0x%x); the experiment did not run",
            (unsigned)status);
        goto done;
    }
    tZero = tAfter;

    /*
     * What did the firmware record for this EQN?
     *
     * This is the question the nine-run pattern forces: handing one ring over writes it
     * every time, handing the other writes nothing, and no host-side difference between
     * the two rings has been found.  "SW2HW_EQ returned 0" says the command was
     * accepted; it does not say the context was stored, nor that the stored MTT base is
     * the one that was sent.  QUERY_EQ is the only thing here that asks the firmware
     * what it thinks it has.
     */
    {
        Mlx4EqContextResult q;
        UInt32 qst = CX3_FWSTATUS_NOT_POSTED;
        if (queryEq(eqn, &q, &qst)) {
            out->queryEqStatus[0] = qst;
            memcpy(out->queryEqWords[0], q.words, sizeof(out->queryEqWords[0]));
            LOG("abring: QUERY_EQ(EQN %u) after SW2HW_EQ: fw status 0x%x, words "
                "%08x %08x %08x %08x %08x %08x %08x %08x",
                (unsigned)eqn, (unsigned)qst,
                (unsigned)q.words[0], (unsigned)q.words[1], (unsigned)q.words[2],
                (unsigned)q.words[3], (unsigned)q.words[4], (unsigned)q.words[5],
                (unsigned)q.words[6], (unsigned)q.words[7]);
        } else {
            out->queryEqStatus[0] = qst ? qst : 0xffffffffu;
            LOG("abring: QUERY_EQ(EQN %u) after SW2HW_EQ FAILED (fw status 0x%x)",
                (unsigned)eqn, (unsigned)qst);
        }
    }

    /*
     * And what the host's own MTT table says, read back from the entries the context
     * points at -- the plain host-memory load, no device involved.  If this does not
     * match the ring's page bus addresses, the page list is the problem, not the
     * firmware's willingness to write.
     */
    for (r = 0; r < 2; ++r) {
        UInt64 first = mttEntryValue(mttOff[r]);
        UInt64 last  = mttEntryValue(mttOff[r] + pages - 1);
        out->mttFirstEntry[r] = (UInt32)(first & 0xffffffffu);
        out->mttLastEntry[r]  = (UInt32)(last & 0xffffffffu);
        LOG("abring: MTT readback for ring %c: entry %u = 0x%llx (ring page bus addr "
            "0x%llx, %s), entry %u = 0x%llx (ring last page 0x%llx, %s)",
            r ? 'B' : 'A', (unsigned)mttOff[r], (unsigned long long)first,
            (unsigned long long)(ring[r]->pages[0].busAddr | 1ull),
            (first == (ring[r]->pages[0].busAddr | 1ull)) ? "match" : "MISMATCH",
            (unsigned)(mttOff[r] + pages - 1), (unsigned long long)last,
            (unsigned long long)(ring[r]->pages[pages - 1].busAddr | 1ull),
            (last == (ring[r]->pages[pages - 1].busAddr | 1ull)) ? "match" : "MISMATCH");
    }

    /* 4. Eight real time points, both rings, full diffs. */
    for (s = 0; s < CX3_AB_SAMPLES; ++s) {
        UInt32 relUs;
        while (s > 0 && (abNowNs() - tZero) / 1000ull < kSampleUs[s]) IOSleep(1);
        relUs = (UInt32)((abNowNs() - tZero) / 1000ull);
        for (r = 0; r < 2; ++r) {
            UInt32 lo = 0xffffffffu, hi = 0xffffffffu;
            memcpy(snap, ring[r]->virt, (size_t)ring[r]->bytes);
            out->changedPerSample[r][s] =
                abDiff(snap, base[r], ring[r]->bytes, &lo, &hi, out->changedPerPage[r]);
            out->nonzeroPerSample[r][s]  = abNonzero(snap, ring[r]->bytes);
            out->ownerSetPerSample[r][s] = abOwnerSet(snap, nent, eqeFactor);
            /*
             * Difference against the PREVIOUS sample, not against the baseline.
             *
             * One review caught that "lastChangeUs" was computed from baseline
             * diffs, so it meant "the last moment this still differed from the
             * beginning" -- not "the last moment anything changed".  A ring that
             * was written, then restored to its baseline and written again, or that
             * keeps being rewritten with identical bytes, produces the same
             * baseline-diff at every sample while it is being written the whole
             * time.  With this, "still changing at +600 ms" is a claim the data can
             * actually support, and identical adjacent snapshots are evidence that
             * nothing happened in between.
             */
            if (s > 0) {
                out->changedVsPrev[r][s] =
                    abDiff(snap, prevSnap[r], ring[r]->bytes, 0, 0, 0);
            }
            memcpy(prevSnap[r], snap, (size_t)ring[r]->bytes);
            if (out->changedPerSample[r][s]) {
                if (out->firstChangeUs[r] == 0xffffffffu) {
                    out->firstChangeUs[r] = relUs;
                    out->changedOffsetLo[r] = lo;
                    out->changedOffsetHi[r] = hi;
                    memcpy(out->headFirst[r], snap, CX3_AB_HEAD);
                }
            }
            if (s > 0 && out->changedVsPrev[r][s]) out->lastChangeUs[r] = relUs;
        }
        /*
         * The byte counts are 64-bit and their two %llu specifiers were missing, so every
         * argument after the first two was shifted by one and the whole line printed
         * misaligned numbers -- found by -Wformat, which is now part of the build.  A
         * diagnostic that prints a wrong number is worse than one that prints nothing, which
         * is why this is fixed rather than reformatted away.
         */
        LOG("abring: t=+%7u us  ring A: %6u vs baseline of %llu, %6u vs previous "
            "(%u nonzero, %u owner=1)   ring B: %6u vs baseline of %llu, %6u vs previous "
            "(%u nonzero, %u owner=1)",
            (unsigned)relUs,
            (unsigned)out->changedPerSample[0][s], (unsigned long long)ring[0]->bytes,
            (unsigned)out->changedVsPrev[0][s],
            (unsigned)out->nonzeroPerSample[0][s], (unsigned)out->ownerSetPerSample[0][s],
            (unsigned)out->changedPerSample[1][s], (unsigned long long)ring[1]->bytes,
            (unsigned)out->changedVsPrev[1][s],
            (unsigned)out->nonzeroPerSample[1][s], (unsigned)out->ownerSetPerSample[1][s]);
    }
    /* The first post-command sample is the one that carries the "handed over" claim,
     * so it is reported in words as well as in the table. */
    for (r = 0; r < 2; ++r)
        LOG("abring: ring %c at the first post-command sample: %u byte(s) differ from "
            "baseline (range 0x%x..0x%x), %u of %u slots owner=1",
            r ? 'B' : 'A', (unsigned)out->changedPerSample[r][0],
            (unsigned)out->changedOffsetLo[r], (unsigned)out->changedOffsetHi[r],
            (unsigned)out->ownerSetPerSample[r][0], (unsigned)nent);

    /* 5. Give the queue back before anything is freed. */
    {
        UInt32 hw2sw = CX3_FWSTATUS_NOT_POSTED;
        bool gave = eqHw2Sw(eqn, &hw2sw);
        out->hw2swStatus = hw2sw;
        if (gave) fwMayHold[handed] = false;
        LOG("abring: HW2SW_EQ %s (fw status 0x%x)",
            gave ? "gave the queue back" : "FAILED", (unsigned)hw2sw);
        {
            Mlx4EqContextResult q;
            UInt32 qst = CX3_FWSTATUS_NOT_POSTED;
            if (queryEq(eqn, &q, &qst)) {
                out->queryEqStatus[1] = qst;
                memcpy(out->queryEqWords[1], q.words, sizeof(out->queryEqWords[1]));
                LOG("abring: QUERY_EQ(EQN %u) after HW2SW_EQ: fw status 0x%x, words "
                    "%08x %08x %08x %08x %08x %08x %08x %08x",
                    (unsigned)eqn, (unsigned)qst,
                    (unsigned)q.words[0], (unsigned)q.words[1], (unsigned)q.words[2],
                    (unsigned)q.words[3], (unsigned)q.words[4], (unsigned)q.words[5],
                    (unsigned)q.words[6], (unsigned)q.words[7]);
            } else {
                out->queryEqStatus[1] = qst ? qst : 0xffffffffu;
                LOG("abring: QUERY_EQ(EQN %u) after HW2SW_EQ did not answer (fw status "
                    "0x%x)", (unsigned)eqn, (unsigned)qst);
            }
        }
    }

    /* 6. Final state, with the per-page account that localises the change. */
    for (r = 0; r < 2; ++r) {
        UInt32 lo = 0xffffffffu, hi = 0xffffffffu;
        UInt32 k;
        UInt32 stride = MLX4_EQ_ENTRY_SIZE << eqeFactor;
        UInt64 off    = MLX4_EQE_OWNER_BYTE(eqeFactor);
        memcpy(snap, ring[r]->virt, (size_t)ring[r]->bytes);
        out->changedFinal[r] = abDiff(snap, base[r], ring[r]->bytes, &lo, &hi,
                                      out->changedPerPage[r]);
        out->nonzeroFinal[r] = abNonzero(snap, ring[r]->bytes);
        out->ownerSetFinal[r] = abOwnerSet(snap, nent, eqeFactor);
        memcpy(out->headAfter[r], snap, CX3_AB_HEAD);
        for (k = 0; k < pages && k < CX3_AB_PAGES; ++k) {
            const UInt8 *pg = snap + (UInt64)k * MLX4_ICM_PAGE_SIZE;
            UInt64 slots = MLX4_ICM_PAGE_SIZE / stride;   /* whole slots in this page */
            UInt64 j;
            UInt32 c = 0;
            if (out->changedPerPage[r][k]) out->pagesChanged[r] |= (1u << k);
            for (j = 0; j < slots; ++j)
                if (pg[j * stride + off] & 0x80) ++c;
            out->ownerSetPerPage[r][k] = c;
        }
        LOG("abring: ring %c final: %u byte(s) differ from baseline, pages changed 0x%02x, "
            "%u of %u slots owner=1", r ? 'B' : 'A', (unsigned)out->changedFinal[r],
            (unsigned)out->pagesChanged[r], (unsigned)out->ownerSetFinal[r],
            (unsigned)nent);
        if (out->firstChangeUs[r] == 0xffffffffu)
            LOG("abring: ring %c NEVER differed from its baseline in any of the %u samples "
                "-- nothing in this whole sequence wrote to those pages",
                r ? 'B' : 'A', (unsigned)CX3_AB_SAMPLES);
        else
            LOG("abring: ring %c first differed at +%u us and last differed at +%u us "
                "(both measured from when SW2HW_EQ returned)",
                r ? 'B' : 'A', (unsigned)out->firstChangeUs[r],
                (unsigned)out->lastChangeUs[r]);
    }
    ok = true;

done:
    if (list) IOFree(list, sizeof(UInt64) * pages);
    if (snap) IOFree(snap, (size_t)(ring[0] ? ring[0]->bytes : 0));
    for (r = 0; r < 2; ++r) {
        if (base[r]) IOFree(base[r], (size_t)(ring[r] ? ring[r]->bytes : 0));
        if (prevSnap[r]) IOFree(prevSnap[r], (size_t)(ring[r] ? ring[r]->bytes : 0));
    }
    /*
     * Free in reverse order, deliberately: mttFree is a bump allocator that can only
     * hand back the most recent allocation (Mlx4Hw.cpp:1833-1839), so freeing B and
     * then A is the order that actually reclaims both.  Freeing A first would
     * silently leak B's entries, and the port's MTT accounting would then disagree
     * with what the next queue is given.
     */
    for (r = 2; r > 0; --r) {
        UInt32 k = r - 1;
        if (!ring[k]) continue;
        if (fwMayHold[k]) {
            /*
             * The MTT entries are NOT handed back either, and that matters more than the
             * ring itself.
             *
             * The page list the firmware would be translating through is the MTT entry
             * array.  Returning those entries to the bump allocator means the next queue
             * gets the same indices and writes its own page list over them -- while the
             * device may still be reading.  Keeping the ring but freeing its MTT entries
             * would protect the memory and not the translation, which is precisely
             * backwards.  Both are left alone together.
             */
            LOG("abring: ring %c is deliberately NOT freed -- HW2SW_EQ did not succeed, so "
                "there is no evidence the firmware stopped reading those pages or their "
                "MTT entry list (fw status 0x%x). The %u-page ring AND its %u MTT entry "
                "indices are both kept. A reboot reclaims them.", k ? 'B' : 'A',
                (unsigned)out->hw2swStatus, (unsigned)pages, (unsigned)pages);
            Mlx4AbRingRetention &retained = fAbRetainedRings[k];
            retained.icm = ring[k];
            retained.mttOffset = mttOff[k];
            retained.pages = pages;
            retained.eqn = eqn;
            retained.generation = ring[k]->generation.allocationGeneration;
            retained.fwMayHold = true;
            out->unknown = 1;
            continue;
        }
        if (mttAllocated[k] && !mttFree(mttOff[k], pages, ring[k])) {
            LOG("abring: refusing to free ring %c after its MTT owner check failed; retaining "
                "its exact owner metadata for retry", k ? 'B' : 'A');
            Mlx4AbRingRetention &retained = fAbRetainedRings[k];
            retained.icm = ring[k];
            retained.mttOffset = mttOff[k];
            retained.pages = pages;
            retained.eqn = eqn;
            retained.generation = ring[k]->generation.allocationGeneration;
            retained.fwMayHold = false;
            out->unknown = 1;
            continue;
        }
        icmFree(ring[k]);
    }
    if (dummyMtt && !fMttAllocator.releaseAbReservationIfUnowned()) {
        LOG("abring: retaining the single tracked dummy MTT reservation because other live "
            "MTT owners or an unreleased experiment ring remain");
    }
    return ok;
}



/* ------------------------------------------------------------------ */
/* R3: QUERY_PORT, SET_PORT general (Ethernet), INIT_PORT              */
/* ------------------------------------------------------------------ */

/*
 * QUERY_PORT -- mlx4_QUERY_PORT's new-port-commands branch (mlx4_fw.c:1202-1243).
 *
 * Read-only.  This is the capability gate: its answer decides whether the write
 * path below may run at all, which is why it is a separate selector rather than a
 * first step inside one.
 *
 * in_param 0, out_param the output mailbox, in_modifier the port, op_modifier 0.
 * The decode lives in cx3_abi.h and is the same code the control tool calls, so the
 * kernel and the tool cannot end up with different opinions about an offset.
 */
bool Mlx4Hw::queryPort(UInt32 port, Cx3QueryPortResult *out)
{
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;

    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->port     = port;
    out->fwStatus = CX3_FWSTATUS_NOT_POSTED;

    if (port < 1 || port > 2) {
        LOG("queryPort refused: port %u is outside the two ports this card has",
            (unsigned)port);
        out->status = (UInt32)kIOReturnBadArgument;
        return false;
    }
    /*
     * The inside of this function is now instrumented, because the outside was not enough.
     *
     * Measured 2026-10-13: `netup` (selector 39) died with "QUERY_PORT in" as the last line
     * anywhere on the machine, four times, while the SAME command through selector 24 succeeded
     * 176 ms earlier in the same state.  "Inside queryPort" is therefore still several steps
     * wide -- a mailbox allocation, a page clear, a posted command with a polled wait of up to
     * 60 s, a 64-byte copy and a decode -- and the next death has to name one of them rather
     * than the whole function.  Same rule as the selector markers: read them as a sequence, and
     * an `in` with no matching `out` names the step that hung.
     */
    LOG("queryPort: mailbox in (mboxAlloc, then one page cleared)");
    if (!mboxAlloc()) {
        LOG("queryPort: no command mailbox");
        out->status = (UInt32)kIOReturnNoMemory;
        return false;
    }
    memset(fMboxVirt, 0, MLX4_MAILBOX_SIZE);
    LOG("queryPort: mailbox ready (bus 0x%llx), posting MLX4_CMD_QUERY_PORT (polled, "
        "class B)", (unsigned long long)fMboxPhys);

    if (!cmdBox(0, fMboxPhys, port, 0, MLX4_CMD_QUERY_PORT, MLX4_CMD_TIME_CLASS_B,
                &status)) {
        out->fwStatus = status;
        out->status   = (UInt32)kIOReturnError;
        LOG("queryPort: port %u failed (fw status 0x%x)", (unsigned)port,
            (unsigned)status);
        return false;
    }
    LOG("queryPort: the command returned (fw status 0x%x); copying %u byte(s) out and decoding",
        (unsigned)status, (unsigned)CX3_QUERY_PORT_RAW_LEN);
    out->fwStatus = status;
    out->status   = 0;
    out->rawLen   = CX3_QUERY_PORT_RAW_LEN;
    memcpy(out->raw, fMboxVirt, CX3_QUERY_PORT_RAW_LEN);
    cx3QueryPortDecode(out->raw, out);

    LOG("queryPort: port %u: link_state %u, supported_types %u, suggested %u, "
        "eth_mtu %u, link_speed 0x%02x (%u Mb/s), max_vl %u, max_tc_eth %u, "
        "def_mac %02x:%02x:%02x:%02x:%02x:%02x, trans_type %u",
        (unsigned)port, (unsigned)out->linkState, (unsigned)out->supportedTypes,
        (unsigned)out->suggestedType, (unsigned)out->ethMtu, (unsigned)out->linkSpeed,
        (unsigned)cx3LinkSpeedMbps(out->linkSpeed), (unsigned)out->maxVl,
        (unsigned)out->maxTcEth, out->defMac[2], out->defMac[3], out->defMac[4],
        out->defMac[5], out->defMac[6], out->defMac[7], (unsigned)out->transType);
    return true;
}

/*
 * QUERY_HCA -- opcode 0x0b, outbox, in_modifier 0, op_modifier 0 (cmd.h:59).
 *
 * Read-only, and it exists because of a real omission: this port's INIT_HCA never
 * wrote INIT_HCA_UC_STEERING_OFFSET, so the HCA was told A0 whatever the port
 * intended, and R4's receive steering depends on which mode is in force.  Asking the
 * device is the only way to be sure -- the capability bits say what the card can do,
 * not what this INIT_HCA chose.
 */
bool Mlx4Hw::queryHca(Cx3QueryHcaResult *out)
{
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;

    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->fwStatus = CX3_FWSTATUS_NOT_POSTED;

    if (!mboxAlloc()) {
        LOG("queryHca: no command mailbox");
        out->status = (UInt32)kIOReturnNoMemory;
        return false;
    }
    memset(fMboxVirt, 0, MLX4_MAILBOX_SIZE);

    if (!cmdBox(0, fMboxPhys, 0, 0, MLX4_CMD_QUERY_HCA, MLX4_CMD_TIME_CLASS_A,
                &status)) {
        out->fwStatus = status;
        out->status   = (UInt32)kIOReturnError;
        LOG("queryHca: failed (fw status 0x%x)", (unsigned)status);
        return false;
    }
    out->fwStatus = status;
    out->status   = 0;
    out->rawLen   = CX3_QUERY_HCA_RAW_LEN;
    memcpy(out->raw, fMboxVirt, CX3_QUERY_HCA_RAW_LEN);

    /*
     * Decode from the FULL mailbox, not from the snapshot.
     *
     * The snapshot is a copy for the tool to inspect; the decoder reads up to 0x0db and
     * an earlier version handed it a 64-byte array, so its "byte 0x0d8 = 0x00" was an
     * out-of-bounds read that happened to be zero.  The command mailbox is 4 KiB and
     * fMboxVirt points at all of it, so there is no reason to decode through a smaller
     * window -- and a decode that can only be as wide as its buffer is a decode whose
     * bounds are visible at the call site.
     */
    cx3QueryHcaDecode((const UInt8 *)fMboxVirt, out);

    LOG("queryHca: steering mode %s (flags dword 0x%08x, bit6 %u, byte 0x0d8 = 0x%02x); "
        "mc_base 0x%llx, log_mc entry_sz %u hash_sz %u table_sz %u; log_num_qps %u, "
        "log_num_cqs %u%s",
        cx3SteerName(out->steeringMode), (unsigned)out->flagsDword,
        (unsigned)out->deviceManagedBit, (unsigned)out->ucSteeringByte,
        (unsigned long long)out->mcBase, (unsigned)out->logMcEntrySz,
        (unsigned)out->logMcHashSz, (unsigned)out->logMcTableSz,
        (unsigned)out->logNumQps, (unsigned)out->logNumCqs,
        out->steeringMode == CX3_STEER_A0
            ? " -- A0: qpn_calc IS sent and a unicast MAC's QPN is base_qpn + mac_index"
            : "");
    return true;
}


/*
 * SET_PORT, Ethernet general sub-command -- mlx4_SET_PORT_general
 * (mlx4_port.c:1544-1574).
 *
 *   in_param   = the input mailbox's DMA address
 *   in_modifier= (MLX4_SET_PORT_GENERAL << 8) | port  == port, because GENERAL is 0
 *   op_modifier= MLX4_SET_PORT_ETH_OPCODE == 1
 *
 * Both reviews made the same point about op_modifier, and it is worth stating in the
 * code rather than only in a comment: 0 is the IB opcode (cmd.h:194-195).  A wrong
 * value here is not "an invalid value the firmware will reject", it is a *different
 * operation*.  That is why none of these fields come from user space.
 *
 * The whole 4 KiB mailbox is zeroed first, as the reference's allocation does, and
 * only the named fields are filled: the structure has trailing reserved fields
 * (mlx4.h:793-795) whose contents are not ours to choose.
 */
bool Mlx4Hw::setPortGeneralEth(UInt32 port, UInt16 mtu, bool roceV1V2, UInt32 *outFwStatus)
{
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    UInt8 *ctx;

    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    if (port < 1 || port > 2) {
        LOG("setPortGeneral: refused, port %u", (unsigned)port);
        return false;
    }
    if (!mboxAlloc()) { LOG("setPortGeneral: no command mailbox"); return false; }
    ctx = (UInt8 *)fMboxVirt;
    memset(ctx, 0, MLX4_MAILBOX_SIZE);

    ctx[0x03] = 0x07;                       /* flags = SET_PORT_GEN_ALL_VALID */
    if (roceV1V2) {
        ctx[0x03] |= 0x10;                  /* SET_PORT_ROCE_2_FLAGS */
        ctx[0x04]  = (UInt8)(0x2 << 4);     /* roce_mode = ROCE_V1_V2, the union byte */
    }
    ctx[0x06] = (UInt8)(mtu >> 8);          /* __be16 mtu */
    ctx[0x07] = (UInt8)(mtu & 0xff);
    /*
     * pptx/pfctx at 0x08/0x09 and pprx/pfcrx at 0x0c/0x0d all stay zero for a first
     * bring-up: the reference computes `pptx = (pptx * !pfctx) << 7`, so zero pause
     * values produce zero bytes and no flow control is requested.  One variable at a
     * time -- flow control is a question for after the link is up.
     */
    LOG("setPortGeneral: port %u, mtu %u, flags 0x%02x, roce_mode 0x%02x, pause all 0, "
        "in_modifier 0x%x (GENERAL<<8|port), op_modifier 1 (ETH)",
        (unsigned)port, (unsigned)mtu, (unsigned)ctx[0x03], (unsigned)ctx[0x04],
        (unsigned)port);

    if (!cmdBox(fMboxPhys, 0, port, MLX4_SET_PORT_ETH_OPCODE, MLX4_CMD_SET_PORT,
                MLX4_CMD_TIME_CLASS_B, &status)) {
        if (outFwStatus) *outFwStatus = status;
        LOG("setPortGeneral: port %u failed (fw status 0x%x)", (unsigned)port,
            (unsigned)status);
        return false;
    }
    if (outFwStatus) *outFwStatus = status;
    LOG("setPortGeneral: port %u accepted (fw status 0x%x)", (unsigned)port,
        (unsigned)status);
    return true;
}

/*
 * setPortQpnCalc -- mlx4_SET_PORT_qpn_calc (mlx4_port.c:1578-1612), R4 stage 7.
 *
 * This is the command that PUSHES the A0 base QPN into the firmware.  Under A0 the MAC
 * table's index is a QPN rather than a lookup key (`*qpn = base_qpn + index`,
 * mlx4_en_netdev.c:573-576), and nothing else in the bring-up tells the firmware what that
 * base is.  Its place is between SET_PORT general and INIT_PORT, which is the reference's own
 * order (mlx4_en_netdev.c:1388, :1394, :1403): a frame can only be steered correctly after
 * INIT_PORT, and by then the base has to be known.
 *
 * Four details, and each of them is a way to send a command that succeeds and puts the MAC
 * range somewhere else:
 *
 *   in_param    = the MAILBOX's DMA address (:1591-1594).  The whole 128-byte context travels
 *                 in the mailbox; the HCR fields carry only the address.
 *   in_modifier = MLX4_SET_PORT_RQP_CALC << 8 | port -- 0x101 for port 1.  This is group 2 of
 *                 the two colliding enums (docs/DEFECTS.md E9): RQP_CALC is 0x1, the SAME
 *                 numeric value as MLX4_SET_PORT_ETH_OPCODE, and it goes in a different field.
 *                 This call site is the first in the port to use both groups at once.
 *   op_modifier = MLX4_SET_PORT_ETH_OPCODE (:1606).  0 would be the IB form of SET_PORT.
 *   timeout     = MLX4_CMD_TIME_CLASS_B (:1607).
 *
 * The context is filled the way the reference fills it: the fields it assigns are assigned,
 * the fields it leaves to its zeroed allocation stay at the clear, and the two VLAN indices are
 * its own enumerators, which are NOT both zero -- no_vlan is MLX4_NO_VLAN_IDX (0) and vlan_miss
 * is MLX4_VLAN_MISS_IDX (1, device.h:424-425).  "The clear value is already right" is true for
 * eleven of the fourteen fields here and false for vlan_miss, which is exactly the shape of
 * mistake that looks like nothing at all in a diff.
 *
 * The values themselves come from the caller: the base QPN is the number this port's RX QP has
 * (or, before it exists, the number the same rule gives it), n_mac is QUERY_PORT's log_max_macs,
 * and the multicast form is the device's VEP_MC_STEER capability bit.  None of them is decided
 * here, and none of them is a literal.
 */
bool Mlx4Hw::setPortQpnCalc(UInt32 port, UInt32 baseQpn, UInt32 promisc, UInt32 nMacLog,
                            UInt32 mPromisc, UInt32 *outFwStatus)
{
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    UInt32 inMod, promiscWord, mcastWord, k;
    UInt8 *ctx;

    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    if (port < 1 || port > 2) {
        LOG("setPortQpnCalc: refused, port %u", (unsigned)port);
        return false;
    }
    /*
     * `promisc` occupies ONE bit at 31 and is ORed onto the base QPN, so any other value would
     * spill into the number that is the whole point of the command.  The reference only ever
     * passes 0 or 1 to it (:1597-1598); the caller here passes the same.
     */
    if (promisc > 1u) {
        LOG("setPortQpnCalc: refused, promisc %u -- it is one bit (SET_PORT_PROMISC_SHIFT %u) "
            "ORed onto the base QPN, so anything above 1 would change the base too",
            (unsigned)promisc, (unsigned)SET_PORT_PROMISC_SHIFT);
        return false;
    }
    if (mPromisc != MCAST_DIRECT && mPromisc != MCAST_DEFAULT) {
        LOG("setPortQpnCalc: refused, m_promisc %u -- the device's VEP_MC_STEER bit selects "
            "MCAST_DIRECT (%u) or MCAST_DEFAULT (%u) (mlx4_port.c:1585-1586); a third value is "
            "not one the reference sends", (unsigned)mPromisc, (unsigned)MCAST_DIRECT,
            (unsigned)MCAST_DEFAULT);
        return false;
    }
    if (!mboxAlloc()) { LOG("setPortQpnCalc: no command mailbox"); return false; }

    ctx = (UInt8 *)fMboxVirt;
    memset(ctx, 0, MLX4_MAILBOX_SIZE);

    /* base_qpn, big-endian, at 0x00 (:1595). */
    ctx[RQP_CALC_BASE_QPN_OFFSET + 0] = (UInt8)((baseQpn >> 24) & 0xffu);
    ctx[RQP_CALC_BASE_QPN_OFFSET + 1] = (UInt8)((baseQpn >> 16) & 0xffu);
    ctx[RQP_CALC_BASE_QPN_OFFSET + 2] = (UInt8)((baseQpn >>  8) & 0xffu);
    ctx[RQP_CALC_BASE_QPN_OFFSET + 3] = (UInt8)(baseQpn & 0xffu);
    /* n_mac is a LOG value, not a count (:1596 takes caps.log_num_macs). */
    ctx[RQP_CALC_N_MAC_OFFSET] = (UInt8)(nMacLog & 0xffu);
    /*
     * The four VLAN-ish bytes the reference assigns, two of them explicitly to zero because the
     * clear is what it wants and it says so (:1599-1602).
     */
    ctx[RQP_CALC_INTRA_NO_VLAN_OFFSET]   = (UInt8)0;
    ctx[RQP_CALC_NO_VLAN_OFFSET]         = (UInt8)MLX4_NO_VLAN_IDX;
    ctx[RQP_CALC_INTRA_VLAN_MISS_OFFSET] = (UInt8)0;
    ctx[RQP_CALC_VLAN_MISS_OFFSET]       = (UInt8)MLX4_VLAN_MISS_IDX;
    /*
     * The two words, exactly as the reference builds them (:1597-1598, :1603-1604): the flags
     * in the top bits and the base QPN underneath.  No mask: the fields are disjoint for every
     * QPN inside a profile, and masking would be this port's invention rather than a copy.
     */
    promiscWord = (promisc << SET_PORT_PROMISC_SHIFT) | baseQpn;
    mcastWord   = (mPromisc << SET_PORT_MC_PROMISC_SHIFT) | baseQpn;
    ctx[RQP_CALC_PROMISC_OFFSET + 0] = (UInt8)((promiscWord >> 24) & 0xffu);
    ctx[RQP_CALC_PROMISC_OFFSET + 1] = (UInt8)((promiscWord >> 16) & 0xffu);
    ctx[RQP_CALC_PROMISC_OFFSET + 2] = (UInt8)((promiscWord >>  8) & 0xffu);
    ctx[RQP_CALC_PROMISC_OFFSET + 3] = (UInt8)(promiscWord & 0xffu);
    ctx[RQP_CALC_MCAST_OFFSET + 0] = (UInt8)((mcastWord >> 24) & 0xffu);
    ctx[RQP_CALC_MCAST_OFFSET + 1] = (UInt8)((mcastWord >> 16) & 0xffu);
    ctx[RQP_CALC_MCAST_OFFSET + 2] = (UInt8)((mcastWord >>  8) & 0xffu);
    ctx[RQP_CALC_MCAST_OFFSET + 3] = (UInt8)(mcastWord & 0xffu);

    inMod = (MLX4_SET_PORT_RQP_CALC << 8) | port;
    {
        /*
         * The context as it goes on the wire, all 28 bytes of it.  A command whose meaning is
         * carried in a structure rather than in the HCR fields is one where "what did we send"
         * has to be answerable from the log, and this is the cheapest way to make it so.
         */
        char line[3 * 0x1cu + 1];
        UInt32 n = 0;
        for (k = 0; k <= RQP_CALC_MCAST_OFFSET + 3u; ++k) {
            static const char hex[] = "0123456789abcdef";
            line[n++] = hex[(ctx[k] >> 4) & 0xf];
            line[n++] = hex[ctx[k] & 0xf];
            line[n++] = ' ';
        }
        line[n ? n - 1 : 0] = '\0';
        LOG("setPortQpnCalc: port %u: in_param = the mailbox 0x%llx, in_modifier 0x%x "
            "(RQP_CALC %u -- the SAME value as ETH_OPCODE, different field -- << 8 | port %u), "
            "op_modifier %u (ETH), timeout class B. Context byte 0x00..0x%02x: %s",
            (unsigned)port, (unsigned long long)fMboxPhys, (unsigned)inMod,
            (unsigned)MLX4_SET_PORT_RQP_CALC, (unsigned)port,
            (unsigned)MLX4_SET_PORT_ETH_OPCODE, (unsigned)(RQP_CALC_MCAST_OFFSET + 3u), line);
    }
    LOG("setPortQpnCalc: base_qpn %u, n_mac %u (a log value: 1 << %u = %u MAC-derived QPNs), "
        "promisc %u (bit 31: %u = not promiscuous, which is the reference's normal activation "
        "path, mlx4_en_netdev.c:1394), mcast m_promisc %u (%s) in bits 31:30 with the same base "
        "underneath, intra_no_vlan 0, no_vlan %u, intra_vlan_miss 0, vlan_miss %u -- the other "
        "fields stay at the clear, as the reference leaves them",
        (unsigned)baseQpn, (unsigned)nMacLog, (unsigned)nMacLog,
        (unsigned)((nMacLog < 32u) ? (1u << nMacLog) : 0xffffffffu), (unsigned)promisc,
        (unsigned)promisc, (unsigned)mPromisc,
        (mPromisc == MCAST_DIRECT) ? "MCAST_DIRECT: this card's VEP_MC_STEER bit is set"
                                   : "MCAST_DEFAULT: VEP_MC_STEER is not set on this card",
        (unsigned)MLX4_NO_VLAN_IDX, (unsigned)MLX4_VLAN_MISS_IDX);
    LOG("setPortQpnCalc: after this command the firmware's A0 mapping is index i -> %u + i, so "
        "MAC table slot 0 is QPN %u. The cost of putting the base HERE (the plan's decision 1) "
        "is recorded in docs/R4-STAGE7-RESULT.md: slot 1 and above reach into the QPNs the "
        "driver's own queues occupy, so this configuration supports ONE MAC address",
        (unsigned)baseQpn, (unsigned)baseQpn);

    if (!cmdBox(fMboxPhys, 0, inMod, MLX4_SET_PORT_ETH_OPCODE, MLX4_CMD_SET_PORT,
                MLX4_CMD_TIME_CLASS_B, &status)) {
        if (outFwStatus) *outFwStatus = status;
        LOG("setPortQpnCalc: port %u failed (fw status 0x%x). A BAD_PARAM (0x3) or BAD_RESOURCE "
            "(0x5) here would mean the firmware DOES check the base against a region of its own, "
            "and the base would have to move to num_qps - (1 << log_max_macs) with the RX QP "
            "alongside it (docs/R4-STAGE7-PLAN.md section 6)", (unsigned)port,
            (unsigned)status);
        return false;
    }
    if (outFwStatus) *outFwStatus = status;
    LOG("setPortQpnCalc: port %u accepted (fw status 0x%x); INIT_PORT may now follow, and this "
        "is the run in which frames can be steered to QPN %u at all", (unsigned)port,
        (unsigned)status, (unsigned)baseQpn);
    return true;
}

/*
 * INIT_PORT -- mlx4_INIT_PORT's new-interface branch (mlx4_fw.c:2299-2308):
 * in_param 0, no mailbox, in_modifier the port, op_modifier 0.
 *
 * The old interface (cmd_if_rev < 3) sends a 256-byte mailbox instead.  This card
 * reports cmd_if_rev 3 (measured, M1b VERDICT line), so the mailbox form would be a
 * guess about a different driver generation -- it is refused rather than attempted.
 */
/*
 * One SET_MCAST_FLTR command.  See the declaration for why this is the whole of multicast on
 * this card, and what the parameter's two halves are.
 *
 * No mailbox: the address travels in the HCR's in_param (the reference's mlx4_cmd call passes
 * it there, mlx4_port.c:1719), which is why cmdBox() is given the value where its other callers
 * pass a mailbox address.
 */
/*
 * CLOSE_PORT.  See the declaration: this is the "stop" half of a port reconfiguration, and the
 * chain's port-up step is the other half.
 *
 * It says nothing about the queues on purpose -- in this driver they are the bring-up's objects
 * and closing a port does not remove a QP -- and the caller is the one that has to know whether
 * the data path is running, because closing a port under a live interface would stop traffic
 * silently.
 */
bool Mlx4Hw::closePort(UInt32 port, UInt32 *outFwStatus)
{
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    bool ok;

    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    if (port < 1u || port > 2u) {
        LOG("closePort: refused, port %u", (unsigned)port);
        return false;
    }
    ok = cmdBox(0, 0, port, /*opMod=*/0u, MLX4_CMD_CLOSE_PORT, MLX4_CMD_TIME_CLASS_A, &status);
    if (outFwStatus) *outFwStatus = status;
    LOG("closePort: port %u -> command %s (fw status 0x%x). The port is stopped; the queues "
        "this driver built are untouched, and re-opening is the port-up sequence the chain runs "
        "(SET_PORT general with the MTU, qpn_calc, INIT_PORT)",
        (unsigned)port, ok ? "accepted" : "FAILED", (unsigned)status);
    return ok;
}

bool Mlx4Hw::setMcastFltr(UInt32 port, UInt64 mac, bool clear, UInt32 mode,
                          UInt32 *outFwStatus)
{
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    UInt64 param;
    bool ok;

    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    if (port < 1u || port > 2u) {
        LOG("setMcastFltr: refused, port %u", (unsigned)port);
        return false;
    }
    if (mode > MLX4_MCAST_ENABLE) {
        LOG("setMcastFltr: refused, mode %u -- the three the reference uses are CONFIG (%u), "
            "DISABLE (%u) and ENABLE (%u) (en_port.h:51-53)", (unsigned)mode,
            (unsigned)MLX4_MCAST_CONFIG, (unsigned)MLX4_MCAST_DISABLE,
            (unsigned)MLX4_MCAST_ENABLE);
        return false;
    }

    param = (mac & 0x0000ffffffffffffull) |
            (clear ? (1ull << MLX4_MCAST_CLEAR_BIT) : 0ull);
    ok = cmdBox(param, 0, port, (UInt8)mode, MLX4_CMD_SET_MCAST_FLTR, MLX4_CMD_TIME_CLASS_B,
                &status);
    if (outFwStatus) *outFwStatus = status;
    LOG("setMcastFltr: port %u, %s%012llx, mode %u (%s) -> command %s, fw status 0x%x",
        (unsigned)port, clear ? "CLEAR then add " : "add ",
        (unsigned long long)(mac & 0x0000ffffffffffffull), (unsigned)mode,
        mode == MLX4_MCAST_CONFIG ? "CONFIG" :
        (mode == MLX4_MCAST_DISABLE ? "DISABLE" : "ENABLE"),
        ok ? "accepted" : "FAILED", (unsigned)status);
    return ok;
}

bool Mlx4Hw::setVlanFilter(UInt32 port,const UInt8 *vidBits,UInt32 *outFwStatus)
{
    // mlx4_en_port.c:44-72: 128 BE32 words in REVERSE word order.
    // Null bitmap explicitly allows all VIDs; it is not a zero-length command.
    if(outFwStatus)*outFwStatus=CX3_FWSTATUS_NOT_POSTED;
    if(port<1||port>2||!mboxAlloc())return false;
    memset(fMboxVirt,0,MLX4_MAILBOX_SIZE);
    for(UInt32 word=0;word<128;++word){
        UInt32 entry=0;
        for(UInt32 bit=0;bit<32;++bit){UInt32 vid=word*32+bit;
            if(!vidBits||(vidBits[vid>>3]&(1u<<(vid&7u))))entry|=1u<<bit;
        }
        mlx4PutBe32((UInt8*)fMboxVirt+(127-word)*4,entry);
    }
    UInt32 status=CX3_FWSTATUS_NOT_POSTED;
    const bool ok=cmdBox(fMboxPhys,0,port,0,MLX4_CMD_SET_VLAN_FLTR,MLX4_CMD_TIME_CLASS_B,&status);
    if(outFwStatus)*outFwStatus=status;
    return ok;
}

bool Mlx4Hw::initPort(UInt32 port, UInt32 *outFwStatus)
{
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;

    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    if (port < 1 || port > 2) {
        LOG("initPort: refused, port %u", (unsigned)port);
        return false;
    }
    if (!fHaveLastFw) {
        LOG("initPort refused: no QUERY_FW answer is held, so the command-interface "
            "revision is unknown and the new-style call cannot be justified");
        return false;
    }
    if (fLastFw.cmdIfRev < 3) {
        LOG("initPort refused: this card reports command-interface revision %u, and the "
            "old interface expects a 256-byte input mailbox; this port implements only "
            "the new one", (unsigned)fLastFw.cmdIfRev);
        return false;
    }
    if (!cmdBox(0, 0, port, 0, MLX4_CMD_INIT_PORT, MLX4_CMD_TIME_CLASS_A, &status)) {
        if (outFwStatus) *outFwStatus = status;
        LOG("initPort: port %u failed (fw status 0x%x)", (unsigned)port, (unsigned)status);
        return false;
    }
    if (outFwStatus) *outFwStatus = status;
    LOG("initPort: port %u initialised (fw status 0x%x) -- note this only says the HCA "
        "accepted the command, not that the link is up", (unsigned)port, (unsigned)status);
    return true;
}

/* mlx4_eq.c:887: in_param is the event bitmask, in_modifier the EQN, and bit 31
 * of the modifier means "unmap" (mlx4_MAP_EQ_wrapper at :858-880). */
bool Mlx4Hw::eqMap(UInt64 eventMask, UInt32 eqn, bool unmap, UInt32 *outFwStatus)
{
    UInt32 modifier = (eqn & 0x3ffu) | (unmap ? 0x80000000u : 0u);
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    bool ok = cmdBox(eventMask, 0, modifier, 0, MLX4_CMD_MAP_EQ,
                     MLX4_CMD_TIME_CLASS_B, &status);
    if (outFwStatus) *outFwStatus = status;
    LOG("MAP_EQ%s: mask 0x%llx -> EQN %u: %s (fw status 0x%x)",
        unmap ? " (unmap)" : "", (unsigned long long)eventMask, (unsigned)eqn,
        ok ? "ok" : "failed", (unsigned)status);
    return ok;
}

/*
 * eq_set_ci() -- mlx4_eq.c:97-104.  The value is the consumer index in the low
 * 24 bits; bit 31 asks the HCA to notify us again (the "request notification"),
 * which is what re-arms the EQ.  Big-endian on the wire, and BAR2 is a flat
 * mapping so the doorbell is a store at a computed offset.
 */
void Mlx4Hw::eqSetConsumerIndex(UInt64 doorbellAddr, UInt32 consIndex, bool reqNot)
{
    UInt32 value = (consIndex & 0xffffffu) | (reqNot ? 0x80000000u : 0u);

    if (!fReg2) {
        LOG("eqSetConsumerIndex: BAR2 is not mapped, doorbell not written");
        return;
    }
    if (doorbellAddr + 4 > 0x800000ull) {
        LOG("eqSetConsumerIndex: 0x%llx is outside the mapped BAR2 window",
            (unsigned long long)doorbellAddr);
        return;
    }
    fReg2[doorbellAddr / 4] = OSSwapHostToBigInt32(value);
    mlx4Wmb();
    LOG("EQ doorbell: wrote 0x%08x to BAR2+0x%llx (cons_index %u, req_not %u)",
        (unsigned)value, (unsigned long long)doorbellAddr,
        (unsigned)(consIndex & 0xffffffu), (unsigned)(reqNot ? 1 : 0));
}

/*
 * next_eqe_sw() -- mlx4_eq.c:121-125.
 *
 * The owner bit alternates on every pass over the ring, so readiness is the
 * owner bit compared against the consumer index's own phase bit:
 *
 *     !!(eqe->owner & 0x80) ^ !!(eq->cons_index & eq->nent) ? NULL : eqe
 *
 * i.e. ready when the two are equal.  Using "owner says hardware" naively would
 * report a freshly armed ring as full, because the HCA owns every slot it has not
 * written yet.
 *
 * With 64-byte EQEs the legacy 32-byte EQE is the second half of the slot
 * (mlx4_eq.c:110-116), which is why the factor shifts the offset by 32 and not
 * the stride.
 */
/*
 * The legacy 32-byte EQE's position inside a slot -- and therefore its owner
 * byte -- comes from MLX4_EQE_LEGACY_OFFSET / MLX4_EQE_OWNER_BYTE in mlx4_regs.h,
 * because the armed-wait in the provider needs the same arithmetic.  Those two
 * used to be separate, and the provider's copy ("half the stride") is wrong for
 * the 32-byte format.
 */
bool Mlx4Hw::eqNextEqe(const Mlx4Icm *ring, UInt32 nent, UInt8 eqeFactor,
                       UInt32 consIndex, UInt8 *outEqe32)
{
    UInt32 stride = MLX4_EQ_ENTRY_SIZE << eqeFactor;
    UInt32 slot = consIndex & (nent - 1);
    UInt64 offset = (UInt64)slot * stride + MLX4_EQE_LEGACY_OFFSET(eqeFactor);
    const UInt8 *eqe;
    UInt32 owner, phase;

    if (!ring || !ring->virt || !outEqe32) return false;
    if (offset + MLX4_EQ_ENTRY_SIZE > ring->bytes) return false;

    eqe = (const UInt8 *)ring->virt + offset;
    owner = (eqe[MLX4_EQE_OWNER_OFFSET] & 0x80) ? 1u : 0u;
    phase = (consIndex & nent) ? 1u : 0u;

    if (owner != phase) return false;          /* the HCA still owns this slot */
    /*
     * The reference reads the owner bit and then a barrier before touching the
     * rest of the EQE (mlx4_eq.c:506-511), because the HCA writes the payload and
     * the owner bit as separate stores and only the owner bit is the handshake.
     * Without the barrier this CPU can read the payload from before the device
     * wrote it and see a complete-looking EQE with stale contents.  On x86_64 the
     * loads are already ordered against each other, so this costs nothing here --
     * it is here because the code must not depend on that.
     */
    /*
     * A compiler barrier, not OSMemoryBarrier(): the latter is not in the kext KPI
     * and the build's symbol accounting refuses it.  What is needed here is that the
     * payload reads below are not hoisted above the owner-bit load -- on x86_64 the
     * hardware already orders loads against each other, so the compiler is the only
     * thing that could reorder them, and this stops it.  On a weakly-ordered CPU
     * this would need a real barrier; the port targets x86_64 only.
     */
    __asm__ __volatile__("" ::: "memory");
    memcpy(outEqe32, eqe, MLX4_EQ_ENTRY_SIZE);
    return true;
}

bool Mlx4Hw::eqHw2Sw(UInt32 eqn, UInt32 *outFwStatus)
{
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    bool ok = cmdBox(0, 0, eqn, 1, MLX4_CMD_HW2SW_EQ, MLX4_CMD_TIME_CLASS_A, &status);
    if (outFwStatus) *outFwStatus = status;
    LOG("HW2SW_EQ(eqn %u): %s (fw status 0x%x)", (unsigned)eqn, ok ? "ok" : "failed",
        (unsigned)status);
    return ok;
}

/*
 * mlx4_QUERY_ADAPTER() -- FreeBSD mlx4_fw.c:1789.  Outbox command, no inbox.
 *
 * One field matters here: inta_pin, at outbox offset 0x10, a u8.  Everything about
 * acknowledging a legacy interrupt is derived from it, which is why it is a
 * command and not a constant.
 */
/* Fixed, read-only module GET. Separate inbox/outbox pages follow the
 * reference rather than assuming MAD_IFC supports overlapping DMA buffers.
 * The extra inbox is cached (one page max) and retained with a poisoned command
 * mailbox. It is never freed/reused on an unknown command completion. */
bool Mlx4Hw::readModule(UInt32 port, UInt32 offset, UInt32 length, Cx3ModuleResult *out)
{
    UInt32 fwStatus = CX3_FWSTATUS_NOT_POSTED, badPage = 0;
    UInt8 *in;
    const UInt8 *reply;
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->version = CX3_MODULE_RESULT_VERSION;
    out->structBytes = (UInt32)sizeof(*out);
    out->deviceGeneration = fDeviceGeneration;
    out->port = port; out->offset = offset; out->requestedBytes = length;
    out->fwStatus = CX3_FWSTATUS_NOT_POSTED;
    out->madStatus = CX3_MODULE_MAD_NOT_RETURNED;
    if (!cx3ModuleRangeValid(port, offset, length)) {
        out->status = (UInt64)kIOReturnBadArgument;
        return false;
    }
    out->i2cAddress = offset < 256 ? 0x50 : 0x51;
    out->wireOffset = offset & 255;
    if (fCmdWedged || fMboxRetained) {
        out->status = (UInt64)kIOReturnNotPermitted;
        out->fwStatus = CX3_FWSTATUS_WEDGED;
        return false;
    }
    if (!mboxAlloc()) { out->status = (UInt64)kIOReturnNoMemory; return false; }
    if (!fModuleInbox) fModuleInbox = icmAlloc(1, true);
    if (!fModuleInbox) { out->status = (UInt64)kIOReturnNoMemory; return false; }
    if (!fModuleInbox->virt || !fModuleInbox->pages || fModuleInbox->npages != 1 ||
        fModuleInbox->bytes != MLX4_MAILBOX_SIZE || !icmCheck(fModuleInbox, &badPage) ||
        !fModuleInbox->pages[0].busAddr || fModuleInbox->pages[0].busAddr > 0xfffff000ull ||
        (fModuleInbox->pages[0].busAddr & 4095) ||
        fModuleInbox->pages[0].busAddr == fMboxPhys) {
        /* No handoff occurred for this inbox; reject before HCR writes. */
        icmFree(fModuleInbox); fModuleInbox = 0;
        out->status = (UInt64)kIOReturnNoMemory;
        return false;
    }
    in = (UInt8 *)fModuleInbox->virt;
    memset(in, 0, MLX4_MAILBOX_SIZE);
    memset(fMboxVirt, 0, MLX4_MAILBOX_SIZE);
    /* device.h:mlx4_mad_ifc (64-byte header); mlx4_port.c:cable_info
     * has a 16-byte header followed by the 48-byte payload. */
    in[0] = 1; in[1] = 1; in[2] = 1; in[3] = 1;
    in[16] = 0xff; in[17] = 0x60;
    in[64] = (UInt8)out->i2cAddress;
    in[66] = (UInt8)(out->wireOffset >> 8); in[67] = (UInt8)out->wireOffset;
    in[70] = (UInt8)(length >> 8); in[71] = (UInt8)length;
    if (!cmdBox(fModuleInbox->pages[0].busAddr, fMboxPhys, port, 3,
                0x24 /* MLX4_CMD_MAD_IFC */, MLX4_CMD_TIME_CLASS_C, &fwStatus)) {
        out->status = (UInt64)kIOReturnError;
        out->fwStatus = fwStatus;
        return false;
    }
    out->fwStatus = fwStatus;
    reply = (const UInt8 *)fMboxVirt;
    memcpy(out->raw, reply, CX3_MODULE_RAW_BYTES);
    out->rawBytes = CX3_MODULE_RAW_BYTES;
    out->madStatus = ((UInt32)reply[4] << 8) | reply[5];
    if (out->madStatus) {
        /* Cable error 7 on high address means that page is unavailable. */
        out->status = (UInt64)((out->i2cAddress == 0x51 &&
            ((out->madStatus >> 8) & 255) == 7) ? kIOReturnUnsupported : kIOReturnError);
        return false;
    }
    memcpy(out->data, reply + 80, length);
    out->returnedBytes = length;
    out->status = 0;
    return true;
}

bool Mlx4Hw::queryAdapter(UInt8 *outIntaPin, UInt8 *outBoardId, UInt32 *outFwStatus,
                          UInt8 *outRaw)
{
    UInt8 *b;
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    UInt32 i;
    bool ok;

    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    if (outIntaPin) *outIntaPin = 0xff;
    if (outRaw) memset(outRaw, 0, CX3_ADAPTER_RAW_BYTES);
    if (!mboxAlloc()) return false;
    b = (UInt8 *)fMboxVirt;
    memset(b, 0, MLX4_MAILBOX_SIZE);

    ok = cmdBox(0, fMboxPhys, 0, 0, MLX4_CMD_QUERY_ADAPTER, MLX4_CMD_TIME_CLASS_A,
                &status);
    if (outFwStatus) *outFwStatus = status;
    if (!ok) {
        LOG("QUERY_ADAPTER: failed (fw status 0x%x)", (unsigned)status);
        return false;
    }
    if (outRaw) memcpy(outRaw, b, CX3_ADAPTER_RAW_BYTES);
    if (outIntaPin) *outIntaPin = b[QUERY_ADAPTER_INTA_PIN_OFFSET];
    if (outBoardId) {
        /* mlx4_fw.c:get_board_id: VSD is a container, not the ID string.
         * Both Topspin signatures are required; all other replies use the
         * Mellanox 16-byte field, whose bytes are reversed within each dword. */
        const UInt8 *vsd = b + QUERY_ADAPTER_VSD_OFFSET;
        UInt32 sig1 = ((UInt32)vsd[MLX4_VSD_SIG1_OFFSET] << 8) |
                       vsd[MLX4_VSD_SIG1_OFFSET + 1];
        UInt32 sig2 = ((UInt32)vsd[MLX4_VSD_SIG2_OFFSET] << 8) |
                       vsd[MLX4_VSD_SIG2_OFFSET + 1];
        memset(outBoardId, 0, MLX4_BOARD_ID_LEN);
        if (sig1 == MLX4_VSD_SIGNATURE_TOPSPIN && sig2 == MLX4_VSD_SIGNATURE_TOPSPIN) {
            const UInt8 *id = vsd + MLX4_VSD_TS_BOARD_ID_OFFSET;
            for (i = 0; i < MLX4_BOARD_ID_LEN - 1 && id[i]; ++i)
                outBoardId[i] = id[i];
        } else {
            for (i = 0; i < 16; ++i)
                outBoardId[i] = vsd[MLX4_VSD_MLX_BOARD_ID_OFFSET + (i ^ 3u)];
        }
    }

    LOG("QUERY_ADAPTER: inta_pin = %u (0x%02x); board id %02x%02x%02x%02x...",
        (unsigned)(outIntaPin ? *outIntaPin : 0),
        (unsigned)(outIntaPin ? *outIntaPin : 0),
        outBoardId ? (unsigned)outBoardId[0] : 0,
        outBoardId ? (unsigned)outBoardId[1] : 0,
        outBoardId ? (unsigned)outBoardId[2] : 0,
        outBoardId ? (unsigned)outBoardId[3] : 0);
    return true;
}

/*
 * Set up the interrupt-clear register.
 *
 * mlx4_eq.c:1179-1187, transcribed:
 *     clr_mask = swab32(1 << (inta_pin & 31));
 *     clr_int  = clr_base + (inta_pin < 32 ? 4 : 0);
 *
 * The register is 8 bytes wide (MLX4_CLR_INT_SIZE) with one 32-bit half per pin
 * range, and the byte order is swapped because the 1<<n is a host-side bit going to
 * a big-endian register.  The reference uses swab32 (a plain byte swap, no
 * reordering), which is what this writes -- not the EQ doorbell's conversion, which
 * is a different thing and the reviews warned against conflating them.
 */
bool Mlx4Hw::clrIntSetup(UInt8 clrBar, UInt64 clrBase, UInt8 intaPin,
                         UInt32 *outMask, UInt32 *outOffset, UInt32 *outVia)
{
    UInt64 byteOff;
    volatile UInt32 *base;

    if (outMask) *outMask = 0;
    if (outOffset) *outOffset = 0;
    if (outVia) *outVia = 0xffffffffu;
    clrIntTearDown();

    /*
     * Reuse the mapping this port already holds whenever the clear register lives in
     * BAR0 or BAR2 -- the device-wide case -- and only map something new when it does
     * not.  Fewer new mappings is fewer new ways to fail.
     */
    if (clrBar == 0 && fReg0) {
        base = fReg0;
    } else if (clrBar == 2 && fReg2) {
        base = fReg2;
    } else {
        UInt8 reg = (UInt8)(kIOPCIConfigBaseAddress0 + 4 * clrBar);
        fClrMap = fPci->mapDeviceMemoryWithRegister(reg, kIOMapAnywhere);
        if (!fClrMap) {
            LOG("clrIntSetup: could not map BAR %u for the interrupt-clear register",
                (unsigned)clrBar);
            return false;
        }
        base = (volatile UInt32 *)fClrMap->getVirtualAddress();
        clrBase = 0;    /* the new mapping starts at the BAR */
        if (outVia) *outVia = (UInt32)kIOPCIConfigBaseAddress0 + 4 * (UInt32)clrBar;
    }

    byteOff = clrBase + ((intaPin < 32) ? 4 : 0);
    if (byteOff + 4 > 0x800000ull) {
        LOG("clrIntSetup: clear register at 0x%llx is outside the mapped BAR",
            (unsigned long long)byteOff);
        clrIntTearDown();
        return false;
    }

    fClrIntPtr = (volatile UInt32 *)((UInt8 *)base + byteOff);
    fClrMask   = OSSwapInt32(1u << (intaPin & 31));
    fClrOffset = (UInt32)byteOff;

    if (outMask) *outMask = fClrMask;
    if (outOffset) *outOffset = fClrOffset;
    LOG("clrIntSetup: BAR %u + 0x%llx (pin %u %s 32) -> mask 0x%08x, offset 0x%x; "
        "region is %u bytes", (unsigned)clrBar, (unsigned long long)clrBase,
        (unsigned)intaPin, (intaPin < 32) ? "<" : ">=", (unsigned)fClrMask,
        (unsigned)fClrOffset, (unsigned)MLX4_CLR_INT_SIZE);
    return true;
}

/*
 * Acknowledge the interrupt at the device.
 *
 * This is the step whose absence wedged the machine: a level-triggered line that is
 * never cleared re-asserts immediately, and the handler ran hundreds of times
 * consuming nothing until the system stopped.  It goes at the START of the handler,
 * before the queue is consumed (mlx4_eq.c:841 is the first statement of
 * mlx4_interrupt, ahead of the EQ scan at :843-844).
 */
bool Mlx4Hw::clrIntWrite()
{
    if (!fClrIntPtr) return false;
    *fClrIntPtr = fClrMask;
    mlx4Wmb();
    return true;
}

void Mlx4Hw::clrIntTearDown()
{
    if (fClrMap) {
        fClrMap->release();
        fClrMap = 0;
    }
    fClrIntPtr = 0;
    fClrMask = 0;
    fClrOffset = 0;
}

UInt32 Mlx4Hw::hcrTokenField() const
{
    if (!fHcr) return 0;
    /* +0x14 holds the token in its high 16 bits.  Read through the warm accessor to
     * stay consistent with every other HCR read in this file. */
    return ((Mlx4Hw *)this)->readRegWarm(MLX4_HCR_BASE + 0x14) >> 16;
}

/*
 * ------------------------------------------------------------------ */
/* M3 restart-1 diagnostics: MTT readback and QUERY_EQ                  */
/* ------------------------------------------------------------------ */

/*
 * Read the MTT entries the HCA would use for a queue.
 *
 * These are host memory (the table is mapped in full by the M2d bring-up), so
 * reading them is a plain load.  The comparison this exists for is against the
 * queue's own page bus addresses: if they match, the HCA will find the right
 * pages, and a missing EQE has to be about the EQ context rather than about the
 * page list.  The ring probe could not answer that, because it handed a bus
 * address over directly instead of going through the MTT.
 */
bool Mlx4Hw::mttReadback(UInt32 offsetEntries, UInt32 n, const Mlx4Icm *icm,
                         UInt64 *out, void *res)
{
    (void)icm; (void)out; (void)res;
    if (fMttBase == 0) {
        LOG("mttReadback: the MTT table is not mapped");
        return false;
    }
    if ((UInt64)(offsetEntries + n) * MLX4_MTT_ENTRY_SIZE > fMttBytes) {
        LOG("mttReadback: entries %u..%u are past the mapped MTT table",
            (unsigned)offsetEntries, (unsigned)(offsetEntries + n - 1));
        return false;
    }
    return true;
}

UInt64 Mlx4Hw::mttEntryValue(UInt32 entryIndex)
{
    const UInt8 *base = (const UInt8 *)fMttBase;
    if (!base) return 0;
    if ((UInt64)(entryIndex + 1) * MLX4_MTT_ENTRY_SIZE > fMttBytes) return 0;
    return mlx4GetBe64(base + (UInt64)entryIndex * MLX4_MTT_ENTRY_SIZE);
}

/*
 * mlx4_QUERY_EQ -- opcode 0x15, an outbox command whose in_modifier is the EQN
 * (mlx4_cmd.c:1284-1291: has_inbox false, has_outbox true, encode_slave_id).
 *
 * The reference never sends it, so its answer is not something this port depends
 * on -- but if the firmware implements it, it is the most direct possible answer
 * to "what does the HCA think this EQ is", which is exactly the question left
 * open when the memory is reachable and no event arrives.  A failure is recorded
 * as "unsupported", not treated as evidence either way.
 */
bool Mlx4Hw::queryEq(UInt32 eqn, Mlx4EqContextResult *out, UInt32 *outFwStatus)
{
    UInt8 *b;
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    int i;
    bool ok;

    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!mboxAlloc()) return false;

    b = (UInt8 *)fMboxVirt;
    memset(b, 0, MLX4_MAILBOX_SIZE);
    ok = cmdBox(0, fMboxPhys, eqn, 0, MLX4_CMD_QUERY_EQ, MLX4_CMD_TIME_CLASS_A, &status);
    if (outFwStatus) *outFwStatus = status;
    if (!ok) {
        LOG("QUERY_EQ(eqn %u): no answer (fw status 0x%x)", (unsigned)eqn, (unsigned)status);
        return false;
    }

    /* The whole first 32 bytes, so the caller can compare it against the context
     * this port wrote without the layout being re-derived here. */
    for (i = 0; i < 8; ++i) out->words[i] = mlx4GetBe32(b + i * 4);
    out->ok = true;
    return true;
}
