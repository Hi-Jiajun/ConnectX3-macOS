# R4 阶段 7（`r4A0Port`）实现记录：把 `qpn_calc` 插进端口 bring-up

本文是**实现记录**，实现的是 `docs/R4-STAGE7-PLAN.md`（已定稿的方案，本批次**没有改它**），
外加方案里点名的两个小修：`docs/DEFECTS.md` C12（阶段 6 的 SQ 初始所有权）与
`docs/M2B-NOTES.md` §42.4 那处"文字即缺陷"。

**本阶段一行都没在硬件上跑过**：kext 未安装、机器未重启、`tools/verify.sh run` 未执行
（本批次的硬约束）。所以第 7 节把每条结论按"实测 / 推断 / 未测"分开写，第 10 节是重启后
**应当**看到什么（预测，不是记录）。

一句话结论：**端口激活现在是参考的那三条命令**（general → `qpn_calc` → INIT_PORT），
`qpn_calc` 的 base 是本 port 的 RX QPN（运行时取值，本卡 64），`promisc` 0，
`m_promisc` 从设备的 `VEP_MC_STEER` 位读出（本卡为 1 ⇒ `MCAST_DIRECT`）；发送队列的内存
按参考的说法把**每个 64 字节 TXBB 的首 dword 写成 `0xffffffff`（软件拥有）**，
并且把"我们读回的是自己写的东西"这件事写进了判据。
**代价写明**：base = 64 使 **MAC 表索引 ≥ 1 不可用**（会撞上入口 QP 65 与发送 QP 66），
本配置只支持一个 MAC 地址。

---

## 1. 改动的文件（绝对路径）

