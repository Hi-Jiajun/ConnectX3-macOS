# cx3-mlx4 -- Mellanox ConnectX-3 Pro (15b3:1007) macOS kext

**Status: the queues move frames; the interface is written, built, and has never run.**

`.139` is the last candidate that built and passed its targeted checks
(`test-native-port.py` 165 checks, `test-vlan-wqe.py` 16 checks). It adds VLAN
tagging, hardware MAC rewriting and the RX/TX checksum paths on top of the
lifecycle work in `.124`–`.126` and the MTU work in `.137`–`.138`. **None of
`.137`–`.139` has ever been installed or loaded.** What is actually running on
the reference machine is still `.47`.

WAVE4 — per-port link observation with host down/up transitions — is written but
**has never been built**. It also carries a known defect: `serviceLink()`
subtracts `fLinkLastNs` from `now` without first checking `now >= fLinkLastNs`,
which underflows when the result stamp is ahead of the clock.

The M4 interface layer is the boundary that matters: the data path is written and
staged, but no line of it has executed on hardware. The rest of this README is
the authority on what was measured; `docs/COVERAGE.md` has the per-selector
evidence, `docs/CX3-PRO-BACKLOG.md` the open work, and
[docs/CHANGELOG-zh.md](docs/CHANGELOG-zh.md) the per-batch Chinese development
notes (`.131`–`.136`).

Two things this repository deliberately does not carry: the `outputs/` debug
tree the reports link to (raw kernel logs from one machine — useful locally, not
useful in a public repo), and the reference machine's real MAC addresses and LAN
addresses, which are replaced with documentation placeholders throughout.
## What works, on this machine, measured

| | Evidence |
| --- | --- |
| The kext matches `15b3:1007` and wins the device against Apple's `AppleEthernetMLX5.dext` | `IOProbeScore 90000`; the dext's personality has no score key.  Bound continuously since M1 |
| Firmware handover: `MAP_FA`, `RUN_FW`, `MOD_STAT_CFG`, `QUERY_DEV_CAP`, `SET_ICM_SIZE`, `MAP_ICM_AUX`, 14 context tables, `INIT_HCA` | every step returns firmware status 0 |
| **The port is up at 10 Gb/s** | `SET_PORT` general + `INIT_PORT`, then `QUERY_PORT` reports link bit 1 and speed `0x01`.  Twice in one boot |
| **The event queue works both ways** -- polling and interrupts | 1031 events with more than two phase wraps, `acks` 1:1 with invocations, and a real `PORT_CHANGE` consumed without being mistaken for a command completion |
| A protection domain, a physical memory region, a completion queue, and a receive ring with 256 WQEs | created and read back; `SW2HW_MPT` / `SW2HW_CQ` accepted |
| Boot-argument switches: `cx3off=1` (do not start), `cx3raw=1` (general command runner), `cx3echo=1`, `cx3debug=N` | the pattern is the one this machine already runs for another driver (`nvfb=1`) |
| **Receive**: real frames land in this driver's ring and are read out of its own buffers | 60+ completions naming our QPN, every one a genuine ARP frame from the LAN, refilled correctly (`r4raw`) |
| **Transmit**: 513 send WQEs, three laps of the ring, both owner phases, zero errors, 511 frames observed on the wire | `r4raw`; the `fence_size` root cause behind the earlier silence is in `docs/M2B-NOTES.md` 54 |
| The user-space bring-up path: reset, `INIT_HCA`, ICM, EQ, CQ, RX ring, RX QP, ingress QP, MAC registration, TX | Historical `tools/verify.sh run`: 50 PASS / 0 FAIL / 1 BLOCKED before M4 acceptance. The newer M4 report linked above records the passing 0.44.47 chain; the historical count is not the current M4 verdict |

## What is still unverified or incomplete

* M4 uses a manual selector bring-up chain and 1 ms RX CQ polling. Automatic startup,
  interrupt-driven RX and a repeatable safe stop/reopen lifecycle remain release gates.
* Media/link, checksum and filtering code exists, but passing baseline DHCP/IPv6 does not
  qualify every offload/filter configuration. Jumbo, VLAN, TSO, multi-queue and combinations
  each need independent acceptance. TSO does not require jumbo and is not tested by ping.
* Historical freezes remain unexplained. A clean M4 run does not establish a root cause or
  long-term stability; `docs/HANG-AUDIT.md` retains the investigation evidence.
* The RSS-entry QUERY_QP field mismatch remains open. Existing single-ring traffic is not
  evidence of RSS distribution. Absence of a public RSS configuration KPI is not proof that
  internal multiple hardware queues are impossible; this is an open architecture decision.
