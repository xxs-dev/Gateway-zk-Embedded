# EMS 产品目录

本目录用于并行维护两条 EMS 产品线。两者共用边端采集、共享内存、策略执行、失电保持和本地画面运行时，但画面、产品配置、工程基线和发布包不能混用。

| 产品目录 | 当前包版本 | 设备形态 | 页面 |
|---|---|---|---:|
| `stationary/1.0` | `1.0.1` | 非移动储能柜 | 15 个圆形页面 |
| `mobile/2.0` | `2.0.9-compact` | 移动储能柜、移动储能车 | 17 个紧凑页面 |

历史 `2.1.x` 包是 EMS 2.0 的误标版本，不建立 EMS 2.1 产品目录。

## 目录边界

- `common`：只放产品线共同使用的构建辅助代码和边界说明。
- `stationary/1.0`、`mobile/2.0`：只放产品元数据、SCADA 基线、产品配置说明、设备工程差异、产品生成器和产品测试。
- 仓库根目录 `src`、`include`、`config/factory`：继续作为共用边端运行时和出厂配置的唯一实现。
- `generated`：只存本机构建结果，不是源码，已由 `.gitignore` 忽略。
- `tools/release`：提供统一发布入口和包校验器。

禁止复制两套 `src/include`，也禁止用长期 Git 分支分别维护 EMS 1.0 和 EMS 2.0。共享能力修改后必须同时跑两个产品的打包校验。

## 发布入口

```powershell
# EMS 1.0，正式构建前必须确认源工程确实属于目标非移动储能柜
.\tools\release\build-ems-stationary.ps1 -MachineCode COMM202600103

# EMS 2.0，密码通过 SecureString 传入，不写入脚本或仓库
$password = Read-Host "本地操作员密码" -AsSecureString
.\tools\release\build-ems-mobile.ps1 `
  -MachineCode COMM202600104 `
  -LocalOperatorPassword $password
```

默认输出位于 `generated/artifacts/releases/ems-stationary` 或 `generated/artifacts/releases/ems-mobile`。构建完成后会自动校验产品版本、machineCode、页面数、文件校验和、共享内存路由和语义角色唯一性。
