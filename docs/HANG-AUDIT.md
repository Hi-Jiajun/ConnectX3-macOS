# 冻屏归因（进行中）

2026-10-24 14:42 事件：[只读取证](../../../outputs/20261024-freeze-1442-readonly/REPORT.md)记录远端 ping/HTTP 失联、beacon 停止和新 boot；用户明确确认人工强制关机/重启。结论为整机失去响应后人工重启，未捕获 panic，根因未定；shutdown cause 5 不能据此判为正常软件重启。探针 ntfy=200 只证明服务端接收。约32秒本机快照缺口仍开放，本次未改在役探针或复现。

同日[superclass/provider 固定源码核查](../../../outputs/20261024-cx3-provider-stop-source/REPORT.md)未发现公开 stop 主动 close PCI provider 的路径；PMstop 有阻止/等待 PM callout 及异步断开 power plane 的语义，网络 gate/workloop 释放在 free。未与本机二进制对齐，不能升级为本机安全证明或本次冻屏归因。最新 RSS 消费端修正仅防止验收误判，也不构成冻屏修复。

> 起因：换到 `IOEthernetController` 基类之后，链条运行期间开始出现**冻屏**（非 panic，需强制重启）。
> 本文只记**证据与判定**；改代码在结论成立之后。

## 1. 症状与诊断报告（2026-10-13）

用户描述是"**显示器冻屏**"。诊断报告分成两批，指向两类受害者：

| 时间 | 报告 | 含义 |
| --- | --- | --- |
| 00:46 / 00:48 / 00:54（5 份） | `WindowServer-*.ips` | **显示栈崩/挂** ⇒ 屏幕冻住（内核可能还活着） |
| 02:31 之后 | `cp` / `bsdtar` / `gh` 的 `.diag` | **用户态进程卡在文件 I/O**（与另一会话记过的"这台机器常有 APFS 卡住"同形） |

**受害者在变**（显示栈 / 文件系统）—— 这正是"有人往别人的内存里写"的形状。

## 2. 每次冻屏的时间形状

三次有内核日志的冻屏，驱动日志末行**都是 `user client closed`**：

| 那次 | 驱动日志末行 | 链条当时 |
| --- | --- | --- |
| 04:25:51 | `fwload OK` → `user client closed` | **第 3 步** |
| 03:25:04 | `m3irqdown: session closed` → `user client closed` | 中断段之后 |
| 02:31:18 | 同上 | 同上 |

⇒ 冻屏**不绑定某一步**，而绑定"**关掉 user client**"这个动作（链条里有几十次开关 ⇒ 挂点看起来到处都是）。

## 3. 已排除的候选

| 候选 | 判据 |
| --- | --- |
| `log stream` 捕获（`verify.sh` 自己起的 debug 级流） | `CX3_NO_KLOG=1` **完全没有捕获**时仍冻 ⇒ 排除 ✓ |
| 某一条固定选择器 | 手动逐条发 R4 那七条：**7/7 全部干净** ✓ |
| `clientClose()` 里的 `terminate()` | **已修**（它终止 provider 链 ⇒ family 的 `handleIsOpen` 空指针 panic）；修完 panic 变成挂起 ⇒ **family 的 close/stop 路径仍未被喂饱** |

## 4. ★ 拆解路径审计：19 处是对的，1 处是**假设**

`grep 'may still'` 得到 19 处显式处理"设备可能仍拥有/仍在翻译"的地方，形状一致且正确：

| 位置 | 做法 |
| --- | --- |
| `mrTeardown`（`:1560`） | MPT 槽位收不回就**留着** |
| QP teardown（`:1574`、`:3416`） | `2RST_QP` 失败就**不拆**，状态标 UNKNOWN |
| CQ teardown（`:6594-6598`） | `HW2SW_CQ` 失败就**漏着** |
| ICM（`:6314`、`:6478`） | unmap 失败宁可**漏**，也不把固件可能仍在用的内存交回系统 |

而且代码自己就写过"设备可能仍在完成旧的那一个"（`:4158`）✓。

**例外：以复位为由直接释放。** 每次链条开头都有这样的日志：

```
TX teardown (HCA was reset): the HCA was reset, so it has forgotten QPN 66 ...
   dropping the host-side state, the send queue's memory and its record
```

**这里的释放凭据是"我们做了一次 HCA reset"，而不是"设备回答它已经放手"。** 两者不等价：

* 我们用的是 **HCA（固件层）复位**，**不是 PCIe FLR（硬件层复位）**（`src/flr.inc` 是另一条路）；
* 固件层复位**不保证数据通路的 DMA 引擎已静默**；上面那 19 处分支的存在，本身说明作者知道"设备可能仍在引用"是常态；
* 一旦那块内存交回系统并**分给别人**（显存缓冲、文件系统缓存），在飞的 DMA 就会写进别人的内存 ⇒ **受害者随机、无 panic、只在重建资源时出现** —— 与全部观测吻合。

## 5. 下一步（改代码之前）

1. **列出"哪些分配在复位后被释放"**（`fR4TxSq` · `fR4TxCqRing` · `fR4CqRing` · `fR4Rq*` · `fEqRing` · ICM 各表），并对每一个写清"释放前设备的状态凭据是什么"；
2. 核对"复位完成"的判据：现在只有 `reset` 的 fw status 0 ⇒ 那是**命令被接受**，不是**数据通路已静默**；
3. 二选一修：改用 **FLR** 之后再释放；或按既有取舍**宁可漏**（与 `2RST`/`HW2SW` 失败时一致）。

**在 1–3 完成之前不再跑链条**（每次运行都是一次冻屏风险）。

---

## 6. 第 1 条做完：复位后释放的分配表（源码级，2026-10-13 05:5x）

`grep -n 'HCA was reset' src/*.cpp` 给出**全部**以复位为由释放的位置 —— **七条，全是同一句凭据**：

| 顺序 | 代码位置 | 释放什么 | 它给的凭据（日志原文的措辞） |
| --- | --- | --- | --- |
| 1 | `Cx3Mlx4.cpp:504` `teardownEq` | EQ 环 + MTT 条目 | "the HCA was reset and has already forgotten the queue" |
| 2 | `:555` `ingressTeardown` | MAC 注册（仅驱动侧状态） | "has forgotten the MAC registration" |
| 3 | `:562` `txTeardown` | **发送队列内存** + 它记录的 `db_rec_addr` | "has forgotten QPN 66 ... dropping the host-side state, **the send queue's memory** and its record" |
| 4 | `:563` `qpTeardown` | RX QP + 生产者记录 | "has forgotten QPN 64 ... dropping the host-side state and the producer record" |
| 5 | `:564` `cqTeardown` | RX 完成队列环 | "has forgotten CQN 128 and there is nothing to hand back" |
| 6 | `:565` `rqTeardown` | RQ 的 WQE 环 + **接收缓冲分配** | "releasing 2 pages of WQE ring **and the buffer allocation**" |
| 7 | `:566` `mrTeardown` | MR / MPT 256 | "has already forgotten MPT 256 (PD 4) and there is nothing to hand back" |

