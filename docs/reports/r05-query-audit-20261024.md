# R05 offline query and raw-field audit

Date: 2026-10-24  
Scope: source/layout audit only. No selector, mailbox command, hardware query, install, load, or restart was run.

## Findings

The apparently nonsensical quota values previously tried for `max_mrw_sz` and `max_mtt_seg` do not establish that the offsets are wrong. The checked-in FreeBSD reference and `src/mlx4_regs.h` agree on the byte offsets, but the FreeBSD decoder does **not** derive these two quantities. The current card's saved selector-8 response has raw bytes `0x40` at `0x21`, `0x23` at `0x23`, and `0x00` at `0x3f` (reconstructed BE response SHA-256 `60aa26f4f4bc75bba4312d5f7516fc1e78b1b70ceca3663d4619764fa5ec84e5`). Raw values are evidence; the transform must be sourced separately:

| Field | Raw byte offset | Reference interpretation | Current macOS path |
| --- | ---: | --- | --- |
| `max_srq_sz` | `0x10` | `1 << byte` WQEs | `Mlx4DevCap` decodes it; raw `QUERY_DEV_CAP` is exported |
| `max_qp_sz` | `0x11` | `1 << byte` WQEs | `Mlx4DevCap` decodes it; raw `QUERY_DEV_CAP` is exported |
| `max_mrw_sz` | `0x21` | Offset is named in FreeBSD; its `QUERY_DEV_CAP` decoder does not consume it. The former local “log2” comment was not an independent primary-source derivation and is now raw-only | Raw `0x40`; no decoded `Mlx4DevCap` member. If treated as exponent, `2^64` cannot fit in `UInt64`; keep meaning/value unresolved pending PRM or another authoritative decoder |
| `reserved_mrws` | `0x22` | `1 << (byte & 0x0f)` objects | `Mlx4DevCap` decodes this field |
| `max_mtt_seg` | `0x23` | Offset is named in FreeBSD; its `QUERY_DEV_CAP` decoder does not consume it. The former local `log2` comment and CLI exponent interpretation are now removed | Raw `0x23`; exponent interpretation is not independently established by the checked-in reference. `2^35` is only a conditional calculation, not an accepted card capability |
| `max_pkeys` | `0x3f` is read only in FreeBSD's legacy-port-command branch; `0x07` low nibble is used by the current `QUERY_PORT` branch | Current port parser: `1 << (raw[0x07] & 0xf)`; saved port-1 byte is `0x77`, yielding 128. Legacy global byte `0x3f` is `0x00` but is not the current path's per-port answer | Keep the legacy and current layouts separate; do not overwrite the validated per-port 128 with the legacy-path value |

The reference declarations are in `vendor/freebsd-src/mlx4_core/mlx4_fw.c` at the `QUERY_DEV_CAP_*_OFFSET` definitions. Linux v6.12 `drivers/net/ethernet/mellanox/mlx4/fw.c` independently declares the same `0x21`, `0x23`, and `0x3f` offsets; it likewise does not decode `max_mrw_sz` or `max_mtt_seg` in `mlx4_QUERY_DEV_CAP()`. The current command's per-port query branch is at `mlx4_QUERY_PORT()`; the `0x3f` PKey decode is specifically inside `MLX4_FLAG_OLD_PORT_CMDS`. Matching offsets are in `src/mlx4_regs.h`. `src/Mlx4Hw.cpp::queryDevCap()` preserves the response as host-order 32-bit words. For byte offsets, reconstruct each word to its original big-endian byte sequence first; indexing a cast `rawWords` buffer as if it were the mailbox byte array is invalid. `tools/cx3ctl.c` already performs that reconstruction for its `querydevcap` display. Its former MTT exponent calculation was an implementation hypothesis, not evidence from either reference decoder; the follow-up below replaces it with raw-only output. Linux source: [v6.12 mlx4/fw.c](https://github.com/torvalds/linux/blob/v6.12/drivers/net/ethernet/mellanox/mlx4/fw.c#L748-L775).

This confirms byte reconstruction and raw offsets, **not semantic decodes** for `0x21` or `0x23`. The saved raw response is at `outputs/20261022-cx3-m4-preflight/runs/netup-20261022-204605/8.json`. Do not label conditional `2^64` / `2^35` arithmetic as the card's validated MRW/MTT limit. Preserve the raw byte and unresolved interpretation until a primary source settles exponent meaning, units, and the representable range.

## Query effects and prerequisites

| Query | Device-side operation | Host-side effect / prerequisite | Classification |
| --- | --- | --- | --- |
| selector 8 `QUERY_DEV_CAP` | Allocates a command mailbox and submits firmware command `0x3`; no `SET_*`, reset, or table programming is in this path | Refuses unless the ownership reference is zero and BAR0 responds; mailbox/HCR must be live and firmware must have reached the state where `QUERY_DEV_CAP` is accepted | Read-only with respect to device configuration, but not a passive memory snapshot; timeout/hang risk exists because it performs a polled command |
| selector 24 `QUERY_PORT` | Allocates a command mailbox and submits polled firmware command `0x43` for the requested port | Valid ports are 1 or 2; successful response updates `fPortMac[port-1]` and its valid flag. The current cache is per-port, so port 2 no longer overwrites port 1's MAC. Mailbox allocation and command completion can fail or stall | Read-only with respect to port configuration, but has a host cache side effect and is not zero-risk/passive |