| 文件 | 改了什么 |
| --- | --- |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/mlx4_regs.h` | 新的 "R4 stage 7" 一节：`RQP_CALC_*` context 偏移（14 个）、`SET_PORT_PROMISC_SHIFT`/`SET_PORT_MC_PROMISC_SHIFT`、`MCAST_DIRECT_ONLY/DIRECT/DEFAULT`、`MLX4_NO_VLAN_IDX`/`MLX4_VLAN_MISS_IDX`、`MLX4_DEV_CAP_FLAG_VEP_MC_STEER`；并把 stage 6 的 stamp 一节从"RX 头标记"改成发送侧的 `MLX4_SQ_STAMP_VAL` + `MLX4_SQ_INIT_OWNER_BIT`（`0xffffffff`），`MLX4_SQ_DUMMY_STAMP` 这个名字随之消失 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Mlx4Hw.h` | `setPortQpnCalc(...)` 声明；`sqCreate` 增加 `outTxbbsStamped` 形参；两处文档注释按新判据重写 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Mlx4Hw.cpp` | 新函数 `setPortQpnCalc`（mailbox `+0` 起、`in_modifier = RQP_CALC<<8\|port`、`op_mod = ETH`、超时 class B，并把 28 字节 context 打成一行日志）；`sqCreate` 改成"整块每个 64 字节单元的首 dword 写 `0xffffffff`"，测量改成三件（头 dword、被 stamp 的 WQE TXBB 计数、stamp 之外的非零字节数） |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/cx3_abi.h` | `kPortUpOutQpnCalcFwStatus = 10`、`kPortUpOutCount = 11`（**未超 `CX3_SCALAR_OUT_MAX = 16`**，并新增该选择器的静态断言）；`kPortUpOutStep` 的语义注释改成 1..5；新枚举 `kPortUpStepGate/General/QpnCalc/InitPort/Steps`（步骤数的唯一出处）；`Cx3R4TxResult` 新增 `sqTxbbsStamped`，并重写 `sqStampValue`/`sqStampMatches`/`sqWqeNonZeroBytes`/`sqWqeUntouched` 四条注释 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Cx3Mlx4.cpp` | `exportM3PortUp`：在 general 与 INIT_PORT 之间插入 `qpn_calc`（含 base 的来源、`VEP_MC_STEER` 读取、代价日志、失败即停链）；`kPortUpOutStep` 的**全部**赋值点改成具名步骤；base 不在 profile 内时**拒绝**；`r4TxPublishScalars` 的 SQ 标量改成"头 + 全部 TXBB"的合取；`exportR4Tx` 接上新的三个测量并重写相关日志；**C 项**：`r4tx` 那句 `%u x the numbers are safe` 改成读得出含义的形式 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/tools/cx3ctl.c` | `m3portup`：打印 `qpn_calc` 的 fw status、`stage reached : N of <kPortUpSteps>`（步骤数从 ABI 读，不再写死 "of 4"）、失败时给出三分支读法；`r4tx`：SQ 的打印改成"每个 64 字节单元的首 dword / 多少个 TXBB 被 stamp / stamp 之外的非零字节" |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/tools/verify.sh` | 解析 `kPortUpOutQpnCalcFwStatus`、`kPortUpSteps`、`kPortUpStepQpnCalc`；`m3portup` 段新增一条判据，措辞区分**被拒绝 / 调用了但没记录 / 根本没走到**（外加 `POSTED 但未完成`）；`r4tx` 的 stamp 行改成"每一个 TXBB"的断言与说明 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/kext/Info.plist` | 版本 0.43.5 → **0.43.6** |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/docs/COVERAGE.md` | `r4a0port` 行（链内，证据栏 **未运行**）、`r4tx` 行补记 0.43.6 的判据变化、版本号同步 0.43.6 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/docs/DEFECTS.md` | C12 确认与"落在哪个版本"；新增 **F11**（工具里那句写死的 "of 4"）；总数与 F 家族计数同步 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/docs/R4-STAGE7-RESULT.md` | 本文件 |

**没有动**：`docs/R4-STAGE7-PLAN.md`、`docs/R4-STAGE6-RESULT.md`、`docs/M2B-NOTES.md`、
`docs/EXTERNAL-REFS.md`、`docs/LOCK-IMPL.md`、`docs/EQ-ORDERING.md`、`tools/mlx4-port/`、
`driver/`、`outputs/` 下别人的目录、ESP、`/Library/Extensions/`。

---

## 2. 判据：每条命令自己的标量，以及三种"读起来不同"的失败

`m3portup` 现在有**四个** firmware 状态标量（每个都先写 `CX3_FWSTATUS_NOT_POSTED`，
所以零缓冲不会读成"接受"）：

| 步 | 标量 | 通过条件 | 为什么它算证据 |
| --- | --- | --- | --- |
| 1 门 | `kPortUpOutQpFwStatus` | `0` | `QUERY_PORT` 答了，且 `supported_types` 含 Ethernet |
| 2 general | `kPortUpOutSetFwStatus` | `0` | `SET_PORT` general 被接受（MTU 实际发出值另有标量） |
| **3 qpn_calc** | **`kPortUpOutQpnCalcFwStatus`** | **`0`** | 固件被告知了 base QPN —— 这是"帧会落到哪个 QPN"的唯一开关 |
| 4 INIT_PORT | `kPortUpOutInitFwStatus` | `0` | 端口被初始化（**不等于**链路 up，链路另有标量） |
| — | `kPortUpOutStep` | `5` | 走到第几小步（1 门、2 general、3 qpn_calc 尝试、4 INIT_PORT 尝试、5 全成） |

`tools/verify.sh` 对第 3 步的四种答案给出**四种不同的话**（这是本阶段最容易被写坏的判据，
因为"没跑到"和"跑了但没记录"都表现为沉默）：

| 标量的值 | 判据行 | 读法 |
| --- | --- | --- |
| `0` | **PASS** | 命令发出并被接受 |
| `0x3` / `0x5` 等真实错误码 | **FAIL** | 被**拒绝**；0x3/0x5 就是方案 §6 写下的**证伪条件**：固件确实校验 base 落在某个 region 里 |
| `0xfffffffe`（`NOT_POSTED`）且 `kPortUpOutStep` ≥ 3 | **FAIL** | **调用了但没记录**：驱动问了设备却没把答案写进标量 —— 驱动缺陷，不是固件的 |
| 同上，但 `kPortUpOutStep` < 3（`kPortUpStepQpnCalc`） | **SKIP** | **根本没走到**：前面某一步失败了，要读的是那一行，这不是第二个故障 |
| `0xfffffffd`（`TIMEOUT`）<br> | **FAIL** | 发出去了但**没完成**（超时，B1 家族）—— 与"被拒绝"是两件事 |

> 第 4 行的阈值与第 3 行的步号都从 `src/cx3_abi.h` **按名字解析**（`abi()`），
> 不在 verify.sh 里再写一份数字。理由见第 6 节 F11。

**`qpn_calc` 失败即停链**（不继续 INIT_PORT），与 general 失败同样处理：固件不知道 base 时
initialize 端口没有意义 —— 那样的端口会 up、会训练链路、会把每个帧都导向一个没人拥有的号，
而症状要到阶段 9 才显形。日志把这条决定写出来了。

---

## 3. 编码：照抄的那些值与来源（行号已逐条核对）

来源全部是 vendored 参考（`vendor/freebsd-src/`，路径相对该目录）。

| 项 | 值 | 依据（核对后） |
| --- | --- | --- |
| opcode | `MLX4_CMD_SET_PORT = 0xc` | `cmd.h:63` |
| `op_mod` | `MLX4_SET_PORT_ETH_OPCODE = 0x1` | `cmd.h:195` |
| `in_modifier` | `MLX4_SET_PORT_RQP_CALC << 8 \| port` = **`0x101`**（port 1） | `cmd.h:202`、`mlx4_port.c:1606` |
| `in_param` | **mailbox 的 DMA 地址**（context 在 `+0`） | `mlx4_port.c:1591-1594` |
| 超时 | `MLX4_CMD_TIME_CLASS_B` | `mlx4_port.c:1607` |
| `promisc` 位 | `<< 31`；`mcast` 的 `m_promisc` 位 `<< 30` | `en_port.h:39-40`（同 `mlx4.h:769-770`） |
| `m_promisc` | `caps.flags & VEP_MC_STEER ? MCAST_DIRECT(1) : MCAST_DEFAULT(2)` | `mlx4_port.c:1585-1586`、`mlx4.h:772-774`、`device.h:182`（bit 42） |
| `no_vlan` / `vlan_miss` | `MLX4_NO_VLAN_IDX = 0` / **`MLX4_VLAN_MISS_IDX = 1`** | `device.h:424-425`、`mlx4_port.c:1600/1602`（前者显式 0，后者不是 0） |
| context 布局 | `base_qpn`(0x00, BE32)、`reserved`(0x04)、`n_mac`(0x05)、`n_vlan`(0x06)、`n_prio`(0x07)、`reserved2[3]`(0x08)、`mac_miss`(0x0b)、`intra_no_vlan`(0x0c)、`no_vlan`(0x0d)、`intra_vlan_miss`(0x0e)、`vlan_miss`(0x0f)、`reserved3[3]`(0x10)、`no_vlan_prio`(0x13)、`promisc`(0x14, BE32)、`mcast`(0x18, BE32) | `mlx4.h:798-813` |
| 参考实际赋值 | 只有 `base_qpn`、`n_mac`、`promisc`、`mcast`、`intra_no_vlan`、`no_vlan`、`intra_vlan_miss`、`vlan_miss` 八个（其余靠零缓冲） | `mlx4_port.c:1595-1604` |
| 发送侧 stamp | `STAMP_VAL = 0x7fffffff`、`STAMP_SHIFT = 31`；`owner = 1` ⇒ `0xffffffff` | `en.h:95-99`、`mlx4_en_tx.c:278-279`（DPDK 独立实现同构，`docs/EXTERNAL-REFS.md` §4） |
| 整环 stamp 的时机与范围 | `memset` 之后，`for (j = 0; j < buf_size; j += STAMP_STRIDE) *((u32*)(buf+j)) = INIT_OWNER_BIT;` | `mlx4_en_tx.c:227`、`mlx4_en_netdev.c:1373-1375` |
| A0 的索引映射 | `*qpn = base_qpn + index` | `mlx4_en_netdev.c:573-576` |

`MLX4_NO_VLAN_IDX` 与 `MLX4_VLAN_MISS_IDX` 是**两个不同的值**这件事值得单独说：十四个
context 字段里有十一个"清成 0 就对了"，而 `vlan_miss` 不在其中。"没赋值"和"赋 0"
在 diff 里长得一模一样，所以代码里逐条写出来，而不是靠零缓冲。

---

## 4. 三处要解释的选择（都不是默认值）

### 4.1 `base_qpn` = 本 port 的 RX QPN，代价是 MAC 索引 ≥ 1 不可用

方案第 5 节决策①（用户已拍板），照此实现：base = RX QPN = **64**，不是 896。

* 好处：MAC 槽 0 ⇒ `64 + 0` = 64 = 我们的 RX QP，**自洽**；阶段 5 只用了槽 0。
* **代价（日志与本文都写明）**：索引 `i` 映射到 `64 + i`，于是
  **索引 1 ⇒ QPN 65 = 阶段 5 的入口 QP**、**索引 2 ⇒ QPN 66 = 阶段 6 的发送 QP**，
  再往上继续落在本驱动自己的编号里。⇒ **本配置只支持一个 MAC 地址**，MAC 表不能"用满
  128 槽"。要支持多 MAC，必须把 base 移到 `num_qps - (1 << log_max_macs)`（本卡 = 896）
  **并把 RX QP 一起搬过去** —— 那要改阶段 4 的取号，是一次决策而不是实现细节，
  所以记在这里、**这一批没做**。

### 4.2 base 从哪来：两支，都是运行时读出来的

`m3portup` 在验证链里**早于** `r4qp`（参考也是先告诉固件 base，再开始建队列），
所以调用时 RX QP 通常**还不存在**。取号因此分两支，两支都打日志说明用了哪一支：

1. **RX QP 已存在** ⇒ `fR4QpQpn` —— 阶段 4 从 `QUERY_QP` 回读确认过的那个号
   （"设备确认的号"，不是驱动打算用的号）；
2. **否则** ⇒ `fDevCap.reserved_qps` —— **和阶段 4 给 RX QP 取号用的同一条规则**
   （`exportR4Qp` 的 `qpnRule`），本卡实测 64。

两支都不是字面量。`check-hardcoded.py --strict` 通过（8186 项检查）。

### 4.3 新增的一处拒绝：base 不在 profile 内就不发这条命令

`baseQpn < reserved_qps || baseQpn >= (1 << log_num_qps)` ⇒ 拒绝（`kIOReturnNotReady`，
打日志、**不发命令**）。原因：`fDevCap`/`fInitHca` 没被填过时 `reserved_qps` 是 0，
而 base = 0 会把 A0 的 MAC 表指向**固件自己的 QP**；那种命令成功以后看起来和正确的一模一样。
这是本批次**唯一**新增的运行时守卫，写在这里以便事后审计；验证链不会走到它
（链里先跑 `m2dprofile`）。

### 4.4 A0 的判定：没有加开关，理由写在日志与注释里

参考的 `mlx4_SET_PORT_qpn_calc` 在非 A0 时**直接 `return 0`**（`mlx4_port.c:1588-1589`），
即 B0 下它是个空操作而不是另一件事。本 port 的 `INIT_HCA` 从未写 B0 的 steering 字节
（`INIT_HCA_UC_STEERING_OFFSET`），所以 HCA 被告知的是 A0，这条命令就是参考会发的那条。
这一批**没有**为此加一个"先查 steering mode 再决定发不发"的分支：那会是一个没有实测依据的
新守卫，而方案给的分岔（fw status 0x3/0x5）本来就是这条假设的证伪条件。

---

## 5. C12：发送队列的初始所有权，以及阶段 6 为什么没抓到

**参考在端口激活时做两步**（`mlx4_en_tx.c:227` 然后是 `mlx4_en_netdev.c:1373-1375`）：

```c
memset(ring->buf, 0, ring->buf_size);
for (j = 0; j < tx_ring->buf_size; j += STAMP_STRIDE)
        *((u32 *) (tx_ring->buf + j)) = INIT_OWNER_BIT;   /* 0xffffffff = 软件拥有 */
