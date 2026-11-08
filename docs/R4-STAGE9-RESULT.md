# R4 阶段 9（`r4Raw`）实现记录：全项目第一次真的让帧流动

本文是**实现记录**，实现的是 `docs/R4-STAGE9-PLAN.md`（已定稿的方案，本批次**没有改它**）。

**本阶段一行都没在硬件上跑过**：kext 未安装、机器未重启、`tools/verify.sh run|full|safe`
未执行（本批次的硬约束——这一阶段第一次有"对外产生副作用"的操作，那一次留给用户重启后跑）。
所以第 7 节把每条结论按"实测 / 推断 / 未测"分开写，第 10 节是重启后**应当**看到什么
（预测，不是记录）。

一句话结论：**发送门铃、WQE 编码、CQE 解析与回收补槽都写好了**——一个选择器 `r4Raw`，
内部 TX/RX 两段各自给标量，先**一个 WQE 一帧**、它的完成回来之后才继续到**绕环**；
`.so` 之外本阶段还**推翻了方案自己的两处编码**（opcode 的位移、CQE 的 byte_cnt/wqe_index
偏移），两处都变成了**构建期断言**（`docs/DEFECTS.md` E10/E11）。

---

## 1. 改动的文件（绝对路径）

| 文件 | 改了什么 |
| --- | --- |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/mlx4_regs.h` | 新一节 "R4 stage 9"：发送 WQE 的 8 个字段偏移 + `MLX4_OPCODE_SEND`/`_NOP`/`MLX4_EN_BIT_DESC_OWN`/`MLX4_TX_OWNER_SHIFT`/`MLX4_TX_PROD_SHIFT`/`MLX4_TX_PROD_MASK`/`MLX4_WQE_CTRL_{OWN,CQ_UPDATE,SOLICITED,FORCE_LOOPBACK}`；`MLX4_SEND_DOORBELL = 0x14` 与 `MLX4_SEND_DB_QPN_SHIFT = 8`（**并写明它与 CQ 的 `0x20` 相差 0xC**）；`struct mlx4_cqe` 的 13 个偏移 + `MLX4_CQE_{QPN_MASK,IS_SEND_MASK,OPCODE_MASK,OPCODE_ERROR,BAD_FCS}` + `MLX4_CQE_SLOT_SIZE`；**错误 CQE 的两个 syndrome 字节偏移**（`0x1a`/`0x1b`，因为 `my_qpn`/`wqe_index` 在两个结构里同址）；本阶段要发的帧（广播目的、ethertype `0x88B5`、60 字节、payload 标签 `'C','X','3','R'`、`CX3_R4RAW_RX_KEEP_BYTES`） |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Mlx4Hw.h` / `.cpp` | 新函数 `sqWriteSendWqe`（一个 WQE：ctrl + 一个 data 段，**owner 位最后写、前后各一次屏障**，顺序照 `mlx4_en_tx.c:897-919`；可选 verbose 日志）、`sqStampTxbb`（`mlx4_en_stamp_wqe`，回收时把槽位盖回"不是描述符"的值 + 下一圈的相位）、`sqRingSendDoorbell`（`BE32(qpn << 8)` 写到 **UAR 页 + 0x14**）、`rqBufferPhys`（**收发两侧共用**的缓冲地址算术）；`rqCreate` 改成调用 `rqBufferPhys`（把"首次填充"与"阶段 9 的补填"变成同一份算术，`docs/DEFECTS.md` D12 的形态） |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/cx3_abi.h` | 选择器 `kCx3SelR4Raw = 37`、`kCx3SelCount = 38`；`Cx3R4RawResult`（含两段各自的判据、首个 WQE/首个 TX CQE/首个 RX CQE 的**32 字节原样**、首个帧的字节、回收补槽的计数、QUERY_MPT 读回的区域）；2 个入参（发帖数、RX 预算）；**16 个标量**（`CX3_SCALAR_OUT_MAX` 正好用满）；`CX3_R4RAW_NOT_MEASURED`；`CX3_R4RAW_TX_*` 三种"TX 段没跑"的原因；三条静态断言 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Cx3Mlx4.h` / `.cpp` | `exportR4Raw` + 它的四段（`r4RawFrameEnsure`/`r4RawTransmit`/`r4RawRxPoll`/`r4RawRxRecycle`）+ 位图与逐槽计数；`cx3GetBe16`（CQE 的三个 16 位字段用它）；成员初始化 12 个新成员；`txTeardown` 释放帧缓冲、`rqTeardown`/`cqTeardown` 清掉本阶段对该环/该 CQ 的记账 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Cx3Mlx4UserClient.h` / `.cpp` | 分发表第 38 行（selector 37）、锁类别表的第 38 项、`sR4Raw`/`doR4Raw` |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/tools/test-cqe-offsets.c` | **新文件**：CQE 与发送 WQE 的字节布局、opcode 的**位位置**、门铃偏移差、帧的布局，以及 ABI 里那两个字节数组宽度的对齐（111 项） |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/tools/cx3ctl.c` | `cmdR4Raw`（打印 WQE/CQE 的十六进制、门铃值与偏移、MR 区域、回收计数）+ `sizes` 表新行 + `r4raw` 子命令 + usage |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/tools/verify.sh` | 17 个新索引名解析 + `r4tx` 之后的 `r4raw` 段（六条判据，每条 FAIL 的话都不一样；RX 一帧都没有时**点名阶段 7 的 `mcast`/`base_qpn`**） |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/build.sh` | `tools/test-cqe-offsets.c` 进构建门禁 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/kext/Info.plist` | 版本 0.43.6 → **0.43.7** |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/docs/COVERAGE.md` | `r4raw` 行（链内，证据栏 **未运行**）、计数 23→24、版本号同步 0.43.7、一段说明这一行是唯一会改动网络状态的一步 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/docs/DEFECTS.md` | 新行 **E10**（opcode 的位移）、**E11**（CQE 偏移差 2）；E8 行补记它没覆盖到的第二个站点（未使用的 CQ 门铃 helper 里那个字面量页号）；总数 59→61、E 家族 9→11 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/docs/R4-STAGE9-RESULT.md` | 本文件 |

**没有动**：`docs/R4-STAGE9-PLAN.md`、`docs/R4-STAGE7-RESULT.md`、`docs/R4-STAGE6-RESULT.md`、
`docs/M2B-NOTES.md`、`docs/EXTERNAL-REFS.md`、`docs/LOCK-IMPL.md`、`docs/EQ-ORDERING.md`、
`tools/mlx4-port/`、`driver/`、`outputs/` 下别人的目录、ESP、`/Library/Extensions/`。

---

## 2. 六条判据，以及每一种失败读起来为什么不同

`r4Raw` 一个选择器做两件互不依赖的事，所以它的 16 个标量分成两段。**TX 段的失败不妨碍
RX 段**（方案决策④）：`kR4RawOutTxRefused` 说明发送那段为什么没跑，接收那段的标量照测；
只有**接收侧的前置**缺失才会让整个调用停下（那时没有可观测的对象，报 `kIOReturnNotReady`，
链上记为 BLOCKED）。

| # | 标量 | 通过条件 | 它真正回答的问题 |
| --- | --- | --- | --- |
| 0 | `kR4RawOutStatus` | `0` | 选择器本身跑通了（不是"设备做了"） |
| 1 | `kR4RawOutTxWqeOk` | `1` | **WQE 写对了**：32 字节从**内存里**读回来，逐字段解码——opcode 在**低字节**、owner 位等于本圈相位、producer 在 bit 8..23、`byte_count` 60、lkey、帧总线地址；另外该 TXBB 里 32 字节之外必须全 0 |
| 2 | `kR4RawOutTxCompletions` / `TxCqeOk` / `TxErrors` | `≥1` / `1` / `0` | **完成事件回来了**，且首条不是错误完成、`wqe_index` 就是我刚发的那个 TXBB。"一条都没有"和"回来的是错误完成"是两句话，分开报 |
| 3 | `kR4RawOutTxWrapOk` / `TxOwnerLapsOk` | `1` / = 覆盖的圈数 | **环绕了两次以上**，每一圈的 owner 位读回来都等于该圈相位，且**两种 owner 值都出现过**（只写过 0 的环满足不了这条） |
| 4 | `kR4RawOutRxFrames` / `RxQpnOk` / `RxQpn` | `≥1` / `1` / 可见 | **收到了帧**，且每条完成的 `vlan_my_qpn` 低 24 位是**我们的 RX QPN** |
| 5 | `kR4RawOutRxMinByteCnt` / `RxContentOk` | `≥60` / `1` | **内容对**：至少一帧不小于最小帧，且它的字节被读出来了（FCS_KEEP ⇒ RX 侧比 TX 多 4 字节，见 §4.4） |
| 6 | `kR4RawOutRxRecycleOk` / `RxLaps` | `1` / `≥2` | **回收补槽正确**：每条完成指到的槽就是环位置预测的那个槽、且它在飞行中；然后环转过两整圈（= 每个槽至少回收两次） |

**"没测"是独立状态**（`CX3_R4RAW_NOT_MEASURED`）：`txWqeOk`/`txCqeOk`/`txWrapOk` 在 TX 段
没跑时、`rxQpnOk`/`rxRecycleOk`/`rxMinByteCnt` 在"一条完成都没有"时都保持这个哨兵。
理由就是 `docs/DEFECTS.md` D10：**"每条完成都指向我们的 QPN"在零条完成时是空真**，
0 会把它读成通过。

### 2.1 判据 4 的措辞是写死的

RX **一帧都没有**时，内核日志、`cx3ctl r4raw` 与 `verify.sh` 的 FAIL 文字都说同一句话：
**第一怀疑是阶段 7 的 `mcast`/`base_qpn`**——A0 下广播/多播的落点由固件决定，而那就是
`SET_PORT qpn_calc` 的 `mcast` 字段（`docs/EXTERNAL-REFS.md` §13），**不是**"阶段 8 的过滤没配"：
A0 下没有任何过滤命令参与这件事。这一条写在方案 §5 的括注里，本批次把它落到了三处文字上。

---

## 3. 编码：照抄的那些值与来源（行号已逐条核对）

来源全部是 vendored 参考（`vendor/freebsd-src/`）。

| 项 | 值 | 依据（核对后） |
| --- | --- | --- |
| opcode | `MLX4_OPCODE_SEND = 0x0a`（**低 8 位**）、`MLX4_OPCODE_NOP = 0x00` | `device.h:369`、`:365` |
| owner 位 | bit 31；`MLX4_EN_BIT_DESC_OWN = 0x80000000`；`MLX4_WQE_CTRL_OWN = 1 << 31` | `en.h:247`、`qp.h:286`、`mlx4_en_tx.c:682-683` |
| producer 位 | `opcode \|= (bf_prod & 0xffff) << 8`（单位 TXBB） | `mlx4_en_tx.c:897` |
| ctrl 段 | 16 字节：`owner_opcode`(0x00)、`vlan_tag`(0x04)/`ins_vlan`(0x06)/`fence_size`(0x07)、`srcrb_flags`(0x08)、`imm`(0x0c) | `qp.h:302-330` |
| data 段 | 16 字节：`byte_count`(0x10)、`lkey`(0x14)、`addr`(0x18) | `qp.h:440-444` |
| `srcrb_flags` | `MLX4_WQE_CTRL_CQ_UPDATE (3<<2) \| SOLICITED (1<<1)` = `0x0e` | `mlx4_en_tx.c:635-636`、`qp.h:292-293` |
| `fence_size` | = **数据段个数**（本 WQE 是 1） | `mlx4_en_tx.c:846`（`ds_cnt & 0x3f`） |
| `byte_count` | 60 = `ETHER_MIN_LEN(64) - ETHER_CRC_LEN(4)`，**不含 FCS** | `mlx4_en_tx.c:764-765` |
| lkey | 阶段 1 的 physical MR lkey（运行时取，`fMrLkey`） | 本任务硬指定；与 RX WQE 同一个 key（`mlx4_en_rx.c:59`） |
| 写序与屏障 | 描述符先落内存 → `wmb()` → **owner_opcode 最后写** → `wmb()` → 敲门铃 | `mlx4_en_tx.c:897-919` |
| 发送门铃 | 值 `BE32(qpn << 8)`，地址 **UAR + 0x14** | `mlx4_en_tx.c:231`、`:918-919`、`doorbell.h:41` |
| 回收后的 stamp | `STAMP_VAL \| owner << 31`，owner = 该槽**下一次**发帖所在圈的相位 | `mlx4_en_stamp_wqe`，`mlx4_en_tx.c:271-287`、`:401-403` |
| CQE 偏移 | `vlan_my_qpn`(0x00)、`immed_rss_invalid`(0x04)、`g_mlpath_rqpn`(0x08)、`sl_vid`(0x0c)、`rlid`(0x0e)、`status`(0x10)、`ipv6_ext_mask`(0x12)、`badfcs_enc`(0x13)、`byte_cnt`(**0x14**)、`wqe_index`(**0x18**)、`checksum`(0x1a)、`owner_sr_opcode`(0x1f) | `cq.h:41-60`（用 `offsetof` 在构建期核对，见 §4.2） |
| CQE 判读 | owner `0x80`、is_send `0x40`、opcode 掩码 `0x1f`、错误 opcode `0x1e`；QPN 掩码 `0xffffff` | `cq.h:99-101`、`device.h:387`、`cq.h:95` |
| 错误 CQE | 另一种 32 字节结构，但 `my_qpn` 在 0x00、`wqe_index` 在 0x18；syndrome `0x1b`、vendor syndrome `0x1a` | `cq.h:63-72` |
| RX 回收 | 重写那一槽的 WQE（`byte_count` = bufBytes − `MLX4_NET_IP_ALIGN`），然后 `*db = cpu_to_be32(prod & 0xffff)` | `mlx4_en_rx.c:49-70`、`:250-254` |
| RX 的槽与完成的对应 | 参考**不读** `wqe_index`，它假设 1:1 并用消费者索引：`index = cons_index & size_mask` | `mlx4_en_rx.c:762-765` |
| CQE 的 CI 记录 | 消费后写 `*set_ci_db = cpu_to_be32(cons_index & 0xffffff)` | `cq.h:163-166` |
| MR 的覆盖面 | 参考的以太网 MR 是 `mlx4_mr_alloc(dev, pd, 0, ~0ull, ...)`，即**整个地址空间** | `mlx4_en_main.c:213` |

---

## 4. 四处值得单独说的地方（两处推翻方案、一处偏离参考、一处只记录）

### 4.1 方案 §2.1 的 opcode 移位是错的——而且它会**静默失败**（`docs/DEFECTS.md` E10）

方案写的是 `owner_opcode = MLX4_OPCODE_SEND << 24 | owner << 31`。**正确的是**
`MLX4_OPCODE_SEND | owner << 31`——opcode 在**低 8 位**：

* `mlx4_en_tx.c:682-683` + `:761-762`：`opcode = cpu_to_be32(MLX4_OPCODE_SEND) | owner_bit`；
* DPDK 的 `mlx4` 独立同构：`MLX4_OPCODE_SEND | (0u << MLX4_SQ_OWNER_BIT)`。

**后果**：位移 24 会让 `0x0a` 落在**大端字的 byte 0**（owner 那一个字节），byte 3 变成 0，
而 `MLX4_OPCODE_NOP = 0x00`（`device.h:365`）——WQE 变成一个**空操作**：门铃敲了、完成事件
可能照样回来、**一个字节都不会出去，且没有任何报错**。这正是"失败被表示成成功"（B 家族），
会长成阶段 10 的"ping 不通"。

**本批次的做法**：位置变成**构建期断言**——`tools/test-cqe-offsets.c` 把 dword 编出来，
断言 byte 3 是 `0x0a`、byte 0 的 bit 7 是 owner 位，再把**位移那一版**显式编一遍并断言它的
byte 3 是 `NOP`；改动一回来就构建失败。选择器另外把 WQE 的 32 字节打进日志并从内存回读解码，
所以用别的方式犯同一个错也会在第一次运行的日志里现形。

### 4.2 方案 §3.2 的 CQE 偏移差 2（`docs/DEFECTS.md` E11）

方案把 `status` 放在 0x0e、`byte_cnt` 放在 0x12、`wqe_index` 放在 0x16；**实际是 0x10、0x14、
0x18**：从 0x0e 起那个匿名 union 里 `rlid` 占 2 字节**在 `byte_cnt` 之前**。按 0x12 读
`byte_cnt` 会读到"状态字低半 + 帧长"的混合值——一个**看起来像测量**的数（D 家族）。

**本批次的做法**：不做第二份手抄。`tools/test-cqe-offsets.c` 里放一份与 `cq.h:41-60` **逐字段
同序**的镜像结构，用 `offsetof` 算出每个偏移，再拿 `mlx4_regs.h` 的常量去比；另外单独断言
方案写错的那四个字面量（0x10/0x14/0x18/0x1f），所以改头文件必须在测试里**显式**改一次。

> 状态说明：写入本文时，方案 `docs/R4-STAGE9-PLAN.md` 的 §2.1 表格已由 `6a5c138` 更正
> （opcode 一行加了更正框），但 **§3.2 那张 CQE 偏移表仍是 0x0e/0x12/0x16**——方案不是本批次
> 可以改的文件，所以以本节与 `tools/test-cqe-offsets.c` 为准，留此一句免得下一个人按 §3.2 抄。

### 4.3 `fence_size`：方案写 0，本批次照参考填**数据段个数**（=1）

方案的 ctrl 段那一行把 `vlan_tag`/`ins_vlan`/`fence_size` 一起写成 0。参考在同一个 WQE 上把
`fence_size` 赋成 `ds_cnt & 0x3f`（`mlx4_en_tx.c:846`），而 `ds_cnt` 就是数据段个数——本 WQE
只有一段，所以是 1。**照抄参考**：这是"默认照抄、偏离要写理由"规矩下的选择，`vlan_tag` 与
`ins_vlan` 仍是 0（不插 VLAN）。**证伪条件**：若第一次运行完成事件正常但外部抓不到帧，
`fence_size` 与下面 4.5 的两条并列成为第一批要改的字段。

### 4.4 两侧的 `byte_count` **故意不对称**

TX 的 `byte_count` = 60（**不含** FCS，硬件自己补）；RX 的 `byte_cnt` **含** 4 字节 FCS
（本卡 `FCS_KEEP = 1`，阶段 4 实测）——所以我们自己那帧若回环回来，RX 侧读到的是 **64**。
判据 5 因此写的是"**至少**最小帧长"，不是"等于我们发的长度"。"统一"这两侧才是错的。

### 4.5 顺带记录、本批次**不**改的两件事（阶段 6 已写下的证伪条件）

若第一次运行"门铃敲了、没有完成"，方案之外的两条现成线索仍是：**SQ 前面那个 64 字节
dummy 头**（`docs/R4-STAGE6-RESULT.md` §4.1）与 **`db_rec_addr` 的 `<< 2` 形态**（同文 §4.2）。
本批次的发送门铃用的是**QP context 自己的 `usr_page`**（运行时索引 `fR4CqUarStart`），
**不是**未使用的 `cqRingDoorbell` helper 里那个写死到 `MLX4_NUM_RESERVED_UARS` 页的字面量——
本卡两者数值相同，所以那是潜在而非现实的错，记在 `docs/DEFECTS.md` E8 的补注里（F8 的教训：
一个修复的记录必须点名它覆盖到哪些站点）。

---

## 5. 先一个 WQE、一帧，确认完成事件回来，再谈绕环

方案 §1 的"先小后大"落在选择器的两段上：

1. **单帧段**：`prod = fR4RawTxProd`（本 boot 已发过 0 个 WQE）⇒ TXBB 0、owner 位 0
   （`mlx4_en_tx.c:682`：`prod & ring->size`，第一圈是 0）；写 WQE → 回读 32 字节逐字段验 →
   敲门铃 → **等它的完成**（默认 100 ms，轮询 TX CQ）→ 回收该 TXBB（stamp 回下一圈相位）。
2. **绕环段**：**只有**第一帧的完成到了才继续（`posts` 的默认值 = `2 × nent + 1`），
   而且仍然是**一次一个**：写完就等它的完成，消费掉再做下一个。所以任何时刻在飞的
   描述符最多一个。

三条边界各自留下一个数：入参 `kR4RawInTxPosts`（`1` = 只做单帧段，谨慎的第一次运行）、
每帧等待 `CX3_R4RAW_DEFAULT_TX_POST_WAIT_MS`、整段预算 `CX3_R4RAW_DEFAULT_TX_WRAP_BUDGET_MS`
（超了停下并报已经发到第几个）。

**为什么抄"一次一个"**：参考的 `mlx4_en_xmit` 每次调用也只发一个 WQE 并在最后敲一次门铃
（`mlx4_en_tx.c:890-919`）。"一次 post 一批、敲一次铃"是另一个实验，而且会让多个描述符在
谁都没读过之前一起在飞——那是这一阶段最不能承受的失败方式。

---

## 6. 回收补槽与"绕两圈"是怎么落地成数的

每收到一条**非错误**完成：先**读走**帧的字节（第一个帧最多 64 字节拷出来打日志），再
重写那一槽的 WQE，最后把 producer 计数发布出去（`prod` 是全量计数，不是环索引——
`mlx4_en_rx.c:250-254` 的 `prod & 0xffff`），并把 CI 记录写给硬件。

判据 6 拆成三个**答案不同**的问题，任何一个"否"都**停止补槽**（但不再停止轮询——环里
已经发出去的缓冲还能收）：

| 问题 | 否的读法 |
| --- | --- |
| CQE 的 `wqe_index` 是环位置预测的那个槽吗 | 补错槽，**或者完成丢了**（CQ 只有 256 项，两次轮询之间来的帧比它多就会绕掉，1:1 的对应关系就断了） |
| 那个槽当时在飞行中吗 | **双占用**：一个缓冲在它的上一帧还没被读走时就交回去了 |
| 它在环的范围内吗（`< nent`） | CQE 的槽字段和这个环不是同一个队列 |

`rxLaps` = 本 boot 补槽总数 / `nent`（整数圈）。因为每条完成补一次、且顺序正确时每圈
恰好把每个槽补一遍，`rxLaps ≥ 2` 就等价于方案里那句"绕两圈后每槽回收 ≥2 次"。
**注意它依赖外部流量**：两整圈 = 2 × 256 = 512 个收到的帧，一段安静的网段在默认预算内
给不出来——那时 `verify.sh` 记 **SKIP**（不是 FAIL），并写明加预算的方法
（`cx3ctl r4raw 0 20000`）与"帧来了但环没转"时要查的第一件事（阶段 7 的 `mcast`/`base_qpn`）。

---

## 7. 实测 / 推断 / 未测

### 实测（本轮真的跑出来的）

* **构建门禁全绿**：`boot-path` PASS（`start()` 可达函数集里无写、无 DMA、无 reset）·
  **146 + 8186 + 115 + 137 + 45** 项检查 0 失败（其中 **115** 是新增的
  `tools/test-cqe-offsets.c`；8186 是 `check-hardcoded.py --strict`，报"no device-sourced
  literal outside the headers that define them"）· 未定义符号 **352/352 全部解析**（与基线相同）·
  四个 `-Werror` 档零告警（构建日志里 `warning:` 计数为 **0**）。
* **kext sha256 = `e30f1d3c12d1b75423bcd8fa80bb02243cb0cc4c2817fdcc5553adf2d14d4280`**，
  382176 字节（`BUILD_TAG=0.43.7`；0.43.6 是 351448 / `4e3ba7f6…`）。
  **可复现**：同一份树连跑两次（`sudo -A env BUILD_TAG=0.43.7 OUT=/tmp/cx3-437f|437g bash
  build.sh`）得到同一 sha。（E12 修好之前那一版是 `d4dd7977…`，同一份树也连跑两次同 sha；
  改的是缓冲尺寸，所以两个值不同是对的。）
* `cx3ctl sizes` 自证新行：**`37 2 16 568 r4raw`**（2 入参、16 标量出、结构 568 字节），
  这一行是**编译器算的**。
* `tools/test-cqe-offsets.c` 的 115 项里包含：`struct mlx4_cqe` = 32 字节、13 个偏移全部由
  `offsetof` 与常量对上、错误 CQE 的同址断言、`CQE_SLOT_OWNER_BYTE(factor)` 与槽位的
  关系、opcode 的**字节位置**（含"位移那一版的 byte 3 = NOP"这条反证）、门铃 0x14 与
  CQ 的 0x20 相差 0xC、帧的布局、以及 ABI 里两个字节数组宽度与 `mlx4_regs.h` 的一致性。
* `bash -n tools/verify.sh` 通过（本机 bash 3.2）；`verify.sh` 用到的 **127 个** ABI 索引名
  逐个用同一段正则验过，**0 缺失**（含新增的 17 个）。
* 编译期静态断言：`kR4RawInCount/kR4RawOutCount` 未超上限、`CX3_R4RAW_MAX_RX_SLOTS % 8 == 0`，
  全部分发表/锁类别表长度断言（`== kCx3SelCount = 38`）通过。
* `cx3ctl r4raw`（无设备）只走到 `IOServiceOpen failed`，不打印任何伪造读数。
* **审稿抓到的一处自己的缺陷（`docs/DEFECTS.md` E12，已修）**：接收轮询里那个十六进制缓冲
  第一版按 CQE 的 32 字节声明（96 字节），却用它格式化**最多 64 字节**的收到帧（需要 193），
  最多会写穿栈缓冲 95 字节——而且是**内核栈**上，第一次收到 ≥32 字节的帧就会发生。
  修法是"一个尺寸一个家"：`CX3_R4RAW_HEX_BUF_BYTES` 成为本阶段所有 hex 缓冲的唯一尺寸，
  并由 `tools/test-cqe-offsets.c` 对着四种数据各断言一次（本轮 115 项里有 4 项是它）。
* **顺带修掉两处与本次改动无因果、但被这次盘点暴露的陈旧数字**（都是"数字有第二个家"的形态）：
  `docs/DEFECTS.md` 家族表里 C 家族的计数写 13 而实际行数是 **12**（本次新增三行 E 之后，
  总数 62 = 6+6+12+13+12+13），`README.md` 的文档索引写"45 rows"而实际是 62。
  两处都只改数字、不改结论；没有为它们新开 DEFECTS 行，因为它们是**汇总表里的漂移**，
  而这一批的合计已经逐家族数过一遍（上表即为核对结果）。

### 推断（有源码依据，未上机）

* §3 的全部编码：逐条照抄参考，行号已核；§4.1/§4.2 两处改动有**两份实现**或**编译器**作证。
* "完成事件会回来"与"门铃 0x14 是对的那个寄存器"：均未上机；但它们同时是**判据本身的**
  证伪点（`txCompletions = 0` 这一支的文字就是为它们写的）。
* "一帧一帧发不会打满环"：`prod` 每次只前进 1 个 TXBB、每次等完成，所以最多 1 个在飞。
* "60 字节广播无害"：`0x88B5` 是 IEEE 本地实验 ethertype，正常网段上没有设备解释它；
  载荷是我们自己的图案，且**不发 ARP**（阶段 10）。
* "`rxLaps ≥ 2` 需要外部流量"：本阶段不给自己造回环（没置 `FORCE_LOOPBACK`，
  `qp.h:299` 那个位**故意不设**），所以 RX 侧的帧必须来自网络自发流量——这直接决定判据
  6 的第二半在安静网段会记 SKIP。
* MR 的覆盖范围：`QUERY_MPT` 读回来的 `start/length` 与帧缓冲地址的**内外关系**是**报告**
  不是判据（本卡链上 `r4mr 0 0` 注册的是 `[0,0)`，而参考注册的是整个地址空间
  `mlx4_en_main.c:213`）。若设备真的校验它，症状是**TX 完成带 ERROR**——那是可读的答案，
  不是沉默。

### 未测（明确没做，或做不到）

* **`r4raw` 从未运行**：kext 未安装、机器未重启（本轮硬约束）。**一帧都没有发出过**，
  WQE 也从未被硬件读过。`docs/COVERAGE.md` 的 `r4raw` 行证据栏写的就是"未运行"。
* `tools/verify.sh run|full|safe` 未跑（会驱动硬件并真的发帧）。
* **`cx3ctl r4raw` 的排版只在编译层验过**（无设备时只走到 `IOServiceOpen failed`），
  没对着真实的 `Cx3R4RawResult` 打印过一遍；第 10 节的分岔判读是设计意图，不是实测过的路径。
* **CQE 的逐字节布局仍只有一个来源**（FreeBSD 的 `struct mlx4_cqe`）：DPDK 那份这次没有取到
  （`docs/EXTERNAL-REFS.md` §12.3 记的缺口）。本批次把它从"没互证"变成"**有构建期断言**"，
  但那不等于互证——第一次运行会把整条 CQE 打出来，那才是真正的核对。
* **幂等性**：本阶段**可以**重复调用（它只发帧、只读状态），但第二次调用会继续同一个
  TX 环的 producer 与同一个 RX CQ 的消费者索引，所以两次调用之后的数字是**累计**的。
  这条路径没在硬件上走过。
* `out[kR4RawOutRxLaps]` 在"拒绝路径"上保持 `CX3_R4RAW_NOT_MEASURED`，而
  `verify.sh` 对它的比较只在 `rxRecycleOk = 1` 的分支里做——那条分支组合没实测过。
* **`fence_size = 1`、`prod` 的 bit 8..23、`0x14` 这个门铃偏移在真卡上的行为**：全部未测。

---

## 8. 失败分岔判读

| 症状 | 先查什么 |
| --- | --- |
| `txRefused = 1`（BLOCKED） | 发送侧没建：`r4tx` 没跑或它自己失败了——读 `r4tx` 的那几行，不要在这里找 |
| `txRefused = 2`（BLOCKED） | 没有注册 MAC（阶段 5）：帧会不得不编一个源地址，那种帧比没有更差 |
| `txRefused = 3`（FAIL） | 一页帧缓冲分配失败：本调用自己的内存问题，不是前置缺失 |
| `txWqeOk = 0` | WQE 在内存里不是编码要的样子，**且发送停在门铃之前**。日志打了两样东西：32 字节原样 + 逐字段的"得到/想要"（opcode 的低字节位置、owner 位、producer、byte_count、lkey、addr、`srcrb_flags`、`fence_size`、imm） |
| `txCompletions = 0` | 门铃**敲了**（日志有值与偏移）却没有完成：① WQE 有没有真写到硬件读数的地方（SQ 前面那个 64 字节 dummy 头，阶段 6 §4.1）；② TX CQ 的 `comp_eqn`/EQ 顺序（`docs/EQ-ORDERING.md` §5）；③ 门铃偏移是不是写成了 CQ 的 `0x20`（差 0xC，日志两个数都有） |
| `txErrors > 0` | **设备明确说描述符坏**：看 WQE 的字段（日志有）与**描述符地址是否落在 MR 注册的区域里**（`QUERY_MPT` 的 start/length 在日志里）。门铃没到不会产生完成事件，所以这条不是"门铃问题" |
| `txWrapOk = 0` 且 `txPosts` 很小 | 预算/单帧等待到了：读日志里"第几个 post 没有完成"那一行 |
| `txWrapOk = 0` 且 `TxOwnerLapsOk <` 覆盖圈数 | owner 相位的算术（`prod & size`，`mlx4_en_tx.c:682`）——这是本阶段最容易错的一处 |
| `rxFrames = 0` | **第一怀疑阶段 7 的 `mcast`/`base_qpn`**（A0 下广播落点由它决定）：核 `SET_PORT qpn_calc` 的 fw status 与那条 28 字节 context 里的 `mcast` 字段。**不是**阶段 8 的过滤。其次：producer 是否真的发布了（阶段 4）、链路是否 up（`m3portup`） |
| `rxQpnOk = 0` | 帧落到了别的 QP：看首条 CQE 的 `qpn`（日志与工具都打），对照阶段 5 的 indirection QP 与 `base_qpn` |
| `rxMinByteCnt < 60` | `wqe_index` 指到的槽与 CQE 说的不是同一个缓冲（读的是错槽）——日志把"槽 / 环位置"两个数并排打 |
| `rxRecycleOk = 0` | 见 §6 的三问表：补错槽 / 完成丢失（CQ 溢出） / 双占用。日志给出槽号与消费者索引，且**补槽已经停下** |
| `rxLaps < 2`（SKIP） | 流量不够（一段安静网段在默认预算内给不出两圈）：`cx3ctl r4raw 0 20000` 加预算。若**帧确实到了**而环没转两圈，回到阶段 7 的 `mcast`/`base_qpn` |
| 全绿但外部抓不到帧 | 若 `txCompletions > 0` 而外部抓包看不到：先怀疑**上层抓不到**（同一台机器上的抓包口/网段位置），再按 §4.3（`fence_size`）、§4.5（dummy 头、`db_rec_addr`）两条改 |

---

## 9. 回滚

本阶段是**加法**（新选择器 + 新函数 + 新常量 + 两处 teardown 里的清理 + 一条测试）。

1. **代码回滚**（本批将提交成一条提交）：

   ```bash
   git -C /Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4 revert --no-edit <该提交>
   ```

   然后重建、重装、重启。构建产物写在 `/tmp` 下的临时目录里，**没有覆盖
   `outputs/20261010-cx3-m1/build`**，所以仓库里没有需要清理的中间产物。
2. **只回滚运行期的行为**：不跑 `r4raw` 即可——它不改变任何既有阶段的对象；
   它留下的只有"TX 环的 producer 走了一些、RX 环的某些槽被重填过、RX CQ 的消费者索引前进"，
   三样都由下一次 `reset`（链条开头本来就有）清掉。
3. **运行期退路**：`sudo tools/cx3-mlx4/rollback.sh [--purge]`（未改动）。
4. **硬件侧残留**：**已经发出去的帧收不回来**（本阶段的特点）；留下的还有"发送门铃被敲过"
   这一事实与"RX 环被重填过"。两者都不改变链路状态，`reset` 之后设备重新开始。
   60 字节的 `0x88B5` 广播对网段无害，也没有设备会回它。

---

## 10. 用户重启后要跑的命令与期望输出（**预测**）

```bash
sudo tools/cx3-mlx4/verify.sh stage      # 构建 + 校验 + 安装（不重启）
sudo reboot
sudo tools/cx3-mlx4/verify.sh run        # 重启后：整条链，含新的 r4raw
```

**这一条链第一次会让帧离开这台机器**（60 字节广播、ethertype `0x88B5`，一段 60 秒里
默认 ~513 个 post，但**一次一个**、每个都等完成）。

只想单独看这一阶段（前提：同一 boot 里已跑过到 `r4tx` 为止的链）：

```bash
sudo /usr/local/bin/cx3ctl r4raw 1         # 谨慎的第一次：只发一帧，不绕环
sudo /usr/local/bin/cx3ctl r4raw           # 默认：单帧 → 绕环 > 两圈 + 3 秒收帧预算
sudo /usr/local/bin/cx3ctl r4raw 0 20000   # 想凑够两圈（每槽回收 ≥2 次）时把预算加长
```

### 10.1 `verify.sh run` 里这一段的样子（成功分支）

```
TX WQE: every posted descriptor (513 post(s)) reads back as encoded -- opcode 0x0a in the
        low byte, owner bit per lap, byte_count 60                                  PASS
