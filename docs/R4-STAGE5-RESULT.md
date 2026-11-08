# R4 阶段 5（`r4Ingress`）实现记录：MAC 表 + 单环 RSS 入口 QP

本文是**实现记录**。实现的是 `docs/R4-STAGE5-PLAN.md`（方案已定稿，结论未改），
其中三个待定项按用户拍板执行（第 5 节逐条记了依据）：

1. **MAC 注册照做**；
2. **入口 QP 的 `db_rec_addr` 用左移那份**（`<< 2`），并且**语义已查清** —— 见 §5.2；
3. **`rss_rings = 1`**（单环）。

**本阶段一行都没在硬件上跑过。** 每处的"实测/推断/未测"在第 7 节；kext 未安装、
机器未重启，验证留给用户重启后做一次。

---

## 1. 改动的文件（绝对路径）

| 文件 | 改了什么 |
| --- | --- |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/mlx4_regs.h` | MAC 表的 7 个常量（`MLX4_SET_PORT_MAC_TABLE` / `MAX_MAC_NUM` / `MAC_ENTRY_SIZE` / `MAC_TABLE_SIZE` / `MAC_VALID` / `MAC_MASK`）；RSS 叠加的 8 个偏移常量（`QP_CONTEXT_RSS_*`）+ `QP_CONTEXT_PRI_PATH_OFFSET`、`MLX4_RSS_OFFSET_IN_QPC_PRI_PATH`、`MLX4_RSS_QPC_FLAG_OFFSET`、`MLX4_RSS_HASH_TOP`、`MLX4_RSS_QPN_HIGH_SHIFT`、四个 `MLX4_RSS_*` 协议位、`MLX4_EN_RSS_KEY_SIZE`；**SET_PORT 两组枚举的重写注释（E9）** |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Mlx4Hw.h` | 声明 `setPortMacTableWrite` / `setPortMacTableRead` / `qpFillRssContext`；`qpFillContext` 增加 `rss` 参数 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Mlx4Hw.cpp` | 新一节 "R4 stage 5"：两个方向各一个函数；`qpFillContext` 增加 `rss` 分支（flags 的 RSS 位、`rq_size_stride` 不写）；`qpFillRssContext` |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/cx3_abi.h` | `kCx3SelR4Ingress = 35`、`kCx3SelCount = 36`、`Cx3R4IngressResult`、字段掩码枚举、15 个标量槽、`CX3_R4INGRESS_NOT_MEASURED`、三条静态断言 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Cx3Mlx4.h` | `exportR4Ingress`、`ingressTeardown`、12 个新成员（MAC 记录 + 入口 QP） |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Cx3Mlx4.cpp` | 成员初始化、`ingressTeardown`、`r4IngressMacEntry` / `r4IngressBuildKey` / `r4IngressQueryAndCompare` / `r4IngressPublishScalars`、`exportR4Ingress`、reset 与 `stop()` 与重做 profile 三条路径的撤销顺序 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Cx3Mlx4UserClient.h` / `.cpp` | 分发表第 36 行（selector 35）、锁类别表、`sR4Ingress` / `doR4Ingress` |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/tools/cx3ctl.c` | `cmdR4Ingress`、`sizes` 表第 36 行、`r4ingress` 子命令与 usage |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/tools/verify.sh` | 15 个新索引名解析 + `r4qp` 之后的链路步骤（MAC 三层 + 入口 QP 四层） |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/kext/Info.plist` | 版本 0.43.2 → **0.43.3** |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/docs/COVERAGE.md` | `r4ingress` 行（链内，证据栏"未运行"）、计数 21→22、版本号同步到 0.43.3 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/docs/DEFECTS.md` | 新行 **D11** 与 **E9**、总数 53→54、D 家族 10→11 |
| `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/docs/R4-STAGE5-RESULT.md` | 本文件 |

未提交：工作区留着本次改动（`HEAD` 仍是 `c780753`），回滚方法见第 8 节。

---

## 2. 两个半边，四种证据，为什么它们不能混着数

方案 §4 要求五层判据，每一层都要**从值读出来**。实现时把它们拆成了两组，因为
这两个半边的"读回"性质不同：

| 层 | 标量 | 通过条件 | 为什么它算证据 |
| --- | --- | --- | --- |
| MAC 命令 | `kR4IngressOutMacStatus` | `0` | 命令被接受了 |
| MAC 槽位 | `kR4IngressOutMacSlot` | `0` | 槽位是**扫出来的第一个空槽**（见 §3.2），不是写死的 |
| **MAC 回读** | `kR4IngressOutMacReadbackOk` / `ReadbackMatch` | `1 / 1` | 固件把表还回来了，且槽 0 与发出的 8 字节逐字节相同 |
| 入口三步 | `kR4IngressOutStepsAccepted` / `StatesConfirmed` | `3 / 3` | 与阶段 4 同一条：命令成功 ≠ 状态被接受 |
| 入口状态 | `kR4IngressOutQueryOk` / `QueryState` | `1 / 3` | 最后那次 `QUERY_QP` 说它在 RTS |
| **入口指向** | `kR4IngressOutRssDefaultMatches` / `RssBaseMatches` | `1 / 1` | 固件持有的 `default_qpn` 与 `base_qpn` 都是阶段 4 的 RX QPN |
| 不指向自己 | （由 `Qpn` ≠ `QueryDefaultQpn` 判） | 两者不同 | 计划点名的失败模式：入口指向自己 |
| 基准值 | `kR4IngressOutRxQpn` | 等于阶段 4 的 QPN | 上面两条比较的是**它**，所以它必须一起报出来，否则"相等"是一个没有参照物的断言 |

**MAC 回读这一层的特殊之处**，也是本阶段最需要读者注意的地方：**参考从不回读这张表**。
全树 `grep get_port_mac_table` 零命中；`mlx4_port.c` 里只有 `mlx4_set_port_mac_table`（只写），
`MLX4_SET_PORT_MAC_TABLE` 只在一处出现（`:140`）。参考自己在软件里维护 `entries[]`/`refs[]`/
`total`（`mlx4_port.c:182-340`）并信任自己那份，**没有任何代码问过固件"你存的是什么"**。

