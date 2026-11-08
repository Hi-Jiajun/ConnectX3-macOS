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
 * Cx3Mlx4 -- Mellanox ConnectX-3 Pro (15b3:1007) kext, milestone M1.
 *
 * start() does only: enable the PCI function, map BAR0/BAR2, read the
 * documented host registers, register the service.  It writes nothing to the
 * device and allocates no DMA, because a fault here is a boot fault.
 *
 * Everything that writes to the device is reached from user space through
 * Cx3Mlx4UserClient (see cx3_abi.h).  This split exists because the 0.3.0 build
 * ran the command path from start() and hung the machine before it could even
 * flush its log buffer.
 */
#ifndef CX3_MLX4_H
#define CX3_MLX4_H

#include <IOKit/IOService.h>
#include <kern/thread_call.h>
#include "Mlx4Hw.h"
#include "cx3_abi.h"
#include "Cx3PortState.h"
#include "Cx3PortQueues.h"

class Cx3EthernetPort;
class IOWorkLoop;
class IOInterruptEventSource;

#include <IOKit/network/IOEthernetController.h>
/* IOEthernetController.h declares the interface's CONTROLLER side; the interface class itself
 * (IOEthernetInterface, which stage 10a instantiates) lives in its own header and is not pulled
 * in by it.  Without this the member below is an unknown type name. */
#include <IOKit/network/IOEthernetInterface.h>

/*
 * The base class is IOEthernetController since stage 10a (docs/R4-STAGE10-PLAN.md section 10).
 * It derives from IOService, so everything this driver already does -- the PCI provider, the
 * user-client selectors, the whole bring-up -- is unchanged; what it adds is the interface end,
 * which is attached on demand by kCx3SelNetUp rather than during start().
 */
class Cx3Mlx4 : public IOEthernetController {
    friend class Cx3EthernetPort;
    OSDeclareDefaultStructors(Cx3Mlx4)
    typedef IOEthernetController super;
    CX3_STATIC_ASSERT(__is_same(super, IOEthernetController),
                      "super must be the direct base: skipping networking init/start leaves attach without a command gate");

public:
    /*
     * Off-machine-readable position of a user-client selector call.  The registry is the only
     * channel that remains readable while the selector transaction lock is held, and the
     * previous death produced neither "enter selector 46" nor any netup marker.  These phases
     * distinguish a call that never entered the driver, one waiting for the transaction lock,
     * one inside dispatch, and one that returned.
     */
    enum {
        kCx3ClientPhaseIdle = 0,
        kCx3ClientPhaseEntered = 1,
        kCx3ClientPhaseSweepIn = 2,
        kCx3ClientPhaseSweepOut = 3,
        kCx3ClientPhaseWaitLock = 4,
        kCx3ClientPhaseLocked = 5,
        kCx3ClientPhaseDispatchIn = 6,
        kCx3ClientPhaseDispatchOut = 7,
        kCx3ClientPhaseReleased = 8,
        kCx3ClientPhaseBusy = 9
    };
    /*
     * Progress through IOServiceOpen() -> newUserClient().
     *
     * The selector phase above starts inside externalMethod().  A freeze before that point
     * therefore left the previous selector's phase visible and could not distinguish the
     * IOKit open trap, client allocation, initWithTask(), attach(), or start().  These
     * values are written to cx3OpenPhase and sampled by the same off-machine watcher.
     */
    enum {
        kCx3OpenPhaseIdle = 0,
        kCx3OpenPhaseEntered = 1,
        kCx3OpenPhaseAllocated = 2,
        kCx3OpenPhaseInitialized = 3,
        kCx3OpenPhaseAttached = 4,
        kCx3OpenPhaseStarted = 5,
        kCx3OpenPhaseReturned = 6,
        kCx3OpenPhaseClosed = 7,
        kCx3OpenPhaseNoMemory = 10,
        kCx3OpenPhaseInitFailed = 11,
        kCx3OpenPhaseAttachFailed = 12,
        kCx3OpenPhaseStartFailed = 13
    };
    /*
     * The thread_call worker has a terminal state, not just a "done" bit.
     *
     * A timeout means attachInterface's state is unknown.  It is therefore
     * poisoned until stop(): a later netup must not launch a second attach
     * into the same controller while the first worker might still be alive.
     */
    enum {
        kCx3AttachWorkerIdle = 0,
        kCx3AttachWorkerQueued = 1,
        kCx3AttachWorkerRunning = 2,
        kCx3AttachWorkerReturned = 3,
        kCx3AttachWorkerFailed = 4,
        kCx3AttachWorkerTimedOut = 5,
        kCx3AttachWorkerPoisoned = 6,
        kCx3AttachWorkerLateReturn = 7,
        /*
         * 8: runAction entered its callback with the controller's work-loop gate
         * held. The callback still executes on the calling thread_call thread.
         * A missing later sample cannot by itself locate the hang.
         */
        kCx3AttachWorkerOnLoop = 8,
        /*
         * 9: the runAction self-test callback ran and published its marker without
         * attaching. This proves gate entry, not dispatch to the work-loop thread.
         */
        kCx3AttachWorkerLoopNoop = 9
    };
    /*
     * How many context tables M2d maps.  The reference maps 14: four cMPT
     * sub-tables plus EQC, MTT, dMPT, QPC, AUXC, ALTC, RDMARC, CQC, SRQC and MCG
     * (mlx4_main.c:1563-1718).  The array is sized with a small margin so that
     * the count is checked rather than assumed to fit.
     */
    /*
     * The reference maps exactly 14 context tables (mlx4_main.c:1563-1718): four
     * cMPT sub-tables plus EQC, MTT, dMPT, QPC, AUXC, ALTC, RDMARC, CQC, SRQC and
     * MCG.  The array is sized with a margin and the count is checked, and this
     * assertion makes sure a future edit cannot shrink the array below what the
     * list needs -- which is what a plain "18" here would allow.
     */
    enum { kRefIcmTables = 14, kMaxIcmTables = 18 };
    /*
     * How many consecutive interrupt invocations that consume no EQE are tolerated
     * before the source is disabled.  An uncleared level interrupt asserts forever
     * and this is what stops it from taking the machine down; it is deliberately
     * well above the few no-op invocations a genuinely shared line can produce.
     */
    enum { kIrqZeroStreakLimit = 64 };
    /*
     * A table's reserved prefix is mapped one 256 KiB chunk at a time.  The bound
     * that matters is Mlx4IcmTable::icm[]'s own size, which the code reads with
     * sizeof so the two cannot disagree; this constant only exists for the table
     * declared below to size against.
     */
    enum { kMaxIcmChunksPerTable = 8 };

    bool       init(OSDictionary *dictionary = 0) override;
    void       free() override;
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool       start(IOService *provider) override;
    void       stop(IOService *provider) override;
    IOReturn   newUserClient(task_t owningTask, void *securityID, UInt32 type,
                             OSDictionary *properties,
                             IOUserClient **handler) override;

    /* Called from Cx3Mlx4UserClient.  Never called during boot. */
    void     exportProbe(UInt64 *out);
    IOReturn exportDma(UInt64 *out);
    IOReturn exportQueryFw(bool force, UInt64 *out);
    IOReturn exportResetHca(bool bypassSem, UInt64 *out);
    IOReturn exportPeek(const UInt64 *in, UInt64 *out);
    IOReturn exportClearOwner(UInt64 *out);
    IOReturn exportPoke(const UInt64 *in, UInt64 *out);
    IOReturn exportFlr(UInt64 *out);
    IOReturn exportQueryDevCap(bool force, bool ignoreBusy, Cx3DevCapResult *res,
                               UInt64 *scalars);
    IOReturn exportQueryAdapter(Cx3AdapterResult *res);
    IOReturn exportModuleRead(UInt32 port, UInt32 offset, UInt32 length, Cx3ModuleResult *res);
    IOReturn exportPcieLink(Cx3PcieLinkResult *res);
    IOReturn exportRuntimeIdentity(Cx3RuntimeIdentityResult *res);
    IOReturn exportNativePort(UInt32 port,UInt32 action,UInt64 generation,Cx3NativePortResult*);
    IOReturn exportNativeConfig(UInt32,UInt32,UInt64,UInt32,Cx3NativeConfigResult*);
    bool stopNativePorts();
    bool hasNativePorts() const { return fNativePorts[0] || fNativePorts[1]; }
    bool preparePortQueueNumbers();
    IOReturn exportPortQueue(UInt32 port, UInt32 action, UInt64 expectedGeneration,
                             Cx3PortQueueResult *res);
    IOReturn exportPortInfo(UInt32 port, bool refresh, Cx3PortInfoResult *res);
    bool recordPortQuery(UInt32 port, const Cx3QueryPortResult &query);
    IOReturn exportAutoNetwork(const Cx3AutoNetworkRequest *req, Cx3AutoNetworkResult *res);
    static IOReturn autoNetworkOnWorkLoop(OSObject *owner, void *arg0,
                                         void *arg1, void *arg2, void *arg3);
    IOReturn exportIcmAlloc(UInt32 pages, UInt64 *out);
    IOReturn exportFwLoad(UInt32 pages, bool lowmem, bool force, bool fill, UInt64 *out);
    IOReturn exportFwArea(const UInt64 *in, UInt64 *out);
    IOReturn exportFwOutbox(const UInt64 *in, UInt64 *out);
    IOReturn exportM2dProfile(const UInt64 *in, Cx3ProfileResult *res, UInt64 *scalars);
    IOReturn exportM2dBringUp(UInt64 *out);
    IOReturn exportM3EqUp(const UInt64 *in, UInt64 *out);
    /* Look a table up by name in the list the M2d bring-up built. */
    Mlx4IcmTable *findTable(const char *name);
    /* Clear the EQC entry for an EQN before SW2HW_EQ, so that an all-zero
     * read-back afterwards unambiguously means "the HCA did not store it". */
    void zeroEqcEntry(UInt32 eqn);
    IOReturn exportM3EqPoll(const UInt64 *in, UInt64 *out);
    /* Can the HCA write into an EQ ring page?  Same instrument as fwoutbox, aimed
     * at the ring -- the discriminator between "the HCA is not using this memory"
     * and "the HCA cannot reach this memory". */
    IOReturn exportM3EqRing(const UInt64 *in, UInt64 *out);
    /* The two read-only checks that come next: what the HCA would follow for this
     * EQ (its MTT entries) and what it thinks the EQ is (QUERY_EQ). */
    IOReturn exportM3EqInfo(UInt64 *out, Cx3EqInfoResult *res);

