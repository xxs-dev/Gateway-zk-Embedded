# KY-EMS 本地界面说明

## 1. 当前定位

当前生产本地界面以独立 `KY-EMS` Qt 工程为准：

- 源码目录：`D:\workspace\Embedded\KY-EMS`
- 边端运行目录：`/opt/modbus-gateway/ky-ems`
- systemd 服务：`ky-ems.service`
- 编译教程：[192.168.22.11边端交叉编译教程.md](../交叉编译教程/192.168.22.11边端交叉编译教程.md)

边端仓库中的 `LocalDisplayQtEms` 只是早期 POC 和点位浏览方案，不再作为当前生产 EMS 画面的主入口。完整 EMS 画面迁移、打包和初始化包嵌入都以 `KY-EMS` 工程为准。

## 2. 数据绑定

旧 KY-EMS 画面主要通过：

```text
KY-EMS-Config.xml widget + appDataIndex -> QRamRT::GetItemValue(appDataIndex)
```

当前迁移目标是：

```text
meterCode + pointCode -> KY-EMS-PointMap.json -> 当前共享内存 index -> 界面控件
```

兼容顺序：

1. 优先使用 `meterCode + pointCode` 从 `PointMap` 查找当前运行 index。
2. 找不到时使用 `indexFallback`。
3. 再找不到时兼容旧 `appDataIndex`。
4. 嵌入式生产环境仍找不到绑定时显示 `--`，不使用界面中的演示数值。

PC 模拟构建可保留默认值用于界面布局验收，但不得将这些数值带入全志生产二进制。

详细规则见：[KY-EMS点位绑定迁移说明.md](../需求说明/KY-EMS点位绑定迁移说明.md)。

## 3. 运行文件

当前 `ky-ems` 目录应包含：

```text
KY-EMS
KY-EMS-Config.xml
KY-EMS-PointMap.example.json
VarList.xml
dataStoreBases/
```

其中：

- `KY-EMS`：全志 aarch64 Qt 可执行文件。
- `KY-EMS-Config.xml`：界面控件绑定配置。
- `KY-EMS-PointMap.example.json`：当前工程点位映射。
- `VarList.xml`：旧工程兼容入口。

## 4. 当前已验证版本

2026-07-14 已验证：

- 编译机：`192.168.22.11`
- 运行测试机：`192.168.22.16`
- 二进制大小：`8851136` 字节
- SHA256：`560eff373a902749575d32a9c3b37b119e8f9d6e7202e4e6dc555c5f555858cc`

验证结果：

- `ky-ems.service` 可正常启动。
- `VarList.xml` 加载成功。
- `KY-EMS-PointMap.example.json` 加载成功。
- 当前二进制已同步到桌面初始化包资源。
- `192.168.22.16` 上 `ky-ems.service` 和 `qt-display-bridge.service` 均为 `active`。
- 顶部导航、运行监测二级导航、BMS 内部页签、报警/报表切换、参数设置以及首页功率曲线往返均已实机点击验证。
- 嵌入式报警页不再生成伪报警；未接入真实报警源时实时和历史报警均为空表。
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
