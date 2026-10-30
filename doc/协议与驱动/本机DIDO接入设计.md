# 本机 DIDO 接入设计

## 1. 背景

当前硬件具备 18 路 DI 和 8 路 DO。旧项目里部分干接点被写在 `modbusTCP_Reg.xml` 中，并在点表里显示为 `modbusTCP`，但这些点实际是本机 DI/DO，不应继续生成 Modbus TCP 设备。

本设计目标是把本机 DI/DO 纳入当前网关统一点位模型：

- DI 周期采样后写入共享内存。
- DO 支持平台 / MQTT / 计算服务按 `index` 写入，并真正下发到本机继电器。
- DIDO 数据继续参与实时监测、全量上传、变位事件、告警、SQLite 存储和配置 OTA。
- Java 平台能在“多串口 / 多接口配置”和“老旧项目兼容导入”中生成 DIDO 配置。

## 2. 说明书结论

硬件说明书路径：

```text
config/SZR-EMS-E1 储能控制器技术说明书  V2.01.pdf
```

### 2.1 DO 通道

DO 通过输出对应 GPIO 电平控制，继电器隔离，常开，拉低闭合。

| 通道 | GPIO | 逻辑 |
| --- | ---: | --- |
| DO1 | 231 | 写 0 闭合，写 1 断开 |
| DO2 | 238 | 写 0 闭合，写 1 断开 |
| DO3 | 239 | 写 0 闭合，写 1 断开 |
| DO4 | 129 | 写 0 闭合，写 1 断开 |
| DO5 | 130 | 写 0 闭合，写 1 断开 |
| DO6 | 131 | 写 0 闭合，写 1 断开 |
| DO7 | 132 | 写 0 闭合，写 1 断开 |
| DO8 | 133 | 写 0 闭合，写 1 断开 |

对平台暴露的语义建议固定为：

- 点值 `1` 表示 DO 动作 / 继电器闭合。
- 点值 `0` 表示 DO 释放 / 继电器断开。
- 驱动内部负责把逻辑值转换为 GPIO 电平，即 `1 -> GPIO 0`，`0 -> GPIO 1`。

### 2.2 DI1-DI5 湿接点

DI1-DI5 为有源输入，输入电压 DC 5-30 V。

| 通道 | GPIO | 输入 DI | GPIO 读值 | 逻辑 |
| --- | ---: | ---: | ---: | --- |
| DI1 | 224 | 1 | 0 | 反相 |
| DI2 | 225 | 1 | 0 | 反相 |
| DI3 | 226 | 1 | 0 | 反相 |
| DI4 | 227 | 1 | 0 | 反相 |
| DI5 | 230 | 1 | 0 | 反相 |

对平台暴露的语义建议固定为：

- 点值 `1` 表示 DI 输入有效。
- 点值 `0` 表示 DI 输入无效。
- 驱动内部负责反相，即 `GPIO 0 -> 点值 1`，`GPIO 1 -> 点值 0`。

### 2.3 DI6-DI18 干接点

DI6-DI18 为无源开关量输入。

| 通道 | GPIO | 输入 DI | GPIO 读值 | 逻辑 |
| --- | ---: | ---: | ---: | --- |
| DI6 | 134 | 1 | 0 | 反相 |
| DI7 | 135 | 1 | 0 | 反相 |
| DI8 | 136 | 1 | 0 | 反相 |
| DI9 | 137 | 1 | 0 | 反相 |
| DI10 | 138 | 1 | 0 | 反相 |
| DI11 | 143 | 1 | 0 | 反相 |
| DI12 | 128 | 1 | 0 | 反相 |
| DI13 | 139 | 1 | 0 | 反相 |
| DI14 | 140 | 1 | 0 | 反相 |
| DI15 | 341 | 1 | 0 | 反相 |
| DI16 | 340 | 1 | 0 | 反相 |
| DI17 | 339 | 1 | 0 | 反相 |
| DI18 | 338 | 1 | 0 | 反相 |

现场验证结论：DI6-DI13 短接有效时 GPIO 从 `1` 变为 `0`，断开后回到 `1`，因此默认配置统一使用 `activeHigh=false`。

## 3. 调用方式

说明书提供两种 GPIO 调用方式。

### 3.1 sysfs GPIO

通用路径：

```text
/sys/class/gpio/export
/sys/class/gpio/gpio<gpio>/direction
/sys/class/gpio/gpio<gpio>/value
```