    /*
     * ------------------- M3, restart 2: interrupts -------------------------
     *
     * The reviews' first item was that a selector and an interrupt callback must
     * not each be moving the consumer index, so everything that touches the queue
     * now goes through fEqLock and the consumption itself is one function shared by
     * the poll selector and the interrupt action.
     *
     * The lifecycle is explicit and selector-driven, which is how the "all device
     * writes come from a selector" rule is kept: it becomes "a session a selector
     * opened may have its callbacks write the consumer index and re-arm".  Nothing
     * about interrupts happens during start(), so the boot path stays read-only and
     * the build check still enforces exactly that.
     */
    IOReturn exportM3IrqUp(const UInt64 *in, UInt64 *out);
    /* Register the token to wait for and post the event-mode command, then return
     * without waiting: the completion is the interrupt's job now. */
    IOReturn exportM3IrqTest(const UInt64 *in, UInt64 *out);
    IOReturn exportM3IrqStatus(UInt64 *out);
    IOReturn exportM3IrqDown(UInt64 *out);
    IOReturn exportM3ClrInt(UInt64 *out);
    /* R3: the port.  queryport is read-only; m3portup writes port configuration and
     * gates itself on the capability query before it does. */
    IOReturn exportM3QueryPort(const UInt64 *in, Cx3QueryPortResult *res, UInt64 *out);
    /* R4 entrance condition: which steering mode did INIT_HCA actually set? */
    IOReturn exportM3QueryHca(Cx3QueryHcaResult *res, UInt64 *out);
    /* R4 stage 1: a protection domain and the physical memory region. */
    IOReturn exportR4Mr(const UInt64 *in, Cx3R4MrResult *res, UInt64 *out);
    /* R4 stage 2: one completion queue. */
    IOReturn exportR4Cq(const UInt64 *in, Cx3R4CqResult *res, UInt64 *out, UInt32 port = 1);
    /* R4 stage 3: the receive ring's buffers and WQEs (memory only). */
    IOReturn exportR4Rq(const UInt64 *in, Cx3R4RqResult *res, UInt64 *out, UInt32 port = 1);
    /* R4 stage 4: the RX QP three steps to RTS, plus the producer publish. */
    IOReturn exportR4Qp(const UInt64 *in, Cx3R4QpResult *res, UInt64 *out, UInt32 port = 1);
    /*
     * R4 stage 5: the port's MAC table and the single-ring RSS indirection QP.
     *
     * One selector for two things, because they share one verdict: the MAC makes the firmware
     * recognise this card's address and the indirection QP gives hashed traffic somewhere to
     * go, and either one alone steers nothing.  Neither produces a completion, so both are
     * read back rather than inferred from traffic.
     */
    IOReturn exportR4Ingress(Cx3R4IngressResult *res, UInt64 *out, UInt32 port = 1);
    /*
     * R4 stage 6: the transmit side's RESOURCES -- a TX completion queue, a send QP walked to
     * RTS, and the send queue's memory.  No frame is posted and no doorbell is rung: this is
     * "the objects exist and their numbers are where the plan needs them", not "the card can
     * send".
     *
     * The constraint that makes this stage more than plumbing is the number space.  A0 maps MAC
     * table index i to base_qpn + i, and the plan's non-promiscuous route (docs/M2B-NOTES.md
     * section 41.5, option R) puts that base at num_qps - (1 << log_num_macs) so the 128
     * MAC-derived QPNs sit on the top of the space instead of on our queues.  Both of this
     * stage's numbers are therefore checked against that line, which is computed from the
     * profile's log_num_qps and QUERY_PORT's log_max_macs -- two runtime values, never 1024
     * and 128.
     */
    IOReturn exportR4Tx(const UInt64 *in, Cx3R4TxResult *res, UInt64 *out, UInt32 port = 1);
    /*
     * R4 stage 9: the first selector that makes a byte move.
     *
     * It posts send WQEs into stage 6's queue and rings the send doorbell, and it consumes
     * completion elements from BOTH completion queues: the send queue's (were the frames
     * accepted?) and the receive side's (did anything arrive, and which buffer did it use?).
     * The six judgements and the reason the transmit half cannot hide the receive half are in
     * the ABI block's comment; what matters here is the shape of the call:
     *
     *   one WQE is posted and its completion awaited FIRST, and only then does the wrap phase
     *   post the rest.  A wrong encoding is met on one frame rather than on five hundred, and
     *   the wrap -- where a mistake would send the hardware after a slot nobody posted --
     *   happens only after a frame has demonstrably gone through the whole path.
     *
     * The frames it sends are on the wire: one 60-byte broadcast frame per post, ethertype
     * 0x88B5 (IEEE Local Experimental 1), carrying this driver's own fixed pattern.  Nothing
     * on a LAN interprets that ethertype, which is why it is the right thing to send during
     * bring-up and ARP -- which does query the network -- is stage 10's.
     */
    IOReturn exportR4Raw(const UInt64 *in, Cx3R4RawResult *res, UInt64 *out);

    /*
     * R4 stage 9's remaining question, asked three ways: does the hardware execute ANY
     * descriptor from this send queue (NOP against SEND), does an MMIO doorbell on the page
     * the QPs were built with reach the device at all (the receive CQ's arm, judged by the
     * COMP EQE it produces), and does the send queue need the doorbell record the public PRM's
     * posting procedure updates.  See the ABI block and docs/M2B-NOTES.md section 52.
     */
    IOReturn exportTxProbe(const UInt64 *in, UInt64 *out);

