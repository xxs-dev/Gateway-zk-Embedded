# 图形化 EMS 逻辑配置设计

## 1. 背景

当前舜通 EMS 逻辑已经迁移为边端 `LegacyEmsEngine`，由 `ComputeEngine` 通过 `script.type=legacyEms` 周期执行。该方式适合快速复刻旧 `script.cpp` 行为，但逻辑被固化在 C++ 中，平台端只能配置启停、点表文件和少量 profile 参数，不能图形化调整策略块、点位映射、调度表和输出写回规则。

当前已经形成两层策略图：旧领域节点图用于兼容既有工程和等价基准；模块化图使用公式、条件、调度、仲裁、约束和写回等通用节点表达算法。`LegacyEmsEngine` 和旧领域图保留为金标准，新的模块化图仍通过 `script.type=graphEms` 执行。

## 2. 目标

- 平台端用图形化界面配置 EMS 策略，不要求现场人员修改 C++。
- 边端执行安全、确定性的 JSON 策略图，不执行任意 JavaScript、Python 或 Shell。
- 支持舜通模板一键加载，也支持在模板基础上调整点位、参数、启停和输出写回。
- 保留现有共享内存、`PointStoreRouter`、计算点、写回队列和 MQTT 上报链路。
- 能用自动化测试对比 `legacyEms` 与 `graphEms` 的关键输出，降低迁移风险。

## 3. 非目标

- 不做无限自由的低代码编程平台。
- 不允许平台下发任意脚本到边端执行。
- 不在第一阶段支持用户自定义 C++ 插件。
- 不改变 Modbus、DLT645、CAN、DIDO 等协议驱动的采集和写回模型。

## 4. 总体架构

```text
Java / Vue 平台
  -> 选择 EMS 模板
  -> 图形化配置策略块、点位映射、调度表和写回规则
  -> 生成 graphEms JSON
  -> 配置包 OTA 下发

边端 ComputeEngine
  -> 加载 app 配置
  -> script.type=graphEms
  -> 读取 graphFile
  -> 校验节点、边、点位和写回权限
  -> 周期执行策略图
  -> PointStoreRouter 写 latest / PendingWriteCommand
```

`legacyEms` 与 `graphEms` 可以在同一个 `ComputeEngine` 中并存。现场切换时先启用影子运行，只比较输出，不下发 PCS 写命令；确认一致后再开启 `submitWrites=true`。

## 5. 配置入口

App 配置增加一种脚本类型：

```json
{
  "ruleCode": "graph_ems_shuntong",
  "name": "舜通 EMS 图形化策略",
  "enabled": true,
  "trigger": {
    "type": "interval",
    "intervalMs": 2000
  },
  "inputs": [],
  "outputs": [],
  "script": {
    "type": "graphEms",
    "graphFile": "runtime/logic/shuntong_ems_graph.json",
    "graphProfile": {
      "PCS_MODEL": "3",
      "BMS_MODEL": "2",
      "ZL_MODEL": "21",
      "Meter_TQ": "1",
      "Meter_CN": "1",
      "Meter_BW": "0",
      "Meter_FH": "0",
      "UPS_MODEL": "0",
      "DEHUMIDIFIER_MODEL": "0",
      "XF_CZ": "1"
    }
  }
}
```

`graphFile` 使用相对路径时，按 app 配置文件所在目录解析。`graphProfile` 用于保留工程级开关和设备型号选择，避免把每个模板都复制成不同文件。

设备存在性规则：

- `profileKey` 用于默认应存在的设备或逻辑块；profile 未配置时按启用处理，配置为 `0/false` 时跳过。
- `optionalProfileKey` 用于 UPS、除湿、负荷表、结算/并网表等可选设备；profile 未配置或为 `0/false` 时跳过，只有明确配置为 `1/true` 或非零型号值才执行。
- 被跳过的节点不读取点位、不写运行态、不参与故障聚合，也不应触发“缺少点位”校验错误。
- 平台提交校验时只校验已启用设备/模式的必填点位；未启用的可选设备不要求绑定测点。

舜通默认 profile 中 `UPS_MODEL=0`、`DEHUMIDIFIER_MODEL=0`、`Meter_BW=0`、`Meter_FH=0`，表示这些设备/分支默认不存在。

平台 EMS 原生配置增加 `devicePresenceProfile`，作为页面和后端校验的统一设备存在性来源。保存 EMS 原生配置时，平台会把该 profile 同步到运行 app 的 `script.graphProfile`；边端只读取 `script.graphProfile`，图文件里的 `devicePresenceProfile` 仅作为模板说明和平台展示元数据。

当前边端已支持 `script.type=graphEms`。`ComputeEngineService` 会按 `ruleCode` 缓存 `GraphEmsEngine`，每轮扫描加载同一份 graph 配置执行。运行态状态文件由 `script.graphStateFile` 指定；未配置时使用 `/opt/modbus-gateway/data/graph_ems_state_<ruleCode>.json`。示例文件：

| 文件 | 作用 |
| --- | --- |
| `config/examples/shuntong_ems_graph.json` | 舜通 EMS 默认策略图模板 |
| `config/examples/shuntong_ems_modular_graph.json` | 舜通 EMS 完全模块化候选图，不包含旧领域节点 |
| `config/examples/device_ems_modular_virtual.json` | 模块化图实际输出和 `700000+` 中间点路由 |
| `config/examples/mqtt-service-graph-ems-example.json` | `graphEms` app 配置示例 |

舜通模板的旧地址段迁移规则固定为：

| 地址段 | 新语义 | 说明 |
| --- | --- | --- |
| `4500-4599` | LED 屏 | 数字显示和 RGB 状态控制，不能再放结算/并网表点 |
| `4600-4699` | 结算/并网表 | 旧 `4501-4599` 结算点平移到 `4601-4699`；旧 `4536-4543/4599` 对应新 `4636-4643/4699` |

