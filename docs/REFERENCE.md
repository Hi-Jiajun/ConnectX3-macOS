# Hard facts about this card, and where each one comes from

The measured values, offsets and formulas that this driver is built on.  Each row says how
it is known: **measured** means read off this card and recorded; **source** means it comes
from the vendored FreeBSD driver at the cited `file:line`.

The point of separating this from the process logs is that a fact and the story of how it
was learned have different lifetimes.  The story belongs in the milestone log; the fact
belongs here, and when a fact changes, this is the file that changes.

## Identity and firmware

| Fact | Value | How |
| --- | --- | --- |
| Device | Mellanox ConnectX-3 Pro, `15b3:1007`, subsystem `15b3:0003` | measured |
| Board | MCX354A-FCCT, "CX354A - ConnectX-3 Pro QSFP", two QSFP+ ports | VPD, via `lspci` on Linux |
| Firmware | **2.42.5000**, command interface revision **3**, max_cmds 16, pf_id 0 | measured, `QUERY_FW` |
| `cmd_if_rev 3` means | the **new** port command interface: `INIT_PORT` takes no mailbox (`mlx4_fw.c:1671` sets the old-flag only when rev < 3) | source |
| Steering mode | **A0** | measured, `QUERY_HCA`: `flags` bit 6 = 0 and byte `0x0d8` bit 3 = 0 |
| A0 consequences | `SET_PORT_qpn_calc` **does** send a command (`mlx4_port.c:1588` returns early otherwise), and a unicast MAC's QPN is `base_qpn + mac_index` | source |
| Firmware flashed? | **No.** Current Ethernet operation has been demonstrated without flashing. MCX354A-FCCT is a VPI model; this card's PSID and firmware-enabled IB modes remain unverified | Linux logs + macOS M4; see `CX3-PRO-FULL-ADAPTATION-PLAN.md` |
| Firmware needed from the host? | None.  `request_firmware` appears 0 times in the porting base | source |

## Capability counts (all from `QUERY_DEV_CAP` unless noted)

| Item | Value |
| --- | --- |
| `reserved_qps` / configured QPs | 64 / 1024 (`log_num_qps` 10) |
| `reserved_cqs` / configured CQs | 128 / 256 (`log_num_cqs` 8) |
| `reserved_srqs` | 64 (`log_num_srqs` 6) |
| `reserved_eqs` / `max_eq` | 8 in `QUERY_DEV_CAP`; **`QUERY_FUNC` reports reserved 28 and max_eq 156** |
| `reserved_mtts` | 64 |
| `reserved_mrws` / configured MPTs | 256 / 2048 (`log_mpt_sz` 11) |
| `reserved_pds` / total PDs | 4 (decoded from `QUERY_DEV_CAP_RSVD_PD_OFFSET` `0x64` high nibble) / 32768 (`MLX4_NUM_PDS 1<<15`) |
| `reserved_uars` | **2 in `QUERY_DEV_CAP`, but 128 is what matters** -- see below |
| `reserved_mgms` | 1 |
| Entry sizes | qpc 256, cqc 128, eqc 128, srq 128, **mtt 8**, dmpt 64, cmpt 64, rdmarc 32, altc 64 |
| `uar_size` / log | 4 MiB, `log_uar_sz` 10 |
| `num_sys_eqs` | 1024 |
| EQE / CQE size | **64 bytes each** (`eqe/cqe` word `0x60000000` sets both 64-byte flags, `mlx4_fw.c:1917-1931`) |

### The UAR index, and why two different numbers are both right

