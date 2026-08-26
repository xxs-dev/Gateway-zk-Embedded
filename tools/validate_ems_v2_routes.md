# EMS V2 图与点表路由闭环验证

## 用途

`validate_ems_v2_routes.py` 从一个 app 配置出发，读取：

1. `deviceConfigFiles[]` 引用的全部设备点表；
2. `computeEngine.enabled=true` 且规则 `enabled=true` 的全部 `graphEms` V2 图；
3. 每条规则的 `script.graphProfile`，按边端 `GraphEmsEngine::shouldRunNode()` 的相同规则判断当前实际执行节点。

验证器把依赖分为三类：

- **EMS 虚拟输出/内部点**：图的所有 enabled 节点输出必须存在唯一、启用的 EMS 虚拟路由。缺失始终阻断。
- **外部输入**：不是由图内节点生成的采集点。当前 Profile 实际使用但缺路由时属于项目配置错误。
- **控制目标**：`target` 端口必须有唯一、启用且 `write.enable=true` 的物理设备路由。不能用 EMS 虚拟点代替。

同时始终阻断：app 引用点表中的重复全局 Index、`binding.index` 与 `parameters/runtimePath` 不一致、数据连线两端 Index 不一致，以及已声明 `pointCode`/`semanticRole` 与路由元数据不一致。

## 命令

在仓库根目录执行普通审计：

```powershell
python tools/validate_ems_v2_routes.py `
  --app config/factory/runtime/apps/mqtt-service.json
```

普通审计会阻断图结构、虚拟路由和全局 Index 错误，但只警告尚未随通用包提供的项目物理点。

部署或恢复现场运行配置后，必须把完整设备目录纳入全局 Index 门禁，避免 app 未引用的物理点与 EMS 虚拟点冲突：

```powershell
python tools/validate_ems_v2_routes.py `
  --app /opt/modbus-gateway/config/runtime/apps/mqtt-service.json `
  --runtime-root /opt/modbus-gateway/config/runtime `
  --all-runtime-devices
```

`--all-runtime-devices` 会扫描 `runtime/devices/*.json`；发现重复 Index 时返回非零，服务不得启动。

项目发布前必须执行严格审计：

```powershell
python tools/validate_ems_v2_routes.py `
  --app config/runtime/apps/mqtt-service.json `
  --runtime-root config/runtime `
  --strict-project-routes
```

CI 可增加 `--json` 获取结构化结果。回归测试：

```powershell
python -m unittest tools/test_validate_ems_v2_routes.py
```

## 当前 factory 结论

修复后，舜通 V2 图的 461 个唯一输出 Index 均有 EMS 虚拟路由；原缺失的 `217..225` 已按既有 `H_TQ_avg_*` 语义补入虚拟点表，其中 `217..220` 同时是图内后续计算的输入。没有把 PCS、BMS、电表或液冷物理点伪造为虚拟点。

当前 `config/factory/runtime/apps/mqtt-service.json` 在 Profile 下实际执行 378 个节点，仍明确依赖：

- 27 个未随当前 app 点表提供的外部采集 Index：`1030,1031,1032,1036..1043,1130,1131,1132,1136..1143,1399,1556,1557,1566,1570`；
- 6 个未随当前 app 点表提供的 PCS 控制目标：`1318..1323`；
- Profile 当前关闭的其他分支还保留 25 个外部输入和 6 个控制目标依赖，启用相应设备或策略前也必须补齐。

因此当前源码中的 EMS app 是**舜通项目模板，不是脱离项目点表即可运行的通用 EMS 配置**。依据如下：

- `config/factory/README.md` 明确通用初始化默认采用 `gateway` 模式；
- `deploy/install-factory-config.sh` 在非 EMS 模式删除 EMS 虚拟点引用和 Graph EMS 规则；
- 当前 EMS app 又显式启用了 `Meter_TQ`、`Meter_CN`、`PCS_MODEL`、`BMS_MODEL`，但 `deviceConfigFiles[]` 只引用两个示例串口点表、DIO 和 EMS 虚拟点表，不能形成舜通项目物理路由闭环。

正确流程是：通用包可以携带图和虚拟点模板，但不能仅因选择 `ems` 模式就把舜通项目图视为可投运；必须先加入项目实际 PCS/BMS/电表等点表、完成语义与 Index 映射，再以 `--strict-project-routes` 通过作为启用条件。当前 app 的启用策略不在本次限定写集内，所以 factory 普通审计通过并保留清晰告警，严格审计仍应失败。
