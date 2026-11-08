任务：核对 R4 阶段 2（建 RX CQ）的**取值决策**。我已经从参考里读出下面这些，请逐条确认或纠正，
并指出我漏掉的必需步骤。**重点是数值与偏移，不是架构。**

## 允许阅读的路径（只读这两处）

```
/Users/you/Documents/Hackintosh-MacLocal/tools/mlx4-port/freebsd-src/   # 参考实现
/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/            # 移植代码
```

## 本卡已实测的事实（不需要重新推导）

```
steering mode = A0（QUERY_HCA: flags bit6=0, byte 0xd8 bit3=0）
entry sizes: qpc 256  cqc 128  eqc 128  srq 128  mtt 8  dmpt 64  cmpt 64 rdmarc 32
reserved:    qps 64  cqs 128  srqs 64  eqs 8  mtts 64  mrws 256  mgms 1
QUERY_FUNC:  max_eq 156  reserved_eqs 28  reserved_uars 7
devcap 的 reserved_uars 原始值 = 2
INIT_HCA 送出: uar_page_sz = 0, log_uar_sz = 10, eqe/cqe = 0x60000000（bit29 + bit30）
               qpc 0x100080000/10  cqc 0x100170000/8  eqc 0x10017a000/31
               mtt 0x1000c0000  dmpt 0x100140000  mc 0x100100000/10/7/8
EQ 已工作: EQN 28, 512 槽, 64 字节 EQE, 门铃 BAR2+0x7800（与下面的移位推算一致）
R4 阶段 1 已通过: PD 4, MPT 256, lkey 0x00010000（physical MR, iova 0, size ~0）
```

## 我的取值与依据（请核对）

**1. UAR 索引 = 128。**
依据 `device.h:1576-1579` 与 `mlx4_main.c:184-193`：

```c
static inline int mlx4_get_num_reserved_uar(struct mlx4_dev *dev)
{
	/* The first 128 UARs are used for EQ doorbells */
	return (128 >> (PAGE_SHIFT - dev->uar_page_shift));
}

static inline void mlx4_set_num_reserved_uars(struct mlx4_dev *dev, struct mlx4_dev_cap *dev_cap)
{
	dev->caps.reserved_uars = max_t(int,
		mlx4_get_num_reserved_uar(dev),
		dev_cap->reserved_uars / (1 << (PAGE_SHIFT - dev->uar_page_shift)));
}
```

本卡 `uar_page_shift = uar_page_sz + 12 = 12 = PAGE_SHIFT`（`mlx4_main.c:856`），
所以两项分别是 `128 >> 0 = 128` 与 `2 / 1 = 2`，`max` ⇒ **`caps.reserved_uars = 128`**。
于是 `mlx4_uar_alloc`（`mlx4_pd.c:145-160`，位图起点 `caps.reserved_uars`）给出的第一个
可用索引就是 **128**。

**请核对**：(a) 这个 max 是否确实覆盖了 QUERY_DEV_CAP 报的 2（也就是我不能再相信
`reserved_uars = 2`）；(b) `QUERY_FUNC` 报的 `reserved_uars = 7` 在这条链路上是否**根本不用**
（我只想确认它不是位图起点）。

**2. 进 CQ context 的 `hw_uar_index` = 128，门铃在 BAR2 + 0x80020。**
`device.h:1571-1573`：`mlx4_to_hw_uar_index(dev, index) = index << (PAGE_SHIFT - uar_page_shift)`
= `128 << 0` = **128**。`uar->pfn = BAR2 起始 PFN + index`（`mlx4_pd.c:158-160`），
所以 UAR 页字节偏移 = `index << 12` = `0x80000`，CQ 门铃 = 该页 + `MLX4_CQ_DOORBELL`
（`cq.h`/`doorbell.h`，我读到是 `0x20`）⇒ **BAR2 + 0x80020**。

**请核对这两个数字与 `MLX4_CQ_DOORBELL` 的值**；并请核对门铃的**写入形状**：
8 字节 MMIO64，值 = `[ BE32((sn<<28)|(2<<24)|cqn), BE32(ci & 0xffffff) ]`
（依据 `cq.h:138-170`）。

