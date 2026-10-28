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
- `read.can` 支持按 `frameId`、标准帧/扩展帧、`byteOffset`、`bitOffset`、`bitLength`、Intel/Motorola 字节序和倍率解析，位置定义见“信号位置定义”。
- `write.can` 支持从共享内存 pending write 队列取写命令，按发送帧镜像编码并发送（见“写入与帧镜像”）。
- 加载配置时校验 CAN 点表（帧 ID 写法与范围、数据类型、字节序、位范围、同帧写入定义一致性等），错误定义直接报错并点名，不再在运行时静默停解或解出错值。
- 维护 `interface_online` 点：读取 SocketCAN 错误帧识别 bus-off / 错误被动，接口 down 或 bus-off 时设备离线、信号点保留旧值并置坏质量。
- 支持 `read.can.invalidRawValues`、按帧选择 CAN FD 发送（`write.can.fd`）；在线与超时判定使用单调时钟。
- 顶层 `startupWrites[]` 已支持驱动启动自动写入。`CanDriver` 打开 SocketCAN 后按 `pointCode` 查找可写点，校验写入约束，发送 `write.can` 帧，并发布 `startup-write-succeeded`、`startup-write-failed` 或 `startup-write-skipped` 状态事件。
- `gateway-services.sh` 已支持按 `deviceConfigFiles[]` 自动启动 `can-driver@device_canX.service`，未配置的 CAN 驱动不会启动。
- 初始化安装包脚本和工厂包脚本已包含 `CanDriver` 与 `can-driver@.service`。
- Java 多接口批量配置页面已支持 `can_socketcan`，可配置 CAN 接口参数、逻辑设备、在线帧和 CAN 点表。
- 测点 Excel 模板已补充 CAN 读写字段；导入时如填写 `readCanFrameId` 或 `writeCanFrameId`，会生成 `read.can` 或 `write.can`。
- 示例文件已新增 `config/examples/device_can0_example.json`。
- Win 端通用设备点表模板已支持按协议文档生成 CAN 全量点位。正华同安复合探测器、优旦 BMS EMS CAN、盛弘 BMS-PCS CAN、华塑 BMS-PCS CAN2.0 模板会生成 `protocol.can`、`read.can`、`write.can`、`meters[].onlineFrameIds`，并仅在设备确实需要时生成 `startupWrites`。EMS 项目的正华同安复合探测器由现场一次性设置自动上传周期，服务启动时只接收主动上报，不重复下发设置命令。

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

启动写入链路：

```text
CanDriver 启动
  -> SocketCAN 接口配置并打开
  -> 读取配置 startupWrites[]
  -> 按 pointCode 定位可写点并校验范围
  -> 按 write.can 编码发送启动参数帧
  -> 发布 startup-write-* 状态事件
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
  "startupWrites": [],
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
| `startupWrites` | 驱动启动后自动执行的安全写入项，例如设置 CAN 设备自动上传周期 |
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
| `fdEnabled` | 接口启用 CAN FD：可接收 FD 帧，并允许 `write.can.fd=true` 的发送帧；默认 `false` |
| `dataBitrate` | CAN-FD 数据段波特率，`fdEnabled=true` 时生效 |
| `manageInterface` | 驱动启动时、以及接口 down 或 bus-off 超过 5 秒未自动恢复时，自动配置并拉起接口 |
| `rxQueueSize` | 仅 `udp_test` 测试传输使用（UDP 接收缓冲）；SocketCAN 使用内核队列 |
| `txQueueSize` | 仅 `udp_test` 测试传输使用（UDP 发送缓冲）；SocketCAN 使用内核队列 |

默认使用经典 CAN，`fdEnabled=false`。`fdEnabled=true` 只决定接口能力，每个发送帧是否以 FD 发出由该帧的 `write.can.fd` 决定（默认经典帧）。

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
| `interfaceCheckIntervalMs` | 检查接口 up/down、载波（bus-off 时内核会关闭载波）的周期；bus-off 另由错误帧即时识别 |
| `receiveWaitMs` | 单次等待收帧的最长时间 |
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
      "endian": "little",
      "receiveTimeoutMs": 5000,
      "invalidRawValues": ["0xFFFF"]
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
    "min": 0,
    "max": 1,
    "step": 1,
    "allowedValues": [0, 1],
    "verifyAfterWrite": false,
    "can": {
      "frameId": "0x321",
      "extended": false,
      "dlc": 8,
      "fd": false,
      "payloadTemplate": "0000000000000000",
      "byteOffset": 0,
      "bitOffset": 0,
      "bitLength": 1,
      "endian": "little"
    }
  },
  "alarms": [],
  "valueMap": {
    "0": "关闭",
    "1": "打开"
  }
}
```