    /* --- stage 10a: the interface ------------------------------------------------ */
    /*
     * Publish an IOEthernetController interface for the port the chain has brought up.  The MAC
     * comes from QUERY_PORT's def_mac -- the firmware's own address for this port, which is the
     * same one stage 5 registered in the steering table -- so this asks the device rather than
     * caching a copy that could disagree with it.
     *
     * The data path is NOT here: outputPacket below counts and drops, and the receive half is
     * not wired at all.  That is 10b, and it is the half that allocates mbufs.
     */
    IOReturn exportNetUp(const UInt64 *in, UInt64 *out);
    /*
     * The interface step's first step and nothing else -- see kCx3SelNetUpStop.  It exists to
     * separate "the steps after the address" from "entering this path at all", because five
     * machine deaths are all inside the netup call and one of them produced no driver log
     * whatsoever (docs/HANG-AUDIT.md 10.9).
     */
    IOReturn exportNetUpStop(const UInt64 *in, UInt64 *out);
    /*
     * The interface step with a stop point: 0 = the whole of it (what the netup selector is),
     * 1..5 = return once that step is done (what netupstop asks for).  One implementation for both
     * selectors, so the diagnostic cannot walk a different path than the real call.
     */
    IOReturn netupRun(UInt32 stopAfter, UInt64 *out);
    static void netAttachThread(thread_call_param_t param0,
                                thread_call_param_t param1);
    static void netDetachThread(thread_call_param_t param0,
                                thread_call_param_t param1);
    bool     netDetachInterfacesAndWait(UInt32 waitMs);
    /*
     * attachInterface under the controller's work-loop gate, on the calling
     * thread_call thread. runAction does not hand off to the work-loop thread.
     * The gate-context experiment is not a demonstrated fix for the 21:23 hang.
     */
    static IOReturn netAttachOnWorkLoopAction(OSObject *owner, void *arg0,
                                              void *arg1, void *arg2, void *arg3);
    /* The no-op half of the same check: runAction with a body that does nothing. */
    static IOReturn netAttachLoopNoopAction(OSObject *owner, void *arg0,
                                            void *arg1, void *arg2, void *arg3);
    /*
     * Run the no-attach action under the controller's work-loop gate.
     * Called from netupRun when the selector's stop point is 6, so the check costs no
     * device command and cannot attach anything.
     */
    bool     netAttachWorkLoopSelfTest();
    IOReturn netAttachOnKernelThread(bool *attached,
                                     IONetworkInterface **interfaceOut);
    void netAttachWorkerMark(UInt32 phase);
    bool netAttachPoisoned();
    /*
     * kCx3SelNetStat -- the data path's counters, read-only, no device access.
     *
     * Deliberately a separate call from netup: netup answers "can an interface exist", this
     * answers "what has it done", and the two are asked at different times (the second one
     * repeatedly, during and after a ping).
     */
    IOReturn exportNetStat(Cx3NetStatResult *res, UInt64 *out);
    /*
     * kCx3SelNetLink -- M5-1: the media table, the selected medium, and a link re-reading.
     *
     * Gated by its own mode input so that the M4 test is untouched: the driver's link report is
     * exactly what it was until `cx3ctl netlink publish` is run, and that can be run after M4
     * passes without another reboot.
     */
    IOReturn exportNetLink(const UInt64 *in, UInt64 *out);
    /*
     * kCx3SelNetOff -- the offload switches (M5-3).  RX only for now, and off by default.
     *
     * getChecksumSupport() is the one place macOS asks what a driver can offload
     * (IONetworkController.h:967-969; any answer other than kIOReturnSuccess means "none"), and
     * the family then enables it in the ifnet itself -- there is no setChecksumSupport in either
     * header set.  So this reports the capability and the switch decides whether frames are
     * actually marked; a frame with no verdict is left unmarked, which is what makes the switch
     * safe to leave off.
     */
    IOReturn exportNetOff(const UInt64 *in, UInt64 *out);
    /*
     * kCx3SelNetArm -- drain the receive CQ, arm it, and see whether a COMP event arrives.
     * The one measurement stage 9 got wrong for a reason of its own (the queue was not empty),
     * and the prerequisite for interrupt-driven receive.  See cx3_abi.h for the refusal codes
     * and for why the outputs are ordered the way they are.
     */
    IOReturn exportNetArm(const UInt64 *in, UInt64 *out);
    /*
     * kCx3SelNetFilt -- the hardware receive filters (M5-8).  Report the switch, turn it on
     * (replaying what the stack already asked for), or turn it off.  See cx3_abi.h for why the
     * whole multicast mechanism on this card is one command sequence and no flow steering.
     */
    IOReturn exportNetFilt(const UInt64 *in, UInt64 *out);
    /*
     * kCx3SelNetPort -- stop the port (CLOSE_PORT).  Re-opening is the chain's port-up step,
     * which takes the MTU as an input; see the ABI header for why there is no "up" mode here.
     */
    IOReturn exportNetPort(const UInt64 *in, UInt64 *out);
    virtual IOReturn getChecksumSupport(UInt32 *checksumMask, UInt32 checksumFamily,
                                        bool isOutput) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getHardwareAddress(IOEthernetAddress *addrP) APPLE_KEXT_OVERRIDE;
    virtual UInt32   outputPacket(mbuf_t pkt, void *param) APPLE_KEXT_OVERRIDE;
    /*
     * The MTU path: what this controller can carry, and the callback the stack uses to change
     * it.  MISSING UNTIL 0.44.23, and jumbo (M5) is why it matters: the family asks
     * getMaxPacketSize for the largest packet INCLUDING the frame header and FCS
     * (IOEthernetController.h:386-402), its own default answers kIOEthernetMaxPacketSize = 1518
     * (:45), and so `ifconfig en<N> mtu 9000` could never be accepted whatever the rings were
     * built to hold.  The working driver on this machine overrides both and derives its answer
     * from the same sum (LucyRTL8125Ethernet.cpp:1013-1031 with hpp:233-234: kMaxMtu +
     * ETH_HLEN + VLAN_HLEN + ETH_FCS_LEN), and its setMaxPacketSize (:1026-1066) is where an
     * MTU change is actually applied.
     *
     * Two facts from the base class decide how these behave, and both are why the implementation
     * below may refuse instead of trying to do something clever: setMaxPacketSize is "a CLIENT
     * request to change the maximum packet size" whose documented base answer is
     * kIOReturnUnsupported -- so an error code is an answer, not a broken bring-up, and nothing
     * in the attach path depends on it succeeding (IONetworkController.h:667-676).  The same
     * block says it "is synchronized by the workloop's gate", which is why the implementation
     * sends no command and takes no lock -- SET_PORT from here would wait on the command
     * semaphore while holding that gate.  (getMaxPacketSize/getMinPacketSize are pure virtual on
     * IONetworkController, :687 and :695, and implemented by IOEthernetController, which is why
     * this driver compiled for nine stages without them.)
     */
    virtual IOReturn getMaxPacketSize(UInt32 *maxSize) const APPLE_KEXT_OVERRIDE;
    virtual IOReturn getMinPacketSize(UInt32 *minSize) const APPLE_KEXT_OVERRIDE;
    virtual IOReturn setMaxPacketSize(UInt32 maxSize) APPLE_KEXT_OVERRIDE;
    /*
     * enable() / disable() -- the callbacks the stack uses to bring the interface up.
     *
     * MISSING UNTIL 0.44.6, and it is a hard blocker rather than a detail: the base class's
     * implementations answer kIOReturnUnsupported (IONetworkController.h:1243-1246 and
     * 1250-1254, "Returns kIOReturnUnsupported. Drivers that override this method must return
     * kIOReturnSuccess on success"), and `ifconfig en<N> up` goes through them.  So the
     * interface could be attached, match the driver stack and be listed -- and still never come
     * up, whatever the data path did.  The working driver on this machine overrides both
     * (LucyRTL8125Ethernet.cpp:350 and :395).
     *
     * Context, and it is the reason these two take no locks and send no commands: both are
     * "synchronized by the workloop's gate" (IONetworkController.h:1242-1253), and this driver
     * hands the family its OWN workloop (getWorkLoop below).  A mailbox command here would wait
     * on the command semaphore while holding that gate, and a selector that holds the
     * transaction lock and is waiting for an interrupt (which the gate delivers) would then
     * deadlock against it.  So: state and link status only.
     */
    virtual IOReturn enable(IONetworkInterface *iface) APPLE_KEXT_OVERRIDE;
    virtual IOReturn disable(IONetworkInterface *iface) APPLE_KEXT_OVERRIDE;
    /*
     * The three filter callbacks, and what they admit.
     *
     * IOEthernetController's own implementations answer kIOReturnUnsupported
     * (IOEthernetController.h:320-323 and the same wording on the other two), and the working
     * driver on this machine overrides all three.  This port does not program a hardware
     * multicast or promiscuous filter -- the device's filter is the firmware MAC table that
     * stage 5 registers, which carries this interface's own address and (as the stage 9 receive
     * run showed, with real ARP frames) broadcast.
     *
     * So the recording is real and the programming is not, and these three say exactly that:
     * they accept the request, remember it, and log once that the hardware was not told.  The
     * alternative -- refusing -- risks the interface failing to come up over a filter this
     * driver's milestone does not need, and the alternative of accepting *silently* would be a
     * driver claiming a capability it does not have, which this project has paid for before.
     * Programming the hardware is M5 work, next to the media table.
     */
    virtual IOReturn setPromiscuousMode(bool active) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setMulticastMode(bool active) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setMulticastList(IOEthernetAddress *addrs, UInt32 count)
        APPLE_KEXT_OVERRIDE;
    /*
     * The three hooks the family's attachInterface goes through, overridden for ONE reason: to
     * name, in a hard death, which of them was running.
     *
     * Measured 2026-10-13: the stop-point bisect named attachInterface as the fatal step (the same
     * selector with steps 3-5 removed, and with doRegister=false, is clean), and it hangs rather
     * than failing cleanly -- so something inside the family's attach does not come back on this
     * machine, with our object and in this context.  Each implementation below behaves exactly as
     * the one it replaces (super is called where there is one) and logs on the way in and on the
     * way out, so the next death says which of the three it was in.
     *
     * createInterface is deliberately NOT reimplemented from scratch: IOEthernetController.h:514-521
     * says its own implementation returns the IOEthernetInterface and that subclasses "will have
     * little reason to override this implementation" -- so overriding it to write our own object
     * would replace the family's answer with our guess at it.
     */
    virtual IONetworkInterface *createInterface() APPLE_KEXT_OVERRIDE;
    virtual bool configureInterface(IONetworkInterface *iface) APPLE_KEXT_OVERRIDE;
    virtual IOReturn registerWithPolicyMaker(IOService *policyMaker) APPLE_KEXT_OVERRIDE;
    /*
     * The family's gate must run on OUR workloop.
     *
     * This override is the one thing the working Ethernet driver on this machine does that this
     * port did not: LucyRTL8125Ethernet.cpp:698 overrides getWorkLoop() and hands the family its
     * own workloop.  Without it the family builds a SECOND, private workloop, and its
     * open/close/stop machinery -- which is entered whenever a user client goes away -- runs on
     * that one.  The symptom measured (docs/M2B-NOTES.md 55) is exactly a deadlock shape: the
     * chain reaches "r4cq OK", the client logs "user client closed", and the machine stops
     * there, with no further driver log line at all.
     */
    virtual IOWorkLoop *getWorkLoop() const APPLE_KEXT_OVERRIDE;
    /*
     * ...and the other half of the same contract.
     *
     * IONetworkController calls createWorkLoop() BEFORE its first getWorkLoop() and fails
     * start() if it returns false (IONetworkController.h:1137-1147).  This port overrode only
     * getWorkLoop(), which leaves the "create" half to the base class -- and the base's half
     * builds a loop of its own.  The working Ethernet driver on this machine overrides both and
     * hands the family the same self-owned loop from each (LucyRTL8125Ethernet.cpp:685 and
     * :698).  However the family ends up holding it, there has to be exactly one workloop, and
     * that is the whole point of implementing both.
     *
     * Access: the base declares this protected, and this class has no protected section (every
     * declaration here is public or private), so it is declared alongside getWorkLoop() rather
     * than opening a section for one line.  It makes no difference to the call: the family
     * reaches it through the base's own protected declaration.
     */
    virtual bool createWorkLoop() APPLE_KEXT_OVERRIDE;
    /* 10b's hook, kept visible so the next reader sees where the receive path goes. */
    void     netReceiveFrame(const UInt8 *frame, UInt32 len);

