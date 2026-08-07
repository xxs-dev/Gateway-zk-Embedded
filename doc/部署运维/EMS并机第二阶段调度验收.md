# EMS 并机第二阶段调度验收

## 1. 验收边界

本流程在第一阶段组网、选举、柜号和分区测试全部通过后执行。协调器只交换能力和目标并更新 `ems_cluster_store`，不会直接写 PCS。最终写入必须经过：

```text
724030-724035 本柜已接受目标
  -> Graph EMS clusterDispatch
  -> phaseArbiter
  -> powerConstraint
  -> PCS PendingWriteCommand
  -> 724040-724045 实际反馈
```

生产默认值是 `emsCluster.controlEnabled=false` 和 `724000=0`。任一映射、方向或反馈不明确时保持关闭。

功率符号固定为：正值表示放电/向外送出，负值表示充电/从外部吸收。不得在不同柜上使用相反方向。

## 2. 启用前配置

1. 所有柜使用相同 `clusterId`、PSK、成员数、调度周期和 `724xxx` 基址。
2. 保持 `controlEnabled=false`，先确认每台柜只有一个稳定主控或从控身份，`quorumValid=true`。
3. 在 Graph EMS 中周期刷新下列本柜能力点：
   - `724020`：SOC，范围 `0-100`。
   - `724021/724022`：额定有功 kW、额定视在功率 kVA。
   - `724023/724024/724025`：可充、可放、可用无功，均填写正的能力绝对值。
   - `724026`：本柜控制链就绪。
   - `724027`：急停、消防、BMS 或 PCS 闭锁汇总，`1=闭锁`。
   - `724028`：人工或高优先级控制接管，`1=接管`。
4. 把 PCS 实际三相 P/Q 反馈映射到 `724040-724045`。
5. 主控站级策略把三相 P/Q 总目标写入 `724060-724065`。六个点必须在同一计算周期刷新，不使用无功时也要持续写 `0`。
6. Graph 增加一个 `clusterDispatch` 节点，并把其本柜输出接入 `phaseArbiter` 候选，再接 `powerConstraint`。该节点不得直接连接设备目标端口。
7. `activeOutputIndexes/reactiveOutputIndexes/validOutputIndex/stationLeaderOutputIndex/reasonOutputIndex` 必须同时登记为本工程 ComputeEngine 输出或 EMS 虚拟点路由；启动时报 `route not found` 时禁止绕过校验。

`clusterDispatch` 必填参数：

| 参数 | 建议值或用途 |
| --- | --- |
| `enableIndex/roleIndex/quorumIndex` | `724000/724001/724005` |
| `dispatchActiveIndexes` | `[724030,724031,724032]` |
| `dispatchReactiveIndexes` | `[724033,724034,724035]` |
| `dispatchValidIndex/dispatchReasonIndex` | `724050/724051` |
| `stationStrategyActiveIndex` | `724052`，只允许有效主控运行站级策略 |
| `activeOutputIndexes/reactiveOutputIndexes` | 本工程预留的 3+3 个 Graph 虚拟 Index |
| `validOutputIndex` | 本柜调度是否可进入仲裁 |
| `stationLeaderOutputIndex` | 主控站级策略门控 |
| `reasonOutputIndex` | 调度拒绝码，供画面、告警和联锁使用 |
| `maxTargetAgeMs` | 不大于 `dispatchTtlMs`，推荐 `3000` |
| `zeroOnInvalid` | 生产必须为 `true` |

常用拒绝码：`0=接受`、`1=本柜限幅后接受`、`10=控制未启用`、`11=无多数派`、`12=主控身份不符`、`13=任期不符`、`14=成员版本不符`、`15=旧序号`、`16=目标过期`、`17=能力过期`、`18=本柜未就绪`、`19=闭锁`、`20=人工接管`、`21=目标非法`。

## 3. 无写回联调

1. 保持 `controlEnabled=false` 和 `724000=0`。
2. 检查 `/opt/modbus-gateway/run/ems-cluster-status.json`：成员 `capabilityFresh=true`，SOC、额定功率、可充放能力和实际反馈符合现场。
3. 检查 `724020-724028` 和 `724040-724045` 每个动态点的时间戳不超过 `capabilityTtlMs`。
4. 临时写入小的站级目标，确认 `controlEnabled=false` 时 `724050=0`、`724051=10`，PCS 写队列没有新增命令。
5. 分别模拟闭锁和人工接管，确认状态文件返回 `interlocked`、`manual_override`，旧目标不会保留为有效状态。

## 4. 低功率真实验收

1. 先把所有站级目标置零，再把各柜 `controlEnabled` 改为 `true` 并重启协调器。
2. 确认所有节点仍为同一任期和成员版本，且 `controlWritesEnabled=true`。
3. 设置 `724000=1`。先下发每柜绝对值不超过 `10 kW`、无功为 `0` 的三相平衡目标。
4. 检查主控分配总和接近站级目标；高 SOC 柜承担更多放电，低 SOC 柜承担更多充电。
5. 检查每个成员的 `dispatch.requested`、`dispatch.accepted`、`dispatch.code` 和实际反馈。`accepted/clamped` 只表示本柜目标入口接受，不等于 PCS 已执行成功。
6. 确认 Graph `validOutputIndex=1` 后才产生 PCS 写命令，且 `powerConstraint` 的 P/Q/S、SOC、BMS 和正反送限制仍生效。
7. 验证实际反馈与接受目标的误差、到达时间和写回结果，不能只看 TCP ACK。

## 5. 故障注入

依次执行并记录：

1. 停止主控协调器：旧主控目标在 TTL 内失效并清零，多数派选出新主控。
2. 三柜隔离旧主控：少数派 `quorumValid=false`，不得继续站级策略。
3. 两柜拔线：双方退出有效调度，不能各自成为单柜主控。
4. 停止任一从柜能力刷新：超过 `capabilityTtlMs` 后主控不再给该柜分配非零目标。
5. 停止站级目标刷新：超过 `stationTargetTtlMs` 后主控停止新调度。
6. 设置 `724027=1` 或 `724028=1`：本柜立即拒绝新目标，旧目标失效。
7. 注入旧任期、旧成员版本、重复序号和超长 TTL：均应返回明确拒绝码。

## 6. 回退

按顺序执行：

1. 把 `724000` 设为 `0`，确认本柜候选目标归零。
2. 把所有柜 `emsCluster.controlEnabled` 改为 `false`。
3. 重启 `ems-cluster@mqtt-service.service`，确认状态为 `controlWritesEnabled=false`。
4. 保留 `emsCluster.enabled=true` 可继续观察选举；需要完全退出并机时再设置 `enabled=false`。

回退不得删除 PCS 本地保护、Graph `powerConstraint` 或反馈校验节点。
