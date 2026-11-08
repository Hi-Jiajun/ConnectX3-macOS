# M5-REFERENCE —— Linux `mlx4_en` 对六项"真网卡功能"到底做了什么

> 用途：M5（校验和卸载 / VLAN / jumbo / 多队列 RSS / 中断调节 / 媒体链路）动手前，
> 照抄这份，而不是自己发明。每一条事实都带 `path:line`。
> 与 `docs/M5-PLAN.md` 配套：那份写"我们要什么 / 怎么算过"，这份写"参考实现怎么做"。

## 0. 来源与引用约定

本文件里出现的路径缩写：

| 缩写 | 展开 |
| --- | --- |
| `mlx4_en/…` | `/Users/you/Documents/Hackintosh-MacLocal/tools/mlx4-port/freebsd-src/mlx4_en/…` |
| `qp.h` `cq.h` `device.h` `cmd.h` `doorbell.h` | `/Users/you/Documents/Hackintosh-MacLocal/tools/mlx4-port/freebsd-src/` 下的同名头 |
| `mlx4_core/…` | `/Users/you/Documents/Hackintosh-MacLocal/tools/mlx4-port/freebsd-src/mlx4_core/…` |
| `mlx4_regs.h` | `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/mlx4_regs.h`（**本项目自己的转写**） |

重要提醒（影响读法）：`mlx4_en/` 这一份**不是原味 Linux 源码，而是 FreeBSD 移植版**
（`if_t dev` 取代 `net_device`，`mbuf` 取代 `skb`，见 `mlx4_en/en.h:48-64`）。
硬件侧的字段、位、命令与 Linux 版同名同值；**软件侧**的对应关系是
`skb->ip_summed == CHECKSUM_UNNECESSARY/PARTIAL` ↔ FreeBSD 的
`mb->m_pkthdr.csum_flags`（`mlx4_en_rx.c:815`、`mlx4_en_tx.c:697`）。
下文凡涉及这一层，都会写明"这是 FreeBSD 侧的表达"。

常量来源标注：**`[SRC]`** = 来自 `mlx4-port` 源码树（`en.h`/`qp.h`/`cq.h`/`device.h`/`cmd.h`/`mlx4_core`）；
**`[PORT]`** = 本项目已在 `mlx4_regs.h` 里转写过。见附录 A。

---

## 0.5 动手之前的前置条件（这些不满足，六项里没有一项能验）

1. **M4 必须先过**（`ping` 通、100 包 0 丢）。理由与 `docs/M5-PLAN.md` §0.1 相同：
   下面每一项都改数据通路或端口配置，变量混在一起无法判读。
2. **"停机 → 重开端口"这条路必须先可用**。参考实现里，**改 MTU 必须 stop/start 端口**
   （`mlx4_en/mlx4_en_netdev.c:1839-1840`）；改 ring 大小也是（`:2363-2376`）；
   VLAN 过滤表只在 `port_up` 时才下发（`:419-423`、`:447-451`）。
   也就是 `mlx4_en_stop_port`（`:1453`）/ `mlx4_en_start_port`（`:1261`）这一对
   在本项目里得有等价物，否则 jumbo / VLAN / 多队列都只能靠"重装 kext 重启"来验。
3. **端口的初始化顺序是固定三步**，任何一项改动都落在其中一步之后：
   `SET_PORT_general`（`mlx4_en_netdev.c:1380-1385`）→ `SET_PORT_qpn_calc`（`:1392`）
   → `INIT_PORT`（`:1400`）。**`INIT_PORT` 本身不带任何 offload 标志**
   （实现见 `mlx4_core/mlx4_fw.c:2259-2312`：只有 `port` 一个入参，命令号 `MLX4_CMD_INIT_PORT` 0x9，
   `cmd.h:57`）。**别指望在 INIT_PORT 上打开校验和/VLAN** —— 见第 1 节。
4. **接收侧要能分配更大的 cluster**。jumbo 的落点完全在 RX 缓冲尺寸上
   （`mlx4_en/mlx4_en_rx.c:342-364`），现在的 2048 B 缓冲是硬边界。
5. **多队列需要第二种资源**：不止 RX/TX 环，还要**一个额外的 RSS 定向 QP + 每个环一个 CQ + EQ 向量**
   （`mlx4_en/mlx4_en_rx.c:1049-1077`、`mlx4_en/mlx4_en_cq.c:128-142`）。这是六项里唯一需要新 QP 的。

---

## 1. CHECKSUM OFFLOAD（校验和卸载）

方向要分清：**RX 是"硬件已经算好了，驱动只读结论"；TX 是"驱动在 WQE 上点名要硬件算"**。
两侧完全独立，没有共用的开关。

### 1.1 硬件/固件需要什么

**没有一个"校验和 offload 使能位"**。这一条是本节最重要的事实：

| 位置 | 事实 | 引用 |
| --- | --- | --- |
| `INIT_PORT` | 入参只有 `port`，无标志位 | `mlx4_core/mlx4_fw.c:2259-2312`；调用点 `mlx4_en/mlx4_en_netdev.c:1400`、`:2325` |
| QP context | 与校验和相关的位**一个也没有**；`param3` 只用 bit29(FCS keep) 与 bit30(VLAN，见第 2 节) | `mlx4_en/mlx4_en_resources.c:79-80`；`mlx4_en_rx.c:949-954` |
| `MLX4_EN_*` 标志 | 全部是混杂/DCB 语义，**没有一个与校验和相关** | `mlx4_en/en.h:518-531`（枚举）；用点见 `mlx4_en_netdev.c:746`、`:1044`、`:2204` |
| `MLX4_PORT_*` | **在这些来源里根本不存在**（全树 `grep` 无命中） | —— |

RX 校验和是**硬件无条件计算**的：驱动只是读 CQE 的结论。TX 校验和是**逐 WQE 请求**的。

### 1.2 RX：CQE 里的两个字段 + 三个状态位

`struct mlx4_cqe` 的相关字段（`cq.h:41-60`）：

| 字段 | 偏移 | 用途 | 引用 |
| --- | --- | --- | --- |
| `status`（`__be16`） | 0x10 | 协议类型 + `IPOK` 位 | `cq.h:49`；`[PORT]` `mlx4_regs.h:1260` |
| `checksum`（`__be16`） | 0x1a | **硬件算出的校验和结论** | `cq.h:57`；`[PORT]` `mlx4_regs.h:1265` |
| `badfcs_enc`（u8） | 0x13 | bit4 = `MLX4_CQE_BAD_FCS` | `cq.h:51`、`:135`；`[PORT]` `mlx4_regs.h:1262`、`:1277` |

`status` 的位（`cq.h:122-130`，**注意全树只有 `cq.h` 定义、`en.h` 使用 `[SRC]`**）：

```
MLX4_CQE_STATUS_IPV4    = 1 << 6      MLX4_CQE_STATUS_TCP   = 1 << 10
MLX4_CQE_STATUS_IPV4F   = 1 << 7      MLX4_CQE_STATUS_UDP   = 1 << 11
MLX4_CQE_STATUS_IPV6    = 1 << 8      MLX4_CQE_STATUS_IPOK  = 1 << 12
MLX4_CQE_STATUS_IPV4OPT = 1 << 9
```

### 1.3 RX：驱动怎么判、判完给栈什么

判据是**两个条件的合取**（`mlx4_en/mlx4_en_rx.c:810-813`）：

1. 接口能力里开着 `IFCAP_RXCSUM | IFCAP_RXCSUM_IPV6`；
2. `cqe->status & MLX4_CQE_STATUS_IPOK`；
3. `cqe->checksum == cpu_to_be16(0xffff)`。

三者都成立 → `rx_chksum_good++`，并给 mbuf 打上（`:814-818`）：

```
mb->m_pkthdr.csum_flags = CSUM_IP_CHECKED | CSUM_IP_VALID |
                          CSUM_DATA_VALID | CSUM_PSEUDO_HDR;
mb->m_pkthdr.csum_data  = htons(0xffff);
```

这就是 FreeBSD 侧的 **"`CHECKSUM_UNNECESSARY` 等价物"**：`CSUM_DATA_VALID + csum_data==0xffff`
告诉栈"IP 与 L4 都验过了，别再算"。任一条件不成立 → `csum_flags = 0` 且 `rx_chksum_none++`（`:837-838`）。

