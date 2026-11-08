# M5 — macOS 对驱动的要求（媒体/链路、校验和、VLAN、巨帧、多队列、中断节流）

下一里程碑（M5）要碰的面比 10a 宽：媒体与链路报告、校验和卸载、VLAN、巨帧、多队列/RSS、
中断节流。本文只做一件事：**把每一项"macOS 要求驱动做什么"钉在*本机*的头文件上**，
并逐条标明**哪些 API 根本不在运行系统的头里**。

最后这一条不是学术问题。本项目的 IOKit 树是对着**运行系统的头**编译的
（`BUILD:18-41`：`-I"$SYSKERNEL_HEADERS"` 在 `-I"$SDK/Headers"` **之前**，`SYSKERNEL_HEADERS`
取 `ls -d /Library/Developer/CommandLineTools/SDKs/MacOSX*.sdk | tail -1`），
而两套头的虚表不是"多两个/少两个"，是**集合不同**；对着一套头实现的回调，在另一套头的
槽位表里可能就是别人的方法。`PLAN:170-205` 已经为此量过一次代价，结论在 §1 复述。

**未修改任何源码。** 本文是本次任务唯一的产物。

---

## 0. 材料、别名与版本事实

| 别名 | 完整路径 |
| --- | --- |
| `KDH` | `/Library/Developer/CommandLineTools/SDKs/MacOSX15.sdk/System/Library/Frameworks/Kernel.framework/Headers`（**运行系统 SDK**，是 `MacOSX15.5.sdk` 的符号链接） |
| `MKS` | `/Users/you/Documents/Hackintosh-MacLocal/tools/research/mksdk/Headers`（本仓库自带的 MacKernelSDK 头） |
| `LUCY` | `/Users/you/Documents/Hackintosh-MacLocal/outputs/20261011-rtl8125-panic/lucy-src/LucyRTL8125Ethernet`（**只读参考，另一个会话的目录，未改动、未拷出**） |
| `PBX` | `.../lucy-src/LucyRTL8125Ethernet.xcodeproj/project.pbxproj` |
| `PLAN` | `tools/cx3-mlx4/docs/R4-STAGE10-PLAN.md`（本仓库） |
| `BUILD` | `tools/cx3-mlx4/build.sh`（本仓库） |

版本事实（本机实测，2026-10-13）：

* 运行内核 `macOS 15.8.1 / 24H32`（`sw_vers`）。
* 装的 SDK 最高 **15.5**（`ls -d /Library/.../MacOSX*.sdk` → `MacOSX14.5 / 14 / 15.2 / 15.5 / 15`，
  其中 `MacOSX15.sdk → MacOSX15.5.sdk`）。**头比内核旧** —— 这不是本文的推测，是 `PLAN:186-205`
  的实测结论，也是 §1 那四个"declared reserved"符号的来源。
* `BUILD:39-41` 的 `tail -1` 落在 `MacOSX15.sdk`，也就是 `KDH`。本文引用 `KDH` 即等于本项目
  实际使用的头（两者同源）。
* 参考驱动**不是**对着 `KDH` 编的：`PBX:3719-3720` 把 `KERNEL_EXTENSION_HEADER_SEARCH_PATHS`
  与 `KERNEL_FRAMEWORK_HEADERS` 都指向 `$(PROJECT_DIR)/MacKernelSDK/Headers`，
  并且 `LUCY/LucyRTL8125Ethernet-Prefix.pch:15` 定义了 `#define __PRIVATE_SPI__`。
  **这两条合起来才有 Lucy 的 pull model**（§6、§8）。

路径注记：该 SDK 里 `Kernel.framework` 的头是"拍平"的（不存在 `bsd/` 子树），
mbuf 相关的两个文件在 `KDH/sys/mbuf.h` 与 `KDH/sys/kpi_mbuf.h`；这一点 `10B-MBUF-CONTRACT.md`
已经记过，本文照旧。

---

## 1. 先讲结论：本机有两套 vtable，且集合不同

这是后面每一节都要用到的前提，所以先钉死。

**两套头声明的方法集合不一样**（`PLAN:183-185` 的实测，本次逐条复核）：

* `MKS` 的 `IONetworkController.h`：`outputStart`、`setInputPacketPollingEnable`、`pollInputPackets`、
  `networkInterfaceNotification`（`MKS/IOKit/network/IONetworkController.h:1596,1616,1643,1667`）。
* `KDH` 的同一位置：`allocatePacketNoWait`（`KDH/IOKit/network/IONetworkController.h:1519`）、
  `setHardwareAssists`（`KDH:1536`）。
* **两个名字在对方头里都不存在**（`setHardwareAssists` 在 `MKS` 里只在 `IOSkywalkNetworkInterface.h`；
  `outputStart` 在 `KDH` 里只作为 `configureOutputStartDelay` 的子串出现，见 §6）。

**虚表长度差两槽**：`BUILD:22-27` 记的是"`MKS` 声明 74 个虚函数、`MacOSX15.sdk` 声明 72 个"，
对着 `MKS` 编译出来的 vtable 因此比内核集合里的家族长两个槽；当时链接向 Apple 的填充槽要符号
（`_RESERVEDIONetworkController2..7`、`_RESERVEDIOEthernetController2..9`）**共 14 个未解析**，
换成 `KDH` 后降到 4 个（`_RESERVEDIONetworkController2..5`）。

**这 4 个的判读（`PLAN:186-205`，本次未重新推导，属引用）**：本机 6 个 SDK
（`MacOSX.sdk`/`15.5`/`15.2`/`15`/`14.5`/`14`）逐个试过都是同样 4 个未解析；
而 `_RESERVEDIONetworkController25Ev`、`**31**Ev` **能**解析。
⇒ 内核（15.8.1）在**导出 25/31、不导出 2..5**，即 `KDH` 里写作
`OSMetaClassDeclareReservedUnused(IONetworkController, 2..5)`（`KDH:1501-1504`）的那四个槽，
**在内核那边是真实方法**。当前构建闸门对此的判决是
`kmutil print-diagnostics` 的 `Dependencies: OK`（`BUILD:427-434`），即计数不匹配是告警、
加载判定以 `print-diagnostics` 为准。

**由此得到本文两条硬规则**：

1. **凡是 `KDH` 没有声明的成员，不要凭 `MKS` 直接调用或覆盖**。两种失败方式要分清：
   （i）对着 `KDH` 编译却写了 `MKS` 独有的名字 ⇒ **硬编译错误**（成员不存在），反而安全；
   （ii）对着 `MKS`（或定义了 `__PRIVATE_SPI__`）编译、跑在另一套布局的家族上 ⇒
   编译链接都过，**调用落到别的槽上**，这是真正危险的那种（`PLAN:170-205` 的成本就在这里）。
   本项目的规则是只走 `KDH`，所以 (i) 会替我们挡住 (ii)。
2. **`KDH` 声明了、`MKS` 没有的成员（`setHardwareAssists`、`allocatePacketNoWait`）本项目可以放心用** ——
   因为本项目是对着 `KDH` 编的，而参考驱动的构建看不到这两个名字。

---

## 2. 媒体与链路（MEDIA AND LINK）

### 2.1 家族要求驱动做什么