所以"再发同一命令但不带 mailbox（`in_param = 0`）"是**本移植自己的探针**，不是参考的做法，
依据只有命令族的惯例（输入走 mailbox 的命令常以同一 mailbox 作答）。这一个区别在失败时
很重要，因此三种结果被分开表示：

| 回读结果 | 表示 | 判据 |
| --- | --- | --- |
| `ReadbackOk = 0` | **回读不可用** | 退化为"命令状态"；不判 FAIL，也不假装 PASS（`verify.sh` 报 SKIP 并说明）。它**不**等于"MAC 没注册"——那两个是不同断言 |
| `ReadbackOk = 1, Match = 0` | 固件答了别的表 | FAIL：这是关于设备的事实 |
| `ReadbackOk = 1, Match = 1` | 固件持有这张表 | 本阶段唯一能拿到的双向证据 |

---

## 3. 编码：照抄的那些值与来源

### 3.1 MAC 表（`mlx4_port.c:127-146`）

| 项 | 值 | 依据（核对后） |
| --- | --- | --- |
| opcode | `MLX4_CMD_SET_PORT = 0xc` | `cmd.h:63` |
| `op_mod` | `MLX4_SET_PORT_ETH_OPCODE = 0x1` | `cmd.h:195` |
| `in_modifier` | `MLX4_SET_PORT_MAC_TABLE << 8 \| port` = `0x2 << 8 \| 1` = **`0x201`** | `cmd.h:203`、`mlx4_port.c:140` |
| `in_param` | **mailbox 的 DMA 地址**，内容就是**整张表** | `mlx4_port.c:138` |
| 超时 | `MLX4_CMD_TIME_CLASS_B` | `mlx4_port.c:142` |
| 表大小 | `MLX4_MAX_MAC_NUM = 128` 项 × 8 字节 = **`MLX4_MAC_TABLE_SIZE = 1024`** | `driver.h:732-733` |
| 每项 | `cpu_to_be64(mac \| MLX4_MAC_VALID)`，`MLX4_MAC_VALID = 1ull << 63` | `mlx4_port.c:299`、`:48` |
| 地址掩码 | `MLX4_MAC_MASK = 0xffffffffffff`（低 48 位） | `driver.h:40` |

### 3.2 MAC 的字节序，以及为什么不走 host-u64 那条路

**MAC 取自 `QUERY_PORT` 的 `def_mac`（BE64 的低 48 位），VALID 位是 BE 首字节的 `0x80`。**

两个编码在这里相遇，差别是字节序，所以两边都写出来而不是相互转换：

* `defMac[]` 存的是响应 offset `0x10` 起**原样 8 字节**（`src/cx3_abi.h:836`），日志里
  `def_mac 00:11:22:33:44:55` 打的是 `defMac[2]`…`defMac[7]`（`Mlx4Hw.cpp` 的 queryPort 日志）
  —— 也就是说地址落在**后 6 字节**，`defMac[0]` 通常是 0；
* 表项要的是 `cpu_to_be64(mac | MLX4_MAC_VALID)`，而 `1ull << 63` 在**大端表示里就是首字节的
  最高位**，也就是 `entry[0] |= 0x80`。

实现（`r4IngressMacEntry`）因此是"复制 8 字节 + 只改第 0 字节的一位"，而不是先拼成 host `UInt64`
再 `cpu_to_be64`：后者需要**在两处**同时把字节序弄对（一次读响应、一次写 mailbox），而错一处
会静默地写出一个"看起来像 MAC 的数"——这正是本项目记录得最多的那类错误。

**槽位要求 index 0 的解释（以及没有声称的部分）**：实现按方案写成"**取第一个空槽并断言它是 0**"
—— 扫出来的，不是写死的。A0 下固件把单播 MAC 条目映射到 QP 的规则是 `base_qpn + mac_index`
（`mlx4_port.c:372-376`；A0 分支见 `mlx4_en_netdev.c:573-577`，而告诉固件 base 的是
`mlx4_SET_PORT_qpn_calc`，`mlx4_port.c:1588`，只对 A0 发命令），所以索引 0 就是映射到 base 本身
的那一项。**未声称**：固件**要求**条目落在 0。全树没有这句话，而且 A0 路径里至少有一处是
no-op（`mlx4_multicast_attach` 在 A0 下直接 `return 0`，`mlx4_mcg.c:1412-1415`）。所以这是一个
**写明了理由的选择**，而不是被观测到的要求。

### 3.3 RSS 入口 QP：三层叠加（`mlx4_en_rx.c:1016-1080`）

| 层 | 内容 | 依据 |
| --- | --- | --- |
| 底：fill | `flags = 7 << 16 \| rss << 13` ⇒ **`0x00072000`** | `mlx4_en_resources.c:49`、`qp.h:125` |
| | `rq_size_stride` **留 0**（`!is_tx && !rss` 为假） | `mlx4_en_resources.c:52` |
| | `sq_size_stride = ilog2(64) - 4 = 0x02` | `mlx4_en_resources.c:55-57`、`en.h:84` |
| | `mtu_msgmax = 0xff`、`usr_page` = CQ 那个 UAR、`counter_index = 0xFF`、`ackto = 1`、`sched_queue = 0x83` | `:50`、`:58`、`:66`、`:60`、`:61` |
| | `db_rec_addr = res.db.dma << 2`（**左移**，decision 2） | `:78` |
| 叠加：RSS @ QPC+`0x3c` | `base_qpn = ilog2(rings) << 24 \| base_qpn` | `:1070-1071` |
| | `default_qpn = base_qpn`（**= 真正的 RX QPN**） | `:1072` |
| | `reserved = 0`、`hash_fn = MLX4_RSS_HASH_TOP = 1` | `qp.h:128-137`、`:1074` |
| | `flags = IPV4\|TCP_IPV4\|IPV6\|TCP_IPV6 = 0x3c` | `:1005-1013`、`qp.h:110-115` |
| | `rss_key[10]` = 40 字节 | `:1075`、`qp.h:127` |
| | `base_qpn_udp` 只在 `udp_rss` 为真时填（本阶段为 0） | `:1072-1073` |
| 迁移 | 与阶段 4 同样的三步 `mlx4_qp_to_ready`，MTT 只在 RST→INIT 写 | `:1076-1078`、`mlx4_qp.c:899-930` |

