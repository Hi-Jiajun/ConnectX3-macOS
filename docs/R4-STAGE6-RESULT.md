# R4 阶段 6（`r4Tx`）实现记录：TX CQ + TX QP + SQ（**建立资源，不发帧**）

本文是**实现记录**，实现的是 `docs/M2B-NOTES.md` §27.2 的第 6 行（`r4Tx`：**资源建立即可，
此时还不算"线上发送成功"**）。判据沿用阶段 4 的三层做法（命令成功 / 每步回读 / 最终字段逐项比对），
只是换成了发送侧。

**本阶段一行都没在硬件上跑过。** kext 未安装、机器未重启，所以本文的每一个设备侧结论都是
**推断**（有源码依据），"是什么就该看到什么"在第 9 节，"没测"在第 7 节。

一句话结论：**发送侧的三个对象建好了**——一条 TX CQ（`SW2HW_CQ` 接受且空环读作空）、一条
send QP 走到 RTS（三步各自回读确认）、一块 SQ 内存（dummy TXBB + 256 个 64 字节 TXBB + MTT）。
**没有投递任何 WQE、没有敲任何门铃**，所以这**不是**"能发帧了"——发帧是阶段 9。

---

## 1. 改动的文件（绝对路径）

| 文件 | 改了什么 |
| --- | --- |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/mlx4_regs.h` | 新一节 "R4 stage 6"：`MLX4_TXBB_SIZE`(64)、`MLX4_STAMP_SHIFT`(31)、`MLX4_SQ_DUMMY_STAMP`(`1<<31`)、`SQ_RING_HEADER_SIZE`(64)，并在注释里写清"参考的 TX 环没有这个头" |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Mlx4Hw.h` | `qpFillContext` 增加 `isTx`（并把 `rqEntries/rqStride` 改名为参考的 `size/stride`）；声明 `sqCreate` |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Mlx4Hw.cpp` | `qpFillContext` 按 `mlx4_en_resources.c:52-57` 的三行实现两个 size/stride 字节与 param3 的 RX/TX 不对称（含两行新日志）；新函数 `sqCreate`（dummy TXBB + WQE 数组 + MTT，并把 stamp 与"WQE 数组非零字节数"读回来） |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/cx3_abi.h` | `kCx3SelR4Tx = 36`、`kCx3SelCount = 37`、`Cx3R4TxResult`、字段掩码枚举、2 个入参 + **16** 个标量槽（`CX3_SCALAR_OUT_MAX` 就是 16，正好占满）、`CX3_R4TX_NOT_MEASURED`、两条默认值、三条静态断言 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Cx3Mlx4.h` | `exportR4Tx`、`txTeardown`、18 个新成员 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Cx3Mlx4.cpp` | 成员初始化、`txTeardown`、`r4TxQueryAndCompare` / `r4TxPublishScalars`、`exportR4Tx`（**放在文件末尾**，新一节）、三条 teardown 路径的插入（reset / `stop()` / 重做 profile）、两处既有 `qpFillContext` 调用各补一个 `isTx=false` |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Cx3Mlx4UserClient.h` / `.cpp` | 分发表第 37 行（selector 36）、锁类别表、`sR4Tx` / `doR4Tx` |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/tools/cx3ctl.c` | `cmdR4Tx`、`sizes` 表第 37 行、`r4tx` 子命令与 usage |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/tools/verify.sh` | 17 个新索引名解析 + `r4ingress` 之后的链路步骤（4 条 PASS/FAIL，外加两处 `echo` 说明） |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/kext/Info.plist` | 版本 0.43.4 → **0.43.5** |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/docs/COVERAGE.md` | `r4tx` 行（链内，证据栏写 **未运行**）、计数 22→23、版本号同步到 0.43.5 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/docs/DEFECTS.md` | 新行 **D13**、总数 55→56、D 家族 12→13 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/docs/R4-STAGE6-RESULT.md` | 本文件 |

---

## 2. 判据的四层，以及每层用什么值读出来

方案 §27.2 与本任务的判据表都要求"能从值读出来"，所以每一层都有自己的**标量**，
`tools/verify.sh` 读标量判 PASS/FAIL，不去解结构：

| 层 | 标量 | 通过条件 | 为什么它算证据 |
| --- | --- | --- | --- |
| 0 前提 | `kR4TxOutStatus` | `0` | 选择器本身跑通了（不是"设备做了"） |
| 1 TX CQ | `kR4TxOutCqFwStatus` | `0` | `SW2HW_CQ` 被接受——这是 CQ 存在的唯一开关 |
| 1 TX CQ（空环） | `kR4TxOutCqReady` | `0` | 消费者自己的就绪规则在**刚建好的环**上找到 0 个可读槽；非 0 意味着"从没人写过的内存里读出完成事件" |
| 2 编号 | `kR4TxOutNumberSafe` | `1` | 一条比特压住四个比较：两个编号都在**顶部保留线**之下，且不与 RX QP / 入口 QP / RX CQN 相撞（四个比较各自也报，见结构体） |
| 2 编号（基准值） | `kR4TxOutTopReserved` | = `num_qps - (1 << log_max_macs)` | 上面那条比较的**参照物**——它必须一起报出来，否则"在安全区"就是一个没有参照物的断言。两个输入都是运行时读的：profile 的 `log_num_qps` 与 `QUERY_PORT` 的 `log_max_macs` |
| 3 三步命令 | `kR4TxOutStepsAccepted` | `3` | 三条状态转换各自 `fw status 0x0` |
| 3 每步状态 | `kR4TxOutStatesConfirmed` | `3` | **每一步之后**单独发 `QUERY_QP`，读回 `flags[31:28]` 等于刚写的那一步 |
| 4 最后一次回读 | `kR4TxOutQueryOk` / `QueryState` / `QpnConfirmed` / `FieldsMatch` | `1 / 3 / 1 / 1` | 同一次读同时回答"是不是我们写的那条 QP"（`local_qpn`）与"上下文字段有没有被固件改写"（8 个字段逐一比对：state、local_qpn、`rq_size_stride`、`sq_size_stride`、`mtu_msgmax`、`cqn_send`、`cqn_recv`、`db_rec_addr`） |
| 4 MTT | `kR4TxOutMttMatches` | `1` | MTT 与 `log_page_size` **只属于 RST→INIT 那一步**、被后两步原样带着走，所以单独报：不一致说明共享 context 在三步之间被扰动 |
| 5 SQ 签名字节 | `kR4TxOutSqSizeStride` | `0x42`（默认 256 × 64 ⇒ 十进制 66） | 阶段 6 的名字所在：`ilog2(256) << 3 \| (ilog2(64) - 4)`。这个字节错了别处全是静默的 |
| 5 SQ dummy stamp | `kR4TxOutSqStampMatches` | `1` | dummy TXBB 的首 dword 从**内存里读回来**等于 `1 << 31`——"这个 64 字节不是描述符"是写进去的，也是量出来的 |

**工具打印但不当判据的**（结构体里有、标量里没有）：SQ 的字节数/页数/MTT 偏移/首个物理页、
WQE 数组的非零字节数（应为 0，即"本阶段什么都没投递"）、`param3` 与 `sched_queue` 的
written/read 两值。最后两条与阶段 4 同形：参考从不回读它们，所以没有参照物，**报告而不判定**。

> **标量槽刚好用满 16 个**：`CX3_SCALAR_OUT_MAX` 是 16（`src/cx3_abi.h:362`），本阶段用了
> 0..15 全满。再加一条判据就必须先合并两条或去掉一条——这件事写在这里，免得下一个人以为
> "再加一个很便宜"。

---

## 3. 编码：照抄的那些值与来源（行号已逐条核对）

来源全部是 vendored 参考（`vendor/freebsd-src/`）。

| 项 | 值 | 依据（核对后） |
| --- | --- | --- |
| opcode | `RST2INIT_QP=0x19`、`INIT2RTR_QP=0x1a`、`RTR2RTS_QP=0x1b`、`2RST_QP=0x21`、`QUERY_QP=0x22` | `cmd.h:108-118`；`in_param` = mailbox DMA、`in_modifier` = qpn、`op_mod` = 0（`mlx4_qp.c:177-186`） |
| mailbox 布局 | `+0` = `optpar`(0)；**`+8` = 248 字节 QP context** | `mlx4_qp.c:177-178`、`:180-181` |
| `size`/`stride` 的含义随 `is_tx` 变 | 一对参数、两种含义 | `mlx4_en_resources.c:46` 的签名；TX 的调用点传的是**本环自己的** `ring->size, ring->stride`（`mlx4_en_tx.c:233-234`），而 `stride` 在 `mlx4_en_netdev.c:1717-1718` 就是 `TXBB_SIZE` |
| `rq_size_stride` | TX **留 0**：`if (!is_tx && !rss)` | `mlx4_en_resources.c:52-53` |
| `sq_size_stride` | TX = `ilog2(size) << 3 \| (ilog2(stride) - 4)`；256 × 64 ⇒ **`0x42`** | `mlx4_en_resources.c:54-55`；RX 侧是 dummy `ilog2(64) - 4 = 0x02`（`:56-57`） |
| `param3` 的 FCS 位 | TX **不设**；`1 << 29` 只出现在 RX 路径 | `mlx4_en_rx.c:950-951`（**不在** `mlx4_en_fill_qp_context` 里，所以发送侧天然为 0） |
| `usr_page` | 运行时 UAR 索引（**与 RX CQ 同一个**） | `mlx4_en_resources.c:58`；参考一个 netdev 只有一个 `priv_uar` |
| `cqn_send` / `cqn_recv` | 都是 **TX CQN** | `mlx4_en_resources.c:76-77` |
| `db_rec_addr` | **不左移**的自有 record 地址（与阶段 4 同；参考的 `<<2` 见 §5.2） | 阶段 4 依据 `mlx4_en_rx.c:947`；本任务硬指定同形 |
| `counter_index` | `0xFF` = 参考自己表达的"无 counter" | `mlx4_en_resources.c:66`，下一行守卫写作 `!= 0xFF` |
| `ackto` / `sched_queue` | `1 & 0x07` / `0x83 \| (port-1)<<6` ⇒ port 1 = `0x83` | `mlx4_en_resources.c:60`、`:61` |
| MTT 与页大小 | **只在 RST→INIT 那一步写**；`mtt_base_addr_h`(0xcb)、`_l`(0xcc)、`log_page_size`(0xc8 = 0) | `mlx4_qp.c:164-169` |
| 三步的上下文差异 | 只有 `flags[31:28]` | `mlx4_qp.c:912-914`；状态值 `qp.h:67-77` |
| TX CQ | 与阶段 2 完全同一条代码路径（`cqCreate`）：64 字节 CQE、`comp_eqn` = 本卡那个 EQ、`db_rec_addr` = CI record 的**字节** DMA 地址 | `mlx4_cq.c:270-286`；本阶段只换了 CQN |
| SQ 的 dummy stamp | `cpu_to_be32(1 << STAMP_SHIFT)` = `0x80000000` | `mlx4_en_rx.c:482-488`；`en.h:98` |
| SQ 的 TXBB 尺寸 | 64 | `en.h:84` |
| SQ 的 WQE 数组初值 | 全 0（激活路径自己就 `memset` 整个环） | `mlx4_en_tx.c:227` |
| SQ 的 MTT 用自己那份 | `mlx4_qp_to_ready(..., &ring->wqres.mtt, ...)` | `mlx4_en_tx.c:238-239` |
| 每 TXBB 的 stamp（**阶段 9 用**） | `STAMP_VAL \| owner << STAMP_SHIFT` | `mlx4_en_stamp_wqe`，`mlx4_en_tx.c:271-287` |
| A0 的 MAC 索引 → QPN | `*qpn = base_qpn + index` | `mlx4_en_netdev.c:573-576` |
| 告诉固件 base 的命令 | `context->base_qpn = cpu_to_be32(base_qpn)`，`n_mac = caps.log_num_macs` | `mlx4_SET_PORT_qpn_calc`，`mlx4_port.c:1578-1601` |

---

## 4. 两处**不照抄参考**的地方（都写明了理由与证伪条件）

阶段 6 与阶段 4/5 有一个性质上的不同：它对参考有两处**主动偏离**。两处都写在了代码注释、
工具输出和本节里，因为"照抄是默认，偏离必须有理由"是这两个阶段共同的规矩。

### 4.1 SQ 的 64 字节 dummy 头：参考的 TX 环**没有**它

本移植的 SQ 内存布局是 `[64 字节 dummy TXBB][256 × 64 字节 WQE 数组]`，头的首 dword 写
`1 << 31`（§27.4 的 "dummy SQ"，与阶段 3 的 RQ 同构）。而参考的 TX 环是
`ring->buf_size = ALIGN(size * ring->stride, PAGE_SIZE)`（`mlx4_en_tx.c:123`）、`ring->buf`
直接就是 TXBB 0（`mlx4_en_tx.c:686` 的 `ring->buf + index * TXBB_SIZE`）——**没有头**。

代价是具体且可说的：**WQE 数组的第 i 个 TXBB 是整块分配的第 i+1 个 64 字节单元**。所以
阶段 9 敲门铃时，门铃里的索引和这块内存的偏移只有在**一端**把那个头算进去时才一致。本阶段
不发帧，所以现在没有任何东西依赖它；`mlx4_en_tx.c:686` 是阶段 9 要对着看的那一行。

**证伪条件**：若阶段 9 发不出帧（门铃敲了、没有 TX 完成、外部抓不到），**第一件要试的事**就是
把这个头去掉（SQ 变成正好 `size × stride`，TXBB i 落在 `i*64`），重建、重试。

### 4.2 `db_rec_addr` 用**不左移**的自有 record

参考的共享 fill 写 `priv->res.db.dma << 2`（`mlx4_en_resources.c:78`），RX 路径随后用 ring
自己那条 record 的**字节**地址覆盖它（`mlx4_en_rx.c:947`，阶段 4 照此实现并通过回读）；
**TX 路径没有任何一行覆盖它**，所以出货的发送环收到的是左移那份。本移植发送环送的是
**不左移**的、自己分配的一页 record——与阶段 4 同形，也正是本任务的硬指定。

理由：`<< 2` 是一次乘四，而 vendored 树里没有任何一行解释这个字段；阶段 5 查完的结论是
"两边都是死的"（固件不读、驱动不写那条 record，`docs/R4-STAGE5-RESULT.md` §5.2）。发送侧
还多一条：**send queue 的 producer index 走的是 send doorbell（MMIO），不是这条 record**
（`mlx4_en_xmit` 写 `UAR + MLX4_SEND_DOORBELL`，`mlx4_en_tx.c:918-919`），所以参考在 TX 路径上
**从不往这条 record 里写 producer**——本移植也没有"发布 producer"这一步，代之以"SQ 内存可审计"
这一层（§2 的层 5）。

字段仍在八个比对字段的掩码里：固件若回了别的值，是**看得见的差异**，不会被"反正没人读"糊过去。

**证伪条件**：若阶段 9 发不出帧，这是与 §4.1 并列的第一件要改的事（改成 `<< 2` 再试一次）。

---

## 5. 硬约束：顶部 128 个 QPN 必须留空

这一条不是本阶段"顺手的好习惯"，而是**从 §41 直接压过来的约束**，代码里的算术与日志都照它
写：

1. A0 下固件把 MAC 表索引 `i` 映射到 `base_qpn + i`（`mlx4_en_netdev.c:573-576`）；
2. base 是**我们交给固件的参数**（`SET_PORT qpn_calc`，`mlx4_port.c:1595`），固件不校验 region；
3. 本项目的正规非 promiscuous 路线（§41.5 的方案 R）把 base 放在
   `num_qps - (1 << log_num_macs)`：本卡 = `1024 - 128 = 896`，于是 128 个 MAC 派生 QPN
   压在 QPN 空间**顶部**（896..1023），与我们放在 64/65 的 QP 永不相撞。

**所以本阶段的两个编号都必须落在 896 之下**，否则方案 R 在没被尝试之前就被堵死。两个输入
都从运行时来（**不写 1024、不写 128**）：

| 量 | 来源 | 实测值（本卡） |
| --- | --- | --- |
| `num_qps` | profile 自己提交的 `log_num_qps`（`fInitHca.log_num_qps`） | 1024 |
| `log_max_macs` | `QUERY_PORT` outbox `0x0a` 低半字节（offset 定义 `mlx4_fw.c:1206`，解码 `:1234-1236`；本 port 已解成 `Cx3QueryPortResult::logMaxMacs`） | 7 |
| 顶部保留起点 | `num_qps - (1 << log_max_macs)` | **896** |

编号本身也是**推出来的**，不是写死的：QPN = 既有两个 QP（RX 4 号的 64 与入口 QP 的 65）
里较大者 + 1 = **66**；CQN = RX CQN 128 + 1 = **129**。两者都低于 896，也都与既有对象不撞，
而"不撞"是**算完之后再比较结果**（`qpnCollidesRx` / `qpnCollidesIngress` / `cqnCollidesRx`），
不是对意图的复述——改取号逻辑不会悄悄产生两个同号对象。

`log_max_macs` 若大到 `1 << log_max_macs >= num_qps`（这一套算术给不出可用的线），选择是
**拒绝**（`kIOReturnNoSpace`），而不是把线画在某个看起来合理的地方。

---

## 6. 失败时的分岔判读

§27.2 点名三个"失败先查自己"，逐条给出可查的位置：

| 症状 | 先查什么 |
| --- | --- |
| 第一步 `fw status 0x3`（BAD_PARAM） | ① **mailbox 偏移**：context 在 `+8`（`qpModify` 每次都打印它用的偏移）；② `sq_size_stride`：发送侧是 `0x42`，**不是** RX 的 `0x02`（日志把 size/stride 与算出来的字节并排打） |
| 三步成功但 `statesConfirmed < 3` | "`QUERY_QP` 自己没答"（`queryOk != 1`）与"答了但不是那个状态"是两件事：前者查 `QUERY_QP` 编码，后者查 `flags[31:28]` 三步之间是否被覆写 |
| 状态对了但 `FieldsMatch = 0` | 日志逐字段并排打 written/read，并点名是哪一条。`sq_size_stride` 是这条线里最像"设备错了"、实际最常是"我们写错了"的字段 |
| `MttMatches = 0` | MTT 只在 RST→INIT 写、被后两步带着走；不一致说明共享 context 被扰动（幂等/并发） |
| `CqFwStatus != 0` | `SW2HW_CQ` 的 context 与阶段 2 一模一样（只有 CQN 不同），所以先看 CQN 是否落在保留区/超出 profile，再看 UAR 与 `comp_eqn`。**本阶段在 CQ 失败后不再往下走**（日志写明），因为一条 `cqn_send` 指向不存在 CQ 的 QP 比没有 QP 更糟 |
| `CqReady != 0` | 刚建好的环被读成"有完成"：查 owner preset（`cqCreate` 自己会扫并打印）与 `cons_index` 的相位 |
| `NumberSafe = 0` | 先看 `topReservedStart` 的两个输入（日志把 `log_max_macs`、`num_qps` 都打出来），再看四个比较各自的值；`kIOReturnNoSpace` 说明是**空间**问题（提高 profile 的 QP 数才是修法） |
| `SqStampMatches = 0` | dummy TXBB 的首 dword 不是 `1 << 31`：查 `sqCreate` 写头的四行与它读回来的同一处（同一段代码写、同一段读） |
| 全绿但阶段 9 发不出帧 | 按 §4.1（先去掉 SQ 的 64 字节头）、§4.2（把 `db_rec_addr` 改成 `<< 2`）、以及阶段 4 记的 `roce_entropy` / `counter_index` / FCS 顺序排查。这三条现在都有明确依据，不用重新推 |

---

## 7. 实测 / 推断 / 未测

### 实测（本轮真的跑出来的）

* **构建门禁全绿**：`boot-path` PASS（`start()` 可达函数集 19 个，无写、无 DMA、无 reset）·
  `146 + 8186 + 137 + 45` 项检查 0 失败 · 未定义符号 **352/352 全部解析**（与基线相同：
  新增的全是内部方法）· 四个 `-Werror` 档零告警 · `tools/check-hardcoded.py --strict` 通过
  （8186 项，0 失败）。
* **kext sha256 = `7138ec7e3ca878605a6c67a4c04245460ca6271a4626153ba42a02df174e5b51`**，
  342888 字节（`BUILD_TAG=0.43.5`）。**可复现**：同一份树**连续三次构建得到同一 sha**
  （`/tmp/cx3-435i`、`/tmp/cx3-435j` 非 root，`/tmp/cx3-435` 是任务要求的
  `sudo -A env BUILD_TAG=0.43.5 OUT=/tmp/cx3-435 bash build.sh`）。顺带记一条给下一个人：
  `OUT` 若上一次是 root 建的，
  非 root 那次构建会因为删不掉旧产物而**被 `artifact freshness` 闸门拦下**（闸门是对的，
  它抓到的是"产物没被真正替换"）。
* `cx3ctl sizes` 自证新行：**`36 2 16 384 r4tx`**（2 标量入、16 标量出、结构 384 字节），
  这一行是**编译器算的**（`sizeof(Cx3R4TxResult)`），与 kext 的分发表同源。
* `verify.sh` 的 17 个新索引名在 `src/cx3_abi.h` 里全部解析成功（用 `abi()` 的正则逐个验过）。
* `bash -n tools/verify.sh` 通过（本机 bash 3.2）。
* 静态断言：分发表与锁类别表的长度（`== kCx3SelCount = 37`）、标量上限（16 = 上限）、
  三步数组长度，编译期全部通过。
* `cx3ctl r4tx` 在**没有设备**时的行为符合预期：打开失败即返回，不打印任何伪造的读数。

### 推断（有源码依据，未上机）

* 第 3 节的全部编码：逐条照抄参考，行号已核。
* "TX 的三步与 RX 一样能到 RTS"：同一套 opcode、同一份 context 布局、同一个 EQ；差异只在
  两个 size/stride 字节、`db_rec_addr` 的形态与 `param3` 的那一位。
* "空环读作空"：`cqCreate` 的 owner preset 在阶段 2 真机通过；本阶段用的是同一条路径。
* `log_max_macs = 7`：来自 §41.1 的**实测**记录（`QUERY_PORT` 字节 `0x0a = 0x77`），本轮没有
  重新读它——本阶段只在日志里报它、并把它当算术输入。

### 未测（明确没做，或做不到）

* **`r4tx` 从未运行**：kext 未安装、机器未重启（本轮硬约束）。TX CQ 是否被接受、空环是否
  真的读作空、send QP 是否真的三步到 RTS、`sq_size_stride` 是否真的回读为 `0x42`、
  `db_rec_addr` 与 dummy stamp 是否真的如写——**全部未测**。`docs/COVERAGE.md` 的 `r4tx` 行
  证据栏写的就是"未运行"。
* `tools/verify.sh run|full|safe` 未跑（会驱动硬件）；kext 未安装。
* **`cx3ctl r4tx` 的排版只在编译层面验过**（无设备时只走到"IOServiceOpen failed"），
  没有对着真实的 `Cx3R4TxResult` 打印过一遍；第 6 节的分岔判读是设计意图，不是实测过的判读路径。
* **幂等路径：本阶段刻意没有**（前两个阶段都有"已存在就重测"的分支）。原因写在选择器里：
  第二次调用会是**第二次创建**（第二块 SQ 内存，第一条 QP 的 context 并不指向它），而不是同一个
  对象的第二次**读数**；固件那边的说法更直接——为一个它已经拥有的 CQN 再发 `SW2HW_CQ` 会失败，
  而那看起来像缺陷而不像"你跑了两遍"。所以第二次调用在入口就**拒绝**
  （`kIOReturnExclusiveAccess`，日志说明怎么恢复）。这条拒绝路径没在硬件上走过；
  验证链不会碰到它（`verify.sh run` 开头先 reset，teardown 里就有 `txTeardown`）。
* 两个队列尺寸（CQ 项数、SQ 的 TXBB 数）在**选择器里**就向上取到 2 的幂（再传给两个创建函数），
  这样"报出来的数 / 写进 context 的数 / 各种扫描用的数"是同一个数；`0` 表示用默认值
  （CQ 256、SQ 256）。这条也没在硬件上验过，但它是纯主机侧算术。
* `txTeardown` 的三个调用点（reset / `stop()` / 重做 profile）只在编译层面通过，
  **回滚路径未实测**。

---

## 8. 回滚

阶段 6 基本是**加法**：一个新选择器 + 新常量 + 三个新 teardown 分支。动了既有行为的有三处，
都是**插入**调用、不改既有顺序：reset 路径、`stop()`、`m2dprofile` 各插一次 `txTeardown`
（都在 `ingressTeardown` 之后、`qpTeardown` 之前）。另外两处既有 `qpFillContext` 调用各补了
一个 `isTx=false` 实参——那是签名扩展，语义不变（阶段 4/5 的行为由回归测试的门禁与真机链覆盖）。

1. **代码回滚**（本轮已提交成一条提交）：

   ```bash
   git -C /Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4 revert --no-edit <该提交>
   ```

   然后重建、重装、重启。构建产物这一轮写在 `/tmp` 下的临时目录里，**没有覆盖
   `outputs/20261010-cx3-m1/build`**，所以仓库里没有需要清理的中间产物。

2. **运行期退路**：`sudo tools/cx3-mlx4/rollback.sh [--purge]`（未改动）。

3. **硬件侧残留**：阶段 6 会在设备里留下**一条 TX CQ（CQN 129）**与**一条 RTS 状态的 QP（QPN 66）**。
   回滚后它们仍在，直到 HCA reset（`m2dbringup` 之前的 `reset` 会清掉，这也是验证链条本来
   就做的事）。**顺序要求**：先 `txTeardown`（`2RST_QP`）再释放 SQ 的 MTT 与 record，再
   `HW2SW_CQ` 收回 CQ——否则固件还通过 context 里的 MTT 与 `db_rec_addr` 翻译着那块内存。
   任何一步失败都**故意不释放**（泄漏一次重启可回收的内存，好过让设备写进已回收的内存），
   日志写明是哪一步。

---

## 9. 用户重启后要跑的那条命令与期望输出

按项目惯例安装，然后跑一次：

```bash
sudo tools/cx3-mlx4/verify.sh stage      # 构建 + 校验 + 安装（不重启）
sudo reboot
sudo tools/cx3-mlx4/verify.sh run        # 重启后：整条链，含新的 r4tx
```

只想单独看阶段 6（前提：同一 boot 里已跑过 `m2dbringup`、`m3equp`、`m3clint`、`m3portup`、
`r4mr`、`r4cq`、`r4rq`、`r4qp`、`r4ingress`）：

```bash
sudo /usr/local/bin/cx3ctl r4tx          # 不带参数：CQ 256 项、SQ 256 个 TXBB
```

**期望（成功那一支，`verify.sh run` 里的六行）**：

```
TX CQ: CQN 129 accepted (fw status 0x0) and the fresh ring reads as empty            PASS
TX numbers: QPN 66 and CQN 129 both below 896 (= num_qps - (1 << log_max_macs)) and
            colliding with nothing that exists                                       PASS
