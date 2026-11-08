# 10B — mbuf 契约（先钉 API 与归属，再写第一行数据通路代码）

本文只做一件事：在 `Cx3Mlx4` 第一次碰 mbuf 之前，把 **RX 分配/交付** 与 **TX 收包/回收** 用到的每个
API、每个返回值、每条归属规则，逐条钉死在**本机可查的头文件**上。

产出物：`tools/cx3-mlx4/docs/10B-MBUF-CONTRACT.md`（本文）。**未修改任何源码。**

## 0. 材料、版本与引用别名

本文所有事实性陈述都带 `路径:行号`。为可读性，正文使用下列**别名**，其完整路径如下：

| 别名 | 完整路径 |
| --- | --- |
| `KDH` | `/Library/Developer/CommandLineTools/SDKs/MacOSX15.sdk/System/Library/Frameworks/Kernel.framework/Versions/A/Headers` |
| `MKS` | `/Users/you/Documents/Hackintosh-MacLocal/tools/research/mksdk/Headers` |
| `LUCY` | `/Users/you/Documents/Hackintosh-MacLocal/outputs/20261011-rtl8125-panic/lucy-src/LucyRTL8125Ethernet`（**只读参考，未改动、未拷出**） |

版本事实（本机实测）：

* 运行内核 `Darwin 24.6.0 / xnu-11417.140.69.712.69~1`，`ProductVersion 15.8.1`（`BuildVersion 24H32`）。
* 在跑的 `com.apple.iokit.IONetworkingFamily (3.4)`（`kextstat` 第 53 项）。
* `KDH` 来自 `MacOSX15.sdk`（2025-04-23 快照），`MKS` 是本仓库自带的 MacKernelSDK 头。

**第一处与任务书假设的出入（必须先记下）**：

* 任务书给的 `.../Kernel.framework/Headers/bsd/sys/mbuf.h` **在本机不存在**。该 SDK 里 `Kernel.framework`
  的头是"拍平"的，mbuf 相关的两个文件实际在 `KDH/sys/mbuf.h` 与 `KDH/sys/kpi_mbuf.h`；
  `KDH/bsd/` 整个目录不存在（`ls: .../Headers/bsd: No such file or directory`）。
  `bsd/kern/...` 同样不存在。（XNU 源码树里才叫 `bsd/sys/mbuf.h`。）
* 而 `KDH/sys/mbuf.h` 本身**只是个壳**：它只 `#include <sys/kpi_mbuf.h>`（`KDH/sys/mbuf.h:88`），
  **驱动能用的 mbuf API 全集在 `KDH/sys/kpi_mbuf.h`**（1470 行）。所以下面所有 API 引用都指向 `kpi_mbuf.h`。

---

## 结论速查

| 问题 | 结论 | 依据 |
| --- | --- | --- |
| RX 用什么分配 | 首选家族 API `allocatePacket()` / `allocatePacketNoWait()` + `freePacket()`（**未被标记废弃**）；KPI 层等价物是 `mbuf_allocpacket()` / `mbuf_getpacket()`（**整个 KPI 都被打上 `__NKE_API_DEPRECATED`，但仍可用且是唯一 API**） | `KDH/IOKit/network/IONetworkController.h:825`、`:1519`、`:891`；`KDH/sys/kpi_mbuf.h:52`、`:539`、`:591` |
| 一帧一 mbuf 怎么写长度 | `mbuf_setlen()` 设分片长度，`mbuf_pkthdr_setlen()` 设包总长；`<=1518B` 一帧通常单 mbuf | `KDH/sys/kpi_mbuf.h:893`、`:987` |
| 字节怎么拷进去 | `mbuf_copyback()`（会按需扩链）或 `bcopy(src, mbuf_data(m), len)`；**没有 `mbuf_append`** | `KDH/sys/kpi_mbuf.h:807`、`:290`；全表 grep 无 `mbuf_append` |
| 交付调用 | `IONetworkInterface::inputPacket(packet, length, options, param)`；`options=0` 立即上栈、`kInputOptionQueuePacket=0x1` 入队 | `KDH/IOKit/network/IONetworkInterface.h:418`、`:424` |
| 交付后 mbuf 归谁 | 交给接口即放弃所有权；**系统头对 `inputPacket` 没写明**，`MKS` 的等价函数 `enqueueInputPacket` 明确写"Caller ceases ownership of the packet regardless of the return value" | `MKS/IOKit/network/IONetworkInterface.h:1439`；`LUCY/LucyRTL8125Ethernet.cpp:1208` |
| 首选 RX 交付路径 | 现代做法是 pull 轮询模型：`configureInputPacketPolling()` + `enqueueInputPacket()` + `pollInputPackets()`（工作驱动就用这套） | `MKS/.../IONetworkInterface.h:1204`、`:1421`；`MKS/.../IONetworkController.h:1622`；`LUCY/LucyRTL8125Ethernet.cpp:671`、`:1441` |
| TX 返回值集合 | **只有 3 个**：`kIOReturnOutputSuccess` / `kIOReturnOutputDropped` / `kIOReturnOutputStall`；**`kIOReturnOutputIgnored` 在本机任何 SDK 里都不存在** | `KDH/IOKit/network/IOOutputQueue.h:70-82`（全树 grep 无 Ignored） |
| Stall 的归属 | "The ownership of the packet reverts back to the queue" | `KDH/IOKit/network/IOOutputQueue.h:44-47` |
| `outputPacket` 能不能阻塞 | **不能**："must not block, since this may cause the network stack to be reentered from an unsafe point" | `KDH/IOKit/network/IONetworkController.h:488-490` |
| TX 是否必须 pull model | **不必须**。`createOutputQueue()` 默认返回 0，此时接口**直接**调 `outputPacket` | `KDH/.../IONetworkController.h:472-476`、`:1330-1342`、`:563-569` |
| 驱动能不能直接摸 `m_*` 字段 | **不能**。SDK 里 `mbuf_t` 是 `struct __mbuf *`，结构体**只有前置声明、定义不可见** | `KDH/sys/kernel_types.h:88`、`:100` |
| `M_EXT` 能不能自己设 | 不能：`mbuf_setflags` 注释"Certain flags such as MBUF_EXT cannot be altered"；且唯一能挂 `M_EXT` 的是 `mbuf_attachcluster`（= 任务书禁止的那条路） | `KDH/sys/kpi_mbuf.h:948`、`:411` |
| 头文件与运行内核是否一致 | **不一致**：本机最高 SDK 15.5、内核 15.8.1（`R4-STAGE10-PLAN.md` §10a 已实测 vtable 填充槽对不上） | 见 §E |