偏移的来路：`rss_context = (u8 *)&context + offsetof(pri_path) + MLX4_RSS_OFFSET_IN_QPC_PRI_PATH`
（`:1067-1069`），`pri_path` 在 `0x18`（`qp.h:140`，本项目已因 `counter_index` 0x1c 与
`sched_queue` 0x38 依赖它），RSS 偏移 `0x24`（`qp.h:117`）⇒ **QPC + `0x3c`**。这是对同一个
248 字节 context 的**字段级叠加**，不是另一条命令：RSS context 走同一份 mailbox。

**MQTT（`mtt_base_addr_l`）指向 RX ring 那一份**：参考给入口 QP 传的也是 `priv->res.mtt`
—— rx qps 那一次整体分配（`mlx4_alloc_hwq_res`，`mlx4_en_netdev.c:2236`）——而不是入口 QP
自己的 MTT（`mlx4_en_rx.c:1076-1078`）。本阶段照做：入口 QP 的 context 写阶段 3 的 ring 的
MTT 条目。这是本阶段唯一一处与阶段 4 共享的东西，而它不是笔误：**那个字段属于 RST→INIT
那一步，而固件不预期为一条不收包的 QP 去跟随它**。

**RSS key 的来源**：参考向宿主栈要（`rss_getkey`，`mlx4_en_rx.c:1003`），本移植没有栈可问，
所以 `r4IngressBuildKey` 造一个固定图样（第 i 字节 = `0x40 + i`）。单环下 key **不承重**
（只有一个目标），选图样而不是全 0 的理由是**可测量性**：40 个零字节读回来和"这个字段没人写"
长得一模一样，图样能让部分写入或错位写入显形。多环里程碑里 key 会变成承重项（流的两端必须
哈希到同一条队列），那时必须换成宿主栈提供的 key——记在这里，免得到时当作惊喜。

---

## 4. 实现里值得记住的三件事

### 4.1 `rq_size_stride = 0` 是照抄，不是漏写

阶段 4 的这个字节是 `0x40`；入口 QP 是 **0**。参考的条件是 `if (!is_tx && !rss)`
（`mlx4_en_resources.c:52`），所以 RSS 入口 QP 根本不发这个字段。原因也清楚：入口 QP
**没有接收队列**，没有 WQE 被投递到它、它也不产生完成，它的职责只是"哈希的目标"。
把这个字节写成真实 ring 的尺寸，等于宣称一条不存在的队列。`qpFillContext` 的 `rss` 分支
因此**不写它**，并在日志里点名说"这不是阶段 4 的 0x40"。

### 4.2 入口 QP 的 QPN 是"RX QPN + 1"，且这个 +1 是一个选择

`pointsAtAnotherQp` 是计划点名的那个失败模式（"入口指向自己"）的数值化形式：这种 QP 每条命令
都通过、能到 RTS、字段全部匹配，然后把每一个哈希帧丢进一条没有接收环的队列，**输出里没有
任何东西会提示这件事**。所以入口 QP 的号码取所依赖的 RX QPN 的**下一个**，并在取号前先检查
两个号码都落在软件可用区间内（下界 `fDevCap.reserved_qps`，上界 profile 配置的
`1 << log_num_qps`——与阶段 4 同一条规则）；越界就拒绝并说明"提高 profile 的 QP 数才是修法，
不是复用 RX QP 的号码"。

`+1` 本身是**本移植的选择**：参考用 `mlx4_qp_reserve_range` 拿一段范围，本移植没有范围
管理器。这句话写在代码注释里，因为下次有人看到 `rxQpn + 1` 时应该知道它是选择而不是抄来的。

### 4.3 幂等路径要**重新推导**，不能记；这条在实现时抓到一个真 bug

第二次调用不能在固件已经持有 QP 的情况下再建一条、也不能把第一条的记录忘了——第一次的记录
才是它 context 里那个。所以已存在的 QP 走**重新测量**：`QUERY_QP` 与记录读回在幂等路径上
照跑，因为一条只报告记忆值的路径发现不了设备已经变了。

我这个分支的第一稿里有一行 `res->sqSizeStrideSent = res->sqSizeStrideSent;`（自赋值，等于
no-op），于是这个字段会保持 `memset` 后的 0，比较时拿固件回读的 `0x02` 去比"记住的" `0x00`，
**layer 2 会因为一个这次调用根本没发过的字段报 FAIL**。它和 D8 是同一个形状（幂等路径报告了
没有被测量的值），只是伤害方向反了。已记进 `docs/DEFECTS.md` **D11**：现在五个字段都从 fill
用的同一批输入重新推导（WQE 尺寸、CQN、UAR、端口、设备自己的 `FCS_KEEP` 位），注释说明
为什么是推导而非重放；固件那一侧的值仍旧来自 `QUERY_QP`。

---

## 5. 三个拍板项的落地方式与依据

### 5.1 决策①：MAC 注册照做

它是"固件认不认这个地址"的唯一开关，而阶段 10 要交付协议栈。代价是一条 `SET_PORT`
（1 张 1024 字节表）+ 一次回读。

### 5.2 决策②：`db_rec_addr` 用左移那份 —— 语义**已经查清**，结论是"死字段"

方案里写"`<< 2` 的语义未查清"，**这一句已经被取代**。查清的结果是三处对照：

