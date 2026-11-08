/*
 * cx3ctl -- user-space control tool for the Cx3Mlx4 kext.
 *
 * Reaching the device from here instead of from IOKit start() means a fault
 * costs a reboot rather than a bootable machine, and the steps below can be run
 * in sequence without rebooting in between.
 *
 * Build:  clang -o cx3ctl cx3ctl.c -framework IOKit -framework CoreFoundation
 * Run as root.
 *
 *   cx3ctl probe            read-only registers; safe, always start here
 *   cx3ctl dma              allocate the command mailbox, no device access
 *   cx3ctl queryfw [force]  HCR handshake + QUERY_FW   <-- writes the device
 *   cx3ctl reset            HCA reset                  <-- can need a power cycle
 *   cx3ctl peek <space> <offset> <count>   read-only dump of any range
 *   cx3ctl clearowner       writel(0) to the ownership word (reference op)
 *   cx3ctl poke <space> <offset> <value>   one guarded store + read-back
 *   cx3ctl flr             PCIe Function Level Reset (hardware reset)
 *   cx3ctl all              probe, then dma, then queryfw
 *   cx3ctl querydevcap [force]   QUERY_DEV_CAP: the device's capability set
 *   cx3ctl icmalloc <pages>      allocate the FW/ICM area (no device access)
 *   cx3ctl icmfree               free it
 *   cx3ctl fwload <fw_pages>     M2b: allocate FW area, MAP_FA, RUN_FW, MOD_STAT_CFG
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <IOKit/IOKitLib.h>

#include "../src/cx3_abi.h"
/* Pure #define file, no kernel dependencies, so the tool can share the
 * QUERY_DEV_CAP byte offsets with the kext instead of repeating them. */
#include "../src/mlx4_regs.h"

#define KEXT_CLASS "Cx3Mlx4"

static io_connect_t gConn = 0;

/* Defined with the other status helpers below; declared here because the oldest
 * subcommands (queryfw) print firmware statuses too. */
static const char *fwStatusName(unsigned long long st);

/*
 * The breadcrumb channel: one UDP datagram per step, to a host named in the environment.
 *
 * Why it exists, and it is measured rather than imagined (2026-10-13): the machine died inside a
 * call to selector 39 five times, and in two of those runs the DRIVER LOGGED NOTHING -- not even
 * `user client opened`, which is the last thing newUserClient does before returning.  Two
 * explanations fit that evidence equally well:
 *
 *   (a) the machine died in the open path, before any of this driver's first lines; or
 *   (b) those lines were produced but logd never pushed them off the machine in the ~0.6 s it had.
 *
 * No amount of reading the driver's own log can separate them -- the log is the thing in doubt.
 * This can, because it uses neither logd nor the disk: the recorder on the LAN VM timestamps
 * whatever arrives on UDP 9999 (the parallel forensics session's black box), so the last
 * breadcrumb says how far the TOOL got even if the kernel said nothing at all.  "open returned
 * 0x0" followed by silence means the death is after the open and before the selector's first
 * line; no "open returned" at all means the death is inside the open path.
 *
 * Off unless CX3_BC=host:port is set, so no ordinary run pays for it and no measurement changes
 * because the instrument is present.
 */
static int breadcrumb(const char *fmt, ...)
{
    static struct sockaddr_in bcAddr;
    static int bcFd = -2;               /* -2 = not looked at yet; -1 = disabled */
    char buf[256];
    va_list ap;
    int n;

    if (bcFd == -2) {
        const char *spec = getenv("CX3_BC");
        bcFd = -1;
        if (spec && *spec) {
            char host[128];
            const char *colon = strrchr(spec, ':');
            if (colon && (size_t)(colon - spec) < sizeof(host)) {
                int port;
                memcpy(host, spec, (size_t)(colon - spec));
                host[colon - spec] = 0;
                port = atoi(colon + 1);
                memset(&bcAddr, 0, sizeof(bcAddr));
                bcAddr.sin_family = AF_INET;
                bcAddr.sin_port = htons((uint16_t)port);
                if (port > 0 && inet_pton(AF_INET, host, &bcAddr.sin_addr) == 1)
                    bcFd = socket(AF_INET, SOCK_DGRAM, 0);
            }
        }
    }
    if (bcFd < 0) return -1;
    va_start(ap, fmt);
    /*
     * A trailing newline, because the recorder on the other end appends what arrives to a file:
     * without it every datagram runs into the next one and the file becomes one unreadable line
     * (measured on the first self-test: five datagrams, one line).
     */
    n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n >= sizeof(buf) - 1)
        n = (int)(sizeof(buf) - 2);
    buf[n] = '\n';
    ++n;
    if (sendto(bcFd, buf, (size_t)n, 0,
               (struct sockaddr *)&bcAddr, sizeof(bcAddr)) < 0)
        return -1;
    return 0;
}

static int openService(void)
{
    io_service_t svc;
    kern_return_t kr;
    CFMutableDictionaryRef match;

    if (gConn) return 0;

    breadcrumb("open: matching entry");
    match = IOServiceMatching(KEXT_CLASS);
    if (!match) {
        fprintf(stderr, "IOServiceMatching failed\n");
        breadcrumb("open: IOServiceMatching failed");
        return -1;
    }
    svc = IOServiceGetMatchingService(kIOMainPortDefault, match);
    if (!svc) {
        fprintf(stderr, "%s: no matching service found - is the kext loaded?\n",
                KEXT_CLASS);
        breadcrumb("open: no matching service");
        return -1;
    }

    breadcrumb("open: calling IOServiceOpen");
    kr = IOServiceOpen(svc, mach_task_self(), 0, &gConn);
    IOObjectRelease(svc);
    breadcrumb("open: IOServiceOpen returned 0x%x", (unsigned)kr);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceOpen failed: 0x%x %s\n", kr,
                kr == kIOReturnNotPrivileged ? "(run as root)" : "");
        gConn = 0;
        return -1;
    }
    return 0;
}

static int cmdBreadcrumbTest(void)
{
    const char *spec = getenv("CX3_BC");
    int saved_errno;

    if (!spec || !*spec) {
        fprintf(stderr, "bctest: CX3_BC is not set\n");
        return 2;
    }
    errno = 0;
    if (breadcrumb("bctest: cx3ctl breadcrumb") != 0) {
        saved_errno = errno;
        fprintf(stderr, "bctest: sendto failed to %s: %s\n",
                spec, saved_errno ? strerror(saved_errno) : "breadcrumb channel disabled");
        return 1;
    }
    printf("bctest: sendto accepted for %s\n", spec);
    return 0;
}

static int cmdOpenTest(void)
{
    breadcrumb("open-test: entering");
    if (openService() != 0) {
        breadcrumb("open-test: open failed");
        return 1;
    }
    breadcrumb("open-test: open returned; closing");
    IOServiceClose(gConn);
    gConn = 0;
    breadcrumb("open-test: closed");
    printf("open-test: PASS (IOServiceOpen + clientClose completed)\n");
    return 0;
}

static int call(uint32_t selector, const uint64_t *in, uint32_t inCount,
                uint64_t *out, uint32_t *outCount)
{
    kern_return_t kr;

    if (openService() != 0) return -1;

    breadcrumb("call: selector %u in %u out %u (scalar path)", (unsigned)selector,
               (unsigned)inCount, (unsigned)(outCount ? *outCount : 0));
    kr = IOConnectCallScalarMethod(gConn, selector, in, inCount, out, outCount);
    breadcrumb("call: selector %u returned 0x%x (scalar path)", (unsigned)selector,
               (unsigned)kr);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "selector %u failed: 0x%x\n", selector, kr);
        return -1;
    }
    return 0;
}

/*
 * A selector that also returns a structure.  io_connect_method needs the structure
 * size in and out; the scalar path is unchanged, and keeping the two helpers separate
 * means the struct-taking selectors cannot be called through the wrong one.
 */
static int callStruct(uint32_t selector, const uint64_t *in, uint32_t inCount,
                      uint64_t *out, uint32_t *outCount, void *res, size_t resSize)
{
    kern_return_t kr;
    size_t sz = resSize;

    if (openService() != 0) return -1;

    breadcrumb("call: selector %u in %u out %u (structured path)", (unsigned)selector,
               (unsigned)inCount, (unsigned)(outCount ? *outCount : 0));
    kr = IOConnectCallMethod(gConn, selector, in, inCount, NULL, 0, out, outCount,
                             res, &sz);
    breadcrumb("call: selector %u returned 0x%x (structured path)", (unsigned)selector,
               (unsigned)kr);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "selector %u failed: 0x%x\n", selector, kr);
        return -1;
    }
    if (sz != resSize) {
        fprintf(stderr, "selector %u returned %zu bytes of structure, expected %zu\n",
                selector, sz, resSize);
        return -1;
    }
    return 0;
}

static void fwBytes(uint64_t packed, unsigned *major, unsigned *minor,
                    unsigned *patch)
{
    /* mlx4_fw.c packs the version the way the driver stores dev->caps.fw_ver. */
    *major = (unsigned)(packed >> 32);
    *minor = (unsigned)((packed >> 16) & 0xffff);
    *patch = (unsigned)(packed & 0xffff);
}

static int cmdProbe(void)
{
    uint64_t out[kProbeOutCount];
    uint32_t n = kProbeOutCount;

    memset(out, 0, sizeof(out));
    if (call(kCx3SelProbe, NULL, 0, out, &n) != 0) return -1;

    printf("probe:\n");
    printf("  status        : %llu\n", (unsigned long long)out[kProbeOutStatus]);
    printf("  window alive  : %s\n", out[kProbeOutWindow] ? "YES" : "NO");
    printf("  owner word    : 0x%08llx\n", (unsigned long long)out[kProbeOutOwner]);
    printf("  HCR status    : 0x%08llx\n", (unsigned long long)out[kProbeOutHcrStat]);
    printf("  reset sem     : 0x%08llx\n", (unsigned long long)out[kProbeOutResetSem]);
    printf("  PCI COMMAND   : 0x%04llx\n", (unsigned long long)out[kProbeOutPciCmd]);
    printf("  BAR0 physical : 0x%llx\n", (unsigned long long)out[kProbeOutBar0Phys]);
    printf("  mapped        : bar0=%s bar2=%s\n",
           (out[kProbeOutMapFlags] & 1) ? "yes" : "no",
           (out[kProbeOutMapFlags] & 2) ? "yes" : "no");
    /*
     * The command mailbox's poison bit.  Printed here rather than only under a new
     * subcommand because probe is the first thing the recovery procedure runs, and this is
     * the answer to "why did that command fail at once instead of timing out?" -- the
     * failure it explains is otherwise indistinguishable from a device that answered
     * "no" (docs/LOCK-IMPL.md).
     */
    printf("  cmd mailbox   : %s\n",
           out[kProbeOutCmdWedged] ? "POISONED - commands are refused before they are posted; "
                                     "an HCA reset or an FLR clears it"
                                   : "usable");

    if (!out[kProbeOutWindow]) {
        printf("  -> window is not live; stop here, the command path would be "
               "meaningless\n");
    }
    return 0;
}

static int cmdDma(void)
{
    uint64_t out[kDmaOutCount];
    uint32_t n = kDmaOutCount;

    memset(out, 0, sizeof(out));
    printf("dma: allocating the command mailbox (no device registers touched)\n");
    fflush(stdout);
    if (call(kCx3SelDma, NULL, 0, out, &n) != 0) return -1;

    printf("  status     : %llu\n", (unsigned long long)out[kDmaOutStatus]);
    printf("  phys addr  : 0x%llx\n", (unsigned long long)out[kDmaOutPhys]);
    printf("  segments   : %llu\n", (unsigned long long)out[kDmaOutSegCount]);
    printf("  below 4GiB : %s\n", out[kDmaOutLow4G] ? "yes" : "NO (would refuse)");
    printf("  offset     : %llu\n", (unsigned long long)out[kDmaOutOffset]);
    return 0;
}

static int cmdQueryFw(int force)
{
    uint64_t in[kFwInCount];
    uint64_t out[kFwOutCount];
    uint32_t n = kFwOutCount;
    unsigned major, minor, patch;

    in[kFwInForce] = force ? 1 : 0;
    memset(out, 0, sizeof(out));

    printf("queryfw: HCR handshake + QUERY_FW%s\n",
           force ? " (force=1, ownership gate bypassed)" : "");
    printf("  note: this writes the HCA command register. If the machine hangs,\n"
           "        power-cycle the card-host and re-run 'probe' first.\n");
    fflush(stdout);

    if (call(kCx3SelQueryFw, in, kFwInCount, out, &n) != 0) return -1;

    printf("  status      : %llu\n", (unsigned long long)out[kFwOutStatus]);
    if (out[kFwOutFwStatus] == 0xffffffffull) {
        printf("  fw status   : not attempted (see the kernel log for the reason)\n");
        return 0;
    }
    printf("  fw status   : 0x%llx  %s\n", (unsigned long long)out[kFwOutFwStatus],
           fwStatusName(out[kFwOutFwStatus]));
    if (out[kFwOutStatus] != 0) return 0;

    fwBytes(out[kFwOutFwVer], &major, &minor, &patch);
    printf("  fw version  : %u.%u.%03u\n", major, minor, patch);
    printf("  cmd if rev  : %llu\n", (unsigned long long)out[kFwOutCmdIfRev]);
    printf("  max cmds    : %llu\n", (unsigned long long)out[kFwOutMaxCmds]);
    printf("  ppf id      : %llu\n", (unsigned long long)out[kFwOutPpfId]);
    printf("  catas       : off=0x%llx size=0x%llx bar=%llu\n",
           (unsigned long long)out[kFwOutCatasOff],
           (unsigned long long)out[kFwOutCatasSize],
           (unsigned long long)out[kFwOutCatasBar]);
    printf("  fw pages    : %llu (0x%llx) -> %llu KiB of FW area to allocate\n",
           (unsigned long long)out[kFwOutFwPages],
           (unsigned long long)out[kFwOutFwPages],
           (unsigned long long)out[kFwOutFwPages] * 4);
    /*
     * outbox word 0 is {u16 fw_pages, first half of the version}, per
     * QUERY_FW_SIZE_OFFSET in mlx4_regs.h.  Printing both halves makes the
     * offset question checkable from the tool's own output instead of from the
     * kernel log, and a disagreement between the two is the signal that the
     * page count above is not the device's answer.
     */
    {
        unsigned long long w0 = (unsigned long long)out[kFwOutRaw0];
        unsigned be16lo = (unsigned)(w0 >> 16);        /* BE16 at offset 0x00 */
        printf("  outbox@0x00 : 0x%04x = %u pages = %u KiB (the reference reads the "
               "FW size here)\n", be16lo, be16lo, be16lo * 4);
    }
    printf("  outbox[0..3]: %08llx %08llx %08llx %08llx\n",
           (unsigned long long)out[kFwOutRaw0],
           (unsigned long long)out[kFwOutRaw1],
           (unsigned long long)out[kFwOutRaw2],
           (unsigned long long)out[kFwOutRaw3]);
    /* outbox dwords 4..7 are no longer returned: fwPages parses offset 0x10
     * directly, and carrying all four cost two scalar slots too many. */
    return 0;
}

static int cmdReset(int bypassSem)
{
    uint64_t in[kRstInCount];
    uint64_t out[kRstOutCount];
    uint32_t n = kRstOutCount;

    in[kRstInConfirm]   = 1;
    in[kRstInBypassSem] = bypassSem ? 1 : 0;
    memset(out, 0, sizeof(out));

    printf("reset: HCA reset. This is the one action that can leave the card\n"
           "       needing a full power cycle to come back.\n");
    if (bypassSem)
        printf("       Semaphore gate BYPASSED on request: 0xf03fc reads a constant\n"
               "       0x01000000 and ignores writes, so it gates nothing.\n");
    fflush(stdout);
    if (call(kCx3SelResetHca, in, kRstInCount, out, &n) != 0) return -1;

    printf("  status       : %llu\n", (unsigned long long)out[kRstOutStatus]);
    printf("  owner before : 0x%08llx\n", (unsigned long long)out[kRstOutOwnerBefore]);
    printf("  owner after  : 0x%08llx\n", (unsigned long long)out[kRstOutOwnerAfter]);
    /*
     * Was the command mailbox poisoned when this arrived?  Not decoration: this reset is
     * the cure for that state, so the pair "poisoned before = 1" and "commands work after"
     * is the evidence that the recovery path works, and "poisoned before = 0" with commands
     * still refused points somewhere else entirely (docs/LOCK-IMPL.md, question 3).
     */
    printf("  cmd mailbox  : %s\n",
           out[kRstOutCmdWedged] ? "was POISONED before this reset (this reset is its cure)"
                                 : "was not poisoned before this reset");
    /*
     * A REFUSED reset must not exit 0.
     *
     * The selector reports a refusal in out[status] and the line above prints it, but the
     * exit code said "fine" -- so a shell chain like
     *
     *     cx3ctl reset force && cx3ctl fwload 1542 && ...
     *
     * carried on as though the HCA were fresh.  It was not: an interrupt session was still
     * open, the reset had been refused on purpose (section 28.2), and the next step armed
     * MAP_FA against a card that still had its tables mapped.  The command register ended
     * up stuck with GO set and every later command timed out, which cost a reboot.
     *
     * The general shape: the selector layer represented the failure correctly and the TOOL
     * layer threw the representation away.  Same family as the timeout reported as firmware
     * status 0, one level up -- and it is the level scripts actually read.
     */
    if (out[kRstOutStatus] != 0) {
        fprintf(stderr, "\n  The reset did NOT happen (status 0x%llx). Nothing after this "
                        "in a shell chain\n  should run: the HCA is still in whatever state "
                        "it was in before.\n",
                (unsigned long long)out[kRstOutStatus]);
        if (out[kRstOutStatus] == CX3_IORETURN_NOT_READY)
            fprintf(stderr, "  kIOReturnNotReady means the selector refused -- most often "
                            "an interrupt\n  session is still open. Run 'cx3ctl m3irqdown' "
                            "first.\n");
        return 3;
    }
    return 0;
}

static const char *fwStatusName(unsigned long long st)
{
    switch (st) {
    /*
     * A firmware status of 0 means "the firmware said ok", so "there was no
     * firmware answer" must never print as 0.  M2b once reported a RUN_FW
     * timeout as `fw status 0x0 ok` and it read as success; these are the values
     * that replaced it (src/mlx4_regs.h, CX3_FWSTATUS_*).
     */
    case CX3_FWSTATUS_TIMEOUT:    return "TIMED OUT - the command never completed";
    case CX3_FWSTATUS_NOT_POSTED: return "not posted - no firmware answer";
    case CX3_FWSTATUS_NO_DEVICE:  return "no HCR handle";
    case CX3_FWSTATUS_WEDGED:     return "NOT SENT - the command mailbox is poisoned by an "
                                         "earlier abandonment; an HCA reset or an FLR is "
                                         "required";
#define CX3_CASE(code, name, text) case code: return text;
    CX3_FW_STATUS_LIST(CX3_CASE)
#undef CX3_CASE
    default: return "unknown";
    }
}

static const char *spaceName(unsigned s)
{
    switch (s) {
    case kPeekSpaceBar0:   return "BAR0";
    case kPeekSpaceBar2:   return "BAR2";
    case kPeekSpaceConfig: return "config";
    default:               return "?";
    }
}

static int cmdPeek(unsigned space, unsigned long long off, unsigned count)
{
    uint64_t in[3];
    uint64_t out[kPeekOutCount_];
    uint32_t n = kPeekOutCount_;
    unsigned i;

    in[kPeekInSpace]  = space;
    in[kPeekInOffset] = off;
    in[kPeekInCount]  = count;
    memset(out, 0, sizeof(out));

    if (call(kCx3SelPeek, in, 3, out, &n) != 0) return -1;
    if (out[kPeekOutStatus] != 0) {
        fprintf(stderr, "peek refused: status %llu\n",
                (unsigned long long)out[kPeekOutStatus]);
        return -1;
    }

    printf("peek %s offset 0x%llx, %llu dword(s):\n", spaceName(space), off,
           (unsigned long long)out[kPeekOutCount]);
    for (i = 0; i < (unsigned)out[kPeekOutCount]; ++i) {
        if (i % 4 == 0) {
            printf("  +0x%05llx ", off + (unsigned long long)i * 4);
        }
        printf("%08llx ", (unsigned long long)out[kPeekOutData0 + i]);
        if (i % 4 == 3 || i + 1 == (unsigned)out[kPeekOutCount]) printf("\n");
    }
    return 0;
}

static int cmdClearOwner(void)
{
    uint64_t in[kClrInCount];
    uint64_t out[kClrOutCount];
    uint32_t n = kClrOutCount;

    in[kClrInConfirm] = 1;
    memset(out, 0, sizeof(out));

    printf("clearowner: writel(0) to the ownership word (mlx4_free_ownership).\n"
           "            single 32-bit store, no handshake, no polling.\n");
    fflush(stdout);
    if (call(kCx3SelClearOwner, in, kClrInCount, out, &n) != 0) return -1;

    printf("  status : %llu\n", (unsigned long long)out[kClrOutStatus]);
    printf("  before : 0x%08llx\n", (unsigned long long)out[kClrOutBefore]);
    printf("  after  : 0x%08llx  %s\n", (unsigned long long)out[kClrOutAfter],
           out[kClrOutAfter] == 0 ? "(cleared)" : "(STILL SET)");
    return 0;
}

static int cmdPoke(unsigned space, unsigned long long off,
                   unsigned long long val)
{
    uint64_t in[kPokeInCount];
    uint64_t out[kPokeOutCount];
    uint32_t n = kPokeOutCount;

    in[kPokeInSpace]   = space;
    in[kPokeInOffset]  = off;
    in[kPokeInValue]   = val;
    in[kPokeInConfirm] = 1;
    memset(out, 0, sizeof(out));

    printf("poke %s 0x%llx = 0x%llx  (one 32-bit store, read back after)\n",
           spaceName(space), off, val);
    printf("  refused by the kext: 0xf0010 (HCA reset) and 0x80698 (HCR control)\n");
    fflush(stdout);
    if (call(kCx3SelPoke, in, kPokeInCount, out, &n) != 0) return -1;

    if (out[kPokeOutStatus] != 0) {
        fprintf(stderr, "  refused: status %llu\n",
                (unsigned long long)out[kPokeOutStatus]);
        return -1;
    }
    printf("  before : 0x%08llx\n", (unsigned long long)out[kPokeOutBefore]);
    printf("  wrote  : 0x%08llx\n", (unsigned long long)out[kPokeOutValue]);
    printf("  after  : 0x%08llx  %s\n", (unsigned long long)out[kPokeOutAfter],
           out[kPokeOutAfter] == out[kPokeOutValue]
               ? "(STORED - writes reach the device)"
               : "(did not take)");
    return 0;
}

static int cmdFlr(void)
{
    uint64_t in[kFlrInCount];
    uint64_t out[kFlrOutCount];
    uint32_t n = kFlrOutCount;

    in[kFlrInConfirm] = 1;
    memset(out, 0, sizeof(out));

    printf("flr: PCIe Function Level Reset.\n");
    printf("  Hardware-executed function reset, the mechanism the PCIe spec\n");
    printf("  defines for a device whose driver is stuck. Does not touch the\n");
    printf("  vendor reset handshake or the firmware semaphore.\n");
    printf("  The kext refuses if this function does not advertise support.\n\n");
    fflush(stdout);

    if (call(kCx3SelFlr, in, kFlrInCount, out, &n) != 0) return -1;

    printf("  status        : %llu\n", (unsigned long long)out[kFlrOutStatus]);
    printf("  device caps   : 0x%08llx\n", (unsigned long long)out[kFlrOutDevCaps]);
    printf("  FLR supported : %s\n", out[kFlrOutSupported] ? "YES" : "NO");
    if (!out[kFlrOutSupported]) return out[kFlrOutStatus] ? -1 : 0;

    printf("  devctl before : 0x%04llx\n", (unsigned long long)out[kFlrOutDevCtlPre]);
    printf("  BAR0 before   : 0x%08llx\n", (unsigned long long)out[kFlrOutBar0Before]);
    printf("  BAR0 after    : 0x%08llx  %s\n",
           (unsigned long long)out[kFlrOutBar0After],
           out[kFlrOutBarStable] ? "(unchanged)" : "(CHANGED - reboot needed)");
    printf("  ids after     : %04llx:%04llx\n",
           (unsigned long long)out[kFlrOutVendorId],
           (unsigned long long)out[kFlrOutDeviceId]);

    if (out[kFlrOutOwner] == 0xffffffffull) {
        printf("  ownership     : not read (no trusted post-reset MMIO view)\n");
    } else if (out[kFlrOutBarStable]) {
        printf("  ownership     : 0x%08llx  %s\n",
               (unsigned long long)out[kFlrOutOwner],
               out[kFlrOutOwner] == 0 ? "(CLEARED - gate gone)"
                                      : "(still set)");
    } else {
        printf("  ownership     : not read (BAR0 moved, mapping untrusted)\n");
        printf("  -> reboot, then run 'probe'\n");
    }
    printf("  recovery      : device commands remain blocked; no release credential\n");
    return out[kFlrOutStatus] ? -1 : 0;
}

/*
 * QUERY_DEV_CAP.  The kext hands back the raw outbox; the parsing lives here so
 * that reading one more field does not require a new kext binary (and therefore
 * a reboot).
 */
static int cmdQueryDevCap(int force, int ignoreBusy)
{
    uint64_t in[kDevCapInCount];
    uint64_t out[kDevCapOutCount];
    uint32_t outCount = kDevCapOutCount;
    struct Cx3DevCapResult res;
    unsigned char *b;
    unsigned char be[kDevCapWords * 4];
    unsigned i;
    kern_return_t kr;

    in[kDevCapInForce] = force ? 1 : 0;
    in[kDevCapInIgnoreBusy] = ignoreBusy ? 1 : 0;
    memset(out, 0, sizeof(out));
    memset(&res, 0, sizeof(res));

    printf("querydevcap: QUERY_DEV_CAP (0x3)%s%s\n",
           force ? " (force=1, ownership gate bypassed)" : "",
           ignoreBusy ? " (ignorebusy=1, posting over a busy command register)" : "");
    printf("  (the outbox comes back as a structure: the scalar path caps at 16\n"
           "   values and this result needs 24 dwords plus two status words)\n");
    fflush(stdout);

    if (openService() != 0) return -1;

    kr = IOConnectCallMethod(gConn, kCx3SelQueryDevCap,
                             in, kDevCapInCount,     /* scalar input  */
                             NULL, 0,                /* struct input  */
                             out, &outCount,         /* scalar output */
                             &res, &(size_t){sizeof(res)});  /* struct output */
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "selector %u failed: 0x%x\n", kCx3SelQueryDevCap, kr);
        return -1;
    }

    if (res.fwStatus == 0xffffffffull) {
        /* The kext never posted the command; it says why in the kernel log. */
        printf("  NOT ATTEMPTED by the kext (status 0x%llx) - see the kernel log\n",
               (unsigned long long)res.status);
        return 0;
    }
    printf("  fw status : 0x%llx  %s\n", (unsigned long long)res.fwStatus,
           fwStatusName(res.fwStatus));
    if (res.fwStatus != 0) {
        printf("  -> the firmware accepted the command and rejected it.\n");
        return 0;
    }

    /*
     * The outbox is a big-endian byte stream and the kext already converted each
     * dword to a host-order value, so the bytes cannot be read straight out of
     * `words[]` memory: on a little-endian host that reverses every dword,
     * single-byte fields included.  Rebuild the stream in the order the device
     * wrote it and index that; the byte offsets from mlx4_regs.h -- which are the
     * reference's own -- then apply unchanged.
     */
    for (i = 0; i < kDevCapWords; ++i) {
        be[i * 4 + 0] = (unsigned char)((res.words[i] >> 24) & 0xff);
        be[i * 4 + 1] = (unsigned char)((res.words[i] >> 16) & 0xff);
        be[i * 4 + 2] = (unsigned char)((res.words[i] >>  8) & 0xff);
        be[i * 4 + 3] = (unsigned char)( res.words[i]        & 0xff);
    }
    b = be;

#define BE16(o) ((uint32_t)(((uint32_t)b[(o)] << 8) | (uint32_t)b[(o)+1]))
#define BE32(o) (((uint32_t)b[(o)+0] << 24) | ((uint32_t)b[(o)+1] << 16) | \
                 ((uint32_t)b[(o)+2] << 8)  | (uint32_t)b[(o)+3])
#define BE64(o) (((uint64_t)BE32(o) << 32) | (uint64_t)BE32((o)+4))
#define F(o)    ((uint32_t)b[(o)])

    /*
     * Decoded with the reference's own expressions, copied from
     * mlx4_core/mlx4_fw.c:840-960 (mlx4_QUERY_DEV_CAP).  Most fields are not the
     * raw byte: they are 1 << (byte & mask), several pack two values into one
     * byte, and uar_size is 1 << ((byte & 0x3f) + 20).  Reporting the raw byte as
     * if it were the count is how this tool first printed a "min page size" of
     * 3.5 GB -- the byte is a log2, and 1 << 9 is what the reference computes.
     */
    printf("\n  decoded exactly as the reference decodes them (mlx4_fw.c:840-960):\n");
    printf("    QPs        : max %10u  reserved %4u  max WQEs/QP %u\n",
           1u << (F(QUERY_DEV_CAP_MAX_QP_OFFSET) & 0x1f),
           1u << (F(QUERY_DEV_CAP_RSVD_QP_OFFSET) & 0xf),
           1u << F(QUERY_DEV_CAP_MAX_QP_SZ_OFFSET));
    printf("    CQs        : max %10u  reserved %4u  max entries %u\n",
           1u << (F(QUERY_DEV_CAP_MAX_CQ_OFFSET) & 0x1f),
           1u << (F(QUERY_DEV_CAP_RSVD_CQ_OFFSET) & 0xf),
           1u << F(QUERY_DEV_CAP_MAX_CQ_SZ_OFFSET));
    printf("    EQs        : max %10u  reserved %4u\n",
           1u << (F(QUERY_DEV_CAP_MAX_EQ_OFFSET) & 0xf),
           1u << (F(QUERY_DEV_CAP_RSVD_EQ_OFFSET) & 0xf));
    printf("    SRQs       : max %10u  reserved %4u  max WQEs %u\n",
           1u << (F(QUERY_DEV_CAP_MAX_SRQ_OFFSET) & 0x1f),
           1u << (F(QUERY_DEV_CAP_RSVD_SRQ_OFFSET) >> 4),
           1u << F(QUERY_DEV_CAP_MAX_SRQ_SZ_OFFSET));
    printf("    MPTs       : max %10u  reserved MTTs %4u\n",
           1u << (F(QUERY_DEV_CAP_MAX_MPT_OFFSET) & 0x3f),
           1u << (F(QUERY_DEV_CAP_RSVD_MTT_OFFSET) >> 4));
    printf("    ports      : %u   num_sys_eqs %u   max GSO %u B   max msg %u B\n",
           F(QUERY_DEV_CAP_VL_PORT_OFFSET) & 0xf,
           BE16(QUERY_DEV_CAP_NUM_SYS_EQ_OFFSET) & 0xfff,
           F(QUERY_DEV_CAP_MAX_GSO_OFFSET) ? (1u << (F(QUERY_DEV_CAP_MAX_GSO_OFFSET) & 0x1f)) : 0u,
           1u << (F(QUERY_DEV_CAP_MAX_MSG_SZ_OFFSET) & 0x1f));
    printf("    UARs       : reserved %u, region size %u B (1<<((0x%02x & 0x3f)+20))\n",
           F(QUERY_DEV_CAP_RSVD_UAR_OFFSET) >> 4,
           1u << ((F(QUERY_DEV_CAP_UAR_SZ_OFFSET) & 0x3f) + 20),
           F(QUERY_DEV_CAP_UAR_SZ_OFFSET));
    printf("    min page sz: %u B (1 << byte 0x%02x)  <-- decides the ICM design\n",
           1u << F(QUERY_DEV_CAP_PAGE_SZ_OFFSET), F(QUERY_DEV_CAP_PAGE_SZ_OFFSET));
    printf("    MRWs       : reserved %u\n", 1u << (F(QUERY_DEV_CAP_RSVD_MRW_OFFSET) & 0xf));

    /* FreeBSD mlx4_fw.c and Linux v6.12 fw.c name these offsets but do not
     * decode them. Preserve the raw fields; log2/units are not established. */
    printf("\n  raw-only fields (interpretation not established by reference decoders):\n");
    printf("    MTT segment: raw 0x%02x at +0x23; size/units unresolved\n",
           F(QUERY_DEV_CAP_MAX_MTT_SEG_OFFSET));
    printf("    MRW size   : raw 0x%02x at +0x21; size/units unresolved\n",
           F(QUERY_DEV_CAP_MAX_MRW_SZ_OFFSET));

    /*
     * Both references declare `u32 field32, flags, ext_flags;` in this function
     * (FreeBSD mlx4_fw.c, mainline fw.c), so `flags` is a 4-byte read at 0x44 and
     * the 64-bit capability word is ext_flags<<32 | flags.  Reading it as a u64
     * at 0x44 swallows the low half of ext_flags and produces a value that looks
     * plausible and is wrong.
     */
    printf("\n  capability flags (ext_flags << 32 | flags, mlx4_fw.c:928-930):\n");
    printf("    ext_flags : 0x%08x\n", BE32(QUERY_DEV_CAP_EXT_FLAGS_OFFSET));
    printf("    flags     : 0x%08x\n", BE32(QUERY_DEV_CAP_FLAGS_OFFSET));
    printf("    combined  : 0x%08x%08x\n",
           BE32(QUERY_DEV_CAP_EXT_FLAGS_OFFSET), BE32(QUERY_DEV_CAP_FLAGS_OFFSET));

    printf("\n  raw outbox, big-endian words (what the device wrote):\n");
    for (i = 0; i < kDevCapWords; ++i) {
        if (i % 4 == 0) printf("    +0x%02x ", i * 4);
        printf("%08x ", res.words[i]);
        if (i % 4 == 3) printf("\n");
    }

#undef BE16
#undef BE32
#undef BE64
#undef F

    return 0;
}

static int cmdIcmAlloc(unsigned pages)
{
    uint64_t in[kIcmInCount];
    uint64_t out[kIcmOutCount];
    uint32_t n = kIcmOutCount;

    in[kIcmInPages] = pages;
    memset(out, 0, sizeof(out));

    if (pages == 0)
        printf("icmalloc: freeing the held area\n");
    else
        printf("icmalloc: %u pages (%u bytes) -- no device access at all\n",
               pages, pages * 4096);
    fflush(stdout);

    if (call(kCx3SelIcmAlloc, in, kIcmInCount, out, &n) != 0) return -1;
    if (n != kIcmOutCount) {
        fprintf(stderr, "icmalloc: incomplete result (%u scalars, expected %u)\n",
                n, (unsigned)kIcmOutCount);
        return -1;
    }

    if (out[kIcmOutStatus] != 0) {
        fprintf(stderr, "  failed: status 0x%llx\n",
                (unsigned long long)out[kIcmOutStatus]);
        return -1;
    }
    if (pages == 0) {
        printf("  freed\n");
        return 0;
    }

    printf("  pages      : %llu\n", (unsigned long long)out[kIcmOutPages]);
    printf("  bytes      : %llu\n", (unsigned long long)out[kIcmOutBytes]);
    printf("  first bus  : 0x%llx\n", (unsigned long long)out[kIcmOutFirstBus]);
    printf("  last bus   : 0x%llx\n", (unsigned long long)out[kIcmOutLastBus]);
    printf("  segments   : %llu %s\n", (unsigned long long)out[kIcmOutSegments],
           out[kIcmOutContiguous] ? "(contiguous)" : "(scattered - fine, each is whole pages)");
    printf("  aligned    : %s\n", out[kIcmOutAligned] ? "yes (every page 4 KiB aligned)" : "NO");
    printf("  checksum   : 0x%llx  (sum of page bus addresses)\n",
           (unsigned long long)out[kIcmOutChecksum]);

    printf("\n  This is what MAP_FA will hand to the firmware. The two things that\n"
           "  matter are checked: every page aligned to its own size, and no bus\n"
           "  segment ending mid-page (which would give one page two addresses).\n");
    return 0;
}

/*
 * fwoutbox -- QUERY_FW whose outbox lives in a page of the FW area.
 *
 * This is the one test that separates the two explanations for "RUN_FW never
 * completes": the device cannot DMA into the memory we mapped for it, or the
 * device has nothing to run.  QUERY_FW is known to complete on this card, its
 * answer is self-checking, and here it must land inside the FW area.
 *
 *   answer parsed AND the page changed  -> the device writes into this memory
 *   answer parsed but the page unchanged -> the device answered elsewhere
 *   no answer (GO stays set)             -> this memory is not usable as a
 *                                           command buffer, which is what RUN_FW
 *                                           needs
 */
static int cmdFwOutbox(unsigned page, unsigned dwords, int ignoreBusy)
{
    uint64_t in[kFwOutboxInCount];
    uint64_t out[kFwOutboxOutCount];
    uint32_t n = kFwOutboxOutCount;
    unsigned i, major, minor, patch;

    if (dwords > (unsigned)kFwOutboxMaxDwords) {
        fprintf(stderr, "fwoutbox: can return at most %u dwords per call\n",
                (unsigned)kFwOutboxMaxDwords);
        return 2;
    }
    in[kFwOutboxInPage]       = page;
    in[kFwOutboxInDwords]     = dwords;
    in[kFwOutboxInIgnoreBusy] = ignoreBusy ? 1 : 0;
    memset(out, 0, sizeof(out));

    printf("fwoutbox: QUERY_FW with its outbox inside FW-area page %u%s\n",
           page, ignoreBusy ? " (posting over a busy command register)" : "");
    printf("  the page is filled with a known pattern first, so a change is\n"
           "  attributable to the device and not to stale memory\n");
    fflush(stdout);

    if (call(kCx3SelFwOutbox, in, kFwOutboxInCount, out, &n) != 0) return -1;

    if (out[kFwOutboxOutStatus] != 0) {
        fprintf(stderr, "  failed: status 0x%llx (see the kernel log)\n",
                (unsigned long long)out[kFwOutboxOutStatus]);
        return -1;
    }

    printf("  page       : %llu (bus 0x%llx)\n",
           (unsigned long long)out[kFwOutboxOutPage],
           (unsigned long long)out[kFwOutboxOutBusAddr]);
    printf("  fw status  : 0x%llx  %s\n",
           (unsigned long long)out[kFwOutboxOutFwStatus],
           fwStatusName(out[kFwOutboxOutFwStatus]));

    if (out[kFwOutboxOutFwStatus] != 0) {
        printf("  verdict    : no answer from the device -- this memory is not "
               "usable as a\n               command buffer, which is the same "
               "capability RUN_FW needs\n");
        return 0;
    }

    fwBytes(out[kFwOutboxOutFwVer], &major, &minor, &patch);
    printf("  fw version : %u.%u.%03u  (the answer is well formed)\n",
           major, minor, patch);
    printf("  changed    : %llu of 1024 dwords differ from the pattern\n",
           (unsigned long long)out[kFwOutboxOutChanged]);
    printf("  non-zero   : %llu of 1024 dwords\n",
           (unsigned long long)out[kFwOutboxOutNonzero]);
    printf("  verdict    : %s\n",
           out[kFwOutboxOutChanged] ?
           "the device DMA'd into the FW area -- reachability is NOT the problem" :
           "the page did not change -- the device answered somewhere else");

    if (dwords) {
        printf("  dump:\n   ");
        for (i = 0; i < dwords; ++i) {
            printf(" %08llx", (unsigned long long)out[kFwOutboxOutData0 + i]);
            if ((i % 4) == 3) printf("\n   ");
        }
        printf("\n");
    }
    return 0;
}

