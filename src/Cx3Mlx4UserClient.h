/*
 * Part of cx3-mlx4, a macOS IOKit port of the FreeBSD Mellanox driver.
 *
 * Original work in this repository.  It cites no upstream file:line and is
 * derived from none; the licence in LICENSE governs it.
 *
 * Copyright (c) 2026 Hi-Jiajun and contributors
 */

/*
 * Cx3Mlx4UserClient -- user-space control surface for the Cx3Mlx4 driver.
 *
 * Everything that writes to the device or allocates DMA lives here, reached
 * from user space on a running system.  Nothing on this path runs during boot.
 */
#ifndef CX3_MLX4_USER_CLIENT_H
#define CX3_MLX4_USER_CLIENT_H

#include <IOKit/IOUserClient.h>
#include <IOKit/IOLocks.h>

#include "cx3_abi.h"

class Cx3Mlx4;

class Cx3Mlx4UserClient : public IOUserClient {
    OSDeclareDefaultStructors(Cx3Mlx4UserClient)
    typedef IOUserClient super;

public:
    bool     initWithTask(task_t owningTask, void *securityID, UInt32 type,
                          OSDictionary *properties) override;
    bool     start(IOService *provider) override;
    void     stop(IOService *provider) override;
    void     free() override;
    IOReturn clientClose() override;

    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                            IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;

private:
    bool clientReady();
    /* Static trampolines, as required by IOExternalMethodDispatch. */
    static IOReturn sProbe(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sDma(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sQueryFw(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sResetHca(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sPeek(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sClearOwner(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sPoke(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sFlr(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sQueryDevCap(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sIcmAlloc(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sFwLoad(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sFwArea(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sFwOutbox(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sM2dProfile(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sM2dBringUp(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sM3EqUp(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sM3EqPoll(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sM3EqRing(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sM3EqInfo(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sM3IrqUp(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sM3IrqTest(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sM3IrqStatus(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sM3IrqDown(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sM3ClrInt(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sM3QueryPort(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sM3PortUp(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sM3AbRing(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sM3QueryHca(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sR4Mr(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sR4Cq(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sR4Rq(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sR4Qp(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sR4Ingress(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sR4Tx(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sR4Raw(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sTxProbe(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sNetUp(OSObject *target, void *ref, IOExternalMethodArguments *args);
    /* The interface step, cut short after its first step: see kCx3SelNetUpStop. */
    static IOReturn sNetUpStop(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sQueryAdapter(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sPcieLink(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sRuntimeIdentity(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sPortQueue(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sPortInfo(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sAutoNetwork(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sModuleRead(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sHcr(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sSurvey(OSObject *target, void *ref, IOExternalMethodArguments *args);
    /* kCx3SelFault: inject/clear the command-timeout states (docs/LOCK-IMPL.md). */
    static IOReturn sFault(OSObject *target, void *ref, IOExternalMethodArguments *args);

    IOReturn doProbe(IOExternalMethodArguments *args);
    IOReturn doDma(IOExternalMethodArguments *args);
    IOReturn doQueryFw(IOExternalMethodArguments *args);
    IOReturn doResetHca(IOExternalMethodArguments *args);
    IOReturn doPeek(IOExternalMethodArguments *args);
    IOReturn doClearOwner(IOExternalMethodArguments *args);
    IOReturn doPoke(IOExternalMethodArguments *args);
    IOReturn doFlr(IOExternalMethodArguments *args);
    IOReturn doQueryDevCap(IOExternalMethodArguments *args);
    IOReturn doIcmAlloc(IOExternalMethodArguments *args);
    IOReturn doFwLoad(IOExternalMethodArguments *args);
    IOReturn doFwArea(IOExternalMethodArguments *args);
    IOReturn doFwOutbox(IOExternalMethodArguments *args);
    IOReturn doM2dProfile(IOExternalMethodArguments *args);
    IOReturn doM2dBringUp(IOExternalMethodArguments *args);
    IOReturn doM3EqUp(IOExternalMethodArguments *args);
    IOReturn doM3EqPoll(IOExternalMethodArguments *args);
    IOReturn doM3EqRing(IOExternalMethodArguments *args);
    IOReturn doM3EqInfo(IOExternalMethodArguments *args);
    IOReturn doM3IrqUp(IOExternalMethodArguments *args);
    IOReturn doM3IrqTest(IOExternalMethodArguments *args);
    IOReturn doM3IrqStatus(IOExternalMethodArguments *args);
    IOReturn doM3IrqDown(IOExternalMethodArguments *args);
    IOReturn doM3ClrInt(IOExternalMethodArguments *args);
    IOReturn doM3QueryPort(IOExternalMethodArguments *args);
    IOReturn doM3PortUp(IOExternalMethodArguments *args);
    IOReturn doM3AbRing(IOExternalMethodArguments *args);
    IOReturn doM3QueryHca(IOExternalMethodArguments *args);
    IOReturn doR4Mr(IOExternalMethodArguments *args);
    IOReturn doR4Cq(IOExternalMethodArguments *args);
    IOReturn doR4Rq(IOExternalMethodArguments *args);
    IOReturn doR4Qp(IOExternalMethodArguments *args);
    IOReturn doR4Ingress(IOExternalMethodArguments *args);
    IOReturn doR4Tx(IOExternalMethodArguments *args);
    IOReturn doR4Raw(IOExternalMethodArguments *args);
    IOReturn doTxProbe(IOExternalMethodArguments *args);
    IOReturn doNetUp(IOExternalMethodArguments *args);
    IOReturn doNetUpStop(IOExternalMethodArguments *args);
    IOReturn doQueryAdapter(IOExternalMethodArguments *args);
    IOReturn doPcieLink(IOExternalMethodArguments *args);
    IOReturn doRuntimeIdentity(IOExternalMethodArguments *args);
    static IOReturn sNativePort(OSObject*,void*,IOExternalMethodArguments*);
    IOReturn doNativePort(IOExternalMethodArguments*);
    static IOReturn sNativeConfig(OSObject*,void*,IOExternalMethodArguments*);
    IOReturn doNativeConfig(IOExternalMethodArguments*);
    IOReturn doPortQueue(IOExternalMethodArguments *args);
    IOReturn doPortInfo(IOExternalMethodArguments *args);
    IOReturn doAutoNetwork(IOExternalMethodArguments *args);
    IOReturn doModuleRead(IOExternalMethodArguments *args);
    IOReturn doHcr(IOExternalMethodArguments *args);
    IOReturn doSurvey(IOExternalMethodArguments *args);
    /* kCx3SelNetStat -- the network data path's counters, read-only. */
    IOReturn doNetStat(IOExternalMethodArguments *args);
    /* kCx3SelNetLink -- media table and link re-reading (M5-1). */
    IOReturn doNetLink(IOExternalMethodArguments *args);
    /* kCx3SelNetOff -- the offload switches (M5-3). */
    IOReturn doNetOff(IOExternalMethodArguments *args);
    /* kCx3SelNetArm -- drain, arm, and look (M5-2's prerequisite). */
    IOReturn doNetArm(IOExternalMethodArguments *args);
    /* kCx3SelNetFilt -- the hardware receive filters (M5-8). */
    IOReturn doNetFilt(IOExternalMethodArguments *args);
    /* kCx3SelNetPort -- stop the port (the first half of any port reconfiguration). */
    IOReturn doNetPort(IOExternalMethodArguments *args);
    static IOReturn sNetPort(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sNetFilt(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sNetArm(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sNetOff(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sNetLink(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sNetStat(OSObject *target, void *ref, IOExternalMethodArguments *args);
    IOReturn doFault(IOExternalMethodArguments *args);

    /*
     * The dispatch table, declared without a size on purpose.
     *
     * One row per selector is required, and a short initialiser list is not a compile error
     * in C++: the missing rows are zero-filled, so a selector would dispatch through a null
     * function pointer instead of failing to build.  Declaring the bound here would make the
     * array the right size whatever the list contains -- a check on the declaration and not
     * on the thing that gets edited.  Unsized, its size comes from the definition in the .cpp
     * and the assertion there (in externalMethod, which can see the private member) is a real
     * check: dropping a row stops the build.  Verified by construction, like the scalar cap
     * assertions in cx3_abi.h.
     */
    static const IOExternalMethodDispatch sMethods[];

    task_t       fTask;
    Cx3Mlx4 *    fProvider;
    /* Provider is immutable after successful start and retained until free.
     * stop only closes this short admission gate; handlers never race a clear. */
    IOLock *     fClientLock;
    bool         fClientStopping;
};

#endif /* CX3_MLX4_USER_CLIENT_H */
