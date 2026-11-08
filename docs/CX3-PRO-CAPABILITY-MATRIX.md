# CX3 Pro 全能力台账

## 2026-10-25 当前增量：B3a 原生过滤与 checksum .136

新双口controller已接入真实 `setPromiscuousMode`、`setMulticastMode` 和 `setMulticastList`：禁用时暂存，enable及运行中变更经worker进入PF生命周期/try-only transaction，再执行本口QUERY_PORT、SET_PORT qpn_calc与SET_MCAST_FLTR。广播始终纳入正常表；组播名单超过128项显式回退全组播，退出混杂模式恢复名单。忙/读失败在修改前返回且不隐式提交；部分写失败/worker超时停住本口并保留owner，无自动reset/retry。I/O Registry分别发布pending、applied revision/count/mode、状态和FW回执。新接口同时向系统开放既有TX WQE与RX CQE的IPv4/IPv6 checksum生产路径。

**该生产切片 DEV_DONE / SMOKE_PASS / 完整验收 DEFERRED。** 完整kext/CLI构建及实际controller/PF/UC/CLI合成简验通过，覆盖第二口过滤归属、模式恢复、Busy/partial/timeout及checksum元数据。见[本批交付](../../../outputs/20261025-cx3-native-filters/README.md)。未安装/硬件未验；同步IOKit/线程和HW mock不能证明真实调度/线上过滤/校验和。F07/F10整体仍开放（MAC修改、多个单播、运行开关及全协议组合等未完成）；MTU/jumbo、VLAN、电源和C–I继续开发，完整测试最后集中执行。下方较旧增量均为历史。

## 2026-10-25 当前增量：B2c 独立原生双口接口 .135

**B2c DEV_DONE / SMOKE_PASS / 完整验收 DEFERRED。** 新 `Cx3EthernetPort` 为每口提供独立 `IOEthernetController`/接口、workloop/timer、callback admission、TX锁、mbuf记录、RX/TX计数和首错状态；绑定的PF/物理口/generation不切换。真实RX/TX、CQ消费和RQ重贴均使用本口 `Cx3PortQueues`；TX帧DMA仍归PF账本，host detach不冒充设备撤销，不释放DMA。自动媒体项和本口MAC接入系统框架；本批标准MTU、软件checksum，动态MTU/过滤/offload留B3/D。

新增selector54与 `cx3ctl nativeport status|attach|detach PORT EXPECTED_BUILD [json]`，CLI前后绑定boot/service/provider/build/generation。attach需managed队列ready、INIT_PORT及标准wire MTU回执；partial/worker超时保留child和DMA，禁止静默重试。存在child时拒绝legacy raw/reset/reprofile入口；共享PF stop在selector排空后关闭两口callback gate，任一host detach失败保留全部PF依赖。单口detach不动另一口；重新attach须显式队列回收重建。RX错误CQE/不确定索引锁住本口RX，不交付错误帧；TX首错停止本口新发送并继续回收完成。

[本批交付](../../../outputs/20261025-cx3-native-ports/README.md)含完整kext/CLI smoke构建、真实新controller/PF/UC/CLI的双口合成简验。**未安装/加载或调用硬件；双口DHCP、线上过滤/steering、真实IOKit/并发/DMA与稳定性仍未验。** B2代码切片接通不代表F04最终PASS。下一生产项为B3，完整回归、故障注入、100轮与48/72h按独立测试计划集中执行。下方较旧增量均为历史。

## 2026-10-25 当前增量：B2b 每口队列创建/回收 .134

**B2b DEV_DONE / SMOKE_PASS / 完整验收 DEFERRED。** 实际 CQ/RQ/RX-QP/RSS-QP/TX setup/teardown 显式传入物理口；共享 PF 仍统一持有 command/EQ/MR/ICM/MTT。新增 generation 绑定的编号空间：每口完整 MAC 窗口、低端 RSS/TX QPN 及独立 RX/TX CQN 不重叠，拒绝在 legacy 活队列上原地改号。selector53 和 `cx3ctl portqueue status|create|destroy PORT EXPECTED_BUILD [json]` 已接通：创建复用五个生产阶段；失败 sticky、不自动重试/reset；销毁只回收本口，已初始化口先确认 CLOSE_PORT；原生第一口已 attach 时拒绝绕过排空。CLI 前后核对 boot/service/provider/build/generation；旧代次、未知 owner、stop/poison 均不能悄悄重建。

