# SCADA 工程部署与验收

## 1. 适用范围

本文用于边端安装 GatewayDesktop-Modern 编译的 `.kyscada` 工程。SCADA 工程只更新画面、Tag 运行映射及相关工程定义，不应无条件重启采集驱动。

## 2. 包要求

包内至少包含：

```text
manifest.json
topology.json
nodes.json
tags.json
runtime-map.json
checksums.json
screens/
```

约束：

- `schemaVersion` 必须为 `2.0`。
- `projectId` 和 `packageVersion` 只能使用字母、数字、点、下划线和连字符。
- 当前 machineCode 在 `nodes.json` 中必须且只能出现一次。
- `checksums.json` 必须精确覆盖包内其他文件。
- 压缩前和解压后的总大小均不能超过 512MiB。
- 禁止绝对路径、`..`、反斜杠路径、重复文件和符号链接。
- 上位机节点包必须保留本地安全规则。

## 3. 手工检查与安装

只校验，不落盘：

```sh
/opt/modbus-gateway/bin/install-scada-project.sh \
  --package /tmp/project.kyscada \
  --machine-code COMM202600999 \
  --dry-run
```

安装并重载本地画面：

```sh
/opt/modbus-gateway/bin/install-scada-project.sh \
  --package /tmp/project.kyscada \
  --machine-code COMM202600999 \
  --restart
```

正常输出先出现 `SCADA validation passed`，最后出现 `SCADA release activated`。

## 4. OTA 安装

Windows OTA 请求必须使用：

```json
{
  "packageType": "scada",
  "version": "1.0.0",
  "sha256": "<package sha256>",
  "size": 8119
}
```

边端下载文件保存为 `<version>.kyscada`，`ota-apply.sh` 识别扩展名后调用 `install-scada-project.sh`。普通 `full/config` OTA 仍沿用原有版本号和配置扩展名，不能被 SCADA 文件名规则影响。

## 5. 落盘与回滚

成功安装后的结构：

```text
/opt/modbus-gateway/scada/
  current -> releases/<projectId>-<version>-<timestamp>
  releases/<projectId>-<version>-<timestamp>/
  backup/monitor-service-<timestamp>.json
```

安装脚本先写 staging，完成后再原子切换 `current`。如果配置更新、服务重启或 PID 健康检查失败，会恢复旧链接、旧 `monitor-service.json` 并删除失败 release。

一体化主工程只重启 `ky-ems.service`；上位机节点包不启动 Qt 本地画面。驱动配置没有变化时不重启协议驱动。

## 6. 验收命令

```sh
readlink -f /opt/modbus-gateway/scada/current
python3 -m json.tool /opt/modbus-gateway/config/runtime/apps/monitor-service.json
systemctl is-active ky-ems.service
systemctl show ky-ems.service -p MainPID -p NRestarts
/opt/modbus-gateway/bin/pointctl get --index 984 \
  --app-config /opt/modbus-gateway/config/runtime/apps/mqtt-service.json
/opt/modbus-gateway/bin/pointctl pending-peek \
  --app-config /opt/modbus-gateway/config/runtime/apps/mqtt-service.json
MQTT_CONNECT_TEST=1 /opt/modbus-gateway/bin/gateway-run.sh smoke
```

通过标准：

- `current` 指向本次正式 release，不指向 `.staging`。
- `monitor-service.json` 的 `projectDirectory` 指向 `/opt/modbus-gateway/scada/current`。
- 一体化模式下 `ky-ems.service=active`，主进程 PID 稳定，`NRestarts=0`。
- 测试点质量正常，控制队列无遗留命令。
- MQTT 双 TLS 正常，冒烟测试 `fail=0`。

## 7. 清理规则

验收结束后可删除旧 release 和过期配置备份，但必须先确认 `current` 的真实目标：

```sh
readlink -f /opt/modbus-gateway/scada/current
```

禁止删除当前 release。不要在服务运行时清理共享内存，也不要用清理 SCADA release 的方式处理采集故障。

## 8. 2026-07-19 验证记录

- 设备：`192.168.22.16 / COMM202600999`。
- 当前 release：`comm202600999-scada-acceptance-1.0.1-acceptance-20260719160309`。
- `MqttDriver`：716,000 bytes，SHA-256 `708c4687f469412ee1d3a6d7f99cf448771093d544d2d2db6f27aec83810b103`。
- `SystemMonitor`：881,456 bytes，SHA-256 `a3e20f76175212fa859d798dd69ea5683e78d416f182c6f97047e099287c8e8f`。
- `KY-EMS`：685,912 bytes，SHA-256 `cf3d4fa7337078876a6e72da88977bdd2208edc2be706f597be32bbd497f2642`。
- MQTT broker 保持 `ssl://kygate.kyxn.net:8883`，双 TLS 连接正常。
- 冒烟结果 `pass=76 warn=2 fail=0`，关键服务 `active` 且 `NRestarts=0`。
- 只验证测试机，没有部署 `10.126.126.*` 生产设备。
