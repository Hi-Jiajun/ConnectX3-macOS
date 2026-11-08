# 事务锁与命令超时的定稿设计（0.44.0 候选）

本文的行号以 **HEAD `0c7a1d8`**（`kext/Info.plist` = 0.43.0）为准。§1.4 之后的
`src/Cx3Mlx4.cpp` 行号是在该提交上重新取的；`src/Mlx4Hw.cpp`、`Mlx4Hw.h`、
`Cx3Mlx4UserClient.cpp`、`mlx4_regs.h`、`cx3_abi.h` 自 0.42.0 起未再改动。

这一份只做设计与评审，不落地代码。范围限定在：`transactionEnter/Exit` 的作用域、
命令超时的上界、以及"一个卡住的 selector 让 `reset` 不可达"这个失败模式。

**结论先行**：采用候选 2 + 候选 3 的合并形态 —— 超时改成**真实墙钟有界**，
并且**第一次放弃就把设备置为"命令已作废"的毒化态，之后所有命令立即失败返回**；
候选 1（`reset` 不取锁旁路）**不采纳**，因为它的危险不在邮箱，而在 `fTables[]`
这个被两个线程同时改的数组（下面是源码级推演）。
另外补第三条改动：`freeIcmTables()` 现在**解映射失败也照样释放内存**，
这条必须一起改，否则上面两条只是把坏状态来得更早。

---

## 1. 现状的精确描述

### 1.1 两把锁，各自保护什么

| 锁 | 定义 | 取/放的位置 | 保护区 |
| --- | --- | --- | --- |
| `fXactLock`（事务锁） | `src/Cx3Mlx4.h:295`；`src/Cx3Mlx4.cpp:2041` / `:2047` | `src/Cx3Mlx4UserClient.cpp:148` / `:150`，包住 `super::externalMethod` | **一个 selector 调用整体**：分发表解析、参数校验、handler、输出拷贝 |
| `fCmdLock`（命令锁） | `src/Mlx4Hw.h:714` | `src/Mlx4Hw.cpp:472-476`（`cmdBox`）、`:497-507`（`cmdBoxImm`）、`:1752-1768`（`cmdBoxEvent`） | 一次 HCR 命令：post → 等 → 读状态，含**投递前的忙等** |

事务锁保护的是**共享的**这些东西，而不是某一处变量：

- 唯一的命令邮箱 `fMboxVirt` / `fMboxPhys`（`Mlx4Hw.cpp:678-712` 分配，4 KiB，低 4 GiB）；
- HCR 寄存器块与 T 位 `fToggle`（`Mlx4Hw.cpp:572-592` 写入，`:434-441` 判忙）；
- **ICM 表状态**：`fTables[]`、`fNumTables`、`fIcmMapped`、`fMrState` 与各队列指针
  （`Cx3Mlx4.h:314-315`；改它们的是 `freeIcmTables()` 与 `exportM2dProfile()`）。

最后一条是关键，`§37.4` 的描述里没有它。

### 1.2 谁取事务锁

**全部 33 个 selector**：`kCx3SelCount = 33`（`src/cx3_abi.h:35-70`），
`sMethods[]`（`Cx3Mlx4UserClient.cpp:25-71`）逐条对应。

其中至少这五个**根本不碰邮箱、也不碰别人正在改的状态**，却一样要排队：

| selector | 编号 | 它做什么 |
| --- | --- | --- |
| `probe` | 0 | 读寄存器 + 读门控位（`cx3_abi.h:78`） |
| `peek` | 4 | 读任意寄存器区间，有边界校验（`Cx3Mlx4.cpp:423-475`） |
| `m3irqstatus` | 21 | 读中断计数（`Cx3Mlx4.cpp:2924`） |
| `m3irqdown` | 22 | 释放中断对象，**不发命令**（`Cx3Mlx4.cpp:2975-3001`；结尾调 `irqReleaseObjects()`，`newUserClient` 从 `:3003` 起） |
| `survey` | 32 | 状态快照；注释自己写了 "no command is sent ... this has to be instant"（`Cx3Mlx4.cpp:1667-1675`） |

`reset`（3）也是邮箱无关的：`exportResetHca()`（`Cx3Mlx4.cpp:322-410`）只做
① 读 ownership 字、② `fHw.resetHca()`（写复位寄存器 + 恢复 PCI 头，`Mlx4Hw.cpp:347-427`）、
③ 调用 `teardownEq/cqTeardown/rqTeardown/mrTeardown`（`:391-393`），而这四个在
`hcaWasReset=true` 时**全部跳过 HW2SW_\* 命令**：守卫分别在 `teardownEq`
（`Cx3Mlx4.cpp:3870-3871`）、`cqTeardown`（`:3749-3752`）、`mrTeardown`（`:3800`）；
`rqTeardown`（`:1331-1345`）是纯内存操作。

也就是说：**`reset` 不需要邮箱，它唯一会被挡住的原因就是这把为邮箱而设的锁。**
这一条是把问题说清楚的关键，也是候选 3"毒化"能替代候选 1"旁路"的原因。

### 1.3 临界区里最长的等待，以及超时值从哪来

两个等待循环，而且**两个的"毫秒"都不是毫秒**：

| 位置 | 常量 | 名义值 | 实际单位 | 实测换算 |
| --- | --- | --- | --- | --- |
| 投递前忙等 `Mlx4Hw.cpp:551-559` | `GO_BIT_TIMEOUT_MSECS`（`mlx4_regs.h:63`，注释指 `mlx4_cmd.c:115`） | 10000 | **循环次数**，每次 {HCR 读 + `IOSleep(1)`} | 2.27 ms/次（§1.4 实测） |
| 投递后等待 `Mlx4Hw.cpp:598-619` | `timeoutMs` 形参 | 调用方给 | 同上 | 2.10 ms/次（§1.4 实测） |