要让 `ifconfig` / 上层拿到媒体信息，驱动要按顺序做三件事，头文件都写明在
`KDH/IOKit/network/IONetworkController.h`：

1. **建一个 `IONetworkMedium` 字典**。介质对象用工厂方法造：
   `static IONetworkMedium * medium(IOMediumType type, UInt64 speed, UInt32 flags = 0, UInt32 index = 0, const char * name = 0)`
   （`KDH/IOKit/network/IONetworkMedium.h:342-346`；`init` 同款在 `:321-325`）。
   入字典用 `static bool addMedium(OSDictionary * dict, const IONetworkMedium * medium)`（`:252`），
   查询用 `getMediumWithType`（`:278`）/`getMediumWithIndex`（`:294`）。
   介质名由 `nameForType()`（`:239`）按类型生成，**必须唯一**，因为它就是字典的 key
   （`:307-309` 的 `@param name` 说明）。
2. **发布字典**：`virtual bool publishMediumDictionary(const OSDictionary * mediumDict)`
   （`KDH:1051`）。文档（`:1023-1050`）明确：**方法会复制字典**再挂进驱动的属性表，
   调用方返回后可以释放；**可以多次调用**（硬件能力动态变化时）；若能力是静态的，
   惯例是**只在 `start()` 里调一次**。发布后属性表里出现 `kIOMediumDictionary`
   = `"IOMediumDictionary"`（`KDH:66`）。
   同一段文档点名了**四个依赖字典的方法**（`:1036-1044`）：`setSelectedMedium()`、
   `getSelectedMedium()`、`getMediumDictionary()`、`copyMediumDictionary()`。
3. **报告链路**：`virtual bool setLinkStatus(UInt32 status, const IONetworkMedium * activeMedium = 0,
   UInt64 speed = 0, OSData * data = 0)`（`KDH:1088`）。文档（`:1069-1087`）逐字要求：
   * 驱动**必须在检测到链路变化时调用**它；家族会更新注册表里的链路属性并向上层发事件；
   * `status` 用 `IONetworkMedium.h` 里的链路位：`kIONetworkLinkValid = IFM_AVALID`（`:177`）、
     `kIONetworkLinkActive = IFM_ACTIVE`（`:178`）、`kIONetworkLinkNoNetworkChange = IFM_WAKESAMENET`（`:179`）；
   * `activeMedium` 是**已发布字典里的一个对象**，"may not be the same as the selected medium"
     （活动介质不必等于选中的介质），**链路不通时传 0**；
   * `speed` 单位为 bit/s，**传 0 表示从 medium 对象里取速度**。
   相关属性名：`kIOLinkSpeed`（`KDH:104`）、`kIOLinkStatus`（`:113`）、`kIOLinkData`（`:122`）。

### 2.2 `selectMedium` / `setSelectedMedium` / `getSelectedMedium` 各要什么

* `virtual IOReturn selectMedium(const IONetworkMedium * medium)`（`KDH:708`）：**家族在客户端要求换介质时调它**。
  文档（`:694-707`）明确：**实现必须在改完之后调用 `setSelectedMedium()`**；子类只覆盖 `selectMedium`，
  不要覆盖 `selectMediumWithName()`（`:710-728` 的说明："Subclasses should override selectMedium() and
  not this method"）。
* `virtual bool setSelectedMedium(const IONetworkMedium * medium)`（`KDH:1066`）：
  **驱动在硬件切到某个介质后调它**，家族据此更新注册表属性；**如果该对象不在已发布的字典里，
  返回 false**（`:1053-1065`）。另有一个非虚的 `setCurrentMedium()`（`:1067`），
  Lucy 走的是这一个（`LUCY/LucyRTL8125Ethernet.cpp:1001`），落在 `selectMedium()` 内部。
* `virtual const IONetworkMedium * getSelectedMedium() const`（`KDH:537`）：
  返回之前 `setSelectedMedium()` 记下的那个对象；**驱动没发布字典时它返回 0**
  （`:529-536`），所以驱动要自己兜底。

### 2.3 要不要周期性地查链路

**家族没有任何"周期性查链路"的接口，也没有这条要求。** 依据：整个
`KDH/IOKit/network/` 里没有 timer/watchdog/poll 之类的链路 API（比对：驱动侧唯一与时间有关的是
`IONetworkInterface::configureOutputStartDelay`，`KDH/IOKit/network/IONetworkInterface.h:1006`，
那是**输出队列启动延迟**，与链路无关）；家族只被动的在 `setLinkStatus()` 被调用时动作
（`KDH:1069-1087`）。

但参考驱动仍然做了，而且做了**两条腿**（这是驱动自己的选择，不是家族要求）：

* **中断腿**：链路变化中断里直接调 `checkLinkStatus()`（`LUCY/LucyRTL8125Ethernet.cpp:1371`，
  在 `status & LinkChg` 分支内）；
* **定时器腿（兜底）**：`IOTimerEventSource`，周期 `kTimerPeriod = 1000000000UL`（1 秒，
  `LUCY/LucyRTL8125Ethernet.hpp:249`）；链路起来后 `wakeAtTime(now + timerInterval)`
  重新上弦（`LUCY:1301`、`LUCY:1672`），定时器回调里做计数器读取、TX 死锁检查与**再次上弦**
  （`LUCY:1643-1676`）。

⇒ 结论：**macOS 不要求**；1 Hz 的本地 watch（Lucy 的形状）是对"链路变化中断不可靠/被合并"
的合理兜底。本项目若只做一次链路读数，风险是链路状态永远不更新。

### 2.4 一个中介质字典都不发布，会发生什么（= 现状）

能从 `KDH` 头文件**确定**的部分：

* `getMediumDictionary()` 在没发布过之前返回 0（`KDH:540-549` 的 `@discussion`）；
* `copyMediumDictionary()` 同理（`:553-561`）；
* `setSelectedMedium()` 因为匹配不到条目而返回 false（`:1053-1065`）；
* `getSelectedMedium()` 返回 0（`:529-536`）；
* `setLinkStatus()` 仍然可调用，但 `activeMedium` 只能是 0，`speed` 只能显式给值
  （`:1074-1082`：`activeMedium` 必须是"已发布字典里的对象"，链路不通传 0；`speed=0`
  时才回落到 medium 对象）。

**不能从这些来源确定**的部分：`ifconfig` 在"无字典"下**具体打印什么**（是不是完全不显
`media:` 行、还是显 `media: none`）。头文件不描述用户态渲染。**能定案的做法**：
装一版跳过 `publishMediumDictionary()` 的构建，在真机上 `ifconfig en<N>` 与
`system_profiler SPEthernetDataType` 各看一次（A/B 各一次即可定论）。

### 2.5 参考驱动怎么做的（可直接抄形状）

* 介质表 + 速度表：`LUCY/LucyRTL8125Setup.cpp:130-152`（`mediumTypeArray` 用
  `kIOMediumEthernetAuto`/`10BaseT`/`100BaseTX`/`1000BaseT`/`2500BaseT` 加
  `kIOMediumOptionFullDuplex|FlowControl|EEE` 组合；速度表并行给出 `10*MBit … 2500*MBit`）。
