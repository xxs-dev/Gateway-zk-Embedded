# Config 目录说明

`config/` 已按用途拆成两类：

## 1. 运行时依赖配置

目录：

- [runtime/apps](D:/workspace/Embedded/Gateway-zk/config/runtime/apps)
- [runtime/devices](D:/workspace/Embedded/Gateway-zk/config/runtime/devices)

说明：

- `runtime/devices` 放 `ModbusRtu` 直接加载的设备采集配置
- `runtime/devices` 放协议驱动直接加载的设备采集配置
- `runtime/apps` 放 `MqttDriver` 或应用级配置
- 这些文件会被程序实际读取，修改后会影响运行结果

## 2. 报文和日志样例

目录：

- [samples/messages](D:/workspace/Embedded/Gateway-zk/config/samples/messages)
- [samples/logs](D:/workspace/Embedded/Gateway-zk/config/samples/logs)

说明：

- `samples/messages` 放 MQTT 请求、回执、状态、告警等 JSON 样例
- `samples/logs` 放 OTA 历史日志样例
- 这些文件主要用于联调、文档和测试，不作为运行时必需配置

## 常用启动路径

单串口多从站：

```bash
./ModbusRtu --config config/runtime/devices/device_slave_ttySP1.json
./MqttDriver --app-config config/runtime/apps/mqtt-service.json
```

多串口：

```bash
./ModbusRtu --config config/runtime/devices/device_slave_ttySP1.json
./ModbusRtu --config config/runtime/devices/device_slave_ttySP2.json
./MqttDriver --app-config config/runtime/apps/mqtt-service.json
```

DLT645-2007 单串口多表：

```bash
./Dlt645Driver --config config/runtime/devices/device_dlt645_multi_meter_1_2.json --mock
./MqttDriver --app-config config/runtime/apps/mqtt-service.json
```
