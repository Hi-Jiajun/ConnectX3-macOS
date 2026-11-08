# 外部参考资料：查到什么、确认了什么、**推翻了我们什么**

起因：到目前为止本项目的编码依据**全部来自 `vendor/freebsd-src` 这一份移植基座**。去做一次
厂商文档/独立实现的检索，看有没有能直接回答"这个字段到底什么意思"的一手材料。

**结论先说：有三份可用材料，其中一份是 ConnectX-3 专属的独立实现；它当场推翻了我们一个已发货的取值。**

---

## 1. 有哪些材料，各自的适用范围（重要：不要张冠李戴）

| 材料 | 覆盖的芯片 | 可获得性 | 对本项目的价值 |
| --- | --- | --- | --- |
| **ConnectX PRM Rev ≥1.3**（2.10 那份文档写 Rev 0.7，2.31 那份写 Rev 1.3；见 §9.1） | **ConnectX-3（我们的卡）** | **要 NDA**（NVIDIA 论坛官方答复：只有有效支持合同的客户能拿） | 最高，但拿不到 |
| **Mellanox Adapters PRM, Rev 0.40**（`MLNX-15-4845`，2016-06，NVIDIA 官网**公开**）<br>`network.nvidia.com/files/doc-2020/ethernet-adapters-programming-manual.pdf` | **ConnectX-4 / 4 Lx**（标题即如此写）——**不含 ConnectX-3** | 公开 | 通用概念可用（门铃记录、WQ 访问三步），**具体字段不能直接照搬到 CX-3** |
| **DPDK `mlx4` PMD**（`drivers/net/mlx4/`，DPDK 主线的 `mlx4_prm.h` / `mlx4_txq.c` / `mlx4_rxq.c`） | **ConnectX-3 / ConnectX-3 Pro**（文档原话） | 公开（BSD-3） | **最高：它是同一颗芯片的第二份独立实现**，与我们的 FreeBSD 基座互为对照 |

**教训（记下来）**：CX-3 自己的 PRM 是 NDA 的，所以"查官方文档"这条路对我们**部分**走得通——
公开的 PRM 是 CX-4 的，**不能**当 CX-3 的规格来引用。真正有对照价值的是 **DPDK 那份独立实现**。

---

## 2. 公开 PRM（CX-4）里可以当作通用概念的：门铃记录与发帖三步

§7.4.2 Doorbell Record 与 §7.4.3.1 Posting a Work Request：

> Doorbell records are located in physical memory. **The address of DoorBell record is passed to
> the HW at RQ/SQ creation.** The receive and send DoorBell record are consecutive in memory.
> **DB records are aligned on a 4 byte boundary.** The DoorBell record denotes the counter
> (`sq_wqebb_counter` for SQ and `wqe_counter` for receive and shared RQs), which reflects the
> number of WQEs posted to this queue since its creation. Specifically, **it points to the first
> empty WQEBB (SQ) or WQE stride (RQ).** The counter wraps around at `0xFFFF` for all other
> queues. **Doorbell record should be initiated to 0 at the creation.**

发帖三步：

1. Write WQE to the WQE buffer sequentially；
2. **Update the Doorbell Record** with `sq_wqebb_counter` / `wqe_counter`；
3. **For send**, ring the DoorBell by writing to the DoorBell Register field in the UAR.

> Software can post a list of WQEs together by repeating step 1 for all WQEs, then updating the
> DoorBell record (step 2) once and ringing one DoorBell (step 3). The DoorBell record
> `wqebb_counter` should count all WQEs posted and **the `wqe_index` in the DoorBell should point
> to the first wqebb of the last WQE posted.** — **Note that this is different than the counter in
> the DB record which points to the next empty WQEBB.**

**对我们的意义（阶段 9 直接要用）**：
* 我们阶段 3/4 写的 RX 生产者（BH32 写进门铃记录）**与规格一致**：记录里的计数是生产者计数，
  指向"下一个空 WQE 步长"；创建时初始化 0（我们做了）；4 字节对齐（我们做了）。
* **但发送门铃里的 `wqe_index` 与 DB 记录里的计数是两回事**——门铃指向"最后发出的那个 WQE 的
  第一个 wqebb"，记录指向"下一个空位"。**这两个数很容易写成同一个**，那会是一个安静的错。
* **计数在 `0xFFFF` 处回绕** ⇒ 阶段 9 的环容量必须按 16 位截断来算（我们 RX 侧已按 `& 0xffff` 做）。
* "HW may execute after step 2" ⇒ 记录不是可选的；"对已执行过的 WQE 再敲门铃无害" ⇒ 重复敲安全。

---

## 3. DPDK 的 CX-3 实现：确认了什么

`mlx4_prm.h` 与 `mlx4_rxq.c`（逐条与我们的实现对照）：