* 造对象 + 入字典 + 发布：`LUCY/LucyRTL8125Setup.cpp:164-205`
  （`IONetworkMedium::medium(...)` `:174`；`addMedium` `:179`；`publishMediumDictionary` `:188`）。
* 选中 + 报告：`selectMedium()` 按 `medium->getIndex()` 分支配硬件（`LUCY:907-1005`），
  末尾 `setCurrentMedium(medium)`（`LUCY:1001`）；
  链路起来时 `setLinkStatus(kIONetworkLinkValid | kIONetworkLinkActive, mediumTable[mediumIndex], mediumSpeed, NULL)`
  （`LUCY:1587`）；链路断开时只给状态位、不给介质
  `setLinkStatus(kIONetworkLinkValid)`（`LUCY:1631`）。
* `enable()` 里先读回"上次选中的介质"，没有就回落到自动协商再选一次
  （`LUCY:368-374`）—— 这是应对"没发布字典 ⇒ `getSelectedMedium()` 返回 0"的标准兜底。
* 该驱动同时实现了 `enable()`/`disable()`（`LUCY:350`、`:395`）；`PLAN:216-234` 记过：
  **基类 `enable(IONetworkInterface*)` 默认返回 `kIOReturnUnsupported`**（`KDH:1241-1254` 的
  `@result`），不覆盖它就永远 `ifconfig up` 不成功。

### 2.6 本机可调用性

**本节用到的每一个 API 都在 `KDH` 里**（`publishMediumDictionary` `:1051`、`setSelectedMedium` `:1066`、
`getSelectedMedium` `:537`、`selectMedium` `:708`、`selectMediumWithName` `:728`、
`setLinkStatus` `:1088`、`IONetworkMedium::medium` `MKS` 与 `KDH` 都有）。
⇒ **媒体/链路这一块没有任何 `MKS`-only 依赖，零 vtable 风险。**

顺带关闭一个任务清单里的疑问：`IONetworkData` **不是**媒体机制。它是"一个固定大小的具名缓冲区"
（`KDH/IOKit/network/IONetworkData.h:98-99` 的 `@class`/`@abstract`），由
`IONetworkInterface::addNetworkData()`（`KDH/IOKit/network/IONetworkInterface.h:557`）挂到接口上，
用于驱动自有统计/参数，不参与 `ifconfig` 的 media 列表。

---

## 3. 校验和卸载（CHECKSUM OFFLOAD）

### 3.1 家族问驱动"你会什么"

只有一个入口，签名与返回约定都在 `KDH/IOKit/network/IONetworkController.h:967-969`：

```
virtual IOReturn getChecksumSupport( UInt32 * checksumMask, UInt32 checksumFamily, bool isOutput );
```

文档（`:947-966`）规定：

* 默认返回 **`kIOReturnUnsupported`**；覆盖者**必须返回 `kIOReturnSuccess`**；
  **"Any other return value will be interpreted as a lack of checksum support, regardless of the value
  returned through the first argument."**（返回别的值 = 不支持，哪怕掩码填得再满）。
* `checksumFamily` 传 `kChecksumFamilyTCPIP = 0x00000001`（`:936`）。
* `isOutput = true` 问"发送插入"，`false` 问"接收校验"；对称硬件可以忽略这个参数。
* 掩码位在 `:936-944`：`kChecksumIP 0x0001`、`kChecksumTCP 0x0002`、`kChecksumUDP 0x0004`、
  `kChecksumTCPIPv6 0x0020`、`kChecksumUDPIPv6 0x0040`、`kChecksumTCPNoPseudoHeader 0x0100`、
  `kChecksumUDPNoPseudoHeader 0x0200`、`kChecksumTCPSum16 0x1000`。
  注意文档明确 **NoPseudoHeader 两个"not currently supported on the output path"**（`:921`、`:926`）。

### 3.2 "打开卸载"没有 setter

**`setChecksumSupport` 在本机两套头里都不存在**（`KDH` 与 `MKS` 全域 grep 均无）。
方向是**家族问、家族自己开**：`getChecksumSupport()` 报出能力后，由家族/协议栈去配置 ifnet。
相关 API 在 `KDH/net/kpi_interface.h`：

* `extern errno_t ifnet_set_offload(ifnet_t interface, ifnet_offload_t offload)`（`:1060`）；
* `extern errno_t ifnet_set_offload_enabled(ifnet_t interface, ifnet_offload_t offload)`（`:1073`）；
* 卸载位 `ifnet_offload_t` 的枚举（`:192-210`）：`IFNET_CSUM_IP 0x00000001`、`IFNET_CSUM_TCP 0x2`、
  `IFNET_CSUM_UDP 0x4`、`IFNET_CSUM_FRAGMENT 0x8`、`IFNET_CSUM_TCPIPV6 0x20`、`IFNET_CSUM_UDPIPV6 0x40`、
  `IFNET_VLAN_TAGGING 0x00010000`（`:200`）、`IFNET_VLAN_MTU 0x00020000`（`:201`）、
  `IFNET_MULTIPAGES 0x00100000`（`:202`）、`IFNET_TSO_IPV4 0x00200000`（`:203`）、
  `IFNET_TSO_IPV6 0x00400000`（`:204`）、`IFNET_LRO 0x10000000`（`:208`）。

另一条"声明能力"的路是特性位 `virtual UInt32 getFeatures() const`（`KDH:506`），位定义在
`KDH:227-236`：`kIONetworkFeatureHardwareVlan 0x002`（`:228`）、`kIONetworkFeatureMultiPages 0x008`（`:230`）、
`kIONetworkFeatureTSOIPv4 0x010`（`:231`）、`kIONetworkFeatureTSOIPv6 0x020`（`:232`）、
`kIONetworkFeatureTransmitCompletionStatus 0x040`（`:233`）、`kIONetworkFeatureLRO 0x200`（`:236`）。
参考驱动两个都用了：`getChecksumSupport()`（`LUCY:816-830`）+ `getFeatures()`
（`LUCY:835-847`，返回 `kIONetworkFeatureMultiPages | kIONetworkFeatureHardwareVlan`
外加 TSO 位）。

### 3.3 接收方向：把校验结果交给协议栈

两条等价的路，**都只在 `KDH` 里有定义**：

**(a) 家族自己的编码器** —— `virtual bool setChecksumResult(mbuf_t packet, UInt32 checksumFamily,
UInt32 resultMask, UInt32 validMask, UInt32 param0 = 0, UInt32 param1 = 0)`
（`KDH:995-1000`）。文档（`:971-994`）要求驱动**在把包交给协议栈之前**调用；
`resultMask` = "做过/能报告"的集合，`validMask` = "硬件确认有效"的集合，
且 **"A checksum cannot be marked valid unless it has also been checked."**

**(b) 裸 mbuf KPI** —— `extern errno_t mbuf_set_csum_performed(mbuf_t mbuf,
mbuf_csum_performed_flags_t flags, u_int32_t value)`（`KDH/sys/kpi_mbuf.h:1196-1197`）。
标志位（`:199-202`）：`MBUF_CSUM_DID_IP 0x0100`、`MBUF_CSUM_IP_GOOD 0x0200`、
`MBUF_CSUM_DID_DATA 0x0400`、`MBUF_CSUM_PSEUDO_HDR 0x0800`；
`value` 在置了 `MBUF_CSUM_DID_DATA` 时填**硬件算出的 TCP/UDP 头校验值**（`:1185-1195`）。
清位用 `mbuf_clear_csum_performed`（`:1238`）。

