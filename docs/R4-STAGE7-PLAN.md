# R4 阶段 7（`r4A0Port`）：把 `qpn_calc` 插进端口 bring-up

实现前的方案。编码带 `vendor/freebsd-src` 行号；本卡实测值标出来源；待决定的事在第 5 节。

---

## 1. 这一阶段小得出人意料：补一条命令

我们的 `m3portup` 现在是**两条**命令（`src/Cx3Mlx4.cpp:4288` → `:4299`）：

```
setPortGeneralEth(...)      SET_PORT general
initPort(...)               INIT_PORT
```

参考的正常端口激活是**三条**（`mlx4_en_netdev.c:1388-1403`）：

```
SET_PORT_general(...)                              :1388
mlx4_SET_PORT_qpn_calc(dev, port, base_qpn, 0)     :1394   <- 我们缺的就是这条
INIT_PORT(...)                                     :1403
```

**所以阶段 7 = 在 general 与 INIT_PORT 之间插入 `qpn_calc`。** 位置不是可以随便放的：
它是"告诉固件 QPN 基址"的动作，必须在端口被 initialize 之前。

链路已经 up 了（阶段 4 时 `link_speed 0x01` = 10 Gb/s），所以这一阶段**不改变"链路是否 up"**，
它改变的是**进来的帧落到哪个 QPN**。

### 1.1 插入点与"步骤计数"（实现时会咬人的一处细节）

`exportM3PortUp` 的现有结构（`src/Cx3Mlx4.cpp`）：

| 行 | 内容 | `kPortUpOutStep` |
| --- | --- | --- |
| 4270 附近 | 门的 `QUERY_PORT` | （通过即 1） |
| **4386** | `setPortGeneralEth(...)` | `:4388` 失败 → **2**；`:4394` 成功 → **2** |
| **4397** | `initPort(...)`（注释写着 `/* 3. INIT_PORT */`） | `:4399` 失败 → **3**；`:4405` 成功 → **4** |

所以"step"的语义是**走到第几小步**，上限 **4**：1=门、2=general、3=INIT_PORT 尝试、4=全成。

**插入 `qpn_calc` 后这条链变成 5 小步，因此**：
* `kPortUpOutStep` 的上限从 4 变 **5**（新顺序：1=门、2=general、**3=qpn_calc**、4=INIT_PORT 尝试、5=全成）；
* `tools/cx3ctl.c:3035` 硬写着 `"stage reached : %llu of 4"` —— **要改成 5**，
  否则工具会永远说"of 4"而设备报 5（一条自相矛盾的输出，本项目的 F 家族）；
* `src/cx3_abi.h:955` 那条注释（`0 = gated before any write`）要补上新的含义；
* `tools/verify.sh` **不检查这个值**（只在 `:604` 读它、不判它），所以改它不会破坏任何断言——
  但也正因为没人判它，它是"打印出来但没人验"的那类数字，改的时候要顺手确认它还与实现一致。

## 2. 命令编码（已核）

| 项 | 值 | 依据 |
| --- | --- | --- |
| opcode | `MLX4_CMD_SET_PORT = 0xc` | `cmd.h:63` |
| `op_mod` | `MLX4_SET_PORT_ETH_OPCODE = 0x1` | `cmd.h:195` |
| `in_modifier` | `MLX4_SET_PORT_RQP_CALC << 8 \| port` = **`0x101`** | `cmd.h:202`、`mlx4_port.c:1606` |
| `in_param` | **mailbox 的 DMA 地址**（context 在 mailbox `+0`，与 MAC 表那条一样） | `mlx4_port.c:1591-1594` |
| 超时 | `MLX4_CMD_TIME_CLASS_B` | `mlx4_port.c:1607` |

**注意 `op_mod` 与 `in_modifier` 是两组数值撞车的枚举**（`ETH_OPCODE = 0x1` 是 op_mod，
`RQP_CALC = 0x1` 是 in_modifier 的高字节）——这一条已经在 `src/mlx4_regs.h` 里按 E9 加了注释，
实现时照用。附带好处：本阶段正好是**第一次同时用到这两组**的代码，E9 那条注释现在有了用例。

## 3. context 字段（`mlx4.h:798-813`，从 mailbox `+0` 起）

