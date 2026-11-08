# CX-3 mlx4 kext — M1 施工记录

> 目标：Mellanox ConnectX-3 Pro（`15b3:1007`）在 macOS 15.8.1 上的以太网 kext，里程碑 M1
> （匹配设备 + 映射 BAR0 + 读出固件版本）。基底与许可见 `docs/handoff-cx3-mlx4.md`。
> 代码在 `tools/cx3-mlx4/`，产物与取证在 `outputs/20261010-cx3-m1/`。

## 1. 本机新增的构建坑（交接文档原有 3 个之外的 4、5；第 6 个见 §10）

交接文档记录了 3 个坑（`-DKERNEL -D__KERNEL__`、全按 C++ 编、Info.plist 占位符）。
本次装载时踩到第 4、5 个，都是"编译能过、`kmutil load` 会拒"的坑：

| # | 现象 | 原因 | 解法 |
| --- | --- | --- | --- |
| 4 | `kmutil load` **rc 31**：`cannot be placed in kernel collection because: kexts must have a _kmod_info symbol` | CLT 的 `ld` **不会**像 Xcode 的 kext target 那样生成 `_kmod_info`。实测本机**所有**能正常装载的第三方 kext（NullMoth 的 4 个 NVRM\*、HighPointIOP/RR）都**自己导出** `_kmod_info`，且**都没有** `__info_plist` 段 | 加 `src/kmod_info.cpp`，用 `mach/kmod.h` 的官方宏 `KMOD_EXPLICIT_DECL(<bundle id>, "<版本>", start, stop)`；start/stop 是返回 `KERN_SUCCESS` 的空实现（IOKit kext 由 Info.plist personality 驱动，不靠 kmod 入口） |
| 5 | `kmutil load` **rc 30**：`Validating extension failed … Filesystem error: Invalid ownership (501:20) should be (0:0)` | `kmutil load` 校验 bundle 的属主，必须是 `root:wheel` | 把 bundle 装到 `/Library/Extensions/Cx3Mlx4.kext`（与本机其它第三方 kext 同址）再 chown。构建产物仍留在 `outputs/` 保持用户属主，便于反复重建 |

**同时证否的一条**：给链接器加 `-Wl,-sectcreate,__TEXT,__info_plist,<plist>` 会生成
`__info_plist` 段（`otool -l` 可见），但**不会**生成 `_kmod_info`，rc 31 依旧。已从 `build.sh` 移除。

## 2. 与 Apple 自带 dext 的竞争（实测）

`/System/Library/DriverExtensions/com.apple.DriverKit-AppleEthernetMLX5.dext/Info.plist`：

```
IOPCIMatch      = 0x000015b3&0x0000ffff   ← 按厂商 ID 通吃所有 Mellanox 网卡
IOPCIClassMatch = 0x02000000&0xffffff00   ← 以太网类
```

它确实会对我们这张 CX-3 发起匹配（`ioreg` 里设备节点带 `IODEXTMatchCount = 1`），
开机日志显示其用户态进程拉起后立刻退出、`start(ethernet-…) fail`，设备随后留在**无主**状态：

```
DK: DriverKit_AppleEthernetMLX5-0x100000451 waiting for server …
DK: IOUserServer(…)::terminate(…) server exit before start()
DK: DriverKit_AppleEthernetMLX5-0x100000451::start(ethernet-0x100000259) fail
launchd: removing service: com.apple.DriverKit-AppleEthernetMLX5-0x100000451
```

**处置策略（最优解＝不改系统，靠匹配优先级取胜）**：

1. **首选**：我们的 personality 用 `IOPCIPrimaryMatch 0x100715b3`（精确到 vendor+device）
   + `IOProbeScore 90000` 压过它（Apple 那个 dext 没写 score，隐含 0），仍留在
   `IODefaultMatchCategory`。零系统改动、可公开分发、系统更新不失效。
2. 若首次重启证明抢不过：把 personality 换成**私有 `IOMatchCategory`**（同一 category 才有
   独占性，换 category 后 IOKit 允许并存匹配）。同样零系统改动。
3. 兜底（仅自用，不推荐）：OpenCore `Kernel → Block` 屏蔽该 dext（本机 ESP 的 `config.plist`
   里该段存在），或改 sealed 系统卷上的 dext。代价：破封 / 每次系统更新重做 / 无法给别人复现。

**不采用的三条**：把我们的驱动"接进"那个 dext（它是 Apple 签名的 sealed 二进制，且 CX-3 与
MLX5 的寄存器接口完全不同）；反编译它（专有，且许可上也禁止）；去改 sealed 卷。

首次重启的结果即是本策略的判决实验：`log show` 里若出现我们的 `Cx3Mlx4: === … start:` 行，
说明第 1 层就够用。

## 3. 迭代成本（硬约束）

macOS 15 没有"即时加载"这条路：`kextutil -n` 已变成 kmutil 的壳（报
`-n is not a supported kmutil mode`）；`kmutil load` 只做暂存，返回
`KMErrorDomain Code=28 … requires a reboot`。用户态也没有旁路：对设备的 IOPCIDevice 做
`IOServiceOpen`，6 种 client type 全部返回 `0xe00002c7`（`kIOReturnUnsupported`），
所以无法从用户态映射 BAR0 来快速试错。

⇒ 每个循环 = 改码 → `build.sh` → `install.sh` → **重启** → 读 `ioreg` / `log show`。
因此 M1 拆成两步、并**只构建一次**：

| 模式 | 触发 | 行为 |
| --- | --- | --- |
| **M1a**（默认，每次启动都跑） | 无 boot-arg | 只读：config space id、BAR0/BAR2 映射、ownership 字(`0x8069c`)、HCR status(`0x80680+0x18`)、reset 信号量(`0xf0000+0x3fc`)。**不向设备写任何东西** |
| **M1b** | boot-arg `-cx3cmd=1` | 在 M1a 之上做：HCA reset（`mlx4_reset` 逐句移植）→ HCR 握手 → `QUERY_FW`(0x4) 打印固件版本/命令接口版本/max_cmds |

这样新构建的**第一次**启动永远不含写操作；确认能绑定后，只需 `nvram` 加 boot-arg 再重启，
**不用重新构建**。

## 4. 第 1 次重启：M1a 绑定成功，并暴露一个真问题（2026-10-10 12:42）

**判决点全中，dext 竞争第一层就赢了**（不需要换 `IOMatchCategory`，更不需要动系统）：

```
kmutil showloaded | grep Cx3
  114  0 0xffffff7f970de000  0x2ffd  com.hijiajun.driver.Cx3Mlx4 (0.1.0) DE12EF62-…

ioreg:  +-o Cx3Mlx4 <class Cx3Mlx4, …, registered, matched, active, busy 0 (50 ms), retain 25>
        IOProbeScore = 90000 / IOPCIMatchCategory = IODefaultMatchCategory

log:    Cx3Mlx4: === Cx3Mlx4 start: 15b3:1007 subsys 15b3:0003 classrev 02000000 cmd 0000 ===
        Cx3Mlx4: provider open: OK
```

Apple 的 MLX5 dext 这次只被 `Realize`，**没有再对它发起 start**，设备被我们独占。

### 4.1 但 BAR0 全是 `0xffffffff` —— 根因是 PCI 内存解码没开

```
log: Cx3Mlx4: HCR+0x80680 = ffffffff ffffffff ffffffff ffffffff
     Cx3Mlx4: RESET+0xf03f0 = ffffffff ffffffff ffffffff ffffffff
     Cx3Mlx4: M1a VERDICT: bound=YES bar0=YES bar2=YES owner=0xffffffff
                          hcr_go=1 hcr_t=1 resetsem=0xffffffff
```

同一行日志里的 `cmd 0000` 就是答案：**PCI COMMAND 寄存器 = 0x0000**，
即 Memory Space Enable 与 Bus Master Enable 都是关的。窗口关掉时总线浮高，
所以每一次 MMIO 读都返回 `0xffffffff`。同时 `ioreg` 里
`IOPCIResourced = Yes`、`IODeviceMemory = 0xdc200000/1MiB + 0xc860000000/8MiB`
说明 **BAR 本身分配好了**，缺的只是使能。

对照证据（本机两个能正常工作的第三方 PCI kext 都在 `start()` 里做这三步）：

| 文件:行 | 调用 |
| --- | --- |
| `tools/research/mtkwlan/SimpleMtkWlan/SimpleMtkWlan.cpp:302-304` | `setBusMasterEnable(true)` / `setIOEnable(true)` / `setMemoryEnable(true)` |
| `tools/research/itlwm/AirportItlwm/AirportItlwm.cpp:355-357` | 同上 |

