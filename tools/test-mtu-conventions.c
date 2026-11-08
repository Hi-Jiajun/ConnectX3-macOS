/*
 * test-mtu-conventions.c -- the three "sizes" of the MTU path, checked against each other
 * rather than against a comment.
 *
 * Why this exists.  Three different quantities meet in the MTU path, each belonging to a
 * different layer, and reading one as another is the defect that only shows up on a machine --
 * as a frame the port drops, or as an MTU that cannot be set at all:
 *
 *   stack frame     L2 header + payload, no FCS        what outputPacket is handed, 1514 @ MTU 1500
 *   family answer   L2 header + payload + FCS          IOEthernetController.h:45, 1518 @ MTU 1500
 *   wire size       + the VLAN tag this driver does not use: SET_PORT general's field, 1522 @ MTU 1500
 *
 * What IS checked here:
 *   - the three constants against their references (literals on the right-hand side, the same
 *     rule the other test-*.c files follow: adopting a new reference version is then a visible
 *     edit in two places rather than a silent drift);
 *   - the identities that tie the conventions together, including the cross-header agreement
 *     between this driver's constants and Apple's kIOEthernetMaxPacketSize;
 *   - the bound arithmetic for the geometry the chain actually builds: m3portup programs a 1522
 *     byte wire size (its default, src/Cx3Mlx4.cpp), r4rq builds 2048-byte receive buffers
 *     (tools/verify.sh), and kNetTxBufBytes is 2048 (src/Cx3Mlx4.h).
 *
 * What is NOT checked: the body of Cx3Mlx4::netStackFrameMax.  This is a host program and that
 * is a kernel method, so the arithmetic is duplicated here -- and the duplication is a real
 * limitation, stated rather than papered over: change the driver's copy and this one alone, and
 * the test keeps passing.  What it protects is the CONVENTIONS.  The driver's own numbers are
 * printed on the machine by getMaxPacketSize, which says all three bounds in its line.
 *
 * Build and run as part of build.sh; no kernel involved.
 */

#include <stdio.h>
#include <stdint.h>

#include "../src/mlx4_regs.h"

static int checks = 0;
static int failures = 0;

static void ck(const char *what, long got, long want)
{
    ++checks;
    if (got != want) {
        printf("  FAIL: %s: got %ld, want %ld\n", what, got, want);
        ++failures;
    }
}

static void note(const char *what, long value)
{
    printf("  %-56s %ld\n", what, value);
}

int main(void)
{
    const long hlen = (long)MLX4_ETH_HLEN;
    const long vlan = (long)MLX4_ETH_VLAN_LEN;
    const long fcs  = (long)MLX4_ETH_FCS_LEN;
    long wireDefault, stackFrame, familyMax;
    long portBound, rxBound, txBound, stackMax;

    /* The constants against the reference names they came from. */
    ck("MLX4_ETH_HLEN vs ETH_HLEN", hlen, 14);
    ck("MLX4_ETH_VLAN_LEN vs VLAN_HLEN", vlan, 4);
    ck("MLX4_ETH_FCS_LEN vs ETH_FCS_LEN", fcs, 4);
    ck("MLX4_NET_IP_ALIGN vs en.h:80", (long)MLX4_NET_IP_ALIGN, 2);

    /*
     * The identities.  A 1500-byte L3 MTU is 1522 in SET_PORT's terms (mlx4_port.c:1306-1308
     * clamps the field with eth_mtu_cap + these three) and 1518 in the family's terms
     * (IOEthernetController.h:45 -- no VLAN tag, because the family's frame is what the stack
     * hands over).  Both are the SAME MTU; the difference is the conventions, and this is where
     * that difference is written down as arithmetic rather than as prose.
     */
    wireDefault = 1500 + hlen + vlan + fcs;
    ck("a 1500 MTU in SET_PORT's terms (mlx4_port.c:1306-1308)", wireDefault, 1522);
    stackFrame = 1500 + hlen;
    ck("a 1500 MTU as a stack frame (no FCS)", stackFrame, 1514);
    familyMax = stackFrame + fcs;
    ck("...and in the family's terms (IOEthernetController.h:45)", familyMax, 1518);
    ck("the family's minimum (IOEthernetController.h:51) is above a bare header",
       64 > hlen, 1);

    /*
     * The bounds for the geometry the chain builds, computed the way
     * Cx3Mlx4::netStackFrameMax computes them.  Each one is a different piece that would drop a
     * larger frame, so the answer is the smallest.
     */
    portBound = wireDefault - (vlan + fcs);      /* the port was told 1522 bytes of WIRE */
    rxBound   = 2048 - fcs;                      /* the CQE's byte count includes the FCS */
    txBound   = 2048 - (long)MLX4_NET_IP_ALIGN;  /* outputPacket's own bound */
    stackMax  = portBound;
    if (rxBound < stackMax) stackMax = rxBound;
    if (txBound < stackMax) stackMax = txBound;

    printf("\nthe three bounds for the geometry the chain builds\n");
    note("port (wire 1522 minus VLAN and FCS)", portBound);
    note("receive ring (2048-byte buffers)", rxBound);
    note("transmit ring (kNetTxBufBytes 2048)", txBound);
    note("the smallest of the three", stackMax);
    ck("the port is the binding bound at this geometry", stackMax, portBound);
    ck("getMaxPacketSize answers that + the FCS", stackMax + fcs, 1518);
    ck("...which is the family's own maximum, arrived at from three real bounds",
       stackMax + fcs, 1518);

    /*
     * And the refusal that makes jumbo a chain change rather than a callback: a 9000 MTU would
     * need a stack frame far above every bound above, so setMaxPacketSize must refuse it until
     * the port is programmed (m3portup's mtu input) and r4rq is built with larger buffers.
     */
    ck("a 9000 MTU does not fit today's port programme", (9000 + hlen) > stackMax, 1);
    ck("nor today's receive buffers", (9000 + hlen) > rxBound, 1);

    /*
     * ...and the transmit buffer's own arithmetic, because Cx3Mlx4::netTxResourcesEnsure claims
     * a property in a comment that nothing was checking: "with MTU 1500 this is exactly
     * kNetTxBufBytes (2048): the default path is unchanged".  That is a claim about two numbers
     * on either side of a rounding step, so it is checked rather than asserted in prose.
     */
    {
        long need = 1500 + hlen + vlan + fcs + (long)MLX4_NET_IP_ALIGN;
        long size = 2048;                       /* kNetTxBufBytes, src/Cx3Mlx4.h */
        ck("the transmit buffer the default MTU needs", need, 1524);
        while (size < need) size <<= 1;
        ck("...which rounds to exactly kNetTxBufBytes", size, 2048);
        ck("...and a 9000 MTU would need a bigger one", (9000 + hlen + vlan + fcs +
                                                        (long)MLX4_NET_IP_ALIGN) > 2048, 1);
    }

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
