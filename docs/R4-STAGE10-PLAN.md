# R4 阶段 10（接协议栈）：让这张卡对系统"存在"

实现前的方案。这是 **M4 的最后一步**：做完它，`ping` 能通，项目就有第一个"能用"的版本。

**前提**：阶段 9 的发送路径必须先过（当前卡在 `feup` 待验 + 可能的 BlueFlame）。
**本方案可以先写**，因为它依赖的是"环能收能发"这个能力，而不是发送的**实现细节**。

---

## 1. 这一阶段做什么

现在 `Cx3Mlx4` 是 `IOService` 的直接子类 ⇒ **网络栈完全不知道这张卡是一张网卡**
（用户截图里"以太网"页看不到它，就是这个原因）。

阶段 10 把它变成 **`IOEthernetController` 的子类**，让 macOS 的网络栈认领它，
于是系统里会出现一个新的 BSD 接口（大概率是 **`en2`**，因为 `en0`/`en1` 已被占用），
拿到 DHCP 地址，然后 **`ping` 通**。

## 2. 已经查实的前置事实（`M2B-NOTES.md` §44，此处只列结论）

| 事实 | 状态 |
| --- | --- |
| MacKernelSDK **有**整套 `IOKit/network/` 头文件（`IOEthernetController.h` 等） | ✅ 实测 |
| 系统里 `com.apple.iokit.IONetworkingFamily (3.4)` 在跑 | ✅ 实测 |
| **免 Xcode 编得过**：探针编到"抽象类未实现 `getHardwareAddress`" | ✅ 实测 |
| **★ 必须加 `-DKERNEL`**：头文件的类声明在 `#ifdef KERNEL` 里，不加就报"unknown class name" | ✅ 实测（**这个报错会把人引向"SDK 没有这个类"的错误结论**） |

**必须实现的纯虚函数（9 个，从 SDK 头文件列出）**：

| 来自 | 必须实现 |
| --- | --- |
| `IOEthernetController` | `getHardwareAddress(IOEthernetAddress *)` —— **只有这一个** |
| `IONetworkController`（父类） | `getMaxPacketSize`、`getMinPacketSize`、`setMulticastMode`、`setPromiscuousMode`、`setMulticastList`、`enable`、`disable`、`createInterface` |

⇒ 工作量集中在**把阶段 1–9 建好的环接到这些回调上**，不是 IOKit 样板。

## 3. ★★ 最大的风险：**mbuf**（这条必须先读）

**这台机器上已经有两个网络驱动因为在 mbuf 分配上犯错而崩过内核**，签名完全相同：

```
assertion failed: m->m_type == MT_FREE && m->m_flags == M_EXT
  file: .../xnu/bsd/kern/uipc_mbuf.c, line: 5584 @uipc_socket.c:8291
```

| 时间 | 驱动 | 栈 |
| --- | --- | --- |
| 2026-10-11 13:46 | `com.insanelymac.RTL812xLucy`（RTL8125） | mbuf 簇分配器 |
| 2026-10-12 01:18 / 03:33 / 05:15 / 06:16 | `local.hiliang.AirportMtk`（MT7922） | `MCLGETL` → `mwx_mcu_alloc_msg` → … |

⇒ 这是**FreeBSD 移植驱动在这台机器上的已知崩溃模式**，而且**两个独立驱动都踩了**。
阶段 10 第一次让我们**分配 mbuf**（阶段 1–9 我们只用普通 DMA 页，从没碰过 mbuf）——
**所以这是本项目下一个最可能崩内核的地方。**

**要遵守的纪律**：
1. **先只做最小路径**：一帧一个 mbuf，只用 `mbuf_allocpacket` / `mbuf_getpacket` 这类**明确 API**，
   不手拼簇、不自己算 `M_EXT`；
2. **集群/外部存储（`M_EXT`）只由分配器设置**，我们**不写** `m_flags` / `m_type`；
3. **释放路径要对称**：`mbuf_freem_list` / `mbuf_free` 与分配一一对应，**不重复释放**；
4. RX 侧先把帧拷进 mbuf（`mbuf_copyback` 或显式 copy），**不要**试图让 mbuf 直接指向我们的 DMA 缓冲
   （那需要 `M_EXT` + 自定义簇，是最容易踩这个崩溃的做法）；
