# ICM 解映射：固件说失败、我们照样释放，以及"为什么它会说失败"（0.43.0）

本文只写本轮实测与源码级证据。它回答 §39 提出的三件事，并给出一个比 §39 更强的结论：
那 14 条 `fw status 0x1` **不是固件坏了，是我们把一条它无法理解的命令发给了它**。

范围：`src/Cx3Mlx4.cpp` 的 ICM 表生命周期 + `src/Mlx4Hw.cpp` 的
`unmapIcm`/`unmapIcmAux`。其他模块未改。

---

## 1. 结论摘要

1. **缺陷一（返回值被丢掉）**：`freeIcmTables()` 调 `unmapIcm()` / `unmapIcmAux()`，
   丢掉返回值，无条件 `icmFree()`，并在结尾打印
   `icm tables: released (the HCA's page tables no longer reference them)` —— 一句
   关于**设备内部状态**的断言，而代码从未问过设备。
2. **缺陷二（根因，§39 没有的）**：拆表时发的那 14 条 `UNMAP_ICM`，是发给一个
   **从来没有被告知过这些映射的固件实例**。`verify.sh` 的链条每轮开头都 `reset` HCA
   （`tools/verify.sh:342`），而 reset 只失效了固件的状态：主机侧 `fTables[].icm[]`
   **活着**（`exportResetHca` 只清 `fHcaInited`/`fIcmMapped`/清除寄存器就绪，不碰表记录），
   于是下一次 `m2dprofile` 的入口 `freeIcmTables()`（`src/Cx3Mlx4.cpp:3293`）就会为
   reset 之前的映射发 unmap。实测 **6/6 个 `0x1` 簇都紧跟一次 reset**，而**唯一一次
   14 条全 `ok` 的簇（2026-10-11 20:22:02）是唯一一次"建表与拆表之间没有 reset"**。
3. **AUX 为什么是 ok**：`UNMAP_ICM_AUX` 是**无参数**命令（`src/Mlx4Hw.cpp:1592-1601`，
   `cmdBox(0, 0, 0, 0, ...)`），固件没有范围要解析，所以无论有没有映射都答 0。
   `UNMAP_ICM` 带 `(virt, npages)`（`src/Mlx4Hw.cpp:1607-1617`），固件必须去自己的映射
   结构里找这个范围 —— 而 reset 之后**这个结构根本不存在**（该实例连 `MAP_ICM_AUX`
   都还没收到：它在 `m2dbringup` 里，晚 0.4 秒，见 01:40:19.767）。找不到范围 →
   `CMD_STAT_INTERNAL_ERR`。这一条标为**推断**（见 §6），但"无参数命令 vs 带参数命令"
   是实测差别。
4. **对 §39.5 的影响（诚实削弱）**：§39.5 的候选机制是"固件仍在引用这块内存 → DMA 写进
   已回收内存 → 内核内存损坏"。但本轮测到的每一次"unmap 失败即释放"**都发生在固件本来
   就没有映射的窗口里**（这正是缺陷二的结论），也就是说最具体的那些实例上，固件**不可能**
   在引着这些页。所以：§39.5 **没有被推翻，但它的实测支撑变弱了**；把它当作根因之前，
   应该先按本文的修法消掉这些失败，再看 panic 是否还在。
5. **修法仍然要做**，而且理由不是"修 panic"：它修的是"失败被表示成成功"（B 家族）和
   "一条我们无从验证的断言写成了既成事实"。代价是一个真实的取舍（泄漏 vs 把固件仍可能
   引用的内存还给内核），见 §4.3。

---

## 2. 缺陷一：源码级

`git show 400929c:src/Cx3Mlx4.cpp` 的 `freeIcmTables()`（第 3534-3557 行）核心三行：

```c
fHw.unmapIcm(t->icm[c]->npages,                                   /* :3544-3545 */
             t->virt + (UInt64)c * MLX4_TABLE_CHUNK_SIZE, &fwStatus);
fHw.icmFree(t->icm[c]);                                           /* :3546 */
```

