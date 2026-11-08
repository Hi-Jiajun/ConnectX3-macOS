# R4 阶段 5（`r4Ingress`）：MAC 注册 + 单环 RSS 入口 QP

实现前的方案。编码全部带 `vendor/freebsd-src` 行号；本卡实测值标出来源。
两份待决定的取舍在第 5 节。

阶段 5 的判据（`docs/M2B-NOTES.md` §27.2）：**MAC 表命令成功；入口 QP 到 RTS；
`base_qpn` 与 `default_qpn` 都指向真正的 RX QP。**

---

## 1. 这一阶段要做的两件独立的事

1. **把本卡的 MAC 写进端口的 MAC 表**（这一步决定"固件认不认这个地址"）。
2. **建 RSS 入口（indirection）QP**，让"按哈希分流"有一个目标，且它指向阶段 4 那个
   已经在 RTS 的 RX QP。

两者都不产生完成事件，所以都**不能靠"有没有中断"来验证**——判据必须是命令状态 + 回读。

## 2. MAC 表：编码与一个容易搞错的地方

来源 `mlx4_port.c:127-146`（`mlx4_set_port_mac_table`）、`:299`、`:48`。

| 项 | 值 |
| --- | --- |
| opcode | `MLX4_CMD_SET_PORT = 0xc`（`cmd.h:63`） |
| `op_mod` | `MLX4_SET_PORT_ETH_OPCODE = 0x1`（`cmd.h:195`） |
| `in_modifier` | `MLX4_SET_PORT_MAC_TABLE << 8 \| port` = **`0x2 << 8 \| 1 = 0x201`**（`cmd.h:203`、`mlx4_port.c:140`） |
| `in_param` | **mailbox 的 DMA 地址**，内容就是**整张表**（`mlx4_port.c:138`） |
| 超时 | `MLX4_CMD_TIME_CLASS_B`（注意：**不是** A，也不是 C） |
| 表大小 | `MLX4_MAX_MAC_NUM = 128` 项 × 8 字节 = **`MLX4_MAC_TABLE_SIZE = 1024` 字节**（`driver.h:732-733`） |
| 每项 | `cpu_to_be64(mac \| MLX4_MAC_VALID)`，`MLX4_MAC_VALID = 1ull << 63`（`mlx4_port.c:48`、`:299`） |

**容易搞错的地方**：命令发的是**整张表**（1024 字节，128 项），不是一条 MAC。
所以"注册一个 MAC"在这里的实现是：把表清零、在目标槽位放 `mac | VALID`、**整表下发**。
参考在软件里维护了 `entries[]` / `refs[]` / `total` 的完整记账（`mlx4_port.c:182-340`），
本阶段只有一条 MAC，不需要那套记账，但**表必须是完整的 1024 字节**。

### 2.1 MAC 从哪里来

**必须从设备读，不能写死**（本项目硬规则）。阶段 3 的日志已经打印过它：
`queryPort: port 1: ... def_mac 00:11:22:33:44:55`（`QUERY_PORT` 的 `def_mac` 字段）。
所以 MAC 取自 `QUERY_PORT` 的响应，**不是**常量，也不是从别处抄一个。

### 2.2 槽位要求：index 0

计划里写着"要求 index 0"。理由要在实现时核实：A0 steering 下固件按 MAC 表索引做匹配，
而条目 0 是它默认使用的那个（`MLX4_MAC_TABLE_SIZE` 与 `A0_STEERING_TABLE_SIZE = 256` 是两套
不同编号空间，别混）。**实现时把这条写成"取第一个空槽并断言它是 0"**，而不是"直接写槽 0 不解释"。

## 3. RSS 入口 QP：三层叠加

来源 `mlx4_en_rx.c:1016-1080`（`mlx4_en_config_rss_steer`）、`mlx4_en_resources.c:46-78`、
`qp.h:113-130`。

入口 QP 的 context **不是**阶段 4 那个 context 的简单复用，它是三层的叠加：

1. **底**：`mlx4_en_fill_qp_context(priv, 0, 0, 0, rss=1, base_qpn, rx_cqn, -1, &context)`
   ——注意第 2/3 参数是 `size=0, stride=0`，第 5 个是 `rss=1`：
   * `flags = 7 << 16 | rss << MLX4_RSS_QPC_FLAG_OFFSET`，`MLX4_RSS_QPC_FLAG_OFFSET = 13`
     （`qp.h:125`）⇒ **`flags = 0x00070000 | (1 << 13) = 0x00072000`**；
   * `rq_size_stride` 因 `!is_tx && !rss` 为假而**不写**（留 0）——这与阶段 4 的 `0x40` 不同；
   * `sq_size_stride` 走 else 分支 = `ilog2(64) - 4 = 0x02`；
   * `db_rec_addr = priv->res.db.dma << 2`（**左移那份**，见第 5 节决策②）。
