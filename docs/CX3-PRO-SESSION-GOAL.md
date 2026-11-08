# CX3 Pro 会话执行目标

## 2026-10-25 最新增量：WAVE2 .138 / WAVE3 .139

功能优先v4继续生效。MTU/jumbo生产切片、VLAN TX插入/RX元数据与内联规范化、端口硬件白名单、原生MAC修改、RX checksum运行标记与TX attach前配置已完成代码和离线简验，完整验收DEFERRED。候选tag `0.44.139-tahoe-l2-local`，bundle数字版本仍 `0.44.48`，不与现场loaded `.47` 混账。交付和真实退出码见[WAVE3](../../../outputs/20261025-tahoe-acceptance/cx3/WAVE3.md)，前批几何与资源限制见[WAVE2](../../../outputs/20261025-tahoe-acceptance/cx3/WAVE2.md)。本批无安装/初始化/网络切换/重启。

下一生产队列：链路动态更新、正常down/up与电源；在线MTU ceiling重建、多单播steering和TX能力在线重新协商仍开放，C–I按依赖并行推进。每批完整编译及关键正/拒绝简验；全量host/故障组合/100轮/48h/72h最后集中执行。历史“当前增量”在下方只作历史，不覆盖此段。

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


## 2026-10-25 功能增量：模块EEPROM/DOM开发切片 .130

已接通固定MAD GET→内核/selector50→CLI→离线模块/DOM解码，详见[实现与简验](../../../outputs/20261025-cx3-module-get/README.md)。既有0–49编号不变；支持page0、I2C0x50/0x51、48字节内分段与完整错误诊断。SFP内校准DOM、QSFP低页监测值已解码，缺字节/外校准/未就绪不伪造数值。扩展上页、外校准换算、阈值/告警等F36子项仍待补，未宣称F36完成。完整kext/CLI的smoke编译、生产路径定向简验与解码简验通过；不是新增硬件PASS。`.130`未安装，`.47`临时网络保持。下一生产项继续A2自动启用产品入口，完整回归/硬件DOM对照留最终测试计划，不因本批未完整验收阻断开发。

## 2026-10-25 执行策略覆盖：功能开发优先

用户明确要求“先把所有功能项做完，测试项简单验证，完整测试单独规划最后执行”。后续以[功能优先v4](CX3-PRO-FEATURE-FIRST-PLAN.md)为执行队列，以[集中测试计划](CX3-PRO-FINAL-TEST-PLAN.md)为最终验收；下方旧版“立即下一项”和阶段全量测试前置不再支配日常开发。每批只编译/ABI/所改功能关键路径，不重复全量host、mutation或独立复审；长时/压力/故障注入/完整组合最后集中做。进度按“生产实现/简单验证/完整验收”三列报告，不按测试数量、版本或归档数量。

**当前下一生产任务：B2c独立原生接口与每口收发绑定；B2b每口setup/teardown与编号、B2a真实queue owner存储、B1管理状态、A1/A2入口切片已离线简验，F03/F04/F36完整验收尚未完成。** 依赖只到所用接口和语义；其余功能可独立提前开发。保留既有.47临时网络，新候选默认不自动启用。`.129`资源包已补唯一服务实例及逐阶段结果门禁，13项host测试通过；独立观察回执adapter未接，真实硬件execute已显式禁用，不再为此反复扩建验证基础设施。没有新候选硬件授权，首次TX错误根因和完整验收仍未完成。

## 当前启动实例更新（2026-10-25 05:38:35）

系统已进入新boot，.47身份保持AA1A2B9E-6BA0-3A54-A28B-553A4F12C6A2；新boot最初en2不存在，冻结.47启用链的fresh-instance只读预检PASS。用户明确授权“工作虽然没有暂停…建议可以进行”，已如实记录workPaused=false；本次仅恢复既有.47与DHCP，使用独立.101/UDP19947观察器，不改生产探针配置。已成功恢复：en2取得DHCP 192.0.2.201/24，绑定该接口的网关/外网ping均3/3，Apple HTTPS200；默认出口仍en0/.101。本机观察正常结束，远端339条记录、正常end且无心跳告警，两观察器均已退出。短时连通不等于长期稳定性，证据见 `outputs/20261025-cx3-temporary-dhcp-055750/`（父工作区）。下方01:46 TX错误和05:11 DHCP失效属于前一boot，不能当作当前已启用状态。.129补充验收为13场景53检查、57脚本及重建通过，二进制与原.129相同；未安装。

## 首错消费与参考差异补证（2026-10-25；仅工具更新）

[首错日志decoder](../../../outputs/20261025-cx3-tx-fault-decoder/README.md)已接入离线测试入口：实际生产logger生成合成日志后独立解码，13 tests OK；原保护测试13场景53检查仍通过。拒绝截断/重复/摘要与raw冲突，保留不明确vendor与异常WQE；尺寸须同次采集提供，运行身份未验证、推断post counter非精确记录均显式输出。真实旧.47日志因缺原始快照被拒绝，未用合成数据冒充根因复现。仅工具/测试/build gate变更，16个生产输入及冻结.129二进制不变；未重建、安装或执行新selector。

[GPT Luna/high参考复核](../../../outputs/20261025-cx3-tx-reference-contract/reference-review.md)逐行确认普通MMIO producer编码、data字段发布次序、最近完成WQE延迟stamp及headroom四类差异。producer偏差原已记载，并非本轮新发现；本卡bf_reg_size=0的历史实测保留。普通DMA byte_count无独立bit31 valid，不能按该假设修复。四类差异都未与首次0x02/0x6d建立因果。下一项是将候选/hash/ABI、同次身份、SQ尺寸、首错raw及外部观察/停止条件绑定为最小实机采集包；先审查既有.47与.129之间差异和每个实验单变量，不直接把所有参考差异一起修改。

## 最新离线切片：TX 首错保护 .129（2026-10-25）

补充[.47 二进制原语对照](../../../outputs/20261025-cx3-tx-primitive-audit/README.md)：已安装.47身份/原构建hash一致；与冻结.129的五个TX/CQ原语及六个内部helper共11对、557条原始指令，在仅规范化地址重定位后相同。生产编码器host检查通过四个65536周期及边界输入，三种实际字段破坏被检出；没有复现首次0x02或证明设备时序正确，不外推整个驱动同源。.47实际error分支仅增加计数/打印后继续回收的控制流已另存反汇编。当前探针主动目标仍旧.196/.130，被动beacon来自.101，硬件试验前须修正身份对齐。

