# CX3 Pro 功能优先实施计划 v4

**生效：2026-10-25，依据用户本轮要求。** 本文覆盖 v3 中“先完成各阶段完整测试才能继续开发”的执行顺序；F01–F44 的范围和最终完成标准保留。详细验收延后到[独立测试计划](CX3-PRO-FINAL-TEST-PLAN.md)，不再作为所有后续编码工作的串行前置。

**目标：** 先完成本卡实际适用功能的生产代码与系统/SDK入口，每批简单验证；形成全功能开发候选，再集中执行完整测试和修复。

**实现架构：** 复用现有 IOKit kext 和原生 Ethernet 接口；没有系统通用入口的功能通过版本化 SDK/工具提供。功能默认关闭或需显式配置，开发候选不替换当前临时使用的 `.47`。不在本轮引入 DriverKit 重写。

**技术栈/依据：** C++/IOKit、C 用户态 CLI/SDK、Python 辅助工具、现有 FreeBSD 双许可参考；[能力矩阵](CX3-PRO-CAPABILITY-MATRIX.md)、[决策](CX3-PRO-DECISIONS.md)和总计划定义范围。

## 执行规则

1. **开发准入与最终验收分开。** T06–T17 可按真实代码/ABI依赖开发，不等待 P0/P1 的完整硬件、100轮、48/72h验收。已知影响本功能的实际错误仍应修；不在无关审计上循环扩展。
2. **每批交付可用行为。** 优先从硬件命令→内核→ABI→CLI/SDK→调用样例接通一条功能链。只有文档、空接口、固定成功返回、软件模拟或“unsupported”占位不能算该功能实现。
3. **日常验证固定缩小。** 编译/链接、内核与CLI的ABI、所改功能一条成功和一条关键拒绝路径；改动触及DMA/权限时保留相应边界检查。不要求每批执行全部既有测试、mutation、全场景失败注入或再次独立复审。
4. **不重复已有验收。** 生产输入没变化就复用已保存结果；只有新失败、相关生产路径变化或最终集成时扩大回归。记录失败后修实际问题，不以测试数量或候选版本数汇报进度。
5. **硬件串行、窗口明确。** 本轮调整授权开发和减少中途测试，不自动覆盖新候选安装/初始化/重启、固件持久写或正在使用的网络切换。拿到窗口后可将多项短冒烟集中在同次候选启动，不必为每项换版本重启；失败停止，不盲目reset循环。
6. **维护两条版本线。** 当前日用临时 `.47` 与开发候选分离。完整测试通过后再晋级稳定版；原先“正常stop/reopen未实测”等风险如实保留，不能靠删除检查制造完成。
7. **协作按独立模块划分。** GPT系列优先；派发前声明模型/思考档位，通常 Luna 或 Sol，Astra仅特别复杂任务且最高 high。生命周期/ABI共同写入串行，独立CLI/SDK/工具可并行；不使用DeepSeek或yeta。

## 状态与进度口径

每个子功能独立记录 `实现 / 简验 / 完整验收` 三列：

- 实现：未开始、开发中、DEV_DONE、BLOCKED-PLATFORM/LAB/HW。
- 简验：未跑、SMOKE_PASS、SMOKE_FAIL；附当前构建与一条功能命令结果。合成transport必须注明。
- 完整验收：DEFERRED、TESTING、PASS、FAIL；有硬件依据才能 N/A-HW。

既有能力矩阵的 I/O/M4/UNKNOWN 含义不改写。`DEV_DONE + SMOKE_PASS` 可以继续下一批开发，但不等于功能实测通过或可长期使用；不能把外设/平台缺口标为已实现或无关。

## 功能批次与实际依赖

