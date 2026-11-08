# R4 阶段 10b：内核数据通路的原语、不变量与锁纪律（实现前地图）

> **这份文件是什么**：阶段 10b（把数据通路从"用户态选择器驱动"换成"网络栈驱动"）动手前的
> 精确地图。它只陈述**代码里已存在的事实**与**已写下来的实测结论**，每条都带 `file:line`。
> 它**不是**实现方案，也**没有**改动任何源文件（本次只新增本文件）。
>
> **证据分级**：每条结论标注来源——[码] = 本树源码；[实] = 文档里记的真机实测；
> [推] = 有据但未上机；[未知] = 本次查不出，明确写"未知"而不是猜。
> **猜错的代价是一台很贵才能重启的机器**。

### 定点与漂移（**读之前先看这一段**）

* 本文所有 `src/Cx3Mlx4.cpp` / `src/Cx3Mlx4.h` 的行号，都固定在 commit
  **5411d55**（`10a had two static blockers that no reboot was needed to find`，2026-10-13 08:52）
  的树上；其余文件（`Mlx4Hw.*`、`mlx4_regs.h`、`cx3_abi.h`、`Cx3Mlx4UserClient.*`、
  `tools/`）在这次读之前没有变动。
* **本工作区在并发写入，而且是同一件事**：我读这份树的过程中（08:47 → 09:03）
  已经落地了 `5411d55`，随后又有 `518c485`（`The chain now tests the interface …`），
  并且工作区里**正在写 10b 的 RX 半边**（写本文时 `git diff --stat`：`Cx3Mlx4.cpp` +339、
  `Cx3Mlx4.h` +63，未提交）。所以"当前树"的行号**随时会再变**，本文不追它。
* **怎么把本文的行号换算到别的树上**：用符号名，不要用数字。
  `git show 5411d55:src/Cx3Mlx4.cpp | grep -n '<符号名>'`，
  或在当前树里 `grep -n '<符号名>' src/Cx3Mlx4.cpp`。
* 实测漂移量（5411d55 → 09:03 的工作区），供粗换算：

  | 区域 | 偏移 |
  | --- | --- |
  | 文件开头 ~100 行 | **+1** |
  | 约 2000–9900 行（`rqTeardown` … `r4TxProbePost`） | **+12** |
  | 约 10000 行之后（`outputPacket` 起） | **+316 … +339** |

  例：本文的 `src/Cx3Mlx4.cpp:9105`（`r4RawRxRecycle`）在 09:03 的树上读作 `:9117`。
  `tools/verify.sh` 也动了：**+11**（例：本文的 `:1446`（`runcall "$SEL_R4RAW" 0 0`）
  现在读作 `:1457`）。`tools/cx3ctl.c`、`src/Mlx4Hw.*`、`src/mlx4_regs.h`、`src/cx3_abi.h`、
  `src/Cx3Mlx4UserClient.*` 在这次核对时**未变**。

---

## 0. 今天的状态：一句话

* 设备的上电、初始化、队列、端口**全部由用户态通过 40 个编号选择器**驱动
  （`src/cx3_abi.h:36-173`，`src/Cx3Mlx4UserClient.cpp:64-124`）。
* 收发**已经被证明能工作**（阶段 9，选择器 37 `r4raw`），但那条通路的形状是
  **"一次一个 WQE，每笔都同步等自己的完成"**——它在协议栈下必然卡死
  （`src/Cx3Mlx4.cpp:10003-10013`、`docs/R4-STAGE10-PLAN.md:83-91`）。
* 阶段 10a（0.44.6）已经把接口发出来了：`netup`（选择器 39）attach 接口、发 MAC 与链路状态，
  并补上了 `enable()`/`disable()`/`createWorkLoop()`（`src/Cx3Mlx4.cpp:9933`、`:9963`、`:100`）。
* **10b 缺的全部东西在两个空壳里**：`outputPacket` 只计数并丢（`src/Cx3Mlx4.cpp:10000-10014`），
  `netReceiveFrame` 是空函数且**没有任何调用者**（`src/Cx3Mlx4.cpp:10016-10026`）。

---

## 1. RX 原语

### 1.1 今天是谁在驱动它们

| 项 | 值 | 出处 |
| --- | --- | --- |
| 选择器 | `kCx3SelR4Raw = 37` | `src/cx3_abi.h:119` |
| 派发表 | `{ sR4Raw, kR4RawInCount, 0, kR4RawOutCount, sizeof(Cx3R4RawResult) }` | `src/Cx3Mlx4UserClient.cpp:121-122` |
| 锁类别 | `kXactLocked`（**整笔选择器都持事务锁**） | `src/Cx3Mlx4UserClient.cpp:218-224` |
| 入口 | `Cx3Mlx4::exportR4Raw` | `src/Cx3Mlx4.cpp:10341` |
| 工具 | `cx3ctl r4raw [posts] [rx ms]` | 用法串 `tools/cx3ctl.c:3903`；分派 `:4102` |
| 链条 | `runcall "$SEL_R4RAW" 0 0`（用驱动默认值） | `tools/verify.sh:1446` |

**RX 一帧的完整函数链（今天）**：

```
exportR4Raw                                       src/Cx3Mlx4.cpp:10341
  +- r4RawRxPoll(res, budgetMs, wait)             :9150    <- 消费 HERE
       +- Mlx4Hw::cqNextCqe(...)                  Mlx4Hw.cpp:2500   <- 就绪判定
       +- Mlx4Hw::cqSetCi(...)                    Mlx4Hw.cpp:2766   <- 发布消费索引
       +- r4RawRxRecycle(res, slot)               :9105
            +- Mlx4Hw::rqBufferPhys(...)          Mlx4Hw.cpp:2868
            +- Mlx4Hw::rqWriteWqe(...)            Mlx4Hw.cpp:2832   <- 重写 WQE
            +- Mlx4Hw::rqSetProducer(...)         Mlx4Hw.cpp:2852   <- 告诉硬件"新槽在这"
```

`exportR4Raw` 里 RX 半被调用**两次**：先排空已排队的（`wait=false`），发完帧再等一段
（`wait=true`），两处见 `src/Cx3Mlx4.cpp:10501` 与 `:10509`。

### 1.2 CQE 字段 → slot → DMA 缓冲

CQE 是 **32 字节的 legacy CQE，落在 64 字节槽的后 32 字节**（本卡 64 字节 CQE：owner 字节在
槽内 `i*64+0x3f`，`src/mlx4_regs.h:655-663`）。字段偏移（`src/mlx4_regs.h:1254-1276`）：

| 字段 | 偏移 | 类型 | 代码怎么用 |
| --- | --- | --- | --- |
| `vlan_my_qpn` | `0x00` | BE32，低 24 位 = QPN | `src/Cx3Mlx4.cpp:9183`；掩码 `MLX4_CQE_QPN_MASK = 0xffffff`（`src/mlx4_regs.h:1255`） |
| `status` | `0x10` | BE16 | 只记录、不判（`src/Cx3Mlx4.cpp:9597-9602` 的规矩） |
| `byte_cnt` | `0x14` | BE32 | `src/Cx3Mlx4.cpp:9187`。**含 FCS**（见 §1.9） |
| **`wqe_index`** | `0x18` | **BE16** | `src/Cx3Mlx4.cpp:9185-9186` → **接收环的 slot 号** |
| `checksum` | `0x1a` | BE16 | 只记录 |
| `owner_sr_opcode` | `0x1f` | u8：bit7 owner / bit6 is_send / bit[4:0] opcode | `src/Cx3Mlx4.cpp:9184`；常量 `src/mlx4_regs.h:1274-1276` |

**slot → 缓冲**：缓冲是一整块 ICM 分配，一个槽一个固定步长，但 `slot` 的字节地址**不是**
`base + slot*size`（缓冲跨页分散，见 `docs/M2B-NOTES.md:2543`），必须走
`Mlx4Hw::rqBufferPhys(bufs, bufBytes, index)`（`src/Mlx4Hw.cpp:2868-2881`：
`page = index / perPage`，`inPage = (index % perPage) * bufBytes`）。
读帧用 `fR4RqBufs->virt + slot * fR4RqBufBytes`（`src/Cx3Mlx4.cpp:9260-9261`）。

**两个索引的关系（1:1 假设）**：参考驱动**根本不读 `wqe_index`**，它假设"完成数 == 描述符数"，
用消费索引自己算槽（`src/Cx3Mlx4.cpp:9125-9134` 的注释，引 `mlx4_en_rx.c:762-765`）。
本树两样都算并**互相比对**：`slot == (cons & (nent - 1))`（`src/Cx3Mlx4.cpp:9308`）。
⇒ **一个 RX CQ 上只能有一个消费者**，否则这条判据必然对不上——这正是 10b 与 `r4raw`
共存的硬约束（见 §4.2）。

### 1.3 就绪判定（owner 位 / 相位）

`Mlx4Hw::cqNextCqe`（`src/Mlx4Hw.cpp:2500-2521`）：

```
owner = (cqe[CQE_OWNER_OFFSET] & MLX4_CQE_OWNER_MASK) ? 1 : 0;   // 0x1f, 0x80
phase = (consIndex & nent) ? 1 : 0;
if (owner != phase) return false;      // 不相等 = 还没写
```

