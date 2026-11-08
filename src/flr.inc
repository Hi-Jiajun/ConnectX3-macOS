/*
 * Included by Cx3Mlx4.cpp.  Kept separate only to keep that file navigable.
 *
 * PCIe Function Level Reset through the public platform transaction. This
 * diagnostic entry does not issue a generation/scope-bound release credential.
 */

#define PCI_EXP_DEVCAP   0x04   /* Device Capabilities, relative to the cap base */
#define PCI_EXP_DEVCTL   0x08   /* Device Control + Device Status                 */
#define PCI_EXP_FLR_CAP  28     /* Function Level Reset Capability, in Device Caps  */

IOReturn Cx3Mlx4::exportFlr(UInt64 *out)
{
    UInt8  cap;
    UInt32 devCaps, devCtlPre;
    UInt32 bar0Before, bar0After;
    UInt32 barHiBefore, idBefore, idAfter, commandAfter;
    Mlx4PciIdentity identityBefore, identityAfter;

    cap        = 0;
    devCaps    = 0;
    devCtlPre  = 0;
    bar0Before = 0;
    bar0After  = 0;
    identityBefore.clear();
    identityAfter.clear();

    memset(out, 0, sizeof(UInt64) * kFlrOutCount);
    out[kFlrOutOwner] = 0xffffffffull; // old MMIO mapping is never used here

    /* FLR is another reset entry, not a way around reset's retention and
     * local CPU-lifecycle preconditions. The caller holds the transaction
     * lock. This is admission only, not a platform/epoch quiescence proof. */
    if (fIrqObjectsUp || fIrqSource || !fStartPciIdentityValid ||
        !resetRetentionPreflight()) {
        out[kFlrOutStatus] = (UInt64)kIOReturnNoSpace;
        LOG("FLR refused before platform reset: reset retention/lifecycle preflight "
            "or interrupt-session preconditions are not established");
        return kIOReturnSuccess;
    }

    cap = fHw.findPcieCapability();

    if (!cap) {
        LOG("FLR refused: no PCI Express capability found");
        out[kFlrOutStatus] = (UInt64)kIOReturnUnsupported;
        return kIOReturnSuccess;
    }

    devCaps = fHw.configRead((UInt8)(cap + PCI_EXP_DEVCAP));
    out[kFlrOutDevCaps] = devCaps;
    out[kFlrOutSupported] = ((devCaps >> PCI_EXP_FLR_CAP) & 1u) ? 1 : 0;

    if (!out[kFlrOutSupported]) {
        LOG("FLR refused: Device Capabilities 0x%08x has bit 28 clear, so this "
            "function does not support it", (unsigned)devCaps);
        out[kFlrOutStatus] = (UInt64)kIOReturnUnsupported;
        return kIOReturnSuccess;
    }

    /* Configuration snapshots are diagnostics only. Even unchanged addresses
     * do not establish a fresh MMIO view or generation after platform reset. */
    bar0Before      = fHw.configRead(kIOPCIConfigBaseAddress0);
    barHiBefore     = fHw.configRead(kIOPCIConfigBaseAddress0 + 4);
    idBefore        = fHw.configRead(kIOPCIConfigVendorID);
    devCtlPre       = fHw.configRead((UInt8)(cap + PCI_EXP_DEVCTL)) & 0xffff;
    out[kFlrOutBar0Before] = bar0Before;
    out[kFlrOutDevCtlPre]  = devCtlPre;

    LOG("FLR starting: pcie cap 0x%02x devcaps 0x%08x devctl 0x%04x bar0 0x%08x",
        (unsigned)cap, (unsigned)devCaps, (unsigned)devCtlPre,
        (unsigned)bar0Before);

    const UInt32 commandBefore = fHw.configRead(kIOPCIConfigCommand) & 0xffff;
    const IOReturn identityStatus = fHw.readPciIdentity(&identityBefore);
    const bool identityBeforeValid = idBefore != 0 && idBefore != 0xffffffffu &&
        (idBefore & 0xffffu) != 0xffffu && identityStatus == kIOReturnSuccess &&
        identityBefore.sameFunction(fStartPciIdentity);
    const bool barBeforeValid = bar0Before != 0xffffffffu &&
        (bar0Before & ~0xfu) != 0;
    if (!identityBeforeValid || !barBeforeValid || commandBefore == 0xffffu ||
        !(commandBefore & 2u)) {
        out[kFlrOutStatus] = (UInt64)kIOReturnNotReady;
        LOG("FLR refused before platform reset: PCI identity, BAR0, or memory decode is invalid");
        return kIOReturnSuccess;
    }
    if (fDeviceGeneration == ~0ull) {
        out[kFlrOutStatus] = (UInt64)kIOReturnNoSpace;
        LOG("FLR refused: provider generation counter exhausted; no platform reset attempted");
        return kIOReturnSuccess;
    }

    /* Success is not a release credential; failure may have affected hardware
     * too. Both latches remain set and no software bit-write fallback exists.
     * The platform may affect associated functions: scope remains unproven. */
    ++fDeviceGeneration;
    fHw.setDeviceGeneration(fDeviceGeneration);
    for (UInt32 i = 0; i < 2; ++i) fPorts[i].newGeneration(fDeviceGeneration);
    fResetUnproven = true;
    fResetCommandsBlocked = true;
    IOReturn resetStatus = fHw.platformFunctionReset();
    if (resetStatus != kIOReturnSuccess) {
        out[kFlrOutStatus] = (UInt64)resetStatus;
        LOG("FLR platform reset failed: 0x%x; no post-reset device access", resetStatus);
        return kIOReturnSuccess;
    }

    out[kFlrOutBar0After] = bar0After = fHw.configRead(kIOPCIConfigBaseAddress0);
    idAfter = fHw.configRead(kIOPCIConfigVendorID);
    const IOReturn identityAfterStatus = fHw.readPciIdentity(&identityAfter);
    commandAfter = fHw.configRead(kIOPCIConfigCommand) & 0xffff;
    out[kFlrOutVendorId]  = idAfter & 0xffff;
    out[kFlrOutDeviceId]  = (idAfter >> 16) & 0xffff;
    out[kFlrOutBarStable] = (bar0After == bar0Before &&
        fHw.configRead(kIOPCIConfigBaseAddress0 + 4) == barHiBefore) ? 1 : 0;

    LOG("FLR done: bar0 0x%08x -> 0x%08x (%s), vendor:device %04llx:%04llx",
        (unsigned)bar0Before, (unsigned)bar0After,
        out[kFlrOutBarStable] ? "unchanged" : "CHANGED",
        (unsigned long long)out[kFlrOutVendorId],
        (unsigned long long)out[kFlrOutDeviceId]);

    const bool ready = idAfter == idBefore && identityAfterStatus == kIOReturnSuccess &&
        identityAfter.sameFunction(identityBefore) &&
        out[kFlrOutBarStable] && commandAfter != 0xffff && (commandAfter & 2u);
    out[kFlrOutStatus] = ready ? (UInt64)kIOReturnSuccess : (UInt64)kIOReturnNotReady;

    /* Preserve every owner/poison and old HCA state as unproven, not valid for
     * reuse. Admission remains blocked even on success. Matching config is a
     * diagnostic only: no old BAR MMIO, free, UNMAP or automatic restart. R06
     * still needs generation/scope/CPU drainage and fresh mapping/rebuild. */
    return kIOReturnSuccess;
}
