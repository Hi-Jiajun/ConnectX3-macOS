# T02：资源生命周期审计与最小修复规格

## 2026-10-24 .127：停止边界与新实例启动准入

[验收](../../../../outputs/20261024-cx3-network-stop-contract/README.md)关闭两个独立start缺口：同一retained/open/已有generation实例不再进入superclass或覆盖PCI/HW状态；fresh实例open失败立即返回且不close旧client，不启用PCI、map/probe或发布worker/service。原RUN_FW sticky/poison/generation保留。实际start 12场景37检查0失败，同最终测试重放.126有22项行为失败，5种单独mutation均编译成功并被行为断言拒绝。stop组合96场景447检查覆盖1/2端口及0/末槽/全槽pending；51脚本、完整构建、85项冻结输入及GPT Luna/high复审通过。[完整资源矩阵](stop-reopen.md)区分实际方法与child/table/IOKit seams。默认START_LEVEL=2已覆盖；诊断level0重复publish仍为明确边界。真实跨实例provider仲裁、superclass stop/termination、PCIe/DMA静止、stop/reopen及预算/异常恢复仍未证明，R02/R03/R06/T05/P1未关闭。本批非14:42冻屏根因修复。

## 2026-10-24 .126：正常FW撤销与命令收尾

[验收](../../../../outputs/20261024-cx3-fw-normal-close/README.md)将FW正常UNMAP/free接在HCA/table关闭后、命令层/provider关闭前。当前代allocation与owner验证、closed/drained callback及无context/table/retention/AB的前置守卫；host-only无RUN_FW时可直接释放，handoff后仅bool与FW状态同时成功才释放精确FW slot。失败/未知保持原slot并pin self/provider，不关闭邮箱/BAR；RUN_FW sticky、poison、generation保持。90场景399检查、UNMAP_FA 20检查、原FW重用93场景753检查通过；50脚本/完整构建、84项冻结输入及GPT Luna/high独立复审通过。正常FW段的参考合同不推广为全DMA/旧代/reset凭据；真实stop/reopen、TX在途失败隔离/预算、provider/框架及异常恢复未验收，R02/R03/R06/T05/P1继续开放。

## 2026-10-24 .125：正常端口/HCA关闭事务

[验收](../../../../outputs/20261024-cx3-normal-close/README.md)补端口关闭、子对象交还、CLOSE_HCA和既有table/AUX UNMAP的结果检查顺序；失败/未知停止下游清理并保留owner及self/provider。仅成功HCA close清initialized，映射知识保留到逐项UNMAP；initialized HCA拒绝ICM释放。port1 INIT前清旧关闭标记，port2不影响port1。最终stop 61场景319检查、ICM 43场景360检查及CLOSE_HCA 35检查通过；49脚本/完整构建、83项冻结输入通过，GPT Luna/high复审覆盖最终INIT增量，无阻塞项。仅离线，FW继续保留，不证明通用DMA静止或完整stop/reopen；R02/R03/R06/T05/P1仍开放。

## 2026-10-24 .124：family enable/disable参加停止排空

[验收](../../../../outputs/20261024-cx3-network-lifecycle/README.md)关闭family启停回调未被既有TX/timer CAS gate计数的局部缺口：enable拒绝reset-blocked/stop-retained/closed gate，已入场回调观察到stop后取消timer；disable在closed gate上幂等成功且不碰正在退休依赖。16场景471检查含100轮同owner down/up，最终测试重放.123有37项行为断言失败；48脚本、完整构建、82项冻结输入及GPT Luna/high限定复审通过。全为host/synthetic，未访问真实设备。门禁是CPU admission/drain，非互斥锁或DMA凭据；当前reset仍拒绝attached interface/timer，未放宽该前置。正常驱动完整卸载缺CLOSE_HCA及后续逐项撤销事务，见[源码对照](../../../../outputs/20261024-cx3-network-lifecycle/normal-close-source.md)；R02/R03/R06/T05/P1保持开放。

## 2026-10-24 .122：FW重用失败与相邻诊断释放入口

[验收及边界](../../../../outputs/20261024-cx3-fw-replacement/README.md)关闭下方.121记录的立即缺陷：failed/unknown UNMAP后保留精确旧FW slot/data、立即返回；icmalloc限制为有效从未handoff的host-only分配，不再从另一个selector释放同一设备owner。共享准入在BAR/分配/释放前拒绝活动/未决依赖，锁内快照attach状态；RUN_FW尝试sticky阻止live/不确定固件被重载，force仅绕过页数。CLI拒绝短scalar并传播序列错误。93场景753检查0失败，旧.121同测试626项行为断言失败；42脚本/完整构建/74项冻结输入核验通过，GPT Luna/high生产复审无具体阻断、首启覆盖建议已补测试。仅离线，没有设备操作。pre-RUN_FW限定UNMAP不是通用DMA flush；正常stop/reopen、RUN_FW后恢复、真实provider/框架、全局预算及正常/异常释放语义仍开放。

## 2026-10-24 .121：成功retention后的本地账本存活

[完成包](../../../../outputs/20261024-cx3-retained-ledger/README.md)补齐.120未覆盖的成功转移路径：旧slot清空后，quarantine/overflow/ICM-kept或mailbox owner仍嵌在controller中；先前stop可能继续shutdown/close并最终销毁容器，留下不可寻址的故意泄漏。新增`hasStopRetentionOwners()`和阶段间检查，保留self/provider以保持owner可寻址；包括完整初始化slots、计数/legacy未决项和共享mailbox决策。健康mailbox仍可释放。stop生产组合41场景219检查0失败，旧.120同测试122项行为失败；mailbox生产链61场景8613检查0失败。全量41脚本、完整构建、73项输入前后/归档一致及GPT Luna/high mailbox/stop范围复审通过；kext SHA-256 `94393beb1ae97b2ca2f19efc912c684e1b2e4467fc58cb0c7da9df20b506ad6c`。

局部owner存活不保证provider继续供电或IOMMU有效；不代表正常stop/reopen完成，也不制造释放凭据。实际provider终止、框架生命周期、预算及DMA静止仍开放。只读OS清单确认本boot加载的是`.47`，没有安装候选或触发设备selector。

**新定位的立即缺陷**：`exportFwLoad()`在旧FW的UNMAP_FA失败后仍直接free/重建；[.121源码证据及下一测试](../../../../outputs/20261024-cx3-retained-ledger/fw-replacement-finding.md)。该调用者不属于本次stop修复，尚未关闭，不能据本批绿色声称全release-site安全。下方.120“下一缺口”的本地owner部分已由本节推进，实际框架/provider/DMA义务保持不变。

## 2026-10-24 .120：停止失败组合与owner初始化

最新有界离线完成包见[.120验收](../../../../outputs/20261024-cx3-stop-composition/README.md)。`stop()`消费子context残留、table/AUX原槽残留和FW转移失败，在table/FW/BAR/provider依赖清理前pin self/provider并sticky返回；`netServiceStop()`将frame-ring转移拒绝向上传播，unlock但不丢ring/slot/lock。新增全槽扫描由完整`fTables`初始化支撑；context谓词纳入raw-frame slot。正常成功控制仍能shutdown/close。

