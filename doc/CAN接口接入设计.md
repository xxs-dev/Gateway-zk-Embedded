# CAN 接口接入设计

## 目标

边端设备具备 4 路 CAN 接口，Linux 侧以 SocketCAN 网络接口形式暴露为 `can0`、`can1`、`can2`、`can3`。CAN 接入必须作为独立协议驱动实现，不挂在 Modbus、DLT645 或 DI/DO 下面。

本设计先落地原始 CAN 帧信号解析和写入，后续再扩展 CANopen、J1939、DBC 文件导入。

目标：

- 边端新增独立 `CanDriver`，每个 CAN 口一个驱动实例。
- CAN 驱动从 SocketCAN 收帧，按 JSON 点表规则解码后写入统一共享内存 `gateway_point_store`。
- MQTT、事件引擎、告警、变位、计算引擎、系统监测继续从统一共享内存取数，不新增 CAN 专属上报格式。
- 平台下发写命令时，`CanDriver` 从共享内存 pending write 队列取命令，按 JSON 写入规则编码 CAN 帧并发送。
- Java 平台支持可视化配置 CAN 接口、逻辑设备、CAN 点表、Excel 导入导出、生成配置、OTA 下发。

## 当前实现状态

已落地第一阶段原始 CAN 帧接入：

- C++ 已新增 `CanDriver`、`CanDriverService`、`CanSignalCodec` 和 `can_signal_codec_test`。
- `protocol.type=can_socketcan` 时，驱动使用 SocketCAN 收发 `can0` 到 `can3`。
- `read.can` 支持按 `frameId`、标准帧/扩展帧、`byteOffset`、`bitOffset`、`bitLength`、大小端和倍率解析。
- `write.can` 支持从共享内存 pending write 队列取写命令，并编码为 CAN 帧发送。
- `gateway-services.sh` 已支持按 `deviceConfigFiles[]` 自动启动 `can-driver@device_canX.service`，未配置的 CAN 驱动不会启动。
- 初始化安装包脚本和工厂包脚本已包含 `CanDriver` 与 `can-driver@.service`。
- Java 多接口批量配置页面已支持 `can_socketcan`，可配置 CAN 接口参数、逻辑设备、在线帧和 CAN 点表。
- 测点 Excel 模板已补充 CAN 读写字段；导入时如填写 `readCanFrameId` 或 `writeCanFrameId`，会生成 `read.can` 或 `write.can`。
- 示例文件已新增 `config/examples/device_can0_example.json`。

已验证：

- Java：`mvn -q -DskipTests compile` 通过。
- 前端：`node --check src/main/resources/static/app.js` 通过。
- C++：交叉编译机执行 `cmake --build build-aarch64 -j2` 通过。
- 边端：`can_signal_codec_test` 和 `CanDriver --once --config /tmp/device_can0_example.json` 通过。

## 当前硬件状态

当前边端系统已经识别 4 路 CAN：

```text
can0 DOWN
can1 DOWN
can2 DOWN
can3 DOWN
```

驱动和内核能力：

```text
driver: sunxican
clock: 40000000
CAN device driver interface
sunxican device registered
can: raw protocol
can: broadcast manager protocol
can: netlink gateway
```

系统已有调试工具：

```text
/usr/bin/cansend
/usr/bin/candump
/usr/sbin/ip
```

结论：硬件和系统已具备 SocketCAN 基础能力，可以直接按 `can0` 到 `can3` 开发和联调。

## 方案选择

### 方案一：CAN 配置复用 Modbus 点表

不推荐。

优点是平台页面改动较少，但 CAN 没有寄存器地址、功能码、从站地址，强行复用 Modbus 字段会继续制造误解，后续 CAN 写入、帧过滤、位域解析都会变得不清晰。

### 方案二：CAN 独立协议，但点表仍使用通用 `read` / `write`

推荐。

使用 `protocol.type=can_socketcan` 标识协议，接口参数放在 `protocol.can`，点位仍保留现有通用字段，例如 `index`、`meterCode`、`pointCode`、`fullUpload`、`reportOnChange`、`read`、`write`、`alarms`。CAN 特有的帧 ID、bit 位、字节序、倍率等放到 `read.can` 和 `write.can`。

这样既不误导为 Modbus，又能复用现有共享内存、MQTT、事件、写队列和平台点表能力。

### 方案三：直接导入 DBC 并生成全部点位

