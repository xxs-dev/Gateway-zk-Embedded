# 边端 AGC / AVC 独立模块设计

## 1. 目标

在边端新增独立 `AgcAvcController` 模块，用于接收调度目标并闭环控制储能 PCS：

- AGC：跟踪有功功率目标，可扩展频率下垂调节。
- AVC：支持无功功率、电压或功率因数目标。
- 多台 PCS 的 P/Q 指令统一分配，并遵守额定功率、SOC、BMS 可充放功率和通信状态限制。
- 所有设备写入继续走共享内存写队列和现有驱动，不直接调用 Modbus、IEC 或其他协议客户端。
- 与 EMS 策略、人工控制和高优先级控制明确互斥，不能同时向同一 PCS 功率点写入。

该模块第一版默认影子运行，只计算、不写设备。现场完成输入映射、方向校验、限幅和反馈验证后，才能显式开启真实写回。

## 2. AGC / AVC 定义

### 2.1 AGC

AGC 负责有功功率闭环。边端接收站级有功目标 `P_target`，读取并网点实际有功 `P_pcc`，计算误差并生成各 PCS 的有功目标。

统一符号约定：

- 正有功：储能向电网放电、站点向外送电。
- 负有功：储能从电网充电、站点从外部吸收功率。
- 厂家寄存器方向不一致时，由设备模板或执行器映射转换，AGC 内核只使用统一符号。

第一版支持：

1. `dispatchP`：直接跟踪调度总有功目标。
2. `pccClosedLoop`：以并网点实际有功为反馈进行闭环修正。
3. `frequencyDroop`：在调度目标上叠加频率下垂补偿，可独立关闭。

### 2.2 AVC

AVC 负责无功和电压闭环，三种模式互斥：

1. `reactivePower`：跟踪站级无功目标 `Q_target`。
2. `voltage`：根据母线或并网点电压偏差计算无功目标。
3. `powerFactor`：根据当前有功和目标功率因数计算无功目标。

统一无功符号必须在项目交付时确认，并在配置中填写 `reactiveSignConvention`。不能依赖不同厂家默认约定。

## 3. 为什么是一个独立进程

建议增加一个二进制和一个 systemd 实例服务：

```text
/opt/modbus-gateway/bin/AgcAvcController
agc-avc@agc-avc-service.service
/opt/modbus-gateway/config/runtime/apps/agc-avc-service.json
```

AGC 和 AVC 放在同一进程，但内部保持两个控制器：

```text
DispatchIngress
      |
      v
InputSnapshot -> SafetyGate -> AGC Controller ----+
                            -> AVC Controller ----+--> P/Q Capability Limiter
                                                  --> PCS Allocator
                                                  --> Write Coordinator
                                                  --> Feedback Verifier
```

不直接放入现有 `GraphEmsEngine`，原因如下：

- AGC/AVC 是持续闭环控制，需要独立状态机、积分状态、命令时效和无扰恢复。
- P/Q 必须同时经过 PCS 视在功率约束，不能由两张互不知情的策略图分别写入。
- 独立服务可以单独启动、停止、升级、回退和观察，不影响采集、MQTT、EventEngine 和普通 EMS 计算。
- Graph EMS 仍可生成本地计划目标，但最终设备写回必须交给控制权仲裁，不能与 AGC/AVC 双写。

## 4. 与现有代码的复用关系

### 4.1 直接复用

| 现有能力 | AGC/AVC 用法 |
| --- | --- |
| `PointStoreRouter::getLatestByIndexes` | 每周期批量读取一致输入快照 |
| `StoredPointValue` | 检查值、质量、时间戳和 stale 状态 |
| `PointStoreRouter::putLatestByIndex` | 写入控制状态、误差和影子输出虚拟点 |
| `PendingWriteCommand` | 提交 PCS 真实写入 |
| `PointStoreRouter::submitWriteCommand` | 校验目标路由和 `write.enable` |
| `WritebackResultRecord` | 获取排队、写入、校验和总耗时 |
| `PriorityControlLease` | 检测人工高优先级控制并暂停自动控制 |
| `gateway-services.sh` | 根据 `agcAvc.enabled` 决定是否启动服务 |

### 4.2 不直接复用的部分

- 不直接复用 Graph EMS 的 `controlWrite` 作为 AGC 循环。其适合离散策略节点，不负责完整闭环状态机。
- 不把连续 AGC/AVC 写入标记为 `highPriority`。当前高优先级租约会暂停其他处理，持续占用会导致采集反馈被饿死。
- 不在 AGC/AVC 中创建第二套 Modbus、IEC 或 MQTT 设备驱动。

### 4.3 需要新增的公共能力

现有 `PriorityControlLease` 是单命令租约，驱动完成一个 `cmdId` 后会释放，不适合作为长期自动控制所有权。需要增加轻量的 `PowerControlOwnership`：

```json
{
  "scope": "pcs-power",
  "owner": "agc-avc",
  "sessionId": "AGC_AVC_20260716_001",
  "priority": 50,
  "heartbeatAt": 1784188800000,
  "expireAt": 1784188803000
}
```

规则：

1. AGC/AVC 激活前获取 `pcs-power` 所有权，并每周期续租。
2. Graph EMS 的 `pcsWriteback/controlWrite` 在提交 PCS 功率写入前检查所有权。
3. 普通 MQTT 写 PCS 功率点时，如果已有自动控制所有权则拒绝；非功率控制点不受影响。
4. 人工 `highPriority=true` 可抢占。AGC/AVC 检测到高优先级租约后进入暂停状态，不再提交新写入。
5. 服务退出、禁用或心跳超时后所有权自动失效，不留下永久锁。