5. 每次上机**先只跑一帧**，与阶段 9 的"先一个 WQE 一帧"同一条纪律。

## 4. Info.plist 要加的东西

现在只声明了 `IOPCIFamily` + `kpi.*`。阶段 10 需要加：

```xml
<key>com.apple.iokit.IONetworkingFamily</key>
<string>3.4</string>
```

（`3.4` 是实测在跑的那个版本。）**这是本阶段唯一改到 kext 元数据的地方**，
而且它会影响 kext 能否加载 ⇒ **加载失败时第一个查这里**。

## 5. 数据通路（本阶段的核心工作）

### 5.1 RX：CQE → mbuf → 协议栈

阶段 9 已经证明**能收到帧**（72 个完成，全是我们的 QPN 64，内容是 ARP）。本阶段要把
`cqNextCqe()` 消费到的帧**交给协议栈**：

```
CQE → byte_cnt（注意：**含 FCS**，要减 4 —— FCS_KEEP=1 已实测）
    → 从 wqe_index 指向的缓冲里拷出帧
    → mbuf（遵守 §3 的纪律）
    → 交给 IOEthernetInterface 的 input 路径
    → **然后**回收补槽（顺序：先交付，再复用缓冲）
```

**两个容易搞错的点**：
* **FCS 要减 4**（CQE 报 68 = 帧 64 + FCS 4，实测）；
* **交付之后再补槽**：先补槽会让硬件把新帧写进我们还没读完的缓冲。

### 5.2 TX：协议栈 → WQE → 门铃

`outputPacket()` 把 mbuf 里的帧写进 SQ WQE 并敲门铃。**发送路径本身在阶段 9 已建好**，
本阶段是把它接到 `outputPacket` 上。

**注意**：`outputPacket` 是在**可能的中断上下文**里被调的 —— 阶段 9 的发送是
**同步等完成事件**（一次一个、等它回来）。**那样做在协议栈下会卡死**（栈期待异步提交）。
⇒ 本阶段要把发送改成**不等完成**：post 若干 WQE、敲门铃、返回、
在 TX 完成处理里回收。**这是阶段 10 真正的架构改动**，不是接线。

**而它依赖一件事**：`TX wrap` 那一段（超过两圈的环绕与 owner 位翻转）必须在阶段 9 验过——
因为异步发送就意味着**多个描述符同时在飞**，而阶段 9 刻意只允许一个。⇒ **阶段 9 的 wrap 判据通过，是阶段 10 的前置条件。**

## 6. 判据（M4 的验收就是一条：**ping 通**）

| # | 判据 | 期望 |
| --- | --- | --- |
| 1 | 系统里出现接口 | `ifconfig` 多出 **`en2`**（或下一个可用号），有 MAC = 卡上注册的 `00:11:22:33:44:55` |
| 2 | 链路状态 | `ifconfig en2` 显示 `status: active`（我们把 link state 报给栈） |
| 3 | DHCP | 拿到 `192.0.2.x`（**这张卡确实在活的 LAN 上，实测收到过真 ARP**） |
| 4 | **ARP 双向** | `arp -a` 能看到网关；**这是我们发的 ARP 被答复** ⇒ 发送路径真的通了 |
| 5 | **★ ping 通** | `ping 192.0.2.1` 有回包 —— **M4 达成** |
| 6 | 100 包无丢 | `ping -c 100` 0% loss（`verify.sh` 里那条 `M4 gate` 的 `soak`，现在还是 BLOCKED） |

`verify.sh` 里那条 **`soak: 100 consecutive pings, 0 loss  BLOCKED the interface attachment does not exist yet`**
从阶段 4 起就挂在那里——**这一阶段做完，它就该变成 PASS 或 FAIL**。

## 7. 明确**不**做的（留给 M5）

多队列 · RSS 分流 · 校验和卸载 · TSO · VLAN · jumbo · 自适应中断调节。
本阶段只用**一个** RX 环 + **一个** TX 环、1500 MTU、**不卸载**。

## 8. 实测 / 推断 / 未测（写方案时）

