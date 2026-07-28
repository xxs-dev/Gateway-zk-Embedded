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
  "editorSourceSchema": "2.x",
  "runtimeSchema": "2.x",
  "compilerContract": "GraphEmsV2/direct",
  "executesEditorSource": true
}
```

`runtimeSchema=2.x` 和 `executesEditorSource=true` 表示边端直接执行被引用的 V2 唯一文件。

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

## 7. 生产验证

1. V2 loader 拒绝 V1、未物化语义绑定、虚拟 index 越界和参数/绑定不一致。
2. V2 fixture 直接执行并产生预期共享内存输出。
3. 旧 GraphEms 节点执行回归通过显式迁移 loader 保持覆盖。
4. 严格图拒绝重复 executable output；迁移图保留重复 output、节点 order 和依赖边，并产生带覆盖风险的告警。
5. 回归测试逐一加载仓库内全部 7 个可部署 GraphEms 图，防止示例或项目图重新退回 V1。
6. 487 节点舜通工厂图由生产 loader 直接加载，确认 22 组重复 output、9 组重复 target 和 8 条弱类型连线均只作为迁移告警。
7. 生产 `ComputeEngineService` 构造测试确认 `legacyEms` 配置被明确拒绝，旧引擎仅由离线工具和测试使用。
8. 当前候选使用隔离的 Ubuntu 20.04 / GCC 9.4 ARM64 工具链构建，动态依赖上限为 `GLIBC_2.29`、`GLIBCXX_3.4.26`；已在 22.16 实机加载运行。正式交叉机恢复后仍应按同一源码重编并复跑本节测试。
9. 测试设备先完成 V1/V2 等价性和受限控制验证，再替换生产策略。

## 8. 22.16 生产程序替换验证（2026-07-28）

测试机 `COMM202600999 / 192.168.22.16` 已将 `ComputeEngine`、主 EMS 图和低压电压合格率图作为一个回滚单元完成替换：

- `ComputeEngine` SHA-256：`6b01a3f595031d53d1de2f8d0e209e490627f8271796fed5d65b6b75a48cd11a`。
- `EmsParityCheck` SHA-256：`90d56bef51c3c4bafbbcbfedc11e01f9826d4c348ab61f0caadd4389f80a25da`。
- 主 V2 图 SHA-256：`ee9e2c276a14adeda29f4c8260a182a84951efda05c1db378f5defdbacdab7ad`，487 个编辑节点、489 条连线（227 条数据连线、262 条依赖连线）。
- 电压合格率 V2 图 SHA-256：`957d4b187227f0c6e1fd9463bcdfc7a2fdc0f3f10f6a63e84ee5534a471c57d0`。
- Windows 与边端内置初始化包 SHA-256：`293114704049b7cd36252e342b2fd2af321a674e3774a4c102d863080e48b0de`；该包从提交前 HEAD 基线精确重建，仅替换 EMS V2 图、虚拟点、`ComputeEngine`/`EmsParityCheck` 并删除 legacy 示例，未混入其他驱动改动。包内 `ComputeEngine` 与本节实机候选哈希一致，7 份可部署策略图均为 V2。
- ARM64 上 `graph_ems_v2_loader_test`、`graph_ems_voltage_quality_test` 和 `legacy_ems_test` 全部通过。
- 使用替换前 V1 主图和现场 887 个共享内存样本执行 10 轮隔离等价计算，8 个实际可比较关键输出全部一致，4 个输出在两边均未生成，差异数为 0。
- 服务启动时产生 39 条受控迁移告警：22 组重复 output、9 组重复 target 和 8 条弱类型连线；除此之外没有 `error`、`failed`、异常退出或重启。
- `compute-engine@mqtt-service.service` 为 `active/running`，`MainPID=17066`、`NRestarts=0`、`MemoryCurrent=21839872` 字节。部署未修改应用配置、MQTT broker 或控制开关。
- 当前 app 的两个启用规则均为 `graphEms`，实际引用的主图和电压合格率图均为 `schemaVersion=2.0.0`，不存在 V1 `params/edges`。
- 验证结束后已检查 `/tmp`、`/dev/shm` 和 `/root`，没有遗留 `legacy_ems_test` 测试文件或共享内存对象。

成套回滚备份位于 `/opt/modbus-gateway/backup/ems-v2-20260728-pre-v2`。回滚时必须同时恢复二进制、主图和电压合格率图，不能形成 V1/V2 混装。