⇒ 只看**持久消费索引指着的那个槽**，不看别的槽；每次只前移一格。索引的相位与环大小共同
决定"哪一圈"。`nent` 必须是 2 的幂（同一行用 `&`）。

### 1.4 消费：索引前移 + 发布 CI（两个动作）

`src/Cx3Mlx4.cpp:9178-9179`：

```
fR4RawRxCons = cons + 1;
if (fR4CqRec) fHw.cqSetCi((volatile UInt32 *)fR4CqRec->virt, fR4RawRxCons);
```

* `Mlx4Hw::cqSetCi`（`src/Mlx4Hw.cpp:2766-2771`）**只写宿主内存记录的第 0 个 dword**
  （BE32，低 24 位），带一次 `mlx4Wmb()`，**没有任何 MMIO**。
* 那块记录就是 CQ 上下文 `db_rec_addr` 指的东西（`src/Mlx4Hw.cpp:2655` 写入；
  `fR4CqRecPhys` / `fR4CqUarStart` / `fR4CqConsIndex` 在 `src/Cx3Mlx4.cpp:2425`、`:2506-2507` 落定）。
* **顺序**：先动索引、再动别的（注释在 `src/Cx3Mlx4.cpp:9176-9177`）——
  索引一动，这个槽就不归我们了。

### 1.5 补槽（refill）：写哪几个字段、什么顺序

`Cx3Mlx4::r4RawRxRecycle`（`src/Cx3Mlx4.cpp:9105-9123`），顺序**固定**：

| 步 | 动作 | 行 |
| --- | --- | --- |
| 1 | `phys = rqBufferPhys(fR4RqBufs, fR4RqBufBytes, slot)`；`phys == 0` 就**不补**（宁缺不错） | `:9107-9117` |
| 2 | `byteCount = fR4RqBufBytes - MLX4_NET_IP_ALIGN`（2048-2，**不是帧长**） | `:9108` |
| 3 | `rqWriteWqe(ring, RX_WQE_SIZE, slot, phys, byteCount, fR4RqLkey)` | `:9119` |
| 4 | `++fR4RawRxProd; ++fR4RawRxRecyclesTotal;` | `:9120-9121` |
| 5 | `rqSetProducer(fR4QpRec->virt, fR4RawRxProd)` | `:9122` |
| 6 | `r4RawRxSetInFlight(slot, true)`；`++res->rxRecycles` | `:9118`、`:9119` 起 |

`rqWriteWqe`（`src/Mlx4Hw.cpp:2832-2842`）只写 **3 个字段**，偏移见 `src/mlx4_regs.h:701-704`：
`byte_count`(`0x00`)、`lkey`(`0x04`)、`addr`(`0x08`)。
**RQ 的 WQE 没有 owner 位、没有 fence_size、没有 opcode**——它是"生产即有效"的：
调用者**先把帧读出来，再重写 WQE，最后才发布生产者**（顺序的理由写在
`src/Cx3Mlx4.cpp:9089-9095`；判据 6 就是为了把"没读先还"这种情况抓住）。

`rqSetProducer`（`src/Mlx4Hw.cpp:2852-2857`）：写记录 word0，宽度 **16 位**
（`RX_PROD_DB_WIDTH = 0xffff`，`src/mlx4_regs.h:724`），值是**累计计数**不是环索引
（参考 `mlx4_en_rx.c:250-254`）。

**这就是 RX 侧的"arm"**：RQ 没有别的门铃；把生产者发布出去，硬件才知道有新工作。

### 1.6 CQ 的通知（arm）——今天没有数据通路在用，而它是 10b 的命门

`Mlx4Hw::cqSetArmDb`（`src/Mlx4Hw.cpp:2773-2778`）+ `Mlx4Hw::cqRingDoorbell`
（`src/Mlx4Hw.cpp:2780-2800`）实现 `mlx4_cq_arm` 的两半：

1. 记录 **word1** 写 `[(armSn&3)<<28 | cmd | cqn]`；
2. 8 字节 MMIO 到 `BAR2 + mlx4UarPageOffset(uar) + MLX4_CQ_DOORBELL(0x20)`，
   值 `[sn|cmd|cqn, ci]`，`cmd = MLX4_CQ_DB_REQ_NOT`（`src/mlx4_regs.h:666-674`）。

**今天唯一的调用者在 `txprobe` 的实验 1 里**：`src/Cx3Mlx4.cpp:10254`（记录）与
`:10256`（MMIO）。`r4raw` **从不 arm** ⇒ 阶段 9 的接收通路**完全不依赖任何 CQ 事件**
（它只轮询环）；而且那次唯一的 arm 尝试也没有观测到任何 COMP EQE（下一张表）。

⇒ **10b 的接收投递必须先解决这件事**，而它恰好是阶段 9 唯一没结论的一条：

| 事实 | 出处 |
| --- | --- |
| arm 实验（UAR+0x20，8 字节，记录与门铃都写对）实测 **零 EQE**，而同期该 CQ 上有几十个完成 | `docs/M2B-NOTES.md:4767-4775`（§54.1） |
| 当时的两种解释都**没有被排除**：(a) 门铃通路真有问题；(b) **测试本身不成立**——mlx4 的 arm 语义是"arm 之后有新完成时通知"，而那个 RX CQ 在 arm 时**从未为空**，参考的用法是"**先消费空再 arm**" | `docs/M2B-NOTES.md:4818-4826`（§54.5） |

⇒ 10b 的正确做法（也是最小先验条件）：**先把 RX CQ 排空，再 arm**，然后判"COMP EQE 有没有到"。
在此之前，"RX 用中断投递"这条链**一段都没被证明过**。

### 1.7 EQ 侧：COMP 事件、消费、re-arm

* COMP 事件类型 `MLX4_EVENT_TYPE_COMP = 0x00`，payload 是 `cqn`（BE32，偏移 `0x04`）：
  `src/mlx4_regs.h:1442-1443`。
* EQ 消费者 `Mlx4Hw::eqNextEqe`（`src/Mlx4Hw.h:907`），64 字节 EQE 的 legacy 半槽：
  `src/mlx4_regs.h:1408-1423`。
* re-arm：`eqSetConsumerIndex(doorbell, cons, reqNot=true)`（`src/Mlx4Hw.cpp:5498-5514`），
  门铃地址 = `BAR2 + ((eqn/4) << 12) + 0x800 + 8*(eqn%4)`（`src/mlx4_regs.h:1461-1462`），
  值 bit31 = request notification。**每次扫完都 re-arm，扫到没扫到都 re-arm**
  （`src/Cx3Mlx4.cpp:5638`，理由见 `:5631-5637`）。
* **今天 `consumeEqLocked` 的 type-0 规则（10b 必须改）**：`src/Cx3Mlx4.cpp:5592-5604`
  —— 看到 type 0（COMP）就 `++fIrqErrors`、置 `blockedByComp`、**break（留在原地不消费）**，
  理由是"本驱动当时没有 CQ，没人能消费它"。一旦 `netup` 之后有 CQ 在跑，
  **这条规则会让 EQ 永久卡在第一个 CQ 事件上**：不消费 ⇒ 不 re-arm ⇒ 之后连 CMD 完成都收不到。
  这不是"优化空间"，是必须与 10b 同时改的一条（见 §5.3）。

### 1.8 每一步改了哪些成员

| 步 | 改的成员 | 行 |
| --- | --- | --- |
| 消费 | `fR4RawRxCons`、`fR4CqRec->virt[0]`（记录） | `:9178-9179` |
| 计数 | `res->rxFrames` / `res->rxQpnMatches` / `res->rxFirst*` | `:9181` 起 |
| 判据 | `res->rxMinByteCnt` / `rxContentOk` / `rxRecycleViolations` / `rxViolationSlot` / `rxViolationCons` | `:9252-9254`；三处 violation 累加在 `:9302`、`:9318`、`:9330` |
| 在飞图 | `fR4RawRxInFlight[]` | `:9080-9090`（置位/清位）、`:9118`、`:9338` |
| 补槽 | `fR4RawRxProd`、`fR4RawRxRecyclesTotal`、`fR4RawRxRecycles[slot]` | `:9120-9121`、`:9126` |
| 出口 | `res->rxCqConsAfter` | `:9351` |

初始值：`init()` 里全部清零（`src/Cx3Mlx4.cpp:262-266`）；
**首次调用时从设备侧种子**：读回 RQ 生产者记录（`prodNow`），非 0 才认，
并把全部槽置为 in-flight（`src/Cx3Mlx4.cpp:10462-10482`）。

### 1.9 交付给协议栈前必须做的两件事（今天都还没做）

1. **`byte_cnt` 含 FCS，要减 4**。本卡 RX 路径设了 `FCS_KEEP`
   （`src/Mlx4Hw.cpp:3682-3690`：只有接收侧设 `MLX4_QP_PARAM3_FCS_KEEP`；
   `src/mlx4_regs.h:541-553`），而 CQE 的 `byte_cnt` 是**含 FCS 的帧长**
   （`src/mlx4_regs.h:1263`、`:1242`）。实测：68 = 64 字节帧 + 4 字节 FCS
   （`docs/M2B-NOTES.md:4020-4024`；`docs/COVERAGE.md:81`）。
   ⇒ 交给栈的长度是 `byteCnt - 4`（`docs/R4-STAGE10-PLAN.md:91-96` 把这条列为
   "两个容易搞错的点"之一）。
