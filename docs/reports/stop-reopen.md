# 正常网络停止与新实例初始化合同

2026-10-24；候选 `0.44.127-local`。**离线部分通过；完整真机 stop/reopen、100轮资源稳定性和故障恢复未验收。** 本文是 T05 的合同与证据入口，不是其完成回执。

## 入口与边界

接口 `disable()/enable()` 属于同一个仍存活 controller 的网络 down/up，保留硬件 owner，使用 callback admission 计数；它不同于驱动 `stop()`。正常 `stop()` 完成后只能由新的 controller 实例继续初始化。旧实例的 RUN_FW sticky、poison、generation 与 allocator history 不能清零来伪装成新对象。

`.127` 在真实 `start()` 中补两条准入：`fStopRetained || fPciOpen || fDeviceGeneration != 0` 时，在 superclass/provider 访问之前拒绝；fresh controller 的 provider `open()` 返回 false 时，清除未获准的 PCI/identity 引用并返回，不进行 PCI enable、BAR/HW init、probe 或服务/worker 发布，也不 close 别人的 owner。`init()` 将这三个条件初始化为 false/false/0；成功 start 设置 generation=1，正常 stop 保留其非零值。

新对象本地状态干净不证明物理设备已恢复。当前 `start()` 不执行硬件 reset，不为旧 owner 生产静止凭据；新实例的 generation=1 是局部标签，不与旧实例组成全局单调代次。跨实例互斥仍依赖真实 provider 仲裁。SDK 的 IOService 默认 handleOpen 是单 owner，但 IOPCIDevice 覆盖该方法；不能只从通用接口注释推导目标内核实现。当前系统文件的 IOPCIFamily CFBundleVersion 为2.9，尚未取得与目标实现对应的仲裁证据。