/*
 * fwarea -- read-only look at the FW area the device was handed.
 *
 * The device is the only writer of this memory (nothing in the host driver
 * writes it), so the non-zero byte count is the one reading that separates "the
 * firmware image arrived" from "the area was never touched".  Without it,
 * "RUN_FW timed out" cannot be told apart from "RUN_FW had nothing to run".
 */
static int cmdFwArea(unsigned long long off, unsigned long count)
{
    uint64_t in[kFwAreaInCount_];
    uint64_t out[kFwAreaOutCount];
    uint32_t n = kFwAreaOutCount;
    unsigned i;

    if (count > (unsigned)kFwAreaMaxDwords) {
        fprintf(stderr, "fwarea: can return at most %u dwords per call\n",
                (unsigned)kFwAreaMaxDwords);
        return 2;
    }
    if (count && (off & 3)) {
        fprintf(stderr, "fwarea: offset 0x%llx is not dword aligned\n", off);
        return 2;
    }
    in[kFwAreaInOffset] = off;
    in[kFwAreaInCount]  = count;
    memset(out, 0, sizeof(out));

    printf("fwarea: read-only view of the FW area%s\n",
           count ? ", with a dump" : " (summary only; pass <offset> <dwords> to dump)");
    fflush(stdout);

    if (call(kCx3SelFwArea, in, kFwAreaInCount_, out, &n) != 0) return -1;

    if (out[kFwAreaOutStatus] != 0) {
        fprintf(stderr, "  failed: status 0x%llx\n",
                (unsigned long long)out[kFwAreaOutStatus]);
        return -1;
    }
    if (!out[kFwAreaOutHeld]) {
        printf("  no FW area is held (run fwload first; a reboot drops it)\n");
        return 0;
    }

    printf("  pages      : %llu\n", (unsigned long long)out[kFwAreaOutPages]);
    printf("  non-zero   : %llu bytes\n", (unsigned long long)out[kFwAreaOutNonzero]);
    printf("  checksum   : 0x%08llx  (additive over dwords)\n",
           (unsigned long long)out[kFwAreaOutChecksum]);
    printf("  verdict    : %s\n", out[kFwAreaOutNonzero] ?
           "the device has written into this area" :
           "ALL ZERO -- the device never wrote anything into the area");

    if (count) {
        printf("  dump @0x%llx:\n", off);
        for (i = 0; i < count; ++i) {
            if ((i % 4) == 0)
                printf("   +0x%08llx ", off + (unsigned long long)i * 4);
            printf("%08llx ", (unsigned long long)out[kFwAreaOutData0 + i]);
            if ((i % 4) == 3 || i + 1 == count) printf("\n");
        }
    }
    return 0;
}


/*
 * m2dprofile -- decode QUERY_DEV_CAP and run mlx4_make_profile.
 *
 * The profile knobs are arguments, not constants: they decide how much of the
 * HCA's context memory is reserved, and this driver has no use for the
 * reference's IB-stack-sized default.  The defaults below are deliberately
 * modest and are printed, so the numbers in the result can be attributed.
 */
static int cmdM2dProfile(unsigned qp, unsigned rdmarc, unsigned srq, unsigned cq,
                         unsigned mcg, unsigned mpt, unsigned mtt)
{
    uint64_t in[kM2dInCount];
    uint64_t out[kProfOutCount];
    uint32_t n = kProfOutCount;
    struct Cx3ProfileResult r;
    kern_return_t kr;

    in[kM2dInNumQp]       = qp;
    in[kM2dInRdmarcPerQp] = rdmarc;
    in[kM2dInNumSrq]      = srq;
    in[kM2dInNumCq]       = cq;
    in[kM2dInNumMcg]      = mcg;
    in[kM2dInNumMpt]      = mpt;
    in[kM2dInNumMtt]      = mtt;
    memset(out, 0, sizeof(out));
    memset(&r, 0, sizeof(r));

    printf("m2dprofile: QUERY_DEV_CAP decode + mlx4_make_profile\n");
    printf("  profile asked for: %u QPs, %u rdmarc/QP, %u SRQs, %u CQs, "
           "%u MCGs, %u MPTs, %u MTT segments\n", qp, rdmarc, srq, cq, mcg, mpt, mtt);
    printf("  (the HCA's context memory is sized from these; the reference's\n"
           "   default is an IB stack's worth and is not what this driver needs)\n");
    fflush(stdout);

    if (openService() != 0) return -1;
    kr = IOConnectCallMethod(gConn, kCx3SelM2dProfile,
                             in, kM2dInCount, NULL, 0,
                             out, &n, &r, &(size_t){sizeof(r)});
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "selector %u failed: 0x%x\n", kCx3SelM2dProfile, kr);
        return -1;
    }
    if (r.status != 0) {
        fprintf(stderr, "  failed: status 0x%llx\n", (unsigned long long)r.status);
        return -1;
    }

    printf("\n  decoded capabilities:\n");
    printf("    ports %u   entry sizes: qpc %u cqc %u eqc %u srq %u dmpt %u "
           "cmpt %u mtt %u rdmarc %u aux %u altc %u\n",
           r.numPorts, r.qpcEntrySz, r.cqcEntrySz, r.eqcEntrySz, r.srqEntrySz,
           r.dmptEntrySz, r.cmptEntrySz, r.mttEntrySz, r.rdmarcEntrySz,
           r.auxEntrySz, r.altcEntrySz);
    printf("    reserved: qps %u cqs %u srqs %u eqs %u mtts %u mrws %u   flags2 0x%x\n",
           r.reservedQps, r.reservedCqs, r.reservedSrqs, r.reservedEqs,
           r.reservedMtts, r.reservedMrws, r.flags2);

    printf("\n  packed layout (the profile is sorted by size and packed end to end):\n");
    printf("    icm size    : %llu bytes (%llu KiB), the HCA offers %llu KiB\n",
           (unsigned long long)r.icmSize, (unsigned long long)(r.icmSize >> 10),
           (unsigned long long)(r.maxIcmSz >> 10));
    printf("    qpc 0x%09llx log %u     srqc 0x%09llx log %u\n",
           (unsigned long long)r.qpcBase, r.logNumQps,
           (unsigned long long)r.srqcBase, r.logNumSrqs);
    printf("    cqc 0x%09llx log %u     eqc  0x%09llx log %u\n",
           (unsigned long long)r.cqcBase, r.logNumCqs,
           (unsigned long long)r.eqcBase, r.logNumEqs);
    printf("    altc 0x%08llx           auxc 0x%09llx\n",
           (unsigned long long)r.altcBase, (unsigned long long)r.auxcBase);
    printf("    rdmarc 0x%07llx log_rd_per_qp %u\n",
           (unsigned long long)r.rdmarcBase, r.logRdPerQp);
    printf("    dmpt 0x%09llx log %u     cmpt 0x%09llx\n",
           (unsigned long long)r.dmptBase, r.logMptSz,
           (unsigned long long)r.cmptBase);
    printf("    mtt 0x%010llx           mc 0x%09llx entry log %u hash log %u "
           "table log %u\n",
           (unsigned long long)r.mttBase, (unsigned long long)r.mcBase,
           r.logMcEntrySz, r.logMcHashSz, r.logMcTableSz);
    printf("    system EQs %u   uar size log %u (page code %u)   mw enabled %u\n",
           r.numSysEqs, r.logUarSz, r.uarPageSz, r.mwEnabled);
    printf("    %u context tables will be mapped\n", r.numTables);

    if (r.icmSize > r.maxIcmSz) {
        printf("\n  NOTE: the profile exceeds the HCA's context memory\n");
    }
    printf("\n  next: sudo cx3ctl m2dbringup\n");
    return 0;
}

/*
 * m2dbringup -- SET_ICM_SIZE, the ICM tables, then INIT_HCA.
 *
 * Each step reports its own firmware status, with 0xffffffff meaning "never
 * reached", so a failure names the step instead of saying "M2d did not work".
 */
static int cmdM2dBringUp(void)
{
    uint64_t out[kBringUpOutCount];
    uint32_t n = kBringUpOutCount;
    unsigned i;

    memset(out, 0, sizeof(out));
    printf("m2dbringup: SET_ICM_SIZE -> MAP_ICM_AUX -> MAP_ICM (each table) -> INIT_HCA\n");
    printf("  This writes the HCA command register many times. If the machine hangs,\n"
           "  power-cycle and re-run 'probe' first.\n");
    fflush(stdout);

    if (call(kCx3SelM2dBringUp, NULL, 0, out, &n) != 0) return -1;

    printf("  icm size     : %llu bytes (%llu KiB)\n",
           (unsigned long long)out[kBringUpOutIcmSize],
           (unsigned long long)(out[kBringUpOutIcmSize] >> 10));
    printf("  SET_ICM_SIZE : fw status 0x%llx %s\n",
           (unsigned long long)out[kBringUpOutSetIcmStat],
           fwStatusName(out[kBringUpOutSetIcmStat]));
    /*
     * SET_ICM_SIZE's answer is already a page count (see setIcmSize), so there is
     * one number here, not two.  The tool used to print it twice, as "units" and
     * as "pages", after the driver multiplied one by 256.
     */
    printf("  aux memory   : %llu page(s) = %llu KiB\n",
           (unsigned long long)out[kBringUpOutAuxUnits],
           (unsigned long long)out[kBringUpOutAuxUnits] * 4);
    printf("  MAP_ICM_AUX  : fw status 0x%llx %s\n",
           (unsigned long long)out[kBringUpOutMapAuxStat],
           fwStatusName(out[kBringUpOutMapAuxStat]));
    printf("  ICM tables   : %llu of %llu mapped\n",
           (unsigned long long)out[kBringUpOutTablesDone],
           (unsigned long long)out[kBringUpOutTablesTotal]);
    if (out[kBringUpOutStatus] == 0) {
        printf("  INIT_HCA     : fw status 0x%llx %s\n",
               (unsigned long long)out[kBringUpOutInitHcaStat],
               fwStatusName(out[kBringUpOutInitHcaStat]));
        printf("\n  M2d is done: the HCA has its context memory and is initialised.\n");
        printf("  The tables above are the layout M3's EQ/CQ/QP objects live in.\n");
        return 0;
    }

    /* Incomplete: say where it stopped. */
    fprintf(stderr, "\n  the sequence did not complete (status 0x%llx)\n",
            (unsigned long long)out[kBringUpOutStatus]);
    if (out[kBringUpOutTableMask] != ~0ull) {
        printf("  tables mapped: mask 0x%llx", (unsigned long long)out[kBringUpOutTableMask]);
        for (i = 0; i < 32; ++i)
            if (!(out[kBringUpOutTableMask] & (1ull << i))) {
                printf("  (first missing: table %u)", i);
                break;
            }
        printf("\n");
    }
    if (out[kBringUpOutInitHcaStat] == 0xffffffffull)
        printf("  INIT_HCA was not attempted\n");
    if (out[kBringUpOutStatus] == (unsigned long long)kIOReturnNotReady)
        printf("\n  Nothing was changed: either no profile is held, or the HCA is\n"
               "  already initialised this boot. INIT_HCA is once per load, after a\n"
               "  reset -- run 'sudo %s reset force' first for a clean sequence.\n",
               "cx3ctl");
    return 1;
}


/*
 * m3equp -- QUERY_FUNC, then one event queue, ready to receive.
 *
 * The EQ number is not assumed: on a device with system EQs, QUERY_FUNC reports
 * how many EQ numbers this function may use and the first usable number is
 * reserved_eqs.  That is printed, because a wrong EQ number would make MAP_EQ
 * route the events somewhere else and look exactly like "no event arrived".
 */
static int cmdM3EqUp(unsigned nentLog2, unsigned intr, unsigned eqeFactor, int asyncSize,
                     int mttAbs)
{
    uint64_t in[kEqUpInCount];
    uint64_t out[kEqUpOutCount];
    uint32_t n = kEqUpOutCount;

    in[kEqUpInNentLog2]  = nentLog2;
    in[kEqUpInIntr]      = intr;
    in[kEqUpInEqeFactor] = eqeFactor;
    in[kEqUpInUseAsync]  = asyncSize ? 1 : 0;
    in[kEqUpInMttAbs]    = mttAbs ? 1 : 0;
    memset(out, 0, sizeof(out));

    printf("m3equp: QUERY_FUNC + one EQ (ring, MTT, SW2HW_EQ, MAP_EQ, arm)\n");
    printf("  %s, intr %u, %u-byte EQEs, MTT address form: %s\n",
           asyncSize ? "async EQ size (256 entries + spare)" : "explicit entry count",
           intr, eqeFactor ? 64 : 32,
           mttAbs ? "ABSOLUTE (table base + offset)" : "offset inside the table");
    printf("  order matters: QUERY_FUNC decides which EQ numbers exist, so it is\n"
           "  asked before any number is chosen.\n");
    fflush(stdout);

    if (call(kCx3SelM3EqUp, in, kEqUpInCount, out, &n) != 0) return -1;

    printf("  QUERY_FUNC : fw status 0x%llx %s\n",
           (unsigned long long)out[kEqUpOutFwStatus],
           fwStatusName(out[kEqUpOutFwStatus]));
    if (out[kEqUpOutStatus] != 0) {
        printf("  stopped: status 0x%llx (see the kernel log for the step)\n",
               (unsigned long long)out[kEqUpOutStatus]);
        return 1;
    }
    printf("    max_eq %llu, reserved_eqs %llu, reserved_uars %llu\n",
           (unsigned long long)out[kEqUpOutMaxEq],
           (unsigned long long)out[kEqUpOutReservedEq],
           (unsigned long long)out[kEqUpOutReservedUar]);
    printf("  EQ         : EQN %llu, %llu entries, %llu pages, MTT offset %llu (addr 0x%llx)\n",
           (unsigned long long)out[kEqUpOutEqn],
           (unsigned long long)out[kEqUpOutNent],
           (unsigned long long)out[kEqUpOutPages],
           (unsigned long long)out[kEqUpOutMTTOffset],
           (unsigned long long)out[kEqUpOutMTTAddr]);
    printf("    ring first bus 0x%llx, doorbell BAR2+0x%llx\n",
           (unsigned long long)out[kEqUpOutRingBus],
           (unsigned long long)out[kEqUpOutDoorbell]);
    printf("  MTT addr   : %s form\n",
           out[kEqUpOutMttAbs] ? "ABSOLUTE (table base + offset)" : "offset inside the table");
    /*
     * The ring's owner state right after arming, 50 ms later.  Measured: at the
     * instant of arming the owner bytes are zero and a poll then consumes a slot
     * the HCA never wrote; a moment later the HCA owns them all, which is what an
     * armed and empty event queue looks like.  "Armed" is therefore a state the
     * HCA reaches asynchronously, and this number says whether it has.
     */
    /*
     * How long the firmware took to arm the ring.  Worth a line of its own: a poll
     * that runs before this completes consumes slots that were never written, and
     * that is exactly what hid a correct CMD EQE that had already arrived.
     */
    printf("  arm wait   : %llu ms until the ring held the armed pattern and "
           "stopped changing\n", (unsigned long long)out[kEqUpOutArmWaitMs]);
    printf("  owner scan : %llu of the first 32 slots have bit 7 set at the owner "
           "offset (a fill byte has it set too, so this is only meaningful on a "
           "ring that is otherwise zero)\n",
           (unsigned long long)out[kEqUpOutOwnerSet32]);
    printf("  MAP_EQ     : fw status 0x%llx %s (CMD and PORT_CHANGE routed here)\n",
           (unsigned long long)out[kEqUpOutMapEqStatus],
           fwStatusName(out[kEqUpOutMapEqStatus]));
    printf("\n  armed. Next: the acceptance test --\n");
    printf("      sudo cx3ctl m3eqpoll nop 0x1234\n");
    return 0;
}

/*
 * m3eqpoll -- fire an event-mode command and look for its CMD EQE.
 *
 * This is the restart-1 acceptance test, and the reason it is a separate command
 * from m3equp: with the E bit clear (every command before this one) the HCA never
 * writes a CMD EQE, so a completed command proves nothing about the event queue.
 * With E set, completion *is* an EQE, and finding it by polling proves the whole
 * chain.  A negative result prints what the ring actually holds, because "the
 * HCA still owns every slot" and "an EQE of the wrong type" are different bugs.
 */
static int cmdM3EqPoll(int fireNop, unsigned token, unsigned consIndex, unsigned dwords,
                       unsigned waitMs, int peek)
{
    uint64_t in[kEqPollInCount];
    uint64_t out[kEqPollOutCount];
    uint32_t n = kEqPollOutCount;
    unsigned i;

    if (dwords > (unsigned)kEqPollMaxDwords) {
        fprintf(stderr, "m3eqpoll: can return at most %u dwords\n",
                (unsigned)kEqPollMaxDwords);
        return 2;
    }
    in[kEqPollInFireNop]    = fireNop ? 1 : 0;
    in[kEqPollInToken]      = token;
    in[kEqPollInConsIndex]  = consIndex;
    in[kEqPollInDumpDwords] = dwords;
    in[kEqPollInWaitMs]     = waitMs;
    in[kEqPollInPeek]       = peek ? 1 : 0;
    memset(out, 0, sizeof(out));

    printf("m3eqpoll: %s, %s\n",
           fireNop ? "post a NOP in EVENT mode (E=1) then poll for its CMD EQE"
                   : "poll only (no command posted)",
           peek ? "PEEK from the given index without consuming or moving the queue"
                : "consuming in order from the head of the queue");
    printf("  waiting up to %u ms, re-scanning: a command completing and its EQE\n"
           "  being visible are not the same instant.\n", waitMs);
    if (fireNop)
        printf("  the token is the matching key: the EQE must carry 0x%x\n", token);
    fflush(stdout);

    if (call(kCx3SelM3EqPoll, in, kEqPollInCount, out, &n) != 0) return -1;
    /*
     * Raw scalar dump while the tool and the kernel log disagree about the high
     * indices: the log reports 32-of-32 owned and an EQE found, while the tool
     * reads 0 and "none" from the same call.  Printing every slot as it arrived
     * decides whether the values are missing in transit or misread here.
     */
    {
        unsigned k;
        printf("  raw scalars (%u returned):", n);
        for (k = 0; k < n && k < 20; ++k)
            printf(" [%u]=0x%llx", k, (unsigned long long)out[k]);
        printf("\n");
    }

    if (out[kEqPollOutStatus] != 0) {
        fprintf(stderr, "  failed: status 0x%llx\n",
                (unsigned long long)out[kEqPollOutStatus]);
        return -1;
    }
    if (fireNop) {
        unsigned long long hp = out[kEqPollOutHcrOnPost];
        printf("  NOP        : fw status 0x%llx %s\n",
               (unsigned long long)out[kEqPollOutNopStatus],
               fwStatusName(out[kEqPollOutNopStatus]));
        /* The command dword as the hardware was handed it: E is bit 22 and the
         * token sits in bits 31:16.  If E is clear the HCA completes the command
         * without ever writing a completion EQE, which looks exactly like an EQ
         * that cannot receive -- so this is worth printing every time. */
        /*
         * The high 16 bits of this dword are T, E and opmod -- NOT the token.  The
         * token lives in the separate token dword at HCR+0x14, which is why the
         * label changed: printing these bits as "token field" made it look as though
         * the token had been checked when it had not.
         */
        printf("  cmd dword  : 0x%08llx  (GO %llu, T %llu, E %s, opcode 0x%03llx)\n",
               hp, (hp >> 23) & 1, (hp >> 21) & 1,
               (hp & (1ull << 22)) ? "SET" : "** CLEAR **", hp & 0xfff);
        printf("  token dword: HCR+0x14 was written separately; the EQE's token is "
               "compared against the token this command was given\n");
        if (out[kEqPollOutZeroSlot])
            printf("  empty slot : index %llu was ready but held no event type\n",
                   (unsigned long long)out[kEqPollOutZeroSlot] - 1);
    }

    /*
     * The owner map first: without it a negative poll cannot be interpreted.  A
     * ring the HCA owns is what an armed, event-free EQ looks like; a ring that
     * looks ready everywhere means the host is reading the wrong memory, or the
     * HCA never took ownership at SW2HW_EQ.
     */
    printf("  owner scan : %llu of the first 32 slots have bit 7 set at the owner "
           "offset\n", (unsigned long long)out[kEqPollOutOwnerSet32]);
    if (out[kEqPollOutZeroSlot])
        printf("  empty slot : index %llu was ready but held no event type\n",
               (unsigned long long)out[kEqPollOutZeroSlot] - 1);

    if (!out[kEqPollOutFound]) {
        printf("  EQE        : none -- the HCA owns every slot in the ring\n");
        if (out[kEqPollOutOwnerSet32] == 0)
            printf("  verdict    : NOT ARMED as expected -- no slot carries the owner\n"
                   "               bit, so the HCA did not take ownership at SW2HW_EQ.\n"
                   "               A poll here cannot be trusted until that is explained.\n");
        else
            printf("  verdict    : armed and empty. Either the event was never\n"
                   "               generated, or the HCA is writing to pages the host\n"
                   "               is not reading (MTT/ring mismatch).\n");
        return 1;
    }

    printf("  EQE        : found at consumer index %llu\n",
           (unsigned long long)out[kEqPollOutConsIndex]);
    printf("    type     : 0x%02llx %s\n", (unsigned long long)out[kEqPollOutFoundType],
           out[kEqPollOutFoundType] == MLX4_EVENT_TYPE_CMD ? "(CMD)" :
           out[kEqPollOutFoundType] == MLX4_EVENT_TYPE_PORT_CHANGE ? "(PORT_CHANGE)" : "");
    if (out[kEqPollOutFoundType] == MLX4_EVENT_TYPE_CMD) {
        printf("    token    : 0x%llx %s\n", (unsigned long long)out[kEqPollOutFoundToken],
               (fireNop && out[kEqPollOutFoundToken] == token) ? "<- matches the NOP" :
               (fireNop ? "<- does NOT match" : ""));
        printf("    status   : 0x%llx %s\n", (unsigned long long)out[kEqPollOutCmdStatus],
               out[kEqPollOutCmdStatus] == 0 ? "(ok)" : "(the command failed)");
    }
    printf("    next cons: %llu (doorbell advanced and re-armed)\n",
           (unsigned long long)out[kEqPollOutNextCons]);
    if (dwords) {
        printf("    raw EQE  :\n    ");
        for (i = 0; i < dwords; ++i) {
            printf(" %08llx", (unsigned long long)out[kEqPollOutData0 + i]);
            if ((i % 4) == 3) printf("\n    ");
        }
        printf("\n");
    }

    if (fireNop && out[kEqPollOutFoundType] == MLX4_EVENT_TYPE_CMD &&
        out[kEqPollOutFoundToken] == token && out[kEqPollOutCmdStatus] == 0) {
        printf("\n  PASS: the event-mode command's completion arrived as a CMD EQE in\n"
               "  this EQ. HCR event request -> routing -> MTT -> EQ DMA -> software\n"
               "  consumption is proven, with no interrupt involved.\n");
        return 0;
    }
    return 1;
}


/*
 * m3eqring -- can the HCA write into the EQ ring's memory?
 *
 * The discriminator a negative poll needs.  The first R1 run showed SQ2HW_EQ and
 * MAP_EQ accepted, an event-mode NOP completed, and the ring completely untouched
 * -- every owner byte zero, and the consumed slot of type 0x00, i.e. never
 * written.  "The HCA is not using this memory" and "the HCA cannot reach this
 * memory" look identical from there, and they need opposite fixes.
 *
 * So the HCA is asked to DMA into a ring page through a path already proven on
 * this device: a QUERY_FW whose outbox points there, the same instrument that
 * proved the FW area reachable.  The page is filled with 0xa5 first, so what the
 * device wrote is unmistakable.
 */
static int cmdM3EqRing(unsigned page, int force)
{
    uint64_t in[kEqRingInCount];
    uint64_t out[kEqRingOutCount];
    uint32_t n = kEqRingOutCount;
    unsigned i, major, minor, patch;

    in[kEqRingInPage] = page;
    in[kEqRingInForce] = force ? 1 : 0;
    memset(out, 0, sizeof(out));

    printf("m3eqring: QUERY_FW with its outbox at EQ ring page %u%s\n", page,
           force ? " (force=1: this OVERWRITES the ring page)" : "");
    printf("  the page is filled with 0xa5 first, so anything the device writes is\n"
           "  unmistakable\n");
    fflush(stdout);

    if (call(kCx3SelM3EqRing, in, kEqRingInCount, out, &n) != 0) return -1;

    if (out[kEqRingOutStatus] != 0) {
        fprintf(stderr, "  failed: status 0x%llx (see the kernel log)\n",
                (unsigned long long)out[kEqRingOutStatus]);
        return -1;
    }
    printf("  fw status  : 0x%llx %s\n", (unsigned long long)out[kEqRingOutFwStatus],
           fwStatusName(out[kEqRingOutFwStatus]));
    if (out[kEqRingOutFwStatus] != 0) {
        printf("  verdict    : no answer from the device -- this memory is NOT usable\n"
               "               as a DMA target, which is also why no EQE ever appeared\n");
        return 1;
    }
    fwBytes(out[kEqRingOutFwVer], &major, &minor, &patch);
    printf("  fw version : %u.%u.%03u  (the answer is well formed)\n", major, minor, patch);
    printf("  changed    : %llu of the first 64 bytes differ from 0xa5\n",
           (unsigned long long)out[kEqRingOutChanged]);
    printf("  dump       :");
    for (i = 0; i < (unsigned)kEqRingMaxDwords; ++i)
        printf(" %08llx", (unsigned long long)out[kEqRingOutData0 + i]);
    printf("\n");
    printf("  verdict    : %s\n", out[kEqRingOutChanged] ?
           "the HCA CAN write into the ring -- the memory and its bus addresses are\n"
           "               fine, so a missing EQE is an EQ-configuration problem" :
           "the page did not change -- the ring's bus addresses are not usable by the\n"
           "               device, which explains the empty ring");
    return out[kEqRingOutChanged] ? 0 : 1;
}


/*
 * m3eqinfo -- two read-only checks for "the ring is untouched and no EQE arrives".
 *
 * The ring probe proved the HCA can write into the ring's *memory*, but it handed
 * a bus address over directly, which is not the path a real event takes: an event
 * is written through the MTT, which the EQ context points at.  So this reads back
 * the MTT entries the HCA would follow and compares them with the ring's own page
 * addresses -- and then asks the HCA what it thinks this EQ is (QUERY_EQ), which
 * the reference driver never sends but which costs nothing to attempt.
 */
static int cmdM3EqInfo(void)
{
    uint64_t out[kEqInfoOutCount];
    uint32_t n = kEqInfoOutCount;
    struct Cx3EqInfoResult r;
    kern_return_t kr;
    unsigned i, nEntries;

    memset(out, 0, sizeof(out));
    memset(&r, 0, sizeof(r));

    printf("m3eqinfo: MTT readback + QUERY_EQ (read-only)\n");
    fflush(stdout);

    if (openService() != 0) return -1;
    kr = IOConnectCallMethod(gConn, kCx3SelM3EqInfo, NULL, 0, NULL, 0,
                             out, &n, &r, &(size_t){sizeof(r)});
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "selector %u failed: 0x%x\n", kCx3SelM3EqInfo, kr);
        return -1;
    }
    if (out[kEqInfoOutStatus] != 0) {
        fprintf(stderr, "  failed: status 0x%llx (see the kernel log)\n",
                (unsigned long long)out[kEqInfoOutStatus]);
        return -1;
    }

    nEntries = (unsigned)out[kEqInfoOutRingPages];
    if (nEntries > (unsigned)kEqInfoMaxEntries) nEntries = (unsigned)kEqInfoMaxEntries;
    printf("  ring pages : %llu, MTT offset %llu\n",
           (unsigned long long)out[kEqInfoOutRingPages],
           (unsigned long long)out[kEqInfoOutMTTOffset]);
    printf("  MTT entries the HCA would read for those pages:\n");
    for (i = 0; i < nEntries; ++i) {
        /*
         * The entry is printed whole, and the verdict comes from its own field in the
         * structure.  Printing "mttEntry[i] & 0x7fffffff" with the top bit as a
         * mismatch flag is what made this line lie: an MTT entry's bit 31 is an address
         * bit, so a ring page at or above 2 GiB lost that bit in the display and read as
         * a mismatch.  A value and its verdict are two fields, not one
         * (docs/DEFECTS.md F7, docs/EQ-ORDERING.md section 4.5).
         */
        printf("    page %u: bus 0x%08x  ->  mtt[%llu] = 0x%08x  %s\n", i,
               r.ringBus[i], (unsigned long long)(out[kEqInfoOutMTTOffset] + i),
               r.mttEntry[i], r.mttMatch[i] ? "matches" : "<- DOES NOT MATCH");
    }
    printf("  match      : %s\n", out[kEqInfoOutMatch] ?
           "every MTT entry equals its page address | present" :
           "MISMATCH -- the HCA would follow the wrong page list");

    /*
     * The HCA's own copy of the EQ context.  This is the direct answer to "did
     * SW2HW_EQ store what we sent": the HCA copies the context into the EQC table
     * at eqc_base + eqn*entry_size and that table is host memory, so no firmware
     * support is needed to read it back.
     */
    printf("  EQC entry  : HCA 0x%llx + 0x%llx, %llu of 128 bytes non-zero\n",
           (unsigned long long)out[kEqInfoOutEqcBase],
           (unsigned long long)out[kEqInfoOutEqcOffset],
           (unsigned long long)out[kEqInfoOutEqcNonzero]);
    printf("    what the HCA holds:");
    for (i = 0; i < (unsigned)kEqInfoCtxWords; ++i) {
        if ((i % 4) == 0) printf("\n     +0x%02x ", i * 4);
        printf(" %08x", r.eqcEntry[i]);
    }
    printf("\n");
    printf("    what this port built: flags 0x00000900, log_eq_size 9, intr 0,\n"
           "     log_page_size 0, mtt_base_addr at +0x1b/+0x1c\n");
    if (out[kEqInfoOutEqcNonzero] == 0)
        /*
         * A fact, not a verdict.
         *
         * This line used to conclude "... -- SW2HW_EQ did not store the context, so
         * nothing downstream can work", and it is wrong: on this card that host copy
         * reads all zero while QUERY_EQ answers 0x00000900 with the geometry this port
         * built, i.e. the context is stored and the host-side copy of the EQC table is
         * simply not where the HCA keeps it (docs/DEFECTS.md F3, and F8 for the fact
         * that this site outlived the fix).  Reproduced live on 2026-10-12 05:03 --
         * the same output said "ALL ZERO" and "QUERY_EQ answered 0x00000900".
         *
         * The readback stays, because it is evidence about one address; the conclusion
         * does not, because that address is not the whole story.
         */
        printf("  note       : this host-memory copy of the EQC entry reads all zero.\n"
               "               That is not a verdict -- on this card the HCA does not\n"
               "               keep it here, and QUERY_EQ below is the authoritative\n"
               "               answer (docs/DEFECTS.md F3/F8)\n");

    if (!out[kEqInfoOutQueryEqOk]) {
        printf("  QUERY_EQ   : no answer (fw status 0x%llx %s) -- unsupported by this\n"
               "               firmware, or unavailable for this EQ; recorded, not assumed\n",
               (unsigned long long)out[kEqInfoOutFwStatus],
               fwStatusName(out[kEqInfoOutFwStatus]));
        return 0;
    }
    printf("  QUERY_EQ   : answered; first 32 bytes of its context:\n   ");
    for (i = 0; i < (unsigned)kEqInfoMaxEntries; ++i) {
        printf(" %08x", r.eqContext[i]);
        if ((i % 4) == 3) printf("\n   ");
    }
    printf("\n  compare that with the context this port wrote (the flags dword should\n"
           "  read 0x00000900 = STATUS_OK | STATE_ARMED, and log_eq_size 9 for 512\n"
           "  entries).\n"
           "  The flags dword carries the EQ's state, and two values are meaningful\n"
           "  here and mean different things:\n"
           "    0x00000900  STATE_ARMED -- the queue will raise an interrupt.\n"
           "    0x00000a00  STATE_FIRED -- it has already fired and NOTHING has\n"
           "                re-armed it.  The only re-arm sites are ring consumers (the\n"
           "                interrupt action at the end of every pass, and the poll\n"
           "                selector), so FIRED with an idle interrupt means delivery is\n"
           "                not running, or the EQ fired before delivery was enabled.\n"
           "                See docs/EQ-ORDERING.md.\n"
           "  A different log_eq_size, or a flags word that is neither, would mean the\n"
           "  HCA rewrote or rejected the geometry.\n");
    return 0;
}


/*
 * ------------------------------------------------------------------ */
/* M3 restart 2: interrupts                                             */
/* ------------------------------------------------------------------ */

/*
 * m3irqup -- open an interrupt session.
 *
 * The source index is an argument because the mapping this project has not measured
 * is "which provider interrupt index is MSI-X table entry 0".  The EQ context's intr
 * field is already 0 (that is what the reference fills in for its asynchronous EQ),
 * so if source 0 turns out to be entry 0 the two agree and the completion arrives;
 * if it does not, the next index is one call away rather than one build away.
 */
static int cmdM3IrqUp(unsigned source, int forceLegacy)
{
    uint64_t in[kIrqUpInCount];
    uint64_t out[kIrqUpOutCount];
    uint32_t n = kIrqUpOutCount;

    in[kIrqUpInSource] = source;
    in[kIrqUpInForceLegacy] = forceLegacy ? 1 : 0;
    memset(out, 0, sizeof(out));

    printf("m3irqup: open an interrupt session on provider source %u\n", source);
    printf("  the EQ context was built with intr=0, so this source has to be MSI-X\n"
           "  table entry 0 for the two to agree; getInterruptType's answer is printed\n"
           "  so that can be settled by measurement.\n");
    fflush(stdout);

    if (call(kCx3SelM3IrqUp, in, kIrqUpInCount, out, &n) != 0) return -1;

    printf("  status     : 0x%llx\n", (unsigned long long)out[kIrqUpOutStatus]);
    if (out[kIrqUpOutStatus] != 0) {
        printf("  the session did not open (see the kernel log for why)\n");
        return 1;
    }
    printf("  source     : %llu\n", (unsigned long long)out[kIrqUpOutSource]);
    printf("  interrupt  : type 0x%llx %s (getInterruptType rc 0x%llx)\n",
           (unsigned long long)out[kIrqUpOutIntType],
           (out[kIrqUpOutIntType] & 0x00010000ull) ? "PCI-messaged (MSI/MSI-X)" : "legacy",
           (unsigned long long)out[kIrqUpOutGetTypeRc]);
    printf("  enabled    : %llu\n", (unsigned long long)out[kIrqUpOutEnabled]);
    printf("  EQ         : EQN %llu, context intr field %llu\n",
           (unsigned long long)out[kIrqUpOutEqn],
           (unsigned long long)out[kIrqUpOutEqIntr]);
    printf("\n  next: post a test command and read the result separately\n");
    printf("      sudo cx3ctl m3irqtest 0x1234\n");
    printf("      sudo cx3ctl m3irqstatus\n");
    return 0;
}

/*
 * m3irqtest -- register the token, post the event-mode NOP, and return.
 *
 * It deliberately does not wait.  A caller that waited here for a completion the
 * interrupt path has to deliver would be waiting on a path that may need this very
 * thread -- and if this ran on the workloop, the workloop would be waiting on
 * itself.  Posting and collecting are separate commands for that reason.
 */
static int cmdM3IrqTest(unsigned token)
{
    uint64_t in[kIrqTestInCount];
    uint64_t out[kIrqTestOutCount];
    uint32_t n = kIrqTestOutCount;

    in[kIrqTestInToken] = token;
    memset(out, 0, sizeof(out));

    printf("m3irqtest: post an event-mode NOP with token 0x%x and return\n", token);
    fflush(stdout);

    if (call(kCx3SelM3IrqTest, in, kIrqTestInCount, out, &n) != 0) return -1;

    printf("  status     : 0x%llx\n", (unsigned long long)out[kIrqTestOutStatus]);
    printf("  NOP fw st. : 0x%llx %s\n", (unsigned long long)out[kIrqTestOutFwStatus],
           fwStatusName(out[kIrqTestOutFwStatus]));
    printf("  token      : 0x%llx\n", (unsigned long long)out[kIrqTestOutToken]);
    {
        unsigned long long hp = out[kIrqTestOutHcrOnPost];
        printf("  cmd dword  : 0x%08llx  (GO %llu, T %llu, E %s, opcode 0x%03llx)\n",
               hp, (hp >> 23) & 1, (hp >> 21) & 1,
               (hp & (1ull << 22)) ? "SET" : "** CLEAR **", hp & 0xfff);
    }
    if (out[kIrqTestOutStatus] != 0) return 1;
    printf("\n  posted. The completion is the interrupt's job now -- read it with:\n");
    printf("      sudo cx3ctl m3irqstatus\n");
    return 0;
}

/*
 * m3irqstatus -- what the interrupt path has seen.
 *
 * "irq count 0" and "irq count > 0 with no match" are different answers: the first
 * says the notification never reached the handler (wrong vector, or the source is
 * not enabled), the second says it did and the queue state is where to look.
 */
