#!/bin/bash
#
# verify.sh -- one entry point for both sides of a reboot, and the only thing that
# decides whether a stage passed.
#
# Why it exists.  The measured shape of this project's cost: 82 install runs, 53 builds,
# and of the last ten reboots, eight were my own code being wrong.  The workflow that
# produced them was "build, install, reboot, then read prose and decide".  Two things
# were wrong with that last part.  The prose is written by the same person who wrote the
# bug, and this project has twice been misled by a number that looked like a measurement
# and was not -- a log line whose format specifiers were one short printed a misaligned
# row, and a "238 ms" that was a constant read as a duration.  So:
#
#   - decisions come from VALUES, read through `cx3ctl rawjson`, which returns the
#     kernel's own scalar output unchanged;
#   - every step prints PASS, FAIL, BLOCKED or SKIP;
#   - the index constants used here are looked up by name in src/cx3_abi.h at startup, so
#     an ABI change is caught here rather than silently reading the wrong slot;
#   - the ordered chain stops at the first failure and marks what follows BLOCKED, while
#     independent checks still run.  "Not reached", "failed" and "never asked" are three
#     different things and the summary says which is which.
#
# Usage:
#   tools/verify.sh stage     build, validate, install, then say to reboot
#   tools/verify.sh run       the chain, with the short soak            (default)
#   tools/verify.sh full      the chain, with the gate soak for M4/M5
#   tools/verify.sh safe      the read-only checks only: the whole device chain is SKIPPED
#                             (measured 2026-10-12: it prints "reset and the whole device
#                             chain SKIP --safe was requested", so it is not a way to get a
#                             brought-up device)
#   tools/verify.sh up        the chain, and then STOP with the stack still up: the MR, both
#                             CQs, the RX/TX QPs, the ingress QP and the port stay attached so
#                             cx3ctl peek/poke can address a live device.  Nothing is reset;
#                             put it back with 'cx3ctl reset 1 1'
#   tools/verify.sh coverage  which selectors have been verified, from docs/COVERAGE.md
#
#   CX3_STOP_AFTER=<step label>   (environment, any chain mode) ends the run BEFORE that step's
#                             work and leaves the device where the steps above brought it.  It is
#                             the bisect handle for the chain's intermittent hang: one attempt
#                             exposes only the steps up to a chosen label.  A label is the text
#                             after the banner, e.g. CX3_STOP_AFTER="R4 stages (memory only so far: nothing is reachable through a QP yet)".
#
#   CX3_SKIP=<comma-separated short names>   (environment, any chain mode) skips a step that is an
#                             EXPERIMENT rather than a prerequisite, by name.  Two names today:
#                             `txprobe` and `arm`.  Why: those two sit between the R4 stages and
#                             the interface step, neither has ever run on this machine, and a step
#                             that hangs takes the run with it -- including the interface step
#                             behind it, which is the one M4 actually needs.  They are not
#                             prerequisites for it (netup needs the R4 rings, not these), so they
#                             can be skipped honestly: the summary counts a SKIP and the banner
#                             says which switch did it.  Example: CX3_SKIP=txprobe,arm
#
# Exit: 0 = nothing FAILed (BLOCKED and SKIP do not fail it); 1 = at least one FAIL;
#       2 = usage or environment error.

set -uo pipefail

ROOT=/Users/you/Documents/Hackintosh-MacLocal
SRC="$ROOT/tools/cx3-mlx4"
ABI="$SRC/src/cx3_abi.h"
CTL="${CX3CTL:-/usr/local/bin/cx3ctl}"
LOG="${CX3VERIFY_LOG:-$SRC/outputs/verify-$(date +%Y%m%d-%H%M%S).log}"

#
# The verdict modes must leave a file, because the summary prints the path and a path is a
# claim that something is there.  Found the hard way: `$LOG` was written by `stage` only
# (the build output is redirected into it), so every `run` printed
# `log: .../verify-<stamp>.log` for a file that was never created -- the transcript of a
# run existed only in whatever terminal scrolled it.  That is the shape this project keeps
# paying for (docs/DEFECTS.md family F: something that looks like a record and is not), and
# it landed exactly where the project's evidence discipline depends on it.
#
# Tee rather than redirect so the operator still sees the chain go by.  `stage` is
# deliberately excluded: it truncates $LOG into itself for the build output, and teeing
# into the same file would have the two writers fighting.
#
MODE="${1:-run}"
if [ -n "${LOG:-}" ] && [ -z "${CX3_VERIFY_TEE:-}" ]; then
    case "$MODE" in
        run|full|safe|up|m5|"")
            export CX3_VERIFY_TEE=1
            exec > >(tee "$LOG") 2>&1
            ;;
    esac
fi
SOAK_SHORT="${CX3_SOAK_SHORT:-100}"
# The profile the verifier asks the port to build.  A request, not a device value, so
# naming it is correct -- and it matches what cx3ctl m2dprofile uses by default, which
# matters: passing zeros makes makeProfile refuse ("a profile needs at least QPs and
# MTT segments"), and the chain then fails with kIOReturnNotReady at bring-up.
PROFILE_QP="${CX3_PROFILE_QP:-1024}"
PROFILE_RDMARC="${CX3_PROFILE_RDMARC:-16}"
PROFILE_SRQ="${CX3_PROFILE_SRQ:-64}"
PROFILE_CQ="${CX3_PROFILE_CQ:-256}"
PROFILE_MCG="${CX3_PROFILE_MCG:-256}"
PROFILE_MPT="${CX3_PROFILE_MPT:-2048}"
PROFILE_MTT="${CX3_PROFILE_MTT:-4096}"
SOAK_FULL="${CX3_SOAK_FULL:-1000}"
# The M4 gate is a ping, so it needs a target and a rate.  The default target is this LAN's
# gateway: it is the one address that is certainly there, certainly answers, and does not
# depend on anything this driver has to get right beyond the frame itself.
SOAK_TARGET="${CX3_SOAK_TARGET:-192.0.2.1}"
SOAK_INTERVAL="${CX3_SOAK_INTERVAL:-0.2}"

pass=0; fail=0; blocked=0; skipped=0
ABI_ERRORS=0
# Where abi() records a name it could not find.
#
# Why a file and not just the counter: every lookup below runs inside $( ), i.e. in a
# subshell, so a counter incremented in there is thrown away when the assignment completes --
# which made the "abi indices resolve by name" step above a check that could not fail
# (docs/DEFECTS.md F4, the same shape as the vacuous self-check that defect records).  The
# value the lookup returns in that case is the sentinel 0xdeadbeef, which does propagate, but
# a caller that reads it three steps later sees a mysterious call failure instead of a name.
# So the miss is written down here and counted in resolve_abi, where the number is used.
ABI_MISSLOG="${CX3_ABI_MISSLOG:-$SRC/outputs/.verify-abi-misses.txt}"
# Set when the running kext is older than the tool: then nothing below can be judged,
# and saying FAIL would be claiming the device misbehaved when it was never asked.
STALE_KEXT=0
LAST_CALL_RC=0
LAST_CALL_SIZES=""
#
# Where a failed selector call leaves its details, and the reason is a defect this file had for a
# long time without anyone noticing: `runcall` is always called as `j=$(runcall ...)`, a COMMAND
# SUBSTITUTION, so every assignment it makes happens in a subshell and never reaches the caller.
# Every failure message in this file therefore printed the values from before the call --
# "the call failed: rc=0", which reads like the kernel answered 0, i.e. like success.  Found by
# running the new `m5` mode and reading its own output; the file is the fix, because a file is the
# one channel a subshell can still write.
# In the temp directory, named per uid, and neither part is cosmetic.  It used to be
# `$SRC/outputs/.verify-last-call.txt`, which any `sudo` run leaves owned by root -- so the
# next NON-root invocation (e.g. `coverage`) could not even truncate it, and the write that
# carries a failed call's details would fail silently.  That is precisely the failure this
# file exists to prevent (see above: a stale rc=0 that read like success), one level down.
# Per-uid also keeps two invocations from sharing one scratch file.
CALLINFO="${CX3_CALLINFO:-${TMPDIR:-/tmp}/cx3-verify-last-call.$(id -u).txt}"
: > "$CALLINFO"

# Resolved once here, before any judgement.

