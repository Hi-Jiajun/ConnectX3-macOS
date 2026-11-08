# R4 阶段 4（`r4RxQp`）实现记录：RX QP 三步到 RTS

本文是**实现记录**，实现的是 `docs/R4-STAGE4-PLAN.md`（方案已定稿，结论未改）。
方案里那两个待定项按用户拍板执行：QPN **按规则取值 + `QUERY_QP` 回读确认**；
`roce_entropy` **留 0，但把"设备宣称支持 RoCE V1/V2"写进日志**；`FCS_KEEP` 照抄；
`counter_index = 0xFF` 并在日志里说明。

**本阶段一行都没在硬件上跑过。** 每处的"实测/推断/未测"在第 6 节；kext 未安装、
机器未重启，验证留给用户重启后做一次。

---

## 1. 改动的文件（绝对路径）

| 文件 | 改了什么 |
| --- | --- |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/mlx4_regs.h` | 三个 QP opcode、QP context 的 21 个偏移常量、`QP_CONTEXT_MAILBOX_OFFSET = 8`、状态枚举与 `MLX4_QP_FLAGS_FIXED` / `MLX4_QP_BIT_FPP`、sched_queue 与 `MLX4_QP_PORT_ETH`、`MLX4_DEV_CAP_FLAG_FCS_KEEP (1ull<<34)`、`MLX4_QP_PARAM3_FCS_KEEP (1<<29)` |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Mlx4Hw.h` | 声明 `qpFillContext` / `qpModify` / `qpQuery` / `qpState` / `qpToReset` |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Mlx4Hw.cpp` | 上述五个函数的实现（新一节 "R4 stage 4: the RX QP"） |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/cx3_abi.h` | `kCx3SelR4Qp = 34`、`kCx3SelCount = 35`、`Cx3R4QpResult`（结构 + 12 个标量槽 + 字段掩码枚举）、`CX3_R4QP_NOT_MEASURED`、两条静态断言 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Cx3Mlx4.h` | `exportR4Qp`、`qpTeardown`、六个 `fR4Qp*` 成员 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Cx3Mlx4.cpp` | 成员初始化、`exportR4Qp`、`r4QpQueryAndCompare` / `r4QpPublishScalars`、`qpTeardown`、`rqTeardown` 的新守卫、两处 teardown 调用顺序 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Cx3Mlx4UserClient.h` / `.cpp` | 分发表第 35 行（selector 34）、锁类别表、`sR4Qp` / `doR4Qp` |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/tools/cx3ctl.c` | `cmdR4Qp`、`sizes` 表第 35 行、`r4qp` 子命令与 usage |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/tools/verify.sh` | 13 个新索引名解析 + `r4rq` 之后的链路步骤（三层各一条 PASS/FAIL） |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/kext/Info.plist` | 版本 0.43.1 → **0.43.2** |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/docs/COVERAGE.md` | `r4qp` 行（链内，证据栏写"从未运行"）、计数 20→21、重新同步版本号 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/docs/DEFECTS.md` | 新行 **D10**（第 4.5 节与第 5 节）、总数 51→52、D 家族 9→10 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/docs/R4-STAGE4-RESULT.md` | 本文件 |

未提交：工作区留着本次改动（`HEAD` 仍是 `46690f7`），回滚方法见第 7 节。

---

## 2. 判据的三层，以及每层用什么值读出来

方案 §7 要求三层都能**以值的形式**读出来。这三层失败时长得像、处置却不同，所以每层都有
自己的**标量输出**，而不是让人去解结构——理由和项目里"`fault` 的毒化位通过 `probe` 读、
不通过 `survey` 读"是同一个（harness 自己解 ABI 结构就是多一处会错的地方）：

| 层 | 标量 | 通过条件 | 为什么它算证据 |
| --- | --- | --- | --- |
| 0 前提 | `kR4QpOutStatus` | `0` | 选择器本身跑通了（不是"设备做了"） |
| 0 身份 | `kR4QpOutQpn` | 非 0 | 数字来自规则（固件保留份额），并被 `QUERY_QP` 确认 |
| 1 三命令 | `kR4QpOutStepsAccepted` | `3` | 三条状态转换各自 `fw status 0x0` |
| 1 每步状态 | `kR4QpOutStatesConfirmed` | `3` | **每一步之后**单独发 `QUERY_QP`，读回 `flags[31:28]` 等于刚写的那一步 |
| 2 最后一次回读 | `kR4QpOutQueryOk` / `QueryState` / `QpnConfirmed` / `FieldsMatch` | `1 / 3 / 1 / 1` | 同一次读同时回答"是不是我们写的那条 QP"（`local_qpn`）与"上下文字段有没有被固件改写"（8 个字段逐一比对：state、local_qpn、rq_size_stride、sq_size_stride、cqn_send、cqn_recv、db_rec_addr、mtu_msgmax） |
| 2 MTT 字段 | `kR4QpOutMttMatches` | `1` | `mtt_base_addr_h/l` 与 `log_page_size` **只属于 RST→INIT 那一步**、被后两步原样带着走，所以它单独报告而不并进上一条：这里不一致说明共享 context 在三步之间被改动了，那是另一种缺陷 |
| 3 生产者 | `kR4QpOutProducerValue` / `ProducerReadback` / `ProducerMatches` | 前两者相等且**非 0**、`Matches = 1` | 本阶段唯一的"交付动作"就是这 4 字节主机内存写，写完必须从同一块内存读回来 |

工具 `cx3ctl r4qp` 按这三层排版打印；`tools/verify.sh` 读的是标量，判 PASS/FAIL。
另外 `r4qp` 还会打印固件**自己**对 `param3` 与 `sched_queue` 的看法（第 4.5 节说明为什么这两条要双向往回看）。

---

## 3. 编码：照抄的那些值与来源

来源全部是 vendored 参考（`vendor/freebsd-src/`）。行号已逐条核对（第 5 节列了**方案里行号偏了**的四处；以下是核对后的行号）。

| 项 | 值 | 依据（核对后） |
| --- | --- | --- |
| opcode | `RST2INIT_QP=0x19`、`INIT2RTR_QP=0x1a`、`RTR2RTS_QP=0x1b`、`2RST_QP=0x21`、`QUERY_QP=0x22` | `cmd.h:108-118` |
| `in_param` | **command mailbox 的 DMA 地址** | `mlx4_qp.c:183` |
| `in_modifier` | `qpn \| (sqd_event<<31)`，本项目 `sqd_event = 0` ⇒ **就是 qpn** | `mlx4_qp.c:184` |
| `op_mod` | 三步都是 **0**（`new_state == RST ? 2 : 0`） | `mlx4_qp.c:185` |
| 超时 | 三步 `TIME_CLASS_C`；`2RST` 走另一条分支的 `TIME_CLASS_A`（`mlx4_qp.c:145-147`）；`QUERY_QP` 是 `TIME_CLASS_A`（`mlx4_qp.c:889`） | 同上 |
| mailbox 布局 | `+0` = `optpar`（本项目 0）；**`+8` = QP context（248 字节）**；`QUERY_QP` 响应也在 `+8` | `mlx4_qp.c:177-178`、`:892` |
| MTT 与页大小 | **只在 RST→INIT 那一步写**：`mtt_base_addr_h`(0xcb)、`mtt_base_addr_l`(0xcc)、`log_page_size`(0xc8 = 0) | `mlx4_qp.c:164-169` |
| 状态位 | `flags[31:28]` = 1/2/3 | `mlx4_qp.c:912-914`、`qp.h:68-71` |
| `params2` 的 FPP | 除 RTR 外清掉；本项目 context 起始为 0，所以是 no-op（代码两处注释写明，不留"漏了一行"的疑问） | `mlx4_qp.c:915-916` |
| `flags` 固定位 | `7 << 16` | `mlx4_en_resources.c:49` |
| `mtu_msgmax` | `0xff` | `mlx4_en_resources.c:51` |
| `rq_size_stride` | `(ilog2(256)<<3) \| (ilog2(16)-4)` = **`0x40`** | `mlx4_en_resources.c:53` |
| `sq_size_stride` | `ilog2(64)-4` = **`0x02`**（dummy SQ 是 TXBB_SIZE = 64） | `mlx4_en_resources.c:57`、`en.h:84` |
| `usr_page` | **128**，即 CQ 用的那个 UAR 索引（参考里一个 netdev 只有一个 `priv_uar`） | `mlx4_en_resources.c:58`；本机实测见方案 §3 |
| `pri_path.counter_index` | **`0xFF`** = 参考自己表达"无 counter"的方式（下一行的守卫就写成 `counter_index != 0xFF`） | `mlx4_en_resources.c:66`、`:69` |
| `pri_path.ackto` | `1 & 0x07` | `mlx4_en_resources.c:60` |
| `pri_path.sched_queue` | `0x83 \| (port-1)<<6` ⇒ port 1 = **`0x83`** | `mlx4_en_resources.c:61` |
| `cqn_send` / `cqn_recv` | 同一个 RX CQN | `mlx4_en_resources.c:76-77` |
| `db_rec_addr` | **`wqres.db.dma`，不左移**——`:78` 的 `<<2` 是 steering QP 的，被显式覆盖 | `mlx4_en_rx.c:947` |
| `param3` | `caps.flags & (1LL<<34)` 为真 ⇒ `\|= 1<<29`（保 FCS） | `mlx4_en_rx.c:950-951`、`device.h:177` |
| 生产者发布 | `*ring->wqres.db.db = cpu_to_be32(ring->prod & 0xffff)`，**主机内存写**，16 位 | `mlx4_en_rx.c:253` |

---

## 4. 实现里值得记住的五件事

### 4.1 mailbox `+0` 还是 `+8`：本阶段第一大坑，现在有守卫

CQ context 在 `+0`（`mlx4_cq.c:151`），QP context 在 `+8`（`mlx4_qp.c:177-178`）。
本移植把 `+8` 命名为 `QP_CONTEXT_MAILBOX_OFFSET`，`qpModify` 是**唯一**写 mailbox 的地方、
每次日志都打印它；三步里任何一步失败时，日志直接点名这条：*"A BAD_PARAM (0x3) on the
first step is this stage's signature mistake: the QP context belongs at mailbox +8, not +0
where the CQ context goes."* 这是唯一一个能被日志直接带向根因的分岔。

### 4.2 一个 context、三步，只有四位不同

`qpFillContext` 一次填满 248 字节；三步各自只改 `flags[31:28]`。**MTT 与 `log_page_size`
是在 RST→INIT 之前单独写进同一个镜像的**，后两步只是把它带着走——所以"复用一个 context"
这件事本身可验证：回读里的 `MttMatches` 就是在验它。

### 4.3 每一步之后都回读，而不是只在最后读一次

每条命令之后立刻一次 `QUERY_QP`。理由写在代码注释里：命令成功不等于状态被接受，而
**中间某一步被悄悄丢掉、最后一样能读到 RTS**（后面的命令可以把它一路带到 RTS）。所以
`stepsAccepted` 与 `statesConfirmed` 是两个独立的数，两个都必须是 3。

### 4.4 QPN：数字来自规则，正确性来自回读

规则沿用 CQN 那条同形的：起点是**固件保留份额** `fDevCap.reserved_qps`（本卡实测
`reserved_qps = 64`、`log_num_qps = 10` ⇒ 可用 64..1023，`docs/M2B-NOTES.md:1750`），
上界是**本 profile 配置的** `1 << log_num_qps`（不是卡的能力上限——阶段 2 已经记过：
能力上限不等于配置）。低于保留份额、或达到/超过配置上界一律拒绝。

然后 `QUERY_QP` 回读 `local_qpn`，与写入值比对（`kR4QpOutQpnConfirmed`）。两个事实都
报出来，所以"规则算出了一个数"不会冒充"硬件里就是这条 QP"。

### 4.5 生产者记录在阶段 4 新建；`param3` / `sched_queue` 双向往回看

阶段 3 只建描述符，它没有理由知道会有一条 QP 需要一个生产者记录；而 `db_rec_addr` 指向的
正是那块主机内存，所以记录在阶段 4 分配（一整页，照参考的 doorbell 记录分配器
`mlx4_alloc.c:731-758`）。分配后**先清、先读一遍**（`producerBefore`），再写
`prod = 256`（= 阶段 3 的 WQE 数），再读回来：两次读都是测量。

`param3`（FCS_KEEP）与 `sched_queue`（port）这两条**工具同时打印"我们写的"和"固件持有的"**。
起因是一次真实的自查：幂等路径（第二次调用）只重测能重测的东西，若照抄 "sent" 字段，
工具会打印 `param3 0x00000000 … bit 29 clear`，而这次调用实际发的是 `0x20000000` ——
**工具自相矛盾**，正是 `D8` / `F8` 那一类。现在幂等路径把这两条**重新推导**（它们是
"设备能力位 + 本阶段固定端口"的函数）并明确说明是推导而非重放，固件那一侧的值仍旧来自
`QUERY_QP`。这个自查记进了 `docs/DEFECTS.md` 的 **D10**。

---

## 5. 方案里发现的问题（结论未变，四处行号偏了）

`docs/R4-STAGE4-PLAN.md` 的**判断全部成立**，逐条实现时没有发现逻辑错误。只有四处
**行号偏移**，实现里用的是核对后的行号：

| 方案写的 | 实际 | 差 |
| --- | --- | --- |
| 只有 RST→INIT 设置 MTT/页大小：`mlx4_qp.c:170-175` | **`164-169`**（`170-175` 是紧随其后的 roce_entropy 块） | 6 行 |
| 三步循环 `mlx4_qp.c:894-924` | `mlx4_qp_to_ready` 是 **`899-930`**，循环是 `912-926` | 5 行 |
| RTR→RTS 设 `roce_entropy`：`mlx4_qp.c:172-176` | **`171-175`** | 1 行 |
| `mlx4_cq.c:274-276`（logsize_usrpage 里的 UAR 换算） | **`275-277`** | 1 行 |

另外核对后确认（这不是偏差，是补强）：`2RST_QP` 的"进 reset"分支在 `mlx4_qp.c:145-147`，
`in_param 0` + `op_modifier 2` + `TIME_CLASS_A`，**完全不带 mailbox**。所以 `qpToReset`
是独立函数，而 `qpModify` 见到 `2RST_QP` 直接拒绝并指向它——那条路径要是走了"带 context
的四位状态变更"的写法，就会把一个固件不期待的 mailbox 递上去。

---

## 6. 实测 / 推断 / 未测

### 实测（本轮真的跑出来的）

* **构建门禁全绿**：`boot-path` PASS（`start()` 可达函数集 19 个，无写、无 DMA、无 reset）·
  `146 + 8186 + 137 + 45` 项检查 0 失败 · 未定义符号 **352/352 全部解析**（与基线 352 相同：
  新增的是内部方法，没有引入新的外部符号）· 四个 `-Werror` 档下零告警 ·
  `tools/check-hardcoded.py --strict` 通过（8186 项，0 失败）。
* **kext sha256 = `5e73ee5ef8855895b9b0cae056b917a9001fa03284af1b160188af9c369ca9b7`**，
  277856 字节（`BUILD_TAG=0.43.2`）。**可复现**：同一份树**连续四次构建得到同一 sha**。
  顺带观察到一条，值得写下来：只改注释（含注释里的行号）时二进制逐字节不变，改到 `LOG`
  的字符串常量才变——所以这一轮 sha 的变化全部来自真正进了 `.rodata` 的日志文本，
  而不是"注释改了所以二进制变了"。
* `cx3ctl sizes` 自证：`34 1 1 272 r4qp`（1 标量入、1 标量出、结构 272 字节），而这行是
  **编译器算的**（`sizeof(Cx3R4QpResult)`），与 kext 的分发表同源。
* `verify.sh` 的 13 个新索引名在 `src/cx3_abi.h` 里全部解析成功（逐个用 `abi()` 的正则验过）。
* 静态断言：分发表与锁类别表的长度（`== kCx3SelCount = 35`）编译期通过。

### 推断（有源码依据，未上机）

* 三条命令的编码、context 字段值、`+8` 布局：逐条照抄参考，行号见第 3 节。
* "发布生产者 = 4 字节主机内存写，不是 MMIO 门铃"由 `mlx4_en_rx.c:253` 直接得出；
  **本卡的 HCA 是否真的会去读它，只有阶段 9 能观测**。
* `FCS_KEEP = 1`：本任务收到的是上层代理 2026-10-12 的实测结果（`rawjson 8 in 1,0 out 2
  struct 272`，从 `structHex` 的 `0x10+0x40/0x44` 读出 `ext_flags 0xeeff1f9d` /
  `flags 0xf677fbef` ⇒ `caps.flags` bit 34 = 1），与 `bmme_flags 0x3d5b8ec2` 这个高熵值
  精确匹配、并与 `mtt_entry_sz = 8` 交叉一致。**我本人本轮没有独立复核这个值**（复核需要
  重启后 `QUERY_FW` + `fwload` 完成之后的 `QUERY_DEV_CAP`）；方案 §8 里 05:20 的内核日志
  也已独立支持它。写进 context 的做法是照参考的 `if` 分支——设备说 0 就写 0，不是硬编码 1。
* `usr_page = 128`、`uar_page_sz = 0`、阶段 1–3 全部 PASS、MTT 索引 64 / `mtt_entry_sz = 8`：
  都来自方案 §8 记的实测（同一轮工作、同一个 boot），本轮未重测。
* 硬件在 RTS 之前是否还要求别的字段：参考的 RX 路径只填这些，我按参考写。若 RTS 到不了，
  第一个要查的是 `flags[31:28]` 有没有被覆盖、以及 FPP 位。

### 未测（明确没做，或做不到）

* **`r4qp` 从未运行**：kext 未安装、机器未重启（本轮硬约束）。QP 是否真的到 RTS、三条命令
  是否真的 `fw status 0x0`、生产者是否真的能被 HCA 读到，**全部未测**。
  `docs/COVERAGE.md` 里 `r4qp` 那行的证据栏就写着"从未运行"。
* `tools/verify.sh run|full|safe` 未跑（会驱动硬件）。
* **`cx3ctl r4qp` 的排版只在编译层面验过**，没有对着真实的 `Cx3R4QpResult` 打印过一遍。
  第 8 节的失败分岔文字是设计意图，不是实测过的判读路径。
* 幂等路径（第二次调用 `r4qp`）：代码写了、编译过了，但没跑过——它要一条已经到 RTS 的 QP
  才谈得上。
* `qpTeardown` / `rqTeardown` 的新守卫未在硬件上走过（只在 `stop()` 与 reset 路径上编译通过）；
  **回滚路径未实测**。
* 我没能查清的点：
  1. `db_rec_addr` 指向的记录，**HCA 到底在什么时刻读**（初始化读一次？每消费一个 WQE 读一次？）。
     参考只有那一行写，行为未记录；这决定"生产者在 RTS 之后发布"是否够早。
  2. `usr_page` 填 128 时，HCA 对**同一个 UAR 页被 CQ 与 QP 共用**是否有额外要求。参考里一个
     netdev 只有一个 `priv_uar`，共用是正常形态，但本卡没在 QP 上验过。
  3. `mlx4_qp.c:890` 的 `MLX4_CMD_WRAPPED` 旗标在本移植里没有对应物——所有命令都走同一个
     `cmdBox`，`hcrRun` 里也没有 wrapped 概念。我按"无差别"处理；**若三步全成功而
     `QUERY_QP` 回 `BAD_PARAM`（0x3），这是要回来看的第一条**。

---

## 7. 回滚

阶段 4 是**加法**：一个新选择器 + 新命令常量 + 一个新 teardown 分支，不改既有路径的语义。
唯一动到既有行为的是 `rqTeardown`：有 QP 在时它现在**拒绝**释放 ring——而这正是阶段 3
自己的注释预告过的那个分支（"Once stage 4 enables the WQEs this changes … and this function
will need that branch"）。

1. **代码回滚**（本轮未提交，工作区是 `HEAD = 46690f7` 之上的改动）：

   ```bash
   git -C /Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4 status --short
   git -C /Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4 diff      # 先看
   git -C /Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4 checkout -- .   # 放弃本次改动
   ```

   若已按项目惯例提交成一条提交，则用方案 §9 的原话：
   `git -C tools/cx3-mlx4 revert --no-edit <该提交>`，然后重建、重装、重启。
   构建产物这一轮都写在 `/tmp` 下的临时目录里，**没有覆盖 `outputs/20261010-cx3-m1/build`**，
   所以仓库里没有需要清理的中间产物。

2. **运行期退路**：`sudo tools/cx3-mlx4/rollback.sh [--purge]`（未改动）。

3. **硬件侧残留**：阶段 4 会在设备里留下一条 **RTS 状态的 QP**；回滚后它仍在，直到 HCA reset
   （`m2dbringup` 之前的 `reset` 会清掉它，这也是验证链条本来就做的事）。生产者记录是本 driver
   的主机内存，随 kext 卸载释放。注意顺序：**先 `qpTeardown`（2RST）再释放 ring**，否则 HCA
   还在通过 context 里的 MTT 翻译那块 ring。

---

## 8. 用户重启后要跑的那条命令与期望输出

按项目惯例安装，然后跑一次：

```bash
sudo tools/cx3-mlx4/verify.sh stage      # 构建 + 校验 + 安装（不重启）
sudo reboot
sudo tools/cx3-mlx4/verify.sh run        # 重启后：整条链，含新的 r4qp
```

只想单独看阶段 4（前提是同一 boot 里已经跑过 `m2dbringup`、`m3equp`、`m3clint`、`m3portup`、
`r4mr`、`r4cq`、`r4rq`）：

```bash
sudo /usr/local/bin/cx3ctl r4qp          # 不带参数 ⇒ QPN 走规则
```

**期望**（阶段 4 那一段）：

```
  QPN               : 64   (rule: the firmware's reserved share 64; the profile
                            configured 1024 QPs)
  local_qpn readback: 64  <- the command was given this QP and the firmware agrees
  layer 1: the three transitions, each followed by its own QUERY_QP
    RST2INIT (0x19): fw status 0x0 ok ;  state after: 1 (wanted 1) ok
    INIT2RTR (0x1a): fw status 0x0 ok ;  state after: 2 (wanted 2) ok
    RTR2RTS  (0x1b): fw status 0x0 ok ;  state after: 3 (wanted 3) ok
    commands accepted 3/3, states confirmed by readback 3/3
  layer 2: the last QUERY_QP against what was written (written / read)
    all eight compared fields : MATCH (mask 0xff)
    the MTT address survived  : yes
  layer 3: the RQ producer -- a 16-bit write into HOST MEMORY, not a doorbell
    value written      : 256  (256 WQEs, one buffer per WQE from stage 3)
    read back          : 256  (it held 0 before the write) MATCH
    param3            : written 0x20000000, read back out of the context 0x20000000
    sched_queue       : written 0x83, read back 0x83