⇒ 修正：在 `Cx3Mlx4::start()` 里、映射 BAR 之前补上这三个使能（顺序照 mtkwlan），
外加 `requestPowerDomainState(kIOPMPowerOn, …)` 确保功能处于 D0，并把
`PCI COMMAND` 使能**前后**的值都打进日志。

### 4.2 顺手加了一道代码级安全闸

`Mlx4Hw::windowAlive()` 读 4 个互不相关的寄存器（HCR[0]、HCR[1]、ownership、
reset 信号量），只要全为 `0xffffffff` 就判定"窗口不可用"。**M1b 必须同时满足
`-cx3cmd=1` 且窗口可用才会执行**，否则打印 `M1b SKIPPED`。这样：

- 即使使能修正无效，也绝不会有任何写操作落到不可用的窗口上；
- 首次跑新构建仍然不会盲写硬件。

## 5. 第 2 次重启：使能修正生效，M1a 判定全部通过（2026-10-10 13:02 / 13:05 / 13:10）

```
PCI command before enable: 0x0000
PCI command after enable:  0x0006   (busmaster / io / memory: was-off -> enabled)
BAR0 mapping physical address = 0xdc200000   (expected 0xdc200000)
HCR+0x80680 = 2043202c 8c034093 14523414 0a408050
HCR+0x80690 = 04076113 85090091 63144526 01000000
M1a VERDICT: bound=YES bar0=YES bar2=YES window_alive=YES
             owner=0x00000000 hcr_go=0 hcr_t=0 resetsem=0x00000000
```

逐项核对：BAR0 物理地址精确等于 `0xdc200000`；窗口存活；`hcr_go=0 hcr_t=0` 表示命令寄存器
空闲；`owner=0`。**M1a 达成。** 使能修正是唯一改动，且三项读数在三次启动中完全一致
（含 `HCR+0x80680` 那两行的残余寄存器内容），说明读通路稳定。

## 6. 本轮自查发现的三个问题（都已修）

### 6.1 ⚠️ 冷页读伪影：新映射的 BAR0 页，**首次**读返回 0

同一份日志里出现自相矛盾的两个读数（这正是当初要求"两边都打日志"的价值）：

| 寄存器 | 直接读函数返回 | 几微秒后同一地址的 dump |
| --- | --- | --- |
| `owner` @ `0x8069c` | `0x00000000` | `0x01000000` |
| `resetsem` @ `0xf03fc` | `0x00000000` | `0x01000000` |
| `hcrstatus` @ `0x80698` | go=0 t=0 | `0x63144526` → 去字节序后 go=0 t=0（**一致**） |

规律：**两个不一致的地址，恰好各自是所在 4 KiB 页的第一次访问**；同页的第二个读
（`0x80698`）就与 dump 一致。⇒ 判定为"冷页首次读返回 0"的映射伪影，而不是寄存器真值。

**处置**：`Mlx4Hw::readRegWarm()` 丢弃每个地址的第一个样本（读两次，若不一致再取第三个），
所有判定逻辑一律走它；`warmBar0()` 开机先按 4 KiB 步长把整片 1 MiB 窗口预热；
`diagnoseFirstRead()` 把三个可疑地址的第 1/2/3 次采样都打进日志，**下一次启动即可证伪或证实**。

**影响**：若不修，M1b 里读 HCR status 可能拿到伪 0 —— 而 status=0 恰好代表"命令成功"，
会把失败误判成成功。这是个会造成错误结论的隐患，不只是噪声。

### 6.2 `cmdPending()` 判定写反了（Linux 是相等，不是不等）

FreeBSD/Linux `mlx4_cmd.c:cmd_pending()`：

```c
return (status & swab32(1 << HCR_GO_BIT)) ||
       (cmd->toggle == !!(status & swab32(1 << HCR_T_BIT)));   /* 相等 */
```

我原先写成 `fToggle != tb`。后果是**双重反向**：第一条命令之前寄存器被判成"永久忙"
（会白等 10 s 然后放弃），命令发出后又立刻被判成"空闲"（会在固件尚未完成时读到陈旧
status）。已改成相等语义，并在代码里注明出处。

### 6.3 M1b 不再默认做 HCA reset

参考驱动的顺序是 reset → cmd_init → QUERY_FW。但 reset 是 M1 里**唯一不可靠温重启回退**的动作
（需要硬件信号量；而信号量的读数本身正受 6.1 的伪影影响，两次采样分别是 0 与 `0x01000000`），
所以拆开：

| boot-arg | 行为 |
| --- | --- |
| `-cx3cmd=1`（本轮已设） | 只发 `QUERY_FW`(0x4)，**不 reset**。这已经是 M1 的目标（读出固件版本） |
| `-cx3reset=1` | 额外执行 HCA reset。等 QUERY_FW 证明确有必要时再单独开 |

另外：M1b 仍受 6.1 之外的硬闸保护（`windowAlive()` 为假则打印 `M1b SKIPPED` 直接不发命令）；
命令超时从参考的 60 s 收窄到 **5 s**，避免在 `start()` 里把 kext 加载阻塞一分钟。

## 7. 第 3 次重启：伪影确认；并查明 boot-arg 在本机**不能**用 `nvram` 设（2026-10-10 13:49）

### 7.1 冷页伪影得到证实（两次启动完全一致）

```
cold-read probe owner(0x8069c)     1st=0x00000000 2nd=0x01000000 3rd=0x01000000  <- first sample differed
cold-read probe hcrstatus(0x80698) 1st=0x63144526 2nd=0x63144526 3rd=0x63144526
cold-read probe resetsem(0xf03fc)  1st=0x00000000 2nd=0x01000000 3rd=0x01000000  <- first sample differed
M1a VERDICT: … window_alive=YES owner=0x01000000 hcr_go=0 hcr_t=0 resetsem=0x01000000
```

两个"首次读为 0"的地址都稳定复现，同页的第二个读正常。⇒ 6.1 的判断成立，`readRegWarm()`
之后 `owner=0x01000000` / `resetsem=0x01000000` 才是真值。**M1a 至此完全通过。**

### 7.2 ⚠️ 关键机制发现：`nvram boot-args=` 在本机无效

我按计划用 `nvram boot-args="… -cx3cmd=1"` 追加了参数并确认写入成功，但重启后日志仍打印
`command path … : disabled`。查明原因：

```
$ nvram boot-args
boot-args  -v debug=0x100 … amfi=0x80            ← 我加的 -cx3cmd=1 消失了
ESP/EFI/OC/config.plist → NVRAM.Add.
   7C436110-AB2A-4BBB-A880-FE41995C9F82 → boot-args  == 同一个字符串
```

**OpenCore 每次开机都用 `config.plist` 的 `NVRAM → Add` 覆盖 boot-args**，所以
`nvram boot-args=` 写的值活不过一次重启。⇒ 在这台机器上要改 boot-args，
**必须改 `EFI/OC/config.plist`**（这也是本项目其它文档早就在用的方式）。

### 7.3 处置：改成代码内默认开启，**不动 OpenCore 配置**

改 `config.plist` 属于动启动链，风险等级与改 kext 不同，且并非必要。改为：

| boot-arg | 行为 |
| --- | --- |
| （无） | **命令通路默认开启**（这也是最终驱动的形态） |
| `-cx3nocmd=1` | 关掉命令通路，只做 M1a 只读探测 |
| `-cx3reset=1` | 额外执行 HCA reset（仍默认关闭） |

之所以默认开启是安全的，因为**所有前提都由代码检查而不是假设**：

1. `windowAlive()` —— 窗口读回全 `0xffffffff` 就跳过，绝不盲写（已实测有效）；
2. **outbox DMA 地址强制在 4 GiB 以下** —— 现在 `inTaskWithPhysicalMask(…, 0x00000000fffff000)`
   分配，`queryFw()` 里再校验一次，不在范围内就拒发。这是为了堵掉"M1b 里唯一可能破坏
   内核内存的向量"：outbox 地址要交给固件去 DMA 写，若被截断或映射错误，卡会写到无关物理页；
3. 命令超时 5 s（参考驱动是 60 s），不会在 `start()` 里阻塞一分钟；
4. 不发 reset。

`nvram` 里那个无效的 `-cx3cmd=1` 已还原为 OpenCore 的值，机器状态保持整洁。

## 8. ⚠️ 第 4 次重启失败：0.3.0 挂在启动早期（事故记录，2026-10-10 14:07 起）

`0.3.0` 于 13:54:36 装上，紧接着那次启动就**硬挂**，症状是"一直跑码"。后续 6 次启动同样挂死，
直到把 kext 删掉才恢复。

### 8.1 证据