# ---------------------------------------------------------------------------
# Reading the ABI instead of duplicating it.
#
# Every index used below is looked up by NAME in src/cx3_abi.h.  Hard-coding them would
# create two copies of the same fact that drift apart -- the failure mode this project
# keeps finding in other forms.  abi() fails loudly rather than returning empty, because
# an empty index would silently read a different slot than intended.
# ---------------------------------------------------------------------------
abi() {
    local name="$1" v
    # Both declaration shapes have to work: the multi-line enums (name at the start of its
    # line, followed by a comment) and the seventy single-line `enum { a = 0, b = 1 };`
    # ones.  A sed pattern anchored to line start silently missed the second kind, which is
    # how kR4RqOutStatus and kSurveyOutEntries were first looked up as "not found".
    v=$(python3 -c 'import re,sys
t=open(sys.argv[1]).read()
m=re.search(r"\b"+re.escape(sys.argv[2])+r"\s*=\s*(0x[0-9a-fA-F]+|\d+)\s*[,}]", t)
print(m.group(1) if m else "")' "$ABI" "$name")
    if [ -z "$v" ]; then
        echo "ABI-ERROR: '$name' is not in $ABI" >&2
        [ -n "$ABI_MISSLOG" ] && printf '%s\n' "$name" >> "$ABI_MISSLOG"
        ABI_ERRORS=$((ABI_ERRORS + 1))
        printf '0xdeadbeef\n'
        return
    fi
    printf '%s\n' "$v"
}

# Resolve every index ONCE, at startup, before anything is judged.
#
# Without this the "ABI self-check" step was vacuous: abi() only complains when it is
# called, so at the top of the run nothing had been looked up and the check always passed.
# A check that cannot fail is not a check -- and the failure it was meant to catch is a
# silently wrong slot, which is exactly the kind of thing this project has been bitten by.
# macOS ships bash 3.2, so there are no associative arrays; each index gets a name.
resolve_abi() {
    # Start from an empty record; the count at the end is the real one (see ABI_MISSLOG).
    [ -n "$ABI_MISSLOG" ] && : >"$ABI_MISSLOG"
    SEL_PROBE=$(abi kCx3SelProbe)
    SEL_RESET=$(abi kCx3SelResetHca)
    SEL_QUERYFW=$(abi kCx3SelQueryFw)
    SEL_FWLOAD=$(abi kCx3SelFwLoad)
    SEL_M2DBRINGUP=$(abi kCx3SelM2dBringUp)
    SEL_M3EQUP=$(abi kCx3SelM3EqUp)
    SEL_M3CLRINT=$(abi kCx3SelM3ClrInt)
    SEL_M3IRQUP=$(abi kCx3SelM3IrqUp)
    SEL_M3IRQTEST=$(abi kCx3SelM3IrqTest)
    SEL_M3IRQSTATUS=$(abi kCx3SelM3IrqStatus)
    SEL_M3IRQDOWN=$(abi kCx3SelM3IrqDown)
    SEL_R4MR=$(abi kCx3SelR4Mr)
    SEL_R4CQ=$(abi kCx3SelR4Cq)
    SEL_R4RQ=$(abi kCx3SelR4Rq)
    SEL_R4QP=$(abi kCx3SelR4Qp)
    SEL_R4INGRESS=$(abi kCx3SelR4Ingress)
    SEL_SURVEY=$(abi kCx3SelSurvey)
    O_PROBEWIN=$(abi kProbeOutWindow)
    O_RSTSTAT=$(abi kRstOutStatus)
    O_FWSTAT=$(abi kFwOutFwStatus)
    O_FWPAGES=$(abi kFwOutFwPages)
    O_MAPFA=$(abi kFwLoadOutMapFaStatus)
    O_RUNFW=$(abi kFwLoadOutRunFwStatus)
    O_MODSTAT=$(abi kFwLoadOutModStatus)
    O_BUSTAT=$(abi kBringUpOutStatus)
    O_INITHCA=$(abi kBringUpOutInitHcaStat)
    O_TABLESDONE=$(abi kBringUpOutTablesDone)
    O_TABLESTOTAL=$(abi kBringUpOutTablesTotal)
    O_EQUPSTAT=$(abi kEqUpOutStatus)
    O_MAPEQS=$(abi kEqUpOutMapEqStatus)
    O_EQN=$(abi kEqUpOutEqn)
    O_CLRSTAT=$(abi kClrIntOutStatus)
    O_IRQUPSTAT=$(abi kIrqUpOutStatus)
    O_IRQUPEN=$(abi kIrqUpOutEnabled)
    O_IRQTESTSTAT=$(abi kIrqTestOutStatus)
    O_IRQMATCHED=$(abi kIrqStatusOutMatched)
    O_IRQMATCHSTAT=$(abi kIrqStatusOutMatchedStatus)
    O_IRQCOUNT=$(abi kIrqStatusOutIrqCount)
    # The port's asynchronous-event reading (docs/EQ-ORDERING.md section 5): the last EQE
    # type consumed is what says whether the event m3portup's INIT_PORT produces arrived
    # at a queue whose delivery was already running.
    O_IRQLASTTYPE=$(abi kIrqStatusOutLastType)
    O_IRQEQECOUNT=$(abi kIrqStatusOutEqeCount)
    O_SURVEYN=$(abi kSurveyOutEntries)
    O_R4MRSTAT=$(abi kR4MrOutStatus)
    O_R4CQSTAT=$(abi kR4CqOutStatus)
    O_R4RQSTAT=$(abi kR4RqOutStatus)
    # R4 stage 4: the three layers, as scalars.  The verifier reads values out of the scalar
    # path on purpose (a harness that decodes its own ABI structures is one more place to be
    # wrong -- the same reason `fault`'s poison bit is read through `probe`).
    O_R4QPSTAT=$(abi kR4QpOutStatus)
    O_R4QPQPN=$(abi kR4QpOutQpn)
    O_R4QPSTEPS=$(abi kR4QpOutStepsAccepted)
    O_R4QPSTATES=$(abi kR4QpOutStatesConfirmed)
    O_R4QPQRYOK=$(abi kR4QpOutQueryOk)
    O_R4QPQRYST=$(abi kR4QpOutQueryState)
    O_R4QPQPNOK=$(abi kR4QpOutQpnConfirmed)
    O_R4QPFIELDS=$(abi kR4QpOutFieldsMatch)
    O_R4QPMTT=$(abi kR4QpOutMttMatches)
    O_R4QPPROD=$(abi kR4QpOutProducerValue)
    O_R4QPPRODBACK=$(abi kR4QpOutProducerReadback)
    O_R4QPPRODOK=$(abi kR4QpOutProducerMatches)
    O_R4INST=$(abi kR4IngressOutStatus)
    O_R4INMACSTAT=$(abi kR4IngressOutMacStatus)
    O_R4INMACSLOT=$(abi kR4IngressOutMacSlot)
    O_R4INMACRBOK=$(abi kR4IngressOutMacReadbackOk)
    O_R4INMACRB=$(abi kR4IngressOutMacReadbackMatch)
    O_R4INQPN=$(abi kR4IngressOutQpn)
    O_R4INSTEPS=$(abi kR4IngressOutStepsAccepted)
    O_R4INSTATES=$(abi kR4IngressOutStatesConfirmed)
    O_R4INQRYOK=$(abi kR4IngressOutQueryOk)
    O_R4INQRYST=$(abi kR4IngressOutQueryState)
    O_R4INDEF=$(abi kR4IngressOutRssDefaultMatches)
    O_R4INBASE=$(abi kR4IngressOutRssBaseMatches)
    O_R4INRXQPN=$(abi kR4IngressOutRxQpn)
    O_R4INQDEFAULT=$(abi kR4IngressOutQueryDefaultQpn)
    O_R4INQBASE=$(abi kR4IngressOutQueryBaseQpn)
    SEL_R4TX=$(abi kCx3SelR4Tx)
    O_R4TXSTAT=$(abi kR4TxOutStatus)
    O_R4TXCQSTAT=$(abi kR4TxOutCqFwStatus)
    O_R4TXCQREADY=$(abi kR4TxOutCqReady)
    O_R4TXQPN=$(abi kR4TxOutQpn)
    O_R4TXSAFE=$(abi kR4TxOutNumberSafe)
    # The two layer-1 counts are published in ONE scalar (steps low, states high) because the
    # kernel's scalar block is 16 wide and r4tx fills all 16 -- the width is the kernel's, not
    # ours, which is docs/DEFECTS.md F13 and the reason 0.43.9's r4tx could not be called at
    # all.  The CQN and the top-reserved LINE stopped being scalars in the same change: they
    # are context rather than verdicts and are printed by `cx3ctl r4tx` out of the structure
    # (the verdict on them, kR4TxOutNumberSafe, is still a scalar and still decides here).
    # The shift comes from the header by name, so a wrong half cannot be read silently.
    O_R4TXSTEPSTATES=$(abi kR4TxOutStepsStates)
    O_R4TXSTATESSHIFT=$(abi kR4TxOutStatesShift)
    O_R4TXQRYOK=$(abi kR4TxOutQueryOk)
    O_R4TXQRYST=$(abi kR4TxOutQueryState)
    O_R4TXQPNOK=$(abi kR4TxOutQpnConfirmed)
    O_R4TXFIELDS=$(abi kR4TxOutFieldsMatch)
    O_R4TXMTT=$(abi kR4TxOutMttMatches)
    O_R4TXSQSTRIDE=$(abi kR4TxOutSqSizeStride)
    O_R4TXSTAMP=$(abi kR4TxOutSqStampMatches)
    O_R4TXFEUPSENT=$(abi kR4TxOutFeupSent)
    O_R4TXFEUPQRY=$(abi kR4TxOutFeupQuery)
    O_R4TXFEUPSURV=$(abi kR4TxOutFeupSurvived)
    SEL_R4RAW=$(abi kCx3SelR4Raw)
    O_R4RAWSTAT=$(abi kR4RawOutStatus)
    O_R4RAWTXREF=$(abi kR4RawOutTxRefused)
    O_R4RAWTXWQE=$(abi kR4RawOutTxWqeOk)
    O_R4RAWTXPOSTS=$(abi kR4RawOutTxPosts)
    O_R4RAWTXCQES=$(abi kR4RawOutTxCompletions)
    O_R4RAWTXCQEOK=$(abi kR4RawOutTxCqeOk)
    O_R4RAWTXERR=$(abi kR4RawOutTxErrors)
    O_R4RAWTXWRAP=$(abi kR4RawOutTxWrapOk)
    O_R4RAWTXLAPS=$(abi kR4RawOutTxOwnerLapsOk)
    O_R4RAWRXFRAMES=$(abi kR4RawOutRxFrames)
    O_R4RAWRXQPNOK=$(abi kR4RawOutRxQpnOk)
    O_R4RAWRXQPN=$(abi kR4RawOutRxQpn)
    O_R4RAWRXMIN=$(abi kR4RawOutRxMinByteCnt)
    O_R4RAWRXCONTENT=$(abi kR4RawOutRxContentOk)
    O_R4RAWRXLAPS=$(abi kR4RawOutRxLaps)
    O_R4RAWRXRECYCLE=$(abi kR4RawOutRxRecycleOk)
    SEL_M2DPROFILE=$(abi kCx3SelM2dProfile)
    # The three discriminators of docs/M2B-NOTES.md section 52.  r4raw's sixteen scalars are
    # full, which is why these are their own block rather than more fields of that one.
    SEL_TXPROBE=$(abi kCx3SelTxProbe)
    O_TPSTAT=$(abi kTxProbeOutStatus)
    O_TPREFUSED=$(abi kTxProbeOutRefused)
    O_TPNOPPOSTS=$(abi kTxProbeOutNopPosts)
    O_TPNOPWQEOK=$(abi kTxProbeOutNopWqeOk)
    O_TPNOPCOMPS=$(abi kTxProbeOutNopComps)
    O_TPNOPOPCODE=$(abi kTxProbeOutNopOpcode)
    O_TPNOPSTATUS=$(abi kTxProbeOutNopStatus)
    O_TPSENDCOMPS=$(abi kTxProbeOutSendComps)
    O_TPSENDOPCODE=$(abi kTxProbeOutSendOpcode)
    O_TPARMRAN=$(abi kTxProbeOutArmRan)
    O_TPARMFOUND=$(abi kTxProbeOutArmEqeFound)
    O_TPARMTYPE=$(abi kTxProbeOutArmEqeType)
    O_TPARMCQN=$(abi kTxProbeOutArmEqeCqn)
    O_TPARMOK=$(abi kTxProbeOutArmEqeOk)
    O_TPRECVAR=$(abi kTxProbeOutRecVariants)
    O_TPRECWIN=$(abi kTxProbeOutRecWinner)
    # The probe's sentinels are #defines rather than enumerators, and abi() reads the header by
    # name = value.  They are written here as literals WITH their source, which is the one place
    # in this script a constant is not resolved from the header -- and the reason is that a
    # #define cannot be looked up by the same route.  If one of them ever moves, this is the
    # line to move with it; the values are asserted where they are defined.
    N_TPNOTRUN=4294967295   # CX3_TXPROBE_NOT_RUN  0xffffffff
    N_TPRAN=0               # CX3_TXPROBE_RAN      0
    N_TPCQNONE=255          # CX3_TXPROBE_OPCODE_NONE 0xff
    N_TPCOMP=0              # MLX4_EVENT_TYPE_COMP 0x00 (device.h:293)
    SEL_M3PORTUP=$(abi kCx3SelM3PortUp)
    O_PUPLINKAFTER=$(abi kPortUpOutLinkAfter)
    O_PUPSTEP=$(abi kPortUpOutStep)
    O_PUPQPNCALC=$(abi kPortUpOutQpnCalcFwStatus)
    # The interface (stage 10a).  Its refusal codes are #defines, so they are written here as
    # literals with their source, the same way the txprobe sentinels are (see the note above).
    SEL_NETUP=$(abi kCx3SelNetUp)
    O_NETREFUSED=$(abi kNetUpOutRefused)
    O_NETUNIT=$(abi kNetUpOutUnit)
    O_NETMACOK=$(abi kNetUpOutMacOk)
    O_NETMACBITS=$(abi kNetUpOutMacBits)
    O_NETLINKUP=$(abi kNetUpOutLinkUp)
    SEL_NETSTAT=$(abi kCx3SelNetStat)
    SEL_NETLINK=$(abi kCx3SelNetLink)
    SEL_NETFILT=$(abi kCx3SelNetFilt)
    # The mode inputs are #defines rather than enumerators, so abi() cannot look them up -- they
    # are written here as literals WITH their source, the same way the txprobe sentinels are.
    # If one of them ever moves, this is the block to move with it.
    N_LINKPUBLISH=1          # CX3_NETLINK_PUBLISH     1
    N_LINKREQUERY=2          # CX3_NETLINK_REQUERY     2
    N_FILTMCASTON=1          # CX3_NETFILT_MCAST_ON    1
    N_FILTPROMISCON=3        # CX3_NETFILT_PROMISC_ON  3
    N_FILTPROMISCOFF=4       # CX3_NETFILT_PROMISC_OFF 4
    N_OFFREPORT=0            # CX3_NETOFF_REPORT       0
    N_OFFRXON=1              # CX3_NETOFF_RX_ON        1
    N_OFFRXOFF=2             # CX3_NETOFF_RX_OFF       2
    SEL_NETOFF=$(abi kCx3SelNetOff)
    SEL_NETARM=$(abi kCx3SelNetArm)
    O_ARMREFUSED=$(abi kNetArmOutRefused)
    O_ARMDRAINED=$(abi kNetArmOutDrained)
    O_ARMCQEMPTY=$(abi kNetArmOutCqEmpty)
    O_ARMEQEFOUND=$(abi kNetArmOutEqeFound)
    O_ARMEQTYPE=$(abi kNetArmOutEqeType)
    O_ARMEQCQN=$(abi kNetArmOutEqeCqn)
    O_ARMEQEOK=$(abi kNetArmOutEqeOk)
    N_ARMRAN=0               # CX3_NETARM_RAN 0
    O_NETSTATUNIT=$(abi kNetStatOutUnit)
    O_LINKMEDIACOUNT=$(abi kNetLinkOutMediaCount)
    O_LINKSELECTED=$(abi kNetLinkOutSelected)
    O_LINKDOWN=$(abi kNetLinkOutLinkUp)
    O_FILTMCASTON=$(abi kNetFiltOutMcastOn)
    O_FILTPROGRAMS=$(abi kNetFiltOutPrograms)
    O_FILTLASTFW=$(abi kNetFiltOutLastFw)
    O_FILTPROMISC=$(abi kNetFiltOutPromiscOn)
    O_OFFRXON=$(abi kNetOffOutRxOn)
    O_OFFRXGOOD=$(abi kNetOffOutRxGood)
    O_OFFTXON=$(abi kNetOffOutTxOn)
    O_NETOFFMODE=$(abi kNetOffOutMode)
    O_NETSTAT=$(abi kNetStatOutStatus)
    N_NETRAN=0              # CX3_NETUP_RAN       0
    N_NETNODEV=1            # CX3_NETUP_NO_DEVICE 1
    N_NETNOIFACE=2          # CX3_NETUP_NO_IFACE  2
    # The step numbers themselves, resolved rather than written down twice: the arm below has
    # to know which step the qpn_calc scalar belongs to (docs/DEFECTS.md F11).
    N_PUPSTEPS=$(abi kPortUpSteps)
    N_PUPQPNCALCSTEP=$(abi kPortUpStepQpnCalc)
    SEL_QUERYPORT=$(abi kCx3SelM3QueryPort)
    SEL_QUERYHCA=$(abi kCx3SelM3QueryHca)
    SEL_QUERYDEVCAP=$(abi kCx3SelQueryDevCap)
    O_QPSTAT=$(abi kQueryPortOutStatus)
    O_QHCASTAT=$(abi kQueryHcaOutStatus)
    O_DCSTAT=$(abi kDevCapOutStatus)
    O_QPPORT=$(abi kQueryPortInPort)
    O_DCIN=$(abi kDevCapInCount)
    O_QPCNT=$(abi kQueryPortInCount)
    # The command-timeout state (docs/LOCK-IMPL.md).  Resolved here like every other index,
    # so a rename in src/cx3_abi.h stops this script at the startup check instead of
    # silently reading the wrong slot.
    SEL_FAULT=$(abi kCx3SelFault)
    O_PROBEWEDGED=$(abi kProbeOutCmdWedged)
    O_RSTWEDGED=$(abi kRstOutCmdWedged)
    O_FAULTSTATUS=$(abi kFaultOutStatus)
    O_FAULTWEDGED=$(abi kFaultOutWedged)
    O_FAULTHOLD=$(abi kFaultOutHeldMs)

    # The count that survived the subshells, read back from the record abi() writes.
    if [ -n "$ABI_MISSLOG" ] && [ -f "$ABI_MISSLOG" ]; then
        ABI_ERRORS=$(wc -l <"$ABI_MISSLOG" | tr -d ' ')
    fi
}

# One value out of a rawjson line: jget '<json>' <name-of-index>
# ---------------------------------------------------------------------------
# The kernel side of a run, captured by the run.
#
# Section 50 is why this exists: this driver's own log does NOT persist for a filter that
# runs after boot -- `log show` finds nothing for it while `log stream` sees everything -- so
# every judgement that reads the driver's account of itself needs a live capture.  Those lines
# were being read from a second terminal by hand, and a run whose kernel log was never captured
# looks exactly like a run in which nothing was logged, which is the mistake section 50 records.
# A run that carries its own kernel log is what makes "one command, all the evidence" true.
#
# /usr/bin/log by absolute path on purpose: in zsh `log` is a builtin that swallows arguments,
# which has cost this project a round already.
#
# Started before anything touches the device and stopped by the EXIT trap, so an early return
# from any of do_run's many paths still leaves a named file behind.
# ---------------------------------------------------------------------------
klog_start() {
    # A run can be asked to go WITHOUT the capture, and the reason is a measurement rather than a
    # preference: the chain's intermittent hang correlates with this stream.  The same seven R4
    # selectors issued one at a time by hand -- no stream, no tee, nothing else concurrent --
    # completed 7/7, while the same steps inside this script hung 3 times in 4, once before the R4
    # section had even started.  A correlation of that shape is cheap to test and expensive to
    # ignore, so: CX3_NO_KLOG=1 runs the chain with no log stream at all.  If it completes, this
    # stream is part of the mechanism; if it hangs anyway, it is not, and that is worth knowing
    # before another hypothesis.
    if [ -n "${CX3_NO_KLOG:-}" ]; then
        KLOG=""
        KLOG_PID=""
        echo "  kernel log capture DISABLED for this run (CX3_NO_KLOG=$CX3_NO_KLOG)"
        return 0
    fi
    KLOG="$ROOT/outputs/cx3-klog-$(date +%Y%m%d-%H%M%S).log"
    : > "$KLOG"
    /usr/bin/log stream --level debug --style compact \
        --predicate 'eventMessage CONTAINS "Cx3Mlx4"' > "$KLOG" 2>/dev/null &
    KLOG_PID=$!
    # The stream needs a moment to attach; a run that starts issuing commands first would lose
    # the first lines, and the first lines are the bring-up.
    sleep 1
}
klog_stop() {
    if [ -n "${KLOG_PID:-}" ]; then kill "$KLOG_PID" 2>/dev/null; fi
    if [ -n "${KLOG:-}" ] && [ -f "$KLOG" ]; then
        echo "  kernel log: $KLOG ($(wc -l < "$KLOG" | tr -d ' ') line(s))"
    fi
}

jget() {
    python3 -c 'import json,sys
try:
    d=json.loads(sys.argv[1])
    o=d.get("out")
    if o is None: print("NO-OUT")
    else:
        i=int(sys.argv[2],0)
        print(int(o[i],0) if i<len(o) else "NO-INDEX")
except Exception:
    print("PARSE-ERROR")' "$1" "$2" 2>/dev/null || echo "PARSE-ERROR"
}

ok()   { printf '  %-52s PASS\n' "$1"; pass=$((pass + 1)); }
bad()  { printf '  %-52s FAIL  %s\n' "$1" "$2"; fail=$((fail + 1)); }
blk()  { printf '  %-52s BLOCKED  %s\n' "$1" "$2"; blocked=$((blocked + 1)); }
skip() { printf '  %-52s SKIP  %s\n' "$1" "$2"; skipped=$((skipped + 1)); }
#
# step -- the step banner, and the one place an early stop is enforced.
#
# CX3_STOP_AFTER=<label> ends the run BEFORE the labelled step's work, which is the boundary a
# bisect needs.  It lives here rather than beside each step on purpose: a per-step check would be
# twenty edits that a later change could half-remove, and this project has already been bitten by
# a check that was present in most places (docs/DEFECTS.md C7's shape).  One gate, in the one
# function every step already goes through.
#
# Why it exists: the chain has an intermittent hang -- three of the last four full runs stopped
# the machine, with the driver's own log ending at "user client closed" after m3irqdown -- and a
# full run per attempt is the most expensive way to find where.  With this, one attempt exposes
# only the steps up to a chosen label, and the next attempt extends it by one.
#
# exit, not return: the banner is printed by the step that is ABOUT to run, so returning here
# would let the caller continue into the very work being excluded.  The run's own log and the
# kernel capture are both files that survive the exit; only the summary lines at the end are
# skipped, and the banner says so.
#
step() {
    if [ -n "${CX3_STOP_AFTER:-}" ] && [ "${CX3_STOP_AFTER}" = "$1" ]; then
        printf '\n[%s] %s\n' "$(ts)" "--- STOPPING BEFORE: $* (CX3_STOP_AFTER=$CX3_STOP_AFTER) ---"
        echo "  Deliberate early stop.  Everything above this line ran; this step's work did not."
        echo "  The device is left in the state the steps above brought it to, so the next"
        echo "  probe/selector can be issued by hand to test exactly this boundary."
        exit 0
    fi
    printf '\n[%s] %s\n' "$(ts)" "--- $* ---"
}

#
# Wall-clock stamp, `[HH:MM:SS]`, on every step banner and on both ends of a run.
#
# Why: a hard death leaves this log flushed up to the moment it stopped.  That is measured,
# not assumed -- the 11:49:58 run's log ends exactly at the step the machine died in
# (docs/HANG-AUDIT.md 10.1), which is what made that run's forensics possible at all.
# Without a stamp the last line says WHERE the machine died and not WHEN, and "when" is the
# reading that separates "died during this step" from "died minutes later with the card
# idle" -- the two shapes section 10 has to tell apart before it can name a mechanism.
# Until now that time came from another session's 5-second heartbeat file, i.e. this
# project's own evidence depended on someone else's tool.
#
ts() { date '+%H:%M:%S'; }

#
# Is this step named in CX3_SKIP?  (Names are short and stable: `txprobe`, `arm`.)  Read through
# a function so the two steps that use it cannot drift apart in how they parse the list -- the
# comma-wrapped match is what makes "arm" not match "farm" and an empty value match nothing.
#
# Whitespace is stripped once, here, rather than at each use: `CX3_SKIP="txprobe, arm"` then reads
# as two names, and a stray space does not silently skip nothing -- which is the shape this
# project keeps having to fix (a value that looks handled and is not).
CX3_SKIP_LIST=",$(printf '%s' "${CX3_SKIP:-}" | tr -d '[:space:]'),"

skip_step() {
    case "$CX3_SKIP_LIST" in
        *",$1,"*) return 0 ;;
        *)        return 1 ;;
    esac
}

raw() { "$CTL" rawjson "$@" 2>&1 | tail -1; }

# Wall-clock milliseconds.  Two of the assertions below are about TIME rather than about a
# value -- "a poisoned command fails at once" and "probe answers while another selector holds
# the lock" -- and the failures they guard against are 22.7 s and 6 min 07.6 s respectively
# (docs/LOCK-DESIGN.md section 1.4), so a whole-second clock (date +%s) is not enough
# resolution to tell the fixed behaviour from the broken one.
# The details of the last failed selector call, or a sentence saying there were none.  Read from
# the file rather than from a variable for the reason written where CALLINFO is set up.
callinfo() {
    if [ -s "$CALLINFO" ]; then cat "$CALLINFO"; else printf 'no failed selector call recorded'; fi
}

ms_now() { python3 -c 'import time; print(int(time.time()*1000))'; }

# The unsigned 32-bit value of the "call" field of a rawjson line: 0 = the kernel answered,
# anything else = the call itself was refused.  Unsigned because the tool prints kern_return_t
# as a signed int, so kIOReturnBusy appears as -536870187 and not as 0xe00002d5.
call_rc() {
    python3 -c 'import json,sys
try: print(int(json.loads(sys.argv[1]).get("call",-1)) & 0xffffffff)
except Exception: print(-1)' "$1" 2>/dev/null || echo -1
}

# kIOReturnBusy, byte for byte: 0xe00002d5.  Verified by compiling it against the SDK's
# IOKit/IOReturn.h rather than from memory, and pinned to decimal here because that is what
# the tool prints.
IORETURN_BUSY=3758097109

# kIOReturnNotReady, byte for byte: 0xe00002d8 (iokit_common_err(0x2d8), read out of the SDK's
# IOReturn.h rather than from memory).  r4ingress returns it when a stage it depends on has not
# run -- so "the prerequisite is missing" is reported as BLOCKED, not as a FAIL: the third state
# this chain keeps for exactly this reason.
IORETURN_NOTREADY=3758097112

# ---------------------------------------------------------------------------
# The dispatch table checks three numbers for EXACT equality: scalar input count,
# scalar output count, and structure output size.  Anything else is
# kIOReturnBadArgument -- and a failed call leaves the output buffer ZEROED, where 0 is
# this project's value for "ok".  So a mis-sized call does not look like a failure; it
# looks like a device reporting success.
#
# That is not hypothetical.  The first version of this script hard-coded the sizes from
# memory, got four of them wrong, printed a false PASS for queryhca (the failed call left
# status 0, which the check read as "ok"), and then attributed a fifth failure to a stale
# kext -- a plausible cause that was wrong and would have sent the reader to reboot for
# nothing.
#
# So the sizes now come from cx3ctl sizes, which computes them in the compiler from the
# same types the kext builds its dispatch table from, and runcall pads the arguments to the
# declared counts.  A call site cannot get this wrong any more, which is the point: the
# alternative is remembering three numbers per selector, thirty-three times.
# ---------------------------------------------------------------------------
SIZES=""
load_sizes() { [ -n "$SIZES" ] || SIZES=$("$CTL" sizes 2>/dev/null); }
sz_field() { printf '%s\n' "$SIZES" | awk -v s="$1" -v f="$2" '$1==s {print $f}'; }