调用方给的预算（`mlx4_regs.h:64`、`:71`、`:640-641`）：

| 常量 | 名义 | 实际墙钟（按 2.1 ms/次） |
| --- | --- | --- |
| `MLX4_CMD_TIME_CLASS_A/B/C` = 60000 | "60 s"（对齐参考的 `cmd.h:176-178`） | **≈ 126 s** |
| `CX3_M1_CMD_TIMEOUT_MS` = 5000 | "5 s" | ≈ 10.5 s（§1.4 实测 10.52 s） |

`Mlx4Hw.cpp:598-608` 的注释自己就写了这件事（"one iteration is not 1 ms ... 60000 iterations
took 185.36 s, i.e. about 3.09 ms each"），但**常量名和数值都还按毫秒在用**。
这是 E 家族（单位/常量错），只是它错的方向恰好是"更慢"。

### 1.4 2026-10-12 01:2x 那次卡死的实测时间线

取自本机 unified log（`/usr/bin/log show --predicate 'process == "kernel" AND
eventMessage CONTAINS "Cx3Mlx4"'`，已按时间序整理）。这比 `§37.4` 的估计更具体，
也修正了其中两个数字。

```
01:27:01.609  [d54b] fwload: MAP_FA 投递（op 0xfff，7 个批次）
01:27:12.138  [d54b] command 0xfff timed out: 5000 iteration(s), status 0x00800fff
                      -> 5000 次 = 10.52 s（2.10 ms/次）；GO(bit23) 从此一直置起
01:27:12.143  [d54b] fwload: MAP_FA failed (fw status 0xfffffffd) after 0 entries
01:27:12.202  [db46] m2dprofile 进入（开始 freeIcmTables -> 逐块 UNMAP_ICM）
01:27:34.878  [db46] cmd 0xff9: command register never went idle (0x00800fff)
                      -> 第一条命令的忙等跑满 10000 次 = 22.68 s（2.27 ms/次）
01:28:02.065  [db46] 第 2 块   27.19 s
01:28:26.177  [db46] 第 3 块   24.11 s
01:28:51.142  [db46] 第 4 块   24.97 s
01:29:25.666  [db46] 第 5 块   34.52 s
01:29:51.888  [db46] 第 6 块   26.22 s
01:30:06.323  [db46] 第 7 块   14.44 s
01:30:20.995  [db46] 第 8 块   14.67 s
01:30:42.657  [db46] 第 9 块   21.66 s
01:31:13.621  [db46] 第 10 块  30.96 s
01:31:32.241  [db46] 第 11 块  18.62 s
01:31:45.393  [db46] 第 12 块  13.15 s
01:32:01.950  [db46] 第 13 块  16.56 s
01:32:22.506  [db46] 第 14 块  20.56 s
01:32:52.539  [db46] UNMAP_ICM_AUX  30.03 s
01:32:52.539  [db46] icm tables: released (the HCA's page tables no longer reference them)
01:33:19.843  [db46] cmd 0x3 (QUERY_DEV_CAP)  27.30 s -> 随后的 m2dprofile 失败返回

01:29:13.249  [fe84] m3irqdown 的连接建立（handler 的第一行日志）
01:33:19.843  [fe84] m3irqdown: session closed ...
                      -> 这一条在事务锁上等了 4 min 06.6 s
01:33:19.904  [1262a] HCA reset requested（前一调用放锁后 61 ms 拿到）
01:33:20.906  [1262a] reset: done
```

实测结论，逐条对着 `§37.4`：

1. 那次持锁 = **01:27:12.202 → 01:33:19.843 = 6 min 07.6 s**，不是"十几分钟"；
   同一窗口里阻塞的 m3irqdown 等了 **4 min 06.6 s**。
2. 逐块代价 = **13.15 – 34.52 s，均值 22.98 s（16 条命令）**，不是"每块 20–80 秒"。
3. 付这笔钱的是**投递前的忙等**（`GO_BIT_TIMEOUT_MSECS` = 10000 次），不是这条命令自己
   5000 次的预算 —— 因为 GO 已经卡住，命令根本没被投递出去。所以只压 `timeoutMs`
   压不到这个主项。
4. "逐块 UNMAP **28** 张表"是数错了：这份 profile 是 14 张表、每张 1 个 chunk
   （`01:26:11.630 profile: ... 14 table(s)`，每张一行 "chunk 1/1 mapped"），
   即 14 个 UNMAP_ICM + 1 个 UNMAP_ICM_AUX + 1 个 QUERY_DEV_CAP = 16 条命令。
5. 卡死的**成因**也能读出来：GO 是在 **01:27:01.609 投递 MAP_FA 时被置起后再没清**，
   即"设备在一条命令上永远不完成"，不是读寄存器读坏的；而那条 MAP_FA 之所以被投递，
   正是 `§37.3` 的"`reset` 被拒绝但退出码为 0"链条（`01:27:01.487` 有那次拒绝的日志）。

### 1.5 同一次事件里暴露的第二个缺陷：解映射失败照样释放

`freeIcmTables()`（`Cx3Mlx4.cpp:3534-3557`）：

```c
3544            fHw.unmapIcm(t->icm[c]->npages,
3545                         t->virt + (UInt64)c * MLX4_TABLE_CHUNK_SIZE, &fwStatus);
3546            fHw.icmFree(t->icm[c]);          /* 返回值没看 */
3547            t->icm[c] = 0;
```

同一时刻的实测日志（01:26:43，`cdd6` 那一次 `m2dprofile`）：

```
01:26:43.431 ..01:26:43.473  14 条 UNMAP_ICM(...): failed (fw status 0x1)   <- INTERNAL_ERR
01:26:43.475  icm tables: released (the HCA's page tables no longer reference them)
```