[本批交付](../../../outputs/20261025-cx3-port-queue-api/README.md)包含完整 kext/CLI smoke 构建、真实 RX/RSS/TX 方法的第二口定向简验、生产编排/UC/CLI 合成链。无新候选安装/硬件调用。MAC 仅有合法写入回执，线上 steering 仍待验，旧 autonet 严格门禁未放宽。**当前下一生产任务为 B2c：独立原生 controller/interface、回调/计数与收发绑定；本批管理队列 ready 不等于双口能 DHCP。** 完整硬件、并发、故障注入与长时测试最后集中执行。下方较旧增量为历史快照。


## 2026-10-25 功能增量：B2a 每口真实队列 owner .133

75个实际CQ/RQ/SQ/RX-QP/RSS-QP/TX-QP/MAC及raw收发状态字段从控制器全局成员迁入`Cx3PortQueues fPortQueues[2]`；原生路径显式绑定对象0，没有动态切换全局active-port。两个容器仅在实例init清零，reset/generation变化不清指针。PF command/EQ/MR/ICM/MTT仍由共享owner管理；共享MR依赖检查、ICM/profile释放判断、首次reset判断与reset保留预算/死块观察均检查两口，端口2的保留指针不能漏账。

**队列存储/共享依赖迁移 DEV_DONE / SMOKE_PASS / 完整验收DEFERRED。** 生产RX/RSS/TX/关闭/reset/回收路径的受影响host简验及完整kext/CLI smoke构建通过，端口1建QP不改端口2、端口2保留QP阻断共享MR、保留CQ阻断共享ICM已有定向场景。仅合成transport，无新硬件PASS，详见[本批交付](../../../outputs/20261025-cx3-port-queues/README.md)。**B2整体仍未完成**：第二口创建/销毁入口、CQ/QP/steering不重叠编号、独立controller/interface与每口原生回调尚未接通；本批不宣称端口2能收发。下一项B2b将生产setup/teardown改为显式端口参数并接编号分配，随后B2c独立原生接口。在役.47未改，完整测试继续最后集中执行。


## 2026-10-25 功能增量：B1 每口管理状态 .132

**管理状态切片：DEV_DONE / SMOKE_PASS / 完整验收DEFERRED。** 两个物理口分别保存QUERY_PORT原始结果/时间戳、有效MAC、链路/速率、MTU及INIT/CLOSE回执；端口2管理操作不再覆盖端口1状态。SET_PORT成功后才记录MTU；查询失败保留历史raw并标最新查询失败；generation变化使观察/配置回执失效。共享CLOSE_HCA要求所有实际端口各自已确认CLOSE_PORT。追加selector52和`cx3ctl portinfo PORT [refresh] [json]`，默认host cache，refresh才发固定QUERY_PORT。详见[实现与简验](../../../outputs/20261025-cx3-port-state/README.md)。

简验只覆盖生产管理/ABI/CLI及受影响的关闭、reset代次路径，底层传输为合成；完整kext/CLI smoke构建通过，未安装/调用新硬件入口。**这不是双口资源隔离或双口联网完成**：CQ/RQ/SQ/QP、MAC/steering编号与原生接口仍是全局单口，端口2的qpn_calc尚未独立。下一项B2：先建立不重叠的每口资源编号和queue owner，再接独立原生controller/interface；共享PF command/ICM/MTT由共同owner仲裁，不通过临时切换全局port索引复用。完整双口/MTU/关闭场景留最终集中验收，.47在役配置未改。


## 2026-10-25 功能增量：自动启用入口 .131

已接通追加selector51的一次初始化事务、版本/service/provider/generation内核绑定、逐阶段payload判定、sticky失败与status/enable/disable。复用16个现有生产export，使用QUERY_FW返回页数，不再写死1542；完整RSS/QP回读不符会在发布接口前停止。首次仅限空实例/端口1/标准MTU；重复enable恢复已有接口，不重新reset；disable暂停主机交付，保留队列，不是DMA静止或CLOSE_PORT证明。堆上scratch承接大QP结果。配套CLI和默认关闭的一次性Python服务已接通，服务按返回接口/MAC校验后才设置up/DHCP；DHCP请求不等于取得租约。launchd模板已生成但未安装，无KeepAlive或失败重试。详见[实现与简验](../../../outputs/20261025-cx3-auto-network/README.md)。生产编排/CLI合成transport简验、服务假ifconfig简验及完整smoke构建通过；新入口尚未上机，F03硬件验收/正式安装和安全模式仍未完成。当前.47运行配置未改。**下一生产项转B：端口对象与双口资源隔离，随后补齐L2/MTU/VLAN/checksum；A2新入口完整测试留最后集中执行。**


