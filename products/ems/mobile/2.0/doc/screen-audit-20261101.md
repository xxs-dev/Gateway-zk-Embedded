# EMS 2.0 画面逐页审查（2026-11-01）

基线 `realese1.0` = `eeaaa8a`。审查对象是 EMS 2.0 实际发布路径：`tools/release/build-ems-mobile.ps1` → `products/ems/mobile/2.0/tools/generate-scada.ps1`（`LayoutProfile=Lanhu`，56 页，1440×810 画布）→ 统一 Qt 运行时 `KY-SCADA`（`local_display_qt_scada_scene.cpp` 等，入口 `local_display_qt_ems_main.cpp`）。旧 XMind 布局、`ky-ems/` 旧二进制和本地截图图库不在本次修改范围。

**结论只到“模拟验证”**：截图和自动化回归使用隔离测试数据，未接设备、未发命令；不是 104 现场画面，也不是现场验收。

第六轮交付（`7377901`）的接收复核发现 PCS 汇总误报和 PCS 锁不过期两项画面问题，补修后又查出第六轮带入的组合状态显示回退，均已在同一分支修正，见第 6 节和 [控制确认链路说明](../../../../../doc/releases/control-confirmation-ems2-20261101.md) 第 8 节。

## 1. 数据与控制链路

**数据**：采集驱动（Modbus/DL645/IEC/CAN/DIO）→ 各自共享内存段（`sharedMemoryName` + `index`，值/质量/时间/过期）→ 工程 `tags.json` 与 `runtime-map.json` 把 `tagId` 解析为段名+索引 → `ScadaSceneView::refresh` 每周期读取 → 质量≠1、过期、非有限值一律显示 `--` 或“反馈不可用/数据不可用”，不显示为 0、正常或成功。

**控制**：按钮（`handleAction`）→ 本机可写映射存在 → PCS 远程/就地守卫（1214，5 秒内的好质量值且=1；否则灰态、点击说明原因、不下发）→ 本地控制事务锁（同一点上一条命令没有确定结果时拒绝，见下）→ 取值/范围/选项 → 二次确认 → 确认后重查守卫和锁 → 本地登录会话（`submitOperatorWrite`）→ `PointStoreRouter::submitWriteCommand`（功率控制所有权、信箱点拒绝、可写、集群授权、EMS 虚拟参数校验）→ 共享内存待写队列 → 驱动执行（allowedValues/范围再校验）→ 回执写入共享内存（stage、outcome 标志、verifyAttempted/verifyPassed、消息）→ `ControlReceiptTracker` 按 cmdId+点号读取并分类显示。

**本地控制事务锁**：命令受理时登记到进程级 `localControlTransactions()`，与页面无关；本命令本点的确定最终回执解锁，无回执、非最终重放、结果未知保持锁定，到 70 秒跟踪期截止一律解锁。运行窗口每次刷新都轮询事务表，页面因退出登录、登录过期、工程重载被重建后，晚到回执仍能结算，重新登录也不能绕过。KY-SCADA 进程重启后不保留。

页面禁用不是安全边界：后端在路由和驱动两处再次校验可写性、取值和授权；PCS 远程/就地由 PCS 自身执行，边端只在界面拦截并以设备拒绝为准。

## 2. 生产绑定的前提

仓库内的 `scada/base-project` 来自 104 已验收工程（2.0.8），只有 tags/runtime-map，没有 104 的设备运行配置。很多控制按钮只有在生成时传入 `-RuntimeConfigDirectory`（现场拉取的设备配置）并通过协议点核对（`Test-RefProtocolPoint`：寄存器地址、功能码、设备）后才会绑定；不传时生成为“未开放”。BAU 控制还需要显式 `-BauMeterCode`。本次审查按基线工程和生成器逻辑进行，**现场工程的实际绑定需用现场运行配置重新生成后复核**。通用 56 页生成器不能替代 104 现场工程；重新生成不得丢掉现场工程已有的绑定（运行配置、显式 BAU、HGM 后处理三者缺一不可）。

104 的柴油发电机（HGM6110NC）控制不是标准生成器的输出，而是 `tools/prepare_comm104_hgm6110nc_controls.py` 对现场工程的后处理（见 `doc/releases/comm104-generator-controls-20261029.md`）。**重新运行标准发布脚本会得到没有发电机控制的 Control-Diesel**，发布时必须再跑该工具。

## 3. 逐页清单

状态：✔ 已核对无问题；✎ 本次修复；? 待核实（缺协议或现场依据，未猜测补齐）；— 正文留空（无完整设计或无确认接口）。

