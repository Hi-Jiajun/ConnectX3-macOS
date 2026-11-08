任务：一次**由中断引起的系统卡死**已经发生，需要判断机理、给出正确的中断方案，并审查我的修复方向。
项目：把 **BSD 许可**（OpenIB.org BSD 分支）的网卡驱动从 FreeBSD `sys/dev/mlx4`
移植到 macOS IOKit。硬件 Mellanox ConnectX-3 Pro（`15b3:1007`，双端口），
目标机 macOS 15.8.1 x86_64。前序阶段已通过：固件启动、`INIT_HCA`、14 张上下文内存表、
以及**用轮询**证明事件队列（EQ）的 DMA 链路可用（事件模式 NOP 的完成能作为 CMD EQE 出现）。

## 允许阅读的路径（**只读这两个目录**）

```
/Users/you/Documents/Hackintosh-MacLocal/tools/mlx4-port/freebsd-src/
/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/
```

## 我做错了什么（事实，不是推测）

为了给 EQ 接中断，我加了一个 selector `m3irqup`，它做：

1. `fPci->getInterruptType(source, &type)` — 返回 **rc=0x0, type=0x1**（即 legacy，**不是**
   `kIOInterruptTypePCIMessaged`）
2. `IOWorkLoop::workLoop()` → `IOInterruptEventSource::interruptEventSource(this, action, fPci, source)`
   → `addEventSource()` → **`enable()`**
3. action 里：取队列锁、按序消费 EQE、记录匹配、推进 consumer index、写 EQ 门铃重新武装

**我没有清除中断**——参考的 legacy INTx 处理函数第一件事是写中断清除寄存器
（`mlx4_interrupt()` 里的 `writel(priv->eq_table.clr_mask, priv->eq_table.clr_int)`），
而 MSI-X 处理函数不写。我照 MSI-X 的形状写了一个 legacy 源。

**现场数据（真机）：**

```
m3irqtest 0x1234 → 完成，m3irqstatus: interrupts 2, EQEs 1, matched YES ← 第一个事件确实通过中断送达了
空闲 6 秒不投递命令          → interrupts 仍为 2（没有无条件风暴）
紧接着三次测试：
  token 0xabcd → interrupts 200, EQEs 1, matched no
  token 0x55aa → interrupts 215, EQEs 3, matched YES
  token 0x9999 → interrupts 215, EQEs 3, matched no
内核日志反复出现：  irq action: 0 EQE(s) consumed, consumer index now 1, irq count 77/78/79...
随后系统卡死 → panic:  waitq lock timeout after 1196862575 ticks @waitq.c:695
                 Panicked task: WindowServer   （回溯全在 mach IPC，无 Cx3Mlx4 符号）
```

**我读出的机理**：电平触发的中断线一直被拉着（没有清除），所以 handler 被反复调用；
每次调用消费 0 个 EQE，于是它只是空转，直到系统被拖死。**这是我的实现缺陷，不是硬件问题。**

## 配置空间实测（我自己 walk 出来的，请核对解读）

capability 链：`PM(0x40) → VPD(0x48) → MSI-X(0x9c) → PCIe(0x60) → VSEC(0xc0) → end`

| 项 | 实测值 |
| --- | --- |
| MSI-X 能力位置 | `0x9c`，ID 0x11，next 0x60 |
| MSI-X Message Control | `0x007f` ⇒ **Table Size = 0x7f = 128 个向量**；**bit15（MSI-X Enable）= 0，即未启用** |
| MSI-X Table | BIR = 0（BAR0），offset = `0x7c000` |
| MSI-X PBA | BIR = 0（BAR0），offset = `0x7d000` |
| MSI（0x05）能力 | **不存在**（链上只有 MSI-X） |
| BAR0 | `0xdc200000`（64 位、非预取） |
| BAR2 | `0x0c86000000`（64 位、预取） |
| 子系统 | vendor `15b3`，id `0003` |

## 我的实现约束（很重要）

