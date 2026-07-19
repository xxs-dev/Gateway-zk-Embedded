# SCADA 双拓扑边端运行时详细设计

## 1. 目标

边端同时支持一体化 EMS 和上位机两种部署方式，并与 Gateway Desktop 使用同一 `.kyscada` Schema。

边端不负责编辑工程，只负责：

- 验证和加载已编译工程包。
- 将 Tag 解析为当前 PointStore 路由。
- 一体化模式下运行 Qt SCADA 和 EMS。
- 上位机模式下提供实时数据流和控制写回。
- 执行设备安全联锁与离线安全策略。

## 2. 运行模式

| 模式 | SCADA | EMS 高层策略 | 采集与写回 |
| --- | --- | --- | --- |
| `integrated` | 边端 Qt | 边端 | 边端 |
| `upperComputer` | Windows | Windows | 边端 |

无论模式如何，驱动、共享内存、控制校验和安全联锁始终在边端。

## 3. 边端模块

```text
include/edge_gateway/scada_models.hpp
include/edge_gateway/scada_project_loader.hpp
include/edge_gateway/scada_runtime_map.hpp
src/scada_project_loader.cpp
src/scada_runtime_map.cpp
local_display_qt_scada_scene.hpp
local_display_qt_scada_scene.cpp
tools/scada_runtime_test.cpp
tools/scada_install_test.sh
```

Qt 层与核心解析分离：

```text
SCADA Core（无 Qt）
  -> ScadaProjectLoader
  -> ScadaRuntimeResolver
  -> ScadaRuntimeMap
  -> PointStoreRouter
  -> PendingWriteCommand

Qt Runtime
  -> ScadaQtSceneRuntime
  -> QGraphicsScene / QGraphicsItem
  -> 当前页面可见 Tag 集合
```

无 Qt 核心必须能在 x64 主机测试中完整运行，Qt 渲染在交叉编译机和测试边端验收。

## 4. 运行目录

```text
/opt/modbus-gateway/scada/
  current -> releases/<projectId>-<version>-<timestamp>
  releases/
    <projectId>-<version>-<timestamp>/
      manifest.json
      topology.json
      nodes.json
      tags.json
      runtime-map.json
      screens/
      symbols/
      assets/
  backup/
    monitor-service-<timestamp>.json
  status.json
```

安装时先解压到 `releases/.<发布名>.staging`，校验和解压完成后改名为正式 release，再通过 `current.new -> current` 原子切换。健康检查失败时恢复旧链接和 `monitor-service.json` 备份。

## 5. 配置入口

`monitor-service.json` 增加：

```json
{
  "localDisplay": {
    "renderer": "scadaQt",
    "scada": {
      "enabled": true,
      "packageFile": "",
      "projectDirectory": "/opt/modbus-gateway/scada/current",
      "nodeId": "edge-001",
      "autoReload": true
    }
  }
}
```

`install-scada-project.sh` 根据包内 `topology.mode` 和 `manifest.packageRole` 决定 `enabled`。一体化主工程启用 Qt 本地画面；上位机节点子包关闭本地画面，只保留节点运行映射和安全配置。

## 6. Tag 解析

解析顺序：

1. runtime-map 中的 `nodeId + tagId`。
2. 当前项目 `meterCode + pointCode` 路由。
3. 唯一 `semanticRole`。
4. `indexFallback`。

解析结果必须包含：

```text
sharedMemoryName
index
writable
dataType
unit
```

同一语义解析到多个运行点时返回冲突，不允许随机取第一个。

## 7. 数据读取

一体化模式：

- 页面激活时汇总当前页和弹窗使用的 Tag。
- 按 sharedMemoryName 分组，批量调用 PointStoreRouter。
- 只向值、质量或时间变化的图元发送更新。
- 页面隐藏后释放订阅。

上位机模式：

- SystemMonitor 根据订阅租约读取 Tag。
- 直连链路按 nodeId 返回值、质量和时间，不发送业务中文名称。
- MQTT 实时监控只在租约存在时发送；全量 Topic 保持独立周期。

## 8. 控制写回

Qt 本地屏和上位机控制均进入同一边端控制入口：

