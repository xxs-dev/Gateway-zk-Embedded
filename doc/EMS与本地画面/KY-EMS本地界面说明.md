# KY-EMS 本地界面说明

## 1. 当前定位

当前生产本地界面使用边端仓库内的统一 Qt SCADA 运行时：

- 源码入口：`local_display_qt_ems_main.cpp`、`local_display_qt_scada_scene.cpp`
- 边端运行目录：`/opt/modbus-gateway/ky-ems`
- systemd 服务：`ky-ems.service`
- 工程目录：`/opt/modbus-gateway/scada/current`
- 编译教程：[192.168.22.11边端交叉编译教程.md](../交叉编译教程/192.168.22.11边端交叉编译教程.md)

二进制文件名和服务名继续使用 `KY-EMS`/`ky-ems.service` 以兼容既有部署脚本，但页面、Tag、资源和跳转的唯一运行输入已经改为 `.kyscada`。旧独立 Qt 工程只作为迁移源，不再作为生产编辑源。

产品版本统一为两条稳定线：

- **EMS 1.0**：用于 `COMM202600103` 这类非移动储能柜，采用 15 个页面的圆形本地界面。
- **EMS 2.0**：用于移动储能柜和移动储能车，包含紧凑画面、策略执行、控制、4G、登录保护等当前移动端功能。

两者是设备产品线，不是简单的新旧版本关系。生产发布必须先确认设备类型，再选择对应 EMS 产品版本。

两条产品线在同一仓库、同一主分支维护。画面和产品工程放在 `products/ems/stationary/1.0`、`products/ems/mobile/2.0`，共用运行时仍在根目录 `src/include`。统一发布入口位于 `tools/release`，`generated` 只保存本机构建结果。

`2.1.0-compact` 至 `2.1.3-compact` 是开发期间误用的包名，不代表第三条产品线，功能归属统一按 EMS 2.0 处理。新包不得继续生成 `2.1.x` 标识。

## 2. 数据绑定

旧 KY-EMS 画面主要通过：

```text
KY-EMS-Config.xml widget + appDataIndex -> QRamRT::GetItemValue(appDataIndex)
```

当前生产链路是：

```text
.kyscada Tag -> runtime-map -> PointStoreRouter -> 当前共享内存 index -> Qt SCADA 图元
```

兼容顺序：

1. 优先使用 `meterCode + pointCode` 从 `PointMap` 查找当前运行 index。
2. 找不到时使用 `indexFallback`。
3. 再找不到时兼容旧 `appDataIndex`。
4. 嵌入式生产环境仍找不到绑定时显示 `--`，不使用界面中的演示数值。

PC 模拟构建可保留默认值用于界面布局验收，但不得将这些数值带入全志生产二进制。

详细规则见：[KY-EMS点位绑定迁移说明.md](../需求说明/KY-EMS点位绑定迁移说明.md)。

### 2.1 多状态灯和说明文字

`KY-EMS-Config.xml` 的 `<StateRules>` 可让一个状态组件同时更新指示灯和说明文字：

- 同一点的不同枚举值可分别显示停机、待机、运行、故障等状态。
- 不同点可按 `all/any` 组合，例如任一故障点为 `1` 时优先显示红色故障。
- 每个状态独立配置 `label/color/image/priority`。
- 无匹配或条件点不可解析时使用灰色未知态。
- 状态条件通过 `EmsPointProvider` 直接读取共享内存，不依赖 MQTT。

## 3. 运行文件

当前运行文件包括：

```text
/opt/modbus-gateway/ky-ems/KY-EMS
/opt/modbus-gateway/scada/current/manifest.json
/opt/modbus-gateway/scada/current/screens/*.json
/opt/modbus-gateway/scada/current/tags.json
/opt/modbus-gateway/scada/current/runtime-map.json
/opt/modbus-gateway/scada/current/assets/**
```

`KY-EMS-Config.xml`、PointMap 和 VarList 仍作为 Windows 转换器的旧工程输入，不是已启用 SCADA 模式时的运行必需文件。

## 4. EMS 2.0 当前版本

2026-08-09 已将当前功能统一生成到 EMS 2.0 产品线：