```

阶段 6 只做到了第一步（WQE 数组留成全零），dummy 头的 stamp 又取了 RX 环的标记
`1 << 31 = 0x80000000`。两处都改掉了：

* **每个 64 字节单元的首 dword 写 `0xffffffff`**（`MLX4_SQ_INIT_OWNER_BIT =
  STAMP_VAL | 1 << 31`），**包括 SQ 最前面那个 64 字节头** —— 对它而言同一个值表达同一件事：
  这个 64 字节不是已投递的描述符；
* 全零的**语义**也随之说清：bit 31 = 0 = "**硬件拥有**"，于是阶段 9 的回收扫描会把**每个**
  槽位都看成在飞行中。

### 5.1 阶段 6 那条判据为什么没抓到它（也就是"文字即缺陷"的另一半）

阶段 6 的判据是"把 dummy TXBB 的首 dword **从内存里读回来**，等于 `1 << 31`"。它**真的**
执行了、也**真的**通过了，因为它读回的是**我们自己刚写进去的东西**。"我们写进去了"与
"硬件怎么解释它"是两句不同的话，而那条判据只能证明前一句 —— 这正是 D 家族的形状
（一个看起来像测量的检查），`docs/EXTERNAL-REFS.md` §5 把它记下来了，C12 行也写了同一句。

所以这一批把判据**拆成两半**，两半都是能被读出来的数：

| 量 | 结构字段 | 通过条件 | 它真正回答的问题 |
| --- | --- | --- | --- |
| 头 dword | `sqStampValue` / `sqStampMatches` | `0xffffffff` | 那个 64 字节单元不是描述符 |
| **被 stamp 的 WQE TXBB 数** | `sqTxbbsStamped` | `== sqNent` | **整块内存**都处于"软件拥有"，不是一个头 |
| stamp 之外的非零字节 | `sqWqeNonZeroBytes` / `sqWqeUntouched` | `0` | 没有任何描述符体被写过（"什么都没投递"） |

单条标量 `kR4TxOutSqStampMatches` 是前两行的**合取**（标量槽 16 个已用满，见
`docs/R4-STAGE6-RESULT.md` §2 的注）：只报头的话，"数组没被 stamp"这一半会照样 PASS，
而那一半正是阶段 9 会踩到的。两个分量在结构体里分开、`cx3ctl r4tx` 都打印，所以失败时
说得清是哪一半。

**仍然要写明**：这三条仍然只是"我们写的东西在那里"，与硬件如何解释**无关**。
真正能证明所有权状态被硬件接受的那次测量在阶段 9（发出一个 WQE、看它回来）。

---

## 6. 三处"文字即缺陷"，本批修掉两处、另记一行

1. **`r4tx` 的 `1 x the numbers are safe`**（`src/Cx3Mlx4.cpp`，`docs/M2B-NOTES.md` §42.4）：
   `%u` 打的是 `numberSafe`（值 1），后缀 `x`，读起来像"验证了 1 次"。值没错、标量也在判，
   但**句子是缺陷**。现在读作"安全位是 %u —— 1 表示安全、0 表示不安全，读它当值而不是当
   计数"，并且句子不再预设值是几（旧写法里的 "It is 1 because…" 同样会被 0 打脸）。
2. **`cx3ctl` 的 `stage reached : N of 4`**：链从 4 步变成 5 步以后，工具会永远说 "of 4"
   而设备报 5 —— 对同一次运行的两份互相矛盾的陈述，正是 F 家族。修法不只是把 4 改成 5：
   步骤数收进 `src/cx3_abi.h`（`kPortUpStep*` 枚举），工具打印它、verify.sh 按名字解析它，
   于是**这个数字只有一个出处**。已记成 `docs/DEFECTS.md` **F11**。
3. `docs/DEFECTS.md` 的 C12 行：确认其描述与本实现一致（"每个 TXBB 写 `0xffffffff`、
   头用发送侧 stamp 编码、并写明读回的是自己写的东西"），补记落在 0.43.6。

---

## 7. 实测 / 推断 / 未测

### 实测（本轮真的跑出来的）

* **构建门禁全绿**：`boot-path` PASS（`start()` 可达函数集里无写、无 DMA、无 reset）·
  **146 + 8186 + 137 + 45** 项检查 0 失败 · 未定义符号 **352/352 全部解析**（与基线相同：
  新增符号全是内部方法）· 四个 `-Werror` 档（shorten-64-to-32 / format /
  shift-count-overflow / implicit-int-conversion）**零告警**（构建日志里 `warning:` 计数为 0）·
  `tools/check-hardcoded.py --strict` 通过。
* **kext sha256 = `4e3ba7f6ca7b11df88c443dbf501f37484bc217e80a39e6f509e94650060bc77`**，
  351448 字节（`BUILD_TAG=0.43.6`，0.43.5 是 342888 / `7138ec7e…`）。
  **可复现**：`sudo -A env BUILD_TAG=0.43.6 OUT=/tmp/cx3-43x bash build.sh` 连跑
  **五次**（`/tmp/cx3-436a`…`436e`）得到同一 sha；其中 a/b/c 是"字面量 3/5 尚未改成
  具名枚举"那一版，d/e 是最终版，**两者字节相同** —— 名字替换字面量、值不变，属于
  `-O2` 下的同一份代码，这条本身也记在这里免得下一个人以为少算了一次。
* `cx3ctl sizes` 自证：**`25 2 11 0 m3portup`**（2 入参、**11** 个标量出，0.43.5 是 10）、
  **`36 2 16 392 r4tx`**（0.43.5 是 384：新增 `sqTxbbsStamped`）。两行都是**编译器算的**。
* verify.sh 里四个新索引名都按 `abi()` 的正则解析成功（用同一段正则逐个验过）；
  `bash -n tools/verify.sh` 通过（本机 bash 3.2）。
* 编译期静态断言：`kPortUpOutCount (11) <= CX3_SCALAR_OUT_MAX (16)`、派发表/锁类别表长度
  `== kCx3SelCount (37)`、`Cx3R4TxResult` 的尺寸与掩码长度，全部通过。
* **无设备时的行为**：`cx3ctl m3portup` 打印新的顺序行与 "stage reached" 行后，
  在 `IOServiceOpen failed` 处停下，不打印任何伪造读数。

### 推断（有源码依据，未上机）

* 第 3 节的全部编码，逐条照抄参考，行号已核。
* "base = 64 自洽"：槽 0 ⇒ QPN 64，而阶段 4 的 RX QP 就是 64（实测值来自阶段 4/6 的
  日志与 `docs/M2B-NOTES.md` §42.1）；"索引 ≥ 1 会撞"是同一套算术的直接结果。
* `n_mac` 用 `QUERY_PORT` 的 `log_max_macs`（本卡 **7**）：`n_mac` 是**幂值**，参考填的
  也是 `caps.log_num_macs`（`mlx4_port.c:1596`），不是 128。本阶段不重新读它，
  用的是**同一次调用里门那条 `QUERY_PORT`** 的 `logMaxMacs`。
* `VEP_MC_STEER = 1` ⇒ `MCAST_DIRECT`：位在 `caps.flags` 里（本卡
  `ext_flags 0xeeff1f9d << 32 | flags 0xf677fbef`，bit 42 = 1，方案第 3 节），
  本阶段从 `fDevCap.flags` 读出来而不是写死；"读得对"这件事本身要等第 10 节的日志确认。
* 发送队列的 owner 语义（"硬件怎么解释那 4 个字节"）：两份独立实现一致
  （`docs/EXTERNAL-REFS.md` §4），但**本阶段不产生**任何依赖它的帧。

### 未测（明确没做，或做不到）

* **`m3portup` 的新中间步从未在硬件上跑过**：`qpn_calc` 是否被接受（`fw status 0x0`）、
  端口是否仍 up、`INIT_PORT` 是否仍成功 —— 全部未测。kext 未安装、未重启、
  `verify.sh run` 未跑（本批次硬约束），`docs/COVERAGE.md` 的 `r4a0port` 行证据栏写的就是
  **未运行**。
* **"帧落到 QPN 64" 本阶段无法验证**：这是阶段 9 的判据，方案 §6 就把它标成未测。
* **C12 的修法未在硬件上验**：`sqTxbbsStamped == sqNent`、
  `sqWqeNonZeroBytes == 0` 都是主机侧读数；阶段 9 第一次真正回收 WQE 时才知道
  固件是否按同样的约定解释它们。
* **`cx3ctl` 的新排版只在编译层验过**（无设备时只走到 `IOServiceOpen failed`），
  没有对着真实的 `Cx3PortUpResult`/`Cx3R4TxResult` 打印过一遍。
* **第 10 节的 PASS 行数是预测**（47 + 1 = 48），不是记录。
* 拒绝路径（4.3）与 `m3portup` 的幂等性：验证链不会走到前者；后者沿用既有行为
  （第二次 `SET_PORT`/`INIT_PORT` 本来就允许），本批没有新增幂等分支。

---

## 8. 失败分岔判读

| 症状 | 先查什么 |
| --- | --- |
| `qpn_calc` 回 `0x3` / `0x5` | 固件**确实**校验 base 落在 region 里 ⇒ 方案 §6 的证伪条件成立：把 base 改成 `num_qps - (1 << log_max_macs)` 并搬 RX QP。日志里那条命令的 28 字节 context 是证据（本来是为了这一刻打的） |
| `qpn_calc` 成功但 `INIT_PORT` 失败 | **插错了位置**（顺序问题，不是参数问题）：`qpn_calc` 必须在 INIT_PORT 之前，且 general 之后 |
| `kPortUpOutStep = 3` 且 fw status 为 `0xfffffffe` | 驱动**没记录答案**（不是设备问题）：看 `setPortQpnCalc` 的日志有没有"accepted/failed"那两行 |
| `step` < 3 | 命令**根本没发**：读它上面那一行的失败（门 / general） |
| `step = 5`、链路 up、但阶段 9 收不到帧 | ① `mcast` 的 `MCAST_DIRECT` 是否真的从设备读出来（日志打 `caps.flags` 与 bit 42）；② `counter_index`；③ 阶段 4/5 记的 `db_rec_addr`、FCS 顺序 |
| `r4tx` 的 SQ 判据 FAIL | 看新打印的三个数：头 dword 不是 `0xffffffff` ⇒ 写头的循环没跑；被 stamp 的 TXBB 数 < `sqNent` ⇒ 只盖住了头；stamp 之外有非零字节 ⇒ **有人投递了东西**（本阶段不该发生） |

---

## 9. 回滚

阶段 7 是**加法**（在 general 与 INIT_PORT 之间插一条命令；C12 是同一批的两处小修）。

1. **代码回滚**（本批将提交成一条提交）：

   ```bash
   git -C /Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4 revert --no-edit <该提交>
   ```

   然后重建、重装、重启。构建产物写在 `/tmp` 下的临时目录里，**没有覆盖
   `outputs/20261010-cx3-m1/build`**，所以仓库里没有需要清理的中间产物。
2. **只回滚 C12**：把 `Mlx4Hw::sqCreate` 里那段整环 stamp 的循环去掉、把
   `sqStampMatches` 的比较值改回 `1 << MLX4_STAMP_SHIFT`、把 verifier 那一行改回原措辞。
   不建议单独回滚 —— 只有 C12 会让阶段 9 的回收扫描读到全零（"全在飞行中"），
   那时会像"硬件不释放槽位"而不是像"我们写错了初值"。
3. **运行期退路**：`sudo tools/cx3-mlx4/rollback.sh [--purge]`（未改动）。
4. **硬件侧残留**：设备里留下的是"固件被告知 base QPN = 64"。它**不改变链路状态**，
   也不需要清理：验证链以 `reset` 开头，HCA reset 之后固件那一侧的映射本来就没了
   （下一次 `m3portup` 会重新告诉它）。

---

## 10. 用户重启后要跑的命令与期望输出（**预测**）

```bash
sudo tools/cx3-mlx4/verify.sh stage      # 构建 + 校验 + 安装（不重启）
sudo reboot
sudo tools/cx3-mlx4/verify.sh run        # 重启后：整条链
```

只想单独看这一阶段（前提：同一 boot 里已跑过 `m2dprofile`、`m2dbringup`、`m3equp`、
`m3clrint`、`m3irqup`）：

```bash
sudo /usr/local/bin/cx3ctl m3portup 1
sudo /usr/local/bin/cx3ctl r4tx          # 看 C12 的新判据
```

### 10.1 `verify.sh run` 的期望变化

`kPortUpOutStep` 现在应当报 **5**（旧口径下是 4），端口那一段多出**一行 PASS**，
于是 PASS 总数从 47 变成 **48**（0 FAIL / 1 BLOCKED / 1 SKIP 的构成不变；
那 1 BLOCKED 是 M4 的 soak、1 SKIP 是 PORT_CHANGE 事件，都与本阶段无关）。

**新判据那一行的样子（成功分支）**：

```
port 1 qpn_calc: fw status 0x0 (the firmware now maps MAC index i to base+i)     PASS
```

它**读起来与另外三种情况不同**（这是方案 §6.1 的要求）：

```
port 1 qpn_calc  FAIL  fw status 0x3 -- the firmware REFUSED the command. If it is 0x3
                       (BAD_PARAM) or 0x5 (BAD_RESOURCE) then the firmware does check that
                       the base QPN lands in a region of its own, which is stage 7's written
                       falsification condition ...