* 0.44.48 is an offline-built diagnostic-index fix, not the last verified loaded driver.
  Its raw RX/TX/arm diagnostic paths still need separate hardware acceptance.
* Dual-port concurrency, high-speed throughput, CPU/latency, sleep/wake, error recovery,
  RDMA/RoCE, IB, timestamping, virtualization and other advanced capabilities have no full
  acceptance record here. See the capability matrix rather than treating M4 as project done.

## Who this is for, and what is not promised

This is a **hardware bring-up for one card on one OS generation**: ConnectX-3 Pro
`15b3:1007` on macOS 15.x. Other versions, architectures, cards and firmware combinations
are not claimed. The new roadmap requires a tested compatibility matrix before release;
the capability matrix is a planning inventory, not that compatibility certification.

It is also **not signed or notarised**, and installing a kext needs SIP and security policy
configured to permit it.  `install.sh` installs the kext and stages it; it deliberately does
not touch boot configuration, because that is per-machine.  `docs/PUBLISH-SCOPE.md` is the
authority on what is and is not distributed, and `LICENSE` / `NOTICE` carry the licensing
position -- which matters for the binary release that is planned.

## Where to look

| Document | For |
| --- | --- |
| `docs/REFERENCE.md` | The hard facts: offsets, formulas, measured values, each with its source.  This is what to cite |
| `docs/DEFECTS.md` | Every defect found (63 rows), by family, with the guard that stops it returning -- or the specific reason no guard is possible |
| `docs/COVERAGE.md` | Which selectors have actually run, on what evidence, and which have never run |
| `docs/M2B-NOTES.md` | How the M2b/M3 conclusions were reached, including the paths that were wrong first. 3300 lines, and it is a **process log**, not a reference -- the facts were extracted into `REFERENCE.md` for that reason |
| `docs/M1-NOTES.md` | The M1 log, kept for its own hard-won details |
| `docs/PUBLISH-SCOPE.md` | What this repository is and deliberately is not |
| `docs/R4-STAGE10-PLAN.md` | The interface and the data path: what 10a/10b are, what was measured on the way, and the two blockers that no reboot was needed to find |
| `docs/HANG-AUDIT.md` | The freeze: what is ruled out, the seven places that release device memory on the strength of a reset, and the instruments + verdict table that decide it |
| `docs/10B-MBUF-CONTRACT.md` | Every mbuf API, return code and ownership rule the data path uses, cited to this machine's headers and to a working driver on it |
| `docs/10B-PRIMITIVES.md` | The queue primitives the data path reuses, their invariants, and the locking discipline that keeps it out of the deadlock shape |
| `docs/CX3-PRO-FULL-ADAPTATION-PLAN.md` | Current project destination, stages, release gates and acceptance contract |
| `docs/CX3-PRO-CAPABILITY-MATRIX.md` | Full capability inventory, hardware evidence and validation gaps |
| `docs/CX3-PRO-DECISIONS.md`, `docs/CX3-PRO-BACKLOG.md` | Open decisions, next tasks and blocking edges |
| `docs/M5-PLAN.md` | Historical Ethernet plan; superseded as the project-wide scope |
| `docs/M5-REFERENCE.md`, `docs/M5-MACOS.md` | What the Linux driver does for those features, and what this machine's headers require -- both cited line by line |

## The two rules that shaped this driver

**Never write to the device from `start()`.**  `start()` enables the PCI function, maps
BAR0/BAR2, reads registers, and registers the service.  No write, no DMA.  This is not
style: build 0.3.0 ran the command path from `start()` and hung the machine before it could
flush a log line, and the card held that state across warm resets so the next six boots hung
too.  A fault in `start()` is a boot fault; a fault behind a selector is a reboot.  The build
walks the call graph from `start()` on every build and fails if a write-capable routine is
reachable.

**A value that can be read back is not written down.**  Everything the device can be asked
-- firmware version, capability counts, EQ and CQ numbers, the UAR index, the MTT address
form -- is read at run time and reported.  Constants that were guesses have cost this
project the most time of anything.

## Building and driving it

The commands below are developer entry points, not permission to run an unattended
hardware experiment. Use the current plan's identity, observer, alert and rollback gates;
the M4 report identifies the exact staged script used for that acceptance run.

No Xcode: Command Line Tools clang plus Acidanthera's MacKernelSDK.