**注意 `0xffff` 是"已验证"的哨兵值，不是校验和本身**（`:813`、`:818` 两处都写成 0xffff）。

顺带两条与 IP options 有关的事实，都在 RX 侧：

- `MLX4_CQE_STATUS_IPV4OPT` 用于 **LRO 资格判定**：带 IP options 的帧被排除在 LRO 之外
  （`mlx4_en/en.h:366-388`，`mlx4_en_can_lro` 只认 `IPV4|IPOK|TCP` 或 `IPV6|IPOK|TCP`，`:376-383`）。
- 同一个 `can_lro` 结果决定是否进 `tcp_lro_rx`（`mlx4_en_rx.c:826-831`）。

RX 侧**不做**的事：不重算、不改帧、不因为校验和错而丢包（丢包只看 `invalid_cqe`：
opcode ERROR 与 `MLX4_CQE_BAD_FCS`，`mlx4_en_rx.c:604-621`）。

### 1.4 TX：WQE 的 ctrl 段两个位

TX 的申请位在 `struct mlx4_wqe_ctrl_seg.srcrb_flags`（`qp.h:302-331`；字段语义注释在 `:312-320`）：

| 位 | 常量 | 值 | 引用 |
| --- | --- | --- | --- |
| bit4 | `MLX4_WQE_CTRL_IP_CSUM` | `1 << 4` | `qp.h:294` `[SRC]` |
| bit5 | `MLX4_WQE_CTRL_TCP_UDP_CSUM` | `1 << 5` | `qp.h:295` `[SRC]` |

驱动的映射（`mlx4_en/mlx4_en_tx.c:695-702`），这是 FreeBSD 侧 **`CHECKSUM_PARTIAL` 的对应物**
（栈在 mbuf 的 `csum_flags` 里点名）：

```
srcrb_flags = CTRL_FLAGS;                                   // :695  (CQ_UPDATE|SOLICITED, :635-636)
if (csum_flags & (CSUM_IP | CSUM_TSO))
        srcrb_flags |= MLX4_WQE_CTRL_IP_CSUM;                // :697-698
if (csum_flags & (CSUM_TCP | CSUM_UDP | CSUM_UDP_IPV6 | CSUM_TCP_IPV6 | CSUM_TSO))
        srcrb_flags |= MLX4_WQE_CTRL_TCP_UDP_CSUM;           // :700-702
```

统计：`if (srcrb_flags != CTRL_FLAGS)` 即认为这一次用了 offload，`tx_chksum_offload++` 与
`ring->tx_csum++`（`:705-708`）。

界面层的开关（macOS 侧要对应的是 `IONetworkInterface` 的 offload 能力位）：

| 能力 | 位置 |
| --- | --- |
| `IFCAP_HWCSUM \| IFCAP_HWCSUM_IPV6` | `mlx4_en_netdev.c:2247-2251`（声明） |
| `if_sethwassist(CSUM_TCP\|CSUM_UDP\|CSUM_IP\|CSUM_UDP_IPV6\|CSUM_TCP_IPV6\|CSUM_TSO)` | `:2263-2270` |
| `SIOCSIFCAP` 逐位开关，含"关 txcsum 就顺带关 tso"的一致性检查 | `:2000-2067` |

### 1.5 伪头校验和 / IP options / "compute checksum" 位

**"让硬件算伪头校验和"的那两个位（`MLX4_WQE_CTRL_IIP`=1<<28、`MLX4_WQE_CTRL_ILP`=1<<27，
`qp.h:289-290`）在本参考实现的 TX 路径上从未被设置**（全树只在 `qp.h` 出现）。
也就是说，CX-3 这条路径走的是"栈已经把校验和字段填成伪头值，硬件补完"的经典模型，
驱动不额外点名。

IP options 在 TX 侧的影响只有一处，且只在 LSO 上：`mlx4_en_get_header_size`
用 `ip->ip_hl << 2` 算真实 IP 头长（`mlx4_en/mlx4_en_tx.c:522-530`），
`ip6->ip6_nxt` 只在无扩展头时认 TCP（`:531-537`）；头长算不出来就直接返回 0，
于是 LSO 被拒（`:740-745`，超 `MAX_INLINE` 记 `oversized_packets++` 并 `EINVAL`）。

LSO 的"头长度 + MSS"写在 `struct mlx4_wqe_lso_seg.mss_hdr_size`（`qp.h:367-370`），
驱动填 `(mss << 16) | ihs`（`mlx4_en_tx.c:746`），opcode 为
`MLX4_OPCODE_LSO | MLX4_WQE_CTRL_RR | owner`（`:738-739`）。

### 1.6 统计口径（判读用）

| 计数 | 来源 |
| --- | --- |
| `port_stats.rx_chksum_good` / `rx_chksum_none` | 累加各 RX 环的 `csum_ok`/`csum_none`，`mlx4_en_port.c:211-216`、`:436-443` |
| `port_stats.tx_chksum_offload` | 累加各 TX 环的 `tx_csum`，`mlx4_en_port.c:217-234` |
| 环形字段定义 | `mlx4_en/en.h:301`（`tx_csum`）、`:359-360`（`csum_ok`/`csum_none`） |

**本项目现状**：`mlx4_regs.h:1251-1252` 明确写着"这个阶段读 CQE 的 checksum/status 但**不判**，
本阶段不做 offload" —— 也就是说口径已经落地，只差把 1.3/1.4 的两段逻辑接上。

---

## 2. VLAN

### 2.1 RX：剥标签由 QP context 的一个位控制，标签落在 CQE

**位**：`MLX4_STRIP_VLAN = 1 << 30`，在 QP context 的 `param3`（`qp.h:278-280` `[SRC]`）。
驱动**有条件**地设它（`mlx4_en/mlx4_en_resources.c:79-80`）：

```
if (!(if_getcapabilities(dev) & IFCAP_VLAN_HWCSUM))
        context->param3 |= cpu_to_be32(1 << 30);
```

调用链：`mlx4_en_fill_qp_context`（`:41-81`）← 每个 RX 环的 QP（`mlx4_en_rx.c:945-946`）
和 RSS 定向 QP（`mlx4_en_rx.c:1056-1057`）、以及 TX 环的 QP（`mlx4_en_tx.c:233-234`）。
能力位 `IFCAP_VLAN_HWCSUM` 在 `mlx4_en_netdev.c:2249` 声明。

> **命名与极性的坑（如实标注）**：位名是 `MLX4_STRIP_VLAN`，而代码是在**没有** VLAN 卸载能力时
> 才置位。两种读法（"置位=要求硬件剥标签" vs "置位=禁止剥标签"）在这些来源里**无法判定**：
> 固件对这个位的正确定义不在本源码树内。要定论需要 CX-3 PRM（NDA）或实机 A/B（打标签帧进，
> 看 CQE 的 CVLAN 位与帧内是否残留标签）。
> 本项目 `mlx4_regs.h` **尚未转写**这个位（`QP_CONTEXT_PARAM3_OFFSET` 已在 `:398` 定义，
> 但只文档化了 bit29，`:553`）。

**标签在哪**：CQE 的 `vlan_my_qpn`（偏移 0x00）高位是标志，`sl_vid`（偏移 0x0c）是 tag 本体。

| 项 | 常量/字段 | 引用 |
| --- | --- | --- |
| "本帧带 CVLAN" | `MLX4_CQE_CVLAN_PRESENT_MASK = 1 << 29`（在 `vlan_my_qpn` 里） | `cq.h:90`；`[PORT]` `mlx4_regs.h:1254` |
| "本帧带 SVLAN" | `MLX4_CQE_SVLAN_PRESENT_MASK = 1 << 30`（**驱动未使用**） | `cq.h:91` |
| tag 值 | `sl_vid`（`__be16`，偏移 0x0c） | `cq.h:45`；`[PORT]` `mlx4_regs.h:1258` |
| VID 掩码 | `MLX4_CQE_VID_MASK = 0xfff`（**驱动未使用**） | `cq.h:97` |

**驱动路径**（`mlx4_en/mlx4_en_rx.c:805-809`）：

```
if (be32_to_cpu(cqe->vlan_my_qpn) & MLX4_CQE_CVLAN_PRESENT_MASK) {
        mb->m_pkthdr.ether_vtag = be16_to_cpu(cqe->sl_vid);
        mb->m_flags |= M_VLANTAG;
}
```

