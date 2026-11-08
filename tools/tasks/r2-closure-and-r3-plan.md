任务：审查一个网卡驱动移植项目的**事件队列中断阶段**（R2）证据是否闭环，并设计下一阶段（R3：
端口与链路）的实现顺序。

## 项目背景

把 **BSD 许可**（OpenIB.org BSD 分支）的 Mellanox 驱动从 FreeBSD `sys/dev/mlx4` 移植到
macOS IOKit kext。硬件 Mellanox ConnectX-3 Pro（PCI `15b3:1007`，板卡零件号 MCX354A-FCCT，
双端口 QSFP+，VPI 卡）。目标机 macOS 15.8.1 x86_64。

前序已完成：固件启动（MAP_FA / RUN_FW / MOD_STAT_CFG / QUERY_DEV_CAP / SET_ICM_SIZE /
MAP_ICM_AUX / 14 张上下文表 / INIT_HCA 全部返回固件状态 0）；事件队列（EQ）先用**轮询**
证明 DMA 链路可用（事件模式 NOP 的完成以 CMD EQE 形式出现）；现在给 EQ 接上了中断。

## 允许阅读的路径（只读这三处，不要去别处）

```
/Users/you/Documents/Hackintosh-MacLocal/tools/mlx4-port/freebsd-src/     # 参考实现
/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/              # 移植代码
/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/docs/M2B-NOTES.md # 证据日志
```

移植代码的入口：`Cx3Mlx4.cpp`（selector 实现、`consumeEqLocked()` 单一消费者、`irqAction()`）、
`Mlx4Hw.cpp`（`eqCreate` / `eqNextEqe` / `eqHw2Sw` / `clrIntSetup` / `clrIntWrite`）、
`mlx4_regs.h`（偏移与 opcode）、`cx3_abi.h`（selector 定义）。

## 已经实测到的事实（真机，不是推测）

1. 提供商给的中断类型是 **legacy INTx**（`getInterruptType` 返回 type=0x1），不是 MSI-X。
   PCI 配置空间里 MSI-X 能力存在但 Enable 位为 0，且没有 MSI 能力。
2. legacy 电平中断**必须**在设备侧清除，否则风暴。第一次没清就发成了真实故障：中断计数
   2→200+、日志刷 "0 EQE(s) consumed"，随后整机卡死（事后确认那次卡死的回溯里没有本 kext
   符号，但触发原因就是这个未清除的中断）。
3. 现在清除已实现：`QUERY_ADAPTER` 取到 `inta_pin = 63`，`clr_mask = swab32(1 << (63 & 31))`
   = `0x00000080`，清除寄存器落在 BAR0 + `0xf0058`（8 字节）。中断 action 的**第一条动作**
   就是写这个清除；`m3irqup` 在清除未就绪时**拒绝**开启 legacy 源（没有强制开关）。
4. 中断投递已拿到结果：`m3irqtest <token>` 登记 token 并投递一条事件模式 NOP 后**立即返回**
   （不等待），完成由中断 action 上的同一个消费者按序取走。三次连续测试（各自独立 token）
   全部 `matched YES`、`status 0`；`m3irqdown` 之后 3 秒内没有迟到回调。
5. **清除确实生效的判据**：`acks`（写清除次数）与 `invocations`（handler 被调用次数）1:1 增长。
   清除之前同样的测试是 200+ 次调用却不前进。
6. **回绕压测刚跑完**：从 `cons_index = 5` 起连续投递 1031 个事件模式 NOP（环形 512 个 EQE），
   结果：`invocations 1031`、`callback count 1031`、`EQEs 1031 via interrupts`（轮询路径 0 个）、
   `acks 1031`、`matched YES token 0x3406 status 0x0`、`cons index 1035`、
   `zero streak 0`（没有任何一次"被调用但什么都没消费"）。即：**2 次以上相位回绕通过**。
7. 消费者的语义：只看持久 CI 指向的槽；按序消费；遇到不可消费的槽就停；一次扫描最多
   **64 个** EQE；扫描结束无条件写一次门铃（参考 `mlx4_eq.c` 是 `eq_set_ci(eq, 1)`）；
   非 CMD 类型的事件（含 type 0）会被记录但**不**被当成待匹配的完成；CMD EQE 的 token
   不等于待匹配 token 时，消费并记录 "does not match the pending"，不报成功。
8. 参考实现里 EQ 上下文有一组**预留 EQ**（本卡 `QUERY_FUNC` 报 `max_eq 156, reserved_eqs 28,
   reserved_uars 7`；异步 EQ 是 `EQN = 28`），它的 `intr` 字段是 **MSI-X 表项索引**，参考给
   异步 EQ 传 0；本移植的 EQ 上下文也写 0，但实际拿到的是 legacy 源，靠索引 0 就能收到。