**实测**：§2 的四条前置事实（SDK 头文件、family 在跑、免 Xcode 编过、`-DKERNEL`）；
阶段 9 的接收（72 个完成、真 ARP、QPN 64）；`FCS_KEEP=1`；卡在活 LAN 上（同一网段收到邻居的 ARP）。

**推断（未上机）**：9 个纯虚函数的实现面就是全部工作量；`en2` 是接口名；DHCP 能拿到地址
（需 §5.1 正确、且发送要能回 ARP 响应）。

**未测**：**阶段 9 的发送还没过**（`feup` 待验）。⇒ **阶段 10 现在不能开始实现**：
它的判据（ping 通）**必须**有可用的发送路径，而发送路径的**异步改造**又依赖阶段 9 的 wrap 判据。

**所以顺序是**：阶段 9 发送过 → 阶段 9 wrap 过 → 阶段 10。**不要在发送还是坏的之前动协议栈**——
那会把"发送不通"和"接入没做对"两个未知叠在一起。

## 9. 回滚

阶段 10 会改 `Info.plist`（family 依赖）与 kext 的基类 ⇒ **这是本项目至今对系统影响最大的一次改动**
（接入网络栈意味着我们的代码会在协议栈的上下文里跑）。
* 运行期退路：`sudo tools/cx3-mlx4/rollback.sh [--purge]` + 重启；
* **`cx3off=1` 仍然有效**（它在 `start()` 最开头就返回 false）⇒ 即使阶段 10 的代码有问题，
  机器仍然能起来；
* **建议**：第一次上机带上 `cx3off=1` 验一次"只加载不启动"，再摘掉它——但那会多一次重启，
  按本项目的成本权衡，**先直接试**，失败再用它。

---

## 10. 实测后的修订（2026-10-12 22:4x）：拆成 10a/10b，并选定 `netup`

**阶段 9 已全绿**（0.43.14）：`TX WQE` 513 笔读回全对、`TX completion` 513 个零错误、
**`TX wrap` 过**（跨 3 圈、两个 owner 相位都见过）、线上 511 帧、链条 `PASS 58 FAIL 0`，
只剩 `soak` 那条 BLOCKED。⇒ §5.2 写的前置条件（wrap 必须先验过）**已经满足**。

**架构决定：bring-up 不搬进 `start()`。** 今天整台设备的初始化是用户态驱动的
（`cx3ctl`：reset → INIT_HCA → ICM → EQ → QP → …），而 `start()` 被构建门禁保护为
**只读、不写设备、不分配 DMA** —— 那是这台机器能反复重启的保险之一。
因此**新增一个选择器 `netup`**：链条把设备带起来之后由它发布接口、接上协议栈；
`verify.sh` 在链条末尾调用它，`soak` 判据就有对象了。

**拆两半**：

* **10a（不碰 mbuf，风险最低）**：`netup` 让接口出现 —— `ifconfig` 看得到、MAC 是卡上注册的
  `00:11:22:33:44:55`、链路状态 active。判据：§6 的 1、2 两条。

### 10a 上机前的拦路（2026-10-12 23:xx 实测）：**头文件比内核旧**

10a 的代码写完了、**编译链接都过**，被本项目自己的**符号账目闸门**拦下：
`undefined 490 / resolved 486` ⇒ 4 个符号解析不了 ⇒ 正确的判决是"不要安装"。

那 4 个（构建脚本现在会打名字，见它的提交信息）是：

```
_RESERVEDIONetworkController2..5        Apple 的 vtable 填充槽
```

**排查结论（实测，不是推理）**：

1. 把 `IOKit` 树指到**运行系统**的头（`MacOSX15.sdk`）而不是 MacKernelSDK ⇒ 未解析从 **14 降到 4**
   （mksdk 的 `IONetworkController.h` 与系统头**不是"多两个"，是集合不同**：
   mksdk 有 `outputStart`/`setInputPacketPollingEnable`/`pollInputPackets`/`networkInterfaceNotification`，
   系统头有 `allocatePacketNoWait`/`setHardwareAssists`）；
2. **把本机 6 个 SDK 逐个试过**（`MacOSX.sdk` / `15.5` / `15.2` / `15` / `14.5` / `14`）——
   **全部是 490/486、同样这 4 个** ⇒ **与头文件无关**；