| 站点 | 写进 `db_rec_addr` | 产生完成事件？ | 驱动写那条 record？ |
| --- | --- | --- | --- |
| CQ：`mlx4_cq.c:284` ← `mlx4_en_cq.c:158` 传 `cq->wqres.db.dma` | **不左移** | 是 | 是（`set_ci_db`，`mlx4_en_cq.c:122`） |
| RX ring QP：`mlx4_en_rx.c:947` | **不左移**（阶段 4 已按此实现并 PASS） | 是 | 是（`mlx4_en_rx.c:253`） |
| **入口 QP**：`mlx4_en_resources.c:78` | `priv->res.db.dma << 2` | **否** | **从不** |

* `.dma` 是**真实字节 DMA 地址**：`mlx4_alloc.c:725` 是 `db->dma = pgdir->db_dma + db->index * 4`，
  每 index 4 字节。所以 `<< 2` 真的把一个字节地址乘了 4；若固件去解引用，会读偏 4 倍。
* `priv->res.db.db`（那个指针）在全树**任何地方都没被写过**——`grep 'res\.db\.db'` 只命中 CQ 与
  ring 的 `wqres`，不是 `res`。
* `priv->res` 本身是**真分配**的（`mlx4_en_netdev.c:2236` 的 `mlx4_alloc_hwq_res(..., &priv->res, ...)`，
  内部 `mlx4_db_alloc`），所以不是"没分配所以是 0"。
* 且入口 QP **不产生完成事件**，它的职责只是把哈希分流指向 `base_qpn`/`default_qpn`。

⇒ **两边都是死的**：固件读它没有意义，驱动也从不写它。

**照参考写左移那份，不"修正"它**，理由是证据站哪边的问题而不是口味：生产环境的 `mlx4_en`
**就是带着 `<< 2` 工作的**（经验证据，说明这个值在这个字段里无害）；而"它错了"的前提是
"固件要字节地址"，可这个字段的语义**没有文档**。所以照抄是有证据的选择，"改成不左移"是
没有任何证据支撑的假设——而阶段 5 的目标是让分流生效，不是顺手改参考。

代码里的处理：`res->dbRecAddrShifted = 1`，`queryDbRecAddr` **只报告不判定**（不参与
`queryRssMatches`），日志把"发出的值"和"固件持有的值"并排打出来。

**证伪条件（保留）**：若阶段 9/10 出现 RSS 相关的收包异常，这是**第一个**要反向试的字段；
若入口到不了 RTS 且唯一可疑字段是它，改成不左移重试。

### 5.3 决策③：`rss_rings = 1`，高位编码本阶段验不到

单环 ⇒ `ilog2(1) = 0` ⇒ `base_qpn` 高字节为 0，`base_qpn` 与 `default_qpn` **相等**且都指向
阶段 4 的 RX QPN。好处是判据干净（两个字段相等且都等于 RX QPN）；代价是**多环才会用到的
高位编码在本阶段不被验证**——属于 M5。这条同时写在 `qpFillRssContext` 的日志里（`high == 0`
时专门打一行）和 ABI 注释里。

`base_qpn_udp` 只在 `udp_rss` 为真时填（`mlx4_en_rx.c:1072-1073`）。本移植的 profile 没有
UDP RSS，所以传 `false`、字段保持 0——这是参考的做法，而不是本移植自己编的值。

---

## 6. 失败时的分岔判读

| 症状 | 先查什么 |
| --- | --- |
| MAC 命令 `fw status 0x3`（BAD_PARAM） | `in_param` 必须是 **mailbox 的 DMA 地址**（不是 0、不是表的地址），`in_modifier` 必须是 `0x201`。两者都在内核日志里逐行打印 |
| MAC 命令别的非 0 | `op_mod` 与 `in_modifier` 是**两个不同的 HCR 字段**，而两组枚举的值是撞的（`docs/DEFECTS.md` E9）。日志打印实际发出的 `in_modifier` |
| MAC 回读不答 | **这不是 FAIL**：回读是本移植的探针，参考从不回读。判据退化为写方向的 `fw status`，`verify.sh` 报 SKIP 并说明。**不要**据此宣布"MAC 没注册" |
| MAC 回读答了但不同 | 固件持有别的表。先确认槽位（日志里扫出来的那个）与 8 字节表项 |
| 入口第一步 `fw status 0x3` | ① `flags` 的 RSS 位（bit 13）在不在；② overlay 是否落在 `QPC+0x3c`；③ decision② 的 `<< 2`（这是它的证伪条件之一） |
| 入口三步成功但 `statesConfirmed < 3` | "`QUERY_QP` 自己没答"和"答了但不是那个状态"是两件事：前者查 `QUERY_QP` 编码，后者查 `flags[31:28]` 是否被覆写 |
| 状态对了但 `queryRssMatches = 0` | 日志并排打印 overlay 的每一对 sent/read（`base_qpn`、`default_qpn`、`hash_fn`、`flags`、40 字节 key、`reserved`、`base_qpn_udp`） |
| `pointsAtAnotherQp` 报"指向自己" | 入口 QPN 与 `default_qpn` 同一个数 ⇒ 取号逻辑被改了（应为 RX QPN + 1），或 `default_qpn` 没写进去 |
| 全绿但阶段 9/10 收不到帧 | 回来看 decision②（`<< 2`）、`rss_rings` 的高位编码、以及 R3 就记过的 `sched_queue` bit 0 被清这件事（`docs/R4-STAGE4-RESULT.md` §9.1） |

---

## 7. 实测 / 推断 / 未测

### 实测（本轮真的跑出来的）

* **构建门禁全绿**：`boot-path` PASS（`start()` 可达函数集无写、无 DMA、无 reset）·
  `146 + 8186 + 137 + 45` 项检查 0 失败 · 未定义符号 **352/352 全部解析**（与基线 352 相同：
  新增的全是内部方法，没有引入新的外部符号）· 四个 `-Werror` 档下零告警 ·
  `tools/check-hardcoded.py --strict` 通过。