aux 分支同样（`:3552-3553`：`fHw.unmapIcmAux(&fwStatus); fHw.icmFree(fAuxIcm);`），
结尾无条件打印 `:3556`。另外 `exportM2dBringUp` 里的复用路径也一样
（旧 `:3652-3653`：`fHw.unmapIcmAux(&stale); fHw.icmFree(fAuxIcm); fAuxIcm = 0;`）。

`unmapIcm()` 是**有返回值的**（`src/Mlx4Hw.cpp:1607-1618`，返回 bool 并把固件状态写进
`outFwStatus`），三处调用者都把它丢了。

那个 `0x1` 的含义在参考枚举里（`vendor/freebsd-src/mlx4_core/mlx4_cmd.c:63-65`）：

```c
CMD_STAT_OK            = 0x00,
/* Internal error (such as a bus error) occurred while processing command: */
CMD_STAT_INTERNAL_ERR  = 0x01,
```

映射到 errno 是 `-EIO`（同文件 `:143`）。它不是 `BAD_RESOURCE`(0x05)、不是
`BAD_PARAM`(0x03)。命令编码本身核对无误：参考 `mlx4_UNMAP_ICM()` 是
`mlx4_cmd(dev, virt, page_count, 0, MLX4_CMD_UNMAP_ICM, ...)`
（`vendor/freebsd-src/mlx4_core/mlx4_icm.c:233-237`），我们是
`cmdBox(virt, 0, npages, 0, MLX4_CMD_UNMAP_ICM, ...)`，逐字段一致。

参考自己也在等价位置丢返回值（`mlx4_icm.c:298-301`、`:441-443`、`:457-459`），所以
"丢返回值"不是本移植独有的疏漏 —— 但参考正常工作时这些 unmap **不该**返回
`INTERNAL_ERR`。我们这里系统性全失败，说明的是**固件与主机的 ICM 关系早就不同步**，
那就是缺陷二。我们的独有产物是**那行断言日志**。

---

## 3. 缺陷二（根因）：reset 只失效了一半，于是我们发了一条固件无法理解的命令

### 3.1 链条读法

`tools/verify.sh` 的 `do_run()` 每轮固定：`reset`（`:342`，`in=1 1`）→ `queryfw force`
→ `fwload`（`MAP_FA` + `RUN_FW` + `MOD_STAT_CFG`）→ `querydevcap` → **`m2dprofile`**
（`:387`）→ `m2dbringup`（`:393`）→ 队列/R4 阶段。

`m2dprofile` 的**第一件事**就是 `freeIcmTables()`（`src/Cx3Mlx4.cpp:3293`：
"A new profile replaces the old layout, so anything mapped for the old one has to go
first"）。而 reset 之后、`m2dbringup` 之前，这个固件实例**没有被给过任何 ICM 映射**：
它只拿到了 FW 区（`MAP_FA`）。

主机侧却不是这样。`exportResetHca()`（`src/Cx3Mlx4.cpp:322-424`）在 reset 成功后清
`fHcaInited`、`fIcmMapped`、`fClrIntReady`，并 teardown EQ/CQ/RQ/MR —— 但
`fNumTables` 与 `fTables[i].icm[c]` 原封不动。这正是 B3 那条缺陷的**同一个形状、
下一个字段**："一次 reset 让某块状态失效了，但失效清单漏了一项"。

### 3.2 实测（`/usr/bin/log show`，只读取证）

命令（本轮实际跑的，窗口两端都包含）：

```sh
/usr/bin/log show --start "2026-10-12 00:00:00" --end "2026-10-12 03:30:00" \
  --predicate 'eventMessage CONTAINS "Cx3Mlx4"' --style compact
/usr/bin/log show --start "2026-10-11 20:14:00" --end "2026-10-11 20:24:00" \
  --predicate 'eventMessage CONTAINS "Cx3Mlx4"' --style compact
```

