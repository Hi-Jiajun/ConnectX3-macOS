任务：为一个已经跑通的移植**设计 M3 的实现计划**（不是可行性分析，不是代码）。
项目：把 **BSD 许可**（OpenIB.org BSD 分支）的网卡驱动从 FreeBSD `sys/dev/mlx4` 移植到
macOS IOKit，硬件 Mellanox ConnectX-3 Pro（`15b3:1007`，双端口）。

## 允许阅读的路径（**只读这两个目录，不要读其它任何目录**）

```
/Users/you/Documents/Hackintosh-MacLocal/tools/mlx4-port/freebsd-src/
/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/
```

## 已经跑通的（全部是真机实测，不是推断）

| 阶段 | 内容 | 实测结果 |
| --- | --- | --- |
| M1 | kext 匹配设备、映射 BAR0/BAR2、HCR 命令寄存器握手 | `QUERY_FW` 返回 `fw_ver 2.42.5000`、`cmd_if_rev 3`、`max_cmds 16` |
| M2b | 分配 FW 区 → `MAP_FA` → `RUN_FW` → `MOD_STAT_CFG` | 全部 fw status 0 |
| M2c | `QUERY_DEV_CAP` | fw status 0 |
| **M2d** | `SET_ICM_SIZE` → `MAP_ICM_AUX` → 14 张表 `MAP_ICM` → `INIT_HCA` | **全部 fw status 0**；aux 2082 页；14/14 表已映射 |

### `INIT_HCA` 实际写入的 inbox（实测日志，可直接引用）

```
version 2 cacheline 64 flags 0x00002019 eqe/cqe 0x60000000
qpc 0x100080000/10  srqc 0x100178000/6  cqc 0x100170000/8  eqc 0x10017a000/31
sys_eqs 1024  dmpt 0x100140000  mtt 0x1000c0000  cmpt 0x0
mc 0x100100000/10/7/8   uar page 0 log 10
```

### 设备能力（实测解码，`QUERY_DEV_CAP` 原始 outbox 见 `docs/M2B-NOTES.md`）

| 项 | 值 |
| --- | --- |
| 端口数 | 2 |
| QPs | max 2^24 空间，本驱动 profile 申请 2^10 = **1024**（`qpc_entry_sz` 256） |
| CQs | profile 申请 2^8 = **256**（`cqc_entry_sz` 128） |
| SRQs | profile 申请 2^6 = **64**（`srq_entry_sz` 128） |
| EQs | **`num_sys_eqs = 1024`**（有 SYS_EQS 位）；profile 区域按 `min(max_eqs=512, MAX_MSIX=64)` = **64** 预留（`eqc_entry_sz` 128）；表映射按 1024 |
| reserved | qps 64, cqs 128, srqs 64, eqs 8, mtts 64, mrws 256, mgms 1 |
| UAR | `uar_size` = 4 MiB，`reserved_uars` = 2；BAR2 已映射（8 MiB） |
| `min_page_sz` | 512 字节 |
| entry sizes | qpc 256, cqc 128, eqc 128, srq 128, dmpt 64, cmpt 64, rdmarc 32, **aux 1**, altc 64, mtt 8 |
| flags | `ext_flags 0xeeff1f9d` / `flags 0xf677fbef`，**64B EQE 与 64B CQE 位都置位** |
| flags2 | `0x120429`（含 SYS_EQS、TS、RSS） |
| max GSO | 128 KiB，max msg 1 GiB，max MTU width 47 |

### 现有代码基础（`src/` 里已经有的东西）

- `Mlx4Hw::cmdBox()` / `cmdBoxImm()`：HCR 命令，带超时、串行锁、immediate 读回
- `Mlx4Hw::icmAlloc()` / `icmFree()` / `mapIcm()` / `mapFa()`：分页 DMA 分配 + 每页总线地址
- `Mlx4Hw::parseDevCap()` / `mlx4MakeProfile()`（在 `src/mlx4_pure.h`，纯算术、可在主机测）
- `Mlx4Hw::initHca()` / `setIcmSize()` / `mapIcmAux()`
- `Cx3Mlx4::initIcmTables()` / `freeIcmTables()`：14 张表的映射与释放
- 设备写入**只能**从用户态的 `cx3ctl` 触发（`start()` 被构建期检查强制只读）