真实stop/EQ/net-stop/timer合成19场景92检查0失败，最终同测试对封存.119重放80项行为失败；真实TX teardown/retention/predicate链37场景164检查、ICM/profile链42场景357检查通过。注意rawframe-only拒绝在reset-time部分转移输入下成立，普通`txTeardown(false)`不因retention容量拒绝，不能冒充普通stop复现或历史冻屏根因。完整41个Python脚本/完整构建exit0，73项输入前后/tar一致，GPT Luna/high复审完成；没有硬件操作。kext SHA-256 `98710f14a97b1ccdf7274febc089fe2e25f502d29980c43c1e24be6abf12043f`。

[CPU/doorbell有限审计](../../../../outputs/20261024-cx3-stop-composition/doorbell-audit.md)未发现当前受门禁调用者的具体可达post-stop BAR2写入路径；timer组合测试覆盖已准入action与stop交错以及关闭后迟到action。仍依赖实际IOKit同步移除/接口终止契约，不能推广为全部回调或DMA静止证明。

**下一缺口**：本批关闭“转移拒绝、原槽仍在”后继续拆依赖；没有关闭“成功转入quarantine/ICM-kept/mailbox账本”后的stop/free/provider跨终止存活，也没有增加真实正常/异常release credential或条目/字节事务预算。不能把强引用当作阻止provider终止/断电的证明。R02/R03/R06与P1仍PARTIAL，原有更早章节为历史过程，冲突处以上述完成包和会话权威摘要为准。

## 2026-10-23 .90：公开平台FLR调用（不是恢复凭据）

### .91复位前底账增量

平台调用前要求vendor/device ID非0/非全F、BAR0配置有效且非未分配，PCI Command
可读且memory decode开启。失败返回NotReady，platformCalls=0且两个reset sticky latch
不置位。身份全F/0与BAR全F/0四个注入用例先红12项，修复后reset入口96场景552
检查0失败；verification-batch45.json通过。该底账只阻止对明显脱落/配置不完整设备
发起reset，不构成多function范围、设备静止或释放凭据。

手写bit+sleep替换为公开IOPCIDevice::reset(FunctionReset,None)。适配器空provider
为NotReady；平台错误原样返回，不读后续配置/MMIO，不重试。成功仅核对配置身份、
BAR两dword及Command，不读旧MMIO、不清poison/owner、不释放，两个准入latch保持。
legacy owner=ffffffff表示未读；工具正确显示并将非成功转为非零退出码。
入口92场景532检查0失败，封存.89重放50失败/exit1；适配器9场景62检查0失败，
五种变异均独立确认编译成功且执行断言失败；工具5项检查通过。
verification-batch44.json为PASS，32组及构建exit0，65条live/tar清单和两binary
逐项一致。未安装/设备调用/重启。代次/scope、TRPND、CPU/框架/电源排空及全局
reservation仍开放，平台返回与配置可读不授权生产释放。

## 2026-10-23 .89：reset/FLR后的设备selector准入

fResetCommandsBlocked为事务锁内的准入latch，独立于fResetUnproven（后者禁止重复
reset但首次成功也置位）。HCA reset尝试前阻断，只有预检通过且无HCA/ICM状态、
clear-int准备、mailbox分配/poison、全部小owner与所有表槽均空的首次成功reset，
才撤销此设备访问阻断，继续原初始化链。它只表示本驱动空实例启动，不证明未知
外部DMA已排空，也不允许回收旧owner。旧owner或失败reset保持阻断；手写FLR无
平台事务凭据，配置写前阻断且不自行撤销。预检/不支持拒绝前不置新latch。

externalMethod两种加锁路径在transaction锁内重查；阻断态仅允许netstat（已检查
实际函数只读主机计数/interface元数据），probe/peek/raw/force均无例外；原停止/
容量门仍优先。已入admission但等锁期间状态变化也拒绝。命令前sweep只读已持有
主机页，phase标记只发布IORegistry属性，不使用BAR。非selector框架回调和stop/
provider路径不因此获得静止保证，完整生命周期仍开放。

重复exportResetHca在MMIO前拒绝；reset失败不再读owner-after，以0xffffffff表示
不可用，结合非零status判断。没有版本/设备代次ABI变更，也没有真实恢复实现。

test-reset-preflight执行实际reset/preflight/fresh判断/FLR，81场景459检查0失败；
test-selector-retention执行实际dispatch与准入方法，全47 selector含锁等待注入，
157场景471检查0失败。两者HCA/IOKit为替代边界，不是真并发或真实固件行为。
最终同测试重放封存.88分别35/184失败、exit1，日志test-reset-admission-final-red-04488.log
和test-selector-reset-final-red-04488.log。未计入活动表槽仍有owner反例初版1失败
后通过，见test-reset-inactive-owner-red.log。verification-batch43.json的30组与
构建exit0；63条live/tar清单、两个binary一致，git diff --check通过。未操作硬件。
后续仍须代次/scope、平台reset与CPU回调排空、全局reservation和真实可重复重建；
本次拒绝不替代最终恢复，也不关闭R03/R06/P0/P1或历史冻屏归因。

## 2026-10-23 .88：命令锁缺失不再fail-open

根因：cmdBox、cmdBoxImm、cmdEvent各自懒分配fCmdLock，分配失败仍执行core；
两个并发首次调用还可能发布不同锁。init成功原本不保证命令锁存在。
修复：init在发布命令入口前分配host锁，失败不映射BAR，BAR0映射/地址失败回滚
锁/已得映射；三个包装方法缺锁返回false与NOT_POSTED，清空可选输出（观测全零
表示HCR核心未进入），不分配锁、不进入core、不读HCR。正常路径始终配对加解锁。
不改变外层mailbox事务锁，命令锁仍仅覆盖HCR事务，不能声称保护整个邮箱构造。

test-command-lock.py执行真实init/三个包装方法，只替代IOKit和HCR核心，13场景
104检查0失败；最终同测试重放封存.87为42失败/exit1，日志
test-command-lock-final2-red-04487.log。新增初始化回滚断言初版6失败后通过，日志
test-command-lock-init-cleanup-red.log。真实QP/HCR17场景186检查及类型负例继续通过。
verification-batch42.json：30组及构建exit0；63条live/tar清单及两个binary一致。
实际构建的boot-path检查PASS：无设备写、DMA分配或reset可达；允许host-only锁。
未安装/调用设备/重启；shutdown与在途命令并发、provider生命周期及等待上界仍开放。

### 跨selector准入：本次新增确定约束，尚未实现状态机

fResetUnproven在exportResetHca实际尝试前及FLR配置写前置true，即使第一次成功
reset也保持true。因此不能简单在externalMethod看到true就永久拒绝所有初始化。
它目前只限制再次reset，并不证明恢复、也不等同“已发生失败”的状态。
后续必须区分首次干净初始化、reset/FLR在途、结果失败/未知、旧owner保留及经凭据
验证的可重建代次；对每个状态列明允许的host-only诊断与可能读写设备的selector。
在同一事务锁内检查状态与dispatch，覆盖锁等待期间状态变化；raw/force不得绕过。
拒绝硬件命令不是完整恢复。只有本代/scope的静止凭据、CPU回调与旧owner依赖均
满足后才允许回收/重建；不能用成功bool、静默采样或单调时间冒充凭据。

