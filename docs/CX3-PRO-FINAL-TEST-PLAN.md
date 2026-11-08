# CX3 Pro 集中完整测试计划

**日期：2026-10-25；排期：功能开发候选形成后集中执行。** 实施依据为[功能优先计划v4](CX3-PRO-FEATURE-FIRST-PLAN.md)。日常简单验证不执行本计划的全部矩阵；本计划也不要求开发期间逐行补齐报告。

## 进入、执行与退出

进入条件：相关适用功能真实生产链已实现，kext/CLI/SDK可构建，未完成项/平台与实验资源缺口已列明。测试人员冻结一份集成候选（源码/配置、二进制hash、UUID、ABI、OS/FW及对端身份），按功能组跑测试；出现缺陷修复后只重跑影响组和基本网络回归，最终再跑一次整体组合。

硬件测试用独立管理接口、真实远端落盘与告警通道、单一owner；硬件操作由当次明确窗口覆盖。每组先确认实际端口/对端/连接与所用固件；条件不足记BLOCKED，不改成PASS。固件/NVRAM/ROM写、模式持久变更单列备份/恢复步骤并取得相应授权。

退出条件仍为：所有实际适用子项端到端PASS，未知/失败/阻塞项均解决，N/A-HW有本卡证据；日用48h和全功能72h均通过。功能开发完成与最终项目完成分别结论，不按编译成功宣称完美适配。

## 测试组与覆盖

| 组 | 功能ID | 完整测试内容与通过标准 | 资源/证据 |
| --- | --- | --- | --- |
| Q0 构建/ABI/权限 | F01/F02/F03/F24/F43/F44 | 全部既有host C/Python测试、standard构建、内核/CLI/SDK ABI表一致；错误大小/版本/句柄/权限被拒绝；支持OS版本分别编译/链接 | 全量命令/退出码、候选hash、API版本；合成与实机分开 |
| Q1 基础联网/自动启动 | F03–F06/F18 | 自动启动、禁用、安全模式、DHCP重租/静态地址/地址切换、ARP/ND、IPv4/6、TCP/UDP、mDNS；每口同时双向流量，独立down/up、路由/抓包/桥接及实际可用bond/LACP | 双口Ethernet对端、LACP对端按需；独立抓包与每口计数，默认出口不走错 |
| Q2 生命周期/故障恢复 | F02/F03/F06/F28/F35/F37 | 正常stop/reopen100轮；cold10/warm20/sleep50/link100；命令超时、client退出/被杀、初始化各阶段拒绝、FW/MTT/CQ/EQ失败与表满；迟到callback/并发配置；失败安全保留或恢复，不UAF/越权DMA/无限重置或资源持续增长 | 现场远端phase/首错CQE-WQE、资源账本/内存计数；正常撤销与异常reset分别证明 |
| Q3 L2/MTU/VLAN | F07–F09/F18 | MAC改动、多单播/组播/表满/allmulti/promisc、正常过滤不泄漏；标准/边界/jumbo/超长帧、DF/ICMP及在线MTU切换；插入/剥离/过滤、多VLAN、cap支持的QinQ与tag-MTU组合 | 线上对端包长/tag/目的MAC、坏包拒绝与关闭后配置清理 |
| Q4 Offload/DMA性能 | F10–F13 | IPv4/6 TCP/UDP checksum开关A/B、分片/扩展头/坏包；TSO4/6真实线上分段及VLAN组合；多段SG/高地址/内存压力、DMA pin/unpin/CQ回收；inline/BF/聚合/复用逐项证明硬件来源和栈语义 | Linux已知好参考、对端抓包；吞吐/CPU/延迟/小包/丢包和重传；目标沿用总计划90%可比Linux吞吐并记录PCIe/CPU限制 |
| Q5 中断/队列/steering | F14–F17 | 每个向量/CQ arm及空闲→负载竞态、moderation/回退；两队列扩展到支持数量；RSS IPv4/6 TCP/UDP/分片key与indirection向量；TX回压/公平/流内有序、单队列失败隔离、规则冲突/容量/增删重配 | raw QPC与同次运行身份、独立每队列计数/流抓包；单环联网不能代替分流 |
| Q6 时钟/流控/隧道 | F19–F23 | RX/TX帧关联硬件时间戳、时钟域/回绕/漂移、PTP/gPTP同步误差；Pause/PFC死锁恢复、ETS带宽份额/DCBX与拥塞；支持的隧道内外checksum/TSO/RSS开关A/B，未知协议不误卸载 | PTP参考与支持PFC/DCB交换机、Linux隧道对端；运行前填入参考可达到的误差/带宽阈值并冻结，不能测后调阈值 |
| Q7 RDMA/RoCE应用 | F24–F28/F43 | context/PD/MR/MW/CQ/QP/SRQ/XRC逐对象创建/销毁/配额/错误句柄；RC/UC/UD SEND/RECV、READ/WRITE/atomic与高级注册/rkey失效/越界；RoCEv1/v2、CM/GID/VLAN/路由；双方向、多进程与Ethernet同时负载、坏client退出及链路恢复；每消费者分别试 | Linux mlx4/verbs与perftest/应用、远端内容校验；无裸物理DMA/BAR跨权限；TCP模拟无效 |
| Q8 IB/VPI | F29–F32/F43 | 本卡适用ETH/IB/混合模式及回滚；IB链路速率/SM/MAD/PKey/GID、分区隔离/错误PKey/QKey、IB RC/UC/UD/组播/APM按cap；IPoIB真实IPv4/6/MTU及应用 | IB对端/SM/模块线缆；RoCE测试不能代替IB，缺资源保留BLOCKED-LAB |
| Q9 扩展/板级/诊断 | F33–F42 | PF/VF创建/删除/reset与配额/MAC/VLAN/mailbox；guest网络/RDMA及IOMMU/恶意VF隔离；WoL/PME/D状态与待机供电、误唤醒；EEPROM/DOM/温度/LED/健康统计与Linux对照；固件备份/恢复；真实peer-memory注册撤销、cap存在的CORE-Direct/sideband、ROM启动与OS交接 | 支持的hypervisor/GPU平台/板级布线/ROM资源分别证明；不存在接口不能以host staging或枚举替代；持久写单独授权 |
| Q10 安装/组合/长时 | F43/F44及Q1–Q9 | 干净安装/升级/ABI错配/卸载/回滚；许可证与支持矩阵；日用候选48h、全功能候选72h，网络/RDMA/队列/电源适用组合；故障后日志/告警与恢复可对账 | 冻结发布包与支持OS/FW组合；功能开关、测试报告、完整未完成清单；任何组未过不得总PASS |