---

## A. RX 交付

### A.1 一帧一个 mbuf：用哪个分配 API、怎么写字节

**前提**：收到的一帧已在**我们自己的 DMA 缓冲**里，长度 `len <= kIOEthernetMaxPacketSize = 1518`
（含 FCS；常量定义 `KDH/IOKit/network/IOEthernetController.h:42-46`，FCS 长度 4 见 `:54-58`）。

#### A.1.1 三条合法路线（按推荐度）

**路线 ①（家族 API，最"正统"）**：`IONetworkController::allocatePacket(UInt32 size)`

* 声明 `KDH/IOKit/network/IONetworkController.h:825`；文档 `:810-824`。
* 文档原文要点（`:812-821`）："This method will always return a single mbuf unless the size requested
  (plus the alignment padding) is greater than MCLBYTES. The data buffer for the mbuf (or an mbuf chain)
  returned is aligned according to the constraints reported by getPacketBufferConstraints().
  The length fields in each mbuf returned are set by this method..."。⇒ **长度字段已由它设好**，
  且缓冲区按你在 `getPacketBufferConstraints()` 里声明的约束对齐（约束结构体见 `:251-262`）。
* 不阻塞版本 `allocatePacketNoWait(UInt32 size)`：声明 `:1519`，文档 `:1507-1517`
  ——"identical to the 'allocatePacket' method except that it will not block indefinitely"。
  ⇒ 反过来说，`allocatePacket()` **可能阻塞**，`allocatePacketNoWait()` 不会死等。
* 释放用 `freePacket(mbuf_t, IOOptionBits options)`（`:891`，`kDelayFree = 0x01` 见 `:877-879`），
  延迟批量的用 `releaseFreePackets()`（`:892-905`）。**这两个都不是废弃 API。**
* **注意**：这条路线的好处是它接入家族的"包缓冲约束 + TX 完成回收"机制；`allocatePacket` 文档还
  明确说返回的 mbuf 可以直接喂给 `IOMbufMemoryCursor`（`:818-820`）。

**路线 ②（KPI，任务书 §3 规则 1 点名的那条）**：

* `mbuf_getpacket(mbuf_how_t how, mbuf_t *mbuf)`：声明 `KDH/sys/kpi_mbuf.h:591`，文档 `:583-590`
  ——原文"Allocate an mbuf, **allocate and attach a cluster, and set the packet header flag**"。
  ⇒ 一次拿到"带簇（即 `M_EXT`，由分配器设）+ 带 `MBUF_PKTHDR`"的包 mbuf，**正好是"一帧一 mbuf"的形态**。
* `mbuf_allocpacket(mbuf_how_t how, size_t packetlen, unsigned int *maxchunks, mbuf_t *mbuf)`：
  声明 `:539-541`，文档 `:504-538`。原文"Allocate an mbuf chain to store a single packet of the
  requested length... **The mbuf type will be set to MBUF_TYPE_DATA**"（`:506-509`）。
  `maxchunks` 传入指针、返回实际分片数；要"一帧一 mbuf"就把 `maxchunks` 指向 1 并接受可能的
  `ENOBUFS`（`:518-523`）。工作驱动的 RxPool 就是用 `maxchunks = 1`（`LUCY/RTL8125LucyRxPool.cpp:71`、`:89`）。
* 错误码：`EINVAL` / `ENOMEM` / `ENOBUFS`（`:533-537`）。

**路线 ③（不推荐，仅列出以划清界限）**：`mbuf_alloccluster()`（`:416-440`）+ `mbuf_attachcluster()`
（`:411-414`）。后者文档原文"Attach an external buffer as a cluster for an mbuf"（`:387`），
**这就是"让 mbuf 指向别人的缓冲"的那条路 = 任务书 §3 规则 4 明令禁止的做法**。故意单列，是为了
让评审一眼看出"我们没走这条"。

