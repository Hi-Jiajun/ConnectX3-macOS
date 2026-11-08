# 有界超时 + 毒化邮箱：实现说明（0.43.0）

本文是 `docs/LOCK-DESIGN.md` 那份设计的**落地记录**。设计说"应该怎么做"，这里说
"做成了什么、三个关键问题各自的答案是什么、哪些上过机哪些没有"。

范围：`src/Mlx4Hw.cpp` / `src/Mlx4Hw.h`（命令路径）、`src/cx3_cmdgate.h`（新的纯函数门）、
`src/mlx4_regs.h`（常量单位）、`src/cx3_abi.h` + `src/Cx3Mlx4UserClient.cpp`（故障注入）、
`src/Cx3Mlx4.cpp`（重置路径清毒）、`src/flr.inc`（FLR 也清毒）、`tools/cx3ctl.c`、
`tools/verify.sh`、`build.sh`、`tools/test-cmd-wedge.c`（新）。

---

## 1. 改了什么

一句话：**把"等待"从迭代计数改成墙钟截止时间，把"放弃"从返回错误改成毒化邮箱，
并给恢复路径留了一条不经过命令路径的路。**

| 改动 | 位置 | 为什么 |
| --- | --- | --- |
| 所有超时改成真毫秒（`clock_get_uptime` 截止时间） | `Mlx4Hw.cpp` 的 `cmdBoxLocked` | `GO_BIT_TIMEOUT_MSECS = 10000` 不是 10 s 而是约 **22.7 s**，`MLX4_CMD_TIME_CLASS_A = 60000` 约 **126 s**——名字说毫秒，代码是循环次数（E 家族）。那 22.7 s **每条命令**正是 6 min 07.6 s 持锁的主项 |
| 投递前的空闲等待 22.7 s → **50 ms**（`CX3_CMD_IDLE_WAIT_MS`） | 同上 | 参考驱动在轮询命令前**根本不等待**（`mlx4_cmd.c:456-459`：`poll_sem` 已经串行化了寄存器）。这里同样被锁串行化，所以这段等待只需覆盖"刚返回那条命令的尾巴" |
| 投递前等待超时 → **毒化**（`cmdPoison`） | 同上 | 见 §2 问题一 |
| 完成等待超时 → **毒化** | 同上 | 同上 |
| 毒化后所有命令**微秒级**失败，不读寄存器、不填邮箱、不投递 | `Mlx4Hw.cpp:707`、`:2986` | 状态在等待之前判定，所以不会"往一条卡住的命令上面再投一条" |
| `reset` / `FLR` **清毒** | `Mlx4Hw.cpp:457`、`flr.inc:105` | 两者都是寄存器级操作、**不投递任何命令**，所以毒化不是死路 |
| 新状态 `CX3_FWSTATUS_WEDGED = 0xfffffffb` | `mlx4_regs.h` | "没发出去"必须区别于"超时"（`0xfffffffd`）和"固件说 ok"（`0`）——同 B 家族的既有规则 |
| 等待的**决策**抽成纯函数并逐构建测试 | `cx3_cmdgate.h` + `tools/test-cmd-wedge.c` | 时序缺陷在主机上无法重现，但"结束等待的那个判断"可以：45 项断言每次构建都跑 |
| 故障注入选择器 33 | `cx3_abi.h`、`Cx3Mlx4UserClient.cpp` | 见 §4——它让恢复路径能在**不弄坏卡**的前提下被验证 |

---

## 2. 三个必须回答的问题

### 问题一：超时到点后"放弃"安全吗？

**不安全——所以放弃必须伴随毒化。** 论证（源码级，落在 `Mlx4Hw.cpp:498-522` 的注释里）：

邮箱是**一个** 4 KiB 缓冲（`MLX4_MAILBOX_SIZE`）。如果放弃后返回错误、让下一条命令照常走，
那么下一条命令的第一件事就是**清空这个缓冲**，而设备可能还在读被放弃那条命令的字节；
固件那条迟到的答复（如果会来）会落在状态字和 outbox 里，**被下一条命令当成自己的答案读走**。
两者产生的都不是失败，而是**看起来合理的错答案**——D 家族，本项目代价最大的那一类。

所以语义定成：**放弃即毒化**。这也是参考驱动自己的语义（`mlx4_cmd.c:634-638` →
`mlx4_catas.c:169-200`，`internal_err_reset` 默认 1）。

**代价说清楚**：一条"只是慢"的命令，从此变成"设备在重置前不可用"。换来的是
**有界的设备损失**替代**无界的驱动损失**——后者正是这次要修的那个失败（恢复路径
6 分钟进不去）。`docs/LOCK-DESIGN.md` §3.3 提的就是这一条。

