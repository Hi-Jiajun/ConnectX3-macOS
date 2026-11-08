任务：审查一组**真机实测结果**，判断结论能不能站住，并设计下一步的判别性实验。
这是一个网卡驱动移植项目的 R3 阶段（端口与链路）。

## 项目

把 **BSD 许可**（OpenIB.org BSD 分支）的 Mellanox 驱动从 FreeBSD `sys/dev/mlx4` 移植到
macOS IOKit kext。硬件 Mellanox ConnectX-3 Pro（`15b3:1007`，板卡 MCX354A-FCCT，
双口 QSFP+ **以太网**卡）。目标机 macOS 15.8.1 x86_64。

已完成：固件启动 / `INIT_HCA` / 14 张上下文表；EQ（EQN 28、512 槽、64 字节 EQE）
的**轮询**与**中断**两条路都验证过（1031 个事件、两次以上相位回绕、token 匹配）。
`cmd_if_rev = 3`（新端口命令接口）。

## 允许阅读的路径（只读这三处）

```
/Users/you/Documents/Hackintosh-MacLocal/tools/mlx4-port/freebsd-src/   # 参考实现
/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/            # 移植代码
/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/docs/M2B-NOTES.md
```

## 结果一：端口拉起来了（真机）

`QUERY_PORT`（只读，端口 1）原始 64 字节，配置**之前**：

```
0000: 12 85 26 ac 08 0f 03 77 02 00 77 08 00 00 00 00
0010: 00 00 ec 0d 9a 2a 15 11 00 00 00 00 00 00 80 1b
0020: 00 00 00 00 00 00 00 00 00 00 ec 0d 9a 2a 15 11
0030: 00 00 25 80 00 00 00 00 00 00 00 00 00 00 00 00
```

解出来：`0x00` = `0x12` → bit7=0（链路 down）、bits1:0 = **2（只支持以太网）**、bit4=1；
`0x01` = `0x85` → autoneg=1、ib_mtu=5；`0x02` = BE16 `0x26ac` = **9900**（eth_mtu）；
`0x05` = `0x0f`（参考也称之为未知编码）；`0x07` = `0x77` → max_gids/max_pkeys = 128/128；
`0x0a` = `0x77`；`0x0b` = `0x08` → max_vl=8、max_tc_eth=0；`0x10` = BE64
`0000ec0d9a2a1511`；`0x18` = `0x1b800000`（trans_type=0x1b？）。

端口 2 的原始字节只有 `0x17` 与 `0x2a` 两处不同（MAC 尾字节 11 → 12，与卡上 NVRAM 一致）。

**跨源交叉验证**：`0x12..0x17` 的 6 个字节 = `00:11:22:33:44:55`，与这张卡在 Linux 上
`ip link` 显示的 `eth0` MAC **逐字节一致**。这条是因为解码写错方向时最可能露馅的地方。

然后按参考的正常启动顺序执行（`in_modifier = (GENERAL<<8)|port`、`op_modifier = 1`
（Ethernet）、输入 mailbox 填 `mtu = 1500+14+4+4 = 1522`、`flags = 0x07`、pause 全 0）：

```
  stage reached      : 4 of 4
  gate QUERY_PORT    : fw status 0x0
  supported types    : 2  (2 = Ethernet)
  link bit before    : 0
  SET_PORT general   : fw status 0x0   (mtu sent: 1522)
  INIT_PORT          : fw status 0x0
  link bit after     : 1
  speed code after   : 0x1 -> 10000 Mb/s
```

之后重新 `QUERY_PORT`：`link_state = 1`、`link_speed = 0x01`（10G XFI）、`eth_mtu = 9900`。
**同一开机内做第二次**（先 `reset` + 完整 bring-up + 同样的序列）**结果相同**：link bit 1、
10 Gb/s。

## 结果二：A/B 环形初始化实验（真机）

背景：此前观察到"把 EQ 环交给 `SW2HW_EQ` 之后，环内存内容变了（`memset` 后为 0，
`SW2HW_EQ` 之前为 0，之后变成 `0xcc`）"，并且项目里长期引用"这个初始化是异步的、
大约持续 **238 ms**"。这两条都只有相关性。

实验：两个**同样构造**的环 A/B（同 nent=512、同 64 字节 EQE、各自的 DMA 页、各自的 MTT
区间，selector 会逐页检查别名），`memset` 清零，各自留全环 32 KiB 基线快照；
**只把其中一个**交给 `SW2HW_EQ`（EQN 28），在 8 个真实时刻（`clock_get_uptime`，
0/5/20/60/150/300/600/1000 ms）对**两个环**做全环逐字节差分。不 `MAP_EQ`、不写门铃、不发事件。

