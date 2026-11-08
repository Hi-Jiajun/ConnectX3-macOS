# M5：把"能用"变成"完整"——计划书

> **2026-10-22：历史计划，已被[全功能适配总计划](CX3-PRO-FULL-ADAPTATION-PLAN.md)替代。**
> 下文2026-10-13状态保留为过程记录，不代表当前状态：M4已在0.44.47真机通过；
> “多队列/RSS不可达、只有DriverKit可行”不再作为范围裁决；TSO不依赖jumbo，不能用大ping验TSO。
> 新目标包含原生网络与配套SDK开放的全部本卡支持能力，包括RDMA、VPI/IB、虚拟化等。
> 按[能力矩阵](CX3-PRO-CAPABILITY-MATRIX.md)与[任务依赖](CX3-PRO-BACKLOG.md)推进，
> 本文及旧接口研究中的冲突结论以新总计划明确的修正为准，旧实验命令不要不经当前门禁直接执行。

> 这份文件是**计划**，不是结果。它写清楚 M5 每一项：**要什么**、**怎么算通过**、**照谁抄**、
> **风险在哪**、**先后顺序**。依据来自三处：本项目自己的实测记录、参考实现
> （`docs/M5-REFERENCE.md`：Linux `mlx4_en` 源码，带 `file:line`）、以及本机 macOS 头文件与参考驱动
> （`docs/M5-MACOS.md`）。**两份参考资料读完再动手。**

## 0. 先决条件（M5 之前的门槛，写在这里免得被跳过）

1. **M4 必须先过**：`ping` 通 + `100 包 0 丢`（`docs/R4-STAGE10-PLAN.md` §6）。M5 的每一项都改
   数据通路或端口配置，在"数据通路本身还没验过"的树上动手，会让两个变量混在一起 —— 这正是
   `docs/R4-STAGE10-PLAN.md` §8 已经吃过一次的教训。
2. **一次只上一项，一项一验**。每一项都要有它自己的判据与 `cx3ctl netstat` 可读的计数。
3. **不引入第二个写入者**。整个驱动的纪律是"设备写入必须来自选择器或已开启的数据通路会话"；
   M5 的新功能要么走选择器，要么挂在 `enable()` 之后的数据通路上，不能另开一条无人持有的路径。
4. **硬约束：只能调用运行系统头里有的方法**（`docs/R4-STAGE10-PLAN.md` §10a、§12.5：两套头的
   vtable 不同，调用家族没有的槽 = 跳进错误的方法，不是编译错误）。`docs/M5-MACOS.md` 会点名哪些
   是只有 MacKernelSDK 才有的。
5. **★ 缺一条"停端口 → 重开端口"的路**（`docs/M5-REFERENCE.md` §0.5、§3.4）：Linux 改 MTU / 改环
   尺寸 / 改 VLAN 过滤表，都要 `stop_port` 之后重新走一遍端口初始化。本项目今天只有"从头建"这一种
   做法（`m3portup` 的三步序：`SET_PORT_general → qpn_calc → INIT_PORT`）——**这一条路要在做
   jumbo/VLAN/多队列之前先补上**，否则那些功能没有"改"的入口，只能靠复位重来。

## 1. 当前状态（2026-10-13，0.44.10 已装待启）

| 已做 | 证据 |
| --- | --- |
| 接口出现、`ifconfig up` 能成功、MAC 是卡的 | `kCx3SelNetUp` + 链条 10a 步骤（**未上机验证**） |
| 接收：定时器轮询 CQ → mbuf → 协议栈 | `netServiceRx`（**未上机验证**） |
| 发送：`outputPacket` 异步投递 + 完成回收 | `netServiceTx`（**未上机验证**） |
| 数据通路计数可读 | `cx3ctl netstat`（选择器 40） |
| 冻屏三件仪器 | `docs/HANG-AUDIT.md` §9 |
| M5-1 媒体表/链路重读（运行期开启） | 选择器 41 + `cx3ctl netlink`（**未上机验证**） |
| M5-3 校验和 RX 侧（运行期开关，默认关） | 选择器 42 + `cx3ctl netoff rxon|rxoff`（**未上机验证**） |
| M5-3 校验和 TX 侧（attach 前开，默认关） | `CX3_TXCSUM=1` + `cx3ctl netoff txon|txoff`（**未上机验证**） |
| M5-2 的前置实验（drain→arm→看事件） | 选择器 43 + 链条步骤 + `cx3ctl netarm`（**未上机验证**） |
| M5-8 组播过滤（运行期开关，默认关） | 选择器 44 + `cx3ctl netfilt mcaston`（**未上机验证**） |
| M5-8 混杂模式（运行期开关，默认关） | `cx3ctl netfilt promiscon`（**未上机验证**） |
| 停端口（jumbo/TSO/VLAN 的前置） | 选择器 45 + `cx3ctl netport down`（**未上机验证**） |
| TX 帧缓冲跟随 MTU（默认下 = 2048，不变） | `fNetTxBufBytes` + `netup` 的日志（**未上机验证**） |