### 问题二：`IOLockTryLock` 过得了符号检查吗？

**过了。** `src/Cx3Mlx4.cpp:2251` 用它实现"拿不到就告诉调用方"的语义；构建门禁里
**351/351 符号全部解析**（见 §3）。所以 `probe`/`survey` 不必排在锁后面：
另一个 selector 持锁时它们要么答出一个值、要么以 `kIOReturnBusy` 明确拒绝，
两种都是"没有排队"。

### 问题三：毒化之后 `reset` 这条恢复路径还可用吗？

**可用。** 毒化只挡**命令路径**（`cmdBox`/`hcrRun`）；`reset` 与 `FLR` 走的是寄存器级
操作，不投递命令，因此不受毒化影响，并且它们是**唯二**清毒的地方
（`Mlx4Hw.cpp:457`、`flr.inc:105`）。如果清毒本身要经过 `cmdBox`，毒化就是死锁——
这一点在代码注释里明确写了（`Mlx4Hw.cpp:544-552`），不是事后补的说明。

`verify.sh` 里对应的检查是 `reset (the cure)` 与 `the poison survived the reset`：
注入毒化 → 确认命令被拒 → `reset` → 确认毒已清。

---

## 3. 验证

### 3.1 构建门禁（本轮真实输出）

```
BUILD RESULT: PASS
  boot-path reachability: PASS (no device write / DMA / HCA reset reachable from start())
  mailbox encoding   : 146 checks, 0 failure(s)
  check-hardcoded.py : 8186 checks, 0 failure(s)
  CQ owner-byte      : 137 checks, 0 failure(s)
  command gate (new) : 45 checks, 0 failure(s)
  undefined in Mach-O: 351 / resolved by kmutil : 351  OK
  sha256             : 37b52fa0d7127e338df748cdad2d9bc6fde1f71d18b97dc5d02e1378901bf3d2
```

新增的那 45 项断言为什么值得存在：被修的是**一个时长**（一个 selector 持锁 6 min 07.6 s
因为每次等待都是"带毫秒名字的循环次数"），而时长没有设备就测不出来。可测的是
**把毫秒预算换算成墙钟截止时间的那一步算错没有**，以及状态机的四个转移——
特别是"已经毒化"必须在**读寄存器之前**判定、且**不会过期**。
`tools/test-cmd-wedge.c` 每次构建都跑（`build.sh` 里编译并执行），这正是
`docs/LOCK-DESIGN.md` §5.1 点名要的那个测试。

### 3.2 重启后要跑的（本轮未做：禁止安装与重启）

```sh
sudo tools/verify.sh stage     # 构建、装 0.43.0、然后重启
sudo tools/verify.sh run       # ← 重启后跑这一条
```

新增的可判读项（都在 `run` 里）：

| 检查 | 期望 |
| --- | --- |
| `probe` 的毒化位 | 干净启动必须是"未毒化" |
| `fault wedge` → 下一条命令 | 被拒，且**耗时微秒级**（不是秒级） |
| 毒化状态下的 `QUERY_FW` | 被拒，不开新等待 |
| **另一个 selector 持锁时 `probe`** | 答出值或 `kIOReturnBusy`，**不排队**（< 1000 ms） |
| `reset (the cure)` | 成功，并报告它到达时是什么状态 |
| reset 之后 | 毒已清，命令恢复可用 |

### 3.3 与另外两项改动的归因

0.43.0 同时含三项：EQ re-arm（`0c7a1d8`）、ICM unmap（`c39281f`）、本次超时/毒化。
**归因是清楚的，因为观测点正交**：

* 中断那一行验 EQ；`icm tables: ...` 两种措辞验 ICM；
  毒化/超时那几个新检查验本次。
* **万一链条第一步就挂**：本次改动是三项里**唯一落在每条命令公共路径上的**
  （`cmdBox`/`hcrRun`），所以第一步失败首先怀疑超时取值，
  方法是看日志里打印的**实际生效预算**与该步的真实耗时。

---

## 4. 可观测性：故障注入选择器（33）

设计里"必须能独立判读"那一条的实现。`cx3ctl`：

```sh
sudo /usr/local/bin/cx3ctl fault wedge        # 像一次放弃那样毒化邮箱
sudo /usr/local/bin/cx3ctl fault unwedge 1    # 清毒（1 是确认，否则拒绝）
sudo /usr/local/bin/cx3ctl fault hold 3000    # 持事务锁 3000 ms（1..30000）
```