参考的 `mlx4_SET_PORT_qpn_calc`（`mlx4_port.c:1578-1612`）**只填了其中一部分**，其余靠
`mlx4_alloc_cmd_mailbox` 的零缓冲——这一点必须照抄，不要"顺手都填上"。

| 偏移 | 字段 | 参考填什么 | 我们要填 |
| --- | --- | --- | --- |
| `0x00` | `base_qpn` (BE32) | 参数 | **我们的 RX QPN**（第 5 节决策①） |
| `0x04` | `reserved` (u8) | 0 | 0 |
| `0x05` | `n_mac` (u8) | `dev->caps.log_num_macs` | **`QUERY_PORT` 的 `logMaxMacs`**（本卡实测 **7**） |
| `0x06` | `n_vlan` (u8) | **不赋值 ⇒ 0** | 0（照抄） |
| `0x07` | `n_prio` (u8) | **不赋值 ⇒ 0** | 0（照抄） |
| `0x08` | `reserved2[3]` | 0 | 0 |
| `0x0b` | `mac_miss` (u8) | 0 | 0 |
| `0x0c` | `intra_no_vlan` (u8) | **= 0**（显式） | 0 |
| `0x0d` | `no_vlan` (u8) | `MLX4_NO_VLAN_IDX = 0` | 0 |
| `0x0e` | `intra_vlan_miss` (u8) | **= 0**（显式） | 0 |
| `0x0f` | `vlan_miss` (u8) | **`MLX4_VLAN_MISS_IDX = 1`** | **1**（注意这个不是 0） |
| `0x10` | `reserved3[3]` | 0 | 0 |
| `0x13` | `no_vlan_prio` (u8) | 0 | 0 |
| `0x14` | `promisc` (BE32) | `promisc << 31 \| base_qpn` | **`0 << 31 \| base_qpn`**（第 5 节决策②） |
| `0x18` | `mcast` (BE32) | `m_promisc << 30 \| base_qpn` | **`MCAST_DIRECT(1) << 30 \| base_qpn`**（见下，**实测**） |

`MLX4_NO_VLAN_IDX = 0`、`MLX4_VLAN_MISS_IDX = 1`（`device.h:424-425`）；
`SET_PORT_PROMISC_SHIFT = 31`、`SET_PORT_MC_PROMISC_SHIFT = 30`（`mlx4_en/en_port.h:39-40`）；
`MCAST_DEFAULT = 2`（`mlx4.h:774`）。

**`m_promisc` 的取值**（`mlx4_port.c:1585-1586`）：
`caps.flags & MLX4_DEV_CAP_FLAG_VEP_MC_STEER ? MCAST_DIRECT(1) : MCAST_DEFAULT(2)`。

**★ 本卡的这一位是 1（实测，写方案后补的）。** `MLX4_DEV_CAP_FLAG_VEP_MC_STEER = 1LL << 42`
（`device.h:182`），而我们的 `caps.flags = ext_flags << 32 | flags`（`mlx4_pure.h:369-370`，
与参考 `mlx4_fw.c:928-930` 逐字一致）——**所以这一位就在我们早就读到并记录过的那两个 dword 里**：

```
ext_flags(0x40) = 0xeeff1f9d   (两次独立读取逐位相同)
flags    (0x44) = 0xf677fbef
caps.flags      = 0xeeff1f9df677fbef
bit 42 (VEP_MC_STEER) = 1        <-- MCAST_DIRECT 分支
bit 34 (FCS_KEEP)     = 1        (阶段 4 已用)
```

⇒ **`m_promisc = MCAST_DIRECT = 1`**，实现时必须这样填，并且**从设备读这一位**而不是写死。
若按"没解码所以取假分支"写成 2，多播的落点就与参考不同——而**症状要到阶段 9 才显形**
（多播/广播不来），归因代价高得多。

**这一处是"能力位其实早就读到了、只是没人看"的第三次**（`log_max_macs`、`FCS_KEEP`、现在是
`VEP_MC_STEER`）——三者都在我们已经读到的数据里。规律记在这里：**要某个能力位之前，
先查 `caps.flags`/`flags2`/`QUERY_PORT` 的原始 dword，它多半已经在手上。**