2. **先交付、再补槽**。顺序理由同 §1.5：先补槽会让硬件把新帧写进我们还没读完的缓冲
   （`docs/R4-STAGE10-PLAN.md:95`；代码侧的对应纪律 `src/Cx3Mlx4.cpp:9089-9095`）。

### 1.10 阶段 9 已经证明 / 还没证明

| 结论 | 状态 | 出处 |
| --- | --- | --- |
| 广播帧真的进了我们的 RX QPN 64（真 ARP 帧逐字节打出来） | **已证**（51/60/49 个完成，全部命名我们的 QPN） | `docs/M2B-NOTES.md:4006-4024`、`docs/COVERAGE.md:81` |
| CQ 的消费/发布、RQ 的 WQE 格式与补槽顺序 | **已证**（判据 6 在实测里没出现 violation） | `docs/R4-STAGE9-RESULT.md`（六条判据）、`docs/COVERAGE.md:81` |
| **CQ 通知（arm → COMP EQE → EQ）** | **未证，且实验结果是"零 EQE"** | `docs/M2B-NOTES.md:4767-4775`、`:4818-4826` |
| 一个 CQ 上多消费者共存 | **未测（今天只有 `r4raw` 一个消费者）** | 本树 |

---

## 2. TX 原语

### 2.1 函数与签名

| 函数 | 签名（原样） | 行 |
| --- | --- | --- |
| 整个发送相 | `void Cx3Mlx4::r4RawTransmit(Cx3R4RawResult *res, UInt32 postsWanted)` | `src/Cx3Mlx4.cpp:9369` |
| 单笔探测投递 | `UInt32 Cx3Mlx4::r4TxProbePost(UInt32 opcode, UInt32 nDataSegs, UInt32 recWord0, UInt32 recWord1, UInt32 waitMs, UInt32 *outOpcode, UInt32 *outStatus, UInt32 *outWqeOk)` | `src/Cx3Mlx4.cpp:9755-9756` |
| **建 WQE（唯一的编码器）** | `void Mlx4Hw::sqWriteSendWqe(UInt8 *sqVirt, UInt32 index, UInt32 prod, UInt32 ownerBit, UInt32 byteCount, UInt32 lkey, UInt64 bufPhys, UInt32 nDataSegs, UInt32 opcode, UInt8 *outImage32, bool verbose)` | `src/Mlx4Hw.cpp:2901-2904` |
| **敲发送门铃（唯一的 MMIO）** | `void Mlx4Hw::sqRingSendDoorbell(UInt32 qpn, UInt32 uarIndex, bool verbose)` | `src/Mlx4Hw.cpp:3023-3024` |
| 写门铃**记录**（宿主内存，不是 MMIO） | `void Mlx4Hw::sqSetDoorbellRecord(volatile UInt32 *record, UInt32 word0, UInt32 word1)` | `src/Mlx4Hw.cpp:2803` |
| 回收后盖章 | `void Mlx4Hw::sqStampTxbb(UInt8 *sqVirt, UInt32 index, UInt32 owner)` | `src/Mlx4Hw.cpp:3004` |
| 建 SQ（并把每个 TXBB 盖 `0xffffffff`） | `bool Mlx4Hw::sqCreate(...)` | `src/Mlx4Hw.cpp:3250`；盖章 `:3316-3319` |

调用点：`sqWriteSendWqe` → `src/Cx3Mlx4.cpp:9452`；`sqRingSendDoorbell` → `:9543`；
`sqStampTxbb` → `:9671`；`sqSetDoorbellRecord` → `:9799`（txprobe）与 `:10322`（txprobe 收尾复位）。

### 2.2 一个 SEND WQE 的字节布局（ctrl + 1 data seg = 32 字节）

偏移全部在 `src/mlx4_regs.h:1163-1176`，写入在 `src/Mlx4Hw.cpp:2930-2949`：

| 偏移 | 内容 | 值 / 来源 |
| --- | --- | --- |
| `0x00` | BE32 `owner[31] \| prod[8..23] \| opcode[0..7]` | `dword = (opcode&0xff) \| ((prod & 0xffff)<<8) \| ((ownerBit&1)<<31)`（`src/Mlx4Hw.cpp:2963-2966`；常量 `src/mlx4_regs.h:1148-1152`） |
| `0x04..0x05` | `vlan_tag` = 0（与 `bf_qpn` 共用） | `src/Mlx4Hw.cpp:2930-2931` |
| `0x06` | `ins_vlan` = 0 | `src/Mlx4Hw.cpp:2932` |
| `0x07` | **`fence_size`** | `fenceSize = TX_WQE_FENCE_SIZE(nDataSegs)`（`src/Mlx4Hw.cpp:2933`） |
| `0x08` | `srcrb_flags` | `MLX4_WQE_CTRL_CQ_UPDATE \| MLX4_WQE_CTRL_SOLICITED`（`src/Mlx4Hw.cpp:2936-2937`） |
| `0x0c` | `imm` = 0 | `src/Mlx4Hw.cpp:2938` |
| `0x10` | `byte_count`（帧长，**不含 FCS**） | `src/Mlx4Hw.cpp:2947`；r4raw 传 `CX3_R4RAW_FRAME_BYTES = 60`（`src/mlx4_regs.h:1308`） |
| `0x14` | `lkey`（MR 的**转换后** lkey，`fMrLkey`） | `src/Mlx4Hw.cpp:2948` |
| `0x18` | `addr`（BE64，帧缓冲总线地址） | `src/Mlx4Hw.cpp:2949` |

**写入顺序与两次屏障是参考的三步，不是风格**（`src/Mlx4Hw.cpp:2889-2899`）：
字段 → `wmb()`(`:2953`) → **owner_opcode 最后写**(`:2966`) → `wmb()`(`:2969`) → 调用者敲门铃。
理由：owner 字之前的一切，硬件只在 owner 说"归我"之后才读；反过来写 = 硬件可能读到一个正在
被写的描述符，**这种失败没有任何报错**。

屏障本体是 `mlx4Wmb() = __sync_synchronize()`（`src/Mlx4Hw.cpp:98-101`）——
0.43.x 那个 `OSSynchronizeIO()` 在 x86_64 上**是空函数**（`src/Mlx4Hw.cpp:60-96` 的整段论证）。

### 2.3 门铃**记录** vs 门铃 **MMIO**（两个不同的东西，别混）

| | 宿主内存记录（`db_rec_addr` 指的） | MMIO 门铃 |
| --- | --- | --- |
| 位置 | `fR4TxQpRec->virt` / `fR4QpRec->virt`（ICM 一页） | `BAR2 + UAR页 + 0x14`（发送）/ `+0x20`（CQ） |
| 谁写 | RX 侧：`rqSetProducer` 写 word0（`src/Mlx4Hw.cpp:2852`）。**TX 侧今天没有任何路径写它** | `sqRingSendDoorbell`（`src/Mlx4Hw.cpp:3023`），值 `BE32(qpn << 8)` |
| 值含义 | 累计计数（RX 生产 / TX 生产，两半**相邻**） | **只有 QPN**（不是生产者索引） |
| 依据 | `src/mlx4_regs.h:1224-1225`（0x14、shift 8）；PRM "接收与发送记录相邻"见 `docs/M2B-NOTES.md:4684-4698` | 同左 |

QP 上下文里 `db_rec_addr` 写的是**未移位**的地址（RX QP：`src/Cx3Mlx4.cpp:2996`；
TX QP：`:8657` 明确标注 `/* UNSHIFTED */`）。只有阶段 5 那个 indirection QP 用了参考的
`<< 2` 形式（`src/Cx3Mlx4.cpp:3798-3805`），那是"逐字照抄参考"的单独决定，与 10b 无关。

**今天 TX 记录是空写的**（重要）：

* `docs/M2B-NOTES.md:4619-4636`（§51.4）逐树 grep：参考里写门铃记录的地方只有三处
  （分配时置 0、RX 生产者、CQ arm），**SQ 的记录没有任何实现写**；
* 唯一的例外是 txprobe 的实验 3（变体 V0..V3：不写 / 写 word0 / 写 word1 / 都写），
  实测**四个变体全都没有变化**（`docs/M2B-NOTES.md:4767-4775`）；
* ⇒ 门铃记录**不是**发送不通的原因；10b 不需要写它（写了也不构成"已证明必要"）。
  但 `docs/M2B-NOTES.md:4818-4826` 提醒：那条 arm 实验不成立，**不要**把"记录不是问题"
  推广成"门铃家族都不是问题"。

### 2.4 owner 位 / `fence_size` / stamp：阶段 9 踩过的三处