| DPDK 的定义/做法 | 我们的对应 | 结论 |
| --- | --- | --- |
| `#define MLX4_CQ_DOORBELL 0x20` | 我们的 CQ 门铃偏移 `0x20` | ✅ 一致 |
| `#define MLX4_CQ_DB_CI_MASK 0xffffff` | 我们写 CQ 记录时 `cons & 0xffffff` | ✅ 一致（消费者索引 24 位） |
| `#define MLX4_SQ_STAMP_STRIDE 64` | 我们的 `SQ_RING_HEADER_SIZE = 64`、每 64 字节一个 stamp | ✅ 一致（stamp 步长 = TXBB） |
| `#define MLX4_SEG_SHIFT 4` | 我们的 WQE 步长 16 字节 | ✅ 一致（段对齐 16 字节） |
| `#define MLX4_MAX_SGE 32`、`struct mlx4_wqe_data_seg` | 我们的 data segment | ✅ 一致 |
| RX 生产者：`trxq->rq_db = rte_cpu_to_be_32(rxq->rq_ci);` **然后 `rte_wmb()`** | 我们写 BE32 生产者 | ⚠️ **我们有没有在写之前加写屏障？要查** |
| `#define MLX4_WQE_CTRL_IIP_HDR_CSUM (1<<28)`、`IL4_HDR_CSUM (1<<27)`、`RR (1<<6)` | 我们阶段 6 还没做 offload | 📌 M5 的现成编码 |

## 4. DPDK 与我们的基座**互相印证**的：owner 位与 stamp 值

这两份独立实现给出了**同一套编码**，只是命名不同：

```
FreeBSD（en.h:95-99）                      DPDK（mlx4_prm.h）
  INIT_OWNER_BIT  0xffffffff                 MLX4_SQ_STAMP_VAL  0x7fffffff
  STAMP_VAL       0x7fffffff                 MLX4_SQ_OWNER_BIT  31
  STAMP_SHIFT     31                       stamp = STAMP_VAL | (owner << 31)

FreeBSD（mlx4_en_tx.c:278-279）
  stamp = STAMP_VAL | (owner << STAMP_SHIFT)          <-- 与 DPDK 逐字同构
    owner = 1 -> 0xffffffff  (SW owns)
    owner = 0 -> 0x7fffffff  (HW owns / valid to execute)
```

⇒ **owner 位 = 1 表示"软件拥有"，0 表示"交给硬件"**；`INIT_OWNER_BIT = 0xffffffff` 正是
`STAMP_VAL | (1<<31)`（owner=1）。两份实现完全一致，**这条可以当作已确立的事实**。

---

## 5. ★ 这次检索推翻的东西：我们阶段 6 的 SQ 初始状态是错的

**参考在端口激活时做的事**（`mlx4_en_netdev.c:1373-1375`）：

```c
for (j = 0; j < tx_ring->buf_size; j += STAMP_STRIDE)
    *((u32 *)(tx_ring->buf + j)) = INIT_OWNER_BIT;      /* = 0xffffffff = SW owns */
```

即在 `memset(ring->buf, 0, ...)`（`mlx4_en_tx.c:227`）**之后**，把**每一个 64 字节 TXBB 的首
dword 都写成 `0xffffffff`（"软件拥有"，所有槽位空闲）**。

**我们阶段 6 做了什么**：SQ 的 WQE 数组**留成全零**（`memset` 之后就完了），并在前面加了一个
64 字节 dummy 头，其首 dword 写 `1 << 31 = 0x80000000`。

**两处问题**：

1. **WQE 数组全零 ⇒ 每个槽位首 dword 的 bit31 = 0 ⇒ "硬件拥有"。**
   参考的最终状态是**全部"软件拥有"**。我们的代码注释把这归因于参考的 `memset`
   ——但那个 `memset` 只是第一步，**第二步的 stamping 被漏掉了**。
   阶段 9 的驱动回收逻辑依赖 owner 位判断"哪些槽位已被硬件还回来"；
   以"全部硬件拥有"开局，意味着**每个槽位看起来都在飞行中**。
   （这也解释了阶段 6 那条判据为什么没抓到它：我们的检查是
   "读回我们写的 `1 << 31`"——**只是把自己写的东西读回来**，与"硬件怎么解释它"无关。
   这是本项目 D 家族的形状，值得单独记。）
2. **dummy 头的 stamp 值取的是 RX 环的标记（`1 << 31 = 0x80000000`），不是 TX 的 stamp 约定
   （`STAMP_VAL | owner<<31`）。** 巧合的是 `0x80000000` 的 bit31 也是 1（= SW owns），
   所以**所有权**这一层没错；错的只是低 31 位的 magic 值（`0x00000000` vs `0x7fffffff`）。
   两份实现都没有拿 `0x80000000` 当 TX 的 stamp，所以这里应当照 TX 的约定。

**修法**：SQ 建好后，照参考把**每个 TXBB 的首 dword 写成 `0xffffffff`**（软件拥有）；
dummy 头用 `STAMP_VAL | (1<<31)` 同一套编码；并把"我们读回的只是自己写的东西"这条
写进判据说明。**归入阶段 7 的批次**（那批本来要重启、升 0.43.6），不单独为它发一次。

---

## 6. 顺手拿到的阶段 9 现成编码（省掉一轮试错）

**发送门铃的格式**（`mlx4_en_tx.c:231`、`:918-919`）：

```c
ring->doorbell_qpn = ring->qp.qpn << 8;                                    /* :231 */
writel(cpu_to_be32(ring->doorbell_qpn), ring->bf.uar->map + MLX4_SEND_DOORBELL);  /* :918 */
```

