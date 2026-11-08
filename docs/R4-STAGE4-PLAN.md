# R4 阶段 4（`r4RxQp`）：RX QP 三步到 RTS 的执行方案

本文是**实现前的方案**，不是结果。所有编码都有 `vendor/freebsd-src` 的行号；所有"本卡实测值"
都标了来源。两份待你决定的取舍在第 6 节。

阶段 4 的判据（`docs/M2B-NOTES.md` §27.2）：**三条状态转换命令成功，且 `QUERY_QP` 读回预期状态。**
它不宣称能收发——那是阶段 9/10。

---

## 1. 为什么这一阶段值得单独一步

QP 不是照 CQ 那样发一个通用 `SW2HW_QP`。它是**三条按顺序的状态转换命令**，每一步都带完整上下文，
而且**同一个 context 在三步之间复用、只改状态字段**（`mlx4_qp_to_ready`，`mlx4_qp.c:894-924`）。
三处最容易错的差异已经在下面逐条钉住。

## 2. 三条命令的确切编码

来源 `mlx4_qp.c:88-187`（`__mlx4_qp_modify`）、`mlx4_qp.c:894-924`（`mlx4_qp_to_ready`）、
`cmd.h`（opcode）。

| 项 | 值 | 依据 |
| --- | --- | --- |
| opcode | `RST2INIT_QP = 0x19`、`INIT2RTR_QP = 0x1a`、`RTR2RTS_QP = 0x1b`（另有 `2RST_QP = 0x21`、`QUERY_QP = 0x22` 供清尾与验证） | `cmd.h:108-118` |
| `in_param` | **command mailbox 的 DMA 地址** | `mlx4_qp.c:183` |
| `in_modifier` | `qpn \| (sqd_event << 31)`；本项目 `sqd_event = 0` ⇒ **就是 qpn** | `mlx4_qp.c:184` |
| `op_mod` | `new_state == RST ? 2 : 0` ⇒ **我们三步全是 0** | `mlx4_qp.c:185` |
| 超时类别 | `MLX4_CMD_TIME_CLASS_C` | `mlx4_qp.c:186` |
| **mailbox 布局** | `+0` = `optpar`（BE32，本项目 0）；**`+8` = QP context** | `mlx4_qp.c:177-178` |

**`+8` 是这一阶段最容易犯的错，而且它有对照**：同一个驱动里 CQ 的 context 放在 mailbox
**`+0`**（`mlx4_cq.c:151` `cq_context = mailbox->buf;`），QP 放在 **`+8`**。我们上一阶段
（`SW2HW_CQ`）已经按 `+0` 做对了，**不要把这个习惯带过来**。

`QUERY_QP` 的响应也在 **`+8`**（`mlx4_qp.c:892` `memcpy(context, mailbox->buf + 8, ...)`），
请求是 `cmd_box(in=0, out=mailbox, in_modifier=qpn, op_mod=0, 0x22)`（`mlx4_qp.c:888-890`）。

### 2.1 三步之间复用的是同一个 context

```c
for (i = 0; i < 3; i++) {
    context->flags &= ~(0xf << 28);
    context->flags |= (states[i+1] << 28);        /* 新状态写进 flags[31:28] */
    if (states[i+1] != RTR) context->params2 &= ~MLX4_QP_BIT_FPP;
    mlx4_qp_modify(dev, mtt, states[i], states[i+1], context, 0, 0, qp);
}
```

状态枚举：`RST=0, INIT=1, RTR=2, RTS=3`（`qp.h:67-77`）。⇒ 三步分别写 `flags[31:28]` = 1、2、3。

**只有 RST→INIT 那一步**设置 MTT 与页大小（`mlx4_qp.c:170-175`）：

```
mtt_base_addr_h = mtt_addr >> 32         (QPC +0xcb, u8)
mtt_base_addr_l = mtt_addr & 0xffffffff  (QPC +0xcc, be32)
log_page_size   = page_shift - 12        (QPC +0xc8, u8)  ⇒ 4 KiB 页时为 0
```

