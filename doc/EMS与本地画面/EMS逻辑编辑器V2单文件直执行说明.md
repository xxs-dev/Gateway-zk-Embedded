# EMS 逻辑编辑器 V2 单文件直执行说明

## 1. 结论

GraphEms 生产配置只保留一个 `schemaVersion=2.x` 文件。Windows 编辑、平台治理和边端执行面对的是同一份 V2 内容，不再生成或发布外部 V1 运行图，也不再使用 source-map 连接两份逻辑文件。

边端加载 `ComputeEngineService` 中 `script.graphFile` 明确引用的文件。识别依据是文件内容中的 `schemaVersion=2.x`，不是文件后缀：

- 新建策略建议使用 `runtime/logic/<graphCode>.logic.json`。
- 旧项目迁移时，优先在原 `graphFile` 路径原地覆盖为 V2；原文件名即使仍为 `.json` 也可以执行。
- `runtime/logic` 中未被 `graphFile` 引用的历史 V1 文件不参与边端启动校验，不阻断当前 V2 策略。

## 2. V2 到执行计划

边端 `GraphEmsConfig::loadFromFile` 直接解析以下 V2 内容，并在内存中规范化为既有 GraphEms 执行结构：

1. 读取 `node.parameters` 作为节点非连线参数。
2. 读取 `ports`、`binding` 和 `links`，将已物化的 index 或常量写入对应 `runtimePath`。
3. 将 `pointInput` 视为输入别名：它自身不进入执行节点，上游 index 直接折叠到下游输入。
4. 将 `data` 连线转换为数据绑定和执行依赖，将 `dependency` 连线转换为纯执行依赖。
5. 按节点 `order`、节点 ID 和端口 ID 保持确定性加载顺序。

这个规范化过程只存在于边端内存，不输出 V1 文件，不产生第二份可执行真源。

## 3. 可执行绑定约束

生产 `binding.kind` 白名单固定为 `point`、`automatic`、`constant`。`semanticRole` 只是 `point` 绑定的元数据，不是独立 binding kind。Windows 保存最终 V2 文件前必须将 `point` 和 `automatic` 绑定物化为 `port.binding.index`。边端采用失败关闭原则：

- 只有 `pointCode` 或 `semanticRole`、没有 `binding.index` 时拒绝加载，不根据本地点表猜测。
- 常量端口必须使用 `binding.kind=constant` 并携带标量 `constant`。
- 数据连线输入可以从上游输出取得 index；同一个输入最多一条数据连线。
- `automatic` 输出 index 必须位于 `compile.virtualIndexStart` 至 `compile.virtualIndexEnd`；新图的不同执行输出不能重复。
- 新图的设备写入目标 index 不能重复。
- `port.binding.index` 与 `runtimePath` 指向的 `node.parameters` 值同时存在时必须完全相等。
- 数据连线存在时，上游输出 index、下游 `binding.index` 和下游 `parameters` 值必须一致。

任何不一致都会在执行器启动前阻断，避免编辑界面显示的点位与实际执行点位分叉。

旧 V1 图迁移时可以显式设置 `compile.preserveImportedBehavior=true`。该标志只用于保留既有执行语义，并有三项受控例外：允许 profile 互斥分支写入相同 output index、允许既有重复设备 target、允许迁移器识别出的弱类型或单位连线。边端不会修改这些 index，也不会重新编排节点；它保留节点 `order` 和依赖顺序，并写入 `loadWarnings`。重复 output 告警会列出 index、两个节点和各自 order，同时提示 profile 配置错误可能导致同周期覆盖。未设置该标志的新图仍按严格规则拒绝。

## 4. V1 迁移边界

`GraphEmsConfig::loadLegacyV1ForMigration` 只供 Windows 迁移、`EmsParityCheck` 和旧执行行为回归测试使用。生产 `ComputeEngineService` 不调用该入口：

- app 配置中只要仍出现 `script.type=legacyEms`，服务构造阶段就会拒绝启动，并提示在 GatewayDesktop 中迁移为 `schemaVersion=2.x`。
- `script.type=graphEms` 指向 V1 文件时，生产 V2 loader 会在首次加载该规则时明确拒绝，规则不会执行；服务进程保活并持续输出诊断日志，便于现场通过 Windows 完成原路径迁移。
- `LegacyEmsEngine` 仅链接到离线等价校验和回归测试路径，不再由生产 `ComputeEngine` 创建或调度。

推荐迁移顺序：