平台导入旧工程或生成舜通默认模板时，应按上表生成点表并在提交时校验地址段冲突。

## 6. 策略图 JSON

策略图是版本化 JSON。平台端只生成受支持的节点类型，边端严格校验。

当前边端只接受 `schemaVersion=1.x`。根级 `limits.maxNodes/maxEdges` 默认 256/512，允许的硬上限为 1024/4096；超过配置或硬上限会拒绝加载。节点参数、重复 ID、未知节点、边引用和有向环同样在加载期拒绝。

```json
{
  "schemaVersion": "1.0.0",
  "graphCode": "shuntong_ems",
  "name": "舜通 EMS 默认策略",
  "scanIntervalMs": 2000,
  "mode": "active",
  "limits": {
    "maxNodes": 200,
    "maxEdges": 500,
    "maxWritesPerScan": 100
  },
  "nodes": [
    {
      "id": "meter_average",
      "type": "meterAverage",
      "enabled": true,
      "params": {
        "windowSizeIndex": 156,
        "mappings": [
          { "input": 1036, "output": 209 },
          { "input": 1037, "output": 210 },
          { "input": 1038, "output": 211 }
        ]
      }
    },
    {
      "id": "ds",
      "type": "timedChargeDischarge",
      "enabled": true,
      "params": {
        "useLocalHour": true,
        "powerScheduleStartIndex": 400,
        "socScheduleStartIndex": 424,
        "modeScheduleStartIndex": 760,
        "bmsSocIndex": 1570,
        "cnVoltageIndexes": [251, 252, 253],
        "gradPIndex": 533,
        "vMaxIndex": 463,
        "vMinIndex": 464
      },
      "outputs": {
        "powerNow": 461,
        "socNow": 462,
        "pa": 615,
        "pb": 616,
        "pc": 617,
        "p3": 618,
        "run": 18
      }
    },
    {
      "id": "pcs_writeback",
      "type": "pcsWriteback",
      "enabled": true,
      "params": {
        "communicationStatusIndex": 1399,
        "requiredCommunicationStatus": 1,
        "submitWrites": true,
        "truncateToInteger": true,
        "deadband": 0.5
      },
      "inputs": {
        "pa": 627,
        "pb": 628,
        "pc": 629,
        "qa": 630,
        "qb": 631,
        "qc": 632
      },
      "outputs": {
        "pControlA": 1318,
        "pControlB": 1319,
        "pControlC": 1320,
        "qControlA": 1321,
        "qControlB": 1322,
        "qControlC": 1323
      }
    }
  ],
  "edges": [
    { "from": "meter_average", "to": "ds" },
    { "from": "ds", "to": "power_solve" },
    { "from": "power_solve", "to": "pcs_writeback" }
  ]
}
```

## 7. 执行模型

边端执行分为加载期和运行期。

加载期：

1. 解析 graph JSON。
2. 校验 `schemaVersion`、节点类型、参数字段和节点 ID。
3. 校验 `edges` 无环，生成拓扑顺序。
4. 收集所有输入和输出点位，通过 `PointStoreRouter::routeByIndex()` 校验路由。
5. 对写设备输出校验 `write.enable=true`。
6. 初始化节点状态，例如 DS 的 `OUT_PA_DS / OUT_PB_DS / OUT_PC_DS`。

运行期：

1. 每轮使用 `nowMs` 创建执行上下文。
2. 首轮从 `graphStateFile` 恢复上一轮输出 latest、平均窗口和状态节点内部计时，恢复过程只写 latest，不提交设备写命令。
3. 节点按拓扑顺序执行。
4. 节点通过 `PointStoreRouter` 读取 latest 值。
5. 节点输出写入 latest 或提交 `PendingWriteCommand`。
6. 每轮结束检查运行态状态；默认每 5 秒最多落盘一次，并通过 `.tmp` 临时文件原子替换正式状态文件。路径默认为 `/opt/modbus-gateway/data/graph_ems_state_<ruleCode>.json`，可用 `graphProfile.stateSaveIntervalMs` 调整节流周期。
7. 执行结果记录节点耗时、跳过原因、写入数量和错误信息。

节点失败时不影响整个进程退出。默认策略是：当前节点输出保持上次值或写入坏质量，具体由节点类型定义。

### 7.1 模块参数语义约束

Windows 配置端与边端加载校验必须使用同一组业务语义，不能依赖操作员阅读源码猜测：

- `timeSource` 读取边端系统时间和系统时区；输出小时、分钟、秒、当日分钟数或星期。
- `scheduleSelect` 自行读取当前本地小时，精确匹配 0-23 整点；未匹配时使用 `defaultPower/defaultTargetSoc/defaultMode`。
- `windowAggregate` 的窗口是有效样本数量 1-4096，不是时间秒数；无效输入不进入窗口。
- `rateLimit` 的上升和下降单位是“每秒工程值”，按真实经过时间计算；输入越界时拒绝本轮计算。
- `hysteresis/debounce` 的 `invert` 只取反最终输出；输入缺失或非有限值时输出安全状态 0。
- `controlWrite` 依次检查真实写入开关、许可值、输入有效性、工程值范围、写入死区、待处理重复命令和目标点可写性。
- `feedbackVerify` 固定输出 `1=成功、0=等待、-1=超时、-2=输入异常`，目标变化超过 `targetChangeTolerance` 后重新计时。
- `sequence` 每轮最多迁移一次，`minDurationMs` 是当前状态最短停留毫秒数，`evaluateOnInitialize=false` 时首次扫描只恢复或建立初始状态。

Windows 端保存前还应检查单字段范围和字段关系；边端加载时执行最终权威校验。两端均不得把非法枚举静默解释为默认值。

## 8. 节点类型