## 4. `base_qpn` 为什么要告诉我们，以及本卡的正确值

见 `docs/M2B-NOTES.md` **§41**（完整推演）。结论三句：

1. `qpn_calc` 是**我们告诉固件基址是什么**（`mlx4_port.c:1595`）——**固件不校验**那个 base
   落在某个 region 里。参考用"从 `num_qps` 顶部往下切 region"的算术，是为了让**主机驱动自己**
   别把 QP 放得和 MAC 派生段撞车，而不是固件的硬性要求。
2. 本卡的 region 计数（`(1<<7)*(1<<7)*2 = 32768` 等）**比我们请求的 1024 个 QP 大 128 倍**，
   所以在我们的布局里 A0 的 ETH_ADDR region **根本不存在**（复算得 base = −97280）。
   参考没事只因为它请求 `1<<18` 个 QP。
3. 于是唯一自洽的选择就是"我们自己的 QPN"，而 `n_mac = 128` ⇒ MAC 索引 0..127 映射到
   `base_qpn + 0..127`。

## 5. 需要你决定的两件事

### 决策①：`base_qpn` 用 64（我们的 RX QPN），还是 896（顶部 128 槽）？

| | base = **64**（阶段 4 的 RX QPN） | base = **896** = `num_qps − (1<<log_max_macs)` |
| --- | --- | --- |
| MAC 索引 0 → | **64 = 我们的 RX QP** ✓ | 896 = 还不是任何 QP ✗（需要把 RX QP 搬过去） |
| MAC 派生段 | 64..191 | 896..1023（空间顶部） |
| 与我们的 QP 相撞？ | **索引 ≥ 1 会撞 65（入口 QP）** | **永不撞** |
| 代价 | 无 | 要改阶段 4 的 QPN 选择 + 一次重启 |

**建议：先用 64。** 理由：阶段 5 只用 MAC **槽 0**，而槽 0 映射到 64 —— 自洽；
把它改成 896 意味着同时搬 RX QP 与入口 QP，而**帧能不能到还没验证过**。
先让 64 这条路把阶段 9 跑通（那才是"证明帧能到"），再决定要不要为了"MAC 表能用满 128 槽"
去搬 QPN。**代价写明**：在 base = 64 下，**MAC 表的索引 ≥ 1 不可用**（会撞入口 QP），
所以本阶段只支持一个 MAC 地址——这对阶段 10 的"一个接口一个 MAC"是够的。

### 决策②：`promisc` 用 0 还是 1？

**建议：0**，即**与参考的正常激活路径逐字一致**（`mlx4_en_netdev.c:1394` 的第四个参数是 0；
`:781-785` 那条 `promisc=1` 只在协议栈开启混杂模式时走）。而 `promisc = 0` 恰好覆盖我们要的：
单播命中 MAC 槽 0 ⇒ QPN 64；广播/多播走 `mcast` 的 `base_qpn` ⇒ 同一个 QPN 64。

**所以 ARP 广播不需要 promisc=1。** 若阶段 9 发现广播不来，第一个要试的才是把它置 1
（那时候它就是一个有假设的实验，而不是常态）。

## 6. 怎么验证

这一阶段的**直接**判据只有"命令被接受 + 端口仍 up"；**真正的判据在阶段 9**（帧是否落到 QPN 64）。
所以验证要分两层，并且**把第二层明确标为未测**：

| 层 | 期望 | 为什么它算证据 |
| --- | --- | --- |
| `qpn_calc` | `fw status 0x0` | 命令"发出去了"与"被接受"是两件事（阶段 4 抓过一次） |
| general → qpn_calc → INIT_PORT **三条都成功** | 顺序对且互不破坏 | 参考的顺序是这三条；中间插一条若破坏了 INIT_PORT 的前提，会在这里显形 |
| 端口仍 up | `QUERY_PORT` 的 link_state / speed 与插命令前一致 | 证明这条命令没把已建立的链路搞掉 |
| 回读 | `QUERY_PORT` 能答、`def_mac` 不变 | 顺带确认端口状态未被扰动 |
| **帧落到 QPN 64** | **未测（阶段 9）** | 本阶段**不做**这个断言 |