```sh
tools/verify.sh stage      # build, validate, install, then say to reboot
# reboot
sudo tools/verify.sh run   # the ordered chain, PASS/FAIL/BLOCKED per step
sudo tools/verify.sh up    # the same chain, but leave a LIVE stack behind for peek/poke
sudo tools/verify.sh m5    # the M5 switches, one at a time, on the interface `up` left behind
```

`m5` is separate from `run` on purpose: the chain's job is to get the device and the interface
to a known state, and the M5 mode's job is to exercise the switches that are OFF by default
*after* that -- so M4's verdict is never entangled with a feature it was not designed against.

`verify.sh` decides from values read back through `cx3ctl rawjson`, not from prose, and it
names the situation it is in: a running kext older than the tool reports INCONCLUSIVE rather
than a cascade of failures.  Individual selectors are still there for single questions
(`sudo cx3ctl queryport 1`), and `cx3ctl survey` prints the driver's whole state in one call.

Useful single commands, all read-only unless they say otherwise:

| Command | Answers |
| --- | --- |
| `sudo cx3ctl netstat` | what the network data path has done: ticks, frames delivered, drops by reason, transmit completions |
| `sudo cx3ctl netlink [publish\|requery]` | the media table (not published until asked) and a fresh `QUERY_PORT` reading |
| `sudo cx3ctl netoff [rxon\|rxoff\|txon\|txoff]` | the checksum switches, and what the hardware has reported |
| `sudo cx3ctl netarm [waitMs]` | drain the receive CQ, arm it, and see whether a COMP event arrives |
| `sudo cx3ctl netfilt [report\|mcaston\|mcastoff\|promiscon\|promiscoff]` | the port's receive filters: the multicast table (remembered always, programmed only when asked) and promiscuous mode |
| `sudo cx3ctl netport [report\|down]` | stop the port -- the first half of any port reconfiguration (the second half is `m3portup <port> <mtu>`) |

`tools/verify.sh up` runs the same chain but leaves a LIVE stack behind for `cx3ctl peek`/`poke`
(nothing is reset at the end), and `CX3_STOP_AFTER=<label>` stops the chain before a chosen step --
both exist to make one reboot answer more than one question.

---

## Appendix: the original M1 notes

Kept because several conclusions arrived by first being wrong, and the wrong paths are
instructive.  Everything below this line describes M1.

```
$ sudo cx3ctl queryfw
  fw status   : 0x0 (ok)
  fw version  : 2.42.5000
  cmd if rev  : 3
  max cmds    : 16
```

The port's hardware layer comes from the FreeBSD `sys/dev/mlx4` tree vendored at
`vendor/freebsd-src`, under the **OpenIB.org BSD** licence.

## Where this is going

| | What | State |
| --- | --- | --- |
| M1 | bind the device, firmware handover | **done, measured** |
| M2 | reset + INIT_HCA + ICM, `QUERY_DEV_CAP` answers | **done, measured** |
| M3 | EQ/CQ/QP bring-up, port to Ethernet, link up | **done, measured** |
| M4 | attach `IOEthernetController`, one queue, ping | **implemented; awaiting its first boot** (`docs/R4-STAGE10-PLAN.md`) |
| M5 | checksum offload, jumbo, TSO, VLAN, filters, interrupt-driven receive | planned, in that order, in `docs/M5-PLAN.md`; the first two are written behind switches |

## What M1 established, and what it cost to learn

The useful parts are in `docs/M1-NOTES.md`, which is long because several
conclusions arrived by first being wrong. Worth reading before changing anything:

- **`start()` must stay read-only.** An earlier build ran the command path from
  `start()` and hung the machine before it could flush a log line; the card kept
  that state across warm resets, so the following six boots hung too and only a
  full power cycle recovered it. A fault in `start()` is a boot fault. The build
  script enforces this by walking the call graph reachable from `start()` and
  failing if any write-capable routine is on it.
- **Do not read BAR0 from outside while a driver owns the device.** Doing that in
  a Linux diagnostic, and then unloading the driver, hung that machine twice: the
  driver serialises HCR access with a mutex, an outside reader ignores it, and the
  interrupted command never completed (`command 0xf timed out`).
- **The first read through a freshly created BAR0 mapping returns 0.** Every later
  read returns the real value. This is a property of the mapping, not of the
  device, and it is identical on macOS and on Linux through unrelated access
  paths. It matters because `mlx4_get_ownership()` creates a fresh mapping and
  reads once, so its ownership check passes on this artifact and gates nothing.
  An earlier version of this port "corrected" the unstable reading and thereby
  failed a check the reference passes.

## Layout