参考驱动走的是 (b)：`LUCY:1463-1477` —— 按描述符状态位攒 `performed`，然后
`mbuf_set_csum_performed(m, performed, value)`；其中数据校验用 `value = 0xffff` 假装"有效值"
（`LUCY:1474` 的注释 `// fake a valid checksum value`）。**注意这在语义上是"我只报告校验结果、
不回报具体数值"的写法**，与 `setChecksumResult` 的 `validMask` 语义等价但更粗糙；
本项目若要在 M5 做"如实定级"的接收卸载，建议按 (a) 的语义实现，或至少保持
"`MBUF_CSUM_IP_GOOD` 只在真校验过时置位"。

### 3.4 发送方向：问协议栈"这包要不要卸载"

同样两条路：

**(a)** `virtual void getChecksumDemand(const mbuf_t packet, UInt32 checksumFamily, UInt32 * demandMask,
void * param0 = 0, void * param1 = 0)`（`KDH:1017-1021`）；文档（`:1002-1016`）：
"驱动**必须**调用它来拿到要算哪些校验和"。

**(b)** `extern errno_t mbuf_get_csum_requested(mbuf_t mbuf, mbuf_csum_request_flags_t * request,
u_int32_t * value)`（`KDH/sys/kpi_mbuf.h:1155-1156`）。请求位（`:167-171`）：
`MBUF_CSUM_REQ_IP 0x0001`、`MBUF_CSUM_REQ_TCP 0x0002`、`MBUF_CSUM_REQ_UDP 0x0004`、
`MBUF_CSUM_REQ_TCPIPV6 0x0020`、`MBUF_CSUM_REQ_UDPIPV6 0x0040`；
**`value` 参数当前不支持**（`:1152-1153` 的 `@param value` "This parameter is currently unsupported."）。
清位用 `mbuf_clear_csum_requested`（`:1181`）。

参考驱动走 (b)：`LUCY:516`（`mbuf_get_csum_requested(m, &offloadFlags, &mss)`，`mss` 当哑变量）
并按位映射到硬件描述符（`LUCY:518-526`）。TSO 走另一条：
`mbuf_get_tso_requested`（`KDH/sys/kpi_mbuf.h:1169`，Lucy 用在 `LUCY:471`）。

> 版本注意：`KDH/sys/kpi_mbuf.h` 里这一整组 KPI 都带 `__NKE_API_DEPRECATED`
> （`:52` 定义为 `__API_DEPRECATED("Network Kernel Extension KPI is deprecated", macos(10.4, 10.15.4))`）。
> 即**声明为"已废弃"但仍然存在**，参考驱动与 IOEthernet 家族都在用；本项目照此使用即可，
> 但要在注释里如实写明这是 deprecated KPI（因为 `-Werror` 类构建可能被这条警告挡住）。

### 3.5 本机可调用性

本节全部 API 在 `KDH` 与 `MKS` 里**都有**：`getChecksumSupport`、`setChecksumResult`、
`getChecksumDemand`、`ifnet_set_offload`、`mbuf_get_csum_requested`、`mbuf_set_csum_performed`
（两套头的逐名比对见 §8）。**零 `MKS`-only 依赖。**

---

## 4. VLAN

### 4.1 声明能力

驱动在 `getFeatures()` 里置 **`kIONetworkFeatureHardwareVlan = 0x002`**
（`KDH/IOKit/network/IONetworkController.h:228`；文档 `:193-198`：置位表示"控制器支持硬件 VLAN"）。
（"置这个位 ⇒ 家族打开 ifnet 的 `IFNET_VLAN_TAGGING`（`KDH/net/kpi_interface.h:200`）"这一步
是**推断**：`KDH:193-198` 只写"表示控制器支持硬件 VLAN"，到 `IFNET_*` 的映射是家族内部行为，
头文件没有逐字写明。）要让**大于标准 MTU 的带标签帧**也成立，还需要 `IFNET_VLAN_MTU`（`:201`）——
参考驱动置了 VLAN 特性位（`LUCY:837`）并在算 MTU 时把 `VLAN_HLEN` 计入
（`LUCY/LucyRTL8125Ethernet.cpp:1036`、`LUCY/LucyRTL8125Hardware.cpp:881`）。

### 4.2 发送：要 tag 就问，把 tag 塞进硬件

`virtual bool getVlanTagDemand(mbuf_t m, UInt32 * vlanTag)`（`KDH/IOKit/network/IOEthernetController.h:563`，
紧随 `OSMetaClassDeclareReservedUsed( IOEthernetController, 0)` `:549` 之后，
并在 `:565` 标记为 reserved slot 1）。文档（`:551-562`）：
**返回 true 表示该包需要插标签；`*vlanTag` 低 16 位是 802.1Q 优先级 + VLAN ID，主机序**；
高 16 位当前未用。
参考用法：`opts2 |= (getVlanTagDemand(m, &vlanTag)) ? (OSSwapInt16(vlanTag) | TxVlanTag) : 0;`
（`LUCY:553`）。

### 4.3 接收：把硬件剥下来的 tag 编回包上

`virtual void setVlanTag(mbuf_t m, UInt32 vlanTag)`（`KDH/IOKit/network/IOEthernetController.h:578`）。
文档（`:567-577`）：**硬件已经把标签剥掉的接收包**，驱动在交给协议栈之前调用；
`vlanTag` 低 16 位是 tag+优先级，主机序，高位应置 0。
参考用法：`if (descStatus2 & RxVlanTag) setVlanTag(rxPacketHead, OSSwapInt16(descStatus2 & 0xffff));`
（`LUCY:1204-1205`）。

### 4.4 低层 mbuf 标签助手（接口/协议栈一侧的机制）

`KDH/sys/kpi_mbuf.h`：

* `mbuf_set_vlan_tag(mbuf_t, u_int16_t vlan)`（`:1116`）—— **接口**用它标注"这包属于哪个 VLAN"；
* `mbuf_get_vlan_tag(mbuf_t, u_int16_t * vlan)`（`:1131`）—— 驱动读标签用；
  **没有标签时返回 `ENXIO`**（`:1120-1130` 的 `@result`），这是与"标签值为 0"的唯一区分手段；
* `mbuf_clear_vlan_tag(mbuf_t)`（`:1141`）。

（`KDH` 的 `sys/mbuf.h` 只是壳，声明都在 `kpi_mbuf.h` —— `docs/10B-MBUF-CONTRACT.md` 已记过这一点。）

### 4.5 `IOEthernetController` 还需要什么特别的？

**不需要。** 头文件里没有 `setVLANSupport()`、`enableVLAN()` 之类的额外开关：
VLAN 的全部接口就是 `getFeatures()` 的一个位 + `getVlanTagDemand()`/`setVlanTag()` 这一对
（特性位枚举在 `KDH/IOKit/network/IONetworkController.h:227-236`；
`getVlanTagDemand`/`setVlanTag` 在 `IOEthernetController.h:563,578`）。
每个 VLAN 子接口（`vlan0` 等）由家族自己派生 —— 头文件里也没有让驱动自己建 vlan 接口的 API，
这一点与 `IOEthernetInterface` 的现有分工一致（`KDH/IOKit/network/IOEthernetInterface.h:86`）。