3. 而早先实测里 `_RESERVEDIONetworkController**25**Ev` 与 `**31**Ev` **是能解析的** ⇒
   内核**导出 25/31、不导出 2..5**。

⇒ **本机装的 SDK 最高 15.5，而系统跑 15.8.1**：内核里的 `IONetworkingFamily 3.4` 是用**比这些头更新**
的布局构建的 —— 头里 2..5 是"预留填充"，内核那边已是真实方法，所以那几个填充符号在内核集合里
不存在。**换任何已装的 SDK 都修不好。**

**⇒ 需要的是一样东西（属外部获取，不是代码问题）**：**与运行内核同版本的头**，两条路任选——

* **KDK（Kernel Debug Kit）for 15.8.1**（Apple 开发者下载页；KDK 里带与内核同版本的头，正是要的东西）；
* 或**更新到 ≥ 15.8 的 Command Line Tools / Xcode**（装好后 `SYSKERNEL_HEADERS` 自动指向它）。

**明确不走的路**：自己把那 4 个填充符号定义成空函数。它们在内核的布局里**是真实方法**
（不是填充），一旦内核通过那几个槽调用我们的对象，空实现会**静默不做事** —— 正是本项目一直在避免的
那种"看起来能跑、实际错得很安静"的失败。
* **10b（mbuf 与异步）**：RX `CQE → 帧 → mbuf → 协议栈`（FCS 减 4、先交付再补槽）、
  TX `outputPacket → WQE → 门铃`（**改成不等完成**，在 TX 完成处理里回收）。
  判据：§6 的 3–6 条（DHCP / ARP 双向 / **ping 通** / 100 包 0 丢）。

**10a 的具体接口**：`Cx3Mlx4` 的基类从 `IOService` 改为 `IOEthernetController`
（它本身派生自 `IOService`），`kext/Info.plist` 加 `com.apple.iokit.IONetworkingFamily 3.4`，
构建加 `-DKERNEL`；纯虚函数表以
`tools/research/mksdk/Headers/IOKit/network/IOEthernetController.h` 为准逐个实现
（`outputPacket` 这一版先返回 `kIOReturnOutputDropped` 并计数，10b 才接真路径）。

---

## 11. 0.44.6：10a 上机前又查出两个**静态**缺口（2026-10-13，纯读代码，无需重启）

上一节把 10a 的拦路记成"头文件比内核旧"（4 个 `_RESERVEDIONetworkController2..5`）。那个已经解决（装了 Xcode，
且现在构建能过、符号账目 `OKWITHDECLAREDRESERVED`）。**但同一节之后没有再做一次"家族要不要我实现某个
回调"的普查**，于是 0.44.5 装上去时还带着两个必然失败的点。两个都不需要上机就能确认，来源是本机
运行系统 SDK 的头文件正文 + 本机能工作的参考驱动（LucyRTL8125）。

### 11.1 ★ `enable()` / `disable()` 从未实现 ⇒ `ifconfig en<N> up` 必然失败

头文件原文（`.../MacOSX15.sdk/.../IOKit/network/IONetworkController.h:1242-1254`）：

* `virtual IOReturn enable(IONetworkInterface * interface);`
  "@result Returns **kIOReturnUnsupported**. Drivers that override this method must return
  kIOReturnSuccess on success, or an error code otherwise."
* `virtual IOReturn disable(IONetworkInterface * interface);` 同款措辞。

`ifconfig en<N> up` 走的就是这条：`IONetworkInterface::enable()` → 控制器的 `enable(iface)`。
**基类返回 `kIOReturnUnsupported`，我们又没覆盖** ⇒ 接口能 attach、能匹配、能出现在 `ifconfig` 列表里，
却**永远起不来**；再往下 ping 就不可能通。参考驱动两个都实现了（`LucyRTL8125Ethernet.cpp:350` / `:395`）。

修法（0.44.6）：

* `enable()`：置 `fNetEnabled`，**重新发布链路状态**（家族只把链路状态带给"已 up"的接口），返回成功；
* `disable()`：清标志、把链路发布成 `VALID`（不带 ACTIVE），返回成功。

