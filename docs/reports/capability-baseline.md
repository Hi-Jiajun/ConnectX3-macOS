# T01：能力基线第一批验收

建立于2026-10-22，更新于2026-10-24；状态 **PARTIAL / 离线核对完成一部分，P0未通过**。
本轮不调用selector、不安装/重启；原始数据仍来自20:46的M4运行和历史Linux对照。

## 证据与解码链

外层产物目录：`outputs/20261022-cx3-fulladapt-batch1/`（相对于工作区根，不在驱动仓库根）。

- `capabilities.json`：机器清单，含原始BE字节、SHA256、每字段偏移/公式/来源/Fxx、全部64位flags逐位台账。未知位明确保留。
- `decode-capabilities.py` / `test-capability-decode.py`：只消费保存JSON；10项测试覆盖ABI/status/长度、字节序、资源读数、已知/未知位、counter门禁、限速单位和未决字段。2026-10-24的14个新增解码字段使数量从37增至51；原37项raw/value均未改变。
- 原始selector8：`outputs/20261022-cx3-m4-preflight/runs/netup-20261022-204605/8.json`。
- 子代理`capability-inventory.md`已标记**未通过**，不是权威报告，不能据其查询愿望清单操作硬件。

`src/cx3_abi.h:524`定义`Cx3DevCapResult = <QQ64I`：前16字节是status/fwStatus，之后64个host-order dword。
`src/Mlx4Hw.cpp:1753`将BE邮箱读为host-order，`src/Cx3Mlx4.cpp:1408`原样复制到ABI。
所以要**跳过16字节，再逐dword还原大端**；不能直接把structHex当mailbox。
公式核对`src/mlx4_pure.h:291`、`vendor/freebsd-src/mlx4_core/mlx4_fw.c:845-1107`。

## 核定的读数（仅为声明或历史事实）

| 字段 | 核定结果 | 边界 |
| --- | --- | --- |
| 身份 | VPD PN MCX354A-FCCT；15b3:1007 / 15b3:0003 | `linux-diag/run1-evidence/01-pci.txt`；PSID未知 |
| FW / command IF | 2.42.5000 / rev3，max_cmds16，pf_id0 | 20:46 `2.json`；不是固件写入许可 |
| 模式 | 型号是VPI；历史QUERY_HCA解码A0；当前已验证ETH | ETH PCI class不能否定IB；当前固件IB模式能力仍待核 |
| 端口 | cap报告2；macOS已测port1、10G、en2 | 模块/QSA/线缆型号与port2/macOS未核验 |
| PCIe | Linux历史Gen3 x4，卡LnkCap x8；编码后单向理论31.504Gb/s | 本轮未读PCI配置；不归因具体插槽/CPU，不承诺双口40/56G载荷线速 |
| QP/CQ/SRQ/MPT maxima | 各`2^24`，16,777,216 | 不是当前分配量，也不是应用可用配额 |
| EQ | max512，reserved8；num_sys_eqs1024 | QUERY_FUNC历史reserved28/max156是另一层约束，不能混用 |
| PD | cap max`2^17`，reserved4 | 驱动软件固定PD范围与硬件cap不是同一个数 |
| page/UAR | min_page_sz512；uar_size4MiB；reserved_uars2 | 不改变当前4KiB分配；软件UAR基准128另有EQ/QUERY_FUNC约束 |
| 条目大小 | QPC256；CQ/EQ/SRQ128；MTT8；dMPT/cMPT64；RDMARC32；ALTC64；AUX1 | 数量单位为bytes；AUX不是2 |
| max_msg / max_GSO | `2^30` / `2^17` bytes | 不代表TSO已接入或验收 |
| max_ICM | `2^52` bytes | 原始上限/地址空间声明，不是可用主存，也不是本轮分配量 |
| flags64 / bmme | `0xeeff1f9df677fbef` / `0x3d5b8ec2` | 特性位不等于端到端能力 |
| recoverable_error_event | `BE32[0x9c]=0x5e5ffff7`，bit7=1 | 与当前pure头中的旧“off”注释不一致；本批不改生产代码，后续清理注释并回归 |
| RSS / TS / VXLAN | RSS表最大256、Toeplitz/XOR；CQ timestamp与VXLAN测试位为1 | F15/F19/F23仍未功能验收 |
| WoL | flags bits37/38均0 | 仅本次FW声明未报告，不足以关闭全部F35电源/板级能力 |

