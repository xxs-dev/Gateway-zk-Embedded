# 控制确认链路与 EMS 2.0 画面审查（2026-11-01）

基线 `realese1.0` = `eeaaa8a`。第六轮在本地分支 `r6/control-ems2` 提交 9 个提交（tip `7377901`），接收复核发现 3 项问题后在同一分支补修（tip `78feead`），再次复核发现 AGC 晚到结果的 2 项遗漏后又补修一次（tip `2a1a51f`）；接收方随后在内网用正式工具链构建并在 22.7 隔离验证，据此又做了两项测试与样式的最小补修（tip `e0b6722`），接收方再用正式工具链和 22.7 复验（第 8 节）。**未推送、未合并、未部署**。云端验证与内网验证分开记录（8.6、8.8 为内网）；所有验证都使用隔离测试数据，不接真实设备、不发电气控制命令；不是现场验收。画面逐页清单见 [EMS 2.0 画面逐页审查](../../products/ems/mobile/2.0/doc/screen-audit-20261101.md)。

## 1. 回执语义（协议不变）

回执字段 `success`、`outcome`、`stage`、`verifyAttempted/verifyPassed`、共享内存标志位与 MQTT/直连维护接口**均未改变**。新增的只是一个共用分类 `include/edge_gateway/control_receipt_view.hpp`，供本地画面使用：

| 回执 | 本地画面显示 | 是否算确认 | 该点的本地锁 |
| --- | --- | --- | --- |
| 尚无回执（等待内） | 命令已受理，等待驱动回执 | 否 | 保持 |
| `writeback-submitted` | 已下发，未确认：仅交给输出或通信，设备是否动作未确认 | 否 | 解除 |
| `writeback-succeeded`，`verifyPassed=false` | 协议写入成功，动作未确认 | 否 | 解除 |
| `verifyPassed=true` | 反馈确认成功（确认范围以回执中的反馈来源为准，回执原文一并显示） | 是 | 解除 |
| `local-parameter-committed` / `mailbox-committed` | 本地参数已提交（未下发设备） | — | 解除 |
| outcome=unknown（`writeback-timeout`/`writeback-mismatch`） | 结果未知：设备可能已执行；未自动重发（不一致时说明反馈与目标不一致） | 否，也不是失败 | **保持到跟踪期结束** |
| 确定失败（`writeback-failed`、`control-rejected` 等） | 执行失败 + 阶段 + 实际原因 | — | 解除 |
| `writeback-unconfirmed`（账本重放，无结果） | 视为未结束，继续等待 | 否 | 保持 |
| 等待 10 秒仍无回执 | 等待回执超时，结果未知；继续跟踪到 70 秒，期间晚到的最终回执标为“晚到回执”替换显示 | 否 | 保持 |

规则：只认本条命令（cmdId + 点号）的回执；最终结果出现后不再被任何较旧或矛盾的记录替换；任何情况都不自动重发。

本地锁（第 8 节修正后）：命令被边端受理时登记到进程级的本地控制事务表 `ControlTransactionRegistry`（`localControlTransactions()`），与页面和控件的生命周期无关。只有本命令、本点的确定最终回执（上表“解除”）才解锁；没有回执、非最终重放和结果未知都保持锁定，到跟踪期截止（单调时钟，按钮与参数输入 70 秒，PCS 三相 70 秒）一律解锁，不留永久锁。运行窗口每次刷新都会轮询事务表，所以页面因退出登录、登录过期或工程重载被重建后，晚到回执仍能结算；重新登录后锁仍有效。事务按 cmdId 区分，旧命令的晚到回执不会解除新命令的锁。事务表只在内存中：KY-SCADA 进程重启后不保留锁。

## 2. 修改的消费路径

