# R05：PCIe 当前链路只读查询规格

状态：下文保留实现前规格。2026-10-24 已完成 `.119-local` 离线实现/集成验收；selector48、368字节ABI、CLI与独立decoder，40个Python脚本和完整构建通过。证据见[本批验收](../../../outputs/20261024-cx3-pcie-query/README.md)。尚未查询真实设备或验证实际PCIe读数；P0/R05仍未完成。

实现合同补充：`readValid`表示实际采样位置，包含判为不可用的全1首次读数；它不是链路语义有效位。重复复查不覆盖首次raw。一致的非PCIe链尾可返回reason6/validity7，decoder独立重走raw链后接受为诊断失败，仍不发布current/max链路。匹配PCIe节点的next也检查范围、对齐和已访问节点。

## 结论与最小接口

从 `IOPCIDevice` conventional PCI configuration space 读取 capability list 中的 PCI Express capability，再读取该 capability 的 `Link Capabilities` 与 `Link Status`。必须把“当前协商值”和“设备声明的能力”分开返回；所有 raw 值与对应有效位同时保留。未知速率编码保留原始 nibble，解码值标为 unresolved，不能猜测。

建议新建独立只读 selector，追加为 ABI selector 48（现 `kCx3SelCount=48`，selector 47 已是 `queryadapter`），并使用版本化定长 structure output；不要扩展既有 `Cx3AdapterResult`（其 360-byte 大小和字段偏移已由静态断言固定）。在表中同时追加 dispatch 与 lock-class 行。此查询会碰 PCI config space，按现有 `probe`/`peek` 的规则用 `kXactTryLock` 避开 reset/FLR 窗口；争锁失败就返回 busy，不绕过 admission gate，也不采用当前被拒绝的 `kXactNoLock`。实现面最小为 `Mlx4Hw` 查询方法、`Cx3Mlx4` 导出转发、ABI 结构及一个 user-client selector/handler；不改变启动路径、既有 selector 或硬件状态。

## 读取与解析契约

### IOKit API / 地址范围

本机构建使用的 IOKit API 是 `IOPCIDevice::configRead8/16/32`；公开的 offset-overload 接收 `IOByteCount`，并转发到 `extendedConfigRead*`，不能把它误称为只能寻址8位。另有接收 `IOPCIAddressSpace` 与 `UInt8 offset` 的virtual overload；internal兼容声明也有UInt8版本。16-bit 读取会忽略 offset bit 0，32-bit 读取会忽略低 2 bit，因此调用方必须主动保证自然对齐。**本查询设计限定 conventional 0x00..0xff**，并非Apple公开API不能访问更宽空间；所有偏移用宽整数先验证，禁止截断回绕。Apple 头文件没有为这些读取给出独立错误码返回值：结果只有 raw 值，不能从 API 签名推断成功。

### Capability list 前置检查及有界遍历

本功能只支持 PCI endpoint header type 0（允许 header-type multifunction 位；判型时屏蔽 bit 7），不把 Type 1 bridge / Type 2 CardBus 当作 CX3 endpoint 解析。按顺序读取并分别验证：

1. `fPci != nullptr`，否则 `NotReady`。
2. Header Type `0x0e` 是 type 0；Status `0x06` 的 Capabilities List bit 4 为 1。任一读取为全 1 时归为 `read-failed-or-removed`，不得当成“无 capability”。
3. 读取 byte `0x34` 的 Capability Pointer。`0x00` 表示空链表（在前置状态有效时为 `no-pcie-capability`）；非零 pointer 必须在 `0x40..0xfc` 且 4-byte 对齐，否则 `malformed-list`。不要用 `configRead32(0x34)` 再截断来代替 byte 读取。
4. 每个节点先检查 offset，再 `configRead16(cap)` 读 ID / next pointer（ID 在低 byte，next 在高 byte）。维护 256-bit visited set，最多 48 个 conventional capability 节点；回访判 `cycle`，超过上限判 `bound-exceeded`。next pointer 的低两位按保留位检查，非零判 malformed；零表示终止。每一步保留实际访问节点计数。
5. PCI Express capability ID 是 `0x10`。遍历正常到链尾而未找到为 `no-pcie-capability`；链表读取异常或结构不一致单独报告，不要折叠为 no-cap。

现有 `findPcieCapability()` 在 [Mlx4Hw.cpp](/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Mlx4Hw.cpp:422) 仅读 pointer、用 48 次 guard；没有验证 Status bit、header type、全 1 失败值、指针范围与 cycle，也没有区分错误和无 capability。新查询应独立实现完整检查，不要悄悄把这个 helper 的 0 返回值解释为可靠的“不支持”。