**第 1 轮（把 A 交出去）**：

```
SW2HW_EQ         : fw status 0x0, took 5188 us
HW2SW_EQ         : fw status 0x0
t=+      4 us  ring A:  32507 changed of 32768 (32507 nonzero, 512 owner=1)   ring B: 0 changed (0 nonzero, 0 owner=1)
t=+   8532 us  ...同上...
...（8 个采样点全部相同）
ring A final: 32507 byte(s) differ from baseline, pages changed 0xff, 512 of 512 slots owner=1
             first differed at +4 us and last differed at +1002427 us
ring B final: 0 byte(s) differ, pages changed 0x00, 0 of 512 slots owner=1
             NEVER differed from its baseline in any of the 8 samples
```

**环 A 前 64 字节的三个时刻**（这是本次新加的 dump）：

```
baseline        : 00 ×64
first post-cmd  : 0000: cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc cc
                  0020: ee ee ee ee ee ee ee ee ee ee ee ee ee ee ee ee ee ee ee ee ee ee ee ee 00 00 ee ee ee ed 96 ee
after HW2SW_EQ  : 与 first post-cmd 完全相同
```

即：每槽前 32 字节填 `0xcc`、后 32 字节填 `0xee`，**但有少数字节不是填充值**
（`0x38`/`0x39` 是两个 `0x00`，`0x3d` = `0xed`，`0x3e` = `0x96`）。
全环 32768 字节里 32507 变化（第 1 轮）/ 32510（重复那一轮），也就是还有 261 / 258 字节保持零，
**两轮的计数不同**。

**第 2 轮（交换角色，把 B 交出去，仍然用 EQN 28）**：

```
SW2HW_EQ         : fw status 0x0, took 9982 us
HW2SW_EQ         : fw status 0x0
t=+0 ... t=+1000000 us：ring A 与 ring B 都是 0 changed / 0 nonzero / 0 owner
两个环都 NEVER differed
```

即**第二次把另一个环交给同一个 EQN，两个环都没有被写**（命令返回 fw status 0）。

**再做一次 `reset` + 完整 bring-up 之后重跑第 1 轮**：环又变了（32510 字节），图案同上。

## 结果三：项目里"238 ms"这个数字的来源，已定位为一个**显示缺陷**

`m3equp` 里有一段"armed wait"：等环出现 armed 图案且连续两次读同一组 16 字节。
工具一直打印 **`arm wait: 238 ms`**，而**同一次调用**的内核日志写的是
`armed wait: the ring reached the armed pattern and stopped changing after 1 ms;
1 distinct reading(s) of the first 16 bytes were seen on the way`。

根因：`reportOwnerMap(out, base)` 有一个隐含约定——它写 `out[base]`（owner 计数）和
`out[base+1]`（slot 0 的 owner 字节原始值）。`m3equp` 传的 `base = 14`，于是它写了
**索引 15**；而 `kEqUpOutArmWaitMs` **也是 15**。owner 字节是 `0xee` = **238**。
所以"238 ms"是 owner 字节被当成毫秒数打印出来，**不是测量值**。
（`m3eqpoll` 那一侧没有这个问题：它的 enum 里 `kEqPollOutOwner0 = kEqPollOutOwnerSet32 + 1`
是专门留的槽位。）

因此项目里"固件异步初始化环形、耗时约 238 ms"的说法**不成立**：真实读数是
**1 ms**（而且 3 次成功的运行都是"1 distinct reading"，即没有观察到分阶段写入）。
新增 selector 共 27 个，输出标量上限是 16，`kEqUpOutCount = 16` 已经顶格。

## 请你回答（逐条，A–F；能给 file:line 就给）

**A. 结果一的结论是否站得住？**"端口已按以太网配置、链路 up、10 Gb/s"这个结论，
需要哪些额外证据才算闭环？我目前只有 `QUERY_PORT` 的 link bit 与速度编码、以及命令返回 0。
特别请判断：**一个只有 link bit 的读数，能不能被别的现象伪造**（例如端口被固件按 IB 配置时
会不会也报 bit7=1；`supported_types = 2` 是否已排除这种可能）。
另外：`0x18` 处的 BE32 在我这里解出 `trans_type = 0x1b`、`vendor_oui = 0x800000`，
而参考把 `0x18` 的高字节当 `transceiver`、`0x1c` 当 `wavelength`（u16）、`0x20` 当
`trans_code`（u64）。**请核对我这几个字段的解码**（`0x1b`/`0x800000` 是否合理，
`wavelength`/`trans_code` 在这种模块上为 0 是否合理）。

