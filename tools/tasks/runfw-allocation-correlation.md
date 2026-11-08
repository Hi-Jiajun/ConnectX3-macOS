任务：解释一个已经测得很干净、但机理不明的相关性，并给出最小验证实验。
项目：把 **BSD 许可**的网卡驱动从 FreeBSD `sys/dev/mlx4` 移植到 macOS IOKit，
硬件 Mellanox ConnectX-3 Pro。

## 允许阅读的路径（**只读这两个目录**）

```
/Users/you/Documents/Hackintosh-MacLocal/tools/mlx4-port/freebsd-src/
/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/
```

## 现象：9 次真机运行的完整记录（同一台机器、同一次启动内前 7 次）

固件交接序列：`reset` → `QUERY_FW` → 分配 FW 区 → `MAP_FA` → `RUN_FW` → `MOD_STAT_CFG`。
关键命令是 `RUN_FW`（opcode 0xff6，无参数）——它让卡上的固件从 FW 区开始运行。

| # | 在 `fwload` 之前是否做过一次 `icmalloc`（且已释放） | FW 区首个总线地址 | RUN_FW |
|---|---|---|---|
| 1 | 否 | 0x4a23fc000（全部 ≥4 GiB） | ❌ 超时 |
| 2 | 否（`lowmem`，强制 <4 GiB） | 0x2c7c8000（连续、单段） | ❌ 超时 |
| 3 | 否 | 0x3fce44000 | ❌ 超时 |
| 4 | 否 | 0x1db1c0000 | ❌ 超时 |
| 5 | 否 | 0x4245a7000 | ❌ 超时 |
| 6 | **是**（1542 页，同尺寸） | 0x3d2ffc000 | ✅ 完成 |
| 7 | **是**（1542 页，同尺寸） | 0x3d2ffc000 | ✅ 完成 |
| 8 | **是**（**16 页**，尺寸不同） | 0x44d5a0000 | ✅ 完成 |
| 9 | **是**（1542 页，同尺寸） | 0x51e9fc000 | ✅ 完成 |

**5 次"没先分配"全部失败，4 次"先分配过"全部成功**（含一次只有 64 KiB 的预分配）。

## 为什么这条相关性值得深究

1. **`icmalloc` 完全不访问设备。** 它只做：`IOBufferMemoryDescriptor::inTaskWithPhysicalMask`
   → `IODMACommand::withSpecification(kIODMACommandOutputHost64, 64, 0, kMapped, bytes, 1)`
   → `setMemoryDescriptor(md, true)` → 反复 `gen64IOVMSegments()` 记录每页总线地址
   → `IOFree`/`release`。**没有任何 MMIO 读写、没有命令**。
   ⇒ 因此"先分配过"这一点**不可能改变设备侧状态**，差异只能在主机侧：FW 区最终落在
   **哪些物理页 / 哪些总线地址**上。
2. **已被排除的因素**（都已做单变量对照）：
   - 不是"CPU 有没有碰过内存"：`fill` 逐页写满 1542 页后仍然超时。
   - 不是"设备有没有往这块内存写过"：先用 FW 区的一页做 `QUERY_FW` 的 outbox
     （设备确实写进去了，读回值正确）也**不是**必要条件（第 8 次成功就没有这一步）。
   - 不是"地址太高"：强制 <4 GiB 且完全连续的单段分配，同样超时。
   - 不是"FW 区大小不对"：页数是参考的 1542（从 outbox 偏移 0x00 读，与 FreeBSD
     `mlx4_core/mlx4_fw.c:1620/:1691` 和 mainline Linux `fw.c:1638/:1709` 一致）。
   - 不是"内容必须非零"：第 7、9 次成功时面积非零率只有 143‰；第 3 次失败时是 1‰。