### PCIe capability 与 link 字段

先读 `cap+0x00` 的 16-bit conventional capability header，拆出 PCIe capability ID（low byte，必须为 `0x10`）与 next pointer（high byte）。另读 `cap+0x02` 的 16-bit PCI Express Capabilities (`PCI_EXP_FLAGS`)；version 是该 word bits `[3:0]`，Device/Port Type 是 bits `[7:4]`，不要从 `cap+0x00` 解析这些字段。此规格仅验证 version `1` 和 `2` 的 endpoint layouts：只接受 Endpoint (`0`) 或 Legacy Endpoint (`1`) 且 version 为 1/2；version 0 或未来 version `>2` 均保留 raw flags，但 `linkFieldsValid=false` 并标注 unsupported-version。version 1 不读 LnkCap2；version 2 才允许按本表读取 LnkCap2。CX3 是 endpoint，拒绝非 endpoint 可避免把桥的 link 描述误标成设备端口链路。

| 字段 | conventional config offset | 提取 | 意义 |
|---|---:|---|---|
| Link Capabilities (`LnkCap`) | `cap+0x0c`, 32-bit | max speed `[3:0]`; max width `[9:4]`; DLL Link Active Reporting Capable bit 20 | 设备宣告的能力；不是当前链路值 |
| Link Status (`LnkSta`) | `cap+0x12`, 16-bit | current speed `[3:0]`; negotiated width `[9:4]`; Training raw bit 11; DLLLA bit 13 | 本次读取时的当前状态；不等价于历史状态 |
| Link Capabilities 2 (`LnkCap2`) | `cap+0x2c`, 32-bit，仅本规格接受的 version `2` | supported speed vector `[7:1]` | PCIe 3.0+ 支持速率集合；与 LnkCap legacy maximum speed 分字段返回 |

Link speed code `1..6` 分别是 2.5、5、8、16、32、64 GT/s。`0`、`7..15` 不解码为速率，保持 raw code、`speedValid=false`、`speedUnresolved=true`。宽度为提取后的原始 lane 数；`0` 或 reserved/不合理值不转换为 `x0` 等有效链路结论。`LnkCap` 的 max width 必须和 `LnkSta` 的 negotiated width 分开命名、存储、呈现。若读取 v2 `LnkCap2`，返回完整 raw DWORD 和 speed-vector raw mask；最高置位只能作为“该 function 声明支持的最高速率”，不能覆盖 `LnkCap` raw 或 `LnkSta` 当前速率。

Link Training bit 11 的 Linux 定义只证实掩码与名称，没有在该头文件注明适用 port type；因此只要 `LnkSta` 可读就保留 `trainingRaw`，但在本规格里 `trainingMeaningValid=false`（未证实），不得向用户显示为已确认的 training 状态。后续若实现者用 PCIe 基础规范确认 version 1/2 Endpoint 与 Legacy Endpoint 的适用条件，可逐类型/版本把该 semantic validity 打开，并增加相应 fixture；即使届时 `trainingRaw=0`，也不能解读为链路稳定历史。DLL Link Active bit 13 只有当 `LnkCap` bit 20（DLLLA reporting capable）为 1 时才标 `dllActiveValid=true`；否则仍返回 `LnkSta` 原始值，但 `dllActiveValid=false`，UI 显示 unsupported/unknown 而不是 inactive。状态 raw 可读但 speed code / width 含保留值时，raw 仍有效，解码字段各自无效。

### 边界、错误、热拔除与不一致

- `configRead16/32` 没有错误码。全 1 (`0xffff` / `0xffffffff`) 必须视为 `read-failed-or-removed` 候选，停止后续 capability 解码；不能作为可解码值。全 0 不可一概当成读取失败，因为合法寄存器字段/空状态可能为零；结合所在字段及 capability 结构判断。
- 所有 offset 算术使用更宽整数先验证：对 `LnkCap` 要求 `cap+0x0f <= 0xff`；对 `LnkSta` 要求 `cap+0x13 <= 0xff`；对 version 2 `LnkCap2` 要求 `cap+0x2f <= 0xff`。Capability pointer 再靠近 config-space 末尾、导致任一字段越界时，返回 `truncated-capability`，不得 UInt8 回绕。至少保证 cap dword、LnkCap 与 LnkSta 自然对齐。
- 每次 config read 都可能遇到热拔除或链路状态变化。读取前后身份变化、全 1、能力链结构变化时标注 `inconsistent-or-removed`，停止并保留已取得的 raw/validity。不要重试到形成“看似稳定”的快照；读取 API 没有提供原子批量快照保证。对于同一查询中的 capability header 与 link registers 分次读取，也不得声称它们同步于同一硬件时刻。
- 通过 selector admission / try-lock 与现有 reset/FLR 序列互斥仅能降低本驱动并发写入造成的混读，无法锁住 PCIe 上游设备或硬件自动 retraining。
- 本查询是有限次 config-space read：最多 48 个节点，按发现 PCIe capability 后读取固定字段，不递归扫描拓扑、不写 config、不操作 BAR、不触发命令、不分配 DMA。