## 2026-10-25 进度记录覆盖

[功能优先v4](CX3-PRO-FEATURE-FIRST-PLAN.md)将“生产实现 / 简单验证 / 完整验收”分列；完整测试集中最后执行，见[测试计划](CX3-PRO-FINAL-TEST-PLAN.md)。下表旧状态为已保存的证据级状态，不因开发策略改变升级为PASS。适用项的DEV_DONE不以完整验收先通过为前置，空接口/unsupported占位不记实现；硬件/平台阻塞如实保留。无新增硬件功能PASS。

日期：2026-10-22；对应[总计划](CX3-PRO-FULL-ADAPTATION-PLAN.md)。这是待核验的最低能力目录，不是本卡全项支持声明。

## 状态与证据规则

2026-10-25 `.129`：[TX 首错保护](../../../outputs/20261025-cx3-tx-fault-containment/README.md)仅离线通过：首个ERROR锁存CQE/WQE并停止新提交，后续完成继续回收，down/up不清latch；12场景48检查、四个破坏反例、57脚本与完整构建通过，92项冻结输入一致。未安装或硬件验收，首次QP错误根因及长期稳定性仍开放；新boot临时DHCP恢复已通过下列短时验证，F02/F06不新增PASS。

2026-10-25 06:04当前更新：[既有.47临时恢复](../../../outputs/20261025-cx3-temporary-dhcp-055750/README.md)成功，en2 DHCP192.0.2.201，绑定该接口网关/外网3/3及HTTPS200；en0/.101仍为默认出口。观察正常结束，不提升长期稳定性等级。F01另补[USB被动Linux身份/PCIe快照](../../../outputs/20261025-cx3-usb-passive-followup/REPORT.md)：05:34采集、05:38新macOS boot之前，8GT/s×4（最大×8）；无mlx4绑定，PSID/FW未知。不能代替macOS当前PCIe状态。

2026-10-25前一boot运行更新：用户授权恢复的仍为0.44.47，01:06 DHCP及绑定en2外网HTTPS通过；05:11只读复查却为169.254/no-server，现有日志01:46:08首次TX CQE error（0x02/0x6d），其后连续0x05/0xf9，RX仍在增加。见[新事件证据](../../../outputs/20261025-cx3-dhcp-loss-readonly/analysis.json)。短连通性不能升级为稳定日用PASS；原联网历史保留，但该boot后来已退化，优先诊断TX故障，不自动reset或更换候选。

2026-10-24 `.128`：[RSS身份绑定验收](../../../outputs/20261024-cx3-rss-runtime-binding/README.md)完成同连接identity→单次RSS→identity、独立解码及候选collector。最终query成功回执是操作成功的必要条件；status0设备拒绝不再误判。56脚本/构建/91输入及GPT限定复审通过；仅离线，无新硬件PASS。F15及R04仍需真实raw、普通QP/key对照和RSS语义。

2026-10-24 `.127`：[启动准入/停止合同](../../../outputs/20261024-cx3-network-stop-contract/README.md)离线通过。真实start拒绝retained/open/非零generation旧实例，provider open失败不再启用PCI或进入HW init/发布；新实例不是设备恢复凭据。[资源矩阵](reports/stop-reopen.md)及0/末槽/全槽pending组合已建立；51脚本/完整构建、85项冻结输入及GPT Luna/high限定复审通过。F02仍I；真实provider仲裁、DMA/IOKit、stop/reopen与100轮验收未完成，没有新增硬件PASS。

2026-10-24 `.126`：[正常FW关闭](../../../outputs/20261024-cx3-fw-normal-close/README.md)完成离线验收：同代FW严格UNMAP成功后释放，host-only无命令释放，失败保留原owner/依赖，随后既有命令shutdown；不清RUN_FW sticky或poison。50脚本/完整构建、84项冻结输入及GPT Luna/high复审通过。F02仍I，完整stop/reopen、真实DMA/provider/框架与恢复未验收，无新增硬件PASS。

2026-10-24 `.125`：[正常端口/HCA关闭验收](../../../outputs/20261024-cx3-normal-close/README.md)通过。端口关闭→子对象交还→CLOSE_HCA→既有table/AUX UNMAP，失败保留owner；INIT前失效port1旧回执，HCA初始化期间禁止ICM释放。49脚本/完整构建、83项冻结输入及GPT Luna/high最终增量复审通过。F02仍I：FW仍保留，完整stop/reopen、真实DMA/框架/provider和恢复未验收，无新增硬件PASS。