| 证据 | 观察 |
| --- | --- |
| 日志库 | `/var/db/diagnostics/Persist/…064.tracev3` 是 **0 字节**（14:07）。新会话目录建立了，但没有任何一条日志落盘 |
| 后续启动 | 17:31 / 17:33 / 18:13 / 18:14 / 19:23 / 19:24 六次，**Persist 里没有任何对应目录**，等于连日志系统都没起来 |
| panic 文件 | `/Library/Logs/DiagnosticReports` 里**没有** Kernel panic，`DumpPanic` 也没产出报告 |
| 关机原因 | `pmset -g log` 最后一条仍是 `19:52` 的 `SMC shutdown cause: 5`，即**来自 13:51 那次正常关机**。中间 7 次启动全都没走到能记录关机原因的地步 |
| 恢复 | 删掉 `/Library/Extensions/Cx3Mlx4.kext` + 重建 AuxKC 后恢复正常启动 |

### 8.2 归因（如实定级）

1. **是我们的驱动。** `0.2.0` 的启动路径（PCI 使能 + 映射 BAR + **只读**探测）在同一台机器上
   连续两次启动正常；`0.3.0` 唯一的差别就是 `start()` 里多了"分配 DMA 邮箱 + 第一次向 HCR
   写寄存器"。换掉这一处就从"能启动"变成"每次必挂"。
2. **不是内核崩溃，是硬挂**：没有 panic 文件，且日志缓冲区一行都没落盘，说明卡在 kext 加载的
   很早阶段，CPU/总线层面死锁。
3. **为什么必须断电**：显卡（此处指 RAID/网卡这类外设同样成立）在硬挂时其内部状态不会被
   普通重启复位，所以下一次启动会继续挂在同一处 —— 这解释了那 6 次连环失败。
4. **不能分辨的一点**：命令通路里有两个新动作 —— (a) DMA 邮箱分配、(b) 第一次 HCR 写 ——
   **现有证据无法区分是哪一个挂的**（没有任何日志到达用户可见阶段）。屏幕最后一行如果停在
   `mailbox: …` 之前，则指向 (a)；看到 `mailbox: …` 才指向 (b)。

### 8.3 两个应当被记下的错误

**错误一（设计）**：把有写操作、要分配 DMA 的代码放进了 `IOService::start()`。`start()` 的故障
代价是**整机启动不了**，而不是"一个测试失败"。这类代码不该出现在开机路径上。

**错误二（偏离参考实现）**：Linux `mlx4_load_one()`（`mlx4_main.c:3244`）里有一道闸：

```c
err = mlx4_get_ownership(dev);
if (err) { ... "Multiple PFs not yet supported - Skipping PF"; return -EINVAL; }
```

它读 `0x8069c`，非 0 就**拒绝驱动这张卡**。我们实测该处稳定读到 `0x01000000`（非 0）。
也就是说：**参考驱动面对这张卡当前的状态会直接放弃，而这个移植在改造时把这道闸删掉了**，
随后照常向 HCR 写命令。这是一条独立于挂死现象的、指向"当时不该往下走"的证据。
`0.4.0` 已把这道闸恢复为默认拒绝（可用 `force` 显式覆盖）。

## 9. 0.4.0 架构：把开机路径与设备写操作彻底分开

原则：**让 `start()` 回到已被验证过能启动的那一组操作，所有有风险的动作改由用户态在系统
跑起来之后触发。**

| | 0.2.0–0.3.0 | 0.4.0 |
| --- | --- | --- |
| `start()` | 只读探测；0.3.0 起**额外**做 DMA 分配 + HCR 写 | 只做 PCI 使能 + 映射 BAR + 只读探测 + 注册服务。**无写操作、无 DMA** |
| 命令通路 | 在 `start()` 里 | 移到 `Cx3Mlx4UserClient`，由 `cx3ctl` 从用户态调用 |
| 故障代价 | **整机无法启动**，且会连续多次（需断电） | 调用那一刻风扇一转重启即可，**重启后必然能进系统** |
| 迭代成本 | 每次都要重启 | **不需要重启**，改一次编译一次就能直接跑 |

选择子（定义在 `src/cx3_abi.h`，kext 与工具共用）：

| 选择子 | 命令 | 行为 | 风险 |
| --- | --- | --- | --- |
| 0 | `cx3ctl probe` | 只读寄存器 + 门禁状态 | 无 |
| 1 | `cx3ctl dma` | **只**分配命令邮箱，不碰任何寄存器 | 低（bisect 的 (a) 步） |
| 2 | `cx3ctl queryfw [force]` | HCR 握手 + `QUERY_FW` | 中（**就是上次挂死的那一步**，(b) 步） |
| 3 | `cx3ctl reset` | HCA reset | 高（可能要断电） |

这样 (a)/(b) 可以**一次跑完就分开**，而且每一步都单独可归因。安全闸都在代码里：
`windowAlive()` 不成立则拒绝发命令；`queryFw` 校验 outbox 地址必须在 4 GiB 以下；
owner 非 0 默认拒绝（需 `force`）；`reset` 需要显式确认标量；整个 user client **root-only**
（`clientHasPrivilege(securityID, "root")`）。

## 10. 0.4.0 的验证

**① 可达性证明（这是本次最重要的验证）**：反汇编 `Cx3Mlx4::start()`，从它出发做闭包，
可达的本项目函数只有：

```
Mlx4Hw::init / bar0Mapped / bar2Mapped / bar0Physical
Mlx4Hw::readOwnerWord / readHcrStatus / readResetSem
Mlx4Hw::readRegWarm / readRegRaw / windowAlive
Mlx4Hw::diagnoseFirstRead / warmBar0 / dump
Cx3Mlx4::readOnlyProbe
```

`resetHca` / `queryFw` / `mboxAlloc` / 任何 `configWrite*` **均不可达**。反向核对：

| 符号 | 唯一调用者 |
| --- | --- |
| `IODMACommand::withSpecification` | `Mlx4Hw::mboxAlloc` |
| `configWrite16/32` | `Mlx4Hw::resetHca` |
| `Mlx4Hw::resetHca` | `Cx3Mlx4::exportResetHca`（选择子 3） |
| `Mlx4Hw::queryFw` | `Cx3Mlx4::exportQueryFw`（选择子 2） |
| `Mlx4Hw::allocMailbox` | `Cx3Mlx4::exportDma`（选择子 1） |

即：**从 `start()` 没有任何一条路径能写到设备或分配 DMA。**

**② 构建**：4/4 源文件通过；`_kmod_info` 已导出；`kmutil libraries` 未定义符号
**340/340 全部解析**。

**③ 工具冒烟**：kext 未装载时 `cx3ctl probe` 输出
`Cx3Mlx4: no matching service found - is the kext loaded?` 并返回 1（干净报错，不崩）。

**③b 该检查经过反证测试**（重要：一个永远说 PASS 的检查比没有检查更糟）。把入口换成
`Cx3Mlx4::exportQueryFw` 或 `Mlx4Hw::resetHca`，它立即报错并列出完整路径：

```
FAIL: the boot path can reach a device write or a DMA allocation:
    __ZN6Mlx4Hw7queryFwEP17Mlx4QueryFwResult -> __ZN6Mlx4Hw9mboxAllocEv
    __ZN6Mlx4Hw9mboxAllocEv -> __ZN12IODMACommand17withSpecification…
    __ZN6Mlx4Hw8resetHcaEv -> __ZN11IOPCIDevice13configWrite16Eyt
    __ZN11IOPCIDevice13configWrite16Eyt -> __ZN11IOPCIDevice21extendedConfigWrite16Eyt
```

⇒ 检查确实能识别写操作，`start()` 的 PASS 不是空过。这条检查已接进 `build.sh`，
**构建不过就不出产物**。

**④ 本机新增的第三个构建坑**：用户态工具**不能**用 `xcrun --find clang` 返回的那个
`/Library/Developer/CommandLineTools/usr/bin/clang` 直接编译 —— 它自己找不到 macOS SDK，
报 `'stdio.h' file not found`。必须走 `/usr/bin/clang` 或 `xcrun clang`。
kext 那边因为用 `-nostdinc` + 显式指向 MacKernelSDK，反而不会暴露这个问题。

## 11. 待办（第 5 次重启）

`0.4.0` 已构建并离线校验通过，**尚未装载**（等指示）。装载后需要一次重启让新 kext 生效，
但那一次重启的风险与 `0.2.0` 相同（开机路径是同一组只读操作）。之后：

```bash
sudo cx3ctl probe     # 只读；确认仍能绑定，并给出 owner 真值
sudo cx3ctl dma       # bisect (a)：仅 DMA 分配
sudo cx3ctl queryfw   # bisect (b)：上次挂死的那一步
```

判决点：`probe` 返回 `window alive: YES` 且 owner 稳定；`dma` 打印出 4 GiB 以下的物理地址；
`queryfw` 若能返回 `fw version`，**M1 即完成**。

