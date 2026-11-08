任务：审查并细化一个网卡驱动移植项目的 **R4 阶段实现方案**（建立 CQ/QP，实现单队列收发，
目标是 ARP/ping 通）。请逐条核对、指出错误，并给出可以直接照着写代码的顺序。

## 项目与已完成的部分

把 **BSD 许可**（OpenIB.org BSD 分支）的 Mellanox 驱动从 FreeBSD `sys/dev/mlx4` 移植到
macOS IOKit kext。硬件 ConnectX-3 Pro（`15b3:1007`，MCX354A-FCCT，**双口 QSFP+ 以太网**）。
目标机 macOS 15.8.1 x86_64。

**已实测完成（每一条都是真机、可复现）：**

- 固件交接：`MAP_FA` / `RUN_FW` / `MOD_STAT_CFG` / `QUERY_DEV_CAP` / `SET_ICM_SIZE` /
  `MAP_ICM_AUX` / **14 张 ICM 表**（全部 `MAP_ICM`）/ `INIT_HCA`，固件状态全 0。
- **端口已 up**：`SET_PORT` general（Ethernet opcode、mtu 1522）→ `INIT_PORT` →
  `QUERY_PORT` 报 link bit 1、`link_speed = 0x01` = **10000 Mb/s**。
- **EQ 已工作**：EQN 28、512 槽、64 字节 EQE、MTT offset 64；轮询与中断两条路都验证过
  （1031 个事件、两次以上相位回绕）；`MAP_EQ` mask `0x600` 路由 CMD + PORT_CHANGE；
  真实的 `PORT_CHANGE`（type `0x09`）已被正确消费且不冒充 CMD 完成。
- 中断：legacy INTx + 设备侧清除寄存器（`QUERY_ADAPTER` 的 `inta_pin`=63 → mask 0x80，
  BAR0+0xf0058）；`acks` 与 `invocations` 1:1。

**从 `QUERY_DEV_CAP` 实测到的关键能力（本卡）：**

```
entry sizes: qp 256  cq 128  eq 128  srq 128  mtt 8  dmpt 64  cmpt 64  rdmarc 32  aux 1  altc 64
reserved:    qps 64  cqs 128  srqs 64  eqs 8  mtts 64  mrws 256  mgms 1
uar_size 4194304  reserved uars 2   min_page 512
flags  0xeeff1f9df677fbef    flags2 0x120429
QUERY_FUNC: max_eq 156  reserved_eqs 28  reserved_uars 7
INIT_HCA 实际用的基址: qpc 0x100080000 cqc 0x100170000 eqc 0x10017a000 mtt 0x1000c0000
                        mc 0x100100000  dmpt 0x100140000   uar page 0  log 10
```

**已经确定的 steering 模式（这一步请重点核对）：**

- `flags` 的 bit41（`MLX4_DEV_CAP_FLAG_VEP_UC_STEER`）= **1**，bit42（`..._MC_STEER`）= **1**。
- `flags2` 的 bit3（`MLX4_DEV_CAP_FLAG2_FS_EN`）= 1。
- 但 Linux 6.6 上同一张卡的内核日志有：
  `mlx4_core 0000:07:00.0: DMFS high rate steer mode is: disabled performance optimized steering`
  ——而按 `mlx4_main.c:2105-2120`，`MLX4_STEERING_DMFS_A0_DISABLE` **只在非
  DEVICE_MANAGED 的 else 分支里**被设置。
- 于是按 `mlx4_main.c:2117-2123`：UC_STEER 与 MC_STEER **同时**置位 ⇒
  `steering_mode = MLX4_STEERING_MODE_B0`。
- 并且 `mlx4_port.c:1585-1586`：`if (dev->caps.steering_mode != MLX4_STEERING_MODE_A0) return 0;`
  ⇒ **B0 下 `mlx4_SET_PORT_qpn_calc` 是空操作**，不会发命令。

## 允许阅读的路径（只读这两处）

```
/Users/you/Documents/Hackintosh-MacLocal/tools/mlx4-port/freebsd-src/   # 参考实现
/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/            # 移植代码
```

移植代码里已有可复用的基础设施：DMA 分配（`Mlx4Hw::icmAlloc`，按 4 KiB 页给出总线地址数组）、
MTT 分配/写入/读回（`mttAlloc`/`mttWrite`/`mttEntryValue`）、命令路径（`cmdBox`/`cmdBoxImm`，
含 mailbox）、以及 `eqCreate`/`eqMap`/`eqHw2Sw` 这套"环 + MTT + context + SW2HW"的写法
（`CQ`/`QP` 应该照它的形状写）。