| 节点类型 | 作用 | 状态 | 第一阶段 |
| --- | --- | --- | --- |
| `pointInput` | 显式读取一组点位，供调试视图展示 | 无 | 可选 |
| `meterAverage` | TQ / CN 移动平均 | 有 | 已实现 |
| `derivedLoad` | 计算 `FH=TQ-CN`、`FH=TQ-BW` 或直接使用 FH | 无 | 已实现 |
| `bmsDerived` | BMS 当日能量、充放电允许功率 | 无 | 已实现 |
| `cosCompensation` | 无功补偿目标和输出 | 有 | 已实现 |
| `voltageCompensation` | LV / HV 有功补偿 | 有 | 已实现 |
| `chargeDischarge` | 手动充电 / 放电模式 | 无 | 已实现 |
| `chargeDischargeCycleTest` | 充放电测试循环策略 | 有 | 已实现 |
| `timedChargeDischarge` | DS 定时充放电 | 有 | 已实现 |
| `photovoltaicCharge` | GF 光伏充电窗口 | 无 | 已实现 |
| `phaseBalance` | PH 三相平衡 | 无 | 已实现 |
| `skOverride` | SK 总控设定 | 无 | 已实现 |
| `reserveCapacity` | ZR 动态增容 | 无 | 已实现 |
| `pcsPowerSolve` | PCS P/Q 输出仲裁和限幅 | 无 | 已实现 |
| `pcsWriteback` | PCS 指令写回 | 有 | 已实现 |
| `formula` | 白名单数学运算、聚合和限幅 | 无 | 已实现第一版 |
| `switch` | 比较或布尔条件驱动的分支选择 | 无 | 已实现第一版 |
| `controlGate` | 多条件控制联锁和写回许可输出 | 无 | 已实现第一版 |
| `feedbackVerify` | 控制目标与设备反馈闭环校验 | 无 | 已实现第一版 |
| `controlWrite` | 单目标通用设备控制写回 | 有 | 已实现第一版 |
| `rateLimit` | 控制目标上升/下降速率限制 | 无 | 已实现第一版 |
| `hysteresis` | 高低阈值滞回判断 | 无 | 已实现第一版 |
| `debounce` | 布尔状态开启/关闭防抖 | 无 | 已实现第一版 |
| `sequence` | 多阶段顺序状态机 | 无 | 已实现第一版 |

旧领域节点继续用于旧工程兼容。新模块化模板不再使用这些节点：数学计算和分支由 `formula/switch/controlGate` 组成，时段选择由 `timeSource/scheduleSelect` 组成，PCS 候选合并与限制分别由 `phaseArbiter/powerConstraint` 执行，设备下发由固定目标的 `controlWrite` 执行。不可绕过的目标可写性、队列容量、控制租约和协议写回校验仍在边端公共控制链中执行。

### 8.1 通用算法节点第一版

`formula` 只允许白名单运算，不解释任意脚本。当前支持：

- `add` / `sum`
- `subtract`
- `multiply`
- `divide`
- `min` / `max`
- `average`
- `abs` / `negate`
- `clamp`

输入使用 `inputs[]`，每个输入只能选择一种来源：

```json
{
  "id": "net_power",
  "type": "formula",
  "params": {
    "operation": "subtract",
    "inputs": [
      { "index": 1039 },
      { "value": 10 }
    ],
    "outputIndex": 700010
  }
}
```

`clamp` 只接受一个输入，上下限可以是常量或点位：

```json
{
  "operation": "clamp",
  "inputs": [{ "index": 700010 }],
  "lower": -100,
  "upperIndex": 535,
  "outputIndex": 700011
}
```

`switch` 支持直接读取布尔条件点：

```json
{
  "conditionIndex": 23,
  "trueIndex": 588,
  "falseValue": 0,
  "outputIndex": 700012
}
```

也支持比较两个操作数。操作数可分别使用 `leftIndex/leftValue` 和 `rightIndex/rightValue`；比较符支持 `gt/gte/lt/lte/eq/ne` 及其符号写法。`eq/ne` 可配置 `tolerance`：

```json
{
  "leftIndex": 1570,
  "operator": "lte",
  "rightValue": 20,
  "trueValue": 1,
  "falseValue": 0,
  "outputIndex": 700013
}
```

运行保护：

- 输入点缺失、坏质量或过期时，本轮不产生输出。
- 除零、非法运算、上下限反转和非有限结果会记录节点错误。
- `outputIndex` 必须在全局点位路由中存在，否则节点报输出拒绝。
- 通用节点只写 latest，不提交设备控制命令。
- 需要真实控制时，结果必须继续经过 PowerSolve、安全约束和写回节点。

完整样例见 `config/examples/graph-ems-generic-nodes-example.json`。

### 8.2 控制联锁第一版

`controlGate` 把通讯状态、控制模式、故障和现场安全条件汇总为一个 `0/1` 许可点。`combine=all` 表示全部条件通过才许可，`combine=any` 表示至少一项通过；单项条件支持 `gt/gte/lt/lte/eq/ne`，右操作数可以使用常量 `value` 或动态点位 `valueIndex`，并可配置 `tolerance` 和 `invert`。

```json
{
  "id": "pcs_control_gate",
  "type": "controlGate",
  "params": {
    "combine": "all",
    "conditions": [
      { "name": "PCS 通讯正常", "index": 1399, "operator": "eq", "value": 1 },
      { "name": "远程控制允许", "index": 700040, "operator": "eq", "value": 1 }
    ],
    "outputIndex": 700041
  }
}
```

联锁固定采用失效闭锁：任何条件点缺失、质量异常、过期或数值非有限时，`outputIndex` 都会主动刷新为 `0`，不能通过配置改成默认放行。`conditions` 至少 1 条、最多 64 条。

需要让联锁约束 PCS 写回时，在 `pcsWriteback` 或启用内部写回的 `pcsPowerSolve` 中设置：

```json
{
  "permitIndex": 700041,
  "permitValue": 1,
  "permitTolerance": 0.000001
}
```

