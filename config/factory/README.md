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
- `runtime/apps/mqtt-service.json:pointHistory.retentionDays`：所有协议驱动共用的点位历史保留期限，出厂默认 30 天，仅作用于 `isStore=true` 的点位。
- `runtime/apps/monitor-service.json`：主站监测、诊断、配置拉取、本地画面配置。
- `runtime/apps/camera-service.json`：摄像头推流配置，出厂默认关闭。
- `runtime/logic/shuntong_ems_graph.json`：舜通 EMS 图形化逻辑模板，当前为 V2 单文件，包含 487 个执行节点、489 条有类型源链接，折叠 `pointInput` 后形成 486 条运行依赖；量产初始化为网关模式时会保留模板文件但不会加入运行规则。
- `runtime/devices/device_slave_ttySP1.json`：`/dev/ttySP1` Modbus RTU 示例。
- `runtime/devices/device_slave_ttySP2.json`：`/dev/ttySP2` Modbus RTU 示例。
- `runtime/devices/device_dio.json`：本机 18 路 DI、8 路 DO 示例。
- `runtime/devices/device_ems_virtual.json`：EMS 本体虚拟点模板，共 622 点，其中 351 个 `700000+` 点仅供模块图内部路由；提供模式、状态、计划曲线、强制满充和中间变量点位。`name`、`pointCode`、`legacyVarName` 分别表示展示名称、平台测点编码和旧舜通实际变量名。量产初始化为网关模式时会从运行目录删除该文件和引用。

舜通模板中的普通 PCS 六路写回默认 `submitWrites=false`。`STATION_LIMIT_V2`、`CHARGE_DISCHARGE_TEST`、`PCS_ENERGY_SAVING`、`LIQUID_COOLING_ENERGY_SAVING` 和 `PCS_AUTO_RESET` 在出厂 app Profile 中均为 `0`。强制满充还必须由虚拟点 `29` 明确使能；仅配置日期 `168/169` 不会启动控制。

该模板由 V1 等价迁移而来，`compile.preserveImportedBehavior=true`。CN/BW 与 schedule/cycle 的 profile 互斥分支保留原 output index；边端加载时会输出重复 index、节点及 order 告警。若 profile 配置错误导致互斥关系失效，同一周期可能发生后执行节点覆盖前值，部署前必须核对 `graphProfile`。
- `runtime/devices/device_can0.json`：CAN SocketCAN 示例模板，默认不加入运行时引用。
- `config/examples/device_can0_example.json`：同一 CAN 示例的联调样例文件。

## 默认运行模式

`deploy/production-init.sh` 和 `deploy/install-factory-config.sh` 支持 `gateway`、`ems`、`agc_avc` 三种互斥运行模式，默认使用 `gateway`。非 EMS 模式会移除 EMS 虚拟点和 Graph EMS 规则；非 AGC/AVC 模式会移除 AGC/AVC 应用、虚拟点、二进制和 service，避免旧文件残留导致误启动。

需要 EMS 项目时必须显式指定：

```sh
sh deploy/production-init.sh --runtime-mode ems
INIT_RUNTIME_MODE=ems sh deploy/install-factory-config.sh
```

需要 AGC/AVC 项目时必须同时指定模式和独立运行模式包：

```sh
sh deploy/production-init.sh \
  --runtime-mode agc_avc \
  --runtime-package /home/gateway-agc-avc-runtime.tar.gz
```

通用 `gateway-factory-defaults.tar.gz` 不包含任何 AGC/AVC 文件；附加包由 `deploy/build-agc-avc-runtime-package.sh` 单独生成。

批量初始化时可在 `deploy/devices.csv` 最后一列填写 `ems`；不填则默认 `gateway`。

## 默认启动规则

以下是生产初始化默认 `gateway` 模式下的服务发现规则。`gateway-services.service` 会根据 app 配置决定启动哪些服务：

- `deviceConfigFiles[]` 引用了 `device_slave_ttySP1.json` 和 `device_slave_ttySP2.json`，因此默认会启动两个 `modbus-rtu@*.service`。
- `deviceConfigFiles[]` 引用了 `device_dio.json`，因此默认会启动 `dio-driver@device_dio.service`。
- `deviceConfigFiles[]` 默认不引用 `device_ems_virtual.json`，因此 EMS 本体虚拟点不会进入共享内存和 MQTT 上报范围。
- `device_can0.json` 只作为模板随包发布，未被 `deviceConfigFiles[]` 引用时不会启动 `can-driver@*.service`。
- `mqtt-service.json:mqtt.enabled=true` 且默认开启 MQTT 上传、事件 outbox、OTA 和控制通道，因此会启动 `mqtt-driver@mqtt-service.service`。
- `mqtt-service.json:pointHistory.retentionDays=30`，所有已启动协议驱动的持久化点位统一保留 30 天。
- `mqtt-service.json:eventEngine.enabled=true`，因此默认会启动 `event-engine@mqtt-service.service`。
- `mqtt-service.json:computeEngine.enabled=true` 时会启动 `compute-engine@mqtt-service.service`；默认 `gateway` 模式不会执行 `runtime/logic/shuntong_ems_graph.json` 中的 EMS 图形逻辑。
- `monitor-service.json:systemMonitor.enabled=true`，因此默认会启动 `system-monitor@monitor-service.service`。
- `localDisplay.enabled=false`，因此默认不启动本地画面。
- `cameraService.enabled=false`，因此默认不启动摄像头推流。

如果 app 的 `mqtt.broker` 改为本机 stunnel 监听地址，例如 `tcp://127.0.0.1:18883`，且 `runtime/tls/*-stunnel.conf` 存在，`gateway-services.service` 会在 MQTT 相关服务前自动启动对应 `mqtt-tls-tunnel@*.service`。默认出厂 broker 不是本机 stunnel 地址，因此不会启动隧道。

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

- 实时基础 topic：`edge/telemetry/realtime`
- 实时实际 topic：`edge/telemetry/realtime/GW0001`
- 全量基础 topic：`edge/telemetry/full`
- 全量实际 topic：`edge/telemetry/full/GW0001`

下行命令、OTA、系统监测、诊断和配置拉取都必须发布到带 `machineCode` 后缀的实际 topic，避免同 broker 下所有网关同时收到请求。

不要在串口或协议设备配置中维护 `machineCode`。运行时代码从 `device_identity.json` 注入网关身份。

## 三端同步要求

`gateway-factory-defaults.tar.gz` 以边端仓库根目录生成的包为准。发布前必须确认以下三份 SHA256 一致：

- 边端：`/Users/song/workspace/Embedded/Gateway-zk/gateway-factory-defaults.tar.gz`
- 桌面初始化内置包：`/Users/song/workspace/CloudPlatform/idea/GatewayDesktop-Modern/src/GatewayDesktop.UI/Assets/DeviceInit/gateway-factory-defaults.tar.gz`
- 平台交付副本：`/Users/song/workspace/CloudPlatform/idea/edge-gateway/config/deploy/gateway-factory-defaults.tar.gz`