**失败分岔**：
* `qpn_calc` 回 `BAD_PARAM(0x3)` 或 `BAD_RESOURCE(0x5)` ⇒ **固件确实校验 base 落在某个 region 里**
  ⇒ P 不成立，直接跳到决策①的第二列（base = 896 且搬 RX QP）。这是**写下来的证伪条件**。
* `qpn_calc` 成功但 `INIT_PORT` 失败 ⇒ 插错了位置（顺序问题，不是参数问题）。
* 三条都成功但阶段 9 没有帧 ⇒ 回到 `mcast` 的 `MCAST_DIRECT` 取值与 `counter_index`。

### 6.1 ABI 与验证链要跟着动的三处（**别漏，否则"三条都成功"这条判据无法验证**）

现在 `Cx3PortUpResult` 只有两个命令的状态：`kPortUpOutSetFwStatus`（general）与
`kPortUpOutInitFwStatus`（INIT_PORT）。插进第三条后，若不给它**自己的**标量，
验证链就只能验证"两条 + 一条没人看的"，而 §6 的第一条判据正是"三条都成功"。所以要：

1. `src/cx3_abi.h`：新增 `kPortUpOutQpnCalcFwStatus`。**注意 `CX3_SCALAR_OUT_MAX = 16` 的上限**
   （`cx3_abi.h` 里已有静态断言）——先数一遍 `kPortUpOut*` 现在用了几个，超了要合并而不是硬塞；
2. `src/Cx3Mlx4.cpp`：在 `qpn_calc` 的成功/失败两处写它，失败分支里**停止链**
   （与 general 失败同样的处理：不继续 INIT_PORT——"基址没告诉固件"时 initialize 端口没有意义）；
3. `tools/verify.sh`：在这一步读它，措辞要能区分三种情况——
   **"被拒绝"**（`fw status` 是具体错误码）、**"调用了但没记录"**（哨兵值）、
   **"根本没走到"**（`kPortUpOutStep` 小于那一步）。三者读起来必须不同——
   这正是"失败被表示成非失败"那一家族。

## 7. 顺带要记的一件事：阶段 8 可能被本阶段吸收

计划里阶段 8 是"RX 过滤"（注册广播/多播）。但在 A0 下
`mlx4_multicast_attach` **直接 `return 0` 什么都不做**（`mlx4_mcg.c:1412-1415`，已在阶段 5 确认），
而广播/多播的落点由 `qpn_calc` 的 `mcast` 字段**现在就决定了**（指向 `base_qpn`）。

**所以阶段 8 很可能只剩"禁用 MC filter 或装入广播"这一条命令，甚至无事可做。**
这一点等阶段 9 的实测（ARP 广播能不能产生有效 RX CQE）来判，不要提前写结论。

## 8. 实测 / 推断 / 未测（写方案时）

**实测**：`log_max_macs = 7`（`QUERY_PORT` outbox `0x0a = 0x77` 低半字节）；
本卡 profile 请求 1024 个 QP；我们的 `m3portup` 目前只有 general + INIT_PORT 两条；
端口 link up 10 Gb/s（阶段 4/5 的运行日志）。

**实测（写方案后补的两条，都来自已经在手的数据）**：
* `caps.flags` bit 42 `VEP_MC_STEER` = **1** ⇒ `m_promisc = MCAST_DIRECT`（推导见第 3 节）；
* `caps.flags` bit 34 `FCS_KEEP` = 1（阶段 4 已用，此处一并列出以免分散在两个文件里找）。

**推断（源码级依据，未上机）**：上面全部编码；`promisc = 0` + `mcast = MCAST_DIRECT | base_qpn`
能让单播与广播都落到 QPN 64。

**未测**：一行代码未写。**"帧真的落到 QPN 64"是本阶段无法验证的**（要阶段 9）。

## 9. 回滚

阶段 7 是**加法**（在 general 与 INIT_PORT 之间插一条命令 + 一个新选择器/开关）。
回滚 = `git -C tools/cx3-mlx4 revert --no-edit <该提交>` 后重构建重启；
运行期退路 `sudo tools/cx3-mlx4/rollback.sh [--purge]`。
硬件里留下的状态（固件被告知了 base_qpn）会在下一次 `reset` 清掉——验证链本来就以 `reset` 开头。
