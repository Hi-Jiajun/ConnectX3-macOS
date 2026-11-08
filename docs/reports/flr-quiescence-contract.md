# FLR 静止凭据：证据、平台语义与实施契约

2026-10-23。**R06设计；六条件纯模型已实现/测试，尚未接入reset或stop，未上机**。
对应P1-S、D02、T05。结论先行：本卡有FLR支援位的历史记录，但“写一次FLR位”不是凭据。
合格凭据需要六项独立证据，且只能覆盖function域；CPU回调和保留依赖要在FLR之前退出。

## 当前实施增量（0.44.90，仅离线）

0.44.91另加平台reset前配置校验：ID、BAR0及Command memory decode任何明显无效
即NotReady退出，不调用平台。4个损坏态反例先红12项，通过96场景552检查。仅是
reset事务准入底账，非DMA静止或恢复证明。

`flr.inc`已改为调用`Mlx4Hw::platformFunctionReset()`；适配器指定公开
FunctionReset+OptionNone，不使用私有API、terminate或手写bit回退。
平台非Success原样写入结果后立即返回，不再读取配置空间或旧MMIO。
平台Success后只作配置身份、两个BAR dword及Command的诊断核对；异常为NotReady。
所有路径均不读旧MMIO，不清poison、不释放资源，也不将`fIcmHcaHolds`改成已归还。
两个准入latch继续保持，旧HCA标记只作为未证明的保留状态，不授权重建。
legacy owner输出固定为`ffffffff`，不是实际寄存器值；工具区分未读和still-set，
reset失败返回非零退出码。结果尺寸不变，本次没有新增代次/scope凭据ABI。

真实入口/合成硬件边界测试92场景532检查0失败，最终同测试重放封存.89为50失败、
exit1；真实cmdFlr/合成transport的5项行为检查通过，旧工具反例因误报still-set失败。
适配器9场景62检查0失败，五种变异逐项编译成功且执行断言失败。
verification-batch44.json为PASS，32组及构建exit0，65条live/tar清单与两个binary
已逐项核对；不代表硬件验证或R06关闭。

## 历史实施增量（0.44.80，仅离线）

FLR已接入reset专用保留预检并拒绝IRQ存活；配置写入前设置sticky reset标记，
不允许reset/FLR交叉重复尝试，不再因手写FLR与sleep清mailbox poison。
生产入口组合测试58场景380检查0失败，旧FLR同测试40失败；细节见
[安全审计](safety-lifecycle-audit.md)。**下文平台reset事务与代次/scope凭据仍是待实现契约**，
初次通过预检的FLR仍走旧bit路径，无生产静止凭据，不授权释放或上线恢复。

## 平台调用面

macOS 15.5 Kernel SDK公开`IOPCIDevice::reset(tIOPCIDeviceResetTypes, options)`和
`kIOPCIDeviceResetTypeFunctionReset`，文档说明配置状态在reset前保存、完成后恢复，调用阻塞到
链路可用；`kIOPCIDeviceResetOptionNone`避免terminate/re-probe。`resetFunction()`是private，
protected的`supportsFLR/prepareFLR/completeFLR`也不能从本驱动直接调用。公开reset文档明确说明
多function设备会复位关联的全部function，不能假定只影响一个端口或一个PCI function。

证据：`/Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk/System/Library/Frameworks/Kernel.framework/Headers/IOKit/pci/IOPCIDevice.h:1095-1134`，
`IOPCIFamilyDefinitions.h:362-398`。因此新路径应调用公开`reset(...,None)`，不能再手写
Device Control位；返回`kIOReturnUnsupported`表示不可用，其它失败同样不得升级为凭据。

封存.89及以前的`src/flr.inc`是手工路径：检查FLR capability、写bit15、`IOSleep(150)`、读BAR/vendor。
旧路径**没有**等待Device Status的Transactions Pending（TRPND）清零，也没有通过平台API完成
“保存/关闭Command、reset、恢复、等待ready”。这些是待实现的确定差距，不是风格差异。

## 规范与参考实现的顺序

本机Linux 6.6 GKI源码（`abl-xr-fuzz/work/gki-vm/ack-r30/drivers/pci/pci.c`）给出可核对顺序：

1. `pci_dev_save_and_disable()`保存状态，向Command写`INTX_DISABLE`，同时清Memory/IO/Bus
   Master：阻止复位前新DMA和BAR访问，并遮住legacy中断；
