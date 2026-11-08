任务：核对一组从参考源码读出的**数值与字节偏移**，用于移植一个网卡驱动的完成队列。
请逐条确认或纠正，给出 file:line。

## 可读路径（只读这两处）

```
/Users/you/Documents/Hackintosh-MacLocal/tools/mlx4-port/freebsd-src/
/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/
```

## 设备参数（已实测，无需重新推导）

```
entry sizes: qpc 256  cqc 128  eqc 128  srq 128  mtt 8  dmpt 64
reserved:    qps 64  cqs 128  srqs 64  eqs 8  mtts 64  mrws 256
QUERY_FUNC:  max_eq 156  reserved_eqs 28  reserved_uars 7
devcap 的 reserved_uars 原始值 = 2
INIT_HCA 送出: uar_page_sz = 0  log_uar_sz = 10  eqe/cqe = 0x60000000
               cqc 0x100170000/8  mtt 0x1000c0000
EQ 已工作: EQN 28, 512 槽, 64 字节 EQE, 门铃在 BAR2+0x7800
已有一个 physical MR: PD 4, MPT 256, lkey 0x00010000
```

## 待核对的取值

**1. UAR 索引 = 128。** 依据 `device.h:1576-1579`（`mlx4_get_num_reserved_uar` 返回
`128 >> (PAGE_SHIFT - uar_page_shift)`）与 `mlx4_main.c:184-193`
（`caps.reserved_uars = max(那个值, devcap.reserved_uars / (1 << (PAGE_SHIFT - uar_page_shift)))`）。
本卡 `uar_page_shift = 0 + 12 = 12 = PAGE_SHIFT` ⇒ `max(128, 2) = 128`。
请确认 `devcap` 报的 2 会被覆盖、以及 `QUERY_FUNC` 报的 7 不是位图起点。

**2. `hw_uar_index` = 128，门铃在 BAR2 + 0x80020。**
`device.h:1571-1573` 的 `mlx4_to_hw_uar_index` 在 `shift = 0` 时返回 `index` 本身；
UAR 页偏移 = `index << 12` = `0x80000`；CQ 门铃 = 页 + `MLX4_CQ_DOORBELL`（我读到 0x20）。
请确认 `MLX4_CQ_DOORBELL` 的值与门铃的写入形状（8 字节 MMIO64）与
`cq.h` 里 `mlx4_cq_set_ci`/arm 那段的位编码一致。

**3. CQE 是 64 字节，legacy 32 字节落在槽内 +32。**
`cq.h:41-60` 的 `struct mlx4_cqe` 是 32 字节、`owner_sr_opcode` 在偏移 31；
`cq.h:101` 的 `MLX4_CQE_OWNER_MASK = 0x80`；
`mlx4_en_rx.c:738` 的 `CQE_FACTOR_INDEX(index,factor) = (index<<factor)+factor`。
请确认在 `factor=1` 时 owner 字节落在每槽第 63 字节。

**4. 每个 CQE 槽的 owner 位要由驱动在启用前写成 0x80。**
依据 `mlx4_en_netdev.c:1306-1307` 的循环。我的问题是：那个循环按 32 字节步长遍历
`cq->buf[j]`，在 64 字节 CQE 模式下这是否等价于"只写 legacy 半区"？
我应该按 32 字节步长全部写，还是只写每槽的 legacy 半区？两种情况在读取时有什么差别？

**5. CQ context 字段**（`struct mlx4_cq_context`，`mlx4.h:344-363`）：

| 偏移 | 字段 | 我的取值 |
| --- | --- | --- |
| `0x00` | flags (BE32) | 0（本阶段不做 collapsed、不做 timestamp） |
| `0x0c` | logsize_usrpage (BE32) | `(ilog2(512) << 24) \| 128` |
| `0x10` | cq_period (BE16) | 0 |
| `0x12` | cq_max_count (BE16) | 0 |
| `0x17` | comp_eqn | 28 |
| `0x18` | log_page_size | 0 |
| `0x1b` | mtt_base_addr_h | `mtt_addr >> 32` |
| `0x1c` | mtt_base_addr_l (BE32) | `mtt_addr & 0xffffffff` |
| `0x38` | db_rec_addr (BE64) | record 的 DMA 字节地址 |

**6. `logsize_usrpage` 的 usrpage 部分填 `hw_uar_index`（即 128）**，依据 `mlx4_cq.c:274-276`。

**7. 建 CQ 前要不要清零 CQ 环内存？** 我在 `mlx4_cq.c:245-270` 附近没看到显式清零，
而 EQ 那边 `mlx4_eq.c:991-1002` 是显式清零的。请确认。

## 请回答

**A. 逐条核对 1–7。**
**B. 漏掉了什么必需步骤**（例如 SYNC_TPT、CQ 环的 MTT、num_comp_vectors 与 vector 的关系）。
**C. 轮询读 CQE 的判据表达式**：请给出与参考完全一致的那一行（owner 位与
`cons_index` 的比较，含 `& nent` 还是 `& (nent-1)`）及其来源行号。

回答用中文，逐条 A–C，区分"源码可确证"与"你的推断"。
