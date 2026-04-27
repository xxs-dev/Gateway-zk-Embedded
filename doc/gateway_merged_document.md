# Gateway 合并设计与配置文档

本文档是当前唯一保留的项目文档，合并了边端 C++ Gateway、Java 配置平台、运行配置、MQTT 消息样例和部署脚本。

生成时间：2026-04-23 11:09:27 +08:00


---

## Gateway-zk/doc/gateway_integrated_design.md

# Gateway 集成设计与配置总览

## 1. 文档目标

本文档整合当前边端网关项目已有设计，作为后续开发、部署、联调和排障的主入口。

覆盖内容：

- C++ 边端驱动整体架构
- Modbus RTU / Modbus TCP / DLT645-2007 驱动设计
- 统一共享内存模型
- MQTT 驱动、实时数据、告警、变位、命令下发、OTA
- MQTT 断链缓存与补传
- Java 端配置生成、下载、OTA 下发流程
- 运行时 JSON 配置样例
- systemd 部署与多驱动启动策略
- 压测、排障和参考文档索引

## 2. 项目边界

### 2.1 C++ 边端项目

路径：

```text
D:\workspace\Embedded\Gateway-zk
```

主要产物：

- `ModbusRtu`：Modbus RTU / Modbus TCP 驱动进程。
- `Dlt645Driver`：DLT645-2007 驱动进程。
- `MqttDriver`：独立 MQTT 驱动进程。
- `pointctl`：共享内存读写调试工具。
- `stress_runner`：共享内存与 MQTT 扫描压测工具。

### 2.2 Java 平台配置项目

路径：

```text
D:\workspace\CloudPlatform\idea\edge-gateway
```

职责：

- 图形化生成 Modbus / DLT645 / MQTT / OTA 配置。
- Excel 批量导入从站和点表。
- 生成运行时 JSON 文件。
- 打包配置 OTA 包。
- 通过 MQTT 下发 OTA 请求。
- 实时数据订阅和 WebSocket 推送到前端页面。

## 3. 总体架构

```text
ModbusRtu / Dlt645Driver
        |
        | write latest values / pending writes / leases
        v
gateway_point_store 共享内存
        |
        | scan latest values / submit write commands
        v
MqttDriver
        |
        | telemetry / alarm / change / command reply / ota status
        v
MQTT Broker
        |
        v
Java 平台 / Web 前端
```

关键设计：

- 所有协议驱动共用一个共享内存：`gateway_point_store`。
- 多个协议驱动可以并行运行，但每个驱动只消费自己配置内的写入命令。
- MQTT 独立为一个驱动进程，只从共享内存读取并统一上传。
- Java 端不直接依赖边端 device 配置解析实时数据，实时 payload 自带 `machineCode / meterCode / pointCode`。
- OTA 配置包可以清理旧运行 JSON，再写入新 JSON，并由 `gateway-services.service` 统一重启所需驱动。

## 4. 进程与服务

### 4.1 驱动进程

| 进程 | 用途 | 典型配置 |
|---|---|---|
| `ModbusRtu` | Modbus RTU / TCP 采集、写回 | `config/runtime/devices/*.json` |
| `Dlt645Driver` | DLT645-2007 采集 | `config/runtime/devices/device_dlt645_multi_meter_1_2.json` |
| `MqttDriver` | MQTT 上传、命令下发、OTA | `config/runtime/apps/mqtt-service.json` |
| `pointctl` | 调试共享内存值和写命令 | `--shm gateway_point_store` |
| `stress_runner` | 性能压测 | 压测参数 |

### 4.2 systemd 服务

边端部署目录：

```text
/opt/modbus-gateway
```

服务模板：

- `modbus-rtu@.service`
- `dlt645-driver@.service`
- `mqtt-driver@.service`
- `gateway-services.service`

推荐只启用：

```bash
systemctl enable gateway-services.service
systemctl start gateway-services.service
```

`gateway-services.sh` 会扫描：

```text
/opt/modbus-gateway/config/runtime/devices/*.json
/opt/modbus-gateway/config/runtime/apps/mqtt-service.json
```

启动规则：

- `protocol.type=modbus_rtu` 或 `modbus_tcp`：启动 `modbus-rtu@<配置文件名>.service`
- `protocol.type=dlt645_2007`：启动 `dlt645-driver@<配置文件名>.service`
- 存在 `runtime/apps/mqtt-service.json`：启动 `mqtt-driver@mqtt-service.service`

如果某类配置不存在，该类驱动不会启动。

## 5. 运行时配置目录

当前配置目录分为运行时依赖和样例：

```text
config/runtime/devices       协议驱动运行配置
config/runtime/apps          MQTT / App / OTA 运行配置
config/templates             DLT645 标准点表模板
config/samples/messages      MQTT 消息样例
deploy                       systemd 和 OTA 脚本
doc                          设计、测试、部署文档
```

当前核心运行配置：

- `config/runtime/apps/mqtt-service.json`
- `config/runtime/devices/device_slave_ttySP1.json`
- `config/runtime/devices/device_slave_ttySP2.json`
- `config/runtime/devices/device_alarm_multi_slave_tcp_1_2.json`
- `config/runtime/devices/device_dlt645_multi_meter_1_2.json`

## 6. 设备配置 JSON 设计

### 6.1 顶层结构

设备配置通用结构：

```json
{
  "schemaVersion": "1.0.0",
  "machineCode": "GW0001",
  "protocol": {},
  "collect": {},
  "memoryStore": {},
  "meters": []
}
```

字段说明：

- `machineCode`：网关编号。
- `protocol`：协议和传输配置。
- `collect`：采集周期和批量优化策略。
- `memoryStore`：共享内存、持久化、写回队列参数。
- `meters`：从站、仪表和点位。

### 6.2 Modbus RTU 示例

示例文件：

```text
config/runtime/devices/device_slave_ttySP1.json
```

核心结构：

```json
{
  "schemaVersion": "1.0.0",
  "machineCode": "GW0001",
  "protocol": {
    "type": "modbus_rtu",
    "slave": 1,
    "transport": {
      "serialPort": "/dev/ttySP1",
      "baudRate": 9600,
      "dataBits": 8,
      "stopBits": 1,
      "parity": "N",
      "timeoutMs": 1000
    }
  },
  "collect": {
    "defaultIntervalMs": 5000,
    "batchOptimize": true,
    "maxBatchRegisters": 120
  },
  "memoryStore": {
    "sharedMemoryName": "gateway_point_store",
    "maxLatestPoints": 100000,
    "maxPendingWrites": 4096,
    "maxPersistentSamples": 20000
  },
  "meters": [
    {
      "meterCode": "TTYSP1_SLAVE0001",
      "slave": 1,
      "points": []
    }
  ]
}
```

点位示例：

```json
{
  "index": 31101,
  "pointCode": "reg_1",
  "name": "Register 1",
  "category": "telemetry",
  "address": 1,
  "enabled": true,
  "reportOnChange": true,
  "read": {
    "enable": true,
    "function": 3,
    "length": 1,
    "dataType": "uint16",
    "scale": 1,
    "offset": 0,
    "byteOrder": "AB",
    "intervalMs": 500
  },
  "write": {
    "enable": true,
    "function": 6,
    "length": 1,
    "dataType": "uint16",
    "verifyAfterWrite": true
  },
  "alarms": []
}
```

设备在线点：

```json
{
  "index": 31100,
  "pointCode": "device_online",
  "category": "status",
  "reportOnChange": true,
  "read": {
    "enable": true,
    "function": 0,
    "length": 0,
    "dataType": "device_online",
    "intervalMs": 500
  },
  "write": {
    "enable": false
  }
}
```

### 6.3 Modbus TCP 示例

示例文件：

```text
config/runtime/devices/device_alarm_multi_slave_tcp_1_2.json
```

核心结构：

```json
{
  "machineCode": "GW_TCP_01",
  "protocol": {
    "type": "modbus_tcp",
    "slave": 1,
    "tcp": {
      "host": "192.168.1.100",
      "port": 502,
      "connectTimeoutMs": 1000,
      "timeoutMs": 1000
    }
  },
  "meters": [
    {
      "meterCode": "TCP_SLAVE0001",
      "slave": 1,
      "points": []
    }
  ]
}
```

### 6.4 DLT645-2007 示例

示例文件：

```text
config/runtime/devices/device_dlt645_multi_meter_1_2.json
```

核心结构：

```json
{
  "machineCode": "GW0001",
  "protocol": {
    "type": "dlt645_2007",
    "transport": {
      "serialPort": "/dev/ttyS1",
      "baudRate": 2400,
      "dataBits": 8,
      "stopBits": 1,
      "parity": "E",
      "timeoutMs": 1000
    },
    "standardPointsFile": "config/templates/dlt645_2007_standard_points.json",
    "standardPointsVersion": "1.1.0"
  },
  "collect": {
    "defaultIntervalMs": 1000,
    "batchOptimize": false,
    "maxBatchRegisters": 1
  },
  "meters": [
    {
      "meterCode": "METER0001",
      "slave": 1,
      "address": "000000000001",
      "points": []
    },
    {
      "meterCode": "METER0002",
      "slave": 2,
      "address": "000000000002",
      "points": []
    }
  ]
}
```

DLT645 规则：

- `points=[]` 时从 `standardPointsFile` 自动展开国标标准点位。
- DLT645 点位核心字段是 `read.dlt645.di`。
- 标准点表位于 `config/templates/dlt645_2007_standard_points.json`。
- 当前线电压等设备不支持项已从标准模板中剔除。

## 7. 统一共享内存

共享内存名称：

```text
gateway_point_store
```

所有协议驱动写入同一个共享内存，MQTT 驱动统一读取。

容量：

- latest slots：`100000`
- pending write slots：`4096`
- persistent sample slots：`20000`
- claim slots：`100000`

主要内容：

- latest values：最新点值。
- pending writes：MQTT / pointctl 写入命令。
- persistent samples：待持久化采样。
- leases：点位归属和活跃状态。

写回保护：

- 每个协议驱动只消费自身配置内的写命令。
- 避免多个协议驱动共用共享内存时抢走不属于自己的写命令。

## 8. MQTT App 配置

统一应用配置文件：

```text
config/runtime/apps/mqtt-service.json
```

核心结构：

```json
{
  "deviceConfigFiles": [
    "/opt/modbus-gateway/config/runtime/devices/device_slave_ttySP1.json",
    "/opt/modbus-gateway/config/runtime/devices/device_slave_ttySP2.json"
  ],
  "mqtt": {},
  "mqttDriver": {},
  "alarmStore": {},
  "realtime": {},
  "ota": {}
}
```

### 8.1 MQTT 连接

```json
{
  "mqtt": {
    "enabled": true,
    "protocolVersion": "mqtt5",
    "broker": "tcp://192.168.22.102:1883",
    "clientId": "mqtt-driver-01",
    "username": "kyxn",
    "password": "whkyxn027",
    "telemetryTopic": "edge/telemetry",
    "changeEventTopic": "edge/event/change",
    "alarmTopic": "edge/alarm",
    "statusTopic": "edge/status",
    "commandRequestTopic": "edge/command/request",
    "commandReplyTopic": "edge/command/reply",
    "otaRequestTopic": "edge/ota/request",
    "otaReplyTopic": "edge/ota/reply",
    "otaStatusTopic": "edge/ota/status",
    "qos": 1,
    "maxPayloadBytes": 49152,
    "cleanSession": true,
    "keepAliveSec": 60,
    "sessionExpirySec": 0
  }
}
```

支持：

- `mqtt3`
- `mqtt5`
- TCP 直连
- 用户名密码
- QoS0 / QoS1
- 实时数据按 `maxPayloadBytes` 自动分片，默认 `49152` 字节

当前不支持：

- TLS

### 8.2 MQTT Driver

```json
{
  "mqttDriver": {
    "enabled": true,
    "sharedMemoryName": "gateway_point_store",
    "scanIntervalMs": 500,
    "fullUploadIntervalMs": 1000,
    "publishFullOnStart": true,
    "publishAllOnFull": true,
    "fullUploadIndexes": []
  }
}
```

说明：

- `scanIntervalMs`：扫描共享内存和处理 MQTT 下发的周期。
- `fullUploadIntervalMs`：全量实时数据上报周期。
- `publishAllOnFull=true`：全量上报全部可见点。
- `fullUploadIndexes`：可指定只上传部分点。

## 9. MQTT 消息结构

### 9.1 实时数据

topic：

```text
edge/telemetry
```

新格式：

```json
{
  "type": "snapshot",
  "machineCode": "GW0001",
  "meters": [
    {
      "meterCode": "TTYSP1_SLAVE0001",
      "values": [
        {
          "index": 31101,
          "pointCode": "reg_1",
          "value": 123,
          "quality": 1,
          "ts": 1776900000000,
          "stale": false
        }
      ]
    }
  ]
}
```

实时数据分片格式：

当一次全量实时数据超过 `mqtt.maxPayloadBytes` 时，边端仍发布到 `edge/telemetry`，但会拆成多片：

```json
{
  "type": "snapshot",
  "machineCode": "GW0001",
  "chunked": true,
  "chunkId": "1776906000000-1",
  "chunkIndex": 1,
  "chunkCount": 3,
  "meters": [
    {
      "meterCode": "TTYSP1_SLAVE0001",
      "values": [
        {
          "index": 31101,
          "pointCode": "reg_1",
          "value": 123,
          "quality": 1,
          "ts": 1776906000000,
          "expireAt": 1776906600000,
          "stale": false
        }
      ]
    }
  ]
}
```

平台处理规则：

- 实时监测页面可以收到一片就刷新一片，不需要等待所有分片。
- 如果需要完整快照，则按 `machineCode + chunkId` 聚合，收到 `chunkCount` 片后合并。
- `chunkIndex` 从 `1` 开始。
- 分片可能乱序到达，平台不能依赖接收顺序。
- 未分片消息不带 `chunked / chunkId / chunkIndex / chunkCount`。

### 9.2 变位事件

topic：

```text
edge/event/change
```

触发条件：

- 点位配置 `reportOnChange=true`
- 第一次扫描只建立基线
- 后续只有 `value` 变化才上传
- `ts / expireAt` 刷新不会重复触发

### 9.3 告警事件

topic：

```text
edge/alarm
```

特点：

- 支持高限、低限、恢复。
- 触发顺序修正为先恢复旧告警，再触发新告警。
- `persistValue` 为空时不存盘；非空时写入告警 SQLite。

### 9.4 命令下发

topic：

```text
edge/command/request
```

请求：

```json
{
  "cmdId": "CMD2026041515300001",
  "machineCode": "GW0001",
  "meterCode": "TTYSP1_SLAVE0001",
  "pointCode": "reg_1",
  "index": 31101,
  "value": 200,
  "source": "mqtt",
  "ts": 1776900000000
}
```

回执 topic：

```text
edge/command/reply
```

说明：

- MQTT 回执表示命令已进入共享写队列或被拒绝。
- 实际设备写入由对应协议驱动消费 pending write 后执行。
- 点位必须 `write.enable=true`。

### 9.5 OTA

请求 topic：

```text
edge/ota/request
```

状态 topic：

```text
edge/ota/status
```

回执 topic：

```text
edge/ota/reply
```

OTA 状态：

- `accepted`
- `downloading`
- `verifying`
- `applying`
- `rollback`
- `completed`
- `failed`

## 10. MQTT 断链缓存与补传

当前采用两条链路：

### 10.1 实时数据 Ring

配置：

```json
{
  "offlineBuffer": {
    "enabled": true,
    "mode": "ring",
    "realtimeFile": "/opt/modbus-gateway/data/mqtt-spool/realtime_ring.dat",
    "realtimeFileSizeBytes": 1073741824,
    "maxRealtimeMessageBytes": 4194304,
    "replayBatchSize": 20
  }
}
```

说明：

- 只缓存实时数据。
- 文件启动时预分配 1GB。
- 单条实时消息最大 4MB。
- 文件满后覆盖最旧实时数据。
- 恢复连接后按批次补传。

容量估算：

```text
1GB / 3MB ~= 341 条
1GB / 2MB ~= 512 条
```

### 10.2 SQLite Event Outbox

缓存数据：

- 告警事件
- 变位事件
- OTA 状态

配置：

```json
{
  "eventOutbox": {
    "sqlitePath": "/opt/modbus-gateway/data/mqtt_event_outbox.db",
    "sqliteLibraryPath": "",
    "retentionMonths": 12,
    "cleanupIntervalHours": 24,
    "replayBatchSize": 100
  }
}
```

表结构：

```sql
CREATE TABLE IF NOT EXISTS mqtt_event_outbox (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  event_type TEXT NOT NULL,
  topic TEXT NOT NULL,
  payload TEXT NOT NULL,
  event_ts INTEGER NOT NULL,
  event_month TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  sent INTEGER NOT NULL DEFAULT 0,
  sent_at INTEGER,
  retry_count INTEGER NOT NULL DEFAULT 0,
  last_error TEXT
);
```

清理规则：

- 只清理 `sent=1` 的已发送事件。
- 保留 12 个自然月。
- 例如：`2026-01` 的数据在 `2027-02` 清理。
- `sent=0` 的未补发数据不会按时间清理。

补发顺序：

```text
SQLite Event Outbox -> Realtime Ring -> 当前新消息
```

详细设计：

```text
doc/mqtt_offline_replay_design.md
```

## 11. OTA 配置包链路

### 11.1 Java 端生成

Java 端会把选中的配置打成 tar.gz：

```text
gateway-config-<machineCode>-<version>.tar.gz
```

包内包含：

- `config/runtime/devices/*.json`
- `config/runtime/apps/mqtt-service.json`
- `config/templates/dlt645_2007_standard_points.json`
- `deploy/*.service`
- `deploy/gateway-services.sh`
- `deploy/ota-apply.sh`
- `deploy/ota-rollback.sh`
- `manifest.json`

### 11.2 manifest 清理旧配置

配置 OTA 包包含：

```json
{
  "cleanBeforeApply": [
    {
      "target": "/opt/modbus-gateway/config/runtime/devices",
      "patterns": ["*.json"]
    },
    {
      "target": "/opt/modbus-gateway/config/runtime/apps",
      "patterns": ["*.json"]
    }
  ]
}
```

边端应用时：

1. 清理旧 `runtime/devices/*.json`
2. 清理旧 `runtime/apps/*.json`
3. 写入新配置
4. 更新 systemd 模板和脚本
5. `systemctl daemon-reload`
6. 重启 `gateway-services.service`

### 11.3 OTA 状态可靠性

OTA 状态先写入：

```text
/opt/modbus-gateway/ota/staging/ota_status_pending.log
```

然后尝试 MQTT 发送。若 MQTT 服务在 OTA 中被重启，新 `MqttDriver` 启动后会补发 pending 状态并清空文件。

## 12. Java 配置平台

主要功能：

- 图形化编辑 MQTT / OTA / 共享内存配置。
- 多串口批量生成 Modbus 配置。
- DLT645 作为协议选项融入多串口配置。
- Excel 导入从站和点表，第二行中文描述会跳过。
- 下载生成的 `device_slave_xxx.json`。
- 构建配置 OTA 包。
- 发布 OTA 到 MQTT。
- 实时监测页面按 `machineCode / meterCode` 分类显示，避免一次性渲染所有点。
- 实时监测页面支持 `chunked=true` 的 MQTT 实时分片消息；Java 后端收到一片就解析 `meters[].values[]`、刷新缓存并通过 WebSocket 推送，前端在分组和点表中显示 `chunkIndex / chunkCount`。

配置文件命名规则：

- 单串口配置：`device_slave_<serial>.json`
- MQTT 配置：`mqtt-service.json`
- 配置 OTA 包：`gateway-config-<machineCode>-<version>.tar.gz`

## 13. DLT645 标准点表

DLT645-2007 国标点表固定，由模板生成：

```text
config/templates/dlt645_2007_standard_points.json
```

设计原则：

- 生成 DLT645 配置时同时附带标准点位模板。
- `points=[]` 表示从标准点表自动展开。
- 只保留 4 个费率：尖、峰、平、谷。
- 支持上 N 结算点电能值。
- 不支持的点位直接从模板剔除，而不是禁用。

## 14. 本地存储

### 14.1 点值持久化

由协议驱动写入：

```text
/opt/modbus-gateway/data/point_samples*.db
```

控制字段：

- `memoryStore.persistFlushIntervalMs`
- `memoryStore.maxPersistentSamples`

### 14.2 告警存盘

由 `alarmStore` 控制：

```json
{
  "alarmStore": {
    "enabled": true,
    "sqlitePath": "/opt/modbus-gateway/data/alarm_events.db"
  }
}
```

只有告警规则 `persistValue` 非空时才存盘。

### 14.3 MQTT 事件补发

SQLite：

```text
/opt/modbus-gateway/data/mqtt_event_outbox.db
```

用于告警、变位、OTA 状态断链补发。

### 14.4 MQTT 实时 ring

文件：

```text
/opt/modbus-gateway/data/mqtt-spool/realtime_ring.dat
```

用于实时大报文断链补发。

## 15. 部署流程

### 15.1 目录

```text
/opt/modbus-gateway/bin
/opt/modbus-gateway/config/runtime/devices
/opt/modbus-gateway/config/runtime/apps
/opt/modbus-gateway/config/templates
/opt/modbus-gateway/data
/opt/modbus-gateway/ota
```

### 15.2 安装服务

```bash
cp deploy/*.service /etc/systemd/system/
cp deploy/*.sh /opt/modbus-gateway/bin/
chmod +x /opt/modbus-gateway/bin/*.sh
systemctl daemon-reload
systemctl enable gateway-services.service
systemctl start gateway-services.service
```

### 15.3 查看状态

```bash
systemctl status gateway-services.service
systemctl status modbus-rtu@device_slave_ttySP1.service
systemctl status mqtt-driver@mqtt-service.service
ps -ef | grep -E 'ModbusRtu|Dlt645Driver|MqttDriver'
```

### 15.4 查看服务扫描结果

```bash
GATEWAY_HOME=/opt/modbus-gateway /opt/modbus-gateway/bin/gateway-services.sh list
```

## 16. 测试与排障

### 16.1 共享内存查看

```bash
/opt/modbus-gateway/bin/pointctl snapshot
/opt/modbus-gateway/bin/pointctl leases
/opt/modbus-gateway/bin/pointctl pending
```

### 16.2 写命令测试

```bash
/opt/modbus-gateway/bin/pointctl write --index 31101 --value 200
```

要求：

- 点位存在。
- 点位 `write.enable=true`。
- 对应协议驱动正在运行。

### 16.3 MQTT 下发测试

topic：

```text
edge/command/request
```

payload：

```json
{
  "cmdId": "CMD_TEST_001",
  "machineCode": "GW0001",
  "meterCode": "TTYSP1_SLAVE0001",
  "index": 31101,
  "value": 200,
  "source": "mqtt",
  "ts": 1776900000000
}
```

### 16.4 OTA 测试

1. Java 页面生成配置 OTA 包。
2. 确认 `artifactUrl` 是完整 URL。
3. 边端 `MqttDriver` 必须运行。
4. 发布到 `edge/ota/request`。
5. 观察 `edge/ota/status`。

### 16.5 断链补发测试

实时 ring：

- 临时把 broker 改成错误端口。
- 触发一次全量上传。
- 检查 ring header 中 `recordCount > 0`。
- 恢复 broker 后检查 `recordCount = 0`。

事件 outbox：

- 插入 `sent=0` 测试事件。
- 触发一次 MQTT 发送。
- 检查该事件被标记为 `sent=1`。

## 17. 性能与容量

共享内存：

- latest：100000 点
- pending writes：4096 条
- persistent：20000 条

MQTT 实时 ring：

- 默认 1GB
- 单条最大 4MB
- 2MB 数据约 512 条
- 3MB 数据约 341 条

事件 outbox：

- SQLite 存储关键事件。
- 已发送事件按自然月保留 12 个月。
- 未发送事件不按时间删除。

## 18. 参考文档索引

详细设计：

- `doc/modbus_gateway_detailed_design.md`
- `doc/spec.md`

配置说明：

- `doc/config_reference.md`
- `doc/app_config.md`
- `config/README.md`

共享内存：

- `doc/memory_sync_design.md`

MQTT：

- `doc/mqtt_driver.md`
- `doc/mqtt_offline_replay_design.md`

OTA：

- `doc/config_to_ota_delivery.md`
- `doc/config_ota_e2e_test.md`

Modbus TCP：

- `doc/modbus_tcp_support.md`

DLT645：

- `doc/dlt645_2007_support.md`
- `doc/dlt645_2007_template_reference.md`
- `doc/dlt645_runtime_test.md`

部署：

- `doc/deployment_ubuntu.md`

压测：

- `doc/performance_stress_test.md`

## 19. 当前已验证状态

截至当前实现：

- Java 编译通过。
- C++ aarch64 交叉编译通过。
- 边端 `192.168.22.12` 已运行新 `MqttDriver`。
- `realtime_ring.dat` 已创建为 1GB。
- `mqtt_event_outbox.db` 已创建。
- 实时 ring 断链缓存与恢复补发已验证。
- SQLite outbox 事件补发并标记 `sent=1` 已验证。


---

## Gateway-zk/doc/spec.md

# Modbus 云边网关项目详设

## 1. 文档目的

本文档用于指导一个基于 **C++ 边缘网关 + Java 云平台** 的 Modbus 设备接入系统实现，重点解决以下问题：

- 如何用配置驱动 Modbus 点位读写
- 如何支持只读、只写、可读可写点位
- 如何把采集结果存入内存数据库
- 如何为 MQTT 上报、命令下发、写后校验提供统一数据模型
- 如何让 C++ 边缘程序与 Java 云端使用统一协议规范

---

## 2. 总体目标

系统包含两部分：

### 2.1 边缘侧（C++）

负责：

- Modbus RTU / TCP 通信
- 周期采集点位
- 执行平台下发写命令
- 写后回读校验
- 将采集值写入内存数据库
- 通过 MQTT 上报最新值、状态、命令结果

### 2.2 云端（Java）

负责：

- 管理设备与点位模板
- 通过 MQTT 下发控制命令
- 接收边缘上报的测点值
- 写入 MySQL / InfluxDB
- 提供查询接口和控制接口

---

## 3. 核心设计原则

### 3.1 一个业务点 = 一个 pointCode

同一个业务点不拆成 read / write 两个点。

例如：

- `target_power` 是一个点
- 它既可以读，也可以写
- 读写规则分别配置在 `read` 和 `write` 下

### 3.2 配置驱动，不写死寄存器逻辑

程序不应该硬编码：

- 地址 0 是电压
- 地址 1 是电流
- 地址 100 是开关

而应该完全依赖配置：

- 点位编码
- 功能码
- 地址
- 长度
- 数据类型
- 字节序
- 比例系数
- 写入约束

### 3.3 采集与上报解耦

推荐流程：

```text
Modbus读取 -> 解析 -> 写入内存数据库 -> MQTT上报线程读取缓存并发布
```

这样可以避免：

- 采集线程被网络阻塞
- 上报失败影响现场读取
- 写后回读与普通采集相互干扰

### 3.4 最新值和短历史都要保存

仅保存最新值不够。系统需要：

- 最新值：用于实时查询
- 短历史：用于变化检测、写后校验、异常分析

---

## 4. 配置文件总体结构

一个设备配置文件建议如下：

```json
{
  "schemaVersion": "1.0.0",
  "machineCode": "GW0001",
  "meterCode": "METER0001",
  "deviceName": "1号储能电表",
  "protocol": {
    "type": "modbus_rtu",
    "slave": 1,
    "transport": {
      "serialPort": "/dev/ttyS1",
      "baudRate": 9600,
      "dataBits": 8,
      "stopBits": 1,
      "parity": "N",
      "timeoutMs": 1000
    }
  },
  "collect": {
    "defaultIntervalMs": 5000,
    "batchOptimize": true,
    "maxBatchRegisters": 120
  },
  "memoryStore": {
    "enabled": true,
    "backend": "memory",
    "keepHistory": 100,
    "defaultTtlMs": 600000,
    "indexBy": ["machineCode", "meterCode", "pointCode"]
  },
  "points": []
}
```

---

## 5. 点位配置规范

### 5.1 点位基础结构

```json
{
  "pointCode": "voltage_a",
  "name": "A相电压",
  "desc": "电表A相电压",
  "category": "telemetry",
  "address": 0,
  "enabled": true,
  "tags": ["power", "phase-a"],
  "read": {},
  "write": {},
  "alarm": {},
  "valueMap": null,
  "qualityRule": {}
}
```

### 5.2 顶层字段定义

| 字段 | 含义 |
|---|---|
| pointCode | 点位唯一编码 |
| name | 点位名称 |
| desc | 点位说明 |
| category | 点位分类 |
| address | 寄存器起始地址 |
| enabled | 是否启用 |
| tags | 标签 |
| read | 读能力定义 |
| write | 写能力定义 |
| alarm | 告警阈值 |
| valueMap | 枚举值映射 |
| qualityRule | 质量判定规则 |

### 5.3 category 建议值

- `telemetry`：遥测
- `status`：状态
- `setting`：设定值
- `command`：控制
- `alarm`：告警
- `counter`：累计量

---

## 6. 读能力 read 规范

```json
"read": {
  "enable": true,
  "function": 3,
  "length": 1,
  "dataType": "uint16",
  "scale": 0.1,
  "offset": 0,
  "byteOrder": "AB",
  "signed": false,
  "unit": "V",
  "intervalMs": 5000,
  "cachePolicy": {
    "storeLatest": true,
    "storeHistory": true,
    "historySize": 100,
    "ttlMs": 600000
  }
}
```

### 6.1 function 支持

- `1`：读线圈
- `2`：读离散输入
- `3`：读保持寄存器
- `4`：读输入寄存器

### 6.2 dataType 建议支持

- `bool`
- `bit`
- `uint16`
- `int16`
- `uint32`
- `int32`
- `uint64`
- `int64`
- `float32`
- `float64`
- `bcd`
- `string`

第一版建议至少支持：

- `bit`
- `uint16`
- `int16`
- `uint32`
- `int32`
- `float32`

### 6.3 byteOrder 支持

- `AB`
- `BA`
- `ABCD`
- `BADC`
- `CDAB`
- `DCBA`

### 6.4 scale 和 offset

最终值：

```text
actualValue = rawValue * scale + offset
```

### 6.5 cachePolicy

```json
"cachePolicy": {
  "storeLatest": true,
  "storeHistory": true,
  "historySize": 100,
  "ttlMs": 600000
}
```

含义：

- `storeLatest`：是否保存最新值
- `storeHistory`：是否保存短历史
- `historySize`：历史条数上限
- `ttlMs`：该点缓存有效期

---

## 7. 写能力 write 规范

```json
"write": {
  "enable": true,
  "function": 6,
  "length": 1,
  "dataType": "uint16",
  "scale": 1,
  "offset": 0,
  "byteOrder": "AB",
  "min": 0,
  "max": 100,
  "step": 1,
  "allowedValues": [0, 1, 2],
  "verifyAfterWrite": true,
  "verifyDelayMs": 200,
  "verifyByRead": true
}
```

### 7.1 function 支持

- `5`：写单线圈
- `6`：写单寄存器
- `15`：写多个线圈
- `16`：写多个寄存器

### 7.2 写入编码规则

原始值计算：

```text
rawValue = (businessValue - offset) / scale
```

### 7.3 写入约束

- `min` / `max`：范围限制
- `step`：步长限制
- `allowedValues`：枚举值白名单

### 7.4 写后校验

- `verifyAfterWrite=true` 时，写完后延迟 `verifyDelayMs` 毫秒
- 若 `verifyByRead=true`，则使用 read 配置重新读取并校验

---

## 8. 特殊点位设计

### 8.1 bit 点

```json
{
  "pointCode": "breaker_status",
  "name": "断路器状态",
  "address": 20,
  "category": "status",
  "enabled": true,
  "read": {
    "enable": true,
    "function": 3,
    "length": 1,
    "dataType": "bit",
    "bit": 3,
    "scale": 1,
    "offset": 0,
    "byteOrder": "AB",
    "unit": "",
    "intervalMs": 2000,
    "cachePolicy": {
      "storeLatest": true,
      "storeHistory": true,
      "historySize": 50,
      "ttlMs": 600000
    }
  },
  "write": {
    "enable": false
  },
  "valueMap": {
    "0": "分闸",
    "1": "合闸"
  }
}
```

### 8.2 可读可写点

```json
{
  "pointCode": "run_mode",
  "name": "运行模式",
  "address": 120,
  "category": "setting",
  "enabled": true,
  "read": {
    "enable": true,
    "function": 3,
    "length": 1,
    "dataType": "uint16",
    "scale": 1,
    "offset": 0,
    "byteOrder": "AB",
    "unit": "",
    "intervalMs": 2000,
    "cachePolicy": {
      "storeLatest": true,
      "storeHistory": true,
      "historySize": 50,
      "ttlMs": 600000
    }
  },
  "write": {
    "enable": true,
    "function": 6,
    "length": 1,
    "dataType": "uint16",
    "scale": 1,
    "offset": 0,
    "byteOrder": "AB",
    "min": 0,
    "max": 2,
    "step": 1,
    "allowedValues": [0, 1, 2],
    "verifyAfterWrite": true,
    "verifyDelayMs": 200,
    "verifyByRead": true
  },
  "valueMap": {
    "0": "停止",
    "1": "自动",
    "2": "手动"
  }
}
```

### 8.3 32 位写点

```json
{
  "pointCode": "target_power",
  "name": "目标功率",
  "address": 200,
  "category": "setting",
  "enabled": true,
  "read": {
    "enable": true,
    "function": 3,
    "length": 2,
    "dataType": "uint32",
    "scale": 1,
    "offset": 0,
    "byteOrder": "ABCD",
    "unit": "W",
    "intervalMs": 3000,
    "cachePolicy": {
      "storeLatest": true,
      "storeHistory": true,
      "historySize": 100,
      "ttlMs": 600000
    }
  },
  "write": {
    "enable": true,
    "function": 16,
    "length": 2,
    "dataType": "uint32",
    "scale": 1,
    "offset": 0,
    "byteOrder": "ABCD",
    "min": 0,
    "max": 50000,
    "step": 100,
    "verifyAfterWrite": true,
    "verifyDelayMs": 300,
    "verifyByRead": true
  }
}
```

---

## 9. 内存数据库设计

边缘端建议实现一个进程内内存数据库 `MemoryPointStore`，而不是一开始就上 Redis。

### 9.1 目标

- 保存最新值
- 保存最近 N 条历史
- 支持按 `machineCode + meterCode + pointCode` 查询
- 支持 TTL 过期判断
- 支持供 MQTT 上报线程、HTTP 查询线程、规则引擎使用

### 9.2 PointValue 数据结构

```cpp
struct PointValue {
    std::string machineCode;
    std::string meterCode;
    std::string pointCode;
    std::string pointName;
    std::string category;
    std::string unit;

    double value = 0.0;
    std::string text;
    std::string rawHex;

    int quality = 1;
    std::string qualityMsg = "ok";

    int64_t ts = 0;
    int64_t expireAt = 0;
    bool stale = false;

    int function = 3;
    int address = 0;
    int length = 1;
};
```

### 9.3 存储结构

#### latestStore

按 key 保存最新值：

```cpp
std::unordered_map<std::string, PointValue> latestStore;
```

key：

```text
machineCode:meterCode:pointCode
```

#### historyStore

按 key 保存最近 N 条历史：

```cpp
std::unordered_map<std::string, std::deque<PointValue>> historyStore;
```

### 9.4 MemoryPointStore 接口建议

```cpp
class MemoryPointStore {
public:
    void putLatest(const PointValue& value);
    void appendHistory(const PointValue& value, size_t maxSize);

    std::optional<PointValue> getLatest(
        const std::string& machineCode,
        const std::string& meterCode,
        const std::string& pointCode
    ) const;

    std::vector<PointValue> getHistory(
        const std::string& machineCode,
        const std::string& meterCode,
        const std::string& pointCode,
        size_t limit
    ) const;

    std::vector<PointValue> getDeviceLatest(
        const std::string& machineCode,
        const std::string& meterCode
    ) const;

    void removeExpired(int64_t nowMs);
};
```

### 9.5 过期策略

- 点位写入时，根据 `ttlMs` 计算 `expireAt`
- 查询时若 `now > expireAt`，则认为该值过期
- 过期后不一定物理删除，但要标记 `stale=true`

### 9.6 写入流程

```text
1. Modbus返回原始寄存器
2. 根据 point 配置解析出业务值
3. 生成 PointValue
4. 写入 latestStore
5. 根据 cachePolicy 决定是否进入 historyStore
```

### 9.7 推荐查询返回 JSON

```json
{
  "machineCode": "GW0001",
  "meterCode": "METER0001",
  "pointCode": "voltage_a",
  "pointName": "A相电压",
  "value": 220.5,
  "text": "220.5",
  "unit": "V",
  "quality": 1,
  "qualityMsg": "ok",
  "ts": 1776038400000,
  "stale": false
}
```

---

## 10. 采集流程设计

### 10.1 周期采集

```text
配置加载 -> 构建读任务 -> 周期执行 -> 解析 -> 写内存库 -> 上报
```

### 10.2 批量合并读

系统应按以下维度合并点位读取：

- slave
- function
- 连续地址范围
- 最大寄存器数限制

合并后形成 `ReadTask`：

```cpp
struct ReadTask {
    int function;
    int start;
    int count;
    std::vector<PointDefinition> points;
};
```

### 10.3 解析流程

```text
for each task:
    read raw registers
    for each point in task:
        calculate offset in register block
        reorder bytes by byteOrder
        parse by dataType
        apply scale + offset
        map valueMap if needed
        build PointValue
        write MemoryPointStore
```

---

## 11. 写命令流程设计

平台下发命令消息建议：

```json
{
  "cmdId": "CMD202604130001",
  "machineCode": "GW0001",
  "meterCode": "METER0001",
  "pointCode": "run_mode",
  "value": 1
}
```

边缘侧执行流程：

```text
1. 根据 pointCode 找到点位配置
2. 检查 write.enable
3. 检查 min / max / step / allowedValues
4. 按 write.dataType / scale / offset / byteOrder 编码
5. 发送 Modbus 写命令
6. 若 verifyAfterWrite=true，则延迟后回读校验
7. 返回执行结果
```

命令执行结果建议：

```json
{
  "cmdId": "CMD202604130001",
  "machineCode": "GW0001",
  "meterCode": "METER0001",
  "pointCode": "run_mode",
  "success": true,
  "message": "ok",
  "ts": 1776038405000
}
```

---

## 12. MQTT 主题建议

### 12.1 遥测上报

```text
iot/{machineCode}/telemetry
```

### 12.2 状态上报

```text
iot/{machineCode}/status
```

### 12.3 命令下发

```text
iot/{machineCode}/command/down
```

### 12.4 命令回执

```text
iot/{machineCode}/command/up
```

---

## 13. C++ 建议结构体

```cpp
struct CachePolicy {
    bool storeLatest = true;
    bool storeHistory = true;
    size_t historySize = 100;
    int64_t ttlMs = 600000;
};

struct ReadSpec {
    bool enable = false;
    int function = 3;
    int length = 1;
    std::string dataType;
    double scale = 1.0;
    double offset = 0.0;
    std::string byteOrder = "AB";
    bool signedFlag = false;
    std::string unit;
    int intervalMs = 5000;
    int bit = -1;
    CachePolicy cachePolicy;
};

struct WriteSpec {
    bool enable = false;
    int function = 6;
    int length = 1;
    std::string dataType;
    double scale = 1.0;
    double offset = 0.0;
    std::string byteOrder = "AB";
    double minValue = 0.0;
    double maxValue = 0.0;
    double step = 0.0;
    std::vector<double> allowedValues;
    bool verifyAfterWrite = false;
    int verifyDelayMs = 200;
    bool verifyByRead = true;
};

struct PointDefinition {
    std::string pointCode;
    std::string name;
    std::string desc;
    std::string category;
    int address = 0;
    bool enabled = true;
    std::vector<std::string> tags;
    ReadSpec read;
    WriteSpec write;
    std::unordered_map<std::string, std::string> valueMap;
};

struct PointValue {
    std::string machineCode;
    std::string meterCode;
    std::string pointCode;
    std::string pointName;
    std::string category;
    std::string unit;
    double value = 0.0;
    std::string text;
    std::string rawHex;
    int quality = 1;
    std::string qualityMsg = "ok";
    int64_t ts = 0;
    int64_t expireAt = 0;
    bool stale = false;
    int function = 3;
    int address = 0;
    int length = 1;
};
```

---

## 14. 最小实现优先级

建议先做以下内容：

### P0

- JSON 配置加载
- 点位模型解析
- Modbus 03 / 04 读取
- uint16 / int16 / uint32 / float32 解析
- 内存数据库 latest + history
- MQTT 遥测上报

### P1

- Modbus 06 / 16 写入
- 写入参数校验
- 写后回读校验
- bit 点解析
- valueMap 映射

### P2

- 线圈读写
- BCD / string 类型
- 本地持久化恢复
- HTTP 查询接口
- 规则引擎

---

## 15. Codex 使用建议

若要在 Codex 中继续当前思路，建议做法：

1. 将本文档保存为 `spec.md`
2. 将 `spec.md` 放入项目根目录
3. 在 Codex 中打开该项目目录
4. 直接给 Codex 下任务，例如：

```text
请根据 spec.md 生成 edge-gateway 的 C++ 项目骨架，包含：
- config 模块
- point definition 解析
- MemoryPointStore 实现
- Modbus 读任务模型
- MQTT 发布接口定义
```

也可以继续分阶段下：

```text
根据 spec.md 先只实现 MemoryPointStore，要求线程安全，支持 latest/history/ttl。
```

```text
根据 spec.md 实现 PointDefinition 的 JSON 反序列化，使用 nlohmann/json。
```

```text
根据 spec.md 实现 Modbus 解析器，先支持 uint16/int16/uint32/float32 和 byteOrder。
```

---

## 16. 结论

本详设的核心是：

- 点位配置驱动读写行为
- 读写能力分离但属于同一业务点
- 采集值先进入内存数据库，再供上层使用
- 通过标准化数据结构支撑 MQTT、写后校验、状态查询和后续时序入库

该设计既适合当前阶段快速落地，也方便后续扩展到完整工业物联网平台。



---

## Gateway-zk/doc/modbus_gateway_detailed_design.md

# Modbus 云边网关项目详设

## 1. 文档目的

本文档用于指导一个基于 **C++ 边缘网关 + Java 云平台** 的 Modbus 设备接入系统实现，重点解决以下问题：

- 如何用配置驱动 Modbus 点位读写
- 如何支持只读、只写、可读可写点位
- 如何把采集结果存入内存数据库
- 如何为 MQTT 上报、命令下发、写后校验提供统一数据模型
- 如何让 C++ 边缘程序与 Java 云端使用统一协议规范

---

## 2. 总体目标

系统包含两部分：

### 2.1 边缘侧（C++）

负责：

- Modbus RTU / TCP 通信
- 周期采集点位
- 执行平台下发写命令
- 写后回读校验
- 将采集值写入内存数据库
- 通过 MQTT 上报最新值、状态、命令结果

### 2.2 云端（Java）

负责：

- 管理设备与点位模板
- 通过 MQTT 下发控制命令
- 接收边缘上报的测点值
- 写入 MySQL / InfluxDB
- 提供查询接口和控制接口

---

## 3. 核心设计原则

### 3.1 一个业务点 = 一个 pointCode

同一个业务点不拆成 read / write 两个点。

例如：

- `target_power` 是一个点
- 它既可以读，也可以写
- 读写规则分别配置在 `read` 和 `write` 下

### 3.2 配置驱动，不写死寄存器逻辑

程序不应该硬编码：

- 地址 0 是电压
- 地址 1 是电流
- 地址 100 是开关

而应该完全依赖配置：

- 点位编码
- 功能码
- 地址
- 长度
- 数据类型
- 字节序
- 比例系数
- 写入约束

### 3.3 采集与上报解耦

推荐流程：

```text
Modbus读取 -> 解析 -> 写入内存数据库 -> MQTT上报线程读取缓存并发布
```

这样可以避免：

- 采集线程被网络阻塞
- 上报失败影响现场读取
- 写后回读与普通采集相互干扰

### 3.4 最新值和短历史都要保存

仅保存最新值不够。系统需要：

- 最新值：用于实时查询
- 短历史：用于变化检测、写后校验、异常分析

---

## 4. 配置文件总体结构

一个设备配置文件建议如下：

```json
{
  "schemaVersion": "1.0.0",
  "machineCode": "GW0001",
  "meterCode": "METER0001",
  "deviceName": "1号储能电表",
  "protocol": {
    "type": "modbus_rtu",
    "slave": 1,
    "transport": {
      "serialPort": "/dev/ttyS1",
      "baudRate": 9600,
      "dataBits": 8,
      "stopBits": 1,
      "parity": "N",
      "timeoutMs": 1000
    }
  },
  "collect": {
    "defaultIntervalMs": 5000,
    "batchOptimize": true,
    "maxBatchRegisters": 120
  },
  "memoryStore": {
    "enabled": true,
    "backend": "memory",
    "keepHistory": 100,
    "defaultTtlMs": 600000,
    "indexBy": ["machineCode", "meterCode", "pointCode"]
  },
  "points": []
}
```

---

## 5. 点位配置规范

### 5.1 点位基础结构

```json
{
  "pointCode": "voltage_a",
  "name": "A相电压",
  "desc": "电表A相电压",
  "category": "telemetry",
  "address": 0,
  "enabled": true,
  "tags": ["power", "phase-a"],
  "read": {},
  "write": {},
  "alarm": {},
  "valueMap": null,
  "qualityRule": {}
}
```

### 5.2 顶层字段定义

| 字段 | 含义 |
|---|---|
| pointCode | 点位唯一编码 |
| name | 点位名称 |
| desc | 点位说明 |
| category | 点位分类 |
| address | 寄存器起始地址 |
| enabled | 是否启用 |
| tags | 标签 |
| read | 读能力定义 |
| write | 写能力定义 |
| alarm | 告警阈值 |
| valueMap | 枚举值映射 |
| qualityRule | 质量判定规则 |

### 5.3 category 建议值

- `telemetry`：遥测
- `status`：状态
- `setting`：设定值
- `command`：控制
- `alarm`：告警
- `counter`：累计量

---

## 6. 读能力 read 规范

```json
"read": {
  "enable": true,
  "function": 3,
  "length": 1,
  "dataType": "uint16",
  "scale": 0.1,
  "offset": 0,
  "byteOrder": "AB",
  "signed": false,
  "unit": "V",
  "intervalMs": 5000,
  "cachePolicy": {
    "storeLatest": true,
    "storeHistory": true,
    "historySize": 100,
    "ttlMs": 600000
  }
}
```

### 6.1 function 支持

- `1`：读线圈
- `2`：读离散输入
- `3`：读保持寄存器
- `4`：读输入寄存器

### 6.2 dataType 建议支持

- `bool`
- `bit`
- `uint16`
- `int16`
- `uint32`
- `int32`
- `uint64`
- `int64`
- `float32`
- `float64`
- `bcd`
- `string`

第一版建议至少支持：

- `bit`
- `uint16`
- `int16`
- `uint32`
- `int32`
- `float32`

### 6.3 byteOrder 支持

- `AB`
- `BA`
- `ABCD`
- `BADC`
- `CDAB`
- `DCBA`

### 6.4 scale 和 offset

最终值：

```text
actualValue = rawValue * scale + offset
```

### 6.5 cachePolicy

```json
"cachePolicy": {
  "storeLatest": true,
  "storeHistory": true,
  "historySize": 100,
  "ttlMs": 600000
}
```

含义：

- `storeLatest`：是否保存最新值
- `storeHistory`：是否保存短历史
- `historySize`：历史条数上限
- `ttlMs`：该点缓存有效期

---

## 7. 写能力 write 规范

```json
"write": {
  "enable": true,
  "function": 6,
  "length": 1,
  "dataType": "uint16",
  "scale": 1,
  "offset": 0,
  "byteOrder": "AB",
  "min": 0,
  "max": 100,
  "step": 1,
  "allowedValues": [0, 1, 2],
  "verifyAfterWrite": true,
  "verifyDelayMs": 200,
  "verifyByRead": true
}
```

### 7.1 function 支持

- `5`：写单线圈
- `6`：写单寄存器
- `15`：写多个线圈
- `16`：写多个寄存器

### 7.2 写入编码规则

原始值计算：

```text
rawValue = (businessValue - offset) / scale
```

### 7.3 写入约束

- `min` / `max`：范围限制
- `step`：步长限制
- `allowedValues`：枚举值白名单

### 7.4 写后校验

- `verifyAfterWrite=true` 时，写完后延迟 `verifyDelayMs` 毫秒
- 若 `verifyByRead=true`，则使用 read 配置重新读取并校验

---

## 8. 特殊点位设计

### 8.1 bit 点

```json
{
  "pointCode": "breaker_status",
  "name": "断路器状态",
  "address": 20,
  "category": "status",
  "enabled": true,
  "read": {
    "enable": true,
    "function": 3,
    "length": 1,
    "dataType": "bit",
    "bit": 3,
    "scale": 1,
    "offset": 0,
    "byteOrder": "AB",
    "unit": "",
    "intervalMs": 2000,
    "cachePolicy": {
      "storeLatest": true,
      "storeHistory": true,
      "historySize": 50,
      "ttlMs": 600000
    }
  },
  "write": {
    "enable": false
  },
  "valueMap": {
    "0": "分闸",
    "1": "合闸"
  }
}
```

### 8.2 可读可写点

```json
{
  "pointCode": "run_mode",
  "name": "运行模式",
  "address": 120,
  "category": "setting",
  "enabled": true,
  "read": {
    "enable": true,
    "function": 3,
    "length": 1,
    "dataType": "uint16",
    "scale": 1,
    "offset": 0,
    "byteOrder": "AB",
    "unit": "",
    "intervalMs": 2000,
    "cachePolicy": {
      "storeLatest": true,
      "storeHistory": true,
      "historySize": 50,
      "ttlMs": 600000
    }
  },
  "write": {
    "enable": true,
    "function": 6,
    "length": 1,
    "dataType": "uint16",
    "scale": 1,
    "offset": 0,
    "byteOrder": "AB",
    "min": 0,
    "max": 2,
    "step": 1,
    "allowedValues": [0, 1, 2],
    "verifyAfterWrite": true,
    "verifyDelayMs": 200,
    "verifyByRead": true
  },
  "valueMap": {
    "0": "停止",
    "1": "自动",
    "2": "手动"
  }
}
```

### 8.3 32 位写点

```json
{
  "pointCode": "target_power",
  "name": "目标功率",
  "address": 200,
  "category": "setting",
  "enabled": true,
  "read": {
    "enable": true,
    "function": 3,
    "length": 2,
    "dataType": "uint32",
    "scale": 1,
    "offset": 0,
    "byteOrder": "ABCD",
    "unit": "W",
    "intervalMs": 3000,
    "cachePolicy": {
      "storeLatest": true,
      "storeHistory": true,
      "historySize": 100,
      "ttlMs": 600000
    }
  },
  "write": {
    "enable": true,
    "function": 16,
    "length": 2,
    "dataType": "uint32",
    "scale": 1,
    "offset": 0,
    "byteOrder": "ABCD",
    "min": 0,
    "max": 50000,
    "step": 100,
    "verifyAfterWrite": true,
    "verifyDelayMs": 300,
    "verifyByRead": true
  }
}
```

---

## 9. 内存数据库设计

边缘端建议实现一个进程内内存数据库 `MemoryPointStore`，而不是一开始就上 Redis。

### 9.1 目标

- 保存最新值
- 保存最近 N 条历史
- 支持按 `machineCode + meterCode + pointCode` 查询
- 支持 TTL 过期判断
- 支持供 MQTT 上报线程、HTTP 查询线程、规则引擎使用

### 9.2 PointValue 数据结构

```cpp
struct PointValue {
    std::string machineCode;
    std::string meterCode;
    std::string pointCode;
    std::string pointName;
    std::string category;
    std::string unit;

    double value = 0.0;
    std::string text;
    std::string rawHex;

    int quality = 1;
    std::string qualityMsg = "ok";

    int64_t ts = 0;
    int64_t expireAt = 0;
    bool stale = false;

    int function = 3;
    int address = 0;
    int length = 1;
};
```

### 9.3 存储结构

#### latestStore

按 key 保存最新值：

```cpp
std::unordered_map<std::string, PointValue> latestStore;
```

key：

```text
machineCode:meterCode:pointCode
```

#### historyStore

按 key 保存最近 N 条历史：

```cpp
std::unordered_map<std::string, std::deque<PointValue>> historyStore;
```

### 9.4 MemoryPointStore 接口建议

```cpp
class MemoryPointStore {
public:
    void putLatest(const PointValue& value);
    void appendHistory(const PointValue& value, size_t maxSize);

    std::optional<PointValue> getLatest(
        const std::string& machineCode,
        const std::string& meterCode,
        const std::string& pointCode
    ) const;

    std::vector<PointValue> getHistory(
        const std::string& machineCode,
        const std::string& meterCode,
        const std::string& pointCode,
        size_t limit
    ) const;

    std::vector<PointValue> getDeviceLatest(
        const std::string& machineCode,
        const std::string& meterCode
    ) const;

    void removeExpired(int64_t nowMs);
};
```

### 9.5 过期策略

- 点位写入时，根据 `ttlMs` 计算 `expireAt`
- 查询时若 `now > expireAt`，则认为该值过期
- 过期后不一定物理删除，但要标记 `stale=true`

### 9.6 写入流程

```text
1. Modbus返回原始寄存器
2. 根据 point 配置解析出业务值
3. 生成 PointValue
4. 写入 latestStore
5. 根据 cachePolicy 决定是否进入 historyStore
```

### 9.7 推荐查询返回 JSON

```json
{
  "machineCode": "GW0001",
  "meterCode": "METER0001",
  "pointCode": "voltage_a",
  "pointName": "A相电压",
  "value": 220.5,
  "text": "220.5",
  "unit": "V",
  "quality": 1,
  "qualityMsg": "ok",
  "ts": 1776038400000,
  "stale": false
}
```

---

## 10. 采集流程设计

### 10.1 周期采集

```text
配置加载 -> 构建读任务 -> 周期执行 -> 解析 -> 写内存库 -> 上报
```

### 10.2 批量合并读

系统应按以下维度合并点位读取：

- slave
- function
- 连续地址范围
- 最大寄存器数限制

合并后形成 `ReadTask`：

```cpp
struct ReadTask {
    int function;
    int start;
    int count;
    std::vector<PointDefinition> points;
};
```

### 10.3 解析流程

```text
for each task:
    read raw registers
    for each point in task:
        calculate offset in register block
        reorder bytes by byteOrder
        parse by dataType
        apply scale + offset
        map valueMap if needed
        build PointValue
        write MemoryPointStore
```

---

## 11. 写命令流程设计

平台下发命令消息建议：

```json
{
  "cmdId": "CMD202604130001",
  "machineCode": "GW0001",
  "meterCode": "METER0001",
  "pointCode": "run_mode",
  "value": 1
}
```

边缘侧执行流程：

```text
1. 根据 pointCode 找到点位配置
2. 检查 write.enable
3. 检查 min / max / step / allowedValues
4. 按 write.dataType / scale / offset / byteOrder 编码
5. 发送 Modbus 写命令
6. 若 verifyAfterWrite=true，则延迟后回读校验
7. 返回执行结果
```

命令执行结果建议：

```json
{
  "cmdId": "CMD202604130001",
  "machineCode": "GW0001",
  "meterCode": "METER0001",
  "pointCode": "run_mode",
  "success": true,
  "message": "ok",
  "ts": 1776038405000
}
```

---

## 12. MQTT 主题建议

### 12.1 遥测上报

```text
iot/{machineCode}/telemetry
```

### 12.2 状态上报

```text
iot/{machineCode}/status
```

### 12.3 命令下发

```text
iot/{machineCode}/command/down
```

### 12.4 命令回执

```text
iot/{machineCode}/command/up
```

### 12.5 推荐统一主题

如果边端驱动统一按独立 MQTT 进程承载，建议在实现中统一为：

```text
edge/{machineCode}/telemetry
edge/{machineCode}/command/request
edge/{machineCode}/command/reply
edge/{machineCode}/ota/request
edge/{machineCode}/ota/reply
edge/{machineCode}/ota/status
```

单网关部署时也可以退化为：

```text
edge/telemetry
edge/command/request
edge/command/reply
edge/ota/request
edge/ota/reply
edge/ota/status
```

---

## 13. MQTT 命令下发设计

### 13.1 目标

- 云端通过 MQTT 下发点位写命令
- 边端统一把命令转换为共享内存写请求
- 由已有 `GatewayDaemon -> CommandExecutor` 执行设备写入
- 最终通过 MQTT 回执执行结果

### 13.2 推荐请求 JSON

```json
{
  "cmdId": "CMD2026041415300001",
  "machineCode": "GW0001",
  "meterCode": "SLAVE0001",
  "index": 11001,
  "pointCode": "reg_1",
  "value": 1,
  "source": "mqtt",
  "ts": 1776155909818
}
```

说明：

- `index` 为主路由字段
- `pointCode` 为辅助字段，便于人看日志
- `source` 建议固定写 `mqtt`

### 13.3 边端处理流程

```text
MQTT命令订阅 -> JSON解析 -> 参数校验 -> submitWriteCommand -> 共享内存pendingWrites -> GatewayDaemon写回 -> 命令回执上报
```

### 13.4 回执 JSON

```json
{
  "cmdId": "CMD2026041415300001",
  "machineCode": "GW0001",
  "meterCode": "SLAVE0001",
  "pointCode": "reg_1",
  "index": 11001,
  "success": true,
  "message": "ok",
  "ts": 1776155910123
}
```

### 13.5 落地建议

- 命令消费模块建议独立于采集线程
- 命令解析失败也应回执
- 命令执行永远走共享内存队列，不直接绕过 `GatewayDaemon`

---

## 14. OTA 升级模块设计

### 14.1 目标

- 云端通过 MQTT 触发 OTA
- 边端负责下载、校验、切换版本、上报进度
- 升级流程不阻塞 Modbus 主采集线程

### 14.2 模块拆分

建议新增边端模块：

- `OtaService`
  - 接收升级任务
  - 状态机驱动
- `OtaDownloader`
  - 下载升级包
- `OtaVerifier`
  - SHA256 / 文件大小校验
- `OtaInstaller`
  - 解包、替换二进制、准备重启
- `OtaReporter`
  - MQTT 回执和进度上报

### 14.3 推荐请求 JSON

```json
{
  "jobId": "OTA2026041416000001",
  "machineCode": "GW0001",
  "artifactUrl": "https://example.com/releases/modbus-gateway-1.2.3.tar.gz",
  "version": "1.2.3",
  "sha256": "7d6f0d2d7c9f1d4d8b0c7b8e5a0c4e4b8d2c8a8d7f4e0a1b2c3d4e5f6a7b8c9d",
  "size": 10485760,
  "upgradeMode": "download_install_reboot",
  "ts": 1776156000000
}
```

### 14.4 OTA 状态机

推荐状态：

- `accepted`
- `downloading`
- `downloaded`
- `verifying`
- `verified`
- `installing`
- `installed`
- `restarting`
- `success`
- `failed`

### 14.5 OTA 主题和消息

立即回执：

```json
{
  "jobId": "OTA2026041416000001",
  "machineCode": "GW0001",
  "accepted": false,
  "message": "ota executor not implemented",
  "ts": 1776156000100
}
```

进度消息：

```json
{
  "jobId": "OTA2026041416000001",
  "machineCode": "GW0001",
  "stage": "downloading",
  "progress": 45,
  "message": "downloading package",
  "ts": 1776156005100
}
```

### 14.6 OTA 本地目录建议

```text
/opt/modbus-gateway/bin
/opt/modbus-gateway/config
/opt/modbus-gateway/data
/opt/modbus-gateway/ota/downloads
/opt/modbus-gateway/ota/staging
/opt/modbus-gateway/ota/backup
```

### 14.7 OTA 安全建议

- 必做 SHA256 校验
- 升级包版本必须带版本号
- 切换前保留旧版本备份
- 安装失败必须可回滚
- 升级状态必须持久化，避免重启后状态丢失

---

## 13. C++ 建议结构体

```cpp
struct CachePolicy {
    bool storeLatest = true;
    bool storeHistory = true;
    size_t historySize = 100;
    int64_t ttlMs = 600000;
};

struct ReadSpec {
    bool enable = false;
    int function = 3;
    int length = 1;
    std::string dataType;
    double scale = 1.0;
    double offset = 0.0;
    std::string byteOrder = "AB";
    bool signedFlag = false;
    std::string unit;
    int intervalMs = 5000;
    int bit = -1;
    CachePolicy cachePolicy;
};

struct WriteSpec {
    bool enable = false;
    int function = 6;
    int length = 1;
    std::string dataType;
    double scale = 1.0;
    double offset = 0.0;
    std::string byteOrder = "AB";
    double minValue = 0.0;
    double maxValue = 0.0;
    double step = 0.0;
    std::vector<double> allowedValues;
    bool verifyAfterWrite = false;
    int verifyDelayMs = 200;
    bool verifyByRead = true;
};

struct PointDefinition {
    std::string pointCode;
    std::string name;
    std::string desc;
    std::string category;
    int address = 0;
    bool enabled = true;
    std::vector<std::string> tags;
    ReadSpec read;
    WriteSpec write;
    std::unordered_map<std::string, std::string> valueMap;
};

struct PointValue {
    std::string machineCode;
    std::string meterCode;
    std::string pointCode;
    std::string pointName;
    std::string category;
    std::string unit;
    double value = 0.0;
    std::string text;
    std::string rawHex;
    int quality = 1;
    std::string qualityMsg = "ok";
    int64_t ts = 0;
    int64_t expireAt = 0;
    bool stale = false;
    int function = 3;
    int address = 0;
    int length = 1;
};
```

---

## 14. 最小实现优先级

建议先做以下内容：

### P0

- JSON 配置加载
- 点位模型解析
- Modbus 03 / 04 读取
- uint16 / int16 / uint32 / float32 解析
- 内存数据库 latest + history
- MQTT 遥测上报

### P1

- Modbus 06 / 16 写入
- 写入参数校验
- 写后回读校验
- bit 点解析
- valueMap 映射

### P2

- 线圈读写
- BCD / string 类型
- 本地持久化恢复
- HTTP 查询接口
- 规则引擎

---

## 15. Codex 使用建议

若要在 Codex 中继续当前思路，建议做法：

1. 将本文档保存为 `spec.md`
2. 将 `spec.md` 放入项目根目录
3. 在 Codex 中打开该项目目录
4. 直接给 Codex 下任务，例如：

```text
请根据 spec.md 生成 edge-gateway 的 C++ 项目骨架，包含：
- config 模块
- point definition 解析
- MemoryPointStore 实现
- Modbus 读任务模型
- MQTT 发布接口定义
```

也可以继续分阶段下：

```text
根据 spec.md 先只实现 MemoryPointStore，要求线程安全，支持 latest/history/ttl。
```

```text
根据 spec.md 实现 PointDefinition 的 JSON 反序列化，使用 nlohmann/json。
```

```text
根据 spec.md 实现 Modbus 解析器，先支持 uint16/int16/uint32/float32 和 byteOrder。
```

---

## 16. 结论

本详设的核心是：

- 点位配置驱动读写行为
- 读写能力分离但属于同一业务点
- 采集值先进入内存数据库，再供上层使用
- 通过标准化数据结构支撑 MQTT、写后校验、状态查询和后续时序入库

该设计既适合当前阶段快速落地，也方便后续扩展到完整工业物联网平台。


---

## Gateway-zk/doc/memory_sync_design.md

# 内存缓存与写回设计

适用目标环境：

- Ubuntu 20.04
- ARM 边端设备
- 同机多进程共享点位最新值和待写命令

## 1. 当前目标

- 内存里只保留轻量最新值，按 `index` 索引
- 只有配置了 `isStore=true` 的点才进入本地持久化采样队列
- 其他服务如果修改共享内存，不直接写设备，而是提交待写命令
- 统一由写回服务消费待写命令，并同步下发到 Modbus 设备

## 2. 轻量内存结构

### 2.1 最新值

`MemoryPointStore` 当前最新值结构为：

```cpp
struct StoredPointValue {
    uint32_t index;
    double value;
    int quality;
    int64_t ts;
    int64_t expireAt;
    bool stale;
};
```

特点：

- 按 `index` 存储
- 不在内存里重复保留完整点位描述
- 适合实时查询和共享访问

### 2.2 点位绑定

内存中保留 `index <-> gateway/device/pointCode` 的绑定关系，用于：

- 按业务键查询最新值
- 其他服务提交 `index` 写命令后反查点位配置

## 3. 本地持久化采样

当点位配置：

```json
{
  "index": 10002,
  "isStore": true,
  "persistIntervalSec": 60
}
```

则该点进入分钟采样队列：

```cpp
struct PersistentPointSample {
    uint32_t index;
    double value;
    int64_t ts;
};
```

调用：

```cpp
auto samples = store.drainPersistentSamples();
```

即可把待落盘数据取出，再写入 SQLite / CSV / 自定义本地库。

## 4. 共享内存写回设备

其他服务不要直接调用 Modbus。

统一做法：

1. 其他服务把写请求提交给 `MemoryPointStore`
2. `WritebackService` 周期消费待写队列
3. `CommandExecutor` 根据 `index` 找到点位并写入设备
4. 写成功后再回写最新值缓存

对应结构：

```cpp
struct PendingWriteCommand {
    std::string cmdId;
    uint32_t index;
    double value;
    std::string source;
    int64_t ts;
};
```

提交命令：

```cpp
store.submitWriteCommand({"CMD202604130002", 10002, 2, "other-service", nowMs});
```

消费命令：

```cpp
WritebackService writebackService(store, executor);
auto results = writebackService.processPendingWrites(nowMs);
```

## 5. 共享内存实现方式

当前目标实现不是 Windows 命名共享内存，而是 Linux POSIX 共享内存：

- `shm_open`
- `ftruncate`
- `mmap`
- `pthread_mutex(process-shared)`

这样边端上的多个进程可以共享：

- 最新测点值
- 待写命令队列
- 待持久化分钟采样队列

## 6. SQLite 分钟落盘

推荐单独起一个本地持久化线程或服务：

1. 每分钟或更短周期调用 `drainPersistentSamples()`
2. 将采样批量写入 SQLite
3. 表结构按 `(point_index, ts)` 做主键

当前工程中的 `SqliteSampleWriter` 负责：

- 自动建表 `point_samples`
- 批量写入分钟采样
- 重复时间点执行 upsert

推荐 SQLite 文件位置例如：

- `/var/lib/modbus_gateway/point_samples.db`
- `/opt/modbus_gateway/data/point_samples.db`

## 7. 守护进程骨架

当前工程新增 `GatewayDaemon`，负责起 3 条后台线程：

- 采集线程：按 `collect.defaultIntervalMs` 周期采集
- 写回线程：按 `memoryStore.writebackIntervalMs` 消费共享写命令
- 落盘线程：按 `memoryStore.persistFlushIntervalMs` 把分钟采样写入 SQLite

支持的手动入口：

```cpp
daemon.collectOnce(nowMs);
daemon.processWritebackOnce(nowMs);
daemon.flushPersistentOnce();
```

后台运行入口：

```cpp
daemon.start();
daemon.stop();
```

## 8. 推荐配置项

`memoryStore` 建议配置：

```json
{
  "sharedMemoryName": "gateway_point_store",
  "maxLatestPoints": 100000,
  "maxPendingWrites": 4096,
  "maxPersistentSamples": 20000,
  "sqlitePath": "point_samples.db",
  "sqliteLibraryPath": "",
  "persistFlushIntervalMs": 60000,
  "writebackIntervalMs": 500,
  "writebackBatchSize": 100
}
```

说明：

- `sharedMemoryName`：POSIX 共享内存段名称；同一网关内 Modbus RTU、Modbus TCP、DLT645 等协议驱动统一使用 `gateway_point_store`
- `maxLatestPoints`：latest 点位建议容量；当前 C++ 运行时会按编译期共享内存布局自动使用 100000 点容量，避免旧配置值过小导致提前满
- `maxPendingWrites`：待写命令环形队列逻辑上限
- `maxPersistentSamples`：待落盘分钟采样环形队列逻辑上限
- `sqlitePath`：本地 SQLite 文件路径
- `sqliteLibraryPath`：如系统库不在默认路径，可显式指定 `libsqlite3.so`
- `persistFlushIntervalMs`：分钟采样刷盘周期
- `writebackIntervalMs`：待写命令消费周期
- `writebackBatchSize`：每次消费的写命令数量上限

## 9. 推荐运行方式

- `Collector` 负责采集并刷新最新值
- `MemoryPointStore` 作为跨进程共享内存缓存
- 业务服务只读写 `MemoryPointStore`
- `WritebackService` 独立线程定时处理共享写命令
- SQLite 落盘线程定时处理 `drainPersistentSamples()`
- 多协议共用同一共享内存时，各协议驱动只消费本进程已注册点位的写命令，其他 index 的命令会继续留在 pending 队列中等待对应驱动处理

## 10. 当前限制

- 当前示例入口仍然在 Windows 开发机上跑模拟串口，真正部署目标是 Ubuntu ARM
- `SqliteSampleWriter` 运行时要求系统存在 `libsqlite3.so`
- 共享内存布局版本或容量变化时，新进程会自动调整共享内存段大小并重新初始化该段；升级时仍建议先停掉旧驱动进程，再启动新版本，避免旧进程继续持有旧�


---

## Gateway-zk/doc/config_reference.md

# Config 说明

本文档专门说明 `config/` 目录下各类配置文件的用途，以及主要字段的含义。

## 文件分类

当前 `config/` 目录建议按两类区分：

- 运行时依赖配置
  - `config/runtime/devices/*.json`
  - `config/runtime/apps/*.json`
- 报文与日志样例
  - `config/samples/messages/*.json`
  - `config/samples/logs/*.log`

当前已整理为：

- 设备采集配置
  - `config/runtime/devices/device_slave_ttySP1.json`
  - `config/runtime/devices/device_slave_ttySP2.json`
  - `config/runtime/devices/device_alarm_multi_slave_tcp_1_2.json`
  - `config/runtime/devices/device_dlt645_multi_meter_1_2.json`
- MQTT 驱动 / 应用配置
  - `config/runtime/apps/mqtt-service.json`
- MQTT 消息样例
  - `config/samples/messages/mqtt_command_request_example.json`
  - `config/samples/messages/mqtt_telemetry_chunk_example.json`
  - `config/samples/messages/mqtt_ota_request_example.json`
  - `config/samples/messages/mqtt_ota_status_example.json`
  - `config/samples/messages/mqtt_status_started_example.json`
  - `config/samples/messages/mqtt_status_full_snapshot_example.json`
  - `config/samples/messages/mqtt_alarm_event_example.json`

## 1. 设备采集配置

设备配置文件负责定义：

- 协议类型
- 通信参数
- 采集周期
- 共享内存参数
- SQLite 参数
- 从站列表
- 点位读写能力
- 点位告警规则

### 顶层字段

#### `schemaVersion`

配置版本号，用于后续配置演进兼容。

#### `machineCode`

网关编码。  
同一边端实例建议唯一。

#### `protocol`

协议定义。

字段：

- `type`
  - `modbus_rtu` 或 `modbus_tcp`
- `slave`
  - 默认从站地址
  - 多从站模式下可被 `devices[].slave` 覆盖
- `transport`
  - RTU 串口参数
- `tcp`
  - TCP 连接参数

#### `collect`

采集线程参数。

字段：

- `defaultIntervalMs`
  - 默认采集周期
- `batchOptimize`
  - 是否按连续地址合并读任务
- `maxBatchRegisters`
  - 单批最大寄存器数

#### `memoryStore`

共享内存和本地缓存参数。

字段：

- `enabled`
  - 是否启用内存存储
- `backend`
  - 当前主要是 `memory`
- `keepHistory`
  - 历史条数逻辑参数
- `defaultTtlMs`
  - 默认缓存有效期
- `indexBy`
  - 逻辑索引键
- `sharedMemoryName`
  - 共享内存段名称
- `maxLatestPoints`
  - latest 点位逻辑上限
- `maxPendingWrites`
  - 待写命令逻辑上限
- `maxPersistentSamples`
  - 待落盘采样逻辑上限
- `sqlitePath`
  - 点位分钟存盘 SQLite 路径
- `sqliteLibraryPath`
  - SQLite 动态库路径
- `persistFlushIntervalMs`
  - 采样落盘周期
- `writebackIntervalMs`
  - 写回线程消费周期
- `writebackBatchSize`
  - 单次写回最多消费多少条命令

#### `devices`

多从站设备列表。

每个元素表示一个逻辑设备，字段：

- `meterCode`
  - 设备编码
- `deviceName`
  - 设备名称
- `slave`
  - 对应 RTU slave / TCP unit id
- `points`
  - 点位列表

补充说明：

- 设备采集配置里的逻辑设备分组字段统一为 `meters`
- MQTT 上送的 grouped telemetry / snapshot 消息体分组字段也是 `meters`
- 配置结构和消息结构字段已统一，平台侧不需要再区分 `devices` / `meters`

## 2. 点位字段

### 基础字段

- `index`
  - 共享内存主索引，必须全局唯一
- `pointCode`
  - 点位编码
- `name`
  - 点位名称
- `desc`
  - 说明
- `category`
  - 分类
- `address`
  - Modbus 寄存器地址
- `enabled`
  - 是否启用
- `isStore`
  - 是否按分钟本地存盘
- `reportOnChange`
  - 是否启用变位上传；为 `true` 时首次扫描只建立基线，后续仅在数值真正变化时发送单条 `change` 事件到 `mqtt.changeEventTopic`，不因 `ts / expireAt` 刷新重复上传
- `persistIntervalSec`
  - 本地采样存盘周期
- `tags`
  - 标签数组

### `read`

定义读能力。

字段：

- `enable`
  - 是否可读
- `function`
  - 读功能码，常见 `3/4`
- `length`
  - 寄存器长度
- `dataType`
  - 数据类型，如 `uint16`
- `scale`
  - 缩放系数
- `offset`
  - 偏移量
- `byteOrder`
  - 字节序
- `signed`
  - 是否按有符号读
- `unit`
  - 单位
- `intervalMs`
  - 该点采集周期
- `cachePolicy`
  - 缓存策略

### `write`

定义写能力。

字段：

- `enable`
  - 是否可写
- `function`
  - 写功能码，常见 `6/16`
- `length`
  - 写入寄存器长度
- `dataType`
  - 写入数据类型
- `scale`
  - 写缩放
- `offset`
  - 写偏移
- `byteOrder`
  - 写字节序
- `min`
  - 最小值
- `max`
  - 最大值
- `step`
  - 步长
- `verifyAfterWrite`
  - 是否写后回读校验
- `verifyDelayMs`
  - 写后延时
- `verifyByRead`
  - 是否通过读回校验

注意：

- 只把 `write.enable=true` 打开还不够
- `function / dataType / byteOrder / length` 至少要配完整

### `alarms`

告警规则数组。

每个元素字段：

- `type`
  - `high` 或 `low`
- `threshold`
  - 阈值
- `reportRecovery`
  - 是否上报恢复
- `persistValue`
  - 非空则告警写 SQLite
  - 空字符串则只上报不存盘

## 3. MQTT 驱动配置

MQTT 驱动配置文件负责定义：

- 要读取哪些设备配置文件
- MQTT 连接参数
- MQTT 上传参数
- 告警存盘参数

### `deviceConfigFiles`

设备配置文件路径数组。

作用：

- MQTT 驱动通过它读取点位告警规则
- 不再依赖独立 `alarmRules`

### `mqtt`

MQTT 连接与 topic 参数。

字段：

- `enabled`
  - 是否启用真实 MQTT
- `protocolVersion`
  - `mqtt3` 或 `mqtt5`
- `broker`
  - broker 地址
- `clientId`
  - 客户端 ID
- `username`
  - 用户名
- `password`
  - 密码
- `telemetryTopic`
  - 遥测 topic
- `changeEventTopic`
  - 变位事件 topic；配置了 `reportOnChange=true` 的点位在数值变化时按单条事件消息发送到这里
- `alarmTopic`
  - 告警 topic
- `statusTopic`
  - 驱动状态与运行事件 topic
- `commandRequestTopic`
  - 命令请求 topic
- `commandReplyTopic`
  - 命令回执 topic，当前回执的是“已入共享写队列/被拒绝”，不是设备最终执行结果
- `otaRequestTopic`
  - OTA 请求 topic
- `otaReplyTopic`
  - OTA 受理结果 topic
- `otaStatusTopic`
  - OTA 过程状态 topic，当前支持 `accepted/downloading/verifying/applying/completed/failed`
- `qos`
  - QoS
- `maxPayloadBytes`
  - 实时数据 `telemetry / snapshot` 单条 MQTT payload 目标上限，默认 `49152` 字节；超过后自动按 `machineCode + chunkId + chunkIndex + chunkCount` 分片发送到同一个 `telemetryTopic`
- `cleanSession`
  - 是否清会话
- `keepAliveSec`
  - 心跳秒数
- `sessionExpirySec`
  - MQTT5 会话过期秒数
- `offlineBuffer`
  - MQTT 断链本地缓冲配置。实时数据写入预分配 ring 文件；告警、变位、OTA 状态写入 SQLite outbox；连接恢复后先补发 outbox，再补发实时 ring。
- `offlineBuffer.enabled`
  - 是否启用断链缓冲
- `offlineBuffer.mode`
  - 当前使用 `ring`
- `offlineBuffer.realtimeFile`
  - 实时数据 ring 文件路径
- `offlineBuffer.realtimeFileSizeBytes`
  - 实时数据 ring 文件大小，默认 1GB
- `offlineBuffer.maxRealtimeMessageBytes`
  - 单条实时消息最大大小，默认 4MB
- `offlineBuffer.flushBatchSize`
  - ring 批量写入/补发控制参数
- `offlineBuffer.flushIntervalMs`
  - 刷盘间隔
- `offlineBuffer.replayBatchSize`
  - 实时 ring 恢复连接后单轮补传条数
- `offlineBuffer.eventOutbox.sqlitePath`
  - 关键事件 outbox SQLite 路径
- `offlineBuffer.eventOutbox.retentionMonths`
  - 已发送事件按自然月保留数量，默认 12；例如 2026-01 数据在 2027-02 清理
- `offlineBuffer.eventOutbox.cleanupIntervalHours`
  - outbox 清理检查间隔
- `offlineBuffer.eventOutbox.replayBatchSize`
  - outbox 恢复连接后单轮补传条数

### `mqttDriver`

MQTT 驱动运行参数。

字段：

- `enabled`
  - 是否启用驱动
- `sharedMemoryName`
  - 要读取的共享内存段
- `scanIntervalMs`
  - 扫描周期
- `fullUploadIntervalMs`
  - 全量上报周期
- `snapshotBacklogThreshold`
  - 待发送事件积压阈值；大于该值时可临时延后 full snapshot，优先补发事件
- `snapshotBackoffIntervalMs`
  - 事件积压时的 full snapshot 回退窗口；通常配合 `snapshotBacklogThreshold` 一起使用
- `eventReplayMaxBytes`
  - 单轮事件补发的最大字节预算；补发顺序按 `alarm` 优先于 `change`
- `publishFullOnStart`
  - 启动时是否立即发全量
- `publishAllOnFull`
  - 全量时是否发全部点
- `fullUploadIndexes`
  - 指定全量上传的点索引

### `alarmStore`

告警 SQLite 存储参数。

字段：

- `enabled`
  - 是否启用告警存盘
- `sqlitePath`
  - 告警 SQLite 路径
- `sqliteLibraryPath`
  - SQLite 动态库路径

### `realtime`

实时监测相关配置。

字段：

- `enabled`
  - 是否启用 realtime 配置段
- `telemetryTopic`
  - 实时遥测消费 topic
- `alarmTopic`
  - 实时告警消费 topic
- `statusTopic`
  - 实时状态消费 topic
- `maxLatestPoints`
  - latest 缓存上限建议值
- `trendBufferSize`
  - 趋势缓存点数
- `pushThrottleMs`
  - 推送节流时间

### `ota`

OTA 升级执行配置。

字段：

- `enabled`
  - 是否启用 OTA 执行链
- `currentVersion`
  - 当前版本号
- `artifactBaseUrl`
  - 制品基地址
- `downloadDir`
  - 下载目录
- `stagingDir`
  - 暂存目录
- `backupDir`
  - 回滚备份目录
- `packageType`
  - 包格式，如 `tar.gz`
- `applyScript`
  - 升级执行脚本
- `rollbackScript`
  - 升级失败回滚脚本
- `checksumRequired`
  - 是否强制校验 `sha256`
- `autoReboot`
  - 是否自动重启，当前只保留配置
- `retentionCount`
  - 历史下载包保留数量，超出后会清理最旧文件
- `statusReportIntervalSec`
  - 状态上报节奏，当前阶段状态为即时推送
- `upgradeTimeoutSec`
  - 升级超时秒数，当前在下载后和校验后检查是否超时
- `storage.provider`
  - `local` 或 `minio`
- `storage.presignExpireMinutes`
  - 临时链接有效期
- `storage.minio.*`
  - MinIO 连接参数

## 4. MQTT 消息样例文件

### `config/samples/messages/mqtt_command_request_example.json`

用于描述 MQTT 下发控制命令消息格式。

核心字段：

- `cmdId`
- `machineCode`
- `meterCode`
- `index`
- `pointCode`
- `value`
- `source`
- `ts`

命令结果现在还会带：

- `requestedValue`
- `verifyAttempted`
- `verifyPassed`

推荐发布到：

```text
edge/command/request
```

### `config/samples/messages/mqtt_ota_request_example.json`

用于描述 MQTT OTA 请求消息格式。

核心字段：

- `jobId`
- `machineCode`
- `artifactUrl`
- `version`
- `sha256`
- `size`
- `upgradeMode`
- `ts`

推荐发布到：

```text
edge/ota/request
```

### `config/samples/messages/mqtt_telemetry_chunk_example.json`

用于描述实时数据超过 `mqtt.maxPayloadBytes` 后的分片上报格式。

核心字段：

- `type`
- `machineCode`
- `chunked`
- `chunkId`
- `chunkIndex`
- `chunkCount`
- `meters[].meterCode`
- `meters[].values[]`

推荐发布到：

```text
edge/telemetry
```

处理规则：

- `chunkIndex` 从 `1` 开始。
- 同一批分片使用相同 `machineCode + chunkId`。
- 平台实时页面可以按片刷新，不必等待全量分片到齐。
- 如果业务需要完整快照，应收齐 `chunkCount` 片后再合并。

## 5. 当前示例里可直接用于控制测试的点

RTU 示例：

- `SLAVE0001` 的 `index=11001`
- `SLAVE0002` 的 `index=12001`

TCP 示例：

- `TCP_SLAVE0001` 的 `index=21001`
- `TCP_SLAVE0002` 的 `index=22001`

这些点已经补了完整 `write` 示例，可直接用 `pointctl write` 做控制联调。


### `config/samples/messages/mqtt_ota_status_example.json`

用于描述 OTA 过程状态消息。

推荐发布到：

```text
edge/ota/status
```

### `config/samples/messages/mqtt_status_started_example.json`

用于描述 MQTT Driver 启动状态消息。

推荐发布到：

```text
edge/status
```

### `config/samples/messages/mqtt_status_full_snapshot_example.json`

用于描述 MQTT Driver 完成一次全量扫描后的状态消息。

推荐发布到：

```text
edge/status
```

当前状态消息仅包含：

- `event = "full-snapshot"`
- `valueCount`
- `ts`

不再输出 `leaseCount`。

### `config/samples/messages/mqtt_alarm_event_example.json`

用于描述独立告警 topic 的事件消息。

推荐发布到：

```text
edge/alarm
```


## 6. OTA 脚本参数说明

当前 `applyScript` / `rollbackScript` 由 OTA 服务按固定顺序传参：

```text
$1 artifactPath
$2 version
$3 jobId
$4 backupDir
$5 stagingDir
```

参考文件：

- [ota-apply.sh](/D:/workspace/Embedded/Gateway-zk/deploy/ota-apply.sh)
- [ota-rollback.sh](/D:/workspace/Embedded/Gateway-zk/deploy/ota-rollback.sh)

- [mqtt_status_command_accepted_example.json](/D:/workspace/Embedded/Gateway-zk/config/samples/messages/mqtt_status_command_accepted_example.json)
- [mqtt_status_command_rejected_example.json](/D:/workspace/Embedded/Gateway-zk/config/samples/messages/mqtt_status_command_rejected_example.json)
- [mqtt_status_ota_rejected_example.json](/D:/workspace/Embedded/Gateway-zk/config/samples/messages/mqtt_status_ota_rejected_example.json)

### OTA 执行后本地文件

- `stagingDir/current_version.txt`
  - 保存升级前后版本、最后一次包路径、保留策略
- `stagingDir/upgrade_history.log`
  - 追加记录每次升级 jobId、版本切换和包路径

- `config/samples/messages/mqtt_status_modbus_collect_failed_example.json`
- `config/samples/messages/mqtt_status_modbus_writeback_failed_example.json`
- `config/samples/messages/mqtt_status_modbus_persist_flushed_example.json`
- `config/samples/messages/mqtt_command_result_success_example.json`
- `config/samples/messages/mqtt_command_result_failed_example.json`
- `config/samples/logs/ota_upgrade_history_failure_example.log`


### OTA 失败历史字段

`upgrade_history.log` 中失败记录额外包含：

- `result=failure`
- `stage`
- `rollbackAttempted`
- `rollbackSucceeded`
- `message`

- `config/samples/messages/mqtt_status_writeback_succeeded_example.json`
- `config/samples/messages/mqtt_status_writeback_skipped_example.json`
- `config/samples/messages/mqtt_ota_status_failed_example.json`
- `config/samples/logs/ota_upgrade_history_success_example.log`


---

## Gateway-zk/doc/app_config.md

# App Config

`device.json` 负责设备、从站、点位、共享内存、SQLite。

`app config` 负责网关级运行参数，例如 MQTT、实时订阅和 OTA。

当前支持的 `app config` 结构：

```json
{
  "deviceConfigFiles": [
    "config/runtime/devices/device_slave_ttySP1.json",
    "config/runtime/devices/device_slave_ttySP2.json"
  ],
  "mqtt": {
    "enabled": true,
    "protocolVersion": "mqtt5",
    "broker": "tcp://127.0.0.1:1883",
    "clientId": "modbus-gateway",
    "username": "",
    "password": "",
    "telemetryTopic": "edge/telemetry",
    "alarmTopic": "edge/alarm",
    "statusTopic": "edge/status",
    "commandRequestTopic": "edge/command/request",
    "commandReplyTopic": "edge/command/reply",
    "otaRequestTopic": "edge/ota/request",
    "otaReplyTopic": "edge/ota/reply",
    "otaStatusTopic": "edge/ota/status",
    "qos": 1,
    "cleanSession": true,
    "keepAliveSec": 60,
    "sessionExpirySec": 0
  },
  "mqttDriver": {
    "enabled": true,
    "sharedMemoryName": "gateway_point_store",
    "scanIntervalMs": 1000,
    "fullUploadIntervalMs": 60000,
    "publishFullOnStart": true,
    "publishAllOnFull": true,
    "fullUploadIndexes": []
  },
  "alarmStore": {
    "enabled": false,
    "sqlitePath": "alarm_events.db",
    "sqliteLibraryPath": ""
  },
  "realtime": {
    "enabled": true,
    "telemetryTopic": "edge/telemetry",
    "alarmTopic": "edge/alarm",
    "statusTopic": "edge/status",
    "maxLatestPoints": 100000,
    "trendBufferSize": 300,
    "pushThrottleMs": 200
  },
  "ota": {
    "enabled": true,
    "currentVersion": "1.0.0",
    "artifactBaseUrl": "https://example.com/releases",
    "downloadDir": "/opt/modbus-gateway/ota/downloads",
    "stagingDir": "/opt/modbus-gateway/ota/staging",
    "backupDir": "/opt/modbus-gateway/ota/backup",
    "packageType": "tar.gz",
    "applyScript": "/opt/modbus-gateway/bin/ota-apply.sh",
    "rollbackScript": "/opt/modbus-gateway/bin/ota-rollback.sh",
    "checksumRequired": true,
    "autoReboot": true,
    "retentionCount": 3,
    "statusReportIntervalSec": 5,
    "upgradeTimeoutSec": 900,
    "storage": {
      "provider": "local",
      "presignExpireMinutes": 60,
      "minio": {
        "endpoint": "http://127.0.0.1:9000",
        "accessKey": "minioadmin",
        "secretKey": "minioadmin",
        "bucket": "edge-ota",
        "basePath": "packages",
        "publicBaseUrl": ""
      }
    }
  }
}
```

样例文件：

- [mqtt-service.json](/D:/workspace/Embedded/Gateway-zk/config/runtime/apps/mqtt-service.json)

运行方式：

```bash
./build-aarch64/MqttDriver --app-config config/runtime/apps/mqtt-service.json
```

字段说明：

- `deviceConfigFiles`: 需要加载的一个或多个 `device.json` 路径。
- `mqtt.telemetryTopic`: 测点数据上传 topic。
- `mqtt.alarmTopic`: 告警事件上传 topic。
- `mqtt.statusTopic`: 状态类消息 topic，供实时监控或状态同步使用。
- `mqtt.commandRequestTopic`: 下行写入命令基础 topic。运行时会自动订阅 `<commandRequestTopic>/<machineCode>`，避免其他网关接收无关命令。
- `mqtt.commandReplyTopic`: 写入命令回执发布 topic。
- `mqtt.otaRequestTopic`: OTA 请求基础 topic。平台发布时会自动发送到 `<otaRequestTopic>/<machineCode>`，边端也只订阅自己的机器级 topic。
- `mqtt.otaReplyTopic`: OTA 受理结果发布 topic。
- `mqtt.otaStatusTopic`: OTA 过程状态发布 topic。
- `mqtt.systemMonitorRequestTopic`: 系统监测订阅基础 topic。运行时自动订阅 `<systemMonitorRequestTopic>/<machineCode>`。
- `mqtt.diagRequestTopic`: 远程诊断请求基础 topic。运行时自动订阅 `<diagRequestTopic>/<machineCode>`。
- `mqttDriver`: 共享内存扫描和 MQTT 上传驱动配置。
- `alarmStore`: 告警落库配置。
- `realtime`: 实时监测侧消费 topic 和内存缓存约束配置。
- `ota`: OTA 升级执行配置，包括本地目录、脚本、超时和对象存储参数。

说明：

- `commandRequestTopic / otaRequestTopic / systemMonitorRequestTopic / diagRequestTopic` 在 JSON 里都只保存基础 topic，不需要手工拼 `machineCode`。
- Java 平台发布下行请求时，会自动改为 `<baseTopic>/<machineCode>`。
- 边端 `MqttDriver / SystemMonitor` 也只会订阅自己 `machineCode` 对应的请求 topic。
- 这样 OTA、写命令、系统监测订阅、诊断命令都不会再广播给同 broker 上的全部网关。
- `ota.storage.provider`: 当前支持 `local` / `minio` 配置解析，并在 OTA 下载阶段参与制品地址解析。
- `ota.storage.minio.publicBaseUrl`: 对外直链基地址，留空则由上层按 endpoint/bucket 拼装。

当前 C++ 侧能力：

- 已支持读取独立 `app config`
- 已支持独立 `telemetry/alarm/status/command/ota` topic 配置
- 已支持 `realtime` 与 `ota` 配置段解析
- 已支持 MQTT 告警发布走 `alarmTopic`
- 已支持 OTA 下载、校验、安装、失败回滚和状态上报


---

## Gateway-zk/doc/mqtt_driver.md

# MQTT Driver

`MqttDriver` 是独立进程，不依赖 Modbus 串口。

同样适用于：

- `modbus_rtu`
- `modbus_tcp`
- `dlt645_2007`

职责：

- 从共享内存读取最新值
- 告警触发上传
- 定时全量上传
- 主动拉取指定点位并上传
- 发布运行状态到 `statusTopic`
- 订阅 MQTT 命令并写入共享待写队列
- 订阅 OTA 请求并执行升级脚本链路

配置文件：

- [mqtt-service.json](/D:/workspace/Embedded/Gateway-zk/config/runtime/apps/mqtt-service.json)

运行：

```bash
./build-aarch64/MqttDriver --app-config config/runtime/apps/mqtt-service.json
```

一次性主动拉取：

```bash
./build-aarch64/MqttDriver --app-config config/runtime/apps/mqtt-service.json --once --index 11000 --index 12000
```

当前状态：

- 已经拆成独立驱动进程
- 已经从共享内存读取数据
- 已经支持定时全量、阈值告警、一次性主动拉取
- 已经支持独立 `telemetryTopic / changeEventTopic / alarmTopic / statusTopic`
- MQTT 版本支持配置切换：`mqtt3` / `mqtt5`
- 已内置 MQTT 客户端实现，不依赖板子额外安装库
- 已支持订阅 `commandRequestTopic` 并将命令写入共享内存 `pendingWrites`
- 已支持发布命令回执到 `commandReplyTopic`
- 已支持接收 `otaRequestTopic` 并执行 OTA 脚本链路
- 已支持按阶段发布 `otaStatusTopic`
- 实时数据、全量快照、告警事件已包含 `machineCode / meterCode / pointCode`，消费端无需依赖本地 device 配置反查
- 已支持点位级 `reportOnChange=true` 的变位事件上报；首次扫描只建立基线，后续仅在数值真正变化时立即发送单条 `change` 事件到 `changeEventTopic`，其余点位仍按定时策略发送

版本配置示例：

```json
{
  "mqtt": {
    "enabled": true,
    "protocolVersion": "mqtt5",
    "broker": "tcp://127.0.0.1:1883",
    "clientId": "mqtt-driver-01",
    "cleanSession": true,
    "keepAliveSec": 60,
    "sessionExpirySec": 0,
    "telemetryTopic": "edge/telemetry",
    "changeEventTopic": "edge/event/change",
    "alarmTopic": "edge/alarm",
    "statusTopic": "edge/status",
    "commandRequestTopic": "edge/command/request",
    "commandReplyTopic": "edge/command/reply",
    "otaRequestTopic": "edge/ota/request",
    "otaReplyTopic": "edge/ota/reply",
    "otaStatusTopic": "edge/ota/status",
    "qos": 1,
    "maxPayloadBytes": 49152,
    "offlineBuffer": {
      "enabled": true,
      "mode": "ring",
      "dir": "/opt/modbus-gateway/data/mqtt-spool",
      "realtimeFile": "/opt/modbus-gateway/data/mqtt-spool/realtime_ring.dat",
      "realtimeFileSizeBytes": 1073741824,
      "maxRealtimeMessageBytes": 4194304,
      "maxMemoryMessages": 200,
      "flushBatchSize": 10,
      "flushIntervalMs": 5000,
      "replayBatchSize": 20,
      "maxDiskBytes": 33554432,
      "eventOutbox": {
        "sqlitePath": "/opt/modbus-gateway/data/mqtt_event_outbox.db",
        "sqliteLibraryPath": "",
        "retentionMonths": 12,
        "cleanupIntervalHours": 24,
        "replayBatchSize": 100
      }
    }
  },
  "realtime": {
    "enabled": true,
    "telemetryTopic": "edge/telemetry",
    "alarmTopic": "edge/alarm",
    "statusTopic": "edge/status",
    "maxLatestPoints": 100000,
    "trendBufferSize": 300,
    "pushThrottleMs": 200
  },
  "ota": {
    "enabled": true,
    "downloadDir": "/opt/modbus-gateway/ota/downloads",
    "stagingDir": "/opt/modbus-gateway/ota/staging",
    "backupDir": "/opt/modbus-gateway/ota/backup",
    "applyScript": "/opt/modbus-gateway/bin/ota-apply.sh",
    "rollbackScript": "/opt/modbus-gateway/bin/ota-rollback.sh",
    "checksumRequired": true,
    "upgradeTimeoutSec": 900
  }
}
```

说明：

- `protocolVersion` 只允许 `mqtt3` 或 `mqtt5`
- 当前内置实现直接编译进驱动，不依赖额外动态库
- 当前实现支持 TCP 直连、用户名密码、QoS0/QoS1、MQTT 3.1.1 和 MQTT 5
- 当前实现暂不支持 TLS
- `statusTopic` 当前用于发布驱动启动事件、全量快照事件和 OTA 过程状态
- `maxPayloadBytes` 当前用于实时数据 `telemetry / snapshot` 分片，默认 `49152` 字节，避免超过 broker 单条报文大小限制
- 实时页面消费直接订阅 `mqtt.telemetryTopic / mqtt.alarmTopic / mqtt.statusTopic`，Java 端不需要额外单独配置 `realtime.*Topic`；边端数据发送周期仍由 `mqttDriver` 的定时策略控制
- OTA 执行链依赖目标环境具备脚本、下载工具和校验命令

## MQTT 离线缓冲

主站或 broker 断链时，`MqttDriver` 按数据类型分开缓存：

- 实时数据写入预分配 ring 文件：`/opt/modbus-gateway/data/mqtt-spool/realtime_ring.dat`
- 告警、变位、OTA 状态写入 SQLite outbox：`/opt/modbus-gateway/data/mqtt_event_outbox.db`

连接恢复后，驱动每次发送前会优先补发 SQLite 关键事件，再补发实时 ring 数据，最后发送当前新消息。

字段说明：

- `enabled`：是否启用断链缓冲。
- `mode`：当前使用 `ring`。
- `realtimeFile`：实时数据 ring 文件。
- `realtimeFileSizeBytes`：ring 文件大小，当前建议 1GB。
- `maxRealtimeMessageBytes`：单条实时消息最大大小，当前建议 4MB。
- `replayBatchSize`：实时 ring 每轮最多补传条数。
- `eventOutbox.sqlitePath`：关键事件 outbox 数据库路径。
- `eventOutbox.retentionMonths`：已发送事件自然月保留数，默认 12。
- `eventOutbox.cleanupIntervalHours`：清理检查间隔。
- `eventOutbox.replayBatchSize`：关键事件每轮最多补传条数。

详细设计见 [mqtt_offline_replay_design.md](D:/workspace/Embedded/Gateway-zk/doc/mqtt_offline_replay_design.md)。

## 实时消息结构

实时点值 `telemetryTopic`：

```json
{
  "type": "telemetry",
  "machineCode": "GW0001",
  "meters": [
    {
      "meterCode": "SLAVE0001",
      "values": [
        {
          "index": 11000,
          "pointCode": "reg_0",
          "value": 2400,
          "quality": 1,
          "ts": 1776155906276,
          "expireAt": 1776156506276,
          "stale": false
        }
      ]
    }
  ]
}
```

全量快照 `telemetryTopic`：

```json
{
  "type": "snapshot",
  "machineCode": "GW0001",
  "meters": [
    {
      "meterCode": "SLAVE0001",
      "values": [
        {
          "index": 11000,
          "pointCode": "reg_0",
          "value": 2400,
          "quality": 1,
          "ts": 1776155906276,
          "expireAt": 1776156506276,
          "stale": false
        }
      ]
    }
  ]
}
```

实时数据分片：

当单次 `telemetry / snapshot` 预计超过 `mqtt.maxPayloadBytes` 时，`MqttDriver` 会自动拆成多条 MQTT 消息，仍发布到 `telemetryTopic`。

```json
{
  "type": "snapshot",
  "machineCode": "GW0001",
  "chunked": true,
  "chunkId": "1776906000000-1",
  "chunkIndex": 1,
  "chunkCount": 3,
  "meters": [
    {
      "meterCode": "TTYSP1_SLAVE0001",
      "values": [
        {
          "index": 31101,
          "pointCode": "reg_1",
          "value": 123,
          "quality": 1,
          "ts": 1776906000000,
          "expireAt": 1776906600000,
          "stale": false
        }
      ]
    }
  ]
}
```

字段说明：

- `chunked=true`：表示这是分片实时数据；未分片消息可没有该字段。
- `chunkId`：同一次全量或主动拉取的分片批次 ID。
- `chunkIndex`：当前第几片，从 `1` 开始。
- `chunkCount`：本批次总片数。
- `meters`：本片包含的仪表和点值，结构与未分片实时数据一致。

平台处理建议：

- 实时页面可以收到一片就刷新一片，不必等待所有分片到齐。
- 如果平台需要恢复完整快照，按 `machineCode + chunkId` 聚合，收到 `chunkCount` 片后再合并。
- 分片可能因网络和补传乱序到达，不能依赖 MQTT 接收顺序。
- `maxPayloadBytes` 是分片目标上限，不建议配置太接近 broker 极限；broker 限制 50KB 时建议保留默认 `49152` 或更小。

样例文件：

- [mqtt_telemetry_chunk_example.json](/D:/workspace/Embedded/Gateway-zk/config/samples/messages/mqtt_telemetry_chunk_example.json)

专用在线状态点：

- `read.dataType = "device_online"`
- 不参与寄存器读取规划
- 某个从站本轮采集成功时写入 `1`
- 某个从站本轮采集异常时写入 `0`
- 建议分类使用 `status`
- 建议开启 `reportOnChange`，用于上下线事件上传

告警事件 `alarmTopic`：

```json
{
  "type": "alarm",
  "machineCode": "GW0001",
  "meterCode": "SLAVE0001",
  "index": 11000,
  "pointCode": "reg_0",
  "alarmType": "high",
  "active": true,
  "value": 2400,
  "quality": 1,
  "ts": 1776155906276,
  "stale": false
}
```

变位事件 `changeEventTopic`：

```json
{
  "type": "change",
  "machineCode": "GW0001",
  "meterCode": "SLAVE0001",
  "index": 11000,
  "pointCode": "reg_0",
  "value": 1,
  "quality": 1,
  "ts": 1776155906276,
  "expireAt": 1776156506276,
  "stale": false
}
```

## 命令下发协议

建议 topic：

```text
edge/command/request
edge/command/reply
```

多网关场景可扩展为：

```text
edge/{machineCode}/command/request
edge/{machineCode}/command/reply
```

推荐请求 JSON：

```json
{
  "cmdId": "CMD2026041415300001",
  "machineCode": "GW0001",
  "meterCode": "SLAVE0001",
  "index": 11001,
  "pointCode": "reg_1",
  "value": 1,
  "source": "mqtt",
  "ts": 1776155909818
}
```

推荐回执 JSON：

```json
{
  "cmdId": "CMD2026041415300001",
  "machineCode": "GW0001",
  "meterCode": "SLAVE0001",
  "pointCode": "reg_1",
  "index": 11001,
  "success": true,
  "message": "ok",
  "ts": 1776155910123,
  "requestedValue": 1,
  "verifyAttempted": true,
  "verifyPassed": true
}
```

## OTA 协议

建议 topic：

```text
edge/ota/request
edge/ota/reply
edge/ota/status
```

推荐 OTA 请求 JSON：

```json
{
  "jobId": "OTA2026041416000001",
  "machineCode": "GW0001",
  "artifactUrl": "https://example.com/releases/modbus-gateway-1.2.3.tar.gz",
  "version": "1.2.3",
  "sha256": "7d6f0d2d7c9f1d4d8b0c7b8e5a0c4e4b8d2c8a8d7f4e0a1b2c3d4e5f6a7b8c9d",
  "size": 10485760,
  "upgradeMode": "download_install",
  "ts": 1776156000000
}
```

立即回执 JSON：

```json
{
  "jobId": "OTA2026041416000001",
  "machineCode": "GW0001",
  "accepted": true,
  "message": "accepted",
  "ts": 1776156000100
}
```

状态上报 JSON：

```json
{
  "jobId": "OTA2026041416000001",
  "machineCode": "GW0001",
  "stage": "verifying",
  "progress": 40,
  "message": "verifying artifact",
  "ts": 1776156005100
}
```

当前 OTA 执行链：

```text
OTA请求 -> 参数校验 -> accepted回执 -> 下载 -> 校验 -> applyScript -> 失败时rollbackScript -> 阶段状态上报
```

限制：

- 当前已写出 `current_version.txt` 与 `upgrade_history.log`，并按 `retentionCount` 清理旧下载包；进程级重启编排仍未实现
- `upgradeTimeoutSec` 当前已在下载后和校验后做阶段超时检查
- `storage.provider=minio` 当前已支持按 `publicBaseUrl` 或 `endpoint/bucket/basePath` 组合生成下载地址


## OTA 脚本协议

当前 OTA 执行器调用脚本时，参数顺序固定如下：

```text
1. artifactPath
2. version
3. jobId
4. backupDir
5. stagingDir
```

示例：

```bash
sh /opt/modbus-gateway/bin/ota-apply.sh   /opt/modbus-gateway/ota/downloads/1.2.3.tar.gz   1.2.3   OTA2026041519000001   /opt/modbus-gateway/ota/backup   /opt/modbus-gateway/ota/staging
```

参考脚本：

- [ota-apply.sh](/D:/workspace/Embedded/Gateway-zk/deploy/ota-apply.sh)
- [ota-rollback.sh](/D:/workspace/Embedded/Gateway-zk/deploy/ota-rollback.sh)

脚本约定：

- 返回 `0` 表示成功
- 非 `0` 表示失败，失败时 OTA 服务会尝试调用 `rollbackScript`
- 当前 OTA 服务不解析脚本 stdout/stderr，只依据退出码判断结果
- 如果需要更细粒度进度，请在脚本内部自行记录日志或写状态文件

- [mqtt_status_command_accepted_example.json](/D:/workspace/Embedded/Gateway-zk/config/samples/messages/mqtt_status_command_accepted_example.json)
- [mqtt_status_command_rejected_example.json](/D:/workspace/Embedded/Gateway-zk/config/samples/messages/mqtt_status_command_rejected_example.json)
- [mqtt_status_ota_rejected_example.json](/D:/workspace/Embedded/Gateway-zk/config/samples/messages/mqtt_status_ota_rejected_example.json)

## Status Topic 事件

当前 `edge/status` 已同时承载两个服务的运行事件：

- `service=mqtt-driver`
  - `started`
  - `full-snapshot`
  - `on-demand`
  - `command-accepted`
  - `command-rejected`
  - `ota-handled`
  - `ota-rejected`
  - `alarm-persisted`
- `service=modbus-daemon`
  - `started`
  - `collect-failed`
  - `writeback-skipped`
  - `writeback-failed`
  - `writeback-succeeded`
  - `persist-flushed`

新增样例：

- [mqtt_status_modbus_collect_failed_example.json](/D:/workspace/Embedded/Gateway-zk/config/samples/messages/mqtt_status_modbus_collect_failed_example.json)
- [mqtt_status_modbus_writeback_failed_example.json](/D:/workspace/Embedded/Gateway-zk/config/samples/messages/mqtt_status_modbus_writeback_failed_example.json)
- [mqtt_status_modbus_persist_flushed_example.json](/D:/workspace/Embedded/Gateway-zk/config/samples/messages/mqtt_status_modbus_persist_flushed_example.json)

命令结果样例：

- [mqtt_command_result_success_example.json](/D:/workspace/Embedded/Gateway-zk/config/samples/messages/mqtt_command_result_success_example.json)
- [mqtt_command_result_failed_example.json](/D:/workspace/Embedded/Gateway-zk/config/samples/messages/mqtt_command_result_failed_example.json)

OTA 失败历史样例：

- [ota_upgrade_history_failure_example.log](/D:/workspace/Embedded/Gateway-zk/config/samples/logs/ota_upgrade_history_failure_example.log)

更多状态样例：

- [mqtt_status_writeback_succeeded_example.json](/D:/workspace/Embedded/Gateway-zk/config/samples/messages/mqtt_status_writeback_succeeded_example.json)
- [mqtt_status_writeback_skipped_example.json](/D:/workspace/Embedded/Gateway-zk/config/samples/messages/mqtt_status_writeback_skipped_example.json)
- [mqtt_ota_status_failed_example.json](/D:/workspace/Embedded/Gateway-zk/config/samples/messages/mqtt_ota_status_failed_example.json)
- [ota_upgrade_history_success_example.log](/D:/workspace/Embedded/Gateway-zk/config/samples/logs/ota_upgrade_history_success_example.log)


---

## Gateway-zk/doc/mqtt_offline_replay_design.md

# MQTT 断链缓存与补传设计

## 1. 目标

边端 `MqttDriver` 与主站 MQTT broker 断链后，需要保证关键数据不丢，并且避免频繁擦写边端硬盘。

本设计按数据类型拆分存储：

- 实时数据：体积大，允许覆盖旧数据，写入预分配环形文件。
- 告警、变位、OTA 状态：体积小，事件发生时间重要，写入 SQLite outbox。
- 普通运行状态：默认不作为关键数据长期缓存。

## 2. 数据分类

### 2.1 实时数据

来源：

- `publishFullSnapshot`
- `publishOnDemand`
- MQTT topic 通常是 `edge/telemetry`

特点：

- 单条可能达到 2-3 MB。
- 数据量大，断链时间长时可能快速占满磁盘。
- 最新数据价值高于旧数据。

处理方式：

- 写入 1GB 预分配 ring 文件。
- 满时按记录粒度覆盖最旧实时数据。

### 2.2 关键事件

来源：

- `publishAlarm`
- `publishChangeEvent`
- `publishOtaStatus`

特点：

- 单条体积小。
- 发生时间、发生顺序、是否补发成功很重要。
- 不应该被实时全量数据挤掉。

处理方式：

- 写入 SQLite 表 `mqtt_event_outbox`。
- 恢复连接后按 `event_ts ASC, id ASC` 补发。
- 只清理已成功发送且超过保留期的数据。

## 3. 配置

配置位于 `mqtt.offlineBuffer`：

```json
{
  "offlineBuffer": {
    "enabled": true,
    "mode": "ring",
    "dir": "/opt/modbus-gateway/data/mqtt-spool",
    "realtimeFile": "/opt/modbus-gateway/data/mqtt-spool/realtime_ring.dat",
    "realtimeFileSizeBytes": 1073741824,
    "maxRealtimeMessageBytes": 4194304,
    "maxMemoryMessages": 200,
    "flushBatchSize": 10,
    "flushIntervalMs": 5000,
    "replayBatchSize": 20,
    "maxDiskBytes": 33554432,
    "eventOutbox": {
      "sqlitePath": "/opt/modbus-gateway/data/mqtt_event_outbox.db",
      "sqliteLibraryPath": "",
      "retentionMonths": 12,
      "cleanupIntervalHours": 24,
      "replayBatchSize": 100
    }
  }
}
```

字段说明：

- `realtimeFile`：实时数据 ring 文件路径。
- `realtimeFileSizeBytes`：ring 文件大小，当前建议 1GB。
- `maxRealtimeMessageBytes`：单条实时消息最大大小，当前建议 4MB。
- `replayBatchSize`：实时数据每轮最多补传条数。
- `eventOutbox.sqlitePath`：事件 outbox 数据库。
- `eventOutbox.retentionMonths`：已发送事件保留自然月数，默认 12。
- `eventOutbox.cleanupIntervalHours`：清理任务检查间隔。
- `eventOutbox.replayBatchSize`：事件每轮最多补传条数。

## 4. 实时 Ring 文件

文件：

```text
/opt/modbus-gateway/data/mqtt-spool/realtime_ring.dat
```

结构：

```text
header 4096 bytes
data area variable records
```

header 保存：

- magic
- version
- fileSize
- readOffset
- writeOffset
- usedBytes
- recordCount
- nextSeq

record 保存：

- magic
- totalLen
- topicLen
- payloadLen
- seq
- createdAt
- topic
- payload

写入规则：

- 写入失败时不影响采集进程。
- 单条超过 `maxRealtimeMessageBytes` 时丢弃。
- 文件剩余空间不足时推进 `readOffset`，丢弃最旧记录。
- 文件尾部空间不足时写 wrap marker，从 data 起点继续写。

补发规则：

- MQTT 可用时优先读取 `readOffset` 处最旧记录。
- 发送成功后推进 `readOffset`。
- 发送失败时停止本轮补发，等待下次扫描。

容量估算：

```text
1GB / 3MB ~= 341 条
1GB / 2MB ~= 512 条
```

如果 1 秒 1 条且每条 3MB，1GB 大约只能缓存 5 分钟。

## 5. SQLite Event Outbox

表结构：

```sql
CREATE TABLE IF NOT EXISTS mqtt_event_outbox (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  event_type TEXT NOT NULL,
  topic TEXT NOT NULL,
  payload TEXT NOT NULL,
  event_ts INTEGER NOT NULL,
  event_month TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  sent INTEGER NOT NULL DEFAULT 0,
  sent_at INTEGER,
  retry_count INTEGER NOT NULL DEFAULT 0,
  last_error TEXT
);

CREATE INDEX IF NOT EXISTS idx_mqtt_event_outbox_pending
ON mqtt_event_outbox(sent, event_ts, id);

CREATE INDEX IF NOT EXISTS idx_mqtt_event_outbox_cleanup
ON mqtt_event_outbox(sent, event_month);
```

写入规则：

- 告警、变位、OTA 状态先写 outbox。
- 写入成功后立即尝试 MQTT 发送。
- 发送成功则标记 `sent=1`。
- 发送失败则保留 `sent=0`，等待恢复后补发。

补发规则：

```sql
SELECT id, topic, payload
FROM mqtt_event_outbox
WHERE sent = 0
ORDER BY event_ts ASC, id ASC
LIMIT ?;
```

发送成功：

```sql
UPDATE mqtt_event_outbox
SET sent = 1, sent_at = ?
WHERE id = ?;
```

## 6. 一年自然月清理规则

清理只针对已发送事件：

```sql
DELETE FROM mqtt_event_outbox
WHERE sent = 1
  AND event_month < :beforeMonth;
```

`beforeMonth` 计算规则：

```text
当前月份往前推 retentionMonths 个自然月
```

示例：

```text
当前时间：2027-02
retentionMonths：12
beforeMonth：2026-02
可删除：event_month < 2026-02
也就是 2026-01 及更早
```

因此：

- 2026-01 的数据在 2027-02 清理。
- 2026-02 的数据在 2027-03 清理。
- 未发送的 `sent=0` 数据不会按时间清理。

## 7. 恢复顺序

每次 MQTT 发布前会尝试补发：

1. SQLite outbox 关键事件。
2. 实时 ring 文件。
3. 当前新消息。

这样可以优先保证告警、变位、OTA 状态先补发。

## 8. 当前实现边界

- 实时 ring 满时丢弃最旧实时数据。
- SQLite outbox 不主动删除未发送数据。
- 如果 SQLite 不可用，事件会尝试直接 MQTT 发送，但断链时无法持久化。
- ring 文件初始化时会预分配到配置大小，1GB 文件会立即占用文件系统空间。


---

## Gateway-zk/doc/config_to_ota_delivery.md

# Driver Config To OTA Delivery Design

## 1. Goal

当前已经具备以下能力：

- Java/Vue 图形化生成设备配置
- Java/Vue 图形化生成 MQTT App 配置
- C++ 边端驱动读取本地 JSON 配置
- MQTT Driver 支持 OTA 请求、回执和状态上报
- 边端 OTA 脚本支持下载后执行

但当前缺失一条完整链路：

`Java 生成配置文件 -> 打包成 OTA 制品 -> 生成下载链接 -> MQTT 下发 OTA -> 边端下载并应用配置`

本文件定义这条链路的中间流程和落地逻辑。

## 2. Current Gap

目前的问题不是某个单点能力缺失，而是流程没有串起来。

### 2.1 Java 侧缺口

Java 现在可以：

- 保存设备配置 JSON
- 下载设备配置 JSON
- 生成多串口配置 ZIP
- 上传 OTA 包
- 发布 OTA 请求

但还不能直接：

- 将当前驱动配置打成 OTA 配置包
- 自动生成配置包版本号
- 自动上传配置包到文件服务或 MinIO
- 自动拿到 `artifactUrl`
- 自动填充 OTA 请求
- 自动发布“配置更新 OTA”

### 2.2 OTA 包语义缺口

当前 OTA 包更偏向“程序升级包”，但驱动配置更新需要一种明确包类型。

建议新增 OTA 包类型：

- `config`
- `binary`
- `full`

含义：

- `config`
  - 只更新配置文件，不替换二进制
- `binary`
  - 只替换程序二进制
- `full`
  - 同时更新二进制和配置

### 2.3 边端脚本能力

当前 `ota-apply.sh` 已标准化处理配置目录、服务管理文件和重启流程。

配置 OTA 会明确：

- 包内配置文件放在哪里
- 解包后复制到哪里
- 是否备份旧配置
- 是否重启对应驱动
- 如何回滚配置

## 3. Target Flow

目标流程如下：

```text
Java 页面
  -> 生成驱动配置
  -> 生成 app 配置
  -> 生成标准点表
  -> 打包 config OTA
  -> 上传到文件服务/MinIO
  -> 生成 artifactUrl
  -> 发布 MQTT OTA request
  -> 边端 MqttDriver 收到请求
  -> 下载 config OTA 包
  -> 校验 sha256
  -> 执行 ota-apply.sh
  -> 备份旧配置
  -> 替换新配置
  -> 可选重启驱动
  -> 上报 OTA 状态
```

## 4. Package Types

### 4.1 Config Package

配置包用于只更新配置。

建议文件名：

```text
gateway-config-${machineCode}-${version}.tar.gz
```

包内结构：

```text
config/
  runtime/
    devices/
      device_dlt645_multi_meter_1_2.json
      device_slave_ttySP1.json
    apps/
      mqtt-service.json
  templates/
    dlt645_2007_standard_points.json
manifest.json
```

### 4.2 Binary Package

二进制包用于更新程序。

包内结构：

```text
bin/
  ModbusRtu
  Dlt645Driver
  MqttDriver
  pointctl
manifest.json
```

### 4.3 Full Package

完整包用于同时更新程序和配置。

包内结构：

```text
bin/
  ModbusRtu
  Dlt645Driver
  MqttDriver
  pointctl
config/
  runtime/
  templates/
manifest.json
```

## 5. Manifest Design

每个 OTA 包必须包含 `manifest.json`。

示例：

```json
{
  "packageType": "config",
  "version": "2026.04.21.001",
  "machineCode": "GW0001",
  "generatedAt": 1776740000000,
  "files": [
    {
      "path": "config/runtime/devices/device_dlt645_multi_meter_1_2.json",
      "target": "/opt/modbus-gateway/config/runtime/devices/device_dlt645_multi_meter_1_2.json",
      "sha256": ""
    },
    {
      "path": "config/runtime/apps/mqtt-service.json",
      "target": "/opt/modbus-gateway/config/runtime/apps/mqtt-service.json",
      "sha256": ""
    },
    {
      "path": "config/templates/dlt645_2007_standard_points.json",
      "target": "/opt/modbus-gateway/config/templates/dlt645_2007_standard_points.json",
      "sha256": ""
    }
  ],
  "restart": {
    "enabled": true,
    "services": [
      "gateway-services.service"
    ]
  },
  "rollback": {
    "enabled": true
  }
}
```

说明：

- `path`
  - 包内相对路径
- `target`
  - 边端目标路径
- `restart.services`
  - 配置更新后需要重启的服务
- `rollback.enabled`
  - 是否备份旧文件以支持回滚

## 6. Java Side Required Logic

### 6.1 New Button

在 Java 页面增加按钮：

- `生成配置 OTA 包`
- `生成并发布配置 OTA`

建议放置位置：

- `DLT645 配置` Tab
- `协议设备配置` Tab
- `MQTT/OTA` Tab 的 OTA 包管理区域

### 6.2 Config OTA Builder

Java 后端新增服务：

```text
ConfigOtaPackageService
```

职责：

- 收集当前设备配置文件
- 收集当前 App 配置文件
- 收集标准点表文件
- 生成 `manifest.json`
- 打包为 `tar.gz`
- 计算 SHA256
- 生成包记录

输入：

```json
{
  "machineCode": "GW0001",
  "version": "2026.04.21.001",
  "deviceConfigFiles": [
    "runtime/devices/device_dlt645_multi_meter_1_2.json"
  ],
  "appConfigFile": "runtime/apps/mqtt-service.json",
  "includeTemplates": true,
  "includeServiceManager": true,
  "restartServices": [
    "gateway-services.service"
  ]
}
```

输出：

```json
{
  "fileName": "gateway-config-GW0001-2026.04.21.001.tar.gz",
  "packageType": "config",
  "version": "2026.04.21.001",
  "sha256": "...",
  "size": 12345,
  "artifactUrl": "http://.../gateway-config-GW0001-2026.04.21.001.tar.gz"
}
```

### 6.3 Upload Provider

生成包后需要上传或暴露下载链接。

支持两种模式：

#### Local HTTP

Java 将包保存到：

```text
config/gateway-otapackage/
```

并通过 Spring Boot 提供下载接口：

```text
GET /api/config/ota/packages/download/{fileName}
```

生成的 `artifactUrl`：

```text
http://platform-ip:port/api/config/ota/packages/download/gateway-config-GW0001-2026.04.21.001.tar.gz
```

#### MinIO

Java 上传到 MinIO 后生成临时链接：

```text
http://minio-host:9000/bucket/path/file?X-Amz-...
```

### 6.4 OTA Request Auto Fill

生成配置 OTA 包后，Java 自动填充：

```json
{
  "jobId": "CONFIG_OTA_2026042110300001",
  "machineCode": "GW0001",
  "artifactUrl": "http://...",
  "version": "2026.04.21.001",
  "sha256": "...",
  "size": 12345,
  "upgradeMode": "download_install",
  "packageType": "config"
}
```

然后发布到：

```text
edge/ota/request
```

## 7. Edge Side Required Logic

### 7.1 MqttDriver

MqttDriver 当前已经可以：

- 接收 `otaRequestTopic`
- 下载包
- 校验 SHA256
- 执行 `applyScript`
- 发布 `otaReplyTopic`
- 发布 `otaStatusTopic`

需要确认：

- `packageType=config` 能原样传给脚本或写入状态
- `upgradeMode=download_install` 时会执行 `ota-apply.sh`

### 7.2 ota-apply.sh

配置 OTA 的 apply 脚本必须做：

1. 解包
2. 读取 `manifest.json`
3. 备份目标文件
4. 拷贝新配置到目标路径
5. 可选重启服务
6. 写入当前版本状态

目标路径示例：

```text
/opt/modbus-gateway/config/runtime/devices/
/opt/modbus-gateway/config/runtime/apps/
/opt/modbus-gateway/config/templates/
```

备份路径示例：

```text
/opt/modbus-gateway/ota/backup/CONFIG_OTA_2026042110300001/config/...
```

### 7.3 ota-rollback.sh

配置 OTA 的 rollback 脚本必须做：

1. 读取当前 jobId 的备份目录
2. 将旧配置复制回目标路径
3. 可选重启服务
4. 记录回滚状态

当前项目中的目标实现已经调整为：

- 优先从 `current_version.txt` 读取上一次 apply 的 `workDir`
- 如果存在 `workDir/restart_services.txt`
  - rollback 后会按该文件重启服务
- 如果存在 `backupDir/opt/modbus-gateway/...`
  - rollback 会将这些文件恢复回 `/opt/modbus-gateway/...`

## 8. Java API Proposal

### 8.1 Build Config OTA Package

```http
POST /api/config/ota/config-package/build
```

Request:

```json
{
  "machineCode": "GW0001",
  "version": "2026.04.21.001",
  "deviceConfigFiles": [
    "runtime/devices/device_dlt645_multi_meter_1_2.json"
  ],
  "appConfigFile": "runtime/apps/mqtt-service.json",
  "includeTemplates": true,
  "includeServiceManager": true,
  "restartServices": [
    "gateway-services.service"
  ]
}
```

Response:

```json
{
  "success": true,
  "packageType": "config",
  "fileName": "gateway-config-GW0001-2026.04.21.001.tar.gz",
  "sha256": "...",
  "size": 12345,
  "artifactUrl": "http://..."
}
```

### 8.2 Build And Publish Config OTA

```http
POST /api/config/ota/config-package/publish
```

Request:

```json
{
  "machineCode": "GW0001",
  "version": "2026.04.21.001",
  "deviceConfigFiles": [
    "runtime/devices/device_dlt645_multi_meter_1_2.json"
  ],
  "appConfigFile": "runtime/apps/mqtt-service.json",
  "includeTemplates": true,
  "includeServiceManager": true,
  "upgradeMode": "download_install"
}
```

Response:

```json
{
  "success": true,
  "request": {
    "jobId": "CONFIG_OTA_2026042110300001",
    "machineCode": "GW0001",
    "artifactUrl": "http://...",
    "version": "2026.04.21.001",
    "sha256": "...",
    "size": 12345,
    "upgradeMode": "download_install",
    "packageType": "config"
  }
}
```

## 9. Frontend Proposal

新增一个区域：

```text
配置 OTA 下发
```

字段：

- `machineCode`
- `version`
- `deviceConfigFiles`
- `appConfigFile`
- `includeTemplates`
- `includeServiceManager`
- `restartServices`
- `artifactUrl`
- `sha256`
- `upgradeMode`

按钮：

- `生成配置 OTA 包`
- `生成并发布 OTA`
- `下载配置 OTA 包`
- `查看发布记录`

补充能力：

- 上传 `dlt645_2007_standard_points.json`
- 下载当前运行目录中的标准点表
- 在生成前检查标准点表是否已就绪

## 10. Minimal Test Flow

### Step 1. Java 保存配置

保存：

- DLT645 device config
- MQTT app config
- standard points template

### Step 2. Java 生成 Config OTA

点击：

```text
生成配置 OTA 包
```

得到：

- `gateway-config-GW0001-xxx.tar.gz`
- `sha256`
- `artifactUrl`

### Step 3. Java 发布 OTA

点击：

```text
生成并发布 OTA
```

### Step 4. 边端接收

边端 `MqttDriver` 收到：

```text
edge/ota/request
```

### Step 5. 边端应用配置

执行：

```text
/opt/modbus-gateway/bin/ota-apply.sh
```

### Step 6. 验证

检查：

```bash
ls -l /opt/modbus-gateway/config/runtime/devices
ls -l /opt/modbus-gateway/config/runtime/apps
ls -l /opt/modbus-gateway/config/templates
cat /opt/modbus-gateway/ota/staging/upgrade_history.log
```

订阅：

```bash
mosquitto_sub -h broker -t edge/ota/reply -v
mosquitto_sub -h broker -t edge/ota/status -v
```

## 11. Implementation Phases

### Phase 1

目标：先打通配置包生成和下载。

- Java 生成 config OTA tar.gz
- 包内包含 config 和 manifest
- Java 提供本地 HTTP 下载链接

### Phase 2

目标：打通 MQTT OTA 发布。

- Java 一键发布 config OTA request
- 边端下载包
- 边端执行 apply script
- 状态上报

### Phase 3

目标：完善边端应用配置。

- `ota-apply.sh` 读取 manifest
- 自动备份旧配置
- 自动替换新配置
- 自动下发服务管理文件
- 自动 `systemctl daemon-reload`
- 自动重启 `gateway-services.service`，由它按当前配置扫描并启动实际需要的驱动

### Phase 4

目标：完善回滚。

- `ota-rollback.sh` 读取备份
- 恢复配置
- 重启服务
- 状态上报

## 12. Key Decisions

建议采用：

- Java 负责生成配置包
- Java 负责生成下载链接
- MQTT 只传 OTA 请求，不直接传大文件
- 边端只按 `artifactUrl` 下载包
- 配置更新和程序更新都走同一 OTA 状态链路

这样优点是：

- 配置和程序升级流程统一
- 边端不需要访问 Java 本地文件路径
- 可复用 MinIO 临时链接
- 可复用现有 OTA 回执和状态页面


---

## Gateway-zk/doc/config_ota_e2e_test.md

# Config OTA End-To-End Test Guide

## 1. Goal

本文用于验证以下完整链路：

```text
Java 页面生成驱动配置
-> Java 页面生成配置 OTA 包
-> Java 页面发布配置 OTA 请求
-> 边端下载配置包
-> ota-apply.sh 解包并按 manifest 落盘
-> 自动重启目标服务
-> MQTT 上报回执和状态
-> 如有需要执行 rollback
```

## 2. Scope

当前测试对象是“配置 OTA”，不是“程序二进制 OTA”。

本次重点验证：

- `device config`
- `app config`
- `dlt645 standard points template`
- `manifest.json`
- `ota-apply.sh`
- `ota-rollback.sh`

## 3. Pre-Check

### 3.1 Java 服务

确认 Java 服务已启动，并可访问页面。

Java 服务启动后会自动准备以下目录：

```text
config/
config/templates/
config/gateway-otapackage/
```

### 3.2 MQTT Broker

确认 Java 与边端都连接到同一个 broker。

至少检查：

- `broker`
- `otaRequestTopic`
- `otaReplyTopic`
- `otaStatusTopic`

### 3.3 边端目录

确认目录存在：

```bash
mkdir -p /opt/modbus-gateway/config/runtime/devices
mkdir -p /opt/modbus-gateway/config/runtime/apps
mkdir -p /opt/modbus-gateway/config/templates
mkdir -p /opt/modbus-gateway/ota/downloads
mkdir -p /opt/modbus-gateway/ota/staging
mkdir -p /opt/modbus-gateway/ota/backup
```

### 3.4 OTA 脚本

确认脚本已部署并有执行权限：

```bash
cp deploy/ota-apply.sh /opt/modbus-gateway/bin/ota-apply.sh
cp deploy/ota-rollback.sh /opt/modbus-gateway/bin/ota-rollback.sh
chmod +x /opt/modbus-gateway/bin/ota-apply.sh
chmod +x /opt/modbus-gateway/bin/ota-rollback.sh
```

### 3.5 边端驱动

确认对应协议驱动和 MQTT Driver 已能正常运行。

例如 DLT645：

```bash
/opt/modbus-gateway/bin/Dlt645Driver --config /opt/modbus-gateway/config/runtime/devices/device_dlt645_multi_meter_1_2.json --app-config /opt/modbus-gateway/config/runtime/apps/mqtt-service.json
/opt/modbus-gateway/bin/MqttDriver --app-config /opt/modbus-gateway/config/runtime/apps/mqtt-service.json
```

## 4. Java Side Steps

### 4.1 保存当前配置

在 Java 页面先完成：

- 如为 DLT645，先在 `DLT645 配置` 页面上传标准点表
- 设备配置保存
- App 配置保存

确保当前页面上的配置是你准备下发给边端的版本。

### 4.2 进入 OTA 页面

打开：

```text
MQTT / OTA / App 配置
```

找到：

```text
配置 OTA 下发
```

### 4.3 填写参数

建议填写：

- `version`
  - 例如：`2026.04.21.001`
- `includeTemplates`
  - 选中
- `includeServiceManager`
  - 选中
  - 配置 OTA 包会额外携带 `gateway-services.sh`、systemd 服务模板、`ota-apply.sh`、`ota-rollback.sh`
- `restartServices`
  - 例如：
    ```text
    gateway-services.service
    ```

### 4.4 生成配置 OTA 包

点击：

```text
生成配置 OTA 包
```

预期结果：

- 页面出现 `artifactUrl`
- 页面出现 `sha256`
- 页面出现 `size`
- 页面出现完整 `manifest`
- 页面下方列表出现新的配置 OTA 包

### 4.5 手工验证下载链接

在边端执行：

```bash
wget "<artifactUrl>" -O /tmp/config-ota.tar.gz
```

如果下载失败，不要继续 OTA。

先解决：

- Java 服务访问性
- 防火墙
- IP/端口
- 路由

### 4.6 一键发布

点击：

```text
生成并发布配置 OTA
```

预期结果：

- 页面显示 `publish.success=true`
- 页面显示发布后的 `request`
- 记录里出现新的 `jobId`

## 5. MQTT Verification

在边端或测试机订阅：

```bash
mosquitto_sub -h <broker> -p <port> -t edge/ota/reply -v
mosquitto_sub -h <broker> -p <port> -t edge/ota/status -v
mosquitto_sub -h <broker> -p <port> -t edge/status -v
```

预期看到：

### 5.1 OTA Reply

```json
{
  "jobId": "...",
  "machineCode": "GW0001",
  "accepted": true,
  "message": "accepted"
}
```

### 5.2 OTA Status

应至少出现：

- `download`
- `install`
- `success` 或 `completed`

## 6. Edge Side Verification

### 6.1 下载目录

检查：

```bash
ls -l /opt/modbus-gateway/ota/downloads
```

应能看到下载的配置 OTA 包。

### 6.2 暂存目录

检查：

```bash
ls -l /opt/modbus-gateway/ota/staging
```

应能看到：

- `upgrade_history.log`
- `current_version.txt`
- `applied_version.txt`
- `<jobId>/`

### 6.3 解包内容

检查：

```bash
find /opt/modbus-gateway/ota/staging/<jobId> -maxdepth 4 -type f
```

应包含：

- `manifest.json`
- `config/runtime/devices/...`
- `config/runtime/apps/...`
- `config/templates/dlt645_2007_standard_points.json`

### 6.4 目标目录是否被替换

检查：

```bash
ls -l /opt/modbus-gateway/config/runtime/devices
ls -l /opt/modbus-gateway/config/runtime/apps
ls -l /opt/modbus-gateway/config/templates
```

重点确认：

- 目标 JSON 文件时间戳是否更新
- 内容是否为新版本

### 6.5 日志

检查：

```bash
cat /opt/modbus-gateway/ota/staging/upgrade_history.log
```

预期能看到：

- `start jobId=...`
- `manifest-copy ... -> ...`
- `restarting ...`
- `success jobId=...`

## 7. Service Restart Verification

如果 manifest 里配置了：

```json
"restart": {
  "enabled": true,
  "services": [
    "gateway-services.service"
  ]
}
```

则检查：

```bash
systemctl status dlt645-driver@device_dlt645_multi_meter_1_2
systemctl status mqtt-driver@mqtt-service
```

同时看 journal：

```bash
journalctl -u dlt645-driver@device_dlt645_multi_meter_1_2 -n 50
journalctl -u mqtt-driver@mqtt-service -n 50
```

## 8. Manual Apply Test

在没有走 MQTT 之前，也可以先手工验证：

```bash
/opt/modbus-gateway/bin/ota-apply.sh \
  /opt/modbus-gateway/ota/downloads/gateway-config-GW0001-2026.04.21.001.tar.gz \
  2026.04.21.001 \
  CONFIG_OTA_TEST_001 \
  /opt/modbus-gateway/ota/backup \
  /opt/modbus-gateway/ota/staging
```

如果手工执行都失败，不要走自动发布。

## 9. Rollback Test

### 9.1 执行回滚

```bash
/opt/modbus-gateway/bin/ota-rollback.sh \
  /opt/modbus-gateway/ota/downloads/gateway-config-GW0001-2026.04.21.001.tar.gz \
  2026.04.21.001 \
  CONFIG_OTA_TEST_001 \
  /opt/modbus-gateway/ota/backup \
  /opt/modbus-gateway/ota/staging
```

### 9.2 回滚后检查

检查：

```bash
cat /opt/modbus-gateway/ota/staging/rollback_CONFIG_OTA_TEST_001.txt
cat /opt/modbus-gateway/ota/staging/rollback_CONFIG_OTA_TEST_001_restored.txt
```

如果启用了服务重启，还要检查：

```bash
systemctl status dlt645-driver@device_dlt645_multi_meter_1_2
systemctl status mqtt-driver@mqtt-service
```

## 10. Failure Checklist

### 10.1 Java 生成成功但边端下载失败

检查：

- `artifactUrl` 是否是边端可访问地址
- Java 服务是否暴露下载接口
- 端口/防火墙是否放通

### 10.2 下载成功但 apply 失败

检查：

- `manifest.json` 是否在包内
- 包内 `path` 是否正确
- `target` 目录是否可写
- `python3`、`unzip`、`tar` 是否存在
- Java 当前运行目录下 `config/templates/dlt645_2007_standard_points.json` 是否已上传

### 10.3 配置已替换但服务没重启

检查：

- `restart.services` 是否写入 manifest
- 系统是否有 `systemctl`
- 服务名是否正确

### 10.4 rollback 没恢复成功

检查：

- `backupDir/opt/modbus-gateway/...` 是否存在
- apply 时是否正确备份了旧配置

## 11. Recommended Test Order

建议顺序：

1. Java 页面生成配置 OTA 包
2. 手工下载 `artifactUrl`
3. 手工执行 `ota-apply.sh`
4. 验证配置替换和服务重启
5. Java 页面一键发布配置 OTA
6. 验证 MQTT OTA request/reply/status
7. 手工执行 `ota-rollback.sh`
8. 验证恢复成功

## 12. Current Notes

当前实现说明：

- 配置 OTA 包格式已统一为 `tar.gz`
- `ota-apply.sh` 已支持按 `manifest.json` 落盘配置
- `ota-apply.sh` 已支持复制服务管理文件、自动 `chmod +x`、`systemctl daemon-reload`，并按 `restart.services` 自动重启服务
- `ota-rollback.sh` 已支持从备份目录恢复 `/opt` 与 `/etc` 下的配置和服务文件

首次从旧版边端升级时，建议先手工更新 `/opt/modbus-gateway/bin/ota-apply.sh` 和 `/opt/modbus-gateway/bin/ota-rollback.sh`。旧版 apply 脚本不会自动执行 `daemon-reload`，可能导致新下发的 `gateway-services.service` 首次无法被 systemd 识别。

未完成但后续可继续增强：

- rollback 也按 manifest 精准恢复
- Java 页面直接展示 OTA 包内文件清单
- MinIO 预签名地址一键生成并发布


---

## Gateway-zk/doc/modbus_tcp_support.md

# Modbus TCP 兼容方案

## 目标

在不破坏现有 Modbus RTU 采集、写回、共享内存、SQLite 落盘链路的前提下，增加 `modbus_tcp` 传输模式。

要求：

- 采集层继续复用现有 `Collector`、`ReadTaskPlanner`、`ModbusCodec`
- 写回层继续复用现有 `CommandExecutor`、`WritebackService`
- 配置层通过 `protocol.type` 切换 `modbus_rtu` / `modbus_tcp`
- 多从站逻辑继续保留
  - RTU 下是串口 slave
  - TCP 下是 unit id

## 现状分析

当前工程已经有一个稳定边界：

- `Collector` 和 `CommandExecutor` 只依赖 `IModbusClient`
- RTU 的串口细节收敛在 `ModbusRtuClient`

这意味着 TCP 兼容不需要重写采集业务，只需要新增一个 `IModbusClient` 的 TCP 实现，再在入口按配置选择客户端。

## 实施方案

### 1. 配置模型扩展

在 `ProtocolConfig` 下增加 `tcp` 配置块：

```json
"protocol": {
  "type": "modbus_tcp",
  "slave": 1,
  "tcp": {
    "host": "192.168.1.100",
    "port": 502,
    "connectTimeoutMs": 1000,
    "timeoutMs": 1000
  }
}
```

说明：

- `type=modbus_rtu`
  - 继续读取 `transport`
- `type=modbus_tcp`
  - 读取 `tcp`
- `slave`
  - 在 TCP 场景下作为 unit id 使用

### 2. 新增 `ModbusTcpClient`

新增 `ModbusTcpClient : IModbusClient`，职责只做：

- 建立 TCP 连接
- 组 MBAP Header
- 发送 Modbus PDU
- 读取并校验响应
- 返回寄存器数组或写入结果

复用现有接口：

- `readHoldingRegisters`
- `readInputRegisters`
- `writeSingleRegister`
- `writeMultipleRegisters`

### 3. 入口按协议切换

`main.cpp` 启动时：

- `protocol.type=modbus_rtu`
  - 继续创建 `ModbusRtuClient`
- `protocol.type=modbus_tcp`
  - 创建 `ModbusTcpClient`

这样 `GatewayDaemon` 不需要改业务逻辑。

### 4. 多从站兼容

当前工程已经支持：

- 一个配置里有多个 logical device
- 每个 logical device 有独立 `slave`

在 TCP 模式下：

- 这个 `slave` 继续透传到 `IModbusClient`
- 在 MBAP 里映射为 unit id

因此多设备配置可以直接复用。

## 已实施内容

本次已完成：

- `ProtocolConfig` 新增 `tcp`
- `ConfigLoader` 支持解析 `protocol.tcp`
- 新增 `ModbusTcpClient`
- `main.cpp` 已支持按 `protocol.type` 自动选择 RTU / TCP
- 新增示例配置：
  - [device_alarm_multi_slave_tcp_1_2.json](/D:/workspace/Embedded/Gateway-zk/config/runtime/devices/device_alarm_multi_slave_tcp_1_2.json)

涉及文件：

- [models.hpp](/D:/workspace/Embedded/Gateway-zk/include/edge_gateway/models.hpp)
- [config_loader.cpp](/D:/workspace/Embedded/Gateway-zk/src/config_loader.cpp)
- [modbus_tcp_client.hpp](/D:/workspace/Embedded/Gateway-zk/include/edge_gateway/modbus_tcp_client.hpp)
- [modbus_tcp_client.cpp](/D:/workspace/Embedded/Gateway-zk/src/modbus_tcp_client.cpp)
- [main.cpp](/D:/workspace/Embedded/Gateway-zk/main.cpp)

## 使用方式

### 单设备 TCP

```bash
./build-aarch64/ModbusRtu --config config/runtime/devices/device_alarm_multi_slave_tcp_1_2.json
```

### 多设备 TCP

可继续使用 `devices[]`，每个逻辑设备设置不同 `slave`：

```json
{
  "machineCode": "GW_TCP_01",
  "protocol": {
    "type": "modbus_tcp",
    "tcp": {
      "host": "192.168.1.100",
      "port": 502,
      "timeoutMs": 1000
    }
  },
  "meters": [
    {
      "meterCode": "DEV01",
      "slave": 1,
      "points": []
    },
    {
      "meterCode": "DEV02",
      "slave": 2,
      "points": []
    }
  ]
}
```

## 测试建议

建议按这个顺序验证：

1. 用 Modbus TCP 仿真器准备一个 502 端口从站
2. 用单点配置验证 `03/04`
3. 再验证 `06/16`
4. 最后再跑 `GatewayDaemon + 共享内存 + pointctl`

建议关注的错误：

- `modbus tcp connect failed`
- `modbus tcp transaction id mismatch`
- `modbus tcp unit id mismatch`
- `modbus tcp exception code X`

## 当前边界

当前版本已能满足大多数普通 TCP 采集场景，但还有两个边界：

- `connectTimeoutMs`
  - 目前配置项已经预留，实际连接阶段仍主要依赖系统 socket connect 行为
- 长连接重连策略
  - 当前实现是“首次访问建连，之后复用；收发异常时断开并在下次请求重连”

如果后面现场需要更严格的连接控制，可以继续补：

- 非阻塞 connect + 精确连接超时
- 心跳保活
- 连接池


---

## Gateway-zk/doc/dlt645_2007_support.md

# DLT645-2007 Support Plan

## 1. Goal

在现有 Gateway 框架内新增 `DLT645-2007` 协议支持，并保持以下公共能力不变：

- 共享内存最新值缓存
- SQLite 历史/告警落盘
- MQTT 独立驱动上传
- Java/Vue 图形化配置
- 实时监测
- `pointctl` 查询
- Java/Vue 侧按标准模板快速生成点表

本次目标不是复用旧工程的线程模型和变量表，而是将 `DLT645-2007` 的采集与解析能力接入现有统一框架。

本次方案新增一个明确原则：

- 每次生成 `DLT645-2007` 配置时，同时固定产出一份国标标准测点模板
- 这份标准点表按国标内置，不依赖用户手工选模板组
- 用户只需要补设备基础信息，标准点位模板直接随配置一并生成

## 2. Reference Project

参考项目路径：

`D:\Qtouch_VMware\QT281\Share1\drivers\DLT645_07`

重点参考内容：

- `DLT645.cpp / DLT645.h`
  - 645 帧组包
  - 应答校验
  - DI 读写处理
  - 数据解析
- `DLT645_Reg.xml`
  - DI 与标准测点模板映射关系
- `VarList.xml`
  - 旧工程变量表定义
- `电能量.csv`、`电能量2.csv`
  - 可作为标准点表示例来源

不建议直接复用的部分：

- Qt 对象模型
- 旧工程 RAM 结构
- 旧工程 UI、日志、线程调度逻辑

## 3. Integration Principle

新增 `DLT645-2007` 时，不改动共享内存、MQTT、告警、Java 实时页面的公共链路。

统一原则：

- 协议差异只留在“采集/编解码/写回”层
- 对上统一输出 `PointValue`
- 对下统一进入 `MemoryPointStore`

链路保持为：

`DLT645 Collector -> MemoryPointStore -> MqttDriver -> Java Realtime/MQTT/Alarm/SQLite`

同时在配置生成层新增一条链路：

`DLT645 National Standard Point Catalog -> Java Config Builder -> device_slave_x.json + standard_point_template.json`

也就是说：

- 运行态仍然只认统一 JSON
- 生成态固定附带一份完整国标标准点表模板
- 设备配置可基于该模板裁剪或引用，但模板文件本身每次都生成

## 4. Current Architecture Fit

现有 Gateway 已具备以下公共骨架：

- `DeviceConfig`
- `PointDefinition`
- `MemoryPointStore`
- `GatewayDaemon`
- `MqttDriverService`
- `CommandExecutor`
- Java 配置生成与实时展示

因此新增 645 不应通过修改 Modbus 逻辑硬兼容，而应作为第三种协议接入。

现有协议类型：

- `modbus_rtu`
- `modbus_tcp`

新增：

- `dlt645_2007`

## 5. National Standard Point Template Strategy

DLT645-2007 与 Modbus 最大差异不是“通信方式”，而是“国标点表天然固定”。

这里不采用“让用户按模板组勾选生成”的方式，而采用更直接的方式：

- 系统内置一份 `DLT645-2007` 国标标准点表模板
- 每次生成 DLT645 配置时，顺便生成这份标准点表文件
- 设备配置直接基于这份固定标准点表使用

### 5.1 设计原则

标准点表模板是固定资产，不是临时拼装结果：

- 模板内容来自国标与参考工程整理结果
- 模板文件版本化管理
- 同一版本下所有设备看到的是同一份标准点表定义

### 5.2 输出要求

每次生成 DLT645 配置时，至少输出两份文件：

1. 设备运行配置
2. 国标标准点表模板

例如：

- `device_slave_ttyS1_dlt645_demo.json`
- `dlt645_2007_standard_points.json`

### 5.3 模板作用

这份标准模板的作用不是给 C++ 运行态动态解释，而是：

- 作为固定标准点位清单
- 作为 Java 前端展示依据
- 作为配置生成依据
- 作为后续 Excel/导入导出的统一基准

### 5.4 为什么这样做

这样做有三个直接好处：

1. 国标点表固定，避免重复设计
2. 生成一次即得到标准模板，便于排查和对接
3. Java/C++/文档都围绕同一份标准点表工作，口径一致

## 6. Protocol Model Extension

### 6.1 Protocol Type

在 `protocol.type` 中新增：

```json
"protocol": {
  "type": "dlt645_2007"
}
```

### 6.2 Transport

DLT645-2007 当前第一阶段按串口实现：

```json
"protocol": {
  "type": "dlt645_2007",
  "transport": {
    "serialPort": "/dev/ttySP1",
    "baudRate": 2400,
    "dataBits": 8,
    "stopBits": 1,
    "parity": "E",
    "timeoutMs": 1000
  }
}
```

说明：

- 缺省参数建议兼容常见 645 电表：`2400 8E1`
- 后续如需要 TCP 转发器，再单独补 `dlt645_tcp` 或串口透传网关方案

## 7. Device Config Mapping

建议继续使用当前 `DeviceConfig + meters[]` 结构。

### 7.1 Device Level

```json
{
  "schemaVersion": "1.0.0",
  "machineCode": "GW0001",
  "protocol": {
    "type": "dlt645_2007",
    "transport": {
      "serialPort": "/dev/ttySP1",
      "baudRate": 2400,
      "dataBits": 8,
      "stopBits": 1,
      "parity": "E",
      "timeoutMs": 1000
    }
  },
  "collect": {
    "defaultIntervalMs": 1000
  },
  "memoryStore": {
    "enabled": true,
    "sharedMemoryName": "gateway_point_store"
  },
  "meters": []
}
```

### 7.2 Meter Level

对于 DLT645，`slave` 不适用，建议新增电表地址字段：

```json
{
  "meterCode": "METER0001",
  "deviceName": "Electric Meter 1",
  "address": "123456789012",
  "points": []
}
```

说明：

- `address` 为 12 位 645 表地址
- `meterCode` 继续作为平台侧逻辑编码

## 8. Point Model Mapping

DLT645 点位不再使用 Modbus 的 `function/address/length` 语义，建议在 `read` 中新增专属配置块。

示例：

```json
{
  "index": 21001,
  "pointCode": "total_active_energy",
  "name": "正向有功总电能",
  "desc": "DLT645 total active energy",
  "category": "telemetry",
  "enabled": true,
  "isStore": true,
  "reportOnChange": false,
  "persistIntervalSec": 60,
  "tags": ["dlt645", "energy"],
  "read": {
    "enable": true,
    "dataType": "dlt645_bcd",
    "intervalMs": 1000,
    "unit": "kWh",
    "dlt645": {
      "di": "00010000",
      "byteCount": 4,
      "scale": 0.01
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
  "alarms": []
}
```

建议新增的 `read.dlt645` 字段：

- `di`
  - 4 字节数据标识，字符串表示，例如 `00010000`
- `byteCount`
  - 响应数据长度
- `scale`
  - 解析倍率
- `decoder`
  - 可选，控制解析方式

建议新增的 `dataType`：

- `dlt645_bcd`
- `dlt645_uint32`
- `dlt645_ascii`
- `dlt645_datetime`

## 9. Standard Point Template File Design

建议增加固定标准点表文件：

- `config/templates/dlt645_2007_standard_points.json`
- `doc/dlt645_2007_template_reference.md`

### 9.1 Standard File Structure

建议标准点表文件结构如下：

```json
{
  "protocol": "dlt645_2007",
  "version": "1.0.0",
  "points": [
    {
      "templateKey": "forward_active_energy_total",
      "pointCode": "forward_active_energy_total",
      "name": "正向有功总电能",
      "di": "00010000",
      "dataType": "dlt645_bcd",
      "byteCount": 4,
      "scale": 0.01,
      "unit": "kWh",
      "category": "telemetry",
      "defaultIntervalMs": 1000,
      "storeLatest": true,
      "storeHistory": true
    }
  ]
}
```

### 9.2 Standard Point Scope

建议第一版直接固化完整标准点位范围，至少包括：

- 正反向有功总电能
- 组合有功总电能
- 正反向无功总电能
- 四象限无功电能
- 尖峰平谷分时电能
- A/B/C 相电压
- A/B/C 相电流
- 总及分相有功功率
- 总及分相无功功率
- 总及分相视在功率
- 总及分相功率因数
- 电网频率
- 需量及发生时间
- 电表运行状态相关标准项
- 驱动补充在线点

### 9.3 Online Point

DLT645 设备建议默认附带一个在线状态点，不依赖标准 DI：

- 点类型：`device_online`
- 值含义：
  - `1`: 最近一次采集成功
  - `0`: 连续超时或解析失败
- 该点由驱动内部生成
- 可作为实时监控和告警基础点

### 9.4 Index Generation Rule

设备配置引用标准点表时，建议统一采用规则化索引，而不是人工逐点输入。

建议规则：

- 一个电表一个索引段
- 每个模板组按固定偏移分配

例如：

- `meterSeq = 1`
- `indexBase = 200000 + meterSeq * 1000`
- `energy_basic` 从 `indexBase + 0`
- `voltage` 从 `indexBase + 100`
- `current` 从 `indexBase + 200`

这样优点是：

- 可预测
- 不冲突
- Java 端可自动生成
- 后期排查更直接

## 10. Runtime Behavior

### 10.1 First Phase

第一阶段建议只做只读采集，不做写入。

行为：

- 按点位 `intervalMs` 调度
- 构造 645 请求帧
- 发送串口命令
- 校验应答
- 解析 DI 对应数据
- 生成统一 `PointValue`
- 写共享内存

### 10.2 Output Format

最终统一输出为：

- `index`
- `machineCode`
- `meterCode`
- `pointCode`
- `pointName`
- `value`
- `quality`
- `ts`
- `expireAt`

这样后续：

- MQTT 上送无需区分 Modbus 或 645
- Java 实时监测无需额外适配

## 11. Java Config Generation Strategy

Java/Vue 侧不应以“模板组勾选”为主，而应固定使用内置国标标准点表。

### 11.1 UI Mode

建议 DLT645 页面分为两部分：

- 设备基础信息
- 国标标准点表预览/导出

### 11.2 User Input

用户只需要输入：

- 串口参数
- 电表地址
- `meterCode`
- `deviceName`
- 可选：采集周期覆盖值
- 可选：索引起始值

系统自动生成：

- `points[]`
- `dlt645_2007_standard_points.json`
- `read.dlt645.di`
- `dataType`
- `scale`
- `unit`
- 在线点

### 11.3 Export Result

最终导出仍为统一 JSON，例如：

- `config/runtime/devices/device_slave_ttyS1_001.json`
- `config/runtime/devices/device_slave_ttyS1_002.json`

如果用户选择“单串口多表一个进程”，则导出一个文件，内部包含多个 `meters[]`。

### 11.4 Excel Import

Excel 导入仍然保留，但重点变成“批量导入电表清单”，而不是逐点导入。

建议 Excel 结构：

- 第一行：字段名
- 第二行：中文说明
- 第三行开始：数据

建议列：

- `machineCode`
- `serialPort`
- `meterCode`
- `deviceName`
- `address`
- `indexBase`
- `intervalMs`

## 12. Code Architecture Proposal

### 12.1 New Components

建议新增：

- `dlt645_client.hpp / cpp`
  - 串口收发
  - 帧构造
  - 校验
- `dlt645_codec.hpp / cpp`
  - DI 请求与响应解析
  - BCD/字符串/时间转换
- `dlt645_collector.hpp / cpp`
  - 按点轮询
  - 到期调度
  - 统一输出 `PointValue`

### 12.2 Template Builder

建议再增加一个模板展开层：

- `dlt645_template_catalog.hpp / cpp`
  - 标准模板定义加载
- `dlt645_template_builder.hpp / cpp`
  - 将模板组展开为 `PointDefinition`

说明：

- C++ 运行期可以不依赖该能力
- 该能力更适合先放在 Java 端
- 但若后续需要 CLI 自动生成配置，C++/工具侧也可以复用

### 12.3 Factory Layer

建议增加协议工厂：

- `ProtocolFactory`

职责：

- 根据 `protocol.type` 创建采集器/写入器

例如：

- `modbus_rtu` -> 现有 Modbus Collector / Writer
- `modbus_tcp` -> 现有 Modbus TCP Collector / Writer
- `dlt645_2007` -> 新增 DLT645 Collector / Writer

### 12.4 Refactor Direction

建议将现有偏 Modbus 的接口逐步抽象：

- `Collector`
- `CommandExecutor`

重构为协议无关接口：

- `IProtocolCollector`
- `IProtocolWriter`

这样 `GatewayDaemon` 不需要感知底层协议差异。

## 13. Scheduling Strategy

DLT645 与 Modbus 不同，不适合按地址连续批量合并。

第一阶段建议：

- 一次请求一个 DI
- 同一表地址按点位到期顺序轮询
- 优先保证稳定

第二阶段可优化：

- 按相同电表地址分组
- 支持少量连续 DI 或块读逻辑
- 参考旧项目里的 `DataVarBlock`

## 14. Write Support Strategy

### 14.1 Phase 1

仅支持读，不支持写。

对 DLT645 点位执行：

- `pointctl write`
- MQTT 下发写入

统一返回：

`unsupported write for dlt645_2007`

### 14.2 Phase 2

针对少量可写 DI 再扩展写入能力。

建议写配置模型：

```json
"write": {
  "enable": true,
  "dataType": "dlt645_bcd",
  "dlt645": {
    "di": "04000101",
    "password": "00000000",
    "operatorCode": "00000000"
  }
}
```

写入链路仍复用：

- `pointctl write`
- 共享内存 pending queue
- `GatewayDaemon` 协议写入执行
- MQTT 命令回执

## 15. Java Side Changes

Java/Vue 侧需要增加协议选择和 645 专属点位编辑字段。

### 15.1 Protocol Type

协议类型下拉新增：

- `modbus_rtu`
- `modbus_tcp`
- `dlt645_2007`

### 15.2 DLT645 Device Fields

当选择 `dlt645_2007` 时：

- 保留串口配置
- 默认隐藏 Modbus `slave/function/address/length`
- 显示：
  - `meter address`
  - `templateGroups`
  - `indexBase`
  - `intervalMs`
  - “生成标准点表”按钮

高级模式下才显示：

- `di`
- `645 dataType`
- `scale`
- `unit`

### 15.3 Import Template

建议新增 645 点表导入模板字段：

- `machineCode`
- `serialPort`
- `meterCode`
- `deviceName`
- `address`
- `templateGroups`
- `indexBase`
- `intervalMs`

## 16. Sample Config Plan

建议新增两类样例。

### 16.1 Runtime Sample

运行态样例：

- `config/runtime/devices/device_slave_ttyS1_dlt645_demo.json`
- `config/runtime/devices/device_slave_ttyS2_dlt645_demo.json`
- `config/runtime/apps/mqtt-service.json`

### 16.2 Template Sample

设计态样例：

- `config/examples/dlt645_meter_import_template.xlsx`
- `config/examples/dlt645_meter_import_template.csv`
- `config/examples/dlt645_2007_standard_points.json`

样例内容：

- 单串口多表
- 固定国标标准点位
- 在线点
- MQTT 配置
- Excel 导入模板

## 17. Document Plan

建议同步补充文档：

- `doc/dlt645_2007_support.md`
- `doc/dlt645_2007_template_reference.md`
- `doc/config_reference.md`
  - 增加 `protocol.type=dlt645_2007`
  - 增加 `read.dlt645.*`
- `doc/app_config.md`
  - 说明 645 与现有 MQTT/共享内存复用关系

## 18. Implementation Phases

### Phase 1

- 新增 `protocol.type=dlt645_2007`
- 支持配置解析
- 固化国标标准点表文件
- Java 生成设备配置时同步生成标准点表文件
- 支持串口单点读取 DI
- 支持结果写共享内存
- 支持 Java 配置导出

### Phase 2

- 接入 MQTT、实时监测、告警
- 补样例 JSON
- 补 Excel 导入模板
- 补配置文档

### Phase 3

- 支持批量优化
- 支持多表地址轮询性能优化
- 压测

### Phase 4

- 支持部分可写 DI
- 接入 `pointctl write`
- 接入 MQTT 命令写回

## 19. Recommended Next Step

建议下一步优先做“固定标准点表落地”而不是做模板组勾选：

1. 先整理完整国标标准点表
2. 先生成固定 `dlt645_2007_standard_points.json`
3. 让 Java 每次导出 DLT645 时同步导出这份文件
4. 再实现 C++ 采集侧对这些标准点位的执行

这样更符合 DLT645 实际使用方式，也能显著降低后续配置工作量。

## 20. Summary

DLT645-2007 接入方案的核心不是复制旧工程，也不是让用户手工逐点配 DI，而是：

- 底层新增 645 帧通信与解析
- 中间统一输出 `PointValue`
- 上层继续复用共享内存、MQTT、Java、告警、实时监测
- 设计侧固定产出一份国标标准点表文件并与设备配置同步生成

推荐按“固定标准点表优先、先生成标准文件、再打通采集”的路线实施。


---

## Gateway-zk/doc/dlt645_2007_template_reference.md

# DLT645-2007 Standard Point Template Reference

## 1. Goal

本文件用于定义 `DLT645-2007` 的固定标准点位模板。

这里的核心约束只有一条：

- 每次生成 `DLT645-2007` 设备配置时，必须同时生成一份固定标准点位模板文件

该模板文件不是用户手工维护的点表，而是系统内置、版本化、统一口径的国标点位清单。

建议固定输出文件名：

- `dlt645_2007_standard_points.json`

## 2. Usage Rule

标准点位模板的使用规则如下：

1. Java 端生成 DLT645 配置时，同时导出这份标准模板文件
2. 设备运行配置中的点位，默认从该标准模板展开
3. Java 前端的 DLT645 点位展示，也以该模板为准
4. C++ 侧采集实现，按该模板中的 `di / dataType / scale / unit` 执行

也就是说：

- 标准点位模板是“固定基线”
- 设备配置是“实例化结果”

## 3. File Structure

建议标准点位模板结构如下：

```json
{
  "protocol": "dlt645_2007",
  "version": "1.0.0",
  "source": "national_standard",
  "points": [
    {
      "templateKey": "forward_active_energy_total",
      "pointCode": "forward_active_energy_total",
      "name": "正向有功总电能",
      "category": "telemetry",
      "di": "00010000",
      "dataType": "dlt645_bcd",
      "byteCount": 4,
      "scale": 0.01,
      "unit": "kWh",
      "defaultIntervalMs": 1000,
      "storeLatest": true,
      "storeHistory": true,
      "reportOnChange": false,
      "enabledByDefault": true
    }
  ]
}
```

## 4. Point Field Definition

每个标准点位建议至少包含以下字段：

- `templateKey`
  - 标准模板唯一键
- `pointCode`
  - 导出到设备配置后的标准点编码
- `name`
  - 中文名称
- `category`
  - `telemetry` / `status` / `event`
- `di`
  - DLT645 数据标识
- `dataType`
  - 解析类型
- `byteCount`
  - 应答数据字节数
- `scale`
  - 倍率
- `unit`
  - 单位
- `defaultIntervalMs`
  - 默认采集周期
- `storeLatest`
  - 是否保存最新值
- `storeHistory`
  - 是否保存历史
- `reportOnChange`
  - 是否按变位事件上传
- `enabledByDefault`
  - 导出时默认启用

建议扩展字段：

- `desc`
- `phase`
- `tariffNo`
- `precision`
- `alarmSuggested`
- `notes`

## 5. Data Type Convention

建议固定支持以下 DLT645 数据类型：

- `dlt645_bcd`
  - 常规 BCD 数值
- `dlt645_bcd_signed`
  - 有符号 BCD
- `dlt645_uint32`
  - 无符号整数
- `dlt645_ascii`
  - 字符串
- `dlt645_datetime`
  - 日期时间
- `dlt645_status_bits`
  - 状态位

## 6. Standard Point Scope

第一版标准点位清单建议直接覆盖最常用且通用的国标数据项。

### 6.1 Energy

- 正向有功总电能
- 反向有功总电能
- 组合有功总电能
- 正向无功总电能
- 反向无功总电能
- 第一象限无功总电能
- 第二象限无功总电能
- 第三象限无功总电能
- 第四象限无功总电能

### 6.2 Tariff Energy

- 尖时正向有功电能
- 峰时正向有功电能
- 平时正向有功电能
- 谷时正向有功电能
- 尖时反向有功电能
- 峰时反向有功电能
- 平时反向有功电能
- 谷时反向有功电能

### 6.3 Settlement Day Energy

这部分是当前文档之前缺失的固定国标点位，必须补进标准模板。

参考工程 `settlementday.cpp` 已明确实现了以下规则：

- 支持 `上1` 到 `上12` 个结算日
- 支持 4 类能量类型
  - 正向有功
  - 反向有功
  - 正向无功
  - 反向无功
- 每类支持 5 个项目
  - 总电量
  - 费率1电量
  - 费率2电量
  - 费率3电量
  - 费率4电量

因此这部分固定标准点数量为：

- `12 * 4 * 5 = 240` 个点

这 240 个点建议全部进入 `dlt645_2007_standard_points.json`。

示例中文点名：

- 上1个结算日正向有功总电量
- 上1个结算日正向有功费率1电量
- 上1个结算日正向有功费率2电量
- 上1个结算日正向有功费率3电量
- 上1个结算日正向有功费率4电量
- 上1个结算日反向有功总电量
- 上1个结算日正向无功总电量
- 上1个结算日反向无功总电量
- 上12个结算日反向无功费率4电量

这些点不应由用户手工添加，而应作为固定标准模板自动附带。

### 6.4 Voltage

- A 相电压
- B 相电压
- C 相电压

### 6.5 Current

- A 相电流
- B 相电流
- C 相电流

### 6.6 Active Power

- 总有功功率
- A 相有功功率
- B 相有功功率
- C 相有功功率

### 6.7 Reactive Power

- 总无功功率
- A 相无功功率
- B 相无功功率
- C 相无功功率

### 6.8 Apparent Power

- 总视在功率
- A 相视在功率
- B 相视在功率
- C 相视在功率

### 6.9 Power Factor

- 总功率因数
- A 相功率因数
- B 相功率因数
- C 相功率因数

### 6.10 Frequency

- 电网频率

### 6.11 Demand

- 正向有功最大需量
- 正向有功最大需量发生时间
- 反向有功最大需量
- 反向有功最大需量发生时间
- 无功最大需量
- 无功最大需量发生时间

### 6.12 Device Status

- 电表运行状态
- 电表告警状态
- 抄表状态

### 6.13 Driver Added Point

这个点不是国标 DI，但必须固定附带：

- `device_online`
  - 设备在线状态
  - `1` 表示采集成功
  - `0` 表示持续超时或失败

## 7. Recommended Naming Convention

建议统一使用英文 `pointCode`，避免后续平台解析再做二次映射。

推荐命名如下：

### 7.1 Energy

- `forward_active_energy_total`
- `reverse_active_energy_total`
- `combined_active_energy_total`
- `forward_reactive_energy_total`
- `reverse_reactive_energy_total`
- `quadrant1_reactive_energy_total`
- `quadrant2_reactive_energy_total`
- `quadrant3_reactive_energy_total`
- `quadrant4_reactive_energy_total`

### 7.2 Tariff Energy

- `forward_active_energy_tariff_1`
- `forward_active_energy_tariff_2`
- `forward_active_energy_tariff_3`
- `forward_active_energy_tariff_4`
- `reverse_active_energy_tariff_1`
- `reverse_active_energy_tariff_2`
- `reverse_active_energy_tariff_3`
- `reverse_active_energy_tariff_4`

### 7.3 Settlement Day Energy

建议对“上 X 个结算日”使用统一规则命名，不单独手写 240 个名字。

命名规则：

- `settlement_day_{day}_{kind}_{item}`

其中：

- `{day}`: `1` 到 `12`
- `{kind}`:
  - `forward_active_energy`
  - `reverse_active_energy`
  - `forward_reactive_energy`
  - `reverse_reactive_energy`
- `{item}`:
  - `total`
  - `tariff_1`
  - `tariff_2`
  - `tariff_3`
  - `tariff_4`

示例：

- `settlement_day_1_forward_active_energy_total`
- `settlement_day_1_forward_active_energy_tariff_1`
- `settlement_day_1_forward_active_energy_tariff_2`
- `settlement_day_1_forward_active_energy_tariff_3`
- `settlement_day_1_forward_active_energy_tariff_4`
- `settlement_day_7_reverse_active_energy_total`
- `settlement_day_12_forward_reactive_energy_tariff_4`
- `settlement_day_12_reverse_reactive_energy_total`

### 7.4 Analog Telemetry

- `voltage_a`
- `voltage_b`
- `voltage_c`
- `current_a`
- `current_b`
- `current_c`
- `active_power_total`
- `active_power_a`
- `active_power_b`
- `active_power_c`
- `reactive_power_total`
- `reactive_power_a`
- `reactive_power_b`
- `reactive_power_c`
- `apparent_power_total`
- `apparent_power_a`
- `apparent_power_b`
- `apparent_power_c`
- `power_factor_total`
- `power_factor_a`
- `power_factor_b`
- `power_factor_c`
- `grid_frequency`

### 7.5 Demand

- `forward_active_demand_max`
- `forward_active_demand_max_time`
- `reverse_active_demand_max`
- `reverse_active_demand_max_time`
- `reactive_demand_max`
- `reactive_demand_max_time`

### 7.6 Status

- `meter_status`
- `meter_alarm_status`
- `meter_read_status`
- `device_online`

## 8. Recommended Default Behavior

默认建议如下：

- 电能类
  - `storeLatest=true`
  - `storeHistory=true`
  - `reportOnChange=false`
- 结算日电能类
  - `storeLatest=true`
  - `storeHistory=true`
  - `reportOnChange=false`
- 电压/电流/功率/频率类
  - `storeLatest=true`
  - `storeHistory=true`
  - `reportOnChange=false`
- 状态类
  - `storeLatest=true`
  - `storeHistory=false`
  - `reportOnChange=true`
- 在线点
  - `storeLatest=true`
  - `storeHistory=false`
  - `reportOnChange=true`

## 9. Index Allocation Rule

标准点位模板本身不固定 `index`，但导出设备配置时建议按规则生成。

建议规则：

- 每块表分配一个固定索引段
- 标准点位按模板顺序依次排布

例如：

- `meterSeq=1` -> `indexBase=200000`
- `meterSeq=2` -> `indexBase=201000`

然后：

- 模板第 1 个点 -> `indexBase + 0`
- 模板第 2 个点 -> `indexBase + 1`
- 模板第 3 个点 -> `indexBase + 2`

这样更简单，避免组偏移和空洞。

## 10. Initial Implementation Suggestion

建议第一轮先落一个“可运行的固定清单”，不要先追求一次性覆盖全部冷门 DI。

建议首批强制内置：

- 电能类
- 分时电能类
- 上1到上12个结算日的总/费率电能类
- 三相电压
- 三相电流
- 总及分相功率
- 总及分相功率因数
- 频率
- 设备状态
- 在线点

最大需量和更多状态字可作为同一份标准模板的后续补充分组。

## 11. Settlement Day DI Rule

参考工程 `settlementday.cpp` 已给出结算日数据标识生成规则。

支持 4 种类型码：

- 正向有功：`0x00010000`
- 反向有功：`0x00020000`
- 正向无功：`0x00030000`
- 反向无功：`0x00040000`

组合规则：

- `dataId = typeCode | (itemNo << 8) | dayNo`

其中：

- `dayNo`: `1..12`
- `itemNo`:
  - `0`: 总电量
  - `1`: 费率1电量
  - `2`: 费率2电量
  - `3`: 费率3电量
  - `4`: 费率4电量

示例：

- 上1个结算日正向有功总电量
  - `dayNo=1`
  - `itemNo=0`
  - `dataId=0x00010001`
- 上1个结算日正向有功费率1电量
  - `dayNo=1`
  - `itemNo=1`
  - `dataId=0x00010101`
- 上12个结算日反向无功费率4电量
  - `dayNo=12`
  - `itemNo=4`
  - `dataId=0x0004040C`

后续生成 `dlt645_2007_standard_points.json` 时，这部分可以按规则程序化展开，不需要手工写 240 条。

## 12. Source Note

当前参考工程中的 `DLT645_Reg.xml` 为 `gbk` 编码，直接读取存在字段乱码问题，但从内容可确认至少已覆盖以下类别：

- 电能
- 分时电能
- 三相电压
- 三相电流
- 总及分相功率
- 频率
- 设备状态

后续整理标准模板时，建议以该文件为主数据源，再人工校正中文名称和 `DI` 编码格式。

## 13. Next Step

本文件确认后，下一步直接做两件事：

1. 生成 `config/templates/dlt645_2007_standard_points.json`
2. 在 Java 导出 DLT645 配置时，同步导出这份标准点位模板文件


---

## Gateway-zk/doc/dlt645_runtime_test.md

# DLT645-2007 Runtime Test Guide

## 1. Scope

当前 DLT645 已落地到以下阶段：

- 配置解析已支持
- 标准点表文件已支持
- 多电表地址已支持
- 空 `points` 可自动从标准点表展开
- 只读采集框架已接入
- 写入暂不支持

当前目标是联调以下链路：

`DLT645 Device -> Dlt645Driver -> MemoryPointStore -> MqttDriver -> Platform`

## 2. Runtime Files

设备配置：

- [device_dlt645_multi_meter_1_2.json](D:/workspace/Embedded/Gateway-zk/config/runtime/devices/device_dlt645_multi_meter_1_2.json)

MQTT App 配置：

- [mqtt-service.json](D:/workspace/Embedded/Gateway-zk/config/runtime/apps/mqtt-service.json)

标准点表：

- [dlt645_2007_standard_points.json](D:/workspace/Embedded/Gateway-zk/config/templates/dlt645_2007_standard_points.json)

## 3. Current Assumptions

当前样例按以下假设编写：

- 协议：`dlt645_2007`
- 串口：`/dev/ttyS1`
- 串口参数：`2400 8E1`
- 电表地址：
  - `000000000001`
  - `000000000002`
- 标准点表：自动从 `config/templates/dlt645_2007_standard_points.json` 展开

## 4. Start Commands

### 4.1 Mock Run

用于先验证程序流程是否打通：

```bash
./Dlt645Driver --config config/runtime/devices/device_dlt645_multi_meter_1_2.json --app-config config/runtime/apps/mqtt-service.json --mock
./MqttDriver --app-config config/runtime/apps/mqtt-service.json
```

说明：

- `--mock` 只用于验证流程
- 当前 mock 只返回固定样例数据，不代表真实电表值

### 4.2 Real Device Run

```bash
./Dlt645Driver --config config/runtime/devices/device_dlt645_multi_meter_1_2.json --app-config config/runtime/apps/mqtt-service.json
./MqttDriver --app-config config/runtime/apps/mqtt-service.json
```

## 5. Expected Runtime Behavior

启动后预期行为：

1. `Dlt645Driver` 读取 DLT645 配置
2. 自动展开标准点表
3. 按每个电表地址轮询 `read.dlt645.di`
4. 将采样结果写入共享内存
5. `MqttDriver` 从共享内存扫描并发布 MQTT

## 6. Expected Index Rule

为避免多表 index 冲突，当前自动展开规则为：

- 第 1 块表：`200000 + 点位偏移`
- 第 2 块表：`210000 + 点位偏移`
- 第 3 块表：`220000 + 点位偏移`

也就是每块表预留 `10000` 个索引位。

## 7. Expected MQTT Payload

实时数据主题：

- `edge/telemetry`

变位事件主题：

- `edge/event/change`

状态主题：

- `edge/status`

当前 DLT645 实时数据结构与 Modbus 统一，不额外分协议字段。

关键识别字段：

- `machineCode`
- `meterCode`
- `pointCode`
- `index`

## 8. Typical Verification Steps

### 8.1 Process Start

检查启动日志是否包含：

- `mode=dlt645_2007`
- `meters=2`
- `sharedMemory=gateway_point_store`

### 8.2 Shared Memory

使用 `pointctl` 或共享内存查看工具确认：

- 两个 `meterCode` 都有数据
- `index` 已按分段分配
- `quality=1`
- `stale=false`

### 8.3 MQTT

使用 MQTTX 或 mosquitto_sub 订阅：

```bash
mosquitto_sub -h 127.0.0.1 -p 1883 -t edge/telemetry -v
mosquitto_sub -h 127.0.0.1 -p 1883 -t edge/status -v
```

### 8.4 Multi-Meter

确认以下几点：

- `METER0001` 和 `METER0002` 都有报文
- 同一 `pointCode` 在不同 `meterCode` 下能区分
- 不同电表不会互相覆盖索引

## 9. Current Known Limits

当前限制：

- 写入未实现
- DLT645 状态字仅做基础透传，位解析未细化
- `ascii/datetime` 目前先做基础文本输出
- 复杂冻结类/特殊表型 `DI` 仍需按设备型号复核
- 当前环境没有本地 `cmake`，未做本机编译验证

## 10. Failure Diagnostics

### 10.1 Address Error

如果表地址错误，通常表现为：

- 无响应
- 持续超时
- `device_online=0`

优先检查：

- `meters[].address`
- 串口参数 `2400 8E1`

### 10.2 Standard Points File Error

如果标准点表路径错误，通常表现为：

- 进程启动失败
- 提示找不到 `standardPointsFile`

优先检查：

- `protocol.standardPointsFile`
- `dlt645.standardPointsFile`

### 10.3 Point Parse Error

如果某些点值异常，优先检查：

- `di`
- `dataType`
- `byteCount`
- `scale`

## 11. Next Step

完成当前联调后，下一步建议继续：

1. 细化 DLT645 mock 数据
2. 增加真实状态字解析
3. 增加冻结类点的分层启用策略
4. 增加 DLT645 专用调试日志


---

## Gateway-zk/doc/deployment_ubuntu.md

# Ubuntu 20.04 ARM 部署说明

## 1. 目录建议

推荐部署目录：

- 程序：`/opt/modbus-gateway/bin/ModbusRtu`
- 程序：`/opt/modbus-gateway/bin/MqttDriver`
- 设备配置：`/opt/modbus-gateway/config/runtime/devices/*.json`
- 应用配置：`/opt/modbus-gateway/config/runtime/apps/*.json`
- SQLite：`/opt/modbus-gateway/data/point_samples.db`

## 2. 运行参数

程序支持：

```bash
./ModbusRtu --config /opt/modbus-gateway/config/runtime/devices/device_slave_ttySP1.json
```

开发调试时可使用：

```bash
./ModbusRtu --config config/runtime/devices/device_slave_ttySP1.json --mock
```

说明：

- Ubuntu 边端默认应走真实串口
- `--mock` 仅用于本地联调

## 3. 依赖

建议安装：

```bash
sudo apt-get update
sudo apt-get install -y libsqlite3-0
```

如果串口设备权限不足，建议把服务用户加入 `dialout`：

```bash
sudo usermod -aG dialout <user>
```

## 4. systemd

当前保留的服务模板：

- `deploy/modbus-rtu@.service`
- `deploy/dlt645-driver@.service`
- `deploy/mqtt-driver@.service`
- `deploy/gateway-services.service`

OTA 脚本：

- `deploy/ota-apply.sh`
- `deploy/ota-rollback.sh`

安装方式示例：

```bash
sudo cp deploy/modbus-rtu@.service /etc/systemd/system/modbus-rtu@.service
sudo cp deploy/dlt645-driver@.service /etc/systemd/system/dlt645-driver@.service
sudo cp deploy/mqtt-driver@.service /etc/systemd/system/mqtt-driver@.service
sudo cp deploy/gateway-services.service /etc/systemd/system/gateway-services.service
sudo cp deploy/gateway-services.sh /opt/modbus-gateway/bin/gateway-services.sh
sudo cp deploy/ota-apply.sh /opt/modbus-gateway/bin/ota-apply.sh
sudo cp deploy/ota-rollback.sh /opt/modbus-gateway/bin/ota-rollback.sh
sudo chmod +x /opt/modbus-gateway/bin/gateway-services.sh /opt/modbus-gateway/bin/ota-apply.sh /opt/modbus-gateway/bin/ota-rollback.sh
sudo systemctl daemon-reload
```

如果之前已经单独 enable 过驱动实例，先停止并交给 `gateway-services.service` 管理：

```bash
sudo systemctl stop 'modbus-rtu@*.service' 'dlt645-driver@*.service' 'mqtt-driver@*.service' || true
sudo systemctl disable 'modbus-rtu@*.service' 'dlt645-driver@*.service' 'mqtt-driver@*.service' || true
```

开机自启动：

```bash
sudo systemctl enable gateway-services.service
sudo systemctl start gateway-services.service
```

`gateway-services.service` 会扫描 `/opt/modbus-gateway/config/runtime/devices/*.json`：

- `protocol.type=modbus_rtu` 或 `modbus_tcp`：启动 `modbus-rtu@<设备配置文件名不含.json>`
- `protocol.type=dlt645_2007`：启动 `dlt645-driver@<设备配置文件名不含.json>`
- 如果存在 `/opt/modbus-gateway/config/runtime/apps/mqtt-service.json`：启动 `mqtt-driver@mqtt-service`
- 如果某类配置文件不存在，对应驱动不会启动

手动重扫配置：

```bash
sudo systemctl restart gateway-services.service
```

查看日志：

```bash
sudo journalctl -u gateway-services.service -f
sudo journalctl -u modbus-rtu@device_slave_ttySP1 -f
sudo journalctl -u dlt645-driver@device_dlt645_multi_meter_1_2 -f
sudo journalctl -u mqtt-driver@mqtt-service -f
```

## 5. 多串口后台启动

多串口统一由 `gateway-services.service` 扫描配置后启动。不要再固定 enable 所有 `modbus-rtu@*` / `dlt645-driver@*` / `mqtt-driver@*` 实例，否则配置删除后旧实例仍可能开机启动。

## 6. 配置建议

`memoryStore` 推荐至少配置：

```json
{
  "sharedMemoryName": "gateway_point_store",
  "sqlitePath": "/opt/modbus-gateway/data/point_samples.db",
  "persistFlushIntervalMs": 60000,
  "writebackIntervalMs": 500,
  "writebackBatchSize": 100
}
```

串口参数放在：

```json
{
  "protocol": {
    "transport": {
      "serialPort": "/dev/ttyUSB0",
      "baudRate": 9600,
      "dataBits": 8,
      "stopBits": 1,
      "parity": "N",
      "timeoutMs": 1000
    }
  }
}
```


---

## Gateway-zk/doc/performance_stress_test.md

# 压测说明

## 目标

用可重复的方式验证以下链路的吞吐和时延：

- 采集值写入共享内存
- 共享内存全量快照读取
- MQTT 驱动扫描与告警处理

当前仓库提供了一个压测工具：

- [stress_runner.cpp](/D:/workspace/Embedded/Gateway-zk/tools/stress_runner.cpp:1)

## 压测工具能力

`stress_runner` 不依赖真实串口，也不访问真实 MQTT broker。

它会：

- 读取一个或多个 `device.json`
- 注册全部点位到共享内存
- 多线程持续调用 `putLatest`
- 定时调用 `getAllLatest`
- 可选调用 `MqttDriverService::runScanOnce`
- 输出吞吐和耗时统计

适合先验证：

- `MemoryPointStore`
- `AlarmService`
- `MqttDriverService`

不适合替代真实现场联调：

- 不测真实 Modbus RTU/TCP 物理链路
- 不测真实 MQTT 网络栈吞吐

## 编译

构建目标：

```bash
cmake --build build-aarch64 --target stress_runner -j$(nproc)
```

Windows/CLion：

```bash
cmake --build cmake-build-debug --target stress_runner -j 8
```

## 参数

```bash
./stress_runner \
  --device-config <path> \
  [--device-config <path> ...] \
  [--app-config <path>] \
  [--duration-sec <n>] \
  [--writer-threads <n>] \
  [--points <n>] \
  [--expand-points <n>] \
  [--snapshot-interval-ms <n>] \
  [--mqtt-scan-interval-ms <n>] \
  [--shm <name>]
```

说明：

- `--device-config`
  - 必填，可重复
  - 用于加载点位定义和告警规则
- `--app-config`
  - 可选
  - 传入后会额外执行 MQTT 驱动扫描压测
- `--duration-sec`
  - 压测持续时间，默认 `30`
- `--writer-threads`
  - 并发写线程数，默认 `1`
- `--points`
  - 只使用前 N 个点，默认全部
- `--expand-points`
  - 按现有点模板自动扩展到目标点数
  - 适合当前配置只有几十个点，但你要压 `10000/20000` 点的场景
- `--snapshot-interval-ms`
  - `getAllLatest` 调用周期，默认 `1000`
- `--mqtt-scan-interval-ms`
  - `runScanOnce` 调用周期，默认 `1000`
- `--shm`
  - 覆盖共享内存名，避免和在线进程共用同一段

## 推荐压测场景

### 场景 1：共享内存写入压力

目标：

- 验证 `putLatest` 吞吐
- 看 `20000` 点规模是否稳定

示例：

```bash
./build-aarch64/stress_runner \
  --device-config config/runtime/devices/device_slave_ttySP1.json \
  --duration-sec 60 \
  --writer-threads 4 \
  --expand-points 10000 \
  --snapshot-interval-ms 1000 \
  --shm stress_point_store
```

重点看：

- `writeOps`
- `opsPerSec`
- `batchAvgUs`
- `batchMaxUs`

### 场景 2：共享内存快照读取

目标：

- 验证 `getAllLatest` 对大点位数的耗时

示例：

```bash
./build-aarch64/stress_runner \
  --device-config config/runtime/devices/device_slave_ttySP1.json \
  --duration-sec 60 \
  --writer-threads 2 \
  --expand-points 20000 \
  --snapshot-interval-ms 200 \
  --shm stress_point_store
```

重点看：

- `snapshotAvgUs`
- `snapshotMaxUs`
- `snapshotPoints`

### 场景 3：告警和 MQTT 驱动扫描

目标：

- 验证点位告警规则计算和 MQTT 驱动扫描性能

示例：

```bash
./build-aarch64/stress_runner \
  --device-config config/runtime/devices/device_slave_ttySP1.json \
  --app-config config/runtime/apps/mqtt-service.json \
  --duration-sec 60 \
  --writer-threads 4 \
  --expand-points 10000 \
  --snapshot-interval-ms 1000 \
  --mqtt-scan-interval-ms 500 \
  --shm stress_point_store
```

重点看：

- `mqttDriverCycles`
- `mqttDriverAvgUs`
- `mqttDriverMaxUs`

## 推荐压测步骤

建议按下面顺序推进：

1. 先压 `5000` 点，确认逻辑稳定
2. 再压 `10000` 点
3. 再压 `20000` 点
4. 再增加 `writer-threads`
5. 最后缩短 `snapshot-interval-ms` 和 `mqtt-scan-interval-ms`

## 判定建议

如果你的目标是边端稳定运行，建议至少满足：

- `10000` 点下持续 `30~60` 分钟无异常退出
- `snapshotAvgUs` 保持在毫秒级
- `mqttDriverAvgUs` 随点位数增长但不出现明显抖动失控
- 没有出现 stale 异常增长
- 没有出现共享内存脏值或重复 index

## 性能瓶颈判断

如果写入慢：

- 优先看 `putLatest`
- 关注锁竞争和线程数过高

如果全量慢：

- 优先看 `getAllLatest`
- 点位数过大时会受共享内存扫描影响

如果 MQTT 扫描慢：

- 优先看告警规则数量
- 其次看全量上传周期是否过短

## 当前工具输出示例

```text
stress started points=10000 writers=4 durationSec=60 shm=stress_point_store mqttDriverCycle=on
stress result
  writeOps=2400000 opsPerSec=40000 batchAvgUs=820 batchMaxUs=5400
  snapshots=60 snapshotAvgUs=3100 snapshotMaxUs=9200 snapshotPoints=600000
  mqttDriverCycles=120 mqttDriverAvgUs=4500 mqttDriverMaxUs=12000
```

## 注意事项

- 压测建议使用独立 `--shm`，不要和生产进程共用共享内存
- 如果共享内存布局版本变化，旧段应停止复用
- 当前工具不发送真实 MQTT，只测业务处理链路


---

## Java edge-gateway/README.md

# Edge Gateway Config Studio

这个项目现在提供一个可运行的 Spring Boot + Vue3 配置管理界面，用来图形化维护边端使用的：

- Modbus 设备配置文件
- MQTT / MqttDriver 配置文件
- OTA 升级策略、升级包清单、发布记录

前端由 Spring Boot 直接提供静态资源，保存后的 JSON 可直接给边端驱动使用。

## 技术栈

- Java 21
- Spring Boot 3.3.x
- Vue3
- Element Plus
- Eclipse Paho MQTT v3/v5

## 启动

```bash
mvn spring-boot:run
```

默认访问：

```text
http://localhost:8080
```

## 配置目录

默认会把 JSON 配置文件保存在项目运行目录下的：

```text
config/
```

可通过 `application.yml` 修改：

```yaml
edge:
  gateway:
    config-base-dir: config
```

## 提供的接口

### 元信息

```http
GET /api/config/meta
```

### 设备配置

```http
GET /api/config/device?name=device-config.json
GET /api/config/device/template
PUT /api/config/device?name=device-config.json
```

### MQTT / OTA / App 配置

```http
GET /api/config/app?name=mqtt-app-config.json
GET /api/config/app/template
PUT /api/config/app?name=mqtt-app-config.json
```

### OTA 包管理

```http
GET /api/config/ota/packages
PUT /api/config/ota/packages
POST /api/config/ota/request
```

### OTA 发布

```http
GET /api/config/ota/publish-records
POST /api/config/ota/publish?appName=mqtt-app-config.json
```

## 页面功能

### Modbus 设备配置

支持编辑：

- `machineCode`
- `protocol.type`
- RTU 串口参数
- TCP 参数
- 共享内存与 SQLite 参数
- 多从站 `meters`
- 点位 `points`
- 变位上传 `reportOnChange`
- 读配置 `read`
- 写配置 `write`
- 告警规则 `alarms`
- 测点 Excel / CSV 导入

测点导入表头支持：

`index, pointCode, name, desc, category, address, enabled, isStore, reportOnChange, persistIntervalSec, tags, readEnable, readFunction, readLength, readDataType, readScale, readOffset, readByteOrder, readSigned, readUnit, readIntervalMs, readBit, storeLatest, storeHistory, historySize, ttlMs, writeEnable, writeFunction, writeLength, writeDataType, writeScale, writeOffset, writeByteOrder, writeMin, writeMax, writeStep, verifyAfterWrite, verifyDelayMs, verifyByRead`

页面支持：

- 下载导入模板：后端生成 `.xlsx`，第一行为字段名，第二行为中文说明
- 导入从站 + 点表：文件中带 `meterCode / deviceName / slave` 时可一次导入多个从站
- 导入单个从站点表：不带 `meterCode` 时按当前页面选中的仪表导入
- 下载设备配置：自动按串口命名为 `device_slave_<serial>.json`
  - `/dev/ttyS1 -> device_slave_ttyS1.json`
  - `/dev/ttyUSB0 -> device_slave_ttyUSB0.json`
  - TCP 模式 -> `device_slave_tcp.json`

### MQTT / OTA / App 配置

支持编辑：

- `deviceConfigFiles`
- MQTT broker / 账号 / topic
- `mqtt3` / `mqtt5`
- `mqttDriver`
- `alarmStore`
- `ota`
- OTA 升级包清单
- OTA 请求预览
- OTA 一键发布到 MQTT
- OTA 发布记录查询
- 实时监测页支持 MQTT 实时数据分片显示；后端收到 `chunked=true` 的 `telemetry / snapshot` 分片后会按片即时刷新缓存和 WebSocket 推送，不等待整批分片

## OTA 一键发布说明

页面会读取当前 `app` 配置中的：

- `mqtt.protocolVersion`
- `mqtt.broker`
- `mqtt.username`
- `mqtt.password`
- `mqtt.qos`
- `mqtt.otaRequestTopic`

点击“一键发布”后，后端会：

1. 根据选择的 OTA 包生成升级请求 JSON
2. 按 `mqtt3` 或 `mqtt5` 建立连接
3. 发布到 `otaRequestTopic/<machineCode>`
4. 将发布结果写入 `config/ota-publish-records.json`

## 生成配置的用途

设备配置文件用于边端 `ModbusRtu` 驱动。

MQTT 配置文件用于边端 `MqttDriver` 驱动。

OTA 包清单和发布记录用于云侧管理升级任务。

这些 JSON 结构已经按你前面 C++ 侧的配置格式对齐，保存后可以直接用于联调。


---

## Java edge-gateway/docs/realtime-monitoring-design.md

# 实时数据监测方案设计

## 1. 背景

当前系统已经具备以下基础能力：

- 边端 `ModbusRtu` 负责采集设备点位
- 边端 `MqttDriver` 可以从共享内存读取点位并上传 MQTT
- Java 侧 `edge-gateway` 已具备配置管理、OTA 管理、任务状态查询能力

下一步要做的是“实时数据监测”，目标不是只看配置，而是让平台页面可以实时看到边端上传的点位值、质量、时间戳、告警状态和变化趋势。

## 2. 目标

实时监测建议至少覆盖以下能力：

- 实时点位列表
- 单点详情
- 告警状态高亮
- 最近一段时间趋势曲线
- 在线状态判断
- 多网关 / 多设备筛选
- 页面低延迟更新

可选增强：

- 历史回放
- 点位订阅
- 异常波动检测
- 实时统计看板

## 3. 约束

结合当前架构，主要约束如下：

- Java 项目本身不直接访问边端共享内存
- 实时数据最自然的入口是 MQTT
- 前端是 Vue3，后端是 Spring Boot，适合做 WebSocket / SSE 推送
- 如果所有页面都直接订阅 MQTT，会导致浏览器和后端耦合过深，不利于权限和后续扩展

因此，推荐不要让前端直接连 MQTT Broker，而是由 Java 后端统一接入 MQTT，再转发给前端。

## 4. 可选方案

### 方案 A：前端定时轮询后端

流程：

1. 边端上传 MQTT
2. Java 后端消费 MQTT 并存最新值
3. 前端每 1s / 2s 轮询 REST 接口获取最新数据

优点：

- 最简单
- 开发快
- 调试容易

缺点：

- 页面多时请求量大
- 延迟高
- 趋势图体验差
- 不适合后续扩展到高频实时监测

适用：

- 初期验证
- 点位数量小
- 并发页面少

### 方案 B：后端 MQTT 消费 + SSE 推送前端

流程：

1. Java 后端订阅 MQTT 遥测主题
2. 后端将最新点位缓存到内存
3. 页面通过 SSE 建立长连接
4. 后端按网关 / 设备 / 点位过滤后把变化推送到前端

优点：

- 比轮询更实时
- 实现复杂度低于 WebSocket
- Spring Boot 很容易落地
- 适合“服务器单向推送监测数据”

缺点：

- 只适合服务端向浏览器单向推送
- 双向交互能力弱
- 连接规模大时控制能力一般

适用：

- 实时监测页
- 趋势、告警列表
- 中等规模并发

### 方案 C：后端 MQTT 消费 + WebSocket 推送前端

流程：

1. Java 后端订阅 MQTT 遥测 / 告警 / 状态主题
2. 后端维护实时缓存和订阅关系
3. 前端通过 WebSocket 建立连接
4. 前端发送订阅条件
5. 后端实时推送符合条件的数据

优点：

- 实时性最好
- 支持双向交互
- 适合做点位订阅、动态筛选、实时趋势、控制反馈
- 后续可以扩展到告警中心、设备状态页、在线诊断

缺点：

- 比 SSE 稍复杂
- 需要维护会话、订阅、心跳、断线重连

适用：

- 中长期正式方案
- 实时监测核心页面

### 方案 D：时序库 + 消息总线 + 独立监测服务

流程：

1. 边端上传 MQTT
2. 数据进入消息总线 / 流处理
3. 实时数据写入 Redis，历史写入时序库
4. 监测服务负责订阅和推送
5. Java 平台负责配置与展示

优点：

- 扩展性最好
- 监测、告警、历史趋势完全解耦
- 能支撑大规模数据

缺点：

- 建设成本高
- 当前阶段偏重

适用：

- 后续平台化阶段
- 多项目、多租户、大规模部署

## 5. 推荐方案

### 推荐结论

建议分两阶段：

- 第一阶段：`方案 B` 或 `方案 C`
- 最终推荐：`方案 C：后端 MQTT + WebSocket`

原因：

- 当前系统已经有 MQTT，Java 侧直接接入最顺
- 前端需要的不只是“看”，后续大概率还会要动态筛选、告警联动、详情订阅、设备状态页
- WebSocket 比 SSE 更适合做长期核心实时监测能力

## 6. 推荐架构

推荐新增以下模块：

### 6.1 后端模块

- `mqtt-realtime-ingest`
  - 订阅边端 MQTT 遥测主题、告警主题、状态主题
- `realtime-cache`
  - 内存缓存最新点位
  - 可选 Redis 做多实例共享
- `realtime-ws-gateway`
  - WebSocket 会话管理
  - 订阅条件管理
  - 实时消息推送
- `trend-query-service`
  - 查询近 5 分钟 / 1 小时趋势
  - 初期可查内存环形缓冲
  - 后期可接数据库

### 6.2 前端模块

- 实时总览页
- 网关监测页
- 设备监测页
- 单点趋势弹窗
- 告警实时列表

## 7. 数据流设计

### 7.1 上行实时数据流

1. 边端采集点位
2. `MqttDriver` 发布 MQTT 遥测
3. Java 后端 MQTT Client 订阅主题
4. 后端解析标准消息
5. 写入：
   - 最新值缓存
   - 趋势缓冲区
   - 告警状态缓存
6. 通过 WebSocket 向订阅页面推送

### 7.2 前端订阅流

1. 前端进入监测页
2. 建立 WebSocket 连接
3. 发送订阅条件：
   - `machineCode`
   - `meterCode`
   - `pointIndexes`
   - `categories`
4. 后端只推送匹配数据

## 8. 数据模型建议

### 8.1 实时点位消息

边端 `MqttDriver` 上传到 `edge/telemetry` 的实时消息可以是完整消息，也可以是分片消息。Java 后端统一按 `meters[].values[]` 解析，每个点位会转换成一条 WebSocket 点值消息推给前端。

```json
{
  "type": "point_realtime",
  "machineCode": "GW0001",
  "meterCode": "SLAVE0001",
  "index": 11000,
  "pointCode": "reg_0",
  "name": "VOLTAGE",
  "value": 220.5,
  "quality": 1,
  "ts": 1776200000000,
  "stale": false,
  "chunked": true,
  "chunkId": "1776906000000-1",
  "chunkIndex": 1,
  "chunkCount": 3,
  "alarm": {
    "active": false,
    "alarmType": ""
  }
}
```

分片处理规则：

- Java 后端不等待所有分片到齐，收到一片就解析该片 `values` 并刷新实时缓存。
- WebSocket 推送给前端的点值会保留 `chunked / chunkId / chunkIndex / chunkCount`。
- 前端实时监测页按 `machineCode / meterCode` 分组展示，并在分组和点表中显示分片进度。
- 如果后续需要“完整快照一致性校验”，再按 `machineCode + chunkId` 聚合 `chunkCount` 片；当前实时页面以低延迟刷新为优先。

### 8.2 告警消息

```json
{
  "type": "alarm_event",
  "machineCode": "GW0001",
  "meterCode": "SLAVE0001",
  "index": 11000,
  "alarmType": "high",
  "active": true,
  "value": 2400,
  "quality": 1,
  "ts": 1776200001000
}
```

### 8.3 WebSocket 订阅请求

```json
{
  "action": "subscribe",
  "machineCode": "GW0001",
  "meterCode": "SLAVE0001",
  "pointIndexes": [11000, 11001, 11002]
}
```

## 9. 页面设计建议

### 9.1 实时总览页

展示：

- 在线网关数量
- 在线设备数量
- 当前告警数量
- 最新更新时间
- 关键点位实时卡片

### 9.2 点位监测页

展示：

- 点位表格
- 当前值
- 质量
- 时间戳
- stale 状态
- 告警状态
- 趋势按钮

### 9.3 趋势弹窗

展示：

- 最近 5 分钟曲线
- 最近 1 小时曲线
- 最小值 / 最大值 / 平均值

## 10. 缓存与存储建议

### 10.1 最新值缓存

建议：

- 先用 Java 本地内存 `ConcurrentHashMap`
- key：
  - `machineCode:meterCode:index`

### 10.2 趋势缓冲

建议：

- 每个点位维护一个固定长度环形缓冲区
- 例如保留最近 300~3000 个点

优点：

- 快
- 不依赖数据库
- 足够支撑“短趋势”

### 10.3 历史库

如果后续要做长期趋势，建议：

- 短期：MySQL / PostgreSQL
- 中长期：InfluxDB / TimescaleDB

## 11. MQTT 主题建议

建议 Java 后端订阅：

- `edge/telemetry`
- `edge/alarm`
- `edge/status`

如果当前边端消息未完全区分，建议后续规范成分主题结构，例如：

- `edge/{machineCode}/telemetry`
- `edge/{machineCode}/alarm`
- `edge/{machineCode}/status`

这样更利于后端按网关过滤和权限控制。

## 12. 安全与权限

实时监测要考虑：

- 页面登录态校验
- WebSocket 鉴权
- 只能看自己授权网关
- 控制类 topic 与监测类 topic 分离

建议：

- WebSocket 连接建立时带 token
- 后端按用户可见网关过滤订阅条件

## 13. 推荐实施顺序

### 第一阶段

目标：

- 先把实时监测跑起来

内容：

- Java 后端接入 MQTT 订阅
- 增加实时缓存
- 增加 WebSocket 推送
- Vue 增加实时点位表格页

### 第二阶段

目标：

- 增加可用性

内容：

- 增加趋势缓冲
- 增加告警实时页
- 增加筛选和订阅控制

### 第三阶段

目标：

- 增加规模能力

内容：

- 引入 Redis 共享缓存
- 引入时序库存历史
- 多实例部署

## 14. 推荐最终方案

建议你当前项目采用：

### 推荐方案

- Java 后端统一订阅 MQTT
- 后端维护最新值缓存 + 短趋势缓冲
- 前端通过 WebSocket 实时订阅
- 后续再逐步加历史库存储

### 这样做的原因

- 与当前架构最匹配
- 复用现有 MQTT 能力
- 开发成本可控
- 能先快速交付实时监测页
- 后续还能自然扩展到告警中心、设备状态页和趋势分析

## 15. 本项目建议新增模块清单

如果确认实施，建议下一步新增：

- `src/main/java/.../realtime/RealtimeMqttConsumer.java`
- `src/main/java/.../realtime/RealtimePointCacheService.java`
- `src/main/java/.../realtime/RealtimeTrendBufferService.java`
- `src/main/java/.../realtime/RealtimeWebSocketConfig.java`
- `src/main/java/.../realtime/RealtimeWebSocketHandler.java`
- `src/main/java/.../web/RealtimeController.java`
- `src/main/resources/static/realtime.js`
- `src/main/resources/static/realtime.css`

## 16. 建议决策

建议你先确认以下 3 个点：

1. 实时监测是否以 MQTT 作为唯一入口
2. 前端实时通信是否采用 WebSocket
3. 历史趋势第一阶段是否只做“内存短趋势”，暂不接数据库

如果这 3 点确认，我下一步就可以按这个文档开始落实现。  


---

## 运行配置 JSON

### config/runtime/apps/mqtt-service.json

```json
{
  "deviceConfigFiles": [
    "/opt/modbus-gateway/config/runtime/devices/device_slave_ttySP1.json",
    "/opt/modbus-gateway/config/runtime/devices/device_slave_ttySP2.json",
    "/opt/modbus-gateway/config/runtime/devices/device_alarm_multi_slave_tcp_1_2.json",
    "/opt/modbus-gateway/config/runtime/devices/device_dlt645_multi_meter_1_2.json"
  ],
  "mqtt": {
    "enabled": true,
    "protocolVersion": "mqtt5",
    "broker": "tcp://192.168.22.102:1883",
    "clientId": "mqtt-driver-01",
    "username": "kyxn",
    "password": "whkyxn027",
    "telemetryTopic": "edge/telemetry",
    "changeEventTopic": "edge/event/change",
    "commandRequestTopic": "edge/command/request",
    "commandReplyTopic": "edge/command/reply",
    "otaRequestTopic": "edge/ota/request",
    "otaReplyTopic": "edge/ota/reply",
    "otaStatusTopic": "edge/ota/status",
    "qos": 1,
    "maxPayloadBytes": 49152,
    "cleanSession": true,
    "keepAliveSec": 60,
    "sessionExpirySec": 0,
    "offlineBuffer": {
      "enabled": true,
      "mode": "ring",
      "dir": "/opt/modbus-gateway/data/mqtt-spool",
      "realtimeFile": "/opt/modbus-gateway/data/mqtt-spool/realtime_ring.dat",
      "realtimeFileSizeBytes": 1073741824,
      "maxRealtimeMessageBytes": 4194304,
      "maxMemoryMessages": 200,
      "flushBatchSize": 10,
      "flushIntervalMs": 5000,
      "replayBatchSize": 20,
      "maxDiskBytes": 33554432,
      "eventOutbox": {
        "sqlitePath": "/opt/modbus-gateway/data/mqtt_event_outbox.db",
        "sqliteLibraryPath": "",
        "retentionMonths": 12,
        "cleanupIntervalHours": 24,
        "replayBatchSize": 100
      }
    },
    "alarmTopic": "edge/alarm",
    "statusTopic": "edge/status"
  },
  "mqttDriver": {
    "enabled": true,
    "sharedMemoryName": "gateway_point_store",
    "scanIntervalMs": 500,
    "fullUploadIntervalMs": 1000,
    "publishFullOnStart": true,
    "publishAllOnFull": true,
    "fullUploadIndexes": []
  },
  "alarmStore": {
    "enabled": true,
    "sqlitePath": "/opt/modbus-gateway/data/alarm_events.db",
    "sqliteLibraryPath": ""
  },
  "realtime": {
    "enabled": true,
    "telemetryTopic": "edge/telemetry",
    "alarmTopic": "edge/alarm",
    "statusTopic": "edge/status",
    "maxLatestPoints": 100000,
    "trendBufferSize": 300,
    "pushThrottleMs": 200
  },
  "ota": {
    "enabled": true,
    "currentVersion": "1.0.0",
    "artifactBaseUrl": "https://example.com/releases",
    "downloadDir": "/opt/modbus-gateway/ota/downloads",
    "stagingDir": "/opt/modbus-gateway/ota/staging",
    "backupDir": "/opt/modbus-gateway/ota/backup",
    "packageType": "tar.gz",
    "applyScript": "/opt/modbus-gateway/bin/ota-apply.sh",
    "rollbackScript": "/opt/modbus-gateway/bin/ota-rollback.sh",
    "checksumRequired": true,
    "autoReboot": false,
    "retentionCount": 3,
    "statusReportIntervalSec": 5,
    "upgradeTimeoutSec": 900,
    "storage": {
      "provider": "local",
      "presignExpireMinutes": 60,
      "minio": {
        "endpoint": "http://127.0.0.1:9000",
        "accessKey": "minioadmin",
        "secretKey": "minioadmin",
        "bucket": "edge-ota",
        "basePath": "packages",
        "publicBaseUrl": ""
      }
    }
  }
}
```

### config/runtime/devices/device_alarm_multi_slave_tcp_1_2.json

```json
{
  "schemaVersion": "1.0.0",
  "machineCode": "GW_TCP_01",
  "protocol": {
    "type": "modbus_tcp",
    "slave": 1,
    "tcp": {
      "host": "192.168.1.100",
      "port": 502,
      "connectTimeoutMs": 1000,
      "timeoutMs": 1000
    }
  },
  "collect": {
    "defaultIntervalMs": 5000,
    "batchOptimize": true,
    "maxBatchRegisters": 120
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
    "sqlitePath": "/opt/modbus-gateway/data/point_samples_tcp.db",
    "sqliteLibraryPath": "",
    "persistFlushIntervalMs": 60000,
    "writebackIntervalMs": 500,
    "writebackBatchSize": 100
  },
  "meters": [
    {
      "meterCode": "TCP_SLAVE0001",
      "deviceName": "Modbus TCP Unit 1",
      "slave": 1,
      "points": [
        {
          "index": 21000,
          "pointCode": "device_online",
          "name": "DEVICE_ONLINE",
          "desc": "TCP unit 1 online status",
          "category": "status",
          "address": 0,
          "enabled": true,
          "isStore": false,
          "reportOnChange": true,
          "persistIntervalSec": 60,
          "tags": ["modbus", "tcp", "status", "online"],
          "read": {
            "enable": true,
            "function": 0,
            "length": 0,
            "dataType": "device_online",
            "scale": 1,
            "offset": 0,
            "byteOrder": "AB",
            "signed": false,
            "unit": "",
            "intervalMs": 500,
            "cachePolicy": {
              "storeLatest": true,
              "storeHistory": true,
              "historySize": 100,
              "ttlMs": 600000
            }
          },
          "write": { "enable": false },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 21001,
          "pointCode": "reg_1",
          "name": "Register 1",
          "desc": "TCP unit 1 register 1",
          "category": "telemetry",
          "address": 1,
          "enabled": true,
          "isStore": false,
          "persistIntervalSec": 60,
          "tags": ["modbus", "tcp"],
          "read": {
            "enable": true,
            "function": 3,
            "length": 1,
            "dataType": "uint16",
            "scale": 1,
            "offset": 0,
            "byteOrder": "AB",
            "signed": false,
            "unit": "",
            "intervalMs": 500,
            "cachePolicy": {
              "storeLatest": true,
              "storeHistory": true,
              "historySize": 100,
              "ttlMs": 600000
            }
          },
          "write": {
            "enable": true,
            "function": 6,
            "length": 1,
            "dataType": "uint16",
            "scale": 1,
            "offset": 0,
            "byteOrder": "AB",
            "min": 0,
            "max": 65535,
            "step": 1,
            "verifyAfterWrite": true,
            "verifyDelayMs": 200,
            "verifyByRead": true
          },
          "alarms": [
            { "type": "high", "threshold": 1000, "reportRecovery": true, "persistValue": "" }
          ],
          "valueMap": null
        },
        {
          "index": 21002,
          "pointCode": "reg_2",
          "name": "Register 2",
          "desc": "TCP unit 1 register 2",
          "category": "telemetry",
          "address": 2,
          "enabled": true,
          "isStore": false,
          "persistIntervalSec": 60,
          "tags": ["modbus", "tcp"],
          "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": false },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 21003,
          "pointCode": "reg_3",
          "name": "Register 3",
          "desc": "TCP unit 1 register 3",
          "category": "telemetry",
          "address": 3,
          "enabled": true,
          "isStore": false,
          "persistIntervalSec": 60,
          "tags": ["modbus", "tcp"],
          "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": false },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 21004,
          "pointCode": "reg_4",
          "name": "Register 4",
          "desc": "TCP unit 1 register 4",
          "category": "telemetry",
          "address": 4,
          "enabled": true,
          "isStore": false,
          "persistIntervalSec": 60,
          "tags": ["modbus", "tcp"],
          "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": false },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 21005,
          "pointCode": "reg_5",
          "name": "Register 5",
          "desc": "TCP unit 1 register 5",
          "category": "telemetry",
          "address": 5,
          "enabled": true,
          "isStore": false,
          "persistIntervalSec": 60,
          "tags": ["modbus", "tcp"],
          "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": false },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 21006,
          "pointCode": "reg_6",
          "name": "Register 6",
          "desc": "TCP unit 1 register 6",
          "category": "telemetry",
          "address": 6,
          "enabled": true,
          "isStore": false,
          "persistIntervalSec": 60,
          "tags": ["modbus", "tcp"],
          "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": false },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 21007,
          "pointCode": "reg_7",
          "name": "Register 7",
          "desc": "TCP unit 1 register 7",
          "category": "telemetry",
          "address": 7,
          "enabled": true,
          "isStore": false,
          "persistIntervalSec": 60,
          "tags": ["modbus", "tcp"],
          "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": false },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 21008,
          "pointCode": "reg_8",
          "name": "Register 8",
          "desc": "TCP unit 1 register 8",
          "category": "telemetry",
          "address": 8,
          "enabled": true,
          "isStore": false,
          "persistIntervalSec": 60,
          "tags": ["modbus", "tcp"],
          "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": false },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 21009,
          "pointCode": "reg_9",
          "name": "Register 9",
          "desc": "TCP unit 1 register 9",
          "category": "telemetry",
          "address": 9,
          "enabled": true,
          "isStore": false,
          "persistIntervalSec": 60,
          "tags": ["modbus", "tcp"],
          "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": false },
          "alarms": [],
          "valueMap": null
        }
      ]
    },
    {
      "meterCode": "TCP_SLAVE0002",
      "deviceName": "Modbus TCP Unit 2",
      "slave": 2,
      "points": [
        {
          "index": 22000,
          "pointCode": "device_online",
          "name": "DEVICE_ONLINE",
          "desc": "TCP unit 2 online status",
          "category": "status",
          "address": 0,
          "enabled": true,
          "isStore": false,
          "reportOnChange": true,
          "persistIntervalSec": 60,
          "tags": ["modbus", "tcp", "status", "online"],
          "read": {
            "enable": true,
            "function": 0,
            "length": 0,
            "dataType": "device_online",
            "scale": 1,
            "offset": 0,
            "byteOrder": "AB",
            "signed": false,
            "unit": "",
            "intervalMs": 500,
            "cachePolicy": {
              "storeLatest": true,
              "storeHistory": true,
              "historySize": 100,
              "ttlMs": 600000
            }
          },
          "write": { "enable": false },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 22001,
          "pointCode": "reg_1",
          "name": "Register 1",
          "desc": "TCP unit 2 register 1",
          "category": "telemetry",
          "address": 1,
          "enabled": true,
          "isStore": false,
          "persistIntervalSec": 60,
          "tags": ["modbus", "tcp"],
          "read": {
            "enable": true,
            "function": 3,
            "length": 1,
            "dataType": "uint16",
            "scale": 1,
            "offset": 0,
            "byteOrder": "AB",
            "signed": false,
            "unit": "",
            "intervalMs": 500,
            "cachePolicy": {
              "storeLatest": true,
              "storeHistory": true,
              "historySize": 100,
              "ttlMs": 600000
            }
          },
          "write": { "enable": false },
          "alarms": [
            { "type": "high", "threshold": 1000, "reportRecovery": true, "persistValue": "" }
          ],
          "valueMap": null
        },
        {
          "index": 22002,
          "pointCode": "reg_2",
          "name": "Register 2",
          "desc": "TCP unit 2 register 2",
          "category": "telemetry",
          "address": 2,
          "enabled": true,
          "isStore": false,
          "persistIntervalSec": 60,
          "tags": ["modbus", "tcp"],
          "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": false },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 22003,
          "pointCode": "reg_3",
          "name": "Register 3",
          "desc": "TCP unit 2 register 3",
          "category": "telemetry",
          "address": 3,
          "enabled": true,
          "isStore": false,
          "persistIntervalSec": 60,
          "tags": ["modbus", "tcp"],
          "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": false },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 22004,
          "pointCode": "reg_4",
          "name": "Register 4",
          "desc": "TCP unit 2 register 4",
          "category": "telemetry",
          "address": 4,
          "enabled": true,
          "isStore": false,
          "persistIntervalSec": 60,
          "tags": ["modbus", "tcp"],
          "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": false },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 22005,
          "pointCode": "reg_5",
          "name": "Register 5",
          "desc": "TCP unit 2 register 5",
          "category": "telemetry",
          "address": 5,
          "enabled": true,
          "isStore": false,
          "persistIntervalSec": 60,
          "tags": ["modbus", "tcp"],
          "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": false },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 22006,
          "pointCode": "reg_6",
          "name": "Register 6",
          "desc": "TCP unit 2 register 6",
          "category": "telemetry",
          "address": 6,
          "enabled": true,
          "isStore": false,
          "persistIntervalSec": 60,
          "tags": ["modbus", "tcp"],
          "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": false },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 22007,
          "pointCode": "reg_7",
          "name": "Register 7",
          "desc": "TCP unit 2 register 7",
          "category": "telemetry",
          "address": 7,
          "enabled": true,
          "isStore": false,
          "persistIntervalSec": 60,
          "tags": ["modbus", "tcp"],
          "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": false },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 22008,
          "pointCode": "reg_8",
          "name": "Register 8",
          "desc": "TCP unit 2 register 8",
          "category": "telemetry",
          "address": 8,
          "enabled": true,
          "isStore": false,
          "persistIntervalSec": 60,
          "tags": ["modbus", "tcp"],
          "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": false },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 22009,
          "pointCode": "reg_9",
          "name": "Register 9",
          "desc": "TCP unit 2 register 9",
          "category": "telemetry",
          "address": 9,
          "enabled": true,
          "isStore": false,
          "persistIntervalSec": 60,
          "tags": ["modbus", "tcp"],
          "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": false },
          "alarms": [],
          "valueMap": null
        }
      ]
    }
  ]
}
```

### config/runtime/devices/device_dlt645_multi_meter_1_2.json

```json
{
  "schemaVersion": "1.0.0",
  "machineCode": "GW0001",
  "protocol": {
    "type": "dlt645_2007",
    "slave": 1,
    "transport": {
      "serialPort": "/dev/ttyS1",
      "baudRate": 2400,
      "dataBits": 8,
      "stopBits": 1,
      "parity": "E",
      "timeoutMs": 1000
    },
    "tcp": {
      "host": "127.0.0.1",
      "port": 502,
      "connectTimeoutMs": 1000,
      "timeoutMs": 1000
    },
    "standardPointsFile": "config/templates/dlt645_2007_standard_points.json",
    "standardPointsVersion": "1.1.0"
  },
  "dlt645": {
    "standardPointsFile": "config/templates/dlt645_2007_standard_points.json",
    "standardPointsVersion": "1.1.0"
  },
  "collect": {
    "defaultIntervalMs": 1000,
    "batchOptimize": false,
    "maxBatchRegisters": 1
  },
  "memoryStore": {
    "enabled": true,
    "backend": "memory",
    "keepHistory": 100,
    "defaultTtlMs": 600000,
    "indexBy": [
      "machineCode",
      "meterCode",
      "pointCode"
    ],
    "sharedMemoryName": "gateway_point_store",
    "maxLatestPoints": 100000,
    "maxPendingWrites": 4096,
    "maxPersistentSamples": 20000,
    "sqlitePath": "/opt/modbus-gateway/data/point_samples.db",
    "sqliteLibraryPath": "",
    "persistFlushIntervalMs": 60000,
    "writebackIntervalMs": 500,
    "writebackBatchSize": 100
  },
  "meters": [
    {
      "meterCode": "METER0001",
      "deviceName": "DLT645 Meter 1",
      "slave": 1,
      "address": "000000000001",
      "points": []
    },
    {
      "meterCode": "METER0002",
      "deviceName": "DLT645 Meter 2",
      "slave": 2,
      "address": "000000000002",
      "points": []
    }
  ]
}
```

### config/runtime/devices/device_slave_ttySP1.json

```json
{
  "schemaVersion": "1.0.0",
  "machineCode": "GW0001",
  "protocol": {
    "type": "modbus_rtu",
    "slave": 1,
    "transport": {
      "serialPort": "/dev/ttySP1",
      "baudRate": 9600,
      "dataBits": 8,
      "stopBits": 1,
      "parity": "N",
      "timeoutMs": 1000
    }
  },
  "collect": {
    "defaultIntervalMs": 5000,
    "batchOptimize": true,
    "maxBatchRegisters": 120
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
    "sqlitePath": "/opt/modbus-gateway/data/point_samples_serial1.db",
    "sqliteLibraryPath": "",
    "persistFlushIntervalMs": 60000,
    "writebackIntervalMs": 500,
    "writebackBatchSize": 100
  },
  "meters": [
    {
      "meterCode": "TTYSP1_SLAVE0001",
      "deviceName": "Serial1 Slave 1",
      "slave": 1,
      "points": [
        {
          "index": 31100,
          "pointCode": "device_online",
          "name": "DEVICE_ONLINE",
          "desc": "Serial1 slave 1 online status",
          "category": "status",
          "address": 0,
          "enabled": true,
          "isStore": false,
          "reportOnChange": true,
          "persistIntervalSec": 60,
          "tags": ["modbus", "serial1", "status", "online"],
          "read": { "enable": true, "function": 0, "length": 0, "dataType": "device_online", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": false },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 31101,
          "pointCode": "reg_1",
          "name": "Register 1",
          "desc": "Serial1 slave 1 register 1",
          "category": "telemetry",
          "address": 1,
          "enabled": true,
          "isStore": false,
          "reportOnChange": true,
          "persistIntervalSec": 60,
          "tags": ["modbus", "serial1"],
          "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": true, "function": 6, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "min": 0, "max": 65535, "step": 1, "verifyAfterWrite": true, "verifyDelayMs": 200, "verifyByRead": true },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 31102, "pointCode": "reg_2", "name": "Register 2", "desc": "Serial1 slave 1 register 2", "category": "telemetry", "address": 2, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial1"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 31103, "pointCode": "reg_3", "name": "Register 3", "desc": "Serial1 slave 1 register 3", "category": "telemetry", "address": 3, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial1"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 31104, "pointCode": "reg_4", "name": "Register 4", "desc": "Serial1 slave 1 register 4", "category": "telemetry", "address": 4, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial1"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 31105, "pointCode": "reg_5", "name": "Register 5", "desc": "Serial1 slave 1 register 5", "category": "telemetry", "address": 5, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial1"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 31106, "pointCode": "reg_6", "name": "Register 6", "desc": "Serial1 slave 1 register 6", "category": "telemetry", "address": 6, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial1"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 31107, "pointCode": "reg_7", "name": "Register 7", "desc": "Serial1 slave 1 register 7", "category": "telemetry", "address": 7, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial1"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 31108, "pointCode": "reg_8", "name": "Register 8", "desc": "Serial1 slave 1 register 8", "category": "telemetry", "address": 8, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial1"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 31109, "pointCode": "reg_9", "name": "Register 9", "desc": "Serial1 slave 1 register 9", "category": "telemetry", "address": 9, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial1"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null }
      ]
    },
    {
      "meterCode": "TTYSP1_SLAVE0002",
      "deviceName": "Serial1 Slave 2",
      "slave": 2,
      "points": [
        {
          "index": 31200,
          "pointCode": "device_online",
          "name": "DEVICE_ONLINE",
          "desc": "Serial1 slave 2 online status",
          "category": "status",
          "address": 0,
          "enabled": true,
          "isStore": false,
          "reportOnChange": true,
          "persistIntervalSec": 60,
          "tags": ["modbus", "serial1", "status", "online"],
          "read": { "enable": true, "function": 0, "length": 0, "dataType": "device_online", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": false },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 31201,
          "pointCode": "reg_1",
          "name": "Register 1",
          "desc": "Serial1 slave 2 register 1",
          "category": "telemetry",
          "address": 1,
          "enabled": true,
          "isStore": false,
          "reportOnChange": true,
          "persistIntervalSec": 60,
          "tags": ["modbus", "serial1"],
          "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": true, "function": 6, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "min": 0, "max": 65535, "step": 1, "verifyAfterWrite": true, "verifyDelayMs": 200, "verifyByRead": true },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 31202, "pointCode": "reg_2", "name": "Register 2", "desc": "Serial1 slave 2 register 2", "category": "telemetry", "address": 2, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial1"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 31203, "pointCode": "reg_3", "name": "Register 3", "desc": "Serial1 slave 2 register 3", "category": "telemetry", "address": 3, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial1"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 31204, "pointCode": "reg_4", "name": "Register 4", "desc": "Serial1 slave 2 register 4", "category": "telemetry", "address": 4, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial1"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 31205, "pointCode": "reg_5", "name": "Register 5", "desc": "Serial1 slave 2 register 5", "category": "telemetry", "address": 5, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial1"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 31206, "pointCode": "reg_6", "name": "Register 6", "desc": "Serial1 slave 2 register 6", "category": "telemetry", "address": 6, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial1"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 31207, "pointCode": "reg_7", "name": "Register 7", "desc": "Serial1 slave 2 register 7", "category": "telemetry", "address": 7, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial1"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 31208, "pointCode": "reg_8", "name": "Register 8", "desc": "Serial1 slave 2 register 8", "category": "telemetry", "address": 8, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial1"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 31209, "pointCode": "reg_9", "name": "Register 9", "desc": "Serial1 slave 2 register 9", "category": "telemetry", "address": 9, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial1"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null }
      ]
    }
  ]
}
```

### config/runtime/devices/device_slave_ttySP2.json

```json
{
  "schemaVersion": "1.0.0",
  "machineCode": "GW0001",
  "protocol": {
    "type": "modbus_rtu",
    "slave": 1,
    "transport": {
      "serialPort": "/dev/ttySP2",
      "baudRate": 9600,
      "dataBits": 8,
      "stopBits": 1,
      "parity": "N",
      "timeoutMs": 1000
    }
  },
  "collect": {
    "defaultIntervalMs": 5000,
    "batchOptimize": true,
    "maxBatchRegisters": 120
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
    "sqlitePath": "/opt/modbus-gateway/data/point_samples_serial2.db",
    "sqliteLibraryPath": "",
    "persistFlushIntervalMs": 60000,
    "writebackIntervalMs": 500,
    "writebackBatchSize": 100
  },
  "meters": [
    {
      "meterCode": "TTYSP2_SLAVE0001",
      "deviceName": "Serial2 Slave 1",
      "slave": 1,
      "points": [
        {
          "index": 32100,
          "pointCode": "device_online",
          "name": "DEVICE_ONLINE",
          "desc": "Serial2 slave 1 online status",
          "category": "status",
          "address": 0,
          "enabled": true,
          "isStore": false,
          "reportOnChange": true,
          "persistIntervalSec": 60,
          "tags": ["modbus", "serial2", "status", "online"],
          "read": { "enable": true, "function": 0, "length": 0, "dataType": "device_online", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": false },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 32101,
          "pointCode": "reg_1",
          "name": "Register 1",
          "desc": "Serial2 slave 1 register 1",
          "category": "telemetry",
          "address": 1,
          "enabled": true,
          "isStore": false,
          "reportOnChange": true,
          "persistIntervalSec": 60,
          "tags": ["modbus", "serial2"],
          "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": true, "function": 6, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "min": 0, "max": 65535, "step": 1, "verifyAfterWrite": true, "verifyDelayMs": 200, "verifyByRead": true },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 32102, "pointCode": "reg_2", "name": "Register 2", "desc": "Serial2 slave 1 register 2", "category": "telemetry", "address": 2, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial2"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 32103, "pointCode": "reg_3", "name": "Register 3", "desc": "Serial2 slave 1 register 3", "category": "telemetry", "address": 3, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial2"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 32104, "pointCode": "reg_4", "name": "Register 4", "desc": "Serial2 slave 1 register 4", "category": "telemetry", "address": 4, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial2"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 32105, "pointCode": "reg_5", "name": "Register 5", "desc": "Serial2 slave 1 register 5", "category": "telemetry", "address": 5, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial2"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 32106, "pointCode": "reg_6", "name": "Register 6", "desc": "Serial2 slave 1 register 6", "category": "telemetry", "address": 6, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial2"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 32107, "pointCode": "reg_7", "name": "Register 7", "desc": "Serial2 slave 1 register 7", "category": "telemetry", "address": 7, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial2"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 32108, "pointCode": "reg_8", "name": "Register 8", "desc": "Serial2 slave 1 register 8", "category": "telemetry", "address": 8, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial2"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 32109, "pointCode": "reg_9", "name": "Register 9", "desc": "Serial2 slave 1 register 9", "category": "telemetry", "address": 9, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial2"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null }
      ]
    },
    {
      "meterCode": "TTYSP2_SLAVE0002",
      "deviceName": "Serial2 Slave 2",
      "slave": 2,
      "points": [
        {
          "index": 32200,
          "pointCode": "device_online",
          "name": "DEVICE_ONLINE",
          "desc": "Serial2 slave 2 online status",
          "category": "status",
          "address": 0,
          "enabled": true,
          "isStore": false,
          "reportOnChange": true,
          "persistIntervalSec": 60,
          "tags": ["modbus", "serial2", "status", "online"],
          "read": { "enable": true, "function": 0, "length": 0, "dataType": "device_online", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": false },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 32201,
          "pointCode": "reg_1",
          "name": "Register 1",
          "desc": "Serial2 slave 2 register 1",
          "category": "telemetry",
          "address": 1,
          "enabled": true,
          "isStore": false,
          "reportOnChange": true,
          "persistIntervalSec": 60,
          "tags": ["modbus", "serial2"],
          "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } },
          "write": { "enable": true, "function": 6, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "min": 0, "max": 65535, "step": 1, "verifyAfterWrite": true, "verifyDelayMs": 200, "verifyByRead": true },
          "alarms": [],
          "valueMap": null
        },
        {
          "index": 32202, "pointCode": "reg_2", "name": "Register 2", "desc": "Serial2 slave 2 register 2", "category": "telemetry", "address": 2, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial2"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 32203, "pointCode": "reg_3", "name": "Register 3", "desc": "Serial2 slave 2 register 3", "category": "telemetry", "address": 3, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial2"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 32204, "pointCode": "reg_4", "name": "Register 4", "desc": "Serial2 slave 2 register 4", "category": "telemetry", "address": 4, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial2"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 32205, "pointCode": "reg_5", "name": "Register 5", "desc": "Serial2 slave 2 register 5", "category": "telemetry", "address": 5, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial2"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 32206, "pointCode": "reg_6", "name": "Register 6", "desc": "Serial2 slave 2 register 6", "category": "telemetry", "address": 6, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial2"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 32207, "pointCode": "reg_7", "name": "Register 7", "desc": "Serial2 slave 2 register 7", "category": "telemetry", "address": 7, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial2"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 32208, "pointCode": "reg_8", "name": "Register 8", "desc": "Serial2 slave 2 register 8", "category": "telemetry", "address": 8, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial2"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null },
        {
          "index": 32209, "pointCode": "reg_9", "name": "Register 9", "desc": "Serial2 slave 2 register 9", "category": "telemetry", "address": 9, "enabled": true, "isStore": false, "persistIntervalSec": 60, "tags": ["modbus", "serial2"], "read": { "enable": true, "function": 3, "length": 1, "dataType": "uint16", "scale": 1, "offset": 0, "byteOrder": "AB", "signed": false, "unit": "", "intervalMs": 500, "cachePolicy": { "storeLatest": true, "storeHistory": true, "historySize": 100, "ttlMs": 600000 } }, "write": { "enable": false }, "alarms": [], "valueMap": null }
      ]
    }
  ]
}
```

---

## 消息样例 JSON

### config/samples/messages/mqtt_alarm_event_example.json

```json
{
  "index": 11001,
  "alarmType": "high",
  "active": true,
  "value": 2400,
  "quality": 1,
  "ts": 1776156180000,
  "stale": false
}
```

### config/samples/messages/mqtt_command_request_example.json

```json
{
  "cmdId": "CMD2026041415300001",
  "machineCode": "GW0001",
  "meterCode": "SLAVE0001",
  "index": 11001,
  "pointCode": "reg_1",
  "value": 1,
  "source": "mqtt",
  "ts": 1776155909818
}
```

### config/samples/messages/mqtt_command_result_failed_example.json

```json
{
  "cmdId": "CMD2026041520000002",
  "machineCode": "GW0001",
  "meterCode": "SLAVE0001",
  "pointCode": "reg_1",
  "success": false,
  "message": "verify failed",
  "ts": 1776157205000,
  "index": 11001,
  "requestedValue": 2,
  "verifyAttempted": true,
  "verifyPassed": false
}
```

### config/samples/messages/mqtt_command_result_success_example.json

```json
{
  "cmdId": "CMD2026041520000001",
  "machineCode": "GW0001",
  "meterCode": "SLAVE0001",
  "pointCode": "reg_1",
  "success": true,
  "message": "ok",
  "ts": 1776157200000,
  "index": 11001,
  "requestedValue": 1,
  "verifyAttempted": true,
  "verifyPassed": true
}
```

### config/samples/messages/mqtt_ota_request_example.json

```json
{
  "jobId": "OTA2026041416000001",
  "machineCode": "GW0001",
  "artifactUrl": "https://example.com/releases/modbus-gateway-1.2.3.tar.gz",
  "version": "1.2.3",
  "sha256": "7d6f0d2d7c9f1d4d8b0c7b8e5a0c4e4b8d2c8a8d7f4e0a1b2c3d4e5f6a7b8c9d",
  "size": 10485760,
  "upgradeMode": "download_install_reboot",
  "ts": 1776156000000
}
```

### config/samples/messages/mqtt_ota_status_example.json

```json
{
  "jobId": "OTA2026041518300001",
  "machineCode": "GW0001",
  "stage": "applying",
  "progress": 70,
  "message": "running apply script",
  "ts": 1776156120000
}
```

### config/samples/messages/mqtt_ota_status_failed_example.json

```json
{
  "jobId": "OTA2026041520300001",
  "machineCode": "GW0001",
  "stage": "failed",
  "progress": 0,
  "message": "ota apply script failed",
  "ts": 1776157810000
}
```

### config/samples/messages/mqtt_status_command_accepted_example.json

```json
{
  "service": "mqtt-driver",
  "event": "command-accepted",
  "ts": 1776156240000,
  "cmdId": "CMD2026041519300001",
  "index": 11001,
  "meterCode": "SLAVE0001"
}
```

### config/samples/messages/mqtt_status_command_rejected_example.json

```json
{
  "service": "mqtt-driver",
  "event": "command-rejected",
  "ts": 1776156245000,
  "cmdId": "CMD2026041519300002",
  "index": 11099,
  "message": "command index not found"
}
```

### config/samples/messages/mqtt_status_dlt645_collect_failed_example.json

```json
{
  "service": "dlt645-daemon",
  "event": "collect-failed",
  "ts": 1776666660000,
  "meterCode": "METER0001",
  "slave": 1,
  "message": "DLT645 response timeout"
}
```

### config/samples/messages/mqtt_status_dlt645_started_example.json

```json
{
  "service": "dlt645-daemon",
  "event": "started",
  "ts": 1776666600000,
  "sharedMemory": "gateway_point_store",
  "mode": "dlt645_2007"
}
```

### config/samples/messages/mqtt_status_full_snapshot_example.json

```json
{
  "service": "mqtt-driver",
  "event": "full-snapshot",
  "valueCount": 20,
  "ts": 1776156060000
}
```

### config/samples/messages/mqtt_status_modbus_collect_failed_example.json

```json
{
  "service": "modbus-daemon",
  "event": "collect-failed",
  "ts": 1776156600000,
  "meterCode": "SLAVE0001",
  "slave": 1,
  "message": "modbus response too short"
}
```

### config/samples/messages/mqtt_status_modbus_persist_flushed_example.json

```json
{
  "service": "modbus-daemon",
  "event": "persist-flushed",
  "ts": 1776156620000,
  "count": 32
}
```

### config/samples/messages/mqtt_status_modbus_writeback_failed_example.json

```json
{
  "service": "modbus-daemon",
  "event": "writeback-failed",
  "ts": 1776156610000,
  "meterCode": "SLAVE0001",
  "index": 11001,
  "message": "verify failed"
}
```

### config/samples/messages/mqtt_status_ota_rejected_example.json

```json
{
  "service": "mqtt-driver",
  "event": "ota-rejected",
  "ts": 1776156250000,
  "jobId": "OTA2026041519300001",
  "message": "machineCode mismatch"
}
```

### config/samples/messages/mqtt_status_started_example.json

```json
{
  "service": "mqtt-driver",
  "event": "started",
  "scanIntervalMs": 1000,
  "fullUploadIntervalMs": 60000,
  "ts": 1776156000000
}
```

### config/samples/messages/mqtt_status_writeback_skipped_example.json

```json
{
  "service": "modbus-daemon",
  "event": "writeback-skipped",
  "ts": 1776157805000,
  "index": 99999,
  "reason": "unknown-index"
}
```

### config/samples/messages/mqtt_status_writeback_succeeded_example.json

```json
{
  "service": "modbus-daemon",
  "event": "writeback-succeeded",
  "ts": 1776157800000,
  "meterCode": "SLAVE0001",
  "index": 11001,
  "cmdId": "CMD2026041520200001"
}
```

### config/samples/messages/mqtt_telemetry_chunk_example.json

```json
{
  "type": "snapshot",
  "machineCode": "GW0001",
  "chunked": true,
  "chunkId": "1776906000000-1",
  "chunkIndex": 1,
  "chunkCount": 3,
  "meters": [
    {
      "meterCode": "TTYSP1_SLAVE0001",
      "values": [
        {
          "index": 31101,
          "pointCode": "reg_1",
          "value": 123,
          "quality": 1,
          "ts": 1776906000000,
          "expireAt": 1776906600000,
          "stale": false
        }
      ]
    }
  ]
}
```

### config/samples/messages/mqtt_telemetry_dlt645_example.json

```json
{
  "type": "telemetry",
  "machineCode": "GW0001",
  "meters": [
    {
      "meterCode": "METER0001",
      "values": [
        {
          "index": 200000,
          "pointCode": "forward_active_energy_total",
          "pointName": "正向有功总电能",
          "value": 12345.67,
          "quality": 1,
          "ts": 1776666600000,
          "expireAt": 1776667200000,
          "stale": false
        },
        {
          "index": 200009,
          "pointCode": "forward_active_energy_tariff_1",
          "pointName": "尖时正向有功电能",
          "value": 2345.67,
          "quality": 1,
          "ts": 1776666600000,
          "expireAt": 1776667200000,
          "stale": false
        }
      ]
    },
    {
      "meterCode": "METER0002",
      "values": [
        {
          "index": 210000,
          "pointCode": "forward_active_energy_total",
          "pointName": "正向有功总电能",
          "value": 22345.67,
          "quality": 1,
          "ts": 1776666600000,
          "expireAt": 1776667200000,
          "stale": false
        }
      ]
    }
  ]
}
```

---

## 部署脚本

### deploy/dlt645-driver@.service

```ini
[Unit]
Description=DLT645-2007 Driver Instance %i
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=/opt/modbus-gateway
ExecStart=/opt/modbus-gateway/bin/Dlt645Driver --config /opt/modbus-gateway/config/runtime/devices/%i.json
Restart=always
RestartSec=3
LimitNOFILE=65535

[Install]
WantedBy=multi-user.target
```

### deploy/gateway-services.service

```ini
[Unit]
Description=Gateway Driver Services Auto Launcher
After=network.target

[Service]
Type=oneshot
User=root
WorkingDirectory=/opt/modbus-gateway
ExecStart=/opt/modbus-gateway/bin/gateway-services.sh apply
ExecReload=/opt/modbus-gateway/bin/gateway-services.sh apply
ExecStop=/opt/modbus-gateway/bin/gateway-services.sh stop
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
```

### deploy/gateway-services.sh

```bash
#!/bin/sh
set -eu

BASE_DIR="${GATEWAY_HOME:-/opt/modbus-gateway}"
DEVICE_DIR="$BASE_DIR/config/runtime/devices"
APP_DIR="$BASE_DIR/config/runtime/apps"
MQTT_APP_NAME="${MQTT_APP_NAME:-mqtt-service}"

stop_units() {
  if ! command -v systemctl >/dev/null 2>&1; then
    return 0
  fi
  systemctl list-units --all --plain --no-legend \
    'modbus-rtu@*.service' \
    'dlt645-driver@*.service' \
    'mqtt-driver@*.service' 2>/dev/null |
    awk '{print $1}' |
    while IFS= read -r unit; do
      [ -z "$unit" ] && continue
      systemctl stop "$unit" || true
      systemctl disable "$unit" >/dev/null 2>&1 || true
      systemctl reset-failed "$unit" || true
    done
}

desired_units() {
  python3 - "$DEVICE_DIR" "$APP_DIR" "$MQTT_APP_NAME" <<'PY'
import json
import os
import sys

device_dir, app_dir, mqtt_app_name = sys.argv[1:4]

if os.path.isdir(device_dir):
    for name in sorted(os.listdir(device_dir)):
        if not name.endswith(".json"):
            continue
        path = os.path.join(device_dir, name)
        try:
            with open(path, "r", encoding="utf-8") as fh:
                root = json.load(fh)
        except Exception as exc:
            print(f"# skip invalid config {name}: {exc}", file=sys.stderr)
            continue

        protocol = str(root.get("protocol", {}).get("type", "")).lower()
        stem = name[:-5]
        if protocol in ("modbus_rtu", "modbus_tcp"):
            print(f"modbus-rtu@{stem}.service")
        elif protocol in ("dlt645_2007", "dlt645"):
            print(f"dlt645-driver@{stem}.service")

mqtt_path = os.path.join(app_dir, mqtt_app_name + ".json")
if os.path.isfile(mqtt_path):
    print(f"mqtt-driver@{mqtt_app_name}.service")
PY
}

start_units() {
  desired_units | while IFS= read -r unit; do
    [ -z "$unit" ] && continue
    case "$unit" in
      \#*) continue ;;
    esac
    echo "[gateway-services] starting $unit"
    systemctl start "$unit"
  done
}

case "${1:-apply}" in
  apply|restart)
    stop_units
    start_units
    ;;
  start)
    start_units
    ;;
  stop)
    stop_units
    ;;
  list)
    desired_units
    ;;
  *)
    echo "Usage: gateway-services.sh [apply|restart|start|stop|list]" >&2
    exit 2
    ;;
esac
```

### deploy/modbus-rtu@.service

```ini
[Unit]
Description=Modbus Driver Instance %i
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=/opt/modbus-gateway
ExecStart=/opt/modbus-gateway/bin/ModbusRtu --config /opt/modbus-gateway/config/runtime/devices/%i.json
Restart=always
RestartSec=3
LimitNOFILE=65535

[Install]
WantedBy=multi-user.target
```

### deploy/mqtt-driver@.service

```ini
[Unit]
Description=MQTT Driver Instance %i
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=/opt/modbus-gateway
ExecStart=/opt/modbus-gateway/bin/MqttDriver --app-config /opt/modbus-gateway/config/runtime/apps/%i.json
Restart=always
RestartSec=3
LimitNOFILE=65535

[Install]
WantedBy=multi-user.target
```

### deploy/ota-apply.sh

```bash
#!/bin/sh
set -eu

ARTIFACT_PATH="${1:-}"
VERSION="${2:-}"
JOB_ID="${3:-}"
BACKUP_DIR="${4:-}"
STAGING_DIR="${5:-}"

if [ -z "$ARTIFACT_PATH" ] || [ -z "$VERSION" ] || [ -z "$JOB_ID" ] || [ -z "$BACKUP_DIR" ] || [ -z "$STAGING_DIR" ]; then
  echo "[ota-apply] usage: ota-apply.sh <artifactPath> <version> <jobId> <backupDir> <stagingDir>" >&2
  exit 2
fi

TIMESTAMP="$(date '+%Y-%m-%d %H:%M:%S')"
ARTIFACT_NAME="$(basename "$ARTIFACT_PATH")"
WORK_DIR="$STAGING_DIR/$JOB_ID"
LOG_FILE="$STAGING_DIR/upgrade_history.log"
STATE_FILE="$STAGING_DIR/current_version.txt"
BACKUP_ARTIFACT="$BACKUP_DIR/${JOB_ID}_${ARTIFACT_NAME}"

mkdir -p "$BACKUP_DIR" "$STAGING_DIR" "$WORK_DIR"

echo "[$TIMESTAMP] [ota-apply] start jobId=$JOB_ID version=$VERSION artifact=$ARTIFACT_PATH" | tee -a "$LOG_FILE"

if [ ! -f "$ARTIFACT_PATH" ]; then
  echo "[$TIMESTAMP] [ota-apply] artifact not found: $ARTIFACT_PATH" | tee -a "$LOG_FILE" >&2
  exit 3
fi

cp "$ARTIFACT_PATH" "$BACKUP_ARTIFACT"
cp "$ARTIFACT_PATH" "$WORK_DIR/$ARTIFACT_NAME"

case "$ARTIFACT_NAME" in
  *.tar.gz|*.tgz)
    tar -xzf "$WORK_DIR/$ARTIFACT_NAME" -C "$WORK_DIR"
    ;;
  *.zip)
    unzip -oq "$WORK_DIR/$ARTIFACT_NAME" -d "$WORK_DIR"
    ;;
  *.bin|*.img)
    :
    ;;
  *)
    echo "[$TIMESTAMP] [ota-apply] unsupported package type: $ARTIFACT_NAME" | tee -a "$LOG_FILE" >&2
    exit 4
    ;;
esac

MANIFEST_PATH="$WORK_DIR/manifest.json"
RESTART_FILE=""
SYSTEMD_RELOAD_FILE=""
CHMOD_FILE=""
if [ -f "$MANIFEST_PATH" ]; then
  RESTART_FILE="$WORK_DIR/restart_services.txt"
  SYSTEMD_RELOAD_FILE="$WORK_DIR/systemd_reload_required"
  CHMOD_FILE="$WORK_DIR/chmod_targets.txt"
  python3 - "$MANIFEST_PATH" "$BACKUP_DIR" "$LOG_FILE" "$RESTART_FILE" "$SYSTEMD_RELOAD_FILE" "$CHMOD_FILE" <<'PY'
import json
import glob
import os
import shutil
import sys

manifest_path, backup_dir, log_file, restart_file, systemd_reload_file, chmod_file = sys.argv[1:7]
root_dir = os.path.dirname(manifest_path)
need_systemd_reload = False
chmod_targets = []

with open(manifest_path, "r", encoding="utf-8") as fh:
    manifest = json.load(fh)

allowed_clean_roots = (
    "/opt/modbus-gateway/config/runtime/devices",
    "/opt/modbus-gateway/config/runtime/apps",
)

for rule in manifest.get("cleanBeforeApply", []):
    target = os.path.abspath(rule.get("target", ""))
    if target not in allowed_clean_roots:
        with open(log_file, "a", encoding="utf-8") as log:
            log.write(f"[manifest-clean-skip] unsafe target {target}\n")
        continue
    patterns = rule.get("patterns", ["*.json"])
    for pattern in patterns:
        if os.path.basename(pattern) != pattern:
            with open(log_file, "a", encoding="utf-8") as log:
                log.write(f"[manifest-clean-skip] unsafe pattern {pattern}\n")
            continue
        for path in glob.glob(os.path.join(target, pattern)):
            if not os.path.isfile(path):
                continue
            backup_path = os.path.join(backup_dir, os.path.relpath(path, "/"))
            os.makedirs(os.path.dirname(backup_path), exist_ok=True)
            shutil.copy2(path, backup_path)
            os.remove(path)
            with open(log_file, "a", encoding="utf-8") as log:
                log.write(f"[manifest-clean] {path}\n")

for item in manifest.get("files", []):
    src = os.path.join(root_dir, item["path"])
    dst = item["target"]
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.exists(dst):
        backup_path = os.path.join(backup_dir, os.path.relpath(dst, "/"))
        os.makedirs(os.path.dirname(backup_path), exist_ok=True)
        shutil.copy2(dst, backup_path)
    shutil.copy2(src, dst)
    if dst.startswith("/etc/systemd/system/") and dst.endswith(".service"):
        need_systemd_reload = True
    if dst.startswith("/opt/modbus-gateway/bin/") and (dst.endswith(".sh") or os.access(src, os.X_OK)):
        chmod_targets.append(dst)
    with open(log_file, "a", encoding="utf-8") as log:
        log.write(f"[manifest-copy] {src} -> {dst}\n")

with open(restart_file, "w", encoding="utf-8") as fh:
    for item in manifest.get("restart", {}).get("services", []):
        fh.write(item + "\n")
if need_systemd_reload:
    with open(systemd_reload_file, "w", encoding="utf-8") as fh:
        fh.write("1\n")
with open(chmod_file, "w", encoding="utf-8") as fh:
    for item in chmod_targets:
        fh.write(item + "\n")
PY
fi

if [ -n "$CHMOD_FILE" ] && [ -f "$CHMOD_FILE" ]; then
  while IFS= read -r target; do
    [ -z "$target" ] && continue
    chmod +x "$target" || echo "[$TIMESTAMP] [ota-apply] chmod failed $target" >> "$LOG_FILE"
  done < "$CHMOD_FILE"
fi

{
  echo "jobId=$JOB_ID"
  echo "version=$VERSION"
  echo "artifact=$ARTIFACT_PATH"
  echo "backupArtifact=$BACKUP_ARTIFACT"
  echo "workDir=$WORK_DIR"
  echo "appliedAt=$TIMESTAMP"
} > "$STATE_FILE"

echo "$VERSION" > "$STAGING_DIR/applied_version.txt"
echo "[$TIMESTAMP] [ota-apply] success jobId=$JOB_ID version=$VERSION workDir=$WORK_DIR" | tee -a "$LOG_FILE"

if command -v systemctl >/dev/null 2>&1 && [ -n "$RESTART_FILE" ] && [ -f "$RESTART_FILE" ]; then
  RESTART_LATER="$WORK_DIR/restart_later.sh"
  cat > "$RESTART_LATER" <<EOF
#!/bin/sh
sleep 2
if [ -f "$SYSTEMD_RELOAD_FILE" ]; then
  echo "[$TIMESTAMP] [ota-apply] systemctl daemon-reload" >> "$LOG_FILE"
  systemctl daemon-reload >> "$LOG_FILE" 2>&1 || echo "[$TIMESTAMP] [ota-apply] daemon-reload failed" >> "$LOG_FILE"
fi
while IFS= read -r service; do
  [ -z "\$service" ] && continue
  if [ "\$service" = "gateway-services.service" ]; then
    systemctl enable "\$service" >> "$LOG_FILE" 2>&1 || echo "[$TIMESTAMP] [ota-apply] enable failed \$service" >> "$LOG_FILE"
  fi
  echo "[$TIMESTAMP] [ota-apply] restarting \$service" >> "$LOG_FILE"
  systemctl restart "\$service" >> "$LOG_FILE" 2>&1 || echo "[$TIMESTAMP] [ota-apply] restart failed \$service" >> "$LOG_FILE"
done < "$RESTART_FILE"
EOF
  chmod +x "$RESTART_LATER"
  nohup sh "$RESTART_LATER" >/dev/null 2>&1 &
fi

exit 0
```

## 压测记录：200 台 Modbus 设备 x 40 点，4 路 / 8 路 RS485 对比

这一轮压测不再使用之前的通用 9790 点模型，而是按实际部署问题重建：

- Modbus 设备：200 台
- 每台设备点数：40
- 每台设备寄存器分两段连续读取：
  - `0-19`
  - `100-119`
- 总点数：`200 * 40 = 8000`
- 事件策略：
  - 每台设备仅首个状态点开启 `reportOnChange=true`
  - 全部点位开启 `high` 告警，阈值 `250`
- 事件链路：
  - `EventEngine`
  - `MqttDriver(mqtt_driver_outbox)`
- MQTT 外发关闭，仅测边端内部综合能力：
  - `mqtt.enabled=false`

### 压测模型

分别构造两组模型：

- 4 路 RS485：
  - `200` 台设备均分到 `4` 个串口
  - 每路 `50` 台
  - 对应 `4` 个共享内存分片
- 8 路 RS485：
  - `200` 台设备均分到 `8` 个串口
  - 每路 `25` 台
  - 对应 `8` 个共享内存分片

说明：

- 本轮 `writer` 数量与串口数保持一致：
  - 4 路模型：`writer=4`
  - 8 路模型：`writer=8`
- 这里的 `writer` 仍然是压测工具里的“并发数据生产者线程”，不是实际串口读线程。
- 这轮压测回答的是“软件链路综合处理能力”和“4 路 / 8 路接口分摊后的处理差异”，不是物理串口最终极限轮询周期。

### 压测结果

压测时长：`90s`

#### 4 路 RS485 模型

- `writers=4`
- `writeOps=5,138,000`
- `opsPerSec=57,088`
- `batchAvgUs=140,148`
- `batchMaxUs=52,330,234`
- `snapshots=38`
- `snapshotAvgUs=1,372,758`
- `snapshotMaxUs=21,728,050`
- `snapshotPoints=294,387`
- `mqtt_event_outbox total=3676`
- `mqtt_event_outbox unsent=0`
- `mqtt_event_outbox sent=3676`
- `alarm_events=3676`

事件类型分布：

- `alarm=3676`
- `change=0`

#### 8 路 RS485 模型

- `writers=8`
- `writeOps=18,036,000`
- `opsPerSec=200,400`
- `batchAvgUs=39,920`
- `batchMaxUs=30,877,881`
- `snapshots=56`
- `snapshotAvgUs=615,236`
- `snapshotMaxUs=18,204,858`
- `snapshotPoints=444,233`
- `mqtt_event_outbox total=2728`
- `mqtt_event_outbox unsent=0`
- `mqtt_event_outbox sent=2728`
- `alarm_events=2728`

事件类型分布：

- `alarm=2728`
- `change=0`

### 结果解读

从边端内部软件链路看，8 路模型明显优于 4 路模型：

- 共享内存写入吞吐：
  - `57,088 ops/s` -> `200,400 ops/s`
  - 提升约 `3.5x`
- snapshot 平均耗时：
  - `1.37s` -> `0.62s`
  - 下降约 `55%`
- 两组模型 `outbox unsent=0`
  - 说明在这一组真实模型下，事件 replay 没有形成持续积压

这说明在“200 台设备、每台 40 点、两段连续寄存器”的规模下：

- 把设备均分到 `8` 路 RS485，比压在 `4` 路上更合理
- 继续增加软件 writer 线程没有意义，接口分摊才是主要收益来源

### 真实串口轮询周期估算

为了避免把“软件压测吞吐”误认为“真实 RTU 轮询周期”，这里单独给出一个按 `9600 8N1` 的估算。

估算前提：

- 每台设备两次读请求：
  - 一次读 `20` 个寄存器：`0-19`
  - 一次读 `20` 个寄存器：`100-119`
- 单次 Modbus RTU 事务大致字节数：
  - 请求：`8` 字节
  - 响应：`45` 字节
  - 合计：`53` 字节
- `9600 8N1` 按每字节约 `10` 位估算
- 再加上 RTU 帧间隔、从站响应准备、主站调度，单次事务按 `65ms - 75ms` 粗估

则每台设备两次事务约：

- `130ms - 150ms`

#### 4 路 RS485

- `200 / 4 = 50` 台每路
- 每路轮询周期约：
  - `50 * (130ms - 150ms)`
  - 约 `6.5s - 7.5s`

#### 8 路 RS485

- `200 / 8 = 25` 台每路
- 每路轮询周期约：
  - `25 * (130ms - 150ms)`
  - 约 `3.25s - 3.75s`

### 部署建议

如果目标是：

- 尽量缩短现场整轮采集周期
- 保证告警 / 事件链路还有余量
- 给后续 CAN / DI / DO / 其他协议预留 CPU

建议优先采用：

- `8` 路 RS485 均匀分摊 Modbus RTU 设备

不建议采用：

- 大量设备集中到 `4` 路 RS485，再依赖软件线程数硬顶

建议的现场口径：

- 这台设备在 `200 x 40` 规模下，软件链路能承受
- 真正瓶颈主要转向 RTU 串口物理轮询周期
- 若现场仍要求更快刷新，应优先：
  - 提升波特率
  - 继续均匀拆分串口负载
  - 进一步减少每台设备每轮事务数

## 压测记录：200 台 Modbus 设备 x 350 点，8 个连续段

为了更贴近现场“每台设备大量点位、连续寄存器段很多”的情况，又补了一轮更重的模型：

- Modbus 设备：200 台
- 每台设备点数：350
- 每台设备拆成 8 个连续段
- 总点数：`200 * 350 = 70000`

连续段规划如下：

- `0-43`
- `100-143`
- `200-243`
- `300-343`
- `400-443`
- `500-543`
- `600-642`
- `700-742`

事件策略：

- 每台设备仅第 1 个状态点开启 `reportOnChange=true`
- 全部点位开启 `high` 告警，阈值 `250`
- `mqtt.enabled=false`
- `EventEngine + MqttDriver(outbox)` 同时参与

### 模型划分

- 4 路 RS485：
  - 200 台设备均分到 4 个串口
  - 每路 50 台
  - `shmCount=4`
- 8 路 RS485：
  - 200 台设备均分到 8 个串口
  - 每路 25 台
  - `shmCount=8`

### 20 秒压测结果

#### 4 路 RS485

- `writers=4`
- `points=70000`
- `shmCount=4`
- `writeOps=70000`
- `opsPerSec=3500`
- `batchAvgUs=261,733,250`
- `batchMaxUs=290,179,629`
- `snapshots=1`
- `snapshotAvgUs=246,845,595`
- `snapshotPoints=47,712`
- `alarm_events=0`
- `mqtt_event_outbox=missing`

#### 8 路 RS485

- `writers=8`
- `points=70000`
- `shmCount=8`
- `writeOps=70000`
- `opsPerSec=3500`
- `batchAvgUs=241,732,449`
- `batchMaxUs=250,064,820`
- `snapshots=1`
- `snapshotAvgUs=214,804,140`
- `snapshotPoints=42,282`
- `alarm_events=0`
- `mqtt_event_outbox=missing`

### 结果解读

这轮结果说明，`70000` 点已经明显超过当前边端软件链路的舒适区：

- 20 秒内只完成了 1 轮点表写入
- 单批写入平均耗时已经到 `241s - 262s` 量级
- 单次 snapshot 耗时也已经达到 `214s - 246s` 量级
- `EventEngine` 尚未进入真正稳定消费阶段，因此还没产生有效 `alarm/outbox`

也就是说：

- 这不是事件链路优化还不够的问题
- 而是总点量和总事务规模已经把整体链路推到了极限之外

### 4 路与 8 路对比

在这个超大模型下，8 路仍然优于 4 路，但提升幅度已经远小于中小规模模型：

- `batchAvgUs`
  - `261.7s` -> `241.7s`
- `snapshotAvgUs`
  - `246.8s` -> `214.8s`

说明当总点位规模膨胀到 `70000` 时：

- 串口分片仍然有帮助
- 但瓶颈已经不仅是“分几路串口”，而是：
  - 点位总量过大
  - 每台设备事务段数过多
  - snapshot 遍历和编码本身开销巨大

### 结论

对当前这台设备而言：

- `200 x 350 x 8段 = 70000 点`
- 不适合作为单网关长期稳定部署目标

更合理的工程建议是：

- 优先控制每台设备真正需要采集的点位
- 优先减少每台设备每轮事务段数
- 继续采用 8 路 RS485 均匀分摊
- 若现场确实存在这种规模，应考虑：
  - 拆成多台网关
  - 提升串口波特率
  - 对点位做分层采集，区分高频 / 低频
  - 避免对大量模拟量同时启用告警和变位

## 按点数 / 段数自动拆网关规划器

为了解决“设备很多、点位很多、连续段很多时，应该拆成几台网关、每台网关的 8 路 RS485 应该怎么挂设备”的问题，仓库新增了一个离线规划器：

- [gateway_split_planner.py](D:/workspace/Embedded/Gateway-zk/tools/gateway_split_planner.py)

这个工具不直接生成最终 `device.json`，而是先做“容量规划”：

- 按每台设备的 `pointCount`
- 按每台设备的 `segmentCount`
- 按每台网关总点数 / 总段数上限
- 按每路 RS485 点数 / 段数上限
- 按“同一路串口只能跑一个协议”

输出建议的：

- 需要多少台网关
- 每台网关分配哪些设备
- 每台网关每路 RS485 分配哪些设备

### 适用场景

适合在以下阶段使用：

- Java 图形化批量生成前，先做容量规划
- 已有 Excel / JSON 点表后，先估算是否需要拆多台网关
- 做现场部署方案评审时，先给出“建议最少网关数”和“每路串口挂载建议”

### 输入格式

样例文件：

- [gateway_split_inventory_example.json](D:/workspace/Embedded/Gateway-zk/config/examples/gateway_split_inventory_example.json)

核心字段：

- `gatewayModel.machineCodePrefix`
  - 生成规划结果时使用的网关编码前缀
- `gatewayModel.rs485PortsPerGateway`
  - 每台网关可用 RS485 口数量，当前一般填 `8`
- `limits.maxPointsPerGateway`
  - 单台网关建议最大点数
- `limits.maxSegmentsPerGateway`
  - 单台网关建议最大连续段数
- `limits.maxPointsPerPort`
  - 单路 RS485 建议最大点数
- `limits.maxSegmentsPerPort`
  - 单路 RS485 建议最大连续段数
- `devices[]`
  - 每台现场设备的负载描述

单个设备对象字段：

- `meterCode`
  - 设备编码
- `protocol`
  - 协议类型，如 `modbus_rtu`、`dlt645_2007`
- `pointCount`
  - 该设备实际采集点数
- `segmentCount`
  - 该设备最终读任务段数
- `transport`
  - 传输介质，默认 `rs485`
- `preferredGateway`
  - 可选，指定希望落到哪个网关
- `preferredPort`
  - 可选，指定希望落到哪个串口

### 规则说明

规划器当前采用保守规则：

1. 一个设备必须完整落到一个网关，不能拆到多个网关。
2. 一个设备必须完整落到一条串口，不能拆到多条串口。
3. 同一条串口只能承载一种协议。
4. 任一设备如果单独就超过：
   - `maxPointsPerGateway`
   - `maxSegmentsPerGateway`
   - `maxPointsPerPort`
   - `maxSegmentsPerPort`
   则直接标记为 `rejected`。
5. 设备优先按“大段数、大点数”先放，避免后面大设备无处可放。

### 使用方式

示例：

```bash
python tools/gateway_split_planner.py \
  --input config/examples/gateway_split_inventory_example.json \
  --pretty
```

输出到文件：

```bash
python tools/gateway_split_planner.py \
  --input config/examples/gateway_split_inventory_example.json \
  --output gateway_split_plan.json \
  --pretty
```

### 输出结果

输出 JSON 中最重要的是：

- `summary.gatewayCount`
  - 规划后需要的网关数量
- `summary.gateways[]`
  - 每台网关的汇总
- `summary.gateways[].ports[]`
  - 每路串口的汇总
- `summary.rejected[]`
  - 无法落入任何单台网关 / 单路串口容量约束的设备

每台网关会给出：

- `machineCode`
- `deviceCount`
- `pointCount`
- `segmentCount`
- `portCount`
- `ports[]`

每路串口会给出：

- `portCode`
- `protocol`
- `deviceCount`
- `pointCount`
- `segmentCount`
- `meterCodes[]`

### 推荐用法

现场落地建议按下面顺序做：

1. 从设备点表统计每台设备的：
   - `pointCount`
   - `segmentCount`
2. 先用规划器跑一轮，得出：
   - 建议网关数量
   - 每台网关每路 RS485 的挂载分布
3. 再由 Java 批量配置页按规划结果生成：
   - `device_slave_xxx.json`
   - `mqtt-service.json`
   - OTA 配置包

这样可以避免：

- 先生成一堆配置，最后才发现单台网关根本装不下
- 串口负载极不均衡
- 某一路 RS485 上段数爆炸，导致轮询周期失控

## 边端资源监测与远程诊断方案

### 目标

平台侧希望具备两类能力：

1. 查看边端设备本身资源占用
   - CPU
   - 内存
   - 磁盘
   - 关键进程状态
2. 在平台侧登录后，对边端发起有限诊断操作，并拿回执行结果

约束：

- 平台监测页面未打开时，不持续高频上报资源数据
- 资源指标超过阈值时，边端必须主动上报异常
- 不实现“任意 Linux 命令执行”
- 只允许白名单诊断命令

### 总体设计

边端新增一个独立的系统监测服务，例如：

- `SystemMonitor`

Java 平台新增两类能力：

- 实时监测订阅页
- 远程诊断命令页

整体链路：

```text
Java 平台监测页打开
  -> 下发 monitor subscribe
  -> 边端开始按短周期采集系统资源并上报

Java 平台监测页关闭 / 超时未续约
  -> 边端停止短周期资源上报

边端资源超过阈值
  -> 不依赖页面订阅
  -> 立即主动上报 resource-alert

Java 平台登录后发起诊断命令
  -> MQTT 下发诊断请求
  -> 边端执行白名单命令
  -> 返回 stdout/stderr/exitCode
  -> 平台记录审计日志
```

### 边端资源监测

#### 采集指标

第一阶段建议支持：

- CPU 使用率
- 内存使用率
- 根分区磁盘使用率
- 负载均值
- 进程总数
- 关键服务存活状态
  - `ModbusRtu`
  - `Dlt645Driver`
  - `MqttDriver`
  - `EventEngine`

可选扩展：

- 每个关键驱动进程 CPU / RSS
- 网络吞吐
- 文件句柄数
- 温度

#### 资源采集方式

Linux 上优先直接读取系统文件，避免调用重命令：

- CPU：`/proc/stat`
- 内存：`/proc/meminfo`
- 负载：`/proc/loadavg`
- 进程数：`/proc`
- 磁盘：`statvfs("/")`

关键进程状态可通过：

- `/proc/<pid>`
- 或 `pidof`
- 或已有 systemd 服务名做一次轻量查询

原则：

- 高频指标尽量用 `/proc` 和系统调用
- 不依赖 `top`、`free`、`df` 这类外部命令做周期采集

### 上报策略

#### 页面打开时才高频上报

平台端监测页打开时：

- Java 平台向边端发送订阅请求
- 内容包含：
  - `machineCode`
  - `sessionId`
  - `intervalMs`
  - `ttlSec`

边端收到后：

- 建立一条监测租约
- 在租约有效期内按 `intervalMs` 上报资源数据

页面关闭或断链时：

- 平台端主动发送取消订阅
- 或不再续约
- 边端在 `ttlSec` 超时后自动停止主动上报

这样可以保证：

- 平台无人查看时，不做高频资源上传
- 避免所有边端长期向平台刷无意义监测流量

#### 超阈值主动上报

即使没有订阅，也必须支持主动异常上报：

- CPU `>= 90%`
- 内存 `>= 90%`
- 磁盘 `>= 90%`
- 关键进程退出

触发时：

- 边端立即发布 `resource-alert`
- 平台侧写入告警/事件流

建议做简单抖动控制：

- 同一告警条件持续存在时，不要每秒狂发
- 可按 `30s` 或 `60s` 节流重复告警
- 恢复时发一条 `recovered`

### MQTT 主题设计

建议新增主题：

- 订阅请求：`edge/system/monitor/request`
- 订阅回复：`edge/system/monitor/reply`
- 资源实时：`edge/system/monitor/telemetry`
- 资源告警：`edge/system/monitor/alert`
- 诊断请求：`edge/system/diag/request`
- 诊断回复：`edge/system/diag/reply`

#### 资源实时消息样例

```json
{
  "type": "system-monitor",
  "machineCode": "GW0001",
  "sessionId": "MONITOR_20260424_001",
  "cpuUsage": 72.4,
  "memUsage": 61.8,
  "diskUsage": 48.2,
  "load1": 1.37,
  "processCount": 96,
  "services": [
    {"name": "ModbusRtu", "running": true},
    {"name": "MqttDriver", "running": true},
    {"name": "EventEngine", "running": true}
  ],
  "ts": 1777000000000
}
```

#### 资源告警消息样例

```json
{
  "type": "system-alert",
  "machineCode": "GW0001",
  "metric": "cpuUsage",
  "level": "critical",
  "value": 93.2,
  "threshold": 90.0,
  "active": true,
  "message": "cpu usage too high",
  "ts": 1777000000000
}
```

### 平台端展示

Java 平台建议增加：

- 边端资源监测页
- 远程诊断页

资源监测页能力：

- 登录后可查看边端当前资源
- 打开页面时自动下发订阅
- 关闭页面时取消订阅
- 展示：
  - CPU
  - 内存
  - 磁盘
  - 负载
  - 服务状态
- 支持最近一段时间趋势

平台端还应提供：

- 当前资源接口
- 最近资源历史接口
- 当前异常列表接口

### 远程诊断命令

#### 安全边界

不允许平台直接发送任意 shell 命令。

必须采用白名单命令模型。

原因：

- 任意 shell 等于远程 root 执行面
- 容易造成误删、提权、数据泄漏
- 也很难做审计和权限控制

#### 白名单建议

第一阶段建议只支持只读诊断命令：

- `uptime`
- `free_m`
- `df_root`
- `top_once`
- `ps_gateway`
- `journal_tail`
- `systemctl_status`

对应执行逻辑：

- `uptime` -> `uptime`
- `free_m` -> `free -m`
- `df_root` -> `df -h /`
- `top_once` -> `top -b -n 1`
- `ps_gateway` -> `ps | grep -E 'ModbusRtu|Dlt645Driver|MqttDriver|EventEngine'`
- `journal_tail` -> `journalctl -n <N>`
- `systemctl_status` -> `systemctl status <allowed-service>`

其中参数也必须白名单化：

- `journal_tail`
  - 只允许 `tailLines`
- `systemctl_status`
  - 只允许平台预定义服务名

不允许：

- 任意拼接参数
- 任意管道
- 任意重定向
- 任意文件路径

#### 诊断请求样例

```json
{
  "cmdId": "DIAG_20260424_001",
  "machineCode": "GW0001",
  "command": "df_root",
  "args": {},
  "operator": "admin",
  "ts": 1777000000000
}
```

#### 诊断回复样例

```json
{
  "cmdId": "DIAG_20260424_001",
  "machineCode": "GW0001",
  "success": true,
  "command": "df_root",
  "exitCode": 0,
  "stdout": "Filesystem Size Used Avail Use% Mounted on ...",
  "stderr": "",
  "ts": 1777000000500
}
```

### 平台侧鉴权与审计

平台侧必须满足：

- 用户登录后才能看到诊断入口
- 诊断命令需校验账号权限
- 每次下发必须记录审计日志

审计日志建议记录：

- 操作人
- machineCode
- 命令类型
- 参数
- 下发时间
- 返回时间
- 是否成功
- 摘要结果

如果后续需要更严：

- 可增加二次确认
- 可限制只有管理员角色能执行
- 可限制生产环境只能执行只读命令

### 边端实现建议

边端建议新增一个独立服务：

- `SystemMonitor`

职责：

- 周期采集系统资源
- 管理监测订阅租约
- 处理白名单诊断命令
- 发布资源遥测 / 资源告警 / 诊断回复

不建议把这部分逻辑塞进：

- `MqttDriver`
- `ModbusRtu`
- `EventEngine`

原因：

- 资源监测是系统级能力，不属于采集协议逻辑
- 诊断命令也不属于 MQTT 业务本体
- 独立进程更方便限权、隔离、重启和后续扩展

### 配置建议

边端 app 配置建议新增：

```json
{
  "systemMonitor": {
    "enabled": true,
    "defaultIntervalMs": 5000,
    "minIntervalMs": 1000,
    "subscriptionTtlSec": 30,
    "cpuAlertThreshold": 90,
    "memAlertThreshold": 90,
    "diskAlertThreshold": 90,
    "alertRepeatIntervalSec": 60,
    "diagEnabled": true,
    "allowedCommands": [
      "uptime",
      "free_m",
      "df_root",
      "top_once",
      "ps_gateway",
      "journal_tail",
      "systemctl_status"
    ]
  }
}
```

### Java 平台接口建议

建议新增：

- 打开监测：
  - `POST /api/system-monitor/subscribe`
- 关闭监测：
  - `POST /api/system-monitor/unsubscribe`
- 当前资源：
  - `GET /api/system-monitor/current`
- 历史趋势：
  - `GET /api/system-monitor/history`
- 下发诊断：
  - `POST /api/system-monitor/diag/execute`
- 审计记录：
  - `GET /api/system-monitor/diag/audit`

### 实施顺序

建议按下面顺序做：

1. 边端 `SystemMonitor` 资源采集
2. 订阅租约机制
3. 平台实时监测页
4. 阈值告警主动上报
5. 白名单诊断命令
6. 平台审计和权限控制

### 当前建议结论

建议接受的方向：

- 页面打开才高频上报
- 超阈值主动告警
- 平台登录后才能触发远程诊断
- 远程诊断只做白名单命令

不建议接受的方向：

- 平台直接下发任意 Linux shell
- 无页面订阅时仍长期高频上报资源
- 未登录用户可执行边端诊断

## machineCode 一级主键落地约束

### 设计约束

当前系统正式收紧为：

- `machineCode` = 一级主键
- `meterCode` = 网关内二级主键
- `pointCode/index` = 点位级主键

约束要求：

1. 所有平台业务记录必须带 `machineCode`
2. 所有边端 MQTT 上下行消息必须带 `machineCode`
3. 平台侧查询入口默认必须带 `machineCode`
4. 边端收到请求时必须校验 `machineCode` 与本机实例匹配
5. 不再兼容 `gatewayCode/gateway/gw/gwCode` 等旧字段名
6. 不再使用 `GW0001` 作为默认兜底值，模板中的 `machineCode` 必须显式填写

### Java 平台侧落地

当前已收紧：

- 实时点位查询
  - `GET /api/realtime/points`
  - `machineCode` 必填
- 实时趋势查询
  - `GET /api/realtime/trend`
  - `machineCode` 必填
  - 系统监测查询
    - `GET /api/system-monitor/history`
    - `GET /api/system-monitor/alerts`
    - `GET /api/system-monitor/diag/audit`
    - `machineCode` 必填
  - OTA 任务查询
    - `GET /api/config/ota/tasks`
    - `machineCode` 必填
  - OTA 任务详情/时间线查询
    - `GET /api/config/ota/tasks/{jobId}`
    - `GET /api/config/ota/tasks/{jobId}/timeline`
    - `machineCode` 必填
- Java 前端交互
  - `实时监测`、`协议设备配置`、`MQTT / OTA / App` 三个大模块先按 `machineCode` 显示网关上下文卡片
  - 只有在当前 `machineCode` 已填写并展开后，才渲染该网关下的查询结果、编辑表单、OTA 任务和配置区
  - 前端修改 `machineCode` 时，会同步更新实时查询、系统监测、OTA 任务筛选和设备/批量工程上下文
  - OTA 发布、配置 OTA 打包、批量设备生成
    - 不再为 `machineCode` 自动补 `GW0001`
  - 实时 MQTT 入库
    - 只认 `machineCode`
    - 不再回退旧别名字段

平台侧消息入库也已收紧：

- 无 `machineCode` 的实时消息不入缓存
- 无 `machineCode` 的系统监测消息不入 MySQL
- 无 `machineCode` 的诊断回复不入审计

### C++ 边端落地

当前已收紧：

- MQTT 写命令请求
  - `machineCode` 必填
- OTA 请求
  - `machineCode` 必填
  - 不再用“本地唯一 machineCode 自动兜底”
- 系统监测订阅请求
  - `machineCode` 必填
  - 必须与当前边端实例 `machineCode` 相等
  - 诊断请求
    - `machineCode` 必填
    - 必须与当前边端实例 `machineCode` 相等
  - 内置示例配置
    - 不再预置 `GW0001`

### 消息规范

后续统一要求：

- telemetry
- snapshot
- alarm
- system-monitor telemetry
- system-monitor alert
- system-monitor reply
- diag reply
- command reply
- ota reply/status

全部都要带：

```json
{
  "machineCode": "GW0001"
}
```

### 查询与展示建议

平台页面后续默认交互建议：

1. 先输入 / 选择 `machineCode`
2. 再进入：
   - 实时监测
   - 系统监测
   - 诊断审计
   - OTA 任务

不再鼓励：

- 不带 `machineCode` 直接扫全量数据

### 当前落地状态

当前实现已经从“弱依赖 machineCode”推进到：

- 平台关键查询入口强依赖 `machineCode`
- 边端关键请求处理强校验 `machineCode`
- 监测 / 诊断 / OTA / 实时链路都围绕 `machineCode` 收口

---

## 压测记录：200 个 Modbus 设备 + 5 个 DLT645 标准表

### 测试时间与环境

- 测试时间：2026-04-23
- 边端设备：`192.168.22.12`
- 系统：Linux aarch64，内存约 2GB，无 swap
- 测试前已停止同类型在线驱动：
  - `modbus-rtu@*.service`
  - `dlt645-driver@*.service`
  - `mqtt-driver@*.service`
  - `gateway-services.service`
- 压测使用独立共享内存名，未复用生产共享内存：
  - `gateway_stress_*`

### 基准场景

用户指定基准规模：

- Modbus 设备：200 个
- 每个 Modbus 设备：40 个点
- Modbus 寄存器分布：两段连续地址
  - 第一段：`0-19`
  - 第二段：`100-119`
- Modbus 点位总数：`200 * 40 = 8000`
- DLT645 设备：5 个
- DLT645 标准点表：每表 358 点
- DLT645 点位总数：`5 * 358 = 1790`
- 基准总点位：`9790`

### 压测工具与命令

压测工具：

```bash
/tmp/gateway-stress-200x40-dlt5/stress_runner_fixed
```

基准命令：

```bash
cd /opt/modbus-gateway
/tmp/gateway-stress-200x40-dlt5/stress_runner_fixed \
  --device-config /tmp/gateway-stress-200x40-dlt5/device_stress_modbus_200x40.json \
  --device-config /tmp/gateway-stress-200x40-dlt5/device_stress_dlt645_5_standard.json \
  --app-config /tmp/gateway-stress-200x40-dlt5/mqtt-service-stress.json \
  --duration-sec 30 \
  --writer-threads 1 \
  --snapshot-interval-ms 1000 \
  --mqtt-scan-interval-ms 500 \
  --shm gateway_stress_sweep_9790
```

说明：

- `stress_runner` 不访问真实串口，不代表真实 Modbus RTU / DLT645 总线轮询耗时。
- 该测试主要评估边端内部链路：
  - 共享内存写入
  - 全量快照读取
  - MQTT Driver 扫描逻辑
  - 告警 / 变位 / 全量上传路径的 CPU 与内存压力
- MQTT 发布器为空实现，不连接 broker；因此结果不包含网络发送、broker ACK、离线 ring 文件刷盘成本。

### 测试中发现并修复的问题

压测初期出现 `stress_runner` 卡住不退出。定位后发现 `MemoryPointStore::getAllLatest()` 与 `putLatest()` 存在反向加锁风险：

- `putLatest()`：本地写锁 -> 共享内存锁
- 原 `getAllLatest()`：共享内存锁 -> 本地读锁

当采集写入和 MQTT 全量快照并发时，可能形成死锁。

修复方式：

- `getAllLatest()` 先复制本地 `bindings_` 快照。
- 释放本地读锁后，再进入共享内存锁读取 latest slots。
- 避免共享内存锁内再次申请本地读锁。

交叉编译验证：

```bash
cmake --build build-aarch64
```

结果：通过，`ModbusRtu / Dlt645Driver / MqttDriver / pointctl / stress_runner` 均重新链接成功。

### 阶梯压测结果

判定口径：

- `exitCode=0`：压测进程能完成。
- `exitCode=124`：被 `timeout` 杀掉，认为该规模在当前测试时间窗口内不可用。
- `mqttDriverAvgUs`：MQTT Driver 周期处理平均耗时。
- `batchAvgUs`：一轮写完整个点表的平均耗时。
- `processPeakRssKb`：进程峰值 RSS。
- CPU 约 `100%` 表示打满一个 CPU 核。

| 目标点位 | 结果 | 写入 ops/s | 单轮写入平均耗时 | 快照平均耗时 | MQTT 扫描平均耗时 | 峰值 RSS | 结论 |
|---:|---|---:|---:|---:|---:|---:|---|
| 9790 | 通过 | 9790 | 1.01s | 49.6ms | 78.1ms | 86.0MB | 基准规模可运行 |
| 12000 | 通过 | 6400 | 1.92s | 56.9ms | 91.1ms | 89.2MB | 可运行 |
| 15000 | 通过 | 3000 | 5.14s | 72.4ms | 112.7ms | 92.9MB | 可运行，但写入开始明显退化 |
| 18000 | 通过 | 1200 | 15.6s | 76.1ms | 117.5ms | 97.1MB | 接近 30s 周期可用边界 |
| 19000 | 通过 | 633 | 31.1s | 77.8ms | 120.0ms | 97.6MB | 已超过 30s 单轮写入周期 |
| 20000 | 通过 | 666 | 32.9s | 80.3ms | 126.2ms | 97.9MB | 可跑完，但刷新周期不可接受 |
| 25000 | 通过 | 833 | 40.3s | 85.1ms | 131.9ms | 99.3MB | 可跑完，但明显退化 |
| 27500 | 通过 | 916 | 44.2s | 89.7ms | 144.8ms | 119.9MB | 可跑完，但已不适合作为实时点表 |
| 30000 | 失败 | - | 未完成 | - | - | 122.9MB | 75s 保护超时，判定当前实现不可用 |
| 40000 | 失败 | - | 未完成 | - | - | 132.3MB | 90s 保护超时，判定当前实现不可用 |

### 4 写线程对比

9790 点，`writer-threads=4`：

```text
writeOps=269217 opsPerSec=8973 batchAvgUs=1100373 batchMaxUs=14301283
snapshots=29 snapshotAvgUs=53168 snapshotMaxUs=73125 snapshotPoints=216095
mqttDriverCycles=51 mqttDriverAvgUs=88967 mqttDriverMaxUs=181692
```

对比单写线程 9790 点：

```text
writeOps=293700 opsPerSec=9790 batchAvgUs=1011076 batchMaxUs=13912296
snapshots=29 snapshotAvgUs=49604 snapshotMaxUs=97718 snapshotPoints=215529
mqttDriverCycles=52 mqttDriverAvgUs=78055 mqttDriverMaxUs=166047
```

结论：

- 4 写线程没有提升吞吐，反而略低。
- 原因是当前共享内存 `putLatest()` 使用单段共享内存全局锁，多线程同时写同一共享内存会变成锁竞争。
- 当前实现更适合“多协议 / 多串口进程并发采集，但共享内存写入快速串行落点”的模型。

### 为什么 CPU 只打满单核

当前测试只打满单核是符合实现现状的：

- `stress_runner --writer-threads 1` 默认只有一个写线程。
- `MqttDriverService::runScanOnce()` 是单线程扫描。
- `MemoryPointStore` 对共享内存段使用全局互斥锁，写入和快照最终都会串行化访问同一段共享内存。
- 单串口 Modbus RTU / DLT645 物理链路本身也是串行总线，不能靠多线程并发读同一串口提升吞吐。

能使用多核的场景：

- 多个串口各自独立驱动进程采集。
- Modbus TCP 多连接并行采集。
- MQTT 上传、采集、SQLite 持久化分进程运行。
- Java 平台侧 WebSocket / MQTT 消费与边端采集分离部署。

当前无法充分使用多核的瓶颈：

- 单共享内存段全局锁。
- `getAllLatest()` 全量扫描固定 `100000` 个 latest slot。
- MQTT 全量扫描单线程。
- `allocateLatestSlot()` 随点位规模增大存在明显退化。

### 当前建议上限

按“能跑起来”定义：

- 当前实现可运行上限约 `25000-27500` 点。
- `30000` 点开始在测试窗口内不可接受。

按“实时数据可用”定义：

- 建议单网关实时点位控制在 `12000-15000` 点以内。
- 如果要求 30 秒内完成一轮完整写入和快照，建议不超过 `18000` 点。
- 如果要求 1 秒级实时全量刷新，当前基准 `9790` 点已经接近合理上限，再继续扩大需要优化。

按用户当前目标：

- `200 * 40 + 5 * 358 = 9790` 点可以运行。
- 但该规模已经会打满一个 CPU 核。
- 对 2GB 内存设备，RSS 约 `86MB`，内存不是主要瓶颈；CPU 和共享内存锁竞争才是主要瓶颈。

### 后续优化方向

优先级从高到低：

1. 优化 `allocateLatestSlot()`：增加 index 到 slot 的共享索引或本地缓存，避免点位规模上升后写入退化。
2. 优化 `getAllLatest()`：只扫描 occupied slot 或维护 active index 列表，避免每次扫描固定 100000 slots。
3. MQTT 全量上传分组扫描：按 `machineCode / meterCode` 或 index 范围分片读取，减少单次锁持有时间。
4. 多共享内存分片：按协议或串口拆分共享内存段，再由 MQTT 聚合，可提升多核利用率，但复杂度会上升。
5. Modbus TCP 并发采集：TCP 场景可按设备连接池并发；RTU/DLT645 同一串口不建议并发读。

### 当前测试结论

本次硬件上的最大建议量：

- 保守生产建议：`<= 12000` 点。
- 可接受上限：`15000-18000` 点。
- 可运行但明显退化：`25000-27500` 点。
- 当前实现不建议：`>= 30000` 点。

用户指定场景 `9790` 点：

- 可以运行。
- 内存占用可接受。
- CPU 会接近单核满载。
- 若还需要更多协议驱动、SQLite 持久化、真实 MQTT 网络发送和 Java 实时页面消费，建议保留 CPU 余量，不要继续在同一网关上显著增加点位。

---

## 多核优化方案草案

### 背景

当前压测显示，在 9790 点规模下，边端内部链路已经接近单核满载：

- 共享内存写入和快照读取存在全局锁。
- `MqttDriverService::runScanOnce()` 单线程扫描。
- `getAllLatest()` 当前全量扫描共享内存 latest slots。
- `allocateLatestSlot()` 在点位规模变大后写入退化明显。
- 单串口 RTU / DLT645 物理链路本身不能并发读同一条串口总线。

因此，多核优化不能简单地把同一个串口或同一个共享内存写入线程拆成多个线程，否则会变成锁竞争。正确方向是把“可并行的边界”拆出来，让多核分别处理不同串口、不同协议、不同数据分片和不同上传任务。

### 优化目标

目标不是单纯让 CPU 占满多核，而是让系统在更多点位下仍保持稳定周期。

建议目标：

- 9790 点：保留充足 CPU 余量，不再长期单核满载。
- 15000 点：全量快照和 MQTT 扫描保持在可接受范围。
- 20000 点：通过分片和并行化降低全量处理长尾，但不保证 1 秒全量实时刷新。
- 30000 点以上：需要结构性改造后再评估。

### 当前瓶颈拆解

#### 1. 采集侧

Modbus RTU / DLT645：

- 同一串口只能串行请求。
- 多个串口可以并行。
- 当前部署方式已经支持多个驱动进程：
  - `modbus-rtu@device_slave_ttySP1`
  - `modbus-rtu@device_slave_ttySP2`
  - `dlt645-driver@xxx`

Modbus TCP：

- 多个 TCP 设备天然可以并发。
- 当前如果仍按单进程串行采集，会浪费多核和网络并发能力。

#### 2. 共享内存侧

当前统一共享内存优点：

- MQTT 只读一个共享内存。
- pointctl 调试简单。
- 多协议统一数据模型。

当前统一共享内存瓶颈：

- `putLatest()` 对共享内存段加全局锁。
- `getAllLatest()` 读取全量快照也要加共享锁。
- 写入、快照、MQTT 扫描会互相等待。
- 增加写线程不能提升性能，因为锁是串行化的。

#### 3. MQTT 上传侧

当前 MQTT 扫描：

- 单线程。
- 全量快照一次读取所有点。
- 再做告警、变位、全量上传、分片。

瓶颈：

- 点位多时，单次扫描耗时增加。
- 即使 MQTT 报文已分片，数据准备阶段仍可能是单线程。

### 方案 A：保持单共享内存，优化内部索引和锁粒度

这是最稳妥的第一阶段方案，尽量不改变外部部署和配置模型。

#### A1. 优化 latest slot 写入索引

当前问题：

- 点位规模上升后，写入一轮耗时快速退化。
- 推测主要成本来自 latest slot 查找 / 分配路径。

改造：

- 在 `MemoryPointStore` 进程内维护 `index -> slot offset` 缓存。
- 第一次写入时仍走共享内存查找。
- 后续写入同一个 index 时直接定位 slot。
- 如果共享内存版本变化或 slot 失效，再回退查找。

收益：

- 不改变共享内存结构。
- 风险低。
- 对采集写入吞吐提升最直接。

限制：

- 每个进程有自己的本地缓存。
- 新进程启动时仍需重建缓存。

#### A2. 优化 `getAllLatest()` 扫描

当前问题：

- 全量快照会扫描固定 `kMaxLatestSlots=100000`。
- 即使只有 9790 个点，也会遍历较大的 slot 数组。

改造选项：

- 维护 occupied index 列表，只扫描已占用 slot。
- 或在共享内存 header 中维护 active slot count 和 compact slot 区。
- 或进程内维护 registered index 列表，MQTT 按注册点位读取。

推荐：

- 第一版使用“MQTT 按注册点位读取”。
- `MqttDriverService` 已经有 device configs，可以构造本进程关心的 index 列表。
- 全量上传时按 index 列表调用批量读取接口，而不是扫描整个 latest 区。

收益：

- 快照耗时从“扫描最大容量”变成“扫描实际配置点”。
- 对 10k-30k 点更线性。

#### A3. 增加批量读取接口

新增接口：

```cpp
std::vector<StoredPointValue> MemoryPointStore::getLatestByIndexes(
    const std::vector<std::uint32_t>& indexes,
    std::int64_t nowMs
) const;
```

实现要求：

- 一次进入共享内存锁。
- 批量读取多个 index。
- 本地 binding 快照在进入共享锁前准备好。

收益：

- 避免每个点 `getLatestByIndex()` 都加锁一次。
- MQTT 分片读取可以按批次处理。

风险：

- 需要确保 binding 快照和 shared slot 的 index 对应正确。

### 方案 B：MQTT 扫描分片并行

目标是让 MQTT 数据准备利用多核，但不破坏单共享内存模型。

#### B1. 按点位范围分片

流程：

1. 启动时根据 device configs 构建所有上报 index。
2. 按固定大小切片，例如每片 1000 或 2000 点。
3. 每个扫描周期只处理一个或多个片。
4. full snapshot 带现有 `chunkId / chunkIndex / chunkCount`。

模式：

- 低负载模式：每周期处理 1 片，降低 CPU 峰值。
- 高实时模式：线程池并行处理多片，再按分片发布。

收益：

- 避免单次全量扫描长时间占用 CPU。
- 和当前 MQTT 分片报文结构匹配。

风险：

- 如果平台要求严格“同一时刻完整快照”，分片之间会有时间差。
- 当前实时页面按片刷新，不需要强一致快照，因此可接受。

#### B2. MQTT Worker 线程池

新增线程池：

- `mqttScanWorkers = 2/4`
- 每个 worker 负责一组 index 分片的数据读取和 JSON 编码。
- 发布阶段仍可单线程串行发送，避免 MQTT client 线程安全问题。

建议：

- 第一阶段只并行“数据读取 + JSON 编码”。
- MQTT socket 发送仍保留单线程。

收益：

- 能利用多核做 JSON 编码和点位过滤。

风险：

- 需要处理分片结果排序和生命周期。
- 如果共享内存读取仍持有全局锁太久，多线程收益有限，所以应先做方案 A。

### 方案 C：多共享内存分片

这是更激进的方案，适合点位继续扩展到 3 万以上。

#### C1. 按串口 / 协议拆共享内存

示例：

```text
gateway_point_store_ttySP1
gateway_point_store_ttySP2
gateway_point_store_dlt645_1
gateway_point_store_tcp_1
```

每个协议驱动写自己的共享内存段。

MQTT 驱动配置多个共享内存：

```json
{
  "mqttDriver": {
    "sharedMemoryNames": [
      "gateway_point_store_ttySP1",
      "gateway_point_store_ttySP2",
      "gateway_point_store_dlt645_1"
    ]
  }
}
```

收益：

- 多个共享内存锁互不影响。
- 多串口采集能真正并行写入。
- MQTT 可多线程读取不同共享内存段。

代价：

- pointctl 要支持多个共享内存。
- Java 配置和 OTA 需要生成多个 shm 名称。
- MQTT 需要聚合多段共享内存。
- index 重复检测要跨多个共享内存做。

适用：

- 单网关点位超过 2 万。
- 多串口、多 TCP 设备较多。
- 对多核利用率要求高。

#### C2. 保留逻辑上的“一个数据池”

即使底层拆成多个共享内存，MQTT 对平台仍输出统一格式：

```json
{
  "machineCode": "GW0001",
  "meters": []
}
```

平台和业务不感知底层分片。

### 方案 D：按协议驱动内部并发

#### D1. Modbus RTU / DLT645

同一串口不做并发读。

可做：

- 一个串口一个进程。
- 一个进程内保持单串口采集线程。
- 多串口由 systemd 启多个实例并行。

不建议：

- 同一串口多个线程并发发请求。
- 多进程抢同一串口。

#### D2. Modbus TCP

TCP 可以做并发。

建议：

- 按 TCP 设备建立连接池。
- 每个连接一个采集 worker。
- 限制最大并发，例如：
  - `maxTcpConcurrentDevices = 16`
  - `maxTcpRequestsPerDevice = 1`

收益：

- 能明显利用多核和网络并发。

风险：

- 部分设备不支持高频并发。
- 需要超时隔离，避免单设备拖慢全局。

### 推荐实施路线

#### 第一阶段：低风险优化

目标：不改配置大结构，先解决当前性能退化。

内容：

1. `MemoryPointStore` 增加本地 `index -> slot` 缓存。
2. 增加 `getLatestByIndexes()` 批量读取接口。
3. `MqttDriverService` 全量扫描改为按配置 index 列表批量读取。
4. MQTT 分片上传保持当前格式。
5. `stress_runner` 增加更明确的多点位阶梯压测参数和输出。

预期收益：

- 9790 点 CPU 下降。
- 15000-18000 点可用性提升。
- 多线程前先减少锁竞争。

风险：

- 需要仔细处理共享内存 slot 缓存失效。

#### 第二阶段：MQTT 分片并行

目标：利用多核处理全量实时数据准备。

内容：

1. `mqttDriver.scanWorkers` 配置项。
2. 按 index 切片。
3. worker 并行读取和编码 JSON。
4. 发布线程按 chunk 发送。

预期收益：

- JSON 编码和数据过滤可多核并行。
- 大点表上传峰值耗时下降。

风险：

- MQTT 消息顺序可能变化，平台必须按 `chunkId/chunkIndex` 处理。

#### 第三阶段：多共享内存分片

目标：突破单共享内存锁瓶颈。

内容：

1. 支持 `sharedMemoryNames[]`。
2. pointctl 支持多 shm 查询。
3. MQTT 聚合多 shm。
4. Java 配置生成按串口自动分配 shm。
5. OTA 配置包同步支持。

预期收益：

- 多串口、多协议真正利用多核。
- 单段共享内存锁竞争下降。

风险：

- 改动范围大。
- 调试复杂度上升。
- 需要重新定义跨 shm 的 index 唯一校验。

### 推荐优先做法

建议先做第一阶段，不建议直接上多共享内存。

原因：

- 当前最大退化点在共享内存写入和全量扫描路径。
- 第一阶段不改变部署模型，风险最低。
- 做完后再压测，如果 2 万点以内已经满足，就没必要引入多共享内存复杂度。
- 如果第一阶段后仍无法满足 3 万点，再进入第三阶段。

### 配置建议草案

后续可增加：

```json
{
  "mqttDriver": {
    "scanWorkers": 1,
    "snapshotChunkPointLimit": 1000,
    "snapshotReadBatchSize": 2000,
    "parallelEncode": false
  },
  "memoryStore": {
    "slotIndexCache": true,
    "batchReadEnabled": true
  }
}
```

字段说明：

- `scanWorkers`：MQTT 扫描 worker 数，第一阶段默认仍为 `1`。
- `snapshotChunkPointLimit`：每个 MQTT 实时分片最多点数。
- `snapshotReadBatchSize`：共享内存批量读取每批点数。
- `parallelEncode`：是否并行 JSON 编码。
- `slotIndexCache`：是否启用本地 index 到 slot 缓存。
- `batchReadEnabled`：是否启用批量读取接口。

### 验证计划

每阶段完成后都跑同一套阶梯压测：

```text
9790 -> 12000 -> 15000 -> 18000 -> 20000 -> 25000 -> 30000
```

核心指标：

- `opsPerSec`
- `batchAvgUs`
- `snapshotAvgUs`
- `mqttDriverAvgUs`
- `processPeakRssKb`
- 单核 CPU 占用
- 系统 CPU 总占用

验收目标：

- 9790 点：CPU 不再长期单核满载。
- 15000 点：MQTT 扫描平均小于 150ms。
- 18000 点：单轮写入小于 30s。
- 25000 点：可以跑完，且不出现超时。
- 30000 点：作为挑战目标，不作为第一阶段验收硬指标。

### 结论

推荐实施顺序：

1. 先做 `MemoryPointStore` slot 缓存和批量读取。
2. 再做 `MqttDriverService` 按 index 分片扫描。
3. 然后评估是否需要 MQTT worker 并行编码。
4. 最后才考虑多共享内存分片。

当前不建议一开始就拆多共享内存，因为它会影响配置、pointctl、MQTT 聚合、OTA 和 Java 生成逻辑。先把单共享内存内部路径优化到位，风险更低，收益也最直接。

---

## 面向实际硬件接口的网关重设计方案

### 硬件接口现状

目标边端硬件具备：

- RS485：8 路
- RS232：2 路
- DI：18 路
- DO：8 路
- CAN：4 路
- 网口：4 路

当前已使用：

- RS485：用于 Modbus RTU / DLT645-2007 等串行协议
- 网口：用于 Modbus TCP、MQTT、OTA、平台通信

后续需要预留：

- CAN 驱动
- DI 采集
- DO 控制
- RS232 协议驱动
- 多网口网络隔离和多业务链路

因此后续架构不能继续只围绕 Modbus RTU 优化，而应升级为“多接口、多协议、多驱动、统一点位总线”的边端网关架构。

### 重新设计目标

1. 每类物理接口都能独立接入驱动。
2. 多路 RS485 / RS232 / CAN 可以并行运行，充分利用多核。
3. DI / DO 作为独立 I/O 驱动，不强行挂到 Modbus 模型里。
4. MQTT 仍统一从边端数据总线读取，对平台输出统一格式。
5. Java 配置平台能按接口类型生成配置和 OTA 包。
6. 未来新增协议时，不影响既有 Modbus / DLT645 / MQTT 逻辑。

### 核心架构调整

原始模型：

```text
ModbusRtu / Dlt645Driver -> gateway_point_store -> MqttDriver -> MQTT
```

建议升级为：

```text
RS485 Driver Instances
RS232 Driver Instances
CAN Driver Instances
DI Driver
DO Driver
Ethernet/TCP Driver Instances
        |
        v
Interface Sharded Point Stores
        |
        v
MqttDriver Aggregator
        |
        v
MQTT / OTA / Java Platform
```

其中：

- 每个物理接口可以拥有自己的驱动进程。
- 每个接口或接口组可以拥有自己的共享内存分片。
- MQTT Driver 负责聚合所有共享内存分片。
- 平台侧仍看到统一的 `machineCode / meterCode / pointCode / index / value`。

### 物理接口建模

新增统一接口配置模型：

```json
{
  "interfaces": [
    {
      "interfaceCode": "RS485_1",
      "type": "rs485",
      "device": "/dev/ttySP1",
      "enabled": true,
      "sharedMemoryName": "gateway_point_store_rs485_1",
      "driverService": "serial-driver@rs485_1",
      "protocols": ["modbus_rtu", "dlt645_2007"]
    },
    {
      "interfaceCode": "ETH_1",
      "type": "ethernet",
      "device": "eth0",
      "enabled": true,
      "sharedMemoryName": "gateway_point_store_eth_1",
      "driverService": "tcp-driver@eth_1",
      "protocols": ["modbus_tcp", "mqtt"]
    },
    {
      "interfaceCode": "CAN_1",
      "type": "can",
      "device": "can0",
      "enabled": false,
      "sharedMemoryName": "gateway_point_store_can_1",
      "driverService": "can-driver@can_1",
      "protocols": ["can_raw", "canopen", "j1939"]
    },
    {
      "interfaceCode": "DIO",
      "type": "dio",
      "enabled": false,
      "sharedMemoryName": "gateway_point_store_dio",
      "driverService": "dio-driver"
    }
  ]
}
```

说明：

- `interfaceCode` 是物理接口唯一编号。
- `type` 表示接口类型，而不是协议类型。
- `device` 是 Linux 设备名或网卡名。
- `sharedMemoryName` 允许按接口分片共享内存。
- `protocols` 表示该接口允许挂载的协议。

### 共享内存设计调整

#### 当前设计

当前只有一个主共享内存：

```text
gateway_point_store
```

优点：

- 简单。
- MQTT 聚合成本低。
- pointctl 使用方便。

缺点：

- 多接口并发时共享内存锁竞争严重。
- 单共享内存全量扫描会拖慢 MQTT。
- CPU 多核利用率不高。

#### 新设计：接口级共享内存分片

建议按物理接口分片：

```text
gateway_point_store_rs485_1
gateway_point_store_rs485_2
...
gateway_point_store_rs485_8
gateway_point_store_rs232_1
gateway_point_store_rs232_2
gateway_point_store_can_1
...
gateway_point_store_can_4
gateway_point_store_dio
gateway_point_store_eth_1
...
gateway_point_store_eth_4
```

MQTT 配置改为支持多个共享内存：

```json
{
  "mqttDriver": {
    "sharedMemoryNames": [
      "gateway_point_store_rs485_1",
      "gateway_point_store_rs485_2",
      "gateway_point_store_eth_1",
      "gateway_point_store_dio"
    ],
    "scanWorkers": 4,
    "snapshotReadBatchSize": 2000
  }
}
```

收益：

- 每个接口独立锁，8 路 485 可以并行写入。
- MQTT 可多线程读取不同 shm。
- 某个接口异常不会阻塞全部接口。
- 后续 CAN / DI / DO 接入更自然。

代价：

- pointctl 要支持多 shm。
- MQTT 要聚合多 shm。
- index 唯一性要跨 shm 校验。
- Java 配置生成要知道接口和 shm 的对应关系。

### 点位 index 规划

由于多接口、多协议、多共享内存后仍要平台统一管理，`index` 必须全网关唯一。

建议规划：

| 接口类型 | 建议 index 段 |
|---|---:|
| RS485_1 | 110000-119999 |
| RS485_2 | 120000-129999 |
| RS485_3 | 130000-139999 |
| RS485_4 | 140000-149999 |
| RS485_5 | 150000-159999 |
| RS485_6 | 160000-169999 |
| RS485_7 | 170000-179999 |
| RS485_8 | 180000-189999 |
| RS232_1 | 210000-219999 |
| RS232_2 | 220000-229999 |
| CAN_1 | 310000-319999 |
| CAN_2 | 320000-329999 |
| CAN_3 | 330000-339999 |
| CAN_4 | 340000-349999 |
| DI | 410000-419999 |
| DO | 420000-429999 |
| ETH_1 | 510000-519999 |
| ETH_2 | 520000-529999 |
| ETH_3 | 530000-539999 |
| ETH_4 | 540000-549999 |

说明：

- 这个规划不是协议强制要求，而是便于人和平台排查。
- Java 生成配置时可按接口自动分配 index。
- OTA 发布前应做全量 index 冲突校验。

### 驱动进程规划

#### RS485 / RS232 串口类

建议统一为 `SerialProtocolDriver`，根据配置决定协议：

```text
serial-driver@rs485_1.service
serial-driver@rs485_2.service
...
serial-driver@rs232_1.service
```

每个串口一个进程。

每个进程内部：

- 只打开一个串口。
- 串口请求串行执行。
- 支持该串口下多个设备。
- 支持协议：
  - `modbus_rtu`
  - `dlt645_2007`
  - 后续 RS232 私有协议

原因：

- 同一串口并发没有意义。
- 多串口进程并行可以利用多核。
- 串口异常隔离更好。

#### Ethernet / TCP 类

建议为 `TcpProtocolDriver`：

```text
tcp-driver@eth_1.service
tcp-driver@eth_2.service
```

每个网口或每组 TCP 设备一个进程。

进程内部：

- 支持 Modbus TCP。
- 后续可支持 HTTP polling、TCP 私有协议。
- 支持连接池和并发采集。

建议配置：

```json
{
  "tcpDriver": {
    "maxConcurrentDevices": 16,
    "connectTimeoutMs": 1000,
    "requestTimeoutMs": 1000
  }
}
```

#### CAN 类

建议独立为 `CanDriver`：

```text
can-driver@can_1.service
can-driver@can_2.service
```

初期支持：

- CAN raw 帧收发。
- 把 CAN ID + signal 映射成点位。

后续可扩展：

- CANopen
- J1939
- 私有 CAN 协议

点位示例：

```json
{
  "index": 310001,
  "pointCode": "can_18ff50e5_speed",
  "name": "CAN Speed",
  "category": "telemetry",
  "can": {
    "channel": "CAN_1",
    "frameId": "18FF50E5",
    "byteOffset": 0,
    "bitOffset": 0,
    "bitLength": 16,
    "endian": "little",
    "scale": 0.1,
    "offset": 0
  }
}
```

#### DI / DO 类

建议独立为 `DioDriver`：

```text
dio-driver.service
```

DI：

- 18 路输入。
- 周期采样或中断事件。
- 每路 DI 映射为一个点。
- 支持变位上传。

DO：

- 8 路输出。
- 控制命令通过 MQTT -> pendingWrites -> DioDriver 执行。
- 支持状态回读。

DI 点位示例：

```json
{
  "index": 410001,
  "pointCode": "di_1",
  "name": "DI 1",
  "category": "status",
  "read": {
    "dataType": "digital_input",
    "channel": 1
  },
  "reportOnChange": true
}
```

DO 点位示例：

```json
{
  "index": 420001,
  "pointCode": "do_1",
  "name": "DO 1",
  "category": "control",
  "read": {
    "dataType": "digital_output",
    "channel": 1
  },
  "write": {
    "enable": true,
    "dataType": "digital_output",
    "allowedValues": [0, 1],
    "verifyAfterWrite": true
  }
}
```

### MQTT 聚合设计

MQTT Driver 从“单共享内存读取器”升级为“多共享内存聚合器”。

配置：

```json
{
  "mqttDriver": {
    "sharedMemoryNames": [
      "gateway_point_store_rs485_1",
      "gateway_point_store_rs485_2",
      "gateway_point_store_dio",
      "gateway_point_store_eth_1"
    ],
    "scanWorkers": 4,
    "publishAllOnFull": true,
    "snapshotReadBatchSize": 2000,
    "snapshotChunkPointLimit": 1000
  }
}
```

扫描流程：

1. MQTT Driver 加载所有设备配置。
2. 构建全局 index 映射。
3. 按共享内存分组。
4. 每个 worker 读取一个或多个共享内存。
5. 聚合为统一 telemetry payload。
6. 按 `maxPayloadBytes` 分片发布。

平台侧 payload 不变：

```json
{
  "type": "snapshot",
  "machineCode": "GW0001",
  "chunked": true,
  "chunkId": "xxx",
  "chunkIndex": 1,
  "chunkCount": 3,
  "meters": []
}
```

### Java 配置平台调整

当前 Java 平台主要按协议配置。

后续应调整为按“物理接口 -> 协议设备 -> 点位”配置。

页面建议：

```text
网关硬件接口
  RS485
    RS485_1
      协议：modbus_rtu / dlt645_2007
      设备列表
      点表
    RS485_2
  RS232
  Ethernet
  CAN
  DI/DO
MQTT / OTA
实时监测
```

批量生成逻辑：

- 先选择接口。
- 再选择协议。
- 再生成设备和点位。
- 自动分配：
  - `interfaceCode`
  - `sharedMemoryName`
  - `index` 段
  - `systemd service name`

配置生成结果：

```text
config/runtime/interfaces/interfaces.json
config/runtime/devices/rs485_1_modbus.json
config/runtime/devices/rs485_2_dlt645.json
config/runtime/devices/dio.json
config/runtime/devices/can_1.json
config/runtime/apps/mqtt-service.json
```

### OTA 调整

OTA 配置包需要包含：

- `interfaces.json`
- 所有 interface 关联的 device config
- `mqtt-service.json`
- systemd 模板
- `gateway-services.sh`

OTA apply 后：

1. 停止旧服务。
2. 读取 `interfaces.json`。
3. 只启动 enabled 的接口驱动。
4. 启动 MQTT 聚合驱动。
5. 发布 OTA 状态。

### systemd 启动策略

不建议开机固定启动所有驱动。

继续使用：

```text
gateway-services.service
```

由它扫描配置后启动：

```text
serial-driver@rs485_1.service
serial-driver@rs485_2.service
tcp-driver@eth_1.service
can-driver@can_1.service
dio-driver.service
mqtt-driver@mqtt-service.service
```

规则：

- 配置不存在：不启动。
- `enabled=false`：不启动。
- 接口驱动异常：只影响该接口。
- MQTT 聚合驱动可继续上传其他接口数据。

### 多核利用方式

实际可并行单元：

- RS485_1 到 RS485_8：最多 8 个串口进程并行。
- RS232_1 到 RS232_2：最多 2 个串口进程并行。
- CAN_1 到 CAN_4：最多 4 个 CAN 进程并行。
- Ethernet TCP：可进程内多连接并行。
- DI/DO：独立低开销驱动。
- MQTT：多共享内存读取 + JSON 编码可并行。

不能并行的单元：

- 同一 RS485 口内部请求。
- 同一 DLT645 总线内部请求。
- 同一个 DO 通道的写操作。

### 推荐实施顺序

#### 阶段 1：接口模型落地

只做配置和服务编排，不大改协议逻辑。

内容：

1. 新增 `interfaces.json`。
2. Java 页面增加硬件接口管理。
3. `gateway-services.sh` 改为按接口配置启动服务。
4. 设备配置增加 `interfaceCode`。
5. index 按接口自动规划。

收益：

- 先把 8 路 485、4 网口、CAN、DI/DO 的管理模型定下来。

#### 阶段 2：共享内存按接口分片

内容：

1. 每个接口生成独立 `sharedMemoryName`。
2. 协议驱动写自己的 shm。
3. pointctl 支持 `--all` 或读取 `interfaces.json`。
4. MQTT 支持 `sharedMemoryNames[]`。

收益：

- 多接口写入不再抢同一把共享内存锁。
- 多核利用率明显提升。

#### 阶段 3：MQTT 聚合并行

内容：

1. MQTT 按 shm 分 worker。
2. 每个 worker 批量读取。
3. 统一汇总分片上传。

收益：

- 多共享内存场景下 MQTT 扫描可并行。

#### 阶段 4：接入 DI/DO

内容：

1. `DioDriver`
2. DI 变位事件。
3. DO MQTT 下发控制。
4. Java 页面配置 DI/DO。

收益：

- 把 18 DI / 8 DO 纳入统一点位模型。

#### 阶段 5：接入 CAN

内容：

1. `CanDriver`
2. CAN raw 信号映射。
3. 后续扩展 CANopen / J1939。
4. Java 页面配置 CAN 点位。

收益：

- 4 路 CAN 纳入统一网关。

### 推荐当前决策

结合实际硬件，建议采用：

```text
按物理接口分进程 + 按接口分共享内存 + MQTT 多 shm 聚合
```

不建议继续让所有协议都写同一个 `gateway_point_store`。

原因：

- 当前硬件接口数量多，天然适合按接口并行。
- 单共享内存会把 8 路 485、4 路 CAN、4 网口的并发能力串行化。
- 后续 DI/DO/CAN 接入后，单共享内存模型会越来越难扩展。
- 按接口分片后，故障隔离、服务重启、压测定位都更清晰。

### 需要用户确认的问题

开始实现前需要确认：

1. 8 路 RS485 在 Linux 下的实际设备名，例如 `/dev/ttySP1` 到 `/dev/ttySP8` 是否准确。
2. 2 路 RS232 的设备名。
3. 4 路 CAN 的设备名，例如 `can0-can3` 是否已经由系统驱动创建。
4. DI/DO 的访问方式：
   - sysfs gpio
   - `/dev/gpiochip`
   - 厂商 SDK
   - 其他字符设备
5. 4 个网口是否需要区分业务：
   - 平台/MQTT
   - Modbus TCP 设备网
   - 本地维护网
   - 预留

如果这些设备名和访问方式确认，就可以开始阶段 1：接口模型和 Java 配置页面改造。

### 统一取数和写入方法

按接口拆共享内存后，上层业务不能直接操作某一个 shm，否则 Java、MQTT、pointctl、命令下发都会被迫理解接口分片细节。

因此需要新增一个统一数据访问层：

```text
PointStoreRouter
```

职责：

- 加载 `interfaces.json` 和所有 device config。
- 建立全局 `index -> sharedMemoryName -> interfaceCode -> driverService` 路由表。
- 对上提供统一读接口。
- 对上提供统一写接口。
- 对下把请求路由到对应接口共享内存。

#### 统一路由表

启动时构建：

```text
index -> PointRoute
```

结构：

```cpp
struct PointRoute {
    std::uint32_t index;
    std::string machineCode;
    std::string meterCode;
    std::string pointCode;
    std::string interfaceCode;
    std::string interfaceType;
    std::string sharedMemoryName;
    std::string driverService;
    bool writable;
};
```

示例：

```json
{
  "index": 110001,
  "machineCode": "GW0001",
  "meterCode": "RS485_1_SLAVE001",
  "pointCode": "reg_1",
  "interfaceCode": "RS485_1",
  "interfaceType": "rs485",
  "sharedMemoryName": "gateway_point_store_rs485_1",
  "driverService": "serial-driver@rs485_1",
  "writable": true
}
```

路由表来源：

- `interfaces.json`
- `config/runtime/devices/*.json`
- 每个点位的 `index / machineCode / meterCode / pointCode / write.enable`
- 每个设备配置里的 `interfaceCode / memoryStore.sharedMemoryName`

校验规则：

- `index` 必须全网关唯一。
- `machineCode + meterCode + pointCode` 必须全网关唯一。
- 每个点必须能找到接口。
- 每个接口必须能找到 `sharedMemoryName`。
- 写入点必须有 `write.enable=true`。

#### 统一取数接口

C++ 内部建议接口：

```cpp
class PointStoreRouter {
public:
    Optional<StoredPointValue> getLatestByIndex(
        std::uint32_t index,
        std::int64_t nowMs
    ) const;

    std::vector<StoredPointValue> getLatestByIndexes(
        const std::vector<std::uint32_t>& indexes,
        std::int64_t nowMs
    ) const;

    std::vector<StoredPointValue> getAllLatest(
        std::int64_t nowMs
    ) const;

    std::vector<StoredPointValue> getLatestByInterface(
        const std::string& interfaceCode,
        std::int64_t nowMs
    ) const;

    std::vector<StoredPointValue> getLatestByMeter(
        const std::string& machineCode,
        const std::string& meterCode,
        std::int64_t nowMs
    ) const;
};
```

取数流程：

1. 上层传 `index` 或筛选条件。
2. `PointStoreRouter` 根据路由表找到目标共享内存。
3. 按共享内存分组。
4. 对每个共享内存调用批量读取。
5. 合并结果并按 index 排序。

示例：

```text
MQTT full snapshot
  -> PointStoreRouter.getAllLatest()
  -> group by sharedMemoryName
  -> gateway_point_store_rs485_1.getLatestByIndexes()
  -> gateway_point_store_rs485_2.getLatestByIndexes()
  -> gateway_point_store_dio.getLatestByIndexes()
  -> merge
  -> chunk publish
```

#### 统一写入接口

C++ 内部建议接口：

```cpp
class PointStoreRouter {
public:
    CommandSubmitResult submitWriteCommand(
        const PendingWriteCommand& command
    );

    std::vector<PendingWriteCommand> peekPendingWrites(
        std::size_t limit
    ) const;
};
```

写入流程：

1. MQTT / pointctl / Java 下发命令传入 `index`。
2. `PointStoreRouter` 查询 `index` 对应的 `PointRoute`。
3. 校验：
   - index 是否存在
   - 是否允许写
   - value 是否在 allowedValues / min / max 范围内
   - 目标接口是否 enabled
4. 将 `PendingWriteCommand` 写入该接口对应的共享内存 pendingWrites。
5. 对应接口驱动只消费自己共享内存里的 pendingWrites。
6. 驱动执行真实设备写入。
7. 写入结果仍写回该接口共享内存，并由 MQTT 聚合上报。

示例：

```text
MQTT command index=420001
  -> PointStoreRouter
  -> route: gateway_point_store_dio
  -> submit pending write to DIO shm
  -> DioDriver drain pending write
  -> set DO_1
  -> write command result
  -> MqttDriver aggregate result
```

#### 对 MQTT Driver 的统一方式

MQTT Driver 不再直接持有单个 `MemoryPointStore`。

改为：

```cpp
PointStoreRouter router;
MqttDriverService service(router, ...);
```

MQTT 取数：

- 全量上传：`router.getAllLatest(nowMs)`
- 按点拉取：`router.getLatestByIndexes(indexes, nowMs)`
- 告警扫描：从 router 获取当前点值
- 变位扫描：从 router 获取当前点值

MQTT 写入：

- 命令下发：`router.submitWriteCommand(command)`

好处：

- MQTT 不需要知道点在哪个共享内存。
- 后续新增 CAN / DI / DO，不需要改 MQTT 消息格式。

#### 对 pointctl 的统一方式

pointctl 增加两种模式。

单 shm 模式保留：

```bash
pointctl snapshot --shm gateway_point_store_rs485_1
```

统一模式新增：

```bash
pointctl snapshot --interfaces /opt/modbus-gateway/config/runtime/interfaces/interfaces.json
pointctl get --index 110001 --interfaces /opt/modbus-gateway/config/runtime/interfaces/interfaces.json
pointctl write --index 420001 --value 1 --interfaces /opt/modbus-gateway/config/runtime/interfaces/interfaces.json
```

统一模式内部也是调用 `PointStoreRouter`。

输出中增加接口信息：

```json
{
  "index": 420001,
  "interfaceCode": "DIO",
  "sharedMemoryName": "gateway_point_store_dio",
  "value": 1,
  "quality": 1,
  "ts": 1776900000000,
  "stale": false
}
```

#### 对 Java 平台的统一方式

Java 平台仍只需要按统一点位模型管理：

- `machineCode`
- `meterCode`
- `pointCode`
- `index`
- `interfaceCode`
- `protocol`

Java 不直接读共享内存。

Java 生成配置时负责：

- 生成 `interfaces.json`
- 给每个接口分配 `sharedMemoryName`
- 给每个点分配全局唯一 `index`
- 生成 MQTT 的 `sharedMemoryNames[]`

Java 实时数据消费仍按 MQTT payload：

```json
{
  "machineCode": "GW0001",
  "meters": []
}
```

不感知底层有多少共享内存。

#### 对接口驱动的统一方式

每个接口驱动只关心自己的共享内存：

```text
serial-driver@rs485_1
  -> gateway_point_store_rs485_1

dio-driver
  -> gateway_point_store_dio

can-driver@can_1
  -> gateway_point_store_can_1
```

接口驱动职责：

- 注册自己配置里的点位。
- 写最新值到自己的 shm。
- 从自己的 shm 读取 pendingWrites。
- 执行设备写入。
- 写入 command result。

接口驱动不需要感知其他共享内存。

#### 统一写命令回执

写命令分两级回执：

1. 路由受理回执
2. 设备执行结果回执

路由受理回执：

```json
{
  "cmdId": "CMD001",
  "index": 420001,
  "accepted": true,
  "stage": "routed",
  "interfaceCode": "DIO",
  "sharedMemoryName": "gateway_point_store_dio",
  "message": "write command routed"
}
```

设备执行结果：

```json
{
  "cmdId": "CMD001",
  "index": 420001,
  "success": true,
  "stage": "executed",
  "interfaceCode": "DIO",
  "message": "write completed"
}
```

如果 index 不存在：

```json
{
  "cmdId": "CMD001",
  "index": 999999,
  "accepted": false,
  "stage": "rejected",
  "message": "command index not found"
}
```

#### 统一 API 语义

最终对上层暴露的语义应该是：

```text
读：按 index / meter / interface / all 取点值
写：按 index 写命令
订阅：按 MQTT topic 收统一 payload
调试：pointctl 统一查询所有接口
```

不暴露的细节：

```text
某个点具体在哪个 shm
某个 shm 如何加锁
某个接口驱动如何消费 pendingWrites
```

这些由 `PointStoreRouter` 统一处理。

#### 实施建议

第一步先实现 `PointStoreRouter`，即使暂时只有一个共享内存也能使用。

阶段顺序：

1. 增加 `PointStoreRouter`，兼容当前单 shm。
2. MQTT Driver 改为通过 router 取数和写命令。
3. pointctl 增加 router 模式。
4. Java 生成 `interfaces.json` 和 `sharedMemoryNames[]`。
5. 协议驱动按接口写不同 shm。
6. MQTT 聚合多个 shm。

这样改造风险最低：

- 第 1 阶段不破坏现有单 shm。
- 第 2 阶段 MQTT 逻辑先统一。
- 第 3 阶段再逐步启用多 shm。

#### 关键约束

- `index` 必须全网关唯一。
- 写命令必须按 `index` 路由，不允许按 shm 直接写。
- MQTT payload 不允许暴露多个 shm 的复杂度。
- pointctl 可以显示 shm 信息，但业务不应依赖 shm 名称。
- OTA apply 后必须重建路由表并重启相关驱动。

#### 当前实现状态

已落地内容：

- C++ 已增加 `PointStoreRouter`，负责维护 `index -> sharedMemoryName / machineCode / meterCode / pointCode / interface` 路由。
- `MqttDriverConfig` 已支持 `sharedMemoryNames[]`，保留 `sharedMemoryName` 作为主共享内存兼容字段。
- MQTT Driver 启动时会加载 `sharedMemoryNames[]`，并自动合并设备配置中出现的 `memoryStore.sharedMemoryName`。
- MQTT 全量上传、按需上传、变位上传、告警取值、命令下发已改为通过 `PointStoreRouter` 读写。
- `MemoryPointStore` 增加 `getLatestByIndexes()`，Router 可以按共享内存分组批量读取，避免每个 index 单独加锁。
- `pointctl` 增加 `--app-config <path>` 统一模式，支持跨共享内存 `write`、`get`、`dump/snapshot`、`pending-peek`。
- Java 端默认模板和 MQTT 配置页面已增加 `mqttDriver.sharedMemoryNames[]`。
- Java 多串口批量生成会按串口自动写入设备配置中的 `memoryStore.sharedMemoryName`，规则为 `gateway_point_store_<串口后缀>`，例如 `/dev/ttySP1 -> gateway_point_store_ttySP1`。
- Java 多串口批量生成和 ZIP 下载会同步刷新 app 配置中的 `deviceConfigFiles[]` 与 `mqttDriver.sharedMemoryNames[]`，保证 OTA 下发后 MQTT Driver 能读取所有接口分片。

当前兼容约束：

- 当前 `interfaceCode` 暂时使用 `sharedMemoryName` 表示，后续接入独立 `interfaces.json` 后再改为真实接口编码，例如 `RS485_1`、`ETH_1`、`CAN_1`。
- 当前协议驱动仍按设备配置中的 `memoryStore.sharedMemoryName` 打开共享内存；Java 批量生成已按串口自动分配，手写 JSON 时仍需要保持一致。
- 共享内存单段容量仍由编译期布局控制，当前 `latest` 上限为 100000 点。多接口扩展优先通过多 shm 分片降低锁竞争，而不是运行中重排单个 shm 内存布局。
- 如果多个设备配置重复使用同一个 `index`，Router 启动阶段会直接拒绝并报出重复路由，避免 MQTT 或写命令路由到错误设备。

#### 8 路 RS485 分片压测记录

测试时间：2026-04-23。

测试设备：`192.168.22.12`。

测试约束：

- 设备均匀分配到 8 路 485。
- 同一串口只能存在一种协议。
- 在同时包含 Modbus 与 DLT645 的条件下，采用 7 路 Modbus + 1 路 DLT645：
- `RS485_1` - `RS485_7`：Modbus RTU。
- `RS485_8`：DLT645-2007。

测试规模：

- Modbus：200 台设备，每台 40 点，两段连续寄存器。
- DLT645：5 台表，每台使用 DLT645-2007 标准点表 358 点。
- 总点数：`200 * 40 + 5 * 358 = 9790`。
- 共享内存分片：8 个。

共享内存分配：

```text
RS485_1 -> gateway_stress_rs485_1
RS485_2 -> gateway_stress_rs485_2
RS485_3 -> gateway_stress_rs485_3
RS485_4 -> gateway_stress_rs485_4
RS485_5 -> gateway_stress_rs485_5
RS485_6 -> gateway_stress_rs485_6
RS485_7 -> gateway_stress_rs485_7
RS485_8 -> gateway_stress_rs485_8
```

压测文件：

```text
/tmp/gateway-stress-8rs485/device_stress_rs485_1_modbus.json
/tmp/gateway-stress-8rs485/device_stress_rs485_2_modbus.json
/tmp/gateway-stress-8rs485/device_stress_rs485_3_modbus.json
/tmp/gateway-stress-8rs485/device_stress_rs485_4_modbus.json
/tmp/gateway-stress-8rs485/device_stress_rs485_5_modbus.json
/tmp/gateway-stress-8rs485/device_stress_rs485_6_modbus.json
/tmp/gateway-stress-8rs485/device_stress_rs485_7_modbus.json
/tmp/gateway-stress-8rs485/device_stress_rs485_8_dlt645.json
/tmp/gateway-stress-8rs485/mqtt-service-stress-8rs485.json
```

压测工具已改为多共享内存 Router 模式：

- 每个点按设备配置中的 `memoryStore.sharedMemoryName` 写入目标 shm。
- 快照读取走 `PointStoreRouter.getAllLatest()`。
- MQTT 扫描走 `MqttDriverService + PointStoreRouter`。
- `stress_runner` 输出 `shmCount`，用于确认实际分片数量。

单写线程对照结果：

```text
stress started points=9790 writers=1 durationSec=30 shmCount=8 mqttDriverCycle=on
stress result
  writeOps=9790 opsPerSec=326 batchAvgUs=35142808 batchMaxUs=35142808
  snapshots=2 snapshotAvgUs=15416512 snapshotMaxUs=22344525 snapshotPoints=11618
  mqttDriverCycles=1 mqttDriverAvgUs=29608683 mqttDriverMaxUs=29608683
```

多写线程分片结果：

```text
writers=2, mqttDriverCycle=on
  writeOps=8517300 opsPerSec=283910 batchAvgUs=34505 batchMaxUs=70867
  snapshots=28 snapshotAvgUs=89551 snapshotMaxUs=102806 snapshotPoints=274120
  mqttDriverCycles=26 mqttDriverAvgUs=188517 mqttDriverMaxUs=205497

writers=4, mqttDriverCycle=on
  writeOps=13221409 opsPerSec=440713 batchAvgUs=22229 batchMaxUs=97097
  snapshots=27 snapshotAvgUs=136689 snapshotMaxUs=199563 snapshotPoints=264330
  mqttDriverCycles=25 mqttDriverAvgUs=246137 mqttDriverMaxUs=362920

writers=8, mqttDriverCycle=on
  writeOps=9369034 opsPerSec=312301 batchAvgUs=31357 batchMaxUs=188421
  snapshots=27 snapshotAvgUs=121094 snapshotMaxUs=218269 snapshotPoints=264330
  mqttDriverCycles=24 mqttDriverAvgUs=250297 mqttDriverMaxUs=311094

writers=16, mqttDriverCycle=on
  writeOps=8093251 opsPerSec=269775 batchAvgUs=36302 batchMaxUs=358790
  snapshots=27 snapshotAvgUs=122554 snapshotMaxUs=162696 snapshotPoints=264330
  mqttDriverCycles=24 mqttDriverAvgUs=251366 mqttDriverMaxUs=367973
```

近似纯写入上限：

```text
writers=4, mqttDriverCycle=off, snapshotIntervalMs=60000
  writeOps=15896511 opsPerSec=529883 batchAvgUs=18479 batchMaxUs=60872
  snapshots=1 snapshotAvgUs=135354 snapshotMaxUs=135354 snapshotPoints=9790
```

结论：

- 8 路分片后，最佳实测点不是 8 或 16 写线程，而是 4 写线程，写吞吐约 `440713 ops/s`。
- 近似关闭 MQTT 扫描和高频快照后，4 写线程写入上限约 `529883 ops/s`。
- MQTT Router 聚合 8 个共享内存后，9790 点扫描平均约 `188ms` 到 `251ms`，在 1 秒实时全量周期内可用。
- 快照平均约 `90ms` 到 `137ms`，在 9790 点规模下可接受。
- 16 写线程吞吐下降，说明线程数超过当前锁竞争和 CPU 调度最佳点后会退化。
- 单写线程结果很差不是分片设计问题，而是单线程无法模拟 8 个独立接口驱动并发写入。
- 后续真实 8 路 485 部署时，应采用“每个串口一个驱动实例 + 独立 shm + 一个 MQTT Driver 聚合”的模式。

仍需优化：

- `MemoryPointStore` 内部 latest slot 查找仍是线性扫描，大规模点位下首次插入和全量扫描仍有明显成本。
- 下一步可增加 index 到 slot 的哈希索引或固定桶索引，进一步降低 `putLatest()` 和 `getLatestByIndexes()` 成本。

#### 告警与变位事件解耦方案

落地状态：

- 已新增 `EventEngine` 可执行程序。
- 已新增 `EventEngineService`。
- 已在共享内存中增加 `PointUpdateRecord` 环形队列。
- `MemoryPointStore::putLatest()` 写入 latest 后会追加更新通知。
- `EventEngine` 消费更新通知后立即判断告警和变位。
- `MqttDriverService` 已移除告警和变位判断入口，只保留 MQTT 下行、OTA、按周期 full snapshot。
- 第二阶段已落地：默认使用 `mqtt_driver_outbox`，`EventEngine` 只写事件 outbox，由 `MqttDriver` 统一补发和发送。

部署注意：

- 共享内存布局版本已升级为 `6`。
- 升级后必须停止旧 `ModbusRtu / Dlt645Driver / MqttDriver / EventEngine` 进程。
- 如果边端已有旧版本共享内存段，应清理 `/dev/shm/gateway_point_store*` 后再启动新进程。
- systemd 新增 `event-engine@.service`，`gateway-services.sh` 会在 app 配置 `eventEngine.enabled=true` 时启动它。

解耦前实现状态：

- `AlarmService` 已经是独立类，但调用入口在 `MqttDriverService::processAlarms()`。
- 变位上传状态保存在 `MqttDriverService::publishedStates_`。
- `MqttDriverService::runScanOnce()` 当前同时承担：
- MQTT 下行命令和 OTA 消息处理。
- 全量 snapshot 定时上传。
- 变位判断。
- 告警判断。
- 告警持久化。

当前问题：

- 告警和变位事件依赖 MQTT Driver 扫描周期。
- MQTT Driver 停止或重启时，事件判断也停止。
- 点位值写入共享内存后不会立即触发事件判断，事件时序受 `mqttDriver.scanIntervalMs` 影响。
- MQTT 断链时，告警和变位事件的检测、存储、补发职责混在一起，后续维护成本高。

目标设计：

```text
ModbusRtu / Dlt645Driver / 后续 CAN / DI / DO Driver
  -> 写入接口级共享内存 latest value
  -> 同步写入轻量 PointUpdate 队列

EventEngine
  -> 消费 PointUpdate 队列
  -> 按点位 index 读取最新值
  -> 判断告警规则
  -> 判断变位规则
  -> 事件先落 SQLite outbox
  -> 在线时触发 MQTT 事件发送

MqttDriver
  -> 处理 MQTT 连接
  -> 处理命令下发
  -> 处理 OTA
  -> 按 fullUploadIntervalMs 定时扫 shared memory 上传 snapshot
  -> 接收 EventEngine 事件发送请求或补发 SQLite outbox
```

核心原则：

- 告警判断不放在 `MqttDriverService`。
- 变位判断不放在 `MqttDriverService`。
- `MqttDriverService` 的周期扫描只用于全量实时 snapshot。
- 点位更新后立即进入事件判断链路，不再等待 MQTT scan。
- 告警、变位事件以发生时间为准，必须先持久化，再发送。
- MQTT 断链不影响事件判断，只影响事件发送。

新增进程：

```text
EventEngine
```

建议启动方式：

```bash
/opt/modbus-gateway/bin/EventEngine \
  --app-config /opt/modbus-gateway/config/runtime/apps/mqtt-service.json
```

systemd：

```text
event-engine@mqtt-service.service
```

`EventEngine` 加载内容：

- `appConfig.mqttDriver.deviceConfigFiles`
- `appConfig.mqttDriver.sharedMemoryNames`
- `appConfig.alarmStore`
- MQTT 事件 topic 配置
- 每个点位的 `alarms[]`
- 每个点位的 `reportOnChange`

输入机制：

每个接口级共享内存增加一个轻量更新队列：

```cpp
struct PointUpdateRecord {
    std::uint64_t seq;
    std::uint32_t index;
    std::int64_t ts;
    double value;
    int quality;
    std::int64_t expireAt;
};
```

驱动写入流程调整为：

```text
MemoryPointStore::putLatest(value)
  -> 更新 latest slot
  -> append PointUpdateRecord 到当前 shm 的 ring queue
```

事件引擎消费流程：

```text
for each sharedMemoryName:
  read PointUpdateRecord after lastSeq
  if gap detected:
    fallback scan current shm latest values
  for each update:
    route = PointStoreRouter.routeByIndex(update.index)
    latest = router.getLatestByIndex(update.index)
    evaluate alarm
    evaluate reportOnChange
    persist event
    notify MQTT event sender
```

队列溢出处理：

- 每个共享内存队列使用固定容量环形队列。
- `EventEngine` 保存每个 shm 的 `lastSeq`。
- 如果发现 `seq` 不连续，说明队列被覆盖。
- 发生覆盖时，不直接丢失状态，而是对该 shm 做一次补偿扫描：
- 读取该 shm 所有 latest。
- 对配置中有告警或变位的点重新判断。
- 记录 `event-engine-resync` 状态日志。

事件状态存储：

告警状态：

```text
alarm_state
  machine_code
  meter_code
  point_code
  point_index
  alarm_type
  active
  last_value
  last_ts
```

变位状态：

```text
change_state
  point_index
  last_value
  last_quality
  last_ts
```

事件 outbox：

```text
event_outbox
  id
  event_type        alarm / change
  topic
  payload
  event_ts
  created_at
  sent
  sent_at
  retry_count
```

为什么状态要落盘：

- `EventEngine` 重启后不会把所有当前值误判成新变位。
- 告警恢复和触发顺序可保持稳定。
- MQTT 断链后事件可补发。
- 设备断电恢复后，仍可按上次状态继续判断。

告警持久化规则：

- 如果告警规则配置了 `persistValue`，告警触发或恢复事件写入 `alarm_events`。
- 如果 `persistValue` 为空，不写业务告警历史表。
- 无论 `persistValue` 是否为空，事件发送 outbox 仍需要记录，确保 MQTT 断链后可补发。

MQTT 发送职责调整：

`MqttDriverService` 保留：

- `processIncomingMessages()`：命令下发、OTA。
- `publishFullSnapshotNow()`：定时全量实时数据。
- `publishOnDemandNow()`：主动拉取数据。
- `replayPendingOtaStatuses()`：OTA 状态补发。
- MQTT 连接、重连、publish。

`MqttDriverService` 移除：

- `AlarmService alarmService_`
- `processAlarms()`
- `publishChangedValues()`
- `publishedStates_`
- 告警持久化直接调用

`MqttDriverService::runScanOnce()` 目标结构：

```cpp
void MqttDriverService::runScanOnce(std::int64_t nowMs) {
    replayPendingOtaStatusesIfNeeded(nowMs);
    processIncomingMessages(nowMs);
    replayEventOutboxIfNeeded(nowMs);

    if (shouldPublishFullSnapshot(nowMs)) {
        publishFullSnapshotNow(nowMs);
    }
}
```

事件发送方式建议分两阶段实现。

第一阶段：EventEngine 自带 MQTT publisher。

```text
EventEngine
  -> 判断事件
  -> 写 SQLite outbox
  -> 直接调用 IMqttDriverPublisher.publishAlarm / publishChangeEvent
```

优点：

- 改动小。
- 不需要先做进程间 IPC。
- 告警和变位判断已经从 MqttDriver 解耦。

限制：

- EventEngine 也会持有 MQTT 连接。
- 边端会有两个 MQTT 客户端：`MqttDriver` 和 `EventEngine`。
- clientId 必须区分，例如：
- `GW0001_mqtt_driver`
- `GW0001_event_engine`

第二阶段：EventEngine 只写 outbox，MqttDriver 统一发送。

当前默认模式：

- `eventEngine.publishMode = "mqtt_driver_outbox"`。
- `EventEngine` 对 `alarm/change` 事件不直接发 MQTT。
- `MqttDriverService::runScanOnce()` 会补发 `mqtt_event_outbox` 中的未发送事件。
- 事件 payload 使用事件发生时的值和时间戳，不再回读 `latest` 覆盖事件值。

```text
EventEngine
  -> 判断事件
  -> 写 SQLite outbox
  -> 通过 Unix Domain Socket / 本地 pipe 通知 MqttDriver

MqttDriver
  -> 收到通知后立即读取 outbox 未发送事件
  -> 发布 MQTT
```

优点：

- 全部 MQTT 连接集中在 `MqttDriver`。
- MQTT 断链、补发、限流、QoS 统一处理。
- 更符合“MQTT 服务只负责传输”的边界。

代价：

- 需要增加本地 IPC。
- `MqttDriver` 需要增加 outbox replay 逻辑。

推荐实施顺序：

1. 新增 `EventEngine` 进程。
2. 新增 `PointUpdateRecord` 环形队列。
3. `MemoryPointStore::putLatest()` 写 latest 后追加更新队列。
4. 抽出 `ChangeEventService`，管理变位状态。
5. `EventEngine` 组合 `AlarmService + ChangeEventService + PointStoreRouter`。
6. 新增 SQLite 表：`alarm_state`、`change_state`、`event_outbox`。
7. 第一阶段让 `EventEngine` 直接发布 MQTT 事件，快速完成解耦。
8. `MqttDriverService` 移除告警和变位判断，只保留 snapshot、命令、OTA。
9. 第二阶段再把事件发送统一收敛到 `MqttDriver` outbox replay。

配置建议：

```json
{
  "eventEngine": {
    "enabled": true,
    "scanFallbackIntervalMs": 5000,
    "updateQueueCapacity": 65536,
    "eventOutboxSqlitePath": "/opt/modbus-gateway/data/event_outbox.db",
    "publishMode": "mqtt_driver_outbox",
    "mqttClientIdSuffix": "event_engine",
    "outboxReplayIntervalMs": 1000,
    "outboxRetentionDays": 365
  }
}
```

字段说明：

- `enabled`：是否启用独立事件引擎。
- `scanFallbackIntervalMs`：更新队列异常或长时间无更新时的兜底扫描周期。
- `updateQueueCapacity`：每个共享内存分片的更新通知环形队列容量。
- `eventOutboxSqlitePath`：事件 outbox 数据库路径。
- `publishMode`：
- `mqtt_driver_outbox`：默认模式。EventEngine 只写 outbox，由 MqttDriver 统一发送。
- `direct_mqtt`：兼容模式。EventEngine 直接发布事件。
- `mqttClientIdSuffix`：第一阶段 EventEngine 独立 MQTT clientId 后缀。
- `outboxReplayIntervalMs`：断链恢复后补发间隔。
- `outboxRetentionDays`：事件 outbox 清理周期，默认 365 天。

事件 MQTT payload 保持现有格式：

告警：

```json
{
  "type": "alarm",
  "machineCode": "GW0001",
  "meterCode": "SLAVE0001",
  "pointCode": "voltage_a",
  "index": 11000,
  "alarmType": "high",
  "active": true,
  "value": 2400,
  "quality": 1,
  "ts": 1776841659459,
  "stale": false
}
```

变位：

```json
{
  "type": "change",
  "machineCode": "GW0001",
  "meterCode": "SLAVE0001",
  "pointCode": "run_state",
  "index": 11008,
  "value": 1,
  "quality": 1,
  "ts": 1776841659459,
  "stale": false
}
```

时序保证：

- 单点位内事件顺序按 `PointUpdateRecord.seq` 和 `ts` 判断。
- 同一告警规则先恢复、后触发的顺序继续由 `AlarmService` 保证。
- 变位事件只在值发生变化时产生一次，后续值不变不重复产生。
- 如果队列溢出触发补偿扫描，可能无法还原溢出期间的每一次中间变化，但可以恢复最终状态并继续判断。

性能影响：

- 驱动每次写点时多写一条轻量 ring queue 记录，开销低于全量扫描。
- EventEngine 只处理变化过的点，告警和变位延迟接近采集写入延迟。
- MQTT snapshot 不再每个 scan 周期都处理告警和变位，CPU 压力下降。
- 大规模点表下，事件判断成本从“按全量点位周期扫描”变为“按实际更新量处理”。

风险与约束：

- 需要调整共享内存布局版本，升级后必须停止旧进程并清理旧 shm。
- 如果 EventEngine 长时间停止，更新队列可能被覆盖，只能通过兜底扫描恢复最终状态。
- 第一阶段 direct MQTT 会引入第二个 MQTT 客户端，需要确认 broker clientId 不冲突。
- 第二阶段 outbox IPC 需要额外实现本地通知机制。

验收标准：

- 停止 `MqttDriver`，启动采集驱动和 `EventEngine`，告警/变位状态仍能写入 SQLite outbox。
- 恢复 `MqttDriver` 或 EventEngine MQTT 连接后，历史事件能补发。
- 点位从 `0 -> 1` 只产生一条变位事件，保持 `1` 不重复上传。
- 告警值跨越阈值后立即产生告警事件，不等待 full snapshot 周期。
- `MqttDriver` 的 full snapshot 仍只按 `fullUploadIntervalMs` 上传。

优化后综合压测结论：

压测场景保持不变：

- 8 个共享内存分片。
- 总点数 `9790`。
- 全点位开启 `reportOnChange=true`。
- 全点位配置 `high` 告警阈值 `250`。
- `eventEngine.publishMode = "mqtt_driver_outbox"`。
- `mqtt.enabled = false`，只测边端内部事件链路。

本轮优化项：

- `EventEngine` 对同一批次内同一点位的变位事件做合并，只保留该批最后一次变化。
- `MqttEventOutbox` 使用批量 `markSentBatch()`，减少逐条更新 SQLite 的开销。
- `MqttDriver` 增加独立 `replayLoop()`，事件补发不再和 snapshot 主扫描共用一条执行链。

优化前结果：

- `writer=4`
  - `writeOps=9790`
  - `opsPerSec=489`
  - `batchAvgUs=27235892`
  - `snapshotAvgUs=23271711`
  - `outbox=(7663 total, 5463 unsent, 2200 sent)`
- `writer=8`
  - `writeOps=9790`
  - `opsPerSec=489`
  - `batchAvgUs=26805098`
  - `snapshotAvgUs=21374023`
  - `outbox=(7850 total, 6250 unsent, 1600 sent)`

优化后结果：

- `writer=4`
  - `writeOps=9790`
  - `opsPerSec=489`
  - `batchAvgUs=40824085`
  - `snapshotAvgUs=26918484`
  - `mqttDriverAvgUs=23548154`
  - `outbox=(2620 total, 1033 unsent, 1587 sent)`
- `writer=8`
  - `writeOps=9790`
  - `opsPerSec=489`
  - `batchAvgUs=37327037`
  - `snapshotAvgUs=20627579`
  - `mqttDriverAvgUs=23354018`
  - `outbox=(5792 total, 4514 unsent, 1278 sent)`

结果解读：

- 本轮 `stress_runner` 统计里 `mqttDriverCycle=on`，因此 `batchAvgUs / mqttDriverAvgUs / snapshotAvgUs` 更适合看同轮趋势，跨轮主对比指标应以 `outbox backlog` 为准。
- 吞吐上限仍然不是写线程数，而是事件链路本身。
- 批内变位合并已经明显降低了 `outbox` 总量，尤其 `writer=4` 下从 `7663` 降到 `2620`。
- `writer=4` 下未发送积压从 `5463` 降到 `1033`，说明批量确认和独立 replay 线程已经起效。
- `writer=8` 仍然会积压，但总事件量和未发送数都低于优化前。
- `snapshot` 耗时没有出现数量级下降，说明当前仍然有共享内存全量扫描本身的成本。
- 在“9790 点全部开启告警和变位”的极端配置下，系统仍然处于高压区，这不适合作为真实生产默认策略。

部署建议：

- `reportOnChange` 只对数字量、设备在线量、少量关键状态量开启。
- 模拟量告警可继续保留，但不建议全点位同时开启变位上传。
- 真实现场建议优先控制事件点比例，而不是继续增加 writer 线程数。

后续优化补充：

- `MqttDriverConfig` 新增：
  - `snapshotBacklogThreshold`
  - `snapshotBackoffIntervalMs`
  - `eventReplayMaxBytes`
- 当 `mqtt_event_outbox` 未发送事件数超过阈值时，`MqttDriver` 会临时推迟下一次 full snapshot。
- 保护目标不是减少实时数据，而是在事件积压时优先让 `event-outbox-replayed` 跑完，避免 snapshot 持续占住 CPU。
- `mqtt_event_outbox` replay 顺序按事件类型优先级执行：
  - `alarm`
  - `change`
  - 其他事件
- 单轮 replay 同时受 `eventReplayMaxBytes` 限制，避免单次补发占用过长时间片。
- 边端临时回归中已观察到状态日志：
  - `full-snapshot-deferred`
  - `event-outbox-replayed`
- 建议生产初值：
  - `snapshotBacklogThreshold = 2000`
  - `snapshotBackoffIntervalMs = 5000`
  - `eventReplayMaxBytes = 262144`

### deploy/ota-rollback.sh

```bash
#!/bin/sh
set -eu

ARTIFACT_PATH="${1:-}"
VERSION="${2:-}"
JOB_ID="${3:-}"
BACKUP_DIR="${4:-}"
STAGING_DIR="${5:-}"

if [ -z "$ARTIFACT_PATH" ] || [ -z "$VERSION" ] || [ -z "$JOB_ID" ] || [ -z "$BACKUP_DIR" ] || [ -z "$STAGING_DIR" ]; then
  echo "[ota-rollback] usage: ota-rollback.sh <artifactPath> <version> <jobId> <backupDir> <stagingDir>" >&2
  exit 2
fi

TIMESTAMP="$(date '+%Y-%m-%d %H:%M:%S')"
LOG_FILE="$STAGING_DIR/upgrade_history.log"
STATE_FILE="$STAGING_DIR/current_version.txt"
ROLLBACK_MARK="$STAGING_DIR/rollback_${JOB_ID}.txt"
RESTORE_LIST="$STAGING_DIR/rollback_${JOB_ID}_restored.txt"
RESTART_FILE="$STAGING_DIR/rollback_${JOB_ID}_restart_services.txt"

mkdir -p "$BACKUP_DIR" "$STAGING_DIR"

echo "[$TIMESTAMP] [ota-rollback] start jobId=$JOB_ID version=$VERSION artifact=$ARTIFACT_PATH" | tee -a "$LOG_FILE"

if [ -f "$STATE_FILE" ]; then
  cp "$STATE_FILE" "$ROLLBACK_MARK"
fi

if [ -f "$BACKUP_DIR/previous_version.txt" ]; then
  cp "$BACKUP_DIR/previous_version.txt" "$STAGING_DIR/applied_version.txt"
fi

if [ -f "$STATE_FILE" ]; then
  WORK_DIR="$(awk -F= '/^workDir=/{print $2}' "$STATE_FILE" | tail -n 1)"
  if [ -n "${WORK_DIR:-}" ] && [ -f "$WORK_DIR/restart_services.txt" ]; then
    cp "$WORK_DIR/restart_services.txt" "$RESTART_FILE"
  fi
fi

if [ -d "$BACKUP_DIR/opt" ] || [ -d "$BACKUP_DIR/etc" ]; then
  python3 - "$BACKUP_DIR" "$RESTORE_LIST" <<'PY'
import os
import shutil
import sys

backup_dir, restore_list = sys.argv[1:3]
restored = []
for top in ("opt", "etc"):
    root = os.path.join(backup_dir, top)
    if not os.path.isdir(root):
        continue
    for current_root, _, files in os.walk(root):
        for name in files:
            src = os.path.join(current_root, name)
            rel = os.path.relpath(src, backup_dir)
            dst = os.path.join("/", rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
            restored.append(f"{src} -> {dst}")
with open(restore_list, "w", encoding="utf-8") as fh:
    for item in restored:
        fh.write(item + "\n")
PY
fi

if command -v systemctl >/dev/null 2>&1 && [ -f "$RESTART_FILE" ]; then
  systemctl daemon-reload || echo "[$TIMESTAMP] [ota-rollback] daemon-reload failed" | tee -a "$LOG_FILE" >&2
  while IFS= read -r service; do
    [ -z "$service" ] && continue
    if [ "$service" = "gateway-services.service" ]; then
      systemctl enable "$service" || echo "[$TIMESTAMP] [ota-rollback] enable failed $service" | tee -a "$LOG_FILE" >&2
    fi
    echo "[$TIMESTAMP] [ota-rollback] restarting $service" | tee -a "$LOG_FILE"
    systemctl restart "$service" || echo "[$TIMESTAMP] [ota-rollback] restart failed $service" | tee -a "$LOG_FILE" >&2
  done < "$RESTART_FILE"
fi

{
  echo "jobId=$JOB_ID"
  echo "rollbackFromVersion=$VERSION"
  echo "artifact=$ARTIFACT_PATH"
  echo "rollbackAt=$TIMESTAMP"
  echo "restoreList=$RESTORE_LIST"
} >> "$ROLLBACK_MARK"

echo "[$TIMESTAMP] [ota-rollback] success jobId=$JOB_ID" | tee -a "$LOG_FILE"
exit 

## machineCode 定向配置拉取与平台快照编辑

### 目标

- 平台不再假设本地模板就是边端当前运行配置。
- 先按 `machineCode` 定向从边端拉取运行配置。
- 平台保存边端配置快照，并把快照加载到设备配置和 App 配置编辑器。
- 用户修改后再生成配置 OTA 包，定向下发到 `machineCode`。

### MQTT Topic 规则

- 配置拉取请求基础 topic：
  - `edge/config/pull/request`
- 配置拉取回复基础 topic：
  - `edge/config/pull/reply`
- 运行时自动按 `machineCode` 定向：
  - 请求：
    - `edge/config/pull/request/<machineCode>`
  - 回复：
    - `edge/config/pull/reply/<machineCode>`

### 边端行为

- `SystemMonitor` 订阅：
  - `edge/config/pull/request/<machineCode>`
- 收到请求后读取当前运行配置文件：
  - 当前 app 配置文件
  - app 配置里声明的 `deviceConfigFiles`
- 回复 payload：

```json
{
  "requestId": "CFG_PULL_1777261368693",
  "machineCode": "GW0001",
  "success": true,
  "files": [
    {
      "path": "/opt/modbus-gateway/config/runtime/apps/monitor-service.json",
      "sizeBytes": 4287,
      "modifiedAtMs": 1777261123000,
      "content": "{...json text...}"
    }
  ],
  "ts": 1777261368840
}
```

当回复体超过 MQTT broker 单包限制时，边端不会把完整配置快照一次性塞进 MQTT，而是按 `16KB` 原始字节分片后上送。分片内容使用十六进制字段承载，避免中文或其他 UTF-8 多字节字符被切半后导致平台 JSON 解析失败。

分片回复 payload：

```json
{
  "requestId": "CFG_PULL_1777261368693",
  "machineCode": "GW0001",
  "success": true,
  "chunked": true,
  "chunkIndex": 1,
  "chunkCount": 4,
  "totalBytes": 58820,
  "payloadHex": "7B22726571756573744964223A...",
  "ts": 1777261368840
}
```

平台处理规则：

- 按 `machineCode + requestId` 建立临时分片缓冲。
- `chunkIndex` 从 `1` 开始，允许乱序到达和重复到达。
- 收齐 `chunkCount` 后按字节重组完整 JSON，再按普通快照落库。
- 未收齐的分片缓冲保留 `120s`，接收进度保留 `300s`。
- 页面通过进度接口显示 `receivedChunks / chunkCount / percent / receivedBytes / totalBytes`。

### 平台侧落库

- 新增快照表：
  - `edge_config_snapshot`
- 新增快照文件明细表：
  - `edge_config_snapshot_file`

平台收到 `edge/config/pull/reply/<machineCode>` 后：

- 以 `machineCode + requestId` 保存一份边端配置快照。
- 每个文件保存：
  - `path`
  - `sizeBytes`
  - `modifiedAtMs`
  - `contentText`

### 平台接口

- 发起配置拉取：
  - `POST /api/system-monitor/config/pull`
- 查询最新快照：
  - `GET /api/system-monitor/config/snapshot/latest?machineCode=GW0001`
- 查询配置拉取进度：
  - `GET /api/system-monitor/config/pull/progress?machineCode=GW0001&requestId=CFG_PULL_1777261368693`

请求示例：

```json
{
  "requestId": "CFG_PULL_1777261368693",
  "machineCode": "GW0001",
  "includeFiles": true,
  "ts": 1777261368693
}
```

### 前端交互

- `协议设备配置`
- `MQTT / OTA / App`

以上两个大模块都先收敛到 `machineCode` 上下文。

操作按钮：

- `从边端拉取配置`
- `加载最新快照`
- `按所选文件加载`

行为：

- `从边端拉取配置`
  - 平台向 `edge/config/pull/request/<machineCode>` 发请求。
- `加载最新快照`
  - 平台读取该 `machineCode` 最新快照。
- 平台展示快照文件选择：
  - `App 文件快照`
  - `设备文件快照`
- `按所选文件加载`
  - 把当前选中的 `/apps/` 文件加载到 App 编辑器。
  - 把当前选中的 `/devices/` 文件加载到设备编辑器。
- 平台同时显示当前草稿状态：
  - `未加载快照`
  - `与快照一致`
  - `与快照有差异`

### 已验证结果

本轮实测：

- 平台通过 `POST /api/system-monitor/config/pull` 发起请求。
- 边端 `GW0001` 正常回复。
- 平台 `latest snapshot` 已能查到最新快照。
- 快照文件数：
  - `3`
- 首批文件路径：
  - `/opt/modbus-gateway/config/runtime/apps/monitor-service.json`
  - `/opt/modbus-gateway/config/runtime/devices/device_slave_ttySP1.json`
  - `/opt/modbus-gateway/config/runtime/devices/device_slave_ttySP2.json`

### 下一步

- 把“加载最新快照”从当前的首文件自动映射，继续扩展成：
  - 多设备配置文件选择
  - 草稿差异对比
  - 基于快照直接生成配置 OTA

### 配置 OTA 约束

- 平台生成或发布配置 OTA 前，必须先满足：
  - 已拉取边端最新快照
  - 已把选中的 `App 文件快照` 加载到 App 编辑器
  - 已把选中的 `设备文件快照` 加载到设备编辑器
- OTA 页面会直接显示：
  - 快照 `requestId`
  - 本次打包 `App 文件`
  - 本次打包 `设备文件`
- 未满足上述条件时：
  - `生成配置 OTA 包`
  - `生成并发布配置 OTA`
  两个按钮会保持禁用

### OTA 与分片边界

- 配置拉取快照可以走 MQTT 分片，因为它是平台读取边端当前小文件内容，分片后仍可在平台内存中重组。
- OTA 包不走 MQTT 分片。OTA 任务 MQTT payload 只携带 `artifactUrl / sha256 / size / version / machineCode` 等元数据。
- 边端收到 OTA 请求后通过 `artifactUrl` 使用 HTTP 下载完整 tar.gz，再做 sha256 校验和 apply。
- OTA 传输进度不由 MQTT 分片体现，而由边端持续上报 `edge/ota/status/<machineCode>`。
- 平台页面应以 OTA 状态时间线展示进度；大文件下载如果后续需要精确到字节级进度，需要边端下载器支持周期性上报 `downloadedBytes / totalBytes`，而不是把 OTA 包拆成 MQTT 消息。