* **kext sha256 = `74953e6704a1252afdb75a552610144eae1e54e48f9b18f87d11bb7945940668`**，
  308512 字节（`BUILD_TAG=0.43.3`）。**可复现**：同一份树**连续两次构建得到同一 sha**
  （`/tmp/cx3-ship-4` 与 `/tmp/cx3-ship-5`）。
* `cx3ctl sizes` 自证新行：**`35 0 15 368 r4ingress`**（0 标量入、15 标量出、结构 368 字节），
  这行是**编译器算的**（`sizeof(Cx3R4IngressResult)`），与 kext 的分发表同源。
* `verify.sh` 的 15 个新索引名在 `src/cx3_abi.h` 里全部解析成功（逐个用 `abi()` 的正则验过）。
* `bash -n tools/verify.sh` 通过（本机 bash 3.2）。
* 静态断言：分发表与锁类别表的长度（`== kCx3SelCount = 36`）、标量上限、三步数组长度、
  `mac[]`/`macReadRaw[]` 同宽，编译期全部通过。

### 推断（有源码依据、未上机）

* 上面 §3 的全部编码：逐条照抄参考，行号已核。
* **MAC 表回读可行**：这是**本移植的假设**，不是参考的做法（参考没有读路径）。若实测读不回来，
  判据退化为命令状态，`verify.sh` 会报 SKIP 而不是 FAIL。
* `<< 2` 不承重：依据是 §5.2 的四条（`.dma` 是真字节地址、驱动从不写那条 record、入口 QP
  不产生完成、参考在带 `<< 2` 的情况下工作）。这一条是**推断**，因为"字段不被读"没有直接观测。
* 槽位 0 的 QPN 映射：`base_qpn + mac_index` 是**源码结论**；但"固件要求条目在 0"**没有依据**，
  本阶段只是选择用 0 并把理由写下来。

### 未测（明确没做，或做不到）

* **`r4ingress` 从未运行**：kext 未安装、机器未重启。MAC 命令是否 `fw status 0x0`、回读是否
  可用、入口 QP 是否到 RTS、overlay 是否被固件回存，**全部未测**。`docs/COVERAGE.md` 里
  `r4ingress` 那行的证据栏就写着"未运行"。
* `tools/verify.sh run|full|safe` 未跑（会驱动硬件）。
* **`cx3ctl r4ingress` 的排版只在编译层面验过**，没对着真实结构打印过；§6 的分岔判读是设计
  意图，不是实测过的判读路径。
* 幂等路径（第二次调用）：代码写了、编译过了，没跑过。

### 我没能查清的点

1. **`db_rec_addr` 到底给谁读**：§5.2 证明了两边都是死的，但"固件永远不读它"仍是**推断**
   （依据是它不产生完成、驱动不写那条 record），不是观测。上机后唯一能间接验证的方式是
   阶段 9/10 的收包结果。
2. **`SET_PORT MAC_TABLE` 的读方向**：`in_param = 0` 会不会作答、答的是不是那张表，只有实测
   能说。参考没有这个用法，所以这不是"抄错了"，是"我们的探针成不成"。
3. **A0 下 `mac_index` 的作用**：`mlx4_set_port_qpn_calc` 只在 A0 发命令（`mlx4_port.c:1588`），
   而 A0 的 multicast 路径里已经有 no-op（`mlx4_mcg.c:1412-1415`）。所以"MAC 表的索引真的参与了
   单播 QP 选择"这件事，本阶段**没有独立证据**，只有源码里的 `base_qpn + index` 一条。

---

## 8. 回滚

阶段 5 是**加法**：一个新选择器 + 新常量 + 两个新 teardown 分支，不改既有路径的语义。唯一
动到既有行为的是三处 teardown 调用的**插入**（`exportResetHca`、`stop()`、`m2dprofile`），
三处都是"在既有调用之前加一次 `ingressTeardown`"，不改既有顺序。

1. **代码回滚**（本轮未提交，工作区是 `HEAD = c780753` 之上的改动）：

   ```bash
   git -C /Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4 status --short
   git -C /Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4 diff      # 先看
   git -C /Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4 checkout -- .   # 放弃本次改动
   ```

   若已提交成一条提交：`git -C tools/cx3-mlx4 revert --no-edit <该提交>`，然后重建、重装、重启。
   构建产物这一轮都写在 `/tmp` 下的临时目录里，**没有覆盖 `outputs/20261010-cx3-m1/build`**，
   所以仓库里没有需要清理的中间产物。

2. **运行期退路**：`sudo tools/cx3-mlx4/rollback.sh [--purge]`（未改动）。

3. **硬件侧残留**：阶段 5 会在设备里留下两样东西——**MAC 表里的一个条目**和**一条 RTS 状态的
   入口 QP**。回滚后它们仍在，直到 HCA reset（`m2dbringup` 之前的 `reset` 会清掉，这也是验证
   链条本来就做的事）。入口 QP 的那块 producer record 是本 driver 的主机内存，随 kext 卸载释放；
   **顺序要求**：先 `ingressTeardown`（2RST）再释放记录，否则固件还通过 context 里的
   `db_rec_addr` 指着那块内存。MAC 表**没有 unregister 命令**（参考的删除路径是重发整张表，
   `mlx4_port.c:415-430`），所以 `ingressTeardown` 只丢主机侧记录、不发命令，日志会明说这一点。

---

## 9. 用户重启后要跑的那条命令与期望输出

按项目惯例安装，然后跑：

```bash
sudo tools/cx3-mlx4/verify.sh stage      # 构建 + 校验 + 安装（不重启）
sudo reboot
sudo tools/cx3-mlx4/verify.sh run        # 重启后：整条链，含新的 r4ingress
```

只想单独看阶段 5（前提：同一 boot 里已经跑过 `m2dbringup`、`m3clrint`、`m3irqup`、`m3portup`、
`r4mr`、`r4cq`、`r4rq`、`r4qp`）：

```bash
sudo /usr/local/bin/cx3ctl r4ingress      # 不带参数：两半边都从设备自己取值
```

**期望（成功那一支）**：