⇒ **发送门铃 = 往 UAR + `MLX4_SEND_DOORBELL` 写 BE32(`QPN << 8`)**。

**发帖 WQE 的顺序与屏障**（`mlx4_en_tx.c:897-919`）：

```c
opcode |= cpu_to_be32((bf_prod & 0xffff) << 8);   /* producer 在 bit 8..23 */
wmb();                                            /* 先让新描述符落到内存 */
tx_desc->ctrl.owner_opcode = opcode;              /* owner 位最后写 */
wmb();
writel(... doorbell ...);
```

⇒ 三条要记：**（a）owner 位是最后写的**；**（b）写 owner 前后各一个 `wmb()`**；
**（c）producer 在 `opcode` 的 bit 8..23**。

**owner 位随环回绕翻转**（`:682`）：`owner_bit = (ring->prod & ring->size) ? ... : ...`
——即每绕环一圈翻转一次，这是驱动与硬件区分"这一轮的槽位"的机制。**阶段 9 必须实现它**，
否则第二个环回绕之后回收逻辑会错。

## 7. 以后遇到"这个字段什么意思"时的检索顺序

1. **`vendor/freebsd-src/` 里那份参考驱动**（我们的移植基座，逐行都有）；
2. **DPDK 的 `drivers/net/mlx4/`**（同芯片的独立实现，公开，和 1 互相对照；
   **两份不一致的地方就是要在硬件上试的地方**）；
3. **公开 PRM（`ethernet-adapters-programming-manual.pdf`）**——只当它是**通用概念**的来源
   （门铃记录、WQ 三步、owner 语义），**不要**当 CX-3 的规格；
4. CX-3 专属的 PRM Rev 0.7 需要 NDA，**不可得**，按缺失处理。

**本次检索的净收益**：确认 6 项、拿到阶段 9 的 4 类现成编码、**以及推翻一处已发货的取值**。

---

## 8. 追加：CX-3 **固件 release notes**（官方、CX-3 专属）——它回答了三件事

用户给的 `manuals.plus/m/0cfb3bb...pdf` 是 **`fw-ConnectX3` Release Notes Rev 2.10.0000**，
**不是 PRM**（2.10.0000 是 2013 年前后的老固件；我们卡上是 **2.42.5000**）。
但顺着它找到了**同一系列的更新版本**，那几份才是与我们固件同代的官方材料：

| 文档 | 覆盖 |
| --- | --- |
| `ConnectX3-FW-2_10_X000 release notes`（用户给的那份） | 很老的固件 |
| `ConnectX3-ConnectX3Pro-FW-2_40_5048-release_notes_for_Dell.pdf` | 含 **"Firmware Changes and New Feature History"** 全表 + Known Issues |
| `ConnectX3-FW-2_42_5032-release_notes.pdf` | 最接近我们（2.42.5000）的一代 |

### 8.1 ★ 它证实了我们卡的规格归属（这不是推断，是官方文字）

> Firmware `fw-ConnectX3` Rev 2.10.0000 complies with the following programmer's reference
> manual: **ConnectX Programmer's Reference Manual (PRM), Rev 0.7 or later, which has
> Command Interface Revision 0x3. The command interface revision can be retrieved by means
> of the QUERY_FW command and is indicated by the field `cmd_interface_rev`.**

而**我们卡实测 `cmd_if_rev = 3`**（`M1b VERDICT: query_fw=OK fw_ver=2.42.5000 cmd_if_rev=3`，
字段在 `QUERY_DEV_CAP`/`QUERY_FW` outbox `0x0a`，`src/mlx4_regs.h:187`）。

⇒ **字段名与取值都与官方文字逐字对应**，所以：(a) 我们的 `QUERY_FW` 解码是对的；
(b) 描述我们卡的 PRM 就是 **Rev 0.7**——也就是那份 NDA 的。

### 8.2 ★ Known Issue #2：告诉我们**该信哪个 MAC**

> **GUID** — On ConnectX-3 Ethernet adapter cards, there is a **mismatch between the GUID value
> returned by firmware management tools and that returned by fabric/driver utilities that read
> the GUID via device firmware**. Mlxburn/flint return `0xffff` as GUID while the utilities
> return a value **derived from the MAC address**. **For all driver/firmware/software purposes,
> the latter value should be used.**

⇒ 我们阶段 5 用的 MAC 来自 **`QUERY_PORT` 的 `def_mac`**（驱动路径），**正属于文档说的"该用的那个"**。
这条把一个当时只是"从设备读而不是写死"的选择，变成了**有官方依据的选择**。

### 8.3 A0 路径是我们这代固件支持的

> `2.34.5000` … • Added support for **priority based A0-DMFS mode** (For further information,
> please refer to the PRM).

⇒ A0 steering（我们阶段 7 正在实现的那条）是 **2.34.5000 引入**的，我们卡上是 2.42.5000
⇒ **在支持范围内**。细节"refer to the PRM"——又指回那份拿不到的文档。

同代还列了两条与我们有上下文关系的：`2.34.5000 • Disabled FCS checks to support switches
that replace FCS with Timestamp`（与我们的 `FCS_KEEP` 工作同域）；
`2.34.5000 • Added a new QP command "INIT2RTS_QP"`（我们走的是三步，不是这条捷径）。