**明确的限制**（不是缺陷，是还没做）：组播/混杂过滤只记录不写硬件（IPv6 会察觉）· 没有媒体表 ·
无校验和卸载 · 无 VLAN · 无 jumbo · 单队列 · 无中断调节 · 接收靠 `1 ms` 轮询而不是 CQ arm。

## 1.5 两份参考书查实的关键点（照着做，不要再重新调研）

`docs/M5-MACOS.md`（本机系统头 + 工作驱动逐条对照）与 `docs/M5-REFERENCE.md`（Linux `mlx4_en`）
已经把每项的**macOS 侧接口**与**硬件侧落点**都钉死了。要点：

| 项 | macOS 侧要做什么 | 硬件侧落点 |
| --- | --- | --- |
| 媒体/链路 | `publishMediumDictionary` → `setLinkStatus(status, activeMedium, speed)`；`selectMedium` 改完**必须回调** `setSelectedMedium`。**没有字典时 `getSelectedMedium()` 返 0**（现状） | `QUERY_PORT` 一条命令给全（state/speed/autoneg） |
| 校验和 | **只报能力**：`getChecksumSupport`（非 `kIOReturnSuccess` 一律当"不支持"）。**开启由家族做**（`ifnet_set_offload`）——`setChecksumSupport` **两套头都没有**。RX 交给栈用 `setChecksumResult`；TX 向栈要需求用 `getChecksumDemand` | RX：硬件无条件算，读 CQE 的 `status.IPOK` + `checksum==0xffff`；TX：逐 WQE 的 `srcrb_flags` bit4/5 |
| jumbo | 能力用 `getMaxPacketSize`，改 MTU 用 `setMaxPacketSize`；**家族不会替你改缓冲**（参考驱动是断链后重建环）。另注意 `getPacketBufferConstraints` **不约束发送侧** | 必须 `stop_port → start_port`；`rx_mb_size` 四档离散 cluster；RQ `byte_count = rx_mb_size - 2` |
| VLAN | `getFeatures()` 置 `kIONetworkFeatureHardwareVlan`；TX 用 `getVlanTagDemand`、RX 用 `setVlanTag` | RX 剥：QP `param3` bit30（**极性待实测**）；TX 插：WQE 的 `vlan_tag` + `ins_vlan`（帧里**不许**带标签）；另有 128 项 `SET_VLAN_FLTR` |
| 中断调节 | macOS **不暴露任何**合并/节流 API ⇒ 驱动自决（CX3 只有 CQ 有调节字段，EQ 没有） | `MODIFY_CQ` 的 `cq_max_count`/`cq_period` |
| 多队列/RSS | **见下面的范围裁定** | 需要新 QP/EQ 向量；TX CQ 复用 RX CQ 的向量 |

**不得调用的 API（只在 MacKernelSDK、且都在 `#ifdef __PRIVATE_SPI__` 里）**：
`outputStart` · `setInputPacketPollingEnable` · `pollInputPackets` · `networkInterfaceNotification`
（这四个正是控制器 **reserved slot 2..5**，本项目 §10a 实测解析不了的那几个）· `dequeueOutputPackets` ·
`configureOutputPullModel` · `startOutputThread`/`stopOutputThread`/`flushOutputQueue`/`signalOutputThread` ·
`setPacketPollingParameters`/`enqueueInputPacket` · 整套 `skywalk/`。
**反向**：`setHardwareAssists`、`allocatePacketNoWait` 只在系统头里有 ⇒ 可用（参考驱动看不到它们）。

### ★ 范围裁定：多队列 / RSS 不在可达范围内（M5-6）