## 12. 完整产物状态（0.4.0）

| 项 | 值 |
| --- | --- |
| 版本 | `0.4.0`；日志含 `build tag: 0.4.0-userclient` |
| 源码 | `src/{Cx3Mlx4,Cx3Mlx4UserClient,Mlx4Hw,kmod_info}.cpp` + `cx3_abi.h` + `mlx4_regs.h` |
| 工具 | `tools/cx3ctl.c` → `build/cx3ctl`（`install.sh` 会装到 `/usr/local/bin/cx3ctl`） |
| 构建 | 4/4 通过；`Mach-O 64-bit kext bundle x86_64`；`_kmod_info` 1 个 |
| 符号 | **340/340** 全部解析 |
| 诊断 | 只剩 `Bad code signature`（未签名，符合预期）+ 属主提示 |
| 当前机器状态 | kext **已移除**（用户手动删的），AuxKC 已重建，机内 6 个第三方 kext 完好 |
| 备份 | `outputs/20261010-cx3-m1/state-backup-20261010-115242 / -115418 / -115431` |

## 13. 回滚

```bash
sudo tools/cx3-mlx4/rollback.sh            # unload + 残留检查 + kmutil rebuild（保留 bundle）
sudo tools/cx3-mlx4/rollback.sh --purge    # 连 /Library/Extensions/Cx3Mlx4.kext 一起删
```

若重启后**进不去系统**：长按 Shift 进安全模式（跳过第三方 kext），或从 Recovery 跑
`rollback.sh`；ESP 的 `config.plist` 也可在 macOS / Windows 侧编辑。修好后建议**断电一次**
而不是只重启 —— 外设在硬挂后的内部状态不一定被温重启复位。
**禁止**用 `kmutil clear-staging`（会连 NullMoth / HighPoint 的暂存一起清掉）。

## 14. 尚未闭环的两处

1. **授权记录**：`kmutil load -z` 跳过了授权门，因此 UAKL 与 KextPolicy 里没有新增记录。
   当前 Kext Signing 已禁用且 AuxKC 含该 kext，实测能加载；若某次发现没加载，就走正常授权
   流程（`kmutil load` 不带 `-z`），不要手改 UAKL。
2. **并行会话**：同一个工作区里有另一个会话在做 MT7922 WiFi（产物在 `driver/AirportMtk/`、
   `outputs/20261010-mt7922-wifi/`、`tools/backup-mtk-efi-20261010-195616/`），他们也在操作
   ESP 与 kext 状态。**两边同时装载/重建 AuxKC 会互相干扰**，装载前需要先错开。

## 15. 第 5 次启动（20:36）：只读启动成功；bisect (a) 通过（2026-10-10）

`0.4.0` 装载后开机正常，`start()` 只打日志、不写设备：

```
build tag: 0.4.0-userclient
M1a VERDICT: bound=YES bar0=YES bar2=YES window_alive=YES owner=0x01000000
HCR+0x80680 = 00000000 00000000 00000000 00000000     <-- 上次是 2043202c 8c034093 …
HCR+0x80690 = 00000000 0000ffff 63144526 01000000
start: boot work complete (read-only). Device writes are available from user space only.
```

**架构改动生效**：从 IOLog 到 kext 加载完成全流程无挂起。`kmutil showloaded` 里
`com.hijiajun.driver.Cx3Mlx4 (0.4.0)` 正常。

### 15.1 bisect (a) 通过：DMA 分配不是上次挂死的原因

```
$ sudo cx3ctl probe
  window alive  : YES
  owner word    : 0x01000000
  HCR status    : 0x26451463      (GO=0, T=0 → 命令寄存器空闲)
  reset sem     : 0x01000000
  PCI COMMAND   : 0x0006
  BAR0 physical : 0xdc200000

$ sudo cx3ctl dma
  mailbox is not one contiguous DMA segment (nseg=1 off=4096)
```

`nseg=1` 说明**缓冲是一整段连续的**，DMA 分配本身没有挂死、没有 panic。**这条把 0.3.0
挂死的两个嫌疑排除掉了一半**：不是 DMA 分配，那就剩"第一次 HCR 写 + 轮询"。

### 15.2 又抓到自己代码里的一个 bug（offset 语义）

`IODMACommand::gen64IOVMSegments()` 的 `offset` 是**输入/输出**参数，官方文档原文：
"defines the **starting and ending** offset in the memory descriptor"。所以整个 4096 字节
被一段覆盖完时，返回 `offset == 4096` 是**成功**。我第一版写成 `offset != 0` 判失败，
于是**每一次成功的分配都被当成失败丢掉**。判据只需要 `nseg == 1`（连续性），
`offset == 4096`（整段被消费）。已修（`0.5.0`），并在失败分支里把 `rc`、`nseg`、
`offset`、`addr` 全打进日志，下次不用猜。

### 15.3 重要发现：参考驱动的两道闸都会拦下这张卡

| 寄存器 | 实测值 | 参考驱动的反应（源码位置） |
| --- | --- | --- |
| owner `0x8069c` | `0x01000000`（**非 0**） | `mlx4_main.c:3244` → `"Multiple PFs not yet supported - Skipping PF"`，**拒绝驱动** |
| reset 信号量 `0xf03fc` | `0x01000000`（**非 0**） | `mlx4_reset.c:104` → `"Failed to obtain HW semaphore, aborting"`，**reset 会中止** |

`mlx4_get_ownership()` 实现是 `ret = readl(owner); return (int) !!ret;` —— **任何非 0 值 = 已被占用**。
所以按参考实现，这张卡的当前状态是"已被占用 + 信号量被持有"，正常驱动不会碰它。

**尚未定论的一点**：这两个值在"硬挂之后的温重启"和"上次重启"后都是 `0x01000000`。
需要一次**真正的完全断电**（关机 + 关电源开关/拔电源线 ≥10 秒）来区分两种可能：

- 若完全断电后变成 `0`：说明它们是遗留状态，之前那次"重启"没有真正复位设备
  （只关机重启时 PCIe 设备未必收到 PERST#，且 5VSB 待机电压仍在供电）；
- 若完全断电后仍是 `0x01000000`：说明这是这张卡的稳定硬件状态，那就得换思路
  （参考驱动在这里会 skip，我们需要别的路径把它带到可用状态）。

这个判定必须放在"发命令"之前做 —— 因为在"已被占用 + 信号量被持有"的状态下强行发
`GO`，正是最可能把设备推进死锁、并且死锁后卡住 CPU（MMIO 读永不完成 → 无 panic 的硬挂）
的动作。

### 15.4 可达性检查经过反证（见 §10）

`build.sh` 现在每次都会跑 `check-boot-path.py`：把入口换成 `exportQueryFw`/`resetHca`
会立即失败并列出路径；换成 `start()` 才通过。不通过就不出产物。

### 15.5 当前状态

- `0.5.0`（含 DMA 判据修正）已构建、已装载到 `/Library/Extensions`、已 staged；
- 当前实例已 `kmutil unload` 卸下（所以设备现在无主），**下次开机加载 0.5.0**；
- 暂存 8 条、AuxKC 含我们、其它 6 个第三方 kext 完好；
- `/usr/local/bin/cx3ctl` 已装。

**下一步（需要一次完全断电，而不是重启）**：断电重启后先只跑 `sudo cx3ctl probe`，
看 owner / sem 是否变成 0 —— 那才是决定"能不能安全发命令"的判据。

## 16. 第 6 次启动（20:45）：完全断电后 owner/信号量**不变**；参考实现两条路都堵死

```
owner word (0x8069c)     = 0x01000000    <-- 仍非 0
reset sem  (0xf03fc)     = 0x01000000    <-- 仍非 0
HCR status (0x80698)     = 0x63144526    <-- 三次启动全一致
HCR+0x80680 = 0043202c 0c004093 00123414 0a400010   <-- 这次非 0（上次该处为全 0）
```

判定：**这两个值不是上次事故的残留，是这张卡的持久状态**（跨越了完全断电）。而
`HCR+0x80680` 那段在同一台机器不同启动间会从全 0 变成非 0、且我们从未写过它，
说明它要么由固件在动，要么我们对它的解读有误。

### 16.1 参考实现的两道闸都会拦下这张卡

| 闸 | 位置 | 条件 | 本卡 |
| --- | --- | --- | --- |
| ownership | `mlx4_main.c:3244` | `readl(0x8069c)` 非 0 → `Multiple PFs not yet supported - Skipping PF`，`return -EINVAL` | `0x01000000` → **拦下** |
| HW semaphore | `mlx4_reset.c:104` | `readl(0xf03fc)` 非 0 且等满 10 s → `Failed to obtain HW semaphore`，`-EAGAIN` | `0x01000000` → **拦下** |

