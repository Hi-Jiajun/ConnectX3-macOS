# R4 阶段 9（`r4Raw`）：第一次真的让帧流动

实现前的方案。**这是 M4 里第一次验证数据通路**——前 8 个阶段全是"资源存在且状态正确"，
没有一个字节曾经流动过。编码带行号；本卡实测值标出来源。

**用户已授权不再等确认**，所以每条决定都给出我的选择与理由，并写成可证伪的形式。

---

## 1. 为什么这一阶段的风险与前 8 个不同

| | 前 8 个阶段 | 阶段 9 |
| --- | --- | --- |
| 失败面 | 命令被拒 / 状态不对 ⇒ **看得见** | 帧没发出/没收到 ⇒ **安静**（没有命令报错） |
| 判据来源 | `QUERY_*` 回读 | **内存内容 + 完成元素计数** |
| 新硬件路径 | 无 | **发送门铃（MMIO）**、**WQE owner 位**、**CQE 解析**、**回收补槽** |
| 出错时机器状态 | 通常仍在 | ⚠️ 写坏的 WQE 可能让硬件读它不该读的地方 |

⇒ 实现要**先小后大**：先**一个** WQE 发**一帧**，确认完成事件回来，再做绕环。
**不要一上来就多帧多队列。**

## 2. 发送路径编码（已核，照抄）

### 2.1 WQE = ctrl seg + data seg（`qp.h:302-330`、`:440-444`）

| 段 | 偏移 | 字段 | 取值 |
| --- | --- | --- | --- |
| **ctrl** | `+0x00` | `owner_opcode` (BE32) | **最后写**：**`MLX4_OPCODE_SEND \| (owner << 31)`** —— **opcode 不移位**（低 8 位）。★ **本方案原写的 `<< 24` 是错的**，理由见下方的更正框 |
| | `+0x04` | `vlan_tag`/`ins_vlan`/`fence_size` | 0 |
| | `+0x08` | `srcrb_flags` (BE32) | 低 8 位是 flags：bit[7] SO、[5] TCP/UDP csum、[4] IP csum、**[3:2] C = 生成完成事件**、[1] SE、[0] FL。**我们要 C=1** |
| | `+0x0c` | `imm` (BE32) | 0 |
| **data** | `+0x10` | `byte_count` (BE32) | **帧长**（含以太头，**不含 FCS**，见 §4 决策②） |
| | `+0x14` | `lkey` (BE32) | 阶段 1 的 physical MR lkey |
| | `+0x18` | `addr` (BE64) | 帧缓冲**总线地址** |

⇒ 32 字节 WQE 正好放进**一个** 64 字节 TXBB，与阶段 6 验过的 `sq_size_stride 0x42` 相符。

### 2.2 发送门铃（**本阶段第一个新硬件路径**）

```
MLX4_SEND_DOORBELL = 0x14                                   (doorbell.h:41)
writel(BE32(qpn << 8), uar_map + MLX4_SEND_DOORBELL)        (mlx4_en_tx.c:231, :918-919)
```

**不是 CQ 的 `0x20`**（`mlx4_prm.h`：`MLX4_CQ_DOORBELL = 0x20`）。两个偏移差 `0xC`，
写错会敲到别处 ⇒ **本阶段第一个要盯的地方**。

### 2.3 顺序与屏障（`mlx4_en_tx.c:897-919` 逐字）

```c
opcode |= cpu_to_be32((bf_prod & 0xffff) << 8);   /* producer 在 opcode 的 bit 8..23，单位是 TXBB */
wmb();                                            /* 描述符先落内存 */
tx_desc->ctrl.owner_opcode = opcode;              /* ★ owner 位最后写 */
wmb();
writel(... doorbell ...);
```

⇒ **（a）owner 位最后写；（b）写 owner 前后各一个写屏障；（c）producer 在 bit 8..23（TXBB 计数）。**

### 2.4 owner 位随绕环翻转（`mlx4_en_tx.c:682`）

`owner_bit = (ring->prod & ring->size) ? ... : ...` ⇒ 每绕一圈翻转一次。
**阶段 9 至少要转两次**才验得到（§5 判据 3）。

## 3. 接收路径：CQE 解析 + 回收补槽

### 3.1 CQE 位置与 owner 判据（**已有构建期门禁**）

阶段 3/4 已实现并验过：`cqNextCqe()`（`Mlx4Hw.cpp`）用
`slot = consIndex & (nent-1)`、`off = slot*stride + 32`（64 字节 CQE）；
owner 字节在 `+0x1f`，`MLX4_CQE_OWNER_MASK = 0x80`，相位 = `consIndex & nent`；
源码 `mlx4_regs.h:554-562`，门禁 `tools/test-cq-owner.c`（137 项）。
**DPDK 的独立公式已互证**（`EXTERNAL-REFS.md` §12.1：`cq->buf + cons_index * cqe_size + cqe_offset`，
`cqe_offset = 32`）⇒ **这条可当已确立。**