后续扩展。

DBC 对车辆和标准 CAN 项目有价值，但当前用户需求是先可视化配置 CAN 点表。第一阶段如果强依赖 DBC，会增加解析器、信号命名、消息周期、枚举映射等复杂度，不利于快速验证硬件链路。

本项目第一阶段采用方案二。

## 运行模型

每个 CAN 口运行一个独立驱动实例：

```text
can-driver@device_can0.service
can-driver@device_can1.service
can-driver@device_can2.service
can-driver@device_can3.service
```

每个实例加载一个设备配置文件：

```text
/opt/modbus-gateway/config/runtime/devices/device_can0.json
/opt/modbus-gateway/config/runtime/devices/device_can1.json
/opt/modbus-gateway/config/runtime/devices/device_can2.json
/opt/modbus-gateway/config/runtime/devices/device_can3.json
```

启动规则：

- `gateway-services.sh` 只启动 `appConfig.deviceConfigFiles[]` 中引用的 CAN 配置。
- 配置文件 `protocol.type=can_socketcan` 且 `enabled` 未显式为 `false` 时，启动对应 `can-driver@<配置文件名>.service`。
- 如果某个 CAN 口没有配置文件或没有被 `deviceConfigFiles[]` 引用，则开机不启动该 CAN 驱动。

数据链路：

```text
CAN 总线
  -> SocketCAN canX
  -> CanDriver
  -> gateway_point_store
  -> EventEngine / MqttDriver / SystemMonitor / ComputeEngine
```

写入链路：

```text
平台 MQTT 写命令 / pointctl
  -> gateway_point_store pending write
  -> CanDriver
  -> SocketCAN 发送 CAN 帧
  -> CAN 设备
```

## 配置文件命名

CAN 设备配置文件固定按接口命名：

| 接口 | 配置文件 |
| --- | --- |
| `can0` | `runtime/devices/device_can0.json` |
| `can1` | `runtime/devices/device_can1.json` |
| `can2` | `runtime/devices/device_can2.json` |
| `can3` | `runtime/devices/device_can3.json` |

原因：

- 和 `device_slave_ttySP1.json`、`device_dio.json` 的命名习惯一致。
- OTA 打包时可以明确看出哪个文件对应哪个物理接口。
- Java 页面按接口生成和覆盖时不需要额外映射。

## JSON 结构总览

CAN 配置文件使用现有设备配置体系：

```json
{
  "schemaVersion": "1.0.0",
  "enabled": true,
  "protocol": {},
  "collect": {},
  "memoryStore": {},
  "meters": []
}
```

字段说明：

| 字段 | 说明 |
| --- | --- |
| `schemaVersion` | 配置版本，当前使用 `1.0.0` |
| `enabled` | 是否启用该配置，默认 `true` |
| `protocol.type` | 固定为 `can_socketcan` |
| `protocol.can` | CAN 接口参数 |
| `collect` | 驱动轮询、在线检查、写队列扫描参数 |
| `memoryStore` | 统一共享内存配置 |
| `meters` | CAN 总线上的逻辑设备分组 |

### `protocol.can`

```json
{
  "type": "can_socketcan",
  "can": {
    "interfaceName": "can0",
    "interfaceCode": "CAN_1",
    "bitrate": 500000,
    "samplePoint": 0.875,
    "restartMs": 100,
    "listenOnly": false,
    "loopback": false,
    "fdEnabled": false,
    "dataBitrate": 2000000,
    "manageInterface": true,
    "rxQueueSize": 4096,
    "txQueueSize": 1024
  }
}
```

字段说明：

| 字段 | 说明 |
| --- | --- |
| `interfaceName` | Linux CAN 网卡名，取值 `can0`、`can1`、`can2`、`can3` |
| `interfaceCode` | 平台接口编码，建议 `CAN_1` 到 `CAN_4` |
| `bitrate` | 仲裁段波特率，例如 `125000`、`250000`、`500000`、`1000000` |
| `samplePoint` | 采样点，可选；不填时使用系统默认 |
| `restartMs` | bus-off 后自动恢复时间，建议 `100` |
| `listenOnly` | 只监听不发送，联调抓包时可开启 |
| `loopback` | 本机环回测试，生产默认 `false` |
| `fdEnabled` | 是否启用 CAN-FD，第一阶段默认 `false` |
| `dataBitrate` | CAN-FD 数据段波特率，`fdEnabled=true` 时生效 |
| `manageInterface` | 驱动启动时是否自动配置并拉起接口 |
| `rxQueueSize` | 驱动内部收帧队列上限 |
| `txQueueSize` | 驱动内部发帧队列上限 |

