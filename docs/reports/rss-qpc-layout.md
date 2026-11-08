# T03：RSS QPC布局核对与raw采集规格

## 2026-10-24 .128运行身份关联（当前离线状态）

[.128验收](../../../../outputs/20261024-cx3-rss-runtime-binding/README.md)补齐工具层RSS身份bracket及冻结候选采集：`boundrss --allow-rss-experiment`在同连接identity49→单次RSS35→identity49；独立`decode-runtime.py`核boot/build/service/provider/generation/生命周期、ABI/scalar及命令时间，collector再核候选hash/loaded UUID/OS boot。decoder现位于项目内`tools/decode-qpc.py`，无须依赖历史outputs路径；与旧decoder字节相同，原11项结果和10项wire测试迁入。

独立审查捕获selector status0但首步QP转换被拒绝的可达误判；现操作成功必须有本次最终QUERY_QP的完整4096字节成功回执，独立解码为RTS且localQPN一致。身份关联有效不等于操作成功，失败结果仍可保留诊断；CLI错误exit0也不能让collector升级成功。最终56脚本/完整构建/91项输入和限定GPT复审通过。仅stub transport与合成邮箱，无真实采集、原子性、DMA静止或RSS功能证明。

本节更新下方历史“身份尚缺/采集未实现”的称谓。剩余为真实boot/raw、普通QP对照、key A/B及RSS语义；不重复已经完成的离线身份/ABI消费实现。

## 2026-10-23 .86优先纠正

.84/.85不部署：真实qpModify/qpQuery把command观测指针传给cmdBox第8参数bool
bypassBusy，而非第9参数；旧底层替身漏bool导致假绿。下文.84“真实链已接通”撤销。
.86显式false并运行真实QP→cmdBox→cmdBoxLocked，声明直接读取生产头；只替代
寄存器/时钟/锁/邮箱分配。17场景186检查0失败，封存.85相同测试42失败/exit1。
.85实际rawjson函数到独立consumer的离线验证仍有效，不证明真实设备端产出元数据。
29组及构建退出0、62条清单live/tar字节和两个二进制一致；未上机，R04仍PARTIAL。
编译器pointer-bool警告拒绝历史成员取地址写法，但不拒绝普通指针变量；类型级保护另补。

.87已补cmdBox第8参数指针的deleted模板重载；历史写法和普通指针变量两种负例
在不加Werror时均被拒，正确实际调用链17场景186检查继续通过。保护前类型测试
exit1，见test-qp-type-contract-red-04486.log。29组及构建退出0，62条live/tar清单
与两个binary一致（verification-batch41.json）。不证明显式旁路调用的授权或真机行为。

2026-10-22；**PARTIAL / D03保持OPEN / 尚无完整raw，未解释固件语义**。
本报告由主代理完成；RSS协作任务一个被中断、另一个429终止，未作为技术证据。
只有离线源码/保存结果核对；不重新初始化或启用多队列。

## 发送、查询与保存的三层布局

`src/Mlx4Hw.cpp:4013`将256-byte context复制到mailbox+8；`:4082`的QUERY_QP从mailbox+8取context。
参考`vendor/freebsd-src/mlx4_core/mlx4_qp.c:890-895`同样从+8读取。
ABI `Cx3R4IngressResult`（`src/cx3_abi.h:2207`起）是**解码结果结构**，不是原始QPC。
不能把selector35的structHex按QPC偏移再次读；也不能用其bool“key matches”恢复40-byte raw。