2. **RSS 叠加**：`rss_context = (u8 *)&context + offsetof(pri_path) + MLX4_RSS_OFFSET_IN_QPC_PRI_PATH`
   ⇒ `0x18 + 0x24` = **QPC + `0x3c`**。字段（`qp.h:130-139`）：

   | QPC 偏移 | 字段 | 值 |
   | --- | --- | --- |
   | `0x3c` | `base_qpn` (BE32) | `ilog2(rss_rings) << 24 \| base_qpn` |
   | `0x40` | `default_qpn` (BE32) | **`base_qpn`**（真正的 RX QPN） |
   | `0x44` | `reserved` (u16) | 0 |
   | `0x46` | `hash_fn` (u8) | `MLX4_RSS_HASH_TOP = 1`（`qp.h:109`；`XOR` 是 0） |
   | `0x47` | `flags` (u8) | `mlx4_en_get_rss_mask`（见下） |
   | `0x48` | `rss_key[10]` (BE32×10) | `MLX4_EN_RSS_KEY_SIZE = 40` 字节 |
   | `0x70` | `base_qpn_udp` (BE32) | profile 的 `udp_rss` 为真时才填 |

   `flags` 的取值（`mlx4_en_rx.c:1005-1013`）：`MLX4_RSS_IPV4(1<<5) | MLX4_RSS_TCP_IPV4(1<<4) |
   MLX4_RSS_IPV6(1<<3) | MLX4_RSS_TCP_IPV6(1<<2)` = **`0x3c`**；`udp_rss` 为真再加 UDP 两位。
3. **迁移**：**与阶段 4 同样的三步** `mlx4_qp_to_ready(dev, &res.mtt, &context, &indir_qp, &state)`
   （`mlx4_en_rx.c:1078`）⇒ 入口 QP 也走 `RST2INIT → INIT2RTR → RTR2RTS`，MTT 只在第一步写。

（`MLX4_RSS_HASH_TOP = 1` 已取到，见上表；`MLX4_RSS_HASH_XOR = 0` 是另一个选择，
本阶段照参考用 `TOP`。）

## 4. 怎么验证（判据必须来自值，不是来自"没报错"）

| 层 | 期望 | 为什么算证据 |
| --- | --- | --- |
| MAC 命令 | `SET_PORT` 回 `fw status 0x0` | 命令"发出去了"和"被接受"是两件事 |
| MAC 回读 | 再发一次 `MAILBOX` 读回**同一张表**，槽 0 == `mac \| VALID` | 唯一能证明固件真接受了这张表的方式 |
| 入口 QP 三步 | 每步 `fw status 0x0`，且每步后 `QUERY_QP` 状态与刚写的一致 | 与阶段 4 同一套（那次就是这条抓出了"命令成功≠状态被接受"） |
| 入口回读 | `QUERY_QP` 的 `default_qpn` == 阶段 4 的 RX QPN、`base_qpn` 高字节 == `ilog2(1)=0` | 直接验"分流目标指向真正的 RX QP"这条判据 |
| 不指向自己 | 入口 QPN ≠ `default_qpn` | 计划里点名的失败模式："入口指向自己" |

**回读 MAC 表怎么做**：`in_modifier` 同 `0x201`、`op_mod` 同 `0x1`，但**不带 mailbox**
（`in_param = 0`）时固件把表放进 outbox。这条要在实现时用「有/无 mailbox 的同一个命令」
验证；若实测发现读不回来，就在文档里写明"回读不可用，判据退化为命令状态"，不要假装验过。

## 5. 需要你决定的两件事

### 决策①：MAC 注册要不要做——它对阶段 9/10 是否真有必要？

参考 `mlx4_en` 在 `mlx4_en_do_set_rx_mode()` 里注册 MAC，而**收包路径本身不依赖它**
（到达哪个 QP 由 steering 决定）。但我们目标里有一条是"ARP 广播能产生有效 RX CQE"，
而 ARP 是**广播**，走 multicast/广播路径。

**建议**：**做**（照计划）。理由：它是"固件认这个地址"的唯一开关，而阶段 10 要交付协议栈；
少做一个"计划里写了、且不贵"的步骤，换来的是阶段 10 出现"帧不来"时多一个未知量。
代价：一条 `SET_PORT` 命令 + 一张 1024 字节表 + 一次回读。