## 集中执行顺序

1. Q0一次全量host与构建；先消除接口/链接错误。
2. Q1、Q2基本运行与恢复；阻断性错误优先修复，再继续硬件功能组。
3. Q3–Q9按设备/外设预约分组，同一候选尽量同次启动完成；共享硬件只允许一个写入者，互不影响的对端分析可并行。
4. 对缺陷定向修复和回归，再执行Q10及最后一次整体回归。

100轮/48h/72h在此集中阶段执行一次；修复影响其结论时才重跑。原有mutation/广泛故障注入/跨功能排列组合都在相应Q组，开发期不为每个小改自动重演。

## 报告格式与资源欠项

每条结果记录：候选/配置/接口/对端身份，输入与预期，实际命令/原始输出/时间，PASS/FAIL/BLOCKED及原因，相关缺陷ID。环境导致跳过与功能失败分开，未知vendor syndrome不猜含义。长时用每阶段检查点与摘要，故障保留前后原始日志。

现有资源不能从历史配置推定为可用：Ethernet Linux对端、第二口线缆、40/56G或IB/SM、PFC/DCB交换机、PTP参考、VF/IOMMU宿主、GPU peer-memory接口及ROM/板级连接，在进入对应Q组前逐项核实。不默认购买硬件。没有对端不阻止生产代码开发，但不能伪造实测或以N/A关闭。

## .131 自动入口最终集成补充

集中上机阶段验证：同一候选boot下自动入口首次初始化/接口身份/实际DHCP与短收发、重复启用不reset、禁用后OS与驱动状态一致及恢复、服务退出/崩溃后完整与部分初始化状态、版本/boot/service错配、忙与provider停止。保留全阶段失败/超时/资源不足、RSS回读拒绝、TX首错与保留owner观测；launchd启动/默认禁用/安全模式/正式安装回滚一起验。上述项目未执行；host合成结果不能代替它们。压力、100轮和48/72h仍在原集中回归阶段，不提前串行阻塞B–I开发。

## .132 每口管理状态集中验收补充

同次候选启动核对两口QUERY_PORT/raw/时间戳/MAC/link/速率各自归属；端口2查询、MTU/INIT/CLOSE不污染端口1。验证SET失败不宣传MTU，INIT后查询失败保留诊断且不报有效link，generation变化失效观察/配置/关闭回执。两口分别成功CLOSE后才允许共享CLOSE_HCA，单口拒绝/超时保持资源可达。将管理状态结果与后续B2每口实际队列、接口、线上包和计数一起核对，不能用selector52读取成功代替独立双口收发。上述硬件场景未执行；与Q1/Q2/Q3集中进行，不提前要求100轮或长时测试。

## .133 每口队列owner集中验收补充