**⇒ 这张表就是答案的形状**：驱动**全部面向设备的分配**（EQ · MAC · SQ+record · QP+record · CQ · RQ+buffers · MR）都在**同一句"固件已经忘了它"**之下被释放 ✓ —— 而**"固件忘了"和"数据通路的 DMA 引擎已停"是两件事** ✓✓（这十九处"收不回就留着"的分支正是作者对此有意识的证据）。

## 7. 第 2 条做完：我们手上"复位完成"的证据是什么

复位路径结束时我们**实际观察到**的只有两样：

| 观察 | 它证明什么 / 不证明什么 |
| --- | --- |
| `reset` 的 fw status == 0 | **命令被接受**（固件层）—— 不证明数据通路已静默 |
| 复位后所有权字回到 `0x01000000` | **驱动/固件之间的所有权交接**完成 —— 不是数据通路的静默凭据 |

**没有任何一处观察是在看"硬件是否还在写内存"**（例如：在飞描述符的 owner 位是否已回、完成计数是否已停、设备是否已不再取 WQE）✓。

## 8. 第 3 条：两种修法（择一，改代码前先定）

| 方案 | 做法 | 代价 |
| --- | --- | --- |
| **A. 先硬件复位再释放** | 释放那七项之前先做 **PCIe FLR**（`src/flr.inc` 已有实现），拿到"硬件被复位"这个**装置级**事实再交回内存 | 每次重建多一次 FLR；要确认 FLR 后重新枚举/BAR 是否变化（现有注释说 FLR 后 BAR0 可能移动 ⇒ 需要重启） |
| **B. 宁可漏（与既有取舍一致）** | 按 `2RST`/`HW2SW`/ICM unmap 失败时的做法：**不释放**，计入泄漏并记录；只把**驱动侧**状态丢掉 | 每次重建漏固定的几页 + 一条队列环；换来"绝不把设备可能仍在写的内存还给系统" |

**建议**：先做 **B**（改动小、与既有纪律一致、不引入 FLR 的不确定性），把冻屏风险先关掉；之后如果泄漏量成为问题，再评估 A。

---

## 9. 0.44.5 落地：三件仪器（2026-10-13）

上一节说"改代码之前先定方案"。**定的不是"选 A 还是选 B"，而是"先把判决做出来再选"** ——
因为 A 和 B 都建立在同一个未验证的前提上（"设备已经不再碰这块内存"），而**这个前提从来没人量过**。
所以 0.44.5 只加读数，不做结论。

### 9.1 仪器一：复位静默读数（`resetSilenceCheck()`，**免费、不改行为**）

复位成功后、**任何释放之前**，把驱动手上**每一块设备能够到的内存**（EQ 环、RX/TX 的 CQ 环、
QP 记录、TX 发送队列、原始帧缓冲、RX WQE 环与接收缓冲、FW 区、ICM 辅助区、14 张上下文表）
各取一份指纹：**dword 累加校验和** + **非零字节数**（后者专门抓"本来是空的、现在被填了东西"
—— 这正是"DMA 写进来了"的形状）。

然后按 **2 ms × 8 步**重采：小块每步都重采，MB 级的大块（≥256 KiB）只在窗口两端重采
（否则扫描开销比被测窗口还大）。任何一块变化就立刻打印**哪一块、多久之后、校验和与非零字节从多少到多少**，
并给出 `reset silence VERDICT`。

同时记录**前提**（这部份决定了"没变"能不能读成"设备停了"）：

| 前提字段 | 为什么必须有 |
| --- | --- |
| `TX in flight`（`fR4RawTxProd - fR4RawTxCons`） | 没有在飞的 WQE ⇒ 设备本来就没活干 |
| `RX posted`（在飞接收槽位数） | 没有已投递的接收 WQE ⇒ 收包也不会写进来 |
| `QPs live` | 三个 QP 是否还在 RTS |
| `ownership word` | 复位后 BAR0 的所有权字 |

**判读纪律**：**"没变 + 前提全是 0" = 无信息**，不能读成"假设被排除"。
只有"没变 + 前提非 0"才是弱证据，"有变化"才是强证据。

### 9.2 仪器二：金丝雀 / 隔离区（`quarantineTake()` + `freezeInstrumentsSweep()`）

复位路径上的七组设备面向内存**不再释放**（第七节那张表里的每一处，现在都走
`releaseIcmOrKeep()` 这一个出口）：**留着、填 `0xDEADBEEF`、登记进隔离区**。

- 填完立刻在日志里留下**前提**（在飞 WQE / 已投递接收槽 / 在 RTS 的 QP / 所有权字）；
- **每次进 user client 选择器**（0.44.4 加的那行 `enter selector N` 旁边）回读整个隔离区，
  一旦某块不再是这个图案，立刻打印**哪一块 / 第几个 dword / 对应物理地址 / 期望值 / 实际值 / 在第几次选择器入口看到**；
- 每 512 次入口打一行汇总，这样"没有命中"和"仪器没在跑"能分辨；
- 隔离区上限 64 块 / 64 MB；满了以后**淘汰**一块"被读过 ≥64 次且从未被写过"的（被写过的一块**永久保留**），
  实在没有可淘汰的才如实打印并真释放（`fQuarRefused` 计数）；
- 驱动 `stop()` 时整区释放；
- `cx3quarfree=1`（boot-arg）可整套关掉做 A/B。

**不隔离的东西，以及为什么**：ICM 上下文表和 FW 区刻意不隔离 —— 它们各自是 MB 级，
而且是在**下一次 reprofile** 时才释放、不是在复位瞬间释放；每次都留会白漏几十 MB，
而仪器一已经在同一个时刻采样了它们。

### 9.3 仪器三：关闭路径时间线（0.44.4 已装，`enter selector N`）

三次有日志的冻屏末行都是 `user client closed`。加了这个之后，下次冻屏只剩两种形状：

| 日志形状 | 结论 |
| --- | --- |
| `closed` 之后**什么都没有** | 卡在**框架的拆卸**里（不是某个选择器） |
| `closed` → `enter N` 之后停 | 卡在**选择器 N** |