**固件连着 14 次说"没给你解开"，代码把 14 块全部释放了，还打了一行断言相反的日志。**
这与 `DEFECTS.md` B2/C2 已经立过的规矩（"只有明确的固件状态才释放，超时就泄漏"）矛盾，
而同一个文件里其它三条拆除路径都守规矩：`cqTeardown`（`:3752-3763`）、
`mrTeardown`（`:3819-3830`）、`teardownEq`（`:3871-3886`）全部是"拿不回来就不释放"。
`exportM2dBringUp` 的 aux 路径同样漏了检查（`Cx3Mlx4.cpp:3650-3655`，`unmapIcmAux` 结果被丢）。

这条是 B 家族的第 6 行，而且发生在**同一个持锁窗口里**：那 16 条命令的 UNMAP 全部失败，
14 块内存照样被释放（`01:32:52.539 icm tables: released`）——设备当时是死的，所以没出事；
设备只是"慢"而不是"死"的时候，这就是让固件继续指向已被回收的宿主内存。

---

## 2. 三条候选逐条评审

### 2.1 候选 1 —— 给 `reset` 一条不取锁的旁路

**(a) 解决什么**：`reset` 必然可达，代价约 1.1 s（实测 `01:33:19.904 → 01:33:20.906`），
与另一个 selector 在干什么无关。

**(b) 引入什么新风险**：两个线程同时改 `fTables[]`。这不是"可能读到不一致的值"，
而是一条具体的释放后使用路径（推演见 §3.1）。另外，设备一旦被复位，卡住那个线程的
忙等立刻结束（GO 被清），它会**继续把剩下的命令投递进刚复位好的设备**。

**(c) 什么竞争下失效**：`reset` 返回之后、卡住的线程还在 `freeIcmTables()` 循环里时，
恢复脚本按 `§37.3` 的写法接着跑 `m2dprofile`。实测这个窗口有 **4 分钟以上**
（`01:29:13` 想 reset，线程要到 `01:33:19.843` 才退出），所以不是理论窗口。

**(d) 要改哪里**：`Cx3Mlx4UserClient.cpp:148-150`（按 selector 分流的取锁表）；
`Cx3Mlx4.h` 新增"表状态被并发改建"的可检测标记；`exportM2dProfile`
（`Cx3Mlx4.cpp:3275` 前后）加拒绝条件。

**(e) 能自动化验证吗**：**部分能，但验不到要害。** 能验的是取锁策略表本身
（调度器的锁分类可以做成构建期检查，像 `check-boot-path.py` 那样）；
验不到的是"两个线程没有同时写 `fTables[]`"——外部看不见这个性质，
和 `DEFECTS.md` C9 一样属于"守卫就是修复本身"的那一类。

### 2.2 候选 2 —— 把命令超时压到有界

**(a) 解决什么**：最坏持锁从"十几分钟"变成"命令条数 × 预算"。配合 §3.4 的 R2/R3，
条数这一项也能消掉，于是最坏 = **一条命令的预算**。

**(b) 引入什么新风险**：① 只压 `timeoutMs` 压不到主项（忙等是 10000 次，见 §1.3）；
② 压太狠会把"慢但合法"的命令变成超时，而本移植对超时是**有语义**的
（`Mlx4Hw.cpp:2360`、`:2958`：超时故意泄漏、不释放），误判超时 = 泄漏一片内存；
③ 参考给所有 class 都是 60 s（`vendor/freebsd-src/cmd.h:176-178`），
所以"压到几秒"必须按命令类别分档，不能全局一刀。

**(c) 什么竞争下失效**：本身不涉及竞争——它只是不足够。

**(d) 要改哪里**：`Mlx4Hw.cpp:551-559`（忙等）、`:598-619`（等完成）、
`:2696`（`hcrRun` 的默认档）、`:1756`（`cmdEvent`）；常量 `mlx4_regs.h:63`、`:64`、
`:71`、`:640-641`。

**(e) 能自动化验证吗**：**能，而且便宜。** 预算算术做成纯函数（项目已有的模式：
`mlx4_pure.h` + `tools/test-*.c`，由 `build.sh:211/229/267/288` 编译并执行），
断言"毫秒预算 → 墙钟落在 [预算, 预算×1.2] 内"、"单位是纳秒不是循环次数"。
真机侧用 `/usr/bin/time` 量一次已知慢调用的墙钟即可对照。

### 2.3 候选 3 —— 锁退化成"邮箱所有权"

**(a) 解决什么**：入口不再阻塞——要么拿到邮箱，要么立刻被告知"设备不可用"。

**(b) 引入什么新风险**：**取决于"放弃"的定义。**
若"放弃"= 返回失败、下一个人可以继续投递，则：
① 邮箱只有一块，下一条命令开头就 `memset(fMboxVirt, 0, MLX4_MAILBOX_SIZE)`
（`Mlx4Hw.cpp:2719`），会擦掉设备可能还在读的 inbox；
② 被放弃那条命令的回复会落进这块邮箱，被**下一条**命令当成自己的状态读走。
本移植防 ② 的唯一手段是 GO/T 位判忙（`Mlx4Hw.cpp:434-441`），而 GO 卡住正是
这个失败模式本身。所以候选 3 若只是"放弃"，反而制造出更坏的一类错——
**看起来像数据的错**。若"放弃"= 放弃**+ 毒化**（之后不再使用邮箱），
则没有 ①②，这正是参考的语义（见 §3.3）。

