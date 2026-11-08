任务：判断一个刚测出来的**决定性现象**的可能机理，并给出下一步最小实验。
这是一个把 **BSD 许可** 的网卡驱动从 FreeBSD `sys/dev/mlx4` 移植到 macOS IOKit 的项目，
硬件是 Mellanox ConnectX-3 Pro。

## 允许阅读的路径（**只读这两个目录**）

```
/Users/you/Documents/Hackintosh-MacLocal/tools/mlx4-port/freebsd-src/
/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/
```

## 新的实测事实（真机输出，全部可复现）

固件交接序列是：`reset` → `QUERY_FW` → 分配 FW 区 → `MAP_FA` → `RUN_FW` → `MOD_STAT_CFG`。
`RUN_FW` 此前一直不完成（HCR 的 GO 位永远为 1）。今天出现了一个**对比实验**：

| # | FW 区分配后做了什么 | RUN_FW 结果 | 起作用的可能因素 |
|---|---|---|---|
| A | 主机**逐页写入**了一个已知图案（1542 页全写），**并且**先用其中一页做过一次 `QUERY_FW`（outbox 指向 FW 区第 0 页） | ✅ **完成**，fw status 0；随后 `MOD_STAT_CFG` 也 ok，`QUERY_DEV_CAP` 正常回答 | CPU 碰过每一页 + 设备写过第 0 页 |
| B | 什么都不做（原始序列，同一启动内、同样先 reset） | ❌ **TIMED OUT**，GO 卡住 | 没有任何页被 CPU 碰过 |
| C | 执行中（正在测）：只写图案，不做那次 `QUERY_FW` | 待测 | 用来分辨"CPU 碰过页面"与"设备写过页面" |

其它已知事实：

- FW 区是 1542 个 4 KiB 页（6316032 字节），由 `IOBufferMemoryDescriptor::inTaskWithPhysicalMask`
  分配，然后交给 `IODMACommand::withSpecification(kIODMACommandOutputHost64, 64, 0, kMapped, …)`
  并用 `setMemoryDescriptor(md, true)` 绑定；每页的总线地址由
  `gen64IOVMSegments()` 逐段枚举得到（实测 **386 个段**、完全不连续、跨度 26 GB）。
- `MAP_FA` 在 A、B 两次都成功（1542 条 / 7 批，fw status 0），所以"页表被固件接受"不是区别。
- 把整个 FW 区限制在 4 GiB 以下（`lowmem`）**同样会超时**，所以不是高地址可达性。
- B 那次（未写图案）读回 FW 区：6316032 字节里只有 **10578 字节非零**（即基本是空白内存）。
- A 那次：写入图案后 998‰ 非零；`RUN_FW` 完成后降到 933‰（说明设备确实改动了这块内存）。
- FreeBSD/Linux 参考实现在同一个位置只做 `dma_alloc_coherent()` 然后直接 `MAP_FA`，**没有任何
  主机侧写 `fw_icm` 的代码**（已全文 grep 确认）。
- Linux 在同一台机器上驱动这张卡**完全正常**（`mlx4_en` 双端口 link up）。

## 我要你回答的问题

1. **A 与 B 的差异最可能是什么？** 请给出你对下面这些候选的判断（支持 / 反对 / 未知），
   每条都要说明理由，并标明是"源码可证"还是"机理推断"：
   - **a) 延迟绑定 / 未落地**：macOS 的 DMA 内存页只有在 CPU 真正访问后才被落地（fault in）
     并进入 IOMMU 映射表；B 那次设备访问的是尚未建立映射的地址，DMA 静默失败。
   - **b) 内存描述符未被 wire**：`IOBufferMemoryDescriptor` 的页在未被触碰时处于可换出/未 wire
     状态，`IODMACommand` 枚举出的总线地址在设备看来无效。
   - **c) 缓存/一致性**：CPU 未写过的行从未进入缓存，设备的 DMA 写入因一致性问题表现异常。
   - **d) 固件真的需要 FW 区里有内容**（例如需要非零数据才肯启动），与主机内存管理无关。
   - **e) 其它**：你可以提出更符合证据的机理。
2. **用现象区分 a/b 与 d 的判据**：B 那次 FW 区有 10578 字节非零却仍超时；A 次几乎全是图案
   才成功。这个差别更支持哪一类？如果支持"必须先落地"，那"落地"到底需要什么条件
   （全页？有映射就够？只碰一页行不行？）
3. **在 IOKit 里正确的做法是什么**（若结论是 a/b）：`IOBufferMemoryDescriptor`、
   `IOMemoryDescriptor::prepare()`、`IODMACommand::setMemoryDescriptor` 的
   `synchronize` 参数、`IODMACommand::prepare()`、`kIODirection*` 选择，
   以及**多段（386 段）大块分配**是否应该改用其它分配路径。请指出当前
   `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Mlx4Hw.cpp` 里
   `icmAlloc()` 与 `mboxAlloc()` 的做法与你的建议差在哪（给行号）。
4. **最小验证实验**：只允许使用现有能力（读/写 BAR0 寄存器、发 HCR 命令、分配 DMA 内存、
   在主机侧读写这块内存）。请给出**能区分 3 个以上假设**的、代价最小的一组实验，并说明
   每个结果该怎么解释。
5. **风险**：如果结论是"必须先落地/必须先 wire"，那么在真正的驱动里，FW 区应当在什么时机
   被落地？会不会有"落地了但设备仍不可达"的情形被这个实验掩盖？

## 格式

用中文。结构化（表格 + 有序列表）。**每个源码论断给 `文件:行号`**，源码查不到的写「未知」。
严格区分「源码确证」「实测确证」「机理推断」。不要写可编译代码。