## 2026-10-23 .86：修正观测接线，不是整体安全结论

.84/.85的qpModify/qpQuery调用漏显式bypassBusy=false，观测指针被转换为bool，
非空时意外旁路busy，观测输出未传入核心。旧测试替身签名也漏bool，造成假绿；
两候选不部署，撤销其真实QP观测链已连通结论。两版未上机，不能用此归因历史冻屏。
.86补false并纠正替身，新增真实QP/HCR执行集成17场景186检查0失败，封存.85
同测试42失败/exit1；生产声明直接取自头文件。29组及构建退出0，未触硬件。
编译器边界补测：pointer-bool警告会拒绝历史成员取地址写法，但普通指针变量负例
仍exit0，不能泛称覆盖所有误传。真实调用链回归有实证；命令锁懒分配/缺锁行为、跨selector准入、全局
reservation、停止/provider/电源/timer/IRQ协调及代次恢复仍开放，P0/P1不关闭。

.87补类型级防护：cmdBox第8参数为指针时选中deleted模板重载，历史写法及普通
指针变量两种负例均在不加Werror时被编译器拒绝；保护前类型测试exit1，新增后
17场景186检查继续通过。29组及构建exit0，62条live/tar清单及两个binary一致。
这不禁止调用者显式传true，不替代busy旁路授权审查、命令锁或跨selector安全协议。

2026-10-23，**隔离缺陷已有候选修复及离线反例；其余释放/停止失败缺口仍开放；历史冻屏未归因；P1-S未通过**。
审查对象为当前未提交候选源码；未反编译证明与已加载0.44.47逐字节相同，候选也**未安装、未上机**。

本日执行更新：R01对0.44.59完成证据归档；R02已移除reset后静默释放，由14个真实执行用例覆盖；R03已让stop在两个worker未静默时保留整笔事务，并让poison mailbox跨shutdown保留。R02/R03其余项仍开，详见[当前任务表](../CX3-PRO-BACKLOG.md)。离线行为测试先红后绿不等于硬件并发证明。

### 本轮复核：R03不能按“保留整笔事务”验收

以下是当前源码可定位的开放缺口，不是已证明的历史冻屏根因：

- **.73已补局部锁协议**：独立短准入锁关闭gate，不等待长transaction；排队selector在
  dispatch前重查Stopping/retention；init建锁、缺锁拒绝、free才释放。drain按monotonic
  tick差转ns计预算。65秒仍不是stop总时限或实时调度保证。
- stop早退仍调用`super::stop(provider)`，但没有为selector失败分支明确建立完整的self/provider/
  workloop存活协议；不能凭注释断言跨终止安全。
- **.66已补该局部缺陷**：mailbox以sticky-retained和poison阻断分配/复用/释放，不完整视图不再返回成功；新mapping校验CPU view、单段完整长度、页对齐及低4G范围。仍缺跨unload的provider依赖与正式回收凭据。
- **.66已补RX/ingress record及RQ局部缺陷**：已投递或前序accepted的transition失败、producer mismatch后RESET失败均登记durable owner，readiness保持false；阻断重建/enable/RQ释放。CQ/MR/ICM依赖、TX失败路径仍须生产执行测试，不能由此称整个依赖图已安全。
- 256项账本覆盖一次生命周期理论数量，不证明重复reset的容量/字节预算与拒绝策略。

因此R02/R03保持PARTIAL，禁止据此放行R07硬件实验。下一步优先建立实际停止/失败路径的执行测试。

### .73停止锁协议离线证据（2026-10-23）

`test-stop-locking.py`抽取生产transaction/admission/drain方法，用真实host线程与mutex；
IOKit锁/时钟为替代边界。覆盖长事务持锁时stop仍关准入/预算失败、已准入调用退出、
零预算、缺锁拒绝、非1:1时基及30ms超睡；16检查0失败。同测试重放.72为5检查失败、
退出1（外层`test-stop-locking-red-04472.log`），没有在真机制造卡死。

`test-selector-retention.py`执行真实externalMethod及准入方法，从8扩到14场景42检查，
新增排队时开始停止、缺锁和停止后拒绝；保留原容量/锁平衡/无派发断言。transaction
边界是recording替代，不是IOKit并发证明。无现有NoLock策略行；该分支改为拒绝，避免
未来引入未同步的retention读取。锁序transaction→短admission；后者内无固件等待/sleep。

完整23组离线检查及构建退出0，`verification-batch27.json`为PASS；kext SHA-256
`43c12c1351f74dc7d1a02f2dfc72256e5ed44af2c70ee33dcc73df88bb2befbc`。
部分锁分配失败依赖正常IOKit析构清理；内核低内存/析构/调度未实测。不完整stop的
self/provider/workloop/回调存活、设备静止及历史冻屏因果仍未证明。R03保持PARTIAL；
下一优先项是完整停止存活协议、事务前条目/字节预算，而非扩大硬件实验。

### .85保存结果离线消费（2026-10-23）

独立decoder已消费保存的rawjson 34/35，严格验证结构和逐步记录；未调用/失败query
不解释成全零成功，hash不冒充来源。实际producer/C结构/替代transport联合consumer
的11项测试通过；29组及构建退出0（`verification-batch39.json`），62条清单与两个
二进制hash复核一致。详见[RSS报告末尾](rss-qpc-layout.md)。仅外层工具改动，未上机；
不新增DMA静止、恢复或并发证明，R03/R06/P0/P1状态不变。

### .84 QP/RSS采集观测与基本版本门（2026-10-23，仅离线）

真实QP方法已返回按调用采集记录，RX/RSS setup与最终query全部接入结果，工具显示
各步构造/快照长度、方法状态和真实命令核心观测。QP章1/RSS章3，17912/18512字节。
详细语义、生产执行测试与.83反例重放见[RSS报告末尾](rss-qpc-layout.md)。
28组及构建退出0（`verification-batch38.json`），60条live/归档清单字节及两个
二进制哈希一致。观察记录不授权任何资源释放，owner/retention逻辑未因此放宽。
未上机；R03/R06全局reservation、代次/scope、raw入口、provider/电源/回调及恢复
仍开放。R04缺运行/boot/代次身份和完整消费闭环，P0/P1不关闭。

### .83独立decoder进展（2026-10-23，仅离线）

完整邮箱格式与10项测试/25项旧版失败反例见[RSS报告末尾](rss-qpc-layout.md)。
28组及构建退出0，`verification-batch37.json`；60条live/归档清单字节及两个二进制
哈希一致。这一批仅改外层decoder和测试，不改驱动逻辑/ABI，kext哈希变化包含构建tag；
不是新增安全保护、真实采集、DMA静止或恢复证明。未上机；开放安全项不变。