## 启动写入 `startupWrites`

2026-10-27 现场确认：COMM202600104 消防厂家不支持远程修改自动上报周期。该设备的 `fire_detector_auto_upload_period` 必须关闭 `write.enable`，并禁用对应启动写入；服务启动、重启及画面操作均不得下发此设置。保留原主动上报接收配置。下述通用示例不适用于该消防设备。

部分 CAN 设备默认不会主动上报，必须先设置上传周期或订阅参数。为了避免现场每次重启后都要人工下发频率，CAN 配置支持顶层 `startupWrites[]`。

该能力不是 EMS 的默认行为。设备已经由调试人员完成一次性设置时，应配置为 `"startupWrites": []`。`COMM202600105` 的正华同安复合探测器已手动设置自动上传周期，EMS 服务启动和重启时只接收主动上报，不得再次写入上传周期。

```json
{
  "startupWrites": [
    {
      "pointCode": "sensor_report_period",
      "meterCode": "CAN_SENSOR_001",
      "value": 1,
      "enabled": true,
      "delayMs": 300,
      "repeat": 2,
      "repeatIntervalMs": 200,
      "reason": "设置状态和实时数据自动上传周期为1秒"
    }
  ]
}
```

执行规则：

- `CanDriver` 完成接口配置、打开 socket、注册点表后执行启动写入。
- `pointCode` 必填，`meterCode` 可选；不填 `meterCode` 时匹配当前配置里的同名点。
- 目标点必须启用 `write.enable=true`，并通过写入上下限校验。
- 写入使用该点的 `write.can` 编码，发送成功或失败都会发布状态事件。
- `repeat` 用于提高启动参数写入成功率，生产建议保持 1 到 2 次。

安全边界：

- 启动写入只用于上传周期、状态订阅、采集开关等不会触发现场动作的参数。
- 消防阀门控制、复位、PCS 功率下发等真实动作点不得默认加入 `startupWrites`。
- 启动写入同样经过发送帧镜像（见“写入与帧镜像”），不会清零同帧其他信号；但驱动刚启动时镜像来自 `payloadTemplate`，同帧其他信号此时取模板值。

### `read.can` 字段

| 字段 | 说明 |
| --- | --- |
| `frameId` | CAN 帧 ID，必须写成 `0x` 开头的十六进制，例如 `"0x123"`、`"0x18FF50E5"`；不带 `0x`（如 `"18FF50E5"`、`"123"`）加载时报错 |
| `extended` | 是否扩展帧，`false` 表示 11 位标准帧（ID 最大 `0x7FF`，超出加载报错），`true` 表示 29 位扩展帧（最大 `0x1FFFFFFF`） |
| `dlc` | 期望数据长度，仅用于提示：信号超出 `dlc` 时加载给出警告；实际以收到的帧长度为准，帧比信号短时该信号置坏质量 |
| `byteOffset` / `bitOffset` | 信号位置，含义随 `endian` 不同，见“信号位置定义” |
| `bitLength` | 信号位数 `1` 到 `64`；不填时按数据类型取默认（`bool`=1、8/16/32/64 位整数、`float32`=32） |
| `endian` | 只接受 `little`（Intel）或 `big`（Motorola），其他写法（`motorola`、`BE`、`Big` 等）加载报错 |
| `bitOrder` | 可省略；若填写只能为 `lsb0`（历史生成器写入的固定值），其他值加载报错。位置完全由 `endian` 决定 |
| `receiveTimeoutMs` | 超过该时间（单调时钟）未收到对应帧，点位保留旧值并置坏质量；`0` 表示不检查 |
| `invalidRawValues` | 表示“无效/不可用”的原始值（缩放前、无符号位型），整数或 `"0x..."` 字符串，例如 J1939 的 `"0xFFFF"`；收到该值时信号保留旧值、置坏质量、不写历史库。值超出信号位宽时加载报错 |