即 FreeBSD 侧"标签已剥离、tag 交给栈"的表达。**注意帧长在剥标签之后已经少了 4 字节**，
驱动只从 `byte_cnt` 里减去 FCS（`mlx4_en_rx.c:784-785`，`ring->fcs_del` 见 `:949-954`），
不再调整 VLAN 相关长度。

### 2.2 TX：必须在 WQE 上插标签，帧里**不含**标签

两个字段同时写（`mlx4_en/mlx4_en_tx.c:710-717`）：

```
if (mb->m_flags & M_VLANTAG) {
        tx_desc->ctrl.vlan_tag = cpu_to_be16(mb->m_pkthdr.ether_vtag);   // :712
        tx_desc->ctrl.ins_vlan = MLX4_WQE_CTRL_INS_CVLAN;                // :713
} else {
        tx_desc->ctrl.vlan_tag = 0;  tx_desc->ctrl.ins_vlan = 0;         // :715-716
}
```

| 项 | 值 | 引用 |
| --- | --- | --- |
| `MLX4_WQE_CTRL_INS_CVLAN` | `1 << 6` | `qp.h:296` `[SRC]` |
| `vlan_tag` 字段（ctrl 段内偏移 0x04） | `__be16` | `qp.h:306`；`[PORT]` `mlx4_regs.h:1164` |
| `ins_vlan` 字段（偏移 0x06） | u8 | `qp.h:307`；`[PORT]` `mlx4_regs.h:1165` |

**"驱动要不要自己把 tag 放进帧里？"——不要。** 证据有三：
① 驱动只写 WQE 字段，数据段是 mbuf 原样（`:775-877`），没有任何插入帧字节的代码；
② 蓝焰（blueflame）路径**看到 VLAN 插入就主动放弃**（`:890-891`
`ring->bf_enabled && bf_size <= MAX_BF && (tx_desc->ctrl.ins_vlan != MLX4_WQE_CTRL_INS_CVLAN)`）
—— 这正是"这一步交给硬件"的痕迹；
③ 能力位里 `IFCAP_VLAN_HWTAGGING`（`mlx4_en_netdev.c:2248`）由栈负责把 tag 从帧里拿走。

**位冲突警告**：`MLX4_WQE_CTRL_INS_CVLAN = 1 << 6` 与 `MLX4_WQE_CTRL_RR = 1 << 6` /
`MLX4_WQE_CTRL_FENCE = 1 << 6` **共用同一位**（`qp.h:288`、`:291`、`:296`）。
也就是说 ctrl 段 bit6 是模式相关的：普通 SEND 时是 VLAN 插入，LSO opcode 时是 RR。
实现时不能"顺手"把它当独立位处理。

### 2.3 VLAN 过滤表（第三张表，别漏）

除了"剥/插"，端口还有一张 **VLAN 过滤表**：

| 项 | 值 | 引用 |
| --- | --- | --- |
| 命令 | `MLX4_CMD_SET_VLAN_FLTR` = 0x47 | `cmd.h:141` |
| 邮箱结构 | `struct mlx4_set_vlan_fltr_mbox { __be32 entry[128]; }` | `mlx4_en/en_port.h:44-47` |
| 位图大小 | `VLAN_FLTR_SIZE = 128`（128×32 = 4096 bit，覆盖 12 位 VID 空间） | `en_port.h:44` |
| 下发函数 | `mlx4_SET_VLAN_FLTR()`，逐 entry 拼 32 位、**倒序**写（`for i = 127..0`） | `mlx4_en/mlx4_en_port.c:44-72`（循环 `:59-67`，命令 `:68-69`） |
| 软件侧的位图 | `priv->active_vlans[]`（`unsigned long[VLAN_N_VID]`） | `mlx4_en/en.h:547` |
| 加/删 VID | `set_bit`/`clear_bit` + 重新下发；**仅当 `device_up && port_up`** | `mlx4_en_netdev.c:415-425`、`:441-451` |
| 注册钩子 | `EVENTHANDLER_REGISTER(vlan_config / vlan_unconfig)` | `:2274-2277` |

另外两个常量：`MLX4_EN_NO_VLAN = 0xffff`（`en.h:152`）、VID 合法范围
`VLAN_MIN_VALUE=1` / `VLAN_MAX_VALUE=4094`（`en.h:112-113`）。

---

## 3. JUMBO

### 3.1 MTU 上下限与默认

| 项 | 值 | 引用 |
| --- | --- | --- |
| 下限 | `MLX4_EN_MIN_MTU = 46` | `mlx4_en/en.h:188` |
| 上限 | `priv->max_mtu = mdev->dev->caps.eth_mtu_cap[priv->port]`（**运行时读**，非硬编码） | `mlx4_en/mlx4_en_netdev.c:2215`；caps 字段 `device.h:532` |
| 默认 | `if_setmtu(dev, ETHERMTU)` = 1500 | `mlx4_en_netdev.c:2147` |
| 变更校验 | `new_mtu < MIN_MTU \|\| new_mtu > priv->max_mtu` → `-EPERM` | `:1826-1830` |
| 启动时夹取 | `if_setmtu(dev, min(if_getmtu(dev), priv->max_mtu))` | `:1287` |
| 声明能力 | `IFCAP_JUMBO_MTU` | `:2250` |

### 3.2 RX 缓冲尺寸的选择规则（jumbo 的真正落点）

`mlx4_en_calc_rx_buf()`（`mlx4_en/mlx4_en_rx.c:342-364`）：

```
eff_mtu = if_getmtu(dev) + ETH_HLEN + VLAN_HLEN + ETH_FCS_LEN + MLX4_NET_IP_ALIGN;   // :345-346
if (eff_mtu > MJUM16BYTES)      eff_mtu = MJUM16BYTES;   // :348-350（且报 "MTU too big"）
else if (eff_mtu > MJUM9BYTES)  eff_mtu = MJUM16BYTES;   // :351-352
else if (eff_mtu > MJUMPAGESIZE)eff_mtu = MJUM9BYTES;    // :353-354
else if (eff_mtu > MCLBYTES)    eff_mtu = MJUMPAGESIZE;  // :355-356
else                            eff_mtu = MCLBYTES;      // :357-358
priv->rx_mb_size = eff_mtu;                              // :361
```

也就是**四档离散尺寸，取"刚好装得下"的那一档**，不是精确匹配。
`MJUM16BYTES`/`MJUM9BYTES`/`MJUMPAGESIZE`/`MCLBYTES` 的数值来自 FreeBSD 的 `sys/mbuf.h`
**不在这两个来源里**（本树只出现名称：`:348-358`、`:390`）——需要时按 FreeBSD 头取值，
不要在这里猜。

缓冲的分配入口是 `m_getjcl(M_NOWAIT, MT_DATA, M_PKTHDR, ring->rx_mb_size)`
（`mlx4_en_rx.c:80`），DMA tag 的上限固定写死 `MJUM16BYTES`
（`mlx4_en_rx.c:390` `maxsize`，`:392` `maxsegsize`），即**最大档就是上限**。

### 3.3 RQ WQE 的 byte_count 规则与对齐

**byte_count**（单段形态，本树编译的就是这一支，`MLX4_EN_MAX_RX_SEGS == 1`，`en.h:87`）：

```
rx_desc->data[0].byte_count = cpu_to_be32(priv->rx_mb_size - MLX4_NET_IP_ALIGN);  // :58
rx_desc->data[0].lkey       = cpu_to_be32(priv->mdev->mr.key);                     // :59
```

两个要点：① byte_count 报的是**缓冲长度减 2**（给 IP 头对齐让出的那 2 字节）；
② 这个值是 `rx_mb_size` 的函数，**MTU 一变就得重填**（`:476-500` 在 `activate_rx_rings` 里重发）。

**多段形态**（`MLX4_EN_MAX_RX_SEGS != 1` 时编译，本树未启用）：每段
`byte_count = segs[i].ds_len`（`:178-182`），**用不满的段必须补空**
（`:183-187`）：

```
rx_desc->data[i].byte_count = 0;
rx_desc->data[i].lkey       = cpu_to_be32(MLX4_EN_MEMTYPE_PAD);   // 0x100, en.h:249
rx_desc->data[i].addr       = 0;
```

**对齐/填充规则**：