## 尚未解决 / 未验证（我明确不假装它们已完成）

- **"固件在 `SW2HW_EQ` 期间异步初始化环形"只是假设**：把内存 `memset` 后立刻轮询会看到全零环形，
  而参考的"owner == phase 即可消费"规则会把全零槽判成可消费（这正是曾经漏掉一个 EQE 的原因）。
  已实测的只是：`memset` 后为 0、自检写 `0x5a` 通过、`SW2HW_EQ` 之前为 0、`SW2HW_EQ` **之后**为 `0xcc`
  （即固件确实在写）、以及该写入过程约持续 238 ms。**没有**做"新环形 + 对照环形、投递/不投递
  `SW2HW_EQ`、交换目标"的 A/B 实验。
- **"混合事件不冒充"只在代码路径上成立**，没有真机证据：要真出一个 PORT_CHANGE 事件需要先把
  端口配置起来（那是 R3），所以这一条目前是空白。
- **32 字节 EQE 格式下 owner 位的偏移未实测**（本卡 `eqe_factor` 已实测，但只验证过一种长度）。
- 中断对象生命周期的**状态机**（preparing/running/failed/stopping）与失败回滚没做：当前只有一把
  队列锁 + "暴风守卫"（连续多次调用什么都没消费就禁用源并锁存故障）。
- MSI-X **完全没做**：IOKit 那个入口（`configureInterrupts` 的签名/可见性/调用时机）我没有可靠
  资料，宁可空着也不猜。现在用的是 legacy 清除这条**实测可行**的路。
- CI（consumer index）回报策略：现在每轮扫描结束都写一次门铃，参考是每 `MLX4_NUM_SPARE_EQE`
  （=128）个 EQE 更新一次 CI。两者代价与正确性差异没有评估过。

## 请你回答（按编号，逐条；能给 file:line 就给）

A. 上面第 6 条的回绕证据，是否足以把"两次以上相位回绕"这一验收项判为通过？还有哪个具体反例
   没被排除（例如：EQE 覆盖写、门铃与消费顺序、跨页边界）？

B. 设计一个**确定性**的最小真机测试来验证"混合事件不冒充 / 不同的完成不会被当成待匹配的完成"，
   约束是：不能依赖 PORT_CHANGE，不能改内核代码（只能用现成的两个 selector：一个"登记 token +
   投递事件模式 NOP 后立即返回"，一个"读中断路径状态"）。说明每一步的预期与反例，并说明如果
   两次登记之间完成已经到达、测试会退化成什么样（这时结论还能说什么）。

C. CI 回报策略：每轮结束写一次 vs 每 128 个 EQE 写一次。给出你的取舍与理由。特别注意：本移植
   的中断 action 每次最多消费 64 个，而重新武装门铃本身也是一次 MMIO 写。

D. 参考实现里 legacy INTx 与 MSI-X 的中断处理函数**分别**做了什么（清除 vs 不清除）？本移植
   "只有清除就够"的结论有没有漏洞（例如：是否还需要读某个状态寄存器、是否需要掩码/解掩码、
   是否存在共享中断线导致必须判断"是不是我的中断"）？

E. **R3 设计**（最重要）。硬件是 MCX354A-FCCT（ConnectX-3 Pro VPI，双 QSFP+）。目标是在**不重刷
   固件**的前提下把端口拉成以太网并让链路 up。请给出：
   1. 怎么从软件确定端口当前的链路类型（IB 还是 Ethernet）——参考里哪些字段可确证
      （`QUERY_DEV_CAP` 的端口相关字段、`QUERY_PORT` 的 `link_layer`/`proto` 等），偏移是多少，
      以及如果固件配置成 IB 时会出现什么现象；
   2. 从当前状态（INIT_HCA 已完成）到"链路 up"需要的 HCR 命令**顺序**，以及每条命令的关键输入
      字段（含偏移）与"读哪个输出字段判成功"；
   3. 参考里这些命令做的**隐含前提**（例如是否需要先 `QUERY_PORT` 再 `INIT_PORT`、`SET_PORT` 的
      `mlx4_set_port_general`/`mlx4_set_port_rqp_calc` 系列、是否需要 MCG/隧道相关的表）；
   4. 每条命令**哪些是参考源码能确证的、哪些必须实测**，并明确标注你的不确定性；
   5. 风险清单：哪些步骤可能让整机卡死、哪些可能让卡进入需要重刷固件才能恢复的状态。

F. 本阶段还有一件事可以花一次重启去做：上面"固件异步初始化环形"的 A/B 实验。你判断现在做、
   还是留到 R4（队列与完成队列）之前更合适？给出理由与预期信息量。

回答用中文。要求：**逐条回答 A–F**，区分"源码可确证"与"你的推断"，不确定就写"unknown"。
不要泛泛而谈，不要复述任务背景。