static int cmdM3IrqStatus(void)
{
    uint64_t out[kIrqStatusOutCount];
    uint32_t n = kIrqStatusOutCount;

    memset(out, 0, sizeof(out));
    if (call(kCx3SelM3IrqStatus, NULL, 0, out, &n) != 0) return -1;

    printf("m3irqstatus:\n");
    {
        unsigned long long f = out[kIrqStatusOutFlags];
        printf("  delivery   : %s%s\n", (f & 1) ? "enabled" : "disabled",
               (f & 4) ? "  [STORM GUARD TRIPPED]" : "");
        printf("  objects    : %s\n", (f & 2) ?
               "source and workloop still allocated" : "none allocated");
        printf("  interrupt  : %s source; clear register %s\n",
               (f & 8) ? "LEGACY (needs a clear)" : "messaged",
               (f & 16) ? "set up" : "not set up");
    }
    /*
     * Invocations, the callback's own count, and EQEs are three different numbers
     * and the reviews asked for them to be kept apart: "handler invocations" is not
     * "hardware interrupts", and the poll selector shares the consumer.
     */
    printf("  invocations: %llu, callback count %llu\n",
           (unsigned long long)out[kIrqStatusOutIrqCount],
           (unsigned long long)out[kIrqStatusOutCallbackCount]);
    printf("  EQEs       : %llu via interrupts, %llu via the poll selector\n",
           (unsigned long long)out[kIrqStatusOutEqeCount],
           (unsigned long long)out[kIrqStatusOutPollEqeCount]);
    printf("  errors     : %llu ready slot(s) carried nothing usable, %llu skipped\n",
           (unsigned long long)out[kIrqStatusOutErrors],
           (unsigned long long)out[kIrqStatusOutSkipped]);
    /*
     * The acknowledgements actually written.  If this is 0 while a legacy source is
     * enabled, the clear never happened -- which is precisely the state that stormed
     * and wedged the machine.
     */
    printf("  acks       : %llu interrupt acknowledgement(s) written\n",
           (unsigned long long)out[kIrqStatusOutClrWrites]);
    printf("  waiting for: 0x%llx %s\n", (unsigned long long)out[kIrqStatusOutWantToken],
           out[kIrqStatusOutPending] ? "(outstanding)" : "(nothing registered)");
    printf("  matched    : %s", out[kIrqStatusOutMatched] ? "YES" : "no");
    if (out[kIrqStatusOutMatched])
        printf("  token 0x%llx, status 0x%llx %s",
               (unsigned long long)out[kIrqStatusOutMatchedToken],
               (unsigned long long)out[kIrqStatusOutMatchedStatus],
               out[kIrqStatusOutMatchedStatus] == 0 ? "(ok)" : "(command failed)");
    printf("\n");
    printf("  last type  : 0x%02llx\n", (unsigned long long)out[kIrqStatusOutLastType]);
    printf("  cons index : %llu\n", (unsigned long long)out[kIrqStatusOutConsIndex]);
    printf("  zero streak: %llu consecutive invocation(s) consumed nothing%s\n",
           (unsigned long long)out[kIrqStatusOutZeroStreak],
           (out[kIrqStatusOutFlags] & 4) ?
           "  -- STORM GUARD TRIPPED, delivery was disabled" : "");

    /*
     * PASS only counts while delivery is on.  The matched flag is provider state and
     * outlives the session, so after m3irqdown this used to report a success for a
     * session that no longer exists -- the same "stale field read as a live result"
     * mistake that has recurred in this project.
     */
    if (out[kIrqStatusOutMatched] && out[kIrqStatusOutMatchedStatus] == 0 &&
        (out[kIrqStatusOutFlags] & 1)) {
        printf("\n  PASS: the completion arrived through the interrupt path, not a poll.\n");
        return 0;
    }
    if (out[kIrqStatusOutMatched] && (out[kIrqStatusOutFlags] & 1) == 0)
        printf("\n  (a completion had matched earlier, but delivery is now disabled; run\n"
               "  m3irqup and m3irqtest again for a live result)\n");
    if (out[kIrqStatusOutIrqCount] == 0) {
        printf("\n  no interrupt was delivered at all. Either the source index is not\n"
               "  the vector the EQ is on, or the source is not enabled -- the EQ\n"
               "  context was built with intr=0 (MSI-X table entry 0).\n");
    } else {
        printf("\n  interrupts arrived but the token did not match; the queue is where to\n"
               "  look (kernel log: 'consumeEq:' lines say what was found).\n");
    }
    return 1;
}

static int cmdM3IrqDown(void)
{
    uint64_t out[kIrqDownOutCount];
    uint32_t n = kIrqDownOutCount;

    memset(out, 0, sizeof(out));
    printf("m3irqdown: closing the interrupt session\n");
    fflush(stdout);
    if (call(kCx3SelM3IrqDown, NULL, 0, out, &n) != 0) return -1;
    printf("  status     : 0x%llx\n", (unsigned long long)out[kIrqDownOutStatus]);
    printf("  invocations: %llu, EQEs via interrupts %llu, skipped %llu%s\n",
           (unsigned long long)out[kIrqDownOutIrqCount],
           (unsigned long long)out[kIrqDownOutEqeCount],
           (unsigned long long)out[kIrqDownOutSkipped],
           out[kIrqDownOutTripped] ? "  (storm guard had tripped)" : "");
    printf("  the event source is disabled and removed before anything is released,\n"
           "  so no callback can be running against freed state.\n");
    return 0;
}


/*
 * m3clrint -- set up the interrupt-clear register so a legacy interrupt can be
 * acknowledged.
 *
 * This exists because of a hang: a level-triggered source was enabled without any
 * way to clear it, the handler re-ran hundreds of times consuming nothing, and the
 * system stopped.  The reference clears the register as the first statement of its
 * legacy handler; this port now has the same step, and m3irqup refuses a legacy
 * source until it is in place.
 */
static int cmdM3ClrInt(void)
{
    uint64_t out[kClrIntOutCount];
    uint32_t n = kClrIntOutCount;

    memset(out, 0, sizeof(out));
    printf("m3clrint: QUERY_ADAPTER + the interrupt-clear register\n");
    fflush(stdout);
    if (call(kCx3SelM3ClrInt, NULL, 0, out, &n) != 0) return -1;

    if (out[kClrIntOutStatus] != 0) {
        fprintf(stderr, "  failed: status 0x%llx (see the kernel log)\n",
                (unsigned long long)out[kClrIntOutStatus]);
        return -1;
    }
    printf("  QUERY_ADAPTER: fw status 0x%llx %s\n",
           (unsigned long long)out[kClrIntOutFwStatus],
           fwStatusName(out[kClrIntOutFwStatus]));
    printf("  inta_pin   : %llu (0x%02llx)\n",
           (unsigned long long)out[kClrIntOutIntaPin],
           (unsigned long long)out[kClrIntOutIntaPin]);
    printf("  board id   : %08llx%08llx...\n",
           (unsigned long long)out[kClrIntOutBoard0],
           (unsigned long long)out[kClrIntOutBoard1]);
    printf("  clear reg  : BAR %llu + 0x%llx, offset 0x%llx\n",
           (unsigned long long)out[kClrIntOutBar],
           (unsigned long long)out[kClrIntOutBase],
           (unsigned long long)out[kClrIntOutOffset]);
    printf("  mask       : 0x%08llx  (swab32(1 << (pin & 31)))\n",
           (unsigned long long)out[kClrIntOutMask]);
    printf("  ready      : %llu\n", (unsigned long long)out[kClrIntOutReady]);
    printf("\n  Now a legacy source can be enabled:\n");
    printf("      sudo cx3ctl m3irqup 0\n");
    return 0;
}

/*
 * queryport -- QUERY_PORT for one port.  Read-only.
 *
 * The whole 64-byte answer is printed byte for byte, and then the decoded view, from
 * the decoder in cx3_abi.h -- the same function the kext calls.  Printing both is
 * deliberate: the raw bytes are the evidence, the decode is the interpretation, and
 * a wrong interpretation cannot hide behind a plausible-looking field list.
 */
static int cmdM3QueryPort(unsigned port)
{
    uint64_t in[kQueryPortInCount];
    uint64_t out[kQueryPortOutCount];
    uint32_t n = kQueryPortOutCount;
    Cx3QueryPortResult res;
    unsigned i;

    in[kQueryPortInPort] = port;
    memset(out, 0, sizeof(out));
    memset(&res, 0, sizeof(res));

    printf("queryport: QUERY_PORT (0x43) for port %u -- read-only\n", port);
    fflush(stdout);

    if (callStruct(kCx3SelM3QueryPort, in, kQueryPortInCount, out, &n,
                   &res, sizeof(res)) != 0) return -1;

    printf("  selector status : 0x%llx\n", (unsigned long long)out[kQueryPortOutStatus]);
    if (res.fwStatus == CX3_FWSTATUS_NOT_POSTED) {
        printf("  the kext did NOT post the command (status 0x%x) -- see the kernel log\n",
               res.status);
        return 0;
    }
    printf("  fw status       : 0x%x %s\n", res.fwStatus,
           res.fwStatus == 0 ? "ok" : "-- rejected");
    if (res.fwStatus != 0) return 0;

    printf("\n  raw answer, first %u bytes as they came back:\n", res.rawLen);
    for (i = 0; i < res.rawLen; i += 16) {
        unsigned k;
        printf("    %04x:", i);
        for (k = 0; k < 16 && i + k < res.rawLen; ++k)
            printf(" %02x", res.raw[i + k]);
        printf("\n");
    }

    printf("\n  decoded (offsets from mlx4_fw.c:1202-1243 and en_port.h:89-102):\n");
    printf("    0x00 link_state      : %u%s\n", res.linkState,
           res.linkState ? "   <- the link is UP" : "");
    printf("    0x00 supported_types : %u  (%s)\n", res.supportedTypes,
           res.supportedTypes == 3 ? "IB and Ethernet" :
           res.supportedTypes == 2 ? "Ethernet only" :
           res.supportedTypes == 1 ? "IB only" : "none reported");
    printf("    0x00 suggested_type  : %u  (%s)\n", res.suggestedType,
           res.suggestedType ? "Ethernet" : "IB");
    printf("    0x00 default_sense   : %u\n", res.defaultSense);
    printf("    0x00 dmfs_optimized  : %u\n", res.dmfsOptimized);
    printf("    0x01 autoneg         : %u\n", res.autoneg);
    printf("    0x01 ib_mtu          : %u\n", res.ibMtu);
    printf("    0x02 eth_mtu         : %u\n", res.ethMtu);
    printf("    0x05 link_speed      : 0x%02x -> %u Mb/s%s\n", res.linkSpeed,
           cx3LinkSpeedMbps(res.linkSpeed),
           cx3LinkSpeedMbps(res.linkSpeed) ? "" : "  (an encoding the reference also "
                                                "calls unknown)");
    printf("    0x06 max_port_width  : %u\n", res.maxPortWidth);
    printf("    0x07 max_gids/pkeys  : %u / %u\n", res.maxGids, res.maxPkeys);
    printf("    0x0a macs/vlans      : log2 %u / log2 %u\n", res.logMaxMacs, res.logMaxVlans);
    printf("    0x0b max_vl/tc_eth   : %u / %u\n", res.maxVl, res.maxTcEth);
    printf("    0x10 def_mac         : %02x:%02x:%02x:%02x:%02x:%02x\n",
           res.defMac[2], res.defMac[3], res.defMac[4], res.defMac[5], res.defMac[6],
           res.defMac[7]);
    printf("    0x18 trans_type/oui  : %u / 0x%06x\n", res.transType, res.vendorOui);
    printf("    0x1c wavelength      : %u\n", res.wavelength);
    printf("    0x20 trans_code      : 0x%016llx\n", (unsigned long long)res.transCode);

    if (!(res.supportedTypes & CX3_PORT_TYPE_ETH))
        printf("\n  NOTE: this port does not advertise Ethernet, so 'm3portup' would "
               "refuse to write anything.\n");
    return 0;
}

/*
 * queryhca -- QUERY_HCA: which steering mode is the HCA actually in?
 *
 * Read-only.  This is R4's entrance condition, and it is a measurement rather than an
 * assumption because of a real omission: the port's INIT_HCA never wrote
 * INIT_HCA_UC_STEERING_OFFSET, so the HCA was told A0 regardless of what the port
 * meant.  A0 and B0 differ in ways that matter for receive steering -- whether
 * mlx4_SET_PORT_qpn_calc sends a command at all, and how a unicast MAC's QPN is derived.
 */
static int cmdM3QueryHca(void)
{
    uint64_t out[kQueryHcaOutCount];
    uint32_t n = kQueryHcaOutCount;
    Cx3QueryHcaResult res;
    unsigned i;

    memset(out, 0, sizeof(out));
    memset(&res, 0, sizeof(res));

    printf("queryhca: QUERY_HCA (0x0b) -- read-only\n");
    fflush(stdout);

    if (callStruct(kCx3SelM3QueryHca, NULL, 0, out, &n, &res, sizeof(res)) != 0) return -1;

    printf("  selector status : 0x%llx\n", (unsigned long long)out[kQueryHcaOutStatus]);
    if (res.fwStatus == CX3_FWSTATUS_NOT_POSTED) {
        printf("  the kext did NOT post the command (status 0x%x) -- see the kernel log\n",
               res.status);
        return 0;
    }
    printf("  fw status       : 0x%x %s\n", res.fwStatus,
           res.fwStatus == 0 ? "ok" : "-- rejected");
    if (res.fwStatus != 0) return 0;

    printf("\n  raw answer, first %u bytes:\n", res.rawLen);
    for (i = 0; i < res.rawLen; i += 16) {
        unsigned k;
        printf("    %04x:", i);
        for (k = 0; k < 16 && i + k < res.rawLen; ++k)
            printf(" %02x", res.raw[i + k]);
        printf("\n");
    }

    printf("\n  steering mode   : %s\n", cx3SteerName(res.steeringMode));
    printf("    flags dword 0x014 : 0x%08x  (device-managed bit 6 = %u)\n",
           res.flagsDword, res.deviceManagedBit);
    printf("    byte 0x0d8        : 0x%02x  (B0 is bit 3 = %u)\n",
           res.ucSteeringByte, (res.ucSteeringByte >> 3) & 1u);
    printf("    mc_base 0x0c0     : 0x%llx\n", (unsigned long long)res.mcBase);
    printf("    log_mc entry/hash/table : %u / %u / %u\n",
           res.logMcEntrySz, res.logMcHashSz, res.logMcTableSz);
    printf("    log_num_qps / cqs : %u / %u\n", res.logNumQps, res.logNumCqs);

    printf("\n  what this means for R4:\n");
    switch (res.steeringMode) {
    case CX3_STEER_A0:
        printf("    A0 -- mlx4_SET_PORT_qpn_calc DOES send a command, and a unicast MAC's\n"
               "    QPN is base_qpn + mac_index.  This is what an INIT_HCA that never\n"
               "    wrote the B0 byte produces, so it is the mode this port should assume\n"
               "    unless the byte is written and this query says otherwise.\n");
        break;
    case CX3_STEER_B0:
        printf("    B0 -- qpn_calc is a no-op (mlx4_port.c:1588 returns early), and unicast\n"
               "    MACs are attached to QPNs explicitly.  The B0 byte is set.\n");
        break;
    default:
        printf("    DEVICE_MANAGED (DMFS) -- MAC/flow steering goes through the flow\n"
               "    steering tables, and the MC hash/table sizes behave differently.\n");
        break;
    }
    return 0;
}

static int cmdQueryAdapter(void)
{
    struct Cx3AdapterResult res;
    size_t outSize = sizeof(res);
    kern_return_t kr;
    unsigned i;

    if (openService() != 0) return -1;
    memset(&res, 0, sizeof(res));
    kr = IOConnectCallMethod(gConn, kCx3SelQueryAdapter,
                             NULL, 0, NULL, 0,
                             NULL, NULL, &res, &outSize);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "queryadapter: selector %u transport failed: 0x%x\n",
                kCx3SelQueryAdapter, kr);
        return -1;
    }
    if (outSize != sizeof(res) || res.version != CX3_ADAPTER_RESULT_VERSION ||
        res.structBytes != sizeof(res) || res.status != 0 || res.fwStatus != 0 ||
        res.rawBytes != CX3_ADAPTER_RAW_BYTES || res.intaPin > 0xff || res.reserved != 0) {
        fprintf(stderr, "queryadapter: invalid/incomplete result (bytes=%zu version=%u size=%u "
                "status=0x%llx fw=0x%x raw=%u pin=%u reserved=%u)\n",
                outSize, res.version, res.structBytes, (unsigned long long)res.status,
                res.fwStatus, res.rawBytes, res.intaPin, res.reserved);
        return -1;
    }

    printf("queryadapter: QUERY_ADAPTER -- read-only\n  board ID / PSID candidate: \"");
    for (i = 0; i < sizeof(res.boardId); ++i) {
        unsigned char c = res.boardId[i];
        if (c == 0) break;
        if (c >= 0x20 && c <= 0x7e && c != '\\' && c != '"') putchar(c);
        else if (c == '\\' || c == '"') { putchar('\\'); putchar(c); }
        else printf("\\x%02x", c);
    }
    printf("\"\n  INTA pin: %u\n  device generation: %llu\n  raw outbox (256 bytes):\n",
           res.intaPin, (unsigned long long)res.deviceGeneration);
    for (i = 0; i < CX3_ADAPTER_RAW_BYTES; i += 16) {
        unsigned j;
        printf("    %04x:", i);
        for (j = 0; j < 16; ++j) printf(" %02x", res.raw[i + j]);
        putchar('\n');
    }
    return 0;
}

static const char *pcieSpeed(unsigned code)
{
    static const char *const speed[] = { NULL, "2.5", "5", "8", "16", "32", "64" };
    return code <= 6 ? speed[code] : NULL;
}

static int pcieWidthKnown(unsigned width)
{
    return width == 1 || width == 2 || width == 4 || width == 8 ||
           width == 12 || width == 16 || width == 32;
}

/* EEPROM query has HCR/mailbox activity even though the operation is GET.
 * One command, no retry or reinitialization; JSON always preserves the reply. */
static int cmdPortInfo(uint32_t port, int refresh, int json)
{
    struct Cx3PortInfoResult r;
    uint64_t input[2] = {port, refresh ? 1u : 0u};
    size_t bytes = sizeof(r);
    uint32_t count = 0;
    kern_return_t kr;
    int good;
    if (port < 1 || port > 2) return 2;
    if (openService() != 0) return 1;
    memset(&r, 0, sizeof(r));
    kr = IOConnectCallMethod(gConn, kCx3SelPortInfo, input, 2, NULL, 0,
                             NULL, &count, &r, &bytes);
    good = !kr && !count && bytes == sizeof(r) && r.version == CX3_PORT_INFO_VERSION &&
        r.structBytes == sizeof(r) && !r.status && r.port == port && r.deviceGeneration &&
        !(r.flags & ~255u) && !(r.flags & CX3_PORT_LAST_QUERY_FAILED) &&
        (r.flags & CX3_PORT_QUERY_VALID) && r.rawBytes == CX3_QUERY_PORT_RAW_LEN &&
        r.queryUptimeNs && !r.reserved[0] && !r.reserved[1] && !r.reservedWord;
    if (json) {
        printf("{\"selector\":52,\"call\":%u,\"inCount\":2,\"in\":[%u,%u],\"outCount\":0,\"out\":[],\"structLen\":%zu,\"structHex\":\"",
               (unsigned)kr, port, refresh ? 1u:0u, !kr && bytes <= sizeof(r) ? bytes:0);
        if (!kr && bytes <= sizeof(r)) {
            const unsigned char *b = (const unsigned char *)&r;
            for (size_t i = 0; i < bytes; ++i) printf("%02x", b[i]);
        }
        printf("\"}\n");
    } else if (good) {
        printf("portinfo: port %u generation %llu flags=0x%x observedAt=%llu ns\n"
               "  link=%s speed=%u Mbps wireMTU=%u payloadCapability=%u MAC=%02x:%02x:%02x:%02x:%02x:%02x\n"
               "  INIT fw=0x%x CLOSE fw=0x%x SET_MTU fw=0x%x (receipts, not DMA release proof)\n",
               r.port, (unsigned long long)r.deviceGeneration, r.flags,
               (unsigned long long)r.queryUptimeNs,
               (r.flags & CX3_PORT_LINK_VALID) ? (r.linkUp ? "up":"down") : "unknown",
               r.speedMbps, r.frameMtu, r.maxPayloadMtu,
               r.mac[0],r.mac[1],r.mac[2],r.mac[3],r.mac[4],r.mac[5],
               r.initFwStatus,r.closeFwStatus,r.mtuFwStatus);
    }
    if (!good) fprintf(stderr,"portinfo: no current valid observation (transport=0x%x status=0x%llx flags=0x%x)\n",
                        kr,(unsigned long long)r.status,r.flags);
    return good ? 0:1;
}

static int cmdModuleRead(uint64_t port, uint64_t offset, uint64_t length, int json)
{
    struct Cx3ModuleResult r;
    uint64_t inputs[3] = {port, offset, length};
    size_t size = sizeof(r);
    unsigned i;
    int good;
    kern_return_t kr;
    if (!cx3ModuleRangeValid(port, offset, length)) {
        fprintf(stderr, "module: require port 1..2, offset 0..511, length 1..48 without crossing 256-byte boundary\n");
        return 2;
    }
    if (openService() != 0) return 1;
    memset(&r, 0, sizeof(r));
    kr = IOConnectCallMethod(gConn, kCx3SelModuleRead, inputs, 3, NULL, 0,
                             NULL, NULL, &r, &size);
    good = kr == KERN_SUCCESS && size == sizeof(r) &&
        r.version == CX3_MODULE_RESULT_VERSION && r.structBytes == sizeof(r) &&
        r.status == 0 && r.fwStatus == 0 && r.madStatus == 0 &&
        r.deviceGeneration != 0 && r.port == port && r.offset == offset &&
        r.requestedBytes == length && r.returnedBytes == length &&
        r.i2cAddress == (offset < 256 ? 0x50u : 0x51u) &&
        r.page == 0 && r.wireOffset == (offset & 255) &&
        r.rawBytes == CX3_MODULE_RAW_BYTES && r.raw[4] == 0 && r.raw[5] == 0 &&
        memcmp(r.data, r.raw + 80, (size_t)length) == 0;
    if (json) {
        const unsigned char *b = (const unsigned char *)&r;
        size_t actual = kr == KERN_SUCCESS && size <= sizeof(r) ? size : 0;
        printf("{\"selector\":50,\"call\":%d,\"inCount\":3,\"in\":[\"0x%llx\",\"0x%llx\",\"0x%llx\"],\"outCount\":0,\"out\":[],\"structLen\":%zu,\"structHex\":\"",
            (int)kr, (unsigned long long)port, (unsigned long long)offset,
            (unsigned long long)length, actual);
        for (i = 0; i < actual; ++i) printf("%02x", b[i]);
        printf("\"}\n");
    } else if (good) {
        printf("module: port %u offset %u length %u (I2C 0x%x, page 0; generation %llu)\n",
            r.port, r.offset, r.returnedBytes, r.i2cAddress,
            (unsigned long long)r.deviceGeneration);
        for (i = 0; i < r.returnedBytes; ++i)
            printf("%02x%s", r.data[i], (i+1)%16 && i+1<r.returnedBytes ? " " : "\n");
    }
    if (!good) {
        fprintf(stderr, "module: no valid sample (transport=0x%x bytes=%zu status=0x%llx fw=0x%x MAD=0x%x returned=%u)\n",
            kr, size, (unsigned long long)r.status, r.fwStatus, r.madStatus, r.returnedBytes);
        return 1;
    }
    return 0;
}

static int pcieByteWasRead(const struct Cx3PcieLinkResult *r, unsigned off)
{
    return off < CX3_PCIE_RAW_BYTES &&
           (r->readValid[off / 8] & (1u << (off % 8))) != 0;
}

static unsigned pcieRaw16(const uint8_t *raw, unsigned off)
{
    return (unsigned)raw[off] | ((unsigned)raw[off + 1] << 8);
}

/* Re-walk the sampled conventional capability list before publishing a link.
 * Transport success alone does not establish the result's structural contract. */
static int pcieSampleField(const struct Cx3PcieLinkResult *r, unsigned off,
                           unsigned bytes, unsigned *value)
{
    unsigned i, v = 0;
    if (bytes == 0 || bytes > 4 || off > 256 - bytes) return 0;
    for (i = 0; i < bytes; ++i) {
        if (!pcieByteWasRead(r, off + i)) return 0;
        v |= (unsigned)r->raw[off + i] << (8 * i);
    }
    if (v == (bytes == 4 ? 0xffffffffu : (1u << (8 * bytes)) - 1)) return 0;
    *value = v;
    return 1;
}

static int pcieSampleShapeValid(const struct Cx3PcieLinkResult *r)
{
    unsigned off, header, status, pointer, count = 0, flags, value, next;
    uint8_t visited[256] = {0};
    for (off = 0; off < 256; ++off)
        if (!pcieByteWasRead(r, off) && r->raw[off]) return 0;
    if (r->identityBefore.deviceNumber >= 32 || r->identityBefore.function >= 8)
        return 0;
    if (!pcieSampleField(r, 0, 2, &value) || !pcieSampleField(r, 2, 2, &value) ||
        !pcieSampleField(r, 0x2c, 2, &value) || !pcieSampleField(r, 0x2e, 2, &value) ||
        !pcieSampleField(r, 0x0e, 1, &header) || (header & 0x7f) ||
        !pcieSampleField(r, 6, 2, &status) || !(status & 0x10) ||
        !pcieSampleField(r, 0x34, 1, &pointer)) return 0;
    while (pointer) {
        if (pointer < 0x40 || pointer > 0xfc || (pointer & 3) ||
            visited[pointer] || count >= 48) return 0;
        visited[pointer] = 1; ++count;
        if (!pcieSampleField(r, pointer, 2, &header)) return 0;
        next = header >> 8;
        if (next && (next < 0x40 || next > 0xfc || (next & 3) || visited[next])) return 0;
        if ((header & 0xff) == 0x10) break;
        pointer = next;
    }
    if (!pointer || pointer != r->capOffset || count != r->walkCount ||
        !pcieSampleField(r, pointer + 2, 2, &flags) || flags != r->pcieFlags ||
        !pcieSampleField(r, pointer + 0xc, 4, &value) || value != r->lnkCap ||
        !pcieSampleField(r, pointer + 0x12, 2, &value) || value != r->lnkSta) return 0;
    if ((flags & 0xf) == 2 &&
        (!pcieSampleField(r, pointer + 0x2c, 4, &value) || value != r->lnkCap2)) return 0;
    return 1;
}

static int cmdPcieLink(void)
{
    struct Cx3PcieLinkResult r;
    size_t n = sizeof(r);
    kern_return_t kr;
    unsigned cs, cw, ms, mw;
    if (openService() != 0) return -1;
    memset(&r, 0, sizeof(r));
    kr = IOConnectCallMethod(gConn, kCx3SelPcieLink, NULL, 0, NULL, 0,
                             NULL, NULL, &r, &n);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "pcielink: selector %u transport failed: 0x%x\n", kCx3SelPcieLink, kr);
        return -1;
    }
    if (n != sizeof(r) || r.version != CX3_PCIE_RESULT_VERSION ||
        r.structBytes != sizeof(r) || r.reason > CX3_PCIE_REASON_INCONSISTENT ||
        (r.validity & ~0x1fu)) {
        fprintf(stderr, "pcielink: malformed result (bytes=%zu version=%u size=%u reason=%u validity=0x%x)\n",
                n, r.version, r.structBytes, r.reason, r.validity);
        return -1;
    }
    if (r.status || r.reason != CX3_PCIE_REASON_OK ||
        r.validity != ((r.pcieFlags & 0xf) == 2 ? 0x1fu : 0x0fu)) {
        fprintf(stderr, "pcielink: query rejected status=0x%llx reason=%u validity=0x%x\n",
                (unsigned long long)r.status, r.reason, r.validity);
        return -1;
    }
    if (!pcieSampleShapeValid(&r)) {
        fprintf(stderr, "pcielink: invalid or incomplete sampled configuration\n"); return -1;
    }
    if (r.capOffset < 0x40 || r.capOffset > 0xfc || (r.capOffset & 3)) {
        fprintf(stderr, "pcielink: invalid capability offset 0x%x\n", r.capOffset); return -1;
    }
    if ((r.pcieFlags & 0xf) < 1 || (r.pcieFlags & 0xf) > 2 ||
        ((r.pcieFlags >> 4) & 0xf) > 1 || r.identityBefore.reserved || r.identityAfter.reserved ||
        memcmp(&r.identityBefore, &r.identityAfter, sizeof(r.identityBefore)) != 0) {
        fprintf(stderr, "pcielink: unsupported flags or nonzero reserved identity byte\n"); return -1;
    }
    {
        static const unsigned offs[] = {0,1,2,3,0x2c,0x2d,0x2e,0x2f};
        unsigned j;
        for (j = 0; j < sizeof(offs)/sizeof(offs[0]); ++j)
            if (!pcieByteWasRead(&r, offs[j])) { fprintf(stderr, "pcielink: identity sample incomplete\n"); return -1; }
        for (j = r.capOffset; j < r.capOffset + 4; ++j)
            if (!pcieByteWasRead(&r, j)) { fprintf(stderr, "pcielink: capability header incomplete\n"); return -1; }
        for (j = r.capOffset + 0x0c; j < r.capOffset + 0x10; ++j)
            if (!pcieByteWasRead(&r, j)) { fprintf(stderr, "pcielink: LnkCap incomplete\n"); return -1; }
        for (j = r.capOffset + 0x12; j < r.capOffset + 0x14; ++j)
            if (!pcieByteWasRead(&r, j)) { fprintf(stderr, "pcielink: LnkSta incomplete\n"); return -1; }
        if ((r.pcieFlags & 0xf) == 2)
            for (j = r.capOffset + 0x2c; j < r.capOffset + 0x30; ++j)
                if (!pcieByteWasRead(&r, j)) { fprintf(stderr, "pcielink: LnkCap2 incomplete\n"); return -1; }
    }
    cs = r.lnkSta & 0xf; cw = (r.lnkSta >> 4) & 0x3f;
    ms = r.lnkCap & 0xf; mw = (r.lnkCap >> 4) & 0x3f;
    if (r.pcieFlags != ((unsigned)r.raw[r.capOffset+2] | ((unsigned)r.raw[r.capOffset+3] << 8)) ||
        r.lnkCap != ((unsigned)r.raw[r.capOffset+0xc] | ((unsigned)r.raw[r.capOffset+0xd] << 8) |
                     ((unsigned)r.raw[r.capOffset+0xe] << 16) | ((unsigned)r.raw[r.capOffset+0xf] << 24)) ||
        r.lnkSta != ((unsigned)r.raw[r.capOffset+0x12] | ((unsigned)r.raw[r.capOffset+0x13] << 8))) {
        fprintf(stderr, "pcielink: side fields disagree with raw config sample\n"); return -1;
    }
    if (r.identityBefore.vendor != pcieRaw16(r.raw, 0) ||
        r.identityBefore.device != pcieRaw16(r.raw, 2) ||
        r.identityBefore.subsystemVendor != pcieRaw16(r.raw, 0x2c) ||
        r.identityBefore.subsystemId != pcieRaw16(r.raw, 0x2e)) {
        fprintf(stderr, "pcielink: identity fields disagree with raw config sample\n"); return -1;
    }
    if ((r.pcieFlags & 0xf) == 2) {
        unsigned raw2 = (unsigned)r.raw[r.capOffset+0x2c] | ((unsigned)r.raw[r.capOffset+0x2d] << 8) |
                        ((unsigned)r.raw[r.capOffset+0x2e] << 16) | ((unsigned)r.raw[r.capOffset+0x2f] << 24);
        if (r.lnkCap2 != raw2) { fprintf(stderr, "pcielink: LnkCap2 disagrees with raw config sample\n"); return -1; }
    } else if (r.lnkCap2 != 0) { fprintf(stderr, "pcielink: v1 result must not expose LnkCap2\n"); return -1; }
    printf("pcielink: bounded read-only PCIe sample (non-atomic)\n  negotiated current: ");
    if (pcieSpeed(cs)) printf("%s GT/s", pcieSpeed(cs)); else printf("speed code %u unresolved", cs);
    printf(" x%u", cw); if (!pcieWidthKnown(cw)) printf(" (raw width unresolved)");
    printf("\n  advertised LnkCap maximum: ");
    if (pcieSpeed(ms)) printf("%s GT/s", pcieSpeed(ms)); else printf("speed code %u unresolved", ms);
    printf(" x%u", mw); if (!pcieWidthKnown(mw)) printf(" (raw width unresolved)");
    putchar('\n');
    printf("  Training raw bit: %u (meaning unverified); LnkCap2 raw: 0x%08x\n",
           (r.lnkSta >> 11) & 1, r.lnkCap2);
    if (r.lnkCap & (1u << 20)) printf("  DLL active: %s\n", (r.lnkSta & (1u << 13)) ? "yes" : "no");
    else printf("  DLL active: unsupported/unknown\n");
    printf("  cap=0x%02x walked=%u provider generation=%llu\n",
           r.capOffset, r.walkCount, (unsigned long long)r.deviceGeneration);
    puts("  Sample is not an atomic hardware snapshot; generation is provider-local bookkeeping only.");
    return 0;
}


/* CX3_RUNTIME_CLI_BEGIN */
struct Cx3RuntimeSample {
    struct Cx3RuntimeIdentityResult result;
    uint64_t connectedServiceRegistryId;
    char bootSessionUuid[37];
    uint32_t transport, scalarOutputCount;
    size_t returnedBytes;
};

static int runtimeUuidValid(const char *s)
{
    unsigned i, nonzero = 0;
    if (s[36]) return 0;
    for (i = 0; i < 36; ++i) {
        char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return 0; }
        else {
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return 0;
            if (c != '0') nonzero = 1;
        }
    }
    return nonzero != 0;
}

static int runtimeMetadataValid(const char *s, size_t cap, int version)
{
    size_t i, length = 0;
    unsigned dots = 0;
    while (length < cap && s[length]) ++length;
    if (!length || length == cap) return 0;
    for (i = length + 1; i < cap; ++i) if (s[i]) return 0;
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (version) {
            if (c == '.' && i && i + 1 < length && s[i-1] != '.') { if (++dots > 2) return 0; }
            else if (c < '0' || c > '9') return 0;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                     (c >= 'A' && c <= 'Z') || (i && (c == '.' || c == '_' || c == '+' || c == '-')))) return 0;
    }
    return version || (strcmp(s, "dev") && strcmp(s, "unversioned-local") && strcmp(s, "unknown"));
}

static int runtimeIdentityShapeValid(const struct Cx3RuntimeSample *s)
{
    const struct Cx3RuntimeIdentityResult *r = &s->result;
    const struct Cx3PcieIdentity *p = &r->pciIdentity;
    if (s->transport || s->returnedBytes != sizeof(*r) || s->scalarOutputCount ||
        r->version != CX3_RUNTIME_RESULT_VERSION || r->structBytes != sizeof(*r) ||
        r->status || r->validity != 63 || (r->lifecycleFlags & ~31u) ||
        !r->serviceRegistryId || !r->providerRegistryId || !r->deviceGeneration || !r->sampleUptimeNs ||
        r->serviceRegistryId != s->connectedServiceRegistryId || !runtimeUuidValid(s->bootSessionUuid) ||
        r->reserved || r->reservedBytes[0] || r->reservedBytes[1] || r->reservedBytes[2] ||
        !p->vendor || p->vendor == 0xffff || !p->device || p->device == 0xffff ||
        !p->subsystemVendor || p->subsystemVendor == 0xffff || !p->subsystemId || p->subsystemId == 0xffff ||
        p->deviceNumber >= 32 || p->function >= 8 || p->reserved) return 0;
    return runtimeMetadataValid(r->bundleVersion, sizeof(r->bundleVersion), 1) &&
           runtimeMetadataValid(r->buildTag, sizeof(r->buildTag), 0);
}

static int readRuntimeSample(struct Cx3RuntimeSample *s)
{
    io_service_t service = 0;
    size_t bootBytes = sizeof(s->bootSessionUuid);
    kern_return_t kr;
    memset(s, 0, sizeof(*s));
    s->transport = (uint32_t)KERN_FAILURE;
    if (sysctlbyname("kern.bootsessionuuid", s->bootSessionUuid, &bootBytes, NULL, 0) != 0 ||
        bootBytes != sizeof(s->bootSessionUuid) || s->bootSessionUuid[36] ||
        !runtimeUuidValid(s->bootSessionUuid) || openService() != 0) return -1;
    kr = IOConnectGetService(gConn, &service);
    if (kr != KERN_SUCCESS || !service) return -1;
    kr = IORegistryEntryGetRegistryEntryID(service, &s->connectedServiceRegistryId);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS || !s->connectedServiceRegistryId) return -1;
    s->returnedBytes = sizeof(s->result);
    s->transport = (uint32_t)IOConnectCallMethod(gConn, kCx3SelRuntimeIdentity,
        NULL, 0, NULL, 0, NULL, &s->scalarOutputCount, &s->result, &s->returnedBytes);
    return runtimeIdentityShapeValid(s) ? 0 : -1;
}

static void runtimeEmitHex(const void *data, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)data;
    size_t i;
    for (i = 0; i < size; ++i) printf("%02x", bytes[i]);
}

static void emitRuntimeSample(const struct Cx3RuntimeSample *s)
{
    printf("{\"schema\":\"cx3-runtime-identity-v1\",\"bootSessionUuid\":\"%s\","
           "\"connectedServiceRegistryId\":\"0x%016llx\",\"transport\":%u,"
           "\"scalarOutputCount\":%u,\"returnedBytes\":%zu,\"rawHex\":\"",
           runtimeUuidValid(s->bootSessionUuid) ? s->bootSessionUuid : "",
           (unsigned long long)s->connectedServiceRegistryId, s->transport,
           s->scalarOutputCount, s->returnedBytes);
    if (!s->transport && s->returnedBytes <= sizeof(s->result))
        runtimeEmitHex(&s->result, s->returnedBytes);
    printf("\"}");
}

static int cmdRuntimeIdentity(int json)
{
    struct Cx3RuntimeSample s;
    int rc = readRuntimeSample(&s);
    if (json) { emitRuntimeSample(&s); putchar('\n'); }
    else if (!rc)
        printf("identity: boot %s\n  service 0x%016llx provider 0x%016llx\n"
               "  release %s build %s generation %llu flags 0x%x\n",
               s.bootSessionUuid, (unsigned long long)s.result.serviceRegistryId,
               (unsigned long long)s.result.providerRegistryId, s.result.bundleVersion,
               s.result.buildTag, (unsigned long long)s.result.deviceGeneration, s.result.lifecycleFlags);
    if (rc) fprintf(stderr, "identity: unavailable, malformed or incomplete runtime metadata\n");
    return rc;
}

static int runtimeSameInstance(const struct Cx3RuntimeSample *a, const struct Cx3RuntimeSample *b)
{
    return runtimeIdentityShapeValid(a) && runtimeIdentityShapeValid(b) &&
        !strcmp(a->bootSessionUuid, b->bootSessionUuid) &&
        a->connectedServiceRegistryId == b->connectedServiceRegistryId &&
        a->result.serviceRegistryId == b->result.serviceRegistryId &&
        a->result.providerRegistryId == b->result.providerRegistryId &&
        a->result.deviceGeneration == b->result.deviceGeneration &&
        a->result.lifecycleFlags == b->result.lifecycleFlags &&
        !((a->result.lifecycleFlags | b->result.lifecycleFlags) & 15u) &&
        b->result.sampleUptimeNs >= a->result.sampleUptimeNs &&
        !memcmp(&a->result.pciIdentity, &b->result.pciIdentity, sizeof(a->result.pciIdentity)) &&
        !strcmp(a->result.bundleVersion, b->result.bundleVersion) &&
        !strcmp(a->result.buildTag, b->result.buildTag);
}

/* Same service/build/boot identity, allowing only the documented first-init
 * generation transition. The kernel checks the before binding while locked. */
static int autoNetworkSameProvider(const struct Cx3RuntimeSample *a,
                                   const struct Cx3RuntimeSample *b)
{
    return runtimeIdentityShapeValid(a) && runtimeIdentityShapeValid(b) &&
        !strcmp(a->bootSessionUuid, b->bootSessionUuid) &&
        a->connectedServiceRegistryId == b->connectedServiceRegistryId &&
        a->result.serviceRegistryId == b->result.serviceRegistryId &&
        a->result.providerRegistryId == b->result.providerRegistryId &&
        b->result.sampleUptimeNs >= a->result.sampleUptimeNs &&
        !memcmp(&a->result.pciIdentity, &b->result.pciIdentity, sizeof(a->result.pciIdentity)) &&
        !strcmp(a->result.bundleVersion, b->result.bundleVersion) &&
        !strcmp(a->result.buildTag, b->result.buildTag);
}

