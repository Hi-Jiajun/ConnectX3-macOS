# T04：观测检查点与危险实验门禁

2026-10-22；**PARTIAL / 观测快照存在；危险实验未放行**。
报告指向23:32采集的历史状态，不表示阅读时仍在线。重新实验前必须重新采集、核对回执与暂停工作窗口。

证据：工作区外层`outputs/20261022-cx3-fulladapt-batch1/runtime/`；manifest记录命令、退出码和UTC时间。
`check-readonly-checkpoint.py`仅消费这些保存文件；绝不打开user client、执行命令或授予硬件写权限。

| 项目 | 23:32快照 | 证据边界 |
| --- | --- | --- |
| 身份 | 0.44.47 / UUID AA1A2B9E-6BA0-3A54-A28B-553A4F12C6A2；安装binary SHA256与M4一致 | boot20:34:50；未换装0.44.48 |
| 独立管理 | en0 192.0.2.196；到192.0.2.60路由interface=en0 | en2为CX3，不将它当管理退路 |
| 本机host probe | heartbeat age1.6s，exec26.4ms，disk15.4ms | 服务活着不代表所有故障可捕获 |
| 远端recorder/alert | 两service active/running，NRestarts0，state.log持续更新 | 未验证这次手机端显示/锁屏提醒 |
| 面包屑UDP | 9997监听、fsync接收进程存在 | listener存在不等于本次phase消息已远端落盘 |
| 专用phase-watch | pgrep exit1，未运行 | 不能把一般host-watch声称为专用阶段覆盖 |
| 手机与锁屏 | 本批未重新演练/确认 | 过去“收到但不弹”已接受；仍不能假设用户忙别的时会注意 |
| P1-S | 安全审计4项红 | 不因观察者在线而豁免 |

远端原始输出中`.alert-offset`无换行，与events文件长度拼接；不将`421631421631`当成两个已解析字段。
outbox数0只表明快照没待发文件，不证明手机收到。下轮精确采集须分别加字段名/JSON输出。

## 离线校验与结果语义

```
python3 outputs/20261022-cx3-fulladapt-batch1/test-readonly-checkpoint.py
python3 outputs/20261022-cx3-fulladapt-batch1/check-readonly-checkpoint.py --at-capture --require-hardware-ready
```

第一条7项离线测试；第二条是historical replay，**预期exit79、hardware_write_permitted=false**。
不带`--at-capture`会检查快照老化。该工具目前是本批检查点，不是通用自动放行器；版本/UUID/哈希刻意固定本次审查对象。
每个run的保存目录、manifest、回执和实际phase必须对应，不能只export某个OK环境变量。

## 下一次有风险实验的前置与停止条件

1. 审查完成并通过P1-S候选的离线故障注入；明确只允许本轮最小实验，禁止自动串全链。
2. 用户当轮明确工作已暂停；恢复入口/强制关机风险、单一硬件owner、独立en0+远端记录器核对。
3. 专用phase-watch启用并先发唯一run ID的无故障面包屑，确认远端持久化；通知演练明确标“演练/非真实故障”。
4. 手机接收与锁屏弹窗分开登记；若只接受非弹窗接收，本轮须有人值守且不依赖用户忙别的时注意提醒。
5. 每个阶段前后身份、持久化phase、单调超时；探针/独立链路失联、资源不变量冲突、固件timeout时停止后续写入，不自动重置/重启。
6. 失败后先取远端最后phase/心跳、boot与panic证据，区分kernel panic/硬挂/仅数据链路/仅USB；未有退出凭证不释放未知DMA资源。

T04未完成新手机回执/专用phase落盘演练，也未完成危险测试门禁闭环。当前可继续离线工作，不需要用户现在重启。
