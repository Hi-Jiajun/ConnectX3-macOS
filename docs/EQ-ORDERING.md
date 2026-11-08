# Why the interrupt never arrived: the EQ state machine, and the arm that was in the wrong place

Status: **fixed in 0.43.0, not yet measured on hardware.**  The fix is one doorbell write,
its position is the whole point, and the two readings that decide whether it worked are
given below with the values to compare.  Nothing in this file has been observed on the
card yet: the machine was in the middle of a reboot when it was written.

## 1. The symptom, as measured

Two runs of the same selectors, differing only in when the port is brought up, produced two
different EQ state words and two different outcomes:

| Run | Order | `cx3ctl m3eqinfo`, QUERY_EQ flags word | Interrupt |
| --- | --- | --- | --- |
| A (worked) | `m3equp` first, no port-up at all | `0x00000900` | the completion arrived in under a second, `matched YES` |
| B (failed) | `m3portup` first, then `m3equp` | `0x00000a00` | never arrived: `interrupt delivery FAIL invocations=0` |

Run B is the order `tools/verify.sh run` uses; run A is the order the older
`m3-r2-sequence.sh` uses.  Both numbers are from `docs/M2B-NOTES.md` section 37, and the
A/B was repeated (section 37.2), so the correlation is measured rather than remembered.

## 2. What the two state words mean

`src/mlx4_regs.h:565-566`:

```
#define MLX4_EQ_STATE_ARMED     (9u  <<  8)     /* 0x900 */
#define MLX4_EQ_STATE_FIRED     (10u <<  8)     /* 0xa00 */
```

The flags dword of the EQ context is built from those, and it is the field that carries the
state: `src/Mlx4Hw.cpp:2901` writes `STATUS_OK | STATE_ARMED` into the context that
`SW2HW_EQ` hands over, and `QUERY_EQ` reads the HCA's copy of the same word back.

The behaviour behind the two values is the part that matters:

* **arming** -- writing the consumer index doorbell with the request-notification bit set,
  which is what `Mlx4Hw::eqSetConsumerIndex(addr, ci, true)` does
  (`src/Mlx4Hw.cpp:3857`) -- is what lets the HCA raise the EQ's interrupt;
* the first event the HCA delivers moves the EQ to **FIRED**, and from FIRED it raises
  nothing further **until software arms it again**.

So FIRED is not damage and not a configuration error.  It is a queue that has already
notified, waiting for the consumer to say "go on".

## 3. Where this port arms, and why that was not enough

There are exactly two places in this port that arm the EQ, and both of them are **consumers
of the ring**:

| Site | When |
| --- | --- |
| `src/Cx3Mlx4.cpp:2439`, inside `consumeEqLocked()` | at the end of every pass -- reached from the interrupt action (`src/Cx3Mlx4.cpp:2454`, consuming at `:2490`) and from the poll selector (`:4695`) |
| `src/Cx3Mlx4.cpp:4081`, inside `exportM3EqUp()` step 4 | once, at EQ creation |

An arm at creation makes the queue usable to a **poller**: without it the HCA writes no
completion EQEs and `m3eqpoll` would have nothing to read.  What it does not do is make the
**notification** work.  Creation happens while nothing is delivering interrupts, so an
event delivered in that window fires the EQ with no listener, and then:

```
fire (no interrupt: nothing enabled)  ->  FIRED
   nothing re-arms, because the only re-arm sites are consumers
   the interrupt action is a consumer, and it is never invoked
   => the queue waits for a notification that only it can produce
```

That is the whole defect.  It is a state machine that the order feeds into it, not a broken
queue: in run B the context is intact, the MTT entries match, `SW2HW_EQ` and `MAP_EQ` both
returned firmware status 0, and the register window is alive.  Nothing reports an error
because nothing is wrong -- the driver is waiting, and the test is waiting, on the same
notification.

## 4. What the reference does, and what that says about reordering

The reference arms the EQ **after** its interrupt is attached, and that arm is the only
one in its bring-up path:

```
mlx4_init_eq_table()                    vendor/freebsd-src/mlx4_core/mlx4_eq.c:1153
  mlx4_create_eq() -> context flags = STATUS_OK | STATE_ARMED      :1023-1024
  request_irq(...)                    <-- delivery is now on
  mlx4_MAP_EQ(async mask -> async eqn)
  eq_set_ci(&eq[MLX4_EQ_ASYNC], 1);   /* arm ASYNC eq */           :1277
```

and its handler re-arms unconditionally at the end of every scan (`mlx4_eq.c:829`), which
this port already mirrors.

Two consequences, and the second one contradicts a hypothesis worth recording:

1. The fix this file documents is the reference's own step, at the reference's own place:
   arm once, after delivery is enabled.

2. **The reference does not create its EQs after `INIT_PORT`, and neither can this port.**
   `mlx4_init_eq_table()` runs from `mlx4_load_one()` (`mlx4_main.c:2669`), long before any
   port is activated -- `mlx4_en_activate()` is the later path that does
   `SET_PORT_general` -> `SET_PORT_qpn_calc` -> `INIT_PORT`
   (`mlx4_en_netdev.c:1380-1403`).  "Move `m3equp` after `m3portup` to match the reference"
   would therefore be moving it *away* from the reference, and it is also the order that was
   measured failing (run B).  `tools/verify.sh` has that order for an unrelated and valid
   reason of its own: `exportM3PortUp()` refuses while an EQ exists
   (`src/Cx3Mlx4.cpp:2151`), because a reset is the way back from a bad port configuration
   and a reset is refused while an interrupt session is open.

   The earlier note in `docs/M2B-NOTES.md` section 37 that cited
   `vendor/freebsd-src/mlx4_en/mlx4_en_netdev.c` around lines 1413-1418 for a comment about
   a "garbage completion event" does not check out: those lines are the promiscuous-mode
   reset and the multicast task scheduling.  The observation it was reaching for -- that
   `INIT_PORT` produces an asynchronous event the driver has to swallow -- is real, and it
   is visible at `mlx4_en_netdev.c:1242-1245` ("we get here on INIT_PORT"), which is the
   link-state path, not a comment about a garbage completion.

## 5. The fix

One doorbell write, in `Cx3Mlx4::exportM3IrqUp()`, immediately after the interrupt event
source is enabled and before the call returns:

```cpp
fIrqSource->enable();
fIrqEnabled = true;

/* arm the EQ again, now that delivery is running -- src/Cx3Mlx4.cpp:2826-2839 */
if (fEqLock) IOLockLock(fEqLock);
armCi = fEqConsIndex;
fHw.eqSetConsumerIndex(fEqDoorbell, armCi, true);
if (fEqLock) IOLockUnlock(fEqLock);
```

Four things about it are deliberate:

* **It is after `enable()`, not before.**  Arming first and enabling second would leave the
  same window this fix exists to close.  The EQ is armed with delivery already running, so
  a fire at that instant is delivered rather than lost.
* **The index written is the live one, not zero.**  An interrupt can already have run
  between `enable()` and this line; writing a stale `0` over an advanced consumer index
  would send the consumer back over slots it has already read.  `fEqLock` is the same lock
  the interrupt action takes around `consumeEqLocked()`, so the read and the write are
  serialised against it.
* **The arm in `exportM3EqUp()` stays.**  It is what makes the queue usable to the poll
  selector and to the ring experiments; that path is unaffected by this change and is
  measured working (1031 events with phase wraps, section 15-17 of the notes).
* **Nothing is drained here.**  A pending event is left in the ring for the interrupt
  action to consume, which is where the reference consumes events too -- and it keeps the
  "a foreign completion must not be reported as the test's" case
  (`m3-r2-sequence.sh` step 9b) exercised rather than swallowed at session open.

No register, no context field, and no event mask changed.  The proof that this is a
positioning fix and not a configuration fix is that the context word already read 0x900 in
run A with byte-identical context-building code.

## 6. How to verify it, and what each reading decides

After installing 0.43.0 and rebooting:

```sh
sudo tools/cx3-mlx4/verify.sh run          # the ordered chain, one command
```

The line that carries the fix is the interrupt stage:

| Reading | Value when the fix works | Value when it does not |
| --- | --- | --- |
| `verify.sh run`, the interrupt stage | `the completion arrived through the interrupt  PASS` (`matched=1`, `status=0`) | `interrupt delivery  FAIL  matched=0 ... invocations=0` |
| `verify.sh run`, a port failure | reported as its own FAIL line, and the interrupt stage still runs (if it also fails, its text says the port stage failed too) | — (this is the attribution the reordering had to preserve) |
| `sudo cx3ctl m3eqinfo`, QUERY_EQ flags word | `0x00000900` (ARMED) | `0x00000a00` (FIRED) |
| kernel log, `m3irqup` | `m3irqup: armed the EQ again after enabling delivery (consumer index N, request-notification set)` | the line is absent |

```sh
sudo /usr/local/bin/cx3ctl m3eqinfo | sed -n '1,40p'
/usr/bin/log show --last 5m --predicate 'eventMessage CONTAINS "Cx3Mlx4"' --style compact \
    | grep -E "armed the EQ again|irq action|interrupt delivery"
```

Which reading to trust when they disagree: **`matched=1` is the outcome and the state word
is the explanation.**  A chain that reaches `PASS` with a `0x900` state word is the fix
working.  `FAIL` with `0xa00` is the defect still present.  `FAIL` with `0x900` would mean
something else: the queue is armed and re-arming, so the source index or the vector is what
to look at next -- `m3irqup` prints `getInterruptType`'s answer for exactly that question.

Note that `QUERY_EQ` is itself a firmware command, so reading the state is not free: its
completion is an event for the same EQ.  That is why the reading is taken after the
interrupt stage has settled (the action has a second to run and re-arm) rather than
immediately after the arm.

## 7. Rollback

The change is two files and it is additive -- no existing call site was moved or removed --
so rolling back the source is:

```sh
cd tools/cx3-mlx4 && git revert --no-edit <this commit>
sudo tools/verify.sh stage      # rebuild, reinstall 0.42.0, then reboot
```

Runtime rollback, if the new kext misbehaves after the reboot, is the one this project
already has, and it does not need the source:

```sh
sudo tools/cx3-mlx4/rollback.sh            # kmutil unload + residue check + kmutil rebuild
sudo tools/cx3-mlx4/rollback.sh --purge    # and remove /Library/Extensions/Cx3Mlx4.kext
```

A reboot is required for either to take effect, and `install.sh` has already backed up
`/var/db/KernelExtensionManagement`, `UAKL.plist`, `KextPolicy` and
`/Library/KernelCollections` under `outputs/20261010-cx3-m1/state-backup-<timestamp>/`.

The one thing rollback does **not** undo is the arm at session open: with 0.42.0 installed
again, the behaviour goes back to run B (FIRED, no interrupt) whenever the port is brought
up before the EQ, which is the order the chain uses.

## 8. What is measured, what is inferred, what is still open

Measured, on this card:

* the two state words and the two outcomes in the table in section 1, reproduced by running
  the chain in two orders;
* that the context, the MTT entries, `SW2HW_EQ` and `MAP_EQ` are all intact in the failing
  case, so the failure is not misconfiguration;
* that the only re-arm sites in this port are the two ring consumers, from the source and
  by counting the call sites in the built binary (four in total: `consumeEqLocked`, the poll
  selector, `exportM3EqUp`, and the new one).

Inferred, from the reference and from the state machine:

* that the event which fired the EQ in run B is the one `INIT_PORT` produces.  The A/B
  changes both the order and whether a port-up happens at all, so it does not isolate the
  trigger; what it isolates is the *condition* -- an EQ that fires before delivery exists
  stays fired.  Any trigger produces it, which is exactly why the fix is at the arm and not
  at the trigger.

Still open, and it needs the card:

* whether the completion arrives on the first `m3irqtest` after the fix (expected), or
  whether the pending event in the ring delays the first notification until a second event
  arrives (possible by the state machine, and it would show as `invocations=0` on the first
  read and a match on a retry);