`docs/M5-MACOS.md` §5 的结论是**这一代从 kext 侧没有可用接口**：只有单个
`createOutputQueue`/`getOutputQueue`；系统头里没有 RSS/channel KPI；真正多队列的 Skywalk **不在
`KDH`**（只有 `MKS` 有，且被 `__PRIVATE_SPI__` 挡住）；头里唯一的多队列面是 AVB/TSN 实时队列。

⇒ **这一项从 M5 范围里去掉**，理由如上（不是"暂时不做"，是"这条路上没有入口"）。
如果将来确实需要多队列，**唯一的路是把驱动改成 DriverKit dext**（用户态驱动，正是 Apple 自带
`AppleEthernetMLX5.dext` 的做法）——那是另一次架构决策，不在本计划内。

## 2. M5 各项：判据 / 参照 / 风险 / 顺序

每一项的"通过"都写成**一条能被别人复现的命令或读数**，不写形容词。

### M5-1 媒体表与链路监视 —— **代码已就位（0.44.11），待上机验证**

* **要什么**：`ifconfig en<N> media` 有内容；`getSelectedMedium()` 不再返回 0；链路变化能被重新
  读到并上报（不是只在 `netup` 那一刻发布一次）。
* **已实现**：选择器 41 `kCx3SelNetLink` + `cx3ctl netlink [report|publish|requery]`：
  * `publish` —— 造一个**只有一项**（`autoselect`）的媒体字典并发布、再 `setSelectedMedium()`；
  * `requery` —— 发一次 `QUERY_PORT`，**只在读数变化时**才重新 `setLinkStatus`（家族要的是
    "变化时调用"，不是每次都调）；
  * `report` —— 只读：发布了几项、选中的是不是我们的对象、链路现在是什么。
* **为什么用"模式开关"而不是无条件发布**：发布字典会改变 M4 验收依赖的那条链路上报路径，而 M4
  还没上机验过。做成运行期开关 ⇒ **M4 判据跑的还是原样行为**，等 M4 过了再 `cx3ctl netlink publish`
  当场验证 M5-1，**不需要再重启一次**。
* **为什么字典只有一项**：列一个 `10GBase-SR/CR4/T` 的清单等于宣称 PHY 种类，而本项目手上的资料
  **没有** `QUERY_PORT` 的 transceiver 类型到这些名字的映射（`docs/M5-MACOS.md` §2、`docs/M5-REFERENCE.md` §6）。
  真实速率不靠媒体表表达——它由 `setLinkStatus` 的 `speed` 携带，而那是**实测**的。
* **明确的限制**：链路**不是**周期性监视的。macOS 不要求（家族只在 `setLinkStatus` 被调时动作），
  而本驱动**不能在 workloop 里查链路**——查链路要发固件命令，从持有工作环门的上下文发命令会与
  "持命令信号量、等工作环投递的选择器"互等（与数据通路用 TryLock 避开的是同一个形状）。
  所以重读是**按需**的（`netlink requery`）。真正的链路 watch（内核线程腿）留作后续项。
* **判据**：重启后 ① `sudo cx3ctl netlink publish` → `ifconfig en<N>` 出现 `media:` 行；
  ② `sudo cx3ctl netlink requery` 读数与 `queryport` 一致；③ 拔线 → `requery` 报 CHANGED 且
  `ifconfig` 的 `status` 变 `inactive`；插回 → 变回 `active`。
* **风险**：低（新增对象与一次可选发布；`publishLinkStatus` 只在已发布时才把 medium 传进去）。

### M5-2 中断驱动的接收（把轮询换成 CQ arm）—— **前置实验已实现（0.44.14）**

* **前置实验（选择器 43 `kCx3SelNetArm` / `cx3ctl netarm [waitMs]`）**：这一段是整件事的钥匙。
  阶段 9 测过 arm 并得到**零 EQE**，但那个实验**本身不成立**（arm 时队列非空，而 mlx4 的 arm
  语义是"之后有新完成才通知"）⇒ 那条链**两个方向都没有读数**。这个选择器就是那个读数：
  **先把 RX CQ 排空**（消费完成并补槽，和数据通路做的一样）→ 再 arm → 再看事件队列。
  它有三个拒绝码（CQ/EQ 不存在、数据通路已启用、中断会话还开着——后者会抢走它等的那个 EQE），
  四个结果各自有明确措辞，而且**先把前提（队列是否真空）打出来再给结论**。