**(c) 什么竞争下失效**：毒化之后若还留着"越过忙位强行投递"的口子就失效——
`bypassBusy`（`Mlx4Hw.cpp:547-549`）目前可被工具直接打开三处：
`querydevcap ... ignorebusy`（`Mlx4Hw.cpp:1367` 的签名、`:1383` 的调用点；
工具解析在 `tools/cx3ctl.c:2871`，打印在 `:468`）、
`fwoutbox ... ignorebusy`（`Mlx4Hw.cpp:1322` 的 `queryFwAt` 透传 `bypassBusy`，
由 `Cx3Mlx4.cpp:1028` 使用；工具解析在 `tools/cx3ctl.c:2883`），以及
`hcr ... bypassbusy`（`Mlx4Hw.cpp:2694` 解出标志、`:2743`/`:2746` 用它投递；
ABI 位在 `cx3_abi.h:1699`；工具打印在 `tools/cx3ctl.c:2030`）。
注意 `queryfw force` **不是**这条路：它只跳过 ownership 门（`cx3_abi.h:101` 的
`kFwInForce`；`tools/cx3ctl.c:183`），仍然等忙位。

**(d) 要改哪里**：`Mlx4Hw.cpp:472-476`/`:497-507`/`:1752-1768`（取锁改成"取到或用不了"）、
`:512`（`cmdBoxLocked` 签名，入口即失败就加在这里）；`Mlx4Hw.h:714` 附近新增状态成员；`Mlx4Hw.cpp:347-427`
（`resetHca` 成功时清除毒化）。

**(e) 能自动化验证吗**：**能，而且核心不需要真机。** 决策逻辑抽成纯函数
（`cx3CmdGate(busy, wedged, elapsedMs, budgetMs) -> {post, retry, failFastAndPoison}`），
构建期断言四条迁移：忙+预算到 → 毒化；已毒化 → 不读寄存器即失败；
复位成功 → 解除毒化；复位后 T 位回声已错位但仍允许投递（`resetHca` 已 `fToggle = 1`，
`Mlx4Hw.cpp:426`，说明程序早就知道复位会让回声错位）。
真机侧再用一个故障注入 selector 复现整条路径（§5）。

---

## 3. 定稿建议

**选 2 + 3，以"有界 + 毒化"合并形态落地，并补上 §1.5 的第三条改动；
不采纳候选 1 的锁旁路。**

### 3.1 为什么"不取锁的 reset 旁路"和"另一个 selector 正拿着邮箱"会真的互相破坏

先回答"会不会"：**会，而且最坏后果在本次启动内不可恢复。** 推演只用源码：

1. 卡住的线程正处在 `freeIcmTables()`（`Cx3Mlx4.cpp:3540-3548`）：逐块
   `fHw.unmapIcm(...)`，然后 `fHw.icmFree(t->icm[c])` 并 `t->icm[c] = 0`。
2. `reset` 走旁路立刻执行：写复位寄存器、恢复 PCI 头（`Mlx4Hw.cpp:396-422`），
   然后 `exportResetHca` 把 `fHcaInited/fIcmMapped/fClrIntReady` 置无效
   （`Cx3Mlx4.cpp:388-404`）。**`fNumTables` 不变，`fTables[]` 里的指针也不变。**
3. 恢复脚本按既定顺序接着跑 `m2dprofile` 与 `m2dbringup`。`exportM2dProfile`
   第一件事就是 `freeIcmTables()`（`:3275`），随后 `makeProfile(..., fTables, ...)`
   重填表结构并 `fNumTables = tablesAsked`（`:3300-3302`）；接着 `m2dbringup`
   走 `initIcmTables()`（调用点 `:3671`）→ `mapIcmTable()`（chunk 循环 `:3426`、
   失败即 `break` 于 `:3509`），逐个 `t->icm[i] = fHw.icmAlloc(...)`（`:3436`）
   并 `fHw.mapIcm(...)`（`:3444`）。
4. 与此同时线程 1 还在它自己的 `freeIcmTables()` 循环里，**下一次迭代就作用在
   同一个 `t->icm[c]`、同一个下标上**：若它在新块分配之后执行，就把新块
   `fHw.icmFree()` 掉而指针还在（新块被释放，固件的页表却指向它）；
   若它落在分配与置位之间，新块指针会被置 0（泄漏，且固件页表指向一块驱动
   已不再持有的内存）。两种次序都是坏的。
5. 结果：`MAP_ICM` 已经把这块内存交给了固件，于是固件的页表指向**已经还给
   分配器的宿主内存**。之后内核把这段内存给谁，这张卡就往谁身上 DMA。
   这是主机内存损坏，不是"状态不一致"。

三个补充事实让这个推演不是纸上谈兵：

- 那条并发写路径**现在正好存在，只是被锁挡着**：`freeIcmTables()` 与
  `exportM2dProfile()` 都改 `fTables[]`/`fNumTables`，锁是唯一把它们排开的东西。
- 实测窗口足够宽：`01:29:13` 发起恢复，卡住的线程到 `01:33:19.843` 才退出。
- 旁路还会让卡住的线程**恢复投递**：复位把 GO 清了，它那 10000 次的忙等会立刻
  通过，然后把剩下的 UNMAP_ICM 投进刚复位好的设备（`01:33:19.843` 那条
   `cmd 0x3` 就是"忙等通过 → 投递"的形状）。

**可恢复还是不可恢复**：`reset` 本身是安全的（它不发任何命令，见 §1.2），
不可恢复的是第 4/5 步——那之后驱动自己也不知道哪些块属于谁，只能整机重启。
所以 `§37.4` 那句"可能在竞争下被观察到不一致状态"**低估了一档**。

### 3.2 超时该定多少（参考驱动的策略）

参考的等待策略，逐条对行号：

