/*
 * Part of cx3-mlx4, a macOS IOKit port of the FreeBSD Mellanox driver.
 *
 * Original work in this repository.  It cites no upstream file:line and is
 * derived from none; the licence in LICENSE governs it.
 *
 * Copyright (c) 2026 Hi-Jiajun and contributors
 */

#ifndef CX3_DEBUG_H
#define CX3_DEBUG_H

/*
 * Boot-argument gated diagnostics, and a console echo for the paths that need it.
 *
 * Why this exists, concretely: this project's cost is dominated by reboots.  Every kernel
 * change needs one, so the goal is that ONE reboot answers as many questions as possible
 * and that the boot it produces is observable even when it goes wrong.  Two things were
 * missing, and both are borrowed from drivers on this machine that already work --
 * Acidanthera's itlwm and NullMoth's NVIDIA kext:
 *
 *   1. kprintf in addition to IOLog.  IOLog goes through the logging subsystem, which is
 *      not usable early in boot; kprintf writes to the console directly and still works
 *      before IOLog can deliver anything.  This port has a concrete history of needing it:
 *      an early build ran device work from start(), hung the machine before any IOLog line
 *      could be flushed, and the following six boots hung the same way with nothing to read.
 *      NullMoth's FBLOG writes both, for exactly that reason.
 *
 *   2. A boot-argument switch for "do not start at all".  NullMoth refuses to start its
 *      framebuffer unless nvfb=1 is passed ("boot-loop guard"), so a driver that hangs the
 *      machine can be taken out of the boot path without editing anything or booting into a
 *      recovery environment.  This port has had that failure mode and had no such guard.
 *
 * The switches (pass with the -v style syntax, e.g. cx3off=1):
 *
 *   cx3off=1     do not start.  The kext still loads and matches, start() returns false,
 *                and the machine boots.  For a driver under development whose failure mode
 *                is "the machine does not come back", this is the most valuable switch here.
 *   cx3echo=1    every LOG line also goes to kprintf.  Off by default because kprintf is a
 *                synchronous console write and this driver logs a lot on the hot paths.
 *   cx3debug=N   raises the verbosity for lines that are gated on it.  0 (default) keeps the
 *                per-step status; higher values add the surroundings of each step.
 *   cx3quarfree=1 turn the reset-path quarantine OFF: memory that the device may still be
 *                able to reach is freed as it was before 0.44.5 instead of being kept and
 *                filled with a pattern.  This is the escape hatch for the quarantine itself
 *                -- it is the only one of these switches whose default is the SAFE setting,
 *                so it exists to compare the two behaviours, not to rescue a boot.
 *
 * The functions cache their answer: PE_parse_boot_argn walks the boot arguments, and these
 * are called on paths that can run per-interrupt.
 */

#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>

/* Returns true when cx3off=1 was passed: "do not start". */
inline bool cx3BootOff()
{
    static int cached = -1;
    int v = 0;
    if (cached < 0) {
        cached = 0;
        if (PE_parse_boot_argn("cx3off", &v, sizeof(v)) && v) cached = 1;
    }
    return cached == 1;
}

/* Returns true when cx3echo=1 was passed: mirror every LOG line to the console. */
inline bool cx3BootEcho()
{
    static int cached = -1;
    int v = 0;
    if (cached < 0) {
        cached = 0;
        if (PE_parse_boot_argn("cx3echo", &v, sizeof(v)) && v) cached = 1;
    }
    return cached == 1;
}

/*
 * Returns true when cx3raw=1 was passed: allow the general-purpose command selector.
 *
 * This is the one switch that is a security-relevant capability rather than a verbosity
 * knob.  The selector it guards can send any command to the HCA, including ones that make
 * the hardware begin referring to host memory; that is exactly what is needed to develop a
 * driver and exactly what should not exist in a released one.  Making it a boot argument
 * means it can be enabled without editing or rebuilding the kext, and that a published
 * build is safe by default rather than safe by remembering.
 */
inline bool cx3RawAllowed()
{
    static int cached = -1;
    int v = 0;
    if (cached < 0) {
        cached = 0;
        if (PE_parse_boot_argn("cx3raw", &v, sizeof(v)) && v) cached = 1;
    }
    return cached == 1;
}

/* Returns cx3debug=N (0 when absent). */
inline int cx3BootDebug()
{
    static int cached = -1;
    int v = 0;
    if (cached < 0) {
        cached = 0;
        if (PE_parse_boot_argn("cx3debug", &v, sizeof(v))) cached = v;
    }
    return cached;
}

/*
 * LEGACY KNOB, NO LONGER CONSULTED FOR RELEASE DECISIONS.
 *
 * 0.44.5 made cx3quarfree=1 free device-facing memory on the reset path, for an A/B on the
 * freeze.  That switch is exactly the unsafe direction: it releases pages whose owner is
 * unknown, so a crash it "removes" only proves the memory was being written.  The release
 * decision now always retains when the device may still hold the block, and there is no
 * boot-argument override (docs/reports/safety-lifecycle-audit.md).  The parser is kept so an
 * existing boot-arg does not become an unknown-argument surprise; nothing reads its value.
 */
inline bool cx3QuarantineOff()
{
    static int cached = -1;
    int v = 0;
    if (cached < 0) {
        cached = 0;
        if (PE_parse_boot_argn("cx3quarfree", &v, sizeof(v)) && v) cached = 1;
    }
    return cached == 1;
}

/*
 * The two logging macros.
 *
 * LOG is the ordinary one: the logging subsystem, plus the console when cx3echo=1 was
 * passed.  LOGBOOT always writes both, and is for the paths where the logging subsystem
 * may not be delivering anything yet -- start(), the HCA reset, and anything a machine that
 * does not come back would have got to before it stopped.
 *
 * Both keep the "Cx3Mlx4: " prefix so that one predicate still finds every line of this
 * driver in the log, which is what every diagnosis in these notes has relied on.
 */
#define CX3_LOG_PREFIX "Cx3Mlx4: "

#define LOG(fmt, ...)                                                     \
    do {                                                                  \
        IOLog(CX3_LOG_PREFIX fmt "\n", ##__VA_ARGS__);                     \
        if (cx3BootEcho())                                                \
            kprintf(CX3_LOG_PREFIX fmt "\n", ##__VA_ARGS__);              \
    } while (0)

#define LOGBOOT(fmt, ...)                                                 \
    do {                                                                  \
        kprintf(CX3_LOG_PREFIX fmt "\n", ##__VA_ARGS__);                  \
        IOLog(CX3_LOG_PREFIX fmt "\n", ##__VA_ARGS__);                    \
    } while (0)

/* Gated on cx3debug >= level.  For lines that are useful only when chasing something. */
#define LOGV(level, fmt, ...)                                             \
    do {                                                                  \
        if (cx3BootDebug() >= (level)) LOG(fmt, ##__VA_ARGS__);           \
    } while (0)

#endif /* CX3_DEBUG_H */

