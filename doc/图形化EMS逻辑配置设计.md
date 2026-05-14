# 图形化 EMS 逻辑配置设计

## 1. 背景

当前舜通 EMS 逻辑已经迁移为边端 `LegacyEmsEngine`，由 `ComputeEngine` 通过 `script.type=legacyEms` 周期执行。该方式适合快速复刻旧 `script.cpp` 行为，但逻辑被固化在 C++ 中，平台端只能配置启停、点表文件和少量 profile 参数，不能图形化调整策略块、点位映射、调度表和输出写回规则。

下一阶段目标是把 EMS 逻辑改造成可由平台图形化配置、边端按配置解释执行的策略图。现有 `LegacyEmsEngine` 保留为金标准和兼容执行器，新能力以 `script.type=graphEms` 并行接入。

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
      "Meter_TQ": "1",
      "Meter_CN": "1",
      "Meter_BW": "0",
      "Meter_FH": "0"
    }
  }
}
```

`graphFile` 使用相对路径时，按 app 配置文件所在目录解析。`graphProfile` 用于保留工程级开关和设备型号选择，避免把每个模板都复制成不同文件。

当前边端已支持 `script.type=graphEms`。`ComputeEngineService` 会按 `ruleCode` 缓存 `GraphEmsEngine`，每轮扫描加载同一份 graph 配置执行。运行态状态文件由 `script.graphStateFile` 指定；未配置时使用 `/opt/modbus-gateway/data/graph_ems_state_<ruleCode>.json`。示例文件：

| 文件 | 作用 |
| --- | --- |
| `config/examples/shuntong_ems_graph.json` | 舜通 EMS 默认策略图模板 |
| `config/examples/mqtt-service-graph-ems-example.json` | `graphEms` app 配置示例 |

## 6. 策略图 JSON

策略图是版本化 JSON。平台端只生成受支持的节点类型，边端严格校验。

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
2. 首轮从 `graphStateFile` 恢复上一轮输出 latest，恢复过程只写 latest，不提交设备写命令。
3. 节点按拓扑顺序执行。
4. 节点通过 `PointStoreRouter` 读取 latest 值。
5. 节点输出写入 latest 或提交 `PendingWriteCommand`。
6. 每轮结束保存运行态状态，状态文件默认路径为 `/opt/modbus-gateway/data/graph_ems_state_<ruleCode>.json`。
7. 执行结果记录节点耗时、跳过原因、写入数量和错误信息。

节点失败时不影响整个进程退出。默认策略是：当前节点输出保持上次值或写入坏质量，具体由节点类型定义。

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
| `timedChargeDischarge` | DS 定时充放电 | 有 | 已实现 |
| `photovoltaicCharge` | GF 光伏充电窗口 | 无 | 已实现 |
| `phaseBalance` | PH 三相平衡 | 无 | 已实现 |
| `skOverride` | SK 总控设定 | 无 | 已实现 |
| `reserveCapacity` | ZR 动态增容 | 无 | 已实现 |
| `pcsPowerSolve` | PCS P/Q 输出仲裁和限幅 | 无 | 已实现 |
| `pcsWriteback` | PCS 指令写回 | 有 | 已实现 |
| `formula` | 受限表达式计算 | 无 | 第二阶段 |
| `switch` | 条件分支选择 | 无 | 第二阶段 |

第一阶段优先实现 EMS 领域节点。通用 `formula` 只用于少量辅助计算，不承载核心 PCS 仲裁逻辑。

`derivedLoad.params.source` 默认值为 `tqCn`，表示 `FH=TQ-CN`；配置为 `tqBw` 或 `bw` 时表示 `FH=TQ-BW`；配置为 `fh`、`direct` 或 `directFh` 时直接读取 `fhPaIndex..fhQ3Index`，并继续计算 `317..325` 的视在功率、功率因数和三相不平衡率。

当前 `pcsPowerSolve` 默认只写 `627..632` 等 latest 输出，不直接写设备。要实际下发 PCS，需要单独启用 `pcsWriteback.submitWrites=true`，或配置 `pcsPowerSolve.params.submitWrites=true` 由该节点内部复用同一套写回逻辑，按 `comStatusIndex` 和 `pControlAIndex..qControlCIndex` 生成待写命令。模板默认关闭写回，适合影子运行。

## 9. 舜通模板映射

| 现有逻辑 | 图形化节点 | 关键输入 | 关键输出 |
| --- | --- | --- | --- |
| TQ / CN / BW 平均 | `meterAverage` | `1030..1043`、`1130..1143`、`4536..4543` | `201..225`、`251..266` |
| 负荷派生 | `derivedLoad` | TQ、CN、BW 或直接 FH | `309..325` |
| BMS 派生 | `bmsDerived` | `1556/1557/1566/1586/1587/398/399` | `1552/1553/1615/1616` |
| COS | `cosCompensation` | `514`、TQ P/Q | `505..508`、`601..604`、`8` |
| LV / HV | `voltageCompensation` | `544..547`、`533`、`535`、CN U | `605..612`、`10/12` |
| CD / FD | `chargeDischarge` | `451/452/455/456`、SOC、台区限制 | `613/614`、`14/16` |
| DS | `timedChargeDischarge` | `400..423`、`424..447`、`760..783` | `461/462`、`615..618`、`18` |
| GF | `photovoltaicCharge` | `581/583`、FH、反送限制 | `619..622`、`22` |
| PH | `phaseBalance` | `562`、TQ、CN | `564..567`、`623..625`、`20` |
| ZR | `reserveCapacity` | `23/588`、FH、反送限制 | `24` 和 PowerSolve 约束 |
| SK | `skOverride` | `590/591` | `26` 和 PowerSolve 覆盖 |
| PowerSolve | `pcsPowerSolve` | 所有模式输出、BMS 限制、SOC 限制 | `627..632`，可选 `1318..1323` 待写命令 |
| PCS 下载 | `pcsWriteback` | `627..632`、`1399` | `1318..1323` 待写命令 |

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