第一阶段默认使用经典 CAN，`fdEnabled=false`。

### `collect`

```json
{
  "defaultIntervalMs": 100,
  "batchOptimize": false,
  "maxBatchRegisters": 1,
  "writebackIntervalMs": 50,
  "interfaceCheckIntervalMs": 1000
}
```

字段说明：

| 字段 | 说明 |
| --- | --- |
| `defaultIntervalMs` | 主循环默认休眠时间，CAN 收帧主要由 socket 触发 |
| `writebackIntervalMs` | 扫描 pending write 队列的周期 |
| `interfaceCheckIntervalMs` | 检查接口 up/down、bus-off 状态的周期 |
| `batchOptimize` | 保留字段，CAN 不做寄存器连续合并，固定 `false` |
| `maxBatchRegisters` | 保留字段，CAN 固定 `1` |

### `memoryStore`

CAN 默认写入统一共享内存：

```json
{
  "enabled": true,
  "backend": "memory",
  "keepHistory": 100,
  "defaultTtlMs": 600000,
  "indexBy": ["machineCode", "meterCode", "pointCode"],
  "sharedMemoryName": "gateway_point_store",
  "maxLatestPoints": 100000,
  "maxPendingWrites": 4096,
  "maxPersistentSamples": 20000,
  "sqlitePath": "/opt/modbus-gateway/data/point_samples_can0.db",
  "sqliteLibraryPath": "",
  "persistFlushIntervalMs": 60000,
  "writebackIntervalMs": 50,
  "writebackBatchSize": 128
}
```

原则：

- 所有协议驱动默认写入同一个 `gateway_point_store`。
- 每路 CAN 的历史 SQLite 文件独立命名，避免单文件过大。
- CAN 事件、告警、变位仍由 `EventEngine` 判断，不在 `CanDriver` 内直接做 MQTT 上报。

## 逻辑设备模型

CAN 没有 Modbus 从站号，但现场仍需要按设备分组展示和过滤点位。因此 `meters[]` 表示 CAN 总线上的逻辑设备。

```json
{
  "meterCode": "CAN0_DEV_0001",
  "deviceName": "CAN0 设备 1",
  "enabled": true,
  "onlineTimeoutMs": 5000,
  "onlineFrameIds": ["0x18FF50E5", "0x123"],
  "points": []
}
```

字段说明：

| 字段 | 说明 |
| --- | --- |
| `meterCode` | 逻辑设备编码，沿用平台现有 `meterCode` |
| `deviceName` | 设备名称 |
| `enabled` | 是否启用该逻辑设备 |
| `onlineTimeoutMs` | 超过该时间未收到设备关键帧，则设备离线 |
| `onlineFrameIds` | 用于判定设备在线的 CAN 帧 ID |
| `points` | 设备点表 |

说明：

- 一个 CAN 帧只能归属到一个逻辑设备，Java 端需要校验重复归属。
- 如果现场没有明确设备边界，也可以只建一个逻辑设备，例如 `CAN0_BUS`。
- 平台实时监测按 `machineCode + meterCode` 分类展示，CAN 点位不会挤到其他协议设备下面。

## 点位模型

CAN 点位保留通用字段，CAN 特有字段放入 `read.can` 和 `write.can`。

### 读点位

```json
{
  "index": 310101,
  "pointCode": "engine_speed",
  "name": "发动机转速",
  "desc": "CAN0 设备 1 发动机转速",
  "category": "telemetry",
  "address": 0,
  "enabled": true,
  "isStore": true,
  "fullUpload": true,
  "reportOnChange": false,
  "persistIntervalSec": 60,
  "tags": ["can", "telemetry"],
  "read": {
    "enable": true,
    "function": 0,
    "length": 1,
    "dataType": "uint16",
    "scale": 0.125,
    "offset": 0,
    "byteOrder": "AB",
    "signed": false,
    "unit": "rpm",
    "intervalMs": 100,
    "can": {
      "frameId": "0x18FF50E5",
      "extended": true,
      "dlc": 8,
      "byteOffset": 0,
      "bitOffset": 0,
      "bitLength": 16,
      "bitOrder": "lsb0",
      "endian": "little",
      "receiveTimeoutMs": 5000,
      "invalidRawValues": []
    },
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
  "valueMap": {}
}
```

