任务：审查一段刚写好、尚未在硬件上跑通的移植代码，并判断几个数值是否合理。
项目：把 **BSD 许可**（OpenIB.org BSD 分支）的网卡驱动从 FreeBSD `sys/dev/mlx4` 移植到
macOS IOKit，硬件 Mellanox ConnectX-3 Pro（`15b3:1007`，双端口）。

## 允许阅读的路径（**只读这两个目录，不要读其它任何目录**）

```
/Users/you/Documents/Hackintosh-MacLocal/tools/mlx4-port/freebsd-src/
/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/
```

## 背景：已经跑通的部分

- kext 匹配设备、映射 BAR0/BAR2、通过 HCR 命令寄存器与卡上固件通信。
- `QUERY_FW` 成功（`fw_ver 2.42.5000`、`cmd_if_rev 3`、`max_cmds 16`、`fw_pages 1542`）。
- 固件启动序列成功：分配 FW 区 → `MAP_FA` → `RUN_FW`（fw status 0）→ `MOD_STAT_CFG`。
- `QUERY_DEV_CAP` 成功。设备返回的原始 outbox（前 256 字节，大端 dword）实测如下，
  这是下面几个问题的依据：

```
+0x00 00000000 00000000 00000000 00000000
+0x10 0e0e0618 60180508 08160718 16180309
+0x20 60400823 00000400 00070007 0011381f
+0x30 00010000 802f0102 1e00007f 00008000
+0x40 eeff1f9d f677fbef 20020009 80090310
+0x50 003e03f0 00200200 00000000 0c030000
+0x60 00094017 40114010 00000100 00000080
+0x70 b5fd0000 fd00e0f4 3000ff01 00000000
+0x80 00200100 00010040 00800080 00800040
+0x90 00080040 3d5b8ec2 00008000 5e5ffff7
+0xa0 00100000 00000000 00000040 000000fe
+0xb0 00000001 00000000 00000000 00000000
```

按参考的解码方式（`mlx4_fw.c:836-1060`）算出来的关键值：

| 项 | 值 |
| --- | --- |
| 端口数 | 2 |
| num_sys_eqs | 1024（⇒ `flags2` 有 SYS_EQS 位） |
| `max_icm_sz`（u64 @0xa0） | **2^52** 字节 |
| entry sizes | qpc 256, cqc 128, eqc 128, srq 128, dmpt 64, cmpt 64, rdmarc 32, **aux 1**, altc 64, mtt 8 |
| reserved | qps 64, cqs 128, srqs 64, eqs 8, mtts 64, mrws 256 |
| min_page_sz | 512 |
| uar_size | 4 MiB，reserved_uars 2 |

## 我方代码在做什么（`src/Mlx4Hw.cpp` 的 `makeProfile`、`Cx3Mlx4.cpp` 的 M2d 部分）

严格按参考顺序：

1. `QUERY_DEV_CAP` 解码（`Mlx4Hw::parseDevCap`）
2. `mlx4_make_profile`（`Mlx4Hw::makeProfile`）——11 类资源、按尺寸降序打包
3. `SET_ICM_SIZE`（`Mlx4Hw::setIcmSize`）→ 取回 aux 单位数（**需要新的 immediate 读回路径**）
4. `MAP_ICM_AUX` → 然后对 **14 张表**逐个 `MAP_ICM`
5. `INIT_HCA`（`Mlx4Hw::initHca`，0x200 字节 inbox）

我方 profile 用的参数是 `qp 1024, rdmarc/QP 16, srq 64, cq 256, mcg 256, mpt 2048,
mtt 4096 段`（不是参考的默认值，参考默认是 IB 栈规模的 2^18 QP）。算出的布局：

```
CMPT   2^26 entries @ 0x0          size 0x100000000   (= 4 GiB)
RDMARC 2^14          @ 0x100000000  size 0x80000
QP     2^10          @ 0x100080000  size 0x40000
MTT    2^15          @ 0x1000c0000  size 0x40000
MCG    2^08          @ 0x100100000  size 0x40000
EQ     2^10          @ 0x100140000  size 0x20000
DMPT   2^11          @ 0x100160000  size 0x20000
ALTC   2^10          @ 0x100180000  size 0x10000
CQ     2^08          @ 0x100190000  size 0x8000
SRQ    2^06          @ 0x100198000  size 0x2000
AUXC   2^10          @ 0x10019a000  size 0x1000
总计 4195948 KiB (~4 GiB)
```

## 我要你回答的问题

1. **`max_icm_sz` 读到 2^52 是正常的吗？** 参考（FreeBSD `mlx4_fw.c:1048`、mainline
   `fw.c:1059`）都以 u64 从 `0xa0` 读，我方字节完全相同。请判断：
   设备报这个值意味着什么？参考里唯一用途是 profile 的容量检查（`mlx4_profile.c:142`），
   那么"检查通过"是否就代表可以按 4 GiB 去 `SET_ICM_SIZE`？有没有已知的上限或告警逻辑
   被我们漏掉（请给 `文件:行号`；没有就写「未知」）。
2. **`icm_size` ≈ 4 GiB 这个量级合理吗？** 它几乎全部来自 CMPT：
   `MLX4_NUM_CMPTS = MLX4_CMPT_NUM_TYPE << MLX4_CMPT_SHIFT` = `4 << 24` = 2^26，
   乘 `cmpt_entry_sz` 64 = 4 GiB。请确认这是参考的真实意图（cMPT 是**索引空间**上界，
   而真正占内存的只有被 `MAP_ICM` 的那部分），还是我们漏了什么把它缩小/放大的东西。
   另外：`SET_ICM_SIZE` 是把这 4 GiB 当作「HCA 侧地址空间预留」还是「真实内存」？
   依据在哪里？
3. **`aux_entry_sz = 1` 可疑吗？** 其它 entry size 都是 8/32/64/128/256，只有它是 1。
   参考有没有针对它的特例（请给 `文件:行号`；没有就写「未知」）。
   我方按 obj_size=1 去算 `MLX4_TABLE_CHUNK_SIZE / obj_size` = 262144 个对象/块、
   reserved 64 字节 ⇒ 1 个 chunk，会不会有什么隐患？
4. **审查我方的 M2d 实现是否与参考等价**，尤其：
   - `setIcmSize()` 里 aux 单位的换算（参考是 1 MiB 单位，我方按 `units * 256` 换算成
     4 KiB 页）。参考在 `#if MLX4_ICM_PAGE_SIZE < PAGE_SIZE` 里做的对齐修正，
     在两者相等的平台上是否确实是恒等变换？
   - `mapIcmTable()` 只映射 reserved 前缀、每个 chunk 256 KiB（`MLX4_TABLE_CHUNK_SIZE`）。
     参考的 chunk 语义、以及"最后一个 chunk 用整块还是按需截短"是否一致
     （注意我方把最后一个 chunk 向上取整到页边界）？
   - `initHca()` 的 inbox 字段：`flags` 的位、`INIT_HCA_EQE_CQE_OFFSETS` 的 bit29/30、
     cacheline 字节、以及 `mc_base/log_mc_hash_sz` 在 A0 steering 下的取值。
   - 有没有**参考做了、我们没做**的前置步骤（在 INIT_HCA 之前）？
5. **最可能的失败点排序**：第一次在硬件上跑这段，请按"最可能先失败"排序，并给每个
   一个最小判据（我们能看到每个命令各自的 fw status 与每张表的映射日志）。

## 格式

用中文。结构化（表格 + 有序列表）。每个源码论断给 `文件:行号`，查不到的写「未知」。
严格区分「源码确证」「机理推断」。不要写可编译代码。