2026-10-12（这次启动，第一个 panic 之后的 B 段；每簇都是 14 条 `UNMAP_ICM`）：

| 簇（14 条 UNMAP_ICM） | 14 条的状态 | 之前最近一次 `HCA reset` | reset→unmap 之间有没有 `MAP_ICM_AUX`/`MAP_ICM` |
| --- | --- | --- | --- |
| 01:26:43.431–.473 | 全 `0x1` | 01:26:38.613（4.8 s 前） | 没有（`MAP_ICM_AUX` 在 01:26:43.596） |
| 01:27:34.878–01:32:22.506 | 全 `0xfffffffd`（超时） | 01:27:01.487 reset **被拒绝**（中断会话未关） | —— 这一簇是"卡住"，不是本文的 `0x1` |
| 01:40:19.372–.420 | 全 `0x1` | 01:40:13.789（5.6 s 前） | 没有（`MAP_ICM_AUX` 在 01:40:19.767） |
| 01:40:29.543–.578 | 全 `0x1` | 01:40:23.915 | 没有（`MAP_ICM_AUX` 在 01:40:29.9 之后） |
| 01:42:03.894–.932 | 全 `0x1` | 01:41:59.063 | 没有（`MAP_ICM_AUX` 在 01:42:04.0） |
| 01:42:10.142–.192 | 全 `0x1` | 01:42:05.012 | 没有（`MAP_ICM_AUX` 在 01:42:10.19 之后） |
| 01:42:36.000–.058 | 全 `0x1` | 01:42:31.188 | 没有（`MAP_ICM_AUX` 在 01:42:36.164） |

控制组，2026-10-11（0.33.0，同一段代码，唯一一次全 `ok`）：

| 簇 | 14 条的状态 | 建表 | 建表→拆表之间 |
| --- | --- | --- | --- |
| 20:22:02.931–.975 | 全 `0x0` | 20:18:50.545–.595（14 张表），`MAP_ICM_AUX` 20:18:50.536 | **没有 reset**，没有别的 bring-up |

于是判读是唯一的：**同一段代码、同一台卡、同一次启动，"有 reset 就全 0x1，没有 reset
就全 0x0"。** 差别不在固件质量、不在命令编码、不在对齐，而在"这个固件实例有没有被告知过
这些映射"。

### 3.3 AUX 与 14 张表的差别，为什么

同一簇里 `UNMAP_ICM_AUX` **每次都是 ok(0x0)**，包括 reset 之后那些簇
（01:26:43.475、01:40:19.421、01:40:29.580、01:42:03.936、01:42:10.195、01:42:36.061），
而 `UNMAP_ICM` 每次都是 0x1。三条可查的事实：

1. `UNMAP_ICM_AUX` 没有参数：`cmdBox(0, 0, 0, 0, MLX4_CMD_UNMAP_ICM_AUX, ...)`
   （`src/Mlx4Hw.cpp:1592-1601`）——固件只需丢掉自己记的那个 aux 指针，**没有范围需要
   解析**，所以在"根本没映射"的固件上也答 0。
2. `UNMAP_ICM` 带 `(virt, npages)`（`src/Mlx4Hw.cpp:1607-1617`）：固件必须在这个实例的
   ICM 映射结构里找这片范围。reset 之后该结构为空（**而且**该实例连 `MAP_ICM_AUX` 都还
   没收到——`ICM` 的表/页表由 aux 区承载，见 `mlx4_main.c:1563-1590` 的顺序：
   `SET_ICM_SIZE` → 分配 aux → `MAP_ICM_AUX` → 各表 `MAP_ICM`）。
3. 同一条窗口里 `MAP_FA`/`RUN_FW` 之后的 `UNMAP_FA` 是 **ok**（01:40:15.411、
   01:42:32.311、01:26:39.736）：`UNMAP_FA` 也是无参数命令
   （`src/Mlx4Hw.cpp:1153-1166` 的 `cmdBox(0,0,0,0,...)`）。这进一步印证"带不带参数"
   才是这条分界，而不是"reset 之后什么 unmap 都失败"。