| 规则 | 值 | 引用 |
| --- | --- | --- |
| IP 头对齐偏移 | `MLX4_NET_IP_ALIGN = 2` | `en.h:80` |
| 收包时把 mbuf 前移 2 字节 | `m_adj(mb, MLX4_NET_IP_ALIGN)` | `mlx4_en_rx.c:130`、`:166` |
| 空段的"特殊 lkey" | `MLX4_EN_MEMTYPE_PAD = 0x100` | `en.h:249`；用法 `mlx4_en_rx.c:68` |
| RX 描述符结构必须 2 的幂 | `CTASSERT(powerof2(sizeof(struct mlx4_en_rx_desc)))` | `en.h:319-320` |
| 环缓冲大小 | `ring->buf_size = size * sizeof(rx_desc) + TXBB_SIZE`（**多留一个 TXBB 放 stamp**） | `mlx4_en_rx.c:406`；stamp 逻辑 `:482-489` |
| 长度从 CQE 取后再减 FCS | `length = be32_to_cpu(cqe->byte_cnt); length -= ring->fcs_del;` | `mlx4_en_rx.c:784-785` |

**FCS（重要且容易踩）**：只有当 `MLX4_DEV_CAP_FLAG_FCS_KEEP`（`device.h:177`，bit34）存在时，
驱动才置 `param3` bit29 并要求硬件保留 FCS，同时把 `ring->fcs_del` 设为 `ETH_FCS_LEN`；
否则 `fcs_del = 0`（`mlx4_en_rx.c:949-954`；本项目已转写为
`MLX4_QP_PARAM3_FCS_KEEP`，`mlx4_regs.h:553`）。**这 4 字节会让每个 jumbo 帧的长度差 4。**

### 3.4 改 MTU 时驱动做什么 —— **端口必须重开**

`mlx4_en_change_mtu()`（`mlx4_en/mlx4_en_netdev.c:1817-1850`）：

```
1. 范围校验（< MIN_MTU 或 > max_mtu → -EPERM）        :1826-1830
2. 取 state_lock，if_setmtu(dev, new_mtu)              :1831-1832
3. 仅当 IFF_DRV_RUNNING：                              :1833
       如果 device_up == false → 什么都不做，留给 watchdog  :1834-1837
       否则 → mlx4_en_stop_port(dev); mlx4_en_start_port(dev);  :1839-1840
4. 重开失败 → 记错误并把 watchdog 任务排上                :1841-1845
```

重开端口会重跑一遍 `mlx4_en_calc_rx_buf`（`:1287-1288`）、重填所有 RQ
（`mlx4_en_activate_rx_rings` → `mlx4_en_init_rx_desc`，`mlx4_en_rx.c:476-525`），
并**重新下发 `SET_PORT_general` 的 MTU 字段**（`:1380-1385`，最终落成
`mlx4_context_mtu`，见 `mlx4_core/mlx4.h:777-796` 的 `struct mlx4_set_port_general_context`，
`mtu` 字段 `:786`，赋值 `mlx4_core/mlx4_port.c:1557`）。

**"不重开端口、只管给栈换个 MTU"是不成立的**：接收缓冲尺寸、RQ 的 byte_count、
端口的 MTU 寄存器三者必须同时换。

顺带：`if_sethwtsomax(MLX4_EN_TX_MAX_PAYLOAD_SIZE - hdr)` 等 TSO 上限也在 init 时设
（`mlx4_en_netdev.c:2257-2259`；`MLX4_EN_TX_MAX_PAYLOAD_SIZE = 65536`，`en.h:268`）。

---

## 4. 多队列 / RSS

### 4.1 队列数量与谁在限制它

| 项 | 规则 | 引用 |
| --- | --- | --- |
| RX 环常量 | `DEF_RX_RINGS = 16`、`MAX_RX_RINGS = 128`、`MIN_RX_RINGS = 4` | `mlx4_en/en.h:81-83` |
| RX 环计算 | `num_of_eqs = max(MIN_RX_RINGS, min(mlx4_get_eqs_per_port(), DEF_RX_RINGS))`；低内存剖析档取 `MIN_RX_RINGS`；最后 `rounddown_pow_of_two` | `mlx4_en_rx.c:329-338` |
| `eqs_per_port` 的定义 | 数该端口在 EQ 表里占的活跃 EQ | `mlx4_core/mlx4_eq.c:1395-1407`；声明 `device.h:1410` |
| RX/TX 环大小 | 默认都是 1024（`MLX4_EN_DEF_RX_RING_SIZE` / `_TX_Q_`） | `en.h:154-155`；上限 8192（`:137-138`），下限 `4096/TXBB_SIZE`（`:141-142`） |
| TX 环数 | `tx_ring_num = num_tx_rings_p_up * MLX4_EN_NUM_UP` | `mlx4_en_main.c:95-96`；`MLX4_EN_NUM_UP = 1`（`en.h:147`） |
| `num_tx_rings_p_up` | `min(mp_ncpus, MLX4_EN_MAX_TX_RING_P_UP=32)` | `mlx4_en_main.c:81-82`；`en.h:146` |
| 硬上限 | `MAX_TX_RINGS = 32 * 1 = 32` | `en.h:149-150` |

即：**TX 环数跟 CPU 数走（封顶 32），RX 环数跟 EQ 数走（封顶 16、下限 4、取 2 的幂）**。
环大小的运行期调整要 stop/start 端口（`mlx4_en_netdev.c:2343-2383`）。

### 4.2 RSS：哈希函数、key、间接表

全部在 **RSS 定向 QP 的 QP context** 里，用一条 `MODIFY_QP` 写下去 ——
**没有独立的"RSS 表"命令**。

结构 `struct mlx4_rss_context`（`qp.h:130-138` `[SRC]`）被放在 QP context 的 `pri_path` 里：

| 字段 | 语义 | 引用 |
| --- | --- | --- |
| 位置 | `((u8 *)&context) + offsetof(pri_path) + MLX4_RSS_OFFSET_IN_QPC_PRI_PATH`，偏移 **0x24** | `mlx4_en_rx.c:1064-1066`；常量 `qp.h:123`；`[PORT]` `mlx4_regs.h:421`、`:423` |
| `base_qpn` | **高 8 位 = `ilog2(rss_rings)`，低 24 位 = 第一个 RX QPN** —— 这就是间接表的规模编码 | `mlx4_en_rx.c:1067-1068`；`[PORT]` `mlx4_regs.h:444`（`MLX4_RSS_QPN_HIGH_SHIFT = 24`） |
| `default_qpn` | 不命中时落到 `base_qpn` | `:1069` |
| `base_qpn_udp` | 仅当 `udp_rss` 打开时设 | `:1070-1071` |
| `flags` | `mlx4_en_get_rss_mask(priv)` | `:1072` |
| `hash_fn` | `MLX4_RSS_HASH_TOP`（Toeplitz）；XOR 是另一种 | `:1073`；常量 `qp.h:108-109` `[SRC]`；`[PORT]` `mlx4_regs.h:445` |
| `rss_key` | `mlx4_en_get_rss_key()` → `rss_getkey()`（**FreeBSD 的 RSS key 生成器，不是硬编码**） | `:1074`、`:997-1003` |
| key 长度 | `MLX4_EN_RSS_KEY_SIZE = 40` | `qp.h:128`；`[PORT]` `mlx4_regs.h:460` |

RSS 的 **flags 掩码**（`mlx4_en_rx.c:1005-1013`）：

```
rss_mask = MLX4_RSS_IPV4 | MLX4_RSS_TCP_IPV4 | MLX4_RSS_IPV6 | MLX4_RSS_TCP_IPV6;
if (udp_rss)  rss_mask |= MLX4_RSS_UDP_IPV4 | MLX4_RSS_UDP_IPV6;
```

位值在 `qp.h:111-116` `[SRC]`（`[PORT]` 也已转写，`mlx4_regs.h:453-458`）。
`udp_rss` 的默认值是 1，但**设备不支持 `MLX4_DEV_CAP_FLAG_UDP_RSS`（`device.h:180`）时会被关掉并告警**
（`mlx4_en_main.c:60-61`、`:83-87`）。

"这个 QP 是 RSS 定向 QP"的标志位在 **context 的 `flags` 字里**
：`flags = 7 << 16 | rss << MLX4_RSS_QPC_FLAG_OFFSET`，
`MLX4_RSS_QPC_FLAG_OFFSET = 13`（`mlx4_en_resources.c:49`；`qp.h:125`；
`[PORT]` `mlx4_regs.h:444`）。普通 RX/TX 环的 QP 传 `rss = 0`（`mlx4_en_rx.c:945`、
`mlx4_en_tx.c:233`），只有定向 QP 传 1（`mlx4_en_rx.c:1056`）。

