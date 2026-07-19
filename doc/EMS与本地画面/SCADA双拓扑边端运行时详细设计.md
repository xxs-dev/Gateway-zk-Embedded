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
src/scada_project_loader.cpp
src/scada_tag_resolver.cpp
src/scada_runtime_map.cpp
src/scada_control_policy.cpp
tools/scada_project_test.cpp
```

Qt 层与核心解析分离：

```text
SCADA Core（无 Qt）
  -> ScadaProjectLoader
  -> ScadaTagResolver
  -> PointStoreRouter
  -> ScadaCommandDispatcher

Qt Runtime
  -> ScadaSceneView
  -> ScadaWidgetItem
  -> VisibleTagSubscription
```

无 Qt 核心必须能在 x64 主机测试中完整运行，Qt 渲染在交叉编译机和测试边端验收。

## 4. 运行目录

```text
/opt/modbus-gateway/scada/
  active -> versions/1.0.0
  versions/
    1.0.0/
      manifest.json
      topology.json
      nodes.json
      tags.json
      runtime-map.json
      screens/
      symbols/
      assets/
  staging/
  backup/
  status.json
```

升级通过 `active` 符号链接原子切换。健康检查失败时恢复旧链接。

## 5. 配置入口

`monitor-service.json` 增加：

```json
{
  "localDisplay": {
    "renderer": "scadaQt",
    "scada": {
      "enabled": true,
      "packageFile": "/opt/modbus-gateway/scada/active/project.kyscada",
      "projectDir": "/opt/modbus-gateway/scada/active",
      "runtimeMode": "integrated",
      "entryScreen": "overview",
      "refreshIntervalMs": 200,
      "maxVisibleTags": 2000
    }
  }
}
```

上位机模式保留同一配置段，但 `runtimeMode=upperComputer` 且不启动 Qt 本地画面。

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