### 写点位

```json
{
  "index": 310201,
  "pointCode": "set_output_1",
  "name": "设置输出 1",
  "category": "control",
  "enabled": true,
  "isStore": false,
  "fullUpload": false,
  "reportOnChange": true,
  "read": {
    "enable": false,
    "dataType": "uint8",
    "unit": ""
  },
  "write": {
    "enable": true,
    "function": 0,
    "length": 1,
    "dataType": "uint8",
    "scale": 1,
    "offset": 0,
    "byteOrder": "AB",
    "minValue": 0,
    "maxValue": 1,
    "step": 1,
    "allowedValues": [0, 1],
    "verifyAfterWrite": false,
    "can": {
      "mode": "signal",
      "frameId": "0x321",
      "extended": false,
      "dlc": 8,
      "payloadTemplate": "0000000000000000",
      "mergePolicy": "template",
      "byteOffset": 0,
      "bitOffset": 0,
      "bitLength": 1,
      "bitOrder": "lsb0",
      "endian": "little",
      "sendRepeat": 1,
      "sendIntervalMs": 0
    }
  },
  "alarms": [],
  "valueMap": {
    "0": "关闭",
    "1": "打开"
  }
}
```

### `read.can` 字段

| 字段 | 说明 |
| --- | --- |
| `frameId` | CAN 帧 ID，支持 `"0x123"` 或 `"0x18FF50E5"` |
| `extended` | 是否扩展帧，`false` 表示 11 位标准帧，`true` 表示 29 位扩展帧 |
| `dlc` | 期望数据长度，经典 CAN 为 `0` 到 `8` |
| `byteOffset` | 信号起始字节，`0` 起始 |
| `bitOffset` | 信号起始 bit，`0` 起始 |
| `bitLength` | 信号 bit 长度，建议 `1` 到 `64` |
| `bitOrder` | bit 编号方式，第一阶段固定 `lsb0` |
| `endian` | 字节序，支持 `little`、`big` |
| `receiveTimeoutMs` | 超过该时间未收到对应帧，点位置 stale |
| `invalidRawValues` | 原始值黑名单，例如设备用 `0xFFFF` 表示无效 |

### `write.can` 字段

| 字段 | 说明 |
| --- | --- |
| `mode` | 写入模式，第一阶段支持 `signal`，后续扩展 `raw_frame` |
| `frameId` | 发送帧 ID |
| `extended` | 是否扩展帧 |
| `dlc` | 发送数据长度 |
| `payloadTemplate` | 发送帧基础数据，十六进制字符串 |
| `mergePolicy` | 位域合并策略，支持 `template`、`latestFrame` |
| `byteOffset` | 写入信号起始字节 |
| `bitOffset` | 写入信号起始 bit |
| `bitLength` | 写入信号 bit 长度 |
| `bitOrder` | bit 编号方式，第一阶段固定 `lsb0` |
| `endian` | 字节序 |
| `sendRepeat` | 每次写入重复发送次数 |
| `sendIntervalMs` | 重复发送间隔 |

`mergePolicy` 说明：

| 策略 | 行为 |
| --- | --- |
| `template` | 使用 `payloadTemplate` 作为基础帧，只替换当前信号对应 bit，推荐第一阶段默认使用 |
| `latestFrame` | 使用最近一次收到的同 ID 帧作为基础帧，再替换当前信号；如果没有缓存帧，则回退到 `payloadTemplate` |

## 数据类型

第一阶段建议支持以下 `read.dataType` / `write.dataType`：

| 类型 | 说明 |
| --- | --- |
| `bool` | 1 bit 开关量 |
| `uint8` / `int8` | 8 位整数 |
| `uint16` / `int16` | 16 位整数 |
| `uint32` / `int32` | 32 位整数 |
| `float32` | IEEE754 单精度浮点 |
| `device_online` | 设备在线状态 |
| `interface_online` | CAN 接口在线状态 |

解码公式：

```text
value = raw * scale + offset
```

编码公式：

```text
raw = round((value - offset) / scale)
```

## 在线状态点

CAN 需要两类在线状态。

### 接口在线

接口在线点表示 `canX` 是否可用。

建议每个 CAN 配置自动生成一个接口在线点：

