# M2 plan: from QUERY_FW to INIT_HCA

Where M1 ended and what M2 has to do, with the parts that are Mac-specific
written out so they do not have to be rediscovered.

## Status

**M1 complete.** Binding, BAR mapping, register read/write, firmware commands.
`QUERY_FW` returns `2.42.5000`, `cmd_if_rev` 3, `max_cmds` 16.

**M2a learned the order the hard way.** `QUERY_DEV_CAP` answered
`fw status = 0x4`, which is `CMD_STAT_BAD_SYS_STATE`. The reference's startup
sequence explains it:

```
mlx4_init_fw():
  1. QUERY_FW                                  done in M1
  2. mlx4_load_fw():
       a. mlx4_alloc_icm(dev, fw_pages, ...)   allocate the FW area
       b. MAP_FA    (0xfff)                    hand that area to the firmware
       c. RUN_FW    (0xff6)                    start the firmware
  3. MOD_STAT_CFG (0x34)                        non-fatal if it fails
mlx4_init_hca():
  4. QUERY_DEV_CAP                              answers only after step 2
```

So the ICM allocator has to come **before** `QUERY_DEV_CAP`, not after.
`QUERY_FW` is cheap because it asks about a stopped device; `QUERY_DEV_CAP`
describes a running one.

**M2b implemented and installed (0.17.0), waiting on a reboot.** All four steps exist:

    sudo cx3ctl icmalloc 64        # allocate the FW area on its own (device-inert)
    sudo cx3ctl fwload <fw_pages>  # allocate + MAP_FA + RUN_FW + MOD_STAT_CFG
    sudo cx3ctl querydevcap force  # now it should answer

`fw_pages` comes from `QUERY_FW` (`cx3ctl queryfw`), and it is a required
argument to `fwload` rather than something the kext guesses, so the number is
visible and the dependency is explicit.

**M2c is testing that**, and M2d cannot be designed until M2c returns, because the
ICM table sizing comes out of `QUERY_DEV_CAP`.

### Running it

```
sudo cx3ctl probe                 # read-only baseline
sudo cx3ctl queryfw               # gives fw_pages
sudo cx3ctl fwload <fw_pages>     # M2b: the firmware handover
sudo cx3ctl querydevcap force     # M2c: should answer now
```

`reset` and `flr` are deliberately not in that sequence. The ownership gate that
blocked `queryfw` in M1 turned out to be a mapping artifact, and the reference
passes it by reading a freshly created mapping once; `checkOwnershipAsReference()`
reproduces that. If a command is still refused with `kIOReturnNotPermitted`, add
`force` and say so in the transcript rather than reaching for `reset`.

### The MAP_FA mailbox, and how its encoding is checked

`mlx4_map_cmd` (FreeBSD `mlx4_fw.c:1501`) fills the mailbox with 64-bit pairs:

```
[0]  the address the firmware should see this block at (MAP_ICM), or nothing at
     all when virt = -1 (MAP_FA maps by bus address, so there is no virtual side)
[1]  the block's bus address, OR-ed with (lg - 12), where lg is the least
     significant set bit of (address | size), i.e. how well aligned the block is
```

`in_param` is the mailbox's DMA address, `in_modifier` the entry count, and the
list goes in batches of 256.

This port emits one entry per 4 KiB page with the alignment field 0, instead of
computing `lg` per scatterlist entry. Both are valid; the reference form is more
compact, and the difference is efficiency rather than correctness.

The layout is easy to get subtly wrong and a mistake means telling the firmware to
use pages it should not, so `tools/test-mapfa-encoding.c` replicates the encoding
on the host and checks it: entry stride, which slot holds the address, big-endian
on the wire, zero in the virtual slot for MAP_FA, no truncation above 4 GiB, and
that 256 entries exactly fill the mailbox. It touches no hardware. Run it with

    cc -o /tmp/t tools/test-mapfa-encoding.c && /tmp/t

### One discrepancy in the vendored source

`QUERY_FW_SIZE_OFFSET` is defined as **0x00** in this FreeBSD copy, which collides
with `QUERY_FW_VER_OFFSET` and would read the top of the version field. Mainline
Linux uses **0x10**. This port uses 0x10 and prints the whole outbox, so `fw_pages`
can be sanity-checked against a plausible firmware size (`pages >> 2` is KB)
rather than trusted.

## Revised work order

| Step | What | State |
| --- | --- | --- |
| M2a | `QUERY_FW`, and read `fw_pages` from it | done |
| M2b | ICM allocator, `MAP_FA`, `RUN_FW`, `MOD_STAT_CFG` | implemented, **not yet run** |
| M2c | `QUERY_DEV_CAP` | implemented; should answer once M2b has run |
| M2d | `SET_ICM_SIZE`, the ICM tables, `MAP_ICM_AUX`, `INIT_HCA` | blocked on M2c's numbers |
| M2e | `QUERY_HCA` / `QUERY_DEV_CAP` afterwards, to read back what was accepted | not started |

Each step has its own selector, so a fault is attributable to one command rather
than to "M2".

## What the ICM actually is

This is the piece that needs real Mac work, so it is worth stating plainly.

The device has its own context tables — QP contexts, CQ contexts, EQ contexts,
MPT/MTT for memory regions. It does not have enough on-chip memory for them, so
the **host allocates ordinary RAM pages and hands the addresses to the firmware**,
which then uses that memory as if it were its own. That memory is the ICM
(InfiniHost Context Memory).