## 请你回答（逐条 A–F）

**A. 核对 steering 模式与 `qpn_calc`。** 上面那条推理链（尤其"Linux 日志里
DMFS high rate steer mode is: disabled 证明走了 else 分支"）是否成立？
`choose_log_fs_mgm_entry_size` 与 `fs_max_num_qp_per_entry` 会不会让条件①成立而选中
DEVICE_MANAGED？请给出**在允许阅读的源码里怎么用已实测的能力字把这件事定下来**
（如果你认为不能定下来，就说不能，并给出最小的判别方法）。

**B. B0 模式下收包需要哪些 steering 配置？** 请按源给出 B0 路径下：
1. 本机单播 MAC 怎么注册（`mlx4_register_mac` 在 B0 走哪条分支、发什么命令、MAC 索引与
   QPN 的关系）；
2. **广播/组播**怎么注册（`mlx4_multicast_attach` 的参数与命令），ARP 请求要能收到需要哪几步；
3. 是否需要 `SET_PORT_MAC_TABLE` / `SET_PORT_VLAN_TABLE` / `PRIO_MAP`，还是 B0 下不需要。
请给出每条命令的 opcode、`in_modifier`/`op_modifier`、mailbox 布局与成功判据。

**C. CQ 的最小正确实现。** 请给出：CQ 环的大小/格式（本卡 `cqc_entry_sz = 128`）、
CQ context 的完整字段（**逐字节给偏移**，包括 `cqn`、`log_cq_size`、`comp_eqn`、
`cq_period`、`cq_max_count`、`db_record_addr`、MTT base）以及 `SW2HW_CQ` 的调用形状。
特别说明：`db_record_addr` 在 B0/本卡上应该怎么填（它是 UAR 里的一个记录地址，
参考里 `mlx4_get_cq_uar`/`mlx4_cq_set_ci` 用的是哪套算术）。**这一处如果填错，
完成事件就不会出现**，请给出可核对的公式与依据。

**D. QP 的最小正确实现。** 本卡 `qpc_entry_sz = 256`。请给出：
1. **PD 分配**（`mlx4_pd.c`）在原生 PF 上的做法：是软件 bitmap 分配，还是要发命令？
   mailbox-entry 形式怎么写？
2. QP 的建立顺序：是发通用的 `SW2HW_QP`，还是 `RST2INIT_QP`→`INIT2RTR_QP`→`RTR2RTS_QP`？
   请给出每一步的 opcode、`in_modifier` 构造、以及 **QP context 里每一步必须填哪些字段**
   （含 `PD`、`QPN`、`MTT base`、`log_sq/rq_size`、`sq/rq *head/tail*`、`state`、
   `mtu`、`msg_max`、`log_rq_stride`、`log_sq_stride`、`db_record_addr` 的偏移）。
3. 建立 QP 之前需要 `INIT_HCA` 里哪些字段已就位（比如 `log_num_qps`、`qpc_base`）——
   请核对本移植的 `INIT_HCA` 是否已经把 QP 需要的表登记好了（本移植映射了 14 张表，
   含 QPC/CQC/EQC/MPT/MTT/MCG 等）。

**E. 收包的完整最小链路。** 从"线缆上来了一个 ARP 请求"到"驱动拿到这块 mbuf"，
必须存在的每一环是什么？请按顺序列出并说明**每一环填错时的可观察症状**
（例如"CQE 不出现"、"CQE 出现但 opcode 不对"、"缓冲区被写但 WQE 没被消费"）。
特别说明：RQ 的 WQE 格式（`mlx4_en/mlx4_en_rx.c` 的 `mlx4_en_prepare_rx_desc`）、
以及 `mlx4_en` 如何判断一个刚收到的帧应该交给协议栈。

**F. 工程顺序与风险。** 每加一段都需要一次重启（kext），用户态工具不需要。
请给一个**分阶段**的推进顺序，每阶段一个 selector、一个明确的成功判据，
并指出哪些阶段"不成功"时**最可能是我自己的编码错误而不是硬件**（例如 CQ 的
`db_record_addr`、QP 的 `log_rq_stride`、MTT 地址单位）。
另外请指出本阶段**最可能把整机弄死**的操作（我在中断阶段已经因为未清除的 legacy 中断
卡死过一次，所以对这一类风险要提前标注）。

回答用中文，**逐条 A–F**，区分"源码可确证"与"你的推断"，不确定写 "unknown"。
不要复述背景。
