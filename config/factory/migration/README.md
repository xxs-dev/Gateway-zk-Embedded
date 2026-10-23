# 出厂策略图迁移包（测点映射）

`gz-ems migrate`（客户端 GatewayClient）由 `runtime/logic/shuntong_ems_graph.json` 生成，**不在 runtime 下，不随出厂配置部署**：

- `shuntong_ems_graph_mapped.json`：迁移后的图（graphCode `shuntong-default-modular-mapped`），只改 index——61 个设备点换成
  52 个 EMS 输入点 680001–680052、12 个只读 EMS 输出点 685001–685012。
- `shuntong_ems_graph_mapped_points.json`：EMS_CORE 点片段（JSON 数组），合并进 `device_ems_virtual.json` 的 EMS_CORE 后图和映射才能用；没有初值。

用到迁移图的设备：客户端把图放进 `runtime/logic/`、点片段合进 EMS_CORE、按这台设备的点表生成 `logic/point-mapping.json`，一次直推
（设备预演核对映射与点表）。没有映射文件的设备继续用原图。新旧图回放对比见 `doc/策略图迁移-测点映射.md`。