* **已接进链条**（位置在 10a 之前，那里三个前提刚好都成立），判据：
  * `eqeOk=1` ⇒ PASS（**这条链第一次被证明**，中断接收才可做）；
  * `cqEmpty=0` ⇒ BLOCKED（前提没满足，这一跑什么也不能说明——正是阶段 9 那次的问题）；
  * 队列空且没事件 ⇒ FAIL（真读数：这条路上 arm 不产生事件）。
* **要什么**：接收由 CQ arm → COMP EQE → EQ 中断驱动，轮询退化为兜底。
* **判据**：`netstat` 的 `ticks` 不再增长（或大幅下降）而 `rxFrames` 照常增长；EQ 侧能看到
  COMP 事件的消费计数。
* **前置**：**必须先做那个最小实验**——先把 RX CQ 排空、再 arm、看 COMP EQE 到不到
  （`docs/10B-PRIMITIVES.md` §1.6：本机实测"零 EQE"，但当时的测试本身不成立，因为 arm 时 CQ 非空）。
* **同时必须改**：`consumeEqLocked` 现在的 type-0（COMP）规则是"看到就不消费、计数、break"
  （因为前九个阶段没有 CQ 可服务）。有 CQ 在跑之后，这条规则会让 EQ **永久卡在第一个 CQ 事件**上
  （不消费 ⇒ 不 re-arm ⇒ 连命令完成都收不到）⇒ 必须与接收投递**一起**改。
* **风险**：高（改的是 EQ 消费与中断路径）。所以排在数据通路稳定之后。
* **实验通过之后的具体改法（写下来，届时机械执行）**：
  1. `consumeEqLocked`：type-0 事件不再"计数后 break"，而是**消费掉并交给 CQ 服务**——
     即把 `++fIrqErrors; break;` 换成"消费 + 记一次 CQ 事件 + 继续"；
  2. 中断 action 在消费完 EQ 之后调用 `netServiceRx()`（与 tick 里同一个函数，锁纪律不变：
     action 持门，服务路径取事务锁用 TryLock）；
  3. `netTick` 的轮询**降级为兜底**：仍是 1 ms，但只在"上一次 arm 之后超过 N ms 没有 COMP 事件"
     时才主动排空（否则就是中断 + 兜底两条腿，和参考驱动一样）；
  4. 每次排空后重新 arm（`cqSetArmDb` + `cqRingDoorbell`，索引 = 排空后的消费者）；
  5. `netstat` 增加"COMP 事件数"与"兜底排空次数"两个计数，判据是
     **帧数照常增长而兜底次数趋近于 0**。
* **如果实验给出"队列空、arm 了、没事件"**：那就**不做中断接收**（这个路径的证据在那一侧），
  把轮询留在原地并在本文写明"为什么它是轮询"，而不是继续投入——这也是"先测量再建"的用处。
* **顺带纠正一处**：中断调节**只在 CQ 上**（`MODIFY_CQ` 的 `cq_max_count`/`cq_period`，默认
  RX 44/16、TX 64/64），**本树的 EQ 根本没有调节字段**（`docs/M5-REFERENCE.md` §5、附录 B）。
  而且 TX 侧的完成回收在 Linux 里是**定时器轮询**、不是 arm —— 与本项目现在的做法相同。

### M5-3 校验和卸载 —— **RX 与 TX 两侧代码已就位（0.44.12 / 0.44.13），待上机验证**

* **要什么**：RX 把设备的校验和结论交给栈；TX 对 IPv4/IPv6 TCP/UDP 请求硬件计算。
* **RX 已实现**（选择器 42 `kCx3SelNetOff` + `cx3ctl netoff [report|rxon|rxoff]`，
  **默认关闭**）：判据就是参考驱动那个合取式 —— `status.IPOK` 且 `checksum == 0xffff`
  （`mlx4_en_rx.c:810-818`，0xffff 是"已验证"的哨兵而非校验和本身）；成立且状态字点名了
  IPv4/IPv6 与 TCP/UDP 时，`setChecksumResult(m, kChecksumFamilyTCPIP, mask, mask)`。
  **没有任何设备访问**——硬件本来就算，驱动只是解释。
  `getChecksumSupport()` 只报 **RX** 能力（IP/TCP/UDP + v6 两位），**输出路径答"不支持"**
  （发出去的 WQE 要加新位，等 M4 验过再动）。
