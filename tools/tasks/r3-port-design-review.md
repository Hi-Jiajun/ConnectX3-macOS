任务：审查一个网卡驱动移植项目的 **R3 阶段设计**（把端口拉成以太网并让链路 up），
以及一个**环形初始化 A/B 实验**的设计。请逐条挑错，特别是偏移、命令编号、顺序和安全性。

## 项目背景

把 **BSD 许可**（OpenIB.org BSD 分支）的 Mellanox 驱动从 FreeBSD `sys/dev/mlx4` 移植到
macOS IOKit kext。硬件 Mellanox ConnectX-3 Pro（`15b3:1007`，板卡 MCX354A-FCCT，
**双口 QSFP+ 以太网卡**），目标机 macOS 15.8.1 x86_64。

已完成：固件启动（MAP_FA / RUN_FW / MOD_STAT_CFG / QUERY_DEV_CAP / SET_ICM_SIZE /
MAP_ICM_AUX / 14 张上下文表 / **INIT_HCA 全部固件状态 0**）；事件队列 EQ 已建好
（EQN 28、512 槽、64 字节 EQE、MTT offset 64，SW2HW_EQ + MAP_EQ 都成功），
**轮询与中断两条路都验证过**（1031 个事件、两次以上相位回绕、token 匹配、清除寄存器在
BAR0+0xf0058、mask 0x00000080）。

## 关键新证据（来自一张 Linux 诊断 U 盘的实测记录，决定性的）

同一张卡在 Linux 6.6 上（`mlx4_core` + `mlx4_en` 正常加载）：

```
mlx4_core: Initializing 0000:07:00.0
mlx4_core 0000:07:00.0: DMFS high rate steer mode is: disabled performance optimized steering
mlx4_en 0000:07:00.0: Activating port:1
mlx4_en: Port 1: Using 16 TX rings / 16 RX rings / Initializing port
mlx4_en: Activating port:2 ... Port 2: Initializing port
mlx4_en: eth0: Link Up
```

同一台机上的 `ip link` 显示 `eth0` / `eth1` 的 MAC 为 `00:11:22:33:44:55` / `00:11:22:33:44:56`
（与卡上 NVRAM 里读出的默认 MAC 一致）。PCI 类码是 `0x020000`（Ethernet）。

本移植从 `QUERY_FW` 读到：**固件版本 2.42.5000、`cmd_if_rev = 3`、max_cmds 16**。
按参考 `mlx4_fw.c:1671`，`cmd_if_rev < MLX4_COMMAND_INTERFACE_NEW_PORT_CMDS(=3)` 才置
`MLX4_FLAG_OLD_PORT_CMDS`；本卡**等于 3，所以走新端口命令接口**。

⇒ 结论：这张卡是**以太网 SKU**、固件已把端口配成以太网、不需要刷固件、没有 VPI 模式歧义。

## 允许阅读的路径（只读这两处）

```
/Users/you/Documents/Hackintosh-MacLocal/tools/mlx4-port/freebsd-src/   # 参考实现
/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/            # 移植代码
```

## 我从参考里读出的 R3 设计（**请逐项核对，找错**）

### 命令编号（`cmd.h`、`mlx4_core/mlx4_port.c`）

| 命令 | 值 | in_param | in_modifier | op_modifier | 输入 mailbox |
| --- | --- | --- | --- | --- | --- |
| QUERY_PORT | `0x43` | 0 | `port` | 0 | 无（输出用 mailbox） |
| SENSE_PORT | `0x4d` | 0 | `port` | 0 | 无（immediate 输出） |
| CLOSE_PORT | `0xa` | 0 | `port` | 0 | 无 |
| SET_PORT（Ethernet general） | `0xc` | 0 | `MLX4_SET_PORT_GENERAL << 8 \| port` = `port` | `MLX4_SET_PORT_ETH_OPCODE` = `1` | 有 |
| INIT_PORT（新接口） | `0x9` | 0 | `port` | 0 | 无 |

### QUERY_PORT 输出解析（`mlx4_fw.c:1202-1236` 的新接口分支）

| 偏移 | 宽度 | 字段 |
| --- | --- | --- |
| `0x00` | u8 | bit7 = link_state；bits1:0 = supported_port_types（1=IB、2=ETH、3=两者）；bit3 = suggested_type；bit4 = default_sense；bit5 = dmfs_optimized_state |
| `0x01` | u8 | ib_mtu（低 4 位） |
| `0x02` | BE16 | eth_mtu |
| `0x06` | u8 | max_port_width（低 4 位） |
| `0x07` | u8 | max_gids = 1 << (>>4)；max_pkeys = 1 << (&0xf) |
| `0x0a` | u8 | log_max_macs（低4）；log_max_vlans（高4） |
| `0x0b` | u8 | max_vl（低4）；max_tc_eth（高4） |
| `0x10` | BE64 | def_mac |
| `0x18` | BE32 | trans_type（>>24）；vendor_oui（&0xffffff） |
| `0x1c` | u32 | wavelength |
| `0x20` | u32 | trans_code |

Ethernet 语义（`mlx4_en/en_port.h:89-102` 的 `mlx4_en_query_port_context`，
确认它和上面的哑元布局是同一块内存）：

| 偏移 | 字段 |
| --- | --- |
| `0x00` | link_up：bit7 = `MLX4_EN_LINK_UP_MASK` 0x80；bit6 = ANC 0x40 |
| `0x01` | autoneg：bit7 = 0x80 |
| `0x02` | BE16 mtu |
| `0x05` | link_speed：掩码 `0x6f`；**40G = 0x40，56G = 0x20** |
| `0x10` | BE64 mac |
| `0x18` | transceiver |

