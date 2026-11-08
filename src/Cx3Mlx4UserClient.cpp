/*
 * Part of cx3-mlx4, a macOS IOKit port of the FreeBSD Mellanox driver.
 *
 * Original work in this repository.  It cites no upstream file:line and is
 * derived from none; the licence in LICENSE governs it.
 *
 * Copyright (c) 2026 Hi-Jiajun and contributors
 */

#include "Cx3Mlx4UserClient.h"
#include "Cx3Mlx4.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "Cx3R4Output.h"

#include "Cx3Debug.h"   /* LOG / LOGBOOT / boot-arg gates */

/*
 * THE SCALAR BLOCK'S WIDTH IS THE KERNEL'S, AND THIS IS THE ONE PLACE BOTH NUMBERS ARE
 * VISIBLE.  src/cx3_abi.h defines CX3_SCALAR_OUT_MAX without including any IOKit header (it
 * is included by both the kernel and the tool, in either order), so it cannot assert against
 * the kernel's constant itself.  This file does include <IOKit/IOUserClient.h> (through its
 * own header), so the identity is asserted here:
 *
 *   Kernel.framework/Headers/IOKit/IOUserClient.h:116-119 (the SDK copy; the copy this build
 *   includes is tools/research/mksdk/Headers/IOKit/IOUserClient.h:150-153):
 *
 *     enum {
 *         kIOExternalMethodScalarInputCountMax  = 16,
 *         kIOExternalMethodScalarOutputCountMax = 16,
 *     };
 *
 * Why this line exists.  0.43.9 raised CX3_SCALAR_OUT_MAX to 20 to fit three new `r4tx`
 * scalars, believing it was a knob.  It is the number above: a declared width past it is
 * refused with kIOReturnBadArgument (0xe00002c2) before any of the selector's code runs --
 * for every argument combination a caller can try, since no caller can supply a width the
 * transport does not have.  All `r4tx` calls in 0.43.9 behaved exactly that way, so the
 * change that batch was made to measure was never measured (docs/DEFECTS.md F13, which also
 * records what reading XNU says about WHERE the refusal happens: not at the dispatch table's
 * own count check, which compares the caller's counts with the declared ones and would pass).
 * Raising our number again is now a build failure at this line rather than a selector that
 * silently cannot be called; the second half of the guard is the per-selector
 * `CX3_STATIC_ASSERT(kXxxOutCount <= CX3_SCALAR_OUT_MAX)` beside each enum in the ABI header.
 */
CX3_STATIC_ASSERT(CX3_SCALAR_OUT_MAX == kIOExternalMethodScalarOutputCountMax,
                  "CX3_SCALAR_OUT_MAX must equal the kernel's "
                  "kIOExternalMethodScalarOutputCountMax (IOUserClient.h:116-119); it is the "
                  "width of the kernel's scalar array, not a tunable");
CX3_STATIC_ASSERT(CX3_SCALAR_IN_MAX == kIOExternalMethodScalarInputCountMax,
                  "CX3_SCALAR_IN_MAX must equal the kernel's "
                  "kIOExternalMethodScalarInputCountMax (IOUserClient.h:116-119)");

namespace {

/*
 * Releases the provider's selector admission exactly once, on every return path.
 *
 * externalMethod() has four exits (no-lock, try-lock refused, try-lock taken, plain locked)
 * plus the two early refusals; incrementing a counter by hand at the top and decrementing at
 * each of them is one edit away from a stop() that waits forever for a selector that already
 * returned.  A destructor cannot be forgotten.
 */
class Cx3SelectorGuard {
public:
    explicit Cx3SelectorGuard(Cx3Mlx4 *provider) : fProvider(provider) {}
    ~Cx3SelectorGuard()
    {
        if (fProvider) fProvider->selectorExit();
    }

private:
    Cx3Mlx4 *fProvider;
};

}   /* namespace */

OSDefineMetaClassAndStructors(Cx3Mlx4UserClient, IOUserClient)

/*
 * The dispatch table is what makes the base class validate argument counts and
 * allocate the scalar arrays; the check columns must match the ABI header
 * exactly, or the call is rejected with kIOReturnBadArgument before it reaches
 * our code.
 *
 * Unsized deliberately: the size has to come from this list so that the assertion in
 * externalMethod() is checking the list rather than the declaration (see the header).
 */