Evidence: `Cx3Mlx4::exportQueryDevCap()` and `Mlx4Hw::queryDevCap()`; `Cx3Mlx4::exportM3QueryPort()` and `Mlx4Hw::queryPort()`. Do not label either query “no side effect” without qualification. The earlier concern that port 2 overwrote a single shared MAC cache is stale for the current source: `fPortMac` is indexed by port and selector 24 explicitly leaves the other entry untouched.

## R05 disposition and next offline checks

R05 remains READY, not complete. This audit narrows the next work to:

1. Keep a field-by-field source ledger that distinguishes fields with a reference decode expression from fields for which the reference only names an offset; leave the latter unresolved until a primary source supplies semantics.
2. Compare each independently sourced decode with `cx3ctl` output and add fixture tests for high exponents, byte order, packed nibbles, and overflow handling before recording values in `capabilities.json`.
3. Complete a query inventory for PSID, PCIe/link, per-port fields, DOM/transceiver, and firmware/configuration data. For every candidate command, record opcode, selector path, mailbox behavior, prerequisites, whether it changes firmware/host state, timeout behavior, and whether it requires a separate R07 hardware gate.
4. Keep port 2 and other not-yet-captured values UNKNOWN until a specifically approved hardware window obtains them; this report does not authorize a query.

The initial source audit above added no capability values. The subsequent offline implementation is recorded below; neither created new hardware evidence.

## 2026-10-24 implementation follow-up

The saved decoder now has 51 fields (37 original + 14 source-supported additions), plus QP rate-limit metadata and two explicitly unresolved raw fields. All original 37 values/raw values, flags and raw mailbox hash remain unchanged. Source provenance for rate units points to the actual anonymous `MLX4_QP_RATE_LIMIT_*` enum in `vendor/freebsd-src/device.h:495-500`.

Two production decode transcriptions were corrected in `src/mlx4_pure.h`:

- Reserved MCG is `byte & 0x0f`, not `1 << nibble`: saved `0x40` gives 0; synthetic `0xa3` and `0xff` give 3 and 15.
- Counters are BE32 at `0x68`, enabled by `MLX4_DEV_CAP_FLAG_COUNTERS` (flags64 bit48); saved `00000100` gives 256, absent flag gives 0. A synthetic `01020304` verifies the width and byte order.

The previous turn observed red tests before these fixes (MCG: one failing check; counters: three), then built `.116-local`. This turn reran the 143-check pure test and the 30 existing Python scripts successfully and verified the recorded `.116` artifact hash: `80f6045ee01fa37e350b21bd4dee0dd0b8da65c93848e1269d4cef6de31d82d8`, at `/tmp/cx3-r05-116.QYqvXZ/out/Cx3Mlx4.kext/Contents/MacOS/Cx3Mlx4`. Its existing build log records `BUILD RESULT: PASS`; this turn did not rebuild or load that kext.

The CLI still printed the unverified MTT exponent as `34359738368 B`. A new production-function test reproduced that incorrect output before the change. `cmdQueryDevCap()` now prints raw MRW/MTT bytes with unresolved size/units, and the header no longer claims a sourced log2 interpretation. `tools/test-devcap-client.py` runs the actual formatter/ABI reconstruction with a synthetic transport (no IOKit), retains known QP/MPT/MRW output checks, and exercises all 256 byte values under UBSan. Both tests pass. The real CLI compiled separately with IOKit/CoreFoundation; SHA-256 `5614d632f11ea5eacef9e900bea1bcd5908b0e4c450a26543806172911da0894`, at outer `outputs/20261024-cx3-r05-close/cx3ctl`. That binary was not run against the driver and is not installed or signed as a release.

Saved-data decoder: 10 tests pass. Final full Python suite: 31 scripts, exit 0 (including the new CLI test). Original JSON backup: outer `outputs/20261022-cx3-fulladapt-batch1/r05-before-20261024.9KZ28e/capabilities.json`. New regression log: outer `outputs/20261024-cx3-r05-close/full-python-regression.log`. No claim here closes R05/P0: MRW/MTT semantics, remaining BF/steering/flag coverage, PSID, port 2, current PCIe, DOM and lab resources remain open. R02/R03/R06 and real hardware recovery remain separate unresolved work.

## QUERY_ADAPTER board identity correction and query inventory

The current `queryAdapter()` had copied 64 bytes from outbox `0x20`, which is the VSD container rather than the board ID. Vendored `mlx4_fw.c:get_board_id()` (1751–1787) supplies the actual layout: both BE16 signatures at VSD-relative `0x00` and `0xde` must equal `0x05ad` to select Topspin; its string starts at VSD `0x20` and is bounded by the 64-byte destination. All other responses use the 16-byte Mellanox ID at VSD `0xd0` (absolute outbox `0xf0`), with bytes reversed within each 32-bit word. The corrected production method preserves the command tuple, INTA pin behavior, 64-byte output and caller ABI.