**两条硬约束（写进代码注释）**：这两个回调是 **"synchronized by the workloop's gate"**，而本驱动把自己的
workloop 交给了家族（`getWorkLoop`）⇒ 里面**不许发固件命令、不许取事务锁**。否则：命令会等命令信号量、
同时握着工作环门；而一个持事务锁、正在等中断（中断要靠这道门投递）的选择器就会与它死锁。

### 11.2 ★ `createWorkLoop()` 从未实现 ⇒ "只有一个 workloop"只是一句断言

头文件原文（`IONetworkController.h:1137-1147`）：`createWorkLoop()` 是**家族在第一次 `getWorkLoop()`
之前**调用的，**返回 false 会让 `IONetworkController::start()` 失败**。

**我们只覆盖了 `getWorkLoop()`** —— 于是"create"那一半留给基类，而**基类那一半会自己造一个 loop**。
也就是说 0.44.5 里那句 `getWorkLoop: ... so there is one loop and not two` 是对 `getWorkLoop()` 的断言，
而 create 路径上随时可能多出第二个。参考驱动**两个都覆盖**，并且从两个入口交出去的**同一个**自持 loop
（`LucyRTL8125Ethernet.cpp:685` / `:698`）。

修法：实现 `createWorkLoop()`，与 `getWorkLoop()` 是**同一句话**（幂等，谁先被问都一样，只有一个
`IOWorkLoop`）。

**与冻屏的关系（记为候选，不是结论）**：`Cx3Mlx4.h` 里那段注释本来就写着，加 `getWorkLoop()` 的动机是
"不加的话家族会建**第二个私有 workloop**，它的 open/close/stop 机制跑在那一只上，症状正是死锁形状"。
如果那个诊断对了，那么**只补了一半**（create 那一半没补）—— 0.44.6 把另一半补上。**这仍然只是候选**：
冻屏的真正判决靠 0.44.5 装好的三件仪器（`docs/HANG-AUDIT.md` §9）。

---

## 12. 10b 落地（0.44.7 / 0.44.8）：内核数据通路的两半

先有两份资料（`docs/10B-MBUF-CONTRACT.md`、`docs/10B-PRIMITIVES.md`，都由子代理写、逐条带 `file:line`），
再写第一行碰 mbuf 的代码 —— 因为这台机器上**已经有两个网络驱动死在 mbuf 簇分配器里**（§3）。

### 12.1 ★ 另外两个**静态**缺陷（都不需要上机就查出来了）

1. **`m3irqup` 无条件重建 workloop**（`src/Cx3Mlx4.cpp`，原 `fWorkLoop = IOWorkLoop::workLoop();`）。
   0.44.6 起 workloop 由 `createWorkLoop()/getWorkLoop()` 建立，**家族的门就挂在它上面**；这一行把它
   整个换掉 ⇒ ①中断事件源跑在**另一条 loop** 上；②此后 `getWorkLoop()` 给出第三个答案；③家族缓存的那条
   **泄漏**。这正是本项目一直怀疑的"两个 workloop"形状（见 `Cx3Mlx4.h` 里 `getWorkLoop()` 那段注释：
   家族的 open/close/stop 每次 user client 关闭都会走，而三次有日志的冻屏**末行都是 `user client closed`**）。
   修法：复用，不重建。
2. **`enable()` / `disable()` 缺失**（见 §11.1）——已修。

### 12.2 接收半边（0.44.7）

* **触发方式：workloop 定时器轮询 CQ**，而不是 arm 队列。理由写在 §10 与资料 §1.6：`arm → COMP EQE → EQ`
  是阶段 9 **唯一没证明**的一条链（实测零 EQE，且该实验本身不成立——arm 的语义是"之后有新完成才通知"，
  而当时那个 CQ 从未为空）。轮询确定、不依赖那条链，还给发送半边提供了回收完成的地方。
  **因此没有 arm ⇒ 没有 COMP 事件 ⇒ EQ 现有的 type-0 规则（拒绝消费 COMP）暂时不必改**；将来要改中断投递，
  必须**同时**改那条规则，否则 EQ 会卡在第一个 CQ 事件上（不消费 ⇒ 不 re-arm ⇒ 连命令完成都收不到）。