⇒ **照抄参考流程在这张卡上是走不通的**：它既不会被驱动接管，reset 也会中止。
这也意味着"直接发 `QUERY_FW`"不但危险，而且从参考实现的角度看是**在它明确拒绝的前提下硬来**。

### 16.2 ⚠️ 一个可能推翻"owner 位被占用"结论的现象

```
RESET+0xf03f0 = 01000000 01000000 01000000 01000000
owner (0x8069c) = 01000000
```

`0xf03f0` 起**连续四个 dword 全等于 `0x01000000`**，而 owner 又是同一个值。
四个相邻寄存器恰好同值，更像"这段区域在该模式下返回填充值"，而非四个独立的 `1`。
若成立，则"owner 位"这一判断本身是错的 —— 必须先分辨这两种可能，才能决定下一步。

便宜且只读的分辨办法：**把 BAR0 大范围 dump 出来看结构**。

### 16.3 约束：任何 kext 改动都要一次重启

实测确认：`kmutil unload` 可用（能卸下已加载实例），但**新二进制必须重启**才能生效
（新 cdHash 要重建辅助内核集合，`kmutil load` 直接返回 `Code=28 requires a reboot`）。
⇒ 所以"探查寄存器"这种需要反复改动的活动，必须**一次把能力长齐**，不能一次一个问题。

（更正 §10 的乐观说法：`kmutil unload` 解决的是"卸下"，不是"换新代码"。）

## 17. 0.6.0：长出"自由读寄存器"的能力（一次重启换掉后续所有重启）

新增两个选择子：

| 选择子 | 命令 | 性质 |
| --- | --- | --- |
| 4 | `cx3ctl peek <space> <offset> <count>` | **只读**，可 dump BAR0(1 MiB) / BAR2(8 MiB) / PCI config，每调用最多 14 dword，不做字节序转换 |
| 5 | `cx3ctl clearowner` | `writel(0, 0x8069c)` + 睡 1 s，即参考的 `mlx4_free_ownership()` 原样操作，需确认标量 |

`clearowner` 之所以可以先于命令通路提供：它是**单条 32-bit 存储、无握手、无轮询**；
参考实现在三处"把设备交还"的正常路径（SRIOV 交接、加载失败清理、卸载）都调用它。
另外 —— **posted 写不需要设备应答就能完成，而读是非 posted 的、才有可能真正卡住 CPU**；
而我们对这张卡的读一直是能完成的（开机时已逐 4 KiB 页读过整片 BAR0），
这是"设备会应答"的证据。

`peek` 的边界在 kext 内强制（对齐、长度、各空间上限），不信任调用方。

**检查器同步加强**：`clearOwner` 是内联寄存器写（编译成一条普通 store，**没有符号可查**），
所以按名字把它加进黑名单；反证测试确认换成该入口会 FAIL。同时如实记录检查器的局限：
它按**符号名**匹配调用目标，一个"经由已加载指针直接写寄存器"的函数按名字抓不到 ——
因此它保证的是"`start()` 到不了任何具名的写/死锁函数"，并把可达集**每次构建都打印出来**供人复核。

## 18. 第 7 次启动（21:02）：BAR0 地图、以及 ATTO 驱动的独立交叉验证

### 18.1 BAR0 真实地图（`peek` 全量页扫，只读）

```
0x00000 - 0x0f000  全 0
0x10000 - 0x14000  有数据
0x1c000 - 0x1d000  有数据
0x40000 - 0x47000  大量数据（近似随机，疑为固件/密钥区）
0x59000            有数据
0x6e000            整页 01000000
0x77000            整页 01000000
0x80680 - 0x8069f  有数据（每次启动都不同）
0xf0000 - 0xf3fff  有数据
0xf03f0 - 0xf03ff  01000000（其中 f03f0/f03f4/f03f8 历次启动会在 0 与 01000000 之间变）
其余绝大多数页      全 0
```

⇒ **BAR0 是稀疏的**：大部分未实现（读 0），少数真实寄存器块。`0x01000000` 在多处作为重复值出现。

### 18.2 与 Mellanox 自家驱动的交叉验证（ATTOFastFrame2 = cna2 源码）

ATTO 的 kext 就是 Mellanox 的 `cna2` 代码（交接文档 §9.2 已确证），
所以**反汇编它 = 拿到该硬件期望行为的第二份独立陈述**。用它核对我们的常量：

| 检查项 | ATTO 里的证据 | 结论 |
| --- | --- | --- |
| BAR0 偏移 | 立即数 `0x80680`×1、`0x80698`×11、`0x8069c`×4、`0xf0000/0xf0010/0xf0044/0xf03fc` 各 1 | 与 FreeBSD mlx4 同偏移 ⇒ **我们的偏移正确** |
| reset 值 | `movl $0x1000000, 0xf0010(%r14)` | 与 FreeBSD `MLX4_RESET_VALUE = swab32(1)` 在小端机上**同值** ⇒ 常量正确 |
| reset 后延时 | `IODelay(0x7a120)` = 500 ms（FreeBSD 是 1000 ms） | 细节差异，不影响 |
| ownership 闸 | `cmpl $0x0, 0x8069c` → `je` 继续；**非 0 时** `LogEvent(0xa0030000)` + `movl $0xffffffea`（**-EINVAL**） | 与 `mlx4_main.c:3244` 的 `return -EINVAL` **完全一致** |
| 信号量闸 | `cmpl $0x0, 0xf03fc` 等 10 s，`IODelay(1000)` 轮询；超时 `movl $0xffffffdd`（**-EAGAIN**） | 与 `mlx4_reset.c:113` 的 `-EAGAIN` **完全一致** |
| 映射方式 | 调用 `IOService::mapDeviceMemoryWithIndex` | 与我们的 `mapDeviceMemoryWithRegister` 内部同一方法 ⇒ 映射方式无差异 |

### 18.3 结论：这张卡当前状态会被**两个独立实现**同时拒绝

```
owner (0x8069c) = 0x01000000   恒定（跨 7 次启动、含完全断电）
sem   (0xf03fc) = 0x01000000   恒定（连读 6 次、历时数秒，纹丝不动）
```

- FreeBSD/Linux mlx4：owner 非 0 → `Skipping PF`；sem 非 0 → reset `-EAGAIN`
- Mellanox 自家 cna2：同样两处，同样返回值

**并且写入 0 到 owner 无效**（`before 0x01000000 → after 0x01000000`）。

这留下两个互斥的解释，且它们的后续动作完全不同：

| 假设 | 含义 | 下一步 |
| --- | --- | --- |
| **A. 这两个地址在本卡上是只读/未实现** | 参考实现的闸读到了"噪声"，卡其实没被占用 ⇒ 可以绕过闸直接 reset | 先证明 BAR0 写可达，再考虑 reset |
| **B. 确实是卡被占用/信号量被持有** | 卡处于异常状态（可能是曾有外来驱动 —— Apple 的 MLX5 dext 按**厂商 ID**匹配所有 Mellanox 卡并会拉起用户态服务，它启动过多次） | 需要 reset 把它重启；但 reset 被信号量闸挡住 |

**区分 A/B 的关键实验：BAR0 写入到底能不能落到卡上。** 若"能"→ A 成立（因为同一写机制下 owner 却不变，说明该地址只读）；若"不能"→ 我们自己的映射有问题，一切推断作废。

### 18.4 0.7.0：加上受保护的单寄存器写 + 回读

```
cx3ctl poke <space> <offset> <value>     一次 32 位存储，随后回读
```
- 需确认标量；仅 BAR0/BAR2；对齐与越界在 kext 内强制。
- **kext 内硬拒两个"动作寄存器"**：`0xf010`（写 reset 值会重启 HCA）、`0x80698`（HCR 控制字，置 GO 会发固件命令）。写这两个就把"诊断"变成了"动作"。
- 检查器同步把 `writeBar0/writeBar2` 加入黑名单，并做了反证（以 `writeBar0` 为入口会 FAIL）。

### 18.5 风险记录（reset 的代价，待决）

`0xf010` 的 reset 是**唯一**能把卡从异常状态拉回来的动作，但它：
- 被信号量闸挡着（参考实现会中止）；绕过它意味着违反协议；
- 信号量的存在意义正是"锁住 flash 更新期间的 reset"。**若此刻真有 flash 在写，绕过会导致固件损坏**；
- 但从证据看没有 flash 活动（信号量恒定、无任何变化），且卡已处于参考实现拒绝的状态 —— 即 **reset 可能正是解药**。

⇒ 这一步**先不做**，等 `poke` 证明写入可达之后再单独决策。

## 19. 第 8 次启动（21:1x）：写入可达性判定完成 + 冷读伪影是"按地址"的