The handover is a firmware command. `mlx4_map_cmd` (FreeBSD
`mlx4_core/mlx4_fw.c:1501`) is worth reading in full; the shape is:

1. Build a mailbox of `__be64` pairs:
   - `pages[n*2]`   = the ICM **virtual** address the firmware should see
   - `pages[n*2 + 1]` = the **physical (bus) address** of that page, ORed with
     `(lg - 12)`, where `lg` is the least significant set bit of `(addr | size)`
   - that OR encodes the page's alignment, so the firmware knows how to treat a
     block larger than one page
2. Send `MAP_ICM` with `in_param = mailbox DMA address`, `in_modifier = entry count`
3. Batch at `MLX4_MAILBOX_SIZE / 16` = **256 entries per command**

So what the Mac port must supply is exactly: **pages with known bus addresses, at
least 4 KiB aligned**, plus the mailbox it already has.

## The Mac side: one general DMA allocator

The mailbox code in `Mlx4Hw::mboxAlloc` already does the hard part for a single
4 KiB buffer, and it generalises directly:

```cpp
IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared,
        size, 0xfffffffffffff000ull);          // page aligned

IODMACommand *cmd = IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 64, 0, IODMACommand::kMapped, size, 1);

cmd->setMemoryDescriptor(md, true);
cmd->gen64IOVMSegments(&offset, &seg, &nseg);  // offset is in/out: == size means fully consumed
```

Each returned segment gives a bus address and a length; a 4 KiB page inside a
segment is at `seg.fIOVMAddr + delta`. With the physical mask above, the
allocation is page-aligned and a 4 KiB ICM page never straddles a segment
boundary, which is the property `mlx4_map_cmd` requires of us.

So M2b is: wrap that in an allocator that returns `{virt, [bus, len], ...}`, then
write the port of `mlx4_map_cmd` on top of it. The list-of-pages bookkeeping
(`mlx4_icm`, `mlx4_icm_chunk`, `mlx4_icm_iter`, `mlx4_table_get/put/find`) is
plain data structure work with no IOKit in it, so that code can come over close to
verbatim; only the allocation and the bus-address discovery are Mac-specific.

`MLX4_TABLE_CHUNK_SIZE` is `1 << 18` (256 KiB) and `MLX4_ICM_PAGE_SIZE` is 4 KiB,
so chunks are 64 pages.

## Two things to check before writing any of it

**The page size.** `QUERY_DEV_CAP` returns `page_size_cap`, and the reference
derives the minimum page size as `~cap + 1` and then refuses to continue if that
is larger than the OS page size (`mlx4_main.c:847`). If this device wants more
than 4 KiB, the ICM layer's page arithmetic has to change, so read this number
before designing around 4 KiB.

**The ownership gate.** Running `querydevcap` without `force` will likely be
refused. That is expected and is not a new problem: the gate reads a mapping
artifact (see `docs/M1-NOTES.md` §26.3), and `checkOwnershipAsReference()` was
added to reproduce the reference's own reading of it. If it still refuses, use
`force` and note it — the gate has no real signal behind it on this hardware, and
Linux drives the card regardless.

## Work order, and why the writes stay in user space

Every step below issues firmware commands, so all of them belong behind
`Cx3Mlx4UserClient` selectors, not in `start()`. That is not a formality: an
earlier build ran command-path code from `start()` and made the machine
unbootable, and the build check (`check-boot-path.py`) now fails the build if a
write-capable routine becomes reachable from `start()`.

| Step | What | New selector |
| --- | --- | --- |
| M2a | `QUERY_DEV_CAP` | `querydevcap` (done) |
| M2b | general DMA allocator + `MAP_ICM` | `icmalloc`, and `icmmap` if useful separately |
| M2c | `SET_ICM_SIZE` + `MAP_ICM_AUX`, then `INIT_HCA` | `icmaux`, `inithca` |
| M2d | `QUERY_DEV_CAP` afterwards to confirm the device is up, and `QUERY_HCA` to read back what it accepted | reuse `querydevcap`, add `queryhca` |

The order matters: `SET_ICM_SIZE` tells the firmware how much ICM there will be
and must precede `MAP_ICM_AUX`; `INIT_HCA` then takes the addresses of all the
tables.

## The wedge risk, and how it is contained

This is the one thing to be careful about, because it has already happened twice
on the Linux side. The command engine deadlocks if a command is posted while
something else is touching the HCR, and a deadlocked command engine is what
produces `command 0xf timed out (go bit not cleared)`. With `INIT_HCA` in play the
device is in a half-configured state, so the failure is worse than a failed query.

Containment, in order of importance:

1. **Never read BAR0 from outside the driver while a command may be in flight.**
   Reads go through the same HCR; that is exactly what wedged the Linux runs.
2. Command timeout is 5 s here rather than the reference's 60 s, so a stall
   surfaces quickly instead of looking like a hang.
3. One selector per step, so a fault can be attributed to a single command rather
   than to "M2".
4. `INIT_HCA` and the ICM mappings are idempotent enough to retry after a reboot,
   and a failed attempt is diagnosable from the returned `fw_status`.

## Open questions to answer with M2a's output

- the minimum page size, hence whether 4 KiB bookkeeping holds
- how many ICM pages the tables actually need, which comes from the resource
  counts
- whether `ext_flags` / `flags` mark anything we must honour early (VXLAN, CQ
  timestamping, 64-byte CQEs, and similar)
- whether the device reports the same `reserved_*` counts the reference assumes