| 路径 | 修改前 | 修改后 |
| --- | --- | --- |
| 本地画面按钮（`local_display_qt_scada_scene.cpp` → `local_display_qt_control_receipt.cpp`） | `success=false` 一律显示“驱动执行失败”（含结果未知）；成功不区分“仅提交/写入/反馈确认”；10 秒后不再接收晚到回执；不核对回执是否属于本命令 | 按上表分类；核对 cmdId+点号；晚到回执可替换超时显示；锁见第 1 节 |
| 本地画面参数输入（同上） | 提交后只提示“已提交”，不跟踪回执 | 与按钮共用同一跟踪 |
| PCS 分相功率（`local_display_qt_pcs_power_control.cpp`） | 每相只显示“成功/失败”，取到第一条回执后不再刷新；汇总按 success | 每相按分类显示；未结束的相继续查询直到最终回执或 70 秒；汇总按执行失败、结果未知、无最终回执、已下发未确认、协议写入未确认、本地参数已提交、反馈确认分别计数，不比逐相证据更强；三相锁见第 1 节，确认对话框之后再次检查 |
| Windows 预览桥接（`scada_windows_renderer_main.cpp`） | 第一条主机回执即弹出待查队列，晚到的最终回执被丢弃；忽略 outcome 字段 | 读取 `outcome/outcomeUnknown/feedbackMismatch/isFinal`；非最终回执不出队；最终回执不被替换；缺少 outcome 字段时按旧版规则（只有未下发的阶段才算确定失败） |
| 图形 EMS `confirmedControlWrite`（`confirmed_control.hpp`、`graph_ems_engine.cpp`） | 只接受“写后回读”的目标，CAN/DO 无法使用；所有未确认回执都记 -1（失败）；账本重放的未结束回执也被当成最终结果 | 见第 3 节 |
| AGC/AVC（`agc_avc_service.cpp`） | 结果未知（含无回执超时）按失败处理并清除已下发值，之后会**以新 cmdId 重发同一设定值** | 见第 4 节 |
| MQTT 回执、直连维护、第三方转发、pointctl | 已原样转发 stage/outcome/verifyPassed，并有非最终/最终回执与同 cmdId 查询 | 未改 |

## 3. confirmedControlWrite 的目标规则

| 目标协议 | 必要配置 | 节点回读 | 确认依据 |
| --- | --- | --- | --- |
| Modbus/DL645/IEC 等 | `write.verifyAfterWrite` 且 `verifyByRead`（原规则不变） | 目标点自身 | 回执 `verifyPassed=true` |
| 本机 DO（`local_dio`） | `write.verifyIndex`（辅助触点 DI，不能是输出本身；`verifyValue` 只能 0/1） | 触点 DI，按 `verifyValue` 换算为输出状态 | 回执 `verifyPassed=true`（触点反馈） |
| CAN（`can_socketcan`） | `write.verifyIndex`（设备状态信号，不能是命令信号；脉冲信号不行） | 状态信号 | 回执 `verifyPassed=true` |

不满足时节点在下发前拒绝、不发送，状态 -2 并报错。锁存回读、`writeback-submitted`、坏质量、过期或发送之前的旧值都不算确认。回执结果：确认 → 3；确定失败 → -1；结果未知、不一致、写入但未确认 → **-2**（原来记 -1）。账本重放的 `writeback-unconfirmed` 不结束事务，等待最终回执或超时。超时之后晚到的确认只结束事务、不清除故障；重启后保留原事务、不重放、不恢复使能。

## 4. AGC/AVC 的结果未知

结果未知（未知回执、反馈不一致、到 `writeResultTimeoutMs` 仍无回执、超时后的账本重放）登记为该目标点的**未知事务**（cmdId、值、所属 PCS 与方向、该方向的总目标、下发时间）。规则（第 8 节两次修正后）：

- 未知事务的值**永不再发**，与写入死区（包括 0）和普通失败处理无关。第六轮的实现借用死区比较，死区为 0 时会以新 cmdId 重发同一值，已修正。
- PCS 遥测反馈超时**不**结束未知事务：遥测不到位不能证明原命令没有执行。第六轮的实现在遥测超时后按普通失败清除已下发值，会以新 cmdId 重发，已修正；同一事务也不再被重复计入连续失败次数。
- 未知事务没有时间上限，在以下情况结束：
  - 同一 cmdId 的晚到**确定**回执：成功则结束；确定失败说明原命令未执行，允许以新 cmdId 再试。回执里的 cmdId 必须与事务一致。
  - PCS 遥测（该 PCS 配置了 `feedbackTimeoutMs > 0`）在容差内到达**本事务自己的**目标，且遥测样本好质量、未过期、采样时间晚于本命令下发时间。遥测观察超时之后才到达目标也算（第二次补修前不算）。
  - 控制器给出**不同**的设定值并被受理（新命令取代旧事务；旧命令的晚到回执和到达旧目标的遥测都不会结束新事务）。