```
  half one: the MAC table -- one SET_PORT carrying the WHOLE table
    address from QUERY_PORT's def_mac : 00:11:22:33:44:55
    the entry as sent (BE64)         : 80 00 ec 0d 9a 2a 15 11
      (byte 0's 0x80 is MLX4_MAC_VALID; the address itself is the BE64's low 48 bits, i.e.
       bytes 2..7 -- which is why the tool prints res.mac[2..7] as the address)
    table                            : 1024 bytes = 128 entries of 8
    slot                             : 0
    write direction                  : fw status 0x0 (accepted)
    read direction (this port's probe, not the reference's practice):
      answered                       : fw status 0x0
      entry 0 read back              : 80 00 ec 0d 9a 2a 15 11
      verdict                        : the firmware holds the table this driver sent

  half two: the RSS indirection QP
    QPN                  : 65  (the RX QP's 64 + 1)
    the hash target      : default_qpn 64 (the RX QP of stage 4), base_qpn 0x00000040
    rss_rings            : 1, so ilog2 = 0, so base_qpn's HIGH BYTE is 0 ...
    overlay at QPC+0x3c  : hash_fn 1 (TOP), flags 0x3c ..., rss_key 40 bytes, base_qpn_udp 0
    db_rec_addr          : record at 0x3b77f9000, SENT as 0xede1fe4000 (the reference's `<< 2`)
    rq_size_stride sent  : 0x00

    layer 1: the three transitions, each followed by its own QUERY_QP
      RST2INIT (0x19): fw status 0x0 ok ;  state after: 1 (wanted 1) ok
      INIT2RTR (0x1a): fw status 0x0 ok ;  state after: 2 (wanted 2) ok
      RTR2RTS  (0x1b): fw status 0x0 ok ;  state after: 3 (wanted 3) ok
    layer 2: the last QUERY_QP against what was written
      flags 0x30070000 -> state 3 ...  default_qpn 0x00000040 ...
      does the entry QP point at ITSELF?  default_qpn 64 vs its own QPN 65 : no
```

`verify.sh run` 里对应的是这几行 PASS（MAC 回读**可用**时 5 行，不可用时 4 行 PASS + 1 行 SKIP）：

```
MAC: SET_PORT MAC_TABLE accepted (fw status 0x0), slot 0                          PASS
MAC: the slot is 0, the first empty entry of the table that was sent             PASS
MAC read-back: the firmware holds the entry this driver sent, byte for byte      PASS
ingress QP: QPN 65 above the RX QPN 64, inside the range software may use        PASS
ingress layer 1: 3/3 commands accepted, 3/3 states read back after each step     PASS
ingress layer 2: the entry QP is in RTS                                          PASS
ingress layer 2 (the hash target): default_qpn and base_qpn are both the RX QPN 64  PASS
ingress: the entry QP does not point at itself (QPN 65, hash target 64)          PASS
```

**失败时的分岔**：见第 6 节的表。最需要注意的一条是 **MAC 回读不答**——那一行是 SKIP 而**不是**
FAIL，判据退化为写方向的 `fw status`；不要把它读成"MAC 没注册"。

---

## 10. 真机实测（0.43.3，2026-10-12 07:16）：40 PASS / 2 FAIL / 1 BLOCKED

**本节在第 1–9 节写完之后追加，不改动它们任何一条结论。** 第 9 节的"期望输出"有三行被这次实测
推翻，逐条列在 §10.5；第 7 节的"未测"清单里也有两条因此过期（§10.6）。

跑法是第 9 节那条命令（`verify.sh stage` → 重启 → `verify.sh run`），完整记录：
`tools/cx3-mlx4/outputs/verify-20261012-071629.log`。汇总是
**40 PASS / 2 FAIL / 1 BLOCKED**（BLOCKED 是 M4 gate 的 soak，接口层还没做，如第 2 节所述）。

### 10.1 入口 QP 那一半：四层全过，值与第 9 节预期一致

| 层 | 实测值 | 判定 |
| --- | --- | --- |
| 三步命令 | `3/3 commands accepted` | PASS |
| 每步后状态 | `3/3 states read back after each step` | PASS |
| 最终状态 | `QUERY_QP` 报 state 3（RTS） | PASS |
| hash 目标 | `default_qpn` 与 `base_qpn` 都是 **64**（阶段 4 的 RX QPN） | PASS |
| 不指向自己 | 入口 QPN **65**，`default_qpn` 64 | PASS |
| overlay（QPC+0x3c） | `hash_fn 1`、`flags 0x3c`、`rss_key` 40 字节**逐字节**回来、`reserved 0`、`base_qpn_udp 0` | PASS |

即：**本阶段的核心判据（入口 QP 在 RTS、且它的 hash 目标是收包那条 RX QP，不是它自己）在真机上
成立**。`rss_rings = 1` 导致的高字节编码仍未验到（§5.3 已记，属 M5）。

### 10.2 两条 FAIL：同一个 bug 的两面

两条都出在 MAC 那一半，而它们是**一个根因**：

```
r4ingress: the slot is the FIRST EMPTY one in the table being sent: 1 (of 128 entries; ...)
setPortMacTable: slot 1 entry bytes (BE64, as they go on the wire): 80 00 ec 0d 9a 2a 15 11
r4ingress: the MAC table read-back ... entry 1 holds 00 00 00 00 00 00 00 00 against the
           80 00 ec 0d 9a 2a 15 11 sent -- MISMATCH: the firmware answered with something else
```

**面①（槽位）**：调用方先把地址写进**槽 0**，**然后**才去扫"第一个空槽"——槽 0 刚被写上，
所以扫描必然跳过它、返回 **1**。于是日志与标量都报 1，而 `setPortMacTableWrite` 拿到的
"一条 entry + 一个槽号"又是**它自己**把 entry 复制到 `fMboxVirt + slot*8` —— **实际发出的表是
"槽 1 有 MAC、槽 0 空"**，调用方构造的那 1024 字节里只有前 8 字节被用到，其余 1016 字节白构造。
**这不是标签写错**：A0 下固件把单播 MAC 条目映射到 QP 的规则是 `base_qpn + mac_index`
（`mlx4_en_netdev.c:573-577`），槽 1 ⇒ **另一个 QPN**，即把地址注册给了不是收包那条的 QP。
验证器报 `slot=1, expected 0`，**验证器是对的**（详见 `docs/DEFECTS.md` D12）。