[.129 验证](../../../outputs/20261025-cx3-tx-fault-containment/README.md)：首个 TX ERROR 保存原始 CQE 与 stamp 前 WQE，锁内停止新提交、保留完成回收与 RX；接口 down/up 不清除故障。生产方法行为测试12场景48检查通过，最终fixture对旧.128有25个行为失败，四种破坏均被检出；全量57脚本与完整构建exit0，92项输入冻结一致。仅离线，未安装；首次0x02/0x6d根因、实际恢复与日用稳定性仍未完成，F02/F06不关闭。前一boot的.47曾DHCP失效；05:38新boot已按顶部记录恢复。冻结启用链要求fresh instance，不能重复初始化已附着接口。补充验收增加fault后成功CQE回收场景，最终13场景53检查；最新输入清单以supplemental-acceptance为准。

## 前一启动实例运行回归（2026-10-25；当前状态见顶部）

用户授权临时恢复的0.44.47在01:06通过DHCP192.0.2.201与绑定en2的外网HTTPS200；[当时实测](../../../outputs/20261025-cx3-temporary-dhcp-20261025-010316/README.md)保留。05:11只读复查已为169.254/no-server；现有kernel日志首次TX错误为01:46:08.272、consumer197208、syndrome0x02/vendor0x6d，之后为连续0x05/0xf9，RX仍增长，见[事件分析](../../../outputs/20261025-cx3-dhcp-loss-readonly/analysis.json)。该启动实例后来失去联网，不能称稳定基线PASS；05:38新boot的临时恢复结果见顶部。

接续优先定位首个TX错误及故障后的继续发送行为；不把后续批量错误当作独立根因，也不把197208靠近16-bit回绕当作已证因果。en0仍承载默认出口，当前地址192.0.2.101；60主目标仍指向旧.196，心跳能见.101但主动探测已漂移，探针配置未改。当时没有新实验许可，未reset、重走初始化或切换驱动；后来新boot的明确授权仅用于既有.47临时启用，不覆盖新候选试验。下方.128是未加载的离线候选状态，与此运行退化分别记账。

## 当前执行覆盖（.128，2026-10-24）

最新完整离线候选 `0.44.128-local` 见[RSS运行身份绑定验收](../../../outputs/20261024-cx3-rss-runtime-binding/README.md)。CLI同连接identity49→单次RSS35→identity49；独立decoder核对boot/build/service/provider/generation、ABI/scalar及每条命令时间；候选collector复用hash/loaded UUID/OS boot前后检查，保留原字节和失败/超时，不重试。只有最终QUERY_QP回执完整成功、RTS/QPN等一致才记操作成功，status0设备拒绝仍只是诊断。此命令有副作用，本批只在stub transport执行；未改kernel或ABI。

最终56个Python脚本、完整构建、91项输入前后/tar及独立字节核验通过；GPT Luna/high七脚本47项复审未发现新的可达误判，修正前旧验收单独保留。kext SHA `424d870916919de34e048e55b47d28d4f22400baa1559f8a24947b007f7e2c62`，CLI SHA `1783441d429f7113da7d532da69111d1852dd37b30883fa0e8510ce198b653cb`，kext UUID `205533F3-D933-30FB-8386-2B0B23243FB2`；bundle/kmod仍0.44.48，未安装/加载。

[固定Apple源码核查](../../../outputs/20261024-cx3-provider-stop-source/REPORT.md)收窄了superclass stop疑点：公开stop不close PCI provider，PMstop有实际PM callout/power-plane语义，network gate/workloop释放在free；源码未匹配本机二进制，目标callback/provider/open仲裁仍开放。[14:42事件](../../../outputs/20261024-freeze-1442-readonly/REPORT.md)经用户确认是人工强制重启；未捕获panic、根因未定，约32秒本机快照缺口未关闭，不能归因未加载候选。

下一项：最小实机包及观测/采集进度和错误回执设计、真实child/table撤销组合；包须绑定候选/hash/ABI/单变量白名单、独立观测与新回执、停止/恢复。R04 RSS身份绑定离线子项已实现不重做，真机raw/普通QP对照/key A/B和RSS语义仍未完成；R05模块GET可离线推进。真实DMA/provider/框架、stop/reopen、预算/异常恢复及F01–F44未完成；没有新硬件窗口。

## 历史执行覆盖（.127，2026-10-24）

最新完整离线候选为`0.44.127-local`，见[启动准入与网络stop合同验收](../../../outputs/20261024-cx3-network-stop-contract/README.md)及[完整状态/资源矩阵](reports/stop-reopen.md)。真实start现拒绝retained/open/非零generation旧实例，在superclass或provider操作前返回；fresh实例open失败不再继续PCI enable/BAR/HW init/probe/发布，也不close旧client。RUN_FW sticky/poison/generation与owner保留合同未放宽。新对象本地干净不等于设备恢复；start不执行reset，也不生产异常释放凭据。

最终start 12场景37检查0失败，同测试重放冻结.126有22项行为失败；5种单独mutation均编译成功并被行为断言拒绝。stop组合增补1/2端口×0/末槽1/全槽pending，96场景447检查0失败。51个Python脚本、完整构建和85项输入前后/tar及独立核验通过；GPT Luna/high限定复审完成。kext SHA-256 `e65347ca24dc9a13eec55d4fb189161aac3833f14b9fde4f6587a62abef6e82e`，CLI `ec6e5a742424694e64e538f8b8f31d06de77e6159ceec98e3e94dd748c7836ac`，LC_UUID `1592271F-867E-3CCB-B6F5-CCE6EBCFCFD6`；bundle/kmod仍0.44.48。未安装/加载或访问设备。测试为默认START_LEVEL=2，诊断level0的重复publish另列边界，不宣称所有编译档重入合同已验证。