该所有权只阻止冲突写入，不暂停采集。采集必须继续运行，为闭环提供反馈。

### 4.4 通过共享内存复用现有点位，不直接依赖采集驱动

AGC/AVC 使用的大部分输入和写入目标已经由 Modbus、IEC、DLT645、CAN、DIDO 等驱动注册到共享内存路由。模块不读取这些驱动的配置文件、不持有驱动对象，也不调用协议接口，只建立“控制语义角色 -> 共享内存 Index”的绑定：

| 类型 | 示例 | 处理方式 |
| --- | --- | --- |
| 设备采集输入 | 并网点 P/Q/电压/频率、PCS 实际 P/Q/状态/温度、BMS SOC/可充放功率、消防和急停 | 通过 `sharedLatest` 读取现有 Index，保留原质量和时间戳 |
| 设备写入目标 | PCS 有功、无功、功率因数、启停控制点 | 通过 `sharedWriteback` 向现有 Index 提交命令，由原驱动消费 |
| 静态设备参数 | 铭牌功率、视在功率、额定电压、厂家能力曲线 | 保存在设备能力配置；厂家支持在线读取时可选绑定采集点覆盖 |
| 调度命令邮箱 | P/Q/电压/PF 目标、sequence、issuedAt、TTL、来源 | 新建 AGC/AVC 命令虚拟点 |
| 计算与诊断输出 | 有效目标、误差、限幅原因、分配结果、状态、失败计数 | 新建 AGC/AVC 输出虚拟点 |

点位引用统一使用 `PointRef`，不能只保存一个含义不明的裸 Index：

```json
{
  "source": "sharedLatest",
  "semanticRole": "pcs.actualActivePower",
  "machineCode": "COMM202600102",
  "meterCode": "PCS_1",
  "pointCode": "PCS_ACTIVE_POWER",
  "index": 1330,
  "required": true
}
```

其中 `index` 是运行期快速访问的主键，`machineCode/meterCode/pointCode` 是配置校验和迁移保护。启动时由共享内存路由元数据确认四者一致；工程重新排序导致 Index 变化时，由 Windows/平台重新解析并更新绑定，不能悄悄读到相同 Index 上的另一个点。

语义角色必须带设备作用域，例如 `site.pcc.activePower`、`pcs.actualActivePower`、`bms.soc`，不能只写 `activePower`。同一角色允许绑定多台设备，但同一设备的单值角色只能有一个主绑定。三相量使用 `phaseA/phaseB/phaseC` 子角色或数组绑定。

实现时增加 `AgcAvcPointBindingResolver`：

1. 从统一工程配置加载已生成的全局点位路由快照和 AGC/AVC 语义绑定，不单独解析各驱动文件。
2. 校验点存在、设备归属、数据类型、单位、读写方向和唯一性。
3. 将绑定一次解析为 Index 缓存，控制循环不扫描 JSON、不按名称查找。
4. 批量调用 `PointStoreRouter::getLatestByIndexes`，直接使用采集值、质量、时间戳和 stale 状态。
5. 写入时使用同一个现有点位 Index，通过 `PointStoreRouter::submitWriteCommand` 写入共享命令队列，不建立影子写点、不直接调用驱动。

现有配置中的部分 `pointRole` 尚未由普通 `PointDefinition` 统一解析。实施时应把标准 `semanticRole` 加入点定义模型和全局路由元数据，或者由独立绑定表维护；不能依赖当前未被运行时读取的扩展字段。

### 4.5 共享内存隔离边界

为防止 AGC/AVC 越过模块边界，增加一个窄接口 `IAgcAvcPointBus`，内部复用现有 `PointStoreRouter`：

```text
readSnapshot(indexes)          -> getLatestByIndexes
publishVirtual(index, value)   -> putLatestByIndex
submitControl(command)         -> submitWriteCommand
readWriteResult(cmdId)         -> WritebackResultRecord
```

约束如下：

1. `readSnapshot` 可以读取绑定的现有采集点和命令虚拟点，但不能修改它们。
2. `publishVirtual` 只能写 AGC/AVC 自己拥有的状态、计算和诊断 Index，禁止覆盖设备采集点。
3. 真实设备控制只能调用 `submitControl` 进入共享写回队列。直接 `putLatestByIndex` 不代表设备控制成功，也禁止用它绕过写回校验。
4. 写回队列由现有采集驱动消费，AGC/AVC 不知道目标来自 Modbus、IEC、CAN 或其他协议。
5. 控制模块不链接具体驱动实现，不读取串口、网口和协议配置，不创建新的 MQTT 或设备连接。
6. 调度命令写入遵循“参数字段先写、`sequence` 最后写”的提交顺序；控制器只有在 sequence、issuedAt 和各目标属于同一版本时才接受，避免读到半包命令。
7. AGC/AVC 输出使用独立共享内存设备和 Index 区间。模块停止或故障不会影响原采集共享内存，采集驱动停止也不会破坏控制器自身诊断区。

这样语义绑定属于配置层，实际数据面仍然完全是共享内存。更换厂家、协议或采集驱动时，只需要更新共享 Index 绑定，AGC/AVC 算法和服务无需修改。

## 5. 模块边界

建议代码拆分：

```text
agc_avc_main.cpp
include/edge_gateway/agc_avc_service.hpp
include/edge_gateway/agc_avc_controller.hpp
include/edge_gateway/power_control_ownership.hpp
src/agc_avc_service.cpp
src/agc_avc_controller.cpp
src/power_control_ownership.cpp
deploy/agc-avc@.service
```

职责：