- 工程版本：`2.0.9-compact`，共 17 个 `1920x1080` 页面。
- 22.16 历史候选包：`generated/artifacts/releases/storage-ems-2.0-compact/storage-ems-COMM202600999-2.0.9-compact.kyscada`；新包统一由 `tools/release/build-ems-mobile.ps1` 输出。
- 当前二进制：`/opt/modbus-gateway/ky-ems/KY-EMS`。
- 二进制 SHA-256：`5e611b5f1e5a41ca439494accc0f69a731fd2e39b8a2a8f8aa7b7673649147f1`。
- 候选包 SHA-256：`9205c94b95fc6da153f53851967403d6d1772a44f6c60301cf401a1b97da2297`。
- 趋势页默认只展示最近 1 小时，首页减少低优先级信息，适配 10.1 寸现场屏。
- 总览和运维页增加 4G 模块、连接、出口、信号、累计流量和实时速率显示。
- 未注入 mock 数据；采集点无有效值时显示 `--` 或“数据不可用”。
- 现场旧 `2.1.x` release 暂时保留作回退，但统一视为 EMS 2.0 历史误标包。

同日已将 4G 监测版本同步到 `10.126.126.9 / COMM202600104`：

- 以设备原 `2.0.8` 工程的 1,896 个 Tag/运行映射为源生成，未复用 22.16 的设备点表。
- 新工程为 1,905 个 Tag/运行映射、17 个页面，只新增 9 个 `SYSTEM_CELLULAR` 系统点。
- 当前历史 release：`/opt/modbus-gateway/scada/releases/ky-mobile-ems-COMM202600104-2.1.2-compact-20260807194107`，该名称属于 EMS 2.0 的历史误标。
- 实机 4G 已连接并作为当前出口，信号、累计流量和实时上下行速率均取得有效值。

### 4.1 EMS 1.0 非移动储能柜失电保持

非移动储能柜 EMS 1.0 不复制独立保持服务，而是和移动储能柜 EMS 2.0 共用 `ComputeEngine`、`device_ems_virtual.json` 和保持目录：

```text
/opt/modbus-gateway/data/ems-virtual-parameters/<index>.value
```

- 当前正式版本：`1.0.17`，`productVersion=1.0`。
- 103 正式 release：`/opt/modbus-gateway/scada/releases/ky-ems-COMM202600103-1.0.17-20260809150207`；后续包必须继续使用 103 自己的工程和运行时点表生成。
- 22.16 测试验收包：`generated/artifacts/releases/ky-ems-1.0-retention/ky-ems-COMM202600999-1.0.1.kyscada`，仅用于兼容性验收。
- 测试验收包 SHA-256：`5355c0d076e3be78d40c0522b84bf83843f6d2e48ba4e8f5289dea6bae19470e`。
- 原 15 个圆形页面和 57 个图片资源逐文件哈希保持不变。
- 0 至 23 时功率设定映射到 `ems_schedule_power_0..23`，index 为 `400..423`。
- 0 至 23 时 SOC 设定映射到 `ems_schedule_soc_0..23`，index 为 `424..447`，输入范围为 `0..100`。
- 48 个设定值和 48 个当前值旧角色按时段和类型合并为 48 个 `readWrite` Tag，分别对应 48 条唯一保持路由，不生成重复共享内存 index。
- 48 个底层参数全部为 `initialValue=0`、`retain=true`。
- 策略使能、运行反馈及其他 EMS 虚拟参数继续使用同一套初值和保持规则；1.0 未展示的点不会因此增加新控件。
- 恢复优先级为：有效共享内存值、保持文件、`initialValue`、`startupValue`。
- 物理 Modbus、CAN、DL/T 645、IEC 点不允许从 EMS 保持文件恢复。
- 22.16 已完成真实 MQTT 写入、停止五个服务、删除共享内存和按生产顺序重启的验收，保持值成功恢复；详细记录见 [EMS 产品版本与失电保持兼容矩阵](EMS产品版本与失电保持兼容矩阵.md)。
- `10.126.126.10 / COMM202600103` 已完成 `1.0.17` 正式部署、运行时点表审计、真实 MQTT 保持测试和 1920x1080 页面截图核验。

### 4.2 本地登录保护

EMS 2.0 将页面分成公开区和受保护区：

| 页面 | 未登录状态 |
| --- | --- |
| 总览、设备、告警、趋势、运维 | 可直接查看 |
| `Strategy-*` 策略执行页 | 点击后先登录，登录成功才创建和显示页面 |
| `Control-*` PCS、DI/DO 操作页 | 点击后先登录，登录成功才创建和显示页面 |

登录规则：

