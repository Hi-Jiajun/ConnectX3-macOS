任务：**接续**上一次的 R4 方案，但现在有了一个确定的答案，方向因此改变。请给出针对
**A0 steering** 的最小实现细节。

## 允许阅读的路径（只读这两处；下面提到的文件名都在里面）

```
/Users/you/Documents/Hackintosh-MacLocal/tools/mlx4-port/freebsd-src/   # 参考实现，mlx4_core/ 与 mlx4_en/ 都在这里
/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/            # 移植代码
```

具体常用文件：`mlx4_core/mlx4_fw.c`、`mlx4_core/mlx4_port.c`、`mlx4_core/mlx4_cq.c`、
`mlx4_core/mlx4_qp.c`、`mlx4_core/mlx4_pd.c`、`mlx4_core/mlx4_mr.c`、`mlx4_core/mlx4.h`、
`mlx4_core/device.h`、`mlx4_core/cmd.h`、`mlx4_en/mlx4_en_rx.c`、`mlx4_en/mlx4_en_cq.c`、
`mlx4_en/mlx4_en_resources.c`、`mlx4_en/mlx4_en_netdev.c`、`mx4_en/mlx4_en_port.c`、
`device.h`、`cmd.h`（根目录也有这两份）。

## 上一版 R4 的阶段表（这次要按 A0 具体化）

上一版的两份方案在
`/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/outputs/krill/r4plan-astra.md` 与
`.../r4plan-sol.md`，可以读。当时的顺序是：
`QUERY_HCA` 定模式 → PD/MR → CQ（含 `db_record_addr` 公式）→ RX 缓冲与 RQ WQE →
RX QP（RST→INIT→RTR→RTS，非通用 `SW2HW_QP`）→ 入口 QP → TX CQ/QP →
GENERAL/`qpn_calc`（仅 A0）→ `INIT_PORT` → broadcast attach 与接收过滤 →
原始双向帧 → ARP/ping。

## 已确定：这张卡/这次 INIT_HCA 的 steering 模式是 **A0**

我按你们两位的要求先加了 `QUERY_HCA` 并真机读回（这是唯一一次测量，不是推断）：

```
raw 0000: 02 00 00 00 00 00 00 00 00 00 00 00 01 3d 40 00
    0010: 00 00 00 00 00 00 20 19 80 00 00 00 00 00 00 00
    0020: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    0030: 00 00 00 01 00 08 00 0a 00 00 00 00 00 00 00 00

  steering mode     : A0
  flags dword 0x014 : 0x00002019   (device-managed bit 6 = 0)
  byte 0x0d8        : 0x00         (B0 is bit 3 = 0)
  mc_base 0x0c0     : 0x0
  log_mc entry/hash/table : 0 / 0 / 0
  log_num_qps       : 10   (0x37, 与 INIT_HCA 送出的 10 一致)
  log_num_cqs       : 0    (0x57)  <- 见下面"一处待核实的疑点"
```

对照参考的判据（`mlx4_fw.c:2097-2105`）：bit6=0 且 `0xd8` bit3=0 ⇒ **A0**。与
`mlx4_port.c:1588-1589`（非 A0 才 return 0）的推论一致。
`mc_base = 0`、`log_mc` 三项全 0 —— 说明移植的 `INIT_HCA` 确实把 MC 表当 A0 送，
但**没有**写 B0 位（这是我们本来就知道的那处）。

## 一处待核实的疑点（请判断是不是我读错了）

`log_num_cqs` 我按 `INIT_HCA_LOG_CQ_OFFSET = INIT_HCA_QPC_OFFSET + 0x37 = 0x57` 读，
得到 **0**；而本移植 `INIT_HCA` 往同一个 `0x57` 写的是 **8**（日志可证），
同一份解码里 `log_num_qps`（`0x37`）读回 10 是对的。

请判断可能的原因（可用的证据有限：我的原始 dump 只有 64 字节，看不到 `0x57`）：
(a) 我把 CQ 的偏移算错了（请按 `mlx4_fw.c` 的宏逐层展开复核 QPC/CQC 两组偏移）；
(b) 固件在 QUERY_HCA 的 outbox 里不回填这个字段；
(c) 别的。
**如果 (a)**，请给出正确偏移；**如果是 (b)/(c)**，请说明"QUERY_HCA 不能用来读 CQ 数量"，
并指出 R4 应该从哪里得到 CQ 数量（我自己的 profile 里算出来的？`QUERY_DEV_CAP` 的
`reserved_cqs`/`max_cqs`？）。