```text
Tag -> RuntimeMap -> writable 校验 -> 控制权校验 -> 高优先级租约 -> 驱动写队列 -> 回读确认 -> 结果
```

禁止 Qt Runtime 直接修改共享内存值伪造设备响应。

## 9. 离线安全

上位机模式必须配置：

```json
{
  "upperComputerOfflinePolicy": {
    "timeoutMs": 10000,
    "action": "zeroPower",
    "retainLocalSafetyRules": true,
    "requireFreshLeaseForControl": true
  }
}
```

支持动作：`hold`、`zeroPower`、`stop`。消防和急停规则不允许被上位机离线策略关闭。

## 10. 服务编排

- `integrated + scadaQt`：启动 Qt SCADA、ComputeEngine、EventEngine、MqttDriver、SystemMonitor 和项目驱动。
- `upperComputer`：不启动 Qt SCADA；根据项目决定是否启动本地 ComputeEngine，默认只保留安全规则。
- 画面包更新只重启 SCADA 服务。
- runtime-map 或策略变化才重载对应核心服务。
- 驱动配置未变化时不得重启采集驱动。

当前安装脚本在一体化主工程发布时只选择 `ky-ems.service`，若不存在则尝试 `local-display@monitor-service.service`。它不会无条件重启 `gateway-services.service`。

## 11. 兼容

过渡阶段支持从 `.kyscada` 编译输出：

- `KY-EMS-Config.xml`。
- `KY-EMS-PointMap.example.json`。
- `localDisplay.screens/widgets`。

边端加载优先级：

1. `.kyscada` v2。
2. 旧 `KY-EMS-Config.xml + PointMap`。
3. 旧 `appDataIndex + VarList`。

## 12. 测试

- Schema 版本与升级测试。
- ZIP 路径穿越、重复文件和校验和测试。
- Tag 精确解析、语义冲突和 Index fallback 测试。
- 多共享内存批量读取测试。
- 可写点、只读点和高优先级控制测试。
- 上位机断线安全策略测试。
- 1920x1080 Qt 截图对比和 500 Tag 性能测试。
- OTA staging、原子切换和回滚测试。

## 13. 当前实现与边界

截至 2026-07-19 已完成：

- Schema 2.0 工程目录加载和结构检查。
- 节点、Tag、页面、状态规则和 runtime-map 解析。
- 运行地址重复、可写属性不一致、无效比较符等发布前阻断。
- 按当前页面绑定批量读取 PointStore。
- 可写 Tag 通过 `PointStoreRouter` 进入统一待写队列，只读 Tag 被拒绝。
- Qt Scene 读取工程页面，在 `KY-EMS` 中直接刷新共享内存数据。
- `.kyscada` 安装包大小、ZIP 路径、符号链接、必需文件、machineCode、schema 和 SHA-256 校验。
- release staging、`current` 原子切换、局部服务重启和失败回滚。
- MQTT OTA 的 `packageType=scada` 分支和独立 `.kyscada` 文件命名。

仍需后续专项验收：

- 多节点上位机断链后的 `hold/zeroPower/stop` 全链路现场验证。
- 500 个动态 Tag 的 1920x1080 长稳性能与截图差异基准。
- SCADA 告警、历史趋势和权限模型的完整 Qt 运行态交互。

## 14. 2026-07-19 实机验证

- 测试边端：`192.168.22.16 / COMM202600999`。
- 当前 release：`/opt/modbus-gateway/scada/releases/comm202600999-scada-acceptance-1.0.1-acceptance-20260719160309`。
- 新 `MqttDriver`、`SystemMonitor` 和 `KY-EMS` 已部署测试机。
- `mqtt-service.runtimeMode` 已与仓库和 `monitor-service` 一致设为 `ems`；MQTT broker 未修改。
- MQTT 双 TLS 已建立，关键服务 `active`，`NRestarts=0`。
- `scada_runtime_test`、`config_loader_test`、`ota_service_test`、`system_monitor_service_test` 和 `scada_install_test.sh` 通过。
- 冒烟测试：`pass=76 warn=2 fail=0`；warning 为 IMEI 为空和 identity 文件权限提示。
- Index `984` 当前值质量正常，控制队列无遗留命令。

本次没有部署 `10.126.126.*` 生产设备。