### SET_PORT Ethernet general 输入 mailbox（`mlx4.h:777-792`）

| 偏移 | 宽度 | 字段 | 我要填的值 |
| --- | --- | --- | --- |
| `0x00` | u16 | reserved1 | 0 |
| `0x02` | u8 | v_ignore_fcs | 0 |
| `0x03` | u8 | flags | `SET_PORT_GEN_ALL_VALID` = `0x7` |
| `0x04` | u8 | roce_mode | 0（除非设备能力 `MLX4_DEV_CAP_FLAG2_ROCE_V1_V2`） |
| `0x05` | u8 | reserved2 | 0 |
| `0x06` | BE16 | mtu | 1500 + 14 + 4 = **1518**（参考用 `接口MTU + ETH_HLEN + VLAN_HLEN + ETH_FCS_LEN`） |
| `0x08` | u8 | pptx | 0（初次先不启用 pause） |
| `0x09` | u8 | pfctx | 0 |
| `0x0c` | u8 | pprx | 0 |
| `0x0d` | u8 | pfcrx | 0 |

（`mlx4_port.c:1560-1567`：`pptx = (pptx * !pfctx) << 7`，`pprx = (pprx * !pfcrx) << 7`。）

### 我打算执行的顺序

1. `QUERY_PORT` port1、port2（只读）→ 打印 raw mailbox 全部 64 字节 + 上表所有字段
2. **能力门禁**：`supported_port_types` 不含 ETH 就停，不发任何写命令
3. `CLOSE_PORT`（如果端口当前是 active 的）
4. `SET_PORT` general（ETH opcode），mtu 1518、pause 全 0
5. `INIT_PORT`（新接口、无 mailbox）
6. 再 `QUERY_PORT` → 读 byte0 bit7 判链路 up、byte5 读速率

## 请你回答

**A. 逐项核对上表**：命令编号、in_param/in_modifier/op_modifier 的组合、每个偏移与宽度、
BE/LE、以及 SET_PORT general 的字段顺序。**发现任何一处与我列的源码不符就指出，给 file:line。**
特别是：`v_ignore_fcs` 与 `flags` 的顺序对不对（这个结构体前面有 `u16 reserved1`，
所以 flags 在 `0x03` 而不是 `0x02`，我这样理解对吗）。

**B. 顺序**：`CLOSE_PORT` 是不是必需？参考在"端口类型切换"时发它，但我们只是要把一个新
reset 过的 HCA 的端口启用起来——参考的正常 probe 路径里有没有发 CLOSE_PORT？
如果不发会怎样？`SET_PORT_general` 必须在 `INIT_PORT` 之前吗（参考的调用顺序是什么）？
`qpn_calc` 不做会不会影响"链路 up"本身（我理解它只影响收包定向）？

**C. 这一段的成功判据**：HCR 完成 ≠ 链路 up。请给出"链路 up"的**最小充分判据**，
以及从 `QUERY_PORT` 的哪个字节读。如果端口没插线会看到什么（`link_state`=0 是"没插线"
还是"没启用"？怎么区分这两种失败）。

**D. 安全性**：这一步有没有可能把卡弄成需要重刷固件才能恢复的状态？请**按源码给结论**，
不要泛泛而谈。哪些参数填错会有不可恢复的后果？（我关心的是：`in_modifier` 或
`op_modifier` 写错会不会被固件解释成 IB 命令或别的子命令。）

**E. A/B 环形初始化实验的设计**：背景——本移植观察到"把 EQ 环交给 `SW2HW_EQ` 之后，
环内存的内容会变（`memset` 后为 0、`SW2HW_EQ` 之前为 0、之后出现 `0xcc`），耗时约 238 ms"。
这只是**相关**，不是因果。我打算这样验：

1. 分配两个**同规格**的环 A、B（同样的 nent / EQE 尺寸 / MTT 登记方式），都 `memset` 为 0
2. 采样两环全部页（32 KiB）→ 基线（每个环记录：非零字节数、每页前若干字节的摘要）
3. **只**对 A 发 `SW2HW_EQ`，在若干时间点（0/10/50/100/200/400 ms）对两环采样
4. 对 A 发 `HW2SW_EQ`，然后**交换**：只对 B 发 `SW2HW_EQ`，再采样两环
5. 报告：变化是否只跟着"被交给 `SW2HW_EQ` 的那个环"走、以及内容何时稳定

请审这个设计：(a) 有没有更省事但同样有说服力的做法；(b) 采样时机与采样方式有没有会让
结论站不住的地方（例如"稳定"的判据）；(c) 交换那一步有没有风险（B 环没有 MAP_EQ，
固件会不会因为 EQ 上下文里的 MTT 指向一个未映射的环而做坏事）；(d) 如果结果是
"两个环都变了"或"两个都没变"，分别说明什么。

**F. 关于我**把这个实验和 R3 的读命令（`QUERY_PORT`）放进**同一个内核构建**（一次重启）**的方案**：
是应该把 R3 的**写**命令（`SET_PORT`/`INIT_PORT`）也放进同一个构建、用独立的 selector
按顺序手动调用（先只读、看清楚了再写），还是分两次重启更稳妥？给你的取舍与理由。

回答用中文。**逐条回答 A–F**，区分"源码可确证"与"你的推断"，不确定就写 "unknown"。
不要复述背景。