### 8.4 我们固件**已经过了**的几个相关修复（说明这些区域历史上真有 FW bug）

| 修的问题 | 修于 | 我们（2.42.5000） |
| --- | --- | --- |
| **QP to firmware ownership**（固件把 QP 交给固件再交给硬件时**没检查是否已有别的固件流程持有同一个 QP**） | 2.40.5000 | 已含 |
| **Driver Start**（固件与硬件在驱动启动时的竞争**挡住了 outbound completions**） | 2.40.5030 | 已含 |
| **MAC address**（`set_port` 配的虚拟 MAC 在驱动重启后**残留**） | 2.40.7000 | 已含 |

⇒ 三条都在我们关心的区域（QP 所有权转移、驱动启动、MAC 表持久化），
**都在我们这版固件之前修掉了**。这条不是结论，但它把"这些区域会不会踩到老固件的坑"
从"未知"变成了"我们这版应当没有"。

### 8.5 关于我们固件版本的定位

NVIDIA 的 ConnectX-3 固件下载页把 **2.42.5000** 列为该卡的最后一代正式版本之一
（另有 2.42.5032 的 release notes）。所以我们卡上的是**末代固件**，
release notes 里"引入 → 修复"两列都早于它 ⇒ 我们不用担心"固件太老导致某特性没有"。

**仍然拿不到的**：PRM 级的字段定义（QPC 布局、门铃格式、A0 细节）。
那部分只能靠 `vendor/freebsd-src` + DPDK 两份实现互证——也就是本文前 7 节的用法。

---

## 9. 第三份 CX-3 官方文档（本地已有）与它带来的**一处更正**

用户提供的本地文件：`~/Downloads/683f576893f334bd3da9a0ca7f460562a55969151e96bc253bb0e140af574892.pdf`
（`device.report` 的转存，文件名即其内容哈希）。
**标题：Mellanox ConnectX®-3 Firmware and UEFI Release Notes，Rev 2.31.5814 / UEFI 10.3.34**，
22 页，`Creator` 日期 2014-07-31，`Producer: Acrobat Distiller 11.0`。

它比用户第一份（2.10.0000）新，但**仍然比我们卡上的 2.42.5000 旧一个代际**。
结构：第 2 章 变更与特性 / 第 3 章 **Known Issues** / 第 4 章 **Bug Fixes History** /
第 5 章 UEFI / 第 6 章 **Firmware Changes and New Feature History（累积表）**。

### 9.1 ★ 更正：描述我们卡的 PRM 是 **Rev 1.3 或更高**，不是 Rev 0.7

我在第 8 节根据 2.10.0000 那份写了"描述我们卡的 PRM 是 Rev 0.7"。**这份把它改掉了**：

> 1.5 Revision Compatibility — Firmware `fw-ConnectX3` Rev **2.31.5814** complies with the
> following programmer's reference manual: ConnectX Programmer's Reference Manual (PRM),
> **Rev 1.3 or later**, which has Command Interface Revision 0x3. …

两代并列：

| 固件 | 它声明的 PRM 版本 | 命令接口修订 |
| --- | --- | --- |
| 2.10.0000 | Rev **0.7** or later | 0x3 |
| 2.31.5814 | Rev **1.3** or later | 0x3 |
| **2.42.5000（我们的）** | **≥ 1.3**（未在可见文本里找到 2.42 那一行，故写"≥"） | 0x3（我们实测） |

⇒ **"Rev 0.7" 是那份老固件文档的说法**；描述我们这版的是 **≥ Rev 1.3**。
（命令接口修订**一直是 0x3**，与我们实测一致——这一条不受影响。）

### 9.2 ★ 一个必须记住的陷阱：**公开的那份"Rev 0.40"不是同一套文档**

| 文档 | 覆盖 | 版本号 |
| --- | --- | --- |
| **ConnectX PRM**（NDA，`DOC18348`） | CX-2 / **CX-3** | Rev **0.7 → 1.3 → …** |
| **Mellanox Adapters PRM**（公开，`MLNX-15-4845`） | **CX-4 / 4 Lx** | Rev **0.40**（2016-06 首发） |

**两套是不同文档，版本号不可比**。所以"公开那份是 0.40，比我们需要的 1.3 **小**，
是不是更老的一版？"——**这个推理是错的**：它们根本不是同一份文档的先后版本。
公开那份**不覆盖 CX-3**，与版本号大小无关。

### 9.3 ★ 一条与我们 M5（jumbo）直接相关的已知问题——以及它的现状

这份（2.31.5814）的 **Known Issue #7**：

> **Large MTU (9K) configuration** — Trying to configure large MTU (9K) may cause the
> **server to stop responding (hang)**. — Workaround: **N/A**，Scheduled: **Future Releases**

这是"整机挂死"级别的问题、且当时无规避手段，而**我们正在用 `eth_mtu 9900`**。
所以我追到了同代的更新文档：

* **`ConnectX3-FW-2_42_5032` 的 Known Issues 表里没有这一条**（可见部分列的条目里没有）。
  ⇒ 按可见证据，它在我们这代**应当已解决**。
