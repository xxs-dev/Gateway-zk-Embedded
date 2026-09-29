# Config 目录说明

`config/` 已按用途拆成三类：

## 1. 运行时依赖配置

目录：

- [runtime/apps](runtime/apps)
- [runtime/devices](runtime/devices)

说明：

- `runtime/devices` 放协议驱动直接加载的设备采集配置，包括 `ModbusRtu`、`Dlt645Driver`、`DioDriver`、`CanDriver`
- `runtime/apps` 放应用级服务配置，包括 `mqtt-service.json`、`monitor-service.json`
- `mqtt-service.json:mqttForward` 是独立第三方 MQTT 最新值转发配置；缺少该节点或 `enabled=false` 时不会启动转发器
- `mqtt-service.json:mqttDriver.fullUploadWorker.mode=isolated` 时，同一个转发器进程还会承载主平台周期 full；默认 `inline` 保持旧行为
- `runtime/device_identity.json` 放网关本机身份，包括 `machineCode`、`imei`、序列号、型号和版本信息
- `runtime/tls` 放生产环境 MQTT TLS CA、客户端证书和可选 stunnel 兜底配置
- 这些文件会被程序实际读取，修改后会影响运行结果
- 出厂和运行样例默认所有协议驱动共用 `gateway_point_store`，MQTT、事件引擎和系统监测只需要读取这一个共享内存

`runtime/apps/mqtt-service.json` 的全局 `pointHistory.retentionDays` 缺省为 30，数值兼容夹取到 1..3650 天，五类协议入口共用。它约束所有进入 `SqliteSampleWriter` 的 flagged 点位历史，不改变 `isStore`、`persistIntervalSec`、`persistOnChange` 的筛选规则，也不改变 full/realtime、MQTT outbox 或事件存储。

既有后台线程按 `ts < 当前时间 - 保留天数` 分批逻辑删除，边界和未来数据保留；每批最多 512 行，满批至少间隔 1 秒，扫尽后 60 秒检查，失败后 5 秒退避，控制优先时暂停。旧库首次建立 `ts` 索引可能耗时，SQLite busy wait、锁等待和磁盘 IO 不受批次行数保证；逻辑删除不保证数据库文件缩小，也不构成全局磁盘预算。

## 2. 报文和日志样例

目录：

- [samples/messages](samples/messages)
- [samples/logs](samples/logs)

说明：

- `samples/messages` 放 MQTT 请求、回执、状态、告警等 JSON 样例
- `samples/logs` 放 OTA 历史日志样例
- 这些文件主要用于联调、文档和测试，不作为运行时必需配置

## 3. 出厂默认模板

目录：

- [factory](factory)

说明：

- `factory/runtime/apps` 放出厂默认的 `mqtt-service.json`、`monitor-service.json`
- `factory/runtime/device_identity.json` 放出厂默认网关身份模板
- `factory/runtime/devices` 放出厂默认的设备配置示例
- `factory/runtime/tls` 放出厂默认 TLS 证书目录和 stunnel 兜底配置模板，正式上线前必须替换 broker、CA 和证书路径
- 出厂模板默认引用 `ttySP1`、`ttySP2` 两个 Modbus RTU 示例串口，以及本机 `device_dio.json`
- 生产初始化默认 `INIT_RUNTIME_MODE=gateway`；脚本会把 app 配置写成网关模式，并移除 EMS 虚拟设备引用和 `graphEms` 规则。EMS 项目必须显式传 `INIT_RUNTIME_MODE=ems` 或 `--runtime-mode ems`
- CAN SocketCAN 示例模板放在 `factory/runtime/devices/device_can0.json`，同一联调样例也放在 `config/examples/device_can0_example.json`；未加入 `deviceConfigFiles[]` 时不会启动 CAN 驱动
- 本机 `device_dio.json` 包含 18 路 DI 和 8 路 DO，DI/DO 默认都按低有效处理，平台逻辑值为 `1=有效/闭合`
- `monitor-service.json` 默认启用 4G 模块状态采集，当前硬件按 `/dev/ttyUSB2`、`/dev/ttyUSB1`、`/dev/ttyUSB0` 顺序探测可响应 AT 口，并读取 SIM、注册、信号、运营商和流量状态
- 正式上线前必须修改 `device_identity.json` 中的 `machineCode`、`imei`，以及 MQTT broker、MQTT 账号密码和实际串口点表
- MQTT 配置里只填写基础 topic，运行时实际 topic 统一为 `<baseTopic>/<machineCode>`。实时与全量已拆分，例如 `edge/telemetry/realtime/GW0001` 和 `edge/telemetry/full/GW0001`
- MQTT 配置里的 `clientId` 保持为当前 `machineCode`；驱动内部连接会追加 `-rx/-tx` 或服务后缀，避免同一 broker 下冲突
- 详细步骤见 [边端网关操作手册](../doc/部署运维/边端操作手册.md)