| 参考行为 | 位置 | 说明 |
| --- | --- | --- |
| 三档超时全是 60000，且单位是**毫秒**：`end = msecs_to_jiffies(timeout) + jiffies` | `mlx4_cmd_poll`，`vendor/freebsd-src/mlx4_core/mlx4_cmd.c:582`、`:615` | 即**真 60 s** |
| 投递前**不等空闲**（polled 命令）：`end = jiffies; if (event) end += msecs_to_jiffies(GO_BIT_TIMEOUT_MSECS);` | `mlx4_cmd_post`，`mlx4_cmd.c:456-459` | 忙等只给 event 模式；polled 靠 `poll_sem` 串行，所以不忙等。**卡住时参考是"立刻失败"，本移植是"等 10000 次迭代"** |
| `poll_sem` 跨 post+wait 持有 | `mlx4_cmd.c:592`、`:662` | 参考也是"锁住整条命令"，只是它的等待有真上界 |
| 超时 → 不是继续走，而是 `mlx4_cmd_reset_flow` | `mlx4_cmd.c:634-638` → `:240-252` | → `mlx4_enter_error_state` |
| `mlx4_enter_error_state`：复位芯片 + 置 `MLX4_DEVICE_STATE_INTERNAL_ERROR`，之后每条命令在 `mlx4_cmd_post` 入口被拒 | `mlx4_catas.c:169-200`、`mlx4_cmd.c:447-455`；默认 `mlx4_internal_err_reset = 1`（`mlx4_catas.c:47`） | **放弃 = 毒化 + 要求复位**，这就是候选 3 的正确形态 |
| 迟到的完成用 token 丢弃 | `mlx4_cmd_event`，`mlx4_cmd.c:666-681` | "previously timed out command completing at long last" |

据此定档（真墙钟，按 `clock_get_uptime()` 算；`Mlx4Hw.cpp:3064` 已有用法）：

| 用途 | 建议值 | 理由 |
| --- | --- | --- |
| 投递前忙等（polled） | **50 ms** | 参考是 0；留一点给"上一条命令刚返回"的正常抖动。**这是本次事故的主项，必须压** |
| 投递前忙等（event 模式） | **10 s**（= 参考的 `GO_BIT_TIMEOUT_MSECS`，改成真毫秒） | 对齐参考语义 |
| ICM / 表拆除类命令 | **5 s** | 对齐现在 `CX3_M1_CMD_TIMEOUT_MS` 的意图（真 5 s，不是 10.5 s） |
| 固件/初始化类（RUN_FW、INIT_HCA、SW2HW_\*） | **60 s** | 与参考的 class A/B/C 一致，不要更短 |

### 3.3 已提交（GO 置起）之后能不能安全放弃

**答案：不能"干净地"放弃；只能"放弃 + 毒化 + 要求复位"。** 三条理由都能落到源码：

1. **邮箱可能还在被读**：GO 置起后设备可能仍在读 inbox；下一条命令开头就
   `memset` 整块邮箱（`Mlx4Hw.cpp:2719`）。这一条**无法从主机侧判定**设备是否读完。
2. **回复可能被下一个人认领**：状态字在 HCR+0x18（`Mlx4Hw.cpp:621`），
   投递不出去时唯一防线是 `cmdPending()`（`:434-441`）；而 GO 卡住正是本失败模式，
   防线在那一刻是失效的。参考为此专门在完成路径上用 token 过滤（`mlx4_cmd.c:666-681`），
   本移植的 polled 路径没有 token 可用；`bypassBusy` 更是明确允许"压在卡住的 GO 上投递"。
3. **不能释放设备可能还指着的内存**：本移植对 EQ/CQ 已经守住这条
   （`Mlx4Hw.cpp:2360`、`:2958`：超时故意泄漏），但 `freeIcmTables()` 没守（§1.5）。
   **所以候选 2/3 能不能用，取决于这条是否同时修掉；只改前两条，等于把"释放后使用"
   从 15 分钟后提前到几秒后。**

结论：**"放弃"必须同时意味着"邮箱和这块设备不再被使用，直到复位"**，
因此候选 3 的"标记邮箱不可用"是必需的，而"不阻塞在锁入口"不是。
候选 1 想要的"绕过锁"，用毒化就能拿到：毒化之后每个命令都是 O(微秒) 失败，
于是**持锁时间自然有界**，`reset` 也就可达了。

### 3.4 定稿的四条规则

- **R1（有界）**：每一次等待都是一个墙钟 deadline，单位是毫秒；投递前忙等 ≤ 50 ms。
  模块内的循环次数不再当作时间单位。
- **R2（第一次放弃就毒化，之后快速失败）**：忙等到点或命令超时 → 置 `fCmdWedged`，
  此后所有命令立刻返回 `CX3_FWSTATUS_WEDGED` 且**不碰 HCR**。
  于是 `freeIcmTables()` 的第 2..16 条命令是微秒级（对照实测 6 min 07.6 s）。
- **R3（拿不回来就不释放）**：`freeIcmTables()` 检查 `unmapIcm/unmapIcmAux` 的返回值；
  失败即停止循环、置 `fIcmStuck`、不释放、日志写清楚。
  只有**设备已被复位**（`hcaWasReset=true`）时才允许无条件释放，因为那时固件确实忘了这些表。
  这条同时把"表拆除"的循环上界压成**一条命令**（第一条失败就退出），
  而不是"条数 × 预算"。
- **R4（复位是唯一解药，且必须总可达）**：`resetHca()` 成功后清除 `fCmdWedged`
  （现有 `fToggle = 1` 保留）；`exportResetHca` 用 `hcaWasReset=true` 释放 ICM 表并
  清零 `fNumTables`、清 `fIcmStuck`。`probe`/`survey`/`peek`/`m3irqstatus`
  **不再排队等命令锁**（见 §4.4），使"设备现在是什么状态"这个问题永远能在 1 s 内回答。

---

## 4. 实现草图

伪代码级，精确到函数与变量名；**只写建议，本轮不含任何已落地的改动**。