读 DI 流程：

```text
echo <gpio> > /sys/class/gpio/export
echo in > /sys/class/gpio/gpio<gpio>/direction
cat /sys/class/gpio/gpio<gpio>/value
```

写 DO 流程：

```text
echo <gpio> > /sys/class/gpio/export
echo out > /sys/class/gpio/gpio<gpio>/direction
echo <0|1> > /sys/class/gpio/gpio<gpio>/value
```

驱动直接操作 sysfs 文件，不依赖 shell 脚本：

- 每次写控制文件（`export`、`direction`）都检查 `open`/`write`/`close` 的结果，被内核拒绝的写入作为错误上报（点质量 `0`），不会被吞掉。
- `export` 后确认 `gpio<gpio>/direction` 已出现才视为导出成功；失败的导出不被记住，单次调用内重试一次，之后至少间隔 5 秒再试，避免一个错误的 GPIO 编号拖慢其他输入的采集周期。
- DO 设为输出时直接写 `high`/`low` 到 `direction`（一步完成方向和初值，不产生低电平毛刺）。

### 3.2 厂商脚本

说明书还提供了脚本：

```bash
wgpio.sh 132 0
wgpio.sh 132 1
rgpio.sh 132
```

脚本适合人工验证，不建议作为生产驱动主路径。原因是每个点都 fork shell 会增加抖动和 CPU 开销，也不利于错误分类。

## 4. 推荐架构

新增独立边端驱动：

```text
DioDriver
```

运行关系：

```text
DioDriver
  -> 读取 DI GPIO
  -> 读取 / 写入 DO GPIO
  -> 写入 gateway_point_store 或 gateway_point_store_dio
  -> 消费 pendingWrites
  -> 执行 DO 下发

MqttDriver / SystemMonitor / EventEngine / ComputeEngine
  -> 通过共享内存读取 DIDO 点
  -> 通过 pendingWrites 写 DO
```

关键边界：

- DIDO 不归属 Modbus RTU、Modbus TCP 或 DLT645。
- DIDO 配置使用 `protocol.type=local_dio`。
- `DioDriver` 只处理本机 GPIO，不打开串口和 TCP 连接。
- DO 写设备仍走现有共享内存写命令队列，而不是让 MQTT 或 Java 直接操作 GPIO。

## 5. 配置设计

推荐新增运行文件：

```text
config/runtime/devices/device_dio.json
```

### 5.1 顶层结构

```json
{
  "schemaVersion": "1.0.0",
  "protocol": {
    "type": "local_dio",
    "backend": "sysfs_gpio",
    "gpioBasePath": "/sys/class/gpio"
  },
  "collect": {
    "defaultIntervalMs": 200,
    "batchOptimize": false,
    "maxBatchRegisters": 1
  },
  "memoryStore": {
    "sharedMemoryName": "gateway_point_store",
    "maxLatestPoints": 100000,
    "maxPendingWrites": 4096,
    "maxPersistentSamples": 20000,
    "sqlitePath": "/opt/modbus-gateway/data/point_samples_dio.db",
    "persistFlushIntervalMs": 60000,
    "writebackIntervalMs": 100,
    "writebackBatchSize": 64
  },
  "meters": [
    {
      "meterCode": "LOCAL_DIO",
      "deviceName": "本机 DI/DO",
      "points": []
    }
  ]
}
```

说明：

- 当前生产默认仍使用统一共享内存 `gateway_point_store`。
- 后续如切到接口级共享内存分片，可把 DIDO 独立为 `gateway_point_store_dio`，上层通过 `PointStoreRouter` 统一取数。
- `machineCode` 建议由 `device_identity.json` 注入，避免配置 OTA 后和设备身份不一致。

### 5.2 DI 点位

```json
{
  "index": 410006,
  "pointCode": "di_6",
  "name": "DI6",
  "desc": "本机 DI6 干接点输入",
  "category": "status",
  "address": 6,
  "enabled": true,
  "isStore": true,
  "fullUpload": true,
  "reportOnChange": true,
  "persistIntervalSec": 60,
  "read": {
    "enable": true,
    "function": 0,
    "length": 1,
    "dataType": "digital_input",
    "unit": "",
    "intervalMs": 200,
    "gpio": 134,
    "activeHigh": false,
    "debounceMs": 30,
    "cachePolicy": {
      "storeLatest": true,
      "storeHistory": true,
      "historySize": 100,
      "ttlMs": 600000
    }
  },
  "write": {
    "enable": false
  },
  "alarms": [],
  "valueMap": {
    "0": "无效",
    "1": "有效"
  }
}
```