### 信号位置定义

只有一种定义，按 DBC：Intel 信号的起始位是最低位（LSB），Motorola 信号的起始位是最高位（MSB）。`bitOffset=0` 且长度为 8 的倍数时，两种字节序都是字节对齐的信号。

| `endian` | `byteOffset` | `bitOffset` | 由 DBC 起始位 S 换算 |
| --- | --- | --- | --- |
| `little`（Intel，`@1`） | LSB 所在字节 | LSB 在该字节内的位号，从字节最低位（0x01）起数 | `byteOffset = S / 8`，`bitOffset = S % 8` |
| `big`（Motorola，`@0`） | MSB 所在字节 | MSB 距该字节最高位（0x80）的位数，`0` 表示 bit7 | `byteOffset = S / 8`，`bitOffset = 7 - S % 8` |

Intel 信号从 LSB 起向高位、再向后一字节的 bit0 延伸；Motorola 信号从 MSB 起向低位、再向后一字节的 bit7 延伸。

示例（与 cantools 对照的测试向量见 `tools/can_signal_layout_test.cpp`，报文 `A5 3C 96 0F 81 7E C3 5A`）：

| DBC 定义 | 配置 | 解码值 |
| --- | --- | --- |
| `3\|12@0+` | `endian=big, byteOffset=0, bitOffset=4, bitLength=12` | `0x53C`（byte0 低 4 位 `5` + byte1 `3C`） |
| `3\|4@0+` | `endian=big, byteOffset=0, bitOffset=4, bitLength=4` | `0x5`（byte0 低半字节） |
| `7\|16@0+` | `endian=big, byteOffset=0, bitOffset=0, bitLength=16` | `0xA53C`（字节对齐大端） |
| `21\|10@0-` | `endian=big, byteOffset=2, bitOffset=2, bitLength=10, dataType=int16` | `0x160` |
| `4\|12@1+` | `endian=little, byteOffset=0, bitOffset=4, bitLength=12` | `0x3CA` |

COMM202600103 正华探测器的浓度点（`endian=big, bitOffset=0, bitLength=16`）按此定义仍是字节对齐大端，结果不变。

### `write.can` 字段

| 字段 | 说明 |
| --- | --- |
| `frameId` / `extended` | 发送帧 ID 与帧类型，规则同 `read.can` |
| `dlc` | 发送数据长度：经典帧 `0` 到 `8`；FD 帧 `0`-`8`、`12`、`16`、`20`、`24`、`32`、`48`、`64` |
| `fd` | 该帧以 CAN FD 发送，需 `protocol.can.fdEnabled=true`；默认 `false`（经典帧） |
| `remoteRequest` | 发送远程帧（仅经典帧） |
| `payloadTemplate` | 发送帧初始内容，正好 `dlc` 字节的十六进制、不带分隔符，例如 `"0000000000000000"`；不填为全 0 |
| `byteOffset` / `bitOffset` / `bitLength` / `endian` | 信号位置，规则同 `read.can` |

写入值按 `write.dataType` 编码：整数类型先 `round((value - offset) / scale)`，超出位宽（无符号负数、有符号越界）直接拒绝，不截断；`read.signed=true` 的 `uintN` 信号按补码写入；非有限值拒绝。

### 写入与帧镜像