    /* IONetworkInterface, not IOEthernetInterface: the interface is created BY
     * IONetworkController::attachInterface() (a null pointer in is the family's signal to build
     * it) and what comes back is the base type.  The first version typed this as the Ethernet
     * subclass and the compiler caught the assignment -- which is the right order for a mistake
     * whose symptom would otherwise have been "the interface is attached and nothing points at
     * it". */
    IONetworkInterface *fNetIface;    /* attached by kCx3SelNetUp, once */
    UInt8    fNetMac[6];              /* the port's address, from QUERY_PORT's def_mac */
    bool     fNetMacValid;
    UInt32   fNetUnit;                /* the kernel's unit number: the interface is en<unit> */
    UInt32   fNetOutputDropped;       /* how many frames the stack handed us (10b will send) */
    Cx3EthernetPort *fNativePorts[2]; /* fixed per-port native callback owners */
    Cx3QueueNamespace fQueueNamespace; /* immutable after reservation for this generation */
    Cx3PortQueues fPortQueues[2]; /* stable per-port owners; no mutable active-port alias */
    Cx3PortState fPorts[2]; /* physical ports; PF command/ICM/MTT ownership remains shared */
    Cx3AutoNetworkResult fAutoNetwork;
    bool     fAutoNetworkUserDisabled; /* changed with the family workloop gate held */
    bool     fNetEnabled;             /* enable() ran and disable() has not; see above */
    /*
     * What the port currently is, as MEASURED rather than as assumed.
     *
     * m3portup polls QUERY_PORT until the link bit is set and decodes the speed code, and it
     * reports both -- but it reported them only into that call's outbox, so the driver itself
     * kept no record and had nothing honest to publish when the stack asked.  These two fields
     * are that record: the link state the bring-up step measured, and the speed it decoded.
     * Nothing re-derives them, and publishLinkStatus() says in the log where they came from,
     * so "the link is active" is never a guess dressed up as a reading.
     */
    /*
     * The port's frame size as PROGRAMMED, and the largest the firmware says it could be.
     *
     * Two members because they are two different things, and confusing them is the defect this
     * pair exists to prevent.  fPortFrameMtu is what m3portup put into SET_PORT general -- the
     * WIRE size, mtu + MLX4_ETH_HLEN + MLX4_ETH_VLAN_LEN + MLX4_ETH_FCS_LEN, i.e. 1522 for a
     * 1500 MTU.  fPortMaxMtu is QUERY_PORT's eth_mtu, which is a CAPABILITY (mlx4 stores it as
     * eth_mtu_cap, device.h:532) and reads 9900 on this card.  Only the first bounds what the
     * port will carry right now; the second bounds what it could be asked for, and the
     * difference between them is the whole of the jumbo question (see getMaxPacketSize).
     */
    /*
     * The port's own address, as the PORT-UP step read it, and the flag that says whether it has
     * been read in this instance.
     *
     * It lives here rather than being re-queried by the interface step, for the reason the link
     * status already lives here: `publishLinkStatus()` publishes "the port-up step's own reading
     * ... not re-queried here", and the address belongs to the same set of facts.  It is also the
     * exact value stage 5 programmed into the steering table, so taking it from the same reading
     * is what makes "the address the device matches on" and "the address the interface reports"
     * one value instead of two that agree by luck.
     *
     * Measured reason to stop re-querying it (2026-10-13): four machines died with netup's own
     * QUERY_PORT as the last line anywhere in the system, while the same command through the
     * standalone queryport selector succeeded 176 ms earlier in the same state
     * (docs/HANG-AUDIT.md 10.9).  Removing the call is not a fix for whatever that is -- it is
     * what lets the four steps AFTER it (attach, publish, timer, frame ring) be exercised at all,
     * and those have never run on this machine.  The root cause stays open, with the
     * instrumentation inside cmdBoxLocked waiting for it.
     */
    /*
     * Per port: index 0 is port 1, index 1 is port 2.  A single shared cache meant a QUERY_PORT
     * for port 2 overwrote the address the port-1 interface step had just published, which is
     * harmless while only one interface exists and wrong the moment a second one does.
     */
    /*
     * Which step of the interface step is running, for an OFF-MACHINE poller to read.
     *
     * Published through netstat (a kXactTryLock selector that touches no device state) because the
     * measured failure mode is a machine that goes silent in under 0.35 s: in that window the
     * driver's own log lines do not get out at all, so a reader has no way to see how far the
     * call got.  Sampled from another process and pushed off the machine, the last value that
     * arrives names the step (docs/HANG-AUDIT.md 10.10).
     *
     * 0 = not in the interface step, 1..5 = inside that step, 6 = it returned, 7 = it is
     * registering the interface (which since 0.44.33 happens after all five steps, not inside
     * step 2 -- see the note on attachInterface in the implementation).
     */
    UInt32   fNetupStep;
    /*
     * attachInterface runs on this one-shot kernel thread, not on the user-client thread that
     * holds the selector transaction lock.  The measured freeze happens immediately after that
     * call in the user-client context; normal networking drivers call it from start().
     */
    thread_call_t        fNetAttachCall;
    IOLock *             fNetAttachLock;
    bool                 fNetAttachInFlight;
    bool                 fNetAttachDone;
    bool                 fNetAttachSuccess;
    IONetworkInterface * fNetAttachIface;
    /* One retain per scheduled worker invocation, released by callback or cancel. */
    UInt32               fNetAttachRetainCount;
    /* A timeout makes the attach state unknown; all later selectors are refused. */
    bool                 fNetAttachPoisoned;
    /* Published as cx3AttachWorker for the native 10 ms watcher. */
    UInt32               fNetAttachWorker;
    /* stop() uses a distinct one-shot worker for synchronous family termination. */
    thread_call_t        fNetDetachCall;
    bool                 fNetDetachInFlight;
    bool                 fNetDetachDone;
    bool                 fNetDetachCallFreeFailed;
    UInt32               fNetDetachIfaceCount;
    IONetworkInterface * fNetDetachIfaces[2];
    UInt32               fNetDetachRefCounts[2];
    /*
     * Record the interface step's progress twice: in fNetupStep (for a selector to read) and in
     * the IORegistry (for `ioreg` to read).
     *
     * The registry copy is the one that matters for a death, and the reason is a lock: netstat is
     * kXactTryLock, so while the interface step holds the transaction lock a sampling selector is
     * refused -- exactly during the window that matters.  The registry has no such coupling, so a
     * 100 ms `ioreg` loop keeps reading the last value the driver wrote, up to the moment the
     * machine stops.  (The parallel forensics session proposed this after measuring that the
     * selector route cannot see a sub-second death.)
     */
    void     netupMark(UInt32 step);
    /*
     * Publish the selector number and the phase above.  Set before any transaction lock is
     * taken, so the watcher can tell "never reached the driver" from "waiting for the lock".
     */
    void     userClientMark(UInt32 selector, UInt32 phase);
    /*
     * Publish progress through IOServiceOpen/newUserClient, before a selector exists.
     */
    void     userClientOpenMark(UInt32 phase);
    /*
     * Which hook inside attachInterface is running, in the same IORegistry channel (see the
     * implementation for why the log alone is not enough here).
     * 0 = none, 1 = registerWithPolicyMaker, 2 = createInterface, 3 = configureInterface.
     */
    void     attachHookMark(UInt32 hook);
    /* The general command runner (gated on cx3raw=1) and the state survey. */
    IOReturn exportHcr(const UInt64 *in, const UInt8 *inbox, UInt8 *outbox,
                       UInt64 *out);
    IOReturn exportSurvey(Cx3SurveyResult *res, UInt64 *out);
    /*
     * kCx3SelFault -- inject and clear the command-timeout states.  See the definition for
     * why a fault injector is part of the fix rather than a debugging extra.
     */
    IOReturn exportFault(const UInt64 *in, UInt64 *out);
    /*
     * One selector call is one transaction.
     *
     * The command mailbox is a single shared buffer: every selector fills it, and the
     * command path uses it, and the answer is copied out of it.  The command lock inside
     * Mlx4Hw::cmdBox() covers only the submit-and-wait part, so two selectors running
     * concurrently could interleave around it -- one filling the inbox while another is
     * between submit and copy -- and each would read a mixture.  Both external reviews
     * raised this, twice, and it is the one live defect that no diagnostic can see:
     * the symptom is an occasional wrong answer, not a failure.
     *
     * The entry point that makes it atomic is IOUserClient::externalMethod(), because that
     * wraps the whole dispatch for one selector: fill, submit, copy out.  These two are
     * called around it, so the transaction boundary is exactly one selector call.
     *
     * Deliberately NOT taken by the interrupt action: that path must never send a command
     * (the command path sleeps, and the action runs on the workloop), and it touches no
     * mailbox.  So there is no ordering between this lock and the EQ lock, and no cycle.
     */
    bool transactionEnter();
    void transactionExit();
    /*
     * The same lock, asked for rather than waited for: true means the caller holds it and
     * must call transactionExit().  Used by the selectors that have to stay answerable
     * while another one is holding the lock (probe, peek, survey, m3irqstatus) -- a
     * refusal with kIOReturnBusy is a usable answer ("someone is using the mailbox"), a
     * queue behind a five-minute command is not.
     *
     * Both entry methods fail closed if init could not establish the lock. Locks
     * are allocated before publication, never by concurrent selector callers.
     */
    bool transactionTryEnter();
    /*
     * The first UAR index software may use, derived rather than written down.
     *
     * device.h:1576-1579 and mlx4_main.c:184-193:
     *
     *     mlx4_get_num_reserved_uar() = 128 >> (PAGE_SHIFT - uar_page_shift)
     *     caps.reserved_uars         = max(that, devcap->reserved_uars /
     *                                      (1 << (PAGE_SHIFT - uar_page_shift)))
     *
     * The max is the part that matters: QUERY_DEV_CAP's reserved_uars does NOT decide the
     * starting index, it is one input to a maximum that 128 always wins when the UAR page
     * size equals the system page size -- which is this card, because INIT_HCA's
     * uar_page_sz is 0 (uart_page_shift = 0 + 12 = PAGE_SHIFT).
     *
     * Written as a function because the CQ path had the constant 128 in it, and a constant
     * cannot notice that the device said something else.  Using devcap's 2 would have put a
     * CQ's doorbell inside the 128 UARs the EQ doorbells own -- on top of the one that
     * already works, with the symptom being a queue whose completions never arrive.
     */
    UInt32 reservedUarStart() const;
    /*
     * Put the RX QP back before the memory its context names is released.
     *
     * There is no HW2SW_QP: a QP is taken away by moving it to RST, and until that happens
     * the hardware still follows the MTT entries and the db_rec_addr in its context.  So
     * this runs before rqTeardown, and rqTeardown refuses to release the ring while this one
     * has not succeeded.
     */
    void qpTeardown(const char *why, bool hcaWasReset, UInt32 port = 1);
    void rqTeardown(const char *why, bool hcaWasReset, UInt32 port = 1);
    /*
     * The indirection QP of stage 5, which has the same lifecycle as the RX QP one stage
     * earlier and for the same reason: its context names a producer record, so it has to be
     * put back to RST before that record is released.  Run before qpTeardown -- not because it
     * names the RX QP's memory (it names only a QPN, a number) but because it is the same kind
     * of object and the order in the log should read in the order things were built.
     */
    void ingressTeardown(const char *why, bool hcaWasReset, UInt32 port = 1);
    /*
     * The send side of stage 6: the QP, its producer record, its SEND QUEUE and the TX CQ.
     *
     * Same shape as qpTeardown with one more object, and the order inside is the point: a QP in
     * RTS names the SQ's MTT entries and the db_rec_addr record through its context, so it is
     * put back to RST first and the memory is released only if that worked -- a leak a reboot
     * reclaims, rather than a device writing into recycled memory.  The TX CQ goes back through
     * HW2SW_CQ afterwards, for the reason cqTeardown gives: a queue the firmware still owns must
     * not have its ring freed underneath it.
     *
     * Run before qpTeardown/rqTeardown in the callers: it is the same kind of object built one
     * stage later, and the log then reads in the order the objects were built.
     */
    void txTeardown(const char *why, bool hcaWasReset, UInt32 port = 1);
    /* Hand the CQ back before the memory it names is released. */
    void cqTeardown(const char *why, bool hcaWasReset, UInt32 port = 1);
    /*
     * Hand the memory region back before anything it depends on is released.
     *
     * Same shape and same reason as teardownEq: an MPT entry the hardware owns points
     * into the dMPT table, so releasing that table while the entry is live is the
     * mistake this exists to avoid.  hcaWasReset means the device has already forgotten
     * it, so there is nothing to hand back and the host-side state is simply dropped.
     */
    void mrTeardown(const char *why, bool hcaWasReset);
    IOReturn exportM3PortUp(const UInt64 *in, UInt64 *out);
    /* The ring-initialisation A/B experiment.  Diagnostic only. */
    IOReturn exportM3AbRing(const UInt64 *in, Cx3AbRingResult *res, UInt64 *out);
    /*
     * How many of the first 32 EQ slots carry the owner bit, plus the raw byte of
     * slot 0 -- the measurement that makes a negative poll interpretable.  The two
     * output indices are
     * explicit because the old "+1" convention put the owner byte in the same slot
     * as m3equp's arm-wait milliseconds -- see the definition for what that cost.
     * kCx3OwnerNoByteSlot means "no room, log it instead".
     */
    void     reportOwnerMap(UInt64 *out, UInt32 setIdx, UInt32 byteIdx);
    /* Log the first bytes of the ring with a label saying when they were read. */
    void     snapshotRing(const char *when, UInt32 nbytes);
    /* Release the host-side EQ state.  Only correct after the HCA has been reset
     * (the device forgot the queue) or when the driver is stopping. */
    /*
     * Release the host-side EQ state.  hcaWasReset says whether the HCA has already
     * forgotten the queue (after a reset) or still holds it (when the driver is
     * stopping) -- in the second case HW2SW_EQ has to be sent first.
     */
    void     teardownEq(const char *why, bool hcaWasReset);