字段建议：

| 字段 | 含义 |
| --- | --- |
| `read.dataType` | `digital_input` 表示本机 DI |
| `address` | DIDO 逻辑通道号 |
| `read.gpio` | 实际 GPIO 编号 |
| `read.activeHigh` | `true` 表示 GPIO 1 为点值 1，`false` 表示 GPIO 0 为点值 1 |
| `read.debounceMs` | 防抖时间，避免干接点抖动导致重复变位事件；按单调时钟计时，系统时间回拨或前跳不会冻结或提前确认输入变化（消防、门禁、急停输入持续更新） |

### 5.3 DO 点位

```json
{
  "index": 420001,
  "pointCode": "do_1",
  "name": "DO1",
  "desc": "本机 DO1 继电器输出",
  "category": "control",
  "address": 1,
  "enabled": true,
  "isStore": true,
  "fullUpload": true,
  "reportOnChange": true,
  "persistIntervalSec": 60,
  "read": {
    "enable": true,
    "function": 0,
    "length": 1,
    "dataType": "digital_output",
    "unit": "",
    "intervalMs": 500,
    "gpio": 231,
    "activeHigh": false,
    "cachePolicy": {
      "storeLatest": true,
      "storeHistory": true,
      "historySize": 100,
      "ttlMs": 600000
    }
  },
  "write": {
    "enable": true,
    "function": 0,
    "length": 1,
    "dataType": "digital_output",
    "min": 0,
    "max": 1,
    "step": 1,
    "allowedValues": [0, 1],
    "verifyAfterWrite": true,
    "verifyDelayMs": 50,
    "verifyByRead": true
  },
  "alarms": [],
  "valueMap": {
    "0": "断开",
    "1": "闭合"
  }
}
```

字段建议：

| 字段 | 含义 |
| --- | --- |
| `read.dataType` | `digital_output` 表示本机 DO 状态 |
| `read.gpio` | 实际 GPIO 编号 |
| `read.activeHigh=false` | 因为说明书定义 DO 拉低闭合 |
| `write.enable=true` | 允许通过 pendingWrites 下发 |
| `write.startupValue` | 可选，`0` 或 `1`：上电后驱动首次启动时该 DO 的逻辑值；不填为 `0`（非激活）。驱动重启时引脚已是输出，保持原电平 |
| `write.verifyByRead=true` | 写后回读“输出状态”：读到的是 SoC 的 GPIO 输出锁存，只能证明输出已置位，不能证明继电器触点动作 |
| `write.verifyIndex` | 可选：触点反馈用的独立 DI（继电器/接触器辅助触点）的网关 index，见“输出状态与触点反馈” |
| `write.verifyValue` | 可选，`0`/`1`：输出状态为 `1` 时触点反馈应读到的值；输出状态为 `0` 时期望相反值。不填为 `1`（常开辅助触点），常闭辅助触点填 `0` |
| `write.verifyTimeoutMs` | 写命令等待触点反馈的时间，也是持续不一致告警的动作宽限时间；默认 `3000`，`1`–`60000`；必须大于反馈 DI 的 `debounceMs` |
| `write.feedbackAlarmDelayMs` | 持续不一致告警的去抖：宽限结束后不一致持续这么久才告警；默认 `2000`，`0`–`600000`；只对配置了 `verifyIndex` 的 DO 有效 |
| `write.feedbackRecoveryMs` | 告警恢复判据：触点反馈与输出状态重新一致并持续这么久才恢复；默认 `2000`，`0`–`600000` |
| `write.feedbackAlarmIndex` | 可选：承载该 DO 持续不一致告警的点（`read.dataType=feedback_alarm`）的 index，见“持续不一致告警” |

#### DO 启动与退出状态