> **术语澄清**：`mbuf_getcluster()`（`:455-484`）与 `mbuf_mclget()`（`:486-502`）是"给已有 mbuf 挂一个
> 分配器自己的簇"；`mbuf_alloccluster()`/`mbuf_attachcluster()` 是"挂一个**外部**缓冲"。前者 `M_EXT`
> 由分配器设（安全），后者由调用方的外部缓冲构成（危险）。

#### A.1.2 字节怎么拷进去

* **`mbuf_copyback(mbuf_t mbuf, size_t offset, size_t length, const void *data, mbuf_how_t how)`**
  ——声明 `KDH/sys/kpi_mbuf.h:829-831`，文档 `:807-828`。原文"Copies data from a buffer to an mbuf chain.
  **mbuf_copyback will grow the chain to fit the specified buffer**"（`:809-810`）。失败返回 `EINVAL`/`ENOBUFS`，
  且"the mbuf chain will be shorter than expected but all of the data up to the end of the mbuf chain
  will be valid"（`:813-815`）。⇒ **任务书 §3 规则 4 点名的这个 API 确实存在，且语义就是"把内核缓冲拷进 mbuf 链"。**
* **显式 copy**：`mbuf_data(m)`（`:290`，返回数据起点指针）+ `bcopy()`，再 `mbuf_setlen(m, len)`
  （`:900`）+ `mbuf_pkthdr_setlen(m, len)`（`:993`）。工作驱动走的就是这条（`LUCY/RTL8125LucyRxPool.cpp:270`
  的 `bcopy(mbuf_data(*mp), mbuf_data(m), len)`）。
* **`mbuf_copydata()` 是反向的**（从 mbuf 拷到缓冲），别用错：`:803-805`。
* **不存在 `mbuf_append`**：对 `KDH/sys/kpi_mbuf.h` 的全部 `extern` 导出做 grep，无此符号。
  想"追加"只能用 `mbuf_copyback` 或 `mbuf_setlen` 手动推长度。

#### A.1.3 `MBUF_WAITOK` 能否在 workloop/中断上下文用

* `mbuf_how_t` 定义：`KDH/sys/kpi_mbuf.h:216-220`，`MBUF_WAITOK = 0`（"Ok to block to get memory"）、
  `MBUF_DONTWAIT = 1`。文档 `:206-215`，关键一句"When in doubt, use **MBUF_DONTWAIT**"（`:211`）。
* 头文件**没有**明文禁止在 workloop/中断里传 `MBUF_WAITOK`；但方向性证据很清楚：
  * 轮询/中断近邻的调试器回调被明确要求"must avoid any memory allocation, and must never block"，
    且注明"may be called from the primary interrupt context"（`KDH/.../IONetworkController.h:1412-1427`、`:1441-1446`）。
  * 工作驱动把 `MBUF_WAITOK` 只用在**池初始化**（`LUCY/RTL8125LucyRxPool.cpp:71`、`:89`），
    数据路径上的**补充**一律用 `MBUF_DONTWAIT`（`:198`、`:213`）。
* **结论**：RX 数据路径（workloop / 中断事件源）用 `MBUF_DONTWAIT`；`MBUF_WAITOK` 只用于
  `start()` 之类可睡眠的上下文。**"workloop 里到底能不能 WAITOK"头文件未定义**（见 §F 第 4 条）。

### A.2 交付调用：`inputPacket` 的声明、options 与归属

**完整声明**（`KDH/IOKit/network/IONetworkInterface.h:418-421`）：

```
virtual UInt32 inputPacket(mbuf_t packet, UInt32 length = 0,
                           IOOptionBits options = 0, void * param = 0);
```

文档（`:397-417`）逐项含义：

* `packet`：装着收到帧的 mbuf。
* `length`："Specify the size of the received packet in the mbuf. **The mbuf length fields are updated
  with this value. If zero, then the mbuf length fields are not updated.**"（`:406-409`）
  ⇒ 你**已经**用 `mbuf_pkthdr_setlen()` 设好长度时传 0 即可；否则传 `len` 让它替你设。
* `options`（`:410-413`）：**传 0 = 绕过输入队列，立即交给协议栈；传 `kInputOptionQueuePacket` = 入队**。
* `param`："A parameter provided by the driver. Not used."（`:414`）。
* 返回值（`:415-416`）："Returns the number of packets that were submitted to the network stack,
  **or zero if the packet was enqueued**."

`kInputOptionQueuePacket` 枚举（`:424-431`）：值 `0x1`，文档原文"...Enqueue the input packet provided to
the input packet queue. **Calls to inputPacket() must be serialized.**"（`:426-427`）。

配套两个 API：

* `flushInputQueue()`（`:433-443`）：把队列里的包一次性提交上栈，返回提交数量。
* `clearInputQueue()`（`:445-452`）：**丢弃**队列里的包，"**releases them back to the free mbuf pool**"
  （`:449-451`）。⇒ 入队后被丢弃的包由**接口**释放，不是你。

**谁释放 mbuf**：