### .82真实命令路径观测基础（2026-10-23，仅离线）

cmdBox/cmdBoxLocked增加可选的40字节按调用观测，由栈上RAII scope记录进入核心、
主机GO store及屏障完成、正常completion/status-read路径、fwStatus及单调ns。
提前拒绝清理旧输出；不给status指针也以局部shadow保存状态；不使用共享last-command缓存。
Posted不证明PCIe写抵达/设备接受，Replied不证明QPC正确；start不含命令锁等待。
无消费者时flags=0不作为成功；QP/RSS尚未接入，不能当R04合同完成。
`test-command-observation.py`执行实际命令核心/poison，只替代HCR/锁/时钟：
7场景86检查0失败；最终同测试重放.81为38失败，日志
`test-command-observation-final-red-04481.log`，基线哈希匹配.81 source-manifest。
28组及构建退出0（`verification-batch36.json`），59条live/归档清单字节及两个二进制
哈希无差异。未安装/操作设备/重启；event路径、命令锁懒分配/fail-open及全局并发
不由本批证明。R03/R04/R06、P0/P1保持未完成。

### .81 mailbox真实失败与合成注入来源（2026-10-23，仅离线）

.80的FLR已不清mailbox poison，但两条共享绕行仍存在：`fault unwedge`只看HCR
idle就调用无条件clear；`Mlx4Hw::resetHca`成功后也清poison。idle/配置恢复均不证明
旧命令没有迟到读写；因此会重新开放`mboxAlloc`及命令复用，shutdown也可能错误释放。

.81增加构造时显式初始化的`fCmdSyntheticPoison`，仅`cmdPoisonInjected`能赋true；
真实`cmdPoison`始终清synthetic资格，并在从synthetic升级时记录真实op/posted/时间。
`cmdClearWedged`只撤销synthetic且非posted、非retained状态；真实opcode即使等于
marker也拒绝。fault处理器保留确认/idle拒绝，并根据清除后仍wedged输出NotPermitted。
HCA reset不再调用clear；干净启动不增加poison，合成注入仍可正常撤销。

扩展`test-mailbox-lifetime.py`，执行实际allocation/free/shutdown、QP方法、
cmdPoison/injected/clear、exportFault和完整resetHca。PCI/IOKit/HCR/时钟为host替代，
没有设备访问。45场景8485检查0失败（大部分为既有逐字节比较），包括：真实post前/后
失败、marker碰撞、对真实poison再注入、retained/posted拒绝、synthetic转真实、
正常合成撤销，以及clean/synthetic/真实post前后×reset成功/信号量失败/设备未回来。
实际reset返回成功/失败均不改变原poison和邮箱字节；这不是硬件reset/DMA证明。
最终同测试重放.80两份源码为57失败、exit1，见外层
`test-mailbox-poison-final-red-04480.log`及两个`*-poison-red-baseline-0.44.80.cpp`。

`verification-batch35.json`：27组检查及构建退出0，58条源码/测试清单记录、归档
对应字节及两个二进制哈希无差异。本批补入build.sh实际调用、旧manifest遗漏的9个
辅助检查源文件；旧49条清单的无差异结论只针对已登记子集，不声称旧快照输入齐全。
旧报告保持原样。未安装/操作设备/重启。

这只是保护未知owner、不再假装恢复。真正代次/scope凭据、统一状态/预算、CPU回调/
provider/电源与硬件恢复仍开放；也不阻断任意raw poke/HCR等其它破坏性入口。
synthetic资格不是全设备静止凭据。R02/R03/R06和P0/P1不因本批关闭。

### .80 FLR共享局部准入与poison保留（2026-10-23，仅离线）

复核.79发现selector7的`exportFlr`在reset标记未知、网络/worker活跃或已有保留时
仍写PCI配置并调用`cmdClearWedged`；`fIcmHcaHolds`也随之清除。独立生产入口fixture
5场景17检查有16失败；随后扩展正式reset测试，执行完整reset/预检/FLR生产方法，
58场景380检查有40失败。失败是写入/poison清除/状态改变/允许重试的实际边界行为，
不是仅检查源码字符串。它不证明历史事故由FLR引起。

.80在FLR配置写入前拒绝IRQ存活或reset预检失败；写入前设置同一个sticky标记，
FLR后FLR/reset与先前reset后FLR均不能再试。未尝试/不支持路径不置新标记，
不改ICM owner状态；沿用输出NoSpace、transport success。移除无凭据的mailbox
poison清除。最终58场景380检查0失败，同测试以`--flr-source`重放.79为40失败，
exit1，日志`test-reset-flr-baseline-replay-04479.log`；旧源码备份
`flr-red-baseline-0.44.79.inc`哈希与.79 source-manifest一致。

完整回归`verification-batch34.json`：27组检查及构建退出0；49个源码/测试文件及
两个二进制哈希无差异。未安装/设备调用/重启。平台边界仍为fake config/MMIO/sleep。
初次FLR保留旧手写bit路径，未实现TRPND/平台reset、ready/scope/generation凭据；
status=0只沿用旧诊断输出，不等于安全恢复。其它selector/raw poke/HCR、CPU回调、
电源及全局reservation仍可能绕过局部状态约束，不能扩大本批证明。R03/R06保持PARTIAL。

### .79 reset专用保留预检（2026-10-23，仅离线）

`exportResetHca`在调用reset与任何teardown之前执行`resetRetentionPreflight`。
已有保留/溢出/sticky耗尽、网络timer/iface/TX ring/enabled、attach in-flight/poison/
orphan iface或attach call缺锁均拒绝。attach状态在其锁内观察，之后才检查网络字段。
完整库存为13类小块、全部18×8个ICM chunk和aux；拒绝重复owner、越界表数，
小块还要求CPU view及至少4字节，按`bytes > limit - accumulated`验证64MiB余量。
这不对ICM总字节建立预算，也不是给其它分配入口预留容量。

预检失败沿用输出status=NoSpace、transport success，不reset、不teardown、不清
owner/readiness，也不设置reset标记。真正调用bool reset API之前设置sticky
`fResetUnproven`；不论结果成功或失败，拒绝再次reset。bool失败无法证明命令没有posted，
成功也没有代次/scope静止凭据；该标记目前仅限此入口，不是全部selector的poison门。
正常reset后其它初始化仍能执行，不能据此声称重建或总保留量已全局有界。

`test-reset-preflight.py`执行完整生产入口与预检，HCA/teardown/IOKit为替代边界：
46场景292检查0失败；同一最终测试重放.77为177检查失败，exit1，日志
`test-reset-preflight-final-v2-red-04477.log`。.78锁外读attach iface，合成锁获取hook
反例有6失败（`test-reset-attach-lock-red-04478.log`），已取代、不部署。.79在锁内观察
同步后状态；hook不是内核并发模拟。

`verification-batch33.json`记录27组离线检查及构建退出0。49个源码/测试清单文件与
两个候选二进制逐项哈希无差异；身份见会话目标。本轮未安装、未调用硬件、未重启。
下一切片为跨selector/FLR状态准入与代次，以及事务前全局条目/字节reservation；
super::stop/provider终止/电源/timer/IRQ仍开放。R03/P1-S不关闭，DMA静止、RSS异常
和历史冻屏因果均未证明。

