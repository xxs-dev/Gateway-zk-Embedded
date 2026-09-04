# 旧项目双 MQTT 隔离迁移

## 1. 迁移目标

老旧通讯管理机原先由一个 `MqttDriver` 同时向自有平台和旧平台 topic 发布。两路共用进程、连接和配置，第三方 Broker、账号或格式变化可能影响自有平台链路。

> 2026-09-03 现场修正：是否拆分连接必须先比较 Broker。自有平台和兼容 topic 使用同一个 Broker 时，保留一个 `MqttDriver` 持久连接并启用 `mqtt.legacyTelemetryEnabled`，比强行启动第二个连接更稳定；只有 Broker、TLS 或账号不同，才使用独立 `MqttForwarder`。下文“隔离迁移”仅适用于不同 Broker。

迁移后职责固定为：

```text
采集驱动 -> PointStore +-> MqttDriver    -> 自有平台，保留全量、实时、控制、OTA 和配置维护
                       +-> MqttForwarder -> 第三方平台，默认仅周期上行，可单独灰度下控
```

第一阶段迁移必须保持 `mqttForward.control.enabled=false`。此时 `MqttForwarder` 不订阅 topic、不执行控制、不建立离线补传队列。第三方连接异常只更新自身健康文件，不重启采集服务或主 MQTT。

同 Broker 的配置应为：

```json
{
  "mqtt": {
    "legacyTelemetryEnabled": true,
    "legacyTelemetryTopic": "ky/peidian/<machineCode>"
  },
  "mqttForward": {
    "enabled": false
  }
}
```

该模式下新版 snapshot 和 legacy payload 共用主 MQTT 持久连接。不要同时启用同 Broker 的 `MqttForwarder`，否则现场弱网络下第二条 TCP 建连可能反复失败，表现为主平台正常、旧平台长时间无数据。

旧通讯管理机当前经过现场验证的稳定上报节奏为：

- 旧平台兼容 topic：`mqtt.legacyTelemetryIntervalMs=10000`，每 10 秒上报一次。
- 新平台全量 topic：`mqttDriver.fullUploadIntervalMs=1000`，每 1 秒上报一次。

两个周期由 `MqttDriverService` 独立调度，但不能在没有平台容量验证时直接把 legacy 全量报文提升到 1 秒。2026-09-03 对 7 台旧通讯管理机同时测试 `legacy=1s` 时，Broker 仍能收到数据，但旧平台解析和落库链路无法持续消费约 1.69 万点/秒，最终将整批项目判定离线。提高频率前必须先做单机灰度、平台消费积压监测和数据库写入压测，禁止直接全量切换。

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

上述脚本只完成低断链上行迁移，不得顺带开启第三方下控。需要下控的储能项目必须在两路上行验收完成后执行第二阶段，并使用同一批次的共享内存、转发器和协议驱动产物。

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

旧平台的在线状态由 `connect/#`、`disconnect/#` 中的 `clientid` 维护。独立
`MqttForwarder` 承担旧平台链路时，`mqttForward.clientId` 必须与平台注册的
`machineCode` 完全相同，不能追加 `-legacy-forward` 等后缀。验收时应主动重连一次，
确认 Broker 产生的上线事件包含准确的 `clientid`；仅收到遥测数据不能证明在线状态已
正确回写。

兼容 topic 的业务前缀也必须沿用原项目类型，不能统一写成 `ky/peidian`。例如储能项目
使用 `ky/chuneng/<machineCode>`，配电项目使用 `ky/peidian/<machineCode>`。迁移前应从
原 MQTT 配置和平台注册类型交叉确认，不能只按当前设备文件名推断。

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

### 4.1 2026-09-03 v9 驱动滚动替换

9 月 2 日的独立双连接方案在同 Broker 场景暴露出第二条 TCP 连接间歇失败。7 台设备已改为单连接双格式，并使用全志 AArch64 同一批次产物完成共享内存 v8 到 v9 的整组升级。

升级工具：