## 请给出针对 A0 的最小实现细节（这是本次的重点）

本卡实测的数字：`qpc_entry_sz 256`、`cqc_entry_sz 128`、`eqc_entry_sz 128`、
`mtt 8`、`dmpt 64`、`rdmarc 32`；`reserved: qps 64 cqs 128 srqs 64 eqs 8 mtts 64 mrws 256 mgms 1`；
`uar_size 4194304`（4 MiB）、`reserved uars 2`、`min_page 512`；`log_num_qps 10`；
`INIT_HCA` 实际基址 `qpc 0x100080000 cqc 0x100170000 eqc 0x10017a000 mtt 0x1000c0000
mc 0x100100000 dmpt 0x100140000`；`uar page 0 log 10`；`QUERY_FUNC` 报
`max_eq 156 reserved_eqs 28 reserved_uars 7`。

**A. A0 下的收包导向到底要哪几步？** 请按源码给出**最小集合**与顺序：
1. MAC 注册（`__mlx4_register_mac`）——A0 下它怎么算 MAC 索引、写哪个命令、
   mailbox 里放什么；
2. `base_qpn` 从哪里来（本移植要自己挑一个，还是固件有分配？A0 下它与 MAC 索引的关系）；
3. `SET_PORT_qpn_calc` 的**完整输入 mailbox 字段与偏移**（`struct
   mlx4_set_port_rqp_calc_context`），每个字段在"只求 ping 通"时的取值，
   以及 `in_modifier`/`op_modifier`；
4. `INIT_PORT` 与 `qpn_calc` 的先后（参考是 general → qpn_calc → INIT_PORT，
   而我现在的 `m3portup` 已经做过 general → INIT_PORT；R4 应该重做一遍还是接着用？）；
5. 组播/广播：A0 下 `mlx4_multicast_attach` 走哪条分支、要什么参数、
   ARP 请求能被接收到最少需要哪一步。

**B. `db_record_addr` 在 A0 下怎么算？** 这是你们上次都标为"填错就永远等不到完成事件"的
字段。请给出：CQ context 里 `db_record_addr`（含高低两个字段的偏移）、
它的单位（字节？UAR 内偏移？），以及参考 `mlx4_get_cq_uar`/`mlx4_cq_set_ci` 用的**完整公式**
（含 `uar_page_shift`、`MLX4_CQ_DOORBELL` 之类的常量）。给一个**用本卡实测数字代入后**
的具体数值，让它可以被直接核对。

**C. QP 的 SQ/RQ 环与 doorbell。** A0 下的 QP context 里，`sq_head/sq_tail`、
`rq_head/rq_tail`、`log_sq/rq_size`、`log_rq_stride`、`sq/rq db_record_addr`
分别怎么填？RX 的 producer doorbell 与 TX 的 doorbell 分别写到哪里、写什么值？
请给出**用本卡数字代入后的具体地址**。

**D. RX WQE 与 CQE 的格式。** 请给出 RQ WQE 的逐字段布局（`mlx4_en/mlx4_en_rx.c` 的
`mlx4_en_prepare_rx_desc` 是权威），以及收到一个帧之后 CQE 里驱动需要读哪些字段
（`mlx4_en/mlx4_en_rx.c` 的 `mlx4_en_process_rx_cq`）、**怎么判断"这是一个完整帧"**
与"一个帧横跨多个 WQE"。

**E. 分阶段顺序（每阶段一个 selector、一个成功判据）。** 上次你们给的顺序我采纳，
但现在模式是 A0，请把其中依赖模式的部分**按 A0 具体化**，并标注每阶段
"失败时最可能是我自己的编码错误"的具体位置（例如 CQ 的 `db_record_addr`、
QP 的 `log_rq_stride`、MTT 地址单位、`base_qpn` 与 MAC 索引的关系）。

回答用中文，**逐条 A–E**，区分"源码可确证"与"你的推断"，不确定写 "unknown"。
不要复述背景。
