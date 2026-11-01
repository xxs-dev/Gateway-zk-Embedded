# 控制确认链路与 EMS 2.0 画面审查（2026-11-01）

基线 `realese1.0` = `eeaaa8a`。本轮在本地分支 `r6/control-ems2` 按功能分 9 个提交（见第 7 节），**未推送、未部署**。所有验证均为模拟验证（隔离测试数据、测试替身），不接设备、不发控制命令；不是现场验收。画面逐页清单见 [EMS 2.0 画面逐页审查](../../products/ems/mobile/2.0/doc/screen-audit-20261101.md)。

## 1. 回执语义（协议不变）

回执字段 `success`、`outcome`、`stage`、`verifyAttempted/verifyPassed`、共享内存标志位与 MQTT/直连维护接口**均未改变**。新增的只是一个共用分类 `include/edge_gateway/control_receipt_view.hpp`，供本地画面使用：

| 回执 | 本地画面显示 | 是否算确认 |
| --- | --- | --- |
| 尚无回执（等待内） | 命令已受理，等待驱动回执 | 否 |
| `writeback-submitted` | 已下发，未确认：仅交给输出或通信，设备是否动作未确认 | 否 |
| `writeback-succeeded`，`verifyPassed=false` | 协议写入成功，动作未确认 | 否 |
| `verifyPassed=true` | 反馈确认成功（确认范围以回执中的反馈来源为准，回执原文一并显示） | 是 |
| `local-parameter-committed` / `mailbox-committed` | 本地参数已提交（未下发设备） | — |
| outcome=unknown（`writeback-timeout`/`writeback-mismatch`） | 结果未知：设备可能已执行；未自动重发（不一致时说明反馈与目标不一致） | 否，也不是失败 |
| 确定失败（`writeback-failed`、`control-rejected` 等） | 执行失败 + 阶段 + 实际原因 | — |
| `writeback-unconfirmed`（账本重放，无结果） | 视为未结束，继续等待 | 否 |
| 等待 10 秒仍无回执 | 等待回执超时，结果未知；继续跟踪到 70 秒，期间晚到的最终回执标为“晚到回执”替换显示 | 否 |

规则：只认本条命令（cmdId + 点号）的回执；最终结果出现后不再被任何较旧或矛盾的记录替换；一条命令无最终回执期间，该点不能再下发第二条本地命令（提示先核实现场）；任何情况都不自动重发。

## 2. 修改的消费路径

| 路径 | 修改前 | 修改后 |
| --- | --- | --- |
| 本地画面按钮（`local_display_qt_scada_scene.cpp` → 新 `local_display_qt_control_receipt.cpp`） | `success=false` 一律显示“驱动执行失败”（含结果未知）；成功不区分“仅提交/写入/反馈确认”；10 秒后不再接收晚到回执；不核对回执是否属于本命令 | 按上表分类；核对 cmdId+点号；晚到回执可替换超时显示；未结束命令锁住该点 |
| 本地画面参数输入（同上） | 提交后只提示“已提交”，不跟踪回执 | 与按钮共用同一跟踪 |
| PCS 分相功率（`local_display_qt_pcs_power_control.cpp`） | 每相只显示“成功/失败”，取到第一条回执后不再刷新；汇总按 success | 每相按分类显示；未结束的相继续查询直到最终回执或 70 秒；汇总区分全部反馈确认、写入未确认、结果未知、失败；未结束期间锁定该三相点 |
| Windows 预览桥接（`scada_windows_renderer_main.cpp`） | 第一条主机回执即弹出待查队列，晚到的最终回执被丢弃；忽略 outcome 字段 | 读取 `outcome/outcomeUnknown/feedbackMismatch/isFinal`；非最终回执不出队；最终回执不被替换；缺少 outcome 字段时按旧版规则（只有未下发的阶段才算确定失败） |
| 图形 EMS `confirmedControlWrite`（`confirmed_control.hpp`、`graph_ems_engine.cpp`） | 只接受“写后回读”的目标，CAN/DO 无法使用；所有未确认回执都记 -1（失败）；账本重放的未结束回执也被当成最终结果 | 见第 3 节 |
| AGC/AVC（`agc_avc_service.cpp`） | 结果未知（含无回执超时）按失败处理并清除已下发值，之后会**以新 cmdId 重发同一设定值** | 结果未知仍计入连续失败次数，但保留已下发值，不重发同一值；配置了 PCS 遥测反馈时由遥测判定；只有安全态 0（不可用 PCS 的尽力下发）仍会重发；账本重放的未结束回执在超时内继续等待 |
| MQTT 回执、直连维护、第三方转发、pointctl | 已原样转发 stage/outcome/verifyPassed，并有非最终/最终回执与同 cmdId 查询 | 未改 |