下一项：核定真实provider open仲裁及superclass stop/provider终止语义，推进真实child/table撤销的组合覆盖，并准备候选/hash/ABI/白名单、观测/停止恢复及新回执绑定的最小实机包。本机14:42快照提前停止缺口仍在；没有新的硬件窗口。真实DMA/provider/框架、stop/reopen、预算/异常恢复和F01–F44仍未完成。R05模块GET与R04 RSS运行绑定仍可离线推进。以下.126及更早条目为历史，不重做已验收子项。

## 历史执行覆盖（.126，2026-10-24）

本节优先于下方历史快照中的“当前/下一”称谓。最新完整离线候选为`0.44.126-local`，见[正常FW关闭验收](../../../outputs/20261024-cx3-fw-normal-close/README.md)。接续.125端口/HCA关闭，在正常CPU/框架排空、child/HCA/table撤销完成且无剩余owner后，never-handed-off FW直接释放；同代MAP_FA/RUN_FW后的FW仅在UNMAP_FA bool与fwStatus=0同时确认后释放精确slot，随后进入既有命令shutdown/provider close。失败/未知保留原FW及self/provider/mailbox/BAR。RUN_FW sticky、poison和generation不清零，同实例重用未开放。

最终stop测试90场景399检查0失败，真实UNMAP_FA封装20检查通过；相同最终测试重放.125分别26/9项行为断言失败。原FW重用93场景753检查保持通过。50个Python脚本、完整构建及84项输入前后/tar核验通过；GPT Luna/high独立复审完成，无具体阻塞项。kext SHA-256 `61827c10318b377ec3c89b09a1bb904b8208b3cf07ee357d73710ec6b7842198`；CLI `ec6e5a742424694e64e538f8b8f31d06de77e6159ceec98e3e94dd748c7836ac`；候选LC_UUID `A0345B8D-7A59-3967-9CAF-3897335EBC4B`。bundle/kmod版本仍0.44.48，候选tag为0.44.126-local。未安装/加载或调用设备selector、reset、重启。

下一就绪项：核对“已联网数据路径→正常stop→新实例初始化”的完整状态/资源矩阵及离线组合，准备准确候选/hash/ABI、白名单、观测/停止恢复与新回执绑定的最小实机包。TX仍在途或child/table/retention失败继续隔离保留，不能取消保护换取通过。完整stop/reopen、真实DMA/provider/框架、预算/异常恢复、RSS及F01–F44仍未完成。R05模块GET与R04 RSS运行绑定可离线推进；不重做.123身份、.124接口启停及.125/.126正常关闭实现。

同日14:42整机失去响应由用户强制关机重启，[只读取证](../../../outputs/20261024-freeze-1442-readonly/REPORT.md)未捕获panic或明确根因。14:43新boot的只读加载清单仍为.47（UUID AA1A2B9E-6BA0-3A54-A28B-553A4F12C6A2）；没有重认其联网/稳定性。旧boot记录和下方旧boot UUID是历史快照，不能冒充本次身份。没有新的危险实验放行。

历史[.125验收](../../../outputs/20261024-cx3-normal-close/README.md)完成端口/HCA关闭及INIT前失效port1旧回执；49脚本/构建和83项输入通过。其FW仍保留的结论仅描述该版本，正常FW段由.126更新。
更新：2026-10-24。每次继续、压缩恢复或重新派活时，先读本文和[总计划](CX3-PRO-FULL-ADAPTATION-PLAN.md)，再读[任务表](CX3-PRO-BACKLOG.md)末尾当前R子项。总计划定义范围/验收；任务表定义就绪项；本文定义续跑入口与协作规则。

## 已注入的持久目标文本

完成这张ConnectX-3 Pro在本机macOS上的全部实际适用功能，原生网络优先、专有能力通过SDK/工具。执行2026-10-25功能优先v4：先补齐生产功能，每批仅编译/ABI/关键路径简单验证；完整回归、故障注入、压力、100轮与48/72h统一最后按独立测试计划执行，不以某阶段完整测试未过阻止无关功能开发。按A1 EEPROM/DOM、A2自动启用、双口/L2、中断/多队列、offload、RDMA/RoCE、时间/QoS/隧道、IB、虚拟化板级及发布集成的实际接口依赖推进。复用已有保护与验证，不重复无关审计/全量构建/归档，不用版本数或测试数代表进度。分别报告实现、简单验证、完整验收；所有适用F01–F44最终仍需真实端到端PASS，未知/失败/阻塞未解决不得称完美适配。当前.47临时网络保留，新候选安装/启用与持久配置需明确窗口；实验单一owner、可观测且失败停止。协作GPT优先，按难度选择档位，Astra仅特别复杂且最高high；不使用DeepSeek/yeta。

## 当前事实与产品目标状态

**历史.124摘要（当前状态以上方执行覆盖为准）：** 最新完整离线候选为`0.44.124-local`，见[原生接口启停验收](../../../outputs/20261024-cx3-network-lifecycle/README.md)。生产enable/disable纳入既有CAS callback admission及stop排空；enable拒绝reset-blocked/stop-retained和closed gate，已入场回调观察到stop后取消timer；closed disable为family detach幂等返回且不接触退休依赖。实际方法+实际gate的16场景471检查0失败，含100轮同owner down/up；相同最终测试重放.123有37项行为断言失败。48个Python脚本和完整构建exit0，82项输入前后/tar及父侧独立核验一致，GPT Luna/high限定复审完成。kext SHA-256 `33fb960747d1ccba10ef6affbb9d17f96887a042e021d9b45c5be8d9e89aa077`；CLI `ec6e5a742424694e64e538f8b8f31d06de77e6159ceec98e3e94dd748c7836ac`。保存kext LC_UUID `97A02478-5D89-3CBC-8436-D7A286B913CB`。未安装/加载、访问真实service/设备、reset或重启；最近保存的加载记录仍为`.47`。100轮仅host fixture，未重测流量/真机循环；R02/R03/R06/T05/P1和全功能目标未关闭。

**历史.123完成包：** [运行身份与候选绑定验收](../../../outputs/20261024-cx3-runtime-identity/README.md)完成selector49、CLI identity/boundpcie、独立decoder与候选manifest/UUID/hash绑定，失败/超时输出保留hash，表校验正确50/50。47脚本/完整构建及81项冻结输入通过；离线身份子项不重复实现，R04仍缺真实boot采集及RSS raw同类绑定/硬件语义。