### .77 UserClient provider局部存活协议（2026-10-23）

旧client stop直接清fProvider，而一次externalMethod/handler/结束标记会反复读取它；
退出guard捕获的旧指针也没有client自有retain。不能假定框架始终序列化这些自定义读取。
现init建立短client锁，失败拒绝；start retain provider并在锁下发布；成功后指针不可变，
stop只在锁下关闭client准入，最终free才release并清指针/释放锁。externalMethod首行
检查client门，不再让已停止client进入无provider的super派发回退。既有provider selector
gate/事务锁/Stopping复查仍保留；client门持有期间不获取provider锁或等待固件。

`test-userclient-lifetime.py`执行真实init/start/stop/free/clientReady/doProbe：22检查
0失败，包括旧准入handler在stop后继续使用同一provider、最终free归还引用、重复stop、
错provider、super-start失败、非root及锁分配失败。旧源码无新增方法时以其原有继承/空门
行为表示，不替换实际旧start/stop/handler；同测试重放.76为6失败。
`test-selector-retention.py`新增真实client stop/clientReady路径，16场景48检查0失败；
重放.76 client源码为4失败，停止后不能新增super派发。对应red日志位于外层
`test-userclient-lifetime-final-red-04476.log`、`test-client-stop-dispatch-red-04476.log`。

`verification-batch31.json`：26组离线检查及构建退出0；kext SHA-256
`803d96c9dd28049f182e8fa0eef14bc0e8ead5d307a8e69a28bbceb5dac6a0dd`。未安装/设备调用/重启。
以上是host锁/引用替代边界的执行测试，不是实际IOKit并发/析构证明。依赖标准OSObject
最终free发生在调用者引用消失之后；IOUserClient superclass stop与在途dispatch、父设备
终止/电源、timer/IRQ排空仍需整体协议。不得把稳定对象指针等同有效MMIO/DMA或P1-S通过。

### .76失败对象引用与worker本地完成栅栏（2026-10-23）

已修三条实际停止路径：selector未排空时不取消/释放可能仍被其使用的thread-call；
任一worker未排空时在接口detach/worker引用结算之前退出，并显式retain self及实际PCI
provider（无PCI指针时保留stop参数provider）。失败态sticky，不重复pin、不重试清理。
这些引用保留至本次启动结束，没有凭worker晚归还自动释放或假装在线恢复。

原`thread_call_free`成功不等于回调结束。先有两次反证：.74用旧fake将free与静默等同；
.75改用`thread_call_isactive`，但kmutil显示该API不可绑定。旧build.sh在任意符号缺失
时只要`Dependencies: OK`就通过，产生假绿。两版均不推荐/不部署。新checker仅豁免
四个历史IONetworkController reserved槽；实际缺失`_thread_call_isactive`的完整构建
重放退出1（`symbol-guard-red-build.log`），不能用内核nm看到实现代替AuxKC可导入。

.76不新增该API：selector生产者排空后cancel；成功cancel撤销唯一pending调用，否则
在attach锁下读取InFlight完成栅栏。worker先完成末次property/接口/workloop访问，再
在同锁下发布InFlight=false/Done并最终unlock；其后只有self->release，无本地资源访问。
callback不自重排，已排空selector不能新增调用。这是本地依赖访问栅栏，不是“callback
机器指令已经返回”或“网络框架/DMA已经静止”。普通thread_call_free允许活动回调持有
自身内部引用，不能反过来以free成功作为栅栏。

主来源：本机Kernel.framework `kern/thread_call.h` cancel/free契约，以及Apple XNU
提交`f6217f891ac0bb64f3d375211650a4c1ff8ca1ea`的
[thread_call.c](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/osfmk/kern/thread_call.c)
（free检查queue/reschedule而非普通running、isactive看submit/finish计数）。上游源码
用于说明语义，不宣称与本机内核逐字节一致；当前AuxKC导入以kmutil为准。

新增`test-stop-retention.py`执行完整生产stop至完整拆卸前的边界，并抽取真实worker
完成后缀；仅完整stop后半段替换为marker。7场景86检查0失败；最终同测试重放.73为
50失败（`test-stop-retention-86-red-04473.log`）。覆盖selector/worker失败、canceled、
free拒绝、重复stop、provider fallback、worker尾部引用归还与完成栅栏；不以fake证明
真实IOKit终止。`test-unresolved-symbols.py`运行实际checker，7场景通过。

`verification-batch30.json`：25组检查及构建退出0；kext SHA-256
`59f952d5a3f256591369fe1a11335b587d0e2ec43d0e3fa5409c1f98f815ae95`。未安装/设备调用/重启。
R03仍PARTIAL：pin不阻止provider终止/断电，super::stop内部并发、timer/IRQ与全部依赖
尚未证明；UserClient stop清空raw provider与在途dispatch的关系仍待核查。预算/代次及
静止凭据仍缺，历史冻屏/RSS异常仍未解释。

### .66生产路径故障注入（2026-10-23）

- `tools/test-mailbox-lifetime.py`执行生产mboxAlloc/mboxFree/shutdown及QP mailbox调用：20场景、8290检查、0失败；大部分为逐字节无覆写检查，不是8290独立场景。
- `tools/test-qp-setup-lifetime.py`执行完整RX/RSS setup、teardown、RQ teardown和enable：42场景、527检查、0失败；context构造/最终字段比较和硬件依赖被替代，不证明实际DMA或完整依赖图。
- 当前测试指向归档.64源码重放：mailbox 8239检查失败，QP 227检查失败（两者退出1）。见外层`r03-mailbox-red-replay-04464.log`、`r02-qp-red-replay-04464.log`；当前完整回归/构建为`verification-batch20.json`。
- .65一度将retained owner写成Up=true，可能放行只检查Up的数据面；复核后由.66分离owner与readiness并补反例。.65不部署，归档保留。

### .67–.68依赖保护（2026-10-23，最新离线候选.68）

确认的局部缺陷：QP归还失败后，上层仍继续调用CQ/MR撤销及ICM UNMAP；profile会在子资源未成功撤销时继续makeProfile并覆写旧table指针。这些源码缺陷尚未关联到历史冻屏的某一次事件。