    /*
     * Consume events in order from the head of the queue, up to `max`.
     *
     * Must be called with fEqLock held.  Shared by the poll selector and the
     * interrupt action so they cannot disagree about where the queue is -- the
     * earlier arrangement had the selector scanning forward and jumping the index,
     * which is how a delivered EQE ended up behind the read pointer.
     */
    UInt32   consumeEqLocked(UInt32 max, UInt32 *outConsAfter, bool *outBlockedByComp);
    /* The interrupt action: runs on the workloop, takes fEqLock, consumes, re-arms. */
    static void irqAction(OSObject *owner, IOInterruptEventSource *src, int count);

    /*
     * Read back the quarantined memory, and say so if anything in it moved.
     *
     * Called from Cx3Mlx4UserClient::externalMethod() on every entry, which is the cheapest
     * place in this driver to put a periodic check: every tool call passes through it, so a
     * modification is timestamped by the log line next to it.  Silent while everything is
     * intact; see the "quarantine" comment in the private section for what it is looking at.
     */
    void     freezeInstrumentsSweep(const char *where);

    /*
     * Selector admission and the stop handshake.  Public because the user client is a
     * different class; every mutation happens under the short admission lock, and the guard
     * object in Cx3Mlx4UserClient makes the matching exit unconditional on all return paths.
     */
    bool     selectorEnter();
    bool     selectorStopping();
    bool     retentionExhausted() const { return fQuarExhausted; }
    bool     resetAdmissionAllows(UInt32 selector) const; /* transaction lock held */
    void     selectorExit();
    bool     beginStopAndDrain(UInt32 waitMs);
    UInt32   selectorActive();

private:
    void readOnlyProbe();
    /*
     * ---------------- Stage 10b: the kernel data path (RX half) ----------------
     *
     * docs/10B-MBUF-CONTRACT.md (the API contract, cited to this machine's SDK headers and to
     * the working driver in this workspace) and docs/10B-PRIMITIVES.md (what this tree already
     * has, and the invariants each piece assumes) are the two documents this half is built on.
     * Both were written before this code, because the two drivers on this machine that have
     * already panicked the kernel here both did it inside the mbuf cluster allocator.
     *
     * Shape, and why it is a timer rather than the event queue:
     *
     *   - the receive CQ's arm -> COMP EQE -> EQ path is the ONE link in the chain stage 9 never
     *     proved.  The measured result was zero EQEs, and the experiment was then shown to be
     *     invalid for a separate reason (mlx4's arm means "notify when something NEW arrives",
     *     and that CQ was never empty when it was armed).  So the delivery is driven by a
     *     workloop TIMER that polls the CQ: deterministic, independent of the unproven link,
     *     and it also gives the TX half somewhere to reclaim completions.  Arming the CQ is
     *     therefore NOT done here -- which also keeps the EQ's existing rule about COMP events
     *     (it refuses to consume them, because for nine stages there was no CQ to serve) out of
     *     the picture until that rule is changed deliberately, together with a consumer.
     *
     *   - one CQ has exactly one consumer (docs/10B-PRIMITIVES.md 1.2: the slot the CQE names is
     *     compared against the ring position, and two consumers make that comparison fail).  The
     *     r4raw selector consumes the same queue, so every tick takes the transaction lock with
     *     TryLock and skips the tick if a selector holds it.  TryLock rather than Lock on
     *     purpose: this runs with the workloop's gate held, and blocking there against a selector
     *     that is waiting for something this gate delivers is a deadlock, not a delay.
     */
    static void netTimerAction(OSObject *owner, IOTimerEventSource *src);
    enum {
        /*
         * One tick per millisecond.  The receive path is POLLED, so this is the delivery
         * latency's upper bound -- and at 1 ms nothing this machine's LAN produces can fill the
         * receive CQ's entries faster than a tick drains them.  It is a first-measurement
         * choice, not a tuned one: M5 is where interrupt delivery (once the arm path is proved)
         * and the tick rate belong.
         */
        kNetTickNs    = 1000000ull,
        /*
         * How many completions one tick will serve, so that a burst cannot hold the workloop's
         * gate for an unbounded time.  Well above what the ring holds (128), because leaving
         * completions behind for the next tick is how a busy queue turns into a wrapping one.
         */
        kNetRxPerTick = 256
        ,
        /*
         * The transmit half.  One frame buffer per TXBB of the send queue, so the mapping
         * between "the completion named TXBB n" and "the buffer to hand back" is the identity
         * and needs no second index to get wrong.  2048 bytes each, which is the receive ring's
         * buffer size for the same reason: two per 4 KiB page, so a buffer's bus address is a
         * page plus a fixed offset (Mlx4Hw::rqBufferPhys computes exactly that).
         */
        /*
         * The FLOOR for one transmit frame buffer, and only the floor: the size actually
         * allocated follows the interface's MTU (fNetTxBufBytes below), because a jumbo frame
         * does not fit 2 KiB.  With the default MTU of 1500 the computed size is exactly this,
         * byte for byte, which is what keeps the default path identical to the one M4 was
         * designed against.
         */
        kNetTxBufBytes = 2048,
        /* The default send queue is 256 TXBBs (CX3_R4TX_DEFAULT_SQ_TXBB); a bigger one is
         * refused rather than silently overflowing this array. */
        kNetTxSlots    = 256,
        kNetTxPerTick  = 256,
        /*
         * The most this driver will put into one WQE is what a frame buffer holds, and that is
         * stated as such rather than as an MTU: the MTU belongs to the stack (it is what
         * `ifconfig en<N> mtu` sets and what the interface reports, and the reference derives
         * its own value the same way), and the family has its own idea of the Ethernet maximum
         * in IOEthernetController.h.  Copying either of those numbers in here would be a
         * second definition of a value that already has one -- and this project's build gate
         * refuses exactly that (tools/check-hardcoded.py, which caught the literal this line
         * used to carry).
         *
         * What is being defended here is narrower and real: mbuf_copydata() writes into this
         * buffer, so a frame longer than the buffer must be refused rather than truncated.
         */
        /*
         * The frame-length cap is NOT here any more: it follows the buffer size, which follows
         * the MTU (outputPacket checks `len > fNetTxBufBytes - MLX4_NET_IP_ALIGN`).  A constant
         * would be a second answer to a question the ring already answers.
         */
        kNetTxMaxFrameUnused = 0,
        /* The smallest thing that can be called an Ethernet frame: the header alone. */
        kNetTxMinFrame = 14
    };
    bool     netServiceStart(const char *why);   /* create the timer and add it to the workloop */
    bool     netTimerRelease(const char *why);   /* retain the timer unless removal is confirmed */
    bool     netServiceStop(const char *why);
    void     netTick();                          /* one timer fire: service, then re-arm */
    void     netServiceRx();                     /* drain the receive CQ into the stack */
    void     netRxRecycle(UInt32 slot);          /* re-post one slot, after its frame was read */
    /*
     * The re-post itself: rewrite the slot's WQE against the SAME buffer, hand the WQE over by
     * publishing the producer, and mark the slot in flight again.  ONE implementation, because
     * there were two -- r4RawRxRecycle (stage 9, the path that has actually received frames on
     * this machine) and netRxRecycle (10b) did byte for byte the same three device operations
     * and differed only in which counters they touched.  Two copies of a device operation is how
     * this project has repeatedly ended up with one of them fixed and the other not
     * (docs/DEFECTS.md F17 and F20 are that shape exactly, in two different files of the same
     * feature), and both callers now keep their own counting while the operation lives here.
     *
     * Returns false when the slot has no usable bus address: then the WQE is deliberately NOT
     * written, because a descriptor naming address zero is one the hardware would fill somewhere
     * else, and the ring running short is the cheaper failure.
     */
    bool     rqRepostSlot(UInt32 slot);
    bool     netTxResourcesEnsure(const char *why);  /* the frame buffer ring, one per TXBB */
    void     netServiceTx();                     /* reclaim completed sends and their mbufs */
    /*
     * The largest STACK frame -- L2 header plus payload, no FCS and no VLAN tag, i.e. what
     * outputPacket is handed and what inputPacket is given -- that all three pieces of this path
     * can carry, together with the three bounds it is the minimum of.  Any argument may be null.
     */
    UInt32   netStackFrameMax(UInt32 *outPortBound, UInt32 *outRxBound,
                              UInt32 *outTxBound) const;

