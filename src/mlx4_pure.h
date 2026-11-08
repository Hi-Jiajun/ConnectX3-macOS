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
 * mlx4_pure.h -- the parts of M2d that are pure arithmetic on byte streams.
 *
 * Why this is a separate header rather than inline in Mlx4Hw.cpp: both bugs found
 * in this code so far were arithmetic-or-shape mistakes that the hardware was the
 * first thing to notice, and each one cost a reboot (a 160-byte stack overflow
 * that panicked the machine, and an in/out parameter that made the table list
 * report zero slots).  Code that only runs on hardware gets tested once per
 * reboot; code in a header that a host program can include gets tested every
 * build.
 *
 * So the dev-cap decode and the profile packing live here, depend on nothing but
 * mlx4_regs.h and these structs, and are exercised by tools/test-m2d-profile.c
 * against the real outbox bytes this card returned.  Nothing here touches IOKit.
 */
#ifndef CX3_MLX4_PURE_H
#define CX3_MLX4_PURE_H

#ifdef CX3_HOST_TEST
/* Host build: same widths, no kernel headers. */
#include <stdint.h>
#include <stdbool.h>
typedef uint8_t  UInt8;
typedef uint16_t UInt16;
typedef uint32_t UInt32;
typedef uint64_t UInt64;
#else
#include <IOKit/IOTypes.h>
#endif

#include "mlx4_regs.h"

/*
 * The queue MTT allocator is a bump allocator because each returned range is
 * contiguous. Teardown is not necessarily LIFO (the EQ is handed back before
 * its dependent queues), so remember exact allocation records and collapse
 * out-of-order frees when the live tail is eventually released. Exact records
 * reject partial frees that could rewind into a still-live allocation. Interior
 * holes are not reused; a full ledger therefore refuses more allocations.
 */
struct Mlx4MttAllocator {
    enum { kAllocationMax = 256 };
    struct Range {
        UInt32 offset;
        UInt32 pages;
        const void *ownerIdentity;
        UInt64 ownerAllocationId;
        UInt64 allocationGeneration;
        UInt64 ownerGeneration;
        bool live;
    };
    UInt32 high;
    UInt32 reserved;
    UInt32 allocationCount;
    Range allocations[kAllocationMax];
    UInt32 abReservationOffset;
    UInt32 abReservationPages;
    UInt64 abReservationGeneration;
    bool abReservationTracked;

    Mlx4MttAllocator() : high(0), reserved(0), allocationCount(0),
        abReservationOffset(0), abReservationPages(0), abReservationGeneration(0),
        abReservationTracked(false) {}
    void init(UInt32 firstUsable)
    {
        high = firstUsable;
        reserved = firstUsable;
        allocationCount = 0;
        abReservationOffset = abReservationPages = 0;
        abReservationGeneration = 0;
        abReservationTracked = false;
    }

    bool allocate(UInt32 pages, UInt64 totalEntries, UInt32 *outOffset)
    {
        if (outOffset) *outOffset = 0;
        if (!pages || allocationCount == kAllocationMax ||
            (UInt64)high + pages > totalEntries ||
            (UInt64)high + pages > 0xffffffffull) return false;
        if (outOffset) *outOffset = high;
        allocations[allocationCount].offset = high;
        allocations[allocationCount].pages = pages;
        allocations[allocationCount].ownerIdentity = 0;
        allocations[allocationCount].ownerAllocationId = 0;
        allocations[allocationCount].allocationGeneration = 0;
        allocations[allocationCount].ownerGeneration = 0;
        allocations[allocationCount].live = true;
        ++allocationCount;
        high += pages;
        return true;
    }

    bool allocateOwned(UInt32 pages, UInt64 totalEntries, const void *ownerIdentity,
                       UInt64 ownerAllocationId, UInt64 generation, UInt32 *outOffset)
    {
        if (!ownerIdentity || !ownerAllocationId || !generation || !pages ||
            allocationCount == kAllocationMax ||
            (UInt64)high + pages > totalEntries ||
            (UInt64)high + pages > 0xffffffffull) return false;
        if (outOffset) *outOffset = 0;
        allocations[allocationCount].offset = high;
        allocations[allocationCount].pages = pages;
        allocations[allocationCount].ownerIdentity = ownerIdentity;
        allocations[allocationCount].ownerAllocationId = ownerAllocationId;
        allocations[allocationCount].allocationGeneration = generation;
        allocations[allocationCount].ownerGeneration = 0;
        allocations[allocationCount].live = true;
        if (outOffset) *outOffset = high;
        ++allocationCount;
        high += pages;
        return true;
    }

    bool allocateReservation(UInt32 pages, UInt64 totalEntries, UInt64 generation,
                             UInt32 *outOffset)
    {
        if (!generation || !pages || allocationCount == kAllocationMax ||
            (UInt64)high + pages > totalEntries ||
            (UInt64)high + pages > 0xffffffffull) return false;
        if (outOffset) *outOffset = 0;
        allocations[allocationCount].offset = high;
        allocations[allocationCount].pages = pages;
        allocations[allocationCount].ownerIdentity = 0;
        allocations[allocationCount].ownerAllocationId = 0;
        allocations[allocationCount].allocationGeneration = generation;
        allocations[allocationCount].ownerGeneration = 0;
        allocations[allocationCount].live = true;
        if (outOffset) *outOffset = high;
        ++allocationCount;
        high += pages;
        return true;
    }

    /*
     * The diagnostic A/B experiment may shift its rings with one ownerless MTT
     * reservation. Repeated experiments reuse that exact range instead of burning
     * one of the finite owner-ledger records each time. A different size or device
     * generation is refused while the old reservation remains live.
     */
    bool canRunAbExperiment(UInt32 pages, UInt64 generation) const
    {
        UInt32 reservationIndex = kAllocationMax;
        if (abReservationTracked) {
            for (UInt32 i = 0; i < allocationCount; ++i) {
                const Range &r = allocations[i];
                if (r.offset == abReservationOffset && r.pages == abReservationPages &&
                    r.ownerIdentity == 0 && r.ownerAllocationId == 0 &&
                    r.allocationGeneration == abReservationGeneration) {
                    reservationIndex = i;
                    break;
                }
            }
            if (reservationIndex == kAllocationMax || !allocations[reservationIndex].live ||
                abReservationOffset != reserved || reservationIndex + 1 != allocationCount ||
                (pages && (pages != abReservationPages ||
                           generation != abReservationGeneration))) return false;
        } else if (!generation) {
            return false;
        } else if (allocationCount != 0 || high != reserved) {
            return false;
        }
        for (UInt32 i = 0; i < allocationCount; ++i) {
            if (i != reservationIndex && allocations[i].live) return false;
        }
        return true;
    }

