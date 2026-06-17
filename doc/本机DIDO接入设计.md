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

驱动实现建议直接操作 sysfs 文件，减少对 shell 脚本的依赖。

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
| `read.debounceMs` | 防抖时间，避免干接点抖动导致重复变位事件 |

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
| `write.verifyByRead=true` | 写后读取 GPIO 电平确认状态 |

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
  -> 读回 GPIO 0，转换回点值 1
  -> 更新 latest
  -> 发布命令执行结果
```

错误处理：

- GPIO 不存在：点质量置为 `0`，状态消息上报 `gpio not found`。
- 没有权限：点质量置为 `0`，状态消息上报 `permission denied`。
- 写入值不在 `0/1`：拒绝命令，不触碰 GPIO。
- 写后读回不一致：命令失败，不伪造成功状态。
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
| 992 | DI | 1 或 6 | KY02200878 | 急停反馈-0 正常 |
| 993 | DI | 2 或 7 | KY02200879 | 浪涌保护器-1 动作 |
| 994 | DI | 3 或 8 | KY02200880 | 水浸反馈-1 动作 |
| 995 | DI | 4 或 9 | KY02200881 | 气溶胶-1 动作 |
| 996 | DI | 5 或 10 | KY02200882 | 前门反馈-1 开门 |
| 997 | DI | 6 或 11 | KY02200883 | 后门反馈-1 开门 |
| 998 | DI | 7 或 12 | KY02200884 | 并网断路器-1 合位 |
| 999 | DI | 8 或 13 | DI_8 | DI8 |

这里有一个必须在实现时明确的转换策略：

- 如果旧项目确认为“干接点”，推荐把旧 `DI0-DI7` 映射到硬件 `DI6-DI13`，因为说明书中 `DI6-DI18` 才是无源干接点。
- 如果现场线缆实际接在面板 `DI1-DI8`，则需要允许用户在 Java 导入页面选择“按面板 DI1-DI8 映射”，其中 `DI1-DI5` 会自动使用反相逻辑。

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
- 首次启动不会误触发 DO。

### 11.3 写回验证

```bash
/opt/modbus-gateway/bin/pointctl write --index 420001 --value 1 --app-config /opt/modbus-gateway/config/runtime/apps/mqtt-service.json
/opt/modbus-gateway/bin/pointctl get --index 420001 --app-config /opt/modbus-gateway/config/runtime/apps/mqtt-service.json
```

验收：

- DO1 继电器闭合。
- `pointctl get` 返回值为 `1`。
- MQTT 命令回执为成功。

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