## 3. confirmedControlWrite 的目标规则

| 目标协议 | 必要配置 | 节点回读 | 确认依据 |
| --- | --- | --- | --- |
| Modbus/DL645/IEC 等 | `write.verifyAfterWrite` 且 `verifyByRead`（原规则不变） | 目标点自身 | 回执 `verifyPassed=true` |
| 本机 DO（`local_dio`） | `write.verifyIndex`（辅助触点 DI，不能是输出本身；`verifyValue` 只能 0/1） | 触点 DI，按 `verifyValue` 换算为输出状态 | 回执 `verifyPassed=true`（触点反馈） |
| CAN（`can_socketcan`） | `write.verifyIndex`（设备状态信号，不能是命令信号；脉冲信号不行） | 状态信号 | 回执 `verifyPassed=true` |

不满足时节点在下发前拒绝、不发送，状态 -2 并报错。锁存回读、`writeback-submitted`、坏质量、过期或发送之前的旧值都不算确认。回执结果：确认 → 3；确定失败 → -1；结果未知、不一致、写入但未确认 → **-2**（原来记 -1）。账本重放的 `writeback-unconfirmed` 不结束事务，等待最终回执或超时。超时之后晚到的确认只结束事务、不清除故障；重启后保留原事务、不重放、不恢复使能。

## 4. 兼容影响

- 回执协议、共享内存布局、MQTT 字段均未改变；平台和 KY-EMS 收到的内容不变。
- `confirmedControlWrite` 的状态输出：未确认但可能已执行的情况由 -1 改为 -2。读该输出点的逻辑（若有）需知道 -2 也表示“需人工复位”。仓库发货配置中没有使用该节点。
- AGC/AVC：结果未知时不再自动以新 cmdId 重发同一设定值。若现场依赖这种重发来补偿丢包，需要配置 PCS 遥测反馈（`feedbackTimeoutMs`），由遥测不到位触发重新下发。
- 本地画面：未结束的命令会锁住该点最长 70 秒；需要紧急操作时应使用设备侧或独立急停回路，不依赖本画面。
- 画面：单一来源的状态遇到未定义枚举由“--”改为“未知(原值)”（放不下时只显示“未知”，原值进提示；陈旧、坏质量仍为“--”）；预留按钮可以点击并说明原因（仍不下发）；电芯页共用探头只显示一次；PCS 模式标签改为“模式设定”；PCS 监测页“控制模式”改为“PCS远程/就地”。
- 生成器：断路器槽位固定按 zh-CN 排序（并网、储能、负荷）。区域设置为 zh-CN 的主机（按拼音排序）结果与以前相同，Windows 上未实测；其他区域设置的主机上生成时，槽位顺序会与以前不同（以前随主机变化）。已发布的工程包不受影响，重新生成时才会体现。
- 构建：新增源文件 `local_display_qt_control_receipt.cpp`，已加入 CMake、`tools/build_scada_qt_aarch64.sh` 和辅助脚本。Windows 预览工程补上了 SOE 依赖（`soe_store.cpp` 等）；修改前在 Linux 上无法链接，Windows 构建本轮未验证。

## 5. 未完成

