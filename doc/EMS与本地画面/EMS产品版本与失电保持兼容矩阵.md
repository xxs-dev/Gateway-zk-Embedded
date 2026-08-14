# EMS 产品版本与失电保持兼容矩阵

## 1. 版本定义

| 产品版本 | 适用设备 | 当前包版本 | 页面 | 功能边界 |
| --- | --- | --- | --- | --- |
| EMS 1.0 | `COMM202600103` 这类非移动储能柜 | `1.0.17` | 15 个圆形界面页面 | 保留非移动储能柜布局和导航，补充定时功率/SOC 参数失电保持 |
| EMS 2.0 | 移动储能柜、移动储能车 | `2.0.9-compact` | 17 个紧凑页面 | 移动设备当前全部新功能，包括策略、控制、设备、趋势、4G 和登录保护 |

历史 `2.1.0-compact` 至 `2.1.3-compact` 是开发阶段误标，不形成 EMS 2.1 产品线。旧 release 路径保留用于审计和回退，新生成器只允许输出 EMS 1.0 或 EMS 2.0 包。

## 2. 为什么只保留两条产品线

EMS 1.0 和 EMS 2.0 首先按设备产品形态区分：1.0 服务非移动储能柜，2.0 服务移动储能柜或移动储能车。圆形界面与紧凑界面是产品形态带来的交互差异，不能把 1.0 简化成“旧版”、把 2.0 简化成“新版”。两条产品线仍共用底层控制和保持能力；若分别实现策略和保持，会产生以下问题：

- 同一个策略参数出现两份状态文件，重启时无法确定恢复哪一份。
- 1.0 与 2.0 切换后参数丢失或回退。
- Graph EMS、Windows 配置和平台看到的 index 不一致。
- 修复需要重复发布两套 `ComputeEngine`，版本无法收敛。

因此两条产品线必须共用统一运行时，只允许 `.kyscada` 工程不同。

源码按单仓库、单主分支维护，产品资产统一放在 `products/ems`：

```text
products/ems/
  common/                 # 共用构建辅助和运行时边界
  stationary/1.0/         # 非移动储能柜 EMS 1.0
  mobile/2.0/             # 移动储能柜/移动储能车 EMS 2.0
tools/release/             # 统一构建和包校验入口
generated/                 # 本地生成物，不作为源码提交
```

`src/include` 继续作为唯一共用运行时。旧 `tools/generate_storage_ems_2_scada.ps1`、`tools/upgrade_ems_1_scada_retention.ps1` 仅保留兼容包装，真实实现位于对应产品目录。

## 3. 统一运行时

两条产品线共用：

```text
ComputeEngine
  -> Graph EMS V2
  -> device_ems_virtual.json
  -> gateway_point_store_ems_virtual
  -> /opt/modbus-gateway/data/ems-virtual-parameters/<index>.value
```

保持文件采用临时文件、文件 `fsync`、原子 `rename` 和目录同步。启动恢复顺序为：

1. 当前共享内存中质量有效且未过期的值。
2. 对应 index 的有效保持文件。
3. 点位 `initialValue`。
4. 点位 `write.startupValue`。

仅 `protocol.type=ems_virtual` 的点可使用该目录。Modbus、CAN、DL/T 645、IEC 等物理采集点禁止恢复旧值，避免设备离线时把历史数据伪装成实时值。

## 4. EMS 1.0 非移动储能柜范围

非移动储能柜 EMS 1.0 圆形工程中有 48 个可编辑定时参数，并为“当前值”和“修改值”分别创建了 96 个旧 Tag。兼容包将每对旧 Tag 合并成一个可读写 Tag 和一条真实运行路由，避免同一共享内存 index 出现重复路由：

| 原界面参数 | 新点位 | 新 index | 初值 | 保持 |
| --- | --- | --- | --- | --- |
| 0 至 23 时功率 | `ems_schedule_power_0..23` | `400..423` | `0 kW` | 是 |
| 0 至 23 时目标 SOC | `ems_schedule_soc_0..23` | `424..447` | `0%` | 是 |

