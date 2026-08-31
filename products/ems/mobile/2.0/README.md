# EMS 2.0 移动储能设备

EMS 2.0 当前包版本为 `2.0.10-xmind-v2`，用于移动储能柜和移动储能车。当前发布工程依据《移动储能画面 V2》XMind 重建为 34 个页面，包含引导、仪表、拓扑、车辆、曲线、信息、维护、策略执行、PCS 与 DI/DO 控制、4G 状态和本地登录保护。

历史 `2.1.x` 名称属于 EMS 2.0 的误标发布记录，不建立 2.1 产品线，新包只允许 `2.0.x`。

## 目录

- `scada/base-project`：来自 COMM104 已验收工程的输入基线，提供真实 tags、runtime-map 和旧画面。当前生成器以真实数据绑定为基础重建 34 页 XMind V2 工程。
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

构建器会验证 34 个页面、登录保护、UPS 内容清理、文件校验和及路由唯一性。引导、车辆、策略和控制页面要求本地登录；所有曲线默认显示最近 1 小时。EMS 2.0 包不得下发到 EMS 1.0 非移动储能柜项目。
