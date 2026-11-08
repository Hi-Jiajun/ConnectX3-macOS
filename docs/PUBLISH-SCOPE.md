# Publish scope

What this repository is, and what it deliberately does not carry.

## In scope

| Path | What it is |
| --- | --- |
| `src/` | The driver source: IOKit glue, hardware layer, user client, ABI header |
| `tools/cx3ctl.c` | The user-space control tool |
| `tools/check-boot-path.py` | Build-time check that `start()` cannot reach a device write |
| `kext/Info.plist` | The personality (PCI match, probe score) |
| `build.sh` / `install.sh` / `rollback.sh` | Build, stage, and revert |
| `vendor/freebsd-src/` | Vendored FreeBSD `sys/dev/mlx4`, 41 files, the porting base |
| `docs/M1-NOTES.md` | The milestone log: evidence, measurements, and mistakes |

## Out of scope, on purpose

1. **No third-party proprietary binaries.** In particular no ATTO FastFrame
   kext. That kext is Mellanox's own `cna2` code shipped under an ATTO badge
   (docs/M1-NOTES.md section 9.2) and is proprietary. It is useful only as a
   behavioural reference, which is what the notes use it for.

2. **No firmware.** None is needed: the ConnectX-3 firmware lives in the card's
   own flash, and `request_firmware` appears zero times in the `mlx4` sources.

3. **No third-party vendor kexts** from any machine's driver backups.

4. **No build output.** The `.gitignore` blocks `*.kext` and the build
   directories, and the rule is enforced from the first commit because git
   history is permanent: a file deleted later is still reachable from the
   commits that contained it.

## Licensing status

The porting base is dual-licensed GPL-2.0 / OpenIB.org BSD, per-file. This port
takes the OpenIB BSD branch, which permits redistribution in source and binary
form provided the copyright notices and licence text are retained.

**Done (2026-10-12).** This used to say the work was deferred while the
repository was private. It was brought forward because binary release became
part of the plan, and the BSD branch makes the notices a *precondition* of
binary redistribution rather than a tidy-up: a published kext without them
would not satisfy the licence, and a version already distributed cannot be
recalled by fixing the documents afterwards.

| Item | Where |
| --- | --- |
| Licence text (both branches named, the BSD branch taken) | `LICENSE` |
| Upstream copyright holders, and which source files are derived | `NOTICE` |
| Per-file headers, derived and original distinguished | the 11 files under `src/` |
| Full upstream licence text, unmodified | each of the 41 files under `vendor/freebsd-src/` |

The derived/original split was not a judgement call: every file under `src/`
that cites upstream `file:line` references is marked derived, and the counts are
recorded in `NOTICE`. Four files cite none and say so.