    /* The data path's own clock and counters.  All of them are readable in the log: a timer
     * that is not firing and a CQ with nothing on it look identical from outside. */
    IOTimerEventSource *fNetTimer;
    UInt64   fNetTicks;
    UInt64   fNetTicksSkipped;    /* ticks that found a selector holding the transaction lock */
    UInt64   fNetRxFrames;
    UInt64   fNetRxDropped;       /* no mbuf, or a frame the ring's own checks refused */
    UInt64   fNetRxBytes;
    UInt64   fNetRxRefills;
    UInt64   fNetRxNotOurs;       /* completions naming another QPN */
    UInt64   fNetRxNotDelivered;  /* inputPacket returned 0: taken from us, but not submitted */
    UInt64   fNetRxNoRings;       /* ticks that found the receive objects gone (a reset ran) */
    UInt32   fNetRxErrors;        /* ring checks that failed: the refill stops on these */
    bool     fNetInTick;          /* re-entry guard for the timer action */
    /*
     * The transmit half's own state.
     *
     * fNetTxMbuf[slot] is "the mbuf that TXBB `slot` is carrying, or 0 while that TXBB is free",
     * and it is the whole async design in one array: the frame cannot be freed when outputPacket
     * returns (the hardware has not sent it yet), and it cannot be freed twice, so the only
     * place it may be freed is the completion that names the same TXBB.  fNetTxOwner records
     * the owner phase that TXBB was posted in, because the reclaim stamp has to carry the
     * slot's OWN phase and not the next one's -- getting that wrong is what produced an ERROR
     * completion for every post after the ring wrapped (docs/M2B-NOTES.md 55).
     */
    Mlx4Icm *fNetTxRing;
    /*
     * The transmit state's own lock, and it exists because of a fact this port established
     * AFTER writing the first version of this half: the pull model is not available here.
     *
     * The reference driver on this machine serialises every transmission on the family's output
     * thread (configureOutputPullModel + startOutputThread + outputStart), which is why it needs
     * no lock of its own.  Those three methods are declared in MacKernelSDK's headers and NOT in
     * this machine's system headers -- and this port builds its IOKit tree against the RUNNING
     * system's headers, because that is what makes the vtable layout match the kernel collection
     * (docs/R4-STAGE10-PLAN.md section 10a: two of these methods differ between the two header
     * sets, and calling one that the running family does not have is a jump into the wrong slot,
     * not a compile error).  So the transmission stays inline in outputPacket -- which the
     * headers say is legal when no output queue is configured -- and the serialisation this lock
     * provides has to be done here instead.
     *
     * What it protects: fR4RawTxProd, fR4RawTxCons, fNetTxMbuf[], fNetTxOwner[].  Two threads
     * really can be in here at once -- a stack thread sending a frame (outputPacket), and the
     * workloop's tick reclaiming completions (netServiceTx) -- and the failure a missing lock
     * produces is the expensive kind: two frames claiming one TXBB, or a completion freeing an
     * mbuf whose WQE was overwritten by the other writer.
     *
     * TryLock and not Lock, because outputPacket is documented as "must not block"
     * (IONetworkController.h:488-490) and nothing here knows what context the stack calls it
     * from.  A lost race returns kIOReturnOutputStall, which hands the frame back rather than
     * dropping it, and is counted separately from a drop.
     */
    IOLock  *fNetTxLock;
    UInt64   fNetTxLockBusy;      /* times a frame was handed back because the lock was held */
    Mlx4TxAdmissionGate fNetTxAdmission; /* TX/timer/enable/disable fence; closed permanently by stop() */
    /* What the filter callbacks were asked for.  Recorded because they are honest about not
     * programming the hardware, and a reader has to be able to see what the stack wanted. */
    /* M5-1's state.  The dictionary and the medium object are this driver's; the publication
     * copies the dictionary, so both are released when the driver goes away. */
    OSDictionary          *fMediaDict;
    const IONetworkMedium *fMediumAuto;
    bool                   fMediaPublished;
    /* M5-3: mark received frames the hardware verified.  Default off -- the stack then
     * computes every checksum itself, which is the behaviour every test so far has run with. */
    bool     fNetRxCsum;
    /*
     * 1 = the receive ring's initial WQEs have been marked in flight for the NET path.
     * Stage 3 fills them and stage 4 publishes the producer, so they are all in flight before
     * this driver posts one of its own; the raw path reads that once (exportR4Raw) and the net
     * path has to do the same, or every completion is refused as "not in flight".
     */
    bool     fNetRxPrimed;
    UInt64   fNetRxCsumGood;
    UInt64   fNetRxCsumNone;
    /*
     * M5-3, transmit side.  Unlike the receive half this one IS an offload the hardware has to be
     * told about, and it is also a capability the FAMILY has to know about -- and the family asks
     * once, at attach.  So the switch is read in two places (getChecksumSupport for the
     * advertisement, outputPacket for the per-frame request), and the honest consequence is that
     * flipping it after the interface exists affects only the receive side until the next attach.
     * kCx3SelNetOff's note in cx3_abi.h carries the same statement for the tool.
     */
    /*
     * M5-8's state.  The list is REMEMBERED because the stack only calls setMulticastList when
     * its own list changes: without a remembered copy there would be nothing to program at the
     * moment the switch is turned on, and the switch would look like it did nothing.
     */
    bool     fNetMcastTable;      /* 1 = group addresses are programmed into the device */
    UInt32   fNetMcastStored;     /* how many addresses the driver remembers */
    UInt32   fNetMcastPrograms;   /* how many times the table has been written */
    UInt32   fNetMcastLastFw;     /* fw status of the last command of the last programming */
    UInt32   fNetMcastTruncated;  /* addresses that did not fit the driver's own list */
    bool     fNetPromiscProgrammed; /* 1 = the unicast filter is off in the device */
    UInt32   fNetPromiscFw;         /* fw status of the last promiscuous-mode command */
    UInt8    fNetMcastList[CX3_NETFILT_MAX_ADDRS][6];
    /* The whole table sequence, in the reference's order (mlx4_en_netdev.c:905-930). */
    bool     programMulticastTable(const char *why);
    /*
     * Promiscuous mode, which under A0 is the qpn_calc command's promisc bit.  It refuses when
     * the RX QP is not up (its QPN is the base the command needs) or when QUERY_PORT does not
     * answer (n_mac is a device answer, not a constant), and it turns the port's multicast
     * filter off when it goes on, exactly as the reference does.
     */
    bool     programPromiscuous(bool on, const char *why, UInt32 *outFwStatus);
    bool     fNetTxCsum;
    UInt64   fNetTxCsumGood;
    UInt64   fNetTxCsumNone;
    bool     fNetMcastMode;
    bool     fNetPromiscMode;
    UInt32   fNetMcastListCount;
    UInt32   fNetMcastListLogs;   /* so the addresses are printed once per list, not per call */
    UInt32   fNetTxSlotCount;     /* == fR4TxSqNent when the ring was built */
    /*
     * The size of ONE frame buffer in the transmit ring, and it is not a constant: it follows
     * the interface's MTU, which is what the reference does (docs/M5-REFERENCE.md 3.2: the buffer
     * is sized from mtu + header + VLAN + FCS + alignment, rounded up to a power of two).  Read
     * at netup, from the interface the family gave us, and logged with its inputs -- because
     * "which MTU was this ring built for" is the question a jumbo failure asks first.
     */
    UInt32   fNetTxBufBytes;
    UInt32   fNetMtu;             /* the MTU this data path was built for */
    mbuf_t   fNetTxMbuf[kNetTxSlots];
    UInt8    fNetTxOwner[kNetTxSlots];
    UInt64   fNetTxFrames;
    UInt64   fNetTxCompleted;
    UInt64   fNetTxDropped;       /* frames this driver had to drop, with the reason in the log */
    UInt64   fNetTxStalled;       /* returned to the stack rather than dropped: no free TXBB */
    UInt32   fNetTxErrors;        /* completions in ERROR, or a completion naming a wrong TXBB */
    UInt32   fNetTxFlushErrors;   /* subset of ERROR CQEs: WR_FLUSH_ERR (cq.h:110) */
    /* Protected by fNetTxLock; sticky for this controller lifetime, not cleared by down/up.
     * Capture before reclaim/stamp. This is host evidence, not a QP/DMA release proof. */
    struct NetTxFault {
        bool latched;
        bool wqeValid;
        bool hadMbuf;
        UInt8 owner;
        UInt32 consumer;
        UInt32 producer;
        UInt32 slot;
        UInt32 qpn;
        UInt8 cqe[CQE_LEGACY_SIZE];
        UInt8 wqe[MLX4_TXBB_SIZE];
    } fNetTxFault;
    UInt32   fNetTxInFlightAtStop;/* how many frames were still posted when the path stopped */
    /*
     * Publish the link state the port-up step measured, and say in the log which reading it
     * came from.  Called from netup (once the interface exists) and from enable() (when the
     * stack brings it up), because the family drops a link status reported while there is no
     * interface to carry it.
     */
    void publishLinkStatus(const char *why);
    /* Build and publish the media dictionary, and select the one entry in it.  Idempotent. */
    bool publishMediaDictionary(const char *why);
    /* --- R4 stage 9: the pieces of exportR4Raw --- */
    /* The frame buffer, allocated on first use and released by txTeardown. */
    bool   r4RawFrameEnsure(Cx3R4RawResult *res);
    /* The transmit phase: one WQE at a time, each completion awaited before the next post. */
    void   r4RawTransmit(Cx3R4RawResult *res, UInt32 postsWanted);
    UInt32 r4TxProbePost(UInt32 opcode, UInt32 nDataSegs, UInt32 recWord0, UInt32 recWord1,
                         UInt32 waitMs, UInt32 *outOpcode, UInt32 *outStatus, UInt32 *outWqeOk);
    /*
     * One pass over the receive completion queue: consume what is ready, refill each slot the
     * CQEs name, and stop when `wait` is false or the budget has run out.  Returns how many
     * completions it consumed.  Called twice per call -- once before the transmit phase (what
     * is already queued) and once after it (what arrives while the frames go out).
     */
    UInt32 r4RawRxPoll(Cx3R4RawResult *res, UInt32 budgetMs, bool wait);
    void   r4RawRxRecycle(Cx3R4RawResult *res, UInt32 slot);
    bool   r4RawRxInFlight(UInt32 slot) const;
    void   r4RawRxSetInFlight(UInt32 slot, bool inFlight);
    UInt32 savedPciCommand() const;
    void fillFwAreaSummary(const char *when, UInt64 *out);
    /* mlx4_init_icm_table(), one table: allocate its reserved prefix and MAP_ICM
     * it at the table's virtual base (mlx4_icm.c:387). */
    bool mapIcmTable(Mlx4IcmTable *t, bool *fatal);
    /* mlx4_init_icm(): the whole context-memory bring-up (mlx4_main.c:1563). */
    void initIcmTables(UInt64 *out, UInt32 *outTablesDone);
    /*
     * UNMAP_ICM every mapped chunk, then the auxiliary area, and free them.
     * Called before re-profiling, on an HCA reset and on stop.
     *
     * What it does with each allocation is decided by whether the unmap was
     * confirmed, not by whether the call was made: a chunk the firmware refused to
     * unmap is kept, and the transaction count is reported.  See docs/ICM-UNMAP.md
     * -- the version before 0.43.0 dropped the return value, freed the memory and
     * then logged that the HCA no longer referenced it.
     */
    bool hasContextOwners() const;
    bool hasIcmTableAllocations() const;
    bool hasStopRetentionOwners() const;
    bool closePortsForStop(const char *why);
    bool closeHcaForStop(const char *why);
    bool closeFwForStop(const char *why);
    bool fwReplacementAllowed(); /* transaction held; no device access */
    void freeIcmTables();