### 4.6 本机可调用性

`getVlanTagDemand` 与 `setVlanTag` 在 `KDH` 与 `MKS` **都有**（`MKS/.../IOEthernetController.h` 同位置）。
**零 `MKS`-only 依赖。**

---

## 5. 巨帧（JUMBO）

### 5.1 广播能力上限：`getMaxPacketSize`

`virtual IOReturn getMaxPacketSize(UInt32 * maxSize) const`：在 `IOEthernetController` 里
（`KDH/IOKit/network/IOEthernetController.h:393`，`APPLE_KEXT_OVERRIDE`），基类声明是**纯虚**
（`KDH/IOKit/network/IONetworkController.h:684`：`= 0`）。配对的有
`getMinPacketSize(UInt32 * minSize) const`（`KDH/IOEthernetController.h:402`；基类纯虚 `:692`）。
参考驱动返回 `kMaxPacketSize = kMaxMtu(9000) + ETH_HLEN + VLAN_HLEN + ETH_FCS_LEN`
（`LUCY/LucyRTL8125Ethernet.hpp:233-234`；实现在 `LUCY:1011-1022`）。
框架常量：`kIOEthernetMaxPacketSize 1518`、`kIOEthernetMinPacketSize 64`、`kIOEthernetCRCSize 4`、
`kIOEthernetAddressSize 6`（`KDH/IOEthernetController.h:45,51,57,39`）。

### 5.2 谁改 MTU

**路线 A（正常路线，家族驱动）**：MTU 从 `ifconfig` 来 → 家族调到
`virtual IOReturn setMaxPacketSize(UInt32 maxSize)`（`KDH/IONetworkController.h:676`）。
依据：`IONetworkInterface` 里有 `SInt32 syncSIOCSIFMTU(IONetworkController * ctr, struct ifreq * ifr)`
（`KDH/IOKit/network/IONetworkInterface.h:323`），它就是把 SIOCSIFMTU 同步给控制器的那条私有路径。
**⇒ 驱动要覆盖的是控制器上的 `setMaxPacketSize`，不是接口上的 MTU setter。**

**路线 B（驱动主动设）**：`virtual bool setMaxTransferUnit(UInt32 mtu)`
（`KDH/IONetworkInterface.h:633`），文档（`:625-632`）说：**接口已 attach 时它内部调 `ifnet_set_mtu`**。
底层是 `extern errno_t ifnet_set_mtu(ifnet_t interface, u_int32_t mtu)`（`KDH/net/kpi_interface.h:1346`；
读取用 `extern u_int32_t ifnet_mtu(ifnet_t interface)` `:1354`）。
`getMaxTransferUnit()`（`KDH/IONetworkInterface.h:507`）文档（`:501-506`）明说它**调 `ifnet_mtu` 并返回**。
⇒ **两条路都存在**；惯例是 A（让家族把 MTU 变化转成 `setMaxPacketSize`），B 用于驱动自己
在 attach 前定基线。

### 5.3 约束

* `getPacketBufferConstraints(IOPacketBufferConstraints * constraints) const`
  （`KDH/IONetworkController.h:807`）告诉家族"我按 `allocatePacket()` 拿到的 mbuf 需要什么对齐"；
  结构体 `IOPacketBufferConstraints { UInt32 alignStart; UInt32 alignLength; UInt32 reserved[6]; }`
  （`:251-262`）。文档（`:796-801`）补了一句重要的边界：**"outbound packets, those that originate from
  the network stack, are not currently subject to the constraints reported here."**
  ⇒ 改 MTU 后如果要重算缓冲，**接收侧**受这份约束保护、**发送侧不受**。
  参考驱动两份对齐都是 1（`LUCY:590-597`）。
* 若驱动能吃"虚拟连续、物理跨页"的簇，要置 `kIONetworkFeatureMultiPages`（`KDH:230`）
  / 对应 `IFNET_MULTIPAGES`（`KDH/net/kpi_interface.h:202`）—— 这在巨帧下尤其相关，因为
  `allocatePacket()` 在"尺寸 + 对齐 > MCLBYTES"时必然返回链（`KDH/IONetworkController.h:815-818`
  的 `@discussion`）。参考驱动置了它（`LUCY:837`）。
* TSO 的分段上限另有一个 setter：`ifnet_set_tso_mtu`（`KDH/net/kpi_interface.h:1096`、
  读 `ifnet_get_tso_mtu` `:1111`）；文档（`:167-184`）说明 TSO 激活时**所有包**都会被标记，
  不只是大包。
* 链路层头长可用 `ifnet_set_hdrlen`（`KDH/net/kpi_interface.h:1396`）；
  以太网接口的 ifnet 参数由 `IONetworkInterface::initIfnetParams(struct ifnet_init_params *)`
  （`KDH/IOKit/network/IONetworkInterface.h:991`；`IOEthernetInterface` 的覆盖在
  `KDH/IOKit/network/IOEthernetInterface.h:290`）落下去 —— 这是家族与 ifnet 参数的交汇点。

### 5.4 MTU 变化时缓冲要怎么办

**家族不会替驱动改缓冲。** 依据：`KDH/IOKit/network/` 里没有任何"随 MTU 调整缓冲"的 API，
`allocatePacket()` 只保证"≥ 请求尺寸 + 对齐"（`KDH:812-824` / 声明 `:825`），而 vtable 上"改尺寸"的唯一入口
就是 `setMaxPacketSize()`（`:676`）。

参考驱动的做法（可作模板）：`setMaxPacketSize()` 里

1. 换算 `mtu = maxSize - (ETH_HLEN + VLAN_HLEN)`（`LUCY:1036`）；
2. 按新 MTU 与 `MSS_MAX` 的关系**增删** `IFNET_TSO_IPV4/IPV6` 位，并
   `offload = ifnet_offload(ifnet); offload ±= mask; ifnet_set_offload(ifnet, offload)`
   （`LUCY:1041-1055`）—— 即**用巨帧换取/归还 TSO**；
3. **强制重新初始化**：`setLinkDown(); timerSource->cancelTimeout(); restartRTL8125();`
   （`LUCY:1056-1058`）—— 缓冲/描述符环在这一步重建。

⇒ 本项目要做的对应动作：改 MTU ⇒ 重算 RX 簇尺寸与描述符环 ⇒ 重新发布/重启链路。
**注意 Lucy 是"先断链再重启"**，因为它在 `setMaxPacketSize` 里不能安全地就地改环。

### 5.5 本机可调用性

`getMaxPacketSize`/`getMinPacketSize`/`setMaxPacketSize`/`getPacketBufferConstraints`/
`getMaxTransferUnit`/`setMaxTransferUnit`/`ifnet_set_mtu`/`ifnet_mtu`/`ifnet_set_offload`/
`ifnet_set_tso_mtu` 在 `KDH` 与 `MKS` **都有**。**零 `MKS`-only 依赖。**

---

### 5.5 落地状态（0.44.23）：栈侧两条通路已实现，缺的是"把上限抬起来"的链改动

