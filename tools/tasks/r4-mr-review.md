任务：审查我刚写完的 **R4 第一阶段（PD + 物理 MR）** 实现。请逐行核对，特别是偏移、
单位、编号变换与"我以为正确但其实可能错"的地方。

## 允许阅读的路径（只读这三处）

```
/Users/you/Documents/Hackintosh-MacLocal/tools/mlx4-port/freebsd-src/   # 参考实现
/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/            # 移植代码
/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/docs/M2B-NOTES.md  # §27 是计划
```

本次要审查的代码位置：

| 文件 | 内容 |
| --- | --- |
| `src/mlx4_regs.h` | `MLX4_CMD_SW2HW_MPT`、`MPT_ENTRY_*` 偏移、`MLX4_MPT_FLAG_*`、`MLX4_NUM_PDS`、`mlx4HwIndexToKey`/`mlx4KeyToHwIndex` |
| `src/mlx4_pure.h` | 新增 `reserved_pds` 解码 |
| `src/Mlx4Hw.cpp` | `setPdRange`、`pdAlloc`、`mrEnablePhysical`、`setMptTable`、`mptEntryRead` |
| `src/Cx3Mlx4.cpp` | `exportR4Mr`；以及在 `m2dbringup` 里新增的 `setPdRange` / `setMptTable` 调用 |
| `tools/cx3ctl.c` | `cmdR4Mr`（`r4mr` 子命令） |

## 背景与已确定的实测值（都不需要你重新推导）

steering 已确认为 **A0**（`QUERY_HCA`：flags bit6=0、byte `0xd8` bit3=0）。
已实测的能力：

```
entry sizes: qp 256  cq 128  eq 128  srq 128  mtt 8  dmpt 64  cmpt 64  rdmarc 32
reserved:    qps 64  cqs 128  srqs 64  eqs 8  mtts 64  mrws 256  mgms 1
dMPT 表: dmpt 0x100140000  log 11  -> 2048 项 × 64 字节 = 128 KiB（< 256 KiB chunk）
MTT 表:  mtt 0x1000c0000  262144 字节 / 32768 项，已整表映射；reserved 前缀 64 项
QUERY_FUNC: max_eq 156  reserved_eqs 28  reserved_uars 7
EQ 已工作：EQN 28，512 槽，64 字节 EQE，MTT offset 64
```

## 我的实现做了什么（请核对的地方）

1. **PD**：`mlx4_pd.c:47-56` 是纯软件 bitmap，无命令。`mlx4_pd.c:122-124` 从
   `dev->caps.reserved_pds` 开始。我解码了 `reserved_pds`（`QUERY_DEV_CAP_RSVD_PD_OFFSET`
   的高半字节，`mlx4_fw.c:964-965`），并把它作为 `fPdNext` 的起点，
   `MLX4_NUM_PDS = 1<<15`（`mlx4.h:91`）为上界。**bump 分配，不回收**（与 `mttAlloc` 同风格）。

2. **MPT 索引**：取 `fDevCap.reserved_mrws`（本卡 256）作为第一个可用索引。
   请核对：`mlx4_mpt_reserve()` 是否确实从 `reserved_mrws` 之后开始；
   `mlx4_mpt_reserved` 用的是不是同一个数。

3. **MPT entry 的 14 个偏移**（`MPT_ENTRY_*`）：按 `struct mlx4_mpt_entry`
   （`mlx4.h:303-320`，`__packed`）逐字段抄的。请**逐字节核对**，
   特别是 `mtt_rep@0x2b`（3 个保留字节之后）、`mtt_addr@0x2c`（BE64 但只 4 字节对齐）、
   `mtt_sz@0x34`、`entity_size@0x38`、`first_byte_offset@0x3c`，以及总长度是否为 64 字节
   （本卡 `dmpt_entry_sz = 64`）。

4. **物理 MR 的 flags 组合**：`MIO | REGION | access | PHYSICAL | SW_OWNS`，
   其中 `access = MLX4_PERM_LOCAL_READ | MLX4_PERM_LOCAL_WRITE`。
   依据是 `mlx4_mr.c:648-670` 在 `mtt.order < 0` 时设 `PHYSICAL` 且 `mtt_addr = 0`，
   然后因为不是"fast register MR"再设 `SW_OWNS`。请核对这个组合与顺序，
   以及 `npages = 0` 是否确实使 `mtt.order < 0`（`mlx4_mtt_init` 在 `!npages` 时
   `order = -1`）。