TX QP layer 1: 3/3 commands accepted, 3/3 states read back after each step           PASS
TX QP layer 2: RTS, local_qpn 66 confirmed, and all eight compared fields match      PASS
TX SQ: sq_size_stride 0x42 (256 TXBBs of 64 bytes), the byte as sent                 PASS
TX SQ: the dummy TXBB's first dword reads back as 1 << 31, out of the memory itself  PASS
```

`cx3ctl r4tx` 的关键几行应当长这样（数字来自本卡前几阶段已实测的值）：

```
  the number space, from two values read at runtime
    QPN space            : 1024  (the profile's log_num_qps)
    log_max_macs         : 7  (QUERY_PORT outbox 0x0a, low nibble -> 128 MAC-derived QPNs)
    top-reserved start   : 896  (num_qps - (1 << log_max_macs) = 1024 - 128)
    TX QPN               : 66  (one above the RX QP 64 and the indirection QP 65)
    TX CQN               : 129  (one above the RX CQN 128)
    both below it        : yes
  ...
    SW2HW_CQ             : fw status 0x0 (accepted)
    empty-ring check     : 0 readable slot(s) at cons_index 0 -- correct
  ...
    the dummy TXBB       : first dword reads back 0x80000000 (as written)
    the WQE array        : 0 non-zero byte(s)
  ...
      RST2INIT (0x19): fw status 0x0 ok ;  state after: 1 (wanted 1) ok
      INIT2RTR (0x1a): fw status 0x0 ok ;  state after: 2 (wanted 2) ok
      RTR2RTS  (0x1b): fw status 0x0 ok ;  state after: 3 (wanted 3) ok
  ...
        sq_size_stride       0x42 / 0x42   <- 0x42 is this stage's signature
        rq_size_stride       0x00 / 0x00   <- 0 for a send QP, not the RX 0x40
```

**失败时的分岔**：见第 6 节的表。最容易误判的两条：**`rq_size_stride 0x00 / 0x00` 是"什么都没
声明"，不是"固件保住了我们写的东西"**（`docs/DEFECTS.md` D13）；以及 **TX 侧 `param3 = 0` 是
故意的**（FCS 位属于接收侧），不是漏写。