3. **成功时设备确实动了这块内存**：RUN_FW 完成后 FW 区约 900 KB 变成非零（第 6/7/8/9 次
   都是 143‰ 左右），看起来像是固件镜像被 DMA 进了 FW 区；失败时几乎全零（1‰）。
   ⇒ 失败的表现是"固件根本没有开始加载"，而不是"加载了一半"。
4. **Linux 在同一台机器上驱动这张卡完全正常**（`mlx4_en` 双端口 link up），
   而参考实现只是 `dma_alloc_coherent`/普通页 + `pci_map_sg` 之后直接 `MAP_FA → RUN_FW`，
   **没有任何主机侧写 FW 区的代码**（已全文 grep）。

## 已经查过的源码结论（不必重复）

- `mlx4_cmd_init()` **不写任何 HCR 寄存器**（只初始化软件对象、映射、邮箱池），
  所以"跳过它"不构成解释；`cmdPending()` 与参考逐位等价；
  `reset` 除硬件信号量外与参考逐句一致。
- 现在的 `Mlx4Hw::icmAlloc()` 与 `Mlx4Hw::mboxAlloc()` 都**没有**显式调用
  `IOMemoryDescriptor::prepare()` 或 `IODMACommand::prepare()`；
  `setMemoryDescriptor(md, true)` 的第二个参数是否等于 prepare，**未确证**。

## 我要你回答的问题

1. **哪一类机理能同时解释"第一次分配失败、之后任何一次分配都成功"？**
   请逐个判断支持/反对/未知，并说明是源码可证还是机理推断：
   - a) 首次大块 `IOBufferMemoryDescriptor` 拿到的物理页位于某种设备不可达的区域
     （例如尚未被 IOMapper 覆盖、或与固件保留区重叠），而分配器在**释放过一次之后**
     会把后续分配放到别处；
   - b) IOMMU/DART 映射是**首次使用时按需建立**的，第一次给出的是尚未建表的地址，
     而释放后再分配会复用已建表的条目；
   - c) 与 `IODMACommand` 的**对象生命周期**有关（第一次创建/销毁后，
     后续创建的 DMA command 行为不同）；
   - d) 与**时间**有关（多一次分配-释放引入了若干毫秒），而不是与内存位置有关；
   - e) 其它，请提出并说明它能解释哪几行数据、不能解释哪几行。
2. **如何用现有能力区分它们？** 现有能力只有：读写任意 BAR0 寄存器、发 HCR 命令、
   分配/释放任意页数的 DMA 内存、在主机侧读写这块内存。
   请给出**代价最小、且能一次区分三个以上候选**的实验设计，并写清每种结果的含义。
   注意：一次失败的 `RUN_FW` 会让命令寄存器 GO 位卡住，必须再做一次 `reset` 才能继续
   （实测 reset 能恢复，所以不需要重启机器）。一次失败要等 60 秒超时。
3. **在 IOKit 里正确的姿势是什么？** 具体到 `src/Mlx4Hw.cpp` 的 `icmAlloc()`
   与 `mboxAlloc()`（请给行号）：是否需要 `prepare()`、是否该换分配方式
   （例如 `IOMemoryDescriptor::withAddressRange`、`IODMACommand::withSpecification`
   的 segment 参数、`kIODMACommandOutputHost64` vs 其它输出模式）、
   是否该在交付地址前显式同步。请明确区分"源码可证的 API 契约"与"你根据文档的推断"。
4. **这条相关性会不会是巧合？** 以你对该平台的了解判断：5/5 与 4/4 的分裂
   在统计上意味着什么，还需要多少次重复才能定论；有没有更便宜的确定性判据。
5. **风险提示**：如果机理是"首次分配不可用"，那么 M3 阶段（EQ/CQ/QP/MTT 大量分配）
   会遇到什么？有没有办法在驱动里做成健壮（例如分配后先验证、或强制某种分配路径）？

## 格式

用中文。结构化（表格 + 有序列表）。每个源码论断给 `文件:行号`；查不到的写「未知」。
严格区分「源码确证」「实测确证」「机理推断」。不要写可编译代码。