* **判据**：`cx3ctl netoff rxon` 后 `ping`/`iperf` 照常；`netstat` 的 `rxFrames` 与
  `netoff` 的 `rxGood` 同步增长；`rxoff` 后一切不退化（栈自己算）。
* **为什么是开关**：逐帧的"已验证"标记若判错，比不标记更糟（错误数据静默上栈 / 好帧被丢），
  而 M4 还没上机验过 ⇒ 默认关，等 M4 过了再 `netoff rxon` 当场验证，**不用再重启**。
* **TX 已实现**（同一选择器的 `txon`/`txoff`，**默认关**）：`getChecksumSupport(isOutput=true)`
  只在开关打开时才报能力（IP + TCP/UDP，v4/v6）；每帧用 `getChecksumDemand()` 读栈的请求，
  映射成 WQE 的 `srcrb_flags` 两个位（`MLX4_WQE_CTRL_IP_CSUM` 1<<4 / `TCP_UDP_CSUM` 1<<5，
  `mlx4_en_tx.c:695-702`）。**伪头那两位（IIP/ILP）刻意不设**——参考实现全树从不设，
  这条路的模型是"栈把伪头值写进帧、硬件补完"。
  `sqWriteSendWqe()` 新增一个 **默认 0** 的 `csumFlags` 形参 ⇒ r4raw 与 txprobe 写出的描述符
  **逐字节不变**（阶段 9 已证明的那两处）。
* **★ TX 有一个必须说清的约束**：能力是**家族在 attach 时问一次**的（`netup`）。
  ⇒ 在接口已经存在之后再 `txon`，只会影响 RX 侧，直到下一次 attach。所以验证配方是
  **在链条的 netup 之前**把它打开：`sudo CX3_TXCSUM=1 tools/cx3-mlx4/verify.sh up`
  （verify.sh 读这个环境变量，在读 netup 之前调用 `netoff txon`）。
  RX 侧没有这个约束（逐帧由驱动自己决定，随时可开）。
* **参照**：`docs/M5-REFERENCE.md` §1、`docs/M5-MACOS.md`（macOS 要问什么、mbuf 上怎么表达）。
* **机制（已查实，纠正"有个端口开关"的想当然）**：**没有端口级 / INIT_PORT 开关** ——
  `INIT_PORT` 只收 `port` 一个入参。RX 是**硬件无条件计算**，驱动只读 CQE 的
  `status.IPOK` 位 + `checksum == 0xffff`，然后告诉栈；**TX 是逐 WQE 的 `srcrb_flags` bit4/bit5**。
  ⇒ 两端是两件不同的事：**RX 是"解释"，TX 是"请求"**。
* **风险**：中。TX 侧改的是**已经证明能工作的 WQE**，RX 侧改的是每个 CQE 的解释 ⇒ 一次只做一半，
  先 RX 后 TX。

### M5-4 JUMBO / MTU —— **前置（停端口）与 TX 缓冲自适应已就位，还差 macOS 侧与 RX 环参数**

* **已实现（0.44.17）**：
  * **停端口**：选择器 45 `kCx3SelNetPort` / `cx3ctl netport report|down`（设备侧就是
    `CLOSE_PORT`，`mlx4_fw.c:2358-2363`：无参、in_mod=port、op_mod=0、**TIME CLASS A**）。
    **没有 up 模式是刻意的**：重开端口就是链条的端口上启那一步，而它**已经接受 MTU 输入**
    （`kPortUpInMtu`），所以 MTU 变更的配方是 `netport down` → `cx3ctl m3portup 1 <mtu>`，
    不在这里再抄一份三步序（那会是第二个可能把顺序写错的地方）。停端口时**链路读数被清空**
    （不是留着），否则 `publishLinkStatus` 会报一个已经过期的 "active"；数据通路启用时**拒绝**
    （在活接口底下关端口 = 静默断流）。
  * **TX 帧缓冲尺寸跟随 MTU**（`fNetTxBufBytes`，在 `netup` 时按
    `mtu + 14 + 4 + 4 + 2` 向上取到 2 的幂，与参考实现的选法一致，`docs/M5-REFERENCE.md` §3.2）：
    **MTU 1500 时算出来正好是 2048**，也就是这条路径被设计时的尺寸 —— 默认行为逐字节不变。