- `AgcAvcService`：周期调度、快照读取、状态机、输出和审计。
- `AgcController`：有功目标、死区、PI、频率下垂和爬坡限制。
- `AvcController`：Q/电压/功率因数模式及无功闭环。
- `PqCapabilityLimiter`：P/Q 视在功率圆、设备能力和优先级裁剪。
- `PcsAllocator`：多 PCS 权重分配、不可用设备剔除和剩余量二次分配。
- `WriteCoordinator`：去重、最小写间隔、批次 cmdId、写回结果和失败退避。
- `PowerControlOwnership`：自动控制写入所有权。

## 6. 运行状态机

```text
DISABLED
   |
   v
STANDBY --有效使能、命令新鲜、输入正常、取得控制权--> ACTIVE
   ^                                                   |
   |                                                   |
   +--命令失效/退出后平滑归零-- RAMP_TO_ZERO <---------+

ACTIVE --人工高优先级控制--> PAUSED_BY_PRIORITY
ACTIVE --部分 PCS 不可用--> DEGRADED
ACTIVE --关键输入无效/连续写失败--> FAILSAFE
```

状态说明：

| 状态 | 行为 |
| --- | --- |
| `DISABLED` | 服务或模块关闭，不计算、不写入 |
| `STANDBY` | 服务运行但没有有效调度命令，只发布状态 |
| `ACTIVE` | 正常闭环计算和写回 |
| `DEGRADED` | 部分设备退出，由剩余 PCS 承担能力范围内目标 |
| `PAUSED_BY_PRIORITY` | 人工高优先级控制期间停止自动写入，继续采集和计算影子值 |
| `RAMP_TO_ZERO` | 命令超时或正常退出时按配置速率把目标降为零 |
| `FAILSAFE` | 关键输入无效、配置冲突或连续写回失败；默认禁止继续写入 |

从暂停或故障恢复时必须重新读取实时 P/Q，并以当前设备值初始化限速器和积分器，避免目标跳变。

## 7. 控制周期

每个周期执行固定步骤：

1. 批量读取命令、并网点、BMS、PCS 和联锁点。
2. 校验质量、stale、最大数据年龄和调度命令 TTL。
3. 检查急停、消防、BMS 禁充禁放、PCS 通讯和高优先级租约。
4. 计算 AGC 原始有功目标。
5. 计算 AVC 原始无功目标。
6. 应用站级 P/Q/S、SOC、充放电和爬坡限制。
7. 按设备可用能力分配到各 PCS。
8. 应用每台 PCS 的 `P² + Q² <= S²` 限制。
9. 写影子点；仅在 `submitWrites=true` 且持有控制权时提交真实写入。
10. 读取 `WritebackResultRecord` 和设备反馈，更新成功、超时和故障状态。

建议默认周期：

- AGC 主周期：`200ms`。
- AVC 主周期：`500ms`，在同一服务循环中按分频执行。
- 调度命令 TTL：`5000ms`。
- 输入最大年龄：`1500ms`，项目可收紧但不能大于命令 TTL。

## 8. AGC 算法

### 8.1 基础闭环

```text
errorP = targetP - measuredPccP
correctionP = KpP * errorP + KiP * integral(errorP)
rawP = targetP + correctionP
```

约束顺序：

1. 小于 `deadbandKw` 时误差按零处理。
2. 积分项限制在 `integralMinKw..integralMaxKw`，防止积分饱和。
3. 根据 BMS 可充、可放功率和 SOC 限制站级目标。
4. 根据 `riseKwPerSec/fallKwPerSec` 做爬坡限制。
5. 根据可用 PCS 的总能力再次限幅。

第一阶段可只启用 P + 前馈，`KiP=0`。现场确认采样延迟和设备响应后再开启积分，避免振荡。

### 8.2 频率下垂

```text
deltaP = Kf * (nominalHz - measuredHz)
targetPWithDroop = dispatchP + clamp(deltaP, -droopLimitKw, droopLimitKw)
```

频率点无效时只关闭下垂补偿，不应使普通调度目标失效。

## 9. AVC 算法

### 9.1 无功目标模式

```text
errorQ = targetQ - measuredPccQ
rawQ = targetQ + KpQ * errorQ + KiQ * integral(errorQ)
```

### 9.2 电压模式

```text
errorV = targetVoltage - measuredVoltage
rawQ = previousQ + KpV * errorV + KiV * integral(errorV)
```

电压控制必须配置死区和无功爬坡，防止采样噪声导致反复写入。

### 9.3 功率因数模式

```text
absQ = abs(P) * tan(acos(abs(targetPf)))
rawQ = reactiveDirection * absQ
```

`targetPf` 必须限制在合法范围，例如 `0.8..1.0`。无功方向由配置明确给出，不能从正负功率因数文本猜测。

## 10. PCS / 逆变器能力模型与分配

### 10.1 铭牌静态能力

每台 PCS 或逆变器必须单独配置铭牌能力，不能只使用站级总功率，也不能只配置一个 `ratedKva`：

- `deviceType`：`storagePcs`、`storageInverter` 或 `gridTieInverter`，决定是否要求 BMS/SOC 能力约束。
- `ratedActivePowerKw`：额定交流有功功率。
- `ratedApparentPowerKva`：额定视在功率，用于 P/Q 能力圆。
- `ratedReactivePowerKvar`：厂家明确给出的最大无功能力；未给出时仍受视在功率限制。
- `maxChargePowerKw`、`maxDischargePowerKw`：允许的最大充电、放电功率，可以小于额定有功。
- `minStableChargePowerKw`、`minStableDischargePowerKw`：最小稳定运行功率，避免设备长期工作在厂家不允许的低功率区间。
- `ratedAcVoltageV`、`ratedAcCurrentA`、`phaseCount`：用于电压或电流降额以及分相限值校验。
- `riseKwPerSec`、`fallKwPerSec`、`riseKvarPerSec`、`fallKvarPerSec`：单机爬坡能力，站级爬坡限制不能替代单机限制。
- `setpointType`：设备接收 `absoluteKw`、`ratedPercent` 或 `perPhaseKw`。内部统一使用 kW/kvar，写回适配层最后转换为厂家寄存器要求。
- `capacityScope`：明确铭牌值是单机、单模块还是整机总值，防止多模块设备重复累计额定功率。

