# EMS 共用运行时

EMS 1.0 和 EMS 2.0 共用以下实现，不在产品目录复制源码：

- `src`、`include/edge_gateway`：共享内存、点位路由、策略图、SCADA 运行时和 SystemMonitor。
- `config/factory/runtime/devices/device_ems_virtual.json`：EMS 虚拟点初值、可写属性和失电保持定义。
- `config/factory/runtime/logic/shuntong_ems_graph.json`：当前模块化 EMS 图配置。
- `/opt/modbus-gateway/data/ems-virtual-parameters`：设备侧保持数据目录。

产品目录只决定“使用哪套画面、哪个产品版本、从哪个工程基线生成”，不另建保持服务或第二套共享内存实现。

`powershell/Repository.ps1` 是产品脚本共用的仓库根目录定位器。它通过 `CMakeLists.txt` 和 `include/edge_gateway` 识别根目录，脚本迁移后不再依赖固定的父目录层数。