只读补充主来源：[Apple IOPCIDevice.cpp固定commit](https://github.com/apple-oss-distributions/IOPCIFamily/blob/4822b27a36e2de70e231ecf2bf3021384fab6ec2/IOPCIDevice.cpp#L2135)的handleOpen委托`super::handleOpen`并返回相同bool，没有将拒绝改为成功。通过gh获取后按commit重新取得并核对内容相同，[来源与SHA-256](../../../../outputs/20261024-cx3-network-stop-contract/provider-source-fetch.json)已保存；它支持遵守open返回值，但没有证明该公开revision就是本机内核实现，也未验证superclass stop是否改变provider open状态。

本批同实例重入结论限默认硬件候选`CX3_START_LEVEL=2`。可选诊断level0只绑定/发布，不设置open或generation，当前同对象重复进入可能重复register；它没有PCI enable、BAR或DMA路径。本批没有将它标为已验证，也没有为完成默认硬件合同新增另一个diagnostic lifecycle状态。fresh-start host fixture采用显式默认值，未执行生产整套init；源码初始化的核对与行为测试分别记账。

## superclass/provider 源码核查（2026-10-24）

[固定 Apple 源码核查](../../../../outputs/20261024-cx3-provider-stop-source/REPORT.md)及其[source-metadata.json](../../../../outputs/20261024-cx3-provider-stop-source/source-metadata.json)表明：公开 IONetworkingFamily commit `bb910e2b458939f6b1f53c0fa6907f258b431ea4` 的 IOEthernetController 未覆盖 stop，实际继承的 IONetworkController::stop 调用 PMstop 和 IOService::stop；公开 XNU commit `f6217f891ac0bb64f3d375211650a4c1ff8ca1ea` 的该方法没有 provider close/terminate。网络 gate/workloop/output queue 的释放在 free，不在 stop。PMstop 则会禁止新 PM driver callout、等待已有 callout，并异步断开 power plane，不能当作空操作。

这些版本未与本机 15.8.1 二进制对齐。没有找到“super::stop 主动 close PCI provider”的具体源码缺陷，但也没有证明本机迟到 callback、provider termination/power 或跨实例 open 仲裁安全。此处源码疑点已收窄，运行时义务仍开放；不应重复从零查同一公开 stop 调用链，也不应因此新增硬件 PASS。

## 状态/资源矩阵

下表顺序就是 stop 的依赖边。任何失败/未知分支都须在释放依赖前 pin self/provider，并使 incomplete-stop sticky；正常尾部才关闭 provider。保留引用不防止 provider termination/power removal。

| 阶段/资源 | 正常前置与结果 | 失败或未知结果 | 当前证据及限制 |
| --- | --- | --- | --- |
| TX、timer、family enable/disable callback | 关闭 admission；已入场调用排空；后续调用拒绝 | 等待超时，不碰 IRQ/网络/queue/table/BAR | 实际 gate 与方法离线交错；真实调度待测 |
| selector、attach/detach worker、interface | selector有界排空；worker不再访问本地资源；同步family detach完成 | 原接口、锁、worker与所有依赖存活 | 实际worker/stop离线测试与SDK合同；不证明目标内核运行 |
| IRQ/timer event source | 成功移出 workloop 后才release；timer移除先于TX accounting | source/workloop owner不丢，后续停止截断 | 各source失败测试与stop组合；不等于DMA停止 |
| TX mbuf与frame ring | 在TX锁下先数未完成mbuf，再清host mbuf；零pending frame ring可按既有合同释放 | pending ring进quarantine；转移失败保留原slot/lock；两者均截断queue及以后阶段 | 组合覆盖0、末槽1、全槽pending；实际stop/EQ/net-stop；CQ完成与设备语义待实测 |
| port1/port2 | HCA已初始化时按已知端口数CLOSE_PORT，bool成功且status=0；新的INIT_PORT尝试先使旧回执失效 | 原child/table/FW保留，不把旧成功回执用于新尝试 | 真实wrapper/helper/stop；端口设备行为待测 |
| EQ、ingress/RX/TX QP、MR、CQ、RQ、MTT及frame | 各owner独立HW2SW/2RST/MTT释放确认；owner谓词为空才继续 | 精确slot/MTT或retention账本保留；禁止HCA/table/FW清理 | 各真实生命周期测试；组合内除EQ外child teardown为显式seam，不能声称完整真实队列链已合成执行 |
| HCA | callback gate关闭/排空、网络禁用、port关闭、child/retention为空；CLOSE_HCA bool/status确认 | HCA初始化与mapping知识保留，不UNMAP table/FW | `.125`真实wrapper/helper和组合；设备语义待测 |
| table/AUX | HCA关闭后逐allocation撤销；bounded slot扫描与retention均为空 | 原slot或kept账本保留，不进入FW/shutdown | 真实ICM生命周期测试；stop组合中的table事务为seam |
| FW | 无HCA/mapping/child/table/retention；精确allocation/owner代次有效；host-only未RUN_FW直接free，handoff后UNMAP_FA bool/status=0才free | 原FW slot、mailbox、BAR/provider及sticky状态保留 | `.126`真实helper/wrapper与组合；不授权异常reset后的释放 |
| mailbox、BAR、PCI/provider、workloop | 所有owner为空才shutdown；命令完结后清BME、close本client provider，最后交还workloop | 之前阶段保留时不执行这些收尾 | 清BME不flush已发PCIe事务；实际provider/IOKit与DMA静止待验证 |
| 旧实例再start | 无允许路径；retained/open/非零generation均拒绝 | 不覆盖原PCI、BAR/worker或owner，不清history | 真实start行为反例与逐guard mutation；不证明framework会实际重调同对象 |
| 新实例start | 本地fresh状态且provider open成功才进入PCI enable/map/probe/publish | open拒绝零写、零map、零发布；不close旧client | 真实start over host seams；没有实际stop后新service重连/联网证明 |

## 离线验收

证据：[.127验收目录](../../../../outputs/20261024-cx3-network-stop-contract/README.md)。

- `tools/test-start-admission.py` 执行实际生产 `start()`，12场景37检查0失败；provider/superclass/硬件init为host seams。对冻结`.126`使用同一最终测试有22项行为失败，不是编译失败。分别移除三个guard、忽略open拒绝、拒绝时错误close的5个mutation均编译成功并被行为断言拒绝。
- `tools/test-stop-composition.py` 执行实际stop、EQ、net-stop、owner谓词与关闭helpers/wrappers；96场景447检查0失败。新增两个端口数×三种pending分布；零pending可走到FW与provider正常收尾，pending保留精确frame ring和全部依赖。其他child/table/IOKit行为仍为seams。
- 全量51个Python脚本、完整kext/CLI构建、85项输入前后及tar字节核验均通过。独立审查区分了源码证明与IOKit/DMA边界。

## 尚未完成与下一步

1. 核定目标IOPCIDevice open仲裁及provider terminate/power语义；若允许跨client共享，必须有同function跨实例准入，不能把当前open检查当作充分互斥。
2. 将真实child/table撤销与网络停止的组合覆盖向下推进；分别记录正常协议撤销、异常reset凭据和PCIe在途事务边界。不得为通过测试取消pending TX或未知owner的保留。
3. 准备绑定准确候选/hash/ABI/selector白名单、独立管理与告警/phase/采集器回执、停止及恢复步骤的最小硬件包。本次14:42本机快照提前停止的缺口尚未关闭，不把历史告警200当新回执。新硬件操作仍需当轮暂停窗口。
4. 硬件验收分别记录重新联网、安全stop、新实例重新联网和100轮资源稳定性；当前这些项均未通过，不以本报告或host测试次数替代。