    bool allocateAbReservation(UInt32 pages, UInt64 totalEntries, UInt64 generation,
                               UInt32 *outOffset)
    {
        if (outOffset) *outOffset = 0;
        if (!pages || !generation) return false;
        if (abReservationTracked) {
            for (UInt32 i = 0; i < allocationCount; ++i) {
                const Range &r = allocations[i];
                if (r.offset != abReservationOffset || r.pages != abReservationPages ||
                    r.ownerIdentity != 0 || r.ownerAllocationId != 0 ||
                    r.allocationGeneration != abReservationGeneration) continue;
                if (!r.live || pages != abReservationPages ||
                    generation != abReservationGeneration) return false;
                if (outOffset) *outOffset = r.offset;
                return true;
            }
            /* The old reservation was fully reclaimed by a tail collapse. */
            abReservationOffset = abReservationPages = 0;
            abReservationGeneration = 0;
            abReservationTracked = false;
        }
        UInt32 offset = 0;
        if (!allocateReservation(pages, totalEntries, generation, &offset)) return false;
        abReservationOffset = offset;
        abReservationPages = pages;
        abReservationGeneration = generation;
        abReservationTracked = true;
        if (outOffset) *outOffset = offset;
        return true;
    }

    /*
     * This range is never handed to the device. It is reclaimable only when no
     * other live MTT allocation exists; otherwise retain and reuse it. Releasing
     * it out of order is deliberately not enough to reuse the hole: release()
     * coalesces only a dead tail, preserving the bump allocator's safety.
     */
    bool releaseAbReservationIfUnowned()
    {
        if (!abReservationTracked) return true;
        UInt32 reservationIndex = kAllocationMax;
        for (UInt32 i = 0; i < allocationCount; ++i) {
            const Range &r = allocations[i];
            if (r.offset == abReservationOffset && r.pages == abReservationPages &&
                r.ownerIdentity == 0 && r.ownerAllocationId == 0 &&
                r.allocationGeneration == abReservationGeneration) {
                reservationIndex = i;
                break;
            }
        }
        if (reservationIndex == kAllocationMax) {
            abReservationOffset = abReservationPages = 0;
            abReservationGeneration = 0;
            abReservationTracked = false;
            return true;
        }
        if (!allocations[reservationIndex].live) return false;
        for (UInt32 i = 0; i < allocationCount; ++i) {
            if (i != reservationIndex && allocations[i].live) return false;
        }
        if (!release(abReservationOffset, abReservationPages)) return false;
        abReservationOffset = abReservationPages = 0;
        abReservationGeneration = 0;
        abReservationTracked = false;
        return true;
    }

    bool canMarkOwnerHandoff(const void *ownerIdentity, UInt64 ownerAllocationId,
                             UInt64 generation) const
    {
        UInt32 found = kAllocationMax;
        for (UInt32 i = 0; i < allocationCount; ++i) {
            const Range &r = allocations[i];
            if (!r.live || r.ownerIdentity != ownerIdentity) continue;
            if (r.ownerAllocationId != ownerAllocationId || found != kAllocationMax ||
                r.allocationGeneration != generation ||
                (r.ownerGeneration != 0 && r.ownerGeneration != generation)) return false;
            found = i;
        }
        return true; /* DMA owners without an MTT range are valid. */
    }

    bool markOwnerHandoff(const void *ownerIdentity, UInt64 ownerAllocationId,
                          UInt64 generation)
    {
        if (!canMarkOwnerHandoff(ownerIdentity, ownerAllocationId, generation)) return false;
        for (UInt32 i = 0; i < allocationCount; ++i) {
            Range &r = allocations[i];
            if (r.live && r.ownerIdentity == ownerIdentity &&
                r.ownerAllocationId == ownerAllocationId) r.ownerGeneration = generation;
        }
        return true;
    }

    bool releaseOwned(UInt32 offset, UInt32 pages, const void *ownerIdentity,
                      UInt64 ownerAllocationId, UInt64 allocationGeneration,
                      UInt64 ownerGeneration)
    {
        if (!pages || !ownerIdentity || !ownerAllocationId || !allocationGeneration ||
            offset < reserved || (UInt64)offset + pages > high) return false;
        for (UInt32 i = 0; i < allocationCount; ++i) {
            Range &r = allocations[i];
            if (r.offset == offset && r.pages == pages && r.live &&
                r.ownerIdentity == ownerIdentity && r.ownerAllocationId == ownerAllocationId &&
                r.allocationGeneration == allocationGeneration &&
                r.ownerGeneration == ownerGeneration) {
                r.live = false;
                while (allocationCount && !allocations[allocationCount - 1].live) {
                    high = allocations[allocationCount - 1].offset;
                    --allocationCount;
                }
                return true;
            }
        }
        return false;
    }

    bool release(UInt32 offset, UInt32 pages)
    {
        if (!pages || offset < reserved || (UInt64)offset + pages > high) return false;
        for (UInt32 i = 0; i < allocationCount; ++i) {
            if (allocations[i].offset == offset && allocations[i].pages == pages &&
                allocations[i].live) {
                allocations[i].live = false;
                while (allocationCount && !allocations[allocationCount - 1].live) {
                    high = allocations[allocationCount - 1].offset;
                    --allocationCount;
                }
                return true;
            }
        }
        return false;
    }
};

/*
 * A/B EQ rings that could not complete queue handback or exact MTT release.
 * The typed ICM pointer is stored as an opaque identity here so the record is
 * host-testable; Mlx4Hw owns the corresponding live allocation until retry.
 */
struct Mlx4AbRingRetention {
    void *icm;
    UInt32 mttOffset;
    UInt32 pages;
    UInt32 eqn;
    UInt64 generation;
    bool fwMayHold;

    Mlx4AbRingRetention() : icm(0), mttOffset(0), pages(0), eqn(0),
        generation(0), fwMayHold(false) {}
    bool valid() const { return icm != 0; }
    void clear()
    {
        icm = 0;
        mttOffset = pages = eqn = 0;
        generation = 0;
        fwMayHold = false;
    }
};

/*
 * Stable PCI identity observed through the current IOPCIDevice provider.
 * This is useful for detecting provider replacement or reset drift, but is
 * deliberately not a DMA-quiescence or resource-release credential.
 */
struct Mlx4PciIdentity {
    UInt16 vendor;
    UInt16 device;
    UInt16 subsystemVendor;
    UInt16 subsystemId;
    UInt8  bus;
    UInt8  deviceNumber;
    UInt8  function;

    void clear()
    {
        vendor = device = subsystemVendor = subsystemId = 0xffffu;
        bus = deviceNumber = function = 0xffu;
    }

    bool valid() const
    {
        return vendor != 0xffffu && device != 0xffffu &&
               subsystemVendor != 0xffffu && subsystemId != 0xffffu &&
               deviceNumber < 32u && function < 8u;
    }

    bool sameFunction(const Mlx4PciIdentity &other) const
    {
        return valid() && other.valid() && vendor == other.vendor &&
               device == other.device && subsystemVendor == other.subsystemVendor &&
               subsystemId == other.subsystemId && bus == other.bus &&
               deviceNumber == other.deviceNumber && function == other.function;
    }
};

/*
 * DMA bookkeeping only: this records when an allocation was made and when it
 * may have been handed to the device. It is never, by itself, a release proof.
 */
struct Mlx4DmaGeneration {
    UInt64 allocationGeneration;
    UInt64 ownerGeneration;
    bool   mayBeDeviceOwned;

    void init(UInt64 generation)
    {
        allocationGeneration = generation;
        ownerGeneration = 0;
        mayBeDeviceOwned = false;
    }

    bool validAllocation() const { return allocationGeneration != 0; }

