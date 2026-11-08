任务：诊断一个**已验证的异常**。请判断哪个假设成立、设计能一锤定音的实验，并评估对下一阶段的
影响。这是一个网卡驱动移植项目。

## 项目

把 **BSD 许可**（OpenIB.org BSD 分支）的 Mellanox 驱动从 FreeBSD `sys/dev/mlx4` 移植到
macOS IOKit kext。硬件 ConnectX-3 Pro（`15b3:1007`，MCX354A-FCCT，双口 QSFP+ 以太网）。
已完成固件启动 / `INIT_HCA` / 14 张上下文表 / 端口拉成以太网并 link up（10 Gb/s）/
EQ 的轮询与中断两条路。现在在做的是"环形初始化"A/B 实验（M2B-NOTES §21-22）。

## 允许阅读的路径（只读这三处）

```
/Users/you/Documents/Hackintosh-MacLocal/tools/mlx4-port/freebsd-src/   # 参考实现
/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/            # 移植代码
/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/docs/M2B-NOTES.md
```

## 实验装置（移植代码里的 `Mlx4Hw::eqAbExperiment`，`src/Mlx4Hw.cpp`）

1. 从 `mttAlloc()` 拿 MTT 条目：这是一个 **bump 分配器**，`fMttHigh` 从
   `reserved_mtts`（本卡 **64**）开始向上发，每次发 `pages` 个连续条目；
   返回的"HCA 地址"是 `offset * 8`（`MLX4_MTT_ENTRY_SIZE`）。
2. 建两个**同规格**的环 A、B（512 槽 × 64 字节 EQE = 8 页 × 4 KiB），各自 `memset` 清零，
   各自 `mttWrite()` 把页总线地址（带 `MLX4_MTT_FLAG_PRESENT` 低位）写进自己的 MTT 条目。
3. 全环 32 KiB 各存一份基线快照，逐页查别名（全对全 + 环内重复 + MTT **区间**相交），
   任一命中就中止。
4. 只把**一个**环的 MTT 地址写进 EQ context 的 `mtt_base_addr`（byte `0x1b` 是
   `mtt_base_addr_h`，`0x1c` 起是 BE32 的 `mtt_base_addr_l`），
   用 `cmdBox(..., MLX4_CMD_SW2HW_EQ=0x13, op_modifier 0, in_modifier = EQN)` 提交。
5. 8 个真实时刻（`clock_get_uptime`）对**两个环**做全环逐字节差分（与基线比、与上一采样比）。
   不 `MAP_EQ`、不写门铃、不发事件。
6. 结束后 `HW2SW_EQ` 归还队列。

本卡实测的相关数字：MTT 表 **262144 字节 / 32768 条目**，已全部映射（一个 ICM chunk，
HCA 空间 **0x1000c0000**）；`reserved_mtts = 64`；`QUERY_FUNC` 报
`max_eq 156, reserved_eqs 28`；EQ 用 64 字节 EQE（`eqe_factor = 1`）、512 槽。

## 已经测到的（真机，全部可复现）

### 关键事实 1：结果跟着 **MTT 偏移**走，不跟着环身份、也不跟着 EQN

| # | 交出去的环 | MTT 条目偏移（字节地址） | EQN | 结果 |
|---|---|---|---|---|
| 1 | A | 64（0x200） | 28 | **整环被写**：32507~32510 / 32768 字节，全 8 页，512/512 槽 owner=1，图案见下 |
| 2 | B | 72（0x240） | 28 | **0 字节变化** |
| 3 | A | 64（0x200） | **29**（首次使用该 EQN） | **整环被写** |
| 4 | B | 72（0x240） | 29 | **0 字节变化** |
| 5 | **B（新周期里先交 B）** | 72（0x240） | 28（首次使用） | **0 字节变化** |
| 6 | **A（同一个环连交两次）** | 64（0x200） | 28（第 2 次使用） | **整环被写** |
| 7 | A | **72（0x240）**（用 `dummyMtt = 8` 先把 64..71 泄漏掉，A 因此落在 72） | 28 | **0 字节变化** |
| 8 | B | 80（0x280） | 28 | 0 字节变化 |