- `deploy/rolling-upgrade-legacy-runtime.py`：从 Broker 缓存两路最后有效报文，在驱动切换期间按原 topic 保活，逐台执行并在异常时回滚。
- `deploy/upgrade-legacy-runtime-v9.sh`：核对 machineCode、产物哈希、服务集合、共享内存 ABI、有效点恢复比例和 EasyTier 配置哈希；失败自动恢复旧二进制。
- `--reset-realtime-ring`：仅用于旧 ring 阻塞新版重放时。旧文件先保存到本次 `runtime-backups`，失败会自动恢复，不直接删除证据。

最终复检结果：

| machineCode | EasyTier IP | 当前有效点数 | 新 topic 最大间隔 | legacy 最大间隔 |
| --- | --- | ---: | ---: | ---: |
| `COMM202600092` | `10.126.126.14` | 1597 | 1.143 s | 11.036 s |
| `COMM202600093` | `10.126.126.6` | 2002 | 1.140 s | 10.007 s |
| `COMM202600095` | `10.126.126.8` | 1285 | 1.127 s | 10.959 s |
| `COMM202600096` | `10.126.126.2` | 4390 | 1.259 s | 10.694 s |
| `COMM202600097` | `10.126.126.3` | 2097 | 1.265 s | 11.110 s |
| `COMM202600098` | `10.126.126.4` | 2123 | 1.335 s | 10.024 s |
| `COMM202600099` | `10.126.126.5` | 3387 | 1.383 s | 10.425 s |

验收期间 14 个 topic 全部收到可解析 payload，未发现业务服务 failed unit。每台 EasyTier 均保持 `hostname=machineCode`、`dhcp=false` 和固定 `/24` 虚拟 IP；升级脚本不重启 EasyTier。

`COMM202600097`、`COMM202600098` 的旧实时 ring 会阻塞新版 MQTT 重放，确认归档旧 ring 后恢复；`COMM202600099` 同批次直接采用归档迁移。升级期间平台收到的是最后有效值保活，实时值会在新采集首轮完成后恢复，因此“topic 不断”不等于采集时间戳无停顿。

### 4.2 2026-09-04 主 full 工作器隔离

上述 7 台设备及 `COMM202600091` 已启用 `mqttDriver.fullUploadWorker.mode=isolated`。
主平台周期 full 由 `MqttForwarder` 独立 worker 发送，原兼容 topic 的配置和周期不变；
控制、OTA、实时监控及 full 故障兜底仍由 `MqttDriver` 负责。

现场弱网验证表明，DNS 解析和 TCP/TLS 建连不得持有发布锁。修复后
`COMM202600098` 在 Forwarder 持续 DNS 失败时由 Driver 于 1.172 秒接管，且 full
持续约 1.1 秒到达；8 台设备的新 full 和原 `ky/peidian` topic 均从 Broker 实收。

## 5. 可选的第三方控制二阶段

只有合同和现场方案明确要求第三方控制 PCS 时才进入本阶段：

1. 按 [第三方储能控制权接管设计](../架构设计/第三方储能控制权接管设计.md) 确认精确控制 topic、正负功率语义、PCS 可写 index、比例、上下限和 15 秒租约。
2. 先升级同一构建批次的 `MqttForwarder`、共享内存生产者以及 Modbus/DLT645/CAN 消费者；共享内存版本 9 不允许混跑旧驱动。
3. 确认 `mqttForward.control.ownershipFile`、`mqttDriver.powerControlOwnershipFile` 和已启用 AGC/AVC 的 `ownership.leaseFile` 指向同一个文件。
4. 先以 `target=0` 验证接管、回执、设备写回、重复 ID 和主动退出，再用小功率验证充放电。
5. 断开第三方指令，确认租约到期后本地 EMS 自动恢复；重启转发器，确认立即回归本地。
6. 最后验证安全高优先级命令在第三方接管期间仍可执行。

控制开关不得写入通用出厂模板，也不得由老旧项目迁移脚本自动打开。

## 6. 回滚

脚本输出的 `backup=<目录>` 是本次回滚基线。若迁移后发现第三方平台业务解析异常：

```bash
systemctl disable --now mqtt-forwarder@mqtt-service.service
cp -p <backup目录>/mqtt-service.json.before /opt/modbus-gateway/config/runtime/apps/mqtt-service.json
systemctl restart mqtt-driver@mqtt-service.service
```

回滚后再次从 Broker 检查原第三方 topic，不能只根据 systemd 状态判断。