# runcall <selector> [input values...] -> prints the JSON, and fails loudly if the call
# itself did not succeed.  "The kernel refused the call" and "the device answered that it
# could not do it" are different, and only one of them is about the device.
runcall() {
    local sel="$1"; shift
    local nin nout ssz vals="" i=0 v j rc
    load_sizes
    nin=$(sz_field "$sel" 2); nout=$(sz_field "$sel" 3); ssz=$(sz_field "$sel" 4)
    for v in "$@"; do vals="$vals${vals:+,}$v"; i=$((i+1)); done
    while [ "$i" -lt "${nin:-0}" ]; do vals="$vals${vals:+,}0"; i=$((i+1)); done
    j=$(raw "$sel" in "$vals" out "$nout" struct "$ssz")
    rc=$(printf '%s' "$j" | python3 -c 'import json,sys
try: print(int(json.loads(sys.argv[1]).get("call",-1)))
except Exception: print(-1)' "$j" 2>/dev/null || echo -1)
    if [ "$rc" != "0" ]; then
        LAST_CALL_RC=$rc
        LAST_CALL_SIZES="in=$nin out=$nout struct=$ssz"
        # ...and to the file, which is the only channel that survives the command substitution
        # this function is always called from.  The messages below read it with callinfo().
        printf 'rc=%s (kern_return 0x%x), selector %s, in=%s out=%s struct=%s' \
               "$rc" "$(( rc & 0xffffffff ))" "$sel" "$nin" "$nout" "$ssz" > "$CALLINFO"
        return 1
    fi
    LAST_CALL_RC=0
    : > "$CALLINFO"
    printf '%s\n' "$j"
    return 0
}

# Is the selector implemented by the KEXT THAT IS RUNNING?
#
# This is a distinction worth its own check.  "The device did not do it" and "the driver
# currently loaded does not have this call at all" look identical if you only read the
# output values -- both give zeros -- and they call for opposite responses.  The second is
# usually just "a newer kext is staged and this machine has not rebooted yet", and if the
# chain cannot tell them apart it reports a cascade of failures that mean nothing.
sel_available() {
    local rc
    rc=$(python3 -c 'import json,sys
try: print(int(json.loads(sys.argv[1]).get("call",-1)))
except Exception: print(-1)' "$1" 2>/dev/null || echo -1)
    [ "$rc" = "0" ]
}

need_root() {
    [ "$(id -u)" -eq 0 ] || { echo "verify.sh: run with sudo (the selectors are root-only)"; exit 2; }
}

# ---------------------------------------------------------------------------
# stage -- the pre-reboot half, so "build then install" is one action.
#
# Kept separate from the chain because it happens before the reboot and the chain after
# it, and because the failure it prevents is real and has happened: a build that
# succeeded and was never installed, followed by confusion about why nothing changed.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# m5 -- the M5 switches, one at a time, on a live interface.
#
# Separate from `up` on purpose: the chain's job is to get the device and the interface to a
# known state, and this mode's job is to exercise the switches that are OFF by default *after*
# that, so that M4's verdict is never entangled with a feature it was not designed against.
#
# Every step is judged from values (never from prose), and every step says which of the three
# things it means: the switch took effect, it did not, or the run could not tell.  The one step
# that needs traffic is the receive-checksum one, and it asks for that traffic itself with a
# single ping rather than assuming someone else's frames are arriving.
# ---------------------------------------------------------------------------
do_m5() {
    # The selector numbers and every output index come from resolve_abi(), which the chain's
    # mode calls for itself -- this mode has to do the same, and did not: the first run of it
    # died on an unbound variable, which is what running a new mode once is for.
    resolve_abi

    step "M5: the media table and the link reading"

    if ! j=$(runcall "$SEL_NETSTAT"); then
        skip "the M5 switches" "the running kext has no netstat selector ($(callinfo)); reboot with the staged kext and re-run"
        return 0
    fi
    unit=$(jget "$j" "$O_NETSTATUNIT")
    if [ -z "$unit" ] || [ "$unit" = "" ] || [ "$unit" -gt 99 ] 2>/dev/null; then
        skip "the M5 switches" "netstat did not report an interface unit ($unit), so there is no interface to talk to. Run 'tools/verify.sh up' first -- this mode does not bring one up"
        return 0
    fi
    ifc="en$unit"
    if ! ifconfig "$ifc" >/dev/null 2>&1; then
        skip "the M5 switches" "netstat says the interface is $ifc but the system does not have it. Run 'tools/verify.sh up' first"
        return 0
    fi
    echo "  the interface is $ifc (from netstat's own unit number, not from parsing a name)"
    if ! ifconfig "$ifc" 2>/dev/null | grep -q "status: active"; then
        blk "the M5 switches" "$ifc exists but its link is not active, so the link-dependent steps below could not be judged. Run 'tools/verify.sh up' first"
        return 0
    fi
    ok "$ifc is up with an active link"

    step "M5-1: publish the media table and re-read the link"
    if ! j=$(runcall "$SEL_NETLINK" "$N_LINKPUBLISH"); then
        bad "netlink publish" "the call failed: $(callinfo)"
    else
        nm=$(jget "$j" "$O_LINKMEDIACOUNT")
        ns=$(jget "$j" "$O_LINKSELECTED")
        if [ "$nm" = "1" ] && [ "$ns" = "1" ]; then
            ok "one medium is published (autoselect) and it is selected, so getSelectedMedium() has an object to answer with and 'ifconfig $ifc media' has something to say"
        else
            bad "netlink publish" "media count $nm, selected $ns -- expected 1 and 1"
        fi
    fi
    if ! j=$(runcall "$SEL_NETLINK" "$N_LINKREQUERY"); then
        bad "netlink requery" "the call failed: $(callinfo)"
    else
        if [ "$(jget "$j" "$O_LINKDOWN")" = "1" ]; then
            ok "QUERY_PORT re-read the link as UP; the reading agrees with the interface's own status line"
        else
            blk "netlink requery" "QUERY_PORT re-read the link as DOWN while the interface says active. One of the two is stale; the kernel log's 'netlink: QUERY_PORT re-read' line has the fields"
        fi
    fi

    step "M5-8: program the multicast table"
    if ! j=$(runcall "$SEL_NETFILT" "$N_FILTMCASTON"); then
        bad "netfilt mcaston" "the call failed: $(callinfo)"
    else
        fon=$(jget "$j" "$O_FILTMCASTON")
        fpr=$(jget "$j" "$O_FILTPROGRAMS")
        ffw=$(jget "$j" "$O_FILTLASTFW")
        if [ "$fon" != "1" ]; then
            bad "netfilt mcaston" "the switch did not take (mcastOn=$fon)"
        elif [ "$fpr" = "0" ] || [ "$ffw" != "0" ]; then
            bad "the multicast table" "the switch is on but the table was written $fpr time(s) with last fw status 0x$ffw -- the sequence stops at the first failure and does not enable a half-written table, so the kernel log names the step"
        else
            ok "the port's multicast table is programmed and enabled ($fpr write(s), last fw status 0). IPv6 needs this to receive a single frame"
        fi
        # ...and the one thing that proves it end to end, if the stack has an IPv6 address.
        ip6=$(ifconfig "$ifc" 2>/dev/null | awk '/inet6/ { print $2; exit }')
        if [ -n "$ip6" ]; then
            ok "the interface has an IPv6 address ($ip6), which it could not have got without receiving multicast"
        else
            blk "the IPv6 address" "no inet6 address on $ifc yet. The table is programmed but nothing has been received through it -- either IPv6 is off for this interface or no frame has arrived"
        fi
    fi

    step "M5-8: promiscuous mode, and back"
    if ! j=$(runcall "$SEL_NETFILT" "$N_FILTPROMISCON"); then
        bad "netfilt promiscon" "the call failed: $(callinfo)"
    else
        if [ "$(jget "$j" "$O_FILTPROMISC")" = "1" ]; then
            ok "promiscuous mode is ON (the port's qpn_calc bit); 'tcpdump -i $ifc' would now see other stations' frames"
        else
            blk "netfilt promiscon" "the switch reports off after being asked for on -- read the kernel log's 'netfilt (promiscon)' lines for which command refused"
        fi
    fi
    # ...and off again, because a promiscuous port left on is a state change nobody asked for to
    # keep, and this is also the check that the undo works.
    if ! j=$(runcall "$SEL_NETFILT" "$N_FILTPROMISCOFF"); then
        bad "netfilt promiscoff" "the call failed: $(callinfo)"
    elif [ "$(jget "$j" "$O_FILTPROMISC")" = "0" ]; then
        ok "promiscuous mode is OFF again -- the same command with the bit cleared, so the undo is a command and not a hope"
    else
        bad "netfilt promiscoff" "the switch still reports on"
    fi

    step "M5-3: receive checksum marking, with one ping to make traffic"
    if ! j=$(runcall "$SEL_NETOFF" "$N_OFFRXON"); then
        bad "netoff rxon" "the call failed: $(callinfo)"
    else
        if [ "$(jget "$j" "$O_OFFRXON")" != "1" ]; then
            bad "netoff rxon" "the switch did not take"
        else
            before=$(jget "$j" "$O_OFFRXGOOD")
            ping -c 2 -t 3 "$SOAK_TARGET" >/dev/null 2>&1
            sleep 1
            if ! j=$(runcall "$SEL_NETOFF" "$N_OFFREPORT"); then
                bad "netoff report" "the call failed: $(callinfo)"
            else
                after=$(jget "$j" "$O_OFFRXGOOD")
                if [ "$after" -gt "$before" ] 2>/dev/null; then
                    ok "the hardware verified $((after - before)) more frame(s) while the switch was on ($after in total), and they were handed to the stack marked as already checked"
                elif [ "$after" = "$before" ]; then
                    blk "the receive checksum switch" "the switch is on but no frame has carried a verdict since ($after in total). Either nothing arrived, or the CQE never sets IPOK for this traffic -- the 'net rx:' lines say which"
                else
                    bad "the receive checksum counter" "it went DOWN ($before -> $after), which cannot happen while the switch is on: the counters are reset by a chain re-run, not by this switch"
                fi
            fi
        fi
    fi
    if ! j=$(runcall "$SEL_NETOFF" "$N_OFFRXOFF"); then
        bad "netoff rxoff" "the call failed: $(callinfo)"
    elif [ "$(jget "$j" "$O_OFFRXOFF")" = "0" ]; then
        ok "receive checksum marking is off again; every frame goes up unmarked and the stack computes the checksums, which is what it did before this mode ran"
    else
        bad "netoff rxoff" "the switch still reports on"
    fi

    step "M5-3: the transmit switch, which the family asks about once"
    if ! j=$(runcall "$SEL_NETOFF" "$N_OFFREPORT"); then
        bad "netoff report" "the call failed: $(callinfo)"
    elif [ "$(jget "$j" "$O_OFFTXON")" = "1" ]; then
        ok "the transmit offload switch is on -- and it was on BEFORE this interface attached, which is the only way it takes effect (the family asks getChecksumSupport once)"
    else
        blk "the transmit offload switch" "it is off, so the stack inserts every transmit checksum itself. Turning it on needs a boot where it is set before the interface attaches: run 'tools/verify.sh up' with CX3_TXCSUM=1"
    fi

    echo
    echo "  --- the data path's own counters after all of it (cx3ctl netstat) ---"
    "$CTL" netstat 2>&1 | sed 's/^/  /'
    return 0
}

do_stage() {
    step "stage: build, validate, install"
    ( cd "$ROOT" && BUILD_TAG="${BUILD_TAG:-verify}" bash "$SRC/build.sh" ) >"$LOG" 2>&1
    if grep -q "^BUILD RESULT: PASS" "$LOG"; then
        ok "build"
    else
        bad "build" "see $LOG"
        grep -E "error:|FAIL:" "$LOG" | head -5 | sed 's/^/      /'
        return 1
    fi
    grep -E "checks, [0-9]+ failure" "$LOG" | sed 's/^/      /'
    if ( cd "$ROOT" && bash "$SRC/install.sh" ) >>"$LOG" 2>&1; then
        ok "install"
    else
        bad "install" "see $LOG"
        return 1
    fi
    echo
    echo "  Next: reboot, then run 'tools/verify.sh run'."
    echo "  If the machine does not come back, add cx3off=1 to boot-args."
    return 0
}

# ---------------------------------------------------------------------------
# The ordered chain.
# ---------------------------------------------------------------------------
do_run() {
    # `leaveUp` is optional: run/full/safe pass two arguments and this script runs under `set -u`,
    # so a bare "$3" is an unbound-variable error rather than an empty string.  The `:-` was
    # missing on the first version of this and it broke `run`, `full` and `safe` at the first
    # line -- which is worth a line of comment, because the failure looks like a broken chain
    # and is a broken shell.
    local safe="$1" soak="$2" leaveUp="${3:-}" j v s pages tdone ttotal rc obs_irq obs_eqe obs_type
    # Set by the port stage below and read by the interrupt stage's failure text.  The port
    # is now downstream of the interrupt session, so a port problem must not abort the chain
    # before the test that matters -- but if both fail, the reader has to be told that the
    # device was not clean when the interrupt stage ran.
    local port_ok=1 port_note=""

    resolve_abi

    # The kernel log goes with the run (see klog_start).  From here to the end of this function
    # every device-facing step has a live capture behind it, and the path prints in the summary.
    klog_start
    trap klog_stop EXIT

    step "ABI self-check (the indices this script uses still exist)"
    if [ "$ABI_ERRORS" -gt 0 ]; then
        bad "abi indices" "$ABI_ERRORS name(s) missing from the header"
        return 1
    fi
    ok "abi indices resolve by name"

    # The scalar block is the KERNEL's 16 wide and a dispatch entry that declares more cannot
    # be called at all -- kIOReturnBadArgument, 0xe00002c2, for every argument combination a
    # caller can try, because a width the transport does not have cannot be supplied: the call
    # is refused before the selector's code runs.  0.43.9 learned this by raising our number
    # instead of moving a fact
    # (docs/DEFECTS.md F13) and lost a reboot's worth of measurements to it.
    #
    # The compiler catches it (the per-selector asserts in the header, plus the identity assert
    # in src/Cx3Mlx4UserClient.cpp where the kernel's constant is visible); this is the
    # independent second view, taken from the table the TOOL is built from, so that a selector
    # over the cap stops here rather than as a refused call whose message looks like a
    # permissions or argument-count problem.
    load_sizes
    local cap over
    cap=$(python3 -c 'import re,sys
m=re.search(r"#define\s+CX3_SCALAR_OUT_MAX\s+(\d+)", open(sys.argv[1]).read())
print(m.group(1) if m else "")' "$ABI")
    if [ -z "$cap" ]; then
        bad "scalar cap" "CX3_SCALAR_OUT_MAX is not a plain number in $ABI"
        return 1
    fi
    over=$(printf '%s\n' "$SIZES" | awk -v cap="$cap" '$3 > cap {print $5 " declares " $3}')
    if [ -n "$over" ]; then
        # BLOCKED rather than FAIL, and the distinction is the point: what this compares is the
        # table the TOOL was built with against the cap in the header, so the realistic cause is
        # a tool and a header from different batches (install.sh copies the tool beside the
        # kext; `cx3ctl sizes` on 0.43.9's tool prints 19 for r4tx).  Either way the run cannot
        # judge anything: a selector over the cap is unreachable (kIOReturnBadArgument, every
        # size combination), so nothing below would be measuring the device.
        blk "scalar cap" "$over scalar outputs, over the kernel's $cap. Such a selector is not wide, it is UNREACHABLE -- every argument combination fails with kIOReturnBadArgument. Either this tool and src/cx3_abi.h come from different batches (install the tool built beside the kext) or the header was edited without rebuilding; nothing below can be judged until they match (docs/DEFECTS.md F13)"
        return 1
    else
        ok "every selector's scalar-output width is within the kernel's $cap"
    fi

    # Is the RUNNING kext the one on disk?  Determined by comparing versions -- a fact --
    # rather than inferred from a call failing, which is what the first version did and
    # which produced a confident, wrong cause.
    step "which kext is running"
    load_sizes || { bad "cx3ctl sizes" "the tool cannot report its own table"; return 1; }
    local loaded installed
    loaded=$(kmutil showloaded 2>/dev/null | sed -n 's/.*Cx3Mlx4 (\([0-9.]*\)).*/\1/p' | head -1)
    installed=$(plutil -extract CFBundleShortVersionString raw \
                 /Library/Extensions/Cx3Mlx4.kext/Contents/Info.plist 2>/dev/null)
    if [ -z "$loaded" ]; then
        bad "loaded kext version" "Cx3Mlx4 does not appear in kmutil showloaded"
        blk "the whole chain" "the driver is not loaded"; return 1
    fi
    if [ "$loaded" != "$installed" ]; then
        blk "the loaded kext is $loaded, the installed one is $installed" \
            "a newer kext is staged; reboot and re-run -- nothing below would be measuring the binary on disk"
        STALE_KEXT=1
        return 1
    fi
    ok "running version $loaded matches the installed one"

    # Only two selectors are genuinely independent of device state.  Everything else this
    # script checks needs the HCA brought up first -- which is not a guess: an earlier
    # version put four of them here and three answered fw status 0x4 (BAD_SYS_STATE),
    # while the fourth "passed" only because its call had failed and left a zero.
    step "independent checks (the only two that need no device state)"
    if ! j=$(runcall "$SEL_PROBE"); then
        bad "probe" "the call failed: $(callinfo)"
        blk "the whole chain" "the device is not answering the simplest call"; return 1
    fi
    v=$(jget "$j" "$O_PROBEWIN")
    if [ "$v" = "1" ]; then ok "probe: the BAR0 window reads back live"
    else bad "probe window" "window=$v -- the device is not answering"; fi

    if ! j=$(runcall "$SEL_SURVEY"); then
        bad "survey" "the call failed: $(callinfo)"
    else
        v=$(jget "$j" "$O_SURVEYN")
        case "$v" in
            NO-INDEX|NO-OUT|PARSE-ERROR) bad "survey" "no usable output" ;;
            0) bad "survey" "zero entries reported" ;;
            *) ok "survey: $v entr(ies), no inconsistency found" ;;
        esac
    fi

    if [ "$fail" -gt 0 ]; then
        blk "everything downstream" "a read-only check failed, so device state is not trustworthy"
        return 1
    fi
    if [ "$safe" = "1" ]; then
        skip "reset and the whole device chain" "--safe was requested"
        return 0
    fi

    step "the chain (each step depends on the ones above it)"

    if ! j=$(runcall "$SEL_RESET" 1 1); then
        bad "reset" "the call failed: $(callinfo)"
        blk "everything after reset" "no known device state"; return 1
    fi
    v=$(jget "$j" "$O_RSTSTAT")
    if [ "$v" = "0" ]; then ok "reset"
    else bad "reset" "status=$v"; blk "everything after reset" "no known device state"; return 1; fi

    if ! j=$(runcall "$SEL_QUERYFW" 1); then
        bad "QUERY_FW" "the call failed: $(callinfo)"
        blk "fwload and after" "no FW size"; return 1
    fi
    pages=$(jget "$j" "$O_FWPAGES")
    if [ "$(jget "$j" "$O_FWSTAT")" = "0" ] && [ "$pages" != "0" ] && [ "$pages" != "NO-INDEX" ]; then
        ok "QUERY_FW: $pages FW pages"
    else
        bad "QUERY_FW" "fwStatus=$(jget "$j" "$O_FWSTAT") pages=$pages"
        blk "fwload and after" "no FW size to allocate"; return 1
    fi

    if ! j=$(runcall "$SEL_FWLOAD" "$pages"); then
        bad "fwload" "the call failed: $(callinfo)"
        blk "context memory and after" "the firmware was not handed over"; return 1
    fi
    if [ "$(jget "$j" "$O_MAPFA")" = "0" ] &&
       [ "$(jget "$j" "$O_RUNFW")" = "0" ] &&
       [ "$(jget "$j" "$O_MODSTAT")" = "0" ]; then
        ok "MAP_FA + RUN_FW + MOD_STAT_CFG"
    else
        bad "fwload" "MAP_FA=$(jget "$j" "$O_MAPFA") RUN_FW=$(jget "$j" "$O_RUNFW") MOD=$(jget "$j" "$O_MODSTAT")"
        blk "context memory and after" "the firmware was not handed over"; return 1
    fi

    # QUERY_DEV_CAP needs RUN_FW to have completed -- measured: before that it answers
    # fw status 0x4 (BAD_SYS_STATE) even with force=1 bypassing the ownership gate.  It is
    # read here rather than only through m2dprofile because reading it independently is
    # exactly the cross-check that catches a decode mistake.
    if ! j=$(runcall "$SEL_QUERYDEVCAP" 1 0); then
        bad "querydevcap" "the call failed: $(callinfo)"
    else
        v=$(jget "$j" "$O_DCSTAT")
        if [ "$v" = "0" ]; then ok "QUERY_DEV_CAP: capabilities read independently"
        else bad "querydevcap" "fw status=$v (RUN_FW has completed, so this should answer)"; fi
    fi

    if ! j=$(runcall "$SEL_M2DPROFILE" "$PROFILE_QP" "$PROFILE_RDMARC" "$PROFILE_SRQ" \
                                 "$PROFILE_CQ" "$PROFILE_MCG" "$PROFILE_MPT" "$PROFILE_MTT"); then
        bad "m2dprofile" "the call failed: $(callinfo)"
        blk "context memory and after" "no profile"; return 1
    fi

    if ! j=$(runcall "$SEL_M2DBRINGUP"); then
        bad "m2dbringup" "the call failed: $(callinfo)"
        blk "queues and after" "no context memory"; return 1
    fi
    tdone=$(jget "$j" "$O_TABLESDONE"); ttotal=$(jget "$j" "$O_TABLESTOTAL")
    if [ "$(jget "$j" "$O_BUSTAT")" = "0" ] &&
       [ "$(jget "$j" "$O_INITHCA")" = "0" ] && [ "$tdone" = "$ttotal" ]; then
        ok "context memory: $tdone/$ttotal tables, INIT_HCA ok"
    else
        bad "bring-up" "status=$(jget "$j" "$O_BUSTAT") initHca=$(jget "$j" "$O_INITHCA") tables=$tdone/$ttotal"
        blk "queues and after" "the HCA is not initialised"; return 1
    fi

    # Now the HCA is initialised, so the two selectors that need it can answer.  They were
    # placed before bring-up in the first version and both refused with fw status 0x4
    # (BAD_SYS_STATE) -- measured, not deduced from the reference.
    if ! j=$(runcall "$SEL_QUERYHCA"); then
        bad "queryhca" "the call failed: $(callinfo)"
    else
        v=$(jget "$j" "$O_QHCASTAT")
        if [ "$v" = "0" ]; then ok "QUERY_HCA: steering mode read"
        else bad "queryhca" "fw status=$v (after INIT_HCA this should answer)"; fi
    fi

    # -----------------------------------------------------------------------
    # Device order, and why the chain is in this one (docs/EQ-ORDERING.md section 5).
    #
    # Create the EQ, attach and arm its interrupt, enable delivery, and only then activate
    # the port.  That is the reference's own relative order and not an interpretation of
    # it: mlx4_init_eq_table() creates the queue, attaches the interrupt
    # (mlx4_eq.c:1250 request_irq), maps the events (:1270) and arms the async EQ (:1277),
    # all inside mlx4_load_one() (mlx4_main.c:2669); the port is activated afterwards by
    # SET_PORT + INIT_PORT (mlx4_main.c:2756, mlx4_en_netdev.c:1394-1403).
    #
    # This order is the fix, not tidiness.  With the port up first, INIT_PORT's
    # asynchronous event lands in a queue that is armed but has no delivery, fires it, and
    # from FIRED the HCA asserts nothing more: 0.43.0 measured 0x00000a00 (FIRED,
    # invocations=0, no interrupt) for that order and 0x00000900 (ARMED, the completion
    # arriving) for this one, same build, only that difference.  The port selector no
    # longer refuses while an EQ exists -- it logs -- which is what makes this order
    # expressible at all (src/Cx3Mlx4.cpp, exportM3PortUp).
    #
    # queryport stays after m3portup: it reads back what the port-up just wrote.
    # -----------------------------------------------------------------------
    if ! j=$(runcall "$SEL_M3EQUP" 0 0 1 1); then
        bad "m3equp" "the call failed: $(callinfo)"
        blk "interrupts and after" "there is no EQ"; return 1
    fi
    if [ "$(jget "$j" "$O_EQUPSTAT")" = "0" ] &&
       [ "$(jget "$j" "$O_MAPEQS")" = "0" ]; then
        ok "EQ: EQN $(jget "$j" "$O_EQN"), MAP_EQ ok"
    else
        bad "EQ bring-up" "status=$(jget "$j" "$O_EQUPSTAT") mapEq=$(jget "$j" "$O_MAPEQS")"
        blk "interrupts and after" "there is no EQ"; return 1
    fi

    if ! j=$(runcall "$SEL_M3CLRINT"); then
        bad "m3clrint" "the call failed: $(callinfo)"
        blk "the interrupt session" "a legacy source without a clear storms"; return 1
    fi
    if [ "$(jget "$j" "$O_CLRSTAT")" = "0" ]; then ok "interrupt-clear register set up"
    else bad "clear register" "status=$(jget "$j" "$O_CLRSTAT")"
         blk "the interrupt session" "a legacy source without a clear storms"; return 1; fi

    if ! j=$(runcall "$SEL_M3IRQUP" 0 0); then
        bad "m3irqup" "the call failed: $(callinfo)"
        blk "the interrupt test" "nothing would be delivered"; return 1
    fi
    if [ "$(jget "$j" "$O_IRQUPSTAT")" = "0" ] &&
       [ "$(jget "$j" "$O_IRQUPEN")" = "1" ]; then
        ok "interrupt session open"
    else
        bad "interrupt session" "status=$(jget "$j" "$O_IRQUPSTAT") enabled=$(jget "$j" "$O_IRQUPEN")"
        blk "the interrupt test" "nothing would be delivered"; return 1
    fi

    # R3's outcome, in the reference's place: the port up and then read back.  Without this
    # the chain would stop short of the state the project is actually in.  It runs with the
    # EQ armed and delivery enabled on purpose -- that is what the order above is for, and
    # what makes INIT_PORT's asynchronous event consumable.
    #
    # A failure here does NOT end the chain, which is the one attribution change this
    # reordering needs.  The interrupt stage below is the test this whole order exists for;
    # aborting on a port problem would turn "the port did not come up" into "interrupt
    # delivery was never asked", and those call for different responses.  Instead the port's
    # outcome is carried into the interrupt stage's failure text, so a double failure reads
    # as a double failure.
    if ! j=$(runcall "$SEL_M3PORTUP" 1 0); then
        bad "m3portup" "the call failed: $(callinfo)"
        port_ok=0
    fi
    if [ "$port_ok" = "1" ]; then
        { s=$(jget "$j" "$O_PUPSTEP"); } 2>/dev/null || true
        if [ "$(jget "$j" "$O_PUPLINKAFTER")" = "1" ]; then
            ok "port 1: link up"
        else
            bad "port 1" "link bit=$(jget "$j" "$O_PUPLINKAFTER") after m3portup (is a cable connected?)"
            port_ok=0
        fi
        # The step that tells the firmware where A0's MAC-derived QPN range starts
        # (docs/R4-STAGE7-PLAN.md section 2/3, docs/R4-STAGE7-RESULT.md).  Its own scalar, and
        # three ANSWERS THAT MUST READ DIFFERENTLY, because they call for three different
        # responses:
        #
        #   fw status 0x0        the command was sent and accepted;
        #   a real error code    the command was sent and REFUSED -- 0x3 BAD_PARAM or
        #                        0x5 BAD_RESOURCE would mean the firmware does check the base
        #                        against a region of its own, which is this stage's written
        #                        falsification condition: the base then has to move to
        #                        num_qps - (1 << log_max_macs) with the RX QP alongside it;
        #   the sentinel         the step RAN and nobody recorded an answer (a driver defect:
        #                        the scalar starts at CX3_FWSTATUS_NOT_POSTED before the call);
        #   a lower step         it never ran at all -- an earlier step failed, and that step's
        #                        own line above is the failure to read.  The threshold is
        #                        kPortUpStepQpnCalc from the ABI, not a literal here.
        #
        # The last two are the pair this project keeps getting wrong: "did not run" and "ran
        # and said nothing" both look like silence, and neither is the same claim as "refused".
        q=$(jget "$j" "$O_PUPQPNCALC")
        case "$q" in
            NO-*|PARSE-ERROR)
                skip "port 1 qpn_calc" "this kext has no $O_PUPQPNCALC scalar -- it predates stage 7; reboot with the staged kext" ;;
            "0")
                ok "port 1 qpn_calc: fw status 0x0 (the firmware now maps MAC index i to base+i)" ;;
            "4294967294")
                # The sentinel means either "never reached" (a step below the qpn_calc one) or
                # "reached and not recorded" (that step or above), and those are different
                # faults.  Whether the step is a number is decided first, so a missing or
                # unreadable step cannot be quietly compared as a number and land in the wrong
                # arm -- and the threshold is the ABI's own name rather than a second literal.
                case "${s:-none}" in
                    ''|*[!0-9]*) pre=none ;;
                    *) if [ "$s" -lt "$N_PUPQPNCALCSTEP" ]; then pre=yes; else pre=no; fi ;;
                esac
                if [ "$pre" = "no" ]; then
                    bad "port 1 qpn_calc" "step $s of $N_PUPSTEPS says the qpn_calc step ran, but its fw status is still the NOT-POSTED sentinel (4294967294 = 0xfffffffe): the driver asked the device and did not record the answer. That is a driver defect, not a firmware one -- the kernel log has the command's own line"
                else
                    skip "port 1 qpn_calc" "NOT REACHED: the port-up chain stopped at step ${s:-an unreadable value} of $N_PUPSTEPS, before the qpn_calc step $N_PUPQPNCALCSTEP, so the command was never sent. The failing line above is the one to read -- this is not a second fault"
                fi ;;
            "4294967293")
                bad "port 1 qpn_calc" "fw status 0xfffffffd: the command was POSTED and never completed (a timeout, docs/DEFECTS.md B1). Different from a rejection: the firmware never answered, so nothing is known about the base QPN" ;;
            *)
                bad "port 1 qpn_calc" "fw status 0x$q -- the firmware REFUSED the command. If it is 0x3 (BAD_PARAM) or 0x5 (BAD_RESOURCE) then the firmware does check that the base QPN lands in a region of its own, which is stage 7's written falsification condition (docs/R4-STAGE7-PLAN.md section 6): the base has to move to num_qps - (1 << log_max_macs) and the RX QP with it. The kernel log prints the context that was sent" ;;
        esac
    fi
    if [ "$port_ok" = "0" ]; then
        # Not a BLOCKED line: nothing downstream depends on the port any more.  The note is
        # what the interrupt stage reads.
        port_note=" [note: the port-up stage above also failed, so the device was not in a clean state when this ran]"
    fi

    if ! j=$(runcall "$SEL_QUERYPORT" 1); then
        bad "queryport" "the call failed: $(callinfo)"
    else
        v=$(jget "$j" "$O_QPSTAT")
        if [ "$v" = "0" ]; then ok "QUERY_PORT 1: port answer read"
        else bad "queryport" "fw status=$v"; fi
    fi

    # The strongest evidence this order can produce without a completion queue: the event
    # INIT_PORT creates is consumed by the interrupt action, not lost.  Last EQE type 9 is
    # MLX4_EVENT_TYPE_PORT_CHANGE (src/mlx4_regs.h:670), and the reference expects exactly
    # that event from this step -- its link-state handler says "we get here on INIT_PORT"
    # (mlx4_en_netdev.c:1242).  A PORT_CHANGE observed here is stronger than m3irqtest
    # below, because it is the port bring-up's own event rather than one the test posted.
    #
    # Not an assertion, deliberately.  Whether the firmware emits a PORT_CHANGE for this
    # particular bring-up, and when, is not something this project has measured, so its
    # absence is recorded as SKIP with the numbers rather than FAIL: a check that can fail
    # for a reason that is not a defect teaches the reader to ignore it.  The interrupt
    # stage below is the actual test.
    if ! j=$(runcall "$SEL_M3IRQSTATUS"); then
        skip "the port's asynchronous event" "m3irqstatus could not be read (rc=$LAST_CALL_RC)"
    else
        obs_irq=$(jget "$j" "$O_IRQCOUNT")
        obs_eqe=$(jget "$j" "$O_IRQEQECOUNT")
        obs_type=$(jget "$j" "$O_IRQLASTTYPE")
        case "$obs_irq:$obs_type" in
            *NO-*|*PARSE-ERROR*)
                skip "the port's asynchronous event" "m3irqstatus gave no usable output" ;;
            "0:"*)
                skip "the port's asynchronous event" "invocations=0: the port bring-up delivered no interrupt within this window. Not a defect (whether the firmware emits a PORT_CHANGE for this bring-up is unmeasured) and not the test -- the interrupt stage below is" ;;
            *:9)
                ok "the port's PORT_CHANGE event arrived through the interrupt ($obs_irq invocation(s), $obs_eqe EQE(s) consumed, last EQE type 0x09)" ;;
            *)
                skip "the port's asynchronous event" "$obs_irq invocation(s), $obs_eqe EQE(s) consumed, last EQE type 0x$obs_type -- an interrupt ran, but this reading does not show a PORT_CHANGE (0x09) as the last event consumed" ;;
        esac
    fi

    if ! j=$(runcall "$SEL_M3IRQTEST" 0x5a5a); then
        bad "m3irqtest" "the call failed: $(callinfo)"
        blk "the interrupt result" "nothing was posted"; return 1
    fi
    if [ "$(jget "$j" "$O_IRQTESTSTAT")" = "0" ]; then ok "event-mode NOP posted"
    else bad "irq test post" "status=$(jget "$j" "$O_IRQTESTSTAT")"
         blk "the interrupt result" "nothing was posted"; return 1; fi

    sleep 1
    if ! j=$(runcall "$SEL_M3IRQSTATUS"); then
        bad "m3irqstatus" "the call failed: $(callinfo)"
    else
        if [ "$(jget "$j" "$O_IRQMATCHED")" = "1" ] &&
           [ "$(jget "$j" "$O_IRQMATCHSTAT")" = "0" ]; then
            ok "the completion arrived through the interrupt"
        else
            # The counters say what happened; the state word says why, and the two are
            # different answers.  The explanation this line used to carry ("the queue
            # fired before delivery existed, so nothing re-armed it") described the
            # order the chain no longer uses -- that was the defect 0.43.1 removed, and
            # a reader who applies it here would be looking for the wrong cause.
            # Readings, in the order that separates what is left: m3eqinfo's QUERY_EQ
            # flags word -- 0x900 ARMED (the queue is armed and re-arms on every
            # consumer pass, so the remaining suspects are the interrupt source index
            # and the vector; m3irqup prints getInterruptType's answer for that), or
            # 0xa00 FIRED (an event fired and nothing consumed it: the poll selector, or
            # a source left disabled by the storm guard) -- then the kernel log's
            # "armed the EQ again after enabling delivery" line from m3irqup.
            bad "interrupt delivery" "matched=$(jget "$j" "$O_IRQMATCHED") status=$(jget "$j" "$O_IRQMATCHSTAT") invocations=$(jget "$j" "$O_IRQCOUNT") -- read m3eqinfo: QUERY_EQ flags 0x900 (ARMED: the queue is armed and re-arms, so suspect the source index or the vector) or 0xa00 (FIRED: an event fired and no consumer ran)$port_note"
        fi
    fi

    if runcall "$SEL_M3IRQDOWN" >/dev/null; then ok "interrupt session closed"
    else bad "m3irqdown" "the call failed: $(callinfo)"; fi

    step "R4 stages (memory only so far: nothing is reachable through a QP yet)"

    if ! j=$(runcall "$SEL_R4MR" 0 0); then
        bad "r4mr" "the call failed: $(callinfo)"
        blk "the CQ and the RQ" "no lkey for a descriptor"; return 1
    fi
    if [ "$(jget "$j" "$O_R4MRSTAT")" = "0" ]; then ok "memory region enabled"
    else bad "r4mr" "status=$(jget "$j" "$O_R4MRSTAT")"
         blk "the CQ and the RQ" "no lkey for a descriptor"; return 1; fi

    if ! j=$(runcall "$SEL_R4CQ" 512 128); then
        bad "r4cq" "the call failed: $(callinfo)"
        blk "the receive ring" "no queue for completions"; return 1
    fi
    if [ "$(jget "$j" "$O_R4CQSTAT")" = "0" ]; then ok "completion queue created"
    else bad "r4cq" "status=$(jget "$j" "$O_R4CQSTAT")"
         blk "the receive ring" "no queue for completions"; return 1; fi

    if ! j=$(runcall "$SEL_R4RQ" 256 2048); then
        bad "r4rq" "the call failed: $(callinfo)"
        return 1
    fi
    if [ "$(jget "$j" "$O_R4RQSTAT")" = "0" ]; then ok "receive ring built"
    else bad "r4rq" "status=$(jget "$j" "$O_R4RQSTAT")"; return 1; fi

    # -----------------------------------------------------------------------
    # R4 stage 4: the RX QP.
    #
    # Three layers, all judged from SCALARS (see the resolution block above), because they
    # fail in ways that look alike and call for different responses:
    #
    #   layer 1  the three commands AND the state read back after EACH of them.  "fw status
    #            0" is not "the hardware is in that state": a middle step that was silently
    #            dropped still ends at RTS if a later one compensates, so the counts of
    #            accepted commands and confirmed states are two separate numbers and both
    #            have to be 3.
    #   layer 2  the final QUERY_QP, compared field by field against what was written
    #            (fieldsMatch) and checked for the two things that identify the QP itself
    #            (queryState == 3, qpnConfirmed).  mttMatches is separate rather than folded
    #            in: the MTT address is written by the RST->INIT step only and copied through
    #            the other two, so a mismatch there is a different defect from a wrong queue
    #            parameter.
    #   layer 3  the producer: the value written into host memory and the value read back out
    #            of it.  Both are printed, and the equality is asserted, because the failure
    #            this guards against is a driver that believes it published an index the
    #            hardware never sees.
    #
    # A failing step here does not end the chain: nothing downstream (the M4 gate) is
    # reachable yet anyway, and reporting "the QP did not come up" as BLOCKED would hide
    # whether the earlier stages still work.  The three layer lines are separate PASS/FAIL
    # decisions for that reason.
    # -----------------------------------------------------------------------
    if ! j=$(runcall "$SEL_R4QP" 0); then
        bad "r4qp" "the call failed: $(callinfo)"
        skip "the three QP layers" "the selector could not be called"
    else
        v=$(jget "$j" "$O_R4QPSTAT")
        if [ "$v" != "0" ]; then
            bad "r4qp" "status=$v -- see the kernel log (a refusal before the device is asked)"
            skip "the three QP layers" "nothing was sent"
        else
            qpn=$(jget "$j" "$O_R4QPQPN")
            steps=$(jget "$j" "$O_R4QPSTEPS")
            states=$(jget "$j" "$O_R4QPSTATES")
            case "$qpn" in
                NO-*|PARSE-ERROR) bad "r4qp: the QPN" "no usable value" ;;
                0) bad "r4qp: the QPN" "the readback reports QPN 0, which is a firmware QP and not one this driver may hold" ;;
                *) ok "QP: QPN $qpn, the rule's value, confirmed by QUERY_QP" ;;
            esac
            # Layer 1.  Both numbers, and the state is read after EVERY step rather than only
            # at the end, which is what makes a dropped middle step visible.
            if [ "$steps" = "3" ] && [ "$states" = "3" ]; then
                ok "QP layer 1: 3/3 commands accepted, 3/3 states read back after each step"
            else
                bad "QP layer 1" "accepted=$steps/3 statesConfirmed=$states/3 -- a command returning 0 and a state that was read back are two different facts; the kernel log prints the per-step fw status and what QUERY_QP said after each one"
            fi
            # Layer 2.
            if [ "$(jget "$j" "$O_R4QPQRYOK")" != "1" ]; then
                bad "QP layer 2" "the final QUERY_QP did not answer, so the resulting state is unknown"
            elif [ "$(jget "$j" "$O_R4QPQRYST")" != "3" ]; then
                bad "QP layer 2" "the QP is in state $(jget "$j" "$O_R4QPQRYST"), not RTS(3) -- check the flags nibble between steps and params2's FPP bit"
            elif [ "$(jget "$j" "$O_R4QPQPNOK")" != "1" ]; then
                bad "QP layer 2" "the firmware's local_qpn is not the QPN written, so this is a different QP"
            elif [ "$(jget "$j" "$O_R4QPFIELDS")" != "1" ]; then
                bad "QP layer 2" "the readback does not match what was written: rq_size_stride/sq_size_stride/cqn_send/cqn_recv/db_rec_addr/mtu_msgmax -- the kernel log names the field and prints both numbers"
            else
                ok "QP layer 2: RTS, local_qpn and all eight compared fields match"
                # The eight are the fields the reference's semantics require to survive.  Two
                # more are written and do NOT come back the same -- param3's FCS-keep bit and
                # sched_queue's low bit, measured 2026-10-12 as 0x20000000/0x0 and 0x83/0x82.
                # They are not judged because the reference never reads them back, so there is
                # nothing to compare against.  But a summary whose last word is "eight matched"
                # reads as "everything matched", and the kernel log two lines above says
                # otherwise -- the F9 shape, where the record is itself an unchecked assertion.
                echo "           and two fields are reported rather than judged: param3 and"
                echo "           sched_queue (the kernel log prints both written/read pairs;"
                echo "           docs/R4-STAGE4-RESULT.md 9.1 says what that does and does not mean)"
            fi
            # The MTT fields are only meaningful if the query that reads them answered at all:
            # when it did not, the scalar is CX3_R4QP_NOT_MEASURED and reporting a verdict
            # about it would be exactly the mistake this project keeps recording -- a number
            # that was never read being printed as a result.  The query's own failure is the
            # line above; this one is only asked when there is something to ask it of.
            if [ "$(jget "$j" "$O_R4QPQRYOK")" = "1" ] &&
               [ "$(jget "$j" "$O_R4QPMTT")" != "1" ]; then
                bad "QP layer 2 (the MTT address)" "the RST->INIT-only fields did not survive the other two steps -- the shared context was disturbed; see the kernel log"
            fi
            # Layer 3: the delivery action.
            pv=$(jget "$j" "$O_R4QPPROD"); pb=$(jget "$j" "$O_R4QPPRODBACK")
            if [ "$(jget "$j" "$O_R4QPPRODOK")" = "1" ] && [ "$pv" = "$pb" ] && [ "$pv" != "0" ]; then
                ok "QP layer 3: the RQ producer $pv is in host memory, read back as $pb"
            else
                bad "QP layer 3" "wrote $pv, read back $pb (matches=$(jget "$j" "$O_R4QPPRODOK")) -- the hardware would read a count nobody published"
            fi
        fi
    fi

    # -----------------------------------------------------------------------
    # R4 stage 5: the MAC table and the RSS indirection QP.
    #
    # Neither half produces a completion, so no line here may be justified by "traffic
    # arrived" -- and the two readbacks are not the same kind of evidence:
    #
    #   the MAC table  the reference NEVER reads this table back: it keeps entries[]/refs[]/
    #                  total in software (mlx4_port.c:182-340) and there is no
    #                  get_port_mac_table anywhere in the tree.  So the read direction -- the
    #                  same SET_PORT with in_param 0 -- is THIS PORT'S PROBE, and the three
    #                  outcomes are kept apart on purpose:
    #                    readbackOk 0                -> "unavailable".  Not a FAIL, and not a
    #                                                   pass either: the MAC's evidence is then
    #                                                   only the write's fw status, and the
    #                                                   line says so in as many words.  This is
    #                                                   the degradation the plan asks for, and
    #                                                   the verifier shows it as SKIP-loud
    #                                                   rather than as a silent success.
    #                    readbackOk 1, matches 0     -> FAIL: the firmware answered with a
    #                                                   different table, which is a fact about
    #                                                   the device.
    #                    readbackOk 1, matches 1     -> the only two-sided evidence available.
    #   the entry QP   the ordinary kind of readback, and the stage's verdict rests on ONE
    #                  field of it: the firmware's default_qpn must be the RX QP of stage 4.
    #                  The failure this exists to catch is "the entry QP points at itself",
    #                  which passes every command, reaches RTS, and loses every hashed frame.
    # -----------------------------------------------------------------------
    if ! j=$(runcall "$SEL_R4INGRESS"); then
        bad "r4ingress" "the call failed: $(callinfo)"
        skip "the ingress layers" "the selector could not be called"
    else
        v=$(jget "$j" "$O_R4INST")
        if [ "$v" != "0" ]; then
            if [ "$v" = "$IORETURN_NOTREADY" ]; then
                blk "r4ingress" "status=$v (kIOReturnNotReady): a stage it depends on has not run -- the indirection QP must point at the RX QP, so this is BLOCKED rather than failed. The kernel log names the missing prerequisite"
            else
                bad "r4ingress" "status=$v -- see the kernel log (a refusal before the device is asked)"
            fi
            skip "the ingress layers" "nothing was sent"
        else
            # --- the MAC half ---
            mstat=$(jget "$j" "$O_R4INMACSTAT")
            mslot=$(jget "$j" "$O_R4INMACSLOT")
            mrbok=$(jget "$j" "$O_R4INMACRBOK")
            mrb=$(jget "$j" "$O_R4INMACRB")
            case "$mstat" in
                NO-*|PARSE-ERROR) bad "r4ingress: the MAC command" "no usable value" ;;
                NO-INDEX) skip "the MAC table" "this kext has no r4ingress scalars -- it predates stage 5; reboot with the staged kext" ;;
                0) ok "MAC: SET_PORT MAC_TABLE accepted (fw status 0x0), slot $mslot" ;;
                *) bad "r4ingress: the MAC command" \
                       "fw status=$mstat -- the table was not accepted. in_param must be the MAILBOX's DMA address and in_modifier 0x201 (MAC_TABLE 2 << 8 | port 1); both are printed in the kernel log" ;;
            esac
            case "$mslot" in
                NO-*|PARSE-ERROR|NO-INDEX) : ;;
                0) ok "MAC: the slot is 0, measured out of the table that was sent (the first non-empty of its entries)" ;;
                *) bad "r4ingress: the MAC slot" "slot=$mslot, expected 0 -- the scalar is the first non-empty entry of the table the command carried, so the address reached the firmware at index $mslot while the driver decided on 0, and under A0 a MAC index selects a QPN (base_qpn + index): a different index is a different queue. The kernel log prints the decided and the measured number and the entry's bytes" ;;
            esac
            case "$mrbok" in
                NO-*|PARSE-ERROR|NO-INDEX) : ;;
                0)
                    # Not a FAIL, and deliberately not written as a PASS either: "the
                    # read-back is unavailable" leaves the MAC's evidence at the write's fw
                    # status alone.  Reporting it as PASS would be the "record that is itself
                    # an unchecked assertion" this project keeps paying for (D8/D10/F9).
                    #
                    # Two ways bring it here and the kernel log says which: the command did not
                    # answer at all, or it answered with an outbox of zeros -- which is what
                    # this card does (measured 2026-10-12 07:16, fw status 0x0, entry
                    # 00 00 00 00 00 00 00 00).  An empty outbox is not a table, so neither
                    # case supports "the firmware holds a DIFFERENT table", which is what this
                    # verifier reported until 0.43.4 -- a verdict with nothing behind it
                    # (docs/DEFECTS.md D12).
                    skip "MAC read-back (this port's probe, not the reference's practice)" \
                         "did not answer, or answered with an all-zero outbox -- either way there is no content to compare, so the read direction is UNAVAILABLE and the MAC's evidence is the write's fw status ($mstat) alone. NOT 'the MAC was not registered', and NOT 'the firmware holds a different table': those are claims this probe cannot make. The kernel log says which of the two happened; reason and falsification condition are in docs/R4-STAGE5-RESULT.md" ;;
                1)
                    if [ "$mrb" = "1" ]; then
                        ok "MAC read-back: the firmware holds the entry this driver sent, byte for byte"
                    else
                        bad "r4ingress: the MAC read-back" "the firmware answered with a DIFFERENT table -- the kernel log prints both 8-byte entries" 
                    fi ;;
            esac
            # --- the entry QP ---
            eqpn=$(jget "$j" "$O_R4INQPN")
            rxqpn=$(jget "$j" "$O_R4INRXQPN")
            steps=$(jget "$j" "$O_R4INSTEPS")
            states=$(jget "$j" "$O_R4INSTATES")
            case "$eqpn" in
                NO-*|PARSE-ERROR|NO-INDEX) skip "the ingress QP" "no QPN in the output" ;;
                0) bad "r4ingress: the entry QPN" "the readback reports 0, which is a firmware QP" ;;
                *) ok "ingress QP: QPN $eqpn above the RX QPN $rxqpn, inside the range software may use" ;;
            esac
            if [ "$steps" = "3" ] && [ "$states" = "3" ]; then
                ok "ingress layer 1: 3/3 commands accepted, 3/3 states read back after each step"
            else
                bad "ingress layer 1" "accepted=$steps/3 statesConfirmed=$states/3 -- a command returning 0 and a state that was read back are two different facts; the kernel log prints the per-step fw status and what QUERY_QP said after each one. For this stage check the RSS bit in flags (bit 13) and the overlay at QPC+0x3c"
            fi
            if [ "$(jget "$j" "$O_R4INQRYOK")" != "1" ]; then
                bad "ingress layer 2" "the final QUERY_QP did not answer, so what the firmware holds is unknown"
            elif [ "$(jget "$j" "$O_R4INQRYST")" != "3" ]; then
                bad "ingress layer 2" "the entry QP is in state $(jget "$j" "$O_R4INQRYST"), not RTS(3)"
            else
                ok "ingress layer 2: the entry QP is in RTS"
            fi
            # The field the whole stage rests on, and the field it exists to rule out.  Both
            # are asked only when the query answered: when it did not, the scalars are
            # CX3_R4INGRESS_NOT_MEASURED and a verdict about them would be a claim about a
            # number nobody read.
            if [ "$(jget "$j" "$O_R4INQRYOK")" = "1" ]; then
                if [ "$(jget "$j" "$O_R4INDEF")" = "1" ] &&
                   [ "$(jget "$j" "$O_R4INBASE")" = "1" ]; then
                    ok "ingress layer 2 (the hash target): default_qpn and base_qpn are both the RX QPN $rxqpn"
                else
                    bad "ingress layer 2 (the hash target)" "default_qpn=$(jget "$j" "$O_R4INQDEFAULT") (wanted $rxqpn), base_qpn=$(jget "$j" "$O_R4INQBASE") -- a mismatch here means the QP steers into a queue this driver did not intend, and the kernel log prints both sent/read pairs"
                fi
                if [ "$eqpn" != "$rxqpn" ] && [ "$(jget "$j" "$O_R4INQDEFAULT")" != "$eqpn" ]; then
                    ok "ingress: the entry QP does not point at itself (QPN $eqpn, hash target $(jget "$j" "$O_R4INQDEFAULT"))"
                else
                    bad "r4ingress: the entry QP points at ITSELF" "QPN $eqpn and default_qpn $(jget "$j" "$O_R4INQDEFAULT") -- this reaches RTS, reads back clean, and loses every hashed frame, because the queue it names has no receive ring"
                fi
                # Two context fields are written and do NOT come back as written, measured
                # 2026-10-12 07:16 on 0.43.3: cqn_recv (128 written / 5066319 = 0x4d504f read
                # back) and db_rec_addr (0x115caec000 written / 0x404040414240 read back).  The
                # firmware normalises them for an indirection QP, the same shape as stage 4's
                # param3/sched_queue, and they are REPORTED rather than judged because there is
                # nothing to compare them against: the reference never reads these back, and an
                # indirection QP produces no completions, so neither field is load-bearing.
                # This is the stage-4 precedent (the PASS whose last word was "eight matched"
                # while two more fields were known to differ -- docs/DEFECTS.md F10); a summary
                # sentence that reads as "everything matched" when it did not is the defect,
                # not the judgement.  The kernel log prints all four values.
                echo "           and two fields are reported rather than judged: cqn_recv and"
                echo "           db_rec_addr (the firmware does not keep either for an"
                echo "           indirection QP; the kernel log prints both written/read pairs"
                echo "           and docs/R4-STAGE5-RESULT.md says whether they bear load)"
            fi
        fi
    fi

    # -----------------------------------------------------------------------
    # R4 stage 6: the transmit side's RESOURCES.
    #
    # Four areas, every JUDGEMENT of them read out of a scalar.  Two of the numbers these
    # lines used to restate stopped being scalars in 0.43.10 -- the CQN and the top-reserved
    # line, which the kernel's 16-wide scalar block had no room for once r4tx filled it
    # (docs/DEFECTS.md F13) -- so they are named as living in the structure and in
    # `cx3ctl r4tx`.  Nothing here decides on either of them: the verdict on them
    # (kR4TxOutNumberSafe) is a scalar and still decides.  The two layer-1 counts are judged
    # out of one scalar (steps low, states high) with the shift resolved from the header.
    #
    #   the TX CQ     SW2HW_CQ accepted, AND the fresh ring reads as empty.  The
    #                 second is not decoration: a slot whose owner byte reads as the
    #                 consumer's phase is a completion read out of memory nobody
    #                 wrote, which is the defect the owner preset exists to prevent.
    #   the numbers   both below num_qps - (1 << log_max_macs), and colliding with
    #                 nothing that already exists.  The line is computed from two
    #                 values read at runtime, so what is checked is what the driver
    #                 read, not a constant either side could agree on by accident.
    #   the send QP   the same three layers stage 4 judges, on the send side: 3/3
    #                 commands accepted AND 3/3 states read back after EACH step,
    #                 then RTS with local_qpn confirmed and the eight compared
    #                 fields matching.
    #   the send queue sq_size_stride 0x42 -- this stage's signature byte, and one
    #                 the plan names first -- plus WQE 0's stamp read back
    #                 OUT of the memory rather than restated from the constant.
    #                 Since 0.43.8 "WQE 0" is the allocation's first unit: a send
    #                 queue has no 64-byte header in front of its WQEs
    #                 (mlx4_en_tx.c:139/:686), unlike the receive ring
    #                 (mlx4_en_rx.c:406/:482-489).  docs/DEFECTS.md E13 is what
    #                 the receive ring's shape costs here.
    #
    # AND WHAT IT DOES NOT CLAIM: nothing was sent.  There is no traffic assertion
    # here because this stage posts no WQE and rings no doorbell, so a line about
    # frames would be a claim nothing measured.  Posting to this queue is stage 9.
    # -----------------------------------------------------------------------
    if ! j=$(runcall "$SEL_R4TX" 0 0); then
        bad "r4tx" "the call failed: $(callinfo)"
        skip "the transmit resources" "the selector could not be called"
    else
        v=$(jget "$j" "$O_R4TXSTAT")
        if [ "$v" != "0" ]; then
            if [ "$v" = "$IORETURN_NOTREADY" ]; then
                blk "r4tx" "status=$v (kIOReturnNotReady): a stage it depends on has not run -- the numbers are derived above the RX QP and the indirection QP, and the UAR comes from the RX CQ, so this is BLOCKED rather than failed. The kernel log names the missing prerequisite"
            else
                bad "r4tx" "status=$v -- see the kernel log (a refusal before the device is asked)"
            fi
            skip "the transmit resources" "nothing was sent"
        else
            cqstat=$(jget "$j" "$O_R4TXCQSTAT")
            cqready=$(jget "$j" "$O_R4TXCQREADY")
            case "$cqstat" in
                NO-*|PARSE-ERROR) bad "r4tx: the TX CQ" "no usable value" ;;
                NO-INDEX) skip "the TX CQ" "this kext has no r4tx scalars -- it predates stage 6; reboot with the staged kext" ;;
                0)
                    if [ "$cqready" = "0" ]; then
                        ok "TX CQ: SW2HW_CQ accepted (fw status 0x0) and the fresh ring reads as empty"
                    else
                        bad "r4tx: the TX CQ reads as non-empty" "readable slots=$cqready on a queue with no completions -- a slot whose owner byte reads as the consumer's phase is a completion out of unwritten memory. The kernel log and 'cx3ctl r4tx' print the same number from the same scan, with the CQN"
                    fi ;;
                *) bad "r4tx: the TX CQ" "fw status=$cqstat -- SW2HW_CQ was refused; the kernel log prints the context it sent (logsize_usrpage, comp_eqn, mtt_base_addr, db_rec_addr)" ;;
            esac
            # The number-space judgement, and the two values it was computed from.  A
            # reader who wants to see WHY the line is what it is has the log's arithmetic;
            # what the verifier asserts is the relation, not the constant.
            #
            # Only one of those two values is still a scalar: the QPN.  The CQN and the line
            # itself (num_qps - (1 << log_max_macs)) moved into the structure in 0.43.10 when
            # the scalar block was found to be full at the kernel's 16 (docs/DEFECTS.md F13),
            # so the PASS line no longer restates them -- it names where they are, and the
            # verdict it decides on (`kR4TxOutNumberSafe`, the conjunction of the four
            # comparisons the kernel made) is unchanged.
            qpn=$(jget "$j" "$O_R4TXQPN")
            if [ "$(jget "$j" "$O_R4TXSAFE")" = "1" ]; then
                ok "TX numbers: QPN $qpn and the CQN are both below the top-reserved line and collide with nothing that exists (the line, the CQN and their arithmetic: 'cx3ctl r4tx')"
            else
                bad "r4tx: the number space" "numberSafe=$(jget "$j" "$O_R4TXSAFE") -- QPN $qpn. The top slots are kept empty on purpose (A0 maps MAC index i to base_qpn + i and the plan puts that base at the line), so a number at or above it, or one that collides with the RX QP / indirection QP / RX CQ, blocks the non-promiscuous route. 'cx3ctl r4tx' prints the CQN, the line and all four comparisons; the kernel log prints them beside the values they came from"
            fi
            case "$qpn" in
                NO-*|PARSE-ERROR|NO-INDEX) : ;;
                0) bad "r4tx: the QPN" "the readback reports 0, which is a firmware QP" ;;
            esac
            # Two counts in one scalar: stepsAccepted low, statesConfirmed high.  The WHOLE
            # value is CX3_R4TX_NOT_MEASURED when the loop never ran, so the sentinel is tested
            # before the split -- splitting first would read 0xffffffff as 4294967295 accepted
            # commands and 0 confirmed ones, i.e. as two measurements (docs/DEFECTS.md D10).
            ss=$(jget "$j" "$O_R4TXSTEPSTATES")
            case "$ss" in
                NO-*|PARSE-ERROR) steps="$ss"; states="$ss" ;;
                4294967295)       steps=4294967295; states=4294967295 ;;
                *) steps=$(( ss & 0xffffffff )); states=$(( ss >> O_R4TXSTATESSHIFT )) ;;
            esac
            if [ "$steps" = "3" ] && [ "$states" = "3" ]; then
                ok "TX QP layer 1: 3/3 commands accepted, 3/3 states read back after each step"
            else
                bad "TX QP layer 1" "accepted=$steps/3 statesConfirmed=$states/3 (one scalar, steps low / states high, shift $O_R4TXSTATESSHIFT) -- a command returning 0 and a state that was read back are two different facts; the kernel log prints the per-step fw status and what QUERY_QP said after each one. For this stage check sq_size_stride (0x42, not the RX 0x02) and the mailbox offset"
            fi
            if [ "$(jget "$j" "$O_R4TXQRYOK")" != "1" ]; then
                bad "TX QP layer 2" "the final QUERY_QP did not answer, so what the firmware holds is unknown"
            elif [ "$(jget "$j" "$O_R4TXQRYST")" != "3" ]; then
                bad "TX QP layer 2" "the send QP is in state $(jget "$j" "$O_R4TXQRYST"), not RTS(3)"
            elif [ "$(jget "$j" "$O_R4TXQPNOK")" != "1" ]; then
                bad "TX QP layer 2" "the firmware's local_qpn is not the number that was written -- this is not the QP this call built"
            elif [ "$(jget "$j" "$O_R4TXFIELDS")" != "1" ]; then
                bad "TX QP layer 2" "RTS, but the context is not what was written (mask $(jget "$j" "$O_R4TXFIELDS")) -- the kernel log prints written/read side by side and names which of the eight failed"
            else
                ok "TX QP layer 2: RTS, local_qpn $qpn confirmed, and all eight compared fields match the write"
            fi
            # One of those eight is a value that cannot be wrong in the same way the others
            # can, and saying so here is the fix for the shape this project keeps recording
            # (an accurate but incomplete PASS line): rq_size_stride is 0 in the sent context
            # because the fill does not write it for a send QP, so the comparison is 0 against
            # the firmware's 0.  It is worth asserting -- it says no receive geometry is
            # described -- but it is not the same kind of evidence as sq_size_stride 0x42,
            # which is a value this driver chose and the firmware had to keep.
            echo "           (of those eight, rq_size_stride is 0 sent / 0 read: a send QP"
            echo "           describes no receive geometry, so that pair is a statement about"
            echo "           what is NOT claimed, not a value the firmware had to preserve."
            echo "           The kernel log prints all eight written/read pairs.)"
            if [ "$(jget "$j" "$O_R4TXQRYOK")" = "1" ] &&
               [ "$(jget "$j" "$O_R4TXMTT")" != "1" ]; then
                bad "TX QP layer 2 (the MTT)" "the send queue's MTT address did not survive: it is written for RST->INIT only and copied through the other two, so a difference means the shared context was disturbed between steps. The kernel log prints sent/read"
            fi
            # sq_size_stride: the stage's signature byte, asserted against the value the
            # driver read out of the context image it sent -- which is derived from
            # ilog2(entries) and ilog2(stride) rather than fixed, so the expectation here is
            # the one the plan names (256 TXBBs of 64 bytes) for the default call.
            if [ "$(jget "$j" "$O_R4TXSQSTRIDE")" = "66" ]; then
                ok "TX SQ: sq_size_stride 0x42 (256 TXBBs of 64 bytes), the byte as sent"
            else
                bad "r4tx: sq_size_stride" "read $(jget "$j" "$O_R4TXSQSTRIDE"), expected 66 (0x42) for the default 256 TXBBs of 64 bytes. The RX QP carries 0x02 here and the RQ 0x40; a wrong byte here reaches RTS and is silent everywhere else"
            fi
            # A separate line rather than a clause on the one above: the two facts come from
            # different places (the context image / the queue's own memory) and a single line
            # claiming both would be the accurate-but-incomplete shape this project records
            # (docs/DEFECTS.md F10) -- it would print "the stamp is in the memory" on a run
            # whose stamp check failed.
            #
            # What the scalar says changed in 0.43.6 (docs/DEFECTS.md C12): it is the
            # conjunction of "the 64-byte header carries the transmit stamp" AND "every one of
            # the WQE TXBBs carries it", because stage 6 stamped the header with the RECEIVE
            # ring's marker and left the array all zero -- and an all-zero array is "the
            # hardware owns every slot", i.e. every slot looks in flight to stage 9's reclaim
            # scan.  The kernel and the tool print both parts.
            if [ "$(jget "$j" "$O_R4TXSTAMP")" = "1" ]; then
                ok "TX SQ: every 64-byte TXBB's first dword reads back as 0xffffffff (STAMP_VAL | 1 << 31, 'software owns'), out of the memory itself"
            else
                bad "r4tx: the SQ ownership stamps" "not every 64-byte TXBB's first dword read back as 0xffffffff (STAMP_VAL | 1 << 31 = 'software owns'). Zero there says bit 31 = 0 = 'hardware owns', which is the state docs/DEFECTS.md C12 records: every slot would look in flight to the reclaim scan. The kernel log and 'cx3ctl r4tx' print WQE 0's value and how many of the WQE TXBBs were stamped"
            fi
            # -----------------------------------------------------------------------
            # pri_path.feup -- the context byte 0.43.9 started writing, judged as TWO facts
            # because they are measured in two places: what this driver PUT in the context
            # image (a fact about the mailbox) and what the firmware KEPT (a fact about the
            # device, from QUERY_QP).  docs/DEFECTS.md E14 is the row for the field having
            # been missing entirely.
            #
            # Why a send queue needs a bit a receive queue does not: the reference's fill sets
            # MLX4_FEUP_FORCE_ETH_UP only when `user_prio >= 0`, its transmit caller passes a
            # queue index >= 0 (mlx4_en_tx.c:233-234 with mlx4_en_netdev.c:1362-1363) and BOTH
            # receive-side callers pass -1 (mlx4_en_rx.c:946 and :1057).  The port's receive QP
            # copied the -1 instantiation and works on this card; its send QP copied the same
            # fill, which is the instantiation that gets the bit.  See MLX4_FEUP_FORCE_ETH_UP in
            # src/mlx4_regs.h for the other three implementations that set it and for the
            # falsification condition.
            #
            # The readback's DROPPED outcome (written 0x40, read back 0x00) is a SKIP rather
            # than a FAIL on purpose: nothing about this driver is wrong in that case -- the
            # byte was written -- and the honest reading is that the experiment has ended, so
            # the sentence says where to look next instead of sending a reader back to the fill.
            # -----------------------------------------------------------------------
            case "$(jget "$j" "$O_R4TXFEUPSENT")" in
                NO-*|PARSE-ERROR) bad "r4tx: pri_path.feup" "no usable value" ;;
                64) ok "TX QP: pri_path.feup was written as 0x40 (MLX4_FEUP_FORCE_ETH_UP) in the context image -- the bit the reference sets for a send queue and NOT for a receive one" ;;
                4294967295) skip "TX QP: pri_path.feup" "the call refused before the fill ran, so there is no context image and the field was never written" ;;
                *)  bad "r4tx: pri_path.feup" "the byte in the context image is $(jget "$j" "$O_R4TXFEUPSENT"), expected 64 (0x40 = MLX4_FEUP_FORCE_ETH_UP). 0 means the fill's userPrio >= 0 branch did not run for this QP; any other value means the write landed in the wrong byte of pri_path, where it is at QPC+0x3a -- between sched_queue (0x38) and fvl_rx (0x3b), with vlan_index (0x39) in between. tools/test-cqe-offsets.c derives that offset from a compiler mirror of the reference's declaration and asserts it, so a wrong constant is a build failure rather than this" ;;
            esac
            # The verdict comes from the equal-bytes scalar and the VALUE is printed beside it,
            # so the two cannot travel apart: that is the rule docs/DEFECTS.md F7 records after
            # this project twice let a value carry its own verdict.
            feup_qry=$(jget "$j" "$O_R4TXFEUPQRY")
            case "$(jget "$j" "$O_R4TXFEUPSURV")" in
                NO-*|PARSE-ERROR) bad "r4tx: pri_path.feup readback" "no usable value" ;;
                4294967295) skip "TX QP: pri_path.feup readback" "QUERY_QP did not answer, so what the firmware holds for this field is UNKNOWN. Reported as the not-measured sentinel rather than as a zero, because 'the firmware said 0' and 'the firmware said nothing' are different answers (docs/DEFECTS.md D10)" ;;
                1) ok "TX QP: QUERY_QP reads pri_path.feup back as 0x40 (value $feup_qry) -- the firmware KEPT the byte, so a transmit that still does nothing is NOT explained by the context it holds" ;;
                0) if [ "$feup_qry" = "0" ]; then
                       skip "TX QP: pri_path.feup readback" "QUERY_QP answered 0x00 for QPC+0x3a although the context image carries 0x40: the firmware DROPPED the byte, exactly as stage 4 measured it doing for param3 (0x20000000 written, 0x0 read back). Nothing is wrong with this build -- the byte was written and the fill's branch did run -- and this line is where the experiment ends: this field is not the gate, so do not spend another run on it. Take the BlueFlame note in src/mlx4_regs.h as the live candidate (mlx4_en on CX-3 has bf_reg_size > 0 and so always takes the BlueFlame branch, mlx4_en_tx.c:157-166 and :890, which means this port's memory+doorbell path may be one the shipping driver never exercises)"
                   else
                       bad "r4tx: pri_path.feup readback" "QUERY_QP answered $feup_qry for QPC+0x3a, which is neither 0x40 (what was written) nor 0x00. A third value in a byte the fill writes one of two values into usually means the OFFSET is wrong -- the byte is between sched_queue (0x38) and fvl_rx (0x3b), with vlan_index (0x39) in between -- and the kernel log prints the written and the read bytes side by side"
                   fi ;;
                *) bad "r4tx: pri_path.feup readback" "the equal-bytes scalar is $(jget "$j" "$O_R4TXFEUPSURV") and the readback is $feup_qry: neither a verdict nor the not-measured sentinel" ;;
            esac
            echo "           and NOTHING WAS SENT: this stage posts no WQE and rings no"
            echo "           doorbell, so these lines are about resources that exist and"
            echo "           numbers that are where they were put. Frames are stage 9."
        fi
    fi

    # -----------------------------------------------------------------------
    # r4raw -- R4 stage 9: THE FIRST STAGE WHERE A BYTE ACTUALLY MOVES.
    #
    # THIS STEP PUTS FRAMES ON THE WIRE.  It is the first thing in this chain whose effect
    # leaves the machine, and it is here rather than behind a flag because the stage's whole
    # claim is about the wire: one 60-byte broadcast frame per post, ethertype 0x88B5 (IEEE
    # Local Experimental 1) with this driver's own payload pattern, so nothing on the segment
    # interprets it and nothing is queried.  ARP is stage 10.
    #
    # The six judgements, each from a scalar and each failing in its own words:
    #
    #   the WQE      the posted descriptor, read back OUT of the queue's memory, decodes to
    #                what the encoder intended: the opcode in the LOW byte of the big-endian
    #                word (shifted by 24 it becomes NOP -- the doorbell rings, a completion
    #                can still arrive, and nothing is transmitted), the owner bit equal to the
    #                lap phase, byte_count 60, the lkey, the frame's address.
    #   the TX CQE   one arrived, its opcode is not MLX4_CQE_OPCODE_ERROR, and its wqe_index
    #                is the TXBB that was posted.  "No completion" and "a completion in
    #                error" are different answers and are reported apart.
    #   the wrap     more than two laps of the ring, owner bit read back equal to the lap phase
    #                on every lap covered, and BOTH owner values observed.  A ring that only
    #                ever turned once cannot tell a correct phase from a constant, which is
    #                what docs/DEFECTS.md C12 was.
    #   the frames   at least one completion, and every completion naming THIS driver's RX
    #                QPN.  WHEN NOT ONE ARRIVES, the first thing to suspect is stage 7's
    #                mcast/base_qpn -- under A0 the firmware sends broadcast where that
    #                field says (docs/EXTERNAL-REFS.md section 13) -- and NOT "stage 8's
    #                filter is not configured": no filter command is involved in A0.
    #   the content  a frame of at least the minimum size, read out of the buffer its CQE
    #                named, with the bytes copied out for the log.
    #   the refill   every completion's slot re-posted -- the slot being the one the ring
    #                position predicts, and one that was in flight -- and then the ring
    #                turned twice.  On a quiet segment the second half will be short, which
    #                is a statement about the traffic rather than about the refill, so it is
    #                reported as SKIP with that sentence in it.
    #
    # WHAT 0.43.7 MEASURED HERE, and why the FAIL texts below read the way they do: the receive
    # half passed completely (frames from a live LAN, every completion naming our RX QPN, the
    # bytes read out of the buffers) and the transmit half produced NOT ONE completion from ONE
    # post.  The deciding experiment was external -- `tcpdump` on the other interface of the
    # same LAN while the chain ran -- and it saw no frame at all, which separates "the hardware
    # never executed the descriptor" from "the descriptor ran and its completion was lost".
    # The cause was the send queue's 64-byte header: WQE 0 was written at MTT base + 64 while
    # the hardware reads the base, where the driver had stamped 0xffffffff ("software owns",
    # i.e. not a descriptor) -- so there was nothing to execute.  Fixed in 0.43.8 by removing
    # the header (SQ_WQE0_OFFSET, mlx4_en_tx.c:139/:686; docs/DEFECTS.md E13, and section 12 of
    # docs/R4-STAGE9-RESULT.md carries the run).  A zero-completion result from THIS build is
    # therefore a different question than it was from that one.
    #
    # -----------------------------------------------------------------------
    if ! j=$(runcall "$SEL_R4RAW" 0 0); then
        bad "r4raw" "the call failed: $(callinfo)"
        skip "the raw send/receive path" "the selector could not be called"
    else
        v=$(jget "$j" "$O_R4RAWSTAT")
        if [ "$v" != "0" ]; then
            if [ "$v" = "$IORETURN_NOTREADY" ]; then
                blk "r4raw" "status=$v (kIOReturnNotReady): a receive-side prerequisite is missing (the RX CQ of stage 2, the ring of stage 3, the RX QP of stage 4, or the published producer record), so there is nothing to observe at all -- BLOCKED rather than failed. The kernel log names which one"
            else
                bad "r4raw" "status=$v -- see the kernel log (a refusal before anything was sent)"
            fi
            skip "the raw send/receive path" "nothing was sent"
        else
            rxqpn=$(jget "$j" "$O_R4RAWRXQPN")
            frames=$(jget "$j" "$O_R4RAWRXFRAMES")
            txref=$(jget "$j" "$O_R4RAWTXREF")
            txwrap=$(jget "$j" "$O_R4RAWTXWRAP")
            txlaps=$(jget "$j" "$O_R4RAWTXLAPS")

            case "$txref" in
                NO-*|PARSE-ERROR) bad "r4raw: the transmit half" "no usable value" ;;
                1) blk "r4raw: the transmit half" "REFUSED: the send side is not built in this boot -- r4tx has to run first. The receive half below ran anyway, which is why this is BLOCKED and not a FAIL: the two halves fail for different reasons and neither may hide the other" ;;
                2) blk "r4raw: the transmit half" "REFUSED: no MAC address is registered (stage 5), so the frame would have to invent a source, and a fabricated source is a worse test frame than none. The receive half below ran anyway" ;;
                3) bad "r4raw: the transmit half" "REFUSED: the one-page frame buffer could not be allocated -- this call's own failure (no memory), not a missing prerequisite" ;;
                *) : ;;
            esac
            if [ "$txref" = "0" ]; then
                case "$(jget "$j" "$O_R4RAWTXWQE")" in
                    NO-*|PARSE-ERROR) bad "r4raw: the TX WQE" "no usable value" ;;
                    1) ok "TX WQE: every posted descriptor ($(jget "$j" "$O_R4RAWTXPOSTS") post(s)) reads back as encoded -- opcode 0x0a in the low byte, owner bit per lap, byte_count 60" ;;
                    0) bad "r4raw: the TX WQE" "the descriptor in memory does not decode to what the encoder intended, and the transmit phase stopped BEFORE the doorbell. The kernel log prints its 32 bytes and every field it compared" ;;
                    *) skip "the TX WQE" "the transmit half did not reach the point of posting" ;;
                esac
                if [ "$(jget "$j" "$O_R4RAWTXCQES")" = "0" ]; then
                    bad "r4raw: the TX completion" "NOT ONE completion arrived although $(jget "$j" "$O_R4RAWTXPOSTS") post(s) were made; the kernel log prints the doorbell's value and offset, so a rung doorbell with no completion after it means the descriptor was not executed. Read the WQE's fields first (its 32 bytes are in the log), then the TX CQ's comp_eqn. This is NOT a completion in error -- that produces a CQE whose opcode says so. AND SEPARATE THE TWO CASES FROM OUTSIDE, because no scalar here can: run 'tcpdump -i <other-if> -e -n -c 50 \"ether proto 0x88b5\"' while the chain runs. No frame on the wire means the descriptor never ran (the 0.43.7 failure, docs/DEFECTS.md E13 -- the send queue's 64-byte header is gone in 0.43.8, so if it still happens look at SQ_WQE0_OFFSET, the doorbell offset and comp_eqn); a frame on the wire with no completion means the descriptor DID run and only the completion went somewhere nobody read"
                elif [ "$(jget "$j" "$O_R4RAWTXERR")" != "0" ]; then
                    bad "r4raw: the TX completion" "$(jget "$j" "$O_R4RAWTXERR") completion(s) came back in ERROR (MLX4_CQE_OPCODE_ERROR). The device is SAYING the descriptor was bad: read the WQE's fields and the memory region the descriptor's address has to lie in (QUERY_MPT's start/length are in the kernel log). A doorbell that never arrived would have produced no completion at all, so this is not that"
                elif [ "$(jget "$j" "$O_R4RAWTXCQEOK")" = "1" ]; then
                    ok "TX completion: $(jget "$j" "$O_R4RAWTXCQES") arrived, not in error, and the first names the TXBB that was posted"
                else
                    bad "r4raw: the TX completion" "$(jget "$j" "$O_R4RAWTXCQES") completion(s) arrived and the first is not well formed: its opcode is the error value, or its bit 6 does not mark a send completion, or its wqe_index is not the TXBB that was posted. The log prints the CQE's 32 bytes and the decoded fields"
                fi
                case "$txwrap" in
                    1) ok "TX wrap: past two laps of the ring, the owner bit reading back as the lap phase on all $txlaps lap(s) and both owner values seen" ;;
                    0) bad "r4raw: the TX wrap" "the wrap was attempted and not proved: posts=$(jget "$j" "$O_R4RAWTXPOSTS") (more than twice the ring's TXBBs is wanted), owner-laps-ok=$txlaps. The kernel log prints the producer, the laps covered and which owner values were observed" ;;
                    *) skip "the TX wrap" "the wrap never started: either the caller asked for a single post, or the first post's completion did not arrive" ;;
                esac
            fi

            if [ "$frames" = "0" ]; then
                bad "r4raw: the receive side" "NOT ONE FRAME arrived at RX QPN $rxqpn. The FIRST thing to suspect is stage 7's mcast/base_qpn: under A0 the firmware decides where broadcast and multicast go, and the mcast field of SET_PORT qpn_calc is what names the QP (docs/EXTERNAL-REFS.md section 13). This is NOT 'stage 8's filter is not configured' -- A0 sends a broadcast to the QP that field names and no filter command is involved. After that: was the producer record published (stage 4), and is the link up (m3portup)?"
            elif [ "$(jget "$j" "$O_R4RAWRXQPNOK")" = "1" ]; then
                ok "RX frames: $frames completion(s), every one naming OUR RX QPN $rxqpn"
            else
                bad "r4raw: the RX QPN" "the completions do not all name RX QPN $rxqpn (verdict $(jget "$j" "$O_R4RAWRXQPNOK")): frames are being delivered to a different queue. The kernel log prints the first CQE's 32 bytes and its decoded qpn, so the number that arrived is visible"
            fi
            rxmin=$(jget "$j" "$O_R4RAWRXMIN")
            if [ "$rxmin" = "4294967295" ] || [ "$rxmin" = "NO-INDEX" ]; then
                skip "the RX content" "no non-error completion arrived, so there is no frame size to judge (4294967295 is the driver's 'never measured', which is not the same as a measured zero -- docs/DEFECTS.md D10)"
            elif [ "$(jget "$j" "$O_R4RAWRXCONTENT")" = "1" ]; then
                ok "RX content: a frame of at least the minimum size was read out of the buffer its CQE named (byte_cnt >= 60; the kernel log prints the frame's bytes)"
            else
                bad "r4raw: the RX content" "the smallest byte_cnt seen was $rxmin, below the 60-byte minimum frame, so no usable frame was read out of a buffer. A completion that small usually means the slot named by wqe_index is not the slot the frame landed in -- the kernel log prints both numbers"
            fi
            rxrec=$(jget "$j" "$O_R4RAWRXRECYCLE")
            rxlaps=$(jget "$j" "$O_R4RAWRXLAPS")
            case "$rxrec" in
                NO-*|PARSE-ERROR) bad "r4raw: the refill" "no usable value" ;;
                0) bad "r4raw: the refill" "a completion's slot was refilled wrongly: not the slot the ring position predicts, or not a slot that was in flight (double occupancy: a buffer handed back while a frame could still land in it), or the producer could not be published. The kernel log names the slot and the consumer index, and the refill STOPPED there rather than continue on a guess" ;;
                4294967295) skip "the refill" "nothing was refilled, so there is no refill to judge -- see the receive line above for why no frame arrived" ;;
                1)
                    if [ "$rxlaps" -ge 2 ] 2>/dev/null; then
                        ok "RX refill: every completion's slot re-posted and the ring turned $rxlaps whole laps (every slot refilled at least twice)"
                    else
                        skip "the RX refill (two laps)" "every refill it made was correct, but the ring has turned only $rxlaps whole lap(s): two laps means every slot refilled twice, and the segment did not deliver that many frames inside the budget. Raise it with 'cx3ctl r4raw 0 <ms>'. If frames DID arrive and the ring still did not turn, look at stage 7's mcast/base_qpn first"
                    fi ;;
            esac
            echo "           the frames this step sent are on the wire: 60-byte broadcasts with"
            echo "           ethertype 0x88B5, which nothing on a LAN interprets. One frame is"
            echo "           posted at a time and its completion consumed before the next, so at"
            echo "           most one descriptor is ever in flight."
        fi
    fi

    # -----------------------------------------------------------------------
    # The three discriminators (docs/M2B-NOTES.md section 52).
    #
    # r4raw has said the same thing for four builds -- one post, no completion, no frame -- and
    # by now every field of the descriptor and every way of delivering the doorbell has either
    # been checked against the reference or falsified on the card.  So this asks the two
    # questions the fields cannot: is this send queue executed AT ALL (a NOP carries no data and
    # still completes, so the pair NOP/SEND moves exactly one variable), and does an MMIO
    # doorbell on the UAR page the QPs were built with reach the device (the receive CQ's arm,
    # judged by the COMP EQE the hardware leaves in the host-memory event ring -- no interrupt
    # session needed).
    #
    # It sends no firmware command, so it cannot poison the mailbox and cannot leave the device
    # needing a reset.  That is what makes it safe to run repeatedly against a live stack, which
    # is how the doorbell's page and width were ruled out before this selector existed.
    # -----------------------------------------------------------------------
    step "is the send queue executed at all, and does the doorbell reach the device"

    if skip_step txprobe; then
        skip "the probe" "CX3_SKIP names txprobe. This is an experiment, not a prerequisite for the interface step behind it, and it has not been run on this machine yet -- so it is skipped rather than allowed to stop the run that is trying to attach an interface. Run the chain again without CX3_SKIP to exercise it (a second session in the same boot works since 0.44.21)"
    elif ! j=$(runcall "$SEL_TXPROBE"); then
        skip "the probe" "the running kext has no txprobe selector ($(callinfo)); reboot with the staged kext and re-run"
    elif [ "$(jget "$j" "$O_TPREFUSED")" != "$N_TPRAN" ]; then
        blk "the probe" "refused (code $(jget "$j" "$O_TPREFUSED")): the transmit side is not built in this boot, so this says nothing about the hardware. r4tx has to run first -- it is upstream in this same chain"
    else
        tp_nop=$(jget "$j" "$O_TPNOPCOMPS")
        tp_nopwqe=$(jget "$j" "$O_TPNOPWQEOK")
        tp_send=$(jget "$j" "$O_TPSENDCOMPS")

        # The experiment is void if the descriptor it posted is not in the queue: a NOP that was
        # never written, or one whose opcode byte landed somewhere else, would answer "no
        # completion" for a reason that has nothing to do with the hardware (this project's
        # F-family: a check that measures the wrong thing reads exactly like a real answer).
        if [ "$tp_nopwqe" != "1" ]; then
            bad "the probe: the NOP descriptor" "the 32 bytes in the send queue do not read back as a NOP with the owner bit handed over (nopWqeOk=$tp_nopwqe), so the experiment says nothing about the hardware. 'cx3ctl txprobe' prints the same number from the same readback"
        elif [ "$tp_nop" = "$N_TPNOTRUN" ]; then
            skip "the probe: the NOP" "not measured"
        elif [ "$tp_nop" != "0" ]; then
            ok "the probe: the hardware EXECUTES this send queue -- a NOP produced $tp_nop completion(s) (CQE opcode $(jget "$j" "$O_TPNOPOPCODE"), status $(jget "$j" "$O_TPNOPSTATUS")). The queue is fetched, the descriptor is where it has to be, and the completion finds its way back to the CQ. The defect is in what a SEND does with its descriptor, not in whether this queue is read"
        else
            bad "the probe: the send queue is not executed" "a NOP produced no completion, and neither did SEND ($tp_send), while the descriptor itself was verified present with the owner bit handed over. So the hardware is not executing descriptors from this queue at all -- and every field INSIDE the descriptor has by now been excluded (docs/M2B-NOTES.md 51.4). What is left is the queue's state as the hardware sees it, which the arm result below separates from 'the doorbell never arrived'"
        fi

        # The arm is independent of the send queue and is the only thing that can tell "the
        # doorbell never lands" from "the doorbell lands and this queue says no".
        if [ "$(jget "$j" "$O_TPARMRAN")" != "1" ]; then
            skip "the probe: the arm doorbell" "the receive CQ or the event queue was not built in this boot, so this half was skipped. That is a statement about which stages have run, not about the hardware"
        elif [ "$(jget "$j" "$O_TPARMOK")" = "1" ]; then
            ok "the probe: an MMIO doorbell on the page the QPs were built with REACHES the device -- arming the receive CQ produced its own COMP event (type $(jget "$j" "$O_TPARMTYPE") = 0x00, device.h:293, naming CQN $(jget "$j" "$O_TPARMCQN")). A send that still does nothing is therefore NOT explained by the doorbell's address, its page or its width, and docs/M2B-NOTES.md 51.3's one limitation is closed"
        elif [ "$(jget "$j" "$O_TPARMFOUND")" = "1" ]; then
            bad "the probe: the arm doorbell" "an EQE arrived but it is not this arm's own event: type $(jget "$j" "$O_TPARMTYPE"), CQN $(jget "$j" "$O_TPARMCQN"), where this arm was for the receive CQ and type 0 (COMP). An event naming another queue is a different answer, not a silent one -- the kernel log prints it with the CQN it had to name"
        else
            # NOT a finding, and 0.43.12 is why.  This experiment exists to answer "does an MMIO
            # doorbell reach the device"; that question is now answered by the transmit itself,
            # which is doorbell-driven and completes (the TX completion line above), so the
            # doorbell path works.  The arm's own silence is therefore a statement about the
            # experiment: mlx4 arms a queue the driver has just EMPTIED, and this one never was
            # -- it holds dozens of unconsumed completions on a live LAN.  Draining it to make
            # the test valid would leak receive slots for the rest of the boot and sabotage the
            # M4 gate, so the measurement is reported and not judged.
            skip "the probe: the arm doorbell" "no EQE in the budget (armRan=1, armEqeFound=0) -- NOT a device statement: mlx4's arm is used with a queue the driver has just drained, and this one was never empty. The doorbell path itself is now answered by the transmit above, which is doorbell-driven and completes"
        fi

        rec_var=$(jget "$j" "$O_TPRECVAR")
        rec_win=$(jget "$j" "$O_TPRECWIN")
        if [ "$rec_var" = "$N_TPNOTRUN" ]; then
            skip "the probe: the send doorbell record" "not measured"
        elif [ "$rec_win" != "0" ]; then
            # The winner is stored as variant + 1, so variant 0 (the plain post with no record
            # written, which is a variant like any other) is 1 rather than 0.
            if [ "$rec_win" = "1" ]; then
                ok "the probe: the SEND post completed with NO doorbell record written (variant 0), which is the reading this port has always used -- so the record is not needed for this queue, and the public PRM's posting step 2 is about something else. All four readings are not needed to say that: this one answered first"
            else
                ok "the probe: the send doorbell record IS read by the hardware -- variant $((rec_win - 1)) (1 = record word 0, 2 = word 1, 3 = both) produced a completion where the readings before it did not, so the send half is the word this names"
            fi
        elif [ "$rec_var" = "4" ]; then
            ok "the probe: all four readings of the send doorbell record were tried and none of them produced a completion -- so the record is not what stops this queue, whatever the PRM's posting procedure intends it for"
        else
            bad "the probe: the doorbell record" "$rec_var variant(s) were tried, no winner, and the loop should have tried four. See the kernel log"
        fi
    fi

    # -----------------------------------------------------------------------
    # Stage 10a: does the card become an interface the system can actually use?
    #
    # The first step in this chain that is about the NETWORK STACK rather than about the
    # device, and the gate everything above M4 waits behind -- the soak below cannot exist
    # until an interface does.  Three things are checked, and they are different failures:
    #
    #   1. did the selector attach an interface at all (the family's own log line says why not);
    #   2. is it in the system with the CARD's address -- the netup outputs carry the address
    #      the firmware reported, and `ifconfig` carries the address the system is using, so
    #      the two are compared rather than asserted to agree;
    #   3. does `ifconfig <if> up` succeed.  That is the only thing that proves the controller's
    #      enable() callback was reached and answered success -- and it is the callback whose
    #      absence (the base class answers kIOReturnUnsupported) leaves a perfectly attached
    #      interface that can never come up.  Before 0.44.6 that is exactly where this would
    #      have failed, and nothing in this chain would have said so.
    # -----------------------------------------------------------------------
    # -----------------------------------------------------------------------
    # The arm experiment, and it has to be here: it needs the receive CQ and the
    # event queue to exist (the receive stages above built them), the interrupt
    # session to be CLOSED (its action would consume the very EQE being waited for),
    # and the network data path to be OFF (the interface has not been brought up
    # yet at this point in the chain).  All three hold exactly here.
    #
    # Its own semantics decide the judgement, and that is the whole point of the
    # selector: arming a queue that is not empty is the experiment stage 9 ran, and
    # its answer meant nothing.  So the precondition is reported first, and only a
    # run with the queue empty is allowed to conclude anything.
    # -----------------------------------------------------------------------
    step "does the CQ arm reach the device (drain, then arm)"

    if skip_step arm; then
        skip "the arm experiment" "CX3_SKIP names arm. This is an experiment, not a prerequisite for the interface step behind it, and it has not been run on this machine yet -- so it is skipped rather than allowed to stop the run that is trying to attach an interface. Run the chain again without CX3_SKIP to exercise it (a second session in the same boot works since 0.44.21)"
    elif ! j=$(runcall "$SEL_NETARM" 2000); then
        skip "the arm experiment" "the running kext has no netarm selector ($(callinfo)); reboot with the staged kext and re-run"
    elif [ "$(jget "$j" "$O_ARMREFUSED")" != "$N_ARMRAN" ]; then
        blk "the arm experiment" "refused (code $(jget "$j" "$O_ARMREFUSED")): the receive CQ or the event queue is not built, or an interrupt session is still open, or the interface is already up. That is a statement about the state of this boot, not about the hardware -- verify it by running the same command by hand"
    elif [ "$(jget "$j" "$O_ARMEQEOK")" = "1" ]; then
        ok "the arm reaches the device: after draining $(jget "$j" "$O_ARMDRAINED") completion(s) (queue empty: $(jget "$j" "$O_ARMCQEMPTY")), a COMP event naming this CQ arrived. The last unproved link in the receive chain -- CQ arm -> COMP EQE -> event queue -- is now proved, which is what interrupt-driven receive was waiting for"
    elif [ "$(jget "$j" "$O_ARMCQEMPTY")" != "1" ]; then
        blk "the arm experiment" "the receive CQ was NOT empty when the arm was rung ($(jget "$j" "$O_ARMDRAINED") completion(s) drained and more waiting), so this run says nothing about the arm -- it is the same invalid experiment stage 9 ran. Re-run when the segment is quieter (or raise the drain bound)"
    elif [ "$(jget "$j" "$O_ARMEQEFOUND")" = "1" ]; then
        bad "the arm experiment" "an EQE arrived but it is not this arm's own event: type $(jget "$j" "$O_ARMEQTYPE") (0x00 is COMP), CQN $(jget "$j" "$O_ARMEQCQN"). A different answer, not a silent one"
    else
        bad "the CQ arm" "the queue WAS empty, the arm was rung, and no event arrived within the budget. That is a real reading -- with the precondition satisfied -- that the arm doorbell does not produce an event on this path. Before concluding, check that a completion actually landed after the arm (cx3ctl netstat and the 'net rx:' lines)"
    fi

    # The transmit-checksum switch has to be set BEFORE the interface is attached, because the
    # family asks getChecksumSupport() once, at attach (see kCx3SelNetOff's note).  It is opt-in
    # through the environment on purpose: the default run is the one M4 was designed against,
    # where the stack inserts every checksum itself.
    if [ -n "${CX3_TXCSUM:-}" ]; then
        if txc=$("$CTL" netoff txon 2>&1); then
            echo "  CX3_TXCSUM is set: transmit checksum offload switched on before the attach"
            printf '%s\n' "$txc" | sed 's/^/    /'
        else
            echo "  CX3_TXCSUM is set but 'netoff txon' failed; the attach will happen without it"
        fi
    fi

    step "the interface (10a)"

    if ! j=$(runcall "$SEL_NETUP"); then
        skip "the interface" "the running kext has no netup selector ($(callinfo)); reboot with the staged kext and re-run"
    else
        nref=$(jget "$j" "$O_NETREFUSED")
        if [ "$nref" = "$N_NETNODEV" ]; then
            blk "the interface" "refused (CX3_NETUP_NO_DEVICE): no port address is held, so nothing has brought this port up in this boot. Since 0.44.28 the interface step does not query the port itself -- the address comes from the port-up step's own QUERY_PORT (m3portup, or the standalone queryport selector) -- so this says 'no port-up step has run', which is a statement about which stages have run and not about the hardware"
        elif [ "$nref" = "$N_NETNOIFACE" ]; then
            bad "the interface" "the controller could not attach an interface (CX3_NETUP_NO_IFACE). The kernel log names which half failed: attachInterface itself, or the null pointer it may come back with. IOEthernetController::configureInterface() returning false fails attachInterface the same way, which is why the log line matters here"
        elif [ "$nref" != "$N_NETRAN" ]; then
            bad "the interface" "netup answered a refusal code this script does not know ($nref) -- see kNetUpOutRefused in src/cx3_abi.h"
        else
            nunit=$(jget "$j" "$O_NETUNIT")
            nif="en$nunit"
            NET_IF="$nif"
            # The driver's own half of the link comparison below.  The alias has been here since
            # the netup step was written and was never read, which is how a slot that reported a
            # hard 1 ("the link is up", whatever the port-up step measured) went unnoticed: the
            # verdict came from ifconfig, so nothing compared the two.
            njlu=$(jget "$j" "$O_NETLINKUP")

            if ifconfig "$nif" >/dev/null 2>&1; then
                ok "the interface exists: $nif is in the system, so attachInterface() attached it and the driver stack matched it"
            else
                bad "the interface exists" "netup reports unit $nunit but 'ifconfig $nif' does not know it -- the attach happened and matching did not complete (check the family's log lines around the attach)"
                echo "  NOTE: the address, the enable callback and the link state below are all read"
                echo "        from $nif, so they will fail too. That is one root cause, not four:"
                echo "        read this failure and the kernel log around the attach, not the ones"
                echo "        that follow it."
            fi

            # The address, compared rather than described.  printf %012x of a 48-bit value is
            # exact in awk's double-precision arithmetic (2^53 is the limit), and both sides are
            # lowercased because ifconfig's case is a display choice.
            nmac=$(awk -v v="$(jget "$j" "$O_NETMACBITS")" 'BEGIN { printf "%012x", v }')
            want=$(printf '%s' "$nmac" | sed 's/\(..\)/\1:/g; s/:$//')
            got=$(ifconfig "$nif" 2>/dev/null | awk '/ether/ { print $2; exit }')
            if [ -z "$got" ]; then
                bad "the interface's address" "'ifconfig $nif' printed no ether line, so the stack has no address for this interface (getHardwareAddress is what supplies it)"
            elif [ "$(printf '%s' "$got" | tr 'A-F' 'a-f')" = "$want" ]; then
                ok "the interface's address is the card's: $got, which is the def_mac the firmware reported for this port (not a locally administered fallback, and not a placeholder)"
            else
                bad "the interface's address" "ifconfig shows $got but the firmware's def_mac is $want -- the two have to be the same address, because the steering table this driver programmed matches on the second one"
            fi

            # The callback.  This is the assertion that would have failed before 0.44.6, and the
            # failure would have looked like "the interface exists but nothing works".
            if ifconfig "$nif" up 2>/dev/null; then
                ok "ifconfig $nif up succeeded -- the controller's enable() callback answered kIOReturnSuccess"
            else
                bad "ifconfig $nif up" "the interface refused to come up. That is the controller's enable() callback refusing (or not being reached): the family's own implementation answers kIOReturnUnsupported, so a driver that does not override it can never bring its interface up"
            fi

            nstat=$(ifconfig "$nif" 2>/dev/null | awk '/status:/ { print $2; exit }')
            case "$nstat" in
                active)
                    ok "the link is reported active ($(ifconfig "$nif" 2>/dev/null | awk '/media:/ { print $2; exit }')), which is the state m3portup measured and netup published" ;;
                inactive)
                    if [ "$njlu" = "1" ]; then
                        bad "the link state" "the interface is up and the driver's own reading (netup's link slot) says the port-up step measured the link UP, yet the family reports status: inactive. netup publishes VALID|ACTIVE from that reading, so the two cannot both be right: this is about how the status is published (a refused setLinkStatus says so in the kernel log), not about the cable"
                    elif [ "$njlu" = "0" ]; then
                        blk "the link state" "the interface is up but the link is inactive, and the driver's own reading agrees: the port-up step measured the link DOWN in this boot (netup publishes what that step measured; it does not re-query). Not a defect by itself -- either the cable/far end, or the port was never brought up in this boot"
                    else
                        blk "the link state" "the interface is up but the link is inactive, and netup's link slot answered '$njlu' -- neither 0 nor 1, so there is no driver-side reading to compare this against and the cause is undecided"
                    fi ;;
                "")
                    bad "the link state" "'ifconfig $nif' printed no status line at all, so the family is not reporting a link state for this interface -- setLinkStatus was either not called or refused" ;;
                *)
                    bad "the link state" "ifconfig reports status '$nstat', which is neither active nor inactive -- this script does not know that value and will not guess" ;;
            esac

            if [ "$(jget "$j" "$O_NETMACOK")" = "1" ]; then
                ok "getHardwareAddress answered with the firmware's address for this port, so the stack asked and was told"
            else
                blk "getHardwareAddress" "netup reported no address (its QUERY_PORT half did not answer), so nothing about the address below this line could be checked"
            fi
        fi
    fi

    # -----------------------------------------------------------------------
    # The M4 gate: a ping that comes back.
    #
    # This was BLOCKED for nine stages with "the interface attachment does not exist yet".
    # It is now a real test, and it is the only assertion in this chain that involves a second
    # machine (or at least a second device): everything else asks this driver or this card,
    # and this one asks the LAN whether a frame that left this card arrived and was answered.
    #
    # Three prerequisites are checked first and each says what it is, because "the ping failed"
    # with no interface, with an inactive link, and with no address are three different
    # findings that would otherwise all read as "the driver does not work":
    #
    #   1. there is an interface (the step above made it);
    #   2. its link is active -- which is the port-up measurement, republished by netup;
    #   3. it has an IPv4 address.  If it does not, DHCP is tried once (`ipconfig set ... DHCP`)
    #      because criterion 3 of the stage 10 plan is exactly "it gets an address from this
    #      LAN"; a lease that never arrives is reported as such rather than skipped.
    #
    # Then the ping itself, whose number is the gate.
    # -----------------------------------------------------------------------
    step "the M4 gate"

    soak_n=$SOAK_SHORT
    soak_what="$SOAK_SHORT consecutive pings, 0 loss"
    if [ "$soak" = "full" ]; then
        soak_n=$SOAK_FULL
        soak_what="$SOAK_FULL consecutive pings, 0 loss, 2 phase wraps"
    fi

    if [ -z "${NET_IF:-}" ]; then
        blk "soak: $soak_what" \
            "there is no interface to ping through: the interface step above refused or could not attach one (it says which). A ping with no interface would be a test of nothing"
    elif ! ifconfig "$NET_IF" >/dev/null 2>&1; then
        blk "soak: $soak_what" \
            "netup reported $NET_IF but ifconfig does not know it, so there is nothing to ping through"
    elif ! ifconfig "$NET_IF" 2>/dev/null | grep -q "status: active"; then
        blk "soak: $soak_what" \
            "$NET_IF exists but its link is not active, so a ping would test the cable and the far end rather than this driver. The link state is what m3portup measured; re-run the chain from the top"
    else
        soak_ip=$(ifconfig "$NET_IF" 2>/dev/null | awk '/inet / { print $2; exit }')
        if [ -z "$soak_ip" ]; then
            echo "  no IPv4 address on $NET_IF yet; asking this LAN for one (the stage 10 plan's"
            echo "  criterion 3 is exactly that). ipconfig set $NET_IF DHCP"
            ipconfig set "$NET_IF" DHCP >/dev/null 2>&1
            soak_wait=0
            while [ "$soak_wait" -lt 60 ]; do
                soak_ip=$(ifconfig "$NET_IF" 2>/dev/null | awk '/inet / { print $2; exit }')
                [ -n "$soak_ip" ] && break
                sleep 1
                soak_wait=$((soak_wait + 1))
            done
        fi
        if [ -z "$soak_ip" ]; then
            bad "soak: DHCP" \
                "$NET_IF never got an IPv4 address (60 s after 'ipconfig set ... DHCP'). This is a real finding about the data path: DHCP needs a broadcast request to leave this card and a reply to reach the stack through it. The kernel log has the driver's own frame counters (net rx: / net tx: / the once-a-second heartbeat) and they say which half is silent"
        else
            ok "the interface has an address: $soak_ip on $NET_IF"
            #
            # -b boundif, and it is not decoration.  This machine has a second Ethernet
            # controller (the RTL8125 port on en0) on the SAME subnet as $SOAK_TARGET, and an
            # unbound `ping` is routed by the routing table -- which is the other interface.
            # The gate would then be asking the LAN whether the RTL8125 works, and reporting
            # the answer as M4.  `-b` is an Apple addition to ping ("Bind the socket to
            # interface boundif for sending", man ping): with it, a 0% loss result means the
            # echo requests left THIS interface and the replies arrived at THIS interface's
            # address.  That is the claim the gate is supposed to make.
            #
            # The driver's own counters are then required to agree with the ping rather than
            # merely printed next to it.  A ping that succeeds while this driver's transmit
            # counter did not move is not a weaker PASS, it is a contradiction -- and it is
            # the exact shape this project keeps finding: a green result that does not cover
            # the requirement.
            #
            ns_before=$("$CTL" netstat 2>&1)
            tx_before=$(printf '%s\n' "$ns_before" | sed -n 's/.*transmit *\([0-9][0-9]*\) frame(s) posted.*/\1/p' | head -1)
            rx_before=$(printf '%s\n' "$ns_before" | sed -n 's/.*receive *\([0-9][0-9]*\) frame(s) handed to the stack.*/\1/p' | head -1)
            soak_out=$(ping -b "$NET_IF" -c "$soak_n" -i "$SOAK_INTERVAL" -t 3 "$SOAK_TARGET" 2>&1)
            soak_loss=$(printf '%s\n' "$soak_out" | sed -n 's/.*[^0-9]\([0-9][0-9]*\.[0-9]\)% packet loss.*/\1/p' | head -1)
            # macOS ping prints one statistics line: "N packets transmitted, M packets received,
            # X% packet loss".  Both numbers are read from it, and each pattern anchors on the
            # words rather than on a field position, because a missing field would otherwise
            # silently read the wrong number.
            #
            # Read from the START of the line, and that is not a style choice.  The statistics
            # line begins with the transmitted count ("3 packets transmitted, 3 packets
            # received, 0.0% packet loss"), so a pattern of the form `.*[^0-9]\([0-9][0-9]*\)
            # packets transmitted` cannot match it: `[^0-9]` has nothing to consume before a
            # digit that is at the beginning of the line.  Measured 2026-10-13 against a real
            # ping: the loss and received patterns matched and this one returned EMPTY -- and
            # empty is the dangerous answer here, because the comparison below falls back to
            # `${soak_sent:-1}` and would then have required only ">= 1 frame left the driver"
            # instead of ">= N", i.e. a silently weaker gate that still printed a PASS.
            #
            soak_sent=$(printf '%s\n' "$soak_out" | sed -n 's/^[^0-9]*\([0-9][0-9]*\) packets transmitted.*/\1/p' | head -1)
            soak_recv=$(printf '%s\n' "$soak_out" | sed -n 's/.*, *\([0-9][0-9]*\) packets received.*/\1/p' | head -1)
            # The driver's own view, asked for rather than read out of a log window.  It is
            # printed in both outcomes: on success it is the evidence that the frames went
            # through THIS driver rather than through some other interface that happens to
            # answer, and on failure it is what separates "nothing arrived" from "nothing was
            # sent" from "the stack refused what it was handed".
            echo
            echo "  --- the data path's own counters (cx3ctl netstat) ---"
            netstat_out=$("$CTL" netstat 2>&1)
            tx_after=$(printf '%s\n' "$netstat_out" | sed -n 's/.*transmit *\([0-9][0-9]*\) frame(s) posted.*/\1/p' | head -1)
            rx_after=$(printf '%s\n' "$netstat_out" | sed -n 's/.*receive *\([0-9][0-9]*\) frame(s) handed to the stack.*/\1/p' | head -1)
            if [ -n "$tx_before" ] && [ -n "$tx_after" ] && [ -n "$rx_before" ] && [ -n "$rx_after" ]; then
                tx_delta=$((tx_after - tx_before))
                rx_delta=$((rx_after - rx_before))
                printf '  during the soak: transmit +%s posted (of %s), receive +%s delivered (of %s),\n' \
                    "$tx_delta" "$tx_after" "$rx_delta" "$rx_after"
                printf '                   ping says %s transmitted / %s received\n' \
                    "${soak_sent:-?}" "${soak_recv:-?}"
            else
                tx_delta=""; rx_delta=""
                echo "  the counters could not be read from 'cx3ctl netstat' (one of its two"
                echo "  lines was not in the expected shape), so this run cannot say whether the"
                echo "  frames went through this driver at all"
            fi
            printf '%s\n' "$netstat_out" | sed 's/^/  /'
            if [ "$soak_loss" = "0.0" ]; then
                if [ -z "$soak_sent" ] || [ -z "$soak_recv" ]; then
                    bad "soak: ping's own statistics line" "the loss figure parsed as 0.0% but the transmitted/received counts did not ('${soak_sent:-empty}' / '${soak_recv:-empty}'), so how many frames this driver was supposed to carry is unknown. Without that number the comparison below would silently weaken itself to '>= 1 frame', which is the failure this check exists to avoid. ping's line is normally 'N packets transmitted, M packets received, X% packet loss'; what it printed follows: $(printf '%s' "$soak_out" | tr '\n' ' ')"
                elif [ -z "$tx_delta" ]; then
                    bad "soak: the counters" "$soak_n ping(s) to $SOAK_TARGET lost 0%, but the driver's own counters could not be read, so 'the frames went through this driver' is unproven. Read the netstat block above: the two lines this reads are 'transmit ... frame(s) posted' and 'receive ... frame(s) handed to the stack'"
                elif [ "$tx_delta" -lt "$soak_sent" ] || [ "$rx_delta" -lt "$soak_recv" ]; then
                    bad "soak: the counters disagree with the ping" "ping through $NET_IF (-b $NET_IF) reports $soak_sent transmitted / $soak_recv received with 0% loss, but this driver posted only +$tx_delta frame(s) and delivered only +$rx_delta. Both cannot be true: the frames did not travel through this driver. Do not read this as M4 met -- find out which interface carried them (netstat -rn, ifconfig) before trusting anything the ping said"
                else
                    ok "soak: $soak_n ping(s) to $SOAK_TARGET through $NET_IF (-b), 0% loss, and the driver's own counters agree (+$tx_delta posted, +$rx_delta delivered) -- M4 IS MET: frames left this card, arrived, were answered, and the replies reached the stack through it"
                fi
            elif [ -n "$soak_loss" ]; then
                bad "soak: $soak_n ping(s) to $SOAK_TARGET" \
                    "$soak_loss% lost ($soak_recv of ${soak_sent:-?} received), bound to $NET_IF. The driver's own counters for the same window: +$tx_delta posted, +$rx_delta delivered. Read them before reading the ping: 'post +0' means outputPacket never put a frame on the ring (and the kernel log's 'net tx:' lines say why), 'post >0, delivered +0' means the frames left but nothing came back, and 'delivered >0' with the ping still failing means the replies arrived and the stack answered them through some other path. The kernel log's once-a-second heartbeat separates these"
            else
                bad "soak: $soak_n ping(s) to $SOAK_TARGET" \
                    "ping printed nothing this script could read as a loss figure, so the gate cannot be judged from it. Its own output follows: $(printf '%s' "$soak_out" | tr '\n' ' ')"
            fi
        fi
    fi

    # -----------------------------------------------------------------------
    # The command timeout and the transaction lock (docs/LOCK-IMPL.md).
    #
    # Last, deliberately: the injection below ends with a reset, and this chain *starts*
    # with a reset, so the next run begins from a state this one saw.
    #
    # Every assertion here is a wall-clock bound or a value.  Nothing asserts that a log line
    # exists -- the two failures being guarded against were both invisible to prose: one
    # selector held the lock for 6 min 07.6 s, and a stuck command register made every later
    # command wait 22.7 s.  "The log says the new code is running" is exactly the kind of
    # evidence this file exists to replace.
    #
    # The poison state is read from probe's poison bit, a scalar, on purpose.  survey carries
    # the same facts, but in a structure, and a harness that decodes its own ABI structures is
    # one more place to be wrong -- this script has four F-family defects behind it, one of
    # which was a hard-coded size that produced a false PASS.
    # -----------------------------------------------------------------------
    if [ "$leaveUp" = "leaveup" ]; then
        skip "the command timeout and the transaction lock" \
            "--up: that section ends with an HCA reset, and this mode exists to hand a LIVE stack to peek/poke"
        echo
        echo "  The stack is up and nothing was reset.  Every object the chain builds -- the MR,"
        echo "  both CQs, the RX QP, the TX QP with its posted WQE, the ingress QP and the port --"
        echo "  is still attached, so 'cx3ctl peek 1 <off> <n>' and 'cx3ctl poke 1 <off> <val>' now"
        echo "  address a live device.  That is the only way to change one variable (here: which UAR"
        echo "  page the send doorbell is rung through) without a new kext and a reboot."
        echo "  When done, put the device back into a known state with 'cx3ctl reset 1 1'."
        return 0
    fi
    step "the command timeout and the transaction lock (docs/LOCK-IMPL.md)"

    if ! j=$(runcall "$SEL_PROBE"); then
        bad "probe (poison bit)" "the call failed: $(callinfo)"
        return 1
    fi
    v=$(jget "$j" "$O_PROBEWEDGED")
    case "$v" in
        NO-INDEX|NO-OUT|PARSE-ERROR)
            skip "the command-timeout state" "this kext has no kProbeOutCmdWedged, so it is older than the change; reboot with the staged kext" ;;
        0) ok "the command mailbox is not poisoned" ;;
        *) bad "the command mailbox is poisoned before anything was injected" "wedged=$v -- run 'cx3ctl reset force' and re-run this chain" ;;
    esac

    # Is the injector there at all?  An unknown mode is refused inside the selector and the
    # call itself still succeeds, so asking this question cannot change any state.  A refused
    # *call* means the running kext predates this selector -- a stale kext, not a defect.
    if ! j=$(runcall "$SEL_FAULT" 9 0); then
        skip "the rest of this step" "the running kext has no fault selector ($(callinfo)); reboot with the staged kext and re-run"
        echo
        echo "  Nothing was injected, so the device is left in the state this run brought it to."
        return 0
    fi
    if [ "$(jget "$j" "$O_FAULTSTATUS")" != "0" ]; then
        ok "fault: the selector exists and refuses an unknown mode"
    else
        bad "fault" "an unknown mode was accepted, so the selector does not validate its input"
    fi

    if [ "$safe" = "1" ]; then
        skip "the injection sequence" "--safe was requested: it poisons the mailbox and ends with a reset, and this mode exists to leave the device alone"
        return 0
    fi

    if ! j=$(runcall "$SEL_FAULT" 1 0); then
        bad "fault wedge" "the call failed: $(callinfo)"
        return 1
    fi
    if [ "$(jget "$j" "$O_FAULTWEDGED")" = "1" ]; then
        ok "fault wedge: the mailbox reports itself poisoned"
    else
        bad "fault wedge" "wedged=$(jget "$j" "$O_FAULTWEDGED") -- the injection did not take"
        return 1
    fi

    # A command must now fail at once, and it must say WHY: 0xfffffffb is "not sent".
    # 0xfffffffd would mean it was posted and timed out, which is the one thing the poison
    # exists to prevent.
    t1=$(ms_now); j=$(runcall "$SEL_QUERYFW" 1) || j=""; t2=$(ms_now)
    s=$(jget "$j" "$O_FWSTAT")
    if [ -z "$j" ]; then
        bad "a command against a poisoned mailbox" "the call itself failed: $(callinfo)"
    elif [ "$s" = "4294967291" ]; then
        if [ "$((t2 - t1))" -lt 2000 ]; then
            ok "QUERY_FW refused in $((t2 - t1)) ms with fw status 0xfffffffb (not sent)"
        else
            bad "a poisoned command took $((t2 - t1)) ms" "it must fail in microseconds, so the poison is not being checked before the command register is read"
        fi
    else
        bad "QUERY_FW against a poisoned mailbox" "fw status=$s, expected 4294967291 (0xfffffffb = not sent)"
    fi

    # The read-only selectors have to stay answerable while the lock is held.  The holder is
    # the injector itself in mode 3, which is the only way to reproduce "another selector is
    # using the mailbox" without breaking the card.
    load_sizes
    raw "$SEL_FAULT" in "3,3000" out "$(sz_field "$SEL_FAULT" 3)" >/dev/null 2>&1 &
    holdpid=$!
    sleep 1
    t1=$(ms_now); pout=$(raw "$SEL_PROBE" out "$(sz_field "$SEL_PROBE" 3)"); t2=$(ms_now)
    rc=$(call_rc "$pout")
    wait "$holdpid" 2>/dev/null
    if [ "$((t2 - t1))" -ge 1000 ]; then
        bad "probe while another selector holds the lock" "took $((t2 - t1)) ms, so it queued behind the lock instead of asking for it"
    elif [ "$rc" = "0" ]; then
        ok "probe answered a value in $((t2 - t1)) ms while the lock was held"
    elif [ "$rc" = "$IORETURN_BUSY" ]; then
        ok "probe refused with kIOReturnBusy in $((t2 - t1)) ms while the lock was held"
    else
        bad "probe while another selector holds the lock" "the call failed with rc=$rc, which is neither a value nor kIOReturnBusy ($IORETURN_BUSY)"
    fi

    # The reset is the cure, and it reports the state it arrived in.
    if ! j=$(runcall "$SEL_RESET" 1 1); then
        bad "reset (the cure)" "the call failed: $(callinfo)"
        return 1
    fi
    if [ "$(jget "$j" "$O_RSTSTAT")" = "0" ]; then ok "reset: status 0"
    else bad "reset (the cure)" "status=$(jget "$j" "$O_RSTSTAT")"; return 1; fi
    if [ "$(jget "$j" "$O_RSTWEDGED")" = "1" ]; then
        ok "reset: reports that the mailbox was poisoned before it ran"
    else
        bad "reset" "kRstOutCmdWedged=$(jget "$j" "$O_RSTWEDGED"), expected 1"
    fi

    if ! j=$(runcall "$SEL_PROBE"); then
        bad "probe after the reset" "the call failed: $(callinfo)"
        return 1
    fi
    if [ "$(jget "$j" "$O_PROBEWEDGED")" = "0" ]; then
        ok "the poison is gone after the reset, so the cure works"
    else
        bad "the poison survived the reset" "wedged=$(jget "$j" "$O_PROBEWEDGED")"
    fi

    echo
    echo "  The device is now reset.  That is deliberate: the point of this step is that a"
    echo "  reset clears the poison, and the next 'tools/verify.sh run' starts from a reset"
    echo "  anyway -- the same state the chain above begins in."
    return 0
}

