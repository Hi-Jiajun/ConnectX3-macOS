# macOS 上的 Mellanox：别人的三类实现，与一条和我们直接相关的结构差异

查于 2026-10-13（Tavily 检索 + 源码/正文核对）。这份文件的目的只有一个：
**在决定"继续修 kext 还是换架构"之前，把别人的做法与它们各自的前提摆出来**。

## 1. 没有开源的 CX-3（mlx4）macOS 驱动

| 实现 | 形态 | 支持范围 | 与本项目的关系 |
| --- | --- | --- | --- |
| Apple `AppleEthernetMLX5` | 闭源 **DriverKit** dext（`/System/Library/DriverExtensions/`） | mlx5 = CX-4/5/6 | 按"**任意** 15b3 厂商 ID + 网络类 0x020000"匹配 ⇒ 会抢 CX-3 然后失败（本机开机日志里可见） |
| ATTO FastFrame 的 macOS 驱动 | 闭源二进制 kext | CX-3 世代芯片 | 社区在 CX-3 上一直走这条线；`docs/handoff-cx3-mlx4.md` 里已有"改 ATTO 驱动行不通"的实测证据 |
| `github.com/b-ostrov/MelonDMA` | 开源、**PCIDriverKit**（dext） | CX-4/5（15b3:1015），RoCEv2/RDMA | README 自述"激活时**取代** AppleEthernetMLX5 成为这张卡的所有者"；作者硬件上 P0–P3 生产门通过 |

⇒ **没有一类是开源的 mlx4/CX-3 以太网驱动。** 我们这条路的坑没有现成答案。

## 2. ★ 结构差异：dext 路线里根本没有 `attachInterface`

Apple 的 mlx5 与 MelonDMA 都是 dext：**以太网接口由 dext 框架（`IOUserNetworkEthernet` 一侧）拥有**，
驱动只做设备操作与队列。也就是说，我们 2026-10-13 用停点二分钉死的那个调用
（`attachInterface`，见 `docs/HANG-AUDIT.md` §10.10）**在那条路线上不存在**。

而且 dext 跑在**用户态**：驱动出错的代价是**进程死**，不是**整机死**。对"设备还没调通"的阶段，
这是巨大的稳健性差别 —— 那一天 10 次冻屏，在 dext 路线里会是 10 次进程崩溃。

代价同样是实在的：要把现有代码里 IOKit 相关的部分（用户客户端、内存描述符、中断源）搬到
DriverKit 表面（PCIDriverKit + IOUserNetworkEthernet），且需要放宽 SIP/AMFI 一侧的限制
（MelonDMA 明说 Apple 尚未授予其 PCI/UserClient entitlement；本机 boot-args 里已有 `amfi=0x80`）。

**定位（本项目的判断）**：已证可行的**退路**，只有在 kext 路线的冻屏最终修不掉时才值得付重写成本。
kext 路线目前只差"attach 的上下文"这一处，成本 1–2 次重启。

## 3. 社区在 Hackintosh 上让 Mellanox 跑起来的两条经验（引述，非本机实测）

来源：MacRumors 那条 ConnectX-3 / CX 系列长线程（"Mellanox ConnectX-3 40 GbE using ATTO FastFrame
macOS driver"）。以下是**引述**，不是我们在本机的实测：

1. 屏蔽 Apple 的 dext 用 boot-arg **`dk.mlx5=0`**，而不是删掉它；
2. 最终能工作的姿态是：**BIOS 开 VT-d / IOMMU** + OpenCore **取消** `DisableIoMapper` + **勾上**
   `DisableIoMapperMapping` —— 即"**开着 AppleVTD，但打上映射补丁**"。

三个键的准确含义：

* `DisableIoMapper`（OpenCore Kernel → Quirks）：移除/改名 ACPI 的 IVRS(DMAR) 表 ⇒ macOS
  **根本不启用 AppleVTD**，DMA 全部直通。这是 Hackintosh 上为了别的设备（Wi-Fi、雷雳）常用的关法；
* `DisableIoMapperMapping`（OpenCore 0.9.4+）：**保留 AppleVTD**，只对 macOS 的 IOMMU 路径处理不了的
  设备/区域补映射修正；
* 网卡为什么在意：AppleVTD 开着时，驱动递给设备的地址必须**经过 IOMMU 映射**（IOKit 的
  `IODMACommand` 会做这件事）；递物理地址 ⇒ 设备写到别处 ⇒ 表现为"设备不工作"或整机挂。

**与 SR-IOV 无关**：SR-IOV 是"一个物理功能虚拟出多个 VF"，与"DMA 重映射"是两件独立的事，
唯一交集是共用同一块 IOMMU 硬件；**打开 IOMMU ≠ 启用 SR-IOV**，反之亦然。

## 4. 本机实测：当前跑在"IOMMU 关闭 / 直通"姿态，及其两个推论

2026-10-13 在本机实测（`ioreg`）：

* `ioreg -p IODeviceTree | grep -E 'IVRS|DMAR'` ⇒ **空**（ACPI 的 IOMMU 表不在 IODeviceTree 里）；
* `ioreg -p IOService | grep AppleVTD` ⇒ **空**（macOS 没有启用 AppleVTD）；
* `nvram boot-args` 里没有任何 IOMMU 相关开关。

⇒ 本机所有 DMA 都是**直通**（设备看到的就是真实总线地址）。两个推论：

1. 我们至今全部收发验证（513 次发送完成、88 帧接收、读出真帧内容）都是在**直通**姿态下做的；
   驱动用的是 `IODMACommand`，所以**两种姿态都应成立**（直通时给物理地址、AppleVTD 下给映射地址）。
2. **直通姿态下 DMA 写错地址不会被拦住** —— 它会静默改内存，而那正是"整机死、无 panic"这类形状。
   所以将来若出现"设备写坏内存"的怀疑，**把 AppleVTD 打开**本身可以当诊断手段（把静默破坏变成可记录的
   fault）；代价是社区报告里它会带来别的问题，而且那份 OpenCore 配置由别的会话维护
   —— 要不要试由操作者决定，本项目不主动改。