2026-10-24 `.124`：[原生接口启停离线验收](../../../outputs/20261024-cx3-network-lifecycle/README.md)通过。enable/disable参加stop callback drain；reset/stop阻断下不启用，closed disable为family detach幂等返回。实际方法16场景471检查含100轮host owner复用，48脚本/构建及82项输入通过，GPT Luna/high限定复审完成。F02仍I：接口down/up不同于整个驱动stop/reopen，CLOSE_HCA/完整撤销、真实DMA/框架/恢复未验收，无新增硬件PASS。

2026-10-24 `.123`：[运行身份与候选绑定离线验收](../../../outputs/20261024-cx3-runtime-identity/README.md)通过。selector49、identity/boundpcie、独立decoder和候选采集器提供boot/build/service/provider/generation与PCIe raw的可核对关联；47脚本、完整构建、81项冻结输入通过，GPT Luna/high限定复审完成。F01/F37/F43仍仅实现中；真实候选采集、RSS raw绑定/硬件语义、正常stop/reopen/recovery及其他原有验收义务保持开放，没有新增硬件能力PASS。当前候选身份见[会话权威摘要](CX3-PRO-SESSION-GOAL.md)。

2026-10-24 `.122`：F02保持I。FW重用失败保留原owner、诊断分配入口禁止释放已handoff owner、共享生命周期准入及CLI失败传播完成离线修复；RUN_FW尝试后仍无在线替换/恢复合同。[93场景753检查、42脚本及构建/冻结源验收](../../../outputs/20261024-cx3-fw-replacement/README.md)通过，独立GPT Luna/high生产复审未发现具体阻断。先前.121段落中的FW调用者未修缺口由本批局部关闭；正常stop/reopen、真实DMA/IOKit/provider、预算和硬件/长期验收未完成，没有新增能力族PASS。

2026-10-24 `.121`：F02保持I。成功retention后的本地账本容器/provider引用与阶段间停止保护完成离线验证（41脚本/完整构建/73项输入归档一致，GPT Luna/high mailbox/stop范围复审）。[证据](../../../outputs/20261024-cx3-retained-ledger/README.md)。只读加载清单确认实际仍`.47`，没有重测M4流量；未安装新候选。已发现exportFwLoad失败UNMAP仍free的独立未修缺口；正常stop/reopen、实际provider/框架/DMA静止和预算仍未完成，无新增硬件能力PASS。

2026-10-24 `.120`：F02仍为I。停止失败组合、全表初始化和raw-frame owner检查完成有界离线修复，41个Python脚本及完整构建通过，73项输入前后/归档一致，GPT Luna/high复审通过。[验收及边界](../../../outputs/20261024-cx3-stop-composition/README.md)。没有安装/加载或新增真机能力PASS；成功retention后的跨stop/provider存活、实际框架/设备静止、预算及正常/异常释放凭据仍未完成。下列旧版本段落为历史记录，候选状态以上述入口和会话权威摘要为准。

.90离线增量：FLR改用公开平台事务，失败后零设备访问；成功仅配置诊断，owner
不读、不清poison、不释放，保持阻断。32组及构建exit0，65条live/tar清单及两个
binary一致；未上机，无新增能力PASS，F02/R06及P0/P1仍未完成。

.91增加调用前配置底账拒绝：异常identity/BAR或PCI memory decode关闭不触发reset。
96场景552检查、32组离线回归和构建通过；无新增硬件能力PASS，R06/P0/P1保持未完成。

.89最新离线增量：失败/旧owner reset及手写FLR后阻断后续设备selector，仅保留
host-only netstat；无owner首次成功HCA reset可继续原初始化链。reset81场景459
检查及selector157场景471检查0失败，30组及构建exit0，未上机。不代表真实恢复，
无新增硬件能力PASS，F02保持I，R03/R06及P0/P1未完成。

.88最新离线增量：命令锁改为init预分配，缺锁的三种命令入口均拒绝；13场景104
检查0失败，旧.87重放42失败。30组及构建exit0、63条live/tar清单与两binary一致。
启动路径检查仍PASS；无硬件操作，无新增能力PASS，F02保持I，P0/P1未关闭。