**(a) owner 位 = 环的圈相位**：`ownerBit = (prod & nent) ? 1 : 0`（`src/Cx3Mlx4.cpp:9434`，
参考 `mlx4_en_tx.c:682`），`prod` 单位是 **TXBB**（64 字节，`src/mlx4_regs.h:737`），
槽号 `index = prod & (nent-1)`（`src/Cx3Mlx4.cpp:9433`）。所以**owner 位每隔一圈翻转一次**，
而"某圈只跑过一次"永远发现不了相位错误（`src/Mlx4Hw.cpp:2955-2962`；
读回侧对 owner/prod 的掩码纪律见 `src/Cx3Mlx4.cpp:9468-9475`）。

**(b) `fence_size` = 描述符总长 / 16（含控制段），不是数据段个数**：

```
#define TX_WQE_FENCE_SIZE(ds)   (((16 + (ds)*16) / 16) & 0x3f)     src/mlx4_regs.h:1206-1207
#define TX_WQE_SEND_FENCE_SIZE  TX_WQE_FENCE_SIZE(1u)                src/mlx4_regs.h:1210  ->  2
```

写 **1** 等于声明"这个描述符 16 字节、没有数据段"= 一个没有数据可发的 SEND ⇒
**无帧、无完成、无报错**，四次上机看到的正是这个（`docs/M2B-NOTES.md:4788-4817` §54.3；
三份实现互证：Linux `en_tx.c:648/:1075/:1082`、libmlx4 `qp.c:269/:446/:449/:78`、
FreeBSD `mlx4_en_tx.c:688/:808/:842`）。0.43.12 的修法是**让调用方传数据段个数、字节由公式
导出**（`src/Mlx4Hw.cpp:2933`），并在 `tools/test-cqe-offsets.c` 里加编译期断言
（=2、=公式、**≠1**、**≠16/16**）。r4raw 的读回检查改成与 `TX_WQE_SEND_FENCE_SIZE` 比
（`src/Cx3Mlx4.cpp:9490`）。

**(c) 回收时盖的 stamp 必须带"该槽自己那一圈的相位"**：`sqStampTxbb(sqVirt, index, ownerBit)`
（`src/Cx3Mlx4.cpp:9671`），值 `MLX4_SQ_STAMP_VAL | (owner<<31)`（`src/Mlx4Hw.cpp:3004-3012`；
常量 `src/mlx4_regs.h:770-772`）。曾经写成"**下一笔**的相位"，两者只在**每圈最后一个 TXBB**
上不同——而那正是硬件绕圈后看的那个槽：实测两圈零错误、环的最后一个槽没有完成，之后每一笔
都回来 ERROR 完成（syndrome 0x05 / vendor 0xf9）。完整论证在 `src/Cx3Mlx4.cpp:9653-9670` 的注释里。

> **文档缺口（本次发现）**：代码里有几处把这个结论引成 `docs/M2B-NOTES.md 55`
> （`src/Cx3Mlx4.cpp:9664`、`:9793`、`:10330`、`src/Cx3Mlx4.h:251`），
> 但 **`docs/M2B-NOTES.md` 到本次读为止最后一节是 §54，§55 不存在**（文件共 4826 行）。
> 该结论的唯一现存记录是上面那段代码注释与 `docs/COVERAGE.md:81`（0.43.7 的实测行）。
> 这是**文档与代码不同步**，不是代码问题。

**(d) 建队列时的初始印章**：每个 64 字节单元首 dword 写 `MLX4_SQ_INIT_OWNER_BIT`
（`= 0x7fffffff | 1<<31 = 0xffffffff`，"软件拥有"）——`src/Mlx4Hw.cpp:3316-3319`，
判据在 `tools/verify.sh:1346`。若这里是 0，则"硬件拥有每一个槽"。

### 2.5 门铃的值与地址（唯一的 MMIO 写）

`src/Mlx4Hw.cpp:3023-3049`：`off = mlx4UarPageOffset(uarIndex) + 0x14`；
`val = (qpn & 0xffffff) << 8`；越界（`off+4 > 0x800000`）就**拒绝写并报错**（不是静默）。
UAR 索引来自 CQ 的上下文（`fR4CqUarStart`；r4raw 把它输出成 `res->txUarIndex`，
`src/Cx3Mlx4.cpp:9379`），与 CQ 门铃共用同一页（页内 0x14 vs 0x20，差 0xC）。
实测 BAR2 是 8 MB、`0x80014` 在窗口内（`docs/M2B-NOTES.md:4619-4636` 第 5 行）。

### 2.6 TX 完成如何识别与消费

`src/Cx3Mlx4.cpp:9558-9600`（r4raw）与 `:9829-9855`（txprobe 复用同一套）：

1. 就绪：`cqNextCqe(fR4TxCqRing, fR4TxCqNent, 1, fR4RawTxCons, cqe)`（`:9561`）；
2. 取 `opcode = cqe[0x1f]`；`err = ((opcode & 0x1f) == MLX4_CQE_OPCODE_ERROR(0x1e))`；
   `isSend = opcode & 0x40`；`wqeIdx = BE16 @0x18`；
3. 消费：`fR4RawTxCons = cons + 1`（`:9571`）+ `cqSetCi(fR4TxCqRec, ...)`（`:9573`）；
4. **判据**：`txCqeOk = (!err && isSend && wqeIdx == 该笔投递的 TXBB)`（`:9603`）。
   **`byte_cnt` / `status` / `checksum` 只记录、不判**（`:9597-9602`：参考的发送轮询只读
   opcode 与 wqe_index，没有第二份来源能解释它们的含义）。

**生产者先动、再等完成**：`fR4RawTxProd = prod + 1`（`:9550`）在 `cqNextCqe` 等待**之前**，
理由是"硬件自己的计数才是等待的对象"（`:9548-9549`）。

### 2.7 今天的 TX **没有**的东西（10b 最大的结构性缺口）

* **没有在飞记录**：没有"哪个 TXBB 属于哪个 mbuf"的数组。整个发送相共用一个帧缓冲
  `fR4RawFrame`（一页 ICM，`src/Cx3Mlx4.cpp:9030-9072`）——上一次投递的帧还在被硬件读、
  下一次投递就覆盖它；阶段 9 之所以安全**只因为它一次只发一笔并等完成**。
* **回收 = 只有盖 stamp**（`:9671`），没有任何"归还缓冲"的动作。
* **没有异步完成路径**：`r4RawTransmit` 内联了一条 `IOSleep(1)` 的等待循环
  （`:9633`；循环体自 `:9558` 起），`r4TxProbePost` 同样（`:9856`）。
  **这两处都不能进内核数据通路**。
* 发送相被显式**限流**：`cap = min(postsWanted, 8*nent+1)`，理由是"一笔调用持事务锁几分钟"
  （`:9409-9413`）。异步数据通路不需要这个上限，但需要自己的在飞上限（见 §4.1）。

---

## 3. 锁与上下文

### 3.1 `fXactLock`（事务锁）

* 声明 `src/Cx3Mlx4.h:543`；惰性分配 + 加锁 `src/Cx3Mlx4.cpp:5031-5035`（`transactionEnter`）；
  解锁 `:5037-5040`；**TryLock 版** `:5063-5068`。
* **取它的位置只有一个**：`Cx3Mlx4UserClient::externalMethod` 把整个选择器调用包起来
  （`src/Cx3Mlx4UserClient.cpp:426-428`；try 类在 `:400-424`）。策略表 `kXactClass[]` 每个
  选择器一行（`src/Cx3Mlx4UserClient.cpp:165-245`）；37/38/39 都是 `kXactLocked`
  （`:218`、`:225-235`）。
* **它保护什么**：① 共享的命令邮箱——填 inbox / 提交 / 拷 outbox 必须是一笔原子
  （`src/Cx3Mlx4.h:308-330`）；② `fTables[]`/`fNumTables`/`fAuxIcm` 等设备面状态
  （`src/Cx3Mlx4.cpp:5048-5051` 的注释点名了这个缺陷）；③ 所有队列指针及其生产者/消费索引
  在**选择器侧**的一致性。
* **它不可能被中断路径取**：`irqAction` 只取 `fEqLock`，绝不取事务锁、绝不发命令
  （`src/Cx3Mlx4.h:324-330`、`src/Cx3Mlx4.cpp:5643-5651`）。所以锁序是单向的
  `fXactLock → fEqLock`，没有反向路径（`src/Cx3Mlx4.cpp:5218-5223` 明确写了这条 grep 结论）。
* 一个易被忽略的细节：**`transactionEnter()` 在锁不存在时会 `IOLockAlloc()`**
  （`src/Cx3Mlx4.cpp:5031-5035`）。从 network 回调里调它不只是"等锁"，
  还可能**在门内做一次分配**——这一条单独就足以否决"数据通路顺手取事务锁"。

### 3.2 `fEqLock`（EQ 锁）

* 声明 `src/Cx3Mlx4.h:736`；在当前树里由 **`m3irqup` 分配**（`src/Cx3Mlx4.cpp:5883-5885`）
  ⇒ **在打开中断会话之前它是 NULL**（那段窗口里对 EQ 的访问没有锁，见下面第 4 条）。
* 它保护：**EQ 的持久消费索引 `fEqConsIndex`、EQ 环扫描、re-arm 的原子性**
  （`src/Cx3Mlx4.h:731-735`）。