* whether `m3eqinfo` reads `0x900` at that point in the chain, as it did in run A.

---

## 4. Measured correction (2026-10-12 04:4x, 0.43.0 on the card): **the re-arm is not the fix**

Everything above was written before the fix ran.  It has now run, and the answer is that the
arm was necessary but **not sufficient** — the state word does not move.

### 4.1 What was measured

`tools/verify.sh run`, twice, same result both times: **28 PASS, 1 FAIL, 1 BLOCKED**.  The one
FAIL is still `interrupt delivery matched=0 invocations=0`.

The re-arm **did** execute — its log line is there:

```
2026-10-12 04:43:31.141 Cx3Mlx4: m3irqup: armed the EQ again after enabling delivery
                             (consumer index 0, request-notification set) ...
```

And `QUERY_EQ` still answers `0x00000a00`, i.e. **FIRED**, at three points: right after
`m3equp`, right after `m3irqup` (post re-arm), and after the test command.  Writing the
consumer index with `req_not` does not take the queue out of FIRED.

### 4.2 The A/B that isolates it, on the same build

Same 0.43.0 on the card, same selectors, only difference = whether the port is up when the EQ
is created:

| Order on 0.43.0 | flags after `m3equp` | flags after `m3irqup` | `m3irqstatus` |
| --- | --- | --- | --- |
| `m3portup` **then** `m3equp` (what `verify.sh run` does) | `0x00000a00` | `0x00000a00` | `invocations=0`, never arrived |
| `m3equp` with **no port-up** | `0x00000900` | `0x00000900` | **`invocations: 1`, `matched: YES`, token `0x5a5a`, status `0x0`** |

So the re-arm is harmless (ARMED stays ARMED, the interrupt still arrives), and the trigger
is unambiguous: **creating the EQ while the port is already up leaves it FIRED forever.**
That is a fact about the *order*, not about the arm.

### 4.3 Why the arm could not have fixed it

FIRED means the queue has already asserted.  A re-arm asks to be notified about *new*
entries; it does not clear the condition that has already been signalled, and the event that
caused the fire is still sitting unconsumed from the host's point of view
(`m3eqpoll` reports every slot still owned by the HCA, so there is nothing for a consumer
pass to consume and re-arm against).  Under this ordering the queue fires **before delivery
exists**, and nothing that runs afterwards can undo that.

### 4.4 Where the fix actually has to go

The reference creates and arms the ASYNC EQ **before** the port is activated
`mlx4_eq.c:1277` inside `mlx4_load_one()`, and only then runs
`SET_PORT_general → qpn_calc → INIT_PORT`.  That is the same relative order as run A above,
which works.

Our kext cannot currently express it: `exportM3PortUp()` refuses while `fEqRing` exists
(`src/Cx3Mlx4.cpp:2157`), and its stated reason is about keeping `reset` available — but the
thing that blocks `reset` is an *interrupt session*, not the ring. The guard conflates the
two, and that conflation is what pins the chain into the failing order.

So the next step is a design decision, not another doorbell write: either

1. relax that guard to refuse only on `fIrqObjectsUp` (so the reference order becomes
   expressible and `verify.sh` can walk it), and move `m3equp` before `m3portup` in the
   chain; or
2. keep port-up first and drain-then-arm after it — but §4.3 says there is nothing to
   drain while every slot is HCA-owned, so this one needs evidence before it is believed.

**The `int`-level lesson, recorded because this project keeps paying for it:** the previous
version of this file asserted a fix and a mechanism ("the arm is in the wrong place") that
the hardware did not support.  It was a plausible reading of a real measurement.  The
distinguishing evidence was one more A/B on the *fixed* build, not more reasoning.

### 4.5 Also found while measuring this: a false alarm in `m3eqinfo`

`m3eqinfo` printed `MTT ... <- DOES NOT MATCH` for a ring whose entries were in fact correct.
The cause is `src/Cx3Mlx4.cpp:4803-4804`:

```c
res->mttEntry[i] = (UInt32)ent;
if (ent != (fEqRing->pages[i].busAddr | MLX4_MTT_FLAG_PRESENT))
    res->mttEntry[i] |= 0x80000000u;   /* mark a mismatch in the high bit */
```

Bit 31 of a 32-bit value is **also an address bit**: for any ring page at or above 2 GiB
(this run: `bus 0xafae4000`) the entry itself has bit 31 set, and the tool then prints
`DOES NOT MATCH` and masks that bit away with `& 0x7fffffffu`.  So the marker collides with
the data — F family, and the same shape as the "a value that looked like a measurement"
rows in `docs/DEFECTS.md`.  It is a false alarm that sends the reader hunting a ring/MTT
mismatch that does not exist; on the previous boot the same code printed `MATCH` only
because that boot's ring happened to land below 2 GiB.

---

## 5. 0.43.1 (2026-10-12 05:0x): the order is now the chain's, and here is what to run

Three changes, one of them the fix and two of them consequences.  Nothing in this section
was measured on the card: 0.43.1 was built and staged for a reboot, and the readings below
are what the next boot is supposed to produce.  Section 4 above is the measurement that
decided the design; this section is its implementation.

### 5.1 The guard that pinned the failing order is gone

`exportM3PortUp()` used to refuse while an EQ existed *or* an interrupt session was open
(the old `src/Cx3Mlx4.cpp:2353`, `if (fEqRing || fIrqObjectsUp)`), and that is what made
§4.4's option 1 impossible: the reference's order could not be expressed at all.  The
guard conflated two conditions, and only one of them has anything to do with the way back:

| Condition | Does it block a reset? | Evidence |
| --- | --- | --- |
| an interrupt session (`fIrqObjectsUp` / `fIrqSource` / `fWorkLoop`) | **yes, deliberately** | `exportResetHca()` refuses with "an interrupt session is still open"; the reason is recorded there (an uncleared legacy source storms, and it wedged this machine once) |
| an EQ ring (`fEqRing`) | **no** | nothing in `exportResetHca()` tests it, and the reset path calls `teardownEq("HCA was reset", true)` itself |