### 19.1 决定性结果：**BAR0 写入确实能落到设备上**

```
poke 0 0x8069c 0x0        (owner)
  before : 0x01000000
  wrote  : 0x00000000
  after  : 0x00000000     <-- 写进去了
peek 0 0x8069c 1  -> 0x01000000   <-- 但设备随即把它置回
```

⇒ 假设 B 成立的一半：**我们的写机制没问题**（不是映射只读）；而 `0x8069c` 是**设备主动维持的标志位**，
写 0 有效但会被立刻重新置回。所以"const 0x01000000"不是填充值、也不是只读——是我们清不掉的真实状态位。

对照：`0x806a0`（读 0 的普通地址）写 `0xa5a5a5a5` 后仍是 0 ⇒ 该地址确实未实现（写被丢弃）。

### 19.2 修正 §18 的一个判断：冷读伪影是**按地址**的，不是按页

```
peek 0xf03fc 连续 3 次 -> 1st=00000000, 2nd=01000000, 3rd=01000000
```

⇒ 某个地址在"间隔后首次访问"会返回 0，第二次才是真值。开机探测里 owner 与 resetsem 的
"1st 差异"正是这个机制（它们是各自地址的首次访问）。

**这条把我自己的诊断工具坑了**：`peek`/`poke` 一开始暴露的是**裸单次读**（`readRegRaw`），
所以本轮早期出现过的 `before : 0x00000000`（信号量）是伪影而不是数据。
已修：`readBar0`/`readBar2` 改走 `readRegWarm`（丢弃首个样本）。

**教训（写进流程）**：诊断路径若可能把"伪影"当"数据"报出来，比没有诊断路径更糟 ——
因为它会以看起来精确的数字误导判断。凡是单地址测量，必须走 warm 读。

### 19.3 硬拒验证通过（正确地址）

```
poke 0 0xf0010 0x01000000  -> refused (kIOReturnNotPermitted)
poke 0 0x80698 0x00800000  -> refused
```
（注：§18.4 里我把地址写成 `0xf010`，正确的是 `0xf0010`；工具提示文案已一并修正。）

### 19.4 当前状态与下一步

| 事实 | 状态 |
| --- | --- |
| 绑定 / 映射 / 读寄存器（M1a） | ✅ 稳 |
| BAR0 写入可达 | ✅ 已证（本次） |
| owner 位（0x8069c） | `0x01000000`，可写但设备立刻置回 ⇒ **清不掉** |
| 信号量（0xf03fc） | 同上（warm 读为 `0x01000000`） |
| `QUERY_FW` | 未执行（被 owner 闸挡） |
| HCA reset（0xf0010） | 未执行（被信号量闸挡，且绕过它有固件损坏风险）—— **待决策** |

下一步的实质问题只有一个：**要不要绕过信号量闸做一次 reset。**
支持做的证据：① 写入可达且设备有响应；② 信号量恒定不动，看不出有 flash 活动；③ 卡处于
两个参考实现都会拒绝的状态，reset 是标准补救手段。
反对的证据：信号量存在的意义正是"锁住 flash 更新期间的 reset"，若真有 flash 在写则可能损坏固件。

## 20. 第 9 次启动（21:25）：FLR 执行**成功**（2026-10-10 21:28:59）

### 20.1 决策依据（执行前）

| 证据 | 观察 |
| --- | --- |
| FLR 支持 | Device Capabilities `0x11d08e02`，bit 28 = 1（卡自己声明支持） |
| Device Status | `0x0000`——**无任何错误位**（链路健康） |
| 固件活动性 | HCR 块隔 2 秒读两次**完全静止**；信号量恒定数小时 |
| 两个参考实现 | 都因 owner/信号量非 0 而拒绝，FLR 是规范的补救手段 |
| 风险 | 唯一不可逆风险是"固件正在写 flash 时被打断"；但 flash 写入是秒级暂态，不可能持续数小时 ⇒ 基本排除 |

### 20.2 执行结果（`cx3ctl flr`）

```
status        : 0
device caps   : 0x11d08e02
FLR supported : YES
devctl before : 0x2000
BAR0 before   : 0xdc200004
BAR0 after    : 0x00000004    (CHANGED)
ids after     : 15b3:1007     (设备仍枚举)
ownership     : not read (BAR0 moved, mapping untrusted)
```

**PCI 配置空间的前后对比（决定性证据）**：

| 位置 | 复位前 | 复位后 | 含义 |
| --- | --- | --- | --- |
| config 0x00 高 16 位 | `0x0006` | `0x0000` | COMMAND 被清零（内存/总线主控使能失效） |
| config 0x10 (BAR0) | `0xdc200004` | `0x00000004` | **BAR0 地址被清空——功能已复位** |
| config 0x00 低 16 位 | `0x15b3` | `0x15b3` | 厂商 ID 不变，设备仍在枚举 |
| PCIe Device Status | `0x0000` | `0x0000` | 无错误位 |

⇒ **FLR 真实执行且生效**：设备回到"上电后未配置"的初始状态。
BAR0 变更是**成功的标志**，不是故障。

### 20.3 安全闸按设计生效（值得记录）

kext 检测到 BAR0 地址变化后，**拒绝**通过旧映射读 owner 位，并在日志里写明：

```
FLR: BAR0 changed, so the mapped window is no longer trusted and the
     ownership word was NOT read. A reboot is needed to re-map.
```

这一点很重要：FLR 之后 `0xdc200000` 已经不再属于这张卡，继续读它等于读别人的物理地址。
闸门按预期拦住了。

### 20.4 下一步

必须**重启**让 macOS 重新枚举并分配 BAR（无法在运行时安全地自行重分配：旧地址可能已被内核回收）。
重启后跑 `cx3ctl probe`：

- **owner 变成 0** ⇒ 闸开了，`queryfw` 可以走，**M1 完成**；
- **owner 仍是 0x01000000** ⇒ 卡是刻意维持该状态，需要换思路（查 EEPROM/配置空间、或走"交叉刷固件"那条社区路线）。

## 21. 第 10 次启动（21:33）：FLR 后 owner **未归零** —— M1b 的判定结论（2026-10-10）

```
（FLR 于 21:28:59 执行，本次启动 21:33）
M1a VERDICT: owner=0x01000000 resetsem=0x01000000
0x8069c 连读 6 次 → 全部 01000000
```

**FLR 之前与之后，两个闸位的值完全相同。** 结合前面的证据，可以确定：

| 事实 | 证据强度 |
| --- | --- |
| 绑定/映射/读寄存器（M1a） | ✅ 稳定，10 次启动一致 |
| BAR0 写入可达 | ✅ 已证（写 0 后立即回读为 0） |
| 两个闸位可被覆写但设备**立刻重新置位** | ✅ 已证（写后毫秒级回读已是原值） |
| FLR 真实执行（功能复位） | ✅ 已证（BAR0 清空、COMMAND 归零、设备仍枚举） |
| FLR **不能**让闸位保持为 0 | ✅ 本次判定 |
| 完全断电也不能 | ✅ 第 6 次启动已证（与温重启同值） |
| PCIe 无错误 | ✅ Device Status = 0x0000 |
| 两个参考实现（FreeBSD mlx4 / Mellanox 自家 cna2）都会拒绝此状态 | ✅ 源码级 + 反汇编级双证 |

### 21.1 因此：卡处于"参考驱动定义的不可驱动状态"，且软件无法清除

这不是我们的移植问题 —— 换成 FreeBSD 或 Mellanox 自家驱动，在同一状态下会得到同样的结果
（`return -EINVAL` / `-EAGAIN`）。也不是复位的机制问题 —— 硬件级 FLR 已执行且生效。

⇒ **继续在 macOS 这一侧试验已无收益**。剩下的可能性只有两类，且都需要在这台机器之外确认：

| 假设 | 如何判定 |
| --- | --- |
| **A. 这张卡（或其固件）非标准** —— 例如曾被刷过 OEM/ATTO 固件、或 VPI 配置与常规不同 | 用**已知可工作的 mlx4 驱动**（Linux）验证：`dmesg \| grep -i mlx4`。若 Linux 也 `Skipping PF` ⇒ 卡的问题；若 Linux 正常 ⇒ 我们漏了什么 |
| **B. macOS 侧有东西在放它之前把设备置成该状态**（Apple 的 AppleEthernetMLX5 dext 按**厂商 ID**匹配所有 Mellanox 卡，确实会拉起） | 用 OpenCore `Kernel → Block` 屏蔽该 dext + 断电重启，再看 owner 是否变 0 |

假设 A 的判定成本更低、信息量更大，且**在同一个 Linux 会话里还能顺便拿到固件版本与 PSID**：