- .67在RX QP的Up或record仍存在时不发送HW2SW_CQ；任一RX/RSS/TX QP owner仍存在时不发送共享MR的HW2SW_MPT。保留原状态和依赖，不把拒绝冒充归还成功。生产执行反例为49场景610检查、83失败（退出1），修复后0失败，见`r02-qp-dependency-red.log`。
- .68新增`hasContextOwners()`，枚举QP/CQ/RQ/TX/EQ的flag及分配指针、MR已拥有/未知状态。`freeIcmTables()`遇到这些owner不发任何UNMAP，不free，不转移/清table或AUX指针及mapping标志。它是保守的依赖保护，不是全设备静止凭据，也未证明全部未来资源已穷尽。
- `exportM2dProfile()`在HCA已初始化、网络enabled或context owner仍存时，在修改profile、读BAR/发命令、尝试teardown之前返回NotReady，scalar与struct一致。**当前不支持live profile重配**，不能把旧的“按顺序调用teardown”当作事务完成。空状态的初次profile仍正常运行。
- ICM/profile反例执行完整生产方法（profile内不应到达的硬件及teardown为替身），36场景308检查、142失败，退出1；当前加入enabled网络控制后37场景310检查0失败。见`r02-icm-child-owner-red.log`，旧14个table/AUX正向及失败用例仍保留。
- 最新QP/CQ/MR测试52场景617检查0失败，包含CQ/MR自身hand-back超时及成功重试；mailbox仍20场景8290检查0失败。完整19组检查与构建退出0，见外层`verification-batch22.json`。kext SHA-256为`c9cda42571c50007753a0272b2df8adcb850c14dc97321d304645ff63b3956ee`。源码/测试运行中无变化；未安装、未调用设备、未重启。

**.68时仍OPEN（.69更新见下一节）**：releaseIcmOrKeep拒绝后调用者清flag/回收MTT；TX setup以Up记录attempted owner，须补实际data-plane可达性测试；query失败/poison后producer发布；reset路径代次/scope、CPU/posted-write静止、retained对象/provider跨终止存活与字节/重复次数预算。stop最终仍会shutdown/close provider，表保留不解决对象存活。

### .69保留失败与重复撤销（2026-10-23，当前离线候选）

本批不是增加一层纯bool模型，而是执行生产保留/撤销方法、完整UserClient分发入口及ICM/profile方法。确认并修复以下局部缺陷：

1. `quarantineTake`原来在zone+overflow全满时仍返回true，调用者清指针，只留下untracked计数；无CPU view或bytes<4时还会直接free。现在两种情况均拒绝转移，原调用者保有指针，不写payload、不free；CPU不可读不等于设备不可达。
2. RQ/QP/ingress/TX SQ和CQ/RX CQ/EQ的释放调用者检查拒绝结果，停止后续清flag/地址/数量和MTT回收。组内第一块已转移、第二块失败时，第一块仍在ledger，剩余块仍在原slot；重复调用不丢owner。它不是整组原子回滚，耗尽后的工作准入必须同时关闭。
3. zone进入overflow、无效CPU view或ICM账本转移失败时，`fQuarExhausted`成为sticky失败状态，同时fNetEnabled=false。selectorEnter拒绝新工作，externalMethod在实际dispatch锁内再查，防止“耗尽前已入场、耗尽后才拿锁”的调用继续执行；enable也拒绝。当前所有selector（包括诊断selector）都被拒绝，不声称可继续在线恢复；日志/外部观察仍可用，恢复须受控重启。**不因此授权unload。**
4. `keepIcmForQuiesce`改为返回是否接管；256项满时不再新增unaddressable计数，而是保留原table/AUX槽和mapping标志。已转移页数在中途退出时仍计入fLostIcmPages。profile既检查入口的耗尽状态，也检查拆旧表期间新发生的耗尽，不继续makeProfile覆写剩余owner。
5. EQ HW2SW失败原来清掉EQ编号但留下ring；第二次teardown会走“无编号”分支直接free。现在失败保留EQ编号/长度/MTT/doorbell/consumer元数据；重复失败仍须发送HW2SW，只有确认归还的正向控制才能free。此反例不证明历史某次冻屏由它造成。

最终离线证据：

| 生产路径测试 | 当前结果 | 当前测试重放归档.68源码 |
| --- | --- | --- |
| test-retention-teardown | 24场景89检查，0失败 | 48检查失败、退出1 |
| test-selector-retention | 8场景24检查，0失败 | 8检查失败、退出1 |
| test-icm-release（含完整profile） | 40场景329检查，0失败 | 16检查失败、退出1 |
| test-qp-setup-lifetime（含enable耗尽反例） | 53场景618检查，0失败 | 分阶段red日志记录enable反例1失败 |

重放日志为外层`test-retention-teardown-red-replay-04468.log`、`test-selector-retention-red-replay-04468.log`、`test-icm-release-red-replay-04468.log`。阶段red日志保留，数量对应当时测试集，不与最终场景数混用。完整21组离线检查和构建退出0：`verification-batch23.json`，源码/测试运行中无变化。kext SHA-256 `d5276c47b81d9d16865bc6b70afb685d65c12f3e4b65e25ca750f806ba8d56f6`。未安装、未调用设备、未重启。

**当前仍OPEN**：保留成功与reset后的代次/scope和MTT复用规则；事务开始前条目/字节预留、未overflow前的重复reset限制；真实线程/锁/gate与posted-write排空；stop锁上界、锁懒分配/fail-open、self/provider/workloop跨终止存活。测试替代IOKit/HCR/allocator及哈希，selector测试只在锁边界注入状态，不是实际线程并发证明。R02/R03/P1-S仍PARTIAL，R07仍不放行。

### 后续执行：0.44.70 TX owner/readiness

`exportR4Tx`原来在transition前Up=true、失败时free本地record，只有成功末尾才保存record指针；每步query失败或错state/QPN仍继续，最终比较失败仍允许TX数据面。现在首个可能交给设备的transition前登记record owner，Up保持false；每步query匹配state与QPN才能继续，最终query/八字段/MTT匹配才能ready。retry及enable拒绝未ready的TX owner；`txTeardown`先清readiness，即使Up=false仍对record owner归还QP，2RST失败保留所有依赖，成功正常释放。实际`outputPacket/netServiceTx/exportR4Raw`仍消费Up门禁，本测试未执行这些方法，不将源码消费点冒充数据面运行证明。

新增完整生产export/teardown/enable、生产context builder和最终query比较的执行测试：35场景302检查0失败；同一最终测试重放.69归档源为170检查失败、退出1（外层`test-tx-setup-red-replay-04469.log`）。3步×6transition失败、每步query超时/错state/错QPN、最终query失败和MTT/CQ不符、正常setup及重复/失败teardown均覆盖。分配器和HCA传输为fake，成功query回显context，只验证控制流与owner/readiness，不证明固件字段语义、并发、DMA排空或历史因果。

完整离线回归`verification-batch24.json`为PASS：22组检查及构建退出0；源码/测试运行中无变化。kext SHA-256 `86fee8d59b7374ae3fe9b035a6463cdd8e98d07460d99d56797966b0d28fd049`。未安装/调用硬件/重启；RX/RSS同类query缺口仍在，R02/R03/P0/P1不关闭。

### 后续执行：0.44.71 RX/RSS query与producer门禁

RX/RSS首个transition前登记record owner并保持Up=false；逐步query必须同时匹配state和QPN，最终query/字段/MTT均通过才发布producer。RSS还核对overlay/目标QP，final comparator新增RSS启用位和log_page_size，不能只看overlay与MTT基址。明确未posted的首步失败及已成功2RST的producer回滚正常free并清owner slot；其余失败保留record。逐步/最终query失败不继续发命令，不用自动reset擦掉现场。复查已ready对象遇到query/producer矛盾时撤销readiness并清fNetEnabled；不把已发布record写0冒充撤销。transition及producer失败也停止网络提交；retry和enable改看record owner与Up，不以最后写入state代替readiness。