- 记账：晚到成功回执和遥测确认与按时成功同样处理；晚到确定失败与普通失败一样阻止同周期的计数清零，但不再重复计数（变成未知时已计一次）。规则是“本周期有成功、且本周期没有任何失败”才把连续失败计数清零、写入状态置 1，所以同一周期另一点的失败不会被成功覆盖。刚变成未知的事务从下一周期开始核对，同一周期内不会既计失败又被确认抵消。
- 写入失败引起的失效（`maxConsecutiveWriteFailures`）不是独立闭锁：原有设计里任何确定成功都会清零计数、退出失效，晚到成功现在与之相同。急停、消防、远方使能等独立闭锁不受影响；不会恢复电气使能、重放命令。若业务要求写入失败后必须人工复位，需要另行建模（待决定）。
- 仍未结束的未知事务数量只在服务内部可查（`unknownWriteCount()`），没有输出点；连续失败计数被其他点的成功清零后，不再体现这些未结束的事务。是否新增输出点展示，待决定。
- 唯一例外（原有规则，保留）：已使能但不可用的 PCS（离线、SOC 不可用等）按尽力下发安全态 0，**即使 0 正是未知事务的值，也会重发**。这与下一条不是同一条件：达到 `maxConsecutiveWriteFailures` 后的失效归零不属于该例外，若未知事务的值本身就是 0，不重发 0（见第 6 节待决定）。所以不能笼统说“所有未知的 0 都不重发”。
- 未知事务只在内存中，AGC/AVC 服务重启后不保留。
- 已确认写入（回执成功）之后 PCS 遥测超时，仍按原有规则视为失败并允许重发，这是第六轮之前就有的行为，未改。其中 `writeback-submitted`（仅提交）在 AGC/AVC 里按成功处理；若 PCS 目标点走 CAN/DO 等只能“仅提交”的通道，需另行评估。

本轮**没有**开放“遥测不到位即自动重试”的策略；若现场确需自动重试，应单独定义授权与条件后再实现。

## 5. 兼容影响

- 回执协议、共享内存布局、MQTT 字段均未改变；平台和 KY-EMS 收到的内容不变。
- `confirmedControlWrite` 的状态输出：未确认但可能已执行的情况由 -1 改为 -2。读该输出点的逻辑（若有）需知道 -2 也表示“需人工复位”。仓库发货配置中没有使用该节点。
- AGC/AVC：结果未知时不再自动重发同一设定值，零死区和遥测超时也不例外。若现场以前依赖这种重发补偿丢包，升级后会表现为连续失败计数上升、按 `maxConsecutiveWriteFailures` 进入失效处理，需人工核实。
- 本地画面：结果未知（包括驱动给出的超时/不一致回执）的点在 70 秒跟踪期内不能再次下发，退出登录、重新登录和工程重载都不能绕过；需要紧急操作时应使用设备侧或独立急停回路，不依赖本画面。
- 画面：单一来源的状态遇到未定义枚举由“--”改为“未知(原值)”（控件配置了默认文字时用默认文字，如 PCS 模式设定显示“未知状态(7)”；放不下时只显示该文字，原值进提示；陈旧、坏质量仍为“--”）；组合状态（如并网/离网两路反馈）的无效组合仍显示“--”；预留按钮可以点击并说明原因（仍不下发）；电芯页共用探头只显示一次；PCS 模式标签改为“模式设定”；PCS 监测页“控制模式”改为“PCS远程/就地”。
- 生成器：断路器槽位固定按 zh-CN 排序（并网、储能、负荷）。区域设置为 zh-CN 的主机（按拼音排序）结果与以前相同，Windows 上未实测；其他区域设置的主机上生成时，槽位顺序会与以前不同（以前随主机变化）。已发布的工程包不受影响，重新生成时才会体现。
- 构建：新增源文件 `local_display_qt_control_receipt.cpp`，已加入 CMake、`tools/build_scada_qt_aarch64.sh` 和辅助脚本；事务表是头文件，不新增源文件。22.11 的 Qt 测试由 3 个变为 5 个（新增 `local_display_qt_control_receipt_test`、`local_display_qt_pcs_receipt_test`）。Windows 预览工程补上了 SOE 依赖（`soe_store.cpp` 等）；修改前在 Linux 上无法链接，Windows 构建未验证。

## 6. 未完成与待决定