`QUERY_DEV_CAP` says `reserved_uars = 2`.  **That is not the starting index software must
use.**  `mlx4_main.c:184-193` computes
`caps.reserved_uars = max(128 >> (PAGE_SHIFT - uar_page_shift), devcap_reserved / that shift)`,
and on this card `uar_page_shift = uar_page_sz + 12 = 12 = PAGE_SHIFT`, so the two terms are
128 and 2 and the maximum is **128** (`device.h:1576-1579`: "the first 128 UARs are used for
EQ doorbells").

So: **software UARs start at 128**, the UAR page is `BAR2 + 0x80000`, and the CQ doorbell is
`BAR2 + 0x80020`.  The arithmetic is corroborated by the EQ doorbell this driver already
uses: `((eqn/4) << 12) + 0x800 + 8*(eqn%4)` with EQN 28 gives `0x7800`, which is what the
working EQ logs show.

## Memory layout after `INIT_HCA` (announced by the port, echoed by `QUERY_HCA`)

| Table | HCA base | Objects |
| --- | --- | --- |
| QPC | `0x100080000` | 1024 |
| CQC | `0x100170000` | 256 |
| EQC | `0x10017a000` | 2048 |
| SRQC | `0x100178000` | 64 |
| dMPT | `0x100140000` | 2048 |
| MPT (MTT) | `0x1000c0000` | 32768 entries, 262144 bytes, mapped in full (64 pages, one chunk) |
| MCG | `0x100100000` | entry log 10, hash log 7, table log 8 |
| FW area | 1542 pages = 6168 KiB | from `QUERY_FW` offset `0x00` |
| ICM total / aux | 4195828 KiB / 2082 pages | measured, `SET_ICM_SIZE` |

## Queue addresses and encodings

| Item | Value |
| --- | --- |
| MTT address in a queue context | `allocated_entry_index * 8` (entry size, from `QUERY_DEV_CAP`, is 8 on this card).  Index 64 → **`0x200`**.  **Not** `0x1000c0200`, not 64 |
| `log_page_size` for 4 KiB pages | `12 - 12 = 0` |
| EQ context `mtt_base_addr` split | high byte at `0x1b`, BE32 at `0x1c` |
| EQ created in the reference | `logs the ring while accepting it`: zeros before `SW2HW_EQ`, then `0xcc` in each slot's first 32 bytes and `0xee` in the second half, owner bit set in all slots | measured, A/B experiment |
| CQ ring | filled the same way by the firmware during `SW2HW_CQ` (measured: `0xcc` in all 512 slots), **but the driver must still preset every slot's owner byte to 0x80 afterwards**, because nothing else does (`mlx4_en_netdev.c:1306-1307`) |
| EQE layout with 64-byte EQEs | the legacy 32-byte EQE is the **second** half of the slot; owner byte at slot + 63 |
| CQE layout | the same: real CQE at slot + 32, owner byte at slot + 63, mask `0x80` (`cq.h:101`) |
| EQ doorbell | `BAR2 + ((eqn/4) << 12) + 0x800 + 8*(eqn%4)`; **EQN 28 → `BAR2 + 0x7800`** | source + measured |
| CQ doorbell | 8 bytes at `UAR + 0x20` (`MLX4_CQ_DOORBELL`), carrying `[(sn<<28)|cmd|cqn, ci]` |
| RX producer index | a 16-bit value written to a host-memory **record** (`mlx4_en_rx.c:250-254`), not to the UAR |
| Interrupt clear register | `QUERY_ADAPTER` reports `inta_pin = 63`; mask `swab32(1 << (63 & 31))` = `0x00000080`; register at **BAR0 + `0xf0058`** (offset 0, because 63 ≥ 32) |

## Command encodings that are easy to get wrong

| Command | in_param | in_modifier | op_modifier | Note |
| --- | --- | --- | --- | --- |
| `QUERY_PORT` `0x43` | 0 | port | 0 | outbox |
| `SET_PORT` `0x0c` | **the input mailbox's DMA address** | `(GENERAL << 8) \| port` = port | `ETH_OPCODE` = **1** (0 is IB) | |
| `INIT_PORT` `0x09` | 0 | port | 0 | new interface: **no mailbox** |
| `SW2HW_MPT` `0x0d` | mailbox DMA | MPT hardware index | 0 | |
| `HW2SW_MPT` `0x0f` | 0 | index | **1** (no outbox) | |
| `SW2HW_CQ` `0x16` | mailbox DMA | CQN | 0 | |
| `HW2SW_CQ` `0x17` | 0 | CQN | **1** | |
| `SW2HW_EQ` `0x13` | mailbox DMA | EQN | 0 | |
| `HW2SW_EQ` `0x14` | 0 | EQN | **1** | |
| `QUERY_EQ` `0x15` | 0 | EQN | 0 | outbox |

## Key transformations

```
hw_index_to_key(ind) = (ind >> 24) | (ind << 8)
key_to_hw_index(key) = (key << 24) | (key >> 8)
```

The MPT entry's `key` field and `SW2HW_MPT`'s `in_modifier` take the **raw index**; the
**lkey that goes into a WQE is the transformed one** (`mlx4_en_rx.c:59`, `mlx4_en_tx.c:873`).
Mixing them produces a descriptor the hardware cannot resolve, with no command failing.

On this card, MPT index 256 → **lkey `0x00010000`** (measured).

## The wrong-answer traps, recorded so they are not re-entered

| Trap | The rule that avoids it |
| --- | --- |
| A fill byte has bit 7 set, so "bit 7 set" cannot distinguish "preset" from "filled" | Count exact byte **values**, not bits |
| `IOLog` is not delivered early in boot | `kprintf` for the paths a hung boot would have reached (`LOGBOOT`) |
| A capability bit says what the card *can* do, not what this configuration *is* | Read the mode back (`QUERY_HCA`), do not infer it |
| A timeout is not a rejection | `CX3_FWSTATUS_TIMEOUT` is its own value, and the state is a third one: ownership unknown |
| A comparison cannot catch a truncation both sides share | Check something that needs no expected value (e.g. addresses are not below 4 GiB) |
| Not reading BAR0 `0xf03fc` between the HCA reset and `RUN_FW` completion | One read makes `RUN_FW` never complete |
| A constant read back as a value looks exactly like a measurement | `-Werror=format`; values come from `rawjson` |