1. Windows 扫描当前工程中全部被引用和可识别的 V1 图。
2. 将 V1 节点参数转换为 V2 `parameters + ports + bindings + links`。
3. 物化所有运行端口 index，检查参数与绑定一致性。
4. 优先覆盖原 `graphFile`，避免远端遗留路径无法可靠删除。
5. 执行 V1 基线与 V2 候选的等价性测试。
6. 仅发布 V2 文件和更新后的 `computeEngine` 配置。

`EmsParityCheck --baseline-graph <V1> --candidate-graph <V2>` 使用显式 V1 迁移 loader 读取基线，使用生产 V2 loader 读取候选。

## 5. 能力声明

`GET /api/v1/ota/capabilities` 返回：

```json
{
  "supportedPackageTypes": ["config", "full", "scada"],
  "directUpload": false,
  "emsLogic": {
    "editorSourceSchema": "2.x",
    "runtimeSchema": "2.x",
    "compilerContract": "GraphEmsV2/direct",
    "executesEditorSource": true
  }
}
```

`emsLogic.runtimeSchema=2.x` 和 `emsLogic.executesEditorSource=true` 表示边端直接执行被引用的 V2 唯一文件。Windows 与平台必须读取 `emsLogic` 子对象，不能把这些字段当作响应顶层字段。

## 6. 仓库生产源迁移检查

以下 7 个可部署 GraphEms 图已经原路径转换为 V2，旧工程引用不需要修改文件名：

- `config/examples/graph-ems-generic-nodes-example.json`
- `config/examples/graph-ems-sequence-example.json`
- `config/examples/shuntong_ems_graph.json`
- `config/examples/shuntong_ems_modular_graph.json`
- `config/examples/voltage-quality-daily-graph.json`
- `config/examples/comm104-voltage-quality-daily-graph.json`
- `config/factory/runtime/logic/shuntong_ems_graph.json`

配套状态：

- `tools/generate_shuntong_modular_graph.ps1` 直接输出 V2 `parameters/ports/bindings/links`，并为迁移图保留原始重复 output index。
- `tools/testdata/shuntong_ems_generator_seed.json` 只保存生成器所需的旧语义参数，属于明确的迁移/生成输入，不是可部署 GraphEms 图，也不会进入出厂配置目录。
- `config/factory/runtime/apps/mqtt-service.json` 的 `computeEngine.script.graphFile` 继续指向原路径 `runtime/logic/shuntong_ems_graph.json`，该文件内容已经是 V2 唯一文件。
- 项目现场若额外引用电压合格率图，必须将 V2 内容覆盖原 `graphFile` 路径；同一 `ComputeEngine` 实例内不能混用 V1 与 V2。
- 出厂压缩包必须由更新后的 `config/factory` 重新生成，不能只替换仓库散文件。

定向 V2 示例位于 `tools/testdata/graph_ems_v2_direct.logic.json`；它覆盖 `pointInput` 折叠、常量绑定、自动输出和数据连线。

## 7. 自动化与交叉构建验证

正式边端源码基线为 `d12272a96863e7c45cc1ab4615396225a69f875e`。验证使用全志 AArch64 Linaro GCC 6.3.1，不再使用 Ubuntu GCC 9 临时候选：

1. V2 路由审计回归测试 `8/8` 通过。
2. `graph_ems_v2_loader_test`、`graph_ems_voltage_quality_test`、`legacy_ems_test` 和 `system_monitor_direct_maintenance_contract_test` 均完成 AArch64 交叉编译，并通过 `qemu-aarch64-static` 加目标 sysroot 执行。
3. V2 loader 拒绝 V1、未物化绑定、非法 binding、虚拟 index 越界、参数/端口分叉和不一致的数据连线。
4. 回归测试逐一加载 7 个可部署 V2 图；487 节点舜通图保留 22 组重复 output、9 组重复 target 和 8 条弱类型连线，并逐项产生受控迁移告警。
5. `script.type=legacyEms` 在服务构造阶段直接拒绝；`graphEms` 指向 V1 时只拒绝该规则加载，服务保持存活并输出可诊断错误。两条路径不能混为一种启动行为。
6. 正式打包前会审计源码状态和构建目录；当前构建目录不会再被误判为脏源码，但其他未提交源码仍会阻止生成 `sourceDirty=false` 的发布包。

## 8. 可追溯正式出厂包（2026-07-29）

正式 full 包由提交 `d12272a96863e7c45cc1ab4615396225a69f875e` 的精确源码树生成：

