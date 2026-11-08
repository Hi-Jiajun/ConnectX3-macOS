# DMA 静止证明：机制、边界与验收设计

2026-10-23。状态：**候选机制设计，尚无全设备静止实现及硬件验收**。对应 P1-S / T05 / R02、R03、R06。
现有`stop()`清PCI bus-master位，但其实际效果尚未在本卡验证；仍无足以授权回收全部设备可达内存的闭环证明。
本日复核修正：FLR成功、配置空间可读、静默采样各自只能支持对应断言，不能直接合成“全部DMA/CPU引用已排空”。先核验规范与平台语义，再设计释放凭据。

## 1. 要证明的到底是什么

设备可达的主机内存（EQ/CQ/RQ/TX 环、doorbell record、ICM 表、FW 区、mailbox）在被
`free` 或复用之前，必须满足：

> 该设备实例自此不再对该地址发起新的读写，且此前发起的写已经完成或可证明不会再落盘。

三条常见"证据"各自只覆盖一部分：

| 手段 | 能证明 | 不能证明 |
| --- | --- | --- |
| 固件 reset（HCR） | 新实例不再认识旧队列；命令接口重新可用 | 旧实例已发出的 PCIe 写；不是 FLR，不保证链路层排空 |
| `setBusMasterEnable(false)` | 可回读PCI Command位已清；用于请求该function停止发起bus-master事务，实际遵从仍须验证 | 不能据此断言根桥会替软件阻断所有请求；不证明posted write排空或CPU回调结束 |
| `2RST_QP` / `HW2SW_CQ` 等 | 该对象已被固件交还 | 同实例其它对象、ICM 页表、doorbell record |
| post-reset 静默读数（`resetSilenceCheck`） | 观测窗口内这些页没有变化 | 未来不会变；零变化也可能只是没有流量 |

因此**没有任何单一手段足够**；下面几种组合才是候选。

## 2. 候选机制

### A. PCIe FLR（已有 selector 7 的 `flr`）

- 前提：重新核对PCIe配置空间中FLR能力及当前状态、平台可用调用路径和同PF/双口影响。`FLR+`是PCIe能力读数，不能用VPD身份信息替代。
- 已核验Linux的TRPND前置、100ms及readiness顺序，以及macOS公开FunctionReset/阻塞/配置恢复API；六项条件模型见[FLR契约](flr-quiescence-contract.md)。现手写`flr.inc`缺TRPND和平台事务，尚未接入free。
- 仍需：FLR 之后重新初始化整条链（HCR/固件/firmware area），不能沿用旧 ICM。
- 风险：历史上有"FLR 后设备不回来"的记录；必须能回滚到"整机重启"。
- 验收设计须区分：复位执行/完成证据、设备代次与资源owner、CPU回调退出、保留provider依赖；配置读回、ownership和静默/CQ读数只是其中的观测量。由R06补齐规范依据及可执行契约后再实施。

### B. bus-master off + 有界等待 + 静默读数

- 顺序：先 teardown 队列 → 清 bus-master → 等待 N 个观察窗口 → 比对设备可达块的摘要。
- 能证明：窗口内所采样内存未变化，PCI Command位读回符合预期；不能外推根桥替软件阻断设备请求。
- 不能证明：窗口之前已发出、尚未落盘的写。
- 适用：**不能**作为释放未知 owner 内存的凭据，只能作为"再等一会儿"的辅助。
- 结论：本项只保留为 FLR 不可用时的降级路径，且必须保持"保留不释放"。

### C. 固件 reset + ownership 握手 + 静默窗口

- 与 B 类似的推理，额外要求 `QUERY_FW`/ownership 字显示新实例已接管。
- 仍不覆盖链路层在途写，因此同样不能单独授权释放。

### D. 整机重启

- 当前受控恢复退路：保留未知owner内存直到整机恢复；内存重用前仍需正常平台复位/设备隔离。历史上暖重启可能保留异常设备状态，故“点重启”本身不能替代恢复完成检查，必要时遵循已批准的断电恢复流程。
- 当前隔离区/`fIcmKept` 的默认结局就是它——这是刻意的，不是遗漏。

## 3. 建议的验收（先离线，后上机）

离线部分（现在就能做）：

1. 把释放前置条件写成可执行断言：凭据由经核验的操作结果生成，绑定设备代次/资源范围，并与CPU引用排空联合检查；B/C的观测结果永远不能单独生成凭据。A的充分条件由R06核定，不能用“FLR+config可读”预先写死。
2. 为 `quarantineDrain`/`freeIcmTables` 增加"凭据类型"枚举，而不是一个 bool——这样日志
   能区分"FLR 证明"与"只是驱动要退出"。
3. 失败注入：让 FLR 返回失败/超时，断言所有保留块仍然保留、没有任何 `icmFree`。

上机部分（需要暂停窗口）：

1. 制造已知在途流量（持续 TX + RX），然后执行 FLR；
2. 比较 FLR 前后设备可达块的摘要（`collectDeviceBlocks`），并要求**连续 K 个窗口零变化**；
3. 同时确认没有新的 completion 进入 CQ；
4. 1–3是观测实验，不能单独授权释放；还必须满足R06规范/平台契约、CPU引用与依赖退出及本代凭据条件，才能标记`Returned`。契约未闭环时保持保留，实验只能收集证据。

停止条件：摘要出现变化、FLR 超时、设备从 PCI 配置空间消失、或独立管理链路（en0）异常
——任一条成立即停止，不自动重试。

## 4. 与现有代码的关系

- `stop()` 目前只做 B 的前半步（清 bus-master），并明确记录"不是 flush、不是静止证明"。
- **2026-10-23 起 `quarantineDrain(why, Cx3QuiesceProof)` 用三值枚举取代了 bool**：
  `kCx3QuiesceNone` / `kCx3QuiesceStopping` / `kCx3QuiesceFlr`，只有最后一个是释放凭据。
  `stop()` 传 `kCx3QuiesceStopping`，因此**生产中没有任何路径会传 `kCx3QuiesceFlr`**——
  这与A尚无完整实现一致。枚举只能表示意图，不能在类型层面证明实际复位成功或代次相符。
- **状态更正（2026-10-23）：** 当前`freeIcmTables()`的`!asked`分支不依据静默读数free；它通过`keepIcmForQuiesce()`转移到保留账本，账本满时保留原table/AUX slot并拒绝继续。`tools/test-icm-release.py`对静默/变化/未采样场景执行生产方法并断言零free。旧版“可能仅凭静默读数free”的描述已过时。剩余问题是之后何种经核验的release credential能安全回收这些owner，以及owner/账本在 stop 后的生命周期、CPU/IRQ drain 与完整reset scope。
- 0.44.59把FW区纳入保留门；mailbox、CPU回调、保留账本与provider依赖仍属R02/R03审查范围。

## 5. 未决

- 本卡 FLR 的实际可用性（历史记录里有成功也有"不回来"）。
- FLR 是否会连带影响另一个 port 的 function（同卡双口通常同一 FLR 域）。
- 静默窗口需要多长才有意义：现有 `resetSilenceCheck` 是 8×2 ms，明显只够捕捉"已经
  在流水线里的完成"，不足以覆盖长延迟写。