MTT 地址用阶段 3 已经建好的那份（`mttEntrySz = 8`，所以仍是"条目索引 × 8"）。**在后续两步里
这些字段必须原样保留**（因为复用的是同一个缓冲）——这正好也是"复用一个 context"这个做法要考的。

## 3. QP context 的字段（248 字节，从 QPC+0 起算）

偏移由 `qp.h:196-236` 的结构体布局推出，并与 §27.4 的独立记录逐项对上（两处独立来源一致才写进来）。
结构体总长 **0xf8 = 248**，放在 mailbox `+8`，所以 mailbox 用到 `+0x100`（4 KiB 的 mailbox 够）。

| 偏移 | 字段 | 本项目的取值 | 依据 |
| --- | --- | --- | --- |
| `0x00` | `flags` (BE32) | `7 << 16`，**状态在 `[31:28]`** | `mlx4_en_resources.c:49` |
| `0x04` | `pd` (BE32) | 阶段 1 的 PD 编号 | `:50` |
| `0x08` | `mtu_msgmax` (u8) | `0xff`（以太网） | `:51` |
| `0x09` | `rq_size_stride` (u8) | `(ilog2(256)<<3) \| (ilog2(16)-4)` = **`0x40`** | `:52-53` |
| `0x0a` | `sq_size_stride` (u8) | `ilog2(64)-4` = **`0x02`**（dummy SQ） | `:55-56` |
| `0x0c` | `usr_page` (BE32) | **128** —— 本机实测 UAR 起始索引，且 `uar_page_sz = 0` ⇒ sw 索引 == hw 索引 | `:57`；实测见下 |
| `0x10` | `local_qpn` (BE32) | 分配到的 QPN（**每次 `mlx4_qp_modify` 都会重写**，`mlx4_qp.c:180-181`） | |
| `0x18` | `pri_path`（44 字节） | 见下 | `qp.h:140-160` |
| `0x1c` | `pri_path.counter_index` | 见第 6 节决策 ② | `mlx4_en_resources.c:60` |
| `0x20` | `pri_path.ackto` | `1` | `:58` |
| `0x38` | `pri_path.sched_queue` | `0x83 \| (port-1)<<6` ⇒ port 1 为 **`0x83`** | `:59` |
| `0x7c` | `cqn_send` (BE32) | RX CQN（阶段 2 的 128） | `:75` |
| `0x9c` | `cqn_recv` (BE32) | 同一个 RX CQN | `:76` |
| `0xa0` | `db_rec_addr` (BE64) | **`wqres.db.dma`，不左移** —— 见 §4 | `mlx4_en_rx.c:947` |
| `0xb4` | `rq_wqe_counter` (BE16) | 0（软件维护，见 §5） | `qp.h:234` |
| `0xb6` | `sq_wqe_counter` (BE16) | 0 | 同上 |
| `0xc0` | `param3` (BE32) | 见第 6 节决策 ② | `mlx4_en_rx.c:949-953` |

**实测值来源**（`usr_page = 128`）：2026-10-12 05:20 的内核日志
`UAR start index 128 = max(128 from the rule, 2 from devcap) at uar_page_sz 0 (shift 12)`。
`uar_page_sz = 0` ⇒ `mlx4_to_hw_uar_index()`（`device.h:1571-1574`）是恒等变换，所以
`usr_page` 填 128 而不是某个换算后的值。

## 4. `db_rec_addr`：计划里标"不要乘 4"的那条，现在有源码级答案

参考驱动里**同一个字段有两种写法**，而且都在同一份 `mlx4_en` 里：

| 位置 | 写法 | 是哪个 QP |
| --- | --- | --- |
| `mlx4_en_resources.c:78` | `db.dma << 2` | **steering/indirection QP**（`rss=1`），它不产生完成事件 |
| `mlx4_en_rx.c:947` | `wqres.db.dma`（**不左移**） | **真正的 RX ring QP** |

决定性的一点：`mlx4_en_resources.c` 那个 `<<2` 会被 `mlx4_en_config_rss_qp()` **显式覆盖**——
它在 `mlx4_en_fill_qp_context()` 之后单独写一行 `context->db_rec_addr = cpu_to_be64(ring->wqres.db.dma);`
（`mlx4_en_rx.c:947`）。所以：