"固件内部在页表 walk 上撞了总线错误"这一步是**推断**：`INTERNAL_ERR` 的参考举例是
bus error（`mlx4_cmd.c:63-65` 注释），与"找不到范围"的形态一致；但固件不开源，我们
没有它内部那一步的观测。可证伪的预测写在 §6。

---

## 4. 修法

### 4.1 新增一个状态：这台固件到底有没有被告知过这些映射

`src/Cx3Mlx4.h:328-350` 新增 `fIcmHcaHolds`（`:341`）与 `fLostIcmPages`（`:350`，见 4.3）：

* `mapIcmTable()` 里**每次 `MAP_ICM` 被接受后**置位（`src/Cx3Mlx4.cpp:3490`）；
* `MAP_ICM_AUX` 被接受后置位（`src/Cx3Mlx4.cpp:3806`）；
* **HCA reset 成功后清零**（`src/Cx3Mlx4.cpp:407`，就在 `fHcaInited`/`fIcmMapped`
  旁边 —— 失效清单放在它唯一该在的地方）；
* `freeIcmTables()` 结束时清零。

注意它置位在"每次 MAP_ICM 成功"而不是"INIT_HCA 成功"：映射从 `MAP_ICM` 被接受那一刻
就存在，半途失败的 bring-up 也仍然要把已经交出去的块收回来。

这样 `freeIcmTables()` 里那个唯一的问题——**要不要发这条 unmap**——就有了依据，
而不是靠调用方的印象。

### 4.2 `freeIcmTables()`：unmap 的答案决定释放，而不是"调用过了"就释放

新实现（`src/Cx3Mlx4.cpp:3596-3668`）：

* `!fIcmHcaHolds`（reset 之后）→ **不发 unmap**，直接释放，并打印**没发哪条命令、
  为什么**：`icm tables: N allocation(s) freed without UNMAP_ICM/UNMAP_ICM_AUX ...`
  （`:3656`）。这是那行断言的诚实版本：它不再说"HCA 不再引用它们"。
* 发了 unmap 且被**确认** → 释放，计数（`:3624-3626`）。
* 发了 unmap 且**没有确认** → **保留这块内存，不释放**，逐个打印带真实 fwStatus 的
  行（`:3613-3621`），并把页数累加进 `fLostIcmPages`。
* aux 同理（`:3629-3645`，`:3635` 是保留时的那行）。

`exportM2dBringUp()` 的 aux 复用路径（`src/Cx3Mlx4.cpp:3761-3807`）也照同一条规则改：

* 没有被告知过（`!fIcmHcaHolds`）→ 直接释放旧 aux；
* `UNMAP_ICM_AUX` 确认 → 释放后新分配；
* `UNMAP_ICM_AUX` 被拒 **且新 profile 需要的页数一样** → 旧 aux **保留并复用**，
  由紧接着的 `MAP_ICM_AUX` 重新声明它（不泄漏，也不把固件可能还引用的内存还给内核）；
* `UNMAP_ICM_AUX` 被拒 **且页数不同** → 旧 aux 保留（计入 `fLostIcmPages`，永不释放），
  另分配一块新的。

### 4.3 取舍（写清楚，不假装没有代价）

| 选择 | 代价 | 风险 |
| --- | --- | --- |
| unmap 未被确认时**保留**（本实现） | 这块 ICM + 描述符**到下次重启前不回内核**；aux 一块就是 2082 页（8328 KiB） | 反复失败会累积 |
| unmap 未被确认时**照旧释放**（旧行为） | 无 | 把固件可能仍在翻译的内存还给内核；本项目已在这类形状上丢过一次机器（§38/§39、DEFECTS B2/C6） |

选保留，还有一个具体理由：失败是**关于固件映射状态**的陈述，而本驱动没有任何观测能证明
这些页没被使用。期望值是 **0 次保留**（reset→reprofile 这条正常链条不再发 unmap，
非 reset 链条的 unmap 实测会被确认），所以代价只在故障时支付。