许可点通过只代表联锁允许继续检查，不会绕过 `submitWrites`、PCS 通讯状态、目标点路由、`write.enable`、队列容量和防重复检查。模板和示例仍默认 `submitWrites=false`。

### 8.3 反馈闭环校验第一版

`feedbackVerify` 比较 EMS 控制目标与设备真实反馈，用于判断控制是否在规定时间内收敛：

```json
{
  "id": "pcs_active_power_feedback",
  "type": "feedbackVerify",
  "params": {
    "targetIndex": 627,
    "feedbackIndex": 1318,
    "tolerance": 1,
    "targetChangeTolerance": 0.01,
    "timeoutMs": 5000,
    "outputIndex": 700042
  }
}
```

输出状态固定为：`1` 已达到目标、`0` 等待反馈、`-1` 超时、`-2` 目标或反馈输入异常。目标变化超过 `targetChangeTolerance` 后重新开始计时；`timeoutMs` 范围为 1-86400000 毫秒。输入点缺失、质量异常、过期或非有限时不会误报成功。

该节点是 EMS 业务层的目标-反馈收敛判断，不代替协议驱动的 `verifyAfterWrite/verifyByRead`。驱动仍负责单条报文写后回读和命令回执，`feedbackVerify` 负责跨扫描周期观察设备业务反馈。边端重启后恢复已经等待的时长；停机期间不累计超时，避免服务恢复瞬间把设备误判为控制超时。

### 8.4 通用控制写回第一版

`controlWrite` 把一个策略结果写入固定设备控制点，适合 PCS 六路功率之外的模式、启停和辅机设定值：

```json
{
  "id": "generic_control_write",
  "type": "controlWrite",
  "params": {
    "submitWrites": false,
    "inputIndex": 700011,
    "targetIndex": 700050,
    "minValue": -100,
    "maxValue": 100,
    "deadband": 0.1,
    "permitIndex": 700041,
    "permitValue": 1,
    "permitTolerance": 0.000001,
    "highPriority": false
  }
}
```

`inputIndex` 是策略计算结果，`targetIndex` 必须是配置中固定的可写物理点；第一版不允许运行时动态选择目标点。`minValue/maxValue` 必填且越界直接拒绝，不会静默限幅。`deadband` 用于当前值和队列防重复，`permitIndex` 可接 `controlGate` 输出。

节点默认 `submitWrites=false`。开启后仍必须通过目标路由存在、`write.enable=true`、共享内存可用和队列容量检查。`highPriority=true` 会把命令标记为高优先级，后续由现有控制租约和驱动写回逻辑处理；普通策略不得默认开启。

### 8.5 变化速率限制第一版

`rateLimit` 放在算法输出与控制联锁/写回之间，按实际两次执行时间限制目标变化：

```json
{
  "id": "power_rate_limit",
  "type": "rateLimit",
  "params": {
    "inputIndex": 700011,
    "outputIndex": 700013,
    "risePerSecond": 10,
    "fallPerSecond": 20,
    "minValue": -100,
    "maxValue": 100,
    "initialValue": 0
  }
}
```

`risePerSecond/fallPerSecond` 都是正数，分别表示每秒最大增加量和最大减少量。`minValue/maxValue` 必填，输入越界直接报错。首次运行优先使用 `outputIndex` 已有有效值；没有历史输出时使用 `initialValue`，默认 0 且必须落在上下限内。

节点输出会纳入 graph 状态文件。服务重启后，状态恢复流程先恢复 `outputIndex`，节点再从该值续接，避免重启后直接跳到当前目标。系统时间倒退时本轮保持原输出并重置计时基准。

### 8.6 状态稳定节点

`hysteresis` 配置 `inputIndex/outputIndex/lowThreshold/highThreshold`。输入达到高阈值后输出 1，达到低阈值后输出 0，中间区间保持原状态。`debounce` 配置 `inputIndex/outputIndex/onDelayMs/offDelayMs`，输入持续稳定到规定时长后才切换。两者支持 `initialState` 和 `invert`，输入缺失、坏质量、过期或非法时固定内部状态为不放行。

### 8.7 顺序状态机第一版

`sequence` 使用 `states[]` 定义最多 32 个阶段，使用 `transitions[]` 定义最多 128 条转换。每条转换包含 `from/to/minDurationMs/combine/conditions[]`，条件格式与 `controlGate` 一致。转换按数组顺序匹配，每轮最多执行一次；任何条件输入异常时该转换不成立。

状态写入 `stateOutputIndex`，后续可用 `switch` 把阶段映射为功率、模式或启停目标。首次运行先稳定一轮；重启后恢复当前阶段和已停留时长，停机期间不累计停留时间。完整样例见 `config/examples/graph-ems-sequence-example.json`。

`derivedLoad.params.source` 默认值为 `tqCn`，表示 `FH=TQ-CN`；配置为 `tqBw` 或 `bw` 时表示 `FH=TQ-BW`；配置为 `fh`、`direct` 或 `directFh` 时直接读取 `fhPaIndex..fhQ3Index`，并继续计算 `317..325` 的视在功率、功率因数和三相不平衡率。

当前 `pcsPowerSolve` 默认只写 `627..632` 等 latest 输出，不直接写设备。要实际下发 PCS，需要单独启用 `pcsWriteback.submitWrites=true`，或配置 `pcsPowerSolve.params.submitWrites=true` 由该节点内部复用同一套写回逻辑，按 `comStatusIndex` 和 `pControlAIndex..qControlCIndex` 生成待写命令。模板默认关闭写回，适合影子运行。

## 9. 舜通模板映射