| 页面 | 绑定来源 | 发现问题 | 修复 | 测试 | 未确认项 |
| --- | --- | --- | --- | --- | --- |
| 公共框架（56 页） | 时钟、品牌、机器码、8 个导航、登录/退出 | 预留按钮在触摸屏上只有提示气泡，点了没反应 | ✎ 预留/未开放按钮保持灰态，点击弹出原因，不下发 | lanhu-scada、local_display_qt_lanhu | — |
| 监测·EMU `Devices-Emu` | 本机：`localSystemInfo`（机器码、型号、固件、EMS 控制权限、云/第三方配置、CPU/内存/磁盘/温度） | ✔ 未借用其他设备数据；EMS 控制权限读 EMS 本机模式文件 | — | lanhu-scada（EMU 绑定全部本机字段） | 序列号、平台连接状态无确认接口 |
| 监测·PCS `Devices-Pcs` | PCS `MTR20250600657`（1200–1323；直流 1243–1245、1249/1250；交流 1220–1248） | “控制模式”易与 EMS 远程/就地混淆；并离网状态显示“并网状态/离网状态” | ✎ 改为“PCS远程/就地”；状态改为“并网/离网” | layout-audit、截图 | “下发有功/无功”列是设定寄存器回读，不是实测 |
| 监测·BMS `Devices-Bms` | BMS `MTR20250600659`（地址 2，1550–1591） | 极值电压单位写死 mV | ✎ 单位按运行配置倍率推导（mV 寄存器×0.001 显示 V），无配置时按基线 mV | lanhu-scada、layout-audit | 实时功率、簇允许功率/电压、循环次数未确认 |
| 监测·电芯 `Devices-Cells`…`-Page9` | BMS 2001–2260（电压）、3001–3130（连接点温度） | **一个连接点温度被重复显示在相邻两节电芯上**，像两份独立测量；极值单位写死 | ✎ 每个共用探头只显示一次（奇数节），偶数节显示“共用探头#k”，页头说明；电芯电压按点表 0.001 倍率显示 V，不再除 1000；极值单位按各自倍率 | lanhu-scada、lanhu-inventory（34 节两簇）、截图 | 是否每节都有独立温度需按 BMS 资料确认（当前按 260 节/130 探头） |
| 监测·TMS `Devices-Thermal`/`Devices-Fire` | 液冷、除湿、DIO 风机、消防 CAN 探测器 | ✔ 风机只有一个继电器反馈，未伪装三台 | — | lanhu-scada | — |
| 监测·MCCB `Devices-Mccb` | DIO 997/998/999 合位（按配置数量） | ✔ 数量来自配置，不因通信失败隐藏；**断路器槽位顺序随构建主机区域设置变化**（不变区域下“储能”排在“并网”前） | ✎ 固定按 zh-CN 排序（并网、储能、负荷） | lanhu-inventory（sparse；不变区域下核对顺序） | 无独立通信反馈 |
| 监测·电表 `Devices-Meters`、`-Page2` | 并网点表 11xx、防逆流表 10xx、负荷表 90xx | ✔ 能量 >100000 kWh 显示 MWh（不是 mWh） | — | 截图（150000 kWh→150 MWh） | PT/CT 变比无点位 |
| 监测·柴油机 `Devices-Diesel` | 通用生成：无 | 通用生成器输出的监测页没有 HGM6110NC 读点绑定 | 未改（标准生成器不认识 HGM，点表不在仓库） | — | ? 104 现场工程当前页面是否已有绑定未核对，不据此推断现场缺绑定；需要时按现场 HGM 点表补 |
| 监测·网络 `Monitor-Network`/`Info-Messages` | `SYSTEM_CELLULAR` 920000003、005–009 | ✔ | — | — | IP、有线、GPS 无接口 |
| 监测·策略 `Monitor-Strategy` | EMS_CORE 8/10/12/18/20/24 运行状态、参数 | ✔ 运行状态与使能点分开 | — | lanhu-scada | 588 单相口径未证实 |
| 仪表 `Meters-*`（8 页） | 各电表三相量、汇总、近 2 小时曲线 | 无功单位 kVar/kvar 写法不一（仅大小写） | — | layout-audit | — |
| 仪表·电能 `Meters-Energy` | 无 | — | — | — | — 正文留空 |
| 拓扑·并机 `Topology` | 本机遥测 1230/1234/1227–1229/1220–1222/1242/1226/1569/1399/3999/997；成员来自组网状态 | ✔ 本机车按本机点显示，从车不借本机遥测 | — | lanhu-scada、local_display_qt_lanhu | ? 卡片“变压器”=防逆流表 10xx、“电网”=并网点表 11xx 的对应需按单线图确认 |
| 拓扑·单机 `Topology-Single` | 负荷表、并网点表、PCS、BMS、3 个断路器 | 状态徽标在长文字时溢出 1.5 px | ✎ 状态文字也按控件自适应缩放 | layout-audit 0 溢出 | AC 母线、DC 接触器、功率方向未确认 |
| 曲线 `Trends-*`（7 页） | 防逆流表、BMS、PCS | 电芯曲线单位写死 mV | ✎ 按倍率推导 | lanhu-scada | ? 选择器“电源侧监测”绑定的是防逆流表 10xx，而单机拓扑“电源侧”是并网点表 11xx，需确认 |
| 告警 `Info-Alarms` | 已配置告警条件 | ✔ 缺失/坏质量/过期不计为零告警 | — | local_display_qt_lanhu | 今日/累计/恢复时间无历史接口 |
| SOE `Info-Soe` | `SoeStore` 只读 | ✔ 录波状态“待核实/当前目录未找到”不显示为“无录波”；22.7 离屏截图发现选中行白字浅蓝底难读（基线已有） | ✎ 选中行固定深色文字（`2ca4f24`） | local_display_qt_soe（含选中/未选中对比度 ≥ 4.5:1） | 设备 Qt 5.12.8 上未复验 |
| 引导 `Guide-*`（3 页） | 通信反馈 | ✔ 未映射检查保持未知；“完成部署”不可执行 | ✎ 预留按钮点击说明原因 | — | 供电状态、自检汇总 |
| 控制·PCS `Control-Pcs` | PCS 1310（模式设定）、1209–1211、1216/1217、1200–1202、1315、1318–1323 | “当前模式”实为保持寄存器的**设定值回读**；未知枚举显示“--” | ✎ 标签改“模式设定”；未知枚举显示“未知(原值)”（放不下时只显示“未知”，原值进提示）；未开放的切换模式/并网/离网说明原因；✎ 第 6 节：分相功率汇总按回执类别分别统计，三相锁不随页面消失 | lanhu-controls、local_display_qt_lanhu、local_display_qt_pcs_receipt、截图 | 并离网设定 1315 只在运行配置核对后开放 |
| 控制·BMS `Control-Bms` | BAU（需 `-BauMeterCode`）或 BMS 状态 | ✔ 只有显式指定的 BAU 有命令，Pack 级 BCU/BMU 不开放 | — | lanhu-controls（BAU+Pack） | 104 的 BAU=地址 2（`MTR20250600659`/`mb3_s2`），与现有配置一致；发布时需显式传入 |
| 控制·TMS `Control-Thermal` | 液冷 128（读）/164（写，枚举不同）、温度设定 | ✔ 读写枚举分开 | ✎ 未开放时说明原因 | lanhu-controls | — |
| 控制·MCCB `Control-Mccb` | 断路器遥控（DL645）+ DIO 合位反馈 | ✔ 分合闸反馈用 DIO 合位，不用按钮或回执代替；槽位顺序问题同监测页，带遥控的并网断路器在 Linux 生成时落到别的槽位 | ✎ 同上 | lanhu-controls | 物理分合闸未验收 |
| 控制·柴油机 `Control-Diesel` | 标准生成：无；104 现场：HGM 7 个命令 | ✔ 合分闸无独立位置反馈，只显示“协议写入成功，动作未确认” | 回执措辞随控制链路修复 | local_display_qt_control_receipt | 物理动作未验收 |
| 控制·DI/DO `Control-Dido`、`Vehicle-Control` | 无 | — | — | — | — 正文留空 |
| 策略 `Strategy-Overview` | EMS_CORE 458 防逆流使能（唯一可写） | ✔ 运行状态点不可写 | — | lanhu-scada | 其他策略使能未确认 |
| 策略子页 `Strategy-Balance/Charge/Grid`、`Maintenance`、`Guide-Startup` | 无 | — | — | — | — 正文留空 |

