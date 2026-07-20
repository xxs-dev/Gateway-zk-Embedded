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

## 8. 2026-07-20 验证记录

- 设备：`192.168.22.16 / COMM202600999`。
- 当前 release：`comm202600999-scada-acceptance-1.0.1-acceptance-20260719160309`。
- `MqttDriver`：725,872 bytes，SHA-256 `cbd6ba571c8622f1008c9780ce3cd79ad3b17648e13db310524b786688c87ae5`。
- `SystemMonitor`：1,115,936 bytes，SHA-256 `3b0911500b54116367f0edc3942069fce286aabf8fbc21deca6913cbf8194a56`。
- `KY-EMS`：685,912 bytes，SHA-256 `cf3d4fa7337078876a6e72da88977bdd2208edc2be706f597be32bbd497f2642`。
- MQTT broker 保持 `ssl://kygate.kyxn.net:8883`，双 TLS 连接正常。
- 最终主动 MQTT 冒烟结果 `pass=77 warn=1 fail=0`，关键服务 `active` 且 `NRestarts=0`。
- 唯一 warning 为测试机 IMEI 为空；mqtt、monitor、identity 三个 JSON 权限已收紧为 `640`。
- 直连实时快照返回 1,001 点，并生成 machineCode 匹配的上位机租约文件。
- 边端 22 个 C++ 测试、`scada_install_test.sh` 和 `scada_rollback_test.sh` 全部通过。
- 安全链 ARM64 测试和真实断链测试均通过。测试使用可写 DIO Index `984`，安全目标值与当前值同为 `1`，避免改变现场输出。
- 控制队列序号为 `7 -> 8 -> 8 -> 9`：首次断链动作只执行一次，同一失联周期不重复；旧租约控制被拒绝且未写队列，新鲜租约控制写入成功并通过回读校验。
- 断链动作持有 `scada-offline-safety` 高优先级租约；新鲜租约控制设备写入 51ms、总耗时 554ms。
- 60 秒观察前后 SystemMonitor、MqttDriver、KY-EMS 的 PID 不变；SystemMonitor 与 MqttDriver 均存在到 broker `:8883` 的已建立连接。
- 测试结束后已恢复原一体化 release，并清理测试工程、控制租约、临时报文和远端备份；Index `984` 保持 `value=1 quality=1`。
- 只验证测试机，没有部署 `10.126.126.*` 生产设备。