**3. CQE 是 64 字节，legacy 32 字节落在槽内 +32，owner 位=0x80 在 legacy 末尾。**
`cq.h:41-60` 的 `struct mlx4_cqe` 是 32 字节，`owner_sr_opcode` 在偏移 31；
`MLX4_CQE_OWNER_MASK = 0x80`（`cq.h:101`）；
`mlx4_en_rx.c:738` 的 `CQE_FACTOR_INDEX(index, factor) = ((index) << (factor)) + (factor)`
在 `factor = 1` 时给出 `2i+1`（32 字节为单位）⇒ 字节偏移 `i*64 + 32`。
所以 CQ 环 = `nent × 64` 字节，owner 字节在每槽的**第 63 字节**（与 EQE 同一约定）。

**4. owner 位必须由驱动预置。** `mlx4_en_netdev.c:1306-1307`：

```c
		for (j = 0; j < cq->size; j++)
			cq->buf[j].owner_sr_opcode = MLX4_CQE_OWNER_MASK;
```

注意这一句的 `cq->buf[j]` 是**按 32 字节步长**索引的（`buf` 是 `struct mlx4_cqe *`），
所以它其实是**每个 32 字节槽**都写了 0x80——包括 64 字节槽里那两个半区。
**请核对我的理解**：在 64 字节 CQE 模式下，驱动应该把 **legacy 半区**（每槽 +32 起的
第 31 字节，即槽内第 63 字节）写成 0x80；我还是应该按 32 字节步长**全部**写成 0x80？
这直接决定"第一个 CQE 会不会被误判为已完成"。

**5. CQ context 字段**（`struct mlx4_cq_context`，`mlx4.h:344-363`，共 128 字节 = `cqc_entry_sz`）：

| 偏移 | 字段 | 我的取值 |
| --- | --- | --- |
| `0x00` | `flags` (BE32) | `collapsed << 18` ⇒ 本阶段 0 |
| `0x0c` | `logsize_usrpage` (BE32) | `(ilog2(nent) << 24) \| 128` |
| `0x10` | `cq_period` (BE16) | 0 |
| `0x12` | `cq_max_count` (BE16) | 0 |
| `0x17` | `comp_eqn` | **28**（EQ 号，不是 vector） |
| `0x18` | `log_page_size` | 0（4 KiB 页） |
| `0x1b` | `mtt_base_addr_h` | `mtt_addr >> 32`（本卡 0） |
| `0x1c` | `mtt_base_addr_l` (BE32) | `mtt_addr & 0xffffffff` |
| `0x38` | `db_rec_addr` (BE64) | **record 的 DMA 字节地址** |

`SW2HW_CQ` = opcode `0x16`，`in_param` = mailbox dma，`in_modifier` = CQN，`op_modifier` = 0。

**6. `logsize_usrpage` 的 usrpage 部分是否就是 `hw_uar_index`？**
`mlx4_cq.c:274-276` 写的是 `mlx4_to_hw_uar_index(dev, uar->index)`，
所以我认为填 **128** 而不是原始 index 或别的编码。请核对。

**7. 建 CQ 之前要不要先清零 CQ 环内存？** 参考里 `mlx4_cq_alloc_icm` 之后我没有看到
显式 memset（`mlx4_cq.c:245-260` 附近），而 EQ 那边 `mlx4_eq.c:991-1002` 是显式 memset 的。
请核对：CQ 环是否依赖固件清零，还是靠驱动预置 owner 位就够。

## 请你回答

**A. 逐条核对 1–7**，尤其是 1（128 这个数）、2（0x80020 与门铃形状）、4（owner 预置的步长）。
给出 file:line。

**B. 我漏掉了什么必需步骤？** 例如：`SYNC_TPT`、CQ 环的 MTT 是否需要、
`num_comp_vectors` 与 vector 的关系、`MLX4_CQ_TO_EQ_VECTOR` 要不要用。

**C. 轮询路径（先不用中断）读 CQE 的判据**：我应该用
`XNOR(cqe->owner_sr_opcode & 0x80, cons_index & nent)` 吗？`cons_index & size` 里的
`size` 是 `nent` 还是 `nent-1`？请给出与参考完全一致的表达式和它的来源行。

回答用中文，逐条 A–C，区分"源码可确证"与"你的推断"，不确定写 "unknown"。