* **诚实的限定**：我取到的是该表的**部分**内容，所以"未列出" ≠ "已证明修复"。
  **记为 M5 的观察项**：一旦开始做 jumbo（或把 MTU 调到 9000/9900），出现"整机卡死"时
  第一时间回想这一条。阶段 9/10 只用 1500 的最小路径不受影响。

### 9.4 Bug Fixes History 里一条我们域内的（已在我们固件之前修掉）

> **30. MTU exceptions RM#556872: Fixed an issue which caused TX traffic to stop when the
> message MTU size was larger than `QP.mtu`.** — Discovered 2.32.5100, **Fixed 2.35.5100**

⇒ 固件历史上**对 `QP.mtu` 与消息 MTU 的不一致敏感**过。我们（2.42.5000）已含此修复，
但它给出一条实现约定：**端口 MTU 与 QP context 的 `mtu_msgmax` 要保持一致**——
阶段 6 我们已经把 `mtu_msgmax = 0xff`（以太网）写进 QP，M5 调 MTU 时不要只改端口。

同表里另有两条同域（都在我们固件之前修掉）：`Steering entries overlapping caused packets
to drop in the receive due to wrong hash size calculation of QP hash folding`、
`QP Sniffer issue — Fixed a hash fold issue for sniffer QPs`（2.30.3000 → 2.30.8000）。

### 9.5 这份文档里**不用管**的部分（免得以后有人重读一遍）

第 3 章其余条目（IPMI/NC-SI/SMBus/LAN 串口/固件升降级/UEFI BIOS 菜单/PCIe SL230/SR-IOV + RH6.3）
都在带外管理或引导 ROM 的域，与我们的数据通路无关；第 5 章整章是 UEFI 驱动；
第 6 章累积表覆盖到 2.10.x，**全部早于我们固件**，因此已被第 8 节用的 2.40/2.42 两份取代。

它另引用了一份 `ConnectX3-Firmware_Archived_Bug_Fixes.pdf`——如果将来需要**更早**的修复史，
那份是入口；对我们当前目标没有增量。

### 9.6 结论：这份有用，但用法是"更正 + 一条 watch item"

| 用途 | 价值 |
| --- | --- |
| 更正 PRM 版本（0.7 → **≥1.3**）与"两套文档不可比"这条陷阱 | **高**（避免以后拿错文档或推错版本关系） |
| 9K MTU 挂死的历史记录 + 它在新版表里已不在 | **中高**（M5 的 watch item） |
| `QP.mtu` 与消息 MTU 一致性的实现约定 | **中** |
| 其余已知问题 / UEFI / 累积历史 | **低**（域外或已被取代） |

---

## 10. 固件下载页（`connectx3proen`）的结论：**对我们没用**，理由两条

用户给的 `https://network.nvidia.com/support/firmware/connectx3proen/` 是**按 PSID 下载固件镜像**
的入口（页面本身是 JS 向导，抽不出列表；实际镜像按 PSID 分发）。

### 10.1 理由一：**我们卡上的 2.42.5000 就是最后一版**

三条独立证据一致：

| 证据 | 内容 |
| --- | --- |
| 我们自己的读数 | `M1b VERDICT: query_fw=OK fw_ver=2.42.5000 cmd_if_rev=3` |
| NVIDIA `mlxfwmanager --download … --key last_release` 的输出（MFT 文档里的实例） | 列表第 2 项写 **"ConnectX3Pro (2.42.5000)"**，即"最后一版" |
| 第三方镜像页（Softpedia，**MCX354A-FCCT 专属页**） | 最新即 **2.42.5000**；更早只有 2.35.5100 / 2.34.5000 / 2.33.5100 |

⇒ **没有更新的固件可刷。**（另注：`2.42.5032` 的 release notes 存在，但它不是我们这个 PSID 的末版；
按上面第三条，MCX354A-FCCT 停在 2.42.5000。）

### 10.2 理由二（更重要）：**我们的驱动根本不需要固件镜像**

这一点从我们自己的日志就能证明，不需要外部资料：

```
fwload: FW area right after allocation, before MAP_FA:  0 of 6316032 bytes non-zero
fwload: FW area after MAP_FA:                       10574 of 6316032 bytes non-zero
fwload: FW area after RUN_FW completed:            905603 of 6316032 bytes non-zero
```

**分配之后那块区域是全零的**（是驱动给固件准备的**运行内存**，不是镜像），
然后 `MAP_FA` 让固件看见它、`RUN_FW` 启动固件，**固件自己往里写**（90 万字节非零）。

⇒ 镜像在**卡的 flash 里**，由卡的引导代码装载；驱动只提供工作区并说"开始"。
**所以固件二进制对驱动开发没有用处**——它只在"要刷新卡"时有意义，而 §10.1 说没有更新的可刷。

### 10.3 但它引出的东西有用：2.42.5000 的修复清单里有两条在我们的域

第三方镜像页列出了与 **2.42.5000** 关联的修复项（**来源是第三方镜像，不是 NVIDIA 直发**，
所以按"二手转录"对待；条目里带 RM# 编号，与官方文档的措辞风格一致）：