| 字段 | QPC偏移（邮箱偏移需另+8） | 发送/查询来源 |
| --- | --- | --- |
| flags/state、PD、MTU、RQ/SQ stride | 00、04、08、09/0a | `src/mlx4_regs.h:369-375`；qpFillContext / ingress comparator |
| local QPN、pri_path | 10、18 | `vendor/freebsd-src/qp.h:196`起的结构 |
| sched_queue | 38（pri_path+20h） | `src/Mlx4Hw.cpp:3785`，查询`Cx3Mlx4.cpp:4080` |
| RSS base/default QPN | 3c / 40 | pri_path+24h；BSD `mlx4_en_rx.c:1067`与本地`:3939` |
| RSS reserved/hash_fn/flags | 44 / 46 / 47 | `src/mlx4_regs.h:437-439` |
| RSS key / UDP base | 48..6f（40 bytes）/70..73 | `src/Mlx4Hw.cpp:3951`、`:3955` |
| cqn_send / cqn_recv | 7c / 9c | `src/mlx4_regs.h:398`、`:406`；comparator `Cx3Mlx4.cpp:4072` |
| db_rec_addr / param3 | a0..a7 / c0 | 同头`:407`、`:410`；comparator`:4074`、`:4079` |
| MTT高/低、log_page | cb/cc、c8 | `src/mlx4_regs.h:411-413` |

确认：**发送代码中的RSS key并不与9c/a0/c0这些字段重叠**。因此“overlay本来就覆盖了CQN”不能仅由当前C结构推出。
查询方向可能使用不同内部语义，但现有参考源码并未证明该特殊布局；也未证明本地ABI/拷贝过程完全无损。

普通RX QP与RSS入口不是同一对象：`qpFillContext`只在`!rss&&!isTx`填写RQ stride（`:3736`），RSS无自身RQ，依赖base/default QPN选择普通RX QP。
本次入口QPN65、target QPN64、CQN128、rss_rings=1；单环流量通过不能证明硬件多流hash/多队列分流。
普通RX覆盖db_rec_addr为未移位地址；入口保留reference fill的`phys<<2`，这是单独的语义疑点，不在key A/B里同时修改。
“两个地址都指向已拥有内存”的旧注释不是普遍成立的算术保证，须独立核定移位编码含义，不能据此改地址或宣称安全。

## 现有保存结果能证明什么

20:46 `35.json`及已有M4报告的解码：RSS/key比较为匹配，入口RTS；sent CQN128/read`0x004d4e4f`；
dbrec read`0x404040414240`，param3 `0x20000000→0x02000000`，sched`0x83→0x82`。
key构造`src/Cx3Mlx4.cpp:3342`是`40h+i`，因此key[13:16]=`4d4e4f`。
这只是相关线索；bool比较与字段读数不足以定位这些字节从哪一步改变。
`r4IngressQueryAndCompare`（`:4043`起）仍按通用字段比较；FULL不成立，不修改校验器凑PASS。

## 最小采集改动规格（尚未实施）

1. 新增有版本/长度的诊断结果或分块读取接口；不能直接扩老struct破坏已加载ABI。默认禁用，仅安全实验窗口开启。
2. 每次transition保存：输入硬件QPC完整248 bytes、实际post前mailbox前264 bytes（含8-byte prefix及8-byte padding）、完整QUERY_QP邮箱4KiB和选取的248-byte context。旧ABI的256-byte槽末8字节是padding；捕获点必须在邮箱被复用/clear前。
3. 全部保存为**设备BE字节流**，附opcode、QPN、fwStatus、posted/timeout、context长度/offset、boot/版本/UUID、generation、phase/单调时间及hash；不得仅输出格式化字段。
4. 同一时点获取普通RX QPN64与入口QPN65的对照；若接口/命令无法无扰采集，先说明限制，不额外reset。query失败只记失败，禁止把旧缓冲当本次成功。
5. 离线由独立decoder和生产decoder分别解码，字节级diff发送→post→raw query→context→ABI；检查长度、+8、BE/host、数组边界及上下文来源。合成fixture只验解析，不冒充硬件raw。
6. 安全退路先于raw诊断：P1-S、T04门禁、唯一硬件owner与用户暂停工作窗口；当前没有这些证据，不运行。

## 离线解码器与 fixture（2026-10-23）

本节前期3项模型fixture是历史记录；当前.83的格式/反例与使用方法见本文末尾。