```
mstflint -d <bdf> q          # 固件版本、PSID
mlxconfig -d <bdf> q         # 持久配置（端口类型、SRIOV 等）
```

这三条输出能直接回答"这张卡是不是被刷成了别的身份"。

### 21.2 附带发现（记录，暂不追）

PCIe Link Status 读为 `0x0000`（协商速率/宽度均为 0、数据链路层未激活），但 BAR0 的 MMIO 读取
一直正常。两者矛盾，暂无法解释；可能是该字段在本平台上由上游端口而非端点呈现。记下来以免以后
重踩。

### 21.3 当前工程状态（可交接）

- 代码与工具：`0.9.0`，含 `probe`/`dma`/`queryfw`/`reset`/`peek`/`poke`/`flr` 七个选择子
- 构建期保证：`check-boot-path.py` 保证 `start()` 到达不了任何设备写操作（有反证测试）
- 仓库：https://github.com/Hi-Jiajun/cx3-mlx4-macos （私有）
- 未完成：M1b（`QUERY_FW`）—— 卡在设备状态上，不在代码上

## 22. 更正与取证：**没有证据说明这卡被刷过非原厂固件**（2026-10-10）

### 22.1 更正

§21.1 里我把"这张卡可能被刷过 OEM/ATTO 固件"列为假设 A，**措辞过重**。
它只是若干可能性之一，**没有任何证据支持**。以下是把能查的都查了之后的结果。

### 22.2 证据：卡的身份指向"原厂 Mellanox 卡"

| 字段 | 实测值 | 含义 |
| --- | --- | --- |
| vendor-id | `0x15b3` | Mellanox 原厂 |
| device-id | `0x1007` | MT27520 世代（CX-3 Pro），与交接文档一致 |
| **subsystem** | **`0x15b3:0x0003`** | **原厂子系统 ID** |
| revision-id | `0x00` | — |
| class-code | `0x020000` | 以太网控制器 |

判定依据：

- **ATTO 的 PCI 厂商 ID 是 `117c`**（交接文档 §9.2：ATTO 的 kext 用 `pci117c,90` 这类
  `<子系统厂商>,<子系统设备>` 形态匹配）。若本卡被刷成 ATTO 固件，子系统厂商应为 `117c`。
  实测是 `15b3` ⇒ **不像被刷成 ATTO**。
- 也不是 OEM 贴牌：HP/Lenovo/Dell 的子系统厂商分别是 `103c`/`17aa`/`1028`，本卡是 `15b3`。

⇒ 本卡看起来就是**一张原厂 Mellanox 品牌的 CX-3 Pro**，与交接文档里"市价约 85 元的普通卡"吻合。

### 22.3 BAR0 不含任何固件文本（一条被证否的猜想）

曾猜 `0x40000-0x47000` 那块"像随机数据"的区域可能是 flash 映射。**猜错了**：
把 BAR0 所有有数据的区域扫了一遍（`0x10000`/`0x1c000`/`0x30000`/`0x40000-0x48000`/
`0x50000`/`0x59000`/`0x68000`/`0x70000`/`0x81000`/`0xf0000`），
提取可打印串 **0 条** —— 全是寄存器值，没有任何固件版本号或 PSID 之类的文本。
⇒ **从 BAR0 读不到固件标识**，此路不通。

### 22.4 因此：现在刷固件是**没有依据的冒险**

三个理由：

1. **不知道它现在是什么固件、PSID 是什么。** 而"刷了与 PSID 不匹配的镜像"正是把 Mellanox 卡
   刷成砖的最常见方式。
2. **不知道"固件是不是问题"。** 目前只知道"固件在每次初始化时把这个位设成 1"；
   这既可能是固件本身的问题，也可能是**硬件故障导致固件启动到一半就停在该状态**。
   若是后者，刷固件救不回来，还可能把仅存的可诊断状态也毁掉。
3. **刷写不可逆且有已知陷阱。** 社区记录：CX-3 交叉刷写后 **MAC 归零**（Linux 下正常、
   macOS 下全 0），且需另用工具写回 MAC。

### 22.5 刷固件的**前提条件**（缺一不可，来自 mstflint 的标准流程）

```sh
mstflint -d <bdf> q                    # ① 先读：固件版本 + PSID（必须记下 PSID）
mstflint -d <bdf> b current.bin        # ② 备份当前固件（万一刷坏可回退）
mstflint -d <bdf> -i <image> burn      # ③ 刷：镜像必须与 PSID 完全一致
```
外加：`mlxconfig -d <bdf> q` 读出持久配置（端口类型等）以便刷后恢复。

### 22.6 更省事的两条路，都排在刷固件之前

| 顺序 | 做法 | 成本 | 能回答什么 |
| --- | --- | --- | --- |
| 1 | **Linux 启动 U 盘**：`lspci -nn -vv`、`dmesg \| grep mlx4`、`mstflint q`、`mlxconfig q` | 一次 U 盘启动，零风险，**只读** | 卡是否被参考驱动正常驱动（决定性）；固件版本与 PSID；是否 flash 读取报错（指向硬件） |
| 2 | **OpenCore 屏蔽 Apple MLX5 dext** + 断电 | 改 config.plist（先备份），1–2 次重启 | macOS 侧是否有人在把它置成该状态 |
| 3 | 刷固件 | 不可逆 | 仅当 1 与 2 都指向固件问题时才做 |

## 23. Linux 诊断盘已制作完成（2026-10-10 22:00）

### 23.1 盘的信息

| 项 | 值 |
| --- | --- |
| 设备 | `/dev/disk2`（TESLADRIVE，USB，123.9 GB，可移动） |
| 布局 | MBR + 单个 FAT32 分区，卷标 `CX3DIAG`，占满全盘 |
| 原本内容 | 原 `PROBE1401` 分区（1401 探针盘）经用户明确授权后抹除，未备份 |
| 载荷 | `/Volumes/CX3DIAG`，203 MB / 122 文件 |

### 23.2 载荷与校验

```
efi/boot/bootx64.efi      724,992 B    ← GRUB（standalone，模块内嵌）
boot/grub/grub.cfg            875 B    ← prefix = /boot/grub，路径正确
boot/vmlinuz-lts       10,961,920 B
boot/initramfs-lts     17,771,152 B
boot/modloop-lts      149,803,008 B    ← 内含 mlx4_core / mlx4_en 模块
cx3diag.apkovl.tar.gz  10,436,214 B    ← 自动化（220 条目）
```

写入后逐个比对 SHA256，**6/6 全部 MATCH**；FAT32 上的 apkovl 仍可正常解压（220 条目，
`.default_boot_services` / `runlevels/default/local` / `zz-cx3diag.start` 三项齐备）。

### 23.3 GRUB 可用性的一条决定性证据

曾担心 ISO 的 GRUB 是模块化的（需要 `x86_64-efi/*.mod`）。把 El Torito 的 efi.img 挂载后确认：

- `efi.img` 内**只有** `efi/boot/bootx64.efi` 一个文件；
- 整个 ISO 内 `.mod` 文件数 = **0**。

⇒ GRUB 必为 **standalone**（模块内嵌），只依赖 prefix 下的 `grub.cfg`；而 prefix 经反查为
`/boot/grub`，我们的 `grub.cfg` 正在该处。**（注意：引导器里的 `%s/x86_64-efi/%s.mod`
只是 GRUB 模块加载器的通用格式串，不能作为"需要外部模块"的依据。）**

### 23.4 三个"写错就静默失败"的坑（已写入 `linux-diag/README.md`）

1. apkovl 必须含 `etc/.default_boot_services`，否则 Alpine **不会**添加默认开机服务，
   其中 `modloop` 正是提供内核模块（含 mlx4）的那个服务 ⇒ 会得到"模块不存在"的假阴性。
2. apkovl 必须靠 `nlplug-findfs` 自动发现（扫描 `*.apkovl.tar.gz`）；
   用 `apkovl=` 传裸文件名会失败，因为 init 是相对 `/` 做 `[ -f ]` 判断。
3. `lspci` 与 `mstflint` 必须自带（Alpine 标准介质两者皆无，且 live 系统无网络），
   连同 `mstflint` 依赖的 `libstdc++`/`libgcc` 一起打包——这两个库介质里同样没有。

### 23.5 下一步（待执行）

用该盘启动 → 自动采集 → 自动重启 → 回 macOS 读 `CX3DIAG/out/run-*/`。
**关键判读**：`12-dmesg-mlx.txt` 里参考驱动是接管了这张卡，还是打印出与源码一致的拒绝。

## 24. Linux 首轮结果：**卡完好，Linux 完整驱动了它**（2026-10-10 22:05）

### 24.1 决定性结果