- **平台与 KY-EMS 仓库无法访问**：平台侧对“仅提交”“结果未知”“晚到回执”的展示和统计，以及旧 `ky-ems/KY-EMS` 二进制的行为，本轮未改、未验证。**不能宣称全链路闭环**。
- 104 现场工程需用现场运行配置重新生成后复核实际绑定；HGM6110NC 发电机控制需在标准生成后再运行 `prepare_comm104_hgm6110nc_controls.py`；柴油机监测页尚未绑定 HGM 读点。
- 22.11 交叉编译（含 Qt）、目标板运行、实屏显示与触摸、真实设备联调均未执行。

## 6. 操作说明（本地画面）

1. 点击控制按钮后，回执框按第 1 节显示。只有“反馈确认成功”或“画面反馈点…一致”是动作确认；“已下发，未确认”“协议写入成功，动作未确认”需现场核实。
2. 出现“结果未知”时不要重复点击：该点会被锁住到最终回执或 70 秒，期间提示上一条命令编号。先到现场或在监测页核实实际状态，再决定是否重新操作。
3. 灰色按钮可以点击，只会弹出不可用的原因（PCS 就地、反馈过期、未开放等），不会下发命令。

## 7. 提交与实际验证结果

| 提交 | 内容 |
| --- | --- |
| `9070897` | feat(control)：控制回执分类 `control_receipt_view.hpp` 与测试 |
| `0c5f1e6` | fix(scada)：本地画面按钮/参数输入/PCS 分相功率/Windows 预览桥接按分类显示，晚到回执，未结束命令锁点 |
| `542c8be` | fix(ems-graph)：`confirmedControlWrite` 支持 CAN/DO `verifyIndex` 辅助反馈 |
| `f8f052e` | fix(agc-avc)：结果未知不以新 cmdId 重发同一设定值 |
| `0181827` | fix(scada)：未知枚举原值、未开放控件说明原因、状态文字自适应 |
| `ea265cb` | fix(ems2)：电芯共用探头只显示一次、单位按倍率、PCS 标签与预留原因 |
| `5e08dd6` | fix(ems2)：断路器槽位顺序不随构建主机区域设置变化 |
| `1dde69e` | build(scada)：`tools/build_scada_qt_aarch64.sh --with-tests` 增加 `local_display_qt_control_receipt_test`（22.11 Qt 测试由 3 个变为 4 个） |
| 本文档所在提交 | docs：本说明与画面逐页审查 |

验证分级如下，只有“模拟验证”和“编译通过”两类，**没有现场验收**：

| 项目 | 环境 | 结果 | 分级 |
| --- | --- | --- | --- |
| CTest 全量（x86_64 GCC 13.3 Release，Qt 6 offscreen） | 云端 | 157 项中 156 项通过；`ems_control_mode_test` 默认不构建（Not Run），单独构建运行通过 | 模拟验证 |
| 中间提交 `0c5f1e6` 单独构建 | 云端 | `control_receipt_view`、`local_display_qt_control_receipt`、`local_display_qt_lanhu` 通过 | 模拟验证 |
| PowerShell 生成器测试（pwsh 7.4.6 Linux） | 云端 | `lanhu-scada`、`lanhu-inventory`、`generate-scada`、`local-controls`、`lanhu-controls` 全部通过 | 模拟验证 |
| 布局审计 `--layout-audit` | 云端 Qt 6 | 448 次渲染，越界/重叠 0 项 | 模拟验证 |
| 边端 19 个目标交叉编译（`tools/build_edge_aarch64.sh --clean`） | 云端 Ubuntu aarch64 GCC 6.4.0（放在 Linaro 路径名下的替身，不是 22.11 的 Linaro 6.3.1 与厂商 sysroot） | 通过，0 警告 | 编译通过（不可部署） |
| Qt 源文件（回执跟踪、画面、PCS 功率、回执跟踪测试）GCC 6.4.0 `-std=gnu++17 -fsyntax-only` | 云端，Ubuntu Qt 5.15 头文件（不是设备 Qt 5 sysroot） | 通过；仅既有的 `QWheelEvent::pos()` 弃用警告 | 语法检查 |
| 22.11 交叉编译（含 KY-SCADA Qt 5）、目标板运行、实屏与触摸、真实设备 | — | **未执行** | — |
