# KY-EMS 本地界面说明

## 1. 当前定位

当前生产本地界面使用边端仓库内的统一 Qt SCADA 运行时：

- 源码入口：`local_display_qt_ems_main.cpp`、`local_display_qt_scada_scene.cpp`
- 边端运行目录：`/opt/modbus-gateway/ky-ems`
- systemd 服务：`ky-ems.service`
- 工程目录：`/opt/modbus-gateway/scada/current`
- 编译教程：[192.168.22.11边端交叉编译教程.md](../交叉编译教程/192.168.22.11边端交叉编译教程.md)

二进制文件名和服务名继续使用 `KY-EMS`/`ky-ems.service` 以兼容既有部署脚本，但页面、Tag、资源和跳转的唯一运行输入已经改为 `.kyscada`。旧独立 Qt 工程只作为迁移源，不再作为生产编辑源。

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

## 4. 当前已验证版本

2026-07-20 已验证：

- 编译机：`192.168.22.11`
- 运行测试机：`192.168.22.16`
- 二进制大小：`724208` 字节
- SHA256：`6551b55585afd3476f95d43ed6d571cb9f63e48a09b513deb105eac2ccf6f35a`

验证结果：

- `ky-ems.service` 可正常启动。
- 完整 15 页 `.kyscada` 工程加载成功，包含 1,595 个图元、1,158 个 Tag 和 57 个资源。
- 通用工程包大小 5,477,976 bytes，SHA-256 `e42f93c6a48e3c737fcf7ca2380ca5c6ed4f83c79d674efd94e2482b5cfbfe08`；固定文本和单点运行值已分别使用 `qtLabel`、`qtValue`，PCS 状态点支持中文值映射。
- 当前二进制已同步到桌面初始化包资源。
- `192.168.22.16` 上 `ky-ems.service` 为 `active`、`NRestarts=0`。
- 多状态规则已在 `192.168.22.16` 用真实共享内存读取链路验证，日志确认 `CtlModeColor -> TEST_OK / 状态规则验证 / #2F9D78`；测试后恢复原项目 XML，仅保留已验证二进制。
- 顶部导航、运行监测二级导航、BMS 内部页签、报警/报表切换、参数设置以及首页功率曲线往返均已实机点击验证。
- 告警页不生成伪报警；未接入活动告警时显示“暂无活动告警”。
- 数据报表和实时曲线保留 16 条曲线绑定；对应点没有真实值时只显示空网格，不填充演示数据。
- 修复 `qt-display-bridge.service` 与 `gateway-services.service` 的启动顺序环；完整网关服务重启实测耗时 `18.35s`。
- 重建损坏的 `gateway_point_store_ems_virtual` 后，Compute、Event、MQTT、Qt Bridge 和 KY-EMS 顺序启动，跨越原超时窗口后重启计数均为 `0`。

## 5. 按钮路由矩阵

### 5.1 顶部主导航

| 按钮 | 目标 | 路由约束 |
| --- | --- | --- |
| 首页 | 系统首页 | 隐藏所有子页，恢复首页完整图层和首页高亮 |
| 运行监测 | 系统概览 | 默认选中“系统概览”二级页 |
| 故障预警 | 实时报警 | 默认选中“实时报警”，可再切换历史报警 |
| 数据报表 | 数据曲线 | 默认展示曲线，可切换数据报表 |
| 参数设置 | 运行模式 | 未授权时先弹出登录；仅二级管理员和超级管理员可进入 |

### 5.2 运行监测二级导航

| 按钮 | 目标页 |
| --- | --- |
| 系统概览 | 运行监测总览 |
| PCS 监测 | PCS 运行与遥测页 |
| BMS 监测 | BMS 基础信息与报警页 |
| 动环消防 | 空调、环境与消防页 |
| UPS 监测 | UPS 监测页 |
| IO 监测 | DI/DO 状态页 |

BMS 页内“BMS 运行”和“基础信息与报警”必须双向切换。切换后顶部仍高亮“运行监测”，二级导航仍高亮“BMS 监测”。

### 5.3 参数设置和首页快捷路由

- 参数设置的三个分类为“运行模式”、“运行参数”和“设备参数”。
- “运行参数”与“充放电参数”必须双向切换，返回后不得丢失当前账号和导航高亮。
- 首页“实时功率”图表按钮进入功率曲线页，曲线页“返回”只返回首页，不触发参数保存或设备控制。

### 5.4 路由与控制的边界

- 页面跳转按钮只负责切换页面、更新高亮和同步账号显示。
- PCS 运行/待机/停止/复位、UPS 输出、空调与消防等按钮是设备控制，不得被接成页面跳转。
- 设备控制必须在点位可写、账号有权限且用户确认后进入写队列；未完成真实写回绑定的按钮必须保持不可用，不能用界面跳转作为占位行为。

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
