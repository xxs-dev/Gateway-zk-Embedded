# EMS 2.0 移动储能设备

EMS 2.0 当前包版本为 `2.0.9-compact`，用于移动储能柜和移动储能车。当前发布工程包含 17 个紧凑页面、策略执行、PCS 与 DI/DO 控制、设备监测、1 小时趋势、4G 状态和本地登录保护。

历史 `2.1.x` 名称属于 EMS 2.0 的误标发布记录，不建立 2.1 产品线，新包只允许 `2.0.x`。

## 目录

- `scada/base-project`：来自 COMM104 已验收 `2.0.8` 工程的输入基线，提供真实 tags、runtime-map 和 7 页旧画面。当前生成器会以该数据绑定为基础重建 17 页紧凑工程。
- `tools/generate-scada.ps1`：真实产品生成器。
- `projects/COMM202600104`：移动储能车正式项目元数据。
- `projects/COMM202600999`：22.16 兼容性测试项目，禁止当作生产项目。
- `config`：产品配置引用说明，不复制共用策略和保持配置。

`scada/base-project` 不保存历史包中夹带的已编译 `KY-SCADA` ELF；边端程序必须由统一交叉编译流程生成，不能作为画面源码提交。

## 发布

```powershell
$password = Read-Host "本地操作员密码" -AsSecureString
.\tools\release\build-ems-mobile.ps1 `
  -MachineCode COMM202600104 `
  -DisplayName "凯源移动储能车" `
  -LocalOperatorPassword $password
```

构建器会验证 17 个页面、登录保护、UPS 内容清理、文件校验和及路由唯一性。EMS 2.0 包不得下发到 EMS 1.0 非移动储能柜项目。