**历史.122完成包：** [FW重用验收](../../../outputs/20261024-cx3-fw-replacement/README.md)已关闭failed/unknown UNMAP后释放旧FW以及相邻icmalloc重用缺口，RUN_FW尝试sticky阻止在线/未知固件替换。93场景753检查0失败，42脚本/构建和74项冻结输入通过；完整证据保留，不重复实现。

**历史.121完成包：** 最新封存完整离线构建为`0.44.121-local`。[.121验收](../../../outputs/20261024-cx3-retained-ledger/README.md)补齐成功转入quarantine/overflow/ICM-kept及poisoned mailbox后的本地owner存活：停止流程在已有owner及network/child/table/FW各阶段之间检查，pin self/provider并sticky返回，避免清理依赖后丢失账本。mailbox判定与shutdown共用同一生产方法，健康分配仍可正常释放。stop组合41场景219检查0失败，同一测试重放.120为122项行为失败；mailbox生命周期61场景8613检查0失败。全量41个Python脚本及完整构建exit0；73项输入前后/tar一致；GPT Luna/high完成mailbox/stop范围独立复审。kext SHA-256 `94393beb1ae97b2ca2f19efc912c684e1b2e4467fc58cb0c7da9df20b506ad6c`，CLI `a32c27cd500aa7dd99867aeaa644fcca8a295cc039b17056569ce5920efd3115`。未安装/加载候选、调用设备selector、reset或重启。

本轮新增[只读OS身份记录](../../../outputs/20261024-cx3-retained-ledger/runtime-baseline.json)：实际加载仍为`0.44.47`，Mach-O UUID `AA1A2B9E-6BA0-3A54-A28B-553A4F12C6A2`；macOS 15.8.1/24H32，boot UUID `6B43565A-082A-4C44-A357-F164DD5EB873`。这只确认加载身份，不重认M4流量、稳定性或安全退出。候选强引用不阻止provider终止/断电，不是DMA静止/恢复证明；初始化后的FW释放目前会转入retention，不能宣称正常stop/reopen已可用。

**历史.124后续项（已由.125/.126推进）：** .124关闭family启停回调未计入stop排空的局部缺口，不重复.122 FW重用或.123身份绑定。下一安全实现切片是结果检查完整的正常HCA关闭事务：区分接口down/up保留owner、端口CLOSE/INIT与core unload；参考[正常关闭源码对照](../../../outputs/20261024-cx3-network-lifecycle/normal-close-source.md)，在CPU/IRQ排空和子对象交还后核定CLOSE_HCA及table/AUX/FW的逐步撤销合同。失败/未知保持owner可寻址，不能凭命令成功制造全DMA释放凭据。据此准备“候选联网→安全关闭→再次联网”最小实验；预算/异常恢复/实际provider框架分别留账。R05模块GET与R04 RSS运行绑定仍可离线推进。整个驱动stop/reopen、RUN_FW后恢复和真机验收未完成。

**历史.120完成包：** `0.44.120-local`停止流程在子context仍存、table/AUX原槽仍存或FW转移失败时，pin self/provider并在依赖清理前sticky返回；网络frame-ring转移失败不再丢失slot/lock元数据。显式初始化全部fTables，补齐raw-frame owner谓词。该次真实stop/EQ/net-stop/timer合成19场景92检查通过；同一最终测试重放封存.119产生80项行为失败。真实TX teardown/retention/predicate链37场景164检查、ICM/profile链42场景357检查通过；reset-time rawframe拒绝与普通stop直接free控制分开，不能当历史冻屏归因。GPT Luna/high复审通过。全量41个Python脚本和build.sh退出0；73项输入构建前后与tar字节一致。kext SHA-256 `98710f14a97b1ccdf7274febc089fe2e25f502d29980c43c1e24be6abf12043f`；证据入口[.120验收](../../../outputs/20261024-cx3-stop-composition/README.md)。首次通过与后续fixture失败分别留在attempt1/attempt2，不代表最终源码。此段仅为历史证据，当前加载身份及最新候选以上方.121摘要为准。

此前[.119 PCIe查询](../../../outputs/20261024-cx3-pcie-query/README.md)已完成selector48、368字节查询ABI、query→provider→handler→CLI→独立decoder合成集成和构建元数据统一；不重复实现这些已完成离线子项。它的72项输入及二进制仍独立封存。

R04构建元数据和候选运行身份接口离线子项已闭环：staged plist与编译头共用source `CFBundleVersion`，当前保存包和Mach-O kmod版本均`0.44.48`，候选tag为`0.44.126-local`。默认未标记构建为`unversioned-local`，source模板未改写。OS boot/连接service/候选build/generation组合绑定已实现但未上机；generation本身不是释放凭据。此前[.118适配器查询](../../../outputs/20261024-cx3-adapter-export/README.md)和[.117 A/B完整事务测试](../../../outputs/20261024-cx3-ab-transaction/README.md)保留；本批所有采集仍为合成数据，本卡PSID和当前链路仍未知。

**后续并行前沿：** CPU/doorbell调用者已有[有限审计](../../../outputs/20261024-cx3-stop-composition/doorbell-audit.md)，timer/stop有两种确定性交错；补尚缺的真实框架/其他回调证据。区分正常队列撤销与异常reset回收；[EQ撤销语义](reports/r06-eq-revoke-contract.md)尚不能证明所有已发PCIe DMA完成，不以generation/命令成功替代释放凭据。最小实验包绑定候选hash、单一断言和真实观测/回执，以重证联网和安全退出；不以整个P1完成制造循环依赖，相应安全前置和当轮暂停窗口仍必需。R05模块EEPROM/DOM专用GET和R04 RSS绑定可离线推进；实际port2/PSID/PCIe采集另需硬件窗口。R02/R03/R04/R05/R06、P0/P1及F01–F44均未完成；历史冻屏未完整归因。

下方逐版本段落为历史证据；续跑以本节权威摘要和文末当前执行入口为准。