port 1 qpn_calc  FAIL  step 3 of 5 says the qpn_calc step ran, but its fw status is still
                       the NOT-POSTED sentinel (4294967294 = 0xfffffffe): the driver asked
                       the device and did not record the answer ...
port 1 qpn_calc  SKIP  NOT REACHED: the port-up chain stopped at step 2 of 5, before the
                       qpn_calc step 3, so the command was never sent ...
```

同时 `r4tx` 那两行 SQ 判据的第二行换成新的断言（措辞变了，行数不变）：

```
TX SQ: every 64-byte TXBB's first dword reads back as 0xffffffff (STAMP_VAL | 1 << 31,
       'software owns'), out of the memory itself                                    PASS
```

### 10.2 `cx3ctl m3portup 1` 应当长这样（数字来自本卡已实测的值）

```
  stage reached      : 5 of 5  (1 gate, 2 general, 3 qpn_calc, 4 INIT_PORT, 5 all done)
  gate QUERY_PORT    : fw status 0x0
  supported types    : 2  (2 = Ethernet)
  link bit before    : 1
  SET_PORT general   : fw status 0x0   (mtu sent: 1522)
  SET_PORT qpn_calc  : fw status 0x0
  INIT_PORT          : fw status 0x0
  link bit after     : 1
  speed code after   : 0x1 -> 10000 Mb/s
