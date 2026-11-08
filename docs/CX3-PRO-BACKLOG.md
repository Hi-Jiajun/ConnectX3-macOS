# CX3 Pro 项目任务与依赖

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

## 2026-10-25 功能优先队列覆盖

依据用户最新指令，[v4功能计划](CX3-PRO-FEATURE-FIRST-PLAN.md)覆盖下面历史执行前沿：**A1 EEPROM/DOM GET生产链 → A2自动启用入口 → B双口/完整L2 → C中断/多队列 → D Offload → E RDMA/RoCE SDK → F时间/QoS/隧道 → G IB/IPoIB → H虚拟化/板级 → I集成发布**；按实际接口依赖交叉推进。后续BLOCKED仅约束对应硬件/平台/最终验收，不让整组编码等待100轮或48/72h。每批用`CX3_TEST_PROFILE=smoke`及功能定向简验，完整测试见[单独规划](CX3-PRO-FINAL-TEST-PLAN.md)，集中最后执行。既有57脚本/保护审计等证据保留且不重复跑；没有新增相关代码或失败不自动扩测试。

`.129`采集包的host链和两项review缺口已补，观察回执缺口保留、真实execute禁用；该包暂存而非继续占据全部开发队列。F01–F44未完成状态不变，生产实现与完整硬件PASS分开统计。

2026-10-25首错消费入口：[离线decoder与验证](../../../outputs/20261025-cx3-tx-fault-decoder/README.md)完成（13 tests；生产logger合成链，原13/53保护回归通过）；[参考差异复核](../../../outputs/20261025-cx3-tx-reference-contract/reference-review.md)收敛为producer编码、data发布顺序、延迟stamp、headroom。均非首次硬件错误复现。下一就绪项：绑定候选/hash/ABI、真实boot/service、同次SQ尺寸、首错raw、独立观察/停止条件的最小实机包；不合并猜测修复，不切换当前日用.47。F02/F06/R06仍开放。

2026-10-25 补充[11个原语/内部helper二进制对照](../../../outputs/20261025-cx3-tx-primitive-audit/README.md)，将.47/当前源码对应缺口收窄到已比对函数；没有根因复现，不能只凭回绕附近就改owner或producer。下一次运行需要首错原始CQE/WQE与精确producer；先对齐60探针旧地址和当前.101身份。该补证不新增硬件PASS。

2026-10-25 最新执行前沿：[TX 首错保护 .129](../../../outputs/20261025-cx3-tx-fault-containment/README.md)离线完成（57脚本/完整构建、92项冻结输入一致）；[运行事件](../../../outputs/20261025-cx3-dhcp-loss-readonly/REPORT.md)确认.47先有0x02/0x6d、随后flush，前一boot失去DHCP。05:38新boot已按用户明确授权恢复既有.47，en2 DHCP.201、绑定接口外网HTTPS200，en0仍默认，见[本次验证](../../../outputs/20261025-cx3-temporary-dhcp-055750/README.md)。下一项为首次QP错误证据/根因、长期稳定性及探针对齐当前身份；.129未安装，不是根因修复，F02/F06及R06仍开放。以下.127/.128是先前切片记录。

2026-10-24 当前执行前沿为[.127启动准入](../../../outputs/20261024-cx3-network-stop-contract/README.md)：51脚本/完整构建、85项冻结输入及GPT Luna/high限定复审通过；start 12场景37检查、stop 96场景447检查0失败。[资源矩阵](reports/stop-reopen.md)已建立，provider open拒绝及旧实例重入缺口已关闭。下一项为目标provider仲裁/superclass stop语义、真实child/table组合及最小硬件包；保留TX在途和失败隔离合同。未上机，无新增硬件PASS。

日期：2026-10-22；对应[总计划](CX3-PRO-FULL-ADAPTATION-PLAN.md)、[能力台账](CX3-PRO-CAPABILITY-MATRIX.md)、[决策清单](CX3-PRO-DECISIONS.md)。