`outputs/20261022-cx3-fulladapt-batch1/qpc-decode.py` 按 `src/mlx4_regs.h` 的
`QP_CONTEXT_*` 偏移解码 256 字节 QPC：27 个字段（flags/pd/mtu_msgmax/rq_size_stride/
sq_size_stride/usr_page/local_qpn/remote_qpn/pri_counter_index/pri_ackto/pri_sched_queue/
pri_feup/rss_base_qpn/rss_default_qpn/rss_reserved/rss_hash_fn/rss_flags/rss_base_qpn_udp/
cqn_send/cqn_recv/db_rec_addr/rq_wqe_counter/sq_wqe_counter/param3/log_page_size/
mtt_base_h/mtt_base_l），并派生 `state`、`rss_key_hex`、`rss_high_byte`。
`test-qpc-decode.py` 用合成 RSS context 做往返校验（3/3 通过），另含长度强制与全零
context 两项。

**这只证明解码器自身正确，不证明任何硬件语义。** 仍缺：完整 raw QPC（发送前/QUERY_QP
回读）、普通 RX QP 对照、以及只改 key 的 A/B；这些都需要在受控窗口里采集，采集接口尚未实现。

## 采集接口（2026-10-23 已实现，未上机）

selector 35（`r4ingress`）的 `Cx3R4IngressResult` 末尾新增两个 256 字节块：

- `rawSentQpc`：交给 `qpModify` 的 context，在三次状态迁移之前；**幂等路径留零**（那条路径不发送，
  没有"送进去的字节"可报）。
- `rawQueriedQpc`：最后一次 `QUERY_QP` 应答的**逐字**副本，在任何字段解码之前。

两者都由 `r4IngressQueryAndCompare()` 在操作成功后才复制；查询失败时 `rawQueriedQpc` 保持为零，
并由 `queryOk` 区分"全零 context"与"没有应答"。`cx3ctl` 的尺寸表用 `sizeof(Cx3R4IngressResult)`
（`tools/cx3-mlx4/tools/cx3ctl.c:2073`），所以结构扩容自动跟随，无需改工具。

离线契约由 `test-lifecycle-order.py::test_selector35_returns_raw_qpc_bytes` 锁定（13/13）。
**仍未证明**：这些字段只在真机跑 `r4ingress` 时才会被填充；本次没有任何硬件 raw 采集。

## key A/B：两个独立受控启动，仅改key

A为现有key `40h+i`；B为`a0h+i`（均40 bytes）。保持binary、QPN/CQN、环几何、端口、其它参数和流量相同；只改变诊断key选择。
预期B的key[13:16]=`adaeaf`；若某字段跟随该片段，记录其准确raw位置，不能自动给它命名“合法CQN”。

| 假说 | 预先预测 | 可否定它的观察 |
| --- | --- | --- |
| H1 固件查询的特殊字段复用/规范化 | 变化首先出现在真实query邮箱；A/B中可疑字节可重复跟随key；普通RX不受影响 | post前已有错误、raw不跟随而仅ABI变，否定该现象由固件query产生 |
| H2 本地解码/拷贝/偏移问题 | raw正确，错误首次出现在context提取或ABI/用户态；独立decoder不复现 | 经两个独立decoder确认错误已在raw同一位置，否定“仅输出解码错误” |
| H3 状态/内存损坏（含确定性错误） | 可能query外资源变化、普通QP异常、guard损坏/时间不稳定，也可能确定性跟key | 单次key跟随不能排除；需边界/代次/写入来源审计、重复对照和权威布局依据才足以区分 |

H1/H2/H3并非仅用两个值就一定可完全区分。先定位**首次变化层**，再逐项缩小；如果仍歧义，就保持OPEN。
异常资源引用、guard破坏、普通QP变化、命令timeout或独立管理/探针丢失时立即停止后续写入；不自动reset/retry。
失败后的内存处理服从安全隔离规则，不能让实验收尾成为新的危险释放。

## R04执行复核：0.44.64（2026-10-23，仅离线）

本节修正上面带日期的历史实现说明，不将离线证据外推为真机验收。

### 真实字节与传输

- `QP_CONTEXT_SIZE=0xf8`，即**248字节**，不是256；mailbox偏移仍为+8。
  老`rawSentQpc/rawQueriedQpc[256]`仅前248字节来自context，其余8字节留零。
  独立decoder的256字节fixture此前只是槽格式；不能把尾部当硬件返回。