- **平台与 KY-EMS 仓库无法访问**：平台侧对“仅提交”“结果未知”“晚到回执”的展示和统计，以及旧 `ky-ems/KY-EMS` 二进制的行为，未改、未验证。**不能宣称全链路闭环**。
- 104 发布前提不变：通用 56 页生成器不能替代 104 现场工程。必须用现场运行配置（`-RuntimeConfigDirectory`）重新生成、显式传入 BAU（`-BauMeterCode`）、标准生成后再运行 `prepare_comm104_hgm6110nc_controls.py`；不得因为重新生成丢掉现场工程已有的绑定。未确认的电表口径（变压器/电网与 10xx/11xx、曲线“电源侧”）不猜测补齐。
- 通用生成器输出的柴油机监测页没有 HGM 读点绑定；104 现场工程当前页面是否已有绑定未核对，不能据此推断现场缺绑定。
- 待决定：AGC/AVC 失效归零时，若未知事务的值本身是 0，是否也按安全态例外重发 0（当前不重发，保守）；是否需要“遥测不到位自动重试”的授权策略（当前未开放）；写入失败失效是否要求人工复位（当前与原设计相同，确定成功即退出）；是否为仍未结束的未知事务新增输出点。
- KY-SCADA 进程重启、AGC/AVC 服务重启后，内存中的锁和未知事务不保留。
- 22.11 正式构建与 22.7 隔离运行已由接收方对 `2a1a51f` 执行（8.6）；第三次补修 `31ee738`、`2ca4f24` 已由接收方用正式工具链和 22.7 隔离复验通过（8.8）。正式全量 CTest、真实屏幕与触摸、104 实际绑定、真实设备、平台/KY-EMS 联调仍未执行。
- 未闭环：云端 `local_display_qt_lanhu` 偶发失败一次，原因未查明（22.7 带工程参数连续 3 次通过只说明未复现）；`ota_original_job_query_test` 在正式 GCC 6 下的兼容问题只是交付方的判断，未构建、未修改。

## 7. 操作说明（本地画面）

1. 点击控制按钮后，回执框按第 1 节显示。只有“反馈确认成功”或“画面反馈点…一致”是动作确认；“已下发，未确认”“协议写入成功，动作未确认”需现场核实。
2. 出现“结果未知”时不要重复点击：该点保持锁定，直到出现确定回执或 70 秒跟踪期结束，期间点击会提示上一条命令编号。退出登录或重新登录不会解除。先到现场或在监测页核实实际状态，再决定是否重新操作。
3. PCS 三相功率的汇总逐类列出各相结果，例如“3 相已下发、未确认”不等于协议写入成功，更不等于功率已到位。
4. 灰色按钮可以点击，只会弹出不可用的原因（PCS 就地、反馈过期、未开放等），不会下发命令。

## 8. 提交与实际验证结果

### 8.1 第六轮提交（`eeaaa8a..7377901`）

| 提交 | 内容 |
| --- | --- |
| `9070897` | feat(control)：控制回执分类 `control_receipt_view.hpp` 与测试 |
| `0c5f1e6` | fix(scada)：本地画面按钮/参数输入/PCS 分相功率/Windows 预览桥接按分类显示，晚到回执，未结束命令锁点 |
| `542c8be` | fix(ems-graph)：`confirmedControlWrite` 支持 CAN/DO `verifyIndex` 辅助反馈 |
| `f8f052e` | fix(agc-avc)：结果未知不以新 cmdId 重发同一设定值（不完整，见 8.2） |
| `0181827` | fix(scada)：未知枚举原值、未开放控件说明原因、状态文字自适应（带入组合状态回退，见 8.2） |
| `ea265cb` | fix(ems2)：电芯共用探头只显示一次、单位按倍率、PCS 标签与预留原因 |
| `5e08dd6` | fix(ems2)：断路器槽位顺序不随构建主机区域设置变化 |
| `1dde69e` | build(scada)：22.11 Qt 测试加入回执跟踪测试 |
| `7377901` | docs：本说明与画面逐页审查 |

### 8.2 接收复核后的补修（`7377901..`）

接收复核在本机（WSL GCC 15.2 Debug、Qt 5.15.18 offscreen、隔离命名空间）用反例确认了 3 项问题；补修时又发现 1 项第六轮带入的回退。

| 提交 | 问题 | 修正 |
| --- | --- | --- |
| `cae85de` | 第六轮 `0181827` 把状态无匹配时的默认文字改成“未知”，组合状态的无效组合不再显示“--”。只有带工程参数运行 `local_display_qt_lanhu_test` 才执行该断言，第六轮只跑了不带参数的 CTest，未发现 | 只有单一来源的未定义枚举显示“未知(原值)”，组合状态恢复“--” |
| `adc4eac` | [P1] AGC 结果未知仍会以新 cmdId 重发同一值：死区为 0 时；遥测超时把未知当失败清除时 | 第 4 节 |
| `daf21cf` | [P2] PCS 控件销毁后三相锁永不过期；普通回执框析构会提前解锁 | 第 1 节“本地锁” |
| `4561ce5` | [P2] PCS 汇总把仅提交/本地提交/协议写入合并成“三相协议写入成功” | 第 2 节 PCS 一行 |
| 本文档所在提交 | docs：本说明与画面逐页审查更新 | — |

反例修复前后对照（云端 x86_64 GCC 13.3，Qt 6 offscreen；“修复前”= `7377901` 源码 + 新测试）：