SOC 写入范围固定为 `0..100`，步长 `0.1`。功率只校验为有限数值，最终额定功率和安全边界继续由 Graph EMS 的容量限制、BMS 能力和统一仲裁执行，避免在画面层固化某一台设备的容量。

“当前值”和“修改值”共用同一个新 index 和同一个 `readWrite` Tag。因此重启后立即显示恢复结果，不再继续读取旧 `226..321` 区域。若其他 EMS 1.0 工程的页面仍引用旧当前值 Tag，转换器只结构化替换该 `tagId`，不修改图元几何、样式或图片。

转换过程必须满足：

- 96 个旧 Tag 必须合并为 48 个 `readWrite` Tag 和 48 条唯一运行路由，少一个即失败。
- 转换后不得再有计划功率/SOC 路由指向旧 `gateway_point_store`。
- 48 条运行路由必须全部指向 `gateway_point_store_ems_virtual`，index 为 `400..447`，且全部可写。
- 运行路由、语义角色和 Tag 标识均不得重复。
- 22.16 圆形基线的 `screens/**` 和 `assets/**` 逐文件 SHA-256 不变；其他工程如存在旧 Tag 引用，只允许绑定字段变化。
- ZIP 条目统一使用 `/`，保证 Windows PowerShell 5.1、PowerShell 7 和 Linux 解包结果一致。
- 包内 `checksums.json` 重新生成并精确覆盖其他文件。
- `manifest.productVersion=1.0`，包版本使用 `1.0.x`。

## 5. EMS 2.0 移动储能柜范围

EMS 2.0 面向移动储能柜和移动储能车，包含当前全部移动端现场页面和功能，不是 EMS 1.0 的通用升级包。其策略使能、运行反馈、参数设置和计划曲线点同样使用 `device_ems_virtual.json` 的初值与保持配置。

当前正式生成参数：

```powershell
$password = Read-Host "请输入本地操作员密码" -AsSecureString
.\tools\release\build-ems-mobile.ps1 `
  -MachineCode COMM202600104 `
  -LocalOperatorPassword $password
```

## 6. EMS 1.0 打包

```powershell
.\tools\release\build-ems-stationary.ps1 `
  -MachineCode COMM202600103 `
  -DisplayName <项目显示名称>
```

对 22.16 圆形基线，工具只更新清单、节点、48 个 Tag 和运行路由，页面文件不变；其他 1.0 工程如仍引用已合并的当前值 Tag，只替换对应绑定。自动测试命令：

```powershell
.\products\ems\stationary\1.0\tests\upgrade-retention.test.ps1
```

## 7. 切换和升级约束

- 生产工程按设备形态选择版本：非移动储能柜使用 EMS 1.0，移动储能柜或移动储能车使用 EMS 2.0，禁止把版本号当作普通的新旧升级关系跨产品下发。
- 仅在兼容性验收或明确回退场景下，才允许在同一测试设备切换 EMS 1.0 与 EMS 2.0；切换时不删除 `ems-virtual-parameters` 目录。
- 首次从旧无保持版本升级时，没有保持文件的点使用 `initialValue` 初始化。
- 切换画面只重启 `ky-ems.service`；更新 `ComputeEngine` 或虚拟点配置时才重启计算服务。
- 回退旧二进制前必须确认其是否理解 `initialValue/retain`；不支持时只能回退画面，不能删除保持文件。
- OTA 和 Windows 工程列表应同时展示产品版本和包版本，禁止再把 `2.1.x` 作为新产品版本发布。

## 8. 22.16 现场验收记录

2026-08-09 在 `192.168.22.16 / COMM202600999` 完成真实运行验收。该设备仅作为兼容性测试机，不改变 EMS 1.0 面向 `COMM202600103` 这类非移动储能柜、EMS 2.0 面向移动储能柜的产品归属：

