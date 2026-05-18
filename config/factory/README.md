# 出厂默认配置说明

该目录保存边端网关的出厂默认运行配置。推荐先用 `deploy/build-factory-package.sh` 生成 `gateway-factory-defaults.tar.gz`，再把压缩包和初始化脚本放到边端 `/home` 目录执行，避免默认文件和 `/opt/modbus-gateway` 运行目录互相覆盖。

## 默认包含内容

出厂默认包包含：

- `config/factory/runtime`：运行时默认配置。
- `config/templates`：DLT645 标准点表等模板。
- `config/examples`：配置样例。
- `deploy`：systemd 模板、初始化脚本、OTA 脚本和运维脚本。
- `build-aarch64`：如果本机已完成交叉编译，则包含边端可执行文件。

默认运行配置包含：

- `runtime/device_identity.json`：网关身份模板，默认 `machineCode=GW_FACTORY_001`。
- `runtime/apps/mqtt-service.json`：第三方 MQTT 转发、事件、OTA 配置。
- `runtime/apps/monitor-service.json`：主站监测、诊断、配置拉取、本地画面配置。
- `runtime/apps/camera-service.json`：摄像头推流配置，出厂默认关闭。
- `runtime/logic/shuntong_ems_graph.json`：舜通 EMS 图形化逻辑模板。
- `runtime/devices/device_slave_ttySP1.json`：`/dev/ttySP1` Modbus RTU 示例。
- `runtime/devices/device_slave_ttySP2.json`：`/dev/ttySP2` Modbus RTU 示例。
- `runtime/devices/device_dio.json`：本机 18 路 DI、8 路 DO 示例。
- `runtime/devices/device_ems_virtual.json`：EMS 本体虚拟点，提供模式、状态、计划曲线和中间变量点位。
- `runtime/devices/device_can0.json`：CAN SocketCAN 示例模板，默认不加入运行时引用。
- `config/examples/device_can0_example.json`：同一 CAN 示例的联调样例文件。

## 默认启动规则

`gateway-services.service` 会根据 app 配置决定启动哪些服务：

- `deviceConfigFiles[]` 引用了 `device_slave_ttySP1.json` 和 `device_slave_ttySP2.json`，因此默认会启动两个 `modbus-rtu@*.service`。
- `deviceConfigFiles[]` 引用了 `device_dio.json`，因此默认会启动 `dio-driver@device_dio.service`。
- `deviceConfigFiles[]` 引用了 `device_ems_virtual.json`，因此 EMS 本体虚拟点会进入共享内存和 MQTT 上报范围。
- `device_can0.json` 只作为模板随包发布，未被 `deviceConfigFiles[]` 引用时不会启动 `can-driver@*.service`。
- `mqtt-service.json` 存在，因此默认会启动 `mqtt-driver@mqtt-service.service`。
- `mqtt-service.json:eventEngine.enabled=true`，因此默认会启动 `event-engine@mqtt-service.service`。
- `mqtt-service.json:computeEngine.enabled=true`，因此默认会启动 `compute-engine@mqtt-service.service`，并执行 `runtime/logic/shuntong_ems_graph.json` 中的 EMS 图形逻辑。
- `monitor-service.json:systemMonitor.enabled=true`，因此默认会启动 `system-monitor@monitor-service.service`。
- `localDisplay.enabled=false`，因此默认不启动本地画面。
- `cameraService.enabled=false`，因此默认不启动摄像头推流。

未被 `deviceConfigFiles[]` 引用的设备 JSON 不会启动；没有某类协议配置时，对应驱动不会运行。

## 初始化参数

如果 `/opt/modbus-gateway/config/runtime/device_identity.json` 已存在，初始化脚本会继承原有 `machineCode`，不会把设备身份重置为出厂模板值。初始化脚本还会把所有 app 配置中的 `mqtt.clientId` 同步为当前 `machineCode`。

交互初始化时会提示填写：

- `machineCode`
- MQTT broker
- MQTT 用户名和密码
- MQTT TLS 开关
- MQTT TLS CA、客户端证书和私钥路径
- MQTT TLS `insecureSkipVerify`

非交互量产可使用 `INIT_MACHINE_CODE`、`INIT_MQTT_BROKER`、`INIT_MQTT_USERNAME`、`INIT_MQTT_PASSWORD`、`INIT_MQTT_TLS_ENABLED` 等环境变量。

## machineCode 和 topic

MQTT 配置中只填写基础 topic。运行时边端会自动追加 `/<machineCode>` 后缀，例如：

- 基础 topic：`edge/telemetry`
- 实际 topic：`edge/telemetry/GW0001`

下行命令、OTA、系统监测、诊断和配置拉取都必须发布到带 `machineCode` 后缀的实际 topic，避免同 broker 下所有网关同时收到请求。

不要在串口或协议设备配置中维护 `machineCode`。运行时代码从 `device_identity.json` 注入网关身份。