* 系统头对 `inputPacket` **没有**一句写明所有权归属（全树 grep `ownership` 只命中 `IOOutputQueue.h:46`）。
* `MKS` 里有一个**显式**表述，且它自称与 `inputPacket` 等价：`enqueueInputPacket(packet, queue, options)`
  文档（`MKS/IOKit/network/IONetworkInterface.h:1421-1450`）原文"This is the preferred interface to queue
  and submit an input packet, and is **functionally equivalent to calling inputPacket with the
  kInputOptionQueuePacket option**"（`:1436-1438`），`@param packet` 处写
  "**Caller ceases ownership of the packet regardless of the return value.**"（`:1439-1440`）。
* 旁证：工作驱动交付后**从不**再碰那个 mbuf（`LUCY/LucyRTL8125Ethernet.cpp:1208` 调 `enqueueInputPacket`
  之后只清自己的记账变量，`rxPacketHead = rxPacketTail = NULL`）。
* ⇒ **可操作结论**：把 mbuf 交出去后**不要再读、不要再 free**。失败时（上栈失败）系统头也没说谁 free
  ——见 §F 第 1 条，这是必须在上机前用一次真实失败实验确认的点。

**接口 down / 调用失败**：头文件**未记录** `inputPacket` 在接口 down 时的行为，也未定义失败时
mbuf 的归宿（`:397-421` 通篇没有）。⇒ 见 §F 第 5 条。

### A.3 `netif->inputPacket(...)` 的调用上下文

* 系统头**没有**直接规定 `inputPacket` 的上下文；但它围绕输入队列的两句话限定了并发：
  "This input queue is **not protected by a lock**. Drivers that leverage this input queue must either
  access the queue from a single thread, or enforce serialized access."（`KDH/.../IONetworkInterface.h:404-406`）
  以及 `kInputOptionQueuePacket` 处的"must be serialized"（`:427`）。
* `MKS` 侧的 pull 模型把上下文讲得更细：
  * `pollInputPackets()` 文档："The poller **can be configured to call this method on the driver's
    work loop context**"（`MKS/IOKit/network/IONetworkController.h:1634-1636`）。
  * `setInputPacketPollingEnable()` 文档："This method is **always called on driver's work loop context**"
    （`:1612`）。
  * `configureInputPacketPolling()` 的 options `kIONetworkWorkLoopSynchronous` 可"forces the poller
    thread to call pollInputPackets() on the driver's work loop context"（`MKS/.../IONetworkInterface.h:1215-1218`）。
* 工作驱动的用法印证：它**从不在裸中断里交付**，而是在 workloop 驱动的中断事件源
  （`LUCY/LucyRTL8125Ethernet.cpp:1340`）或轮询线程（`:1441`）里，先填本地队列再 `flushInputQueue()`
  （`:1344`，poll 路径则传 `pollQueue`）。
* ⇒ **结论**：`inputPacket` 合法且要求"单线程串行"上下文；**workloop / 中断事件源是合适的**（事件源本身跑在
  workloop 上，天然串行）。**"能否在主中断上下文调用"头文件未定义**（§F 第 6 条）。
  若你既想用 workloop 又可能被轮询线程并发调用，必须自己串行化（工作驱动用 `__POLLING` 位自旋锁式门闸
  `:1445-1446` 做到）。

---

## B. TX

### B.1 `outputPacket` 的返回值集合与归属

**声明**：`virtual UInt32 outputPacket(mbuf_t m, void * param);`
（`KDH/IOKit/network/IONetworkController.h:498`；注意它定义在 `IONetworkController` 上，
`IOEthernetController` 里**没有**再声明一次。）

**返回码全集**（只有 3 个，定义在 `KDH/IOKit/network/IOOutputQueue.h:70-82`）：

| 常量 | 值 | 语义（文档原文） |
| --- | --- | --- |
| `kIOReturnOutputSuccess` | `kIOOutputStatusAccepted \| kIOOutputCommandNone` | "Packet was accepted."（`:72`、`:79`） |
| `kIOReturnOutputDropped` | `kIOOutputStatusDropped \| kIOOutputCommandNone` | "Packet was dropped."（`:73`、`:80`） |
| `kIOReturnOutputStall` | `kIOOutputStatusRetry \| kIOOutputCommandStall` | "Stall the queue and retry the same packet when the queue is restarted."（`:74-75`、`:81`） |

底层状态位：`kIOOutputStatusAccepted/Dropped/Retry` 与 `kIOOutputCommandNone/Stall`
（`IOOutputQueue.h:50-55`、`:64-68`）。⇒ **任务书假设的 `kIOReturnOutputIgnored` 在本机不存在**
（`KDH` 全树与 `MKS` 全树 grep `OutputIgnored` = 0 命中）。不要按它写代码。

**归属（谁 free）**：

* 头文件里**唯一**一句显式归属："Target ran out of resources, and is unable to accept the packet.
  **The ownership of the packet reverts back to the queue.**"（`IOOutputQueue.h:44-47`，讲的是 retry/stall）
  ——`IOBasicOutputQueue::stop()` 也印证队列持有包（`:216-228`），`flush()`"Drops and **frees** all
  packets currently held by the queue"（`KDH/.../IOBasicOutputQueue.h:252-260`）。