2026-10-23最新候选.87承接.86的QP观测接线修复，增加第8参数指针的deleted模板重载；两类编译负例不加Werror均被拒。29组及构建退出0（verification-batch41.json），62条清单live/tar及两个binary一致。只补类型防护，无新增能力PASS。
.84/.85不部署，其真实QP观测接通结论撤销，旧测试签名遗漏导致假绿。.85独立consumer离线测试仍有效，但不能证明真实生产采集。
.86真实QP/HCR集成17场景186检查0失败，旧.85同测试42失败；29组及构建退出0，62条live/tar清单和两个二进制一致。boot/运行/代次身份、实际安全与真机语义未完成，无新增能力PASS，R04/P0/P1不关闭。
F02仍实现中：实际框架/timer/IRQ并发、provider终止/断电、预算/代次和设备静止未证明，
无新增功能PASS。身份与回归统一见[续跑入口](CX3-PRO-SESSION-GOAL.md)，证明边界见[安全审计](reports/safety-lifecycle-audit.md)。

硬件列：`K`=本卡存在直接证据（仅限描述范围），`C`=官方家族/型号或参考源码中的候选，`U`=未知。`C` 不能自动变为 `K`。
进度列：`M4`=仅基线切片通过，`I`=部分代码/仪器存在、尚无完整验收，`O`=本计划尚无验收证据。
该快照没有任何一族已经达到“全功能 PASS”；后续逐子项记录 `PASS / FAIL / UNKNOWN / BLOCKED-PLATFORM / BLOCKED-LAB / N/A-HW`，另记实现版本与证据 URI。

下表“最低验收”须与总计划共同验收契约合用；每族中的 transport、速率、端口、协议、组合分别拆子项。硬件原始位、接口文档、Linux 行为、macOS 实测是不同来源，不能互相冒充。

