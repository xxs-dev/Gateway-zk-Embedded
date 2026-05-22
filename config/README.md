# Config 目录说明

`config/` 已按用途拆成三类：

## 1. 运行时依赖配置

目录：

- [runtime/apps](D:/workspace/Embedded/Gateway-zk/config/runtime/apps)
- [runtime/devices](D:/workspace/Embedded/Gateway-zk/config/runtime/devices)

说明：

- `runtime/devices` 放协议驱动直接加载的设备采集配置，包括 `ModbusRtu`、`Dlt645Driver`、`DioDriver`、`CanDriver`
- `runtime/apps` 放应用级服务配置，包括 `mqtt-service.json`、`monitor-service.json`
- `runtime/device_identity.json` 放网关本机身份，包括 `machineCode`、`imei`、序列号、型号和版本信息
- `runtime/tls` 放生产环境 MQTT TLS CA、客户端证书和可选 stunnel 兜底配置
- 这些文件会被程序实际读取，修改后会影响运行结果
- 出厂和运行样例默认所有协议驱动共用 `gateway_point_store`，MQTT、事件引擎和系统监测只需要读取这一个共享内存

## 2. 报文和日志样例

目录：

- [samples/messages](D:/workspace/Embedded/Gateway-zk/config/samples/messages)
- [samples/logs](D:/workspace/Embedded/Gateway-zk/config/samples/logs)

说明：

- `samples/messages` 放 MQTT 请求、回执、状态、告警等 JSON 样例
- `samples/logs` 放 OTA 历史日志样例
- 这些文件主要用于联调、文档和测试，不作为运行时必需配置

## 3. 出厂默认模板

目录：

- [factory](D:/workspace/Embedded/Gateway-zk/config/factory)

说明：

- `factory/runtime/apps` 放出厂默认的 `mqtt-service.json`、`monitor-service.json`
- `factory/runtime/device_identity.json` 放出厂默认网关身份模板
- `factory/runtime/devices` 放出厂默认的设备配置示例
- `factory/runtime/tls` 放出厂默认 TLS 证书目录和 stunnel 兜底配置模板，正式上线前必须替换 broker、CA 和证书路径
- 出厂模板默认引用 `ttySP1`、`ttySP2` 两个 Modbus RTU 示例串口，以及本机 `device_dio.json`
- CAN SocketCAN 示例模板放在 `factory/runtime/devices/device_can0.json`，同一联调样例也放在 `config/examples/device_can0_example.json`；未加入 `deviceConfigFiles[]` 时不会启动 CAN 驱动
- 本机 `device_dio.json` 包含 18 路 DI 和 8 路 DO，DI/DO 默认都按低有效处理，平台逻辑值为 `1=有效/闭合`
- `monitor-service.json` 默认启用 4G 模块状态采集，当前硬件按 `/dev/ttyUSB2`、`/dev/ttyUSB1`、`/dev/ttyUSB0` 顺序探测可响应 AT 口，并读取 SIM、注册、信号、运营商和流量状态
- 正式上线前必须修改 `device_identity.json` 中的 `machineCode`、`imei`，以及 MQTT broker、MQTT 账号密码和实际串口点表
- MQTT 配置里只填写基础 topic，运行时实际 topic 统一为 `<baseTopic>/<machineCode>`，例如 `edge/telemetry/GW0001`
- MQTT 配置里的 `clientId` 保持为当前 `machineCode`；驱动内部连接会追加 `-rx/-tx` 或服务后缀，避免同一 broker 下冲突
- 详细步骤见 [边端网关操作手册](D:/workspace/Embedded/Gateway-zk/doc/边端操作手册.md)

## 常用启动路径

单串口多从站：

```bash
./ModbusRtu --config config/runtime/devices/device_slave_ttySP1.json
./EventEngine --app-config config/runtime/apps/mqtt-service.json
./MqttDriver --app-config config/runtime/apps/mqtt-service.json
./SystemMonitor --app-config config/runtime/apps/monitor-service.json
```

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

`gateway-services.sh` 会根据 `runtime/apps/mqtt-service.json` 和 `runtime/apps/monitor-service.json` 中的 `deviceConfigFiles[]` 自动决定启动哪些协议驱动；`runtime/devices` 目录里未被 app 配置引用的 JSON 不会被启动。
如果 app 的 `mqtt.broker` 指向本机 stunnel 监听端口，且 `runtime/tls/*-stunnel.conf` 存在，统一服务入口会在 MQTT 相关服务前自动启动对应 `mqtt-tls-tunnel@*.service`。

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

生产设备推荐把 `gateway-factory-defaults.tar.gz` 和 `install-factory-config.sh` 放在 `/home` 或 `/home/gateway-factory`。初始化脚本会解压包并复制二进制、服务脚本、factory 配置、模板和样例到 `/opt/modbus-gateway`。不要把出厂默认包直接放在 `/opt/modbus-gateway` 内，否则源目录和运行目录容易重叠。

常用变量：

- `GATEWAY_HOME=/opt/modbus-gateway` 指定边端安装目录
- `DEFAULT_SOURCE_ROOT=/home/gateway-factory` 指定出厂默认包目录
- `SOURCE_ROOT=/home/gateway-factory` 显式指定本次初始化使用的源目录
- `START_SERVICES=0` 只安装配置，不启动服务
- `RESET_SHM=1` 停服务后清理旧共享内存，再恢复出厂配置

初始化脚本会继承当前 `/opt/modbus-gateway/config/runtime/device_identity.json` 中已有的 `machineCode`，不会把网关标识重置为出厂模板值；同时会把运行 app 配置里的 `clientId` 同步为该 `machineCode`。

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
