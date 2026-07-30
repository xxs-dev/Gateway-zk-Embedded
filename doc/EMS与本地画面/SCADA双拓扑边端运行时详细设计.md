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
- 直连实时快照和 MQTT 实时订阅都会原子更新 `/opt/modbus-gateway/run/scada-upper-computer-lease.json`。

## 8. 控制写回

Qt 本地屏和上位机控制均进入同一边端控制入口：

```text
Tag -> RuntimeMap -> writable 校验 -> 控制权校验 -> 高优先级租约 -> 驱动写队列 -> 回读确认 -> 结果
```

禁止 Qt Runtime 直接修改共享内存值伪造设备响应。

Windows 上位机控制必须使用 `scada-windows:` 来源前缀。直连和 MQTT 控制入口只对该来源检查 SCADA 租约；普通维护、边端 EMS 和 AGC/AVC 继续使用各自现有控制边界。控制入口失败后由调用方返回结果，不允许自动换通道重发。

## 9. 离线安全

上位机模式必须配置：

```json
{
  "upperComputerOfflinePolicy": {
    "timeoutMs": 10000,
    "action": "executeConfiguredActions",
    "retainLocalSafetyRules": true,
    "requireFreshLeaseForControl": true,
    "safetyActions": [
      {
        "actionId": "pcs-active-power-zero",
        "nodeId": "edge-a",
        "tagId": "pcs-active-power-setpoint",
        "value": 0,
        "highPriority": true
      }
    ]
  }
}
```

每个动作必须明确配置 `actionId + nodeId + tagId + value`，目标 Tag 和运行映射都必须可写。边端不根据 PCS、功率语义或设备类型猜测控制点。节点子包只携带本节点动作；消防和急停规则不允许被上位机离线策略关闭。

SystemMonitor 加载新工程后从当前时间开始计算超时，避免安装瞬间误动作。心跳超时后，动作通过现有共享内存写回队列提交，默认使用高优先级。同一失联周期中，已经被队列接受的动作不重复提交；收到新心跳或工程版本变化后才开始新的周期。

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
- 上位机租约来源隔离、同周期动作去重和节点动作裁剪测试。
- 1920x1080 Qt 截图对比和 500 Tag 性能测试。
- OTA staging、原子切换和回滚测试。

## 13. 当前实现与边界

截至 2026-07-20 已完成：

- Schema 2.0 工程目录加载和结构检查。
- 节点、Tag、页面、状态规则和 runtime-map 解析。
- 运行地址重复、可写属性不一致、无效比较符等发布前阻断。
- 按当前页面绑定批量读取 PointStore。
- 可写 Tag 通过 `PointStoreRouter` 进入统一待写队列，只读 Tag 被拒绝。
- Qt Scene 读取工程页面，在 `KY-EMS` 兼容服务中直接刷新共享内存数据；已支持 Qt 原始像素字体、对齐、横纵进度、状态图片、告警表和多序列实时曲线。
- `.kyscada` 安装包大小、ZIP 路径、符号链接、必需文件、machineCode、schema 和 SHA-256 校验。
- release staging、`current` 原子切换、局部服务重启和失败回滚。
- MQTT OTA 的 `packageType=scada` 分支和独立 `.kyscada` 文件命名。
- SystemMonitor 上位机心跳监测、明确安全动作提交和同一失联周期去重。
- 直连实时快照与 MQTT 实时订阅续租；两个控制入口统一检查 `scada-windows:` 新鲜租约。
- `MqttDriver` 使用轻量租约解析，不链接完整 SCADA 工程解析器。

仍需后续专项验收：

- 多节点上位机 72 小时长稳、同时断链与分批恢复的现场验证。
- 500 个动态 Tag 的 1920x1080 长稳性能与截图差异基准。
- 长期历史趋势查询和更细粒度的运行时权限交互仍需专项验收；活动告警表、空告警状态和实时多序列曲线已经进入 Qt 运行态。

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

## 15. 2026-07-20 上位机安全链验证