* 实际取锁者（`rg -n fEqLock` 的全部命中）：`irqAction`（`src/Cx3Mlx4.cpp:5679` / `:5706`，
  风暴分支 `:5738-5740`）、`m3irqup` 的补 arm（`:6028-6031`）、`m3irqtest`（`:6092-6096`）、
  `m3irqstatus`（`:6123` 起，锁在 `:6126-6148`）、`m3irqdown`（`:6174` 起，锁在 `:6183-6189`）。
* **发现（"两个消费者共用一个锁"这条不变量今天并不成立）**：
  `consumeEqLocked` 的**唯一**调用者是 `irqAction`（`:5689`）；
  **poll 选择器 `exportM3EqPoll` 不调它、也不取 `fEqLock`**
  （`:7955` 起，自己走 `eqNextEqe`，`:8091` 直接 `eqSetConsumerIndex`）。
  于是以下四处的说法**与代码不符**：`src/Cx3Mlx4.h:128-131`、
  `src/Cx3Mlx4.h:431-432`、`src/Cx3Mlx4.cpp:5565`、`src/Cx3Mlx4.cpp:6022-6024`
  （都在说"消费是 poll 选择器与中断动作共用的同一个函数 / 同一个锁"）。
  实际后果：`m3eqpoll` 与 `irqAction` 能同时移动 `fEqConsIndex` 而不互斥。
  10b 若再引入第三个消费者（中断里消费 COMP 事件），**这条必须先修**。
* `fEqConsIndex` 还有非锁访问者：`txprobe` 直接读改写它（`src/Cx3Mlx4.cpp:10247`、`:10273`）。

### 3.3 `irqAction` 今天做什么、跑在哪条 loop 上

`static void Cx3Mlx4::irqAction(OSObject *owner, IOInterruptEventSource *src, int count)`
——`src/Cx3Mlx4.cpp:5653`。今天只做四件事：

1. **先在设备侧确认中断**（只对 legacy 源：`fClrIntLegacy && clrIntReady()` → `clrIntWrite()`，
   `:5667-5678`）。不这么做会以"level 中断永不消失"把机器打死（`:5655-5666` 记录了那次事故）。
2. 取 `fEqLock`（`:5679`），`++fIrqCount`，累积回调的 `count`（`:5681-5686`）。
3. `consumeEqLocked(64, &after, &blockedByComp)`（`:5689`）——**消费 EQ、认 CMD token、
   计数并在末尾 re-arm**（`:5638`）。**它不碰任何 CQ，也不理解 COMP 事件**（见 §1.7）。
4. 放锁，做**风暴守卫**（`:5724-5746`）：连续 64 次零消费（`kIrqZeroStreakLimit`，
   `src/Cx3Mlx4.h:71`）就 `src->disable()`、`fIrqEnabled = false`、`fIrqStormTripped = true`；
   **不在这里释放 event source**（在 workloop 上释放正在执行的 callout = use-after-free，
   理由见 `:5717-5723`）。

**跑在哪条 workloop 上**：`IOInterruptEventSource` 加在 `fWorkLoop` 上
（`addEventSource`，`src/Cx3Mlx4.cpp:5942`）。而 `fWorkLoop` 由
`createWorkLoop()`（`:100-112`）或 `getWorkLoop()`（`:73-88`）创建，
**同一个对象也交给网络家族**——`IONetworkController.h:594-605`：控制器 start 时会
实例化一个 `IOCommandGate` 并 attach 到 `getWorkLoop()` 返回的那条 loop 上，
客户端相关的调用都过这道门。
⇒ **数据通路的分派与中断下段在同一条 loop 上串行化**。这既是 10b 的机会
（大量状态天然不需要额外锁），也是死锁的根源（§3.5）。

**并发隐患（顺序未被强制）**：`m3irqup` **无条件** `fWorkLoop = IOWorkLoop::workLoop()`
（`src/Cx3Mlx4.cpp:5925`），它不检查 `createWorkLoop()`/`getWorkLoop()` 是否已经建过一个。
0.44.6 补上 `createWorkLoop()` 之后"家族那一半不再自造 loop"，但**顺序**仍未被强制：
凡是在 `m3irqup` 之前问过 `getWorkLoop()` 的路径，之后 `m3irqup` 都会把 `fWorkLoop` 换掉，
而旧对象再没人释放（`stop()` 只释放最后一个，`:6462`；`irqReleaseObjects` 刻意不释放它，
理由见 `:5793-5796`）。
⇒ [未知] 当前链条里"家族先问还是 `m3irqup` 先跑"没有被测量过。10b 若要依赖
"家族的门与中断在同一条 loop 上"，**这条必须先核实**（最省事的读法：启动日志里
`createWorkLoop`/`getWorkLoop` 与 `m3irqup` 两行的先后）。

### 3.4 内核数据通路要碰、而今天只在 `fXactLock` 下被碰的状态

| 状态 | 今天谁在动 | 行 |
| --- | --- | --- |
| RX CQ 消费索引（`fR4RawRxCons` + 记录 word0） | 只有 `r4raw`（整笔调用持事务锁） | `src/Cx3Mlx4.cpp:9178-9179` |
| RX 生产者（`fR4RawRxProd` → RQ 记录 word0） | 只有 `r4raw` | `:9120-9122` |
| RX WQE 环内容 | 只有 `r4raw` 的补槽 | `:9119` |
| TX 生产者与 TXBB owner/stamp | `r4raw` 与 `txprobe`（靠事务锁互斥） | `:9432-9434`、`:9671`、`:9799-9833`、`:10322` |
| TX CQ 消费索引（`fR4RawTxCons`） | `r4raw` 与 `txprobe` | `:9571`、`:9848` |
| SQ 环内存本体、发送门铃 | `r4raw` / `txprobe` | `:9452`、`:9543` |
| 每槽簿记（`fR4RawRxInFlight[]`、`fR4RawRxRecycles[]`） | 只有 `r4raw` | `:9080-9089`、`:9126` |
| 队列/记录/环的**释放**与计数复位 | `rqTeardown`/`txTeardown`/`cqTeardown`（`stop()` 或复位路径，同一笔事务） | `:2017`、`:2182`、`:7110`；`stop()` `:6389` 起 |
| EQ 消费索引与 re-arm | `irqAction`（持 `fEqLock`）与 `m3eqpoll`/`txprobe`（**不持**） | `:5638`、`:8091`、`:10247` |

**关键点**：以上几乎每一项今天都**没有自己的锁**——它们的互斥是"**整笔选择器持事务锁**"
给的。数据通路不持事务锁，因此它要么**自己引入互斥**，要么**改变谁拥有这些状态**（§4.2）。

### 3.5 从网络回调里取 `fXactLock` 的确切死锁形状

前提（全部已核实）：

1. `enable()` / `disable()` / `configureInterface()` / `setMulticastMode` / `setPromiscuousMode` /
   `enablePacketFilter` 等头文件逐条写着 **"synchronized by the workloop's gate"**
   （`IONetworkController.h:1244`（enable）、`:1257`（disable）、`:610`、`:625`、`:647`、`:666`、
   `:676`、`:726`、`:742`、`:779`；`IOEthernetController.h:297`、`:325`、`:530`）；
2. 那道门是控制器自己的 **`IOCommandGate`，attach 在 `getWorkLoop()` 返回的 loop 上**
   （`IONetworkController.h:594-605`），而本驱动**把自己的 loop 交了出去**
   （`src/Cx3Mlx4.cpp:100-112`、`:73-88`；动机写在 `src/Cx3Mlx4.h:245-260`）；
3. 中断下段 `irqAction` **加在同一条 loop 上**（§3.3）——**门关着时它跑不了**；
4. `outputPacket` **不得阻塞**：`IONetworkController.h:496-498`
   （"The implementation in the driver must not block, since this may cause the network stack
   to be reentered from an unsafe point."）；`docs/R4-STAGE10-PLAN.md:83-91` 也把 10b 的
   发送写成"必须异步提交"。

死锁形状（两步，任一步单独就够）：

* **A：门 ↔ 事务锁**。回调（门内，即跑在这条 loop 上）取 `fXactLock`；与此同时某个选择器
  正持 `fXactLock`，而**那一侧可能正在等只有这条 loop 才能投递的东西**（例如 `m3irqtest`
  等 token；10b 之后任何"等完成"的实现也是）。门被回调焊在 `fXactLock` 上 ⇒ 中断下段
  永远跑不到 ⇒ 互等。表现是**挂起**，不是报错——本项目已经为同形状付过三次冻屏
  （`docs/HANG-AUDIT.md:1-20`）与一次 6 min 07.6 s 的持锁（`docs/LOCK-DESIGN.md` §1.4）。
* **B：门 ↔ 这条 loop 自己**。回调在门内做任何"需要这条 loop 推进"的动作
  （发固件命令并等命令完成事件、`IOSleep`、等 `IOCommandGate`/`IOLock`）都会让唯一的
  loop 停住。`fXactLock` 只是其中最常见的一个；注意它是 `IOLock`（可睡眠互斥），
  **从 loop 线程取它本身就会睡掉这条 loop**。
* 另外：`transactionEnter()` 可能在门内**分配** `IOLock`（§3.1），与取锁是同一类错误。