```

`verify.sh run` 里对应的是这四行 PASS：

```
QP: QPN 64, the rule's value, confirmed by QUERY_QP
QP layer 1: 3/3 commands accepted, 3/3 states read back after each step
QP layer 2: RTS, local_qpn and all eight compared fields match the write
QP layer 3: the RQ producer 256 is in host memory, read back as 256
```

**失败时的分岔判读**（按方案 §7 的"失败先查自己"）：

| 症状 | 先查什么 |
| --- | --- |
| 第一步就 `fw status 0x3`（BAD_PARAM） | **mailbox 偏移**：context 必须在 `+8`。`qpModify` 的日志会打印它用的偏移和 context 里的 flags，直接对上 |
| 某步 `fw status` 是别的非 0 | 那一步的 opcode 与 state（日志逐行给）。`0x4` BAD_SYS_STATE ⇒ HCA 没起来；`0x1` INTERNAL_ERR ⇒ 先看是不是 reset 之后没重跑 bring-up |
| 命令都成功、`statesConfirmed < 3` | **`QUERY_QP` 自己没答**（`QueryOk != 1`）与"答了但不是那个状态"是两件事：前者查 `QUERY_QP` 的编码（本记录第 6 节"未测"第 3 条），后者查 `flags[31:28]` 在三步之间是否被覆写、以及 `params2` 的 FPP 位 |
| 状态对了、`FieldsMatch = 0` | 日志会点名是哪个字段（written/read 并排）。`rq_size_stride` 应为 `0x40`、`sq_size_stride` 应为 `0x02`、`db_rec_addr` 应为记录地址且**不左移** |
| `MttMatches = 0` | MTT 地址只在 RST→INIT 写、被后两步带着走；不一致说明共享 context 被扰动（幂等/并发） |
| 生产者 `MISMATCH` | 记录地址与 `db_rec_addr` 是不是同一块；这次调用会把 QP 退回 RST（日志写明）。退回也失败则 QP 与记录都故意保留到重启 |
| 以上全绿但阶段 9/10 收不到帧 | 回到决策②：`roce_entropy`（本阶段刻意留 0）、FCS（本卡为 1 ⇒ 帧里带 FCS，多 4 字节必须减掉）、`counter_index = 0xFF`。方案第 6 节已经把这条写成了证伪条件 |

---

## 9. 重启后的实测结果（2026-10-12 06:19，0.43.2 真机）

**四层全绿，`verify.sh run` 得到 `34 PASS / 0 FAIL / 1 BLOCKED`**（BLOCKED 仍是 M4 的 ping 闸门）。

```
QP: QPN 64, the rule's value, confirmed by QUERY_QP  PASS
QP layer 1: 3/3 commands accepted, 3/3 states read back after each step PASS
QP layer 2: RTS, local_qpn and all eight compared fields match the write PASS
QP layer 3: the RQ producer 256 is in host memory, read back as 256 PASS
```

内核日志（`outputs/verify-20261012-061937.log` 对应的那次运行）逐条对上设计意图：

```
r4qp: QPN 64 (rule: the firmware's reserved share 64, profile ceiling 1024), context at
      mailbox +8, 248 bytes; the ring's MTT: entry 80 -> byte address 0x280 (index x 8),
      log_page_size 0 (set for RST->INIT only); producer record at 0x3b77f8000
r4qp: step 1 accepted (fw status 0x0); QUERY_QP says state 1, local_qpn 64
r4qp: step 2 accepted (fw status 0x0); QUERY_QP says state 2, local_qpn 64
r4qp: step 3 accepted (fw status 0x0); QUERY_QP says state 3, local_qpn 64
r4qp: QUERY_QP QPN 64: fw status 0x0; state 3 (flags 0x30070000), local_qpn 64,
      rq_size_stride 0x40, sq_size_stride 0x02, mtu_msgmax 0xff, cqn_send 128,
      cqn_recv 128, db_rec_addr 0x3b77f8000, mtt_base_addr_l 0x00000280, log_page_size 0
r4qp: RQ producer published: 256 (0x100) ... Read back out of host memory: 256 (matches)
```

**这三条是这一阶段真正要的证据**：`0x40`/`0x02` 的 stride 编码对上了（§3 的两条"最容易错"之一）、
MQTT 地址 `0x280 = 索引 80 × 8` 在两步之间**survived**、以及生产者是**主机内存里的 256**。
`flags 0x30070000` 的高半字节 `3` 正是 RTS。

### 9.1 ★ 一个必须带进阶段 9 的分歧：两个字段固件**没有回存**

同一份日志里，紧接着那行成功之后是：

```
r4qp: two fields the firmware holds differ from what this driver writes:
      param3 0x20000000 written / 0x00000000 read back, sched_queue 0x83 / 0x82
```

| 字段 | 写入 | 固件回读 | 差异 |
| --- | --- | --- | --- |
| `param3` | `0x20000000`（bit 29 = keep FCS） | `0x00000000` | bit 29 被丢弃 |
| `sched_queue` | `0x83` | `0x82` | bit 0 被清 |

**为什么这一行存在却不判 FAIL**：参考驱动**从不回读**这两个字段（全树 `grep` 只有写、没有读），
所以没有"应当等于什么"的参照；上下文里固件对自己解释的字段做归一化是常见形态，
而 layer 2 比较的那 8 个字段是参考语义要求必须保住的（尺寸、CQN、地址、QPN）。
把"没有参照的量"判成 FAIL，会把未知报成已知——那是本项目代价最大的一类错误。

**但它对阶段 9/10 有实质影响，所以记在这里而不是埋在日志里**：

* 本卡宣称 `FCS_KEEP` 为 1（§10 of `R4-STAGE4-PLAN.md`，两次实测复现），而我们写进 `param3` 的
  bit 29 固件没回存。**参考并不依赖这个回读**：它的 `ring->fcs_del = ETH_FCS_LEN` 来自
  `caps.flags & MLX4_DEV_CAP_FLAG_FCS_KEEP`（`mlx4_en_rx.c:949-953`），也就是**能力位**，
  和我们的 `fcsKeep` 是同一个输入。所以阶段 9/10 应当**照参考用能力位决定是否减 4 字节 FCS**，
  而不是去看 `param3` 的回读——这一点现在有了明确依据。
* `sched_queue` 的 bit 0 被清是否影响调度（进而影响帧是否送到这个 QP），**本阶段无法判定**。
  若阶段 9 收不到帧，这是要查的一条（和 `roce_entropy`/`counter_index` 并列）。

**汇总行的一个措辞缺口，已修**：原来那行说 "all eight compared fields match the write"，
只说对了 8 个字段，对"另有 2 个字段不一致"完全沉默——只看汇总的人不会知道 §9.1 这件事。
这正是本项目的 F9 形态（**记录本身是一个没人核验过的断言**）。现在 `verify.sh` 的那一行
会点名这两个字段是被**单独比较**的，并指明去哪里看值。

**下一步的候选（未做，属范围外）**：把这两个字段的相等性做成 ABI 里的两个标量，
让验证器能**数值化**地断言它——现在它们是"打印但不判定"，机器读不到。
那需要一次 kext 改动 + 重启，因此与阶段 5 合并做更划算。