| 现有逻辑 | 图形化节点 | 关键输入 | 关键输出 |
| --- | --- | --- | --- |
| TQ / CN / BW 平均 | `meterAverage` | `1030..1043`、`1130..1143`、`4636..4643` | `201..225`、`251..266` |
| 负荷派生 | `derivedLoad` | TQ、CN、BW 或直接 FH | `309..325` |
| BMS 派生 | `bmsDerived` | `1556/1557/1566/1586/1587/398/399` | `1552/1553/1615/1616` |
| COS | `cosCompensation` | `514`、TQ P/Q | `505..508`、`601..604`、`8` |
| LV / HV | `voltageCompensation` | `544..547`、`533`、`535`、CN U | `605..612`、`10/12` |
| CD / FD | `chargeDischarge` | `451/452/455/456`、SOC、台区限制；`mode=dischargeThenCharge` 时先按 `455/456` 放电到目标 SOC，再按 `451/452` 充电到目标 SOC | `613/614`、`14/16`，顺序模式可输出阶段状态点 |
| 充放电测试 | `chargeDischargeCycleTest` | SOC、三相功率或总功率、放电深度、充电深度 | 默认复用 DS 的 `615..618`、`18`，并通过 `phaseStateOutput` 保存阶段 |
| DS | `timedChargeDischarge` | `400..423`、`424..447`、`760..783` | `461/462`、`615..618`、`18` |
| GF | `photovoltaicCharge` | `581/583`、FH、反送限制 | `619..622`、`22` |
| PH | `phaseBalance` | `562`、TQ、CN | `564..567`、`623..625`、`20` |
| ZR | `reserveCapacity` | `23/588`、FH、反送限制 | `24` 和 PowerSolve 约束 |
| SK | `skOverride` | `590/591` | `26` 和 PowerSolve 覆盖 |
| PowerSolve | `pcsPowerSolve` | 所有模式输出、BMS 限制、SOC 限制；其中 CD/FD 的 `613/614` 会按三相均分并进入 P 输出仲裁 | `627..632`，可选 `1318..1323` 待写命令 |
| PCS 下载 | `pcsWriteback` | `627..632`、`1399` | `1318..1323` 待写命令 |

`chargeDischarge` 默认保持兼容的独立充电 / 放电判断。配置 `mode=dischargeThenCharge` 后变为 EMS 内部顺序策略：阶段 `1` 表示放电，SOC 小于等于 `fdTargetSocIndex` 后进入阶段 `2` 充电，SOC 大于等于 `cdTargetSocIndex` 后进入阶段 `3` 完成并输出零功率。可通过 `phaseStateOutput` 把阶段写入 EMS 虚拟点，并随 `graphStateFile` 持久化。

`chargeDischargeCycleTest` 专用于生产联调的一充一放循环测试，不改变 `chargeDischarge` 的老策略语义。参数支持两种写法：直接配置 `phasePowerA/phasePowerB/phasePowerC`、`dischargeDepth`、`chargeDepth`，或配置 `phasePowerAIndex/phasePowerBIndex/phasePowerCIndex`、`totalPowerIndex`、`dischargeDepthIndex`、`chargeDepthIndex` 从 EMS 虚拟点读取。阶段 `1` 表示放电，输出负三相有功；SOC 小于等于放电深度后切换到阶段 `2` 充电，输出正三相有功；SOC 大于等于充电深度后回到阶段 `1`，循环执行。`phaseStateOutput` 必须落在可路由虚拟点上，例如 400xxx 映射项目使用 `400017`，模板本体点使用 `17`。

## 10. 平台图形化界面

平台端建议分为 4 个区域。

### 10.1 模板入口

- 「加载舜通 EMS 默认模板」：生成标准节点图和默认点位映射。
- 「从当前配置导入」：读取已有 `graphEms` JSON 并反显。
- 「影子运行」开关：生成配置时把 `pcsWriteback.submitWrites=false`。

当前平台端已在 `MQTT / OTA / App -> EMS 策略` 页面落地最小闭环：

- `GET /api/config/app/ems/shuntong-graph` 读取当前 `runtime/logic/shuntong_ems_graph.json`；文件不存在时返回内置舜通模板。
- `POST /api/config/app/ems/shuntong-template/apply` 写入模板，并同步 App 配置中的 `computeEngine.rules[]`。
- `PUT /api/config/app/ems/shuntong-graph` 保存编辑后的 graph，并执行节点、边和环路校验。
- `GET /api/config/app/ems/native` 读取 EMS 原生配置；如果文件不存在，会从当前 graph 反推一份标准模式和标准点位绑定。
- `PUT /api/config/app/ems/native` 保存 EMS 原生配置，先校验必填点位，再编译覆盖 `runtime/logic/shuntong_ems_graph.json`。
- 配置包生成会扫描 `computeEngine.rules[].script.graphFile`，把 `runtime/logic/shuntong_ems_graph.json` 放入 OTA 包。
- 配置包如果存在 `runtime/logic/shuntong_ems_native_config.json`，会随 graph 一起带入包内，作为平台下次编辑的源配置；边端运行仍只依赖 graph。

## 10.1.1 EMS 原生配置模型

为了避免在运行时继续套 `VarList / GLList / script / graph` 多层解析，平台新增一层更贴近网关原始设计的 EMS 原生配置。它只作为平台编辑和提交校验模型，边端仍执行编译后的 `graphEms`。

```text
当前点表
  -> 平台选择 EMS 模式
  -> 绑定 EMS 标准点位
  -> 提交时校验缺失/读写属性
  -> 编译为 shuntong_ems_graph.json
  -> 边端 ComputeEngine 执行 graphEms
```

原生配置文件路径：

```text
runtime/logic/shuntong_ems_native_config.json
```

第一版标准点位覆盖稳定运行必须直接选择的点位：

