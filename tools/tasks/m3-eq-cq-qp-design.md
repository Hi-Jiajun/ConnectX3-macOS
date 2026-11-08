任务：为一个 macOS 内核扩展（kext）设计 M3 阶段的移植方案。

## 这是什么项目

把一个 **BSD 许可**（OpenIB.org BSD 分支）的网卡驱动，从 FreeBSD 的
`sys/dev/mlx4` 移植到 macOS 的 IOKit 驱动框架。硬件是 Mellanox ConnectX-3 Pro。

## 已经完成的部分

- kext 能匹配 PCI 设备、映射 BAR0/BAR2、读写寄存器、通过 HCR 命令接口与卡上固件通信
- 已实现并跑通：`QUERY_FW`、固件区分配、`MAP_FA`、`RUN_FW`、`MOD_STAT_CFG`、
  一个可分页 DMA 内存分配器（记录每页的总线地址）、`MAP_ICM` 的邮箱编码
- 命令超时、DMA 低 4 GiB 约束、只读探测等基础设施已在

## 你要设计的阶段（M3）

把 **EQ（事件队列）、CQ（完成队列）、QP（队列对）** 建立起来，让中断能工作，
端口切换到以太网模式，链路 up。这是整个移植里最难的一段。

## 允许阅读的路径（**只读这两个目录，不要读其它任何目录**）

```
/Users/you/Documents/Hackintosh-MacLocal/tools/mlx4-port/freebsd-src/   ← FreeBSD 原始源码（41 个 .c/.h）
/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/            ← 我当前的移植代码
```

## 产出要求

一份设计文档（不要写可编译的代码）。**每个论断必须给出 `文件:行号` 出处**；
源码里查不到的写"**未知**"，不要推测。

### 1. EQ 建立流程

读 `mlx4_core/mlx4_eq.c`。按顺序列出建立一个 EQ 必须做的每一步：
内存分配（大小怎么算）、内容怎么初始化、`SW2HW_EQ` 命令的形态、
UAR 页怎么用、`ARM` 寄存器怎么写、门铃地址怎么算。
逐项说明**每个字段/参数从哪里来**（由哪个结构体或哪次查询给出）。
特别指出：**EQ 的初始化顺序里，哪些步骤之间有硬性先后依赖**。

### 2. 中断模型（这一段最重要）

FreeBSD 侧用 `taskqueue`（约 10 处调用）和 `callout_reset`（约 2 处）。
请列出：

- EQ 相关的中断处理路径：中断顶半部做什么、底半部（taskqueue）做什么、
  `mlx4_eq_int`（或等价的处理函数）内部的分支逻辑是什么
- 事件类型与 EQ 编号的关系（哪些 EQ 是异步事件、哪些是命令完成、哪些是收发完成）
- **macOS 侧对应的原语**：`IOInterruptEventSource` / `IOFilterInterruptEventSource` /
  `IOWorkLoop` / `IOCommandGate` 各自适合承担这里的哪一部分，为什么
- MSI-X 在 IOKit 里怎么申请：`IOService::getInterruptType`、
  `registerInterrupt`、`IOInterruptEventSource` 三者的分工，
  以及**多向量中断**（我们的卡支持多个 MSI-X 向量）该怎么组织

### 3. CQ 建立流程

读 `mlx4_core/mlx4_cq.c`，同样列出建立步骤与 `SW2HW_CQ` 的形态。
说明 CQ 与 EQ 在建立顺序上的依赖关系。

### 4. QP 建立与 UAR / 门铃

读 `mlx4_core/mlx4_qp.c`、`doorbell.h`、`qp.h`：

- 门铃地址（UAR page）怎么算出来
- `MLX4_GET_DOORBELL_LOCK`（`doorbell.h`）在 FreeBSD 上是什么，
  macOS 的单内核地址空间下**还需不需要**这个锁，为什么
- QP 建立的最小集合：以太网收发最少需要哪些 QP 及其类型
- `rqpn` / `rqn` / `qpn` 这些编号的层次关系

### 5. 移植清单表

一张表：要搬哪些函数（列函数名与所在文件）、各多少行、依赖哪些现在还没有的原语、
macOS 侧必须改写什么、能不能原样搬。

### 6. 风险与未知

- 最容易出错的地方（给出源码依据）
- 哪些**必须实测确认**、不能从源码推断
- 你对整体工作量的估计，以及理由

## 格式

用中文。结构化（标题 + 表格 + 有序列表）。**不要写实现代码**，只要设计。