const IOExternalMethodDispatch Cx3Mlx4UserClient::sMethods[] = {
    /* probe   */ { &Cx3Mlx4UserClient::sProbe,    0, 0, kProbeOutCount, 0 },
    /* dma     */ { &Cx3Mlx4UserClient::sDma,      0, 0, kDmaOutCount,   0 },
    /* queryfw */ { &Cx3Mlx4UserClient::sQueryFw,  kFwInCount, 0, kFwOutCount, 0 },
    /* reset   */ { &Cx3Mlx4UserClient::sResetHca, kRstInCount, 0, kRstOutCount, 0 },
    /* peek    */ { &Cx3Mlx4UserClient::sPeek,     kPeekInCount_, 0, kPeekOutCount_, 0 },
    /* clearown*/ { &Cx3Mlx4UserClient::sClearOwner, kClrInCount, 0, kClrOutCount, 0 },
    /* poke    */ { &Cx3Mlx4UserClient::sPoke,     kPokeInCount, 0, kPokeOutCount, 0 },
    /* flr     */ { &Cx3Mlx4UserClient::sFlr,      kFlrInCount, 0, kFlrOutCount, 0 },
    /* querydevcap */ { &Cx3Mlx4UserClient::sQueryDevCap, kDevCapInCount, 0, kDevCapOutCount,
                   (uint32_t)sizeof(Cx3DevCapResult) },
    /* icm     */ { &Cx3Mlx4UserClient::sIcmAlloc,  kIcmInCount, 0, kIcmOutCount, 0 },
    /* fwload  */ { &Cx3Mlx4UserClient::sFwLoad,    kFwLoadInCount, 0, kFwLoadOutCount, 0 },
    /* fwarea  */ { &Cx3Mlx4UserClient::sFwArea,    kFwAreaInCount_, 0, kFwAreaOutCount, 0 },
    /* fwoutbox*/ { &Cx3Mlx4UserClient::sFwOutbox,  kFwOutboxInCount, 0, kFwOutboxOutCount, 0 },
    /* m2dprof */ { &Cx3Mlx4UserClient::sM2dProfile, kM2dInCount, 0, kProfOutCount,
                   (uint32_t)sizeof(Cx3ProfileResult) },
    /* m2dbring*/ { &Cx3Mlx4UserClient::sM2dBringUp, 0, 0, kBringUpOutCount, 0 },
    /* m3equp  */ { &Cx3Mlx4UserClient::sM3EqUp,    kEqUpInCount, 0, kEqUpOutCount, 0 },
    /* m3eqpoll*/ { &Cx3Mlx4UserClient::sM3EqPoll,  kEqPollInCount, 0, kEqPollOutCount, 0 },
    /* m3eqring*/ { &Cx3Mlx4UserClient::sM3EqRing,  kEqRingInCount, 0, kEqRingOutCount, 0 },
    /* m3eqinfo*/ { &Cx3Mlx4UserClient::sM3EqInfo,  0, 0, kEqInfoOutCount,
                   (uint32_t)sizeof(Cx3EqInfoResult) },
    /* m3irqup */ { &Cx3Mlx4UserClient::sM3IrqUp,    kIrqUpInCount, 0, kIrqUpOutCount, 0 },
    /* m3irqtest*/ { &Cx3Mlx4UserClient::sM3IrqTest,  kIrqTestInCount, 0, kIrqTestOutCount, 0 },
    /* m3irqst */ { &Cx3Mlx4UserClient::sM3IrqStatus, 0, 0, kIrqStatusOutCount, 0 },
    /* m3irqdown*/ { &Cx3Mlx4UserClient::sM3IrqDown,  0, 0, kIrqDownOutCount, 0 },
    /* m3clrint */ { &Cx3Mlx4UserClient::sM3ClrInt,   0, 0, kClrIntOutCount, 0 },
    /* queryport   */ { &Cx3Mlx4UserClient::sM3QueryPort, kQueryPortInCount, 0,
                   kQueryPortOutCount, (uint32_t)sizeof(Cx3QueryPortResult) },
    /* m3portup*/ { &Cx3Mlx4UserClient::sM3PortUp,    kPortUpInCount, 0, kPortUpOutCount, 0 },
    /* m3abring*/ { &Cx3Mlx4UserClient::sM3AbRing,    kAbRingInCount, 0,
                   kAbRingOutCount, (uint32_t)sizeof(Cx3AbRingResult) },
    /* queryhca    */ { &Cx3Mlx4UserClient::sM3QueryHca, 0, 0, kQueryHcaOutCount,
                   (uint32_t)sizeof(Cx3QueryHcaResult) },
    /* r4mr    */ { &Cx3Mlx4UserClient::sR4Mr, kR4MrInCount, 0, kR4MrOutCount,
                   (uint32_t)sizeof(Cx3R4MrResult) },
    /* r4cq    */ { &Cx3Mlx4UserClient::sR4Cq, kR4CqInCount, 0, kR4CqOutCount,
                   (uint32_t)sizeof(Cx3R4CqResult) },
    /* r4rq    */ { &Cx3Mlx4UserClient::sR4Rq, kR4RqInCount, 0, kR4RqOutCount,
                   (uint32_t)sizeof(Cx3R4RqResult) },
    /*
     * hcr is the first selector that takes a structure INPUT.  The size is pinned to the
     * mailbox size so the intent is stated in the dispatch table rather than checked by
     * hand: the caller always hands in one mailbox's worth of bytes and says in scalarInput
     * how many of them are meaningful.
     */
    /* hcr     */ { &Cx3Mlx4UserClient::sHcr, kHcrInCount, CX3_HCR_MAILBOX_SIZE,
                   kHcrOutCount, CX3_HCR_MAILBOX_SIZE },
    /* survey  */ { &Cx3Mlx4UserClient::sSurvey, 0, 0, kSurveyOutCount,
                   (uint32_t)sizeof(Cx3SurveyResult) },
    /* fault   */ { &Cx3Mlx4UserClient::sFault, kFaultInCount, 0, kFaultOutCount, 0 },
    /* r4qp    */ { &Cx3Mlx4UserClient::sR4Qp, kR4QpInCount, 0, kR4QpOutCount,
                   (uint32_t)sizeof(Cx3R4QpResult) },
    /* r4ingr  */ { &Cx3Mlx4UserClient::sR4Ingress, kR4IngressInCount, 0,
                   kR4IngressOutCount, (uint32_t)sizeof(Cx3R4IngressResult) },
    /* r4tx    */ { &Cx3Mlx4UserClient::sR4Tx, kR4TxInCount, 0, kR4TxOutCount,
                   (uint32_t)sizeof(Cx3R4TxResult) },
    /* r4raw   */ { &Cx3Mlx4UserClient::sR4Raw, kR4RawInCount, 0, kR4RawOutCount,
                   (uint32_t)sizeof(Cx3R4RawResult) },
    /* txprobe */ { &Cx3Mlx4UserClient::sTxProbe, kTxProbeInCount, 0, kTxProbeOutCount, 0 },
    /* netup   */ { &Cx3Mlx4UserClient::sNetUp, kNetUpInCount, 0, kNetUpOutCount, 0 },
    /* netstat */ { &Cx3Mlx4UserClient::sNetStat, 0, 0, kNetStatOutCount,
                   (uint32_t)sizeof(Cx3NetStatResult) },
    /* netlink */ { &Cx3Mlx4UserClient::sNetLink, kNetLinkInCount, 0, kNetLinkOutCount, 0 },
    /* netoff  */ { &Cx3Mlx4UserClient::sNetOff, kNetOffInCount, 0, kNetOffOutCount, 0 },
    /* netarm  */ { &Cx3Mlx4UserClient::sNetArm, kNetArmInCount, 0, kNetArmOutCount, 0 },
    /* netfilt */ { &Cx3Mlx4UserClient::sNetFilt, kNetFiltInCount, 0, kNetFiltOutCount, 0 },
    /* netport */ { &Cx3Mlx4UserClient::sNetPort, kNetPortInCount, 0, kNetPortOutCount, 0 },
    /* netupst */ { &Cx3Mlx4UserClient::sNetUpStop, kNetUpStopInCount, 0, kNetUpOutCount, 0 },
    /* queryadapter */ { &Cx3Mlx4UserClient::sQueryAdapter, 0, 0, 0,
                        (uint32_t)sizeof(Cx3AdapterResult) },
    /* pcielink */ { &Cx3Mlx4UserClient::sPcieLink, 0, 0, 0,
                    (uint32_t)sizeof(Cx3PcieLinkResult) },
    /* runtime identity */ { &Cx3Mlx4UserClient::sRuntimeIdentity, 0, 0, 0,
                             (uint32_t)sizeof(Cx3RuntimeIdentityResult) },
    /* module read */ { &Cx3Mlx4UserClient::sModuleRead, 3, 0, 0,
                       (uint32_t)sizeof(Cx3ModuleResult) },
    /* auto network */ { &Cx3Mlx4UserClient::sAutoNetwork, 0,
                         (uint32_t)sizeof(Cx3AutoNetworkRequest), 0,
                         (uint32_t)sizeof(Cx3AutoNetworkResult) },
    /* port info */ { &Cx3Mlx4UserClient::sPortInfo, 2, 0, 0,
                     (uint32_t)sizeof(Cx3PortInfoResult) },
    /* port queue */ { &Cx3Mlx4UserClient::sPortQueue, 3, 0, 0,
                      (uint32_t)sizeof(Cx3PortQueueResult) },
    /* native port */ { &Cx3Mlx4UserClient::sNativePort, 3, 0, 0,
                       (uint32_t)sizeof(Cx3NativePortResult) },
    /* native config */ { &Cx3Mlx4UserClient::sNativeConfig, 4, 0, 0,
                         (uint32_t)sizeof(Cx3NativeConfigResult) },
};


/*
 * Which selectors take the transaction lock, and in which manner.
 *
 * One entry per selector, in selector order, because this is a policy that a reader has to
 * be able to audit against the list above.  The three classes:
 *
 *   kXactLocked   the default, and the one that has to justify itself: hold the lock across
 *                 the whole selector call.  Everything that fills the mailbox, sends a
 *                 command, or modifies fTables[]/fQueues belongs here.
 *
 *   kXactTryLock  ask for the lock; refuse with kIOReturnBusy rather than queue.  For the
 *                 selectors that answer "what state is the driver in" and that a reader
 *                 needs *while* something else is running: a busy answer is information,
 *                 and a five-minute wait behind a stuck command is what made this driver
 *                 unreachable on 2026-10-12 (docs/LOCK-DESIGN.md sections 1.4 and 4.4).
 *
 *   kXactNoLock   do not take the lock at all.  Nothing here uses it: see the note on
 *                 probe below.  Keeping the value and the reasoning rather than deleting
 *                 the option is deliberate -- the design proposed it for probe and peek,
 *                 and the next reader should find out why it is not used instead of
 *                 re-deriving it.
 *
 * On probe: the design (docs/LOCK-DESIGN.md section 4.4) assigns it kXactNoLock, on the
 * ground that it only reads registers.  It does, and it also reads the reset semaphore at
 * 0xf03fc, which is the one register this project has measured as harmful to read at the
 * wrong moment: "a read of that word between the HCA reset and RUN_FW stops RUN_FW from
 * ever completing" (Mlx4Hw::windowAlive's comment, and docs/DEFECTS.md C1).  Taking the
 * lock is what currently keeps probe out of the middle of a reset (resetHca() writes the
 * reset register and then restores the PCI header, and nothing may read BAR0 in between).
 * Since kXactTryLock gives the same "answerable within a moment" property without opening
 * that window, probe uses it.  This is a deliberate departure from the design sketch and it
 * is recorded in docs/LOCK-IMPL.md.
 *
 * On peek: the design gives it kXactNoLock for the same "registers only" reason, and it is
 * the worst fit of the three, because peek reads *arbitrary* offsets including PCI config
 * space -- i.e. it can read exactly the registers the reset and FLR paths are writing.  Same
 * treatment as probe, same reason.
 */
enum { kXactLocked = 0, kXactTryLock = 1, kXactNoLock = 2 };

/*
 * Unsized, so that its length is the number of entries written here and the assertion below
 * is about this list.  With an explicit bound the assertion would compare the bound with
 * itself and could never fail -- the vacuous-check shape docs/DEFECTS.md F4 records.
 */