### 4.1 `src/mlx4_regs.h`

```c
/* 时间单位一律是真实毫秒；循环次数不再当作时间用。 */
#define CX3_CMD_IDLE_WAIT_MS       50u   /* polled：参考是 0（mlx4_cmd.c:456-459） */
#define CX3_CMD_IDLE_WAIT_EVENT_MS 10000u/* event：对齐 GO_BIT_TIMEOUT_MSECS，真毫秒 */
#define CX3_CMD_BUDGET_ICM_MS      5000u
#define CX3_CMD_BUDGET_FW_MS       60000u/* == mlx4_cmd.c 的三档 class */
#define CX3_FWSTATUS_WEDGED        0xfffffffbu /* 主机侧：一条命令已被放弃且未复位 */
/* GO_BIT_TIMEOUT_MSECS 只在 event 路径保留，注释里标明"参考的 10000 真毫秒" */
```

### 4.2 `src/Mlx4Hw.h` / `Mlx4Hw.cpp`

```c
/* Mlx4Hw.h：新增状态与访问器 */
bool  fCmdWedged;                       /* 构造函数初始化列表要加（Mlx4Hw.cpp:63-64） */
bool  cmdWedged() const { return fCmdWedged; }
void  cmdPoison(const char *why);       /* 只在第一次时打日志 */
void  cmdClearWedged() { fCmdWedged = false; }

/* cmdBoxLocked()：在 "!fHcr" 检查（Mlx4Hw.cpp:532）之后立刻加 */
if (fCmdWedged) {
    if (outFwStatus) *outFwStatus = CX3_FWSTATUS_WEDGED;
    return false;                       /* 不读 HCR、不 memset 邮箱 */
}

/* Mlx4Hw.cpp:551-559 的忙等换成 deadline：
   polled 用 CX3_CMD_IDLE_WAIT_MS，event 用 CX3_CMD_IDLE_WAIT_EVENT_MS */
UInt64 idleEnd = nowNs() + idleWaitMs * 1000000ull;
while (cmdPending()) {
    if (nowNs() >= idleEnd) { cmdPoison("the command register never went idle"); ... }
    IOSleep(1);
}

/* Mlx4Hw.cpp:598-619 的等完成换成同一个 deadline 形状；到点调 cmdPoison() */

/* 预算默认值：cmdBox/cmdBoxEvent 的 timeoutMs=0 分支（Mlx4Hw.cpp:2696）改用
   CX3_CMD_BUDGET_FW_MS；unmapIcm/unmapIcmAux 的调用点（Mlx4Hw.cpp:1595、:1610）
   改用 CX3_CMD_BUDGET_ICM_MS */

/* Mlx4Hw.cpp:347-427 resetHca()：成功后 cmdClearWedged()（fToggle = 1 已在 :426） */
```

### 4.3 `src/Cx3Mlx4.cpp`

```c
/* 1) 表拆除：拿不回来就不释放（R3）。freeIcmTables 由 0 参变成 1 参 */
void Cx3Mlx4::freeIcmTables(bool hcaWasReset)          /* :3534 */
{
    for (i ...) for (c ...) {
        if (!t->icm[c]) continue;
        if (!hcaWasReset) {
            if (!fHw.unmapIcm(..., &fwStatus)) {       /* :3544 现在要看返回值 */
                fIcmStuck = true;
                LOG("icm table %s chunk %u was NOT handed back (fw status 0x%x): it is "
                    "NOT freed and the remaining chunks are left alone", ...);
                return;                                /* 循环上界 = 一条命令 */
            }
        }
        fHw.icmFree(t->icm[c]); t->icm[c] = 0;         /* :3546-3547 原样 */
    }
    /* :3550-3554 的 aux 同样处理；并修掉 :3650-3655 的同型漏检 */
}

/* 2) 新 profile 的入口闸门 */
IOReturn Cx3Mlx4::exportM2dProfile(...)                /* :3253 */
{
    if (fIcmStuck) {                                   /* 新增，放在 windowAlive 之后 :3268 */
        LOG("m2dprofile refused: the previous layout's memory could not be handed back; "
            "a reset is required before the layout can be reused");
        res->status = (UInt64)kIOReturnNotReady;
        return kIOReturnSuccess;
    }
    freeIcmTables(false);                              /* :3275 */
    ...
}

/* 3) 复位：唯一解药（R4） */
IOReturn Cx3Mlx4::exportResetHca(bool bypassSem, UInt64 *out)   /* :322 */
{
    /* 复位成功之后、现有失效处理之前（:387-390 附近） */
    if (fNumTables || fAuxIcm) freeIcmTables(true);     /* 设备已忘，无需 UNMAP */
    fNumTables = 0;
    fIcmStuck  = false;
    out[kRstOutCmdWedged] = fHw.cmdWedged() ? 1 : 0;    /* 新输出槽：复位前是否毒化 */
    /* 之后按现有顺序 teardownEq/cqTeardown/rqTeardown/mrTeardown */
}

/* 4) 只读诊断不再排队（R4）：probe(0)/peek(4)/survey(32)/m3irqstatus(21) 见 4.4 */

/* 5) survey 暴露状态（:1667） */
SURVEY(CX3_SURVEY_KIND_FACT, kSurveyFactCmdWedged, fHw.cmdWedged() ? 1 : 0, 0, 0);
SURVEY(CX3_SURVEY_KIND_FACT, kSurveyFactIcmStuck,  fIcmStuck ? 1 : 0, 0, 0);

/* 6) stop() 的调用点 :3220 改成 freeIcmTables(false)：驱动停机时拿不回来就泄漏，
      与 teardownEq/cqTeardown/mrTeardown 的既有取舍一致 */
```