| ID | 能力族 / 必须追踪的子项 | 硬件证据 / 当前进度 | 开放方式；阶段 / 任务 | 最低验收 |
| --- | --- | --- | --- | --- |
| F01 | 身份/资源：PN、PSID、FW、PCIe、portcap、QPC/CQC/EQC格式、配额、ICM | K：PN/双口/FW历史读数；[Linux被动身份/PCIe快照](../../../outputs/20261025-cx3-usb-passive-followup/REPORT.md)；PSID U；I | 工具；P0 / T01 | raw+解码可对账，所有特性位/限制映射入本台账 |
| F02 | 初始化、命令、DMA/IRQ、PD/MR/QP/CQ/EQ 生命周期与故障恢复 | K：手工链；I | 内核核心；P1 / T02、T05 | 失败注入/并发退出/超时不UAF、不泄漏、不继续错误DMA；恢复或安全隔离 |
| F03 | 自动启用、可禁用、安全模式、服务退出、版本/ABI协商 | U：软件路线；I：.131单口自动入口/默认关闭服务已生产实现与离线简验，未上机；安全模式/正式安装仍开放 | 原生+控制服务；P2 / T06 | 开机无手工命令；服务崩溃/版本错配不误初始化；禁用能启动 |
| F04 | 双端口独立接口/资源、混合负载、共享PF仲裁 | K：Linux两口；I：B1管理状态及B2a真实队列owner存储已实现/离线简验；每口setup/编号/独立接口未完成 | 原生；P2 / T07 | 两口同时双向收发，单口down/重开不损坏另口 |
| F05 | 速率/媒体/autoneg、link事件、模块/QSA/线缆兼容 | K：macOS已测10G；C：型号40/56G；I | 原生；P2–P3 / T07 | 每个本卡支持且实验室具备的速率独立连通/吞吐；链路事件正确发布 |
| F06 | L2、DHCP、ARP/ND、IPv4/IPv6、广播、标准MTU | K；M4 | 原生；P2–P3 / T07、T08 | 保留M4回归，双口、DHCP重租/地址变更、TCP/UDP并发通过 |
| F07 | MAC变更、多单播地址、组播表/溢出、allmulti、promisc | K：自身单播/基础组播；I | 原生；P3 / T08 | 正常过滤、无泄漏；mDNS/IPv6组播；表满/切换与回退有线上证据 |
| F08 | 可变MTU/jumbo、边界帧/超长帧、在线重配置 | C；I | 原生；P3 / T08 | cap范围内各边界、双向DF/线上包长正确；反复变更无旧DMA/缓冲越界 |
| F09 | VLAN插入/剥离/过滤、多VLAN、QinQ/S-VLAN按cap | C；I | 原生优先；P3 / T08 | 远端tag正确、隔离/过滤正确、无重复tag、MTU含tag边界覆盖 |
| F10 | RX/TX checksum：IPv4/6、TCP/UDP、分片/异常包 | C；I | 原生；P3 / T09 | 开关A/B、栈metadata、对端抓包；坏包不能误报已校验 |
| F11 | TSO IPv4/IPv6、扩展头、VLAN组合、异常长度 | C；O | 原生；P3 / T09 | 大TCP write线上分段≤MTU，checksum/重传正确；不是ping验收 |
| F12 | SG/DMA映射、对齐/边界、inline/BlueFlame等TX优化 | C；I：现有DMA通路 | 内核核心；P3 / T09 | 多段/非连续内存/高地址压力、pin/unpin与CQ回收正确；优化不改包内容 |
| F13 | RX聚合/LRO/GRO、零拷贝/缓冲复用候选 | C：须逐项查证；O | 原生或SDK；P3 / T09 | 有适配接口才启用；TCP语义/转发/抓包/内存归属正确，软件聚合不冒充硬件 |
| F14 | CQ中断、MSI-X映射、再arm、moderation、自适应与轮询回退 | K：EQ中断；I：RX轮询 | 内核核心；P3 / T10 | 无丢唤醒/中断风暴；空闲CPU/高负载对照；回退/退出顺序可证 |
| F15 | RSS hash/key/indirection、IPv4/6 TCP/UDP/分片、多RX队列 | C；I：单环RSS入口读回异常 | 驱动内部+工具；P1、P3 / T03、T11 | raw QPC语义明确；测试向量+每队列计数证实分流、流内有序 |
| F16 | 多TX队列、每队列回压/完成、并发调度 | K：Linux16TX；O | 原生内部调度；P3 / T11 | 多流并发真实使用多环，回压/公平性/流内有序、单队列故障隔离 |
| F17 | Flow steering/DMFS/A0、精确过滤、组播/镜像/sniffer按cap | C；O | 原生过滤+管理工具；P3–P4 / T11、T12 | 逐条规则命中/冲突/容量/删除，模式切换可回滚；不依赖未证明的steering假设 |
| F18 | 桥接、bond/LACP、路由/NAT、抓包、系统网络配置 | U：系统集成；O | 原生；P3 / T08 | macOS实际可用模式分别互通；LACP须对端；功能关闭后无残留配置 |
| F19 | 硬件RX/TX时间戳、硬件时钟、回绕/频率与时钟域 | K：Linux注册PHC；O | SDK/工具，原生接口可行时接入；P4 / T12 | 确认硬件来源，线上帧关联、回绕与漂移，参考时钟量化误差 |
| F20 | PTP/gPTP/AVB、定时发送/TSN候选及平台集成 | U，不能从F19推断；O | 原生可行部分+SDK；P4 / T12 | 分清时戳/同步/调度能力；支持项与Linux对端同步并测误差，不支持项有cap证据 |
| F21 | Pause/PFC、优先级/队列映射、拥塞计数 | C；O | 原生/工具；P4 / T12 | 拥塞/优先级隔离/死锁恢复；开关A/B；不能开流控掩盖丢包 |
| F22 | ETS、DCB/DCBX、QoS/限速、QCN、SL/VL映射按cap | C；O | 管理服务/工具；P4 / T12 | 交换机/对端协商、带宽份额/上限、拥塞反馈有线上与计数证据 |
| F23 | VXLAN/NVGRE/IP-in-IP等隧道解析、内外层checksum/TSO/RSS | C：官方历史FW；O | 原生可行部分+工具；P4 / T12 | 仅公开本卡支持组合，真实封装包A/B、分段/过滤正确，未知协议不误卸载 |
| F24 | RDMA对象：context/PD/MR/MW/CQ/QP、SRQ/XRC、事件 | K：现有基础PD/MR/CQ/QP；非verbs；O | SDK+内核隔离；P5 / T13 | 对象生命周期/依赖/配额、跨进程拒绝、异常退出撤销DMA、ABI协商 |
| F25 | RDMA transport：RC/UC/UD、SEND/RECV、READ/WRITE、atomic | C；O | SDK verbs；P5 / T14 | 每种支持transport/操作和方向与Linux互通，内容一致、超时/重试正确 |
| F26 | MR/MW高级能力：FRWR/FMR、远程失效、rkey、inline按cap | C；O | SDK verbs；P5 / T14 | pin权限/撤销/远端越界/旧key/并发失效测例；不支持ODP等不得宣称支持 |
| F27 | RoCE v1/v2、GID、CM、VLAN/IP/MTU、组播与路由 | C：官方FW历史/本机mlx4_ib；O | SDK+网络控制面；P5 / T14 | 真正硬件RDMA与Linux双向互通，v1/v2分别验；TCP转发不是RoCE |
| F28 | RDMA与普通Ethernet并发、拥塞/链路抖动/恢复 | U：尚无验收；O | 原生+SDK；P5 / T14 | 多进程与网络同时压力，无资源串用；断链/服务崩溃后可恢复或明确隔离 |
| F29 | VPI、ETH/IB单口/双口/混合模式、能力与模式持久化 | C：本型号官方VPI；本卡portcap待核；O | 管理工具；P6 / T15 | PN/PSID/portcap一致；支持配置逐项切换/回滚；不默认刷FW |
| F30 | IB链路/速率、MAD/SM、PKey/GID、计数/诊断、APM按cap | C；O | SDK+管理工具；P6 / T15 | 真IB/FDR对端与SM、分区隔离、link事件/诊断、支持的迁移能力通过 |
| F31 | IB verbs、UD组播、RC/UC等transport与RDMA | C；O | SDK；P6 / T15 | 与Linux IB对端逐种互通，错误PKey/QKey拒绝，不能以RoCE结果代替 |
| F32 | IPoIB、MTU与connected/datagram模式按cap/栈支持 | C；O | 原生IP接口或经决策的配套服务；P6 / T15 | macOS应用经真实IB链路IPv4/6互通，地址解析/组播/MTU与性能通过 |
| F33 | SR-IOV PF管理：VF枚举、资源配额、MAC/VLAN/GUID与mailbox | C；O | PF驱动+管理工具；P7 / T16 | 创建/删除/复位VF与PF并发，配额/权限与mailbox隔离，状态可回收 |
| F34 | VF驱动/guest直通、IOMMU隔离、恶意VF、宿主恢复 | C：硬件候选；平台U；O | 驱动+受支持hypervisor；P7 / T16 | guest真实收发/RDMA、DMA隔离/复位与恶意输入测试；枚举不算可用 |
| F35 | PME/WoL、D状态、待机供电、唤醒包过滤 | C；O | 原生电源+工具；P2、P7 / T07、T16 | 独立验证sleep/shutdown支持状态、双口/速率/唤醒条件，不误唤醒 |
| F36 | 热量/传感器/模块EEPROM、DOM、LED/beacon/线缆诊断 | C；I：.130 page0 GET/离线DOM，未实测；其余开放 | 只读工具为先；P0、P4 / T01、T12 | 有支持则与Linux/外部参考一致；缺传感器不能伪造0值；写类操作受控 |
| F37 | 统计/错误事件、资源dump、自检、固件健康与恢复工具 | K：已有部分计数；I | 工具+探针；P1、P4 / T04、T12 | 逐原因计数、回绕/复位语义、现场持久化、故障重建可对账 |
| F38 | 固件/NVRAM/PSID配置与兼容性管理、备份/恢复 | K：FW可查询；I | 只读盘点；写入独立受控工具；P0、P7 / T01、T16 | 读取不变状态；确需写的能力列出备份/校验/掉电恢复及另行授权，禁止自动跨PSID烧写 |
| F39 | GPUDirect RDMA、peer memory、PCIe P2P/第三方设备内存 | C：型号手册；平台/GPU接口U；O | SDK+相关GPU/设备接口；P7 / T16 | 真peer-memory路径、隔离与撤销；host staging不算GPUDirect，平台不支持记阻塞 |
| F40 | CORE-Direct/任务链、同步、其他cap发现的专有offload | U：不能由手册商标证明；O | SDK，按原始cap扩展；P7 / T16 | 有本卡依据再实现；硬件执行/完成次序与故障取消可证；无依据保持未知 |
| F41 | SMBus/I2C/NC-SI/sideband、管理控制与板级接口 | C：型号/家族候选；O | 工具或平台管理接口；P7 / T16 | 区分本板布线与芯片功能，确证适用项端到端；无布线有证据才N/A |
| F42 | PXE/UEFI/FlexBoot、网络/iSCSI/IB引导及OS接管 | C：选装ROM/平台候选；O | 预启动组件与macOS接管协同；P7 / T16 | 不重写macOS驱动冒充ROM；适用ROM的启动/交接/失链恢复可证；持久ROM变更另授权 |
| F43 | RDMA应用生态：verbs兼容层、CM、perftest与示例、存储/HPC消费者 | U：软件集成；O | 配套SDK/工具；P5–P8 / T13、T14、T17 | 原生macOS程序可编译运行、Linux互通；SMB Direct/NFS-RDMA/MPI等逐消费者决策，不由verbs通过自动宣称支持 |
| F44 | 安装/回滚、签名/entitlement、版本支持、许可、长期回归 | K：开发机可构建加载；I | 发布包/源码/SDK；P8 / T17 | 干净环境可复现，支持矩阵真实，72h与全部功能报告齐全，恢复方案通过 |