M4 `40-final.json`实际为 RX7303，TX4434、完成4434；草稿误把RX7303抄成TX。
后续22:52计数仍引用[已有报告](../../../../outputs/20261022-cx3-m4-preflight/POST-M4-0.44.48-20261022-2252.md)，不冒充本批新流量。
selector13 `icmSize`是**bytes**（`src/cx3_abi.h:873`），不照抄草稿的KiB表达。

## 已入账特性位（2026-10-23，离线解码）

来源仍是 20:46 保存的 selector 8 原始 256-byte outbox；偏移取自 `src/mlx4_regs.h`，
位语义与表达式取自 `vendor/freebsd-src/mlx4_core/mlx4_fw.c:1004-1057` 与 `device.h:159-232`。
下表是**硬件声明的能力位**，逐字节由 `decode-capabilities.py` 复现，`capabilities.json`
的 `extra_flags` 有同样 22 条记录（每条的 offset/bytes/mask/source 都在 JSON 里）。

| 位组 | 偏移 | 入账位 | 本卡原始值 |
| --- | --- | --- | --- |
| ext2_flags | 0x70 BE32 | PORT_FLOWSTATS_EN(4)、UPDATE_QP(16)、UPDATE_QP_SRC_CHECK_LB(18)、LB_SRC_CHK(19)、FSM(20)、80_VFS(21)、VLAN_CONTROL(26) | `0xb5fd0000`：bit4=0，其余=1 |
| bmme_flags | 0x94 BE32 | ROCE_V1_V2(19)、PORT_REMAP(24) | `0x3d5b8ec2`：均为1 |
| eth_backpl | 0x9c BE32 | ETH_BACKPL_AN_REP(0)、RECOVERABLE_ERROR_EVENT(7)、DIAG_PER_PORT(17) | `0x5e5ffff7`：均为1 |
| phv_en | 0x96 u8 | SKIP_OUTER_VLAN(6)、PHV_EN(7) | `0x8e`：PHV_EN=1，SKIP_OUTER_VLAN=0 |
| fw_reassign_mac | 0x9d u8 | REASSIGN_MAC_EN(6) | 1 |
| vxlan | 0x9e u8 | VXLAN_OFFLOADS(3)、ETS_CFG(5) | `0xff`：均为1 |
| mad_demux | 0xb0 BE32 | MAD_DEMUX(0) | `0x00000001`：1 |
| cq_eq_stride | 0x7a u8 | QOS_VPP(4)、ETH_PROT_CTRL(5)、CQE_STRIDE(6)、EQE_STRIDE(7) | `0xff`：均为1 |
| reserved_lkey | 0x98 BE32 | — | `0x00008000` |

两点必须一起读：

- **CONFIG_DEV 与 bmme_flags 共用 0x94 已解释清楚**。参考实现里两个宏都写 `0x94`，
  但 `MLX4_GET` 的接收变量不同：`CONFIG_DEV` 用 `u8 field` 读该 dword 的**首字节**，
  `bmme_flags` 用 u32 读**整个 dword**。Linux `fw.c:1028` 与 FreeBSD `mlx4_fw.c` 完全一致，
  所以这不是笔误。本卡 0x94 首字节 = `0x3d` → `CONFIG_DEV(0x20)=1`、`IGNORE_FCS(0x04)=1`，
  两项已写入 `capabilities.json` 的 `feature_tests`（现共 13 条）。
- **声明位不是功能**。例如 ROCE_V1_V2 为 1 只说明固件声明了 RoCE v1/v2 的硬件前置条件，
  F27 仍需要真实的 QP 握手、内存注册与 Linux 对端互通才算通过；VXLAN_OFFLOADS=1 同样不等于
  macOS 路径已经接了隧道解析。

可复现命令（只读保存的 JSON，不碰设备）：

```bash
/usr/bin/python3 outputs/20261022-cx3-fulladapt-batch1/decode-capabilities.py \
  --output outputs/20261022-cx3-fulladapt-batch1/capabilities.json
/usr/bin/python3 outputs/20261022-cx3-fulladapt-batch1/test-capability-decode.py
```

### port 1 的 QUERY_PORT（同一 20:46 快照）

`capabilities.json` 的 `portcap.port1` 由 `decode_port()` 解码；该 selector 返回的是 ABI 结构
（`<4I>` status/fwStatus/port/rawLen，随后 `raw[64]`，再后面是已解码字段），**不是裸 outbox**，
所以解码前先按 rawLen 切出 `data[16:80]`。核对值：