- 驱动启动、开始采集和执行写命令之前，检查每个可写 DO（`write.enable=true` 且 `write.dataType=digital_output`）：
  - 引脚还不是输出（上电后首次启动）：设为输出并写入启动值 `write.startupValue`，未配置时为逻辑 `0`（非激活）。按 `activeHigh` 换算电平，例如 `activeHigh=false` 的 DO 启动为高电平（继电器断开）；用内核的 `high`/`low` 方向值一步完成，没有毛刺。
  - 引脚已是输出（驱动重启、升级、崩溃后被拉起）：保持当前电平不动，正在由 EMS 保持的继电器（风机、并网脱扣、HPLC 等）不会因驱动重启断开。
- 某个 DO 启动时无法设置（GPIO 不可用）时记录日志，该点上报坏质量，不会把悬空输入电平当作 DO 状态上报；GPIO 可用后由采集周期补设为启动值。已经是输出的引脚，采集不会改动它的电平（不覆盖已执行的写命令）。
- `--once` 诊断运行不强制写启动值，已是输出的引脚保持原电平，避免干扰正在运行的驱动。
- 进程退出（停止服务、崩溃）时不主动改变输出，DO 保持最后的电平；驱动再次启动时也保持（见上），只有整机上电后才回到启动值。
- 只读的 `digital_output` 点（`write.enable=false`）不由驱动驱动，按引脚当前电平上报。

#### 加载校验

以下定义加载时报错并点名测点：

- 同一个 GPIO 既配置为 DI（`digital_input`）又配置为 DO（读或写）。
- 两个可写 DO 使用同一个 GPIO。
- `digital_input` 点打开了 `write.enable`；可写点的 `write.dataType` 不是 `digital_output`。
- 读点缺少 `read.gpio`、`read.dataType` 不是 `digital_input`/`digital_output`、`debounceMs` 为负、`startupValue` 不是 `0`/`1`。
- `write.verifyIndex` 指向自己；指向本配置内的点但该点不是启用的 `digital_input`；`verifyValue` 不是 `0`/`1`；`verifyTimeoutMs` 不在 `1`–`60000` 或不大于反馈 DI 的 `debounceMs`。指向本配置以外的 index 时只能在运行时从共享点库读取，加载时不检查。
- `feedbackAlarmDelayMs`/`feedbackRecoveryMs` 不在 `0`–`600000`；配置了 `feedbackAlarmIndex` 但没有 `verifyIndex`；`feedbackAlarmIndex` 不是本配置内启用的 `feedback_alarm` 点，或被两个 DO 共用；`feedback_alarm` 点没有被任何 DO 的 `feedbackAlarmIndex` 引用，或配置了 `gpio`/`write.enable`。
- 配置了 `verifyIndex` 但没有 `feedbackAlarmIndex` 时给出警告（告警只进日志和状态事件）。

#### 输出状态与触点反馈

DO 写入的回执（`CommandResult.message`、写回记录）和画面区分两件事：

- **输出状态**：网关写到 GPIO 的值（SoC 输出锁存）。锁存回读（`verifyAfterWrite` + `verifyByRead`）只能证明锁存已置位，不能证明继电器触点动作，因此不算“校验”：`verifyAttempted`/`verifyPassed` 不因锁存回读而置位。DO 点本身的值就是输出状态。
- **触点反馈**：接到独立 DI 上的继电器/接触器辅助触点（`write.verifyIndex`）。只有它能确认设备实际动作；`verifyAttempted`/`verifyPassed` 只指它。没有配置时一律写明“未配置反馈”。

能力边界：没有接辅助触点的 DO，软件无法知道继电器、接触器或被控设备是否动作，这是硬件接线决定的，软件不能补上。这类写入只能报“输出状态已置位”，不得报成动作已确认，也不会因此重发。

成功回执（回执消息在共享内存中最多 127 字节，UTF-8）：

| 配置 | 写回阶段 / outcome | `verifyAttempted` / `verifyPassed` | 回执示例 |
| --- | --- | --- | --- |
| 无 `verifyIndex`，不回读 | `writeback-submitted` / succeeded | `false` / `false` | `输出状态已置为1（未回读）；未配置反馈，触点动作未确认` |
| 无 `verifyIndex`，`verifyAfterWrite` + `verifyByRead` | `writeback-submitted` / succeeded | `false` / `false` | `输出状态已置为1（锁存回读一致）；未配置反馈，触点动作未确认` |
| 配置 `verifyIndex`，触点反馈确认 | `writeback-succeeded` / succeeded | `true` / `true` | `输出状态已置为1；触点反馈(index 998)=1，动作已确认` |