### 3.2 CQE 字段（**只有 FreeBSD 一个来源**，见 §4 决策③）

`vendor/freebsd-src/cq.h:41-60`（结构体 32 字节；本卡 64 字节 CQE ⇒ 取**后半**）：

| 偏移 | 字段 | 本阶段用 |
| --- | --- | --- |
| `+0x00` | `vlan_my_qpn` (BE32) | 低 24 位 = **收到这帧的 QPN**（确认是我们的 RX QP） |
| `+0x0e` | 联合体里的 `status` (BE16) | 状态位；DPDK 的 `MLX4_CQE_STATUS_*` 在 22..27（`EXTERNAL-REFS.md` §12.2） |
| `+0x12` | `byte_cnt` (BE32) | **帧长**——判据核心量 |
| `+0x16` | `wqe_index` (BE16) | **哪一槽的缓冲被用掉** ⇒ 回收补槽要用它 |
| `+0x1f` | `owner_sr_opcode` (u8) | **bit7 = owner**；低 5 位 = opcode |

### 3.3 回收补槽与绕环判据

每收到一帧：读 `wqe_index` ⇒ 那槽的缓冲归我们 ⇒ **重新 post**（写 WQE + 推进生产者的
**主机内存门铃记录**，阶段 3 已实现）⇒ 环继续转。

**绕环判据**：让 `consIndex` 走过 **> 2 × nent**（≥512 个完成），期间
**每槽被回收过至少两次**，且**同一槽不会同时被两帧占用**（用 `wqe_index` 的重现间隔判断）。

## 4. 决定（**不等确认；给出选择、理由、证伪条件**）

### 决策①：发什么帧

**选择：最小以太网帧，目的地址广播，ethertype `0x88B5`（Local Experimental Ethertype 1），
payload 补到 60 字节（不含 FCS）。**

* **无害**：`0x88B5` 是 IEEE 规定的本地实验 ethertype，局域网里没有设备会解释它。
  发 ARP 请求会真的去问网络（且需要合法源 IP），那是阶段 10 的事。
* **可判读**：内容是我们自己填的固定图案 ⇒ RX 侧收到时逐字节比对，不需要协议栈。
* **满足最小帧长**：60 + FCS = 64，避开"小于最小帧被丢弃/补齐"的歧义。

**代价**：这一帧不会有回应（对端不认识 `0x88B5`）⇒ **RX 侧的帧必须来自网络自发流量**
（ARP/DHCP/mDNS 广播），不是我们这帧的回应。这直接决定了 §5 判据 4 的读法。

### 决策②：TX 的 `byte_count` 含不含 FCS

本卡 **`FCS_KEEP = 1`**（阶段 4 实测）⇒ **收到的**帧带 FCS。发送侧呢？
参考 `mlx4_en_tx.c:764-765` 的 `bytes += max(m_pkthdr.len, ETHER_MIN_LEN - ETHER_CRC_LEN)`
**减掉了 CRC** ⇒ **发送侧 `byte_count` 不含 FCS**（硬件自己补）。

**选择：`byte_count` = 60（不含 FCS）**，与参考一致。
**证伪条件**：若 TX 完成事件报长度类错误、或阶段 10 对端看到的帧长差 4 ⇒ 回头查这条。
**注意两侧不对称**（RX 多 4 字节、TX 不多），这是硬件设计使然，不要"统一"。

### 决策③：CQE 字段偏移要不要现在就信

**选择：信，但只信要用的几个，并做成构建期测试。**

CQE 逐字节布局**只有 FreeBSD 一个来源**（`EXTERNAL-REFS.md` §12.3 明确标为未互证）。所以：
* 只依赖 `vlan_my_qpn`(QPN)、`byte_cnt`、`wqe_index`、`owner` 四个字段；
* 把偏移写成 `mlx4_regs.h` 常量 + **新 `tools/test-cqe-offsets.c`**（照 `test-m2d-offsets.c`
  的做法）——把"未互证的布局"变成"有门禁的布局"；
* 日志里**把整个 32 字节 CQE 打十六进制** ⇒ 偏移若错，第一次运行就能从字节里看出来。

### 决策④：TX/RX 一个选择器还是两个

**选择：一个 `r4Raw`，内部两段分别给标量，且 RX 段可独立运行**（TX 失败不妨碍看 RX）。
与前 8 个阶段一致（"两个答案一次重启拿到"），且 TX/RX 的失败原因完全不同。

## 5. 判据（**全部来自值，且失败读起来不同**）