它的价值是**让恢复路径在不弄坏卡的前提下可被验证**：以前要测"毒化之后 `reset` 还灵不灵"，
只能真把卡搞坏一次（本项目已经这么付过两次代价）。注入的毒化带**专用 opcode 标记**，
所以日志和 `survey` 里能一眼区分"硬件造成的"和"故意注入的"。

计数量也保留下来了：`fCmdAbandons`（放弃次数）、`fCmdRefused`（毒化期间被拒次数）、
`fCmdWedgedOp`（哪条命令毒的）。**故意不清零**——"总共发生了几次"是关于这次启动的事实，
不是关于毒化的事实。

---

## 5. 实测 / 推断 / 未测

### 实测（有日志时间戳、源码行号或可复现命令）

* 旧常量的真实含义：5000 次迭代 = 10.52 s、60000 次 = 185.36 s、10000 次 ≈ 22.7 s
  （从 2026-10-12 01:2x 那次卡住的**内核日志时间戳**量出，见 `mlx4_regs.h:63-80` 的注释）。
* 持锁 6 min 07.6 s 的窗口：01:27:12.202 → 01:33:19.843（`docs/LOCK-DESIGN.md` §1.4）。
* **新预算相对最慢的成功命令有余量**：`verify.sh run` 里最慢一步是 `MAP_FA` → `RUN_FW`
  固件启动，实测 **3.59 s**（01:39:57.610 → 01:40:01.201，同型另两次 3.59 / 3.58 s）。
  它落在 `CX3_CMD_BUDGET_FW_MS = 60000` 之下，余量约 16 倍。
* ICM 拆表那 14 条命令的实际耗时：`01:40:19.372–.421`，**合计 49 ms**，
  远低于 `CX3_CMD_BUDGET_ICM_MS = 5000`。
* 构建门禁与 45 项主机断言：§3.1。
* `IOLockTryLock` 的符号解析：351/351。
* `reset`/`FLR` 是唯二清毒点：`Mlx4Hw.cpp:457`、`flr.inc:105`（`grep cmdClearWedged`）。

### 推断（有据，但没有设备侧观测）

* **投递前的 50 ms 足够**：依据是"锁已串行化 + 参考驱动根本不等待"。
  反例形态：若某次真机上 `probe`/`fwload` 报 `ABANDONED before posting`，则 50 ms 不够，
  该值要按那次实测的等待时长上调。**这是本次最该盯的一处。**
* **毒化不会掩盖真正的固件故障**：毒化只在"已超预算"时发生，而预算已按实测最慢命令
  留了余量；但"固件真的变慢到超过 60 s"这种情况现在会被判成放弃而不是继续等——
  这是有意选择（有界优先），代价写在这里。

### 未测（需要重启 / 需要设备）

* **整条链没上机**：本轮禁止安装 kext 与重启，所以 0.43.0 的行为全部是预期而非实测。
* `fault wedge` / `unwedge` / `hold` 三条路径**从未在硬件上跑过**（它们是新选择器）。
* `probe` 在锁被持有时的两条分支（答值 / `kIOReturnBusy`）**从未跑过**。
* 真机上"放弃"路径没有被真正触发过（希望永远不触发）；它只有主机侧状态机测试。

---

## 6. 回滚

代码回滚（改动是加法为主，没有调用点被移动）：

```sh
cd /Users/you/Documents/Hackintosh-MacLocal
git -C tools/cx3-mlx4 revert --no-edit <本次提交>
BUILD_TAG=0.43.0 bash tools/cx3-mlx4/build.sh
sudo tools/cx3-mlx4/install.sh      # 装回；随后重启
```

运行期回滚（新 kext 重启后行为不对时）：

```sh
sudo tools/cx3-mlx4/rollback.sh            # kmutil unload + 残留检查 + kmutil rebuild
sudo tools/cx3-mlx4/rollback.sh --purge    # 再删 /Library/Extensions/Cx3Mlx4.kext
```

两者都要重启才生效。回滚后的行为：超时重新变成"约 2.1 倍于名义值的迭代计数"，
一步卡住会重新让驱动对用户态不可达数分钟，但**不会**毒化。

---

## 7. 同一形态、本次没改的地方

按 `docs/DEFECTS.md` 的规则列出，免得读者以为这里已经清干净：

| 位置 | 形态 | 为什么没改 |
| --- | --- | --- |
| `cmdBoxLocked` 的事件路径 | 投递前等待仍是 10000 真毫秒 | 那条路径可能在等 completion EQE 而不是等寄存器变闲，压缩它需要单独论证 |
| `reset` 的 `bypassSem` 分支 | 绕过 ownership 门但不绕过毒化检查 | 语义正确（它本来就要在不干净的状态下工作），但没有专门的测试覆盖，仅在文档里说明 |