`tools/test-adapter-query.py` executes that production method with literal mailbox fixtures. Before the fix, 9 of 28 checks failed; after the fix, all 28 pass with UBSan and compiler warnings as errors. Fixtures include both signature checks independently, Mellanox word order, Topspin truncation/NUL padding, destination canaries, omitted outputs, mailbox allocation failure and firmware rejection. This validates decoding of synthetic replies; no response from this card was newly collected, so its PSID remains unknown.

| Target | Exact reference path / command | Host effects, prerequisites and missing work |
| --- | --- | --- |
| Board identity / candidate PSID | `mlx4_fw.c:1789–1812`: `QUERY_ADAPTER=0x6`, outbox 256 bytes, no input mailbox, modifier/opmod `0/0`, time class A (60 s) | Allocates/uses a command mailbox and writes HCR; firmware must accept the command. Existing selector 23 `exportM3ClrInt()` exports only the first 8 ID bytes and also calls `clrIntSetup()`: tears down the old clear-register mapping, may map a BAR, updates clear pointer/mask/offset and marks `fClrIntReady`. That selector itself does not call `clrIntWrite()`. A dedicated full/raw identity query is still required; selector 23 is unsuitable as a passive PSID collection endpoint. |
| Transceiver EEPROM / DOM input | `mlx4_port.c:1859–2018`: MAD_IFC opcode `0x24`, modifier=port, opmod=3, time class C (60 s), input and output mailboxes; MAD base/class version=1, management class=1, method GET=1, attribute BE16 `0xff60` | Maximum 48 bytes per command. Do not cross the 256-byte low-page boundary; low page I2C `0x50`, high page `0x51` with offset reduced by 256, page number 0. Check both HCR and MAD status. Unsupported `0x51` can yield zero returned bytes, which means no sample, not a valid all-zero DOM reading. No macOS dedicated selector exists yet. Arbitrary MAD_IFC is not read-only: a future endpoint must construct this exact GET request and validate port/range. Module type must determine which EEPROM bytes carry DOM. |
| Current PCIe link | Separate PCI configuration-space capability read; no firmware mailbox command specified by this audit | Historical Linux Gen3 x4 is not a current measurement. A bounded read-only PCIe capability walker and dedicated export remain to be specified; do not infer current width/speed from BAR allocation or PCI identity. |

Isolated build `0.44.117-local` passed with the board-ID correction and preceding CLI raw-only MRW/MTT display. Saved artifact: outer `outputs/20261024-cx3-ab-transaction/kernel-0.44.117/`; kext SHA-256 `536d16c278763691f4133be6b69b1845d523fb92f12a5d7022b173e6cf35ebea`. The CLI built in that invocation has SHA-256 `e956eab40e3bf17bc8a8a4075160c5343a1b0f19fe0a26ceafab9d1df559141a`. The full 33-script Python suite passed; log: outer `outputs/20261024-cx3-ab-transaction/full-python-regression.log`. No device query, installation, loading, reset or restart was performed.

## .118 complete/raw identity endpoint — offline accepted

The dedicated endpoint requested in the inventory is now implemented as selector47 (`kCx3SelQueryAdapter`), separate from clrint/selector23. Its version1 360-byte output carries status, firmware status, provider-local generation, INTA, the full64-byte candidate ID and all256 bytes of the mailbox. It has no scalar/structure input or scalar output. UserClient validates exact output size and runs under the existing transaction/admission locks. Reset/retention, ownership and live-window checks precede the command; failures return no raw/ID payload. This is still a firmware command with mailbox/HCR activity and timeout risk, not a passive read.

`cx3ctl queryadapter` validates the reply before displaying it; `rawjson 47 out 0 struct 360` preserves explicit zero scalar output and saves the complete result. `tools/decode-adapter.py` independently re-decodes the raw mailbox and compares ID/INTA, preserves empty/binary identities as unresolved, and rejects failed frames with nonzero identity data. This closes the **offline query/export/consumer implementation sub-item**, not the card identity hardware capture or R05/P0.

Production query-method fixtures:39 checks; export/handler:19 checks; CLI valid/rejection matrix and independent decoder8 tests pass. Full35-script Python regression and complete `.118-local` build exit0. Independent GPT Luna/high review found a stale highest-selector assertion, fixed and verified as200 CQE/WQE/ABI checks with zero failures; the original red build remains saved. Final65-item source lists and unpacked archive match. Artifact hashes and logs: [batch evidence](../../../outputs/20261024-cx3-adapter-export/README.md).

The saved `PSIDTEST00000001` sample is synthetic transport data, not this card's PSID. No hardware query, installation/loading, reset or reboot occurred. Next: [current PCIe query specification](r05-pcie-query-spec-20261024.md) is now primary-source checked and ready for implementation; dedicated EEPROM/DOM GET and approved per-port/identity captures remain. Existing unresolved field semantics and resources remain open.