| 反例 | 修复前 | 修复后 |
| --- | --- | --- |
| 复核者 PCS 夹具 `pcs-submitted-summary`（三相均为 `writeback-submitted`） | 失败：`三相协议写入成功，其中 3 相未经回读确认（动作未确认）` | 通过：`A/B/C 三相：3 相已下发、未确认（仅交给输出或通信，设备是否动作未确认）` |
| 复核者 PCS 夹具 `pcs-destroy-lock`（受理后销毁控件，等待原 70000 ms 截止 + 200 ms） | 失败：`tagUnresolved=1` | 通过：`tagUnresolved=0`，`fakeWrites=1` |
| 回执框页面销毁（新夹具，两版共用） | 失败：销毁后立即解锁 | 通过：销毁后仍锁定 |
| `agc_avc_unknown_result_test` 12 个场景 | 7 个失败：零死区、反馈不一致、无回执、遥测超时（正/零死区）、晚到确定失败、失效阈值 | 12 个全部通过 |
| `local_display_qt_lanhu_test` 带工程参数 | 失败：`invalid mode pair claims a mode` | 通过（含新增的退出/重新登录后锁仍有效、窗口刷新结算晚到回执） |

修复前在零死区和遥测超时两种情况下观察到的就是复核报告中的同值重发；“晚到确定失败”在修复前失败，是因为旧实现根本不跟踪晚到回执（新增能力），不是复核所列问题。

### 8.3 验证分级

只有“模拟验证”“编译通过”和“语法检查”三类，**没有现场验收**：

| 项目 | 环境 | 结果 | 分级 |
| --- | --- | --- | --- |
| 新增与相关测试：`agc_avc_unknown_result`、`agc_avc_controller`、`control_transaction_registry`、`control_receipt_view`、`local_display_qt_control_receipt`、`local_display_qt_pcs_receipt`、`local_display_qt_lanhu`（不带参数与带工程参数） | 云端 x86_64 GCC 13.3 Release，Qt 6 offscreen | 全部通过 | 模拟验证 |
| CTest 全量 | 同上，独立 IPC/挂载命名空间 | 见 8.4 | 模拟验证 |
| 布局审计 `--layout-audit`（含断路器排序修复后重新生成的工程） | 云端 Qt 6 | 448 次渲染，越界/重叠 0 项 | 模拟验证 |
| PowerShell 生成器测试 5 组 | 云端 pwsh 7.4.6 | 第六轮结果，本轮未改生成器 | 模拟验证 |
| 修改的 C++ 源文件与测试 GCC 6.4.0 aarch64 `-fsyntax-only`（Qt 部分对 Ubuntu Qt 5.15 头文件） | 云端，替身工具链，不是 22.11 的 Linaro 6.3.1 与厂商 sysroot | 通过；`agc_avc_controller_test.cpp` 中既有的 `has_value()`（GCC 6 的 experimental optional 没有）报错，非本轮引入，该测试不在 aarch64 构建列表（第三次补修 `31ee738` 已改，见 8.7） | 语法检查 |
| 22.11 交叉编译（含 KY-SCADA Qt 5 与 5 个 Qt 测试）、目标板运行、实屏与触摸、真实设备、平台联调 | — | **未执行** | — |

### 8.4 CTest 全量

补修后的完整源码（与最终提交一致），云端 x86_64 GCC 13.3 Release、Qt 6 offscreen，独立挂载与 IPC 命名空间、独立 `/dev/shm`：160 项中 158 项通过。未通过的 `memory_point_store_migration`、`memory_point_store_v11_migration` 报“cannot inspect /proc/1/maps”，是运行环境没有独立 PID 命名空间所致，与本轮修改无关；在独立 PID 命名空间（`--pid --mount-proc`）中重跑这 2 项均通过。`ems_control_mode_test` 默认不构建，单独构建运行通过。新增的 3 项（`agc_avc_unknown_result`、`control_transaction_registry`、`local_display_qt_pcs_receipt`）包含在 160 项内。

中间提交：`cae85de`、`adc4eac`、`daf21cf` 的修改文件逐一做了编译检查。`daf21cf` 单独构建后 `control_transaction_registry`、`local_display_qt_control_receipt`、`local_display_qt_lanhu`（不带参数与带工程参数）通过；`cae85de` 单独构建后 `local_display_qt_lanhu` 带工程参数通过。对照：`eeaaa8a` 带工程参数通过，`7377901` 失败（组合状态回退）。

### 8.5 第二次复核后的补修（`78feead..`）

复核在本机用独立夹具（`agc-late-review.cpp`，只读取未知事务数量）确认 `78feead` 上 AGC 晚到结果的 2 项遗漏：遥测观察超时后才到达目标，未知事务无法结束；晚到成功回执只删除事务，不更新连续失败计数和写入状态。上一轮的 3 项问题复核通过。