* **还差两块（都已定位）**：
  1. **RX 环缓冲尺寸**：链条的 `r4rq` **已经接受** `bufBytes`（`kR4RqInBufBytes`，0=2048），
     所以 jumbo 配置在 RX 侧是**参数**而不是新代码；
  2. **macOS 侧**：`getMaxPacketSize`（能力上限）与 `setMaxPacketSize`（栈告诉驱动新的上限），
     见 `docs/M5-MACOS.md` §4。**这一块要小心**：栈发起的 MTU 变更会落在一个**门内回调**里，
     而设备侧的重开端口**不能在门内发命令**（那正是本项目用 TryLock 避开的死锁形状）⇒
     要么由选择器驱动（诚实的配方：`netport down` → `m3portup 1 <mtu>`），
     要么把设备侧放到一个内核线程上（更大的改动，独立决策）。**在这一点定下来之前不 advertise
     jumbo 能力**——宣称栈能改、实际要你手动跑两条命令，那不是诚实的能力声明。
* **要什么**：能把 MTU 提到 9000 且大数据包真的通；不支持的配置被**明确拒绝**而不是静默丢帧。
* **判据**：`ping -s 8000`（分片关闭）通；`netstat` 的 `txDropped` 不增长。
* **风险**：中高，而且**依赖上面第 5 条**：Linux 改 MTU 时是 `stop_port → start_port`
  （`docs/M5-REFERENCE.md` §3.4），也就是**端口必须重开**。接收缓冲尺寸按
  `mtu + ETH_HLEN + VLAN_HLEN + FCS + 2` 从**四档离散 cluster** 里选（§3.2），
  RQ 的 `byte_count = rx_mb_size - 2`（§3.3）。⇒ 一次要同时改对三处：
  `rx_mb_size` → RQ `byte_count` → `SET_PORT_general.mtu`。
* **注**：现在的诚实边界是"帧长超过缓冲就丢弃并计数"（`kNetTxMaxFrame`）。**在没有 jumbo 之前，
  把 MTU 上限钉在 1500 也是一种可接受的诚实做法**——只要它不静默通过。

### M5-5 VLAN
* **要什么**：带标签的帧能收（标签剥离并交给栈）与能发（标签插入）。
* **判据**：`ifconfig vlan0 create` + 打标签的 ping 通。
* **机制（已查实）**：RX 剥标签是 **QP context 的 `param3` bit30**（`MLX4_STRIP_VLAN`），
  标签落在 CQE 的 `sl_vid`；TX **只写 WQE 的 `vlan_tag` + `ins_vlan`，帧里不许带标签**
  （`docs/M5-REFERENCE.md` §2）。还有第三张表：128 项的 `SET_VLAN_FLTR`。
* **注意**：`MLX4_STRIP_VLAN` 的**极性在本树不可判定**（代码在*没有* `IFCAP_VLAN_HWCSUM` 时才置位，
  位名却叫 STRIP）⇒ 上机第一步是**打标签帧进、看 CQE 的 CVLAN 位与帧内是否残留 tag** 的 A/B，
  不是照抄那一行（`docs/M5-REFERENCE.md` 附录 B）。
* **那条 A/B 的具体配方（可执行，不需要新代码）**：
  1. 用 `r4tx` 的原始发送路径发**一个带 802.1Q 标签的帧**（现有 raw 路径就是自造帧，标签只是
     那几个字节的事）；
  2. 对端（或本机另一张网卡）看到的是**打了标签的帧**，所以"发出去"这一侧不需要新的测量；
  3. 收侧：让对端发一个**带标签**的帧，看两件事——CQE 的 `sl_vid`（0x0c）里有没有 VlanID、
     以及**帧里是否还留 4 个标签字节**（我们自己的缓冲，直接读）；
  4. 两个 QP 各建一次（`param3` 的 bit30 置位 / 清零），比较第 3 步的两个观察。
  ⇒ **结论是 4 个读数的一张 2×2 表**，极性从此不再是未知；在此之前不写这一位。
* **风险**：低（不属于默认路径），但需要端口就绪时就设好剥离位。