2026-10-24 历史增量：[.122 FW重用](../../../outputs/20261024-cx3-fw-replacement/README.md)关闭失败UNMAP后仍free及同owner的icmalloc绕过，并补CLI错误/短结果传播。RUN_FW尝试后不允许无恢复合同的替换。93场景753检查0失败，最终测试重放.121为626项行为断言失败；42脚本/完整构建及74项冻结输入核验通过，GPT Luna/high生产复审无具体阻断。仅离线，无新增硬件功能PASS。下一就绪产品项为R04候选内运行/boot/build身份绑定，R05模块GET可并行；真实正常退出/恢复、框架/provider与DMA语义及预算仍开放，不重复已闭合调用者反例。
状态更新：2026-10-24；当前完整离线构建与证据以[会话权威摘要](CX3-PRO-SESSION-GOAL.md#当前事实与产品目标状态)为准。T01–T05仍未完整验收，P0/P1未通过；已有M4联网证据不自动关闭后续任务，离线候选不冒充已上机结果。下方版本条目为历史记录。

2026-10-23 R06 timer source removal fail-closed：`netServiceStop()`此前忽略`removeEventSource(fNetTimer)`错误，仍释放source并进入TX teardown。新增`netTimerRelease()`检查缺失workloop/移除返回值；只有workloop确认移除后才释放本地引用并清空字段。失败时`netServiceStop()`在TX锁/mbuf/ring清理前返回false，`stop()` pin self/provider并在后续队列、表与BAR teardown前退出。生产helper host harness对移除拒绝、无workloop、正常成功、无timer执行10项检查；stop调用合同4项、TX stop合同15项、IRQ release合同8项通过。全量30个Python脚本通过；`git diff --check`通过；隔离`BUILD_TAG=0.44.112-local`完整构建PASS，kext SHA-256 `00f80c747f12731d5a46ba7e8117f98e53f1d977899c4ad59deee8264c1ae170`，产物`/tmp/cx3-timer-release-build-otd8ic/out/`。只属离线/合成证据，未安装、加载或触碰硬件；不能证明真实IOKit callback drainage或DMA静止。

2026-10-23 R06 IRQ source removal fail-closed：`IOWorkLoop::removeEventSource()`的本机SDK文档约定同步确认移除，并在成功返回前释放workloop持有的source引用；生产代码现检查返回值，缺少workloop或remove失败时保留本地source owner并拒绝依赖资源teardown。stop先移除IRQ再stop网络service；IRQ-down不继续HW2SW_EQ；profile replacement失败时不覆盖旧profile。新行为测试11项、调用点/source合同8项通过；原`test-icm-release.py`的host shim也同步bool签名。全量29个Python脚本、`git diff --check`和隔离`0.44.111-local`完整构建通过，kext SHA-256 `35f58e01085c43c9d99c1f89ab2fc98e738a477858ce4d6507bb55f06e1b4b35`。仅离线/合成验证，不证明真实IRQ或DMA静止；未安装/加载/触碰硬件。

2026-10-23 R06 A/B reset-generation guard：reset preflight原先未检查仅保存在`Mlx4Hw`的A/B retained ring；若reset执行，provider generation会在尝试前递增，使ring的HW2SW retry不再匹配。新增生产入口回归先触发6项失败；现有预检在generation递增和HCR命令前拒绝此状态。`test-reset-preflight.py` 106 cases/691 checks、完整28脚本、`git diff --check`均PASS；隔离`BUILD_TAG=0.44.110-local`完整构建PASS，kext SHA-256 `5ce6d5541226f332c685850afe00a802e098ce4f62141d3e687e25b48d66a4a0`，产物`/tmp/cx3-ab-reset-gate-build.7wQYwf/out/`。离线/合成证据，未安装/加载/触碰硬件；这只是防止不可retry状态转移，不是reset scope或DMA静止证明。

2026-10-23 R06 A/B retained-ring owner 切片：新增测试直接抽取 `retryAbRetainedRings()`，覆盖9项行为（HW2SW拒绝与重试、MTT拒绝后不重复HW2SW、代次漂移不发命令、未handoff释放重试、双ring逆序回收）；4项源码合同确认两种失败分支都留存完整owner及stop pin路径。修正源码函数提取器原先在嵌套块提前截断的问题，并让 `test-stop-retention.py` shim声明 `fHw.hasAbRetainedRings()`。28个 `tools/test-*.py` 全PASS，`git diff --check` PASS；`BUILD_TAG=0.44.109-local` 隔离完整构建PASS，kext SHA-256 `1fe7399f7527bebea16c81711950f4c03a1d317289a0b72cf352fac2f5b4600f`，产物 `/tmp/cx3-ab-retention-build.HlGpJ0/out/`。仅离线/合成证据；未安装/加载/触碰硬件。下一步仍是实验队列/UAR/doorbell与全部release-site盘点，以及正常/异常静止credential的生产和消费；R06保持PARTIAL。

2026-10-23 R06 A/B dummy MTT reservation：A/B路径现要求MTT allocator空闲或只含本实验reservation，并验证reservation锚定首个可用entry，避免活队列/历史分配污染偏移对照。按设备代次精确跟踪一份reservation；尺寸/代次不匹配拒绝，完整ring清理且无其他live owner时回收；失败保留而后续实验fail-closed，不会累积耗尽256条owner记录。生产allocator/reservation/table setter/A-B路径合同回归及全量27个Python脚本通过；隔离`0.44.108-local`构建PASS，kext SHA-256 `a896ac313bfbb4f2534d3374543a487c86b3763232ad1f7c79ba7e73895d09b0`，产物`/tmp/cx3-ab-mtt-bounded-final-20261023/`。仅离线/合成验证；未安装、未加载或触碰硬件。R06仍PARTIAL；A/B ring释放失败后的controller owner可达性与其他实验队列/UAR/doorbell释放点未闭环。

2026-10-23 R06 RQ/SQ create rollback owner：`rqCreate/sqCreate`在MTT释放被拒绝时会返回完整可达owner元数据；RQ控制器将其记为不可用、禁止重建，`rqTeardown`可在`Up=false`时重试；SQ由现有TX teardown路径保留并重试，且新增setup拒绝门。测试覆盖RQ/SQ teardown拒绝后保留与成功重试，并用源契约断言全部6个创建回滚拒绝分支回传owner；`test-retention-teardown.py` 35 cases/154 checks、`test-unknown-owner-path.py`、QP 107 cases/1206 checks、TX 35 cases/302 checks及全量27个Python脚本均通过。隔离`BUILD_TAG=0.44.107-local`构建PASS，kext SHA-256 `7512c5e70fddf74fc5b29ac7a27d26a6bbb89378cc08f7d24b077553aa00d729`，产物`/tmp/cx3-rq-sq-create-retention-20261023/`。仅离线/合成证据；未安装、未加载、未触碰硬件。RQ/SQ完整真实IOKit调用与DMA/CPU/IRQ静止仍未证明。

2026-10-23 R06 EQ create rollback owner：发现`eqCreate`在任一创建回滚中`mttFree`拒绝时仅返回false，ring与MTT owner留在分配器但controller得不到指针；同时controller把所有未知EQ teardown都当成可能已发出`SW2HW_EQ`。现在所有5个EQ回滚拒绝点都返回ring/pages/MTT/doorbell元数据，controller持久化失败owner和`fEqMayBeDeviceOwned`状态；teardown仅在SW2HW可能已到设备时发送HW2SW，未发命令的owner可直接重试释放。新增行为测试覆盖未发命令场景、拒绝释放后保留owner并重试；retention teardown 32 cases/142 checks、unknown-owner contracts通过。完整27个Python脚本exit 0；隔离`BUILD_TAG=0.44.105-local`构建PASS，kext SHA-256 `0ec4b6dd8dc1d80184c717f792755006718cca2be2951c675bd5d61bdb3353c5`，产物`/tmp/cx3-eq-create-retention-20261023/`。离线/合成验证；CQ/RQ/SQ创建回滚相同缺口尚未处理，未证明真实硬件静止；未安装、未加载、未触碰硬件。

2026-10-23 R06 MTT owner/generation slice：MTT ledger现将每个range绑定到实际`Mlx4Icm*`、allocation ID、allocation generation与handoff后的owner generation；`markDmaHandoff`同步校验/标记对应range。`mttFree`返回可观察的bool，只接受精确range+精确owner/ID/generation；RQ、CQ、TX SQ/CQ、EQ teardown在拒绝时保留owner字段和底层ICM，不重复发送已确认成功的HW2SW命令，供安全重试。更新QP/TX合成fixture以匹配真实接口。generation契约25检查、MTT allocator 29检查+真实`mttFree`行为+table setter检查、retention teardown 31 cases/138 checks、QP 107 cases/1206 checks、TX 35 cases/302 checks；完整27个Python测试全通过。隔离`BUILD_TAG=0.44.104-local`构建PASS，kext SHA-256 `7c6750830adc7e69286fb539bd74cca0bb86babd15bd866b160633302beb89fd`，产物`/tmp/cx3-mtt-owner-20261023/`。仅离线/合成验证；generation不是静止/free凭据，创建过程回滚、实验A/B dummy reservation及全部DMA/CPU/IRQ/reset-scope释放闭环仍需审计。未安装、未加载、未触碰硬件。

2026-10-23 R06 CQ/EQ `UnknownRetained`：在`test-retention-teardown.py`上先加“未Up CQ owner在HW2SW失败后可保留并在重试成功后释放”的行为测试，初始4项失败；修复生产`cqTeardown` early-return后行为测试通过。CQ/EQ创建超时现在返回ring/MTT等owner元数据并由controller持久化；未知CQ阻止重复创建，未知EQ阻止探针/poll/IRQ/net-arm，teardown失败仍保留，成功路径才释放。全量27个Python测试通过；隔离`0.44.103-local`构建PASS，SHA-256 `855582de78d7edadecf66afaf4c3e914f23fe098effdfbc1ccccffdb925d176b`，产物`/tmp/cx3-unknown-owner-build.zhgIIS/out/`。离线/合成测试不证明真实硬件行为；MTT owner-gen绑定、`mttFree`失败传播与DMA静止凭据仍未闭环。

2026-10-23 R06 MTT slice：修复EQ错误传`mttFree(0,0)`和非LIFO回收，并发现旧bump allocator会接受伪造的partial top-range free、回退到仍存活MTT范围中。现保存EQ页数、仅在成功HW2SW_EQ后归还；reset-only的EQ/CQ/RQ/SQ teardown保留索引。分配器跟踪精确的256个allocation owner记录，partial/duplicate/untracked free及UInt32 high-water溢出拒绝；owner ledger满时拒绝新分配而不改high-water mark。活跃MTT owner现在阻止profile释放/重建、table view替换与bring-up设备命令。MTT helper 24行为检查+5个setter行为检查+4源码合同检查；ICM release 41 cases/350 checks；reset preflight 105 cases/682 checks；retention teardown 25 cases/108 checks。全量测试与完整构建通过，kext SHA-256 `181c8a80b6191b8c065af4a6f293c6a1fd7ccafe9428a0bfd5503c8ad065f302`。离线修复不代表DMA静止凭据；记录上限会拒绝后续分配，实验队列owner与并发/生命周期仍需继续审计。未安装、未加载、未触碰硬件。

2026-10-23 R06 TX callback slice 3：为补齐CAS前窗口，新增独立thread_call同步终止worker，stop在2秒deadline有界等待；worker调用SDK `detachInterface(..., true)`，成功后release接口引用并在自身回调中按官方thread_call合同自释放；超时、缺worker或call-free失败均让stop pin self/provider并在设备资源teardown前返回。一个worker接管普通接口与attach迟到接口，重复指针合并release计数。`test-net-detach-worker.py`直接抽取生产helper/worker执行7个行为场景/24检查（成功单/双接口、重复引用、无接口、缺worker、超时、worker自释放失败）；`test-net-stop-tx-drain.py` 13项合同；完整26个Python测试通过。`BUILD_TAG=0.44.101-local`隔离kext/CLI构建PASS，hash `d035e34b6ab7a07f66a32b5f8cb7ebb19f4782858b3f5b765f0d952fc887718a`，产物`/tmp/cx3-txsync-final.qek88e/out/`。该设计以SDK同步终止契约补上未入CAS的family引用窗口，但尚未在真实IOKit/硬件运行；它不是DMA静止证据，timer/workloop、IRQ、reset scope及实际free凭据仍未闭环。未安装、未加载、未触碰硬件。

2026-10-23 R06 TX detach already-pending 分支：Apple SDK 头文件定义 `thread_call_enter()` 返回 TRUE 表示 work item 已待执行。原代码在准备接口数组后先 `retain()`，若返回 TRUE 却未新排入回调，仍把该额外self引用留给旧worker释放；该旧worker只拥有先前排队时的引用，因此可能永久多留一引用，且驱动字段仍指向将由worker release的接口。新增主机行为用例先红3项；修复后引用转移给待执行worker、清除驱动侧接口字段，并平衡当前未新增回调的retain。`test-net-detach-worker.py` 8 cases/30 checks、stop合同13项、retention teardown 25 cases/108 checks均PASS；全部26个Python测试通过。`BUILD_TAG=0.44.102-local`隔离完整构建PASS，kext SHA-256 `a30c17f92a778aa0989859079ff9f64656705146a938e1d6624b947a107696e1`，产物`/tmp/cx3-detach-pending-20261023/`。这是SDK语义与离线模型验证，没有真实IOKit/硬件复现；该调用分支未被证实曾在线上触发。设备DMA、timer/workloop、IRQ、reset scope和release credential仍未闭环。

2026-10-23 R06 MTT source inventory：逐一追踪`cqCreate/rqCreate/sqCreate/eqCreate`、A/B EQ dummy与ring分配，以及所有常规teardown的`mttFree`。明确CQ/EQ `SW2HW`超时时ring/MTT虽保留，但函数输出仍为空，controller丢失owner指针，后续teardown无法重试；A/B dummy range永久占用一个固定owner ledger记录。完整分类及尚缺的credential/副作用关系见[`r06-mtt-owner-inventory.md`](reports/r06-mtt-owner-inventory.md)。另修正`teardownEq`源码注释，消除“reset会rewind MTT”的旧说法；实际reset路径会保留MTT indices。仅做源码盘点/注释修正，尚未修复CQ/EQ timeout owner可达性，也未作硬件验证。

.93记录PCI vendor/device/subsystem+BDF，并在FLR前后对比当前provider identity；11个新增/调整的身份漂移门禁与平台适配器均通过，32组离线检查及kext/CLI构建exit0。18个生产源live/tar清单一致，两个binary哈希封存。身份不是generation或DMA静止凭据，owner仍不释放，R06仍PARTIAL，未上机。

.96加入provider-local generation计数：start绑定为1，每次HCA reset/平台FLR尝试前检查溢出并递增；ICM记录allocation generation，MAP_FA/MAP_ICM/MAP_ICM_AUX首个命令前记录may-be-device-owned generation，quarantine/overflow/UNMAP保留账本复制该标签。新增纯模型、源码顺序及保留账本测试；33组检查和kext/CLI构建exit0。该标注目前未覆盖QP/CQ/MR通过MTT转交设备的所有路径，也未被实际free site作为凭据消费；不允许据此释放。详见[R06审计](reports/r06-generation-scope-audit.md)。

.97新增实际DMA handoff登记：正常CQ/EQ的SW2HW及RX/RSS/TX QP状态转换之前，整组预检并标记其ring/record/buffer/SQ owners；标记失败则拒绝后续设备命令。generation模型明确仍只是标签，任何free site都没有消费release credential。目标测试18项源码契约、QP链107场景1206检查、TX链35场景302检查通过；全量离线批次52含33项与构建exit0。审计仍需枚举MR/MTT、实验命令和全部释放点，R06保持PARTIAL且未上机。

.98/.99续补MR与实验handoff：物理MR在SW2HW_MPT前标记`fMrGeneration`；CQ/EQ及实验A/B路径在命令前标记owner，无法证明设备未持有时保留ring与MTT。新增代次来源契约断言后，`test-generation-stamp.py`为25项PASS；QP链107场景1206检查、TX链35场景302检查、ICM释放40场景343检查、retention teardown 25场景93检查全部PASS。新离线批次`verification-batch54.json`封存候选0.44.99-local：33组检查、kext/CLI构建exit0，源与测试输入运行期间无变化。MR代次仍未由释放点消费；其他实验CQ/RQ/SQ/EQ的每个失败/析构分支、MTT复用与UAR/doorbell owner仍需完整盘点。该批次不证明DMA静止或允许上机，R06/P0/P1仍PARTIAL。

.91进一步核验FLR前身份、BAR0与PCI memory decode；无效时平台调用次数为0，sticky
latch不置位。4个反例先红12项，修复后96场景552检查0失败。verification-batch45
32组及构建exit0；65条live/tar清单及两binary一致。其余代次/scope/排空/R06未闭环。

.90接入公开平台FLR事务，无手写bit回退。失败零后续设备访问，成功仅配置核对，
不读旧MMIO、不清poison/owner、不free，保持阻断。入口92场景532检查、适配器
9场景62检查、工具5检查通过；旧.89入口同测试50失败。32组及构建exit0，65条
live/tar清单与两binary一致。R06仍PARTIAL，继续TRPND/CPU排空/scope/代次诊断。

.90独立FLR审查无本切片阻塞；跟进：reset前身份/BAR损坏应更早拒绝及补反例、
多function范围核定仍为上机前门禁。永久阻断当前function不能保护未知peer的运行。

.89补reset/FLR局部跨selector准入：尝试前阻断设备访问，仅空实例首次成功HCA
reset允许继续初始化；失败/旧owner reset/手写FLR后仅host-only netstat允许。
全部47 selector及锁内重查已覆盖，157场景471检查0失败；reset81场景459检查
0失败，旧.88同测试35/184失败。30组及构建exit0，63条live/tar和两binary一致。
该latch不是代次/scope凭据，无真实恢复；平台FLR、CPU回调排空及全局预算继续开放。

.88关闭命令入口的缺锁fail-open/懒分配：init预分配锁，失败回滚该锁及已得BAR0
映射；三种命令包装缺锁均NOT_POSTED且无core/HCR访问。真实方法13场景104检查
0失败，最终同测试重放.87为42失败；30组及构建exit0，启动路径仍无设备写/DMA。
63条live/tar清单及两个binary一致。跨selector的reset失败/干净初始化分流、预算、
shutdown并发排空与恢复未完成；不能把本局部修复当R03/R06或P1-S关闭。

优先纠正：.84/.85不部署，旧QP测试替身漏了真实cmdBox第8参数bypassBusy，
把观测指针错传为bool，既漏观测又旁路busy；撤销下文历史“真实QP观测接通”结论。
.86显式传false，真实QP/HCR集成17场景186检查0失败，同测试重放.85为42失败。
29组及构建退出0（verification-batch40.json），62条live/tar清单和两个二进制一致。
Apple Clang 17警告拒绝历史成员取地址写法，但不拒绝普通指针变量；.87以deleted
模板重载拒绝第8参数指针，两类编译负例不加Werror均被拒，17场景186检查继续通过。
29组及构建退出0（verification-batch41.json），62条清单live/tar及两个binary一致。
下一项继续R04身份与R03/R06跨selector准入/预算/恢复。未上机、不证明历史冻屏归因。

首批报告：[能力基线](reports/capability-baseline.md)、[安全审计与修复规格](reports/safety-lifecycle-audit.md)、[RSS布局与raw采集规格](reports/rss-qpc-layout.md)、[危险实验门禁](reports/hardware-test-safety-gate.md)。外层产物`outputs/20261022-cx3-fulladapt-batch1/`含机器清单、离线测试和历史23:32观测快照；最初隔离红例已有9项通过证据，但剩余安全缺口未关闭，危险实验仍未放行。

## 协作与执行约定

- 先读总计划共同验收契约、相关Fxx和Dxx，再领任务；下列依赖指验收依赖，离线研究/安全回归可提前，但不因此放行硬件写。
- 主代理负责范围、决策、验收与集成。若使用子代理，每次明确文件/模块所有权、不回退他人修改、任务文件投递、终态回收；本计划不要求必须并发。
- 用户已重新允许协作。模型、档位、渠道身份和不可用时的处理以[会话执行目标](CX3-PRO-SESSION-GOAL.md)为准；主代理可独立完成所有任务，派出和收到结果不等于验收通过。
- 同一张卡只有一个硬件写入者/实验owner；软件任务可并行，初始化、reset、端口配置实验串行，禁止两个模型各跑一套链。
- 每项先产出可独立评审的规格与失败测试，再实现/离线验证，最后按实验窗口上机；未决架构不编造代码级步骤。
- 现有未提交代码与0.44.48候选保留；不自动提交/推送、不为新计划重启。
- 机器证据写任务自己的外层 `outputs/`，只将脱敏报告/必要摘要加入驱动仓库。未来文件路径是产物约定，不表示本轮已经创建。

## 近期任务：先把未知与安全门拆开

### T01 本卡能力与实验基线 — PARTIAL（离线位映射/查询规格继续）

**覆盖** F01、F05、F29、F36、F38；决策 D01、D06；依赖：无。

输入：`docs/REFERENCE.md`、`linux-diag/run1-evidence/`、已有raw capability、`src/cx3_abi.h`、`vendor/freebsd-src/`能力定义。
负责：能力盘点报告/机器清单；必要新增只读查询先评审其底层命令是否真的无写，不能重演SET_PORT冒充回读。

- [x] 离线列出已有PN/FW/cap/PCIe/端口证据，标记数据年代与未知字段；不凭官方常见PSID填本卡值。
- [ ] 形成查询清单：可用只读selector与缺失raw字段分开；如查询需先初始化，标为写前置而非“只读随时可跑”。
- [ ] 在安全窗口采集所缺身份、portcap、速率/PCIe/模块数据，必要时安排独立Linux参考启动；不自动切模式/刷FW。
- [x] 输出 `outputs/<run>/capability-raw/`、`capabilities.json`、`docs/reports/capability-baseline.md`，每cap映射Fxx；列出对端/线缆/交换机/恢复资源缺口（本批：`outputs/20261022-cx3-fulladapt-batch1/capabilities.json` + `docs/reports/capability-baseline.md`；port2/PSID/模块仍缺）。

验收：raw↔decode抽样与参考一致；全部特性位/限制入账；未知/不适用有依据；绑定接口规则明确，默认路由不能使测试走en0。

### T02 资源安全、冻屏故障树与退出不变量 — PARTIAL（隔离、两处顺序、selector门禁、ICM保留账本、reset-only释放门控、PCI bus-master关闭、poisoned接口enable拒绝已实现；DMA静止证明仍OPEN）

**覆盖** F02；决策 D02；依赖：已有代码/历史报告即可，硬件退出验收依赖T01、T04。

输入：`docs/HANG-AUDIT.md`、`docs/DEFECTS.md`、`docs/LOCK-DESIGN.md`、`docs/ICM-UNMAP.md`及源码 `Cx3Mlx4.*`、`Mlx4Hw.*`。
负责：故障树、资源owner/lifetime表、锁图和缺陷回归；不在这项顺手改全部数据通路。

- [ ] 对每次历史卡死建立boot/版本/阶段/最后远端证据/USB和管理链路/是否panic的事件行，事实与假设分列。
- [ ] 列出每条分配/释放、IRQ/timer回调、user-client close、错误/超时分支；记录DMA停止的实际证明条件。
- [ ] 为可重现不变量错误增加失败测试，尤其序号回绕、重复close、部分分配、超时后释放和重置并发；逐项最小修复。
- [x] 输出 `docs/reports/safety-lifecycle-audit.md`、离线回归与实验提案；区分已确认缺陷和未证明历史根因。

验收：无未解释的“reset完就释放”路径，无并发双写；所有仍可能被DMA访问的内存有隔离策略；故障注入可以证明安全终态，不以短时间未冻屏通过。

### T03 RSS raw QPC核对与最小对照设计 — PARTIAL（规格已有，raw/fixture与上机未完成）

**覆盖** F15；决策 D03第一问；依赖：离线无；硬件实验T01、T04与T02安全审查。

输入：后续0.44.48报告、selector35已有解码、QP context构造/QUERY_QP解码与BSD参考。负责：QPC语义和最小诊断，不启用多队列。

- [ ] 对比发送context/查询布局/内存覆盖范围，写raw字节采集规格和解码fixture；普通RX QP作为对照。
- [x] 写key只变一个变量、QPN/CQN/环不变的两个独立运行方案；预先写出三种假设的预测和否定条件（两次运行仍可能不足以排除确定性损坏，见报告）。
- [ ] 在独立受控启动中采集完整raw、key与资源读数、线上定向流量；原活跃会话不被强行reset。
- [ ] 输出 `docs/reports/rss-qpc-layout.md` 与raw/fixture，保留FULL/QUALIFIED判据差异。

验收：能够区分字段复用、解码问题与状态损坏；若不能，原样报告阻塞，不放宽校验器凑PASS。

### T04 探针、独立管理与危险测试门禁 — PARTIAL（快照已核，新回执/phase演练未做）

**覆盖** F37；决策 D06；依赖：无；演练通知在明确窗口，演练不得模拟真实硬件故障。

输入：外层 `tools/freeze-capture/`、本项目preflight脚本和上次ntfy投递结果。
负责：观测/运行手册与门禁，不能顺带修改其他会话的探针服务或CPA。

- [x] 核验远端记录器/管理网/持久化路径，区分CX3 phase-watch与一般host-watch覆盖（23:32历史快照，不是永久放行）。
- [ ] 测试明确标注“演练”的通知，手机接收与锁屏弹窗分别登记；不能只设环境变量假装回执。
- [ ] 固定每轮身份/阶段/心跳/超时/异常现场收集；确认恢复入口与停止条件，不自动重启。
- [ ] 输出 `docs/reports/hardware-test-safety-gate.md` 和实测回执；不足项阻止危险实验。

验收：被测链路失效时仍可记录/告警；至少明确一种用户可及时注意的提醒方式。若用户接受非弹窗接收，风险必须写明且实验需有人值守，不能再假定忙碌中会及时看到。

### T05 可重复stop/reopen、失败回滚与隔离 — 隔离部分已实现；DMA 静止的机制与验收设计见 [dma-quiescence-design](reports/dma-quiescence-design.md)；stop/reopen硬件验收BLOCKED(T02、T04)

**覆盖** F02；决策 D02；硬件验收另需T01。
负责：核心状态机/资源事务，文件落点从 `Cx3Mlx4.*`、`Mlx4Hw.*` 和对应离线测试确定，不同时做多队列/offload。

- [ ] 先写状态转移、允许命令和每状态owner/回调/内存不变量；定义失败后继续/回滚/必须重启的区别。
- [ ] 用分配失败、命令超时、重复close、reset并发建立失败回归；实现有界撤销与重新建链。
- [ ] 独立硬件窗口做接口down/up、stop/reopen、失败阶段再进入和链路中断；检查另一管理设备健康。
- [ ] 输出 `docs/reports/stop-reopen.md` 和100轮计数/内存快照；不可恢复时必须安全隔离而非无限尝试。

验收：100轮无资源持续增长/陈旧回调/非法DMA；成功重建或明确安全终态；满足P1-S。

## 后续交付分支：决策后再生成代码级实施规格

以下任务定义交付和阻塞边，不伪装为已可直接执行的实现代码。每项开工先将相应决策收敛，拆成可独立测试的小任务；大族不能一个提交全部开启。

| 任务 / 状态 | 范围、输入与产物 | 验收 | Blocking edges |
| --- | --- | --- | --- |
| **T06 安全自动启用** / BLOCKED | F03；D04；输入现有手工链/状态机；产出原生网络启用服务、权限/禁用/恢复手册与版本门测试 | 无手工selector联网；错版本/被杀/重复启动不会双写或拖死开机；保留start守卫 | T05、T04；先关闭D04 |
| **T07 双口、媒体与电源** / BLOCKED | F04–F06、F35的电源部分；输入cap/资源模型；产出每口对象/共享PF边界、link/media和power实现、双口报告 | 冷启10/暖启20/sleep50/link100；单口重开不损坏另口；支持速率分别测 | T01、T05；自动开机验收依赖T06 |
| **T08 完整L2/MTU/系统集成** / BLOCKED | F06–F09、F18；输入端口事务与macOS KPI研究；产出过滤、MTU、VLAN、桥接/bond等独立规格/回归 | 双口标准/边界帧、组播/隔离/配置切换与系统应用互通；QinQ等仅cap支持时承诺 | T05、T07；外设依D06 |
| **T09 Offload与DMA性能基线** / BLOCKED | F10–F13；输入mbuf合同、cap、Linux参考；产出checksum/TSO4/6/SG各自开关与测试；聚合/零拷贝先决策 | 真实TCP线上分段/校验和、内存压力/异常回收；无jumbo也能验TSO；给CPU/延迟/吞吐 | T01、T05、T07；组合验收依T08，不把jumbo作TSO硬前置 |
| **T10 RX中断与调节** / BLOCKED | F14；输入CQ/EQ/arm原始证据及已修诊断回绕；产出再arm竞态规格、ISR/workloop实现与调节/回退策略 | 无丢唤醒/风暴，负载/空闲CPU对照、回退/close正确；0.44.48诊断路径若使用须单独硬件验收 | T05、T04、T01 |
| **T11 多硬件队列与steering** / BLOCKED | F15–F17；D03第二问；产出两队列垂直切片、逐步扩队列、流映射/锁/向量/配额规格与测试 | 多流真实分流、流内有序、回压公平、重配/关闭无串用；架构不可行要证据 | T03、T05、T07、T10；T09提供性能参照 |
| **T12 时间/QoS/隧道/诊断** / BLOCKED | F17、F19–F23、F36–F37；输入cap和实验资源；分别产出时戳SDK/PTP、DCB、隧道及诊断子项目规格/工具 | 按能力矩阵逐项线上测；PTP有误差阈值；PFC死锁恢复；不把AVB/TSN当既成事实 | T01、T05；数据面依T07/T09，RSS组合依T11；D06外设就绪 |
| **T13 安全RDMA最小SDK** / BLOCKED | F24、F43；D05；产出版本化ABI、权限/对象/内存模型、macOS最小RC客户端+Linux对端与析构回归 | context/PD/MR/CQ/QP→SEND/RECV→销毁端到端；坏句柄/进程退出不泄漏/越权DMA | T01、T05、T07；D05关闭；T12中相关QoS子项按实验需要 |
| **T14 完整RoCE与verbs扩展** / BLOCKED | F25–F28、F43；输入T13；产出RC/UC/UD/atomic/MW等本卡子项、CM/GID/v1/v2互通矩阵与消费者示例 | 两方向真实RDMA、错误/重试/撤销/隔离；普通网络+多进程RDMA压力通过；每子项独立PASS | T13；相关T08/T10/T12子项，不能为RoCE无损假设跳过拥塞测试 |
| **T15 VPI / InfiniBand / IPoIB** / BLOCKED | F29–F32；D07；产出mode工具、MAD/SM/PKey/GID、IB transport与macOS IP入口规格/实现 | IB对端实际互通、模式回滚、分区隔离、IPoIB应用可用；双口混合按cap验 | T01、T05、T13；D07及IB实验资源就绪；复用T14适用transport |
| **T16 虚拟化及专有/板级扩展** / BLOCKED | F33–F42、F35剩余部分；D08；按VF、WoL、peer memory、专有offload、sideband、ROM/FW分别出规格 | guest与IOMMU隔离、真实peer DMA、唤醒/ROM接管等分别验；不支持有硬证据，平台缺口仍阻塞 | T01、T05、D08；按子项依T07/T13/T14；所有持久硬件变更另授权 |
| **T17 版本发布与组合回归** / BLOCKED | F43–F44；D09；输入全部报告；产出SDK示例、安装/回滚、支持矩阵、许可/依赖审计、离线CI和发布说明 | 日用候选48h；全功能候选72h；全部适用子项PASS、UNKNOWN/BLOCKED为0；可复现安装与恢复 | 日用：T01–T11及适用T12；全功能：T01–T16；D09关闭 |

## 当前就绪任务与阻塞边（2026-10-23修订）

下列R编号是T01–T05的近期子项，不替换既有任务；状态反映本轮核对结果。每项交付一条可独立验收的行为链，按真正阻塞关系推进。

| 子项 / 状态 | 交付行为与验收证据 | 阻塞边 |
| --- | --- | --- |
| **R01 候选记录闭环 / DONE（离线部分）**（T02） | 0.44.59的验证工具快照、完整日志、源/测试清单与哈希已归档；起初暴露了build.sh替换OUT导致证据丢失，后续runner把构建放子目录并监测源清单，正式重跑无变化。0.44.60起保留CHANGELOG和整套证据 | 无 |
| **R02 未证明owner的释放 / PARTIAL**（T02/T05，最高安全优先级） | 已补QP失败record与CQ/MR/ICM依赖、live profile拒绝、.69容量/MTT/EQ保护及.70/.71 QP owner/readiness/query门禁。.72让RX/RSS撤销先清ready/网络准入，2RST或retention失败仍持有owner，不能重新enable/rebuild。QP链105场景1166检查、retention24场景89检查0失败，重放.71分别10/4检查失败；TX35场景302检查通过。仍缺provider/并发和代次范围证明，不称完整依赖图闭环 | R01完成；剩余与R03/R06共享生命周期文件项串行 |
| **R03 停止失败与容量 / PARTIAL**（T02/T05） | mailbox不可复用已补；.69隔离及ICM账本满时保留调用者原指针，不新增untracked transfer，无CPU view不授权free；容量耗尽/overflow后停数据面并拒绝selector（含锁内再查）及enable。selector 8场景24检查、ICM/profile 40场景329检查通过。锁获取无65秒上界、super::stop早退存活、事务前条目/字节预算、未耗尽时重复reset及provider跨终止仍开放，见安全审计 | R02的已整合部分完成；剩余依赖R06契约 |
| **R04 RSS采集与ABI闭环 / PARTIAL**（T03） | QP/RSS每步264字节post前快照/4KiB query、版本章和独立decoder已有离线证据；.123完成运行身份/PCIe绑定，.128完成同连接RSS绑定/时间与ABI核对/候选collector，成功须本次最终query回执，status0设备拒绝不误判。56脚本/构建/91输入及GPT复审通过，见[验收](../../../outputs/20261024-cx3-rss-runtime-binding/README.md)。仍缺真实boot/raw、普通QP对照/key A/B及RSS语义 | 离线身份绑定已实现不重做；真实采集依赖R07 |
| **R05 P0缺项与实验资源 / READY**（T01） | 用raw/主来源逐字段核定剩余配额的字节序、偏移、指数和单位；异常值保持未决并给出否定依据，不因数值大就断言错误。PSID、port2、当前PCIe、DOM等列成准确查询表，标记所需初始化/写前置；对端、交换机、IB/PTP资源列明缺口。2026-10-24 离线查询/字段布局审计见[报告](reports/r05-query-audit-20261024.md)：DEV_CAP字段offset与FreeBSD参考一致，区分rawWords主机序与原始BE字节；确认QUERY_PORT的MAC缓存已按端口分离，仍未执行任何新selector | 离线无；新硬件采集依赖相应R07准入 |
| **R06 静止凭据与恢复候选 / PARTIAL**（T05） | MTT range现绑定DMA owner/allocation+owner generation；拒绝释放向RQ/CQ/TX/EQ teardown传播，owner保留；CQ/EQ/RQ/SQ创建回滚owner可达性已有离线路径。`.108/.109`限制A/B只在MTT布局可判读时运行、dummy reservation单份精确跟踪；`.109`补A/B ring失败后的controller owner重试；`.110`阻止该owner存活时递增代次并reset。TX admission/detach已有离线覆盖。源码盘点确认无动态UAR alloc/free API，doorbell通过共享BAR2 mapping。仍缺其余实验队列和所有release-site行为覆盖、MTT并发/table-generation边界、正常/异常release credential生产与消费、完整reset scope（peer function及本卡两个物理端口）、TRPND/CPU/workloop/IRQ drainage与失败零free证明。真实IOKit/硬件静止尚未验证；当前无release credential，generation标签本身不授权free。详见[R06 generation/scope审计](reports/r06-generation-scope-audit.md)与[MTT owner盘点](reports/r06-mtt-owner-inventory.md) | 离线可继续；proof生产者需要平台/设备语义；真实scope和DMA停止证据依赖R07及实际硬件 |
| **R07 单变量硬件窗口 / BLOCKED**（T01/T03/T04/T05） | 每次只提交一份最小实验包：确切版本/hash/ABI、允许selector、预测、停止与恢复；先更新独立管理/远端phase持久化与演练回执，再由当轮暂停窗口放行。先取得安全退出/保留证据，再做可安全收尾的QP/RSS对照；一次通过只关闭该实验断言 | 该实验涉及的R01–R06离线项通过并独立审查；T04新回执及当轮窗口。**不依赖最终P1-S PASS** |
| **R08 P1收敛与晋级 / BLOCKED**（T02/T03/T05） | 安全退出/失败隔离硬件证据、历史事件台账、100轮stop/reopen和RSS语义分别关闭；正常循环无持续资源增长，故障保留有预算且阻止无限重试。P1-S与P1-R分开结论，满足相应门后晋级T06/T07或T11 | R07对应实验及回归完成；T01基线满足相关验收 |

## 当前执行前沿（2026-10-24；0.44.128-local离线验收通过，子项就绪不等于硬件放行）

2026-10-25前一boot运行回归（新boot已临时恢复，见顶部）：0.44.47短时DHCP/外网通过后，01:46:08出现首个TX CQE错误0x02/0x6d，之后连续0x05/0xf9；05:11为169.254/no-server。见[只读事件分析](../../../outputs/20261025-cx3-dhcp-loss-readonly/analysis.json)。先定位首发与故障持续发包路径、建立有真实反例的离线回归；不重置正在使用的硬件、不将旧联网PASS冒充当前状态。en0仍在线，但IP变为.101，60主动probe仍旧.196需另行处理。

[.128验收](../../../outputs/20261024-cx3-rss-runtime-binding/README.md)完成RSS运行身份绑定及最终query成功判据；CLI/独立decoder/collector保留失败诊断，即使错误CLI exit0也独立拒绝。56脚本、完整构建、91项冻结输入及GPT Luna/high复审通过；没有kernel/ABI变化或设备操作。[固定superclass源码核查](../../../outputs/20261024-cx3-provider-stop-source/REPORT.md)未发现公开stop主动close PCI provider/free路径，已收窄疑点；目标版本匹配、PM/迟到callback/provider仲裁仍未证明。

[.127验收](../../../outputs/20261024-cx3-network-stop-contract/README.md)补start的旧实例guard与provider open失败零PCI enable/map/发布；完整矩阵见[stop-reopen](reports/stop-reopen.md)。同最终测试重放.126产生22项行为失败；三个guard/忽略open/错误close五种mutation均编译成功且被测试拒绝。51脚本、完整构建、85项输入前后/tar一致，GPT Luna/high复审完成；诊断level0重复publish为有界未覆盖项。完整IOKit/provider/设备DMA与真机stop/reopen仍未验收。

[.126验收](../../../outputs/20261024-cx3-fw-normal-close/README.md)完成HCA/table正常撤销之后的FW精确UNMAP/free及命令层收尾接线。成功结果严格检查，失败/未知保留原slot和self/provider，RUN_FW sticky/poison/generation不清。90场景399检查、UNMAP_FA 20检查、50脚本/完整构建及84项冻结输入通过，GPT Luna/high独立复审完成。此前.123身份绑定、.124接口启停、.125端口/HCA关闭均保留；不重复这些实现。当前候选身份见[会话执行覆盖](CX3-PRO-SESSION-GOAL.md)，本批没有上机。

**立即下一项**：形成“候选联网→安全关闭→新实例再次联网”单变量最小实机包及观测/采集错误回执设计，推进真实child/table撤销组合；目标provider open仲裁、termination和框架运行语义仍待证明，不重做公开superclass源码盘点。实际IOKit/provider、DMA语义、预算/异常恢复仍独立开放；R05模块GET可离线推进，R04剩余真机raw/对照/key A/B。14:42人工强制重启未明根因，本机快照缺口仍需补齐；不能归责未加载候选或晋级任何功能。没有新硬件窗口。下方旧版本段落为历史。

当前候选和本批证据以[会话权威摘要](CX3-PRO-SESSION-GOAL.md#当前事实与产品目标状态)为准。[.120验收](../../../outputs/20261024-cx3-stop-composition/README.md)完成停止失败组合修复：context/table/AUX/FW保留拒绝时在依赖清理前pin self/provider，网络frame-ring拒绝保留原slot/lock；全表初始化与raw-frame谓词复审缺口已补。stop组合19场景92检查、retention链37场景164检查、ICM/profile链42场景357检查通过；最终同测试重放.119为80项行为失败。41个Python脚本与完整构建exit0，73条输入前后/tar一致，GPT Luna/high复审通过。原始review的rawframe触发限定为reset-time合成输入，普通stop因容量拒绝的推论不成立。成功转入retention账本后的跨stop/free/provider存活、预算和正常/异常静止语义仍是R02/R03/R06下一主线，未被本批拒绝分支覆盖。CPU/doorbell已有有限审计和timer/stop交错，不重复全量盘点。[.119](../../../outputs/20261024-cx3-pcie-query/README.md)的PCIe查询/导出/独立解码和R04构建元数据子项保留。没有上机或新增能力族PASS，本卡PSID/port2/当前PCIe仍待实际采集；模块EEPROM/DOM专用GET与运行身份可离线并行。下方逐版本增量是历史记录；R04/R05/P0及R02/R03/R06/P1均未关闭。

整体门状态仍按上表记录；本节解决“父任务 BLOCKED 导致其全部子项停工”的问题。每项都要把实现、离线、硬件、长期验收分开记，不能将离线 PASS 直接晋级为硬件 PASS。

### 本轮重规划摘要（2026-10-23；Astra high 只读分析后收敛）

- **两条证据线并行保留，不做版本回退叙事**：0.44.47/M4 是已实测的联网基线；0.44.110-local 是最新离线候选。后者未上机不代表前者的历史实测失效，也不代表新候选已继承其硬件表现。未经新实验，不声称当前加载状态。
- **执行拆成三条流**：A) R02/R03/R06 的当前候选安全闭环，先处理可达的CQ/EQ未知owner、MTT归属及实际释放点；B) R04/R05 的身份/能力查询与raw消费离线工作；C) D04/D05/D09 的自动启用状态机、SDK权限/ABI、安装许可文档。B/C不等待完整P1-S，但不得绕过其各自的硬件副作用门。
- **硬件只作为独立串行流**：每个实验只回答一个断言；先完成该路径的安全前置和T04观测/告警回执，再请求当轮暂停窗口。实验通过只关闭对应断言，不自动晋级整阶段。当前没有由本规划授予的安装、启动、reset或重启授权。
- **进度按断言关闭计量**：记录新增的生产不变量、独立反例、离线/真机证据和剩余阻塞边；不按版本号、测试数量或综合百分比报进度。
- **交付层级不混称**：已有“基础联网原型”是历史事实；稳定日用以太网版仍需对应稳定性与网络验收；全功能版仍需F01–F44适用子项通过、UNKNOWN/BLOCKED清零且所有N/A-HW有证据。