* `Success` 与 `Dropped` 两条路的归属，**系统头没有明文**。可操作结论（由队列语义推断，非头文件直述）：
  * 返回 `Success` ⇒ 你已"接管"该 mbuf，**由你在发送完成后自行释放**（工作驱动就是这样：把 mbuf 存进
    `txMbufArray`，在 TX 完成中断里 `freePacket(m, kDelayFree)` 批量回收 `LUCY/.../LucyRTL8125Ethernet.cpp:1094`、
    `:1104`）。
  * 返回 `Dropped` ⇒ 你已丢弃该 mbuf，**同样由你 free**（工作驱动的丢包路径都在 free 之后 `continue`：
    `:476`、`:542`）。
  * 返回 `Stall` ⇒ **不要 free**，所有权回到队列，等你 `service()` 后重投（`IOBasicOutputQueue.h:230-250`）。
* ⇒ **本项目 10a 的"先返回 `kIOReturnOutputDropped` 并计数"（`R4-STAGE10-PLAN.md:212`）必须同时
  `freePacket(pkt)`，否则每收一个出包就漏一个 mbuf。**

### B.2 `outputPacket` 的上下文与禁止事项

* 上下文：头文件的**只有一句硬约束**——"The implementation in the driver **must not block**, since this
  may cause the network stack to be reentered from an unsafe point."（`KDH/.../IONetworkController.h:488-490`）
  **头文件没有**说它"可能来自中断上下文"。任务书 §5.2 那句"`outputPacket` 是在可能的中断上下文里被调的"
  属于**推断**，不是头文件原文（见 §F 第 3 条）。
* 队列语义给出实践上的上下文印象：`IOBasicOutputQueue` 用互斥保护队列，"A single producer is promoted
  to become a consumer when the queue is not active"，且"The target is expected to call those methods
  from a single threaded context, i.e. the work loop context in a network driver."（`KDH/.../IOBasicOutputQueue.h:31-47`）。
* ⇒ **禁止事项（硬性）**：不得 `IOSleep`/等待发送完成事件/`mbuf_allocpacket(MBUF_WAITOK,...)`
  /任何会睡眠或长阻塞的操作。这正是 `R4-STAGE10-PLAN.md:100-103` 要改掉"同步等完成"的原因——
  **那条推理方向是对的**（"栈期待异步提交"），只是"中断上下文"这个措辞头文件不背书。

### B.3 是否**必须**有 output thread / pull model？

**答案：不必须。把描述符在 `outputPacket` 里边直接填好并返回 success 是合法设计。**

证据（**头文件直述**，非推断）：

* `outputPacket` 文档："**If an IOOutputQueue was created by createOutputQueue(), then this method will
  be called by the output queue object. Otherwise, an interface object will call this method directly
  when it receives an output packet from the data link layer.**"（`KDH/.../IONetworkController.h:472-476`）
  ⇒ 没有输出队列时接口也照样直接调 `outputPacket`，这就是 push（inline）模型。
* `createOutputQueue()` 默认实现"**will always return 0, hence no output queue will be created**"
  （`:1330-1342`）。⇒ 不覆写它 = 走 push 模型。
* 接口的默认输出目标就是控制器 + `getOutputHandler()`，而 `getOutputHandler()` 返回 `outputPacket`
  的地址（`KDH/.../IONetworkInterface.h:462-476`；`KDH/.../IONetworkController.h:563-569`）。
* pull 模型是**显式选择加入**的：`configureOutputPullModel()` 文档"A driver that supports the pull-model
  to transmit packets **must call this method from configureInterface()**"（`MKS/.../IONetworkInterface.h:1171-1194`），
  之后才会由 per-interface 输出线程回调 `outputStart()`（`MKS/.../IONetworkController.h:1563-1600`）。

**推断部分（明确标注）**：工作驱动选了 pull 模型（`LUCY/.../LucyRTL8125Ethernet.cpp:664` +
`createOutputQueue()` 返回 `IOBasicOutputQueue::withTarget(this)` `:606` + `outputStart()` `:442`），
原因是它要在"可能非 workloop 的消费者线程"上做 TX 且能优雅处理资源耗尽（返回 `kIOReturnNoResources`
让输出线程稍后再来，`:587-590`）。**这是选择，不是 IOKit 的强制要求。** 对本项目（单环、先求 ping 通），
任务书 §5.2 的"inline 填描述符 + 异步回收 + 返回 success"是合法且更省事的路径。

### B.4 让栈认为"接口 up 且能发"必须实现什么

（全部为头文件直述）