2026-10-24 R06 timer callback admission：确认`IOTimerEventSource` action未加入原有TX callback gate，stop虽关闭并排空该gate，不能由此推出已进入`netTick()`的action退出。现`netTimerAction()`先进入同一个原子gate并以RAII离开；stop关闭gate、最多等待2秒排空后，才取消/移除timer和释放依赖。已排队但在close后才执行的action被拒绝，不进入`netTick()`；已有action的重排留给后续`cancelTimeout()`清除。测试先新增后观察到预期失败，再实现：timer生产方法2个行为检查，timer stop合同6项，TX stop合同15项，gate CAS压力测试通过，停止保留8 cases/91 checks通过；全量30个Python脚本、`git diff --check`和隔离构建`0.44.114-local`均PASS，kext SHA-256 `1ffc70d591129d4460ae0e09b11cf8499e86989ca16b5784a595ff4fc08cc64d`。仅离线/合成验证，未安装/加载/触碰硬件；不证明真实调度运行或设备DMA停止。

2026-10-23 R06 IRQ/workloop 证据校准：本机SDK的`IOInterruptEventSource.h:48-54`承诺workloop event chain内单线程action；`IOWorkLoop.h:246-251`规定`removeEventSource()`在workloop确认移除并释放source引用后返回。结合当前代码只在成功移除后释放本地引用并继续teardown，IRQ action排空有文档化API契约支撑，但没有目标内核运行时验证。timer callback已由后续 `.114` admission gate/cancel 增量补上离线排空边界。完整30个Python测试通过；隔离`0.44.113-local`构建PASS，SHA-256 `8dadbaa7f164a8a3b3162460b73ffeea90b6ad5af12001774eff98115f59ed3e`。未安装、加载或触碰硬件；R06仍PARTIAL，DMA静止、reset scope及实际release credential未闭环。

2026-10-23 IRQ teardown fail-closed 增量：`irqReleaseObjects()`现在将missing workloop或`removeEventSource()`非成功视为未证明移除，保留event source引用并返回失败；stop在`netServiceStop()`和队列释放之前fail-closed并pin owner，IRQ-down拒绝HW2SW_EQ，profile replacement在改写`fProfile`前中止。macOS SDK对`IOWorkLoop::removeEventSource`规定同步确认移除后才返回，不能把非成功返回等同于移除成功。29个Python测试通过；`0.44.111-local`隔离构建PASS，SHA-256 `35f58e01085c43c9d99c1f89ab2fc98e738a477858ce4d6507bb55f06e1b4b35`。纯离线/合成，未安装、加载或触碰硬件；R06仍PARTIAL，DMA/CPU/timer/reset scope释放凭据和P0/P1仍未闭环。

2026-10-23 R06 network timer source removal fail-closed：此前`netServiceStop()`忽略timer的`removeEventSource()`返回值，仍release并清空本地source owner、继续TX teardown。现新增生产`netTimerRelease()`，缺workloop或移除未确认时保留timer引用并返回失败；`netServiceStop()`在任何TX清理前失败返回，`stop()` pin self/provider并在队列/BAR teardown前停止。新host行为测试覆盖移除失败、workloop缺失、成功移除和无timer；更新TX stop/IRQ调用合同测试。全量30个Python测试通过，`git diff --check`及隔离完整构建`0.44.112-local`通过，kext SHA-256 `00f80c747f12731d5a46ba7e8117f98e53f1d977899c4ad59deee8264c1ae170`。离线/合成验证，未安装、加载或触碰硬件；仍不证明真实IOKit timer/IRQ执行或DMA静止，R06保持PARTIAL。

2026-10-23 R06 A/B owner reset门禁：发现 `resetRetentionPreflight()` 不检查只保存在 `Mlx4Hw` 的A/B retained ring；旧路径会在reset命令前递增provider generation，令ring的唯一HW2SW retry因代次漂移永久拒绝。新增反例在修复前令 reset/FLR preflight 测试6项失败；现加入预检拒绝，`reset/FLR preflight` 为106 cases/691 checks PASS（synthetic HCA），完整28个Python测试及 `git diff --check` PASS。隔离完整构建 `BUILD_TAG=0.44.110-local` PASS，kext SHA-256 `5ce6d5541226f332c685850afe00a802e098ce4f62141d3e687e25b48d66a4a0`，产物 `/tmp/cx3-ab-reset-gate-build.7wQYwf/out/`。这是离线/合成证据；未安装、未加载、未触碰硬件。该门禁避免把已retained owner推进到不可retry代次，但还不是 reset scope / DMA 静止凭据，也不关闭R06/P1。

2026-10-23 R06 A/B retained-ring owner 切片：新增 `test-ab-ring-retention.py`，以生产 `retryAbRetainedRings()` 做合成行为验证（9项），并修正源码函数提取/合同检查，确认 HW2SW 失败保留owner、成功后MTT拒绝可重试、generation drift不下发命令、逆序释放及失败时 stop pin self/provider。`test-stop-retention.py`现在也以可控host stub执行A/B owner stop分支；该脚本8 cases/91 checks通过。全量28个Python测试通过，`git diff --check`通过。隔离完整构建 `BUILD_TAG=0.44.109-local` PASS；kext SHA-256 `1fe7399f7527bebea16c81711950f4c03a1d317289a0b72cf352fac2f5b4600f`，产物 `/tmp/cx3-ab-retention-build.HlGpJ0/out/`。仅离线/合成验证，未安装、未加载、未触碰硬件。R06仍PARTIAL：所有实验队列release-site行为、真实正常/异常静止凭据及CPU/workloop/IRQ/reset-scope仍未闭环。

2026-10-23 R06 CQ/RQ/SQ 创建回滚owner更新（阶段快照）：`.106-local`补齐CQ创建时`mttFree`拒绝的owner回传、unknown-retained状态与teardown重试；`.107-local`补齐RQ/SQ相同的创建回滚owner回传，RQ允许`Up=false`时 teardown 重试并禁止重建，SQ保持在TX teardown owner中且拒绝后续setup。27个Python脚本全PASS；retention teardown 35 cases/154 checks；unknown-owner源契约PASS；隔离构建`BUILD_TAG=0.44.107-local` PASS，kext SHA-256 `7512c5e70fddf74fc5b29ac7a27d26a6bbb89378cc08f7d24b077553aa00d729`，产物`/tmp/cx3-rq-sq-create-retention-20261023/`。此为离线/合成验证，不能证明真机/真实IOKit或DMA、CPU、IRQ静止；没有安装、加载或触碰硬件。该阶段后续计划由`.108/.109`及当前R06审计增量推进；R06仍PARTIAL。