| 顺序 | 可执行工作 | 完成包 / 退出判据 | 硬件边界 |
| --- | --- | --- | --- |
| 1 | **R06 当前候选安全闭环**：精确MTT range owner/代次绑定、拒绝传播、CQ/EQ/RQ/SQ创建回滚owner及A/B ring失败重试已有离线切片；reset preflight拒绝在A/B owner存活时推进代次。A/B dummy reservation限制为单份精确跟踪。UAR无动态分配，doorbell共用BAR2映射，stop顺序已盘点；下一步覆盖剩余实际release-site行为并设计由真实静止证据产生、被free site消费的credential | 创建回滚拒绝路径必须保留可追踪owner、正常可重试释放且不得重复无关设备命令；范围审计缺口需逐项关闭。不是R06完成 | 代码/离线可推进、不产生设备命令；真实IOKit/family、IRQ/workloop drain、TRPND与reset scope仍待证明 |
| 2 | **R02/R03 生产安全切片**：对审计包内实际释放点验证凭据/owner 消费、停止准入、worker/provider存活、容量预算及失败终态 | 同一行为链正反例均通过；失败时保留可追踪 owner，正常成功路径能够继续；明确未覆盖范围和阻塞依据 | 代码测试不授予硬件写权限；危险实验依实验专属门禁 R07 |
| 3（并行） | **R04/R05**：补运行/boot/版本身份与 raw 导出消费合同；核正剩余 capability 字段、port2/PSID/PCIe/DOM 查询及副作用，整理实验资源 | 身份未知时拒绝伪归因；字段有来源/偏移/单位；每条查询注明副作用与前置，不把port2查询伪装成无副作用 | 离线规格可直接推进；任何新 selector 按 R07 审批 |
| 4（并行） | **D04/D05/D09 子项**：自动启用/回滚状态机、SDK ABI/权限威胁模型、安装签名/许可/支持文档 | 子项有可独立审查的设计、失败矩阵和离线验证；不裸露任意BAR/DMA；未选定架构不写大规模实现 | 设计/用户态工具可推进；内核 DMA 路径和实际安装仍需对应门禁 |
| 5（串行） | **R07 第一实验包**：准确候选和哈希、单一问题、selector 白名单、停止/恢复、管理网/远端记录与告警接收回执 | 由主代理独立复核实验断言及安全覆盖；先在单次批准窗口重证必要联网/退出路径，再决定是否进入RSS或循环测试 | 必须满足对应安全前置、T04实测回执与当轮用户明确暂停窗口；本计划本身不构成设备操作授权 |