- 普通RX QP（34）与RSS（35）各保存三步实际构造后、cmdBox前的264字节
  （8前缀+248 context+8已清零padding），成功query的完整4096字节，以及最终query的4096字节。
  没有“最终send”操作，已移除未填充的`rawSentMailboxFinal`，避免零数组伪装证据。
- qpModify/qpQuery拒绝不够完整采集的目标缓冲，失败query清理目标中的旧数据；
  query成功且内容全零仍须以query状态识别，不能靠数据非零判断有效性。
- 两个大结果目前分别17456/18064字节；源码没有将它们作为内核栈局部对象。
  `Cx3R4Output`支持普通输出与IOKit out-of-line descriptor：descriptor路径有界堆分配、
  精确尺寸检查、同步writeBytes和RAII释放，分配失败不进入设备export，短写返回错误。
  工具rawjson的输出容量为32768，超限明确拒绝，不再静默截断到4096。
- ingress版本2/长度在export后盖章；dispatch与处理器要求精确长度。新旧尺寸混用由固定尺寸
  dispatch在设备操作前拒绝；QP尚无版本头，长度不同不是完整的版本/代次协议。

### 平台依据与反例

核对Apple主来源：

- [IOKitUser IOKitLib.c](https://github.com/apple-oss-distributions/IOKitUser/blob/323ead896d04424f87184d8f6ff0cce811aab106/IOKitLib.c#L1831)：
  IOConnectCallMethod按in-band上限选择普通或OOL输出。
- [XNU IOUserClient.cpp](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/iokit/Kernel/IOUserClient.cpp#L5197)：
  将OOL输出填入structureOutputDescriptor；约6689行的dispatch按descriptor长度校验。
  本机15.5 SDK的IOExternalMethodArguments也声明相同字段；源码依据不等于已验证运行内核行为。

`test-r4-abi.py`抽取实际处理器并编译真实输出helper，仅替代transport/descriptor/allocator。
红例最初复现尺寸放宽和rawjson截断；加入与XNU一致的descriptor形态后，0.44.63再现8项失败，
证明原模拟遗漏平台边界。0.44.64修复后35项检查全通过，并验证18064字节逐字导出和超限零transport。
`test-qp-mailbox.py`执行真实qpModify/qpQuery：短缓冲、post前快照、失败/timeout、完整query、
尾部与guard共13389项检查通过；数量主要来自逐字节比较，不是13389个独立场景。

完整结果：外层`verification-batch18.json`。0.44.63的PASS只覆盖当时测试范围，
已被本节平台反例推翻其“大结果可正常返回”的推论；原始归档保留，不覆盖或删除。

### R04仍未完成

以下缺口清单记录.82时点；.83/.84的已补项与当前剩余项以本文末尾为准。

.82离线进展（2026-10-23）：真实cmdBox/cmdBoxLocked新增可选、按调用保存的
40字节`Cx3CommandObservation`。Entered含核心提前拒绝；Posted是主机GO store及
写屏障完成，不保证抵达/接受；Replied含FW错误的正常completion/status-read路径，
不证明QPC语义。start/posted/end为本次启动内单调ns，start不含命令锁等待。
flags=0表示未填充，不能凭零fwStatus判成功；没有跨调用last-command缓存。
生产执行测试7场景86检查0失败，最终同测试重放.81为38失败、exit1；
完整28组及构建退出0，见`verification-batch36.json`。
当前QP/RSS方法尚未请求观测输出，结果ABI/工具/独立decoder也未消费；
这是传输观测基础，不是采集闭环。event路径未独立验收；真实并发与硬件语义未证明。

还需每步采集长度/有效标记、query fwStatus、prepared与posted区别、boot/版本/代次/单调时间/
运行ID及哈希的完整元数据；普通QP版本头与独立decoder消费完整邮箱的闭环也未验收。
目前某些失败结果只有零数组，不能单独判断未尝试、未分配、已构造但未post或真实全零。
不得把新数组存在、模型通过或构建成功当成采集合同完成。所有真机raw、key A/B和RSS语义仍待R07。

### .83独立decoder完整邮箱格式（2026-10-23，仅离线）

`qpc-decode.py`现支持四种精确尺寸；默认auto仅按长度选格式，也可显式`--format`：

| 格式 | 输入字节 | QPC起点 | QPC之后的字节 |
| --- | --- | --- | --- |
| qpc | 248 | 0 | 无 |
| legacy-slot | 256 | 0 | 8字节旧槽padding |
| modify-mailbox | 264 | 8 | 8字节发送快照padding |
| query-mailbox | 4096 | 8 | 3840字节query邮箱余部，不假定为padding |

保留27字段与3派生字段；新增capture块保存完整prefix/context/suffix hex、各自边界、
全输入及248字节QPC的SHA-256。三段重组可逐字节还原输入，无静默丢弃尾部。
旧槽/发送padding非零单列warning而不解释成QPC；query余部原样保留。
全部结果标记`validity=not-established-by-bytes`：全零或解析成功均不能证明命令尝试、
主机posted、FW completion或QPC语义正确。hash只绑定文件字节，不证明来源。
RSS overlay字段名沿用参考布局，仍不是异常归因结论。

纯离线用法（输入是已保存hex文本，不运行cx3ctl，不打开设备）：

```sh
python3 outputs/20261022-cx3-fulladapt-batch1/qpc-decode.py \
  --file /absolute/path/query-mailbox.hex --format query-mailbox
```

`test-qpc-decode.py`共10项测试通过；新增248字节手写literal，期望值独立于生产builder，
检查BE/+8、尾字保留、完整邮箱重组/hash、非零padding、全零未知、尺寸/格式错配和CLI
输入错误。CLI拒绝时无部分JSON/traceback。最终同测试通过`CX3_QPC_DECODER`重放
封存.82 decoder得到25项失败、exit1，日志`test-qpc-capture-final-red-04482.log`。
旧decoder基线哈希与.82 verification-manifest一致；28组及构建退出0，见
`verification-batch37.json`，60条live/归档清单字节及两个二进制哈希一致。
本批不改驱动逻辑/ABI、没有硬件raw或操作。下一项仍需生产观测→结果/工具→独立decoder
完整连接、版本/运行/代次协议；R04和P1-R保持PARTIAL。

### .84采集观测接入生产QP/RSS（2026-10-23，仅离线）

`qpModify/qpQuery`新增可选64字节`Cx3QpCaptureObservation`。方法进入时清旧输出并
盖版本/长度章，早拒绝时fwStatus为NOT_POSTED、HCR记录零；从未调用的结果记录连章
也是零。`preparedBytes`表示主机已构造的采集相关邮箱区域（modify264/query4096），
不表示posted；`capturedBytes`表示已复制到调用者的原始字节数。modify在cmdBox前
捕获，即使后续失败也可能是264；query仅成功复制完整4096时才填4096，失败留零。
不给raw目标时captured为0；真实全零成功query仍有4096长度，不靠数据内容猜有效性。
方法fwStatus与内嵌40字节HCR观测分开：后者只由真实cmdBox核心写入，记录Entered/
主机Posted/Replied和单调时间。现有owner处置逻辑不因观测标志被放宽。

普通RX/RSS结果各自保存三组modify、三组query及最终query记录，query各步的fwStatus
不再丢失。现qp方法与结果消费者均已接入，不再是仅声明结构或无使用的transport输出。
普通QP在尾部扩展新增版本章1；RSS升为3，尺寸分别17912/18512。current dispatch/
handler继续精确长度门，章在export清零之后填写；工具先验章再显示各步记录，设备级
失败也显示。不把输出版本章称完整输入协商；与历史已加载版本的组合尚未实测。
旧工具必须与旧kext成套使用，当前候选未安装。

测试证据：

- `test-qp-mailbox.py`执行真实QP方法；新增12组观测情形并保留原逐字节用例，总13545
  检查0失败。HCR/allocator是替代边界，HCR核心的flag/timing另由.82的7场景86检查覆盖。
  包括分配/参数失败、FW失败/timeout、无raw目标、复用输出清旧状态及null status。
- `test-qp-setup-lifetime.py`执行完整生产setup与final比较，107场景1206检查0失败；
  新增检查每步观测写入正确结果槽，幂等重测的未调用步骤留零，最终query独立。
  QP传输为recording边界，不是模拟固件正确性或真机并发。
- `test-r4-abi.py`的36项client检查通过，执行真实handler、OOL helper及formatter；
  18512字节rawjson完整输出/超限零transport均通过。formatter验证未调用/已构造未进入
  HCR/FW错误/错误章，不把fwStatus0解释成成功。
- 最终同测试重放.83：底层140失败，RX/RSS28失败，QP版本章1失败且formatter失败。
  底层基线的两段方法逐字节匹配.83源码归档；上层从该归档读取真实旧controller/client/tool，
  仅替换测试的源读取，使用当前结果类型以断言新增输出缺失，未替换生产方法。
  日志`test-qp-capture-final-red-04483.log`、`test-r4-capture-final-red-04483.log`。

28组及构建退出0（`verification-batch38.json`），60条live/归档清单字节和两个
二进制哈希一致。本批未安装/调用设备/重启；不证明历史冻屏、DMA静止或RSS布局语义。

**当前R04剩余项**：boot/版本身份/运行ID/设备代次及采集hash绑定，生产结果到独立
decoder的可复核导出/消费闭环；真实raw、普通QP对照、key A/B、RSS异常解释依赖R07。
单调时间只在同一boot内有效，不是运行ID或代次凭据；全局准入与并发仍由R03/R06追踪。

### .85保存结果到独立decoder的离线消费链（2026-10-23）

`qpc-decode.py --rawjson /absolute/path/saved-result.json`现直接读取已保存的
`cx3ctl rawjson` JSON，不打开设备或执行cx3ctl。支持当前selector34/QP章1/17912字节
和selector35/RSS章3/18512字节；结果结构显式按x86_64 LE处理，抽取的邮箱按BE解码。
长度/偏移为独立列出的ABI表，测试由编译器布局的真实C结构产出，避免两边共用偏移常量
自证。ABI有变化时需显式升级，不猜测旧版或截断结构。

先验证整数selector/call/structLen、call==0、完整hex/实际长度及结果章；设备级result
status非零仍可检查诊断内容。每个modify/query/最终query检查采集章、长度、保留字段、
命令章/flags/fwStatus一致性和单调时间；命令进入前必须已构造邮箱。未调用记录不得夹带
旧证据，captured=0邮箱不得夹带旧字节；已复制query须有完整状态读取且fwStatus为0。
错误时退出非零，无部分JSON。未调用/失败query的decoded为null；已报告成功复制的
全零query可以解析为全零内容，仍不升级为正确QPC或真实设备成功。
modify的pre-post快照可在后续timeout时保留并解码，不称设备接受。

输出包含7个带原始span/观测位置的记录、各邮箱hash、完整结果hash和保存文件hash。
这些hash只绑定文件字节；记录本身仍是不可信保存输入，未认证来源。
`identity=not-established-by-this-result-ABI`明确boot/运行/版本身份/设备代次尚未建立。

`test-qpc-result.py`联合执行实际cmdRawJson函数、真实C结果结构、recording transport
及消费端CLI，共11项测试通过；不使用生产QP context builder。覆盖两个布局、失败
result仍可读、send timeout快照、query未调用/timeout、全零成功、字节hash、旧版/
截断/章错配、无prepared却有命令、混杂旧字节及非法flags/时间。额外prepared反例
在首次消费实现上1项失败、修复后通过；见`test-qpc-result-preparation-red.log`。
最终同测试重放封存.84 decoder为5项失败、exit1，见
`test-qpc-result-final-red-04484.log`；基线hash匹配.84 verification-manifest。
原独立字段/四格式10项测试继续通过。

29组及构建退出0（`verification-batch39.json`），62条live/归档清单字节和两个
二进制hash一致。本批只改外层consumer/test/runner，不改驱动逻辑或ABI，未上机。
**剩余**：boot/运行/版本身份绑定及R06设备代次、真实导出/普通QP对照/key A/B、RSS
异常解释。离线消费链通过不是完整R04或P1-R验收。