5. **`pd_flags = pd | MLX4_MPT_PD_FLAG_EN_INV`**（`3 << 24`）。请核对
   `MLX4_MPT_PD_MASK` 的用法与是否需要别的位。

6. **`entity_size = 12`**（`mr->mtt.page_shift`，`MLX4_ICM_PAGE_SHIFT`），
   **不是 4096**。这是我的推断，请核对 —— `mlx4_mtt_init` 在 `!npages` 时设
   `page_shift = MLX4_ICM_PAGE_SHIFT`，而 `mlx4_mr_enable` 写
   `mpt_entry->entity_size = cpu_to_be32(mr->mtt.page_shift)`。这个字段到底期望
   shift 还是字节数？

7. **`SW2HW_MPT` 的调用形状**：`in_param = mailbox dma`、`in_modifier = mpt 索引`、
   `op_modifier = 0`。请核对 `mlx4_SW2HW_MPT`（`mlx4_mr.c:282-290`）与
   `mlx4_mr_enable` 里那个 `& (dev->caps.num_mpts - 1)` 的掩码是否需要我也做
   （我用的是 256，不会溢出 2048 的掩码，但请确认语义）。

8. **lkey**：我认为进 WQE 的 lkey = `hw_index_to_key(index)`
   （依据 `mlx4_en_rx.c:59`、`mlx4_en_tx.c:873` 用 `priv->mdev->mr.key`，
   而 `mr.key = hw_index_to_key(index)`）。但我在 `mlx4_mr.c:1033` 看到
   `fmr->mpt->lkey = cpu_to_be32(key)` 用的是**未变换**的 key。
   请判断：MPT entry 里的 `lkey` 字段该不该填？如果要填，填变换前还是变换后的？
   我把 `lkey` 字段留成 0（因为 `mlx4_mr_enable` 没填它），这是我的判断，请核对。

9. **MPT 读回**：从映射的 dMPT 表（主机内存）读回 entry，检查 flags 的**状态半字节**
   （`MLX4_MPT_FLAG_SW_OWNS = 0xf << 28`，状态 0 = 硬件持有）。请核对这个
   "写 0xf、硬件改成 0" 的方向是否正确 —— 我担心可能反过来。

10. **`setMptTable`/`mptEntryRead` 的边界**：我只用 chunk 0，并要求整表
    `<= MLX4_TABLE_CHUNK_SIZE`（256 KiB）。本卡 128 KiB 满足，但请核对
    `MLX4_TABLE_CHUNK_SIZE` 的值与 `Mlx4IcmTable::icm[]` 的分块语义是否一致
    （即 `icm[0]->virt` 是否确实是表的第一个 256 KiB）。

## 请你回答

**A. 逐项核对上面 1–10，指出任何错误**（给 file:line）。特别是第 6、8、9 这三处
我明确标了"这是我的判断"的地方——如果错了，它们都不会让任何命令失败，
只会让后面的 QP 或描述符静默无法解析。

**B. 这一阶段有没有遗漏的必需步骤？** 例如：是否需要在 `SW2HW_MPT` 之前
先做别的事（清 dMPT 表项？设置别的字段？）；`iova = 0 / size = ~0` 在这张卡、
这个固件版本上是否真的可用（A0、`cmd_if_rev 3`）。

**C. 读回检查会不会有陷阱？** 主机读到的 dMPT 表内存是否与固件写入的**同一份**？
（我担心如果 dMPT 表在 IMC/ICM 缓存里而不是直写主机内存，读回可能看到旧值——
请从参考源码判断这一点，如果判断不了就说 unknown，并给出一个能区分的方法。）

**D. 这个实现引入的风险。** 它第一次让硬件持有了一个 MPT 条目（`SW2HW_MPT` 之后
硬件拥有它）。如果后续阶段失败或需要重来，正确的撤销顺序是什么
（`HW2SW_MPT`？PD 呢？）？以及：**这一步有没有可能把整机或卡弄到需要重启/重刷的状态**？

**E. 阶段 2（RX CQ）的入口条件。** 下一步要建 CQ。请确认我现在应该用什么：
CQN 从哪个数开始（我算的是 128..255，依据 `reserved_cqs = 128` 与
`log_num_cqs = 8` ⇒ 256 项）、`comp_eqn` 填 EQN 28、`db_rec_addr` 的 rec 记录
在哪块内存、以及**是否必须先有 PD+MR 才能建 CQ**（还是 CQ 与 PD 无关）。

回答用中文，**逐条 A–E**，区分"源码可确证"与"你的推断"，不确定写 "unknown"。
不要复述背景。