## 常用启动路径

单串口多从站：

```bash
./ModbusRtu --config config/runtime/devices/device_slave_ttySP1.json
./EventEngine --app-config config/runtime/apps/mqtt-service.json
./MqttDriver --app-config config/runtime/apps/mqtt-service.json
./SystemMonitor --app-config config/runtime/apps/monitor-service.json
```

启用 `mqttForward.enabled=true` 或 `mqttDriver.fullUploadWorker.mode=isolated` 后，数据转发工作器使用独立进程：

```bash
./MqttForwarder --app-config config/runtime/apps/mqtt-service.json
```

`mqttForward.pointIndexes` 是第三方专属点位数组。启用转发时必须至少配置一个不重复的 uint32 index；转发器每周期只读取这些点位的最新值，不读取或回退到点位的 `fullUpload` 标记，也不复用 `mqttDriver.fullUploadIndexes`、`publishAllOnFull` 或 `fullUploadJsonFormat`。`mqttForward.payloadFormat` 默认 `compactArray`，也可显式设为 `object` 或旧平台兼容格式 `legacy`。

`mqttForward.control` 缺失或 `enabled=false` 时第三方进程保持 TX-only：只向 `mqttForward.fullTelemetryTopic` 发布，不订阅、不处理控制或实时监测命令，也不做离线补发。只有显式设置 `mqttForward.control.enabled=true` 后，进程才订阅一个精确的第三方控制 topic；此时 `mqttForward.qos` 必须为 `1` 或 `2`。控制结果数据库无需增加配置项，默认由 `control.ownershipFile` 派生：以 `.json` 结尾时替换为 `.control-results.db`，否则直接追加该后缀。详细仲裁规则见 [第三方储能控制权接管设计](../doc/架构设计/第三方储能控制权接管设计.md)。主 MQTT 的 realtime/full 启停与第三方周期转发互不影响。

控制与事件转发共用一次 `runOnce()` 调度时，每轮最多补发一条事件，确保下一轮先得到控制轮询机会。该限制只约束一轮中的事件条数；`replayMaxBytes` 也只是单条/批次字节边界，不是 MQTT 发布时限。当前 `publishReliableJsonMessage()` 没有覆盖连接、发送和等待 ACK 全过程的单一绝对 deadline，单条调用仍可能受底层 Socket/TLS 行为阻塞。配置发布周期时必须把这一残余边界计入控制响应预算，不能把“每轮一条”等同于有界实时调度。

多串口：

```bash
./ModbusRtu --config config/runtime/devices/device_slave_ttySP1.json
./ModbusRtu --config config/runtime/devices/device_slave_ttySP2.json
./EventEngine --app-config config/runtime/apps/mqtt-service.json
./MqttDriver --app-config config/runtime/apps/mqtt-service.json
./SystemMonitor --app-config config/runtime/apps/monitor-service.json
```

DLT645-2007 单串口多表：

```bash
./Dlt645Driver --config config/runtime/devices/device_dlt645_multi_meter_1_2.json --app-config config/runtime/apps/mqtt-service.json
./EventEngine --app-config config/runtime/apps/mqtt-service.json
./MqttDriver --app-config config/runtime/apps/mqtt-service.json
```

模拟采集联调时才追加 `--mock`。

本机 DI/DO：

```bash
./DioDriver --config config/runtime/devices/device_dio.json --app-config config/runtime/apps/mqtt-service.json
```

## systemd 推荐入口

部署到边端后推荐只启用统一服务管理器：

```bash
systemctl enable gateway-services.service
systemctl start gateway-services.service
```