- 账号和密码可直接使用屏幕软键盘输入，支持大小写、数字、`!`、`#`、`-`、`_`、`.`、`@`、退格、清空和密码显隐。
- 当前验收账号为 `operator`；明文密码不写入代码、工程和文档，由项目交付记录单独保存。
- `permissions.json` 只保存 16 字节随机盐和 `SHA-256(salt + ":" + password)`，不保存明文密码。
- 默认无操作超时为 900 秒。鼠标、触摸、键盘和滚轮操作会续期；超时后立即退出并返回总览。
- 受保护页右上角显示“当前账号 · 退出”，现场可随时主动退出。
- 未登录时不实例化策略和控制页，不能通过先加载再遮罩的方式绕过认证。
- 工程校验会拒绝位于公开页的写控件、控制动作和 PCS 三相功率控制器。

生成工程时必须显式传入 `SecureString` 密码：

```powershell
$password = Read-Host "请输入本地操作员密码" -AsSecureString
.\tools\release\build-ems-mobile.ps1 `
  -MachineCode COMM202600104 `
  -LocalOperatorUsername operator `
  -LocalOperatorPassword $password
```

重新生成会产生新的随机盐。不要手工复用其他项目的 `passwordSha256`，也不要把明文密码写进脚本参数、Git 或初始化包说明。

## 5. EMS 2.0 页面路由

### 5.1 顶部主导航

| 按钮 | 目标 | 路由约束 |
| --- | --- | --- |
| 总览 | `Overview` | 公开，显示核心运行状态、能量流、设备健康和活动设备告警 |
| 策略 | `Strategy-Overview` | 受保护，登录后显示执行总览，可切换充放电、电网友好、三相与无功 |
| 设备 | `Devices-Pcs` | 公开，可切换 PCS、BMS、温控和消防 |
| 告警 | `Alarms` | 公开，只展示设备与安全告警，不把通讯延迟混入告警 |
| 趋势 | `Trends-Power` | 公开，可切换功率、电池、电网和温控，默认最近 1 小时 |
| 控制 | `Control-Pcs` | 受保护，可切换 PCS 三相功率和 DI/DO 控制 |
| 运维 | `Maintenance` | 公开，只提供运行状态和诊断信息，不放置设备写控件 |

### 5.2 子页面

| 一级页 | 子页面 |
| --- | --- |
| 策略 | 执行总览、充放电、电网友好、三相与无功 |
| 设备 | PCS、BMS、温控、消防 |
| 趋势 | 功率、电池、电网、温控 |
| 控制 | PCS 控制、DI/DO 控制 |

### 5.3 路由与控制的边界

- 页面跳转按钮只负责切换页面、更新高亮和同步账号显示。
- PCS 启停、三相功率、DI/DO 等按钮是设备控制，不得被接成页面跳转。
- 设备控制必须在点位可写、账号有权限且用户确认后进入写队列；未完成真实写回绑定的按钮必须保持不可用，不能用界面跳转作为占位行为。
- “校验并确认”只进行参数校验和确认；确认弹窗通过后才进入统一边端写队列，页面不得直接修改共享内存伪造结果。

### 5.4 4G 状态与流量

EMS 2.0 在两个位置显示 4G 状态：

- 总览“设备健康”区域使用第六个状态位显示 4G 信号，便于值守人员快速判断联网状态。
- 运维页显示模块状态、网络连接、当前出口、信号条、累计上传/下载流量和实时上传/下载速率。

信号条按真实百分比显示：低于 `25%` 为红色，`25%` 至 `49%` 为黄色，`50%` 及以上为绿色。模块缺失、未连接或信号无效时显示灰色和 `--`，不显示虚假的 `0%`。

画面直接读取 `gateway_point_store_system_monitor` 中 `920000001..920000009`，不依赖 MQTT 是否在线。累计流量是当前蜂窝接口的内核累计值，不是月度套餐或计费流量；接口重建、驱动重载或设备重启后可能清零。

## 6. 部署检查

查看服务：

```sh
systemctl status ky-ems.service --no-pager
journalctl -u ky-ems.service -n 100 --no-pager
```

替换前建议备份：

```sh
mkdir -p /opt/modbus-gateway/backup/ky-ems
cp -a /opt/modbus-gateway/ky-ems/KY-EMS \
  /opt/modbus-gateway/backup/ky-ems/KY-EMS.$(date +%Y%m%d%H%M%S)
```

替换后重启：

```sh
systemctl restart ky-ems.service
systemctl status ky-ems.service --no-pager
```