本节 §5.1/§5.2 说的两个覆盖点**已经实现**（`src/Cx3Mlx4.cpp`，`tools/test-mtu-conventions.c`
用 13 条断言钉住三个约定，接在 `build.sh` 里）：

* `getMaxPacketSize` / `getMinPacketSize` / `setMaxPacketSize` 三个都覆盖了；
* 答案**由三个真实边界取最小**得到，不是写死的常量：端口（`m3portup` 写进 SET_PORT 的
  线上尺寸，1500 MTU 时 1522）、接收环（`r4rq` 的每缓冲字节数，CQE 的 byte count **含 FCS**）、
  发送环（`outputPacket` 自己的界）。默认几何下最小值是端口的 1514，加上 FCS = **1518** ——
  与 `kIOEthernetMaxPacketSize` 一致，但这是从本端三个边界算出来的，不是抄来的；
* `setMaxPacketSize` 超过边界就**拒绝**（`kIOReturnUnsupported`）并在日志里写清三个边界各是多少、
  以及"要更大就得先抬谁"。依据是 `IONetworkController.h:667-676` 的原文：它是**客户端请求**、
  基类答案本来就是 `kIOReturnUnsupported`，所以拒绝是一个答复而不是把 bring-up 弄坏；
  同一段还说它跑在 workloop 门内，所以实现里**不发命令、不拿锁**；
* 三个长度约定（栈帧 / 家族答案 / SET_PORT 线上尺寸）混用是这一路上最容易犯的错，
  已在 `src/mlx4_regs.h` 里以 `MLX4_ETH_HLEN` / `_VLAN_LEN` / `_FCS_LEN` 命名并把
  `mlx4_port.c:1306-1308` 的钳制式写在注释里。

**真正打开巨帧还缺什么（都是链/设备侧，不是这张回调）**：把 MTU 抬上去要同时抬两处 ——
`m3portup` 的 mtu 输入（**线上尺寸**，jumbo 9000 对应 9022）和 `r4rq` 的每缓冲字节数
（≥ 9022）；两者都抬起来之后，`getMaxPacketSize` 会自动回答更大的值，`setMaxPacketSize`
就会接受。端口的能力上限是 `QUERY_PORT` 的 `eth_mtu`（本卡 9900，即线上 9922）。

## 6. 多队列 / RSS

### 6.1 结论先讲

**这一代 IONetworkingFamily 从 kext 侧没有可用的多硬件发送/接收队列或 RSS 接口。**
能拿到的只有"一个控制器一个输出队列"，以及一条**和 RSS 无关**的 AVB/TSN 实时队列面。

### 6.2 证据

**(a) 输出队列只有一个。** `virtual IOOutputQueue * createOutputQueue()`
（`KDH/IOKit/network/IONetworkController.h:1343`）文档（`:1330-1342`）：**默认实现永远返回 0**
（= 不建队列），驱动可以返回 `IOOutputQueue` 的子类；**控制器只保存这一个引用**，
由 `virtual IOOutputQueue * getOutputQueue() const`（`:792`）取回。
`IOOutputQueue` 本身是抽象基类（`KDH/IOKit/network/IOOutputQueue.h:98`），
具体的 `IOBasicOutputQueue`（`KDH/IOKit/network/IOBasicOutputQueue.h`）提供
`withTarget(...)`（`:144,157,172,188`）、`enqueue`（`:206`）、`setCapacity/getCapacity/getSize`
（`:269,278,285`）—— 全都是**单队列**语义。参考驱动就用一个固定容量队列：
`createOutputQueue()` 返回 `IOBasicOutputQueue::withTarget(this)`（`LUCY:600-606`），
在 `initEventSources()` 里 `getOutputQueue()` 取回并 `retain()`（`LUCY/LucyRTL8125Setup.cpp:214-219`）。

**(b) 系统头里没有 RSS / 多 RX 通道 KPI。** 逐项核对 `KDH/IOKit/network/*` 与
`KDH/net/kpi_interface.h`：没有 `rss`、没有 channel/bucket 队列 API
（`kpi_interface.h` 里唯一含 `channel`/`rss` 的是路由/QoS 面与 WiFi RSSI，
不是硬件队列分配）。全 `KDH` 里 `rss`/`RSS` 只出现在
`net/route_private.h`、`net/if_private.h`、`net/if_var_status.h` —— 那是
`IFRTYPE_QOSMARKING_*` 的 **socket/channel** 标记（`net/if_private.h:236-238`）与
`quality_score_channel`/RSSI 之类的**无线指标**（`net/if_var_status.h:402,434`），
与"把 TCP 流散列到多个硬件 RX 队列"无关。

**(c) 真正支持多队列的那条路（Skywalk / tnet）不在系统头里。** `KDH` 下**没有 `skywalk/` 目录**
（`ls -d .../Headers/skywalk` → `No such file or directory`），而 `MKS` 下有整套
`skywalk/{channel,core,lib,mem,namespace,nexus}` 与 `os_skywalk*.h`，
并且 `MKS` 独占一个头 `IOSkywalkEthernetInterface.h`（`MKS/IOKit/network/`，
`KDH/IOKit/network/` 里没有这个文件）。⇒ **从本项目对 `KDH` 编译的 kext 里，Skywalk 通道 API 根本不可见**，
而 Skywalk 正是 macOS 上"每队列一个接口"的那套机制。

**(d) 头里最强的"多队列"面是 AVB/TSN，不是 RSS。** `IOEthernetController` 在 `KDH` 里确实有
多发送队列的接口：`transmitRealtimePackets(uint32_t queueIndex, IOEthernetAVBPacket **packets,
uint32_t packetCount, bool commonTimestamp, uint32_t * successfulPacketCount)`
（`KDH/IOKit/network/IOEthernetController.h:822`，reserved slot 3 紧随其后 `:823`）、
`setNumberOfRealtimeTransmitQueues(uint32_t)`（`:845`）、
接收侧 `setNumberOfRealtimeReceiveQueues(uint32_t)`（`:1093`）、
以及 `setRealtimeReceiveQueueFilter` 等（`:1044,1057,1070,1082`）。
但这一整套是 **AVB/TSN 的实时队列**：入口要先报能力
`bool getAVBSupport(IOEthernetControllerAVBSupport *avbSupport) const`（`:615`），
包类型是 `IOEthernetAVBPacket`（带 launch timestamp），
要 `setAVBPacketMapper`（`:635`）/`setAVBControllerState`（`:699`）/时间同步回调
（`:1105-1134`）一整套配合，且由家族按 gPTP/AVB 状态机驱动。
**把它当"多队列"用会偏离 AVB 语义**（`:730-731` 明确 `vlanTag` 字段是给接收路径带剥离的 VLAN 信息的）。

### 6.3 一句话给本项目

M5 阶段**放弃"多队列/RSS"这一项**，只保留：
（i）单输出队列（`createOutputQueue` / `getOutputQueue`，`KDH:1343` / `KDH:792`）；
（ii）输入侧若要用"驱动主动交给接口"的形状，`inputPacket()`/`flushInputQueue()`/`clearInputQueue()`
（`KDH/IOKit/network/IONetworkInterface.h:418,442,451`）—— 这些是 `KDH` 里的；
（iii）**不要**按 `MKS` 的 pull model 去写（见 §8 表 A：那批方法 `KDH` 里一个都没有）。