static int cmdAutoNetwork(uint32_t action, const char *expectedBuild, int json)
{
    struct Cx3RuntimeSample before, after;
    struct Cx3AutoNetworkRequest req;
    struct Cx3AutoNetworkResult r;
    size_t bytes = sizeof(r), tagBytes = strlen(expectedBuild);
    uint32_t outputs = 0, transport = (uint32_t)KERN_FAILURE;
    int beforeOk, afterOk = 0, called = 0, bound = 0, good = 0;
    if (!tagBytes || tagBytes >= sizeof(req.expectedBuildTag) ||
        !runtimeMetadataValid(expectedBuild, tagBytes + 1, 0)) return 2;
    memset(&req, 0, sizeof(req)); memset(&r, 0, sizeof(r)); memset(&after, 0, sizeof(after));
    beforeOk = readRuntimeSample(&before) == 0;
    if (beforeOk && !strcmp(expectedBuild, before.result.buildTag)) {
        req.version = CX3_AUTO_NETWORK_VERSION; req.structBytes = sizeof(req);
        req.action = action; req.port = 1;
        req.expectedServiceId = before.result.serviceRegistryId;
        req.expectedProviderId = before.result.providerRegistryId;
        req.expectedGeneration = before.result.deviceGeneration;
        memcpy(req.expectedBuildTag, expectedBuild, tagBytes + 1);
        called = 1;
        transport = (uint32_t)IOConnectCallMethod(gConn, kCx3SelAutoNetwork,
            NULL, 0, &req, sizeof(req), NULL, &outputs, &r, &bytes);
        afterOk = readRuntimeSample(&after) == 0;
        bound = afterOk && autoNetworkSameProvider(&before, &after);
        if (bound && !transport && !outputs && bytes == sizeof(r) &&
            r.version == CX3_AUTO_NETWORK_VERSION && r.structBytes == sizeof(r) &&
            r.action == action && r.port == 1 && !r.reserved && r.state <= kCx3AutoFailed &&
            r.stage <= 16 && r.completedStage <= 16 && r.failedStage <= 16 &&
            !(r.flags & ~31u) && !(r.completedMask & ~CX3_AUTO_NETWORK_ALL_STAGES) &&
            r.deviceGeneration == after.result.deviceGeneration) {
            int sameEpoch = before.result.deviceGeneration == after.result.deviceGeneration &&
                before.result.lifecycleFlags == after.result.lifecycleFlags;
            int firstInit = action == kCx3AutoEnable && before.result.deviceGeneration == 1 &&
                !before.result.lifecycleFlags && after.result.deviceGeneration == 2 &&
                after.result.lifecycleFlags == CX3_RUNTIME_FLAG_FW_RUN_ATTEMPTED &&
                r.completedMask == CX3_AUTO_NETWORK_ALL_STAGES;
            good = !r.status && (sameEpoch || firstInit) &&
                !(r.flags & (CX3_AUTO_NET_TX_FAULT | CX3_AUTO_NET_LIFECYCLE_BLOCKED)) &&
                (action == kCx3AutoStatus ||
                 (action == kCx3AutoEnable && r.state == kCx3AutoReady &&
                  r.completedMask == CX3_AUTO_NETWORK_ALL_STAGES) ||
                 (action == kCx3AutoDisable && r.state == kCx3AutoDisabled));
        }
    }
    if (json) {
        printf("{\"schema\":\"cx3-auto-network-v1\",\"called\":%s,\"bound\":%s,\"ok\":%s,\"before\":",
               called ? "true":"false", bound ? "true":"false", good ? "true":"false");
        emitRuntimeSample(&before);
        printf(",\"control\":{\"transport\":%u,\"returnedBytes\":%zu,\"rawHex\":\"", transport, called ? bytes:0);
        if (called && !transport && bytes <= sizeof(r)) runtimeEmitHex(&r, bytes);
        printf("\"},\"after\":");
        if (afterOk) emitRuntimeSample(&after); else printf("null");
        printf("}\n");
    } else {
        printf("autonet: %s state=%u stage=%u completed=0x%llx failedStage=%u status=0x%llx generation=%llu unit=%u flags=0x%x\n",
            good ? "OK":"STOP", r.state, r.stage, (unsigned long long)r.completedMask,
            r.failedStage, (unsigned long long)r.status, (unsigned long long)r.deviceGeneration,
            r.interfaceUnit, r.flags);
    }
    if (!good) fprintf(stderr, "autonet: stopped; no retry/reset/cleanup (called=%d transport=0x%x bound=%d)\n",called,transport,bound);
    return good ? 0 : 1;
}

static int cmdNativeConfig(uint32_t port,uint32_t action,uint32_t value,const char*expectedBuild,int json)
{
    struct Cx3RuntimeSample before,after;
    struct Cx3NativeConfigResult r;
    uint64_t input[4];size_t bytes=sizeof(r),tagBytes=strlen(expectedBuild);
    uint32_t outputs=0,transport=(uint32_t)KERN_FAILURE;
    int afterOk=0,called=0,bound=0,good=0;
    if(port<1||port>2||action>CX3_NCFG_TXCSUM||(action<=CX3_NCFG_VLAN_CLEAR&&value)||
       ((action==CX3_NCFG_VLAN_ADD||action==CX3_NCFG_VLAN_DEL)&&value>=4095)||
       (action>=CX3_NCFG_RXCSUM&&value>1)||!tagBytes||tagBytes>=sizeof(before.result.buildTag)||
       !runtimeMetadataValid(expectedBuild,tagBytes+1,0))return 2;
    memset(&before,0,sizeof(before));memset(&after,0,sizeof(after));memset(&r,0,sizeof(r));
    if(!readRuntimeSample(&before)&&!strcmp(expectedBuild,before.result.buildTag)&&!(before.result.lifecycleFlags&15u)){
        input[0]=port;input[1]=action;input[2]=before.result.deviceGeneration;input[3]=value;called=1;
        transport=(uint32_t)IOConnectCallMethod(gConn,kCx3SelNativeConfig,input,4,NULL,0,NULL,&outputs,&r,&bytes);
        afterOk=readRuntimeSample(&after)==0;bound=afterOk&&runtimeSameInstance(&before,&after);
        if(bound&&!transport&&!outputs&&bytes==sizeof(r)&&r.version==CX3_NATIVE_CONFIG_VERSION&&
           r.structBytes==sizeof(r)&&r.deviceGeneration==before.result.deviceGeneration&&r.port==port&&
           r.action==action&&r.value==value&&!(r.flags&~15u)&&r.vlanCount<=4095&&
           (!(r.flags&CX3_NCFG_VLAN_APPLIED)||(!r.fwStatus&&r.vlanRevision)))good=!r.status;
    }
    if(json){
        printf("{\"schema\":\"cx3-native-config-v1\",\"called\":%s,\"bound\":%s,\"ok\":%s,\"before\":",called?"true":"false",bound?"true":"false",good?"true":"false");
        emitRuntimeSample(&before);printf(",\"config\":{\"transport\":%u,\"returnedBytes\":%zu,\"rawHex\":\"",transport,called?bytes:0);
        if(called&&!transport&&bytes<=sizeof(r))runtimeEmitHex(&r,bytes);
        printf("\"},\"after\":");if(afterOk)emitRuntimeSample(&after);else printf("null");printf("}\n");
    }else printf("nativeconfig: %s port=%u action=%u value=%u flags=0x%x VLANs=%u revision=%llu status=0x%llx fw=0x%x\n",
        good?"OK":"STOP",port,action,value,r.flags,r.vlanCount,(unsigned long long)r.vlanRevision,(unsigned long long)r.status,r.fwStatus);
    if(!good)fprintf(stderr,"nativeconfig: stopped; no retry/reset/cleanup (called=%d transport=0x%x bound=%d)\n",called,transport,bound);
    return good?0:1;
}

static int cmdNativePort(uint32_t port, uint32_t action, const char *expectedBuild, int json)
{
    struct Cx3RuntimeSample before, after;
    struct Cx3NativePortResult r;
    uint64_t input[3];
    size_t bytes=sizeof(r), tagBytes=strlen(expectedBuild);
    uint32_t outputs=0, transport=(uint32_t)KERN_FAILURE;
    int afterOk=0, called=0, bound=0, good=0;
    if (port<1 || port>2 || action>CX3_NATIVE_DETACH || !tagBytes ||
        tagBytes>=sizeof(before.result.buildTag) || !runtimeMetadataValid(expectedBuild,tagBytes+1,0)) return 2;
    memset(&before,0,sizeof(before)); memset(&after,0,sizeof(after)); memset(&r,0,sizeof(r));
    if (!readRuntimeSample(&before) && !strcmp(expectedBuild,before.result.buildTag) &&
        !(before.result.lifecycleFlags & 15u)) {
        input[0]=port; input[1]=action; input[2]=before.result.deviceGeneration;
        called=1;
        transport=(uint32_t)IOConnectCallMethod(gConn,kCx3SelNativePort,input,3,NULL,0,
            NULL,&outputs,&r,&bytes);
        afterOk=readRuntimeSample(&after)==0;
        bound=afterOk && runtimeSameInstance(&before,&after);
        if (bound && !transport && !outputs && bytes==sizeof(r) &&
            r.version==CX3_NATIVE_VERSION && r.structBytes==sizeof(r) &&
            r.deviceGeneration==before.result.deviceGeneration && r.port==port && r.action==action &&
            !(r.flags & ~63u) && !r.reserved[0] && !r.reserved[1]) {
            int attached=(r.flags & CX3_NATIVE_ATTACHED)!=0;
            int present=(r.flags & CX3_NATIVE_PRESENT)!=0;
            int enabled=(r.flags & CX3_NATIVE_ENABLED)!=0;
            int fault=(r.flags & (CX3_NATIVE_POISONED|CX3_NATIVE_TX_FAULT|CX3_NATIVE_RX_FAULT))!=0;
            int shape=(!attached||present)&&(!enabled||attached);
            good=shape && !r.status && !fault &&
                (action==CX3_NATIVE_STATUS || (action==CX3_NATIVE_ATTACH && attached && r.interfaceUnit!=0xffffffffu) ||
                 (action==CX3_NATIVE_DETACH && !present));
        }
    }
    if (json) {
        printf("{\"schema\":\"cx3-native-port-v1\",\"called\":%s,\"bound\":%s,\"ok\":%s,\"before\":",
            called?"true":"false",bound?"true":"false",good?"true":"false");
        emitRuntimeSample(&before);
        printf(",\"native\":{\"transport\":%u,\"returnedBytes\":%zu,\"rawHex\":\"",transport,called?bytes:0);
        if (called && !transport && bytes<=sizeof(r)) runtimeEmitHex(&r,bytes);
        printf("\"},\"after\":"); if (afterOk) emitRuntimeSample(&after); else printf("null"); puts("}");
    } else {
        printf("nativeport: %s port=%u unit=%u flags=0x%x status=0x%llx generation=%llu\n"
               "  RX=%llu/%llu bytes dropped=%llu TX=%llu completed=%llu dropped=%llu errors=%u/%u\n",
            good?"OK":"STOP",port,r.interfaceUnit,r.flags,(unsigned long long)r.status,
            (unsigned long long)r.deviceGeneration,(unsigned long long)r.rxFrames,(unsigned long long)r.rxBytes,
            (unsigned long long)r.rxDropped,(unsigned long long)r.txFrames,(unsigned long long)r.txCompleted,
            (unsigned long long)r.txDropped,r.rxErrors,r.txErrors);
    }
    if (!good) fprintf(stderr,"nativeport: stopped; no retry/reset/cleanup (called=%d transport=0x%x bound=%d)\n",called,transport,bound);
    return good?0:1;
}

static int cmdPortQueue(uint32_t port, uint32_t action, const char *expectedBuild, int json)
{
    struct Cx3RuntimeSample before, after;
    struct Cx3PortQueueResult r;
    uint64_t input[3];
    size_t bytes=sizeof(r), tagBytes=strlen(expectedBuild);
    uint32_t outputs=0, transport=(uint32_t)KERN_FAILURE;
    int afterOk=0, called=0, bound=0, good=0;
    if (port<1 || port>2 || action>CX3_PORT_QUEUE_DESTROY || !tagBytes ||
        tagBytes>=sizeof(before.result.buildTag) || !runtimeMetadataValid(expectedBuild,tagBytes+1,0)) return 2;
    memset(&before,0,sizeof(before)); memset(&after,0,sizeof(after)); memset(&r,0,sizeof(r));
    if (!readRuntimeSample(&before) && !strcmp(expectedBuild,before.result.buildTag) &&
        !(before.result.lifecycleFlags & 15u)) {
        input[0]=port; input[1]=action; input[2]=before.result.deviceGeneration;
        called=1;
        transport=(uint32_t)IOConnectCallMethod(gConn,kCx3SelPortQueue,input,3,NULL,0,
            NULL,&outputs,&r,&bytes);
        afterOk=readRuntimeSample(&after)==0;
        bound=afterOk && runtimeSameInstance(&before,&after);
        if (bound && !transport && !outputs && bytes==sizeof(r) &&
            r.version==CX3_PORT_QUEUE_VERSION && r.structBytes==sizeof(r) &&
            r.deviceGeneration==before.result.deviceGeneration && r.port==port && r.action==action &&
            r.numPorts>=port && r.numPorts<=2 && !(r.flags & ~15u) &&
            r.stage<=5 && r.failedStage<=5 && !(r.completedMask & ~31ull) && !(r.ownerMask & ~2047ull)) {
            int ready=(r.flags & CX3_PORT_QUEUE_READY)!=0;
            int failed=(r.flags & CX3_PORT_QUEUE_FAILED)!=0;
            int reserved=(r.flags & CX3_PORT_QUEUE_RESERVED)!=0;
            int owners=(r.flags & CX3_PORT_QUEUE_OWNERS)!=0;
            int shape=(!r.ownerMask || owners) && (!failed || r.failedStage) &&
                (!ready || (reserved && owners && !failed && r.stage==5 && r.completedMask==31 &&
                    r.rxQpn==r.macBase && r.macSlots && !(r.macSlots & (r.macSlots-1)) &&
                    r.macSlots<=128 && !r.fwStatus && r.macBase<=(1u<<24)-r.macSlots &&
                    r.rxCqn<(1u<<24) && r.txCqn<(1u<<24) &&
                    r.rssQpn!=r.txQpn && r.rssQpn<r.macBase &&
                    r.txQpn<r.macBase && r.rxCqn!=r.txCqn));
            good=shape && !r.status && !failed &&
                (action==CX3_PORT_QUEUE_STATUS || (action==CX3_PORT_QUEUE_CREATE && ready) ||
                 (action==CX3_PORT_QUEUE_DESTROY && !ready && !owners && !r.ownerMask && !r.completedMask));
        }
    }
    if (json) {
        printf("{\"schema\":\"cx3-port-queue-v1\",\"called\":%s,\"bound\":%s,\"ok\":%s,\"before\":",
            called?"true":"false",bound?"true":"false",good?"true":"false");
        emitRuntimeSample(&before);
        printf(",\"queue\":{\"transport\":%u,\"returnedBytes\":%zu,\"rawHex\":\"",transport,called?bytes:0);
        if (called && !transport && bytes<=sizeof(r)) runtimeEmitHex(&r,bytes);
        printf("\"},\"after\":"); if (afterOk) emitRuntimeSample(&after); else printf("null"); puts("}");
    } else {
        printf("portqueue: %s port=%u flags=0x%x stage=%u completed=0x%llx failedStage=%u status=0x%llx generation=%llu\n"
               "  RX/RSS/TX QPN=%u/%u/%u RX/TX CQN=%u/%u MAC base=%u slots=%u owners=0x%llx\n",
            good?"OK":"STOP",port,r.flags,r.stage,(unsigned long long)r.completedMask,r.failedStage,
            (unsigned long long)r.status,(unsigned long long)r.deviceGeneration,
            r.rxQpn,r.rssQpn,r.txQpn,r.rxCqn,r.txCqn,r.macBase,r.macSlots,(unsigned long long)r.ownerMask);
    }
    if (!good) fprintf(stderr,"portqueue: stopped; no retry/reset/cleanup (called=%d transport=0x%x bound=%d)\n",called,transport,bound);
    return good?0:1;
}

static int cmdBoundPcieJson(void)
{
    struct Cx3RuntimeSample before, after;
    struct Cx3PcieLinkResult p;
    size_t n = sizeof(p);
    uint32_t transport = 0xffffffffu;
    int beforeOk = readRuntimeSample(&before) == 0, afterAttempted = 0, matched = 0;
    memset(&p, 0, sizeof(p)); memset(&after, 0, sizeof(after));
    if (beforeOk) {
        if (!(before.result.lifecycleFlags & 15u))
            transport = (uint32_t)IOConnectCallMethod(gConn, kCx3SelPcieLink,
                NULL, 0, NULL, 0, NULL, NULL, &p, &n);
        afterAttempted = 1;
        if (readRuntimeSample(&after) == 0 && !transport && n == sizeof(p) &&
            runtimeSameInstance(&before, &after) && p.version == CX3_PCIE_RESULT_VERSION &&
            p.structBytes == sizeof(p) && !p.status && p.reason == CX3_PCIE_REASON_OK &&
            p.validity == ((p.pcieFlags & 15u) == 2 ? 31u : 15u) &&
            (p.pcieFlags & 15u) >= 1 && (p.pcieFlags & 15u) <= 2 && ((p.pcieFlags >> 4) & 15u) <= 1 &&
            p.deviceGeneration == before.result.deviceGeneration &&
            !memcmp(&p.identityBefore, &before.result.pciIdentity, sizeof(p.identityBefore)) &&
            !memcmp(&p.identityBefore, &p.identityAfter, sizeof(p.identityBefore)) &&
            pcieSampleShapeValid(&p) &&
            p.identityBefore.vendor == pcieRaw16(p.raw, 0) && p.identityBefore.device == pcieRaw16(p.raw, 2) &&
            p.identityBefore.subsystemVendor == pcieRaw16(p.raw, 0x2c) && p.identityBefore.subsystemId == pcieRaw16(p.raw, 0x2e) &&
            ((p.pcieFlags & 15u) == 2 || p.lnkCap2 == 0)) matched = 1;
    }
    printf("{\"schema\":\"cx3-bound-pcie-v1\",\"before\":"); emitRuntimeSample(&before);
    printf(",\"pcie\":{\"transport\":%u,\"returnedBytes\":%zu,\"rawHex\":\"", transport, n);
    if (!transport && n <= sizeof(p)) runtimeEmitHex(&p, n);
    printf("\"},\"after\":");
    if (afterAttempted) emitRuntimeSample(&after); else printf("null");
    printf(",\"identityBracketMatched\":%s}\n", matched ? "true" : "false");
    if (!matched) fprintf(stderr, "boundpcie: identity bracket or PCIe result incomplete; no binding established\n");
    return matched ? 0 : -1;
}
/* CX3_RUNTIME_CLI_END */

/* CX3_BOUND_RSS_CLI_BEGIN */
/* One existing RSS experiment, bracketed on this connection; never a retry.
 * Identity equality associates observations, not DMA safety or atomicity. */
static int cmdBoundRssJson(int allowExperiment)
{
    struct Cx3RuntimeSample before, after;
    Cx3R4IngressResult result;
    uint64_t out[kR4IngressOutCount];
    uint32_t count = kR4IngressOutCount, transport = 0xffffffffu;
    size_t size = sizeof(result), captured = 0;
    int beforeOk, afterAttempted = 0, matched = 0, completed = 0;
    unsigned i;
    if (!allowExperiment) {
        fprintf(stderr, "boundrss: --allow-rss-experiment required; selector 35 changes MAC/RSS QP state\n");
        return 2;
    }
    memset(&result, 0, sizeof(result)); memset(out, 0, sizeof(out));
    memset(&after, 0, sizeof(after));
    beforeOk = readRuntimeSample(&before) == 0;
    if (beforeOk) {
        if (!(before.result.lifecycleFlags & 15u)) {
            transport = (uint32_t)IOConnectCallMethod(gConn, kCx3SelR4Ingress,
                NULL, 0, NULL, 0, out, &count, &result, &size);
            if (!transport && size <= sizeof(result)) captured = size;
        }
        afterAttempted = 1;
        if (readRuntimeSample(&after) == 0 && !transport && size == sizeof(result) &&
            count == kR4IngressOutCount && runtimeSameInstance(&before, &after) &&
            result.abiVersion == CX3_R4INGRESS_ABI && result.abiBytes == sizeof(result) &&
            out[kR4IngressOutStatus] == result.status && out[kR4IngressOutQpn] == result.qpn)
            matched = 1;
    }
    {
        const Cx3QpCaptureObservation *q = &result.finalQueryCapture;
        /* status zero alone also describes an answered selector whose first
         * QP transition was refused. Require this call's final query receipt. */
        completed = matched && !result.status && result.qpn && result.qpn < (1u << 24) &&
            result.queryOk == 1 && result.queryState == 3 &&
            out[kR4IngressOutQueryOk] == 1 && out[kR4IngressOutQueryState] == 3 &&
            q->abiVersion == 1 && q->abiBytes == sizeof(*q) &&
            q->preparedBytes == sizeof(result.rawQueryMailboxFinal) &&
            q->capturedBytes == sizeof(result.rawQueryMailboxFinal) && !q->fwStatus &&
            q->command.abiVersion == 1 && q->command.abiBytes == sizeof(q->command) &&
            q->command.flags == 7 && !q->command.fwStatus &&
            before.result.sampleUptimeNs <= q->command.startNs &&
            q->command.startNs <= q->command.postedNs &&
            q->command.postedNs <= q->command.endNs &&
            q->command.endNs <= after.result.sampleUptimeNs;
    }
    printf("{\"schema\":\"cx3-bound-rss-v1\",\"before\":"); emitRuntimeSample(&before);
    printf(",\"rss\":{\"selector\":35,\"call\":%u,\"inCount\":0,\"in\":[],\"outCount\":%u,\"out\":[",
           transport, count);
    for (i = 0; i < count && i < kR4IngressOutCount; ++i)
        printf("%s\"0x%llx\"", i ? "," : "", (unsigned long long)out[i]);
    printf("],\"structLen\":%zu,\"structHex\":\"", captured);
    if (captured) runtimeEmitHex(&result, captured);
    printf("\"},\"after\":");
    if (afterAttempted) emitRuntimeSample(&after); else printf("null");
    printf(",\"identityBracketMatched\":%s}\n", matched ? "true" : "false");
    if (!matched) fprintf(stderr, "boundrss: identity bracket or result incomplete; no binding established\n");
    else if (!completed) fprintf(stderr, "boundrss: RSS stage lacks a successful final-query receipt; bound diagnostic retained (status 0x%x)\n", result.status);
    return completed ? 0 : -1;
}
/* CX3_BOUND_RSS_CLI_END */

/*
 * sizes -- what each selector's dispatch entry demands, as the kernel will check it.
 *
 * Why this has to exist.  IOExternalMethodDispatch does not accept "at least" for either
 * of its counts: the scalar output count and the structure output size must match what the
 * entry declares, exactly.  Nothing reports a mismatch as a mismatch -- a call with the
 * wrong sizes fails with kIOReturnBadArgument and leaves the output buffer zeroed, and zero
 * is the value this project's selectors use for "ok".  So a mis-sized call is
 * indistinguishable from a successful one that reported all is well.
 *
 * That is not hypothetical: it is what the first verifier did.  Its read-only section
 * called four structure-returning selectors with wrong sizes and reported a false PASS for
 * one of them, then attributed a fifth failure to a stale kext -- a plausible cause that
 * happened to be wrong, and which would have sent the reader to reboot for nothing.
 *
 * Printing the numbers from here rather than writing them down in the verifier is the same
 * rule the rest of this project runs on: the tool holds the types, so the tool computes
 * sizeof, and the pair cannot drift from the struct definitions.
 *
 * Output, one line per selector:
 *   <selector> <scalar-in-count> <scalar-out-count> <struct-out-size> <name>
 *
 * All three are checked for EXACT equality by the dispatch table, so all three matter.
 *
 * The PAIRING of selector to result type is written here and also in the kext's dispatch
 * table (Cx3Mlx4UserClient.cpp).  That duplication is real and is the one thing about this
 * design that can drift; it is called out here rather than left to be discovered.
 */
/*
 * txprobe -- the three discriminators of docs/M2B-NOTES.md section 52.
 *
 * The two things this can say that no scalar in r4raw can: whether the hardware executes ANY
 * descriptor from the send queue (a NOP carries no data and still completes), and whether an
 * MMIO doorbell on the UAR page the QPs were built with reaches the device at all (the receive
 * CQ's arm, judged by the COMP EQE it leaves in the event ring).
 *
 * It sends no firmware command, so unlike every other selector here it cannot poison the
 * command mailbox and cannot leave the device needing a reset: it is safe to run repeatedly on
 * a live stack ('tools/verify.sh up', then this).
 */
static int cmdTxProbe(unsigned postWaitMs, unsigned armWaitMs)
{
    uint64_t in[kTxProbeInCount];
    uint64_t out[kTxProbeOutCount];
    uint32_t n = kTxProbeOutCount;
    uint64_t refused, nop, nopOp, armRan, armFound, armType, armCqn, armOk;

    memset(in, 0, sizeof(in));
    memset(out, 0, sizeof(out));
    in[kTxProbeInNopWaitMs] = postWaitMs;
    in[kTxProbeInArmWaitMs] = armWaitMs;

    printf("txprobe: is the send queue executed at all, and does the doorbell reach the device?\n");
    fflush(stdout);
    if (call(kCx3SelTxProbe, in, kTxProbeInCount, out, &n) != 0) return -1;

    if (out[kTxProbeOutStatus] != 0) {
        fprintf(stderr, "  failed: status 0x%llx (see the kernel log)\n",
                (unsigned long long)out[kTxProbeOutStatus]);
        return -1;
    }
    refused = out[kTxProbeOutRefused];
    if (refused != CX3_TXPROBE_RAN) {
        printf("  REFUSED (code %llu): the transmit side is not built in this boot.\n"
               "  Run 'cx3ctl r4tx' first -- this selector posts into the queue r4tx creates.\n",
               (unsigned long long)refused);
        return -1;
    }

    nop = out[kTxProbeOutNopComps];
    nopOp = out[kTxProbeOutNopOpcode];
    printf("\n  1. NOP (opcode 0x%02x, no data, still completes) into the send queue:\n",
           (unsigned)MLX4_OPCODE_NOP);
    printf("     descriptor read back as encoded : %s\n",
           out[kTxProbeOutNopWqeOk] == 1 ? "yes" : "NO -- the queue does not hold the NOP");
    printf("     completions                     : %llu%s\n", (unsigned long long)nop,
           nop == 0 ? "  -- the hardware did not execute it" : "");
    if (nop != 0) {
        printf("     the first one                   : opcode 0x%02llx, status 0x%04llx\n",
               (unsigned long long)nopOp, (unsigned long long)out[kTxProbeOutNopStatus]);
    }
    printf("     the same post with SEND         : %llu completion(s)%s\n",
           (unsigned long long)out[kTxProbeOutSendComps],
           out[kTxProbeOutSendComps] == 0 ? " (the failure this stage started with)" : "");
    if (nop != 0)
        printf("     => the hardware DOES execute this send queue; the defect is in what a SEND\n"
               "        does with the descriptor, not in whether the queue is read.\n");
    else
        printf("     => the hardware does NOT execute descriptors from this send queue at all,\n"
               "        so no field of the descriptor can be the cause.\n");

    armRan   = out[kTxProbeOutArmRan];
    armFound = out[kTxProbeOutArmEqeFound];
    armType  = out[kTxProbeOutArmEqeType];
    armCqn   = out[kTxProbeOutArmEqeCqn];
    armOk    = out[kTxProbeOutArmEqeOk];
    printf("\n  2. the receive CQ's arm doorbell through UAR page 128 (0x20, 8 bytes):\n");
    if (armRan != 1) {
        printf("     SKIPPED -- the receive CQ or the event queue is not built in this boot\n"
               "     (that is about which stages have run, not about the hardware)\n");
    } else if (armFound != 1) {
        printf("     no EQE arrived within the budget -- an arm doorbell on the page the QPs\n"
               "     were built with produced no CQ event, while completions kept landing on\n"
               "     that queue.  The MMIO doorbell is the thing that did not reach the device.\n");
    } else {
        printf("     an EQE arrived: type 0x%02llx, CQN %llu -- %s\n",
               (unsigned long long)armType, (unsigned long long)armCqn,
               armOk == 1 ? "the arm's own CQ event (device.h:293), so the doorbell DID reach "
                            "the device"
                          : "NOT the arm's own event; see the kernel log for what it was");
    }

    printf("\n  3. the send doorbell record (the PRM's posting step 2):\n");
    printf("     variants tried                  : %llu\n",
           (unsigned long long)out[kTxProbeOutRecVariants]);
    if (out[kTxProbeOutRecVariants] != CX3_TXPROBE_NOT_RUN) {
        uint64_t w = out[kTxProbeOutRecWinner];
        /* Stored as variant + 1: variant 0 is the plain post with no record written. */
        printf("     first one that completed        : %s\n",
               w == 0 ? "none of them" :
               (w == 1 ? "variant 0 -- no record written (the reading this port has always used)"
                       : (w == 2 ? "variant 1 (record word 0 = 1)" :
                         (w == 3 ? "variant 2 (record word 1 = 1)"
                                 : "variant 3 (both words = 1)"))));
        if (w == 0)
            printf("     (not one of the four readings of that record changed the outcome)\n");
        else
            printf("     => the record IS read on the send path, and the reading that works is "
                   "variant %llu.\n", (unsigned long long)w);
    }
    return 0;
}

static int cmdSizes(void)
{
    struct { uint32_t sel; uint32_t in; uint32_t out; size_t sz; const char *name; } t[] = {
        { 0,  0, kProbeOutCount,      0,                          "probe" },
        { 1, 0, kDmaOutCount, 0, "dma" },
        { 2, kFwInCount, kFwOutCount, 0, "queryfw" },
        { 3, kRstInCount, kRstOutCount, 0, "reset" },
        { 4, kPeekInCount_, kPeekOutCount_, 0, "peek" },
        { 5, kClrInCount, kClrOutCount, 0, "clearowner" },
        { 6, kPokeInCount, kPokeOutCount, 0, "poke" },
        { 7, kFlrInCount, kFlrOutCount, 0, "flr" },
        { 8, kDevCapInCount, kDevCapOutCount, sizeof(struct Cx3DevCapResult), "querydevcap" },
        { 9, kIcmInCount, kIcmOutCount, 0, "icmalloc" },
        { 10, kFwLoadInCount, kFwLoadOutCount, 0, "fwload" },
        { 11, kFwAreaInCount_, kFwAreaOutCount, 0, "fwarea" },
        { 12, kFwOutboxInCount, kFwOutboxOutCount, 0, "fwoutbox" },
        { 13, kM2dInCount, kProfOutCount, sizeof(struct Cx3ProfileResult), "m2dprofile" },
        { 14, 0, kBringUpOutCount, 0, "m2dbringup" },
        { 15, kEqUpInCount, kEqUpOutCount, 0, "m3equp" },
        { 16, kEqPollInCount, kEqPollOutCount, 0, "m3eqpoll" },
        { 17, kEqRingInCount, kEqRingOutCount, 0, "m3eqring" },
        { 18, 0, kEqInfoOutCount, sizeof(struct Cx3EqInfoResult), "m3eqinfo" },
        { 19, kIrqUpInCount, kIrqUpOutCount, 0, "m3irqup" },
        { 20, kIrqTestInCount, kIrqTestOutCount, 0, "m3irqtest" },
        { 21, 0, kIrqStatusOutCount, 0, "m3irqstatus" },
        { 22, 0, kIrqDownOutCount, 0, "m3irqdown" },
        { 23, 0, kClrIntOutCount, 0, "m3clrint" },
        { 24, kQueryPortInCount, kQueryPortOutCount, sizeof(Cx3QueryPortResult), "queryport" },
        { 25, kPortUpInCount, kPortUpOutCount, 0, "m3portup" },
        { 26, kAbRingInCount, kAbRingOutCount, sizeof(Cx3AbRingResult), "m3abring" },
        { 27, 0, kQueryHcaOutCount, sizeof(Cx3QueryHcaResult), "queryhca" },
        { 28, kR4MrInCount, kR4MrOutCount, sizeof(Cx3R4MrResult), "r4mr" },
        { 29, kR4CqInCount, kR4CqOutCount, sizeof(Cx3R4CqResult), "r4cq" },
        { 30, kR4RqInCount, kR4RqOutCount, sizeof(Cx3R4RqResult), "r4rq" },
        { 31, kHcrInCount, kHcrOutCount, CX3_HCR_MAILBOX_SIZE, "hcr" },
        { 32, 0, kSurveyOutCount, sizeof(Cx3SurveyResult), "survey" },
        { 33, kFaultInCount, kFaultOutCount, 0, "fault" },
        { 34, kR4QpInCount, kR4QpOutCount, sizeof(Cx3R4QpResult), "r4qp" },
        { 35, kR4IngressInCount, kR4IngressOutCount, sizeof(Cx3R4IngressResult), "r4ingress" },
    { 36, kR4TxInCount, kR4TxOutCount, sizeof(Cx3R4TxResult), "r4tx" },
    { 37, kR4RawInCount, kR4RawOutCount, sizeof(Cx3R4RawResult), "r4raw" },
    { 38, kTxProbeInCount, kTxProbeOutCount, 0, "txprobe" },
    /*
     * 39 and 40 were missing, and that was a real defect rather than tidiness: verify.sh reads
     * this table (`cx3ctl sizes`) to learn each selector's in/out counts and structure size
     * before it calls one, so a selector that is not here is called with no outputs asked for --
     * and the chain step that drives it reports "the call failed" instead of anything about the
     * device.  netup's step was written before this row existed.
     */
    { 39, 0, kNetUpOutCount, 0, "netup" },
    { 40, 0, kNetStatOutCount, sizeof(Cx3NetStatResult), "netstat" },
    { 41, kNetLinkInCount, kNetLinkOutCount, 0, "netlink" },
    { 42, kNetOffInCount, kNetOffOutCount, 0, "netoff" },
    { 43, kNetArmInCount, kNetArmOutCount, 0, "netarm" },
    { 44, kNetFiltInCount, kNetFiltOutCount, 0, "netfilt" },
    { 45, kNetPortInCount, kNetPortOutCount, 0, "netport" },
    /* 46 is the interface step cut short after its first step: see kCx3SelNetUpStop.  It is in
     * this table because the table is what the build guard and `cx3ctl sizes` read, and a selector
     * missing from it is a selector the harness would call with no outputs asked for (DEFECTS F16). */
    { 46, kNetUpStopInCount, kNetUpOutCount, 0, "netupstop" },
    { 47, 0, 0, sizeof(struct Cx3AdapterResult), "queryadapter" },
    { 48, 0, 0, sizeof(struct Cx3PcieLinkResult), "pcielink" },
    { 49, 0, 0, sizeof(struct Cx3RuntimeIdentityResult), "identity" },
    { 50, 3, 0, sizeof(struct Cx3ModuleResult), "module" },
    { 51, 0, 0, sizeof(struct Cx3AutoNetworkResult), "autonet" },
    { 52, 2, 0, sizeof(struct Cx3PortInfoResult), "portinfo" },
    { 53, 3, 0, sizeof(struct Cx3PortQueueResult), "portqueue" },
    { 54, 3, 0, sizeof(struct Cx3NativePortResult), "nativeport" },
    { 55, 4, 0, sizeof(struct Cx3NativeConfigResult), "nativeconfig" },
    };
    unsigned i;
    for (i = 0; i < sizeof(t) / sizeof(t[0]); ++i)
        printf("%u %u %u %zu %s\n", t[i].sel, t[i].in, t[i].out, t[i].sz, t[i].name);
    return 0;
}


/*
 * rawjson -- call any selector and print the kernel's own values as JSON.
 *
 * Why this exists, and why it is one command rather than a --json flag on thirty-four:
 *
 * The verification harness has to decide PASS or FAIL without a human reading prose, and
 * this project has twice been misled by prose that was itself wrong -- a log line whose
 * format specifiers were one short printed a whole row of misaligned numbers, and a
 * "238 ms" turned out to be a constant read as a duration.  So the harness reads VALUES,
 * and the values come from the kernel unchanged.
 *
 * Putting JSON on every existing subcommand would mean thirty-four places deciding how to
 * describe themselves: thirty-four places to drift, and thirty-four copies of the
 * interpretation.  Instead this does exactly what the other commands do -- scalar inputs,
 * optional structure input, scalar outputs, optional structure output -- and prints the
 * buffers.  Interpretation stays in one file: the harness, which cross-checks its own
 * index constants against src/cx3_abi.h at run time, so drift is detected rather than
 * silently misread.
 *
 * Usage:
 *   cx3ctl rawjson SELECTOR [in a,b,c] [hex aabbcc] [out N] [struct N]
 *
 *   in a,b,c     scalar inputs, comma separated, decimal or 0x-prefixed
 *   hex aabbcc   structure input bytes, hex, optional colons or spaces
 *   out N        scalar outputs to request (default 16, the kernel's cap)
 *   struct N     structure-output bytes to request (default 0)
 *
 * Output is one line of JSON on stdout:
 *   {"selector":24,"call":0,"inCount":1,"in":[...],"outCount":16,"out":[...],
 *    "structLen":0,"structHex":""}
 *
 * "call" is the IOKit return code of the call itself: 0 means the kernel answered at all.
 * Whether the DEVICE did what was asked is a different question and lives inside out[] or
 * structHex -- deliberately not flattened here, because deciding what "ok" means is
 * interpretation, and interpretation belongs in exactly one place.
 */