`gateway-services.sh` 会根据 `runtime/apps/mqtt-service.json` 和 `runtime/apps/monitor-service.json` 中的 `deviceConfigFiles[]` 自动决定启动哪些协议驱动；`runtime/devices` 目录里未被 app 配置引用的 JSON 不会被启动。MQTT、事件、计算、监测、本地画面和摄像头服务也只在对应配置开关或通道启用时进入启动清单。
如果 app 的 `mqtt.broker` 指向本机 stunnel 监听端口，且 `runtime/tls/*-stunnel.conf` 存在，统一服务入口会在 MQTT 相关服务前自动启动对应 `mqtt-tls-tunnel@*.service`。
`mqttForward.enabled=true` 或 `mqttDriver.fullUploadWorker.mode=isolated` 时会额外启动 `mqtt-forwarder@mqtt-service.service`。主 full 工作器仅建立独立 TX 连接并由主驱动短租约兜底；第三方输出只有在 `mqttForward.control.enabled=true` 时增加独立 RX 连接。出厂默认 `fullUploadWorker.mode=inline` 且关闭第三方转发，因此不在下面的默认清单中。

当前出厂默认服务发现结果应包含：

```text
modbus-rtu@device_slave_ttySP1.service
modbus-rtu@device_slave_ttySP2.service
dio-driver@device_dio.service
event-engine@mqtt-service.service
mqtt-driver@mqtt-service.service
system-monitor@monitor-service.service
```

## 出厂安装脚本

```bash
sh deploy/install-factory-config.sh
```

生产设备推荐把 `gateway-factory-defaults.tar.gz` 和 `install-factory-config.sh` 放在 `/home` 或 `/home/gateway-factory`。初始化脚本会解压包并复制二进制、服务脚本、factory 配置、模板和样例到 `/opt/modbus-gateway`。不要把出厂默认包直接放在 `/opt/modbus-gateway` 内，否则源目录和运行目录容易重叠。发布前需要确认边端包、桌面初始化内置包和平台 `config/deploy` 包三份 SHA256 一致。

常用变量：

- `GATEWAY_HOME=/opt/modbus-gateway` 指定边端安装目录
- `DEFAULT_SOURCE_ROOT=/home/gateway-factory` 指定出厂默认包目录
- `SOURCE_ROOT=/home/gateway-factory` 显式指定本次初始化使用的源目录
- `INIT_RUNTIME_MODE=gateway|ems|agc_avc` 指定运行模式，默认 `gateway`
- `INIT_RUNTIME_PACKAGE=/path/gateway-agc-avc-runtime.tar.gz` 指定 AGC/AVC 独立运行模式包；仅 `agc_avc` 模式需要
- `START_SERVICES=0` 只安装配置，不启动服务
- `RESET_SHM=1` 停服务后清理旧共享内存，再恢复出厂配置

初始化脚本会继承当前 `/opt/modbus-gateway/config/runtime/device_identity.json` 中已有的 `machineCode`，不会把网关标识重置为出厂模板值；同时只把运行 app 配置中主 `mqtt.clientId` 同步为该 `machineCode`。`mqttForward` 的 broker、账号、密码、TLS 和可选 clientId 始终独立，不会继承主 MQTT 凭据。第三方控制所有权文件默认继承 `mqttDriver.powerControlOwnershipFile`，显式填写时必须与其完全相同。未指定 `INIT_RUNTIME_MODE` 时按网关模式安装；EMS 项目传 `ems`，AGC/AVC 项目传 `agc_avc` 并同时提供独立运行模式包。

日常运维入口：

```bash
/opt/modbus-gateway/bin/gateway-run.sh list
/opt/modbus-gateway/bin/gateway-run.sh restart
/opt/modbus-gateway/bin/gateway-run.sh status
/opt/modbus-gateway/bin/gateway-run.sh logs
/opt/modbus-gateway/bin/gateway-run.sh stats
/opt/modbus-gateway/bin/gateway-run.sh health
/opt/modbus-gateway/bin/gateway-run.sh smoke
```

确认需要清理压测或异常退出留下的运行缓存时，先执行：

```bash
/opt/modbus-gateway/bin/gateway-run.sh cleanup
```

`cleanup` 会停止网关服务、删除 `/dev/shm/gateway_point_store*` 和 `/tmp/gateway-stress-*` / `/tmp/gateway-ci-*` 临时目录；生产现场不要在服务运行中手工删除共享内存。