## 更新操作

2026-10-23第二批：F02新增reset后一律保留、stop不完整时不拆卸、poison mailbox跨shutdown保留的候选实现及执行/源码测试；F02仍为`I`，因为这些是离线策略与分支，未观测真实IOKit对象、迟到selector、worker回调或硬件timeout。R01证据归档完成。详见[安全审计](reports/safety-lifecycle-audit.md)。

同日R06：FLR六条件模型8检查先红后绿，发现现手写路径缺TRPND和平台reset事务；仍无生产凭据或同域实测，见[FLR契约](reports/flr-quiescence-contract.md)。

2026-10-23执行复核：0.44.72完整离线回归通过。F02已补reset静默释放、QP失败owner、mailbox不可复用及CQ/MR/ICM依赖保护、容量/MTT/EQ保护，.70/.71补QP owner/readiness/query门禁，.72补RX/RSS撤销先清ready、归还/保留失败仍持有owner。QP链105场景1166检查、retention24场景89检查0失败，重放.71分别10/4检查失败；TX35场景302检查仍通过。预算/代次、停止/锁/回调/provider存活和实际静止凭据仍开放。F15已补完整query邮箱、普通QP采集和IOKit大结果descriptor路径及失败测试；QPC实际248字节（旧256字节槽末尾8字节为padding），元数据/有效性/版本及独立解码闭环、真机raw及历史RSS异常解释仍缺。两族继续为`I`，没有新增PASS。能力字段/PSID/port2/模块缺口见[能力基线](reports/capability-baseline.md)，具体续跑见[当前任务表](CX3-PRO-BACKLOG.md)。