    bool markHandoffAttempted(UInt64 generation)
    {
        if (!generation) return false;
        ownerGeneration = generation;
        mayBeDeviceOwned = true;
        return true;
    }

    bool matchesOwnerGeneration(UInt64 generation) const
    {
        return mayBeDeviceOwned && ownerGeneration != 0 &&
               ownerGeneration == generation;
    }
};

/*
 * The selector/stop handshake, as pure counters.
 *
 * stop() can run while a selector is inside the driver.  The selector holds the transaction
 * lock, so the two cannot interleave inside a command -- but stop() does not take that lock,
 * and the teardowns it runs (QP->CQ/RQ->tables) are exactly the state a selector might still
 * be using.  The rule this encodes:
 *
 *   - a selector may enter only while the gate is open;
 *   - the first thing stop() does is close the gate, so no NEW selector starts;
 *   - stop() then waits for `active` to reach zero before it touches any resource.
 *
 * `enter()` returns false once stopping is set, and counts the refusal: a selector that
 * arrives during shutdown must get an error, not a pointer into half-torn state.
 */
struct Mlx4LifecycleGate {
    UInt32 active;      /* selectors currently inside externalMethod() */
    UInt32 entered;     /* admitted, for the log */
    UInt32 refused;     /* turned away because stopping was set */
    bool   stopping;    /* set once, by beginStop(); never cleared in this instance */

    void init()
    {
        active = 0;
        entered = 0;
        refused = 0;
        stopping = false;
    }

    bool enter()
    {
        if (stopping) { ++refused; return false; }
        ++active;
        ++entered;
        return true;
    }

    void exit()
    {
        if (active) --active;
    }

    /* Returns true when the gate is already drained; the caller waits for the rest. */
    bool beginStop()
    {
        stopping = true;
        return active == 0;
    }

    bool drained() const { return active == 0; }
};

/*
 * Admission/drain gate for outputPacket() and the network timer action, both of
 * which can touch shared queue state outside the user-client selector boundary.
 * `closed` and `active` share one atomic word so close races linearly with entry:
 * a callback either increments the active count before close, or observes close
 * and is refused. This is a CPU-callback lifetime fence only; it says nothing
 * about device DMA or family callbacks that have not entered this method yet.
 */
struct Mlx4TxAdmissionGate {
    volatile UInt32 state;
    enum { kClosed = 0x80000000u, kActiveMask = 0x7fffffffu };

    void init() { state = 0; }

    UInt32 load() const
    {
        return __sync_fetch_and_add(const_cast<volatile UInt32 *>(&state), 0u);
    }

    bool enter()
    {
        UInt32 old = load();
        for (;;) {
            if (old & kClosed) return false;
            if ((old & kActiveMask) == kActiveMask) return false;
            const UInt32 observed = __sync_val_compare_and_swap(
                &state, old, old + 1u);
            if (observed == old) return true;
            old = observed;
        }
    }

    UInt32 close()
    {
        UInt32 old = load();
        for (;;) {
            if (old & kClosed) return old & kActiveMask;
            const UInt32 observed = __sync_val_compare_and_swap(
                &state, old, old | kClosed);
            if (observed == old) return old & kActiveMask;
            old = observed;
        }
    }

    bool leave()
    {
        UInt32 old = load();
        for (;;) {
            if ((old & kActiveMask) == 0u) return false;
            const UInt32 observed = __sync_val_compare_and_swap(
                &state, old, old - 1u);
            if (observed == old) return true;
            old = observed;
        }
    }

    UInt32 active() const { return load() & kActiveMask; }
    bool drained() const { return active() == 0u; }
    bool isClosed() const { return (load() & kClosed) != 0u; }
};

/*
 * What the two stop-time workers allow.  Device teardown is one transaction over the
 * mailbox, BAR and the resources either worker can still name.  A worker that has not
 * quiesced therefore retains the whole transaction instead of racing a partially-torn
 * state.  The lock that protects the still-running selector is released only once that
 * selector has left.
 */
struct Mlx4StopDisposition {
    bool selectorDrained;
    bool workerQuiesced;

    void init(bool selectors, bool worker)
    {
        selectorDrained = selectors;
        workerQuiesced = worker;
    }

    bool complete() const { return selectorDrained && workerQuiesced; }
    bool poisoned() const { return !workerQuiesced; }
    bool retainSelectorLock() const { return !selectorDrained; }
    bool settleAttach() const { return workerQuiesced; }
};

/*
 * A command that timed out leaves the device able to read its inbox or write its outbox
 * after the driver has stopped posting.  The mailbox therefore outlives shutdown in that
 * state.  Its IODMACommand and memory descriptor stay retained, and with them the IOMMU
 * mapping, until reboot; an ordinary mailbox is released normally.
 */
struct Mlx4MailboxDisposition {
    bool wedged;

    void init(bool timeoutPoison) { wedged = timeoutPoison; }
    bool retain() const { return wedged; }
    bool release() const { return !wedged; }
};

struct Mlx4FlrProof {
    bool supported;
    bool pendingCleared;
    bool cpuDrained;
    bool resetIssued;
    bool readyReadable;
    bool scopeAccounted;

    void init()
    {
        supported = pendingCleared = cpuDrained = false;
        resetIssued = readyReadable = scopeAccounted = false;
    }

    bool credential() const
    {
        return supported && pendingCleared && cpuDrained &&
               resetIssued && readyReadable && scopeAccounted;
    }
};

/*
 * The evidence required before a Function Level Reset can stand in for a
 * device-quiescence proof.  Each field is an observation or a completed action; the enum tag
 * is not enough on its own.  `scopeAccounted` covers the other function on this ASIC, which
 * may share the reset domain.
 */

/*
 * How much of each command's outbox is kept.  QUERY_DEV_CAP needs a wide window
 * (the profile is computed from the entry sizes at 0x80-0x92 and the ICM limit at
 * 0xa0); QUERY_FW needs only its first few dwords.  They are separate named
 * constants because conflating them is how the stack overflow happened.
 */
enum { MLX4_DEVCAP_WORDS = 64 };
enum { MLX4_QUERY_FW_WORDS = 24 };

struct Mlx4QueryFwResult {
    bool     ok;
    UInt32   fwStatus;      /* status byte from the HCR, 0 == success */
    UInt64   fwVer;         /* computed exactly as dev->caps.fw_ver */
    UInt8    functionId;    /* QUERY_FW_PPF_ID (ppf id) */
    UInt16   cmdIfRev;
    UInt8    maxCmdLog2;
    UInt32   maxCmds;
    UInt64   catasOffset;
    UInt32   catasSize;
    UInt32   catasBar;
    UInt16   fwPages;       /* QUERY_FW_SIZE_OFFSET (0x00), in 4 KiB pages */
    UInt16   fwPages0x10;   /* the 0x10 candidate, recorded for contrast only */
    /*
     * Where the interrupt-clear register lives.  Both are in the QUERY_FW outbox
     * this port already keeps; they were simply never decoded, because nothing
     * needed them until the legacy interrupt path.
     *
     * clrIntBase is a u64 at 0x20.  clrIntBar is a u32 at 0x28 whose value the
     * reference converts with (v >> 6) * 2 -- it selects a BAR in the same encoding
     * PCI config space uses, not a raw bar index (mlx4_fw.c:1693-1694).
     */
    UInt64   clrIntBase;
    UInt32   clrIntBar;
    UInt32   rawWords[MLX4_QUERY_FW_WORDS];  /* the outbox, host-order dwords */
};