**阶段 4（RX ring QP）用不左移的地址；那个 `<<2` 是阶段 5（indirection QP）的事，且它那份大概率是
死字段。** 计划里"RX record 不要乘 4"是对的，现在有行号了。

## 5. "发布 RQ producer"是什么操作

**不是 MMIO 门铃，是往主机内存写一个 BE32**：

```c
static inline void mlx4_en_update_rx_prod_db(struct mlx4_en_rx_ring *ring)
{
    *ring->wqres.db.db = cpu_to_be32(ring->prod & 0xffff);
}
```
（`mlx4_en_rx.c:250-254`，在 `mlx4_en_activate_rx_rings()` 里 `:501` 和 `:524` 各调一次。）

也就是说 QP context 的 `db_rec_addr` 指向的**就是**这块主机内存：HCA 读它来知道主机生产到哪了。
这与 CQ 侧相反（CQ 用 MMIO 门铃 + 主机 record 报消费）、也和 EQ 侧相反（EQ 用 MMIO 门铃）。
**阶段 4 要交付的"发布"就是这一次 4 字节主机内存写**，不是寄存器访问。

`rq_wqe_counter`/`sq_wqe_counter` 留在 0：QPC 里没有 `sq_head/sq_tail/rq_head/rq_tail`，
producer/consumer 由软件维护（`qp.h:196-237`，§27.4 已记）。

## 6. 需要你决定的两件事

### 决策 ①：QP 号怎么选（本项目"不硬编码"的规则撞上 QP 编号）

参考的 QPN 来自资源跟踪器（`mlx4_qp_alloc` 只做 ICM 记帐，**不发命令**，`mlx4_qp.c:836-870`）。
本项目现有的编号都是**按规则算出来再回读确认**（PD 从 4 起、CQN 128、EQN 28、MTT 索引 64）。

建议：**沿用同一条路子**——按规则取一个 QPN，但**用 `QUERY_QP` 回读确认它就是我们写的那个 QP**
（回读 `local_qpn` 与状态）。这样"不硬编码"仍然成立：数字来自规则，正确性来自回读。
备选：用 `qpn_calc`/`ALLOC_RES` 让固件分配（那是阶段 7 的事，现在引入会把两件事混在一起）。

### 决策 ②：三个模板字段——两个已经能读，一个只能"故意不照抄"

写方案前我担心"能力位读不到"，查完**三条都有确定答案**，而且比预想的简单：

| 参考行为 | 需要的位 | 结论 |
| --- | --- | --- |
| `param3 \|= 1<<29`（保 FCS） | `caps.flags` bit 34 `FCS_KEEP` | **读得到，直接照抄** |
| `param3 \|= 1<<30`（无 VLAN hwcsum） | 本机能力，不是 devcap 位 | 可直接定 |
| RTR→RTS 设 `roce_entropy` | `flags2` bit 33 | **能判定**（来源不是 flags2，见下），但要不要照抄是个取舍 |
| `pri_path.counter_index` | 参考取 `priv->counter_index` | 用 `0xFF`（参考自己表达"无"的方式） |

**FCS_KEEP 为什么能读到**：参考的 `caps.flags` 就是 `ext_flags << 32 | flags`
（`mlx4_fw.c:928-930`，两个源字分别在 outbox `0x40` 与 `0x44`），而**本 port 已经这样拼了**
（`mlx4_pure.h:369-370`，同一个字段在 `:167` 是 `UInt64`）。所以 `flags & (1LL << 34)` 直接可用，
**不需要动 devcap**。

**ROCE_V1_V2 的判定链比想象中短**：`flags2` 的 bit 33 不是从某个 64 位字段读的，而是
`bmme_flags & MLX4_BMME_FLAG_ROCE_V1_V2`（`mlx4_fw.c:1015-1016`，`device.h:282` 该位是 `1<<19`）。
**本 port 已经解码 `bmme_flags`**（`mlx4_pure.h:372`），而本机 05:20 的运行日志写着
`port 1 bmme_flags 0x3d5b8ec2 -> RoCE V1/V2 advertised`——bit 19 = 1，**设备确实宣称支持**。
所以 `flags2` 在本 port 是 32 位拼装字段（`mlx4_pure.h:168`）这件事**不阻塞本阶段**。