2026-10-22首批增量：[T01核正基线](reports/capability-baseline.md)为F01/F05/F15/F19/F23/F35等提供**声明级/历史**读数，未改变功能通过状态；机器清单位于外层`outputs/20261022-cx3-fulladapt-batch1/capabilities.json`。全部flags64位已保留。该日快照中的 ext2/bmme/portcap 映射缺口由 2026-10-23 能力基线报告更新：22 个位及 port1 已解码；剩余配额字段、port2、PSID、模块/拓扑等仍未完结，具体以[能力基线报告](reports/capability-baseline.md)“仍缺什么以及如何取证”为准。能力声明仍不等于功能端到端通过。
F02新增[安全审计](reports/safety-lifecycle-audit.md)与4项失败要求，P1-S未通过；F15见[RSS报告](reports/rss-qpc-layout.md)，D03不关闭；F37见[门禁报告](reports/hardware-test-safety-gate.md)，观察者在线不替代通知/phase回执。没有能力族因此晋升为全功能PASS。

2026-10-23后续复核：.79增加reset前库存/容量/重复owner及本地生命周期预检，
reset尝试后拒绝再次reset；.78锁外读取attach状态已被反例否定并取代。身份与离线
结果见[续跑入口](CX3-PRO-SESSION-GOAL.md)，局部证明边界见[安全审计](reports/safety-lifecycle-audit.md)。
后续.80将FLR接入预检/IRQ拒绝和sticky尝试门，移除其无凭据清mailbox poison；离线
通过不代表初次手写FLR已符合平台事务。仍非全局预算、完整代次/poison协议或设备静止
凭据。F02仍为`I`，无新增硬件PASS。

.81继续补真实mailbox poison与synthetic注入的来源区分，idle寄存器或HCA reset
不能授权清除真实poison；纯注入撤销与正常干净路径仍通过。离线结果见续跑入口，
不增加硬件PASS，不以“拒绝恢复”关闭最终恢复/全功能目标。

P0 输出机器可读清单并附 raw 数据，逐一为 Fxx 增加支持依据、子项状态、实现版本、报告路径、所需外设和阻塞决策链接。
例如“F15 IPv4/TCP两队列 PASS”不关闭 F15 的 IPv6/UDP/分片/重配等适用子项。
若原始 capability 出现本表没有的能力，新增 F45 起的稳定 ID；不重编号既有 ID，不删除失败历史。
某功能暂时只提供用户态接口时，明确记录其与原生接口的差异和应用限制；用户已接受 SDK 路线，不等于接受功能缩水。