- `MqttDriver`：725,872 bytes，SHA-256 `cbd6ba571c8622f1008c9780ce3cd79ad3b17648e13db310524b786688c87ae5`。
- `SystemMonitor`：1,115,936 bytes，SHA-256 `3b0911500b54116367f0edc3942069fce286aabf8fbc21deca6913cbf8194a56`。
- 出厂包：5,198,688 bytes，SHA-256 `8a532369a13615e392d466d9badb6db314be70a0eab9071cbb4e84d2b03f19fc`；包内二进制与仓库产物哈希一致，不包含废弃的独立维护代理或 AGC/AVC 独立运行模式。远程维护走 MQTT，本地网口维护复用 `SystemMonitor` 内嵌接口；打包脚本会拒绝废弃组件重新进入归档。
- 直连实时快照返回 1,001 点，并成功更新 machineCode 为 `COMM202600999` 的租约文件。
- 60 秒观察前后 SystemMonitor、MqttDriver、KY-EMS 的 PID 不变，三者 `NRestarts=0`。
- SystemMonitor 与 MqttDriver 均有到 broker `:8883` 的已建立连接；broker 仍为 `ssl://kygate.kyxn.net:8883`。
- 最终主动 MQTT 冒烟结果 `pass=77 warn=1 fail=0`，唯一 warning 为测试机 IMEI 为空；mqtt、monitor、identity 三个配置文件权限已收紧为 `640`。
- 边端 22 个 C++ 测试、ARM64 安全链测试、SCADA 安装与回滚脚本测试全部通过。
- 使用可写 DIO Index `984` 完成真实断链测试，安全目标值取当前值 `1`，因此不会改变现场输出。控制队列序号变化为 `7 -> 8 -> 8 -> 9`：断链安全动作只入队一次，同一失联周期不重复入队，恢复新鲜租约后人工控制再入队一次。
- 安全动作持有 `scada-offline-safety` 高优先级租约，租约记录包含本次 `cmdId`、表计和 Index；旧租约控制返回 HTTP 400 且未写队列，新鲜租约控制返回 HTTP 200，设备写入 51ms、回读校验通过、总耗时 554ms。
- 测试完成后恢复原一体化 SCADA release，Index `984` 保持 `value=1 quality=1`，控制租约、测试工程、临时报文和远端备份均已清理；没有部署 `10.126.126.*`。

## 16. 2026-07-20 旧 EMS 全量 SCADA 验证

- 完整工程：15 页、1,595 图元、1,158 Tag、57 资源，5,477,976 bytes，SHA-256 `e42f93c6a48e3c737fcf7ca2380ca5c6ed4f83c79d674efd94e2482b5cfbfe08`。其中 `qtLabel` 706 个且不绑定运行点位，`qtValue` 220 个负责单点值和值映射。
- Qt SCADA 运行时：724,208 bytes，SHA-256 `6551b55585afd3476f95d43ed6d571cb9f63e48a09b513deb105eac2ccf6f35a`；枚举映射在初始化时解析一次，刷新时按数值匹配，未知值回退为原始数值。
- 22.16 使用 X11 事件自动遍历 15 个页面，页面截图均为 1920x1080、非空且哈希不同。
- 当前主设备点没有有效采集值，因此数值和曲线保持空值；未注入 mock。旧画面中的数字和状态占位已改为 `--`，固定容量等静态文字继续保留。DIO Index `984` 保持 `value=1 quality=1`。
- `ky-ems.service` 为 `active`、`NRestarts=0`，broker 保持 `ssl://kygate.kyxn.net:8883`。
- 配置、SCADA 运行时、上位机安全、OTA、SystemMonitor、安装和回滚测试均通过；未部署 `10.126.126.*`。

## 17. SCADA 独立拉取范围（2026-07-25）

为了让通用 Windows 运行器只同步画面工程，SystemMonitor 在原配置拉取协议上增加 `scope`，不新增服务或端口：

- `scope=config`：保持现有配置快照行为。
- `scope=scada`：只遍历 `localDisplay.scada.projectDirectory` 指向的当前 SCADA release。

允许文件为固定元数据 `manifest.json`、`topology.json`、`nodes.json`、`tags.json`、`runtime-map.json`、`symbols.json`、`alarms.json`、`trends.json`、`permissions.json`、`checksums.json`，以及 `screens/**`、`assets/**`。实现复用现有 MQTT 分片、缺片补发、直连 HTTP、大小限制和安全路径检查，不建立第二套传输协议。

22.16 实测候选二进制先使用关闭 MQTT 和直连端口的临时配置执行 `--once`，再经 `ldd` 预检和自动回滚脚本原子替换。新服务稳定后，直连 `scope=scada` 在约 0.9 秒内返回 82 个文件、9,115,329 bytes；Windows 能重建 checksums 完整的 `.kyscada`，第二次相同内容命中缓存。生产采集驱动、MqttDriver 和 KY-EMS 未因本次替换重启。
