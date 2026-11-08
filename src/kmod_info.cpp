/*
 * Part of cx3-mlx4, a macOS IOKit port of the FreeBSD Mellanox driver.
 *
 * Original work in this repository.  It cites no upstream file:line and is
 * derived from none; the licence in LICENSE governs it.
 *
 * Copyright (c) 2026 Hi-Jiajun and contributors
 */

/*
 * kmod_info for Cx3Mlx4.
 *
 * A kext has to export the _kmod_info symbol.  Xcode's kernel-extension target
 * generates it; the Command Line Tools linker does not, so kmutil load refuses
 * the bundle with "kexts must have a _kmod_info symbol" (rc 31).
 *
 * Every working third-party kext on this machine (the four NullMoth NVRM*
 * kexts, HighPointIOP/RR) exports it the same way, and mach/kmod.h provides the
 * KMOD_EXPLICIT_DECL macro that builds exactly that structure.  The start and
 * stop entry points are no-ops returning KERN_SUCCESS: an IOKit kext is driven
 * by its Info.plist personalities, not by the kmod entry point.
 */
#include <mach/kmod.h>
#include <mach/kern_return.h>

#ifndef CX3_BUNDLE_VERSION
#error "Build through build.sh: kmod version must come from the staged source plist"
#endif

extern "C" kern_return_t Cx3Mlx4_kmod_start(kmod_info_t *ki, void *data)
{
    (void)ki;
    (void)data;
    return KERN_SUCCESS;
}

extern "C" kern_return_t Cx3Mlx4_kmod_stop(kmod_info_t *ki, void *data)
{
    (void)ki;
    (void)data;
    return KERN_SUCCESS;
}

extern "C" {
KMOD_EXPLICIT_DECL(com.hijiajun.driver.Cx3Mlx4, CX3_BUNDLE_VERSION,
                   Cx3Mlx4_kmod_start, Cx3Mlx4_kmod_stop)
}