```json
{
  "index": 310000,
  "pointCode": "interface_online",
  "name": "CAN0 接口在线",
  "category": "online",
  "enabled": true,
  "isStore": false,
  "fullUpload": true,
  "reportOnChange": true,
  "read": {
    "enable": true,
    "function": 0,
    "length": 0,
    "dataType": "interface_online",
    "unit": "",
    "intervalMs": 1000,
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
  "valueMap": {
    "0": "离线",
    "1": "在线"
  }
}
```

判定规则：

- `canX` 已 up 且未 bus-off：值为 `1`。
- 接口 down、bus-off、socket 打开失败：值为 `0`。

### 设备在线

设备在线点表示某个逻辑设备是否还在发送关键帧。

建议每个 `meter` 自动生成一个设备在线点：

```json
{
  "index": 310100,
  "pointCode": "device_online",
  "name": "CAN0 设备 1 在线",
  "category": "online",
  "enabled": true,
  "isStore": false,
  "fullUpload": true,
  "reportOnChange": true,
  "read": {
    "enable": true,
    "function": 0,
    "length": 0,
    "dataType": "device_online",
    "unit": "",
    "intervalMs": 1000,
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
  "valueMap": {
    "0": "离线",
    "1": "在线"
  }
}
```

判定规则：

- 该设备 `onlineFrameIds[]` 中任意帧在 `onlineTimeoutMs` 内出现，值为 `1`。
- 超过 `onlineTimeoutMs` 未出现，值为 `0`。
- 如果 `onlineFrameIds[]` 为空，则使用该设备所有读点位涉及的 `frameId` 作为在线帧集合。

## index 规划

现有 DI/DO 已使用 `410001` 起的点位，CAN 不能继续使用 `41000` 到 `44999` 这类范围。

CAN 推荐使用硬件接口规划中的 6 位范围：

| 接口 | 范围 | 保留点 |
| --- | --- | --- |
| `can0` / `CAN_1` | `310000` 到 `319999` | `310000` 作为接口在线 |
| `can1` / `CAN_2` | `320000` 到 `329999` | `320000` 作为接口在线 |
| `can2` / `CAN_3` | `330000` 到 `339999` | `330000` 作为接口在线 |
| `can3` / `CAN_4` | `340000` 到 `349999` | `340000` 作为接口在线 |

逻辑设备点位建议按设备分段：

| 示例 | 范围 |
| --- | --- |
| `CAN0_DEV_0001` | `310100` 到 `310999` |
| `CAN0_DEV_0002` | `311000` 到 `311999` |
| `CAN0_DEV_0003` | `312000` 到 `312999` |

Java 端必须校验 index 全局唯一，并避免和 Modbus、DLT645、DI/DO、计算点位冲突。

## 完整配置样例