| 提交 | 修正 |
| --- | --- |
| `5ae7df4` | 第 4 节：未知事务自带 PCS/方向/目标/下发时间，每周期自行核对回执与遥测；晚到结果与按时结果统一记账；同周期失败不被成功覆盖；新旧事务隔离 |
| 本文档所在提交 | docs：第 4、6 节与本节 |

复核者夹具修复前后对照（云端 x86_64 GCC 13.3；修复前链接 `78feead` 的库，修复后链接 `5ae7df4`，夹具源码和断言未改）：

| 夹具场景 | 修复前（`78feead`） | 修复后 |
| --- | --- | --- |
| `late-telemetry`：P80/Q0，死区 0.1，`feedbackTimeoutMs=100`，阈值 10；+600 ms 未知，+1200 ms 观察超时，+1500 ms 遥测到 P80/Q0 | `failures=2 lastWriteStatus=-1 state=2 unknownTransactions=2`，失败 | `failures=0 lastWriteStatus=1 state=2 unknownTransactions=0 submitted=0`，通过 |
| `late-receipt`：遥测观察关闭，死区 0.1，两条未知随后同 cmdId 成功 | `failures=2 lastWriteStatus=-1 unknownTransactions=0`，失败 | `failures=0 lastWriteStatus=1 unknownTransactions=0 submitted=0`，通过 |
| `late-receipt-failsafe`：初始 P/Q 均为 0，阈值 2 | `state=6`（Failsafe）`failures=2 lastWriteStatus=-1`，失败 | `state=2 failures=0 lastWriteStatus=1 submitted=0`，通过（与按时成功一样退出写入失败失效，未重放命令） |

`agc_avc_unknown_result_test` 增至 21 个场景，新增 9 个全部通过：超时后到达目标的遥测（正/零死区）、命令下发前采样的旧遥测不算、晚到成功（含全零目标的失效阈值）、晚到确定失败只计一次、回执 cmdId 不符不算、同周期另一点确定失败不清零、新旧事务隔离（旧命令晚到成功和到达旧目标的遥测都不结算新命令）。变异检查：去掉“采样晚于下发”、去掉晚到确定失败的本周期失败标记、去掉晚到成功的本周期成功标记，分别使对应场景失败。零死区时，事务结算后按原有零死区规则周期刷新已确认的值（新 cmdId），这是刷新，不是对未知结果的重试。

CTest 全量（独立挂载、IPC 与 PID 命名空间，独立 `/dev/shm`）：160 项全部通过；`ems_control_mode_test` 单独构建运行通过。修改的源文件（`agc_avc_service.cpp`、`agc_avc_controller.cpp`、`agc_avc_main.cpp`、新测试）用替身 GCC 6.4.0 aarch64 做语法检查通过。22.11 构建、ARM 运行、Qt 实机、真实设备、平台联调仍未执行。

### 8.6 内网正式构建与 22.7 隔离验证（接收方执行，对象 `2a1a51f`）

以下是接收方在内网实际执行的结果，摘自其报告 `driver-r8-production-20261101/report.md`，云端没有执行这些步骤。

**22.11 正式构建**：Ubuntu 18.04.4，Linaro GCC 6.3.1 20170404，CMake 3.25.2，Python 3.6.9，Allwinner Qt 5.12.8 sysroot；独立路径，没有覆盖 `/srv/build/Gateway-zk`。构建前后逐项校验 3182 个 Git blob 与执行位，均一致。使用仓库原有 `tools/build_edge_aarch64.sh` 与 `tools/build_scada_qt_aarch64.sh --with-tests`，没有使用发布或部署选项。

| 构建对象 | 数量 | 结果 |
| --- | ---: | --- |
| 边端既定目标（含迁移工具、stress_runner） | 19 | 通过 |
| KY-SCADA | 1 | 通过 |
| Qt 测试：access、lanhu、soe、control_receipt、pcs_receipt | 5 | 通过 |
| AGC 未知结果、confirmedControlWrite、回执分类、事务表测试 | 4 | 通过 |
| 原始晚到结果复核夹具（源码 SHA256 `8eaabaee…`，诊断构建用 `-fno-access-control`） | 1 | 通过 |
| 额外构建的 `agc_avc_controller_test` | 1 | **编译失败**（`has_value()`，`eeaaa8a` 已存在），未运行 |

29 个主构建产物下载后重新读取，SHA256 全部一致。