| 必须项 | 契约 | 依据 |
| --- | --- | --- |
| `enable(IONetworkInterface *interface)` | 返回 `kIOReturnSuccess` 才算启用成功；栈在 open 之后、收发之前调用；"This method call is synchronized by the workloop's gate" | `KDH/.../IONetworkController.h:1228-1241`、`:631-640` |
| `disable(IONetworkInterface *interface)` | "should stop the hardware and disable hardware interrupt sources. Any resources allocated by enable() should also be deallocated." | `:1243-1254` |
| `configureInterface(IONetworkInterface *interface)` | 返回 `true` 才允许；"Returns false → this will cause attachInterface() to fail" | `:1317-1329` |
| `getPacketFilters(const OSSymbol *group, UInt32 *filters)` | 返回 `kIOReturnSuccess`；默认实现返回 `Unicast\|Broadcast\|Multicast\|Promiscuous` | `KDH/IOKit/network/IOEthernetController.h:273-292`、`:404-414` |
| `getHardwareAddress(IOEthernetAddress *addrP)` | **纯虚**，必须实现；"called from the workloop context" | `IOEthernetController.h:416-426` |
| `getMaxPacketSize` / `getMinPacketSize` | 返回 `kIOReturnSuccess`；范围含帧头与 FCS | `IOEthernetController.h:386-402` |
| 包过滤器响应 | `setMulticastMode` / `setMulticastList` / `setPromiscuousMode`；"If overridden, drivers must return kIOReturnSuccess on success" | `IOEthernetController.h:443-454`、`:456-482`、`:485-496` |
| 链路状态上报 | `setLinkStatus(status, activeMedium, speed, data)`；link up 传 `kIONetworkLinkValid \| kIONetworkLinkActive` | `KDH/.../IONetworkController.h:1069-1093`；`KDH/IOKit/network/IONetworkMedium.h:177-179`；工作驱动用法 `LUCY/.../LucyRTL8125Ethernet.cpp:1587` |
| 过滤器组常量 | `kIOPacketFilterUnicast=0x1`/`Broadcast=0x2`/`Multicast=0x10`/`Promiscuous=0x100` | `KDH/.../IONetworkController.h:180-185` |

补充：`enablePacketFilter`/`disablePacketFilter` 的默认实现会在多播/混杂过滤器变化时调
`setMulticastMode()`/`setPromiscuousMode()`，对 Unicast/Broadcast 的**禁用**请求静默处理
（`IOEthernetController.h:296-347`）——所以"能收发"这条链路上，Unicast/Broadcast 不需要你额外做什么。

---

## C. 项目自身纪律（`R4-STAGE10-PLAN.md` §3）逐条对照

原文在 `tools/cx3-mlx4/docs/R4-STAGE10-PLAN.md:55-62`。逐条给出"满足它的具体 API"与"会违反它的 API"。

| # | 纪律原文 | 满足它的 API | 会违反它的 API |
| --- | --- | --- | --- |
| 1 | 一帧一个 mbuf，只用 `mbuf_allocpacket` / `mbuf_getpacket` 这类明确 API，不手拼簇、不自己算 `M_EXT` | `mbuf_allocpacket(how, len, &chunks=1, &m)`（`s/kpi_mbuf.h:539`）；`mbuf_getpacket(how,&m)`（`:591`）；或家族 `allocatePacket/allocatePacketNoWait`（`IONetworkController.h:810/1507`） | 用 `mbuf_alloccluster`+`mbuf_attachcluster` 自建簇（`:439`/`:411`）；手工 `mbuf_setnext` 串出多条 mbuf 拼一帧（`:863`） |
| 2 | 集群/外部存储（`M_EXT`）只由分配器设置，我们不写 `m_flags`/`m_type` | 让上面的分配器自动设 `M_EXT`（`MBUF_EXT=0x0001`，`s/kpi_mbuf.h:77`）；需要改标志时用 `mbuf_setflags_mask`（`:964`）只动允许的位 | `mbuf_attachcluster`（`:411`，自挂外部簇）；任何试图改 `MBUF_EXT` 的 `mbuf_setflags`（`:951`，注释明说 `MBUF_EXT` 不可改）；直接访问结构体字段（本 SDK 根本不允许，见 §D） |
| 3 | 释放路径对称：`mbuf_freem_list` / `mbuf_free` 与分配一一对应，不重复释放 | `freePacket(m)`（家族，成对 `allocatePacket`，`IONetworkController.h:891`）；`mbuf_freem_list(m)` 成对"一批"（`s/kpi_mbuf.h:619`）；`mbuf_freem`（`:609`）、`mbuf_free`（`:601`） | 交付给栈后**再** `mbuf_free`（`inputPacket`/`enqueueInputPacket` 之后所有权已交出，`MKS/IONetworkInterface.h:1439`）；对同一 mbuf 既 `freePacket` 又 `mbuf_freem` |
| 4 | RX 先把帧拷进 mbuf（`mbuf_copyback` 或显式 copy），**不要**让 mbuf 直接指向我们的 DMA 缓冲 | `mbuf_copyback(m,0,len,src,how)`（`:829`）；或 `bcopy(src, mbuf_data(m), len)`（`:290`）+ `mbuf_setlen`（`:900`）+ `mbuf_pkthdr_setlen`（`:993`） | `mbuf_attachcluster` + 自定义 `extfree`，让 mbuf 直接 alias 我们的 DMA 缓冲（`:411-413`）——这正是任务书说的"最容易踩崩溃的做法" |
| 5 | 每次上机先只跑一帧 | 与 mbuf API 无关（是上机纪律） | — |

**额外发现（与 §3 措辞有出入）**：