**第 7 行是最关键的一条**：与第 1 行相比，**同一个环身份 A、同一个 EQN 28、同一次开机、
同一段代码**，只有"环落在哪个 MTT 偏移"变了，结果就从"写"变成"不写"。
第 6 行则排除了"EQN 只能用一次"。第 5 行排除了"新周期里第一个交出去的会被写"。

### 关键事实 2：写进去的内容

环 A 前 64 字节（`SW2HW_EQ` 之后，与第一个采样点一样）：

```
0000: cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc
0020: ee ee ee ee ee ee ee ee ee ee ee ee ee ee ee ee ee ee ee ee ee ee ee ee 00 00 ee ee ee ed 96 ee
```

即每槽前 32 字节 `0xcc`、后 32 字节 `0xee`（`0xee` 的 bit7 置位 ⇒ 512/512 owner=1），
但槽内偏移 `0x38`/`0x39` 是 `00 00`、`0x3d` 是 `0xed`、`0x3e` 是 `0x96`。
全环 32768 字节里变化的字节数在两轮之间不同（32507 / 32510）。

### 关键事实 3：`QUERY_EQ` 的读回，有一处与我送出的不一致

每次 `SW2HW_EQ` 之后立刻发 `QUERY_EQ`（`0x15`，outbox），读回前 32 字节（8 个 BE32）：

```
交给 A（mtt_base 0x200）: 00000900 00000000 00000000 09000000 00000000 00000000 00000000 00000200
交给 A（mtt_base 0x240）: 00000900 00000000 00000000 09000000 00000000 00000000 00000040 00000240
```

按参考 `struct mlx4_eq_context`（`mlx4.h:323-342`）逐字节拆：
`flags`(0x00, BE32)=`0x00000900`（ARMED=9<<8 ✓）、`log_eq_size`(0x0c)=`0x09` ✓、
`intr`(0x17)=`0x00` ✓、`log_page_size`(0x18)=`0x00` ✓、
`mtt_base_addr_h`(0x1b) 与 `mtt_base_addr_l`(0x1c, BE32)。

**送出去时 `mtt_base_addr_h` 两次都是 `0x00`**（`mttAddr >> 32`，而 `mttAddr` 是 0x200/0x240）。
但读回：`0x200` 那次是 `0x00`（一致），**`0x240` 那次是 `0x40`（不一致）**。
`mtt_base_addr_l` 两次都与送出的一致（`0x00000200` / `0x00000240`）。

`HW2SW_EQ` 之后再发 `QUERY_EQ` 一律 `fw status 0x9`（不回答）。

### 已被我核对并**排除**的解释

- 别名：两环的页总线地址互不重叠（全对全比对）、MTT 条目区间不相交、环内无重复页；
  两环都 "8 pages, 3 segments, page aligned"。
- MTT 写入失败：`mttWrite` 每次成功，且事后从主机内存读回 MTT 条目，首/末条目与各自环的
  页总线地址**逐条匹配**（`entry 64 = 0x4676b6001 (ring page bus addr 0x4676b6001, match)`
  这种形式，两个环、每次运行都 match）。
- `IODMACommand` 构造参数错位：与 `IODMACommand.h:219-226` 的签名逐项对应
  （`kIODMACommandOutputHost64, 64, 0, kMapped, bytes, 1`）。
- 环大小/格式不一致、EQN 复用、reset 状态、新周期首轮、环身份 —— 都被上面的表格排除。

## 请你回答（逐条 A–E）

**A. 哪个假设最可能？** 现在的候选：

1. `mtt_base_addr` 必须是**某种对齐**的值。0x200 可用，0x240/0x280 不可用。
   请从允许阅读的源码里找依据：参考的 MTT 分配器（`mlx4_mr.c` 的 `__mlx4_alloc_mtt_range` /
   `mlx4_mtt_init` / `mlx4_mtt_addr`）、`mlx4_buddy`（注意 `mlx4_buddy.c` **可能不在**
   这个 vendored 集合里，若不在就明说）、以及 `MLX4_MTT_ENTRY_PER_SEG` 的用法，
   能不能推出某一个最小对齐/粒度要求？**注意**：参考的分配器看起来会产生 64、72、80…
   这种连续偏移，所以"必须 0x200 对齐"会与"参考在本卡 Linux 上正常工作"矛盾——
   请正面处理这个矛盾。