### 9.4 判决表（下次冻屏/下次跑链条时按此读）

| 金丝雀 | 复位静默 | 结论 | 修法 |
| --- | --- | --- | --- |
| **被改**（日志点名某块 / 某物理地址） | — | **设备仍在写我们放弃的内存**，且指到了具体一块 | 只对**那一块**改成"宁可漏"（不泄漏一堆）；再评估 FLR |
| 原样 + 心跳证明仪器在跑 | 有变化 | 设备在复位窗口内仍在写（但没写进隔离区那几块） | 同上，范围按读数定 |
| 原样 + 心跳在跑 | 没变且**前提非 0** | "设备复位后仍在写"这条假设**在当前路径上不成立** | 转 close/workloop 那条线（仪器三 + 框架拆卸） |
| 原样 | 没变且前提全是 0 | **无信息** | 造出前提（例如先 post 一批 RX WQE 再复位）重测 |

### 9.5 新增日志行速查（`grep Cx3Mlx4` 后按关键词过滤）

```
reset silence: sampling <N> live device-facing block(s) ...   # 开始采样（前置标记）
reset silence: <name> CHANGED within <n> ms of the reset: ...  # 复位后仍有写入
reset silence VERDICT: <n> block(s) changed inside <n> ms ...  # 强证据
reset silence: <N> block(s) / <K> KiB re-read over <n> ms ...  # 静默（读前提）
quarantine: <name> (...) is KEPT and filled with 0xdeadbeef ... # 隔离一块（含前提）
quarantine: <name> (...) NO LONGER holds the pattern ...        # ★ 金丝雀命中
canary: <N> block(s) / <K> KiB held across <n> entr(ies) ...    # 心跳（每 512 次入口）
reset done: <N> device-facing block(s) / <K> KiB are now HELD  # 每次复位的总结
```

### 9.6 装好之后的第一次测法（按此顺序，先小后大）

0.44.5 装进 AuxKC 需要一次重启（`install.sh` 报 `kmutil load rc=28` 是预期成功路径）。
重启后**不要**先跑整条链条 —— 先用最小暴露面把假设打掉或坐实：

| 步 | 命令 | 期望读到的 |
| --- | --- | --- |
| 1 | `sudo cx3ctl probe` | `enter selector 0`，无其它（隔离区空 ⇒ 扫描直接返回） |
| 2 | `sudo tools/cx3-mlx4/verify.sh up` | 链条跑完**不复位**，把活栈留给下一步；日志里应有 `enter selector N` 全程 |
| 3 | `sudo cx3ctl reset 1 1`（**活栈状态下复位**） | `reset silence: sampling <N> live device-facing block(s)` → 之后是 `quarantine: ... is KEPT`×N + `reset done: N device-facing block(s) ... HELD` |

第 3 步是**信息量最大的一次调用**：栈是活的（QP 在 RTS、接收 WQE 已投递、TX 也有在飞的），
所以"复位后窗口里没变"这个读数**有前提**，不是空读；而七组内存也第一次被**留着**。

三种结果对应的下一步，就是 9.4 那张判决表。**注意**：第 3 步本身就是一次复位，
如果这台机器真会因此冻屏，那一次就是最短、最干净的复现路径 —— 冻结前的最后几行会写明它卡在哪。

> 为什么先 `up` 再单独 `reset`，而不是直接跑整条链条：整条链条有几十次 user-client 开关、
> 一次复位、还有 r4raw 的收发窗口，出问题时分不清是哪一段；而"活栈 + 一次复位"只有一个变量，
> 且耗时几秒。**先把假设测掉，再去跑长链条。**

## 10. 冻屏与"上卡操作"的时间相关性（2026-10-13 12:1x，本轮实测）

问题来自操作者："之前是不是有上卡的操作，冻屏是不是它导致的"。下表是**可复现的四处证据**对齐出来的，
不是印象：① `tools/cx3-mlx4/outputs/verify-*.log`（tee 到磁盘，写到冻屏那一瞬，硬死也留得下）；
② 本轮 CX3 内核行（`/usr/bin/log show … --predicate 'process=="kernel" AND eventMessage CONTAINS "Cx3"'`）；
③ 并行会话的 **5 秒心跳** `/var/log/freeze-capture/watchdog.log`（独立于统一日志，硬死时给出 5 秒精度的死亡时刻）；
④ 开机时刻（`--predicate 'eventMessage CONTAINS "M1a VERDICT"'`）。

### 10.1 08:22 开机之后的五次冻屏

| 冻屏时刻 | 时刻怎么定的 | 死前最后的"上卡" | 链条停在哪一步 | 中断会话 |
| --- | --- | --- | --- | --- |
| ~10:02 | 最后一条 CX3 内核行 10:01:57（只有进程 map，**没有** `user client opened`）；10:08:08 开机 | 手工 `cx3ctl` | 上一次链条（10:00:15）已 PASS 结束，只读 | 未开 |
| ~10:47–10:52（5 分钟窗口，定不准） | 最后一条 CX3 行 10:46:51（只读 `survey`）；10:52:09 开机；watchdog 当时未运行 | 10:45:48–10:46:51 手工选择器 34/32 | — | 未开 |
| 10:58:40 | `verify-20261013-105809.log` 停在 TX QP 段，文件 mtime 10:58 | 链条 10:58:09 起 | QP 三层、TX CQ、TX QP 都过了，停在 TX QP 读回之后 | 10:58:31.97 开 → 10:58:38.29 **正常关**（消耗 2 个中断） |
| 11:13:33 | `verify-20261013-111308.log` 停在 `event-mode NOP posted PASS`，下一步正是"完成通过中断到达" | 链条 11:13:08 起 | 死在等中断投递的事件上 | 11:13:31.60 开，**直到死都没关** |
| 11:50:17±5s | watchdog 心跳在 11:50:17 后断（间隔 5 秒）；11:51:37 开机 | 链条 11:49:58 起 —— **此前 35 分钟一次都没碰卡** | `verify-20261013-114958.log` 停在 `interrupt session open PASS`，下一步是 `port 1: link up` | 开着（死时） |

### 10.2 同样是上卡、但没死的反例（这部分才是关键）

- **10:41:26 整条链条跑完**：中断会话 10:41:50.65 开 → 10:41:57.21 正常关（2 中断 / 2 EQE），
  R4 段因当时的分发表错位瞬间 FAIL，机器活着（到 10:46:51 还在记日志）。