`writeback-submitted` 表示“仅到输出”：outcome 仍是 succeeded（输出确实已置位，不得当失败重发），但它不是动作确认。需要“设备已动作”的消费方（EMS `confirmedControlWrite` 等）必须看 `verifyPassed=true`。

配置了 `write.verifyIndex` 时，写 GPIO 之后在 `verifyTimeoutMs` 内等触点反馈：

- 只认写出之后的新值：反馈 DI 属于同一逻辑设备（同一 `meter`）时，执行器直接读该 DI 的 GPIO，按其 `debounceMs` 判稳（与采集线程无关，采集因优先控制暂停时也能确认）；属于其他设备（例如另一台 IO 模块）时从共享点库读取，只认好质量、未过期且时间戳严格晚于写出时刻的值。坏质量、已过期、写出之前的旧值都不算反馈。
- 期间读到等于期望值的新值：成功，`verifyPassed=true`。

结果分类（`CommandResult` 与写回阶段）：

| 情况 | `success` / `feedbackMismatch` / `outcomeUnknown` | 写回阶段 / outcome | 回执示例 |
| --- | --- | --- | --- |
| 未配置反馈 | `true` / `false` / `false`（`submittedOnly`） | `writeback-submitted` / succeeded | 见上表 |
| 触点反馈确认 | `true` / `false` / `false` | `writeback-succeeded` / succeeded | 见上表 |
| 超时时读到过新值但不等于期望值（反馈不一致） | `false` / `true` / `false` | `writeback-mismatch` / unknown | `反馈不一致：输出状态已置为1，触点反馈(index 998)=0，期望1；已执行，不得自动重发` |
| 输出锁存回读不一致（`verifyByRead`） | `false` / `true` / `false` | `writeback-mismatch` / unknown | `输出状态回读不一致：已写入1，锁存回读0；已执行，不得自动重发` |
| 超时内没有任何可用新值（反馈 DI 读失败；跨设备的值坏质量、过期或未更新） | `false` / `false` / `true` | `writeback-timeout` / unknown | `输出状态已置为1；触点反馈(index 998) 3000ms内无新值，结果未知: stale or bad quality` |
| GPIO 写入被拒绝、值非法等，输出未改变 | `false` / `false` / `false` | `writeback-failed` / failed | 内核错误原因 |

- 反馈不一致≠设备未执行：输出写入已经执行，设备可能已动作（辅助触点卡滞、接线错误）也可能没有。它不是确定失败，回执不得写成“未执行”；调用方和驱动都不得自动重发，不得换新 cmdId 重试，需人工核实现场后再决定。`feedbackMismatch` 到 `writeback-mismatch`（outcome=unknown）的映射由共用回执分类（`writeback_outcome.hpp`）完成，持久化去重的命令（durableControl）在账本中同样记为 `writeback-mismatch`，重放时返回同一结论；`writeback-submitted` 同样原样入账、原样重放。
- 无论确认成功与否，输出保持已置位状态，驱动不自动回退、不补发。
- 晚到反馈（`verifyTimeoutMs` 之后才到）：命令回执已是最终结果（`writeback-timeout` 或同一 DI 判稳后的 `writeback-mismatch`），晚到的值不改写它、不补发回执、不触发重发。晚到反馈只体现在反馈 DI 点自身的值，以及下面的持续不一致告警：与输出状态一致则不告警（已告警的按恢复判据恢复），持续不一致则告警。需要知道动作是否最终发生时，看这两处，不要换新 cmdId 重发。
- 跨设备反馈依赖对方驱动的采集：优先控制租约期间其他驱动的采集会暂停，此时可能等不到新值而报结果未知；需要可靠确认的 DO 应把辅助触点接到同一 DIO 设备。
- 等待期间本驱动的写回线程被占用（最长 `verifyTimeoutMs`），同批后续写命令顺延。

#### 持续不一致告警

命令回执只覆盖写命令后的 `verifyTimeoutMs`。配置了 `write.verifyIndex` 的 DO 由采集线程持续比较输出状态与触点反馈（没有配置反馈的 DO 不做比较，也不告警）：