static const UInt8 kXactClass[] = {
    /*  0 probe       */ kXactTryLock,
    /*  1 dma         */ kXactLocked,
    /*  2 queryfw     */ kXactLocked,
    /*  3 reset       */ kXactLocked,   /* must stay queueing: it is the recovery path and
                                         * it is bounded now (docs/LOCK-IMPL.md, Q3) */
    /*  4 peek        */ kXactTryLock,
    /*  5 clearowner  */ kXactLocked,
    /*  6 poke        */ kXactLocked,
    /*  7 flr         */ kXactLocked,
    /*  8 querydevcap */ kXactLocked,
    /*  9 icmalloc    */ kXactLocked,
    /* 10 fwload      */ kXactLocked,
    /* 11 fwarea      */ kXactLocked,
    /* 12 fwoutbox    */ kXactLocked,
    /* 13 m2dprofile  */ kXactLocked,
    /* 14 m2dbringup  */ kXactLocked,
    /* 15 m3equp      */ kXactLocked,
    /* 16 m3eqpoll    */ kXactLocked,
    /* 17 m3eqring    */ kXactLocked,
    /* 18 m3eqinfo    */ kXactLocked,
    /* 19 m3irqup     */ kXactLocked,
    /* 20 m3irqtest   */ kXactLocked,
    /* 21 m3irqstatus */ kXactTryLock,  /* counters only; the EQ lock it takes gives the
                                         * consistency, this one only keeps it out of a
                                         * mailbox transaction */
    /* 22 m3irqdown   */ kXactLocked,
    /* 23 m3clrint    */ kXactLocked,
    /* 24 queryport   */ kXactLocked,
    /* 25 m3portup    */ kXactLocked,
    /* 26 m3abring    */ kXactLocked,
    /* 27 queryhca    */ kXactLocked,
    /* 28 r4mr        */ kXactLocked,
    /* 29 r4cq        */ kXactLocked,
    /* 30 r4rq        */ kXactLocked,
    /* 31 hcr         */ kXactLocked,
    /* 32 survey      */ kXactTryLock,  /* the "what state is everything in" call: it reads
                                         * fTables[]/fNumTables and every queue pointer, all
                                         * of which another selector can be changing (C9), so
                                         * it takes the lock -- but it must not wait, because
                                         * it is what a reader runs when something is stuck */
    /* 33 fault       */ kXactLocked,   /* mode 3 holds the lock on purpose, which is only
                                         * meaningful if this call owns it */
    /* 34 r4qp        */ kXactLocked,
    /* 35 r4ingress   */ kXactLocked,
    /* 36 r4tx        */ kXactLocked,
    /* 37 r4raw       */ kXactLocked,   /* the one selector that talks to the wire: it posts
                                         * WQEs, rings a doorbell and consumes completions from
                                         * two queues, so it has the strongest claim on "one
                                         * selector call is one transaction" of anything here.
                                         * It is also the longest-running call (a receive budget
                                         * of seconds), which is why the budget defaults are
                                         * named constants rather than "as long as it takes" */
    /* 38 txprobe     */ kXactLocked,   /* posts into the same send queue r4raw posts into,
                                         * rings the same doorbells and consumes completions
                                         * from the same two rings.  Two selectors sharing one
                                         * queue's producer index is exactly the state that has
                                         * to be one transaction; it sends no firmware command,
                                         * so the lock is held for milliseconds and cannot
                                         * poison anything (docs/LOCK-IMPL.md) */
    /* 39 netup       */ kXactLocked,   /* reads the port through QUERY_PORT and creates kernel
                                         * objects (an IOEthernetInterface and its nubs), so it
                                         * must not interleave with a selector that is
                                         * reconfiguring the device.  One command, one attach */
    /* 40 netstat     */ kXactTryLock,  /* counters only, and the same class as survey: it has to
                                         * be answerable while something is stuck, so it must not
                                         * wait.  What it reads is written by the data path's timer
                                         * on the workloop -- which holds the transaction lock
                                         * while it runs -- so getting in gives a consistent view
                                         * of the tick's own updates, and being refused while a
                                         * chain run holds the lock is the honest alternative to a
                                         * torn snapshot */
    /* 41 netlink     */ kXactLocked,   /* PUBLISH creates kernel objects and REQUERY sends
                                         * QUERY_PORT, so this is a transaction like every other
                                         * command-sending selector.  REPORT on its own would be a
                                         * TryLock, and the stricter class is the safe one for the
                                         * selector as a whole */
    /* 42 netoff      */ kXactTryLock,  /* counters and one flag, no device access at all: it
                                         * must be answerable while something is stuck */
    /* 43 netarm      */ kXactLocked,   /* it CONSUMES completions and re-posts their slots, so
                                         * it must be one transaction: a selector interleaving
                                         * here would see a receive ring being rewritten */
    /* 44 netfilt     */ kXactLocked,   /* it sends port commands (the multicast table), so it is
                                         * a transaction like every other command sender */
    /* 45 netport     */ kXactLocked,   /* it sends CLOSE_PORT, a device command, so it is a
                                         * transaction like every other command sender */
    /* 46 netupstop   */ kXactLocked,   /* it is netup's first step and nothing else, so it holds
                                         * the lock for the same reason netup does: the question it
                                         * exists to answer is whether ENTERING that path is what
                                         * the machine cannot survive, and running it in a
                                         * different lock class would change the thing being
                                         * measured */
    /* 47 queryadapter */ kXactLocked,
    /* 48 pcielink */ kXactTryLock,
    /* 49 runtime identity */ kXactTryLock,
    /* 50 module read */ kXactLocked,
    /* 51 auto network */ kXactTryLock, /* entire chain or immediate Busy */
    /* 52 port info */ kXactTryLock, /* bounded query or host cache; never queue behind init */
    /* 53 port queue */ kXactTryLock, /* one per-port control transaction, no queued retry */
    /* 54 native port */ kXactTryLock,
    /* 55 native config */ kXactTryLock,
};

/*
 * ...and that the table has one entry per selector.
 *
 * A shorter initialiser list is not a compile error in C++: the remaining entries are
 * zero-filled, and zero is kXactLocked -- the permissive default -- so a forgotten entry
 * would silently take the lock and read as correct (docs/DEFECTS.md C7/C10 are this shape:
 * a list that was nearly complete).  The dispatch table above has the same hazard for a
 * different reason (a missing entry shifts every later selector), so both sizes are pinned.
 */
CX3_STATIC_ASSERT(sizeof(kXactClass) / sizeof(kXactClass[0]) == kCx3SelCount,
                  "the lock-class table must have one entry per selector");

bool Cx3Mlx4UserClient::initWithTask(task_t owningTask, void *securityID,
                                     UInt32 type, OSDictionary *properties)
{
    fProvider = 0;
    fClientLock = 0;
    fClientStopping = false;
    if (!super::initWithTask(owningTask, securityID, type, properties)) return false;

    /*
     * This client can write the command register and reset the HCA, so it is
     * root-only.  Checked here rather than per selector so there is no path in
     * that could be reached without the check.
     */
    if (clientHasPrivilege(securityID, "root") != kIOReturnSuccess) {
        LOG("user client refused: caller is not root");
        return false;
    }

    fTask     = owningTask;
    fClientLock = IOLockAlloc();
    return fClientLock != 0;
}

bool Cx3Mlx4UserClient::start(IOService *provider)
{
    Cx3Mlx4 *candidate = OSDynamicCast(Cx3Mlx4, provider);
    if (!candidate || !fClientLock) {
        LOG("user client: provider or client admission lock unavailable");
        return false;
    }
    IOLockLock(fClientLock);
    bool refused = fClientStopping || fProvider != 0;
    IOLockUnlock(fClientLock);
    if (refused || !super::start(provider)) return false;
    IOLockLock(fClientLock);
    if (fClientStopping || fProvider) {
        IOLockUnlock(fClientLock);
        super::stop(provider);
        return false;
    }
    candidate->retain();
    fProvider = candidate;
    IOLockUnlock(fClientLock);
    return true;
}

void Cx3Mlx4UserClient::stop(IOService *provider)
{
    if (fClientLock) {
        IOLockLock(fClientLock);
        fClientStopping = true;
        IOLockUnlock(fClientLock);
    }
    super::stop(provider);
}

void Cx3Mlx4UserClient::free()
{
    /* Final OSObject destruction, not stop: an admitted handler and its exit
     * guard keep seeing the same provider for the client object's lifetime.
     * This owns object storage, not provider hardware/power or framework state. */
    if (fProvider) { fProvider->release(); fProvider = 0; }
    if (fClientLock) { IOLockFree(fClientLock); fClientLock = 0; }
    super::free();
}

bool Cx3Mlx4UserClient::clientReady()
{
    if (!fClientLock) return false;
    IOLockLock(fClientLock);
    bool ready = !fClientStopping && fProvider != 0;
    IOLockUnlock(fClientLock);
    return ready;
}