static int cmdRawJson(int argc, char **argv, int first)
{
    uint64_t in[CX3_SCALAR_IN_MAX];
    uint64_t out[CX3_SCALAR_OUT_MAX];
    static uint8_t structIn[CX3_HCR_MAILBOX_SIZE];
    /* Diagnostics may contain several 4 KiB mailbox snapshots; never clamp them. */
    static uint8_t structOut[32768];
    uint32_t inCount = 0, outCount = 16;
    size_t structInLen = 0, structOutLen = 0, wantStructOut = 0;
    unsigned selector = 0;
    kern_return_t kr = KERN_FAILURE;
    int i;

    memset(in, 0, sizeof(in));
    memset(out, 0, sizeof(out));
    memset(structIn, 0, sizeof(structIn));
    memset(structOut, 0, sizeof(structOut));

    if (first >= argc) {
        fprintf(stderr, "rawjson: need a selector number\n");
        return 2;
    }
    selector = (unsigned)strtoul(argv[first], NULL, 0);

    for (i = first + 1; i < argc; ++i) {
        if (!strcmp(argv[i], "in") && i + 1 < argc) {
            const char *p = argv[++i];
            while (*p && inCount < CX3_SCALAR_IN_MAX) {
                char *end = NULL;
                in[inCount++] = (uint64_t)strtoull(p, &end, 0);
                if (!end || end == p) break;
                p = end;
                if (*p == ',') ++p;
            }
        } else if (!strcmp(argv[i], "hex") && i + 1 < argc) {
            const char *p = argv[++i];
            while (p[0] && p[1] && structInLen < sizeof(structIn)) {
                unsigned v;
                if (*p == ':' || *p == ' ') { ++p; continue; }
                if (sscanf(p, "%2x", &v) != 1) break;
                structIn[structInLen++] = (uint8_t)v;
                p += 2;
            }
        } else if (!strcmp(argv[i], "out") && i + 1 < argc) {
            outCount = (uint32_t)strtoul(argv[++i], NULL, 0);
            if (outCount > CX3_SCALAR_OUT_MAX) outCount = CX3_SCALAR_OUT_MAX;
        } else if (!strcmp(argv[i], "struct") && i + 1 < argc) {
            wantStructOut = (size_t)strtoul(argv[++i], NULL, 0);
            if (wantStructOut > sizeof(structOut)) {
                fprintf(stderr, "rawjson: requested structure exceeds %zu-byte capacity\n",
                        sizeof(structOut));
                return 2;
            }
        } else {
            fprintf(stderr, "rawjson: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (openService() != 0) {
        printf("{\"selector\":%u,\"call\":-1,\"note\":\"no service: is the kext loaded?\"}\n",
               selector);
        return 1;
    }

    {
        size_t soSz = wantStructOut;
        breadcrumb("rawjson: selector %u in %u out %u -- calling IOConnectCallMethod",
                   (unsigned)selector, (unsigned)inCount, (unsigned)outCount);
        kr = IOConnectCallMethod(gConn, selector,
                                 in, inCount,
                                 structInLen ? structIn : NULL, structInLen,
                                 out, &outCount,
                                 wantStructOut ? structOut : NULL, &soSz);
        breadcrumb("rawjson: selector %u returned 0x%x", (unsigned)selector, (unsigned)kr);
        /*
         * Report the smaller of what was asked for and what came back.  The alternative --
         * trusting the request -- is how a reader ends up looking at bytes the kernel never
         * wrote, which is the same shape as reading a field that was never filled.
         */
        structOutLen = (kr == KERN_SUCCESS && soSz <= wantStructOut) ? soSz : 0;
    }

    {
        uint32_t k;
        printf("{\"selector\":%u,\"call\":%d,\"inCount\":%u,\"in\":[",
               selector, (int)kr, inCount);
        for (k = 0; k < inCount; ++k)
            printf("%s\"0x%llx\"", k ? "," : "", (unsigned long long)in[k]);
        printf("],\"outCount\":%u,\"out\":[", outCount);
        for (k = 0; k < outCount; ++k)
            printf("%s\"0x%llx\"", k ? "," : "", (unsigned long long)out[k]);
        printf("],\"structLen\":%u,\"structHex\":\"", (unsigned)structOutLen);
        for (k = 0; k < structOutLen; ++k) printf("%02x", structOut[k]);
        printf("\"}\n");
    }
    fflush(stdout);
    return (kr == KERN_SUCCESS) ? 0 : 1;
}


/*
 * hcr -- send one HCR command with the parameters you choose.
 *
 * This is the answer to "every experiment needs a kext change and a reboot".  The driver
 * side is a single gated pass-through: it fills a mailbox from the bytes you give it, hands
 * your opcode/modifiers to the ordinary command path, and copies the mailbox back.  Nothing
 * is interpreted, which is the point -- so a question like "what does the firmware answer for
 * this field set to that value" costs one command here instead of an edit and a restart.
 *
 * It requires the boot argument cx3raw=1.  That is deliberate rather than tiresome: this can
 * send any command, including ones that make the hardware begin referring to host memory.
 * The switch means a published build is safe by default without anyone having to remember.
 *
 * Usage:
 *   cx3ctl hcr OPCODE [inmod N] [opmod N] [mailbox] [imm] [busy] [wait MS]
 *                    [inbox HEX] [outbox N]
 *
 * Examples:
 *   cx3ctl hcr 0x43 inmod 1 mailbox outbox 64      QUERY_PORT for port 1
 *   cx3ctl hcr 0x15 inmod 28 mailbox outbox 64     QUERY_EQ for EQN 28
 *   cx3ctl hcr 0x31 inmod 0x1f                     a NOP, immediate form
 */
static int cmdHcr(int argc, char **argv, int first)
{
    uint64_t in[kHcrInCount];
    uint64_t out[kHcrOutCount];
    uint32_t n = kHcrOutCount;
    static uint8_t inbox[CX3_HCR_MAILBOX_SIZE];
    static uint8_t outbox[CX3_HCR_MAILBOX_SIZE];
    unsigned opcode = 0, inmod = 0, opmod = 0, flags = 0, waitms = 0;
    unsigned inboxLen = 0, outboxLen = 0;
    int i;

    memset(in, 0, sizeof(in));
    memset(out, 0, sizeof(out));
    memset(inbox, 0, sizeof(inbox));
    memset(outbox, 0, sizeof(outbox));

    if (first >= argc) {
        fprintf(stderr, "hcr: need an opcode, e.g. 'hcr 0x43 inmod 1 mailbox outbox 64'\n");
        return 2;
    }
    opcode = (unsigned)strtoul(argv[first], NULL, 0);

    for (i = first + 1; i < argc; ++i) {
        if (!strcmp(argv[i], "inmod") && i + 1 < argc)
            inmod = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "opmod") && i + 1 < argc)
            opmod = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "wait") && i + 1 < argc)
            waitms = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "outbox") && i + 1 < argc)
            outboxLen = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "mailbox"))
            flags |= CX3_HCR_F_USE_MAILBOX;
        else if (!strcmp(argv[i], "imm"))
            flags |= CX3_HCR_F_WANT_IMM;
        else if (!strcmp(argv[i], "busy"))
            flags |= CX3_HCR_F_BYPASS_BUSY;
        else if (!strcmp(argv[i], "inbox") && i + 1 < argc) {
            const char *hex = argv[++i];
            unsigned k = 0;
            while (hex[0] && hex[1] && k < CX3_HCR_MAILBOX_SIZE) {
                unsigned v;
                if (sscanf(hex, "%2x", &v) != 1) break;
                inbox[k++] = (uint8_t)v;
                hex += 2;
                if (*hex == ':' || *hex == ' ') ++hex;
            }
            inboxLen = k;
            flags |= CX3_HCR_F_FILL_INBOX;
        } else {
            fprintf(stderr, "hcr: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    if (outboxLen > CX3_HCR_MAILBOX_SIZE) {
        fprintf(stderr, "hcr: outbox is capped at %u bytes\n", CX3_HCR_MAILBOX_SIZE);
        return 2;
    }

    in[kHcrInOpcode]     = opcode;
    in[kHcrInInModifier] = inmod;
    in[kHcrInOpModifier] = opmod;
    in[kHcrInFlags]      = flags;
    in[kHcrInTimeoutMs]  = waitms;
    in[kHcrInInboxLen]   = inboxLen;
    in[kHcrInOutboxLen]  = outboxLen;

    printf("hcr: opcode 0x%x, in_modifier 0x%x, op_modifier %u%s%s%s, inbox %u byte(s)\n",
           opcode, inmod, opmod, (flags & CX3_HCR_F_USE_MAILBOX) ? ", mailbox" : "",
           (flags & CX3_HCR_F_WANT_IMM) ? ", imm" : "",
           (flags & CX3_HCR_F_BYPASS_BUSY) ? ", bypass-busy" : "", inboxLen);
    fflush(stdout);

    {
        kern_return_t kr;
        size_t outSz = CX3_HCR_MAILBOX_SIZE;
        size_t inSz  = CX3_HCR_MAILBOX_SIZE;
        if (openService() != 0) return -1;
        kr = IOConnectCallMethod(gConn, kCx3SelHcr,
                                 in, kHcrInCount, inbox, inSz,
                                 out, &n, outbox, &outSz);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "selector %u failed: 0x%x\n", kCx3SelHcr, kr);
            return -1;
        }
    }

    if (!out[kHcrOutAllowed]) {
        printf("\n  REFUSED: the general command runner needs the boot argument cx3raw=1.\n"
               "  It can send any command, including ones that make the hardware refer to\n"
               "  host memory, so it is a switch.  Add cx3raw=1 to boot-args and reboot;\n"
               "  nothing else has to change.\n");
        return 3;
    }

    printf("  completed     : %s\n", out[kHcrOutCompleted] ? "yes" : "NO");
    printf("  fw status     : 0x%llx\n", (unsigned long long)out[kHcrOutFwStatus]);
    printf("  HCR after     : 0x%08llx\n", (unsigned long long)out[kHcrOutHcrFinal]);
    if (flags & CX3_HCR_F_WANT_IMM)
        printf("  immediate     : 0x%08llx%08llx\n",
               (unsigned long long)out[kHcrOutImmMsb],
               (unsigned long long)out[kHcrOutImmLsb]);

    if (outboxLen) {
        unsigned k;
        printf("\n  outbox, first %u byte(s) as they came back:\n", outboxLen);
        for (k = 0; k < outboxLen; k += 16) {
            unsigned j;
            printf("    %04x:", k);
            for (j = 0; j < 16 && k + j < outboxLen; ++j) printf(" %02x", outbox[k + j]);
            printf("\n");
        }
    }
    return out[kHcrOutCompleted] ? 0 : 1;
}

/*
 * survey -- everything the driver currently knows, in one call.
 *
 * No command is sent: this is the first thing to run when something is wrong, so it has to
 * be instant and incapable of making anything worse.  The value is in seeing the numbers
 * together -- a CQ whose completions cannot arrive, an MPT entry while the tables it lives in
 * are gone, an RQ ring whose lkey nothing resolves are all visible here and would otherwise
 * each cost a reboot to ask about.
 */
static const char *surveyName(const Cx3SurveyItem *it)
{
    static char buf[80];
    switch (it->kind) {
    case CX3_SURVEY_KIND_TABLE:
        /* The table names live in the kext; the tool only knows the order, which is the
         * order the profile built them in. */
        snprintf(buf, sizeof(buf), "table #%u", it->id);
        return buf;
    case CX3_SURVEY_KIND_QUEUE:
        switch (it->id) {
        case kSurveyQueueEq: return "eq";
        case kSurveyQueueMr: return "mr";
        case kSurveyQueueCq: return "cq";
        case kSurveyQueueRq: return "rq";
        default: return "queue ?";
        }
    default:
        switch (it->id) {
        case kSurveyFactPciHeld:        return "pci device held";
        case kSurveyFactBars:           return "bars mapped (bar0, bar2)";
        case kSurveyFactHcaState:       return "hca initialised, icm mapped";
        case kSurveyFactReservedEqUar:  return "reserved eqs, reserved uars (devcap)";
        case kSurveyFactCqCounts:       return "cqs: reserved, configured";
        case kSurveyFactPdCounts:       return "pds: reserved, total";
        case kSurveyFactMptCounts:      return "mpt: reserved, configured";
    case kSurveyFactCmdWedged:      return "command mailbox: poisoned, abandons, refused";
    case kSurveyFactCmdWedgedOp:    return "command mailbox: poisoned by op, when";
    case kSurveyFactCmdBudget:      return "command timeouts in force (ms)";
        case kSurveyFactInconsistent:   return "INCONSISTENT";
        default:                        return "fact ?";
        }
    }
}

/*
 * The network data path's counters.  The kext returns numbers and this function explains them:
 * that is this project's convention (the port decoder and survey do the same), and it keeps the
 * interpretation in one place a reader can check against the driver's own comments.
 *
 * The order is deliberate -- the fields that answer "did anything happen at all" come first,
 * because a reader who finds ticks 0 should stop reading; and every "why nothing happened" case
 * (no timer, no frame ring, the receive objects gone because a reset ran, the stack refusing
 * what it was handed) has its own line rather than being folded into a total.
 */
static int cmdNetStat(void)
{
    uint64_t out[kNetStatOutCount];
    uint32_t n = kNetStatOutCount;
    Cx3NetStatResult r;
    char mac[32];
    unsigned i, k;

    memset(out, 0, sizeof(out));
    memset(&r, 0, sizeof(r));

    printf("netstat: what the network data path has done, no device access\n");
    fflush(stdout);
    /*
     * Retried on a busy kernel, because this selector is deliberately TryLock: the data path's
     * timer holds the transaction lock for microseconds every millisecond, and a read that
     * lands in that window is refused with kIOReturnBusy rather than queued (it must stay
     * answerable while something is stuck -- the same reasoning as survey).  One refusal in a
     * hundred is not a finding about the driver, and a caller that reported it as "the call
     * failed" would be reporting the retry policy instead of the device.
     */
    {
        int attempt, rc = -1;
        for (attempt = 0; attempt < 5; ++attempt) {
            memset(out, 0, sizeof(out));
            memset(&r, 0, sizeof(r));
            rc = callStruct(kCx3SelNetStat, NULL, 0, out, &n, &r, sizeof(r));
            if (rc == 0) break;
            /*
             * Retried ONLY for kIOReturnBusy (0xe00002d5), and that distinction is the whole
             * point: a busy answer means the timer holds the transaction lock at this instant
             * and a retry in a moment will work, while any other refusal -- an older kext with
             * no selector 40 answers kIOReturnUnsupported -- is a statement about the driver and
             * retrying it four more times would only print the same line five times and imply a
             * transient that does not exist.
             */
            if ((uint32_t)rc != 0xe00002d5u) break;
            if (attempt == 0) {
                printf("  (busy: the data path's timer holds the transaction lock at this\n"
                       "   instant, which is what a TryLock read is for. Retrying.)\n");
            }
            usleep(20000);
        }
        if (rc != 0) {
            printf("  the counter read did not succeed (see the line above). On a kext older\n"
                   "  than this tool the selector does not exist yet: reboot with the staged\n"
                   "  one. Nothing was asked of the device.\n");
            return -1;
        }
        (void)rc;
    }

    {
        static const char hd[] = "0123456789abcdef";
        for (k = 0; k < 6; ++k) {
            unsigned b = (unsigned)((r.macBits >> (8 * (5 - k))) & 0xffu);
            mac[k * 3 + 0] = hd[(b >> 4) & 0xf];
            mac[k * 3 + 1] = hd[b & 0xf];
            mac[k * 3 + 2] = (k == 5) ? 0 : ':';
        }
        mac[18] = 0;
    }

    printf("  interface      en%u, %s, link %s at %u Mb/s\n",
           r.unit, r.macValid ? mac : "(no address)", r.linkUp ? "UP" : "DOWN", r.speedMbps);
    printf("  data path      %s, timer %s, transmit ring %s\n",
           r.enabled ? "ENABLED (the stack brought the interface up)"
                     : "not enabled: `ifconfig en%d up` is what starts it",
           r.timerCreated ? "created" : "MISSING -- no frame can be delivered",
           r.txRingPresent ? "allocated" : "MISSING -- every frame is dropped");
    if (!r.enabled) {
        printf("\n  Nothing below has run. That is a state, not a failure.\n");
        return 0;
    }

    printf("  timer          %llu tick(s), %llu skipped (a selector held the transaction lock),\n"
           "                 %llu tick(s) found the receive objects gone\n",
           (unsigned long long)r.ticks, (unsigned long long)r.ticksSkipped,
           (unsigned long long)r.noRings);
    if (r.ticks == 0) {
        printf("\n  The timer has never fired, so nothing could have been received or reclaimed.\n"
               "  That is the first thing to explain.\n");
        return 0;
    }
    if (r.noRings) {
        printf("\n  %llu tick(s) found no receive objects: something reset the HCA while the\n"
               "  interface was up, so the queues are gone. Re-run the chain.\n",
               (unsigned long long)r.noRings);
    }

    printf("  receive        %llu frame(s) handed to the stack, %llu byte(s)\n",
           (unsigned long long)r.rxFrames, (unsigned long long)r.rxBytes);
    printf("                 %llu taken by inputPacket but NOT submitted, %llu no-mbuf drop(s),\n"
           "                 %llu refill(s), %llu other-QPN completion(s), %u ring error(s)\n",
           (unsigned long long)r.rxNotDelivered, (unsigned long long)r.rxDropped,
           (unsigned long long)r.rxRefills, (unsigned long long)r.rxNotOurs, r.rxErrors);
    if (r.rxFrames == 0 && r.rxNotDelivered == 0 && r.rxDropped == 0) {
        printf("\n  No receive completion has been consumed at all: either no frame arrived\n"
               "  (nothing on the wire for this address) or nobody is reading the queue.\n"
               "  %llu tick(s) DID run, so it is not the timer.\n",
               (unsigned long long)r.ticks);
    }

    printf("  transmit       %llu frame(s) posted, %llu reclaimed\n",
           (unsigned long long)r.txFrames, (unsigned long long)r.txCompleted);
    printf("                 %llu stalled (ring full: handed back to the stack), %llu dropped,\n"
           "                 %u error completion(s), %llu frame(s) handed back on lock contention\n",
           (unsigned long long)r.txStalled, (unsigned long long)r.txDropped, r.txErrors,
           (unsigned long long)r.txLockBusy);
    if (r.txFrames > 0 && r.txCompleted == 0) {
        printf("\n  Frames were posted and NOT ONE completion came back: the device did not\n"
               "  execute them (or the completions went somewhere nobody reads). Read the\n"
               "  'net tx:' lines for what was written into the WQE.\n");
    }

    printf("  filters        multicast %s with %u address(es), promiscuous %s --\n"
           "                 requested and RECORDED, not programmed into the device\n",
           r.mcastMode ? "ON" : "off", r.mcastCount, r.promiscMode ? "ON" : "off");
    (void)i;
    return 0;
}

/*
 * M5-1: the media table and one reading of the link.
 *
 * Three modes, and the reason they are modes rather than one action is in the ABI header: the
 * media dictionary changes the link report the M4 acceptance depends on, so it is published when
 * asked and not before.  `publish` is what makes `ifconfig en<N> media` answer at all;
 * `requery` sends QUERY_PORT and republishes only if the reading changed, because the family
 * wants setLinkStatus on a CHANGE, not on every reading.
 */
static int cmdNetLink(const char *mode)
{
    uint64_t in[kNetLinkInCount], out[kNetLinkOutCount];
    uint32_t n = kNetLinkOutCount;
    unsigned m;

    if (!mode || strcmp(mode, "report") == 0)      m = CX3_NETLINK_REPORT;
    else if (strcmp(mode, "publish") == 0)         m = CX3_NETLINK_PUBLISH;
    else if (strcmp(mode, "requery") == 0)         m = CX3_NETLINK_REQUERY;
    else {
        fprintf(stderr, "netlink: mode must be report, publish or requery (got '%s')\n", mode);
        return 2;
    }

    memset(in, 0, sizeof(in));
    memset(out, 0, sizeof(out));
    in[kNetLinkInMode] = m;

    printf("netlink %s: media table and link reading, no frame is moved\n",
           m == CX3_NETLINK_REPORT ? "report" : (m == CX3_NETLINK_PUBLISH ? "publish" : "requery"));
    fflush(stdout);
    if (call(kCx3SelNetLink, in, kNetLinkInCount, out, &n) != 0) return -1;

    if (out[kNetLinkOutMediaCount] == 0) {
        printf("  media          NOTHING PUBLISHED, so the family has no dictionary and\n"
               "                 getSelectedMedium() answers 0. 'cx3ctl netlink publish' is what\n"
               "                 changes that\n");
    } else {
        printf("  media          %llu entr(ies) published, selected medium %s\n",
               (unsigned long long)out[kNetLinkOutMediaCount],
               out[kNetLinkOutSelected] ? "set (it is this driver's object)" : "NOT set");
    }
    printf("  link           %s at %llu Mb/s%s\n",
           out[kNetLinkOutLinkUp] ? "UP" : "DOWN",
           (unsigned long long)out[kNetLinkOutSpeedMbps],
           (m == CX3_NETLINK_REQUERY)
               ? (out[kNetLinkOutChanged] ? " (re-read: CHANGED, so setLinkStatus was called again)"
                                          : " (re-read: unchanged, so nothing was republished)")
               : "");
    if (out[kNetLinkOutLinkUp] == 0) {
        printf("                 A link that is DOWN is a reading, not a failure: it is what the\n"
               "                 port-up step measured and what the last QUERY_PORT said.\n");
    }
    return 0;
}

/*
 * M5-3: the offload switches.  Receive side only, and off by default -- the reasoning is in the
 * ABI header (a wrong per-frame verdict is worse than no verdict, and M4 has not run yet).
 *
 * The report says both things a reader needs kept apart: whether the switch is on, and what the
 * hardware has actually reported since.  A switch that is ON with rxGood 0 means no frame has
 * carried a verdict yet -- which is a statement about the traffic, not about the driver.
 */
static int cmdNetOff(const char *mode)
{
    uint64_t in[kNetOffInCount], out[kNetOffOutCount];
    uint32_t n = kNetOffOutCount;
    unsigned m;

    if (!mode || strcmp(mode, "report") == 0)  m = CX3_NETOFF_REPORT;
    else if (strcmp(mode, "rxon") == 0)        m = CX3_NETOFF_RX_ON;
    else if (strcmp(mode, "rxoff") == 0)       m = CX3_NETOFF_RX_OFF;
    else if (strcmp(mode, "txon") == 0)        m = CX3_NETOFF_TX_ON;
    else if (strcmp(mode, "txoff") == 0)       m = CX3_NETOFF_TX_OFF;
    else {
        fprintf(stderr, "netoff: mode must be report, rxon, rxoff, txon or txoff (got '%s')\n",
                mode);
        return 2;
    }

    memset(in, 0, sizeof(in));
    memset(out, 0, sizeof(out));
    in[kNetOffInMode] = m;

    printf("netoff %s: what this driver offloads\n",
           m == CX3_NETOFF_REPORT ? "report" : (m == CX3_NETOFF_RX_ON ? "rxon" : "rxoff"));
    fflush(stdout);
    if (call(kCx3SelNetOff, in, kNetOffInCount, out, &n) != 0) return -1;

    printf("  receive csum   switch %s\n", out[kNetOffOutRxOn] ? "ON" : "off");
    printf("                 %llu frame(s) marked verified (the CQE said IPOK + 0xffff),\n"
           "                 %llu frame(s) left unmarked (the stack checksums those itself)\n",
           (unsigned long long)out[kNetOffOutRxGood],
           (unsigned long long)out[kNetOffOutRxNone]);
    printf("  transmit csum  switch %s\n", out[kNetOffOutTxOn] ? "ON" : "off");
    printf("                 %llu frame(s) posted with a hardware checksum request,\n"
           "                 %llu posted with none (the stack inserted it)\n",
           (unsigned long long)out[kNetOffOutTxGood],
           (unsigned long long)out[kNetOffOutTxNone]);
    if (!out[kNetOffOutTxOn]) {
        printf("                 OFF means getChecksumSupport() answers 'no' for the output path,\n"
               "                 so the stack inserts every transmit checksum itself -- which is what\n"
               "                 M4 was designed against. The family asks that question ONCE, at\n"
               "                 attach: 'netoff txon' has to be run BEFORE the interface is\n"
               "                 attached (before the chain's netup step) to take effect\n");
    }
    if (out[kNetOffOutRxOn] && out[kNetOffOutRxGood] == 0 && out[kNetOffOutRxNone] == 0) {
        printf("  note: the switch is on but no frame has arrived since -- nothing has been\n"
               "        judged either way yet.\n");
    }
    return 0;
}

/*
 * The arm experiment, done the way its own semantics require: drain the receive CQ first, then
 * arm, then look.  Stage 9 armed a queue that was NOT empty and measured zero events, and that
 * measurement could not be interpreted in either direction -- so this prints the precondition
 * before the result, and says which of the four outcomes it is.
 */
static int cmdNetArm(unsigned waitMs)
{
    uint64_t in[kNetArmInCount], out[kNetArmOutCount];
    uint32_t n = kNetArmOutCount;
    int rc;

    memset(in, 0, sizeof(in));
    memset(out, 0, sizeof(out));
    in[kNetArmInWaitMs] = waitMs;

    printf("netarm: drain the receive CQ, arm it, and watch the event queue%s\n",
           waitMs ? "" : " (default wait)");
    fflush(stdout);
    rc = call(kCx3SelNetArm, in, kNetArmInCount, out, &n);
    /*
     * The same check as netfilt's, and it matters more here: a failed call leaves the outputs at
     * zero, and zero is CX3_NETARM_RAN -- so without this the tool would print a complete
     * experiment report (drained 0, empty, armed, no event) that describes a run which never
     * happened.  A fabricated measurement is worse than no measurement, which is this project's
     * oldest rule.
     */
    if (rc != 0) {
        printf("  the call did not run (see the line above), so NOTHING was measured. On a kext\n"
               "  older than this tool the selector does not exist yet: reboot with the staged\n"
               "  one. Nothing was asked of the device.\n");
        return rc;
    }

    if (out[kNetArmOutRefused] != CX3_NETARM_RAN) {
        const char *why =
            out[kNetArmOutRefused] == CX3_NETARM_NO_CQ ? "no receive CQ in this boot (run r4cq)" :
            out[kNetArmOutRefused] == CX3_NETARM_NO_EQ ? "no event queue (run m3equp)" :
            out[kNetArmOutRefused] == CX3_NETARM_IFACE_UP ?
                "the network data path is enabled and owns this queue: bring the interface down" :
            out[kNetArmOutRefused] == CX3_NETARM_IRQ_UP ?
                "an interrupt session is open and would consume the very EQE this waits for: run m3irqdown" :
                "unknown refusal";
        printf("  REFUSED: %s\n", why);
        printf("  Nothing was measured. That is a statement about the state of this boot, not\n"
               "  about the hardware.\n");
        return rc;
    }

    printf("  1. drain      %llu completion(s) consumed and re-posted; the CQ was %s when the\n"
           "                 arm was rung\n",
           (unsigned long long)out[kNetArmOutDrained],
           out[kNetArmOutCqEmpty] ? "EMPTY" : "STILL NOT EMPTY");
    printf("  2. EQ drained %llu event(s) that were already there, so what follows is\n"
           "                 attributable to this arm\n",
           (unsigned long long)out[kNetArmOutEqeBefore]);
    printf("  3. arm        rung through the QP's own UAR page; watching for %llu ms\n",
           (unsigned long long)out[kNetArmOutWaitMs]);
    printf("  4. result     ");
    if (out[kNetArmOutEqeOk]) {
        printf("A COMP EVENT naming this CQ arrived -- the arm reaches the device.\n");
        printf("                 THE LINK IS PROVED: arm -> COMP EQE -> event queue.\n");
    } else if (out[kNetArmOutEqeFound]) {
        printf("an EQE arrived but it is not this arm's event: type 0x%llx, CQN %llu\n",
               (unsigned long long)out[kNetArmOutEqeType],
               (unsigned long long)out[kNetArmOutEqeCqn]);
        printf("                 (a different answer, not a silent one)\n");
    } else if (!out[kNetArmOutCqEmpty]) {
        printf("no EQE -- and THE PRECONDITION WAS NOT MET (the queue was not empty).\n");
        printf("                 This run says nothing about the arm; it is the same invalid\n"
               "                 experiment stage 9 ran. Re-run with less traffic.\n");
    } else {
        printf("no EQE, with the queue empty and the arm rung. That is a real reading:\n");
        printf("                 on this path the arm does not produce an event.\n");
    }
    return rc;
}

/*
 * M5-8: the hardware receive filters.  Turning multicast on REPLAYS the addresses the stack has
 * already asked for -- the stack only calls setMulticastList when its list changes, so without a
 * replay the switch would look like it did nothing until something unrelated changed.
 *
 * Turning it off does NOT rewrite the device's table, and this prints that rather than implying
 * the addresses were removed.
 */
static int cmdNetFilt(const char *mode)
{
    uint64_t in[kNetFiltInCount], out[kNetFiltOutCount];
    uint32_t n = kNetFiltOutCount;
    unsigned m;
    int rc;

    if (!mode || strcmp(mode, "report") == 0)   m = CX3_NETFILT_REPORT;
    else if (strcmp(mode, "mcaston") == 0)      m = CX3_NETFILT_MCAST_ON;
    else if (strcmp(mode, "mcastoff") == 0)     m = CX3_NETFILT_MCAST_OFF;
    else if (strcmp(mode, "promiscon") == 0)    m = CX3_NETFILT_PROMISC_ON;
    else if (strcmp(mode, "promiscoff") == 0)   m = CX3_NETFILT_PROMISC_OFF;
    else {
        fprintf(stderr, "netfilt: mode must be report, mcaston, mcastoff, promiscon or "
                        "promiscoff (got '%s')\n", mode);
        return 2;
    }

    memset(in, 0, sizeof(in));
    memset(out, 0, sizeof(out));
    in[kNetFiltInMode] = m;

    printf("netfilt %s: the port's receive filters\n",
           m == CX3_NETFILT_REPORT ? "report" :
           (m == CX3_NETFILT_MCAST_ON ? "mcaston" :
            (m == CX3_NETFILT_MCAST_OFF ? "mcastoff" :
             (m == CX3_NETFILT_PROMISC_ON ? "promiscon" : "promiscoff"))));
    fflush(stdout);
    rc = call(kCx3SelNetFilt, in, kNetFiltInCount, out, &n);
    /*
     * NOTHING below is printed when the call itself failed, and this was a real defect found by
     * running the command against a kext that does not have the selector: the outputs are then
     * whatever the memset left -- zeros -- and a report of zeros reads as "the switch is off and
     * nothing has been programmed", which is a statement about the driver rather than about the
     * call.  Every other command here checks this first; this one did not.
     */
    if (rc != 0) {
        printf("  the call did not run (see the line above), so NOTHING is reported. On a kext\n"
               "  older than this tool the selector does not exist yet: reboot with the staged\n"
               "  one. Nothing was asked of the device.\n");
        return rc;
    }

    printf("  multicast      programming %s; %llu address(es) remembered by the driver\n",
           out[kNetFiltOutMcastOn] ? "ON" : "off",
           (unsigned long long)out[kNetFiltOutStored]);
    printf("                 the table has been written %llu time(s), last fw status 0x%llx\n",
           (unsigned long long)out[kNetFiltOutPrograms],
           (unsigned long long)out[kNetFiltOutLastFw]);
    if (out[kNetFiltOutTruncated] != 0 && out[kNetFiltOutTruncated] != 0xffffffffu) {
        printf("                 %llu address(es) did NOT fit the driver's list and are not\n"
               "                 programmed\n", (unsigned long long)out[kNetFiltOutTruncated]);
    }
    printf("  promiscuous    %s (last qpn_calc fw status 0x%llx)\n",
           out[kNetFiltOutPromiscOn] ? "ON -- every station's frames are accepted"
                                     : "off -- the unicast filter is doing its job",
           (unsigned long long)out[kNetFiltOutPromiscFw]);
    if (out[kNetFiltOutPromiscOn]) {
        printf("                 under A0 this is one bit of the port's qpn_calc command, the\n"
               "                 same one the bring-up sends; turning it on also disables the\n"
               "                 multicast table, as the reference does  (accepting everything\n"
               "                 includes the group addresses)\n");
    }
    if (!out[kNetFiltOutMcastOn]) {
        printf("\n  The device's group table is NOT being written by this driver. What is in it\n"
               "  stays there until something else writes it (a power cycle, or a chain run,\n"
               "  which redoes the port bring-up). Unicast and broadcast are unaffected either\n"
               "  way: they are the firmware MAC table's business, not this one's.\n");
    } else {
        printf("\n  Broadcast is seeded into the table before the group addresses and the table\n"
               "  is enabled only after they are all in -- a table enabled without broadcast\n"
               "  would stop broadcast delivery, which is the one way this could break\n"
               "  something that already worked.\n");
    }
    return rc;
}

/*
 * M5's prerequisite: stopping the port, which is the first half of any configuration change the
 * port has to be re-opened for (the reference's change_mtu is stop-then-start).
 *
 * There is no "up" here on purpose: re-opening a port IS the port-up sequence the chain runs, and
 * the MTU is one of that step's inputs -- so the recipe for a jumbo (or any MTU) change is
 * `netport down`, then `m3portup 1 <mtu>`, plus whatever the frame buffers need.  A second copy
 * of the three-step order here would be a second place to get it wrong.
 */
static int cmdNetPort(const char *mode)
{
    uint64_t in[kNetPortInCount], out[kNetPortOutCount];
    uint32_t n = kNetPortOutCount;
    unsigned m;
    int rc;

    if (!mode || strcmp(mode, "report") == 0)  m = CX3_NETPORT_REPORT;
    else if (strcmp(mode, "down") == 0)        m = CX3_NETPORT_DOWN;
    else {
        fprintf(stderr, "netport: mode must be report or down (got '%s')\n", mode);
        return 2;
    }

    memset(in, 0, sizeof(in));
    memset(out, 0, sizeof(out));
    in[kNetPortInMode] = m;

    printf("netport %s: the port's own state\n", m == CX3_NETPORT_REPORT ? "report" : "down");
    fflush(stdout);
    rc = call(kCx3SelNetPort, in, kNetPortInCount, out, &n);
    if (rc != 0) {
        printf("  the call did not run (see the line above), so NOTHING is reported. On a kext\n"
               "  older than this tool the selector does not exist yet: reboot with the staged\n"
               "  one. Nothing was asked of the device.\n");
        return rc;
    }

    printf("  port           %s (last CLOSE_PORT fw status 0x%llx)\n",
           out[kNetPortOutDown] ? "STOPPED by this driver" : "not stopped by this driver",
           (unsigned long long)out[kNetPortOutFw]);
    printf("  link reading   %s\n", out[kNetPortOutLinkUp] ? "UP" : "DOWN");
    if (out[kNetPortOutDown]) {
        printf("\n  The port is closed. Re-opening it is the chain's port-up step:\n"
               "    sudo cx3ctl m3portup 1 <mtu>      (1500 for the default, 9000 for jumbo)\n"
               "  and that step's own log says which of its three commands each line belongs\n"
               "  to. The queues this driver built are untouched by either direction.\n");
    }
    return rc;
}

static int cmdSurvey(void)
{
    uint64_t out[kSurveyOutCount];
    uint32_t n = kSurveyOutCount;
    Cx3SurveyResult res;
    unsigned i;
    int bad = 0;

    memset(out, 0, sizeof(out));
    memset(&res, 0, sizeof(res));

    printf("survey: the driver's current state, no command sent\n");
    fflush(stdout);
    if (callStruct(kCx3SelSurvey, NULL, 0, out, &n, &res, sizeof(res)) != 0) return -1;

    printf("  %u entr(ies)\n\n", res.entries);
    for (i = 0; i < res.entries && i < CX3_SURVEY_MAX_ENTRIES; ++i) {
        const Cx3SurveyItem *it = &res.item[i];
        if (it->kind == CX3_SURVEY_KIND_TABLE) {
            printf("    %-42s chunks %u, objects %u, bytes %llu\n", surveyName(it),
                   it->value0, it->value1, (unsigned long long)it->value64);
            continue;
        }
        if (it->kind == CX3_SURVEY_KIND_QUEUE) {
            if (it->id == kSurveyQueueEq)
                printf("    %-42s up %u, eqn %u, cons %llu\n", surveyName(it), it->value0,
                       it->value1, (unsigned long long)it->value64);
            else
                printf("    %-42s up %u, v0 %u, v1 %u\n", surveyName(it), it->value0,
                       it->value1, (unsigned)it->value64);
            continue;
        }
        if (it->id == kSurveyFactInconsistent) {
            ++bad;
            printf("    %-42s ", surveyName(it));
            switch (it->value0) {
            case kSurveyInconsCqWithoutEq:
                printf("a CQ exists but no EQ does -- its completions cannot arrive\n");
                break;
            case kSurveyInconsMrWithoutIcm:
                printf("an MPT entry is enabled but the icm tables are released\n");
                break;
            case kSurveyInconsRqWithoutMr:
                printf("an RQ ring exists but no MR is enabled -- its WQEs carry an lkey "
                       "nothing resolves\n");
                break;
            default: printf("code %u\n", it->value0); break;
            }
            continue;
        }
        /*
         * The command-timeout facts, spelled out.  The generic "%u / %u" line below would
         * print them, but it would drop value64 -- and value64 is where the numbers that
         * answer the reader's question live (how many commands were refused; which budget is
         * in force).  A fact that is printed but not readable is not observable, which is
         * the rule this whole batch is built around.
         */
        if (it->id == kSurveyFactCmdWedged) {
            printf("    %-42s %s, %u abandonment(s), %llu command(s) refused while it held\n",
                   surveyName(it), it->value0 ? "POISONED" : "usable",
                   it->value1, (unsigned long long)it->value64);
            continue;
        }
        if (it->id == kSurveyFactCmdWedgedOp) {
            if (!it->value0) {
                printf("    %-42s none -- nothing has been abandoned\n", surveyName(it));
            } else {
                printf("    %-42s op 0x%x, %s posting, at %llu ms uptime\n", surveyName(it),
                       it->value0,
                       it->value1 ? "BEFORE" : "after",
                       (unsigned long long)(it->value64 / 1000000ull));
            }
            continue;
        }
        if (it->id == kSurveyFactCmdBudget) {
            printf("    %-42s polled idle wait %u, event idle wait %u, "
                   "firmware budget %u, ICM budget %u\n",
                   surveyName(it), it->value0, it->value1,
                   (unsigned)(it->value64 >> 32),
                   (unsigned)(it->value64 & 0xffffffffu));
            continue;
        }
        printf("    %-42s %u / %u\n", surveyName(it), it->value0, it->value1);
    }
    if (bad)
        printf("\n  %d inconsistency(ies) found.  That is what this call is for.\n", bad);
    else
        printf("\n  No inconsistency among the things this call can see.  (It checks what the\n"
               "  driver believes, not what the hardware holds -- for that, use the individual\n"
               "  readback selectors.)\n");
    return bad ? 1 : 0;
}
/*
 * r4qp -- R4 stage 4: the RX QP three steps to RTS, then the producer index.
 *
 * Everything this prints is one of the three layers the stage is judged on, because they
 * fail in ways that look alike and call for different responses:
 *
 *   the three commands and the state read back after EACH of them -- "the command returned
 *   fw status 0" is not "the hardware is in the state asked for", and a dropped middle step
 *   still leaves a final state of RTS;
 *
 *   the last QUERY_QP, field by field against what was written -- one read answering "is
 *   this our QP" (local_qpn) and "did the firmware keep the context" (the queue fields);
 *
 *   the producer read back out of host memory -- the delivery action of this stage, and the
 *   only one of the three that is a write.
 *
 * The QP number is printed WITH the rule it came from (the firmware's reserved share, the
 * same rule the CQN follows), so a wrong rule is visible instead of being hidden behind a
 * number that happens to work.
 */
static void printQpCapture(const char *label, const Cx3QpCaptureObservation *c)
{
    if (c->abiVersion == 0) {
        printf("  capture %s: not called (no command/capture evidence)\n", label);
        return;
    }
    if (c->abiVersion != CX3_QP_CAPTURE_OBSERVATION_ABI || c->abiBytes != sizeof(*c)) {
        printf("  capture %s: invalid capture ABI %u/%u\n", label, c->abiVersion, c->abiBytes);
        return;
    }
    printf("  capture %s: prepared=%u captured=%u method-fw=0x%x", label,
           c->preparedBytes, c->capturedBytes, c->fwStatus);
    if (!(c->command.flags & kCx3CmdObservedEntered)) {
        printf("; HCR not entered\n");
        return;
    }
    if (c->command.abiVersion != CX3_COMMAND_OBSERVATION_ABI ||
        c->command.abiBytes != sizeof(c->command)) {
        printf("; invalid command observation ABI\n");
        return;
    }
    printf("; HCR flags=0x%x fw=0x%x ns(start/post/end)=%llu/%llu/%llu\n",
           c->command.flags, c->command.fwStatus,
           (unsigned long long)c->command.startNs,
           (unsigned long long)c->command.postedNs,
           (unsigned long long)c->command.endNs);
}