* §3 规则 1 把 `mbuf_allocpacket`/`mbuf_getpacket` 当"明确 API"，但它们（以及**整个 `kpi_mbuf.h`**）
  在本机 SDK 里被统一打上 `__NKE_API_DEPRECATED`（`KDH/sys/kpi_mbuf.h:52`
  `#define __NKE_API_DEPRECATED __API_DEPRECATED("Network Kernel Extension KPI is deprecated", macos(10.4, 10.15.4))`）。
  ⇒ **它们是"废弃但仍可用、且仍是唯一全量 API"**；而家族的 `allocatePacket/freePacket` **没有**这个标记。
  写代码时不必为此惊慌（可用），但要知道"正统"的是家族那对。
* §3 规则 4 提到"显式 copy"；工作驱动实际用的是 `bcopy(mbuf_data(*mp), mbuf_data(m), len)`
  （`LUCY/RTL8125LucyRxPool.cpp:270`）配合 `mbuf_copy_pkthdr`（`:268`）——**与规则一致**。

---

## D. `m_*` 字段：驱动能写什么、必须留给分配器

**最硬的一条事实**：在本机 SDK 里，驱动**根本看不到 `struct mbuf` 的定义**。

* `mbuf_t` 只是 `typedef struct __mbuf* mbuf_t;`（`KDH/sys/kernel_types.h:100`），
  而 `struct __mbuf;` **只有前置声明**（`kernel_types.h:88`）。
* `KDH/sys/mbuf.h` 不含结构体定义，只 `#include <sys/kpi_mbuf.h>`（`:88`）。
* 实测：`grep -rln "struct mbuf {" $KDH/` 与对 Xcode 26.2 SDK 同样 grep 均**无命中**。
* 工作驱动全树 `grep`（`LUCY/*.cpp`、`*.hpp`）**无任何** `->m_xxx` / `m_flags` / `m_type` 字段访问。
  ⇒ "不写 `m_flags`/`m_type`"（§3 规则 2）在本 SDK 下**是编译期就强制的**，不是靠自律。

因此 §D 的答案是"**用访问器，不碰字段**"。可写的（都是访问器，且各有取值范围约束）：

| 想改什么 | 允许的访问器 | 头文件约束原文 |
| --- | --- | --- |
| 本 mbuf 数据长度 | `mbuf_setlen(m, len)`（`:900`） / `mbuf_adjustlen(m, delta)`（`:768`） | "Be careful to not set the length over the space available in the mbuf."（`:895-896`） |
| 包总长（`pkthdr.len`） | `mbuf_pkthdr_setlen(m, len)`（`:993`） / `mbuf_pkthdr_adjustlen`（`:1004`） | 只改 pkthdr 字段（`:989-994`） |
| 数据起点指针 | `mbuf_setdata(m, data, len)`（`:321`） | "The data value must be in a valid range... data value plus the length must be less than the end of the cluster / within the data area."（`:309-315`） |
| 标志位（除 `MBUF_EXT`） | `mbuf_setflags`（`:951`） / `mbuf_setflags_mask`（`:964`） | "**Certain flags such as MBUF_EXT cannot be altered.**"（`:948`、`:960`） |
| 链指针 | `mbuf_setnext`（`:863`） / `mbuf_setnextpkt`（`:881`） | 见各自文档（`:856-882`） |
| 类型 | `mbuf_settype`（`:931`） | 存在，但按 §3 规则 2 **留给分配器**（`mbuf_allocpacket` 会自动设 `MBUF_TYPE_DATA`，`:508-509`） |

**必须留给分配器的（驱动不许碰）**：

* `M_EXT` 及簇结构：由 `mbuf_getpacket`/`mbuf_mclget`/`mbuf_getcluster`/`mbuf_allocpacket` 自动建立
  （`:583-592`、`:486-502`、`:455-484`、`:504-538`），且 `mbuf_setflags` 明说 `MBUF_EXT` 不可改（`:948`）。
* `m_type`：`mbuf_getpacket`/`mbuf_allocpacket` 由分配器设（`:508-509`）；`MT_FREE=0`/`MT_DATA=1` 等定义在
  `KDH/sys/mbuf.h:92-108`——**这些是阅读用常量，不是给驱动写的**。
* `pkthdr` 里的 `rcvif` 你**可以**设（`mbuf_pkthdr_setrcvif`，`:1029`），但要在把包交出去之前，
  且文档提醒 `mbuf_pkthdr_rcvif` 的返回值不增加接口引用计数（`:1008-1018`）——本项目 10b 不设它，
  让接口自己填。

崩溃视角（把 §D 与 §3 的头号风险连起来）：`R4-STAGE10-PLAN.md:42` 记录的那条断言
`m->m_type == MT_FREE && m->m_flags == M_EXT` 是"mbuf 被重复释放/簇被二次归还"类错误的典型签名。
只要坚持"用访问器、`M_EXT` 全交给分配器、free 与 alloc 一一对应、交付后不再碰"，就绕开了这条路径。

---

## E. 与其他材料的出入（不影响结论，但会影响"照抄"）

1. **`kIOReturnOutputIgnored` 不存在**于本机 `KDH`/`MKS` 的任何头文件里（全树 grep = 0 命中）。
   任务书把它列为"家族区分的返回值之一"——不要按它写。真实集合只有 3 个（§B.1）。