```

内核日志里应当能看到（同一次调用）：

```
m3portup: qpn_calc base QPN 64 comes from the rule the RX QP will take its number from,
          because the RX QP does not exist yet in this boot (m3portup runs before r4qp);
          the firmware's reserved share (fDevCap.reserved_qps) is 64 and this profile
          configured 1024 QPs
m3portup: caps.flags 0xeeff1f9df677fbef -> VEP_MC_STEER (bit 42) set -> m_promisc 1
          (MCAST_DIRECT); promisc 0, and n_mac 7 is QUERY_PORT's log_max_macs
          (1 << 7 = 128 MAC-derived QPNs)
setPortQpnCalc: port 1: in_param = the mailbox 0x..., in_modifier 0x101 (RQP_CALC 1 -- the
          SAME value as ETH_OPCODE, different field -- << 8 | port 1), op_modifier 1 (ETH),
          timeout class B. Context byte 0x00..0x1b: 00 00 00 40 00 07 00 00 00 00 00 00 00
          00 00 01 00 00 00 00 00 00 00 40 40 00 00 40
m3portup: qpn_calc accepted (fw status 0x0): the firmware's A0 mapping is now index i ->
          64 + i. MAC table slot 0 (the address stage 5 registered) is QPN 64; slots 1..127
          reach 65..191, which is inside this driver's own numbering ...