R01 已 DONE，不再补 .59 证据。R02/R03/R06 仍 PARTIAL；R04/R05 可并行；T06–T17 的总体硬件/最终验收仍 BLOCKED，但上述离线设计、文档和隔离测试不因此自动停止。不得通过累计候选版本或修改测试期待值关闭任务。

2026-10-23 R03增量：.73已补准入/长事务解耦和实际drain预算；.76补失败时显式
self/provider引用pin，selector未排空不操作thread-call，失败前不拆orphan接口/归还引用，
worker最后资源访问后的完成栅栏及失败态不重试。停止保留7场景86检查0失败，重放.73
为50失败；25组离线检查及构建退出0（`verification-batch30.json`）。.74缺worker栅栏、
.75含不可绑定API且旧构建门禁假绿，均不部署；新门禁拒绝非四个历史reserved槽的缺失
符号。R03仍PARTIAL：provider终止/断电、super::stop内部并发、timer/IRQ、事务预算/
代次及硬件验收未闭环，不把对象pin当设备静止或65秒当stop总上界。

最新.77补UserClient局部存活：start持有provider至free，stop只关闭client准入、不清空
在途指针；externalMethod在provider访问前查门。生命周期22检查、selector16场景48检查
0失败，重放.76分别6/4失败；`verification-batch31.json`26组检查及构建通过。R03仍
PARTIAL，剩余整体停止/框架/电源/预算缺口见安全审计，不能把client门当硬件静止。