资源布局：`mlx4_en_rss_map`（`en.h:473-479`）持有 `qps[MAX_RX_RINGS]` + `indir_qp`；
保留区间 `mlx4_qp_reserve_range(dev, rx_ring_num, rx_ring_num, ...)`（`mlx4_en_rx.c:1029-1031`），
每个 RX 环一个 QPN（`:1037-1047`），定向 QP 用 `priv->base_qpn`（`:1050`）。

### 4.3 队列 → CQ → EQ → MSI-X 向量

| 层 | 关系 | 引用 |
| --- | --- | --- |
| 环 → CQ | **1:1**，每个 RX 环一个 CQ、每个 TX 环一个 CQ | `mlx4_en_netdev.c:1701-1709`（RX）、`:1712-1720`（TX） |
| CQ → EQ | TX CQ **借用对应 RX CQ 的向量**：`cq_idx %= priv->rx_ring_num; cq->vector = rx_cq->vector;` | `mlx4_en_cq.c:143-152` |
| RX CQ → 向量 | 初始 `cq->vector = num_comp_vectors`（= "未分配"哨兵，`mlx4_en_cq.c:88`）；激活时若向量无效 → `cq_idx % num_comp_vectors` 再 `mlx4_assign_eq()` | `:128-142` |
| 向量有效性 | `mlx4_is_eq_vector_valid()`：在 `num_comp_vectors+1` 范围内且**该 EQ 真的服务这个端口** | `mlx4_core/mlx4_eq.c:1382-1393`；声明 `device.h:1411` |
| 分配 | `mlx4_assign_eq()`：优先请求的向量，否则挑该端口**引用计数最小**的 EQ；MSI-X 时把向量位记入池并命名 IRQ | `mlx4_core/mlx4_eq.c:1422`（池/命名见其中 `irq_names`、`pool_bm`） |
| EQ 的 MSI-X 号 | `mlx4_create_eq(..., (dev->flags & MLX4_FLAG_MSI_X) ? i + 1 - !!(i > MLX4_EQ_ASYNC) : 0, eq)` | `mlx4_core/mlx4_eq.c:1229-1233` |
| 向量号 → EQ 号 | `MLX4_CQ_TO_EQ_VECTOR(v) = v + !!(v >= MLX4_EQ_ASYNC)`，`MLX4_EQ_ASYNC = 0` | `mlx4_core/mlx4.h:294-298` |
| CQ 的 comp 回调 | `cq->mcq.comp = cq->is_tx ? mlx4_en_tx_irq : mlx4_en_rx_irq` | `mlx4_en_cq.c:163` |
| CQ 分配时的向量入参 | `mlx4_cq_alloc(dev, cq->size, &mtt, &uar, db, &mcq, cq->vector, 0, timestamp_en)`；`mlx4_cq_alloc` 里 `vector >= num_comp_vectors` 直接 `-EINVAL` | `mlx4_en_cq.c:157-159`；`mlx4_core/mlx4_cq.c:mlx4_cq_alloc` |
| 传统（非 MSI-X）模式判定 | `MLX4_IS_LEGACY_EQ_MODE(cap) = cap.num_comp_vectors < cap.num_ports * MIN_MSIX_P_PORT(=5)` | `device.h:51-55` |

RX CQ 激活后驱动**主动 arm 一次**（`mlx4_en_netdev.c:1314`），TX CQ 也 arm（`:1371`）。

### 4.4 `set_rx_mode` / 混杂 / 组播 —— 与本项目现有诚实缺口的对应

**入口**：`mlx4_en_set_rx_mode()` 只是排一个工作项（`mlx4_en_netdev.c:731-739`），
真正干活的是 `mlx4_en_do_set_rx_mode()`（`:1009-1055`）；触发点包括
`SIOCSIFFLAGS` 与 `SIOCADDMULTI/SIOCDELMULTI`（`:1974-1995`）。

`do_set_rx_mode` 的顺序（`:1039-1052`）：

```
mlx4_en_do_unicast()      // 单播地址表
if (IFF_PROMISC || FORCE_PROMISC)  mlx4_en_set_promisc_mode()
else if (PROMISC flag)             mlx4_en_clear_promisc_mode()
mlx4_en_do_multicast()    // 组播
```

它在做正事之前还会**顺手刷新一次链路状态**（`:1025-1037`，`QUERY_PORT` + `if_setbaudrate` +
`if_link_state_change(UP)`）—— 这是第 6 节里"INIT_PORT 时不能说 UP"的补偿。

**组播有两套机制，两套都要做**（这正是本项目"组地址不写硬件"缺口的正面对照）：

1. **端口硬件组播哈希过滤表**，命令 `MLX4_CMD_SET_MCAST_FLTR`（`cmd.h:142`，签名
   `device.h:1373`，实现 `mlx4_core/mlx4_port.c:1716-1722`：把 `mac | clear<<63` 当入参、
   `port` 当 in_mod、`mode` 当 opcode）。`mlx4_en_do_multicast` 的用法（`:910-930`）
   严格是"清空 → 写入广播地址 → 逐个写组地址 → 使能"：
   ```
   SET_MCAST_FLTR(port, 0, 0, MLX4_MCAST_DISABLE)      // :910-911
   SET_MCAST_FLTR(port, ETH_BCAST, 1, MLX4_MCAST_CONFIG) // :916-917（清空并种入广播）
   对每个 mc_list 项：SET_MCAST_FLTR(port, mcast_addr, 0, MLX4_MCAST_CONFIG)  // :922-926
   SET_MCAST_FLTR(port, 0, 0, MLX4_MCAST_ENABLE)        // :927-928
   ```
   `MLX4_MCAST_CONFIG/DISABLE/ENABLE` = 0/1/2（`en_port.h:50-54`）。
2. **流定向表**：每个组地址挂到定向 QP 上 —— `mlx4_multicast_attach(dev,
   &priv->rss_map.indir_qp, mc_list, priv->port, 0, MLX4_PROT_ETH, &reg_id)`（`:961-966`），
   撤销时 `mlx4_multicast_detach`（`:939-943`）。地址表的增量由
   `update_addr_list_flags()` 做差集（`:682-729`，动作枚举 `MLX4_ADDR_LIST_NONE/REM/ADD`，
   `en.h:493-497`）。

**单播**：`mlx4_en_do_unicast()`（`:979-1007`）→ `mlx4_en_uc_steer_add()`
（`:475-525`，按 `dev->caps.steering_mode` 分 B0 / DEVICE_MANAGED 两条路）。

**混杂**：按 `steering_mode` 三分支（`mlx4_en_netdev.c:750-789`）——
DEVICE_MANAGED 用 `mlx4_flow_steer_promisc_add(MLX4_FS_ALL_DEFAULT)`；
B0 用 `mlx4_unicast_promisc_add` + `mlx4_multicast_promisc_add`；
A0 用 `mlx4_SET_PORT_qpn_calc(..., promisc=1)`（实现 `mlx4_core/mlx4_port.c:1578-1613`，
`context->promisc = promisc << SET_PORT_PROMISC_SHIFT | base_qpn`，`:1597-1598`；
shift 常量 `en_port.h:39-40`）。
**注意混杂模式会无条件把组播过滤表关掉**（`:791-795`、`:910-913`）。

**广播**是启动时单独挂的（`:1406-1412`，`mc_list[10..15] = 0xff`；B0 还需要
`mc_list[5] = priv->port`），停止时摘掉（`:1516-1519`）。

**注意 `MLX4_EN_FLAG_FORCE_PROMISC`**：非 A0 steering 时 `mlx4_en_put_qp()` 会置它
（`:605`），即"MAC 注销后强制全收"，避免注销期间的丢包。

---

## 5. INTERRUPT MODERATION（中断调节）

### 5.1 两个参数，两个字段

驱动侧的载体是 `struct mlx4_en_cq` 的 `moder_cnt` / `moder_time`（`mlx4_en/en.h:402-403`），
写下去的函数只有三行（`mlx4_en/mlx4_en_cq.c:212-216`）：