| Path | Purpose |
| --- | --- |
| `src/mlx4_regs.h` | BAR0 / HCR / reset / QUERY_FW constants, each with its FreeBSD origin |
| `src/Mlx4Hw.{h,cpp}` | Register access, HCA reset, HCR command path, DMA mailbox |
| `src/Cx3Mlx4.{h,cpp}` | IOKit glue. `start()` is read-only; device work is exported for the user client |
| `src/Cx3Mlx4UserClient.{h,cpp}` | The user-space control surface (root-only) |
| `src/cx3_abi.h` | Selector numbers and scalar layouts, shared by the kext and the tool |
| `tools/cx3ctl.c` | `cx3ctl`, the tool that drives the selectors |
| `kext/Info.plist` | Personality: `IOPCIPrimaryMatch 0x100715b3`, `IOProbeScore 90000` |
| `build.sh` | No-Xcode build (CLT clang + MacKernelSDK) + offline validation + builds `cx3ctl` |
| `install.sh` | Backup persistent state, stage the kext, install `cx3ctl` |
| `rollback.sh` | `kmutil unload` + residue check + `kmutil rebuild` |

## Design rule: never write to the device from start()

`start()` performs only: PCI function enables, BAR0/BAR2 mapping, a read-only
register probe, and `registerService()`. It writes no register and allocates no
DMA.

This is not a stylistic preference. Build 0.3.0 ran the command path from
`start()` and hung the machine before it could flush a single log line; the card
kept that wedged state across warm resets, so the next six boots hung too until
the kext was removed. A fault in `start()` is a boot fault. A fault behind a
selector is a reboot.

The build script verifies this property on every build by walking the call graph
reachable from `Cx3Mlx4::start()`; `resetHca`, `queryFw`, `mboxAlloc` and every
`configWrite*` must be unreachable from it.

## Driving the device

Everything risky happens from user space, on a live system, as root:

```sh
sudo cx3ctl probe            # read-only registers + gate state. safe.
sudo cx3ctl dma              # allocate the command mailbox only. no device access.
sudo cx3ctl queryfw          # HCR handshake + QUERY_FW. writes the device.
sudo cx3ctl queryfw force    # same, but bypass the ownership gate deliberately
sudo cx3ctl reset            # HCA reset. can require a power cycle.
sudo cx3ctl all              # probe, dma, queryfw in sequence
```

The `dma` step exists so that a DMA-allocation fault and a register-write fault
can be told apart without rebooting in between.

Safeguards, all enforced in code rather than assumed:

* `queryfw` refuses unless the BAR0 window reads back live, and unless the outbox
  DMA address is below 4 GiB, so the card can never be handed an address that a
  truncating window would mis-route.
* The ownership word is checked first, mirroring FreeBSD `mlx4_load_one()` which
  skips a device whose ownership word is already set. This card reads
  `0x01000000` there, so the gate trips by default; `force` overrides it.
* `reset` requires an explicit confirmation scalar.
* The whole user client is root-only.

## Iteration cost

macOS 15 has no live kext loading (`kextutil -n` now reports
`-n is not a supported kmutil mode`), so every cycle is
`build.sh` → `install.sh` → **reboot** → read `ioreg` and `log show`.

## Apple's MLX5 dext competes for this device

`/System/Library/DriverExtensions/com.apple.DriverKit-AppleEthernetMLX5.dext`
matches `IOPCIMatch 0x000015b3&0x0000ffff` + ethernet class, i.e. *every*
Mellanox NIC.  It is launched at boot, fails on a CX-3 (`server exit before
start()`), and the device is left unclaimed.  Our personality therefore uses an
`IOProbeScore` of 90000 against the dext's implicit 0.

Escalation ladder if the kext does not bind:

1. raise `IOProbeScore` further;
2. switch the personality to a private `IOMatchCategory` so IOKit exclusivity
   no longer applies;
3. last resort: block the dext with OpenCore `Kernel -> Block`, or patch the
   sealed system volume (breaks the seal, must be redone on every OS update,
   not reproducible for other users).

## Recovery

`install.sh` backs up `/var/db/KernelExtensionManagement`,
`/private/var/db/SystemPolicyConfiguration/UAKL.plist`,
`/var/db/SystemPolicyConfiguration/KextPolicy` and `/Library/KernelCollections`
to `outputs/20261010-cx3-m1/state-backup-<timestamp>/` before staging.

If the machine will not boot after staging: boot with **Shift** (safe mode skips
third-party kexts) or from Recovery, then run `rollback.sh`.  The ESP at
`/Volumes/NO NAME` (or `/tmp/esp_ro` when mounted read-only) also allows editing
OpenCore `config.plist` from macOS or Windows.