```json
{
  "schemaVersion": "1.0.0",
  "enabled": true,
  "protocol": {
    "type": "can_socketcan",
    "can": {
      "interfaceName": "can0",
      "interfaceCode": "CAN_1",
      "bitrate": 500000,
      "samplePoint": 0.875,
      "restartMs": 100,
      "listenOnly": false,
      "loopback": false,
      "fdEnabled": false,
      "dataBitrate": 2000000,
      "manageInterface": true,
      "rxQueueSize": 4096,
      "txQueueSize": 1024
    }
  },
  "collect": {
    "defaultIntervalMs": 100,
    "batchOptimize": false,
    "maxBatchRegisters": 1,
    "writebackIntervalMs": 50,
    "interfaceCheckIntervalMs": 1000
  },
  "memoryStore": {
    "enabled": true,
    "backend": "memory",
    "keepHistory": 100,
    "defaultTtlMs": 600000,
    "indexBy": ["machineCode", "meterCode", "pointCode"],
    "sharedMemoryName": "gateway_point_store",
    "maxLatestPoints": 100000,
    "maxPendingWrites": 4096,
    "maxPersistentSamples": 20000,
    "sqlitePath": "/opt/modbus-gateway/data/point_samples_can0.db",
    "sqliteLibraryPath": "",
    "persistFlushIntervalMs": 60000,
    "writebackIntervalMs": 50,
    "writebackBatchSize": 128
  },
  "meters": [
    {
      "meterCode": "CAN0_DEV_0001",
      "deviceName": "CAN0 设备 1",
      "enabled": true,
      "onlineTimeoutMs": 5000,
      "onlineFrameIds": ["0x18FF50E5"],
      "points": [
        {
          "index": 310100,
          "pointCode": "device_online",
          "name": "CAN0 设备 1 在线",
          "category": "online",
          "enabled": true,
          "isStore": false,
          "fullUpload": true,
          "reportOnChange": true,
          "read": {
            "enable": true,
            "function": 0,
            "length": 0,
            "dataType": "device_online",
            "unit": "",
            "intervalMs": 1000,
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
          "valueMap": {
            "0": "离线",
            "1": "在线"
          }
        },
        {
          "index": 310101,
          "pointCode": "engine_speed",
          "name": "发动机转速",
          "category": "telemetry",
          "enabled": true,
          "isStore": true,
          "fullUpload": true,
          "reportOnChange": false,
          "persistIntervalSec": 60,
          "read": {
            "enable": true,
            "function": 0,
            "length": 1,
            "dataType": "uint16",
            "scale": 0.125,
            "offset": 0,
            "byteOrder": "AB",
            "signed": false,
            "unit": "rpm",
            "intervalMs": 100,
            "can": {
              "frameId": "0x18FF50E5",
              "extended": true,
              "dlc": 8,
              "byteOffset": 0,
              "bitOffset": 0,
              "bitLength": 16,
              "bitOrder": "lsb0",
              "endian": "little",
              "receiveTimeoutMs": 5000,
              "invalidRawValues": []
            },
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
          "alarms": [
            {
              "type": "high",
              "threshold": 3000,
              "reportRecovery": true
            }
          ],
          "valueMap": {}
        },
        {
          "index": 310201,
          "pointCode": "set_output_1",
          "name": "设置输出 1",
          "category": "control",
          "enabled": true,
          "isStore": false,
          "fullUpload": false,
          "reportOnChange": true,
          "read": {
            "enable": false,
            "dataType": "uint8",
            "unit": ""
          },
          "write": {
            "enable": true,
            "function": 0,
            "length": 1,
            "dataType": "uint8",
            "scale": 1,
            "offset": 0,
            "byteOrder": "AB",
            "minValue": 0,
            "maxValue": 1,
            "step": 1,
            "allowedValues": [0, 1],
            "verifyAfterWrite": false,
            "can": {
              "mode": "signal",
              "frameId": "0x321",
              "extended": false,
              "dlc": 8,
              "payloadTemplate": "0000000000000000",
              "mergePolicy": "template",
              "byteOffset": 0,
              "bitOffset": 0,
              "bitLength": 1,
              "bitOrder": "lsb0",
              "endian": "little",
              "sendRepeat": 1,
              "sendIntervalMs": 0
            }
          },
          "alarms": [],
          "valueMap": {
            "0": "关闭",
            "1": "打开"
          }
        }
      ]
    }
  ]
}
```

## App 配置关联

`mqtt-service.json` 和 `monitor-service.json` 的 `deviceConfigFiles[]` 需要引用 CAN 配置文件：

```json
{
  "deviceConfigFiles": [
    "/opt/modbus-gateway/config/runtime/devices/device_slave_ttySP1.json",
    "/opt/modbus-gateway/config/runtime/devices/device_dio.json",
    "/opt/modbus-gateway/config/runtime/devices/device_can0.json"
  ]
}
```

OTA 生成配置包时必须把引用关系一起打包：

- `runtime/devices/device_can0.json`
- `runtime/apps/mqtt-service.json`
- `runtime/apps/monitor-service.json`
- `runtime/device_identity.json`

## Java 平台可视化配置设计

### 页面入口

在现有「多串口批量生成」基础上扩展为「多接口批量配置」。协议选项新增：

```text
modbus_rtu
modbus_tcp
dlt645_2007
local_dio
can_socketcan
```

当接口协议选择 `can_socketcan` 时：

- 隐藏串口参数 `baudRate`、`dataBits`、`stopBits`、`parity`。
- 显示 CAN 参数 `interfaceName`、`bitrate`、`samplePoint`、`restartMs`、`listenOnly`、`loopback`、`fdEnabled`、`dataBitrate`。
- 设备列表标题从「从站」改为「逻辑设备」。
- 点位表格切换为 CAN 专用列。

### 接口配置区

CAN 接口配置字段：

