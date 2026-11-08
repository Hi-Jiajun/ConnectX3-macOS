# Development log

This repository's git history was squashed before publication, because the 238
development commits carried the reference machine's MAC and LAN addresses in
every snapshot. The commit subjects are preserved here, newest first, so the
shape of the work is still readable.

The per-task evidence lives in `docs/` and `docs/reports/`; the per-batch
Chinese notes are in [CHANGELOG-zh.md](CHANGELOG-zh.md).

```
2026-11-09  Publish: bring the README up to .139, redact identifiers, drop the debug tree
2026-11-09  docs: the full-adaptation programme, capability matrix and per-task reports
2026-11-09  tools: runtime capture, selector table checks, and per-feature test harnesses
2026-11-09  0.44.124-139: safe stop, jumbo, VLAN and checksum, plus the unbuilt WAVE4 link worker
2026-10-21  Prove the work-loop hand-off before attaching
2026-10-21  Run attachInterface on the controller's work loop
2026-10-21  Instrument the CX3 user-client open path
2026-10-21  Harden deferred attach worker and phase watcher
2026-10-21  Make the phase watcher prove it read the registry
2026-10-21  Name the user-client phase before the one-second death
2026-10-13  The hook marker goes to the registry too, where a one-second death cannot outrun it
2026-10-13  What other people's Mellanox-on-macOS drivers look like, and what this machine's IOMMU posture is
2026-10-13  The three hooks inside the family's attach, so the next death names one
2026-10-13  The watch pushes with a socket write, not a one-second nc timeout
2026-10-13  Every watch sample is kept, so a death is not written as a repeat
2026-10-13  The cut-short selector takes a stop point, so one boot can bisect four steps
2026-10-13  registerService is void in this SDK, and the build said so
2026-10-13  The interface is registered when it is ready, as the family's own documentation says
2026-10-13  The step counter moves to the registry, where a held lock cannot hide it
2026-10-13  A step counter that a non-blocking selector publishes, for an off-machine poller
2026-10-13  A cut-short interface step, and a breadcrumb channel that does not need the log to work
2026-10-13  A capped heartbeat carries what the register answered, not just that time passed
2026-10-13  The interface step reads the port-up step's own address instead of querying again
2026-10-13  The instrumentation goes inside the mailbox command, where the next death will name a register access
2026-10-13  Five steps inside netup, each one able to be the last line
2026-10-13  The session close hands back the queue and keeps the ring the stages after it name
2026-10-13  The comment follows the code it describes, and the transmit buffer names its overheads
2026-10-13  The coverage table gets the row for the selector that was measured at 01:11
2026-10-13  One re-post, not two: the receive refill the data path and the r4raw path share
2026-10-13  The two experiments between the rings and the interface can be stepped over
2026-10-13  The stack finally learns what this card can carry, from its own three bounds
2026-10-13  What the M4 gate now means, written where a reader checks what has actually run
2026-10-13  The M4 gate binds to the interface and judges the counters, and netup stops reporting a link it did not measure
2026-10-13  One command for the narrowest session experiment, and a clock for a log that has to survive a hard death
2026-10-13  The tee log gets a clock, and the scratch file stops needing root
2026-10-13  The session close hands the queue back, and a second session is possible again
2026-10-13  The freeze tracks the interrupt session, and that session can only be opened once per boot
2026-10-13  The dispatch table was shifted, and the run that found it
2026-10-13  The coverage table gets the seven selectors that have never run, and two recipes for what only the machine can settle
2026-10-13  A build-time guard for the selector table, and this batch's rows in the defect log
2026-10-13  One command for the M5 verification, and a harness defect that running it exposed
2026-10-13  The port can be stopped, and the transmit ring now follows the MTU
2026-10-13  Two new commands printed a report of zeros when the call had not run
2026-10-13  M5-8's other half: promiscuous mode, which under A0 is one bit of a command already sent
2026-10-13  M5-8: the multicast table, which on this card is the whole mechanism
2026-10-13  README: say that the PASS/FAIL counts are a measurement, not a property
2026-10-13  The README described a driver from nine stages ago
2026-10-13  The arm experiment, redone so that its answer means something
2026-10-13  M5-3 transmit side, and the one constraint that has to be said out loud
2026-10-13  M5-3 receive side: mark frames the hardware verified, off until asked
2026-10-13  M5-1: the media table and a link that can be re-read, reachable at runtime
2026-10-13  M5 is scoped: what macOS lets this kind of driver do, and one thing it does not
2026-10-13  M5 reference: what the Linux driver actually does, and three corrections to the plan
2026-10-13  M5 plan: what is left, how each item is judged, and the order it goes in
2026-10-13  netstat: retry only a busy read, and do not describe an older kext as transient
2026-10-13  The data path's counters were only readable from the kernel log
2026-10-13  inputPacket's return value was being counted as a delivery
2026-10-13  The transmit path needed its own lock, because the pull model is not available here
2026-10-13  The M4 gate is a ping now, and two more unsynchronised consumers
2026-10-13  Stage 10b, transmit half -- and one more static defect that a reboot would have hidden
2026-10-13  Stage 10b, receive half: frames reach the stack, and a timer polls the CQ to make that happen
2026-10-13  The chain now tests the interface, and the interface reports the address it was given
2026-10-13  10a had two static blockers that no reboot was needed to find
2026-10-13  canary sweep under the transaction lock, and the order of the first test after the reboot
2026-10-13  The reset path stops resting on an assumption: measure it, and stop handing the memory back
2026-10-13  Instrument one of the freeze investigation: a line at every selector entry
2026-10-13  HANG-AUDIT 6-8: the table of what is freed on reset, and the two ways to fix it
2026-10-13  HANG-AUDIT: the free-after-reset path is the one teardown that rests on an assumption
2026-10-13  CX3_NO_KLOG: run the chain without the kernel-log capture, to test a correlation
2026-10-13  CX3_STOP_AFTER: stop a run before a chosen step, which is the bisect handle
2026-10-13  getWorkLoop: the family's gate must run on our workloop, not a second one
2026-10-13  netup: let attachInterface create the interface, which is how it is done
2026-10-13  The panic was ours: clientClose() terminated the provider chain
2026-10-12  Stage 10a: the card becomes an interface, and the gate that was blocking it was wrong
2026-10-12  A run carries its own kernel log, which section 50 said it must
2026-10-12  Stage 10a's blocker is a header older than the kernel, and that is not a code problem
2026-10-12  On a symbol mismatch, print WHICH symbols could not resolve
2026-10-12  Stage 9 is green end to end, and stage 10 splits into the interface and the data path
2026-10-12  The reclaim stamp carried the next lap's phase, which is the phase the hardware executes
2026-10-12  The guard that could not fail, the probe that posted in the wrong lap, and the NOP that was not one
2026-10-12  fence_size is the descriptor's size in 16-byte units, not the segment count
2026-10-12  txprobe: ask whether the send queue is executed at all, and whether the doorbell arrives
2026-10-12  The send doorbell is not the gate, and now that is a measurement
2026-10-12  The driver's post-boot log does not persist, so log show cannot see it
2026-10-12  Say where the cap bites, because reading XNU rules out the obvious place
2026-10-12  This card has no BlueFlame, and the reference takes the same path we do
2026-10-12  Stage 10 planned: attaching to the network stack, and the mbuf risk that comes with it
2026-10-12  The cap is the kernel's, not ours: r4tx was declared 19 scalars wide, so no call could reach it
2026-10-12  Stage 9's transmit selector was unfalsifiable: our own scalar cap was raised past the kernel's
2026-10-12  A send queue's context never claimed Ethernet-up: pri_path.feup was never written
2026-10-12  The audit found two real things, one of them a concrete field difference
2026-10-12  Two more ruled out, and a structural clue: every doorbell that does not depend on a UAR page works
2026-10-12  My own theory was wrong, and DPDK is what refuted it: the transmit path does not write a doorbell record
2026-10-12  fence_size crosses off the candidate list: one data segment means the value 1, which is what was sent
2026-10-12  Six things ruled out for the dead transmit path, leaving one step of the PRM's three not done
2026-10-12  Say where each piece of stage 9's run evidence came from
2026-10-12  Remove the send queue's 64-byte header: the hardware reads a send queue's WQE 0 at its MTT base
2026-10-12  Both CQs are wired identically, and the receive one works, so TX's silence is not a CQ problem
2026-10-12  Stage 9 on the card: receive works on a live LAN, transmit never left, and the cause is the SQ's header
2026-10-12  Stage 9's plan had three wrong things in it, all caught before a single frame was sent
2026-10-12  A hex buffer sized for a CQE, formatted with a frame: found in review, fixed structurally
2026-10-12  Stage 9 implemented: r4Raw posts a WQE, rings the send doorbell, and reads CQEs back
2026-10-12  Stage 10's biggest unknown is gone: the network stack does not need Xcode
2026-10-12  The plan's own opcode encoding was wrong, and it was the kind that fails silently
2026-10-12  R4 stage 9 planned: the first stage where a byte actually moves
2026-10-12  Stage 7 measured: the firmware accepted base 64, which settles the falsification condition
2026-10-12  The port tells the firmware its A0 base QPN, and the send queue says who owns it
2026-10-12  Stage 8 confirmed at source level: under A0 broadcast needs nothing extra
2026-10-12  The CQE cross-check: two implementations agree on the offset, and crc_present confirms the FCS finding
2026-10-12  The official driver list for this card, and why macOS structurally has none
2026-10-12  The firmware download page is not useful, for two independent reasons
2026-10-12  The third CX-3 document corrects the PRM revision, and warns about 9K MTU
2026-10-12  The CX-3 firmware release notes are CX-3 documentation, and they settle three things
2026-10-12  Official docs and an independent implementation, and the value they refuted
2026-10-12  Stage 6 measured: 47 PASS, 0 FAIL, and the transmit side's resources exist with nothing sent
2026-10-12  The r4tx coverage row said four judgements; the verifier prints six lines
2026-10-12  Stage 6 builds the transmit side's resources and posts nothing, on purpose
2026-10-12  Stage 7 needs its own scalar for the new command, or 'all three succeeded' is unverifiable
2026-10-12  VEP_MC_STEER is set on this card, so mcast takes MCAST_DIRECT and not MCAST_DEFAULT
2026-10-12  Stage 7 will make the port-up step counter claim 5 where the tool says 4
2026-10-12  R4 stage 7 planned: one command, inserted where the reference puts it
2026-10-12  The reference's normal port activation calls qpn_calc with promisc 0, not 1
2026-10-12  The QPN space, answered: base_qpn is a parameter we hand the firmware, not a region it enforces
2026-10-12  Stage 5 after the fix: 41 PASS, 0 FAIL, 1 BLOCKED, 1 SKIP -- and the slot is now measured, not restated
2026-10-12  Stage 5's MAC half: the table goes out whole, and the slot is measured out of it
2026-10-12  The fields stage 7 has to read, and the two different things both called reserved_qps
2026-10-12  Stage 5 measured: the ingress QP is in RTS, and "QPN 64" turns out to be a region count
2026-10-12  R4 stage 5: the MAC table is one command carrying the whole table, and the ingress QP is an overlay
2026-10-12  The << 2 on the ingress QP's doorbell record is a dead field, not a semantic
2026-10-12  R4 stage 5 planned: the MAC table is one command with the whole table in it
2026-10-12  0.43.2 measured: the QP reaches RTS, and the summary line was hiding two fields
2026-10-12  R4 stage 4: the RX QP goes to RTS, and the producer publish is a host-memory write
2026-10-12  The FCS_KEEP reading reproduces, and the u16 field needs the opposite byte order
2026-10-12  FCS_KEEP is set on this card, and the devcap read that said otherwise was off by 0x10
2026-10-12  R4 stage 4, planned before it is written: the three commands, and the two places the plan was ambiguous
2026-10-12  0.43.1 measured: the event that used to poison the queue is now the proof
2026-10-12  The guard refused the ring, but the ring was never what blocks a reset
2026-10-12  The arm was in the right place and the order was still wrong
2026-10-12  Every wait is now a clock and every abandon now poisons the mailbox
2026-10-12  The firmware was never asked, so the unmaps came back as internal errors
2026-10-12  The firmware said the unmap failed and we freed the memory anyway
2026-10-12  An EQ that fires while nothing is listening never fires again -- the arm moved to session open
2026-10-12  A refused reset exited 0, the verifier remembered thirty-three sizes, and the chain asked in the wrong order
2026-10-12  The mailbox race, a constructor that initialized six members of fifty, and three registries
2026-10-12  One entry point for both sides of a reboot, deciding from values instead of prose
2026-10-12  Release compliance, done now because binary release was chosen -- and one copy of the porting base was pointing nowhere
2026-10-12  Audit of the diagnostics' coverage: no, and the biggest gap was the compiler
2026-10-12  cx3raw=1 and cx3debug=1 added to boot-args, the change verified byte by byte
2026-10-11  Diagnostics made systematic: what three working drivers on this machine do, and the real limit on "fewer reboots"
2026-10-11  Stage 3's first run caught a real one: a 64-bit bus address through a 32-bit parameter
2026-10-11  Stage 2 closes with readyAtStart 0, and stage 3 builds the receive ring
2026-10-11  The histograms answer both questions, and the idempotent path was reporting zeroes it never measured
2026-10-11  Stage 2 works, but its ring came back 0xcc and that turned up two things I had wrong
2026-10-11  R4 stage 2: the RX completion queue, and the owner-byte claim turned into a number
2026-10-11  Stage 2's values, two of which correct my own earlier guess -- and one place where I will not copy the reference
2026-10-11  Stage 1 verified, and the status-byte caveat both reviews raised did not materialise
2026-10-11  R4 stage 1: a PD and the physical MR -- and two real bugs the reviews caught in it
2026-10-11  R4's plan, written down where it can be followed rather than remembered
2026-10-11  Re-read with the full window: A0 holds, and the three zeros were all artefacts
2026-10-11  I read a byte that was never in the buffer, and reported it as a measurement
2026-10-11  R4 starts by measuring the one thing both reviews refused to let me infer
2026-10-11  Confirmed: 0x240 and 0x2C0 write once the encoding is right, and a real PORT_CHANGE closes R2's last item
2026-10-11  The firmware was never asking for alignment: a 32-bit shift by 32 looked exactly like one
2026-10-11  The A/B anomaly follows the ring, not the EQN -- and the two candidate causes are perfectly collinear
2026-10-11  R3: the port is up at 10 Gb/s, and a number this project kept quoting was a constant read as a duration
2026-10-11  Keep the MTT entries too when the ring cannot be handed back
2026-10-11  R3 designed and both reviews acted on: two wrong field widths, a wrong MTU, and a free-on-timeout
2026-10-11  Both reviews, acted on: the offset that only works on this card, and the token that could never match
2026-10-11  Reset must invalidate what it destroys, and a foreign completion must not impersonate
2026-10-11  Implement the legacy interrupt clear -- the step whose absence wedged the machine
2026-10-11  I wedged the machine with an uncleared level interrupt, and both reviews agree on why
2026-10-11  M3 R2: the same completion, delivered by an interrupt
2026-10-11  R1 complete: an event-mode command's completion arrives as a CMD EQE, in order
2026-10-11  Act on both reviews: sequential consumption, token matching, safe teardown
2026-10-11  The EQ has been working: the reporting of a found EQE was missing
2026-10-11  The ring is written in stages, and the poll was racing all of it
2026-10-11  Found it: the EQ works, and the EQE was there all along
2026-10-11  Instrument the clear itself: four labelled readings, two of them inside eqCreate
2026-10-11  The ring's content is written by something, and it is not poison
2026-10-11  M2B-NOTES: the arming sequence, three confirmations, and what the hex dump must settle
2026-10-11  R1B first run: "armed" is asynchronous, and two range-edits ate adjacent code
2026-10-11  M2B-NOTES: what R1's failure measured, and the four ways 0.24.4 separates the causes
2026-10-11  R1B: two questions the first run could not answer, reachable in one boot
2026-10-11  M2B-NOTES: cross-reference the machine's non-project crashes
2026-10-11  Fix the reset path, and add the two measurements the ring probe could not make
2026-10-11  M2B-NOTES: R1's first hardware run, and what its failure measured
2026-10-11  R1 first run: everything up to the event accepted, and the HCA wrote nothing into the ring
2026-10-11  M2B-NOTES: R1 design, both reviews' verdict, and the assumption the host test disproved
2026-10-11  R1 reviewed before running: two fixes, and a host test that caught a wrong assumption
2026-10-11  M3 restart 1: event mode, QUERY_FUNC, MTT, and an event queue that can be polled
2026-10-11  M2B-NOTES: M3 plan summary, and R1's prerequisites pinned to source
2026-10-11  M2B-NOTES: M2 complete, with the two things only a second run could show
2026-10-11  M2 is done: the HCA has its context memory and INIT_HCA succeeded
2026-10-11  M2B-NOTES: point section 10's recipe at the current state
2026-10-11  M2B-NOTES: M2d's two reviews -- the four bugs, the four non-issues, and the checker's false positive
2026-10-11  M2d: release what was mapped, and decode the last capability bit
2026-10-11  M2d twice reviewed: three of my bugs, one false-positive in the checker
2026-10-11  M2B-NOTES: the panic, its three-way-confirmed mechanism, and why the fix is an assertion
2026-10-11  Fix a kernel panic I caused: the dev-cap outbox window was widened on the wrong struct
2026-10-11  M2B-NOTES: M2d section, with the constants check and the 0xf03fc landmine recorded
2026-10-11  m2d-sequence.sh: M2d in one run, with the 0xf03fc rule enforced by construction
2026-10-11  M2d: context memory and INIT_HCA (not yet run on hardware)
2026-10-11  The RUN_FW hang was a read: 0xf03fc after the reset
2026-10-11  M2b done: the firmware runs, and QUERY_DEV_CAP answers
2026-10-11  M2B-NOTES: fix a duplicated section number
2026-10-11  M2B-NOTES: record the two falsified hypotheses and the 0.22.0 switches
2026-10-11  0.22.0: three switches for the RUN_FW question
2026-10-11  M2B-NOTES: add the Linux-side cross-check for fw_pages
2026-10-11  M2b: the FW area was 3.3x too big, and a timeout was reading as success
2026-10-11  Give reset a way past the semaphore gate, and fwload a lowmem option
2026-10-11  M2b: RUN_FW hangs; read the reset semaphore the way the reference does
2026-10-11  Fix the scalar-count guard, which never worked, and the count it should have caught
2026-10-11  Install 0.17.0 and record how to run M2b/M2c
2026-10-11  Share one implementation for MAP_FA and MAP_ICM, and test both
2026-10-11  M2b: MAP_FA, RUN_FW and MOD_STAT_CFG, with the mailbox encoding checked on the host
2026-10-11  Remove two calls the reference never makes, and add a bisect switch
2026-10-10  M2b: the ICM allocator, and a diagnosable build failure
2026-10-10  M2: QUERY_DEV_CAP returns BAD_SYS_STATE, which corrects the order in my plan
2026-10-10  Fix QUERY_DEV_CAP: the scalar path caps at 16 values, so send the outbox as a structure
2026-10-10  build.sh: make failures unambiguous, and stop two builds from colliding
2026-10-10  M2a: QUERY_DEV_CAP, and an M2 plan
2026-10-10  README: M1 is complete; record the three lessons that cost the most
2026-10-10  M1 complete: QUERY_FW returns the firmware version
2026-10-10  Solve it: the readings were never wrong, the artifact handling was
2026-10-10  linux-diag: make the readback minimal, read-only, and unable to wedge the card
2026-10-10  linux-diag: read BAR0 with devmem2, and keep run 2
2026-10-10  linux-diag: keep run 1's raw output as evidence
2026-10-10  linux-diag: read BAR0 on Linux at the offsets macOS read, and record run 1
2026-10-10  docs: the Linux diagnostic stick is built and verified on the stick itself
2026-10-10  Add a Linux diagnostic stick for the ConnectX-3 Pro
2026-10-10  docs: correct the reflash hypothesis; the card's IDs point to stock Mellanox
2026-10-10  docs: FLR does not clear the gate; record the M1b verdict
2026-10-10  docs: FLR executed successfully; record the before/after config space
2026-10-10  Add PCIe Function Level Reset as the sanctioned way past the gate
2026-10-10  Fix peek/poke to use warm reads; prove BAR0 writes reach the device
2026-10-10  docs: record the BAR0 map and the ATTO cross-check (M1-NOTES section 18)
2026-10-10  Add guarded register poke, and record why the command path is still blocked
2026-10-10  M1: Mellanox ConnectX-3 Pro (15b3:1007) macOS kext
```