一个容易被忽略的细节：**槽位仍然被清 0**，即使那块内存被保留（`:3620`、`:3644`）。
因为 `mapIcmTable()` 的复用判据是"槽位非 0"（`src/Cx3Mlx4.cpp:3447`），把保留的块留在
槽里会让下一次 bring-up 直接把一个"映射恰恰是没能建立的那个"的地址交给 INIT_HCA。
内存保留、记录改由 `fLostIcmPages` 承担（`:3647`、`:3665-3667`）。

---

## 5. 回滚

代码回滚（两个文件，改动是加法的，没有调用点被移动）：

```sh
cd /Users/you/Documents/Hackintosh-MacLocal
git -C tools/cx3-mlx4 revert --no-edit <本次提交>
BUILD_TAG=0.43.0 bash tools/cx3-mlx4/build.sh
sudo tools/cx3-mlx4/install.sh      # 装回；随后重启
```

运行期回滚（新 kext 重启后行为不对时），本项目已有的、不需要源码的那条：

```sh
sudo tools/cx3-mlx4/rollback.sh            # kmutil unload + 残留检查 + kmutil rebuild
sudo tools/cx3-mlx4/rollback.sh --purge    # 再删 /Library/Extensions/Cx3Mlx4.kext
```

两者都要重启才生效。0.42.0 的行为是"每次 reset→reprofile 都发 14 条注定 0x1 的
`UNMAP_ICM`，并把未确认的块直接还给内核"；回滚后它就回来了。

---

## 6. 实测 / 推断 / 未测

### 实测（有日志时间戳或源码行号，可复现）

* 旧代码丢返回值、无条件释放、并无条件打印那句断言：`400929c:src/Cx3Mlx4.cpp:3544-3546`、
  `:3552-3553`、`:3556`；`exportM2dBringUp` 的 aux 复用路径 `:3652-3653`。
* `0x1` 的枚举含义：`vendor/freebsd-src/mlx4_core/mlx4_cmd.c:63-65`，errno `-EIO`
  `:143`。命令编码与参考逐字段一致：`mlx4_icm.c:233-237` 对 `src/Mlx4Hw.cpp:1607-1617`。
* 2026-10-12 的 6 个 `0x1` 簇，每个都紧跟一次成功的 `HCA reset`（4.8–5.6 s 之前），
  且 reset→unmap 之间**没有**任何 `MAP_ICM_AUX`/`MAP_ICM`（§3.2 的表）。
* 2026-10-11 20:22:02 的 14 条全 `ok`，是唯一"建表与拆表之间没有 reset"的场合；
  同一次启动的建表在 20:18:50，`MAP_ICM_AUX` 在 20:18:50.536。
* `UNMAP_ICM_AUX` 在全部 7 个簇里都是 `ok`，含 reset 之后的 6 个；`UNMAP_FA` 在
  reset 之后也是 `ok`。两者都是无参数命令（`src/Mlx4Hw.cpp:1592-1601`、`:1153-1166`）。
* 2026-10-12 01:27:34–01:32:52 那一簇是**超时**（`0xfffffffd`），不是本文的 `0x1`：
  它前面那次 `reset` 被"中断会话未开"守卫拒绝（01:27:01.487），之后命令寄存器就卡住了
  （§37.3）。两条线索不同源，本文只解释 `0x1`。
* 构建门禁（本轮，`BUILD_TAG=0.43.0`，两次构建 sha256 一致 =
  `4c2c685f1ce9d6578b4296afb407f438f09260f8407e662eed80f4be1ba7d457`）：boot-path 19 个
  可达函数、0 failures；146 + 8186 + 137 项检查 0 失败；351/351 符号；
  `-Werror=shorten-64-to-32/format/shift-count-overflow/implicit-int-conversion` 下
  0 warning。

### 推断（有据但仍需设备侧观测确认）