充电和放电必须使用两个正数能力值表达，AGC 内核再依据“正值放电、负值充电”的统一符号生成上下限，避免负数配置造成二次取反。

上述能力字段允许使用两种配置形式：

```json
{
  "ratedActivePowerKw": 100,
  "ratedApparentPowerKva": {
    "commissionedLimit": 110,
    "sourcePoint": {
      "source": "sharedLatest",
      "semanticRole": "pcs.ratedApparentPower",
      "pointCode": "RATED_APPARENT_POWER",
      "index": 1360
    },
    "combinePolicy": "min",
    "stalePolicy": "useCommissionedLimit"
  }
}
```

纯数字表示只使用工程配置值；对象形式表示结合现有采集点。用于真实控制时，`commissionedLimit` 是调试确认后的安全上限，采集值只能收紧能力，不能把控制范围放大到该上限以上。铭牌参数点失效时可以使用工程上限；实时可充放功率、温度降额等动态能力点失效时仍按 10.2 节退出设备分配，二者不能混用同一种 stale 策略。

### 10.2 动态可用能力

控制周期内根据实时状态计算每台设备的动态能力，最终可用功率取所有约束中的最小值：

```text
availableDischargeKw = min(
  ratedActivePowerKw,
  maxDischargePowerKw,
  bmsAvailableDischargeKw,
  pcsDynamicDischargeLimitKw,
  thermalDeratingKw,
  acCurrentLimitKw,
  socDischargeLimitKw)

availableChargeKw = min(
  ratedActivePowerKw,
  maxChargePowerKw,
  bmsAvailableChargeKw,
  pcsDynamicChargeLimitKw,
  thermalDeratingKw,
  acCurrentLimitKw,
  socChargeLimitKw)
```

动态能力输入包括：

- BMS 可充、可放功率及充放电允许状态。
- PCS 厂家提供的动态有功、无功能力值。
- SOC 上下限及接近边界时的可配置渐进降额区间。
- PCS、变压器或电池温度引起的降额。
- 交流侧电压、电流和厂家能力曲线限制。
- 设备通讯、就绪、故障、并离网及运行模式。

储能 PCS/逆变器必须接入 BMS 和 SOC 约束。普通并网逆变器不强制要求 BMS 点，其动态能力可以来自直流源可用功率或厂家实时限功率点；未配置的约束不能伪造为有效 BMS 数据。

任一动态能力点无效时，必须按配置选择 `fallbackToStaticLimit` 或“该设备退出分配”。生产默认建议退出分配；不能在实时能力未知时自动按满额定功率控制。

### 10.3 P/Q 能力包络

AGC 和 AVC 的结果必须经过统一能力限制：

```text
P_cmd² + Q_cmd² <= S_available²
abs(P_cmd) <= availableActivePowerKw
abs(Q_cmd) <= min(ratedReactivePowerKvar, dynamicReactivePowerKvar)
```

`S_available` 默认不超过 `ratedApparentPowerKva`，并可根据温度、电压和厂家 P/Q 能力曲线进一步降额。若厂家不是标准圆形能力曲线，应支持配置分段能力曲线，不能强行按额定 KVA 圆计算。

默认采用 `activePowerFirst`：优先保证 AGC 有功目标，超出视在功率能力时先裁剪 Q。项目也可以配置 `reactivePowerFirst` 或 `proportional`，但必须显式选择。

### 10.4 多 PCS 分配

多 PCS 分配步骤：

1. 剔除通讯异常、未就绪、故障、禁充或禁放设备。
2. 根据每台设备当前方向的动态可用功率和配置权重计算初次份额，不能只按铭牌额定功率分配。
3. 根据 SOC 做可选修正，放电优先高 SOC，充电优先低 SOC。
4. 应用单机有功、无功、视在功率、最小稳定功率和爬坡限制。
5. 对达到限值的设备裁剪，并把剩余目标重新分配给其他设备。
6. 总目标无法满足时进入 `DEGRADED`，发布未满足功率，不得扩大单机限值或静默丢弃偏差。
7. 单台 PCS 如果使用三相独立控制点，分别校验每相功率和电流；没有分相调节需求时才允许平均分相。

站级额定功率应由所有启用设备的有效铭牌值汇总并与项目配置上限交叉校验。配置值不一致时 dry-run 报错，不允许控制器自行选择较大的值。

## 11. 建议配置结构