| 修复项 | 与本项目的关系 |
| --- | --- |
| **RX packet RM#825412**：*"Fixed an issue while **closing the HCA**, where **RX packet** caused bad access to resources that did not exist, and consequently caused the QPCGW or the irisc to get stuck."* | **我们正踩在这个域上。** "关闭 HCA 时 RX 包访问了不存在的资源"就是 **teardown 顺序**（我们的 `rqTeardown`/`cqTeardown`/`qpTeardown` 与被释放的环 buffer 之间的竞争）。修在我们**正在跑的这版固件里**——好消息，但也说明**这个域历史上真有会让硬件卡死的 bug** |
| **PCIe RM#954259**：*"a rare issue that cause the PCIe configuration cycle that arrived during the time of **sw_reset** to generate 2 completions"* | 我们的 reset 路径（`exportResetHca` 用 PCI config devctl 做复位）。同样是"已在我们的固件里修掉"，但说明 reset 期间到达的 config 周期是敏感的 |
| Flow steering / BMC RM#659925（IPv6 ping）、Link Down RM#1000626（reboot 后链路起不来）、FLR+VF 超时 RM#999432、PortRcvPkts RM#1009607 | 域外或边缘；记录以免重查 |

**怎么用这三条**：不是"要改代码"，而是**给阶段 9/10 排故障时的优先级提示**——
若出现"收包正常但关闭 / reset 时硬件卡死"，先看我们的 teardown 顺序与环 buffer 生命周期，
因为固件在那一带的历史 bug 说明硬件对顺序敏感。

### 10.4 有没有理由考虑刷固件？

**目前没有。** 三条理由：
1. 已是最新（§10.1）；
2. 驱动开发不需要镜像（§10.2）；
3. 刷写是**对卡的破坏性写操作**，而这台机器上的好状态（阶段 1–6 全绿、link up 10 Gb/s）建立在
   当前固件上——为"可能有的一点点新东西"去冒险，收益不成比例。

**唯一将来可能值得考虑的场景**：阶段 9/10 出现**指向固件**的症状（例如与 §10.3 那几条同域、
且我们已排除自己的代码），届时"换一版固件"是一个有价值的**变量消除**手段。
那要先单独评估刷写风险与回滚（卡的 flash 可备份，但刷坏需要恢复模式）。
**在动手之前要单独问用户**，不要顺手做。

### 10.5 本节的净收益

一句"没用的下载页"背后是两件事：
* **确认了固件已到末版**（省掉将来重查"有没有新版"）；
* **确认了驱动不需要固件镜像**（这一点是从我们自己的日志推出来的，不是猜的）；
* 外加**两条域内历史 bug**，作为阶段 9/10 的排障优先级提示。

---

## 11. 这张卡的官方驱动清单（含"macOS 为什么没有"）——留档以免重查

### 11.1 只有**一个驱动家族**：`mlx4`

我们这张卡（`15b3:1007`，**ConnectX-3 Pro**）在 NVIDIA 那边属于 **`mlx4`** 家族
（CX-4 及以后才是 `mlx5`）。**所有平台的官方驱动都是这一份代码的派生。**

| 平台 | 官方驱动 | 组件 | 现在的状态 |
| --- | --- | --- | --- |
| **Linux** | `mlx4_core` + `mlx4_en`（以太网）+ `mlx4_ib`（IB/RoCE） | mainline 内核 | ✅ **内核内置至今仍在**；厂商包另有 MLNX_OFED / MLNX_EN |
| **Linux（厂商包）** | **MLNX_OFED**（含 **MLNX_EN**） | 同上 + `mlx_compat`/`ib_core`/`ib_sa`/`ib_mad`/`ib_ipoib` | ⚠️ **v5.1 起 CX-3/Pro 被移出**（官方原文：*"As of MLNX_OFED v5.1, ConnectX-3, ConnectX-3 Pro or Connect-IB NICs are no longer"* supported ⇒ 改用 **MLNX_OFED LTS**） |
| **Windows** | **WinOF v5.50**（**不是 WinOF-2**） | `mlx4`, IPoIB, ND, NDK, Eth | ⚠️ 最后一版。官方原话：*"For ConnectX-4 and onwards … download WinOF-2. For ConnectX-3 and ConnectX-3 Pro drivers download WinOF."* |
| **VMware ESXi** | `net-mlx4-core` + `net-mlx4-en` VIB；MLNX_OFED for ESXi | OFED 2.4.0 base（`mlx_compat`/`mlx4_core`/`mlx4_ib`/`ib_*`） | ⚠️ 6.x 时代有；**7.0 起没了** |
| **FreeBSD** | **`sys/dev/mlx4`** | `mlx4en`/`mlx4ib`（或 `mlx4_en`/`mlx4_ib`） | ✅ 在 base 里 —— **这就是本项目的移植基座** |
| **UEFI / PXE** | FlexBoot option ROM + UEFI 驱动 | 固件自带 | — |
| **macOS** | **没有** | — | ❌ **从来没有过** |

### 11.2 macOS 这边到底有什么（重要，因为它曾经误导过我们）

Apple **确实**自带一个 Mellanox 驱动：

```
/System/Library/DriverExtensions/com.apple.DriverKit-AppleEthernetMLX5.dext
```

它的匹配条件**按厂商 ID 一网打尽**：

```
IOPCIMatch      = 0x000015b3&0x0000ffff      ← 15b3 全厂商，不限 device id
IOPCIClassMatch = 0x02000000&0xffffff00      ← 以太网类
```

