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
 * Cx3Mlx4 -- IOKit glue for the mlx4 hardware layer.
 * Hardware programming derived from vendor/freebsd-src (OpenIB BSD).
 */
#include "Cx3Mlx4.h"
#include "Cx3EthernetPort.h"

#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include "Cx3Mlx4UserClient.h"

#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>
/*
 * clock_get_uptime, for the fault injector's "how long did I really hold the transaction
 * lock" measurement.  The same clock Mlx4Hw.cpp uses for the command deadlines, so the
 * number a held-lock run reports and the number a command deadline reports are comparable.
 */
#include <kern/clock.h>

#include "Cx3Debug.h"   /* LOG / LOGBOOT / boot-arg gates */

/* Device-level actions live in their own files; both rely on LOG above. */
#include "flr.inc"

/*
 * Stage 10a's interface end needs three things that the IOKit networking headers do not pull in
 * by themselves, and each of the three was a compile error rather than a runtime surprise,
 * which is the right order:
 *
 *   <sys/kpi_mbuf.h>                    mbuf_freem -- outputPacket receives an mbuf it owns
 *   <IOKit/network/IOOutputQueue.h>     kIOReturnOutputDropped, the code that says "dropped"
 *   <IOKit/network/IONetworkMedium.h>   the medium setLinkStatus takes as its SECOND argument
 */
#include <sys/kpi_mbuf.h>
#include <IOKit/network/IOOutputQueue.h>
#include <IOKit/network/IONetworkMedium.h>

#ifndef CX3_BUILD_TAG
#define CX3_BUILD_TAG "dev"
#endif
#ifndef CX3_BUNDLE_VERSION
#define CX3_BUNDLE_VERSION "unversioned-local"
#endif

/* Copy an entire build string only when it is bounded and identifies a candidate build. */
static bool cx3CopyBuildIdentity(char *dst, UInt32 capacity, const char *src)
{
    UInt32 n = 0;
    if (!dst || !src || capacity == 0) return false;
    while (n < capacity && src[n] != '\0') ++n;
    if (n == 0 || n >= capacity) return false;
    if ((n == 3 && src[0] == 'd' && src[1] == 'e' && src[2] == 'v') ||
        (n >= 6 && src[0] == 'u' && src[1] == 'n' && src[2] == 'k' &&
         src[3] == 'n' && src[4] == 'o' && src[5] == 'w') ||
        (n >= 11 && src[0] == 'u' && src[1] == 'n' && src[2] == 'v' &&
         src[3] == 'e' && src[4] == 'r' && src[5] == 's' && src[6] == 'i' &&
         src[7] == 'o' && src[8] == 'n' && src[9] == 'e' && src[10] == 'd'))
        return false;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return true;
}

/*
 * The metaclass superclass, C++ base class, AND the header's super alias must agree.
 * The alias stayed IOService after stage 10a and skipped networking init/start/stop/free.
 * In particular IONetworkController::start creates the private command gate that attach
 * uses before any driver hook. A private driver work loop cannot replace that gate.
 * The header now asserts the alias so this mismatch fails at compile time.
 */
OSDefineMetaClassAndStructors(Cx3Mlx4, IOEthernetController)

/*
 * getWorkLoop -- the family's gate runs on OUR workloop (see the declaration in the header).
 *
 * Created on demand, because the family may ask for it before this driver's interrupt stage has
 * ever run -- and it is deliberately NOT released when the interrupt session comes down: the
 * family holds this object for the life of the controller, so a workloop freed at m3irqdown
 * would be one the family is still scheduling on.  The interrupt session's own state is
 * fIrqObjectsUp / fIrqSource, which is what the "is anything still up" gates ask about.
 */
IOWorkLoop *Cx3Mlx4::getWorkLoop() const
{
    Cx3Mlx4 *me = const_cast<Cx3Mlx4 *>(this);

    if (!me->fWorkLoop) {
        me->fWorkLoop = IOWorkLoop::workLoop();
        if (me->fWorkLoop) {
            LOG("getWorkLoop: created the workloop the family will schedule on (this is the one "
                "m3irqup uses for the interrupt event source, so there is one loop and not two)");
        }
    }
    return me->fWorkLoop;
}

/*
 * createWorkLoop -- the other half of the same contract, and it was missing.
 *
 * The family calls this before its first getWorkLoop() and fails start() if it returns false
 * (IONetworkController.h:1137-1147).  Overriding only getWorkLoop() leaves this half to the
 * base class, and the base's half builds a loop of its own -- so "one loop and not two" was an
 * assertion about getWorkLoop() while the create path was free to make a second one.  The
 * working driver on this machine overrides both and hands out the same self-owned loop from
 * each (LucyRTL8125Ethernet.cpp:685/:698).
 *
 * Idempotent, and deliberately the same statement as getWorkLoop(): whichever of the two is
 * asked first, there is one IOWorkLoop behind both.
 */
bool Cx3Mlx4::createWorkLoop()
{
    if (!fWorkLoop) {
        fWorkLoop = IOWorkLoop::workLoop();
        if (fWorkLoop) {
            LOG("createWorkLoop: created the one workloop this controller owns (the family's "
                "createWorkLoop/getWorkLoop pair now both answer with this object, so there is "
                "no second loop for its open/close machinery to run on)");
        }
    }
    return fWorkLoop != 0;
}

bool Cx3Mlx4::init(OSDictionary *dictionary)
{
    /* free() must also tolerate a failed superclass init. */
    fNativePorts[0]=fNativePorts[1]=nullptr;
    fXactLock = 0;
    fAdmissionLock = 0;
    fStopRetained = false;
    fStopProvider = 0;
    if (!super::init(dictionary)) return false;
    /*
     * Every member, and this list is a fix rather than tidiness.
     *
     * It used to initialize six of roughly fifty.  The other forty-odd -- fEqRing, fEqLock,
     * fNumTables, fMrState, the R4 queue pointers, this batch's new fXactLock and the rest --
     * were left whatever the allocator handed back, and the code tests several of them
     * (if (!fEqRing), if (fNumTables == 0)) before ever assigning to them.
     *
     * Section 28.2 was this same defect in the hardware layer: a constructor that listed
     * some members and not the others, where a missing one was later used as a pointer.  The
     * lesson recorded there applies verbatim -- the object comes from an allocator that does
     * not promise zeroed memory, so "it has always worked" is evidence about the allocator
     * so far, not about the code.  A kext reload that recycles the same buffer is exactly
     * when that stops being true.
     *
     * Written longhand, one per line, in header declaration order, so that a member added
     * later stands out by its absence rather than being silently covered.
     */
    fPci = 0;
    fStartPciIdentity.clear();
    fDeviceGeneration = 0;
    memset(&fAutoNetwork, 0, sizeof(fAutoNetwork));
    fAutoNetwork.version = CX3_AUTO_NETWORK_VERSION;
    fAutoNetwork.structBytes = sizeof(fAutoNetwork);
    fAutoNetwork.interfaceUnit = 0xffffffffu;
    fAutoNetwork.port = 1;
    fAutoNetworkUserDisabled = false;
    fStartPciIdentityValid = false;
    fPciOpen = false;
    fPciCommandAfterEnable = 0;
    fBarMapFlags = 0;
    fFwIcm = 0;
    fQueriedFwPages = 0;
    fFwRunAttempted = false;
    fNumTables = 0;
    memset(fTables, 0, sizeof(fTables));
    fIcmSize = 0;
    fAuxUnits = 0;
    fAuxIcm = 0;
    fIcmMapped = false;
    fIcmHcaHolds = false;
    fLostIcmPages = 0;
    fHcaInited = false;
    fFuncMaxEq = 0;
    fFuncReservedEq = 0;
    fFuncReservedUar = 0;
    memset(&fProfile, 0, sizeof(fProfile));   /* a struct, not a pointer */
    /* the event queue */
    fEqRing = 0;
    fEqUnknownRetained = false;
    fEqHwReturned = false;
    fEqMayBeDeviceOwned = false;
    fEqNent = 0;
    fEqn = 0;
    fEqMTTOffset = 0;
    fEqMttPages = 0;
    fEqDoorbell = 0;
    fEqConsIndex = 0;
    fEqeFactor = 0;
    fEqLock = 0;
    /* stage 10a: the interface is attached on demand, so it starts un-attached and the address
     * invalid -- getHardwareAddress has to be able to tell "not read yet" from "zeros". */
    fNetIface = 0;
    fNetMacValid = false;
    fNetUnit = 0;
    fNetRxPrimed = false;
    fNetOutputDropped = 0;
    fNetEnabled = false;
    fNetTxAdmission.init();
    fPorts[0].linkUp = false;
    fPorts[0].speedMbps = 0;
    /*
     * Both start at 0, which means "not known yet" rather than any size: getMaxPacketSize reads
     * them that way (an unknown bound is skipped, not treated as zero).  fPorts[0].frameMtu becomes
     * 1522 when m3portup programs SET_PORT -- the wire size for a 1500 MTU -- and fPorts[0].maxPayloadMtu
     * becomes 9900 when QUERY_PORT is read, because that field is the port's capability.
     */
    fPorts[0].frameMtu = 0;
    fPorts[0].maxPayloadMtu = 0;
    /* No port address until a port-up step reads one; see the header for why this is held. */
    for (UInt32 i = 0; i < 2; ++i) fPorts[i].init(i + 1, 0);
    fNetupStep = 0;
    fNetAttachCall = 0;
    fNetAttachLock = 0;
    fNetAttachInFlight = false;
    fNetAttachDone = false;
    fNetAttachSuccess = false;
    fNetAttachIface = 0;
    fNetAttachRetainCount = 0;
    fNetAttachPoisoned = false;
    fNetAttachWorker = kCx3AttachWorkerIdle;
    fNetDetachCall = 0;
    fNetDetachInFlight = false;
    fNetDetachDone = false;
    fNetDetachCallFreeFailed = false;
    fNetDetachIfaceCount = 0;
    memset(fNetDetachIfaces, 0, sizeof(fNetDetachIfaces));
    memset(fNetDetachRefCounts, 0, sizeof(fNetDetachRefCounts));
    /* the data path (10b): no timer until netup asks for one, and counters at zero */
    fNetTimer = 0;
    fNetTicks = 0;
    fNetTicksSkipped = 0;
    fNetRxFrames = 0;
    fNetRxDropped = 0;
    fNetRxBytes = 0;
    fNetRxRefills = 0;
    fNetRxNotOurs = 0;
    fNetRxNotDelivered = 0;
    fNetRxNoRings = 0;
    fNetRxErrors = 0;
    fNetInTick = false;
    fMediaDict = 0;
    fMediumAuto = 0;
    fMediaPublished = false;
    fNetRxCsum = false;
    fNetRxCsumGood = 0;
    fNetRxCsumNone = 0;
    fNetTxCsum = false;
    fNetTxCsumGood = 0;
    fNetTxCsumNone = 0;
    fNetMcastTable = false;
    fNetMcastStored = 0;
    fNetMcastPrograms = 0;
    fNetMcastLastFw = 0;
    fNetMcastTruncated = 0;
    fNetPromiscProgrammed = false;
    fNetPromiscFw = 0;
    fPorts[0].closeConfirmed = false;
    fPorts[0].closeFwStatus = 0;
    memset(fNetMcastList, 0, sizeof(fNetMcastList));
    fNetTxRing = 0;
    fNetTxBufBytes = (UInt32)kNetTxBufBytes;
    fNetMtu = 1500u;
    fNetTxLock = 0;
    fNetTxLockBusy = 0;
    fNetMcastMode = false;
    fNetPromiscMode = false;
    fNetMcastListCount = 0;
    fNetMcastListLogs = 0;
    fNetTxSlotCount = 0;
    fNetTxFrames = 0;
    fNetTxCompleted = 0;
    fNetTxDropped = 0;
    fNetTxStalled = 0;
    fNetTxErrors = 0;
    fNetTxFlushErrors = 0;
    memset(&fNetTxFault, 0, sizeof(fNetTxFault));
    fNetTxInFlightAtStop = 0;
    memset(fNetTxMbuf, 0, sizeof(fNetTxMbuf));
    memset(fNetTxOwner, 0, sizeof(fNetTxOwner));
    memset(fNetMac, 0, sizeof(fNetMac));
    /* the interrupt session */
    fWorkLoop = 0;
    fIrqSource = 0;
    fIrqSourceIndex = 0;
    fIrqEnabled = false;
    fIrqObjectsUp = false;
    fIrqPending = false;
    fIrqWantToken = 0;
    fIrqMatched = false;
    fIrqMatchedToken = 0;
    fIrqMatchedStatus = 0;
    fIrqLastType = 0;
    fIrqCount = 0;
    fIrqCallbackCount = 0;
    fIrqEqeCount = 0;
    fPollEqeCount = 0;
    fIrqSkipped = 0;
    fIrqErrors = 0;
    fIrqZeroStreak = 0;
    fIrqStormTripped = false;
    fClrIntWrites = 0;
    fClrIntLegacy = false;
    fClrIntReady = false;
    /* R4: the memory region, the completion queue, the receive ring */
    fMrState = 0;
    fMrPdn = 0;
    fMrMptIndex = 0;
    fMrLkey = 0;
    fMrGeneration.init(0);
    fQueueNamespace.init();
    for (UInt32 i = 0; i < 2; ++i) fPortQueues[i].init(i + 1);
    /* the freeze investigation's instruments: the quarantine and the silence readout */
    memset(fQuar, 0, sizeof(fQuar));
    memset(fQuarOverflow, 0, sizeof(fQuarOverflow));
    memset(fQuarOverflowGeneration, 0, sizeof(fQuarOverflowGeneration));
    fQuarCount = 0;
    fQuarBytes = 0;
    fQuarRefused = 0;
    fQuarOverflowCount = 0;
    fQuarOverflowUntracked = 0;
    fQuarExhausted = false;
    fResetUnproven = false;
    fResetCommandsBlocked = false;
    fQuarSweeps = 0;
    fQuarHits = 0;
    fGate.init();
    memset(fIcmKept, 0, sizeof(fIcmKept));
    fIcmKeptCount = 0;
    fIcmKeptOverflow = 0;
    fResetSilenceRuns = 0;
    fResetSilenceChanges = 0;
    fResetPreTxPosted = 0;
    fResetPreRxPosted = 0;
    fResetPreQpsUp = 0;
    fResetPreOwner = 0;
    /* Before registerService/user clients, not lazily in concurrent entry paths.
     * This allocates no device resource and issues no hardware command. */
    fXactLock = IOLockAlloc();
    fAdmissionLock = IOLockAlloc();
    if (!fXactLock || !fAdmissionLock) return false;
    return true;
}

void Cx3Mlx4::free()
{
    /* stop closes admission, but rejected late calls can still use its lock.
     * Keep both locks until final object destruction, not just successful stop.
     * Object/provider survival on an incomplete stop remains a separate contract. */
    if (fAdmissionLock) { IOLockFree(fAdmissionLock); fAdmissionLock = 0; }
    if (fXactLock) { IOLockFree(fXactLock); fXactLock = 0; }
    super::free();
}

IOService *Cx3Mlx4::probe(IOService *provider, SInt32 *score)
{
    /* The score itself comes from IOProbeScore in Info.plist; refusing
     * non-PCI providers here keeps the kext from attaching anywhere else. */
    if (!OSDynamicCast(IOPCIDevice, provider)) return 0;
    return this;
}

void Cx3Mlx4::readOnlyProbe()
{
    UInt32 owner, hcr, sem;

    /* Evidence first: capture the read artifact before anything else touches
     * BAR0, so a boot log shows whether it is still there. */
    fHw.diagnoseFirstRead();

    owner = fHw.readOwnerWord();
    hcr   = fHw.readHcrStatus();
    sem   = fHw.readResetSem();

    fHw.dump("HCR", MLX4_HCR_BASE, 8);
    fHw.dump("RESET", MLX4_RESET_BASE + 0x3f0, 4);

    LOG("BAR0 mapping physical address = 0x%llx (expected 0xdc200000)",
        (unsigned long long)fHw.bar0Physical());

    LOG("M1a VERDICT: bound=YES bar0=%s bar2=%s window_alive=%s owner=0x%08x "
        "hcr_go=%u hcr_t=%u resetsem=0x%08x",
        fHw.bar0Mapped() ? "YES" : "NO",
        fHw.bar2Mapped() ? "YES" : "NO",
        fHw.windowAlive() ? "YES" : "NO",
        (unsigned)owner,
        (unsigned)((hcr >> HCR_GO_BIT) & 1u),
        (unsigned)((hcr >> HCR_T_BIT) & 1u),
        (unsigned)sem);
}

/* ------------------------------------------------------------------ */
/* user-space exports.  None of this runs during boot.                */
/* ------------------------------------------------------------------ */

UInt32 Cx3Mlx4::savedPciCommand() const
{
    return fPciCommandAfterEnable;
}

void Cx3Mlx4::exportProbe(UInt64 *out)
{
    UInt32 owner = fHw.readOwnerWord();
    UInt32 hcr   = fHw.readHcrStatus();
    UInt32 sem   = fHw.readResetSem();

    out[kProbeOutStatus]   = 0;
    out[kProbeOutWindow]   = fHw.windowAlive() ? 1 : 0;
    out[kProbeOutOwner]    = owner;
    out[kProbeOutHcrStat]  = hcr;
    out[kProbeOutResetSem] = sem;
    out[kProbeOutPciCmd]   = savedPciCommand();
    out[kProbeOutBar0Phys] = fHw.bar0Physical();
    out[kProbeOutMapFlags] = fBarMapFlags;
    /*
     * The poison bit, so that "why did that command fail at once?" is answered by the same
     * call that answers "is the window alive?" -- probe is what the recovery procedure
     * starts with, and a reader who has to run two selectors to learn one thing will
     * eventually run one.
     */
    out[kProbeOutCmdWedged] = fHw.cmdWedged() ? 1 : 0;

    LOG("probe: window=%u owner=0x%08x hcr=0x%08x sem=0x%08x pci_cmd=0x%04x "
        "bar0=0x%llx cmd_wedged=%u",
        (unsigned)out[kProbeOutWindow], (unsigned)owner, (unsigned)hcr,
        (unsigned)sem, (unsigned)savedPciCommand(),
        (unsigned long long)out[kProbeOutBar0Phys],
        (unsigned)out[kProbeOutCmdWedged]);
}

IOReturn Cx3Mlx4::exportDma(UInt64 *out)
{
    bool ok = fHw.allocMailbox();

    out[kDmaOutStatus]   = ok ? 0 : 1;
    out[kDmaOutPhys]     = fHw.mboxPhys();
    out[kDmaOutSegCount] = fHw.mboxSegCount();
    out[kDmaOutLow4G]    = (fHw.mboxPhys() != 0 && fHw.mboxPhys() < 0x100000000ull) ? 1 : 0;
    out[kDmaOutOffset]   = fHw.mboxOffset();

    LOG("dma step: ok=%u phys=0x%llx segs=%u low4g=%u offset=%llu "
        "(no device registers touched)",
        (unsigned)ok, (unsigned long long)fHw.mboxPhys(),
        (unsigned)fHw.mboxSegCount(), (unsigned)out[kDmaOutLow4G],
        (unsigned long long)fHw.mboxOffset());
    return kIOReturnSuccess;
}

IOReturn Cx3Mlx4::exportQueryFw(bool force, UInt64 *out)
{
    Mlx4QueryFwResult fw;
    /*
     * Read the ownership word the way the reference reads it (once), and also
     * take the settled value, so the log shows both.
     *
     * At this address the first read returns 0 and later reads return
     * 0x01000000, identically on macOS and Linux.  The reference's gate passes
     * on the first read; reading the settled value would fail it.  Since Linux
     * drives this card happily, the gate is not a real signal here, and matching
     * the reference's access pattern is the honest fix rather than special-casing
     * a threshold.
     */
    UInt32 ownerRef = fHw.checkOwnershipAsReference();
    UInt32 owner    = fHw.readOwnerWord();
    int    i;

    LOG("ownership word: fresh-mapping single read (the reference's own method) "
        "= 0x%08x, settled value through the persistent mapping = 0x%08x",
        (unsigned)ownerRef, (unsigned)owner);

    /*
     * The gate decides on the reference's reading, because the reference is what
     * we are porting and it is what is known to work: Linux drives this card with
     * this exact value present.  On this hardware that reading is 0 -- the first
     * read through a freshly created mapping -- so the gate passes, as it does
     * for the reference.
     */
    owner = ownerRef;

    memset(out, 0, sizeof(UInt64) * kFwOutCount);

    /*
     * FreeBSD mlx4_load_one() (mlx4_main.c:3244) refuses to touch a device whose
     * ownership word is already set:
     *     err = mlx4_get_ownership(dev);
     *     if (err) { ... "Multiple PFs not yet supported - Skipping PF"; }
     * That gate was dropped when this port was first slimmed down, and the first
     * command attempt then hung the machine.  Restored, and reported: if it
     * trips, the ownership word is part of the reason the card is not in the
     * state the reference driver expects.
     */
    if (!force && owner != 0) {
        LOG("QUERY_FW refused: ownership word is 0x%08x (the reference driver "
            "would skip this device).  Pass force=1 to override deliberately.",
            (unsigned)owner);
        out[kFwOutStatus]   = (UInt64)kIOReturnNotPermitted;
        out[kFwOutFwStatus] = 0xffffffffull;   /* "not attempted" marker */
        return kIOReturnSuccess;
    }

    if (!fHw.windowAlive()) {
        LOG("QUERY_FW refused: BAR0 window is not reading back live");
        out[kFwOutStatus]   = (UInt64)kIOReturnOffline;
        out[kFwOutFwStatus] = 0xffffffffull;
        return kIOReturnSuccess;
    }

    LOG("QUERY_FW starting: force=%u owner=0x%08x hcr=0x%08x (first device "
        "access since boot)",
        (unsigned)force, (unsigned)owner, (unsigned)fHw.readHcrStatus());

    if (!fHw.queryFw(&fw)) {
        LOG("QUERY_FW failed: fw_status=0x%x hcr_after=0x%08x",
            (unsigned)fw.fwStatus, (unsigned)fHw.readHcrStatus());
        out[kFwOutStatus]   = (UInt64)kIOReturnError;
        out[kFwOutFwStatus] = fw.fwStatus;
        return kIOReturnSuccess;
    }

    for (i = 0; i < 4; ++i) out[kFwOutRaw0 + i] = fw.rawWords[i];
    out[kFwOutFwPages]   = fw.fwPages;
    fQueriedFwPages      = fw.fwPages;   /* fwload checks against this */

    out[kFwOutStatus]    = 0;
    out[kFwOutFwStatus]  = fw.fwStatus;
    out[kFwOutFwVer]     = fw.fwVer;
    out[kFwOutCmdIfRev]  = fw.cmdIfRev;
    out[kFwOutMaxCmds]   = fw.maxCmds;
    out[kFwOutPpfId]     = fw.functionId;
    out[kFwOutCatasOff]  = fw.catasOffset;
    out[kFwOutCatasSize] = fw.catasSize;
    out[kFwOutCatasBar]  = fw.catasBar;
    out[kFwOutFwPages]   = fw.fwPages;

    LOG("M1b VERDICT: query_fw=OK fw_ver=%u.%u.%03u cmd_if_rev=%u max_cmds=%u "
        "ppf_id=%u",
        (unsigned)(fw.fwVer >> 32),
        (unsigned)((fw.fwVer >> 16) & 0xffff),
        (unsigned)(fw.fwVer & 0xffff),
        (unsigned)fw.cmdIfRev, (unsigned)fw.maxCmds, (unsigned)fw.functionId);
    return kIOReturnSuccess;
}

IOReturn Cx3Mlx4::exportResetHca(bool bypassSem, UInt64 *out)
{
    memset(out, 0, sizeof(UInt64) * kRstOutCount);
    if (fResetUnproven || fResetCommandsBlocked) {
        out[kRstOutStatus] = (UInt64)kIOReturnNoSpace;
        out[kRstOutOwnerBefore] = out[kRstOutOwnerAfter] = 0xffffffffu;
        return kIOReturnSuccess;
    }
    UInt32 before = fHw.readOwnerWord();

    out[kRstOutOwnerBefore] = before;
    out[kRstOutOwnerAfter]  = before;
    /*
     * Whether the mailbox was poisoned when this reset arrived.
     *
     * Reported for every outcome, including the refusals below, because the answer is about
     * the state *before* the reset and a refusal leaves that state unchanged.  This is the
     * pre-reset poison diagnostic. Reset does not revoke poison: recovery still
     * needs a generation/scope credential before the old mailbox can be reused.
     */
    out[kRstOutCmdWedged] = fHw.cmdWedged() ? 1 : 0;
    LOG("HCA reset requested from user space; ownership word before = 0x%08x, command "
        "mailbox %s",
        (unsigned)before, fHw.cmdWedged() ? "POISONED (reset alone will not clear it)" :
                                            "not poisoned");

    /*
     * An open interrupt session is refused here, not torn down here.
     *
     * Measured, not theoretical: an enabled legacy level source with no clear at
     * the device storms, and it has already wedged this machine once.  A reset
     * throws away the register state the clear path was built on, so the session
     * has to be closed deliberately with "m3irqdown" while the device is still
     * coherent enough to disable the source safely.  Doing it inside the reset
     * path would mean releasing interrupt objects at a moment when the device has
     * already been written to.
     */
    /* fWorkLoop is not session state any more: the family holds the workloop for the life of
     * the controller (getWorkLoop), so a live fWorkLoop no longer means an interrupt session is
     * open.  The session's own state is fIrqObjectsUp and fIrqSource. */
    if (fIrqObjectsUp || fIrqSource) {
        out[kRstOutStatus] = (UInt64)kIOReturnNotReady;
        LOG("HCA reset refused: an interrupt session is still open (objects %u, "
            "source %u, workloop %u, delivery %s). Run 'm3irqdown' first: the source "
            "has to be disabled and released while the device is still coherent, and "
            "resetting with an uncleared legacy source is what wedged this machine.",
            (unsigned)fIrqObjectsUp, fIrqSource ? 1u : 0u, fWorkLoop ? 1u : 0u,
            fIrqEnabled ? "enabled" : "disabled");
        return kIOReturnSuccess;
    }

    if (!resetRetentionPreflight()) {
        out[kRstOutStatus] = (UInt64)kIOReturnNoSpace;
        LOG("HCA reset refused before command: retention headroom or local lifecycle "
            "preconditions are not established; no reset/teardown was attempted");
        return kIOReturnSuccess;
    }
    if (fDeviceGeneration == ~0ull) {
        out[kRstOutStatus] = (UInt64)kIOReturnNoSpace;
        LOG("HCA reset refused: provider generation counter exhausted; no device access attempted");
        return kIOReturnSuccess;
    }
    /* The bool reset API cannot prove a failed attempt was never posted. Until
     * R06 supplies an epoch-bound proof, no repeated reset in this instance. */
    const bool freshStart = resetFreshStartEligible();
    ++fDeviceGeneration;
    fHw.setDeviceGeneration(fDeviceGeneration);
    for (UInt32 i = 0; i < 2; ++i) fPorts[i].newGeneration(fDeviceGeneration);
    fResetUnproven = true;
    fResetCommandsBlocked = true;
    if (!fHw.resetHca(bypassSem)) {
        out[kRstOutStatus] = (UInt64)kIOReturnError;
        out[kRstOutOwnerAfter] = 0xffffffffu;
        LOG("HCA reset failed; device access blocked, no post-failure MMIO read attempted");
        return kIOReturnSuccess;
    }

    out[kRstOutStatus] = 0;
    out[kRstOutOwnerAfter] = fHw.readOwnerWord();

    /*
     * Ask the one question the teardowns below cannot answer for themselves, and ask it
     * BEFORE anything is released: is the device still writing into host memory?
     *
     * This runs whether or not the quarantine is on, because it is the reading that decides
     * how the quarantine's own result has to be read: a quiet window with queues live is
     * evidence, and a quiet window with nothing in flight is not.  It is read-only, it is
     * bounded (a few tens of milliseconds), and it changes no behaviour.
     */
    resetSilenceCheck();

    /*
     * The HCA forgot every queue it had, so the host side has to forget them too.
     * Without this, "reset then try again" is not a working sequence: the
     * "an EQ is already up" flag survives the reset and every later attempt
     * refuses at the entrance.  That is what happened, and it turned a four-way
     * intr sweep into four identical refusals, which is worse than a failure --
     * it looks like data.
     */
    teardownEq("HCA was reset", true);

    /*
     * Everything the HCA was told about itself is unknown again.
     *
     * Measured defect, found by trying to use the documented recovery: this flag
     * was not cleared, so "reset force" followed by a re-run of the sequence
     * refused at m2dbringup with "the HCA is already initialised this boot" --
     * which is the guard's own advice failing to work.  The guard exists because a
     * second INIT_HCA is rejected by the firmware with CMD_STAT_BAD_SYS_STATE, so
     * the flag and the device have to agree; after a reset the device says
     * "uninitialised" and the flag has to say the same.
     *
     * The ICM mappings and the context tables the HCA held are gone with the
     * reset, so the next bring-up has to be a full one: m2dprofile then m2dbringup.
     */
    fHcaInited = false;
    fIcmMapped = false;
    /*
     * ...and with them the firmware's side of every ICM mapping, which is not the
     * same thing as the host's.
     *
     * The chunks fTables[] and fAuxIcm still name are host memory this provider has
     * not released; the reset restarted the firmware (QUERY_FW, MAP_FA, RUN_FW come
     * next), and a firmware instance that has just been started has been given no
     * ICM mapping at all.  So the next teardown must not send UNMAP_ICM for them:
     * measured 2026-10-12, that is exactly the case in which all fourteen answer
     * fw status 0x1 (CMD_STAT_INTERNAL_ERR) -- the range is not in the mapping
     * structure the new firmware built, because it has not built one.  The one
     * teardown that ran *without* a reset in between (2026-10-11 20:22:02) answered
     * 0x0 to all fourteen.  docs/ICM-UNMAP.md carries the log lines and the
     * inference this rests on.
     */
    fIcmHcaHolds = false;
    /* The reset HCA has forgotten the MPT too, so its host-side record goes with it. */
    /*
     * The QP goes back first, and it is the only one of these four that is not optional:
     * its context names the ring's MTT entries and the producer record, so everything after
     * it is memory it would otherwise still point at.  After a reset there is nothing to
     * send -- the device has forgotten the QP -- but the host state and the record still
     * have to go in this order.
     */
    /*
     * The indirection QP before the RX QP: it names the RX QP only by NUMBER (it steers into
     * it), so there is no memory relation to order -- but the number is about to stop being
     * meaningful, and the log then reads in the order the objects were built.  The MAC
     * registration goes with it: the reference has no "unregister" command (its removal path
     * re-sends the whole table without the entry, mlx4_port.c:415-430), and after a reset the
     * firmware has forgotten the table anyway, so the host-side record is simply dropped and
     * the next bring-up re-registers it.
     */
    ingressTeardown("HCA was reset", true);
    ingressTeardown("HCA was reset", true, 2);
    /*
     * The transmit side, which is the same kind of object one stage later: its QP's context
     * names the send queue's MTT entries and its own db_rec_addr record, and after a reset the
     * firmware has forgotten both.  Placed with the other QPs rather than with the CQs so that
     * the log reads in the order the objects were built.
     */
    txTeardown("HCA was reset", true);
    txTeardown("HCA was reset", true, 2);
    qpTeardown("HCA was reset", true);
    qpTeardown("HCA was reset", true, 2);
    cqTeardown("HCA was reset", true);
    cqTeardown("HCA was reset", true, 2);
    rqTeardown("HCA was reset", true);
    rqTeardown("HCA was reset", true, 2);
    mrTeardown("HCA was reset", true);
    /*
     * The interrupt-clear readiness does not survive either, even though the clear
     * register's address and mask are firmware constants that come back the same:
     * they come from QUERY_FW and QUERY_ADAPTER answers given before the reset, and
     * a legacy source must not be enabled on the strength of a pre-reset answer.
     * m3clrint re-reads both during the next bring-up, and m3irqup already refuses
     * a legacy source without it.
     */
    fClrIntReady = false;
    fClrIntLegacy = false;
    fHw.clrIntTearDown();

    /* Only the first empty-instance startup may continue the existing init
     * chain. A successful reset bool cannot return old device owners. */
    if (freshStart) fResetCommandsBlocked = false;

    LOG("HCA reset done; ownership word after = 0x%08x; INIT_HCA state, the EQ and "
        "the interrupt-clear readiness are invalidated -- the next bring-up starts "
        "at 'm2dprofile'",
        (unsigned)out[kRstOutOwnerAfter]);
    /*
     * One line that says what the teardown above did with the device-facing memory, so a
     * reader does not have to infer it from the absence of "freeing" lines.  Those blocks are
     * held (quarantined) unless cx3quarfree=1 was passed; the individual decisions are in the
     * "quarantine:" and "kept, not freed" lines above.
     */
    LOG("reset done: %u device-facing block(s) / %llu KiB are now HELD, read-only, and "
        "watched; %u block(s) were retained outside the sweeper's zone (%u untracked); "
        "%u reset(s) so far saw host memory change after the reset. cx3quarfree no longer "
        "overrides this: a debug switch may not free memory the device may still own.",
        (unsigned)fQuarCount, (unsigned long long)(fQuarBytes >> 10),
        (unsigned)fQuarRefused, (unsigned)fQuarOverflowUntracked,
        (unsigned)fResetSilenceChanges);
    return kIOReturnSuccess;
}

/* Reset-only headroom check, not a global reservation or a quiescence proof.
 * The selector transaction serializes the owners inspected here. Any network
 * lifecycle is refused until its separate workloop/power protocol is proved.
 * No retained allocation may be silently carried into a second reset attempt. */
bool Cx3Mlx4::resetRetentionPreflight()
{
    /* A/B EQ ring owners are kept in Mlx4Hw rather than the controller's
     * reset-quarantine inventory. Reset advances the provider generation before
     * issuing the command, which would make their only HW2SW retry stale and
     * permanently pin the MTT range. Until reset-scope release credentials
     * cover these owners, refuse before changing generation or touching HCR. */
    if (fHw.hasAbRetainedRings()) return false;
    if (fResetCommandsBlocked || fResetUnproven || fQuarExhausted || fQuarCount || fQuarBytes ||
        fQuarOverflowCount || fQuarOverflowUntracked || fIcmKeptCount || fIcmKeptOverflow)
        return false;
    if (fNetAttachLock) {
        IOLockLock(fNetAttachLock);
        bool busy = fNetAttachInFlight || fNetAttachPoisoned || fNetAttachIface != 0;
        IOLockUnlock(fNetAttachLock);
        if (busy) return false;
    } else if (fNetAttachCall || fNetAttachIface) {
        return false;
    }
    if (fNetTimer || fNetIface || fNetTxRing || fNetEnabled) return false;
    if (fNumTables > (UInt32)kMaxIcmTables) return false;

    Mlx4Icm *small[] = { fEqRing, fFwIcm,
        fPortQueues[0].qpRec, fPortQueues[0].cqRing, fPortQueues[0].cqRec, fPortQueues[0].rqRing, fPortQueues[0].rqBufs, fPortQueues[0].ingressRec, fPortQueues[0].txQpRec, fPortQueues[0].txSq, fPortQueues[0].rawFrame, fPortQueues[0].txCqRing, fPortQueues[0].txCqRec,
        fPortQueues[1].qpRec, fPortQueues[1].cqRing, fPortQueues[1].cqRec, fPortQueues[1].rqRing, fPortQueues[1].rqBufs, fPortQueues[1].ingressRec, fPortQueues[1].txQpRec, fPortQueues[1].txSq, fPortQueues[1].rawFrame, fPortQueues[1].txCqRing, fPortQueues[1].txCqRec };
    Mlx4Icm *seen[sizeof(small) / sizeof(small[0]) +
                  kMaxIcmTables * kMaxIcmChunksPerTable + 1];
    UInt32 seenCount = 0, smallCount = 0, icmCount = 0;
    UInt64 smallBytes = 0;
    for (UInt32 i = 0; i < (UInt32)(sizeof(small) / sizeof(small[0])); ++i) {
        Mlx4Icm *b = small[i];
        if (!b) continue;
        if (!b->virt || b->bytes < 4 || smallCount >= (UInt32)kQuarantineMax ||
            b->bytes > (UInt64)kQuarantineMaxBytes - smallBytes) return false;
        for (UInt32 j = 0; j < seenCount; ++j) if (seen[j] == b) return false;
        seen[seenCount++] = b;
        ++smallCount;
        smallBytes += b->bytes;
    }
    /* Auxiliary plus every physical chunk, not just the sampler's 64-entry
     * prefix. ICM byte policy is still a separate open task; this bounds slots. */
    const UInt32 chunks = (UInt32)(sizeof(fTables[0].icm) / sizeof(fTables[0].icm[0]));
    if (chunks > (UInt32)kMaxIcmChunksPerTable) return false;
    for (UInt32 i = 0; i <= fNumTables; ++i) {
        for (UInt32 c = 0; c < (i == fNumTables ? 1u : chunks); ++c) {
            Mlx4Icm *b = i == fNumTables ? fAuxIcm : fTables[i].icm[c];
            if (!b) continue;
            if (icmCount >= (UInt32)kIcmKeptMax ||
                seenCount >= (UInt32)(sizeof(seen) / sizeof(seen[0]))) return false;
            for (UInt32 j = 0; j < seenCount; ++j) if (seen[j] == b) return false;
            seen[seenCount++] = b;
            ++icmCount;
        }
    }
    LOG("reset preflight: %u small owners / %llu bytes and %u ICM owners fit local "
        "retention slots; not a DMA, global byte budget or recovery proof",
        (unsigned)smallCount, (unsigned long long)smallBytes, (unsigned)icmCount);
    return true;
}

bool Cx3Mlx4::resetFreshStartEligible() const
{
    /* Called only after resetRetentionPreflight, under the transaction lock.
     * No known device-facing owner may cross this initial-startup exception. */
    if (fHcaInited || fIcmMapped || fIcmHcaHolds || fClrIntReady ||
        fNumTables || fAuxIcm || fHw.cmdWedged() || fHw.hasMailboxAllocation())
        return false;
    for (UInt32 i = 0; i < (UInt32)kMaxIcmTables; ++i)
        for (UInt32 c = 0; c < (UInt32)kMaxIcmChunksPerTable; ++c)
            if (fTables[i].icm[c]) return false;
    for (UInt32 port = 0; port < 2; ++port)
        if (fPortQueues[port].hasOwners()) return false;
    return !fHw.hasMttAllocations() && !fEqRing && !fFwIcm;
}

bool Cx3Mlx4::resetAdmissionAllows(UInt32 selector) const
{
    if (hasNativePorts()) {
        switch(selector) {
            case kCx3SelRuntimeIdentity: case kCx3SelPortInfo: case kCx3SelPortQueue:
            case kCx3SelNativePort: case kCx3SelNativeConfig: case kCx3SelM3QueryPort: case kCx3SelM3PortUp:
            case kCx3SelQueryAdapter: case kCx3SelPcieLink: case kCx3SelModuleRead: break;
            default: return false;
        }
    }

    /* netstat reads host counters/interface metadata only. Even probe/peek
     * can touch a reset-sensitive BAR word; raw/force get no bypass. */
    return !fResetCommandsBlocked || selector == (UInt32)kCx3SelNetStat ||
           selector == (UInt32)kCx3SelRuntimeIdentity ||
           selector == (UInt32)kCx3SelAutoNetwork || selector == (UInt32)kCx3SelNativePort ||
           selector == (UInt32)kCx3SelNativeConfig; /* host status; actions guard themselves */
}

/*
 * ==================================================================== */
/* The freeze investigation's instruments (docs/HANG-AUDIT.md section 9) */
/* ==================================================================== */

/*
 * The two instruments live here because they are both about the reset path: the silence
 * readout runs inside exportResetHca() and the quarantine is what the teardowns it calls
 * put the device-facing memory into.  The design note -- why the reset path was the one
 * teardown resting on an assumption, and what each instrument reads -- is in Cx3Mlx4.h.
 */

/* How many set bits in a bitmap, for "how many receive slots were posted". */
static UInt32 cx3CountBits(const UInt8 *bits, UInt32 nbytes)
{
    UInt32 i, n = 0;
    if (!bits) return 0;
    for (i = 0; i < nbytes; ++i) {
        UInt8 b = bits[i];
        while (b) { n += (UInt32)(b & 1u); b = (UInt8)(b >> 1); }
    }
    return n;
}

/*
 * Release one device-facing block, or retain it in the quarantine.
 *
 * Single entry point on purpose: the places that used to free under "the HCA was reset, so it
 * has forgotten it" now all go through here, so the question "what did the reset path hand
 * back on the strength of the reset alone" has one answer in one place instead of a dozen
 * call sites that have to be kept in step.  deviceMayStillHold is what the caller knows and
 * this function does not: on the reset path it is true, and on the paths where the device
 * itself answered 2RST_QP or HW2SW_CQ it is false and the memory is freed exactly as before.
 *
 * Returns false when the block could not be taken into the retention ledger.  In that case
 * the caller still owns it and the pointer is left in place: refusing to release is the only
 * safe answer when nothing proves the device has stopped naming those pages.
 */
bool Cx3Mlx4::releaseIcmOrKeep(Mlx4Icm **slot, const char *tag, bool deviceMayStillHold)
{
    Mlx4Icm *icm;

    if (!slot) return true;
    icm = *slot;
    if (!icm) return true;

    if (deviceMayStillHold) {
        if (!quarantineTake(icm, tag)) {
            LOG("release (%s): RETAINED IN PLACE, not freed: the quarantine could not take "
                "ownership and nothing here proves the device stopped using these pages. "
                "The caller still holds the allocation; a reboot is what reclaims it.",
                tag ? tag : "unnamed block");
            return false;
        }
        *slot = 0;
        return true;
    }
    *slot = 0;
    fHw.icmFree(icm);
    return true;
}

/*
 * Take a device-facing block into the quarantine.
 *
 * The block is READ, never written.  The earlier version filled it with 0xDEADBEEF so that a
 * later read could tell "still ours" from "written since"; that test cost a write into memory
 * whose owner was exactly the thing in doubt.  The replacement is a read-only digest taken
 * now and re-read on every user-client entry: same bytes as at the take, or changed.
 *
 * The zone is bounded by entries and bytes.  When it is full the new block is NOT freed:
 * freeing would rest on the reset alone, which is the assumption this whole instrument
 * exists to avoid.  It goes to a second ledger that is never swept, so the retention stays
 * countable; if that is full too, transfer is refused and the caller keeps its slot.
 * Overflow/exhaustion is sticky: new selectors and network enable are refused.
 */
bool Cx3Mlx4::quarantineTake(Mlx4Icm *icm, const char *tag)
{
    QuarantineBlock *q;
    Mlx4IcmSum s;

    if (!icm) return true;
    if (!icm->virt || icm->bytes < 4) {
        /* An unreadable CPU view says nothing about the device's DMA mapping. */
        ++fQuarRefused;
        fQuarExhausted = true;
        fNetEnabled = false;
        LOG("quarantine: %s has no digestible CPU view; ownership transfer refused, "
            "allocation retained in the caller, not freed", tag ? tag : "unnamed block");
        return false;
    }

    if (fQuarCount >= (UInt32)kQuarantineMax ||
        fQuarBytes + icm->bytes > (UInt64)kQuarantineMaxBytes) {
        ++fQuarRefused;
        fQuarExhausted = true;
        fNetEnabled = false;
        if (fQuarOverflowCount < (UInt32)kQuarantineOverflowMax) {
            fQuarOverflow[fQuarOverflowCount] = icm;
            fQuarOverflowGeneration[fQuarOverflowCount] = icm->generation;
            ++fQuarOverflowCount;
        } else {
            LOG("quarantine: %s cannot fit either ledger; ownership transfer refused. "
                "The caller must keep its pointer and dependency metadata", tag ? tag : "unnamed block");
            return false;
        }
        LOG("quarantine: %s (%llu bytes) is NOT freed and NOT swept: the zone is full "
            "(%u block(s), %llu of %u KiB). It stays allocated in the overflow ledger "
            "(%u/%u tracked, %u untracked) until a proven quiesce or a reboot. Freeing here "
            "would rest on the reset alone.",
            tag ? tag : "unnamed block", (unsigned long long)icm->bytes,
            (unsigned)fQuarCount, (unsigned long long)(fQuarBytes >> 10),
            (unsigned)(kQuarantineMaxBytes >> 10),
            (unsigned)fQuarOverflowCount, (unsigned)kQuarantineOverflowMax,
            (unsigned)fQuarOverflowUntracked);
        return true;
    }

    q = &fQuar[fQuarCount];
    memset(q, 0, sizeof(*q));
    q->icm    = icm;
    q->tag    = tag ? tag : "unnamed block";
    q->busLow = icm->busLow;
    q->bytes  = (UInt32)icm->bytes;
    q->pages  = icm->npages;
    /* The preconditions were captured once, at the reset, before any teardown ran. */
    q->txPosted  = fResetPreTxPosted;
    q->rxPosted  = fResetPreRxPosted;
    q->qpsUp     = fResetPreQpsUp;
    q->ownerWord = fResetPreOwner;
    fHw.icmSum(icm, &s);
    q->baselineSum     = s.checksum;
    q->baselineNonzero = (UInt32)s.nonzero;
    q->lastSum         = q->baselineSum;
    q->lastNonzero     = q->baselineNonzero;
    q->generation = icm->generation;
    /* Only now is the entry visible to the sweep: the count is the commit. */
    ++fQuarCount;
    fQuarBytes += icm->bytes;

    LOG("quarantine: %s (%u bytes, %u page(s) from bus 0x%llx) is KEPT, read-only: baseline "
        "sum 0x%08x, %u non-zero byte(s). The reset is a firmware-level reset, not a PCIe "
        "FLR, so this is not released on the strength of it. Preconditions at the reset: %u "
        "send WQE(s) in flight, %u receive slot(s) posted, %u QP(s) live, ownership word "
        "0x%08x. Any change in the digest from here is a change this driver did not make.",
        q->tag, q->bytes, q->pages, q->busLow, (unsigned)q->baselineSum,
        (unsigned)q->baselineNonzero, q->txPosted, q->rxPosted, q->qpsUp, q->ownerWord);
    return true;
}

/*
 * Read every quarantined block back.
 *
 * Silent while everything is unchanged, and that is deliberate: this runs on every
 * user-client entry, including the ones that post frames.  A change is logged at once with
 * the entry it was seen at, which is the timestamp that makes it placeable in time; a
 * periodic summary says the zone is being watched at all, so silence can be told apart from
 * "the instrument was not running".  The sweep only reads.
 */
void Cx3Mlx4::freezeInstrumentsSweep(const char *where)
{
    UInt32 i;
    Mlx4IcmSum s;

    if (fQuarCount == 0) return;
    ++fQuarSweeps;

    for (i = 0; i < fQuarCount; ++i) {
        QuarantineBlock *q = &fQuar[i];

        if (!q->icm || !q->icm->virt) continue;
        fHw.icmSum(q->icm, &s);
        ++q->sweeps;
        q->lastSum     = s.checksum;
        q->lastNonzero = (UInt32)s.nonzero;

        if (s.checksum == q->baselineSum && q->lastNonzero == q->baselineNonzero) {
            /* Unchanged.  Said once per block, after enough sweeps to mean something. */
            if (!q->reported && q->sweeps >= 256) {
                q->reported = true;
                LOG("quarantine: %s (%u bytes, %u page(s) from bus 0x%llx) is unchanged after "
                    "%u read-back(s) -- no write has been observed since the take. A quiet "
                    "window is not a proof that the device cannot write; it is a reading.",
                    q->tag, q->bytes, q->pages, q->busLow, (unsigned)q->sweeps);
            }
            continue;
        }

        ++q->hits;
        ++fQuarHits;
        /*
         * Every hit is logged while they are rare (the first eight), then one in sixty-four:
         * a device writing continuously must not turn the log into the reason the machine is
         * unusable, and the count keeps the evidence.
         */
        if (q->hits <= 8 || (q->hits % 64) == 0) {
            LOG("quarantine: %s (%u bytes, %u page(s) from bus 0x%llx) CHANGED since the "
                "take: checksum 0x%08x -> 0x%08x, non-zero bytes %u -> %u. Seen at entry "
                "'%s', hit %u. The block is retained rather than handed back, so this is not "
                "a write into recycled pages -- but a digest change alone does not say who "
                "wrote, and it is not a conclusion about the historical freezes.",
                q->tag, q->bytes, q->pages, q->busLow, (unsigned)q->baselineSum,
                (unsigned)q->lastSum, (unsigned)q->baselineNonzero,
                (unsigned)q->lastNonzero, where ? where : "?", (unsigned)q->hits);
        }
    }

    if ((fQuarSweeps % 512) == 0) {
        LOG("quarantine: %u block(s) / %llu KiB held across %llu user-client entr(ies), %u "
            "change(s), %u block(s) retained outside the sweeper's zone (%u untracked)",
            (unsigned)fQuarCount, (unsigned long long)(fQuarBytes >> 10),
            (unsigned long long)fQuarSweeps, (unsigned)fQuarHits,
            (unsigned)fQuarOverflowCount, (unsigned)fQuarOverflowUntracked);
    }
}

/*
 * Hand every quarantined block back -- only with proof that the device has stopped.
 *
 * "The driver is stopping" used to be treated as that proof.  It is not: a firmware-level
 * reset is not a PCIe FLR, and closing the provider does not by itself demonstrate that the
 * device stopped naming host pages.  With deviceQuiesced=false this function releases
 * nothing, says how much stays allocated, and leaves the reclaim to a reboot.
 */
void Cx3Mlx4::quarantineDrain(const char *why, Cx3QuiesceProof proof)
{
    UInt32 i, n = 0;
    const char *proofName = (proof == kCx3QuiesceFlr)      ? "FLR"
                          : (proof == kCx3QuiesceStopping) ? "driver stopping"
                                                           : "none";

    if (proof != kCx3QuiesceFlr) {
        if (fQuarCount || fQuarOverflowCount || fQuarOverflowUntracked) {
            LOG("quarantine NOT drained (%s): the proof offered is '%s', which does not "
                "establish that the device stopped touching host memory; %u swept and %u "
                "overflow block(s) (%llu KiB in the zone) stay allocated; %u untracked "
                "allocation(s) are also retained. A reboot reclaims them.",
                why ? why : "?", proofName, (unsigned)fQuarCount, (unsigned)fQuarOverflowCount,
                (unsigned long long)(fQuarBytes >> 10), (unsigned)fQuarOverflowUntracked);
        }
        return;
    }

    for (i = 0; i < fQuarCount; ++i) {
        if (fQuar[i].icm) { fHw.icmFree(fQuar[i].icm); fQuar[i].icm = 0; ++n; }
    }
    for (i = 0; i < fQuarOverflowCount; ++i) {
        if (fQuarOverflow[i]) {
            fHw.icmFree(fQuarOverflow[i]); fQuarOverflow[i] = 0;
            fQuarOverflowGeneration[i].init(0);
            ++n;
        }
    }
    for (i = 0; i < fIcmKeptCount; ++i) {
        if (fIcmKept[i].icm) { fHw.icmFree(fIcmKept[i].icm); fIcmKept[i].icm = 0; ++n; }
    }
    if (fIcmKeptCount || fIcmKeptOverflow) {
        LOG("icm: released %u unmap-refused chunk(s) after an explicit quiesce proof; %u "
            "chunk(s) recorded as overflow stay allocated until reboot",
            (unsigned)fIcmKeptCount, (unsigned)fIcmKeptOverflow);
    }
    fIcmKeptCount = 0;
    if (n) {
        LOG("quarantine drained (%s): %u block(s) released after an explicit quiesce proof "
            "(%llu KiB held, %u change(s) over %llu sweep(s))",
            why ? why : "?", (unsigned)n, (unsigned long long)(fQuarBytes >> 10),
            (unsigned)fQuarHits, (unsigned long long)fQuarSweeps);
    }
    fQuarCount = 0;
    fQuarBytes = 0;
    fQuarOverflowCount = 0;
    memset(fQuarOverflowGeneration, 0, sizeof(fQuarOverflowGeneration));
}

/*
 * Every block of host memory the device can be told to reach, with the two readings that make
 * two samples comparable: an additive checksum over the dwords and the count of non-zero
 * bytes.
 *
 * Both, because they answer different questions.  The checksum catches any change at all; the
 * non-zero count is what catches the change that matters most here -- a block that was empty
 * and now has something in it, which is what "the device DMA'd into it" looks like.  A single
 * field could not tell those apart from a reader's point of view.
 *
 * The list is built fresh on every sample rather than cached, so it names what is live NOW:
 * the check runs before the teardowns, at which point the queues of a chain that just ran
 * still exist and are exactly what is at risk.
 */
UInt32 Cx3Mlx4::collectDeviceBlocks(DeviceBlock *out, UInt32 max, UInt32 *outBytes,
                                    bool scanBig)
{
    const UInt32 nchunks = (UInt32)(sizeof(fTables[0].icm) / sizeof(fTables[0].icm[0]));
    Mlx4IcmSum s;
    UInt32 n = 0, total = 0, i, c, skipped = 0;
    Mlx4Icm *b;
    const char *tag;
    /*
     * Blocks this size and up are the context tables and the firmware area -- megabytes each
     * -- and are summed only at the two ends of the window (scanBig).  Every intermediate
     * step re-reads just the small blocks, which are the ones a completion can land in
     * quickly; scanning ten megabytes eight times would cost more than the window it is
     * measuring and would report the same fact once at the end anyway.
     */
    static const UInt32 kBigBlockBytes = 256u * 1024u;

    if (outBytes) *outBytes = 0;
    if (!out || max == 0) return 0;

#define CX3_DBBLK(ptr_, name_)                                                 \
    do {                                                                       \
        b = (ptr_);                                                            \
        tag = (name_);                                                         \
        if (b) {                                                               \
            if (!scanBig && b->bytes >= (UInt64)kBigBlockBytes) { /* skipped by policy */ } \
            else if (n >= max) { ++skipped; }                                  \
            else {                                                             \
                fHw.icmSum(b, &s);                                             \
                out[n].tag     = tag;                                          \
                out[n].icm     = b;                                            \
                out[n].sum     = s.checksum;                                   \
                out[n].nonzero = (UInt32)s.nonzero;                            \
                out[n].bytes   = (UInt32)s.bytes;                              \
                total += (UInt32)s.bytes;                                      \
                ++n;                                                           \
            }                                                                  \
        }                                                                      \
    } while (0)

    CX3_DBBLK(fEqRing,     "EQ ring");
    for (UInt32 port = 0; port < 2; ++port) {
    CX3_DBBLK(fPortQueues[port].qpRec,    "RX QP producer record");
    CX3_DBBLK(fPortQueues[port].cqRing,   "RX CQ ring");
    CX3_DBBLK(fPortQueues[port].cqRec,    "RX CQ doorbell record");
    CX3_DBBLK(fPortQueues[port].rqRing,   "RX WQE ring");
    CX3_DBBLK(fPortQueues[port].rqBufs,   "RX frame buffers");
    CX3_DBBLK(fPortQueues[port].ingressRec, "indirection QP producer record");
    CX3_DBBLK(fPortQueues[port].txQpRec,  "TX QP producer record");
    CX3_DBBLK(fPortQueues[port].txSq,     "TX send queue");
    CX3_DBBLK(fPortQueues[port].rawFrame, "TX raw frame buffer");
    CX3_DBBLK(fPortQueues[port].txCqRing, "TX CQ ring");
    CX3_DBBLK(fPortQueues[port].txCqRec,  "TX CQ doorbell record");
    }
    CX3_DBBLK(fFwIcm,      "FW area (host-written)");
    CX3_DBBLK(fAuxIcm,     "ICM auxiliary area");
#undef CX3_DBBLK

    /* The context tables: the MTT lives in one of them, and the MTT is what every queue's
     * context points at, so this is the memory whose contents matter most. */
    for (i = 0; i < fNumTables && i < (UInt32)kMaxIcmTables; ++i) {
        for (c = 0; c < nchunks; ++c) {
            b = fTables[i].icm[c];
            if (!b) continue;
            if (!scanBig && b->bytes >= (UInt64)kBigBlockBytes) continue;
            if (n >= max) { ++skipped; continue; }
            fHw.icmSum(b, &s);
            out[n].tag     = fTables[i].name;
            out[n].icm     = b;
            out[n].sum     = s.checksum;
            out[n].nonzero = (UInt32)s.nonzero;
            out[n].bytes   = (UInt32)s.bytes;
            total += (UInt32)s.bytes;
            ++n;
        }
    }

    if (outBytes) *outBytes = total;
    if (skipped) {
        LOG("reset silence: %u live block(s) were NOT sampled (the list holds %u); the readout "
            "is a sample of the device-facing memory, not all of it",
            (unsigned)skipped, (unsigned)max);
    }
    return n;
}

/*
 * Does anything still write into host memory after the HCA reset?
 *
 * This is the measurement the investigation was missing.  Everything else in this file
 * establishes that the reset COMMAND was accepted; none of it says the datapath stopped
 * referring to host memory, and the teardowns that follow immediately hand that memory back
 * on the strength of exactly that inference.  So: sum every reachable block, wait in short
 * bounded steps, sum again, and report what moved and when.
 *
 * Read-only, and it changes no state: called once per reset, from exportResetHca(), after the
 * reset has succeeded and before the first teardown.  The window is short (tens of
 * milliseconds) and the honest reading of a quiet window is stated in the log itself, along
 * with the preconditions that make it interpretable at all.
 */
void Cx3Mlx4::resetSilenceCheck()
{
    /* Eight steps of 2 ms: long enough for a completion that was already in the pipeline to
     * land, short enough that a reset is not perceptibly slower.  Bounded by construction. */
    static const UInt32 kStepMs = 2;
    static const UInt32 kSteps  = 8;
    DeviceBlock prev[kDeviceBlockMax], now[kDeviceBlockMax];
    UInt32 nPrev, nNow, bytesPrev = 0, bytesNow = 0;
    UInt32 step, i, j, changed = 0, changedStep = 0;

    ++fResetSilenceRuns;
    /*
     * The preconditions first: they are about the state BEFORE this function's window, and the
     * quarantine entries the teardowns are about to create read them from here.
     */
    fResetPreTxPosted = (fPortQueues[0].rawTxProd >= fPortQueues[0].rawTxCons) ? (fPortQueues[0].rawTxProd - fPortQueues[0].rawTxCons) : 0;
    fResetPreRxPosted = cx3CountBits(fPortQueues[0].rawRxInFlight, (UInt32)sizeof(fPortQueues[0].rawRxInFlight));
    fResetPreQpsUp    = (fPortQueues[0].qpUp ? 1u : 0u) + (fPortQueues[0].ingressUp ? 1u : 0u) +
                        (fPortQueues[0].txQpUp ? 1u : 0u);
    fResetPreOwner    = fHw.readOwnerWord();

    nPrev = collectDeviceBlocks(prev, (UInt32)kDeviceBlockMax, &bytesPrev, true);
    if (nPrev == 0) {
        LOG("reset silence: there is no live device-facing allocation to sample, so nothing can "
            "be said about whether the datapath stopped (preconditions: %u send WQE(s) in "
            "flight, %u receive slot(s) posted, %u QP(s) live)",
            (unsigned)fResetPreTxPosted, (unsigned)fResetPreRxPosted,
            (unsigned)fResetPreQpsUp);
        return;
    }
    /* A line before the scan as well as after it, so that a machine which stops inside the
     * scan is distinguishable from one that stopped before the readout began. */
    LOG("reset silence: sampling %u live device-facing block(s) / %u KiB over %u ms, starting "
        "now (TX %u in flight, RX %u posted, %u QP(s) live, owner word 0x%08x)",
        (unsigned)nPrev, (unsigned)(bytesPrev >> 10), (unsigned)(kSteps * kStepMs),
        (unsigned)fResetPreTxPosted, (unsigned)fResetPreRxPosted,
        (unsigned)fResetPreQpsUp, (unsigned)fResetPreOwner);

    /*
     * Every step compares against the sample taken at the reset, not against the previous step:
     * a change that is reverted, or one that both steps happen to see the same way, is still a
     * change from the state the reset left behind, and that is the state the teardowns are
     * about to free.  The last step scans the big blocks again, so the window is bracketed at
     * both ends and every step in between is cheap.
     */
    for (step = 0; step < kSteps; ++step) {
        bool last = (step + 1 == kSteps);

        IOSleep(kStepMs);
        nNow = collectDeviceBlocks(now, (UInt32)kDeviceBlockMax, &bytesNow, last);
        for (i = 0; i < nNow; ++i) {
            /* Match by identity: a sample that skipped the big blocks has fewer rows, and a
             * bring-up can have added new ones since the reset. */
            for (j = 0; j < nPrev; ++j) if (prev[j].icm == now[i].icm) break;
            if (j == nPrev) continue;
            if (prev[j].sum == now[i].sum && prev[j].nonzero == now[i].nonzero) continue;
            LOG("reset silence: %s (%u bytes) CHANGED within %u ms of the reset: checksum "
                "0x%08x -> 0x%08x, non-zero bytes %u -> %u. Something is still writing into "
                "host memory the driver is about to hand back.",
                now[i].tag, (unsigned)now[i].bytes, (unsigned)((step + 1) * kStepMs),
                (unsigned)prev[j].sum, (unsigned)now[i].sum,
                (unsigned)prev[j].nonzero, (unsigned)now[i].nonzero);
            ++changed;
        }
        if (changed) { changedStep = step + 1; break; }
    }

    if (changed) {
        ++fResetSilenceChanges;
        LOG("reset silence VERDICT: %u block(s) changed inside %u ms of the reset -- the HCA "
            "reset does NOT mean the datapath has stopped referring to host memory, and the "
            "teardowns that follow are releasing memory the device is still writing to",
            (unsigned)changed, (unsigned)(changedStep * kStepMs));
    } else {
        LOG("reset silence: %u block(s) / %u KiB re-read over %u ms after the reset and nothing "
            "changed. That is a reading, not a proof: a device with nothing in flight looks the "
            "same, which is what the preconditions are for -- %u send WQE(s) were in flight, "
            "%u receive slot(s) were posted, %u QP(s) were live, ownership word 0x%08x. If "
            "those are all zero, this window tested nothing.",
            (unsigned)nPrev, (unsigned)(bytesPrev >> 10), (unsigned)(kSteps * kStepMs),
            (unsigned)fResetPreTxPosted, (unsigned)fResetPreRxPosted,
            (unsigned)fResetPreQpsUp, (unsigned)fResetPreOwner);
    }
}

/*
 * Read-only dump of any range, from user space.
 *
 * Motivation: every kext change costs a reboot, so the register exploration
 * that is still needed (what is really at 0x8069c, whether 0xf0000 is a reset
 * block or just filler, whether the HCR dump is coherent) would otherwise cost
 * one reboot per question.  With this selector it costs one reboot total.
 *
 * Bounds are enforced here rather than trusted from the caller.
 */
IOReturn Cx3Mlx4::exportPeek(const UInt64 *in, UInt64 *out)
{
    UInt64 space  = in[kPeekInSpace];
    UInt64 off    = in[kPeekInOffset];
    UInt64 want   = in[kPeekInCount];
    UInt64 limit;
    UInt64 i;

    memset(out, 0, sizeof(UInt64) * kPeekOutCount_);
    out[kPeekOutStatus] = 0;
    out[kPeekOutCount]  = 0;

    if (want == 0 || want > (UInt64)kPeekMaxDwords) {
        LOG("peek refused: count %llu outside 1..%u",
            (unsigned long long)want, (unsigned)kPeekMaxDwords);
        out[kPeekOutStatus] = (UInt64)kIOReturnBadArgument;
        return kIOReturnSuccess;
    }
    if (off & 3) {
        LOG("peek refused: offset 0x%llx is not dword aligned",
            (unsigned long long)off);
        out[kPeekOutStatus] = (UInt64)kIOReturnBadArgument;
        return kIOReturnSuccess;
    }

    switch (space) {
    case kPeekSpaceBar0:    limit = 0x00100000ull; break;
    case kPeekSpaceBar2:    limit = 0x00800000ull; break;
    case kPeekSpaceConfig:  limit = 0x00001000ull; break;
    default:
        LOG("peek refused: unknown space %llu", (unsigned long long)space);
        out[kPeekOutStatus] = (UInt64)kIOReturnBadArgument;
        return kIOReturnSuccess;
    }
    if (off + want * 4 > limit) {
        LOG("peek refused: 0x%llx + %llu dwords is past the %llu-byte limit",
            (unsigned long long)off, (unsigned long long)want,
            (unsigned long long)limit);
        out[kPeekOutStatus] = (UInt64)kIOReturnBadArgument;
        return kIOReturnSuccess;
    }

    for (i = 0; i < want; ++i) {
        UInt64 o = off + i * 4;
        UInt64 v;
        if (space == kPeekSpaceBar0)        v = fHw.readBar0((UInt32)o);
        else if (space == kPeekSpaceBar2)   v = fHw.readBar2((UInt32)o);
        else                                v = fPci ? fPci->configRead32((UInt8)o) : 0;
        out[kPeekOutData0 + i] = v;
    }
    out[kPeekOutCount] = want;
    return kIOReturnSuccess;
}

/*
 * mlx4_free_ownership(): writel(0, owner) then settle.  Before/after values are
 * reported so the caller can tell "cleared" from "would not clear".
 */
IOReturn Cx3Mlx4::exportClearOwner(UInt64 *out)
{
    UInt32 before = fHw.readOwnerWord();
    UInt32 after;

    out[kClrOutBefore] = before;
    out[kClrOutStatus] = 0;

    if (!fHw.clearOwner()) {
        out[kClrOutStatus] = (UInt64)kIOReturnError;
        out[kClrOutAfter] = fHw.readOwnerWord();
        return kIOReturnSuccess;
    }

    after = fHw.readOwnerWord();
    out[kClrOutAfter] = after;
    LOG("clearOwner: before=0x%08x after=0x%08x %s",
        (unsigned)before, (unsigned)after,
        (after == 0) ? "(cleared)" : "(STILL SET - the store did not take)");
    return kIOReturnSuccess;
}

/*
 * One guarded 32-bit store, with before/after read-back.
 *
 * The question this answers: does a BAR0 MMIO write from this kext reach the
 * device at all?  clearOwner() wrote 0 to 0x8069c and the value did not move,
 * which is consistent with two very different worlds -- that address being
 * read-only, or none of our writes landing.  Poking a second address separates
 * them.
 */
IOReturn Cx3Mlx4::exportPoke(const UInt64 *in, UInt64 *out)
{
    UInt64 space = in[kPokeInSpace];
    UInt64 off   = in[kPokeInOffset];
    UInt32 value = (UInt32)in[kPokeInValue];
    UInt64 limit;
    UInt32 before, after;

    memset(out, 0, sizeof(UInt64) * kPokeOutCount);
    out[kPokeOutValue] = value;

    if (off & 3) {
        LOG("poke refused: offset 0x%llx is not dword aligned",
            (unsigned long long)off);
        out[kPokeOutStatus] = (UInt64)kIOReturnBadArgument;
        return kIOReturnSuccess;
    }
    if (space != kPeekSpaceBar0 && space != kPeekSpaceBar2) {
        LOG("poke refused: only BAR0 and BAR2 may be written");
        out[kPokeOutStatus] = (UInt64)kIOReturnBadArgument;
        return kIOReturnSuccess;
    }
    limit = (space == kPeekSpaceBar0) ? 0x00100000ull : 0x00800000ull;
    if (off + 4 > limit) {
        LOG("poke refused: offset 0x%llx is past the window",
            (unsigned long long)off);
        out[kPokeOutStatus] = (UInt64)kIOReturnBadArgument;
        return kIOReturnSuccess;
    }

    /*
     * Two addresses turn a diagnostic into an action, so they are excluded
     * here regardless of what the caller asks for:
     *   0x00f010  writing the reset value here reboots the HCA
     *   0x080698  the HCR control dword, where GO posts a firmware command
     */
    if (space == kPeekSpaceBar0 &&
        (off == (MLX4_RESET_BASE + MLX4_RESET_OFFSET) ||
         off == (MLX4_HCR_BASE + HCR_STATUS_OFFSET))) {
        LOG("poke refused: 0x%05llx is an action register, not a diagnostic one",
            (unsigned long long)off);
        out[kPokeOutStatus] = (UInt64)kIOReturnNotPermitted;
        return kIOReturnSuccess;
    }

    if (space == kPeekSpaceBar0) {
        before = fHw.readBar0((UInt32)off);
        fHw.writeBar0((UInt32)off, value);
        after = fHw.readBar0((UInt32)off);
    } else {
        before = fHw.readBar2((UInt32)off);
        fHw.writeBar2((UInt32)off, value);
        after = fHw.readBar2((UInt32)off);
    }

    out[kPokeOutStatus] = 0;
    out[kPokeOutBefore] = before;
    out[kPokeOutAfter]  = after;
    LOG("poke BAR%llu 0x%05llx: before=0x%08x wrote=0x%08x after=0x%08x %s",
        (unsigned long long)space, (unsigned long long)off,
        (unsigned)before, (unsigned)value, (unsigned)after,
        (after == value) ? "(STORED - writes reach the device)"
                         : "(did not take)");
    return kIOReturnSuccess;
}

/*
 * QUERY_DEV_CAP, exposed to user space.  The raw outbox is handed back rather
 * than parsed fields, so that reading the capability set does not require a new
 * kext binary each time we want one more field.
 */
/*
 * Compile-time guards on the two places where two headers describe the same
 * buffer.  Both of these existed as runtime overflows first, and a wrong size
 * here does not fail a build -- it smashes the kernel stack, which is what
 * happened: the QUERY_DEV_CAP outbox window was widened on the QUERY_FW struct
 * while the QUERY_DEV_CAP struct stayed at 24 dwords, and the fill loop was
 * changed for both, so queryDevCap() wrote 64 dwords into a 24-dword stack
 * object (panic: general protection in Mlx4Hw::parseDevCap).
 *
 * So the widths are tied to each other in the compiler, not in a comment.
 */
CX3_STATIC_ASSERT(Cx3Mlx4::kMaxIcmTables >= Cx3Mlx4::kRefIcmTables,
                  "the ICM table array is smaller than the reference's table list");
CX3_STATIC_ASSERT(MLX4_DEVCAP_WORDS == kDevCapWords,
                  "the kext's dev-cap window and the user-space ABI's disagree");
CX3_STATIC_ASSERT(kDevCapWords * 4 <= sizeof(Cx3DevCapResult) - 16,
                  "Cx3DevCapResult is too small for kDevCapWords words");

IOReturn Cx3Mlx4::exportQueryDevCap(bool force, bool ignoreBusy, Cx3DevCapResult *res,
                                    UInt64 *scalars)
{
    Mlx4QueryDevCapResult dc;
    UInt32 ownerRef = fHw.checkOwnershipAsReference();
    UInt32 owner    = fHw.readOwnerWord();
    int    i;

    memset(res, 0, sizeof(*res));
    if (scalars) { scalars[kDevCapOutStatus] = 0; scalars[kDevCapOutFwStatus] = 0; }

    LOG("QUERY_DEV_CAP: ownership single-read=0x%08x settled=0x%08x (force=%u)",
        (unsigned)ownerRef, (unsigned)owner, (unsigned)force);

    if (!force && ownerRef != 0) {
        LOG("QUERY_DEV_CAP refused: ownership word reads 0x%08x", (unsigned)ownerRef);
        res->status = (UInt64)kIOReturnNotPermitted;
        res->fwStatus = 0xffffffffull;
        if (scalars) { scalars[kDevCapOutStatus] = res->status;
                       scalars[kDevCapOutFwStatus] = res->fwStatus; }
        return kIOReturnSuccess;
    }
    if (!fHw.windowAlive()) {
        LOG("QUERY_DEV_CAP refused: BAR0 window is not reading back live");
        res->status = (UInt64)kIOReturnOffline;
        res->fwStatus = 0xffffffffull;
        if (scalars) { scalars[kDevCapOutStatus] = res->status;
                       scalars[kDevCapOutFwStatus] = res->fwStatus; }
        return kIOReturnSuccess;
    }

    if (!fHw.queryDevCap(&dc, ignoreBusy)) {
        LOG("QUERY_DEV_CAP failed: fw_status=0x%x hcr_after=0x%08x",
            (unsigned)dc.fwStatus, (unsigned)fHw.readHcrStatus());
        res->status = (UInt64)kIOReturnError;
        res->fwStatus = dc.fwStatus;
        if (scalars) { scalars[kDevCapOutStatus] = res->status;
                       scalars[kDevCapOutFwStatus] = res->fwStatus; }
        return kIOReturnSuccess;
    }

    res->status   = 0;
    res->fwStatus = dc.fwStatus;
    /*
     * Both arrays have the same width by construction (asserted above), but the
     * loop is bounded by the destination as well, so a future edit to one header
     * cannot turn this copy into an overflow again.
     */
    for (i = 0; i < kDevCapWords && i < MLX4_DEVCAP_WORDS; ++i)
        res->words[i] = dc.rawWords[i];
    if (scalars) { scalars[kDevCapOutStatus] = 0;
                   scalars[kDevCapOutFwStatus] = dc.fwStatus; }

    LOG("QUERY_DEV_CAP OK: min_page_size=%u max_qp_log2=%u max_cq_log2=%u "
        "max_eq_log2=%u mtt_seg_log2=%u uar_sz_log2=%u flags=0x%llx ext=0x%08x",
        (unsigned)dc.minPageSize, (unsigned)dc.maxQpLog2, (unsigned)dc.maxCqLog2,
        (unsigned)dc.maxEqLog2, (unsigned)dc.maxMttSegLog2, (unsigned)dc.uarSzLog2,
        (unsigned long long)dc.flags, (unsigned)dc.extFlags);

    if (dc.minPageSize > 4096) {
        LOG("QUERY_DEV_CAP: device needs %u-byte pages, larger than the 4 KiB "
            "the ICM layer assumes -- M2 has to account for that",
            (unsigned)dc.minPageSize);
    }
    return kIOReturnSuccess;
}

/* QUERY_ADAPTER is kept separate from the interrupt setup selector: it only
 * submits the firmware read and exports the complete outbox. */
IOReturn Cx3Mlx4::exportQueryAdapter(Cx3AdapterResult *res)
{
    UInt8 pin = 0xff;
    UInt32 fwStatus = CX3_FWSTATUS_NOT_POSTED;

    if (!res) return kIOReturnBadArgument;
    memset(res, 0, sizeof(*res));
    res->version = CX3_ADAPTER_RESULT_VERSION;
    res->structBytes = (uint32_t)sizeof(*res);
    res->status = (UInt64)kIOReturnSuccess;
    res->fwStatus = CX3_FWSTATUS_NOT_POSTED;
    res->deviceGeneration = fDeviceGeneration;
    res->intaPin = 0xff;

    if (fResetCommandsBlocked || retentionExhausted()) {
        res->status = (UInt64)kIOReturnNotPermitted;
        return kIOReturnSuccess;
    }
    if (fHw.checkOwnershipAsReference() != 0) {
        res->status = (UInt64)kIOReturnNotPermitted;
        return kIOReturnSuccess;
    }
    if (!fHw.windowAlive()) {
        res->status = (UInt64)kIOReturnOffline;
        return kIOReturnSuccess;
    }

    if (!fHw.queryAdapter(&pin, res->boardId, &fwStatus, res->raw)) {
        res->status = (UInt64)kIOReturnError;
        res->fwStatus = fwStatus;
        return kIOReturnSuccess;
    }
    res->intaPin = pin;
    res->fwStatus = fwStatus;
    res->rawBytes = CX3_ADAPTER_RAW_BYTES;
    return kIOReturnSuccess;
}

IOReturn Cx3Mlx4::exportModuleRead(UInt32 port, UInt32 offset, UInt32 length,
                                     Cx3ModuleResult *res)
{
    if (!res) return kIOReturnBadArgument;
    memset(res, 0, sizeof(*res));
    res->version = CX3_MODULE_RESULT_VERSION;
    res->structBytes = (UInt32)sizeof(*res);
    res->deviceGeneration = fDeviceGeneration;
    res->port = port; res->offset = offset; res->requestedBytes = length;
    res->fwStatus = CX3_FWSTATUS_NOT_POSTED;
    res->madStatus = CX3_MODULE_MAD_NOT_RETURNED;
    if (!cx3ModuleRangeValid(port, offset, length)) {
        res->status = (UInt64)kIOReturnBadArgument;
        return kIOReturnSuccess;
    }
    res->i2cAddress = offset < 256 ? 0x50 : 0x51;
    res->wireOffset = offset & 255;
    if (fResetCommandsBlocked || retentionExhausted() ||
        fHw.checkOwnershipAsReference() != 0) {
        res->status = (UInt64)kIOReturnNotPermitted;
        return kIOReturnSuccess;
    }
    if (!fIcmMapped || !fHcaInited) {
        res->status = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }
    if (!fHw.windowAlive()) {
        res->status = (UInt64)kIOReturnOffline;
        return kIOReturnSuccess;
    }
    (void)fHw.readModule(port, offset, length, res);
    return kIOReturnSuccess;
}

IOReturn Cx3Mlx4::exportPcieLink(Cx3PcieLinkResult *res)
{
    if (!res) return kIOReturnBadArgument;
    memset(res, 0, sizeof(*res));
    res->version = CX3_PCIE_RESULT_VERSION;
    res->structBytes = sizeof(*res);
    res->status = (UInt64)kIOReturnSuccess;
    res->reason = CX3_PCIE_REASON_OK;
    res->deviceGeneration = fDeviceGeneration;
    if (fResetCommandsBlocked) {
        res->reason = CX3_PCIE_REASON_RESET_BLOCKED;
        res->status = (UInt64)kIOReturnNotPermitted;
        return kIOReturnSuccess;
    }
    if (retentionExhausted()) {
        res->reason = CX3_PCIE_REASON_RETENTION_BLOCKED;
        res->status = (UInt64)kIOReturnNoSpace;
        return kIOReturnSuccess;
    }
    if (!fPci) {
        res->reason = CX3_PCIE_REASON_NOT_READY;
        res->status = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }
    return fHw.queryPcieLink(res);
}

IOReturn Cx3Mlx4::exportRuntimeIdentity(Cx3RuntimeIdentityResult *res)
{
    if (!res) return kIOReturnBadArgument;
    memset(res, 0, sizeof(*res));
    res->version = CX3_RUNTIME_RESULT_VERSION;
    res->structBytes = sizeof(*res);
    res->status = (UInt64)kIOReturnSuccess;

    res->serviceRegistryId = getRegistryEntryID();
    if (res->serviceRegistryId != 0)
        res->validity |= CX3_RUNTIME_VALID_SERVICE;
    if (fPci) {
        res->providerRegistryId = fPci->getRegistryEntryID();
        if (res->providerRegistryId != 0)
            res->validity |= CX3_RUNTIME_VALID_PROVIDER;
    }
    if (fStartPciIdentityValid && fStartPciIdentity.valid()) {
        res->pciIdentity.vendor = fStartPciIdentity.vendor;
        res->pciIdentity.device = fStartPciIdentity.device;
        res->pciIdentity.subsystemVendor = fStartPciIdentity.subsystemVendor;
        res->pciIdentity.subsystemId = fStartPciIdentity.subsystemId;
        res->pciIdentity.bus = fStartPciIdentity.bus;
        res->pciIdentity.deviceNumber = fStartPciIdentity.deviceNumber;
        res->pciIdentity.function = fStartPciIdentity.function;
        res->validity |= CX3_RUNTIME_VALID_PCI_SNAPSHOT;
    }
    res->deviceGeneration = fDeviceGeneration;
    if (fDeviceGeneration != 0)
        res->validity |= CX3_RUNTIME_VALID_GENERATION;
    UInt64 sampleTicks = 0;
    clock_get_uptime(&sampleTicks);
    absolutetime_to_nanoseconds(sampleTicks, &res->sampleUptimeNs);
    if (res->sampleUptimeNs != 0)
        res->validity |= CX3_RUNTIME_VALID_UPTIME;
    if (cx3CopyBuildIdentity(res->bundleVersion, sizeof(res->bundleVersion), CX3_BUNDLE_VERSION) &&
        cx3CopyBuildIdentity(res->buildTag, sizeof(res->buildTag), CX3_BUILD_TAG))
        res->validity |= CX3_RUNTIME_VALID_BUILD;
    if (fResetCommandsBlocked) res->lifecycleFlags |= CX3_RUNTIME_FLAG_RESET_BLOCKED;
    if (fStopRetained) res->lifecycleFlags |= CX3_RUNTIME_FLAG_STOP_RETAINED;
    if (fHw.cmdWedged()) res->lifecycleFlags |= CX3_RUNTIME_FLAG_CMD_WEDGED;
    if (fQuarExhausted) res->lifecycleFlags |= CX3_RUNTIME_FLAG_QUAR_EXHAUSTED;
    if (fFwRunAttempted) res->lifecycleFlags |= CX3_RUNTIME_FLAG_FW_RUN_ATTEMPTED;
    return kIOReturnSuccess;
}


/* Both FW-area selectors run under the transaction lock. Refuse lifecycle
 * dependencies before BAR reads, UNMAP, allocation, fill or free. This is an
 * admission check, not a quiescence credential. fResetUnproven alone is not a
 * refusal: a clean first-start reset sets it while permitting initialization. */
bool Cx3Mlx4::fwReplacementAllowed()
{
    if (selectorStopping() || fStopRetained || fResetCommandsBlocked ||
        fQuarExhausted || fHw.cmdWedged() || fFwRunAttempted ||
        fHcaInited || fIcmMapped || fIcmHcaHolds ||
        hasContextOwners() || hasIcmTableAllocations() || hasStopRetentionOwners() ||
        fHw.hasAbRetainedRings() || fNetEnabled || fNetIface || fNetTimer ||
        fNetTxRing || fPortQueues[0].macUp || fIrqSource || fIrqObjectsUp || fIrqEnabled ||
        fClrIntReady) return false;
    if (fNetAttachLock) {
        IOLockLock(fNetAttachLock);
        bool busy = fNetAttachInFlight || fNetAttachPoisoned || fNetAttachIface != 0;
        IOLockUnlock(fNetAttachLock);
        if (busy) return false;
    } else if (fNetAttachCall || fNetAttachIface) {
        return false;
    }
    if (fFwIcm) {
        const Mlx4DmaGeneration &g = fFwIcm->generation;
        if (!g.validAllocation() || g.allocationGeneration != fDeviceGeneration ||
            (g.mayBeDeviceOwned ? !g.matchesOwnerGeneration(fDeviceGeneration)
                                : g.ownerGeneration != 0)) return false;
    }
    return true;
}

/* Allocate/free only an audited never-handed-off FW area. The shared slot can
 * also hold MAP_FA-owned pages, so this diagnostic must not bypass fwload's
 * revoke contract. No device access is made by this selector. */
IOReturn Cx3Mlx4::exportIcmAlloc(UInt32 pages, UInt64 *out)
{
    UInt32 badPage = 0;
    UInt64 checksum = 0;
    UInt32 i;

    memset(out, 0, sizeof(UInt64) * kIcmOutCount);

    if (!fwReplacementAllowed() ||
        (fFwIcm && fFwIcm->generation.mayBeDeviceOwned)) {
        out[kIcmOutStatus] = (UInt64)kIOReturnNotReady;
        LOG("icm: refusing FW-area free/replacement with live, retained or handed-off owners");
        return kIOReturnSuccess;
    }

    if (pages == 0) {
        if (fFwIcm) {
            LOG("icm: freeing %u pages", (unsigned)fFwIcm->npages);
            fHw.icmFree(fFwIcm);
            fFwIcm = 0;
        }
        out[kIcmOutStatus] = 0;
        LOG("icm: freed");
        return kIOReturnSuccess;
    }

    if (fFwIcm) {
        LOG("icm: freeing the previous %u pages before allocating %u",
            (unsigned)fFwIcm->npages, (unsigned)pages);
        fHw.icmFree(fFwIcm);
        fFwIcm = 0;
    }

    fFwIcm = fHw.icmAlloc(pages);
    if (!fFwIcm) {
        LOG("icm: allocation of %u pages failed", (unsigned)pages);
        out[kIcmOutStatus] = (UInt64)kIOReturnNoMemory;
        return kIOReturnSuccess;
    }

    for (i = 0; i < fFwIcm->npages; ++i) checksum += fFwIcm->pages[i].busAddr;

    out[kIcmOutStatus]     = 0;
    out[kIcmOutPages]      = fFwIcm->npages;
    out[kIcmOutBytes]      = fFwIcm->bytes;
    out[kIcmOutFirstBus]   = fFwIcm->pages[0].busAddr;
    out[kIcmOutLastBus]    = fFwIcm->pages[fFwIcm->npages - 1].busAddr;
    out[kIcmOutAligned]    = fHw.icmCheck(fFwIcm, &badPage) ? 1 : 0;
    out[kIcmOutSegments]   = fFwIcm->nseg;
    out[kIcmOutContiguous] = fFwIcm->contiguous ? 1 : 0;
    out[kIcmOutChecksum]   = checksum;

    LOG("icm: allocated %u pages (%llu bytes): first bus 0x%llx, last 0x%llx, "
        "%u segment(s), aligned=%llu, checksum 0x%llx",
        (unsigned)out[kIcmOutPages], (unsigned long long)out[kIcmOutBytes],
        (unsigned long long)out[kIcmOutFirstBus],
        (unsigned long long)out[kIcmOutLastBus],
        (unsigned)out[kIcmOutSegments], (unsigned long long)out[kIcmOutAligned],
        (unsigned long long)checksum);

    /* Handing this to the firmware comes next (MAP_FA), which is a separate
     * selector so that an allocation fault and a command fault stay separable. */
    return kIOReturnSuccess;
}


/*
 * M2b: allocate the FW area, hand it to the firmware, start the firmware, then
 * ask it to reconsider the ICM page size.
 *
 * This is the step QUERY_DEV_CAP was waiting for.  Until the firmware is running,
 * that command answers CMD_STAT_BAD_SYS_STATE (0x4), which is what the first
 * attempt got and what sent us back to read the reference's startup order.
 *
 * Every firmware status is passed back individually, so if this fails it says
 * which of the four steps failed rather than "M2b did not work".
 */
IOReturn Cx3Mlx4::exportFwLoad(UInt32 pages, bool lowmem, bool force, bool fill,
                               UInt64 *out)
{
    UInt32 mapEntries = 0, unmapStatus = CX3_FWSTATUS_NOT_POSTED;
    UInt32 mapStatus = 0xffffffffu, runStatus = 0xffffffffu, modStatus = 0xffffffffu;

    memset(out, 0, sizeof(UInt64) * kFwLoadOutCount);
    out[kFwLoadOutPages]       = pages;
    out[kFwLoadOutMapFaStatus] = 0xffffffffull;
    out[kFwLoadOutRunFwStatus] = 0xffffffffull;
    out[kFwLoadOutModStatus]   = 0xffffffffull;

    if (pages == 0 || pages > (1u << 20)) {
        LOG("fwload: refusing, %u pages is not a plausible fw_pages", (unsigned)pages);
        out[kFwLoadOutStatus] = (UInt64)kIOReturnBadArgument;
        return kIOReturnSuccess;
    }
    /*
     * The page count must be the one the device asked for.
     *
     * This is the check whose absence let M2b hand the firmware 5123 pages when
     * the FW size field (at 0x00) said 1542: fwload accepted any number in range,
     * so a wrong offset in QUERY_FW turned into a wrong mapping with nothing in
     * between to notice.  force=1 is the deliberate escape hatch.
     */
    if (!force && fQueriedFwPages != 0 && pages != fQueriedFwPages) {
        LOG("fwload: refusing %u pages, the device asked for %u (QUERY_FW). "
            "Pass force=1 only to run a deliberate experiment.",
            (unsigned)pages, (unsigned)fQueriedFwPages);
        out[kFwLoadOutStatus] = (UInt64)kIOReturnBadArgument;
        return kIOReturnSuccess;
    }
    if (!fwReplacementAllowed()) {
        LOG("fwload: refusing before device access; lifecycle or FW ownership is unresolved");
        out[kFwLoadOutStatus] = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }
    if (!fHw.windowAlive()) {
        LOG("fwload: refusing, the BAR0 window is not reading back live");
        out[kFwLoadOutStatus] = (UInt64)kIOReturnOffline;
        return kIOReturnSuccess;
    }

    /* 1. Allocate the area the firmware asked for.  Device-inert. */
    if (fFwIcm) {
        /*
         * The reference unmaps before it frees, always (mlx4_close_fw,
         * mlx4_main.c:1913-1918, and the load-failure path at :1496).  Freeing
         * first would leave the firmware holding a page list that points at
         * memory the host is about to reuse, which is exactly the kind of bug
         * that makes attempt 2 fail differently from attempt 1.
         */
        /* A valid never-handed-off allocation is still host-only. Otherwise
         * require a confirmed mapping revoke in this pre-RUN_FW lifecycle;
         * failed/unknown completion leaves the exact old slot and bytes intact.
         * This does not authorize live firmware replacement or generic DMA free. */
        if (fFwIcm->generation.mayBeDeviceOwned && !fHw.unmapFa(&unmapStatus)) {
            LOG("fwload: UNMAP_FA before re-use failed (fw status 0x%x), "
                "keeping the previous FW area and refusing replacement",
                (unsigned)unmapStatus);
            out[kFwLoadOutStatus] = (UInt64)kIOReturnError;
            return kIOReturnSuccess;
        }
        LOG("fwload: freeing the previous FW area (%u pages) first",
            (unsigned)fFwIcm->npages);
        fHw.icmFree(fFwIcm);
        fFwIcm = 0;
    }
    fFwIcm = fHw.icmAlloc(pages, lowmem);
    if (!fFwIcm) {
        LOG("fwload: could not allocate %u pages for the FW area", (unsigned)pages);
        out[kFwLoadOutStatus] = (UInt64)kIOReturnNoMemory;
        return kIOReturnSuccess;
    }
    out[kFwLoadOutFirstBus] = fFwIcm->pages[0].busAddr;
    out[kFwLoadOutLastBus]  = fFwIcm->pages[fFwIcm->npages - 1].busAddr;
    LOG("fwload: FW area %u pages = %u KiB, bus 0x%llx..0x%llx, %u page(s) >= 4 GiB, "
        "lowmem=%u",
        (unsigned)fFwIcm->npages, (unsigned)(fFwIcm->npages * 4),
        (unsigned long long)fFwIcm->busLow, (unsigned long long)fFwIcm->busHigh,
        (unsigned)fFwIcm->pagesAbove4G, (unsigned)lowmem);

    /*
     * Sample the area at each step that could change it.  Nothing in the host
     * driver ever writes this memory (the reference has no write to fw_icm), so
     * any content here came from the device, and the step at which it appears is
     * the step at which the device loaded its firmware image.
     */
    fillFwAreaSummary("right after allocation, before MAP_FA", out);

    if (fill) {
        /*
         * Write a known pattern before handing the area over, so that what is in
         * it afterwards is interpretable: pattern intact means the device never
         * wrote, pattern gone means it did.  Host memory only.
         */
        fHw.icmFill(fFwIcm);
        fillFwAreaSummary("right after filling it with a known pattern", out);
    }

    /* 2. MAP_FA: tell the firmware which pages it may use. */
    if (!fHw.mapFa(fFwIcm, &mapEntries, &mapStatus)) {
        out[kFwLoadOutEntries]     = mapEntries;
        out[kFwLoadOutMapFaStatus] = mapStatus;
        out[kFwLoadOutStatus]      = (UInt64)kIOReturnError;
        fillFwAreaSummary("after MAP_FA failed", out);
        LOG("fwload: MAP_FA failed (fw status 0x%x) after %u entries",
            (unsigned)mapStatus, (unsigned)mapEntries);
        return kIOReturnSuccess;
    }
    out[kFwLoadOutEntries]     = mapEntries;
    out[kFwLoadOutMapFaStatus] = mapStatus;
    fillFwAreaSummary("after MAP_FA", out);

    /* 3. RUN_FW: leave the idle state. */
    fFwRunAttempted = true;
    if (!fHw.runFw(&runStatus)) {
        out[kFwLoadOutRunFwStatus] = runStatus;
        out[kFwLoadOutStatus]      = (UInt64)kIOReturnError;
        fillFwAreaSummary("after RUN_FW (it did not complete)", out);
        LOG("fwload: RUN_FW failed (fw status 0x%x)", (unsigned)runStatus);
        return kIOReturnSuccess;
    }
    out[kFwLoadOutRunFwStatus] = runStatus;
    fillFwAreaSummary("after RUN_FW completed", out);

    /*
     * 4. MOD_STAT_CFG: the reference asks for log_pg_sz_m = 1, log_pg_sz = 0 and
     *    only warns if it fails, so a failure here is recorded but not fatal.
     */
    fHw.modStatCfg(0, 1, &modStatus);
    out[kFwLoadOutModStatus] = modStatus;

    out[kFwLoadOutStatus] = 0;
    LOG("fwload OK: %u pages, %u MAP_FA entries, MAP_FA status 0x%x, "
        "RUN_FW status 0x%x, MOD_STAT_CFG status 0x%x -- QUERY_DEV_CAP should "
        "answer now",
        (unsigned)pages, (unsigned)mapEntries, (unsigned)mapStatus,
        (unsigned)runStatus, (unsigned)modStatus);
    return kIOReturnSuccess;
}

/*
 * Record what the FW area holds right now, both in the fwload result and in the
 * log.  Called at every point where the contents could have changed.
 *
 * The reason this is worth a whole scan of up to a few megabytes: the device is
 * the only writer of this memory, so "non-zero bytes" is direct evidence that
 * the firmware image was DMA'd in.  Without it, a RUN_FW that never completes
 * cannot be told apart from a RUN_FW that had nothing to run.
 */
void Cx3Mlx4::fillFwAreaSummary(const char *when, UInt64 *out)
{
    Mlx4IcmSum sum;
    UInt64 permille;

    if (!fFwIcm) return;
    fHw.icmSum(fFwIcm, &sum);
    permille = sum.bytes ? (sum.nonzero * 1000ull) / sum.bytes : 0;

    if (out) {
        out[kFwLoadOutNonzero]  = sum.nonzero;
        out[kFwLoadOutChecksum] = sum.checksum;
    }
    LOG("fwload: FW area %s: %llu of %llu bytes non-zero (%llu permille), "
        "checksum 0x%08x",
        when, (unsigned long long)sum.nonzero, (unsigned long long)sum.bytes,
        (unsigned long long)permille, (unsigned)sum.checksum);
}

/*
 * kCx3SelFwArea -- read-only inspection of the FW area the device was handed.
 *
 * Answers the question no register answers: is there a firmware image in the
 * memory we mapped?  Also the only way to look at it without another reboot,
 * because a kext change costs one.
 */
IOReturn Cx3Mlx4::exportFwArea(const UInt64 *in, UInt64 *out)
{
    Mlx4IcmSum sum;
    UInt32 offset = (UInt32)in[kFwAreaInOffset];
    UInt32 count  = (UInt32)in[kFwAreaInCount];
    UInt32 words[kFwAreaMaxDwords];
    UInt32 i;
    UInt64 permille;

    memset(out, 0, sizeof(UInt64) * kFwAreaOutCount);

    if (count > (UInt32)kFwAreaMaxDwords) {
        LOG("fwarea refused: %u dwords is over the %u-dword budget",
            (unsigned)count, (unsigned)kFwAreaMaxDwords);
        out[kFwAreaOutStatus] = (UInt64)kIOReturnBadArgument;
        return kIOReturnSuccess;
    }
    if (!fFwIcm) {
        LOG("fwarea: no FW area is held; run 'fwload' or 'icmalloc' first");
        out[kFwAreaOutStatus] = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }

    fHw.icmSum(fFwIcm, &sum);
    out[kFwAreaOutHeld]     = 1;
    out[kFwAreaOutPages]    = fFwIcm->npages;
    out[kFwAreaOutNonzero]  = sum.nonzero;
    out[kFwAreaOutChecksum] = sum.checksum;

    if (count) {
        if (!fHw.icmDump(fFwIcm, offset, count, words)) {
            LOG("fwarea refused: offset 0x%x plus %u dwords is past the %u-page area",
                (unsigned)offset, (unsigned)count, (unsigned)fFwIcm->npages);
            out[kFwAreaOutStatus] = (UInt64)kIOReturnBadArgument;
            return kIOReturnSuccess;
        }
        for (i = 0; i < count; ++i) out[kFwAreaOutData0 + i] = words[i];
    }

    permille = sum.bytes ? (sum.nonzero * 1000ull) / sum.bytes : 0;
    LOG("fwarea: %u pages (%llu bytes), %llu non-zero (%llu permille), checksum 0x%08x",
        (unsigned)fFwIcm->npages, (unsigned long long)sum.bytes,
        (unsigned long long)sum.nonzero, (unsigned long long)permille,
        (unsigned)sum.checksum);
    out[kFwAreaOutStatus] = 0;
    return kIOReturnSuccess;
}

/*
 * kCx3SelFwOutbox -- prove whether the device can write into the FW area.
 *
 * Sends QUERY_FW with the outbox address set to a page inside the FW area, then
 * reports both the device's answer and how the page changed.  Three readings are
 * distinguishable, and that is the whole point:
 *
 *   answer parsed AND the page changed  -> the device DMA'd into this memory, so
 *                                          reachability is not the problem and a
 *                                          RUN_FW hang has another cause
 *   answer parsed but the page unchanged -> the command completed somewhere else;
 *                                          the address we believe we handed over
 *                                          is not the address the device used
 *   no answer at all (GO stays set)      -> this memory is not usable as a command
 *                                          buffer, which is what RUN_FW needs
 *
 * The page is filled with a known pattern first so "changed" means something:
 * freshly allocated kernel memory may already hold old data.
 */
IOReturn Cx3Mlx4::exportFwOutbox(const UInt64 *in, UInt64 *out)
{
    UInt32 page       = (UInt32)in[kFwOutboxInPage];
    UInt32 dwords     = (UInt32)in[kFwOutboxInDwords];
    bool   ignoreBusy = (in[kFwOutboxInIgnoreBusy] != 0);
    UInt32 before[kFwOutboxMaxDwords];
    Mlx4QueryFwResult fw;
    UInt32 fwStatus = CX3_FWSTATUS_NOT_POSTED;
    UInt32 changed = 0, nonzero = 0, i;
    UInt32 *pageBase;
    UInt64  pageBus;

    memset(out, 0, sizeof(UInt64) * kFwOutboxOutCount);
    memset(before, 0, sizeof(before));
    memset(&fw, 0, sizeof(fw));

    if (dwords > (UInt32)kFwOutboxMaxDwords) {
        LOG("fwoutbox refused: %u dwords is over the %u-dword budget",
            (unsigned)dwords, (unsigned)kFwOutboxMaxDwords);
        out[kFwOutboxOutStatus] = (UInt64)kIOReturnBadArgument;
        return kIOReturnSuccess;
    }
    if (!fFwIcm || !fFwIcm->pages) {
        LOG("fwoutbox: no FW area is held; run fwload or icmalloc first");
        out[kFwOutboxOutStatus] = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }
    if (page >= fFwIcm->npages) {
        LOG("fwoutbox refused: page %u is past the %u-page area",
            (unsigned)page, (unsigned)fFwIcm->npages);
        out[kFwOutboxOutStatus] = (UInt64)kIOReturnBadArgument;
        return kIOReturnSuccess;
    }
    if (!fHw.windowAlive()) {
        LOG("fwoutbox refused: the BAR0 window is not reading back live");
        out[kFwOutboxOutStatus] = (UInt64)kIOReturnOffline;
        return kIOReturnSuccess;
    }

    pageBus  = fFwIcm->pages[page].busAddr;
    pageBase = (UInt32 *)((UInt8 *)fFwIcm->virt + ((UInt64)page << 12));

    /* Known pattern first, so "changed" is meaningful. */
    for (i = 0; i < 1024; ++i) pageBase[i] = 0xf00d0000u | (i & 0xffffu);
    for (i = 0; i < dwords; ++i) before[i] = pageBase[i];

    LOG("fwoutbox: QUERY_FW into FW-area page %u (bus 0x%llx), ignoreBusy=%u",
        (unsigned)page, (unsigned long long)pageBus, (unsigned)ignoreBusy);

    /*
     * QUERY_FW's outbox is handed over as out_param, exactly as it is for the
     * ordinary mailbox -- the only difference is that this address points into
     * the FW area instead of the dedicated mailbox.
     */
    if (!fHw.queryFwAt(fFwIcm, page, &fw, &fwStatus, ignoreBusy)) {
        out[kFwOutboxOutStatus]   = (UInt64)kIOReturnError;
        out[kFwOutboxOutPage]     = page;
        out[kFwOutboxOutBusAddr]  = pageBus;
        out[kFwOutboxOutFwStatus] = fwStatus;
        for (i = 0; i < dwords; ++i) out[kFwOutboxOutData0 + i] = pageBase[i];
        LOG("fwoutbox: the command produced no answer (fw status 0x%x); "
            "the page is %s the pattern", (unsigned)fwStatus,
            (pageBase[0] == (0xf00d0000u)) ? "still holding" : "no longer holding");
        return kIOReturnSuccess;
    }

    for (i = 0; i < 1024; ++i) if (pageBase[i]) ++nonzero;
    for (i = 0; i < 1024; ++i)
        if (pageBase[i] != (0xf00d0000u | (i & 0xffffu))) ++changed;

    out[kFwOutboxOutStatus]    = 0;
    out[kFwOutboxOutPage]      = page;
    out[kFwOutboxOutBusAddr]   = pageBus;
    out[kFwOutboxOutFwStatus]  = fw.fwStatus;
    out[kFwOutboxOutFwVer]     = fw.fwVer;
    out[kFwOutboxOutChanged]   = changed;
    out[kFwOutboxOutNonzero]   = nonzero;
    for (i = 0; i < dwords; ++i) out[kFwOutboxOutData0 + i] = pageBase[i];

    LOG("fwoutbox: QUERY_FW answered (status 0x%x, fw_ver 0x%llx); page %u has "
        "%u/1024 dwords differing from the pattern and %u non-zero",
        (unsigned)fw.fwStatus, (unsigned long long)fw.fwVer,
        (unsigned)page, (unsigned)changed, (unsigned)nonzero);
    if (changed == 0) {
        LOG("fwoutbox: the page did NOT change -- the device answered somewhere "
            "other than the address it was given");
    }
    return kIOReturnSuccess;
}

/*
 * kCx3SelM3ClrInt -- set up the legacy interrupt-clear register.
 *
 * The reviews' ordering: a legacy source may only be enabled once a real clear path
 * exists, so this is the prerequisite step and m3irqup refuses without it.  It runs
 * the one command whose answer it needs (QUERY_ADAPTER for inta_pin), takes
 * clr_int_base/clr_int_bar from the QUERY_FW result this port already kept, and
 * reports the exact address and value it will write -- so the clear can be checked
 * against the reference's arithmetic before anything is enabled.
 */
IOReturn Cx3Mlx4::exportM3ClrInt(UInt64 *out)
{
    UInt8  intaPin = 0xff, boardId[MLX4_BOARD_ID_LEN];
    UInt32 fwStatus = CX3_FWSTATUS_NOT_POSTED, mask = 0, offset = 0, via = 0xffffffffu;
    Mlx4QueryFwResult fw;

    memset(out, 0, sizeof(UInt64) * kClrIntOutCount);
    memset(boardId, 0, sizeof(boardId));
    out[kClrIntOutFwStatus] = CX3_FWSTATUS_NOT_POSTED;

    if (!fHw.clrIntFieldsFromQueryFw(&fw)) {
        LOG("m3clrint refused: no QUERY_FW result is held; run 'm3equp' (which runs "
            "QUERY_FW) first, or 'queryfw'");
        out[kClrIntOutStatus] = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }
    if (!fHw.queryAdapter(&intaPin, boardId, &fwStatus)) {
        out[kClrIntOutFwStatus] = fwStatus;
        out[kClrIntOutStatus] = (UInt64)kIOReturnError;
        return kIOReturnSuccess;
    }
    out[kClrIntOutFwStatus] = fwStatus;
    out[kClrIntOutIntaPin]  = intaPin;
    out[kClrIntOutBar]      = fw.clrIntBar;
    out[kClrIntOutBase]     = fw.clrIntBase;
    out[kClrIntOutBoard0]   = ((UInt64)boardId[0] << 24) | ((UInt64)boardId[1] << 16) |
                              ((UInt64)boardId[2] << 8) | (UInt64)boardId[3];
    out[kClrIntOutBoard1]   = ((UInt64)boardId[4] << 24) | ((UInt64)boardId[5] << 16) |
                              ((UInt64)boardId[6] << 8) | (UInt64)boardId[7];

    if (!fHw.clrIntSetup((UInt8)fw.clrIntBar, fw.clrIntBase, intaPin,
                         &mask, &offset, &via)) {
        LOG("m3clrint: could not set up the clear register (BAR %u + 0x%llx, pin %u)",
            (unsigned)fw.clrIntBar, (unsigned long long)fw.clrIntBase,
            (unsigned)intaPin);
        out[kClrIntOutStatus] = (UInt64)kIOReturnError;
        return kIOReturnSuccess;
    }

    fClrIntReady = true;
    fClrIntWrites = 0;
    out[kClrIntOutMask] = mask;
    out[kClrIntOutOffset] = offset;
    out[kClrIntOutReady] = 1;
    out[kClrIntOutStatus] = 0;
    LOG("m3clrint OK: the legacy interrupt can now be acknowledged; m3irqup will "
        "accept a non-messaged source from here on");
    return kIOReturnSuccess;
}

/* ==================================================================== */
/* R4 stage 3: the receive ring's buffers and WQEs                      */
/* ==================================================================== */

/*
 * kCx3SelR4Rq -- build the receive ring.
 *
 * Nothing here touches the hardware: rqCreate allocates the ring and the buffers, writes
 * every WQE, and fills in the MTT entries.  Handing it over is stage 4, with the QP.  That
 * split is deliberate and is the same one that made stages 1 and 2 debuggable -- "the
 * memory is built correctly" and "the hardware accepted it" are two claims, and when they
 * are one step a failure cannot say which failed.
 *
 * It needs stage 1's MR, because every WQE carries that MR's lkey.  It does NOT need a CQ:
 * buffers and descriptors exist independently of where completions go.
 */
IOReturn Cx3Mlx4::exportR4Rq(const UInt64 *in, Cx3R4RqResult *res, UInt64 *out, UInt32 port)
{
    if (port<1 || port>2 || !res || !out || !in) return kIOReturnBadArgument;
    for (UInt32 i=0;i<kR4RqInCount;++i)
        if (in[i]>0x7fffffffull) return kIOReturnBadArgument;
    Cx3PortQueues &queue = fPortQueues[port-1];
    if ((port==2 && !queue.numbersReserved) || queue.setupFailed ||
        (queue.numbersReserved && queue.numberGeneration!=fDeviceGeneration)) {
        memset(res,0,sizeof(*res)); res->status=(UInt32)kIOReturnNotReady;
        memset(out,0,sizeof(UInt64)*kR4RqOutCount);
        out[0]=(UInt64)kIOReturnNotReady; return kIOReturnSuccess;
    }

    UInt32 nent     = (UInt32)in[kR4RqInNent];
    UInt32 bufBytes = (UInt32)in[kR4RqInBufBytes];
    UInt32 pages = 0, mttOffset = 0, mttAddr = 0;
    UInt64 bufPhysFirst = 0, ringPhysFirst = 0;
    UInt32 bufPhysCount = 0, ringPhysCount = 0;
    Mlx4Icm *ring = 0, *bufs = 0;
    const UInt8 *w;
    UInt32 k;

    memset(out, 0, sizeof(UInt64) * kR4RqOutCount);
    memset(res, 0, sizeof(*res));
    res->status = (UInt32)kIOReturnError;

    if (nent == 0)     nent     = CX3_R4RQ_DEFAULT_NENT;
    if (bufBytes == 0) bufBytes = CX3_R4RQ_DEFAULT_BUF;

    /*
     * Idempotence, from the start this time rather than after a review.  A second run
     * would otherwise allocate a second ring and a second set of buffers while the first
     * set is still the one the descriptors name.
     */
    if (queue.rqUp) {
        const UInt8 *wr = (const UInt8 *)queue.rqRing->virt + RX_RING_HEADER_SIZE;
        res->nent       = queue.rqNent;
        res->bufBytes   = queue.rqBufBytes;
        res->byteCountSent = queue.rqBufBytes - MLX4_NET_IP_ALIGN;
        res->ipAlign    = MLX4_NET_IP_ALIGN;
        res->wqeSize    = RX_WQE_SIZE;
        res->ringHeaderBytes = RX_RING_HEADER_SIZE;
        res->ringPages  = queue.rqPages;
        res->mttOffset  = queue.rqMttOff;
        res->lkey       = queue.rqLkey;
        res->lkeyRawIndex = fMrMptIndex;
        res->bufPhysFirst = queue.rqBufPhys;
        res->ringPhysFirst = (UInt32)queue.rqRing->pages[0].busAddr;
        res->ringPhysCount = queue.rqPages;
        /* Re-read WQE 0 out of the ring rather than reporting remembered numbers. */
        {
            UInt32 bc = 0, lk = 0;
            UInt64 ad = 0;
            for (k = 0; k < 4; ++k) bc = (bc << 8) | wr[RX_WQE_BYTE_COUNT_OFFSET + k];
            for (k = 0; k < 4; ++k) lk = (lk << 8) | wr[RX_WQE_LKEY_OFFSET + k];
            for (k = 0; k < 8; ++k) ad = (ad << 8) | (UInt64)wr[RX_WQE_ADDR_OFFSET + k];
            res->wqe0ByteCount = bc; res->wqe0Lkey = lk; res->wqe0Addr = ad;
            res->wqe0Matches = (bc == res->byteCountSent && lk == queue.rqLkey &&
                                ad == queue.rqBufPhys) ? 1 : 0;
        }
        res->bufsInOneAlloc = 1;
        res->status = 0;
        out[kR4RqOutStatus] = 0;
        LOG("r4rq: a receive ring already exists (%u entries, buffers from 0x%llx); "
            "measured its WQE 0 rather than building a second one",
            (unsigned)queue.rqNent, (unsigned long long)queue.rqBufPhys);
        return kIOReturnSuccess;
    }
    if (queue.rqUnknownRetained || queue.rqRing || queue.rqBufs) {
        LOG("r4rq refused: an unavailable RQ allocation owner is retained; teardown must "
            "retry its MTT release before creating or consuming another ring");
        res->status = (UInt32)kIOReturnNotReady;
        out[kR4RqOutStatus] = res->status;
        return kIOReturnSuccess;
    }

    if (!fHcaInited) {
        LOG("r4rq refused: the HCA is not initialised (run m2dprofile/m2dbringup first)");
        res->status = (UInt32)kIOReturnNotReady;
        out[kR4RqOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if (fMrState != 1) {
        LOG("r4rq refused: no memory region is enabled. Every WQE carries its lkey, so "
            "stage 1 has to have run and succeeded (state is %u).", (unsigned)fMrState);
        res->status = (UInt32)kIOReturnNotReady;
        out[kR4RqOutStatus] = res->status;
        return kIOReturnSuccess;
    }

    if (!fHw.rqCreate(nent, bufBytes, fMrLkey, &ring, &bufs, &pages, &mttOffset, &mttAddr,
                      &bufPhysFirst, &bufPhysCount, &ringPhysFirst, &ringPhysCount)) {
        if (ring && bufs && pages != 0u) {
            queue.rqRing = ring;
            queue.rqBufs = bufs;
            queue.rqNent = nent;
            queue.rqBufBytes = bufBytes;
            queue.rqPages = pages;
            queue.rqMttOff = mttOffset;
            queue.rqMttPages = pages;
            queue.rqMttAddr = mttAddr;
            queue.rqLkey = fMrLkey;
            queue.rqBufPhys = bufPhysFirst;
            queue.rqUnknownRetained = true;
            LOG("r4rq: creation stopped with a retained pre-handoff ring/buffer/MTT owner; "
                "do not reuse it until teardown successfully retries the exact MTT release");
        }
        out[kR4RqOutStatus] = (UInt32)kIOReturnError;
        return kIOReturnSuccess;
    }

    res->nent           = nent;
    res->bufBytes       = bufBytes;
    res->byteCountSent  = bufBytes - MLX4_NET_IP_ALIGN;
    res->ipAlign        = MLX4_NET_IP_ALIGN;
    res->wqeSize        = RX_WQE_SIZE;
    res->ringHeaderBytes = RX_RING_HEADER_SIZE;
    res->ringPages      = pages;
    res->ringBytes      = RX_RING_HEADER_SIZE + nent * RX_WQE_SIZE;
    res->mttOffset      = mttOffset;
    res->mttAddr        = mttAddr;
    res->lkey           = fMrLkey;
    res->lkeyRawIndex   = fMrMptIndex;
    res->bufPhysFirst   = bufPhysFirst;
    /*
     * The last buffer's address, from the page list for the same reason the WQE check
     * below does it that way: the allocation is scattered, so this is NOT
     * first + (n-1)*size.  The contiguous formula would have printed a wrong range while
     * the descriptors were right.
     */
    {
        res->bufPhysLast = fHw.rqBufferPhys(bufs,bufBytes,bufPhysCount-1u);
    }
    res->ringPhysFirst  = ringPhysFirst;
    res->ringPhysCount  = ringPhysCount;
    res->bufsInOneAlloc = 1;
    /*
     * The Ethernet reference sets the packed mtu/message-max field to 0xff
     * (mlx4_en_resources.c:51), including for jumbo. It is not a byte count
     * of 255*16. Actual frame capacity is bounded by SET_PORT and RQ WQEs.
     */
    res->mtu_msgmax = 0xff;

    /*
     * Verify rather than assume: read WQE 0 and the LAST WQE back out of the ring.  An
     * off-by-one in the address step is invisible in WQE 0 and would show up as frames
     * landing in the wrong buffer -- or nowhere.
     */
    w = (const UInt8 *)ring->virt + RX_RING_HEADER_SIZE;
    {
        UInt32 bc = 0, lk = 0;
        UInt64 ad = 0;
        for (k = 0; k < 4; ++k) bc = (bc << 8) | w[RX_WQE_BYTE_COUNT_OFFSET + k];
        for (k = 0; k < 4; ++k) lk = (lk << 8) | w[RX_WQE_LKEY_OFFSET + k];
        for (k = 0; k < 8; ++k) ad = (ad << 8) | (UInt64)w[RX_WQE_ADDR_OFFSET + k];
        res->wqe0ByteCount = bc; res->wqe0Lkey = lk; res->wqe0Addr = ad;
        res->wqe0Matches = (bc == bufBytes - MLX4_NET_IP_ALIGN && lk == fMrLkey &&
                            ad == bufPhysFirst) ? 1 : 0;
    }
    {
        const UInt8 *wl = w + (UInt64)(nent - 1) * RX_WQE_SIZE;
        UInt32 bc = 0, lk = 0;
        UInt64 ad = 0;
        /*
         * The expected address comes from the PAGE LIST, not from first + (n-1)*bufBytes.
         *
         * The first version assumed contiguity and was wrong on its first run: the buffer
         * allocation is 128 pages in 32 segments -- scattered -- so buffer i's bus address
         * is its own page's plus an offset and nothing more.  The WQE writing already went
         * through the page list; only this check assumed contiguity, and the mismatch it
         * printed was the check being right about the wrong thing.
         */
        UInt64 want = fHw.rqBufferPhys(bufs,bufBytes,nent-1u);
        for (k = 0; k < 4; ++k) bc = (bc << 8) | wl[RX_WQE_BYTE_COUNT_OFFSET + k];
        for (k = 0; k < 4; ++k) lk = (lk << 8) | wl[RX_WQE_LKEY_OFFSET + k];
        for (k = 0; k < 8; ++k) ad = (ad << 8) | (UInt64)wl[RX_WQE_ADDR_OFFSET + k];
        res->wqeLastByteCount = bc; res->wqeLastLkey = lk; res->wqeLastAddr = ad;
        res->wqeLastMatches = (bc == bufBytes - MLX4_NET_IP_ALIGN && lk == fMrLkey &&
                               ad == want) ? 1 : 0;
    }
    LOG("r4rq: WQE 0: byte_count %u, lkey 0x%08x, addr 0x%llx (%s); WQE %u: byte_count %u, "
        "lkey 0x%08x, addr 0x%llx (%s)",
        (unsigned)res->wqe0ByteCount, (unsigned)res->wqe0Lkey,
        (unsigned long long)res->wqe0Addr, res->wqe0Matches ? "matches" : "MISMATCH",
        (unsigned)(nent - 1), (unsigned)res->wqeLastByteCount, (unsigned)res->wqeLastLkey,
        (unsigned long long)res->wqeLastAddr,
        res->wqeLastMatches ? "matches" : "MISMATCH");

    queue.rqRing     = ring;
    queue.rqBufs     = bufs;
    queue.rqNent     = nent;
    queue.rqBufBytes = bufBytes;
    queue.rqPages    = pages;
    queue.rqMttOff   = mttOffset;
    queue.rqMttPages = pages;
    queue.rqMttAddr  = mttAddr;
    queue.rqLkey     = fMrLkey;
    queue.rqBufPhys  = bufPhysFirst;
    queue.rqProd     = 0;
    queue.rqUp       = true;

    res->status = 0;
    out[kR4RqOutStatus] = 0;
    LOG("r4rq OK: %u WQEs of %u bytes at ring+%u, buffers %u bytes each from 0x%llx to "
        "0x%llx, lkey 0x%08x (the TRANSFORMED key -- the raw index is %u), ring %u pages "
        "MTT offset %u (0x%x). NOT handed to the hardware yet: that is stage 4.",
        (unsigned)nent, (unsigned)RX_WQE_SIZE, (unsigned)RX_RING_HEADER_SIZE,
        (unsigned)bufBytes, (unsigned long long)res->bufPhysFirst,
        (unsigned long long)res->bufPhysLast, (unsigned)fMrLkey,
        (unsigned)fMrMptIndex, (unsigned)pages, (unsigned)mttOffset, (unsigned)mttAddr);
    return kIOReturnSuccess;
}

/*
 * Release the receive ring's memory.
 *
 * Nothing hands anything back to the hardware, because stage 3 never gave it anything: the
 * ring and the buffers are only reachable through the QP, and no QP exists yet.  Once
 * stage 4 enables the WQEs this changes -- at that point the queue has to be handed back
 * first, exactly as the EQ and the CQ do, and this function will need that branch.
 */
void Cx3Mlx4::rqTeardown(const char *why, bool hcaWasReset, UInt32 port)
{
    if (port<1 || port>2) return;
    Cx3PortQueues &queue = fPortQueues[port-1];
    queue.setupReady=false;

    if (!queue.rqUp && !queue.rqUnknownRetained) return;
    (void)hcaWasReset;
    /*
     * Update 2026-10-12 (stage 4): this function's old log line said "nothing was handed to
     * the hardware -- stage 3 only builds memory", and that was true until stage 4 ran.  It
     * is not true any more once a QP exists: the QP context names these MTT entries, so
     * releasing them under a live QP is the same mistake teardownEq and cqTeardown refuse to
     * make.  The QP is therefore handed back first, by qpTeardown(), and that call is
     * deliberately OUTSIDE this one (both callers run it before this) rather than inside, so
     * that "the ring is being released" and "the QP was taken back" stay two separate
     * statements in the log.
     */
    if (queue.qpUp || queue.qpRec) {
        /*
         * Reached only if a caller skipped qpTeardown().  Say so loudly rather than release
         * memory a live QP still points at: this is the one path where being conservative
         * costs a leak instead of a device writing into recycled pages.
         */
        LOG("RQ teardown (%s): a QP (QPN %u) is still up and its context names this ring's "
            "MTT entries, so the ring and the buffers are NOT released. qpTeardown has to run "
            "first; a reboot reclaims the memory.", why, (unsigned)queue.qpQpn);
        return;
    }
    LOG("RQ teardown (%s): releasing %u pages of WQE ring and the buffer allocation -- %s",
        why, (unsigned)queue.rqPages,
        hcaWasReset ? "retained read-only, not freed" : "freed");
    if (!hcaWasReset && queue.rqMttPages) {
        if (!fHw.mttFree(queue.rqMttOff, queue.rqMttPages, queue.rqRing)) {
            LOG("RQ teardown (%s): MTT release refused; preserving ring/buffers and MTT owner metadata",
                why);
            return;
        }
        queue.rqMttOff = queue.rqMttAddr = queue.rqMttPages = 0;
    }
    if (!releaseIcmOrKeep(&queue.rqBufs, "RX frame buffers", hcaWasReset) ||
        !releaseIcmOrKeep(&queue.rqRing, "RX WQE ring", hcaWasReset)) return;
    if (hcaWasReset) {
        LOG("RQ teardown (%s): reset alone is not an MTT release credential; retaining "
            "indices %u + %u for this table lifetime", why, (unsigned)queue.rqMttOff,
            (unsigned)queue.rqPages);
    }
    queue.rqUp = false;
    queue.rqUnknownRetained = false;
    queue.rqNent = 0;
    queue.rqPages = 0;
    queue.rqMttOff = 0;
    queue.rqMttPages = 0;
    queue.rqMttAddr = 0;
    /*
     * R4 stage 9's view of the ring goes with it: the WQEs it re-posted, the producer count it
     * published and the per-slot bookkeeping all describe memory that no longer exists, so a
     * later r4raw must seed them again from the new ring rather than continue counting.
     */
    queue.rawRxProd = 0;
    queue.rawRxCons = 0;
    queue.rawRxRecyclesTotal = 0;
    memset(queue.rawRxRecycles, 0, sizeof(queue.rawRxRecycles));
    memset(queue.rawRxInFlight, 0, sizeof(queue.rawRxInFlight));
}

/*
 * Put the RX QP back and release its producer record.
 *
 * Same shape and same reasoning as cqTeardown, with one difference that is specific to a
 * QP: there is no single "hand it back" command.  A QP is taken away by moving it to RST
 * (2RST_QP, mlx4_qp.c:145-147), and that is the only state in which the hardware stops
 * referring to the context's MTT entries and its db_rec_addr -- which is what makes this
 * the prerequisite for rqTeardown releasing the ring.
 *
 * A failure here does NOT release the record and does not clear the state, for the reason
 * mrTeardown keeps an MPT slot it could not take back: an object the hardware may still own
 * is worth leaking on the way out, and reusing its memory is not.
 */
void Cx3Mlx4::qpTeardown(const char *why, bool hcaWasReset, UInt32 port)
{
    if (port<1 || port>2) return;
    Cx3PortQueues &queue = fPortQueues[port-1];
    queue.setupReady=false;

    UInt32 status = CX3_FWSTATUS_NOT_POSTED;

    if (!queue.qpUp && !queue.qpRec) return;

    /* Beginning teardown ends readiness, not ownership. Retain the record
     * owner until 2RST and the release/retention transaction both succeed. */
    if (port==1) fNetEnabled = false;
    queue.qpUp = false;
    queue.qpState = CX3_R4QP_NOT_MEASURED;

    if (hcaWasReset) {
        LOG("QP teardown (%s): the HCA was reset, so it has forgotten QPN %u and there is "
            "nothing to put back; dropping the host-side state and the producer record",
            why, (unsigned)queue.qpQpn);
    } else if (!fHw.qpToReset(queue.qpQpn, &status)) {
        LOG("QP teardown (%s): 2RST_QP on QPN %u failed (fw status 0x%x) -- the QP may still "
            "be in RTS and its context still names the ring's MTT entries and the producer "
            "record, so neither is released and the owner is kept, without readiness. "
            "rqTeardown will refuse to release the ring while this record owner remains: a leak "
            "that a reboot reclaims, rather than a device writing into recycled memory.",
            why, (unsigned)queue.qpQpn, (unsigned)status);
        return;
    }

    if (!releaseIcmOrKeep(&queue.qpRec, "RX QP producer record", hcaWasReset)) return;
    queue.qpRecPhys = 0;
    queue.qpProd    = 0;
    queue.qpState   = MLX4_QP_STATE_RST;
    queue.qpUp      = false;
    LOG("QP teardown (%s): QPN %u is back in RST and the producer record is released",
        why, (unsigned)queue.qpQpn);
}

/*
 * Put the indirection QP back, and drop the MAC registration's host-side record.
 *
 * The QP half is identical in shape to qpTeardown and for the same reason: a QP is taken away
 * by moving it to RST, and that is the only state in which the firmware stops following the
 * MTT entry and the db_rec_addr in its context.  The difference worth stating is what this QP
 * does NOT hold: it names the RX QP only as a NUMBER (base_qpn/default_qpn are QPNs, not
 * addresses), so releasing the indirection QP is not a prerequisite for releasing the ring --
 * unlike qpTeardown, which owns the MTT entries of the ring's memory.
 *
 * The MAC half has no "undo" command at all.  The reference's removal path re-sends the whole
 * table with the entry cleared (mlx4_port.c:415-430), which is the same shape as registering
 * it, so "unregistering" here would mean sending another 1024-byte command.  This port does
 * not: after an HCA reset the firmware has forgotten the table, and on the way out of the
 * driver a command that only removes a filter costs a failure mode (what if it is refused?)
 * for no benefit.  The host-side record is dropped either way, and the log says which of the
 * two happened rather than implying the device was cleaned up.
 */
void Cx3Mlx4::ingressTeardown(const char *why, bool hcaWasReset, UInt32 port)
{
    if (port<1 || port>2) return;
    Cx3PortQueues &queue = fPortQueues[port-1];
    queue.setupReady=false;

    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    const bool hadQpOwner = queue.ingressUp || queue.ingressRec;

    if (!hadQpOwner && !queue.macUp) return;
    if (port==1) fNetEnabled = false;
    queue.ingressUp = false;

    if (hadQpOwner) {
        queue.ingressState = CX3_R4INGRESS_NOT_MEASURED;
        if (hcaWasReset) {
            LOG("ingress teardown (%s): the HCA was reset, so it has forgotten the "
                "indirection QPN %u and there is nothing to put back; dropping the host-side "
                "state and the producer record", why, (unsigned)queue.ingressQpn);
        } else if (!fHw.qpToReset(queue.ingressQpn, &status)) {
            LOG("ingress teardown (%s): 2RST_QP on the indirection QPN %u failed (fw status "
                "0x%x) -- the QP may still be in RTS and its context still names the producer "
                "record, so neither is released and the state is kept: a leak a reboot "
                "reclaims, rather than a device writing into recycled memory",
                why, (unsigned)queue.ingressQpn, (unsigned)status);
            return;
        }
        if (!releaseIcmOrKeep(&queue.ingressRec, "indirection QP producer record", hcaWasReset)) return;
        queue.ingressRecPhys = 0;
        queue.ingressProd    = 0;
        queue.ingressState   = MLX4_QP_STATE_RST;
        queue.ingressUp      = false;
        LOG("ingress teardown (%s): the indirection QPN %u is back in RST and its producer "
            "record is released", why, (unsigned)queue.ingressQpn);
    }
    if (queue.macUp) {
        LOG("ingress teardown (%s): the MAC registration (slot %u, %02x:%02x:%02x:%02x:%02x:"
            "%02x) is dropped from the driver's state only -- the reference has no "
            "unregister command (%s), so no command is sent here; %s",
            why, (unsigned)queue.macSlot, queue.mac[2], queue.mac[3], queue.mac[4], queue.mac[5],
            queue.mac[6], queue.mac[7],
            "mlx4_port.c:415-430 re-sends the whole table instead",
            hcaWasReset ? "the HCA was reset, so the firmware has already forgotten it"
                        : "the firmware's table keeps it until the next reset, which is the "
                          "state the reference leaves behind too");
        queue.macUp = false;
        queue.macSlot = 0;
        memset(queue.mac, 0, sizeof(queue.mac));
        queue.macValue = 0;
    }
}

/*
 * Take the transmit side apart: the send QP first, then everything it names.
 *
 * The order is the whole content of this function, and it is the same relation qpTeardown and
 * rqTeardown have: a QP in RTS has the SQ's MTT address and the db_rec_addr record inside its
 * context, so the hardware is still reading both.  Moving the QP to RST is the only command
 * that stops that (there is no HW2SW_QP), and everything is released only if it succeeded --
 * otherwise the memory is deliberately kept and the log says so, because a leak a reboot
 * reclaims is the cheaper failure.
 *
 * The TX CQ comes back through HW2SW_CQ afterwards, with cqTeardown's rule: a queue the
 * firmware may still own must not have its ring freed underneath it, and a failed hand-back
 * means the ring, the record and the MTT entries are all kept.
 */
void Cx3Mlx4::txTeardown(const char *why, bool hcaWasReset, UInt32 port)
{
    if (port<1 || port>2) return;
    Cx3PortQueues &queue = fPortQueues[port-1];
    queue.setupReady=false;

    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    bool hadQpOwner = queue.txQpUp || queue.txQpRec;
    queue.txQpUp = false;  /* Stop submission even if ownership return fails. */

    /*
     * The QP first, and everything it names only if that worked.
     *
     * A QP in RTS has the SQ's MTT address and the db_rec_addr record inside its context, so
     * the hardware is still following both.  Moving the QP to RST is the only command that
     * stops that (there is no HW2SW_QP), so a failure here releases NOTHING and keeps the
     * state for a later attempt: a leak a reboot reclaims, rather than a device writing into
     * recycled memory.
     *
     * Two things this function has to survive, and both are reachable: the QP never got built
     * (an earlier step of exportR4Tx refused, so the SQ and the CQ exist with no QP) and the
     * QP was built (the normal case).  The release below therefore does not sit inside the
     * QP branch -- with no QP, nothing names the memory and it must still be given back.
     */
    if (hadQpOwner) {
        if (hcaWasReset) {
            queue.txQpReturned = true;
            LOG("TX teardown (%s): the HCA was reset, so it has forgotten QPN %u and there is "
                "nothing to put back; dropping the host-side state, the send queue's memory "
                "and its record", why, (unsigned)queue.txQpn);
        } else if (!queue.txQpReturned && !fHw.qpToReset(queue.txQpn, &status)) {
            LOG("TX teardown (%s): 2RST_QP on QPN %u failed (fw status 0x%x) -- the QP may "
                "still be in RTS and its context still names the send queue's MTT entries and "
                "the db_rec_addr record, so NONE of that is released and the state is kept; a "
                "reboot reclaims it", why, (unsigned)queue.txQpn, (unsigned)status);
            return;
        } else {
            queue.txQpReturned = true;
        }
    }
    if (!hcaWasReset && queue.txSqMttPages) {
        if (!fHw.mttFree(queue.txSqMttOff, queue.txSqMttPages, queue.txSq)) {
            LOG("TX teardown (%s): SQ MTT release refused; preserving QP/SQ owner metadata",
                why);
            return;
        }
        queue.txSqMttOff = queue.txSqMttPages = queue.txSqMttAddr = 0;
    }
    if (!releaseIcmOrKeep(&queue.txQpRec, "TX QP producer record", hcaWasReset) ||
        !releaseIcmOrKeep(&queue.txSq, "TX send queue", hcaWasReset)) return;
    /*
     * ...and stage 9's frame buffer, which is released here rather than in a teardown of its
     * own: no WQE can name it any more once the QP is out of RTS (or the HCA has forgotten
     * everything), which is exactly the condition the two lines above rely on for the send
     * queue's own memory.
     */
    if (!releaseIcmOrKeep(&queue.rawFrame, "TX raw frame buffer", hcaWasReset)) return;
    queue.txQpState = MLX4_QP_STATE_RST;
    queue.txQpUp = false;
    queue.rawFramePhys  = 0;
    queue.rawFrameBytes = 0;
    /* The producer count and the TX CQ's consumer index belong to the objects being released:
     * a new send queue starts at TXBB 0 (first lap, owner bit 0) and at the first CQE. */
    queue.rawTxProd = 0;
    queue.rawTxCons = 0;
    if (hcaWasReset) {
        LOG("TX teardown (%s): reset alone is not an MTT release credential; retaining "
            "SQ indices %u + %u for this table lifetime", why,
            (unsigned)queue.txSqMttOff, (unsigned)queue.txSqPages);
    }
    queue.txQpRecPhys = 0;
    queue.txSqMttOff  = 0;
    queue.txSqMttPages = 0;
    queue.txSqMttAddr = 0;
    queue.txSqUnknownRetained = false;
    queue.txSqPages   = 0;
    queue.txSqPhys    = 0;
    queue.txQpReturned = false;
    LOG("TX teardown (%s): the send queue's memory and its record are released; %s", why,
        hadQpOwner ? "QPN was put back to RST first, so nothing in the device still follows them"
                : "there was no QP in RTS to put back (an earlier step of r4tx refused, so the "
                  "memory was never named by a context)");
    if (queue.txCqUp) {
        if (hcaWasReset) {
            queue.txCqHwReturned = true;
            LOG("TX CQ teardown (%s): the HCA was reset, so it has forgotten CQN %u and there "
                "is nothing to hand back; dropping the host-side state", why,
                (unsigned)queue.txCqn);
        } else if (!queue.txCqHwReturned && !fHw.cqHw2Sw(queue.txCqn, &status)) {
            LOG("TX CQ teardown (%s): HW2SW_CQ %u failed (fw status 0x%x) -- the queue may "
                "still be hardware-owned, so its ring, its record and its MTT entries are all "
                "deliberately kept and nothing it names is released. A reboot reclaims them.",
                why, (unsigned)queue.txCqn, (unsigned)status);
            return;
        } else {
            queue.txCqHwReturned = true;
        }
        if (!hcaWasReset && queue.txCqMttPages) {
            if (!fHw.mttFree(queue.txCqMttOff, queue.txCqMttPages, queue.txCqRing)) {
                LOG("TX CQ teardown (%s): MTT release refused; preserving CQ ring/record owner",
                    why);
                return;
            }
            queue.txCqMttOff = queue.txCqMttPages = 0;
        }
        if (!releaseIcmOrKeep(&queue.txCqRing, "TX CQ ring", hcaWasReset) ||
            !releaseIcmOrKeep(&queue.txCqRec, "TX CQ doorbell record", hcaWasReset)) return;
        if (hcaWasReset) {
            LOG("TX CQ teardown (%s): reset alone is not an MTT release credential; "
                "retaining indices %u + %u for this table lifetime", why,
                (unsigned)queue.txCqMttOff, (unsigned)queue.txCqMttPages);
        }
        queue.txCqUp = false;
        queue.txCqHwReturned = false;
        queue.txCqNent = 0;
        queue.txCqMttOff = 0;
        queue.txCqMttPages = 0;
        LOG("TX CQ teardown (%s): CQN %u given back and the ring released", why,
            (unsigned)queue.txCqn);
    }
}


/* ==================================================================== */
/* R4 stage 2: the RX completion queue                                  */
/* ==================================================================== */

/*
 * kCx3SelR4Cq -- create one CQ and hand it to the hardware.
 *
 * Deliberately poll-only for now, and deliberately without arming: the point of this
 * stage is to prove that a CQ can be created, that its context carries the values it
 * should, and that the ring's owner preset actually covered every slot.  Arming and the
 * interrupt path come after, for the same reason the EQ work did them in that order.
 *
 * The owner scan is the part worth reading twice.  A slot whose owner byte is left at
 * zero reads as ready under the phase rule on the first lap, so a preset that covered
 * only part of the ring would not fail -- it would produce completions out of memory
 * nobody wrote.  The scan counts them, so "the preset covered all of it" is checked
 * rather than inferred from the loop that was supposed to do it.
 */
IOReturn Cx3Mlx4::exportR4Cq(const UInt64 *in, Cx3R4CqResult *res, UInt64 *out, UInt32 port)
{
    if (port<1 || port>2 || !res || !out || !in) return kIOReturnBadArgument;
    for (UInt32 i=0;i<kR4CqInCount;++i)
        if (in[i]>0x7fffffffull) return kIOReturnBadArgument;
    Cx3PortQueues &queue = fPortQueues[port-1];
    if ((port==2 && !queue.numbersReserved) || queue.setupFailed ||
        (queue.numbersReserved && queue.numberGeneration!=fDeviceGeneration)) {
        memset(res,0,sizeof(*res)); res->status=(UInt32)kIOReturnNotReady;
        memset(out,0,sizeof(UInt64)*kR4CqOutCount);
        out[0]=(UInt64)kIOReturnNotReady; return kIOReturnSuccess;
    }

    UInt32 nent = (UInt32)in[kR4CqInNent];
    UInt32 cqn  = (UInt32)in[kR4CqInCqn];
    UInt32 fwStatus = CX3_FWSTATUS_NOT_POSTED;
    UInt32 pages = 0, mttOffset = 0, mttAddr = 0;
    UInt64 recPhys = 0;
    Mlx4Icm *rec = 0;     /* the CI record: the hardware writes into it too */
    Mlx4Icm *ring = 0;
    UInt32 i;
    UInt8  cqeFactor = 1;   /* the CQE size factor: 0 = 32-byte CQEs, 1 = 64 */
    UInt32 stride;
    const UInt8 *r;

    memset(out, 0, sizeof(UInt64) * kR4CqOutCount);
    memset(res, 0, sizeof(*res));
    res->status   = (UInt32)kIOReturnError;
    res->fwStatus = CX3_FWSTATUS_NOT_POSTED;
    res->firstNotPresetSlot = 0xffffffffu;

    if (nent == 0) nent = CX3_R4CQ_DEFAULT_NENT;
    if (queue.numbersReserved) {
        if (in[kR4CqInCqn] && in[kR4CqInCqn]!=queue.numbers.rxCqn) {
            res->status=(UInt32)kIOReturnBadArgument; out[0]=res->status; return kIOReturnSuccess;
        }
        cqn=queue.numbers.rxCqn;
    } else if (cqn == 0) cqn = CX3_R4CQ_DEFAULT_CQN;

    if (fEqUnknownRetained) {
        LOG("r4cq refused: the EQ owner is retained but unconfirmed; do not create a CQ "
            "against an EQ that is not safe to consume");
        res->status = (UInt32)kIOReturnNotReady;
        res->fwStatus = CX3_FWSTATUS_TIMEOUT;
        out[kR4CqOutStatus] = res->status;
        return kIOReturnSuccess;
    }

    /*
     * Idempotence, for the same reason r4mr has it, and this one would be worse: a
     * second call would allocate a second ring and record and re-send SW2HW_CQ for a CQN
     * the hardware already owns.  Stage 1's review found exactly this pattern and called
     * it out, so it is written in from the start here rather than after the fact.
     */
    if (queue.cqUp) {
        /*
         * Report the queue that exists -- and MEASURE it, rather than filling in the
         * fields from stored state.
         *
         * The first version of this path returned the numbers it had kept and left the
         * owner histogram and the readiness count at zero, so the tool printed "owner
         * byte at slot + 0" and a row of zeros that looked like measurements.  That is
         * the same mistake this project has made more than once in other forms -- a
         * field that was never written being read as a live result -- and the fix is the
         * same: if a number cannot be measured on this path, do not report it.  Here it
         * can be measured, because the ring is still there.
         */
        UInt8  cqeFactorIdem = 1;
        UInt32 strideIdem = CQE_LEGACY_SIZE << cqeFactorIdem;
        const UInt8 *rIdem = (const UInt8 *)queue.cqRing->virt;
        UInt8  cqe[CQE_LEGACY_SIZE];
        UInt32 ci = 0, ready = 0;

        res->cqn        = queue.cqCqn;
        res->nent       = queue.cqNent;
        res->pages      = queue.cqMttPages;
        res->mttOffset  = queue.cqMttOff;
        res->uarIndex   = queue.cqUarStart;
        res->hwUarIndex = mlx4ToHwUarIndex(queue.cqUarStart);
        res->uarPageOffset  = mlx4UarPageOffset(queue.cqUarStart);
        res->doorbellOffset = res->uarPageOffset + MLX4_CQ_DOORBELL;
        res->dbRecordPhys   = queue.cqRecPhys;
        res->logsizeUsrpageSent = (mlx4Log2(queue.cqNent) << 24) | res->hwUarIndex;
        res->eqnSent    = fEqn;
        res->fwStatus   = 0;
        res->ownerByteOffsetInSlot = CQE_SLOT_OWNER_BYTE(cqeFactorIdem);

        for (i = 0; i < queue.cqNent; ++i) {
            UInt8 v = rIdem[(UInt64)i * strideIdem + CQE_SLOT_OWNER_BYTE(cqeFactorIdem)];
            if (v == 0x80) ++res->ownerAfter[0]; else ++res->ownerAfter[1];
            if ((v & MLX4_CQE_OWNER_MASK) == MLX4_CQE_OWNER_MASK)
                ++res->ownerPresetCount;
            else {
                ++res->ownerNotPresetCount;
                if (res->firstNotPresetSlot == 0xffffffffu) res->firstNotPresetSlot = i;
            }
        }
        memcpy(res->slot0First,  rIdem, 16);
        memcpy(res->slot0Second, rIdem + CQE_LEGACY_SIZE, 16);

        while (ready < queue.cqNent) {
            memset(cqe, 0, sizeof(cqe));
            if (!fHw.cqNextCqe(queue.cqRing, queue.cqNent, cqeFactorIdem, ci, cqe)) break;
            ++ready; ++ci;
        }
        res->readyAtStart = ready;

        res->status     = 0;
        out[kR4CqOutStatus] = 0;
        LOG("r4cq: a queue already exists (CQN %u, %u entries, record 0x%llx); measured it "
            "rather than creating a second one: owner AFTER 0x80 x%u, %u readable slot(s) "
            "from cons_index %u",
            (unsigned)queue.cqCqn, (unsigned)queue.cqNent,
            (unsigned long long)queue.cqRecPhys, (unsigned)res->ownerAfter[0],
            (unsigned)ready, (unsigned)queue.cqConsIndex);
        return kIOReturnSuccess;
    }
    if (queue.cqUnknownRetained || queue.cqRing || queue.cqRec) {
        LOG("r4cq refused: CQN %u has a retained but unconfirmed owner; retry teardown "
            "before creating or consuming another CQ", (unsigned)queue.cqCqn);
        res->status = (UInt32)kIOReturnNotReady;
        res->fwStatus = queue.cqUnknownRetained ? CX3_FWSTATUS_TIMEOUT : CX3_FWSTATUS_NOT_POSTED;
        out[kR4CqOutStatus] = res->status;
        return kIOReturnSuccess;
    }

    /*
     * Two preconditions, both of which would otherwise be discovered by a CQ that never
     * produces anything: the HCA has to be up, and the EQ the completions are routed to
     * has to exist.
     */
    if (!fHcaInited) {
        LOG("r4cq refused: the HCA is not initialised (run m2dprofile/m2dbringup first)");
        res->status = (UInt32)kIOReturnNotReady;
        out[kR4CqOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if (!fEqRing) {
        LOG("r4cq refused: no EQ is up, so comp_eqn would name a queue that does not "
            "exist. Run m3equp first.");
        res->status = (UInt32)kIOReturnNotReady;
        out[kR4CqOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if (cqn < fDevCap.reserved_cqs) {
        LOG("r4cq refused: CQN %u is inside the firmware's reserved range (0..%u)",
            (unsigned)cqn, (unsigned)(fDevCap.reserved_cqs - 1));
        res->status = (UInt32)kIOReturnBadArgument;
        out[kR4CqOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if (cqn >= (1u << fInitHca.log_num_cqs)) {
        LOG("r4cq refused: CQN %u is at or past the %u CQs this profile configured "
            "(the card's ceiling is a different number and not what was configured)",
            (unsigned)cqn, (unsigned)(1u << fInitHca.log_num_cqs));
        res->status = (UInt32)kIOReturnBadArgument;
        out[kR4CqOutStatus] = res->status;
        return kIOReturnSuccess;
    }

    /*
     * The UAR, derived from the device rather than written down.
     *
     * This used the constant 128.  It happens to be right -- the rule is
     * max(128, devcap/reserved) at this card's UAR page size, and 128 wins -- but a
     * constant cannot notice the device saying something else, and the value is computed
     * from two fields this port already reads back (INIT_HCA's uar_page_sz and
     * QUERY_DEV_CAP's reserved_uars).  reservedUarStart() does the arithmetic and logs
     * both of its inputs, so a disagreement is visible instead of silent.
     *
     * Getting this wrong is the expensive kind: a CQ in the EQ doorbells' range would sit
     * on the doorbell that already works, and the symptom would be a queue whose
     * completions never arrive.
     */
    queue.cqUarStart = reservedUarStart();
    /* fEqn is a UInt32 field on purpose (EQ numbers are not architecturally bytes), and the
     * cast says the value being handed to the context is one that fits.  Silent narrowing
     * is what -Wimplicit-int-conversion is asked to refuse. */
    if (!fHw.cqCreate(cqn, nent, (UInt8)fEqn, queue.cqUarStart, &ring, &pages, &mttOffset,
                      &mttAddr, &recPhys, &rec, res->ownerBefore,
                      res->ownerAfter, &fwStatus)) {
        res->fwStatus = fwStatus;
        if (ring && rec && pages != 0u) {
            queue.cqRing = ring;
            queue.cqRec = rec;
            queue.cqRecPhys = recPhys;
            queue.cqCqn = cqn;
            queue.cqNent = nent;
            queue.cqMttOff = mttOffset;
            queue.cqMttPages = pages;
            queue.cqHwReturned = false;
            queue.cqMayBeDeviceOwned = fwStatus == CX3_FWSTATUS_TIMEOUT;
            queue.cqConsIndex = 0;
            queue.cqUnknownRetained = true;
            res->cqn = cqn;
            res->nent = nent;
            res->pages = pages;
            res->mttOffset = mttOffset;
            res->mttAddr = mttAddr;
            res->status = (UInt32)kIOReturnNotReady;
            LOG("r4cq: create failed with retained ring/record/MTT owner (fw status 0x%x, "
                "may_be_device_owned=%u); do not consume or recreate before teardown",
                (unsigned)fwStatus, (unsigned)queue.cqMayBeDeviceOwned);
        }
        out[kR4CqOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    res->fwStatus  = fwStatus;
    res->cqn       = cqn;
    res->nent      = nent;
    res->pages     = pages;
    res->mttOffset = mttOffset;
    res->mttAddr   = mttAddr;
    res->uarIndex  = queue.cqUarStart;
    res->hwUarIndex = mlx4ToHwUarIndex(queue.cqUarStart);
    res->uarPageOffset  = mlx4UarPageOffset(queue.cqUarStart);
    res->doorbellOffset = res->uarPageOffset + MLX4_CQ_DOORBELL;
    res->dbRecordPhys   = recPhys;
    res->logsizeUsrpageSent = (mlx4Log2(nent) << 24) | res->hwUarIndex;
    res->eqnSent = fEqn;

    /*
     * The owner scan.  Read every slot's owner byte out of the ring the hardware was just
     * given and count how many hold 0x80.  This is the check for the defect that would
     * otherwise show up as a completion read out of zeroed memory.
     */
    stride = CQE_LEGACY_SIZE << cqeFactor;
    r = (const UInt8 *)ring->virt;
    for (i = 0; i < nent; ++i) {
        UInt8 owner = r[(UInt64)i * stride + CQE_SLOT_OWNER_BYTE(cqeFactor)];
        if ((owner & MLX4_CQE_OWNER_MASK) == MLX4_CQE_OWNER_MASK) {
            ++res->ownerPresetCount;
        } else {
            ++res->ownerNotPresetCount;
            if (res->firstNotPresetSlot == 0xffffffffu) res->firstNotPresetSlot = i;
        }
    }
    memcpy(res->slot0First,  r, 16);
    memcpy(res->slot0Second, r + CQE_LEGACY_SIZE, 16);
    res->ownerByteOffsetInSlot = CQE_SLOT_OWNER_BYTE(cqeFactor);

    /*
     * And now the check that stage 3 depends on: run the consumer's own readiness rule
     * over the ring without consuming, and count what it finds.  Zero is the only correct
     * answer for a queue that was just created.
     */
    {
        UInt8 cqe[CQE_LEGACY_SIZE];
        UInt32 ci = 0, ready = 0;
        while (ready < nent) {
            memset(cqe, 0, sizeof(cqe));
            if (!fHw.cqNextCqe(ring, nent, cqeFactor, ci, cqe)) break;
            ++ready;
            ++ci;
        }
        res->readyAtStart = ready;
        LOG("r4cq: the consumer's readiness rule finds %u readable slot(s) at "
            "cons_index 0 on the freshly created queue%s", (unsigned)ready,
            ready ? " -- EXPECTED 0: a queue with no completions must read as empty"
                  : " (correct: an empty queue reads as empty)");
    }
    LOG("r4cq: owner preset scan: %u of %u slots hold 0x80%s",
        (unsigned)res->ownerPresetCount, (unsigned)nent,
        res->ownerNotPresetCount
            ? " -- NOT all of them, which would have produced completions out of "
              "zeroed memory"
            : " (every slot, so nothing in the ring can be mistaken for a completion)");

    /* Keep the ring and the record alive: later stages need them, and the hardware now
     * refers to both. Stated here because this is the first selector that leaves a QUEUE
     * behind rather than just numbers. */
    queue.cqRing    = ring;
    queue.cqNent    = nent;
    queue.cqCqn     = cqn;
    queue.cqMttOff  = mttOffset;
    queue.cqMttPages = pages;
    queue.cqHwReturned = false;
    queue.cqMayBeDeviceOwned = true;
    queue.cqRec = rec;
    queue.cqRecPhys = recPhys;
    queue.cqConsIndex = 0;
    queue.cqUp      = true;
    queue.cqUnknownRetained = false;

    res->status = 0;
    out[kR4CqOutStatus] = 0;
    LOG("r4cq OK: CQN %u, %u entries, %u pages, mtt offset %u (0x%x), record 0x%llx, "
        "uar index %u -> hw %u, doorbell BAR2+0x%llx",
        (unsigned)cqn, (unsigned)nent, (unsigned)pages, (unsigned)mttOffset,
        (unsigned)mttAddr, (unsigned long long)recPhys, (unsigned)queue.cqUarStart,
        (unsigned)res->hwUarIndex, (unsigned long long)res->doorbellOffset);
    return kIOReturnSuccess;
}


/* ==================================================================== */
/* R4 stage 4: the RX QP, three steps to RTS, and the producer publish  */
/* ==================================================================== */

/*
 * Big-endian reads, local to this file.
 *
 * Mlx4Hw.cpp has its own pair of these as static inlines; they are not shared through a
 * header because every context layout in mlx4_regs.h is written as offsets into a byte
 * buffer, and a shared helper would invite the belief that a struct could be read instead.
 * These two exist for the one place here that decodes a context the firmware wrote.
 */
static inline UInt32 cx3GetBe32(const void *p)
{
    return OSSwapBigToHostInt32(*(const UInt32 *)p);
}
/* The 16-bit member of the family, for the CQE's status / checksum / wqe_index words and the
 * WQE's vlan_tag -- all big-endian 16-bit fields whose width is part of their meaning. */
static inline UInt16 cx3GetBe16(const void *p)
{
    return OSSwapBigToHostInt16(*(const UInt16 *)p);
}
static inline UInt64 cx3GetBe64(const void *p)
{
    return OSSwapBigToHostInt64(*(const UInt64 *)p);
}

/*
 * The comparison half of the stage, shared by the fresh path and the idempotent one.
 *
 * It runs QUERY_QP and puts the answer next to what the caller wrote, field by field,
 * because one read is answering two questions at once and they must not be merged:
 *
 *   local_qpn        -- is this the QP we wrote?  mlx4_qp_modify sets it from its own
 *                       argument (mlx4_qp.c:180-181), so a QP somebody else's driver made
 *                       would say so here rather than merely failing to answer.
 *   the queue fields -- did the firmware KEEP what was sent?  A command that returns fw
 *                       status 0 has accepted the mailbox, not promised the context.
 *
 * The expectations come out of `res` -- the very values the caller put into the context --
 * rather than out of literals here, so the two cannot drift: a field this function
 * compares is by construction a field the fill step wrote.
 */
static void r4QpQueryAndCompare(Mlx4Hw &hw, Cx3R4QpResult *res)
{
    UInt8 q[QP_CONTEXT_SIZE];
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    UInt32 mask = 0;
    UInt64 dbRec = 0, mttL = 0;
    UInt32 mttH = 0;

    memset(q, 0, sizeof(q));
    res->queryFwStatus = CX3_FWSTATUS_NOT_POSTED;
    if (!hw.qpQuery(res->qpn, q, QP_CONTEXT_SIZE, &status,
                    res->rawQueryMailboxFinal, sizeof(res->rawQueryMailboxFinal),
                    &res->finalQueryCapture)) {
        res->queryFwStatus = status;
        res->queryOk = 0;
        LOG("r4qp: QUERY_QP on QPN %u did not answer (fw status 0x%x), so the state after "
            "this step is UNKNOWN -- the command may have been accepted and the query may "
            "have been refused, and those look the same from here",
            (unsigned)res->qpn, (unsigned)status);
        return;
    }
    res->queryFwStatus = status;
    res->queryOk = 1;
    res->queryFlagsRaw = cx3GetBe32(q + QP_CONTEXT_FLAGS_OFFSET);
    res->queryState    = (res->queryFlagsRaw >> MLX4_QP_STATE_NIBBLE_SHIFT) & 0xfu;
    res->queryLocalQpn = cx3GetBe32(q + QP_CONTEXT_LOCAL_QPN_OFFSET);
    res->queryRqSizeStride = q[QP_CONTEXT_RQ_SIZE_STRIDE_OFFSET];
    res->querySqSizeStride = q[QP_CONTEXT_SQ_SIZE_STRIDE_OFFSET];
    res->queryMtuMsgmax    = q[QP_CONTEXT_MTU_MSGMAX_OFFSET];
    res->queryCqnSend = cx3GetBe32(q + QP_CONTEXT_CQN_SEND_OFFSET);
    res->queryCqnRecv = cx3GetBe32(q + QP_CONTEXT_CQN_RECV_OFFSET);
    dbRec             = cx3GetBe64(q + QP_CONTEXT_DB_REC_ADDR_OFFSET);
    res->queryDbRecAddr = dbRec;
    res->queryLogPageSize = q[QP_CONTEXT_LOG_PAGE_SIZE_OFFSET];
    mttH = q[QP_CONTEXT_MTT_BASE_H_OFFSET];
    mttL = (UInt64)cx3GetBe32(q + QP_CONTEXT_MTT_BASE_L_OFFSET);
    res->queryMttAddrH = mttH;
    res->queryMttAddrL = mttL;
    res->queryParam3     = cx3GetBe32(q + QP_CONTEXT_PARAM3_OFFSET);
    res->querySchedQueue = q[QP_CONTEXT_PRI_SCHED_QUEUE_OFFSET];

    /*
     * The eight fields, each against the value that went in.  The mask is reported as well
     * as the all-or-nothing answer, because "which one" is the first thing a reader needs
     * and a boolean would send them back to the log to find it.
     */
    if (res->queryState    == MLX4_QP_STATE_RTS)  mask |= 1u << kR4QpFieldState;
    if (res->queryLocalQpn == res->qpn)           mask |= 1u << kR4QpFieldLocalQpn;
    if (res->queryRqSizeStride == res->rqSizeStrideSent)
        mask |= 1u << kR4QpFieldRqSize;
    if (res->querySqSizeStride == res->sqSizeStrideSent)
        mask |= 1u << kR4QpFieldSqSize;
    if (res->queryMtuMsgmax == res->mtuMsgmaxSent)
        mask |= 1u << kR4QpFieldMtuMsgmax;
    if (res->queryCqnSend == res->cqn)            mask |= 1u << kR4QpFieldCqnSend;
    if (res->queryCqnRecv == res->cqn)            mask |= 1u << kR4QpFieldCqnRecv;
    if (dbRec == res->dbRecordPhys)               mask |= 1u << kR4QpFieldDbRecAddr;
    res->queryFieldMask = mask;
    res->queryAllFieldsMatch =
        (mask == ((1u << kR4QpFieldState) | (1u << kR4QpFieldLocalQpn) |
                  (1u << kR4QpFieldRqSize) | (1u << kR4QpFieldSqSize) |
                  (1u << kR4QpFieldCqnSend) | (1u << kR4QpFieldCqnRecv) |
                  (1u << kR4QpFieldDbRecAddr) | (1u << kR4QpFieldMtuMsgmax))) ? 1u : 0u;
    res->queryMttMatches =
        (mttH == (UInt32)(res->mttAddr >> 32) &&
         mttL == (res->mttAddr & 0xffffffffull) &&
         res->queryLogPageSize == 0) ? 1u : 0u;

    LOG("r4qp: QUERY_QP QPN %u: fw status 0x%x; the firmware's own view: state %u "
        "(flags 0x%08x), local_qpn %u, rq_size_stride 0x%02x, sq_size_stride 0x%02x, "
        "mtu_msgmax 0x%02x, cqn_send %u, cqn_recv %u, db_rec_addr 0x%llx, "
        "mtt_base_addr_h 0x%02x, mtt_base_addr_l 0x%08x, log_page_size %u",
        (unsigned)res->qpn, (unsigned)res->queryFwStatus, (unsigned)res->queryState,
        (unsigned)res->queryFlagsRaw, (unsigned)res->queryLocalQpn,
        (unsigned)res->queryRqSizeStride, (unsigned)res->querySqSizeStride,
        (unsigned)res->queryMtuMsgmax, (unsigned)res->queryCqnSend,
        (unsigned)res->queryCqnRecv, (unsigned long long)dbRec, (unsigned)mttH,
        (unsigned)mttL, (unsigned)res->queryLogPageSize);

    if (!res->queryAllFieldsMatch) {
        LOG("r4qp: the readback does NOT match what was written (written/read): state "
            "%u/%u, local_qpn %u/%u, rq_size_stride 0x%02x/0x%02x, sq_size_stride "
            "0x%02x/0x%02x, mtu_msgmax 0x%02x/0x%02x, cqn_send %u/%u, cqn_recv %u/%u, "
            "db_rec_addr 0x%llx/0x%llx",
            (unsigned)MLX4_QP_STATE_RTS, (unsigned)res->queryState,
            (unsigned)res->qpn, (unsigned)res->queryLocalQpn,
            (unsigned)res->rqSizeStrideSent, (unsigned)res->queryRqSizeStride,
            (unsigned)res->sqSizeStrideSent, (unsigned)res->querySqSizeStride,
            (unsigned)res->mtuMsgmaxSent, (unsigned)res->queryMtuMsgmax,
            (unsigned)res->cqn, (unsigned)res->queryCqnSend,
            (unsigned)res->cqn, (unsigned)res->queryCqnRecv,
            (unsigned long long)res->dbRecordPhys, (unsigned long long)dbRec);
    }
    if (!res->queryMttMatches) {
        LOG("r4qp: the MTT address did not survive the three steps: sent 0x%llx "
            "(h 0x%02x / l 0x%08x, log_page_size 0), read back h 0x%02x / l 0x%08x, "
            "log_page_size %u. Those fields belong to the RST->INIT step alone "
            "(mlx4_qp.c:164-169) and are copied through the other two, so a difference "
            "means the shared context was disturbed between steps.",
            (unsigned long long)res->mttAddr, (unsigned)(res->mttAddr >> 32),
            (unsigned)(res->mttAddr & 0xffffffffull), (unsigned)mttH, (unsigned)mttL,
            (unsigned)res->queryLogPageSize);
    }
    /*
     * And the two fields whose wrong value is silent rather than loud: param3's FCS_KEEP bit
     * decides whether a received frame is 4 bytes longer, and sched_queue's port bits decide
     * which port the queue belongs to.  Reported on every path -- including the idempotent
     * one, where the SENT values are re-derived from the device's own capability word and
     * from the port this stage uses, because remembering them would print a number that was
     * never sent on that call.
     */
    if (res->queryParam3 != res->param3Sent ||
        res->querySchedQueue != res->schedQueueSent) {
        LOG("r4qp: two fields the firmware holds differ from what this driver writes: "
            "param3 0x%08x written / 0x%08x read back, sched_queue 0x%02x / 0x%02x. param3's "
            "bit 29 is FCS_KEEP (the device advertises it, so frames carry their 4-byte FCS) "
            "and sched_queue's high bits are the port",
            (unsigned)res->param3Sent, (unsigned)res->queryParam3,
            (unsigned)res->schedQueueSent, (unsigned)res->querySchedQueue);
    }
    LOG("r4qp: param3 written 0x%08x / read back 0x%08x; sched_queue written 0x%02x / read "
        "back 0x%02x", (unsigned)res->param3Sent, (unsigned)res->queryParam3,
        (unsigned)res->schedQueueSent, (unsigned)res->querySchedQueue);
}

/*
 * Publish the three layers as scalars, so the verifier can judge them without decoding a
 * structure -- the same reason `fault` reports its poison bit through `probe`.
 *
 * `haveQuery` and `haveProducer` say whether those layers were measured at all, and anything
 * that was not measured is left as CX3_R4QP_NOT_MEASURED rather than as a zero that a reader
 * would take for a value.  That distinction matters most for the producer, whose verdict is
 * an EQUALITY: two zeros satisfy it.
 */
static void r4QpPublishScalars(const Cx3R4QpResult *res, UInt64 *out,
                               bool haveQuery, bool haveProducer)
{
    out[kR4QpOutQpn]           = res->qpn;
    out[kR4QpOutStepsAccepted] = res->stepsAccepted;
    out[kR4QpOutStatesConfirmed] = res->statesConfirmed;
    if (haveQuery) {
        out[kR4QpOutQueryOk]   = res->queryOk;
        out[kR4QpOutQueryState] = res->queryOk ? res->queryState : CX3_R4QP_NOT_MEASURED;
        out[kR4QpOutQpnConfirmed] = res->queryOk ? res->qpnConfirmed : CX3_R4QP_NOT_MEASURED;
        out[kR4QpOutFieldsMatch] = res->queryOk ? res->queryAllFieldsMatch
                                                : CX3_R4QP_NOT_MEASURED;
        out[kR4QpOutMttMatches] = res->queryOk ? res->queryMttMatches
                                               : CX3_R4QP_NOT_MEASURED;
    }
    if (haveProducer) {
        out[kR4QpOutProducerValue]    = res->producerValue;
        out[kR4QpOutProducerReadback] = res->producerReadback;
        out[kR4QpOutProducerMatches]  = res->producerMatches;
    }
}

/*
 * kCx3SelR4Qp -- the RX QP three steps to RTS, and then the producer index.
 *
 * This is the stage that makes the receive ring reachable: until now its WQEs were
 * descriptors in host memory that nothing referred to.  Four things about it are worth
 * stating before the code, because each is a way to send three commands that all return
 * success and end up with a QP that receives nothing:
 *
 *   1. THE CONTEXT GOES AT MAILBOX +8.  The CQ context this project already sends lives at
 *      +0 (mlx4_cq.c:151) and the QP context at +8 (mlx4_qp.c:177-178), and both are "the
 *      context in the mailbox" in prose.  A BAD_PARAM (fw status 0x3) from any of the three
 *      commands is this mistake's signature, and it is the first thing to check.
 *
 *   2. ONE CONTEXT, THREE STATES.  All three transitions carry the same 248 bytes with only
 *      flags[31:28] different; the MTT address and log_page_size are set for RST->INIT and
 *      have to survive the other two (mlx4_qp.c:912-926).  The comparison after each step
 *      is what makes that checkable rather than assumed.
 *
 *   3. EVERY STEP IS QUERIED, NOT JUST THE LAST.  A command returning 0 says the mailbox
 *      was accepted.  Only QUERY_QP says the hardware is in that state, and an intermediate
 *      step that was silently dropped still ends at RTS if a later one compensates -- so
 *      the intermediate readings are the evidence that the sequence really went
 *      RST -> INIT -> RTR -> RTS in that order.
 *
 *   4. PUBLISHING THE PRODUCER IS A HOST MEMORY WRITE, NOT A DOORBELL.
 *      `*ring->wqres.db.db = cpu_to_be32(ring->prod & 0xffff)` (mlx4_en_rx.c:250-254), into
 *      the record whose DMA address is the context's db_rec_addr.  That record is allocated
 *      here: stage 3 built descriptors and had no reason to know a QP would need a producer
 *      record.  The value is read back OUT of that memory, because a write nobody can
 *      observe is a claim rather than a measurement.
 *
 * The QP number follows this project's rule for every other resource: it is derived (the
 * firmware's reserved share -- the same rule the CQN follows) and then confirmed by the
 * query.  Both facts are reported, so "the rule produced a number" cannot pass as "the
 * hardware has that QP".
 */
IOReturn Cx3Mlx4::exportR4Qp(const UInt64 *in, Cx3R4QpResult *res, UInt64 *out, UInt32 port)
{
    if (port<1 || port>2 || !res || !out || !in) return kIOReturnBadArgument;
    for (UInt32 i=0;i<kR4QpInCount;++i)
        if (in[i]>0x7fffffffull) return kIOReturnBadArgument;
    Cx3PortQueues &queue = fPortQueues[port-1];
    if ((port==2 && !queue.numbersReserved) || queue.setupFailed ||
        (queue.numbersReserved && queue.numberGeneration!=fDeviceGeneration)) {
        memset(res,0,sizeof(*res)); res->status=(UInt32)kIOReturnNotReady;
        memset(out,0,sizeof(UInt64)*kR4QpOutCount);
        out[0]=(UInt64)kIOReturnNotReady; return kIOReturnSuccess;
    }

    UInt32 qpnRequested = (UInt32)in[kR4QpInQpn];
    UInt32 qpnProfileMax;
    UInt32 fwStatus = CX3_FWSTATUS_NOT_POSTED;
    UInt32 i;
    UInt8  context[QP_CONTEXT_SIZE];
    Mlx4Icm *rec = 0;
    UInt64 recPhys = 0;
    UInt32 prod = 0, prodBefore = 0, prodBack = 0;
    /*
     * The three transitions, in the reference's order and with its own state numbers
     * (mlx4_qp.c:912-926 for the walk, qp.h:67-77 for the values).
     */
    static const UInt16 stepOp[3] = { MLX4_CMD_RST2INIT_QP, MLX4_CMD_INIT2RTR_QP,
                                      MLX4_CMD_RTR2RTS_QP };
    static const UInt32 stepState[3] = { MLX4_QP_STATE_INIT, MLX4_QP_STATE_RTR,
                                         MLX4_QP_STATE_RTS };
    /*
     * counter_index = 0xFF, which is how the reference spells "no counter"
     * (mlx4_en_resources.c:66 takes it from priv->counter_index, and the guard on the line
     * after it is written as `counter_index != 0xFF`).  This port has reserved no counter:
     * the counters table exists in the profile and nothing has allocated an index in it, so
     * "none" is the only truthful value and it is the reference's own word for it.
     */
    const UInt32 counterIndex = 0xffu;
    bool fcsKeep = false, roce = false;

    memset(out, 0, sizeof(UInt64) * kR4QpOutCount);
    memset(res, 0, sizeof(*res));
    res->status = (UInt32)kIOReturnError;
    /*
     * Every measured scalar starts as "never reached", so that a path which refuses before
     * asking the device cannot leave zeros behind that a reader would take for answers --
     * including the two equality assertions, which two zeros would satisfy.
     */
    out[kR4QpOutQpn]              = CX3_R4QP_NOT_MEASURED;
    out[kR4QpOutStepsAccepted]    = CX3_R4QP_NOT_MEASURED;
    out[kR4QpOutStatesConfirmed]  = CX3_R4QP_NOT_MEASURED;
    out[kR4QpOutQueryOk]          = CX3_R4QP_NOT_MEASURED;
    out[kR4QpOutQueryState]       = CX3_R4QP_NOT_MEASURED;
    out[kR4QpOutQpnConfirmed]     = CX3_R4QP_NOT_MEASURED;
    out[kR4QpOutFieldsMatch]      = CX3_R4QP_NOT_MEASURED;
    out[kR4QpOutMttMatches]       = CX3_R4QP_NOT_MEASURED;
    out[kR4QpOutProducerValue]    = CX3_R4QP_NOT_MEASURED;
    out[kR4QpOutProducerReadback] = CX3_R4QP_NOT_MEASURED;
    out[kR4QpOutProducerMatches]  = CX3_R4QP_NOT_MEASURED;

    if (queue.qpRec && !queue.qpUp) {
        LOG("r4qp: unresolved setup owns QPN %u and its record; refusing to overwrite it",
            (unsigned)queue.qpQpn);
        res->status = (UInt32)kIOReturnNotReady;
        out[kR4QpOutStatus] = res->status;
        return kIOReturnSuccess;
    }

    /*
     * The four things this step needs, named rather than assumed.  Each is a stage that has
     * already run: the HCA has to be up, the memory region has to be enabled (every WQE
     * carries its lkey), the CQ has to exist (the context names its number), and the ring
     * has to be built (the context names its MTT entries and the hardware its buffers).
     */
    if (!fHcaInited) {
        LOG("r4qp refused: the HCA is not initialised (run m2dprofile/m2dbringup first)");
        res->status = (UInt32)kIOReturnNotReady;
        out[kR4QpOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if (fMrState != 1) {
        LOG("r4qp refused: no memory region is enabled (state %u). Every WQE carries its "
            "lkey, so stage 1 has to have run and succeeded.", (unsigned)fMrState);
        res->status = (UInt32)kIOReturnNotReady;
        out[kR4QpOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if (!queue.cqUp) {
        LOG("r4qp refused: no completion queue exists, so cqn_send/cqn_recv would name a "
            "queue the hardware cannot report to (stage 2)");
        res->status = (UInt32)kIOReturnNotReady;
        out[kR4QpOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if (!queue.rqUp) {
        LOG("r4qp refused: no receive ring exists (stage 3), so the QP would name MTT "
            "entries and buffers that do not exist");
        res->status = (UInt32)kIOReturnNotReady;
        out[kR4QpOutStatus] = res->status;
        return kIOReturnSuccess;
    }

    /*
     * The QP number.  The rule is the firmware's reserved share, the same shape the CQN
     * follows, measured on this card as 64 with log_num_qps 10 (1024 QPs) -- so 64..1023 are
     * software's.  The ceiling comes from the PROFILE and not from the card's capability,
     * for the reason the CQ path already records: a number the configured cMPT has no entry
     * for is a different thing from a number the silicon could hold.
     */
    res->qpnRule = fDevCap.reserved_qps;
    qpnProfileMax = (fInitHca.log_num_qps < 32u) ? (1u << fInitHca.log_num_qps) : 0xffffffffu;
    res->qpnProfileMax = qpnProfileMax;
    if (queue.numbersReserved && in[kR4QpInQpn] && in[kR4QpInQpn]!=queue.numbers.rxQpn) {
        res->status=(UInt32)kIOReturnBadArgument; out[0]=res->status; return kIOReturnSuccess;
    }
    res->qpn = queue.numbersReserved ? queue.numbers.rxQpn :
               (qpnRequested ? qpnRequested : res->qpnRule);
    if (res->qpn < fDevCap.reserved_qps) {
        LOG("r4qp refused: QPN %u is inside the firmware's reserved range (0..%u). Those are "
            "the firmware's own QPs, including the special ones at the base of the range.",
            (unsigned)res->qpn,
            (unsigned)(fDevCap.reserved_qps ? fDevCap.reserved_qps - 1u : 0u));
        res->status = (UInt32)kIOReturnBadArgument;
        out[kR4QpOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if (res->qpn >= qpnProfileMax) {
        LOG("r4qp refused: QPN %u is at or past the %u QPs this profile configured "
            "(log_num_qps %u); the card's ceiling is a different number and not what was "
            "configured", (unsigned)res->qpn, (unsigned)qpnProfileMax,
            (unsigned)fInitHca.log_num_qps);
        res->status = (UInt32)kIOReturnBadArgument;
        out[kR4QpOutStatus] = res->status;
        return kIOReturnSuccess;
    }

    /*
     * Idempotence.  A second call must not allocate a second producer record and re-send
     * three transitions for a QP the hardware already has: the first run's record is the one
     * the context names, so a second one would leave the QP pointing at the first while the
     * new value was published somewhere nobody reads.  The existing QP is therefore
     * re-MEASURED -- the query and the producer readback both run on this path exactly as
     * they do on the fresh one -- because a path that reports remembered numbers cannot
     * notice the device disagreeing.
     */
    if (queue.qpUp) {
        res->qpn = queue.qpQpn;
        res->pdn = fMrPdn;
        res->uarIndex = queue.cqUarStart;
        res->cqn = queue.cqCqn;
        res->nent = queue.rqNent;
        res->dbRecordPhys = queue.qpRecPhys;
        res->mttOffset = queue.rqMttOff;
        res->mttAddr = queue.rqMttAddr;
        res->producerValue = queue.qpProd;
        res->producerWidth = RX_PROD_DB_WIDTH;
        res->counterIndexSent = counterIndex;
        fcsKeep = (fDevCap.flags & MLX4_DEV_CAP_FLAG_FCS_KEEP) != 0;
        res->fcsKeep = fcsKeep ? 1u : 0u;
        res->roceAdvertised =
            (fDevCap.bmme_flags & MLX4_BMME_FLAG_ROCE_V1_V2) ? 1u : 0u;
        /*
         * param3 and sched_queue are RE-DERIVED rather than remembered, and the difference
         * matters: this call sent nothing, so "the value that went into the context" does not
         * exist for it.  What does exist is the value this driver's fill produces for this
         * QP -- a function of the device's own FCS_KEEP capability and of the port this stage
         * uses -- and that is what is reported.  The firmware's own copy of both is printed
         * beside it by the comparison above, which is the measurement; this is the claim it
         * is measured against.
         */
        res->param3Sent = fcsKeep ? MLX4_QP_PARAM3_FCS_KEEP : 0u;
        res->schedQueueSent = (UInt8)(MLX4_QP_SCHED_QUEUE_ETH |
                                      ((port - 1u) <<
                                       MLX4_QP_SCHED_QUEUE_PORT_SHIFT));
        res->rqSizeStrideSent =
            (UInt8)((mlx4Log2(queue.rqNent) << MLX4_QP_RQ_STRIDE_SHIFT) |
                    (mlx4Log2(MLX4_QP_WQE_SIZE_RQ) - MLX4_QP_STRIDE_SUB));
        res->sqSizeStrideSent =
            (UInt8)(mlx4Log2(MLX4_QP_WQE_SIZE_DUMMY_SQ) - MLX4_QP_STRIDE_SUB);
        res->mtuMsgmaxSent = MLX4_QP_MTU_MSGMAX_ETH;
        r4QpQueryAndCompare(fHw, res);
        res->qpnConfirmed = (res->queryOk && res->queryLocalQpn == res->qpn) ? 1u : 0u;
        if (queue.qpRec) {
            prodBefore = OSSwapBigToHostInt32(((volatile UInt32 *)queue.qpRec->virt)[0]);
        }
        res->producerBefore   = prodBefore;
        res->producerReadback = prodBefore;
        res->producerMatches  = (prodBefore == res->producerValue) ? 1u : 0u;
        LOG("r4qp: QPN %u already exists (state last written %u, producer value %u); "
            "measured it instead of building a second QP: query state %u, local_qpn %u, the "
            "record holds %u, the eight fields %s",
            (unsigned)res->qpn, (unsigned)queue.qpState, (unsigned)res->producerValue,
            (unsigned)res->queryState, (unsigned)res->queryLocalQpn, (unsigned)prodBefore,
            res->queryAllFieldsMatch ? "match" : "MISMATCH");
        r4QpPublishScalars(res, out, true, true);
        if (!res->queryOk || !res->queryAllFieldsMatch || !res->queryMttMatches ||
            !res->producerMatches) {
            queue.qpUp = false;
            queue.qpState = CX3_R4QP_NOT_MEASURED;
            if (port==1) fNetEnabled = false;
            res->status = (UInt32)kIOReturnNotReady;
            out[kR4QpOutStatus] = res->status;
            return kIOReturnSuccess;
        }
        res->status = 0;
        out[kR4QpOutStatus] = 0;
        return kIOReturnSuccess;
    }

    /*
     * The producer record: host DMA memory the HCA reads to learn how many WQEs it may use.
     * One whole page, because that is what the reference's doorbell allocator hands out
     * (mlx4_alloc.c:731-758 allocates a page per group of records), and because the record
     * must not share a page with anything whose address is derived from it.  Nothing about
     * its contents is assumed: it is cleared and then read back before the publish, so the
     * "before" value in the result is a measurement too.
     */
    rec = fHw.icmAlloc(1, false);
    if (!rec) {
        LOG("r4qp: could not allocate the producer record");
        res->status = (UInt32)kIOReturnNoMemory;
        out[kR4QpOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    memset(rec->virt, 0, (size_t)rec->bytes);
    recPhys = rec->pages[0].busAddr;

    res->pdn = fMrPdn;
    res->uarIndex = queue.cqUarStart;
    res->cqn = queue.cqCqn;
    res->nent = queue.rqNent;
    res->dbRecordPhys = recPhys;
    res->dbRecordPage = 0;              /* the first page of this allocation */
    res->mttOffset = queue.rqMttOff;
    res->mttAddr = queue.rqMttAddr;
    res->counterIndexSent = counterIndex;
    res->producerWidth = RX_PROD_DB_WIDTH;

    /*
     * FCS_KEEP, read from the device rather than decided here.  Measured on this card:
     * caps.flags bit 34 is 1 (ext_flags 0xeeff1f9d << 32 | flags 0xf677fbef), so the bit IS
     * set and param3 gets 1 << 29 instead of 0.  The consequence for stages 9/10 is not
     * cosmetic -- frames arrive carrying their 4-byte Ethernet FCS, exactly as the branch
     * that sets this bit says (`ring->fcs_del = ETH_FCS_LEN`, mlx4_en_rx.c:949-953), so
     * whatever reads the frame has to account for those four bytes.
     */
    fcsKeep = (fDevCap.flags & MLX4_DEV_CAP_FLAG_FCS_KEEP) != 0;
    res->fcsKeep = fcsKeep ? 1u : 0u;
    /*
     * RoCE V1/V2, reported and deliberately NOT acted on.
     *
     * The device advertises it -- bmme_flags bit 19, which is precisely the bit
     * mlx4_fw.c:1015-1016 turns into the flags2 word (device.h:282) -- so the reference
     * would set roce_entropy on the RTR->RTS transition (mlx4_qp.c:171-175).  This port
     * leaves it 0 as a decision rather than an omission: computing it needs an extra
     * QUERY_QP round trip to read the firmware's remote_qpn plus a folding hash
     * (mlx4_qp.c:932-947), and it is a RoCE SEND-side flow hint while this stage's target is
     * Ethernet receive.  The falsification condition is written down here and in
     * docs/R4-STAGE4-RESULT.md: if the QP does not reach RTS, or stages 9/10 receive
     * nothing, this is among the first things to revisit.
     */
    roce = (fDevCap.bmme_flags & MLX4_BMME_FLAG_ROCE_V1_V2) != 0;
    res->roceAdvertised = roce ? 1u : 0u;
    LOG("r4qp: the device advertises RoCE V1/V2 (bmme_flags 0x%08x, bit 19 = %u) and "
        "roce_entropy is nevertheless left 0 on purpose: it is a RoCE send-side flow hint, it "
        "needs an extra QUERY_QP round trip plus a fold (mlx4_qp.c:932-947), and this stage's "
        "target is Ethernet receive. This is a recorded decision, not a field that was "
        "missed.", (unsigned)fDevCap.bmme_flags, roce ? 1u : 0u);

    /*
     * One context, built once.  The MTT fields are deliberately NOT part of the fill: they
     * belong to the RST->INIT step alone and are written just before that command, so the
     * other two carry them as inherited data -- which is what the per-step query checks.
     */
    fHw.qpFillContext(context, fMrPdn, res->qpn, queue.cqUarStart, queue.rqNent,
                      MLX4_QP_WQE_SIZE_RQ, queue.cqCqn, recPhys, MLX4_QP_MTU_MSGMAX_ETH,
                      port, fcsKeep, counterIndex, /*rss=*/false, /*isTx=*/false,
                      /*userPrio=*/-1);
    res->rqSizeStrideSent = context[QP_CONTEXT_RQ_SIZE_STRIDE_OFFSET];
    res->sqSizeStrideSent = context[QP_CONTEXT_SQ_SIZE_STRIDE_OFFSET];
    res->mtuMsgmaxSent    = context[QP_CONTEXT_MTU_MSGMAX_OFFSET];
    res->schedQueueSent   = context[QP_CONTEXT_PRI_SCHED_QUEUE_OFFSET];
    res->param3Sent       = cx3GetBe32(context + QP_CONTEXT_PARAM3_OFFSET);
    if ((res->mttAddr >> 32) != 0) {
        /*
         * mtt_base_addr_h is a single byte, so the byte address has to fit in 32 bits.  It
         * does here (the table's first chunk is 256 KiB and the allocation starts at entry
         * 64), and this refuses rather than truncating, because a truncated MTT address is a
         * QP that reads ring descriptors out of some other memory.
         */
        LOG("r4qp: the MTT byte address 0x%llx does not fit in mtt_base_addr_l (the high "
            "byte would be 0x%02x) -- refusing rather than truncating, because a truncated "
            "MTT address is a QP whose descriptors come from somewhere else",
            (unsigned long long)res->mttAddr, (unsigned)(res->mttAddr >> 32));
        fHw.icmFree(rec);
        res->status = (UInt32)kIOReturnUnsupported;
        out[kR4QpOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    context[QP_CONTEXT_LOG_PAGE_SIZE_OFFSET] = 0;      /* 4 KiB pages: 12 - 12 */
    context[QP_CONTEXT_MTT_BASE_H_OFFSET] = 0;
    {
        UInt32 lo = (UInt32)(res->mttAddr & 0xffffffffull);
        context[QP_CONTEXT_MTT_BASE_L_OFFSET + 0] = (UInt8)((lo >> 24) & 0xffu);
        context[QP_CONTEXT_MTT_BASE_L_OFFSET + 1] = (UInt8)((lo >> 16) & 0xffu);
        context[QP_CONTEXT_MTT_BASE_L_OFFSET + 2] = (UInt8)((lo >>  8) & 0xffu);
        context[QP_CONTEXT_MTT_BASE_L_OFFSET + 3] = (UInt8)(lo & 0xffu);
    }
    LOG("r4qp: QPN %u (rule: the firmware's reserved share %u, profile ceiling %u), context "
        "at mailbox +%u, %u bytes; the ring's MTT: entry %u -> byte address 0x%llx "
        "(index x %u), log_page_size 0 (set for RST->INIT only); producer record at 0x%llx",
        (unsigned)res->qpn, (unsigned)res->qpnRule, (unsigned)res->qpnProfileMax,
        (unsigned)QP_CONTEXT_MAILBOX_OFFSET, (unsigned)QP_CONTEXT_SIZE,
        (unsigned)res->mttOffset, (unsigned long long)res->mttAddr,
        (unsigned)MLX4_MTT_ENTRY_SIZE, (unsigned long long)recPhys);

    /* Register ownership before the first possible device hand-off. Readiness
     * remains false until every query and the producer readback have passed. */
    queue.qpRec = rec;
    queue.qpRecPhys = recPhys;
    queue.qpQpn = res->qpn;
    queue.qpProd = 0;
    queue.qpState = CX3_R4QP_NOT_MEASURED;
    queue.qpUp = false;

    /* The first state-transition command may publish every referenced DMA block. */
    {
        Mlx4Icm *dmaOwners[5] = { queue.qpRec, queue.cqRing, queue.cqRec,
                                  queue.rqRing, queue.rqBufs };
        if (!fHw.markDmaHandoff(dmaOwners, 5)) {
            if (port==1) fNetEnabled = false;
            res->status = (UInt32)kIOReturnNotReady;
            LOG("r4qp: refusing QP transition because a referenced DMA owner has no generation");
            out[kR4QpOutStatus] = res->status;
            return kIOReturnSuccess;
        }
    }

    /* --- layer 1: the three transitions, each followed by its own query --- */
    for (i = 0; i < 3; ++i) {
        UInt32 flags;
        /*
         * Same context, four bits different: flags[31:28] is the new state, and everything
         * the fill step put in place stays as it is (mlx4_qp.c:912-914 masks the nibble and
         * ORs the new state in).  Written byte by byte because the field is big-endian in a
         * byte buffer and this file has no put helper of its own.
         */
        flags  = cx3GetBe32(context + QP_CONTEXT_FLAGS_OFFSET);
        flags &= ~(0xfu << MLX4_QP_STATE_NIBBLE_SHIFT);
        flags |= (stepState[i] << MLX4_QP_STATE_NIBBLE_SHIFT);
        res->stepOpcode[i]      = stepOp[i];
        res->stepStateWanted[i] = stepState[i];
        context[QP_CONTEXT_FLAGS_OFFSET + 0] = (UInt8)((flags >> 24) & 0xffu);
        context[QP_CONTEXT_FLAGS_OFFSET + 1] = (UInt8)((flags >> 16) & 0xffu);
        context[QP_CONTEXT_FLAGS_OFFSET + 2] = (UInt8)((flags >>  8) & 0xffu);
        context[QP_CONTEXT_FLAGS_OFFSET + 3] = (UInt8)(flags & 0xffu);
        /*
         * The reference clears params2's FPP bit for every state except RTR
         * (mlx4_qp.c:915-916).  This port's context is zero from the start, so the bit is
         * already clear; the decision is named here rather than left as a missing line, and
         * the fill step's comment says the same thing at the field's offset.
         */

        if (!fHw.qpModify(stepOp[i], res->qpn, context, &fwStatus,
                          res->rawSentMailboxStep[i], sizeof(res->rawSentMailboxStep[i]),
                          &res->modifyCapture[i])) {
            if (port==1) fNetEnabled = false;
            res->stepFwStatus[i]   = fwStatus;
            res->stepQueryOk[i]    = 0;
            res->stepStateAfter[i] = 0xffffffffu;
            res->statesConfirmed   = 0;
            if (fwStatus == CX3_FWSTATUS_TIMEOUT) {
                LOG("r4qp: step %u (opcode 0x%02x) TIMED OUT after being posted, so the QP's "
                    "state is UNKNOWN. Nothing is torn down and nothing is assumed: the "
                    "device may still own this QP, and 2RST_QP on a QP whose state is unknown "
                    "is a command this port does not send blind.",
                    (unsigned)i + 1, (unsigned)stepOp[i]);
            } else {
                LOG("r4qp: step %u (opcode 0x%02x, state %u) FAILED with fw status 0x%x; "
                    "stopping here. A BAD_PARAM (0x3) on the first step is this stage's "
                    "signature mistake: the QP context belongs at mailbox +%u, not +0 where "
                    "the CQ context goes.", (unsigned)i + 1, (unsigned)stepOp[i],
                    (unsigned)stepState[i], (unsigned)fwStatus,
                    (unsigned)QP_CONTEXT_MAILBOX_OFFSET);
            }
            res->stepsAccepted = i;
            res->status = 0;      /* the selector answered; it was the device that refused */
            r4QpPublishScalars(res, out, false, false);
            out[kR4QpOutStatus] = 0;
            if (i != 0 || (fwStatus != CX3_FWSTATUS_NOT_POSTED &&
                           fwStatus != CX3_FWSTATUS_WEDGED &&
                           fwStatus != CX3_FWSTATUS_NO_DEVICE)) {
                /* Keep the owner pointer but NOT readiness. Every data-plane caller
                 * still sees Up=false; teardown must also account for the owner pointer. */
                queue.qpRec = rec;
                queue.qpRecPhys = recPhys;
                queue.qpQpn = res->qpn;
                queue.qpProd = 0;
                queue.qpState = CX3_R4QP_NOT_MEASURED;
                queue.qpUp = false;
                LOG("r4qp: failed setup retained with its record and RQ dependencies");
            } else {
                /* No earlier transition and this one never posted: the new record
                 * was never transferred to the device. A FW error alone is not enough. */
                fHw.icmFree(rec);
                queue.qpRec = 0;
                queue.qpRecPhys = 0;
            }
            return kIOReturnSuccess;
        }
        res->stepFwStatus[i] = fwStatus;
        res->stepsAccepted = i + 1;

        /*
         * The query, immediately, while the state being asked about is the one just written.
         * A single reading of RTS at the end would be consistent with "the INIT step was
         * dropped and a later command carried the queue all the way", which is a different
         * thing from the sequence this stage claims to have performed.
         */
        {
            UInt8 q[QP_CONTEXT_SIZE];
            UInt32 qStatus = CX3_FWSTATUS_NOT_POSTED;
            memset(q, 0, sizeof(q));
            if (fHw.qpQuery(res->qpn, q, QP_CONTEXT_SIZE, &qStatus,
                            res->rawQueryMailboxStep[i], sizeof(res->rawQueryMailboxStep[i]),
                            &res->queryCapture[i])) {
                UInt32 st = (cx3GetBe32(q + QP_CONTEXT_FLAGS_OFFSET) >>
                             MLX4_QP_STATE_NIBBLE_SHIFT) & 0xfu;
                UInt32 backQpn = cx3GetBe32(q + QP_CONTEXT_LOCAL_QPN_OFFSET);
                res->stepQueryOk[i]    = 1;
                res->stepStateAfter[i] = st;
                if (st == stepState[i] && backQpn == res->qpn)
                    res->statesConfirmed = i + 1;
                LOG("r4qp: step %u accepted (fw status 0x%x); QUERY_QP says state %u, "
                    "local_qpn %u -- %s",
                    (unsigned)i + 1, (unsigned)fwStatus, (unsigned)st, (unsigned)backQpn,
                    (st == stepState[i] && backQpn == res->qpn)
                        ? "the state and the QP number are what was written"
                        : "NOT what was written: the command was accepted and the state was "
                          "not");
            } else {
                res->stepQueryOk[i]    = 0;
                res->stepStateAfter[i] = 0xffffffffu;
                LOG("r4qp: step %u returned fw status 0x%x but QUERY_QP did not answer (fw "
                    "status 0x%x), so the state after this step is UNKNOWN -- the two are "
                    "different reasons to stop", (unsigned)i + 1, (unsigned)fwStatus,
                    (unsigned)qStatus);
            }
        }
        if (res->statesConfirmed != i + 1) {
            if (port==1) fNetEnabled = false;
            res->status = (UInt32)kIOReturnNotReady;
            r4QpPublishScalars(res, out, false, false);
            out[kR4QpOutStatus] = res->status;
            return kIOReturnSuccess;
        }
    }

    /* --- layer 2: the last query, field by field --- */
    r4QpQueryAndCompare(fHw, res);
    res->qpnConfirmed = (res->queryOk && res->queryLocalQpn == res->qpn) ? 1u : 0u;
    if (!res->queryOk || !res->queryAllFieldsMatch || !res->queryMttMatches) {
        if (port==1) fNetEnabled = false;
        res->status = (UInt32)kIOReturnNotReady;
        r4QpPublishScalars(res, out, true, false);
        out[kR4QpOutStatus] = res->status;
        return kIOReturnSuccess;
    }

    /* --- layer 3: publish the producer, then read it back --- */
    /*
     * The value is the number of WQEs the hardware may use, which is the ring's size,
     * because stage 3 posted one buffer per WQE.  The reference publishes ring->prod at this
     * point and prod is exactly the count of descriptors it initialised
     * (mlx4_en_rx.c:485-495, and mlx4_en_rx.c:951-961 publishes it right after the queue
     * reaches its state).  The width is 16 bits, not the CQ's 24: `prod & 0xffff`
     * (mlx4_en_rx.c:253).
     */
    prod = queue.rqNent;
    prodBefore = OSSwapBigToHostInt32(((volatile UInt32 *)rec->virt)[0]);
    fHw.rqSetProducer((volatile UInt32 *)rec->virt, prod);
    prodBack = OSSwapBigToHostInt32(((volatile UInt32 *)rec->virt)[0]);
    res->producerValue    = prod;
    res->producerBefore   = prodBefore;
    res->producerReadback = prodBack;
    res->producerMatches  = (prodBack == prod) ? 1u : 0u;
    /*
     * The readback is the same load the hardware performs to learn the index, so it is not
     * a formality: a different width, a different endianness, or a record other than the one
     * the context's db_rec_addr names would each produce a driver that believes it published
     * something while the hardware read zero.
     */
    LOG("r4qp: RQ producer published: %u (0x%x) into the record at 0x%llx -- the DMA byte "
        "address the QP context's db_rec_addr carries. Read back out of host memory: %u "
        "(%s). This is a %u-bit write into host memory, NOT an MMIO doorbell: the RX "
        "producer goes to the record (mlx4_en_rx.c:250-254), unlike the CQ's arm.",
        (unsigned)prod, (unsigned)prod, (unsigned long long)recPhys, (unsigned)prodBack,
        (prodBack == prod) ? "matches" : "MISMATCH", (unsigned)RX_PROD_DB_WIDTH);
    if (!res->producerMatches) {
        if (port==1) fNetEnabled = false;
        /*
         * The QP is in RTS and the record it names is about to be released, which is exactly
         * the state this project's teardown paths exist to avoid.  Put the QP back first,
         * and release the record only if that worked.
         */
        UInt32 rstStatus = CX3_FWSTATUS_NOT_POSTED;
        LOG("r4qp: the producer did not survive its own write, so the QP is put back to RST "
            "and the record released rather than left published -- a QP in RTS pointing at "
            "released memory is worse than no QP");
        if (fHw.qpToReset(res->qpn, &rstStatus)) {
            LOG("r4qp: QPN %u is back in RST (fw status 0x%x)", (unsigned)res->qpn,
                (unsigned)rstStatus);
            fHw.icmFree(rec);
            queue.qpRec = 0;
            queue.qpRecPhys = 0;
        } else {
            LOG("r4qp: putting QPN %u back to RST failed (fw status 0x%x), so the QP is left "
                "as it is and the record is deliberately NOT released; a reboot reclaims both",
                (unsigned)res->qpn, (unsigned)rstStatus);
            queue.qpRec = rec;
            queue.qpRecPhys = recPhys;
            queue.qpQpn = res->qpn;
            queue.qpProd = 0;
            queue.qpState = CX3_R4QP_NOT_MEASURED;
            queue.qpUp = false;
        }
        res->status = (UInt32)kIOReturnError;
        r4QpPublishScalars(res, out, true, false);
        out[kR4QpOutStatus] = res->status;
        return kIOReturnSuccess;
    }

    /*
     * Durable state.  The hardware now names this record and the ring's MTT entries, so both
     * have to outlive this call -- the same reason stage 2 kept its ring and record, with one
     * addition: this is the first object here that can outlive a FAILED later stage, and the
     * teardown path needs its number to put it back.
     */
    queue.qpRec     = rec;
    queue.qpRecPhys = recPhys;
    queue.qpQpn     = res->qpn;
    queue.qpState   = MLX4_QP_STATE_RTS;
    queue.qpProd    = prod;
    queue.qpUp      = true;

    res->status = 0;
    out[kR4QpOutStatus] = 0;
    r4QpPublishScalars(res, out, true, true);
    LOG("r4qp OK: QPN %u is in RTS. Commands accepted %u/3, states confirmed by QUERY_QP "
        "%u/3, the readback's eight fields %s, the MTT address %s, local_qpn %s, and the "
        "producer record holds %u. The receive ring is reachable now: the hardware knows "
        "where the WQEs are and how many it may use. Whether frames arrive is stages 5-10.",
        (unsigned)res->qpn, (unsigned)res->stepsAccepted, (unsigned)res->statesConfirmed,
        res->queryAllFieldsMatch ? "all match" : "DO NOT all match",
        res->queryMttMatches ? "survived" : "DID NOT survive",
        res->qpnConfirmed ? "confirmed" : "NOT confirmed", (unsigned)prodBack);
    return kIOReturnSuccess;
}

/* ==================================================================== */
/* R4 stage 5: the MAC table, and the RSS indirection QP                */
/* ==================================================================== */

/*
 * The port's own address, as the MAC table wants it, taken from QUERY_PORT's def_mac.
 *
 * Two encodings meet here and the difference between them is a byte order, so both are written
 * out rather than converted:
 *
 *   def_mac arrives as an 8-byte BE64 (Cx3QueryPortResult::defMac is filled straight from the
 *   answer's offset 0x10, and the log prints bytes 2..7 as the address -- that is what
 *   `def_mac 00:11:22:33:44:55` is).
 *
 *   a table entry is `cpu_to_be64(mac | MLX4_MAC_VALID)` (mlx4_port.c:299), and MLX4_MAC_VALID
 *   is 1ull << 63 (mlx4_port.c:48) -- the TOP bit of the big-endian quantity, i.e. the 0x80 bit
 *   of its FIRST byte.
 *
 * So the entry is built by copying the device's eight bytes and setting one bit in byte 0.
 * Going through a host UInt64 instead would mean getting the byte order right twice (once
 * reading the answer, once writing the mailbox), and a mistake in either produces a number that
 * still LOOKS like a MAC address -- which is the failure mode this project keeps recording.
 *
 * `outValue` is the same entry's low 48 bits, for the log and the tool: a plain number, derived
 * from the bytes rather than assembled separately, so the two cannot disagree.
 */
static void r4IngressMacEntry(const UInt8 defMac[8], UInt8 entry[8], UInt64 *outValue)
{
    UInt32 i;
    UInt64 v = 0;

    for (i = 0; i < 8; ++i) entry[i] = defMac[i];
    entry[0] |= (UInt8)(MLX4_MAC_VALID >> 56);   /* 0x80: the valid bit, byte 0's top bit */
    /* The address itself: the low 48 bits of the BE64, i.e. its last six bytes. */
    for (i = 2; i < 8; ++i) v = (v << 8) | (UInt64)defMac[i];
    if (outValue) *outValue = v;
}

/*
 * The Toeplitz key for the RSS overlay.
 *
 * The reference asks its host stack (`rss_getkey`, mlx4_en_rx.c:1003); this port has no stack to
 * ask, so the key is a fixed pattern built here -- byte i is 0x40 + i, forty distinct values.
 * Two reasons for a pattern rather than zeros, and neither is cosmetic:
 *
 *   - with ONE ring the key cannot change which queue a packet lands in, because there is only
 *     one target, so its value is not load-bearing for this stage;
 *   - what IS worth measuring is that the forty bytes survive the context round trip, and a
 *     patterned key makes a partial or misplaced write visible where forty zero bytes would
 *     read back exactly like a field nobody wrote.
 *
 * If a later milestone runs multiple rings the key becomes load-bearing -- both ends of a flow
 * have to hash to the same queue -- and this function has to be replaced by something a host
 * stack supplies.  That is recorded in docs/R4-STAGE5-RESULT.md rather than left as a surprise.
 */
static void r4IngressBuildKey(UInt8 *key, UInt32 bytes)
{
    UInt32 i;
    for (i = 0; i < bytes; ++i) key[i] = (UInt8)(0x40u + i);
}

/*
 * Publish the stage's judged claims as scalars, so the verifier decides from values.
 *
 * `haveQuery` says whether the second layer was measured at all; anything not measured stays
 * CX3_R4INGRESS_NOT_MEASURED rather than becoming a zero a reader would take for an answer.
 * That matters most for the two equalities here -- both of which a pair of zeros satisfies --
 * and for the MAC read-back, where "the firmware did not answer that direction at all" and "the
 * firmware answered with a different table" are different facts with different next steps.
 */
static void r4IngressPublishScalars(const Cx3R4IngressResult *res, UInt64 *out,
                                    bool haveQuery)
{
    out[kR4IngressOutMacStatus]        = res->macFwStatus;
    out[kR4IngressOutMacSlot]          = res->macSlot;
    out[kR4IngressOutMacReadbackOk]    = res->macReadbackOk;
    out[kR4IngressOutMacReadbackMatch] = res->macReadbackOk
                                             ? res->macReadbackMatches
                                             : CX3_R4INGRESS_NOT_MEASURED;
    out[kR4IngressOutQpn]              = res->qpn;
    out[kR4IngressOutStepsAccepted]    = res->stepsAccepted;
    out[kR4IngressOutStatesConfirmed]  = res->statesConfirmed;
    out[kR4IngressOutRxQpn]            = res->rxQpn;
    if (haveQuery) {
        out[kR4IngressOutQueryOk]           = res->queryOk;
        out[kR4IngressOutQueryState]        = res->queryOk ? res->queryState
                                                           : CX3_R4INGRESS_NOT_MEASURED;
        out[kR4IngressOutRssDefaultMatches] = res->queryOk ? res->queryDefaultQpnMatches
                                                           : CX3_R4INGRESS_NOT_MEASURED;
        out[kR4IngressOutRssBaseMatches]    = res->queryOk ? res->queryBaseQpnMatches
                                                           : CX3_R4INGRESS_NOT_MEASURED;
        out[kR4IngressOutQueryDefaultQpn]   = res->queryOk ? res->queryDefaultQpn
                                                           : CX3_R4INGRESS_NOT_MEASURED;
        out[kR4IngressOutQueryBaseQpn]      = res->queryOk ? res->queryBaseQpn
                                                           : CX3_R4INGRESS_NOT_MEASURED;
    }
}

/*
 * The QP half's comparison helper.  Declared here and defined below the selector, because it is
 * only ever called by exportR4Ingress and reading it in the order "what was sent, then what came
 * back" is easier with the structure definition in between.
 */
static void r4IngressQueryAndCompare(Mlx4Hw &hw, Cx3R4IngressResult *res,
                                    const UInt8 *sentContext,
                                    const UInt8 *keySent, UInt32 keyBytes);

/*
 * kCx3SelR4Ingress -- the port's MAC table, and the single-ring RSS indirection QP.
 *
 * Two things that do not produce a completion event between them, which is what makes this
 * stage's evidence different from every stage before it: "a frame arrived" cannot be the proof,
 * because this stage does not test addressed traffic. Evidence differs between the two halves:
 *
 *   the MAC table  the reference NEVER reads it back.  It keeps `entries[]`/`refs[]`/`total` in
 *                  software (mlx4_port.c:182-340) and trusts its own copy, so there is no
 *                  documented read operation. The former zero-input SET_PORT probe was another
 *                  write command, so it is disabled. macReadbackOk remains 0; actual addressed
 *                  reception must establish whether the write supplied working steering.
 *
 *   the indirection QP  a readback of the ordinary kind: three state transitions, each followed
 *                  by its own QUERY_QP, and then the fields that say whether the QP is still the
 *                  one that was written and still points at the queue stage 4 built.
 *
 * Three decisions are baked in here and each is written down where it is made, because a reader
 * who disagrees with one needs to know it was a decision rather than an oversight:
 *
 *   1. the MAC address comes from QUERY_PORT's def_mac, never from a literal;
 *   2. db_rec_addr is the reference's shifted form (`<< 2`), which is what the shipping driver
 *      sends for this QP (mlx4_en_resources.c:78) -- see r4IngressQueryAndCompare for why it is
 *      reported rather than judged, and the result document for the falsification condition;
 *   3. rss_rings = 1, so base_qpn's high byte is 0 and base_qpn == default_qpn == the RX QPN.
 *      The high-byte ENCODING is therefore not exercised by this stage (decision 3 in the plan).
 */
IOReturn Cx3Mlx4::exportR4Ingress(Cx3R4IngressResult *res, UInt64 *out, UInt32 port)
{
    if (port<1 || port>2 || !res || !out) return kIOReturnBadArgument;
    Cx3PortQueues &queue = fPortQueues[port-1];
    if ((port==2 && !queue.numbersReserved) || queue.setupFailed ||
        (queue.numbersReserved && queue.numberGeneration!=fDeviceGeneration)) {
        memset(res,0,sizeof(*res)); res->status=(UInt32)kIOReturnNotReady;
        memset(out,0,sizeof(UInt64)*kR4IngressOutCount);
        out[0]=(UInt64)kIOReturnNotReady; return kIOReturnSuccess;
    }

    UInt32 fwStatus = CX3_FWSTATUS_NOT_POSTED;
    UInt32 qpnProfileMax;
    UInt32 i, slot, measuredSlot;
    UInt8  context[QP_CONTEXT_SIZE];
    UInt8  key[MLX4_EN_RSS_KEY_SIZE];
    UInt8  defMac[8];
    UInt8  tableIn[MLX4_MAC_TABLE_SIZE];
    Cx3QueryPortResult qp;
    Mlx4Icm *rec = 0;
    UInt64 recPhys = 0, dbRecSent = 0;
    UInt32 prod = 0, prodBack = 0;
    bool fcsKeep = false;
    static const UInt16 stepOp[3] = { MLX4_CMD_RST2INIT_QP, MLX4_CMD_INIT2RTR_QP,
                                      MLX4_CMD_RTR2RTS_QP };
    static const UInt32 stepState[3] = { MLX4_QP_STATE_INIT, MLX4_QP_STATE_RTR,
                                         MLX4_QP_STATE_RTS };
    /*
     * counter_index = 0xFF, the reference's way of saying "no counter"
     * (mlx4_en_resources.c:66, whose guard on the next line is written as `counter_index !=
     * 0xFF`).  Same value and same reason as stage 4: this driver has reserved no counter.
     */
    const UInt32 counterIndex = 0xffu;

    memset(out, 0, sizeof(UInt64) * kR4IngressOutCount);
    memset(res, 0, sizeof(*res));
    memset(key, 0, sizeof(key));
    memset(defMac, 0, sizeof(defMac));
    memset(tableIn, 0, sizeof(tableIn));
    memset(&qp, 0, sizeof(qp));
    res->status = (UInt32)kIOReturnError;
    /* Every measured scalar starts as "never reached" -- see r4IngressPublishScalars. */
    for (i = 0; i < (UInt32)kR4IngressOutCount; ++i) out[i] = CX3_R4INGRESS_NOT_MEASURED;
    out[kR4IngressOutStatus] = res->status;

    if (queue.ingressRec && !queue.ingressUp) {
        LOG("r4ingress: unresolved setup owns QPN %u; refusing to overwrite it",
            (unsigned)queue.ingressQpn);
        res->status = (UInt32)kIOReturnNotReady;
        out[kR4IngressOutStatus] = res->status;
        return kIOReturnSuccess;
    }

    /*
     * The entrance conditions, named rather than assumed.  The indirection QP names the RX ring
     * by number and the CQ by number, so both have to exist; and its whole purpose is to point
     * at the queue stage 4 built, so that QP has to exist too -- there is no "RSS target" to
     * invent and no default that would be right.
     */
    if (!fHcaInited) {
        LOG("r4ingress refused: the HCA is not initialised (run m2dprofile/m2dbringup first)");
        res->status = (UInt32)kIOReturnNotReady;
        out[kR4IngressOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if (!queue.cqUp) {
        LOG("r4ingress refused: no completion queue exists (stage 2), so the indirection QP "
            "would name a CQ the hardware cannot report to");
        res->status = (UInt32)kIOReturnNotReady;
        out[kR4IngressOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if (!queue.qpUp || queue.qpState != MLX4_QP_STATE_RTS) {
        /*
         * The strongest of the three, and deliberately so: this stage's verdict is "the hash
         * target is the queue that receives".  With no RX QP there is no such queue, so
         * building an indirection QP here would produce a QP that points at a number nothing
         * answers to -- exactly the failure the stage exists to rule out.
         */
        LOG("r4ingress refused: the RX QP of stage 4 does not exist, and this stage's whole "
            "verdict is that the hash target is that queue (run r4qp first)");
        res->status = (UInt32)kIOReturnNotReady;
        out[kR4IngressOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    res->rxQpn = queue.qpQpn;
    res->pdn = fMrPdn;
    res->uarIndex = queue.cqUarStart;
    res->cqn = queue.cqCqn;
    res->rssRings = 1;                  /* decision 3: a single ring for this stage */
    res->rssKeyBytes = MLX4_EN_RSS_KEY_SIZE;
    res->hashFnSent = MLX4_RSS_HASH_TOP;
    res->rssFlagsSent = (UInt32)(MLX4_RSS_IPV4 | MLX4_RSS_TCP_IPV4 | MLX4_RSS_IPV6 |
                                 MLX4_RSS_TCP_IPV6);
    res->defaultQpnSent = res->rxQpn;
    res->baseQpnSent = (mlx4Log2(res->rssRings) << MLX4_RSS_QPN_HIGH_SHIFT) | res->rxQpn;
    res->baseQpnUdpSent = 0;            /* the profile does not ask for UDP RSS */
    /*
     * rqSizeStrideSent is NOT set here: it is read out of the context image after the fill, so
     * that "0" is a fact about the bytes in the mailbox rather than this line's intention.
     */
    res->counterIndexSent = counterIndex;
    res->schedQueueSent = (UInt32)(MLX4_QP_SCHED_QUEUE_ETH |
                                   ((port - 1u) <<
                                    MLX4_QP_SCHED_QUEUE_PORT_SHIFT));
    r4IngressBuildKey(key, MLX4_EN_RSS_KEY_SIZE);
    LOG("r4ingress: rss_rings = %u (decision 3), so ilog2 = %u and base_qpn's high byte is 0; "
        "base_qpn and default_qpn are both 0x%08x -- the RX QP of stage 4 -- by construction, "
        "which is why the high-byte ENCODING is not exercised by this stage",
        (unsigned)res->rssRings, (unsigned)mlx4Log2(res->rssRings),
        (unsigned)res->defaultQpnSent);

    /* ------------------------------------------------------------------ */
    /* half one: the MAC table                                             */
    /* ------------------------------------------------------------------ */

    /*
     * The shape of the command, on both paths: the table is always the whole 1024 bytes / 128
     * entries, whichever way this call got here.  Set on both so that a second call does not
     * print zeros -- a remembered number standing in for a measurement is D8's shape.
     */
    res->macTableBytes = MLX4_MAC_TABLE_SIZE;
    res->macEntries = MLX4_MAX_MAC_NUM;

    /*
     * The address, from the device.  QUERY_PORT is read-only, and def_mac is the field the plan
     * names: the address the card carries (stage 3's log already prints it, e.g.
     * `def_mac 00:11:22:33:44:55`).  Nothing is written down here: a literal would make the
     * driver register an address the card does not have, and no command would report that as
     * wrong -- the firmware would simply never match an incoming frame.
     */
    if (!fHw.queryPort(port, &qp)) {
        LOG("r4ingress: QUERY_PORT on port %u failed (fw status 0x%x), so this driver does not "
            "know the card's own address and will not guess one -- nothing was sent",
            (unsigned)port, (unsigned)qp.fwStatus);
        res->status = (UInt32)kIOReturnError;
        out[kR4IngressOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    for (i = 0; i < 8; ++i) defMac[i] = qp.defMac[i];
    LOG("r4ingress: QUERY_PORT port %u answered (fw status 0x%x): def_mac %02x:%02x:%02x:%02x:"
        "%02x:%02x (the BE64's bytes 0..7: %02x %02x %02x %02x %02x %02x %02x %02x), link_state "
        "%u. The MAC below comes from here and from nowhere else.",
        (unsigned)port, (unsigned)qp.fwStatus, defMac[2], defMac[3], defMac[4],
        defMac[5], defMac[6], defMac[7], defMac[0], defMac[1], defMac[2], defMac[3], defMac[4],
        defMac[5], defMac[6], defMac[7], (unsigned)qp.linkState);
    /*
     * The slot, and what is NOT claimed about it.
     *
     * THE DECISION IS 0.  The table this call sends has exactly one entry in it, so index 0 is
     * that table's first entry, and 0 is what this driver writes down.
     *
     * How this went wrong the first time, because it is why the comment is this long.  The slot
     * used to be "the first EMPTY entry, found by a scan" -- and the scan ran AFTER the address
     * had been written into slot 0, so it could not return 0: it returned 1, the command carried
     * the address in slot 1, and the log and the scalar both said "1" while this comment said
     * the scan "lands on 0".  The measurement was real; the reasoning about what it measured was
     * wrong (docs/DEFECTS.md D12, and the verifier is what caught it).  So the slot is decided
     * here, and the decision is checked against the bytes that are about to be sent:
     *
     *     the placement is   tableIn + slot * MLX4_MAC_ENTRY_SIZE      (one expression)
     *     the reported slot  is the first non-empty entry OF THAT TABLE, measured below
     *     a table with no non-empty entry at all is refused, not sent
     *
     * If the placement ever moves, the measurement moves with it, so the number the verifier
     * reads is the number the firmware was sent -- the previous revision's "slot 1 reported
     * against a table holding the address in slot 0" cannot happen silently again.
     *
     * WHY 0, and what is NOT established about it.
     *
     *   - The firmware's rule for a unicast MAC entry is `base_qpn + mac_index`
     *     (mlx4_en_netdev.c:573-577, the A0 branch: `*qpn = base_qpn + index`), where base_qpn
     *     is mlx4_get_base_qpn() = reserved_qps_base[MLX4_QP_REGION_ETH_ADDR]
     *     (mlx4_port.c:372-375).
     *   - That region's base is NOT 64, and is not knowable from here: reserved_qps_base[] is
     *     computed top-down from num_qps by subtracting each region's count (mlx4_qp.c:777-794),
     *     and the ETH_ADDR count is (1 << log_num_macs) * (1 << log_num_vlans) * num_ports
     *     (mlx4_main.c:545-548) -- four devcap fields this port does not decode.  64 is what
     *     QUERY_DEV_CAP's reserved_qps field holds, i.e. reserved_qps_cnt[MLX4_QP_REGION_FW]
     *     (mlx4_main.c:544), and it is also A0's DMFS base (dmfs_high_rate_qpn_base,
     *     mlx4_main.c:554-557).
     *   - So "index 0 maps to the ETH_ADDR region's base" is a source-level statement, while
     *     "index 0 maps to the RX QP of stage 4" is NOT established -- not by anything this
     *     driver has measured, and not by anything it can measure here.  Which QPN a MAC entry
     *     selects is a stage-7 question: under A0 the firmware is told the base by SET_PORT
     *     qpn_calc (mlx4_port.c:1578-1600), sent with priv->base_qpn
     *     (mlx4_en_netdev.c:782-785, :835), and that base has to come from
     *     mlx4_get_base_qpn() rather than from a number this port picked.
     *     docs/R4-STAGE5-RESULT.md carries this as an input to stage 7.
     *   - What IS established: the entry has to be somewhere, an empty slot is what a
     *     one-address table has to offer, and 0 is that table's first entry.  That the firmware
     *     REQUIRES index 0 is not established either -- nothing in the vendored tree says so,
     *     and the A0 path has at least one no-op in it (mlx4_multicast_attach returns 0 without
     *     doing anything, mlx4_mcg.c:1412-1415).
     */
    slot = 0;
    r4IngressMacEntry(defMac, tableIn + (UInt64)slot * MLX4_MAC_ENTRY_SIZE, &res->macValue);
    for (i = 0; i < 8; ++i) {
        res->mac[i] = tableIn[(UInt64)slot * MLX4_MAC_ENTRY_SIZE + i];
    }

    /*
     * The check on the decision, by measurement rather than by restating it: where IS the first
     * non-empty entry of the table that is about to be sent?  MLX4_MAX_MAC_NUM means "no
     * non-empty entry at all", which is a table with nothing to register and is refused below
     * rather than sent as 128 empty slots.
     */
    measuredSlot = MLX4_MAX_MAC_NUM;
    for (i = 0; i < MLX4_MAX_MAC_NUM; ++i) {
        UInt32 k;
        bool nonEmpty = false;
        for (k = 0; k < MLX4_MAC_ENTRY_SIZE; ++k) {
            if (tableIn[(UInt64)i * MLX4_MAC_ENTRY_SIZE + k] != 0) { nonEmpty = true; break; }
        }
        if (nonEmpty) { measuredSlot = i; break; }
    }
    if (measuredSlot != slot) {
        LOG("r4ingress: the address is not where this driver decided to put it: decided %u, and "
            "the first non-empty entry of the table about to be sent is %u (%u means no "
            "non-empty entry at all). Nothing was sent -- a table whose content disagrees with "
            "the driver's own decision is the shape of the defect this check exists for "
            "(docs/DEFECTS.md D12)", (unsigned)slot, (unsigned)measuredSlot,
            (unsigned)MLX4_MAX_MAC_NUM);
        res->status = (UInt32)kIOReturnError;
        out[kR4IngressOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    res->macSlot = measuredSlot;
    LOG("r4ingress: the MAC table's slot is %u -- decided here and then MEASURED out of the "
        "table being sent (the first non-empty of its %u entries), not restated: the number the "
        "verifier reads is the number that goes on the wire",
        (unsigned)res->macSlot, (unsigned)MLX4_MAX_MAC_NUM);

    if (!fHw.setPortMacTableWrite(port, tableIn, MLX4_MAC_TABLE_SIZE, &fwStatus)) {
        /*
         * A refused MAC command ends this half but not the call: the QP half does not depend on
         * it, and a reader wants both answers out of one reboot.  The MAC verdict is already a
         * failure, which the scalars carry; nothing here is remembered as registered.
         */
        res->macFwStatus = fwStatus;
        LOG("r4ingress: the MAC table command was refused (fw status 0x%x). Building the "
            "indirection QP anyway -- the two halves are independent and both answers are worth "
            "having from one run. The MAC verdict is a failure.", (unsigned)fwStatus);
    } else {
        res->macFwStatus = fwStatus;
        queue.macUp = true;
        queue.macSlot = measuredSlot;
        for (i = 0; i < 8; ++i) {
            queue.mac[i] = tableIn[(UInt64)slot * MLX4_MAC_ENTRY_SIZE + i];
        }
        queue.macValue = res->macValue;
    }

    /* SET_PORT_MAC_TABLE has no documented read direction. Earlier builds
     * sent it again with in_param=0, which can change the table rather than
     * observe it. Keep readback explicitly unavailable and issue no command.
     * The remaining proof is the write status plus addressed packet reception. */
    res->macReadFwStatus = CX3_FWSTATUS_NOT_POSTED;
    res->macReadbackOk = 0;
    res->macReadbackMatches = 0;
    LOG("r4ingress: MAC readback unavailable: speculative SET_PORT read disabled; "
        "no read command posted, write fw status 0x%x", (unsigned)res->macFwStatus);

    /* ------------------------------------------------------------------ */
    /* half two: the RSS indirection QP                                    */
    /* ------------------------------------------------------------------ */

    /*
     * The QP number's RULE -- the firmware's reserved share as the floor and the profile's
     * configured ceiling as the bound -- is computed here rather than inside the fresh path,
     * because both paths report it: a second call that printed 0 for the rule would be showing a
     * number nobody measured (D8's shape), and the rule is a function of the device and the
     * profile rather than of this call.
     */
    res->qpnRule = fDevCap.reserved_qps;
    qpnProfileMax = (fInitHca.log_num_qps < 32u) ? (1u << fInitHca.log_num_qps)
                                                 : 0xffffffffu;
    res->qpnProfileMax = qpnProfileMax;

    /*
     * Idempotence, like every other stage: a second call must not allocate a second record nor
     * re-send three transitions for a QP the firmware already has, because the first run's
     * record is the one its context names.  The existing QP is re-MEASURED rather than reported
     * from memory -- a path that replays remembered numbers cannot notice the device disagreeing
     * -- and the MAC half above ran again on this path on purpose: re-sending the same 1024-byte
     * table is what the reference does on every change (mlx4_port.c:299-301), so it is a
     * supported operation rather than a risk.
     */
    if (queue.ingressUp) {
        res->qpn = queue.ingressQpn;
        res->mttOffset = queue.rqMttOff;
        res->mttAddr = queue.rqMttAddr;
        /*
         * Re-derived rather than remembered, exactly as stage 4's idempotent path does it: this
         * call sent nothing, so "what went into the context" does not exist for it.  What does
         * exist is what this driver's fill produces for this QP -- a function of the WQE size, the
         * CQN, the UAR and the port this stage uses -- and that is what is reported.  The
         * firmware's own copy is printed beside it by the comparison below, and that is the
         * measurement.
         */
        res->sqSizeStrideSent = (UInt32)(mlx4Log2(MLX4_QP_WQE_SIZE_DUMMY_SQ) -
                                        MLX4_QP_STRIDE_SUB);
        /*
         * rq_size_stride is 0 because this driver's fill never writes it for an indirection QP
         * (the reference's `!is_tx && !rss`), so it is the zero the clear left -- derived here
         * the same way the fresh path reads it out of the context.  It is reported and not
         * judged: the firmware's copy is printed beside it because the field is not part of what
         * this driver sent for this QP.
         */
        res->rqSizeStrideSent = 0;
        res->mtuMsgmaxSent = MLX4_QP_MTU_MSGMAX_ETH;
        res->schedQueueSent = (UInt32)(MLX4_QP_SCHED_QUEUE_ETH |
                                       ((port - 1u) <<
                                        MLX4_QP_SCHED_QUEUE_PORT_SHIFT));
        res->param3Sent = ((fDevCap.flags & MLX4_DEV_CAP_FLAG_FCS_KEEP) != 0)
                              ? MLX4_QP_PARAM3_FCS_KEEP : 0u;
        res->counterIndexSent = counterIndex;
        res->producerValue = queue.ingressProd;
        res->dbRecordPhys = queue.ingressRecPhys;
        res->dbRecAddrSent = queue.ingressRecPhys << 2;
        res->dbRecAddrShifted = 1;
        r4IngressQueryAndCompare(fHw, res, /*sentContext=*/0, key, MLX4_EN_RSS_KEY_SIZE);
        if (queue.ingressRec) {
            prodBack = OSSwapBigToHostInt32(((volatile UInt32 *)queue.ingressRec->virt)[0]);
        }
        res->producerReadback = prodBack;
        res->producerMatches = (prodBack == res->producerValue) ? 1u : 0u;
        if (!res->queryOk || !res->queryAllFieldsMatch || !res->queryMttMatches ||
            !res->queryRssMatches || !res->pointsAtAnotherQp || !res->producerMatches) {
            queue.ingressUp = false;
            queue.ingressState = CX3_R4INGRESS_NOT_MEASURED;
            if (port==1) fNetEnabled = false;
            res->status = (UInt32)kIOReturnNotReady;
            r4IngressPublishScalars(res, out, true);
            out[kR4IngressOutStatus] = res->status;
            return kIOReturnSuccess;
        }
        LOG("r4ingress: the indirection QPN %u already exists (state last written %u); measured "
            "it instead of building a second one: query state %u, local_qpn %u, the overlay %s, "
            "the record holds %u",
            (unsigned)res->qpn, (unsigned)queue.ingressState, (unsigned)res->queryState,
            (unsigned)res->queryLocalQpn,
            res->queryRssMatches ? "matches" : "MISMATCH",
            (unsigned)prodBack);
        res->status = 0;
        out[kR4IngressOutStatus] = 0;
        r4IngressPublishScalars(res, out, true);
        return kIOReturnSuccess;
    }

    /*
     * The RX QPN has to be inside the range software may use, because this call reasons about the
     * entry QP's number FROM it (+1 below).  If stage 4 ever handed back something outside the
     * range, the +1 would be meaningless and refusing is the only honest answer.
     */
    if (res->rxQpn >= qpnProfileMax || res->rxQpn < fDevCap.reserved_qps) {
        LOG("r4ingress: the RX QPN %u reported by stage 4 is not inside the range software may "
            "use (%u..%u), so this call cannot reason about the entry QP's number either -- "
            "nothing was sent", (unsigned)res->rxQpn, (unsigned)fDevCap.reserved_qps,
            (unsigned)(qpnProfileMax ? qpnProfileMax - 1u : 0u));
        res->status = (UInt32)kIOReturnBadArgument;
        out[kR4IngressOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    /*
     * The entry QP takes the RX QP's number plus one.  Both were checked against the range above,
     * and the +1 is stated rather than hidden: it is a CHOICE about which number to use (nothing
     * in the reference fixes it -- mlx4_qp_reserve_range hands out a range and this port reserves
     * none), and it is what guarantees the two QPs differ, which is the whole point of
     * `pointsAtAnotherQp` below.  If the next number were taken, the check below would refuse
     * rather than build a QP that steers into itself.
     */
    res->qpn = queue.numbersReserved ? queue.numbers.rssQpn : res->rxQpn + 1u;
    if (res->qpn >= qpnProfileMax) {
        LOG("r4ingress: the entry QPN would be %u, which is at or past the %u QPs this profile "
            "configured (the RX QP took %u); nothing was sent. Raising the profile's QP count is "
            "the fix, not reusing the RX QP's number -- a QP that steers into itself reaches RTS "
            "and loses every hashed frame.",
            (unsigned)res->qpn, (unsigned)qpnProfileMax, (unsigned)res->rxQpn);
        res->status = (UInt32)kIOReturnNoSpace;
        out[kR4IngressOutStatus] = res->status;
        return kIOReturnSuccess;
    }

    /*
     * The producer record, for the field's sake rather than for a queue's: an indirection QP has
     * no receive queue and produces no completions, so nothing should ever read this record --
     * but db_rec_addr is part of the context the fill writes, so it names something.  Allocating
     * it means that whatever the field means, it points at memory this driver owns, cleared, and
     * holding the same producer value the RX QP published.
     */
    rec = fHw.icmAlloc(1, false);
    if (!rec) {
        LOG("r4ingress: could not allocate the indirection QP's producer record");
        res->status = (UInt32)kIOReturnNoMemory;
        out[kR4IngressOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    memset(rec->virt, 0, (size_t)rec->bytes);
    recPhys = rec->pages[0].busAddr;
    res->dbRecordPhys = recPhys;
    /*
     * DECISION 2: the reference's shifted form.  The fill writes `priv->res.db.dma << 2`
     * (mlx4_en_resources.c:78) and NOTHING overwrites it for the indirection QP, while the RX
     * ring's own line overwrites the same field with the UNSHIFTED address (mlx4_en_rx.c:947 --
     * which is what stage 4 sends and what its readback confirmed).  Those are two different
     * values for one field of one struct, and neither the header nor any comment in the tree
     * says what the field means, so this is a copy of the shipping driver rather than a
     * derivation.  It is expected not to be read at all: this QP produces no completions, so no
     * consumer ever advances an index here.
     */
    dbRecSent = recPhys << 2;
    res->dbRecAddrSent = dbRecSent;
    res->dbRecAddrShifted = 1;
    LOG("r4ingress: the indirection QP's db_rec_addr is the reference's SHIFTED form: record at "
        "0x%llx, sent 0x%llx (`priv->res.db.dma << 2`, mlx4_en_resources.c:78 -- not overwritten "
        "for this QP; the RX ring's line at mlx4_en_rx.c:947 overwrites it with the unshifted "
        "address, which is what stage 4 sent). The field's semantics are not documented and this "
        "QP produces no completions, so it is expected not to be read; the record is allocated "
        "and filled anyway so that either form names memory this driver owns.",
        (unsigned long long)recPhys, (unsigned long long)dbRecSent);

    fcsKeep = (fDevCap.flags & MLX4_DEV_CAP_FLAG_FCS_KEEP) != 0;
    res->param3Sent = fcsKeep ? MLX4_QP_PARAM3_FCS_KEEP : 0u;
    /*
     * The context: the fill, then the overlay.  Two calls, in that order, because that is the
     * reference's order and because the second REINTERPRETS bytes the first wrote -- pri_path's
     * first 0x40 bytes carry the RSS struct for an indirection QP, so an overlay applied before
     * the fill would be partly overwritten by it.
     *
     * rqEntries and rqStride are passed even though the fill will NOT write rq_size_stride for an
     * RSS QP: they are parameters of a function that serves both kinds of QP, and passing the
     * real ring's values keeps this call site identical to stage 4's -- the difference between
     * the two lives inside the fill, where the reference's `!is_tx && !rss` lives.
     */
    fHw.qpFillContext(context, fMrPdn, res->qpn, queue.cqUarStart, queue.rqNent,
                      MLX4_QP_WQE_SIZE_RQ, queue.cqCqn, dbRecSent, MLX4_QP_MTU_MSGMAX_ETH,
                      port, fcsKeep, counterIndex, /*rss=*/true, /*isTx=*/false,
                      /*userPrio=*/-1);
    /*
     * Read all four back OUT of the image that is about to be sent rather than restating what
     * the arguments were: the claim this stage makes is about the bytes in the mailbox -- in
     * particular that rq_size_stride is 0, which is the one field the rss branch deliberately
     * does not write -- and a value taken from the buffer cannot disagree with the buffer.
     */
    res->rqSizeStrideSent = context[QP_CONTEXT_RQ_SIZE_STRIDE_OFFSET];
    res->sqSizeStrideSent = context[QP_CONTEXT_SQ_SIZE_STRIDE_OFFSET];
    res->mtuMsgmaxSent    = context[QP_CONTEXT_MTU_MSGMAX_OFFSET];
    res->schedQueueSent   = context[QP_CONTEXT_PRI_SCHED_QUEUE_OFFSET];
    fHw.qpFillRssContext(context, res->rxQpn, res->rssRings, res->rxQpn,
                         /*udpRss=*/false, key, MLX4_EN_RSS_KEY_SIZE);
    res->pdn = fMrPdn;
    res->mttOffset = queue.rqMttOff;
    res->mttAddr = queue.rqMttAddr;
    if ((res->mttAddr >> 32) != 0) {
        /*
         * mtt_base_addr_h is a single byte, so the byte address has to fit in 32 bits.  Refused
         * rather than truncated, for the same reason stage 4 refuses: a truncated MTT address is
         * a QP whose descriptors come from somewhere else.
         */
        LOG("r4ingress: the MTT byte address 0x%llx does not fit in mtt_base_addr_l (the high "
            "byte would be 0x%02x) -- refusing rather than truncating",
            (unsigned long long)res->mttAddr, (unsigned)(res->mttAddr >> 32));
        fHw.icmFree(rec);
        res->status = (UInt32)kIOReturnUnsupported;
        out[kR4IngressOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    /*
     * The MTT fields, written into the shared image just before the first command.  The
     * indirection QP names the RX RING's MTT entry rather than one of its own: the reference
     * passes `priv->res.mtt` -- the single MTT allocation made for the rx qps
     * (mlx4_alloc_hwq_res in mlx4_en_netdev.c:2236) -- to mlx4_qp_to_ready for the indirection QP
     * (mlx4_en_rx.c:1076-1078), and this call does the same with the ring's entry.  It is the one
     * place where this stage's QP shares something with stage 4's, and it is not a mistake: the
     * field belongs to the RST->INIT step alone and the firmware is not expected to follow it for
     * a QP that receives nothing.
     */
    context[QP_CONTEXT_LOG_PAGE_SIZE_OFFSET] = 0;
    context[QP_CONTEXT_MTT_BASE_H_OFFSET] = 0;
    {
        UInt32 lo = (UInt32)(res->mttAddr & 0xffffffffull);
        context[QP_CONTEXT_MTT_BASE_L_OFFSET + 0] = (UInt8)((lo >> 24) & 0xffu);
        context[QP_CONTEXT_MTT_BASE_L_OFFSET + 1] = (UInt8)((lo >> 16) & 0xffu);
        context[QP_CONTEXT_MTT_BASE_L_OFFSET + 2] = (UInt8)((lo >>  8) & 0xffu);
        context[QP_CONTEXT_MTT_BASE_L_OFFSET + 3] = (UInt8)(lo & 0xffu);
    }
    LOG("r4ingress: the indirection QPN %u above the RX QPN %u, context at mailbox +%u, %u "
        "bytes; it names the RX ring's MTT entry %u -> 0x%llx (the reference passes priv->res.mtt "
        "for the indirection QP too, mlx4_en_rx.c:1076-1078), log_page_size 0 (RST->INIT only)",
        (unsigned)res->qpn, (unsigned)res->rxQpn, (unsigned)QP_CONTEXT_MAILBOX_OFFSET,
        (unsigned)QP_CONTEXT_SIZE, (unsigned)res->mttOffset,
        (unsigned long long)res->mttAddr);

    queue.ingressRec = rec;
    queue.ingressRecPhys = recPhys;
    queue.ingressQpn = res->qpn;
    queue.ingressProd = 0;
    queue.ingressState = CX3_R4INGRESS_NOT_MEASURED;
    queue.ingressUp = false;

    {
        Mlx4Icm *dmaOwners[3] = { queue.ingressRec, queue.rqRing, queue.rqBufs };
        if (!fHw.markDmaHandoff(dmaOwners, 3)) {
            res->status = (UInt32)kIOReturnNotReady;
            LOG("r4ingress: refusing QP transition because a referenced DMA owner has no generation");
            out[kR4IngressOutStatus] = res->status;
            return kIOReturnSuccess;
        }
    }

    /* --- layer 1: the three transitions, each followed by its own query --- */
    for (i = 0; i < 3; ++i) {
        UInt32 flags;
        flags  = cx3GetBe32(context + QP_CONTEXT_FLAGS_OFFSET);
        flags &= ~(0xfu << MLX4_QP_STATE_NIBBLE_SHIFT);
        flags |= (stepState[i] << MLX4_QP_STATE_NIBBLE_SHIFT);
        res->stepOpcode[i]      = stepOp[i];
        res->stepStateWanted[i] = stepState[i];
        context[QP_CONTEXT_FLAGS_OFFSET + 0] = (UInt8)((flags >> 24) & 0xffu);
        context[QP_CONTEXT_FLAGS_OFFSET + 1] = (UInt8)((flags >> 16) & 0xffu);
        context[QP_CONTEXT_FLAGS_OFFSET + 2] = (UInt8)((flags >>  8) & 0xffu);
        context[QP_CONTEXT_FLAGS_OFFSET + 3] = (UInt8)(flags & 0xffu);
        /* params2's FPP bit: the reference clears it for every state except RTR
         * (mlx4_qp.c:915-916).  This context starts zeroed, so the clear is a no-op; named here
         * rather than left as a missing line (same note as stage 4). */

        if (!fHw.qpModify(stepOp[i], res->qpn, context, &fwStatus,
                          res->rawSentMailboxStep[i], sizeof(res->rawSentMailboxStep[i]),
                          &res->modifyCapture[i])) {
            res->stepFwStatus[i]   = fwStatus;
            res->stepQueryOk[i]    = 0;
            res->stepStateAfter[i] = CX3_R4INGRESS_NOT_MEASURED;
            res->statesConfirmed   = 0;
            if (fwStatus == CX3_FWSTATUS_TIMEOUT) {
                LOG("r4ingress: step %u (opcode 0x%02x) TIMED OUT after being posted, so the "
                    "QP's state is UNKNOWN. Nothing is torn down: the device may still own this "
                    "QP, and 2RST_QP on a QP whose state is unknown is a command this port does "
                    "not send blind.", (unsigned)i + 1, (unsigned)stepOp[i]);
            } else {
                LOG("r4ingress: step %u (opcode 0x%02x, state %u) FAILED with fw status 0x%x. "
                    "For this stage the first thing to check is the RSS bit in flags (bit %u) and "
                    "the overlay at QPC + 0x%02x: an indirection QP whose flags word lost that bit "
                    "is an ordinary queue to the firmware, while the overlay still travels in the "
                    "mailbox. A BAD_PARAM (0x3) on the first step is also this stage's signature "
                    "for db_rec_addr -- see decision 2 in docs/R4-STAGE5-RESULT.md.",
                    (unsigned)i + 1, (unsigned)stepOp[i], (unsigned)stepState[i],
                    (unsigned)fwStatus, (unsigned)MLX4_RSS_QPC_FLAG_OFFSET,
                    (unsigned)QP_CONTEXT_RSS_BASE_QPN_OFFSET);
            }
            res->stepsAccepted = i;
            res->status = 0;      /* the selector answered; the device refused */
            if (port==1) fNetEnabled = false;
            r4IngressPublishScalars(res, out, false);
            out[kR4IngressOutStatus] = 0;
            if (i != 0 || (fwStatus != CX3_FWSTATUS_NOT_POSTED &&
                           fwStatus != CX3_FWSTATUS_WEDGED &&
                           fwStatus != CX3_FWSTATUS_NO_DEVICE)) {
                queue.ingressRec = rec;
                queue.ingressRecPhys = recPhys;
                queue.ingressQpn = res->qpn;
                queue.ingressProd = 0;
                queue.ingressState = CX3_R4INGRESS_NOT_MEASURED;
                queue.ingressUp = false;
                LOG("r4ingress: failed setup retained with its producer record");
            } else {
                fHw.icmFree(rec);
                queue.ingressRec = 0;
                queue.ingressRecPhys = 0;
            }
            return kIOReturnSuccess;
        }
        res->stepFwStatus[i] = fwStatus;
        res->stepsAccepted = i + 1;

        {
            UInt8 q[QP_CONTEXT_SIZE];
            UInt32 qStatus = CX3_FWSTATUS_NOT_POSTED;
            memset(q, 0, sizeof(q));
            if (fHw.qpQuery(res->qpn, q, QP_CONTEXT_SIZE, &qStatus,
                            res->rawQueryMailboxStep[i], sizeof(res->rawQueryMailboxStep[i]),
                            &res->queryCapture[i])) {
                UInt32 st = (cx3GetBe32(q + QP_CONTEXT_FLAGS_OFFSET) >>
                             MLX4_QP_STATE_NIBBLE_SHIFT) & 0xfu;
                UInt32 backQpn = cx3GetBe32(q + QP_CONTEXT_LOCAL_QPN_OFFSET);
                res->stepQueryOk[i]    = 1;
                res->stepStateAfter[i] = st;
                if (st == stepState[i] && backQpn == res->qpn)
                    res->statesConfirmed = i + 1;
                LOG("r4ingress: step %u accepted (fw status 0x%x); QUERY_QP says state %u, "
                    "local_qpn %u -- %s", (unsigned)i + 1, (unsigned)fwStatus, (unsigned)st,
                    (unsigned)backQpn,
                    (st == stepState[i] && backQpn == res->qpn)
                        ? "the state and the QP number are what was written"
                        : "NOT what was written: the command was accepted and the state was not");
            } else {
                res->stepQueryOk[i]    = 0;
                res->stepStateAfter[i] = CX3_R4INGRESS_NOT_MEASURED;
                LOG("r4ingress: step %u returned fw status 0x%x but QUERY_QP did not answer (fw "
                    "status 0x%x), so the state after this step is UNKNOWN -- two different "
                    "reasons to stop", (unsigned)i + 1, (unsigned)fwStatus, (unsigned)qStatus);
            }
        }
        if (res->statesConfirmed != i + 1) {
            if (port==1) fNetEnabled = false;
            res->status = (UInt32)kIOReturnNotReady;
            r4IngressPublishScalars(res, out, false);
            out[kR4IngressOutStatus] = res->status;
            return kIOReturnSuccess;
        }
    }

    /* --- layer 2: the last query, field by field --- */
    r4IngressQueryAndCompare(fHw, res, context, key, MLX4_EN_RSS_KEY_SIZE);
    if (!res->queryOk || !res->queryAllFieldsMatch || !res->queryMttMatches ||
        !res->queryRssMatches || !res->pointsAtAnotherQp) {
        if (port==1) fNetEnabled = false;
        res->status = (UInt32)kIOReturnNotReady;
        r4IngressPublishScalars(res, out, true);
        out[kR4IngressOutStatus] = res->status;
        return kIOReturnSuccess;
    }

    /* --- the record, so that the field names memory this driver owns --- */
    prod = queue.rqNent;               /* the same producer value the RX QP published */
    fHw.rqSetProducer((volatile UInt32 *)rec->virt, prod);
    prodBack = OSSwapBigToHostInt32(((volatile UInt32 *)rec->virt)[0]);
    res->producerValue    = prod;
    res->producerReadback = prodBack;
    res->producerMatches  = (prodBack == prod) ? 1u : 0u;
    LOG("r4ingress: the indirection QP's record at 0x%llx holds the same producer value the RX "
        "QP published (%u), read back as %u (%s). Nothing is expected to consume it -- this QP "
        "has no receive queue and produces no completions -- so this is a statement about the "
        "memory the context names, not about traffic.",
        (unsigned long long)recPhys, (unsigned)prod, (unsigned)prodBack,
        res->producerMatches ? "matches" : "MISMATCH");
    if (!res->producerMatches) {
        /*
         * The same refusal stage 4 makes, for the same reason: a QP whose context names memory
         * that does not hold what was written should not be left live.  Put it back first, and
         * release the record only if that worked.
         */
        UInt32 rstStatus = CX3_FWSTATUS_NOT_POSTED;
        LOG("r4ingress: the record did not hold what was written, so the indirection QP is put "
            "back to RST and the record released rather than left published");
        if (port==1) fNetEnabled = false;
        if (fHw.qpToReset(res->qpn, &rstStatus)) {
            LOG("r4ingress: the indirection QPN %u is back in RST (fw status 0x%x)",
                (unsigned)res->qpn, (unsigned)rstStatus);
            fHw.icmFree(rec);
            queue.ingressRec = 0;
            queue.ingressRecPhys = 0;
        } else {
            LOG("r4ingress: putting the indirection QPN %u back to RST failed (fw status 0x%x), "
                "so the QP is left as it is and the record is deliberately NOT released; a "
                "reboot reclaims both", (unsigned)res->qpn, (unsigned)rstStatus);
            queue.ingressRec = rec;
            queue.ingressRecPhys = recPhys;
            queue.ingressQpn = res->qpn;
            queue.ingressProd = 0;
            queue.ingressState = CX3_R4INGRESS_NOT_MEASURED;
            queue.ingressUp = false;
        }
        res->status = (UInt32)kIOReturnError;
        r4IngressPublishScalars(res, out, true);
        out[kR4IngressOutStatus] = res->status;
        return kIOReturnSuccess;
    }

    queue.ingressRec     = rec;
    queue.ingressRecPhys = recPhys;
    queue.ingressQpn     = res->qpn;
    queue.ingressState   = MLX4_QP_STATE_RTS;
    queue.ingressProd    = prod;
    queue.ingressUp      = true;

    res->status = 0;
    out[kR4IngressOutStatus] = 0;
    r4IngressPublishScalars(res, out, true);
    LOG("r4ingress OK: MAC %02x:%02x:%02x:%02x:%02x:%02x in table slot %u (fw status 0x%x, "
        "read back %s), and the indirection QPN %u is in RTS steering into the RX QP %u. "
        "Commands accepted %u/3, states confirmed by QUERY_QP %u/3, the overlay %s, the queue "
        "fields %s, base_qpn 0x%08x, default_qpn 0x%08x, and the entry QP is not the RX QP "
        "(%s). Whether ARP or anything else arrives is stages 6-10.",
        res->mac[2], res->mac[3], res->mac[4], res->mac[5], res->mac[6], res->mac[7],
        (unsigned)res->macSlot, (unsigned)res->macFwStatus,
        res->macReadbackOk ? (res->macReadbackMatches ? "identical" : "DIFFERENT")
                           : "not available (see the scalar)",
        (unsigned)res->qpn, (unsigned)res->rxQpn, (unsigned)res->stepsAccepted,
        (unsigned)res->statesConfirmed,
        res->queryRssMatches ? "survived" : "DID NOT survive",
        res->queryAllFieldsMatch ? "match" : "DO NOT match",
        (unsigned)res->queryBaseQpn, (unsigned)res->queryDefaultQpn,
        res->pointsAtAnotherQp ? "so it steers somewhere else" : "NO -- it steers into itself");
    return kIOReturnSuccess;
}

/*
 * The comparison half of the ingress QP: one QUERY_QP, put next to what was sent.
 *
 * Three groups, kept apart because they answer different questions:
 *
 *   the queue fields  sq_size_stride, cqn_send, cqn_recv, mtu_msgmax and local_qpn -- the fields
 *                     the reference's semantics require to survive.  rq_size_stride is NOT in
 *                     this group: an indirection QP does not send one (mlx4_en_resources.c:52),
 *                     so there is nothing to compare it against.  It is reported, not judged.
 *   the overlay       base_qpn, default_qpn, the reserved word, hash_fn, flags and the forty key
 *                     bytes: "is this still an indirection QP pointing where we aimed it".
 *                     default_qpn is the field the stage's verdict rests on.
 *   db_rec_addr       reported only.  What this port sends is the reference's shifted value
 *                     (mlx4_en_resources.c:78), which is a decision rather than a measurement,
 *                     so comparing it would be comparing the decision with itself -- and the
 *                     readback's copy is printed beside the sent one for the reader.
 */
static void r4IngressQueryAndCompare(Mlx4Hw &hw, Cx3R4IngressResult *res,
                                     const UInt8 *sentContext,
                                     const UInt8 *keySent, UInt32 keyBytes)
{
    UInt8 q[QP_CONTEXT_SIZE];
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    UInt32 mask = 0, i;
    UInt32 keyOk = 1;
    UInt32 mttH = 0;
    UInt64 mttL = 0;

    memset(q, 0, sizeof(q));
    res->queryFwStatus = CX3_FWSTATUS_NOT_POSTED;
    /* Keep the bytes that went in; null on the idempotent path, which sends nothing. */
    if (sentContext) memcpy(res->rawSentQpc, sentContext, QP_CONTEXT_SIZE);
    if (!hw.qpQuery(res->qpn, q, QP_CONTEXT_SIZE, &status,
                    res->rawQueryMailboxFinal, sizeof(res->rawQueryMailboxFinal),
                    &res->finalQueryCapture)) {
        res->queryFwStatus = status;
        res->queryOk = 0;
        LOG("r4ingress: QUERY_QP on the indirection QPN %u did not answer (fw status 0x%x), so "
            "what the firmware holds is UNKNOWN -- the command may have been accepted and the "
            "query refused, and those two look the same from here",
            (unsigned)res->qpn, (unsigned)status);
        return;
    }
    res->queryFwStatus = status;
    res->queryOk = 1;
    /* Verbatim answer, before any field decode -- see the field comment in cx3_abi.h. */
    memcpy(res->rawQueriedQpc, q, QP_CONTEXT_SIZE);
    res->queryFlagsRaw    = cx3GetBe32(q + QP_CONTEXT_FLAGS_OFFSET);
    res->queryState       = (res->queryFlagsRaw >> MLX4_QP_STATE_NIBBLE_SHIFT) & 0xfu;
    res->queryLocalQpn    = cx3GetBe32(q + QP_CONTEXT_LOCAL_QPN_OFFSET);
    res->querySqSizeStride = q[QP_CONTEXT_SQ_SIZE_STRIDE_OFFSET];
    res->queryRqSizeStride = q[QP_CONTEXT_RQ_SIZE_STRIDE_OFFSET];
    res->queryMtuMsgmax   = q[QP_CONTEXT_MTU_MSGMAX_OFFSET];
    res->queryCqnSend     = cx3GetBe32(q + QP_CONTEXT_CQN_SEND_OFFSET);
    res->queryCqnRecv     = cx3GetBe32(q + QP_CONTEXT_CQN_RECV_OFFSET);
    res->queryDbRecAddr   = cx3GetBe64(q + QP_CONTEXT_DB_REC_ADDR_OFFSET);
    mttH = q[QP_CONTEXT_MTT_BASE_H_OFFSET];
    mttL = (UInt64)cx3GetBe32(q + QP_CONTEXT_MTT_BASE_L_OFFSET);
    res->queryMttMatches = (mttH == (UInt32)(res->mttAddr >> 32) &&
                            mttL == (res->mttAddr & 0xffffffffull) &&
                            q[QP_CONTEXT_LOG_PAGE_SIZE_OFFSET] == 0) ? 1u : 0u;
    res->queryParam3     = cx3GetBe32(q + QP_CONTEXT_PARAM3_OFFSET);
    res->querySchedQueue = q[QP_CONTEXT_PRI_SCHED_QUEUE_OFFSET];

    if (res->queryState == MLX4_QP_STATE_RTS)       mask |= 1u << kR4IngressFieldState;
    if (res->queryLocalQpn == res->qpn)             mask |= 1u << kR4IngressFieldLocalQpn;
    if (res->querySqSizeStride == res->sqSizeStrideSent)
        mask |= 1u << kR4IngressFieldSqSize;
    if (res->queryCqnSend == res->cqn)              mask |= 1u << kR4IngressFieldCqnSend;
    if (res->queryCqnRecv == res->cqn)              mask |= 1u << kR4IngressFieldCqnRecv;
    if (res->queryMtuMsgmax == res->mtuMsgmaxSent)  mask |= 1u << kR4IngressFieldMtuMsgmax;
    res->queryFieldMask = mask;
    res->queryAllFieldsMatch =
        (mask == ((1u << kR4IngressFieldState) | (1u << kR4IngressFieldLocalQpn) |
                  (1u << kR4IngressFieldSqSize) | (1u << kR4IngressFieldCqnSend) |
                  (1u << kR4IngressFieldCqnRecv) |
                  (1u << kR4IngressFieldMtuMsgmax))) ? 1u : 0u;

    /* --- the overlay --- */
    res->queryBaseQpn    = cx3GetBe32(q + QP_CONTEXT_RSS_BASE_QPN_OFFSET);
    res->queryDefaultQpn = cx3GetBe32(q + QP_CONTEXT_RSS_DEFAULT_QPN_OFFSET);
    res->queryReserved   = ((UInt32)q[QP_CONTEXT_RSS_RESERVED_OFFSET] << 8) |
                           (UInt32)q[QP_CONTEXT_RSS_RESERVED_OFFSET + 1];
    res->queryHashFn     = q[QP_CONTEXT_RSS_HASH_FN_OFFSET];
    res->queryRssFlags   = q[QP_CONTEXT_RSS_FLAGS_OFFSET];
    res->queryBaseQpnUdp = cx3GetBe32(q + QP_CONTEXT_RSS_BASE_QPN_UDP_OFFSET);
    res->queryRssHighByte = (res->queryBaseQpn >> 24) & 0xffu;
    if (keySent && keyBytes) {
        for (i = 0; i < keyBytes && i < MLX4_EN_RSS_KEY_SIZE; ++i) {
            if (q[QP_CONTEXT_RSS_KEY_OFFSET + i] != keySent[i]) keyOk = 0;
        }
    } else {
        keyOk = 0;      /* nothing was sent, so nothing can be said to have survived */
    }
    res->queryRssKeyMatches     = keyOk;
    res->queryBaseQpnMatches    = (res->queryBaseQpn == res->baseQpnSent) ? 1u : 0u;
    res->queryDefaultQpnMatches = (res->queryDefaultQpn == res->defaultQpnSent) ? 1u : 0u;
    res->queryRssMatches =
        ((res->queryFlagsRaw & (1u << MLX4_RSS_QPC_FLAG_OFFSET)) &&
         res->queryBaseQpnMatches && res->queryDefaultQpnMatches && keyOk &&
         res->queryHashFn == (UInt32)MLX4_RSS_HASH_TOP &&
         res->queryRssFlags == res->rssFlagsSent &&
         res->queryReserved == 0 && res->queryBaseQpnUdp == 0) ? 1u : 0u;
    /*
     * The stage's own failure mode, as one number: an indirection QP whose hash target is
     * itself.  Every command of the sequence is happy with that QP -- it reaches RTS, its
     * fields match, its overlay reads back -- and it steers hashed traffic into a queue with no
     * receive ring, so the frames are lost with nothing in the output to say why.
     */
    res->pointsAtAnotherQp =
        (res->queryDefaultQpn != res->qpn && res->queryDefaultQpnMatches) ? 1u : 0u;

    LOG("r4ingress: QUERY_QP on the indirection QPN %u: fw status 0x%x; the firmware's own "
        "view: state %u (flags 0x%08x), local_qpn %u, sq_size_stride 0x%02x, rq_size_stride "
        "0x%02x (not judged: an indirection QP sends none), cqn_send %u, cqn_recv %u, "
        "mtu_msgmax 0x%02x, db_rec_addr 0x%llx (sent 0x%llx: the reference's shifted form), "
        "mtt_base h 0x%02x / l 0x%08x",
        (unsigned)res->qpn, (unsigned)res->queryFwStatus, (unsigned)res->queryState,
        (unsigned)res->queryFlagsRaw, (unsigned)res->queryLocalQpn,
        (unsigned)res->querySqSizeStride, (unsigned)res->queryRqSizeStride,
        (unsigned)res->queryCqnSend, (unsigned)res->queryCqnRecv,
        (unsigned)res->queryMtuMsgmax, (unsigned long long)res->queryDbRecAddr,
        (unsigned long long)res->dbRecAddrSent, (unsigned)mttH, (unsigned)mttL);
    LOG("r4ingress: the RSS overlay as the firmware holds it, at QPC + 0x%02x: base_qpn "
        "0x%08x (high byte %u), default_qpn 0x%08x, reserved %u, hash_fn %u, flags 0x%02x, "
        "base_qpn_udp 0x%08x, rss_key %s",
        (unsigned)QP_CONTEXT_RSS_BASE_QPN_OFFSET, (unsigned)res->queryBaseQpn,
        (unsigned)res->queryRssHighByte, (unsigned)res->queryDefaultQpn,
        (unsigned)res->queryReserved, (unsigned)res->queryHashFn,
        (unsigned)res->queryRssFlags, (unsigned)res->queryBaseQpnUdp,
        res->queryRssKeyMatches ? "came back byte for byte" : "DOES NOT match what was sent");
    if (!res->queryAllFieldsMatch) {
        LOG("r4ingress: the readback does NOT match what was written (written/read): state "
            "%u/%u, local_qpn %u/%u, sq_size_stride 0x%02x/0x%02x, cqn_send %u/%u, cqn_recv "
            "%u/%u, mtu_msgmax 0x%02x/0x%02x",
            (unsigned)MLX4_QP_STATE_RTS, (unsigned)res->queryState, (unsigned)res->qpn,
            (unsigned)res->queryLocalQpn, (unsigned)res->sqSizeStrideSent,
            (unsigned)res->querySqSizeStride, (unsigned)res->cqn,
            (unsigned)res->queryCqnSend, (unsigned)res->cqn, (unsigned)res->queryCqnRecv,
            (unsigned)res->mtuMsgmaxSent, (unsigned)res->queryMtuMsgmax);
    }
    if (!res->queryRssMatches) {
        LOG("r4ingress: the RSS overlay did not survive: base_qpn 0x%08x sent / 0x%08x read, "
            "default_qpn 0x%08x / 0x%08x, hash_fn %u / %u, flags 0x%02x / 0x%02x, key %s. This "
            "is the layer that decides where hashed traffic goes, so a mismatch here means the "
            "queue this QP steers into is not the one this driver believes",
            (unsigned)res->baseQpnSent, (unsigned)res->queryBaseQpn,
            (unsigned)res->defaultQpnSent, (unsigned)res->queryDefaultQpn,
            (unsigned)MLX4_RSS_HASH_TOP, (unsigned)res->queryHashFn,
            (unsigned)res->rssFlagsSent, (unsigned)res->queryRssFlags,
            res->queryRssKeyMatches ? "matched" : "DID NOT match");
    }
    if (!res->pointsAtAnotherQp) {
        LOG("r4ingress: the indirection QP's hash target is NOT a different QP: default_qpn "
            "0x%08x against this QP's own number %u (matches=%u). A QP that steers into itself "
            "reaches RTS and reads back clean while losing every hashed frame, because the "
            "queue it names has no receive ring",
            (unsigned)res->queryDefaultQpn, (unsigned)res->qpn,
            (unsigned)res->queryDefaultQpnMatches);
    }
}


/* ==================================================================== */
/* R4 stage 6: the transmit side's resources                            */
/* ==================================================================== */

/*
 * The comparison half of stage 6: one QUERY_QP, put next to what was written.
 *
 * Same shape and same reason as the RX side's: one read answers two questions at once, and
 * they must not be merged --
 *
 *   local_qpn        is this the QP we wrote?  mlx4_qp_modify sets it from its own argument
 *                    (mlx4_qp.c:180-181), so a QP somebody else's driver made would say so
 *                    here rather than merely failing to answer.
 *   the queue fields did the firmware KEEP what was sent?  A command that returns fw status 0
 *                    has accepted the mailbox, not promised the context.
 *
 * The expectations come out of `res` -- the values that went into the mailbox -- rather than
 * out of literals here, so the two cannot drift.
 *
 * TWO FIELDS ARE ITEMS OF INTEREST ON THIS SIDE, and both are named in the log rather than
 * only in the mask:
 *
 *   sq_size_stride  the signature value of the stage (0x42 for 256 TXBBs of 64 bytes).  A
 *                   wrong one is silent everywhere else: the three commands still return 0
 *                   and the QP still reaches RTS.
 *   rq_size_stride  expected 0.  The fill does not write it for a send QP
 *                   (mlx4_en_resources.c:52's `!is_tx && !rss`), so what is compared is the
 *                   byte as it stood in the sent image (a zero from the clear) against the
 *                   firmware's copy.  It IS in the mask because the byte really was sent --
 *                   the claim being tested is "no receive geometry is described here", and
 *                   the only way to test it is to read the firmware's copy back.  Unlike
 *                   stage 5's indirection QP, where the field was excluded for the opposite
 *                   reason: there it would have been compared against a value the driver
 *                   chose not to send at all.
 */
static void r4TxQueryAndCompare(Mlx4Hw &hw, Cx3R4TxResult *res)
{
    UInt8 q[QP_CONTEXT_SIZE];
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    UInt32 mask = 0;
    UInt64 dbRec = 0, mttL = 0;
    UInt32 mttH = 0;

    memset(q, 0, sizeof(q));
    res->queryFwStatus = CX3_FWSTATUS_NOT_POSTED;
    if (!hw.qpQuery(res->qpn, q, QP_CONTEXT_SIZE, &status)) {
        res->queryFwStatus = status;
        res->queryOk = 0;
        LOG("r4tx: QUERY_QP on the send QPN %u did not answer (fw status 0x%x), so what the "
            "firmware holds is UNKNOWN -- the command may have been accepted and the query "
            "refused, and those two look the same from here", (unsigned)res->qpn,
            (unsigned)status);
        return;
    }
    res->queryFwStatus = status;
    res->queryOk = 1;
    res->queryFlagsRaw = cx3GetBe32(q + QP_CONTEXT_FLAGS_OFFSET);
    res->queryState    = (res->queryFlagsRaw >> MLX4_QP_STATE_NIBBLE_SHIFT) & 0xfu;
    res->queryLocalQpn = cx3GetBe32(q + QP_CONTEXT_LOCAL_QPN_OFFSET);
    res->queryRqSizeStride = q[QP_CONTEXT_RQ_SIZE_STRIDE_OFFSET];
    res->querySqSizeStride = q[QP_CONTEXT_SQ_SIZE_STRIDE_OFFSET];
    res->queryMtuMsgmax    = q[QP_CONTEXT_MTU_MSGMAX_OFFSET];
    res->queryCqnSend = cx3GetBe32(q + QP_CONTEXT_CQN_SEND_OFFSET);
    res->queryCqnRecv = cx3GetBe32(q + QP_CONTEXT_CQN_RECV_OFFSET);
    dbRec             = cx3GetBe64(q + QP_CONTEXT_DB_REC_ADDR_OFFSET);
    res->queryDbRecAddr = dbRec;
    res->queryLogPageSize = q[QP_CONTEXT_LOG_PAGE_SIZE_OFFSET];
    mttH = q[QP_CONTEXT_MTT_BASE_H_OFFSET];
    mttL = (UInt64)cx3GetBe32(q + QP_CONTEXT_MTT_BASE_L_OFFSET);
    res->queryMttAddrH = mttH;
    res->queryMttAddrL = (UInt32)mttL;
    res->queryParam3     = cx3GetBe32(q + QP_CONTEXT_PARAM3_OFFSET);
    res->querySchedQueue = q[QP_CONTEXT_PRI_SCHED_QUEUE_OFFSET];
    /*
     * The feup readback, and the comparison against the byte the fill wrote.  Nothing is judged
     * on it (it is not one of the eight fields in the mask just below, for the same reason
     * param3 and sched_queue are not), but the two numbers and their equality are reported,
     * because this batch's whole question is whether the firmware keeps this byte.
     */
    res->queryFeup       = q[QP_CONTEXT_PRI_FEUP_OFFSET];
    res->feupSurvived    = (res->queryFeup == res->feupSent) ? 1u : 0u;

    /*
     * The eight fields, each against the value that went in.  The mask is reported as well as
     * the all-or-nothing answer, because "which one" is the first thing a reader needs and a
     * boolean would send them back to the log to find it.
     */
    if (res->queryState == MLX4_QP_STATE_RTS)         mask |= 1u << kR4TxFieldState;
    if (res->queryLocalQpn == res->qpn)               mask |= 1u << kR4TxFieldLocalQpn;
    if (res->queryRqSizeStride == res->rqSizeStrideSent) mask |= 1u << kR4TxFieldRqSize;
    if (res->querySqSizeStride == res->sqSizeStrideSent) mask |= 1u << kR4TxFieldSqSize;
    if (res->queryMtuMsgmax == res->mtuMsgmaxSent)    mask |= 1u << kR4TxFieldMtuMsgmax;
    if (res->queryCqnSend == res->cqn)                mask |= 1u << kR4TxFieldCqnSend;
    if (res->queryCqnRecv == res->cqn)                mask |= 1u << kR4TxFieldCqnRecv;
    if (dbRec == res->dbRecAddrSent)                  mask |= 1u << kR4TxFieldDbRecAddr;
    res->queryFieldMask = mask;
    res->queryAllFieldsMatch =
        (mask == ((1u << kR4TxFieldState) | (1u << kR4TxFieldLocalQpn) |
                  (1u << kR4TxFieldRqSize) | (1u << kR4TxFieldSqSize) |
                  (1u << kR4TxFieldMtuMsgmax) | (1u << kR4TxFieldCqnSend) |
                  (1u << kR4TxFieldCqnRecv) | (1u << kR4TxFieldDbRecAddr))) ? 1u : 0u;
    res->queryMttMatches =
        (mttH == (UInt32)(res->sqMttAddr >> 32) &&
         mttL == (res->sqMttAddr & 0xffffffffull) &&
         res->queryLogPageSize == 0) ? 1u : 0u;

    LOG("r4tx: QUERY_QP QPN %u: fw status 0x%x; the firmware's own view: state %u "
        "(flags 0x%08x), local_qpn %u, rq_size_stride 0x%02x, sq_size_stride 0x%02x, "
        "mtu_msgmax 0x%02x, cqn_send %u, cqn_recv %u, db_rec_addr 0x%llx, mtt_base_addr_h "
        "0x%02x, mtt_base_addr_l 0x%08x, log_page_size %u",
        (unsigned)res->qpn, (unsigned)res->queryFwStatus, (unsigned)res->queryState,
        (unsigned)res->queryFlagsRaw, (unsigned)res->queryLocalQpn,
        (unsigned)res->queryRqSizeStride, (unsigned)res->querySqSizeStride,
        (unsigned)res->queryMtuMsgmax, (unsigned)res->queryCqnSend,
        (unsigned)res->queryCqnRecv, (unsigned long long)dbRec, (unsigned)mttH,
        (unsigned)mttL, (unsigned)res->queryLogPageSize);
    if (!res->queryAllFieldsMatch) {
        LOG("r4tx: the readback does NOT match what was written (written/read): state %u/%u, "
            "local_qpn %u/%u, rq_size_stride 0x%02x/0x%02x, sq_size_stride 0x%02x/0x%02x, "
            "mtu_msgmax 0x%02x/0x%02x, cqn_send %u/%u, cqn_recv %u/%u, db_rec_addr "
            "0x%llx/0x%llx. sq_size_stride is the first field to look at: a send QP's value is "
            "0x42 for 256 TXBBs of 64 bytes, and a wrong one is silent everywhere else",
            (unsigned)MLX4_QP_STATE_RTS, (unsigned)res->queryState,
            (unsigned)res->qpn, (unsigned)res->queryLocalQpn,
            (unsigned)res->rqSizeStrideSent, (unsigned)res->queryRqSizeStride,
            (unsigned)res->sqSizeStrideSent, (unsigned)res->querySqSizeStride,
            (unsigned)res->mtuMsgmaxSent, (unsigned)res->queryMtuMsgmax,
            (unsigned)res->cqn, (unsigned)res->queryCqnSend,
            (unsigned)res->cqn, (unsigned)res->queryCqnRecv,
            (unsigned long long)res->dbRecAddrSent, (unsigned long long)dbRec);
    }
    if (!res->queryMttMatches) {
        LOG("r4tx: the MTT address did not survive the three steps: sent 0x%llx (h 0x%02x / "
            "l 0x%08x, log_page_size 0), read back h 0x%02x / l 0x%08x, log_page_size %u. "
            "Those fields belong to the RST->INIT step alone (mlx4_qp.c:164-169) and are "
            "copied through the other two, so a difference means the shared context was "
            "disturbed between steps",
            (unsigned long long)res->sqMttAddr, (unsigned)(res->sqMttAddr >> 32),
            (unsigned)(res->sqMttAddr & 0xffffffffull), (unsigned)mttH, (unsigned)mttL,
            (unsigned)res->queryLogPageSize);
    }
    /*
     * And the two fields that are REPORTED rather than judged, on every path including the
     * idempotent one.  No reference line reads either back, so there is nothing to compare
     * them against; what can be said is what this driver's fill produces for this QP and what
     * the firmware holds.  For a send QP both are known in advance: param3's FCS_KEEP bit is
     * the receive side's (mlx4_en_rx.c:950-951) so it is 0 here, and sched_queue's low bits
     * and port are the Ethernet form this driver pins to port 1.  Stage 4 measured the
     * firmware clearing sched_queue's bit 0 on the RX QP; the same is expected here.
     */
    LOG("r4tx: param3 written 0x%08x / read back 0x%08x (0 here is the receive-side FCS_KEEP "
        "bit not being set, mlx4_en_rx.c:950-951); sched_queue written 0x%02x / read back "
        "0x%02x -- both are reported and NOT judged, because no reference line reads either "
        "back and failing on an unknown would report the unknown as known",
        (unsigned)res->param3Sent, (unsigned)res->queryParam3,
        (unsigned)res->schedQueueSent, (unsigned)res->querySchedQueue);
    /*
     * The third reported-not-judged field, and the one this batch added to the fill.  The three
     * readings are printed as three facts and the conclusion is left to the reader, which is
     * this project's rule for a value whose verdict cannot be derived (docs/DEFECTS.md F7).
     * The sentence that follows the numbers says what each outcome means for the next run,
     * because that is a statement about the EXPERIMENT rather than a verdict on the value.
     */
    LOG("r4tx: pri_path.feup written 0x%02x / read back 0x%02x at QPC+0x%x (%s). This is the "
        "byte the reference's shared fill sets to MLX4_FEUP_FORCE_ETH_UP (0x40) for a SEND "
        "queue only -- its transmit caller passes a queue index >= 0 and both receive-side "
        "callers pass -1 (mlx4_en_resources.c:62-65, mlx4_en_tx.c:233-234 vs "
        "mlx4_en_rx.c:946/:1057) -- and 0.43.9 is the first build that writes it. What the "
        "three numbers mean: %s",
        (unsigned)res->feupSent, (unsigned)res->queryFeup,
        (unsigned)QP_CONTEXT_PRI_FEUP_OFFSET,
        res->feupSurvived ? "the firmware kept what was written"
                          : "THE FIRMWARE'S COPY DIFFERS FROM WHAT WAS WRITTEN",
        (res->feupSent == (UInt32)MLX4_FEUP_FORCE_ETH_UP)
            ? (res->feupSurvived
                   ? "the byte reached the QP context, so a send that still does nothing is "
                     "NOT explained by the context the firmware holds -- look elsewhere "
                     "(mlx4_regs.h names BlueFlame as the next candidate)"
                   : "the firmware DROPPED the byte, exactly as stage 4 measured it doing for "
                     "param3. If the send path is still silent, this field is not the gate and "
                     "no further run should be spent on it: put the fill back and take "
                     "mlx4_regs.h's BlueFlame note as the live candidate")
            : "this is not a send queue, so 0x40 was correctly not written here");
}

/*
 * Publish the judged claims as scalars, so the verifier decides from values without decoding a
 * structure -- the rule every other stage follows, and the reason `fault` reports its poison
 * bit through `probe`.
 *
 * `haveQuery` says whether layer 2 was measured at all: anything not measured stays
 * CX3_R4TX_NOT_MEASURED rather than becoming a zero a reader would take for an answer.  That
 * matters most for the three equalities here (the QPN comparison, the field mask, the MTT
 * match), all of which a pair of zeros would satisfy.
 *
 * The list is 16 wide because the kernel's scalar array is 16 wide and that is not a knob
 * (CX3_SCALAR_OUT_MAX, and docs/DEFECTS.md F13 for what 0.43.9 spent learning it).  Two
 * consequences of filling it exactly, both argued in the ABI header beside the enum:
 *
 *   - the CQN and the top-reserved line are NOT published here.  They are context for the
 *     number-space message rather than verdicts -- the verdict is `kR4TxOutNumberSafe`, which
 *     does not depend on either number being in the scalar block -- and both stay in
 *     Cx3R4TxResult, where the tool and the kernel log read them.
 *   - the two layer-1 counts share one scalar (`kR4TxOutStepsStates`: steps low, states high),
 *     which is lossless because each is 0..3 and they are only ever read together.  The whole
 *     scalar is the sentinel when the loop never ran.
 */
static void r4TxPublishScalars(const Cx3R4TxResult *res, UInt64 *out, bool haveQuery)
{
    out[kR4TxOutCqFwStatus]      = res->cqFwStatus;
    out[kR4TxOutCqReady]         = res->cqReadyAtStart;
    out[kR4TxOutQpn]             = res->qpn;
    out[kR4TxOutNumberSafe]      = res->numberSafe;
    /* Two counts, one 64-bit value, one shift named in the ABI both sides use. */
    out[kR4TxOutStepsStates]     = (UInt64)res->stepsAccepted |
                                   ((UInt64)res->statesConfirmed << kR4TxOutStatesShift);
    out[kR4TxOutSqSizeStride]    = res->sqSizeStrideSent;
    /*
     * The one scalar for the send queue's memory, and it is the CONJUNCTION of the two things
     * that can be measured about it rather than the header's value alone: a scalar that only
     * said "the 64-byte header is stamped" would pass on a run whose WQE array was left
     * unstamped -- which is the half of docs/DEFECTS.md C12 that stage 9 actually trips over.
     * The two parts stay separate in the structure and the tool prints both, so a failure says
     * which one it was; the scalar is the aggregate, computed here from them.
     *
     * (0.43.9 note, corrected in 0.43.10: this conjunction was first written because the
     * scalar block was full, and that batch then widened the block to 20 believing the width
     * was ours to set -- which made every call to this selector fail before it ran
     * (docs/DEFECTS.md F13).  The width is the kernel's 16; the conjunction stays because
     * these two numbers are only ever wanted together, which is a reason the scalar can
     * honestly carry, unlike "there was no room".)
     */
    out[kR4TxOutSqStampMatches]  = (res->sqStampMatches && res->sqTxbbsStamped == res->sqNent)
                                       ? 1u : 0u;
    if (haveQuery) {
        out[kR4TxOutQueryOk]      = res->queryOk;
        out[kR4TxOutQueryState]   = res->queryOk ? res->queryState : CX3_R4TX_NOT_MEASURED;
        out[kR4TxOutQpnConfirmed] = res->queryOk ? res->qpnConfirmed : CX3_R4TX_NOT_MEASURED;
        out[kR4TxOutFieldsMatch]  = res->queryOk ? res->queryAllFieldsMatch
                                                 : CX3_R4TX_NOT_MEASURED;
        out[kR4TxOutMttMatches]   = res->queryOk ? res->queryMttMatches
                                                 : CX3_R4TX_NOT_MEASURED;
        /*
         * The two feup READBACK numbers keep the not-measured sentinel when the query did not
         * answer, because "the firmware said 0" and "the firmware said nothing" are different
         * answers (docs/DEFECTS.md D10).  The written byte is published outside this branch,
         * two lines below.
         */
        out[kR4TxOutFeupQuery]    = res->queryOk ? res->queryFeup : CX3_R4TX_NOT_MEASURED;
        out[kR4TxOutFeupSurvived] = res->queryOk ? res->feupSurvived : CX3_R4TX_NOT_MEASURED;
    }
    /*
     * ...and the WRITTEN byte goes out on every call that reached the fill, query or no query:
     * it is a fact about the 248 bytes in the mailbox, which is a different claim from any of
     * the firmware's answers.  It is still the sentinel when a refusal stopped the call before
     * the fill ran, so a refusal cannot publish a zero as "what was written".
     */
    out[kR4TxOutFeupSent] = res->feupSent;
}

/* ==================================================================== */
/* The general-purpose command runner, and the one-call survey          */
/* ==================================================================== */

/*
 * kCx3SelHcr -- one command, the caller's parameters, nothing interpreted.
 *
 * Gated on cx3raw=1.  Not because the individual selectors are safer -- several of them can
 * send the same commands -- but because a general runner makes it possible to send a
 * dangerous COMBINATION by accident, and because a published driver should not ship the
 * ability to make its hardware start referring to arbitrary host memory.
 *
 * The gate reports itself in the output rather than only refusing, so "why did nothing
 * happen" has an answer in the same call.
 */
IOReturn Cx3Mlx4::exportHcr(const UInt64 *in, const UInt8 *inbox, UInt8 *outbox,
                            UInt64 *out)
{
    UInt32 fwStatus = CX3_FWSTATUS_NOT_POSTED, hcrFinal = 0;
    UInt64 imm = 0;
    bool completed;

    memset(out, 0, sizeof(UInt64) * kHcrOutCount);
    out[kHcrOutAllowed] = cx3RawAllowed() ? 1 : 0;

    if (!cx3RawAllowed()) {
        LOG("hcr refused: the general command runner needs the boot argument cx3raw=1. "
            "It can send any command, including ones that make the hardware refer to host "
            "memory, so it is a switch rather than something always on. Add cx3raw=1 to "
            "boot-args and reboot; nothing else has to change.");
        out[kHcrOutStatus] = (UInt64)kIOReturnNotPrivileged;
        return kIOReturnSuccess;
    }

    completed = fHw.hcrRun((UInt16)in[kHcrInOpcode], (UInt32)in[kHcrInInModifier],
                           (UInt8)in[kHcrInOpModifier], (UInt32)in[kHcrInFlags],
                           (UInt32)in[kHcrInTimeoutMs],
                           inbox, (UInt32)in[kHcrInInboxLen],
                           outbox, (UInt32)in[kHcrInOutboxLen],
                           &fwStatus, 0, &hcrFinal, &imm);

    out[kHcrOutCompleted] = completed ? 1 : 0;
    out[kHcrOutFwStatus]  = fwStatus;
    out[kHcrOutHcrFinal]  = hcrFinal;
    out[kHcrOutImmLsb]    = (UInt64)(imm & 0xffffffffu);
    out[kHcrOutImmMsb]    = (UInt64)(imm >> 32);
    out[kHcrOutStatus]    = 0;
    return kIOReturnSuccess;
}

/*
 * kCx3SelSurvey -- the whole current state, in one call, without sending anything.
 *
 * Deliberately no commands: this is what to run first when something is wrong, and it has
 * to be cheap, instant and incapable of making anything worse.  It reports what the driver
 * believes it has -- identity, capabilities, which of the fourteen tables are mapped, which
 * queues exist and with what parameters -- so that a single call either shows the
 * inconsistency or rules the obvious ones out.
 *
 * The value is in the comparison rather than in any one field: "the CQ exists but its CQN is
 * inside the firmware's reserved range", "the MR is enabled but the table it lives in was
 * released", "the EQ and the CQ disagree about which EQN completions go to" are all visible
 * here and are all things that would otherwise take a reboot to ask about.
 */
IOReturn Cx3Mlx4::exportSurvey(Cx3SurveyResult *res, UInt64 *out)
{
    UInt32 i, n = 0;

    memset(out, 0, sizeof(UInt64) * kSurveyOutCount);
    memset(res, 0, sizeof(*res));

    /* Numbers only, and no command is sent: this has to be instant, and incapable of making
     * anything worse, because it is the first thing to run when something is wrong. */
#define SURVEY(k_, id_, v0_, v1_, v64_)                                        \
    do {                                                                       \
        if (n < CX3_SURVEY_MAX_ENTRIES) {                                      \
            res->item[n].kind = (k_); res->item[n].id = (id_);                 \
            res->item[n].value0 = (UInt32)(v0_);                               \
            res->item[n].value1 = (UInt32)(v1_);                               \
            res->item[n].value64 = (UInt64)(v64_);                             \
            ++n;                                                               \
        }                                                                      \
    } while (0)

    SURVEY(CX3_SURVEY_KIND_FACT, kSurveyFactPciHeld, fPci ? 1 : 0, 0, 0);
    SURVEY(CX3_SURVEY_KIND_FACT, kSurveyFactBars, fHw.bar0Mapped() ? 1 : 0,
           fHw.bar2Mapped() ? 1 : 0, 0);
    SURVEY(CX3_SURVEY_KIND_FACT, kSurveyFactHcaState, fHcaInited ? 1 : 0,
           fIcmMapped ? 1 : 0, 0);
    SURVEY(CX3_SURVEY_KIND_FACT, kSurveyFactReservedEqUar, fDevCap.reserved_eqs,
           fDevCap.reserved_uars, 0);
    SURVEY(CX3_SURVEY_KIND_FACT, kSurveyFactCqCounts, fDevCap.reserved_cqs,
           1u << fInitHca.log_num_cqs, 0);
    SURVEY(CX3_SURVEY_KIND_FACT, kSurveyFactPdCounts, fDevCap.reserved_pds,
           MLX4_NUM_PDS, 0);
    SURVEY(CX3_SURVEY_KIND_FACT, kSurveyFactMptCounts, fDevCap.reserved_mrws,
           1u << fInitHca.log_mpt_sz, 0);
    /*
     * The command-timeout state.  It is in survey rather than only in the log because the
     * question it answers -- "is the driver refusing to post, and if so which command did
     * that?" -- has to be answerable by the harness as a value, and because a log line can
     * only be read after the fact.  The budget fact is also the check that catches a stale
     * kext: it reads back the numbers the *running* binary uses, which no build-time test
     * can see.
     */
    SURVEY(CX3_SURVEY_KIND_FACT, kSurveyFactCmdWedged,
           fHw.cmdWedged() ? 1u : 0u, fHw.cmdAbandons(), fHw.cmdRefused());
    SURVEY(CX3_SURVEY_KIND_FACT, kSurveyFactCmdWedgedOp, fHw.cmdWedgedOp(),
           fHw.cmdWedged() ? (fHw.cmdWedgedPosted() ? 0u : 1u) : 0u,
           fHw.cmdWedgedAt());
    SURVEY(CX3_SURVEY_KIND_FACT, kSurveyFactCmdBudget,
           (UInt32)CX3_CMD_IDLE_WAIT_MS, (UInt32)CX3_CMD_IDLE_WAIT_EVENT_MS,
           ((UInt64)CX3_CMD_BUDGET_FW_MS << 32) | (UInt64)CX3_CMD_BUDGET_ICM_MS);

    /* The fourteen tables: how many chunks of each are actually mapped, and their size. */
    for (i = 0; i < fNumTables && i < kMaxIcmTables; ++i) {
        UInt32 chunks = 0, c;
        UInt64 bytes = 0;
        for (c = 0; c < (UInt32)(sizeof(fTables[i].icm) / sizeof(fTables[i].icm[0])); ++c) {
            if (!fTables[i].icm[c]) continue;
            ++chunks;
            bytes += fTables[i].icm[c]->bytes;
        }
        SURVEY(CX3_SURVEY_KIND_TABLE, i, chunks, fTables[i].nObj, bytes);
    }

    SURVEY(CX3_SURVEY_KIND_QUEUE, kSurveyQueueEq, fEqRing ? 1 : 0, fEqn, fEqConsIndex);
    SURVEY(CX3_SURVEY_KIND_QUEUE, kSurveyQueueMr, fMrState, fMrPdn, fMrMptIndex);
    SURVEY(CX3_SURVEY_KIND_QUEUE, kSurveyQueueCq, fPortQueues[0].cqUp ? 1 : 0, fPortQueues[0].cqCqn, fPortQueues[0].cqNent);
    SURVEY(CX3_SURVEY_KIND_QUEUE, kSurveyQueueRq, fPortQueues[0].rqUp ? 1 : 0, fPortQueues[0].rqNent, fPortQueues[0].rqBufBytes);

    /*
     * The comparisons that have actually bitten, as lines rather than left to the reader.
     * Each is a state this driver can reach and that no create path refuses: a CQ whose
     * comp_eqn names no live EQ; an MPT entry the firmware owns while the tables it lives in
     * are released; an RQ ring whose WQEs carry an lkey nothing resolves.
     */
    if (fPortQueues[0].cqUp && !fEqRing)
        SURVEY(CX3_SURVEY_KIND_FACT, kSurveyFactInconsistent, kSurveyInconsCqWithoutEq, 0, 0);
    if (fMrState != 0 && !fIcmMapped)
        SURVEY(CX3_SURVEY_KIND_FACT, kSurveyFactInconsistent, kSurveyInconsMrWithoutIcm, 0, 0);
    if (fPortQueues[0].rqUp && fMrState != 1)
        SURVEY(CX3_SURVEY_KIND_FACT, kSurveyFactInconsistent, kSurveyInconsRqWithoutMr, 0, 0);

#undef SURVEY
    res->entries = n;
    out[kSurveyOutEntries] = n;
    out[kSurveyOutStatus]  = 0;
    LOG("survey: %u entr(ies), no command sent (the lkey is reported alongside the MR state "
        "so they can be compared without another call)", (unsigned)n);
    return kIOReturnSuccess;
}

/*
 * kCx3SelFault -- inject the command-timeout states, so that the recovery path can be
 * exercised without having to break the card first.
 *
 * Why this selector exists at all.  The failure this batch fixes happened once, by
 * accident, on 2026-10-12 at 01:27, and it cost a boot: one selector held the transaction
 * lock for 6 min 07.6 s and every other selector -- including the one that could have
 * recovered the machine, reset -- waited behind it (docs/LOCK-DESIGN.md section 1.4).  A
 * fix for that which can only be tested by making it happen again is not testable, and this
 * project's rule is that a defect either gains a guard or its row says why there can be
 * none (docs/DEFECTS.md).  So the two states the fix introduces are reachable on demand:
 *
 *   mode 1  poison the mailbox exactly as an abandonment would, with a marker opcode and
 *           WITHOUT counting an abandonment, so that the numbers stay honest;
 *   mode 2  clear it again -- refused unless the confirmation scalar is 1 and the command
 *           register reads idle, because on a device that is genuinely stuck this is not an
 *           injection and clearing it would post a new command over an old one;
 *   mode 3  hold the transaction lock for a bounded number of milliseconds, which is the
 *           only way to reproduce "another selector is using the mailbox" while asking
 *           whether probe and survey still answer.
 *
 * What it cannot do: send a command, write a register, allocate anything, or make the
 * device do something it was not already doing.  Mode 1 and 3 make the driver *less*
 * capable, never more.
 *
 * The lock is held for the whole call (this selector is class 0 in the user client's
 * classification table), which is what makes mode 3 a test of the lock rather than a test of
 * a sleep.
 */
IOReturn Cx3Mlx4::exportFault(const UInt64 *in, UInt64 *out)
{
    UInt64 mode = in[kFaultInMode];
    UInt64 arg  = in[kFaultInArg];

    memset(out, 0, sizeof(UInt64) * kFaultOutCount);
    out[kFaultOutStatus] = (UInt64)kIOReturnBadArgument;

    switch (mode) {
    case kFaultModeWedge:
        fHw.cmdPoisonInjected();
        break;

    case kFaultModeUnwedge:
        if (arg != 1) {
            LOG("fault unwedge refused: the confirmation scalar must be 1 (got %llu). "
                "Clearing a poison lets commands be posted again, which is only safe when "
                "the poison was an injection.",
                (unsigned long long)arg);
            out[kFaultOutStatus] = (UInt64)kIOReturnNotPermitted;
            goto report;
        }
        if (fHw.cmdRegisterBusy()) {
            /* Busy is one refusal reason, never the proof of poison provenance.
             * The clearing API independently requires a synthetic injection. */
            LOG("fault unwedge refused: the command register still reports an outstanding "
                "command (reads 0x%08x); do not clear or retry. A real abandonment "
                "needs controlled recovery, not an idle-register shortcut.",
                (unsigned)fHw.readHcrStatus());
            out[kFaultOutStatus] = (UInt64)kIOReturnNotReady;
            goto report;
        }
        fHw.cmdClearWedged("fault injection (mode 2, unwedge)");
        if (fHw.cmdWedged()) {
            out[kFaultOutStatus] = (UInt64)kIOReturnNotPermitted;
            goto report;
        }
        break;

    case kFaultModeHold: {
        UInt64 startNs = 0, endNs = 0;
        if (arg == 0 || arg > (UInt64)CX3_FAULT_HOLD_MAX_MS) {
            LOG("fault hold refused: %llu ms is outside 1..%u (a hook that can hold the "
                "transaction lock indefinitely would be the defect this batch fixes)",
                (unsigned long long)arg, (unsigned)CX3_FAULT_HOLD_MAX_MS);
            goto report;
        }
        clock_get_uptime(&startNs);
        LOG("fault hold: holding the transaction lock for %llu ms on purpose; every other "
            "selector that takes the lock is blocked until this returns, which is the point "
            "(probe, peek, survey and m3irqstatus ask instead of waiting and will say busy)",
            (unsigned long long)arg);
        IOSleep((UInt32)arg);
        clock_get_uptime(&endNs);
        out[kFaultOutHeldMs] = (endNs > startNs) ? (endNs - startNs) / 1000000ull : 0ull;
        LOG("fault hold: released after %llu ms of wall clock (asked for %llu)",
            (unsigned long long)out[kFaultOutHeldMs], (unsigned long long)arg);
        break;
    }

    default:
        LOG("fault: unknown mode %llu (1 = wedge, 2 = unwedge with arg 1, 3 = hold for arg "
            "ms)", (unsigned long long)mode);
        goto report;
    }

    out[kFaultOutStatus] = 0;

report:
    out[kFaultOutWedged]   = fHw.cmdWedged() ? 1 : 0;
    out[kFaultOutWedgedOp] = fHw.cmdWedgedOp();
    out[kFaultOutAbandons] = fHw.cmdAbandons();
    out[kFaultOutRefused]  = fHw.cmdRefused();
    LOG("fault: mode %llu -> status 0x%llx, wedged %llu (op 0x%x), abandons %llu, "
        "refused %llu, held %llu ms",
        (unsigned long long)mode, (unsigned long long)out[kFaultOutStatus],
        (unsigned long long)out[kFaultOutWedged], (unsigned)fHw.cmdWedgedOp(),
        (unsigned long long)out[kFaultOutAbandons],
        (unsigned long long)out[kFaultOutRefused],
        (unsigned long long)out[kFaultOutHeldMs]);
    return kIOReturnSuccess;
}

/* ==================================================================== */
/* R4 stage 1: a protection domain and the physical memory region        */
/* ==================================================================== */

/*
 * kCx3SelR4Mr -- the first thing R4 needs, and the first place where three numbering
 * schemes meet.
 *
 * A protection domain is a software number on a native PF (mlx4_pd.c:47-56: a bitmap,
 * no command), an MPT index is a hardware index into the dMPT table, and the lkey that
 * goes into a descriptor is that index run through hw_index_to_key.  This selector
 * reports all three, plus the MPT entry read back out of the mapped table, because the
 * failure mode of getting any of them wrong is silence: no command fails, the QP or the
 * descriptor simply never resolves.
 *
 * It is also the first selector that leaves durable state behind (a PD and an MPT entry
 * the hardware owns). Running it twice allocates a second pair rather than reusing the
 * first -- which is stated here because a diagnostic that quietly reuses state is how
 * "attempt 2 fails differently from attempt 1" starts.
 */
IOReturn Cx3Mlx4::exportR4Mr(const UInt64 *in, Cx3R4MrResult *res, UInt64 *out)
{
    UInt64 iova = in[kR4MrInIova];
    UInt64 size = in[kR4MrInSize];
    UInt32 fwStatus = CX3_FWSTATUS_NOT_POSTED;
    UInt8  entry[MPT_ENTRY_SIZE];
    UInt32 pdn = 0, mptIndex = 0, mptTableSize = 0;
    UInt32 access = MLX4_PERM_LOCAL_READ | MLX4_PERM_LOCAL_WRITE;

    memset(out, 0, sizeof(UInt64) * kR4MrOutCount);
    memset(res, 0, sizeof(*res));
    memset(entry, 0, sizeof(entry));
    res->status   = (UInt32)kIOReturnError;
    res->fwStatus = CX3_FWSTATUS_NOT_POSTED;

    /*
     * 0a. Idempotence, because the alternative is a real conflict.
     *
     * This used to allocate a fresh PD and re-send SW2HW_MPT for the same MPT slot every
     * time it was run.  Both reviews of this stage flagged it: the second run would have
     * handed the hardware a new PD for a slot it already owned, and what the firmware
     * does with that is unknown -- it is not safe to assume it overwrites cleanly.  So a
     * second run reports the region that already exists instead of making another one.
     */
    if (fMrState != 0) {
        res->pdn        = fMrPdn;
        res->mptIndex   = fMrMptIndex;
        res->keySent    = fMrMptIndex;
        res->lkey       = fMrLkey;
        res->iovaSent   = 0;
        res->sizeSent   = ~0ull;
        res->pdReserved = fDevCap.reserved_pds;
        res->mptCount   = 1u << fInitHca.log_mpt_sz;
        res->mptMaxCap  = fDevCap.max_mpts;   /* the idempotent path reported 0 here at
                                               * first: the capability ceiling is not
                                               * part of the resource, so it has to be
                                               * filled in on every path */
        res->fwStatus   = (fMrState == 1) ? 0 : CX3_FWSTATUS_TIMEOUT;
        res->status     = 0;
        out[kR4MrOutStatus] = 0;
        LOG("r4mr: a region already exists (PD %u, MPT %u, lkey 0x%08x, state %u); "
            "reporting it rather than allocating a second one",
            (unsigned)fMrPdn, (unsigned)fMrMptIndex, (unsigned)fMrLkey,
            (unsigned)fMrState);
        return kIOReturnSuccess;
    }

    /* 0b. the inputs have to be known before anything is written */
    if (!fHcaInited) {
        LOG("r4mr refused: the HCA is not initialised (run m2dprofile/m2dbringup first)");
        res->status = (UInt32)kIOReturnNotReady;
        out[kR4MrOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if (size == 0) size = ~0ull;                     /* mlx4_en_main.c:212 */

    /* 1. the protection domain */
    if (!fHw.pdAlloc(&pdn)) {
        res->status = (UInt32)kIOReturnNoSpace;
        out[kR4MrOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    res->pdn         = pdn;
    res->pdReserved  = fDevCap.reserved_pds;

    /* 2. the MPT index.  The firmware's own entries occupy 0..reserved_mrws-1
     *    (the profile passes reserved_mrws as the dMPT table's reserved prefix), so
     *    the driver starts above them -- and this is the same first-free choice the
     *    reference makes with mlx4_mpt_reserve(). */
    mptIndex = fDevCap.reserved_mrws;
    /*
     * The bound is the table this port actually configured, not the capability ceiling.
     *
     * max_mpts is what the card could have (QUERY_DEV_CAP); 1 << log_mpt_sz is what
     * INIT_HCA was told to make (the profile's dMPT object count).  Both reviews caught
     * that reporting the first as "entries in the table" would be a capability presented
     * as a configuration -- the same class of mistake as reading reserved_cqs as the
     * configured CQ count.
     */
    mptTableSize = 1u << fInitHca.log_mpt_sz;
    if (mptIndex >= mptTableSize) {
        LOG("r4mr: no MPT left: the firmware reserves %u of the %u configured "
            "(the card's own ceiling is %u, which is not what was configured)",
            (unsigned)fDevCap.reserved_mrws, (unsigned)mptTableSize,
            (unsigned)fDevCap.max_mpts);
        fHw.pdFree(pdn);
        res->status = (UInt32)kIOReturnNoSpace;
        out[kR4MrOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    res->mptIndex  = mptIndex;
    res->mptCount  = mptTableSize;
    res->mptMaxCap = fDevCap.max_mpts;
    res->keySent  = mptIndex;                        /* key_to_hw_index(mr.key) == index */
    res->lkey     = mlx4HwIndexToKey(mptIndex);      /* what a WQE carries */
    res->iovaSent = iova;
    res->sizeSent = size;

    /* 3. build and hand over the entry */
    fMrGeneration.init(fDeviceGeneration);
    if (!fHw.mrEnablePhysical(mptIndex, pdn, iova, size, access,
                              &fMrGeneration, &fwStatus)) {
        res->fwStatus = fwStatus;
        out[kR4MrOutStatus] = (UInt32)kIOReturnError;
        /*
         * A timeout is not a rejection, so it is recorded as its own state rather than
         * rolled back: the firmware may have taken the entry.  A definite failure is
         * rolled back, PD included -- the number was handed out and never used, and the
         * allocator only reclaims the most recent one, which this is.
         */
        if (fwStatus == CX3_FWSTATUS_TIMEOUT) {
            fMrState = 2;
            fMrPdn = pdn;
            fMrMptIndex = mptIndex;
            fMrLkey = mlx4HwIndexToKey(mptIndex);
            LOG("r4mr: SW2HW_MPT TIMED OUT, so the entry's ownership is UNKNOWN -- not "
                "rejected. The PD and the slot are kept and the state is recorded; "
                "'mrTeardown' will try to take it back, and nothing releases the table "
                "underneath it.");
        } else {
            fHw.pdFree(pdn);
        }
        return kIOReturnSuccess;
    }
    res->fwStatus = fwStatus;
    fMrState    = 1;
    fMrPdn      = pdn;
    fMrMptIndex = mptIndex;
    fMrLkey     = mlx4HwIndexToKey(mptIndex);

    /*
     * 4. Read the entry back out of the mapped dMPT table.
     *
     * "SW2HW_MPT returned 0" is a claim about a command; this is a claim about the
     * entry. The difference matters because the whole point of stage 1 is that several
     * wrong values here produce no error at all -- so the entry is what gets checked.
     * SW_OWNS (0xf) is what was written; the hardware replaces it with its own state.
     */
    if (fHw.mptEntryRead(mptIndex, entry, sizeof(entry))) {
        res->readbackOk     = 1;
        res->flagsReadback  = ((UInt32)entry[MPT_ENTRY_FLAGS_OFFSET] << 24) |
                              ((UInt32)entry[MPT_ENTRY_FLAGS_OFFSET + 1] << 16) |
                              ((UInt32)entry[MPT_ENTRY_FLAGS_OFFSET + 2] << 8) |
                               (UInt32)entry[MPT_ENTRY_FLAGS_OFFSET + 3];
        res->keyReadback    = ((UInt32)entry[MPT_ENTRY_KEY_OFFSET] << 24) |
                              ((UInt32)entry[MPT_ENTRY_KEY_OFFSET + 1] << 16) |
                              ((UInt32)entry[MPT_ENTRY_KEY_OFFSET + 2] << 8) |
                               (UInt32)entry[MPT_ENTRY_KEY_OFFSET + 3];
        res->pdFlagsReadback = ((UInt32)entry[MPT_ENTRY_PD_FLAGS_OFFSET] << 24) |
                               ((UInt32)entry[MPT_ENTRY_PD_FLAGS_OFFSET + 1] << 16) |
                               ((UInt32)entry[MPT_ENTRY_PD_FLAGS_OFFSET + 2] << 8) |
                                (UInt32)entry[MPT_ENTRY_PD_FLAGS_OFFSET + 3];
        res->entitySizeReadback =
            ((UInt32)entry[MPT_ENTRY_ENTITY_SIZE_OFFSET] << 24) |
            ((UInt32)entry[MPT_ENTRY_ENTITY_SIZE_OFFSET + 1] << 16) |
            ((UInt32)entry[MPT_ENTRY_ENTITY_SIZE_OFFSET + 2] << 8) |
             (UInt32)entry[MPT_ENTRY_ENTITY_SIZE_OFFSET + 3];
        {
            UInt32 i;
            UInt64 mtt = 0;
            for (i = 0; i < 8; ++i)
                mtt = (mtt << 8) | (UInt64)entry[MPT_ENTRY_MTT_ADDR_OFFSET + i];
            res->mttAddrReadback = mtt;
        }
        res->statusNibble = (res->flagsReadback & MLX4_MPT_STATUS_NIBBLE_MASK) >> 28;
        LOG("r4mr: MPT %u readback: flags 0x%08x (status nibble 0x%x -- 0 means the "
            "hardware owns it, 0xf would mean it did not take), key 0x%08x, pd_flags "
            "0x%08x, entity_size %u, mtt_addr 0x%llx",
            (unsigned)mptIndex, (unsigned)res->flagsReadback, (unsigned)res->statusNibble,
            (unsigned)res->keyReadback, (unsigned)res->pdFlagsReadback,
            (unsigned)res->entitySizeReadback,
            (unsigned long long)res->mttAddrReadback);
    } else {
        LOG("r4mr: the MPT entry could not be read back; the command status is all there "
            "is to go on");
    }

    /*
     * 5. And now ask the firmware itself.
     *
     * The readback above is a claim about the host-memory copy of the table; this is a
     * claim about what the firmware stored. If they disagree, the readback was the
     * misleading one -- which is exactly the kind of thing stage 1 exists to find out
     * before a QP is built on top of it.
     */
    {
        UInt8 qentry[MPT_ENTRY_SIZE];
        UInt32 qst = CX3_FWSTATUS_NOT_POSTED;
        memset(qentry, 0, sizeof(qentry));
        if (fHw.queryMpt(mptIndex, qentry, sizeof(qentry), &qst)) {
            res->queryMptOk        = 1;
            res->queryMptStatus    = qst;
            res->queryFlags        = ((UInt32)qentry[MPT_ENTRY_FLAGS_OFFSET] << 24) |
                                     ((UInt32)qentry[MPT_ENTRY_FLAGS_OFFSET + 1] << 16) |
                                     ((UInt32)qentry[MPT_ENTRY_FLAGS_OFFSET + 2] << 8) |
                                      (UInt32)qentry[MPT_ENTRY_FLAGS_OFFSET + 3];
            res->queryKey          = ((UInt32)qentry[MPT_ENTRY_KEY_OFFSET] << 24) |
                                     ((UInt32)qentry[MPT_ENTRY_KEY_OFFSET + 1] << 16) |
                                     ((UInt32)qentry[MPT_ENTRY_KEY_OFFSET + 2] << 8) |
                                      (UInt32)qentry[MPT_ENTRY_KEY_OFFSET + 3];
            res->queryPdFlags      = ((UInt32)qentry[MPT_ENTRY_PD_FLAGS_OFFSET] << 24) |
                                     ((UInt32)qentry[MPT_ENTRY_PD_FLAGS_OFFSET + 1] << 16) |
                                     ((UInt32)qentry[MPT_ENTRY_PD_FLAGS_OFFSET + 2] << 8) |
                                      (UInt32)qentry[MPT_ENTRY_PD_FLAGS_OFFSET + 3];
            res->queryEntitySize   =
                ((UInt32)qentry[MPT_ENTRY_ENTITY_SIZE_OFFSET] << 24) |
                ((UInt32)qentry[MPT_ENTRY_ENTITY_SIZE_OFFSET + 1] << 16) |
                ((UInt32)qentry[MPT_ENTRY_ENTITY_SIZE_OFFSET + 2] << 8) |
                 (UInt32)qentry[MPT_ENTRY_ENTITY_SIZE_OFFSET + 3];
            {
                UInt32 i;
                UInt64 mtt = 0;
                for (i = 0; i < 8; ++i)
                    mtt = (mtt << 8) | (UInt64)qentry[MPT_ENTRY_MTT_ADDR_OFFSET + i];
                res->queryMttAddr = mtt;
            }
            res->queryStatusNibble = (res->queryFlags & MLX4_MPT_STATUS_NIBBLE_MASK) >> 28;
            LOG("r4mr: QUERY_MPT %u says: flags 0x%08x (status 0x%x), key 0x%08x, "
                "pd_flags 0x%08x, entity_size %u, mtt_addr 0x%llx (fw status 0x%x)",
                (unsigned)mptIndex, (unsigned)res->queryFlags,
                (unsigned)res->queryStatusNibble, (unsigned)res->queryKey,
                (unsigned)res->queryPdFlags, (unsigned)res->queryEntitySize,
                (unsigned long long)res->queryMttAddr, (unsigned)qst);
        } else {
            res->queryMptStatus = qst ? qst : 0xffffffffu;
            LOG("r4mr: QUERY_MPT %u did not answer (fw status 0x%x); the host-memory "
                "readback above is then the only view, with the caveat that nothing "
                "proves the firmware writes that copy", (unsigned)mptIndex, (unsigned)qst);
        }
    }

    res->status = 0;
    out[kR4MrOutStatus] = 0;
    LOG("r4mr OK: PD %u, MPT %u, lkey 0x%08x (this is what a WQE must carry), physical "
        "region iova 0x%llx size 0x%llx, fw status 0x%x",
        (unsigned)pdn, (unsigned)mptIndex, (unsigned)res->lkey,
        (unsigned long long)iova, (unsigned long long)size, (unsigned)fwStatus);
    return kIOReturnSuccess;
}


/* ==================================================================== */
/* R3: the port                                                         */
/* ==================================================================== */

/*
 * kCx3SelM3QueryPort -- QUERY_PORT, read-only, one port.
 *
 * Kept separate from the selector that writes port configuration on purpose: the
 * capability answer decides whether the write path may run at all, and a reading
 * step that cannot write is a reading step whose result is worth trusting.
 */
IOReturn Cx3Mlx4::exportM3QueryPort(const UInt64 *in, Cx3QueryPortResult *res,
                                    UInt64 *out)
{
    if (!in || !res || !out) return kIOReturnBadArgument;
    if (in[kQueryPortInPort] < 1 || in[kQueryPortInPort] > 2) return kIOReturnBadArgument;
    UInt32 port = (UInt32)in[kQueryPortInPort];

    memset(out, 0, sizeof(UInt64) * kQueryPortOutCount);
    memset(res, 0, sizeof(*res));
    res->port = port;
    res->status = (UInt32)kIOReturnError;
    res->fwStatus = CX3_FWSTATUS_NOT_POSTED;

    if (!fHw.queryPort(port, res)) {
        recordPortQuery(port, *res);
        if (res->status == 0) res->status = (UInt32)kIOReturnError;
        out[kQueryPortOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    recordPortQuery(port, *res);
    /*
     * Publish the address this read produced, because it is the same value the interface step
     * needs -- and taking it from here rather than re-querying is what makes the two one fact
     * instead of two that agree by luck.  Held for the instance, like the link state.
     */
    {
        UInt32 mi = (port >= 1u && port <= 2u) ? port - 1u : 0u;
        LOG("m3queryport: port %u's address is held for its interface step (%02x:%02x:%02x:%02x:%02x:%02x, "
            "bytes 2..7 of QUERY_PORT's def_mac); the other port's entry is untouched",
            (unsigned)port, fPorts[mi].mac[0], fPorts[mi].mac[1], fPorts[mi].mac[2],
            fPorts[mi].mac[3], fPorts[mi].mac[4], fPorts[mi].mac[5]);
    }
    out[kQueryPortOutStatus] = 0;
    return kIOReturnSuccess;
}

/*
 * The selector transaction lock.
 *
 * Allocated in init(), before publication. No entry path allocates or silently
 * proceeds unlocked. start() remains free of device writes and DMA setup.
 *
 * Held for the duration of one externalMethod() call, which is one selector: inbox filled,
 * command submitted, outbox copied.  That is the unit both reviews asked to be atomic.
 */
bool Cx3Mlx4::transactionEnter()
{
    if (!fXactLock) return false;
    IOLockLock(fXactLock);
    return true;
}

void Cx3Mlx4::transactionExit()
{
    if (fXactLock) IOLockUnlock(fXactLock);
}

/*
 * The transaction lock, asked for instead of waited for.
 *
 * The defect this closes is the other half of the one the lock was added for.  The lock is
 * right: it is the only thing that keeps two selectors from interleaving around the shared
 * mailbox *and* around fTables[]/fNumTables, which freeIcmTables() and exportM2dProfile()
 * both modify.  What was wrong was that a selector could hold it for 6 min 07.6 s, during
 * which the driver was unreachable -- including "reset", the one call that could have
 * recovered the machine (docs/LOCK-DESIGN.md sections 1.4 and 3.1).
 *
 * So the selectors whose job is to answer "what state is this device in" ask for the lock
 * and report kIOReturnBusy if someone else has it, rather than queueing behind a command
 * that may be waiting for a bounded-but-long budget.  Their data is read in one pass with
 * the lock held, so the answer is still a consistent snapshot; the only difference is that
 * "I could not look" is now a possible answer, and it is a much better one than a wrong
 * answer (this project's C9) or a five-minute silence.
 *
 * Deliberately not used by the selectors that touch the mailbox or the table list: for
 * those, waiting is the correct behaviour, because the alternative is the interleaving that
 * C9 records.
 */
bool Cx3Mlx4::transactionTryEnter()
{
    if (!fXactLock) return false;
    return IOLockTryLock(fXactLock) ? true : false;
}

/*
 * Selector admission vs stop().
 *
 * Plain gate fields are protected only by fAdmissionLock. No firmware command,
 * sleep or acquisition of fXactLock occurs while that short lock is held.
 * A caller may take admission while holding transaction, never the reverse.
 * Retention is checked under transaction at dispatch, not read under a different
 * lock here. Already-admitted transaction waiters must recheck stopping too.
 */
bool Cx3Mlx4::selectorEnter()
{
    bool ok;

    if (!fAdmissionLock || !fXactLock) return false;
    IOLockLock(fAdmissionLock);
    ok = fGate.enter();
    IOLockUnlock(fAdmissionLock);
    return ok;
}

void Cx3Mlx4::selectorExit()
{
    if (!fAdmissionLock) return;
    IOLockLock(fAdmissionLock);
    fGate.exit();
    IOLockUnlock(fAdmissionLock);
}

UInt32 Cx3Mlx4::selectorActive()
{
    UInt32 n;

    if (!fAdmissionLock) return 0;
    IOLockLock(fAdmissionLock);
    n = fGate.active;
    IOLockUnlock(fAdmissionLock);
    return n;
}

bool Cx3Mlx4::selectorStopping()
{
    if (!fAdmissionLock) return true;
    IOLockLock(fAdmissionLock);
    bool stopping = fGate.stopping;
    IOLockUnlock(fAdmissionLock);
    return stopping;
}

/*
 * Close the gate and wait, bounded, for the selectors already inside to leave.
 *
 * True means drained. False forbids dependent teardown. The elapsed monotonic
 * budget includes admission lock acquisition and scheduler oversleep; it is not
 * a hard real-time promise, nor a bound for all other phases of stop().
 */
bool Cx3Mlx4::beginStopAndDrain(UInt32 waitMs)
{
    UInt64 start = 0;
    clock_get_uptime(&start);
    if (!fAdmissionLock || !fXactLock) return false;
    IOLockLock(fAdmissionLock);
    fGate.beginStop();
    UInt32 entered = fGate.entered, refused = fGate.refused;
    IOLockUnlock(fAdmissionLock);
    LOG("stop: selector gate closed (%u admitted so far, %u refused); waiting for %u active "
        "selector(s) to leave, up to %u ms",
        (unsigned)entered, (unsigned)refused,
        (unsigned)selectorActive(), (unsigned)waitMs);

    for (;;) {
        if (selectorActive() == 0) {
            LOG("stop: selector gate drained");
            return true;
        }
        UInt64 now = 0, elapsedNs = 0;
        clock_get_uptime(&now);
        absolutetime_to_nanoseconds(now - start, &elapsedNs);
        if (elapsedNs >= (UInt64)waitMs * 1000000ULL) break;
        IOSleep(1);
    }
    LOG("stop: %u selector(s) still active at the %u ms drain budget -- dependent "
        "teardown is forbidden; incomplete-stop object survival is not proven.",
        (unsigned)selectorActive(), (unsigned)waitMs);
    return false;
}

/*
 * The first UAR index software may use.  See the declaration for the two source lines this
 * reproduces; the arithmetic is guarded so that a card whose UAR page is LARGER than the
 * system page cannot make the shift negative, which in C would be undefined behaviour --
 * the same shape as the UInt32 >> 32 that cost this project hours in section 23.2.
 */
UInt32 Cx3Mlx4::reservedUarStart() const
{
    UInt32 uarPageShift = (UInt32)fInitHca.uar_page_sz + 12;   /* mlx4_main.c:856 */
    UInt32 diff = (uarPageShift > 12u) ? (uarPageShift - 12u) : 0u;
    UInt32 fromRule = MLX4_NUM_RESERVED_UARS >> diff;          /* 128 >> diff */
    UInt32 fromCap  = (fDevCap.reserved_uars >> diff);
    UInt32 start    = (fromRule > fromCap) ? fromRule : fromCap;

    LOG("UAR start index %u = max(%u from the rule, %u from devcap) at uar_page_sz %u "
        "(shift %u); the first %u UARs belong to the EQ doorbells",
        (unsigned)start, (unsigned)fromRule, (unsigned)fromCap,
        (unsigned)fInitHca.uar_page_sz, (unsigned)uarPageShift,
        (unsigned)fromRule);
    return start;
}

/*
 * kCx3SelM3QueryHca -- what steering mode is the HCA actually in?
 *
 * Read-only, and the entrance condition for R4.  The capability bits say the card
 * supports B0 (UC_STEER and MC_STEER are both set in flags), but what the device is
 * actually doing depends on what this port's INIT_HCA wrote -- and it never wrote
 * INIT_HCA_UC_STEERING_OFFSET.  In A0, mlx4_SET_PORT_qpn_calc() sends a command and a
 * unicast MAC's QPN is base_qpn + mac_index; in B0 neither is true.  Receive steering
 * built on the wrong assumption would not fail loudly, it would just not match.
 */
IOReturn Cx3Mlx4::exportM3QueryHca(Cx3QueryHcaResult *res, UInt64 *out)
{
    memset(out, 0, sizeof(UInt64) * kQueryHcaOutCount);
    memset(res, 0, sizeof(*res));
    res->status   = (UInt32)kIOReturnError;
    res->fwStatus = CX3_FWSTATUS_NOT_POSTED;

    if (!fHw.queryHca(res)) {
        if (res->status == 0) res->status = (UInt32)kIOReturnError;
        out[kQueryHcaOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    out[kQueryHcaOutStatus] = 0;
    return kIOReturnSuccess;
}


/*
 * kCx3SelM3PortUp -- bring one port up in the reference's order.
 *
 *   QUERY_PORT (gate) -> SET_PORT general (Ethernet) -> SET_PORT qpn_calc -> INIT_PORT
 *                     -> QUERY_PORT
 *
 * The middle command is stage 7's, and its position is not a preference: under A0 the MAC
 * table's index becomes a QPN (`*qpn = base_qpn + index`, mlx4_en_netdev.c:573-576) and the
 * base is PUSHED by the driver with that command (mlx4_port.c:1595), so it has to be in the
 * firmware before INIT_PORT makes the port receive anything.  That is the reference's own
 * order (mlx4_en_netdev.c:1388, :1394, :1403).
 *
 * Five things about this sequence are deliberate, and the first four came out of the two
 * external reviews of it:
 *
 *   - No CLOSE_PORT.  The reference's Ethernet start path is general then INIT_PORT
 *     (mlx4_en_netdev.c:2312-2325); CLOSE_PORT belongs to the stop and type-change
 *     paths.  And the link bit must NOT be used to decide whether a close is needed:
 *     it reports link state and says nothing about whether the port was initialised.
 *
 *   - The gate runs first and stops everything.  If the port does not advertise
 *     Ethernet, nothing is written.  Both reviews asked for exactly this, and neither
 *     could rule out that a wrong SET_PORT is a valid *different* operation rather
 *     than an error the firmware would reject.
 *
 *   - MTU.  The reference sends interface MTU + ETH_HLEN + VLAN_HLEN + ETH_FCS_LEN
 *     (mlx4_en_netdev.c:2312-2316).  With the usual 1500/14/4/4 that is 1522, and an
 *     earlier draft of this port had 1518 because it left the FCS out.  The value
 *     actually sent is reported, so this can be checked rather than believed.
 *
 *   - INIT_PORT succeeding is NOT the acceptance criterion.  It says the HCA accepted
 *     the command.  "Link up" is the link bit of a fresh QUERY_PORT -- byte 0x00 bit 7
 *     -- which is how the reference's own Ethernet path computes link_state
 *     (mlx4_en_port.c:91-93).
 */
IOReturn Cx3Mlx4::exportM3PortUp(const UInt64 *in, UInt64 *out)
{
    if (!in || !out) return kIOReturnBadArgument;
    UInt32 port   = (UInt32)in[kPortUpInPort];
    if (port>=1 && port<=2 && fNativePorts[port-1]) {
        memset(out,0,sizeof(UInt64)*kPortUpOutCount); out[0]=kIOReturnNotReady; return kIOReturnSuccess;
    }
    UInt32 mtuIn  = (UInt32)in[kPortUpInMtu];
    UInt32 qpStatus = CX3_FWSTATUS_NOT_POSTED;
    UInt32 setStatus = CX3_FWSTATUS_NOT_POSTED;
    UInt32 qpnCalcStatus = CX3_FWSTATUS_NOT_POSTED;
    UInt32 initStatus = CX3_FWSTATUS_NOT_POSTED;
    Cx3QueryPortResult qp;
    UInt16 mtu;
    bool roce;

    (void)qpStatus;
    memset(out, 0, sizeof(UInt64) * kPortUpOutCount);
    memset(&qp, 0, sizeof(qp));
    out[kPortUpOutQpFwStatus]   = CX3_FWSTATUS_NOT_POSTED;
    out[kPortUpOutSetFwStatus]  = CX3_FWSTATUS_NOT_POSTED;
    out[kPortUpOutQpnCalcFwStatus] = CX3_FWSTATUS_NOT_POSTED;
    out[kPortUpOutInitFwStatus] = CX3_FWSTATUS_NOT_POSTED;
    out[kPortUpOutLinkBefore]   = ~0ull;
    out[kPortUpOutLinkAfter]    = ~0ull;
    out[kPortUpOutSpeedAfter]   = ~0ull;
    out[kPortUpOutStatus]       = (UInt64)kIOReturnError;

    if (!in || in[kPortUpInPort] < 1 || in[kPortUpInPort] > 2 ||
        in[kPortUpInMtu] > 65535 || (in[kPortUpInMtu] && in[kPortUpInMtu] < 64)) {
        out[kPortUpOutStatus] = (UInt64)kIOReturnBadArgument;
        return kIOReturnSuccess;
    }
    Cx3PortState &portState = fPorts[port - 1];

    /*
     * There was a guard here: "refuse while an EQ exists or an interrupt session is
     * open".  It is gone, and the reason is that it conflated two conditions that are
     * not the same -- which is what pinned this port's chain into the one order the
     * hardware fails in (docs/EQ-ORDERING.md section 4.2, section 5).
     *
     * What it was protecting is the way back, and the way back is blocked by exactly
     * one thing: a reset is refused while an interrupt session is open
     * (exportResetHca, "HCA reset refused: an interrupt session is still open", the
     * fIrqObjectsUp || fIrqSource || fWorkLoop test).  A reset is NOT refused for a
     * ring -- exportResetHca tears the ring down itself through teardownEq(), so an
     * existing EQ costs the way back nothing.  The reference activates its port in that
     * state: mlx4_init_eq_table() creates every EQ, attaches the interrupt
     * (mlx4_eq.c:1250), maps the events (:1270) and arms the async EQ (:1277), all
     * inside mlx4_load_one() (mlx4_main.c:2669) -- and the port is activated later, by
     * SET_PORT/INIT_PORT (mlx4_main.c:2756, mlx4_en_netdev.c:1394-1403).  Refusing on
     * the ring is therefore what makes the reference's order inexpressible, and that
     * order is the whole point: with the port brought up FIRST, INIT_PORT's asynchronous
     * event lands in a queue that has an arm but no delivery, fires it, and from FIRED
     * the HCA asserts nothing more -- measured twice on 0.43.0, same build, changing
     * only whether the port was up when the EQ was created (QUERY_EQ 0x00000a00 FIRED
     * and invocations=0, versus 0x00000900 ARMED and the completion arriving).
     *
     * The premise "a busy device means no way back" does not hold either:
     *
     *   - every command now waits against a wall-clock budget and poisons the mailbox
     *     when it expires (docs/LOCK-IMPL.md), so a stuck command costs a bounded
     *     amount of time and the driver stays answerable instead of holding a selector
     *     for minutes;
     *   - m3irqdown works off object existence rather than a session flag, so it closes
     *     the session even after the storm guard tripped, and reset is then one command
     *     away;
     *   - FLR (src/flr.inc) is available with a session open.  Verified by reading that
     *     path end to end rather than assuming it: exportFlr() checks the PCIe
     *     capability, bit 28 of Device Capabilities, and BAR0 stability, and consults
     *     fIrqObjectsUp / fIrqSource / fWorkLoop nowhere.  It is a configuration-space
     *     write, so no interrupt object is in its way.
     *
     * ...and the new order cannot deadlock against the interrupt action, which is the one
     * thing worth checking before letting a port activation run with delivery on.  The
     * transaction lock is taken once, at the user-space boundary, around the whole
     * externalMethod dispatch (src/Cx3Mlx4UserClient.cpp:298), so this selector holds it;
     * the action (irqAction) takes only fEqLock and never the transaction lock, sends no
     * commands and touches no register other than the EQ doorbell.  The order is therefore
     * fXactLock -> fEqLock with no path going the other way (grep: fXactLock appears only
     * in transactionEnter/transactionExit/transactionTryEnter, all reached from the
     * user-client dispatch), so there is no cycle to wait on.
     *
     * What is NOT claimed: that FLR under a live session is a well-behaved reset.  It
     * resets the function while those objects exist, and what they do next has not been
     * measured -- the ring is stale, and if BAR0 moves the window is not trusted until a
     * reboot.  So FLR is the reachable way back, not the preferred one.  The preference
     * is still 'm3irqdown' then reset, and the log line below says so, which is what
     * replaces the refusal: the state is recorded and the caller is told which way back
     * is open, instead of the call being rejected on a condition this path does not
     * depend on.
     */
    if (fEqRing || fIrqObjectsUp) {
        LOG("m3portup: proceeding with %s%s%s open. Nothing on this path needs a quiet "
            "device -- the reference activates its port with the EQ created, mapped and "
            "armed and delivery running. The way back if this goes wrong: 'm3irqdown' "
            "then reset (a reset is refused while an interrupt session is open), or FLR "
            "(src/flr.inc), which does not consult the session but resets the function "
            "under these objects -- that combination is unmeasured, and it needs a reboot "
            "if BAR0 moves",
            fEqRing ? "an EQ" : "", (fEqRing && fIrqObjectsUp) ? " and " : "",
            fIrqObjectsUp ? "an interrupt session" : "");
    }

    /* 1. the gate */
    if (!fHw.queryPort(port, &qp)) {
        recordPortQuery(port, qp);
        out[kPortUpOutQpFwStatus] = qp.fwStatus;
        LOG("m3portup: the gating QUERY_PORT on port %u failed (fw status 0x%x); "
            "nothing was written", (unsigned)port, (unsigned)qp.fwStatus);
        return kIOReturnSuccess;
    }
    recordPortQuery(port, qp);
    out[kPortUpOutQpFwStatus] = qp.fwStatus;
    out[kPortUpOutSupported]  = qp.supportedTypes;
    out[kPortUpOutLinkBefore] = qp.linkState;
    if (!(qp.supportedTypes & CX3_PORT_TYPE_ETH)) {
        LOG("m3portup: port %u reports supported_types %u, which does not include "
            "Ethernet (2); nothing is written. Switching a port's protocol is a "
            "different operation and is not attempted here.",
            (unsigned)port, (unsigned)qp.supportedTypes);
        out[kPortUpOutStatus] = (UInt64)kIOReturnUnsupported;
        return kIOReturnSuccess;
    }

    /* 2. SET_PORT, Ethernet general */
    /*
     * The MTU here is the WIRE size, not an L3 MTU: the reference clamps this very field with
     * `eth_mtu_cap[port] + ETH_HLEN + VLAN_HLEN + ETH_FCS_LEN` (mlx4_port.c:1306-1308), which
     * is why the default is 1500 + 22 and why the named constants are used rather than the sum
     * written out (src/mlx4_regs.h has the convention and the citations).
     */
    mtu = (UInt16)(mtuIn ? mtuIn
                         : (UInt32)(1500u + MLX4_ETH_HLEN + MLX4_ETH_VLAN_LEN + MLX4_ETH_FCS_LEN));
    out[kPortUpOutMtu] = mtu;
    /*
     * Remember BOTH numbers, and keep them apart.
     *
     * What was programmed (the wire size) is what bounds a frame the port will carry today, and
     * it is what getMaxPacketSize has to answer with.  What QUERY_PORT reported (eth_mtu) is a
     * CAPABILITY -- mlx4's own name for the field is eth_mtu_cap, device.h:532 -- and it is only
     * the ceiling that a later jumbo run could ask for.  Reading one as the other would either
     * advertise frames the port drops (capability as configuration) or hide jumbo entirely
     * (configuration as capability), which is why they are two members and not one.
     */
    if (qp.ethMtu && (UInt32)mtu > qp.ethMtu +
        (MLX4_ETH_HLEN + MLX4_ETH_VLAN_LEN + MLX4_ETH_FCS_LEN)) {
        out[kPortUpOutStatus] = (UInt64)kIOReturnBadArgument;
        LOG("m3portup: port %u wire MTU %u exceeds reported payload capability %u + overhead",
            (unsigned)port, (unsigned)mtu, (unsigned)qp.ethMtu);
        return kIOReturnSuccess;
    }
    /*
     * ROCE_V1_V2: set both fields together or neither, as the reference does
     * (mlx4_port.c:1562-1566).
     *
     * The test is against bmme_flags, which is where this capability actually comes
     * from (mlx4_fw.c:1015-1016 sets the flag2 bit from it).  Testing the flag2 bit
     * instead would have been always-false here, because this port's capability decode
     * never collects that particular bit -- an always-false test that looks like a live
     * check is worse than no test.
     */
    roce = (fDevCap.bmme_flags & MLX4_BMME_FLAG_ROCE_V1_V2) != 0;
    LOG("m3portup: port %u bmme_flags 0x%08x -> RoCE V1/V2 %s",
        (unsigned)port, (unsigned)fDevCap.bmme_flags, roce ? "advertised" : "not advertised");
    portState.mtuConfigured = false; /* ambiguous SET must not advertise an old size */
    portState.frameMtu = 0;
    if (!fHw.setPortGeneralEth(port, mtu, roce, &setStatus)) {
        portState.mtuFwStatus = setStatus;
        out[kPortUpOutSetFwStatus] = setStatus;
        out[kPortUpOutStep] = kPortUpStepGeneral;
        LOG("m3portup: SET_PORT general failed on port %u (fw status 0x%x); stopping "
            "before INIT_PORT", (unsigned)port, (unsigned)setStatus);
        return kIOReturnSuccess;
    }
    portState.mtuFwStatus = setStatus;
    portState.frameMtu = (UInt32)mtu;
    portState.mtuConfigured = true;
    out[kPortUpOutSetFwStatus] = setStatus;
    out[kPortUpOutStep] = kPortUpStepGeneral;

    /*
     * 3. SET_PORT qpn_calc -- tell the firmware where the A0 MAC-derived QPN range starts.
     *
     * The reference's normal port activation is these three commands in this order
     * (mlx4_en_netdev.c:1388 general, :1394 qpn_calc, :1403 INIT_PORT).  Without the middle
     * one the port comes up and the link trains, and every frame the firmware steers by the MAC
     * table goes to a QPN based on a base the firmware was never told -- which is the state
     * stages 4/5 measured their way into: the RX QP exists at QPN 64, the MAC table holds the
     * card's address in slot 0, and nothing has said what 64 means (docs/M2B-NOTES.md section
     * 41, docs/R4-STAGE7-PLAN.md section 4).
     *
     * THE BASE IS THIS PORT'S RX QPN, and that is the plan's decision 1
     * (docs/R4-STAGE7-PLAN.md section 5).  The alternative -- the reference's own arithmetic,
     * num_qps - (1 << log_max_macs) = 896, which keeps the 128 MAC-derived QPNs off the top of
     * the space -- is REFUTED as a starting point by the numbers: this profile configures 1024
     * QPs, so the whole MAC range would be 128 slots at 896..1023 with the RX QP (64) outside
     * it, i.e. MAC slot 0 would map to a QPN this driver does not own and nothing would
     * receive.  Putting the base at 64 makes slot 0 self-consistent by construction.
     *
     * THE PRICE, which the log and docs/R4-STAGE7-RESULT.md both state: with base 64 the MAC
     * table can carry ONE usable entry.  Index i maps to 64 + i, so index 1 lands on 65 -- the
     * indirection QP stage 5 created -- and index 2 on 66, the send QP stage 6 created, and so
     * on into the rest of this driver's numbering.  Fixing that means moving the base (and the
     * RX QP with it) to the top of the space, which is a change to stage 4's numbers and a
     * decision, not an implementation detail; it is recorded rather than taken.
     *
     * Where the base comes from, read at runtime and never written down:
     *
     *   - if the RX QP of this boot exists, its own number (fPortQueues[0].qpQpn, which stage 4 read back
     *     out of QUERY_QP -- "the number is the one the device confirmed", not the one the
     *     driver intended);
     *   - otherwise the SAME rule that will give the RX QP its number, the firmware's reserved
     *     share (fDevCap.reserved_qps, which stage 4 uses as its qpnRule).  This is the usual
     *     branch: m3portup runs BEFORE r4qp in the verification chain, because the reference
     *     tells the firmware the base and only then starts building queues.
     *
     * Both branches log which one was used, and neither is a literal.  The refusal below is the
     * one guard this step adds: a base outside the profile means fDevCap/fInitHca were never
     * populated (or the profile is smaller than the firmware's own reserved range), and a base
     * of 0 would be a command that names the firmware's own QPs as the MAC table's target.
     */
    {
        UInt32 qpnProfileMax = (fInitHca.log_num_qps < 32u)
                                   ? (1u << fInitHca.log_num_qps) : 0xffffffffu;
        const Cx3PortQueues &queue=fPortQueues[port-1];
        UInt32 baseQpn = queue.numbersReserved ? queue.numbers.macBase :
            (queue.qpUp ? queue.qpQpn : fDevCap.reserved_qps);
        UInt32 nMacLog = qp.logMaxMacs;
        UInt32 macSlots = (nMacLog < 32u) ? (1u << nMacLog) : 0xffffffffu;
        UInt32 mPromisc = (fDevCap.flags & MLX4_DEV_CAP_FLAG_VEP_MC_STEER)
                              ? MCAST_DIRECT : MCAST_DEFAULT;
        const UInt32 promisc = 0;   /* the reference's normal activation path, :1394 */

        LOG("m3portup: qpn_calc base QPN %u comes from %s; the firmware's reserved share "
            "(fDevCap.reserved_qps) is %u and this profile configured %u QPs",
            (unsigned)baseQpn,
            queue.qpUp ? "the RX QP that exists in this boot, read back from QUERY_QP by stage 4"
                    : "the rule the RX QP will take its number from, because the RX QP does not "
                      "exist yet in this boot (m3portup runs before r4qp)",
            (unsigned)fDevCap.reserved_qps, (unsigned)qpnProfileMax);
        if (baseQpn < fDevCap.reserved_qps || baseQpn >= qpnProfileMax) {
            LOG("m3portup: refusing to send qpn_calc with base QPN %u. It has to be a QPN this "
                "driver owns (>= the firmware's reserved share %u) and inside the profile (%u "
                "QPs, log_num_qps %u) -- outside that, the command would point A0's MAC table "
                "at the firmware's own queues, and a successful command would look exactly like "
                "a correct one. The usual cause is that m2dprofile has not run in this boot "
                "(run it first). Nothing was written by this step.",
                (unsigned)baseQpn, (unsigned)fDevCap.reserved_qps, (unsigned)qpnProfileMax,
                (unsigned)fInitHca.log_num_qps);
            out[kPortUpOutStatus] = (UInt64)kIOReturnNotReady;
            return kIOReturnSuccess;
        }
        /*
         * VEP_MC_STEER is read off the device rather than assumed: it is bit 42 of the 64-bit
         * caps.flags word this port has held since QUERY_DEV_CAP, and the reference's whole
         * choice between the two multicast forms is that one bit (mlx4_port.c:1585-1586).
         * Measured on this card: set, so MCAST_DIRECT (1).  The consequence of getting it
         * wrong does not show up here -- it would show up at stage 9 as multicast and broadcast
         * frames landing on the wrong QPN, i.e. as "ping does not answer" three stages later.
         */
        LOG("m3portup: caps.flags 0x%016llx -> VEP_MC_STEER (bit 42) %s -> m_promisc %u (%s); "
            "promisc %u, and n_mac %u is QUERY_PORT's log_max_macs (1 << %u = %u MAC-derived "
            "QPNs)",
            (unsigned long long)fDevCap.flags,
            (fDevCap.flags & MLX4_DEV_CAP_FLAG_VEP_MC_STEER) ? "set" : "clear",
            (unsigned)mPromisc, (mPromisc == MCAST_DIRECT) ? "MCAST_DIRECT" : "MCAST_DEFAULT",
            (unsigned)promisc, (unsigned)nMacLog, (unsigned)nMacLog, (unsigned)macSlots);
        if (!fHw.setPortQpnCalc(port, baseQpn, promisc, nMacLog, mPromisc, &qpnCalcStatus)) {
            out[kPortUpOutQpnCalcFwStatus] = qpnCalcStatus;
            out[kPortUpOutStep] = kPortUpStepQpnCalc;
            LOG("m3portup: SET_PORT qpn_calc failed on port %u (fw status 0x%x); stopping "
                "before INIT_PORT. That is deliberate: initialising a port whose MAC-derived "
                "QPN base the firmware has not been told is a port that comes up and steers "
                "frames nowhere useful, and the failure would then surface at stage 9 as 'no "
                "frames arrive' instead of here.", (unsigned)port, (unsigned)qpnCalcStatus);
            return kIOReturnSuccess;
        }
        out[kPortUpOutQpnCalcFwStatus] = qpnCalcStatus;
        out[kPortUpOutStep] = kPortUpStepQpnCalc;
        LOG("m3portup: qpn_calc accepted (fw status 0x%x): the firmware's A0 mapping is now "
            "index i -> %u + i. MAC table slot 0 (the address stage 5 registered) is QPN %u; "
            "slots 1..%u reach %u..%u, which is inside this driver's own numbering (the "
            "indirection QP 65, the send QP 66), so this configuration supports one MAC "
            "address. Multi-MAC needs the base at num_qps - (1 << log_max_macs) = %u with the RX "
            "QP moved alongside it -- recorded in docs/R4-STAGE7-RESULT.md, not done here.",
            (unsigned)qpnCalcStatus, (unsigned)baseQpn, (unsigned)baseQpn,
            (unsigned)(macSlots - 1u), (unsigned)(baseQpn + 1u),
            (unsigned)(baseQpn + macSlots - 1u),
            (unsigned)((qpnProfileMax > macSlots) ? (qpnProfileMax - macSlots) : 0u));
    }

    /* 4. INIT_PORT */
    /* Once opening is attempted, an earlier CLOSE_PORT receipt is no longer
     * usable, even if INIT_PORT times out or the following QUERY_PORT fails.
     * Each port invalidates only its own close receipt. */
    portState.closeConfirmed = false;
    portState.initAttempted = true;
    portState.initAccepted = false;
    portState.linkKnown = false;
    portState.linkUp = false;
    portState.speedMbps = 0;
    if (!fHw.initPort(port, &initStatus)) {
        portState.initFwStatus = initStatus;
        out[kPortUpOutInitFwStatus] = initStatus;
        out[kPortUpOutStep] = kPortUpStepInitPort;
        LOG("m3portup: INIT_PORT failed on port %u (fw status 0x%x)", (unsigned)port,
            (unsigned)initStatus);
        return kIOReturnSuccess;
    }
    portState.initFwStatus = initStatus;
    portState.initAccepted = true;
    out[kPortUpOutInitFwStatus] = initStatus;
    out[kPortUpOutStep] = kPortUpSteps;

    /*
     * 5. Read the link state.
     *
     * A single reading right after INIT_PORT is not a fair test of "link up": the
     * physical link trains on its own schedule.  So the link bit is polled over a
     * bounded window and the first observed up is reported together with how long it
     * took.  "Up at 300 ms" and "still down after 5 s" are different results and are
     * not collapsed into one pass/fail.
     */
    {
        UInt32 waitedMs = 0;
        const UInt32 windowMs = 5000;
        bool up = false;
        while (waitedMs <= windowMs) {
            if (!fHw.queryPort(port, &qp)) {
                recordPortQuery(port, qp);
                out[kPortUpOutStatus] = (UInt64)kIOReturnError;
                return kIOReturnSuccess;
            }
            recordPortQuery(port, qp);
            if (qp.linkState) { up = true; break; }
            IOSleep(100);
            waitedMs += 100;
        }
        out[kPortUpOutLinkAfter]  = qp.linkState;
        out[kPortUpOutSpeedAfter] = qp.linkSpeed;
        /*
         * ...and keep it.  Until 0.44.6 the link state existed only in this call's outbox, so
         * the driver had no reading of its own to publish when the stack later asked -- and the
         * interface reported the link it was told to report rather than the one measured here.
         */
        portState.linkUp    = (qp.linkState != 0);
        portState.speedMbps = cx3LinkSpeedMbps(qp.linkSpeed);
        /*
         * ...and the address, in the same block, because it is read from the same command and it
         * is the value stage 5 registered in the steering table.  The interface step used to send
         * its own QUERY_PORT for this; it now reads what this block published, for the reasons in
         * the header (four measured machine deaths, and "one fact, one reading").
         */
        {
            UInt32 mi = (port >= 1u && port <= 2u) ? port - 1u : 0u;
            LOG("m3portup: port %u's address is held for its interface step (%02x:%02x:%02x:%02x:%02x:%02x)",
                (unsigned)port, fPorts[mi].mac[0], fPorts[mi].mac[1], fPorts[mi].mac[2],
                fPorts[mi].mac[3], fPorts[mi].mac[4], fPorts[mi].mac[5]);
        }
        /* This port's close receipt was invalidated before INIT_PORT, independently
         * of whether link polling obtained a usable observation. */
        LOG("m3portup: port %u link after INIT_PORT: %s (link bit %u, speed code 0x%02x "
            "= %u Mb/s after %u ms of polling)",
            (unsigned)port, up ? "UP" : "still DOWN", (unsigned)qp.linkState,
            (unsigned)qp.linkSpeed, (unsigned)cx3LinkSpeedMbps(qp.linkSpeed),
            (unsigned)waitedMs);
        if (!up)
            LOG("m3portup: the link bit is 0 after %u ms. That means 'down'; it does NOT "
                "say why -- these fields carry no 'no cable' reason code, so a cable, the "
                "far end, or the port not being trained are all still open. The command "
                "statuses above are what separate 'the HCA accepted the configuration' "
                "from 'the link came up'.", (unsigned)windowMs);
    }

    out[kPortUpOutStatus] = 0;
    return kIOReturnSuccess;
}

/*
 * kCx3SelM3AbRing -- the ring-initialisation A/B experiment.
 *
 * Refused while any EQ exists, because the experiment hands a ring to SW2HW_EQ on the
 * same EQ number the production queue would use; running it over a live EQ would
 * replace that EQ's context and quietly break the thing it is meant to diagnose.
 */
IOReturn Cx3Mlx4::exportM3AbRing(const UInt64 *in, Cx3AbRingResult *res, UInt64 *out)
{
    bool handOverB = (in[kAbRingInHandOverB] != 0);
    UInt32 eqn = (UInt32)in[kAbRingInEqn];
    UInt32 dummyMtt = (UInt32)in[kAbRingInDummyMtt];

    /*
     * The EQN is an input so this experiment can answer the question the first
     * version of it could not: whether the ring write happens once per EQN or once
     * per boot.  Guards, in order: default to the asynchronous EQ; refuse a number
     * below the reserved range (those belong to the firmware's own queues -- this
     * card reserves 28, and EQN 28 is the async EQ); refuse anything at or above the
     * maximum QUERY_FUNC reported, when that is known.  It is not known where this
     * experiment runs -- before m3equp -- because QUERY_FUNC is part of the EQ
     * bring-up; the reserved-range guard, which is the one that matters, always
     * applies.
     */
    /*
     * Ask QUERY_FUNC first, so the default and the guard come from the DEVICE.
     *
     * These were literal 28s, and so was the earlier explanation of the rule.  That is the
     * habit the "do not hard-code what can be read back" rule exists to stop, and here it
     * was inconsistent within one repository: m3equp derives the asynchronous EQ number
     * from QUERY_FUNC while this selector wrote it down.  A card that reserved a different
     * number of EQs would have been described wrongly by one of them, and the failure would
     * have been an experiment that silently ran against a firmware-owned queue.
     *
     * fFuncReservedEq is what QUERY_FUNC reports (28 on this card), and the asynchronous EQ
     * is the first of the reserved range.
     */
    if (fFuncMaxEq == 0) {
        UInt32 fwStatus = CX3_FWSTATUS_NOT_POSTED;
        if (!fHw.queryFunc(&fFuncMaxEq, &fFuncReservedEq, &fFuncReservedUar, &fwStatus)) {
            LOG("abring refused: QUERY_FUNC failed (fw status 0x%x), so neither the default "
                "EQ number nor its range can be known", (unsigned)fwStatus);
            out[kAbRingOutStatus] = (UInt64)kIOReturnError;
            return kIOReturnSuccess;
        }
        LOG("abring: QUERY_FUNC says max_eq %u, reserved_eqs %u (asked here because the EQ "
            "number has to come from the device, not from this source file)",
            (unsigned)fFuncMaxEq, (unsigned)fFuncReservedEq);
    }
    if (eqn == 0) eqn = fFuncReservedEq;          /* the asynchronous EQ, as m3equp picks it */
    if (eqn < fFuncReservedEq) {
        LOG("abring refused: EQN %u is inside the reserved range (< %u), which belongs to "
            "the firmware's own queues", (unsigned)eqn, (unsigned)fFuncReservedEq);
        out[kAbRingOutStatus] = (UInt64)kIOReturnBadArgument;
        return kIOReturnSuccess;
    }
    if (eqn >= fFuncMaxEq) {
        LOG("abring refused: EQN %u is at or past max_eq %u that QUERY_FUNC reported",
            (unsigned)eqn, (unsigned)fFuncMaxEq);
        out[kAbRingOutStatus] = (UInt64)kIOReturnBadArgument;
        return kIOReturnSuccess;
    }

    memset(out, 0, sizeof(UInt64) * kAbRingOutCount);
    memset(res, 0, sizeof(*res));
    out[kAbRingOutStatus]    = (UInt64)kIOReturnError;
    out[kAbRingOutSw2hwStat] = CX3_FWSTATUS_NOT_POSTED;
    out[kAbRingOutHw2swStat] = CX3_FWSTATUS_NOT_POSTED;

    if (fEqRing) {
        LOG("abring refused: an EQ is already up (EQN %u). This experiment hands a ring "
            "to SW2HW_EQ on EQN %u, which would replace that context. Run it before "
            "'m3equp', or reset first.", (unsigned)fEqn, (unsigned)eqn);
        out[kAbRingOutStatus] = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }

    /*
     * The same shape the production EQ uses: 512 entries, 64-byte EQEs.  The point is
     * to reproduce observations made on a real EQ, so the ring has to be the same size
     * and format, not a convenient smaller one.
     */
    if (!fHw.eqAbExperiment(handOverB, eqn, 512, 1, dummyMtt, res)) {
        out[kAbRingOutSw2hwStat] = res->sw2hwStatus;
        out[kAbRingOutHw2swStat] = res->hw2swStatus;
        if (res->sw2hwStatus != CX3_FWSTATUS_NOT_POSTED) out[kAbRingOutStatus] = 0;
        return kIOReturnSuccess;
    }

    out[kAbRingOutStatus]    = 0;
    out[kAbRingOutSw2hwStat] = res->sw2hwStatus;
    out[kAbRingOutHw2swStat] = res->hw2swStatus;
    out[kAbRingOutUnknown]   = res->unknown;
    out[kAbRingOutCmdUs]     = res->cmdUs;
    out[kAbRingOutQueryEq0]  = res->queryEqStatus[0];
    out[kAbRingOutQueryEq1]  = res->queryEqStatus[1];
    return kIOReturnSuccess;
}



/* ==================================================================== */
/* M3 restart 2: interrupts                                             */
/* ==================================================================== */

/*
 * Consume events in order from the head of the queue.
 *
 * Caller holds fEqLock.  One implementation for the poll selector and the
 * interrupt action, because two consumers is two chances to lose an event: the
 * previous version scanned forward for any ready slot and then advanced the index
 * past it, and a delivered EQE ended up behind the read pointer.
 *
 * Ordered means ordered: it looks at the slot the persistent index points at, and
 * stops the moment that slot is not consumable.  Anything else silently discards
 * the events in between.
 */
UInt32 Cx3Mlx4::consumeEqLocked(UInt32 max, UInt32 *outConsAfter,
                               bool *outBlockedByComp)
{
    UInt8  eqe[MLX4_EQ_ENTRY_SIZE];
    UInt32 consumed = 0;

    if (outConsAfter) *outConsAfter = fEqConsIndex;
    if (outBlockedByComp) *outBlockedByComp = false;
    if (!fEqRing) return 0;

    while (consumed < max) {
        UInt32 token, type;

        memset(eqe, 0, sizeof(eqe));
        if (!fHw.eqNextEqe(fEqRing, fEqNent, fEqeFactor, fEqConsIndex, eqe)) break;

        type = eqe[MLX4_EQE_TYPE_OFFSET];
        fIrqLastType = type;
        if (type == 0) {
            /*
             * Type 0 is a legitimate COMP event (device.h:293); this port has no
             * completion queues, so nothing can consume it.  Counted and left in
             * place -- stepping over it is what caused the walk-past.
             */
            ++fIrqErrors;
            if (outBlockedByComp) *outBlockedByComp = true;
            LOG("consumeEq: slot %u is ready with type 0x00 (a COMP event); nothing "
                "consumes completions yet, leaving it in place",
                (unsigned)(fEqConsIndex & (fEqNent - 1)));
            break;
        }

        if (type == MLX4_EVENT_TYPE_CMD) {
            token = ((UInt32)eqe[MLX4_EQE_CMD_TOKEN_OFFSET] << 8) |
                    (UInt32)eqe[MLX4_EQE_CMD_TOKEN_OFFSET + 1];
            if (fIrqPending && token == fIrqWantToken) {
                fIrqMatched = true;
                fIrqMatchedToken = token;
                fIrqMatchedStatus = eqe[MLX4_EQE_CMD_STATUS_OFFSET];
                LOG("consumeEq: CMD EQE token 0x%x matches the pending test, status "
                    "0x%x (slot %u)", (unsigned)token, (unsigned)fIrqMatchedStatus,
                    (unsigned)(fEqConsIndex & (fEqNent - 1)));
            } else {
                LOG("consumeEq: CMD EQE token 0x%x does not match the pending 0x%x; "
                    "consumed and recorded, not reported as the test", (unsigned)token,
                    (unsigned)(fIrqPending ? fIrqWantToken : 0xffff));
            }
        } else {
            LOG("consumeEq: event type 0x%02x at slot %u (not a CMD completion)",
                (unsigned)type, (unsigned)(fEqConsIndex & (fEqNent - 1)));
        }

        ++fEqConsIndex;
        ++consumed;
    }

    /*
     * Re-arm at the end of every pass, consumed or not.
     *
     * The reference calls eq_set_ci(eq, 1) unconditionally when its scan ends
     * (mlx4_eq.c:829), and both reviews flagged that this port only re-armed when it
     * had consumed something -- which leaves the notification state different from
     * the reference after a no-progress pass.
     */
    fHw.eqSetConsumerIndex(fEqDoorbell, fEqConsIndex, true);
    if (outConsAfter) *outConsAfter = fEqConsIndex;
    return consumed;
}

/*
 * The interrupt action.
 *
 * Runs on the workloop.  It does what the reviews said a bottom half should do
 * here: consume the queue in order, record any completion that matches the
 * pending token, advance the consumer index and re-arm.  It does NOT send
 * firmware commands -- any consumer that needs one must schedule it elsewhere,
 * because the command path sleeps and this thread must not wait on work that the
 * same workloop would have to run.
 */
void Cx3Mlx4::irqAction(OSObject *owner, IOInterruptEventSource *src, int count)
{
    Cx3Mlx4 *me = OSDynamicCast(Cx3Mlx4, owner);
    UInt32 after = 0, got;
    bool storm = false;
    UInt64 total;

    if (!me) return;

    /*
     * Acknowledge the interrupt at the device BEFORE touching the queue.
     *
     * This is the step whose absence wedged the machine.  The reference writes the
     * clear register as the first statement of its legacy handler, ahead of the EQ
     * scan (mlx4_eq.c:841 then :843); without it a level-triggered line re-asserts
     * immediately and the handler runs forever consuming nothing.
     *
     * Only for a legacy source: an MSI-X notification is edge-like and the reference
     * does not clear anything on that path.
     */
    if (me->fClrIntLegacy && me->fHw.clrIntReady()) {
        if (me->fHw.clrIntWrite()) ++me->fClrIntWrites;
        else LOG("irq action: the clear register write FAILED -- a legacy source that "
                 "cannot be acknowledged will storm; disabling it");
    }

    if (me->fEqLock) IOLockLock(me->fEqLock);
    ++me->fIrqCount;
    /*
     * The callback's count argument is accumulated rather than ignored: it is the
     * only hardware-side number available, and the reviews pointed out that action
     * invocations are not the same quantity as interrupts.
     */
    if (count > 0) me->fIrqCallbackCount += (UInt64)count;
    {
        bool blockedByComp = false;
        got = me->consumeEqLocked(64, &after, &blockedByComp);
        me->fIrqEqeCount += got;
        /*
         * Zero consumption does not mean "the device produced nothing": a legal COMP
         * event at the head blocks the queue, because this port has no completion
         * queues to consume it with.  That case must not feed the storm counter --
         * otherwise a driver that is merely missing a feature would look like a
         * storming interrupt.  Both reviews made this point.
         */
        if (got == 0 && !blockedByComp) {
            ++me->fIrqSkipped;
            if (++me->fIrqZeroStreak >= kIrqZeroStreakLimit) storm = true;
        } else if (got) {
            me->fIrqZeroStreak = 0;
        }
    }
    total = me->fIrqCount;
    if (me->fEqLock) IOLockUnlock(me->fEqLock);

    /*
     * Storm guard, and it acts before it speaks.
     *
     * Measured, and expensive to learn: with a level-triggered interrupt that is
     * never cleared at the device, the handler is invoked again immediately.  The
     * log filled with "irq action: 0 EQE(s) consumed" while the counter climbed
     * 77, 78, 79, ... and the machine wedged -- the panic was a waitq lock timeout
     * under WindowServer, with nothing of this kext in the backtrace.
     *
     * A run of invocations that consume nothing means the source is asserting
     * without producing events.  Disabling it is the difference between a driver
     * that reports "the interrupt never stopped asserting" and a machine that has
     * to be power-cycled.  The count is generous because a genuine shared line can
     * plausibly fire a few times with nothing to do, and this is a diagnostic
     * fallback, not a substitute for clearing the interrupt properly.
     */
    if (storm) {
        /*
         * Restrict delivery first, then report once.  The earlier version logged
         * and only then disabled, on the hot path, on every invocation.
         *
         * The event source is deliberately NOT released here: this runs on the
         * workloop and freeing the object whose callout is executing would be
         * exactly the use-after-free the reviews warned about.  It is disabled and
         * the fault is latched; m3irqdown releases it, and that path is written to
         * work off object existence rather than off the "session is up" flag, which
         * used to make it return early and leak.
         */
        if (src) src->disable();
        me->fIrqEnabled = false;
        if (me->fEqLock) IOLockLock(me->fEqLock);
        me->fIrqStormTripped = true;
        if (me->fEqLock) IOLockUnlock(me->fEqLock);
        LOG("irq action: STORM GUARD TRIPPED after %u consecutive invocations that "
            "consumed nothing. Delivery is disabled and the fault is latched; run "
            "'m3irqdown' to release the session. (An uncleared level interrupt storms, "
            "and this one has already wedged this machine once.)",
            (unsigned)me->fIrqZeroStreak);
        return;
    }

    /*
     * Hot-path logging, rate-limited.  Both reviews flagged the per-invocation log:
     * on a storming line it is itself a source of load, and it fills the log with
     * the same line.  Only the first few no-progress invocations and every
     * consumption are reported.
     */
    if (got || me->fIrqSkipped <= 4)
        LOG("irq action: %u EQE(s) consumed, consumer index now %u, invocation %llu "
            "(zero streak %u, skipped so far %llu)", (unsigned)got, (unsigned)after,
            (unsigned long long)total, (unsigned)me->fIrqZeroStreak,
            (unsigned long long)me->fIrqSkipped);
}

/*
 * kCx3SelM3IrqUp -- open an interrupt session.
 *
 * Order per the reviews: create the software objects first so that a notification
 * arriving immediately has somewhere to land, then attach the source to the
 * provider, then enable.  Nothing here touches a device register other than the
 * EQ doorbell write that arm/consume already does.
 */
/*
 * Release the interrupt objects, if they exist.
 *
 * Works off object existence, not off a "session is up" flag: the first version
 * cleared that flag when the storm guard tripped and m3irqdown then returned early
 * and left the source and the workloop allocated forever.  Both reviews found it.
 *
 * Order is the one the reviews described: stop delivery, remove the source, then
 * release. The local macOS SDK contract says removeEventSource does not return
 * until the workloop acknowledges removal and releases its source reference. A
 * failure return is not that acknowledgement, so keep the local source reference
 * and every dependent queue owner reachable and refuse teardown.
 */
bool Cx3Mlx4::irqReleaseObjects(const char *why)
{
    if (!fIrqObjectsUp && !fIrqSource) return true;

    if (fIrqSource) {
        fIrqSource->disable();
        fIrqEnabled = false;
        if (!fWorkLoop) {
            LOG("irq objects retained (%s): event source exists without its workloop; "
                "the source and every queue it can reach must remain alive", why);
            return false;
        }
        const IOReturn removeRc = fWorkLoop->removeEventSource(fIrqSource);
        if (removeRc != kIOReturnSuccess) {
            LOG("irq objects retained (%s): removeEventSource returned 0x%x; source "
                "ownership is uncertain, so no dependent queue or BAR state may be torn down",
                why, (unsigned)removeRc);
            return false;
        }
        fIrqSource->release();
        fIrqSource = 0;
    }
    /* The workloop is NOT released here.  It is the one getWorkLoop() hands the networking
     * family, and the family schedules on it for as long as this controller exists; freeing it
     * when the interrupt session ends would leave the family holding a freed object.  It is
     * released in stop(), with everything else this driver owns. */
    fIrqObjectsUp = false;
    fIrqEnabled = false;
    LOG("irq objects released (%s): %llu invocation(s), %llu callback count, "
        "%llu EQE(s) consumed, %llu skipped, storm tripped %s", why,
        (unsigned long long)fIrqCount, (unsigned long long)fIrqCallbackCount,
        (unsigned long long)fIrqEqeCount, (unsigned long long)fIrqSkipped,
        fIrqStormTripped ? "yes" : "no");
    return true;
}

/*
 * Refuse anything this port cannot service safely.
 *
 * Returns true when the source may be enabled.  The reviews were specific: a
 * getInterruptType failure is not "unknown, proceed" -- it is a stop.  And a
 * non-messaged source is refused unconditionally for now, because the legacy path
 * requires clearing the interrupt at the device, which is not implemented.  An
 * override that let it through would only re-create the storm, so there is no
 * override: the correct missing piece is the clear, not a flag.
 */
bool Cx3Mlx4::irqSourceAcceptable(int source, int type, IOReturn typeRc,
                                  UInt64 *outStatusOut)
{
    if (typeRc != kIOReturnSuccess) {
        LOG("m3irqup refused: getInterruptType(source %d) failed with 0x%x; the "
            "interrupt type is unknown, and enabling an unknown source is how the "
            "storm started", source, (unsigned)typeRc);
        if (outStatusOut) *outStatusOut = (UInt64)kIOReturnUnsupported;
        return false;
    }
    if (!(type & kIOInterruptTypePCIMessaged)) {
        /*
         * Legacy is allowed, but only with a working clear path -- which is now
         * implemented and is a separate, testable prerequisite (m3clrint).  The rule
         * the reviews asked for: a legacy source may be enabled once the interrupt
         * can be acknowledged, and not before.  Enabling it without that is what
         * wedged the machine.
         */
        if (!fClrIntReady) {
            LOG("m3irqup refused: source %d reports legacy (0x%x) but the interrupt "
                "clear is not set up, so nothing could acknowledge it and it would "
                "storm. Run 'm3clrint' first (QUERY_ADAPTER's inta_pin plus QUERY_FW's "
                "clr_int_base/bar), then try again.", source, (unsigned)type);
            if (outStatusOut) *outStatusOut = (UInt64)kIOReturnNotReady;
            return false;
        }
        LOG("m3irqup: source %d is legacy (0x%x) and the clear register is set up, so "
            "the action will acknowledge it before consuming", source, (unsigned)type);
        fClrIntLegacy = true;
        return true;
    }
    fClrIntLegacy = false;
    return true;
}

IOReturn Cx3Mlx4::exportM3IrqUp(const UInt64 *in, UInt64 *out)
{
    UInt32 sourceIndex = (UInt32)in[kIrqUpInSource];
    int    type = 0;
    UInt64 u64Status = (UInt64)kIOReturnError;
    (void)in[kIrqUpInForceLegacy];
    IOReturn rc;

    memset(out, 0, sizeof(UInt64) * kIrqUpOutCount);

    /*
     * Guard on object existence, not on a session flag: after a storm trip the flag
     * used to be false while the objects were still allocated, so a reopen would
     * have overwritten them.
     *
     * fWorkLoop is deliberately NOT part of this test.  The workloop's lifetime is the
     * controller's, not the session's: getWorkLoop() hands it to the networking family,
     * and irqReleaseObjects() says so in its own comment ("the workloop is NOT released
     * here ... It is released in stop()").  Testing it here made a SECOND session
     * impossible in the same boot, which cost a reboot per attempt and is measured:
     * 2026-10-13, m3irqup succeeded once (12:05:27) and then every later attempt in that
     * boot was refused with "interrupt objects still exist (enabled=0)" -- 12:08:15,
     * 12:08:55, 12:16:00, 12:17:02 -- while the refusals BEFORE it named other reasons
     * ("no EQ is up", "the interrupt clear is not set up"), which is how the workloop
     * was identified as the thing that changed (docs/HANG-AUDIT.md 10.6).
     */
    if (fIrqObjectsUp || fIrqSource) {
        LOG("m3irqup refused: interrupt objects still exist (enabled=%u, storm tripped "
            "%s); run m3irqdown first", (unsigned)fIrqEnabled,
            fIrqStormTripped ? "yes" : "no");
        out[kIrqUpOutStatus] = (UInt64)kIOReturnStillOpen;
        return kIOReturnSuccess;
    }
    if (!fEqRing) {
        LOG("m3irqup refused: no EQ is up (run m3equp first)");
        out[kIrqUpOutStatus] = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }
    if (fEqUnknownRetained) {
        LOG("m3irqup refused: EQN %u has an unknown retained owner and cannot be armed",
            (unsigned)fEqn);
        out[kIrqUpOutStatus] = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }
    if (!fPci) {
        out[kIrqUpOutStatus] = (UInt64)kIOReturnNotAttached;
        return kIOReturnSuccess;
    }

    if (!fEqLock) fEqLock = IOLockAlloc();
    if (!fEqLock) {
        out[kIrqUpOutStatus] = (UInt64)kIOReturnNoMemory;
        return kIOReturnSuccess;
    }

    /*
     * Ask the provider what kind of interrupt this source is.  Recorded rather than
     * assumed: which provider index corresponds to which MSI-X table entry is not
     * something this project has measured yet, and the EQ context's intr field is
     * already committed to table entry 0 (that is what the reference fills in for
     * its asynchronous EQ, mlx4_main.c:2860-2874 and mlx4_eq.c:1200-1202).
     */
    rc = fPci->getInterruptType((int)sourceIndex, &type);
    LOG("m3irqup: provider interrupt source %u: getInterruptType rc=0x%x type=0x%x "
        "(%s)", (unsigned)sourceIndex, (unsigned)rc, (unsigned)type,
        (type & kIOInterruptTypePCIMessaged) ? "PCI-messaged (MSI/MSI-X)" :
        "not reported as PCI-messaged");
    out[kIrqUpOutIntType] = (UInt64)(UInt32)type;
    out[kIrqUpOutGetTypeRc] = (UInt64)(UInt32)rc;

    /*
     * Refuse a legacy source unless it is asked for deliberately.
     *
     * This is the fix for a hang I caused: the provider reported legacy, the
     * driver enabled the event source anyway, and because a level-triggered line
     * that is never cleared re-asserts, the handler was invoked hundreds of times
     * consuming nothing until the system wedged (waitq lock timeout under
     * WindowServer; nothing of this kext in that backtrace).
     *
     * The reference driver clears the interrupt as the first act of its INTx
     * handler, which this port does not do yet, and its MSI-X handler has no such
     * step.  So enabling a non-messaged source here means enabling one this driver
     * cannot clear -- a configuration that can only storm.  Refusing by default
     * makes that impossible to trigger by accident; the override exists so the
     * reason can be recorded once the clear is implemented.
     */
    if (!irqSourceAcceptable((int)sourceIndex, type, rc, &u64Status)) {
        out[kIrqUpOutStatus] = u64Status;
        return kIOReturnSuccess;
    }

    /*
     * ONE WORKLOOP, and this line used to make a second one.
     *
     * It read `fWorkLoop = IOWorkLoop::workLoop();` unconditionally.  Since 0.44.6 the loop is
     * created by createWorkLoop()/getWorkLoop(), which is where IONetworkController expects a
     * subclass to make it (IONetworkController.h:1137-1147) and where this driver hands the
     * family the loop its command gate is built on.  Overwriting that pointer here did three
     * things at once: the interrupt event source went onto a DIFFERENT loop from the family's
     * gate; getWorkLoop() then answered with the new one, so any later family call that asks for
     * the workloop got a third answer from a third place; and the loop the family had cached was
     * leaked -- neither released nor reachable.
     *
     * That is precisely the "two workloops" shape this project has been chasing as the freeze's
     * likely mechanism (see the getWorkLoop() comment in Cx3Mlx4.h: the family's open/close/stop
     * machinery is entered whenever a user client goes away, and every freeze with a surviving
     * log ends at "user client closed").  A fix that rules it out statically is worth a reboot
     * here even though the instruments staged in 0.44.5 are what will decide it.
     *
     * So: reuse.  A workloop that does not exist yet is still created, because a source with no
     * loop has nowhere to run.
     */
    if (!fWorkLoop) {
        fWorkLoop = IOWorkLoop::workLoop();
        if (!fWorkLoop) {
            LOG("m3irqup: could not create a workloop");
            out[kIrqUpOutStatus] = (UInt64)kIOReturnNoMemory;
            return kIOReturnSuccess;
        }
        LOG("m3irqup: no workloop existed yet, so one was created here. getWorkLoop() and "
            "createWorkLoop() normally make it first, which is why this is worth a line");
    } else {
        LOG("m3irqup: reusing the workloop this controller already has -- the one getWorkLoop() "
            "hands the family, so the family's gate and this interrupt source run on the same "
            "loop. Creating a second one here (which is what this line used to do) puts them on "
            "different loops and leaks the first");
    }

    fIrqSource = IOInterruptEventSource::interruptEventSource(
        this, &Cx3Mlx4::irqAction, fPci, (int)sourceIndex);
    if (!fIrqSource) {
        LOG("m3irqup: could not create the interrupt event source for index %u",
            (unsigned)sourceIndex);
        fWorkLoop->release();
        fWorkLoop = 0;
        out[kIrqUpOutStatus] = (UInt64)kIOReturnNoResources;
        return kIOReturnSuccess;
    }
    if (fWorkLoop->addEventSource(fIrqSource) != kIOReturnSuccess) {
        LOG("m3irqup: could not add the event source to the workloop");
        fIrqSource->release();
        fIrqSource = 0;
        fWorkLoop->release();
        fWorkLoop = 0;
        out[kIrqUpOutStatus] = (UInt64)kIOReturnError;
        return kIOReturnSuccess;
    }

    /*
     * Every field the callback can read is initialised BEFORE delivery starts.
     *
     * Both reviews flagged the original order: enable() came first and the counters
     * were reset after it, so an interrupt arriving immediately would have been
     * counted and then had its count erased, or worse, read state that was not set
     * up yet.  Objects exist now, but nothing is enabled until the state is ready.
     */
    fIrqSourceIndex  = sourceIndex;
    fIrqCount        = 0;
    fIrqCallbackCount = 0;
    fIrqEqeCount     = 0;
    fPollEqeCount    = 0;
    fIrqErrors       = 0;
    fIrqSkipped      = 0;
    fIrqPending      = false;
    fIrqMatched      = false;
    fIrqZeroStreak   = 0;
    fIrqStormTripped = false;
    fIrqObjectsUp    = true;
    fClrIntWrites    = 0;

    /*
     * Event sources are created disabled (IOInterruptEventSource.h: "All
     * IOInterruptEventSources are created in the disabled state"), so this is the
     * step that actually starts delivery -- and it is last, deliberately.
     */
    fIrqSource->enable();
    fIrqEnabled = true;

    /*
     * Re-arm the EQ, now that delivery is running.  This is one line, and it is the
     * fix for a state machine rather than a tidy-up.
     *
     * What is measured (docs/EQ-ORDERING.md; M2B section 37): with the chain in the
     * order tools/verify.sh runs -- INIT_PORT first, then m3equp -- the event queue
     * reads QUERY_EQ flags 0x00000a00 (MLX4_EQ_STATE_FIRED) and the interrupt never
     * arrives: m3irqtest is never matched and the action is invoked zero times.  With
     * m3equp first and no port-up at all, the identical call reads 0x00000900
     * (MLX4_EQ_STATE_ARMED) and the completion arrives in under a second.
     *
     * The state machine is the reason, not the order as such.  Arming -- writing the
     * consumer index doorbell with the request-notification bit, which is what
     * eqSetConsumerIndex(..., true) does -- is what lets the HCA assert; the first
     * event it delivers moves the EQ to FIRED, and from FIRED it asserts nothing more
     * until software arms it again.  This port re-arms in two places, and both of them
     * are consumers of the ring: consumeEqLocked(), which the interrupt action and the
     * poll selector call.  So a fire that happens BEFORE delivery is enabled is
     * permanent: no interrupt is raised for it (nothing is listening yet), no action
     * runs, and therefore nothing ever re-arms.  The queue is not broken and its
     * context is intact; it is waiting for a notification that only it can produce.
     *
     * The reference arms at exactly this point, and it is the only arm in its bring-up
     * path.  mlx4_init_eq_table() creates every EQ first (the context written with
     * STATE_ARMED, mlx4_eq.c:1023-1024), then attaches the interrupt (request_irq),
     * then maps the events, and only then -- delivery already on -- calls
     * eq_set_ci(&eq[MLX4_EQ_ASYNC], 1) (mlx4_eq.c:1277).  Its EQs exist before any
     * port is activated (mlx4_init_eq_table is called from mlx4_load_one,
     * mlx4_main.c:2669; mlx4_en_activate runs later), so the reference is not "EQ after
     * INIT_PORT" and never was.  What matters is that the arm is ordered against
     * delivery, which is the part this port had wrong: it armed inside m3equp, a step
     * where the EQ already exists and can fire, and nothing is listening yet.
     *
     * The arm stays in m3equp as well, and the two are not redundant.  A queue that has
     * never been armed produces no completion EQEs, so m3eqpoll, the ring A/B
     * experiment and the ring snapshots would have nothing to read.  The first arm
     * makes the queue usable to a poller; this one makes the notification usable.
     *
     * The index written is the one in use, not zero: an interrupt can already have run
     * between enable() and this line, and writing a stale 0 over an advanced index
     * would send the consumer back over slots it has already read.  fEqLock covers
     * that, and it is the same lock the action takes around consumeEqLocked().
     */
    {
        UInt32 armCi;

        if (fEqLock) IOLockLock(fEqLock);
        armCi = fEqConsIndex;
        fHw.eqSetConsumerIndex(fEqDoorbell, armCi, true);
        if (fEqLock) IOLockUnlock(fEqLock);
        LOG("m3irqup: armed the EQ again after enabling delivery (consumer index %u, "
            "request-notification set) -- the reference arms at this same point, "
            "mlx4_eq.c:1277, and without it an EQ that fired while nothing was listening "
            "stays FIRED and never raises an interrupt. What this is expected to show: "
            "m3irqtest matched, and m3eqinfo reporting a QUERY_EQ flags word of 0x900 "
            "(ARMED) rather than 0xa00 (FIRED).", (unsigned)armCi);
    }

    out[kIrqUpOutStatus] = 0;
    out[kIrqUpOutSource] = sourceIndex;
    out[kIrqUpOutEnabled] = 1;
    out[kIrqUpOutEqn] = fEqn;
    out[kIrqUpOutEqIntr] = 0;   /* what the EQ context was built with */
    LOG("m3irqup OK: event source %u enabled on a workloop; the EQ context's intr "
        "field is 0, so if this source is MSI-X table entry 0 the two agree. "
        "'m3irqtest' is the measurement that settles it.", (unsigned)sourceIndex);
    return kIOReturnSuccess;
}

/*
 * kCx3SelM3IrqTest -- register the token we want, post the event-mode command,
 * and return without waiting.
 *
 * The wait is deliberately not here.  The reviews flagged the self-deadlock risk:
 * if this ran on the workloop and waited for a completion that the same workloop
 * has to deliver, nothing would ever finish.  So the posting happens on the
 * caller's thread and the completion is collected later by "m3irqstatus".
 */
IOReturn Cx3Mlx4::exportM3IrqTest(const UInt64 *in, UInt64 *out)
{
    UInt32 token = (UInt32)in[kIrqTestInToken];
    UInt32 fwStatus = CX3_FWSTATUS_NOT_POSTED;
    UInt32 hcrPost = 0;

    memset(out, 0, sizeof(UInt64) * kIrqTestOutCount);
    out[kIrqTestOutFwStatus] = CX3_FWSTATUS_NOT_POSTED;

    /*
     * The token travels as 16 bits -- it is a u16 field in the EQE and cmdEvent
     * takes a UInt16 -- while the registration below would keep all 32.  A token
     * above 0xffff could therefore never match: the command completes, the EQE
     * carries the truncated value, and the test sits at "pending, never matched"
     * with nothing to say why.  Both source reviews flagged this as a trap; it is
     * refused rather than silently truncated.
     */
    if (token > 0xffffu) {
        out[kIrqTestOutStatus] = (UInt64)kIOReturnBadArgument;
        LOG("m3irqtest refused: token 0x%x does not fit the 16-bit field the HCA "
            "echoes back, so it could never match anything", (unsigned)token);
        return kIOReturnSuccess;
    }

    if (!fIrqEnabled) {
        LOG("m3irqtest refused: no interrupt delivery is enabled (objects %u, storm "
            "tripped %s)", (unsigned)fIrqObjectsUp,
            fIrqStormTripped ? "yes" : "no");
        out[kIrqTestOutStatus] = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }

    if (fEqLock) IOLockLock(fEqLock);
    fIrqPending    = true;
    fIrqWantToken  = token;
    fIrqMatched    = false;
    if (fEqLock) IOLockUnlock(fEqLock);

    /* in_modifier 0x1f, as mlx4_NOP sends it. */
    if (!fHw.cmdEvent(0, 0, 0x1f, 0, MLX4_CMD_NOP, (UInt16)token,
                      MLX4_CMD_TIME_CLASS_A, &fwStatus, &hcrPost, 0)) {
        out[kIrqTestOutFwStatus] = fwStatus;
        out[kIrqTestOutStatus] = (UInt64)kIOReturnError;
        LOG("m3irqtest: the command did not complete (fw status 0x%x)", (unsigned)fwStatus);
        return kIOReturnSuccess;
    }

    out[kIrqTestOutStatus] = 0;
    out[kIrqTestOutFwStatus] = fwStatus;
    out[kIrqTestOutToken] = token;
    out[kIrqTestOutHcrOnPost] = hcrPost;
    LOG("m3irqtest: event-mode NOP posted with token 0x%x (fw status 0x%x, cmd dword "
        "0x%08x); returning without waiting -- the completion is the interrupt's job",
        (unsigned)token, (unsigned)fwStatus, (unsigned)hcrPost);
    return kIOReturnSuccess;
}

/*
 * kCx3SelM3IrqStatus -- what the interrupt path has seen so far.
 *
 * Deliberately a separate call: it is what makes "did the interrupt deliver the
 * completion" answerable without the posting call having waited for it.
 */
IOReturn Cx3Mlx4::exportM3IrqStatus(UInt64 *out)
{
    memset(out, 0, sizeof(UInt64) * kIrqStatusOutCount);
    if (fEqLock) IOLockLock(fEqLock);
    out[kIrqStatusOutFlags] =
        (fIrqEnabled      ? CX3_IRQ_F_ENABLED   : 0) |
        (fIrqObjectsUp    ? CX3_IRQ_F_OBJECTS   : 0) |
        (fIrqStormTripped ? CX3_IRQ_F_STORM     : 0) |
        (fClrIntLegacy    ? CX3_IRQ_F_LEGACY    : 0) |
        (fClrIntReady     ? CX3_IRQ_F_CLR_READY : 0);
    out[kIrqStatusOutIrqCount]    = fIrqCount;
    out[kIrqStatusOutCallbackCount] = fIrqCallbackCount;
    out[kIrqStatusOutEqeCount]    = fIrqEqeCount;
    out[kIrqStatusOutPollEqeCount] = fPollEqeCount;
    out[kIrqStatusOutClrWrites]   = fClrIntWrites;
    out[kIrqStatusOutErrors]      = fIrqErrors;
    out[kIrqStatusOutSkipped]     = fIrqSkipped;
    out[kIrqStatusOutPending]     = fIrqPending ? 1 : 0;
    out[kIrqStatusOutWantToken]   = fIrqWantToken;
    out[kIrqStatusOutMatched]     = fIrqMatched ? 1 : 0;
    out[kIrqStatusOutMatchedToken] = fIrqMatchedToken;
    out[kIrqStatusOutMatchedStatus] = fIrqMatchedStatus;
    out[kIrqStatusOutLastType]    = fIrqLastType;
    out[kIrqStatusOutConsIndex]   = fEqConsIndex;
    out[kIrqStatusOutZeroStreak]  = fIrqZeroStreak;
    if (fEqLock) IOLockUnlock(fEqLock);

    LOG("m3irqstatus: flags=0x%llx irq=%llu cb=%llu eqe=%llu poll=%llu clrw=%llu "
        "errors=%llu skipped=%llu pending=%u matched=%u token=0x%x status=0x%x "
        "last_type=0x%02x cons=%u",
        (unsigned long long)out[kIrqStatusOutFlags],
        (unsigned long long)fIrqCount, (unsigned long long)fIrqCallbackCount,
        (unsigned long long)fIrqEqeCount, (unsigned long long)fPollEqeCount,
        (unsigned long long)fClrIntWrites, (unsigned long long)fIrqErrors,
        (unsigned long long)fIrqSkipped,
        (unsigned)out[kIrqStatusOutPending], (unsigned)out[kIrqStatusOutMatched],
        (unsigned)fIrqMatchedToken, (unsigned)fIrqMatchedStatus,
        (unsigned)fIrqLastType, (unsigned)fEqConsIndex);
    return kIOReturnSuccess;
}

/*
 * kCx3SelM3IrqDown -- close the session, in an order that leaves neither a callback
 * running against freed memory nor an asserting line with nothing listening.
 *
 * Hazard 1: a callback against freed memory.  Disable first (so no new notification is
 * scheduled), then remove the source from the workloop, which is the synchronising step:
 * the workloop will not run the action again once the source is gone.  Only then release.
 * That is stop()'s order and stop()'s reason.
 *
 * Hazard 2: a level-triggered interrupt left with no assistant.  This port's source is
 * legacy INTx -- getInterruptType reports 0x1, not PCIMessaged -- so the line stays
 * asserted until the event is consumed AND the clear register is written.  While a session
 * is open, irqAction() is that assistant.  This selector used to close the session by
 * removing the source and leaving the queue with the firmware ("the EQ itself is
 * untouched"), so from that moment until the end of the boot the card still owned an armed
 * queue and could assert a line that nothing would ever acknowledge.  That is the shape
 * docs/HANG-AUDIT.md section 10 is chasing, and it is why the reference's own teardown
 * sends HW2SW_EQ as its FIRST statement (mlx4_eq.c:1080, ahead of the memory being freed).
 * So the close is: acknowledge, drain, stop delivery, hand the queue back.
 *
 * This is a REVERSAL and is recorded as one: leaving the EQ alone was an earlier
 * deliberate decision ("tearing it down here would take the queue away from a caller who
 * only wanted to stop interrupts").  The cost of reversing is that a caller who wants the
 * EQ after closing the session must build it again -- m3equp, which every run reaches
 * through its own reset anyway.  The cost of not reversing it is the hazard above.
 */
IOReturn Cx3Mlx4::exportM3IrqDown(UInt64 *out)
{
    UInt32 after = 0, drained = 0;
    bool   blockedByComp = false;

    memset(out, 0, sizeof(UInt64) * kIrqDownOutCount);

    /*
     * No "was a session open" early return: after a storm trip the session flag is
     * false and the objects are still there, so returning early is exactly how they
     * leaked.  irqReleaseObjects() works off object existence.
     */
    if (fEqLock) IOLockLock(fEqLock);
    fIrqPending = false;

    /*
     * Acknowledge at the device, exactly as the handler's own first statement does
     * (mlx4_eq.c:841): the line must not still be up at the instant the assistant goes
     * away.  Only for a legacy source -- an MSI-X notification is edge-like and the
     * reference clears nothing on that path.
     */
    if (fClrIntLegacy && fHw.clrIntReady()) {
        if (fHw.clrIntWrite()) ++fClrIntWrites;
        else LOG("m3irqdown: the clear-register write failed, so the line may still be "
                 "asserted at the device; the queue is handed back below regardless");
    }

    /*
     * Drain what the handler would have drained.  An unconsumed EQE is itself an assert
     * condition, and this count is the reading that says whether the line was still up at
     * the moment the session ended -- the question section 10 cannot answer from the logs
     * it has.  It is added to the session's EQE count because the session did consume it.
     */
    if (fEqRing) {
        drained = consumeEqLocked(64, &after, &blockedByComp);
        fIrqEqeCount += drained;
    }

    out[kIrqDownOutIrqCount] = fIrqCount;
    out[kIrqDownOutEqeCount] = fIrqEqeCount;
    out[kIrqDownOutSkipped]  = fIrqSkipped;
    out[kIrqDownOutTripped]  = fIrqStormTripped ? 1 : 0;
    if (fEqLock) IOLockUnlock(fEqLock);

    if (!irqReleaseObjects("m3irqdown")) {
        out[kIrqDownOutStatus] = (UInt64)kIOReturnNotReady;
        LOG("m3irqdown: refusing HW2SW_EQ because the interrupt source was not "
            "confirmed removed from the workloop");
        return kIOReturnSuccess;
    }

    /*
     * And now take the QUEUE away from the firmware -- the queue, and nothing else.
     *
     * This is the step that stops the card asserting a line nobody would acknowledge, and it is
     * the reference's first teardown statement (mlx4_eq.c:1080, ahead of its memory being freed).
     *
     * What is deliberately NOT done here is the REST of teardownEq(): the host ring stays
     * allocated and fEqn stays set, because everything downstream of this call still names this
     * EQ.  The chain's own order is why: a completion queue's context carries comp_eqn, the R4
     * stages build that CQ after this one, and r4cq refuses outright when there is no EQ to name.
     * Measured, 2026-10-13 13:20 on 0.44.24: with the full teardown here, the chain ran all the
     * way through `interrupt session closed` (the step this change exists for -- and the machine
     * did NOT freeze, four runs after that step had killed it three times) and then stopped at
     * `r4cq refused: no EQ is up, so comp_eqn would name a queue that does not exist`.
     *
     * Keeping the ring is safe once the queue has been handed back: the firmware no longer owns
     * the EQ, so it cannot post into the ring or assert the line, and the memory stays inside
     * the freeze instruments' sweep.  It is released by the next reset (`teardownEq(..., true)`,
     * which the reset path already calls) or by stop().
     *
     * The order relative to delivery follows the same reasoning as before: delivery is stopped
     * first, so no new invocation can start; a hand-back that FAILS is reported as the finding
     * it is, because the card can then still own and arm the queue.
     */
    if (fEqn) {
        UInt32 hw2sw = CX3_FWSTATUS_NOT_POSTED;

        if (!fHw.eqHw2Sw(fEqn, &hw2sw)) {
            LOG("m3irqdown: HW2SW_EQ failed on EQN %u (fw status 0x%x) -- the card may still own "
                "and arm this queue, so a later event could assert the legacy line with nothing "
                "listening. The session's objects are released either way; this line is the "
                "finding, not a warning about one", (unsigned)fEqn, (unsigned)hw2sw);
        }
    } else {
        LOG("m3irqdown: no EQ number is held, so there is no queue to hand back. The session's "
            "objects are released; this says the session was never opened in this boot");
    }

    out[kIrqDownOutStatus] = 0;
    LOG("m3irqdown: session closed after %llu invocation(s), %llu EQE(s) consumed "
        "through interrupts%s and %llu through the poll selector; %u EQE(s) were still in "
        "the queue when it closed%s, and the EQ went back to the firmware",
        (unsigned long long)fIrqCount,
        (unsigned long long)fIrqEqeCount,
        drained ? " (including what this close drained)" : "",
        (unsigned long long)fPollEqeCount,
        (unsigned)drained,
        blockedByComp ? " (the head was a COMP event nothing consumes)" : "");
    return kIOReturnSuccess;
}

IOReturn Cx3Mlx4::newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                OSDictionary *properties, IOUserClient **handler)
{
    Cx3Mlx4UserClient *client;

    (void)type;
    (void)properties;

    userClientOpenMark(kCx3OpenPhaseEntered);
    client = new Cx3Mlx4UserClient;
    if (!client) {
        userClientOpenMark(kCx3OpenPhaseNoMemory);
        return kIOReturnNoMemory;
    }
    userClientOpenMark(kCx3OpenPhaseAllocated);

    if (!client->initWithTask(owningTask, securityID, type, properties)) {
        userClientOpenMark(kCx3OpenPhaseInitFailed);
        client->release();
        return kIOReturnNotPrivileged;
    }
    userClientOpenMark(kCx3OpenPhaseInitialized);
    if (!client->attach(this)) {
        userClientOpenMark(kCx3OpenPhaseAttachFailed);
        client->release();
        return kIOReturnError;
    }
    userClientOpenMark(kCx3OpenPhaseAttached);
    if (!client->start(this)) {
        userClientOpenMark(kCx3OpenPhaseStartFailed);
        client->detach(this);
        client->release();
        return kIOReturnError;
    }

    *handler = client;
    userClientOpenMark(kCx3OpenPhaseReturned);
    LOG("user client opened");
    return kIOReturnSuccess;
}

/*
 * CX3_START_LEVEL -- how much start() is allowed to do.
 *
 * Exists so that "is this driver the cause of the black screen?" can be answered
 * in one reboot instead of by argument, and so the answer is a fact rather than a
 * suspicion.  Each level is one build:
 *
 *   0  bind only: super::start, OSDynamicCast, registerService.  No PCI access,
 *      no BAR mapping, no device read at all.  If the black screen survives this,
 *      the problem is not what start() does -- it is the kext's presence or
 *      matching, and the whole approach has to change.
 *   1  + PCI enables (bus master, memory space)
 *   2  + BAR0/BAR2 mapping and the read-only register probe   [default, M1 state]
 *
 * Build with:  BUILD_TAG=... CX3_START_LEVEL=0 bash tools/cx3-mlx4/build.sh
 */
#ifndef CX3_START_LEVEL
#define CX3_START_LEVEL 2
#endif

bool Cx3Mlx4::start(IOService *provider)
{
    UInt16 vendor, device, subsysVendor, subsys, command;
    UInt32 classRev;

    /*
     * The boot-loop guard, before anything else -- including before super::start().
     *
     * This driver has hung a machine in start() before.  What made that expensive was not
     * the hang: it was that recovering meant booting into a different environment to remove
     * the kext, because there was no way to tell the driver "load, match, but do not start".
     * NullMoth's framebuffer driver has exactly this guard (it refuses to start unless
     * nvfb=1 is passed), and running it here costs one branch on a path that runs once.
     *
     * It returns before super::start() deliberately: the point is that NOTHING happens, so
     * there is no base-class work to reason about either.  The kext still loads and still
     * matches the device, which is what makes this recoverable -- the line below appears in
     * the boot log, so a machine that comes up with the card undriven can be told apart from
     * one where the kext never loaded.
     *
     * LOGBOOT rather than LOG because this runs while the logging subsystem may not be
     * delivering anything yet, and this is the one line that has to survive that.
     */
    if (cx3BootOff()) {
        LOGBOOT("start: cx3off=1 was passed, so this driver is NOT starting. No register, "
                "command or allocation has been touched. Remove cx3off (or set it to 0) to "
                "let it start; the kext stays loaded and matched either way.");
        return false;
    }

    /* start is for a fresh controller, not recovery of this instance. An
     * incomplete stop keeps its original ownership; even a clean stop leaves
     * generation/command/allocator history that Mlx4Hw::init does not reset.
     * Reject before superclass work or overwriting the existing PCI handle. */
    if (fStopRetained || fPciOpen || fDeviceGeneration != 0) {
        LOG("start: existing controller lifecycle cannot be restarted; use a fresh instance "
            "only after the previous owner has completed its normal stop");
        return false;
    }

    if (!super::start(provider)) return false;

    fPci = OSDynamicCast(IOPCIDevice, provider);
    if (!fPci) return false;

#if CX3_START_LEVEL == 0
    /*
     * Level 0: bind and stop, before even reading config space.
     *
     * This is the smallest thing that can still be called a driver, and it exists
     * to make one question answerable in a single reboot: is the black screen
     * caused by what start() does, or by the kext merely existing?  If level 0
     * still black-screens, the answer is "merely existing" and the approach has
     * to change rather than the code being trimmed further.
     */
    LOG("start level 0: binding only; no config read, no BAR, no device access");
    registerService();
    return true;
#endif

    vendor       = fPci->configRead16(kIOPCIConfigVendorID);
    device       = fPci->configRead16(kIOPCIConfigDeviceID);
    command      = fPci->configRead16(kIOPCIConfigCommand);
    classRev     = fPci->configRead32(kIOPCIConfigRevisionID);
    subsysVendor = fPci->configRead16(kIOPCIConfigSubSystemVendorID);
    subsys       = fPci->configRead16(kIOPCIConfigSubSystemID);

    LOG("=== Cx3Mlx4 start: %04x:%04x subsys %04x:%04x classrev %08x cmd %04x ===",
        (unsigned)vendor, (unsigned)device,
        (unsigned)subsysVendor, (unsigned)subsys,
        (unsigned)classRev, (unsigned)command);
    LOG("build tag: %s", CX3_BUILD_TAG);

    if (vendor != MLX4_PCI_VENDOR_MELLANOX || device != MLX4_PCI_DEVICE_CX3_PRO) {
        LOG("unexpected PCI id, refusing to claim the device");
        return false;
    }

    fStartPciIdentity.vendor = vendor;
    fStartPciIdentity.device = device;
    fStartPciIdentity.subsystemVendor = subsysVendor;
    fStartPciIdentity.subsystemId = subsys;
    fStartPciIdentity.bus = fPci->getBusNumber();
    fStartPciIdentity.deviceNumber = fPci->getDeviceNumber();
    fStartPciIdentity.function = fPci->getFunctionNumber();
    if (!fStartPciIdentity.valid()) {
        LOG("start: PCI identity/BDF is invalid; refusing an unbound provider");
        return false;
    }
    fStartPciIdentityValid = true;

    fPciOpen = fPci->open(this);
    LOG("provider open: %s", fPciOpen ? "OK" : "FAILED");
    if (!fPciOpen) {
        /* A previous incomplete stop can still own this provider. A fresh
         * controller is not permission to re-enable DMA or map that owner's
         * BARs. Respect every open refusal, without closing another client. */
        LOG("start: provider open refused; no PCI enable, BAR mapping or service publication");
        fStartPciIdentityValid = false;
        fStartPciIdentity.clear();
        fPci = 0;
        return false;
    }

    /*
     * The first boot of this kext read PCI COMMAND as 0x0000 and every BAR0
     * register as 0xffffffff: the BARs were assigned (IODeviceResourced = Yes,
     * IODeviceMemory = 0xdc200000/1MiB) but memory space decoding was off, so
     * the window floated high.  Every working PCI kext on this machine does the
     * same three enables in start() (SimpleMtkWlan.cpp:302-304,
     * AirportItlwm.cpp:355-357); this is the step that was missing here.
     */
    LOG("PCI command before enable: 0x%04x",
        (unsigned)fPci->configRead16(kIOPCIConfigCommand));
    bool busWas  = fPci->setBusMasterEnable(true);
    bool ioWas   = fPci->setIOEnable(true);
    bool memWas  = fPci->setMemoryEnable(true);
    LOG("PCI command after enable:  0x%04x (busmaster %s->enabled, io %s->enabled, "
        "memory %s->enabled)",
        (unsigned)fPci->configRead16(kIOPCIConfigCommand),
        busWas  ? "was-on" : "was-off",
        ioWas   ? "was-on" : "was-off",
        memWas  ? "was-on" : "was-off");
    fPciCommandAfterEnable = fPci->configRead16(kIOPCIConfigCommand);

    /*
     * requestPowerDomainState() used to be called here, on the theory that the
     * function might not be in D0.  It has been removed:
     *
     *   - the reference driver never does anything like it
     *   - it is not needed: if the function were not in D0, BAR0 would read
     *     0xffffffff, and the probe below shows live data
     *   - it always failed on this machine, so it was never doing anything for us
     *   - on a black-screen boot (2026-10-11) WindowServer reported the display
     *     invalid 17 s after this driver started, and a power-domain request is
     *     the one thing here that reaches beyond our own device.  Causation was
     *     never established, but a call that is unnecessary, that the reference
     *     does not make, and that always fails has no reason to stay.
     *
     * Anything that reaches outside this device does not belong in the boot path.
     */

    if (!fHw.init(fPci, this)) {
        LOG("hardware layer init failed (command lock or BAR mapping)");
        if (fPciOpen) { fPci->close(this); fPciOpen = false; }
        return false;
    }
    fDeviceGeneration = 1;
    fHw.setDeviceGeneration(fDeviceGeneration);
    for (UInt32 i = 0; i < 2; ++i) fPorts[i].newGeneration(fDeviceGeneration);

    fBarMapFlags = (fHw.bar0Mapped() ? 1u : 0u) | (fHw.bar2Mapped() ? 2u : 0u);
    readOnlyProbe();

    /*
     * Stop here.  start() performs no register writes and allocates no DMA:
     * this exact set of operations booted cleanly twice as build 0.2.0, and that
     * property is the whole point.  The command path is reached from user space
     * through Cx3Mlx4UserClient (selectors in cx3_abi.h) once the system is up.
     *
     * Reason, recorded so it is not undone: build 0.3.0 ran the command path
     * from here and hung the machine before it could flush a single log line.
     * The card also keeps that wedged state across warm resets, so the failure
     * repeated on the next several boots until the kext was removed.  A fault in
     * this function is a boot fault; a fault behind a selector is a reboot.
     */
    LOG("start: boot work complete (read-only). Device writes are available "
        "from user space only.");

    fNetAttachLock = IOLockAlloc();
    fNetAttachCall = thread_call_allocate(&Cx3Mlx4::netAttachThread, this);
    if (!fNetAttachLock || !fNetAttachCall) {
        LOG("start: the deferred attach context could not be allocated "
            "(lock=%s call=%s); netup will refuse instead of calling attachInterface "
            "inside the user-client transaction",
            fNetAttachLock ? "ok" : "NULL", fNetAttachCall ? "ok" : "NULL");
        if (fNetAttachCall) {
            thread_call_free(fNetAttachCall);
            fNetAttachCall = 0;
        }
        if (fNetAttachLock) {
            IOLockFree(fNetAttachLock);
            fNetAttachLock = 0;
        }
    } else {
        netAttachWorkerMark(kCx3AttachWorkerIdle);
        LOG("start: deferred attach context ready (thread_call, outside the user-client "
            "transaction)");
    }
    fNetDetachCall = thread_call_allocate(&Cx3Mlx4::netDetachThread, this);
    if (!fNetDetachCall) {
        LOG("start: synchronous network-detach worker unavailable; stop will retain resources "
            "if an interface is attached");
    }

    registerService();
    return true;
}

/* Normal core shutdown, not interface down/up. stop owns the selector,
 * family and callback barriers before entering these helpers. A command
 * completion is scoped to that operation; neither helper grants DMA free. */
bool Cx3Mlx4::closePortsForStop(const char *why)
{
    if (!fHcaInited) return true;
    if (!fNetTxAdmission.isClosed() || !fNetTxAdmission.drained() || fNetEnabled ||
        fResetCommandsBlocked || fHw.cmdWedged()) return false;
    const UInt32 ports = fDevCap.num_ports;
    if (ports == 0 || ports > 2) {
        LOG("core close (%s): invalid/unavailable CX3 port count %u; no command sent",
            why, (unsigned)ports);
        return false;
    }
    for (UInt32 port = 1; port <= ports; ++port) {
        if (fPorts[port - 1].closeConfirmed) continue;
        UInt32 fw = CX3_FWSTATUS_NOT_POSTED;
        const bool closed = fHw.closePort(port, &fw);
        fPorts[port - 1].closeFwStatus = fw;
        if (!closed || fw != 0) {
            LOG("core close (%s): CLOSE_PORT %u unconfirmed (fw 0x%x); "
                "child/table/FW owners remain reachable", why, (unsigned)port, (unsigned)fw);
            return false;
        }
        fPorts[port - 1].closeConfirmed = true;
        fPorts[port - 1].linkKnown = false; /* close receipt, not a new QUERY_PORT sample */
        fPorts[port - 1].linkUp = false;
        fPorts[port - 1].speedMbps = 0;
        fPorts[port - 1].initAccepted = false;
    }
    return true;
}

bool Cx3Mlx4::closeHcaForStop(const char *why)
{
    if (!fHcaInited) return true;
    if (!fNetTxAdmission.isClosed() || !fNetTxAdmission.drained() || fNetEnabled ||
        fResetCommandsBlocked || fHw.cmdWedged() ||
        hasContextOwners() || hasStopRetentionOwners()) return false;
    if (fDevCap.num_ports < 1 || fDevCap.num_ports > 2) return false;
    for (UInt32 port = 0; port < fDevCap.num_ports; ++port)
        if (!fPorts[port].closeConfirmed) return false;
    UInt32 fw = CX3_FWSTATUS_NOT_POSTED;
    if (!fHw.closeHca(&fw) || fw != 0) {
        LOG("core close (%s): CLOSE_HCA unconfirmed (fw 0x%x); "
            "initialized state and table/FW owners retained", why, (unsigned)fw);
        return false;
    }
    fHcaInited = false;
    /* fIcmHcaHolds describes firmware mappings, which CLOSE_HCA does not
     * unmap. Keep it until the individual table/AUX UNMAP transactions.
     * RUN_FW sticky and all retained-owner/poison state also stay intact. */
    LOG("core close (%s): CLOSE_HCA confirmed; table/AUX mappings and FW "
        "still require their separate revocation/retention decisions", why);
    return true;
}

/* Scope: only the FW area in this slot, after normal HCA/table shutdown.
 * FreeBSD close_fw unmaps this region before freeing it and before command
 * cleanup (mlx4_main.c:1913-1918,3884-3894). Check the result rather than copy
 * its ignored-error free. This does not revoke queue/EQ/reset-era DMA or
 * clear RUN_FW sticky state for another load on this controller. */
bool Cx3Mlx4::closeFwForStop(const char *why)
{
    if (!fFwIcm) return true;
    if (!fNetTxAdmission.isClosed() || !fNetTxAdmission.drained() || fNetEnabled ||
        fResetCommandsBlocked || fHw.cmdWedged() || fHcaInited ||
        fIcmMapped || fIcmHcaHolds || hasContextOwners() ||
        hasIcmTableAllocations() || hasStopRetentionOwners() ||
        fHw.hasAbRetainedRings()) return false;

    const Mlx4DmaGeneration &g = fFwIcm->generation;
    if (!fFwIcm->allocationId || !g.validAllocation() ||
        g.allocationGeneration != fDeviceGeneration ||
        (g.mayBeDeviceOwned ? !g.matchesOwnerGeneration(fDeviceGeneration)
                            : g.ownerGeneration != 0 || fFwRunAttempted)) {
        LOG("FW close (%s): unresolved FW lifecycle; keeping the exact allocation", why);
        return false;
    }
    if (g.mayBeDeviceOwned) {
        UInt32 fw = CX3_FWSTATUS_NOT_POSTED;
        if (!fHw.unmapFa(&fw) || fw != 0) {
            LOG("FW close (%s): UNMAP_FA unconfirmed (fw 0x%x); "
                "FW slot, mailbox, BAR and provider remain owned", why, (unsigned)fw);
            return false;
        }
    }
    /* The only release is the never-published or positively unmapped FW slot.
     * Keep the slot until the descriptor release completes. No generic
     * generation/poison or RUN_FW state is changed here. */
    fHw.icmFree(fFwIcm);
    fFwIcm = 0;
    return true;
}

void Cx3Mlx4::stop(IOService *provider)
{
    LOG("stop");
    if (fStopRetained) {
        LOG("stop: incomplete-stop retention is sticky; no cleanup retry or new reference");
        return;
    }
    /* Close TX admission before selector drain or asynchronous interface detach. */
    /* Native pointers may still be changed by an admitted selector. Their
     * gates are closed by stopNativePorts only after selector drain. While
     * waiting, selectorStopping invalidates their epoch and no DMA is freed. */
    const UInt32 txCallbacksAtClose = fNetTxAdmission.close();
    LOG("stop: network callback admission (TX + timer) closed with %u already admitted",
        (unsigned)txCallbacksAtClose);
    fNetEnabled = false;
    /*
     * Refuse new selectors and wait for the ones already inside BEFORE anything is torn
     * down.  The transaction lock already serialises selectors against each other; this
     * closes the other direction, which had no owner: stop() does not take that lock.
     */
    /*
     * A selector can legitimately be inside a command with a 60 s firmware budget.  Stopping
     * after the old 5 s turned a slow but healthy command into a race against every teardown
     * below.  The wait is still bounded; if it expires, nothing is torn down (see the early
     * return after the attach-worker check).
     */
    const bool selectorDrained = beginStopAndDrain(65000);
    /*
     * Stop the deferred attach worker before any device or framework state is
     * torn down.  thread_call_cancel() is deliberately used instead of
     * thread_call_cancel_wait(): the latter is not bindable from this AuxKC on
     * macOS 15.8.1.  A failure to free an active call is retained as a leak
     * rather than followed by freeing the lock the callback would use.
     */
    /* An admitted selector can still enqueue/use fNetAttachCall. Until all
     * selectors leave, even cancel/free of that call would race its owner. */
    bool attachWorkerQuiesced = selectorDrained;
    UInt32 retained = 0;
    if (selectorDrained && fNetAttachCall) {
        const bool cancelled = thread_call_cancel(fNetAttachCall);
        /* No selector can queue a new call, and this worker never rearms itself.
         * A successful cancel removes the sole pending invocation. Otherwise the
         * attach lock observes the worker's LAST local-resource access fence.
         * free success alone is not drainage, and thread_call_isactive is not
         * bindable from this AuxKC even though the kernel implements it. */
        attachWorkerQuiesced = false;
        if (fNetAttachLock) {
            IOLockLock(fNetAttachLock);
            attachWorkerQuiesced = cancelled || !fNetAttachInFlight;
            IOLockUnlock(fNetAttachLock);
        }
        if (attachWorkerQuiesced)
            attachWorkerQuiesced = thread_call_free(fNetAttachCall) ? true : false;
        if (attachWorkerQuiesced) {
            fNetAttachCall = 0;
            LOG("stop: deferred attach worker quiesced");
        } else {
            LOG("stop: deferred attach worker could not be freed; keeping its lock and "
                "object reference rather than freeing memory it could still touch");
        }
    }
    Mlx4StopDisposition disposition;
    disposition.init(selectorDrained, attachWorkerQuiesced);
    if (disposition.retainSelectorLock()) {
        LOG("stop: incomplete selector drain; locks and local dependency owners must survive");
    }
    if (!disposition.complete()) {
        /* Pin real objects, not just raw pointers, before super::stop can be
         * followed by detach and the framework dropping its references.
         * This is deliberately boot-long: a late worker release cannot remove
         * this separate owner. No proof exists to release it or retry teardown.
         * Retaining a provider does NOT prevent termination/power removal or
         * prove the networking family's internals survive super::stop. */
        retain();
        fStopRetained = true;
        fStopProvider = fPci ? fPci : provider;
        if (fStopProvider) fStopProvider->retain();
        netAttachWorkerMark(disposition.poisoned() ? kCx3AttachWorkerPoisoned
                                                   : kCx3AttachWorkerIdle);
        LOG("stop: INCOMPLETE (selectorDrained=%u workerQuiesced=%u); self/provider pinned, "
            "no local dependency cleanup. This is not DMA or framework quiescence proof",
            (unsigned)selectorDrained, (unsigned)attachWorkerQuiesced);
        super::stop(provider);
        return;
    }
    if (!stopNativePorts()) {
        retain(); fStopRetained=true; fStopProvider=fPci ? fPci:provider;
        if(fStopProvider)fStopProvider->retain();
        LOG("stop: native child drain/detach incomplete; retaining all PF dependencies");
        super::stop(provider); return;
    }
    if (fHw.hasAbRetainedRings() || hasStopRetentionOwners()) {
        /* The A/B diagnostic owns an ICM ring whose HW2SW or exact MTT release
         * failed. Keep the Mlx4Hw ledger, ring descriptor, provider and every
         * dependent teardown object reachable until reboot; generic stop must
         * not clear the MTT/table view and then lose the only retry owner. */
        retain();
        fStopRetained = true;
        fStopProvider = fPci ? fPci : provider;
        if (fStopProvider) fStopProvider->retain();
        LOG("stop: INCOMPLETE (A/B ring or retained ledger/mailbox owner remains); "
            "self/provider pinned and no device/table/ICM teardown attempted");
        super::stop(provider);
        return;
    }
    if (disposition.settleAttach() && fNetAttachLock) {
        IOLockLock(fNetAttachLock);
        retained = fNetAttachRetainCount;
        fNetAttachRetainCount = 0;
        IOLockUnlock(fNetAttachLock);
    }
    while (retained--) release();
    netAttachWorkerMark(disposition.poisoned() ? kCx3AttachWorkerPoisoned
                                               : kCx3AttachWorkerIdle);
    /*
     * Move both an ordinary attached interface and any late orphan from the attach worker to
     * a dedicated thread_call. It invokes detachInterface(..., true) off the selector/workloop
     * context, giving us the family's synchronous termination barrier without making this
     * stop() thread unbounded. On timeout, ownership and the entire teardown transaction stay
     * pinned; the worker may still complete later but cannot trigger a cleanup retry.
     */
    if (disposition.settleAttach() && !netDetachInterfacesAndWait(2000u)) {
        retain();
        fStopRetained = true;
        fStopProvider = fPci ? fPci : provider;
        if (fStopProvider) fStopProvider->retain();
        LOG("stop: INCOMPLETE (synchronous interface detach did not finish/free its worker "
            "within 2000 ms); self/provider pinned and all TX/device resources retained");
        super::stop(provider);
        return;
    }
    /*
     * The callback count and close bit are one CAS-protected state. A lock holder cannot be
     * missing from this count, including one preempted before trying fNetTxLock. If it does
     * not drain, keep every resource alive: later QP/CQ/table teardown would race the callback.
     */
    {
        UInt32 waitedMs = 0;
        while (!fNetTxAdmission.drained() && waitedMs < 2000u) {
            IOSleep(1);
            ++waitedMs;
        }
    }
    if (!fNetTxAdmission.drained()) {
        retain();
        fStopRetained = true;
        fStopProvider = fPci ? fPci : provider;
        if (fStopProvider) fStopProvider->retain();
        LOG("stop: INCOMPLETE (network callback admission did not drain within 2000 ms; "
            "active=%u at close=%u); self/provider pinned and all TX/device resources retained",
            (unsigned)fNetTxAdmission.active(), (unsigned)txCallbacksAtClose);
        super::stop(provider);
        return;
    }
    /* Stop delivery before any network, queue, table, or BAR dependency is released. */
    if (!irqReleaseObjects("driver stopping")) {
        retain();
        fStopRetained = true;
        fStopProvider = fPci ? fPci : provider;
        if (fStopProvider) fStopProvider->retain();
        LOG("stop: INCOMPLETE (interrupt source removal was not confirmed); self/provider "
            "pinned and all network/EQ/QP/CQ/table/BAR resources retained");
        super::stop(provider);
        return;
    }
    /*
     * The admitted callback set is empty before timer, mbuf, ring, and lock teardown begins.
     * This is CPU callback drainage, not proof that device DMA or an unentered framework callback
     * is quiescent; those remain separate release gates.
     */
    if (!netServiceStop("driver stopping") || hasStopRetentionOwners()) {
        retain();
        fStopRetained = true;
        fStopProvider = fPci ? fPci : provider;
        if (fStopProvider) fStopProvider->retain();
        LOG("stop: INCOMPLETE (network stop failed or frame-ring retention remains); "
            "self/provider pinned before queue/table/FW teardown");
        super::stop(provider);
        return;
    }
    if (!closePortsForStop("driver stopping")) {
        retain();
        fStopRetained = true;
        fStopProvider = fPci ? fPci : provider;
        if (fStopProvider) fStopProvider->retain();
        LOG("stop: INCOMPLETE (normal port close unconfirmed); "
            "self/provider pinned before child/table/FW teardown");
        super::stop(provider);
        return;
    }
    /*
     * Give the EQ back before the tables it depends on.
     *
     * This order used to be the other way round -- the 14 context tables were
     * released first and only then the EQ -- which is backwards: the EQ context
     * names an MTT entry list inside the MTT table, so freeing that table while the
     * firmware still held the queue left it pointing into recycled memory.
     */
    teardownEq("driver stopping", false);
    /*
     * The QP before everything it names, and in particular before the ring.
     *
     * A QP's context holds the MTT address of the receive ring and the DMA address of the
     * producer record, and there is no single command that hands it back -- it is taken away
     * by moving it to RST.  Until that happens, releasing either piece of memory is exactly
     * the mistake the EQ, CQ and MR teardowns all exist to avoid.  rqTeardown enforces this
     * from the other side: while this flag is still set, it refuses to release the ring.
     */
    ingressTeardown("driver stopping", false);
    ingressTeardown("driver stopping", false, 2);
    txTeardown("driver stopping", false);
    txTeardown("driver stopping", false, 2);
    qpTeardown("driver stopping", false);
    qpTeardown("driver stopping", false, 2);
    /*
     * The MR has to go back before the dMPT table it lives in, and before any queue that
     * could name its key -- which is why it is here, next to teardownEq, rather than
     * anywhere the tables are released.  Both reviews of stage 1 flagged that the
     * previous order released the tables with an entry the hardware still owned.
     */
    mrTeardown("driver stopping", false);
    /* And the CQ before the MTT table its context names. */
    cqTeardown("driver stopping", false);
    cqTeardown("driver stopping", false, 2);
    rqTeardown("driver stopping", false);
    rqTeardown("driver stopping", false, 2);
    if (hasContextOwners() || hasStopRetentionOwners()) {
        /* Child teardown can refuse HW2SW/2RST or exact MTT release. Keeping
         * its ring alone is insufficient if shutdown drops the only provider,
         * table view and retry owner. Preserve this incomplete transaction. */
        retain();
        fStopRetained = true;
        fStopProvider = fPci ? fPci : provider;
        if (fStopProvider) fStopProvider->retain();
        LOG("stop: INCOMPLETE (queue/context or retention owner remains after child teardown); "
            "self/provider pinned before table/FW/BAR cleanup");
        super::stop(provider);
        return;
    }
    if (!closeHcaForStop("driver stopping")) {
        retain();
        fStopRetained = true;
        fStopProvider = fPci ? fPci : provider;
        if (fStopProvider) fStopProvider->retain();
        LOG("stop: INCOMPLETE (normal HCA close unconfirmed); "
            "self/provider pinned before table/FW/BAR teardown");
        super::stop(provider);
        return;
    }
    if (fNumTables || fAuxIcm) freeIcmTables();
    if (hasIcmTableAllocations() || hasStopRetentionOwners()) {
        /* A failed retention transfer leaves the original table/AUX slot
         * owned in place. Profile counts do not prove those slots are empty. */
        retain();
        fStopRetained = true;
        fStopProvider = fPci ? fPci : provider;
        if (fStopProvider) fStopProvider->retain();
        LOG("stop: INCOMPLETE (ICM table/AUX or retention owner remains); "
            "self/provider pinned before FW/BAR cleanup");
        super::stop(provider);
        return;
    }
    /* The FW region has its own normal UNMAP_FA contract, after HCA/core
     * table shutdown and before releasing command or provider dependencies. */
    if (!closeFwForStop("driver stopping")) {
        retain();
        fStopRetained = true;
        fStopProvider = fPci ? fPci : provider;
        if (fStopProvider) fStopProvider->retain();
        LOG("stop: INCOMPLETE (normal FW close unconfirmed); "
            "original owner and self/provider pinned before BAR cleanup");
        super::stop(provider);
        return;
    }
    if (hasStopRetentionOwners()) {
        /* A successful transfer clears the original slot, not its ownership
         * obligation. These ledgers and Mlx4Hw are members of this controller;
         * keeping only their allocations would orphan the owning pointers on
         * destruction. Pin the ledger container and provider before shutdown.
         * This is boot-long local ownership, not DMA quiescence or prevention
         * of provider termination/power removal, and does not permit reopen. */
        retain();
        fStopRetained = true;
        fStopProvider = fPci ? fPci : provider;
        if (fStopProvider) fStopProvider->retain();
        LOG("stop: INCOMPLETE (quarantine/ICM-kept/mailbox retention owner remains); "
            "self/provider pinned before ledger/BAR/provider teardown");
        super::stop(provider);
        return;
    }
    /*
     * No device-quiesce proof exists on this path: the driver is going away, which is not
     * the same statement as "the device stopped".  Retained blocks therefore stay allocated
     * and are reclaimed by a reboot; the function logs what it kept.
     */
    quarantineDrain("driver stopping", kCx3QuiesceStopping);
    /*
     * The media dictionary and its one medium object.  publishMediumDictionary() COPIES the
     * dictionary into the driver's property table (IONetworkController.h:1023-1050), so the
     * family's copy outlives our release -- and the medium object is ours to release because
     * IONetworkMedium::medium() returns it with a reference.  Released here rather than in
     * free(), so a kext unload cannot leave the family holding a pointer into this driver's
     * memory.
     */
    if (fMediaDict) { fMediaDict->release(); fMediaDict = 0; }
    if (fMediumAuto) { fMediumAuto->release(); fMediumAuto = 0; }
    fMediaPublished = false;
    fHw.setMptTable(0, 0, 0);       /* the view is gone with the table */
    /* Admission remains closed. Both host locks live until free(), so a late
     * rejected call cannot access a lock freed by this stop phase. */

    /*
     * LAST-RESORT DMA STOP.
     *
     * Every command has been sent by now (the teardowns above need the mailbox), so this is
     * the last moment at which the device can be told to stop issuing new bus-master
     * requests.  Clearing the PCI command register's bus-master bit takes effect in the
     * bridge/root complex, so it works even when the device's own firmware is wedged -- the
     * case this driver has actually met.
     *
     * What it is NOT: a flush.  Transactions already in flight may still complete, and the
     * device may already have written into pages this driver no longer owns.  It therefore
     * does not authorise freeing anything; the quarantine / fIcmKept rules stand on their
     * own.  start() enables bus mastering again, so a reload is not poisoned by this.
     */
    if (fPci) {
        bool wasEnabled = fPci->setBusMasterEnable(false);
        UInt16 after = fPci->configRead16(kIOPCIConfigCommand);
        LOG("stop: PCI bus mastering disabled (was %s); command register now 0x%04x. This "
            "blocks NEW DMA requests only -- it is not a flush and not a quiescence proof, "
            "so retained memory stays retained.",
            wasEnabled ? "enabled" : "already disabled", (unsigned)after);
    } else {
        LOG("stop: no PCI device handle, so bus mastering could NOT be disabled; treat any "
            "retained device-facing memory as still reachable until reboot");
    }
    fHw.shutdown();
    if (fPciOpen && fPci) {
        fPci->close(this);
        fPciOpen = false;
    }
    fPci = 0;
    super::stop(provider);
    if (disposition.settleAttach() && fNetAttachLock) {
        IOLockFree(fNetAttachLock);
        fNetAttachLock = 0;
    }
    /*
     * The workloop LAST, and after the family's stop: it is the one getWorkLoop() handed the
     * networking family, so the family's own teardown may still schedule on it.  Releasing it
     * before super::stop would be the same shape as releasing it at m3irqdown, one step later.
     */
    if (fWorkLoop) {
        fWorkLoop->release();
        fWorkLoop = 0;
    }
}

/* ==================================================================== */
/* M2d: profile, context-memory tables, INIT_HCA                        */
/* ==================================================================== */

/*
 * kCx3SelM2dProfile -- decode the capability set and pack the profile.
 *
 * Everything is kept in the provider, because the bring-up step needs the same
 * numbers and re-deriving them would be a second place to get the arithmetic
 * wrong.  The profile knobs come from the caller: this driver does not need the
 * reference's build-an-IB-stack profile, and a smaller profile means fewer ICM
 * pages to allocate and map.
 */
IOReturn Cx3Mlx4::exportM2dProfile(const UInt64 *in, Cx3ProfileResult *res,
                                   UInt64 *scalars)
{
    UInt32 tablesAsked;
    Mlx4Profile requestedProfile;

    memset(res, 0, sizeof(*res));
    res->status   = (UInt64)kIOReturnError;
    res->fwStatus = 0xffffffffull;
    if (scalars) { scalars[kProfOutStatus] = res->status;
                   scalars[kProfOutFwStatus] = res->fwStatus; }

    /* Repacking fTables is not a live reconfiguration transaction. Refuse before
     * changing fProfile, issuing commands or attempting teardown: any failed
     * child return would otherwise leave pointers that makeProfile overwrites. */
    if (fQuarExhausted || fHcaInited || fNetEnabled || hasContextOwners()) {
        LOG("m2dprofile refused: initialized HCA, enabled network or context owners remain; "
            "the old profile and table allocations are unchanged");
        res->status = (UInt64)kIOReturnNotReady;
        if (scalars) scalars[kProfOutStatus] = res->status;
        return kIOReturnSuccess;
    }

    if (!fHw.windowAlive()) {
        LOG("m2dprofile refused: the BAR0 window is not reading back live");
        res->status = (UInt64)kIOReturnOffline;
        return kIOReturnSuccess;
    }

    memset(&requestedProfile, 0, sizeof(requestedProfile));
    requestedProfile.num_qp       = (UInt32)in[kM2dInNumQp];
    requestedProfile.rdmarc_per_qp = (UInt32)in[kM2dInRdmarcPerQp];
    requestedProfile.num_srq      = (UInt32)in[kM2dInNumSrq];
    requestedProfile.num_cq       = (UInt32)in[kM2dInNumCq];
    requestedProfile.num_mcg      = (UInt32)in[kM2dInNumMcg];
    requestedProfile.num_mpt      = (UInt32)in[kM2dInNumMpt];
    requestedProfile.num_mtt      = (UInt32)in[kM2dInNumMtt];
    if (requestedProfile.num_qp == 0 || requestedProfile.num_mtt == 0) {
        LOG("m2dprofile refused: a profile needs at least QPs and MTT segments");
        res->status = (UInt64)kIOReturnBadArgument;
        return kIOReturnSuccess;
    }

    if (!fHw.parseDevCap(&fDevCap)) {
        LOG("m2dprofile: QUERY_DEV_CAP failed");
        res->status = (UInt64)kIOReturnError;
        return kIOReturnSuccess;
    }

    /*
     * Everything that references the OLD tables is handed back before those tables are
     * unmapped, and only then is the new layout packed.
     *
     * The order used to be the other way round: freeIcmTables() ran at the top of this
     * function and only MR/ingress/TX were torn down afterwards.  An HCA still running a QP,
     * CQ, RQ or EQ whose context points into an MTT/EQC/CQC table that has just been
     * unmapped is exactly the shape all the other teardowns exist to avoid -- and the RX QP,
     * CQ and RQ were not torn down on this path at all.  This is the same dependency order
     * stop() uses, minus the parts that belong to the interface and the timer.
     */
    if (!irqReleaseObjects("a new profile replaces the tables")) {
        LOG("m2dprofile refused: interrupt source removal was not confirmed; old profile "
            "and its dependent queue/table owners remain unchanged");
        res->status = (UInt64)kIOReturnNotReady;
        if (scalars) scalars[kProfOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    fProfile = requestedProfile;
    teardownEq("a new profile replaces the tables", false);
    ingressTeardown("a new profile replaces the tables", false);
    ingressTeardown("a new profile replaces the tables", false, 2);
    txTeardown("a new profile replaces the tables", false);
    txTeardown("a new profile replaces the tables", false, 2);
    qpTeardown("a new profile replaces the tables", false);
    qpTeardown("a new profile replaces the tables", false, 2);
    mrTeardown("a new profile replaces the tables", false);
    cqTeardown("a new profile replaces the tables", false);
    cqTeardown("a new profile replaces the tables", false, 2);
    rqTeardown("a new profile replaces the tables", false);
    rqTeardown("a new profile replaces the tables", false, 2);
    /* Nothing may still name the old pages: now the page tables can go. */
    freeIcmTables();
    if (fQuarExhausted) {
        LOG("m2dprofile refused: retention exhausted while dismantling the previous "
            "tables; surviving table/AUX pointers are not overwritten by a new layout");
        res->status = (UInt64)kIOReturnNotReady;
        if (scalars) scalars[kProfOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    fIcmMapped = false;

    /*
     * Pack the NEW layout only after the old allocations have been released: makeProfile()
     * overwrites fTables[], and freeIcmTables() reads that same array to unmap and free the
     * old chunks.  Running it earlier would lose the old pointers and leak mapped pages while
     * telling the firmware to forget them.
     *
     * tablesAsked is a pure out parameter now; the capacity is passed separately so the two
     * cannot be confused (see the note on makeProfile).
     */
    fIcmSize = fHw.makeProfile(&fProfile, &fDevCap, &fInitHca, fTables,
                               kMaxIcmTables, &tablesAsked);
    fNumTables = tablesAsked;
    if (fIcmSize == 0) {
        LOG("m2dprofile: the profile does not fit, or %u tables exceed the %u "
            "the array holds; the previous profile has already been quiesced and released",
            (unsigned)tablesAsked, (unsigned)kMaxIcmTables);
        fNumTables = 0;
        res->status = (UInt64)kIOReturnNoSpace;
        return kIOReturnSuccess;
    }

    /*
     * The UAR size and the ICMP/UAR fields INIT_HCA needs are derived here, not
     * in the profile: they come from the capability set, not from the packing
     * (mlx4_main.c:2272-2276).
     */
    {
        UInt32 numUars = fDevCap.uar_size / 4096;
        UInt32 l = 0;
        while ((1u << l) < numUars) ++l;
        fInitHca.log_uar_sz = (UInt8)l;      /* ilog2(num_uars) */
    }
    fInitHca.uar_page_sz = (UInt8)0;         /* PAGE_SHIFT - 12, i.e. 0 at 4 KiB */

    res->icmSize       = fIcmSize;
    res->maxIcmSz      = fDevCap.max_icm_sz;
    res->qpcBase       = fInitHca.qpc_base;
    res->srqcBase      = fInitHca.srqc_base;
    res->cqcBase       = fInitHca.cqc_base;
    res->eqcBase       = fInitHca.eqc_base;
    res->altcBase      = fInitHca.altc_base;
    res->auxcBase      = fInitHca.auxc_base;
    res->rdmarcBase    = fInitHca.rdmarc_base;
    res->dmptBase      = fInitHca.dmpt_base;
    res->cmptBase      = fInitHca.cmpt_base;
    res->mttBase       = fInitHca.mtt_base;
    res->mcBase        = fInitHca.mc_base;
    res->logNumQps     = fInitHca.log_num_qps;
    res->logNumSrqs    = fInitHca.log_num_srqs;
    res->logNumCqs     = fInitHca.log_num_cqs;
    res->logNumEqs     = fInitHca.log_num_eqs;
    res->logRdPerQp    = fInitHca.log_rd_per_qp;
    res->logMptSz      = fInitHca.log_mpt_sz;
    res->logMcEntrySz  = fInitHca.log_mc_entry_sz;
    res->logMcTableSz  = fInitHca.log_mc_table_sz;
    res->logMcHashSz   = fInitHca.log_mc_hash_sz;
    res->numSysEqs     = fInitHca.num_sys_eqs;
    res->logUarSz      = fInitHca.log_uar_sz;
    res->uarPageSz     = fInitHca.uar_page_sz;
    res->mwEnabled     = fInitHca.mw_enabled;
    res->numTables     = fNumTables;
    res->qpcEntrySz    = fDevCap.qpc_entry_sz;
    res->cqcEntrySz    = fDevCap.cqc_entry_sz;
    res->eqcEntrySz    = fDevCap.eqc_entry_sz;
    res->srqEntrySz    = fDevCap.srq_entry_sz;
    res->dmptEntrySz   = fDevCap.dmpt_entry_sz;
    res->cmptEntrySz   = fDevCap.cmpt_entry_sz;
    res->mttEntrySz    = fDevCap.mtt_entry_sz;
    res->rdmarcEntrySz = fDevCap.rdmarc_entry_sz;
    res->auxEntrySz    = fDevCap.aux_entry_sz;
    res->altcEntrySz   = fDevCap.altc_entry_sz;
    res->reservedQps   = fDevCap.reserved_qps;
    res->reservedCqs   = fDevCap.reserved_cqs;
    res->reservedSrqs  = fDevCap.reserved_srqs;
    res->reservedEqs   = fDevCap.reserved_eqs;
    res->reservedMtts  = fDevCap.reserved_mtts;
    res->reservedMrws  = fDevCap.reserved_mrws;
    res->numPorts      = fDevCap.num_ports;
    res->flags2        = fDevCap.flags2;

    res->status   = 0;
    res->fwStatus = 0;
    if (scalars) { scalars[kProfOutStatus] = 0; scalars[kProfOutFwStatus] = 0; }
    LOG("m2dprofile OK: icm_size %llu KiB, %u tables, %u ports, %u QPs, %u CQs",
        (unsigned long long)(fIcmSize >> 10), (unsigned)fNumTables,
        (unsigned)fDevCap.num_ports, (unsigned)(1u << fInitHca.log_num_qps),
        (unsigned)(1u << fInitHca.log_num_cqs));
    return kIOReturnSuccess;
}

/*
 * One table, mapped the way mlx4_init_icm_table does it (mlx4_icm.c:387):
 *
 *   obj_per_chunk = CHUNK / obj_size
 *   chunks        = ceil(reserved * obj_size / CHUNK)
 *   for each chunk: allocate it, then MAP_ICM it at virt + i * CHUNK
 *
 * Only the reserved prefix is mapped; the reference maps the rest lazily as
 * objects are allocated, so this is not a simplification.  `fatal` is set when
 * the failure is a command failure rather than a refusal, because after a failed
 * command the command register may be unusable for the rest of the sequence.
 */
bool Cx3Mlx4::mapIcmTable(Mlx4IcmTable *t, bool *fatal)
{
    UInt32 reservedBytes, chunks, i;
    UInt32 fwStatus = 0;

    if (fatal) *fatal = false;
    if (!t) return false;

    /* Objects cannot be smaller than nothing, and a zero-sized table has
     * nothing to map. */
    if (t->objSize == 0 || t->nObj == 0) {
        LOG("icm table %s: nothing to map (obj size %u, %u objects)",
            t->name, (unsigned)t->objSize, (unsigned)t->nObj);
        return true;
    }

    reservedBytes = t->reserved * t->objSize;
    chunks = (reservedBytes + MLX4_TABLE_CHUNK_SIZE - 1) / MLX4_TABLE_CHUNK_SIZE;
    if (chunks == 0) {
        LOG("icm table %s: nothing reserved, skipping", t->name);
        return true;
    }

    /* The allocator takes whole pages, so the chunk size is rounded up.  The
     * reference allocates CHUNK bytes per chunk unconditionally
     * (mlx4_icm.c:262-268), which is what this does too -- the last chunk is
     * rounded to a page rather than shortened, because the object count is a
     * power of two times the entry size and so already page-aligned. */
    for (i = 0; i < chunks; ++i) {
        UInt32 bytes;

        if (t->icm[i]) continue;    /* already mapped (repeat call) */
        bytes = MLX4_TABLE_CHUNK_SIZE;
        if ((UInt64)(i + 1) * MLX4_TABLE_CHUNK_SIZE > (UInt64)t->nObj * t->objSize)
            bytes = (UInt32)(((UInt64)t->nObj * t->objSize) - (UInt64)i * MLX4_TABLE_CHUNK_SIZE);
        bytes = (bytes + 4095u) & ~4095u;
        if (bytes == 0) bytes = MLX4_TABLE_CHUNK_SIZE;

        t->icm[i] = fHw.icmAlloc(bytes >> 12, t->lowmem);
        if (!t->icm[i]) {
            LOG("icm table %s: chunk %u of %u (%u bytes) could not be allocated",
                t->name, (unsigned)i, (unsigned)chunks, (unsigned)bytes);
            return false;
        }
        {
        UInt32 entries = 0;
        if (!fHw.mapIcm(t->icm[i], t->virt + (UInt64)i * MLX4_TABLE_CHUNK_SIZE,
                        &entries, &fwStatus)) {
            LOG("icm table %s: MAP_ICM of chunk %u (virt 0x%llx, %u pages) failed "
                "(fw status 0x%x)", t->name, (unsigned)i,
                (unsigned long long)(t->virt + (UInt64)i * MLX4_TABLE_CHUNK_SIZE),
                (unsigned)entries, (unsigned)fwStatus);
            /*
             * Release it, as the reference does (mlx4_icm.c:423-426 frees and
             * nulls the slot on MAP failure).  Keeping the pointer would make a
             * later attempt treat an unmapped chunk as already mapped, because
             * the reuse test is "is this slot non-zero".
             */
            fHw.icmFree(t->icm[i]);
            t->icm[i] = 0;
            if (fatal) *fatal = true;
            return false;
        }
        }
        LOG("icm table %-8s: chunk %u/%u mapped at virt 0x%llx (%u pages, bus 0x%llx)",
            t->name, (unsigned)(i + 1), (unsigned)chunks,
            (unsigned long long)(t->virt + (UInt64)i * MLX4_TABLE_CHUNK_SIZE),
            (unsigned)(bytes >> 12), (unsigned long long)t->icm[i]->pages[0].busAddr);
        /*
         * The firmware has been told about this chunk, so a later teardown may ask it
         * to unmap this range.  Set here rather than after INIT_HCA, because the
         * mapping exists from the moment MAP_ICM is accepted, and a bring-up that
         * fails halfway still leaves this chunk to be given back.
         */
        fIcmHcaHolds = true;
    }
    return true;
}

/*
 * mlx4_init_icm() -- FreeBSD mlx4_main.c:1563, in order, stopping at the first
 * failure so that the step which failed is the step that is reported.
 *
 * The MTT table needs one adjustment the reference makes just before mapping:
 * the reserved MTT entries are rounded up to a cache line because the firmware
 * writes to them (mlx4_main.c:1612-1620).
 */
void Cx3Mlx4::initIcmTables(UInt64 *out, UInt32 *outTablesDone)
{
    UInt32 i, done = 0;

    if (outTablesDone) *outTablesDone = 0;
    for (i = 0; i < fNumTables; ++i) {
        Mlx4IcmTable *t = &fTables[i];
        bool fatal = false;
        UInt32 chunks;

        if (strcmp(t->name, "MTT") == 0) {
            UInt32 align = 64;      /* dma_get_cache_alignment() on this machine */
            UInt64 bytes = (UInt64)t->reserved * t->objSize;
            bytes = (bytes + align - 1) & ~((UInt64)align - 1);
            t->reserved = (UInt32)(bytes / t->objSize);
            LOG("icm table MTT: reserved rounded up to a cache line: %u objects",
                (unsigned)t->reserved);
        }

        chunks = (t->reserved * t->objSize + MLX4_TABLE_CHUNK_SIZE - 1) /
                 MLX4_TABLE_CHUNK_SIZE;
        if (chunks == 0) { ++done; continue; }
        /* Bounded by the array itself, not by a second copy of its size, so the
         * two cannot disagree. */
        if (chunks > (UInt32)(sizeof(t->icm) / sizeof(t->icm[0]))) {
            LOG("icm table %s: %u chunks exceeds the %u-chunk array",
                t->name, (unsigned)chunks,
                (unsigned)(sizeof(t->icm) / sizeof(t->icm[0])));
            break;
        }

        if (!mapIcmTable(t, &fatal)) {
            LOG("icm tables: stopped at table %u (%s) after %u mapped",
                (unsigned)i, t->name, (unsigned)done);
            break;
        }
        ++done;
        if (fatal) break;
    }
    if (outTablesDone) *outTablesDone = done;
    if (out) out[kBringUpOutTablesDone] = done;
}

/*
 * Release everything the ICM bring-up mapped: UNMAP_ICM each chunk, then the
 * auxiliary area.
 *
 * Two reasons this is not just "free the memory":
 *   - the HCA's page tables still point at these pages.  Freeing host memory the
 *     firmware still references is the failure mode the reference avoids by
 *     unmapping first (mlx4_icm.c:305-320, mlx4_main.c:1752-1762), and it is what
 *     made an earlier problem in this project look like "attempt 2 fails
 *     differently from attempt 1".
 *   - a re-profile produces different table bases, so the old mappings must be
 *     gone before the new layout is announced, or the HCA ends up holding both.
 *
 * The rule this function did not follow before 0.43.0: it called unmapIcm(),
 * dropped the return value, freed the memory anyway, and then logged "the HCA's
 * page tables no longer reference them" -- an assertion about the device's internal
 * state that nothing in this driver had checked.  Measured on 2026-10-12, fourteen
 * consecutive UNMAP_ICM commands answered fw status 0x1, which the reference's own
 * enum defines as CMD_STAT_INTERNAL_ERR, "internal error (such as a bus error)
 * occurred while processing command" (mlx4_cmd.c:63-65), and the memory went back
 * to the kernel regardless.
 *
 * So the unmap decides, and the log says what was done:
 *
 *   - the firmware is asked to unmap only when this firmware instance was told
 *     about the mapping (fIcmHcaHolds).  After an HCA reset it was not (the reset
 *     restarts the firmware), and asking anyway is what produced the 0x1s;
 *   - a chunk whose unmap *was* asked for and refused is KEPT: the firmware may
 *     still be translating through it, so it is not handed back to the kernel.
 *     The cost is a deliberate leak -- those pages, and the descriptor with them,
 *     do not return to the kernel before the next reboot -- and that is the smaller
 *     cost: the alternative is host memory the HCA can still write into, which is
 *     the failure this project has lost a machine to.  The total is kept in
 *     fLostIcmPages and every instance is logged with its fw status.  Expected
 *     value zero, and zero is what the reset-then-reprofile chain should produce;
 *   - a chunk the firmware did confirm is freed, and is named as freed because of
 *     the confirmation rather than because the command was sent.
 *
 * The slot is cleared in either case, the kept one included, because the reuse test
 * in mapIcmTable is "is this slot non-zero": leaving a kept chunk in its slot would
 * make the next bring-up hand the HCA a base whose mapping is precisely the thing
 * that could not be established.  The memory is still kept; only the record of it
 * moves to fLostIcmPages.
 *
 * Tradeoff, stated plainly: keeping the memory is not free (a repeated failure
 * leaks more each time, and the auxiliary area alone is 2082 pages for this card's
 * profile), and freeing it is not evidently safe.  This chooses the leak, because
 * the failure is a statement about the firmware's mapping and this driver has no
 * observation that could show the pages are unused.  docs/ICM-UNMAP.md.
 */
/*
 * Keep an ICM chunk the firmware refused to unmap.
 *
 * The old code dropped the pointer and incremented a page counter: the memory stayed mapped
 * (which is correct -- the firmware may still translate through it) but nothing could ever
 * name it again.  A count is not an owner.  This ledger is that owner: the chunk stays
 * addressable, is reported with the others, and is released only by the proven-quiesce path.
 *
 * If the ledger itself is full, transfer is refused: the caller must keep the original
 * table/AUX slot. New work is blocked rather than losing a pointer behind a page counter.
 */
bool Cx3Mlx4::keepIcmForQuiesce(Mlx4Icm *icm, const char *tag)
{
    if (!icm) return true;
    if (fIcmKeptCount < (UInt32)kIcmKeptMax) {
        fIcmKept[fIcmKeptCount].icm   = icm;
        fIcmKept[fIcmKeptCount].pages = icm->npages;
        fIcmKept[fIcmKeptCount].tag   = tag ? tag : "unnamed ICM chunk";
        fIcmKept[fIcmKeptCount].generation = icm->generation;
        ++fIcmKeptCount;
        return true;
    }
    fQuarExhausted = true;
    fNetEnabled = false;
    LOG("icm: %s (%u page(s)) could not be unmap-confirmed and the retention ledger is full "
        "(%u entries); ownership transfer refused. The original table/AUX slot must "
        "remain addressable and no new work is admitted",
        tag ? tag : "unnamed ICM chunk", (unsigned)icm->npages,
        (unsigned)kIcmKeptMax);
    return false;
}

/* Readiness flags alone cannot represent partial setup or failed hand-back.
 * A surviving allocation is also an owner, even when its Up bit is false.
 * This is an admission/dependency guard, not a device-quiescence proof. */
bool Cx3Mlx4::hasContextOwners() const
{
    if (fHw.hasMttAllocations() || fMrState != 0 || fEqRing || fEqNent != 0) return true;
    for (UInt32 port = 0; port < 2; ++port)
        if (fPortQueues[port].hasOwners()) return true;
    return false;
}

bool Cx3Mlx4::hasIcmTableAllocations() const
{
    if (fAuxIcm) return true;
    /* Inspect the bounded allocation slots, not only active profile counts:
     * these pointers remain the owners when a retention transfer refuses. */
    for (UInt32 i = 0; i < kMaxIcmTables; ++i)
        for (UInt32 c = 0; c < sizeof(fTables[i].icm) / sizeof(fTables[i].icm[0]); ++c)
            if (fTables[i].icm[c]) return true;
    return false;
}

bool Cx3Mlx4::hasStopRetentionOwners() const
{
    /* Counts include legacy unresolved retention diagnostics. Scan the bounded
     * initialized slots as well: a zero count cannot authorize losing a pointer. */
    if (fQuarCount || fQuarOverflowCount || fQuarOverflowUntracked ||
        fIcmKeptCount || fIcmKeptOverflow || fHw.mailboxNeedsRetention()) return true;
    for (UInt32 i = 0; i < sizeof(fQuar) / sizeof(fQuar[0]); ++i)
        if (fQuar[i].icm) return true;
    for (UInt32 i = 0; i < sizeof(fQuarOverflow) / sizeof(fQuarOverflow[0]); ++i)
        if (fQuarOverflow[i]) return true;
    for (UInt32 i = 0; i < sizeof(fIcmKept) / sizeof(fIcmKept[0]); ++i)
        if (fIcmKept[i].icm) return true;
    return false;
}

void Cx3Mlx4::freeIcmTables()
{
    UInt32 i, c, fwStatus = 0;
    UInt32 nChunks = (UInt32)(sizeof(fTables[0].icm) / sizeof(fTables[0].icm[0]));
    UInt32 freed = 0, kept = 0, keptPages = 0;
    bool asked = fIcmHcaHolds;      /* was this firmware instance told about them? */
    /*
     * !asked means an HCA reset happened after these tables were mapped, so the CURRENT
     * firmware instance was never told about them.  The old code freed them on exactly that
     * sentence.  It is true for the new instance and silent about the old one: a firmware
     * reset is not a PCIe FLR, and it does not flush writes the previous instance had already
     * issued.  A post-reset silence window is only an observation: even arbitrarily many
     * quiet samples cannot hand back ownership.  With no current-instance unmap or other
     * release proof, retain both table chunks and the auxiliary area regardless of that
     * readout.  !asked also conflates "never published" with "old instance knew it"; until
     * per-allocation ownership distinguishes them, prefer retention to a guessed free.
     */

    if (fNumTables == 0 && !fAuxIcm) return;
    if (fHcaInited) {
        LOG("icm tables: HCA remains initialized; CLOSE_HCA must be confirmed "
            "before table/AUX UNMAP or owner transfer");
        return;
    }
    if (hasContextOwners()) {
        LOG("icm tables: a context owner is outstanding; no table/AUX UNMAP or free "
            "is attempted, and all allocation pointers and mapping flags stay in place");
        return;
    }
    for (i = 0; i < fNumTables && i < kMaxIcmTables; ++i) {
        Mlx4IcmTable *t = &fTables[i];
        for (c = 0; c < nChunks; ++c) {
            UInt64 virt;
            if (!t->icm[c]) continue;
            virt = t->virt + (UInt64)c * MLX4_TABLE_CHUNK_SIZE;
            if (asked) {
                fwStatus = CX3_FWSTATUS_NOT_POSTED;
                if (!fHw.unmapIcm(t->icm[c]->npages, virt, &fwStatus)) {
                    LOG("icm table %s: chunk %u (virt 0x%llx, %u pages) is kept, not "
                        "freed: the firmware did not confirm the unmap (fw status 0x%x), "
                        "so it may still be translating through these pages",
                        t->name, (unsigned)c, (unsigned long long)virt,
                        (unsigned)t->icm[c]->npages, (unsigned)fwStatus);
                    if (!keepIcmForQuiesce(t->icm[c], t->name)) {
                        fLostIcmPages += keptPages;
                        return;
                    }
                    keptPages += t->icm[c]->npages;
                    ++kept;
                    t->icm[c] = 0;
                    continue;
                }
            } else {
                LOG("icm table %s: chunk %u (%u page(s)) is KEPT: no current-instance "
                    "unmap or device-quiescence proof exists (silence runs=%u changes=%u). "
                    "A quiet observation cannot authorise release",
                    t->name, (unsigned)c, (unsigned)t->icm[c]->npages,
                    (unsigned)fResetSilenceRuns, (unsigned)fResetSilenceChanges);
                if (!keepIcmForQuiesce(t->icm[c], t->name)) {
                    fLostIcmPages += keptPages;
                    return;
                }
                keptPages += t->icm[c]->npages;
                ++kept;
                t->icm[c] = 0;
                continue;
            }
            fHw.icmFree(t->icm[c]);
            t->icm[c] = 0;
            ++freed;
        }
    }
    if (fAuxIcm) {
        bool freeIt = true;

        if (asked) {
            fwStatus = CX3_FWSTATUS_NOT_POSTED;
            if (!fHw.unmapIcmAux(&fwStatus)) {
                LOG("icm aux area (%u pages) is kept, not freed: the firmware did not "
                    "confirm UNMAP_ICM_AUX (fw status 0x%x)",
                    (unsigned)fAuxIcm->npages, (unsigned)fwStatus);
                if (!keepIcmForQuiesce(fAuxIcm, "ICM auxiliary area")) {
                    fLostIcmPages += keptPages;
                    return;
                }
                keptPages += fAuxIcm->npages;
                ++kept;
                freeIt = false;
            }
        } else {
            LOG("icm aux area (%u pages) is KEPT: no current-instance unmap or "
                "device-quiescence proof exists (silence runs=%u changes=%u). "
                "A quiet observation cannot authorise release",
                (unsigned)fAuxIcm->npages,
                (unsigned)fResetSilenceRuns, (unsigned)fResetSilenceChanges);
            if (!keepIcmForQuiesce(fAuxIcm, "ICM auxiliary area")) {
                fLostIcmPages += keptPages;
                return;
            }
            keptPages += fAuxIcm->npages;
            ++kept;
            freeIt = false;
        }
        if (freeIt) { fHw.icmFree(fAuxIcm); ++freed; }
        fAuxIcm = 0;
    }
    /* Counted even when nothing was asked for: "nothing was released" is a fact too. */
    if (keptPages) fLostIcmPages += keptPages;
    fIcmMapped = false;
    fIcmHcaHolds = false;
    if (!asked) {
        /*
         * The honest version of the line this function used to print.  It does not say
         * the HCA forgot anything -- it names the command that was not sent and why, so
         * a reader can tell this case from a confirmed unmap and from a refusal.
         */
        LOG("icm tables: %u allocation(s) freed without UNMAP_ICM/UNMAP_ICM_AUX; "
            "%u allocation(s) retained because current-instance unmap and device "
            "quiescence are unproven. Silence readouts do not authorise release",
            (unsigned)freed, (unsigned)kept);
    } else {
        LOG("icm tables: %u allocation(s) unmapped and freed, %u kept (%u pages) because "
            "the firmware did not confirm the unmap",
            (unsigned)freed, (unsigned)kept, (unsigned)keptPages);
    }
    if (fLostIcmPages)
        LOG("icm tables: %u page(s) of ICM are now held by deliberate leaks (not returned "
            "to the kernel for the life of this boot)", (unsigned)fLostIcmPages);
}

/*
 * kCx3SelM2dBringUp -- the rest of M2d in one call, in the reference's order.
 *
 * Each step's firmware status is reported separately (0xffffffff = not reached),
 * so a failure names what failed rather than reporting "M2d did not work".
 */
IOReturn Cx3Mlx4::exportM2dBringUp(UInt64 *out)
{
    UInt32 setIcmStatus = 0xffffffffu, mapAuxStatus = 0xffffffffu;
    UInt32 initHcaStatus = 0xffffffffu;
    UInt32 tablesDone = 0;
    UInt32 auxPages = 0;
    UInt32 i;
    bool mttTableSet = false;

    memset(out, 0, sizeof(UInt64) * kBringUpOutCount);
    out[kBringUpOutSetIcmStat]  = 0xffffffffull;
    out[kBringUpOutMapAuxStat]  = 0xffffffffull;
    out[kBringUpOutInitHcaStat] = 0xffffffffull;
    out[kBringUpOutIcmSize]     = fIcmSize;

    if (fIcmSize == 0 || fNumTables == 0) {
        LOG("m2dbringup refused: run m2dprofile first (no profile is held)");
        out[kBringUpOutStatus] = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }
    if (fHw.hasMttAllocations()) {
        LOG("m2dbringup refused before device commands: MTT allocation owner records remain "
            "from an earlier table lifetime");
        out[kBringUpOutStatus] = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }
    /*
     * INIT_HCA is a once-per-load command, like the reference's: it is issued
     * after a reset and never twice, because the HCA rejects a second one with
     * CMD_STAT_BAD_SYS_STATE (0x4).  Without this guard a re-run does the rest of
     * the sequence -- re-sends SET_ICM_SIZE, allocates and maps a second
     * auxiliary area -- and then fails at the last step with a status that says
     * nothing about the real cause.  Measured: running this twice in a row
     * produced exactly that, and it is how the unmap-before-free bug below was
     * found.
     */
    if (fHcaInited) {
        LOG("m2dbringup refused: the HCA is already initialised this boot; the "
            "reference issues INIT_HCA once per load, after a reset. Run "
            "'cx3ctl reset force' first if a clean sequence is wanted.");
        out[kBringUpOutStatus] = (UInt64)kIOReturnNotReady;
        out[kBringUpOutTablesTotal] = fNumTables;
        out[kBringUpOutTablesDone]  = fNumTables;
        out[kBringUpOutTableMask]   = ~0ull;
        out[kBringUpOutInitHcaStat] = CX3_FWSTATUS_NOT_POSTED;
        return kIOReturnSuccess;
    }
    if (!fHw.windowAlive()) {
        LOG("m2dbringup refused: the BAR0 window is not reading back live");
        out[kBringUpOutStatus] = (UInt64)kIOReturnOffline;
        return kIOReturnSuccess;
    }

    /* 1. SET_ICM_SIZE: how much context memory, and what the HCA wants back. */
    if (!fHw.setIcmSize(fIcmSize, &fAuxUnits, &setIcmStatus)) {
        out[kBringUpOutSetIcmStat] = setIcmStatus;
        out[kBringUpOutStatus]    = (UInt64)kIOReturnError;
        return kIOReturnSuccess;
    }
    out[kBringUpOutSetIcmStat] = setIcmStatus;
    out[kBringUpOutAuxUnits]   = fAuxUnits;

    /*
     * 2. The auxiliary area.
     *
     * SET_ICM_SIZE's answer is already a count of 4 KiB pages: the reference
     * hands it straight to mlx4_alloc_icm(dev, aux_pages, ...) whose parameter is
     * npages in system pages (mlx4_icm.c:128-200), and its own log line prints
     * aux_pages << 2 as KB (mlx4_main.c:1577-1579).  This port first treated it
     * as a count of 1 MiB units and multiplied by 256, asking for 256x the
     * memory the HCA wanted.
     */
    auxPages = (UInt32)fAuxUnits;
    out[kBringUpOutAuxPages] = auxPages;
    if (auxPages == 0) {
        /*
         * No auxiliary memory requested.  The reference still calls
         * MAP_ICM_AUX unconditionally with whatever it allocated, and a
         * zero-page allocation would be meaningless, so this is reported as a
         * distinct outcome rather than silently skipped.
         */
        LOG("m2dbringup: SET_ICM_SIZE asked for 0 aux pages; skipping the aux "
            "mapping (MLX4_CMD_MAP_ICM_AUX takes a page list, so there is nothing "
            "to send)");
    } else {
        /*
         * Unmap before freeing.  The HCA's page tables still point at the old
         * auxiliary area; recycling that host memory while the firmware holds a
         * reference to it is exactly the failure mode the reference avoids
         * (mlx4_main.c:1752-1762 unmaps then frees).  This path did free first,
         * which a re-run of the whole sequence exposed.
         *
         * And the unmap's answer now decides, as in freeIcmTables: this call used to
         * drop it and free anyway, so a refusal -- the firmware saying it could not
         * take the area back -- was represented as a successful release.
         */
        if (fAuxIcm) {
            UInt32 stale = CX3_FWSTATUS_NOT_POSTED;
            if (!fIcmHcaHolds || fHw.unmapIcmAux(&stale)) {
                fHw.icmFree(fAuxIcm);
                fAuxIcm = 0;
            } else if (fAuxIcm->npages == auxPages) {
                /*
                 * The firmware would not take the area back and the new profile wants
                 * the same size, so MAP_ICM_AUX below re-declares this allocation as
                 * the auxiliary area: it is not freed and not leaked, and the mapping
                 * the firmware did not give up is the one it keeps.
                 */
                LOG("m2dbringup: UNMAP_ICM_AUX failed (fw status 0x%x); the %u-page aux "
                    "area is kept and MAP_ICM_AUX re-declares it -- freeing it would hand "
                    "the firmware back memory it may still be using",
                    (unsigned)stale, (unsigned)fAuxIcm->npages);
            } else {
                /*
                 * Different size, so the old area cannot be reused and cannot be freed.
                 * It stays allocated and is counted with the deliberate leaks; a new
                 * area is allocated and MAP_ICM_AUX points the firmware at that.
                 */
                LOG("m2dbringup: UNMAP_ICM_AUX failed (fw status 0x%x) and the new profile "
                    "needs %u aux pages, not %u: the old area is kept, not freed, and a new "
                    "one is allocated", (unsigned)stale, (unsigned)auxPages,
                    (unsigned)fAuxIcm->npages);
                fLostIcmPages += fAuxIcm->npages;
                fAuxIcm = 0;
            }
        }
        if (!fAuxIcm) fAuxIcm = fHw.icmAlloc(auxPages, false);
        if (!fAuxIcm) {
            LOG("m2dbringup: could not allocate %u aux pages", (unsigned)auxPages);
            out[kBringUpOutStatus] = (UInt64)kIOReturnNoMemory;
            return kIOReturnSuccess;
        }
        if (!fHw.mapIcmAux(fAuxIcm, &mapAuxStatus)) {
            out[kBringUpOutMapAuxStat] = mapAuxStatus;
            out[kBringUpOutStatus]    = (UInt64)kIOReturnError;
            return kIOReturnSuccess;
        }
        fIcmHcaHolds = true;    /* the firmware has now been told about the aux area */
        out[kBringUpOutMapAuxStat] = mapAuxStatus;
    }

    /* 3. Every table INIT_HCA will refer to. */
    initIcmTables(out, &tablesDone);
    out[kBringUpOutTablesTotal] = fNumTables;
    if (tablesDone != fNumTables) {
        UInt64 mask = 0;
        for (i = 0; i < tablesDone && i < 64; ++i) mask |= (1ull << i);
        out[kBringUpOutTableMask] = mask;
        out[kBringUpOutStatus]    = (UInt64)kIOReturnError;
        LOG("m2dbringup: %u of %u tables mapped; stopping before INIT_HCA",
            (unsigned)tablesDone, (unsigned)fNumTables);
        return kIOReturnSuccess;
    }
    out[kBringUpOutTableMask] = ~0ull;
    fIcmMapped = true;

    /*
     * Hand the queue code the MTT table's host base.  The table is one mapped
     * 256 KiB chunk at mtt_base, and MTT entries are plain host memory written
     * directly on a physical function (mlx4_mr.c:697-721), so the queue code
     * needs the base and the reserved prefix and nothing else.
     */
    for (i = 0; i < fNumTables; ++i) {
        if (strcmp(fTables[i].name, "MTT") == 0 && fTables[i].icm[0]) {
            if (!fHw.setMttTable(fTables[i].icm[0]->virt, fTables[i].icm[0]->bytes,
                                 fDevCap.reserved_mtts, fTables[i].virt)) {
                out[kBringUpOutStatus] = (UInt64)kIOReturnNotReady;
                LOG("m2dbringup refused before INIT_HCA: the MTT table view could not be "
                    "safely installed; mapped resources remain owned for recovery");
                return kIOReturnSuccess;
            }
            mttTableSet = true;
            break;
        }
    }
    if (!mttTableSet) {
        out[kBringUpOutStatus] = (UInt64)kIOReturnNotReady;
        LOG("m2dbringup refused before INIT_HCA: the profile has no mapped MTT table");
        return kIOReturnSuccess;
    }

    /*
     * The same for the dMPT table, and for the same reason: R4 needs to read an MPT
     * entry back after SW2HW_MPT rather than trusting the command's status.  R4 stage 1
     * also needs the PD range, which comes from QUERY_DEV_CAP -- this port did not
     * decode reserved_pds at all before now.
     */
    fHw.setPdRange(fDevCap.reserved_pds);
    for (i = 0; i < fNumTables; ++i) {
        if (strcmp(fTables[i].name, "dMPT") == 0 && fTables[i].icm[0]) {
            fHw.setMptTable(fTables[i].icm[0]->virt, fTables[i].icm[0]->bytes,
                            fTables[i].objSize);
            break;
        }
    }

    /* 4. INIT_HCA. */
    if (!fHw.initHca(&fInitHca, &fDevCap, &initHcaStatus)) {
        out[kBringUpOutInitHcaStat] = initHcaStatus;
        out[kBringUpOutStatus]     = (UInt64)kIOReturnError;
        return kIOReturnSuccess;
    }
    out[kBringUpOutInitHcaStat] = initHcaStatus;
    fHcaInited = true;

    out[kBringUpOutStatus] = 0;
    LOG("m2dbringup OK: icm %llu KiB, aux %u page(s) (%u KiB), %u tables mapped, "
        "INIT_HCA ok",
        (unsigned long long)(fIcmSize >> 10), (unsigned)auxPages,
        (unsigned)(auxPages * 4), (unsigned)tablesDone);
    return kIOReturnSuccess;
}

/* ==================================================================== */
/* M3, restart 1: the event queue                                        */
/* ==================================================================== */

/*
 * Hand the completion queue back.
 *
 * Same shape and same reasoning as teardownEq and mrTeardown, and it has to run before
 * anything the CQ names is released: the CQ context points at MTT entries inside the MTT
 * table and at a record in host DMA memory, so freeing either while the hardware still
 * owns the queue leaves it writing into recycled memory.
 */
void Cx3Mlx4::cqTeardown(const char *why, bool hcaWasReset, UInt32 port)
{
    if (port<1 || port>2) return;
    Cx3PortQueues &queue = fPortQueues[port-1];
    queue.setupReady=false;

    UInt32 status = CX3_FWSTATUS_NOT_POSTED;

    /* A timed-out SW2HW_CQ is not a rejection: the creator may have retained
     * the ring/record/MTT while queue.cqUp stayed false. Keep that owner reachable
     * so a later teardown can retry HW2SW_CQ instead of leaking it forever. */
    if (!queue.cqUp && !queue.cqRing && !queue.cqRec) return;

    /* A failed/partial RX QP still names this CQN even when readiness is false.
     * Calling HW2SW_CQ while that owner survives is not an ownership proof for
     * the dependency. Leave the CQ and its MTT/record intact until QP return. */
    if (!hcaWasReset && (queue.qpUp || queue.qpRec)) {
        LOG("CQ teardown (%s): RX QPN %u still has an owner; keeping CQN %u and all "
            "its memory without issuing HW2SW_CQ", why,
            (unsigned)queue.qpQpn, (unsigned)queue.cqCqn);
        return;
    }

    if (hcaWasReset) {
        queue.cqHwReturned = true;
        LOG("CQ teardown (%s): the HCA was reset, so it has forgotten CQN %u and there is "
            "nothing to hand back; dropping the host-side state", why, (unsigned)queue.cqCqn);
    } else if (queue.cqMayBeDeviceOwned && !queue.cqHwReturned &&
               !fHw.cqHw2Sw(queue.cqCqn, &status)) {
        /*
         * Do not release the ring or the record, and do not hand the numbers back.  The
         * CQ may still be hardware-owned and its context still names that memory; leaking
         * it is the cheaper failure, exactly as teardownEq decides for a ring it cannot
         * take back.
         */
        LOG("CQ teardown (%s): HW2SW_CQ %u failed (fw status 0x%x) -- the queue may still "
            "be hardware-owned, so the ring, the record and the MTT entries are all "
            "deliberately kept and nothing the CQ names is released. A reboot reclaims "
            "them.", why, (unsigned)queue.cqCqn, (unsigned)status);
        return;
    } else if (queue.cqMayBeDeviceOwned) {
        queue.cqHwReturned = true;
    }

    queue.cqUp = false; /* A returned queue remains owned by the host but is no longer usable. */
    if (!hcaWasReset && queue.cqMttPages) {
        if (!fHw.mttFree(queue.cqMttOff, queue.cqMttPages, queue.cqRing)) {
            LOG("CQ teardown (%s): MTT release refused; preserving ring/record and exact MTT owner",
                why);
            return;
        }
        queue.cqMttOff = queue.cqMttPages = 0;
    }

    if (!releaseIcmOrKeep(&queue.cqRing, "RX CQ ring", hcaWasReset) ||
        !releaseIcmOrKeep(&queue.cqRec, "RX CQ doorbell record", hcaWasReset)) return;
    if (hcaWasReset) {
        LOG("CQ teardown (%s): reset alone is not an MTT release credential; retaining "
            "indices %u + %u for this table lifetime", why, (unsigned)queue.cqMttOff,
            (unsigned)queue.cqMttPages);
    }
    queue.cqHwReturned = false;
    queue.cqUnknownRetained = false;
    queue.cqMayBeDeviceOwned = false;
    queue.cqNent      = 0;
    queue.cqMttOff    = 0;
    queue.cqMttPages  = 0;
    queue.cqRecPhys   = 0;
    queue.cqConsIndex = 0;
    /* Stage 9's consumer index for this queue goes with the queue: its CQEs no longer exist,
     * so a later r4raw starts reading from slot 0 of the new ring. */
    queue.rawRxCons   = 0;
    LOG("CQ teardown (%s): CQN %u given back and the ring released", why,
        (unsigned)queue.cqCqn);
}


/*
 * Hand the memory region back.
 *
 * The order mirrors teardownEq and for the same reason: an MPT entry the hardware owns
 * names memory in the dMPT table, so the table must not be released underneath it.  The
 * reference does the same -- mlx4_mr_free_reserved issues HW2SW_MPT before it touches
 * the MTT or frees the ICM reference (mlx4_mr.c:551-581), and the Ethernet driver
 * destroys the network device before releasing the shared MR
 * (mlx4_en_main.c:154-163).
 *
 * A failure to take the entry back does NOT proceed to release anything, and it is not
 * reported as success: the entry may still be hardware-owned, and the next user of that
 * slot would silently share it.
 */
void Cx3Mlx4::mrTeardown(const char *why, bool hcaWasReset)
{
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;

    if (fMrState == 0) return;

    /* The shared key/PD is a dependency of RX/RSS/TX QPs. A false Up bit
     * does not prove ownership return after partial setup; retained records
     * are owners too. Do not revoke the MR out from under any such QP. */
    if (!hcaWasReset) {
        for (UInt32 port = 0; port < 2; ++port) {
            const Cx3PortQueues &q = fPortQueues[port];
            if (q.qpUp || q.qpRec || q.ingressUp || q.ingressRec || q.txQpUp || q.txQpRec) {
                LOG("MR teardown (%s): port %u has a QP owner; retaining shared MPT %u and PD %u",
                    why, (unsigned)(port + 1), (unsigned)fMrMptIndex, (unsigned)fMrPdn);
                return;
            }
        }
    }

    if (hcaWasReset) {
        LOG("MR teardown (%s): the HCA was reset, so it has already forgotten MPT %u "
            "(PD %u) and there is nothing to hand back; dropping the host-side state",
            why, (unsigned)fMrMptIndex, (unsigned)fMrPdn);
        fMrState = 0;
        return;
    }

    if (fMrState == 2) {
        /*
         * The entry's ownership was never established -- the enabling command timed
         * out.  Handing it back is worth attempting, but a failure here must not be
         * read as "it is safe to release": nothing is known either way.
         */
        LOG("MR teardown (%s): MPT %u was left in the UNKNOWN state (the enabling "
            "command did not complete), so taking it back is an attempt, not a "
            "guarantee", why, (unsigned)fMrMptIndex);
    }

    if (!fHw.mrHw2Sw(fMrMptIndex, &status)) {
        /*
         * Do not release, and do not hand the numbers back.  The same choice teardownEq
         * makes for a ring it cannot take back: leaking a slot on the way out is the
         * cheaper failure than freeing memory the device still names.
         */
        fMrState = 2;
        LOG("MR teardown (%s): HW2SW_MPT %u failed (fw status 0x%x) -- the entry may "
            "still be hardware-owned, so the MPT slot and the PD are deliberately kept "
            "and nothing that the entry names is released. A reboot reclaims them.",
            why, (unsigned)fMrMptIndex, (unsigned)status);
        return;
    }

    LOG("MR teardown (%s): MPT %u is back in software's hands; PD %u is free to release",
        why, (unsigned)fMrMptIndex, (unsigned)fMrPdn);
    fMrState = 0;
}


/*
 * Drop the host-side EQ state after an acknowledged queue return or an HCA reset.
 *
 * Reset invalidates the EQ's device context, but reset alone is not the release
 * credential for memory the device may have owned. On the reset path the ring is
 * transferred to the read-only retention ledger and its MTT indices stay allocated
 * for the table lifetime. Only the non-reset path that successfully completes
 * HW2SW_EQ may return the ring and its MTT range to their allocators.
 */
void Cx3Mlx4::teardownEq(const char *why, bool hcaWasReset)
{
    UInt32 status = 0;

    if (!fEqRing && !fEqNent) return;

    /*
     * Order matters, and it did not use to be right: the queue has to stop being
     * the firmware's before the memory it points at is given back.  Two cases:
     *
     *   the HCA was just reset -> it has forgotten the queue, so there is nothing
     *                             to hand back and HW2SW_EQ would fail anyway;
     *   the driver is stopping -> the firmware still holds the EQ, so HW2SW_EQ
     *                             comes first.  FreeBSD mlx4_eq.c:1080-1090 does
     *                             exactly that before freeing the ring.
     *
     * Skipping it in the second case leaves the HCA holding a context and an MTT
     * entry list that point at host memory which is about to be recycled.
     */
    if (!hcaWasReset && fEqMayBeDeviceOwned && fEqn && !fEqHwReturned) {
        if (!fHw.eqHw2Sw(fEqn, &status)) {
            /*
             * Do NOT free the ring.
             *
             * Both reviews: without HW2SW_EQ there is no evidence the device stopped
             * referring to these pages, and handing recycled memory to the allocator
             * while the firmware still holds the page list is how a device writes into
             * whatever the host puts there next.  Leaking a ring on the way out is the
             * cheaper failure -- and this only happens when the driver is stopping or
             * the hand-back failed, never on the reset path where the device has
             * already forgotten the queue.
             */
            LOG("EQ teardown (%s): HW2SW_EQ failed (fw status 0x%x) -- the HCA may "
                "still hold the ring's page list, so the ring is deliberately NOT "
                "freed. The EQ number, ring and dependency metadata remain owned; "
                "a retry must still obtain HW2SW_EQ confirmation.",
                why, (unsigned)status);
            return;
        }
        fEqHwReturned = true;
    } else {
        if (hcaWasReset) fEqHwReturned = true;
        LOG("EQ teardown (%s): no HW2SW_EQ -- %s", why,
            hcaWasReset ? "the HCA was reset and has already forgotten the queue"
                        : (fEqHwReturned ? "a prior HW2SW_EQ already returned the queue"
                                         : (fEqMayBeDeviceOwned ? "there was no EQ number"
                                                               : "SW2HW_EQ was never issued")));
    }

    if (!hcaWasReset && fEqMttPages) {
        if (!fHw.mttFree(fEqMTTOffset, fEqMttPages, fEqRing)) {
            fEqUnknownRetained = true;
            LOG("EQ teardown (%s): MTT release refused; preserving returned EQ ring and owner metadata",
                why);
            return;
        }
        fEqMTTOffset = fEqMttPages = 0;
    }

    LOG("EQ teardown (%s): the %u-entry ring and the MTT entries it sat in are being "
        "released -- %s", why, (unsigned)fEqNent,
        hcaWasReset ? "retained read-only, not freed" : "freed");
    if (!releaseIcmOrKeep(&fEqRing, "EQ ring", hcaWasReset)) return;
    fEqNent = 0;
    fEqeFactor = 0;
    fEqn = 0;
    if (hcaWasReset) {
        LOG("EQ teardown (%s): reset alone is not an MTT release credential; retaining "
            "indices %u + %u for this table lifetime", why, (unsigned)fEqMTTOffset,
            (unsigned)fEqMttPages);
    }
    fEqDoorbell = 0;
    fEqConsIndex = 0;
    fEqUnknownRetained = false;
    fEqHwReturned = false;
    fEqMayBeDeviceOwned = false;
}

/*
 * Look a context table up by name in the list the M2d bring-up built.
 *
 * (This and zeroEqcEntry were declared and called for a while before the
 * definitions existed -- an edit that removed the bodies silently, caught by the
 * build's symbol accounting rather than by anything at run time.  Noting it
 * because the build failing is the system working: two undefined symbols is
 * exactly what that check exists to turn into a refusal.)
 */
Mlx4IcmTable *Cx3Mlx4::findTable(const char *name)
{
    UInt32 i;

    if (!name) return 0;
    for (i = 0; i < fNumTables && i < kMaxIcmTables; ++i)
        if (fTables[i].name && strcmp(fTables[i].name, name) == 0) return &fTables[i];
    return 0;
}

/*
 * Zero the EQC table entry the HCA will write our EQ context into.
 *
 * Without this the read-back cannot tell "SW2HW_EQ did not store the context"
 * apart from "the entry still holds what a previous attempt left there" -- and
 * with resets and retries in the same boot, the second is quite possible.  Zeroed
 * first, an all-zero entry afterwards is unambiguous, and that distinction is the
 * whole point of reading it back.
 */
void Cx3Mlx4::zeroEqcEntry(UInt32 eqn)
{
    Mlx4IcmTable *eqc = findTable("EQC");
    UInt64 off;

    if (!eqc || !eqc->icm[0] || !eqc->icm[0]->virt) {
        LOG("zeroEqcEntry: no mapped EQC table, so the read-back will be ambiguous");
        return;
    }
    off = (UInt64)eqn * eqc->objSize;
    if (off + eqc->objSize > eqc->icm[0]->bytes) {
        LOG("zeroEqcEntry: EQN %u (offset 0x%llx) is past the mapped EQC chunk "
            "(%llu bytes)", (unsigned)eqn, (unsigned long long)off,
            (unsigned long long)eqc->icm[0]->bytes);
        return;
    }
    memset((UInt8 *)eqc->icm[0]->virt + off, 0, eqc->objSize);
    LOG("zeroEqcEntry: cleared the %u-byte EQC entry for EQN %u (HCA 0x%llx + 0x%llx)",
        (unsigned)eqc->objSize, (unsigned)eqn, (unsigned long long)eqc->virt,
        (unsigned long long)off);
}

/*
 * kCx3SelM3EqUp -- QUERY_FUNC, then one EQ: ring, MTT, SW2HW_EQ, MAP_EQ, arm.
 *
 * The EQ number is NOT assumed.  On a device with system EQs the numbers this
 * function may use start after `reserved_eqs`, and QUERY_FUNC is what reports it
 * (mlx4_main.c:2289-2297 overwrites the QUERY_DEV_CAP values with these) -- so it
 * is asked first and its answer is what the EQ number is built from.
 */
IOReturn Cx3Mlx4::exportM3EqUp(const UInt64 *in, UInt64 *out)
{
    UInt32 nentLog2     = (UInt32)in[kEqUpInNentLog2];
    UInt32 intr         = (UInt32)in[kEqUpInIntr];
    UInt32 eqeFactor    = (UInt32)in[kEqUpInEqeFactor] ? 1u : 0u;
    UInt32 useAsyncSize = (UInt32)in[kEqUpInUseAsync];
    bool   mttAbs       = (in[kEqUpInMttAbs] != 0);
    UInt32 fwStatus = CX3_FWSTATUS_NOT_POSTED, mapStatus = CX3_FWSTATUS_NOT_POSTED;
    UInt32 nent, pages = 0, mttOffset = 0;
    UInt64 mttAddr = 0, doorbell = 0;
    UInt64 eventMask;

    memset(out, 0, sizeof(UInt64) * kEqUpOutCount);
    out[kEqUpOutFwStatus]    = CX3_FWSTATUS_NOT_POSTED;
    out[kEqUpOutMapEqStatus] = CX3_FWSTATUS_NOT_POSTED;
    out[kEqUpOutStatus]      = (UInt64)kIOReturnError;

    if (!fHcaInited) {
        LOG("m3equp refused: the HCA is not initialised (run m2dbringup first)");
        out[kEqUpOutStatus] = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }
    if (fEqRing) {
        LOG("m3equp refused: an EQ is already up this boot; reset first");
        out[kEqUpOutStatus] = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }
    if (!fHw.windowAlive()) {
        out[kEqUpOutStatus] = (UInt64)kIOReturnOffline;
        return kIOReturnSuccess;
    }

    /* 1. QUERY_FUNC: this function's EQ and UAR allocation. */
    if (!fHw.queryFunc(&fFuncMaxEq, &fFuncReservedEq, &fFuncReservedUar, &fwStatus)) {
        out[kEqUpOutFwStatus] = fwStatus;
        return kIOReturnSuccess;
    }
    out[kEqUpOutFwStatus]    = fwStatus;
    out[kEqUpOutMaxEq]       = fFuncMaxEq;
    out[kEqUpOutReservedEq]  = fFuncReservedEq;
    out[kEqUpOutReservedUar] = fFuncReservedUar;

    /* 2. The ring size: the reference's async EQ is 256 data entries plus 128
     *    spare, rounded up to a power of two (mlx4_eq.c:967-972). */
    if (useAsyncSize) {
        UInt32 want = MLX4_NUM_ASYNC_EQE + MLX4_NUM_SPARE_EQE;
        nent = 1;
        while (nent < want) nent <<= 1;
    } else {
        nent = (nentLog2 == 0) ? 256u : (1u << nentLog2);
    }

    /*
     * The EQ number: the first one this function may use.  reserved_eqs comes
     * from QUERY_FUNC and is the count the firmware keeps for itself, so the
     * driver's first EQ is at that index.
     */
    fEqn = fFuncReservedEq;
    zeroEqcEntry(fEqn);

    if (!fHw.eqCreate(nent, (UInt8)intr, (UInt8)eqeFactor, fEqn, mttAbs, &fEqRing,
                      &pages, &mttOffset, &mttAddr, &doorbell, &fwStatus)) {
        out[kEqUpOutFwStatus] = fwStatus;
        if (fEqRing && pages != 0u) {
            fEqNent = nent;
            fEqeFactor = (UInt8)eqeFactor;
            fEqMTTOffset = mttOffset;
            fEqMttPages = pages;
            fEqHwReturned = false;
            fEqMayBeDeviceOwned = fwStatus == CX3_FWSTATUS_TIMEOUT;
            fEqDoorbell = doorbell;
            fEqConsIndex = 0;
            fEqUnknownRetained = true;
            out[kEqUpOutEqn] = fEqn;
            out[kEqUpOutNent] = nent;
            out[kEqUpOutPages] = pages;
            out[kEqUpOutMTTOffset] = mttOffset;
            out[kEqUpOutMTTAddr] = mttAddr;
            out[kEqUpOutDoorbell] = doorbell;
            out[kEqUpOutStatus] = (UInt64)kIOReturnNotReady;
            LOG("m3equp: EQN %u ring/MTT owner retained after create failure (fw status 0x%x, "
                "may_be_device_owned=%u); do not consume or recreate it",
                (unsigned)fEqn, (unsigned)fwStatus, (unsigned)fEqMayBeDeviceOwned);
        }
        return kIOReturnSuccess;
    }
    fEqNent       = nent;
    fEqeFactor    = (UInt8)eqeFactor;
    snapshotRing("after icmAlloc + memset(0) in eqCreate", 16);
    fEqMTTOffset  = mttOffset;
    fEqMttPages   = pages;
    fEqHwReturned = false;
    fEqMayBeDeviceOwned = true;
    fEqDoorbell   = doorbell;
    fEqConsIndex  = 0;
    fEqUnknownRetained = false;
    out[kEqUpOutFwStatus] = fwStatus;
    out[kEqUpOutEqn]      = fEqn;
    out[kEqUpOutNent]     = nent;
    out[kEqUpOutPages]    = pages;
    out[kEqUpOutMTTOffset] = mttOffset;
    out[kEqUpOutMTTAddr]  = mttAddr;
    out[kEqUpOutDoorbell] = doorbell;
    out[kEqUpOutRingBus]  = fEqRing->pages[0].busAddr;
    out[kEqUpOutMttAbs]   = mttAbs ? 1 : 0;

    /*
     * 3. MAP_EQ: route the events we care about to this EQ.  The mask is the
     *    reference's async mask (mlx4_eq.c:66-90) -- it includes
     *    MLX4_EVENT_TYPE_CMD, which is the one the test depends on, and
     *    PORT_CHANGE, which makes a link event visible later.
     */
    eventMask = (1ull << MLX4_EVENT_TYPE_CMD) |
                (1ull << MLX4_EVENT_TYPE_PORT_CHANGE);
    if (!fHw.eqMap(eventMask, fEqn, false, &mapStatus)) {
        out[kEqUpOutMapEqStatus] = mapStatus;
        return kIOReturnSuccess;
    }
    out[kEqUpOutMapEqStatus] = mapStatus;

    /* 4. Arm: write the consumer index with "request notification" set, which is
     *    what the reference's eq_set_ci(eq, 1) does (mlx4_eq.c:829 -- the
     *    unconditional re-arm at the end of its handler's scan).
     *
     *    This arm is what makes the queue usable to a POLLER: an EQ that has never
     *    been armed produces no completion EQEs, so m3eqpoll and the ring snapshots
     *    would have nothing to read.  It is deliberately not the arm that makes the
     *    NOTIFICATION work: it happens here, at creation, while nothing is delivering
     *    interrupts yet.  An event delivered into a queue in that window moves it to
     *    FIRED, and from FIRED it asserts nothing until software arms it again -- so
     *    the arm that matters is the one at session open, in exportM3IrqUp.  See
     *    docs/EQ-ORDERING.md. */
    snapshotRing("after SW2HW_EQ (before arm)", 16);
    fHw.eqSetConsumerIndex(fEqDoorbell, 0, true);

    /*
     * Give the HCA a moment to take ownership of the ring, then record what it
     * looks like.  Measured: immediately after arming the owner bytes are all
     * zero, and a poll at that instant consumes an unwritten slot; a little later
     * they are set.  So "armed" is a state the HCA reaches asynchronously, and
     * anything that polls before it arrives is reading an unarmed ring.
     */
    /*
     * Wait for the ring to be armed AND stable before returning.
     *
     * What is measured, as of the A/B ring experiment (M2B-NOTES section 21): the
     * firmware writes the whole ring while it accepts the queue -- zero before
     * SW2HW_EQ, 0xcc in each slot's first half and 0xee in the second half right
     * after, with the owner bit set in all 512 slots -- and the write is already
     * complete when the command returns.  Eight samples over a second see no further
     * change.  The untouched control ring in that experiment never changed at all.
     *
     * What this comment used to claim, and why it was wrong: that the initialisation
     * was asynchronous and took "on the order of 200 ms".  There is no such
     * measurement.  The number came from the tool printing 238 where the kernel had
     * written 1: reportOwnerMap wrote a raw owner byte (0xee = 238) into the same
     * output slot as kEqUpOutArmWaitMs.  A constant was read as a duration for
     * several milestones, and the claim was repeated in these notes as though it had
     * been observed.  The kernel's own log said 1 ms on every run.
     *
     * The wait is kept anyway, and for a reason that survives the correction: by the
     * reference's own rule (owner == phase, both 0 on the first lap) an all-zero ring
     * reads as consumable in every slot, so a poll that runs before the firmware has
     * written the ring walks the consumer index over slots nobody wrote.  Whether
     * that window is 1 ms or 200 ms, the cheap guard is the same, and this code does
     * not have to believe anything about firmware timing to apply it.  What changed
     * is the claim, not the guard.
     */
    {
        UInt32 waited = 0, distinct = 0;
        /*
         * The owner byte's position comes from the same definition the consumer
         * uses.  This line used to compute "half the stride", which agrees with
         * the 64-byte format this card uses and is 16 bytes off for the 32-byte
         * one -- see MLX4_EQE_OWNER_BYTE.
         */
        UInt64 off = MLX4_EQE_OWNER_BYTE(fEqeFactor);
        const UInt8 *v = (const UInt8 *)fEqRing->virt;
        UInt8 prev[16];

        bool settled = false;

        memset(prev, 0, sizeof(prev));
        for (waited = 0; waited < 1000; ++waited) {
            bool armedHere = (v[off] & 0x80) != 0;
            bool same = true;
            UInt32 k;
            for (k = 0; k < sizeof(prev); ++k)
                if (v[k] != prev[k]) { same = false; break; }
            if (!same) ++distinct;
            memcpy(prev, v, sizeof(prev));
            if (armedHere && same && waited > 0) { settled = true; break; }
            IOSleep(1);
        }
        /*
         * The timeout is a failure, not a detail.  The previous version fell out of
         * this loop and carried on reporting success, which meant "the ring never
         * settled" was indistinguishable from "the ring is armed" -- the same class
         * of mistake as a timeout reported as firmware status 0.
         */
        if (!settled) {
            LOG("armed wait: the ring never settled within %u ms (owner byte 0x%02x); "
                "refusing to report the EQ as up", (unsigned)waited, (unsigned)v[off]);
            out[kEqUpOutArmWaitMs] = waited;
            out[kEqUpOutStatus] = (UInt64)kIOReturnTimeout;
            teardownEq("the ring never settled", false);
            return kIOReturnSuccess;
        }
        LOG("armed wait: the ring reached the armed pattern and stopped changing "
            "after %u ms; %u distinct reading(s) of the first 16 bytes were seen "
            "on the way (more than 1 means the firmware writes it in stages, which "
            "is exactly the window a poll must not run in; slot 0 owner byte 0x%02x)",
            (unsigned)waited, (unsigned)distinct, (unsigned)v[off]);
        out[kEqUpOutArmWaitMs] = waited;
    }
    snapshotRing("after arm + armed-wait", 16);
    reportOwnerMap(out, kEqUpOutOwnerSet32, kCx3OwnerNoByteSlot);

    out[kEqUpOutStatus] = 0;
    LOG("m3equp OK: EQN %u, %u entries, %u pages, MTT offset %u, doorbell BAR2+0x%llx, "
        "MAP_EQ ok -- the ring is armed and a CMD event has somewhere to land",
        (unsigned)fEqn, (unsigned)nent, (unsigned)pages, (unsigned)mttOffset,
        (unsigned long long)doorbell);
    return kIOReturnSuccess;
}

/*
 * kCx3SelM3EqPoll -- fire an event-mode command and look for its CMD EQE.
 *
 * This is restart 1's acceptance test.  Order matters: the NOP is posted first
 * (it is E=1, so its completion becomes an EQE), then the ring is polled.  If the
 * EQE is there with a matching token, the whole chain is proven.  If it is not,
 * the raw first EQE is reported anyway, because "the owner bits say the HCA owns
 * every slot" and "there is an EQE of the wrong type" are different failures and
 * the bytes are what tell them apart.
 */
/*
 * The owner map, as the host sees it: how many of the first 32 slots carry the
 * owner bit, and the raw byte of the first slot.
 *
 * This is what makes a negative poll interpretable.  "The HCA owns every slot" is
 * what an armed, event-free queue looks like; a ring that reads as ready
 * everywhere means the host is looking at the wrong memory, or the HCA never took
 * ownership when the queue was armed.  The two are indistinguishable from the
 * symptom alone, and the initial owner state is hardware behaviour the source
 * does not state, so it is measured rather than assumed.
 */
/*
 * Log the first bytes of the EQ ring, with a label saying WHEN they were read.
 *
 * Three snapshots -- right after the ring is allocated and zeroed, right after
 * SW2HW_EQ, and right after arming -- answer mechanically what has been guessed at
 * twice: who writes the 0xcc/0xee content that appears in the ring, and when.  The
 * ring probe (/m3eqring) already proved the buffer itself is writable, readable
 * and reachable by the HCA, so the content is not a broken mapping and must have
 * been written deliberately by something.
 */
void Cx3Mlx4::snapshotRing(const char *when, UInt32 nbytes)
{
    static const char hexd[] = "0123456789abcdef";
    char line[96];
    const UInt8 *v;
    UInt32 j, k, stride;

    if (!fEqRing || !fEqRing->virt) return;
    v = (const UInt8 *)fEqRing->virt;
    stride = MLX4_EQ_ENTRY_SIZE << fEqeFactor;
    if (nbytes > 16) nbytes = 16;

    for (k = 0; k < 2; ++k) {
        UInt64 base0 = (UInt64)k * stride;
        if (base0 + nbytes > fEqRing->bytes) break;
        for (j = 0; j < nbytes; ++j) {
            UInt8 b = v[base0 + j];
            line[j * 2]     = hexd[(b >> 4) & 0xf];
            line[j * 2 + 1] = hexd[b & 0xf];
        }
        line[nbytes * 2] = 0;
        LOG("ring snapshot [%s]: slot %u first %u bytes = %s",
            when, (unsigned)k, (unsigned)nbytes, line);
    }
}

void Cx3Mlx4::reportOwnerMap(UInt64 *out, UInt32 setIdx, UInt32 byteIdx)
{
    UInt32 stride = MLX4_EQ_ENTRY_SIZE << fEqeFactor;
    UInt32 probe = (fEqNent < 32) ? fEqNent : 32;
    UInt32 set = 0, k;
    UInt8  slot0OwnerByte = 0xff;
    const UInt8 *v;
    char line[96];
    /*
     * The owner byte's position comes from the same definition the consumer uses.
     * This function used to compute it as "half the stride + 0x1f", which is the
     * same number for this card's 64-byte EQEs and 16 bytes off for 32-byte ones --
     * the third copy of that arithmetic, and the one that survived the first pass
     * over it.
     */
    UInt64 ownerOff = MLX4_EQE_OWNER_BYTE(fEqeFactor);

    /*
     * The two output indices are arguments now, and that is the fix for a bug that
     * cost real time: this function used to write out[base] and out[base + 1], and
     * m3equp passed base = 14 -- where 15 is kEqUpOutArmWaitMs.  So the owner byte
     * (0xee, i.e. 238) overwrote the arm-wait milliseconds, and for a while this
     * project believed the firmware took "238 ms" to initialise the ring.  It takes
     * 1 ms; 238 was a constant being read as a duration.  An implicit "+1
     * convention" is how two fields end up in one slot; an explicit index cannot
     * be ambiguous, and kCx3OwnerNoByteSlot says so when there is no room (m3equp's
     * output block is at the 16-scalar cap).
     */
    if (!fEqRing || !fEqRing->virt) return;
    v = (const UInt8 *)fEqRing->virt;

    if (ownerOff < fEqRing->bytes) slot0OwnerByte = v[ownerOff];
    if (byteIdx != kCx3OwnerNoByteSlot) out[byteIdx] = slot0OwnerByte;

    for (k = 0; k < probe; ++k) {
        UInt64 off = (UInt64)k * stride + ownerOff;
        if (off < fEqRing->bytes && (v[off] & 0x80)) ++set;
    }
    out[setIdx] = set;

    /*
     * The raw bytes, because every inference from a summary has been wrong so far
     * and the EQE's position inside a 64-byte slot is exactly the kind of thing a
     * summary hides.  Two lines per slot: the first 32 bytes (what the source
     * calls reserved) and the next 32 (where the legacy EQE is supposed to live).
     *
     * Hand-encoded rather than with snprintf: snprintf is not in the kext KPI, and
     * the build's symbol accounting refused to produce a kext that called it --
     * which is the check doing its job for the second time in one afternoon.
     */
    {
        static const char hexd[] = "0123456789abcdef";
        for (k = 0; k < 2 && (UInt64)(k + 1) * stride <= fEqRing->bytes; ++k) {
            UInt32 j;
            for (j = 0; j < 32 && j < stride; ++j) {
                UInt8 b = v[(UInt64)k * stride + j];
                line[j * 2]     = hexd[(b >> 4) & 0xf];
                line[j * 2 + 1] = hexd[b & 0xf];
            }
            line[64] = 0;
            LOG("m3eqpoll: slot %u bytes 0x00-0x1f (reserved half): %s", (unsigned)k, line);
            if (stride >= 64) {
                for (j = 0; j < 32; ++j) {
                    UInt8 b = v[(UInt64)k * stride + 32 + j];
                    line[j * 2]     = hexd[(b >> 4) & 0xf];
                    line[j * 2 + 1] = hexd[b & 0xf];
                }
                line[64] = 0;
                LOG("m3eqpoll: slot %u bytes 0x20-0x3f (legacy half):  %s", (unsigned)k, line);
            }
        }
    }

    LOG("m3eqpoll: owner map: %u of the first %u slots have the owner bit set "
        "(owner byte at slot base + 0x%llx), slot 0 owner byte = 0x%02x "
        "(ring %p, %llu bytes, %u entries of %u bytes)",
        (unsigned)set, (unsigned)probe,
        (unsigned long long)ownerOff,
        (unsigned)slot0OwnerByte, fEqRing->virt,
        (unsigned long long)fEqRing->bytes, (unsigned)fEqNent, (unsigned)stride);
}

/*
 * kCx3SelM3EqRing -- can the HCA write into the EQ ring's memory at all?
 *
 * This is the discriminator the first R1 run needed and did not have.  The run
 * showed: SW2HW_EQ and MAP_EQ both accepted, an event-mode NOP completed, and yet
 * the ring was untouched -- every owner byte zero and the consumed "EQE" of type
 * 0x00, i.e. a slot that was never written.  Two very different faults look
 * exactly like that:
 *
 *   - the HCA is not using this memory (MTT entries wrong, or the event routed
 *     to some other EQ), or
 *   - the HCA cannot reach this memory.
 *
 * Asking the HCA to DMA into a ring page via a path already proven to work -- a
 * QUERY_FW whose outbox points there, the same instrument used to prove the FW
 * area reachable -- separates them.  If the answer appears in the ring page, the
 * memory is fine and the fault is in the EQ's configuration; if it does not, the
 * fault is in the ring's bus addresses.
 */
IOReturn Cx3Mlx4::exportM3EqRing(const UInt64 *in, UInt64 *out)
{
    UInt32 page    = (UInt32)in[kEqRingInPage];
    bool   destructive = (in[kEqRingInForce] != 0);
    UInt32 fwStatus = CX3_FWSTATUS_NOT_POSTED;
    Mlx4QueryFwResult fw;
    UInt8  snapshot[MLX4_EQ_ENTRY_SIZE * 2];
    UInt32 i, changed = 0;

    memset(out, 0, sizeof(UInt64) * kEqRingOutCount);
    memset(snapshot, 0, sizeof(snapshot));
    out[kEqRingOutFwStatus] = CX3_FWSTATUS_NOT_POSTED;
    out[kEqRingOutStatus]   = (UInt64)kIOReturnError;

    if (fEqUnknownRetained) {
        LOG("m3eqring refused: EQN %u is retained after an ambiguous SW2HW_EQ timeout; "
            "the diagnostic must not read, overwrite, or hand its ring to QUERY_FW",
            (unsigned)fEqn);
        out[kEqRingOutStatus] = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }
    if (!fEqRing || !fEqRing->pages) {
        LOG("m3eqring refused: no EQ ring is up (run m3equp first)");
        out[kEqRingOutStatus] = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }
    if (page >= fEqRing->npages) {
        LOG("m3eqring refused: page %u is past the %u-page ring", (unsigned)page,
            (unsigned)fEqRing->npages);
        out[kEqRingOutStatus] = (UInt64)kIOReturnBadArgument;
        return kIOReturnSuccess;
    }
    /*
     * Refuse unless it is asked for explicitly.
     *
     * This probe writes 0xa5 over the target page and then hands the ring page to
     * the firmware as a command outbox.  On a live queue that destroys the ring's
     * contents, so it cannot be a routine diagnostic -- and once an interrupt
     * consumer exists it would be corrupting memory the device owns.  It stays
     * available because it is the only probe that separates "the HCA cannot reach
     * this memory" from "the configuration is wrong", but it now has to be intended.
     */
    if (!destructive) {
        LOG("m3eqring refused: it overwrites ring page %u with 0xa5 and hands it to "
            "the firmware; pass force=1 to intend that", (unsigned)page);
        out[kEqRingOutStatus] = (UInt64)kIOReturnNotPermitted;
        return kIOReturnSuccess;
    }
    if (!fHw.windowAlive()) {
        out[kEqRingOutStatus] = (UInt64)kIOReturnOffline;
        return kIOReturnSuccess;
    }

    /* Snapshot the first 64 bytes of that page, fill them with a pattern, then
     * let the HCA write its QUERY_FW answer over it. */
    {
        UInt8 *base = (UInt8 *)fEqRing->virt + ((UInt64)page << MLX4_ICM_PAGE_SHIFT);
        memcpy(snapshot, base, sizeof(snapshot));
        memset(base, 0xa5, sizeof(snapshot));
    }

    LOG("m3eqring: QUERY_FW with its outbox at EQ ring page %u (bus 0x%llx)",
        (unsigned)page, (unsigned long long)fEqRing->pages[page].busAddr);

    if (!fHw.queryFwAt(fEqRing, page, &fw, &fwStatus, false)) {
        out[kEqRingOutStatus]   = (UInt64)kIOReturnError;
        out[kEqRingOutFwStatus] = fwStatus;
        LOG("m3eqring: the command produced no answer (fw status 0x%x)", (unsigned)fwStatus);
        return kIOReturnSuccess;
    }

    {
        const UInt8 *base = (const UInt8 *)fEqRing->virt + ((UInt64)page << MLX4_ICM_PAGE_SHIFT);
        for (i = 0; i < sizeof(snapshot); ++i) if (base[i] != 0xa5) ++changed;
        /*
         * The status has to be reported on the success path too.  This line was
         * missing and the tool printed "not posted" for a probe that had in fact
         * answered -- the same class of mistake as the dropped EQE fields, so it
         * is worth noting that the byte count below is weak evidence anyway:
         * queryFwAt() zeroes the page before sending, so "changed" is satisfied by
         * the host's own memset.  The fw_ver that parses out of the page is the
         * real evidence, and only a well-formed answer produces it.
         */
        out[kEqRingOutFwStatus] = fw.fwStatus;
        out[kEqRingOutChanged] = changed;
        out[kEqRingOutFwVer]   = fw.fwVer;
        for (i = 0; i < sizeof(snapshot) && i / 4 < (UInt32)kEqRingMaxDwords; ++i) {
            if ((i % 4) == 0) out[kEqRingOutData0 + i / 4] = 0;
            out[kEqRingOutData0 + i / 4] |= (UInt64)base[i] << (24 - 8 * (i % 4));
        }
        LOG("m3eqring: QUERY_FW answered (fw_ver 0x%llx); %u of the first %u bytes "
            "differ from the 0xa5 pattern",
            (unsigned long long)fw.fwVer, (unsigned)changed, (unsigned)sizeof(snapshot));
    }

    out[kEqRingOutStatus] = 0;
    return kIOReturnSuccess;
}

/*
 * kCx3SelM3EqInfo -- two read-only checks that narrow the "no EQE" fault.
 *
 * The ring probe showed the HCA can write to the ring's *memory*, but it handed
 * the page's bus address over directly as the command's out_param, which is not
 * the path a real event takes.  A real event is written through the MTT: the EQ
 * context carries an MTT base and the HCA walks the entries to find the pages.  So
 * the memory being reachable says nothing about the MTT entries being right.
 *
 * This reports, without touching the device:
 *   - the ring's page bus addresses, as the driver allocated them, and
 *   - the MTT entries the HCA would read for those pages, read back from the
 *     mapped table
 * so the two can be compared directly.  A mismatch is the fault; a match moves
 * the search to the EQ context.
 *
 * It then asks the HCA what it thinks this EQ is (QUERY_EQ, opcode 0x15).  The
 * reference driver never sends it, so it may be unsupported on this firmware --
 * if it fails, that costs nothing but is recorded as unsupported rather than
 * assumed either way.
 */
IOReturn Cx3Mlx4::exportM3EqInfo(UInt64 *out, Cx3EqInfoResult *res)
{
    UInt32 fwStatus = CX3_FWSTATUS_NOT_POSTED;
    UInt32 i, n;
    const UInt8 *mtt;

    memset(out, 0, sizeof(UInt64) * kEqInfoOutCount);
    memset(res, 0, sizeof(*res));
    out[kEqInfoOutStatus]   = (UInt64)kIOReturnError;
    out[kEqInfoOutFwStatus] = CX3_FWSTATUS_NOT_POSTED;

    if (fEqUnknownRetained) {
        LOG("m3eqinfo refused: EQN %u has an unknown retained owner", (unsigned)fEqn);
        out[kEqInfoOutStatus] = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }
    if (!fEqRing || !fEqRing->pages) {
        LOG("m3eqinfo refused: no EQ ring is up");
        out[kEqInfoOutStatus] = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }

    /* The ring's own page addresses (up to 8 of them), and the MTT entries the
     * HCA would read for those same pages. */
    n = fEqRing->npages < (UInt32)kEqInfoMaxEntries ? fEqRing->npages
                                                    : (UInt32)kEqInfoMaxEntries;
    out[kEqInfoOutRingPages] = fEqRing->npages;
    out[kEqInfoOutMTTOffset] = fEqMTTOffset;

    if (!fHw.mttReadback(fEqMTTOffset, n, fEqRing, out, res)) {
        LOG("m3eqinfo: could not read the MTT entries back");
        out[kEqInfoOutStatus] = (UInt64)kIOReturnError;
        return kIOReturnSuccess;
    }

    /*
     * One measurement per page, and the verdict travels in its own array.
     *
     * The verdict used to be bit 31 of mttEntry[] itself -- and bit 31 of an MTT entry
     * is an address bit, so every ring page at or above 2 GiB reported "DOES NOT MATCH"
     * for an entry that matched.  Measured on this card: the ring's first page read as
     * bus 0xafae4000 (bit 31 set, so the value alone is >= 2 GiB), and the tool printed
     * "MTT ... <- DOES NOT MATCH" and masked that
     * bit off before printing the value, i.e. it displayed a number the kernel never
     * read and sent the reader after a ring/MTT mismatch that did not exist.  The
     * earlier boot printed MATCH only because that boot's ring landed below 2 GiB
     * (docs/EQ-ORDERING.md section 4.5; docs/DEFECTS.md F7).
     *
     * This is the same lesson the low-32-bit ABI defect recorded (F2/D9) one level
     * down: a value must not carry its own verdict, because the two can collide and
     * nothing in the representation says which one was meant.  mttMatch[] is that
     * separate field, and the old second loop -- which re-read every entry to decide
     * the aggregate -- is gone with it, so the per-page verdicts and the summary line
     * cannot disagree: the summary is computed from the verdicts.
     */
    {
        UInt32 mismatches = 0;

        for (i = 0; i < n; ++i) {
            UInt64 ent  = fHw.mttEntryValue(fEqMTTOffset + i);
            UInt64 want = fEqRing->pages[i].busAddr | MLX4_MTT_FLAG_PRESENT;

            res->ringBus[i]  = (UInt32)fEqRing->pages[i].busAddr;
            res->mttEntry[i] = (UInt32)ent;
            res->mttMatch[i] = (ent == want) ? 1u : 0u;
            if (!res->mttMatch[i]) ++mismatches;
        }
        out[kEqInfoOutMatch] = (mismatches == 0) ? 1 : 0;
        LOG("m3eqinfo: MTT entries %u..%u %s the ring's page addresses (%u of %u "
            "differ; the ring's first page is at bus 0x%llx)",
            (unsigned)fEqMTTOffset, (unsigned)(fEqMTTOffset + n - 1),
            out[kEqInfoOutMatch] ? "MATCH" : "DO NOT MATCH",
            (unsigned)mismatches, (unsigned)n,
            (unsigned long long)fEqRing->pages[0].busAddr);
    }

    /*
     * The HCA's own copy of our EQ context, straight out of the EQC table.
     *
     * This is the decisive check for "did SW2HW_EQ store what we sent": the HCA
     * copies the context into the table at eqc_base + eqn * entry_size, the whole
     * table is mapped in host memory, and nothing has to be asked of the firmware.
     * If the bytes match the context this port built, the context is not the
     * problem; if they are zero, SW2HW_EQ never stored it and nothing downstream
     * can work.
     */
    {
        Mlx4IcmTable *eqc = findTable("EQC");
        if (eqc && eqc->icm[0] && eqc->icm[0]->virt) {
            UInt64 off = (UInt64)fEqn * eqc->objSize;
            const UInt8 *base;
            UInt32 nz = 0;
            out[kEqInfoOutEqcBase]   = eqc->virt;
            out[kEqInfoOutEqcOffset] = off;
            if (off + eqc->objSize <= eqc->icm[0]->bytes) {
                base = (const UInt8 *)eqc->icm[0]->virt + off;
                for (i = 0; i < (UInt32)kEqInfoCtxWords; ++i) {
                    res->eqcEntry[i] = ((UInt32)base[i * 4] << 24) |
                                       ((UInt32)base[i * 4 + 1] << 16) |
                                       ((UInt32)base[i * 4 + 2] << 8) |
                                       (UInt32)base[i * 4 + 3];
                }
                for (i = 0; i < eqc->objSize; ++i) if (base[i]) ++nz;
                out[kEqInfoOutEqcNonzero] = nz;
                LOG("m3eqinfo: EQC entry for EQN %u at HCA 0x%llx + 0x%llx: %u of %u "
                    "bytes non-zero; first dwords %08x %08x %08x %08x %08x %08x %08x %08x",
                    (unsigned)fEqn, (unsigned long long)eqc->virt,
                    (unsigned long long)off, (unsigned)nz, (unsigned)eqc->objSize,
                    (unsigned)res->eqcEntry[0], (unsigned)res->eqcEntry[1],
                    (unsigned)res->eqcEntry[2], (unsigned)res->eqcEntry[3],
                    (unsigned)res->eqcEntry[4], (unsigned)res->eqcEntry[5],
                    (unsigned)res->eqcEntry[6], (unsigned)res->eqcEntry[7]);
            } else {
                LOG("m3eqinfo: EQN %u (offset 0x%llx) is past the mapped EQC chunk "
                    "(%llu bytes)", (unsigned)fEqn, (unsigned long long)off,
                    (unsigned long long)eqc->icm[0]->bytes);
            }
        } else {
            LOG("m3eqinfo: no mapped EQC table, cannot read the HCA's copy");
        }
    }

    /* And what the HCA says about the EQ itself, if it supports the query. */
    {
        Mlx4EqContextResult ec;
        if (fHw.queryEq(fEqn, &ec, &fwStatus)) {
            out[kEqInfoOutQueryEqOk] = 1;
            for (i = 0; i < (UInt32)kEqInfoMaxEntries && i * 4 < 32; ++i)
                res->eqContext[i] = ec.words[i];
            LOG("m3eqinfo: QUERY_EQ answered; first words %08x %08x %08x %08x",
                (unsigned)ec.words[0], (unsigned)ec.words[1],
                (unsigned)ec.words[2], (unsigned)ec.words[3]);
        } else {
            out[kEqInfoOutQueryEqOk] = 0;
            LOG("m3eqinfo: QUERY_EQ did not answer (fw status 0x%x) -- unsupported "
                "or unavailable for this EQ; recorded, not assumed", (unsigned)fwStatus);
        }
        out[kEqInfoOutFwStatus] = fwStatus;
    }

    out[kEqInfoOutStatus] = 0;
    return kIOReturnSuccess;
}

IOReturn Cx3Mlx4::exportM3EqPoll(const UInt64 *in, UInt64 *out)
{
    UInt32 fireNop  = (UInt32)in[kEqPollInFireNop];
    UInt32 token    = (UInt32)in[kEqPollInToken];
    UInt32 consFrom = (UInt32)in[kEqPollInConsIndex];
    UInt32 dumpW    = (UInt32)in[kEqPollInDumpDwords];
    UInt32 waitMs   = (UInt32)in[kEqPollInWaitMs];
    /*
     * peek: read the head of the queue without consuming it or moving the index.
     * Consuming is the default; the diagnostic is the opt-in.
     */
    bool   peek     = (in[kEqPollInPeek] != 0);
    UInt8  eqe[MLX4_EQ_ENTRY_SIZE];
    UInt32 i, cons = consFrom;
    UInt32 nopStatus = CX3_FWSTATUS_NOT_POSTED;

    memset(eqe, 0, sizeof(eqe));
    memset(out, 0, sizeof(UInt64) * kEqPollOutCount);
    out[kEqPollOutNopStatus] = CX3_FWSTATUS_NOT_POSTED;
    out[kEqPollOutStatus]    = (UInt64)kIOReturnError;

    if (fEqUnknownRetained) {
        LOG("m3eqpoll refused: EQN %u is retained after an ambiguous SW2HW_EQ timeout",
            (unsigned)fEqn);
        out[kEqPollOutStatus] = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }
    if (!fEqRing) {
        LOG("m3eqpoll refused: no EQ is up (run m3equp first)");
        out[kEqPollOutStatus] = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }
    if (dumpW > (UInt32)kEqPollMaxDwords) {
        out[kEqPollOutStatus] = (UInt64)kIOReturnBadArgument;
        return kIOReturnSuccess;
    }

    /* 1. Event-mode NOP.  E is set, so the HCA will answer with a CMD EQE whose
     *    token matches -- that is the difference from every command before M3. */
    if (fireNop) {
        /*
         * in_modifier 0x1f, as the reference sends it: mlx4_NOP's comment is
         * "Input modifier of 0x1f means 'finish as soon as possible.'"  The first
         * version of this sent 0, which is a difference from the one command the
         * reference uses to test exactly this path (mlx4_test_async).
         */
        UInt32 hcrPost = 0, hcrEnd = 0;
        if (!fHw.cmdEvent(0, 0, 0x1f, 0, MLX4_CMD_NOP, (UInt16)token,
                          MLX4_CMD_TIME_CLASS_A, &nopStatus, &hcrPost, &hcrEnd)) {
            out[kEqPollOutHcrOnPost] = hcrPost;
            out[kEqPollOutNopStatus] = nopStatus;
            LOG("m3eqpoll: the event-mode NOP did not complete (fw status 0x%x); "
                "there is nothing to poll for", (unsigned)nopStatus);
            return kIOReturnSuccess;
        }
        out[kEqPollOutNopStatus]  = nopStatus;
        out[kEqPollOutHcrOnPost]  = hcrPost;
    }

    /*
     * 2. Poll for it, in order, for up to waitMs.
     *
     * The consumer only ever looks at the slot the persistent index points at, and
     * stops there.  Both source reviews said the same thing about the previous
     * version, which scanned forward for any ready slot and then jumped the index
     * past it: that silently consumes every slot in between, whether or not the
     * firmware ever wrote them.  Events are delivered in order, so the head of the
     * queue is the only correct place to look, and "not ready" is the only correct
     * reason to stop.
     *
     * consFrom is now only a starting point for a read-only peek (peek != 0), which
     * does not move the persistent index at all.  Consuming always happens at the
     * persistent index, which is what keeps the two selectors from disagreeing
     * about where the queue is.
     */
    {
        UInt32 attempt = 0, c;
        bool found = false;

        for (;;) {
            c = peek ? consFrom : fEqConsIndex;
            if (fHw.eqNextEqe(fEqRing, fEqNent, fEqeFactor, c, eqe)) {
                if (eqe[MLX4_EQE_TYPE_OFFSET] == 0) {
                    /*
                     * Type 0 is a legitimate COMP event (device.h:293), not an
                     * "empty slot" in general -- but this port has no completion
                     * queues yet, so nothing here can consume it, and stepping over
                     * it is what caused the walk-past.  Reported and left in place.
                     */
                    out[kEqPollOutZeroSlot] = c + 1;
                    LOG("m3eqpoll: slot %u is ready and carries type 0x00 (a COMP "
                        "event, device.h:293). Nothing consumes completions yet, so it "
                        "is left in place rather than stepped over.", (unsigned)c);
                    break;
                }
                /*
                 * A match is only a match if the token is the one that was asked
                 * for.  The previous version treated any non-zero type as success,
                 * so a PORT_CHANGE or a completion left over from an earlier command
                 * could have been reported as this NOP's completion.
                 */
                if (fireNop &&
                    (eqe[MLX4_EQE_TYPE_OFFSET] != MLX4_EVENT_TYPE_CMD ||
                     ((((UInt32)eqe[MLX4_EQE_CMD_TOKEN_OFFSET] << 8) |
                       (UInt32)eqe[MLX4_EQE_CMD_TOKEN_OFFSET + 1]) != token))) {
                    LOG("m3eqpoll: slot %u holds type 0x%02x but that is not this "
                        "NOP's token 0x%x; leaving it in place", (unsigned)c,
                        (unsigned)eqe[MLX4_EQE_TYPE_OFFSET], (unsigned)token);
                    out[kEqPollOutZeroSlot] = c + 1;
                    break;
                }
                cons = c;
                found = true;
                break;
            }
            if (attempt >= waitMs) {
                LOG("m3eqpoll: slot %u is not consumable after %u ms; stopping at the "
                    "head of the queue (the HCA still owns it, or nothing has been "
                    "delivered)", (unsigned)c, (unsigned)attempt);
                break;
            }
            IOSleep(1);
            ++attempt;
        }
        if (!found) {
            reportOwnerMap(out, kEqPollOutOwnerSet32, kEqPollOutOwner0);
            out[kEqPollOutConsIndex] = fEqConsIndex;
            out[kEqPollOutStatus] = 0;
            return kIOReturnSuccess;
        }
    }

    /* Advance the consumer index so the next poll starts where this one left off,
     * and re-arm -- the reference does the same after consuming (mlx4_eq.c:838). */
    /*
     * Only a real consumption moves the queue.  A peek (peek != 0) reads the ring
     * without changing anything, which is what makes "look at slot 0 again" a safe
     * diagnostic instead of an action that loses events.
     */
    if (!peek) {
        /*
         * Under fEqLock, and this selector did not use to take it -- while the interrupt
         * action does, and the two consume the SAME index.  That is a defect with a name in this
         * project's taxonomy (docs/DEFECTS.md C: resource lifecycle and ordering) and its
         * symptom is the one this chain has been chasing in the EQ since restart 1: the poll
         * reads the index and the EQE, the interrupt path consumes that same EQE and advances
         * the index, and the poll then writes `cons + 1` -- an index the interrupt path has
         * already passed.  The queue is re-read from an older position, so one event is
         * delivered twice and the re-arm can be lost entirely.
         *
         * The index is therefore RE-READ once the lock is held: if it has moved, this poll's
         * `cons` is stale and consuming at it would walk the queue backwards.  That case is
         * reported and the consumption skipped -- the event is not lost, because whoever moved
         * the index consumed it.
         *
         * Nothing sleeps inside the lock: this is the whole critical section.
         */
        if (fEqLock) IOLockLock(fEqLock);
        if (fEqConsIndex == cons) {
            fEqConsIndex = cons + 1;
            ++fPollEqeCount;
            fHw.eqSetConsumerIndex(fEqDoorbell, fEqConsIndex, true);
        } else {
            LOG("m3eqpoll: the interrupt path consumed while this selector was reading (the "
                "index was %u, now %u), so this poll's finding is stale and the index is left "
                "where the interrupt path put it. Without this check the next write would have "
                "moved the queue backwards to %u, re-delivering the event it just consumed",
                (unsigned)cons, (unsigned)fEqConsIndex, (unsigned)(cons + 1u));
            out[kEqPollOutStatus] = (UInt64)kIOReturnBusy;
            if (fEqLock) IOLockUnlock(fEqLock);
            return kIOReturnSuccess;
        }
        if (fEqLock) IOLockUnlock(fEqLock);
    } else {
        LOG("m3eqpoll: peek mode -- consumer index left at %u", (unsigned)fEqConsIndex);
    }

    /*
     * Report what was found.  These assignments went missing for three rounds --
     * an earlier edit added them and a later pattern-based edit silently removed
     * them again -- and the effect was exactly backwards from the truth: the kernel
     * found a complete CMD EQE (its own log said so) while the tool was told
     * "none", because every field below was still zero from the memset.  The
     * kernel log's "type 0x00" was that same hole, not the EQE.
     *
     * The zero-type check just above reads the local buffer rather than this array,
     * which is why it did not fire and why the found path was taken.
     */
    out[kEqPollOutFound]     = 1;
    out[kEqPollOutFoundType] = eqe[MLX4_EQE_TYPE_OFFSET];
    if (eqe[MLX4_EQE_TYPE_OFFSET] == MLX4_EVENT_TYPE_CMD) {
        out[kEqPollOutFoundToken] = ((UInt32)eqe[MLX4_EQE_CMD_TOKEN_OFFSET] << 8) |
                                    (UInt32)eqe[MLX4_EQE_CMD_TOKEN_OFFSET + 1];
        out[kEqPollOutCmdStatus]  = eqe[MLX4_EQE_CMD_STATUS_OFFSET];
    }
    out[kEqPollOutNextCons]  = cons + 1;
    /*
     * The index the EQE was actually found at.  It is the head of the queue for a
     * real consumption, but a peek can look anywhere, and reporting it is what lets
     * the caller check the ordering arithmetic.  This was the fourth field in this
     * function to go unassigned on one path while the kernel log had the right
     * value -- the log showed next-cons 2 and 3 while the output still said index 0.
     */
    out[kEqPollOutConsIndex] = cons;
    {
        UInt32 k;
        for (k = 0; k < dumpW && k < (UInt32)kEqPollMaxDwords; ++k) {
            out[kEqPollOutData0 + k] = ((UInt32)eqe[k * 4] << 24) |
                                       ((UInt32)eqe[k * 4 + 1] << 16) |
                                       ((UInt32)eqe[k * 4 + 2] << 8) |
                                       (UInt32)eqe[k * 4 + 3];
        }
    }
    reportOwnerMap(out, kEqPollOutOwnerSet32, kEqPollOutOwner0);

    out[kEqPollOutStatus] = 0;
    LOG("m3eqpoll: EQE at consumer index %u: type 0x%02x (%s), token 0x%x, "
        "status 0x%x -- next consumer index %u",
        (unsigned)cons, (unsigned)out[kEqPollOutFoundType],
        (out[kEqPollOutFoundType] == MLX4_EVENT_TYPE_CMD) ? "CMD" :
        (out[kEqPollOutFoundType] == MLX4_EVENT_TYPE_PORT_CHANGE) ? "PORT_CHANGE" : "other",
        (unsigned)out[kEqPollOutFoundToken], (unsigned)out[kEqPollOutCmdStatus],
        (unsigned)fEqConsIndex);
    return kIOReturnSuccess;
}
/*
 * kCx3SelR4Tx -- R4 stage 6: the transmit side's resources, and the number space they must
 * stay out of.
 *
 * WHAT THIS IS NOT.  Nothing is posted and no doorbell is rung: no frame leaves the card, so
 * the plan's own words apply -- "resources built; that is not yet a successful transmission".
 * Everything below is a measurement about objects that exist and numbers that are where they
 * were put.
 *
 * WHAT IT BUILDS, in order:
 *
 *   1. a TX completion queue, through the same cqCreate stage 2 used (same 64-byte CQEs, same
 *      comp_eqn -- the one EQ this card has -- and the same runtime UAR index, because a netdev
 *      has one UAR for all its queues);
 *   2. the send queue's memory: 256 TXBBs of WQE array and their MTT entries
 *      (Mlx4Hw::sqCreate), which is what the QP's context will name -- with WQE 0 at the
 *      allocation's first byte, because a send queue has no 64-byte unit in front of its
 *      WQEs the way the receive ring does (mlx4_en_tx.c:139/:686 against
 *      mlx4_en_rx.c:406/:482-489; docs/DEFECTS.md E13);
 *   3. a send QP walked RST -> INIT -> RTR -> RTS with the reference's own three opcodes, each
 *      step followed by its own QUERY_QP so "the command was accepted" and "the state is what
 *      was written" are two separate readings.
 *
 * THE NUMBER SPACE, which is this stage's only constraint from outside itself.  A0 maps MAC
 * table index i to `base_qpn + i` (mlx4_en_netdev.c:573-577), the base is a parameter this
 * driver hands the firmware with SET_PORT qpn_calc (mlx4_port.c:1578-1600), and this project's
 * non-promiscuous route puts that base at `num_qps - (1 << log_num_macs)` so the 128
 * MAC-derived QPNs land on the top of the space instead of on our queues
 * (docs/M2B-NOTES.md section 41.5, option R).  Placing anything at or above that line here
 * would block that route before it is ever tried, so both of this stage's numbers are checked
 * against it and the line itself is computed from two READ values -- the profile's log_num_qps
 * and QUERY_PORT's log_max_macs -- never from 1024 and 128.
 *
 * The rest of the "check yourself first" list is in the fields it writes: sq_size_stride is
 * the send queue's own 0x42 (256 TXBBs of the 64-byte unit, mlx4_en_resources.c:54-57) where the
 * receive QP has the dummy 0x02 and the RQ has 0x40; rq_size_stride is deliberately 0; the UAR
 * is the runtime index; db_rec_addr is the UNSHIFTED record address, as stage 4 sends and
 * unlike the indirection QP of stage 5; and param3's FCS_KEEP bit is not set, because the
 * reference sets it on the receive side only (mlx4_en_rx.c:950-951).
 */
IOReturn Cx3Mlx4::exportR4Tx(const UInt64 *in, Cx3R4TxResult *res, UInt64 *out, UInt32 port)
{
    if (port<1 || port>2 || !res || !out || !in) return kIOReturnBadArgument;
    for (UInt32 i=0;i<kR4TxInCount;++i)
        if (in[i]>0x7fffffffull) return kIOReturnBadArgument;
    Cx3PortQueues &queue = fPortQueues[port-1];
    if ((port==2 && !queue.numbersReserved) || queue.setupFailed ||
        (queue.numbersReserved && queue.numberGeneration!=fDeviceGeneration)) {
        memset(res,0,sizeof(*res)); res->status=(UInt32)kIOReturnNotReady;
        memset(out,0,sizeof(UInt64)*kR4TxOutCount);
        out[0]=(UInt64)kIOReturnNotReady; return kIOReturnSuccess;
    }

    UInt32 cqNent = (UInt32)in[kR4TxInCqNent];
    UInt32 sqNent = (UInt32)in[kR4TxInSqNent];
    UInt32 fwStatus = CX3_FWSTATUS_NOT_POSTED;
    UInt32 i, macSlots;
    UInt32 cqPages = 0, cqMttOff = 0, sqPages = 0, sqMttOff = 0, sqStamp = 0, sqStamped = 0;
    UInt32 sqNonZero = 0;
    UInt32 cqMttAddr = 0, sqMttAddr = 0;
    UInt64 cqRecPhys = 0, sqPhys = 0, recPhys = 0;
    Mlx4Icm *cqRing = 0, *cqRec = 0, *sq = 0, *rec = 0;
    UInt8 context[QP_CONTEXT_SIZE];
    Cx3QueryPortResult qp;
    bool fcsKeep = false;
    static const UInt16 stepOp[3] = { MLX4_CMD_RST2INIT_QP, MLX4_CMD_INIT2RTR_QP,
                                      MLX4_CMD_RTR2RTS_QP };
    static const UInt32 stepState[3] = { MLX4_QP_STATE_INIT, MLX4_QP_STATE_RTR,
                                         MLX4_QP_STATE_RTS };
    /*
     * counter_index = 0xFF, the reference's way of saying "no counter"
     * (mlx4_en_resources.c:66, whose guard on the next line is written as `counter_index !=
     * 0xFF`).  Same value and same reason as stages 4 and 5: this driver has reserved none.
     */
    const UInt32 counterIndex = 0xffu;

    memset(out, 0, sizeof(UInt64) * kR4TxOutCount);
    memset(res, 0, sizeof(*res));
    memset(&qp, 0, sizeof(qp));
    res->status = (UInt32)kIOReturnError;
    /*
     * feup starts at the sentinel, not at zero, and that is the D-family rule rather than a
     * formality: the fill below is reached only after the CQ and the send queue are built, and
     * every refusal before it would otherwise publish "the byte we wrote is 0" -- a value this
     * driver never wrote, presented as a measurement.  The struct's other fields may be zero
     * because a zero there means something; here it does not.
     */
    res->feupSent = CX3_R4TX_NOT_MEASURED;
    /* Every measured scalar starts as "never reached" -- see r4TxPublishScalars. */
    for (i = 0; i < (UInt32)kR4TxOutCount; ++i) out[i] = CX3_R4TX_NOT_MEASURED;
    out[kR4TxOutStatus] = res->status;

    if (cqNent == 0) cqNent = CX3_R4TX_DEFAULT_CQ_NENT;
    if (sqNent == 0) sqNent = CX3_R4TX_DEFAULT_SQ_TXBB;
    /*
     * Both counts are rounded up to a power of two HERE rather than inside the two creation
     * helpers, because the number that matters is the one the context and the readiness rule
     * use: sq_size_stride encodes ilog2(entries) and the CQ's phase rule is `cons_index &
     * nent`.  Rounding them in one place means the value reported, the value in the context and
     * the value the scans use are the same number -- a caller passing 1000 gets 1024 everywhere
     * rather than 1024 in the encoding and 1000 in the check.
     */
    { UInt32 n = 1; while (n < cqNent) n <<= 1; cqNent = n; }
    { UInt32 n = 1; while (n < sqNent) n <<= 1; sqNent = n; }
    res->cqNent = cqNent;
    res->sqNent = sqNent;
    res->sqStride = MLX4_TXBB_SIZE;
    /*
     * 0, and a fact rather than a placeholder: this queue's WQE 0 is at its MTT base
     * (mlx4_en_tx.c:139, :686) while the receive ring's WQE 0 is 64 bytes past it
     * (mlx4_en_rx.c:406, :482-489).  Reported rather than assumed, because 0.43.7 built the
     * send queue with the receive ring's 64 and no descriptor was ever executed
     * (docs/DEFECTS.md E13).
     */
    res->sqHeaderBytes = SQ_WQE0_OFFSET;

    if (queue.txSqUnknownRetained) {
        LOG("r4tx refused: an unavailable pre-handoff SQ owner is retained; teardown must "
            "successfully retry its MTT release before another TX setup");
        res->status = (UInt32)kIOReturnNotReady;
        out[kR4TxOutStatus] = res->status;
        return kIOReturnSuccess;
    }

    /*
     * The entrance conditions, named rather than assumed.  Each of the three earlier stages is
     * needed for a different reason, and the last is needed for the NUMBER rather than for the
     * object.
     */
    if (!fHcaInited) {
        LOG("r4tx refused: the HCA is not initialised (run m2dprofile/m2dbringup first)");
        res->status = (UInt32)kIOReturnNotReady;
        out[kR4TxOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if (fMrState != 1) {
        LOG("r4tx refused: no memory region is enabled (state %u). The send queue's WQEs would "
            "carry its lkey, and this stage reports the same PD the earlier stages use, so "
            "stage 1 has to have succeeded.", (unsigned)fMrState);
        res->status = (UInt32)kIOReturnNotReady;
        out[kR4TxOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if (!queue.cqUp) {
        LOG("r4tx refused: no completion queue exists (stage 2). This stage takes its UAR index "
            "and its ceiling-of-comparison from that queue, and the reference gives a netdev "
            "ONE UAR for all of its queues, so there is nothing to reuse otherwise.");
        res->status = (UInt32)kIOReturnNotReady;
        out[kR4TxOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if (!queue.qpUp) {
        LOG("r4tx refused: the RX QP of stage 4 does not exist, so this call has no number to "
            "place the send QP above and nothing to compare it against (run r4qp first)");
        res->status = (UInt32)kIOReturnNotReady;
        out[kR4TxOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if (!queue.ingressUp) {
        LOG("r4tx refused: the indirection QP of stage 5 does not exist, and the number this "
            "stage picks is derived from the two QPs that DO exist -- with one of them missing, "
            "'the next number above them' would silently land on it (run r4ingress first)");
        res->status = (UInt32)kIOReturnNotReady;
        out[kR4TxOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    /*
     * This selector is NOT idempotent, and that is a decision rather than an omission.
     *
     * Stages 2, 4 and 5 all have an "it already exists, so measure it again" branch, because
     * for those the second call is a second READING of the same object: the same CQN, the same
     * QPN, the ring still there.  Here the second call would be a second CREATION -- a second
     * send queue's memory, which the first QP's context does not name -- and the only way to
     * tell the two apart afterwards would be the driver's own bookkeeping.  What the firmware
     * would say is blunter: SW2HW_CQ for a CQN it already owns fails, and the failure would
     * look like a defect rather than like "you ran it twice".
     *
     * The verification chain does not hit this: `verify.sh run` begins with an HCA reset, whose
     * teardown path calls txTeardown, so the objects are gone before the next r4tx.
     */
    if (queue.txCqUp || queue.txQpUp || queue.txQpRec) {
        LOG("r4tx refused: this boot already has a transmit side -- TX CQN %u (%s), TX QPN %u "
            "(state last attempted %u, %s). This selector builds objects rather than re-reading "
            "them, so a second call would allocate a SECOND send queue the first QP does not "
            "name; it is deliberately not idempotent. Reset the HCA, restart the driver, or "
            "reboot -- txTeardown runs on all three.",
            (unsigned)queue.txCqn, queue.txCqUp ? "up" : "not up", (unsigned)queue.txQpn,
            (unsigned)queue.txQpState,
            (queue.txQpUp || queue.txQpRec) ? "may be owned by the firmware" : "not built");
        res->status = (UInt32)kIOReturnExclusiveAccess;
        out[kR4TxOutStatus] = res->status;
        return kIOReturnSuccess;
    }

    /*
     * The line the top 128 QPNs start at, from two runtime values.
     *
     * num_qps comes from the profile this driver itself submitted (fInitHca.log_num_qps), not
     * from the card's capability -- the distinction stage 2 already recorded: a number the
     * configured cMPT has no entry for is a different thing from a number the silicon could
     * hold.  log_max_macs comes from QUERY_PORT's outbox byte 0x0a, low nibble
     * (QUERY_PORT_MAX_MACVLAN_OFFSET = 0x0a, mlx4_fw.c:1206; decoded at :1234-1236), which this
     * port already carries as Cx3QueryPortResult::logMaxMacs.
     *
     * If the two cannot produce a usable line -- a MAC count that alone fills the space -- the
     * honest answer is to refuse, because "the top 128 are reserved" would be false and the
     * plan's option R would be impossible on this configuration.
     */
    if (!fHw.queryPort(port, &qp)) {
        LOG("r4tx: QUERY_PORT on port %u failed (fw status 0x%x), so log_max_macs -- half of "
            "the arithmetic that keeps this stage's numbers out of the MAC-derived range -- is "
            "unknown, and this driver will not guess it. Nothing was sent.",
            (unsigned)port, (unsigned)qp.fwStatus);
        res->status = (UInt32)kIOReturnError;
        out[kR4TxOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    res->logMaxMacs = qp.logMaxMacs;
    res->numQps = (fInitHca.log_num_qps < 32u) ? (1u << fInitHca.log_num_qps) : 0xffffffffu;
    res->qpnProfileMax = res->numQps;
    macSlots = (res->logMaxMacs < 32u) ? (1u << res->logMaxMacs) : 0xffffffffu;
    if (macSlots >= res->numQps) {
        LOG("r4tx refused: QUERY_PORT says log_max_macs %u (%u MAC-derived QPNs) and the profile "
            "configured %u QPs, so there is no room to keep the MAC range off this driver's "
            "queues -- the whole point of the top-reserved rule (docs/M2B-NOTES.md section "
            "41.4-41.5). Nothing was sent: raising the profile's QP count or lowering the MAC "
            "count is the fix, not placing a TX QP in the range.",
            (unsigned)res->logMaxMacs, (unsigned)macSlots, (unsigned)res->numQps);
        res->status = (UInt32)kIOReturnNoSpace;
        out[kR4TxOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    res->topReservedStart = res->numQps - macSlots;
    LOG("r4tx: the QPN space is %u (the profile's log_num_qps %u) and A0's MAC table maps index "
        "i to base_qpn + i with log_max_macs %u, so %u QPNs (0x%x..0x%x) are kept EMPTY: the "
        "plan's non-promiscuous route puts the MAC-derived base at num_qps - (1 << log_max_macs) "
        "= %u (docs/M2B-NOTES.md section 41.5, option R). This stage's numbers are chosen below "
        "that line, and both are checked against it.",
        (unsigned)res->numQps, (unsigned)fInitHca.log_num_qps, (unsigned)res->logMaxMacs,
        (unsigned)macSlots, (unsigned)res->topReservedStart,
        (unsigned)(res->numQps - 1u), (unsigned)res->topReservedStart);

    /*
     * The two numbers, each derived from something that exists rather than written down.
     *
     * The QPN is "one above the highest number the earlier stages hold", which is the same
     * shape stage 5 used for its own (+1) and for the same reason: this port has no range
     * allocator, so the derivation is stated rather than hidden, and both existing numbers are
     * compared against the result afterwards.
     *
     * The CQN is "one above the RX CQ's", for the same reason and with the note that the CQN
     * space is separate from the QPN space in the hardware -- the comparison against
     * topReservedStart below is applied to it anyway, because the plan's constraint is stated
     * for both and a number this stage chose should not be the one that breaks it.
     */
    res->qpnRule    = fDevCap.reserved_qps;
    res->rxQpn      = queue.qpQpn;
    res->ingressQpn = queue.ingressQpn;
    res->rxCqn      = queue.cqCqn;
    res->qpn = queue.numbersReserved ? queue.numbers.txQpn :
        ((res->rxQpn > res->ingressQpn) ? res->rxQpn : res->ingressQpn) + 1u;
    res->cqn = queue.numbersReserved ? queue.numbers.txCqn : res->rxCqn + 1u;
    res->qpnCollidesRx      = (res->qpn == res->rxQpn) ? 1u : 0u;
    res->qpnCollidesIngress = (res->qpn == res->ingressQpn) ? 1u : 0u;
    res->cqnCollidesRx      = (res->cqn == res->rxCqn) ? 1u : 0u;
    /*
     * The two "is it below the line" answers are MEASURED here, before the refusals, because
     * they are two of the four numbers the scalars report and the tool prints them: a field
     * left at its memset zero would make the tool say "both below it: NO" on a run that
     * passed, which is a statement about nothing (the defect family this project has the most
     * rows in).
     */
    res->qpnBelowTop = (res->qpn < res->topReservedStart) ? 1u : 0u;
    res->cqnBelowTop = (res->cqn < res->topReservedStart) ? 1u : 0u;

    if (res->qpn < fDevCap.reserved_qps) {
        LOG("r4tx refused: QPN %u is inside the firmware's reserved range (0..%u)",
            (unsigned)res->qpn,
            (unsigned)(fDevCap.reserved_qps ? fDevCap.reserved_qps - 1u : 0u));
        res->status = (UInt32)kIOReturnBadArgument;
        out[kR4TxOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if (res->qpn >= res->numQps) {
        LOG("r4tx refused: QPN %u is at or past the %u QPs this profile configured "
            "(log_num_qps %u); the card's ceiling is a different number and not what was "
            "configured", (unsigned)res->qpn, (unsigned)res->numQps,
            (unsigned)fInitHca.log_num_qps);
        res->status = (UInt32)kIOReturnBadArgument;
        out[kR4TxOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if (res->qpn >= res->topReservedStart || res->cqn >= res->topReservedStart) {
        LOG("r4tx refused: QPN %u / CQN %u is at or above the top-reserved line %u "
            "(num_qps %u - %u MAC-derived QPNs). Those slots are kept empty on purpose: A0's "
            "MAC table index i maps to base_qpn + i, and the plan puts that base at this line "
            "so the MAC range never lands on a queue this driver owns (docs/M2B-NOTES.md "
            "section 41.4-41.5). Lowering the numbers is the fix -- or a profile with more QPs "
            "-- not moving the line.", (unsigned)res->qpn, (unsigned)res->cqn,
            (unsigned)res->topReservedStart, (unsigned)res->numQps, (unsigned)macSlots);
        res->status = (UInt32)kIOReturnNoSpace;
        out[kR4TxOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if (res->cqn < fDevCap.reserved_cqs) {
        LOG("r4tx refused: CQN %u is inside the firmware's reserved range (0..%u)",
            (unsigned)res->cqn, (unsigned)(fDevCap.reserved_cqs - 1u));
        res->status = (UInt32)kIOReturnBadArgument;
        out[kR4TxOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if (res->cqn >= (1u << fInitHca.log_num_cqs)) {
        LOG("r4tx refused: CQN %u is at or past the %u CQs this profile configured",
            (unsigned)res->cqn, (unsigned)(1u << fInitHca.log_num_cqs));
        res->status = (UInt32)kIOReturnBadArgument;
        out[kR4TxOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if (res->qpnCollidesRx || res->qpnCollidesIngress || res->cqnCollidesRx) {
        /*
         * Cannot happen with the derivation above, which is exactly why it is checked: the check
         * is on the RESULT, not on the intent, so a later change to the derivation cannot
         * quietly produce two objects with one number.  A collision here would reach RTS and
         * read back clean while the two queues shared a context.
         */
        LOG("r4tx refused: a number collides with an existing object -- QPN %u against the RX "
            "QPN %u (%u) and the indirection QPN %u (%u), CQN %u against the RX CQN %u (%u). "
            "Nothing was sent.", (unsigned)res->qpn, (unsigned)res->rxQpn,
            (unsigned)res->qpnCollidesRx, (unsigned)res->ingressQpn,
            (unsigned)res->qpnCollidesIngress, (unsigned)res->cqn, (unsigned)res->rxCqn,
            (unsigned)res->cqnCollidesRx);
        res->status = (UInt32)kIOReturnNoSpace;
        out[kR4TxOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    res->numberSafe = (res->qpnBelowTop && res->cqnBelowTop &&
                       !res->qpnCollidesRx && !res->qpnCollidesIngress &&
                       !res->cqnCollidesRx) ? 1u : 0u;
    /*
     * The safety bit is printed with its meaning attached, because the previous wording was a
     * defect of the text rather than of the value: it read "1 x the numbers are safe", which
     * "%u" filled with the value 1 and "x" made look like a count of something that had been
     * checked once.  docs/M2B-NOTES.md section 42.4 recorded it as the F family's shape (the
     * text is the defect) and this batch fixes it (docs/DEFECTS.md F10 is the same lesson one
     * stage earlier).  The value itself is also a scalar, and tools/verify.sh asserts on it, so
     * the sentence is not the evidence -- but a sentence that reads as a different claim is
     * how a correct number sends its reader after the wrong thing.
     */
    LOG("r4tx: QPN %u (one above the RX QP %u and the indirection QP %u) and CQN %u (one "
        "above the RX CQN %u), both below the top-reserved line %u -- the top %u slots "
        "(0x%x..0x%x) stay empty for A0's MAC-derived range. The safety bit is %u -- 1 means "
        "safe and 0 means not, so read it as a value and not as a count of checks. It is "
        "computed from the four comparisons against the top-reserved line, which comes from "
        "QUERY_PORT's log_max_macs %u and the profile's %u QPs.",
        (unsigned)res->qpn, (unsigned)res->rxQpn, (unsigned)res->ingressQpn,
        (unsigned)res->cqn, (unsigned)res->rxCqn, (unsigned)res->topReservedStart,
        (unsigned)macSlots, (unsigned)res->topReservedStart, (unsigned)(res->numQps - 1u),
        (unsigned)res->numberSafe, (unsigned)res->logMaxMacs, (unsigned)res->numQps);

    /* ------------------------------------------------------------------ */
    /* 1. the TX completion queue                                          */
    /* ------------------------------------------------------------------ */

    /*
     * The same code path stage 2 used (Mlx4Hw::cqCreate), with two differences and no others:
     * the number, and the fact that the UAR is TAKEN FROM the existing RX CQ rather than
     * derived again.  The reference gives a netdev one priv_uar for all of its queues
     * (mlx4_en_resources.c:58), so sharing the index is what the shipping driver does -- and
     * deriving it a second time would be a second chance to disagree.
     */
    res->uarIndex = queue.cqUarStart;
    if (!fHw.cqCreate(res->cqn, cqNent, (UInt8)fEqn, res->uarIndex, &cqRing, &cqPages,
                      &cqMttOff, &cqMttAddr, &cqRecPhys, &cqRec, 0, 0, &fwStatus)) {
        res->cqFwStatus = fwStatus;
        LOG("r4tx: SW2HW_CQ for the send queue's CQN %u did NOT succeed (fw status 0x%x). "
            "Stopping here: the send QP's context would name this CQ in cqn_send and cqn_recv, "
            "and a QP whose completions have nowhere to go is worse than no QP. Nothing was "
            "freed (cqCreate deliberately leaks on a timeout, because a timeout does not prove "
            "the context was rejected).", (unsigned)res->cqn, (unsigned)fwStatus);
        res->status = (UInt32)kIOReturnError;
        r4TxPublishScalars(res, out, false);
        out[kR4TxOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    res->cqFwStatus     = fwStatus;
    res->cqPages        = cqPages;
    res->cqMttOffset    = cqMttOff;
    res->cqMttAddr      = cqMttAddr;
    res->cqDbRecordPhys = cqRecPhys;
    /*
     * The check stage 3 onward depends on, run here for the second queue: how many slots does
     * the consumer's own readiness rule find, from cons_index 0, without consuming anything?
     * Zero is the only correct answer for a queue with no completions.  Anything else means a
     * slot would be read as a completion out of memory nobody wrote -- the defect the owner
     * preset exists to prevent, and this is the measurement that says whether it worked.
     */
    {
        UInt8 cqe[CQE_LEGACY_SIZE];
        UInt32 ci = 0, ready = 0;
        while (ready < cqNent) {
            memset(cqe, 0, sizeof(cqe));
            if (!fHw.cqNextCqe(cqRing, cqNent, /*cqeFactor=*/1, ci, cqe)) break;
            ++ready; ++ci;
        }
        res->cqReadyAtStart = ready;
        LOG("r4tx: the send CQ (CQN %u, %u entries, %u pages, record 0x%llx) finds %u readable "
            "slot(s) at cons_index 0%s", (unsigned)res->cqn, (unsigned)cqNent,
            (unsigned)cqPages, (unsigned long long)cqRecPhys, (unsigned)ready,
            ready ? " -- EXPECTED 0: a queue with no completions must read as empty"
                  : " (correct: an empty queue reads as empty)");
    }
    /*
     * The CQ is DURABLE from here on, before anything else can fail.
     *
     * It has been handed to the firmware (SW2HW_CQ succeeded), so it is a device object now:
     * if a later step of this call refuses, a CQ left in the firmware's hands with nothing
     * remembering it would make the next call's "the RX CQN + 1" collide with a queue nobody
     * can hand back.  Storing it here -- rather than in the success path at the bottom -- is
     * what makes every refusal below leave the driver able to take it back (txTeardown).
     */
    queue.txCqRing     = cqRing;
    queue.txCqNent     = cqNent;
    queue.txCqn        = res->cqn;
    queue.txCqMttOff   = cqMttOff;
    queue.txCqMttPages = cqPages;
    queue.txCqHwReturned = false;
    queue.txCqRec      = cqRec;
    queue.txCqUp       = true;

    /* ------------------------------------------------------------------ */
    /* 2. the send queue's memory                                          */
    /* ------------------------------------------------------------------ */

    if (!fHw.sqCreate(sqNent, &sq, &sqPages, &sqMttOff, &sqMttAddr, &sqPhys, 0, &sqStamp,
                      &sqStamped, &sqNonZero)) {
        if (sq && sqPages != 0u) {
            queue.txSq = sq;
            queue.txSqNent = sqNent;
            queue.txSqPages = sqPages;
            queue.txSqMttOff = sqMttOff;
            queue.txSqMttPages = sqPages;
            queue.txSqMttAddr = sqMttAddr;
            queue.txSqPhys = sqPhys;
            queue.txSqUnknownRetained = true;
            LOG("r4tx: SQ creation stopped with a retained pre-handoff ring/MTT owner; "
                "TX teardown can retry the exact release, but no TX setup may consume it");
        }
        LOG("r4tx: the send queue's memory could not be built; the transmit side stops here "
            "(the CQ above is left in place and reported, because it is a complete object and "
            "its own evidence is worth having from the same run)");
        res->status = (UInt32)kIOReturnError;
        r4TxPublishScalars(res, out, false);
        out[kR4TxOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    res->sqBytes           = sqNent * MLX4_TXBB_SIZE;
    res->sqPages           = sqPages;
    res->sqMttOffset       = sqMttOff;
    res->sqMttAddr         = sqMttAddr;
    res->sqPhysFirst       = sqPhys;
    res->sqStampValue      = sqStamp;
    res->sqStampMatches    = (sqStamp == MLX4_SQ_INIT_OWNER_BIT) ? 1u : 0u;
    res->sqTxbbsStamped    = sqStamped;
    res->sqWqeNonZeroBytes = sqNonZero;
    res->sqWqeUntouched    = (sqNonZero == 0) ? 1u : 0u;
    /*
     * ...and the send queue's memory is durable from here too, for the same reason as the CQ
     * above: the refusals below (the size/stride cross-check and the MTT address check) can
     * still stop the call, and what has been allocated has to be reachable by txTeardown
     * rather than dropped on the floor.
     */
    queue.txSq        = sq;
    queue.txSqNent    = sqNent;
    queue.txSqPages   = sqPages;
    queue.txSqMttOff  = sqMttOff;
    queue.txSqMttPages = sqPages;
    queue.txSqMttAddr = sqMttAddr;
    queue.txSqPhys    = sqPhys;
    if (!res->sqStampMatches) {
        LOG("r4tx: WQE 0's first dword (the allocation's first four bytes) reads back 0x%08x, "
            "not the transmit stamp with owner = 1 (0x%08x, i.e. STAMP_VAL | 1 << STAMP_SHIFT). "
            "Every 64-byte unit of this memory carries that value because it is what 'software "
            "owns this slot' looks like; a unit whose first dword does not say it is a unit the "
            "send path may read as in flight", (unsigned)sqStamp,
            (unsigned)MLX4_SQ_INIT_OWNER_BIT);
    }
    if (res->sqTxbbsStamped != sqNent) {
        LOG("r4tx: only %u of the %u WQE TXBBs carry the ownership stamp (0x%08x) although the "
            "reference stamps every one of them (mlx4_en_netdev.c:1373-1375). A partially "
            "stamped array is the shape docs/DEFECTS.md C12 had: the slots that are not stamped "
            "read as hardware-owned, i.e. in flight, to the transmit path's own reclaim scan",
            (unsigned)sqStamped, (unsigned)sqNent, (unsigned)MLX4_SQ_INIT_OWNER_BIT);
    }
    if (!res->sqWqeUntouched) {
        /*
         * Report it as what it is rather than as a failure: this stage posts nothing, so a
         * non-zero byte in the WQE array outside the ownership stamps means something wrote
         * there that this driver did not intend -- worth knowing, and worth not confusing with
         * "the SQ is unusable".
         */
        LOG("r4tx: the send queue's WQE array holds %u non-zero byte(s) although this stage "
            "posts nothing. Expected 0 -- the count excludes each TXBB's 4-byte ownership stamp, "
            "which is written on purpose. The array is where stage 9 will write descriptors; a "
            "byte here now was not written by the transmit path, because there is none yet",
            (unsigned)sqNonZero);
    }

    /* ------------------------------------------------------------------ */
    /* 3. the send QP: one context, three states                           */
    /* ------------------------------------------------------------------ */

    /*
     * The db_rec_addr record.
     *
     * WHY IT IS UNSHIFTED HERE, and this is the one place this stage deliberately does not do
     * what the reference's shared fill would do.  mlx4_en_fill_qp_context writes
     * `priv->res.db.dma << 2` (mlx4_en_resources.c:78) for BOTH kinds of queue; the RX path then
     * overwrites it with the unshifted address of the ring's own record (mlx4_en_rx.c:947), and
     * stage 4 sends that unshifted value.  Nothing overwrites it for a TX ring, so the shipping
     * driver does send the shifted form there.  This port sends the unshifted address of a
     * record of its own, for two reasons and no third:
     *
     *   - the record is host memory this driver allocated and owns, and an unshifted byte
     *     address is the form the RX QP's readback showed the firmware keeping (stage 4
     *     measured db_rec_addr 0x3b77f8000 round-tripping byte for byte);
     *   - `<< 2` is a multiply by four that nothing in the vendored tree explains, and stage 5's
     *     investigation ended with "both sides are dead" for the indirection QP -- where the
     *     shifted form is harmless because that QP produces no completions.
     *
     * The honest statement of the risk: a send queue's producer index is delivered by the send
     * doorbell (MMIO), not by this record -- the reference never writes a producer into it on
     * the TX path -- so the field is expected not to be read at all.  It is still in the field
     * mask, so if the firmware returns something else the difference is visible rather than
     * assumed away.  If stage 9 cannot send, this is one of the first two things to try
     * differently; the other was the 64-byte unit this port used to put in front of the WQEs,
     * which 0.43.7 measured as the actual cause (docs/DEFECTS.md E13).
     */
    rec = fHw.icmAlloc(1, false);
    if (!rec) {
        LOG("r4tx: could not allocate the send QP's db_rec_addr record");
        res->status = (UInt32)kIOReturnNoMemory;
        r4TxPublishScalars(res, out, false);
        out[kR4TxOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    memset(rec->virt, 0, (size_t)rec->bytes);
    recPhys = rec->pages[0].busAddr;
    res->dbRecordPhys     = recPhys;
    res->dbRecAddrSent    = recPhys;      /* UNSHIFTED */
    res->dbRecAddrShifted = 0;
    res->isTxSent         = 1;
    LOG("r4tx: the send QP's db_rec_addr record is at 0x%llx and is sent UNSHIFTED -- the form "
        "the RX QP of stage 4 sends for its own (mlx4_en_rx.c:947) and NOT the reference's "
        "shared-fill value for a TX ring (mlx4_en_resources.c:78's << 2, which nothing "
        "overwrites on that path). It stays at 0: a send queue's producer index goes to the send "
        "doorbell in MMIO, not into this record, and the reference never writes one here",
        (unsigned long long)recPhys);

    res->pdn              = fMrPdn;
    res->counterIndexSent = counterIndex;
    /*
     * FCS_KEEP: the device's capability, passed in and NOT applied, because the reference sets
     * that bit only on the receive path (mlx4_en_rx.c:950-951).  What the fill produced is read
     * back out of the context image below rather than restated, so param3Sent is a fact about
     * the bytes in the mailbox.
     */
    fcsKeep = (fDevCap.flags & MLX4_DEV_CAP_FLAG_FCS_KEEP) != 0;
    fHw.qpFillContext(context, fMrPdn, res->qpn, res->uarIndex, sqNent, MLX4_TXBB_SIZE,
                      res->cqn, recPhys, MLX4_QP_MTU_MSGMAX_ETH, port, fcsKeep,
                      counterIndex, /*rss=*/false, /*isTx=*/true,
                      /*
                       * user_prio 0, and the SIGN is the point: the reference's transmit ring
                       * passes `i / priv->num_tx_rings_p_up` (mlx4_en_netdev.c:1362-1363),
                       * which is >= 0 for every ring, and the fill turns that into
                       * pri_path.feup = MLX4_FEUP_FORCE_ETH_UP (mlx4_en_resources.c:62-65).
                       * Both of the receive-side callers pass -1 instead (:946 for the ring,
                       * :1057 for the RSS indirection QP), which is why the two calls three
                       * hundred lines up carry -1 and this one does not.  A priority of 0
                       * leaves sched_queue at 0x83, so the only byte this sign changes is
                       * feup -- see MLX4_FEUP_FORCE_ETH_UP in mlx4_regs.h.
                       */
                      /*userPrio=*/0);
    res->rqSizeStrideSent = context[QP_CONTEXT_RQ_SIZE_STRIDE_OFFSET];
    res->sqSizeStrideSent = context[QP_CONTEXT_SQ_SIZE_STRIDE_OFFSET];
    res->mtuMsgmaxSent    = context[QP_CONTEXT_MTU_MSGMAX_OFFSET];
    res->schedQueueSent   = context[QP_CONTEXT_PRI_SCHED_QUEUE_OFFSET];
    res->param3Sent       = cx3GetBe32(context + QP_CONTEXT_PARAM3_OFFSET);
    /*
     * Read out of the context image that is about to be sent, like the four above it: the claim
     * is about the bytes in the mailbox, so a value taken from the buffer cannot disagree with
     * the buffer.  A literal MLX4_FEUP_FORCE_ETH_UP here would be this driver's intention,
     * which is exactly the thing the QUERY_QP readback exists to test.
     */
    res->feupSent         = context[QP_CONTEXT_PRI_FEUP_OFFSET];
    LOG("r4tx: the context as it stands in the mailbox: is_tx 1, size/stride %u/%u -> "
        "sq_size_stride 0x%02x, rq_size_stride 0x%02x, cqn_send/recv %u, usr_page %u (the "
        "runtime UAR the CQs use), db_rec_addr 0x%llx, param3 0x%08x (FCS_KEEP clear, and the "
        "device does advertise it: %u -- the reference sets that bit on the receive side only)",
        (unsigned)sqNent, (unsigned)MLX4_TXBB_SIZE, (unsigned)res->sqSizeStrideSent,
        (unsigned)res->rqSizeStrideSent, (unsigned)res->cqn, (unsigned)res->uarIndex,
        (unsigned long long)res->dbRecAddrSent, (unsigned)res->param3Sent, fcsKeep ? 1u : 0u);
    if (res->sqSizeStrideSent !=
        (UInt32)((mlx4Log2(sqNent) << MLX4_QP_RQ_STRIDE_SHIFT) |
                 (mlx4Log2(MLX4_TXBB_SIZE) - MLX4_QP_STRIDE_SUB))) {
        /*
         * Belt and braces on the field the plan names first: the byte is compared with the
         * arithmetic rather than with a literal 0x42, so a caller configuring a different TXBB
         * count still gets a derived expectation.
         */
        LOG("r4tx: REFUSING to continue: sq_size_stride 0x%02x is not ilog2(%u) << %u | "
            "(ilog2(%u) - %u) = 0x%02x", (unsigned)res->sqSizeStrideSent, (unsigned)sqNent,
            (unsigned)MLX4_QP_RQ_STRIDE_SHIFT, (unsigned)MLX4_TXBB_SIZE,
            (unsigned)MLX4_QP_STRIDE_SUB,
            (unsigned)((mlx4Log2(sqNent) << MLX4_QP_RQ_STRIDE_SHIFT) |
                       (mlx4Log2(MLX4_TXBB_SIZE) - MLX4_QP_STRIDE_SUB)));
        fHw.icmFree(rec);
        res->status = (UInt32)kIOReturnError;
        r4TxPublishScalars(res, out, false);
        out[kR4TxOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if ((res->sqMttAddr >> 32) != 0) {
        /*
         * mtt_base_addr_h is one byte, so the byte address has to fit in 32 bits.  The send
         * queue's MTT entries start where the MTT allocator put them, and this refuses rather
         * than truncating: a truncated MTT address is a queue whose descriptors come from
         * somewhere else.
         */
        LOG("r4tx: the send queue's MTT byte address 0x%llx does not fit in mtt_base_addr_l "
            "(the high byte would be 0x%02x) -- refusing rather than truncating, because a "
            "truncated MTT address is a QP whose descriptors come from somewhere else",
            (unsigned long long)res->sqMttAddr, (unsigned)(res->sqMttAddr >> 32));
        fHw.icmFree(rec);
        res->status = (UInt32)kIOReturnUnsupported;
        r4TxPublishScalars(res, out, false);
        out[kR4TxOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    context[QP_CONTEXT_LOG_PAGE_SIZE_OFFSET] = 0;      /* 4 KiB pages: 12 - 12 */
    context[QP_CONTEXT_MTT_BASE_H_OFFSET]    = 0;
    {
        UInt32 lo = (UInt32)(res->sqMttAddr & 0xffffffffull);
        context[QP_CONTEXT_MTT_BASE_L_OFFSET + 0] = (UInt8)((lo >> 24) & 0xffu);
        context[QP_CONTEXT_MTT_BASE_L_OFFSET + 1] = (UInt8)((lo >> 16) & 0xffu);
        context[QP_CONTEXT_MTT_BASE_L_OFFSET + 2] = (UInt8)((lo >>  8) & 0xffu);
        context[QP_CONTEXT_MTT_BASE_L_OFFSET + 3] = (UInt8)(lo & 0xffu);
    }
    LOG("r4tx: QPN %u, context at mailbox +%u, %u bytes; the send queue's MTT: entry %u -> byte "
        "address 0x%x (index x %u), log_page_size 0 (set for RST->INIT only)",
        (unsigned)res->qpn, (unsigned)QP_CONTEXT_MAILBOX_OFFSET, (unsigned)QP_CONTEXT_SIZE,
        (unsigned)res->sqMttOffset, (unsigned)res->sqMttAddr,
        (unsigned)MLX4_MTT_ENTRY_SIZE);

    /* Retain the complete owner BEFORE the first possible device hand-off.
     * Up means verified readiness, never merely an attempted transition. */
    queue.txQpRec = rec;
    queue.txQpRecPhys = recPhys;
    queue.txQpUp = false;

    {
        Mlx4Icm *dmaOwners[4] = { queue.txQpRec, queue.txSq, queue.txCqRing, queue.txCqRec };
        if (!fHw.markDmaHandoff(dmaOwners, 4)) {
            res->status = (UInt32)kIOReturnNotReady;
            LOG("r4tx: refusing QP transition because a referenced DMA owner has no generation");
            out[kR4TxOutStatus] = res->status;
            return kIOReturnSuccess;
        }
    }

    /* --- layer 1: the three transitions, each followed by its own query --- */
    for (i = 0; i < 3; ++i) {
        UInt32 flags;
        /*
         * The QP becomes the firmware's from the moment this driver ASKS, not from the moment
         * it succeeds, and the distinction is the difference between a leak and a device
         * writing into recycled memory.  A command that fails -- or times out -- does not prove
         * the firmware rejected the context: it may have taken the QP and lost the answer.  So
         * the number and the state being attempted are recorded HERE, before the first
         * transition, which is what gives txTeardown something to take back (2RST_QP) before it
         * releases the send queue's MTT entries and the db_rec_addr record that a partly-built
         * context already names.  If the 2RST itself fails, teardown releases nothing.
         *
         * queue.txQpState is therefore "the state last attempted", not "the state a query
         * confirmed" -- the query's answer is never stored, exactly as on the receive side.
         */
        queue.txQpn     = res->qpn;
        queue.txQpState = stepState[i];
        /* Same context, four bits different -- the reference's own walk (mlx4_qp.c:912-914). */
        flags  = cx3GetBe32(context + QP_CONTEXT_FLAGS_OFFSET);
        flags &= ~(0xfu << MLX4_QP_STATE_NIBBLE_SHIFT);
        flags |= (stepState[i] << MLX4_QP_STATE_NIBBLE_SHIFT);
        res->stepOpcode[i]      = stepOp[i];
        res->stepStateWanted[i] = stepState[i];
        context[QP_CONTEXT_FLAGS_OFFSET + 0] = (UInt8)((flags >> 24) & 0xffu);
        context[QP_CONTEXT_FLAGS_OFFSET + 1] = (UInt8)((flags >> 16) & 0xffu);
        context[QP_CONTEXT_FLAGS_OFFSET + 2] = (UInt8)((flags >>  8) & 0xffu);
        context[QP_CONTEXT_FLAGS_OFFSET + 3] = (UInt8)(flags & 0xffu);

        if (!fHw.qpModify(stepOp[i], res->qpn, context, &fwStatus)) {
            res->stepFwStatus[i]   = fwStatus;
            res->stepQueryOk[i]    = 0;
            res->stepStateAfter[i] = 0xffffffffu;
            res->statesConfirmed   = 0;
            if (fwStatus == CX3_FWSTATUS_TIMEOUT) {
                LOG("r4tx: step %u (opcode 0x%02x) TIMED OUT after being posted, so the QP's "
                    "state is UNKNOWN. Nothing is torn down and nothing is assumed: the device "
                    "may still own this QP, and 2RST_QP on a QP whose state is unknown is a "
                    "command this port does not send blind.", (unsigned)i + 1,
                    (unsigned)stepOp[i]);
            } else {
                LOG("r4tx: step %u (opcode 0x%02x, state %u) FAILED with fw status 0x%x; "
                    "stopping here. A BAD_PARAM (0x3) on the first step points at the context "
                    "itself: it belongs at mailbox +%u, and a send QP's sq_size_stride is 0x%02x "
                    "rather than the 0x02 a receive queue carries.",
                    (unsigned)i + 1, (unsigned)stepOp[i], (unsigned)stepState[i],
                    (unsigned)fwStatus, (unsigned)QP_CONTEXT_MAILBOX_OFFSET,
                    (unsigned)res->sqSizeStrideSent);
            }
            res->stepsAccepted = i;
            res->status = 0;      /* the selector answered; it was the device that refused */
            r4TxPublishScalars(res, out, false);
            out[kR4TxOutStatus] = 0;
            return kIOReturnSuccess;
        }
        res->stepFwStatus[i] = fwStatus;
        res->stepsAccepted   = i + 1;

        /*
         * The query, immediately.  A single reading of RTS at the end would be consistent with
         * "the INIT step was dropped and a later command carried the queue all the way", which
         * is a different thing from the sequence this stage claims to have performed.
         */
        {
            UInt8 q[QP_CONTEXT_SIZE];
            UInt32 qStatus = CX3_FWSTATUS_NOT_POSTED;
            memset(q, 0, sizeof(q));
            if (fHw.qpQuery(res->qpn, q, QP_CONTEXT_SIZE, &qStatus)) {
                UInt32 st = (cx3GetBe32(q + QP_CONTEXT_FLAGS_OFFSET) >>
                             MLX4_QP_STATE_NIBBLE_SHIFT) & 0xfu;
                UInt32 backQpn = cx3GetBe32(q + QP_CONTEXT_LOCAL_QPN_OFFSET);
                res->stepQueryOk[i]    = 1;
                res->stepStateAfter[i] = st;
                if (st == stepState[i] && backQpn == res->qpn)
                    res->statesConfirmed = i + 1;
                LOG("r4tx: step %u accepted (fw status 0x%x); QUERY_QP says state %u, local_qpn "
                    "%u -- %s", (unsigned)i + 1, (unsigned)fwStatus, (unsigned)st,
                    (unsigned)backQpn,
                    (st == stepState[i] && backQpn == res->qpn)
                        ? "the state and the QP number are what was written"
                        : "NOT what was written: the command was accepted and the state was not");
            } else {
                res->stepQueryOk[i]    = 0;
                res->stepStateAfter[i] = 0xffffffffu;
                LOG("r4tx: step %u returned fw status 0x%x but QUERY_QP did not answer (fw "
                    "status 0x%x), so the state after this step is UNKNOWN -- the two are "
                    "different reasons to stop", (unsigned)i + 1, (unsigned)fwStatus,
                    (unsigned)qStatus);
            }
        }
        if (res->statesConfirmed != i + 1) {
            /* A missing/contradictory readback is not permission to advance.
             * Keep QP/SQ/CQ/record until ownership can be explicitly returned. */
            res->status = (UInt32)kIOReturnNotReady;
            r4TxPublishScalars(res, out, false);
            out[kR4TxOutStatus] = res->status;
            return kIOReturnSuccess;
        }
    }

    /* --- layer 2: the last query, field by field --- */
    r4TxQueryAndCompare(fHw, res);
    res->qpnConfirmed = (res->queryOk && res->queryLocalQpn == res->qpn) ? 1u : 0u;
    if (!res->queryOk || !res->queryAllFieldsMatch || !res->queryMttMatches) {
        /*
         * The QP reached RTS but something about it is not what was written.  The objects are
         * kept and reported: the firmware accepted this context, so the difference is what the
         * next run should look at, and tearing the QP down here would remove the evidence.  The
         * verdict is in the scalars.
         */
        LOG("r4tx: the send QP reached RTS but its context is NOT what was written (mask "
            "0x%02x). The QP, the send queue's memory and the record are all KEPT for that "
            "reason.", (unsigned)res->queryFieldMask);
        res->status = (UInt32)kIOReturnNotReady;
        r4TxPublishScalars(res, out, true);
        out[kR4TxOutStatus] = res->status;
        return kIOReturnSuccess;
    }

    /*
     * Durable state, and the one place this stage's objects differ in kind from the RX side's:
     * a send queue's producer index is delivered by the send doorbell, not by the db_rec_addr
     * record, so there is no "publish the producer" step here -- and inventing one to have a
     * third layer like stage 4's would be a measurement of this driver's own write rather than
     * of the device.  What can be observed about the send queue is in the fields above: where
     * its memory is, what the context says about it, and that its WQE 0 -- the allocation's
     * first unit, with nothing in front of it -- carries the ownership stamp.
     *
     * ...and the consequence for the failure paths is the same: stage 4 rolls the QP back to RST
     * when its producer does not survive its own write, and there is no equivalent step here to
     * fail -- so a refusal above leaves the objects in place (stored as they were created) for
     * txTeardown to take back in the right order, rather than tearing the QP down from inside
     * this call.
     */
    queue.txQpUp = true;

    res->status = 0;
    out[kR4TxOutStatus] = 0;
    r4TxPublishScalars(res, out, true);
    LOG("r4tx OK: CQN %u (fw status 0x%x, %u readable slot(s) on an empty ring) and QPN %u in "
        "RTS. Commands accepted %u/3, states confirmed by QUERY_QP %u/3, the readback's eight "
        "fields %s, the MTT address %s, local_qpn %s; sq_size_stride 0x%02x, rq_size_stride "
        "0x%02x, WQE 0's stamp %s, the WQE array %s. THE TRANSMIT SIDE'S RESOURCES "
        "EXIST -- and nothing has been sent: no WQE is posted and no doorbell has been rung, so "
        "this says nothing about whether a frame can go out. That is stage 9.",
        (unsigned)res->cqn, (unsigned)res->cqFwStatus, (unsigned)res->cqReadyAtStart,
        (unsigned)res->qpn, (unsigned)res->stepsAccepted, (unsigned)res->statesConfirmed,
        res->queryAllFieldsMatch ? "all match" : "DO NOT all match",
        res->queryMttMatches ? "survived" : "DID NOT survive",
        res->qpnConfirmed ? "confirmed" : "NOT confirmed",
        (unsigned)res->sqSizeStrideSent, (unsigned)res->rqSizeStrideSent,
        res->sqStampMatches ? "reads back as STAMP_VAL|1<<31 (0xffffffff: SW owns)"
                            : "READS BACK AS SOMETHING ELSE",
        res->sqWqeUntouched ? "holds no descriptor bytes (only the ownership stamps)"
                            : "HAS BEEN WRITTEN TO");
    return kIOReturnSuccess;
}


/* ==================================================================== */
/* R4 stage 9: r4Raw -- post a WQE, ring the send doorbell, read CQEs    */
/* ==================================================================== */

/*
 * Wall clock in milliseconds.  The waits in this stage are durations (how long to keep
 * polling a completion queue), and a duration measured as "iterations of IOSleep" is the
 * defect docs/DEFECTS.md B1 and docs/LOCK-IMPL.md section 5 were both about.
 */
static UInt64 r4RawNowMs(void)
{
    UInt64 t = 0;
    clock_get_uptime(&t);
    return t / 1000000ull;
}

/* Bytes as hex, space separated, into a caller buffer of 3*n bytes.  snprintf is not in the
 * kext KPI (the build's symbol accounting refuses it), so this is the same loop the port
 * decoder uses. */
static char *r4RawHex(char *dst, const UInt8 *b, UInt32 n)
{
    static const char hexdigits[] = "0123456789abcdef";
    UInt32 i, k = 0;

    for (i = 0; i < n; ++i) {
        if (i) dst[k++] = ' ';
        dst[k++] = hexdigits[(b[i] >> 4) & 0xf];
        dst[k++] = hexdigits[b[i] & 0xf];
    }
    dst[k] = '\0';
    return dst;
}

/*
 * The frame this stage puts on the wire (decision 1 in the plan, and the constants' own
 * comment in mlx4_regs.h).  Built in one place so that "what was sent" and "what a received
 * frame is compared against" cannot be two different patterns: the payload is the driver's own
 * tag followed by a byte counter, and r4RawFrameIsOurs reads exactly those bytes.
 */
static void r4RawFillFrame(UInt8 *frame, UInt64 mac)
{
    UInt32 i;

    memset(frame, 0, CX3_R4RAW_FRAME_BYTES);
    /* destination: broadcast */
    for (i = 0; i < CX3_R4RAW_MAC_BYTES; ++i) frame[i] = 0xffu;
    /* source: this port's registered address, big-endian in the low 48 bits (the MAC table's
     * entry as the firmware holds it, not a fabricated address) */
    for (i = 0; i < CX3_R4RAW_MAC_BYTES; ++i)
        frame[CX3_R4RAW_MAC_BYTES + i] =
            (UInt8)((mac >> (8 * (CX3_R4RAW_MAC_BYTES - 1u - i))) & 0xffu);
    /* ethertype: Local Experimental 1, so nothing on the segment interprets the frame */
    frame[CX3_R4RAW_ETHERTYPE_OFFSET + 0] = (UInt8)((CX3_R4RAW_ETHERTYPE >> 8) & 0xffu);
    frame[CX3_R4RAW_ETHERTYPE_OFFSET + 1] = (UInt8)(CX3_R4RAW_ETHERTYPE & 0xffu);
    /* payload: the tag, then a counter, so a frame that comes back is identifiable and two
     * received frames are tellable apart */
    frame[CX3_R4RAW_PAYLOAD_OFFSET + 0] = (UInt8)CX3_R4RAW_TAG_0;
    frame[CX3_R4RAW_PAYLOAD_OFFSET + 1] = (UInt8)CX3_R4RAW_TAG_1;
    frame[CX3_R4RAW_PAYLOAD_OFFSET + 2] = (UInt8)CX3_R4RAW_TAG_2;
    frame[CX3_R4RAW_PAYLOAD_OFFSET + 3] = (UInt8)CX3_R4RAW_TAG_3;
    for (i = 4; i < CX3_R4RAW_PAYLOAD_BYTES; ++i)
        frame[CX3_R4RAW_PAYLOAD_OFFSET + i] = (UInt8)(i & 0xffu);
}

/* Is this received frame the one r4RawFillFrame builds?  Read out of the receive buffer, not
 * remembered: a broadcast destination, our ethertype, and the tag. */
static bool r4RawFrameIsOurs(const UInt8 *frame, UInt32 len)
{
    UInt32 i;

    if (!frame || len < CX3_R4RAW_PAYLOAD_OFFSET + 4u) return false;
    for (i = 0; i < CX3_R4RAW_MAC_BYTES; ++i) if (frame[i] != 0xffu) return false;
    if (frame[CX3_R4RAW_ETHERTYPE_OFFSET + 0] != (UInt8)((CX3_R4RAW_ETHERTYPE >> 8) & 0xffu))
        return false;
    if (frame[CX3_R4RAW_ETHERTYPE_OFFSET + 1] != (UInt8)(CX3_R4RAW_ETHERTYPE & 0xffu))
        return false;
    if (frame[CX3_R4RAW_PAYLOAD_OFFSET + 0] != (UInt8)CX3_R4RAW_TAG_0) return false;
    if (frame[CX3_R4RAW_PAYLOAD_OFFSET + 1] != (UInt8)CX3_R4RAW_TAG_1) return false;
    if (frame[CX3_R4RAW_PAYLOAD_OFFSET + 2] != (UInt8)CX3_R4RAW_TAG_2) return false;
    if (frame[CX3_R4RAW_PAYLOAD_OFFSET + 3] != (UInt8)CX3_R4RAW_TAG_3) return false;
    return true;
}

/*
 * The scalars, and the rule for the ones that may not be measured.
 *
 * `txWqeOk`, `txCqeOk`, `txWrapOk`, `rxQpnOk`, `rxRecycleOk` and `rxMinByteCnt` are written
 * only when the thing they judge actually happened, and otherwise keep CX3_R4RAW_NOT_MEASURED.
 * The reason is docs/DEFECTS.md D10: an assertion that a zeroed structure can satisfy is not
 * an assertion.  "Every completion named our QPN" is *vacuously true* when there were no
 * completions, so the honest value for that scalar on such a run is "not measured" and the
 * verifier reads it together with the count that makes it meaningful (`rxFrames`).
 */
static void r4RawPublishScalars(const Cx3R4RawResult *res, UInt64 *out)
{
    out[kR4RawOutTxRefused]     = res->txRefused;
    out[kR4RawOutTxWqeOk]       = res->txWqeOk;
    out[kR4RawOutTxPosts]       = res->txPosts;
    out[kR4RawOutTxCompletions] = res->txCompletions;
    out[kR4RawOutTxCqeOk]       = res->txCqeOk;
    out[kR4RawOutTxErrors]      = res->txErrors;
    out[kR4RawOutTxWrapOk]      = res->txWrapOk;
    out[kR4RawOutTxOwnerLapsOk] = res->txOwnerLapsOk;
    out[kR4RawOutRxFrames]      = res->rxFrames;
    out[kR4RawOutRxQpnOk]       = res->rxQpnOk;
    out[kR4RawOutRxQpn]         = res->rxQpn;
    out[kR4RawOutRxMinByteCnt]  = res->rxMinByteCnt;
    out[kR4RawOutRxContentOk]   = res->rxContentOk;
    out[kR4RawOutRxLaps]        = res->rxLaps;
    out[kR4RawOutRxRecycleOk]   = res->rxRecycleOk;
}

/*
 * The frame buffer, allocated once per send side.
 *
 * It is a page of ICM memory like every other queue's memory in this driver, so its bus
 * address is one the device can reach; the WQE names that address with stage 1's lkey, which
 * is the same key the receive WQEs carry.
 */
bool Cx3Mlx4::r4RawFrameEnsure(Cx3R4RawResult *res)
{
    if (fPortQueues[0].rawFrame) {
        res->txFramePhys  = fPortQueues[0].rawFramePhys;
        res->txFrameBytes = fPortQueues[0].rawFrameBytes;
        return true;
    }
    fPortQueues[0].rawFrame = fHw.icmAlloc(1, false);
    if (!fPortQueues[0].rawFrame) {
        LOG("r4raw: could not allocate the one-page frame buffer the send WQE has to name; "
            "the transmit half does not run. (The receive half still does: the two halves fail "
            "for different reasons and neither may hide the other.)");
        return false;
    }
    memset(fPortQueues[0].rawFrame->virt, 0, (size_t)fPortQueues[0].rawFrame->bytes);
    r4RawFillFrame((UInt8 *)fPortQueues[0].rawFrame->virt, fPortQueues[0].macValue);
    fPortQueues[0].rawFramePhys  = fPortQueues[0].rawFrame->pages[0].busAddr;
    fPortQueues[0].rawFrameBytes = CX3_R4RAW_FRAME_BYTES;
    res->txFramePhys  = fPortQueues[0].rawFramePhys;
    res->txFrameBytes = fPortQueues[0].rawFrameBytes;
    {
        char hexbuf[CX3_R4RAW_HEX_BUF_BYTES];
        LOG("r4raw: the frame, one page of ICM memory at bus 0x%llx, is built once and reused "
            "by every post. dst ff:ff:ff:ff:ff:ff (broadcast), src %02x:%02x:%02x:%02x:%02x:%02x "
            "(this port's registered address, stage 5), ethertype 0x%04x (IEEE Local "
            "Experimental 1 -- nothing on a LAN interprets it), %u bytes without the FCS (the "
            "hardware appends the 4), payload %u bytes starting with the tag 'C','X','3','R'",
            (unsigned long long)fPortQueues[0].rawFramePhys,
            (unsigned)((fPortQueues[0].macValue >> 40) & 0xffu), (unsigned)((fPortQueues[0].macValue >> 32) & 0xffu),
            (unsigned)((fPortQueues[0].macValue >> 24) & 0xffu), (unsigned)((fPortQueues[0].macValue >> 16) & 0xffu),
            (unsigned)((fPortQueues[0].macValue >>  8) & 0xffu), (unsigned)(fPortQueues[0].macValue & 0xffu),
            (unsigned)CX3_R4RAW_ETHERTYPE, (unsigned)CX3_R4RAW_FRAME_BYTES,
            (unsigned)CX3_R4RAW_PAYLOAD_BYTES);
        LOG("r4raw: the frame's bytes: %s",
            r4RawHex(hexbuf, (const UInt8 *)fPortQueues[0].rawFrame->virt, CX3_R4RAW_FRAME_BYTES));
    }
    return true;
}

/*
 * The receive ring's slot bookkeeping.  A bitmap rather than a byte per slot because the
 * question is binary ("is this slot posted and not yet completed?") and criterion 6 is exactly
 * that question asked before every refill.
 */
bool Cx3Mlx4::r4RawRxInFlight(UInt32 slot) const
{
    if (slot >= CX3_R4RAW_MAX_RX_SLOTS) return false;
    return (fPortQueues[0].rawRxInFlight[slot >> 3] & (UInt8)(1u << (slot & 7u))) ? true : false;
}

void Cx3Mlx4::r4RawRxSetInFlight(UInt32 slot, bool inFlight)
{
    UInt8 bit;

    if (slot >= CX3_R4RAW_MAX_RX_SLOTS) return;
    bit = (UInt8)(1u << (slot & 7u));
    if (inFlight) fPortQueues[0].rawRxInFlight[slot >> 3] |= bit;
    else          fPortQueues[0].rawRxInFlight[slot >> 3] &= (UInt8)(~bit);
}

/*
 * Re-post one receive slot, and tell the hardware it is there.
 *
 * The order matters and is the mirror image of the transmit side's: the frame is read out of
 * the buffer FIRST (the caller does that), then the WQE is rewritten to point at the same
 * buffer, and only then is the producer count published.  Publishing before rewriting would
 * hand the hardware a WQE it could execute into a buffer whose contents the driver is still
 * reading -- which is what "criterion 6" exists to rule out.
 *
 * The producer value is a running total, not a ring index: the reference publishes
 * `ring->prod & 0xffff` (mlx4_en_rx.c:250-254) where prod is the count of descriptors ever
 * posted, and the hardware consumes while its own count is below it.  So the value increments
 * by one per refill and wraps at 16 bits -- a value this driver publishes with the same width
 * (`rqSetProducer`).
 */
/*
 * Re-post one receive slot.  See the declaration for why this is one function and not two.
 *
 * The three device steps, in the order the reference uses (mlx4_en_rx.c:951-961: the descriptor
 * is written, then the producer is published into the doorbell RECORD -- host memory, not the
 * UAR, mlx4_en_rx.c:250-254), and the same rqBufferPhys/rqWriteWqe pair that built the ring in
 * the first place (rqCreate, Mlx4Hw.cpp:3161), so a re-posted descriptor cannot disagree with
 * an initial one about the address, the key or the byte count.
 */
bool Cx3Mlx4::rqRepostSlot(UInt32 slot)
{
    UInt64 phys = fHw.rqBufferPhys(fPortQueues[0].rqBufs, fPortQueues[0].rqBufBytes, slot);
    UInt32 byteCount = fPortQueues[0].rqBufBytes - MLX4_NET_IP_ALIGN;

    if (phys == 0) return false;

    fHw.rqWriteWqe((UInt8 *)fPortQueues[0].rqRing->virt, RX_WQE_SIZE, slot, phys, byteCount, fPortQueues[0].rqLkey);
    ++fPortQueues[0].rawRxProd;
    ++fPortQueues[0].rawRxRecyclesTotal;
    fHw.rqSetProducer((volatile UInt32 *)fPortQueues[0].qpRec->virt, fPortQueues[0].rawRxProd);
    r4RawRxSetInFlight(slot, true);
    return true;
}

void Cx3Mlx4::r4RawRxRecycle(Cx3R4RawResult *res, UInt32 slot)
{
    if (!rqRepostSlot(slot)) {
        LOG("r4raw: slot %u has no address from the buffer allocation, so its WQE was NOT "
            "re-posted: a WQE with a zero address is one the hardware would fill somewhere "
            "else. The ring will run short rather than be handed a wrong descriptor.",
            (unsigned)slot);
        ++res->rxRecycleViolations;
        res->rxRecycleOk = 0;
        return;
    }
    ++res->rxRecycles;
    if (slot < CX3_R4RAW_MAX_RX_SLOTS) {
        if (++fPortQueues[0].rawRxRecycles[slot] == 2u) ++res->rxSlotsRecycledTwice;
    }
}

/*
 * One pass over the receive completion queue.
 *
 * `wait` false consumes what is already queued; true keeps polling until the budget is spent.
 * Both modes refill as they go, which is the point of the stage: a completion names the slot
 * whose buffer may be handed back, and the ring only keeps working if that hand-back happens.
 *
 * The two things judged per completion, and why they are separate numbers:
 *
 *   the QPN (criterion 4).  `vlan_my_qpn`'s low 24 bits must be THIS driver's RX QPN.  A frame
 *   that belongs to another queue would otherwise be read as "traffic arrived".
 *
 *   the slot (criterion 6).  The reference does not read `wqe_index` at all for the receive
 *   side -- it assumes a 1:1 mapping between completions and descriptors and uses the consumer
 *   index (mlx4_en_rx.c:762-765) -- while this stage's plan asks for the field to be used.  So
 *   both are computed and compared: the CQE's own index and the ring position.  Disagreeing
 *   means either the driver refilled the wrong slot or completions were lost (a CQ that
 *   overflowed because more frames arrived than it has entries), and either way the refill
 *   stops rather than continuing on an assumption.
 */
UInt32 Cx3Mlx4::r4RawRxPoll(Cx3R4RawResult *res, UInt32 budgetMs, bool wait)
{
    UInt32 nent = fPortQueues[0].rqNent;
    UInt32 consumed = 0;
    UInt64 t0 = r4RawNowMs();
    UInt8 cqe[CQE_LEGACY_SIZE];
    bool firstFrame = true;
    bool firstBytesRead = false;

    for (;;) {
        UInt32 cons = fPortQueues[0].rawRxCons;
        UInt32 slot, cqeIdx, qpn, opcode, byteCnt;
        UInt8 *buf;
        /* One size for every hex dump in this stage -- see CX3_R4RAW_HEX_BUF_BYTES:
         * the first draft sized this for a CQE and then formatted a 64-byte frame into
         * it (docs/DEFECTS.md E12). */
        char hexbuf[CX3_R4RAW_HEX_BUF_BYTES];

        memset(cqe, 0, sizeof(cqe));
        if (!fHw.cqNextCqe(fPortQueues[0].cqRing, fPortQueues[0].cqNent, /*cqeFactor=*/1, cons, cqe)) {
            if (!wait) break;
            if ((r4RawNowMs() - t0) >= (UInt64)budgetMs) break;
            IOSleep(1);
            continue;
        }

        /* Consume it: the index moves, and the hardware is told, before anything else can go
         * wrong.  The CI record is the same one the CQ context's db_rec_addr names. */
        fPortQueues[0].rawRxCons = cons + 1;
        if (fPortQueues[0].cqRec) fHw.cqSetCi((volatile UInt32 *)fPortQueues[0].cqRec->virt, fPortQueues[0].rawRxCons);
        ++consumed;
        ++res->rxFrames;

        qpn    = cx3GetBe32(cqe + CQE_VLAN_MY_QPN_OFFSET) & MLX4_CQE_QPN_MASK;
        opcode = cqe[CQE_OWNER_SR_OPCODE_OFFSET];
        /* As in netServiceRx: the CQE carries a 16-bit absolute counter.
         * Keep it for diagnostics; only the masked counter names a buffer. */
        cqeIdx = cx3GetBe16(cqe + CQE_WQE_INDEX_OFFSET);
        slot   = cqeIdx & (nent - 1u);
        byteCnt = cx3GetBe32(cqe + CQE_BYTE_CNT_OFFSET);

        if (qpn == res->rxQpn) ++res->rxQpnMatches;

        if (firstFrame) {
            /*
             * The whole 32 bytes, in hex, for the reason the plan gives for the transmit side:
             * the CQE's field layout has one source in this tree, so if an offset is wrong the
             * first run has to show it in the bytes rather than in a number derived from a
             * possibly-wrong offset.
             */
            firstFrame = false;
            res->rxFirstRawConsIndex = cons;
            memcpy(res->rxFirstRaw, cqe, CQE_LEGACY_SIZE);
            res->rxFirstQpn = qpn;
            res->rxFirstOpcode = opcode & MLX4_CQE_OPCODE_MASK;
            res->rxFirstIsSend = (opcode & MLX4_CQE_IS_SEND_MASK) ? 1u : 0u;
            res->rxFirstOwnerSrOpcode = opcode;
            res->rxFirstWqeIndex = cqeIdx;
            res->rxFirstExpectedSlot = cons & (nent - 1u);
            res->rxFirstSlotMatches = (cqeIdx == (cons & 0xffffu)) ? 1u : 0u;
            if (res->rxFirstOpcode != MLX4_CQE_OPCODE_ERROR) {
                res->rxFirstByteCnt = byteCnt;
                res->rxFirstStatus  = (UInt32)((cqe[CQE_STATUS_OFFSET + 0] << 8) |
                                               (cqe[CQE_STATUS_OFFSET + 1]));
                res->rxFirstChecksum = (UInt32)((cqe[CQE_CHECKSUM_OFFSET + 0] << 8) |
                                                (cqe[CQE_CHECKSUM_OFFSET + 1]));
                res->rxFirstBadfcsEnc = cqe[CQE_BADFCS_ENC_OFFSET];
            }
            LOG("r4raw: the first receive completion, out of the receive CQ at consumer index "
                "%u, all %u bytes: %s",
                (unsigned)cons, (unsigned)CQE_LEGACY_TOTAL,
                r4RawHex(hexbuf, cqe, CQE_LEGACY_TOTAL));
            LOG("r4raw: ...decoded with the offsets this build checked (byte_cnt 0x%02x, "
                "wqe_index 0x%02x, status 0x%02x, owner_sr_opcode 0x%02x): qpn %u (ours is "
                "%u), opcode 0x%02x %s, is_send %u, byte_cnt %u, wqe_index %u (the ring "
                "absolute counter says %u), status 0x%04x, checksum 0x%04x",
                (unsigned)CQE_BYTE_CNT_OFFSET, (unsigned)CQE_WQE_INDEX_OFFSET,
                (unsigned)CQE_STATUS_OFFSET, (unsigned)CQE_OWNER_SR_OPCODE_OFFSET,
                (unsigned)qpn, (unsigned)res->rxQpn, (unsigned)(opcode & MLX4_CQE_OPCODE_MASK),
                ((opcode & MLX4_CQE_OPCODE_MASK) == MLX4_CQE_OPCODE_ERROR)
                    ? "(ERROR -- the fields at these offsets are the error struct's: syndrome "
                      "at 0x1b)" : "",
                (unsigned)((opcode & MLX4_CQE_IS_SEND_MASK) ? 1 : 0), (unsigned)byteCnt,
                (unsigned)cqeIdx, (unsigned)(cons & 0xffffu),
                (unsigned)res->rxFirstStatus, (unsigned)res->rxFirstChecksum);
        }

        if ((opcode & MLX4_CQE_OPCODE_MASK) == MLX4_CQE_OPCODE_ERROR) {
            ++res->rxErrorCqes;
            LOG("r4raw: receive completion %u is an ERROR completion (opcode 0x%02x) -- the "
                "bytes at the good CQE's status/byte_cnt offsets mean something else in the "
                "error struct, so NOTHING is judged from them (that is the whole reason the "
                "opcode is read first). Syndrome 0x%02x, vendor syndrome 0x%02x (mlx4_err_cqe, "
                "cq.h:63-72). The slot it names is still refilled, because my_qpn and "
                "wqe_index sit at the same offsets in both structs.",
                (unsigned)cons, (unsigned)(opcode & MLX4_CQE_OPCODE_MASK),
                (unsigned)cqe[CQE_ERR_SYNDROME_OFFSET],
                (unsigned)cqe[CQE_ERR_VENDOR_SYNDROME_OFFSET]);
        } else {
            /*
             * Criterion 5.  byte_cnt is the frame length WITH the FCS (this card has FCS_KEEP),
             * so a minimum-size frame reads as 64 -- which is why the test is "at least the
             * minimum frame", not "equal to what we sent".
             */
            if (res->rxMinByteCnt == CX3_R4RAW_NOT_MEASURED || byteCnt < res->rxMinByteCnt)
                res->rxMinByteCnt = byteCnt;
            if (byteCnt >= CX3_R4RAW_FRAME_BYTES) res->rxContentOk = 1;
            if (!firstBytesRead && slot < nent) {
                UInt32 keep = (byteCnt < CX3_R4RAW_RX_KEEP_BYTES) ? byteCnt
                                                                 : CX3_R4RAW_RX_KEEP_BYTES;
                firstBytesRead = true;
                buf = (UInt8 *)fPortQueues[0].rqBufs->virt + (UInt64)slot * (UInt64)fPortQueues[0].rqBufBytes;
                memcpy(res->rxFirstBytes, buf, keep);
                res->rxFirstBytesLen = keep;
                LOG("r4raw: the first frame: %u bytes reported by the CQE, %u of them read out "
                    "of the buffer this slot points at (bus 0x%llx): %s",
                    (unsigned)byteCnt, (unsigned)keep,
                    (unsigned long long)fHw.rqBufferPhys(fPortQueues[0].rqBufs, fPortQueues[0].rqBufBytes, slot),
                    r4RawHex(hexbuf, res->rxFirstBytes, keep));
                if (r4RawFrameIsOurs(buf, keep)) {
                    /*
                     * Our own frame came back.  On a switched segment a broadcast frame does
                     * NOT return to the port it left, so this is expected to be 0 there and is
                     * not one of the six judgements -- but when it does happen it is the
                     * strongest possible content check, so it is copied out and reported.
                     */
                    memcpy(res->rxOursBytes, buf, keep);
                    res->rxOursBytesLen = keep;
                    res->rxOurs = 1;
                    LOG("r4raw: and that frame is THIS DRIVER'S OWN: broadcast destination, "
                        "ethertype 0x%04x, payload tag 'C','X','3','R' -- it left the card and "
                        "came back, which is a loopback on the segment rather than an echo of "
                        "the transmit path being repeated", (unsigned)CX3_R4RAW_ETHERTYPE);
                }
            }
        }

        /*
         * Criterion 6, asked as three questions with three different answers.
         *
         *   Is the slot the CQE names the slot the ring position predicts?
         *   Was that slot in flight (posted and not yet completed)?
         *   Is it a slot this ring has (i.e. inside the WQE array)?
         *
         * Any "no" stops the refill.  The frames already in the ring's remaining buffers can
         * still be collected -- that is why only the refill stops and not the polling -- but
         * nothing further is handed back on a guess.
         */
        if (slot >= nent) {
            LOG("r4raw: the completion at consumer index %u names slot %u, which is outside the "
                "%u-WQE receive ring. Nothing is refilled from here on: the CQE's slot field and "
                "this ring do not describe the same queue, and guessing which one is right is "
                "how a buffer gets handed back while a frame can still land in it.",
                (unsigned)cons, (unsigned)slot, (unsigned)nent);
            ++res->rxRecycleViolations;
            res->rxRecycleOk = 0;
            res->rxViolationSlot = slot;
            res->rxViolationCons = cons;
            break;
        }
        if (cqeIdx != (cons & 0xffffu)) {
            LOG("r4raw: the completion at consumer index %u carries absolute wqe_index %u, "
                "but the expected 16-bit counter is %u. Two readings disagree: either the "
                "refill wrote the wrong slot, or completions were LOST (this CQ has %u entries "
                "and the hardware wraps it: more frames arriving between two reads than it can "
                "hold breaks the one-to-one correspondence the reference relies on, "
                "mlx4_en_rx.c:762-765). The refill stops here rather than continue on an "
                "assumption; the frames already posted can still be collected.",
                (unsigned)cons, (unsigned)cqeIdx, (unsigned)(cons & 0xffffu),
                (unsigned)fPortQueues[0].cqNent);
            ++res->rxRecycleViolations;
            res->rxRecycleOk = 0;
            res->rxViolationSlot = slot;
            res->rxViolationCons = cons;
            break;
        }
        if (!r4RawRxInFlight(slot)) {
            LOG("r4raw: slot %u is NOT in flight, and the completion at consumer index %u says "
                "a frame landed in it -- which means it was handed back before its previous "
                "frame was read. This is the double-occupancy case criterion 6 exists for; the "
                "refill stops and the ring's state is left as it is for a reader to inspect",
                (unsigned)slot, (unsigned)cons);
            ++res->rxRecycleViolations;
            res->rxRecycleOk = 0;
            res->rxViolationSlot = slot;
            res->rxViolationCons = cons;
            break;
        }

        /* Its frame has been read (above), so the slot can go back. */
        r4RawRxSetInFlight(slot, false);
        r4RawRxRecycle(res, slot);
        /* A line per lap rather than per completion: the interesting numbers are the totals,
         * and several hundred identical lines would bury the one that differs. */
        if ((fPortQueues[0].rawRxRecyclesTotal % nent) == 0u) {
            LOG("r4raw: the receive ring has now been refilled %u time(s) in total (%u whole "
                "laps of %u slots), producer %u published; the consumer index is %u",
                (unsigned)fPortQueues[0].rawRxRecyclesTotal,
                (unsigned)(fPortQueues[0].rawRxRecyclesTotal / nent), (unsigned)nent,
                (unsigned)fPortQueues[0].rawRxProd, (unsigned)fPortQueues[0].rawRxCons);
        }
    }

    res->rxCqConsAfter = fPortQueues[0].rawRxCons;
    return consumed;
}

/*
 * The transmit phase: one WQE at a time, and each completion is awaited before the next post.
 *
 * Why one at a time is not just caution (plan section 1): "post N frames and ring once" is a
 * different experiment from "post, ring, wait, reclaim", and the second is the one the
 * reference implements per call (mlx4_en_xmit posts one WQE and rings the doorbell at the end,
 * mlx4_en_tx.c:890-919).  Posting a batch would also mean several descriptors in flight before
 * any of them is known to be readable, which is the failure this stage cannot afford.
 *
 * The loop is bounded three ways: the caller's post count, a per-post wait for its completion,
 * and a wall-clock budget for the whole wrap.  Any of them stopping the loop leaves the
 * scalars saying which: posts (how far it got), timeouts (a completion that never came) and
 * the owner-lap counts (whether the wrap's phase rule held on the laps it did cover).
 */
void Cx3Mlx4::r4RawTransmit(Cx3R4RawResult *res, UInt32 postsWanted)
{
    UInt32 nent = fPortQueues[0].txSqNent;
    UInt32 cap, i;
    UInt32 lapSeen = 0, lapBad = 0, ownerBitsSeen = 0;
    UInt8 *sqVirt = fPortQueues[0].txSq ? (UInt8 *)fPortQueues[0].txSq->virt : 0;
    UInt64 t0 = r4RawNowMs();
    bool wrapAttempted = false;

    res->txQpn           = fPortQueues[0].txQpn;
    res->txUarIndex      = fPortQueues[0].cqUarStart;
    res->txSqNent        = nent;
    /*
     * 0, and reported rather than assumed: the send queue has no 64-byte unit in front of its
     * WQEs, so the doorbell's TXBB count and the ring's byte offset are the same number
     * (SQ_WQE0_OFFSET, mlx4_en_tx.c:139/:686).  0.43.7 sent the receive ring's 64 here and no
     * descriptor was ever executed -- docs/DEFECTS.md E13.
     */
    res->txSqHeaderBytes = SQ_WQE0_OFFSET;
    res->txWqeNonZeroBytes = 0xffffffffu;   /* not measured: nothing has been posted yet */
    res->txWqeOk         = CX3_R4RAW_NOT_MEASURED;
    res->txCqeOk         = CX3_R4RAW_NOT_MEASURED;
    res->txWrapOk        = CX3_R4RAW_NOT_MEASURED;

    if (!sqVirt || nent < 2u) {
        LOG("r4raw: the send queue's memory is not there (or its TXBB count is %u), so nothing "
            "is posted", (unsigned)nent);
        return;
    }
    /*
     * Which numbers go into the doorbell and the WQEs, named once.
     *
     * The UAR index is the one stage 6 put in the QP's context (usr_page) -- the QP and the
     * doorbell have to agree, and this is the only place where they could disagree.  The value
     * is written at that UAR page + MLX4_SEND_DOORBELL (0x14); the unused CQ-doorbell helper in
     * the hardware layer writes to the page at the constant MLX4_NUM_RESERVED_UARS instead,
     * which is numerically the same index on this card and is recorded in
     * docs/R4-STAGE9-RESULT.md rather than copied into the new path.
     */
    cap = postsWanted ? postsWanted : (2u * nent + 1u);
    if (cap > 8u * nent + 1u) {
        LOG("r4raw: %u posts were asked for, more than eight laps (%u); clamping -- a call that "
            "takes minutes would hold the transaction lock for that long, and the wrap is "
            "already proved by two", (unsigned)cap, (unsigned)(8u * nent + 1u));
        cap = 8u * nent + 1u;
    }
    LOG("r4raw: the transmit phase: %u post(s) planned, one at a time, each followed by its "
        "completion; QPN %u, SQ %u TXBBs of %u bytes with WQE 0 at offset %u (no header: the "
        "receive ring's WQE 0 is at +%u, the send queue's at +0), frame %u bytes at "
        "bus 0x%llx, lkey 0x%08x, UAR index %u -> doorbell BAR2+0x%llx (offset 0x%x inside that "
        "page). %s",
        (unsigned)cap, (unsigned)fPortQueues[0].txQpn, (unsigned)nent, (unsigned)MLX4_TXBB_SIZE,
        (unsigned)SQ_WQE0_OFFSET, (unsigned)RQ_WQE0_OFFSET,
        (unsigned)CX3_R4RAW_FRAME_BYTES,
        (unsigned long long)fPortQueues[0].rawFramePhys, (unsigned)fMrLkey, (unsigned)fPortQueues[0].cqUarStart,
        (unsigned long long)(mlx4UarPageOffset(fPortQueues[0].cqUarStart) + MLX4_SEND_DOORBELL),
        (unsigned)MLX4_SEND_DOORBELL,
        (postsWanted == 1u) ? "The caller asked for ONE post, so the wrap phase is skipped and "
                              "this run claims nothing about the ring turning."
                            : "The first post happens alone, and the wrap only continues if its "
                              "completion arrives.");

    for (i = 0; i < cap; ++i) {
        UInt32 prod     = fPortQueues[0].rawTxProd;
        UInt32 index    = prod & (nent - 1u);
        UInt32 ownerBit = (prod & nent) ? 1u : 0u;   /* mlx4_en_tx.c:682: prod & ring->size */
        UInt32 lap      = prod / nent;
        bool   verbose  = (i == 0u) || (index == 0u);   /* the first post of each lap */
        UInt8  image[TX_WQE_SEND_SIZE];
        UInt8  back[TX_WQE_SEND_SIZE];
        UInt32 dword, backOwner, j;
        bool   ok = true;
        char   hexbuf[CX3_R4RAW_HEX_BUF_BYTES];

        if (i > 0u && (r4RawNowMs() - t0) > (UInt64)CX3_R4RAW_DEFAULT_TX_WRAP_BUDGET_MS) {
            LOG("r4raw: the wrap phase's %u ms budget is spent after %u post(s); stopping. "
                "Every post that was made had its completion consumed before the next one was "
                "written, so nothing is left in flight by the loop stopping here.",
                (unsigned)CX3_R4RAW_DEFAULT_TX_WRAP_BUDGET_MS, (unsigned)res->txPosts);
            break;
        }
        if (i > 0u) wrapAttempted = true;

        fHw.sqWriteSendWqe(sqVirt, index, prod, ownerBit, CX3_R4RAW_FRAME_BYTES, fMrLkey,
                           fPortQueues[0].rawFramePhys, /*nDataSegs=*/1u, MLX4_OPCODE_SEND, image, verbose);
        /*
         * Read it back OUT of the queue's memory, with a load, and decode the fields rather
         * than compare two copies of the driver's own bytes: what is judged is whether the
         * descriptor in memory says what it has to say.  This is criterion 1 -- and the one
         * thing it cannot say anything about is how the hardware interprets it, which is the
         * completion's business (docs/R4-STAGE6-RESULT.md section 5.1 is the same lesson from
         * the other side).
         */
        memcpy(back, sqVirt + MLX4_SQ_WQE_OFFSET(index), TX_WQE_SEND_SIZE);
        dword = cx3GetBe32(back + TX_WQE_OWNER_OPCODE_OFFSET);
        backOwner = (dword >> MLX4_TX_OWNER_SHIFT) & 1u;
        if ((dword & 0xffu) != MLX4_OPCODE_SEND) ok = false;
        if (backOwner != ownerBit) ok = false;
        /*
         * The owner bit lives ABOVE the producer field in this dword, so the shift has to be
         * masked before it is compared -- and the log line below always did mask it, which is
         * why this read as a correct value while the check failed.  It only fails once the ring
         * passes the first lap, because that is when the owner bit is set for the first time;
         * the transmit never got that far until 0.43.12, so the comparison was never exercised.
         * A guard that has never been able to fail is the shape docs/DEFECTS.md records, and
         * this one failed on the first lap it ever saw.
         */
        if (((dword >> MLX4_TX_PROD_SHIFT) & MLX4_TX_PROD_MASK) !=
            (prod & MLX4_TX_PROD_MASK)) ok = false;
        if (cx3GetBe32(back + TX_WQE_BYTE_COUNT_OFFSET) != CX3_R4RAW_FRAME_BYTES) ok = false;
        if (cx3GetBe32(back + TX_WQE_LKEY_OFFSET) != fMrLkey) ok = false;
        if (cx3GetBe64(back + TX_WQE_ADDR_OFFSET) != fPortQueues[0].rawFramePhys) ok = false;
        if (cx3GetBe32(back + TX_WQE_SRCRB_FLAGS_OFFSET) !=
            (MLX4_WQE_CTRL_CQ_UPDATE | MLX4_WQE_CTRL_SOLICITED)) ok = false;
        if (cx3GetBe32(back + TX_WQE_IMM_OFFSET) != 0) ok = false;
        /*
         * The fence_size byte is the descriptor's size in 16-byte units INCLUDING the control
         * segment -- 2 for this ctrl-plus-one-segment WQE, and 1 was what four builds of a
         * silent transmit wrote (docs/M2B-NOTES.md section 54).  Judged against the constant
         * this writer derives it from, so the check and the writer cannot disagree.
         */
        if (back[TX_WQE_FENCE_SIZE_OFFSET] != (UInt8)TX_WQE_SEND_FENCE_SIZE) ok = false;
        if (back[TX_WQE_INS_VLAN_OFFSET] != 0u) ok = false;
        if (cx3GetBe16(back + TX_WQE_VLAN_TAG_OFFSET) != 0u) ok = false;
        /* Nothing may have written past the 32-byte WQE inside its TXBB. */
        {
            UInt32 nz = 0;
            for (j = TX_WQE_SEND_SIZE; j < MLX4_TXBB_SIZE; ++j) if (back[j] != 0) ++nz;
            if (i == 0u) res->txWqeNonZeroBytes = nz;
        }
        ++res->txWqeChecked;
        if (i == 0u) {
            res->txWqeIndex          = index;
            res->txWqeDword          = dword;
            res->txWqeOwnerBit       = backOwner;
            res->txWqeOwnerWanted    = ownerBit;
            res->txFramePayloadBytes = CX3_R4RAW_PAYLOAD_BYTES;
            memcpy(res->txWqeImage, back, TX_WQE_SEND_SIZE);
        }
        if (!ok) {
            res->txWqeOk = 0;
            LOG("r4raw: the WQE at TXBB %u (lap %u, prod %u) does NOT read back as it was "
                "encoded, so the transmit phase stops HERE -- before the doorbell. The 32 bytes "
                "in memory: %s", (unsigned)index, (unsigned)lap, (unsigned)prod,
                r4RawHex(hexbuf, back, TX_WQE_SEND_SIZE));
            LOG("r4raw: ...decoded: owner_opcode 0x%08x (opcode 0x%02x must be in the LOW byte; "
                "owner %u; prod 0x%04x), byte_count %u (want %u), lkey 0x%08x (want 0x%08x), "
                "addr 0x%llx (want 0x%llx), srcrb_flags 0x%08x (want 0x%08x), fence_size %u "
                "(want %u -- the descriptor's size in 16-byte units, ctrl seg included), "
                "imm 0x%08x (want 0)",
                (unsigned)dword, (unsigned)(dword & 0xffu), (unsigned)backOwner,
                (unsigned)((dword >> MLX4_TX_PROD_SHIFT) & MLX4_TX_PROD_MASK),
                (unsigned)cx3GetBe32(back + TX_WQE_BYTE_COUNT_OFFSET),
                (unsigned)CX3_R4RAW_FRAME_BYTES,
                (unsigned)cx3GetBe32(back + TX_WQE_LKEY_OFFSET), (unsigned)fMrLkey,
                (unsigned long long)cx3GetBe64(back + TX_WQE_ADDR_OFFSET),
                (unsigned long long)fPortQueues[0].rawFramePhys,
                (unsigned)cx3GetBe32(back + TX_WQE_SRCRB_FLAGS_OFFSET),
                (unsigned)(MLX4_WQE_CTRL_CQ_UPDATE | MLX4_WQE_CTRL_SOLICITED),
                (unsigned)back[TX_WQE_FENCE_SIZE_OFFSET],
                (unsigned)TX_WQE_SEND_FENCE_SIZE,
                (unsigned)cx3GetBe32(back + TX_WQE_IMM_OFFSET));
            break;
        }
        if (res->txWqeOk == CX3_R4RAW_NOT_MEASURED) res->txWqeOk = 1;
        /* The lap phase, and whether the memory agreed with it. */
        lapSeen |= (1u << (lap & 31u));
        if (backOwner != ownerBit) lapBad |= (1u << (lap & 31u));
        ownerBitsSeen |= (backOwner ? 2u : 1u);

        /*
         * The doorbell -- and nothing between it and the wait.  BE32(qpn << 8) at UAR + 0x14
         * (mlx4_en_tx.c:231, :918-919): the send doorbell, not the CQ's 0x20.
         */
        fHw.sqRingSendDoorbell(fPortQueues[0].txQpn, fPortQueues[0].cqUarStart, verbose);
        if (i == 0u) {
            res->txDoorbellValue  = (fPortQueues[0].txQpn & MLX4_CQE_QPN_MASK) << MLX4_SEND_DB_QPN_SHIFT;
            res->txDoorbellOffset = mlx4UarPageOffset(fPortQueues[0].cqUarStart) + MLX4_SEND_DOORBELL;
        }
        /* The WQE is posted, so the ring's producer moves now -- before the wait, because the
         * hardware's own count is what the wait is about. */
        fPortQueues[0].rawTxProd = prod + 1u;
        ++res->txPosts;

        /* --- criterion 2: the completion --- */
        {
            UInt64 tw = r4RawNowMs();
            bool got = false;

            for (;;) {
                UInt8 cqe[CQE_LEGACY_SIZE];
                memset(cqe, 0, sizeof(cqe));
                if (fHw.cqNextCqe(fPortQueues[0].txCqRing, fPortQueues[0].txCqNent, /*cqeFactor=*/1, fPortQueues[0].rawTxCons,
                                  cqe)) {
                    UInt32 cons   = fPortQueues[0].rawTxCons;
                    UInt32 opcode = cqe[CQE_OWNER_SR_OPCODE_OFFSET];
                    UInt32 wqeIdx = cx3GetBe16(cqe + CQE_WQE_INDEX_OFFSET);
                    UInt32 bc     = cx3GetBe32(cqe + CQE_BYTE_CNT_OFFSET);
                    UInt32 isSend = (opcode & MLX4_CQE_IS_SEND_MASK) ? 1u : 0u;
                    UInt32 err    = ((opcode & MLX4_CQE_OPCODE_MASK) == MLX4_CQE_OPCODE_ERROR)
                                    ? 1u : 0u;

                    fPortQueues[0].rawTxCons = cons + 1u;
                    if (fPortQueues[0].txCqRec)
                        fHw.cqSetCi((volatile UInt32 *)fPortQueues[0].txCqRec->virt, fPortQueues[0].rawTxCons);
                    ++res->txCompletions;
                    if (err) ++res->txErrors;
                    if (res->txCompletions == 1u) {
                        char chb[CX3_R4RAW_HEX_BUF_BYTES];
                        res->txCqeRawConsIndex  = cons;
                        res->txCqeQpn           = cx3GetBe32(cqe + CQE_VLAN_MY_QPN_OFFSET) &
                                                  MLX4_CQE_QPN_MASK;
                        res->txCqeByteCnt       = bc;
                        res->txCqeWqeIndex      = wqeIdx;
                        res->txCqeOwnerSrOpcode = opcode;
                        res->txCqeOpcode        = opcode & MLX4_CQE_OPCODE_MASK;
                        res->txCqeIsSend        = isSend;
                        res->txCqeStatus        = cx3GetBe16(cqe + CQE_STATUS_OFFSET);
                        res->txCqeChecksum      = cx3GetBe16(cqe + CQE_CHECKSUM_OFFSET);
                        memcpy(res->txCqeRaw, cqe, CQE_LEGACY_TOTAL);
                        /*
                         * "Well formed and not in error" as one number, with both halves in
                         * the log so a failure says which: the opcode must not be the error
                         * value (the reference's own test, mlx4_en_tx.c:381-382), the bit that
                         * marks a send completion must be set, and the CQE's wqe_index must be
                         * the low 16 bits of the absolute producer that was posted, not its
                         * masked TXBB slot -- which makes this completion THIS
                         * frame's rather than a leftover.
                         *
                         * byte_cnt and the status word are recorded and NOT judged: the
                         * reference's transmit poll reads neither (mlx4_en_tx.c:381-395 reads
                         * the opcode and wqe_index), and there is no second source for what a
                         * send completion's byte_cnt means.  Judging an unknown against an
                         * unknown is how a PASS line ends up saying nothing.
                         */
                        res->txCqeOk = (!err && isSend && wqeIdx == (prod & 0xffffu)) ? 1u : 0u;
                        LOG("r4raw: the first transmit completion, out of the TX CQ at consumer "
                            "index %u, all %u bytes: %s", (unsigned)cons,
                            (unsigned)CQE_LEGACY_TOTAL,
                            r4RawHex(chb, cqe, CQE_LEGACY_TOTAL));
                        LOG("r4raw: ...decoded: qpn %u, opcode 0x%02x%s, is_send %u, wqe_index "
                            "%u (the WQE posted was TXBB %u), byte_cnt %u, status 0x%04x, "
                            "checksum 0x%04x -- those last three recorded, not judged: the "
                            "reference's transmit poll reads the opcode and wqe_index only",
                            (unsigned)res->txCqeQpn, (unsigned)res->txCqeOpcode,
                            err ? " (ERROR: its syndrome bytes are at 0x1a/0x1b)" : "",
                            (unsigned)isSend, (unsigned)wqeIdx, (unsigned)res->txWqeIndex,
                            (unsigned)bc, (unsigned)res->txCqeStatus,
                            (unsigned)res->txCqeChecksum);
                    }
                    if (err) {
                        LOG("r4raw: the completion for post %u (TXBB %u) is an ERROR completion: "
                            "syndrome 0x%02x, vendor syndrome 0x%02x. That is the device SAYING "
                            "the descriptor was bad -- so look at the WQE's fields (logged above) "
                            "and at the memory region the WQE's address has to lie in, NOT at the "
                            "doorbell: a doorbell that never arrived produces no completion at "
                            "all, and this one arrived",
                            (unsigned)(i + 1u), (unsigned)index,
                            (unsigned)cqe[CQE_ERR_SYNDROME_OFFSET],
                            (unsigned)cqe[CQE_ERR_VENDOR_SYNDROME_OFFSET]);
                    }
                    got = true;
                    break;
                }
                if ((r4RawNowMs() - tw) >= (UInt64)CX3_R4RAW_DEFAULT_TX_POST_WAIT_MS) break;
                IOSleep(1);
            }
            if (!got) {
                ++res->txTimeouts;
                LOG("r4raw: post %u (TXBB %u, lap %u, prod %u, owner bit %u) produced NO "
                    "completion within %u ms, and this stops the loop: no completion means the "
                    "WQE was not executed (or its completion went to a queue nobody read), and "
                    "posting another would build on that. The ring's producer is at %u and the "
                    "TX CQ's consumer at %u.",
                    (unsigned)(i + 1u), (unsigned)index, (unsigned)lap, (unsigned)prod,
                    (unsigned)ownerBit, (unsigned)CX3_R4RAW_DEFAULT_TX_POST_WAIT_MS,
                    (unsigned)fPortQueues[0].rawTxProd, (unsigned)fPortQueues[0].rawTxCons);
                break;
            }
        }
        /*
         * Reclaim the TXBB: the transmit stamp with the phase it will next be posted in
         * (mlx4_en_stamp_wqe, mlx4_en_tx.c:271-287).  A reclaimed slot has to stop looking
         * like a posted descriptor, and this is where the next lap's marker comes from.
         */
        /*
         * The phase this stamp carries is THE SLOT'S OWN -- the lap the descriptor that just
         * completed was posted in, which is ownerBit.  The reference writes the same thing and
         * has to reconstruct it from the consumer count because it reclaims in batches:
         *
         *     mlx4_en_stamp_wqe(priv, ring, stamp_index,
         *                       !!((ring->cons + txbbs_stamp) & ring->size));
         *                                                    (mlx4_en_tx.c:398-400)
         *
         * This port wrote `((prod + 1) & nent)` instead, the NEXT post's phase, and the two
         * agree everywhere except on the LAST TXBB of each lap -- which is precisely the slot
         * the hardware looks at after it wraps.  Measured (docs/M2B-NOTES.md 55): two laps of
         * posts completed with zero errors and the ring's final slot did not, and every post
         * after the wrap came back as an ERROR completion (syndrome 0x05, vendor syndrome 0xf9),
         * i.e. the device executed something in that slot that was not a valid WQE -- the stamp.
         * Changing this cannot touch laps 0 and 1 (both expressions give 0 for prod 0..254), so
         * the working case is unchanged by construction; only the slot at the wrap moves.
         */
        fHw.sqStampTxbb(sqVirt, index, ownerBit);
    }

    res->txProdAfter     = fPortQueues[0].rawTxProd;
    res->txLapsCovered   = (UInt32)__builtin_popcount(lapSeen);
    res->txOwnerLapsOk   = (UInt32)__builtin_popcount(lapSeen & ~lapBad);
    res->txOwnerBitsSeen = ownerBitsSeen;
    /*
     * Criterion 3 as one bit: the posts went PAST two laps of the ring (so both were covered
     * in full and a third has begun), every post's readback was what the encoder intended, the
     * owner bit read back as the lap phase said on every lap covered, and BOTH owner values
     * were observed -- because a ring that only ever wrote 0 would satisfy "each post matched
     * its phase" while proving nothing about the flip.
     */
    if (wrapAttempted) {
        res->txWrapOk = (res->txProdAfter > 2u * nent && res->txWqeOk == 1u &&
                         res->txOwnerLapsOk == res->txLapsCovered &&
                         res->txOwnerBitsSeen == 3u) ? 1u : 0u;
    }
    res->txWrapMs = (UInt32)(r4RawNowMs() - t0);
    LOG("r4raw: the transmit phase is done: %u post(s), %u completion(s) (%u in error, %u "
        "timed out), producer %u (laps covered, %u TXBBs each: owner bits seen 0x%x, laps whose "
        "readback matched the phase %u), WQE readback %s, wrap %s, %u ms",
        (unsigned)res->txPosts, (unsigned)res->txCompletions, (unsigned)res->txErrors,
        (unsigned)res->txTimeouts, (unsigned)res->txProdAfter, (unsigned)nent,
        (unsigned)res->txOwnerBitsSeen, (unsigned)res->txOwnerLapsOk,
        (res->txWqeOk == 1u) ? "matched on every post"
                             : ((res->txWqeOk == 0u) ? "MISMATCHED" : "not reached"),
        (res->txWrapOk == 1u) ? "PROVED (more than two laps, both owner phases seen)"
                              : ((res->txWrapOk == 0u) ? "NOT proved (see the counts above)"
                                                       : "not reached"),
        (unsigned)res->txWrapMs);
}

/*
 * kCx3SelR4Raw -- the whole stage, in the order the plan lays out.
 *
 *   1. the receive prerequisites.  Nothing can be observed without them, so their absence is
 *      kIOReturnNotReady (the chain reports BLOCKED: a stage it depends on has not run).
 *   2. the memory region, read back from the firmware.  The WQEs on both sides carry stage 1's
 *      lkey and a bus address, and the reference's own MR covers the WHOLE address space
 *      (`mlx4_mr_alloc(dev, priv_pdn, 0, ~0ull, ...)`, mlx4_en_main.c:213) so that any DMA
 *      buffer is inside it.  This driver's region was registered with whatever `r4mr` was
 *      given -- the chain passes 0 and 0 -- so whether the frame buffer lies inside it is a
 *      real question with a real consequence (a descriptor whose address is outside the MR is
 *      a completion in error).  It is read here and REPORTED, in the same run, instead of
 *      discovered one reboot later.
 *   3. the frame buffer and its 60 bytes.
 *   4. the receive drain: consume and refill whatever is already queued.  The port is up and
 *      frames have been landing in the ring since stage 4 published the producer, so this is
 *      where the first frames usually are.
 *   5. the transmit phase: one WQE, one frame, its completion; then the wrap.
 *   6. the receive wait: keep polling for the budget, so frames that arrive while the frames
 *      go out are collected and refilled too.
 *   7. the scalars, and a log line that says in words what the numbers say.
 *
 * The order of 4 and 5 matters for one reason worth writing down: the receive CQ can only hold
 * so many completions (256 here) and the ring behind it, so consuming before transmitting means
 * the frames the transmit half may provoke cannot arrive into a queue that is already full of
 * older ones.
 */
/* ------------------------------------------------------------------ */
/* kCx3SelTxProbe -- the three discriminators of docs/M2B-NOTES.md 52  */
/* ------------------------------------------------------------------ */

/*
 * One post from a known ring state, with the two things this selector moves: the descriptor's
 * OPCODE and the send doorbell record's contents.
 *
 * The stamps are reset first, so every call starts from the state r4tx left the queue in (every
 * 64-byte unit reading 0xffffffff, "software owns").  Without that the calls would not be
 * independent: the second one would post into a ring whose slot 0 still reads like a descriptor
 * the hardware owns, and a completion belonging to the earlier post could be counted as this
 * one's -- which is the quiet mis-attribution this stage exists to avoid.
 *
 * Anything already sitting in the send CQ is consumed BEFORE the post and not counted, for the
 * same reason: a completion that arrived earlier is not evidence about this descriptor.  The
 * consumer index is advanced and published exactly as the wrap phase does it, so a completion
 * consumed here can never be read twice.
 *
 * Returns the number of completions that arrived after this post (0 or more).  A completion in
 * ERROR still counts: it too proves the hardware executed the descriptor, and that is the
 * question at this level.  What the first one was is handed back through the out parameters.
 */
UInt32 Cx3Mlx4::r4TxProbePost(UInt32 opcode, UInt32 nDataSegs, UInt32 recWord0, UInt32 recWord1,
                              UInt32 waitMs, UInt32 *outOpcode, UInt32 *outStatus, UInt32 *outWqeOk)
{
    UInt8 *sqVirt = fPortQueues[0].txSq ? (UInt8 *)fPortQueues[0].txSq->virt : 0;
    UInt8 image[TX_WQE_SEND_SIZE];
    UInt32 dword, comps = 0, pre = 0, probeIndex, probeOwner;
    UInt64 t0;

    if (outOpcode) *outOpcode = CX3_TXPROBE_OPCODE_NONE;
    if (outStatus) *outStatus = 0u;
    if (outWqeOk)  *outWqeOk  = 0u;
    if (!sqVirt || !fPortQueues[0].txCqRing) return 0u;

    /* --- the drain: whatever was already there is not this post's --- */
    for (;;) {
        UInt8 cqe[CQE_LEGACY_SIZE];
        memset(cqe, 0, sizeof(cqe));
        if (!fHw.cqNextCqe(fPortQueues[0].txCqRing, fPortQueues[0].txCqNent, /*cqeFactor=*/1, fPortQueues[0].rawTxCons, cqe))
            break;
        ++pre;
        fPortQueues[0].rawTxCons = fPortQueues[0].rawTxCons + 1u;
        if (fPortQueues[0].txCqRec) fHw.cqSetCi((volatile UInt32 *)fPortQueues[0].txCqRec->virt, fPortQueues[0].rawTxCons);
        if (pre >= 8u) break;
    }
    if (pre) {
        LOG("txprobe: %u completion(s) were already in the send CQ before this post; they were "
            "consumed and are not counted against the descriptor that follows", (unsigned)pre);
    }

    /*
     * WHERE the descriptor goes and WHICH PHASE its owner bit carries are the ring's current
     * state, not constants.  The hardware consumes this queue in lap order and the owner bit's
     * meaning alternates with the lap (mlx4_en_tx.c:682), so a post at a fixed TXBB 0 with
     * owner 0 is correct only while the ring is in its first lap.
     *
     * The first version of this probe did exactly that, and the report it produced was wrong in
     * the most expensive possible way: run after r4raw had taken the ring into lap 1, its posts
     * were never executed, so "a NOP produced no completion" was read as "the hardware does not
     * execute this queue" (docs/M2B-NOTES.md 55).  The fix is not a constant either -- both
     * numbers come from fPortQueues[0].rawTxProd, and the producer advances after the post because the
     * descriptor really is in the ring now.
     */
    probeIndex = fPortQueues[0].rawTxProd & (fPortQueues[0].txSqNent - 1u);
    probeOwner = ((fPortQueues[0].rawTxProd & fPortQueues[0].txSqNent) != 0u) ? 1u : 0u;
    fHw.sqSetDoorbellRecord(fPortQueues[0].txQpRec ? (volatile UInt32 *)fPortQueues[0].txQpRec->virt : 0,
                            recWord0, recWord1);
    fHw.sqWriteSendWqe(sqVirt, probeIndex, fPortQueues[0].rawTxProd, probeOwner, CX3_R4RAW_FRAME_BYTES,
                       fMrLkey, fPortQueues[0].rawFramePhys, /*nDataSegs=*/nDataSegs, opcode, image,
                       /*verbose=*/false);
    /*
     * Read the 32 bytes back OUT of the queue's memory, as r4raw's criterion 1 does: the
     * question is what the descriptor in memory carries, not what was passed in.  A descriptor
     * that is not there cannot be executed, and a NOP that was written as a SEND (or the other
     * way round) would make the whole comparison meaningless.
     */
    memcpy(image, sqVirt + MLX4_SQ_WQE_OFFSET(probeIndex), TX_WQE_SEND_SIZE);
    dword = cx3GetBe32(image + TX_WQE_OWNER_OPCODE_OFFSET);
    if (outWqeOk) {
        *outWqeOk = (((dword & 0xffu) == (opcode & 0xffu)) &&
                     (((dword >> MLX4_TX_OWNER_SHIFT) & 1u) == probeOwner) &&
                     (((dword >> MLX4_TX_PROD_SHIFT) & MLX4_TX_PROD_MASK) ==
                      (fPortQueues[0].rawTxProd & MLX4_TX_PROD_MASK)) &&
                     (image[TX_WQE_FENCE_SIZE_OFFSET] == (UInt8)TX_WQE_FENCE_SIZE(nDataSegs)) &&
                     ((nDataSegs == 0u) ||
                      ((cx3GetBe32(image + TX_WQE_BYTE_COUNT_OFFSET) == CX3_R4RAW_FRAME_BYTES) &&
                       (cx3GetBe32(image + TX_WQE_LKEY_OFFSET) == fMrLkey) &&
                       (cx3GetBe64(image + TX_WQE_ADDR_OFFSET) == fPortQueues[0].rawFramePhys)))) ? 1u : 0u;
    }

    fHw.sqRingSendDoorbell(fPortQueues[0].txQpn, fPortQueues[0].cqUarStart, /*verbose=*/false);
    fPortQueues[0].rawTxProd = fPortQueues[0].rawTxProd + 1u;

    t0 = r4RawNowMs();
    for (;;) {
        UInt8 cqe[CQE_LEGACY_SIZE];
        UInt32 cons;

        memset(cqe, 0, sizeof(cqe));
        if (fHw.cqNextCqe(fPortQueues[0].txCqRing, fPortQueues[0].txCqNent, /*cqeFactor=*/1, fPortQueues[0].rawTxCons, cqe)) {
            cons = fPortQueues[0].rawTxCons;
            if (comps == 0u) {
                if (outOpcode) *outOpcode = cqe[CQE_OWNER_SR_OPCODE_OFFSET] & MLX4_CQE_OPCODE_MASK;
                if (outStatus) *outStatus = cx3GetBe16(cqe + CQE_STATUS_OFFSET);
                LOG("txprobe: a completion ARRIVED for the opcode-0x%02x post -- CQE opcode "
                    "0x%02x, status 0x%04x, wqe_index %u, byte_cnt %u. Whatever else is true, "
                    "the hardware fetched and executed a descriptor from this send queue.",
                    (unsigned)(opcode & 0xffu),
                    (unsigned)(cqe[CQE_OWNER_SR_OPCODE_OFFSET] & MLX4_CQE_OPCODE_MASK),
                    (unsigned)cx3GetBe16(cqe + CQE_STATUS_OFFSET),
                    (unsigned)cx3GetBe16(cqe + CQE_WQE_INDEX_OFFSET),
                    (unsigned)cx3GetBe32(cqe + CQE_BYTE_CNT_OFFSET));
            }
            ++comps;
            fPortQueues[0].rawTxCons = cons + 1u;
            if (fPortQueues[0].txCqRec) fHw.cqSetCi((volatile UInt32 *)fPortQueues[0].txCqRec->virt, fPortQueues[0].rawTxCons);
            /* One post produces one completion; anything further is a previous post's, and
             * draining a burst is cheaper than another wait. */
            if (comps >= 4u) break;
            continue;
        }
        if ((r4RawNowMs() - t0) >= (UInt64)waitMs) break;
        IOSleep(1);
    }
    return comps;
}

/*
 * exportTxProbe -- the three experiments, in one call, stopping as soon as one of them
 * answers the question.
 *
 * It sends NO firmware command.  Everything it does is host memory (the queue's stamps, the
 * doorbell record) or MMIO (the two doorbells), so unlike every other selector in this project
 * it cannot poison the command mailbox (docs/LOCK-IMPL.md) and cannot leave the device needing
 * a reset.  That is deliberate: it is meant to be run repeatedly on a live stack
 * ('tools/verify.sh up' then this, which is how section 51.3 was measured).
 */
/*
 * "Now plus `afterNs`", in the units IOTimerEventSource::wakeAtTime() takes.
 *
 * Two conversions are needed and both are easy to get wrong in the same silent way:
 * clock_get_uptime() is NANOSECONDS (that is why the waits in this file divide by 1000000 to
 * get milliseconds), while wakeAtTime() wants absolute time in the machine's timebase, and the
 * SDK's conversion is an out-parameter call (kern/clock.h:121-123) rather than the "return the
 * ticks" form older XNU headers declare.  A tick count passed where nanoseconds belong is not
 * a crash: it is a timer that fires at the wrong time, or never.
 */
static UInt64 cx3WakeInTicks(UInt64 afterNs)
{
    UInt64 ns = 0, ticks = 0;

    clock_get_uptime(&ns);
    nanoseconds_to_absolutetime(ns + afterNs, &ticks);
    return ticks;
}

/* ------------------------------------------------------------------ */
/* Stage 10a: the interface (docs/R4-STAGE10-PLAN.md section 10)        */
/* ------------------------------------------------------------------ */

/*
 * Report the link, from the one reading this driver actually has.
 *
 * m3portup polls QUERY_PORT until the link bit is set and decodes the speed code; that is the
 * measurement, and the two fields this reads are what it stored.  Nothing here re-derives the
 * link from "the queues exist" -- a queue can exist with the cable out, and reporting ACTIVE
 * on that inference is exactly the kind of claim this project keeps having to retract.
 *
 * The status bits: VALID means the link state is known, ACTIVE means carrier is up.  A link
 * that was measured DOWN is published as VALID alone, which is what `ifconfig` shows as
 * "status: inactive" -- a state, not an error.
 */
void Cx3Mlx4::publishLinkStatus(const char *why)
{
    UInt32 status = fPorts[0].linkUp ? (kIONetworkLinkValid | kIONetworkLinkActive)
                                : kIONetworkLinkValid;
    UInt64 bps    = fPorts[0].linkUp ? (UInt64)fPorts[0].speedMbps * 1000000ull : 0ull;

    if (!fNetIface) {
        /*
         * Said rather than done: setLinkStatus() with no interface attached updates the
         * controller's own properties and nothing else, and a reader who saw only silence
         * would not know whether the link had been reported at all.
         */
        LOG("link (%s): not published -- no interface is attached yet (measured state: %s, "
            "%u Mb/s)", why, fPorts[0].linkUp ? "UP" : "DOWN", (unsigned)fPorts[0].speedMbps);
        return;
    }

    /*
     * setLinkStatus(status, activeMedium, speed, data) -- the medium is the SECOND argument
     * and the speed the third.  The medium is left null and the speed carries the truth:
     * publishing an IONetworkMedium would also change what `ifconfig` reports as the media
     * list, and that table is M5 work.
     */
    /*
     * The medium is passed once one has been published, and 0 before that.  The header requires
     * activeMedium to be an object from the published dictionary and allows 0 for "no active
     * medium" (IONetworkController.h:1069-1087), so both states are legal -- which is what makes
     * the media table a runtime choice rather than something M4 has to be re-run for.
     */
    if (!setLinkStatus(status, fMediaPublished ? fMediumAuto : (const IONetworkMedium *)0, bps))
        LOG("link (%s): setLinkStatus returned false -- the family refused the update, so "
            "`ifconfig` will not show it", why);
    LOG("link (%s): %s at %u Mb/s reported for en%u (from the port-up step's QUERY_PORT poll "
        "-- not re-queried here: this callback runs under the workloop gate, and a mailbox "
        "command would wait on the command semaphore while holding it)",
        why, fPorts[0].linkUp ? "VALID|ACTIVE" : "VALID, not active", (unsigned)fPorts[0].speedMbps,
        (unsigned)fNetUnit);
}

/*
 * enable() -- the stack is bringing the interface up.
 *
 * Why this exists at all: the base class answers kIOReturnUnsupported and `ifconfig up` goes
 * through it, so before 0.44.6 the interface could attach, match and appear, and still never
 * come up.  Nothing about the device changes here yet -- that is 10b, whose data path starts
 * where this function's marked hook is -- but the link status is re-published, because the
 * family only carries a link state to an interface that is up.
 *
 * No locks and no commands: this runs with the workloop's gate held (see the header), so a
 * command here could deadlock against a selector that holds the transaction lock while waiting
 * for an interrupt this gate has to deliver.
 */
IOReturn Cx3Mlx4::enable(IONetworkInterface *iface)
{
    if (!iface) return kIOReturnBadArgument;

    /* Family callbacks do not enter the selector gate. Count this callback in
     * the same atomic fence as TX/timer so stop cannot release its timer or
     * device dependencies while enable is still publishing/arming them. */
    if (!fNetTxAdmission.enter()) return kIOReturnNotPermitted;
    struct NetworkAdmissionExit {
        Mlx4TxAdmissionGate *gate;
        ~NetworkAdmissionExit() { gate->leave(); }
    } networkAdmissionExit = { &fNetTxAdmission };
    if (fAutoNetworkUserDisabled || fAutoNetwork.state == kCx3AutoFailed ||
        fResetCommandsBlocked || fStopRetained) {
        LOG("enable: refused -- reset recovery is blocked or driver stop retained its owners");
        return kIOReturnNotPermitted;
    }

    /* stop may close admission after this callback entered. Its drain waits
     * for our reference; cancel any wake we created before returning failure.
     * A close after this observation is handled by stop's subsequent drain
     * and timer cancellation. This is a host callback fence, not a DMA fence. */
    const auto finishEnable = [this]() -> IOReturn {
        if (!fNetTxAdmission.isClosed()) return kIOReturnSuccess;
        fNetEnabled = false;
        if (fNetTimer) fNetTimer->cancelTimeout();
        return kIOReturnNotPermitted;
    };

    if (fQuarExhausted || fHw.cmdWedged() ||
        (fPortQueues[0].txQpRec && !fPortQueues[0].txQpUp) ||
        (fPortQueues[0].qpRec && !fPortQueues[0].qpUp) ||
        (fPortQueues[0].ingressRec && !fPortQueues[0].ingressUp)) {
        LOG("enable: refused -- retention exhausted, or command/QP ownership unresolved");
        return kIOReturnNotPermitted;
    }

    /*
     * Refuse to bring the data path up on a driver whose deferred attach worker timed out.
     *
     * stop() deliberately does NOT detach the interface in that state -- detaching could race a
     * worker that may still be inside the family -- so the interface can still be attached
     * while the queues and tables are being torn down.  outputPacket already drops in that
     * case (fNetEnabled is false), but enable() used to set fNetEnabled = true
     * unconditionally, which would re-arm the timer and hand frames to a data path that is
     * being dismantled.  This is the same poison the selector path already refuses on.
     */
    if (netAttachPoisoned()) {
        LOG("enable: refused -- the deferred attach worker is poisoned (timeout/unknown "
            "state); the interface must not be brought up on a driver that is stopping");
        return kIOReturnNotPermitted;
    }

    if (fNetEnabled) {
        LOG("enable: the interface is already enabled; re-publishing the link and returning "
            "success rather than starting anything twice");
        publishLinkStatus("enable (repeat)");
        return finishEnable();
    }

    fNetEnabled = true;
    publishLinkStatus("enable");
    /*
     * Start the receive half.  wakeAtTime, not setTimeout: the absolute form cannot lose a tick
     * to the time between this call and the timer's own evaluation.  Safe to call with the
     * workloop's gate held -- the working driver on this machine calls cancelTimeout() from its
     * own disable(), which is the same situation (IOTimerEventSource keeps its own lock).
     */
    if (fNetTimer) {
        fNetTimer->wakeAtTime(cx3WakeInTicks(kNetTickNs));
        LOG("enable: en%u is up, and the receive path is running: the data path's timer polls "
            "the receive CQ every %llu us and hands frames to the stack; outputPacket posts into "
            "the send queue and the same tick reclaims what completed. Both halves are live for "
            "the first time in this build, so this is the line after which a ping can work -- "
            "and if it does not, the log says which half was silent.",
            (unsigned)fNetUnit, (unsigned long long)(kNetTickNs / 1000ull));
    } else {
        LOG("enable: en%u is up (which is what this callback has to prove -- the family's own "
            "enable() answers kIOReturnUnsupported), but there is NO data path timer: netup did "
            "not manage to create one, so no frame will be delivered. The kernel log says why",
            (unsigned)fNetUnit);
    }
    return finishEnable();
}

/*
 * disable() -- the client is closing the controller (for example `ifconfig en2 down`).
 *
 * The documented contract is "stop the hardware and deallocate whatever enable() allocated"
 * (IONetworkController.h:1244-1248).  enable() allocates nothing yet, so there is nothing to
 * deallocate; what this has to do today is stop claiming the link is active, because an
 * interface that is down with an ACTIVE link published is a state nothing else in the system
 * would correct.
 */
IOReturn Cx3Mlx4::disable(IONetworkInterface *iface)
{
    if (!iface) return kIOReturnBadArgument;
    /* stop owns disabling delivery and closing/draining this gate. A
     * family close arriving during synchronous detach needs an idempotent
     * success without touching timer/interface dependencies being retired. */
    if (!fNetTxAdmission.enter()) return kIOReturnSuccess;
    struct NetworkAdmissionExit {
        Mlx4TxAdmissionGate *gate;
        ~NetworkAdmissionExit() { gate->leave(); }
    } networkAdmissionExit = { &fNetTxAdmission };

    if (!fNetEnabled) {
        LOG("disable: the interface was not enabled; nothing to stop");
        return kIOReturnSuccess;
    }
    fNetEnabled = false;
    /*
     * Stop the receive path first: cancelTimeout() and not a removal, because the timer object
     * outlives an interface being brought down -- it is released by netServiceStop() when the
     * driver goes away.
     */
    if (fNetTimer) fNetTimer->cancelTimeout();
    if (fNetIface) setLinkStatus(kIONetworkLinkValid, /*activeMedium=*/0, /*speed=*/0ull);
    LOG("disable: en%u is down; the link is published as valid but not active and the receive "
        "timer is cancelled. No device state was touched: the queues and the port stay exactly "
        "as the chain left them, so bringing the interface back up resumes on the same rings",
        (unsigned)fNetUnit);
    return kIOReturnSuccess;
}

/*
 * The MTU path: what this controller can carry, and the callback that changes it.
 *
 * Read the declaration in the header for why these were missing and what the working driver on
 * this machine does.  What matters while reading the arithmetic is that THREE conventions meet
 * here, and confusing them is how a driver ends up advertising a frame it cannot carry:
 *
 *   - the family's question and answer (get/setMaxPacketSize) are in L2 terms INCLUDING the
 *     FCS: 1518 for a 1500 MTU (IOEthernetController.h:45, kIOEthernetMaxPacketSize);
 *   - outputPacket and inputPacket carry L2 frames WITHOUT the FCS: 1514 for the same MTU;
 *   - SET_PORT general's mtu field is a WIRE size including the VLAN tag this driver does not
 *     use and the FCS: 1500 + 22 = 1522 (mlx4_port.c:1306-1308, and what m3portup programs).
 *
 * Everything inside this driver is done in the middle convention -- the stack's -- and the FCS
 * is added only at the boundary, which is also why the header's 1518 appears nowhere in this
 * file: it is derived, not written down.
 */
UInt32 Cx3Mlx4::netStackFrameMax(UInt32 *outPortBound, UInt32 *outRxBound,
                                 UInt32 *outTxBound) const
{
    UInt32 vlanFcs   = (UInt32)(MLX4_ETH_VLAN_LEN + MLX4_ETH_FCS_LEN);
    UInt32 portBound = 0, rxBound = 0, txBound = 0, m = 0;

    /*
     * The port: what SET_PORT was told, minus the VLAN and FCS allowances this driver does not
     * put on the wire.  0 means m3portup has not run in this boot, i.e. the port is at whatever
     * the firmware left it at -- unknown, and skipped below rather than read as a size.
     */
    if (fPorts[0].frameMtu > vlanFcs) portBound = fPorts[0].frameMtu - vlanFcs;

    /*
     * The receive ring: the DMA buffer a frame has to fit in.  The CQE's byte count INCLUDES the
     * FCS (measured: 68 = a 64-byte frame + 4) and netServiceRx drops anything above
     * fPortQueues[0].rqBufBytes, so the largest frame this driver can deliver is that minus the FCS.  The
     * buffer size is chosen by the chain's r4rq call, not by the MTU -- which is exactly why the
     * bound is read from the ring rather than computed from fNetMtu.
     */
    if (fPortQueues[0].rqBufBytes > (UInt32)MLX4_ETH_FCS_LEN)
        rxBound = fPortQueues[0].rqBufBytes - (UInt32)MLX4_ETH_FCS_LEN;

    /*
     * The transmit ring: outputPacket's own bound, byte for byte, because mbuf_copydata writes a
     * frame into that buffer and a longer frame would be a truncation rather than a refusal.
     */
    if (fNetTxBufBytes > (UInt32)MLX4_NET_IP_ALIGN)
        txBound = fNetTxBufBytes - (UInt32)MLX4_NET_IP_ALIGN;

    if (outPortBound) *outPortBound = portBound;
    if (outRxBound)   *outRxBound   = rxBound;
    if (outTxBound)   *outTxBound   = txBound;

    /* The minimum of the bounds that are KNOWN; 0 means none of them is. */
    if (portBound && (!m || portBound < m)) m = portBound;
    if (rxBound   && (!m || rxBound   < m)) m = rxBound;
    if (txBound   && (!m || txBound   < m)) m = txBound;
    return m;
}

IOReturn Cx3Mlx4::getMaxPacketSize(UInt32 *maxSize) const
{
    UInt32 portBound = 0, rxBound = 0, txBound = 0, frame;

    if (!maxSize) return kIOReturnBadArgument;

    frame = netStackFrameMax(&portBound, &rxBound, &txBound);
    if (!frame) {
        /*
         * Nothing is built yet, so there is no measured bound to answer with.  The family's own
         * maximum stands, which is what every run got before this method existed -- a caller
         * that asks during bring-up therefore sees no change at all.
         */
        *maxSize = kIOEthernetMaxPacketSize;
        return kIOReturnSuccess;
    }

    *maxSize = frame + (UInt32)MLX4_ETH_FCS_LEN;
    /*
     * Said out loud every time rather than once, and deliberately: the family asks this rarely,
     * and the number IS the answer to "why can I not set a jumbo MTU" -- a question asked long
     * after the boot that would have logged it.  With this driver's defaults (a 1522 wire size
     * from m3portup, 2048-byte rings) it answers 1518, i.e. the family's own default, arrived at
     * from the three real bounds instead of from a constant.
     */
    LOG("getMaxPacketSize: %u byte(s) -- the smallest of port %u / receive ring %u / transmit "
        "ring %u (all in stack frames: L2 header + payload, no FCS), plus the 4-byte FCS the "
        "family counts. The port's bound is what m3portup programmed into SET_PORT, so a larger "
        "one means re-running m3portup with a larger mtu input; the receive ring's means "
        "building r4rq with a larger buffer size",
        (unsigned)*maxSize, (unsigned)portBound, (unsigned)rxBound, (unsigned)txBound);
    return kIOReturnSuccess;
}

IOReturn Cx3Mlx4::getMinPacketSize(UInt32 *minSize) const
{
    if (!minSize) return kIOReturnBadArgument;
    /*
     * The family's own minimum (IOEthernetController.h:51), and it is honest for this path: the
     * transmit side refuses anything below a bare header (kNetTxMinFrame) and the receive side
     * anything below four bytes, so 64 is the Ethernet's limit rather than one this driver adds.
     */
    *minSize = kIOEthernetMinPacketSize;
    return kIOReturnSuccess;
}

IOReturn Cx3Mlx4::setMaxPacketSize(UInt32 maxSize)
{
    UInt32 portBound = 0, rxBound = 0, txBound = 0, stackMax, frame, mtuNew;

    stackMax = netStackFrameMax(&portBound, &rxBound, &txBound);

    if (maxSize < (UInt32)kIOEthernetMinPacketSize || maxSize <= (UInt32)MLX4_ETH_FCS_LEN) {
        LOG("setMaxPacketSize(%u): refused -- below the family's minimum (%u)",
            (unsigned)maxSize, (unsigned)kIOEthernetMinPacketSize);
        return kIOReturnBadArgument;
    }
    frame = maxSize - (UInt32)MLX4_ETH_FCS_LEN;   /* the stack frame the request implies */
    if (frame < (UInt32)MLX4_ETH_HLEN) {
        LOG("setMaxPacketSize(%u): refused -- %u byte(s) after the FCS is less than an Ethernet "
            "header, so no MTU can be derived from it",
            (unsigned)maxSize, (unsigned)frame);
        return kIOReturnBadArgument;
    }
    mtuNew = frame - (UInt32)MLX4_ETH_HLEN;       /* the L3 MTU it asks for */

    if (!stackMax) {
        /*
         * Nothing measured to compare against: the chain has not run in this boot, so there are
         * no rings and no programmed port.  Accepting would be a promise about geometry that
         * does not exist yet and refusing silently would look like a driver fault during
         * bring-up, so the answer says which of the two it is.
         */
        LOG("setMaxPacketSize(%u): refused (kIOReturnUnsupported) -- nothing has been built yet "
            "in this boot, so there is no port or ring to size this against. Run the chain "
            "(tools/verify.sh run) first; getMaxPacketSize answers with the family's default "
            "until then, which is why this is a refusal and not a failure", (unsigned)maxSize);
        return kIOReturnUnsupported;
    }

    if (frame > stackMax) {
        LOG("setMaxPacketSize(%u): refused -- the %u byte stack frame it implies does not fit "
            "this path: port bound %u, receive ring %u, transmit ring %u (smallest %u, all in "
            "stack frames without the FCS). This is the whole answer to 'why can I not set a "
            "jumbo MTU yet': the port has been programmed for %u bytes of wire (m3portup's mtu "
            "input is the wire size, 1500 + %u for a 1500 MTU) and the receive ring was built "
            "with %u-byte buffers. Raising either is a chain change, not a family callback: "
            "SET_PORT and the ring allocation are device operations, and this callback runs with "
            "the workloop's gate held",
            (unsigned)maxSize, (unsigned)frame, (unsigned)portBound, (unsigned)rxBound,
            (unsigned)txBound, (unsigned)stackMax, (unsigned)fPorts[0].frameMtu,
            (unsigned)(MLX4_ETH_HLEN + MLX4_ETH_VLAN_LEN + MLX4_ETH_FCS_LEN),
            (unsigned)fPortQueues[0].rqBufBytes);
        return kIOReturnUnsupported;
    }

    /*
     * It fits, so it is recorded.  Note what is NOT done here: the receive ring is not rebuilt
     * and the transmit ring is not resized, because the bounds above already guarantee that the
     * frames this MTU allows fit BOTH as they stand -- the request was only accepted because
     * `frame <= stackMax`, and stackMax is the minimum of the two rings' own limits.  A
     * geometry that would need rebuilding is a geometry this method refuses.  (The rings follow
     * the MTU on the next netup, which is where netTxResourcesEnsure sizes the transmit one.)
     */
    LOG("setMaxPacketSize(%u): accepted -- the stack frame becomes %u byte(s) and the L3 MTU %u "
        "%s. Both rings already fit that: port bound %u, receive ring %u, transmit ring %u",
        (unsigned)maxSize, (unsigned)frame, (unsigned)mtuNew,
        (mtuNew == fNetMtu) ? "(unchanged)" : "(changed)", (unsigned)portBound,
        (unsigned)rxBound, (unsigned)txBound);
    fNetMtu = mtuNew;
    return kIOReturnSuccess;
}

/* ------------------------------------------------------------------ */
/* M5-2's prerequisite: drain, then arm, then look (NetArm)             */
/* ------------------------------------------------------------------ */

/*
 * The experiment stage 9 could not interpret.
 *
 * What it does, in this order, and every step of the order is load-bearing:
 *
 *   1. DRAIN the receive CQ.  Consume what is there and re-post each slot, exactly as the data
 *      path does -- because if the queue is not empty when the arm is rung, the arm's answer
 *      means nothing (mlx4's own usage is drain-then-arm; stage 9 armed a queue holding dozens
 *      of unconsumed completions, measured zero EQEs, and then could not tell "the doorbell does
 *      not reach the device" from "the test was invalid").
 *   2. DRAIN the event queue, so the EQE that follows is attributable to this arm rather than to
 *      something that was already sitting there.
 *   3. ARM, both halves in the reference's order (the arm word into the CQ's record, then the
 *      eight-byte doorbell carrying [sn|cmd|cqn, ci]).
 *   4. WAIT for the event queue to produce anything, and report what it was.
 *
 * It refuses in three situations rather than producing a number that would be read as a device
 * answer: while the network data path owns the CQ (its timer is the consumer then), while an
 * interrupt session is open (its action would consume the very EQE being waited for), and when
 * the CQ or the EQ does not exist in this boot.
 *
 * It sends no firmware command, so like txprobe it cannot poison the command mailbox and cannot
 * leave the device needing a reset.
 */
IOReturn Cx3Mlx4::exportNetArm(const UInt64 *in, UInt64 *out)
{
    UInt32 waitMs = in ? (UInt32)in[kNetArmInWaitMs] : 0u;
    UInt32 drained = 0u, eqeBefore = 0u, eqeType = 0xffu, eqeCqn = 0u;
    bool armed = false, found = false, empty = false;
    UInt64 t0 = 0;
    UInt32 i;

    memset(out, 0, sizeof(UInt64) * (UInt64)kNetArmOutCount);
    for (i = 0; i < (UInt32)kNetArmOutCount; ++i) out[i] = 0xffffffffu;
    out[kNetArmOutStatus] = (UInt64)kIOReturnError;

    if (waitMs == 0u) waitMs = CX3_NETARM_DEFAULT_WAIT_MS;

    /* --- the refusals first, so no number below can be mistaken for a device answer --- */
    if (!fPortQueues[0].cqUp || !fPortQueues[0].cqRing || !fPortQueues[0].cqRec || fPortQueues[0].cqCqn == 0u) {
        LOG("netarm refused: no receive completion queue in this boot, so there is nothing to "
            "drain and nothing to arm (r4cq builds it)");
        out[kNetArmOutRefused] = CX3_NETARM_NO_CQ;
        return kIOReturnSuccess;
    }
    if (fEqUnknownRetained || !fEqRing || !fEqNent) {
        LOG("netarm refused: no usable event queue in this boot; an unknown retained EQ "
            "must be handed back before its ring can be consumed");
        out[kNetArmOutRefused] = CX3_NETARM_NO_EQ;
        return kIOReturnSuccess;
    }
    if (fNetEnabled) {
        LOG("netarm refused: the network data path is ENABLED and its timer is the consumer of "
            "this queue, so draining it here would race that. Bring the interface down first "
            "(`ifconfig en%u down`) -- the queues themselves stay exactly as the chain left "
            "them", (unsigned)fNetUnit);
        out[kNetArmOutRefused] = CX3_NETARM_IFACE_UP;
        return kIOReturnSuccess;
    }
    if (fIrqObjectsUp || fIrqSource) {
        LOG("netarm refused: an interrupt session is open, and its action consumes the event "
            "queue -- it would take the very EQE this experiment waits for, and whichever of the "
            "two saw it first would decide the answer. Run 'm3irqdown' first; that is also what "
            "an HCA reset requires, so the chain's order already has it");
        out[kNetArmOutRefused] = CX3_NETARM_IRQ_UP;
        return kIOReturnSuccess;
    }

    /*
     * --- 1. drain the receive CQ ---
     *
     * Bounded, and the bound is reported: a live LAN keeps delivering broadcast frames, so
     * "empty at last" is a fact that has to be measured rather than assumed.  The completions
     * are consumed (consumer index moved and published) and their slots re-posted through the
     * data path's own recycle, so the ring is left in the state the chain had it in.
     */
    for (i = 0; i < (UInt32)CX3_NETARM_MAX_DRAIN; ++i) {
        UInt8 cqe[CQE_LEGACY_SIZE];
        UInt32 cons = fPortQueues[0].rawRxCons, slot;

        memset(cqe, 0, sizeof(cqe));
        if (!fHw.cqNextCqe(fPortQueues[0].cqRing, fPortQueues[0].cqNent, /*cqeFactor=*/1, cons, cqe)) {
            empty = true;
            break;
        }
        fPortQueues[0].rawRxCons = cons + 1u;
        if (fPortQueues[0].cqRec) fHw.cqSetCi((volatile UInt32 *)fPortQueues[0].cqRec->virt, fPortQueues[0].rawRxCons);
        ++drained;
        slot = (UInt32)((cqe[CQE_WQE_INDEX_OFFSET] << 8) | cqe[CQE_WQE_INDEX_OFFSET + 1]);
        /* wqe_index continues counting across ring laps, just as in netServiceRx. */
        slot &= fPortQueues[0].rqNent - 1u;
        if (slot < fPortQueues[0].rqNent && fPortQueues[0].rqRing && fPortQueues[0].rqBufs && fPortQueues[0].qpRec) netRxRecycle(slot);
    }
    if (empty) {
        LOG("netarm: drained %u completion(s) and the receive CQ is EMPTY -- which is the "
            "condition the arm's semantics require, and the one stage 9's measurement did not "
            "have", (unsigned)drained);
    } else {
        LOG("netarm: drained %u completion(s) and the receive CQ is STILL NOT EMPTY (the drain "
            "hit its %u-completion bound). The arm that follows cannot be interpreted: mlx4's arm "
            "means 'notify when something NEW arrives', so a run in this state says nothing about "
            "the doorbell -- it is the same invalid experiment stage 9 ran",
            (unsigned)drained, (unsigned)CX3_NETARM_MAX_DRAIN);
    }
    out[kNetArmOutDrained] = drained;
    out[kNetArmOutCqEmpty] = empty ? 1u : 0u;

    /* --- 2. drain the event queue, so what follows is attributable --- */
    for (;;) {
        UInt8 eqe[MLX4_EQ_ENTRY_SIZE];

        memset(eqe, 0, sizeof(eqe));
        if (!fHw.eqNextEqe(fEqRing, fEqNent, fEqeFactor, fEqConsIndex, eqe)) break;
        fEqConsIndex = fEqConsIndex + 1u;
        ++eqeBefore;
        if (eqeBefore >= 8u) break;
    }
    if (eqeBefore) {
        fHw.eqSetConsumerIndex(fEqDoorbell, fEqConsIndex, true);
        LOG("netarm: %u event(s) were already in the queue and were consumed first, so an EQE "
            "after this point belongs to this arm", (unsigned)eqeBefore);
    }
    out[kNetArmOutEqeBefore] = eqeBefore;

    /* --- 3. arm, in the reference's order --- */
    {
        UInt32 armCons = fPortQueues[0].rawRxCons;

        fHw.cqSetArmDb((volatile UInt32 *)fPortQueues[0].cqRec->virt, /*armSn=*/0u, MLX4_CQ_DB_REQ_NOT,
                       armCons);
        fHw.cqRingDoorbell(fPortQueues[0].cqCqn, armCons, /*armSn=*/0u, /*arm=*/true, fPortQueues[0].cqUarStart);
        armed = true;
        LOG("netarm: the receive CQ %u is armed at consumer index %u through UAR page %u, and "
            "the event queue is watched for up to %u ms. A COMP EQE (type 0x%02x) naming CQN %u "
            "is what proves the arm; anything else that arrives is reported rather than "
            "discarded", (unsigned)fPortQueues[0].cqCqn, (unsigned)armCons, (unsigned)fPortQueues[0].cqUarStart,
            (unsigned)waitMs, (unsigned)MLX4_EVENT_TYPE_COMP, (unsigned)fPortQueues[0].cqCqn);
    }
    out[kNetArmOutArmed] = 1u;
    out[kNetArmOutWaitMs] = waitMs;

    /* --- 4. wait for the event --- */
    t0 = r4RawNowMs();
    for (;;) {
        UInt8 eqe[MLX4_EQ_ENTRY_SIZE];

        memset(eqe, 0, sizeof(eqe));
        if (fHw.eqNextEqe(fEqRing, fEqNent, fEqeFactor, fEqConsIndex, eqe)) {
            eqeType = eqe[MLX4_EQE_TYPE_OFFSET];
            eqeCqn  = cx3GetBe32(eqe + MLX4_EQE_COMP_CQN_OFFSET);
            found   = true;
            fEqConsIndex = fEqConsIndex + 1u;
            fHw.eqSetConsumerIndex(fEqDoorbell, fEqConsIndex, true);
            break;
        }
        if ((r4RawNowMs() - t0) >= (UInt64)waitMs) break;
        IOSleep(1);
    }

    out[kNetArmOutEqeFound] = found ? 1u : 0u;
    out[kNetArmOutEqeType]  = found ? eqeType : 0xffu;
    out[kNetArmOutEqeCqn]   = found ? eqeCqn : 0u;
    out[kNetArmOutEqeOk]    = (found && eqeType == MLX4_EVENT_TYPE_COMP && eqeCqn == fPortQueues[0].cqCqn)
                              ? 1u : 0u;
    out[kNetArmOutRefused]  = CX3_NETARM_RAN;
    out[kNetArmOutStatus]   = 0;

    if (out[kNetArmOutEqeOk] == 1u) {
        LOG("netarm VERDICT: the arm reached the device. A COMP event naming CQN %u arrived %llu "
            "ms after the doorbell, with the queue having been emptied first -- so the last link "
            "in the receive chain (arm -> COMP EQE -> event queue) is now proved, and "
            "interrupt-driven receive is buildable",
            (unsigned)fPortQueues[0].cqCqn, (unsigned long long)(r4RawNowMs() - t0));
    } else if (found) {
        LOG("netarm VERDICT: an EQE arrived but it is not this arm's own event: type 0x%02x, "
            "CQN %u (this arm was for CQN %u and type 0x%02x). That is a different answer, not "
            "a silent one", (unsigned)eqeType, (unsigned)eqeCqn, (unsigned)fPortQueues[0].cqCqn,
            (unsigned)MLX4_EVENT_TYPE_COMP);
    } else if (!empty) {
        LOG("netarm VERDICT: no EQE, but the receive CQ was NOT empty when the arm was rung "
            "(%u completion(s) were drained and more were waiting). mlx4's arm means \"notify "
            "when something NEW arrives\", so this run says NOTHING about the doorbell -- it is "
            "the same invalid experiment stage 9 ran. Re-run with less traffic, or raise the "
            "drain bound", (unsigned)drained);
    } else {
        LOG("netarm VERDICT: the queue WAS empty, the arm was rung, and no EQE arrived within "
            "%u ms. That is now a real reading -- with the precondition satisfied -- and it says "
            "the arm doorbell does NOT reach the device on this path (or the arm needs something "
            "else the reference does not do here). Before concluding: check that a completion "
            "actually landed after the arm (the counters in 'cx3ctl netstat' and the 'net rx:' "
            "lines say whether traffic arrived at all)", (unsigned)waitMs);
    }
    return kIOReturnSuccess;
}

/* ------------------------------------------------------------------ */
/* M5-3: what this driver offloads (NetOff), receive side first         */
/* ------------------------------------------------------------------ */

/*
 * The capability answer, and the only place macOS asks for it.
 *
 * IONetworkController.h:947-969: the base answers kIOReturnUnsupported, an override "must return
 * kIOReturnSuccess", and ANY other return value counts as no support whatever the mask says.
 * There is no setChecksumSupport in either header set -- the family enables what is reported --
 * so this is a claim about capability, and the switch in netServiceRx is what makes it true per
 * frame.
 *
 * Receive only, for now: the transmit side needs new bits in the send WQE that this project's
 * transmit path depends on, and that waits until M4 has been verified (docs/M5-PLAN.md).  Saying
 * "no" for the output path is the honest half-answer, not an omission.
 */
IOReturn Cx3Mlx4::getChecksumSupport(UInt32 *checksumMask, UInt32 checksumFamily, bool isOutput)
{
    if (!checksumMask) return kIOReturnBadArgument;
    *checksumMask = 0;

    if (checksumFamily != (UInt32)kChecksumFamilyTCPIP) {
        LOG("getChecksumSupport: family 0x%x is not the TCP/IP family this driver was written "
            "against, so the answer is no", (unsigned)checksumFamily);
        return kIOReturnUnsupported;
    }
    if (isOutput) {
        /*
         * The transmit path, and this answer is a capability the FAMILY acts on: it enables the
         * ifnet's transmit offloads for what is reported here.  So the switch has to be on when
         * this is asked -- which is once, at attach -- and answering "no" otherwise is what keeps
         * the M4 test running with the behaviour it was designed against (the stack inserts every
         * checksum itself, as it always has here).
         */
        if (!fNetTxCsum) {
            LOG("getChecksumSupport: the OUTPUT path answers no -- the transmit offload switch "
                "is OFF, so the stack inserts every checksum itself. That is the default and it "
                "is what M4 is verified against; 'cx3ctl netoff txon' BEFORE the interface is "
                "attached is what turns it on (the family asks this once)");
            return kIOReturnUnsupported;
        }
        *checksumMask = kChecksumIP | kChecksumTCP | kChecksumUDP |
                        kChecksumTCPIPv6 | kChecksumUDPIPv6;
        LOG("getChecksumSupport: the OUTPUT path answers 0x%x. From now on the stack will hand "
            "frames down with a checksum request instead of inserting the checksum itself, and "
            "outputPacket turns that request into the two srcrb_flags bits. The stack still "
            "writes the pseudo-header value into the frame -- that is the model this hardware "
            "implements (docs/M5-REFERENCE.md 1.5)",
            (unsigned)*checksumMask);
        return kIOReturnSuccess;
    }
    /*
     * What the hardware can report on receive, from the CQE's status word: IP and L4 (TCP/UDP),
     * IPv4 and IPv6 alike.  This is a capability claim, so it is made whether or not the switch
     * is currently on -- the switch decides whether frames are MARKED, and an unmarked frame is
     * one the stack checksums itself.
     */
    *checksumMask = kChecksumIP | kChecksumTCP | kChecksumUDP |
                    kChecksumTCPIPv6 | kChecksumUDPIPv6;
    LOG("getChecksumSupport: receive side answers 0x%x (IP + TCP/UDP, v4 and v6). The frames "
        "are marked only while the switch is on (%s), and the transmit side is not offloaded at "
        "all yet", (unsigned)*checksumMask, fNetRxCsum ? "it is ON" : "it is OFF");
    return kIOReturnSuccess;
}

/* kCx3SelNetOff -- report the switch and its counters, and flip it. */
IOReturn Cx3Mlx4::exportNetOff(const UInt64 *in, UInt64 *out)
{
    UInt32 mode = in ? (UInt32)in[kNetOffInMode] : (UInt32)CX3_NETOFF_REPORT;
    UInt32 i;

    memset(out, 0, sizeof(UInt64) * (UInt64)kNetOffOutCount);
    for (i = 0; i < (UInt32)kNetOffOutCount; ++i) out[i] = 0xffffffffu;
    out[kNetOffOutStatus] = (UInt64)kIOReturnError;

    switch (mode) {
        case CX3_NETOFF_REPORT:
            break;
        case CX3_NETOFF_RX_ON:
            if (!fNetRxCsum) {
                fNetRxCsum = true;
                LOG("netoff: receive checksum marking is ON. Every frame whose CQE says IPOK "
                    "with the 0xffff verdict is now handed up as already verified; a frame "
                    "without that verdict is left unmarked and the stack checksums it. Nothing "
                    "was sent to the device: this is an interpretation, not an offload the "
                    "hardware has to be told about");
            }
            break;
        case CX3_NETOFF_RX_OFF:
            if (fNetRxCsum) {
                fNetRxCsum = false;
                LOG("netoff: receive checksum marking is OFF; every frame goes up unmarked and "
                    "the stack computes the checksums again. Nothing was sent to the device");
            }
            break;
        case CX3_NETOFF_TX_ON:
            if (!fNetTxCsum) {
                fNetTxCsum = true;
                LOG("netoff: transmit checksum offload is ON. getChecksumSupport() will answer "
                    "for the output path -- and the family asks that ONCE, at attach, so an "
                    "interface that already exists keeps the answer it was given. Setting this "
                    "before the chain's netup step is what makes it take effect in this boot");
            }
            break;
        case CX3_NETOFF_TX_OFF:
            if (fNetTxCsum) {
                fNetTxCsum = false;
                LOG("netoff: transmit checksum offload is OFF; from the next attach the stack "
                    "inserts transmit checksums itself. Frames already in flight are unaffected "
                    "-- their descriptors are written");
            }
            break;
        default:
            LOG("netoff refused: mode %u is not one of REPORT/RX_ON/RX_OFF", (unsigned)mode);
            out[kNetOffOutStatus] = (UInt64)kIOReturnBadArgument;
            return kIOReturnSuccess;
    }

    out[kNetOffOutMode]   = mode;
    out[kNetOffOutRxOn]   = fNetRxCsum ? 1u : 0u;
    out[kNetOffOutRxGood] = fNetRxCsumGood;
    out[kNetOffOutRxNone] = fNetRxCsumNone;
    out[kNetOffOutTxOn]   = fNetTxCsum ? 1u : 0u;
    out[kNetOffOutTxGood] = fNetTxCsumGood;
    out[kNetOffOutTxNone] = fNetTxCsumNone;
    out[kNetOffOutStatus] = 0;
    return kIOReturnSuccess;
}

/* ------------------------------------------------------------------ */
/* M5-1: the media table, and a link that can be re-read (NetLink)      */
/* ------------------------------------------------------------------ */

/*
 * Build the media dictionary, publish it, and select the one entry in it.
 *
 * One entry: `autoselect`.  The reason it is not a list is in the ABI header (a list would be a
 * claim about the PHY that this port's sources cannot map from QUERY_PORT's transceiver type),
 * and the reason it is worth publishing at all is that `getSelectedMedium()` answers 0 without a
 * dictionary (IONetworkController.h:529-536), which leaves the family with no medium to reason
 * about and `ifconfig` with no `media:` line.
 *
 * The shape is the working driver's (LucyRTL8125Setup.cpp:168-198): the dictionary takes a
 * +1 reference, the medium object is released onto it, and the publication copies the dictionary
 * so ours can be released -- except that this driver keeps both, because it needs to hand the
 * SAME medium object to setSelectedMedium() and to setLinkStatus().
 *
 * Idempotent: a second call reports what is already there.
 */
bool Cx3Mlx4::publishMediaDictionary(const char *why)
{
    IONetworkMedium *m;

    if (fMediaPublished) return true;

    fMediaDict = OSDictionary::withCapacity(1);
    if (!fMediaDict) {
        LOG("netlink (%s): could not allocate the media dictionary, so nothing is published and "
            "`ifconfig` keeps showing no media line", why);
        return false;
    }
    /*
     * IONetworkMedium::medium(type, speed, flags, index, name) -- name 0 makes the factory
     * derive it from the type, and that name is the dictionary's key, so it has to be unique.
     * With one entry that is automatic.
     *
     * Speed 0 in the medium: the ACTUAL speed is not a property of the medium here (it is what
     * the port reported), and setLinkStatus carries it explicitly.  Putting a guessed speed in
     * the medium would make `ifconfig` show it as a capability.
     */
    m = IONetworkMedium::medium((IOMediumType)kIOMediumEthernetAuto, 0u, 0u, /*index=*/0u,
                                /*name=*/0);
    if (!m) {
        LOG("netlink (%s): could not create the autoselect medium", why);
        fMediaDict->release();
        fMediaDict = 0;
        return false;
    }
    fMediumAuto = m;
    if (!IONetworkMedium::addMedium(fMediaDict, m)) {
        LOG("netlink (%s): the dictionary refused the medium (the factory's name is the key, so "
            "a duplicate would be why -- there is only one entry here)", why);
        m->release();
        fMediumAuto = 0;
        fMediaDict->release();
        fMediaDict = 0;
        return false;
    }
    if (!publishMediumDictionary(fMediaDict)) {
        LOG("netlink (%s): publishMediumDictionary refused the dictionary, so the family still "
            "has none and getSelectedMedium() still answers 0", why);
        m->release();
        fMediumAuto = 0;
        fMediaDict->release();
        fMediaDict = 0;
        return false;
    }
    fMediaPublished = true;
    /*
     * Say what was published BEFORE selecting it, because the two are different claims:
     * published is "the family has a dictionary", selected is "the driver says which entry the
     * hardware is on".  setSelectedMedium() answers false for an object that is not in the
     * published dictionary, so its return value is the check that the publication took.
     */
    LOG("netlink (%s): published a media dictionary with one entry, autoselect -- the family "
        "copies it, so this driver can be unloaded without leaving the family holding it. A "
        "richer list (10GBase-SR/CR4/T) would be a claim about the PHY that this port cannot "
        "derive from QUERY_PORT, so it is not made; the measured speed travels in "
        "setLinkStatus instead", why);
    if (setSelectedMedium(fMediumAuto))
        LOG("netlink (%s): autoselect is the selected medium, so getSelectedMedium() no longer "
            "answers 0", why);
    else
        LOG("netlink (%s): setSelectedMedium() refused autoselect, which means the object is not "
            "in the dictionary the family holds -- the publication did not take", why);
    return true;
}

/*
 * kCx3SelNetLink -- report / publish / re-read.
 *
 * The RE-QUERY mode is the honest answer to "should the link be watched in kernel context?".
 * The family does not require it (IONetworkController.h:1069-1087: it acts when the driver calls
 * setLinkStatus), and this driver cannot do it from the workloop: a link check means sending
 * QUERY_PORT, and a firmware command from the workloop-gated context can block on the command
 * semaphore while a selector that holds it waits for work this same workloop has to deliver --
 * the deadlock shape the data path's TryLock exists to avoid.  A kernel thread would be the
 * other way; until then the reading is on demand, and this is where it is taken.
 */
IOReturn Cx3Mlx4::exportNetLink(const UInt64 *in, UInt64 *out)
{
    UInt32 mode = in ? (UInt32)in[kNetLinkInMode] : (UInt32)CX3_NETLINK_REPORT;
    UInt32 i;

    memset(out, 0, sizeof(UInt64) * (UInt64)kNetLinkOutCount);
    for (i = 0; i < (UInt32)kNetLinkOutCount; ++i) out[i] = 0xffffffffu;
    out[kNetLinkOutStatus]  = (UInt64)kIOReturnError;
    out[kNetLinkOutRefused] = 0xffffffffu;

    if (mode == CX3_NETLINK_PUBLISH) {
        if (!publishMediaDictionary("requested"))
            out[kNetLinkOutStatus] = (UInt64)kIOReturnError;
        else
            out[kNetLinkOutStatus] = 0;
        out[kNetLinkOutRefused] = mode;
    } else if (mode == CX3_NETLINK_REQUERY) {
        Cx3QueryPortResult qp;
        bool upBefore = fPorts[0].linkUp;
        UInt32 speedBefore = fPorts[0].speedMbps;

        memset(&qp, 0, sizeof(qp));
        if (!fHw.queryPort(MLX4_QP_PORT_ETH, &qp)) {
            recordPortQuery(1, qp);
            LOG("netlink: REQUERY refused -- QUERY_PORT did not answer, so the link state cannot "
                "be re-read. That is a statement about the device's state (the HCA is probably "
                "not up in this boot), not about the cable");
            out[kNetLinkOutRefused] = mode;
            out[kNetLinkOutStatus]  = (UInt64)kIOReturnNotReady;
        } else {
            recordPortQuery(1, qp);
            fPorts[0].linkUp    = (qp.linkState != 0);
            fPorts[0].speedMbps = cx3LinkSpeedMbps(qp.linkSpeed);
            LOG("netlink: QUERY_PORT re-read: link %s at %u Mb/s (autoneg %s, transceiver type "
                "%u, eth mtu %u) -- %s",
                fPorts[0].linkUp ? "UP" : "DOWN", (unsigned)fPorts[0].speedMbps,
                qp.autoneg ? "done" : "not reported", (unsigned)qp.transType,
                (unsigned)qp.ethMtu,
                (fPorts[0].linkUp != upBefore || fPorts[0].speedMbps != speedBefore)
                    ? "CHANGED, so the link status is republished"
                    : "unchanged, so nothing is republished (setLinkStatus is called when the "
                      "link CHANGES, not on every reading)");
            if (fPorts[0].linkUp != upBefore || fPorts[0].speedMbps != speedBefore) {
                publishLinkStatus("netlink requery");
                out[kNetLinkOutChanged] = 1u;
            } else {
                out[kNetLinkOutChanged] = 0u;
            }
            out[kNetLinkOutRefused] = mode;
            out[kNetLinkOutStatus]  = 0;
        }
    } else if (mode == CX3_NETLINK_REPORT) {
        out[kNetLinkOutRefused] = mode;
        out[kNetLinkOutStatus]  = 0;
    } else {
        LOG("netlink refused: mode %u is not one of REPORT/PUBLISH/REQUERY (see kNetLinkInMode)",
            (unsigned)mode);
        out[kNetLinkOutStatus] = (UInt64)kIOReturnBadArgument;
        return kIOReturnSuccess;
    }

    out[kNetLinkOutMediaCount] = fMediaPublished ? 1u : 0u;
    out[kNetLinkOutSelected]   = (fMediaPublished && getSelectedMedium() == fMediumAuto) ? 1u : 0u;
    out[kNetLinkOutLinkUp]     = fPorts[0].linkUp ? 1u : 0u;
    out[kNetLinkOutSpeedMbps]  = fPorts[0].speedMbps;
    return kIOReturnSuccess;
}

/* ------------------------------------------------------------------ */
/* Stage 10b: what the data path has done (kCx3SelNetStat)              */
/* ------------------------------------------------------------------ */

/*
 * The counters, in one call, with no device access at all.
 *
 * The point is the iteration cost: everything this data path can get wrong -- the timer not
 * firing, the receive objects missing because a reset ran, frames refused by one of the ring
 * checks, the stack refusing what was handed to it, the transmit ring full -- has a counter
 * here, and asking costs one selector call instead of a log window whose start time had to
 * have been right.  The first version of this path could only be read from the kernel log.
 *
 * The values are read without the transaction lock (see the lock policy for this selector:
 * TryLock, so a call during a chain run is refused rather than queued).  The counters are
 * written by the workloop's tick, which DOES hold that lock, so a caller that gets in has a
 * consistent view of the tick's own updates; the transmit counters are also written by
 * outputPacket under its own lock, so a value read here can be one frame stale relative to
 * those.  Stating that is the point -- a reader must not treat two of these numbers as an
 * atomic pair.
 */
IOReturn Cx3Mlx4::exportNetStat(Cx3NetStatResult *res, UInt64 *out)
{
    if (!res) {
        if (out) out[kNetStatOutStatus] = (UInt64)kIOReturnBadArgument;
        return kIOReturnSuccess;
    }
    memset(res, 0, sizeof(*res));

    /*
     * The family assigns the unit asynchronously, so it is read live here rather than trusted
     * from the netup-time cache: on 2026-10-22 getUnitNumber() returned 0 at attach AND right
     * after registerService, while the kernel had already named the interface en2.
     */
    if (fNetIface) {
        UInt32 liveUnit = fNetIface->getUnitNumber();
        if (liveUnit != 0u) fNetUnit = liveUnit;
    }
    res->unit         = fNetUnit;
    res->enabled      = fNetEnabled ? 1u : 0u;
    res->linkUp       = fPorts[0].linkUp ? 1u : 0u;
    res->speedMbps    = fPorts[0].speedMbps;
    res->macValid      = fNetMacValid ? 1u : 0u;
    res->mcastMode    = fNetMcastMode ? 1u : 0u;
    res->promiscMode  = fNetPromiscMode ? 1u : 0u;
    res->mcastCount   = fNetMcastListCount;
    res->rxErrors     = fNetRxErrors;
    res->txErrors     = fNetTxErrors;
    res->timerCreated = fNetTimer ? 1u : 0u;
    res->txRingPresent = fNetTxRing ? 1u : 0u;
    res->macBits      = ((UInt64)fNetMac[0] << 40) | ((UInt64)fNetMac[1] << 32) |
                        ((UInt64)fNetMac[2] << 24) | ((UInt64)fNetMac[3] << 16) |
                        ((UInt64)fNetMac[4] << 8)  |  (UInt64)fNetMac[5];
    res->ticks        = fNetTicks;
    res->ticksSkipped = fNetTicksSkipped;
    res->noRings      = fNetRxNoRings;
    res->rxFrames     = fNetRxFrames;
    res->rxBytes      = fNetRxBytes;
    res->rxNotDelivered = fNetRxNotDelivered;
    res->rxDropped    = fNetRxDropped;
    res->rxRefills    = fNetRxRefills;
    res->rxNotOurs    = fNetRxNotOurs;
    res->txFrames     = fNetTxFrames;
    res->txCompleted  = fNetTxCompleted;
    res->txStalled    = fNetTxStalled;
    res->txDropped    = fNetTxDropped;
    res->txLockBusy   = fNetTxLockBusy;

    if (out) {
        out[kNetStatOutStatus] = 0;
        out[kNetStatOutUnit]   = (UInt64)fNetUnit;
        out[kNetStatOutNetupStep] = (UInt64)fNetupStep;
    }
    return kIOReturnSuccess;
}

/* ------------------------------------------------------------------ */
/* Stage 10b: the kernel data path (RX half)                            */
/* ------------------------------------------------------------------ */

/*
 * The timer action.  A static trampoline, because IOTimerEventSource wants a plain function;
 * the object it is given is this controller (the owner passed to timerEventSource()).
 */
void Cx3Mlx4::netTimerAction(OSObject *owner, IOTimerEventSource *src)
{
    Cx3Mlx4 *me = OSDynamicCast(Cx3Mlx4, owner);
    (void)src;
    if (!me) return;
    /* stop() closes and drains this shared network-callback gate before timer or
     * queue teardown. A timer already queued but not admitted after close must
     * not enter netTick() and touch device-facing state. */
    if (!me->fNetTxAdmission.enter()) return;
    struct TimerAdmissionExit {
        Mlx4TxAdmissionGate *gate;
        ~TimerAdmissionExit() { gate->leave(); }
    } timerAdmissionExit = { &me->fNetTxAdmission };
    me->netTick();
}

/*
 * Create the data path's timer and put it on the workloop.
 *
 * Called from netup, which runs on a user thread through the selector path -- NOT from
 * enable(): addEventSource() takes the workloop's gate, and enable() is documented to be
 * called with that gate already held, so doing it there would be a self-deadlock.  That is the
 * same split the working driver on this machine uses (its event sources are created in
 * initEventSources() during start(), and enable() only arms them).
 *
 * Idempotent: a second netup in the same boot finds the timer already there.
 */
bool Cx3Mlx4::netServiceStart(const char *why)
{
    if (fNetTimer) return true;
    if (!fWorkLoop) {
        LOG("net (%s): there is no workloop to put the data path's timer on, so no frame will "
            "be delivered. getWorkLoop() creates it on demand and the family's start() needs it, "
            "so this should be unreachable", why);
        return false;
    }

    fNetTimer = IOTimerEventSource::timerEventSource(this, &Cx3Mlx4::netTimerAction);
    if (!fNetTimer) {
        LOG("net (%s): could not create the timer event source", why);
        return false;
    }
    if (fWorkLoop->addEventSource(fNetTimer) != kIOReturnSuccess) {
        LOG("net (%s): could not add the timer to the workloop, so the data path stays off",
            why);
        fNetTimer->release();
        fNetTimer = 0;
        return false;
    }
    LOG("net (%s): the data path's timer is on the workloop. It POLLS the receive CQ; nothing "
        "is armed at the device, because the CQ arm -> COMP EQE -> EQ link is the one link "
        "stage 9 never proved (docs/10B-PRIMITIVES.md 1.6)", why);
    return true;
}

/* Take it off the workloop.  Same release order as irqReleaseObjects(): disable, remove,
 * release -- and note that this does NOT release the workloop, which the family holds. */
bool Cx3Mlx4::netTimerRelease(const char *why)
{
    if (!fNetTimer) return true;

    fNetTimer->cancelTimeout();
    if (!fWorkLoop) {
        LOG("net (%s): timer retained because its workloop is unavailable; no TX or device "
            "resource teardown may proceed", why);
        return false;
    }
    const IOReturn removeRc = fWorkLoop->removeEventSource(fNetTimer);
    if (removeRc != kIOReturnSuccess) {
        LOG("net (%s): timer retained because removeEventSource returned 0x%x; callback "
            "source ownership is uncertain", why, (unsigned)removeRc);
        return false;
    }
    fNetTimer->release();
    fNetTimer = 0;
    return true;
}

bool Cx3Mlx4::netServiceStop(const char *why)
{
    fNetEnabled = false;
    if (!netTimerRelease(why)) {
        LOG("net (%s): refusing TX and dependent resource teardown while timer removal "
            "is unconfirmed", why);
        return false;
    }
    /*
     * outputPacket runs on a stack thread and is not covered by the user-client transaction
     * lock. The timer's TX reclaimer uses this same lock with TryLock. Synchronous interface
     * termination has already removed the family's interface references; once the timer is
     * removed, this lock serializes any remaining admitted callback with mbuf accounting and
     * frame-ring release. Neither this lock nor family termination proves device DMA quiescence.
     */
    const bool txLockHeld = fNetTxLock != 0;
    if (txLockHeld) IOLockLock(fNetTxLock);
    /*
     * The frames still in flight are host objects (mbufs), and freeing them is host-side only:
     * the device never owns an mbuf, it owns the WQE that names a buffer.  The BUFFERS are a
     * different question and are handled below.
     */
    {
        UInt32 i, n = 0;
        /*
         * Counted BEFORE the mbufs are freed, because the count is what answers the next
         * question: a frame still in flight has a WQE naming its buffer, and that WQE may not
         * have been executed yet.  Reading the array after clearing it would answer "nothing
         * was in flight" to every call -- which is this project's F-family defect exactly: a
         * check whose answer is always the same one.
         */
        for (i = 0; i < (UInt32)kNetTxSlots; ++i) if (fNetTxMbuf[i]) ++n;
        for (i = 0; i < (UInt32)kNetTxSlots; ++i) {
            if (fNetTxMbuf[i]) { freePacket(fNetTxMbuf[i]); fNetTxMbuf[i] = 0; }
        }
        if (n) {
            LOG("net (%s): %u frame(s) were still in flight and their mbufs have been freed. "
                "Whether the hardware sent them is not knowable from here; what matters is that "
                "no mbuf is left owned by a completion that will never come", why, (unsigned)n);
        }
        fNetTxInFlightAtStop = n;
    }
    /*
     * The frame ring: device-facing memory, so it goes through the same gate as every other
     * device-facing allocation -- released only when nothing may still be reading it.  A frame
     * whose completion never arrived still has a WQE (possibly not yet executed) naming its
     * buffer, so "in flight is empty" is the precondition for a real free.
     */
    if (!releaseIcmOrKeep(&fNetTxRing, "network transmit frame ring",
                          fNetTxInFlightAtStop != 0)) {
        if (txLockHeld) IOLockUnlock(fNetTxLock);
        LOG("net (%s): frame-ring retention refused; keeping original owner, "
            "slot count and TX lock, and reporting incomplete stop", why);
        return false;
    }
    fNetTxSlotCount = 0;
    /*
     * The lock goes last: the ring above is device-facing and its release decision is the one
     * that matters, and a poster arriving between these two lines would find fNetTxSlotCount 0
     * and be refused before it ever reaches for the lock.
     */
    if (txLockHeld) IOLockUnlock(fNetTxLock);
    if (fNetTxLock) { IOLockFree(fNetTxLock); fNetTxLock = 0; }
    LOG("net (%s): the data path's timer is off the workloop (%llu tick(s), %llu frame(s) "
        "delivered, %llu dropped, %llu refill(s), %u ring error(s); transmit: %llu posted, %llu "
        "completed, %llu stalled, %llu dropped, %u error(s))",
        why, (unsigned long long)fNetTicks, (unsigned long long)fNetRxFrames,
        (unsigned long long)fNetRxDropped, (unsigned long long)fNetRxRefills,
        (unsigned)fNetRxErrors, (unsigned long long)fNetTxFrames,
        (unsigned long long)fNetTxCompleted, (unsigned long long)fNetTxStalled,
        (unsigned long long)fNetTxDropped, (unsigned)fNetTxErrors);
    return true;
}

/*
 * One timer fire: service the queues under the transaction lock, then re-arm.
 *
 * The lock is taken with TryLock and a failure SKIPS the tick rather than waiting, for the
 * reason in the header: this runs with the workloop's gate held, and a selector that holds the
 * transaction lock while waiting for something this same gate delivers would deadlock against
 * a blocking wait here.  A skipped tick costs one millisecond of latency on a frame that is
 * still sitting in a buffer; the alternative costs the machine.
 *
 * The re-arm happens whether or not the tick did any work, and it is not conditioned on
 * anything the service path found: a path that stops itself because one tick failed is a path
 * that silently stops delivering.
 */
void Cx3Mlx4::netTick()
{
    if (!fNetTimer) return;
    ++fNetTicks;

    if (fNetInTick) {
        /*
         * Re-entry.  It should not be possible (the workloop is serial), and that is exactly
         * why it is counted and said out loud rather than assumed away.
         */
        LOG("net: the timer action is re-entering itself; the tick is skipped. This should not "
            "be possible on a serial workloop, so it is a finding about the workloop, not about "
            "the network");
        return;
    }
    fNetInTick = true;
    if (fNetEnabled) {
        if (transactionTryEnter()) {
            netServiceRx();
            /*
             * The transmit half AFTER the receive half, and that order is not arbitrary: a frame
             * delivered by netServiceRx() can make the stack answer immediately (an ARP request
             * becomes an ARP reply inside inputPacket), so by the time this runs there may be a
             * reply already posted and worth reclaiming.  Reclaiming before delivering would just
             * find one fewer completion.
             */
            netServiceTx();
            transactionExit();
        } else {
            ++fNetTicksSkipped;
        }
    }
    fNetInTick = false;

    /* The heartbeat: a timer that is not firing and a queue that is empty look the same from
     * outside, and this is the line that tells them apart.  Once a second at 1 ms. */
    if ((fNetTicks % 1024ull) == 0ull) {
        LOG("net: %llu tick(s), %llu skipped (a selector held the transaction lock), %llu "
            "frame(s) delivered / %llu byte(s), %llu dropped, %llu refill(s), %llu not-ours, "
            "%llu taken-but-not-submitted, "
            "%u ring error(s), %llu tick(s) with no receive objects (a reset ran under the "
            "interface -- re-run the chain to put them back); transmit: %llu posted, %llu "
            "completed, %llu stalled (ring full: the stack keeps those), %llu dropped, %u "
            "error(s), %llu frame(s) handed back on lock contention; enabled %s",
            (unsigned long long)fNetTicks, (unsigned long long)fNetTicksSkipped,
            (unsigned long long)fNetRxFrames, (unsigned long long)fNetRxBytes,
            (unsigned long long)fNetRxDropped, (unsigned long long)fNetRxRefills,
            (unsigned long long)fNetRxNotOurs, (unsigned long long)fNetRxNotDelivered,
            (unsigned)fNetRxErrors,
            (unsigned long long)fNetRxNoRings, (unsigned long long)fNetTxFrames,
            (unsigned long long)fNetTxCompleted, (unsigned long long)fNetTxStalled,
            (unsigned long long)fNetTxDropped, (unsigned)fNetTxErrors,
            (unsigned long long)fNetTxLockBusy, fNetEnabled ? "yes" : "no");
    }

    if (fNetEnabled) fNetTimer->wakeAtTime(cx3WakeInTicks(kNetTickNs));
}

/*
 * Drain the receive CQ, hand what is in it to the stack, and re-post each slot.
 *
 * Read docs/10B-PRIMITIVES.md 1.2-1.5 before changing anything here.  The order of the three
 * things this does per completion is the whole function:
 *
 *   1. CONSUME -- the consumer index moves and is published into the CQ's record, before
 *      anything else can go wrong.  Once it moves, that slot is ours to read.
 *   2. READ the frame out of the buffer the CQE's slot names, and hand it to the stack.
 *   3. REFILL -- rewrite the WQE and publish the producer, never before step 2 has finished
 *      reading.  Refilling first would let the hardware write a new frame into a buffer whose
 *      bytes this function is still copying.
 *
 * The three ring checks are the same three the r4raw selector makes, and they stop the refill
 * rather than guessing: a completion whose slot disagrees with the ring position means either
 * the refill wrote the wrong slot or completions were lost (the CQ wrapped), and continuing on
 * either of those hands a buffer back while a frame can still land in it.
 */
void Cx3Mlx4::netServiceRx()
{
    UInt32 nent = fPortQueues[0].rqNent;
    UInt32 served = 0;
    UInt32 i;
    UInt8 cqe[CQE_LEGACY_SIZE];

    if (!fNetIface) return;
    if (!fPortQueues[0].cqUp || !fPortQueues[0].cqRing || !fPortQueues[0].rqRing || !fPortQueues[0].rqBufs || !fPortQueues[0].qpRec) {
        /*
         * Counted, not logged.  This becomes true the moment anything resets the HCA while the
         * interface is up (the reset tears the queues down), and it stays true until the chain
         * is re-run -- so one line per millisecond here would be a log flood that buries the
         * freeze evidence this same build exists to collect.  The heartbeat line in netTick()
         * reports the count, which is where a reader looking at this state will be anyway.
         */
        ++fNetRxNoRings;
        return;
    }
    /*
     * Prime the in-flight bitmap once, the way the raw path does (exportR4Raw reads the
     * producer record and marks every slot it published).  If a frame can arrive before this
     * driver has posted anything of its own, those completions are the stage-4 WQEs coming
     * back, so every slot the chain published is in flight.  Without this, every net-path
     * completion is refused as "not in flight" and no frame is ever delivered.
     */
    if (!fNetRxPrimed) {
        UInt32 prodNow = OSSwapBigToHostInt32(((volatile UInt32 *)fPortQueues[0].qpRec->virt)[0]);
        if (prodNow == 0u) {
            ++fNetRxErrors;
            LOG("net rx: the receive producer record reads 0, so the hardware was never told "
                "that any WQE exists; refusing to invent an in-flight set and stopping the "
                "receive path");
            return;
        }
        fPortQueues[0].rawRxProd = prodNow;
        for (i = 0; i < fPortQueues[0].rqNent; ++i) r4RawRxSetInFlight(i, true);
        fNetRxPrimed = true;
        LOG("net rx: first service for this ring: the producer record reads %u (the %u WQEs "
            "stage 3 filled and stage 4 published), so every slot is in flight until a "
            "completion says otherwise; completions already in the CQ are from those frames",
            (unsigned)prodNow, (unsigned)fPortQueues[0].rqNent);
    }

    while (served < kNetRxPerTick) {
        UInt32 cons = fPortQueues[0].rawRxCons, slot, cqeIdx, qpn, byteCnt, len;
        UInt8 *frame;
        mbuf_t m;

        memset(cqe, 0, sizeof(cqe));
        if (!fHw.cqNextCqe(fPortQueues[0].cqRing, fPortQueues[0].cqNent, /*cqeFactor=*/1, cons, cqe)) break;

        /*
         * Step 1: consume.  This is deliberately the first thing that happens, and the order
         * is r4raw's (src/Cx3Mlx4.cpp, r4RawRxPoll): the index moves, then the hardware is
         * told, then the fields are read.  A CQE that has been consumed is one this driver can
         * describe from its own copy of the bytes.
         */
        fPortQueues[0].rawRxCons = cons + 1;
        if (fPortQueues[0].cqRec) fHw.cqSetCi((volatile UInt32 *)fPortQueues[0].cqRec->virt, fPortQueues[0].rawRxCons);
        ++served;

        qpn     = cx3GetBe32(cqe + CQE_VLAN_MY_QPN_OFFSET) & MLX4_CQE_QPN_MASK;
        /*
         * The completion's wqe_index is the ABSOLUTE WQE index, not the slot.  Measured
         * 2026-10-22: after the ring's first wrap it read 506 while the ring position was 250
         * (506 & 255), and comparing the two refused all 256 completions of that lap.  The
         * reference does not read this field at all (mlx4_en_rx.c uses cons_index & size_mask),
         * so the slot comes from the consumer and the field is only an independent cross-check
         * against the same absolute counter.
         */
        slot    = cons & (nent - 1u);
        cqeIdx  = (UInt32)((cqe[CQE_WQE_INDEX_OFFSET] << 8) | cqe[CQE_WQE_INDEX_OFFSET + 1]);
        byteCnt = cx3GetBe32(cqe + CQE_BYTE_CNT_OFFSET);

        if (qpn != fPortQueues[0].qpQpn) {
            /*
             * Not this queue's completion.  Consumed and counted rather than delivered: a frame
             * that belongs to another QPN is not this interface's traffic, and handing it up
             * would be inventing an answer.
             */
            ++fNetRxNotOurs;
            continue;
        }
        if (slot >= nent || cqeIdx != (cons & 0xffffu) || !r4RawRxInFlight(slot)) {
            ++fNetRxErrors;
            LOG("net rx: the completion at consumer index %u carries wqe_index %u, and this ring "
                "(%u entries) has slot %u%s. The refill stops here rather than continue on an "
                "assumption -- a buffer handed back while a frame can still land in it is the "
                "defect this check exists for. Nothing is lost that was already read.",
                (unsigned)cons, (unsigned)cqeIdx, (unsigned)nent, (unsigned)slot,
                r4RawRxInFlight(slot) ? "" : ", and it was NOT in flight");
            break;
        }
        if (byteCnt <= 4u || byteCnt > fPortQueues[0].rqBufBytes) {
            /*
             * A length the ring cannot have produced.  The slot is still re-posted (the buffer
             * itself is fine) but nothing is delivered: a frame whose length is 0 or longer
             * than the buffer is not a frame this driver can describe.
             */
            ++fNetRxErrors;
            LOG("net rx: the completion at consumer index %u reports %u byte(s), and this ring's "
                "buffers are %u byte(s): nothing is delivered for it and the slot is re-posted",
                (unsigned)cons, (unsigned)byteCnt, (unsigned)fPortQueues[0].rqBufBytes);
            netRxRecycle(slot);
            continue;
        }

        /*
         * The CQE's byte count includes the FCS (the receive path sets FCS_KEEP; measured: 68 =
         * a 64-byte frame + 4), and the stack's idea of a frame does not have one.
         */
        len   = byteCnt - 4u;
        frame = (UInt8 *)fPortQueues[0].rqBufs->virt + (UInt64)slot * (UInt64)fPortQueues[0].rqBufBytes;

        /* Step 2: deliver.  Copies into an mbuf and hands ownership to the interface -- see
         * docs/10B-MBUF-CONTRACT.md A.1/A.2: it may NOT be freed afterwards, whatever
         * inputPacket returns, and the copy is deliberate (the zero-copy route would mean
         * attaching our DMA buffer as an external cluster, which is the one thing the stage 10
         * plan forbids: two drivers on this machine have already panicked there). */
        m = allocatePacketNoWait(len);
        if (!m) {
            ++fNetRxDropped;
        } else {
            UInt32 submitted;
            bcopy(frame, mbuf_data(m), len);
            /*
             * M5-3, receive side, and it is OFF unless asked for.  The hardware checksums
             * received frames whether or not anybody asked (there is no port-level switch --
             * see docs/M5-REFERENCE.md section 1), so this costs nothing to leave enabled; what
             * makes it a switch is that a WRONG verdict passed to the stack is worse than no
             * verdict at all, and M4 has not been verified yet.
             *
             * The test is the reference driver's own conjunction (mlx4_en_rx.c:810-818): the
             * status word says IPOK and the checksum field holds the 0xffff sentinel.  An
             * unverified frame is left completely unmarked -- the stack then checksums it
             * itself, which is what every test so far has done.
             */
            if (fNetRxCsum) {
                UInt16 st = (UInt16)cx3GetBe16(cqe + CQE_STATUS_OFFSET);
                UInt16 cs = (UInt16)cx3GetBe16(cqe + CQE_CHECKSUM_OFFSET);
                UInt32 mask = 0;
                bool isV6 = (st & MLX4_CQE_STATUS_IPV6) != 0;

                if ((st & MLX4_CQE_STATUS_IPOK) && cs == (UInt16)MLX4_CQE_CHECKSUM_VALID &&
                    (st & (MLX4_CQE_STATUS_IPV4 | MLX4_CQE_STATUS_IPV6))) {
                    if (!isV6) mask |= kChecksumIP;   /* IPv6 has no header checksum */
                    if (st & MLX4_CQE_STATUS_TCP)
                        mask |= isV6 ? kChecksumTCPIPv6 : kChecksumTCP;
                    if (st & MLX4_CQE_STATUS_UDP)
                        mask |= isV6 ? kChecksumUDPIPv6 : kChecksumUDP;
                    if (mask) {
                        /*
                         * resultMask and validMask are the same set here, and the header's rule
                         * is why they have to be stated separately: "A checksum cannot be marked
                         * valid unless it has also been checked" (IONetworkController.h:971-994).
                         * The hardware verified both, so both are claimed -- and nothing is
                         * claimed about a protocol the status word did not name.
                         */
                        setChecksumResult(m, kChecksumFamilyTCPIP, mask, mask);
                        ++fNetRxCsumGood;
                    } else {
                        ++fNetRxCsumNone;
                    }
                } else {
                    ++fNetRxCsumNone;
                }
            }
            /*
             * The return value matters, and ignoring it is this project's "something that looked
             * like a measurement" (docs/DEFECTS.md D).  inputPacket returns "the number of
             * packets that were submitted to the network stack, or zero if the packet was
             * enqueued" (IONetworkInterface.h:415-416); with options = 0 there is no queue to
             * enqueue into, so a zero means the frame was NOT submitted.  Counting it as
             * delivered would make the frame counter a claim about the stack instead of about
             * this driver -- and the first thing a reader does with that number is decide
             * whether the receive path works.
             *
             * The mbuf is not freed on either path: ownership passes with the call, whatever it
             * returns.
             */
            submitted = fNetIface->inputPacket(m, len, /*options=*/0);
            if (submitted == 0u) {
                ++fNetRxNotDelivered;
                if (fNetRxNotDelivered <= 8ull) {
                    LOG("net rx: inputPacket took %u byte(s) and submitted nothing (returned 0), "
                        "so this frame did not reach the stack -- the interface is down, or its "
                        "input path refused it. The mbuf belongs to the interface either way",
                        (unsigned)len);
                }
            } else {
                ++fNetRxFrames;
                fNetRxBytes += len;
            }
            if (fNetRxFrames <= 8 || (fNetRxFrames % 64ull) == 0ull) {
                LOG("net rx: frame %llu delivered: %u byte(s) after the FCS was removed, from "
                    "slot %u (consumer %u, buffer bus 0x%llx), %02x:%02x:%02x:%02x:%02x:%02x -> "
                    "%02x:%02x:%02x:%02x:%02x:%02x ethertype 0x%02x%02x",
                    (unsigned long long)fNetRxFrames, (unsigned)len, (unsigned)slot,
                    (unsigned)cons,
                    (unsigned long long)fHw.rqBufferPhys(fPortQueues[0].rqBufs, fPortQueues[0].rqBufBytes, slot),
                    frame[6], frame[7], frame[8], frame[9], frame[10], frame[11],
                    frame[0], frame[1], frame[2], frame[3], frame[4], frame[5],
                    frame[12], frame[13]);
            }
        }

        /* Step 3: refill, and only now. */
        netRxRecycle(slot);
    }
}

/*
 * The transmit half's only allocation: one frame buffer per TXBB of the send queue.
 *
 * Called from netup -- a selector, on a user thread, with no gate held -- for the same reason
 * the timer is created there: this allocates ICM memory, and neither an allocation nor a
 * firmware call belongs in a gated callback.
 *
 * The send queue has to exist already (the chain's r4tx builds it), and its size has to fit the
 * compile-time slot arrays; both are checked and refused with a line that says which.
 *
 * No MTT entries: the send WQE names a frame's BUS ADDRESS directly in its data segment, with
 * the memory region's lkey for the access check -- which is exactly how the proven stage 9
 * transmit posts its frame, and the region this chain registers covers the physical address
 * space (r4mr's size defaults to ~0), so a fresh allocation needs no second region.
 */
bool Cx3Mlx4::netTxResourcesEnsure(const char *why)
{
    UInt32 pages;

    if (fNetTxRing) return true;
    if (!fPortQueues[0].txSq || !fPortQueues[0].txCqRing || !fPortQueues[0].txQpUp || fPortQueues[0].txSqNent == 0) {
        LOG("net tx (%s): the send queue is not built (sq %u, cq %u, qp %u, nent %u), so there "
            "is no frame ring to allocate. That is a statement about which stages have run, not "
            "about the hardware", why, fPortQueues[0].txSq ? 1u : 0u, fPortQueues[0].txCqRing ? 1u : 0u,
            fPortQueues[0].txQpUp ? 1u : 0u, (unsigned)fPortQueues[0].txSqNent);
        return false;
    }
    /*
     * The buffer size, from the interface's own MTU and the reference's allowance for what sits
     * around a frame: header 14, a VLAN tag 4, the FCS 4, and the 2 bytes of alignment the
     * receive side also reserves (MLX4_NET_IP_ALIGN).  Rounded up to a power of two -- which the
     * buffer-address arithmetic in rqBufferPhys() requires, since it computes a page and an
     * offset within it from the index.
     *
     * With MTU 1500 this is exactly kNetTxBufBytes (2048): the default path is unchanged, and
     * that is the property to check if this ever needs changing again.
     */
    {
        UInt32 mtu = 1500u, need, size = (UInt32)kNetTxBufBytes;

        if (fNetIface && fNetIface->getIfnet()) mtu = ifnet_mtu(fNetIface->getIfnet());
        if (mtu == 0u) mtu = 1500u;
        /*
         * The same three overheads the MTU path names, plus the alignment the receive side also
         * reserves.  Written as the named constants since 0.44.24 rather than as 14 + 4 + 4:
         * they are the same convention mlx4_port.c:1306-1308 clamps SET_PORT with, and one
         * convention written twice is the shape this project keeps having to fix.
         */
        need = mtu + (UInt32)MLX4_ETH_HLEN + (UInt32)MLX4_ETH_VLAN_LEN +
               (UInt32)MLX4_ETH_FCS_LEN + (UInt32)MLX4_NET_IP_ALIGN;
        while (size < need) size <<= 1;
        fNetMtu = mtu;
        fNetTxBufBytes = size;
        LOG("net tx (%s): the interface's MTU is %u, which with a header, a possible VLAN tag, "
            "the FCS and the alignment needs %u byte(s) of buffer -> %u byte(s) allocated per "
            "TXBB. With the default MTU that is %u, the size this path was designed with",
            why, (unsigned)mtu, (unsigned)need, (unsigned)size, (unsigned)kNetTxBufBytes);
    }

    if (fPortQueues[0].txSqNent > (UInt32)kNetTxSlots) {
        LOG("net tx (%s): the send queue has %u entries and this data path carries at most %u "
            "(one mbuf record and one owner byte per TXBB, sized at compile time). Refusing "
            "rather than posting frames whose records would not fit", why,
            (unsigned)fPortQueues[0].txSqNent, (unsigned)kNetTxSlots);
        return false;
    }

    pages = (fPortQueues[0].txSqNent * fNetTxBufBytes + 4095u) / 4096u;
    fNetTxRing = fHw.icmAlloc(pages, /*lowmem=*/false);
    if (!fNetTxRing) {
        LOG("net tx (%s): could not allocate %u page(s) for %u frame buffer(s) of %u byte(s); "
            "transmit stays off and the reason is this line", why, (unsigned)pages,
            (unsigned)fPortQueues[0].txSqNent, (unsigned)fNetTxBufBytes);
        return false;
    }
    if (!fNetTxLock) fNetTxLock = IOLockAlloc();
    if (!fNetTxLock) {
        LOG("net tx (%s): could not allocate the lock the poster and the reclaimer both need, "
            "so transmit stays off rather than run without it", why);
        /*
         * Safe to free here: the ring was just allocated and has NOT been given to the device
         * (no MTT entry, no doorbell, no post).  The retention rule applies from the moment the
         * device can name the memory, which is later in this function.
         */
        fHw.icmFree(fNetTxRing);
        fNetTxRing = 0;
        return false;
    }
    fNetTxSlotCount = fPortQueues[0].txSqNent;
    LOG("net tx (%s): %u frame buffer(s) of %u byte(s) in %u page(s), bus 0x%llx..0x%llx -- one "
        "per TXBB, so a completion's wqe_index names its buffer and its mbuf directly",
        why, (unsigned)fNetTxSlotCount, (unsigned)fNetTxBufBytes, (unsigned)pages,
        (unsigned long long)fNetTxRing->busLow, (unsigned long long)fNetTxRing->busHigh);
    return true;
}

/*
 * Reclaim completed sends: free each frame's mbuf and stamp the TXBB that carried it.
 *
 * This is the other half of the async design and it is the only place a transmitted frame's
 * mbuf may be freed.  The completion names the TXBB (wqe_index), the TXBB names the mbuf, and
 * the record is cleared before the free so that a repeated completion cannot free it twice.
 *
 * The stamp carries the phase that TXBB was POSTED in, taken from the record made at post time
 * (fNetTxOwner) rather than recomputed: the reference reconstructs it from its consumer count
 * because it reclaims in batches, and this port has already paid for getting it wrong once --
 * the next lap's phase instead of the slot's own left every post after the wrap completing as
 * an ERROR (docs/M2B-NOTES.md 55).
 */
void Cx3Mlx4::netServiceTx()
{
    UInt32 served = 0, nent = fPortQueues[0].txSqNent;
    UInt8 cqe[CQE_LEGACY_SIZE];

    if (!fPortQueues[0].txCqRing || !fPortQueues[0].txSq || fNetTxSlotCount == 0 || nent == 0) return;
    /*
     * The same lock outputPacket takes, and for the reason written on fNetTxLock: the reclaimer
     * and the poster are different threads.  TryLock because this runs on the workloop with its
     * gate held, and a stack thread that is mid-post holds this lock only for a copy and an
     * MMIO write -- waiting for it would stall the gate for a bounded but unknown time, and
     * skipping costs one millisecond because the next tick reclaims the same completions.
     */
    if (!fNetTxLock || !IOLockTryLock(fNetTxLock)) {
        ++fNetTxLockBusy;
        return;
    }

    while (served < (UInt32)kNetTxPerTick) {
        UInt32 cons = fPortQueues[0].rawTxCons, opcode, err, wqeIdx, isSend;
        mbuf_t m;

        memset(cqe, 0, sizeof(cqe));
        if (!fHw.cqNextCqe(fPortQueues[0].txCqRing, fPortQueues[0].txCqNent, /*cqeFactor=*/1, cons, cqe)) break;

        opcode = cqe[CQE_OWNER_SR_OPCODE_OFFSET];
        err    = ((opcode & MLX4_CQE_OPCODE_MASK) == MLX4_CQE_OPCODE_ERROR) ? 1u : 0u;
        isSend = (opcode & MLX4_CQE_IS_SEND_MASK) ? 1u : 0u;
        wqeIdx = cx3GetBe16(cqe + CQE_WQE_INDEX_OFFSET);

        fPortQueues[0].rawTxCons = cons + 1u;
        if (fPortQueues[0].txCqRec) fHw.cqSetCi((volatile UInt32 *)fPortQueues[0].txCqRec->virt, fPortQueues[0].rawTxCons);
        ++served;
        ++fNetTxCompleted;

        if (err) {
            ++fNetTxErrors;
            /* cq.h:110: 0x05 is WR_FLUSH_ERR, not proof of a bad descriptor.
             * Even a first flush means this TX QP cannot be assumed usable. Stop new
             * posts under the poster's lock, but keep draining actual CQEs below.
             * No reset, QP transition, guessed free or automatic recovery here. */
            const bool flush = cqe[CQE_ERR_SYNDROME_OFFSET] == 0x05u;
            if (flush) ++fNetTxFlushErrors;
            if (!fNetTxFault.latched) {
                fNetTxFault.latched = true;
                fNetTxFault.consumer = cons;
                fNetTxFault.producer = fPortQueues[0].rawTxProd;
                fNetTxFault.slot = wqeIdx & (nent - 1u);
                fNetTxFault.qpn = fPortQueues[0].txQpn;
                memcpy(fNetTxFault.cqe, cqe, sizeof(fNetTxFault.cqe));
                const UInt32 slot = fNetTxFault.slot;
                if (slot < fNetTxSlotCount && slot < (UInt32)kNetTxSlots) {
                    fNetTxFault.owner = fNetTxOwner[slot];
                    fNetTxFault.hadMbuf = fNetTxMbuf[slot] != 0;
                    const UInt64 offset = MLX4_SQ_WQE_OFFSET(slot);
                    if (fPortQueues[0].txSq->virt && offset + MLX4_TXBB_SIZE <= fPortQueues[0].txSq->bytes) {
                        memcpy(fNetTxFault.wqe, (UInt8 *)fPortQueues[0].txSq->virt + offset,
                               sizeof(fNetTxFault.wqe));
                        fNetTxFault.wqeValid = true;
                    }
                }
                LOG("net tx: FIRST ERROR (%s): syndrome 0x%02x vendor 0x%02x, consumer %u "
                    "producer %u wqe_index %u slot %u posted_owner %u mbuf %u WQE_valid %u "
                    "configured_qpn 0x%x cqe_qpn 0x%x; new TX blocked for controller lifetime; "
                    "CQ draining continues, root cause unconfirmed",
                    flush ? "WR_FLUSH_ERR" : "non-flush completion error",
                    (unsigned)cqe[CQE_ERR_SYNDROME_OFFSET],
                    (unsigned)cqe[CQE_ERR_VENDOR_SYNDROME_OFFSET],
                    (unsigned)cons, (unsigned)fNetTxFault.producer, (unsigned)wqeIdx,
                    (unsigned)slot, (unsigned)fNetTxFault.owner, (unsigned)fNetTxFault.hadMbuf,
                    (unsigned)fNetTxFault.wqeValid, (unsigned)fNetTxFault.qpn,
                    (unsigned)(cx3GetBe32(cqe + CQE_VLAN_MY_QPN_OFFSET) & MLX4_CQE_QPN_MASK));
                /* Fixed bound: one summary, two CQE rows and at most four WQE rows.
                 * The snapshot is taken before the completed TXBB is stamped/reused. */
                for (UInt32 row = 0; row < CQE_LEGACY_SIZE; row += 16u) {
                    const UInt8 *p = fNetTxFault.cqe + row;
                    LOG("net tx: FIRST CQE +%02x: %08x %08x %08x %08x", (unsigned)row,
                        (unsigned)cx3GetBe32(p), (unsigned)cx3GetBe32(p + 4),
                        (unsigned)cx3GetBe32(p + 8), (unsigned)cx3GetBe32(p + 12));
                }
                if (fNetTxFault.wqeValid) {
                    for (UInt32 row = 0; row < MLX4_TXBB_SIZE; row += 16u) {
                        const UInt8 *p = fNetTxFault.wqe + row;
                        LOG("net tx: FIRST WQE +%02x: %08x %08x %08x %08x", (unsigned)row,
                            (unsigned)cx3GetBe32(p), (unsigned)cx3GetBe32(p + 4),
                            (unsigned)cx3GetBe32(p + 8), (unsigned)cx3GetBe32(p + 12));
                    }
                }
            }
        }
        if (!isSend && !err) {
            /* A receive completion on the send queue's CQ: not this path's, counted. */
            continue;
        }
        /* CQE wqe_index is a 16-bit absolute WQE counter, not a TXBB slot.
         * On .45 the first completion after 255 named 256, so every later
         * mbuf was left owned by a completion already consumed.  The reference
         * mlx4_en_tx.c masks this field with size_mask before indexing records.
         * Keep the raw index in error logs above; only map it at the lookup. */
        wqeIdx &= (nent - 1u);
        if (wqeIdx >= nent || wqeIdx >= (UInt32)kNetTxSlots) {
            ++fNetTxErrors;
            if (!fNetTxFault.latched) LOG("net tx: the completion at consumer index %u names TXBB %u, and this send queue "
                "has %u entries, so no frame record matches it. Nothing is freed on a guess: a "
                "record freed for the wrong TXBB is a frame freed while the hardware may still "
                "be reading it", (unsigned)cons, (unsigned)wqeIdx, (unsigned)nent);
            continue;
        }

        m = fNetTxMbuf[wqeIdx];
        fNetTxMbuf[wqeIdx] = 0;
        if (m) freePacket(m);
        else if (!fNetTxFault.latched &&
                 (fNetTxCompleted <= 8ull || (fNetTxCompleted % 64ull) == 0ull)) {
            /*
             * A completion for a TXBB this driver has no frame record for.  Not an error by
             * itself (the txprobe experiment posts WQEs into this same queue without a record),
             * and said only while it is rare.
             */
            LOG("net tx: the completion for TXBB %u has no mbuf recorded, so nothing was freed "
                "-- either the frame was posted by the selector path (txprobe/r4raw share this "
                "queue) or the record was already cleared", (unsigned)wqeIdx);
        }
        fHw.sqStampTxbb((UInt8 *)fPortQueues[0].txSq->virt, wqeIdx, fNetTxOwner[wqeIdx]);
    }
    IOLockUnlock(fNetTxLock);
}

/*
 * Re-post one receive slot on the data path's behalf, and count it.
 *
 * The operation itself is rqRepostSlot() -- one implementation shared with the r4raw selector,
 * because two copies of a device operation is how this project has repeatedly ended up with one
 * of them fixed and the other not (docs/DEFECTS.md F17 and F20 are that shape).  What belongs
 * HERE is only what the data path adds on top: its refill counter, the shared per-slot histogram
 * the r4raw results are read against, and the error count for the one case rqRepostSlot()
 * refuses -- a slot with no bus address, where the WQE is deliberately not written rather than
 * pointing the hardware at address zero.
 *
 * This comment used to describe the three device steps as if they were here, and they were until
 * the two copies were merged (0.44.24).  A comment describing a sequence the function no longer
 * contains is the same defect family as a report that measures the wrong thing, so it is written
 * out rather than deleted: the next reader should know the sequence lives one function up, in
 * rqRepostSlot(), with the ordering rule stated in full there (rewrite the WQE and only THEN
 * publish the producer -- publishing first would hand the hardware a descriptor it can execute
 * into a buffer the driver is still reading).
 */
void Cx3Mlx4::netRxRecycle(UInt32 slot)
{
    if (!rqRepostSlot(slot)) {
        ++fNetRxErrors;
        LOG("net rx: slot %u has no address from the buffer allocation, so its WQE was NOT "
            "re-posted: a WQE naming address zero is one the hardware would fill somewhere else. "
            "The ring runs short instead, and every later completion will fail the ring checks "
            "and say so", (unsigned)slot);
        return;
    }
    ++fNetRxRefills;
    if (slot < CX3_R4RAW_MAX_RX_SLOTS) ++fPortQueues[0].rawRxRecycles[slot];
}

/*
 * The filter callbacks.  See the header for what they claim: the request is recorded, the
 * hardware is not told, and every one of them says so in the log the first time.
 *
 * All three return kIOReturnSuccess.  That is a deliberate trade and it is worth stating: a
 * refusal is the more truthful answer to "did you program this filter?", and success is the
 * answer that keeps `ifconfig en<N> up` from failing over a filter this milestone does not
 * need.  What makes success acceptable here is that the log immediately says the opposite of
 * what the return value implies -- "recorded, NOT programmed into the device" -- and that the
 * unicast and broadcast filters the M4 criteria depend on are real: they are the firmware MAC
 * table registration of stage 5, and stage 9 received live broadcast traffic through it.
 */
IOReturn Cx3Mlx4::setPromiscuousMode(bool active)
{
    fNetPromiscMode = active;
    LOG("net filters: promiscuous mode %s -- recorded, NOT programmed into the device. This "
        "driver's receive filter is the firmware MAC table it registered (this interface's "
        "address and broadcast), so frames addressed to other stations still will not be "
        "delivered. Programming a real promiscuous filter is M5 work",
        active ? "ON" : "off");
    return kIOReturnSuccess;
}

IOReturn Cx3Mlx4::setMulticastMode(bool active)
{
    fNetMcastMode = active;
    LOG("net filters: multicast mode %s -- recorded, NOT programmed into the device. The "
        "group list that follows is remembered for the log and for a later implementation; "
        "until then the device delivers unicast to this interface's address and broadcast only",
        active ? "ON" : "off");
    return kIOReturnSuccess;
}

/*
 * The group list.  Each distinct list is printed once (by count, which is what changes when a
 * station joins or leaves a group), because this is called from the workloop and a list that
 * is re-sent unchanged on every filter update would otherwise fill the log.
 */
IOReturn Cx3Mlx4::setMulticastList(IOEthernetAddress *addrs, UInt32 count)
{
    UInt32 i, k, stored = 0;
    char buf[3 * 6 + 8];
    static const char hexd[] = "0123456789abcdef";

    fNetMcastListCount = count;
    /*
     * The list is REMEMBERED, always -- because the stack calls this only when its own list
     * changes, so this call is the only chance to capture it.  What the switch decides is
     * whether the DEVICE is told.
     */
    fNetMcastStored = 0;
    for (i = 0; i < count; ++i) {
        if (stored >= (UInt32)CX3_NETFILT_MAX_ADDRS) {
            fNetMcastTruncated = count - stored;
            break;
        }
        for (k = 0; k < 6u; ++k) fNetMcastList[stored][k] = addrs ? addrs[i].bytes[k] : 0u;
        ++stored;
    }
    fNetMcastStored = stored;

    if (!addrs || count == 0u || !fNetMcastTable) {
        /*
         * Either there is nothing to program, or the switch is off -- and off is the state M4
         * is verified in.  The log says which, once per distinct list, as it always has.
         */
        if (count != fNetMcastListLogs) {
            fNetMcastListLogs = count;
            LOG("net filters: the stack asked for %u multicast address(es); %u remembered, and "
                "%s. The device's receive filter is the firmware MAC table this driver "
                "registered (this interface's address and broadcast), so IPv4 unicast and "
                "broadcast work and group addresses do not. 'cx3ctl netfilt mcaston' programs "
                "them (M5-8); turning it OFF does not undo an already-programmed table",
                (unsigned)count, (unsigned)fNetMcastStored,
                !addrs ? "the list is empty as given" :
                (count == 0u ? "the list is empty" :
                 (fNetMcastTable ? "programming follows" :
                  "the switch is OFF so the hardware is NOT told")));
        }
        return kIOReturnSuccess;
    }

    for (i = 0; i < stored && i < 4u; ++i) {
        for (k = 0; k < 6u; ++k) {
            UInt8 b = fNetMcastList[i][k];
            buf[(k * 3) + 0] = hexd[(b >> 4) & 0xf];
            buf[(k * 3) + 1] = hexd[b & 0xf];
            buf[(k * 3) + 2] = (k == 5u) ? 0 : ':';
        }
        buf[3 * 6] = 0;
        LOG("net filters:   group %u: %s", (unsigned)i, buf);
    }
    if (stored > 4u) LOG("net filters:   ...and %u more", (unsigned)(stored - 4u));
    if (fNetMcastTruncated)
        LOG("net filters: %u address(es) did not fit this driver's list of %u and are NOT "
            "programmed", (unsigned)fNetMcastTruncated, (unsigned)CX3_NETFILT_MAX_ADDRS);
    programMulticastTable("setMulticastList");
    return kIOReturnSuccess;
}

/*
 * The table sequence, in the reference's order (mlx4_en_netdev.c:905-930) and for its reason:
 *
 *   DISABLE   -- stop filtering through a table that is about to be rewritten
 *   CONFIG with the broadcast address and CLEAR -- flush it and seed it in one command
 *   CONFIG per group address
 *   ENABLE    -- start filtering through it
 *
 * Broadcast is seeded first because it is not in the stack's group list and without it the
 * port would stop accepting broadcast frames the moment the table was enabled -- which is the
 * one way this feature could break something that already works.
 *
 * A failure STOPS the sequence and does not send ENABLE: a table that is half written and then
 * enabled is worse than one that is not enabled at all (multicast stops, unicast is untouched
 * either way, because the group table does not carry the station's own address).
 */
bool Cx3Mlx4::programMulticastTable(const char *why)
{
    UInt32 fwStatus = CX3_FWSTATUS_NOT_POSTED;
    UInt32 i, k;
    UInt64 mac;
    static const UInt8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    if (!fPortQueues[0].macUp) {
        LOG("netfilt (%s): the port's MAC registration is not in place (r4ingress), so the port "
            "configuration this command belongs to has not been built. Refusing rather than "
            "sending a port command into a port that is not up", why);
        return false;
    }

    if (!fHw.setMcastFltr(MLX4_QP_PORT_ETH, 0ull, false, MLX4_MCAST_DISABLE, &fwStatus)) {
        LOG("netfilt (%s): the DISABLE step failed (fw status 0x%x), so the table is left as it "
            "was and nothing is enabled", why, (unsigned)fwStatus);
        fNetMcastLastFw = fwStatus;
        return false;
    }
    /* big-endian packing: mlx4_mac_to_u64, driver.h:92-100 */
    mac = 0; for (k = 0; k < 6u; ++k) mac = (mac << 8) | (UInt64)bcast[k];
    if (!fHw.setMcastFltr(MLX4_QP_PORT_ETH, mac, /*clear=*/true, MLX4_MCAST_CONFIG, &fwStatus)) {
        LOG("netfilt (%s): the step that flushes the table and seeds broadcast failed (fw "
            "status 0x%x); ENABLE is NOT sent, so group addresses stay unprogrammed -- which is "
            "the safe half of the failure, because a table enabled without broadcast would also "
            "stop broadcast delivery", why, (unsigned)fwStatus);
        fNetMcastLastFw = fwStatus;
        return false;
    }
    for (i = 0; i < fNetMcastStored; ++i) {
        mac = 0;
        for (k = 0; k < 6u; ++k) mac = (mac << 8) | (UInt64)fNetMcastList[i][k];
        if (!fHw.setMcastFltr(MLX4_QP_PORT_ETH, mac, false, MLX4_MCAST_CONFIG, &fwStatus)) {
            LOG("netfilt (%s): adding group %u failed (fw status 0x%x); the sequence stops here "
                "and ENABLE is not sent, so the table is disabled rather than half written",
                why, (unsigned)i, (unsigned)fwStatus);
            fNetMcastLastFw = fwStatus;
            return false;
        }
    }
    if (!fHw.setMcastFltr(MLX4_QP_PORT_ETH, 0ull, false, MLX4_MCAST_ENABLE, &fwStatus)) {
        LOG("netfilt (%s): the ENABLE step failed (fw status 0x%x): the addresses are in the "
            "table but the port is not filtering through it", why, (unsigned)fwStatus);
        fNetMcastLastFw = fwStatus;
        return false;
    }
    fNetMcastLastFw = fwStatus;
    ++fNetMcastPrograms;
    LOG("netfilt (%s): the multicast table is programmed and enabled: broadcast plus %u group "
        "address(es), in %u command(s). The port's own address is not in this table -- unicast "
        "is the firmware MAC table's business -- so a mistake here cannot take unicast away",
        why, (unsigned)fNetMcastStored, (unsigned)(fNetMcastStored + 3u));
    return true;
}

/*
 * Promiscuous mode under A0: the qpn_calc command's promisc bit, and the multicast filter off
 * while it is on.
 *
 * Every input is read rather than assumed: the base QPN is the RX QP's own number (the same
 * choice the port bring-up made), n_mac comes from a FRESH QUERY_PORT (it is a device answer),
 * and m_promisc comes from the capability bit this port has held since QUERY_DER_CAP.  Refusing
 * when any of them is unavailable is the point -- a qpn_calc with a wrong base QPN would point
 * A0's MAC table at the firmware's queues and look like a success.
 */
bool Cx3Mlx4::programPromiscuous(bool on, const char *why, UInt32 *outFwStatus)
{
    Cx3QueryPortResult qp;
    UInt32 status = CX3_FWSTATUS_NOT_POSTED;
    UInt32 mfStatus = CX3_FWSTATUS_NOT_POSTED;
    UInt32 mPromisc;

    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_NOT_POSTED;

    if (!fPortQueues[0].qpUp) {
        LOG("netfilt (%s): refused -- promiscuous mode on this card is a field of the port's "
            "qpn_calc command, and that command's base QPN is the RX QP's number. There is no RX "
            "QP in this boot, so there is no base to send. Run r4qp first", why);
        return false;
    }
    memset(&qp, 0, sizeof(qp));
    if (!fHw.queryPort(MLX4_QP_PORT_ETH, &qp)) {
        LOG("netfilt (%s): refused -- QUERY_PORT did not answer, and n_mac is one of the fields "
            "this command carries: it is a device answer (log_max_macs), not a constant this "
            "driver may invent", why);
        return false;
    }
    mPromisc = (fDevCap.flags & MLX4_DEV_CAP_FLAG_VEP_MC_STEER) ? MCAST_DIRECT : MCAST_DEFAULT;

    LOG("netfilt (%s): qpn_calc with base QPN %u (the RX QP), n_mac %u (QUERY_PORT's "
        "log_max_macs), promisc %u%s, m_promisc %u -- the same command the port bring-up sends, "
        "with one bit moved", why, (unsigned)fPortQueues[0].qpQpn, (unsigned)qp.logMaxMacs, on ? 1u : 0u,
        on ? " (accept every station's frames)" : " (the normal path again)",
        (unsigned)mPromisc);

    if (!fHw.setPortQpnCalc(MLX4_QP_PORT_ETH, fPortQueues[0].qpQpn, on ? 1u : 0u, qp.logMaxMacs, mPromisc,
                            &status)) {
        LOG("netfilt (%s): SET_PORT qpn_calc FAILED (fw status 0x%x); the port's unicast "
            "filtering is in whatever state that command left it, so re-run the chain if unicast "
            "traffic stops", why, (unsigned)status);
        fNetPromiscFw = status;
        if (outFwStatus) *outFwStatus = status;
        return false;
    }

    if (on) {
        /*
         * The reference disables the port's multicast filter whenever promiscuous mode goes on,
         * unconditionally (mlx4_en_netdev.c:790-795), because every group address is already
         * being accepted.
         */
        (void)fHw.setMcastFltr(MLX4_QP_PORT_ETH, 0ull, false, MLX4_MCAST_DISABLE, &mfStatus);
        LOG("netfilt (%s): promiscuous mode is ON, and the port's multicast table has been "
            "DISABLED with it (fw status 0x%x) -- the reference does the same, unconditionally, "
            "because accepting every station's frames includes the group ones. 'netfilt "
            "mcaston' reprograms the table and 'netfilt promiscoff' puts the unicast filter "
            "back", why, (unsigned)mfStatus);
    } else if (fNetMcastTable) {
        /*
         * ...and when it goes off, the multicast table has to be put back: the reference's
         * caller does exactly that as the next step of its rx_mode path, and this is the same
         * statement in one selector.
         */
        LOG("netfilt (%s): promiscuous mode is off; the multicast table is being reprogrammed "
            "because it was disabled when promiscuous mode went on", why);
        programMulticastTable("promisc off");
    } else {
        LOG("netfilt (%s): promiscuous mode is off and the multicast switch is off too, so no "
            "table is reprogrammed -- the device keeps whatever group table it has", why);
    }

    fNetPromiscProgrammed = on;
    fNetPromiscFw = status;
    if (outFwStatus) *outFwStatus = status;
    return true;
}

/*
 * kCx3SelNetPort -- the port's own state, and stopping it.
 *
 * The refusal while the data path is enabled is the important part: CLOSE_PORT under a live
 * interface stops traffic, and a driver that does that on request without saying so is doing a
 * silent state change.  With the interface down the port can be stopped and the port-up sequence
 * re-run with different parameters -- which is the shape a jumbo (or any MTU) change needs.
 */
IOReturn Cx3Mlx4::exportNetPort(const UInt64 *in, UInt64 *out)
{
    UInt32 mode = in ? (UInt32)in[kNetPortInMode] : (UInt32)CX3_NETPORT_REPORT;
    UInt32 i, fw = CX3_FWSTATUS_NOT_POSTED;

    memset(out, 0, sizeof(UInt64) * (UInt64)kNetPortOutCount);
    for (i = 0; i < (UInt32)kNetPortOutCount; ++i) out[i] = 0xffffffffu;
    out[kNetPortOutStatus] = (UInt64)kIOReturnError;

    switch (mode) {
        case CX3_NETPORT_REPORT:
            break;
        case CX3_NETPORT_DOWN:
            if (fNetEnabled) {
                LOG("netport refused: the network data path is enabled, and CLOSE_PORT under a "
                    "live interface would stop traffic silently. Bring the interface down first "
                    "(`ifconfig en%u down`) -- the queues are untouched by either step",
                    (unsigned)fNetUnit);
                out[kNetPortOutMode]   = mode;
                out[kNetPortOutDown]   = fPorts[0].closeConfirmed ? 1u : 0u;
                out[kNetPortOutFw]     = fPorts[0].closeFwStatus;
                out[kNetPortOutLinkUp] = fPorts[0].linkUp ? 1u : 0u;
                out[kNetPortOutStatus] = (UInt64)kIOReturnNotReady;
                return kIOReturnSuccess;
            }
            if (fPorts[0].closeConfirmed) {
                LOG("netport: the port is already stopped; nothing was sent");
                break;
            }
            if (!fHw.closePort(MLX4_QP_PORT_ETH, &fw)) {
                LOG("netport: CLOSE_PORT did not complete (fw status 0x%x). The port is in "
                    "whatever state the command left it; the link reading below is now a "
                    "statement about the past, not the present", (unsigned)fw);
                fPorts[0].closeFwStatus = fw;
                out[kNetPortOutMode]   = mode;
                out[kNetPortOutDown]   = 0u;
                out[kNetPortOutFw]     = fw;
                out[kNetPortOutLinkUp] = fPorts[0].linkUp ? 1u : 0u;
                out[kNetPortOutStatus] = (UInt64)kIOReturnError;
                return kIOReturnSuccess;
            }
            /*
             * The link reading is invalidated, not merely left alone: a stopped port has no
             * link, and the stored value came from a QUERY_PORT that is no longer true.  Saying
             * so here is what keeps publishLinkStatus() from reporting a stale "active" if
             * something asks before the port is re-opened.
             */
            fPorts[0].closeConfirmed = true;
            fPorts[0].initAccepted = false;
            fPorts[0].linkKnown = false;
            fPorts[0].closeFwStatus = fw;
            fPorts[0].linkUp = false;
            fPorts[0].speedMbps = 0;
            LOG("netport: the port is STOPPED and the link reading is cleared with it. Re-opening "
                "is the chain's port-up step ('cx3ctl m3portup 1 <mtu>'), which is also where a "
                "different MTU would be asked for -- the queues this driver built are untouched "
                "by either");
            break;
        default:
            LOG("netport refused: mode %u is not one of REPORT/DOWN", (unsigned)mode);
            out[kNetPortOutStatus] = (UInt64)kIOReturnBadArgument;
            return kIOReturnSuccess;
    }

    out[kNetPortOutMode]   = mode;
    out[kNetPortOutDown]   = fPorts[0].closeConfirmed ? 1u : 0u;
    out[kNetPortOutFw]     = fPorts[0].closeFwStatus;
    out[kNetPortOutLinkUp] = fPorts[0].linkUp ? 1u : 0u;
    out[kNetPortOutStatus] = 0;
    return kIOReturnSuccess;
}

/*
 * kCx3SelNetFilt -- report, turn on (replaying the remembered list), turn off.
 *
 * The replay is the part that makes this usable: the stack calls setMulticastList only when its
 * list changes, so between changes this selector is the only thing that can programme what was
 * already asked for.
 */
IOReturn Cx3Mlx4::exportNetFilt(const UInt64 *in, UInt64 *out)
{
    UInt32 mode = in ? (UInt32)in[kNetFiltInMode] : (UInt32)CX3_NETFILT_REPORT;
    UInt32 i;

    memset(out, 0, sizeof(UInt64) * (UInt64)kNetFiltOutCount);
    for (i = 0; i < (UInt32)kNetFiltOutCount; ++i) out[i] = 0xffffffffu;
    out[kNetFiltOutStatus] = (UInt64)kIOReturnError;

    switch (mode) {
        case CX3_NETFILT_REPORT:
            break;
        case CX3_NETFILT_MCAST_ON:
            if (!fNetMcastTable) {
                fNetMcastTable = true;
                LOG("netfilt: multicast programming is ON. %u remembered address(es) are being "
                    "written into the port's table now, and future setMulticastList calls "
                    "program directly", (unsigned)fNetMcastStored);
            }
            programMulticastTable("mcaston");
            break;
        case CX3_NETFILT_MCAST_OFF:
            if (fNetMcastTable) {
                fNetMcastTable = false;
                LOG("netfilt: multicast programming is OFF -- this driver will not write the "
                    "port's group table again. The table ITSELF is untouched: whatever is in the "
                    "device stays there until something else programs it (a power cycle, or a "
                    "chain run, which redoes the port bring-up)");
            }
            break;
        case CX3_NETFILT_PROMISC_ON:
            if (!fNetPromiscProgrammed) programPromiscuous(true, "promiscon", &fNetPromiscFw);
            else LOG("netfilt: promiscuous mode is already on; nothing was sent");
            break;
        case CX3_NETFILT_PROMISC_OFF:
            if (fNetPromiscProgrammed) programPromiscuous(false, "promiscoff", &fNetPromiscFw);
            else LOG("netfilt: promiscuous mode was not on; nothing was sent");
            break;
        default:
            LOG("netfilt refused: mode %u is not one of REPORT/MCAST_ON/MCAST_OFF/PROMISC_ON/"
                "PROMISC_OFF", (unsigned)mode);
            out[kNetFiltOutStatus] = (UInt64)kIOReturnBadArgument;
            return kIOReturnSuccess;
    }

    out[kNetFiltOutMode]      = mode;
    out[kNetFiltOutMcastOn]   = fNetMcastTable ? 1u : 0u;
    out[kNetFiltOutStored]    = fNetMcastStored;
    out[kNetFiltOutPrograms]  = fNetMcastPrograms;
    out[kNetFiltOutLastFw]    = fNetMcastLastFw;
    out[kNetFiltOutTruncated] = fNetMcastTruncated;
    out[kNetFiltOutPromiscOn] = fNetPromiscProgrammed ? 1u : 0u;
    out[kNetFiltOutPromiscFw] = fNetPromiscFw;
    out[kNetFiltOutStatus]    = 0;
    return kIOReturnSuccess;
}

IOReturn Cx3Mlx4::getHardwareAddress(IOEthernetAddress *addrP)
{
    UInt32 i;

    if (!addrP) return kIOReturnBadArgument;
    if (!fNetMacValid) {
        /*
         * Asked before the port's address was read.  Answering with zeros is the honest answer
         * -- the alternative is an address the firmware never gave -- and the log says which of
         * the two happened, because "the stack asked too early" and "this card has no address"
         * are different problems (docs/DEFECTS.md D10's rule, applied to an interface).
         */
        LOG("netup: getHardwareAddress was asked before QUERY_PORT's def_mac was read; "
            "answering with zeros rather than an address the firmware never gave");
        memset(addrP, 0, sizeof(*addrP));
        return kIOReturnSuccess;
    }
    for (i = 0; i < 6; ++i) addrP->bytes[i] = fNetMac[i];
    return kIOReturnSuccess;
}

UInt32 Cx3Mlx4::outputPacket(mbuf_t pkt, void *param)
{
    UInt32 nent, slot, prod, ownerBit, len;
    UInt64 bufPhys;
    UInt8 *bufVirt;

    (void)param;
    if (!pkt) return kIOReturnOutputDropped;
    if (!fNetTxAdmission.enter()) return kIOReturnOutputStall;
    struct TxAdmissionExit {
        Mlx4TxAdmissionGate *gate;
        ~TxAdmissionExit() { gate->leave(); }
    } txAdmissionExit = { &fNetTxAdmission };

    /*
     * Everything that would make this frame's ownership ambiguous is refused BEFORE the
     * ownership transfer, and each refusal says which one it was.  The contract, from this
     * machine's headers (docs/10B-MBUF-CONTRACT.md B):
     *
     *   kIOReturnOutputSuccess  the driver has the frame and must free it later;
     *   kIOReturnOutputDropped  the frame is gone: the driver frees it;
     *   kIOReturnOutputStall    the frame goes back to the queue, NOT the driver's to free, and
     *                           the stack may retry it later.
     *
     * Getting those backwards leaks or double-frees an mbuf, so every path below says which it
     * is -- and `kIOReturnOutputIgnored`, which the stage 10 plan once listed, does not exist in
     * any SDK on this machine.
     */
    if (!fNetEnabled || !fNetTxRing || !fPortQueues[0].txSq || !fPortQueues[0].txCqRing || !fPortQueues[0].txQpUp) {
        ++fNetTxDropped;
        freePacket(pkt);
        return kIOReturnOutputDropped;
    }
    /*
     * Released again on EVERY path below, and the length check comes first so that the lock is
     * held for one decision rather than for a function that can return five ways.
     */
    if (!fNetTxLock || !IOLockTryLock(fNetTxLock)) {
        ++fNetTxLockBusy;
        return kIOReturnOutputStall;    /* the queue keeps the frame: not ours to free */
    }

    /* Serialized with CQ error observation. This queue has no qualified recovery:
     * consume/drop this new packet once, without copying, publishing a WQE or ringing.
     * Already-posted packets remain owned by their completion/controlled stop path. */
    if (fNetTxFault.latched) {
        ++fNetTxDropped;
        IOLockUnlock(fNetTxLock);
        freePacket(pkt);
        return kIOReturnOutputDropped;
    }

    len = (UInt32)mbuf_pkthdr_len(pkt);
    if (len == 0) len = (UInt32)mbuf_len(pkt);
    if (len < (UInt32)kNetTxMinFrame || len > (fNetTxBufBytes - (UInt32)MLX4_NET_IP_ALIGN)) {
        ++fNetTxDropped;
        if (fNetTxDropped <= 8) {
            LOG("net tx: a %u byte frame is not something this driver can put on the wire "
                "(%u..%u), so it is dropped and counted. A zero here means the packet carries "
                "neither a pkthdr length nor a length in its single mbuf",
                (unsigned)len, (unsigned)kNetTxMinFrame,
                (unsigned)(fNetTxBufBytes - (UInt32)MLX4_NET_IP_ALIGN));
        }
        IOLockUnlock(fNetTxLock);
        freePacket(pkt);
        return kIOReturnOutputDropped;
    }

    /*
     * The queue's state -- and note what is NOT here: this does not take the transaction lock.
     * It can be called from inside netServiceRx() (the stack answering a received frame, which
     * is what an ARP reply is), and that call already holds the lock, which is not recursive.
     * What makes the omission safe is that the selector path refuses to run while the data path
     * is enabled, so there is one producer and one consumer and no third party (see exportR4Raw).
     */
    nent = fPortQueues[0].txSqNent;
    prod = fPortQueues[0].rawTxProd;
    slot = prod & (nent - 1u);
    ownerBit = (prod & nent) ? 1u : 0u;

    if (fNetTxMbuf[slot] != 0) {
        /*
         * No free TXBB: the ring still holds frames the hardware has not reported.  This is the
         * one return value that does not give the frame up -- the queue keeps it and may retry
         * -- and it is counted separately so that "the driver drops frames" and "the driver is
         * being outpaced" can never look like each other in a report.
         */
        ++fNetTxStalled;
        IOLockUnlock(fNetTxLock);
        return kIOReturnOutputStall;
    }

    bufPhys = fHw.rqBufferPhys(fNetTxRing, fNetTxBufBytes, slot);
    if (bufPhys == 0) {
        ++fNetTxErrors;
        LOG("net tx: TXBB %u has no address in the frame ring, so no WQE was written and the "
            "frame is dropped rather than sent from an address of zero", (unsigned)slot);
        IOLockUnlock(fNetTxLock);
        freePacket(pkt);
        return kIOReturnOutputDropped;
    }
    bufVirt = (UInt8 *)fNetTxRing->virt + (UInt64)slot * (UInt64)fNetTxBufBytes;

    /*
     * mbuf_copydata copies OUT of an mbuf chain into a caller's buffer -- the direction wanted
     * here, and the opposite of mbuf_copyback (docs/10B-MBUF-CONTRACT.md A.1.2).  Using the
     * other one would copy in the wrong direction and put a frame of stale bytes on the wire.
     */
    if (mbuf_copydata(pkt, 0, len, bufVirt) != 0) {
        ++fNetTxErrors;
        LOG("net tx: mbuf_copydata could not produce %u contiguous byte(s) for TXBB %u, so the "
            "frame is dropped rather than sent with a hole in it", (unsigned)len,
            (unsigned)slot);
        IOLockUnlock(fNetTxLock);
        freePacket(pkt);
        return kIOReturnOutputDropped;
    }

    /*
     * The descriptor, then the doorbell.  sqWriteSendWqe writes the fields, a barrier, the
     * owner word, a second barrier -- that order is what makes the owner bit the last thing the
     * hardware can see (Mlx4Hw.h's note on the writer).  The send doorbell RECORD is not
     * written: the txprobe experiment measured that this queue completes with no record written
     * at all (variant 0 won), so writing one here would add a second mechanism to a path that
     * has been shown to work without it.
     */
    /*
     * M5-3, transmit side: did the stack ask this frame's checksum to be computed by hardware?
     * getChecksumDemand() is the family's own way to ask (IONetworkController.h:1017-1021, "the
     * driver must call this to learn which checksums to compute"), and it is used rather than the
     * mbuf KPI because that whole KPI group is marked deprecated (sys/kpi_mbuf.h:52) while this
     * is not.  The request is turned into the two bits the reference driver maps it to
     * (mlx4_en_tx.c:695-702): IP_CSUM for the IP header, TCP_UDP_CSUM for the layer 4 header.
     *
     * The pseudo-header bits (IIP/ILP) are deliberately NOT set -- the reference never sets them,
     * and what this hardware implements on this path is "the stack wrote the pseudo-header value
     * into the frame, the hardware finishes it".
     */
    {
        UInt32 demand = 0;
        UInt32 csumFlags = 0;

        if (fNetTxCsum) {
            getChecksumDemand(pkt, kChecksumFamilyTCPIP, &demand);
            if (demand & kChecksumIP) csumFlags |= (UInt32)MLX4_WQE_CTRL_IP_CSUM;
            if (demand & (kChecksumTCP | kChecksumUDP |
                          kChecksumTCPIPv6 | kChecksumUDPIPv6))
                csumFlags |= (UInt32)MLX4_WQE_CTRL_TCP_UDP_CSUM;
        }
        if (csumFlags) {
            ++fNetTxCsumGood;
            if (fNetTxCsumGood <= 8ull || (fNetTxCsumGood % 64ull) == 0ull) {
                LOG("net tx: frame %llu asks for hardware checksums (demand 0x%x -> srcrb_flags "
                    "0x%x): the IP and/or L4 checksum field already holds the pseudo-header value "
                    "and the device completes it", (unsigned long long)fNetTxCsumGood,
                    (unsigned)demand, (unsigned)csumFlags);
            }
        } else {
            ++fNetTxCsumNone;
        }
        fHw.sqWriteSendWqe((UInt8 *)fPortQueues[0].txSq->virt, slot, prod, ownerBit, len, fMrLkey, bufPhys,
                           /*nDataSegs=*/1u, MLX4_OPCODE_SEND, /*outImage32=*/0,
                           /*verbose=*/false, csumFlags);
    }
    /* Ownership moves here: from this line on, the completion owns the mbuf. */
    fNetTxMbuf[slot] = pkt;
    fNetTxOwner[slot] = (UInt8)ownerBit;
    fPortQueues[0].rawTxProd = prod + 1u;
    fHw.sqRingSendDoorbell(fPortQueues[0].txQpn, fPortQueues[0].cqUarStart, /*verbose=*/false);

    ++fNetTxFrames;
    IOLockUnlock(fNetTxLock);
    if (fNetTxFrames <= 8ull || (fNetTxFrames % 64ull) == 0ull) {
        UInt8 *f = bufVirt;
        LOG("net tx: frame %llu posted: %u byte(s) in TXBB %u (prod %u, owner bit %u, buffer "
            "bus 0x%llx), %02x:%02x:%02x:%02x:%02x:%02x -> %02x:%02x:%02x:%02x:%02x:%02x "
            "ethertype 0x%02x%02x. The WQE is NOT waited for: the completion frees the mbuf",
            (unsigned long long)fNetTxFrames, (unsigned)len, (unsigned)slot, (unsigned)prod,
            (unsigned)ownerBit, (unsigned long long)bufPhys,
            f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11],
            f[12], f[13]);
    }
    return kIOReturnOutputSuccess;
}

void Cx3Mlx4::netReceiveFrame(const UInt8 *frame, UInt32 len)
{
    /*
     * 10b's entry point, and nothing calls it yet.  The receive path still ends at this
     * driver's own counters: turning a completed frame into an mbuf and handing it to the
     * stack is the step that allocates mbufs for the first time in this project, which is the
     * one thing section 3 of the stage 10 plan says to approach deliberately.
     */
    (void)frame;
    (void)len;
}

/*
 * kCx3SelNetUp -- stage 10a: the card becomes something the system can see.
 *
 * Why it is a selector and not work in start(): the device is brought up from user space by
 * this project's own tooling (reset, INIT_HCA, ICM, EQ, QPs), and start() is held read-only by
 * a build gate -- no device write, no DMA allocation -- which is what makes this machine safe
 * to reboot through dozens of times.  Attaching the interface here keeps both properties: the
 * interface appears exactly when there is a device behind it, and the boot path stays inert.
 */
/*
 * kCx3SelNetUpStop -- the interface step's first step, and then return.
 *
 * The experiment it exists for, proposed by the parallel forensics session: cut the interface
 * step short and see whether the machine still dies.
 *
 *   alive -> everything netup does AFTER reading the port's address is the fatal part, and
 *            netup's own 2/5 ... 5/5 markers then say which step (attach, publish, timer, ring);
 *   dead  -> entering this path at all is fatal, and the attach, the timer and the frame ring
 *            are all innocent -- which is also what the one death that produced no driver log
 *            whatsoever would look like from the inside.
 *
 * It sends NO command of its own: the address must already be held (published by m3portup or by
 * the standalone queryport selector), and if it is not, this refuses exactly as netup does.  So a
 * run of this cannot be confounded with the mailbox question that A was about.
 */
/*
 * Record the interface step's progress where a process that is NOT holding our lock can see it.
 *
 * Two copies on purpose.  fNetupStep is what the netstat selector publishes; the IORegistry
 * property is what `ioreg` reads.  The registry one is the instrument that survives the measured
 * failure: the machine stops less than a second after the call, so nothing written to the log gets
 * out, and netstat is a TryLock selector that is refused while this step holds the transaction
 * lock -- which is precisely the window in question.  `setProperty` takes no lock of ours.
 */
/*
 * The three hooks inside the family's attachInterface -- see the header for why they exist.
 *
 * The order the family uses (IONetworkController.h:1137 and :1256-1276) is:
 * registerWithPolicyMaker (power management, "before the initial attempt is made to attach a
 * client"), then createInterface (allocate and initialise the interface object), then
 * configureInterface (configure it).  Each hook logs on entry and on exit, so a machine that stops
 * in the middle of the attach leaves the name of the hook it was in as the last line -- the same
 * rule the selector markers use, one level deeper.
 *
 * Every one of them calls super, so the behaviour is the family's own: these are thermometers, not
 * changes.  If a death lands between two of these lines, the thing that hangs is the family's own
 * code between the hooks (the nub creation and registration that attachInterface does around
 * them), and that is a finding of a different kind.
 */
IOReturn Cx3Mlx4::registerWithPolicyMaker(IOService *policyMaker)
{
    IOReturn rc;

    LOG("attach hook 1/3: registerWithPolicyMaker in (the family's power-management step, which "
        "it calls before it tries to attach a client)");
    attachHookMark(1);
    /* Qualified with the class that DECLARES it, not with `super`: in this file `super` resolves
     * to IOService (the compiler said so when this line was first written), which does not declare
     * any of these three. */
    rc = IONetworkController::registerWithPolicyMaker(policyMaker);
    attachHookMark(0);   /* it returned: a stale "still inside" would be read as a death here */
    LOG("attach hook 1/3: registerWithPolicyMaker out (0x%x -- the base class answers "
        "kIOReturnUnsupported, and this driver does not power manage the card)", (unsigned)rc);
    return rc;
}

IONetworkInterface *Cx3Mlx4::createInterface()
{
    IONetworkInterface *iface;

    LOG("attach hook 2/3: createInterface in (the family allocates and initialises the interface "
        "object here)");
    attachHookMark(2);
    iface = IOEthernetController::createInterface();
    attachHookMark(0);
    LOG("attach hook 2/3: createInterface out (%s)", iface ? "an interface object" : "NULL");
    return iface;
}

bool Cx3Mlx4::configureInterface(IONetworkInterface *iface)
{
    bool ok;

    LOG("attach hook 3/3: configureInterface in (the family configures the object it made)");
    attachHookMark(3);
    ok = IONetworkController::configureInterface(iface);
    attachHookMark(0);
    LOG("attach hook 3/3: configureInterface out (%s)", ok ? "true" : "false");
    return ok;
}

void Cx3Mlx4::netupMark(UInt32 step)
{
    fNetupStep = step;
    setProperty("cx3NetupStep", (unsigned long long)step, 64);
}

void Cx3Mlx4::netAttachWorkerMark(UInt32 phase)
{
    fNetAttachWorker = phase;
    setProperty("cx3AttachWorker", (unsigned long long)phase, 64);
}

bool Cx3Mlx4::netAttachPoisoned()
{
    bool poisoned = false;

    if (!fNetAttachLock) return false;
    IOLockLock(fNetAttachLock);
    poisoned = fNetAttachPoisoned;
    IOLockUnlock(fNetAttachLock);
    return poisoned;
}

void Cx3Mlx4::userClientMark(UInt32 selector, UInt32 phase)
{
    setProperty("cx3ClientSelector", (unsigned long long)selector, 64);
    setProperty("cx3ClientPhase", (unsigned long long)phase, 64);
}

void Cx3Mlx4::userClientOpenMark(UInt32 phase)
{
    setProperty("cx3OpenPhase", (unsigned long long)phase, 64);
}

/*
 * The same idea one level deeper: which of the three hooks inside attachInterface is running.
 *
 * The logs for these hooks exist and are the richer record, but they are the *unreliable* channel
 * here: the measured death takes about a second, which is exactly logd's delivery window.  The
 * 15:52 run on 0.44.35 left those three lines in the kernel ring and nothing on disk, which is why
 * this copy exists.  The watcher can read it from another process while the machine is still alive,
 * and the same channel has already proved itself for the step counter.
 *
 * 0 = not inside a hook, 1 = registerWithPolicyMaker, 2 = createInterface, 3 = configureInterface.
 */
void Cx3Mlx4::attachHookMark(UInt32 hook)
{
    setProperty("cx3AttachHook", (unsigned long long)hook, 64);
}

/*
 * The two selectors, and they are thin on purpose: both walk netupRun, so the diagnostic cannot
 * take a different path than the real call.  What the caller gets back is netup's own output
 * layout either way.
 */
IOReturn Cx3Mlx4::exportNetUp(const UInt64 *in, UInt64 *out)
{
    (void)in;   /* netup takes no inputs: it is the full run, by definition */
    return netupRun(0u, out);
}

IOReturn Cx3Mlx4::exportNetUpStop(const UInt64 *in, UInt64 *out)
{
    UInt64 requested = in ? in[kNetUpStopInStopAfter] : 0u;

    /* Validate BEFORE narrowing. Never turn a diagnostic/invalid input into attach.
     * 0.44.41 clamped 6 to 5, making its no-attach self-test unreachable. */
    if (requested > 6u) return kIOReturnBadArgument;
    UInt32 stopAfter = (UInt32)requested;

    if (stopAfter == 0u) stopAfter = 1u;   /* the original cut-short form is the default */

    LOG("netupstop: entered with stopAfter=%u (1 = address only, 2 = +attach, 3 = +link publish, "
        "4 = +timer, 5 = +frame ring and no registration, 6 = gate self-test without attach)",
        (unsigned)stopAfter);
    return netupRun(stopAfter, out);
}

/*
 * The interface step itself, with a stop point.
 *
 * `stopAfter` is the whole reason this is a function rather than a selector body: 0 means "do
 * everything" (which is what the netup selector is), and 1..5 means "return once that step is
 * done", which is what the netupstop selector asks for.  ONE implementation, so the diagnostic
 * cannot walk a different path than the real call -- the alternative was a second copy of these
 * five steps, and this project has twice paid for exactly that (docs/DEFECTS.md F17, F20).
 *
 * The early exit goes through the same tail as the normal path, so the refusal code and the
 * status word are filled in exactly as they would be on a full run.
 */
IOReturn Cx3Mlx4::netupRun(UInt32 stopAfter, UInt64 *out)
{
    IOEthernetInterface *iface;
    UInt32 i;
    UInt64 refused = CX3_NETUP_RAN;
    IOReturn terminalRc = kIOReturnNotReady;

    memset(out, 0, sizeof(UInt64) * (UInt64)kNetUpOutCount);
    for (i = 0; i < (UInt32)kNetUpOutCount; ++i) out[i] = 0xffffffffu;
    out[kNetUpOutStatus] = (UInt64)kIOReturnError;

    /*
     * Step markers, and their purpose is to be the LAST line in a hard death.
     *
     * Measured 2026-10-13: this selector has been called twice on this machine and the whole
     * machine stopped both times, with `enter selector 39` as the last line anywhere in the
     * system (found in the parallel session's out-of-machine black box, because a hard death
     * leaves nothing in the unified log).  So the useful question is no longer "which selector"
     * but "which step inside it", and these lines answer exactly that: read them as a sequence,
     * and an `in` with no matching `out` names the step that hung.
     *
     * Five steps, and each is logged BEFORE its work as well as after, so that a step which is
     * entered and never left is distinguishable from one never reached at all.
     */
    LOG("netup: entered (unit %u, interface %s, and this selector HOLDS the transaction lock -- "
        "sNetUp's mode is kXactLocked, so any mailbox call that never returns takes every later "
        "command with it)", (unsigned)fNetUnit, fNetIface ? "already attached" : "not attached");
    /*
     * The step number is also published for an off-machine poller, not only logged: the measured
     * failure mode is a machine that goes silent before any line leaves the kernel, so the log is
     * exactly the record this case cannot use.
     */
    netupMark(1);

    /*
     * The address comes from the PORT-UP step's reading, not from a command sent here.
     *
     * The old form queried the device from inside this selector, on the argument that a cached
     * copy could drift from the firmware's answer.  Three things changed that:
     *
     *   - the link status already comes from the port-up step ("from the port-up step's own
     *     reading -- not re-queried here", publishLinkStatus), and the address belongs to the
     *     same set of facts: one of the two being re-queried and the other cached is the split
     *     this project keeps paying for;
     *   - the value is the one stage 5 programmed into the steering table, so taking it from the
     *     same reading is what makes "the address the device matches on" and "the address the
     *     interface reports" one value instead of two;
     *   - and, measured 2026-10-13: this selector's own QUERY_PORT stopped the whole machine four
     *     times in a row, while the identical command through the queryport selector succeeded
     *     176 ms earlier in the same state (docs/HANG-AUDIT.md 10.9).  Whatever that turns out to
     *     be, this is not the place to settle it -- and the instrumentation inside cmdBoxLocked
     *     is still waiting for it.
     *
     * What replaces the command as the readiness test is the state the driver already keeps:
     * "has a port-up step run in this instance".  That is the question the command was really
     * asking.
     */
    LOG("netup 1/5: the port's address, read from the port-up step (this selector sends no "
        "command of its own any more)");
    if (!fPorts[0].macValid) {
        refused = CX3_NETUP_NO_DEVICE;
        terminalRc = kIOReturnNotReady;
        LOG("netup refused: no port address is held, so nothing has brought this port up in this "
            "boot. Run the chain (tools/verify.sh run) first -- this is a statement about which "
            "stages have run, not about the hardware.");
    } else {
        for (i = 0; i < 6; ++i) fNetMac[i] = fPorts[0].mac[i];
        fNetMacValid = true;
        out[kNetUpOutMacOk] = 1u;
        LOG("netup 1/5: holding %02x:%02x:%02x:%02x:%02x:%02x from that reading",
            fNetMac[0], fNetMac[1], fNetMac[2], fNetMac[3], fNetMac[4], fNetMac[5]);
    }
    if (stopAfter == 1u) goto finish;   /* asked to stop after reading the address */
    netupMark(2);   /* from here on the work is attachInterface */
    /*
     * The address as one number as well, so a caller can compare it with what the system
     * ended up showing.  Filled in both branches below -- and on the refusal path it stays
     * 0xffffffff, which is not a valid MAC, so "no address" cannot be mistaken for one.
     */
    if (fNetMacValid) {
        out[kNetUpOutMacBits] = ((UInt64)fNetMac[0] << 40) | ((UInt64)fNetMac[1] << 32) |
                                ((UInt64)fNetMac[2] << 24) | ((UInt64)fNetMac[3] << 16) |
                                ((UInt64)fNetMac[4] << 8)  |  (UInt64)fNetMac[5];
    }

    /*
     * Stop point 6: test the work-loop gate before anything is attached.
     *
     * Every death so far took the machine down too fast for logd to deliver the worker's
     * own lines, which left "runAction never ran" and "attach ran and killed the kernel"
     * looking identical.  This runs the same runAction with a body that only publishes
     * cx3AttachWorker=9. runAction executes on the caller with the loop gate held;
     * it does not dispatch onto the loop thread. Success tests gate entry only.
     */
    if (stopAfter == 6u) {
        bool loopOk = netAttachWorkLoopSelfTest();

        LOG("netup: work-loop self test %s (cx3AttachWorker should now read 9)",
            loopOk ? "PASSED" : "FAILED");
        goto finish;
    }

    if (refused == CX3_NETUP_RAN && !fNetIface) {
        /*
         * LET attachInterface CREATE IT.  A null IONetworkInterface ** is the family's signal
         * that the controller should build its own interface, and that is how the working
         * driver on this machine does it: the RTL8125 port in this workspace never calls
         * `new IOEthernetInterface` -- it sets its pointer to NULL and hands over its address
         * (LucyRTL8125Ethernet.cpp:50 and :214, where the pointer is assigned BY attachInterface).
         *
         * This port built the interface by hand first, and the family refused it:
         *
         *     netup: IOEthernetInterface::init(this) failed
         *
         * -- and the MAC had already been read from the device, so the address was not why.  An
         * IOEthernetInterface initialised outside the controller's own createInterface() path is
         * missing whatever that path sets up; the header's contract is the other way round
         * anyway: "An interface object ... must be instantiated by the driver, through
         * attachInterface()" (IOEthernetController.h:181-183).
         */
        IONetworkInterface *netif = 0;
        bool attached = false;

        /*
         * 2/5, and the one step here that runs OTHER people's code.  The family builds the
         * interface and its nub, and it calls back into this driver while it does:
         * configureInterface, getHardwareAddress, getPacketFilters, getMaxPacketSize -- all of
         * which this driver implements.  None of them takes the transaction lock and none sends a
         * command (checked by reading each one: they are a copy, a record, and arithmetic), which
         * is what makes calling it from a kXactLocked selector safe.  If a future callback ever
         * does either, this call deadlocks on the lock this selector is holding -- which is why
         * the rule is written here rather than in the callback.
         */
        LOG("netup 2/5: attachInterface in (doRegister=false: the interface is created but NOT "
            "published to the stack yet -- see the note below); the call runs on a kernel "
            "thread so a family hang cannot strand the user-client transaction");
        /*
         * Cleared before the attach: a stale "inside hook N" from an earlier call would otherwise
         * be read as a death inside THAT hook, which is the one mistake this instrument must not
         * make.
         */
        attachHookMark(0);
        /*
         * doRegister = FALSE, and the family's own documentation is why.
         *
         * IONetworkController.h:1256-1284: "Before this method returns, registerService() is called
         * on the interface to start matching, which will ultimately attach the interface to the
         * networking stack.  Drivers will typically call this method from their start() method
         * after they are ready to process interface requests."  And on the parameter: "If true,
         * start interface matching before returning.  Drivers can pass false to postpone interface
         * matching, then register the interface when ready."
         *
         * This driver is not in start() -- it is a selector on a user thread, and it holds the
         * transaction lock while it runs.  With doRegister = true the stack could start driving
         * this controller (open, enable, the receive timer, and whatever the interface layer does
         * with it) *while the four steps below are still running*.  That is the one place in this
         * selector where code belonging to another subsystem runs as a consequence of what this
         * selector does, and it is exactly the difference between this call and the same call made
         * by a working driver from start().  So matching is started deliberately, at the end,
         * once there is a link published, a timer created and a frame ring allocated.
         */
        IOReturn attachRc = netAttachOnKernelThread(&attached, &netif);
        LOG("netup 2/5: attachInterface out (returned %s, interface pointer %s)",
            attached ? "true" : "false", netif ? "non-null" : "null");

        if (attachRc != kIOReturnSuccess) {
            refused = CX3_NETUP_NO_IFACE;
            terminalRc = attachRc;
            out[kNetUpOutStatus] = (UInt64)attachRc;
            LOG("netup: deferred attachInterface failed or timed out (0x%x); the user-client "
                "thread is returning instead of waiting indefinitely", (unsigned)attachRc);
        } else if (!attached) {
            refused = CX3_NETUP_NO_IFACE;
            terminalRc = kIOReturnError;
            LOG("netup: attachInterface failed -- the family would not take this controller as "
                "an interface's provider (the family creates the interface itself; its own log "
                "line says why)");
        } else if (!netif) {
            refused = CX3_NETUP_NO_IFACE;
            terminalRc = kIOReturnError;
            LOG("netup: attachInterface returned true but left the interface pointer null");
        } else {
            fNetIface = netif;
            fNetUnit  = netif->getUnitNumber();
            /*
             * fNetIface is now the claimed owner.  Clear the worker's staging slot so a
             * later stop() cannot detach and release the same interface a second time.
             */
            if (fNetAttachLock) {
                IOLockLock(fNetAttachLock);
                if (fNetAttachIface == netif) fNetAttachIface = 0;
                IOLockUnlock(fNetAttachLock);
            }
            /*
             * The link, from the port-up step's own reading rather than from "the queues
             * exist" -- see publishLinkStatus().  The call was inline here until 0.44.6, which
             * meant the same three lines existed twice (and the second copy reported ACTIVE
             * unconditionally, whatever the port stage had measured).
             */
            LOG("netup 3/5: publishLinkStatus in (setLinkStatus only -- no command, no lock: this "
                "runs with the transaction lock held)");
            if (stopAfter == 2u) goto finish;   /* asked to stop after the attach */
            netupMark(3);
            publishLinkStatus("netup attach");
            LOG("netup 3/5: publishLinkStatus out (link %s at %u Mb/s)",
                fPorts[0].linkUp ? "VALID|ACTIVE" : "VALID only", (unsigned)fPorts[0].speedMbps);
            out[kNetUpOutLinkUp] = fPorts[0].linkUp ? 1u : 0u;
            /*
             * The data path's timer, created here and not in start(): this runs on a user
             * thread through the selector path, while enable() is documented to run with the
             * workloop's gate held and adding an event source takes that gate.  The timer is
             * created whether or not the stack ever enables the interface -- it does nothing
             * until enable() arms it -- because creating it later would mean doing it from a
             * gated context, which is the deadlock this placement avoids.
             */
            LOG("netup 4/5: netServiceStart in (creates the receive timer and adds it to the "
                "workloop, which takes that workloop's gate)");
            if (stopAfter == 3u) goto finish;   /* asked to stop after publishing the link */
            netupMark(4);
            if (!netServiceStart("netup attach")) {
                LOG("netup: the interface is attached but the data path has no timer, so no "
                    "frame will be delivered and nothing will drain the receive CQ. The "
                    "attachment itself is unaffected");
            }
            LOG("netup 4/5: netServiceStart out (%s)",
                fNetTimer ? "timer source is on the workloop" : "NO timer");
            /*
             * ...and the transmit half's frame ring.  Separate from the timer because it needs
             * the send queue (which the chain builds in r4tx, upstream of this call) and because
             * an allocation and a timer are different kinds of failure: the timer failing leaves
             * a receive-only interface, the ring failing leaves a receive-only interface too,
             * but a reader has to be able to tell those apart from the log.
             */
            LOG("netup 5/5: netTxResourcesEnsure in (host frame buffers, one per TXBB)");
            if (stopAfter == 4u) goto finish;   /* asked to stop after the timer */
            netupMark(5);
            if (!netTxResourcesEnsure("netup attach")) {
                LOG("netup: the interface is attached but the transmit side has no frame ring, "
                    "so outputPacket will drop and count every frame the stack hands it");
            }
            LOG("netup 5/5: netTxResourcesEnsure out (%s)",
                fNetTxRing ? "frame ring allocated" : "NO frame ring");
            /*
             * ...and only now let the system at it.
             *
             * This is the other half of attachInterface(..., false): the family's documented
             * alternative is "postpone interface matching, then register the interface when
             * ready", and this is where it is ready -- the link is published from the port-up
             * step's own reading, the receive timer is on the workloop, and the transmit frame
             * ring exists.
             *
             * One open question, written down rather than assumed away: the header also says a
             * driver that cannot trigger matching synchronously from start() should bracket this
             * with adjustBusy(), because the IOService root's busyState is what holds off the
             * process that assigns BSD names.  This call happens far later than start(), so that
             * advice may or may not apply here -- the measurement that settles it is whether the
             * interface comes back with a BSD name (the chain's own 'ifconfig en<N>' check), and
             * if it does not, adjustBusy is the next thing to try.
             */
            /* 7 is not one of the five steps: it is the registration that now happens after them
             * all, and giving it its own number is what keeps "died in step 3" and "died while
             * registering" from reading as the same event. */
            if (stopAfter == 5u) goto finish;   /* every step done, registration deliberately not */
            fNetupStep = 7;
            LOG("netup: registering the interface now that every step above is done "
                "(registerService)");
            /*
             * No return to test: IOService::registerService() is void in this SDK (the first
             * version of this line wrote `if (!netif->registerService())`, which does not compile
             * -- and that is the honest record of it).  So the call is made and the *evidence*
             * that it worked is external: the interface appearing, which is exactly what the
             * chain's own `ifconfig en<N>` check and the M4 gate then measure.
             */
            netif->registerService();
            /*
             * The unit number is assigned BY registration.  getUnitNumber() read 0 at attach
             * (measured 2026-10-22: the log said en0 and the scalar said 0 while the kernel had
             * named the interface en2), so it is read again here, after the name exists, and
             * everything that prints or returns it uses this value.
             */
            fNetUnit = netif->getUnitNumber();
            LOG("netup OK: the interface is en%u, MAC %02x:%02x:%02x:%02x:%02x:%02x (from "
                "QUERY_PORT's def_mac), and the link is published from the port-up step's own "
                "reading: %s at %u Mb/s. Nothing has enabled it yet -- `ifconfig en%u up` is "
                "what calls enable(), and enable() is what arms the receive timer. The data path "
                "is WIRED IN THIS BUILD -- outputPacket posts into the send queue and the same "
                "tick reclaims what completed, and the timer polls the receive CQ and hands "
                "frames to the stack -- and it has NEVER RUN on this machine. This line used to "
                "say the opposite (\"THE DATA PATH IS NOT CONNECTED: outputPacket drops and "
                "counts\"), which was true until stage 10b and false after it: exactly the shape "
                "docs/DEFECTS.md's F-family collects. What says whether either half moved is "
                "values, not this sentence: `cx3ctl netstat`, and the kernel log's 'net rx:' / "
                "'net tx:' lines and the once-a-second heartbeat.",
                (unsigned)fNetUnit, fNetMac[0], fNetMac[1], fNetMac[2], fNetMac[3], fNetMac[4],
                fNetMac[5], fPorts[0].linkUp ? "UP (VALID|ACTIVE)" : "DOWN (VALID only)",
                (unsigned)fPorts[0].speedMbps, (unsigned)fNetUnit);
            out[kNetUpOutUnit] = fNetUnit;
        }
    } else if (refused == CX3_NETUP_RAN) {
        /*
         * Already attached in this boot: report the same unit rather than attaching twice.
         *
         * The link is REPUBLISHED, not assumed.  This line was a hard 1 -- "the link is up",
         * whatever the port-up step had measured -- which is the same defect the fresh path
         * above was fixed for in 0.44.6 ("the second copy reported ACTIVE unconditionally"),
         * left behind one branch below.  It is a report rather than a gate (the chain reads
         * ifconfig for the link verdict), and that is exactly why it survived: a wrong number
         * nobody judges still misleads the person reading `cx3ctl netup`.  The slot is read by
         * tools/verify.sh's link verdict as the driver's own half of that comparison, so it has
         * to mean what its name says.
         */
        out[kNetUpOutUnit]   = fNetUnit;
        out[kNetUpOutLinkUp] = fPorts[0].linkUp ? 1u : 0u;
        publishLinkStatus("netup (already attached)");
        LOG("netup: the interface is already attached (en%u); reporting it rather than creating "
            "a second one, and republishing the link from the port-up step's reading (%s)",
            (unsigned)fNetUnit, fPorts[0].linkUp ? "UP" : "DOWN");
    }

finish:
    /*
     * One tail for every path: the full run, each stop point, and the refusals.  That is what
     * makes a stopped run and a completed one comparable -- the caller reads the same slots
     * either way, and the log says which stop point it came from.
     */
    if (stopAfter) {
        LOG("netup: stopped after step %u on request (the netupstop selector's input). The steps "
            "below %u are the only ones this run took -- if the machine is alive, the fault is "
            "in a step above this number", (unsigned)stopAfter, (unsigned)stopAfter);
    }
    out[kNetUpOutRefused] = refused;
    if (refused != CX3_NETUP_RAN) out[kNetUpOutStatus] = (UInt64)terminalRc;
    else                          out[kNetUpOutStatus] = 0u;
    /*
     * The last line of the sequence, and it is what turns "no line after step N" into "step N
     * returned": a reader who sees this knows every step above came back, whatever the caller
     * does next.
     */
    LOG("netup: returning (refused code %llu, unit %u, link %u)",
        (unsigned long long)refused, (unsigned)fNetUnit, (unsigned)fPorts[0].linkUp);
    netupMark(6);   /* it returned; a sample of 6 is the "every step above came back" reading */
    return kIOReturnSuccess;
}

/*
 * Context written by the gated action and read by the calling thread_call worker.
 * runAction is synchronous on that caller, not dispatch to the loop's thread.
 */
struct Cx3AttachActionCtx {
    IONetworkInterface *netif;
    bool                attached;
    bool                ran;
};

IOReturn Cx3Mlx4::netAttachOnWorkLoopAction(OSObject *owner, void *arg0,
                                            void *arg1, void *arg2, void *arg3)
{
    Cx3Mlx4 *self = OSDynamicCast(Cx3Mlx4, owner);
    Cx3AttachActionCtx *ctx = (Cx3AttachActionCtx *)arg0;

    (void)arg1;
    (void)arg2;
    (void)arg3;
    if (!self || !ctx) return kIOReturnBadArgument;

    self->netAttachWorkerMark(kCx3AttachWorkerOnLoop);
    LOG("netAttach gated action: attachInterface in, controller work-loop gate held");
    ctx->attached = self->attachInterface(&ctx->netif, /*doRegister=*/false);
    ctx->ran = true;
    LOG("netAttach workloop action: attachInterface out (attached=%s, interface=%s)",
        ctx->attached ? "true" : "false", ctx->netif ? "non-null" : "null");
    return kIOReturnSuccess;
}

IOReturn Cx3Mlx4::netAttachLoopNoopAction(OSObject *owner, void *arg0,
                                          void *arg1, void *arg2, void *arg3)
{
    Cx3Mlx4 *self = OSDynamicCast(Cx3Mlx4, owner);
    bool *ran = (bool *)arg0;

    (void)arg1;
    (void)arg2;
    (void)arg3;
    if (!self) return kIOReturnBadArgument;

    if (ran) *ran = true;
    self->netAttachWorkerMark(kCx3AttachWorkerLoopNoop);
    LOG("netAttach gate self test: no-attach action entered with work-loop gate held");
    return kIOReturnSuccess;
}

bool Cx3Mlx4::netAttachWorkLoopSelfTest()
{
    IOWorkLoop *loop = getWorkLoop();
    bool ran = false;
    IOReturn rc;

    if (!loop) {
        LOG("netAttach workloop self test: NO WORK LOOP");
        return false;
    }
    LOG("netAttach gate self test: entering runAction on the calling thread");
    rc = loop->runAction(&Cx3Mlx4::netAttachLoopNoopAction, this, &ran);
    LOG("netAttach workloop self test: runAction returned 0x%x, ran=%s",
        (unsigned)rc, ran ? "true" : "false");
    return ran && rc == kIOReturnSuccess;
}

/*
 * Detach all interface references without making stop() wait indefinitely. The
 * dedicated worker performs the family's synchronous termination off the
 * selector/workloop thread; the worker's self-reference and transferred
 * interface references survive if this method reaches its deadline.
 */
bool Cx3Mlx4::netDetachInterfacesAndWait(UInt32 waitMs)
{
    AbsoluteTime deadline;
    wait_result_t waitResult;
    UInt32 i;
    bool queuedAlready;
    const UInt64 deadlineNs = (UInt64)waitMs * 1000000ULL;

    /* clock_interval_to_deadline accepts a UInt32 interval; refuse overflow, never truncate. */
    if (deadlineNs > 0xffffffffULL) return false;
    if (!fNetAttachLock) {
        if (fNetIface || fNetAttachIface) return false;
        if (!fNetDetachCall) return true;
        if (!thread_call_free(fNetDetachCall)) return false;
        fNetDetachCall = 0;
        return true;
    }
    IOLockLock(fNetAttachLock);
    if (fNetDetachInFlight) {
        IOLockUnlock(fNetAttachLock);
        LOG("stop: synchronous detach worker already active; refusing duplicate detach");
        return false;
    }

    fNetDetachIfaceCount = 0;
    memset(fNetDetachIfaces, 0, sizeof(fNetDetachIfaces));
    memset(fNetDetachRefCounts, 0, sizeof(fNetDetachRefCounts));
    IONetworkInterface *candidates[2] = { fNetAttachIface, fNetIface };
    for (i = 0; i < 2u; ++i) {
        IONetworkInterface *candidate = candidates[i];
        UInt32 slot;
        if (!candidate) continue;
        for (slot = 0; slot < fNetDetachIfaceCount; ++slot) {
            if (fNetDetachIfaces[slot] == candidate) break;
        }
        if (slot < fNetDetachIfaceCount) {
            ++fNetDetachRefCounts[slot];
        } else if (fNetDetachIfaceCount < 2u) {
            slot = fNetDetachIfaceCount++;
            fNetDetachIfaces[slot] = candidate;
            fNetDetachRefCounts[slot] = 1u;
        } else {
            IOLockUnlock(fNetAttachLock);
            LOG("stop: more than two interface references need detach; retaining the stop "
                "transaction rather than dropping an owner");
            return false;
        }
    }

    if (fNetDetachIfaceCount == 0u) {
        thread_call_t unusedCall = fNetDetachCall;
        IOLockUnlock(fNetAttachLock);
        if (!unusedCall) return true;
        if (!thread_call_free(unusedCall)) {
            LOG("stop: unused detach worker could not be freed; retaining its coordination lock");
            return false;
        }
        IOLockLock(fNetAttachLock);
        if (fNetDetachCall == unusedCall) fNetDetachCall = 0;
        IOLockUnlock(fNetAttachLock);
        return true;
    }
    if (!fNetDetachCall) {
        IOLockUnlock(fNetAttachLock);
        LOG("stop: interfaces exist but the synchronous detach worker is unavailable");
        return false;
    }

    fNetDetachDone = false;
    fNetDetachCallFreeFailed = false;
    fNetDetachInFlight = true;
    retain();                    /* this submission's worker reference */
    queuedAlready = thread_call_enter(fNetDetachCall);
    /* A newly queued call or the already-pending call will consume these references. */
    fNetAttachIface = 0;
    fNetIface = 0;
    IOLockUnlock(fNetAttachLock);
    if (queuedAlready) {
        /* The pending invocation owns its earlier retain; this submission added no callback. */
        release();
        LOG("stop: detach worker was unexpectedly already pending; retain teardown state");
        return false;
    }

    clock_interval_to_deadline((UInt32)deadlineNs, kNanosecondScale, &deadline);
    IOLockLock(fNetAttachLock);
    while (!fNetDetachDone) {
        waitResult = IOLockSleepDeadline(fNetAttachLock, &fNetDetachDone,
                                         deadline, THREAD_UNINT);
        if (waitResult != THREAD_AWAKENED) break;
    }
    const bool completed = fNetDetachDone && !fNetDetachInFlight &&
                           !fNetDetachCallFreeFailed && !fNetDetachCall;
    IOLockUnlock(fNetAttachLock);
    if (!completed) {
        LOG("stop: synchronous detach worker did not complete and self-free within %u ms",
            (unsigned)waitMs);
        return false;
    }
    return true;
}

void Cx3Mlx4::netDetachThread(thread_call_param_t param0,
                              thread_call_param_t param1)
{
    Cx3Mlx4 *self = OSDynamicCast(Cx3Mlx4, (OSObject *)param0);
    IONetworkInterface *ifaces[2] = { 0, 0 };
    UInt32 refs[2] = { 0, 0 };
    UInt32 count, i, j;
    thread_call_t call;
    bool callFreed;

    (void)param1;
    if (!self) return;
    IOLockLock(self->fNetAttachLock);
    count = self->fNetDetachIfaceCount;
    call = self->fNetDetachCall;
    for (i = 0; i < count && i < 2u; ++i) {
        ifaces[i] = self->fNetDetachIfaces[i];
        refs[i] = self->fNetDetachRefCounts[i];
    }
    IOLockUnlock(self->fNetAttachLock);

    for (i = 0; i < count && i < 2u; ++i) {
        if (!ifaces[i]) continue;
        LOG("stop worker: synchronous detach %u/%u (owned references=%u)",
            (unsigned)(i + 1u), (unsigned)count, (unsigned)refs[i]);
        self->detachInterface(ifaces[i], /*sync=*/true);
        for (j = 0; j < refs[i]; ++j) ifaces[i]->release();
    }

    /* Apple documents freeing a thread_call from its own callback as safe. */
    callFreed = call && thread_call_free(call);
    IOLockLock(self->fNetAttachLock);
    if (callFreed) self->fNetDetachCall = 0;
    else self->fNetDetachCallFreeFailed = true;
    self->fNetDetachIfaceCount = 0;
    memset(self->fNetDetachIfaces, 0, sizeof(self->fNetDetachIfaces));
    memset(self->fNetDetachRefCounts, 0, sizeof(self->fNetDetachRefCounts));
    self->fNetDetachInFlight = false;
    self->fNetDetachDone = true;
    IOLockWakeup(self->fNetAttachLock, &self->fNetDetachDone, false);
    IOLockUnlock(self->fNetAttachLock);
    self->release();
}

void Cx3Mlx4::netAttachThread(thread_call_param_t param0,
                              thread_call_param_t param1)
{
    Cx3Mlx4 *self = OSDynamicCast(Cx3Mlx4, (OSObject *)param0);
    IONetworkInterface *netif = 0;
    bool attached = false;
    bool late;

    (void)param1;
    if (!self) return;

    self->netAttachWorkerMark(kCx3AttachWorkerRunning);
    /*
     * Serialize with the controller's work-loop gate. Per IOWorkLoop.h runAction
     * executes on this caller with a recursive gate held; it does NOT move attach
     * to the work-loop thread. 0.44.40's thread-handoff hypothesis was incorrect.
     * This remains a gate-context experiment, not a demonstrated fix.
     */
    IOWorkLoop *loop = self->getWorkLoop();
    if (loop) {
        Cx3AttachActionCtx ctx;
        IOReturn actRc;

        ctx.netif = 0;
        ctx.attached = false;
        ctx.ran = false;
        LOG("netAttach worker: calling attachInterface under the work-loop gate");
        actRc = loop->runAction(&Cx3Mlx4::netAttachOnWorkLoopAction, self, &ctx);
        if (actRc == kIOReturnSuccess && ctx.ran) {
            attached = ctx.attached;
            netif = ctx.netif;
            LOG("netAttach worker: the work loop returned attachInterface (attached=%s)",
                attached ? "true" : "false");
        } else {
            LOG("netAttach worker: the work loop did not run the attach action (rc 0x%x, ran=%s); "
                "leaving the interface unattached rather than calling attachInterface from this "
                "thread", (unsigned)actRc, ctx.ran ? "true" : "false");
            attached = false;
            netif = 0;
        }
    } else {
        /*
         * No work loop means the family was never given one, which start() would have
         * failed on.  Fall back to the old behaviour rather than silently doing nothing,
         * and say so: a run that reaches this line is not testing the work-loop path.
         */
        LOG("netAttach worker: NO WORK LOOP, falling back to attachInterface on this thread "
            "(this run does not test the work-loop hypothesis)");
        self->netAttachWorkerMark(kCx3AttachWorkerRunning);
        attached = self->attachInterface(&netif, /*doRegister=*/false);
        LOG("netAttach worker: attachInterface (fallback path) returned attached=%s interface=%s",
            attached ? "true" : "false", netif ? "non-null" : "null");
    }
    IOLockLock(self->fNetAttachLock);
    late = self->fNetAttachPoisoned;
    IOLockUnlock(self->fNetAttachLock);
    self->netAttachWorkerMark(late ? kCx3AttachWorkerLateReturn
                                   : (attached ? kCx3AttachWorkerReturned
                                               : kCx3AttachWorkerFailed));
    IOLockLock(self->fNetAttachLock);
    bool release_self = self->fNetAttachRetainCount != 0;
    self->fNetAttachSuccess = attached;
    self->fNetAttachIface = netif;
    /* Completion fence: after this final unlock, no callback accesses attach
     * locks, interfaces, properties or workloop again. Only its self reference
     * is released. stop() observes this flag under the same lock. */
    self->fNetAttachInFlight = false;
    self->fNetAttachDone = true;
    if (self->fNetAttachRetainCount) self->fNetAttachRetainCount--;
    IOLockWakeup(self->fNetAttachLock, &self->fNetAttachDone, false);
    IOLockUnlock(self->fNetAttachLock);
    if (release_self) self->release();
}

IOReturn Cx3Mlx4::netAttachOnKernelThread(bool *attached,
                                          IONetworkInterface **interfaceOut)
{
    AbsoluteTime deadline;
    wait_result_t waitResult;

    if (!attached || !interfaceOut || !fNetAttachLock || !fNetAttachCall)
        return kIOReturnNotReady;

    IOLockLock(fNetAttachLock);
    if (fNetAttachPoisoned) {
        IOLockUnlock(fNetAttachLock);
        return kIOReturnNotPermitted;
    }
    if (fNetAttachInFlight) {
        IOLockUnlock(fNetAttachLock);
        return kIOReturnBusy;
    }
    fNetAttachInFlight = true;
    fNetAttachDone = false;
    fNetAttachSuccess = false;
    fNetAttachIface = 0;
    retain();
    fNetAttachRetainCount++;
    netAttachWorkerMark(kCx3AttachWorkerQueued);
    IOLockUnlock(fNetAttachLock);

    if (thread_call_enter(fNetAttachCall)) {
        release();
        IOLockLock(fNetAttachLock);
        if (fNetAttachRetainCount) fNetAttachRetainCount--;
        /*
         * A pending call with no in-flight bit is an inconsistent state.  Do not
         * launch a second worker; poison this controller until stop() can cancel
         * and free the original call.
         */
        fNetAttachPoisoned = true;
        netAttachWorkerMark(kCx3AttachWorkerPoisoned);
        IOLockUnlock(fNetAttachLock);
        return kIOReturnBusy;
    }

    clock_interval_to_deadline(10ULL * 1000ULL * 1000ULL * 1000ULL,
                               kNanosecondScale, &deadline);
    IOLockLock(fNetAttachLock);
    while (!fNetAttachDone) {
        waitResult = IOLockSleepDeadline(fNetAttachLock, &fNetAttachDone,
                                         deadline, THREAD_INTERRUPTIBLE);
        if (waitResult != THREAD_AWAKENED)
            break;
    }
    *attached = fNetAttachSuccess;
    *interfaceOut = fNetAttachIface;
    bool timedOut = !fNetAttachDone;
    if (timedOut) {
        fNetAttachPoisoned = true;
        /*
         * Publish the timeout while holding the same lock the worker uses
         * before it publishes late_return. This keeps the two transitions
         * ordered and prevents a late worker from being overwritten by a
         * delayed timeout mark.
         */
        netAttachWorkerMark(kCx3AttachWorkerTimedOut);
    }
    IOLockUnlock(fNetAttachLock);

    if (timedOut) {
        *attached = false;
        *interfaceOut = 0;
        LOG("netup: deferred attachInterface did not return within 10 s; returning timeout "
            "to the selector while the kernel worker remains contained; this controller is "
            "poisoned against another attach until stop()");
    }
    return timedOut ? kIOReturnTimeout : kIOReturnSuccess;
}

IOReturn Cx3Mlx4::exportTxProbe(const UInt64 *in, UInt64 *out)
{
    UInt32 postWaitMs = (UInt32)in[kTxProbeInNopWaitMs];
    UInt32 armWaitMs  = (UInt32)in[kTxProbeInArmWaitMs];
    UInt32 i, opcode, status, wqeOk, comps, tried, winner;
    UInt64 refused = CX3_TXPROBE_RAN;

    memset(out, 0, sizeof(UInt64) * (UInt64)kTxProbeOutCount);
    /* Every field starts as "never reached": docs/DEFECTS.md D10 -- a zero here would read as
     * a measurement of zero, and "the hardware said no" and "this driver never asked" must not
     * look the same. */
    for (i = 0; i < (UInt32)kTxProbeOutCount; ++i) out[i] = CX3_TXPROBE_NOT_RUN;
    out[kTxProbeOutStatus] = (UInt64)kIOReturnError;

    if (postWaitMs == 0u) postWaitMs = CX3_TXPROBE_DEFAULT_POST_WAIT_MS;
    if (armWaitMs  == 0u) armWaitMs  = CX3_TXPROBE_DEFAULT_ARM_WAIT_MS;
    if (postWaitMs > 5000u)  postWaitMs = 5000u;
    if (armWaitMs  > 10000u) armWaitMs  = 10000u;

    /*
     * The refusal gate.  r4tx has to have built the send side this boot: this selector posts
     * into that queue and rings the QPN it owns, and a missing prerequisite is a statement
     * about which stage has run, never a verdict about the hardware.
     */
    /*
     * ...and the same exclusion r4raw states: while the network data path is enabled it owns
     * this send queue (it posts from outputPacket and reclaims from the timer), so a probe that
     * posted into it would move the producer under the data path's feet and take a completion
     * the data path is waiting for. Refused rather than interleaved.
     */
    if (fNetEnabled)                       refused = CX3_TXPROBE_IFACE_UP;
    else if (!fPortQueues[0].txCqRing || !fPortQueues[0].txCqNent) refused = CX3_TXPROBE_NO_TX_CQ;
    else if (!fPortQueues[0].txSq || fPortQueues[0].txSqNent < 2u) refused = CX3_TXPROBE_NO_TX_SQ;
    else if (fPortQueues[0].txQpn == 0u)               refused = CX3_TXPROBE_NO_TX_QP;
    if (refused != CX3_TXPROBE_RAN) {
        LOG("txprobe refused: the transmit side is not built in this boot (code %u). Run "
            "'cx3ctl r4tx' first -- this selector posts into the queue r4tx creates and rings "
            "the QPN r4tx puts in RTS.", (unsigned)refused);
        out[kTxProbeOutRefused] = refused;
        out[kTxProbeOutStatus]  = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }
    out[kTxProbeOutRefused] = 0u;

    /* --- 1. NOP versus SEND: is this send queue executed at all? --- */
    opcode = CX3_TXPROBE_OPCODE_NONE; status = 0u; wqeOk = 0u;
    /*
     * The NOP is a CONTROL-SEGMENT-ONLY descriptor: no data segment, so its fence_size is 1
     * (TX_WQE_FENCE_SIZE(0)) and nothing is written behind the ctrl segment.  A "NOP" that
     * carries a data segment is not a NOP descriptor, and its silence would say nothing about
     * the hardware -- which is what the first version of this experiment did.
     */
    comps = r4TxProbePost(MLX4_OPCODE_NOP, 0u, 0u, 0u, postWaitMs, &opcode, &status, &wqeOk);
    out[kTxProbeOutNopPosts]  = 1u;
    out[kTxProbeOutNopWqeOk]  = wqeOk;
    out[kTxProbeOutNopComps]  = comps;
    out[kTxProbeOutNopOpcode] = opcode;
    out[kTxProbeOutNopStatus] = status;
    LOG("txprobe 1/3 NOP: posted one 32-byte descriptor with opcode 0x%02x (MLX4_OPCODE_NOP, "
        "device.h:365 -- no data, but a completion) into the send queue %u TXBBs long, rung "
        "the send doorbell at UAR %u + 0x%x, and waited %u ms. Completions: %u. Readback "
        "matched: %u.",
        (unsigned)MLX4_OPCODE_NOP, (unsigned)fPortQueues[0].txSqNent, (unsigned)fPortQueues[0].cqUarStart,
        (unsigned)MLX4_SEND_DOORBELL, (unsigned)postWaitMs, (unsigned)comps, (unsigned)wqeOk);

    /* --- 2. the doorbell record, four variants, all with opcode SEND --- */
    tried = 0u; winner = CX3_TXPROBE_NOT_RUN;
    for (i = 0u; i < 4u; ++i) {
        UInt32 w0 = ((i == 1u) || (i == 3u)) ? 1u : 0u;   /* the record's first dword */
        UInt32 w1 = ((i == 2u) || (i == 3u)) ? 1u : 0u;   /* and its second */

        opcode = CX3_TXPROBE_OPCODE_NONE; status = 0u; wqeOk = 0u;
        comps = r4TxProbePost(MLX4_OPCODE_SEND, 1u, w0, w1, postWaitMs,
                              &opcode, &status, &wqeOk);
        ++tried;
        if (i == 0u) {
            /* Variant 0 is the plain SEND post this stage has always made, kept as its own
             * number so the two halves of the experiment are both readable. */
            out[kTxProbeOutSendComps]  = comps;
            out[kTxProbeOutSendOpcode] = opcode;
        }
        LOG("txprobe 2/3 record variant %u: doorbell record = [%u, %u], opcode 0x%02x -- "
            "completions: %u (first CQE opcode 0x%02x, status 0x%04x), readback matched: %u. "
            "The public PRM's posting procedure updates this record between the WQE and the "
            "doorbell (section 7.4.3.1) and says the receive and send records are consecutive "
            "(section 7.4.2); no implementation this project checked writes the send half "
            "(docs/M2B-NOTES.md 51.4), which is why all four readings are tried rather than "
            "one.", (unsigned)i, (unsigned)w0, (unsigned)w1, (unsigned)MLX4_OPCODE_SEND,
            (unsigned)comps, (unsigned)opcode, (unsigned)status, (unsigned)wqeOk);
        if (comps > 0u) { winner = i; break; }
    }
    out[kTxProbeOutRecVariants] = tried;
    /* Variant 0 is a variant: it is the plain post with no record written.  Both it and "no
     * variant worked" used to be 0, and the report said "none of them changed the outcome"
     * about a run in which the first one had. */
    out[kTxProbeOutRecWinner]   = (winner == CX3_TXPROBE_NOT_RUN) ? 0u : (winner + 1u);

    /* --- 3. the arm doorbell on the page the QPs were built with --- */
    {
        UInt8 eqe[MLX4_EQ_ENTRY_SIZE];
        UInt32 cons = fPortQueues[0].rawRxCons;
        UInt32 drained = 0u, eqeType = CX3_TXPROBE_OPCODE_NONE, eqeCqn = 0u;
        bool found = false, ran = false;
        UInt64 t0;

        if (fPortQueues[0].cqRec && fPortQueues[0].cqCqn != 0u && fEqRing && fEqNent) {
            ran = true;
            out[kTxProbeOutArmRan] = 1u;
            /*
             * Anything already in the EQ ring is consumed first, so the EQE this arm produces
             * cannot be confused with one that was already there -- the same mistake the
             * m3eqpoll code records having made with a type-0 EQE.
             */
            for (;;) {
                memset(eqe, 0, sizeof(eqe));
                if (!fHw.eqNextEqe(fEqRing, fEqNent, fEqeFactor, fEqConsIndex, eqe)) break;
                fEqConsIndex = fEqConsIndex + 1u;
                ++drained;
                if (drained >= 8u) break;
            }
            if (drained) {
                LOG("txprobe: %u EQE(s) were already in the event queue and were consumed "
                    "first, so what follows is attributable to this arm", (unsigned)drained);
                fHw.eqSetConsumerIndex(fEqDoorbell, fEqConsIndex, true);
            }
            /*
             * mlx4_cq_arm, both halves and in that order (cq.h:138-161): the arm word into the
             * record's second dword, a barrier, then the 8-byte MMIO write carrying
             * [sn | cmd | cqn, ci] to UAR + MLX4_CQ_DOORBELL.
             */
            fHw.cqSetArmDb((volatile UInt32 *)fPortQueues[0].cqRec->virt, /*armSn=*/0u,
                           MLX4_CQ_DB_REQ_NOT, cons);
            fHw.cqRingDoorbell(fPortQueues[0].cqCqn, cons, /*armSn=*/0u, /*arm=*/true, fPortQueues[0].cqUarStart);
            LOG("txprobe 3/3 arm: the receive CQ %u was armed at consumer index %u through UAR "
                "page %u (the same page the QPs' usr_page names) and the event queue is polled "
                "for up to %u ms. A COMP EQE (type 0x%02x) naming CQN %u is what says the MMIO "
                "doorbell reached the device; a completion has to land on an armed queue first, "
                "and this segment delivers one every few tens of milliseconds.",
                (unsigned)fPortQueues[0].cqCqn, (unsigned)cons, (unsigned)fPortQueues[0].cqUarStart,
                (unsigned)armWaitMs, (unsigned)MLX4_EVENT_TYPE_COMP, (unsigned)fPortQueues[0].cqCqn);

            t0 = r4RawNowMs();
            for (;;) {
                memset(eqe, 0, sizeof(eqe));
                if (fHw.eqNextEqe(fEqRing, fEqNent, fEqeFactor, fEqConsIndex, eqe)) {
                    eqeType = eqe[MLX4_EQE_TYPE_OFFSET];
                    eqeCqn  = cx3GetBe32(eqe + MLX4_EQE_COMP_CQN_OFFSET);
                    found   = true;
                    fEqConsIndex = fEqConsIndex + 1u;
                    fHw.eqSetConsumerIndex(fEqDoorbell, fEqConsIndex, true);
                    break;
                }
                if ((r4RawNowMs() - t0) >= (UInt64)armWaitMs) break;
                IOSleep(1);
            }
        } else {
            LOG("txprobe 3/3 arm: the receive CQ's record or the event queue is not there, so "
                "the arm experiment is SKIPPED. That is a statement about which stages have "
                "run, not about the hardware (r4cq and m3equp build them).");
        }

        if (ran) {
            out[kTxProbeOutArmEqeFound] = found ? 1u : 0u;
            /* What arrived, even when it is not what was asked for: an EQE of another type
             * naming another queue is a different (and informative) answer. */
            out[kTxProbeOutArmEqeType] = found ? eqeType : CX3_TXPROBE_OPCODE_NONE;
            out[kTxProbeOutArmEqeCqn]  = found ? eqeCqn : 0u;
            out[kTxProbeOutArmEqeOk] =
                (found && eqeType == MLX4_EVENT_TYPE_COMP && eqeCqn == fPortQueues[0].cqCqn) ? 1u : 0u;
            if (found) {
                LOG("txprobe 3/3 arm: an EQE arrived -- type 0x%02x, CQN %u. type 0x%02x with "
                    "CQN %u is the arm's own event (a CQ event, device.h:293); anything else is "
                    "reported rather than discarded, because an event naming another queue is a "
                    "different answer and not a silent one.",
                    (unsigned)eqeType, (unsigned)eqeCqn, (unsigned)MLX4_EVENT_TYPE_COMP,
                    (unsigned)fPortQueues[0].cqCqn);
            } else {
                LOG("txprobe 3/3 arm: NO EQE in %u ms. The arm was written to the record and "
                    "the doorbell was rung at BAR2+0x%llx with [0x%08x, %u]; if nothing arrives "
                    "even though completions kept landing on that queue, the arm doorbell did "
                    "not reach the device -- which is the independent confirmation of "
                    "docs/M2B-NOTES.md section 51.3 that section 47.4.4 asked for.",
                    (unsigned)armWaitMs,
                    (unsigned long long)(mlx4UarPageOffset(fPortQueues[0].cqUarStart) + MLX4_CQ_DOORBELL),
                    (unsigned)((0u << 28) | MLX4_CQ_DB_REQ_NOT | (fPortQueues[0].cqCqn & 0xffffffu)),
                    (unsigned)cons);
            }
        }
    }

    /*
     * The doorbell record goes back to zero, and that is ALL this puts back.  It deliberately
     * does not touch the stamps: the descriptors this selector posted are in the ring now and
     * the hardware's consumer index has moved past them, so blanket-stamping every slot as
     * "software owns" would be a claim about a lap the ring is not in (the first version did
     * stamp them all, and it was wrong for the same reason its posts carried a fixed owner
     * bit -- see docs/M2B-NOTES.md 55).
     */
    fHw.sqSetDoorbellRecord(fPortQueues[0].txQpRec ? (volatile UInt32 *)fPortQueues[0].txQpRec->virt : 0, 0u, 0u);

    LOG("txprobe done: NOP %u completion(s), the four record variants tried %u (winner %u), "
        "the arm %s. %s",
        (unsigned)out[kTxProbeOutNopComps], (unsigned)out[kTxProbeOutRecVariants],
        (unsigned)out[kTxProbeOutRecWinner],
        (out[kTxProbeOutArmEqeOk] == 1u) ? "produced its own CQ event (the doorbell reaches the "
                                           "device)" : "produced no CQ event of its own",
        (out[kTxProbeOutNopComps] != 0u) ?
            "A NOP completed, so the hardware DOES execute this send queue: the defect is "
            "specific to the SEND data path and the descriptor's other fields are the place to "
            "look next." :
            "Not even a NOP completed, so the hardware is not executing descriptors from this "
            "send queue at all -- no field of the descriptor can be the cause.");

    out[kTxProbeOutStatus] = 0u;
    return kIOReturnSuccess;
}

IOReturn Cx3Mlx4::exportR4Raw(const UInt64 *in, Cx3R4RawResult *res, UInt64 *out)
{
    UInt32 postsWanted = (UInt32)in[kR4RawInTxPosts];
    UInt32 rxBudgetMs  = (UInt32)in[kR4RawInRxBudgetMs];
    UInt32 i;

    memset(out, 0, sizeof(UInt64) * kR4RawOutCount);
    memset(res, 0, sizeof(*res));
    res->status = (UInt32)kIOReturnError;
    /* Every measured scalar starts as "never reached" -- see r4RawPublishScalars and
     * docs/DEFECTS.md D10 for why a zeroed field cannot be allowed to satisfy one. */
    for (i = 0; i < (UInt32)kR4RawOutCount; ++i) out[i] = CX3_R4RAW_NOT_MEASURED;
    out[kR4RawOutStatus] = res->status;
    res->txWqeOk         = CX3_R4RAW_NOT_MEASURED;
    res->txCqeOk         = CX3_R4RAW_NOT_MEASURED;
    res->txWrapOk        = CX3_R4RAW_NOT_MEASURED;
    res->rxQpnOk         = CX3_R4RAW_NOT_MEASURED;
    res->rxRecycleOk     = CX3_R4RAW_NOT_MEASURED;
    res->rxMinByteCnt    = CX3_R4RAW_NOT_MEASURED;

    if (rxBudgetMs == 0u) rxBudgetMs = CX3_R4RAW_DEFAULT_RX_BUDGET_MS;
    res->rxBudgetMs = rxBudgetMs;
    res->calls      = fPortQueues[0].rawCalls + 1u;

    /* ------------------------------------------------------------------ */
    /* 1. the receive side has to exist at all                             */
    /* ------------------------------------------------------------------ */
    /*
     * Refused while the network data path is running, and this is the rule that makes the data
     * path safe without taking the transaction lock (see outputPacket):
     *
     * r4raw and the kernel data path consume the SAME receive CQ and post into the SAME send
     * queue.  One CQ has one consumer -- stage 9 proved that by comparing the slot a completion
     * names against the ring position, a comparison two consumers guarantee will fail -- so the
     * two can never both be live.  The data path's timer runs on the workloop and cannot wait
     * for this selector, and this selector cannot see the timer's state, so the exclusion has to
     * be by refusal rather than by a lock.
     */
    if (fNetEnabled) {
        LOG("r4raw refused: the network data path is enabled (en%u is up), and it consumes this "
            "receive CQ and posts into this send queue. Two consumers on one CQ is the state "
            "stage 9's slot-versus-position check exists to catch, so this selector will not run "
            "while the interface is up. Bring it down first (ifconfig en%u down), or reboot",
            (unsigned)fNetUnit, (unsigned)fNetUnit);
        res->status = (UInt32)kIOReturnExclusiveAccess;
        out[kR4RawOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if (!fPortQueues[0].cqUp || !fPortQueues[0].qpUp || !fPortQueues[0].rqUp || !fPortQueues[0].rqRing || !fPortQueues[0].rqBufs || !fPortQueues[0].qpRec) {
        LOG("r4raw refused: the receive side is not built in this boot -- completion queue %s, "
            "RX QP %s, receive ring %s, its buffers %s, producer record %s -- so there is not "
            "one completion to read and no ring to refill. r4cq, r4rq and r4qp build those and "
            "the chain runs them before this.",
            fPortQueues[0].cqUp ? "up" : "missing", fPortQueues[0].qpUp ? "up" : "missing",
            fPortQueues[0].rqRing ? "there" : "missing", fPortQueues[0].rqBufs ? "there" : "missing",
            fPortQueues[0].qpRec ? "there" : "missing");
        res->status = (UInt32)kIOReturnNotReady;
        out[kR4RawOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    if (fPortQueues[0].rqNent > CX3_R4RAW_MAX_RX_SLOTS) {
        LOG("r4raw refused: the receive ring has %u WQEs and this stage tracks %u per slot "
            "(how often each was refilled, and whether it is in flight), so the numbers it "
            "reports would be about slots it is not following. Build the ring with %u entries "
            "or fewer (the chain uses 256), or raise CX3_R4RAW_MAX_RX_SLOTS.",
            (unsigned)fPortQueues[0].rqNent, (unsigned)CX3_R4RAW_MAX_RX_SLOTS,
            (unsigned)CX3_R4RAW_MAX_RX_SLOTS);
        res->status = (UInt32)kIOReturnUnsupported;
        out[kR4RawOutStatus] = res->status;
        return kIOReturnSuccess;
    }
    res->rxQpn   = fPortQueues[0].qpQpn;
    res->rxNent  = fPortQueues[0].rqNent;
    res->rxStride = RX_WQE_SIZE;

    /* ------------------------------------------------------------------ */
    /* 2. the memory region, as the firmware holds it                      */
    /* ------------------------------------------------------------------ */
    {
        UInt8  entry[MPT_ENTRY_SIZE];
        UInt32 st = CX3_FWSTATUS_NOT_POSTED;

        memset(entry, 0, sizeof(entry));
        res->mrMptIndex = fMrMptIndex;
        if (fHw.queryMpt(fMrMptIndex, entry, sizeof(entry), &st)) {
            res->mrQueryOk = 1u;
            res->mrFwStatus = st;
            res->mrStart  = cx3GetBe64(entry + MPT_ENTRY_START_OFFSET);
            res->mrLength = cx3GetBe64(entry + MPT_ENTRY_LENGTH_OFFSET);
            LOG("r4raw: the memory region every WQE's lkey belongs to, as QUERY_MPT reports it: "
                "MPT %u, lkey 0x%08x, start 0x%llx, length 0x%llx. The reference's own region "
                "covers the whole address space (iova 0, size ~0, mlx4_en_main.c:213) so that any "
                "DMA buffer is inside it; whether THIS one does is what decides whether a "
                "descriptor's address is legal",
                (unsigned)fMrMptIndex, (unsigned)fMrLkey, (unsigned long long)res->mrStart,
                (unsigned long long)res->mrLength);
        } else {
            res->mrFwStatus = st ? st : 0xffffffffu;
            LOG("r4raw: QUERY_MPT %u did not answer (fw status 0x%x), so the region's start and "
                "length are unknown and not reported; the addresses below are still what the "
                "WQEs carry", (unsigned)fMrMptIndex, (unsigned)res->mrFwStatus);
        }
    }

    /* ------------------------------------------------------------------ */
    /* 3. the frame, and the send side's availability                      */
    /* ------------------------------------------------------------------ */
    res->txRefused = CX3_R4RAW_TX_RAN;
    if (!fPortQueues[0].txQpUp || !fPortQueues[0].txCqUp || !fPortQueues[0].txSq || !fPortQueues[0].txCqRing) {
        res->txRefused = CX3_R4RAW_TX_NO_STAGE6;
        LOG("r4raw: the transmit half does not run -- the send side is not built in this boot "
            "(send QP %s, TX CQ %s, its ring %s, send queue %s). r4tx builds those. The receive "
            "half runs anyway: the two halves fail for different reasons and neither may hide "
            "the other.",
            fPortQueues[0].txQpUp ? "up" : "missing", fPortQueues[0].txCqUp ? "up" : "missing",
            fPortQueues[0].txCqRing ? "there" : "missing", fPortQueues[0].txSq ? "there" : "missing");
    } else if (!fPortQueues[0].macUp) {
        res->txRefused = CX3_R4RAW_TX_NO_MAC;
        LOG("r4raw: the transmit half does not run -- no MAC address is registered (stage 5), so "
            "the frame would have to invent a source address. A frame whose source is fabricated "
            "is a worse test frame than none, so the receive half proceeds alone.");
    } else if (!r4RawFrameEnsure(res)) {
        res->txRefused = CX3_R4RAW_TX_NO_FRAME;
    }
    res->frameInRegion = 0;
    if (res->mrQueryOk && res->txFramePhys) {
        UInt64 end = res->mrStart + res->mrLength;   /* ~0 wraps to 0, hence the second test */
        res->frameInRegion = (res->txFramePhys >= res->mrStart &&
                              (res->mrLength == ~0ull ||
                               res->txFramePhys + CX3_R4RAW_FRAME_BYTES <= end)) ? 1u : 0u;
        LOG("r4raw: the frame buffer at 0x%llx is %s the region [0x%llx, 0x%llx) this QP's "
            "lkey resolves. If the device checks that (the one thing this stage cannot know "
            "before it sends), a descriptor outside it completes in ERROR -- which is a readable "
            "answer, in the TX CQE's opcode, not a silence",
            (unsigned long long)res->txFramePhys, res->frameInRegion ? "INSIDE" : "OUTSIDE",
            (unsigned long long)res->mrStart,
            (unsigned long long)(res->mrStart + res->mrLength));
    }

    /* ------------------------------------------------------------------ */
    /* 4. the receive side's own bookkeeping, seeded from the device        */
    /* ------------------------------------------------------------------ */
    if (fPortQueues[0].rawRxProd == 0u) {
        /*
         * First call in this boot: read the producer record back rather than assume stage 4's
         * value.  It is the number the hardware itself uses ("how many WQEs have been posted"),
         * and with n WQEs posted and none refilled, every slot is in flight.
         */
        UInt32 prodNow = OSSwapBigToHostInt32(((volatile UInt32 *)fPortQueues[0].qpRec->virt)[0]);
        if (prodNow == 0u) {
            LOG("r4raw refused: the receive producer record reads 0, so the hardware was never "
                "told that any WQE exists -- frames could not have arrived, and refilling on top "
                "of an unpublished ring would mean publishing a count this driver made up. "
                "Stage 4 publishes it (r4qp); run that first.");
            res->status = (UInt32)kIOReturnNotReady;
            out[kR4RawOutStatus] = res->status;
            return kIOReturnSuccess;
        }
        fPortQueues[0].rawRxProd = prodNow;
        for (i = 0; i < fPortQueues[0].rqNent; ++i) r4RawRxSetInFlight(i, true);
        LOG("r4raw: first call for this receive ring: the producer record already reads %u (the "
            "WQEs stage 3 filled and stage 4 published), so every one of the %u slots is in "
            "flight until a completion says otherwise. The completions already sitting in the "
            "receive CQ are from those frames.",
            (unsigned)prodNow, (unsigned)fPortQueues[0].rqNent);
    }
    res->rxProducerBefore = fPortQueues[0].rawRxProd;

    /* ------------------------------------------------------------------ */
    /* 5. the receive drain, then the frames, then the receive wait          */
    /* ------------------------------------------------------------------ */
    r4RawRxPoll(res, 0u, /*wait=*/false);
    LOG("r4raw: the receive drain consumed %u completion(s); consumer index %u, producer %u",
        (unsigned)res->rxFrames, (unsigned)fPortQueues[0].rawRxCons, (unsigned)fPortQueues[0].rawRxProd);

    if (res->txRefused == CX3_R4RAW_TX_RAN) r4RawTransmit(res, postsWanted);

    {
        UInt32 before = res->rxFrames;
        r4RawRxPoll(res, rxBudgetMs, /*wait=*/true);
        LOG("r4raw: the receive wait collected %u more completion(s) in up to %u ms; %u in "
            "total this call, %u of them naming our own RX QPN %u",
            (unsigned)(res->rxFrames - before), (unsigned)rxBudgetMs, (unsigned)res->rxFrames,
            (unsigned)res->rxQpnMatches, (unsigned)res->rxQpn);
    }

    /* ------------------------------------------------------------------ */
    /* 6. the judgements that are about the totals rather than one event     */
    /* ------------------------------------------------------------------ */
    res->rxProducerAfter = fPortQueues[0].rawRxProd;
    res->rxLaps          = fPortQueues[0].rawRxRecyclesTotal / fPortQueues[0].rqNent;
    {
        UInt32 inFlight = 0;
        for (i = 0; i < fPortQueues[0].rqNent; ++i) if (r4RawRxInFlight(i)) ++inFlight;
        res->rxInFlightAtExit = inFlight;
    }
    /*
     * Criterion 4's verdict, and the "not measured" state that keeps it honest: with no
     * completions at all, "every completion named our QPN" is vacuously true and must not be
     * reported as a pass (docs/DEFECTS.md D10's shape).  The same applies to criterion 6's
     * verdict: with nothing refilled, "every refill was correct" says nothing.
     */
    if (res->rxFrames > 0u) res->rxQpnOk = (res->rxQpnMatches == res->rxFrames) ? 1u : 0u;
    /*
     * ...and the refill's verdict is measured when there was a refill OR when a violation was
     * seen without one -- the case where the very first completion is already wrong: judging
     * "no refills happened, so nothing to say" there would report a run whose refill was
     * refused as one where the question never came up.
     */
    if (res->rxRecycles > 0u || res->rxRecycleViolations > 0u)
        res->rxRecycleOk = (res->rxRecycleViolations == 0u) ? 1u : 0u;

    fPortQueues[0].rawCalls = res->calls;
    res->status = 0;
    out[kR4RawOutStatus] = 0;
    r4RawPublishScalars(res, out);

    /*
     * The summary, and the sentence the plan asks to be written down: when not one frame
     * arrived, the first thing to suspect is stage 7's mcast/base_qpn, not stage 8's filter.
     */
    LOG("r4raw OK: RX QPN %u, %u frame(s) received and %u refilled (%u whole laps of %u slots), "
        "%u slot(s) refilled at least twice, %u of the frames named our QPN, QPN verdict %s, "
        "content %s; TX: %u post(s), %u completion(s)%s, owner laps %u/%u, wrap %s, WQE readback "
        "%s%s",
        (unsigned)res->rxQpn, (unsigned)res->rxFrames, (unsigned)res->rxRecycles,
        (unsigned)res->rxLaps, (unsigned)fPortQueues[0].rqNent, (unsigned)res->rxSlotsRecycledTwice,
        (unsigned)res->rxQpnMatches,
        (res->rxQpnOk == 1u) ? "all of them" : ((res->rxQpnOk == 0u) ? "MISMATCH"
                                                                     : "not measured"),
        res->rxContentOk ? "a frame of at least the minimum was read out"
                         : "nothing readable yet",
        (unsigned)res->txPosts, (unsigned)res->txCompletions,
        res->txRefused ? " (the transmit half was REFUSED -- see the log above)" : "",
        (unsigned)res->txOwnerLapsOk, (unsigned)res->txLapsCovered,
        (res->txWrapOk == 1u) ? "PROVED"
                              : ((res->txWrapOk == 0u) ? "NOT proved" : "not attempted"),
        (res->txWqeOk == 1u) ? "matched" : ((res->txWqeOk == 0u) ? "MISMATCHED" : "n/a"),
        res->rxOurs ? "; and one of the received frames was THIS DRIVER'S OWN" : "");
    if (res->rxFrames == 0u) {
        LOG("r4raw: NOT ONE FRAME ARRIVED AT RX QPN %u. The first thing to suspect is stage 7's "
            "mcast/base_qpn: under A0 the firmware decides where broadcast and multicast go, and "
            "it is the `mcast` field of SET_PORT qpn_calc that names the QP (docs/EXTERNAL-REFS.md "
            "section 13). This is NOT \"stage 8's filter is not configured\" -- A0 sends a "
            "broadcast to the QP that field names, and no filter command is involved. The other "
            "two things to check are that the producer record was published (stage 4) and that "
            "the link is up (m3portup).",
            (unsigned)res->rxQpn);
    }
    return kIOReturnSuccess;
}

/* Called synchronously with the family's gate held. No firmware commands and
 * no transaction-lock acquisition here: the selector already owns that lock. */
IOReturn Cx3Mlx4::autoNetworkOnWorkLoop(OSObject *owner, void *arg0,
                                      void *arg1, void *arg2, void *arg3)
{
    (void)arg1; (void)arg2; (void)arg3;
    Cx3Mlx4 *self = OSDynamicCast(Cx3Mlx4, owner);
    if (!self || !arg0) return kIOReturnBadArgument;
    bool on = *(bool *)arg0;
    self->fAutoNetworkUserDisabled = !on;
    if (!self->fNetIface) return on ? kIOReturnNotReady : kIOReturnSuccess;
    return on ? self->enable(self->fNetIface) : self->disable(self->fNetIface);
}

IOReturn Cx3Mlx4::exportAutoNetwork(const Cx3AutoNetworkRequest *req,
                                   Cx3AutoNetworkResult *res)
{
    if (!req || !res) return kIOReturnBadArgument;
    memset(res, 0, sizeof(*res));
    if (req->version != CX3_AUTO_NETWORK_VERSION || req->structBytes != sizeof(*req) ||
        req->action > kCx3AutoDisable || req->port != 1 ||
        req->expectedBuildTag[64] || !req->expectedBuildTag[0]) return kIOReturnBadArgument;
    for (UInt32 i = 0; i < sizeof(req->reserved); ++i)
        if (req->reserved[i]) return kIOReturnBadArgument;
    /* Check the binding INSIDE the chain's transaction. Never trust only the
     * preceding userspace sample; a different selector may have run since it. */
    if (!fPci || req->expectedServiceId != getRegistryEntryID() ||
        req->expectedProviderId != fPci->getRegistryEntryID() ||
        req->expectedGeneration != fDeviceGeneration ||
        strcmp(req->expectedBuildTag, CX3_BUILD_TAG)) return kIOReturnNotPermitted;

    bool txFault = false;
    if (fNetTxLock) { IOLockLock(fNetTxLock); txFault = fNetTxFault.latched;
                     IOLockUnlock(fNetTxLock); }
    const bool blocked = fResetCommandsBlocked || fStopRetained || fQuarExhausted ||
                         fHw.cmdWedged() || selectorStopping();
    const auto snapshot = [&]() {
        *res = fAutoNetwork;
        res->action = req->action;
        res->deviceGeneration = fDeviceGeneration;
        res->interfaceUnit = fNetIface ? fNetIface->getUnitNumber() : 0xffffffffu;
        if (fNetTxLock) { IOLockLock(fNetTxLock); txFault = fNetTxFault.latched;
                         IOLockUnlock(fNetTxLock); }
        res->flags = (fNetEnabled ? CX3_AUTO_NET_ENABLED : 0u) |
            (fPorts[0].linkUp ? CX3_AUTO_NET_LINK_UP : 0u) |
            (fAutoNetworkUserDisabled ? CX3_AUTO_NET_USER_DISABLED : 0u) |
            (txFault ? CX3_AUTO_NET_TX_FAULT : 0u) |
            ((fResetCommandsBlocked || fStopRetained || fQuarExhausted || fHw.cmdWedged() ||
              selectorStopping()) ? CX3_AUTO_NET_LIFECYCLE_BLOCKED : 0u);
        if (((res->flags & (CX3_AUTO_NET_LIFECYCLE_BLOCKED | CX3_AUTO_NET_TX_FAULT)) || (fAutoNetwork.completedMask &&
             fAutoNetwork.deviceGeneration != fDeviceGeneration)) && !res->status)
            res->status = (UInt64)kIOReturnNotReady;
    };
    if (req->action == kCx3AutoStatus) { snapshot(); return kIOReturnSuccess; }
    if (fAutoNetwork.state == kCx3AutoFailed) { snapshot(); return kIOReturnSuccess; }
    if (blocked || txFault) {
        snapshot(); res->status = (UInt64)kIOReturnNotReady; return kIOReturnSuccess;
    }
    if (fAutoNetwork.completedMask && fAutoNetwork.deviceGeneration != fDeviceGeneration) {
        snapshot(); res->status = (UInt64)kIOReturnNotReady; return kIOReturnSuccess;
    }
    /* Administrative pause/resume uses the existing callbacks with the same
     * family gate as ifconfig. Never destroy/rebuild queues on an enable retry.
     * The BSD IFF_UP and DHCP configuration remain userspace responsibilities. */
    if (req->action == kCx3AutoDisable ||
        fAutoNetwork.completedMask == CX3_AUTO_NETWORK_ALL_STAGES) {
        bool on = req->action == kCx3AutoEnable;
        IOWorkLoop *loop = getWorkLoop();
        IOReturn rc = loop ? loop->runAction(&Cx3Mlx4::autoNetworkOnWorkLoop, this, &on)
                           : kIOReturnNotReady;
        fAutoNetwork.status = (UInt64)rc;
        fAutoNetwork.state = rc ? kCx3AutoFailed : (on ? kCx3AutoReady : kCx3AutoDisabled);
        snapshot(); return kIOReturnSuccess;
    }
    /* The first reset exception is for a completely empty provider only.
     * Never adopt manually initialized/partial resources or use auto as reset
     * recovery. No BAR access until all host-only admission checks pass. */
    if (fDeviceGeneration != 1 || fFwRunAttempted || fResetUnproven ||
        fNetIface || fNetTimer || fNetEnabled || hasStopRetentionOwners() ||
        !resetFreshStartEligible()) {
        snapshot(); res->status = (UInt64)kIOReturnNotReady; return kIOReturnSuccess;
    }
    union Scratch {
        Cx3DevCapResult cap; Cx3ProfileResult profile; Cx3QueryPortResult port;
        Cx3R4MrResult mr; Cx3R4CqResult cq; Cx3R4RqResult rq;
        Cx3R4QpResult qp; Cx3R4IngressResult ingress; Cx3R4TxResult tx;
    };
    /* QP capture is over 16 KiB; never allocate this on the kernel stack. */
    Scratch *w = (Scratch *)IOMalloc(sizeof(Scratch));
    if (!w) { snapshot(); res->status = (UInt64)kIOReturnNoMemory; return kIOReturnSuccess; }
    fAutoNetwork.state = kCx3AutoInitializing;
    fAutoNetwork.status = 0;
    UInt64 *s = fAutoNetwork.lastScalars;
    UInt32 fwPages = 0;
    const auto begin = [&](UInt32 stage, UInt32 selector) {
        fAutoNetwork.stage = stage;
        fAutoNetwork.lastSelector = selector;
        fAutoNetwork.lastStageStatus = (UInt64)kIOReturnNotReady;
        memset(s, 0xff, sizeof(fAutoNetwork.lastScalars));
        memset(w, 0, sizeof(*w));
        setProperty("cx3AutoNetworkStage", stage, 32);
        LOG("auto network: stage %u selector %u", (unsigned)stage, (unsigned)selector);
    };
    const auto finish = [&](IOReturn rc, bool semanticOk) -> bool {
        UInt64 status = rc ? (UInt64)rc : s[0];
        if (!status && !semanticOk) status = (UInt64)kIOReturnError;
        fAutoNetwork.lastStageStatus = status;
        fAutoNetwork.deviceGeneration = fDeviceGeneration;
        if (status) {
            fAutoNetwork.status = status;
            fAutoNetwork.failedStage = fAutoNetwork.stage;
            fAutoNetwork.state = kCx3AutoFailed;
            fNetEnabled = false; /* stop new host delivery; retain all device owners */
            LOG("auto network: stopped at stage %u status 0x%llx; no retry/reset/cleanup",
                (unsigned)fAutoNetwork.stage, status);
            return false;
        }
        fAutoNetwork.completedStage = fAutoNetwork.stage;
        fAutoNetwork.completedMask |= 1ull << (fAutoNetwork.stage - 1);
        return true;
    };
    /* No generic arbitrary-selector interpreter. Each typed production result
     * has its own acceptance predicate, including full QP/RSS readback. */
#define CX3_AUTO_STEP(n, sel, call, valid) do { \
        begin(n, sel); \
        if (selectorStopping() || fResetCommandsBlocked || fStopRetained || \
            fQuarExhausted || fHw.cmdWedged()) { finish(kIOReturnNotReady, false); goto auto_done; } \
        IOReturn stageRc = (call); \
        if (!finish(stageRc, (valid))) goto auto_done; \
    } while (0)
    {
        const UInt64 port[] = {1, 0}, profile[] = {1024,16,64,256,256,2048,4096};
        const UInt64 eq[] = {0,0,1,1,0}, mr[] = {0,0}, cq[] = {512,128};
        const UInt64 rq[] = {256,2048}, qp[] = {0}, tx[] = {0,0};
        CX3_AUTO_STEP(1, kCx3SelResetHca, exportResetHca(true,s),
                      fDeviceGeneration == 2 && !fResetCommandsBlocked);
        CX3_AUTO_STEP(2, kCx3SelQueryFw, exportQueryFw(true,s),
                      !s[kFwOutFwStatus] && s[kFwOutFwPages] && s[kFwOutFwPages] <= 65535);
        fwPages = (UInt32)s[kFwOutFwPages];
        CX3_AUTO_STEP(3, kCx3SelFwLoad, exportFwLoad(fwPages,false,false,false,s),
                      !s[kFwLoadOutMapFaStatus] && !s[kFwLoadOutRunFwStatus]);
        CX3_AUTO_STEP(4, kCx3SelQueryDevCap, exportQueryDevCap(true,false,&w->cap,s),
                      !w->cap.status && !w->cap.fwStatus);
        CX3_AUTO_STEP(5, kCx3SelM2dProfile, exportM2dProfile(profile,&w->profile,s),
                      !w->profile.status && !w->profile.fwStatus && w->profile.numTables > 0);
        CX3_AUTO_STEP(6, kCx3SelM2dBringUp, exportM2dBringUp(s),
                      fHcaInited && fIcmMapped && !s[kBringUpOutInitHcaStat] &&
                      s[kBringUpOutTablesDone] && s[kBringUpOutTablesDone] == s[kBringUpOutTablesTotal]);
        CX3_AUTO_STEP(7, kCx3SelM3QueryPort, exportM3QueryPort(port,&w->port,s),
                      !w->port.status && !w->port.fwStatus && (w->port.supportedTypes & 2u));
        CX3_AUTO_STEP(8, kCx3SelM3PortUp, exportM3PortUp(port,s),
                      s[kPortUpOutStep] == kPortUpSteps && !s[kPortUpOutSetFwStatus] &&
                      !s[kPortUpOutQpnCalcFwStatus] && !s[kPortUpOutInitFwStatus]);
        CX3_AUTO_STEP(9, kCx3SelM3EqUp, exportM3EqUp(eq,s),
                      !s[kEqUpOutFwStatus] && !s[kEqUpOutMapEqStatus]);
        CX3_AUTO_STEP(10, kCx3SelR4Mr, exportR4Mr(mr,&w->mr,s),
                      !w->mr.status && !w->mr.fwStatus && w->mr.queryMptOk);
        CX3_AUTO_STEP(11, kCx3SelR4Cq, exportR4Cq(cq,&w->cq,s),
                      !w->cq.status && !w->cq.fwStatus && !w->cq.readyAtStart);
        CX3_AUTO_STEP(12, kCx3SelR4Rq, exportR4Rq(rq,&w->rq,s),
                      !w->rq.status && w->rq.wqe0Matches && w->rq.wqeLastMatches);
        CX3_AUTO_STEP(13, kCx3SelR4Qp, exportR4Qp(qp,&w->qp,s),
                      !w->qp.status && w->qp.stepsAccepted == 3 && w->qp.statesConfirmed == 3 &&
                      w->qp.queryOk && w->qp.queryState == 3 && w->qp.qpnConfirmed &&
                      w->qp.queryAllFieldsMatch && w->qp.queryMttMatches && w->qp.producerMatches);
        CX3_AUTO_STEP(14, kCx3SelR4Ingress, exportR4Ingress(&w->ingress,s),
                      !w->ingress.status && !w->ingress.macFwStatus &&
                      w->ingress.macReadbackOk && w->ingress.macReadbackMatches &&
                      w->ingress.stepsAccepted == 3 && w->ingress.statesConfirmed == 3 &&
                      w->ingress.queryOk && w->ingress.queryState == 3 &&
                      w->ingress.queryAllFieldsMatch && w->ingress.queryMttMatches &&
                      w->ingress.queryRssMatches && w->ingress.producerMatches);
        CX3_AUTO_STEP(15, kCx3SelR4Tx, exportR4Tx(tx,&w->tx,s),
                      !w->tx.status && !w->tx.cqFwStatus && !w->tx.cqReadyAtStart &&
                      w->tx.numberSafe && w->tx.stepsAccepted == 3 && w->tx.statesConfirmed == 3 &&
                      w->tx.queryOk && w->tx.queryState == 3 && w->tx.queryAllFieldsMatch &&
                      w->tx.queryMttMatches && w->tx.qpnConfirmed && w->tx.sqStampMatches);
        CX3_AUTO_STEP(16, kCx3SelNetUp, exportNetUp(0,s),
                      !s[kNetUpOutRefused] && s[kNetUpOutMacOk] == 1 &&
                      fNetIface && fNetTimer && fNetTxRing);
    }
    fAutoNetwork.state = fAutoNetworkUserDisabled ? kCx3AutoDisabled : kCx3AutoReady;
    /* A previous 'disable while idle' persists until explicit enable. */
    if (fAutoNetworkUserDisabled) {
        bool on = true;
        IOWorkLoop *loop = getWorkLoop();
        IOReturn rc = loop ? loop->runAction(&Cx3Mlx4::autoNetworkOnWorkLoop, this, &on)
                           : kIOReturnNotReady;
        fAutoNetwork.status = (UInt64)rc;
        fAutoNetwork.state = rc ? kCx3AutoFailed : kCx3AutoReady;
    }
auto_done:
#undef CX3_AUTO_STEP
    IOFree(w, sizeof(Scratch));
    snapshot();
    return kIOReturnSuccess;
}

bool Cx3Mlx4::recordPortQuery(UInt32 port, const Cx3QueryPortResult &query)
{
    if (port < 1 || port > 2) return false;
    UInt64 ticks = 0, ns = 0;
    clock_get_uptime(&ticks);
    absolutetime_to_nanoseconds(ticks, &ns);
    return fPorts[port - 1].observe(query, fDeviceGeneration, ns);
}

IOReturn Cx3Mlx4::exportPortInfo(UInt32 port, bool refresh, Cx3PortInfoResult *res)
{
    if (!res || port < 1 || port > 2) return kIOReturnBadArgument;
    Cx3PortState &state = fPorts[port - 1];
    IOReturn rc = kIOReturnSuccess;
    if (refresh) {
        if (!fHcaInited || !fIcmMapped || fResetCommandsBlocked || fStopRetained ||
            retentionExhausted() || fHw.cmdWedged()) {
            rc = kIOReturnNotReady;
        } else {
            Cx3QueryPortResult query;
            memset(&query, 0, sizeof(query));
            const bool answered = fHw.queryPort(port, &query);
            const bool observed = recordPortQuery(port, query);
            if (!answered || !observed) rc = kIOReturnError;
        }
    }
    state.snapshot(*res);
    if (!rc && (state.generation != fDeviceGeneration || !state.queryValid || state.lastQueryFailed))
        rc = kIOReturnNotReady;
    res->status = (UInt64)rc;
    return kIOReturnSuccess;
}

bool Cx3Mlx4::preparePortQueueNumbers()
{
    if (fQueueNamespace.valid) return fQueueNamespace.generation==fDeviceGeneration &&
        fQueueNamespace.ports==fDevCap.num_ports;
    if (!fHcaInited || !fIcmMapped || fMrState!=1 || !fEqRing || fEqUnknownRetained ||
        fDevCap.num_ports<1 || fDevCap.num_ports>2) return false;
    for (UInt32 p=0;p<2;++p) if (fPortQueues[p].hasOwners()) return false;
    if (fNetIface || fNetTimer || fNetTxRing || fNetEnabled) return false;
    UInt32 maxMacLog=0;
    for (UInt32 p=1;p<=fDevCap.num_ports;++p) {
        Cx3QueryPortResult q; memset(&q,0,sizeof(q));
        bool ok=fHw.queryPort(p,&q); bool observed=recordPortQuery(p,q);
        if (!ok || !observed || q.logMaxMacs>7) return false;
        if (q.logMaxMacs>maxMacLog) maxMacLog=q.logMaxMacs;
    }
    if (!fQueueNamespace.configure(fInitHca.log_num_qps,fInitHca.log_num_cqs,
        fDevCap.reserved_qps,fDevCap.reserved_cqs,fDevCap.num_ports,maxMacLog,fDeviceGeneration))
        return false;
    for (UInt32 p=0;p<fDevCap.num_ports;++p) {
        Cx3PortQueues &q=fPortQueues[p];
        q.numbers=fQueueNamespace.numbers[p]; q.numbersReserved=true;
        q.numberGeneration=fDeviceGeneration;
    }
    return true;
}

IOReturn Cx3Mlx4::exportPortQueue(UInt32 port, UInt32 action, UInt64 expectedGeneration,
                                 Cx3PortQueueResult *res)
{
    if (!res || port<1 || port>2 || action>CX3_PORT_QUEUE_DESTROY) return kIOReturnBadArgument;
    Cx3PortQueues &q=fPortQueues[port-1];
    IOReturn rc=kIOReturnSuccess;
    auto snapshot=[&]() {
        memset(res,0,sizeof(*res)); res->version=CX3_PORT_QUEUE_VERSION; res->structBytes=sizeof(*res);
        res->status=(UInt64)rc; res->deviceGeneration=fDeviceGeneration; res->port=port; res->action=action;
        res->flags=(q.numbersReserved ? CX3_PORT_QUEUE_RESERVED:0u) |
            (q.setupReady && q.numberGeneration==fDeviceGeneration ? CX3_PORT_QUEUE_READY:0u) | (q.setupFailed ? CX3_PORT_QUEUE_FAILED:0u) |
            (q.hasOwners() ? CX3_PORT_QUEUE_OWNERS:0u);
        res->stage=q.setupStage; res->failedStage=q.failedStage; res->fwStatus=q.lastFwStatus;
        res->rxQpn=q.qpQpn; res->rssQpn=q.ingressQpn; res->txQpn=q.txQpn;
        res->rxCqn=q.cqCqn; res->txCqn=q.txCqn;
        if (q.numbersReserved) { res->macBase=q.numbers.macBase; res->macSlots=q.numbers.macSlots; }
        res->numPorts=fDevCap.num_ports; res->completedMask=q.completedStages;
        Mlx4Icm *owners[]={q.qpRec,q.cqRing,q.cqRec,q.rqRing,q.rqBufs,q.ingressRec,
            q.txQpRec,q.txSq,q.rawFrame,q.txCqRing,q.txCqRec};
        for (UInt32 i=0;i<sizeof(owners)/sizeof(owners[0]);++i) if (owners[i]) res->ownerMask|=1ull<<i;
    };
    if (!expectedGeneration || expectedGeneration!=fDeviceGeneration ||
        (q.numbersReserved && q.numberGeneration!=fDeviceGeneration)) rc=kIOReturnNotReady;
    if (!rc && action!=CX3_PORT_QUEUE_STATUS && (fResetCommandsBlocked || fStopRetained ||
        retentionExhausted() || fHw.cmdWedged() || !fHcaInited || port>fDevCap.num_ports))
        rc=kIOReturnNotReady;
    if (!rc && action!=CX3_PORT_QUEUE_STATUS && fNativePorts[port-1]) rc=kIOReturnNotReady;
    if (rc || action==CX3_PORT_QUEUE_STATUS) { snapshot(); return kIOReturnSuccess; }
    if (action==CX3_PORT_QUEUE_DESTROY) {
        /* Native callbacks are still primary-only. Their drain/detach protocol
         * must own primary disposal; a management call cannot bypass it. */
        if ((port==1 && (fNetIface || fNetTimer || fNetTxRing || fNetEnabled)) ||
            (q.numbersReserved && q.numberGeneration!=fDeviceGeneration)) rc=kIOReturnNotReady;
        else {
            q.setupReady=false;
            Cx3PortState &state=fPorts[port-1];
            if (state.initAttempted && !state.closeConfirmed) {
                UInt32 fw=CX3_FWSTATUS_NOT_POSTED;
                bool closed=fHw.closePort(port,&fw); state.closeFwStatus=fw;
                if (!closed || fw) rc=kIOReturnError;
                else { state.closeConfirmed=true; state.initAccepted=false; state.linkKnown=false;
                       state.linkUp=false; state.speedMbps=0; }
            }
            if (!rc) {
                ingressTeardown("per-port destroy",false,port);
                txTeardown("per-port destroy",false,port);
                qpTeardown("per-port destroy",false,port);
                cqTeardown("per-port destroy",false,port);
                rqTeardown("per-port destroy",false,port);
                q.setupReady=false;
                if (q.hasOwners()) rc=kIOReturnNotReady;
                else { q.setupAttempted=q.setupFailed=false; q.setupStage=q.failedStage=0;
                       q.completedStages=0; q.lastFwStatus=CX3_FWSTATUS_NOT_POSTED; }
            }
        }
        snapshot(); return kIOReturnSuccess;
    }
    if (q.setupFailed || (q.numbersReserved && q.numberGeneration!=fDeviceGeneration)) rc=kIOReturnNotReady;
    if (!rc && q.setupReady) { snapshot(); return kIOReturnSuccess; }
    if (!rc && (q.setupAttempted || q.hasOwners() || !preparePortQueueNumbers())) rc=kIOReturnNotReady;
    if (rc) { snapshot(); return kIOReturnSuccess; }
    union Work { Cx3R4CqResult cq; Cx3R4RqResult rq; Cx3R4QpResult qp;
                 Cx3R4IngressResult rss; Cx3R4TxResult tx; };
    Work *w=(Work*)IOMalloc(sizeof(Work));
    if (!w) { rc=kIOReturnNoMemory; snapshot(); return kIOReturnSuccess; }
    q.setupAttempted=true;
    UInt64 s[CX3_SCALAR_OUT_MAX]{}; const UInt64 cq[]={0,0},rq[]={0,16384},qp[]={0},tx[]={0,0};
#define CX3_QUEUE_STAGE(n,call,valid,fw) do { \
    q.setupStage=(n); q.lastFwStatus=CX3_FWSTATUS_NOT_POSTED; memset(w,0,sizeof(*w)); memset(s,0,sizeof(s)); \
    if (selectorStopping() || fResetCommandsBlocked || fStopRetained || retentionExhausted() || \
        fHw.cmdWedged() || q.numberGeneration!=fDeviceGeneration) { \
        rc=kIOReturnNotReady; q.setupFailed=true; q.failedStage=(n); goto queue_done; } \
    rc=(call); q.lastFwStatus=(fw); \
    if (rc || s[0] || !(valid)) { if (!rc) rc=kIOReturnError; \
        q.setupFailed=true; q.failedStage=(n); goto queue_done; } \
    q.completedStages|=1ull<<((n)-1); \
} while(0)
    CX3_QUEUE_STAGE(1,exportR4Cq(cq,&w->cq,s,port),!w->cq.status && !w->cq.fwStatus && !w->cq.readyAtStart,w->cq.fwStatus);
    CX3_QUEUE_STAGE(2,exportR4Rq(rq,&w->rq,s,port),!w->rq.status && w->rq.wqe0Matches && w->rq.wqeLastMatches,CX3_FWSTATUS_NOT_POSTED);
    CX3_QUEUE_STAGE(3,exportR4Qp(qp,&w->qp,s,port),!w->qp.status && w->qp.stepsAccepted==3 && w->qp.statesConfirmed==3 &&
        w->qp.queryOk && w->qp.queryState==3 && w->qp.qpnConfirmed && w->qp.queryAllFieldsMatch && w->qp.queryMttMatches && w->qp.producerMatches,w->qp.queryFwStatus);
    /* MAC write acknowledgement is the available command receipt. There is
     * no speculative SET_PORT read direction. Addressed traffic remains an
     * explicit hardware acceptance item; RSS/QPC must still match exactly. */
    CX3_QUEUE_STAGE(4,exportR4Ingress(&w->rss,s,port),!w->rss.status && !w->rss.macFwStatus &&
        w->rss.stepsAccepted==3 && w->rss.statesConfirmed==3 && w->rss.queryOk && w->rss.queryState==3 &&
        w->rss.queryAllFieldsMatch && w->rss.queryMttMatches && w->rss.queryRssMatches && w->rss.producerMatches,w->rss.queryFwStatus);
    CX3_QUEUE_STAGE(5,exportR4Tx(tx,&w->tx,s,port),!w->tx.status && !w->tx.cqFwStatus && !w->tx.cqReadyAtStart &&
        w->tx.numberSafe && w->tx.stepsAccepted==3 && w->tx.statesConfirmed==3 && w->tx.queryOk && w->tx.queryState==3 &&
        w->tx.queryAllFieldsMatch && w->tx.queryMttMatches && w->tx.qpnConfirmed && w->tx.sqStampMatches,w->tx.queryFwStatus);
    q.setupReady=true;
queue_done:
#undef CX3_QUEUE_STAGE
    IOFree(w,sizeof(Work)); snapshot(); return kIOReturnSuccess;
}


/* Managed native controllers never acquire ownership of the PF's DMA ledger.
 * This method executes under the selector transaction, without a child gate. */
IOReturn Cx3Mlx4::exportNativePort(UInt32 port,UInt32 action,UInt64 generation,Cx3NativePortResult*r)
{
 if(!r||port<1||port>2||action>CX3_NATIVE_DETACH)return kIOReturnBadArgument;
 memset(r,0,sizeof(*r));r->version=CX3_NATIVE_VERSION;r->structBytes=sizeof(*r);
 r->port=port;r->action=action;r->deviceGeneration=fDeviceGeneration;r->interfaceUnit=0xffffffffu;
 IOReturn rc=0;auto*&child=fNativePorts[port-1];auto&q=fPortQueues[port-1];
 if(!generation||generation!=fDeviceGeneration||port>fDevCap.num_ports)rc=kIOReturnNotReady;
 if(!rc&&action==CX3_NATIVE_ATTACH&&(selectorStopping()||fResetCommandsBlocked||fStopRetained||retentionExhausted()||fHw.cmdWedged()))rc=kIOReturnNotReady;
 if(!rc&&action==CX3_NATIVE_ATTACH){
  if(child){if(child->poisoned()||!child->attached())rc=kIOReturnNotReady;}
  else if((port==1&&(fNetIface||fNetTimer||fNetTxRing||fNetEnabled))||!q.setupReady||!q.numbersReserved||
          q.numberGeneration!=generation||!fPorts[port-1].initAccepted||q.rawFrame||
          !q.qpUp||!q.ingressUp||!q.txQpUp||!q.rqRing||!q.rqBufs||!q.txSq||!q.txCqRing||
          !q.txSqNent||q.txSqNent>256||(q.txSqNent&(q.txSqNent-1u))||
          !q.rqNent||q.rqNent>CX3_R4RAW_MAX_RX_SLOTS||(q.rqNent&(q.rqNent-1u))||
          q.rqBufBytes<2048||q.rqBufBytes>16384||(q.rqBufBytes&(q.rqBufBytes-1u))||
          !fPorts[port-1].mtuConfigured||
          fPorts[port-1].frameMtu<1500u+MLX4_ETH_HLEN+MLX4_ETH_VLAN_LEN+MLX4_ETH_FCS_LEN||
          fPorts[port-1].frameMtu>q.rqBufBytes-MLX4_NET_IP_ALIGN)rc=kIOReturnNotReady;
  else {
   UInt8 mac[6];memcpy(mac,q.mac+2,6);UInt8 any=0;for(UInt32 i=0;i<6;++i)any|=mac[i];
   if(!any||(mac[0]&1))rc=kIOReturnNotReady;
   else {
    auto*c=new Cx3EthernetPort;
    if(!c)rc=kIOReturnNoMemory;
    else if(!c->init()){c->release();rc=kIOReturnNoMemory;}
    else {
     child=c; // pin before any operation that can create a callback/worker
     if(!c->bind(this,port,generation,mac)||!c->attach(this)||!c->start(this))rc=kIOReturnError;
     else {
      UInt32 pages=(q.txSqNent*q.rqBufBytes+4095u)/4096u;
      q.rawFrame=fHw.icmAlloc(pages,false,q.rqBufBytes>4096u);q.rawFrameBytes=q.rawFrame?pages*4096u:0;
      bool spansOk=q.rawFrame!=nullptr;
      for(UInt32 i=0;spansOk&&i<q.txSqNent;++i)
       if(!fHw.rqBufferPhys(q.rawFrame,q.rqBufBytes,i))spansOk=false;
      Mlx4Icm*owners[]={q.rawFrame};
      if(!spansOk||!fHw.markDmaHandoff(owners,1))rc=kIOReturnNoMemory;
      else if(!c->attachAndWait())rc=kIOReturnError;
     }
    }
   }
  }
 } else if(!rc&&action==CX3_NATIVE_DETACH&&child){
  if(!child->shutdown())rc=kIOReturnNotReady;
  else {child->stop(this);if(child->getProvider()==this)child->detach(this);child->release();child=nullptr;}
 }
 if(child)child->snapshot(r);
 r->status=(UInt64)rc;return kIOReturnSuccess;
}
bool Cx3Mlx4::stopNativePorts()
{
 for(UInt32 p=0;p<2;++p)if(fNativePorts[p])fNativePorts[p]->closeAdmission();
 for(UInt32 p=0;p<2;++p)if(fNativePorts[p]){
  auto*c=fNativePorts[p];if(!c->shutdown())return false;
  c->stop(this);if(c->getProvider()==this)c->detach(this);c->release();fNativePorts[p]=nullptr;
 }
 return true;
}

/* One selector transaction spans policy edits and the corresponding HW command.
 * No worker waiting for this same transaction is started from here. */
IOReturn Cx3Mlx4::exportNativeConfig(UInt32 port,UInt32 action,UInt64 generation,UInt32 value,Cx3NativeConfigResult*r)
{
 if(!r||port<1||port>2||action>CX3_NCFG_TXCSUM||
    (action<=CX3_NCFG_VLAN_CLEAR&&value)||
    ((action==CX3_NCFG_VLAN_ADD||action==CX3_NCFG_VLAN_DEL)&&value>=4095u)||
    (action>=CX3_NCFG_RXCSUM&&value>1u))return kIOReturnBadArgument;
 memset(r,0,sizeof(*r));r->version=CX3_NATIVE_CONFIG_VERSION;r->structBytes=sizeof(*r);
 r->port=port;r->action=action;r->value=value;r->deviceGeneration=fDeviceGeneration;
 auto&q=fPortQueues[port-1];auto*c=fNativePorts[port-1];IOReturn rc=kIOReturnSuccess;
 if(!generation||generation!=fDeviceGeneration||port>fDevCap.num_ports)rc=kIOReturnNotReady;
 if(!rc&&action!=CX3_NCFG_STATUS){
  if(selectorStopping()||fResetCommandsBlocked||fStopRetained||retentionExhausted()||fHw.cmdWedged()||
     (c&&c->poisoned())||(port==1&&fNetIface))rc=kIOReturnNotReady;
  else if(action==CX3_NCFG_TXCSUM){
   if(c&&q.nativeTxCsum!=bool(value))rc=kIOReturnBusy;
   else q.nativeTxCsum=bool(value);
  }else if(action==CX3_NCFG_RXCSUM){
   if(c)rc=c->configureRxChecksum(bool(value));
   if(!rc)q.nativeRxCsum=bool(value);
  }else {
   UInt8 bits[512];memcpy(bits,q.vlanBits,sizeof(bits));bool filtering=q.vlanFilter;
   if(action==CX3_NCFG_VLAN_ALL){filtering=false;memset(bits,0,sizeof(bits));}
   else if(action==CX3_NCFG_VLAN_CLEAR){filtering=true;memset(bits,0,sizeof(bits));}
   else if(!filtering)rc=kIOReturnBadArgument; // clear before editing a whitelist
   else if(action==CX3_NCFG_VLAN_ADD)bits[value>>3]|=UInt8(1u<<(value&7u));
   else bits[value>>3]&=UInt8(~(1u<<(value&7u)));
   if(!rc&&c)rc=c->applyVlanFilter(filtering?bits:nullptr);
   if(!rc){q.vlanFilter=filtering;memcpy(q.vlanBits,bits,sizeof(bits));if(!c)q.vlanApplied=false;}
  }
 }
 r->status=(UInt64)rc;r->fwStatus=q.vlanFwStatus;r->vlanRevision=q.vlanRevision;
 r->flags=(q.nativeTxCsum?CX3_NCFG_TX_ON:0)|(q.nativeRxCsum?CX3_NCFG_RX_ON:0)|
  (q.vlanFilter?CX3_NCFG_VLAN_FILTER:0)|(q.vlanApplied&&c&&c->attached()&&q.setupReady&&q.numberGeneration==fDeviceGeneration?CX3_NCFG_VLAN_APPLIED:0);
 if(!q.vlanFilter)r->vlanCount=4095;
 else for(UInt32 i=0;i<4095;++i)if(q.vlanBits[i>>3]&(1u<<(i&7u)))++r->vlanCount;
 return kIOReturnSuccess;
}