2026-10-23 R06 A/B dummy MTT reservation：将实验限定为MTT allocator空闲（或只剩本实验跟踪reservation）且reservation锚定首个可用entry，避免活队列/历史分配污染偏移对照；每次只建立一份精确、按设备代次绑定的reservation，重复尝试不累积256条owner ledger，尺寸/代次不匹配时拒绝。实验完成后仅当全部ring与其他MTT owner都释放才回收此非DMA预留；table view替换也会尝试同一安全回收。定向allocator（42项主检查，含400次reserve/reclaim循环）、生产`mttAllocAbDummy`、table setter与A/B生命周期合同通过；全量27个Python测试脚本PASS。隔离构建`BUILD_TAG=0.44.108-local` PASS，kext SHA-256 `a896ac313bfbb4f2534d3374543a487c86b3763232ad1f7c79ba7e73895d09b0`，产物`/tmp/cx3-ab-mtt-bounded-final-20261023/`。仅离线/合成验证；未安装、未加载、未触碰硬件。R06仍PARTIAL：A/B实验ring在HW2SW/MTT释放失败时的controller可达owner、其余实验队列/UAR/doorbell释放点、真实DMA/CPU/workloop/IRQ/reset-scope静止凭据仍待完成。后续 A/B ring owner 切片及其验证状态见本文最新记录。

2026-10-23 R06 MTT owner/generation slice：MTT ledger将range绑定到`Mlx4Icm*`、allocation ID、allocation generation及handoff后的owner generation；`markDmaHandoff`同步更新匹配range。`mttFree`返回bool并拒绝错误owner/代次/range；RQ/CQ/TX SQ/CQ/EQ teardown失败时保留owner字段和ICM内存，重试不重复已成功的HW2SW。更新QP/TX合成fixture匹配生产签名。完整27个Python测试全PASS；generation 25 checks、MTT allocator 29 checks+生产free/table setter行为、retention teardown 31 cases/138 checks、QP 107 cases/1206 checks、TX 35 cases/302 checks通过。隔离`BUILD_TAG=0.44.104-local`完整构建PASS，kext SHA-256 `7c6750830adc7e69286fb539bd74cca0bb86babd15bd866b160633302beb89fd`，产物`/tmp/cx3-mtt-owner-20261023/`。离线/合成证据；generation不是DMA静止/free凭据，创建回滚owner可达性、A/B实验dummy reservation、CPU回调/IRQ/设备DMA静止与reset scope仍待审计。未安装、未加载、未触碰硬件。

2026-10-23 R06 CQ/EQ UnknownRetained 切片：`cqCreate/eqCreate`在`SW2HW_*`超时时把ring、MTT范围及CQ record等teardown元数据返回并持久化；owner保持不可用，重复创建/使用被拒绝；CQ在未标记Up但有owner时仍可重试`HW2SW_CQ`，失败不释放，成功后才归还ring/record与MTT；EQ读写探针、poll、IRQ enable及net-arm拒绝未知owner。`test-retention-teardown.py`真实抽取生产teardown，26 cases/117 checks通过；新增`test-unknown-owner-path.py`源契约检查通过；全量27个Python测试通过。隔离`BUILD_TAG=0.44.103-local`完整构建PASS，kext SHA-256 `855582de78d7edadecf66afaf4c3e914f23fe098effdfbc1ccccffdb925d176b`，位于`/tmp/cx3-unknown-owner-build.zhgIIS/out/`。这是离线代码/合成IOKit-HCR验证，不是硬件验证；MTT range与DMA owner/generation绑定、`mttFree`错误传播、其他实验owner、DMA/IRQ静止及reset scope仍开放。未安装、未加载、未触碰硬件。

2026-10-23 R06 TX detach 异常分支修复：本次新增 `thread_call_enter()==true`（调用已排队）行为场景；先红3项，修复为接口引用仍交给待执行worker，并释放本次未排入新回调所增加的self引用。完整26个`tools/test-*.py`全PASS；detach worker生产方法8例/30检查，stop合同13项；隔离构建`BUILD_TAG=0.44.102-local` PASS，kext SHA-256 `a30c17f92a778aa0989859079ff9f64656705146a938e1d6624b947a107696e1`，位于`/tmp/cx3-detach-pending-20261023/`。这是离线构建/测试，不是硬件候选验收；未安装、未加载、未触碰硬件。

2026-10-23 R06 MTT source inventory（历史发现，已部分修复）：盘点`mttAlloc/mttFree`全部生产调用，确认R4 QP/CQ/MR依赖的常规handoff与teardown边，以及实验A/B EQ路径的dummy/ring分配。最初发现CQ/EQ在`SW2HW_*` timeout时保留ring/MTT但丢失controller可达owner；该问题已在`0.44.103-local`离线修复：超时元数据返回并持久化为不可用owner，禁止复用，CQ/EQ teardown可重试且失败不释放。详见[`r06-mtt-owner-inventory.md`](reports/r06-mtt-owner-inventory.md)。当前下一代码切片改为将MTT range绑定到DMA owner/generation、把`mttFree`拒绝结果传回调用者，并审计实验队列/UAR/doorbell实际释放点。该修复为离线/合成行为证据，不代表现场触发或真实硬件静止；R06仍未完成。

SDK声明同步detach可能不定时阻塞；因此阻塞发生在受self/interface引用保护的worker内，而stop线程到2秒即保留整笔状态返回。离线模型验证了引用移交、重复引用、双接口、worker缺失、超时与call-free失败；没有在真实IOKit运行此worker。同步family终止只解决接口回调生命周期证据，不是DMA静止凭据；timer/workloop、IRQ、设备DMA与R02/R03/R06完整闭环仍待验证。