**B. 结果二第 1 轮能不能支撑"写入者是固件"？**我的推理链是：两个环经过完全相同的
主机侧步骤（分配、清零、MTT 写入、同一时刻的时钟），只有被交给 `SW2HW_EQ` 的那个被写；
而且是**按 MTT 描述的那些页**被写（对照环不在任何活动上下文里）。请指出这条推理
**还缺哪一步**，特别是：如果写入者不是固件而是别的 DMA 代理，会是什么样子、怎么排除。

**C. `0xcc`/`0xee` 这个图案是什么？**每槽前 32 字节 `0xcc`、后 32 字节 `0xee`，
但少数字节不同（`0x38`/`0x39` = `0x00`，`0x3d` = `0xed`，`0x3e` = `0x96`），
而且**两轮之间"保持零的字节数"不同（261 vs 258）**。请判断：
这更像固件的固定填充、还是包含某些随运行变化的内容（校验/指针/时间）？
**在允许阅读的参考源码里，有没有任何地方提到固件会写队列内存 / 写什么图案？**
如果没有（我倾向于没有），请说明"参考源码沉默"能支持到什么程度。

**D. 第 2 轮"两个环都没变"说明什么？**候选解释：
① 固件**每个 EQN 只初始化一次**（同一次开机内，第一次 `SW2HW_EQ` 之后不复位就不再做）；
② 固件**每次开机只初始化一次**；
③ 固件认为 EQN 28 上"队列还在"（尽管 `HW2SW_EQ` 返回 0），于是没有重新建立；
④ 别的解释。
请设计一个**判别性实验**把我的 selector 只增加一个输入就能区分 ①②③：
我打算给 A/B selector 加一个"用哪个 EQN"的输入。请给出**每一轮用哪个 EQN、预期观察、
以及什么结果支持哪个解释**的完整表格；并指出这个实验里还有哪些因素必须保持不变
（例如两轮之间要不要 reset、要不要 `HW2SW_EQ`、环的内容要不要重新清零）。
注意：`QUERY_FUNC` 在本卡上报 `max_eq = 156, reserved_eqs = 28`，异步 EQ 是 EQN 28。
**哪些 EQN 是安全的**（不会撞到别的保留队列）？请给出依据。

**E. 这个结果对驱动设计的影响。**`m3equp` 里那段 armed wait 的存在理由原本是
"固件异步初始化、期间轮询会读到全零环形并把没写过的槽当成可消费"。现在：
写入在 `SW2HW_EQ` 返回时已经完成、arm wait 1 ms、没有观察到分阶段写入。
请判断：(a) 那段 armed wait 还应不应该保留（保留的理由是什么、去掉的风险是什么）；
(b) 我之前把一次"EQE 落在读指针后面"的 bug 归因于"固件异步初始化窗口"，
现在这个归因还成不成立？如果不成立，**在允许阅读的源码里**有没有别的解释能同时说明
那个观察（环形 512 槽、`cons_index` 曾从 1 走过、一个正确的 CMD EQE 被留在后面）；
(c) 队列消者的正确性是否依赖"环在 `SW2HW_EQ` 之后是 `0xcc`/`0xee`"这个事实
（即：如果某次 `SW2HW_EQ` 没有写环，全零环会不会被 owner/phase 规则判成"全部可消费"）——
如果是，驱动应该怎么防。

**F. R4 的顺序。**端口已经 up。下一步是 CQ + QP + 单队列收发（目标：ping 通）。
请按参考源码给出**最小可信顺序**，并明确每步的成功判据与"哪些是源码可确证的、
哪些必须实测"。特别注意这几件事的先后与依赖：
`SET_PORT_qpn_calc`（本卡 `DMFS` 是 disabled，参考只在 A0 steering 下发它）、
MAC 表 / VLAN 表 / 组播（broadcast）attach、`SET_PORT_PRIO_MAP`、
`PD` 分配、`CQ`/`QP`（`SW2HW_*` 与状态机的顺序）、`RSS`、
以及"收包要能工作"最少需要哪些 steering 配置。

回答用中文，**逐条 A–F**，区分"源码可确证"与"你的推断"，不确定写 "unknown"。
不要复述背景。