**22.7 隔离运行**：设备身份 `COMM202600998`，Ubuntu 20.04.6，aarch64，Linux 5.10.198。独立 mount/net/IPC/PID 命名空间，私有 `/dev/shm`、`/tmp`、`/run`，网络仅 lo 与 sit0 且均 DOWN 无地址；屏蔽安装目录与串口、CAN、framebuffer 等设备路径；UID 65534、无能力、NoNewPrivs。没有接入真实设备，没有发送电气控制命令。

| 项目 | 实际结果 |
| --- | --- |
| AGC 未知结果 21 个场景 | 21/21 通过 |
| 原始晚到结果 3 个反例 | 通过（`failures=0 lastWriteStatus=1 state=2 unknownTransactions=0 submitted=0`，未重放命令） |
| confirmedControlWrite、回执分类、事务表 | 通过 |
| Qt access、control receipt、PCS receipt | 通过 |
| Qt Lanhu（带生成工程参数） | 通过 |
| Qt SOE | **首轮失败**：隔离 `/tmp` 只有 128 MiB，低于 SOE 生产存储保护要求的 256 MiB 保留空间，报 `SOE storage unavailable: min-free-bytes`。复测只把私有 tmpfs 上限改为 512 MiB，二进制、夹具、断言和生产保护均未改，复测通过 |
| 布局审计 | 56 页 × 4 种分辨率 × 2 种数据状态 = 448 次离屏渲染，报告 0 项。四种尺寸检查的是等比缩放；文字越界/重叠检测只针对 1920×1080 的文字控件，不覆盖颜色、图标和现场绑定 |
| 正式 KY-SCADA | 加载生成工程与私有空共享内存运行 15 秒，发送终止信号后 exit 0；前后私有段 `pendingWrites=0` |

首批 15 个运行条目为 14 通过、1 个 SOE 环境失败，之后只复测 SOE 并通过。**不能写成首轮 15/15，也不能写成正式全量 CTest 通过**。测试工程由快照生成（56 页、1905 tags/routes、模拟机器码 `COMM202600999`），不是 104 现场工程。

**现场保持**：前后 130 个安装程序/配置/服务配置文件 SHA256 一致；显示、监测、健康看门狗的 PID、启动时间、重启计数一致；原有共享内存段名称/inode/大小一致；boot ID 与接口地址一致。没有重启现场服务，没有修改网络、EasyTier、4G、账号或电气控制配置；104 未操作。

**截图抽查发现**：SOE 选中行为白字浅蓝底，难以阅读（`local_display_qt_soe.hpp` 与 `eeaaa8a` 相同，不是本分支回退）。布局审计不检查颜色，所以没有报告。

### 8.7 第三次补修（`2a1a51f..`，云端验证）

| 提交 | 内容 |
| --- | --- |
| `31ee738` | `tools/agc_avc_controller_test.cpp:705/710` 的 `has_value()` 改为 `static_cast<bool>(...)`，与同文件已有写法一致，断言语义不变；生产控制逻辑未改 |
| `2ca4f24` | `local_display_qt_soe.hpp` 补 `selection-color:#303438`（与正文、蓝湖表格一致）；`local_display_qt_soe_test` 增加选中行（有/无键盘焦点）、普通行、交替行第 1–7 列文字对比度 ≥ 4.5:1（WCAG AA）及选中行背景可区分的像素检查，可选第二个参数保存选中状态截图 |
| 本文档所在提交 | docs：8.6、8.7 与第 6 节 |

| 项目 | 修复前（`2a1a51f`） | 修复后 | 环境与分级 |
| --- | --- | --- | --- |
| `agc_avc_controller_test` 用 GCC 6 编译 | 705、710 两处 `has_value` 报错 | 编译链接通过，`qemu-aarch64` 用户态运行 `agc_avc_controller_test passed` | 云端替身 GCC 6.4.0（Ubuntu 交叉编译器）+ qemu；**不是**正式 Linaro 6.3.1 与 sysroot，不能记为正式编译通过 |
| `agc_avc_controller_test` x86 | 通过 | 通过 | 云端 GCC 13.3 |
| SOE 选中行对比度 | 失败：选中行第 1 列 1.14:1（背景 `#e3f2fc`，白字） | 通过：选中行（有/无焦点）、普通行、交替行均 ≥ 4.5:1，选中背景可区分 | 云端 Qt 6 offscreen；设备 Qt 5.12.8 上**未复验** |
| `local_display_qt_soe_test` 其余断言 | — | 分页、质量、录波关联、对齐、只读、机器隔离仍通过 | 云端 Qt 6 |
| `local_display_qt_lanhu_test` 带工程参数 | — | 通过 | 云端 |
| Qt/AGC/控制相关 CTest 17 项 | — | 第一次运行 16 项通过，`local_display_qt_lanhu` 在 1.63 秒处失败（未加 `--output-on-failure`，失败信息未保留）；带输出重跑 17 项通过；另单独 ctest 3 次、三项组合 6 次、直接运行 12 次、CPU 加载下 8 次均通过 | 云端；原因未查明，本轮未改该测试。该测试有 250 ms 等待对 200 ms 回执轮询的窄余量（`85917a30` 起就有），是可能原因之一，未证实 |