B2创建接口接通后，分别在两口制造部分创建/未知FW回执/未回收指针，核对共享MR、ICM/profile、CLOSE_HCA与首次reset判断不漏任一口；reset保留预算去重两口所有真实分配，观察报告可见两口owner，代次变化不能清空旧指针。真实并发收发/单口回收验证另外一口CQ/RQ/SQ/QP、producer/consumer与原生回调不受污染。上述仅有host定向简验，完整硬件场景随Q1/Q2/Q5集中执行。

## .134 每口创建/回收集中验收补充

Q0纳入 `tools/test-port-queue.py`；Q1/Q2/Q5同次候选验证两口编号/QPC/MTT/MAC归属及真实steering，核对完整MAC窗口与RSS/TX对象不重叠。与B2c接口联调后验证双口收发、第一口负载中创建/关闭第二口、partial失败后显式销毁/重建、CLOSE_PORT失败保留owner、shared reset/stop覆盖两口、generation变化拒绝旧预留、legacy与managed入口混用。当前managed预留跨代次不重新分配；安全恢复/重建须验证实际owner归还后再设计，不能清指针绕过。MAC写入回执不能替代线上寻址验证；旧autonet MAC readback门禁问题另行处理，未在本批放宽。上述完整场景均DEFERRED，不以合成transport或管理ready代替原生双口联网。

## .135 独立原生双口集中验收补充

Q0纳入 `tools/test-native-port.py`，其IOKit/HW/scheduler是合成边界。Q1同一候选双口各取得DHCP地址，核对独立en unit/MAC/media/link、并发IPv4/IPv6收发和路由；一口down/up或host detach不影响另一口。Q2验证attach/stop交错、selector排空后再排空child、attach worker超时/迟到、timer移除或thread_call_free失败、mbuf与PF-DMA分别保留，host detach后显式queue destroy/create才重附。query/command损坏及retention耗尽时host status/detach可达，但不能强开新接口。关闭gate或旧generation不post、不重arm；RX错误/错索引停止本口refill，TX首错锁住本口新TX且持续回收完成，另一口继续。Q3/Q5验证实际MAC/steering归属；原生对象分开不等于硬件过滤已证实。真实IOKit retain/worker/free、provider终止及DMA撤销必须上机验证，不能以同步stub模拟当作竞态PASS。上述完整场景均DEFERRED，与Q1/Q2/Q3/Q5集中进行，不在开发期追加100轮/长时门禁。

## .136 原生过滤/checksum集中验收补充

Q1/Q3/Q4在双口同时收发下核对系统混杂/组播mode与名单真正进入各自端口硬件：broadcast、空名单、IPv4/IPv6组播、多次名单更换、溢出全组播和恢复、promisc开/关后正常名单还原；核对对应物理口MAC窗口/QPN和Linux对端抓包，不以registry/applied revision代替线上收包。Q2验证workloop持门期间worker争用PF、stop/attach/detach交错、命令失败/超时与迟到worker；Busy不可提交被拒绝请求，部分写入失败保持applied回执未晋级并关闭本口，不能悄悄重试或影响另一口。超时状态不得读取尚运行worker的半写配置。Q4校验IPv4/IPv6 TCP/UDP、分片/扩展头/VLAN组合、坏RX checksum不被标记为good、真实TX线上checksum；需要补运行开关入口后再做A/B，当前synthetic WQE flags/CQE metadata不能视为报文checksum已验收。真实框架、并发及硬件场景均DEFERRED，与Q1–Q4最后集中执行。

## .138/.139 MTU、VLAN、MAC与配置集中验收补充

Q0纳入 `test-jumbo-dma.py`、`test-vlan-wqe.py` 和扩展的 `test-native-port.py`；简验中的页表、IOKit/HW/scheduler仍为合成环境。Q3验证9000 payload的9014/9018/9022长度、预配置ceiling与软件MTU切换、线上C-tag插入/剥离及完整PCP/DEI/VID、VID0/4094/4095、白名单增删/清空/恢复全部、promisc绕过白名单及退出后恢复、另一口不受影响、MAC切换前后寻址和slot/QPN归属。RX内联规范化不能替代硬件剥离证明；QinQ和多单播待实现后另验。Q4通过selector55做RX标记开关A/B；TX开关目前仅允许attach前配置，已有child的变化返回Busy。按显式detach/queue destroy/create重附后，核对family缓存与线上checksum；不能以直接改bool冒充在线能力重新协商。Q2覆盖配置与family回调/PF事务/stop交错、命令超时与迟到完成、失败后host停止但PF DMA保留、代次变化不报告旧applied配置。当前所有硬件及长时场景均DEFERRED，开发期不追加全套重跑。