So the ring half of the guard was a false coupling, and it was load-bearing in the wrong
direction.  **It is now a log line, not a refusal** — the middle state ("allowed but only
if nothing is watching") is what §4.4 called out as unacceptable, and it would have been
the easy mistake here.

Why the relaxation is safe, i.e. why "the device is busy, so there is no way back" stopped
being true (the argument is in the code, at the guard, rather than only here):

1. Every command now waits against a **wall-clock budget and poisons the mailbox** when it
   expires (`docs/LOCK-IMPL.md`), so a stuck command costs a bounded time instead of
   holding a selector for minutes.
2. `m3irqdown()` works off **object existence**, not off a session flag, so it still closes
   the session after the storm guard tripped — which was the case where the old session
   flag was false while the objects were still allocated.
3. **FLR does not check the interrupt session.**  Verified by reading `src/flr.inc` end to
   end rather than assuming: `exportFlr()` gates on the PCIe capability, bit 28 of Device
   Capabilities and BAR0 stability, and consults `fIrqObjectsUp` / `fIrqSource` /
   `fWorkLoop` **nowhere**.
4. The new order cannot deadlock against the interrupt action.  The transaction lock is
   taken once, at the user-space boundary, around the whole `externalMethod` dispatch
   (`src/Cx3Mlx4UserClient.cpp:298`), so `m3portup` runs holding it; the action
   (`irqAction`) takes only `fEqLock`, sends no command and touches no register other than
   the EQ doorbell.  The order is `fXactLock` → `fEqLock` with no path back (grep:
   `fXactLock` appears only in `transactionEnter` / `transactionExit` /
   `transactionTryEnter`, all reached from the user-client dispatch), so there is no cycle.

One thing is deliberately **not** claimed: that FLR under a live session is a well-behaved
reset.  It resets the function while those objects exist, the ring is stale afterwards, and
if BAR0 moves the mapped window is not trusted until a reboot.  FLR is the reachable way
back, not the preferred one; the log line names the preference (`m3irqdown`, then reset).

### 5.2 `tools/verify.sh` walks the reference's order now

The chain is: EQ → interrupt clear → session open (which arms again) → **then** `m3portup`
→ `queryport`.  The relative order is the reference's own, and it is not an interpretation
of the reference: `mlx4_init_eq_table()` creates the queue, attaches the interrupt
(`mlx4_eq.c:1250 request_irq`), maps the events (`:1270`) and arms the async EQ (`:1277`),
all inside `mlx4_load_one()` (`mlx4_main.c:2669`); the port is activated afterwards by
`SET_PORT` + `INIT_PORT` (`mlx4_main.c:2756`, `mlx4_en_netdev.c:1394-1403`).  `queryport`
stays after `m3portup` because it reads back what the port-up just wrote, and it is
read-only.

Two attribution details changed with it:

* `m3portup`'s FAIL lines are unchanged, but they now fire *after* the interrupt session
  exists — and **a port failure no longer ends the chain**, which the old position made
  inevitable.  The interrupt stage below is the test this order exists for, so aborting on a
  port problem would turn "the port did not come up" into "interrupt delivery was never
  asked".  The port's outcome is carried into the interrupt stage's failure text instead, so
  a double failure reads as a double failure and mentions the port.  Nothing downstream
  depends on the port any more, so there is no BLOCKED line for it.
* The old chain reported the reverse dependency — the port was a prerequisite of the
  interrupt, and a port failure marked the interrupt stage BLOCKED.  That was accurate for
  the old order and is no longer the shape of the chain.
* The old `interrupt delivery` FAIL text told the reader that a `0xa00` state word "means
  the queue fired before delivery existed, so nothing re-armed it".  That described the
  order the chain no longer runs, so it was replaced: `0x900` (ARMED) now points at the
  source index and the vector (whose evidence `m3irqup` already prints via
  `getInterruptType`), and `0xa00` (FIRED) at "an event fired and no consumer ran".

**The new check, and why it is a SKIP and not an assertion.**  With delivery already
running, the asynchronous event that `INIT_PORT` produces should now be *consumed* by the
interrupt action, and the visible signature of that is the last consumed EQE type being
`0x09` — `MLX4_EVENT_TYPE_PORT_CHANGE` (`src/mlx4_regs.h:670`), which is exactly the event
the reference's own link-state handler expects from this step ("we get here on INIT_PORT",
`mlx4_en_netdev.c:1242`).  A `PORT_CHANGE` consumed here would be **stronger evidence than
`m3irqtest`**, because it is the port bring-up's own event rather than one the test posted.

It is nevertheless recorded as SKIP-with-numbers, never FAIL, for a reason that is itself
measured: `0x09` has **never** been observed as `last_type` on this card.  A scan of the
whole log for the 19:00–05:00 window gives only `0x00` (9 times) and `0x0a` (14 times).
Whether the firmware emits a `PORT_CHANGE` for *this* bring-up (and whether it arrives
inside the window between `m3portup` returning and the read) is unmeasured.  An assertion
that fails for a reason which is not a defect is how a reader learns to ignore the line, so
the chain reports what it saw and lets the interrupt stage below — which is a real test —
decide.

### 5.3 The `m3eqinfo` false alarm is fixed, and the ABI with it

Per §4.5, with one consequence worth naming: `Cx3EqInfoResult` changed shape.  The verdict
now travels in its own array (`mttMatch[kEqInfoMaxEntries]`, one byte per page) and the
aggregate `kEqInfoOutMatch` is computed from those per-page verdicts, so the two cannot
disagree.  **The struct is 192 bytes, up from 160** (`cx3ctl sizes` prints it; 0.43.0
printed 160), so the tool and the kext must be updated together — `install.sh` does both.
A new tool against an old kext would call `m3eqinfo` with the wrong structure size, which
the dispatch table rejects with `kIOReturnBadArgument` and a zeroed output buffer, where 0
is this project's value for "ok"; that is F4's shape, and it is why the size travels in
`cx3ctl sizes` rather than being remembered.  The tool also now prints the MTT entry whole
instead of masking bit 31 off before displaying it.