| 标准点位 | 含义 | 默认 index | 编译目标 |
| --- | --- | --- | --- |
| `bms.soc` | BMS SOC | `1570` | `ds.bmsSocIndex`、`cd_fd.bmsSocIndex`、`power_solve.bmsSocIndex` |
| `meter.storage.ua` | 储能侧 A 相电压 | `251` | `ds.cnUaIndex`、`lv_hv.cnUaIndex` |
| `meter.storage.ub` | 储能侧 B 相电压 | `252` | `ds.cnUbIndex`、`lv_hv.cnUbIndex` |
| `meter.storage.uc` | 储能侧 C 相电压 | `253` | `ds.cnUcIndex`、`lv_hv.cnUcIndex` |
| `pcs.com_status` | PCS 通讯允许 | `1399` | `pcs_writeback.comStatusIndex` |
| `pcs.active_power_set_a/b/c` | PCS A/B/C 相有功下发 | `1318..1320` | `pcs_writeback.pControlAIndex..pControlCIndex` |
| `pcs.reactive_power_set_a/b/c` | PCS A/B/C 相无功下发 | `1321..1323` | `pcs_writeback.qControlAIndex..qControlCIndex` |

第一版模式定义仍沿用当前舜通计算逻辑节点：

| EMS 模式 | graph 节点 |
| --- | --- |
| 电表平均与负荷派生 | `tq_average`、`cn_average`、`fh_derived` |
| BMS 派生 | `bms` |
| 计划曲线 | `ds` |
| 手动充放电 | `cd_fd` |
| 无功补偿 | `cos` |
| 电压补偿 | `lv_hv` |
| 光伏优先充电 | `gf` |
| 三相平衡 | `ph` |
| 动态增容 | `zr` |
| 总控设定 | `sk` |
| PCS 功率仲裁与下发 | `power_solve`、`pcs_writeback` |

提交校验规则：

- 启用某个模式后，该模式声明的必填标准点位必须绑定。
- 开启 PCS 下发后，`pcs.com_status` 和 A/B/C 相有功下发点必须绑定。
- 当前点表存在时，绑定 index 必须能在当前点表里找到；找不到时阻止编译并返回缺失清单。
- 写入标准点位如果对应测点没有 `write.enable=true`，平台给出警告；边端仍由 `PendingWriteCommand` 和设备点位写权限做最终保护。

这层配置不是新的边端执行格式。边端不解析标准点位名称、不按中文名称匹配测点、不读取工程点表，只读取平台已经编译好的 graph JSON。

### 10.2 策略块画布

画布显示预定义策略块，不提供任意代码节点。用户可以启停节点、调整执行顺序、查看输入输出关系。

节点外观建议：

- 电表平均：蓝色数据块。
- 策略模式：绿色控制块。
- PCS 仲裁：橙色核心块。
- 写回：红色设备写入块。

节点连线只表示依赖关系，不允许形成环。

### 10.3 属性面板

选中节点后显示结构化表单：

- 点位选择器：按「元器件 / 设备 -> 测点」选择，也支持按测点名称、index、设备、接口过滤。
- 数值参数：梯度、死区、TTL、扫描周期。
- 24 小时表：DS `scheduleCurve` 使用 hour、power、targetSoc 配置。
- 写回参数：通讯状态点、目标控制点、死区、是否整数截断。

点位选择器必须存储 index，不依赖测点名称做运行时绑定。

当前第一版属性面板覆盖：

- 节点启停。
- DS 以 `scheduleCurve` 为主配置 0-23 点的 hour、power、targetSoc；旧 72 点功率、SOC、模式三组起始点位仅用于兼容导入和历史配置运行。
- PCS 写回开关、通讯允许点位、`627..632` 来源点和 `1318..1323` 控制目标点。
- 通用模块模板选择、默认停用新增、选中编辑和删除节点；删除节点时同步清理显式边。
- `formula/switch/hysteresis/debounce/sequence/rateLimit/controlGate/controlWrite/feedbackVerify` 参数通过节点参数表编辑，数组结构使用 JSON 数组编辑器。
- 发布前检查通用输出冲突、点位存在性、控制点写权限、许可点和高优先级租约风险。

后续再补节点级完整参数表单、画布拖拽和平台侧模拟执行。

### 10.4 调试视图

调试视图用于上线前核对：

- 输入一组最新值快照。
- 平台端按同一份 graph JSON 做模拟执行。
- 展示每个节点的输入、输出、跳过原因和最终 PCS 指令。
- 支持与 `legacyEms` 输出对比，差异超过阈值时标红。

## 11. 安全与校验

边端必须执行以下校验：

- 节点类型必须在白名单中。
- graph 无环，节点数量和边数量不超过限制。
- 所有点位 index 必须是正整数。
- `deviceWrite` 输出必须有路由且 `write.enable=true`。
- PCS 写回默认要求 `1399 == 1`。
- 所有计算结果必须是有限数值，`NaN` 和无穷值按坏质量处理。
- 写回必须经过 `PendingWriteCommand`，不能直接调用协议客户端。
- 图版本不兼容时拒绝加载，并输出明确错误。

平台端也要做同样校验，但不能依赖平台校验保证边端安全。

## 12. 兼容策略

- `legacyEms` 不删除，继续作为旧工程兼容和回归基准。
- `graphEms` 第一阶段只覆盖舜通 EMS 模板。
- 同一工程允许配置 `legacyEms` 和 `graphEms` 两条规则，但只有一条允许开启 PCS 写回。
- 迁移验收时使用同一输入快照分别跑 `legacyEms` 和 `graphEms`，比较 `461/462/601..632/1318..1323`。

### 12.1 完全替代验收报告

边端新增 `EmsParityCheck`，它先读取当前共享内存快照，再把同一份输入复制到两个独立临时共享内存，分别执行 `legacyEms` 和 `graphEms`。临时执行不会修改生产 latest，也不会把控制命令写入生产队列。

```bash
/opt/modbus-gateway/bin/EmsParityCheck \
  --app-config /opt/modbus-gateway/config/runtime/apps/mqtt-service.json \
  --legacy-rule shuntong_legacy \
  --graph-rule shuntong_graph \
  --report /opt/modbus-gateway/config/runtime/logic/ems_graph_parity_report.json
```