| UI 字段 | JSON 字段 | 说明 |
| --- | --- | --- |
| 接口 | `protocol.can.interfaceName` | 下拉 `can0` 到 `can3` |
| 接口编码 | `protocol.can.interfaceCode` | 自动映射 `CAN_1` 到 `CAN_4` |
| 波特率 | `protocol.can.bitrate` | 下拉并允许输入 |
| 采样点 | `protocol.can.samplePoint` | 可选，默认 `0.875` |
| 自动恢复 | `protocol.can.restartMs` | 默认 `100` |
| 只监听 | `protocol.can.listenOnly` | 抓包时使用 |
| 环回 | `protocol.can.loopback` | 本机测试使用 |
| CAN-FD | `protocol.can.fdEnabled` | 第一阶段默认关闭 |
| 数据段波特率 | `protocol.can.dataBitrate` | CAN-FD 开启时显示 |

校验规则：

- 同一个 `machineCode` 下不能重复配置同一个 `interfaceName`。
- 同一个 CAN 接口只能绑定 `can_socketcan` 协议。
- `bitrate` 必须大于 `0`。
- `fdEnabled=false` 时隐藏并忽略 `dataBitrate`。

### 逻辑设备配置区

逻辑设备字段：

| UI 字段 | JSON 字段 | 说明 |
| --- | --- | --- |
| 设备编码 | `meterCode` | 例如 `CAN0_DEV_0001` |
| 设备名称 | `deviceName` | 页面展示名称 |
| 启用 | `enabled` | 是否生成和运行 |
| 在线超时 | `onlineTimeoutMs` | 默认 `5000` |
| 在线帧 ID | `onlineFrameIds[]` | 多个 ID 用逗号或表格维护 |

新增逻辑设备时自动生成：

- `meterCode`：`CAN0_DEV_0001`、`CAN0_DEV_0002`
- `device_online` 点
- 设备点位 index 起始段

### CAN 点表表格

点表使用类似 Excel 的可编辑表格，不使用 Modbus 弹窗。

建议列：

| 列名 | JSON 字段 |
| --- | --- |
| index | `index` |
| 点位编码 | `pointCode` |
| 点位名称 | `name` |
| 类别 | `category` |
| 数据类型 | `read.dataType` / `write.dataType` |
| 单位 | `read.unit` |
| 帧 ID | `read.can.frameId` / `write.can.frameId` |
| 扩展帧 | `read.can.extended` / `write.can.extended` |
| DLC | `read.can.dlc` / `write.can.dlc` |
| 起始字节 | `read.can.byteOffset` / `write.can.byteOffset` |
| 起始 bit | `read.can.bitOffset` / `write.can.bitOffset` |
| bit 长度 | `read.can.bitLength` / `write.can.bitLength` |
| 字节序 | `read.can.endian` / `write.can.endian` |
| 倍率 | `read.scale` / `write.scale` |
| 偏移 | `read.offset` / `write.offset` |
| 全量上传 | `fullUpload` |
| 变位上传 | `reportOnChange` |
| 可写 | `write.enable` |
| 写入模板 | `write.can.payloadTemplate` |
| 合并策略 | `write.can.mergePolicy` |

交互规则：

- `category=online` 时锁定 CAN 信号列。
- `write.enable=false` 时隐藏或禁用写入模板、合并策略。
- `dataType=bool` 时默认 `bitLength=1`。
- `float32` 默认 `bitLength=32`。
- 标准帧 `frameId` 最大 `0x7FF`。
- 扩展帧 `frameId` 最大 `0x1FFFFFFF`。
- `byteOffset + bitLength` 不得超过 `dlc * 8`。

### Excel 导入导出

CAN 点表模板第二行保留中文说明，导入时跳过第二行，延续现有测点 Excel 规则。

模板列：

```text
index
meterCode
pointCode
name
category
dataType
unit
frameId
extended
dlc
byteOffset
bitOffset
bitLength
endian
scale
offset
fullUpload
reportOnChange
writeEnable
payloadTemplate
mergePolicy
onlineFrame
receiveTimeoutMs
```

导入行为：

- 根据 `meterCode` 自动创建逻辑设备。
- 如果同一个 `meterCode` 已存在，则追加或覆盖点位。
- 如果 `onlineFrame=true`，把该行 `frameId` 加入对应设备 `onlineFrameIds[]`。
- 如果缺少 `device_online` 点，导入完成后自动补一个。
- 导入完成后显示解析结果表，用户确认后再写入配置区。

导出行为：