---

## 7. 中断节流（INTERRUPT MODERATION）

### 7.1 macOS 给驱动暴露了什么

**没有任何中断合并/节流 API。** 证据：

* `KDH/IOKit/network/` 全域没有 coalesce/moderate/rate-limit 之类的接口；
* 全 `Kernel.framework/Headers` 里 `coalesc` 在 `IOKit/network/` 下只命中两个文件：
  `IONetworkController.h` 的两处**文档注释**（`:210`、`:480`）与
  `IOMbufMemoryCursor.h`，讲的都是 `getPhysicalSegmentsWithCoalesce`
  （mbuf 物理段合并），**与中断无关**；
* IOKit 的中断接口本身也没有节流参数：`IOInterruptEventSource::init(OSObject * owner,
  Action action, IOService * provider = NULL, int intIndex = 0)`
  （`KDH/IOKit/IOInterruptEventSource.h:170-173`）与工厂
  `interruptEventSource(owner, action, provider, intIndex)`（`:139-143`）**都没有延迟/上限参数**；
  `IOFilterInterruptEventSource`（`KDH/IOKit/IOFilterInterruptEventSource.h`）给的是
  "过滤 + 决定要不要唤醒 workloop"的能力，也不是节流。

⇒ **中断节流是驱动自己的选择**：要么在硬件里配合并寄存器（对 CX3 就是 `EQ`/`CQ` 的
moderation 参数），要么在软件上"取够 N 个包或超时再处理"。

### 7.2 家族对中断延迟/合并有没有期望

**头文件里没有可引用的延迟要求**（"not determinable from these sources"）。能确定的相关事实只有三条：

1. **中断回调的串行化契约**：`IOInterruptEventSource` 的类文档（`KDH/IOKit/IOInterruptEventSource.h:50-58`）
   说明它把中断例程投递到客户端的**单线程**上下文，并且对**电平触发**中断会
   自动在首次中断时关源、回调返回后重新打开（`:56`，原文称 "This action is fairly expensive
   but it is 100% safe"）。⇒ 每次中断的**软件开销不小**，这是"少打断"的现实理由，
   但它是实现说明，不是强制指标。
2. **家族的输出侧有一个可配置的"启动延迟"**，这是最接近"批处理"的官方旋钮：
   `IOReturn configureOutputStartDelay(uint16_t outputStartDelayQueueLength,
   uint16_t outputStartDelayTimeout)`（`KDH/IOKit/network/IONetworkInterface.h:1006`），
   文档（`:995-1005`）说明它映射到 `ifnet_init_eparams.start_delay_qlen` 与
   `.start_delay_timeout`，且**必须在 `init()` 之后、`attachToDataLinkLayer()` 之前**调用。
   **这是发送调度，不是中断合并。**
3. **接收侧如果自己攒包，家族在 `MKS` 提供了一个"轮询"模型**（`setInputPacketPollingEnable`/
   `pollInputPackets`），但**那是 `MKS`-only**（§8 表 A）⇒ 对 `KDH` 构建不可用。
   参考驱动用它的位置：`LUCY:1418`、`LUCY:1441`，以及 `netif->setPacketPollingParameters(&pParams, 0)`
   （`LUCY:1613`，`IONetworkPacketPollingParameters` 结构体本身也是 `MKS`-only）。

### 7.3 参考驱动怎么做

* 用 `IOTimerEventSource` 做 1 Hz 的统计/看门狗（`LUCY.hpp:249`，`LUCY:1672` 上弦），
  统计用 `thread_call_enter_delayed(statCall, statDelay)` 异步做（`LUCY:1668`，
  `statDelay = 1ms` @ `LUCY.hpp:237`）；
* 中断处理里读完中断状态后**重写 IMR 掩码**而不是依赖家族（`LUCY:1358-1377`）；
* **没有任何中断合并寄存器配置** —— 即参考驱动也没做节流。本项目要做的就是自己决定
  CX3 的 EQ/CQ moderation 值，并在文档里如实记录"macOS 不参与"。

---

## 8. 系统头 vs MacKernelSDK：逐名总表

判定方法：对每个符号在 `KDH`（`Kernel.framework/Headers` 全树）与 `MKS`（`Headers` 全树）
各做一次带词边界的全树 `grep`，结果只有"在/不在"两种。下表是本次实测结果。

### 表 A —— **只在 MacKernelSDK 里**（`KDH` 无声明）⇒ 本项目**不得**调用或覆盖

| 符号 | 所属类 | 在 `MKS` 的位置 | 备注 |
| --- | --- | --- | --- |
| `configureOutputPullModel` | `IONetworkInterface` | `MKS/IOKit/network/IONetworkInterface.h`（`#ifdef __PRIVATE_SPI__` 块 `:1120-1524`） | pull model 的配置入口 |
| `startOutputThread` | `IONetworkInterface` | 同上块 | 启动输出线程 |
| `stopOutputThread` | `IONetworkInterface` | 同 | |
| `flushOutputQueue` | `IONetworkInterface` | 同 | |
| `signalOutputThread` | `IONetworkInterface` | 同 | |
| `dequeueOutputPackets` | `IONetworkInterface` / `IONetworkController` | 同 | pull model 取包 |
| `enqueueInputPacket` | `IONetworkInterface` / `IONetworkController` | 同 | 输入轮询队列 |
| `setPacketPollingParameters` | `IONetworkInterface` | 同 | |
| `setInterfaceSubType` | `IONetworkInterface` | 同 | |
| `isBPFTapEnabled` / `getLoggingLevel` | `IONetworkInterface` | 同 | |
| `IONetworkPacketPollingParameters`（结构体） | — | `MKS/.../IONetworkInterface.h` | |
| `outputStart` | `IONetworkController` | `MKS/.../IONetworkController.h:1596`，随后 `OSMetaClassDeclareReservedUsed(IONetworkController, 2)` `:1600` | **reserved slot 2** |
| `setInputPacketPollingEnable` | `IONetworkController` | `:1616` + Used(3) `:1620` | slot 3 |
| `pollInputPackets` | `IONetworkController` | `:1643` + Used(4) `:1649` | slot 4 |
| `networkInterfaceNotification` | `IONetworkController` | `:1667` + Used(5) `:1672`（`#if __MAC_OS_X_VERSION_MIN_REQUIRED >= __MAC_10_9`） | slot 5 |
| `getMbufServiceClass` / `attachAuxiliaryDataToPacket` / `removeAuxiliaryDataFromPacket` | `IONetworkController` | 同在 `#ifdef __PRIVATE_SPI__` 块内 | |
| `IOSkywalkEthernetInterface.h` 及整套 `skywalk/` | — | `MKS/IOKit/network/IOSkywalkEthernetInterface.h`、`MKS/skywalk/` | `KDH` 无 `skywalk/` 目录 |

关键结构性事实（本节的核心）：