/*
 * QUERY_DEV_CAP (0x3), the device's capability set.
 *
 * Same command shape as QUERY_FW: a 4 KiB outbox, no inbox.  The differences
 * that matter are the offsets, which are byte offsets rather than word offsets,
 * and that several of them are single bytes, so they cannot be read as dwords.
 * The whole 256-byte record is kept: the context-memory profile is computed from
 * fields that live past the first 96 bytes.
 */
struct Mlx4QueryDevCapResult {
    bool     ok;
    UInt32   fwStatus;

    UInt8    maxSrqSzLog2;    /* 0x10 */
    UInt8    maxQpSzLog2;     /* 0x11 */
    UInt8    reservedQps;     /* 0x12, low nibble */
    UInt8    maxQpLog2;       /* 0x13 */
    UInt8    reservedSrqs;    /* 0x14, high nibble */
    UInt8    maxSrqLog2;      /* 0x15 */
    UInt8    maxCqSzLog2;     /* 0x19 */
    UInt8    reservedCqs;     /* 0x1a, low nibble */
    UInt8    maxCqLog2;       /* 0x1b */
    UInt8    maxMptLog2;      /* 0x1d */
    UInt8    reservedEqs;     /* 0x1e, low nibble */
    UInt8    maxEqLog2;       /* 0x1f, low nibble */
    UInt8    reservedMtts;    /* 0x20, high nibble */
    UInt8    maxMttSegLog2;   /* 0x23 */
    UInt8    maxGso;          /* 0x2d, low 5 bits */
    UInt8    mtuWidth;        /* 0x36 */
    UInt8    maxMsgSzLog2;    /* 0x38, low 5 bits */
    UInt8    maxGidLog2;      /* 0x3b */
    UInt8    reservedUars;    /* 0x48, high nibble */
    UInt8    uarSzLog2;       /* 0x49, code (not a plain log2) */
    UInt32   extFlags;        /* 0x40 */
    UInt64   flags;           /* 0x44, the 32-bit word */
    UInt32   pageSizeCap;     /* 0x4b, the byte */
    UInt32   minPageSize;     /* 1 << that byte */

    /*
     * The whole QUERY_DEV_CAP record, host-order dwords.  This is the array the
     * fill loop and mlx4DevCapBytes() both index with MLX4_DEVCAP_WORDS, and
     * CX3_STATIC_ASSERT ties its width to the user-space ABI's kDevCapWords so
     * the two can never drift apart again.
     */
    UInt32   rawWords[MLX4_DEVCAP_WORDS];
};

/* Forward declaration only: the pure code never dereferences an ICM area, it
 * just records pointers to them in the table list. */
struct Mlx4Icm;

/*
 * The subset of QUERY_DEV_CAP this port actually consumes, decoded with the
 * reference's own expressions (FreeBSD mlx4_fw.c:836-1060).
 *
 * Only the fields something reads are here on purpose: a full transcription of
 * an 80-byte capability record would be mostly dead weight, and every field
 * carried is one that has to be kept right.  The offsets themselves are all in
 * mlx4_regs.h, so adding a field later is a decode line, not new constants.
 */
struct Mlx4DevCap {
    UInt32 max_qps, reserved_qps, max_cqs, reserved_cqs;
    UInt32 max_srqs, reserved_srqs, max_eqs, reserved_eqs;
    UInt32 max_mpts, reserved_mtts, reserved_mrws;
    /*
     * PDs the firmware owns.  The allocator must start above these
     * (mlx4_pd.c:122-124), and this port did not decode the field at all
     * until R4 needed a PD -- the reference takes it from the high nibble
     * of the byte at QUERY_DEV_CAP_RSVD_PD_OFFSET (mlx4_fw.c:964-965).
     */
    UInt32 reserved_pds;
    UInt32 max_srq_sz, max_qp_sz, max_cq_sz;
    UInt32 max_msg_sz, max_gso_sz;
    UInt32 max_mcg, reserved_mcg, max_pd, max_xrc, max_eec;
    UInt32 max_qp_per_mcg, max_counters;
    UInt32 max_sq_sg, max_rq_sg, max_sq_desc_sz, max_rq_desc_sz;
    UInt32 num_ports, num_sys_eqs;
    UInt32 uar_size, reserved_uars, min_page_sz;
    UInt32 stat_rate_support, local_ca_ack_delay;
    UInt32 max_requester_per_qp, max_responder_per_qp, max_rdma_global;
    UInt32 bf_reg_size, bf_regs_per_page;
    UInt32 qpc_entry_sz, rdmarc_entry_sz, altc_entry_sz, aux_entry_sz;
    UInt32 srq_entry_sz, cqc_entry_sz, eqc_entry_sz;
    UInt32 dmpt_entry_sz, cmpt_entry_sz, mtt_entry_sz;
    UInt64 max_icm_sz;
    UInt64 flags;         /* ext_flags << 32 | flags */
    UInt32 flags2;        /* collected from the scattered single-bit fields */
    UInt32 bmme_flags;    /* 0x94, u32: memory-window and type-2 window bits */
    UInt32 rss_log_max_tbl;
};

/*
 * The reference's resource profile (mlx4_profile.c:69): how many of each object
 * the driver asks the HCA to make room for.  These six numbers decide icm_size,
 * so they are inputs rather than constants -- a smaller profile means fewer ICM
 * pages to map, which matters on a driver that does not need 2^18 QPs.
 */
struct Mlx4Profile {
    UInt32 num_qp;
    UInt32 rdmarc_per_qp;
    UInt32 num_srq;
    UInt32 num_cq;
    UInt32 num_mcg;
    UInt32 num_mpt;
    UInt32 num_mtt;      /* in segments */
};

/*
 * The result of packing the profile: where each context table lives in the
 * HCA's context memory, which is exactly what INIT_HCA is told (mlx4_profile.c
 * fills this struct, mlx4_fw.c:1930-2060 encodes it).
 */
struct Mlx4InitHcaParam {
    UInt64 qpc_base, rdmarc_base, altc_base, auxc_base, srqc_base;
    UInt64 cqc_base, eqc_base, mc_base, dmpt_base, mtt_base, cmpt_base;
    UInt8  log_num_qps, log_rd_per_qp, log_num_srqs, log_num_cqs;
    UInt8  log_num_eqs, log_mpt_sz, log_mc_table_sz;
    UInt8  log_uar_sz, uar_page_sz, mw_enabled;
    /*
     * These three are u16 in the reference's mlx4_init_hca_param (fw.h), and the
     * widths matter:
     *   num_sys_eqs can exceed 255 -- this card reports 1024, and it was a UInt8
     *   here, which silently truncated it to 0 and would have told INIT_HCA that
     *   the HCA has no system EQs.
     *   log_mc_entry_sz / log_mc_hash_sz are written as 2 bytes big-endian by
     *   MLX4_PUT; writing one byte happens to be identical for the values used
     *   (10 and 7) only because the inbox is zeroed first.
     */
    UInt16 log_mc_entry_sz, log_mc_hash_sz;
    UInt16 num_sys_eqs;
};