```json
{
  "schemaVersion": "1.0.0",
  "runtimeMode": "agc_avc",
  "identityConfigFile": "/opt/modbus-gateway/config/runtime/device_identity.json",
  "deviceConfigFiles": [],
  "agcAvc": {
    "enabled": true,
    "shadowMode": true,
    "submitWrites": false,
    "cycleMs": 200,
    "avcCycleMs": 500,
    "commandTimeoutMs": 5000,
    "inputMaxAgeMs": 1500,
    "resumeDelayMs": 2000,
    "signConvention": "positiveDischarge",
    "reactiveSignConvention": "positiveInductive",
    "pqPriority": "activePowerFirst",
    "stationLimits": {
      "ratedActivePowerKw": 100,
      "ratedApparentPowerKva": 110,
      "maxImportPowerKw": 100,
      "maxExportPowerKw": 100,
      "maxReactivePowerKvar": 60,
      "transformerRatedKva": 125
    },
    "ownership": {
      "scope": "pcs-power",
      "leaseFile": "/opt/modbus-gateway/run/power-control-owner.json",
      "ttlMs": 3000,
      "heartbeatMs": 500
    },
    "interlocks": {
      "remoteEnable": {
        "source": "sharedLatest",
        "semanticRole": "site.remoteEnable",
        "meterCode": "DIDO_1",
        "pointCode": "REMOTE_ENABLE",
        "index": 2101,
        "required": true
      },
      "emergencyStop": {
        "source": "sharedLatest",
        "semanticRole": "site.emergencyStop",
        "meterCode": "DIDO_1",
        "pointCode": "EMERGENCY_STOP",
        "index": 2102,
        "required": true
      },
      "fireAlarm": {
        "source": "sharedLatest",
        "semanticRole": "site.fireAlarm",
        "meterCode": "FIRE_1",
        "pointCode": "FIRE_ALARM",
        "index": 3101,
        "required": true
      }
    },
    "agc": {
      "enabled": true,
      "mode": "pccClosedLoop",
      "target": {
        "source": "sharedCommand",
        "semanticRole": "agc.dispatchActivePower",
        "index": 720010
      },
      "commandSequence": {
        "source": "sharedCommand",
        "semanticRole": "agc.commandSequence",
        "index": 720011
      },
      "commandTimestamp": {
        "source": "sharedCommand",
        "semanticRole": "agc.commandTimestamp",
        "index": 720012
      },
      "pccActivePower": {
        "source": "sharedLatest",
        "semanticRole": "site.pcc.activePower",
        "meterCode": "PCC_METER_1",
        "pointCode": "TOTAL_ACTIVE_POWER",
        "index": 1039,
        "required": true
      },
      "frequency": {
        "source": "sharedLatest",
        "semanticRole": "site.pcc.frequency",
        "meterCode": "PCC_METER_1",
        "pointCode": "GRID_FREQUENCY",
        "index": 1055,
        "required": false
      },
      "deadbandKw": 1.0,
      "kp": 0.2,
      "ki": 0.0,
      "minKw": -100,
      "maxKw": 100,
      "riseKwPerSec": 20,
      "fallKwPerSec": 20,
      "frequencyDroop": {
        "enabled": false,
        "nominalHz": 50.0,
        "kwPerHz": 100,
        "limitKw": 20
      }
    },
    "avc": {
      "enabled": true,
      "mode": {"source": "sharedCommand", "semanticRole": "avc.mode", "index": 720020},
      "targetQ": {"source": "sharedCommand", "semanticRole": "avc.dispatchReactivePower", "index": 720021},
      "targetVoltage": {"source": "sharedCommand", "semanticRole": "avc.dispatchVoltage", "index": 720022},
      "targetPowerFactor": {"source": "sharedCommand", "semanticRole": "avc.dispatchPowerFactor", "index": 720023},
      "pccReactivePower": {
        "source": "sharedLatest",
        "semanticRole": "site.pcc.reactivePower",
        "meterCode": "PCC_METER_1",
        "pointCode": "TOTAL_REACTIVE_POWER",
        "index": 1043,
        "required": true
      },
      "pccVoltage": {
        "source": "sharedLatest",
        "semanticRole": "site.pcc.voltage",
        "meterCode": "PCC_METER_1",
        "pointCode": "GRID_VOLTAGE",
        "index": 1030,
        "required": true
      },
      "pccPowerFactor": {
        "source": "sharedLatest",
        "semanticRole": "site.pcc.powerFactor",
        "meterCode": "PCC_METER_1",
        "pointCode": "TOTAL_POWER_FACTOR",
        "index": 1048,
        "required": true
      },
      "deadbandKvar": 1.0,
      "voltageDeadbandV": 1.0,
      "kpQ": 0.2,
      "kiQ": 0.0,
      "kpV": 1.0,
      "kiV": 0.0,
      "minKvar": -100,
      "maxKvar": 100,
      "riseKvarPerSec": 20,
      "fallKvarPerSec": 20
    },
    "pcs": [
      {
        "machineCode": "COMM202600102",
        "meterCode": "PCS_1",
        "enabled": true,
        "deviceType": "storagePcs",
        "weight": 1.0,
        "capacityScope": "deviceTotal",
        "ratedActivePowerKw": 100,
        "ratedApparentPowerKva": 110,
        "ratedReactivePowerKvar": 60,
        "maxChargePowerKw": 100,
        "maxDischargePowerKw": 100,
        "minStableChargePowerKw": 2,
        "minStableDischargePowerKw": 2,
        "ratedAcVoltageV": 400,
        "ratedAcCurrentA": 159,
        "phaseCount": 3,
        "setpointType": "absoluteKw",
        "riseKwPerSec": 20,
        "fallKwPerSec": 20,
        "riseKvarPerSec": 20,
        "fallKvarPerSec": 20,
        "fallbackToStaticLimit": false,
        "points": {
          "online": {"source": "sharedLatest", "semanticRole": "pcs.online", "pointCode": "ONLINE", "index": 1399},
          "ready": {"source": "sharedLatest", "semanticRole": "pcs.ready", "pointCode": "READY", "index": 1300},
          "soc": {"source": "sharedLatest", "semanticRole": "bms.soc", "meterCode": "BMS_1", "pointCode": "SOC", "index": 1569},
          "actualP": {"source": "sharedLatest", "semanticRole": "pcs.actualActivePower", "pointCode": "ACTIVE_POWER", "index": 1330},
          "actualQ": {"source": "sharedLatest", "semanticRole": "pcs.actualReactivePower", "pointCode": "REACTIVE_POWER", "index": 1331},
          "chargeAllowed": {"source": "sharedLatest", "semanticRole": "bms.chargeAllowed", "meterCode": "BMS_1", "pointCode": "CHARGE_ALLOWED", "index": 1570},
          "dischargeAllowed": {"source": "sharedLatest", "semanticRole": "bms.dischargeAllowed", "meterCode": "BMS_1", "pointCode": "DISCHARGE_ALLOWED", "index": 1571},
          "availableChargeKw": {"source": "sharedLatest", "semanticRole": "bms.availableChargePower", "meterCode": "BMS_1", "pointCode": "AVAILABLE_CHARGE_POWER", "index": 1590},
          "availableDischargeKw": {"source": "sharedLatest", "semanticRole": "bms.availableDischargePower", "meterCode": "BMS_1", "pointCode": "AVAILABLE_DISCHARGE_POWER", "index": 1591},
          "dynamicReactiveLimitKvar": {"source": "sharedLatest", "semanticRole": "pcs.availableReactivePower", "pointCode": "AVAILABLE_REACTIVE_POWER", "index": 1592},
          "acVoltage": [
            {"source": "sharedLatest", "semanticRole": "pcs.acVoltage.phaseA", "pointCode": "UA", "index": 1340},
            {"source": "sharedLatest", "semanticRole": "pcs.acVoltage.phaseB", "pointCode": "UB", "index": 1341},
            {"source": "sharedLatest", "semanticRole": "pcs.acVoltage.phaseC", "pointCode": "UC", "index": 1342}
          ],
          "activeTargets": [
            {"source": "sharedWriteback", "semanticRole": "pcs.activePowerTarget.phaseA", "pointCode": "PA_TARGET", "index": 1318},
            {"source": "sharedWriteback", "semanticRole": "pcs.activePowerTarget.phaseB", "pointCode": "PB_TARGET", "index": 1319},
            {"source": "sharedWriteback", "semanticRole": "pcs.activePowerTarget.phaseC", "pointCode": "PC_TARGET", "index": 1320}
          ],
          "reactiveTargets": [
            {"source": "sharedWriteback", "semanticRole": "pcs.reactivePowerTarget.phaseA", "pointCode": "QA_TARGET", "index": 1321},
            {"source": "sharedWriteback", "semanticRole": "pcs.reactivePowerTarget.phaseB", "pointCode": "QB_TARGET", "index": 1322},
            {"source": "sharedWriteback", "semanticRole": "pcs.reactivePowerTarget.phaseC", "pointCode": "QC_TARGET", "index": 1323}
          ]
        }
      }
    ],
    "outputs": {
      "stateIndex": 720050,
      "agcEffectiveTargetIndex": 720051,
      "agcErrorIndex": 720052,
      "avcEffectiveTargetIndex": 720053,
      "avcErrorIndex": 720054,
      "availableActivePowerIndex": 720055,
      "availableReactivePowerIndex": 720056,
      "unservedActivePowerIndex": 720057,
      "unservedReactivePowerIndex": 720058,
      "lastCommandStatusIndex": 720059
    }
  }
}
```