2. **`bsd/sys/mbuf.h` / `bsd/kern/...` 在本机不存在**；等价物是 `KDH/sys/mbuf.h`（壳）与
   `KDH/sys/kpi_mbuf.h`（真身），见 §0。
3. **头文件比运行内核旧**：本机 Command Line Tools 最高 SDK 15.5，内核 15.8.1；
   `R4-STAGE10-PLAN.md` §10a（`:196-220`）已实测 `_RESERVEDIONetworkController2..5` 解析不了、
   而 25/31 能解析。⇒ 本文引用的**方法契约（文档语义）可信**，但**vtable 槽位编号**在
   15.8.1 上可能已变；`configureOutputPullModel`/`startOutputThread` 等 mksdk 私有 SPI
   在系统头里是 `OSMetaClassDeclareReservedUnused`（`KDH/.../IONetworkController.h:1501-1504`），
   与 mksdk 的 `ReservedUsed`（`MKS/.../IONetworkController.h:1596-1673`）**对不上号**。
4. **工作驱动的 RxPool 用了 `mbuf_setdata(m, data, 0)`**（`LUCY/RTL8125LucyRxPool.cpp:85`、`:103`、`:152`）
   来"重置"分配出来的包数据指针与长度——这是它自己的用法，不是头文件推荐的必需步骤；
   本项目不必模仿（`allocatePacket` 已经把长度字段设好了，`IONetworkController.h:818-819`）。

---

## F. 无法从这些材料确定的（不猜，标 unknown，并说明什么能定论）

1. **`inputPacket` 返回失败时，mbuf 归谁？** 系统头对 `inputPacket` 完全没写所有权
   （`KDH/.../IONetworkInterface.h:397-421`）；`MKS` 只对等价的 `enqueueInputPacket` 写了
   "caller ceases ownership regardless of the return value"（`MKS/.../IONetworkInterface.h:1439-1440`）。
   `inputPacket` 本身是同一回事，但**明文只覆盖 `enqueueInputPacket`**。
   **能定论的做法**：读 XNU 源码 `bsd/kern/uipc_mbuf.c` / IONetworkingFamily 的
   `IONetworkInterface::inputPacket`（或在 10a 上做一次"构造必然失败"的实验，看是否漏 mbuf）。
2. **`outputPacket` 返回 `Success`/`Dropped` 后谁 free？** 头文件只对 Stall 写了归属
   （`IOOutputQueue.h:44-47`）。**能定论的做法**：XNU `IONetworkInterface`/DLIL 的输出路径源码，
   或对运行内核二进制做符号/反汇编核对。
3. **`outputPacket` 是否可能来自（主）中断上下文？** 头文件只有"must not block"
   （`KDH/.../IONetworkController.h:488-490`），**没有**上下文声明。任务书 §5.2 的"中断上下文"是推断。
   **能定论的做法**：在 `outputPacket()` 首行打印 `in_interrupt()`/`get_preemption_level()` 类信息实测，
   或读 DLIL 源码确认调用点。
4. **`MBUF_WAITOK` 能否在 workloop / 线程化中断里用？** 头文件只给方向（`kpi_mbuf.h:206-215`），
   真正的阻塞语义在 `uipc_mbuf.c` 里。**能定论的做法**：KDK（与 15.8.1 同版本）里的
   `sys/mbuf.h` + `uipc_mbuf.c` 源码；或直接把数据路径写成 `MBUF_DONTWAIT` 绕开这个问题。
5. **接口 down 时 `inputPacket` 的行为**（返回值 / 是否仍接收 / mbuf 归宿）：头文件未定义。
   **能定论的做法**：源码或实测（down 掉接口后仍投递一帧，观察返回值与泄漏计数）。
6. **`inputPacket` 能否在主中断上下文调用**：头文件未定义（只有"must be serialized"）。
   工作驱动把投递放在 workloop 事件源/轮询线程里（`LUCY/...:1340`、`:1441`），**未证明**裸中断可行。
   **能定论的做法**：源码或实测。
7. **`MCLBYTES` / `MLEN` / `MHLEN` 的数值**：SDK 只给函数 `mbuf_get_mlen()`/`mbuf_get_mhlen()`/
   `mbuf_get_minclsize()`（`kpi_mbuf.h:1209`、`:1218`、`:1228`），**常量不在头里**；只能运行时取值。
   不要写死"2048/4096"之类假设（`mbuf_alloccluster` 文档提到 2048/4096/16384 是**簇**的粒度，
   `:422-430`，不等于 `MLEN`）。
8. **运行内核 15.8.1 上 `IOEthernetController`/`IONetworkController` 的真实 vtable 槽位**：
   本机头与内核不一致（§E.3），任何"按头文件算槽位"的结论都不可靠。
   **能定论的做法**：KDK for 15.8.1，或 ≥15.8 的 Command Line Tools（`R4-STAGE10-PLAN.md:225-235` 已给出这两条路）。
9. **`getFeatures()` / `setHardwareAssists()` 是否为本阶段必需**：头文件说前者"always return 0"
   （`IONetworkController.h:500-506`）、后者是"通知硬件加速配置变化"（`:1523-1538`），
   本阶段不做卸载 ⇒ 推测不需要，但**"不需要"这点头文件没有明说**。