默认比较 `461/462/601..632`，可通过 `--indexes` 指定其他输出，`--tolerance` 设置绝对误差。没有实时共享内存时可重复使用 `--point INDEX=VALUE` 构造可复现输入。报告包含两份 graph 文件内容指纹。`comparisonCount` 表示实际参与比较的点，`missingInBoth` 单独列出双方都没有产出的索引；只有 `comparisonCount>0` 且 `mismatchCount=0` 时才会写出 `passed=true`。生产验收应增加 `--require-refreshed`，防止把共享内存中的旧值误判为本轮等价；状态策略还应使用 `--iterations/--step-ms` 连续运行多轮。

Windows 的“发布就绪 -> Graph 完全替代”同时检查：启用的 `graphEms` 规则、`graphStateFile`、模式所需节点、双写冲突和与当前 `graphCode` 匹配的零差异报告。没有报告只能停留在影子运行阶段，不能判定为完全替代。

## 13. 测试策略

边端测试：

- JSON 解析和字段默认值测试。
- graph 校验测试：未知节点、重复 ID、环、缺失路由、不可写输出。
- 节点单测：DS、GF、PH、PowerSolve、PCS 写回。
- 对比测试：同一输入下 `legacyEms` 和 `graphEms` 输出一致。
- 边端 aarch64 运行测试：只运行交叉编译后的测试二进制，不在边端编译。

平台测试：

- 舜通模板加载后生成合法 graph JSON。
- 点位选择器保存 index，不用名称做绑定。
- 24 小时表编辑后能正确写入 `400..423/424..447/760..783`。
- 调试视图能展示节点输入输出和差异。

## 14. 分阶段落地

### 阶段 1：边端 graphEms 基础能力

- 增加 graph 配置模型和解析。
- 增加 graph 校验器。
- 增加 `GraphEmsEngine` 执行框架。
- 接入 `ComputeEngineService` 的 `script.type=graphEms`。

### 阶段 2：舜通领域节点

- 实现 `meterAverage`、`derivedLoad`、`bmsDerived`。
- 实现 `cosCompensation`、`voltageCompensation`、`chargeDischarge`、`skOverride`、`reserveCapacity`。
- 实现 `timedChargeDischarge`、`photovoltaicCharge`、`phaseBalance`。
- 实现 `pcsPowerSolve` 和 `pcsWriteback`。
- 用 `legacy_ems_test` 扩展对比测试。

### 阶段 3：模板和配置样例

- 生成 `config/examples/shuntong_ems_graph.json`。
- 生成 `config/examples/mqtt-service-graph-ems-example.json`。
- 更新生产配置说明和验收步骤。

### 阶段 4：平台图形化配置

- 网关配置界面增加「EMS 策略」入口。
- 实现舜通模板加载。
- 实现节点画布、属性面板、点位选择器和调试视图。
- 配置包 OTA 时包含 graph JSON。

### 阶段 5：现场切换

- 先启用影子运行，关闭 PCS 写回。
- 对比关键输出和待写命令。
- 通过后开启 `pcsWriteback.submitWrites=true`。
- 保留回退到 `legacyEms` 的配置项。

## 15. 完全模块化等价替换（2026-07-13）

### 15.1 生成物与模块边界

`tools/generate_shuntong_modular_graph.ps1` 读取旧领域图中的点位和参数，生成以下两份配套文件：

- `shuntong_ems_modular_graph.json`：315 个节点、314 条有向边，旧领域节点数量为 0。
- `device_ems_modular_virtual.json`：保留原 130 个 EMS 虚拟点，并增加 219 个 `700000..700218` 内部路由点；内部点不全量上传、不落历史。

| 旧领域职责 | 模块化实现 |
| --- | --- |
| TQ/CN 平均 | 独立 `windowAggregate` |
| FH、视在功率、功率因数、BMS 派生 | `formula`，零除数使用 `safeDivide` |
| COS、电压、手动充放电、光伏、三相平衡 | `formula + switch + controlGate` |
| 24 小时计划 | `scheduleSelect`，逐相渐变由公式和选择器显式展开 |
| 当前本地小时 | `timeSource(component=hour)` |
| PCS 模式候选合并 | `phaseArbiter` |
| 正反送、增容、P/Q/S、BMS功率、SOC限制 | `powerConstraint` |
| PCS 六路写回 | 6 个固定目标 `controlWrite(valueMode=truncate)` |

`timeSource.component` 不是“配置几点”，而是选择写入虚拟点的时间分量。时间来自边端 Linux 系统时间和系统时区：`hour` 输出 0-23，`minute` 输出 0-59，`second` 输出 0-59，`minuteOfDay` 输出 0-1439（例如 01:30 输出 90），`weekday` 输出 1-7（周一为 1、周日为 7）。`scheduleSelect` 当前直接读取设备本地小时，不依赖 `timeSource` 输出；需要分钟级条件时，应使用 `minuteOfDay` 输出点连接公式或条件模块。

`formula` 额外支持 `square/sqrt/acos/tan/sin/cos/safeDivide`。`safeDivide` 可配置零除数返回值；`sqrt/acos` 可使用 `invalidPolicy=skip` 保留旧逻辑“本轮不覆盖输出”的语义。节点还可用 `profileIntKey/profileIntValue..4` 按设备型号执行，例如 BMS 型号 1/3 的当日累计量。`profileKey` 要求设备存在，`optionalProfileKey` 要求可选设备被明确启用，`profileDisabledKey` 要求对应设备不存在，用于表达 `TQ-CN`、`TQ-BW` 和独立 FH 三条互斥取数路径。

原来关闭的 `charge_discharge_test` 已展开为 `sequence + formula + switch`。它使用业务状态 Index 17 保存“1=放电、2=充电”，支持 20%/95% 深度切换和三相独立功率。节点默认保留在图中但由 `graphProfile.CHARGE_DISCHARGE_TEST=0` 关闭；启用 profile 只开放计算链，不会自动开启六个 PCS `controlWrite.submitWrites`。