### M5-6 多队列 / RSS —— **已裁定：不在可达范围内**（见 §1.5 的范围裁定）

macOS 这一代不给 kext 多队列/RSS 的接口（`docs/M5-MACOS.md` §5：只有单个 output queue；
Skywalk 不在系统头里，且 pull-model 那批 API 全在 `__PRIVATE_SPI__`）。
硬件侧的做法记在 `docs/M5-REFERENCE.md` §4（RX 环数 = `max(4, min(eqs_per_port,16))` 取 2 的幂、
RSS 走额外定向 QP、TX CQ 复用 RX CQ 的向量），**但栈侧没有入口，所以不做**。
要做只有换架构（DriverKit dext），那是另一个决定。

### M5-7 TSO（大包分片卸载）
* **要什么**：栈交下来的大 `mbuf` 由硬件切分发送（`MLX4_OPCODE_LSO`），省掉栈里的软件分片。
* **判据**：`ping -s 8000`（关分片）或 `iperf -l 64k` 时线上分片正确、对端计数一致；关掉后不退化。
* **参照**：`docs/M5-REFERENCE.md` §1.5（LSO 相关的 WQE 字段）。**数值已知**：
  `MLX4_OPCODE_LSO = 0x0e`（本树 `mlx4_regs.h` 尚未转写 ⇒ 先补常量再动手）。
* **前置**：M5-4（jumbo/MTU）先做完 —— TSO 的分片尺寸由 MTU 决定，MTU 都还不能改的时候做 TSO 没有意义。
* **风险**：中高（改的是发送描述符的构造，而发送是本项目最贵的路径之一）。

### M5-8 组播 / 混杂过滤 —— **组播侧已实现（0.44.15），混杂侧仍是记录的（见下）**

* **关键证据（这次才拿到的）**：`mlx4_multicast_attach()` 在 **A0 模式且协议为 Ethernet 时直接
  `return 0`，什么都不发**（`mlx4_core/mlx4_mcg.c:1408-1417`）；而**我们的卡就是 A0**
  （驱动用 `queryhca` 从设备读出来验过，不是假设）。⇒ 这张卡上组播**只有一套机制**：
  端口的组播过滤表，用 `SET_MCAST_FLTR` 编程，**哈希由固件做**（驱动只给 MAC 和模式）。
  实现量因此从"两套机制 + 流定向"缩到"一条命令序列"。
* **已实现**（选择器 44 `kCx3SelNetFilt` / `cx3ctl netfilt [report|mcaston|mcastoff]`，
  **默认关**）：`setMulticastList` 现在**记住**栈给的列表（栈只在列表变化时调用，这是唯一
  的捕获时机），开关打开时按参考实现的顺序编程：
  `DISABLE → CONFIG(广播 + CLEAR 一步完成清表并种入广播) → 每个组地址 CONFIG → ENABLE`。
  **先种广播**是刻意的：否则一旦使能，端口会连广播也不再收——这是这个功能唯一可能弄坏
  "已经能工作的东西"的途径。任何一步失败就**停止且不发 ENABLE**（半张表使能比不使能更糟）。
* **`mcaston` 会重放**已经记住的地址：否则开关看起来什么都没做（栈不会再调一次）。
  `mcastoff` **不改写设备里的表**，日志明说这一点。
* **判据**：`netfilt mcaston` 后 `ifconfig en<N> inet6` 出现 link-local 地址、
  `ping6 ff02::1%en<N>` 有回应；`netstat` 的 `rxFrames` 继续增长（单播没被弄坏）。
* **混杂模式也已实现**（同一个选择器的 `promiscon`/`promiscoff`，**默认关**）：A0 下它就是
  `SET_PORT_qpn_calc` 的 promisc 位（`mlx4_en_netdev.c:780-786`），**与端口上启时发的是同一条
  命令、只动一个位**；三个入参全部**读出来**而不是假设（base QPN = RX QP 自己的号、n_mac =
  **现场** QUERY_PORT 的 `log_max_macs`、m_promisc = 能力位），任一不可得就**拒绝**。
  另外按参考实现：**打开时无条件把端口组播表关掉**（`:790-795`），关闭时若组播开关也开着就
  **重新编程组播表**（参考实现里这一步是它的调用者做的下一步）。
  判据：`promiscon` 后 `tcpdump -i en<N>` 能看到**别人之间**的帧（组播/广播之外的单播）；
  `promiscoff` 后恢复，且 `ping` 仍然通（这一步就是可用的"撤销"：同一条命令把位清零）。