### 4.4 `src/Cx3Mlx4UserClient.cpp:140-160`（取锁策略）

```c
/* 一个显式的分类表，按 selector 编号决定取锁方式，取代"一律 transactionEnter" */
static const UInt8 kLockClass[kCx3SelCount] = {
    /* probe */ 1, /* peek */ 1, /* survey */ 2, /* m3irqstatus */ 2,
    /* 其余全部 */ 0,
};
/* 0 = transactionEnter/Exit（原样）
   1 = 不取锁：只读寄存器，不读任何可能被并发改的状态
   2 = IOLockTryLock 成功才进；失败则填 kIOReturnBusy 并在 LOG 说明
       注意：IOLockTryLock 是否通过 build.sh:304-318 的符号核对，
       要先用一次构建确认；不通过就退回"1"并把 survey 的撕裂读风险写进注释 */
```

### 4.5 `src/cx3_abi.h` / `tools/cx3ctl.c`

- `kCx3SelCount` 33 → 34，新增 `kCx3SelFault = 33`（故障注入，见 §5；
  `in_mode`：1=置毒化，2=清毒化，3=按住 N 毫秒再返回）。
- `kSurveyFactCmdWedged = 7`、`kSurveyFactIcmStuck = 8`；`kRstOutCount` 3 → 4
  （加 `kRstOutCmdWedged`）。**这两处都改标量/结构体布局，属 ABI 变更。**
- `tools/cx3ctl.c`：新增 `fault` 子命令；`probe`（`cmdProbe` `:127-152`）打印毒化位；
  `reset`（`cmdReset` `:235`）打印复位前毒化态；`survey` 打印两个新 fact。

### 4.6 影响的 selector 与是否需要重启

| selector | 行为变化 |
| --- | --- |
| `probe(0)` `peek(4)` `survey(32)` `m3irqstatus(21)` | 不再等事务锁；`survey` 可能返回 busy |
| `reset(3)` | 释放 ICM 表（`hcaWasReset=true`）、清毒化与 `fIcmStuck`；仍需等锁，但**上界 = 一条命令预算** |
| `m2dprofile(13)` | `fIcmStuck` 时拒绝；表拆除在第一条失败处停止 |
| 所有发命令的 selector（2/8/10/13/14/15/16/17/18/20/24/25/26/27/28/29/30/31） | 毒化后立即失败；预算改成真毫秒 |
| `queryfw(2)` `querydevcap(8)` `hcr(31)` 的 `bypassBusy` | 只允许"不带邮箱"的诊断型命令越忙投递，否则拒绝 |
| `m3irqdown(22)` | 行为不变（仍取锁），但现在最多等一条命令预算 |

**重启要求**：kext 与 ABI 都变了（selector 数、标量数、新增 fact）→ **要重启并重新
`verify.sh stage` + `install`**。只改 `tools/cx3ctl.c` 的话不需要重启，工具侧改动即时生效。

---

## 5. 验证计划

### 5.1 构建期（不需要设备，也不需要重启）

| 检查 | 内容 | 期望 |
| --- | --- | --- |
| `bash build.sh` | 新增 `tools/test-cmd-wedge.c`（纯函数 `cx3CmdGate`），编译并执行，位置对齐 `build.sh:211/229/267/288` 的既有模式 | 四条迁移全部断言通过：忙+预算到 → 毒化；已毒化 → 不读寄存器即失败；复位 → 解除；真毫秒 deadline 落在 [budget, 1.2×budget] 内 |
| `bash build.sh` 末尾 | 符号核对（`build.sh:304-318`） | `clock_get_uptime`（以及若用到的 `IOLockTryLock`）全部在 kernel collections 里解析；不一致就换等价实现 |
| `tools/verify.sh stage` | ABI 自检 + boot-path 检查 | 新 selector 的索引能被解析；`start()` 仍不可达任何写路径 |

### 5.2 重启后（逐条命令与期望值）

前置：kext **0.44.0** 已加载，`kmutil showloaded` 的版本与 `kext/Info.plist` 一致
（`tools/verify.sh` 的 `do_run()` 自己会比，不一致直接报 INCONCLUSIVE 并退出 3）。
落地的版本号归属：`eq_ordering` 已经把 0.43.0 用掉（`0c7a1d8`），本方案是 **0.44.0**。

```sh
# 0. 基线：GO 位必须是清的
sudo cx3ctl probe
#   期望 HCR status 的 bit23(0x00800000) 为 0；若已置位，先 reset 再继续。
#   这一条就是"设备当前是否卡着"的判据，且不需要发任何命令

# 1. 回归：这次改动不许碰坏任何东西（对照 §37.1 在 0.42.0 上的 20 项 PASS）
sudo tools/verify.sh run
#   期望：除中断那几项外逐项 PASS，总耗时与 §37.1（0.42.0）同量级。
#   中断的结果**本方案不预设**：0.43.0 把 arm 移到 session open（0c7a1d8），
#   §37.1 那条 "interrupt delivery FAIL" 是否还在，取决于那次改动的效果

# 2. 故障注入：复现"毒化之后 m2dprofile 必须秒回"
sudo cx3ctl fault wedge
sudo cx3ctl survey                    # 期望：kSurveyFactCmdWedged = 1，且 1 s 内返回
time sudo cx3ctl m2dprofile 2 2 2 2 2 2 2
#   期望：< 2 s 返回（对照实测 6 min 07.6 s）；
#   UNMAP_ICM 报 0xfffffffb（WEDGED）而不是 0xfffffffd；
#   日志出现 "was NOT handed back ... NOT freed"；survey 的 kSurveyFactIcmStuck = 1；
#   probe 的 HCR status 与注入前一致（证明没往卡住的设备写任何东西）

# 3. 只读诊断在长 selector 运行中仍然可达
sudo cx3ctl fault hold 3000 &
sleep 1
time sudo cx3ctl probe                # 期望：< 1 s
time sudo cx3ctl survey               # 期望：< 1 s，或明确报 busy
wait

# 4. 恢复路径的等待有界，且复位是唯一解药
sudo cx3ctl fault wedge
time sudo cx3ctl reset force
#   期望：等锁时间 ≤ 一条命令预算（5 s 档）+ 复位自身约 1.1 s；
#   输出里 kRstOutCmdWedged = 1（复位前确实毒化）
sudo cx3ctl probe                     # 期望：HCR status bit23 = 0
sudo cx3ctl survey                    # 期望：CmdWedged = 0、IcmStuck = 0

# 5. 复位之后整条链仍然走得通（证明毒化不会粘住）
sudo tools/verify.sh run
#   期望：与第 1 步完全相同的 PASS 分布

# 6. 超时单位回归（只读）
/usr/bin/time -p sudo cx3ctl querydevcap
#   期望：real 明显小于 60 s；若某条命令真的超时，日志里
#   "timed out: N iteration(s)" 应改成以毫秒标注的 deadline 文案
```