QP链测试升级为真实context/RSS builder及真实final比较器（原来这两个边界被替代）；103场景1146检查0失败。同一最终测试重放.70归档为387检查失败、退出1（`test-rx-rss-query-red-replay-04470.log`）。覆盖query超时/错state/QPN、MTT/CQ/几何/RSS key/启用位/页大小不符、复查query/producer失败，以及正常首次/重复测量。HCA传输、allocator/IOKit仍是fake，成功query回显context；不证明真实固件布局、并发、数据面运行或历史原因。真实`netTick`依赖fNetEnabled停新工作，但本测试未执行该方法或注入并发。

完整回归`verification-batch25.json`为PASS：22组检查及构建退出0，源码/测试运行中无变化；kext SHA-256 `22c8925cc57f3184b9e11d390ea9738dfc1d8446be0a23ecbf0985d227fa96d6`。未安装/调用硬件/重启。尚未处理RX/RSS已ready后2RST失败仍保留Up的撤销路径；stop存活/预算/代次等仍开放，P0/P1未通过。

### 后续执行：0.44.72 RX/RSS撤销readiness

`qpTeardown/ingressTeardown`原来在2RST或retention失败时留下Up=true。.72开始撤销就清网络提交/Up，确认state置未知；record owner/地址/producer及MAC元数据继续按原条件保留。重复撤销仍检查record owner，不能因Up=false跳过归还。该state是主机“不再确认”的标志，不是设备返回的state。没有向已发布record写0。

完整生产setup/teardown/enable测试增加2个ready→归还失败→重复失败→最终归还场景；保留测试纠正旧的Up==full期待为!Up，同时保留原owner/地址/producer/MAC/MTT检查。QP链105场景1166检查、retention24场景89检查0失败，同一最终测试重放.71分别10/4检查失败、退出1（`test-qp-teardown-red-replay-04471.log`、`test-retention-ready-red-replay-04471.log`）。HCA、IOKit及allocator仍为fake，不是实际并发或DMA证明。

完整回归`verification-batch26.json`为PASS：22组检查及构建退出0，源码/测试运行中无变化；kext SHA-256 `b4efd253ff46d6dd8419b50037203033b5f955f1cc940f9c8ff2dccc879c7d1a`。未安装/操作硬件/重启。下一优先项仍为停止/锁协议：beginStopAndDrain先阻塞取事务锁，selectorActive也会阻塞；transaction锁懒分配且失败有fail-open，不能仅改sleep次数便宣称有界或并发安全。stop早退的super/provider/self/workloop存活亦未解决。

## 修复前后：源码级安全要求

外层`outputs/20261022-cx3-fulladapt-batch1/audit-quarantine.py`从当前生产源码抽取
`releaseIcmOrKeep`、`quarantineTake`、`freezeInstrumentsSweep`、`quarantineDrain` 的完整方法，
用C++假分配器执行。这不是DMA仿真，更不证明历史冻屏的因果。

旧版本该脚本退出1、5项中4项不满足；2026-10-23 修复后为 **9项全部满足、`--enforce` 退出0**。

| 情形 | 修复前 | 修复后（当前源码） |
| --- | --- | --- |
| 未知owner、空间足 | 不free，但覆写0xdeadbeef | **只读**取基线摘要，不写入 |
| 区满、无“安静”候选 | free第65块 | 不free；进overflow账本，仍可计数 |
| 区满且有sweeps≥64/hits0 | free旧块换新块 | 不淘汰任何块；静默采样不作为凭据 |
| 无设备静止凭据drain | 直接free | 不释放，记录保留量，等重启回收 |
| 有显式静止凭据drain | 无此参数 | 释放zone+overflow，计数1 |
| deviceMayStillHold=false控制组 | free一次 | free一次（不变） |
| 扫描是否改动内存 | 每轮覆写 | 只读；用例证明字节不变 |
| 外部写入能否发现 | 能（靠pattern） | 能（摘要对比，用例证明hits=1） |
| 300轮静默扫描 | 不适用 | hits=0，不误报 |

`cx3quarfree` 不再能绕过释放判定：`src/Cx3Debug.h` 保留解析以避免启动参数变未知项，但没有任何代码读它的值来做释放决定。

## 修复的具体语义与代价

- `freezeInstrumentsSweep` 改为比较“取回时的只读摘要（checksum + 非零字节数）”，命中只说明**内容变了**，不再声称是谁写的、也不声称是DMA；日志明确写了这一点。
- `quarantineTake` 满区时把块放进 `fQuarOverflow[]`（32项、永不扫描）。早期再满会只记untracked计数；**.69已改为拒绝转移并保留原slot，关闭新工作**，不能沿用“计数即owner”的旧结论。
- `quarantineDrain(why, Cx3QuiesceProof)`：`stop()` 传`kCx3QuiesceStopping`，因此保留区最多64 MiB+overflow会**故意保留到重启**。枚举替换了早期bool，但仅保护该drain入口；整个卸载路径是否安全还取决于mailbox、CPU回调和保留对象依赖。P1-S需要可证明的静止/退出路径，不能只证明该方法不free。
- 已验证：`audit-quarantine.py --enforce` 9/9、退出0；`build.sh` BUILD RESULT PASS（0.44.49-local，binary sha256 `491e4b6d342dbcdcaa2c3153842d3c02152fb201c773241ea456cdb3896f0161`）；4+7+4+9 项既有离线测试、11 dispatch、137 pure-logic 检查全通过。

## 资源owner与撤销表（首轮，非穷尽证明）