```
return mlx4_cq_modify(priv->mdev->dev, &cq->mcq, cq->moder_cnt, cq->moder_time);
```

`mlx4_cq_modify` 把它们塞进 **CQ context 的两个字段**（`mlx4_core/mlx4_cq.c:mlx4_cq_modify`）：

| 参数 | CQ context 字段 | 结构位置 | 含义（按驱动命名） |
| --- | --- | --- | --- |
| `count` | `cq_max_count` | `mlx4_core/mlx4.h:344`（`struct mlx4_cq_context`，字段在 `:349-350`） | 攒够多少个 CQE 才中断 |
| `period` | `cq_period` | 同上（`mlx4_core/mlx4.h:349`） | 最长等多久 |

命令是 `MLX4_CMD_MODIFY_CQ` = 0x2c（`cmd.h:99`），实现里没有做任何单位换算
（`cq_context->cq_max_count = cpu_to_be16(count); cq_context->cq_period = cpu_to_be16(period);`
—— **原值直传**）。

> **单位**：驱动把这两个量命名成 `rx_frames`/`rx_usecs`、`tx_frames`/`tx_usecs`
> （`mlx4_en_netdev.c:1082-1085`），所以**软件口径是"包数"与"微秒"**；
> 但 `cq_period` 在固件里的物理单位**这些来源没有说明**（没有注释、没有换算）——
> 要定论需要 CX-3 PRM 或实测标定。如实标注：**单位不可从这些来源判定**。

### 5.2 默认值

`mlx4_en_set_default_moderation()`（`mlx4_en/mlx4_en_netdev.c:1071-1115`）：

| 量 | 宏 | 值 | 引用 |
| --- | --- | --- | --- |
| `rx_frames` | `MLX4_EN_RX_COAL_TARGET` | 44 | `mlx4_en_netdev.c:1082`；`en.h:158` |
| `rx_usecs` | `MLX4_EN_RX_COAL_TIME` | 0x10 = 16 | `:1083`；`en.h:159` |
| `tx_frames` | `MLX4_EN_TX_COAL_PKTS` | 64 | `:1084`；`en.h:161` |
| `tx_usecs` | `MLX4_EN_TX_COAL_TIME` | 64 | `:1085`；`en.h:162` |

分配：RX CQ 逐个设 `moder_cnt/moder_time`（`:1091-1098`）；TX CQ 同样（`:1100-1104`）。
调用 `mlx4_en_set_cq_moder` 的位置是 **start_port**：RX 在 `:1308`、TX 在 `:1350`
（失败即 deactivate + 报错）；默认值本身在 `init_netdev` 时算好（`:2282`）。

注释原文把这套默认解释得很清楚（`:1076-1081`）："`moder_cnt` 设成多少个 MTU 大小的包
才够我们的合并目标；`moder_time` 设成固定值"。

### 5.3 自适应（RX）

`mlx4_en_auto_moderation()`（`mlx4_en/mlx4_en_netdev.c:1117-1178`），由统计任务每个
`STATS_DELAY`（`HZ/4`，`en.h:100`）调一次（`:1198`）：

```
rate 超过 MLX4_EN_RX_RATE_THRESH / rx_ring_num 且平均包长 > MLX4_EN_AVG_PKT_SMALL(=256) 才调  // :1148-1149
rate <  pkt_rate_low  → moder_time = rx_usecs_low                                        // :1150-1151
rate >  pkt_rate_high → moder_time = rx_usecs_high                                       // :1152-1153
之间 → 线性插值                                                                          // :1154-1158
否则 → rx_usecs_low                                                                      // :1160
只有 moder_time 变了才下发 MODIFY_CQ                                                     // :1163-1172
```

阈值常量（`en.h:164-171`）：`MLX4_EN_RX_RATE_LOW = 400000`、`RX_COAL_TIME_LOW = 0`、
`RX_RATE_HIGH = 450000`、`RX_COAL_TIME_HIGH = 128`、`RX_SIZE_THRESH = 1024`、
`RX_RATE_THRESH = 1000000 / RX_COAL_TIME_HIGH`、`SAMPLE_INTERVAL = 0`、
`AVG_PKT_SMALL = 256`；`MLX4_EN_AUTO_CONF = 0xffff` 是"上次值未知"的哨兵（`en.h:173`，
初始化于 `mlx4_en_netdev.c:1095`）。

### 5.4 怎么 re-arm

**RX 靠 CQ arm**（`mlx4_en/mlx4_en_cq.c:218-224`）：

```
mlx4_cq_arm(&cq->mcq, MLX4_CQ_DB_REQ_NOT, priv->mdev->uar_map, &priv->mdev->uar_lock);
```

`mlx4_cq_arm` 本体（`cq.h:138-161`）写两处：先写**门铃记录**（host memory）
`*cq->arm_db = sn << 28 | cmd | ci`（`:149`），`wmb()`（`:155`），再写 **UAR 门铃**
`doorbell[0] = sn<<28 | cmd | cqn` / `doorbell[1] = ci`（`:157-160`）。
`MLX4_CQ_DB_REQ_NOT = 2 << 24`，另有 `MLX4_CQ_DB_REQ_NOT_SOL = 1 << 24`（`cq.h:168-171`）。

RX 的 arm 时机（`mlx4_en_rx.c:881-917`）：

```
mlx4_en_rx_irq():   先就地 poll 一个 budget(64, en.h:264)                     // :889
        done == budget → 记下 curcpu，把 cq_task 排进 taskqueue               // :890-893
        否则          → mlx4_en_arm_cq()                                      // :894-896
mlx4_en_rx_que():   绑到 cq->curr_poll_rx_cpu_id，循环 poll 到不满，然后 arm  // :899-917
```

同时**每轮 poll 结束后都要推进 CQ 消费者**：`mcq->cons_index = cons_index;
mlx4_cq_set_ci(mcq); wmb(); ring->cons = mcq->cons_index;`（`mlx4_en_rx.c:857-861`）。
`mlx4_cq_set_ci` 只写 `set_ci_db`（`cq.h:163-166`），与 arm 的 `arm_db` 是两个不同的门铃记录
（`mlx4_en_cq.c:122-125`：`set_ci_db = db`、`arm_db = db + 1`）。

**TX 不靠 arm，靠定时器轮询兜底**：

```
mlx4_en_tx_irq():   process_tx_cq 后 mod_timer(&cq->timer, jiffies + 1)        // mlx4_en_tx.c:423-434
mlx4_en_poll_tx_cq(): 定时器回调，若仍有在途包就再排 MLX4_EN_TX_POLL_TIMEOUT  // :436-461
mlx4_en_xmit_poll(): 每发 MLX4_EN_TX_POLL_MODER(16) 个包顺手 poll 一次        // :463-482；en.h:180
```

一个例外：**TX 环满时用 arm 等"队列开了"**（`mlx4_en_tx.c:664-668`
`mlx4_en_arm_cq(priv, priv->tx_cq[tx_ind]); return ENOBUFS;`）。

**EQ 侧的调节：这些来源里没有。** `mlx4_en` 从不调用任何 EQ 调节接口
（全树无命中）；EQ 的创建/门铃在 `mlx4_core/mlx4_eq.c`，`mlx4_eq.c` 里也没有
moderation 相关字段（`grep moder` 无命中）。**EQ 中断调节不可从这些来源判定**；
要答案需读 EQ context 的固件定义（`struct mlx4_eq_context` 不在本树）或 CX-3 PRM。

---

## 6. LINK / MEDIA

### 6.1 怎么"学到"链路状态、速率、双工

**唯一来源是 `QUERY_PORT` 命令**（`MLX4_CMD_QUERY_PORT` = 0x43，`cmd.h:60`）。
`mlx4_en_QUERY_PORT()`（`mlx4_en/mlx4_en_port.c:74-131`）：

| 返回字段 | 掩码/映射 | 引用 |
| --- | --- | --- |
| `link_up` | bit7 `MLX4_EN_LINK_UP_MASK = 0x80` → `state->link_state` | `en_port.h:90-91`；`:94` |
| `link_up` bit6 | `MLX4_EN_ANC_MASK = 0x40` → `MLX4_EN_PORT_ANC`（自动协商完成） | `en_port.h:92`；`:123-124`；标志枚举 `en.h:481-484` |
| `autoneg` | bit7 `MLX4_EN_AUTONEG_MASK = 0x80` → `MLX4_EN_PORT_ANE` | `en_port.h:93-94`；`:125-126` |
| `link_speed` | 掩码 `MLX4_EN_SPEED_MASK = 0x6f`，映射见下表 | `en_port.h:97-98`；`mlx4_en_port.c:95-118` |
| `transceiver` | 直传 `state->transceiver` | `en_port.h:101`；`mlx4_en_port.c:120` |
| `mac` | `__be64`（**驱动此处未用**） | `en_port.h:100` |