最新.79补reset专用保留预检：46场景292检查0失败，最终测试重放.77为177失败；
.78锁外读取attach iface被6项反例否定，已取代、不部署。`verification-batch33.json`
27组检查及构建退出0；49个源码/测试文件及两个二进制哈希逐项复核一致。完整库存、
容量/重复owner检查与reset尝试后拒绝再次reset已实现，但不是全局reservation或完整
poison代次状态机。下一就绪切片先列跨selector/FLR状态与允许命令，再用生产执行
反例锁定；ICM/FW/mailbox统一预算及完整停止协调继续开放。未上机，R03仍PARTIAL、P0/P1未通过。

.80进一步把FLR接入相同预检并拒绝IRQ存活，FLR写入前设置sticky标记，不再以手写
FLR/sleep清mailbox poison。58场景380检查0失败，旧.79 FLR重放40失败；27组离线
检查及构建通过（`verification-batch34.json`），源码/测试/二进制哈希复核一致。
初次FLR仍是旧手写路径，平台事务/凭据、全selector准入和全局预算未完成；本批未上机。

.81补真实mailbox poison不得由fault idle读数或HCA reset清除；显式synthetic注入
才可在posted/retained均无时撤销，真实失败覆盖synthetic资格，marker opcode不可冒充。
生产mailbox/fault/reset方法45场景8485检查0失败，最终测试重放.80为57失败；27组
检查及构建通过（`verification-batch35.json`）。新增9个build辅助检查源文件入归档，
58条清单记录的live/归档字节及两个二进制复核一致。未上机，真实恢复仍未实现。
下一就绪离线切片可推进R04元数据/独立解码，R03/R06全局预算与恢复目标保持不变。

