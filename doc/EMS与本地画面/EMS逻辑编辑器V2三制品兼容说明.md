# EMS 逻辑编辑器 V2 三制品兼容说明

## 1. 结论

EMS 逻辑编辑器 V2 采用“Windows 编辑和编译、边端执行”的边界：

- Windows 维护 `schemaVersion=2.x` 类型化源图。
- Windows 将源图确定性编译为 `schemaVersion=1.x` GraphEms 运行图。
- 边端只加载运行图，不解析 V2 源图和 source-map。
- 平台保存并校验三制品，但不在服务端重新编译或改写策略。

这样可以继续复用现有 `GraphEmsEngine`，避免把编辑器布局、端口类型和点位选择逻辑带入生产执行器。

## 2. 三制品路径

每张 V2 策略图必须成套发布：

| 制品 | 目标路径 | 用途 |
| --- | --- | --- |
| V2 源图 | `runtime/logic/design/<graph>.logic.json` | Windows 后续编辑的唯一真源 |
| GraphEms v1 运行图 | `runtime/logic/<graph>.json` | 边端实际加载和执行 |
| source-map | `runtime/logic/design/<graph>.source-map.json` | 记录编译器、端口折叠关系和双向 SHA-256 |

source-map 中的 `sourceTarget`、`runtimeTarget`、`graphCode`、`sourceSha256` 和 `runtimeSha256` 必须与同一配置快照中的两个制品完全一致。平台在上传完整快照后统一校验，缺件或摘要不一致时拒绝发布。

## 3. 边端能力声明

`GET /api/v1/ota/capabilities` 的 `emsLogic` 字段声明：

```json
{
  "editorSourceSchema": "2.x",
  "runtimeSchema": "1.x",
  "compilerContract": "GatewayDesktop.EmsLogicCompilerV2/1.x",
  "executesEditorSource": false
}
```

`executesEditorSource=false` 是生产约束，不是能力缺失。它保证即使 V2 源图被随配置包保存到设备，也不会绕过编译结果直接执行。

## 4. 兼容规则

- 旧项目只有 GraphEms v1 运行图时继续加载和执行。
- 旧运行图导入 Windows 后，可生成 V2 源图和 source-map，再按三制品发布。
- V2 源图中的 `pointInput` 是编辑器别名，编译时折叠为真实共享内存 index，不进入运行图。
- V2 源图不得直接配置为 `ComputeEngineService.script.graphFile`。
- OTA 或配置发布必须把运行图放在 `runtime/logic` 根目录，不能只下发 `design` 目录。

## 5. 验证要求

生产前至少完成以下检查：

1. Windows 编译器校验类型、单位、环路、只读写入和重复写入目标。
2. 平台校验三制品路径、`graphCode` 和 SHA-256 一致。
3. 边端使用 `GraphEmsConfig::loadFromFile` 成功加载编译后的 v1 运行图。
4. 仅在测试设备下发运行图并运行影子或受限策略，确认无未知节点、悬空边和循环依赖。
5. 验证完成后再替换生产图，不把 V2 源图加入边端执行配置。