- 期望值：输出状态为 `1` 时期望 `verifyValue`（默认 `1`），为 `0` 时期望相反值。输出状态取本驱动最近一次成功采集的 DO 值；触点反馈取本驱动采集的 DI 值，或跨设备时取共享点库中好质量、未过期的值。任一方取不到可信值时既不告警也不恢复。
- 动作宽限：输出状态每次变化（命令写入或其他原因）后 `verifyTimeoutMs` 内不判不一致，给触点动作留时间。
- 去抖：宽限结束后不一致连续持续 `feedbackAlarmDelayMs` 才告警，中间一旦一致就重新计时。
- 恢复：告警后两者重新一致并连续持续 `feedbackRecoveryMs` 才恢复。
- 告警与恢复只报告，不改输出、不自动补发动作。
- 输出：日志 `DO feedback mismatch alarm doIndex=… output=… feedbackIndex=… feedback=… (no output is re-sent)` / `DO feedback mismatch cleared …`；有状态发布通道时发布状态事件 `do-feedback-mismatch` / `do-feedback-restored`（字段 `doIndex`、`feedbackIndex`、`outputValue`、`feedbackValue`）。生产 `DioDriver` 没有状态发布通道，平台侧需要告警时配置告警点：

```json
{
  "index": 420101,
  "pointCode": "do_1_feedback_alarm",
  "name": "DO1 触点反馈不一致",
  "category": "status",
  "enabled": true,
  "reportOnChange": true,
  "read": { "enable": true, "dataType": "feedback_alarm", "intervalMs": 500 },
  "write": { "enable": false },
  "alarms": [],
  "valueMap": { "0": "一致", "1": "触点反馈不一致" }
}
```

DO 上配置 `"feedbackAlarmIndex": 420101`。告警点值 `1` 表示告警中、`0` 表示正常，告警/恢复时立即更新；文本带 DO index、输出状态、触点反馈 index 与反馈值，例如 `触点反馈不一致：DO(index 420001) 输出状态=1，触点反馈(index 410006)=0`。告警规则和变位事件仍由 `EventEngine` 按该点配置产生。

#### 独立 DI 的显示

作为辅助触点接入的 DI 在画面上显示为“触点反馈”，与 DO 的“输出状态”分开；没有接辅助触点的 DO 在画面上标注“未配置反馈”，不得把输出状态当作设备已动作展示。

## 6. 写回设备逻辑

DO 必须支持“写共享内存后实际下发设备”。

推荐流程：

```text
平台 / MQTT / ComputeEngine / pointctl
  -> submit PendingWriteCommand(index=420001, value=1)
  -> MemoryPointStore pendingWrites
  -> DioDriver 周期 drain pendingWrites
  -> 根据 index 找到 DO1
  -> value=1 转换为 GPIO 0
  -> 写 /sys/class/gpio/gpio231/value
  -> 读回 GPIO 0（输出状态 = 输出锁存），转换回点值 1
  -> 更新 latest（DO 点的值就是输出状态）
  -> 配置了 write.verifyIndex 时等触点反馈确认动作
  -> 发布命令执行结果
```

错误处理：

- GPIO 不存在：点质量置为 `0`，状态消息上报 `gpio not found`。
- 没有权限：点质量置为 `0`，状态消息上报 `permission denied`。
- 写入值不在 `0/1`：拒绝命令，不触碰 GPIO。
- 写后输出状态回读不一致：反馈不一致（`feedbackMismatch`，`writeback-mismatch`，outcome=unknown），不伪造成功状态，也不写成“未执行”，不自动重发。回读只能确认 SoC 输出锁存（输出状态），确认不了继电器触点是否真的动作；需要触点确认时接一路 DI 辅助触点并配置 `write.verifyIndex`。
- sysfs 控制文件写入被拒绝：命令失败并带上内核错误原因。
- pendingWrites 中不是本 DIDO 点：忽略，交给对应驱动消费。

## 7. DI 采集与变位

DI 采集建议由 `DioDriver` 周期扫描，第一阶段不依赖 GPIO 中断。

默认参数：

| 参数 | 建议值 | 说明 |
| --- | ---: | --- |
| `collect.defaultIntervalMs` | 200 | 本机 GPIO 读取开销低，可比串口更快 |
| `read.debounceMs` | 30 | 过滤机械抖动 |
| `reportOnChange` | `true` | DI 是典型事件点 |
| `fullUpload` | `true` | 平台打开实时页面时能看到当前状态 |

变位事件仍由 `EventEngine` 统一判断。`DioDriver` 只负责尽快把最新值写入共享内存，避免把告警和变位逻辑重新耦合回驱动。

## 8. 旧项目兼容导入