### 3.6 因此，安全的锁纪律是（10b 的硬约束）

1. **网络回调里绝不取 `fXactLock`、绝不发固件命令、绝不 `IOSleep`、绝不分配内存**。
   已写下的同款结论：`src/Cx3Mlx4.h:226-243`、`src/Cx3Mlx4.cpp:9925-9932`、
   `docs/R4-STAGE10-PLAN.md:216-249`（§11.1）。
2. **数据通路的所有权在 `enable()`/`disable()` 处一次性交接**：这两个回调本身就在门内，
   与选择器互斥的窗口**由门提供**；交接内容（谁拥有 RX CQ 消费索引、谁拥有 TX 生产者、
   在飞的 mbuf 怎么处理）必须在**门内、且不持事务锁**完成。
3. 数据通路内部状态（在飞 mbuf、每槽映射）**只需要与这条 loop 互斥**；
   若要与另一个上下文同步，用 **`IOCommandGate`**（本控制器已有，`IONetworkController.h:594-605`），
   而不是新造 `IOLock`。
4. **EQ 仍按 `fEqLock` 的规矩走**，但先补 §3.2 的漏洞（让 `m3eqpoll`/`txprobe` 也取它，
   或者把数据通路做成唯一消费者）。
5. **在飞状态的清理与释放留在选择器侧**：`rqTeardown`/`txTeardown`/`cqTeardown` 今天靠
   事务锁与"整笔选择器"保证原子（`:2017`、`:2182`、`:7110`），而且它们**会重置计数**
   （`:2057-2058`、`:2229-2230`、`:7141`、`:7144`）。⇒ `disable()` 必须先交出所有权，
   这些 teardown 才允许跑。

### 3.7 与正在查的冻屏的关系（记为候选，不是结论）

§3.3 那条"`m3irqup` 无条件换 loop"与"家族的门可能落在另一条 loop 上"的组合，
是 §3.5 形状的一个具体化身；`docs/R4-STAGE10-PLAN.md:245-257`（§11.2）已把
`createWorkLoop()` 缺失记为**候选解释**。**不要**把本条当结论：判决靠
`docs/HANG-AUDIT.md` §9 的三件仪器。

### 3.8 写本文时工作区里已经在实现的 10b（**移动靶，只记形状**）

写这份地图的同一时段，另一路正在**未提交地**写 10b 的 RX 半边（09:03 的工作区：
`src/Cx3Mlx4.cpp` +339 / `src/Cx3Mlx4.h` +63）。我读到的形状是
（行号属那个未提交状态：`src/Cx3Mlx4.cpp:10025` `netTimerAction`、`:10164` `netServiceRx`）：
**用 workloop 定时器轮询 RX CQ，而不是等 CQ 事件**；因此**不 arm CQ**，
也就暂时绕开了 §1.6 那条未证明的链与 §1.7 的 type-0 规则；每个 tick 用
**`transactionTryEnter()`** 拿事务锁，拿不到就跳过并计数
（`src/Cx3Mlx4.cpp:10117-10122`；动机写在 `src/Cx3Mlx4.h:470-479`）。

对本节的结论而言，这个选择**改变了**三点，逐一写清楚（这不是对那份代码的审查——
它仍在变；§3.6 的不变量照旧成立）：

1. **§3.5 A（门 ↔ 事务锁的互等）不成立**：`TryLock` 不等待，拿不到就跳过。
   这是它比"在回调里 `transactionEnter()`"安全的地方。
2. **代价是收入取决于能不能拿到锁**：选择器持锁期间（`r4raw` 一笔就是几十秒预算，
   命令路径最长以分钟计）数据通路**整段停摆**，表现为丢帧而不是报错。
   所以"跳过"必须被计数、被看见（那份实现已经在计数），否则就是静默丢包。
3. **新引入的一处耦合（值得单独盯）**：`netServiceRx` 现在**在持有事务锁期间**分配 mbuf。
   mbuf 分配可能因等待簇内存而睡眠——那会让"持锁 + 持门"的时长取决于内存压力，
   即 §3.5 B 的形状（在门内做会阻塞的事）。⇒ 若保持这个结构，分配必须**有界**：
   先走非等待路径（`mbuf_getpacket` 一类），失败就跳过本帧并计数，
   **绝不在持锁期间等内存**。
4. **第一次 tick 仍可能触发一次分配**：`transactionTryEnter()` 在锁不存在时会
   `IOLockAlloc()`（§3.1）。把锁的创建挪到 `netServiceStart()` 或 `netup`
   （门内、且那时还没有 tick），可以让数据路径上永远不出现分配。

---

## 4. 10b 需要的接口：只有签名与职责

> 以下**没有实现**，也**不在**本树里——只是把职责、依赖与必须处理的分支写清楚。

### 4.1 建议的函数

```
/* ---- 生命周期：从 enable()/disable() 调用，跑在门内，不取事务锁、不发命令 ---- */

/* 建立数据通路：在飞 map 清零；把 RX CQ 排空（这是 arm 能生效的前提，§1.6）；
 * arm RX CQ；把准备复用的 TXBB 依当前生产者相位重新盖章；记录 nent/指针；置 up 标志。
 * 失败必须返回错误码，让 enable() 别假装成功。 */
IOReturn netDataPathStart(IONetworkInterface *iface);

/* 拆除：停止接受新帧（先清 up 标志）；把在飞的 TX 处理策略写死（丢 还是 等一个有界时间）；
 * 解除 CQ arm；**不释放队列内存、不碰设备状态、不做 teardown**。 */
IOReturn netDataPathStop(IONetworkInterface *iface, const char *why);

/* ---- RX：CQE -> mbuf -> 协议栈 ---- */

/* 消费最多 maxCqes 个完成，逐笔：cqNextCqe -> 取 byte_cnt-4 与 wqe_index ->
 * 取/分配该槽的 mbuf -> 交付给栈 -> 补槽（rqWriteWqe + rqSetProducer）。
 * 返回交付的帧数，*outConsumed 给出消费的 CQE 数。 */
UInt32 netServiceRx(UInt32 maxCqes, UInt32 *outConsumed);

/* 在 CQ 已空之后 arm（cqSetArmDb + cqRingDoorbell，§1.6）。
 * 不满足"CQ 已空"就 arm，按 §54.5 的语义它不会产生事件——所以调用点要能证明它空了。*/
void   netRxRearm(UInt32 consIndex, UInt32 armSn);

/* ---- TX：不在 outputPacket 里等完成 ---- */

/* 占一个 TXBB + 一个 mbuf 槽位；把帧拷进该槽的 DMA 缓冲；写 WQE（nDataSegs=1）；
 * 敲门铃；立即返回。在飞已满时返回 kIOReturnOutputDropped 并计数，不阻塞。 */
UInt32 netServiceTx(mbuf_t pkt);

/* 在 TX 完成处理里回收：从当前 TX CQ 消费索引起读完成，按 wqe_index 找到槽，
 * 释放它的 mbuf、清"在飞"、盖章（相位 = 该槽自己的相位，§2.4(c)）。 */
void   netTxReclaim(UInt32 maxCqes);

/* ---- 每槽簿记（新增，见 §4.3） ---- */

bool   netTxRingAlloc(UInt32 nent);      /* 分配 mbuf 指针数组，在 netDataPathStart 里 */
void   netTxRingFree(void);              /* 释放，并释放仍在飞的 mbuf，在 netDataPathStop 里 */
```

职责边界（写给实现者）：

* `netServiceRx` **同时**是帧的交付者与补槽者，两步顺序不可换（§1.9）。
* `netServiceTx` **不做任何等待**；回收全部在 `netTxReclaim`（由完成驱动）。
* `netRxRearm` 与 `netTxReclaim` 都必须能退化到"poll 兜底"：因为 §1.6 那条机制
  （arm → COMP EQE）在本项目里**尚未被证明过**。
* 三条路径都**不写**内核日志（只计数）；上层用 `cx3ctl`/日志开关按需打印。

### 4.2 与 `r4raw`（37）/ `txprobe`（38）的共存：**不能共存，必须改**

事实（§1.2、§2.7）：

* `r4raw` 的判据 6 **本身就是**"RX CQ 只有一个消费者"的断言（`src/Cx3Mlx4.cpp:9308`）；
  数据通路一旦也消费 RX CQE，`r4raw` 会以 `rxRecycleViolations` 失败——
  而那是**正确的失败**（它确实在说"两个消费者对不上"）。
* `fR4RawTxProd`/`fR4RawTxCons`/`fR4RawRxCons`/`fR4RawRxProd` 是**同一批变量**
  （`src/Cx3Mlx4.h:660-665`），`r4raw`、`txprobe` 与数据通路会各自算 index/owner，
  互相把对方的槽当成自己的 ⇒ 静默地把 WQE 写到别处（本项目最贵的那一类失败）。
* 三者的互斥今天**只有事务锁**（37/38 是 `kXactLocked`，`src/Cx3Mlx4UserClient.cpp:218`、
  `:225`），而数据通路不持它。

可选方案（按代价排序，**推荐 (a)**）：