另见：`tools/ota_original_job_query_test.cpp` 直接使用 `std::optional` 的 `has_value()`，正式 GCC 6 同样无法编译；它不在 aarch64 构建列表，与本分支无关，本轮未改。

接收方随后用 22.11 正式工具链重建这两个测试并在 22.7 隔离运行，结果见 8.8。

### 8.8 第三次补修的内网正式复验（接收方执行，对象 `e0b6722`）

以下是接收方在内网实际执行的结果，摘自其报告 `driver-r9-review-20261101/report.md`；云端没有执行这些步骤。

**正式编译**：22.11，Linaro GCC 6.3.1 20170404，Allwinner Qt 5.12.8 sysroot，CMake 3.25.2；独立目录，未复用 R8 构建缓存。构建前后 3182 个源码文件按 Git blob 与执行位校验一致。命令为 `tools/build_edge_aarch64.sh --jobs 3 agc_avc_controller_test agc_avc_unknown_result_test confirmed_control_test` 与 `tools/build_scada_qt_aarch64.sh --with-tests`：重建了核心库、这 3 个 C++ 测试、KY-SCADA 和 5 个 Qt 测试，**没有**重建全部 19 个边端生产目标。另用同一编译器和 sysroot 构建两份 SOE 对照程序（同一份新测试源码，分别配旧样式和新样式头文件，各自单独链接）。共 11 个产物下载后 SHA256 一致；未改变的 AGC unknown、confirmed control 产物与 R8 哈希一致。KY-SCADA 已重编，未作为现场显示服务启动。

**22.7 隔离运行**：身份 `COMM202600998`，隔离条件与 8.6 相同；私有 `/tmp` 从一开始就是 512 MiB（满足 SOE 256 MiB 保留要求，生产保护未改）。

| 验证项 | 实际结果 |
| --- | --- |
| `agc_avc_controller_test` | 正式 GCC 6 编译、ARM 运行通过；R8 的两处 `has_value` 编译错误已消除 |
| AGC 未知结果 | 21/21 场景通过 |
| confirmedControlWrite | 通过 |
| 新像素测试配旧样式（反例） | 按预期失败：无焦点选中行第 1 列 `1.143001 < 4.5`，背景 `#e3f2fc`，exit -6 |
| 新像素测试配新样式 | 通过 |
| 仓库正式脚本构建的 Qt SOE 测试 | 全部断言通过（选中/未选中对比度、分页、质量、录波关联、居中、只读、机器隔离） |
| Qt access、control receipt、PCS receipt | 通过 |
| Qt Lanhu 带工程参数 | 连续 3 次通过（24.435 / 24.444 / 24.477 秒） |

共 12 条记录：11 条正向运行 exit 0，1 条旧样式反例按预期 exit -6。**不能写成 12 个程序全部正常退出**。接收方复看了正式 SOE 测试的有焦点/无焦点截图：选中行为深色文字、浅蓝背景，可读，选中态仍可区分（板上离屏模拟数据，不是实体触摸屏照片）。测试工程复用 R8 生成的 56 页隔离工程（逐文件哈希一致，机器码 `COMM202600999`，不是 104 工程）；本轮**没有重跑** 448 次布局审计。

**现场保持**：前后 130 个安装程序/配置/服务配置哈希一致；显示、监测、健康看门狗的 PID、启动时间、重启计数一致；boot ID、接口地址、6 个现场共享内存段名称/inode/大小一致；私有共享内存结束时为空。没有重启现场服务、连接真实设备或改变网络/EasyTier/4G；104 未操作。

**结论分层**：`31ee738`、`2ca4f24` 两项为“22.11 正式工具链与 22.7 ARM/Qt 隔离复验通过”，不是现场全链路验收，也不是合并、推送或部署许可。仍未闭环：云端 `local_display_qt_lanhu` 偶发失败（原因未明；板上 3 次通过只说明未复现，250 ms/200 ms 时序余量仍只是假设）；`ota_original_job_query_test` 的 GCC 6 兼容问题（未构建、未修改）；正式全量 CTest；真实触摸屏、104 现场绑定、真实设备、电气动作、平台/KY-EMS 联调。四项 AGC 策略和进程重启不保留未知事务/画面锁的限制不变。