- **12:05:27 手工**：`irqup` OK → 12:05:31 `irqdown` OK（1 中断）→ 活。
- **11:55–11:59 与 12:04 的手工轮次**：`m3irqup` **全部被拒**（`no EQ is up` / `the interrupt clear is not
  set up`）⇒ 这些轮**根本没有中断会话**。所以"手工跑了同样的动作也不冻"这句话，
  不能当成"上卡无害"的证据 —— 手工那一侧的变量和链条那一侧本来就不一样。
- **12:07:54 / 12:08:33 两次链条**：也都死在 `m3irqup refused: interrupt objects still exist`，同样没开成。

### 10.3 两条修正，其中一条**推翻**本文件此前（和交接文档里）的结论

1. **"关掉日志捕获就不冻 ⇒ 元凶是 harness 写盘" 不成立。** 那两次（12:07:54 / 12:08:33）我同时改了
   **两个**变量：捕获关掉了，**而且 `m3irqup` 失败了、中断会话没开成**。混淆实验不能定元凶。
   （另：早先那两轮 `CX3_NO_KLOG=1 … sudo …` 因为变量被 sudo 剥掉，捕获**其实一直开着**，那一版证据本来就作废。）
2. 目前最窄、还站得住的相关关系：**冻屏只出现在"中断会话开成且中断在投递"的运行里（3/5：10:58、11:13、11:49）；
   `m3irqup` 被拒的运行一次没冻（0/4）**。样本小，但这比"上卡操作"精确得多。

### 10.4 为什么还不能就此定案

- `irqup OK` 的 5 次里有 2 次（10:41、12:05:27）没冻 ⇒ "中断会话开着"**不是充分条件**；
- 10:58 那次是**关掉会话之后 2 秒**才死，11:13/11:49 是**开着**时死 ⇒ 也不能说"死在打开的那一刻"；
- 10:02 与 10:47–52 两次离最后一次调用有 1–5 分钟 ⇒ 至少有一部分是**延迟效应**：
  留下来的东西（事件源 / 已武装的 EQ / 电平触发的中断线）过一会儿才爆，而不是那一次调用。

### 10.5 下一步：一次只动一个变量

1. **对照组（零成本）**：现在中断对象是泄漏状态，`m3irqup` 必然被拒 ⇒ 跑一次链条，**预测它不冻**，
   停在 `interrupt session FAIL`。这既验证 10.3 的相关性，又顺带把分发表/守卫改动跑一遍。
2. **单变量实验**：`cx3ctl m3irqdown` 清掉泄漏，然后只跑到 `irqup` 为止（`CX3_STOP_AFTER`），
   **然后什么都不做，静置 2–3 分钟**（watchdog 5 秒心跳在记时间戳）。
   死 ⇒ 最小 9 次调用就能复现，嫌疑锁定中断会话本身；活 ⇒ 嫌疑转到"会话开着时的后续动作"
   （`portup` 的链路轮询、PORT_CHANGE 事件、`netarm`、`r4raw`、`txprobe`），继续二分。
3. 每轮开跑前先 `date` + 抄一行 watchdog 尾部，跑完再抄一行 —— 这样即便硬死也能定出 5 秒精度的时刻。

### 10.6 10.5 第 1 步的结果，与两条更硬的新事实（2026-10-13 12:16–12:18）

**对照组跑了**：`sudo -A ./tools/verify.sh run`，12:15:59 起，12:16:22 结束（22 秒），
`interrupt session FAIL status=0xffffffffe00002d2`，机器**没冻**（watchdog 心跳连续，ws_cpu 一直涨）。
至此"`m3irqup` 被拒 ⇒ 不冻"今天已 6 次：11:59:01、11:59:33、12:04:55、12:07:54、12:08:33、12:16:00。

**新事实 1：`m3irqup` 每次开机只能成功一次 —— 这是代码问题，不是设备问题。**

- `exportM3IrqUp` 的守卫是 `if (fIrqObjectsUp || fIrqSource || fWorkLoop)`（`src/Cx3Mlx4.cpp:5928`），
  而 `irqReleaseObjects()` **故意不释放 workloop**（`:5855-5858`：它是 family 用的那个 loop，
  要在 `stop()` 里才放）。
- 两者口径不一致 ⇒ 第一次会话关掉之后，`fWorkLoop` 还在，守卫就永远拒绝。
- 实测吻合：今天唯一一次成功是本次开机的 **12:05:27**；之后 12:08:15、12:08:55、12:16:00、12:17:02
  全部 `refused: interrupt objects still exist (enabled=0)`。而 12:05 之前的拒绝理由是另外两条
  （`no EQ is up`、`the interrupt clear is not set up`）—— 也就是 12:05 之前 `fWorkLoop` 还没建。
- 推论：**要做 10.5 第 2 步的单变量实验，必须重启**（或先把这个守卫改掉）。

**新事实 2：整份驱动里没有任何"关掉设备侧中断投递"的步骤，只有 ack。**

- 与中断有关的设备侧写操作只有一处：`clrIntWrite()` 写 `clr_int`（`Mlx4Hw.cpp:5788` 附近），
  它是**应答**；`grep -rn "SET_INT\|setInt\|INT_MASK" src/` 没有任何"屏蔽/关闭投递"的写入。
- 而这条源是 **legacy 电平触发** INTx（`getInterruptType` 报 `type=0x1`，不是 `PCIMessaged`），
  电平线的撤销靠"消费掉 EQ 里那个事件 + 写 clr_int"。
- 于是有一个与观测形状完全吻合的机制候选：**源被禁用/移除之后，设备侧电平线又被拉起来
  （link/port 事件、或会话期间的后续事件），此时没有任何人去撤它 ⇒ 电平输入一直有效，
  ISR 里没人 EOI ⇒ CPU 反复进中断 ⇒ 硬死**。这正好解释"没有 panic 报告、统一日志尾部全丢、
  有时几分钟后才死"；`irqAction` 里那段注释（`:5756-5770`）记的正是本项目上一次同形状的 wedge。
- 检验方向（回**参考实现**而不是猜）：`mlx4_eq.c:1536 mlx4_disable_interrupts()` 在关机/卸载路径上
  先 `disable_irq` 再 `free_eq`；`:841` 的 handler 先写 `clr_mask` 再消费、`:824/829` 再 `eq_set_ci` 重新武装。
  我们缺的是"释放源之前先让设备侧不可能再拉线 + 保证 EQ 被排空"这一步。

**不能因此定案的理由（照 10.4）**：本次开机 12:05:27 会话开过也关过，机器到现在（12:18）活着 ——
所以这不是"一开就死"的确定性关系，而是间歇的。