### 决策②：入口 QP 的 `db_rec_addr` 用左移那份吗？

参考里这个字段有**两种**写法，而且**同一份上下文的不同 QP 用不同的**：
`fill_qp_context` 写 `res.db.dma << 2`（`mlx4_en_resources.c:78`），
而 RX ring QP **覆盖**成不左移的 `ring->wqres.db.dma`（`mlx4_en_rx.c:947`，阶段 4 已按此实现）。

**入口 QP 不覆盖**，所以它带着左移那份。

**`<< 2` 的语义已查清（写方案后补的，三处对照）**：

| 站点 | 写进 `db_rec_addr` | 产生完成事件？ | 驱动写那条 record？ |
| --- | --- | --- | --- |
| CQ：`mlx4_cq.c:284` ← `mlx4_en_cq.c:158` 传 `cq->wqres.db.dma` | **不左移** | 是 | 是（`mlx4_en_cq.c:122` 的 `set_ci_db`） |
| RX ring QP：`mlx4_en_rx.c:947` | **不左移** | 是 | 是（`mlx4_en_rx.c:253` 的生产者） |
| **入口 QP**：`mlx4_en_resources.c:78` | `priv->res.db.dma << 2` | **否** | **从不** |

* `mlx4_alloc.c:725`：`db->dma = pgdir->db_dma + db->index * 4;` ⇒ **`.dma` 是真实字节 DMA 地址**，
  每 index 4 字节。所以 `<< 2` 真的把字节地址**乘了 4**。
* `priv->res` 是**真分配**的（`mlx4_en_netdev.c:2236` 的 `mlx4_alloc_hwq_res(..., &priv->res, ...)`，
  内部 `mlx4_db_alloc(dev, &wqres->db, 1, ...)`），所以不是"没分配所以是 0"。
* **`priv->res.db.db` 在全树从未被写过**（`grep 'res\.db\.db'` 只命中 CQ 与 ring 的 `wqres`，不是 `res`）。
* 入口 QP 是 indirection，职责只是把哈希分流指向 `base_qpn`/`default_qpn`，
  **不产生完成事件**。

⇒ 结论：**这个字段两边都是死的**（固件读它无意义，驱动也不写它），
所以那个偏 4 倍的地址**不可能被观测到**。它是参考驱动里一个潜伏的不一致，不是语义。

**建议**：**照参考**（入口 QP 用左移那份），并在文档里写明：
* 依据是"入口 QP 不覆盖 fill 的值"这一行事实；
* 上面那条三处对照；
* **为什么不"修正"它**：生产环境的 `mlx4_en` 就是带着 `<< 2` 工作的——这是经验证据；
  而"应改成不左移"的前提是"固件要的是字节地址"，**这个字段的语义没有文档**。
  所以照抄是有证据的选择，"修正"是没有证据支持的假设。

**证伪条件**：若阶段 9/10 出现 RSS 相关的收包异常，这是第一个要反向试（改不左移）的字段。

### 决策③（附）：`rss_rings` 与 `base_qpn` 的关系

`base_qpn = ilog2(rss_rings) << 24 | base_qpn`：单环时 `ilog2(1) = 0`，所以高位为 0、
这个字段**就等于** `base_qpn`，与 `default_qpn` 相同。这让单环情形的判据很干净
（两个字段相等且都等于 RX QPN），但也意味着**高位编码在本阶段不会被验证**——
多环才会。**建议**：照参考填，并在文档里注明"高位编码本阶段验不到，属 M5"。

## 6. 实测 / 推断 / 未测（写方案时）

**实测**：阶段 4 的 RX QP 在 RTS、`local_qpn 64`（2026-10-12 06:19，`verify.sh run` 34 PASS）。
`QUERY_PORT` 的 `def_mac` 已在本机日志里出现过（`00:11:22:33:44:55`）。
`mlx4_multicast_attach` 在 A0 + `MLX4_PROT_ETH` 下**直接 `return 0`**
（`mlx4_mcg.c:1412-1415`）——所以阶段 8 不能等它生效，这条现在是源码级确认的。

**推断（有源码依据、未上机）**：上面全部编码；MAC 表回读可行的假设；`<<2` 不承重。

**未测**：一行代码未写。阶段 6–10 不在范围内。

## 7. 回滚

加法为主；MAC 表与入口 QP 都会在硬件里留下状态，直到下一次 `reset`
（验证链条本来就以 `reset` 开头，所以这是自愈的）。运行期退路
`sudo tools/cx3-mlx4/rollback.sh [--purge]`。