## 我要的计划

**不要写可编译代码**，要一份可执行的分步计划。每步给出：要做什么、涉及参考里的哪个函数
（`文件:行号`）、依赖前面哪一步、以及**怎么验证这一步成功了**（我们只有"读寄存器 / 发 HCR
命令 / 分配 DMA 内存"这三种能力，没有中断还不算能收到事件）。

### 1. 第一件事：把 EQ 建起来并**看见它工作**

在有中断之前，"EQ 建好了"能观测到什么？请给出具体的判据（例如 `SW2HW_EQ` 的返回值、
EQ 上下文里状态位、ARM 之后某个寄存器的读数）。**这一条最重要**：请说明我们怎么在不依赖
中断的前提下证明 EQ 是活的（可以用轮询 EQ 里的 EQE 吗？轮询哪个地址？）。

### 2. 中断：MSI-X 的申请顺序与最小可用集合

- IOKit 侧：`IOService::getInterruptType` / `registerInterrupt` /
  `IOInterruptEventSource` / `IOWorkLoop` 各自的职责，以及**第一次只用 1 个向量**
  是否可行、怎么把 EQ 的 `intr` 字段与向量对上。
- 参考里 EQ 的 `intr` 字段从哪来（`mlx4_eq.c`），MSI-X 向量号与 EQ 编号的关系。
- 我们**没有**实现的部分：目前 `Mlx4Hw::initHca()` 只在 inbox 里写了 `num_sys_eqs` 与
  `log_num_eqs`，没有申请任何 MSI-X。请指出顺序上的坑（先注册中断还是先建 EQ？）。

### 3. CQ 与 QP：以太网收发最少需要哪几个

- 读 `mlx4_cq.c` / `mlx4_qp.c`：建立一个 CQ 与一个以太网 QP 的最小调用序列（`SW2HW_CQ`、
  `RST2INIT_QP`、`INIT2RTR_QP`、`RTR2RTS_QP` 等），以及每个命令的邮箱形态。
- **门铃 / UAR**：`doorbell.h` 里发送门铃的地址怎么从 QP 编号算出；我们的 BAR2 已映射为
  一个平坦的 8 MiB 映射（`Cx3Mlx4` 里 `fReg2`），是否足够，需不需要按 UAR 页再映射。
- QP0/特殊 QP（比如 RAW 以太网 QP）在 `mlx4_en` 的发送路径里是怎么用的。

### 4. 端口切以太网 + 链路 up

- `mlx4_en` 里把端口设成以太网模式、设 MTU、读 MAC、打开端口的那串命令
  （`SET_PORT`、`QUERY_PORT` 等）与顺序。
- 我们缺的能力：`SET_PORT` 需要一个 inbox，我们现在只有 outbox 路径（`cmdBox` 的
  `inParam` 是 in_param 不是 inbox 地址）。请指出要补什么。

### 5. 建议的提交顺序（每个提交一次重启可验证）

macOS 上换 kext 二进制要重启，所以**"一次重启能验证多少"是关键约束**。
请把 M3 切成若干次重启，每次给出：这次新增哪些 selector / 命令、这次能观测到什么、
以及这次如果失败最可能是什么。

### 6. 风险与未知

- 哪些地方最可能第一次就失败（按可能性排序）
- 哪些**只能实测**、不能从源码推断
- 工作量估计（按"重启次数"和"新增代码行数"两个口径）

## 格式

用中文。结构化（标题 + 表格 + 有序列表）。每个源码论断给 `文件:行号`；查不到的写「未知」。
严格区分「源码确证」「实测确证」「推断」。不要写可编译代码。