- 每个发送帧（`frameId` + `extended`）在驱动内维护一份“最后发送的载荷”镜像，初值为 `payloadTemplate`（不填为全 0）。写某个信号时只改它自己的位，再整帧发送，同帧其他信号保持最后一次下发的值。
- 同一批待写命令中，连续写同一帧不同信号的命令合并成一帧发送；同一信号在一批里出现两次（例如脉冲 1→0）时按顺序分两帧发出，不会被合并吞掉。
- 共用一个发送帧的所有写点必须一致：`dlc`、`fd`、`remoteRequest` 相同，非空的 `payloadTemplate` 相同，否则加载报错。写信号位重叠时加载给出警告。
- 镜像不跨进程保存：驱动重启后回到 `payloadTemplate`。
- 脉冲型命令位（启动、复位、清故障）在镜像里保持最后写入的值，之后写同帧其他信号时会随整帧再次发出；这类位写 `1` 后应再写 `0`，或单独占一帧。
- 写入成功只表示帧已进入内核发送队列（`write()` 成功），CAN 没有接收方确认；需要确认结果时应由设备的状态反馈点判断。CAN 写点不支持 `verifyAfterWrite`（配置了会给出警告并忽略）。
- 原 `mode`、`mergePolicy`、`sendRepeat`、`sendIntervalMs`、`protocol.can.preserveFrameSignals` 均未实现/已删除，不要再配置。

## 数据类型

`read.dataType` / `write.dataType` 只接受下列名称，其他名称（如 `int`、`double`、`float`、`short`）加载报错：

| 类型 | 说明 |
| --- | --- |
| `bool` | 开关量，原始值非 0 为 1 |
| `uint8` / `int8` | 8 位整数 |
| `uint16` / `int16` | 16 位整数 |
| `uint32` / `int32` | 32 位整数 |
| `uint64` / `int64` | 64 位整数 |
| `float32` | IEEE754 单精度浮点，`bitLength` 必须为 32 |
| `device_online` | 设备在线状态（只读） |
| `interface_online` | CAN 接口在线状态（只读） |

`intN` 按补码符号扩展；`uintN` 配 `read.signed=true` 时也按补码解释。

解码公式：

```text
value = raw * scale + offset
```

编码公式：

```text
raw = round((value - offset) / scale)
```

## 接收与解码失败

- 每个信号独立解码：某个信号出错（帧比信号短、配置错误、浮点非有限值、命中 `invalidRawValues`）只影响该信号，同帧其他信号照常更新。
- 出错的信号保留旧值、置坏质量（`quality=0`，`qualityMsg` 为原因），`isStore=false`，不写历史库，不会被写成 0。没有旧值时发布值 0 + 坏质量。
- 解码错误按点限频写日志（每点每分钟一条，附带被抑制的条数）。
- 其他驱动持有优先控制租约时，CAN 接收不暂停（接收不占用共享总线，消防探测器等告警必须持续更新）；历史库刷盘仍让路。

## 在线状态点

CAN 需要两类在线状态。

### 接口在线

接口在线点表示 `canX` 是否可用。驱动按 `read.dataType=interface_online` 识别该点（index 和 `pointCode` 任意），放在任一逻辑设备下即可。

建议每个 CAN 配置生成一个接口在线点：

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

- `canX` 已 up、有载波且未 bus-off，socket 正常：值为 `1`。
- 接口 down、无载波、bus-off、socket 打开失败：值为 `0`。
- bus-off 由 SocketCAN 错误帧（`CAN_RAW_ERR_FILTER` 订阅 `CAN_ERR_BUSOFF`、`CAN_ERR_CRTL`、`CAN_ERR_RESTARTED`）即时识别，另外每 `interfaceCheckIntervalMs` 检查接口标志 `IFF_UP` / `IFF_RUNNING`。控制器重启（`restartMs` 自动恢复或人工 `ip link set canX type can restart`）、链路重新 up 或重新收到数据帧后恢复为 `1`。
- 无载波超过 5 秒（例如 `restartMs=0` 时的 bus-off）且 `manageInterface=true` 时，驱动重新配置并拉起接口。
- 接口离线时：所有逻辑设备 `device_online=0`，所有信号点保留旧值、置坏质量（`qualityMsg` 为 `CAN interface offline: <原因>`），不写历史库；接口恢复后，设备要重新收到帧才回到在线。
- 错误被动（error-passive）仍可收发，只发布状态事件 `bus-error-passive` / `bus-error-active`，不改变在线状态。
- 状态事件：`interface-online`、`interface-offline`（带 `reason`）、`bus-off`、`bus-restarted`。
- 在线、接收超时、接口检查都按单调时钟计时，系统时间回拨或前跳不会冻结或误判。