static void printR4Captures(const Cx3QpCaptureObservation *sent,
                          const Cx3QpCaptureObservation *queried,
                          const Cx3QpCaptureObservation *finalQuery)
{
    unsigned i;
    char label[32];
    printf("  capture flags: 1=HCR entered, 2=host GO store/barrier, 4=completion status path;\n"
           "  no flag proves device acceptance, QPC correctness or DMA quiescence.\n");
    for (i = 0; i < 3; ++i) {
        snprintf(label, sizeof(label), "modify-%u", i + 1);
        printQpCapture(label, &sent[i]);
        snprintf(label, sizeof(label), "query-%u", i + 1);
        printQpCapture(label, &queried[i]);
    }
    printQpCapture("query-final", finalQuery);
}

static int cmdR4Qp(unsigned qpnIn)
{
    uint64_t in[kR4QpInCount];
    uint64_t out[kR4QpOutCount];
    uint32_t n = kR4QpOutCount;
    Cx3R4QpResult res;
    unsigned i;

    in[kR4QpInQpn] = qpnIn;
    memset(out, 0, sizeof(out));
    memset(&res, 0, sizeof(res));

    printf("r4qp: the RX QP: RST2INIT -> INIT2RTR -> RTR2RTS, then the RQ producer\n");
    printf("  needs stages 1-3 (MR, CQ, RQ). The QP context goes at mailbox +8, not +0;\n");
    printf("  %s\n", qpnIn ? "the QPN is the one given on the command line"
                            : "the QPN comes from the rule (the firmware's reserved share)");
    fflush(stdout);

    if (callStruct(kCx3SelR4Qp, in, kR4QpInCount, out, &n, &res, sizeof(res)) != 0)
        return -1;

    if (res.abiVersion != CX3_R4QP_ABI || res.abiBytes != sizeof(res)) {
        printf("  ABI mismatch: QP version %u bytes %u, want %u/%zu\n",
               res.abiVersion, res.abiBytes, CX3_R4QP_ABI, sizeof(res));
        return -1;
    }
    printR4Captures(res.modifyCapture, res.queryCapture, &res.finalQueryCapture);
    printf("  selector status   : 0x%llx\n", (unsigned long long)out[kR4QpOutStatus]);
    if (res.status != 0) {
        if (res.status == (uint32_t)kIOReturnNotReady)
            printf("  not ready: needs an initialised HCA and stages 1, 2 and 3 to have "
                   "run\n");
        else if (res.status == (uint32_t)kIOReturnBadArgument)
            printf("  refused: the QPN is outside the range software may use -- see the "
                   "kernel log\n");
        else if (res.status == (uint32_t)kIOReturnNoMemory)
            printf("  no memory for the producer record\n");
        else
            printf("  failed: status 0x%x -- see the kernel log\n", res.status);
        return -1;
    }

    printf("  QPN               : %u   (rule: the firmware's reserved share %u; the "
           "profile\n                            configured %u QPs)\n",
           res.qpn, res.qpnRule, res.qpnProfileMax);
    printf("  local_qpn readback: %u  %s\n", res.queryLocalQpn,
           res.qpnConfirmed ? "<- the command was given this QP and the firmware agrees"
                            : "<- DOES NOT MATCH what was written");
    printf("  context           : pd %u, usr_page %u (the UAR index), cqn_send/recv %u\n",
           res.pdn, res.uarIndex, res.cqn);

    printf("\n  layer 1: the three transitions, each followed by its own QUERY_QP\n");
    for (i = 0; i < 3; ++i) {
        const char *name = (i == 0) ? "RST2INIT " : (i == 1) ? "INIT2RTR " : "RTR2RTS ";
        printf("    %s(0x%02x): fw status 0x%x %s;  state after: ", name, res.stepOpcode[i],
               res.stepFwStatus[i], res.stepFwStatus[i] == 0 ? "ok " : "REJECTED");
        if (!res.stepQueryOk[i]) printf("? (QUERY_QP did not answer)\n");
        else printf("%u (wanted %u) %s\n", res.stepStateAfter[i], res.stepStateWanted[i],
                    res.stepStateAfter[i] == res.stepStateWanted[i] ? "ok" : "MISMATCH");
    }
    printf("    commands accepted %u/3, states confirmed by readback %u/3\n",
           res.stepsAccepted, res.statesConfirmed);
    if (res.stepsAccepted != 3 || res.statesConfirmed != 3)
        printf("      -> a command that returns 0 and a state that was read back are two\n"
               "         different facts. Read the kernel log: it prints the per-step fw "
               "status\n         and what QUERY_QP said after each one.\n");

    printf("\n  layer 2: the last QUERY_QP against what was written (written / read)\n");
    printf("    fw status 0x%x, flags 0x%08x -> state %u (RTS is 3)\n", res.queryFwStatus,
           res.queryFlagsRaw, res.queryState);
    printf("    rq_size_stride   0x%02x / 0x%02x\n", res.rqSizeStrideSent,
           res.queryRqSizeStride);
    printf("    sq_size_stride   0x%02x / 0x%02x\n", res.sqSizeStrideSent,
           res.querySqSizeStride);
    printf("    mtu_msgmax       0x%02x / 0x%02x\n", res.mtuMsgmaxSent, res.queryMtuMsgmax);
    printf("    cqn_send         %u / %u\n", res.cqn, res.queryCqnSend);
    printf("    cqn_recv         %u / %u\n", res.cqn, res.queryCqnRecv);
    printf("    db_rec_addr      0x%llx / 0x%llx\n",
           (unsigned long long)res.dbRecordPhys, (unsigned long long)res.queryDbRecAddr);
    printf("    mtt_base (h/l)   0x%02x/0x%08x / 0x%02x/0x%08x   log_page_size 0 / %u\n",
           (unsigned)(res.mttAddr >> 32), (unsigned)(res.mttAddr & 0xffffffffull),
           res.queryMttAddrH, (unsigned)res.queryMttAddrL, res.queryLogPageSize);
    printf("    all eight compared fields : %s (mask 0x%02x)\n",
           res.queryAllFieldsMatch ? "MATCH" : "MISMATCH", res.queryFieldMask);
    printf("    the MTT address survived  : %s\n", res.queryMttMatches ? "yes" : "NO");
    if (!res.queryAllFieldsMatch) {
        static const char *names[] = { "state", "local_qpn", "rq_size_stride",
                                       "sq_size_stride", "cqn_send", "cqn_recv",
                                       "db_rec_addr", "mtu_msgmax" };
        printf("      -> which one: ");
        for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
            if (!(res.queryFieldMask & (1u << i))) printf("%s ", names[i]);
        printf("\n");
        printf("         The firmware rewrote a field the context sent. Check nothing else\n"
               "         wrote to the QP in between.\n");
    }
    if (!res.queryMttMatches)
        printf("      -> the MTT address belongs to the RST->INIT step alone and is copied\n"
               "         through the other two, so a difference here means the shared\n"
               "         context was disturbed between steps.\n");

    printf("\n  layer 3: the RQ producer -- a %u-bit write into HOST MEMORY, not a "
           "doorbell\n", res.producerWidth);
    printf("    record DMA address : 0x%llx  (the QP context's db_rec_addr)\n",
           (unsigned long long)res.dbRecordPhys);
    printf("    value written      : %u  (%u WQEs, one buffer per WQE from stage 3)\n",
           res.producerValue, res.nent);
    printf("    read back          : %u  (it held %u before the write) %s\n",
           res.producerReadback, res.producerBefore,
           res.producerMatches ? "MATCH" : "MISMATCH");
    if (!res.producerMatches)
        printf("      -> the record does not hold what was written, so the hardware would\n"
               "         read a count nobody published. The QP was put back to RST.\n");

    printf("\n  the three fields of the context that decide whether a frame can land:\n");
    printf("    param3            : written 0x%08x, read back out of the context 0x%08x\n"
           "                        FCS_KEEP: the device %s it (caps.flags bit 34 = %u), so "
           "bit 29\n                        (%s) is %s. Stages 9/10 must expect the 4-byte\n"
           "                        Ethernet FCS in received frames when it is set.\n",
           res.param3Sent, res.queryParam3,
           res.fcsKeep ? "advertises" : "does not advertise", res.fcsKeep,
           res.fcsKeep ? "keep the FCS" : "let the firmware strip it",
           (res.param3Sent & (1u << 29)) ? "SET" : "clear");
    printf("    counter_index     : 0x%02x (0xFF is how the reference says \"no counter\")\n",
           res.counterIndexSent);
    printf("    sched_queue       : written 0x%02x, read back 0x%02x (0x83 | (port-1) << 6)\n",
           res.schedQueueSent, res.querySchedQueue);
    printf("    RoCE V1/V2        : %s (bmme_flags bit 19); roce_entropy is 0 on purpose\n",
           res.roceAdvertised ? "advertised by the device" : "not advertised");

    printf("\n  Stage 4 done: QPN %u is in RTS and the receive queue knows it has %u WQEs.\n"
           "  Whether frames arrive is stages 5-10 -- this stage claims reachability, not "
           "traffic.\n", res.qpn, res.producerReadback);
    return 0;
}

/*
 * r4ingress -- R4 stage 5: the port's MAC table, and the single-ring RSS indirection QP.
 *
 * Neither half produces a completion event, so neither can be judged by traffic -- and the two
 * readbacks are not the same kind of thing:
 *
 *   the MAC table  the reference never reads this table back (it keeps its own copy in software,
 *                  mlx4_port.c:182-340) and has no `get_port_mac_table` at all, so the read
 *                  direction -- the same SET_PORT with in_param 0 -- is THIS PORT'S PROBE.  The
 *                  tool prints it as a probe and, when it does not answer, says "unavailable"
 *                  rather than "the MAC was not registered": those are different claims and only
 *                  the first is established by a failed read.
 *   the entry QP   a readback of the ordinary kind, and the stage's verdict rests on one field
 *                  of it: the firmware's default_qpn has to be the RX QP of stage 4.  The other
 *                  failure this stage exists to catch is "the entry QP points at itself", which
 *                  reaches RTS and reads back clean while steering every hashed frame into a
 *                  queue with no receive ring.
 */
static int cmdR4Ingress(void)
{
    uint64_t out[kR4IngressOutCount];
    uint32_t n = kR4IngressOutCount;
    Cx3R4IngressResult res;
    unsigned i;

    memset(out, 0, sizeof(out));
    memset(&res, 0, sizeof(res));

    printf("r4ingress: the MAC table and the RSS indirection QP (R4.5)\n");
    printf("  needs stage 4 in RTS (the hash target is that QP). The MAC comes from\n");
    printf("  QUERY_PORT's def_mac, never from a literal.\n");
    fflush(stdout);

    if (callStruct(kCx3SelR4Ingress, NULL, 0, out, &n, &res, sizeof(res)) != 0)
        return -1;

    if (res.abiVersion != CX3_R4INGRESS_ABI ||
        res.abiBytes != sizeof(Cx3R4IngressResult)) {
        printf("  ABI mismatch: version %u bytes %u, want %u/%zu -- reboot into a matching "
               "kext and tool\n", res.abiVersion, res.abiBytes, CX3_R4INGRESS_ABI,
               sizeof(Cx3R4IngressResult));
        return -1;
    }

    printR4Captures(res.modifyCapture, res.queryCapture, &res.finalQueryCapture);
    printf("  selector status : 0x%llx\n", (unsigned long long)out[kR4IngressOutStatus]);
    if (res.status != 0) {
        if (res.status == (uint32_t)kIOReturnNotReady)
            printf("  not ready: needs an initialised HCA, a CQ, and the RX QP of stage 4 to\n"
                   "             exist -- the entire verdict of this stage is that the hash\n"
                   "             target IS that queue\n");
        else if (res.status == (uint32_t)kIOReturnNoSpace)
            printf("  no room: the profile's QP count leaves no number above the RX QP's for\n"
                   "            the entry QP (a QP that steers into itself is not an option)\n");
        else if (res.status == (uint32_t)kIOReturnBadArgument)
            printf("  refused: the RX QPN reported by stage 4 is outside the range software\n"
                   "           may use -- see the kernel log\n");
        else
            printf("  failed: status 0x%x -- see the kernel log\n", res.status);
        return -1;
    }

    printf("\n  half one: the MAC table -- one SET_PORT carrying the WHOLE table\n");
    printf("    address from QUERY_PORT's def_mac : %02x:%02x:%02x:%02x:%02x:%02x\n",
           res.mac[2], res.mac[3], res.mac[4], res.mac[5], res.mac[6], res.mac[7]);
    printf("    the entry as sent (BE64)         : %02x %02x %02x %02x %02x %02x %02x %02x\n",
           res.mac[0], res.mac[1], res.mac[2], res.mac[3], res.mac[4], res.mac[5], res.mac[6],
           res.mac[7]);
    printf("      (byte 0's 0x80 is MLX4_MAC_VALID = 1ull << 63: the valid bit is the TOP bit\n"
           "       of the big-endian quantity, not a bit in a host integer)\n");
    printf("    table                            : %u bytes = %u entries of 8, all in the\n"
           "                                       command's mailbox (in_param = its DMA)\n",
           res.macTableBytes, res.macEntries);
    printf("    slot                             : %u (decided by the driver, then MEASURED out\n"
           "                                       of the table being sent: the first non-empty\n"
           "                                       entry of its %u -- so this number is the one\n"
           "                                       that goes on the wire)\n",
           res.macSlot, res.macEntries);
    printf("    write direction                  : fw status 0x%x %s\n", res.macFwStatus,
           res.macFwStatus == 0 ? "(accepted)" : "(REFUSED)");
    printf("    read direction (this port's probe, not the reference's practice):\n");
    if (out[kR4IngressOutMacReadbackOk] == 1) {
        printf("      answered                       : fw status 0x%x\n", res.macReadFwStatus);
        printf("      entry %u read back             : %02x %02x %02x %02x %02x %02x %02x %02x\n",
               res.macSlot,
               res.macReadRaw[0], res.macReadRaw[1], res.macReadRaw[2], res.macReadRaw[3],
               res.macReadRaw[4], res.macReadRaw[5], res.macReadRaw[6], res.macReadRaw[7]);
        printf("      verdict                        : %s\n",
               res.macReadbackMatches
                   ? "the firmware holds the table this driver sent"
                   : "MISMATCH -- the firmware answered with something else");
    } else if (res.macReadFwStatus == 0) {
        /*
         * Answered, and the outbox held nothing: the two cases behind macReadbackOk 0 are told
         * apart by the write's counterpart -- fw status 0 with nothing in the outbox, versus a
         * command that did not answer at all.
         */
        printf("      answered with NO CONTENT       : fw status 0x0, the outbox is all zeros\n");
        printf("      entry %u read back             : %02x %02x %02x %02x %02x %02x %02x %02x\n",
               res.macSlot,
               res.macReadRaw[0], res.macReadRaw[1], res.macReadRaw[2], res.macReadRaw[3],
               res.macReadRaw[4], res.macReadRaw[5], res.macReadRaw[6], res.macReadRaw[7]);
        printf("      verdict                        : the read-back is UNAVAILABLE. An empty\n"
               "        outbox is NOT 'the firmware holds a different table': what it stored and\n"
               "        what it never wrote are indistinguishable from here, and only the second\n"
               "        fits a command that reports success. The MAC evidence degrades to the\n"
               "        write's fw status above. (Measured on this card 2026-10-12 07:16.)\n");
    } else {
        printf("      did NOT answer                 : fw status 0x%x\n", res.macReadFwStatus);
        printf("      verdict                        : the read-back is UNAVAILABLE. That is\n"
               "        not 'the MAC was not registered': the reference never reads this table\n"
               "        back and has no get_port_mac_table, so this shape is this port's probe.\n"
               "        The MAC evidence degrades to the write's fw status above.\n");
    }

    printf("\n  half two: the RSS indirection QP\n");
    printf("    QPN                  : %u  (the RX QP's %u + 1, inside the range software may\n"
           "                                use: reserved %u, profile ceiling %u)\n",
           res.qpn, res.rxQpn, res.qpnRule, res.qpnProfileMax);
    printf("    the hash target      : default_qpn %u (the RX QP of stage 4), base_qpn 0x%08x\n",
           res.defaultQpnSent, res.baseQpnSent);
    printf("    rss_rings            : %u, so ilog2 = 0, so base_qpn's HIGH BYTE is 0 and\n"
           "                           base_qpn == default_qpn by construction. The high-byte\n"
           "                           ENCODING is therefore NOT exercised here -- that is the\n"
           "                           multi-ring milestone's job (plan decision 3).\n",
           res.rssRings);
    printf("    overlay at QPC+0x3c  : hash_fn %u (TOP), flags 0x%02x (IPV4|TCP_IPV4|IPV6|\n"
           "                           TCP_IPV6), rss_key %u bytes, base_qpn_udp %u (the\n"
           "                           profile does not ask for UDP RSS)\n",
           res.hashFnSent, res.rssFlagsSent, res.rssKeyBytes, res.baseQpnUdpSent);
    printf("    db_rec_addr          : record at 0x%llx, SENT as 0x%llx (%s)\n",
           (unsigned long long)res.dbRecordPhys, (unsigned long long)res.dbRecAddrSent,
           res.dbRecAddrShifted
               ? "the reference's `<< 2` form, mlx4_en_resources.c:78"
               : "the unshifted form");
    printf("                           Decision 2 in docs/R4-STAGE5-RESULT.md: the same struct\n"
           "                           member is written shifted for this QP and unshifted for\n"
           "                           the RX ring (mlx4_en_rx.c:947), the field's semantics are\n"
           "                           undocumented, and this QP produces no completions, so the\n"
           "                           value is expected not to be read. Copied, not derived.\n");
    printf("    rq_size_stride sent  : 0x%02x  (an indirection QP sends NONE: the reference's\n"
           "                           condition is `!is_tx && !rss`, mlx4_en_resources.c:52)\n",
           res.rqSizeStrideSent);

    printf("\n    layer 1: the three transitions, each followed by its own QUERY_QP\n");
    for (i = 0; i < 3; ++i) {
        const char *name = (i == 0) ? "RST2INIT " : (i == 1) ? "INIT2RTR " : "RTR2RTS ";
        printf("      %s(0x%02x): fw status 0x%x %s;  state after: ", name, res.stepOpcode[i],
               res.stepFwStatus[i], res.stepFwStatus[i] == 0 ? "ok " : "REJECTED");
        if (!res.stepQueryOk[i]) printf("? (QUERY_QP did not answer)\n");
        else printf("%u (wanted %u) %s\n", res.stepStateAfter[i], res.stepStateWanted[i],
                    res.stepStateAfter[i] == res.stepStateWanted[i] ? "ok" : "MISMATCH");
    }
    printf("      commands accepted %u/3, states confirmed by readback %u/3\n",
           res.stepsAccepted, res.statesConfirmed);

    printf("\n    layer 2: the last QUERY_QP against what was written\n");
    printf("      fw status 0x%x, flags 0x%08x -> state %u (RTS is 3), local_qpn %u\n",
           res.queryFwStatus, res.queryFlagsRaw, res.queryState, res.queryLocalQpn);
    printf("      the queue fields       : %s (mask 0x%02x)\n",
           res.queryAllFieldsMatch ? "MATCH" : "MISMATCH", res.queryFieldMask);
    printf("      sq_size_stride 0x%02x / 0x%02x, cqn_send %u / %u, cqn_recv %u / %u,\n"
           "      mtu_msgmax 0x%02x / 0x%02x\n",
           res.sqSizeStrideSent, res.querySqSizeStride, res.cqn, res.queryCqnSend, res.cqn,
           res.queryCqnRecv, res.mtuMsgmaxSent, res.queryMtuMsgmax);
    if (!res.queryAllFieldsMatch) {
        static const char *fnames[] = { "state", "local_qpn", "sq_size_stride", "cqn_send",
                                        "cqn_recv", "mtu_msgmax" };
        printf("        -> which one: ");
        for (i = 0; i < sizeof(fnames) / sizeof(fnames[0]); ++i)
            if (!(res.queryFieldMask & (1u << i))) printf("%s ", fnames[i]);
        printf("\n");
    }
    printf("      rq_size_stride 0x%02x reported, NOT judged: this driver sends none for an\n"
           "      indirection QP, so there is nothing to compare it against\n",
           res.queryRqSizeStride);
    printf("      the RSS overlay        : sent / read back\n");
    printf("        base_qpn             0x%08x / 0x%08x  (high byte %u)%s\n", res.baseQpnSent,
           res.queryBaseQpn, res.queryRssHighByte,
           res.queryBaseQpnMatches ? "" : "   <- DOES NOT MATCH");
    printf("        default_qpn          0x%08x / 0x%08x%s\n", res.defaultQpnSent,
           res.queryDefaultQpn, res.queryDefaultQpnMatches ? "" : "   <- DOES NOT MATCH");
    printf("        hash_fn              0x%02x / 0x%02x\n", res.hashFnSent, res.queryHashFn);
    printf("        flags                0x%02x / 0x%02x\n", res.rssFlagsSent, res.queryRssFlags);
    printf("        reserved             %u (0x%04x), base_qpn_udp 0x%08x / 0x%08x\n",
           res.queryReserved, res.queryReserved, res.baseQpnUdpSent, res.queryBaseQpnUdp);
    printf("        rss_key (40 bytes)   : %s\n",
           res.queryRssKeyMatches ? "came back byte for byte" : "DOES NOT match what was sent");
    printf("      the MTT fields         : %s\n",
           res.queryMttMatches ? "survived (log_page_size and mtt_base read back as sent)"
                               : "DID NOT survive -- the shared context was disturbed");
    printf("    param3 / sched_queue     : written 0x%08x / 0x%02x, read back 0x%08x / 0x%02x\n",
           res.param3Sent, res.schedQueueSent, res.queryParam3, res.querySchedQueue);
    printf("      (the same two fields stage 4 reports rather than judges: the reference never\n"
           "       reads them back, so there is no reference value to compare against)\n");

    printf("\n    the stage's own failure mode, as one number:\n");
    printf("      does the entry QP point at ITSELF?  default_qpn %u vs its own QPN %u : %s\n",
           res.queryDefaultQpn, res.qpn,
           res.pointsAtAnotherQp ? "no -- the hash target is a different QP"
                                 : "YES -- every hashed frame would be lost silently");
    printf("      the record's producer            : wrote %u, read back %u (%s)\n",
           res.producerValue, res.producerReadback,
           res.producerMatches ? "matches" : "MISMATCH");
    printf("      (nothing is expected to consume that record: this QP has no receive queue\n"
           "       and produces no completions, so it is a statement about the memory the\n"
           "       context names, not about traffic)\n");

    printf("\n  Stage 5 done: MAC %02x:%02x:%02x:%02x:%02x:%02x registered in slot %u and the\n"
           "  indirection QPN %u in RTS steering into the RX QP %u. Whether a frame actually\n"
           "  arrives is stages 6-10 -- this stage claims the steering target exists and is\n"
           "  the right queue, not traffic.\n",
           res.mac[2], res.mac[3], res.mac[4], res.mac[5], res.mac[6], res.mac[7],
           res.macSlot, res.qpn, res.rxQpn);
    return 0;
}

/*
 * r4tx -- R4 stage 6: the transmit side's RESOURCES.
 *
 * Read the last paragraph of this command's output before anything else it prints: this stage
 * builds a TX completion queue, a send QP in RTS and a send queue's memory, and posts NOTHING.
 * No WQE is written, no doorbell is rung, so "it worked" here means three objects exist and
 * their numbers are where the plan needs them -- not that a frame can go out.
 *
 * The numbers are the part of this stage with a constraint from outside itself, so the tool
 * prints the arithmetic rather than only its result: the QPN space, the two runtime inputs, the
 * line the top 128 QPNs start at, and where this stage's two numbers sit relative to it.  A
 * reader who wants to know "is there room for the MAC-derived range later" has the three
 * numbers in front of them (docs/M2B-NOTES.md sections 41.4-41.5).
 */
static int cmdR4Tx(uint32_t cqNent, uint32_t sqNent)
{
    uint64_t out[kR4TxOutCount];
    uint32_t n = kR4TxOutCount;
    uint64_t in[kR4TxInCount];
    Cx3R4TxResult res;
    unsigned i;

    memset(out, 0, sizeof(out));
    memset(in, 0, sizeof(in));
    memset(&res, 0, sizeof(res));
    in[kR4TxInCqNent] = cqNent;
    in[kR4TxInSqNent] = sqNent;

    printf("r4tx: the transmit side's resources (R4.6) -- a TX CQ, a send QP in RTS, and the\n"
           "      send queue's memory. NOTHING IS SENT: no WQE is posted and no doorbell is\n"
           "      rung. This says the resources exist, not that a frame can go out.\n");
    printf("  needs stage 4's RX QP and stage 5's indirection QP (the numbers are derived\n"
           "  above them) and stage 2's CQ (the UAR is taken from it).\n");
    fflush(stdout);

    if (callStruct(kCx3SelR4Tx, in, kR4TxInCount, out, &n, &res, sizeof(res)) != 0)
        return -1;

    printf("  selector status : 0x%llx\n", (unsigned long long)out[kR4TxOutStatus]);
    if (res.status != 0) {
        if (res.status == (uint32_t)kIOReturnNotReady)
            printf("  not ready: needs an initialised HCA, the MR, the RX CQ, the RX QP and\n"
                   "             the indirection QP -- the numbers are derived from the two QPs\n"
                   "             that exist, so a missing one would make 'the next number above\n"
                   "             them' land on it\n");
        else if (res.status == (uint32_t)kIOReturnNoSpace)
            printf("  no room: either a number would land at or above the top-reserved line, or\n"
                   "           a number collides with an existing object, or the MAC range alone\n"
                   "           fills the QPN space. These slots are kept empty on purpose; see\n"
                   "           the kernel log for the arithmetic\n");
        else if (res.status == (uint32_t)kIOReturnBadArgument)
            printf("  refused: a number is inside the firmware's reserved range or past the\n"
                   "           profile's configured ceiling -- see the kernel log\n");
        else
            printf("  failed: status 0x%x -- see the kernel log\n", res.status);
        return -1;
    }

    printf("\n  the number space, from two values read at runtime\n");
    printf("    QPN space            : %u  (the profile's log_num_qps)\n", res.numQps);
    printf("    log_max_macs         : %u  (QUERY_PORT outbox 0x0a, low nibble -> %u MAC-derived\n"
           "                                QPNs; A0 maps MAC index i to base_qpn + i)\n",
           res.logMaxMacs, res.topReservedStart ? (res.numQps - res.topReservedStart) : 0);
    printf("    top-reserved start   : %u  (num_qps - (1 << log_max_macs) = %u - %u)\n",
           res.topReservedStart, res.numQps,
           res.topReservedStart ? (res.numQps - res.topReservedStart) : 0);
    printf("                           the top %u slots (0x%x..0x%x) stay EMPTY: the plan's\n"
           "                           non-promiscuous route hands the firmware this base with\n"
           "                           qpn_calc, so the MAC-derived range never lands on a queue\n"
           "                           this driver owns (M2B-NOTES 41.4-41.5, option R)\n",
           res.topReservedStart ? (res.numQps - res.topReservedStart) : 0,
           res.topReservedStart, res.numQps - 1);
    printf("    TX QPN               : %u  (one above the RX QP %u and the indirection QP %u)\n",
           res.qpn, res.rxQpn, res.ingressQpn);
    printf("    TX CQN               : %u  (one above the RX CQN %u)\n", res.cqn, res.rxCqn);
    printf("    both below it        : %s  (QPN %s, CQN %s; collisions with the RX QPN %s, the\n"
           "                           indirection QPN %s, the RX CQN %s)\n",
           res.numberSafe ? "yes" : "NO", res.qpnBelowTop ? "yes" : "NO",
           res.cqnBelowTop ? "yes" : "NO", res.qpnCollidesRx ? "YES" : "no",
           res.qpnCollidesIngress ? "YES" : "no", res.cqnCollidesRx ? "YES" : "no");
    /*
     * Where these numbers live, said out loud, because the scalar block is the kernel's 16
     * wide and it is full: the CQN and the top-reserved line are NOT scalars -- they are
     * struct fields this command has just printed -- and the two layer-1 counts travel as one
     * scalar (`stepsAccepted | statesConfirmed << kR4TxOutStatesShift`).  A reader comparing
     * this output against `rawjson 36` should not have to work that out from the indices
     * (docs/DEFECTS.md F13; the reasoning is in src/cx3_abi.h beside the r4tx enum).
     */
    printf("    (the CQN and this line are read out of the structure, not out of the scalar\n"
           "     block: the kernel's scalar array is 16 wide and r4tx fills all 16 -- the two\n"
           "     layer-1 counts share one scalar. 'cx3ctl sizes' says 36 %u %u %zu r4tx)\n",
           kR4TxInCount, kR4TxOutCount, sizeof(res));

    printf("\n  the TX completion queue (the same cqCreate stage 2 used)\n");
    printf("    CQN / entries        : %u / %u, %u pages, MTT %u, record 0x%llx\n", res.cqn,
           res.cqNent, res.cqPages, res.cqMttOffset, (unsigned long long)res.cqDbRecordPhys);
    printf("    SW2HW_CQ             : fw status 0x%x %s\n", res.cqFwStatus,
           res.cqFwStatus == 0 ? "(accepted)" : "(REFUSED)");
    printf("    empty-ring check     : %u readable slot(s) at cons_index 0 -- %s\n",
           res.cqReadyAtStart,
           res.cqReadyAtStart == 0
               ? "correct: a queue with no completions reads as empty"
               : "EXPECTED 0: this queue would report a completion out of unwritten memory");
    printf("    comp_eqn / UAR       : the same EQ and the same runtime UAR index the RX CQ uses\n"
           "                           (%u): the reference gives a netdev one priv_uar for all\n"
           "                           of its queues (mlx4_en_resources.c:58)\n",
           res.uarIndex);

    printf("\n  the send queue's memory (built, not handed over until RST->INIT names it)\n");
    printf("    WQE array            : %u TXBBs of %u bytes = %u bytes, %u pages; WQE 0 at\n"
           "                           offset %u of the allocation\n",
           res.sqNent, res.sqStride, res.sqBytes, res.sqPages, res.sqHeaderBytes);
    printf("                           NO header in front of the WQEs: a send queue's WQE i is\n"
           "                           at the MTT base + i*%u (mlx4_en_tx.c:139/:686), while\n"
           "                           the receive ring keeps a 64-byte unit in front of its own\n"
           "                           (mlx4_en_rx.c:406/:482-489).  0.43.7 gave this queue the\n"
           "                           receive ring's shape, the hardware read the base and found\n"
           "                           no descriptor there, and nothing was ever executed --\n"
           "                           no completion, no frame, no error (docs/DEFECTS.md E13)\n",
           res.sqStride);
    printf("    initial ownership    : every 64-byte unit's first dword = 0xffffffff\n"
           "                           (STAMP_VAL | owner 1 << 31 = 'software owns this\n"
           "                           slot', mlx4_en_netdev.c:1373-1375). Zero there would say\n"
           "                           bit 31 = 0 = 'hardware owns', i.e. every slot in flight\n");
    printf("    WQE 0 (offset %u)     : first dword reads back 0x%08x (%s; 0xffffffff wanted)\n",
           res.sqHeaderBytes,
           res.sqStampValue,
           res.sqStampMatches ? "as written" : "NOT what was written");
    printf("    the WQE TXBBs        : %u of %u stamped%s\n", res.sqTxbbsStamped, res.sqNent,
           res.sqTxbbsStamped == res.sqNent ? " -- every one of them"
                                            : " -- NOT all of them (every one is wanted)");
    printf("    the WQE array        : %u non-zero byte(s) outside the ownership stamps -- this\n"
           "                           stage posts nothing, so 0 is the expected answer and the\n"
           "                           number is how 'nothing was posted' is measured rather than\n"
           "                           assumed (the stamps are 4 bytes of every 64 and are not\n"
           "                           counted)\n",
           res.sqWqeNonZeroBytes);
    printf("    MTT                  : entry %u -> 0x%llx (index x 8), first page at 0x%llx\n",
           res.sqMttOffset, (unsigned long long)res.sqMttAddr,
           (unsigned long long)res.sqPhysFirst);

    printf("\n  the send QP\n");
    printf("    QPN                  : %u  (rule: the firmware's reserved share %u, profile\n"
           "                                ceiling %u)\n", res.qpn, res.qpnRule,
           res.qpnProfileMax);
    printf("    context              : pd %u, usr_page %u, mtu_msgmax 0x%02x, counter_index\n"
           "                           0x%02x, sched_queue 0x%02x, db_rec_addr 0x%llx\n",
           res.pdn, res.uarIndex, res.mtuMsgmaxSent, res.counterIndexSent, res.schedQueueSent,
           (unsigned long long)res.dbRecAddrSent);
    printf("    the two size bytes   : sq_size_stride 0x%02x (the SEND ring: %u TXBBs of %u\n"
           "                           bytes), rq_size_stride 0x%02x (a send QP sends NONE:\n"
           "                           the reference's `!is_tx && !rss`, mlx4_en_resources.c:52)\n",
           res.sqSizeStrideSent, res.sqNent, res.sqStride, res.rqSizeStrideSent);
    printf("    db_rec_addr          : record at 0x%llx, SENT UNSHIFTED as 0x%llx (%s)\n",
           (unsigned long long)res.dbRecordPhys, (unsigned long long)res.dbRecAddrSent,
           res.dbRecAddrShifted ? "the reference's `<< 2` form"
                                : "the unshifted form, as stage 4 sends for the RX QP");
    printf("                           Deliberate, and the one place this stage does not copy the\n"
           "                           reference: mlx4_en_resources.c:78's `<< 2` IS what the\n"
           "                           shipping TX ring sends (nothing overwrites it there the way\n"
           "                           mlx4_en_rx.c:947 overwrites it for the RX ring), and this\n"
           "                           port sends the unshifted address of a record of its own.\n"
           "                           A send queue's producer index goes to the send doorbell in\n"
           "                           MMIO, not into this record, so the field is expected not to\n"
           "                           be read -- see docs/R4-STAGE6-RESULT.md for the condition\n"
           "                           under which this is the thing to change.\n");
    printf("    param3               : 0x%08x  (FCS_KEEP NOT set: the reference sets that bit on\n"
           "                           the receive side only, mlx4_en_rx.c:950-951)\n",
           res.param3Sent);
    printf("    pri_path.feup        : 0x%02x  (%s -- the one context field the reference's shared\n"
           "                           fill gives a SEND queue and not a receive one:\n"
           "                           `if (user_prio >= 0) feup = 1 << 6`)\n",
           res.feupSent,
           (res.feupSent == 0x40u) ? "MLX4_FEUP_FORCE_ETH_UP, written"
                                   : "NOT 0x40: the fill's userPrio branch did not run");

    printf("\n    layer 1: the three transitions, each followed by its own QUERY_QP\n");
    for (i = 0; i < 3; ++i) {
        const char *name = (i == 0) ? "RST2INIT " : (i == 1) ? "INIT2RTR " : "RTR2RTS ";
        printf("      %s(0x%02x): fw status 0x%x %s;  state after: ", name, res.stepOpcode[i],
               res.stepFwStatus[i], res.stepFwStatus[i] == 0 ? "ok " : "REJECTED");
        if (!res.stepQueryOk[i]) printf("? (QUERY_QP did not answer)\n");
        else printf("%u (wanted %u) %s\n", res.stepStateAfter[i], res.stepStateWanted[i],
                    res.stepStateAfter[i] == res.stepStateWanted[i] ? "ok" : "MISMATCH");
    }
    printf("      commands accepted %u/3, states confirmed by readback %u/3\n",
           res.stepsAccepted, res.statesConfirmed);

    printf("\n    layer 2: the last QUERY_QP against what was written\n");
    printf("      fw status 0x%x, flags 0x%08x -> state %u (RTS is 3), local_qpn %u %s\n",
           res.queryFwStatus, res.queryFlagsRaw, res.queryState, res.queryLocalQpn,
           res.qpnConfirmed ? "(confirmed)" : "(NOT the number that was written)");
    printf("      the eight fields       : %s (mask 0x%02x)\n",
           res.queryAllFieldsMatch ? "MATCH" : "MISMATCH", res.queryFieldMask);
    printf("        sq_size_stride       0x%02x / 0x%02x   <- 0x42 is this stage's signature\n"
           "        rq_size_stride       0x%02x / 0x%02x   <- 0 for a send QP, not the RX 0x40\n"
           "        cqn_send             0x%x / 0x%x\n"
           "        cqn_recv             0x%x / 0x%x\n"
           "        mtu_msgmax           0x%02x / 0x%02x\n"
           "        db_rec_addr          0x%llx / 0x%llx\n"
           "        local_qpn            %u / %u\n"
           "        state                %u / %u\n",
           res.sqSizeStrideSent, res.querySqSizeStride, res.rqSizeStrideSent,
           res.queryRqSizeStride, res.cqn, res.queryCqnSend, res.cqn, res.queryCqnRecv,
           res.mtuMsgmaxSent, res.queryMtuMsgmax, (unsigned long long)res.dbRecAddrSent,
           (unsigned long long)res.queryDbRecAddr, res.qpn, res.queryLocalQpn,
           (unsigned)3, res.queryState);
    if (!res.queryAllFieldsMatch) {
        static const char *fnames[] = { "state", "local_qpn", "rq_size_stride",
                                        "sq_size_stride", "mtu_msgmax", "cqn_send",
                                        "cqn_recv", "db_rec_addr" };
        printf("        -> which one: ");
        for (i = 0; i < sizeof(fnames) / sizeof(fnames[0]); ++i)
            if (!(res.queryFieldMask & (1u << i))) printf("%s ", fnames[i]);
        printf("\n");
    }
    printf("      the MTT fields         : %s (mtt_base h 0x%02x / l 0x%08x, log_page_size %u)\n",
           res.queryMttMatches ? "survived (written for RST->INIT and copied through the other "
                                 "two)"
                               : "DID NOT survive -- the shared context was disturbed",
           res.queryMttAddrH, res.queryMttAddrL, res.queryLogPageSize);
    printf("      param3 / sched_queue   : written 0x%08x / 0x%02x, read back 0x%08x / 0x%02x\n",
           res.param3Sent, res.schedQueueSent, res.queryParam3, res.querySchedQueue);
    printf("      pri_path.feup          : written 0x%02x, read back 0x%02x  %s\n",
           res.feupSent, res.queryFeup,
           res.feupSurvived ? "(the firmware KEPT the byte)"
                            : "(THE FIRMWARE'S COPY DIFFERS FROM WHAT WAS WRITTEN)");
    printf("      (the three fields stage 4's rule says to report rather than judge: the\n"
           "       reference never reads them back, so there is no reference value to compare\n"
           "       against -- and this build is the first that writes pri_path.feup at all,\n"
           "       which the reference sets to MLX4_FEUP_FORCE_ETH_UP for a SEND queue only:\n"
           "       mlx4_en_resources.c:62-65, its transmit caller passing a queue index >= 0\n"
           "       and both receive-side callers passing -1. 0x00 read back after writing 0x40\n"
           "       means the firmware dropped it, exactly as it drops param3, and then this\n"
           "       field is not the gate -- see src/mlx4_regs.h for what to look at instead)\n");

    printf("\n  Stage 6 done: CQN %u accepted and empty, QPN %u in RTS with sq_size_stride\n"
           "  0x%02x, and the send queue's %u TXBBs in place with their stamp. NOTHING HAS BEEN\n"
           "  SENT -- no WQE is posted and no doorbell has been rung, so no frame has left the\n"
           "  card and none is claimed to. Posting to this queue is stage 9.\n",
           res.cqn, res.qpn, res.sqSizeStrideSent, res.sqNent);
    return 0;
}