| 方案 | 做法 | 代价 / 风险 |
| --- | --- | --- |
| **(a) 单一所有者 + 硬闸门（推荐）** | 约定"**接口 up 期间数据通路独占两环**"：`r4raw`/`txprobe` 在 `fNetEnabled` 时直接拒绝（`kIOReturnBusy` 并说明原因）；`netup` 未跑时数据通路不存在；`enable()`/`disable()` 是唯一交接点（门内） | 需要一个状态位 + 两处选择器侧改动；判据脚本要能把"被拒绝"与 FAIL 区分开 |
| (b) 只靠流程约束"`r4raw` 跑在 `netup` 之前" | 不改选择器 | 没有代码强制；违反时静默写坏描述符。**不够**，除非同时加 (a) 的闸门 |
| (c) 数据通路用**自己的** CQ/QP | 彻底分离 | 这是 M5 的多队列工作（`docs/R4-STAGE10-PLAN.md:122-124` 明确划归 M5），不是 10b |

### 4.3 需要的成员变量 / 可复用的现成成员

**需要新增**（全部要加进 `init()` 的逐条初始化清单——本类没有构造函数，
遗漏就是"分配器给什么就是什么"，`src/Cx3Mlx4.cpp:113-145`）：

| 新增 | 为什么 |
| --- | --- |
| `bool fNetDataUp;` | 所有权标志：`enable` 置位、`disable` 清位、选择器读它做闸门（§4.2）。工作区里那份 10b 实现用的是既有 `fNetEnabled`（§3.8）；两种都能用，**要紧的是只有一个** |
| `mbuf_t *fNetTxMbuf;`（长度 `fR4TxSqNent`） | **每 TXBB 一个槽**：这笔属于谁、回收时释放谁。今天完全没有 |
| `bool fNetTxInFlight[(nent+7)/8];` 或一个计数 | 在飞上限 + "这个槽能不能再用" |
| `mbuf_t *fNetRxMbuf;`（长度 `fR4RqNent`） | 每接收槽的 mbuf；也用来判断"空槽需要新 mbuf" |
| `UInt32 fNetRxArmSn;` | CQ arm 的 `arm_sn`（2 位，`cqSetArmDb` 需要） |
| `UInt64 fNetRxFrames, fNetTxFrames, fNetRxDropped, fNetTxDropped;` | 计数；**累加不重置**，与既有 `fNetOutputDropped` 同风格 |

**可以直接复用**（都已由选择器建好，生命周期与"设备被复位/重建"绑在一起）：

| 现成成员 | 用途 | 声明行 |
| --- | --- | --- |
| `fR4RqRing` / `fR4RqBufs` / `fR4RqNent` / `fR4RqBufBytes` / `fR4RqLkey` / `fR4RqBufPhys` | RX 环与缓冲（补槽的全部输入） | `src/Cx3Mlx4.h:544-559` |
| `fR4QpRec` / `fR4QpRecPhys` / `fR4QpQpn` | RX 生产者记录 + QPN | `src/Cx3Mlx4.h:574-577` |
| `fR4CqRing` / `fR4CqNent` / `fR4CqCqn` / `fR4CqRec` / `fR4CqUarStart` | RX CQ（消费 + arm 的三件套） | `src/Cx3Mlx4.h:529-539` |
| `fR4TxSq` / `fR4TxSqNent` / `fR4TxSqPhys` / `fR4TxQpRec` / `fR4TxQpn` | SQ 与门铃值 | `src/Cx3Mlx4.h:627-635` |
| `fR4TxCqRing` / `fR4TxCqNent` / `fR4TxCqRec` | TX CQ | `src/Cx3Mlx4.h:621-625` |
| `fMrLkey` | WQE 的 lkey（两侧共用） | `src/Cx3Mlx4.h:525` |
| `fEqRing` / `fEqNent` / `fEqeFactor` / `fEqDoorbell` / `fEqConsIndex` / `fEqLock` | EQ（消费 COMP 事件 + re-arm） | `src/Cx3Mlx4.h:704-710`、`:736` |
| `fR4RawRxInFlight` / `fR4RawRxRecycles` | 现成的"该槽是否已投递"图（**但要与所有权一起决定**，§4.2） | `src/Cx3Mlx4.h:667-668` |
| `fNetIface` / `fNetUnit` / `fNetMac` / `fNetMacValid` | 交付帧要用的接口与地址 | `src/Cx3Mlx4.h:282-284` |
| `fNetOutputDropped` / `fPortLinkUp` / `fPortSpeedMbps` / `fNetEnabled` | 已有计数、链路读数与 up 标志 | `src/Cx3Mlx4.h:286`、`:287`、`:298-299` |
| `fWorkLoop` | 数据通路运行的 loop（门就在它上面） | `src/Cx3Mlx4.h:737` |

**不能直接当自己状态用的**（会被别人重置）：`fR4RawRxCons`（`cqTeardown` `src/Cx3Mlx4.cpp:7144`、
`rqTeardown` `:2058`）、`fR4RawRxProd`（`:2057`）、`fR4RawTxProd`/`fR4RawTxCons`（`:2229-2230`）、
`fR4CqConsIndex`（`:7141`）。若复用它们，必须像 `exportR4Raw` 那样**从设备侧重新种子**
（`:10462-10482`），而不是假设自己记的数还成立。

---

## 5. 不许做的事（10b 的"不要"清单）

### 5.1 内核上下文（门内 / 中断下段）里**不许改**的状态

| 不许 | 为什么 | 出处 |
| --- | --- | --- |
| 命令邮箱（`cmdBox`/`cmdBoxLocked`/`hcrRun`/`cmdEvent`/`fMboxVirt`） | 共享 4 KiB 邮箱；从回调里发命令 = 在门内等命令信号量，而命令完成又要靠这条 loop 投递 | `src/Cx3Mlx4.h:308-330`、`docs/LOCK-IMPL.md` §2 |
| `fTables[]` / `fNumTables` / `fAuxIcm` / `fIcmMapped` | 这些会在 reprofile 时被释放重建（`freeIcmTables`，`src/Cx3Mlx4.cpp:6823`）；数据通路改它 = 拆自己的地基 | `src/Cx3Mlx4.cpp:5048-5051` |
| 任何 teardown / 释放 | `rqTeardown` `:2017`、`txTeardown` `:2182`、`cqTeardown` `:7110`、`mrTeardown` `:7164`、`teardownEq` `:7221`：都要发命令、都按"设备是否仍拥有它"分叉、**都会重置生产者/消费索引** | 同上 |
| HCA reset / FLR | 会毁掉在飞状态，且 reset 路径本身要跑一整套 teardown | `src/Cx3Mlx4.cpp:481`、`src/flr.inc` |
| `fEqConsIndex`（不持 `fEqLock` 时） | 与 `irqAction` 争夺同一个索引（§3.2） | `src/Cx3Mlx4.h:731-735` |
| 风暴守卫的三个状态（`fIrqZeroStreak`/`fIrqStormTripped`/`fIrqEnabled`）的语义 | 它是"机器不被打死"的最后一道；顺手清它 = 拆掉那次事故换来的保险 | `src/Cx3Mlx4.cpp:5724-5746`、`src/Cx3Mlx4.h:71` |
| `fR4Raw*` 这批共享计数 | 见 §4.2（单一所有者原则） | `src/Cx3Mlx4.h:660-668` |
| 接口的生命周期：`attachInterface`/`registerService`/`terminate` | `clientClose()` 里那句 `terminate()` 曾经终止 provider 链，导致 `IONetworkController::handleIsOpen` 空指针 panic | `src/Cx3Mlx4UserClient.cpp:288-309` |

### 5.2 会阻塞的调用（数据通路里一个都不能出现）

| 调用 | 今天出现在哪 | 为什么不能进 10b |
| --- | --- | --- |
| `IOSleep(1)` 忙等 | `r4RawRxPoll` `src/Cx3Mlx4.cpp:9172`；`r4RawTransmit` `:9633`；`r4TxProbePost` `:9856`；`txprobe` 的 arm 段 `:10287`；命令路径 `src/Mlx4Hw.cpp:821`、`:895` | 门内/中断里睡眠 = 让唯一的 workloop 停摆（§3.5 B） |
| 由 `clock_get_uptime` 驱动的等待循环 | `r4RawNowMs()`（`src/Cx3Mlx4.cpp:8920`）的每处使用 | 同上，形状是"等到某个墙钟时刻" |
| 固件命令路径 | 全部选择器 | 会取 `fCmdLock`（`src/Mlx4Hw.cpp:667-671`）并 `IOSleep`；**命令完成要中断投递**，而中断靠这条 loop |
| `IOLockLock(fXactLock)` / `IOLockLock(fCmdLock)` / `IOLockAlloc()` | `src/Cx3Mlx4.cpp:5031-5035`、`src/Mlx4Hw.cpp:667` | 可睡眠互斥 + 可能在门内分配 |
| 可阻塞内存分配 | 各处（`IOMalloc`/`icmAlloc`） | 中断/门内分配是禁忌；mbuf 必须走包 API 并遵守 §5.3 第 6 条 |
| 每帧 `LOG(...)` | 现有代码大量使用 | 内核日志在高包速率下本身就是负载；数据通路只允许计数 |