**One more false alarm, found while checking the above against the running card** (so it is
recorded here even though it is not part of the ordering story).  `m3eqinfo` also printed
`verdict: the EQC entry is ALL ZERO -- SW2HW_EQ did not store the context, so nothing
downstream can work`, and `docs/DEFECTS.md` F3 says that verdict text was removed.  It was
not: it was removed at one site and this one survived, and on 2026-10-12 05:03 the same
output showed the verdict, the readback `0 of 128 bytes non-zero`, and `QUERY_EQ answered
0x00000900` with the geometry this port built -- the tool contradicting itself two lines
apart.  The line is a fact now and points at `QUERY_EQ`; the row is F8.  That reading is
what makes the F7 impact concrete: **this is the second time in one output that a
summary sentence sent the reader after a fault that did not exist**, and both were read as
measurements.

### 5.4 What to run after the reboot, and what each line decides

```sh
sudo tools/cx3-mlx4/verify.sh stage     # build, install 0.43.1 + the tool, then reboot
sudo tools/cx3-mlx4/verify.sh run       # ← this one, after the reboot
```

`stage` and `run` both need `sudo`; the helper on this machine is
`export SUDO_ASKPASS=~/.cache/cx3-askpass/ap.sh; sudo -A <cmd>`.

| Reading | 0.43.1 works | 0.43.1 did not work |
| --- | --- | --- |
| `verify.sh run`, the interrupt stage | `the completion arrived through the interrupt  PASS` (`matched=1`, `status=0`) | `interrupt delivery  FAIL  matched=0 ... invocations=0` |
| `verify.sh run`, the port's asynchronous event | `the port's PORT_CHANGE event arrived through the interrupt (... last EQE type 0x09)  PASS` — **bonus evidence, may legitimately be SKIP** | `skip ... last EQE type 0x0a` or `invocations=0`: recorded, not failed |
| `sudo /usr/local/bin/cx3ctl m3eqinfo` | `MTT entries ... MATCH`, every `page N ... mtt[i]` line ending in `matches`, and each entry printed whole (bit 31 set is fine for a ring above 2 GiB) | any `<- DOES NOT MATCH` — now a real mismatch |
| kernel log, `m3portup` with an EQ up | `m3portup: proceeding with an EQ and an interrupt session open ...` | the older refusal line, which 0.43.1 no longer emits |
| `verify.sh run` summary | `28 PASS` + the new line as `PASS` or `SKIP`, `0 FAIL`, `1 BLOCKED` (the M4 gate) | `FAIL` on the interrupt stage |

The decisive reading is still §6's rule: **`matched=1` is the outcome and the state word is
the explanation.**  A `0x900` state word with the interrupt arriving is the fix working.  A
`0xa00` with `invocations=0` would mean the ordering change did not take effect — check
first that `cx3ctl probe` reports version 0.43.1 and that the chain is actually running the
new order (the `m3portup` log line is the tell).

```sh
/usr/bin/log show --last 10m --predicate 'eventMessage CONTAINS "Cx3Mlx4"' --style compact \
    | grep -E "proceeding with|armed the EQ again|irq action|PORT_CHANGE|port_change"