### 15.2 等价校验

`EmsParityCheck` 支持旧领域 graph 与模块化 graph 直接比较，不要求 app 中已有计算规则：

```bash
/opt/modbus-gateway/bin/EmsParityCheck \
  --app-config /opt/modbus-gateway/config/runtime/apps/mqtt-service.json \
  --baseline-graph /opt/modbus-gateway/config/runtime/logic/shuntong_ems_graph.json \
  --candidate-graph /opt/modbus-gateway/config/runtime/logic/shuntong_ems_modular_graph.json \
  --profile Meter_TQ=true --profile Meter_CN=true \
  --profile PCS_MODEL=1 --profile BMS_MODEL=2 \
  --indexes 461,462,601,602,603,604,627,628,629,630,631,632 \
  --require-refreshed --iterations 10 --step-ms 200
```

工具把同一现场快照复制到两套隔离共享内存；不会修改生产 latest，也不会向生产写回队列提交命令。`--point INDEX=VALUE` 可重复使用，用于构造 SOC、功率限制、时段等可复现场景。

### 15.3 22.16 影子验收结果

测试机 `192.168.22.16` 已完成以下场景，全部 `mismatchCount=0`：

| 场景 | 比较结果 |
| --- | --- |
| 正常输入全链路 | 101 个本轮刷新输出零差异 |
| 低 SOC / 高 SOC | 各 42 个关键输出零差异 |
| 手动总控覆盖 | 42 个关键输出零差异 |
| 动态增容 + BMS 限功率 | 42 个关键输出零差异 |
| BMS 型号 1 当日累计量 | 4 个派生输出零差异 |
| 定时充电，10 轮、200ms 步进 | 18 个状态/功率输出零差异 |
| 定时放电，10 轮、200ms 步进 | 18 个状态/功率输出零差异 |
| 循环充放电首次 SOC=80 / SOC=20 | 阶段、运行标志、三相及总功率 6 点零差异 |
| Profile 矩阵 | TQ 关闭、CN 关闭、BW 支路、直接 FH、BMS 型号 3 均通过 |

测试机已经安装新版 `ComputeEngine` 和 `EmsParityCheck`，候选 graph 与虚拟点文件已放入 runtime。当前 `computeEngine.enabled=true`，`compute-engine@mqtt-service.service` 已启用模块化 Graph 影子运行；所有候选写回仍保持 `submitWrites=false`，因此只计算并发布虚拟点，不会向 PCS 提交控制命令。

回滚文件仅保留在 `/opt/modbus-gateway/backup/ems-modular-preactivate-20260713`。模块化虚拟点文件已经加入 `deviceConfigFiles`，唯一的 `graphEms` 规则为 `shuntong_modular_shadow`。生产写回启用前仍必须确认旧策略不同时写回，并单独审批六个 `controlWrite.submitWrites`，不得把影子运行等同于真实控制已启用。

### 15.4 运行时性能优化与实测

模块化 Graph 一轮会多次读取相同物理点，并让后续节点读取前序节点的中间结果。Linux 共享内存读取还会检查映射对象是否被重建；逐节点读取会重复执行大量 `shm_open/fstat`。`GraphEmsEngine` 因此采用以下边界：

- 构造时缓存拓扑执行顺序，不在每轮重复排序。
- 每轮开始按 Graph 涉及的索引批量读取一致快照。
- 本轮节点输出立即写入本地快照，后续节点直接读取，仍保持既有拓扑语义。
- 无法从老配置静态识别的默认索引保留按需回退，兼容旧 Graph。
- 状态持久化改为批量读取；虚拟点对共享内存的写入和控制提交语义不变。

2026-07-13 先在 `192.168.22.16`、315 节点/314 边的模块化影子图上完成快照优化实测：

| 扫描与规则周期 | 优化前 CPU | 优化后 CPU | RSS |
| --- | ---: | ---: | ---: |
| 1000 ms | 约 35% | 稳定约 1%~2% | 约 51 MB |
| 200 ms | 约 69% | 稳定约 4%~6% | 约 52 MB |

随后补齐循环策略和 Profile 分支，当前现场图为 372 节点/371 边，1000 ms 周期实测约 2.4% CPU、约 54 MB RSS。EMS 虚拟共享内存待写命令为 0，六个 `controlWrite` 均为 `submitWrites=false`。宿主机构建的 `legacy_ems_test` 通过；22.11 完成交叉编译；22.16 上 PCS 六路输出连续 10 轮、200 ms 步进比较 `mismatchCount=0`。包含 `461/462` 的非定时时段校验会报告旧 Graph 本轮未刷新这两个点，这属于旧策略的时段语义，不应把该 `require-refreshed` 结果误判为数值差异。

异常恢复验收中主动对 ComputeEngine 执行一次 `SIGKILL`，systemd 在 5 秒后自动拉起，`NRestarts` 从 0 增至 1；共享内存值保留并继续刷新，待写队列仍为 0。这一计数是受控验收结果，不是现场无故崩溃。损坏状态文件的宿主回归也确认：恢复错误会记录 `restoreState` 告警，计算继续执行并原子重写有效状态文件。

### 15.5 Windows 展示约束

Windows 客户端必须把“传统策略兼容”和“模块化策略”作为两个明确区域：

- 传统区只读展示旧领域节点和迁移入口，不允许伪装成通用模块。
- 模块化区只接受通用节点，画布展示中间索引、条件、数据流、仲裁和约束。
- 导入旧图后先调用生成/转换流程，完成零差异报告后才能标记为“可替代”。
- `missingInBoth` 只表示未覆盖，不表示错误；发布检查必须同时关注 `comparisonCount` 和 `--require-refreshed`。