- 可下载当前 CAN 点表 Excel。
- 可下载最终 `device_canX.json`。
- 生成并发布 OTA 时，把 CAN 配置加入配置包。

### 配置生成规则

Java 生成 CAN 配置时：

- 文件名固定为 `device_can0.json`、`device_can1.json`、`device_can2.json`、`device_can3.json`。
- `protocol.type` 固定为 `can_socketcan`。
- `protocol.can.interfaceName` 来自页面接口选择。
- `memoryStore.sharedMemoryName` 默认 `gateway_point_store`。
- `sqlitePath` 按接口生成，例如 `/opt/modbus-gateway/data/point_samples_can0.db`。
- 自动把生成文件加入 `mqtt-service.json` 和 `monitor-service.json` 的 `deviceConfigFiles[]`。
- 如果用户取消某路 CAN 配置，则从 `deviceConfigFiles[]` 移除对应文件，开机不启动该驱动。

## 边端实现边界

第一阶段 `CanDriver` 只实现原始 CAN：

- 自动配置并拉起 `canX`。
- 打开 `AF_CAN / SOCK_RAW / CAN_RAW` socket。
- 按 `frameId + extended` 做帧过滤。
- 按 `read.can` 解码 bit 位并写共享内存。
- 按设备 `onlineFrameIds[]` 维护 `device_online`。
- 按接口状态维护 `interface_online`。
- 扫描 pending write，按 `write.can` 编码并发送帧。
- bus-off 或接口 down 时发布状态事件，并把接口在线点置 `0`。

第一阶段不实现：

- CANopen 对象字典。
- J1939 PGN / SPN 自动解析。
- DBC 文件解析。
- 多包传输协议。
- CAN-FD 真实收发，配置字段先保留。

## systemd 模板

建议新增：

```text
/etc/systemd/system/can-driver@.service
```

服务命令：

```text
/opt/modbus-gateway/bin/CanDriver --config /opt/modbus-gateway/config/runtime/devices/%i.json
```

如果实例名为 `device_can0`：

```sh
systemctl start can-driver@device_can0.service
systemctl status can-driver@device_can0.service
```

## 调试流程

接口拉起：

```sh
ip link set can0 down
ip link set can0 type can bitrate 500000 restart-ms 100
ip link set can0 up
ip -details link show can0
```

收包：

```sh
candump can0
```

发包：

```sh
cansend can0 123#1122334455667788
```

驱动运行：

```sh
/opt/modbus-gateway/bin/CanDriver --config /opt/modbus-gateway/config/runtime/devices/device_can0.json
```

共享内存检查：

```sh
/opt/modbus-gateway/bin/pointctl get --shm gateway_point_store 310101
/opt/modbus-gateway/bin/pointctl dump --shm gateway_point_store
```

## 验收标准

边端验收：

- `can0` 到 `can3` 可按配置拉起。
- 收到匹配帧后，对应点位在 `gateway_point_store` 中刷新。
- 未收到设备关键帧超过 `onlineTimeoutMs` 后，`device_online=0`。
- CAN 接口 down 或 bus-off 后，`interface_online=0`。
- 平台写点位后，`CanDriver` 能发送正确 CAN 帧。
- 没有配置某路 CAN 时，对应 `can-driver@...` 不启动。

Java 验收：

- 可在页面选择 `can_socketcan`。
- CAN 页面不出现 Modbus 从站、寄存器、功能码弹窗。
- 可视化表格能编辑 CAN 点表。
- 可导入 Excel CAN 点表并显示解析结果。
- 可下载 `device_canX.json`。
- 生成并发布 OTA 后，边端配置文件更新，服务按引用关系启动。

联调验收：

- Java 系统监测和实时数据页面能按 `machineCode + meterCode` 查看 CAN 点位。
- MQTT 全量快照只包含 `fullUpload=true` 的 CAN 点。
- `reportOnChange=true` 的 CAN 点产生变位事件。
- 告警规则仍由 `EventEngine` 判断，不依赖 `MqttDriver` 扫描逻辑。

## 后续扩展

后续可继续增加：

- DBC 文件导入，自动生成 `read.can` 信号。
- J1939 PGN / SPN 模板。
- CANopen SDO / PDO。
- CAN-FD 真实收发。
- 原始 CAN 帧透传诊断命令。
- CAN 总线负载率统计。
- bus-off 自动恢复计数和平台告警。