**建议（可被你否）**：

1. `roce_entropy` **先不设**（留 0），并把"设备宣称支持 RoCE V1/V2"写进日志，让这个偏离是
   **有记录的知情选择**而不是漏掉。理由：它要先对 QP 发一次 `QUERY_QP` 读回固件填的
   `remote_qpn`，再做折叠哈希（`mlx4_qp.c:932-947`）——一次额外往返 + 一个哈希函数；而它是
   RoCE **发送**侧的流哈希提示，我们的目标是**以太网 RX**。
   **证伪条件写清**：若 QP 到不了 RTS，或阶段 9/10 收不到帧，这是第一批要回来补的点。
2. `FCS_KEEP` **照抄**（`flags` bit 34）。这一条不是可选项：不处理的话**帧长差 4 字节**，
   会影响阶段 9/10 的长度判读。
3. `counter_index = 0xFF`，并在日志里说明。

## 7. 怎么验证（一次重启）

新增 `r4RxQp` 选择器，判据分三层，**都要能从值上读出来**：

| 层 | 期望 | 为什么它算证据 |
| --- | --- | --- |
| 三条命令 | 每条 `fw status 0x0`，且**每一步之后** `flags[31:28]` 与刚写的一致 | 命令"成功"不等于状态被接受；`QUERY_QP` 回读是唯一能区分的东西 |
| `QUERY_QP` | 回读的 `local_qpn` == 我们写的 QPN；状态 == `RTS(3)`；`rq_size_stride`/`sq_size_stride`/`cqn_recv`/`db_rec_addr` 与写入值逐一相等 | 一次回读同时验"是同一个 QP"和"上下文字段没有被固件改写" |
| producer 发布 | 写完 4 字节后**从主机内存读回**该 dword == 期望值 | 这是本阶段唯一的"交付动作"，必须回读而不是断言 |

判读失败时的分岔（照 §27.2 的"失败先查自己"）：

* 命令回 `BAD_PARAM(0x3)` ⇒ 先查 **mailbox `+8`** 有没有写对（这是本阶段第一大坑）。
* 命令成功但 `QUERY_QP` 状态不是 RTS ⇒ 查 `flags[31:28]` 有没有在三步之间被覆写、以及
  `params2` 的 FPP 位。
* 状态对了但阶段 9 收不到帧 ⇒ 回到决策 ②（`roce_entropy` / FCS / `counter_index`）。

## 8. 实测 / 推断 / 未测（截至写方案时）

**实测**：`usr_page = 128`、`uar_page_sz = 0`（05:20 内核日志）；阶段 1–3 全部 PASS
（`verify.sh run` 30 PASS / 0 FAIL）；阶段 3 的 MTT 索引 64、`mttEntrySz = 8`；
**`bmme_flags 0x3d5b8ec2` 的 bit 19 = 1**，即设备宣称支持 RoCE V1/V2（同一条运行日志）。
本机 `QUERY_DEV_CAP` 的 `flags` 组装方式与参考逐字一致（源字 outbox `0x40`/`0x44`）。

**推断（有源码依据、未上机）**：三步命令的编码与字段值全部照 `mlx4_qp.c` / `mlx4_en_*` 抄；
"发布 producer = 4 字节主机内存写"由 `mlx4_en_update_rx_prod_db` 的定义直接得出，但**本卡上
HCA 是否真的会去读它，只有阶段 9 才能观测**。

**未测**：本方案一行代码都没写。阶段 9/10 的收发能力不在本阶段范围内。

---

## 10. 写完方案后补的一个实测值：`FCS_KEEP = 1`（决策②不再依赖推理）

§6 决策②把 `FCS_KEEP` 写成"去查源位并照抄，查不到就明写查不到"。**查到了，而且是实测的**：
本卡 `caps.flags` 的 **bit 34 = 1**，即**支持保留 FCS**。