## 输出建议

版本化结构至少包括：`version`、`structBytes`、整体 `status` / `reason`、`headerTypeRaw`、`statusRaw`、`capPointerRaw`、`capOffset`、`capWalkCount`、`capabilityIdRaw`、`pcieFlagsRaw`（来自 `cap+0x02`）、`pcieVersion`、`devicePortType`、`lnkCapRaw`、`lnkStaRaw`、`lnkCap2Raw`、以及逐字段 validity：`capListValid`、`pcieCapValid`、`linkFieldsValid`、`maxSpeedValid`、`maxWidthValid`、`currentSpeedValid`、`currentWidthValid`、`trainingRawValid`、`trainingMeaningValid`、`dllActiveValid`、`lnkCap2Valid`。raw 未读字段清零，同时 validity 为 false；不可用 magic raw 值单独代替有效位。`currentSpeedCode` 与解码后的 GT/s 是不同字段；未知 code 必须原样返回。

运输层可以对“格式错误 / 参数错误”返回 IOReturn 错误；设备查询本身应将 no-cap、malformed、read-failed 等作为结构化查询结果返回，避免 transport failure 把诊断证据抹掉。UserClient 需和现有 `queryadapter` 一样严格检查零 scalar input/output、无 structure input、structure output 精确大小；当前 `doQueryAdapter()` 已提供该模式的例子，dispatch 表及 selector 数组均按位置对齐，不应插入中间项。

## 验收夹具（后续实现，不在本任务运行硬件）

所有 fixtures 用可注入 config-read stub 喂寄存器返回值，验证字段与状态，不访问真实 IOKit service：

| Fixture | 配置 | 期望 |
|---|---|---|
| 正常 Gen1 x1 当前链路 | Status capability-list=1；合法 cap 链；PCIe ID 0x10 / version1 / Endpoint；LnkCap=Gen3 x8；LnkSta=Gen1 x1、Training=0 | current=2.5 GT/s x1，max=8 GT/s x8，全部分别有效；两套值不混写 |
| Gen3 x4，Training raw bit置位 | LnkSta speed code3 / width4 / bit11=1 | current raw 与 decoded 8 GT/s x4；`trainingRaw=1`、`trainingMeaningValid=false`，不声称Endpoint正在训练 |
| DLLLA 不支持 | LnkCap bit20=0，LnkSta bit13=0 | LnkSta raw 保留；`dllActiveValid=false`，不能报告 DLL inactive |
| 未知 speed code | LnkSta speed=7 或 0 | raw code 原样，current speed decoded invalid/unresolved，不推测速率 |
| 无 conventional capability list | status bit4=0 | `capListValid=true`（已读到 status），PCIe capability 不存在/未遍历；`no-pcie-capability`，不读 0x34 |
| 合法链尾但无 PCIe ID | 指针合法，ID 序列正常，next=0 | `no-pcie-capability`，不是 read error |
| 越界 / 非对齐 / 环链 / 最大长度 | 首指针 0x3c/0xfd，next低位异常，重复节点，或48个不同合法节点 | 分别malformed / cycle；48节点链正常结束仍可返回no-cap。该地址范围只有48个对齐位置，不伪造49个不同合法节点；另用读取次数证明48节点上界 |
| capability / status read 全 1 | 对应 read 返回 all-ones | `read-failed-or-removed`；不把 0xff ID 当 no-cap，不生成 link decode |
| 热移除或不一致读取 | 遍历成功后 identity/read 变全 1，或重新检查结构变化 | `inconsistent-or-removed`；保留成功读过的 raw 与有效位，不做快照声称 |
| capability 靠近 8-bit config 末尾 | cap 使任一目标寄存器越过 0xff | `truncated-capability`；无 offset 回绕 |
| Type 9 / Type 10 或未知 type | `cap+0x02` flags 中 type 不受支持 | raw flags 返回；link fields invalid/unsupported；不按 endpoint 解读 |
| v0 / v1 / v2 / future version | `cap+0x02` flags 中 version 分别为 0/1/2/>2 | v0 与 future version 只回 raw flags，不解码 link layout；v1 不访问 LnkCap2；v2 读取并单独报告 LnkCap2 raw/vector |
| Training bit applicability | LnkSta 可读，bit11 为 0/1，但未取得注明 endpoint 适用性的规范证据 | raw 保留；`trainingRawValid=true`、`trainingMeaningValid=false`，不得报告语义值 |
| UserClient 输出 ABI | selector 参数有任一意外 scalar/input，structure 缺失或 size 不精确 | `kIOReturnBadArgument`，不访问 PCI config；符合现有 selector 的参数门禁 |
| try-lock 被占 | transaction admission busy | 及时拒绝/返回 busy，不等锁、不读 config、不跳过 reset exclusion |