**但它是 `MLX5` 的**（名字里就写着），只服务 CX-4 及以后。所以我们这张卡会被它匹配上、
然后因为芯片不是 MLX5 而**启动失败**（本机日志里的
`DriverKit_AppleEthernetMLX5 … start(…) fail` 就是这个）。设备随后留在无主状态——
**这正是我们能拿到它的前提。**

**结论**：Apple 没有、也不会为 CX-3（mlx4）提供驱动。**这就是本项目的全部理由**，
而且它是"结构性"的（Apple 只做 mlx5 那一代），不是"暂时没人做"。

### 11.3 别混淆：这些**不是**网卡驱动

| 名字 | 实际是什么 |
| --- | --- |
| **MFT**（`mstflint` / `mlxconfig` / `mlxfwmanager` / `flint`） | **固件管理工具** |
| **libmlx4** / **rdma-core** / **libibverbs** | **用户态库**（verbs 接口） |
| **FlexBoot** | **引导期网络栈**（option ROM） |
| **DPDK `mlx4` PMD** | **用户态第三方实现**（BSD-3，非厂商）——见本文 §3/§4，是我们的互证来源 |

### 11.4 对本项目的三个含义

1. **移植基座与 Linux 官方驱动同源**：FreeBSD 的 `sys/dev/mlx4` 与 Linux 的 `mlx4`
   是同一套代码谱系 ⇒ 我们的编码依据有权威性，不是二手转述。
2. **DPDK 是"同一颗芯片的另一套代码"** ⇒ 它反驳/确认我们的地方都值得上机验证
   （本文 §5 就是它推翻了我们一个已发货的取值）。
3. **整条产品线已 EOL**：固件停在 **2.42.5000**（末代，见 §10.1）、厂商 Linux 包 v5.1 砍掉、
   ESXi 7.0 砍掉、Windows 停在 WinOF 5.50。所以本项目**不是"从活跃开发里挑一个来做"**，
   而是给一张已经停止支持的卡**补一条它从来没有过的路**。

---

## 12. CQE（RX 完成元素）的两份实现互证——阶段 9 的前置作业

阶段 9 要解析 CQE，而 CQE 的字节布局只有"参考驱动"一个来源。既然 §5 那次互证抓到了真问题，
这里对 CQE 也做一遍。

### 12.1 DPDK 确认了我们已有的三件事

| DPDK（`mlx4_prm.h` / `mlx4_rxq.c`） | 我们的实现 | |
| --- | --- | --- |
| `#define MLX4_CQ_DOORBELL 0x20` | CQ 门铃偏移 `0x20` | ✅ |
| `#define MLX4_CQ_DB_CI_MASK 0xffffff` | 写 CQ 记录时 `cons & 0xffffff` | ✅ |
| **取 CQE 的公式**：`cqe = cq->buf + cons_index * cqe_size + cqe_offset` | `cqNextCqe()`：`slot = consIndex & (nent-1)`、`off = slot*stride + (cqeFactor ? 32 : 0)` | ✅ **同一约定**：64 字节 CQE 时，真正的 CQE 在槽位的**第二半（+32）** |

⇒ `cqe_offset = 32` 这条与 `src/mlx4_regs.h:554-559` 的注释一致（"the real CQE is the SECOND
32-byte half of the slot"），也与 `cx3_abi.h:1623` 里"the CQEs are at +32 in each slot"一致。
**三处独立表述一致**（FreeBSD 结构体 + DPDK 公式 + 我们的常量）。

**而且这条算术已经有构建期门禁**：`tools/test-cq-owner.c`（137 项检查）验 owner 字节的偏移，
`CQE_SLOT_OWNER_BYTE(factor)` 就是那条公式（`mlx4_regs.h:561-562`）。

### 12.2 DPDK 给的新东西（阶段 9/10 用得上）

**① 包类型 / 校验和状态位就在 CQE 里**（`mlx4_prm.h`）：

```
MLX4_CQE_STATUS_IPV4    (1 << 22)
MLX4_CQE_STATUS_IPV4F   (1 << 23)
MLX4_CQE_STATUS_IPV6    (1 << 24)
MLX4_CQE_STATUS_IPV4OPT (1 << 25)
MLX4_CQE_STATUS_TCP     (1 << 26)
MLX4_CQE_STATUS_UDP     (1 << 27)
MLX4_CQE_STATUS_PTYPE_MASK = 上面几位之或
```

⇒ 阶段 10 要"把帧交付协议栈"时，这些位是现成的 ptype/校验和依据，**不需要自己解析包头**。
（M5 的 checksum offload 也从这里读结果。）

**② 独立的 `crc_present` 印证**（`mlx4_rxtx.h` 的 `struct rxq`）：

```c
uint32_t crc_present:1;   /* CRC must be subtracted. */
uint32_t csum:1;          /* Enable checksum offloading. */
uint32_t csum_l2tun:1;    /* Same for L2 tunnels. */
```

⇒ **DPDK 也有一个"CRC 必须被减掉"的显式标志**。这与我们刚从固件 release notes 推出的结论
（§4 的 `FCS_KEEP = 1` ⇒ 收到的帧带 4 字节 FCS，软件要减）**是同一件事，两个独立来源**。
⇒ §4 那条不再是"我们自己的推导"，而是**两份实现都处理过的已知条件**。