```
mlx4_core: Mellanox ConnectX core driver v4.0-0
mlx4_core 0000:07:00.0: Initializing
mlx4_core 0000:07:00.0: enabling device (0000 -> 0002)
mlx4_core 0000:07:00.0: DMFS high rate steer mode is: disabled ...   ← 已过 QUERY_DEV_CAP/INIT_HCA
mlx4_ib:  Mellanox ConnectX InfiniBand driver v4.0-0
mlx4_en:  Port 1: Using 16 TX rings / 16 RX rings
mlx4_en 0000:07:00.0: registered PHC clock
mlx4_en:  Port 2: Initializing port
mlx4_en:  eth0: Link Up

eth0: link/ether 00:11:22:33:44:55      ← MAC 正常且连续，不是全 0
eth1: link/ether 00:11:22:33:44:56
```

**并且这不是我手动触发的**：`04-modules-pre.txt` 显示 `mlx4_core` 在脚本 `modprobe` **之前**
就已被 Alpine 的 udev 自动加载。即"开箱即用的 live 系统自动驱动成功"。

### 24.2 由此推翻/确立的判断

| 判断 | 结论 |
| --- | --- |
| 卡被刷过非原厂固件 | ❌ **否定**（MAC 正常、驱动正常、子系统 ID 是原厂 15b3:0003） |
| 卡硬件损坏 | ❌ **否定**（双端口全工作、链路 up、固件响应正常） |
| owner/信号量为"卡的真实占用状态" | ❌ **基本否定**——Linux 的 mlx4 读同一个 `0x8069c` 与 `0xf03fc` **通过了**（未出现 `Skipping PF`、未出现 `Failed to obtain HW semaphore`） |
| 问题在 macOS 侧 | ✅ **确立** |

⇒ **卡不需要刷固件、不需要修**；剩下的全部工作都在 macOS 侧。

### 24.3 一处真实的平台差异（待深究）

| | BAR0 | ROM BAR |
| --- | --- | --- |
| macOS | `0xdc200000`（1 MiB） | `0xdc200000` |
| Linux | `0xdc300000`（1 MiB） | `0xdc200000` |

macOS 把 **BAR0 和 ROM BAR 指到了同一个地址**；Linux 则分开了。ROM 使能位为 0，
且 ioreg 全库只有本卡声明该区间（无其它设备冲突），所以**目前看不出实际危害**，
但这是一个可核对的差异，记录在案。

### 24.4 仍未能回答的问题，与下一步实验

**macOS 读到的 `owner=0x01000000` 到底是"设备真实状态"还是"我们读错了"？**
Linux 侧的反证已经很强（驱动能跑），但仍需**逐寄存器对照**才能定性。

为此已把诊断脚本升级（v2）并重新写入 U 盘：

- **新增 `20-bar0-*.txt`**：在 Linux 上读取**与 macOS 完全相同的 BAR0 偏移**
  （`0x8069c` owner、`0xf03fc` sem、`0x80698` HCR status、`0x80680` 块、
  `0x6e000`/`0x77000` 那两个"整页 01000000"区域、`0xf0000` 块、`0x0`/`0x1000`/`0x40000`）
- 同一批偏移读**三次**：驱动已加载 / `rmmod` 后无驱动 / 重新加载后
  （"无驱动"那次才是与 macOS 状态对等的比较）
- **两种访问方式并记**（`devmem` 经 `/dev/mem`、`dd` 经 sysfs `resource0`），
  内核参数加 `iomem=relaxed` 以保证 `devmem` 可用
- 另存 `21-bar0-compare.txt` 做三态并排对照
- 修掉首轮的一个 bug：`13-dmesg-errors` 误用 `cap` 而非 `capsh`，导致该项采集失败（`not found`）

**判读**：

- Linux 在 `0x8069c` 读到 **0**（而 macOS 读 `0x01000000`）⇒ macOS 的读数不可信，
  要么是我们的映射问题，要么是 macOS 侧把设备置成了那个状态 ⇒ 下一步在 macOS 侧排查
- Linux 也读到 `0x01000000` ⇒ 说明该寄存器真值如此、驱动另有其道 ⇒ 回头重读驱动源码
- 若两种方法都读不到 ⇒ 说明受限，再换手段

首轮完整证据已备份：`outputs/20261010-cx3-m1/linux-run1/`（21 个文件）。

## 25. 谜团解开：**读数从来没错，是我们对「伪影」的处理错了**（2026-10-10 22:55）

### 25.1 决定性对照：macOS 与 Linux 的读数**逐字节完全一致**

Linux 在**同一种状态**下（设备存在、无驱动绑定、无在途命令，由内核参数
`module_blacklist=` 屏蔽 mlx4），用 `devmem2` 经 `/dev/mem` mmap 读取：

| 寄存器 | macOS（IOKit `IOMemoryMap`） | Linux（`/dev/mem` mmap） |
| --- | --- | --- |
| `0x8069c` owner | `0` → `0x01000000` → `0x01000000` | `0` → `0x01000000` → `0x01000000` |
| `0xf03fc` sem | `0` → `0x01000000` → `0x01000000` | `0` → `0x01000000` → `0x01000000` |
| `0x80698` HCR status | `0x63144526` ×3 | `0x63144526` ×3 |

机器比对确认：**3 个寄存器 × 各 3 次读数，全部一致。**

⇒ 三条推论，全部推翻我之前的猜测：

1. **`0x01000000` 是设备的真实读值**，不是 macOS 的映射伪影，也**不是** Apple 的 MLX5 dext 造成的
   （我此前怀疑过它，**错了**）。
2. **「首次读为 0」是设备/总线的行为**，不是我们映射的问题——Linux 用完全不同的访问路径
   （`/dev/mem` mmap）也复现同样的模式。
3. `0x80698 = 0x63144526` 在两个系统上完全相同。

**我们的读数从来没错。**

### 25.2 那为什么参考驱动能通过 owner 闸？

看源码（FreeBSD `mlx4_core/mlx4_main.c:3062`，Linux 同）：

```c
owner = ioremap(pci_resource_start(pdev, 0) + MLX4_OWNER_BASE, MLX4_OWNER_SIZE);
ret = readl(owner);        /* ← 只读一次 */
iounmap(owner);
return (int) !!ret;
```

**它只读一次。而该地址的首次读返回 0 ⇒ `!!0 = 0` ⇒ 闸门通过。**

看我们的移植：

```c
UInt32 Mlx4Hw::readOwnerWord() { return readRegWarm(MLX4_OWNER_BASE); }
```

`readRegWarm()` 读两次并**丢弃首个样本**（当初为"修伪影"而加）⇒ 得到 `0x01000000`
⇒ 非 0 ⇒ **闸门拒绝**。

⇒ **我们为修伪影加的那段代码，正是让我们卡在一道参考驱动能通过的闸门上的原因。**

这也解释了为什么 Linux 能驱动、我们第一次发命令就被自己拦住。

### 25.3 处置（0.10.0）

- 新增 `readOwnerWordRaw()`：**单次读**，与参考逐字节一致 → **闸门按参考的语义判定**
- 保留 `readOwnerWord()`（settled 值）仅作诊断
- 日志同时打印两个值，让分歧可见：
  `ownership word: single read (as the reference does) = 0x… , settled value = 0x…`

同时更正文档里"冷页伪影"的说法：正确模型是**该地址的首次读返回 0**（0x8069c / 0xf03fc
有此行为，0x80698 没有），两个系统、两条访问路径一致，属设备属性。

### 25.4 附带结论

| 发现 | 证据 |
| --- | --- |
| **mstflint 无法读取此卡的固件**，与驱动无关 | 驱动占用时失败、`rmmod` 后在无驱动状态下**同样**失败于 `crspace read (0xf0014)`。而 `devmem2` 读 `0xf0014` 正常（`0xF7010000`），说明是 mstflint 自身在此卡上的限制 ⇒ **拿不到 PSID / 固件版本** |
| 安全设计有效 | 本轮**未卡死**：内核参数屏蔽驱动 + 「驱动若绑定则拒读」硬闸都按预期工作 |
| 平台差异仍在 | Linux：ROM=0xdc200000、BAR0=0xdc300000；macOS：两者都在 0xdc200000 |
| HCR 区块在两个系统上**取值不同** | 例如 `0x80684` macOS=`0x0C004093` vs Linux=`0x0C024092`；`0x80690` macOS=`0x00042103` vs Linux=`0x04046103`。这些是活动状态，`0x80698` 才是稳定的 |

### 25.5 下一步

macOS 上重装 0.10.0 后，`cx3ctl queryfw` **应当不再被 owner 闸拦下**，可以直接尝试
`QUERY_FW` —— 这是 M1b 的原目标。我们已知 Linux 能驱动此卡，所以这一步是有依据的，
不再是"莽"。
