# 旧项目双 MQTT 隔离迁移

## 1. 迁移目标

老旧通讯管理机原先由一个 `MqttDriver` 同时向自有平台和旧平台 topic 发布。两路共用进程、连接和配置，第三方 Broker、账号或格式变化可能影响自有平台链路。

迁移后职责固定为：

```text
采集驱动 -> PointStore +-> MqttDriver    -> 自有平台，保留全量、实时、控制、OTA 和配置维护
                       +-> MqttForwarder -> 第三方平台，仅周期上行，Broker 和格式独立配置
```

`MqttForwarder` 不订阅 topic、不执行控制、不建立离线补传队列。第三方连接异常只更新自身健康文件，不重启采集服务或主 MQTT。

## 2. 低断链迁移顺序

迁移脚本 `deploy/migrate-legacy-mqtt-forwarder.sh` 必须在设备本机执行。运行前把与当前边端版本匹配的 `MqttForwarder` 和 systemd 单元放到 `/tmp`：

```bash
chmod +x /tmp/MqttForwarder /tmp/migrate-legacy-mqtt-forwarder.sh
/tmp/migrate-legacy-mqtt-forwarder.sh
```

脚本按以下顺序处理：

1. 备份原 `mqtt-service.json`、转发器二进制和 systemd 单元。
2. 从原主 MQTT 配置继承第三方 Broker、账号、TLS、旧 topic、周期和点位映射。
3. 根据 `legacyTelemetryMappedOnly` 选择映射点集或主全量点集。
4. 保持主 MQTT 继续发布旧 topic，同时启动独立转发器。
5. 等待 `/opt/modbus-gateway/run/mqtt-forwarder-health.json` 返回 `healthy=true` 且 `valueCount>0`。
6. 健康检查通过后才关闭主 MQTT 内的 `legacyTelemetryEnabled`，并只重启主 MQTT 服务。
7. 任一步失败时恢复原 app 配置并停用转发器；采集驱动全程不重启。

迁移要求原 `legacyTelemetryTopic` 以 `/<machineCode>` 结尾。脚本会把前半段保存为第三方基础 topic，由运行时统一补上 machineCode。

## 3. 验收方法

迁移成功不能只检查进程。至少确认：

```bash
systemctl is-active mqtt-driver@mqtt-service.service
systemctl is-active mqtt-forwarder@mqtt-service.service
cat /opt/modbus-gateway/run/mqtt-forwarder-health.json
```

还必须从 Broker 实际接收以下两类消息：

- 自有平台：`edge/telemetry/full/<machineCode>`，应为新版 snapshot/分片结构。
- 第三方平台：原兼容 topic，应为 `data[].meterid + metrics[] + msgid + split + timestamp` 结构。

第三方兼容包需要对比迁移前后的 meterCode/pointCode 集合。只有 topic 可达但结构不同，仍视为迁移失败。

## 4. 2026-09-02 现场结果

下列设备已完成独立双 MQTT 迁移，并从 Broker 收到两路实际报文：

| machineCode | EasyTier IP | 第三方有效读取点数 | 主 MQTT 重启耗时 |
| --- | --- | ---: | ---: |
| `COMM202600092` | `10.126.126.14` | 1597 | 196 ms |
| `COMM202600093` | `10.126.126.6` | 1344 | 322 ms |
| `COMM202600095` | `10.126.126.8` | 903 | 189 ms |
| `COMM202600096` | `10.126.126.2` | 3898 | 321 ms |
| `COMM202600097` | `10.126.126.3` | 1776 | 275 ms |
| `COMM202600098` | `10.126.126.4` | 1764 | 335 ms |
| `COMM202600099` | `10.126.126.5` | 2908 | 448 ms |

`COMM202600092` 迁移前后第三方 payload 均为 122 个 meter、1597 个测点，业务键集合完全一致。7 台设备的自有平台和第三方平台 topic 最终验收结果为 `14/14` 全部到达。

MQTT 迁移脚本本身不会修改 EasyTier、实体网口或 DHCP 配置。现场复核时发现 `COMM202600092` 的 EasyTier 仍为动态地址，已另行修正为 `dhcp=false`、`ipv4=10.126.126.14/24`、`hostname=COMM202600092`，重连后 SSH 和两路 MQTT 均自动恢复。其余 6 台原本已经使用固定 `/24` 地址。EasyTier 虚拟 IP 和 P2P/relay 状态属于独立的网络验收项，不能仅凭 MQTT 迁移成功推断网络配置正确。

## 5. 回滚

脚本输出的 `backup=<目录>` 是本次回滚基线。若迁移后发现第三方平台业务解析异常：

```bash
systemctl disable --now mqtt-forwarder@mqtt-service.service
cp -p <backup目录>/mqtt-service.json.before /opt/modbus-gateway/config/runtime/apps/mqtt-service.json
systemctl restart mqtt-driver@mqtt-service.service
```

回滚后再次从 Broker 检查原第三方 topic，不能只根据 systemd 状态判断。