do_coverage() {
    local f="$SRC/docs/COVERAGE.md"
    if [ -f "$f" ]; then cat "$f"
    else echo "docs/COVERAGE.md does not exist yet -- it is the next piece to write"; exit 1; fi
}

printf '\n[%s] === %s: start ===\n' "$(ts)" "$MODE"
case "$MODE" in
    stage)    need_root; do_stage ;;
    m5)       need_root; do_m5 ;;
    run|"")   need_root; do_run 0 short ;;
    full)     need_root; do_run 0 full ;;
    safe)     need_root; do_run 1 short ;;
    up)       need_root; do_run 0 short leaveup ;;
    coverage) do_coverage ;;
    *) echo "usage: verify.sh [stage|run|full|safe|up|m5|coverage]"; exit 2 ;;
esac
rc=$?   # kept: the summary below reports counts, and the exit code carries the verdict
printf '[%s] === %s: end, rc=%d ===\n' "$(ts)" "$MODE" "$rc"

echo
echo "============================================================"
printf '  PASS %d   FAIL %d   BLOCKED %d   SKIP %d\n' "$pass" "$fail" "$blocked" "$skipped"
echo "  log: $LOG"
if [ "$STALE_KEXT" = "1" ]; then
    echo "  VERDICT: INCONCLUSIVE -- the running kext is older than this tool, so nothing"
    echo "           below could be judged.  Reboot (see tools/verify.sh stage) and re-run."
    echo "           Nothing was asked of the device, so this is not a failure."
elif [ "$fail" -gt 0 ]; then
    echo "  VERDICT: FAIL -- something the device was asked to do did not happen."
elif [ "$blocked" -gt 0 ]; then
    echo "  VERDICT: the chain held as far as it is implemented. $blocked step(s) BLOCKED,"
    echo "           which means not yet reachable -- not failed."
else
    echo "  VERDICT: PASS"
fi
echo "============================================================"
[ "$STALE_KEXT" = "1" ] && exit 3
[ "$fail" -gt 0 ] && exit 1
exit 0