```
ext_flags(QUERY_DEV_CAP outbox 0x40) = 0xeeff1f9d
flags    (QUERY_DEV_CAP outbox 0x44) = 0xf677fbef
caps.flags = ext_flags << 32 | flags  = 0xeeff1f9df677fbef
bit 34 (FCS_KEEP) = 1
```

⇒ 实现**必须**走参考的 `if` 分支（`mlx4_en_rx.c:949-953`）：`param3 |= 1<<29`。
**并且阶段 9/10 要按"帧里带 FCS"处理**（多 4 字节，软件里减掉）——这正是 §6 里说"不是可选项"的那条后果。

### 10.1 测量方法，以及一个差点把结论读反的坑

```sh
# 必须先 QUERY_FW + fwload 让 RUN_FW 完成，否则 QUERY_DEV_CAP 回 fw status 0x4 (BAD_SYS_STATE)
sudo /usr/local/bin/cx3ctl rawjson 8 in 1,0 out 2 struct 272
```

读回的 `structHex` 是 **`Cx3DevCapResult`**（`src/cx3_abi.h:338-342`）：

```c
struct Cx3DevCapResult { uint64_t status; uint64_t fwStatus; uint32_t words[64]; };
```

**`words[]` 从结构体偏移 `0x10` 开始**，装的是**原始 outbox dword（主机序）**。所以
「outbox 偏移 X」对应「`structHex` 偏移 `0x10 + X`」，按 little-endian 读 dword 即得数值。

**这个 `0x10` 差点让我报出相反的结论**：第一次我漏掉它，按 `structHex` 的 `0x40`/`0x44` 读，
得到 `FCS_KEEP = 0`。修正后有两处独立校验才敢下结论：

| 字段 | outbox 偏移 | 读法 | 得到 | 已知值 | |
| --- | --- | --- | --- | --- | --- |
| `bmme_flags` | `0x94` | LE u32 @ `0x10+0x94` | `0x3d5b8ec2` | 运行日志 `0x3d5b8ec2` | ✓ 高熵值精确匹配 |
| `mtt_entry_sz` | `0x90` | **BE** u16（即 `W16()`） | `8` | profile 解码 `8` | ✓ 顺带确认字节序 |

两者读法不同不是笔误：`words[]` 存的是**主机序数值**，而 `W16()/W32()` 是对**原始 outbox 字节**
做大端读（参考 `mlx4_fw.c:989` 就是这么读 `mtt_entry_sz` 的）。这也解释了为什么
`w(0x94)` 与运行日志一致、而 `w(0x90)` 不等于 8 —— 后者该用 `W16` 读。

**可复现性**：同一读法跑了两次（各自走一遍 `reset → QUERY_FW → fwload → QUERY_DEV_CAP`），
两次逐位相同：`ext_flags 0xeeff1f9d`、`flags 0xf677fbef`、`bmme_flags 0x3d5b8ec2`，所以
`FCS_KEEP = 1` 不是一次误读。

**一个我自己踩到的次级坑，写下来免得别人重踩**：`words[]` 是主机序，所以按偏移取 dword 直接
LE 读**就能**拿到 32 位字段的数值（`bmme_flags`、`ext_flags`、`flags` 都这样读）。
但 `mtt_entry_sz` 是 **u16**，参考用 `W16()` 读——那是对**原始 outbox 字节**做大端读，
相对 `words[]` 的字节是**反的**。照前者的读法会得到 `16384` 而不是 `8`。两次都栽在同一个
地方，所以那个数字当时不能当证据；正确读法（按 4 字节一组反转再取前两字节）才给 8。

## 9. 回滚

阶段 4 是**加法**：新选择器 + 新命令常量，不改既有路径。回滚 =
`git -C tools/cx3-mlx4 revert --no-edit <该提交>` 后重构建重启；运行期退路仍是
`sudo tools/cx3-mlx4/rollback.sh [--purge]`。**注意**：阶段 4 会在硬件里留下一个 RTS 状态的 QP，
回滚后它仍在那里直到 HCA reset——`m2dbringup` 之前的 `reset` 会清掉它，这也是验证链条本来就做的事。