* **固件为什么答 `INTERNAL_ERR`**：范围不在这个实例的映射结构里（该实例连
  `MAP_ICM_AUX` 都还没收到），固件在解析/查表时撞内部错误。参考把这一状态注释为
  "such as a bus error"，形态相符；但固件不开源，这一步没有直接观测。
* **reset 之后固件一定不再引用这些页**：由"reset 重启固件（`MAP_FA`+`RUN_FW` 是重新
  下载并启动）"推得，而且实测这些 unmap 全部失败（即它不认为自己有这些映射）。
  本文所有"不 unmap 直接释放"的决定都建立在这条上。反例形态：若哪次 reset 之后
  `UNMAP_ICM` 返回 `0x0`，这条推断就要重写。
* **§39.5 与 panic 的关系**：见 §1.4。方向是**削弱**——最具体的那些"失败即释放"实例，
  都发生在固件没有映射的窗口里。

### 未测（需要重启 / 需要设备，本轮明确没做）

* **修法本身没上机**：本轮禁止安装与重启，所以"reset→reprofile 之后不再出现 `0x1`、
  不再出现 `fLostIcmPages > 0`"是预期，不是实测。
* **AUX 被拒时的两条分支**（同尺寸复用 / 异尺寸保留）没有触发过：7 个簇里
  `UNMAP_ICM_AUX` 全是 `ok`。这两条是防御性的，未走过。
* **`fLostIcmPages` 的非零路径**没有触发过（那是故障路径）。
* **`0x1` 是否与那两次 panic 有关**：没有内核转储、没有固件写主机内存的观测，
  本轮无法判。修完再看同样的链条是否还 panic，才是判据。
* 参考驱动在正常工作下这些 unmap 是否也偶发 `INTERNAL_ERR`：未查（没有可对照的参考
  运行环境）。

---

## 7. 边界：同一形态、本轮明确没改的地方

列出来是因为它们形状相同，读者不该以为这里已经"清干净了"：

| 位置 | 形态 | 本轮为什么没改 |
| --- | --- | --- |
| `src/Cx3Mlx4.cpp:3238-3239`（`stop()`） | 释放 FW 区（`fFwIcm`）前**不**调 `UNMAP_FA`，直接 `icmFree` | FW 区是另一套资源、另一条历史（`fwload` 路径**有**检查返回值并逐条记录），改动面属于下一批 |
| `src/Cx3Mlx4.cpp:691-716`（`exportIcmAlloc`） | 诊断选择器，`icmalloc 0` 直接释放持有的区块，同样不问 unmap | 它的用法在脚本里是"在 `MAP_FA` 之前"的（`m2b-diagnose.sh:75-79` 明确写了这条前提）；要给它加守卫得先记录"这块区块当前是否被 `MAP_FA` 过"，是独立的小改动 |
| `src/Cx3Mlx4.cpp:808-814`（`fwload`） | `UNMAP_FA` 失败后仍然释放旧 FW 区 | **是已写明的取舍**（`:810` 那行日志就是"continuing anyway so the retry is still attributable"），且紧接着会 `MAP_FA` 一块新区域把它替换掉。改它需要单独评估 |
| `src/flr.inc:14-111`（`exportFlr`，选择器 7） | PCIe FLR 之后 **`fHcaInited`/`fIcmMapped` 一个都不失效**（除本轮新增的那一项外，整节没有失效动作）——B3 的同一形状 | 本轮只补了与本批有关的那一项（`fIcmHcaHolds = false`，`src/flr.inc:80-93`）。另外两个该说什么，决定的是 FLR 之后 `m2dbringup` 该拒绝还是该允许，属于那个选择器自己的改动面 |

上面三条都没有进 `docs/DEFECTS.md`：那个文件的规则是"要么有 guard，要么写明结构性的
理由"，而这三条的理由是**改动面**，不是结构性的。等它们各自被改时再成行。
（同一批里被改掉的那条已经成行：B 家族新增一行。）