* **每个完成的顺序**：消费（索引前移 + 发布 CI）→ 读出帧 + 交付协议栈 → 补槽。
  **先交付再补槽**：反过来会让硬件把新帧写进正在被读的缓冲。
* **交付**：`allocatePacketNoWait()`（不是会阻塞的 `allocatePacket`，也不是 `MBUF_WAITOK`）+ 拷贝进 mbuf；
  **不用**零拷贝（那要 `mbuf_attachcluster`，正是 §3 规则 4 禁止的那条）。交给 `inputPacket` 后**不再释放**
  （所有权转移，无论返回值）。
* **长度**：CQE 的 `byte_cnt` **含 FCS**（`FCS_KEEP` 已设，实测 68 = 64 + 4）⇒ 上栈长度 = `byteCnt - 4`。
* **一个 CQ 一个消费者**：`r4raw` 用的是同一个 CQ ⇒ 每次 tick 用 `TryLock` 取事务锁，**取不到就跳过这一拍**
  （不是阻塞——本回调持有工作环门，阻塞会与"持事务锁等中断投递的选择器"互等）。
* 日志：前 8 帧与每 64 帧点名地址/ethertype；每秒一行心跳（tick/帧/丢/补槽/错误/无环 tick）。

### 12.3 发送半边（0.44.8）

* **帧缓冲环**：`netup` 里分配 `nent × 2048 B`（一个 TXBB 一个缓冲，1:1 映射，不需要第二个索引）。
  数据段直接用**总线地址** + 内存区域 lkey —— 与阶段 9 已证明的发送写法相同；该区域覆盖物理地址空间
  （`r4mr` 的 size 默认 `~0`），所以不需要第二个 MR、也不需要 MTT 条目。
* **`outputPacket` 异步**：拷帧 → 写 WQE → 敲门铃 → **不等完成**，立刻返回 `kIOReturnOutputSuccess`；
  mbuf 记在 `fNetTxMbuf[TXBB]` 里，**只有完成能释放它**。
  `Stall`（环满）**不**释放（所有权还给队列）；`Dropped` 才释放 —— 这三条的归属写进了注释，因为弄反就是泄漏或双释放。
* **不再往门铃记录里写**：txprobe 实测"不写记录也能完成"（variant 0 胜），所以这里不引入第二个机制。
* **回收**：tick 里先收再发（`netServiceTx` 在 `netServiceRx` 之后——收到的帧可能让协议栈立刻回一个 ARP 应答）。
  回收时 **stamp 用该 TXBB 自己的圈相位**（记录在 `fNetTxOwner`），不是下一圈的：用错就是**环绕之后每笔都变 ERROR
  完成**（`M2B-NOTES.md` §55）。
* **互斥**：`r4raw` 与 `txprobe` 在 `fNetEnabled` 时**拒绝运行**（它们用同一个 CQ / 同一个 SQ）。
  这条拒绝正是 `outputPacket` 敢不取事务锁的前提：一个生产者、一个消费者、没有第三方。

### 12.4 上机顺序（重启后）

1. `sudo cx3ctl probe`；
2. `sudo tools/cx3-mlx4/verify.sh up`（链条跑完不复位；末尾的 10a 步骤会 `netup` + `ifconfig up`）；
3. `ping -c 3 192.0.2.1` —— **M4 的判据**；同时看日志里 `net rx:` / `net tx:` 的行与每秒心跳；
4. 若不通：按心跳判断是"没收到帧"（RX 侧）还是"收到但没发出"（TX 侧），两者日志形状完全不同；
5. `sudo tools/cx3-mlx4/verify.sh run` 跑整条（`soak` 应从 BLOCKED 变成 PASS/FAIL）。

### 12.5 0.44.9：写完之后又拔掉的三处（都是"看起来能跑"的隐患）