IOReturn Cx3Mlx4UserClient::clientClose()
{
    /*
     * NOT terminate().
     *
     * This line used to read `terminate();`, which is a pattern that appears in sample code and
     * is harmless while the provider is a plain IOService -- and it terminated the PROVIDER CHAIN,
     * so every close of the user client tore this driver down.  That was invisible until the
     * driver became an IONetworkingFamily subclass, at which point the teardown walked into the
     * family's own stop path with state that had never been set up and took the machine with it:
     *
     *     panic(cpu 0): Kernel trap, type = 14 = page fault, CR2 = 0x0
     *     com.apple.iokit.IONetworkingFamily : IONetworkController::handleIsOpen(IOService const*)
     *       <- IOService::actionStop <- IOWorkLoop::runAction <- IOService::terminateWorker
     *     (Kernel-2026-10-13-002213.panic, boot args included cx3raw=1 cx3debug=1)
     *
     * Nothing needs to be terminated here: the framework releases the client once the task's
     * references are gone, which is what this method's return value tells it.  The user client
     * owning no DMA resources is why close does not tear down the device. The
     * provider reference and client lock are released at final free(), not here.
     */
    if (fProvider)
        fProvider->userClientOpenMark(Cx3Mlx4::kCx3OpenPhaseClosed);
    LOG("user client closed");
    return kIOReturnSuccess;
}

IOReturn Cx3Mlx4UserClient::externalMethod(uint32_t selector,
                                           IOExternalMethodArguments *arguments,
                                           IOExternalMethodDispatch *dispatch,
                                           OSObject *target, void *reference)
{
    if (!clientReady()) return kIOReturnNotAttached;
    if (fProvider)
        fProvider->userClientMark(selector, Cx3Mlx4::kCx3ClientPhaseEntered);
    /*
     * Once the deferred attach worker has timed out, its state is unknown.
     * Refuse every later selector rather than letting a reset, mailbox command
     * or second netup race a worker that may still be inside the family.
     * Recovery is an explicit stop()/reboot, which cancels and waits for the
     * worker before tearing the provider down.
     */
    if (fProvider && fProvider->netAttachPoisoned()) {
        LOG("selector %u: refused because the deferred attach worker is poisoned "
            "(timeout/unknown state); stop or reboot before issuing more device commands",
            (unsigned)selector);
        fProvider->userClientMark(selector, Cx3Mlx4::kCx3ClientPhaseBusy);
        return kIOReturnNotPermitted;
    }
    /*
     * One line at every entry, and it is the cheapest instrument this investigation has.
     *
     * The freezes whose driver log survives all end with "user client closed" -- the last thing
     * THIS driver says before the framework tears the client down -- and the next thing the log
     * would show is the next call's entry.  With the entry line present, a freeze leaves either
     *
     *     ... closed            <- and nothing after it: the stall is in the teardown
     *     ... closed, enter N    <- the teardown completed, so the stall is in selector N
     *
     * and that is a distinction no measurement we have can make today.  No counter and no state:
     * the log stream already timestamps every line, and the selector number is the one piece of
     * context a reader would otherwise have to reconstruct from the previous lines.
     *
     * (Added 2026-10-13 with the freeze investigation; see docs/HANG-AUDIT.md.)
     */
    LOG("enter selector %u (scalarIn %u, scalarOut %u)",
        (unsigned)selector,
        (unsigned)(arguments ? arguments->scalarInputCount : 0),
        (unsigned)(arguments ? arguments->scalarOutputCount : 0));
    /*
     * Admit the call, or refuse it because stop() has closed the gate.  After admission every
     * return path releases the count through the guard above; stop() waits for that count to
     * reach zero before it unmaps a table or tears down a queue.
     */
    if (fProvider && !fProvider->selectorEnter()) {
        LOG("selector %u: refused with kIOReturnNotPermitted -- the driver is stopping "
            "or its admission locks are unavailable", (unsigned)selector);
        fProvider->userClientMark(selector, Cx3Mlx4::kCx3ClientPhaseBusy);
        return kIOReturnNotPermitted;
    }
    Cx3SelectorGuard selectorGuard(fProvider);
    /*
     * One row per selector, checked against the definition above rather than against a
     * declaration that would make the array the right size no matter what.  A row that is
     * missing leaves sMethods[selector] all-zero, and the base class would call a null
     * dispatch function -- a kext panic, not a rejection.  Here because this is the one place
     * outside the class that can name the private member without widening its access.
     */
    CX3_STATIC_ASSERT(sizeof(sMethods) / sizeof(sMethods[0]) == kCx3SelCount,
                      "the dispatch table must have one entry per selector");

    /*
     * ...and that each row is in the right PLACE, which the count cannot say.  The rows above
     * are POSITIONAL -- sMethods[selector] -- so a row inserted anywhere but its own index
     * shifts every later selector onto the wrong handler, and the kernel's argument check then
     * rejects the call with kIOReturnBadArgument while the COUNT stays right.
     *
     * Measured 2026-10-13: six rows (netstat..netport) were appended after `survey` (index 32)
     * instead of at indices 40..45, and every selector from 33 up failed that way at the chain's
     * r4qp step -- a chain run that reached INIT_HCA and the port and then failed four R4 steps
     * with "kern_return 0xe00002c2", which is what that rejection looks like from outside.
     *
     * The guard for it is in build.sh, not here: this translation unit cannot assert on the
     * table's contents (a read of a `static const` array's initialiser is not a constant
     * expression), so build.sh compares the row ORDER of this table against the lock-class table
     * below, which carries a number in every row.  A shift in either one fails the build.
     */

    if (selector < (uint32_t)kCx3SelCount) {
        dispatch = (IOExternalMethodDispatch *)&sMethods[selector];
        if (!target) target = this;
    }
    /*
     * One selector call is one transaction, and this is the only place that is true.
     *
     * The command mailbox is shared by every selector: fill the inbox, submit, copy the
     * outbox.  The lock inside the command path covers only submit-and-wait, so without
     * this two selectors could interleave in the gaps and each read a mixture of the two.
     * Both external reviews raised it, twice, and it is the one live defect no diagnostic
     * can see -- the symptom is an occasional wrong answer rather than a failure, and this
     * project has already learned that a wrong answer which looks like data is the most
     * expensive kind.
     *
     * Wrapping super::externalMethod is deliberate: it covers the dispatch, the validation
     * of argument counts, the handler and its output copy, which is exactly the unit that
     * has to be atomic.  Locking inside each of the thirty-four handlers instead would be
     * thirty-four places to remember.
     */
    if (!fProvider)
        return super::externalMethod(selector, arguments, dispatch, target, reference);

    /*
     * Read the quarantine back, here, once per entry.
     *
     * The quarantine holds memory the reset path released on the strength of "the device has
     * forgotten it" (docs/HANG-AUDIT.md sections 6-9).  If the device had not forgotten it,
     * this is where that becomes visible, and the timestamp is the entry line above -- which
     * is what makes the difference between "a block was modified at some point" and "it was
     * modified between these two calls".  Silent while nothing moved, so it costs a memcmp
     * over a few pages per call and nothing in the log.
     *
     * Under the transaction lock, and as its own short transaction rather than by reusing the
     * one taken below (the lock is not recursive).  The reason is that the reset path holds
     * that lock while it adds to, and evicts from, the zone -- so without it a concurrent
     * entry could read a descriptor as it is being replaced, and dereferencing a freed
     * descriptor is a kext fault rather than a wrong reading.  TryLock rather than Lock so
     * that a sweep can never make an entry wait: if another selector is mid-transaction this
     * reading is simply deferred to the next entry, which is the next thing that happens
     * anyway.
     */
    fProvider->userClientMark(selector, Cx3Mlx4::kCx3ClientPhaseSweepIn);
    if (fProvider->transactionTryEnter()) {
        if (selector != (uint32_t)kCx3SelRuntimeIdentity)
            fProvider->freezeInstrumentsSweep("user client entry");
        fProvider->transactionExit();
        fProvider->userClientMark(selector, Cx3Mlx4::kCx3ClientPhaseSweepOut);
    }

    /*
     * The lock policy for this selector (kXactClass above).  An unknown selector number
     * cannot exist here -- the base class rejects anything at or above kCx3SelCount before
     * this runs -- but the table is indexed defensively anyway, because reading one byte
     * past a 34-entry table is a kext fault and a default costs nothing.
     */
    UInt8 klass = (selector < (uint32_t)kCx3SelCount) ? kXactClass[selector] : kXactLocked;

    if (klass == kXactNoLock) {
        /* No current selector uses this class. Do not introduce an unlocked
         * retention read/dispatch when a future policy row changes. */
        return kIOReturnNotPermitted;
    }

    if (klass == kXactTryLock) {
        fProvider->userClientMark(selector, Cx3Mlx4::kCx3ClientPhaseWaitLock);
        if (!fProvider->transactionTryEnter()) {
            /*
             * Refused, not queued.  The caller has to be able to tell this apart from a
             * failure of the thing it asked about: the returned value is kIOReturnBusy, and
             * the log says who is holding the lock only in the sense that it says a
             * transaction is in progress -- the holder is not recorded anywhere.  That is a
             * gap, not a design choice: a "held by selector N since T" field would make this
             * line readable, and it is not here because this port has no way to test it yet.
             */
            LOG("selector %u: refused with kIOReturnBusy -- another selector holds the "
                "transaction lock, so this read would not be a consistent snapshot. Retry, "
                "or read the driver's own log line for the command in progress.",
                (unsigned)selector);
            fProvider->userClientMark(selector, Cx3Mlx4::kCx3ClientPhaseBusy);
            return kIOReturnBusy;
        }
        fProvider->userClientMark(selector, Cx3Mlx4::kCx3ClientPhaseLocked);
        /* Admission may have preceded another transaction exhausting retention.
         * Recheck with the dispatch lock held; the admission check alone races. */
        if (fProvider->selectorStopping() ||
            (selector != (uint32_t)kCx3SelRuntimeIdentity &&
             selector != (uint32_t)kCx3SelAutoNetwork &&
             selector != (uint32_t)kCx3SelNativePort &&
             selector != (uint32_t)kCx3SelNativeConfig && fProvider->retentionExhausted()) ||
            !fProvider->resetAdmissionAllows(selector)) {
            fProvider->transactionExit();
            fProvider->userClientMark(selector, Cx3Mlx4::kCx3ClientPhaseBusy);
            return kIOReturnNotPermitted;
        }
        fProvider->userClientMark(selector, Cx3Mlx4::kCx3ClientPhaseDispatchIn);
        IOReturn r = super::externalMethod(selector, arguments, dispatch, target, reference);
        fProvider->userClientMark(selector, Cx3Mlx4::kCx3ClientPhaseDispatchOut);
        fProvider->transactionExit();
        fProvider->userClientMark(selector, Cx3Mlx4::kCx3ClientPhaseReleased);
        return r;
    }

    fProvider->userClientMark(selector, Cx3Mlx4::kCx3ClientPhaseWaitLock);
    if (!fProvider->transactionEnter()) return kIOReturnNotPermitted;
    fProvider->userClientMark(selector, Cx3Mlx4::kCx3ClientPhaseLocked);
    if (fProvider->selectorStopping() ||
        (selector != (uint32_t)kCx3SelRuntimeIdentity &&
             selector != (uint32_t)kCx3SelAutoNetwork &&
             selector != (uint32_t)kCx3SelNativePort &&
             selector != (uint32_t)kCx3SelNativeConfig && fProvider->retentionExhausted()) ||
        !fProvider->resetAdmissionAllows(selector)) {
        fProvider->transactionExit();
        fProvider->userClientMark(selector, Cx3Mlx4::kCx3ClientPhaseBusy);
        return kIOReturnNotPermitted;
    }
    fProvider->userClientMark(selector, Cx3Mlx4::kCx3ClientPhaseDispatchIn);
    IOReturn r = super::externalMethod(selector, arguments, dispatch, target, reference);
    fProvider->userClientMark(selector, Cx3Mlx4::kCx3ClientPhaseDispatchOut);
    fProvider->transactionExit();
    fProvider->userClientMark(selector, Cx3Mlx4::kCx3ClientPhaseReleased);
    return r;
}