* **要什么**：`ifconfig en<N> multicast` 之后组播帧真的收得到（IPv6 因此能用）；
  混杂模式真的收到别人地址的帧。
* **判据**：加入一个组播组后能收到该组的流量；`ping6 ff02::1%en<N>` 有回应。
* **参照**：`docs/M5-REFERENCE.md`（Linux 侧 `set_rx_mode` / 组播表的整表下发）。
* **现状**：三个回调**接受并记录、但不写硬件**（`docs/R4-STAGE10-PLAN.md` §12.6）。这一项就是把它
  变成真的。
* **风险**：中。改的是**收包过滤表**——如果表写错，可能把单播也过滤掉 ⇒ M4 的判据会跟着坏。
  所以必须先做 M5-1/M5-2，并且改完立刻重跑 M4 判据。

## 3. 建议顺序（每步之间都要重跑 M4 判据）

```
M4 验收（ping 通 + 100 包 0 丢）
   │
   ├─► M5-1 媒体表/链路监视 ─────────────── 唯一不碰数据通路布置的一项，先做
   │
   ├─► M5-3 校验和卸载（先 RX 解释，再 TX 请求）
   │
   ├─► M5-2 中断驱动的接收 ──► M5-8 组播/混杂过滤
   │      （先做「排空再 arm」的最小实验；组播表写错会连单播一起坏，所以放最后一批）
   │
   └─► 【前置：停端口 → 重开端口 的路】（第 0.5 条）
            │
            └─► M5-4 jumbo/MTU ──► M5-7 TSO ──► M5-5 VLAN
                （M5-6 多队列/RSS 已裁定不在范围内，见 §1.5）
```

理由：先做**不碰数据通路布置**的（M5-1）；再做**只改解释、不改投递**的（M5-3）；然后才改
**投递机制**（M5-2）；尺寸那一条链（M5-4→M5-5）需要"停/重开端口"的路，
而且 TSO 依赖 jumbo 的 MTU ⇒ 排在最后。**每做完一项都要重跑 M4 判据。**

## 3.5 一条命令验证 M5：`tools/verify.sh m5`

M5 的开关**默认都是关的**，所以它们和 M4 的判据天然分离——但是**验证起来是五条命令**。现在是一条：

```sh
sudo tools/verify.sh up     # 链条 + 一个活接口（M4 gate 就在里面）
sudo tools/verify.sh m5     # M5-1 媒体表/链路 → M5-8 组播/混杂 → M5-3 RX 校验和（自己打一次 ping 造流量）
```

它从 `netstat` 的**单位号标量**（不是解析文本）拿到接口名，逐步判定，并且每步都区分
"开关生效 / 没生效 / 这次判不了"；最后附上 `cx3ctl netstat` 的完整计数。传输侧校验和那一步需要
`CX3_TXCSUM=1` 在**接口 attach 之前**设置，所以它只在真的设置了的时候报 PASS，否则如实说
"要在 attach 前打开，得用 `CX3_TXCSUM=1 verify.sh up`"。

**顺带修掉一个 harness 老缺陷**：`runcall` 一直被 `j=$(runcall ...)` 这样调用——**命令替换是子 shell**，
所以它对 `LAST_CALL_RC/LAST_CALL_SIZES` 的赋值**从来没传回调用方**，于是本文件里 43 处
"the call failed: rc=..." 一直在报**调用前的旧值**（通常是 `rc=0`，读起来像"内核回了 0 = 成功"）。
现在失败细节写进一个文件（子 shell 唯一还能通信的通道）并由 `callinfo()` 读出，
实测已显示真值：`rc=-536870201 (kern_return 0xe00002c7), selector 40, in=0 out=1 struct=168`。
这个缺陷是新写的 `m5` 模式**运行一次**才暴露的。

## 4. 每一项都要带的三样东西

1. **一条可复现的判据**（命令 + 期望读数），写进 `tools/verify.sh` 或本文件；
2. **`cx3ctl netstat` 里能看到的计数**（失败时能区分"没做"和"做了但没生效"）；
3. **回滚**：这一项关掉之后，M4 的判据仍然通过。
