任务：审查一段**尚未在硬件上运行过**的移植代码，找出会导致它在第一次运行时就失败的问题。
项目：把 **BSD 许可**的网卡驱动从 FreeBSD `sys/dev/mlx4` 移植到 macOS IOKit，
硬件 Mellanox ConnectX-3 Pro（`15b3:1007`，双端口）。这一次新增的是**事件队列（EQ）**。

## 允许阅读的路径（**只读这两个目录**）

```
/Users/you/Documents/Hackintosh-MacLocal/tools/mlx4-port/freebsd-src/
/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/
```

## 已经跑通的（真机实测）

- `QUERY_FW`、`MAP_FA`、`RUN_FW`、`MOD_STAT_CFG`、`QUERY_DEV_CAP` 全部 fw status 0
- **M2d 全部通过**：`SET_ICM_SIZE`（2082 aux 页）→ `MAP_ICM_AUX` → **14 张表 `MAP_ICM`**
  → `INIT_HCA` fw status 0
- `INIT_HCA` 写入：`log_num_qps 10`、`log_num_cqs 8`、`log_num_eqs 0x1f`、`num_sys_eqs 1024`、
  `eqe/cqe 0x60000000`（**64 字节 EQE 与 CQE**）、`mtt_base 0x1000c0000`、`qpc 0x100080000` 等
- **MTT 表已整块映射**：它是 profile 里 64 个对象 × 8 字节 = 512 字节，但按 256 KiB chunk
  映射（日志：`icm table MTT: chunk 1/1 mapped at virt 0x1000c0000 (64 pages)`），
  即**映射了 32768 个 MTT 条目**，远超一个队列需要的页数

## 本次新增（`src/` 里新写的部分，全部尚未执行）

| 函数 | 作用 |
| --- | --- |
| `Mlx4Hw::cmdEvent()` | 与已有 `cmdBox()` 相同，但**置 HCR 的 E 位**并把 token 写进 token 字段 |
| `Mlx4Hw::queryFunc()` | `QUERY_FUNC`（opcode 0x56），取回本功能的 `max_eq`/`reserved_eqs`/`reserved_uars` |
| `Mlx4Hw::mttAlloc/mttWrite` | 从已映射的 MTT 表里分配条目并写入页总线地址（native 路径，直接写内存） |
| `Mlx4Hw::eqCreate()` | 分配 EQ 环（`icmAlloc`）→ 写 MTT → 填 EQ context → `SW2HW_EQ` |
| `Mlx4Hw::eqMap()` | `MAP_EQ`：把 `CMD` 与 `PORT_CHANGE` 事件路由到本 EQ |
| `Mlx4Hw::eqSetConsumerIndex()` | 写门铃（BAR2 平坦映射），值 = consumer_index \| (req_not << 31) |
| `Mlx4Hw::eqNextEqe()` | 轮询判定：`!!(owner & 0x80) == !!(cons_index & nent)` |
| `Cx3Mlx4::exportM3EqUp/exportM3EqPoll` | 两个 selector；后者发 E=1 的 NOP 再轮询 EQE |

设计上我采用的关键值（**请逐条核对**）：

- `nent` = `MLX4_NUM_ASYNC_EQE(0x100) + MLX4_NUM_SPARE_EQE(0x80)` → roundup_pow2 = **256**
- EQE 大小 **64 字节**（`eqeFactor = 1`）；`mlx4_eq.c:110-116` 说 64B EQE 的**前 32 字节保留、
  legacy EQE 在后 32 字节**，所以我取 `offset = slot*64 + 32`
- `log_eq_size = ilog2(256) = 8`
- `log_page_size = PAGE_SHIFT - MLX4_ICM_PAGE_SHIFT = 0`
- EQ context flags = `MLX4_EQ_STATUS_OK | MLX4_EQ_STATE_ARMED`（都是 0），即 `9 << 8 = 0x900`
- `mtt_base_addr` 高/低写入 context 的 0x1b（u8）与 0x1c（be32）
- **EQN = `reserved_eqs`（来自 `QUERY_FUNC`）**，不是 0
- 门铃地址 = `BAR2 + ((eqn/4) << 12) + 0x800 + 8*(eqn%4)`
- 轮询判定与参考 `next_eqe_sw()` 同式
- MTT 条目 = `busAddr | MLX4_MTT_FLAG_PRESENT(1)`，BE64

## 我要你回答的问题

1. **逐字段核对 EQ context 的字节布局**。参考的 `struct mlx4_eq_context`（`mlx4.h:323`）与
   我写的偏移（`src/mlx4_regs.h` 里的 `EQ_CONTEXT_*`）是否逐个一致？我是否漏写了参考会填的字段
   （例如 `page_offset`、`eq_period`、`eq_max_count`、`consumer_index`、`producer_index`）？
   如果参考不写它们，为什么不写？
2. **MTT 的地址与条目语义**。`mlx4_mtt_addr()` 返回的是 `offset * mtt_entry_sz`，
   `mlx4_write_mtt_chunk` 写到 `table + (mtt->offset + start_index)*8`。我的
   `mttAlloc/mttWrite` 是否与之一致？特别是：**MTT 条目数是否等于页数**（我按 1 条目/页），
   还是说一个条目覆盖 `1 << log_mtts_per_seg` = 8 页？这一点我把握不足，请以源码为准说明。
3. **EQN 的取值**。参考里 `mlx4_bitmap_init(&eq_table.bitmap, roundup_pow_of_two(num_eqs),
   num_eqs - 1, reserved_eqs, ...)`，然后用 `mlx4_bitmap_alloc`。这个 bitmap 的第一个可分配
   编号到底是 `reserved_eqs` 还是 0？我用 `reserved_eqs` 作 EQN 是否正确？
   另外：异步 EQ 在参考里是 **EQ 0** 还是"第一个可分配的编号"？如果固件把异步事件默认路由到
   EQ 0，我用非 0 的 EQN 是否会收不到事件（`MAP_EQ` 能不能改这件事）？
4. **E 位与 token 的用法**。我置 E 位并发 `NOP`（opcode 0x31）带 token；参考里
   `mlx4_cmd_wait` 用 `context->token`，而 `mlx4_cmd_poll` 用 `CMD_POLL_TOKEN`。
   我的做法是否等价？CMD EQE 的 token 字段（EQE 偏移 0x06，BE16）是硬件回显我写进
   HCR token 字段的那个值吗？**有没有哪一步要求我先把某个 EQ 设成"命令 EQ"**
   （例如 `MAP_EQ` 之前必须先 `SW2HW_EQ`、或命令 EQ 必须是特定编号）？
5. **最可能第一次就失败的地方，按可能性排序**，每条给出"如果它错了会看到什么"。
   我们能看到：每个命令各自的 fw status、EQ context 全文的日志、MTT 地址、门铃地址、
   以及轮询结果（哪个 consumer index、EQE 的 type/token/status 或"整环都归 HCA 所有"）。
6. **轮询判定**：`!!(owner & 0x80) == !!(cons_index & nent)` 在"环刚 arm、硬件还没写任何
   EQE"时应当判定为**不可消费**。请确认这个推论（以及 `nent` 参与相位、而不是 `nent-1`）。

## 格式

用中文。结构化（表格 + 有序列表）。每个源码论断给 `文件:行号`；查不到的写「未知」。
严格区分「源码确证」「推断」。不要写可编译代码。