/* One resource of the packed profile, for reporting. */
struct Mlx4ProfileEntry {
    const char *name;
    UInt64      size;
    UInt64      start;
    UInt32      num;
    UInt32      logNum;
};
enum { MLX4_PROFILE_MAX_ENTRIES = 11 };   /* MLX4_RES_NUM */

/*
 * One context-memory table, as mapped into the HCA.
 *
 * The reference keeps an array of ICM chunks per table (mlx4_icm.h:55,
 * mlx4_init_icm_table at mlx4_icm.c:387) because tables are mapped lazily as
 * objects are used.  Only the reserved prefix is mapped up front, which is what
 * the firmware needs at INIT_HCA, so one chunk per table is enough here: the
 * reserved prefix is at most 256 KiB by construction.
 */
struct Mlx4IcmTable {
    const char *name;
    UInt64      virt;       /* HCA virtual base of the table */
    UInt32      objSize;    /* bytes per object */
    UInt32      nObj;       /* objects in the whole table */
    UInt32      reserved;   /* objects that must exist before INIT_HCA */
    bool        lowmem;
    bool        coherent;
    /*
     * Chunks of the reserved prefix, one per 256 KiB.  Bounded rather than
     * dynamic because the reserved prefixes are small by construction and a
     * checked bound is simpler than an allocation whose failure mode is a
     * half-mapped table.  The bound is read from this array's own size, so the
     * count and the array cannot disagree.
     */
    Mlx4Icm    *icm[8];
};

/* ilog2 and round-up-to-power-of-two, spelled out because the reference relies on
 * exact power-of-two behaviour for the packing alignment. */
inline UInt32 mlx4Log2(UInt64 v)
{
    UInt32 l = 0;
    while (v > 1) { v >>= 1; ++l; }
    return l;
}
inline UInt32 mlx4RoundupPow2(UInt32 v)
{
    UInt32 p = 1;
    if (v == 0) return 0;
    while (p < v) p <<= 1;
    return p;
}

/*
 * Rebuild a QUERY_DEV_CAP outbox as the device wrote it: a big-endian byte
 * stream.  The words arrive host-order from the fill loop, so indexing them as
 * bytes would reverse every dword and every single-byte field with it -- a
 * mistake this project has now made twice, once in the user-space tool and once
 * here.
 */
inline void mlx4DevCapBytes(const Mlx4QueryDevCapResult *r, UInt8 *be)
{
    UInt32 i;
    for (i = 0; i < MLX4_DEVCAP_WORDS; ++i) {
        be[i * 4 + 0] = (UInt8)((r->rawWords[i] >> 24) & 0xff);
        be[i * 4 + 1] = (UInt8)((r->rawWords[i] >> 16) & 0xff);
        be[i * 4 + 2] = (UInt8)((r->rawWords[i] >>  8) & 0xff);
        be[i * 4 + 3] = (UInt8)( r->rawWords[i]        & 0xff);
    }
}

/*
 * Decode a big-endian QUERY_DEV_CAP outbox into the fields this port uses.
 *
 * Transcribed expression for expression from the reference; see the comments
 * inside for the fields whose raw byte is not the value.
 */
inline void mlx4DecodeDevCap(const UInt8 *b, Mlx4DevCap *out)
{
    /* Byte, big-endian 16/32/64-bit reads, exactly as MLX4_GET does them
     * (mlx4_fw.c:58-70).  The pointer is a byte stream, never the rawWords
     * array, because those words are already host-order. */
#define F(o)  ((UInt32)b[(o)])
#define W16(o) ((UInt32)(((UInt32)b[(o)] << 8) | (UInt32)b[(o)+1]))
#define W32(o) (((UInt32)b[(o)] << 24) | ((UInt32)b[(o)+1] << 16) | \
                ((UInt32)b[(o)+2] << 8) | (UInt32)b[(o)+3])