1. **发送路径的并发** —— 这是我在 12.3 里留下的错。`outputPacket` 由**网络栈线程**调用，
   而回收在**工作环**上；两个线程都会动 `fR4RawTxProd/Cons` 与 `fNetTxMbuf[]` ⇒ 两帧抢同一个 TXBB、
   或完成释放掉已被覆写的 mbuf。
   **参考驱动不需要锁**，因为它把发送串行化在家族的 output 线程上
   （`configureOutputPullModel` + `startOutputThread` + `outputStart`）。**但这三个方法只存在于
   MacKernelSDK 头里，本机系统头（`MacOSX15.sdk`）里没有** —— 而本项目的 IOKit 树必须对着**运行系统的头**
   编译（§10a 实测的两套 vtable 差异）。对着系统头调用家族没有的槽 = 跳进错误的方法，不是编译错误。
   ⇒ 拉模型在这里**不可用**，发送留在 `outputPacket` 内联，串行化由自建的 `fNetTxLock` 提供
   （`TryLock`，取不到返回 `Stall`，帧还给队列而不是丢掉，单独计数）。
2. **三处日志说反了**：`enable()` 的行还写着"发送半边没实现"（12.3 之后就不成立了）。
3. **`soak` 的措辞**：现在它是真 ping，不再是"接口还不存在"。

### 12.6 明确记录的限制（不是缺陷，是还没做的 M5）

* `setMulticastMode` / `setMulticastList` / `setPromiscuousMode`：**接受并记录，但不写硬件**。
  设备上的过滤器就是阶段 5 注册的固件 MAC 表（本接口地址 + 广播，阶段 9 用真 ARP 证明过）。
  ⇒ **IPv4 单播与广播可用（M4 判据需要的正是这两样），组播投递不可用，IPv6 会察觉**。
  三个回调都返回成功并在日志里**立刻说出相反的事实**——之所以不返回"不支持"，是因为那可能让
  `ifconfig up` 因为一个本里程碑不需要的过滤器而失败；之所以不能静默接受，是因为那等于宣称一个
  本驱动没有的能力。真正写硬件的实现排在 M5（与媒体表一起）。
* 媒体表（`IONetworkMedium`）尚未发布：`ifconfig` 不会显示 media 列表，链路状态仍然有效（ACTIVE）。

### 12.7 0.44.10：给数据通路一个**用户态可读**的状态（`kCx3SelNetStat` / `cx3ctl netstat`）

动机很直接：数据通路的第一版把它的计数**只**写进内核日志（每秒一行心跳 + 前 8 帧）。于是
"帧进来了吗 / 交给栈了吗 / 被哪个检查拒了 / 定时器在跑吗" 每一个问题都要靠一次 `log stream`，
而且那个窗口**必须恰好在正确时刻开始**。这与本项目"一次重启要回答尽可能多的问题"的原则相悖。

新选择器（40）**只读、零设备访问**，返回一个结构（26 个字段），工具负责解释：

| 读到什么 | 说明什么 |
| --- | --- |
| `enabled=0` | 栈还没把接口起起来 —— 状态，不是故障 |
| `ticks=0` | 定时器没跑过 ⇒ **下面所有数字都没有意义**，先解释这个 |
| `noRings>0` | 接口活着时 HCA 被复位过 ⇒ 队列没了，重跑链条 |
| `rxFrames=0` 且 `ticks>0` | 不是定时器的问题：要么线上没帧给这个地址，要么没人读队列 |
| `rxNotDelivered>0` | `inputPacket` 收下 mbuf 却没提交给栈（与"驱动收到了"是两件事） |
| `txFrames>0` 且 `txCompleted=0` | 帧发出去了但**一个完成都没回来** ⇒ 设备没执行它们，去看 `net tx:` 写的 WQE |
| `txStalled>0` | 环满、帧还给栈（与"驱动丢帧"不同） |

`verify.sh` 的 M4 判据现在**无论成败都打印这份计数**：成功时它是"帧确实走了这个驱动"的证据
（而不是碰巧另一个接口在应答），失败时它就是"没到货 / 没发出 / 被栈拒收"的分界。

**顺带修掉一个会让 10a 步骤直接失败的缺陷**：`cx3ctl sizes` 表里**连 netup(39) 都没有**，
而 `verify.sh` 靠这张表学习每个选择器的 in/out 数与结构大小——表里没有的选择器会被"以零个输出"
调用，链条步骤于是只会报"call failed"。39/40 两行都已补上。
（`tools/test-cqe-offsets.c` 里的"最后一个选择器"绊线也按设计移动到了 40，并说明了第二次移动的原因。）