2. `pcie_flr()`先`pci_wait_for_pending_transaction()`，轮询Device Status的TRPND；
3. 写Device Control的FLR bit；
4. 按PCIe r4.0 §6.6.2等待100ms；设备可在此期间静默丢弃请求；
5. 轮询配置空间，直到Vendor/Command不是全1，并处理Root Port RRS；
6. `pci_dev_restore()`恢复配置，再重新初始化MSI/BAR/解码等。

关键行：`pci.c:5130-5172`（保存/关闭与恢复）、`:4486-4513`（TRPND、写FLR、100ms、
readiness）、`:5303-5326`（作为一笔保存/重置/恢复事务）。`include/uapi/linux/pci_regs.h:537`
定义TRPND。Linux在TRPND等待超时时会记错但仍继续reset；本项目的释放凭据比“reset成功”更严格，
因此TRPND超时不算六条件齐全。

Apple公开文档没有展开TRPND顺序，但承诺调用阻塞到设备可用、配置先保存后恢复。实现后仍由
selector返回的实际状态和上机观测决定，不能从头文件推断内部每一步。

## 六项证据

`mlx4_pure.h::Mlx4FlrProof`把条件写死为六个bool，只有全真才`credential()`：

| 条件 | 取得方式 | 为什么必需 |
| --- | --- | --- |
| `supported` | 当前配置空间Device Capabilities bit28 + 平台返回非Unsupported | VPD里的FLR记号不能冒充当前能力 |
| `pendingCleared` | 读Device Status确认TRPND为0；若初值为1则有界等到清零 | FLR适用前置；不是要求必须先观察到1，也不单独证明posted writes完成 |
| `cpuDrained` | selector gate与attach worker均静默（R03 disposition complete） | FLR只清设备，CPU可能仍解引用要释放的内存 |
| `resetIssued` | 平台reset调用完成并返回Success | 只是调用开始、写位或sleep都不能证明完成 |
| `readyReadable` | reset返回后Vendor不是`ffff`且Command不是`ffff`，BAR身份复核 | 设备可能不回来或配置未恢复 |
| `scopeAccounted` | 另一个function/端口的当前owner和状态已记录，并按实测影响处理 | 双功能可能共享受影响域，不能只看port1 |

六项全真后，还要在释放前重新清Bus Master并回读Command；平台恢复配置可能把它重新置位。
随后执行有界静默摘要对照，作为观察窗口内无变化的补充记录。六bool只是候选契约模型，
尚未证明posted writes、平台reset作用域与设备代次全部闭环，不能因全真就直接授权生产free。

## 范围、失败与恢复

- 只在全部设备可达内存已有owner账本并且CPU引用退出的停止事务里使用。FLR成功后旧
  `fIcmHcaHolds/fHcaInited/fIcmMapped`及全部QP/CQ/EQ代次失效，必须进入新代并整链重建。
- function reset若实际连带peer：记录peer的BAR/厂商/命令和驱动attach状态；若peer在复位后
  不可读，只允许把凭证用于已登记的同域块，未登记/peer仍owned的块保留。
- Unsupported、TRPND超时、reset非Success、ready读回失败、BAR变化、peer不能解释：六条件
  至少一项为假，**零free**，保留所有未知owner并停止后续命令，不自动重试。
- 回滚入口是整机重启。历史上有FLR后设备不回应的记录，恢复盘与独立管理链路必须先可用。
- 成功后不能沿用旧BAR映射；地址变化时重新映射并作废旧视图。命令poison可清，但命令接口
  是否真正可用需QUERY_FW新读数验证。

## 实施与验收顺序

1. 纯模型六条件测试已实现，含全真与逐项缺失8例；先保持红-绿证据。
2. 新增“FLR proof、generation、scope mask”诊断结构，不在旧ABI中间插字段；旧工具/旧kext/
   新工具组合要明确拒绝。
3. selector7改为平台FunctionReset；记录TRPND初值/等待耗时、reset返回、ready值、BAR和peer。
4. 只有六条件全真才向`quarantineDrain/freeIcmTables/fIcmKept`传带代次和范围的凭据；先离线
   注入每个失败分支，断言保留与零free。
5. 上机窗口先无流量验证reset/回来/peer，再做持续TX+RX时的FLR和摘要，最后才做短循环。

TDD 证据：`tools/test-flr-proof.py` 8项检查。仍是设计模型，不代表生产代码已生成六条件。