    IOPCIDevice *fPci;
    Mlx4Hw       fHw;
    Mlx4PciIdentity fStartPciIdentity;
    UInt64       fDeviceGeneration;
    bool         fStartPciIdentityValid;
    bool         fPciOpen;
    Mlx4Icm *    fFwIcm;        /* the FW area, kept for a later MAP_FA */
    /*
     * fw_pages as QUERY_FW answered it, so fwload can refuse a page count that
     * disagrees with the device rather than trusting its own command line.
     */
    UInt32       fQueriedFwPages;
    /* Sticky for this controller lifetime: no live/uncertain RUN_FW replacement
     * until a separate close/recovery contract proves it safe. Not a DMA proof. */
    bool         fFwRunAttempted;
    /* M2d state.  Held in the provider rather than in the user client so that
     * the profile, the mapped tables and the INIT_HCA parameters stay one
     * object across the calls that build them. */
    Mlx4DevCap       fDevCap;
    Mlx4Profile      fProfile;
    Mlx4InitHcaParam fInitHca;
    /*
     * The memory region, as durable state rather than only as this call's output.
     *
     * Both reviews of stage 1 made the same point: returning the numbers to the caller
     * is not the same as owning a resource.  Re-running the selector used to allocate a
     * fresh PD and re-send SW2HW_MPT for the SAME MPT slot, and the teardown path
     * released the tables underneath an entry the hardware owned.  These fields are
     * what make the selector idempotent and the teardown ordered.
     *
     * fMrState: 0 = none, 1 = the hardware owns the entry, 2 = it was never confirmed
     * (a timeout means "unknown", which is a third state, not a failure to reuse).
     */
    UInt32           fMrState;
    UInt32           fMrPdn;
    UInt32           fMrMptIndex;
    UInt32           fMrLkey;
    Mlx4DmaGeneration fMrGeneration; /* MPT SW2HW attempt; bookkeeping only, never release authority */
    /* PF-wide transaction/admission and retained-provider ownership. */
    IOLock *         fXactLock;
    IOLock *         fAdmissionLock;
    bool             fStopRetained;
    IOService *      fStopProvider;
    Mlx4IcmTable     fTables[kMaxIcmTables];
    UInt32           fNumTables;
    UInt64           fIcmSize;
    UInt64           fAuxUnits;      /* SET_ICM_SIZE's answer, 1 MiB units */
    Mlx4Icm *        fAuxIcm;
    bool             fIcmMapped;     /* tables have been mapped already */
    /*
     * Whether the firmware instance running *now* has been told about the mappings
     * this provider still records in fTables[]/fAuxIcm.
     *
     * Set when a MAP_ICM or MAP_ICM_AUX is accepted, cleared by the HCA reset (which
     * restarts the firmware: exportFwLoad's MAP_FA + RUN_FW) and by freeIcmTables.
     * It exists because the host's table list survives a reset while the firmware's
     * side of every mapping does not, so "these chunks are mapped" and "the firmware
     * knows about these chunks" are two different facts.  Sending the second one's
     * command on the strength of the first is what produced fourteen UNMAP_ICM
     * failures ending in fw status 0x1 on every teardown that followed a reset --
     * docs/ICM-UNMAP.md has the log evidence.
     */
    bool             fIcmHcaHolds;
    /*
     * ICM pages a failed UNMAP left behind on purpose: memory the firmware may still
     * be translating through, so it is never handed back to the kernel.  A count
     * rather than the pointers, because keeping a pointer means the next bring-up
     * would find a non-zero slot and reuse it as if its mapping were known.
     * Expected value 0; every nonzero increment is logged with the fw status that
     * caused it.
     */
    UInt32           fLostIcmPages;
    bool             fHcaInited;
    /*
     * M3 restart 1: the one EQ this step builds, kept here so polling does not
     * have to be handed anything.  fEqRing is host memory whose per-page bus
     * addresses are in the MTT, which is what the HCA writes into.
     */
    Mlx4Icm *        fEqRing;
    bool             fEqUnknownRetained; /* SW2HW_EQ timed out; owner is tracked but not usable */
    bool             fEqHwReturned; /* HW2SW_EQ succeeded; retry must not send it again */
    bool             fEqMayBeDeviceOwned; /* command reached/was ambiguously posted to HCA */
    UInt32           fEqNent;
    UInt8            fEqeFactor;
    UInt32           fEqn;
    UInt32           fEqMTTOffset;
    UInt32           fEqMttPages;
    UInt64           fEqDoorbell;
    UInt32           fEqConsIndex;
    UInt32           fFuncMaxEq, fFuncReservedEq, fFuncReservedUar;
    /*
     * Interrupt session state.
     *
     * Three separate conditions, because conflating them is how the first version
     * leaked: a storm trip set "session up = false" while the event source and the
     * workloop were still allocated, and m3irqdown then returned early on that flag
     * and never released them.  So:
     *
     *   fIrqObjectsUp  the source and workloop exist and must be released
     *   fIrqEnabled    notifications are being delivered
     *   fIrqStormTripped  a fault is latched; delivery is off and the session must be
     *                     closed and reopened rather than resumed
     *
     * Both reviews called this out independently.
     */
    bool                   fIrqObjectsUp;
    bool                   fIrqEnabled;
    bool                   fIrqStormTripped;
    /*
     * fEqLock serialises every access to the queue: the poll selector, the interrupt
     * action and the teardown all take it, because a callback that moves the consumer
     * index while a selector is reading it is the kind of race that shows up as an
     * intermittent lost event.
     */
    IOLock                *fEqLock;
    IOWorkLoop            *fWorkLoop;
    IOInterruptEventSource *fIrqSource;
    UInt32                 fIrqSourceIndex;
    /*
     * Counted separately, on both reviews' advice.  "Action invocations" is not
     * "hardware interrupts" (the callback's own count argument is ignored), and
     * EQEs consumed through the interrupt is not the same quantity as EQEs consumed
     * by the poll selector, because the two share one consumer.
     */
    UInt64                 fIrqCount;        /* action invocations */
    UInt64                 fIrqCallbackCount;/* sum of the callback's count argument */
    UInt64                 fIrqEqeCount;     /* EQEs consumed inside the action */
    UInt64                 fPollEqeCount;    /* EQEs consumed by the poll selector */
    UInt64                 fIrqErrors;       /* ready slots that carried nothing usable */
    UInt64                 fIrqSkipped;      /* invocations that consumed nothing */
    /*
     * The token an irqTest is waiting for, and what the action saw of it.  The
     * result is kept here rather than returned from the posting call, because the
     * whole point is that the completion arrives later, on another thread.
     */
    bool                   fIrqPending;
    UInt32                 fIrqWantToken;
    bool                   fIrqMatched;
    UInt32                 fIrqMatchedToken;
    UInt32                 fIrqMatchedStatus;
    UInt32                 fIrqLastType;
    /* True when a legacy interrupt can be acknowledged, i.e. the clear is set up. */
    bool                   fClrIntReady;
    /* True while the enabled source is a legacy one, i.e. one that needs a clear. */
    bool                   fClrIntLegacy;
    /* Per-invocation count of real acknowledgements written, for the report. */
    UInt64                 fClrIntWrites;
    UInt32                 fIrqZeroStreak;   /* consecutive 0-consumption invocations */
    /* Release the source/workloop if they exist, whatever the other flags say. */
    bool     irqReleaseObjects(const char *why);
    /*
     * ------------------ the freeze investigation's instruments -----------------
     *
     * docs/HANG-AUDIT.md sections 6-8.  Every allocation this driver exposes to the device
     * (the EQ ring, the QP records, the send queue, the TX and RX CQ rings, the receive ring
     * and its buffers, the raw frame buffer) is released on the reset path on the strength of
     * one sentence: "the HCA was reset, so it has forgotten it".  That is a firmware-level
     * (HCA) reset, not a PCIe FLR, and the two are not the same statement -- the firmware
     * restarting does not prove the datapath's DMA engines have stopped referring to host
     * memory.  The other teardown branches in this file exist precisely because the device
     * "may still" own something, so the reset path was the one place resting on an assumption
     * rather than on the device's own answer.
     *
     * Two instruments, both of them readings rather than opinions:
     *
     *   (1) the SILENCE READOUT, in resetSilenceCheck().  At the reset, before anything is
     *       released, every block of host memory the device can reach is checksummed, then
     *       re-checksummed across a short bounded window.  A block that changes after the
     *       reset is a block the device is still writing to, and that is the fact this
     *       investigation has been missing: nothing in this driver has ever measured "the
     *       hardware has stopped" -- it has only ever seen "the reset command was accepted".
     *
     *   (2) the QUARANTINE (quarantineTake and friends): on the reset path the small
     *       device-facing blocks are NOT freed.  They are kept, filled with a pattern that no
     *       correctly transcribed descriptor could hold, and read back on every user-client
     *       entry.  A block that no longer holds the pattern was written by something that is
     *       not this driver -- and the report names which block, at which offset, and what
     *       the word changed from and to.  Keeping the memory is also the conservative
     *       choice: memory that is never handed back cannot be recycled under a device that
     *       is still writing to it.
     *
     * What this deliberately does NOT do: it does not quarantine the ICM context tables or
     * the firmware area.  Those are megabytes each and are released later (at the next
     * re-profile), not at the reset instant, so keeping them at every reset would leak tens
     * of megabytes per run for no extra signal -- the silence readout already samples them at
     * the same moment the small blocks are released.
     *
     * The leak is bounded (kQuarantineMax blocks / kQuarantineMaxBytes), is drained when the
     * driver stops, and the fallback when the zone is full is a logged real free rather than
     * a silent one.  cx3quarfree=1 turns the whole thing off for an A/B (Cx3Debug.h).
     */
    enum {
        kQuarantineMax      = 64,
        kQuarantineMaxBytes = 64u * 1024 * 1024,
        /*
         * Blocks that arrive after the sweeper's zone is full are still retained; this
         * second, never-swept ledger keeps them addressable so the retention is countable.
         * It is not an extension of the zone and nothing evicts into it.
         */
        kQuarantineOverflowMax = 32,
        /*
         * ICM chunks whose UNMAP_ICM / UNMAP_ICM_AUX the firmware did not confirm.  They stay
         * mapped and must not be freed; this ledger keeps them addressable so the leak is
         * enumerable and a later, proven-quiesce path can release them.
         */
        /*
         * Every table can contribute all eight chunks, and the quarantine and auxiliary
         * area can still arrive after them: 18*8 + 64 + 32 + 1 = 241.  Round up so neither
         * the unmap-refusal path nor the quarantine path can exhaust the ledger in one
         * lifecycle.  The overflow counter remains the guard for programming errors.
         */
        kIcmKeptMax = 256,
        /* How many device-facing blocks the silence readout samples.  collectDeviceBlocks()
         * refuses to overrun this and says so rather than sampling a prefix silently. */
        kDeviceBlockMax     = 64
    };
    struct QuarantineBlock {
        Mlx4Icm    *icm;          /* still allocated: this is the point of the exercise */
        const char *tag;          /* what it is, as it will be reported */
        UInt64      busLow;       /* first bus address, so the report names a machine address */
        UInt32      bytes;
        UInt32      pages;
        /*
         * A read-only digest taken when the block was retained.  The quarantine must not
         * write into memory whose owner is unknown, so there is no canary pattern: the
         * instrument is "same bytes as at the take", not "still holds a word we wrote".
         */
        UInt32      baselineSum;
        UInt32      baselineNonzero;
        UInt32      lastSum;      /* digest of the most recent sweep */
        UInt32      lastNonzero;
        Mlx4DmaGeneration generation; /* bookkeeping only; never a release credential */
        UInt32      sweeps;       /* how many times this block has been read back */
        UInt32      hits;         /* sweeps whose digest differed from the take */
        bool        reported;     /* "unchanged after N sweeps" has been said once */
        /*
         * The preconditions, recorded at the moment of the reset because they are what makes
         * a quiet window interpretable: a window in which nothing was posted is not evidence
         * that the device cannot write.  Kept here rather than in a local because the reading
         * that matters happens later, on a different selector call.
         */
        UInt32      txPosted;     /* send WQEs posted and not yet completed */
        UInt32      rxPosted;     /* receive slots currently posted */
        UInt32      qpsUp;        /* how many QPs were live at the reset */
        UInt32      ownerWord;    /* BAR0 ownership word immediately after the reset */
    };
    /* The silence readout's own view of one block: two sums, taken at two moments. */
    struct DeviceBlock {
        const char *tag;
        Mlx4Icm    *icm;
        UInt32      sum;          /* additive checksum over the block, in dwords */
        UInt32      nonzero;      /* non-zero bytes: catches "the firmware filled it" */
        UInt32      bytes;
    };