把第 2/4 步接进 `tools/verify.sh`（新的一步，位置随实现者定），
断言的是**墙钟上界**与**毒化位**，不是"日志里出现了某行"。

### 5.3 验不到的部分（要如实写在报告里）

- **触发条件没有解决**：这次改的是"坏掉之后多久能恢复"，不是"为什么 GO 会永久置起"。
  实测只能指到"MAP_FA 投递后设备不再完成"（`01:27:01.609` → `01:27:12.138`），
  为什么会这样，本轮没有证据。
- 真机上的**真毒化路径**（而不是注入的）要等下一次自然发生，或者接受注入的那一次作为证据。
- 两个线程真的同时进 `freeIcmTables()` 会怎样，最终靠 R3 的守卫而不是测试
  （同 `DEFECTS.md` C9）。

---

## 6. 与 §37.4 的关系（采纳 / 修正 / 推翻）

| §37.4 的说法 | 本次结论 |
| --- | --- |
| "锁把…换成了…整个驱动对用户态完全不可达，包括 reset" | **采纳。** 并补实测：那次持锁 6 min 07.6 s，同一时刻 m3irqdown 等了 4 min 06.6 s；`reset` 不需要邮箱，它被挡住的唯一原因就是这把锁 |
| "它要逐块 UNMAP 28 张表，每块超时约 20–80 秒…拿着锁跑十几分钟" | **修正数字**：14 张表（每张 1 chunk）+ 1 个 aux + 1 个 QUERY_DEV_CAP = 16 条命令，13.15–34.52 s，均值 22.98 s，合计 6 min 07.6 s |
| "候选 2：把命令超时压到有界（几秒）" | **部分采纳，但指出它压错了地方**：主项是投递前的忙等（`GO_BIT_TIMEOUT_MSECS` = 10000 次 ≈ 22.7 s），不是 `timeoutMs`；而且两个常量的"毫秒"其实都是循环次数，`CLASS_A` 名义 60 s、实际 ≈126 s（E 家族） |
| "锁的作用域选错了：它应该保护填 inbox→提交→复制 outbox，而不该跨越一个可能长达数分钟的命令等待" | **推翻这个诊断。** 作用域没问题，问题是**持锁时长无上界**。缩到"只看邮箱"会放开 §3.1 那条 `fTables[]` 并发写（现有锁是唯一屏障），并且放开的正是一个会导致主机内存损坏的窗口 |
| "候选 1：给 reset 一条不取锁的旁路，写明可能在竞争下被观察到不一致状态" | **不采纳，且这句描述低估了风险**：并发下不是"读到不一致的值"，是"释放了刚被新 profile 分配、且固件页表已指向的块" |
| "候选 3：超时就放弃并把邮箱标记为不可用" | **采纳其"标记"的部分**："放弃 + 毒化"正是参考 `mlx4_cmd_reset_flow` → `mlx4_enter_error_state` 的语义（`mlx4_cmd.c:634-638`、`mlx4_catas.c:169-200`）；"不阻塞在锁入口"这一半不需要，因为毒化之后持锁本身就变短了 |
| "我倾向 1+2 一起做" | **建议改成 2+3，加 §1.5 的第三条。** 1 的收益（reset 可达）用 3 就能拿到，而 1 的代价是 §3.1 那条 |

另外两条建议（都属于别的文档，本轮没动）：`DEFECTS.md` 应新增一行 B 家族
（"解映射失败照样释放"，实测日志见 §1.5）和一行 E 家族（`GO_BIT_TIMEOUT_MSECS`
名字是毫秒、单位是循环次数，且 `CLASS_A` 名义 60 s、实际 ≈126 s）。

---

## 7. 最关键的未知项

1. **GO 永久置起的触发条件**未定论：实测只到"MAP_FA 投递后设备不再完成"。
   本设计只保证"坏掉之后有界可恢复"，不保证"不会再坏"。
2. **`probe`/`survey` 不取锁的可行性**取决于 `IOLockTryLock`（或所用的原子操作）
   能否通过 `build.sh:304-318` 的符号核对——本轮没有构建验证，只在方案里留了退路
   （`survey` 报 busy，或退回"不取锁 + 注释写明撕裂读风险"）。
3. **毒化后 `reset` 能清掉卡住的 GO**：有旁证（`01:33:20.906` 复位后
   `01:39:57` 的 MAP_FA 成功、之后不再出现 "never went idle"），但"复位导致 GO 清零"
   仍是推断，不是直接测量。
4. 参考里 `mlx4_internal_err_reset` 默认 1（`mlx4_catas.c:47`）是读源码得来，
   没有做运行时验证。