| # | 判据 | 期望 | 它说明什么 |
| --- | --- | --- | --- |
| 1 | TX WQE 写对 | 内存回读 `owner_opcode` 的 owner 位 = 0（交硬件）、producer 字段正确 | 我们自己的写错了（对照阶段 6 的 `0xffffffff`） |
| 2 | TX 完成事件到了 | TX CQ 的 `cqNextCqe` 给出 CQE，`byte_cnt`/`wqe_index` 合理、status 无错 | 门铃没响 / WQE 没被接受 / CQ 接错 |
| 3 | TX 环回绕两次 | 连续 post（> 2×nent）后 owner 位按环翻转正确 | owner 翻转逻辑错（本阶段最易错处之一） |
| 4 | RX 收到过帧 | 至少一个 CQE，`vlan_my_qpn` 低 24 位 = **我们的 RX QPN 64** | 帧根本没到我们的 QP ⇒ **回到阶段 7 的 `mcast`/`base_qpn`**，**不是**阶段 8 的过滤 |
| 5 | RX 内容对 | 收到我们自己的 `0x88B5` 帧（若链路回环）或外部广播，`byte_cnt` ≥ 60 且字节可读 | — |
| 6 | 回收补槽正确 | `wqe_index` 指定的槽被重新 post；绕两圈后每槽回收 ≥2 次、无重复占用 | 回收顺序错 ⇒ 缓冲会被复用给还没读的帧 |

**判据 4 的读法要写进脚本措辞**：RX **一帧都没有**时，第一怀疑是**阶段 7 的 `mcast`/`base_qpn`**
（已源码确认 A0 下广播落点由它决定，`EXTERNAL-REFS.md` §13），**不是**"阶段 8 的过滤没配"。

## 6. 明确**不**做的（避免阶段 9 变成阶段 10）

不解析 ARP/ICMP/IP；不接协议栈（不注册 `IOEthernetController`）；不做多队列；
不做 offload（校验和/TSO/VLAN）——CQE 的 checksum 字段本阶段**只记录不判**。

## 7. 实测 / 推断 / 未测

**实测**：`sq_size_stride 0x42`（阶段 6）；`FCS_KEEP = 1`（阶段 4，两次复读）；
`qpn_calc` 接受 base 64（阶段 7，08:40）；link up 10 Gb/s；CQE 槽位 `+32`（三处独立一致）；
`MLX4_SEND_DOORBELL = 0x14` vs `MLX4_CQ_DOORBELL = 0x20`（源码）。

**推断（源码级，未上机）**：§2 全部编码；`byte_count` 不含 FCS；
"A0 下广播落到 `base_qpn`"（只有阶段 9 的 ARP 能观测）。

**未测**：一行代码未写。**本阶段是全项目第一次写 WQE 与敲发送门铃**——
WQE 编码错可能让硬件读它不该读的地方，所以 §1 的"先一个 WQE 一帧"是刻意的。

## 8.5 ★ 本方案自身的一处更正（写方案后核出来的）

§2.1 表格的 `owner_opcode` 一行原本写的是 `MLX4_OPCODE_SEND << 24 | owner << 31`。**那是错的。**

**正确**：`owner_opcode`（BE32）= **`MLX4_OPCODE_SEND | (owner << 31)`** —— **opcode 不移位**。

依据是两份独立实现一致：

* **FreeBSD**（`mlx4_en_tx.c:682-683` + `:761-762`）：
  `owner_bit = ... cpu_to_be32(MLX4_EN_BIT_DESC_OWN) : 0;`
  `opcode = cpu_to_be32(MLX4_OPCODE_SEND) | owner_bit;`
  其中 `MLX4_OPCODE_SEND = 0x0a`（`device.h:369`）、`MLX4_EN_BIT_DESC_OWN = 0x80000000`（`en.h:247`）。
* **DPDK**（`mlx4_prm.h` + `mlx4_txq.c`）：
  `MLX4_SQ_OWNER_BIT = 31`；`sq->owner_opcode = MLX4_OPCODE_SEND | (0u << MLX4_SQ_OWNER_BIT);`

⇒ 作为 BE32 值：**opcode 在低 8 位、owner 在 bit 31**。内存里（大端）**opcode 落在 byte 3、
owner 落在 byte 0 的 bit 7**。

**为什么这个错值得单独记**：若照错的写法（`<< 24`），opcode 会落到 **byte 0**（owner 那一个字节），
而 byte 3 变成 `0x00` —— 而 **`MLX4_OPCODE_NOP = 0x00`**（`device.h:365`）。
⇒ WQE 变成**空操作**：门铃敲了、完成事件**可能照样回来**、**但一个字节都不会发出去，且没有任何报错**。
这是"失败被表示成成功"的形态，而它会长成阶段 10 的"ping 不通"——归因代价高一个数量级。

**⇒ 因此 §5 判据 1（从内存回读 `owner_opcode`）现在有了具体理由**：它能在第一次运行时就暴露这种位错。
日志里打 WQE 十六进制也是同理。

## 8. 回滚

代码是加法（新选择器 + 新常量 + 新测试）。回滚 = `git revert <该提交>` 后重构建重启；运行期退路
`sudo tools/cx3-mlx4/rollback.sh [--purge]`。硬件里已 post 的 WQE 与**已发出的帧无法收回**，
但下一次 `reset` 清掉环与 QP；发出去的只有一个 `0x88B5` 广播，无害。