## 4. 布局

`local_display_qt_lanhu_test --layout-audit`：56 页 × 4 种分辨率（1440×810、1920×1080、1280×720、1280×1024）× 缺值/超长值 = 448 次渲染，1920×1080 下逐文本越界/重叠检查 **0 项**。中途加入“未知枚举显示原值”后同口径曾出现 30 项（窄状态框放不下“未知(-123456.78)”），已改为放不下时只显示“未知”、原值进提示，并让状态文字按控件自适应缩放。未对 `eeaaa8a` 原版单独跑该审计。

第六轮的布局审计和截图使用断路器排序修复之前生成的工程（Linux 下顺序为储能、并网、负荷）；补修后的布局审计改用修复后重新生成的工程，结果相同。MCCB 页面未单独截图。

截图已用补修后的代码重拍。核对时发现：第六轮交付的截图是由最终提交前的一个中间构建渲染的，与 `7377901` 实际渲染结果有亚像素级的文字位置差异（内容相同）；另外截图脚本按顺序渲染多页，Control-Pcs 渲染时测试数据已超过 5 秒，PCS 远程/就地守卫把按钮显示成灰态，所以第六轮的 Control-Pcs“正常”截图实为“远程反馈过期”状态。本轮每页单独生成新鲜的测试数据，Control-Pcs 正常场景按钮可用、离线场景灰态。