* `MKS` 把这批方法**整体放在 `#ifdef __PRIVATE_SPI__` 之内**
  （`MKS/.../IONetworkController.h:1518` … `#else` `:1680` … `#endif` `:1685`；
  `MKS/.../IONetworkInterface.h:1120` … `:1524`）。
  **不定义 `__PRIVATE_SPI__` 时，`MKS` 自己就把 slots 2..5（控制器）与 5..10（接口）
  声明为 `Unused`**（`MKS:1681-1684` / `MKS:1525-1536`）。
* `KDH` **根本没有 `__PRIVATE_SPI__` 这个开关**，那两个区间一律是
  `OSMetaClassDeclareReservedUnused(...)`（`KDH/IOKit/network/IONetworkController.h:1501-1504`；
  `KDH/IOKit/network/IONetworkInterface.h:1010-1020`）。
* 参考驱动是"**定义 `__PRIVATE_SPI__` + 用自带 `MKS`**"这一路
  （`LUCY/LucyRTL8125Ethernet-Prefix.pch:15`、`PBX:3719-3720`），
  因此它**看得见** `outputStart`/`configureOutputPullModel`/`dequeueOutputPackets`/
  `enqueueInputPacket`/`setInputPacketPollingEnable`/`pollInputPackets`，
  并真的用了（`LUCY:442,467,664,1208,1418,1441`）。
* `PLAN:170-205` 量到：本机内核**不导出** `_RESERVEDIONetworkController2..5`，
  即那几个槽在内核侧是真实方法。**这是 pull model 属于"内核有、头没写"的直接证据**；
  但要注意该测量只覆盖**控制器**的 2..5，**没有**覆盖接口的 5..10 ——
  见 §9 第 2 条。

### 表 B —— **只在系统头里**（`MKS` 无声明）⇒ 本项目可以用，参考驱动看不到

| 符号 | 所属类 | 在 `KDH` 的位置 |
| --- | --- | --- |
| `setHardwareAssists(UInt32, UInt32)` | `IONetworkController` | `KDH/IOKit/network/IONetworkController.h:1536`（后随 `OSMetaClassDeclareReservedUsed(..., 7)` `:1538`） |
| `allocatePacketNoWait(UInt32 size)` | `IONetworkController` | `KDH:1519`（后随 `Used(..., 6)` `:1521`） |
| `IONetworkMedium.h` 的 "Important Note"（`kIOMediumEthernet*` 只是 `IFM_*` 别名、不再扩展） | — | `KDH/IOKit/network/IONetworkMedium.h:85-93` |
| `IOEthernetAVBPacketPoolEntry` / `createRealtimeAVBPacketPool` / `realtimePoolAVBPacketCompletion` | `IOEthernetController` | `KDH/IOKit/network/IOEthernetController.h:196-202,783,787` |

> `setHardwareAssists` 值得注意：它是**发送侧多队列/硬件协助**的协商口（"allow the Controller to be
notified of a Hardware acceleration configuration change"，`KDH:1523-1537`）。
它**只在 `KDH` 里有**，所以本项目能覆盖它、参考驱动不能 —— 但它是 `Used(...,7)` 的真实槽位，
覆盖前要确认半双工/多队列语义（同一区域的 `kIONetworkFeatureTransmitCompletionStatus` 见 §3.2）。

### 表 C —— **两套都有**（本节各功能的其余 API）

`publishMediumDictionary`、`setSelectedMedium`、`getSelectedMedium`、`selectMedium`、
`selectMediumWithName`、`getMediumDictionary`、`setLinkStatus`（`KDH` 在 `IONetworkController.h`；
`MKS` 另外还在 `IONetworkInterface.h` 有一份同名者 —— 这是**另一处集合差异**，
`KDH` 的 `IONetworkInterface` 没有）、`getChecksumSupport`、`setChecksumResult`、`getChecksumDemand`、
`createOutputQueue`、`getOutputQueue`、`getPacketBufferConstraints`、`getMaxTransferUnit`、
`setMaxTransferUnit`、`inputPacket`、`flushInputQueue`、`clearInputQueue`、`configureOutputStartDelay`、
`getIfnet`、`getVlanTagDemand`、`setVlanTag`、`ifnet_set_offload`、`ifnet_set_mtu`、
`mbuf_set_csum_performed`、`mbuf_get_csum_requested`、`mbuf_set_vlan_tag`。

---

## 9. 不能从这些来源判定的（及各自能定案的办法）

1. **"无介质字典时 `ifconfig` 到底打印什么"** —— 头文件不描述用户态渲染。
   *定案*：A/B 两次真机 `ifconfig en<N>`（一次跳过 `publishMediumDictionary`）。
2. **运行内核里 `IONetworkInterface` 的 reserved slots 5..10 是不是真实方法**
   （即 `configureOutputPullModel`/`startOutputThread`/`dequeueOutputPackets` 那批在**接口**侧
   是否可用）。`PLAN:186-205` 的证据只覆盖**控制器**的 2..5。
   *定案*：拿与 15.8.1 同版的 KDK 头比对这两段；或对一个**定义 `__PRIVATE_SPI__`、引用
   `configureOutputPullModel`** 的构建跑一次符号账目（`BUILD` 里那套 `nm -u` + `kmutil libraries`）。
   现状的旁证（推理，非测量）：参考驱动确实在工作的前提下调用这批方法（`LUCY:664,1616`），
   若那些槽是填充，调用会静默无效。
3. **CX3 的 RSS/多队列在 macOS 下能否成立** —— 头文件层面没有可用 API（§6），
   所以"能不能"不是头文件能回答的问题。*定案*：先确认是否存在能承载多队列的受支持接口
   （Skywalk 不可见），否则应按"不做"归档。
4. **家族在 MTU 变化时对 `setMaxPacketSize` 与 `ifnet_set_mtu` 的调用次序**
   —— `KDH` 只暴露 `syncSIOCSIFMTU` 的存在（`IONetworkInterface.h:323`），不描述次序。
   *定案*：真机上 `ifconfig en<N> mtu 9000` 时打点。
5. **中断延迟/合并的"家族期望值"** —— `KDH` 中没有可引用的数值或约束（§7.2）。

---

## 10. 落地顺序建议

1. **先做媒体与链路（§2）。** 理由：本节用到的 API 100% 在 `KDH` 里（零 vtable 风险）、
   不碰 mbuf 与数据通路（与 `PLAN:167` 选 `netup` 的"风险最低"口径一致）、
   而且它是后面每一项的**可观测前提** —— 没有 `setLinkStatus` 的可信报告，
   "巨帧换上去了没有""链路是不是 1000M 全双工"都无从判断。
2. **再做接收侧校验和（§3.3）**，只做"报告结果"、不动发送插入，仍然不碰发送路径；
   落地时按 `setChecksumResult()` 的 `resultMask`/`validMask` 语义（`KDH:995`），
   而不是照抄 `LUCY:1474` 的 `value = 0xffff`。
3. **VLAN（§4）与巨帧（§5）可以并行**，两者都只用到 `KDH` 里的 API；
   巨帧的成本主要在"改 MTU 要重建描述符环"（§5.4）。
4. **多队列/RSS 与中断节流按"不做/自决"归档**：前者是 API 不存在（§6），
   后者是 macOS 不参与（§7），只需在代码注释里如实写明，并记录 CX3 侧自己设定的
   EQ/CQ moderation 值。