**还有一个反例必须写在案上**：冻屏 A（~10:02）所在的那次开机（08:22:38 开机）**整个生命周期
一次 `m3irqup OK` 都没有**（08:20–09:00 与 09:00–12:12 两段日志查询都是空）。
所以"上卡操作/中断会话"能解释 4/5，解释不了那一次 —— 那次的形状也不同（两个 cx3ctl 只留下进程 map，
连 `user client opened` 都没有）。

### 10.7 0.44.21 的两处改动：把"会话边界"补上，以及它的预测（2026-10-13 12:28，已装、等重启）

改的是**代码**，不是设备；两处都来自 10.6 的两条新事实，且都只动一个变量。构建链 PASS
(`bash build.sh`，含 check-boot-path / check-selector-tables / check-hardcoded 与 46/46 选择器行检查)，
磁盘版本 **0.44.21**，暂存进 AuxKC 的那份二进制与构建产物 sha256 一致
(`11940f775f89a599a71f7e6a05cd2cce46f583a5ae119f39995b96feb2f22566`)。

1. **`m3irqdown` 现在按"应答 → 排空 → 停投递 → 把队列交还固件"的次序关闭会话**
   （`src/Cx3Mlx4.cpp` `exportM3IrqDown`）。交还那一步调用的就是项目里已有的
   `teardownEq(why, /*hcaWasReset=*/false)` → `HW2SW_EQ`，也就是参考实现拆卸时的**第一条语句**
   （`mlx4_eq.c:1080`）。**这是一次方向反转**，代码注释里按本项目的规矩写明了：
   原先"不碰 EQ"是有意为之，现在改掉，代价是"关掉会话后还想要 EQ 就得重建"（每次运行本来就会经过一次复位，m3equp 会重建）。
2. **`fWorkLoop` 从 `m3irqup` 的"对象还在"判据里拿掉**（`src/Cx3Mlx4.cpp:5928`）。
   它是控制器的对象，不是会话的对象；把它算进去就等于"每次开机只能开一次会话"，见 10.6 新事实 1。

**预测（可被证伪）**：如果 10.3 的机制对，那么下一次开机跑链条时——

- 关闭会话的日志里会出现 `HW2SW_EQ(eqn 28): ok`，并且
  `m3irqdown: ... N EQE(s) were still in the queue when it closed` 的 N 就是**关闭那一刻线上是否还压着事件**的读数；
- 同一开机内**第二次**开中断会话应当成功（旧版本必然被拒，见 10.6）；
- 若机器照旧在"会话打开之后/关闭之后不久"硬死，且形状不变（无 panic、心跳断），
  则该机制**被证伪**，嫌疑转向会话**开着时**的后续动作（`m3portup` 的链路轮询、PORT_CHANGE 事件等待、
  `netarm`、`r4raw`、`txprobe`），用 `CX3_STOP_AFTER` 一级级加。

**留一个已知残留（未修，写明）**：`irqAction` 里的 storm guard 触发时只 `src->disable()`，
不做交还 —— 真发生风暴时仍是同一形状。本机至今没触发过（每次关闭都记 `storm tripped no`）。
不在这一版里改它，因为那就是在中断上下文里释放自己的环形内存，属于另一类风险。

> 判读顺序（重启后按此走，先小后大）：`probe`（只读）→ 链条（看 `interrupt session open` 与
> `HW2SW_EQ` 两行）→ **同一开机再跑一次链条**（验证 F17 修好没）→ 然后是最窄的那次实验
> `sudo tools/session-idle-test.sh`：9 次调用把会话打开、静置 120 秒、关掉会话、再静置 60 秒，
> 每 10 秒一行时间戳 —— 硬死时那最后一行就是死亡时刻（10 秒精度，且不再依赖别的会话的心跳文件）。
> 10.5 第 3 步的 `CX3_STOP_AFTER` 二分排在它后面：这条最小路径如果就复现，二分的**前提**就变了。
>
> 另有一条保命开关：**`CX3_SKIP=txprobe,arm`**。这两个实验夹在 R4 段与接口步骤之间，都从未上机，
> 而它们**不是接口的前置条件**（`netup` 要的是 R4 建好的那些环，不是这两个实验）。所以如果某次
> 运行死在这两步里的任何一步，下一次加上这个开关就能把接口步骤跑出来 —— 那正是 M4 要的那一步，
> 不该被两个诊断挡在后面。跳过的步会在汇总里记成 SKIP，banner 里写明是哪个开关跳的。

### 10.8 冻屏已定位到**选择器 39（`netup`）**；会话修法实测通过（2026-10-13 13:2x–13:3x）

#### 10.8.1 机器外证据（并行取证会话提供，权威且不可自欺）

13:22:34 那次运行整机死亡，全机**任何子系统**的最后一条日志是：

```
13:22:35.035  (Cx3Mlx4) enter selector 39 (scalarIn 0, scalarOut 6)      ← 到此为止
13:22:34.831  r4tx OK ... / user client closed
13:22:35.024  map_with_linking_np: [16262(cx3ctl)] / user client opened
```

按本项目自己在 `Cx3Mlx4UserClient.cpp` 里定的判读规则（`closed` 之后**跟着** `enter N`
⇒ teardown 已完成 ⇒ 卡住的是选择器 N），**卡的是 `netup`**。而且统计很硬：**`netup` 至今只被调用过两次，
两次都以整机死亡收场（2/2）**（今早 10:41:59.319 那次同样止于 `enter selector 39`）。
死亡形状：最后一次健康探针 13:22:35.312（`ws_cpu` 仍在涨、死前 40 秒毫无征兆），下一次探针超时，
**13:22:38 有线 ICMP 死** ⇒ 整机级硬挂，**无 panic 文件**。

**两条旧结论据此修正**：§3 的"已排除某条固定选择器"只覆盖手工发过的那 7 条 R4 选择器、**没覆盖 netup**；
§2 的"冻屏只绑定 user client close"被这次推翻（这次是 `closed` → `enter 39`）。

#### 10.8.2 同一批证据顺手证明的两件事

1. **会话修法通过。** 用 `CX3_STOP_AFTER="the interface (10a)"` 把链条停在接口**之前**跑完（13:31:34，
   `outputs/verify-20261013-1331*.log`）：`interrupt session closed` 之后机器活着，而且 **R4 阶段从
   `r4cq` 起全部跑通**（0.44.24 的那个回归已由 0.44.25 的"只交还队列、不拆状态"修掉）。
   里面最重的一条读数：**`TX WQE: 513 post(s)` 全部读回正确、`TX completion: 513 arrived`、
   `RX frames: 88 completion(s)` 全部指向我们的 QPN 64、并读出真帧内容** —— 发送与接收这条链路
   在真机上是工作的。