### 12.3 还没互证的（**留给阶段 9，别假装已经验过**）

**CQE 的逐字节字段布局**（`vlan_my_qpn` / `immed_rss_invalid` / `g_mlpath_rqpn` / `sl_vid` /
`status` / `byte_cnt` / `wqe_index` / `checksum` / `owner_sr_opcode`，`vendor/freebsd-src/cq.h:41-60`）
**只有 FreeBSD 一个来源**——DPDK 那份这次没有取到它的 `struct mlx4_cqe`（提取是围绕查询词返回窗口的，
没覆盖到那一段）。

**所以**：现在**不要**把 CQE 字段偏移写成"已由两份实现确认"。到阶段 9 真正要解析时，
再定向取一次 DPDK 的 CQE 结构与它的 poll 函数，**对齐之后再写进 `mlx4_regs.h`**。
我们没有 CQE 字段偏移的构建期测试（只有 owner 字节那条），这本身也是阶段 9 该补的
（照 `test-m2d-offsets.c` / `test-cq-owner.c` 的做法）。

---

## 13. ★ 阶段 8 的预判被源码证实：**A0 下广播不需要额外动作**

`docs/R4-STAGE7-PLAN.md` §7 我写过"阶段 8 可能被阶段 7 吸收"。查完参考的多播路径，
**这条成立**，而且比预想的更干净。

### 13.1 参考在 A0 下对多播 promisc 的分支是"什么都不做"

`mlx4_en_do_multicast()`（`mlx4_en_netdev.c:844-905`）在两个方向上都按 steering 模式分支：

```c
case MLX4_STEERING_MODE_DEVICE_MANAGED:  mlx4_flow_steer_promisc_add/remove(...);  break;
case MLX4_STEERING_MODE_B0:              mlx4_multicast_promisc_add/remove(...);   break;
case MLX4_STEERING_MODE_A0:              break;      /* <-- 我们在这里：不做任何事 */
```

与我们之前确认的 `mlx4_multicast_attach` 在 A0 + `MLX4_PROT_ETH` 下**直接 `return 0`**
（`mlx4_mcg.c:1412-1415`）是同一件事的两处体现。

⇒ **A0 下多播/广播的落点完全由固件决定**，而那个落点就是 `qpn_calc` 的 **`mcast` 字段**：
我们阶段 7 填的 `MCAST_DIRECT << 30 | base_qpn` ⇒ **广播/多播送到 base_qpn（我们的 RX QPN 64）**。

### 13.2 于是阶段 8 的三种可能，按需要程度排序

| 动作 | 参考在 A0 下 | 我们需要吗 |
| --- | --- | --- |
| **让广播（ARP 用的）能到** | **什么都不做** —— A0 固件按 `mcast` 字段送 | ❌ **不需要额外命令**（阶段 7 的 `mcast` 字段就是全部） |
| 关掉多播过滤（收**所有**多播） | 只在 `IFF_ALLMULTI \| IFF_PROMISC` 时发 `SET_MCAST_FLTR(..., DISABLE)` | ⚠️ 阶段 10 若需要"全多播"才做；**ARP 不需要** |
| 注册具体多播地址 | `mlx4_multicast_attach` ⇒ A0 下 no-op | ❌ 无效动作，不要写 |

### 13.3 若将来真要做"全多播"，编码在这里（免得再查）

```
mlx4_SET_MCAST_FLTR(dev, port, mac=0, clear=0, mode)
  opcode      MLX4_CMD_SET_MCAST_FLTR = 0x48     (cmd.h:142)
  in_param    mac | (clear << 63)                = 0（两个参数都 0 时）
  in_modifier port                               (mlx4_port.c:1719)
  op_mod      mode                               = MLX4_MCAST_DISABLE(1) / MLX4_MCAST_ENABLE(2)
                                                    (mlx4.h:444-445)
  timeout     MLX4_CMD_TIME_CLASS_B
```

### 13.4 这对阶段 9 的验收判据意味着什么

阶段 8 的判据原本写着"过滤命令成功；随后**外部 ARP 广播能产生有效 RX CQE**"。
按 §13.1，**后半句其实不依赖阶段 8**，它依赖 **阶段 7（`mcast` 字段）+ 阶段 9（真的能收）**。

⇒ **验收顺序应该调整**：阶段 9 一旦收到帧（哪怕只有单播），**广播就应当同时能到**；
若单播到了而 ARP 广播没到，第一个要查的是 `qpn_calc` 的 `mcast` 字段（值对不对、命令是否真被接受），
**不是**"阶段 8 的过滤还没配"。这条写进计划，免得将来在错误的地方找原因。

**诚实的限定**：以上是源码级推演，**没有上机**。"A0 固件确实按 `mcast` 字段送广播"这一步，
只有阶段 9 的 ARP 才能观测。§13.4 的"验收顺序调整"是**基于这个推演的预期**，
若实测与它矛盾（单播到了、广播没到且 `mcast` 字段确认正确），那说明 A0 的广播路径还有别的东西，
届时要单独查——**不要把推演当成已验证**。