当前 Java 平台已经能识别舜通工程里的本地 DI/DO，并生成 `device_dio.json`，不会再错误生成 Modbus TCP 设备。

老项目 Excel / XML 中如果出现独立的 `DIDO` 配置页或等价配置，`rwtype` 必须按产品文档语义解释：

- `R` 表示干接点输入，生成 `digital_input`，只读，默认映射到硬件干接点 DI6-DI13。
- `W` 表示 DO 输出控制，生成 `digital_output`，可写，默认映射到硬件 DO1-DO8。
- 不要把 `DIDO` 页里的 `R` 当作普通协议读寄存器，也不要生成到 Modbus RTU/TCP 设备文件。

识别到的本地 IO 转换为 `device_dio.json`：

| 旧项目字段 | 新配置字段 |
| --- | --- |
| `index` | `points[].index`，保留原值或按 DIDO 段重排 |
| `单位` 中的 `KY...` | `pointCode` |
| `描述` | `name` / `desc` |
| `DO线圈` | `digital_output` |
| `DI离散输入寄存器` | `digital_input` |
| `读写` | `write.enable=true` |
| `只读` | `write.enable=false` |

现有样例：

| index | 类型 | 建议通道 | pointCode | 名称 |
| ---: | --- | ---: | --- | --- |
| 984 | DO | 1 | KY02201770 | 运行指示灯 |
| 985 | DO | 2 | KY02201771 | 并网脱扣继电器 |
| 986 | DO | 3 | KY02201772 | 故障指示灯 |
| 987 | DO | 4 | KY02201773 | 充电指示灯 |
| 988 | DO | 5 | KY02201774 | 放电指示灯 |
| 989 | DO | 6 | DO_6 | DO6 |
| 990 | DO | 7 | KY02301775 | HPLC 继电器给 1 断开 |
| 991 | DO | 8 | KY02300877 | 风机继电器 |
| 992 | DI | 9 | KY02200878 | 急停（按下闭点） |
| 993 | DI | 10 | KY02200879 | 浪涌保护器动作 |
| 994 | DI | 11 | KY02200880 | 水浸 |
| 995 | DI | 12 | KY02200881 | 消防动作 |
| 996 | DI | 13 | KY02200882 | 前门关闭 |
| 997 | DI | 7 | di_storage_breaker_closed | 储能断路器合位 |
| 998 | DI | 6 | KY02200884 | 并网断路器合位 |
| 999 | DI | 8 | di_load_breaker_closed | 负荷断路器合位 |

这里有一个必须在实现时明确的转换策略：

- 如果旧项目确认为“干接点”，推荐把旧 `DI0-DI7` 映射到硬件 `DI6-DI13`，因为说明书中 `DI6-DI18` 才是无源干接点。
- 如果现场线缆实际接在面板 `DI1-DI8`，则需要允许用户在 Java 导入页面选择“按面板 DI1-DI8 映射”，其中 `DI1-DI5` 会自动使用反相逻辑。
- `COMM202600104` 于 2026-07-23 将原 `DI1-DI5` 五路业务信号迁移到 `DI9-DI13`，保留 `index 992-996` 和测点编码不变；对应 GPIO 为 `137、138、143、128、139`。

## 9. Java 平台改造

### 9.1 多接口配置页面

在现有多接口配置中增加 `local_dio`：

- 接口类型：`本机 DI/DO`
- 驱动服务：`DioDriver`
- 默认设备：`LOCAL_DIO`
- 默认点位：18 个 DI + 8 个 DO，可按需删除或改名。

页面行为：

- DI/DO 点位表应使用类似 Excel 的表格编辑。
- DI 点默认只读。
- DO 点默认可读写。
- `pointCode/name/index/reportOnChange/fullUpload/isStore` 可编辑。
- GPIO 编号和 `activeHigh` 默认由硬件模板生成，普通用户不应误改；高级模式才允许修改。

### 9.2 老旧项目兼容导入

舜通导入结果中的“本地 DI/DO 干接点”生成规则：

- 默认生成文件：`runtime/devices/device_dio.json`。
- 默认 DI 映射策略：旧 `DI0-DI7` 映射到硬件 `DI6-DI13`。
- 同步更新 `runtime/apps/mqtt-service.json` 的 `deviceConfigFiles[]`。
- 生成并发布 OTA 时把 `device_dio.json` 一起打包。
- 后续如现场存在接到面板 `DI1-DI8` 的项目，再增加页面选择项。