fixture 必须证明查询只调用 config read、没有 config write / BAR write / firmware command / DMA allocation；该静态或 stub 证据只证明实现路径，不证明硬件当前链路状态。

## 不可推断项与未决假设

- 不从历史 Linux 日志推断当前为 x4，也不把 endpoint `LnkCap`、`LnkCap2` 或任何历史训练记录当成当前 negotiated link。
- Link speed / width 不是吞吐量；也不能据此声称双口总带宽、实际 payload、链路稳定性或可持续性能。
- PCI config reads 不是被动的历史快照，不保证多寄存器原子性；不能证明 retraining 没发生、DMA 已静止、设备没有 reset，或硬件已授权/安全。
- 所有 0xffff/0xffffffff 判为读失败候选是保守的读取约定，不是 Apple API 文档承诺的 IOReturn 映射；需用 stub 和真实平台验证失败时返回值。`IOPCIDevice` read API 不暴露失败状态，因此输出 reason 应叫 `read-failed-or-removed`，不能断言具体是热拔除。
- 报告限定 CX3 function 为 PCIe Endpoint/Legacy Endpoint；若目标 PCIe function 暴露其他 port type，应先更新设计和 fixtures，不放宽为默许解析。
- config offset 上限是本查询对conventional PCI config space的范围选择；当前公开 `IOByteCount` overload 能传更宽offset，不得把本设计上限外推为Apple API限制。

## 证据与定位

- Apple SDK（本机 macOS 15.5）IOKit API：`/Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk/System/Library/Frameworks/Kernel.framework/Versions/A/Headers/IOKit/pci/IOPCIDevice.h:461-469`（`UInt8` offsets），`:484-505`（IOByteCount overload 和对齐 / 返回值文档）。本机 vendored copy：`/Users/you/Documents/Hackintosh-MacLocal/outputs/20261013-applevtd-amdvi/impl/3rdparty/MacKernelSDK/Headers/IOKit/pci/IOPCIDevice.h`。
- Apple PCIe capability ID：`/Library/Developer/CommandLineTools/SDKs/MacOSX15.5.sdk/System/Library/Frameworks/Kernel.framework/Versions/A/Headers/IOKit/pci/IOPCIFamilyDefinitions.h:65-82, 160-162`（PCI Express conventional capability ID = `0x10`）。
- Current implementation being superseded: `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Mlx4Hw.cpp:422-435` (`findPcieCapability`); PCI identity read: same file `:560-576`.
- Existing ABI selector boundary: `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/cx3_abi.h:274-275` (selector 47, count 48); `Cx3AdapterResult` fixed size/offset assertions `:828-830`.
- UserClient examples: `/Users/you/Documents/Hackintosh-MacLocal/tools/cx3-mlx4/src/Cx3Mlx4UserClient.cpp:89-100` (positional dispatch), `:202-216, 301-302` (lock classes and selector rows), `:1525-1539` (exact structure-output admission). `kXactNoLock` is explicitly refused at `:556-560`.
- PCIe register masks and semantic names verified from primary Linux PCI definitions using authenticated GitHub API (`torvalds/linux`, `include/uapi/linux/pci_regs.h`): https://github.com/torvalds/linux/blob/master/include/uapi/linux/pci_regs.h#L56 (Status Cap List), `#L122` (cap pointer), `#L234` (PCIe cap ID), `#L483-L490` (PCIe flags, version and port types), `#L545-L563` (LnkCap and DLLLA capability), `#L577-L595` (LnkSta fields; Training is only a bit mask/name here, not a port-type applicability statement), `#L693-L700` (LnkCap2 speed vector). These definitions were fetched via `gh api`; no direct raw/CDN access.