.82新增真实HCR路径的按调用命令观测（Entered/主机Posted/Replied、单调时间/fwStatus），
7场景86检查0失败，最终同测试重放.81为38失败；28组检查及构建退出0
（`verification-batch36.json`），59条清单记录live/归档字节及两个二进制复核一致。
未上机；QP/RSS生产调用者尚未接入，R04仍PARTIAL，下一切片接入真实QP与结果/工具。
Posted只表明主机GO store和屏障完成，Replied只表明走到正常completion/status-read路径。

.83补R04独立decoder完整邮箱格式：248/256/264/4096精确尺寸，分离前缀/硬件QPC/
padding或query余部，保留全字节及hash；全零不推断有效/固件成功。10项测试通过，
同测试重放.82为25项失败；28组及构建退出0（`verification-batch37.json`），60条
清单记录live/归档字节及两个二进制复核一致。本批未改驱动逻辑/ABI，未上机。
下一切片仍是QP生产观测输出→结果/工具的接入和版本门，运行/代次信息及真机语义未完成。

.84把真实命令观测接入QP方法、RX/RSS全部三步modify/query及最终query，保存64字节
按调用记录：调用/构造/快照长度、方法fwStatus、HCR路径和单调时间；不靠数组非零推断。
QP版本章1、RSS版本3，尺寸17912/18512；工具验证章后显示记录，现内核拒绝旧尺寸。
13545底层检查、107场景1206上层检查、36项client ABI及formatter/大结果传输通过。
同测试重放.83分别140、28、1项失败且formatter失败；28组及构建退出0
（`verification-batch38.json`），60条live/归档清单与二进制哈希一致。未上机。
R04仍PARTIAL：下一就绪项是boot/运行/代次身份、采集hash绑定及生产结果→独立decoder
连接，不把版本章、观测标志或单调时间当恢复凭据。R03/R06及P0/P1不关闭。

.85补保存的rawjson 34/35→独立decoder离线链：实际producer函数、真实C结果布局、
替代transport与独立consumer联合11项测试通过；重放.84 decoder为5项失败。
严格检查selector/call/尺寸/章/逐步状态/时间；失败及未调用query不解码，全零成功query
仍解析。附文件/结构/邮箱hash；身份明确未知。本批不改驱动逻辑/ABI，未上机。
29组及构建退出0（`verification-batch39.json`），62条live/归档清单与两个二进制
哈希一致。下一项是boot/运行/版本身份绑定及R06设备代次，不把artifact hash当凭据。
真机raw/对照/key A/B与RSS语义仍未完成，R04/P0/P1保持未通过。

现有旧报告中的“4项预期红”“采集尚未实现”等为带日期的历史记录；判断当前状态要合并后续段落。R04明确：部分字段已实现，完整采集契约未闭环。若平台路线被证伪，先更新决策和台账，再比较备选架构，保留原功能目标。