```

(`/usr/bin/log`, not `log`: this machine's shell has a `log` builtin that eats the
arguments.)

### 5.5 Rollback

Source rollback is one revert; the change is additive apart from the guard, whose removal
is what puts the chain back into the failing order:

```sh
cd /Users/you/Documents/Hackintosh-MacLocal
git -C tools/cx3-mlx4 revert --no-edit <this commit>
sudo -A env BUILD_TAG=0.43.0 bash tools/cx3-mlx4/build.sh
sudo -A tools/cx3-mlx4/install.sh          # installs the rebuilt tool too
# then reboot
```

Runtime rollback, if the new kext misbehaves after the reboot, is unchanged and does not
need the source:

```sh
sudo -A tools/cx3-mlx4/rollback.sh            # kmutil unload + residue check + kmutil rebuild
sudo -A tools/cx3-mlx4/rollback.sh --purge    # and remove /Library/Extensions/Cx3Mlx4.kext
```

A reboot is required for either.  Note that a source rollback also reverts the struct
size, so the old tool has to be reinstalled with it — `install.sh` does that.

### 5.6 Measured / inferred / untested, as of this writing

* **Measured (this batch, no device access):** the build gate; the tool's struct size (192);
  that `fXactLock` is taken only from the user-client dispatch and `irqAction` takes only
  `fEqLock`; that `exportFlr()` (src/flr.inc) consults no interrupt-session state; that the
  reset path tears the ring down itself; that `MLX4_EVENT_TYPE_PORT_CHANGE == 0x09` and
  that `0x09` has never appeared as `last_type` in the logs available here.
* **Measured (earlier, on the card, and the reason for all of this):** the A/B in §4.2 —
  port-first gives `0x00000a00` FIRED and no interrupt, EQ-first with no port-up gives
  `0x00000900` ARMED and the completion arriving, same 0.43.0 build.
* **Inferred:** that the new chain therefore lands in the ARMED branch and the interrupt
  arrives; that `INIT_PORT`'s event is what fires an unlistened EQ (§4.3 argues it from the
  state machine and the A/B condition, not from isolating the trigger); that FLR under a
  live session leaves a usable-but-stale device.
* **Untested:** everything in the table in §5.4 — 0.43.1 has never run.  Also untested:
  `m3portup` with an interrupt session actually open (the code path is new), and the FLR
  path under a live session.

---

## 6. 0.43.1 measured (2026-10-12 05:1x–05:2x): **it works, and the event that used to be lost is now the proof**

0.43.1 ran on the card.  `tools/verify.sh run`, twice, both times:

```
  PASS 30   FAIL 0   BLOCKED 1   SKIP 0
```

The step that had been the only FAIL since M1 now reads, from the run's own stored record
(`outputs/verify-20261012-052131.log`, 56 lines):

```
  the port's PORT_CHANGE event arrived through the interrupt
      (2 invocation(s), 2 EQE(s) consumed, last EQE type 0x09)   PASS
  the completion arrived through the interrupt                   PASS
```

That is a stronger result than the one §4.2 predicted, and it is worth being precise about
why.  The prediction was "the completion arrives".  What actually happened is that the
**`PORT_CHANGE` event itself** — type `0x09`, an event this project had never once observed
being consumed (`last_type` across 19:00–05:00 was only `0x00` nine times and `0x0a`
fourteen times) — was delivered through the interrupt and consumed by the handler.

That event is the *cause* of the old failure: bringing the port up with no delivery running
is what fired the queue into FIRED and left it there.  So the evidence is not merely "the
interrupt path works now"; it is "the exact event that used to poison the queue is now
handled, and the queue stayed ARMED around it".  Before this change the same step produced
`invocations=0`; the run above shows 2 invocations and 2 EQEs consumed by the time the port
step finished.

The rest of the chain is unchanged and green (28 steps in §5.4's list, plus the two new
ones), and the ICM fix held on this boot as well: zero `UNMAP_ICM` commands answered
`0x1`, against fourteen per teardown before.

Recorded here rather than in §4 because §4 is the correction and this is the confirmation;
the two together are the full shape — a plausible mechanism, a measurement that refuted it,
and a second measurement that refuted the refutation's own proposed cause.

### 6.1 One more check, and one defect it exposed

Quoting those numbers required opening the file the run had printed a path to — and the file
did not exist.  `tools/verify.sh` printed `log: .../verify-<stamp>.log` in the summary of
every `run` while only `stage` ever wrote `$LOG`, so every `run` transcript (including the
two quoted in §4 and §5) lived in terminal scrollback and nowhere else.  Fixed in this same
commit (`exec > >(tee "$LOG")` for `run`/`full`/`safe`; `stage` excluded because it
truncates `$LOG` into itself), and the run above is the first one with a stored record.
Row F9 in `docs/DEFECTS.md`.