#define W64(o) (((UInt64)W32(o) << 32) | (UInt64)W32((o)+4))
    /* Counts.  The masks are the reference's: several of these bytes pack two
     * values, and reading the byte as if it were the count has been a real
     * failure mode in the user-space tool already (mlx4_fw.c:840-880). */
    out->reserved_qps  = 1u << (F(QUERY_DEV_CAP_RSVD_QP_OFFSET) & 0xf);
    out->max_qps       = 1u << (F(QUERY_DEV_CAP_MAX_QP_OFFSET) & 0x1f);
    out->reserved_srqs = 1u << (F(QUERY_DEV_CAP_RSVD_SRQ_OFFSET) >> 4);
    out->max_srqs      = 1u << (F(QUERY_DEV_CAP_MAX_SRQ_OFFSET) & 0x1f);
    out->reserved_cqs  = 1u << (F(QUERY_DEV_CAP_RSVD_CQ_OFFSET) & 0xf);
    out->max_cqs       = 1u << (F(QUERY_DEV_CAP_MAX_CQ_OFFSET) & 0x1f);
    out->max_mpts      = 1u << (F(QUERY_DEV_CAP_MAX_MPT_OFFSET) & 0x3f);
    out->reserved_eqs  = 1u << (F(QUERY_DEV_CAP_RSVD_EQ_OFFSET) & 0xf);
    out->max_eqs       = 1u << (F(QUERY_DEV_CAP_MAX_EQ_OFFSET) & 0xf);
    out->reserved_mtts = 1u << (F(QUERY_DEV_CAP_RSVD_MTT_OFFSET) >> 4);
    out->reserved_mrws = 1u << (F(QUERY_DEV_CAP_RSVD_MRW_OFFSET) & 0xf);
    out->reserved_pds  = F(QUERY_DEV_CAP_RSVD_PD_OFFSET) >> 4;
    out->num_sys_eqs   = (UInt32)W16(QUERY_DEV_CAP_NUM_SYS_EQ_OFFSET) & 0xfffu;
    out->reserved_mcg  = F(QUERY_DEV_CAP_RSVD_MCG_OFFSET) & 0xf;
    out->max_mcg       = 1u << (F(QUERY_DEV_CAP_MAX_MCG_OFFSET) & 0x1f);
    out->max_pd        = 1u << (F(QUERY_DEV_CAP_MAX_PD_OFFSET) & 0x3f);
    out->max_xrc       = 1u << (F(QUERY_DEV_CAP_MAX_XRC_OFFSET) & 0x1f);
    out->max_eec       = 1u << (F(QUERY_DEV_CAP_MAX_EEC_OFFSET) & 0x1f);
    out->max_qp_per_mcg = 1u << F(QUERY_DEV_CAP_MAX_QP_MCG_OFFSET);
    out->max_requester_per_qp = 1u << (F(QUERY_DEV_CAP_MAX_REQ_QP_OFFSET) & 0x3f);
    out->max_responder_per_qp = 1u << (F(QUERY_DEV_CAP_MAX_RES_QP_OFFSET) & 0x3f);
    out->max_rdma_global = 1u << (F(QUERY_DEV_CAP_MAX_RDMA_OFFSET) & 0x3f);

    /* Sizes. */
    out->max_srq_sz = 1u << F(QUERY_DEV_CAP_MAX_SRQ_SZ_OFFSET);
    out->max_qp_sz  = 1u << F(QUERY_DEV_CAP_MAX_QP_SZ_OFFSET);
    out->max_cq_sz  = 1u << F(QUERY_DEV_CAP_MAX_CQ_SZ_OFFSET);
    out->max_msg_sz = 1u << (F(QUERY_DEV_CAP_MAX_MSG_SZ_OFFSET) & 0x1f);
    /* A zero max-GSO byte means "no LSO", not "1 << 0" (mlx4_fw.c:875-878). */
    out->max_gso_sz = (F(QUERY_DEV_CAP_MAX_GSO_OFFSET) & 0x1f) ?
                      1u << (F(QUERY_DEV_CAP_MAX_GSO_OFFSET) & 0x1f) : 0;
    out->max_sq_sg       = F(QUERY_DEV_CAP_MAX_SG_SQ_OFFSET);
    out->max_rq_sg       = F(QUERY_DEV_CAP_MAX_SG_RQ_OFFSET);
    out->max_sq_desc_sz  = (UInt32)W16(QUERY_DEV_CAP_MAX_DESC_SZ_SQ_OFFSET);
    out->max_rq_desc_sz  = (UInt32)W16(QUERY_DEV_CAP_MAX_DESC_SZ_RQ_OFFSET);
    out->stat_rate_support = (UInt32)W16(QUERY_DEV_CAP_RATE_SUPPORT_OFFSET);
    out->local_ca_ack_delay = F(QUERY_DEV_CAP_ACK_DELAY_OFFSET) & 0x1f;
    out->num_ports      = F(QUERY_DEV_CAP_VL_PORT_OFFSET) & 0xf;
    out->reserved_uars  = F(QUERY_DEV_CAP_RSVD_UAR_OFFSET) >> 4;
    /* uar_size is a log2 with a 20-bit bias, not a byte count
     * (mlx4_fw.c:934-935: 1 << ((field & 0x3f) + 20)). */
    out->uar_size       = 1u << ((F(QUERY_DEV_CAP_UAR_SZ_OFFSET) & 0x3f) + 20);
    /* min_page_sz is also a log2 (mlx4_fw.c:936: min_page_sz = 1 << field). */
    out->min_page_sz    = 1u << F(QUERY_DEV_CAP_PAGE_SZ_OFFSET);
    if (F(QUERY_DEV_CAP_BF_OFFSET) & 0x80) {
        out->bf_reg_size     = 1u << (F(QUERY_DEV_CAP_LOG_BF_REG_SZ_OFFSET) & 0x1f);
        out->bf_regs_per_page = 1u << (F(QUERY_DEV_CAP_LOG_MAX_BF_REGS_PER_PAGE_OFFSET) & 0x3f);
    }

    /* Entry sizes: plain u16 BE, and the profile is built from them. */
    out->rdmarc_entry_sz = (UInt32)W16(QUERY_DEV_CAP_RDMARC_ENTRY_SZ_OFFSET);
    out->qpc_entry_sz    = (UInt32)W16(QUERY_DEV_CAP_QPC_ENTRY_SZ_OFFSET);
    out->aux_entry_sz    = (UInt32)W16(QUERY_DEV_CAP_AUX_ENTRY_SZ_OFFSET);
    out->altc_entry_sz   = (UInt32)W16(QUERY_DEV_CAP_ALTC_ENTRY_SZ_OFFSET);
    out->eqc_entry_sz    = (UInt32)W16(QUERY_DEV_CAP_EQC_ENTRY_SZ_OFFSET);
    out->cqc_entry_sz    = (UInt32)W16(QUERY_DEV_CAP_CQC_ENTRY_SZ_OFFSET);
    out->srq_entry_sz    = (UInt32)W16(QUERY_DEV_CAP_SRQ_ENTRY_SZ_OFFSET);
    out->cmpt_entry_sz   = (UInt32)W16(QUERY_DEV_CAP_C_MPT_ENTRY_SZ_OFFSET);
    out->mtt_entry_sz    = (UInt32)W16(QUERY_DEV_CAP_MTT_ENTRY_SZ_OFFSET);
    out->dmpt_entry_sz   = (UInt32)W16(QUERY_DEV_CAP_D_MPT_ENTRY_SZ_OFFSET);

    /* Flags: the 64-bit capability word is ext_flags<<32 | flags, where both are
     * 32-bit reads (mlx4_fw.c:928-930). */
    out->flags  = (UInt64)W32(QUERY_DEV_CAP_FLAGS_OFFSET) |
                  ((UInt64)W32(QUERY_DEV_CAP_EXT_FLAGS_OFFSET) << 32);
    /* fw.h declares max_counters as u32: MLX4_GET reads four BE bytes,
     * conditional on COUNTERS support, rather than a log2 byte. */
    out->max_counters = (out->flags & MLX4_DEV_CAP_FLAG_COUNTERS) ?
                       W32(QUERY_DEV_CAP_MAX_COUNTERS_OFFSET) : 0;
    out->max_icm_sz = W64(QUERY_DEV_CAP_MAX_ICM_SZ_OFFSET);
    out->bmme_flags = W32(QUERY_DEV_CAP_BMME_FLAGS_OFFSET);

    /* flags2 is assembled bit by bit in the reference; only the bits this port
     * acts on are collected, each with the test the reference uses. */
    {
        UInt32 rss = F(QUERY_DEV_CAP_RSS_OFFSET);
        if (rss & 0xf) {
            out->flags2 |= MLX4_DEV_CAP_FLAG2_RSS;
            out->rss_log_max_tbl = 1u << (rss & 0xf);
        }
        if (F(QUERY_DEV_CAP_CQ_TS_SUPPORT_OFFSET) & 0x80)
            out->flags2 |= MLX4_DEV_CAP_FLAG2_TS;
        if (F(QUERY_DEV_CAP_FLOW_STEERING_RANGE_EN_OFFSET) & 0x80)
            out->flags2 |= (1u << 3);        /* FS_EN */
        if (F(QUERY_DEV_CAP_VXLAN) & (1 << 3))
            out->flags2 |= MLX4_DEV_CAP_FLAG2_VXLAN_OFFLOADS;
        /* Bit 7 of the u32 at 0x9c (mlx4_fw.c:1032-1036; mainline fw.c:1041-1045
         * says the same, not inverted).  This device's byte is 0x5e, so the flag
         * is off here -- but leaving it undecoded would silently skip the
         * INIT_HCA bit on a device that has it. */
        if (W32(QUERY_DEV_CAP_ETH_BACKPL_OFFSET) & (1u << 7))
            out->flags2 |= MLX4_DEV_CAP_FLAG2_RECOVERABLE_ERROR_EVENT;
    }
    /*
     * "Does the HCA have system EQs" is not a capability bit: the reference
     * derives it from num_sys_eqs, and when there are none it raises the
     * reserved-EQ count to cover the UAR doorbell pages instead
     * (mlx4_fw.c:1104-1107).  Getting this backwards changes both the EQ profile
     * and the EQ numbering model, so it is decided here, once.
     */
    if (out->num_sys_eqs == 0) {
        UInt32 byUar = out->reserved_uars * 4;
        if (byUar > out->reserved_eqs) out->reserved_eqs = byUar;
    } else {
        out->flags2 |= MLX4_DEV_CAP_FLAG2_SYS_EQS;
    }

}