2. **一条新的独立读数**：`netarm` **FAIL** —— "队列已空、arm 已敲、2000 ms 内没有事件"。它不阻塞 M4
   （我们的收包是定时器轮询，不依赖 arm），但它是真结论，已在 10.6/10.7 之外单独记一笔。

#### 10.8.3 下一次 netup 之前装上的仪器（0.44.26）

`exportNetUp` 现在有五条**成对**的逐步日志（`netup 1/5` 到 `5/5`，各在开工前与收工后各一条），
外加进入时一条与返回前一条：

```
netup: entered (unit N, interface ..., and this selector HOLDS the transaction lock)
netup 1/5: QUERY_PORT in / out          ← 固件邮箱往返
netup 2/5: attachInterface in / out     ← 家族建 interface+nub，会回调进驱动
netup 3/5: publishLinkStatus in / out   ← 只 setLinkStatus，不发命令
netup 4/5: netServiceStart in / out     ← 建收包定时器并挂到 workloop
netup 5/5: netTxResourcesEnsure in / out
netup: returning (...)
```

判读规则与 10.8.1 同一条：**有 `in` 无 `out` 的那一步就是停住的地方**；从未出现 `in` 则是"没走到"。
`netup 2/5` 那一步跑的是别人的代码（`attachInterface` 会回调 `configureInterface` /
`getHardwareAddress` / `getPacketFilters` / `getMaxPacketSize`），代码里已写明这些回调**都不得**
取事务锁或发命令 —— 现在没有一个取，所以这一步安全；将来谁加了，就会死在这一行。

#### 10.8.4 下一次尝试怎么读

并行会话的机器外黑匣子（`http://127.0.0.1:8099/state`、`/dump?secs=N` 经 socket 拉全系统栈、
`/diag`）在异常时会**自动抓 dump**，所以下一次 netup 若再挂，判读顺序是：

1. 全机最后一条 `netup N/5` → 停在哪一步；
2. 黑匣子的自动 dump → 那条线程的栈（`attachInterface` 内部？还是邮箱等待？）；
3. LAN VM 的 `~/mac-watch/logs/broad-*.log` —— 这台机器上唯一能留下冻屏当下日志的地方。

两条候选机制仍由这一对读数分开：邮箱往返不返回（总线/固件级）vs `attachInterface` 内部挂住（家族代码）。
**不要**再加宽同步面去"试试看"：每一步都已经能自证。

### 10.9 四次死亡全部落在 `netup` 第 1 步；仪器再往下一层（2026-10-13 13:2x–13:59）

| # | 时刻 | netup 前面的状态 | 结果 |
| --- | --- | --- | --- |
| 1 | 13:22:35 | 刚跑完 r4tx | 死在 `enter selector 39` |
| 2 | 13:40:29 | r4raw → txprobe → netarm（CQ 被 arm），空档 25 s | 死在 `netup 1/5: QUERY_PORT in` |
| 3 | 13:46:22 | clrint → irqup（legacy 源装上、EQ 重新 armed），空档 15.6 s | 同上 |
| 4 | 13:50:58 | 只有 INIT_HCA，**176 ms 前独立 QUERY_PORT（选择器 24）刚成功** | 同上 |

**据此排除的**：① "没人应答的电平线"——#3 特意让中断会话开着（有人在听）**照样死**，#4 连会话都没开；
② "前面某一步留下的状态"——四次的前置状态各不相同，而 #4 里同一个函数相隔 176 ms 一次成功一次致命；
③ "驱动自己的等待逻辑"——死前 1–3 秒，连超时/中毒那行都没打。
**留下的问题**：同一个 `queryPort`，同样的锁级别、同样的 polled 模式，为什么从 24 调用成功、从 39 调用致命。

**这一轮用最便宜的一次判别缩了范围**（13:59:00）：在**未初始化**状态（本开机一次设备调用都没发过）调 netup，
它**干净、即时返回**：`out=[0xffffffffe00002d8 (NotReady), 0x1 (=CX3_NETUP_NO_DEVICE), …]`，机器毫发无损。
⇒ `queryPort` 之前的代码（含 `out[]` 初始化、两条日志、结构体清零）无罪，**致命性需要命令真的走完投递路径**。
（诚实边界：未初始化时 `cmdBoxLocked` 在"邮箱中毒检查"处就早退了，**没有读寄存器、也没投递**，所以这一刀只切掉了
"命令之前"，没切掉"投递之前"。）

**读代码排除的**：`queryPort` 写回调用者结构体的每一处都在界内 —— `raw[64]` ↔ `memcpy(..., 64)`，
`defMac[8]` 正好 8 次，`cx3QueryPortDecode` 的每次写都落在结构体内部。所以"越界砸栈帧"这个形状（能解释
"24 写页缓冲无事、39 写栈帧致命"）**在源码里找不到落点**，暂不作为假设。

**因此 0.44.27 把仪器再往下一层**（取证会话的建议 4），下一次硬死能落到"某一次寄存器访问"：

* `queryPort` 内部四点：`mailbox in` → `mailbox ready + posting` → `the command returned` → `decoded`；
* `cmdBoxLocked` 三点：`idle, writing 7 dwords and then the GO bit` → `posted (GO set); polling` →
  轮询里**第一趟 + 之后每秒一条** `still outstanding N ms (... HCR reads 0x%08x)`。

判读：最后一行是 `writing...` ⇒ 死在 MMIO 写；是 `still outstanding N ms` ⇒ 轮询中停住（并给出 N 与 HCR 读数）；
是 `posted` 之后立刻没有下文 ⇒ 死在第一次寄存器读。**三种形状对应三种机制**，一次开机一次调用即可分辨。

### 10.10 二分落地：凶手是 `attachInterface`（netup 第 2 步），2026-10-13 15:07

`netupstop`(46) 加了一个输入 = **"停在第 N 步"**（同一份实现 `netupRun`，`netup` 就是"不停"），
一次开机按 1→2→3→4→39 走。结果（机器外证据，两条独立复现）：

```
15:07:02.944  46 停1：读地址 → 返回            → returned 0x0   ← 活
15:07:05.554  46 停2：再走进 attachInterface   → 之后再无任何一块面包屑；≲1 秒后 ICMP DEAD
```

**同一条路、同一个前缀，唯一差别就是"要不要走进第 2 步"。** 这是今天第 10 次 netup 死亡，也是第一次
能把凶手指到**具体一个调用**。

顺带排除的（取证会话读代码 + 我们自己的二分）：