/*
 * r4raw -- R4 stage 9: post WQEs, ring the send doorbell, read completions.
 *
 * THIS CALL PUTS FRAMES ON THE WIRE.  It is the first selector in the project whose effect
 * leaves the machine, which is why the tool says so before it runs rather than after: the
 * frame is a 60-byte broadcast with ethertype 0x88B5 (IEEE Local Experimental 1), nothing on
 * a LAN interprets it, and the payload is this driver's own pattern.
 *
 * The tool prints the kernel's numbers and does not judge them -- the harness decides PASS or
 * FAIL from the scalars, and the reasons are in the kernel log.  What is printed here is
 * everything a reader needs to see WHY a judgement came out the way it did: the WQE's 32 bytes
 * (where a wrong opcode position is unmistakable), the send doorbell's value and offset, the
 * first TX completion's 32 bytes, the first receive completion's 32 bytes, the first frame's
 * bytes, and the two counters the refill is driven from.
 */
static int cmdR4Raw(unsigned posts, unsigned rxBudgetMs)
{
    uint64_t in[kR4RawInCount];
    uint64_t out[kR4RawOutCount];
    uint32_t n = kR4RawOutCount;
    Cx3R4RawResult res;
    unsigned i;

    memset(in, 0, sizeof(in));
    memset(out, 0, sizeof(out));
    memset(&res, 0, sizeof(res));
    in[kR4RawInTxPosts]    = posts;
    in[kR4RawInRxBudgetMs] = rxBudgetMs;

    printf("r4raw: the first stage where a byte actually moves (R4.9).\n");
    printf("  IT SENDS FRAMES: %u post(s) -- one at a time, each awaited -- of a %u-byte\n"
           "  broadcast frame, ethertype 0x%04x (IEEE Local Experimental 1), payload this\n"
           "  driver's own pattern. ARP is stage 10; nothing here queries the network.\n",
           posts ? posts : 0, (unsigned)CX3_R4RAW_FRAME_BYTES, (unsigned)CX3_R4RAW_ETHERTYPE);
    printf("  receive budget %u ms (0 = the driver's default, %u)\n",
           rxBudgetMs, (unsigned)CX3_R4RAW_DEFAULT_RX_BUDGET_MS);
    printf("  Put ONE post first (r4raw 1): that is the cautious first run, and the wrap\n"
           "  phase only starts after the first frame's completion has come back.\n");
    fflush(stdout);

    if (callStruct(kCx3SelR4Raw, in, kR4RawInCount, out, &n, &res, sizeof(res)) != 0)
        return -1;

    printf("\n  selector status : 0x%llx\n", (unsigned long long)out[kR4RawOutStatus]);
    if (res.status != 0) {
        if (res.status == (uint32_t)kIOReturnNotReady)
            printf("  not ready: the RECEIVE side is not built (needs the RX CQ of stage 2,\n"
                   "             the ring of stage 3 and the QP of stage 4) -- or the producer\n"
                   "             record was never published. See the kernel log; the chain runs\n"
                   "             those before this.\n");
        else
            printf("  failed: status 0x%x -- see the kernel log\n", res.status);
        return -1;
    }

    printf("\n  the memory region the WQEs' lkey belongs to, as QUERY_MPT reports it\n");
    printf("    MPT %u              : QUERY_MPT %s\n", res.mrMptIndex,
           res.mrQueryOk ? "answered" : "did not answer (start/length unknown)");
    if (res.mrQueryOk) {
        printf("    start / length      : 0x%llx / 0x%llx\n",
               (unsigned long long)res.mrStart, (unsigned long long)res.mrLength);
        printf("    the frame buffer    : %s that region\n",
               res.frameInRegion ? "INSIDE" : "OUTSIDE -- a descriptor outside the region is a\n"
                                              "                          completion in error, if\n"
                                              "                          the device checks it");
        printf("                          The reference's own region covers everything (iova 0,\n"
               "                          size ~0, mlx4_en_main.c:213); this driver's was\n"
               "                          registered with whatever r4mr was given, and 'r4mr 0 0'\n"
               "                          registers nothing. That is the first thing to change if\n"
               "                          a completion comes back in error.\n");
    }

    printf("\n  the transmit half\n");
    if (res.txRefused != CX3_R4RAW_TX_RAN) {
        printf("    REFUSED             : %s\n",
               res.txRefused == CX3_R4RAW_TX_NO_STAGE6
                   ? "the send side is not built (run r4tx first)"
                   : (res.txRefused == CX3_R4RAW_TX_NO_MAC
                          ? "no MAC address is registered (stage 5): the frame would have to "
                            "invent a source"
                          : "the frame buffer could not be allocated"));
        printf("    (the receive half ran anyway -- the two halves fail for different reasons)\n");
    } else {
        printf("    QPN / UAR / SQ      : %u / index %u / %u TXBBs, WQE 0 at SQ+%u (no header:\n"
               "                          the doorbell's TXBB index and the byte offset are the\n"
               "                          same number when this is 0)\n",
               res.txQpn, res.txUarIndex, res.txSqNent, res.txSqHeaderBytes);
        printf("    the first WQE       : TXBB %u, owner bit %u (wanted %u: `prod & size`, so 0\n"
               "                          on the first lap), owner_opcode 0x%08x\n",
               res.txWqeIndex, res.txWqeOwnerBit, res.txWqeOwnerWanted, res.txWqeDword);
        printf("                          opcode 0x%02x in the LOW byte (a shift by 24 would put\n"
               "                          MLX4_OPCODE_NOP here and transmit nothing)\n",
               (unsigned)(res.txWqeDword & 0xffu));
        printf("    the WQE's 32 bytes  : ");
        for (i = 0; i < sizeof(res.txWqeImage); ++i)
            printf("%02x%s", res.txWqeImage[i], (i + 1 == sizeof(res.txWqeImage)) ? "\n" : " ");
        printf("    bytes past the WQE  : %u  (inside its 64-byte TXBB; 0 wanted)\n",
               res.txWqeNonZeroBytes);
        printf("    the frame           : %u bytes at 0x%llx (bus address the WQE carries),\n"
               "                          ethertype 0x%04x, payload %u bytes\n",
               res.txFrameBytes, (unsigned long long)res.txFramePhys,
               res.txFrameEthertype, res.txFramePayloadBytes);
        printf("    the send doorbell   : value 0x%08x (BE32(QPN << 8)) at BAR2+0x%llx\n",
               res.txDoorbellValue, (unsigned long long)res.txDoorbellOffset);
        printf("                          the CQ's doorbell is 0x%x, 0xC away -- a different\n"
               "                          register\n", (unsigned)MLX4_CQ_DOORBELL);
        printf("    posts / completions : %u / %u (%u in error, %u timed out); producer now %u\n",
               res.txPosts, res.txCompletions, res.txErrors, res.txTimeouts, res.txProdAfter);
        printf("    laps                : %u covered, %u whose owner bit read back as the lap\n"
               "                          phase said; owner bits seen 0x%x (3 = both)\n",
               res.txLapsCovered, res.txOwnerLapsOk, res.txOwnerBitsSeen);
        printf("    the WQE readback    : %s\n",
               res.txWqeOk == 1 ? "matched on every post"
                                : (res.txWqeOk == 0 ? "MISMATCHED (see the kernel log)"
                                                    : "not reached"));
        printf("    the wrap (> 2 laps) : %s\n",
               res.txWrapOk == 1 ? "PROVED" : (res.txWrapOk == 0 ? "NOT proved"
                                                                 : "not attempted"));
        if (res.txCompletions) {
            printf("    the first TX CQE    : read at consumer index %u\n",
                   res.txCqeRawConsIndex);
            printf("                          32 bytes: ");
            for (i = 0; i < sizeof(res.txCqeRaw); ++i)
                printf("%02x%s", res.txCqeRaw[i],
                       (i + 1 == sizeof(res.txCqeRaw)) ? "\n" : " ");
            printf("                          qpn %u, opcode 0x%02x%s, is_send %u, wqe_index %u,\n"
                   "                          byte_cnt %u, status 0x%04x, checksum 0x%04x\n",
                   res.txCqeQpn, res.txCqeOpcode,
                   res.txCqeOpcode == (uint32_t)MLX4_CQE_OPCODE_ERROR ? " (ERROR)" : "",
                   res.txCqeIsSend, res.txCqeWqeIndex, res.txCqeByteCnt, res.txCqeStatus,
                   res.txCqeChecksum);
            printf("                          byte_cnt/status/checksum are RECORDED, not judged:\n"
                   "                          the reference's transmit poll reads the opcode and\n"
                   "                          wqe_index only (mlx4_en_tx.c:381-395)\n");
        }
    }

    printf("\n  the receive half\n");
    printf("    RX QPN / ring       : %u / %u WQEs of %u bytes\n", res.rxQpn, res.rxNent,
           res.rxStride);
    printf("    frames received     : %u (%u named our QPN; verdict %s)\n", res.rxFrames,
           res.rxQpnMatches,
           res.rxQpnOk == 1 ? "all of them" : (res.rxQpnOk == 0 ? "MISMATCH"
                                                               : "not measured"));
    printf("    error completions   : %u\n", res.rxErrorCqes);
    printf("    smallest byte_cnt   : %s\n",
           res.rxMinByteCnt == (uint32_t)CX3_R4RAW_NOT_MEASURED
               ? "not measured (no non-error completion)"
               : "(printed below with the first frame)");
    printf("    content             : %s\n",
           res.rxContentOk ? "a frame of at least the minimum size was read out of its buffer"
                           : "no frame of that size yet");
    if (res.rxFrames) {
        printf("    the first RX CQE    : read at consumer index %u\n", res.rxFirstRawConsIndex);
        printf("                          32 bytes: ");
        for (i = 0; i < sizeof(res.rxFirstRaw); ++i)
            printf("%02x%s", res.rxFirstRaw[i],
                   (i + 1 == sizeof(res.rxFirstRaw)) ? "\n" : " ");
        printf("                          qpn %u (ours is %u), opcode 0x%02x%s, is_send %u,\n"
               "                          byte_cnt %u, wqe_index %u, the ring position says %u %s\n",
               res.rxFirstQpn, res.rxQpn, res.rxFirstOpcode,
               res.rxFirstOpcode == (uint32_t)MLX4_CQE_OPCODE_ERROR ? " (ERROR)" : "",
               res.rxFirstIsSend, res.rxFirstByteCnt, res.rxFirstWqeIndex,
               res.rxFirstExpectedSlot, res.rxFirstSlotMatches ? "(they agree)" : "(THEY DISAGREE)");
        printf("                          status 0x%04x, checksum 0x%04x, badfcs byte 0x%02x --\n"
               "                          recorded, not judged (no offload in this stage)\n",
               res.rxFirstStatus, res.rxFirstChecksum, res.rxFirstBadfcsEnc);
    }
    if (res.rxFirstBytesLen) {
        printf("    the first frame     : %u bytes read out of its buffer: ",
               res.rxFirstBytesLen);
        for (i = 0; i < res.rxFirstBytesLen && i < CX3_R4RAW_RX_KEEP_BYTES; ++i)
            printf("%02x%s", res.rxFirstBytes[i],
                   (i + 1 == res.rxFirstBytesLen) ? "\n" : " ");
    }
    printf("    our own frame back  : %s\n",
           res.rxOurs ? "YES -- a received frame carried this driver's pattern (a loopback on "
                        "the segment)"
                      : "no (expected on a switched segment: a broadcast does not return to the "
                        "port it left)");
    printf("    refills             : %u this call, %u whole laps of the ring, %u slot(s)\n"
           "                          refilled at least twice; verdict %s\n",
           res.rxRecycles, res.rxLaps, res.rxSlotsRecycledTwice,
           res.rxRecycleOk == 1 ? "every refill correct"
                                : (res.rxRecycleOk == 0
                                       ? "A VIOLATION was seen (see the kernel log)"
                                       : "not measured (nothing was refilled)"));
    printf("    producer / consumer : %u -> %u, CQ consumer %u, %u slot(s) in flight at exit\n",
           res.rxProducerBefore, res.rxProducerAfter, res.rxCqConsAfter, res.rxInFlightAtExit);
    if (res.rxFrames == 0)
        printf("\n    NOT ONE FRAME ARRIVED. The first thing to suspect is stage 7's\n"
               "    mcast/base_qpn: under A0 the FIRMWARE decides where broadcast goes and the\n"
               "    `mcast` field of SET_PORT qpn_calc names the QP (docs/EXTERNAL-REFS.md 13).\n"
               "    This is NOT 'stage 8's filter is not configured' -- no filter command is\n"
               "    involved in A0. Then the producer publish (stage 4) and the link (m3portup).\n");
    return 0;
}


/*
 * r4rq -- R4 stage 3: build the receive ring, its buffers and their WQEs.
 *
 * Memory only: nothing is handed to the hardware here, because the ring and its buffers
 * become reachable only through a QP and no QP exists yet.  That separation is the point
 * of the stage -- if a frame never arrives in stage 4, this lets "the descriptors are
 * wrong" be told apart from "the hardware did not take the queue".
 *
 * Three fields decide whether a frame can land at all, and all three are silent when
 * wrong, so all three are printed: the WQE's lkey must be the TRANSFORMED key (not the raw
 * MPT index), byte_count is the buffer size rather than a frame length, and every buffer
 * must sit inside one page so it has a single bus address.
 */
static int cmdR4Rq(unsigned nent, unsigned bufBytes)
{
    uint64_t in[kR4RqInCount];
    uint64_t out[kR4RqOutCount];
    uint32_t n = kR4RqOutCount;
    Cx3R4RqResult res;

    in[kR4RqInNent]     = nent;
    in[kR4RqInBufBytes] = bufBytes;
    memset(out, 0, sizeof(out));
    memset(&res, 0, sizeof(res));

    printf("r4rq: build the receive ring -- %u WQEs, %u-byte buffers\n",
           nent ? nent : CX3_R4RQ_DEFAULT_NENT,
           bufBytes ? bufBytes : CX3_R4RQ_DEFAULT_BUF);
    printf("  memory only: the WQEs become reachable through a QP, and stage 4 creates "
           "that.\n");
    fflush(stdout);

    if (callStruct(kCx3SelR4Rq, in, kR4RqInCount, out, &n, &res, sizeof(res)) != 0)
        return -1;

    printf("  selector status  : 0x%llx\n", (unsigned long long)out[kR4RqOutStatus]);
    if (res.status != 0) {
        if (res.status == (uint32_t)kIOReturnNotReady)
            printf("  not ready: needs an initialised HCA and an enabled MR (stages 0 "
                   "and 1)\n");
        else
            printf("  failed: status 0x%x -- see the kernel log\n", res.status);
        return -1;
    }

    printf("  ring             : %u WQEs of %u bytes at ring+%u, %u pages (%u bytes)\n",
           res.nent, res.wqeSize, res.ringHeaderBytes, res.ringPages, res.ringBytes);
    printf("                     the %u-byte header in front of WQE 0 is the dummy SQ "
           "section\n", res.ringHeaderBytes);
    printf("  MTT              : offset %u -> 0x%x for the ring's %u pages\n",
           res.mttOffset, res.mttAddr, res.ringPages);
    printf("  lkey in every WQE: 0x%08x  <- the TRANSFORMED key\n", res.lkey);
    printf("                     (the raw MPT index is %u; putting THAT in a WQE would be "
           "a\n                     descriptor the hardware cannot resolve, silently)\n",
           res.lkeyRawIndex);
    printf("  byte_count       : %u  (= %u-byte buffer minus the %u-byte IP align; this "
           "is\n                     the space the hardware may fill, NOT a frame length)\n",
           res.byteCountSent, res.bufBytes, res.ipAlign);
    printf("  buffers          : 0x%llx .. 0x%llx (%u buffers of %u bytes)\n",
           (unsigned long long)res.bufPhysFirst, (unsigned long long)res.bufPhysLast,
           res.nent, res.bufBytes);
    /*
     * A 64-bit-aware sanity check on the addresses, and it is here because the tool once
     * reported "matches" for a WQE whose address had been truncated: the expectation was
     * computed from the same 32-bit value as the actual, so both were wrong identically and
     * the comparison could not see it.  A comparison cannot catch a truncation that both
     * sides share -- so this checks the ADDRESS ITSELF against the only thing known
     * independently: that a buffer of this size fits in a page and that the addresses are
     * not below 4 GiB, which this card's are not.
     */
    if (res.bufPhysFirst < 0x100000000ull)
        printf("    WARNING: the first buffer address is below 4 GiB. This card's DMA\n"
               "             addresses have never been that low; suspect truncation.\n");
    if (res.wqe0Addr < 0x100000000ull || res.wqeLastAddr < 0x100000000ull)
        printf("    WARNING: a WQE holds an address below 4 GiB -- the descriptor may\n"
               "             have been truncated to 32 bits.\n");
    if ((res.wqe0Addr & (res.bufBytes - 1)) != 0)
        printf("    WARNING: WQE 0\'s address is not %u-byte aligned.\n", res.bufBytes);

    printf("\n  the descriptors as they read back out of the ring:\n");
    printf("    WQE 0    : byte_count %u, lkey 0x%08x, addr 0x%llx  %s\n",
           res.wqe0ByteCount, res.wqe0Lkey, (unsigned long long)res.wqe0Addr,
           res.wqe0Matches ? "matches" : "MISMATCH");
    printf("    WQE last : byte_count %u, lkey 0x%08x, addr 0x%llx  %s\n",
           res.wqeLastByteCount, res.wqeLastLkey, (unsigned long long)res.wqeLastAddr,
           res.wqeLastMatches ? "matches" : "MISMATCH");
    if (!res.wqe0Matches || !res.wqeLastMatches)
        printf("    -> a mismatch here is the difference between 'a frame arrives' and\n"
               "       'nothing ever arrives'. Read the kernel log.\n");

    printf("\n  Stage 3 done: %u receive WQEs and their buffers exist and are described.\n"
           "  Next is stage 4, the RX QP: RST2INIT -> INIT2RTR -> RTR2RTS, and then the\n"
           "  producer index that tells the hardware how many WQEs there are.\n", res.nent);
    return 0;
}


/*
 * r4cq -- R4 stage 2: create the RX completion queue.
 *
 * Two things in the CQ context are the ones that produce "completions never arrive"
 * rather than an error, and both are printed so they can be checked by eye: the UAR
 * index inside logsize_usrpage (the first 128 UARs belong to the EQ doorbells, so a CQ
 * placed there would sit on the doorbell that already works) and db_rec_addr being the
 * DMA byte address of the record rather than an offset.
 *
 * The third is not in the context at all: the owner byte of every slot has to be preset
 * to 0x80 by the driver.  Nothing does it for a CQ, and a slot left at zero reads as
 * ready under the phase rule -- a false completion out of memory nobody wrote.  The
 * count below is the check, because a preset loop that ran is not the same claim as a
 * ring whose every slot actually holds the bit.
 */
static int cmdR4Cq(unsigned nent, unsigned cqn)
{
    uint64_t in[kR4CqInCount];
    uint64_t out[kR4CqOutCount];
    uint32_t n = kR4CqOutCount;
    Cx3R4CqResult res;
    unsigned i;

    in[kR4CqInNent] = nent;
    in[kR4CqInCqn]  = cqn;
    memset(out, 0, sizeof(out));
    memset(&res, 0, sizeof(res));

    printf("r4cq: create the RX completion queue (CQN %u, %u entries)\n",
           cqn ? cqn : CX3_R4CQ_DEFAULT_CQN, nent ? nent : CX3_R4CQ_DEFAULT_NENT);
    printf("  64-byte CQEs, so each slot is 64 bytes and the real CQE is its second "
           "half;\n  the owner byte is at slot + 63 and the driver has to preset it\n");
    fflush(stdout);

    if (callStruct(kCx3SelR4Cq, in, kR4CqInCount, out, &n, &res, sizeof(res)) != 0)
        return -1;

    printf("  selector status : 0x%llx\n", (unsigned long long)out[kR4CqOutStatus]);
    if (res.status != 0) {
        if (res.status == (uint32_t)kIOReturnNotReady)
            printf("  not ready: needs an initialised HCA and a live EQ (m2dbringup + "
                   "m3equp)\n");
        else if (res.status == (uint32_t)kIOReturnBadArgument)
            printf("  refused: the CQN is outside this profile's CQ range -- see the "
                   "kernel log\n");
        else
            printf("  failed: status 0x%x -- see the kernel log\n", res.status);
        return -1;
    }

    printf("  CQN              : %u   (%u entries, %u pages)\n", res.cqn, res.nent,
           res.pages);
    printf("  MTT              : offset %u -> 0x%x (index x 8)\n", res.mttOffset,
           res.mttAddr);
    printf("  SW2HW_CQ         : fw status 0x%x %s\n", res.fwStatus,
           res.fwStatus == 0 ? "ok" : "-- rejected");
    printf("  comp_eqn         : %u   (an EQ NUMBER -- the EQ that already works, not a "
           "vector)\n", res.eqnSent);
    printf("  UAR index        : %u -> hw_uar_index %u  (the first 128 UARs are the EQ "
           "doorbells')\n", res.uarIndex, res.hwUarIndex);
    printf("  logsize_usrpage  : 0x%08x\n", res.logsizeUsrpageSent);
    printf("  UAR page         : BAR2 + 0x%llx\n", (unsigned long long)res.uarPageOffset);
    printf("  CQ doorbell      : BAR2 + 0x%llx\n", (unsigned long long)res.doorbellOffset);
    printf("  db_rec_addr sent : 0x%llx  (the record's DMA BYTE address, not an offset)\n",
           (unsigned long long)res.dbRecordPhys);

    printf("\n  owner byte at slot + %u, by exact value:\n", res.ownerByteOffsetInSlot);
    printf("    BEFORE the driver's preset:  0x80 x%u  0xcc x%u  0xee x%u  0x00 x%u  "
           "other x%u\n", res.ownerBefore[0], res.ownerBefore[1], res.ownerBefore[2],
           res.ownerBefore[3], res.ownerBefore[4]);
    printf("    AFTER  the driver's preset:  0x80 x%u  other x%u\n", res.ownerAfter[0],
           res.ownerAfter[1]);
    if (res.ownerBefore[1] || res.ownerBefore[2])
        printf("      -> the ring already held 0xcc/0xee before the preset: the FIRMWARE\n"
               "         wrote it at SW2HW_CQ, exactly as it does for an EQ ring.  Those\n"
               "         bytes have bit 7 set, so a plain 'bit 7 set' count would have\n"
               "         looked identical whether or not the preset ran.\n");
    if (res.ownerAfter[1])
        printf("      -> the preset did NOT cover the ring: %u slot(s) are not 0x80\n",
               res.ownerAfter[1]);
    else
        printf("      -> after the preset every slot holds 0x80, so nothing in the ring\n"
               "         can be mistaken for a completion.\n");
    printf("    bit-7 count after preset    : %u of %u  (this alone proves nothing -- "
           "see above)\n", res.ownerPresetCount, res.nent);
    printf("\n  the consumer's own readiness rule, run over the new queue without "
           "consuming:\n");
    printf("    slots it finds readable at cons_index 0 : %u\n", res.readyAtStart);
    if (res.readyAtStart)
        printf("      -> EXPECTED 0.  A queue with no completions must read as empty; %u\n"
               "         means the rule or the initial owner state is wrong, and a "
               "consumer\n         would report zeroed memory as a completion.\n",
               res.readyAtStart);
    else
        printf("      -> correct: an empty queue reads as empty, so stage 3 can build "
               "on this.\n");

    printf("\n  the first two 32-byte halves of slot 0, so the layout is visible:\n");
    for (i = 0; i < 2; ++i) {
        unsigned k;
        const uint8_t *p = i ? res.slot0Second : res.slot0First;
        printf("    %s:", i ? "second half (the real CQE)" : "first half              ");
        for (k = 0; k < 16; ++k) printf(" %02x", p[k]);
        printf("\n");
    }

    printf("\n  Stage 2 done: CQN %u is real and the hardware owns it. Next is stage 3,\n"
           "  the receive buffers and their WQEs.\n", res.cqn);
    return 0;
}


/*
 * r4mr -- R4 stage 1: a protection domain and the physical memory region.
 *
 * This is the first R4 step, and it is worth its own reboot because three numbering
 * schemes meet here and every one of them can be wrong without any command failing:
 * the PD is a software number that must not collide with the firmware's own, the MPT
 * index is a hardware index into a table, and the lkey that a descriptor carries is
 * that index run through a rotation.  The entry is read back out of the mapped dMPT
 * table afterwards, so "the hardware stored what was sent" is checked rather than
 * assumed from a successful command.
 */
static int cmdR4Mr(uint64_t iova, uint64_t size)
{
    uint64_t in[kR4MrInCount];
    uint64_t out[kR4MrOutCount];
    uint32_t n = kR4MrOutCount;
    Cx3R4MrResult res;

    in[kR4MrInIova] = iova;
    in[kR4MrInSize] = size;
    memset(out, 0, sizeof(out));
    memset(&res, 0, sizeof(res));

    printf("r4mr: allocate a PD and enable the physical memory region\n");
    printf("  the reference's own MR is iova 0, size ~0, LOCAL_READ|LOCAL_WRITE, zero "
           "pages\n");
    printf("  (zero pages makes it PHYSICAL: no page list, mtt_addr 0, entity_size 12)\n");
    fflush(stdout);

    if (callStruct(kCx3SelR4Mr, in, kR4MrInCount, out, &n, &res, sizeof(res)) != 0)
        return -1;

    printf("  selector status : 0x%llx\n", (unsigned long long)out[kR4MrOutStatus]);
    if (res.status != 0) {
        if (res.status == (uint32_t)kIOReturnNotReady)
            printf("  the HCA is not initialised -- run m2dprofile + m2dbringup first\n");
        else if (res.status == (uint32_t)kIOReturnNoSpace)
            printf("  no PD or MPT left\n");
        else
            printf("  failed: status 0x%x -- see the kernel log\n", res.status);
        return -1;
    }

    printf("  protection domain : %u   (the firmware owns 0..%u)\n", res.pdn,
           res.pdReserved ? res.pdReserved - 1 : 0);
    printf("  MPT index         : %u   (the profile configured %u entries; the card's\n",
           res.mptIndex, res.mptCount);
    printf("                                    ceiling is %u, which is NOT what was "
           "configured)\n", res.mptMaxCap);
    printf("  key sent          : 0x%08x  (the raw index: key_to_hw_index(key) == index)\n",
           res.keySent);
    printf("  lkey for a WQE    : 0x%08x  <- hw_index_to_key; this is the number every\n",
           res.lkey);
    printf("                                    RX and TX descriptor must carry\n");
    printf("  region            : iova 0x%llx, size 0x%llx\n",
           (unsigned long long)res.iovaSent, (unsigned long long)res.sizeSent);
    printf("  SW2HW_MPT         : fw status 0x%x %s\n", res.fwStatus,
           res.fwStatus == 0 ? "ok" : "-- rejected");

    if (!res.readbackOk) {
        printf("\n  the MPT entry could NOT be read back, so the only evidence is the "
               "command\n  status above. That is not the same claim, and the log says "
               "why.\n");
        return 0;
    }

    printf("\n  MPT entry read back out of the mapped dMPT table (host memory, no "
           "command):\n");
    printf("    flags       : 0x%08x   status nibble 0x%x %s\n", res.flagsReadback,
           res.statusNibble,
           res.statusNibble == 0 ? "(hardware owns it -- it was taken)"
                                 : "(NOT taken: 0xf would mean software still owns it)");
    printf("    key         : 0x%08x   %s\n", res.keyReadback,
           res.keyReadback == res.keySent ? "(matches what was sent)"
                                          : "(DIFFERENT from what was sent)");
    printf("    pd_flags    : 0x%08x   (pd %u %s)\n", res.pdFlagsReadback,
           res.pdFlagsReadback & 0x1ffff,
           (res.pdFlagsReadback & 0x1ffff) == res.pdn ? "-- matches" : "-- MISMATCH");
    printf("    entity_size : %u\n", res.entitySizeReadback);
    printf("    mtt_addr    : 0x%llx  (0 is correct for a physical region)\n",
           (unsigned long long)res.mttAddrReadback);

    printf("\n  and what the firmware itself says (QUERY_MPT, opcode 0x0e):\n");
    if (!res.queryMptOk) {
        printf("    no answer (fw status 0x%x) -- the readback above is then the only "
               "view, and\n    nothing yet proves the firmware writes that copy of the "
               "table.\n", res.queryMptStatus);
    } else {
        printf("    flags       : 0x%08x   status nibble 0x%x %s\n", res.queryFlags,
               res.queryStatusNibble,
               res.queryStatusNibble == 0 ? "(hardware owns it)" : "(SW_OWNS still set)");
        printf("    key         : 0x%08x   %s\n", res.queryKey,
               res.queryKey == res.keySent ? "(matches)" : "(DIFFERENT)");
        printf("    pd_flags    : 0x%08x   (pd %u)\n", res.queryPdFlags,
               res.queryPdFlags & 0x1ffff);
        printf("    entity_size : %u\n", res.queryEntitySize);
        printf("    mtt_addr    : 0x%llx\n", (unsigned long long)res.queryMttAddr);
    }

    if (res.statusNibble != 0)
        printf("\n  The hardware did not take the entry even though the command "
               "succeeded.\n  That is the case this readback exists to catch.\n");
    else
        printf("\n  Stage 1 done: PD %u and MPT %u are real, and the lkey a descriptor "
               "needs is\n  0x%08x. Next is stage 2, the RX CQ.\n", res.pdn, res.mptIndex,
               res.lkey);
    return 0;
}


/*
 * fault -- inject the command-timeout states on purpose.
 *
 * Usage:
 *   cx3ctl fault wedge          poison the command mailbox as an abandonment would
 *   cx3ctl fault unwedge 1      clear it; the 1 is a confirmation and is refused otherwise
 *   cx3ctl fault hold <ms>      hold the transaction lock for <ms> (1..30000)
 *
 * Why the tool has this at all: the state it injects is otherwise only reachable by
 * breaking the card -- one accidental MAP_FA left the command register with GO set on
 * 2026-10-12 and every command after that timed out for minutes at a time.  With this,
 * "a poisoned mailbox makes a command fail at once instead of waiting", "the read-only
 * selectors still answer while a selector holds the lock", and "an HCA reset clears it"
 * are all testable without a broken device (docs/LOCK-IMPL.md).
 *
 * The exit code follows the same rule as 'reset': a refused unwedge is not success, because
 * a shell chain that continues after it would be running against a mailbox that is still
 * poisoned.
 */
static int cmdFault(const char *what, unsigned long long arg)
{
    uint64_t in[kFaultInCount];
    uint64_t out[kFaultOutCount];
    uint32_t n = kFaultOutCount;
    unsigned mode;

    if (!strcmp(what, "wedge"))         mode = kFaultModeWedge;
    else if (!strcmp(what, "unwedge"))  mode = kFaultModeUnwedge;
    else if (!strcmp(what, "hold"))     mode = kFaultModeHold;
    else {
        fprintf(stderr, "fault: unknown mode '%s' (wedge | unwedge 1 | hold <ms>)\n", what);
        return 2;
    }

    in[kFaultInMode] = mode;
    in[kFaultInArg]  = arg;
    memset(out, 0, sizeof(out));

    if (call(kCx3SelFault, in, kFaultInCount, out, &n) != 0) return -1;

    printf("fault %s (arg %llu):\n", what, arg);
    printf("  status            : %llu\n", (unsigned long long)out[kFaultOutStatus]);
    printf("  mailbox poisoned  : %s\n", out[kFaultOutWedged] ? "YES" : "no");
    printf("  poisoned by op    : 0x%llx%s\n", (unsigned long long)out[kFaultOutWedgedOp],
           (out[kFaultOutWedgedOp] == CX3_FAULT_OP_MARKER)
               ? "  (injected on purpose, not an abandonment)"
               : "");
    printf("  abandonments      : %llu   (real ones: commands the driver gave up on)\n",
           (unsigned long long)out[kFaultOutAbandons]);
    printf("  refused while held: %llu\n", (unsigned long long)out[kFaultOutRefused]);
    if (mode == kFaultModeHold)
        printf("  lock held for     : %llu ms\n",
               (unsigned long long)out[kFaultOutHeldMs]);

    if (out[kFaultOutStatus] != 0) {
        fprintf(stderr, "\n  The fault was NOT applied (status 0x%llx).\n",
                (unsigned long long)out[kFaultOutStatus]);
        if (out[kFaultOutStatus] == CX3_IORETURN_NOT_READY &&
            mode == kFaultModeUnwedge)
            fprintf(stderr, "  kIOReturnNotReady from 'unwedge' means the command register "
                            "still reads busy,\n  so this poison is not an injection: use "
                            "'cx3ctl reset force' or 'cx3ctl flr',\n  which reset the device "
                            "instead of posting into it.\n");
        return 3;
    }

    if (mode == kFaultModeWedge)
        printf("\n  Every command now fails at once with fw status 0x%x. That is the "
               "point: compare\n  'cx3ctl queryfw force' now with what it does after a "
               "reset.\n", CX3_FWSTATUS_WEDGED);
    return 0;
}

/*
 * m3portup -- the gated sequence: QUERY_PORT -> SET_PORT general -> SET_PORT qpn_calc ->
 * INIT_PORT -> QUERY_PORT.  That is the reference's own port activation, in its own order
 * (mlx4_en_netdev.c:1388, :1394, :1403), and the third command is what tells the firmware
 * where A0's MAC-derived QPN range starts -- without it the MAC table steers frames to a base
 * the firmware was never told.  The port number is the only parameter; every command field is
 * fixed in the kext, because a wrong op_modifier is a different operation rather than an
 * invalid one.
 */
static int cmdM3PortUp(unsigned port, unsigned mtu)
{
    uint64_t in[kPortUpInCount];
    uint64_t out[kPortUpOutCount];
    uint32_t n = kPortUpOutCount;

    in[kPortUpInPort] = port;
    in[kPortUpInMtu]  = mtu;
    memset(out, 0, sizeof(out));

    printf("m3portup: bring port %u up as Ethernet, in the reference's order\n", port);
    printf("  QUERY_PORT (gate) -> SET_PORT general -> SET_PORT qpn_calc -> INIT_PORT ->"
           " QUERY_PORT\n");
    printf("  No CLOSE_PORT: the reference's start path is general, qpn_calc then INIT_PORT.\n");
    fflush(stdout);

    if (call(kCx3SelM3PortUp, in, kPortUpInCount, out, &n) != 0) return -1;

    /*
     * The step count comes from the ABI rather than from this file: the chain gained the
     * qpn_calc command in 0.43.6, and "of 5" written here (as "of 4" was, against a device that
     * had grown a step) is a second copy of a fact the driver owns.  Two accounts of one run
     * that disagree is docs/DEFECTS.md F11.
     */
    printf("  stage reached      : %llu of %u  (1 gate, 2 general, 3 qpn_calc, 4 INIT_PORT,"
           " 5 all done)\n", (unsigned long long)out[kPortUpOutStep], (unsigned)kPortUpSteps);
    printf("  gate QUERY_PORT    : fw status 0x%llx\n",
           (unsigned long long)out[kPortUpOutQpFwStatus]);
    printf("  supported types    : %llu  (2 = Ethernet)\n",
           (unsigned long long)out[kPortUpOutSupported]);
    printf("  link bit before    : %llu\n", (unsigned long long)out[kPortUpOutLinkBefore]);
    printf("  SET_PORT general   : fw status 0x%llx   (mtu sent: %llu)\n",
           (unsigned long long)out[kPortUpOutSetFwStatus],
           (unsigned long long)out[kPortUpOutMtu]);
    printf("  SET_PORT qpn_calc  : fw status 0x%llx%s\n",
           (unsigned long long)out[kPortUpOutQpnCalcFwStatus],
           (out[kPortUpOutQpnCalcFwStatus] == CX3_FWSTATUS_NOT_POSTED)
               ? "   (0xfffffffe = NOT POSTED: the step never ran, or ran without recording)"
               : "");
    printf("  INIT_PORT          : fw status 0x%llx\n",
           (unsigned long long)out[kPortUpOutInitFwStatus]);
    printf("  link bit after     : %llu\n", (unsigned long long)out[kPortUpOutLinkAfter]);
    printf("  speed code after   : 0x%llx -> %u Mb/s\n",
           (unsigned long long)out[kPortUpOutSpeedAfter],
           cx3LinkSpeedMbps((uint32_t)out[kPortUpOutSpeedAfter]));

    if (out[kPortUpOutStatus] == CX3_IORETURN_UNSUPPORTED) {
        printf("\n  STOPPED AT THE GATE: the port does not advertise Ethernet; nothing "
               "was written.\n");
        return 2;
    }
    if (out[kPortUpOutStep] < kPortUpSteps && out[kPortUpOutStep] != 0 &&
        out[kPortUpOutQpnCalcFwStatus] != 0) {
        printf("\n  look at 'stage reached': a value below %u means qpn_calc was never sent, a\n"
               "  value of %u with a non-zero fw status means the firmware REJECTED the base\n"
               "  QPN, and 0xfffffffe means the step ran without recording an answer. Those\n"
               "  are three different failures; the kernel log names the one that happened.\n",
               (unsigned)kPortUpStepQpnCalc, (unsigned)kPortUpStepQpnCalc);
    }
    if (out[kPortUpOutStatus] != 0) {
        printf("\n  did not complete (status 0x%llx) -- see the kernel log\n",
               (unsigned long long)out[kPortUpOutStatus]);
        return 1;
    }
    if (out[kPortUpOutLinkAfter] == 1)
        printf("\n  LINK UP. Note which step said what: INIT_PORT's status only means the "
               "HCA accepted the command;\n  the link bit above is the measurement.\n");
    else
        printf("\n  Commands went through but the link bit is 0. That means DOWN and says "
               "nothing about why:\n  a cable, the far end, or an untrained port are all "
               "still open. These fields have no 'no cable' reason code.\n");
    return out[kPortUpOutLinkAfter] == 1 ? 0 : 3;
}

/*
 * abring -- the ring-initialisation A/B experiment.
 *
 * `a` hands ring A to the firmware, `b` hands ring B.  Running it both ways is the
 * point: if the change follows the ring that was handed over rather than the ring at a
 * particular address, the two runs disagree about which ring changed.
 */