/*
 * mlx4_make_profile() -- FreeBSD mlx4_profile.c:69.
 *
 * Packs the requested object counts into the HCA's context memory.  The
 * reference's method is kept exactly, because the layout is not arbitrary: every
 * resource's size is rounded up to a power of two, the resources are sorted by
 * decreasing size, and then packed end to end with no gaps.  That is what makes
 * "each table is aligned to its own size" true for free, and INIT_HCA then simply
 * receives the resulting offsets.
 *
 * tableCapacity and numTablesOut are separate parameters on purpose.  The first
 * version took one pointer that meant "how many slots you have" on the way in and
 * "how many I wrote" on the way out, and cleared it before reading it -- so the
 * capacity became 0 and the table list was reported as overflowing.  An in/out
 * count is a trap; two names cannot be confused.
 *
 * Returns the total size in bytes, or 0 if the profile cannot be satisfied.
 */

inline UInt64 mlx4MakeProfile(const Mlx4Profile *request, const Mlx4DevCap *cap,
                       Mlx4InitHcaParam *param, Mlx4IcmTable *tables,
                       UInt32 tableCapacity, UInt32 *numTablesOut,
                       Mlx4ProfileEntry *entries, UInt32 entryCapacity,
                       UInt32 *nEntriesOut)
{
    struct Res {
        UInt64 size;
        UInt64 start;
        UInt32 type;
        UInt32 num;
        UInt32 logNum;
    };
    enum { RES_QP, RES_RDMARC, RES_ALTC, RES_AUXC, RES_SRQ, RES_CQ, RES_EQ,
           RES_DMPT, RES_CMPT, RES_MTT, RES_MCG, RES_NUM };
    static const char *resName[RES_NUM] = {
        "QP", "RDMARC", "ALTC", "AUXC", "SRQ", "CQ", "EQ",
        "DMPT", "CMPT", "MTT", "MCG"
    };

    Res res[RES_NUM];
    UInt64 total = 0;
    UInt32 i, j;
    UInt32 mgmEntrySize = 1u << MLX4_DEFAULT_MGM_LOG_ENTRY_SIZE;
    UInt32 eqProfileNum, eqTableNum;

    if (!request || !cap || !param) return 0;
    memset(res, 0, sizeof(res));
    memset(param, 0, sizeof(*param));
    if (tables && tableCapacity)
        memset(tables, 0, sizeof(Mlx4IcmTable) * tableCapacity);
    if (numTablesOut) *numTablesOut = 0;

    /* Sizes come from the capability set; counts come from the request. */
    res[RES_QP].size      = cap->qpc_entry_sz;
    res[RES_RDMARC].size  = cap->rdmarc_entry_sz;
    res[RES_ALTC].size    = cap->altc_entry_sz;
    res[RES_AUXC].size    = cap->aux_entry_sz;
    res[RES_SRQ].size     = cap->srq_entry_sz;
    res[RES_CQ].size      = cap->cqc_entry_sz;
    res[RES_EQ].size      = cap->eqc_entry_sz;
    res[RES_DMPT].size    = cap->dmpt_entry_sz;
    res[RES_CMPT].size    = cap->cmpt_entry_sz;
    res[RES_MTT].size     = cap->mtt_entry_sz;
    res[RES_MCG].size     = mgmEntrySize;

    res[RES_QP].num      = request->num_qp;
    res[RES_RDMARC].num  = request->num_qp * request->rdmarc_per_qp;
    res[RES_ALTC].num    = request->num_qp;
    res[RES_AUXC].num    = request->num_qp;
    res[RES_SRQ].num     = request->num_srq;
    res[RES_CQ].num      = request->num_cq;
    /*
     * The EQs appear twice with two different meanings, and the reference uses
     * two different numbers:
     *
     *   the profile region is sized for min(max_eqs, MAX_MSIX) -- how many EQs the
     *   driver will create (mlx4_profile.c:108-110, same in mainline).  The
     *   multi-function case uses phys_caps.num_phys_eqs instead, which does not
     *   apply to a plain PF.
     *
     *   the table mappings use phys_caps.num_phys_eqs, i.e. num_sys_eqs on a
     *   device with system EQs, else MLX4_MAX_EQ_NUM (mlx4_main.c:376-379, and
     *   the num_eqs in mlx4_init_icm/mlx4_init_cmpt_table at :1539 and :1600).
     *
     * Note what that means: on this card the EQC region is reserved as 64 entries
     * (8 KiB) while its table maps 1024 objects (128 KiB).  That is what the
     * reference does on this exact device and Linux drives this card with it, so
     * it is reproduced rather than "corrected" -- but it is recorded here as an
     * unexplained overlap, not as something understood.
     */
    eqProfileNum = cap->max_eqs;
    if (eqProfileNum > MLX4_MAX_MSIX) eqProfileNum = MLX4_MAX_MSIX;
    eqTableNum = (cap->flags2 & MLX4_DEV_CAP_FLAG2_SYS_EQS) ? cap->num_sys_eqs
                                                            : MLX4_MAX_EQ_NUM;
    res[RES_EQ].num      = eqProfileNum;
    res[RES_DMPT].num    = request->num_mpt;
    res[RES_CMPT].num    = (UInt32)MLX4_CMPT_TYPE_EQ << MLX4_CMPT_SHIFT; /* NUM_CMPTS */
    res[RES_MTT].num     = request->num_mtt * (1u << 3);   /* 1 << log_mtts_per_seg */
    res[RES_MCG].num     = request->num_mcg;

    for (i = 0; i < RES_NUM; ++i) {
        res[i].type   = i;
        res[i].num    = mlx4RoundupPow2(res[i].num);
        res[i].logNum = mlx4Log2(res[i].num);
        res[i].size  *= res[i].num;
        if (res[i].size < 4096) res[i].size = 4096;   /* max(size, PAGE_SIZE) */
    }

    if (nEntriesOut) *nEntriesOut = (entryCapacity < RES_NUM) ? entryCapacity : RES_NUM;

    /* Decreasing size, so each power-of-two resource stays aligned by packing. */
    for (i = RES_NUM; i > 0; --i)
        for (j = 1; j < i; ++j)
            if (res[j].size > res[j - 1].size) { Res t = res[j]; res[j] = res[j - 1]; res[j - 1] = t; }

    for (i = 0; i < RES_NUM; ++i) {
        if (res[i].size) {
            res[i].start = total;
            total += res[i].size;
        }
        /* Hand the packed layout back for logging: the kernel log's per-table
         * lines are the main diagnostic when INIT_HCA fails, and this way the
         * arithmetic and the reporting cannot disagree. */
        if (entries && i < entryCapacity) {
            entries[i].name   = resName[res[i].type];
            entries[i].size   = res[i].size;
            entries[i].start  = res[i].start;
            entries[i].num    = res[i].num;
            entries[i].logNum = res[i].logNum;
        }
        if (total > cap->max_icm_sz) {
            return 0;
        }
    }

    /*
     * After sorting, res[] is in decreasing size order, so a resource can no
     * longer be found by index.  Index it by type once, here, instead of trying
     * to remember where each one landed.
     */
    {
        Res *byType[RES_NUM];
        for (i = 0; i < RES_NUM; ++i) byType[res[i].type] = &res[i];

        param->qpc_base    = byType[RES_QP]->start;
        param->log_num_qps = (UInt8)byType[RES_QP]->logNum;
        param->srqc_base    = byType[RES_SRQ]->start;
        param->log_num_srqs = (UInt8)byType[RES_SRQ]->logNum;
        param->cqc_base    = byType[RES_CQ]->start;
        param->log_num_cqs = (UInt8)byType[RES_CQ]->logNum;
        param->eqc_base    = byType[RES_EQ]->start;
        param->altc_base   = byType[RES_ALTC]->start;
        param->auxc_base   = byType[RES_AUXC]->start;
        param->dmpt_base   = byType[RES_DMPT]->start;
        param->log_mpt_sz  = (UInt8)byType[RES_DMPT]->logNum;
        param->cmpt_base   = byType[RES_CMPT]->start;
        param->mtt_base    = byType[RES_MTT]->start;
        param->mc_base     = byType[RES_MCG]->start;
        param->log_mc_entry_sz = MLX4_DEFAULT_MGM_LOG_ENTRY_SIZE;
        param->log_mc_table_sz = (UInt8)byType[RES_MCG]->logNum;
        /*
         * A0 steering splits the multicast table into a hash half and an
         * associated-memory half; B0 and device-managed modes do not
         * (mlx4_profile.c:222-236).
         */
        param->log_mc_hash_sz  = (UInt8)(byType[RES_MCG]->logNum - 1);

        /*
         * The RDMARC table has one entry per QP, rounded up to a power of two;
         * the excess per QP is what the HCA is told as log_rd_per_qp
         * (mlx4_profile.c:174-181).
         */
        {
            UInt32 shift = 0;
            while ((request->num_qp << shift) < byType[RES_RDMARC]->num) ++shift;
            param->rdmarc_base   = byType[RES_RDMARC]->start;
            param->log_rd_per_qp = (UInt8)shift;
        }

        if (cap->flags2 & MLX4_DEV_CAP_FLAG2_SYS_EQS) {
            param->log_num_eqs = 0x1f;   /* "the HCA numbers EQs globally" */
            param->num_sys_eqs = (UInt16)cap->num_sys_eqs;
        } else {
            param->log_num_eqs = (UInt8)byType[RES_EQ]->logNum;
        }

        /* Memory windows are enabled if the HCA advertises them either as a
         * dev_cap flag or as a BMME type-2 window (mlx4_main.c:2276-2280). */
        if ((cap->flags & MLX4_DEV_CAP_FLAG_MEM_WINDOW) ||
            (cap->bmme_flags & MLX4_BMME_FLAG_TYPE_2_WIN))
            param->mw_enabled = (UInt8)INIT_HCA_TPT_MW_ENABLE;

        /*
         * Everything INIT_HCA refers to has to be mapped before INIT_HCA can
         * succeed, so the table list is built from the same numbers.  The
         * reference maps only the reserved prefix up front and the rest lazily
         * (mlx4_icm.c:400-405); at most 256 KiB of objects is reserved, so one
         * chunk per table covers it and a chunk list is not needed.
         */
        if (tables && tableCapacity) {
            UInt32 capTables = tableCapacity;
            UInt32 k = 0;
            UInt64 cmptStep = ((UInt64)cap->cmpt_entry_sz) << MLX4_CMPT_SHIFT;
            UInt32 mcgObj = byType[RES_MCG]->num;   /* num_mgms + num_amgms */

#define ADD_TABLE(nm, virt_, objsz, nobj, resv, low, coh)     \
            do {                                              \
                if (k < capTables) {                          \
                    tables[k].name     = (nm);                \
                    tables[k].virt     = (virt_);             \
                    tables[k].objSize  = (objsz);             \
                    tables[k].nObj     = (nobj);              \
                    tables[k].reserved = (resv);              \
                    tables[k].lowmem   = (low);               \
                    tables[k].coherent = (coh);               \
                    { UInt32 c_; for (c_ = 0; c_ < 8; ++c_) tables[k].icm[c_] = 0; } \
                }                                             \
                ++k;                                          \
            } while (0)

            ADD_TABLE("cmpt/QP", param->cmpt_base + MLX4_CMPT_TYPE_QP * cmptStep,
                      cap->cmpt_entry_sz, byType[RES_QP]->num, cap->reserved_qps,
                      false, false);
            ADD_TABLE("cmpt/SRQ", param->cmpt_base + MLX4_CMPT_TYPE_SRQ * cmptStep,
                      cap->cmpt_entry_sz, byType[RES_SRQ]->num, cap->reserved_srqs,
                      false, false);
            ADD_TABLE("cmpt/CQ", param->cmpt_base + MLX4_CMPT_TYPE_CQ * cmptStep,
                      cap->cmpt_entry_sz, byType[RES_CQ]->num, cap->reserved_cqs,
                      false, false);
            ADD_TABLE("cmpt/EQ", param->cmpt_base + MLX4_CMPT_TYPE_EQ * cmptStep,
                      cap->cmpt_entry_sz, eqTableNum, eqTableNum, false, false);
            ADD_TABLE("EQC", param->eqc_base, cap->eqc_entry_sz, eqTableNum,
                      eqTableNum, false, false);
            ADD_TABLE("MTT", param->mtt_base, cap->mtt_entry_sz,
                      byType[RES_MTT]->num, cap->reserved_mtts, true, false);
            ADD_TABLE("dMPT", param->dmpt_base, cap->dmpt_entry_sz,
                      byType[RES_DMPT]->num, cap->reserved_mrws, true, true);
            ADD_TABLE("QPC", param->qpc_base, cap->qpc_entry_sz,
                      byType[RES_QP]->num, cap->reserved_qps, false, false);
            ADD_TABLE("AUXC", param->auxc_base, cap->aux_entry_sz,
                      byType[RES_QP]->num, cap->reserved_qps, false, false);
            ADD_TABLE("ALTC", param->altc_base, cap->altc_entry_sz,
                      byType[RES_QP]->num, cap->reserved_qps, false, false);
            ADD_TABLE("RDMARC", param->rdmarc_base,
                      cap->rdmarc_entry_sz << param->log_rd_per_qp,
                      byType[RES_QP]->num, cap->reserved_qps, false, false);
            ADD_TABLE("CQC", param->cqc_base, cap->cqc_entry_sz,
                      byType[RES_CQ]->num, cap->reserved_cqs, false, false);
            ADD_TABLE("SRQC", param->srqc_base, cap->srq_entry_sz,
                      byType[RES_SRQ]->num, cap->reserved_srqs, false, false);
            ADD_TABLE("MCG", param->mc_base, mgmEntrySize, mcgObj, mcgObj,
                      false, false);
#undef ADD_TABLE
            /*
             * Report the count that was actually written, and say how many would
             * have been needed if it did not fit -- so the caller can distinguish
             * "no room" from "nothing to do" without guessing.
             */
            if (numTablesOut) *numTablesOut = k;
            if (k > capTables) {
                return 0;
            }
        }
    }

    return total;
}

#endif /* CX3_MLX4_PURE_H */