**面②（回读判据）**：`setPortMacTableRead` 回 **`fw status 0x0`**，但 outbox **全 0**，而验证链
把它判成了 `FAIL the firmware answered with a DIFFERENT table`。**这个判定没有证据**：
"固件存了一条全 0 的 entry"和"固件根本没写 outbox"从这里**无法区分**，而后者才与"命令报成功"
相容。全 0 的 outbox 不是一张表 ⇒ 判据只能是**回读不可用**（SKIP），不是"它存了别的"。
这一条是本项目 F/D 家族的形状：**一个没有任何测量支撑的断言**，而且它出现在验证器自己身上。

### 10.3 修法（0.43.4）

1. **双份构造消失**：`setPortMacTableWrite(UInt32 port, const UInt8 *table, UInt32 tableBytes,
   UInt32 *outFwStatus)` —— 硬件层把整张表**逐字节**放进 mailbox；`slot` 参数**取消**，所以
   "发出的表"与"调用方构造的表"是同一份字节，槽位不可能有第三种说法。`tableBytes` 必须是整张
   `MLX4_MAC_TABLE_SIZE`，因为这条命令是**替换**固件那张表，不是改其中一条。
   代价：调用方要自己保证表是整张且内容自洽——它本来就在做这件事，只是以前没人检查。
2. **槽位的选定与断言**：槽位改为**先决定、再落笔**（`slot = 0`），并且**从"即将发出的那张表"里
   把它量回来**（第一个非空 entry）。量出来与决定不符、或整张表没有非空 entry ⇒ **拒绝发送**
   （日志写明两个数字）。于是**标量报的数就是固件收到的数**：以后若落笔位置变了，这个数跟着变，
   验证链的 `slot=0` 就会失败——这次那个"1"不会再藏在一句注释后面。
   代价：多一次 128×8 字节的扫描（一次调用，可忽略）。
3. **回读判读改成三态**：命令不答 **或** 答了但 outbox 全 0 ⇒ `macReadbackOk = 0`（回读**不可用**，
   `verify.sh` 报 **SKIP**，并说明是哪一种，内核日志区分）；只有**回了非零内容且与发出的不同**
   才是 **FAIL**。`cx3ctl r4ingress` 也据此多打印一支 "answered with NO CONTENT"。
   这条同时写进了 `verify.sh` 的措辞与 `docs/DEFECTS.md` D12。

### 10.4 "验证器抓到了这个 bug"这件事本身值得记

这是本项目**第一个在真机上被验证器抓到的、而不是被读代码读出来的**缺陷，抓到的方式正是
`tools/verify.sh` 存在的理由（文件头那句 "decisions come from VALUES"）：

- 驱动侧**每一行输出都是自洽的**——槽位 1、那条 entry 的 8 字节、`fw status 0x0` 全都真实；
- 只有把**标量**（`kR4IngressOutMacSlot`）拿去看**验证器自己写的期望值**（0）时，矛盾才出现；
- 而"槽 0 才对"这件事的理由（`base_qpn + mac_index` ⇒ 索引即 QPN 偏移）本来就是**源码级结论**，
  写下它和读它的是同一次思考，所以靠读代码必然放过。

一句话：**判据从值来，让"发现不了自己写错"这件事有了出路。** 这也是 D12 那一行里
"验证器是对的"要单独写出来的原因——把槽位写成"扫出来的第一个空槽"**曾经**被认为更可检查，
而它恰恰把缺陷藏进了一句听起来精确的注释。

### 10.5 第 9 节的期望输出里有三行被实测推翻

| 第 9 节写的 | 实测（0.43.3） | 结论 |
| --- | --- | --- |
| `slot : 0` | `slot 1`（内核日志与标量都报 1） | **驱动错了**，见 §10.2 面①、D12 |
| `read direction ... entry 0 read back : 80 00 ec ...` | `fw status 0x0` 但 outbox **全 0** | 零回读 ⇒ 该判 **SKIP**，见 §10.2 面② |
| `verdict : the firmware holds the table this driver sent` | 无可比内容 | 同上；0.43.4 起是 SKIP 而不是 PASS/FAIL |

其余期望（入口 QP 的 QPN 65、RTS、overlay 逐字节）**与实际一致**。

### 10.6 第 7 节的"未测"清单：两条已过期，两条仍成立

- **已过期**：`r4ingress` 跑过了；`verify.sh run` 跑过了；`cx3ctl r4ingress` 的排版对着真实结构
  打印过了。
- **仍成立**：**幂等路径（第二次调用）仍未跑过**——验证链只调用一次 `r4ingress`，上机证据只能
  覆盖首次路径。

### 10.7 固件不回存的两个字段（indirection QP，记档但不改代码）

**实测（同一次运行，0.43.3 07:16）**，`QUERY_QP` 对入口 QPN 65 的回读与写进去的值：

| 字段 | 写进去 | 回读 | 说明 |
| --- | --- | --- | --- |
| `cqn_recv` | `128` | **`5066319`** = `0x004D504F` | 字节 `4D 50 4F` = ASCII `"MPO"` |
| `db_rec_addr` | `0x115caec000`（参考的 `<< 2` 形态） | **`0x404040414240`** | 字节 `40 40 40 41 42 40` = `"@@@AB@"` |
| 同一次回读的其余字段 | `local_qpn 65`、`sq_size_stride 0x02`、`mtu_msgmax 0xff`、`rq_size_stride 0x00` | 与写入一致 | 内核日志里那段 written/read 逐字可核 |