/* ---- trampolines ------------------------------------------------------ */

IOReturn Cx3Mlx4UserClient::sProbe(OSObject *target, void *ref,
                                   IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doProbe(args);
}

IOReturn Cx3Mlx4UserClient::sDma(OSObject *target, void *ref,
                                 IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doDma(args);
}

IOReturn Cx3Mlx4UserClient::sQueryFw(OSObject *target, void *ref,
                                     IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doQueryFw(args);
}

IOReturn Cx3Mlx4UserClient::sResetHca(OSObject *target, void *ref,
                                      IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doResetHca(args);
}

IOReturn Cx3Mlx4UserClient::sPeek(OSObject *target, void *ref,
                                  IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doPeek(args);
}

IOReturn Cx3Mlx4UserClient::sClearOwner(OSObject *target, void *ref,
                                        IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doClearOwner(args);
}

IOReturn Cx3Mlx4UserClient::sPoke(OSObject *target, void *ref,
                                  IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doPoke(args);
}

IOReturn Cx3Mlx4UserClient::sFlr(OSObject *target, void *ref,
                                 IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doFlr(args);
}

IOReturn Cx3Mlx4UserClient::sQueryDevCap(OSObject *target, void *ref,
                                         IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doQueryDevCap(args);
}

IOReturn Cx3Mlx4UserClient::sQueryAdapter(OSObject *target, void *ref,
                                          IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doQueryAdapter(args);
}

IOReturn Cx3Mlx4UserClient::sPcieLink(OSObject *target, void *ref,
                                      IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doPcieLink(args);
}

IOReturn Cx3Mlx4UserClient::sRuntimeIdentity(OSObject *target, void *ref,
                                             IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doRuntimeIdentity(args);
}

IOReturn Cx3Mlx4UserClient::sModuleRead(OSObject *target, void *ref,
                                        IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    return me ? me->doModuleRead(args) : kIOReturnBadArgument;
}

/* ---- implementations -------------------------------------------------- */

IOReturn Cx3Mlx4UserClient::doProbe(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    fProvider->exportProbe(args->scalarOutput);
    return kIOReturnSuccess;
}