2. `mtt_base_addr` 期望的是**绝对 HCA 地址**（表基址 0x1000c0000 + 偏移），
   而我送的是偏移。请评估：这个假设能不能解释"0x200 成功、0x240 失败"？
   如果能，机理是什么（例如固件把送进来的值当成绝对地址后落在某段可写/不可写区域）；
   如果不能，明确说明为什么 0x200 与 0x240 在这种解释下的区别。
3. 固件**自己改写了** context（`0x1b` 从 0x00 变成 0x40）。请判断：
   `QUERY_EQ` 的 outbox 布局是否必然与 `SW2HW_EQ` 的 inbox 相同？参考里有没有
   `QUERY_EQ` 的解析代码（若没有，明确说"参考从不发这条命令，输出布局未经参考确证"）。
   `0x40`（= 64 = `reserved_mtts`）这个值是否可能不是 `mtt_base_addr_h`，而是别的字段——
   请按 `struct mlx4_eq_context` 的字节布局**逐字节**核对我上面对那两个 8 字的拆分是否有错。
4. 其他你想到的、能同时解释全部 8 行观察的假设。

**B. 一锤定音的实验。** 我现在有一个现成的输入 `dummyMtt`：先泄漏这么多 MTT 条目再建两个环，
于是环 A 的偏移 = `64 + dummyMtt`（B = `+8`）。每次运行需要一个新周期（reset + bring-up），
约 7 秒。请给出一个**最小运行集合**：

1. 每个点用哪个 `dummyMtt`（即 A 的条目偏移 = 字节地址 `(64+dummy)*8`）；
2. **每个点在每个假设下的预测**（写 / 不写），做成表；
3. 哪些点能区分哪些假设；哪些点即使不写也不提供信息（因为所有假设都预测不写）；
4. 如果你认为需要额外的输入（例如"按绝对地址形式送 mtt_base"），说明要加什么、
   以及为什么现有输入不够——我可以在下一次构建里加（每次构建要一次重启）。

请特别注意：条目偏移 → 字节地址是 `×8`，所以"偏移对齐"和"字节地址对齐"要分清；
给出你的预测时请同时写出**条目偏移**与**字节地址**，并说明你假设的对齐粒度是哪一个
（例如 32 条目 = 256 字节、64 条目 = 512 字节、2 的幂、页 4 KiB 等）。

**C. `0xcc`/`0xee` 图案与 `0xed`/`0x96`。** 参考里 RX CQ 的注释提到过 64 字节 CQE 前面的
32 字节"垃圾区"举例 `0xccccccc`（`mlx4_en/mlx4_en_rx.c:733-738`），EQ 这里对应的是
"前 32 字节保留、后 32 字节放 legacy EQE"（`mlx4_core/mlx4_eq.c:111-118`）。
请判断：把"每槽前半 `0xcc`、后半 `0xee`"解释成**固件的调试填充**是否站得住？
槽内 `0x38/0x39 = 00 00`、`0x3d = 0xed`、`0x3e = 0x96` 这几个非填充字节落在 EQE 的
哪个字段上（按 `struct mlx4_eqe`，`device.h:891` 附近）？它们是否可能是一个合法的
"空"EQE？**如果整个环都是"合法空 EQE"，那么 owner/phase 规则会把它判成什么**——
这对队列消费者的正确性意味着什么？

**D. 如果 A1（对齐）成立，对 R4 的影响。** R4 要建 CQ 环、QP 的 RQ/SQ 环等多条队列，
每条都要自己的 MTT 条目。请说明：如果固件真的对 `mtt_base_addr` 有对齐/粒度要求，
移植的 MTT 分配器应当怎么改（是改成按粒度对齐分配、还是改成每次都从表头开始、
还是别的）；并指出这条要求会不会让"参考在本卡上正常工作"变得不可能
（即：参考是否本来就会满足某种对齐，只是我读错了它的分配器）。

**E. 现在该不该继续 R4。** 这些环的 MTT 偏移问题会不会影响 R4 的正确性判断
（例如"ping 不通"到底是 steering 问题还是 MTT 问题）？请给出你的建议顺序：
先把 MTT 这条查清，还是可以并行推进 R4 的只读/上位部分。

回答用中文，**逐条 A–E**，区分"源码可确证"与"你的推断"，不确定写 "unknown"。
不要复述背景。