示例 Index 仅用于说明。实际项目必须由全局点位路由表分配，禁止直接复制示例导致重复 Index。

PCS `points` 内的引用默认继承外层 `machineCode/meterCode`；绑定 BMS 或其他设备时必须在该引用内显式覆盖 `meterCode`。Windows 和平台应让用户按“设备 -> 测点”选择，保存完整 `PointRef`，不能要求操作员手填裸 Index。

## 12. 调度指令入口

控制内核只依赖共享点和命令序号，不绑定具体上行协议。不同入口统一转换成以下字段：

- `enable`
- `mode`
- `targetP/targetQ/targetVoltage/targetPf`
- `sequence`
- `issuedAt`
- `validForMs`
- `source`

建议入口：

1. IEC/Modbus 项目：现有驱动采集或北向服务把调度值写入命令虚拟点。
2. 平台 MQTT：由现有 `MqttDriver` 增加调度适配器，使用 QoS 2，把通过校验的报文写入同一命令邮箱或虚拟点。
3. Windows 直连测试：由 `SystemMonitor` 维护 API 写入同一命令入口，不直接调用 PCS。

AGC/AVC 模块本身不再建立第二条 MQTT 连接，避免重复 TLS、离线重传和身份配置。

命令必须校验 machineCode、递增 sequence、时间戳、TTL 和模式。重复或倒序 sequence 只记录，不重复执行。

## 13. 写回和反馈

每轮写回使用批次 ID：

```text
AGCAVC_<session>_<sequence>_<cycle>_<targetIndex>
```

规则：

- 目标变化小于死区时不写。
- 队列已有相同目标时不重复提交。
- 每个目标遵守最小写间隔。
- 只允许写 `write.enable=true` 的点。
- 写回结果必须读取 `WritebackResultRecord`，记录排队、设备写入、校验和总耗时。
- 连续失败达到阈值后退出 `ACTIVE`；不能只在日志中报错后继续无限写。
- 反馈校验读取 PCS 实际 P/Q，不把“寄存器写成功”当作“功率已跟踪成功”。

## 14. 虚拟点和可观测性

建议为模块生成独立虚拟设备文件，例如：

```text
config/runtime/devices/device_agc_avc_virtual.json
sharedMemoryName = gateway_point_store_agc_avc
```

至少暴露：