| 批次 | 功能/稳定ID | 主要代码位置与交付 | 开发只依赖什么 | 简单验证 |
| --- | --- | --- | --- | --- |
| A 管理与启用入口 | F01/F03/F36/F37/F38/F44 | `src/Mlx4Hw.{h,cpp}`、`Cx3Mlx4{,UserClient}.{h,cpp}`、`cx3_abi.h`、`tools/cx3ctl.c`；模块EEPROM/DOM专用GET、完整能力/双口查询、单实例自动启用控制器和可禁用配置。复用已有47/48/49身份接口 | 现有命令/身份/生命周期接口；不等长期稳定性 | 编译、ABI、GET结果解码一正一负；启用状态机dry-run。未获硬件窗口只标离线简验 |
| B 完整原生网络 | F04–F10/F18/F35电源部分 | `Cx3Mlx4.{h,cpp}`、`Mlx4Hw.{h,cpp}`、`mlx4_pure.h`、CLI；每口资源/接口、link/media、过滤、MTU/jumbo、VLAN/checksum、正常down/up和电源回调。复用已有实现补缺口，不重写全部 | 每口对象/共享PF仲裁；相关正常关闭接口存在。框架限制需明确处置 | 每特性1条短功能链；有窗口时两口各取得地址、单次切换/短收发；不跑100轮 |
| C 中断与多队列 | F14–F17 | IRQ/CQ arm/moderation、每口多RX/TX环、RSS key/indirection、流过滤与计数；内核调度入口不依赖macOS提供Linux式RSS控制API | B的端口/队列所有权；RSS异常所需原始查询在可用窗口集中采集。缺真机语义不能猜值强开 | 两队列/两流最小例、一次arm/rearm、规则增删；先接通实现，压力/竞态组合延后 |
| D Offload/数据通路优化 | F11–F13，补齐F10 | `mlx4_pure.h`、TX/RX路径与ABI/CLI；TSO4/6、SG、inline/BF按cap、缓冲复用/聚合按平台接口 | B的标准帧路径及所用字段合同；TSO不以jumbo为前置 | 大TCP请求分段样例、SG一例、开关与非法长度各一例；吞吐/内存压力延后 |
| E RDMA公共SDK与RoCE | F24–F28/F43 | 新 `include/cx3/`、`sdk/`、`examples/`；现有UserClient分离受限对象ABI，context/PD/MR/CQ/QP/SRQ与事件；RC SEND/RECV后扩展READ/WRITE/atomic、UC/UD、CM/GID、v1/v2和应用接入 | 共用对象/端口接口及内存授权/撤销实现；SDK生命周期与代码可提前，不等B–D所有测试 | 库/样例可编译；真实transport一正一负；有对端时最小SEND/RECV。mock不能算RDMA已实现 |
| F 时钟/QoS/隧道/诊断 | F19–F23/F36/F37 | 专用硬件查询/配置及SDK；原始时钟/时间戳→关联帧；Pause/PFC/ETS/DCB/QCN、隧道offload按cap。诊断/计数随A先交付 | 对应cap/命令和B/D数据路径；可拆开开发 | 一次查询/配置/样例，误差、拥塞、隧道组合交最终阶段 |
| G VPI/IB/IPoIB | F29–F32/F43 | 模式工具、MAD/SM接口、PKey/GID、IB transport、IPoIB原生IP入口或配套服务；共享E对象 | 本卡cap和E公共对象；模式切换另窗口，SM可用既有Linux端 | 编译、管理帧解码；有IB对端时一次链路和最小传输；不能以RoCE替代 |
| H 虚拟化/板级扩展 | F33–F35/F38–F42 | PF/VF mailbox与配额、宿主/VF接口、WoL、peer-memory注册撤销、专有offload、sideband、ROM交接工具 | 每项本卡支持及macOS实际接口。平台缺口尽早调查，与A–G开发并行；可实现的项继续写代码 | 每项最小实际操作或明确阻断证据；禁止把host staging叫GPUDirect或把VF枚举叫guest可用 |
| I 集成/发布实现 | F43/F44及全部能力入口 | 安装/回滚、SDK版本协商、配置/权限、支持矩阵和功能开关；整合上述开发候选 | 相关接口已实现，不等最后72h结果才写安装和文档 | 干净构建、安装包静态检查、卸载/回滚命令审阅；实际安装另窗口 |

每批结束即进入下一条可开发任务；某批卡在硬件/外设/平台时转到其他批，不继续无限增加其离线审计。共享核心改动按文件owner串行；只有关键架构决策需要额外设计，常规取舍直接执行并记录。

## 现在的执行队列

