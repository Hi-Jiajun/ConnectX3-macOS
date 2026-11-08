# Which selectors have actually been exercised, and on what evidence

`tools/verify.sh coverage` prints this file.  It exists because the project could not
answer a question that matters more as it ages: **"can this path still work?"**  A
selector that compiled is not a selector that ran, and the difference is invisible until
someone depends on it.  Of the selectors this file lists, **twenty-four** are exercised by the
verifier's own chain (the table below) and the rest are not -- several of those write to the
device.  (`r4a0port` below is not a selector: it is one command *inside* `m3portup`, and it is
listed where it runs rather than in a chain position of its own.)

The rule this table follows: **a row is only as strong as its evidence column.**  "No
record" is written where there is no record, rather than leaving the reader to assume a
path works because it exists.

## How to read the columns

| Column | Meaning |
| --- | --- |
| **writes** | Does it change device state?  `read` is safe to run any time; `write` needs the machine's state to be understood first |
| **last evidence** | Where a real run is recorded.  `M1` = `docs/M1-NOTES.md`, `M2B` = `docs/M2B-NOTES.md`, plus the date |
| **in verify** | Does `tools/verify.sh run` exercise it?  A selector outside the chain is not covered by the one-command entry point |

## The chain the verifier walks (in order)

Re-synced with `tools/verify.sh` at 0.43.10.  The order of the queue and port steps is
deliberate and is now the reference's own (`docs/EQ-ORDERING.md` section 5): the EQ is
created, its interrupt attached, the session opened, and only then is the port activated --
with the port brought up first, the event `INIT_PORT` produces fires an EQ that nothing is
listening to, and the interrupt never arrives again.

**0.43.9 added two criteria to the `r4tx` row** (`pri_path.feup`: the byte written in the
context image, and what `QUERY_QP` reads back for it).  They are separate lines because they
are measured in different places, and the readback one is deliberately allowed to be a SKIP:
if the firmware drops the byte, nothing this driver did is wrong and the sentence says which
candidate to take next (`docs/R4-STAGE9-RESULT.md` section 13.8,
`docs/DEFECTS.md` E14).  **But that batch's own run did not exercise them, and the reason is
this file's subject matter**: to fit the three new scalars it raised `CX3_SCALAR_OUT_MAX`
16 -> 20, and that number is the kernel's (`kIOExternalMethodScalarOutputCountMax`), not ours.
Every `r4tx` call on 0.43.9 returned `kIOReturnBadArgument` before reaching the driver, so the
`r4tx` row below describes criteria that had not been asked (`docs/DEFECTS.md` F13,
`docs/R4-STAGE9-RESULT.md` section 14).  **0.43.10** puts the cap back at 16 with the identity
asserted against the kernel's constant, keeps the three `feup` facts as three scalars, and makes
room by moving two context numbers (the CQN and the top-reserved line) into the structure and
packing the two layer-1 counts into one scalar losslessly.  No other selector's judgement
changed.

`r4raw` is last in this list and is the first row in this table whose step **changes the state
of the network**: it posts send WQEs and rings the send doorbell, so the frames it describes
leave the machine.  That is also why the row's own judgement is written in two independent
halves (transmit and receive) and why the tool says so before it runs.  It has now run three
times (2026-10-12, 09:22:50 / 09:24:43 / 09:25:34 on 0.43.7): **the receive half passed every
time and the transmit half sent nothing**, with the cause found and fixed in 0.43.8
(`docs/DEFECTS.md` E13, `docs/R4-STAGE9-RESULT.md` section 12) -- so this chain's last row is,
today, the one place where a PASS is not yet expected on the first try.