### 5.3 会被数据通路破坏的**选择器侧**不变量

1. **`consumeEqLocked` 的 type-0 规则**（`src/Cx3Mlx4.cpp:5592-5604`）：COMP 事件留在原地不消费。
   有 CQ 在跑之后，这条规则会让 EQ 永久卡住（不消费 ⇒ 不 re-arm）。
   **必须**改成"把 COMP 事件交给它命名的那个 CQ 的消费者"（`MLX4_EQE_COMP_CQN_OFFSET`，
   `src/mlx4_regs.h:1443`），否则 10b 会以"中断从此再也不来"的形式当场失败。
2. **`r4raw` 的判据 6**（`src/Cx3Mlx4.cpp:9308`）——见 §4.2。
3. **`fEqConsIndex` 的三个非互斥访问者**（`irqAction` 持锁、`m3eqpoll` 不持、`txprobe` 不持）
   —— 见 §3.2；10b 会变成第四个。
4. **计数会在 teardown 里被清零**（`src/Cx3Mlx4.cpp:2057-2058`、`:2229-2230`、`:7141`、`:7144`）
   —— 数据通路不能假设"我记的数和设备里的一致"，必须按设备侧种子重建（`:10462-10482`）。
5. **`init()` 的逐条初始化约定**（`src/Cx3Mlx4.cpp:113-268`）：新增成员**必须**进那份清单；
   本树已记过两处正是这个形状的缺陷（`:118-129`）。
6. **mbuf 纪律**（`docs/R4-STAGE10-PLAN.md:37-64`，其中两条是本机两次真实 panic 的签名）：
   `M_EXT` / `m_flags` / `m_type` **只由分配器设置，驱动不写**；分配/释放一一对应；
   RX 侧**先拷进 mbuf**，不要让 mbuf 直接指向我们的 DMA 缓冲；
   第一次上机**只跑一帧**（与阶段 9"先一个 WQE"同一条纪律）。
   签名原文与两次事故：`docs/R4-STAGE10-PLAN.md:42`、`:48-49`
   （RTL8125 2026-10-11 13:46；AirportMtk 2026-10-12 四次）。

---

## 6. 本次没能确定的（未知，不要当已知用）

1. **CQ arm → COMP EQE 这条路到底能不能用**。阶段 9 的 arm 实验零 EQE，而那次测试本身可疑
   （arm 时 CQ 从未为空）——`docs/M2B-NOTES.md:4818-4826`。10b 的接收投递**完全依赖**这条链。
   ⇒ 必须先做一个"**排空后 arm**"的最小实验再来写投递路径。
2. **家族的门与中断是否真的在同一条 workloop 上**：`m3irqup` 无条件重建 `fWorkLoop`
   （`src/Cx3Mlx4.cpp:5925`），顺序未被任何代码强制，也未被测量过（§3.3）。
3. **`m3eqpoll` 不取 `fEqLock`** 是遗漏还是刻意（`src/Cx3Mlx4.cpp:7955` 起）。代码与
   四处注释不一致；**本文只陈述不一致，不判谁对**。
4. **`netup` 怎么被调用**：在本文定点的树上（5411d55），`tools/verify.sh` 里**没有** netup、
   `tools/cx3ctl.c` 的命令表也只到选择器 38（`tools/cx3ctl.c:1953-1955`）——所以 10a 那次
   是怎么发的**本文无法确定**。**但这条已经过时了**：随后落地的 `518c485` 把 netup 接进了链条
   （`tools/verify.sh:302` 取选择器号，`:1641` 起是判据）；`cx3ctl` 至今仍**没有** netup 子命令
   （`rg NetUp tools/cx3ctl.c` 为空）。⇒ 按当前树读即可，不要在 39 号上再绕外部调用。
5. **`fR4TxSqNent` 的实际实测值**：默认常量是 256（`src/cx3_abi.h:2617`），验证器措辞也是
   "the default 256 TXBBs of 64 bytes"（`tools/verify.sh:1329`），但**默认值与回读值是否一致
   本文没有从真机日志核实**。
6. **mbuf 侧的具体 API 面**：本树从未分配过 mbuf（`docs/R4-STAGE10-PLAN.md:52-58`）。
   该用 `mbuf_allocpacket` 还是 `mbuf_getpacket`、交付用哪个 `inputPacket` 入口，
   本文**没有核实**。⇒ 这一条由**另一路**的 `docs/10B-MBUF-CONTRACT.md`（写这份地图时它已有
   402 行，逐条钉到本机 SDK 头文件）回答；我**没有逐条核对**它的内容，
   引用它之前请自己核一遍（与 0.44.6 查 `enable()`/`createWorkLoop()` 是同一类工作）。
7. **`docs/M2B-NOTES.md` 的 §55 缺失**（代码引用它，见 §2.4(c)）——需要补写或改引用。

---

## 附录 A：常量速查（除注明外都在 `src/mlx4_regs.h`）

| 常量 | 值 | 行 |
| --- | --- | --- |
| `MLX4_TXBB_SIZE` | 64 | 737 |
| `SQ_WQE0_OFFSET` / `RQ_WQE0_OFFSET` | 0 / 64 | 800 / 801 |
| `MLX4_SQ_WQE_OFFSET(index)` | `0 + index*64` | 811-812 |
| `RX_WQE_SIZE` / `RX_RING_HEADER_SIZE` | 16 / 64 | 701 / 714 |
| `MLX4_NET_IP_ALIGN` | 2 | 706 |
| `RX_PROD_DB_WIDTH` | 0xffff | 724 |
| `MLX4_OPCODE_SEND` | 0x0a | 1148 |
| `MLX4_TX_OWNER_SHIFT` / `MLX4_TX_PROD_SHIFT` / `MLX4_TX_PROD_MASK` | 31 / 8 / 0xffff | 1150-1152 |
| `MLX4_WQE_CTRL_CQ_UPDATE` / `MLX4_WQE_CTRL_SOLICITED` | (3<<2) / (1<<1) | 1155-1156 |
| `TX_WQE_SEND_SIZE` | 0x20 | 1176 |
| `TX_WQE_FENCE_SIZE(ds)` / `TX_WQE_SEND_FENCE_SIZE` | (16+16ds)/16 / **2** | 1206 / 1210 |
| `MLX4_SEND_DOORBELL` / `MLX4_SEND_DB_QPN_SHIFT` | 0x14 / 8 | 1224 / 1225 |
| `MLX4_CQ_DOORBELL` / `MLX4_CQ_DB_REQ_NOT` | 0x20 / 0x02000000 | 666 / 673 |
| `CQE_LEGACY_SIZE` / `CQE_OWNER_OFFSET` / `MLX4_CQE_OWNER_MASK` | 0x20 / 0x1f / 0x80 | 658 / 659 / 660 |
| `CQE_BYTE_CNT_OFFSET` / `CQE_WQE_INDEX_OFFSET` | 0x14 / 0x18 | 1263 / 1264 |
| `MLX4_CQE_IS_SEND_MASK` / `MLX4_CQE_OPCODE_MASK` / `MLX4_CQE_OPCODE_ERROR` | 0x40 / 0x1f / 0x1e | 1274 / 1275 / 1276 |
| `MLX4_SQ_STAMP_VAL` / `MLX4_STAMP_SHIFT` / `MLX4_SQ_INIT_OWNER_BIT` | 0x7fffffff / 31 / 0xffffffff | 771 / 770 / 772 |
| `MLX4_NUM_RESERVED_UARS` | 128 | 840 |
| `MLX4_UAR_PAGE_SHIFT` / `MLX4_EQ_DOORBELL_OFFSET` | 12 / 0x800 | 1461 / 1462 |
| `MLX4_EVENT_TYPE_COMP` / `MLX4_EQE_COMP_CQN_OFFSET` | 0x00 / 0x04 | 1442 / 1443 |
| `CX3_R4RAW_FRAME_BYTES` / `CX3_R4RAW_RX_KEEP_BYTES` | 60 / 64 | 1308 / 1322 |
| `CX3_R4RAW_MAX_RX_SLOTS` | 1024 | `src/cx3_abi.h:2711` |
| `CX3_R4TX_DEFAULT_SQ_TXBB` / `CX3_R4CQ_DEFAULT_NENT` / `CX3_R4RQ_DEFAULT_NENT` | 256 / 512 / 256 | `src/cx3_abi.h:2617` / `:1842` / `:1914` |

## 附录 B：今天的调用图与 10b 的目标图

```
今天：cx3ctl r4raw / tools/verify.sh
        -> 选择器 37（kXactLocked）
        -> externalMethod 取 fXactLock          src/Cx3Mlx4UserClient.cpp:426
        -> exportR4Raw -> r4RawRxPoll / r4RawTransmit（同步等完成，IOSleep）

10b 目标：
    网络栈 -> IOCommandGate（attach 在 fWorkLoop 上）
        -> outputPacket / enable / disable -> netServiceTx / netDataPathStart|Stop
    硬件 -> MSI-X -> 同一条 fWorkLoop -> irqAction
        -> 消费 EQ（含 COMP 事件）-> netServiceRx（CQE -> mbuf -> 栈） / netTxReclaim
```