* 家族在 attach 期间回调进我们的方法（`configureInterface`/`getHardwareAddress`/`getPacketFilters`/
  `getMaxPacketSize`，以及 `enable`/`disable`/`outputPacket`/`setMulticastMode`）**都不碰设备、不取锁**；
* 分派 / 取锁 / open 路径：同一分钟内 open 成功 ×4、`46 停1` 成功返回；
* `doRegister=false`（0.44.33 按文档把"注册"推迟到四步之后）**没能避免** ⇒ 致命性在 `attachInterface`
  自身内部，不在"接口对系统可见之后"。

**0.44.35 把 attach 内部的三个钩子各插一对进/出标记**（`registerWithPolicyMaker` → `createInterface` →
`configureInterface`，顺序见 IONetworkController.h:1137 与 :1256-1276），三处都调用**声明它的基类**、
行为保持默认 —— 它们是温度计不是改动。下一次死亡会报出**死在哪一个钩子**；若死在两个钩子之间，那就是
家族自己的 nub 创建/注册那段，属于另一类结论。下一次死亡的三种读法与各自的下一步都写在上面表格里。

> 小坑记一笔：本文件所在的 translation unit 里 `super` 解析成 `IOService`（编译报错原话），
> 而这三个方法声明在 `IONetworkController`/`IOEthernetController` 上 ⇒ 调用点必须用**显式基类限定**。

> **2026-10-21 23:24 更正**：这不是无害的编译限定问题。头文件仍是
> `typedef IOService super`，而 C++ 直接基类已经是 `IOEthernetController`，
> 导致 init/start/stop/free 跳过网络框架生命周期。仅在三个 hook 显式写基类
> 没有修复这个缺陷。0.44.43 离线候选修正 alias 并增加编译期断言。

### 10.11 选择器入口状态机：区分“没进驱动 / 等锁 / 在 dispatch”（0.44.37）

2026-10-21 10:06 的复现里，60 侧只收到工具的
`pre selector 46 netupstop 2`，随后机器冻结；`cx3NetupStep=0`、
`cx3AttachHook=0`，连 `enter selector 46` 都没有落盘。因此“attach 内部挂住”
仍不是可由现有证据单独证明的结论——还缺一段：调用是否进入 driver、是否卡在事务锁。

0.44.37 在 user client 入口增加了一对 registry 属性：

```text
cx3ClientSelector = 当前 selector
cx3ClientPhase    = 当前阶段
```

阶段值：

| phase | 含义 |
| --- | --- |
| 0 | idle / 本次 boot 尚未调用 |
| 1 | 已进入 `externalMethod` |
| 2 | 进入 entry quarantine sweep |
| 3 | quarantine sweep 已返回 |
| 4 | 正在等待 transaction lock |
| 5 | 已拿到 transaction lock |
| 6 | 即将调用 `super::externalMethod` |
| 7 | dispatch 已返回 |
| 8 | transaction lock 已释放 |
| 9 | TryLock 被拒绝（`kIOReturnBusy`） |

`netup-watch.sh` 现每 100 ms 同时推送：

```text
call=<selector> phase=<phase> step=<netupStep> hook=<attachHook>
```

2026-10-21 11:29 的首次自测发现旧 watcher 本身有一个假读数缺陷：它执行的是
`ioreg -n Cx3Mlx4 -d1 -w0`，在 24H6 上返回 **0 字节**；脚本于是把缺失值打印成
`call=0 phase=0 step=0 hook=0`，与“driver 真写了 0”无法区分。0.44.37 安装后
`/usr/local/bin/cx3ctl probe` 实测确实写出了 `cx3ClientSelector=0`、
`cx3ClientPhase=8`；修正后的读取命令是：

```sh
ioreg -r -n Cx3Mlx4 -d 1 -l -w0
```

watcher 现在还会输出 `read=ok` 或 `read=EMPTY`。今后 `EMPTY` 表示探针读通道坏了，
不再伪装成 phase 0。

判读：

- 最后 `call=46 phase=1`：调用进入 driver，但死在 sweep 之前；
- `phase=2`：死在 quarantine sweep；
- `phase=4`：没有拿到 transaction lock；
- `phase=5`：拿到锁，死在进入 dispatch 前；
- `phase=6 step=0`：死在 `super::externalMethod` / dispatch 入口；
- `phase=6 step=2 hook=0`：已进入 netup 第 2 步，死在 `attachInterface` 内任何一个 hook 之前；
- `phase=6 step=2 hook=1/2/3`：死在该 hook；
- 完全没有 `call=46`：调用没有到达 driver，下一次转向 IPC/IOUserClient 入口。

本轮只安装取证版本，不调用 selector 46。下一次复现必须同时运行
`cx3freeze`、UDP watcher 和这个 phase 状态机。

### 10.12 open 路径状态机：补上 `externalMethod` 之前的空白（0.44.39）

2026-10-21 12:46 的复现再次停在用户态打印
`pre selector 46 netupstop 2` 之后。60 侧收到 `cx3ctl` 的
`pre selector` UDP，但驱动仍停在 selector 24 的
`call=24 phase=8`；没有 selector 46 的 `phase=1`。

复盘发现两个仪器没有接通：

1. `cx3ctl` 已有 `open: calling IOServiceOpen` / `open: IOServiceOpen returned`
   等 breadcrumb，但复现脚本没有导出 `CX3_BC`，所以它们没有发送；
2. 本机未签名的 `cx3ctl` 即使设置 `CX3_BC`，LAN UDP 也可能被系统以
   `EHOSTUNREACH` 拒绝。

0.44.39 因此增加三个纯观测面：

```text
cx3OpenPhase:
  0=idle, 1=newUserClient entered, 2=client allocated,
  3=initWithTask returned, 4=attach returned, 5=start returned,
  6=newUserClient returned, 7=clientClose
  10=no memory, 11=init failed, 12=attach failed, 13=start failed
```

`cx3ctl opentest` 只做 `IOServiceOpen` + `clientClose`，不调用任何 selector；
`cx3ctl bctest` 用同一二进制验证 `CX3_BC` 的 `sendto`。构建脚本会对
`cx3ctl` 做 ad-hoc codesign，复现脚本会在 selector 前强制：

```text
codesign --verify --strict
CX3_BC=<same endpoint as cx3watch>
cx3ctl bctest
cx3ctl opentest
```

下一次最后样本的判读：