```

（context 那串 28 个字节的含义，逐段核对：`+0x00` = `00000040` 即 base **64**；
`+0x04` = `00 07` 即 reserved 0 与 `n_mac` **7**（幂值）；`+0x06..+0x0b` 全 0
（`n_vlan`/`n_prio`/`reserved2[3]`/`mac_miss`）；`+0x0c..+0x0f` = `00 00 00 01`
即 `intra_no_vlan 0 / no_vlan 0 / intra_vlan_miss 0 / **vlan_miss 1**`；
`+0x10..+0x13` 全 0（`reserved3[3]`/`no_vlan_prio`）；`+0x14` = `00000040` 即
promisc 位 0 + base 64；`+0x18` = `40000040` 即 `MCAST_DIRECT(1) << 30 | 64`。）

### 10.3 `cx3ctl r4tx` 的 SQ 段应当长这样

```
    initial ownership    : every 64-byte unit's first dword = 0xffffffff
                           (STAMP_VAL | owner 1 << 31 = 'software owns this
                           slot', mlx4_en_netdev.c:1373-1375). Zero there would say
                           bit 31 = 0 = 'hardware owns', i.e. every slot in flight
    the 64-byte header   : first dword reads back 0xffffffff (as written; 0xffffffff wanted)
    the WQE TXBBs        : 256 of 256 stamped -- every one of them
    the WQE array        : 0 non-zero byte(s) outside the ownership stamps -- this
                           stage posts nothing, so 0 is the expected answer ...
```

若第二行读回 **`0x00000000`**（阶段 6 的输出）：那是**旧 kext**，或者 C12 的修法没生效 ——
它读起来像"数组是干净的"，实际含义是"硬件拥有每一个槽位"。