static int cmdM3AbRing(int handOverB, unsigned eqn, unsigned dummyMtt)
{
    uint64_t in[kAbRingInCount];
    uint64_t out[kAbRingOutCount];
    uint32_t n = kAbRingOutCount;
    Cx3AbRingResult res;
    unsigned r, s;
    static const char *name[2] = { "A", "B" };

    in[kAbRingInHandOverB] = handOverB ? 1 : 0;
    in[kAbRingInEqn]       = eqn;
    in[kAbRingInDummyMtt]  = dummyMtt;
    memset(out, 0, sizeof(out));
    memset(&res, 0, sizeof(res));

    printf("abring: two identically built rings; only ring %s is handed to SW2HW_EQ "
           "as EQN %u\n", name[handOverB ? 1 : 0], eqn ? eqn : 28);
    if (dummyMtt)
        printf("  %u tracked dummy MTT entries shift both rings off offsets 64/72; "
               "the reservation is reused and reclaimed when safe\n", dummyMtt);
    printf("  the other ring is the control: it goes through every host-side step and "
           "never sees the firmware.\n");
    printf("  both are diffed byte-for-byte against their own baseline at every sample. "
           "No MAP_EQ, no doorbell.\n");
    fflush(stdout);

    if (callStruct(kCx3SelM3AbRing, in, kAbRingInCount, out, &n,
                   &res, sizeof(res)) != 0) return -1;

    if (out[kAbRingOutStatus] != 0) {
        printf("  the experiment did not run (status 0x%llx)\n",
               (unsigned long long)out[kAbRingOutStatus]);
        return 1;
    }
    printf("  handed over      : ring %s\n", name[res.handedOver]);
    printf("  SW2HW_EQ         : fw status 0x%x, took %u us\n", res.sw2hwStatus, res.cmdUs);
    printf("  HW2SW_EQ         : fw status 0x%x\n", res.hw2swStatus);
    printf("  QUERY_EQ after SW2HW_EQ : fw status 0x%x\n", res.queryEqStatus[0]);
    printf("       words: %08x %08x %08x %08x | %08x %08x %08x %08x\n",
           res.queryEqWords[0][0], res.queryEqWords[0][1], res.queryEqWords[0][2],
           res.queryEqWords[0][3], res.queryEqWords[0][4], res.queryEqWords[0][5],
           res.queryEqWords[0][6], res.queryEqWords[0][7]);
    printf("  QUERY_EQ after HW2SW_EQ : fw status 0x%x\n", res.queryEqStatus[1]);
    printf("  inbox mtt_base bytes   : h=0x%02x l=0x%08x -> reassembled 0x%llx\n",
           res.inboxMttH, res.inboxMttL,
           (unsigned long long)(((uint64_t)res.inboxMttH << 32) | res.inboxMttL));
    printf("        (that must equal the address the log's 'encoding' line printed; if\n"
           "         it does not, the context is not being encoded as intended -- which\n"
           "         is what a 32-bit shift of a 32-bit value produced here once)\n");
    printf("  MTT readback (host memory, no device involved):\n");
    for (r = 0; r < 2; ++r)
        printf("    ring %s: first entry 0x%08x, last entry 0x%08x\n", name[r],
               res.mttFirstEntry[r], res.mttLastEntry[r]);
    if (res.unknown)
        printf("  NOTE: a ring was left unfreed on purpose (the firmware may still hold "
               "it); a reboot reclaims it.\n");

    printf("\n  the two rings, so 'independent' can be checked:\n");
    for (r = 0; r < 2; ++r)
        printf("    ring %s: virt 0x%llx, first page 0x%llx, last page 0x%llx, "
               "MTT entry offset 0x%llx\n", name[r],
               (unsigned long long)res.ringVirt[r],
               (unsigned long long)res.busFirst[r],
               (unsigned long long)res.busLast[r],
               (unsigned long long)res.mttEntryOffset[r]);

    printf("\n  %-10s %-26s %-26s\n", "time (us)", "A: base/prev/nz/own",
           "B: base/prev/nz/own");
    printf("  (\"vs base\" = bytes differing from the baseline snapshot; \"vs prev\" = bytes\n"
           "   differing from the previous sample, which is what shows whether the ring is\n"
           "   still being written and not merely still different from the original)\n");
    for (s = 0; s < res.samples; ++s) {
        char ca[48], cb[48];
        snprintf(ca, sizeof(ca), "%u / %u / %u / %u", res.changedPerSample[0][s],
                 res.changedVsPrev[0][s], res.nonzeroPerSample[0][s],
                 res.ownerSetPerSample[0][s]);
        snprintf(cb, sizeof(cb), "%u / %u / %u / %u", res.changedPerSample[1][s],
                 res.changedVsPrev[1][s], res.nonzeroPerSample[1][s],
                 res.ownerSetPerSample[1][s]);
        printf("  +%-9u %-26s %-26s\n", res.sampleUs[s], ca, cb);
    }

    printf("\n  final state and where the change is:\n");
    for (r = 0; r < 2; ++r) {
        printf("    ring %s: %u byte(s) differ from baseline; pages changed 0x%02x; "
               "owner=1 in %u of %u slots\n", name[r], res.changedFinal[r],
               res.pagesChanged[r], res.ownerSetFinal[r], res.nent);
        if (res.firstChangeUs[r] == 0xffffffffu)
            printf("             NEVER changed in any sample -- nothing wrote to it\n");
        else
            printf("             first difference from baseline at +%u us; last CHANGE "
                   "between two samples at +%u us\n", res.firstChangeUs[r],
                   res.lastChangeUs[r]);
    }

    printf("\n  the first %u bytes of each ring, at three moments:\n", CX3_AB_HEAD);
    for (r = 0; r < 2; ++r) {
        static const char *when[3] = { "baseline        ", "first post-cmd  ",
                                       "after HW2SW_EQ  " };
        const uint8_t *bytes[3] = { res.headBefore[r], res.headFirst[r], res.headAfter[r] };
        unsigned w;
        printf("    ring %s:\n", name[r]);
        for (w = 0; w < 3; ++w) {
            unsigned k;
            printf("      %s:", when[w]);
            for (k = 0; k < CX3_AB_HEAD; ++k) {
                if (k % 32 == 0) printf("\n        %04x:", k);
                printf(" %02x", bytes[w][k]);
            }
            printf("\n");
        }
    }

    printf("\n  how to read it:\n");
    if (res.firstChangeUs[res.handedOver] != 0xffffffffu &&
        res.firstChangeUs[res.handedOver ^ 1] == 0xffffffffu)
        printf("    the change is on the ring that was handed over and NOT on the control\n"
               "    ring -> the change is associated with SW2HW_EQ and its target, not with\n"
               "    anything the host did. Run 'm3abring %s' for the swapped replication.\n",
               res.handedOver ? "a" : "b");
    else if (res.firstChangeUs[0] == 0xffffffffu && res.firstChangeUs[1] == 0xffffffffu)
        printf("    neither ring changed: this run did not reproduce the earlier reading.\n"
               "    That does not disprove it -- the change may need MAP_EQ or the doorbell,\n"
               "    or it may have happened inside the command. Check the log's\n"
               "    'first post-command sample' lines.\n");
    else
        printf("    BOTH rings changed, or the control did. The simple story does not hold;\n"
               "    check for page/MTT aliasing, host writes, and other DMA before drawing\n"
               "    any conclusion.\n");
    return 0;
}


static int cmdFwLoad(unsigned pages, int lowmem, int force, int fill)
{
    uint64_t in[kFwLoadInCount];
    uint64_t out[kFwLoadOutCount];
    uint32_t n = kFwLoadOutCount;

    in[kFwLoadInPages]  = pages;
    in[kFwLoadInLowmem] = lowmem ? 1 : 0;
    in[kFwLoadInForce]  = force ? 1 : 0;
    in[kFwLoadInFill]   = fill ? 1 : 0;
    memset(out, 0, sizeof(out));

    printf("fwload: %u pages (%u KiB) of FW area, then MAP_FA, RUN_FW, MOD_STAT_CFG\n",
           pages, pages * 4);
    if (force)
        printf("  force=1: the page count is not checked against QUERY_FW\n");
    if (fill)
        printf("  fill=1: writing a known pattern into the area before MAP_FA, so that\n"
               "          what is in it afterwards can be read unambiguously\n");
    printf("  Order matters: QUERY_DEV_CAP answers CMD_STAT_BAD_SYS_STATE (0x4)\n"
           "  until the firmware is running.\n");
    printf("  Accepted requests issue firmware commands; rejected requests may issue none.\n"
           "  If the machine hangs,\n"
           "  power-cycle and re-run 'probe' first.\n");
    fflush(stdout);

    if (call(kCx3SelFwLoad, in, kFwLoadInCount, out, &n) != 0) return -1;
    if (n != kFwLoadOutCount) {
        fprintf(stderr, "fwload: incomplete result (%u scalars, expected %u)\n",
                n, (unsigned)kFwLoadOutCount);
        return -1;
    }

    if (out[kFwLoadOutStatus] != 0) {
        fprintf(stderr, "  sequence did not complete: status 0x%llx\n",
                (unsigned long long)out[kFwLoadOutStatus]);
    }

    printf("  pages      : %llu\n", (unsigned long long)out[kFwLoadOutPages]);
    printf("  bus range  : 0x%llx .. 0x%llx\n",
           (unsigned long long)out[kFwLoadOutFirstBus],
           (unsigned long long)out[kFwLoadOutLastBus]);
    printf("  MAP_FA     : %llu entr(ies), fw status ",
           (unsigned long long)out[kFwLoadOutEntries]);
    if (out[kFwLoadOutMapFaStatus] == 0xffffffffull) printf("not reached\n");
    else printf("0x%llx %s\n", (unsigned long long)out[kFwLoadOutMapFaStatus],
                fwStatusName(out[kFwLoadOutMapFaStatus]));
    printf("  RUN_FW     : fw status ");
    if (out[kFwLoadOutRunFwStatus] == 0xffffffffull) printf("not reached\n");
    else printf("0x%llx %s\n", (unsigned long long)out[kFwLoadOutRunFwStatus],
                fwStatusName(out[kFwLoadOutRunFwStatus]));
    printf("  MOD_STAT_CFG: fw status ");
    if (out[kFwLoadOutModStatus] == 0xffffffffull) printf("not reached\n");
    else printf("0x%llx %s  %s\n", (unsigned long long)out[kFwLoadOutModStatus],
                fwStatusName(out[kFwLoadOutModStatus]),
                out[kFwLoadOutModStatus] == 0 ? "" :
                "(non-fatal: the reference only warns)");

    /* The ABI has no summary-validity bit. A rejected/partial sequence may
     * return no sample; even a completed zero sample is not write-history proof. */
    printf("  FW area    : %llu non-zero bytes, checksum 0x%08llx %s\n",
           (unsigned long long)out[kFwLoadOutNonzero],
           (unsigned long long)out[kFwLoadOutChecksum],
           out[kFwLoadOutStatus] ? " (sample may be absent or incomplete)" : " (sampled contents)");
    printf("  read it    : sudo cx3ctl fwarea [offset] [dwords]\n");

    if (out[kFwLoadOutStatus] == 0) {
        printf("\n  The firmware is running. QUERY_DEV_CAP should answer now:\n"
               "      sudo cx3ctl querydevcap force\n");
    }
    return out[kFwLoadOutStatus] == 0 ? 0 : -1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s <command>\n"
            "  probe             read-only registers (safe; start here)\n"
            "  dma               allocate the command mailbox only\n"
            "  queryfw [force]   HCR handshake + QUERY_FW (writes the device)\n"
            "  reset [force]     HCA reset; force bypasses the semaphore gate\n"
            "  peek <space> <offset> <count>   read-only dump; space 0=BAR0 1=BAR2 2=config\n"
            "  clearowner        write 0 to the ownership word\n"
            "  poke <space> <offset> <value>   one guarded store + read-back\n"
            "  flr               PCIe Function Level Reset (hardware reset)\n"
            "  querydevcap [force] [ignorebusy]  QUERY_DEV_CAP: device capability set\n"
            "                     ignorebusy posts even when the command register looks busy\n"
            "  fault wedge             poison the command mailbox on purpose: every later\n"
            "                     command then fails at once, without touching the HCR\n"
            "  fault unwedge 1         clear that poison; the 1 is a confirmation, and it is\n"
            "                     refused if the command register still reads busy\n"
            "  fault hold <ms>         hold the transaction lock for <ms> (1..30000), to show\n"
            "                     that probe/peek/survey/m3irqstatus still answer meanwhile\n"
            "  icmalloc <pages>      allocate the FW/ICM area; 0 frees it\n"
            "  m2dprofile [qp N] [cq N] [mtt N] ...   QUERY_DEV_CAP decode + profile\n"
            "                     knobs: qp rdmarc srq cq mcg mpt mtt (defaults are\n"
            "                     sized for one Ethernet port, not an IB stack)\n"
            "  m2dbringup        SET_ICM_SIZE, ICM tables, INIT_HCA (after m2dprofile)\n"
            "  m3equp [async] [nent N] [intr N] [eqe32|eqe64] [mttabs]\n"
            "                     mttabs = write an absolute MTT address instead of an offset\n"
            "                     QUERY_FUNC + one EQ (MTT, SW2HW_EQ, MAP_EQ, arm)\n"
            "  m3eqinfo             MTT readback + QUERY_EQ (read-only)\n"
            "  m3clrint             set up the legacy interrupt-clear register\n"
            "  r4mr [iova] [size]   R4.1: allocate a PD and enable the physical MR\n"
            "  r4cq [nent] [cqn]    R4.2: create the RX completion queue\n"
            "  r4rq [nent] [bufsz]  R4.3: build the receive ring: buffers + WQEs (memory)\n"
            "  r4qp [qpn]           R4.4: the RX QP RST->INIT->RTR->RTS and the RQ producer\n"
            "                     qpn 0 (the default) derives it from the firmware's\n"
            "                     reserved share and confirms it with QUERY_QP\n"
            "  r4ingress            R4.5: the port's MAC table (one command, whole table) and\n"
            "                     the single-ring RSS indirection QP pointing at the RX QP\n"
            "                     (no arguments: both halves read what they need from the\n"
            "                     device -- def_mac and stage 4's QPN)\n"
            "  r4tx [cq nent] [sq nent]  R4.6: the transmit side's RESOURCES -- a TX CQ, a\n"
            "                     send QP walked to RTS, and the send queue's memory (the WQE\n"
            "                     array with WQE 0 at its base + MTT). Sends NOTHING: no WQE is\n"
            "                     posted and\n"
            "                     no doorbell is rung, so this is not 'a frame can go out'. The\n"
            "                     numbers are chosen below num_qps - (1 << log_max_macs), both\n"
            "                     read at runtime, so the top slots stay free for A0's MAC table\n"
            "                     indexes; the tool prints that line and where both numbers sit\n"
            "  r4raw [posts] [rx ms]     R4.9: THE FIRST STAGE WHERE A BYTE MOVES -- it posts\n"
            "                     send WQEs, rings the send doorbell, and reads completions from\n"
            "                     both queues. IT PUTS FRAMES ON THE WIRE: a 60-byte broadcast\n"
            "                     with ethertype 0x88B5 (IEEE Local Experimental 1), which no\n"
            "                     device on a LAN interprets; ARP is stage 10. No arguments =\n"
            "                     one frame alone first and then the wrap (past two laps), plus\n"
            "                     a few seconds of waiting for received frames. 'r4raw 1' posts\n"
            "                     ONE frame and stops -- the cautious first run\n"
            "  txprobe [ms] [arm ms]     R4.9's remaining question, asked three ways: does the\n"
            "                     hardware execute ANY descriptor from the send queue (a NOP\n"
            "                     carries no data and still completes), does an MMIO doorbell\n"
            "                     on the UAR page the QPs were built with reach the device (the\n"
            "                     receive CQ's arm, judged by the COMP EQE it leaves in the\n"
            "                     event ring), and does the send queue need the doorbell record\n"
            "                     the public PRM's posting procedure updates. Sends no firmware\n"
            "                     command, so it cannot poison the mailbox and cannot leave the\n"
            "                     device needing a reset -- safe to run repeatedly on a live\n"
            "                     stack ('tools/verify.sh up' first, or after 'r4tx')\n"
            "\n"
            "  sizes               what each selector's dispatch entry demands: scalar-out\n"
            "                     count and structure-out size, which the kernel checks for\n"
            "                     EXACT equality -- and a mis-sized call fails with a zeroed\n"
            "                     output buffer, where 0 is this project's 'ok' value\n"
            "  bctest              send one CX3_BC UDP breadcrumb and report sendto errors\n"
            "  opentest            open and close the user client once without calling a\n"
            "                     selector; exercises newUserClient and clientClose only\n"
            "  rawjson SEL [in a,b,c] [hex AA BB] [out N] [struct N]\n"
            "                     call any selector and print the kernel's own values as\n"
            "                     one line of JSON -- this is what the verifier reads, so\n"
            "                     that PASS/FAIL is decided from values and not from prose\n"
            "  hcr OPCODE [inmod N] [opmod N] [mailbox] [imm] [busy] [wait MS]\n"
            "              [inbox HEX] [outbox N]\n"
            "                     send one HCR command with your parameters (needs cx3raw=1).\n"
            "                     This is the way to try a command without rebuilding the\n"
            "                     kext: nothing is interpreted on either side.\n"
            "  survey             the driver's whole current state, one call, no command sent\n"
            "  queryhca             read-only: QUERY_HCA -- which steering mode is in force\n"
            "  queryadapter         read-only: QUERY_ADAPTER -- complete raw outbox + board ID candidate\n"
            "  autonet status|enable|disable EXPECTED_BUILD [json]  bound, one-shot port-1 control\n"
            "  autonet plan  offline initialization stages, no device access\n"
            "  nativeport status|attach|detach PORT EXPECTED_BUILD [json]  independent native interface\n"
            "  nativeconfig status|vlan-all|vlan-clear|vlan-add|vlan-del|rxcsum|txcsum PORT VALUE EXPECTED_BUILD [json]\n"
            "    VALUE=0 for status/all/clear, VID=0..4094 for add/del (clear first), 0/1 for checksum; TX before attach\n"
            "  portqueue status|create|destroy PORT EXPECTED_BUILD [json]  bound per-port queues\n"
            "  portinfo PORT [refresh] [json]  cached per-port state, refresh sends QUERY_PORT\n"
            "  module PORT OFFSET LENGTH [json]  EEPROM MAD GET, max 48 bytes, page 0 only\n"
            "  pcielink             read-only: bounded conventional PCIe current/max link sample\n"
            "  identity [json]      host-only: boot/service/build/generation identity\n"
            "  boundpcie            JSON: identity -> PCIe sample -> identity, one connection\n"
            "  boundrss --allow-rss-experiment\n"
            "                       JSON: identity -> state-changing RSS stage -> identity, once\n"
            "  queryport N          read-only: QUERY_PORT for port N (raw bytes + decode)\n"
            "  m3portup N [mtu]     bring port N up as Ethernet, gated on that query\n"
            "  m3abring [a|b] [eqn] [dummyMtt]\n"
            "                     ring-initialisation A/B experiment (hand over A or B to\n"
            "                     EQN, default 28; dummyMtt leaks MTT entries first)\n"
            "  m3irqup [source]     open an interrupt session (needs m3clrint first\n"
            "                     if the source is legacy; it refuses otherwise)\n"

            "  m3irqtest [token]    register the token and post the event-mode NOP\n"
            "  m3irqstatus          what the interrupt path has seen (the verdict)\n"
            "  m3irqdown            close the session\n"
            "  m3eqring [page] [force]   QUERY_FW whose outbox is an EQ ring page\n"
            "                     destructive: pass force only when the ring is idle\n"
            "                     (can the HCA write into the ring at all?)\n"
            "  m3eqpoll [poll] [nop TOKEN] [from N] [dump N] [wait MS] [peek]\n"
            "                     event-mode NOP, then poll the EQ for its CMD EQE\n"
            "  fwarea [offset] [dwords]   read-only: what is IN the FW area\n"
            "  fwoutbox [page] [dwords] [ignorebusy]\n"
            "                     QUERY_FW whose answer must land in the FW area; the\n"
            "                     direct test of whether the device can write there\n"
            "  fwload <fw_pages> [lowmem] [force] [fill]  M2b: MAP_FA + RUN_FW + MOD_STAT_CFG\n"
            "                     force also skips the check against QUERY_FW's page count\n"
            "  all               probe, then dma, then queryfw\n", argv0);
}

int main(int argc, char **argv)
{
    const char *cmd;
    int rc = 0;

    if (argc < 2) { usage(argv[0]); return 2; }
    cmd = argv[1];

    if (strcmp(cmd, "probe") == 0) {
        rc = cmdProbe();
    } else if (strcmp(cmd, "bctest") == 0) {
        rc = cmdBreadcrumbTest();
    } else if (strcmp(cmd, "opentest") == 0) {
        rc = cmdOpenTest();
    } else if (strcmp(cmd, "dma") == 0) {
        rc = cmdDma();
    } else if (strcmp(cmd, "queryfw") == 0) {
        int force = (argc > 2 && strcmp(argv[2], "force") == 0);
        rc = cmdQueryFw(force);
    } else if (strcmp(cmd, "reset") == 0) {
        int bypass = (argc > 2 && strcmp(argv[2], "force") == 0);
        rc = cmdReset(bypass);
    } else if (strcmp(cmd, "peek") == 0) {
        if (argc < 5) { usage(argv[0]); return 2; }
        rc = cmdPeek((unsigned)strtoul(argv[2], NULL, 0),
                     strtoull(argv[3], NULL, 0),
                     (unsigned)strtoul(argv[4], NULL, 0));
    } else if (strcmp(cmd, "clearowner") == 0) {
        rc = cmdClearOwner();
    } else if (strcmp(cmd, "poke") == 0) {
        if (argc < 5) { usage(argv[0]); return 2; }
        rc = cmdPoke((unsigned)strtoul(argv[2], NULL, 0),
                     strtoull(argv[3], NULL, 0),
                     strtoull(argv[4], NULL, 0));
    } else if (strcmp(cmd, "flr") == 0) {
        rc = cmdFlr();
    } else if (strcmp(cmd, "querydevcap") == 0) {
        int force = 0, ignoreBusy = 0, i;
        for (i = 2; i < argc; ++i) {
            if (strcmp(argv[i], "force") == 0)           force = 1;
            else if (strcmp(argv[i], "ignorebusy") == 0) ignoreBusy = 1;
            else { fprintf(stderr, "querydevcap: unknown option %s\n", argv[i]); return 2; }
        }
        rc = cmdQueryDevCap(force, ignoreBusy);
    } else if (strcmp(cmd, "fwarea") == 0) {
        unsigned long long off = (argc > 2) ? strtoull(argv[2], NULL, 0) : 0;
        unsigned long cnt = (argc > 3) ? strtoul(argv[3], NULL, 0) : 0;
        rc = cmdFwArea(off, cnt);
    } else if (strcmp(cmd, "fwoutbox") == 0) {
        unsigned long page = 0, dwords = 8;
        int ignoreBusy = 0, i2;
        for (i2 = 2; i2 < argc; ++i2) {
            if (strcmp(argv[i2], "ignorebusy") == 0) ignoreBusy = 1;
            else if (i2 == 2) page   = strtoul(argv[i2], NULL, 0);
            else if (i2 == 3) dwords = strtoul(argv[i2], NULL, 0);
            else { fprintf(stderr, "fwoutbox: unknown option %s\n", argv[i2]); return 2; }
        }
        rc = cmdFwOutbox((unsigned)page, (unsigned)dwords, ignoreBusy);
    } else if (strcmp(cmd, "m2dprofile") == 0) {
        /* Defaults chosen for a single-port Ethernet driver, not for an IB
         * stack: the reference's default_profile asks for 2^18 QPs. */
        unsigned qp = 1024, rdmarc = 16, srq = 64, cq = 256, mcg = 256,
                 mpt = 2048, mtt = 4096;
        int i3;
        for (i3 = 2; i3 + 1 < argc; i3 += 2) {
            unsigned v = (unsigned)strtoul(argv[i3 + 1], NULL, 0);
            if      (!strcmp(argv[i3], "qp"))     qp = v;
            else if (!strcmp(argv[i3], "rdmarc")) rdmarc = v;
            else if (!strcmp(argv[i3], "srq"))    srq = v;
            else if (!strcmp(argv[i3], "cq"))     cq = v;
            else if (!strcmp(argv[i3], "mcg"))    mcg = v;
            else if (!strcmp(argv[i3], "mpt"))    mpt = v;
            else if (!strcmp(argv[i3], "mtt"))    mtt = v;
            else { fprintf(stderr, "m2dprofile: unknown knob %s\n", argv[i3]); return 2; }
        }
        rc = cmdM2dProfile(qp, rdmarc, srq, cq, mcg, mpt, mtt);
    } else if (strcmp(cmd, "m2dbringup") == 0) {
        rc = cmdM2dBringUp();
    } else if (strcmp(cmd, "m3equp") == 0) {
        unsigned nentLog2 = 0, intr = 0, eqeFactor = 1;
        int asyncSize = 0, mttAbs = 0, i4;
        for (i4 = 2; i4 < argc; ++i4) {
            if      (!strcmp(argv[i4], "async"))     asyncSize = 1;
            else if (!strcmp(argv[i4], "mttabs"))    mttAbs = 1;
            else if (!strcmp(argv[i4], "nent") && i4 + 1 < argc) nentLog2 = (unsigned)strtoul(argv[++i4], NULL, 0);
            else if (!strcmp(argv[i4], "intr") && i4 + 1 < argc) intr = (unsigned)strtoul(argv[++i4], NULL, 0);
            else if (!strcmp(argv[i4], "eqe32"))     eqeFactor = 0;
            else if (!strcmp(argv[i4], "eqe64"))     eqeFactor = 1;
            else { fprintf(stderr, "m3equp: unknown option %s\n", argv[i4]); return 2; }
        }
        rc = cmdM3EqUp(nentLog2, intr, eqeFactor, asyncSize, mttAbs);
    } else if (strcmp(cmd, "m3eqring") == 0) {
        unsigned page = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 0) : 0;
        int force = (argc > 3 && strcmp(argv[3], "force") == 0);
        rc = cmdM3EqRing(page, force);
    } else if (strcmp(cmd, "m3eqinfo") == 0) {
        rc = cmdM3EqInfo();
    } else if (strcmp(cmd, "m3irqup") == 0) {
        unsigned source = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 0) : 0;
        int forceLegacy = (argc > 3 && strcmp(argv[3], "force") == 0);
        rc = cmdM3IrqUp(source, forceLegacy);
    } else if (strcmp(cmd, "m3irqtest") == 0) {
        unsigned token = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 0) : 0x1234;
        rc = cmdM3IrqTest(token);
    } else if (strcmp(cmd, "m3irqstatus") == 0) {
        rc = cmdM3IrqStatus();
    } else if (strcmp(cmd, "m3clrint") == 0) {
        rc = cmdM3ClrInt();
    } else if (strcmp(cmd, "sizes") == 0) {
        rc = cmdSizes();
    } else if (strcmp(cmd, "txprobe") == 0) {
        unsigned postWait = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 0) : 0;
        unsigned armWait  = (argc > 3) ? (unsigned)strtoul(argv[3], NULL, 0) : 0;
        rc = cmdTxProbe(postWait, armWait);
    } else if (strcmp(cmd, "rawjson") == 0) {
        rc = cmdRawJson(argc, argv, 2);
    } else if (strcmp(cmd, "hcr") == 0) {
        rc = cmdHcr(argc, argv, 2);
    } else if (strcmp(cmd, "survey") == 0) {
        rc = cmdSurvey();
    } else if (strcmp(cmd, "netstat") == 0) {
        rc = cmdNetStat();
    } else if (strcmp(cmd, "netlink") == 0) {
        rc = cmdNetLink(argc > 2 ? argv[2] : "report");
    } else if (strcmp(cmd, "netoff") == 0) {
        rc = cmdNetOff(argc > 2 ? argv[2] : "report");
    } else if (strcmp(cmd, "netport") == 0) {
        rc = cmdNetPort(argc > 2 ? argv[2] : "report");
    } else if (strcmp(cmd, "netfilt") == 0) {
        rc = cmdNetFilt(argc > 2 ? argv[2] : "report");
    } else if (strcmp(cmd, "netarm") == 0) {
        unsigned waitMs = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 0) : 0;
        rc = cmdNetArm(waitMs);
    } else if (strcmp(cmd, "fault") == 0) {
        /*
         * The second scalar is a confirmation for 'unwedge' and the hold time for 'hold';
         * it defaults to 0, which both modes refuse.  That is deliberate: clearing a poison
         * and holding the transaction lock are things a caller should have to ask for on
         * purpose, and a default that did them would make the harmless invocation the
         * dangerous one.
         */
        const char *what = (argc > 2) ? argv[2] : "wedge";
        unsigned long long arg = (argc > 3) ? strtoull(argv[3], NULL, 0) : 0ull;
        rc = cmdFault(what, arg);
    } else if (strcmp(cmd, "r4rq") == 0) {
        unsigned nent = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 0) : 0;
        unsigned bb   = (argc > 3) ? (unsigned)strtoul(argv[3], NULL, 0) : 0;
        rc = cmdR4Rq(nent, bb);
    } else if (strcmp(cmd, "r4qp") == 0) {
        unsigned qpn = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 0) : 0;
        rc = cmdR4Qp(qpn);
    } else if (strcmp(cmd, "r4ingress") == 0) {
        rc = cmdR4Ingress();
    } else if (strcmp(cmd, "r4tx") == 0) {
        unsigned cqnent = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 0) : 0;
        unsigned sqnent = (argc > 3) ? (unsigned)strtoul(argv[3], NULL, 0) : 0;
        rc = cmdR4Tx(cqnent, sqnent);
    } else if (strcmp(cmd, "r4raw") == 0) {
        /*
         * `cx3ctl r4raw` = the driver's defaults (one frame first, then the wrap; a few
         * seconds of receive budget).  `r4raw 1` = ONE frame and nothing else, which is the
         * cautious first run on a card that has never sent anything.
         */
        unsigned posts  = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 0) : 0;
        unsigned budget = (argc > 3) ? (unsigned)strtoul(argv[3], NULL, 0) : 0;
        rc = cmdR4Raw(posts, budget);
    } else if (strcmp(cmd, "r4cq") == 0) {
        unsigned nent = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 0) : 0;
        unsigned cqn  = (argc > 3) ? (unsigned)strtoul(argv[3], NULL, 0) : 0;
        rc = cmdR4Cq(nent, cqn);
    } else if (strcmp(cmd, "r4mr") == 0) {
        uint64_t iova = (argc > 2) ? strtoull(argv[2], NULL, 0) : 0;
        uint64_t size = (argc > 3) ? strtoull(argv[3], NULL, 0) : 0;
        rc = cmdR4Mr(iova, size);
    } else if (strcmp(cmd, "queryhca") == 0) {
        rc = cmdM3QueryHca();
    } else if (strcmp(cmd, "queryadapter") == 0) {
        if (argc != 2) { usage(argv[0]); return 2; }
        rc = cmdQueryAdapter();
    } else if (strcmp(cmd, "autonet") == 0) {
        uint32_t action;
        if (argc == 3 && !strcmp(argv[2], "plan")) {
            puts("{\"schema\":\"cx3-auto-network-plan-v1\",\"hardwareAccess\":false,\"port\":1,\"mtu\":1500,\"selectors\":[3,2,10,8,13,14,24,25,15,28,29,30,34,35,36,39],\"failurePolicy\":\"stop-no-retry\"}");
            return 0;
        }
        if ((argc != 4 && argc != 5) || (argc == 5 && strcmp(argv[4], "json"))) {
            usage(argv[0]); return 2;
        }
        if (!strcmp(argv[2], "status")) action = kCx3AutoStatus;
        else if (!strcmp(argv[2], "enable")) action = kCx3AutoEnable;
        else if (!strcmp(argv[2], "disable")) action = kCx3AutoDisable;
        else { usage(argv[0]); return 2; }
        rc = cmdAutoNetwork(action, argv[3], argc == 5);
    } else if (strcmp(cmd, "nativeconfig") == 0) {
        uint32_t action,port,value;char*end;unsigned long long parsed;
        if((argc!=6&&argc!=7)||(argc==7&&strcmp(argv[6],"json"))||
           (strcmp(argv[3],"1")&&strcmp(argv[3],"2"))||!argv[4][0]||strlen(argv[4])>10){usage(argv[0]);return 2;}
        for(const char*p=argv[4];*p;++p)if(*p<'0'||*p>'9'){usage(argv[0]);return 2;}
        parsed=strtoull(argv[4],&end,10);if(*end||parsed>0xffffffffull){usage(argv[0]);return 2;}
        if(!strcmp(argv[2],"status"))action=CX3_NCFG_STATUS;
        else if(!strcmp(argv[2],"vlan-all"))action=CX3_NCFG_VLAN_ALL;
        else if(!strcmp(argv[2],"vlan-clear"))action=CX3_NCFG_VLAN_CLEAR;
        else if(!strcmp(argv[2],"vlan-add"))action=CX3_NCFG_VLAN_ADD;
        else if(!strcmp(argv[2],"vlan-del"))action=CX3_NCFG_VLAN_DEL;
        else if(!strcmp(argv[2],"rxcsum"))action=CX3_NCFG_RXCSUM;
        else if(!strcmp(argv[2],"txcsum"))action=CX3_NCFG_TXCSUM;
        else{usage(argv[0]);return 2;}
        port=(uint32_t)strtoul(argv[3],NULL,10);value=(uint32_t)parsed;
        rc=cmdNativeConfig(port,action,value,argv[5],argc==7);
    } else if (strcmp(cmd, "nativeport") == 0) {
        uint32_t action, port;
        if ((argc!=5 && argc!=6) || (argc==6 && strcmp(argv[5],"json")) ||
            (strcmp(argv[3],"1") && strcmp(argv[3],"2"))) { usage(argv[0]); return 2; }
        if (!strcmp(argv[2],"status")) action=CX3_NATIVE_STATUS;
        else if (!strcmp(argv[2],"attach")) action=CX3_NATIVE_ATTACH;
        else if (!strcmp(argv[2],"detach")) action=CX3_NATIVE_DETACH;
        else { usage(argv[0]); return 2; }
        port=(uint32_t)strtoul(argv[3],NULL,10);
        rc=cmdNativePort(port,action,argv[4],argc==6);
    } else if (strcmp(cmd, "portqueue") == 0) {
        uint32_t action, port;
        if ((argc!=5 && argc!=6) || (argc==6 && strcmp(argv[5],"json")) ||
            (strcmp(argv[3],"1") && strcmp(argv[3],"2"))) { usage(argv[0]); return 2; }
        if (!strcmp(argv[2],"status")) action=CX3_PORT_QUEUE_STATUS;
        else if (!strcmp(argv[2],"create")) action=CX3_PORT_QUEUE_CREATE;
        else if (!strcmp(argv[2],"destroy")) action=CX3_PORT_QUEUE_DESTROY;
        else { usage(argv[0]); return 2; }
        port=(uint32_t)strtoul(argv[3],NULL,10);
        rc=cmdPortQueue(port,action,argv[4],argc==6);
    } else if (strcmp(cmd, "portinfo") == 0) {
        uint32_t port; int refresh=0, json=0; char *end;
        if (argc < 3 || argc > 5 || (strcmp(argv[2],"1") && strcmp(argv[2],"2"))) {
            usage(argv[0]); return 2;
        }
        port = (uint32_t)strtoul(argv[2],&end,10);
        for (int i=3; i<argc; ++i) {
            if (!strcmp(argv[i],"refresh") && !refresh) refresh=1;
            else if (!strcmp(argv[i],"json") && !json) json=1;
            else { usage(argv[0]); return 2; }
        }
        rc = cmdPortInfo(port,refresh,json);
    } else if (strcmp(cmd, "module") == 0) {
        uint64_t input[3]; int i;
        if ((argc != 5 && argc != 6) || (argc == 6 && strcmp(argv[5], "json"))) {
            usage(argv[0]); return 2;
        }
        for (i = 0; i < 3; ++i) {
            char *end; const char *arg = argv[i+2];
            if (*arg < '0' || *arg > '9') return 2;
            errno = 0; input[i] = (uint64_t)strtoull(arg, &end, 0);
            if (errno || end == arg || *end) return 2;
        }
        rc = cmdModuleRead(input[0], input[1], input[2], argc == 6);
    } else if (strcmp(cmd, "pcielink") == 0) {
        if (argc != 2) { usage(argv[0]); return 2; }
        rc = cmdPcieLink();
    } else if (strcmp(cmd, "identity") == 0) {
        if (argc > 3 || (argc == 3 && strcmp(argv[2], "json"))) { usage(argv[0]); return 2; }
        rc = cmdRuntimeIdentity(argc == 3);
    } else if (strcmp(cmd, "boundpcie") == 0) {
        if (argc != 2) { usage(argv[0]); return 2; }
        rc = cmdBoundPcieJson();
    } else if (strcmp(cmd, "boundrss") == 0) {
        if (argc != 3 || strcmp(argv[2], "--allow-rss-experiment")) { usage(argv[0]); return 2; }
        rc = cmdBoundRssJson(1);
    } else if (strcmp(cmd, "queryport") == 0) {
        unsigned port = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 0) : 1;
        rc = cmdM3QueryPort(port);
    } else if (strcmp(cmd, "m3portup") == 0) {
        unsigned port = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 0) : 1;
        unsigned mtu  = (argc > 3) ? (unsigned)strtoul(argv[3], NULL, 0) : 0;
        rc = cmdM3PortUp(port, mtu);
    } else if (strcmp(cmd, "m3abring") == 0) {
        int handOverB = (argc > 2 && (argv[2][0] == 'b' || argv[2][0] == 'B'));
        unsigned eqn  = (argc > 3) ? (unsigned)strtoul(argv[3], NULL, 0) : 0;
        unsigned dmy  = (argc > 4) ? (unsigned)strtoul(argv[4], NULL, 0) : 0;
        rc = cmdM3AbRing(handOverB, eqn, dmy);
    } else if (strcmp(cmd, "m3irqdown") == 0) {
        rc = cmdM3IrqDown();
    } else if (strcmp(cmd, "m3eqpoll") == 0) {
        int fireNop = 1;
        unsigned token = 0x1234, consIndex = 0, dwords = 0, waitMs = 50;
        int peekFlag = 0;
        int i5;
        for (i5 = 2; i5 < argc; ++i5) {
            if      (!strcmp(argv[i5], "poll"))  fireNop = 0;
            else if (!strcmp(argv[i5], "nop") && i5 + 1 < argc) token = (unsigned)strtoul(argv[++i5], NULL, 0);
            else if (!strcmp(argv[i5], "from") && i5 + 1 < argc) consIndex = (unsigned)strtoul(argv[++i5], NULL, 0);
            else if (!strcmp(argv[i5], "dump") && i5 + 1 < argc) dwords = (unsigned)strtoul(argv[++i5], NULL, 0);
            else if (!strcmp(argv[i5], "wait") && i5 + 1 < argc) waitMs = (unsigned)strtoul(argv[++i5], NULL, 0);
            else if (!strcmp(argv[i5], "peek")) peekFlag = 1;
            else { fprintf(stderr, "m3eqpoll: unknown option %s\n", argv[i5]); return 2; }
        }
        rc = cmdM3EqPoll(fireNop, token, consIndex, dwords, waitMs, peekFlag);
    } else if (strcmp(cmd, "icmalloc") == 0) {
        if (argc < 3) { usage(argv[0]); return 2; }
        rc = cmdIcmAlloc((unsigned)strtoul(argv[2], NULL, 0));
    } else if (strcmp(cmd, "fwload") == 0) {
        if (argc < 3) { usage(argv[0]); return 2; }
        int lowmem = 0, force = 0, fill = 0, i;
        for (i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "lowmem") == 0)       lowmem = 1;
            else if (strcmp(argv[i], "force") == 0)   force = 1;
            else if (strcmp(argv[i], "fill") == 0)    fill = 1;
            else { fprintf(stderr, "fwload: unknown option %s\n", argv[i]); return 2; }
        }
        rc = cmdFwLoad((unsigned)strtoul(argv[2], NULL, 0), lowmem, force, fill);
    } else if (strcmp(cmd, "icmfree") == 0) {
        rc = cmdIcmAlloc(0);
    } else if (strcmp(cmd, "all") == 0) {
        if ((rc = cmdProbe()) == 0) { printf("\n"); rc = cmdDma(); }
        if (rc == 0) { printf("\n"); rc = cmdQueryFw(0); }
    } else {
        usage(argv[0]);
        return 2;
    }

    if (gConn) IOServiceClose(gConn);
    return rc == 0 ? 0 : 1;
}