## 10. 服务与部署

新增可执行文件和 systemd：

```text
/opt/modbus-gateway/bin/DioDriver
/etc/systemd/system/dio-driver@.service
```

推荐启动命令：

```bash
/opt/modbus-gateway/bin/DioDriver \
  --config /opt/modbus-gateway/config/runtime/devices/device_dio.json \
  --app-config /opt/modbus-gateway/config/runtime/apps/mqtt-service.json
```

`gateway-services.sh` 规则：

- `protocol.type=local_dio` 时启动 `dio-driver@device_dio.service`。
- 配置文件不存在或 `enabled=false` 时不启动。
- 停止 / OTA apply 时和其他驱动一样停止。

初始化包需要包含：

- `DioDriver`
- `dio-driver@.service`
- 可选的 `config/factory/runtime/devices/device_dio.json`
- 更新后的 `gateway-services.sh`
- 更新后的 `production-smoke-test.sh`

## 11. 测试方案

### 11.1 人工 GPIO 验证

在边端设备上先用说明书脚本验证：

```bash
rgpio.sh 134
wgpio.sh 231 0
wgpio.sh 231 1
```

验收：

- DI6 短接时 GPIO134 返回 `0`，平台逻辑值为 `1`。
- DI6 断开时 GPIO134 返回 `1`，平台逻辑值为 `0`。
- DO1 写 `0` 时继电器闭合。
- DO1 写 `1` 时继电器断开。

### 11.2 驱动单机验证

```bash
/opt/modbus-gateway/bin/DioDriver --config /opt/modbus-gateway/config/runtime/devices/device_dio.json --once
```

验收：

- 共享内存能看到 DI/DO latest。
- GPIO 读失败时 quality 为 `0`。
- 首次启动不会误触发 DO：DO 均为输出且处于启动值（默认非激活）。

### 11.3 写回验证

```bash
/opt/modbus-gateway/bin/pointctl write --index 420001 --value 1 --app-config /opt/modbus-gateway/config/runtime/apps/mqtt-service.json
/opt/modbus-gateway/bin/pointctl get --index 420001 --app-config /opt/modbus-gateway/config/runtime/apps/mqtt-service.json
```

验收：

- DO1 继电器闭合（现场目视或万用表确认：软件回执不能证明这一点）。
- `pointctl get` 返回值为 `1`（输出状态）。
- 未配置触点反馈时，MQTT 命令回执为 `success=true`、`outcome=succeeded`、`stage=writeback-submitted`、`verifyPassed=false`，消息含“未配置反馈，触点动作未确认”；配置了 `write.verifyIndex` 时为 `stage=writeback-succeeded`、`verifyPassed=true`。

### 11.4 事件验证

验收：

- DI 状态稳定不重复上传变位事件。
- DI 从 `0 -> 1` 只产生 1 条变位事件。
- DI 从 `1 -> 0` 只产生 1 条恢复事件。
- DO 写成功后状态变化可进入实时数据和变位事件。

## 12. 实施顺序

推荐按以下顺序推进：

1. C++ 增加 `local_dio` 配置模型字段，包括 `gpio`、`activeHigh`、`debounceMs`。
2. C++ 新增 `GpioPort` 抽象和 `SysfsGpioPort` 实现。
3. C++ 新增 `DioDriver`，支持 `--once`、周期采集、DO pendingWrites 写回。
4. `gateway-services.sh`、systemd、初始化包和冒烟脚本接入 `DioDriver`。
5. Java 多接口配置增加“本机 DI/DO”。
6. Java 舜通导入把本地 IO 生成 `device_dio.json`。
7. 更新运行样例和消息样例。
8. 在边端设备上做 GPIO 实测、共享内存验证和 MQTT 联调。

## 13. 当前建议

本需求应按独立 `DioDriver + local_dio` 落地，不建议复用 `ModbusRtu` 或 `modbus_tcp`。

原因：

- DIDO 访问的是本机 GPIO，不存在 Modbus 从站、寄存器和 TCP 连接。
- DO 拉低闭合、DI 湿接点反相是硬件语义，放在 Modbus 模型里会持续误导配置和排障。
- 独立驱动后，旧项目兼容导入、Java 页面、OTA 和共享内存都能保持清晰边界。
