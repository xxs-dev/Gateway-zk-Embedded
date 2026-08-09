# EMS 1.0 非移动储能柜

EMS 1.0 当前包版本为 `1.0.17`，用于 `COMM202600103` 这类非移动储能柜。它保留 15 个圆形页面，同时把 24 小时功率和 SOC 的 96 个旧“当前值/设定值”角色合并为 48 个唯一可读写路由，并接入共用失电保持实现。

## 目录

- `scada/base-project`：已验证的 15 页圆形源工程。其清单仍保留测试机 `COMM202600999/1.0.0` 身份，因为这里是不可直接下发的输入基线；构建器会重写目标 machineCode 和包版本。
- `tools/upgrade-retention.ps1`：真实产品转换器。
- `tools/merge-runtime-retention.ps1`：只向现场旧虚拟点表追加 `400..447` 保持点，不重排原有 index。
- `tests/upgrade-retention.test.ps1`：不依赖现场设备的结构化转换回归测试。
- `projects/COMM202600103`：103 项目的产品归属、历史设备配置快照和源工程核验状态。只保留 `device_identity.json` 与 `runtime/devices`，不保存 MQTT 密码、应用配置、证书私钥或工作站绝对路径。
- `config`：产品配置引用说明，不复制共用运行时配置。

## 发布约束

`COMM202600103` 已在 `10.126.126.10` 完成现场工程重拉取、运行时点表核对、SCADA 结构审计和 `1.0.17` 部署。后续构建仍必须使用 103 自己的工程和运行时设备配置，不能把 22.16 的兼容性测试点表当成 103 的设备配置。

```powershell
.\tools\release\build-ems-stationary.ps1 -MachineCode COMM202600103
```

统一构建器会自动生成 `1.0.x` 包并调用 `validate-ems-package.ps1`。EMS 1.0 包不得下发到移动储能柜项目。

当前部署基线：

- release：`/opt/modbus-gateway/scada/releases/ky-ems-COMM202600103-1.0.17-20260809150207`
- 包 SHA-256：`21ca52eb0ec2ccc99c1e52e05a268fb8b3ae37c0ca8487dca7282349338c09dd`
- 回滚目录：`/opt/modbus-gateway/backup/ems-stationary-1.0.16-before-1.0.17-20260809`
- 审计结果：15 个页面、57 个资源、1,968 个运行时点、1,356 个 Tag、1,288 条路由，其中 48 条为新增保持路由；无重复 Tag、路由或语义角色。
- 现场状态：五个核心服务均为 `active/running`、`NRestarts=0`，5 路 Modbus、CAN 和 DIO 采集进程未被部署中断。

现场工程已经移除旧 96 个定时 Tag 时，可传入项目覆盖文件和现场设备配置。转换器会补充 48 条规范保持路由，并只裁剪“运行配置不存在且画面没有引用”的历史路由：

```powershell
.\tools\release\build-ems-stationary.ps1 `
  -MachineCode COMM202600103 `
  -PackageVersion 1.0.17 `
  -SourceProjectDirectory <现场工程目录> `
  -RuntimeDeviceDirectory <现场 runtime/devices 目录> `
  -ProjectOverlay .\products\ems\stationary\1.0\projects\COMM202600103\scada-overlay.json
```