| 资源/异步owner | 当前撤销入口/已见保护 | 未完成的证明或缺口 |
| --- | --- | --- |
| IRQ event source/回调、EQ ring | irqReleaseObjects先disable/remove/release；teardownEq有HW2SW失败保留 | 仍须证明在途回调退出、与stop/事务锁不会倒序 |
| RX/Ingress/TX QP→RQ/MTT/doorbell | qp/ingress/txTeardown走2RST；未知record独立登记；.70/.71补query门禁，.72补RX/RSS撤销先清ready、失败保留owner、重复不能跳过归还 | 局部MTT拒绝保护已有，完整依赖、代次和并发未证明 |
| CQ / MPT / EQ | .67起RX QP owner阻止CQ撤销，任一QP owner阻止共享MR撤销；各HW2SW失败保留 | reset分支和完整并发组合仍未证明，不能将局部执行测试称全图安全 |
| ICM表/AUX | 解映射失败或reset后`!asked`都进`fIcmKept[]`，不再由静默读数free；账本增至256，覆盖理论最大241块 | reset后的块现在会保留到重启；普通成功路径由执行测试正向覆盖。仍缺字节预算、重复reset/代次及真机长时失败 |
| FW ICM / mailbox | FW区走保留门；0.44.61在命令poison时让shutdown保留mailbox的IODMACommand/内存描述符，否则正常释放 | 分支6项检查通过；真实对象保留、跨unload存活及timeout后设备是否仍写mailbox未上机。FW保留不证明mailbox安全 |
| RX timer / TX frame ring / lock | stop先关CAS准入门，再由独立worker同步terminate接口，最多等待2秒；排空成功后才移除timer并持`fNetTxLock`处理mbuf/释放ring；超时则pin self/provider并在资源teardown前返回 | SDK同步terminate说明接口数据链已移除引用，但真实IOKit worker路径尚未上机；即使family callback安全退出，仍不证明timer/workloop、IRQ或DMA静止 |
| outputPacket / runtime UAR send doorbell | `outputPacket`先做CAS准入，RAII覆盖出口；门关闭后返回stall不访问TX state；同步detach成功后family不再持有接口结构引用，stop再确认准入计数为0 | 这是SDK合同+离线执行模型，非真机观测；不证明设备DMA静止、复位范围或可释放凭据；R02/R03/R06保持PARTIAL |
| deferred attach worker | thread_call_free失败保留lock/ref；0.44.61起该失败与selector失败一样触发整笔事务提前返回 | 未证明回调已退出时保留对象能跨provider终止存活；内存描述符/provider引用仍待真机 |
| user client / selector | externalMethod事务锁；clientClose只mark/log（UserClient.cpp:325）；selector门禁等待65秒（覆盖命令预算），Cx3SelectorGuard保证每条返回路径减计数 | 真机并发未测；迟到selector在service已终止后如何退出尚仅有源码路径 |
| 接口 detach / enable / stop失败 | `Mlx4StopDisposition`确保selector或attach worker任一未静默就不拆卸；selector预算65秒覆盖60秒命令。`enable()`在poison时拒绝数据面。只有两worker静默才detach并停表 | 提前返回会把整笔事务保留到重启；服务已终止后迟到selector能否正常退出仅有源码路径。四组合24检查通过，无真机并发 |
| 释放凭据类型 | 枚举仍只表达通道；R06新增六条件`Mlx4FlrProof`（能力/TRPND/CPU/调用/可读/同域范围），8检查先红后绿 | 纯模型未接selector或free；实际事件、代次和范围ABI尚缺。现`flr.inc`手写位路径缺TRPND及平台事务，详见[FLR契约](flr-quiescence-contract.md) |
| PCI bus-master | `stop()`在全部teardown之后、`fHw.shutdown()`之前清bus-master位并回读command register；`start()`重新使能 | 只阻止**新的**DMA请求（桥/根复合体执行，固件卡死也有效）；**不是flush**，在途写仍可能完成，因此不授权释放任何内存；无fPci时明确告警；真机未验证 |
| quarantine块 | zone+overflow两个账本；drain需显式静止凭据 | 已修复上表四类缺陷；卸载时故意保留到重启，待P1-S给出可证静止路径 |

`exportM2dProfile`早先只修了调用顺序，未处理子撤销失败。.68进一步在活跃/未知context、已初始化HCA或enabled网络存在时提前拒绝；初次空状态路径仍保留IRQ/EQ/ingress/TX/QP/MR/CQ/RQ→ICM→makeProfile的顺序，结构检查只作补充。完整生产profile入口的拒绝副作用和空状态正向路径已有执行测试，**不等于已证明历史事故由它触发或支持live重配**。
`Mlx4Hw::shutdown`只释放映射/邮箱/锁；已见`setBusMasterEnable(true)`，未见相应的全设备DMA停止闭环。
简单添加busmaster=false或FLR不能自动充当排空、回调和内存撤销证明。

## 锁与停止协议审查范围

已见路径：selector→事务锁→命令锁→固件等待；netTick在workloop gate下TryLock事务锁（`:11200`），失败跳过tick。
TryLock避免一类gate↔事务锁等待，但不证明stop已排空timer或selectors。
停止协议应分两段：先原子拒绝新工作并标Stopping，再在**不阻塞所需workloop gate**的协调上下文等待在途owner归零，最后按依赖图拆资源。
不能在持事务锁时同步等待一个必须拿同锁/同gate才能完成的回调。完整锁序图与worker读写集仍待完成。

## 历史故障树与当前可下的结论

| 现象/证据 | 当前判定 | 不能推导 |
| --- | --- | --- |
| 早期parseDevCap栈越界panic，已有host decode/尺寸断言回归 | 独立已知缺陷类别；查既有M2d记录 | 所有冻屏均为此问题 |
| 10月13日10:02/~10:47、10:58/11:13/11:50记录（HANG-AUDIT §10.1） | 有的时间窗口宽；IRQ/session/日志变量不同 | 用相关性排除日志或锁/设备问题 |
| 13:22:35 selector39后整机失联，无panic（§10.8） | 外部证据定位到netup运行区间 | close必是根因，或一定为设备DMA |
| attachInterface二分与super typedef错误（§10.10/§10.16） | 源码违反直接基类生命周期契约，有更窄证据 | 当前系统唯一根因已证实 |
| 0.44.47 M4网络通过 | 基础收发路径取得正证据 | 安全退出/长期/全功能全部通过 |
| 仅键盘失效、只CX3断链、host-watch也停 | 必须分别对照USB/管理en0/心跳/远端日志 | 混称“冻屏”或把强制开机当自动panic重启 |

每个历史boot的版本/UUID、准确最后phase/外部fsync行尚未全量对齐；T02该清单不勾完成。
旧报告前部的排除结论要读后部修正，尤其§10.3推翻双变量“关日志就安全”。

## 下一最小实现规格（T05输入，不是已实现）

1. 每块设备可达分配有generation、依赖、owner状态和可审计释放凭据；最小状态为HostOnly→DeviceOwned→Returned或UnknownRetained。
2. UnknownRetained不free、不填canary、不改descriptor、不作为新环复用。可只读快照/hash，不能用静默次数升级为Returned。
3. 在开始会使owner不确定的事务**之前**预留retention元数据和额度；不足则拒绝事务并保留原状态。失败后不能“没位置就free”。容量含字节与条目两种预算。
4. Retained对象必须可枚举、计数、持有必要provider/allocator依赖；不能只把指针清掉记lostpages。未实现可证的全设备安全排空前，未知状态拒绝unload/reopen/重建，要求受控整机恢复；不得假装正常释放。
5. profile在任一活跃/未知owner资源存在时先拒绝；若未来支持重配，须统一quiesce与依赖撤销事务，不先拆表再拆QP。
6. 分离“停止新提交/排空CPU回调”和“撤销设备owner”；先排回调，再QP→CQ/MR/环→ICM/FW/mailbox，实际命令顺序由依赖语义核定。任一步超时终止后续破坏性释放并poison代次。
7. 先离线让上述4项红测试真正变绿，再补条目/字节满区、分配失败、timeout、重复close、代次错配、stop/tick/attach并发等注入；不能改期待值凑绿。之后单独审查候选，再受控硬件窗口验证。

安全规格可以继续离线实现；目前不放行reset/profile/stop/unload等危险实验，也不能称冻屏已修复。