TX completion: 513 arrived, not in error, and the first names the TXBB that was posted  PASS
TX wrap: past two laps of the ring, the owner bit reading back as the lap phase on all 3
        lap(s) and both owner values seen                                           PASS
RX frames: N completion(s), every one naming OUR RX QPN 64                             PASS
RX content: a frame of at least the minimum size was read out of the buffer its CQE
        named (byte_cnt >= 60; the kernel log prints the frame's bytes)                PASS
RX refill: every completion's slot re-posted and the ring turned M whole laps (every
        slot refilled at least twice)                                               PASS   (M ≥ 2 时才 PASS，否则 SKIP)
```

**预期数字**（来自本卡已实测的值）：TX QPN 66、TX CQN 129、SQ 256 个 TXBB、
`9 × 256 + 1 = 2305` 是夹取后的上限而默认是 `2 × 256 + 1 = 513`；RX QPN 64、
RX 环 256 个 WQE；门铃 `BE32(66 << 8) = 0x4200`，偏移 `BAR2 + 0x8000 + 0x14`
（UAR 索引 128 × 4 KiB = 0x80000……见日志里的实际值）。

**三条最可能出现的"不是全绿"**，以及它们各自该读什么：

* `TX wrap … SKIP`：如果第一帧的完成没回来，绕环段根本不会开始——那是上面 `TX completion`
  已经 FAIL 了，这一条只是不重复报。
* `RX refill (two laps) … SKIP`：段上流量不足（默认 3 秒）。**这不是回收补槽错了**；
  先看 `RX frames` 那一行：`N > 0` 且 `RX refill` PASS/SKIP 说明回收是对的。
* `r4raw: the receive side … FAIL … NOT ONE FRAME arrived`：见 §2.1——先查阶段 7 的
  `mcast`/`base_qpn`。

### 10.2 内核日志里应当能看到（同一次调用）

```
r4raw: the frame, one page of ICM memory at bus 0x…, is built once and reused by every post.
       dst ff:ff:ff:ff:ff:ff (broadcast), src xx:xx:… (this port's registered address, stage 5),
       ethertype 0x88b5 (IEEE Local Experimental 1 -- nothing on a LAN interprets it),
       60 bytes without the FCS (the hardware appends the 4), payload 46 bytes starting with
       the tag 'C','X','3','R'
r4raw: the frame's bytes: ff ff ff ff ff ff xx xx … 
r4raw: the memory region every WQE's lkey belongs to, as QUERY_MPT reports it: MPT n,
       lkey 0x00010000, start 0x0, length 0x0. … whether THIS one does is what decides
       whether a descriptor's address is legal
r4raw: the frame buffer at 0x… is OUTSIDE|INSIDE the region [0x0, 0x0) this QP's lkey resolves.…
sqWriteSendWqe: WQE 0 at SQ+0x40 = 0x0000000a owner 0 prod 0 opcode 0x0a fence 1,
       byte_count 60 lkey 0x00010000 addr 0x… ; srcrb_flags 0x0000000e (C and SE), vlan 0, imm 0
SEND doorbell: wrote 0x00004200 (QPN 66 << 8) to BAR2+0x80014 -- the UAR page of index 128,
       offset 0x14. The CQ's doorbell is 0x20, 0xC away…
r4raw: the first transmit completion, out of the TX CQ at consumer index 0, all 32 bytes: …
r4raw: ...decoded: qpn 0, opcode 0x00, is_send 1, wqe_index 0, byte_cnt …, status 0x0000,
       checksum 0x0000 -- those last three recorded, not judged…
```

（最后两行的**具体值**是预测：TX 完成里 `qpn`/`byte_cnt` 两字段的含义没有第二份来源，
所以它们**只记录不判**，这也是文档里写明的原因。）

---

## 11. 我没能查清的点（写出来，免得下一个人以为它们已经清楚了）

1. **`fence_size` 的语义**：只有 `mlx4_en_tx.c:846` 一处赋值，vendored 树里没有第二处解释它。
   本批次照参考填 1；若第一次运行"完成事件正常但帧没出去"，它与 §4.5 两条并列。
2. **TX 完成里 `byte_cnt` / `status` 的含义**：参考的发送轮询只读 opcode 与 `wqe_index`
   （`mlx4_en_tx.c:381-395`），DPDK 也没取到对应片段。所以本批次把它们**只记录不判**，
   并在日志与工具里写清楚——判一个未知对未知是"看起来像测量的检查"。
3. **`wqe_index` 在 RX CQE 里是否就是环槽号**：参考压根不读它（假设 1:1，用消费者索引）。
   本批次按方案要求**用**它，并同时算环位置、把两者不一致当成一个独立的失败来报——但这只
   把问题变成"第一次运行时能读到两个数"，没有把"哪个才是对的"变成已知。
4. **门铃 `0x14` 与 `usr_page` 的对应**：`cqRingDoorbell`（未使用）用的是
   `MLX4_NUM_RESERVED_UARS` 页，本批次用运行时的 `fR4CqUarStart`（= 同一页，本卡 128）。
   两者何时会不同、不同时哪个才对，我没有能证明的材料——只知道本卡相同。
5. **MR 区域是否被硬件校验**：`frameInRegion` 是**报告**。本卡链上注册的是 `[0,0)`，
   即"什么都不覆盖"；参考注册整个地址空间。若第一次运行 TX 完成带 ERROR，这条是第一名嫌疑，
   而修法是**先 reset 再 `r4tx` 之前**用 `r4mr 0 0xffffffffffffffff` 重新注册（链条里
   `r4mr 0 0` 是硬编码的那一行）。

---

## 12. 上机结果（0.43.7，2026-10-12）：**接收通了，发送被 SQ 的 64 字节头挡死**；0.43.8 修掉

本节是**追加**的：上面第 1–11 节是"未上机"时写的预测与设计，**原文一字未改**。本节记录
0.43.7 装上机、重启、跑完链之后的实测，以及它推翻的那条假设。凡与上文冲突处，以本节为准，
§12.9 逐条列出被取代的旧句。

### 12.1 一句话

**接收路径全线打通**（真 ARP 广播帧落进我们的 RX QP，每一条完成都点名我们的 QPN 64，帧的
字节从 CQE 指到的缓冲里读出来了）；**发送一帧没上线**（1 个 post，0 条完成、0 条错误、0 条
超时），根因是**我们给 SQ 加了参考 TX 环没有的 64 字节头**——硬件对 SQ 和 RQ 的 TXBB 0
位置约定本来就不同。修法是去掉这个头（`SQ_WQE0_OFFSET = 0`），本批随 0.43.8 出货。

### 12.2 实测：`verify.sh run`（0.43.7）的结果

**51 PASS / 1 FAIL / 1 BLOCKED / 3 SKIP**。四条与本质有关的：

> 出处说明：下面这段是**那次 `verify.sh run` 自己的输出**（由跑它的人转述），本节其余部分
> （§12.3 起的全部日志行）是本批次**直接从机器上重读的**——`/usr/bin/log show --last boot`，
> 同一 boot 里 09:22:50 / 09:24:43 / 09:25:34 三次调用。三次的内核数字与上面这段一致
> （51 / 60 / 49 条接收完成，发送侧一律 1 post / 0 completion）。

```
RX frames: 51 completion(s), every one naming OUR RX QPN 64   PASS
RX content: a frame of at least the minimum size was read out
            of the buffer its CQE named                      PASS
TX WQE: every posted descriptor (1 post(s)) reads back as
        encoded -- opcode 0x0a in the low byte, ...          PASS
TX completion: NOT ONE completion arrived although 1 post(s)
        were made                                            FAIL
```

**接收的证据（内核日志逐字节打出来的那一帧是真 ARP 请求）**：

```
r4raw: ...decoded with the offsets this build checked (byte_cnt 0x14, wqe_index 0x18,
       status 0x10, owner_sr_opcode 0x1f): qpn 64 (ours is 64), opcode 0x01 , is_send 0,
       byte_cnt 68, wqe_index 0 (the ring position says 0), status 0x0000, checksum 0x3549
r4raw: the first frame: 68 bytes reported by the CQE, 64 of them read out of the buffer
       this slot points at (bus 0x42f478000): ff ff ff ff ff ff d0 f9 28 ff fa eb 08 06
       00 01 08 00 06 04 00 01 d0 f9 28 ff fa eb c0 a8 01 fc 00 00 00 00 00 00 c0 a8 01 01
```

⇒ `who-has 192.0.2.1 tell 192.0.2.252`：**这张卡的端口连在一张活的 LAN 上**，而阶段
2/3/4 的 CQ/环/QP、阶段 5 的 MAC 槽 0、阶段 7 的 `qpn_calc(base 64, mcast DIRECT)` **全部
按设计工作**——广播帧真的落进了我们的接收环，而且每条完成的 `vlan_my_qpn` 都是 64。
另外 CQE 报 **68 字节 = 64 帧 + 4 FCS**，**`FCS_KEEP` 在收包路径上被实测确认**（正是 §4.4
写的那条故意不对称）。

> 数字口径：本节写的是**09:22:50 那一次**调用的 51 条；同一晚 09:24:43 与 09:25:34 又各跑
> 了一次，分别收到 60 与 49 条。**三次的帧数不同是流量决定的**（收帧是外部流量驱动的），
> 不是矛盾；三次的**发送侧完全一致：1 post、0 completion**。`docs/M2B-NOTES.md` §46.1 记的
> 60 就是 09:24:43 那一次。

### 12.3 实测：发送失败，而且**没有任何报错**

三条日志放在一起就是全部事实（同一 boot，`/usr/bin/log show --last boot`）：

```
sqCreate: 256 TXBBs of 64 bytes behind a 64-byte dummy TXBB (16448 bytes total, 5 pages)
          ... the header's dword reads 0xffffffff and 256 of 256 WQE TXBBs carry the same value
sqWriteSendWqe: WQE 0 at SQ+0x40 = 0x0000000a owner 0 prod 0 opcode 0x0a fence 1,
          byte_count 60 lkey 0x00010000 addr 0x42a4cf000; srcrb_flags 0x0000000e (C and SE)
SEND doorbell: wrote 0x00004200 (QPN 66 << 8) to BAR2+0x80014 -- the UAR page of index 128,
          offset 0x14
r4raw: post 1 (TXBB 0, lap 0, prod 0, owner bit 0) produced NO completion within 100 ms
r4raw: the transmit phase is done: 1 post(s), 0 completion(s) (0 in error, 1 timed out)
```

**注意 `SQ+0x40`**：WQE 0 被写在 MTT 基址 **+64**。而 WQE 自己的编码是对的（opcode 在低字节、
owner 位按圈、`byte_count` 60、lkey、帧地址、`srcrb_flags` 都有），门铃也敲在**正确的寄存器**
（`0x14`，不是 CQ 的 `0x20`）与**正确的 UAR 页**（128，即 QP context 里的 `usr_page`）。

**决定性实验（外部、可复用）**：在 `en0` 上同时抓包——

```sh
sudo tcpdump -i en0 -e -n -c 50 'ether proto 0x88b5 or ether host 00:11:22:33:44:55'
# 同时跑链条
```

⇒ **一个包都没抓到**（`en0` 与这张卡在同一张 LAN，默认路由都走 192.0.2.1）。
⇒ **帧没有上线** ⇒ **描述符根本没有被执行**，而不是"执行了但完成事件丢了"。
**这一条把两种完全不同的故障分开**，是本阶段最有价值的一步：判据表里
`txCompletions = 0` 后面那三条嫌疑（WQE 位置 / `comp_eqn` / 门铃偏移）本来是无法区分的，
外部抓包把第一条顶到了前面。

### 12.4 根因：SQ 与 RQ 的 TXBB 0 位置**约定不同**，我们给两个队列用了同一个形状

| 队列 | 参考怎么做 | 硬件约定 | 0.43.7 的我们 | 结果 |
| --- | --- | --- | --- | --- |
| **RQ** | 先 stamp 头，再 `ring->buf += TXBB_SIZE`（`mlx4_en_rx.c:406`、`:482-489`） | **WQE 0 在 MTT 基址 + 64** | 也是 +64 | ✅ **工作**（本 boot 51/60/49 条完成，帧内容读出来了） |
| **SQ** | **无头**：`ring->buf = ring->wqres.buf.direct.buf`（`mlx4_en_tx.c:139`，`ring->buf_size = ALIGN(size*stride, PAGE_SIZE)` `:123`），WQE i 在 `ring->buf + i*TXBB_SIZE`（`:686`） | **WQE 0 在 MTT 基址 + 0** | 也加了 64 字节头 | ❌ **不执行**：0 完成、0 报错、0 帧 |

⇒ 我们把 WQE 写在基址 **+64**，硬件从基址 **+0** 读，读到的是那个
**全 `0xffffffff`（`MLX4_SQ_INIT_OWNER_BIT`，"驱动拥有"）的 dummy 单元**
⇒ **没有可执行的描述符** ⇒ 不发送、无完成、无报错。

**这条不对称被三方证据同时锁定**：我们的 RQ（+64）**工作**；我们的 SQ（+64）**不工作**；
参考的 SQ 明确无头。三方一起只剩一个解释。

### 12.5 这是阶段 6 那条"偏离"的后果（`docs/R4-STAGE6-RESULT.md` §4.1）

阶段 6 自己写下了这件事并要求"若阶段 9 发不出去，第一件事就是去掉这个头"：
`docs/R4-STAGE6-RESULT.md:100-105` 的 §4.1、以及 `docs/M2B-NOTES.md:3901` 那句
"**SQ 有 64 字节 dummy 头**（`SQ_RING_HEADER_SIZE`），参考的 TX 环没有"。
**偏离的理由当时是"两个队列共用一个内存形状，两份审计报告才可比"——这个理由在阶段 9 被
硬件驳回**：两个队列的形状本来就不一样，把它们统一起来等于让发送路径的 WQE 0 落在硬件
不读的地址上。责任在阶段 9 的方案（`docs/R4-STAGE9-PLAN.md` §2 照抄了那个形状），
不在阶段 6 的记录——阶段 6 把偏离也写下来了，这正是它现在能被一眼认出的原因。

### 12.6 0.43.8 的修法（本批改动）

1. `src/Mlx4Hw.cpp` `sqCreate`：`ringBytes = nent * stride`（去掉 `SQ_RING_HEADER_SIZE`）；
   stamping 循环与两个测量同样按"整块就是 WQE 数组"重算（`MLX4_SQ_WQE_OFFSET(i)`）；
   `sqBytes` 相应变成 `nent * 64`，本卡从 16448/5 页 变成 **16384/4 页**——附带的一个可核
   事实：环现在**正好等于** `sq_size_stride` 描述的那个数（4 页 = 16384 = 256 × 64），而原来
   那个头把 WQE 数组整体推后 64 字节，使 16384 字节的环跨到第 5 页（16448 字节）上——多一页，
   且 MTT 覆盖的范围与 strided 环描述的范围**不是同一个范围**。
2. `src/Cx3Mlx4.cpp`：`r4RawTransmit` 的 WQE 写偏移与回读偏移都改用
   `MLX4_SQ_WQE_OFFSET(index)`（= `index*64`），门铃里的 TXBB 计数现在与环位置**一一对应**。
3. `src/mlx4_regs.h`：`SQ_RING_HEADER_SIZE` 删除，换成两个各有一处家的常量
   `SQ_WQE0_OFFSET = 0` 与 `RQ_WQE0_OFFSET = RX_RING_HEADER_SIZE (64)`，**ASCII 注释写明
   这条不对称与两侧的参考行号**；`MLX4_SQ_WQE_OFFSET(index)` 只给 SQ（RQ 的算术留在它自己
   的调用点，因为"把两套约定折进一个宏"就是 E13 的成因）。
4. `tools/test-cqe-offsets.c`：**新增 `test_sq_rq_header_asymmetry()`（22 项）**——SQ 的
   `WQE0 == 0`、RQ 的 `WQE0 == 64 == TXBB_SIZE == RX_RING_HEADER_SIZE`、两者**必须不等**、
   各自的 `WQE i` 偏移公式、以及"SQ 的 `offset / TXBB_SIZE` 必须还原出索引而 RQ 不能"
   这条身份式。115 → **137** 项。
5. `tools/verify.sh` / `tools/cx3ctl.c`：所有 "dummy TXBB"/"behind a 64-byte header" 的文字
   改掉；`txCompletions = 0` 那支 FAIL 现在**点名用 tcpdump 从外部把两种故障分开**。
6. `kext/Info.plist`：0.43.7 → **0.43.8**。

### 12.7 门铃是对的——它**没有**被改（四个环节的排除依据）

修复只碰描述符的位置，**没有碰门铃**，因为它在同一晚被逐一排除：

| 环节 | 核过的值 | 参考依据 | 结论 |
| --- | --- | --- | --- |
| 门铃**地址** | UAR 页 128 + `MLX4_SEND_DOORBELL 0x14` = `BAR2+0x80014`；本卡 `num_uars = 256`（devcap `uar_size` = 0 → `1 << (0+20)` = 1 MiB → /4096），128 在范围内 | `mlx4_en_tx.c:918-919` | ✅ |
| 门铃**值** | `(qpn & QPN_MASK) << 8` = `0x4200`，与 `ring->doorbell_qpn = ring->qp.qpn << 8`（`:231`）+ `writel(cpu_to_be32(ring->doorbell_qpn), map + MLX4_SEND_DOORBELL)` 逐字相同 | `mlx4_en_tx.c:231`, `:918-919` | ✅ |
| TX CQ 接线 | CQN 129 与 CQN 128（**实测收到 51 条完成**）除环大小与各自 MTT/记录地址外逐字段相同（都 `comp_eqn 28`、hw UAR 128） | 本 boot 两次 `cqCreate` 日志 | ✅ |
| WQE 回读 | 判据 1 PASS：`opcode 0x0a` 在低字节、owner 位按圈、`byte_count` 60 | 本 boot 日志 | ✅ |

**并且：不要把 producer 写进门铃或 opcode。** 参考的**非 blueflame** 路径（我们走这条）里
`opcode = cpu_to_be32(MLX4_OPCODE_SEND) | owner_bit`（`mlx4_en_tx.c:761-762`），producer 只在
**blueflame** 分支并进 `opcode`（`:897` 的 `opcode |= (bf_prod & 0xffff) << 8`），门铃值是纯
`qpn << 8`。**遗留的一处偏离（本批没改，记录在案）**：我们的 `sqWriteSendWqe` 会把
`prod` 并进 `owner_opcode` 的 bit 8..23（`MLX4_TX_PROD_SHIFT`），那是 blueflame 的约定。
它的**风险可评估**：一圈的**第一个** post 的 `prod` 恰为 0，所以那一次的 4 个字节与参考
内存路径**逐字节相同**（本卡的第一次 post、第一次绕圈、以及每一圈的第一次都是这样）；
若这一修之后"第 1 条完成回来了、绕环段出问题"，这里是**第一个**要改成 0 的字段。

### 12.8 本批门禁与产物（0.43.8）

* `boot-path` **PASS**（`start()` 可达 19 个函数里无设备写、无 DMA、无 reset）
* **146 + 8186 + 137 + 137 + 45** 项检查 **0 失败**（第三个数是新增 22 项之后的 CQE/WQE 测试）
* 未定义符号 **352/352 全部解析**；四个 `-Werror` 档**零告警**
* `check-hardcoded.py --strict` 通过（"no device-sourced literal outside the headers that define them"）
* `bash -n tools/verify.sh` 通过；`verify.sh` 用到的 ABI 索引名 **128 个唯一名 / 130 处引用，
  0 缺失**（与改动前同数）
* **kext sha256 = `58c1ab03d5d705e829e14ecf4c777a011d4482e53aa565114097be9ae2586716`**，
  382176 字节；**可复现**：同一份树连跑两次（`sudo -A env BUILD_TAG=0.43.8
  OUT=/tmp/cx3-438f|438g bash build.sh`）——两次 sha 相同、`cmp` 逐字节相同。
  （0.43.7 的基线本轮也在动手之前重跑过一次，`e30f1d3c…` / 382176 字节，与本文档原文一致，
  所以"改了什么是本次造成的"是有对照的。）

### 12.9 本节取代了上文哪几句（**不改原文**，在这里点名）

| 旧句位置 | 旧句说的 | 现在 |
| --- | --- | --- |
| §4.5 | "若第一次运行门铃敲了没有完成，SQ 前面那个 64 字节 dummy 头……仍是现成线索" | **它就是根因**，已修（本节 §12.4/§12.6） |
| §8 表格 `txCompletions = 0` 行 | 三条并列嫌疑（dummy 头 / `comp_eqn` / 门铃写成 `0x20`） | 第一条**已确认为真并修掉**；另两条在本 boot 被实测排除（§12.7） |
| §10.2 | 预测日志行 `sqWriteSendWqe: WQE 0 at SQ+0x40 = …` | 0.43.8 起是 **`WQE 0 at SQ+0x0`** |
| §10.1 | 预测 `TX completion: 513 arrived …` | 0.43.7 实测 **0 条**；0.43.8 的期望见 §12.10 |
| §1 表格 `docs/COVERAGE.md` 行 | "`r4raw` 行（链内，证据栏 **未运行**）" | 已改为**实跑**（RX 通过、TX 待 0.43.8 复测） |
| §7"未测" | "`r4raw` 从未运行……一帧都没有发出过" | 已运行三次；**发出的帧数仍为 0**（这正是本次的结论） |

**别人文档里被本次改动"变旧"的引用**（本批次**没有改它们**，逐条列在这里：既有结论都不受
影响，老的只是名字）：

| 位置 | 那条引用 | 现在的状态 |
| --- | --- | --- |
| `docs/R4-STAGE6-RESULT.md` §4.1 与 §5 | "SQ 内存布局是 `[64 字节 dummy TXBB][WQE 数组]`" | 布局已改（无头）。该节记录的是**当时的实现与它自己的偏离声明**，结论"若阶段 9 发不出去，第一件事是去掉这个头"已被实测证实 |
| `docs/EXTERNAL-REFS.md` §5 第 65 行 | "我们的 `SQ_RING_HEADER_SIZE = 64`、每 64 字节一个 stamp ✅ 一致" | 常量 **已删除**；"stamp 步长 = TXBB = 64"这条**互证结论不受影响**（stamp 循环仍是每 64 字节一次） |
| `docs/M2B-NOTES.md` §42.3、§46.3/§46.3.1 | §42.3 把"SQ 有 64 字节 dummy 头"列为"阶段 9 发不出帧时第一批要反向试的偏离"；§46.3 记的就是本节的根因，§46.3.1 记了"TX CQ 接线与一个已知能工作的 CQ 逐字段相同" | §42.3 的第一条**已被实测证实并修掉**；§46.3 与本节同源（本节是它的实现记录侧），**没有改它们** |
| `docs/R4-STAGE7-RESULT.md` §5 | `MLX4_SQ_INIT_OWNER_BIT` 的来历 | 不受影响（值没变） |

### 12.10 重启后要跑的命令与判据（0.43.8）

```bash
sudo tools/cx3-mlx4/verify.sh stage      # 构建 + 校验 + 安装（不重启）
sudo reboot
sudo tools/cx3-mlx4/verify.sh run        # 整条链，含 r4raw
```

**必须同时开第二个终端抓包**（这是 0.43.7 那个决定性实验，抓的是**帧有没有上线**，
而不是完成事件有没有回）：

```sh
sudo tcpdump -i en0 -e -n -c 50 'ether proto 0x88b5 or ether host <本卡 MAC>'
```

* **期望 1（修的核心）**：`TX completion: N arrived, not in error, and the first names the TXBB
  that was posted  PASS`——**完成事件本身回来了**就是描述符被执行了的证据。
* **期望 2（更硬的一条）**：`tcpdump` **看到 60 字节的 `0x88B5` 广播**（`ff:ff:ff:ff:ff:ff` →
  本卡 MAC，ethertype 0x88B5）。**这条才证明帧真的上线**；完成事件只是"设备说它处理了"。
  注意 `en0` 与这张卡在同一张 LAN 上才看得到（本机就是这样），若在交换机上抓，广播会到。
* **期望 3**：`TX wrap: past two laps …`（513 个 post、两种 owner 位都出现）；
  `RX frames` / `RX content` 仍应 PASS（这部分 0.43.7 已经通过，本次没动 RQ 的任何偏移）。
* **期望 4（可选自证）**：因为我们**不置 `FORCE_LOOPBACK`**，自己发的广播在交换式网段上
  不会回来，所以 `RX frames` 里**不应**出现我们自己的帧；`rxOurs = 1` 只在集线器/回环式
  网段上才会出现——出现了也不矛盾，那说明这一小段是共享介质。

**若仍不绿，按这个顺序读**（每条都指向一个可判的数）：

1. `txCompletions = 0` 且 **tcpdump 也空** ⇒ 描述符仍未被执行。此时查：
   `sqCreate` 日志的 `WQE 0 at offset` 是不是 0、`WQE 0` 的 dword 是不是 `0xffffffff`、
   `sqWriteSendWqe` 打的偏移是不是 `SQ+0x0`、门铃是不是 `BAR2+0x80014` / `0x4200`。
2. `txCompletions = 0` 但 **tcpdump 看到了帧** ⇒ 描述符**执行了**，完成事件去了别处：
   查 TX CQ 的 `comp_eqn` 与 EQ 的顺序（`docs/EQ-ORDERING.md` §5），不是查 WQE。
3. 第 1 条完成回来了、**绕环段**出问题 ⇒ §12.7 那个 producer-in-opcode 的偏离是第一个要
   改成 0 的字段；其次是 `fence_size`（§4.3）。
4. 完成回来了但**帧带 ERROR** ⇒ 看 `QUERY_MPT` 的 start/length 与帧地址的包含关系
   （本 boot 实测 `start 0x0, length 0xffffffffffffffff`，帧在里面，所以这条现在可能性很低）。

---

## 13. 0.43.9：发送仍失败之后**逐行审**，推翻一条我自己的推论，抓到一个字段级遗漏（2026-10-12，未上机）

本节是**追加**，以上各节一字未改。背景：0.43.8 去掉 SQ 的 64 字节头之后上机，发送**仍然一帧没上线**，
`verify.sh run` = `51 PASS / 1 FAIL / 1 BLOCKED / 3 SKIP`，RX 侧照旧全通（60 个完成、全是真 ARP）。
于是把范围收到"发送路径本身"，做了三件事：① 核一条我自己提出的"门铃记录（DB record）没写"假说；
② 把发送的写序列逐行对照参考与 DPDK；③ 把"参考给 TX 队列的 QP context"与"我们的"逐字段比。

### 13.1 ★ 我自己的假说被推翻：发送门铃记录（DB record）不是门禁

我先提出的假说是"PRM 的发帖三步只做了第 1、3 步，第 2 步（更新 Doorbell Record）从没做"。
**这个推论是错的，而且是被第三份实现推翻的**：

| 实现 | 接收侧写记录？ | 发送侧写记录？ |
| --- | --- | --- |
| FreeBSD `mlx4_en` | 是（`mlx4_en_rx.c:253`） | **否**（`mlx4_en_tx.c` 里没有该行） |
| rdma-core / libmlx4 | 是（`providers/mlx4/qp.c:577`，`_mlx4_post_recv`） | **否**（`:505-506` 只有 `mmio_write32_be(ctx->uar + MLX4_SEND_DOORBELL, qp->doorbell_qpn)`） |
| DPDK mlx4 PMD | 是 | **否**（`mlx4_txq.c` 的发送结尾只有 `rte_write32(txq->msq.doorbell_qpn, ...)`；`txq->msq.db` 在 TX 路径零命中） |

⇒ **四份实现一致**（含能跑的生产环境）：只写接收记录，发送记录谁都不写。我原来的推理错在**类比**：
接收记录是"有 WQE 可取"的门禁，而发送的"有 WQE 可发"由 post 的索引/门铃本身承载，两者语义不同。
**这条已作废，`x` 不再动代码。**

顺带一个值得留下的观察：rdma-core 与 `mlx4_en` **都只在 BlueFlame/inline 分支**把 producer 并进
opcode 的 bit 8..23（`mlx4_en_tx.c:897`、rdma-core `qp.c:480`），走纯 MMIO 门铃那条分支时
opcode 是干净的、门铃**只有** `qpn<<8`。我们两条都做，但**首次 post 时 prod = 0**，与参考逐位相同，
所以这一项也不构成本次的因（只记档，不改）。

### 13.2 逐行审发送序列：顺序对，但**屏障是假的**

三个怀疑点逐个核：

* **顺序与设置**：`sqWriteSendWqe`（`src/Mlx4Hw.cpp`）是"字段 → 屏障 → owner_opcode → 屏障"，
  owner 位最后写，与 `mlx4_en_tx.c:897-919` 一致。**没有顺序错误。**
* **`prod` 簿记**：opcode 带 prod（首次为 0）、门铃只有 `qpn<<8`（实测 `0x00004200`）。**对。**
* **WQE 与 MTT 是否同一块分配**：是**结构性保证**——`sqCreate` 的 MTT 条目就是从将要写 WQE 的
  那块 icm 上建的（`Mlx4Hw.cpp` `list[i] = ring->pages[i].busAddr`），WQE 写在
  `fR4TxSq->virt + MLX4_SQ_WQE_OFFSET(index)`。**该怀疑点划掉。**

**★ 但发现了一个真缺陷：`OSSynchronizeIO()` 在本项目构建的架构上是空函数。**

```c
/* MacOSX.sdk/System/Library/Frameworks/Kernel.framework/Headers/libkern/OSAtomic.h:549-556 */
#if defined(__arm__) || defined(__arm64__)
extern void OSSynchronizeIO(void);
#else
static __inline__ void
OSSynchronizeIO(void)
{
}
#endif
```

本项目只编 x86_64（`build.sh` 的 `-arch x86_64`）⇒ **全项目 13 处调用全部编译成零字节**，
连编译器屏障都不是；而其中三处注释**自称它是 wmb()**（`Mlx4Hw.cpp` 的 HCR 提交处原文
"Linux wmb() / mmiowb(): keep the field stores ahead of the GO bit"）。
`OSAtomic.h:541-548` 的文档注释说的是 PowerPC 上的语义——名字读起来像屏障，本机没有。

**严重性如实说：多半不是本次的因。** x86-64 是 TSO，WB 描述符存储不会被重排到其后的 UC 门铃存储之后，
且门铃前还隔着一整个 `memcpy` 回读与一批比较，那些 store 早已退休。记这一条是因为**代码宣称了它没有的保证**，
而这正是下一次改动（编译器内联更狠、或某次 store 被优化掉）会变成无迹可循的真故障的状态。
修法与自动门禁见 §13.5。

### 13.3 ★★ 抓到的正主候选：`pri_path.feup` —— 参考的共享 fill 对 TX 队列才设的位，我们**一次都没写**

把"参考给发送队列的 248 字节 QP context"与我们的逐字段比，**差异恰好两处**：

| QPC 偏移 | 字段 | 参考（TX 队列） | 我们（0.43.8 及以前） |
| --- | --- | --- | --- |
| **0x3a** | **`pri_path.feup`** | **`0x40`** | **`0x00`** |
| 0xa0 | `db_rec_addr` | 移位形（`<< 2`） | 未移位（阶段 6 有意为之、已记档） |

原因在同一份共享 fill 里，而且**唯一的条件就是那个参数的符号**：

```c
/* mlx4_en_resources.c:62-65 —— 共享 fill 里对 feup 的唯一赋值点 */
	if (user_prio >= 0) {
		context->pri_path.sched_queue |= user_prio << 3;
		context->pri_path.feup = 1 << 6;      /* MLX4_FEUP_FORCE_ETH_UP, qp.h:187 */
	}
```

两条调用路径分得很干净：

* **TX**：`mlx4_en_tx.c:233-234` 把 `user_prio` 传下去，调用方 `mlx4_en_netdev.c:1362-1363` 传
  `i / priv->num_tx_rings_p_up` ⇒ **恒 ≥ 0** ⇒ **`feup = 0x40` 被设上**；
* **RX**：`mlx4_en_rx.c:944-945` 传 **`-1`**；RSS 间接 QP 也是 **`-1`**（`:1056-1057`）
  ⇒ 条件为假 ⇒ `feup` 留 `0x00`。

**我们全项目写 `feup` 的次数：0。** 于是**我们的对错精确复现了参考的不对称**：RX 侧（0，即 `-1`
那个实例化）在这张卡上**实测是通的**（51–60 个完成），而**只有 TX 错**。

偏移 `0x3a` 不是一行能引出来的：它是 `pri_path + 34`（`qp.h:140-160` 的字段序），而 `pri_path`
从 `0x18` 开始（`qp.h:196-236`）。所以本批没有把它写成"手抄的常量"，而是**用编译器从一个 mirror
算出来并断言**（§13.5）。

**其他实现都设这个位**，只有我们不设：`drivers/infiniband/hw/mlx4/qp.c:1805`、
`mlx4_core/mlx4_resource_tracker.c:821` 与 `:5344`。**DPDK 不是反例**：它自己不造 context，
走 verbs（`mlx4_txq.c:431` `create_qp`，`:442/:455/:467` `ibv_modify_qp`），context 是内核 mlx4_ib 填的，
而内核在以太网/RoCE 路径上正是 `path->feup |= MLX4_FEUP_FORCE_ETH_UP`。

**反方意见（如实说）**：`qp.h:187` 的注释只有 "force Eth UP" 四个字，CX-3 的 PRM 是 NDA 的、
公开的 CX-4 PRM 不含这个字段，所以**固件拿这个位做什么并没有被证实**。端口已经起来、RX 在同一张卡上
工作，所以"链路没 up"**不是**假说；假说是"一个不声称 Ethernet-up 的发送队列，描述符根本不被执行"，
这与症状（门铃敲了、无完成、无错误完成、线上无帧）完全吻合。**这是最强的源码级候选，不是已定因。**

### 13.4 顺手排掉的两项（免得下次再被当候选）

* **`pri_path.fl` / `vlan_control` 那两行**受 `counter_index != 0xFF` 保护
  （`mlx4_en_resources.c:66-73`），我们写 `counter_index = 0xff` ⇒ 条件为假 ⇒ 参考也留 0。**不是差异。**
* **`param3` 的 bit 30** 受 `IFCAP_VLAN_HWCSUM` 保护，参考的 netdev 支持它（`mlx4_en_netdev.c:2249`）
  ⇒ 不设。**不是差异。**
* **UAR 128 是否"可用于发送"**：`MLX4_RESERVE_ETH_BF_QP` / `MLX4_RESERVE_A0_QP`
  （`device.h:251-252`）是 **QPN 预留区间**位，不是 UAR 类别；参考在非 BlueFlame 下根本不改
  `usr_page`（`mlx4_en_resources.c:58`，`mlx4_en_tx.c:235-236` 只在 `bf_enabled` 时覆盖），
  即参考的 TX QP 用**和 RX 同一个 UAR**，与我们一致；而且同一 UAR 上的那张 CQ 是工作过的。**划掉。**

### 13.5 本批做了什么（可核的具体改动）

1. **`feup = MLX4_FEUP_FORCE_ETH_UP`，只对 TX**：`qpFillContext` 按参考的机制写成
   `if (userPrio >= 0) { sched_queue |= userPrio << 3; feup = 0x40; }`，并把 `userPrio` 提成参数，
   让三个调用点各自把**参考的符号**写在明处：RX QP `-1`、RSS QP `-1`、TX QP `0`。
   RX 侧**没有**加（参考对 RX 传 `-1`，加了反而偏离）。日志点名"这是参考对 TX 队列才设的位、
   RX 走 -1 不设"。
2. **偏移是构建期事实**：`tools/test-cqe-offsets.c` 新增 `test_qpc_path_offsets`（`+31` 项，
   `137 → 168`），用**参考声明的 mirror** 让编译器算出 `pri_path.feup` 并与
   `QP_CONTEXT_PRI_FEUP_OFFSET` 对断言；同时断言本文件**早就在用**的三个路径偏移
   （`counter_index 0x1c` / `ackto 0x20` / `sched_queue 0x38`）与 QPC 的其余全部偏移——
   这就是它可信而不是"第四份手抄"的原因。
3. **报告，而不是判分**：新增三个标量（写入值 / QUERY_QP 回读值 / 两者是否相等）+ ABI 三个成员，
   `tools/verify.sh` 与 `cx3ctl r4tx` 都打印。理由与 `param3`/`sched_queue` 同：参考没有任何一行
   回读它，拿未知去判分就是把未知报成已知。**`CX3_SCALAR_OUT_MAX` 从 16 抬到 20**——
   `r4tx` 原本用满 16 个，而这三条是三个**必须分开读**的事实（写没写 / 固件留没留 / 查不到），
   不该压成一个数（那正是 `docs/DEFECTS.md` F7 的形状）。
   > **§14 更正（2026-10-12，0.43.10）：上一句的那个决定是错的。** 上限不是我们的参数，是内核的
   > `kIOExternalMethodScalarOutputCountMax = 16`；抬到 20 的后果是 **0.43.9 的每一次 `r4tx`
   > 调用都返回 `kIOReturnBadArgument`**，所以本批这三条判据**一次都没有被问到**。三个标量本身
   > （分开读）的理由仍然成立，0.43.10 里它们原样保留。见 `docs/DEFECTS.md` F13 与 §14。
4. **屏障修真**：本文件定义一次 `static inline void mlx4Wmb(void) { __sync_synchronize(); }`，
   **13 处全部替换**（不只 TX 那两处——同一句错话在 HCR 路径上危害一样）。函数注释里逐字引了
   SDK 的 `#else` 分支。
5. **屏障的自动门禁**：`tools/check-hardcoded.py` 新增第二条规则，`src/` 在**值位置**调用
   `OSSynchronizeIO` 就让 build 失败（复用该脚本已有的注释/字符串剥离，所以**正文里提到**这个
   名字仍然合法）。双向都实测过：真调用会触发、注释里提到不触发。
6. `docs/DEFECTS.md` 两行（**D14** 屏障是空函数、**E14** `feup` 缺失），
   版本号 `0.43.8 → 0.43.9`。

### 13.6 本轮的方法论收获（写下来，因为它是两条结论真正的来路）

**"两份实现一致"不足以定因——要找第三份。** 我原来的"PRM 第 2 步没做"是被 **DPDK**（第三份）
推翻的（§13.1）；而 `feup` 是从**反方向**发现的：不是找同一路径的更多实现，而是问
**"参考那一份共享 fill，对两种队列产生的 context 是不是同一个"**——不是。

所以下次任何"某字段没用 / 某步骤可省"的结论，先问一句：
**是不是 RX 与 TX 对同一份 fill（或同一段代码）的实例化不同？** 这正是 **E13**（同一个内存形状套给两种队列）
与 **E14**（同一个 context 字节）共同的那一个形状。

### 13.7 证伪条件（写在跑之前，好让这一次跑能把它结掉）

这是一个**一字节**的 context 改动，用阶段 9 自己的六条判据判：

* **若** TX 完成事件回来了、并且 `tcpdump` 看到帧 ⇒ **这个位就是因**。
* **若** WQE 仍然照发、门铃照敲，而**没有完成、没有错误完成、线上没有帧** ⇒ 这个字段**不是**门禁，
  那么剩下的最强候选是**传输通路本身**：参考在 CX-3 上有 BlueFlame（`bf_reg_size > 0` 是
  `mlx4_bf_alloc` 的前提，`mlx4_en_tx.c:157-166`）因而**永远走 BlueFlame 分支**（`:890`），
  也就是说我们走的"内存 + 门铃"这条 fallback 路径，**shipping driver 可能从不经过**。
  这已经写进 `src/mlx4_regs.h` 该常量旁边，以及 `verify.sh` 那条 SKIP 的文字里。
* `db_rec_addr`（0xa0，那条记录）**两种情况都不是候选**：四份实现都只写接收记录（§13.1）。

### 13.8 重启后要跑的命令与期望（含"帧真上线"那条）

```sh
sudo -A ~/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/install.sh <新 kext>   # 由用户执行
tools/verify.sh run
```

**必须同时开第二个终端抓包**（这条证明帧真的上线，不只是"设备说它处理了"）：

```sh
sudo tcpdump -i en0 -e -n -c 50 'ether proto 0x88b5 or ether host <本卡 MAC>'
```

期望（按重要性排序）：

1. **`r4tx: pri_path.feup` 两行**：
   * `TX QP: pri_path.feup was written as 0x40 …` = **PASS**（写入值取自 context 影像，不是字面量）；
   * `TX QP: QUERY_QP reads pri_path.feup back as 0x40 …` = **PASS** ⇒ 固件**留住了**这个字节，
     于是"发送仍不动"**不能**再归因于固件持有的 context；若这一行是 **SKIP 且文字是 DROPPED**
     （写入 0x40、回读 0x00），则**这条线到此为止**，不要再在这条路上花时间，按 §13.7 转 BlueFlame。
2. **`TX completion: N arrived, not in error, and the first names the TXBB that was posted` = PASS**
   ——完成事件本身回来就是描述符被执行了的证据。
3. **`tcpdump` 看到 60 字节的 `0x88B5` 广播**（`ff:ff:ff:ff:ff:ff` → 本卡 MAC）。
   **这条才证明帧真的上线。**
4. `TX wrap: past two laps …`（513 个 post、两种 owner 位都出现）；
   `RX frames` / `RX content` 仍应 PASS（RQ 侧本批一行未动）。
5. 因为我们**不置 `FORCE_LOOPBACK`**，自己发的广播在交换式网段上不会回来，`rxOurs` 允许为 0。

### 13.9 本轮没能查清的点

* **`MLX4_FEUP_FORCE_ETH_UP` 在 CX-3 固件里的确切语义**：强制的是端口的 eth 状态还是 QP 的 vport
  状态。公开材料里查不到（CX-3 PRM 是 NDA、CX-4 PRM 不含该字段），只有四处实现无条件设它。
  **这是本节最大的不确定处**，也是它被写成"候选"而不是"因"的原因。
* **为什么 RX 路径不需要它**：合理的原因是"接收不需要 eth-up"，但同样没有原文依据。
* **固件会不会像丢 `param3` 一样丢 `pri_path` 的某些位**：阶段 4 实测 `param3` 写 `0x20000000`
  回读 `0x0`。如果 `pri_path` 也被这样对待，那么 `feup` 写了也白写——**这正是 §13.5 第 3 条把回读做成
  标量的原因**：它顺手把这个问题在同一次上机里问出来。
* **BlueFlame 到底需要什么**（若最终要转这条线）：`mlx4_bf_alloc` 走 `mlx4_pd.c:172-231`，
  需要 UAR 上有 `bf_map`（`io_mapping_map_wc`），即需要一个**能被映射为 WC 的 UAR 页**；
  本卡 `bf_reg_size` 的实际取值本轮没有从 `QUERY_DEV_CAP` 的原始字节里解出来核过（这是下一步的第一件事）。

---

## 14. 0.43.9 上机的真实结果：**`r4tx` 整个选择器不可调用**，于是 §13 的三条判据一次都没被问到

### 14.1 一句话

0.43.9 装上去之后，**`r4tx` 的每一次调用都返回 `kIOReturnBadArgument`（`0xe00002c2`）**。
根因不是参数、不是尺寸、也不是设备：**内核的标量出参块就是 16 宽，而我们把这个上限当成自己的旋钮
从 16 抬到了 20**（为了给 `feup` 腾三个标量槽）。于是那个调度行声明的 19 个标量出参**永远不可能被满足**，
内核在**进入这个选择器的代码之前**就把调用拒了——**函数体从未执行**（被拒在内核的哪一层见 §14.8：
不是派发表那道计数检查，那道这道题会通过）。因此 §13 那批的 `feup` 修复
（写没写 `0x40`、固件留不留它）**本轮完全没测到**，判据一条都没问到。

### 14.2 穷举证据（**用户上机实测**，本轮，0.43.9）

* `in` / `out` / `struct` 的**所有组合**都试过（struct `384…416`、out `14…20`、in `2`）——
  **全部失败**，同一个 `0xe00002c2`；
* 但**紧邻的三个选择器全成功**：`r4raw`（sel 37，16 出 / 568 字节结构）、`r4ingress`（15 出）、
  `r4qp`（12 出）——同一个 boot、同一套调用方式；
* `cx3ctl sizes` 显示**全项目只有一行越界**：

  ```
  34  r4qp       out=12
  35  r4ingress  out=15
  36  r4tx       out=19   ← 超过内核的 16
  37  r4raw      out=16
  ```

⇒ **"所有尺寸组合都失败"由此只有唯一自洽的解释**：不是参数错、不是设备错，而是**这一行声明了一个
不存在的宽度**。这也是为什么它读起来像"权限/参数个数"问题（见 14.4）。

### 14.3 根因（源码级，两处）

1. **上限是内核的**：`Kernel.framework/Headers/IOKit/IOUserClient.h`

   ```c
   /* /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/System/Library/Frameworks/
      Kernel.framework/Headers/IOKit/IOUserClient.h:116-119 */
   enum {
       kIOExternalMethodScalarInputCountMax  = 16,
       kIOExternalMethodScalarOutputCountMax = 16,
   };
   ```

   本仓库 `build.sh` 用 `-nostdinc -I$SDK/Headers`（`SDK=tools/research/mksdk`）编译，看到的是它的副本
   `tools/research/mksdk/Headers/IOKit/IOUserClient.h:150-153`，**同样是 16**。头文件自己的注释把这两个
   常量称作 *documentary*：真正生效的是基类拿调度行声明的 `scalarOutputCount` 与 16 的**比较**。
2. **是我们自己抬的**：上一批为了给 `feup` 加三个标量（`feupSent` / `queryFeup` / `feupSurvived`），
   把 `CX3_SCALAR_OUT_MAX` 从 16 改成 20，注释里写着"the cap was the thing to move rather than the
   facts to compress"。**这个推理是错的**：`CX3_SCALAR_OUT_MAX` 只是我们自己对这个内核常量的**复述**，
   不是第二份可调的真相。

### 14.4 危害的形状：**所有构建门禁都会通过**（这才是它值一行的地方）

抬上限的同时，`CX3_STATIC_ASSERT(kR4TxOutCount <= CX3_SCALAR_OUT_MAX)` 这条**唯一能挡住它的门禁，
两边一起动了**，所以：

* 编译通过、`verify.sh` 的 ABI 名称解析通过、`cx3ctl sizes` 与内核派发表一致（同一份头文件）；
* 错误只在**运行时**表现为"该选择器彻底不可用"，而且报错码（`kIOReturnBadArgument`）指向的是
  **权限/参数个数**这一类完全不同的原因。

本批把这条形状**证明**过一次（单文件编译，MacKernelSDK，产物在 `/tmp/abi-guard-test.*`）：

| 实验 | 期望 | 实测 |
| --- | --- | --- |
| 0.43.9 的数字（上限 20、`r4tx` 19） | 什么都不报（这就是那个洞） | **编译通过，无任何告警** |
| 上限改成 17（其余不动） | 新加的"与内核常量相等"断言必须失败 | **失败**：`static assertion failed due to requirement '17 == kIOExternalMethodScalarOutputCountMax'`（`src/Cx3Mlx4UserClient.cpp:42`） |
| `r4tx` 加第 17 个标量 | 每选择器断言必须失败 | **失败**：`kR4TxOutCount <= 16` 不成立（`src/cx3_abi.h`） |

### 14.5 0.43.10 改了什么（本批）

1. **上限回 16，并且焊到内核常量上**：`src/cx3_abi.h` 的 `CX3_SCALAR_OUT_MAX = 16`；在**能同时看见
   两个数**的 `src/Cx3Mlx4UserClient.cpp`（它 include 了 `<IOKit/IOUserClient.h>`）加
   `CX3_STATIC_ASSERT(CX3_SCALAR_OUT_MAX == kIOExternalMethodScalarOutputCountMax, ...)`，入参侧同样一条。
   **再抬一次就是编译失败**，不再是"选择器静默不可用"。
2. **`r4tx` 压回 16，三条 `feup` 判据原样保留**（仍然是三个独立标量）。腾槽的方式：
   * `cqn` 与 `topReservedStart` **移出标量块**（它们是消息的背景量，不是判据；对它们的判据
     `kR4TxOutNumberSafe` 仍是标量、仍在这里决定），值留在 `Cx3R4TxResult` 里，`cx3ctl r4tx` 与内核日志照旧打印；
   * `stepsAccepted` / `statesConfirmed` **无损打包**进一个标量（低 32 位 / 高 32 位，移位常量
     `kR4TxOutStatesShift` 由 `verify.sh` 按名字解析）。
   **没有采用"把 0/1 判断打包成位域"那条路**，理由写在 `src/cx3_abi.h` 里那条枚举旁：`r4tx` 的 0/1 判断
   **每条都带第三态**（`CX3_R4TX_NOT_MEASURED`，而且是**逐条**的：CQ 建好之后拒绝，`cqReady`/`numberSafe`
   是"已测"，layer 2 那几条仍是"未测"）。位装不下三个状态，装进去就把"没测"读成"测出来是错的"
   —— 那正是 `docs/DEFECTS.md` D10。
3. **门禁补齐**：38 个选择器逐个核过（见 14.6），缺的出/入参断言补上；顺手抓到一条**断言写错了名字**：
   `peek` 的入参断言写的是 `kPeekInCount`（那是调用方传的**值**）而不是宽度 `kPeekInCount_`，
   于是它一直在检查一个与上限无关的数，而消息说它在检查宽度；同时那个宽度在两个表里（派发表、
   `cx3ctl sizes`）是**字面量 3**，现在都改成名字。
4. **`tools/verify.sh` 多一道独立门禁**：每次 `run` 在碰设备之前，用 `cx3ctl sizes` 与从
   `src/cx3_abi.h` 里读出的上限比较，任何选择器的出参宽度超上限就 FAIL（错误消息直接点明
   "unreachable"，而不是让人再去猜权限）。

### 14.6 全部 38 个选择器的复核结果

`cx3ctl sizes`（0.43.10 构建产物）逐行核过：**38 行、出参最大 16、无一行越界**，
`36 2 16 400 r4tx`（`r4tx` 从 19 降到 16）。逐选择器的断言覆盖：见 `docs/DEFECTS.md` F13 的 Guard 栏
（补了 8 条出参 + 8 条非零入参断言；声明 0 个入参的选择器不加断言，因为 `0 <= 16` 不是检查）。

### 14.7 重启后要跑的命令与判据（`feup` 三条现在从哪读）

与 §13.8 相同（`tools/verify.sh run` + 第二个终端跑 `tcpdump`），差别只有**读法**：

* 三条判据**仍然是三个独立标量**，`verify.sh` 用 `abi()` 按名字取索引，所以**命令与命令的输出格式都不变**：
  `TX QP: pri_path.feup was written as 0x40 …` / `TX QP: QUERY_QP reads pri_path.feup back as 0x40 …`；
  `cx3ctl r4tx` 与内核日志照旧打印写入值/回读值/是否相等。
* 变的是**索引号**（`kR4TxOutFeupSent/Query/Survived` 现在是 13/14/15），以及
  `stepsAccepted`/`statesConfirmed` 现在从**同一个标量**里取（低/高 32 位），`cqn` 与 top-reserved 线
  改从结构体（`cx3ctl r4tx`）读。任何直接按下标读 `rawjson 36` 的手工脚本都要跟着改。
* **先确认 `r4tx` 能被调用**：`verify.sh run` 的 r4tx 段若再出现 `the call failed: rc=…`（配
  `0xe00002c2`），那就是又踩了同一个坑，不要再去枚举尺寸组合。

### 14.8 本轮我没能查清 / 需要你知道的

* **`feup` 的固件行为仍未测**（本批没有上机）：本批只把"能问"这件事修回来；
  写没写、固件留不留，要等 §14.7 那次跑。
* **"函数体没进"这条：穷举是主的，日志是辅的。** 事后用绝对区间查了内核日志（`/usr/bin/log show`，
  区间覆盖 0.43.9 那个 boot）：**整段 boot 里没有任何 `r4tx` 行**，而这个选择器一旦进了函数体
  每次会打约十条（同一份日志里 15:48:34 那次 0.43.8 的调用就是十行）。零行与"函数体没进"一致；
  但**日志投递失败也会长成这个样子**（`docs/M2B-NOTES.md` §48.5 记过同类困惑），所以这条只当旁证，
  判据仍以你那轮穷举为准。
* **同一段窗口里连 `user client opened` 都没有**（每次成功的 `IOServiceOpen` 都会打一条，
  `src/Cx3Mlx4.cpp:5654`），而这段时间确实有 root 活动（`sudo` 在 16:55–17:00 有大量条目）。
  两种读法都没被排除：**要么日志没投递这些行**，**要么这次拒绝发生在 user client 存在之前**——
  后者与 §14.3 的"拦截在传输层而不是派发表"是一致的。**这一点本轮没有查清**，写下来是为了
  下一次上机时别把"日志里没有"当成"没发生"。
* **拒绝到底发生在内核的哪一层，我没有拿到源码级定点**：读了 XNU 的
  `iokit/Kernel/IOUserClient.cpp`（`IOUserClient::externalMethod` 的派发分支），它比较的是
  **调用方声明的计数**与**我们声明的计数**（这里是 19 比 19，会通过），所以真正咬人的 16
  是**消息里那对内联标量数组的宽度**（`kIOExternalMethodScalar{Input,Output}CountMax` 就是它们的
  尺寸）。这条排除是有用的（它说明"加一条 dispatch 层的检查"救不了这个错误），但**具体在哪一行被拒
  没有查到**：XNU 里那段是 MIG 生成的代码，不在 `IOUserClient.cpp` 里。
* **`kmod_info.cpp` 里的版本字符串仍是 `0.20.0`**（`kext/Info.plist` 是 0.43.x）。实测
  `kmutil showloaded` 报的是 Info.plist 的版本（实测那时：本机 /Library/Extensions 的
  Info.plist 是 0.43.9，`kmutil showloaded` 也就报 0.43.9），所以 `verify.sh` 的
  "哪份 kext 在跑"不受影响；但这是同一事实的两个副本、其中一个是陈旧的，本批**没有动它**（超出范围）。