速度映射（`mlx4_en_port.c:95-118`，常量 `en_port.h:78-87`）：

| 寄存器值 | 常量 | 驱动报 |
| --- | --- | --- |
| 0x04 | `MLX4_EN_100M_SPEED` | 100 |
| 0x02 | `MLX4_EN_1G_SPEED` | 1000 |
| 0x00 / 0x01 | `MLX4_EN_10G_SPEED_XAUI` / `_XFI` | 10000 |
| 0x08 | `MLX4_EN_20G_SPEED` | 20000 |
| 0x40 | `MLX4_EN_40G_SPEED` | 40000 |
| 0x20 | `MLX4_EN_56G_SPEED` | 56000 |
| 其它 | —— | **-1**（未知） |

`struct mlx4_en_query_port_context` 完整定义在 `en_port.h:89-102`。
另有 `MLX4_EN_LINK_SPEED_1G/10G/40G` 用于自检（`en.h:904-906`）。

### 6.2 链路事件怎么进来、怎么发布

```
固件事件 MLX4_DEV_EVENT_PORT_UP / PORT_DOWN
  → mlx4_en_event(): priv->link_state = event; queue_work(linkstate_task)   // mlx4_en_main.c:117-127
  → mlx4_en_linkstate(): 与 last_link_state 比较，变了才动                     // mlx4_en_netdev.c:1224-1258
        DOWN → if_link_state_change(LINK_STATE_DOWN) + if_setbaudrate(0)      // :1235-1239
        UP   → 先 QUERY_PORT 刷新速率，再 if_setbaudrate(IF_Mbps(speed))
               + if_link_state_change(LINK_STATE_UP)                          // :1247-1254
```

注意 `mlx4_en_main.c:123-124` 的注释说明了为什么要绕一圈工作项：
"为避免竞争，链路状态在独立任务里轮询而不是就地改"。

**还有第二个发布点**：`mlx4_en_do_set_rx_mode()` 在每次 rx_mode 变更时也刷新一次
（`mlx4_en_netdev.c:1025-1037`），代码里明说这是为了补 INIT_PORT 场景下
不能立刻报 UP 的缺口（`:1241-1246` 的注释）。**做 macOS 侧链路监视时这两处都要覆盖。**

初始状态被显式置为 DOWN（`:2281` `priv->last_link_state = MLX4_DEV_EVENT_PORT_DOWN;`
与 `:2289` `if_link_state_change(dev, LINK_STATE_DOWN)`）。

### 6.3 媒体表（macOS 侧 `ifconfig media` 的对照）

| 函数 | 作用 | 引用 |
| --- | --- | --- |
| `ifmedia_init(&priv->media, IFM_IMASK \| IFM_ETH_FMASK, media_change, media_status)` | 建表，选项掩码含流控 | `mlx4_en_netdev.c:2290-2291` |
| `ifmedia_add`：`1000baseT-FDX`、`10G_SR-FDX`、`10G_CX4-FDX`、`40G_CR4-FDX`、`IFM_AUTO` | 候选项**硬编码** | `:2292-2296` |
| `ifmedia_set(IFM_ETHER \| IFM_AUTO)` | 默认自动 | `:2297` |
| `mlx4_en_calc_media()` | 由 `port_state.link_speed` + `transceiver` 反推当前媒体 | `:1852-1886` |
| `mlx4_en_media_status()` | `ifm_status = IFM_AVALID [+IFM_ACTIVE]`，`ifm_active = calc_media()` | `:1888-1899` |
| `mlx4_en_media_change()` | 只接受 AUTO 或"与当前实际媒体一致"的手动项，否则 `EINVAL` | `:1901-1946` |
| `SIOCSIFMEDIA/SIOCGIFMEDIA` | 转给 `ifmedia_ioctl` | `:1996-1999` |

`calc_media` 的关键分支（`:1862-1879`）：

```
100    → IFM_100_T
1000   → IFM_1000_T
10000  → transceiver 在 (0, 0xC] → IFM_10G_SR
         否则 transceiver == 0x80 或 0 → IFM_10G_CX4
40000  → IFM_40G_CR4
```

即 **10G 的 SR/CX4 之分靠 `transceiver` 字节猜**，源码自己承认
`/* XXX I don't know all of the transceiver values. */`（`:1862`）。
双工是**无条件 `IFM_FDX`**（`:1860`，只要是 up）。
链路 down 时只返回 `IFM_ETHER`（`:1858-1859`）。

### 6.4 流控（pause）与它怎么被"发布"

| 项 | 值 | 引用 |
| --- | --- | --- |
| 默认 | `rx_pause = 1`、`tx_pause = 1`（每个端口） | `mlx4_en_main.c:88-92`；宏 `en.h:175-176` |
| 下发 | `mlx4_SET_PORT_general(dev, port, mtu, tx_pause, tx_ppp, rx_pause, rx_ppp)` | `mlx4_en_netdev.c:1380-1385`、`:2313-2316` |
| 设备的四个字段 | `pptx`/`pfctx`/`pprx`/`pfcrx`，其中 `pptx = (pptx * !pfctx) << 7` | `mlx4_core/mlx4.h:787-792`；`mlx4_core/mlx4_port.c:1556-1561` |
| 每优先级流控模块参数 | `pfctx`/`pfcrx`，范围 0..0xff，越界打警告并归 0 | `mlx4_en_main.c:64-67`、`:276-286` |
| 媒体表里的流控位 | `IFM_ETH_TXPAUSE` / `IFM_ETH_RXPAUSE` 由 `prof->tx_pause/rx_pause` 决定 | `mlx4_en_netdev.c:1880-1883` |
| 用户改流控 | `media_change` 解析 `IFM_OPTIONS(...)` 后重下发 `SET_PORT_general` | `:1932-1944`（注意这里 MTU 传的是 `rx_mb_size + ETHER_CRC_LEN`） |
| 运行期改 PPP | sysctl 处理函数，改 rx_ppp 会 stop/start 端口 | `:2573-2644`（`:2615-2633`） |

### 6.5 光模块 / EEPROM（`transceiver` 的另一半）

| 函数 | 作用 | 引用 |
| --- | --- | --- |
| `mlx4_en_get_module_info()` | 读前 2 字节判模块类型：QSFP/QSFP+/QSFP28/SFP → SFF-8436/8636/8472 与 EEPROM 长度 | `mlx4_en_netdev.c:2417-2462` |
| `mlx4_en_get_module_eeprom()` | 分段读完整 EEPROM | `:2464-2500` |
| `mlx4_en_read_eeprom()` | sysctl 触发，打印到日志 | `:2525-2571` |
| `SIOCGI2C` | ioctl 路径读指定 offset/len（**注释指出地址 0x50 是硬编码，与标准 0xA0 不同**） | `:2068-2090` |

---

## 附录 A —— 常量来源对照（哪些已在 `mlx4_regs.h`，哪些还没有）

已转写（可直接引用 `[PORT]`）：