| 字段 | 值 |
| --- | --- |
| supported_types | 2（Ethernet；1=IB，3=VPI） |
| eth_mtu | 9900 |
| link_speed_code | 0x0f（OTHER，尚未协商；0x01=10G XFI、0x40=40G、0x20=56G） |
| max_port_width | 3 |
| max_gids / max_pkeys | 128 / 128 |
| log_max_macs / log_max_vlans | 7 / 7（即 128 个 MAC / 128 个 VLAN） |
| max_vl / max_tc_eth | 8 / 0 |
| def_mac bytes 2..7 | 00:11:22:33:44:55 |
| port 2 | unknown-on-macOS（本驱动从未发过 port=2 的 QUERY_PORT） |

`trans_type/vendor_oui/wavelength/trans_code` 也一并解析，但**不据此推断模块或线缆型号**。
`supported_types=2` 只说明这份固件当前把 port 1 配成 Ethernet，不能反证芯片不是 VPI。

## 仍缺什么以及如何取证

1. ext2/bmme/eth_backpl/phv_en/fw_reassign_mac/vxlan/mad_demux/cq_eq_stride 共22条位已入账（见上节）；port 1 的 QUERY_PORT 已解码并入 `capabilities.json` 的 `portcap`。CONFIG_DEV/IGNORE_FCS 已按 u8 语义入账。2026-10-24已补14个有参考表达式的字段和QP限速声明（见下节），但BF/steering等覆盖仍不完整、port 2 在 macOS 上未知，因此仍不满足T01“全部位/限制入账”验收。
   先前把 `2^64`、`2^35`、legacy PKey值1称为“不可能值，因此偏移错误”的推论已撤销。FreeBSD和Linux v6.12均只命名MRW/MTT两处offset、没有解码表达式；本地log2注释不是独立依据。当前保存raw `0x40` / `0x23`，值为null；CLI也只显示raw/unresolved，不将猜测当容量。PKey按当前QUERY_PORT的0x07低半字节解码为128，不能用legacy DEV_CAP分支的0x3f覆盖。完整证据见[字段审计](r05-query-audit-20261024.md)。
2. 真实PSID、固件配置与模块DOM：命令、参数和执行前置尚需查参考实现/手册。不得使用草稿猜测的`0x14`/`0x1b`，也不把MAD_IFC泛称无副作用只读。
3. port2可追踪已有selector24→`exportM3QueryPort`→`queryPort`；其实现含邮箱分配和固件命令，不是零副作用的内存快照。当前代码已按`fPortMac[port-1]`缓存，并只更新对应端口；先前“单一缓存会覆盖port1”的断言已过时。原活跃会话的允许条件仍须单独审查；本批未执行。源码未提供“必须INIT_HCA”的独立证明，不把草稿该前置当规范。
4. 当前PCIe/物理连线、可用Linux/RDMA/IB对端、40/56G线缆、PFC交换机、PTP参考与VF宿主清单仍待核。缺设备记`BLOCKED-LAB`，不是N/A-HW。
5. 每项上机必须绑定CX3接口/源地址并在远端抓包核验；管理路径仍经en0，不能将默认路由流量误算CX3。

T01第一项离线盘点已有可复现产物；查询规格与完整位映射仍在推进。D01、D06保持OPEN。

## 2026-10-24 配额补全与生产解析修正（仅离线）

14个新增字段均由原20:46邮箱解码，不是新硬件采集：

| 组 | 新增读数 |
| --- | --- |
| 队列容量 | SRQ/QP各16384 WQEs；CQ 4194304 entries |
| RDMA资源 | requester/responder每QP各128；global 2147483648（声明上限，非可分配预算） |
| 组播/XRC | QP/MCG 512；MCG 8388608；reserved MGMs 0；XRC domains最大65536、保留4 |
| 其他 | static-rate bitmap 0；ACK-delay code 15；counter数量256 |

另存`qp_rate_limit`：1024种rate；max raw `0xc028`→value40、unit3（GBS），min raw `0x4200`→value512、unit1（KBS）。保留bits13:12；num_rates为0时不解释上下限。**仅固件声明，不代表QoS已实现或验收。**

核对同时修正内核`mlx4DecodeDevCap()`两处转录错误：`reserved_mcg`应为低4位直接值（本卡0，不是1）；`max_counters`为0x68的BE32，并由flags bit48门控（本卡256，不是0）。纯逻辑143项检查通过；隔离`.116-local`构建通过。CLI后续修正单独编译，保持同一内核候选，不为显示变更重建内核；其2个行为测试含256种raw byte和UBSan。完整本批记录见[审计增量](r05-query-audit-20261024.md#2026-10-24-implementation-follow-up)。