历史候选入口：**0.44.99-local**；[`verification-batch54.json`](../../../outputs/20261022-cx3-fulladapt-batch1/verification-batch54.json)为PASS，33组离线检查及kext/CLI构建退出0，源与测试输入在运行期间无变化。0.44.100/0.44.101/0.44.102均为后续离线源状态。当前最新隔离构建为**0.44.103-local**；27个离线Python脚本及完整构建通过，包含CQ未知owner teardown重试的26 cases/117 checks。当前代次契约测试25项PASS；QP链107场景1206检查、TX链35场景302检查、ICM释放41 cases/350 checks、reset preflight 105 cases/682 checks通过。所有这些都是离线证据，未替代0.44.47真机联网基线。
kext SHA-256 `9fc90b61e6407a6cc42be7b8bff7fe555c01d2516028ab1a707b732f0420b11d`；
当前续跑提示：前文候选摘要更新到`.103`，其“当前最新”描述已由本轮`.104`验证结果取代；不要把`.104`视为已上机版本。最新kext SHA-256 `7c6750830adc7e69286fb539bd74cca0bb86babd15bd866b160633302beb89fd`；产物`/tmp/cx3-mtt-owner-20261023/`。
cx3ctl SHA-256 `2b11d44f928db80e3cabccfb92f0becafb632b4b2c940dc4d5c5b25a1af6bdad`。
18个生产源文件live/tar清单记录一致，完整验证输入以batch47 manifest封存；两个二进制哈希已记录。
历史局部保护、红绿反例及被取代版本见[安全审计](reports/safety-lifecycle-audit.md)和外层各候选CHANGELOG；
**.74/.75/.78/.84/.85不部署**。.81补齐9个旧manifest漏掉的build辅助检查源，不改写旧快照。

重要纠正：.84/.85的qpModify/qpQuery把观测指针传入cmdBox第8参数bool bypassBusy，
漏传第9参数观测输出；非空指针还会旁路busy检查。旧QP测试替身漏掉bool，造成假绿。
下文.84“真实观测接通”相关历史表述撤销；.85保存结果的独立consumer测试仍有效，
不能证明真实设备端产出元数据。.86显式传false并补真实QP→cmdBox→cmdBoxLocked
执行集成测试，声明直接来自生产头文件；17场景186检查0失败，最终同测试重放
封存.85 QP为42失败/exit1。它不是历史冻屏根因证明，两版均未上机。
编译警告边界：Apple Clang 17会用-Werror=pointer-bool-conversion拒绝历史成员取地址
写法，但普通指针变量仍可隐式转bool；不能泛称覆盖所有误传。.87新增deleted模板
重载拒绝第8参数为指针，实际运行17场景186检查继续通过；两种编译负例均在不加
Werror时因deleted拒绝。类型保护加入前测试exit1，见test-qp-type-contract-red-04486.log。
该保护不改变显式bool旁路调用的授权语义，不是命令锁/整体安全完成。

.88把命令锁分配移至Mlx4Hw::init，分配失败不映射BAR；BAR0映射/地址失败回收
该锁/已取得映射。cmdBox/Imm/Event不再懒分配，缺锁返回NOT_POSTED、不进入core/
读HCR；观测清零表示核心未进入。执行真实init/三包装方法13场景104检查0失败，
同测试重放.87为42失败/exit1；实际QP/HCR17场景186检查及两类类型负例继续通过。
启动路径检查PASS，仍无命令/设备写/DMA分配。shutdown并发排空/锁等待上界未证明。
跨selector不能简单用fResetUnproven全禁：首次正常reset也置位，须区分首次干净
初始化、reset失败及保留owner。下一切片先完善这些状态与允许命令，再实现准入。

.89已补局部reset准入：实际尝试前置设备命令阻断；仅无任何已知设备owner/HCA
状态/mailbox的首次成功HCA reset可继续初始化，不将成功bool当回收凭据。失败、
旧owner reset及手写FLR后只允许host-only netstat；两类事务锁路径均复查，包括
锁等待期间状态变化。重复reset拒绝早于MMIO，失败reset不再读owner-after。
81场景459检查、全47 selector的157场景471检查0失败，旧.88重放分别35/184
失败。63条live/tar清单及两个binary一致。新字段只是准入latch，不是设备代次，
没有真正恢复；下一项继续代次/scope凭据、平台FLR/CPU排空与全局reservation。

.90将FLR接入公开平台FunctionReset+OptionNone。失败原样返回且无后续设备访问；
成功只核对配置身份/BAR/Command，不读旧MMIO，不清poison、不free，阻断与owner
保持。入口92场景532检查通过，封存.89同测试50失败/exit1。适配器9场景62检查
通过；五种mutation已独立确认编译成功且执行断言失败。工具正确显示未读owner，
失败返回非零，5项consumer检查通过。未上机，不是代次/scope凭据，不关闭R06。

.91按独立审查补上复位前配置完整性门：vendor/device identity必须有效，BAR0须
已分配，PCI Command须可读且memory decode已开；否则在平台调用和sticky latch之前
以NotReady拒绝。4个前置损坏反例先产生12个失败，修复后入口96场景552检查0失败。
32组/构建通过，65条live/tar清单及二进制哈希一致；硬件状态仍未验证，R06仍PARTIAL。

.93在start保存provider PCI ID+BDF；FLR前后重读并要求与启动identity、复位前identity一致。
adapter 16场景79检查及5个mutation通过；reset/FLR生产入口102场景582检查通过。该快照只
检测provider/function漂移，不是generation或DMA静止凭据；R06释放授权仍关闭，未上机。

.96补provider generation递增与溢出拒绝；ICM分配、三类MAP尝试及quarantine/overflow/
UNMAP保留账本记录代次。generation source contract 8项、reset/FLR 104场景678检查、
retention teardown 25场景93检查通过。

.97新增真实DMA handoff标记器，覆盖正常CQ/EQ SW2HW与RX/RSS/TX QP状态转换前的owner集合；
无效owner会拒绝设备命令，集合预检失败不留下部分标记。目标测试18项源码契约、QP链107
场景1206检查、TX链35场景302检查通过，整批33项和构建通过。仍有MR/MTT/实验路径未完整
盘点，且释放点没有消费静止凭据；generation不是DMA停止证明或free授权，R06仍PARTIAL。