| 常量 | `mlx4_regs.h` | 原始出处 |
| --- | --- | --- |
| `MLX4_RSS_OFFSET_IN_QPC_PRI_PATH` 0x24 | `:421`、`:423` | `qp.h:123` |
| `MLX4_RSS_QPC_FLAG_OFFSET` 13 | `:444` | `qp.h:125` |
| `MLX4_RSS_HASH_TOP` 1 | `:445` | `qp.h:109` |
| `MLX4_RSS_UDP_IPV6`…`MLX4_RSS_IPV4` | `:453-458` | `qp.h:111-116` |
| `MLX4_EN_RSS_KEY_SIZE` 40 | `:460` | `qp.h:128` |
| `MLX4_QP_MTU_MSGMAX_ETH` 0xff | `:538` | `mlx4_en_resources.c:51` |
| `MLX4_DEV_CAP_FLAG_FCS_KEEP` (1<<34) | `:552` | `device.h:177` |
| `MLX4_QP_PARAM3_FCS_KEEP` (1<<29) | `:553` | `mlx4_en_rx.c:949-954` |
| `MLX4_QP_SCHED_QUEUE_ETH` 0x83 | `:501` | `mlx4_en_resources.c:61` |
| CQE 偏移 0x00/0x04/0x0c/0x10/0x14/0x1a | `:1254`、`:1256`、`:1258`、`:1260`、`:1263`、`:1265` | `cq.h:41-60` |
| `MLX4_CQE_OWNER_MASK` / `OPCODE_MASK` / `OPCODE_ERROR` / `BAD_FCS` | `:660`、`:1275`、`:1276`、`:1277` | `cq.h:101`、`:103`、`device.h:387`、`cq.h:135` |
| WQE ctrl 偏移（`vlan_tag` 0x04 / `ins_vlan` 0x06） | `:1164`、`:1165` | `qp.h:306-307` |
| `MLX4_WQE_CTRL_OWN/CQ_UPDATE/SOLICITED/FORCE_LOOPBACK` | `:1154-1157` | `qp.h:286`、`:292`、`:293`、`:299` |
| `MLX4_OPCODE_SEND` 0x0a / `NOP` 0x00 | `:1148`、`:1146` | `device.h:369`、`:365` |
| `MLX4_CMD_INIT_PORT` 0x9 | `:308` | `cmd.h:57` |
| QP context `param3` 偏移 0xc0 | `:398` | `qp.h:230` |
| RQP_CALC 结构各字段偏移 | `:1042-1064` | `mlx4_core/mlx4.h:798-814`；`mlx4_core/mlx4_port.c:1595-1604` |
| `MLX4_DEV_CAP_FLAG_IPOIB_CSUM` (1<<7) | `:1645` | `device.h:164` |

**尚未转写、本节需要新增的常量**（都在 `[SRC]` 里已有明确定义）：

| 常量 | 值 | 出处 | 用在哪一节 |
| --- | --- | --- | --- |
| `MLX4_CQE_STATUS_IPV4/IPV4F/IPV6/IPV4OPT/TCP/UDP/IPOK` | `1<<6`…`1<<12` | `cq.h:122-130` | §1.2、§1.3 |
| 校验和已验证哨兵 | `0xffff` | `mlx4_en_rx.c:813`、`:818` | §1.3 |
| `MLX4_WQE_CTRL_IP_CSUM` / `TCP_UDP_CSUM` | `1<<4` / `1<<5` | `qp.h:294-295` | §1.4 |
| `MLX4_WQE_CTRL_IIP` / `ILP` | `1<<28` / `1<<27`（**未使用**） | `qp.h:289-290` | §1.5 |
| `MLX4_OPCODE_LSO` / `MLX4_WQE_CTRL_RR` | `0x0e` / `1<<6` | `device.h:371`、`qp.h:288` | §1.5 |
| `mlx4_wqe_lso_seg.mss_hdr_size` | 偏移 0（`(mss<<16)\|ihs`） | `qp.h:367-370`；`mlx4_en_tx.c:746` | §1.5 |
| `MLX4_STRIP_VLAN` | `1<<30` | `qp.h:279` | §2.1 |
| `MLX4_CQE_CVLAN_PRESENT_MASK` | `1<<29` | `cq.h:90` | §2.1 |
| `MLX4_CQE_SVLAN_PRESENT_MASK` | `1<<30`（未使用） | `cq.h:91` | §2.1 |
| `MLX4_WQE_CTRL_INS_CVLAN` | `1<<6` | `qp.h:296` | §2.2 |
| `VLAN_FLTR_SIZE` | 128 | `en_port.h:44` | §2.3 |
| `MLX4_CMD_SET_VLAN_FLTR` / `SET_MCAST_FLTR` / `MODIFY_CQ` | 0x47 / 0x48 / 0x2c | `cmd.h:141-142`、`:99` | §2.3、§4.4、§5.1 |
| `MLX4_MCAST_CONFIG/DISABLE/ENABLE` | 0/1/2 | `en_port.h:50-54` | §4.4 |
| `MLX4_DEV_CAP_FLAG_UDP_RSS` | `1<<40` | `device.h:180` | §4.2 |
| `MLX4_EN_RX_COAL_TARGET/TIME`、`TX_COAL_PKTS/TIME` | 44/16/64/64 | `en.h:158-162` | §5.2 |
| CQ context `cq_period` / `cq_max_count` | 见结构 | `mlx4_core/mlx4.h:349-350` | §5.1 |
| `MLX4_CQ_DB_REQ_NOT` | `2<<24` | `cq.h:170` | §5.4 |
| `MLX4_EN_LINK_UP_MASK` / `ANC_MASK` / `AUTONEG_MASK` | 0x80 / 0x40 / 0x80 | `en_port.h:91-94` | §6.1 |
| 速度枚举 `MLX4_EN_*_SPEED` | 见 `en_port.h:78-87` | `en_port.h:78-87` | §6.1 |

---

## 附录 B —— 这些来源答不出来的问题（如实列出，并写明要什么才能答）

| 问题 | 状态 | 要什么才能答 |
| --- | --- | --- |
| `param3` bit30（`MLX4_STRIP_VLAN`）的**极性**：置位是"剥标签"还是"禁止剥标签"？ | **不可判定**。代码只在**没有** `IFCAP_VLAN_HWCSUM` 时置位（`mlx4_en_resources.c:79-80`），位名却叫 STRIP | CX-3 PRM（NDA）或实机 A/B：打标签帧进，看 CQE 的 CVLAN 位与帧内是否残留 tag |
| `cq_period` 的**物理单位**（微秒？1024ns？） | **不可判定**。驱动按 `usecs` 命名（`mlx4_en_netdev.c:1083`）但原值直传（`mlx4_core/mlx4_cq.c:mlx4_cq_modify`） | 固件定义或实测标定 |
| **EQ** 的中断调节参数与默认值 | **本树没有**。`mlx4_en` 从不调用 EQ 调节，`mlx4_core/mlx4_eq.c` 里也无 moder 字段 | `struct mlx4_eq_context` 的定义（不在本树）或 CX-3 PRM |
| `MJUM16BYTES` / `MJUM9BYTES` / `MJUMPAGESIZE` / `MCLBYTES` 的**数值** | 本树只有名称（`mlx4_en_rx.c:348-358`、`:390`） | FreeBSD `sys/mbuf.h` |
| ~~`MLX4_OPCODE_LSO` 的数值~~ | **已答**：`0x0e`（`device.h:371`），`mlx4_regs.h` 里尚未转写 | —— |
| IPSec / TSO 之外的卸载（如 VXLAN 内层校验和） | 驱动里有 VXLAN 的 steer 代码（`mlx4_en_netdev.c:456-473`），但**与本六项无关**，也未在内层做 csum | 需要时另开一节 |
| macOS 侧这些字段**怎么表达** | **不在本文件范围** | `docs/M5-MACOS.md`（IONetworkInterface / mbuf 侧的表达） |

---

## 附录 C —— 六项的最小落地清单（照 §0.5 的前置条件挂在同一条路径上）

| 项 | 硬件落点 | 一句话 |
| --- | --- | --- |
| 校验和 | CQE `status`+`checksum`（RX，只读）；WQE `srcrb_flags` bit4/bit5（TX，逐包） | RX 是解释，TX 是请求；**没有端口级开关** |
| VLAN | QP `param3` bit30（RX 剥）；WQE `vlan_tag`+`ins_vlan` bit6（TX 插）；`SET_VLAN_FLTR`（过滤表） | TX 侧**不要**把 tag 写进帧 |
| Jumbo | `rx_mb_size` → RQ `byte_count` → `SET_PORT_general.mtu` 三者同改 | **必须 stop/start 端口** |
| 多队列/RSS | 额外 RSS 定向 QP + 每环一个 CQ + `mlx4_assign_eq` 向量 | TX CQ 复用 RX CQ 的向量 |
| 中断调节 | CQ context 的 `cq_max_count`+`cq_period`（`MODIFY_CQ`）；RX 靠 `CQ_DB_REQ_NOT` re-arm | TX 侧是定时器轮询，不是 arm |
| 链路/媒体 | `QUERY_PORT` 一个命令给全（state/speed/autoneg/transceiver）；发布有**两个**点 | `1000/10G_SR/10G_CX4/40G_CR4` 的候选是硬编码的 |