- EMS 1.0 测试验收包：`generated/artifacts/releases/ky-ems-1.0-retention/ky-ems-COMM202600999-1.0.1.kyscada`；103 正式包必须使用 103 自己的工程和 `COMM202600103` 重新生成。
- EMS 1.0 测试验收包 SHA-256：`5355c0d076e3be78d40c0522b84bf83843f6d2e48ba4e8f5289dea6bae19470e`。
- 现场 EMS 1.0 release：`/opt/modbus-gateway/scada/releases/ky-ems-COMM202600999-1.0.1-retention-fixed2-20260809124000`。
- 包含 15 个原页面、57 个图片资源、48 条唯一保持路由和 48 个可写 Tag；包内校验和全部通过。
- 切换 EMS 1.0 后 `ky-ems.service` 连续运行，`NRestarts=0`，没有语义或路由重复错误。
- 通过真实作用域 topic `edge/command/request/COMM202600999` 将 `index=400` 写为 `1.2`，共享内存和 `400.value` 均立即更新。
- 停止 `KY-EMS、SystemMonitor、EventEngine、MqttDriver、ComputeEngine`，确认共享内存无人占用并删除 `/dev/shm/gateway_point_store_ems_virtual`。
- 按 `ComputeEngine -> MqttDriver -> EventEngine -> SystemMonitor -> KY-EMS` 启动后，`index=400` 从保持文件恢复为 `1.2`，五个服务均为 `active`。
- 验收后通过同一 MQTT 路径将 `index=400` 恢复为 `0`，保持文件同步恢复为 `0`。
- 现场配置共 750 点，其中 `retain=true` 为 179 点、可写点为 69 点；保持目录共 187 个文件。多出的 8 个文件是现场既有的直接可写 EMS 虚拟参数，运行时按原规则保持，不是测试残留。
- 最终画面切回 EMS 2.0 release：`/opt/modbus-gateway/scada/releases/ky-mobile-ems-COMM202600999-2.0.9-compact-20260809121342`，五个服务均为 `active`，`ky-ems.service NRestarts=0`。

`pointctl write` 只向待写队列提交驱动命令，不经过 EMS 虚拟参数的直接落盘路由，不能用于判定失电保持是否成功。保持验收必须走 SCADA 或带 machineCode 的 MQTT 命令 topic。

## 9. COMM202600103 正式部署记录

2026-08-09 已在 `10.126.126.10 / COMM202600103` 部署 EMS 1.0 `1.0.17`：

- 正式 release：`/opt/modbus-gateway/scada/releases/ky-ems-COMM202600103-1.0.17-20260809150207`。
- 正式包 SHA-256：`21ca52eb0ec2ccc99c1e52e05a268fb8b3ae37c0ca8487dca7282349338c09dd`。
- 回滚目录：`/opt/modbus-gateway/backup/ems-stationary-1.0.16-before-1.0.17-20260809`，原 `1.0.16` SCADA release 同时保留。
- 结构化审计通过：15 个页面、57 个图片资源、130 个被页面引用的 Tag、1,968 个运行时点、1,356 个 Tag、1,288 条路由和 48 条保持路由；无重复 Tag、路由或语义角色。
- PCS V126 已删除的 `1261..1265` 不再绑定运行点，画面按预期显示 `--`；无实际设备的 8 个断路器通信图元已删除，真实 DIO 网侧断路器输入保留。
- 通过 MQTT 将 `index=400` 写入 `1.2` 后，共享内存、质量位和保持文件同时更新；重启 `ComputeEngine` 后仍恢复为 `1.2`，验收结束后已通过同一路径恢复为 `0`。
- 五个核心服务延迟检查均为 `active/running`、`NRestarts=0`，部署后日志没有 warning 及以上记录；5 路 Modbus、CAN、DIO 采集进程持续运行。
- 现场 1920x1080 首页截图通过可视核验，页面完整、无空白和布局错位，真实 SOC/电量/状态数据仍有回显。