    Mlx4LifecycleGate fGate;
    struct IcmKeptBlock {
        Mlx4Icm *icm;
        UInt32   pages;
        const char *tag;
        Mlx4DmaGeneration generation;
    };
    IcmKeptBlock fIcmKept[kIcmKeptMax];
    UInt32       fIcmKeptCount;
    UInt32       fIcmKeptOverflow;   /* legacy diagnostic; new refusal keeps original slot */
    QuarantineBlock fQuar[kQuarantineMax];
    UInt32          fQuarCount;
    UInt64          fQuarBytes;
    UInt32          fQuarRefused;      /* blocks retained outside the sweeper's zone */
    Mlx4Icm        *fQuarOverflow[kQuarantineOverflowMax];
    Mlx4DmaGeneration fQuarOverflowGeneration[kQuarantineOverflowMax];
    UInt32          fQuarOverflowCount;
    UInt32          fQuarOverflowUntracked; /* legacy diagnostic; no new untracked transfer */
    bool            fQuarExhausted;    /* sticky: overflow, invalid view or ICM ledger refusal */
    bool            fResetUnproven;   /* reset attempted; no epoch-bound quiescence proof yet */
    bool            fResetCommandsBlocked; /* reset outcome cannot authorize device access */
    UInt64          fQuarSweeps;       /* user-client entries that read the zone back */
    UInt32          fQuarHits;         /* sweeps that found at least one modification */
    UInt32          fResetSilenceRuns;
    UInt32          fResetSilenceChanges;
    /*
     * What the reset looked like at the moment it was done, captured by
     * resetSilenceCheck() BEFORE any teardown runs -- so every quarantine entry can carry the
     * same preconditions rather than the flag state at the instant its own object was
     * released (which is later for some of them and would undercount).  Without these a
     * quiet window cannot be interpreted: "nothing was in flight" and "the device cannot
     * write" look identical in the entry log.
     */
    UInt32          fResetPreTxPosted;
    UInt32          fResetPreRxPosted;
    UInt32          fResetPreQpsUp;
    UInt32          fResetPreOwner;

    /* The one way to release a device-facing block on the reset path (see above). */
    bool     releaseIcmOrKeep(Mlx4Icm **slot, const char *tag, bool deviceMayStillHold);
    bool     quarantineTake(Mlx4Icm *icm, const char *tag);
    bool     resetRetentionPreflight(); /* caller holds selector transaction lock */
    bool     resetFreshStartEligible() const; /* empty host inventory, not quiescence proof */
    /*
     * Releasing retained memory requires an explicit proof that the device has been
     * quiesced.  "The driver is stopping" is not that proof, and passing false must leave
     * the blocks allocated (and say so) rather than freeing pages a device may still write.
     */
    /*
     * Why the driver believes the device has stopped touching host memory.
     *
     * Only a completed PCIe FLR (with a config-space read-back afterwards) counts as proof
     * today.  "The driver is stopping" is deliberately its own enumerator rather than `true`:
     * it is precisely the case that must NOT free anything, and a bool put that mistake one
     * keystroke away.
     */
    enum Cx3QuiesceProof {
        kCx3QuiesceNone     = 0,   /* no evidence at all */
        kCx3QuiesceStopping = 1,   /* driver exit; not a device-quiesce proof */
        kCx3QuiesceFlr      = 2    /* FLR completed and the device answered afterwards */
    };
    void     quarantineDrain(const char *why, Cx3QuiesceProof proof);
    /* Keep an ICM chunk the firmware refused to unmap, instead of dropping its pointer. */
    bool     keepIcmForQuiesce(Mlx4Icm *icm, const char *tag);

    UInt32   collectDeviceBlocks(DeviceBlock *out, UInt32 max, UInt32 *outBytes,
                                 bool scanBig);
    void     resetSilenceCheck();
    /*
     * Whether a provider source may be enabled at all.  False for a failed type
     * query and for anything not PCI-messaged, because the legacy path needs the
     * interrupt cleared at the device and that is not implemented.
     */
    bool     irqSourceAcceptable(int source, int type, IOReturn typeRc,
                                 UInt64 *outStatusOut);
    UInt32       fPciCommandAfterEnable;
    UInt32       fBarMapFlags;
};

#endif /* CX3_MLX4_H */