- [x] 本轮策略、总计划/续跑入口和独立完整测试计划落盘；构建新增显式 `smoke` 路径。
- [x] **A1首个切片：EEPROM/DOM page0 GET生产链（.130实现与简验；其余F36子项仍开放）。** 复用 `vendor/freebsd-src/mlx4_port.c:1859–2018` 的MAD_IFC GET；固定GET/属性0xff60/opmod3，校验port、offset/length≤48和低/高页边界；保存HCR/MAD状态与实际长度，未读到不伪造零值。接入追加selector、dispatch/sizes/CLI和模块类型解码。文件归A；一正一负及编译通过后继续A2。
- [x] **A2首个切片：自动启用产品入口生产实现与离线简验（.131）。** selector51在全局transaction内复用16个生产export；严格阶段结果、版本/service/provider/generation绑定；CLI补OS boot绑定；status/enable/disable、重复启用不重置与sticky失败。配套一次性服务默认disabled，校验返回接口/MAC后才配置up/DHCP；launchd模板只生成未安装，无自动重试。编排/CLI合成传输28检查、服务3简验及完整smoke构建通过。支持端口1标准MTU；disable只暂停主机交付，不是硬件关闭。首次上机、实际开机自启/禁用启动、安全模式和完整F03验收仍开放。现转B，不等全量回归。
- [x] **B1管理状态切片（.132）：** 两口独立QUERY/raw/时间戳、有效MAC、link/MTU与INIT/CLOSE回执，追加selector52/CLI；SET成功才记MTU、查询失败不冒充有效链路，所有实际端口关闭后才可CLOSE_HCA。生产路径定向host简验和完整smoke构建通过；无新硬件PASS，详见[交付](../../../outputs/20261025-cx3-port-state/README.md)。管理状态拆分不等于双口queue资源隔离。
- [x] **B2a真实队列owner存储（.133）：** 75个实际CQ/RQ/SQ/QP/MAC/raw字段迁入固定的两个端口容器；原生路径仍显式对象0，共享MR/ICM/profile/reset保留预算与观察覆盖两口。受影响生产路径host简验、端口间状态隔离和共享依赖拒绝场景、完整smoke构建通过；无新硬件PASS，见[交付](../../../outputs/20261025-cx3-port-queues/README.md)。不清空旧owner、不复制活跃队列。
- [x] **B2b每口setup/teardown与编号（.134）：** 真实CQ/RQ/RX-QP/RSS-QP/TX与回收显式传口；完整不重叠MAC窗口、低端RSS/TX QPN和独立CQ编号；selector53/CLI的status/create/destroy、代次/身份绑定、partial sticky失败和保留owner。完整smoke构建及生产编排/UC/CLI、第二口实际RX/RSS/TX方法的合成简验通过，见[交付](../../../outputs/20261025-cx3-port-queue-api/README.md)。未安装、无新硬件PASS；ready只指队列创建，不含INIT_PORT、原生接口或联网。
- [x] **B2c独立原生接口（.135）：** 新`Cx3EthernetPort`固定PF/port/generation，每口独立controller/interface、workloop/timer、callback gate、TX锁与收发计数；真实RX/TX/repost使用本口队列，DMA仍归PF。selector54/身份绑定CLI已接通；partial/timeout保留owner，单口detach不动另一口，PF stop先排空selector再排空两口。完整smoke及真实controller/PF/UC/CLI双口合成简验通过，见[交付](../../../outputs/20261025-cx3-native-ports/README.md)。未安装/硬件未验，标准MTU/软件checksum起步；动态MTU/过滤/offload等继续B3/D，双口DHCP与真实框架/DMA/稳定性集中验收。
- [x] **B3a原生过滤/checksum切片（.136）：** 新controller的真实混杂/组播mode/list回调经worker编程本口，禁用暂存、enable应用、部分失败/timeout保留；支持广播保留和名单溢出全组播回退，发布desired/applied状态。既有IPv4/IPv6 TX/RX checksum路径接入native family能力。完整smoke和真实方法合成简验通过，见[交付](../../../outputs/20261025-cx3-native-filters/README.md)。未安装/线上未验；F07/F10整体不关闭，多个单播/MAC变更、checksum运行开关和全协议组合仍开放。
- [x] **B3b MTU/jumbo切片（.138）：** managed双口RQ/TX 16KiB槽、跨页IOVA连续性校验、预配置wire ceiling内的family MTU及单层VLAN边界，完整smoke/定向简验通过；未安装，在线扩大硬件ceiling/缓冲重建仍开放。见[WAVE2](../../../outputs/20261025-tahoe-acceptance/cx3/WAVE2.md)。
- [x] **B3c L2与配置切片（.139）：** TX硬件C-tag插入（owner发布前编码）、RX CQE剥离元数据与内联C-tag规范化、SET_VLAN_FLTR端口白名单及selector55/CLI、原生MAC回调重写本口slot0、RX checksum运行标记开关及TX attach前配置。完整smoke与定向合成简验通过，详见[WAVE3](../../../outputs/20261025-tahoe-acceptance/cx3/WAVE3.md)。未安装/无线上PASS；QinQ、多单播、TX在线重新协商与全协议组合仍开放。
- [ ] **B3余项：** 链路事件/media动态更新、正常down/up与电源回调；在线MTU ceiling/缓冲重建、多个单播的实际steering，以及TX checksum在线重新协商按真实依赖继续。已接通的VLAN/MAC/checksum不反复重跑广泛审计，真实双口、并发、完整回归按独立测试计划集中执行。
- [ ] C–I 按表的接口依赖继续；H平台可行性提前查，缺接口如实阻塞。

`.129` QP/RSS采集包停在当前只读预检/host测试水平；本轮不继续为其扩建观察基础设施或重复全量测试。独立观察回执缺失仍记为硬件执行阻断，代码不会以手填布尔值获得新候选上机放行。

## 开发验证命令

在父工作区使用全新OUT，避免覆盖旧证据：

```bash
CX3_TEST_PROFILE=smoke BUILD_TAG=<本批候选tag> OUT=<本批全新构建目录> bash tools/cx3-mlx4/build.sh
/usr/bin/python3 tools/cx3-mlx4/tools/<所改功能的定向测试>.py
```

`smoke`保留完整kext/CLI编译、启动路径检查、符号解析、selector表/CQE-WQE布局、静态sizes与CLI breadcrumb自检；跳过原构建中较广的host测试。`standard`保留原build测试选择，仍不等于整个项目或硬件完整验收。最终阶段执行全部既有host测试及独立测试计划。开发时间优先投向生产路径，不为每个新case单独升级版本、重复全量构建或保存多份相同输入归档。
