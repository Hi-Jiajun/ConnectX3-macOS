任务：审查一段 macOS 内核扩展（kext）代码，找出其中的 bug。

## 背景

这是一个把 **BSD 许可** 的网卡驱动从 FreeBSD `sys/dev/mlx4` 移植到 macOS IOKit 的项目。
当前阶段（M2b）要让卡上固件跑起来：分配固件要用的内存 → 把内存交给固件 → 启动固件。

**已知的失败现象**（真实发生，不是假设）：

```
map op 0xfff: 5123 页 / 20983808 字节 / 21 批，first bus 0x915961000   ← 成功
command 0xff6 timed out (go bit not cleared)                          ← RUN_FW 挂住
HCR status = 0x00a00ff6  →  opcode 字段 = 0xff6，GO 位仍为 1
```

也就是说：把内存页面交给固件的命令（`MAP_FA`，opcode 0xfff）**成功**了，
紧接的启动命令（`RUN_FW`，opcode 0xff6）**发出后固件从不回应**。
之后命令接口就死锁了（再发任何命令都超时）。

参考实现（FreeBSD）在同样硬件上是**秒级完成**这两步的。

## 要审查的文件（**只读这两个目录，不要读其它任何目录**）

```
/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/            ← 我的移植代码（主要看这个）
/Users/you/Documents/Hackintosh-MacLocal/tools/mlx4-port/freebsd-src/   ← FreeBSD 原始源码（作为对照）
```

需要重点对照的两个函数：
- 我的：`src/Mlx4Hw.cpp` 里的 `mapCmd()`、`allocMailbox()`、`cmdBox()`、`icmAlloc()`
- 参考：`mlx4_core/mlx4_fw.c` 的 `mlx4_map_cmd()`（约 1501 行）、
  `mlx4_core/mlx4_cmd.c` 的 `mlx4_cmd_post()`（约 432 行）与 `mlx4_cmd_poll()`

## 请回答

### 1. 逐条对照 `mapCmd` 与 `mlx4_map_cmd`

把两边**逐字段**列成表对照：邮箱每一条目的布局与字节序、批次大小、
`in_param` / `in_modifier` / `op_modifier` 各填什么、命令的调用形态。
**任何差异都要指出**，并判断它是否可能解释"MAP_FA 成功但 RUN_FW 挂住"。

特别注意我做的这个简化是否安全，请给出明确判断：

> 参考实现对每个 scatterlist 条目计算 `lg = ffs(addr | size) - 1`，
> 用 `(lg - 12)` 作为对齐编码，从而一条目可描述一整个对齐块；
> 我改成**每 4 KiB 页一条目、对齐编码恒为 0**。

### 2. 逐条对照 `cmdBox` 与 `mlx4_cmd_post` / `mlx4_cmd_poll`

重点检查：
- HCR 的 7 个 dword 字段顺序、每字段的宽度与字节序、token 的位置
- **toggle 位的语义**：参考里 `cmd_pending()` 用 `(cmd->toggle == !!(status & T_BIT))` 判断
  "命令仍在途"，请确认我的实现与之一致
- GO 位的等待与轮询循环
- 状态码的提取位置（高位字节）
- **`out_param` 的写入时机**：参考是先写 in_param/in_modifier/out_param 再置 GO 位，确认我的顺序

### 3. 检查 `icmAlloc` 的单页总线地址推导

它从一个段的起始总线地址按 `seg.fIOVMAddr + (k << 12)` 推导每页地址。
请判断这在什么情况下**会算错**，以及代码里的检查是否足够。

### 4. 给出你对「RUN_FW 为何不回应」的**排序假设**

列出你认为最可能的原因（按可能性排序），**每条给出源码或硬件依据**，
并说明**如何用最小代价验证**（例如改一个参数、加一条日志、换一次实验）。

### 5. 明确列出未知

哪些地方**无法从源码判断**、必须实测，不要猜。

## 格式

用中文。**只报告发现，不要改任何文件。**
每个论断给出 `文件:行号`。如果某处你看不出问题，就明确说"未发现差异"——
不要把"没找到"包装成"看起来没问题"。