- 模块状态和控制权状态。
- 调度命令邮箱、有效目标和跟踪误差。
- AGC/AVC 限幅前后结果。
- 可用 P/Q、未满足 P/Q。
- 每台 PCS 分配目标。
- 当前命令 sequence、命令年龄和来源。
- 最后写回状态、失败计数、反馈校验状态和耗时。
- 当前降级或禁止原因码。

PCS 实际 P/Q、SOC、温度、状态和并网点反馈继续使用原采集点，不在该虚拟设备中复制一份。Windows、本地 Qt 和 MQTT 展示时根据语义绑定把原采集点与 AGC/AVC 计算点组合成一个视图。

这些点由现有 MQTT 全量/实时、Windows 监测和本地 Qt 画面读取，不需要 AGC/AVC 自己发布第二套遥测。

## 15. 安全要求

真实写回必须同时满足：

1. `agcAvc.enabled=true`。
2. `shadowMode=false`。
3. `submitWrites=true`。
4. 远方控制允许、急停未触发、消防未触发。
5. 调度命令 sequence 和 TTL 有效。
6. 关键输入质量正常且未 stale。
7. 已取得 `pcs-power` 控制权。
8. 没有人工高优先级租约。
9. 目标点可写且数值通过 P/Q/S、SOC、BMS 和爬坡限制。

配置 dry-run 必须检查：

- 所有输入和输出 Index 存在且无重复。
- 所有 `PointRef` 的 Index、machineCode、meterCode 和 pointCode 与全局路由一致，语义角色在设备作用域内唯一。
- 采集输入的单位和数据类型符合语义要求；例如有功必须可归一化为 kW，SOC 必须可归一化为百分比。
- 写目标属于 PCS 且 `write.enable=true`。
- AGC/AVC 与启用写回的 Graph EMS 不存在目标交集。
- 每台 PCS 的 P/Q 数量匹配，额定有功、视在功率、无功、充放电能力和单机爬坡率合法。
- `ratedActivePowerKw <= ratedApparentPowerKva`，充放电上限不超过厂家允许值，最小稳定功率不大于对应最大功率。
- `setpointType`、`capacityScope`、相数和目标点数量一致，百分比写入能够正确换算到单机铭牌功率。
- 所有启用 PCS 汇总能力与站级配置上限一致；禁止把单模块铭牌功率误当整机功率重复累计。
- 站级目标同时受并网进出功率、变压器容量和站级 P/Q/S 限制，设备能力总和更大时仍以站级较小边界为准。
- `storagePcs/storageInverter` 必须配置 BMS/SOC 约束；`gridTieInverter` 必须明确其直流源或厂家动态能力来源。
- 动态能力、SOC、温度、电压和电流点缺失时的降级策略明确，生产配置不得隐式按满额定能力回退。
- 周期、TTL、输入年龄和租约时间关系合法。
- 电压、功率因数和无功方向配置完整。

## 16. 服务管理和发布

需要更新：

- `CMakeLists.txt`：增加 `AgcAvcController` 目标。
- `deploy/agc-avc@.service`：独立 systemd 实例。
- `deploy/gateway-services.sh`：读取 `agc-avc-service.json`，仅在 `runtimeMode=agc_avc` 且 `agcAvc.enabled=true` 时启动。
- 通用工厂包：不携带 AGC/AVC 二进制、service 或配置，`gateway/ems` 初始化不会上传这些文件。
- AGC/AVC 运行模式包：`gateway-agc-avc-runtime.tar.gz` 只携带控制器、service、应用配置和虚拟点配置；仅选择 `agc_avc` 模式时叠加安装。
- OTA：允许单独替换 AGC/AVC 二进制和配置，也可随整包升级。

停止顺序应先停止 AGC/AVC，再停止采集驱动；启动顺序应先启动采集驱动，确认共享内存可读后再启动 AGC/AVC。

## 17. 测试和验收

### 17.1 单元测试

- AGC 正负功率方向、死区、PI、积分限幅和爬坡。
- 语义绑定解析、设备作用域、Index 迁移保护、重复角色和缺失必选点。
- 原采集点质量、时间戳和 stale 状态能够原样进入控制输入，且不会生成重复采集数据。
- AVC 三种模式、功率因数边界和无功方向。
- P/Q 视在功率圆三种优先策略。
- 不同 PCS 额定有功、额定视在功率、充放电不对称能力和最小稳定功率。
- kW、额定功率百分比和三相独立写入的目标换算。
- BMS 动态能力、温度、电压、电流和 SOC 降额，以及动态能力点失效后的安全退出。
- 标准能力圆和厂家分段 P/Q 能力曲线边界。
- 多 PCS 分配、设备退出和剩余量二次分配。
- 命令重复、倒序、超时和时间回拨。
- 输入坏质量、stale、消防、禁充禁放和急停。
- 所有权抢占、超时释放和高优先级人工控制。
- 写入去重、失败退避、反馈超时和恢复无扰切换。

### 17.2 22.16 分阶段测试

1. 只部署二进制和关闭状态模板，不改变当前服务。
2. 开启 `shadowMode=true`，连续运行至少 2 小时，确认没有 pending write。
3. 用固定输入回放阶跃、斜坡、正负切换、SOC 边界和 PCS 掉线。
4. 对比计算目标、限幅结果和现有 EMS 结果。
5. 关闭现有 EMS 的六路 PCS 写回，再开启 AGC/AVC 小功率真实测试。
6. 首次真实测试限制在 `±5kW` 和 `±5kvar`，确认方向后再逐级提高。
7. 测试人工高优先级抢占，确认 AGC/AVC 立即停止自动提交但采集不中断。
8. 测试命令超时、服务重启和网络断开，确认不会恢复执行过期命令。
9. 测试完成后删除临时配置和备份，只保留已确认版本。