.82在真实cmdBox/cmdBoxLocked核心增加按调用保存的40字节命令观测：进入核心、
主机GO store及屏障完成、正常completion/status-read路径与单调时间/fwStatus。
Posted不证明写抵达设备，Replied不证明QPC语义正确；start时间不含命令锁等待。
生产执行测试7场景86检查0失败，最终同测试重放.81为38失败。
.84已将该输出接入真实QP方法及普通RX/RSS的三次modify、逐步/最终query；不是R04全部闭环。

.83先补独立decoder就绪子项：按精确长度或显式格式读取248字节QPC、256字节旧槽、
264字节发送快照和4096字节query邮箱，分离+8前缀与尾部，完整保留字节及hash。
10项测试通过，同测试重放.82为25项失败。固定字节fixture独立于生产builder；
全零不声明有效或成功。该批不改驱动逻辑/ABI，kext哈希变化含构建tag，不是新硬件功能。

.84新增64字节按调用采集记录，区分未调用、验证/分配失败、host邮箱prepared、原始
快照captured及HCR核心路径；每次query有独立fwStatus。QP结果新增尾部版本章1、
RSS版本升3；结果尺寸17912/18512，现handler与dispatch拒绝旧尺寸，工具检查版本后
显示各步记录，方法失败也保留观测。底层13545检查0失败（主要为字节比较），
RX/RSS执行107场景1206检查0失败，client ABI36检查及formatter/18512字节传输通过。
同测试重放.83：底层140失败、RX/RSS28失败、client版本章1失败且formatter失败。
boot/运行/代次身份和生产结果→独立decoder完整连接仍缺；未上机。

.85补保存的rawjson 34/35结果→独立decoder消费：显式x86_64 LE结果ABI，邮箱仍按BE，
检查transport/selector/长度/章及每步采集状态；未调用、失败query留decoded=null，
成功全零query仍可解析。新增11项测试，执行真实cmdRawJson函数和编译器布局的真实
结果结构，transport为替代边界；全部通过，同测试重放.84 decoder为5项失败。
额外反例修复“命令已进入但无prepared邮箱仍接受”，先1失败后通过。文件/结果/各邮箱
hash绑定具体字节，不证明来源或硬件行为。本批仅改外层消费/测试与runner，无驱动逻辑/ABI改动。

- R03/R06仍PARTIAL：局部reset/FLR预检和真实poison保留已有离线保护；全局reservation、
  完整代次/scope凭据、raw入口、stop/provider/电源/timer/IRQ协调及真正恢复仍开放。
- R04仍PARTIAL：每步记录、基本版本门及生产rawjson→独立decoder离线消费链已有验证；
  boot/运行/版本/代次身份绑定、真机导出/对照和RSS语义仍缺，详见[RSS报告](reports/rss-qpc-layout.md)。
- 最新硬件联网证据仍为总计划所列0.44.47/M4历史快照。本批未安装、未调用设备、未重启，
  未核对当前加载状态；P0/P1未完成，历史冻屏未归因。
- 持久目标为active，覆盖F01–F44、R01–R08及P2–P8；批次离线通过不改变全项目完成判据。

## 协作模型（用户最新指定）

2026-10-24 用户最新约束：**协作者仅使用 GPT 系列，不使用 DeepSeek-flash、TokenCome、Yeta 或 Gemini。** 常规实现/审查按难度选择Luna或Sol；Astra仅用于非常复杂任务且最高`high`。派活前注明模型与思考档位；工具不接受合适的GPT型号时，由主代理继续，不改派其他系列。本轮已停止先前DeepSeek审查调用，GPT Luna调用已成功收回。

| 用户指定显示名 | 工作与思考档位 | 实际选择条件 |
| --- | --- | --- |
| `gpt-6.1-sol` / `gpt-6-luna` | 常规开发/分析按实际难度选合适档位 | 优先使用，按量/额度保持合理；模型与档位在派活或显式协作时声明 |
| `gpt-6-astra` | 极复杂架构/根因分析；本次规划为`high` | 仅在任务确有高复杂度且用户同意时使用；不默认升到max |

此前重规划使用`gpt-6-astra / high`，是历史记录，不表示当前正在调用Astra。主代理对项目范围、实施、测试、验收与集成负全责，不把工作人为限定在验收脚本/测试。

协作仍按全局任务文件通道：完整brief先落到对应任务文件，消息只指向读取；明确文件所有权、验收证据与本轮独有核对细节，要求保留他人修改。派出后主代理做不冲突工作并在同回合收齐或明确处理终态；单卡硬件写入始终只有一个owner。

## 续跑与停止点

当前执行入口（2026-10-24）：先读上方.128执行覆盖与stop-reopen矩阵。下一项为最小硬件包及观测缺口设计、真实child/table组合；公开superclass stop源码已核查，目标provider仲裁/termination和运行时行为仍开放。.123–.128已验收离线子项不重做；R05模块GET可继续。实际stop/reopen、DMA/provider/框架、预算、异常恢复和reset scope仍开放；没有当轮窗口时不安装/加载/调用设备selector或重启。

1. 读取总计划 v3、本任务表“当前执行前沿”及最近batch/报告；不重做R01/.59，不根据文件名推断加载状态。最新候选与已实测联网基线分别记账。
2. 继续完成有限R06安全审计包：TX/timer admission gate与有界排空已有离线实现，不重新从零设计。对剩余真实释放点验证正常路径成功、失败路径零危险释放和owner保留；真实IOKit调度、设备DMA静止、正常/异常release credential及reset scope仍未闭环。generation标签不是静止凭据。同步推进R04/R05离线子项。
3. D04/D05/D09 的用户态状态机、SDK权限/ABI、安装/许可文档可并行设计与离线验证；不得因此绕过 DMA/stop/reset 安全门。
4. 硬件工作只按单一实验包串行推进：前置安全不变量、候选/hash、操作白名单、独立管理/远端phase/告警回执、停止和恢复方案齐备后，仍需当轮用户明确“工作已暂停，可以复现”。绝不自动刷固件、持久改端口模式或重启。
5. 全项目完成只按总计划的全功能完成定义判定。若必须用户输入/外设/平台变化且无可继续项，给出具体证据和下一动作；正常批次交付或本地通过不把持久总目标标complete。