| 最后证据 | 结论 |
| --- | --- |
| 用户态 `pre selector`，没有 `open: calling IOServiceOpen` | 卡在 `cx3ctl` 发送前的本地路径 |
| 有 `open: calling IOServiceOpen`，没有 `open returned` | 卡在 `IOServiceOpen` trap 或 `newUserClient` |
| `open returned`，`cx3OpenPhase` 停在 1/2/3/4/5 | 分别卡在分配、`initWithTask`、`attach`、`start` |
| `cx3OpenPhase=6`，但没有 selector `phase=1` | open 已成功，卡在返回用户态与 selector 分发之间 |
| selector `phase=1` | 已进入 `externalMethod`，才开始用后续 phase 判读 |

### 10.13 attach 的 work-loop gate 实验（0.44.40；22:27 更正）

**更正**：`IOWorkLoop::runAction()` 在调用线程持有 work-loop gate 执行动作，
不会派发到 work-loop 线程。此前把 0.44.40 描述为“切换线程”是错误的。
以下实验只改变 gate 上下文，不能检验“必须在 work-loop 线程 attach”的假设。

2026-10-21 21:23 的同 boot 夹逼（`outputs/20261021-cx3-open-probe/FINDINGS-20261021-2123.md`）
把致命点压到一步：

```text
opentest / 3 / 2 / 10 / 8 / 13 / 14 / 24   全部 0x0
netupstop(46) in=1                          0x0，内核日志完整：
                                            enter selector 46 -> netup 1/5 ->
                                            stopped after step 1 -> returning(0)
netupstop(46) in=2                          整机在约 1 s 内硬停
```

两条旧结论因此作废：**不是 open 路径**（`opentest` 两次 PASS），
**也不是"没进 selector 46"**（`in=1` 的每一行内核日志都实时到达了 60；
12:46 那次看不到是 logd 没落盘）。

`in=2` 之后三个通道同时断：内核日志连 `user client opened` 都没送出、
registry 采样在最后一个 `in=1` 样本后连 1 秒心跳都没有、重启后 `log show`
也查不到上一个 boot 的记录。registry 读被阻塞说明停顿发生在内核层。

0.44.38 的后台 worker 因此没有起到保护作用：attach 已经被移到独立
thread_call 线程、调用方还有 10 s 超时，但整机在约 1 s 内硬停 —— 是 worker
线程里的 `attachInterface` 自己拖死了内核。

0.44.40 的改动只有一处：worker 不再自己调 `attachInterface`，而是通过
`getWorkLoop()->runAction(&Cx3Mlx4::netAttachOnWorkLoopAction, this, &ctx)`
在调用线程持有控制器 work-loop gate 执行，
并新增 worker 状态 `8 = gated callback entered`。`in=1`、selector 表、`netupRun` 的其余步骤未改。

判读方式：

- `worker=8` 之后变成 `3/4`：gated attach 返回，分别表示成功/失败；
- `worker=8` 后再无样本：最后已观察到 callback 入口，仍需排除采样中断；
- 没有 `worker=8`：没有观察到该标记，不能据此断言 `runAction` 没执行。

### 10.14 0.44.41 自检不可达（22:27 离线证实，0.44.42 候选）

22:13:28 远端记录 `pre selector 46 netupstop 6`，22:13:29.111 DEAD。
但 `exportNetUpStop()` 中仍有 `if (stopAfter > 5u) stopAfter = 5u`，
所以这轮实际走的是 `netupRun(5)`，不是 no-attach 自检。
0.44.41 实际二进制 `exportNetUpStop+0x49..0x56` 也有 compare 5 / store 5。
**撤销“no-op 冻死，证明 runAction 本身死锁”的推断。**

0.44.42 保留 6，先按 64 位校验范围，再缩窄为 UInt32；>6 返回 BadArgument，
不会误入 attach。主机回归测试直接编译生产入口函数，只有 `netupRun` 用记录参数
的 sink 替代；旧代码 `input=6 -> route=5` 失败，新代码 11 个边界用例通过。
该测试只证明输入路由，不证明 IOKit gate 或 attach 能在真机正常返回。

0.44.42 离线构建 PASS，尚未安装/重启/运行 selector。后续仍需明确复现放行，
先且只运行修正后的 `in=6`，不得自动继续 `in=2`。

### 10.15 0.44.42 真机 gate 自检 PASS（23:21）

boot 23:18:18，实际加载 0.44.42，UUID `771A013B-7BA8-35AD-B5FF-50610860F993`。
前缀 opentest / 3 / 2 / 10 / 8 / 13 / 14 / 24 / 46 in=1 均正常返回。
23:21:26.191 内核确认 `stopAfter=6`，runAction 返回 0、ran=true；
23:21:26.199 远端 watcher 读到 worker=9，用户态 call=0，POST rc=0。
23:23:14 结束采样前持续存活。没有运行 attach，没有安装下一版本。

该结果排除了“这个 no-attach gate 动作在当前条件下就会冻死”；它没有
验证 attach，更没有证明原始冻屏已修复。注意该路径打印的通用 tail
“stopped after step 6”不表示执行过 2..5，自检在 attach 之前直接退出。

### 10.16 网络框架生命周期被跳过（23:24，0.44.43 离线候选）

本轮 no-op 第一次调用 getWorkLoop 时才创建 loop，与 start 没执行家族初始化吻合。
当前 registry 没有 IOLinkStatus、IOLinkSpeed、IOActiveMedium、IOCurrentMedium。
源码直接基类/OSDefineMetaClassAndStructors 均为 IOEthernetController，
但 `typedef IOService super` 使四个生命周期调用跳过家族实现。

Apple 公开 IONetworkingFamily 源码（commit `bb910e2b458939f6b1f53c0fa6907f258b431ea4`）
中 IONetworkController::start 创建 _workLoop、_cmdGate 和链路属性，
attachInterface 首先调用 executeCommand，后者直接 `_cmdGate->runAction`，
发生在 registerWithPolicyMaker/createInterface/configureInterface hooks 之前。
这可以解释已有“地址读取成功、attach 前段死亡、没有 hook 标记”的组合。
本机 SDK IONetworkController.h:385–405 同样要求子类先执行父类 start。
公开源码不是当前 24H32 二进制的反汇编，故仍需真机因果验证，不能定为唯一根因。

0.44.43 只改 super 为直接基类、增加编译期类型断言、修正注释和版本号。
先加断言而保留 IOService，构建确实 FAIL（2 个 translation units）；
改为 IOEthernetController 后构建 PASS（4/4），入口路由 11 cases 继续通过。
候选 binary sha256 `4b01ae1cd2411b7cbb8a901322f83074a832616a7544d408cf79816d078caafd`。
尚未安装。下一轮先验收 boot 时 createWorkLoop 和家族链路属性，
不得默认直接重复已知致命的 attach。
