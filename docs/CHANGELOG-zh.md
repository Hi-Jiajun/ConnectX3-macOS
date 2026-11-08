
`.136`当前增量：独立双口原生接口已接通真实混杂/组播mode/list配置和IPv4/IPv6 checksum路径；worker在PF生命周期/try-only事务内执行本口命令，pending/applied分开发布。完整smoke构建与关键合成简验通过，未安装/上机；线上过滤/校验和仍待验收。下一项MTU/jumbo及DMA几何、VLAN/MAC/电源。见[.136交付](../../outputs/20261025-cx3-native-filters/README.md)。下方版本段落保留各批历史。

`.133`开发增量：实际CQ/RQ/SQ/QP/MAC/raw状态已迁入固定的每口queue owner对象，共享MR/ICM/profile/reset依赖覆盖两口；受影响路径离线简验和完整smoke构建通过。第二口setup/编号/原生接口仍待接通，未安装候选。见[.133交付](../../outputs/20261025-cx3-port-queues/README.md)。

双口管理开发切片 `.132`：新增每口查询/MTU/INIT/CLOSE状态与 `cx3ctl portinfo PORT [refresh] [json]`（默认host cache），修正端口2覆盖端口1与共享HCA关闭门禁。生产管理链离线简验与完整smoke构建通过，尚未上机；收发队列/steering与原生接口仍单口，双口网络未完成。见[.132交付](../../outputs/20261025-cx3-port-state/README.md)。

自动启用开发入口：`cx3ctl autonet plan`仅输出离线计划；`autonet status|enable|disable EXPECTED_BUILD [json]`调用追加selector51。`tools/cx3-network-service.py --config <绝对路径>`默认dry-run，显式`--execute`且配置enabled=true才启用；不自行安装、不重试。仅端口1标准MTU，要求空实例首次初始化；版本/boot/service绑定与阶段错误停止。disable暂停主机交付，不关闭硬件或释放DMA。新候选尚未上机，当前`.47`不变。见[.131交付与限制](../../outputs/20261025-cx3-auto-network/README.md)；完整验收遵循[最终测试计划](docs/CX3-PRO-FINAL-TEST-PLAN.md)。

**Development workflow (2026-10-25):** [feature-first plan v4](docs/CX3-PRO-FEATURE-FIRST-PLAN.md) now prioritizes production feature implementation with brief per-batch checks. Use `CX3_TEST_PROFILE=smoke` for development builds; the default `standard` retains the original build checks. Both compile the complete kext/CLI and check boot-path, symbols and ABI. Comprehensive regression, stress and endurance work is deferred to the [final test plan](docs/CX3-PRO-FINAL-TEST-PLAN.md). A smoke build is not hardware or release acceptance.

**Status (updated 2026-10-25): M4 Ethernet networking passed briefly on 0.44.47,
and an earlier boot later developed TX errors and lost DHCP. After the 05:38 reboot,
existing 0.44.47 was enabled again: en2 acquired 192.0.2.201 and interface-bound
Internet HTTPS returned 200. Default routing remains on en0. Daily-use stability
and full hardware functionality have NOT passed.**

See the [latest temporary-network verification](../../outputs/20261025-cx3-temporary-dhcp-055750/README.md)
and the earlier [runtime incident](../../outputs/20261025-cx3-dhcp-loss-readonly/REPORT.md).
[0.44.129-local](../../outputs/20261025-cx3-tx-fault-containment/README.md) adds offline-tested
TX first-fault evidence and stops new posts after an error; it is not installed, hardware
accepted or a fix for the first QP error.

Normal-mode own-MAC unicast, DHCP, ARP, IPv4/IPv6 and full-MTU traffic passed on
this machine. See the [M4 report](../../outputs/20261022-cx3-m4-preflight/M4-PASS-0.44.47-20261022-2046.md)
and [follow-up snapshot](../../outputs/20261022-cx3-m4-preflight/POST-M4-0.44.48-20261022-2252.md).
These evidence bundles live in the parent workspace; a standalone checkout needs the
corresponding run artifacts. This is a dated snapshot, not a live health assertion.

The new [full-adaptation plan](docs/CX3-PRO-FULL-ADAPTATION-PLAN.md) defines the project
destination, [capability matrix](docs/CX3-PRO-CAPABILITY-MATRIX.md),
[open decisions](docs/CX3-PRO-DECISIONS.md) and [task dependencies](docs/CX3-PRO-BACKLOG.md).
The goal includes all verified hardware-supported capabilities, with native networking
first and a companion user-space SDK/tools for capabilities without a native macOS API.
RDMA, VPI/InfiniBand, multi-queue/RSS, timestamping and SR-IOV are not silently excluded.
This roadmap is not a claim that every candidate is supported or implementable on macOS.

The development candidate `0.44.130-local` adds fixed module EEPROM GETs via
`cx3ctl module PORT OFFSET LENGTH [json]`, plus `tools/decode-module.py` for saved
SFP/QSFP data and basic DOM values. The kernel/CLI build and targeted host checks
pass; it is not installed or hardware accepted. Page 0 only; external calibration,
extended pages and remaining diagnostic features are still incomplete. See the
[module implementation record](../../outputs/20261025-cx3-module-get/README.md).