截图（`--screenshot --preview-values`，图上标注“模拟验证·隔离测试数据·非104现场画面”）：Devices-Cells、Devices-Cells-Page9、Devices-Bms、Devices-Pcs、Control-Pcs、Topology-Single 的正常数据场景，以及 Devices-Cells、Control-Pcs、Devices-Bms 的离线/过期场景。

已知限制：

- 渲染环境是云端 Linux Qt 6 offscreen + Noto CJK 字体；设备上是 Allwinner Qt 5 和设备字体，字形宽度可能不同。设备实屏显示和触摸未验证。
- 直流侧小卡片里 6 位以上的电量（如 99999.9 kWh）会自动缩小字号；跨过 100000 kWh 改为 MWh 时字号会恢复，属于数值变化引起的字号变化，未改。
- 单体电压按通用格式去掉末尾 0（3.32 V 与 3.304 V 位数不同）。

## 5. 测试结果（模拟验证）

| 测试 | 结果 |
| --- | --- |
| `lanhu-scada.test.ps1`（56 页、绑定、共用探头只显示一次、PCS 模式设定标签、未开放按钮有原因） | 通过 |
| `lanhu-inventory.test.ps1`（两簇 34 节、稀疏配置、数量按配置） | 通过 |
| `generate-scada.test.ps1`（旧 XMind 34 页兼容） | 通过 |
| `local-controls.test.ps1` | 通过 |
| `lanhu-controls.test.ps1`（PCS/TMS/MCCB/BAU 控制绑定、数值范围、协议不符时不开放、Pack 不开放） | 通过。修复前在 `eeaaa8a` 的 Linux pwsh 7.4.6 下失败：断路器按主机区域设置排序，带遥控的并网断路器不在 `mccb-0`，测试在 MCCB 断言处中止，其后的 BAU 等检查未执行。修复后全部执行并通过；Windows 上未复核 |
| `local_display_qt_lanhu`（不带参数：含未知枚举原值、陈旧值仍为 `--`） | 通过 |
| `local_display_qt_lanhu` 带工程参数（运行窗口、登录门禁、单机拓扑模式对、退出/重新登录后锁仍有效） | 第六轮未执行；补修时执行发现组合状态回退，修正后通过（第 6 节） |
| `local_display_qt_control_receipt`、`local_display_qt_pcs_receipt`、`control_transaction_registry` | 通过（补修新增/扩充） |
| `--layout-audit` | 448 次渲染，0 项（补修后用重新生成的工程复跑） |

## 6. 接收复核后的画面补修

| 问题 | 页面 | 修正 | 测试 |
| --- | --- | --- | --- |
| PCS 三相均为“仅提交”时汇总显示“三相协议写入成功，其中 3 相未经回读确认” | Control-Pcs 分相功率 | 汇总按执行失败、结果未知、无最终回执、已下发未确认、协议写入未确认、本地参数已提交、反馈确认分别计数，例如“A/B/C 三相：3 相已下发、未确认（仅交给输出或通信，设备是否动作未确认）” | `control_receipt_view`（混合七类）、`local_display_qt_pcs_receipt`（同类三相、混合）、复核者夹具修复前失败/修复后通过 |
| PCS 控件销毁（退出登录、登录过期、工程重载）后三相锁永不过期；普通回执框随页面销毁会提前解锁 | Control-Pcs 及所有带控制的页面 | 锁移到进程级事务表，确定回执或 70 秒截止解锁；窗口刷新结算晚到回执；重建后的控件和页面不能绕过 | `control_transaction_registry`、`local_display_qt_control_receipt`（页面销毁、截止、新旧事务）、`local_display_qt_pcs_receipt`（销毁、重建控件被拒、截止）、`local_display_qt_lanhu` 带工程参数（退出/重新登录）、复核者 70 秒夹具修复前失败/修复后通过 |
| 第六轮把状态无匹配时的默认文字改为“未知”，组合状态（单机拓扑并网/离网两路反馈）的无效组合显示“未知” | Topology-Single 等组合状态 | 只有单一来源的未定义枚举显示“未知(原值)”，组合状态无效组合恢复“--” | `local_display_qt_lanhu` 带工程参数，修复前失败/修复后通过 |