### 17.3 验收指标

- 控制循环无阻塞，200ms 周期下无持续积压。
- 相同目标不重复写，目标变化才进入写回队列。
- 所有真实写入都有 cmdId、来源、时间和结果。
- AGC/AVC 与 EMS、MQTT 人工控制不存在双写。
- 命令超时、高优先级抢占和关键输入失效时行为可重复。
- 模块重启后不执行旧命令，恢复过程无目标突跳。

## 18. 实施阶段

### 阶段 1：骨架与影子计算

- 新增配置模型、服务、状态机、AGC/AVC 计算和虚拟点。
- `submitWrites` 强制为 false。
- 完成单元测试和 22.16 影子运行。

### 阶段 2：P/Q 协同与多 PCS 分配

- 增加 P/Q/S 限制、SOC/BMS 能力和多机分配。
- 增加反馈状态、失败退避和运行指标。

### 阶段 3：控制权仲裁与真实写回

- 增加 `PowerControlOwnership`。
- Graph EMS、MQTT 人工控制和 AGC/AVC 接入同一写入仲裁。
- 在 22.16 完成小功率实机测试。

### 阶段 4：Windows 和平台

- Windows 增加 AGC/AVC 配置、影子监测、指令注入和实机测试页。
- 平台维护项目模板、调度入口、历史趋势和告警，不直接绕过边端安全门写 PCS。

## 19. 最终建议

首版不要直接做复杂自适应算法。先完成以下闭环：

```text
有效调度命令
-> 输入质量与安全联锁
-> P/Q 基础闭环
-> P/Q/S 与 SOC 限制
-> 多 PCS 分配
-> 共享写回队列
-> 设备反馈验证
-> 可观测状态点
```

在现场获得采样延迟、PCS 响应时间和并网点波动数据后，再调整 PI、频率下垂和电压控制参数。这样能保持模块独立、可配置、可回退，也符合现有边端的共享内存和驱动写回架构。

## 20. 当前实现说明（2026-07-16）

当前代码已完成以下闭环：

- AGC 主周期和 AVC 分频周期；AVC 非计算周期保持上一轮目标，不重复积分。
- 站级和单机两级爬坡、最小稳定功率、多 PCS 二次分配及 P/Q/S 能力圆。
- BMS 动态可充放能力、SOC 渐进降额、温度降额和交流电流降额。
- 命令参数锁存和 sequence 二次读取；参数变化但 sequence 未变化时继续使用上一份完整命令。
- 命令超时按单机爬坡平滑归零，急停、消防和关键输入失败时生成零目标。
- 真实控制统一进入共享写回队列，读取 `WritebackResultRecord`，并对 PCS 实际 P/Q 做反馈超时检查。
- PCS 功率控制权采用跨进程文件锁保护的租约文件，避免两个进程同时抢占成功。
- MQTT 与 SystemMonitor 识别 `agc_avc_virtual` 的 command 点为共享命令邮箱，不把它误当普通设备寄存器写回。
- 直连维护只允许带 `agc-avc-shadow-test` 来源的影子注入；生产调度只允许 MQTT 使用明确的 `agc-avc-dispatch` 来源。

命令邮箱提交顺序固定为：有功目标、AVC 模式、Q/电压/PF 目标、`issuedAt`、`sequence`。其中 `sequence` 必须最后提交。

工厂模板继续保持 `enabled=false`、`shadowMode=true`、`submitWrites=false`。项目配置必须完成现场影子验证和安全联锁复核后，才能单独开启真实写回。

## 21. 192.168.22.16 影子联调记录（2026-07-16）

测试设备 `COMM202600999` 已部署 ARM64 `AgcAvcController`、`MqttDriver`、`SystemMonitor` 和 `ComputeEngine`。AGC/AVC 项目配置保持：

```text
enabled=true
shadowMode=true
submitWrites=false
cycleMs=200
avcCycleMs=500
```

现场绑定使用台区电表 `T2216_modbusRTU_1_slave_2` 的 `1039/1043/1030/1051/1052`，PCS `T2216_modbusRTU_4_slave_1` 的 `1214/1215/1216/1230/1234/1293/1294/1399`，以及 BMS `T2216_modbusRTU_3_slave_2` 的 SOC `1569`。消防联锁没有找到经过确认的现场语义点，因此保持未绑定；该配置只允许影子验证，不能作为真实写回配置。

已验证：

- 边端 `--validate` 通过，Index、设备身份、点码和写目标可写性一致。
- ARM64 `agc_avc_controller_test` 在设备本机通过。
- 7 个命令邮箱字段全部提交成功，顺序为 P、AVC 模式、Q、电压、PF、时间戳、sequence。
- 非影子来源和高优先级邮箱写入均被拒绝。
- 现场采集通讯坏质量时进入 `FAILSAFE`，虚拟状态点保持刷新。
- `gateway_point_store_ttySP4` 的 pending write 始终为 0，没有 `agc-avc` 设备写回。
- 服务无重启，稳定观察期间内存约 6.1MB，相关服务没有新增 warning。
- 直连工程快照包含应用配置和虚拟点配置；双文件配置 apply dry-run 通过且未改盘。

本次没有执行真实 PCS 控制。由于联调时台区电表、PCS 和 BMS 均为坏质量，现场只验证了安全降级、命令邮箱、发布链路和零写回；恢复设备通讯后仍需完成至少 2 小时有效数据影子对比，再评审是否进入 `±5kW/±5kvar` 小功率测试。