| Selector | writes | last evidence | in verify |
| --- | --- | --- | --- |
| `probe` | read | M1 + every sequence script since | yes |
| `survey` | read | every `verify.sh run` since it joined the chain -- 2026-10-12 06:19 on 0.43.2 reported 28 entries, no inconsistency | yes |
| `reset` | **write** | every sequence script | yes |
| `queryfw` | read (writes the HCR, reads the answer) | every sequence script | yes |
| `fwload` | **write** | every sequence script | yes |
| `m2dprofile` | read | M2B, 2026-10-11 | yes |
| `m2dbringup` | **write** | M2B §14, 2026-10-11 | yes |
| `queryhca` | read | M2B §26, 2026-10-11 | yes (moved in with 0.43.0) |
| `m3equp` | **write** | M2B §15/§16, 2026-10-11 | yes |
| `m3clrint` | read + maps a register | M2B §17, 2026-10-11 | yes |
| `m3irqup` | **write** (enables an interrupt source) | M2B §17, 2026-10-11 | yes |
| `m3portup` | **write** (`SET_PORT` general + `qpn_calc` + `INIT_PORT`) | M2B §21, twice in one boot | yes (moved in with 0.43.0; now runs *after* the interrupt session) |
| `r4a0port` (inside `m3portup`) | **write** (the `SET_PORT` `qpn_calc` sub-command: it tells the firmware where A0's MAC-derived QPN range starts -- `base_qpn` = this port's RX QPN, `promisc` 0, `m_promisc` from the device's `VEP_MC_STEER` bit) | **2026-10-12 08:40 on 0.43.6** -- `fw status 0x0`: the firmware **accepted base 64**, which resolves the falsification condition written in `docs/M2B-NOTES.md` section 41 (a `BAD_PARAM`/`BAD_RESOURCE` would have proved it validates the base against a region of its own, and the base would have had to move).  The cost is measured and logged rather than implicit: slot 0 is QPN 64 and slots 1..127 reach 65..191, inside this driver's own numbering, so **one MAC address** is what this configuration supports.  `docs/R4-STAGE7-RESULT.md`; the step counter's value (5) was not directly observed -- `verify.sh` does not print it, and the standalone call gates out legitimately because the chain ends with a reset | yes (it is the middle step of `m3portup`) |
| `queryport` | read | M2B §21, 2026-10-11 | yes -- it reads back what `m3portup` wrote |
| `m3irqtest` | **write** (posts a command) | M2B §17, 2026-10-11 | yes |
| `m3irqstatus` | read | M2B §17, 2026-10-11 | yes, twice: once after `m3portup` (does the port's own asynchronous event show up?) and once after `m3irqtest` |
| `m3irqdown` | **write** (disables and releases) | M2B §17, 2026-10-11 | yes |
| `r4mr` | **write** (SW2HW_MPT) | M2B §29, 2026-10-11 | yes |
| `r4cq` | **write** (SW2HW_CQ) | M2B §32, 2026-10-11 | yes |
| `r4rq` | **write** (memory only, nothing handed over) | M2B §34, 2026-10-11 | yes |
| `r4qp` | **write** (three QP state transitions, then a 4-byte write into host memory) | M2B-adjacent: `docs/R4-STAGE4-RESULT.md` §9, **2026-10-12 06:19 on 0.43.2** -- 3/3 commands `fw status 0x0`, 3/3 states confirmed by `QUERY_QP`, the QP in RTS with `local_qpn` 64, the eight compared fields matching, and the producer read back as 256 out of host memory. Two further fields (`param3`, `sched_queue`) come back different and are reported rather than judged -- see §9.1 for what that does and does not mean | yes |
| `r4ingress` | **write** (`SET_PORT` carrying the whole 1024-byte MAC table, then three indirection-QP state transitions and a host-memory write) | **2026-10-12 07:16 on 0.43.3** -- the second half passed all four layers (QPN 65 in RTS, `default_qpn` and `base_qpn` both 64 = the RX QP of stage 4, the overlay at QPC+0x3c byte for byte, the QP not pointing at itself), and **the MAC half failed twice, both from one defect**: the driver wrote the address into slot 0 and then scanned that table for "the first empty slot", which returned 1, so the command carried the address in slot 1 -- a different QPN under A0 -- while the log and the scalar both said 1 and the code's comment said 0 (`docs/DEFECTS.md` D12).  The second failure was the verifier's own: `fw status 0x0` with an all-zero outbox was reported as "the firmware holds a DIFFERENT table", a claim nothing measured.  Both are fixed in 0.43.4 -- the table is now passed to the hardware layer whole and the slot is measured out of it, and an unanswered-or-all-zero read-back is SKIP, not FAIL.  `docs/R4-STAGE5-RESULT.md` §10 carries the run and §11 what the MAC index→QPN mapping leaves to stage 7 | yes (after `r4qp`) |
| `r4tx` | **write** (`SW2HW_CQ` for a second CQ, three send-QP state transitions, and the send queue's memory allocated -- **no frame is posted and no doorbell is rung**) | **2026-10-12 09:22:48 on 0.43.7, in the chain (three times: 09:22:48 / 09:24:41 / 09:25:32)** -- identical every time, from the kernel log: `r4tx OK: CQN 129 (fw status 0x0, 0 readable slot(s) on an empty ring) and QPN 66 in RTS. Commands accepted 3/3, states confirmed by QUERY_QP 3/3, the readback's eight fields all match, the MTT address survived, local_qpn confirmed; sq_size_stride 0x42, rq_size_stride 0x00, the dummy TXBB's stamp reads back as STAMP_VAL\|1<<31 (0xffffffff: SW owns), the WQE array holds no descriptor bytes`.  So every one of the six judgements this row listed as unrun has now been observed on the card -- *including* the ownership stamps, which is worth noting because that check passed while the queue itself could not send anything: it measures what this driver wrote, not what the hardware read (that lesson is `docs/DEFECTS.md` E13, and it is why the judgement is stated as a conjunction).  **Changed in 0.43.6** (`docs/DEFECTS.md` C12): that last judgement was "the dummy TXBB's first dword is `1 << 31`" and it checked only the 64-byte header, while the WQE array was left all zero -- which says "the hardware owns every slot" (bit 31 = 0) and would make stage 9 read every slot as in flight.  The memory carries `0xffffffff` (`STAMP_VAL \| 1 << 31`, "software owns") in every 64-byte unit, and since 0.43.8 every unit IS a WQE slot: the 64-byte header in front of the array was removed (`docs/DEFECTS.md` E13), so the judgement is "all `sqNent` WQE TXBBs are stamped" with `sqWqeNonZeroBytes` counting bytes outside those stamps  **Added in 0.43.9**: `pri_path.feup` at QPC `0x3a`, written as `0x40` (`MLX4_FEUP_FORCE_ETH_UP`) for this send queue only -- the reference's shared fill sets it when `user_prio >= 0`, its transmit caller passes a queue index and both receive-side callers pass `-1` (`mlx4_en_resources.c:62-65`, `mlx4_en_tx.c:233-234` vs `mlx4_en_rx.c:946`/`:1057`), and this port had never written the byte at all (`docs/DEFECTS.md` E14, `docs/R4-STAGE9-RESULT.md` section 13).  It is judged as two lines -- the byte written and what `QUERY_QP` reads back -- and the readback is allowed to be a SKIP, because a firmware that drops the byte (as it measurably drops `param3`) ends this line of investigation rather than failing this build. | yes (after `r4ingress`) |
| `r4raw` | **write, and it leaves the machine** -- posts send WQEs, rings the send doorbell, writes receive WQEs back into the ring and republishes its producer.  The frames are 60-byte broadcasts with ethertype `0x88B5` (IEEE Local Experimental 1) | **2026-10-12 (three calls: 09:22:50, 09:24:43, 09:25:34) on 0.43.7: the RECEIVE half passed, the TRANSMIT half sent nothing -- and the fix for that ships in 0.43.8, so the transmit half is the one judgement in this chain that has NOT yet been observed green.**  Receive, identical in all three calls: 51 / 60 / 49 completions, **every one naming this driver's RX QPN 64**, the bytes read out of the buffer each CQE named -- and the frames were real ARP requests off a live LAN (`who-has 192.0.2.1 tell 192.0.2.252`, `byte_cnt` 68 = a 64-byte frame + the 4-byte FCS that `FCS_KEEP` keeps).  Transmit, identical in all three: `1 post(s), 0 completion(s) (0 in error, 1 timed out)` -- no completion, **no error**, and `tcpdump` on the same LAN saw no frame at all, which is what separates "the descriptor never ran" from "it ran and its completion was lost".  Cause: the send queue carried the receive ring's 64-byte header, so WQE 0 was written at MTT base + 64 (`sqWriteSendWqe: WQE 0 at SQ+0x40`) while the hardware reads the base, where the driver had stamped `0xffffffff` ("software owns" = not a descriptor).  `docs/DEFECTS.md` E13, `docs/R4-STAGE9-RESULT.md` section 12.  **0.43.8 removes that header** (`SQ_WQE0_OFFSET` 0, asserted as different from the receive ring's 64 by `tools/test-cqe-offsets.c`, now 137 checks), and `tools/verify.sh`'s zero-completion FAIL now names the `tcpdump` experiment to run alongside.  What the stage claims is still the six judgements: the posted WQE reads back as encoded (opcode in the LOW byte of the big-endian word, owner bit equal to the lap phase, `byte_count` 60, the lkey, the frame's address); a TX completion arrived, not in error, and names the TXBB that was posted; the ring wrapped past two laps with the owner bit alternating; completions arrived at *this* driver's RX QPN; a frame of at least the minimum size was read out of the buffer its CQE named; and every completion's slot was refilled with the ring turning twice.  A quiet segment makes the last of those SKIP rather than FAIL, and the SKIP text names stage 7's `mcast`/`base_qpn` as the first thing to check when no frame arrives at all (docs/EXTERNAL-REFS.md section 13) -- the receive half's own evidence says that path is working | yes (after `r4tx`) |
| `fault` | **write** (poisons the mailbox, holds the transaction lock) | `docs/LOCK-IMPL.md`, 2026-10-11 | yes, last -- it ends with a reset |

**Correction to the `r4tx` row (2026-10-12, 0.43.10).**  Its 0.43.9 text
describes two `pri_path.feup` criteria as added and judged.  They were never asked: in 0.43.9
the selector declared 19 scalar outputs where the kernel's array is 16
(`kIOExternalMethodScalarOutputCountMax`), so **every** `r4tx` call returned
`kIOReturnBadArgument` (`0xe00002c2`) before the driver ran, and the batch's `feup` change was
never executed.  The 0.43.7 evidence in the row above is unaffected (that build's width was 16).
0.43.10 puts the width back at 16 with the cap asserted against the kernel's constant, keeps the
three `feup` facts as three scalars, and makes room by moving the CQN and the top-reserved line
into the structure and packing the two layer-1 counts into one scalar.  `docs/DEFECTS.md` F13,
`docs/R4-STAGE9-RESULT.md` section 14, `docs/M2B-NOTES.md` section 48.

## Verified, but outside the chain

These ran and their results are recorded, but the verifier does not exercise them -- so a
regression here would not be caught by the one-command entry point.

| Selector | writes | last evidence | in verify |
| --- | --- | --- | --- |
| `m3abring` | **write** (`SW2HW_EQ` on an experiment ring) | M2B §34, 2026-10-11 | no -- `m3-abring-*.sh` |
| `m3eqpoll` | read + posts a command | M2B §15, 2026-10-11 | no |
| `m3eqinfo` | read (MTT readback + QUERY_EQ) | M2B §16, 2026-10-11; **the "MATCH"/"DOES NOT MATCH" per-page reading was a false alarm until 0.43.1** (`docs/DEFECTS.md` F7), and the structure output grew from 160 to 192 bytes, so the tool and the kext must be installed together | no -- run it by hand |
| `m3eqring` | **write** (fills the ring and uses it as an outbox) | M2B §15.9, 2026-10-11 | **no, and deliberately** -- it writes 0xa5 into a ring, so it must never run during an experiment that is measuring who writes the ring |
| `querydevcap` | read | M2B §5, 2026-10-11 | yes (moved in with 0.43.0) |
| `fwarea` | read | M2B §5, 2026-10-11 | no |
| `fwoutbox` | read | M2B §8, 2026-10-11 | no |
| `icmalloc` | **write** (host allocation only, no device access) | M2B §8, 2026-10-11 | no |

## Run once, during M1, and not since

These were used to explore the device on 2026-10-10 and are recorded in `M1-NOTES.md`.  They
are all **write** capabilities except `peek`.  Nothing has exercised them since, so their
current validity is a reasonable expectation rather than a measurement.

| Selector | writes | last evidence | in verify |
| --- | --- | --- | --- |
| `peek` | read | M1, 2026-10-10 | no.  Worth noting for a different reason: the register-side-effect defect (`M2B` §9 -- one read of `0xf03fc` after reset stopped `RUN_FW` from ever completing) went through this command, and `peek` records nothing about the addresses it read |
| `poke` | **write** (a single guarded store) | M1, 2026-10-10 | no |
| `flr` | **write** (PCIe function-level reset) | M1, 2026-10-10 | no |
| `clearowner` | **write** | M1, 2026-10-10 | no |
| `dma` | **write** (allocates the command mailbox) | M1, 2026-10-10 | no |

## No record at all

| Selector | writes | last evidence | in verify |
| --- | --- | --- | --- |
| `hcr` | **write, arbitrary** | **none -- added 0.40.0, gated on `cx3raw=1`, never run** | no |
| `survey` | read | every `verify.sh run` since it joined the chain -- 2026-10-12 06:19 on 0.43.2 reported 28 entries, no inconsistency | yes (first in the chain) |
| `all` | composes probe + dma + queryfw | none | no -- it is a convenience wrapper, not a test |
| `icmfree` | **write** (frees the ICM areas) | none | no |
| `txprobe` (38) | **write** (posts a 32-byte NOP and a SEND into the send queue, rings the send doorbell; the arm half writes a doorbell record and one MMIO doorbell for the receive CQ) | **2026-10-13 01:11 on 0.44.1** (`outputs/verify-20261013-011049.log:7` is that boot's own version check; the results are the kernel's `txprobe 1/3`/`2/3`/`3/3` lines at 01:11:33-36). It answered both of its questions: a NOP produced a completion (CQE opcode `0x1e`, status `0x0000`), so the hardware DOES fetch and execute this send queue; and of the four doorbell-record variants **variant 0 won** -- the plain SEND with NO record written completed (1 tried, winner 1, readback matched), which is why `outputPacket` writes no record. The arm half produced no CQ event in 2000 ms, which is the case its own step reports as SKIP and not as a device statement | yes (its own chain step) |
| `netup` (39) | **write** (attaches an interface, reads the port through `QUERY_PORT`) | **none -- added 0.44.6, never run** | yes (the interface step) |
| `netstat` (40) | read (counters only) | **none -- added 0.44.10** | no -- a readout, not a test; `verify.sh m5` prints it at the end |
| `netlink` (41) | **write** (publishes a media dictionary; `requery` sends `QUERY_PORT`) | **none -- added 0.44.11** | yes (`verify.sh m5`) |
| `netoff` (42) | **write** (flips per-frame behaviour; nothing is sent to the device) | **none -- added 0.44.12/0.44.13** | yes (`verify.sh m5`) |
| `netarm` (43) | **write** (consumes receive completions, re-posts their slots, arms the CQ) | **none -- added 0.44.14** | yes (its own chain step) |
| `netfilt` (44) | **write** (port commands: the multicast table; `qpn_calc` for promiscuous) | **none -- added 0.44.15/0.44.16** | yes (`verify.sh m5`) |
| `netport` (45) | **write** (`CLOSE_PORT`) | **none -- added 0.44.17** | no -- it is the first half of a recipe, not a test of its own |

**The seven rows above are the batch this table exists for.**  Every one of them is in the
installed kext and NONE has ever run on hardware: the boot that carries them is the first test
they will ever have.  That is not a caveat to the M4 result -- M4's own verdict comes from the
chain, and the switches are OFF by default -- but a reader must not take "it is in the driver"
for "it works", and the column that says so is this one.

**M4's own criterion was tightened in 0.44.22** (docs/DEFECTS.md F21), and it changes what a PASS
will mean from now on, so it belongs next to the rows above:

* the soak pings with `-b <our interface>`.  This machine has a second Ethernet controller on the
  same subnet (en0), so an unbound ping was routed by the routing table -- the gate could have
  reported "M4 IS MET" from the RTL8125's answer whether or not this card worked;
* a 0% loss result must now also show up in the driver's own counters (posted >= transmitted,
  delivered >= received).  A ping that succeeded while this driver's counters did not move is
  reported as a **contradiction**, not as a weaker PASS, because the frames would have had to
  travel through something else while the verdict said "through this card";
* the decision block was extracted from `tools/verify.sh` by line range and run against five
  controlled readings (counters agree / counters do not move / total loss / loss without counts /
  counters unreadable) before being trusted.  The second of those five is the false PASS above and
  it now reports FAIL.

## What this table is for

Three concrete uses, all of which this project needed and did not have:

1. **Before relying on a path, check its row.**  `hcr` and `survey` have never run, and
   their rows say so; a claim about either is a claim about the code, not about a measured
   run, until the row gets a date.  `r4qp` had the same status for the four hours between
   0.43.2's build and its first run, which is what that row's earlier text was for -- it
   now carries the date (2026-10-12 06:19) and the measured values.

2. **The verifier's gaps are visible.**  The three that were named here as "should be moved
   in" -- `queryhca`, `queryport`, `querydevcap` -- went in with 0.43.0 and 0.43.1, and the
   rows now say so.  What remains outside the chain is what cannot go in: `m3eqring` writes
   0xa5 into the ring it is measuring, `m3abring` needs the EQ number free, `m3eqpoll`
   consumes ring slots, and `hcr` needs the `cx3raw=1` boot argument.

3. **It is the place to record the next run.**  When a selector is exercised, the row gets
   a new date and a pointer to where the output is.  Otherwise this file rots, which would
   make it worse than nothing.

4. **And it is the place to record a run that did NOT happen** -- see the correction note
   printed immediately after the chain table above, which is where the `r4tx` row's 0.43.9
   `pri_path.feup` criteria are now marked as criteria rather than as a measurement (they were
   never asked: the selector could not be called at all in that build, `docs/DEFECTS.md` F13,
   `docs/R4-STAGE9-RESULT.md` section 14).  A row that gains criteria has to gain a date in
   the same edit, or it is a promise rather than evidence.