IOReturn Cx3Mlx4UserClient::doDma(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    return fProvider->exportDma(args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::doQueryFw(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    bool force = (args->scalarInputCount > 0) && (args->scalarInput[0] != 0);
    return fProvider->exportQueryFw(force, args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::doResetHca(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (args->scalarInputCount < 1 || args->scalarInput[0] != 1) {
        LOG("reset refused: caller did not pass the confirmation scalar");
        return kIOReturnNotPermitted;
    }
    bool bypass = (args->scalarInputCount > kRstInBypassSem) &&
                  (args->scalarInput[kRstInBypassSem] != 0);
    if (bypass) LOG("reset: caller asked to bypass the semaphore gate");
    return fProvider->exportResetHca(bypass, args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::doPeek(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (args->scalarInputCount < 3) return kIOReturnBadArgument;
    return fProvider->exportPeek(args->scalarInput, args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::doClearOwner(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (args->scalarInputCount < 1 || args->scalarInput[0] != 1) {
        LOG("clearOwner refused: caller did not pass the confirmation scalar");
        return kIOReturnNotPermitted;
    }
    return fProvider->exportClearOwner(args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::doPoke(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (args->scalarInputCount < kPokeInCount) return kIOReturnBadArgument;
    if (args->scalarInput[kPokeInConfirm] != 1) {
        LOG("poke refused: caller did not pass the confirmation scalar");
        return kIOReturnNotPermitted;
    }
    return fProvider->exportPoke(args->scalarInput, args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::doFlr(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (args->scalarInputCount < kFlrInCount ||
        args->scalarInput[kFlrInConfirm] != 1) {
        LOG("FLR refused: caller did not pass the confirmation scalar");
        return kIOReturnNotPermitted;
    }
    return fProvider->exportFlr(args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sIcmAlloc(OSObject *target, void *ref,
                                      IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doIcmAlloc(args);
}

IOReturn Cx3Mlx4UserClient::doIcmAlloc(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (args->scalarInputCount < kIcmInCount) return kIOReturnBadArgument;
    return fProvider->exportIcmAlloc((UInt32)args->scalarInput[kIcmInPages],
                                     args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sFwLoad(OSObject *target, void *ref,
                                    IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doFwLoad(args);
}

IOReturn Cx3Mlx4UserClient::doFwLoad(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (args->scalarInputCount < kFwLoadInCount) return kIOReturnBadArgument;
    bool lowmem = (args->scalarInputCount > kFwLoadInLowmem) &&
                  (args->scalarInput[kFwLoadInLowmem] != 0);
    bool force  = (args->scalarInputCount > kFwLoadInForce) &&
                  (args->scalarInput[kFwLoadInForce] != 0);
    bool fill   = (args->scalarInputCount > kFwLoadInFill) &&
                  (args->scalarInput[kFwLoadInFill] != 0);
    return fProvider->exportFwLoad((UInt32)args->scalarInput[kFwLoadInPages],
                                   lowmem, force, fill, args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sFwArea(OSObject *target, void *ref,
                                    IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doFwArea(args);
}

IOReturn Cx3Mlx4UserClient::doFwArea(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (args->scalarInputCount < kFwAreaInCount_) return kIOReturnBadArgument;
    return fProvider->exportFwArea(args->scalarInput, args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sFwOutbox(OSObject *target, void *ref,
                                      IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doFwOutbox(args);
}

IOReturn Cx3Mlx4UserClient::doFwOutbox(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (args->scalarInputCount < kFwOutboxInCount) return kIOReturnBadArgument;
    return fProvider->exportFwOutbox(args->scalarInput, args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sM2dProfile(OSObject *target, void *ref,
                                        IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doM2dProfile(args);
}

IOReturn Cx3Mlx4UserClient::doM2dProfile(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (args->scalarInputCount < kM2dInCount) return kIOReturnBadArgument;
    if (!args->structureOutput ||
        args->structureOutputSize < sizeof(Cx3ProfileResult)) {
        LOG("m2dprofile: structure output missing or too small (%u bytes, need %u)",
            (unsigned)args->structureOutputSize, (unsigned)sizeof(Cx3ProfileResult));
        return kIOReturnBadArgument;
    }
    return fProvider->exportM2dProfile(args->scalarInput,
                                       (Cx3ProfileResult *)args->structureOutput,
                                       args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sM2dBringUp(OSObject *target, void *ref,
                                        IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doM2dBringUp(args);
}

IOReturn Cx3Mlx4UserClient::doM2dBringUp(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    return fProvider->exportM2dBringUp(args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sM3EqUp(OSObject *target, void *ref,
                                    IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doM3EqUp(args);
}

IOReturn Cx3Mlx4UserClient::doM3EqUp(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (args->scalarInputCount < kEqUpInCount) return kIOReturnBadArgument;
    return fProvider->exportM3EqUp(args->scalarInput, args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sM3EqPoll(OSObject *target, void *ref,
                                      IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doM3EqPoll(args);
}

IOReturn Cx3Mlx4UserClient::doM3EqPoll(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (args->scalarInputCount < kEqPollInCount) return kIOReturnBadArgument;
    return fProvider->exportM3EqPoll(args->scalarInput, args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sM3EqRing(OSObject *target, void *ref,
                                      IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doM3EqRing(args);
}

IOReturn Cx3Mlx4UserClient::doM3EqRing(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (args->scalarInputCount < kEqRingInCount) return kIOReturnBadArgument;
    return fProvider->exportM3EqRing(args->scalarInput, args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sM3EqInfo(OSObject *target, void *ref,
                                      IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doM3EqInfo(args);
}

IOReturn Cx3Mlx4UserClient::doM3EqInfo(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (!args->structureOutput ||
        args->structureOutputSize < sizeof(Cx3EqInfoResult)) {
        LOG("m3eqinfo: structure output missing or too small (%u bytes, need %u)",
            (unsigned)args->structureOutputSize, (unsigned)sizeof(Cx3EqInfoResult));
        return kIOReturnBadArgument;
    }
    return fProvider->exportM3EqInfo(args->scalarOutput,
                                     (Cx3EqInfoResult *)args->structureOutput);
}

IOReturn Cx3Mlx4UserClient::sM3IrqUp(OSObject *target, void *ref,
                                     IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doM3IrqUp(args);
}

IOReturn Cx3Mlx4UserClient::doM3IrqUp(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (args->scalarInputCount < kIrqUpInCount) return kIOReturnBadArgument;
    return fProvider->exportM3IrqUp(args->scalarInput, args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sM3IrqTest(OSObject *target, void *ref,
                                       IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doM3IrqTest(args);
}

IOReturn Cx3Mlx4UserClient::doM3IrqTest(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (args->scalarInputCount < kIrqTestInCount) return kIOReturnBadArgument;
    return fProvider->exportM3IrqTest(args->scalarInput, args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sM3IrqStatus(OSObject *target, void *ref,
                                         IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doM3IrqStatus(args);
}

IOReturn Cx3Mlx4UserClient::doM3IrqStatus(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    return fProvider->exportM3IrqStatus(args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sM3IrqDown(OSObject *target, void *ref,
                                       IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doM3IrqDown(args);
}

IOReturn Cx3Mlx4UserClient::doM3IrqDown(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    return fProvider->exportM3IrqDown(args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sM3ClrInt(OSObject *target, void *ref,
                                       IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doM3ClrInt(args);
}

IOReturn Cx3Mlx4UserClient::sM3QueryPort(OSObject *target, void *ref,
                                         IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doM3QueryPort(args);
}

IOReturn Cx3Mlx4UserClient::sM3PortUp(OSObject *target, void *ref,
                                      IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doM3PortUp(args);
}

IOReturn Cx3Mlx4UserClient::sM3AbRing(OSObject *target, void *ref,
                                      IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doM3AbRing(args);
}

IOReturn Cx3Mlx4UserClient::doM3ClrInt(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    return fProvider->exportM3ClrInt(args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sTxProbe(OSObject *target, void *ref,
                                     IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doTxProbe(args);
}

IOReturn Cx3Mlx4UserClient::doTxProbe(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    return fProvider->exportTxProbe(args->scalarInput, args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sNetUp(OSObject *target, void *ref,
                                   IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doNetUp(args);
}

IOReturn Cx3Mlx4UserClient::doNetUp(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    return fProvider->exportNetUp(args->scalarInput, args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sNetUpStop(OSObject *target, void *ref,
                                       IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doNetUpStop(args);
}

IOReturn Cx3Mlx4UserClient::doNetUpStop(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    return fProvider->exportNetUpStop(args->scalarInput, args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::doM3QueryPort(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (!args->structureOutput ||
        args->structureOutputSize < sizeof(Cx3QueryPortResult)) {
        LOG("queryport: structure output missing or too small (%u bytes, need %u)",
            (unsigned)args->structureOutputSize, (unsigned)sizeof(Cx3QueryPortResult));
        return kIOReturnBadArgument;
    }
    if (args->scalarInputCount <= kQueryPortInPort) return kIOReturnBadArgument;
    return fProvider->exportM3QueryPort(args->scalarInput,
                                        (Cx3QueryPortResult *)args->structureOutput,
                                        args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::doM3PortUp(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (args->scalarInputCount <= kPortUpInPort) return kIOReturnBadArgument;
    return fProvider->exportM3PortUp(args->scalarInput, args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sHcr(OSObject *target, void *ref,
                                  IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doHcr(args);
}

IOReturn Cx3Mlx4UserClient::doHcr(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (args->scalarInputCount < kHcrInCount) return kIOReturnBadArgument;
    if (args->structureOutputSize < CX3_HCR_MAILBOX_SIZE) {
        LOG("hcr: the caller must provide a %u-byte outbox (got %u)",
            (unsigned)CX3_HCR_MAILBOX_SIZE, (unsigned)args->structureOutputSize);
        return kIOReturnBadArgument;
    }
    return fProvider->exportHcr(args->scalarInput,
                                (const UInt8 *)args->structureInput,
                                (UInt8 *)args->structureOutput,
                                args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sSurvey(OSObject *target, void *ref,
                                    IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doSurvey(args);
}

IOReturn Cx3Mlx4UserClient::sFault(OSObject *target, void *ref,
                                   IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doFault(args);
}

IOReturn Cx3Mlx4UserClient::doFault(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    /*
     * Two scalars are required rather than optional: the second one is a confirmation for
     * mode 2 and the hold time for mode 3, and both must be deliberate.  The base class
     * already validated the count against the dispatch table (kFaultInCount); the check
     * here is for the path where a caller reaches doFault() some other way.
     */
    if (args->scalarInputCount < kFaultInCount) return kIOReturnBadArgument;
    return fProvider->exportFault(args->scalarInput, args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sNetStat(OSObject *target, void *ref,
                                    IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doNetStat(args);
}

IOReturn Cx3Mlx4UserClient::sNetLink(OSObject *target, void *ref,
                                    IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doNetLink(args);
}

IOReturn Cx3Mlx4UserClient::sNetOff(OSObject *target, void *ref,
                                   IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doNetOff(args);
}

IOReturn Cx3Mlx4UserClient::sNetArm(OSObject *target, void *ref,
                                   IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doNetArm(args);
}

IOReturn Cx3Mlx4UserClient::sNetFilt(OSObject *target, void *ref,
                                    IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doNetFilt(args);
}

IOReturn Cx3Mlx4UserClient::sNetPort(OSObject *target, void *ref,
                                    IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doNetPort(args);
}

IOReturn Cx3Mlx4UserClient::doNetPort(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    return fProvider->exportNetPort(args->scalarInput, args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::doNetFilt(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    return fProvider->exportNetFilt(args->scalarInput, args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::doNetArm(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    return fProvider->exportNetArm(args->scalarInput, args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::doNetOff(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    return fProvider->exportNetOff(args->scalarInput, args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::doNetLink(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    return fProvider->exportNetLink(args->scalarInput, args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::doNetStat(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (!args->structureOutput || args->structureOutputSize < sizeof(Cx3NetStatResult)) {
        LOG("netstat: structure output missing or too small (%u bytes, need %u)",
            (unsigned)args->structureOutputSize, (unsigned)sizeof(Cx3NetStatResult));
        return kIOReturnBadArgument;
    }
    return fProvider->exportNetStat((Cx3NetStatResult *)args->structureOutput,
                                    args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::doSurvey(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (!args->structureOutput ||
        args->structureOutputSize < sizeof(Cx3SurveyResult)) {
        LOG("survey: structure output missing or too small (%u bytes, need %u)",
            (unsigned)args->structureOutputSize, (unsigned)sizeof(Cx3SurveyResult));
        return kIOReturnBadArgument;
    }
    return fProvider->exportSurvey((Cx3SurveyResult *)args->structureOutput,
                                   args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sR4Rq(OSObject *target, void *ref,
                                   IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doR4Rq(args);
}

IOReturn Cx3Mlx4UserClient::doR4Rq(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (!args->structureOutput ||
        args->structureOutputSize < sizeof(Cx3R4RqResult)) {
        LOG("r4rq: structure output missing or too small (%u bytes, need %u)",
            (unsigned)args->structureOutputSize, (unsigned)sizeof(Cx3R4RqResult));
        return kIOReturnBadArgument;
    }
    if (args->scalarInputCount < kR4RqInCount) return kIOReturnBadArgument;
    return fProvider->exportR4Rq(args->scalarInput,
                                 (Cx3R4RqResult *)args->structureOutput,
                                 args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sR4Cq(OSObject *target, void *ref,
                                   IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doR4Cq(args);
}

IOReturn Cx3Mlx4UserClient::doR4Cq(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (!args->structureOutput ||
        args->structureOutputSize < sizeof(Cx3R4CqResult)) {
        LOG("r4cq: structure output missing or too small (%u bytes, need %u)",
            (unsigned)args->structureOutputSize, (unsigned)sizeof(Cx3R4CqResult));
        return kIOReturnBadArgument;
    }
    if (args->scalarInputCount < kR4CqInCount) return kIOReturnBadArgument;
    return fProvider->exportR4Cq(args->scalarInput,
                                 (Cx3R4CqResult *)args->structureOutput,
                                 args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sR4Mr(OSObject *target, void *ref,
                                   IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doR4Mr(args);
}

IOReturn Cx3Mlx4UserClient::doR4Mr(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (!args->structureOutput ||
        args->structureOutputSize < sizeof(Cx3R4MrResult)) {
        LOG("r4mr: structure output missing or too small (%u bytes, need %u)",
            (unsigned)args->structureOutputSize, (unsigned)sizeof(Cx3R4MrResult));
        return kIOReturnBadArgument;
    }
    return fProvider->exportR4Mr(args->scalarInput,
                                 (Cx3R4MrResult *)args->structureOutput,
                                 args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sR4Qp(OSObject *target, void *ref,
                                   IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doR4Qp(args);
}

IOReturn Cx3Mlx4UserClient::doR4Qp(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (args->scalarInputCount < kR4QpInCount) return kIOReturnBadArgument;
    Cx3R4Output output(args, sizeof(Cx3R4QpResult));
    if (output.status() != kIOReturnSuccess) return output.status();
    Cx3R4QpResult *res = (Cx3R4QpResult *)output.data();
    IOReturn rc = fProvider->exportR4Qp(args->scalarInput, res, args->scalarOutput);
    res->abiVersion = CX3_R4QP_ABI;
    res->abiBytes = (uint32_t)sizeof(Cx3R4QpResult);
    return output.publish(rc);
}

IOReturn Cx3Mlx4UserClient::sR4Ingress(OSObject *target, void *ref,
                                        IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doR4Ingress(args);
}

IOReturn Cx3Mlx4UserClient::doR4Ingress(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    /*
     * No scalar inputs at all: both halves read what they need from the device's own state
     * (QUERY_PORT's def_mac and stage 4's QPN).  The count is still checked, because the
     * dispatch table's number is a contract and a mismatch here is a tool built against a
     * different ABI -- which produces a zeroed answer, and zero is this project's "ok".
     */
    if (args->scalarInputCount < kR4IngressInCount) return kIOReturnBadArgument;
    Cx3R4Output output(args, sizeof(Cx3R4IngressResult));
    if (output.status() != kIOReturnSuccess) return output.status();
    Cx3R4IngressResult *res = (Cx3R4IngressResult *)output.data();
    IOReturn rc = fProvider->exportR4Ingress(res, args->scalarOutput);
    /* export() zeroes the structure, so stamp after it, including on device-level failure. */
    res->abiVersion = CX3_R4INGRESS_ABI;
    res->abiBytes = (uint32_t)sizeof(Cx3R4IngressResult);
    return output.publish(rc);
}

IOReturn Cx3Mlx4UserClient::sR4Tx(OSObject *target, void *ref,
                                  IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doR4Tx(args);
}

IOReturn Cx3Mlx4UserClient::doR4Tx(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (!args->structureOutput ||
        args->structureOutputSize < sizeof(Cx3R4TxResult)) {
        LOG("r4tx: structure output missing or too small (%u bytes, need %u)",
            (unsigned)args->structureOutputSize, (unsigned)sizeof(Cx3R4TxResult));
        return kIOReturnBadArgument;
    }
    if (args->scalarInputCount < kR4TxInCount) return kIOReturnBadArgument;
    return fProvider->exportR4Tx(args->scalarInput, (Cx3R4TxResult *)args->structureOutput,
                                 args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sR4Raw(OSObject *target, void *ref,
                                   IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doR4Raw(args);
}

/*
 * The stage-9 entry point.  The two scalar inputs are the post count and the receive budget; a
 * caller that passes none gets `0, 0`, which the selector reads as "the defaults" -- one frame
 * first and then the wrap, and a few seconds of waiting for frames.  That is deliberate: the
 * zeroed-argument call and the default call are the same call, so the verifier cannot invoke
 * something the tool cannot.
 */
IOReturn Cx3Mlx4UserClient::doR4Raw(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (!args->structureOutput ||
        args->structureOutputSize < sizeof(Cx3R4RawResult)) {
        LOG("r4raw: structure output missing or too small (%u bytes, need %u)",
            (unsigned)args->structureOutputSize, (unsigned)sizeof(Cx3R4RawResult));
        return kIOReturnBadArgument;
    }
    if (args->scalarInputCount < kR4RawInCount) return kIOReturnBadArgument;
    return fProvider->exportR4Raw(args->scalarInput, (Cx3R4RawResult *)args->structureOutput,
                                  args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::sM3QueryHca(OSObject *target, void *ref,
                                         IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    if (!me) return kIOReturnBadArgument;
    return me->doM3QueryHca(args);
}

IOReturn Cx3Mlx4UserClient::doM3QueryHca(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (!args->structureOutput ||
        args->structureOutputSize < sizeof(Cx3QueryHcaResult)) {
        LOG("queryhca: structure output missing or too small (%u bytes, need %u)",
            (unsigned)args->structureOutputSize, (unsigned)sizeof(Cx3QueryHcaResult));
        return kIOReturnBadArgument;
    }
    return fProvider->exportM3QueryHca((Cx3QueryHcaResult *)args->structureOutput,
                                      args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::doM3AbRing(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (!args->structureOutput ||
        args->structureOutputSize < sizeof(Cx3AbRingResult)) {
        LOG("abring: structure output missing or too small (%u bytes, need %u)",
            (unsigned)args->structureOutputSize, (unsigned)sizeof(Cx3AbRingResult));
        return kIOReturnBadArgument;
    }
    return fProvider->exportM3AbRing(args->scalarInput,
                                     (Cx3AbRingResult *)args->structureOutput,
                                     args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::doQueryDevCap(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (!args->structureOutput ||
        args->structureOutputSize < sizeof(Cx3DevCapResult)) {
        LOG("querydevcap: structure output missing or too small (%u bytes, need %u)",
            (unsigned)args->structureOutputSize, (unsigned)sizeof(Cx3DevCapResult));
        return kIOReturnBadArgument;
    }
    bool force      = (args->scalarInputCount > kDevCapInForce) &&
                      (args->scalarInput[kDevCapInForce] != 0);
    bool ignoreBusy = (args->scalarInputCount > kDevCapInIgnoreBusy) &&
                      (args->scalarInput[kDevCapInIgnoreBusy] != 0);
    return fProvider->exportQueryDevCap(force, ignoreBusy,
                                        (Cx3DevCapResult *)args->structureOutput,
                                        args->scalarOutput);
}

IOReturn Cx3Mlx4UserClient::doQueryAdapter(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (args->scalarInputCount != 0 || args->scalarOutputCount != 0 ||
        args->structureInput || args->structureInputSize != 0) {
        LOG("queryadapter: unexpected input or scalar arguments");
        return kIOReturnBadArgument;
    }
    if (!args->structureOutput ||
        args->structureOutputSize != sizeof(Cx3AdapterResult)) {
        LOG("queryadapter: structure output missing or wrong size (%u bytes, need %u)",
            (unsigned)args->structureOutputSize, (unsigned)sizeof(Cx3AdapterResult));
        return kIOReturnBadArgument;
    }
    return fProvider->exportQueryAdapter((Cx3AdapterResult *)args->structureOutput);
}

IOReturn Cx3Mlx4UserClient::doModuleRead(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (args->scalarInputCount != 3 || args->scalarOutputCount != 0 ||
        !args->scalarInput || args->structureInput || args->structureInputSize ||
        !args->structureOutput || args->structureOutputSize != sizeof(Cx3ModuleResult))
        return kIOReturnBadArgument;
    if (!cx3ModuleRangeValid(args->scalarInput[0], args->scalarInput[1], args->scalarInput[2]))
        return kIOReturnBadArgument;
    return fProvider->exportModuleRead((UInt32)args->scalarInput[0],
        (UInt32)args->scalarInput[1], (UInt32)args->scalarInput[2],
        (Cx3ModuleResult *)args->structureOutput);
}

IOReturn Cx3Mlx4UserClient::doPcieLink(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (args->scalarInputCount != 0 || args->scalarOutputCount != 0 ||
        args->structureInput || args->structureInputSize != 0) {
        LOG("pcielink: unexpected input or scalar arguments");
        return kIOReturnBadArgument;
    }
    if (!args->structureOutput || args->structureOutputSize != sizeof(Cx3PcieLinkResult)) {
        LOG("pcielink: structure output missing or wrong size (%u bytes, need %u)",
            (unsigned)args->structureOutputSize, (unsigned)sizeof(Cx3PcieLinkResult));
        return kIOReturnBadArgument;
    }
    return fProvider->exportPcieLink((Cx3PcieLinkResult *)args->structureOutput);
}

IOReturn Cx3Mlx4UserClient::doRuntimeIdentity(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (!args || args->scalarInputCount != 0 || args->scalarOutputCount != 0 ||
        args->structureInput || args->structureInputSize != 0) {
        LOG("runtime identity: unexpected input or scalar arguments");
        return kIOReturnBadArgument;
    }
    if (!args->structureOutput ||
        args->structureOutputSize != sizeof(Cx3RuntimeIdentityResult)) {
        LOG("runtime identity: structure output missing or wrong size (%u bytes, need %u)",
            (unsigned)args->structureOutputSize, (unsigned)sizeof(Cx3RuntimeIdentityResult));
        return kIOReturnBadArgument;
    }
    return fProvider->exportRuntimeIdentity(
        (Cx3RuntimeIdentityResult *)args->structureOutput);
}

IOReturn Cx3Mlx4UserClient::sAutoNetwork(OSObject *target, void *ref,
                                        IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    return me ? me->doAutoNetwork(args) : kIOReturnBadArgument;
}

IOReturn Cx3Mlx4UserClient::doAutoNetwork(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (!args || args->scalarInputCount || args->scalarOutputCount ||
        !args->structureInput || args->structureInputSize != sizeof(Cx3AutoNetworkRequest) ||
        !args->structureOutput || args->structureOutputSize != sizeof(Cx3AutoNetworkResult))
        return kIOReturnBadArgument;
    return fProvider->exportAutoNetwork((const Cx3AutoNetworkRequest *)args->structureInput,
                                       (Cx3AutoNetworkResult *)args->structureOutput);
}

IOReturn Cx3Mlx4UserClient::sPortInfo(OSObject *target, void *ref,
                                    IOExternalMethodArguments *args)
{
    (void)ref;
    Cx3Mlx4UserClient *me = OSDynamicCast(Cx3Mlx4UserClient, target);
    return me ? me->doPortInfo(args) : kIOReturnBadArgument;
}

IOReturn Cx3Mlx4UserClient::doPortInfo(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (!args || args->scalarInputCount != 2 || args->scalarOutputCount ||
        !args->scalarInput || args->structureInput || args->structureInputSize ||
        !args->structureOutput || args->structureOutputSize != sizeof(Cx3PortInfoResult) ||
        args->scalarInput[0] < 1 || args->scalarInput[0] > 2 || args->scalarInput[1] > 1)
        return kIOReturnBadArgument;
    return fProvider->exportPortInfo((UInt32)args->scalarInput[0], args->scalarInput[1] != 0,
                                    (Cx3PortInfoResult *)args->structureOutput);
}

IOReturn Cx3Mlx4UserClient::sPortQueue(OSObject *target, void *ref, IOExternalMethodArguments *args)
{
    (void)ref; Cx3Mlx4UserClient *me=OSDynamicCast(Cx3Mlx4UserClient,target);
    return me ? me->doPortQueue(args) : kIOReturnBadArgument;
}
IOReturn Cx3Mlx4UserClient::doPortQueue(IOExternalMethodArguments *args)
{
    if (!fProvider) return kIOReturnNotAttached;
    if (!args || args->scalarInputCount!=3 || args->scalarOutputCount || !args->scalarInput ||
        args->structureInput || args->structureInputSize || !args->structureOutput ||
        args->structureOutputSize!=sizeof(Cx3PortQueueResult) || args->scalarInput[0]<1 ||
        args->scalarInput[0]>2 || args->scalarInput[1]>CX3_PORT_QUEUE_DESTROY || !args->scalarInput[2])
        return kIOReturnBadArgument;
    return fProvider->exportPortQueue((UInt32)args->scalarInput[0],(UInt32)args->scalarInput[1],
        args->scalarInput[2],(Cx3PortQueueResult*)args->structureOutput);
}

IOReturn Cx3Mlx4UserClient::sNativePort(OSObject*t,void*,IOExternalMethodArguments*a)
{
 auto*me=OSDynamicCast(Cx3Mlx4UserClient,t);return me?me->doNativePort(a):kIOReturnBadArgument;
}
IOReturn Cx3Mlx4UserClient::doNativePort(IOExternalMethodArguments*a)
{
 if(!fProvider)return kIOReturnNotAttached;
 if(!a||a->scalarInputCount!=3||a->scalarOutputCount||!a->scalarInput||a->structureInput||a->structureInputSize||
    !a->structureOutput||a->structureOutputSize!=sizeof(Cx3NativePortResult)||a->scalarInput[0]<1||a->scalarInput[0]>2||
    a->scalarInput[1]>CX3_NATIVE_DETACH||!a->scalarInput[2])return kIOReturnBadArgument;
 return fProvider->exportNativePort((UInt32)a->scalarInput[0],(UInt32)a->scalarInput[1],a->scalarInput[2],(Cx3NativePortResult*)a->structureOutput);
}

IOReturn Cx3Mlx4UserClient::sNativeConfig(OSObject*t,void*,IOExternalMethodArguments*a)
{
 auto*me=OSDynamicCast(Cx3Mlx4UserClient,t);return me?me->doNativeConfig(a):kIOReturnBadArgument;
}
IOReturn Cx3Mlx4UserClient::doNativeConfig(IOExternalMethodArguments*a)
{
 if(!fProvider)return kIOReturnNotAttached;
 if(!a||a->scalarInputCount!=4||a->scalarOutputCount||!a->scalarInput||a->structureInput||a->structureInputSize||
    !a->structureOutput||a->structureOutputSize!=sizeof(Cx3NativeConfigResult)||a->scalarInput[0]<1||a->scalarInput[0]>2||
    a->scalarInput[1]>CX3_NCFG_TXCSUM||!a->scalarInput[2]||a->scalarInput[3]>0xffffffffu)return kIOReturnBadArgument;
 return fProvider->exportNativeConfig((UInt32)a->scalarInput[0],(UInt32)a->scalarInput[1],a->scalarInput[2],
  (UInt32)a->scalarInput[3],(Cx3NativeConfigResult*)a->structureOutput);
}