两个回读值都长得像**固件的标记/填充**（一个是 ASCII 串、一个是 `0x40` 系图案），而不是任意残值；
但本移植**没有**任何源码依据说明它们是什么，所以这里只记事实、不下结论。

**这是与阶段 4 同一个形态**：阶段 4 的 RX QP 有两个字段（`param3` 写 `0x20000000` 读回 `0x0`、
`sched_queue` 写 `0x83` 读回 `0x82`）**不参与判定、只并排报告**（`docs/R4-STAGE4-RESULT.md` §9.1，
以及 `docs/DEFECTS.md` F10 记的那次"措辞不完整"）。本次对入口 QP 照同一做法处理：`verify.sh`
在入口 layer 2 的 PASS 之后加两行 `echo`，明说 `cqn_recv` 与 `db_rec_addr` **是报告而非判定**，
并把它们并排打在内核日志里。**注意：`cqn_recv` 仍在 `queryAllFieldsMatch` 的掩码里**（所以内核会
打一行 "the readback does NOT match what was written"），但**没有任何标量把它喂给验证器**，
所以那两条 FAIL 都不是它产生的——这一点必须说清楚，否则"回读不匹配"这句话看起来像第三处故障。

**是否承重**：

- **`cqn_recv`：预期不承重。** 入口 QP 是 RSS 分流目标，**不产生完成事件**（§4.1：它没有接收队列、
  没有 WQE 投递给它），所以它的 `cqn_recv` 指向哪条 CQ 都不会有人读；RX QP 自己的 `cqn_recv`
  是另一个上下文，阶段 4 实测 `128/128` 一致，不受这里影响。
- **`db_rec_addr`：预期不承重。** 同上，没有消费者会推进那条 record；§5.2 已论证这个字段
  "两边都是死的"（固件不读、驱动不写那条 record），本次实测**加强**了这个推断——固件连回存都没做，
  而 QP 仍然到了 RTS。
- **证伪条件**：① 若阶段 6–10 出现**收包/完成事件异常**（尤其是完成事件丢失或 RSS 相关丢帧），
  这两个字段是**第一个**要反向试的对象：把 `db_rec_addr` 改成不左移、把 `cqn_recv` 改成别的编号，
  各试一次；② 若入口 QP 在某个改动后**到不了 RTS**，且唯一可疑字段是这两个之一，同样先试它们；
  ③ 若多环（M5）下测出"某个流被哈希到错误的队列"，先排除这两个字段被固件归一化这一可能。

---

## 11. 给阶段 7 的输入：MAC 表索引 → QPN 的映射**本阶段无法确立**

这一节的目的是**阻止一个错误结论被带进阶段 7**。§3.2 论证过"A0 下固件按 `base_qpn + mac_index`
把单播条目映射到 QP"，那是源码事实；**但"槽 0 映射到我们的 RX QPN 64"不是**。逐条列事实：

1. `mlx4_get_base_qpn()` 用的是 **`reserved_qps_base[MLX4_QP_REGION_ETH_ADDR]`**
   （`vendor/freebsd-src/mlx4_core/mlx4_port.c:372-375`）。
2. `reserved_qps_base[]` 是在 **`mlx4_qp.c:777-794`** 里**从 `num_qps`（本卡 1024）顶部往下**逐个
   减去各 region 的 count 算出来的；ETH_ADDR 的 count 是
   `(1 << log_num_macs) * (1 << log_num_vlans) * num_ports`（`mlx4_main.c:545-548`）。
   ⇒ **ETH_ADDR 的 base ≠ 64**，而且**这个基址本移植算不出来**：上面四个 devcap 字段我们的解码
   一个都没有（只解了 `reserved_qps`）。
3. 我们读到的 **64** 是 devcap 的 `reserved_qps` 字段，内核把它记成
   `reserved_qps_cnt[MLX4_QP_REGION_FW]`（`mlx4_main.c:544`）—— **FW region 的计数**，同时也是
   A0 的 DMFS 基址 `dmfs_high_rate_qpn_base`（`mlx4_main.c:554-557`）；配合 A0 的 range 256
   （`MLX4_A0_STEERING_TABLE_SIZE`，`mlx4_main.c:568`）⇒ `64..319` 是 A0 的 flow-steering
   QPN 区间。
4. 所以本阶段的结论只能是：**"槽位 0 是这张只有一个地址的表的第一条"这个理由成立**；
   **"槽 0 的 QPN 是（或不是）我们的 RX QP 64"两条都不成立**——它是**未确立**的。阶段 5 的判据
   （入口 QP 在 RTS 且 hash 目标是 RX QP）**不依赖**它，所以这**不是**阶段 5 的失败。
5. **为什么这对阶段 7 是关键**：A0 下让 MAC 真正生效的是 `SET_PORT qpn_calc` 把 base 告诉固件
   （`mlx4_port.c:1578-1600`；调用点传的是 `priv->base_qpn`，`mlx4_en_netdev.c:782-785`、`:835`），
   而那个 base 该取自 `mlx4_get_base_qpn()`（`mlx4_en_netdev.c:574-575` 的 A0 分支正是
   `*qpn = base_qpn + index`）。**也就是说阶段 4/5 选的 QPN 空间在 A0 下可能需要重做**：阶段 7
   要么把 devcap 的 `log_num_macs` / `log_num_vlans` / `num_ports` / `num_qps` 解出来自己算 base，
   要么显式声明"本移植不用 A0 的 MAC 索引→QP 映射"，并说明收包路径靠什么。
6. 顺带一条对阶段 4/5 表述的修正：`res->qpnRule = fDevCap.reserved_qps` 与内核日志里的
   "the firmware's reserved share" 读的是**FW region 的计数**，不是"软件可用起点"的完整口径
   （参考里 `dev->caps.reserved_qps` 是四个 region count 之和，`mlx4_main.c:576-580`）。本阶段拿它
   当**下界**用（RX QPN 与入口 QPN 都必须 ≥ 它），这个用法不受影响，但"它是固件保留的起点"这句话
   不够精确，按上面更正。