### 设备在线

设备在线点表示某个逻辑设备是否还在发送关键帧。

驱动注册完设备点位后必须立即发布一次 `device_online=0`，不能等到首帧或首次超时才创建该点。在线状态还必须按 `onlineTimeoutMs` 与点位 TTL 的较小周期定时续期，不能只在在线/离线切换时写一次，否则值在长时间稳定运行后会过期。这样计算引擎能持续区分“已配置但尚未收到报文”和“该设备未配置”：前者参与全局故障并按离线处理，后者因路由不存在而跳过。

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

- 该设备 `onlineFrameIds[]` 中任意帧（不区分标准帧/扩展帧），或该设备任一读点位的帧，在 `onlineTimeoutMs` 内出现，值为 `1`。
- 超过 `onlineTimeoutMs` 未出现，或接口离线，值为 `0`。
- `onlineFrameIds[]` 中的 ID 必须写成 `0x` 开头的十六进制，否则加载报错。

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
              "endian": "little",
              "receiveTimeoutMs": 5000,
              "invalidRawValues": ["0xFFFF"]
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
            "min": 0,
            "max": 1,
            "step": 1,
            "allowedValues": [0, 1],
            "verifyAfterWrite": false,
            "can": {
              "frameId": "0x321",
              "extended": false,
              "dlc": 8,
              "fd": false,
              "payloadTemplate": "0000000000000000",
              "byteOffset": 0,
              "bitOffset": 0,
              "bitLength": 1,
              "endian": "little"
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
| CAN-FD | `protocol.can.fdEnabled` | 默认关闭；开启后每个发送帧仍按 `write.can.fd` 决定是否以 FD 发出 |
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
| FD 帧 | `write.can.fd` |

交互规则：

- `category=online` 时锁定 CAN 信号列。
- `write.enable=false` 时隐藏或禁用写入模板、FD 帧。
- `dataType=bool` 时默认 `bitLength=1`。
- `float32` 默认 `bitLength=32`。
- `frameId` 一律生成 `0x` 开头的十六进制。
- 标准帧 `frameId` 最大 `0x7FF`。
- 扩展帧 `frameId` 最大 `0x1FFFFFFF`。
- `byteOffset * 8 + bitOffset + bitLength` 不得超过 `dlc * 8`。
- 从 DBC 导入大端（Motorola）信号时按“信号位置定义”换算 `bitOffset = 7 - 起始位 % 8`。
- 同一发送帧的写点 `dlc`、FD、模板必须一致。

边端在加载时对以上规则做同样校验，不符合的配置直接拒绝启动并点名测点。

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
fd
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
- 按接口状态和错误帧维护 `interface_online`。
- 扫描 pending write，按 `write.can` 编码进发送帧镜像并发送。
- bus-off 或接口 down 时发布状态事件，接口在线点置 `0`，设备离线，信号点保留旧值并置坏质量。
- `fdEnabled=true` 时接收 CAN FD 帧，`write.can.fd=true` 的帧以 CAN FD 发送。

第一阶段不实现：

- CANopen 对象字典。
- J1939 PGN / SPN 自动解析。
- DBC 文件解析。
- 多包传输协议。
- 写入回读确认（`verifyAfterWrite`）。

## systemd 模板

当前已随部署脚本和初始化包安装：

```text
/etc/systemd/system/can-driver@.service
```

服务命令：

```text
/opt/modbus-gateway/bin/CanDriver \
  --config /opt/modbus-gateway/config/runtime/devices/%i.json \
  --app-config /opt/modbus-gateway/config/runtime/apps/mqtt-service.json
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
- 原始 CAN 帧透传诊断命令。
- CAN 总线负载率统计。
- bus-off 自动恢复计数和平台告警。