- 这台机器上，**kext 的新二进制必须重启才生效**，所以"一次重启能验证多少"是关键约束。
- 构建期有三道闸：符号核对（`kmutil libraries`）、EQE/offset 偏移核对（146 项）、
  以及"`start()` 不得可达任何写设备/分配 DMA 的路径"的调用图检查（`start()` 必须只读）。
- **MacKernelSDK 的 `IOPCIDevice.h` 里没有 `configureInterrupts` 声明**（我 grep 过），
  只有 `kIOInterruptTypePCIMessaged = 0x00010000` 与 `IOPCIMessagedInterruptController` 前向声明。
  真实 IOPCIFamily 是否提供该符号、以什么签名提供，**我不知道**。

## 我要你回答的问题

### A. 机理确认

1. 我的"电平 INTx 未清除 ⇒ 反复触发 ⇒ 系统卡死"这个结论**对不对**？如果对，为什么
   **空闲时计数不涨**（不投递命令时 interrupts 停在 2），而一投递命令就跳到 200+？
   （我怀疑与"命令完成会再次拉起中断线"有关，但请从源码/机制给出解释。）
2. 参考实现的清除逻辑逐字是什么（`文件:行号`）？`clr_mask` 与 `clr_int` 分别从哪来
   （`QUERY_FW` 的哪些字段、PCI 配置的哪个寄存器）？`mlx4_map_clr_int` 做了什么？
3. 如果不清除中断，参考的 legacy 路径会发生什么？它有没有别的防护（例如禁止未清除就返回）？

### B. 正确的下一步：MSI-X 还是清 INTx

4. **对这张卡（CX-3，有 128 向量的 MSI-X、但没有 MSI），在 macOS IOKit kext 里应该用哪条路？**
   请给出理由与优先级。若你主张 MSI-X，请说明 **IOKit 侧到底怎么让 IOPCIFamily 启用 MSI-X**
   （`configureInterrupts` 的正确签名、调用时机、需要在 `start()` 还是更早、
   以及它与 `registerInterrupt`/`IOInterruptEventSource` 的关系）。**如果这属于你无法确证的内容，
   请明确写"未知"，不要编造签名。**
5. 若走 MSI-X：EQ context 的 `intr` 字段该填什么？参考把**哪个**东西填进 `intr`
   （MSI-X table entry 号？软件 IRQ？），以及在单向量场景下它应该是几？（`文件:行号`）
6. 若走 INTx 清除：正确的清除序列是什么？清除放在 action 的哪个位置
   （消费之前还是之后）？`clr_mask` 怎么算（我看到参考用 `swab32(1 << (inta_pin & 31))`，
   请确认并给出处）？

### C. 审查我要加的防护（我认为无论如何都该有）

7. **风暴防护**：我打算在 action 里统计"连续 N 次调用消费 0 个 EQE"，达到阈值就
   `disable()` 该 event source 并打印醒目日志。这个做法在你看来是否合理？
   阈值取多少？有没有更好/更标准的做法（例如 IOKit 本身有类似机制）？
8. **默认拒绝非 messaged 源**：我打算让 `m3irqup` 在 `getInterruptType` 返回非
   `kIOInterruptTypePCIMessaged` 时**默认拒绝**，必须显式 force 才允许——这样"误开 INTx
   导致卡死"不可能再意外发生。你认为这个默认值对吗？
9. 参考在**顶半部/底半部**的分工上，有没有我该模仿但还没模仿的东西？
   （已知：`mlx4_msi_x_interrupt` 直接调 `mlx4_eq_int`；只有需要发固件命令的工作才延后。
   请补充我漏掉的细节，例如中断使能/屏蔽、`synchronize_irq`、以及释放顺序。）

### D. 还有哪些我没想到的

10. 这次事故暴露了我对"中断生命周期"的理解缺口。请列出**在 macOS IOKit 上做这块时
    必须遵守的检查清单**（注册、使能、屏蔽、清除、关闭、与 DMA 释放的先后），
    并标明哪些是你能从源码确证的、哪些是 IOKit 侧你需要标注"未知"的。

## 格式

用中文。结构化（表格 + 有序列表）。每个源码论断给 `文件:行号`；查不到的写「未知」。
严格区分「源码确证」「实测确证」「推断」。不要写可编译代码。