- 包大小：`5360815` 字节；SHA-256：`9172423e97c88b06c3884817b7cd6ed7ac61a5c0e9501378c9a63e81dc8c83f7`。
- manifest：`packageProfile=full`、`sourceDirty=false`、`createdAt=2026-07-29T05:46:32Z`。
- 工具链：`aarch64-linux-gnu-g++ (Linaro GCC 6.3-2017.05) 6.3.1 20170404`。
- `ComputeEngine`：`786856` 字节，SHA-256 `fe5a76edf242cc3ff3d9d8ed3a24830b4972bb0ca0b480a78cd7cad2e25f9736`。
- `EmsParityCheck`：`877288` 字节，SHA-256 `2010440f136202342c1165f56734ecbb9598c67ede85b0b53ecb34ac06ce8eb9`。
- `SystemMonitor`：`1116472` 字节，SHA-256 `1fbd40d953791dd01fee9397c12c216a60b0e884be1dad86e7e1c59e84578926`。
- 该包已作为正式初始化资产写入 `GatewayDesktop-Modern/src/GatewayDesktop.UI/Assets/DeviceInit/gateway-factory-defaults.tar.gz`；Windows 提交 `2fb504b` 首次纳入，后续 V2 收尾提交未改动包字节。

manifest 同时记录全部驱动的大小和 SHA-256。验收与部署必须以 manifest 和整包 SHA-256 为准，不能从某个脏工作区的散文件重新拼包。

## 9. 22.16 最终生产复核（2026-07-29）

测试边端为 `COMM202600999 / 192.168.22.16`。最终仅替换正式二进制并修复现场 V2 点位路由，未覆盖现场采集配置、MQTT 参数或控制开关。

### 9.1 配置与路由

- App 配置 SHA-256：`6a405eb827b00b89c996f90b8ec1d8b6a7357c6c26e976c1e03bf4ef6c2dd3f3`；两条启用规则均为 `graphEms` 和 `schemaVersion=2.0.0`。
- 既有虚拟点按语义重映射：`166→800166`、`203→800203`、`209-211→800209-800211`、`1615-1616→801615-801616`，并补齐图实际使用的虚拟点 `217-225`。
- `device_ems_modular_virtual.json` SHA-256：`1687a31998fb7b4788944d533ba6385d334599fe1f6993874d5019ebe33f1eb4`。
- `shuntong_ems_modular_graph.json` SHA-256：`b653360aaafffc8cf28cffd6fa59993cba91c320e4adefe904993140f48d731e`。
- `voltage_quality_daily_graph.json` SHA-256：`957d4b187227f0c6e1fd9463bcdfc7a2fdc0f3f10f6a63e84ee5534a471c57d0`。
- 严格项目路由审计：`1737` 条路由、`1737` 个唯一 Index、2 个启用图、489 个启用节点、380 个当前 profile 活跃节点；虚拟输出、内部输入、活跃外部输入、活跃控制目标和只读目标均无缺口，结果为 `PASS (structural=0, project=0, strictProjectRoutes=true)`。
- 当前停用 profile 仍声明 `1341/4000/4048/4199` 输入和 `1341/4000/4065` 控制目标依赖。它们不影响当前 profile；启用对应策略前必须先补齐项目路由并重新执行严格审计。

### 9.2 运行状态

- 设备上的 `ComputeEngine`、`EmsParityCheck`、`SystemMonitor` 大小和 SHA-256 与正式包完全一致。
- `compute-engine@mqtt-service`、`mqtt-driver@mqtt-service`、`event-engine@mqtt-service`、`system-monitor@monitor-service` 均为 `active/running`，`NRestarts=0`。
- 能力接口返回 `emsLogic.runtimeSchema=2.x`、`compilerContract=GraphEmsV2/direct`、`executesEditorSource=true`。
- ComputeEngine 启动时记录 39 条 `preserveImportedBehavior` 兼容告警；其中 22 组重复 output 和 9 组重复 target 另有 31 条确定性归属诊断。四个服务从最终重启至复核时没有 `error`、`failed`、`exception` 或 `fatal` 日志。
- EMS 虚拟共享内存持续刷新。部分新映射输出当前为空，是因为对应采集源没有有效值，不是图加载或路由失败；接入真实源值后仍需做一次业务值验收。

### 9.3 清理与回滚

- `192.168.22.11` 的 `/tmp/gateway-v2-*`、`/tmp/gateway-build-*` 已清空。
- `192.168.22.16` 的本轮部署目录、审计包、路由修复目录和 7 月 29 日中间备份已清空；`/root` 与 `/dev/shm` 无 V2/legacy 测试残留。
- 仅保留 `/opt/modbus-gateway/backup/ems-v2-20260728-pre-v2` 作为 V2 上线前成套回滚点。回滚时必须同时恢复二进制、主图、虚拟点和电压合格率图，不能形成 V1/V2 混装。
