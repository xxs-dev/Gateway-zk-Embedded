# realese1.0 驱动审查剩余问题闭环（第五轮）

日期：2026-10-30。基线 `realese1.0` = `80b11bf`（GitHub、GitLab 一致）。承接[驱动审查回复与合并验证](driver-review-integration-20261030.md)。

状态只用四类：**代码已修**、**模拟已验证**、**现场待验证**、**固有限制**。模拟测试只说明代码按设计工作，不代表厂家语义、硬件或现场行为，不作现场放行依据。本文所列事项均未验收。

本轮未连接任何设备或平台，未发启停、分合闸、复位、功率或校时命令；未修改 `config/runtime`、`products/**` 现场配置、MQTT 现场配置、证书、账号、EasyTier、4G 和网络；未部署。

## 提交

| 提交 | 内容 |
| --- | --- |
| `6defbb0` `a0b5314` | AGC/AVC 命令信箱按入口和服务端策略授权；文档与迁移 |
| `831e90b` `d895b45` `f4252a6` `640184c` | MQTT 默认 TLS、拒绝明文、取消跳过校验；部署脚本与预检；文档 |
| `956dc23` `c24cc8e` `b927f3c` | `writeback-submitted`；CAN 可配状态信号确认；DO 输出状态与触点反馈分开 |
| `86fbfd9` `2f8539e` | IEC103 遥信保留原始 DPI 与映射依据；AM5SE 证据清单与只读验证步骤 |
| `5b67d88` `09391f8` | RS-485 读回核对修正与逐项报错；逐串口只读核验方法 |

## 问题状态

| 问题 | 代码已修 | 模拟已验证 | 现场待验证 | 固有限制 |
| --- | --- | --- | --- | --- |
| 一、AM5SE 变位 DPI | 点值 `text` 追加 ASDU/COT/FUN/INF/DPI 八位组/解释值/质量/映射名和 `verified=false`，`rawHex` 存整个 ASDU；DPI=0/3 及备用位非 0 的八位组为坏质量、保持旧值。**映射未改**（`0x01`→分 0，`0x02`→合 1） | `iec_codec`、`iec_collector_incremental`、`am5se_iec103_codec`（构造帧，只验证解码与诊断保留） | DPI 极性、ASDU44 状态位与实际分合的对应；仓库内无厂家规约、固件说明或真实 ASDU 1/2 报文，证据不足 | 共享内存只存值/质量/时间，诊断文本只在日志（`GATEWAY_IEC103_DEBUG=1`） |
| 二、RS-485 | 读回改为与未经内核改写的请求副本比较（原实现与被内核原地回写过的同一结构比较，内核未采纳的设置不会被发现）；读改写保留未知位；`RX_DURING_TX` 随 `localEcho`；逐字段报错；配置类型与适用协议校验。默认关闭不变 | `posix_serial_rs485`：关闭时不发 ioctl、不支持（ENOTTY）、设置失败（EPERM/EIO/EINVAL）、读回失败、逐字段读回不一致、重开重新设置并核对 | 每路串口的方向机制（硬件自动/设备树/RTS/GPIO）；生产配置未改 | `enabled=false` 不清除内核里已有设置；通信正常不能证明方向机制 |
| 三、CAN 与 DI/DO 确认语义 | 新阶段 `writeback-submitted`（outcome=succeeded，`submittedOnly`）；CAN 可配 `write.verifyIndex` 由设备状态信号确认；DO 锁存回读只算“输出状态”，不再置 `verifyAttempted/verifyPassed`；独立辅助 DI 才是触点反馈；未配置反馈在回执中写明；坏质量/过期/旧值不算反馈；不一致与超时为 unknown；晚到反馈不改写回执；不自动重发 | `can_dio_control_outcome`（去重账本、共享内存回执、MQTT 首次回执、同 cmdId 重放、晚到反馈一致）、`dio_driver_review`、`can_driver_runtime`、`can_signal_layout`、`control_dedup_*`、`command_receipt_observation` | 真实 CAN 设备是否有可用状态信号、bus-off/restart-ms；DO 辅助触点接线及反馈时延 | SocketCAN 发送成功只表示内核接受；无独立反馈不能确认设备动作；跨设备反馈在优先控制租约期间可能等不到新值；CAN 无持续不一致告警 |
| 四、AGC/AVC source 授权 | 授权只取决于入口和 `agcAvc.commandMailbox` 策略（`platformDispatch`、`maintenanceShadowTest`，默认均 false）；载荷 `source` 只作审计；latest 槽 `reserved[0]` 记入口，普通写入清零；AgcAvcController 读取时复核；高优先级、非命令点、保留消息、EMS 就地模式均拒绝；SCADA/Graph EMS/计算引擎/转发/`pointctl --app-config`/采集驱动写邮箱点一律拒绝 | `agc_avc_mailbox_authorization`：伪造 source、策略关闭、入口不匹配、缺失入口、正常授权、路由/latest/批量写绕过、普通写清标记后消费者拒绝、直连维护四种场景；`agc_avc_controller` | 平台下发联调；broker 账号与 ACL 是否只允许平台发布本机命令主题 | 服务均以 root 运行，root 进程可直接改共享内存或伪造标记；控制权/优先级锁文件 0664；MQTT 入口身份只能依赖 broker 认证和 ACL |
| 五、MQTT 明文默认 | 出厂模板与示例改为 TLS（broker 为空、`tls.enabled=true`、`caFile` 指向部署路径）；明文默认拒绝且不建 socket；`allowInsecurePlaintext` 实验开关默认 false、开启时持续告警；`insecureSkipVerify` 取消；TLS 1.2+、证书链、域名 SAN+SNI、IP SAN 校验；TLS 失败不回落明文；缺 CA/坏 CA/无信任库/缺客户端密钥时拒绝；初始化脚本拒绝明文与跳过校验；`production-smoke-test.sh --mqtt-transport` 只读预检 | `mqtt_tls_transport`（本机 OpenSSL 3.0 + 临时证书测试 broker：正常 TLS、错误 CA、主机名不符、IP 不符、证书过期、双向 TLS、禁止降级、明文拒绝、实验开关）、`mqtt_transport_deploy_policy`、`builtin_mqtt_driver_publisher`、`mqtt_forwarder_service`、`config_corpus_load` | 目标板 OpenSSL 1.1、真实 broker 与平台签发证书；现有明文现场的迁移 | 不支持证书固定、CRL/OCSP、PKCS#12、带口令私钥、WebSocket；证书过期判断依赖设备时间 |

## 原交付阶段测试结果

以下为原交付报告的云端验证记录；内网接收验证单独记录在后文，不混用测试数量与运行环境。

| 验证 | 结果 |
| --- | --- |
| x86 GCC 13.3 Release 全目标构建（合并后源码树，与最终提交的代码一致） | 通过 |
| CTest 全量 | 155/155 通过，测试耗时合计 390.7 s；在 `unshare -fp --mount-proc` 中运行，`GATEWAY_MIGRATION_TEST_BACKUP_DIR` 指向专用空目录，`QT_QPA_PLATFORM=offscreen`；`mqtt_tls_transport` 实际执行，未跳过 |
| `config_corpus_load` | 33 个设备配置、11 个应用配置加载，0 失败 |
| 云端 aarch64 交叉编译 | 19/19 生产目标通过；工具链为 Ubuntu `aarch64-linux-gnu-g++-6` 6.4.0，不是 AGENTS.md 指定的 Linaro 6.3.1，仅作编译/链接检查，产物不可部署（见下一节） |
| 22.11 独立目录交叉编译（边端 + Qt） | 原云端交付阶段未执行；内网接收阶段已补验，见后文。报告与产物哈希保存在本机验证目录，不在仓库内 |
| 目标 ARM 运行、Qt 运行、真实设备、平台联调 | **未执行** |

### 云端 aarch64 编译

`tools/build_edge_aarch64.sh --clean --build-dir <checkout>/build-aarch64-r5 --jobs 2`，源码树与最终提交的代码一致（只差本文档）：19 个生产目标全部编译链接成功，退出码 0，无编译警告，产物均为 AArch64 ELF。工具链是 Ubuntu GCC 6.4.0 交叉编译器（默认生成 PIE），与 22.11 的 Linaro 6.3.1 和全志 sysroot 不同：这只说明合并后的源码能被 GCC 6 系列编译，不代替 22.11 编译，产物不回传、不可部署。Qt SCADA 需要全志 sysroot，云端未编译。

## 2026-10-31 内网接收验证

导入并核对交付末端 `2df66deab7f8f772602fa67a1fa21ee159ef1276`，以 `80b11bf` 为共同基线。在指定构建机 `192.168.22.11` 的独立目录 `/tmp/gateway-build-r5-20261030163913/` 上传 Git archive，并按 Git 对象哈希和执行位逐一核验 3170 个受控文件。未操作共享 `/srv/build/Gateway-zk`，未连接现场设备或部署产物。

| 验证 | 本轮实测结果 |
| --- | --- |
| Allwinner AArch64 Release 构建 | 指定 Linaro GCC 6.3.1 与全志 sysroot；19 个生产目标全部编译链接通过 |
| Qt AArch64 构建 | KY-SCADA 和 3 个 Qt 测试程序编译链接通过；未运行 Qt 测试或截图验收 |
| 产物回传 | 23 个 AArch64 ELF 已回传本地，重新读取文件并逐个核对 SHA256，全数一致 |
| 原生 Debug 构建 | GCC 7.5 全目标及单独的 `ems_control_mode_test` 构建通过 |
| 原生普通测试 | 首次 150 项中 149 项通过，401.11 秒；唯一失败是 `mqtt_transport_deploy_policy` 的 Python 3.6 兼容问题。修复后该项专项测试通过；最终 CTest 重跑 `mqtt_tls_transport`、`mqtt_transport_deploy_policy` 均通过 |
| 迁移测试 | `memory_point_store_migration`、`memory_point_store_v11_migration` 在独立 PID/proc、mount、IPC、network 环境和专用备份目录运行，两项均通过 |
| 覆盖结论 | 152 个唯一原生测试均取得通过结果，包含修复后的复测，不是一次全量 152/152 无失败。该环境缺少原生 Qt 开发库，不含原交付 155 项中的 3 个 Qt 运行测试 |
| TLS 环境 | Python 3.6.9、OpenSSL 1.1.1，TLS 专项实际运行，未跳过；不等于目标 ARM 板验证 |

内网验证发现并补修两处兼容问题：

- `9eaa265`：测试 broker 在 Python 3.6 没有 `SSLContext.sni_callback`，改为检测 API 并回退到 `set_servername_callback`。复现失败后，完整 TLS 专项通过。
- `78889a9`：生产预检使用 Python 3.7 才支持的 `subprocess.run(capture_output=True)`，改为显式 `stdout/stderr=subprocess.PIPE`；专项测试增加有效 CA 必须通过 30 天有效期检查的断言，防止只检查文件存在而漏检有效期。

23 个二进制产物的原始构建提交仍为 `2df66de`，原报告不改写。后续差异仅为上述测试脚本、生产预检脚本及本文档，没有修改编入产物的 C++ 或构建配置；最终源码审计以 `final-source-audit.json` 记录接收末端提交与构建提交的差异。

本地证据目录：`D:/workspace/new/driver-r5-20261031/`。包括 `report/`、`artifacts.local.sha256.json`、`ordinary-initial-LastTest.log`、`mqtt-recheck-LastTest.log`、`migration-LastTest.log`、`finish_verify.log`、`final-source-audit.json`。初始失败记录保留，不删除或覆盖成通过结果。

目标 ARM 运行、Qt 运行、真实设备、平台联调均未执行。MQTT 明文拒绝及 AGC/AVC 平台调度默认关闭的行为未放宽；硬件语义和现场接线问题仍待现场验证，不因构建和模拟通过而标记验收。

## 行为变化与迁移

1. **MQTT**：现场 app 配置仍为 `tcp://` 且未设 `allowInsecurePlaintext=true` 的设备，升级后所有 MQTT 客户端拒绝连接，平台侧离线（本地采集、控制、画面、直连维护、EasyTier 不受影响）。升级前必须执行 `GATEWAY_HOME=/opt/modbus-gateway sh <新包>/deploy/production-smoke-test.sh --mqtt-transport`，失败时按[MQTT TLS 接入说明](../部署运维/边端MQTT%20TLS接入说明.md) §6 迁移到 TLS（推荐，旧版本可先完成），或临时加实验开关。`mqttForward.allowInsecurePlaintext` 只能在升级后添加（旧版本会拒绝未知字段）。IP 方式接入需要服务端证书带 IP SAN。仓库内实验室快照 `config/runtime/apps/*.json` 仍为明文，未改，新程序会拒绝其连接。
2. **AGC/AVC**：经 MQTT 下发调度的项目须在 `agc-avc-service.json` 的 `agcAvc` 下增加 `"commandMailbox": {"platformDispatch": true}` 并确认 broker ACL，否则回执 `control-rejected`。直连维护影子注入改为 `maintenanceShadowTest=true`。升级时须同时停止所有写邮箱共享内存的服务（旧程序写 latest 不清 `reserved[0]`；升级前的邮箱值标记为 0 会被拒绝）。EMS 就地模式拒绝的 MQTT 控制回执 `stage` 由 `rejected` 改为 `control-rejected`（outcome 仍为 failed）。共享内存槽大小与布局不变，只新增 `reserved[0]` 的语义。
3. **控制回执**：未配置反馈的 CAN 写和 DO 写由 `writeback-succeeded` 变为 `writeback-submitted`（success=true，outcome=succeeded，`verifyPassed=false`）。需要“设备已执行”的消费方须看 `verifyPassed=true`。平台、KY-EMS 对新 stage 的展示未改。以本机 DO（未配 `verifyIndex`）为目标的 EMS `confirmedControlWrite` 会因此由“确认”变为故障停止，见下节。
4. **IEC103**：备用位非 0 的 DPI 八位组由“取低两位解释”改为坏质量保持旧值；若真实装置置备用位，相关点会显示坏质量（原始八位组在日志和 `qualityMsg` 中可见）。
5. **RS-485**：仅对显式 `enabled=true` 的端口生效；读回与请求不一致时打开串口失败并逐字段报错，不降级运行。

## 需要用户决定

- AGC/AVC：发货模板是否默认开启 `platformDispatch`（当前 false）；是否需要可执行的本地 AGC/AVC 入口（当前本地只有经维护口令鉴权的影子注入，SCADA/本地画面一律拒绝写邮箱）。
- MQTT：现场是否采用 stunnel；OTA 配置包是否禁止打开 `allowInsecurePlaintext`；`caFile` 为空时是否禁止使用系统信任库；`mqttForward` 的 CA 规则；实验室 `config/runtime` 快照如何处理。
- 控制回执：`writeback-submitted` 维持 outcome=succeeded 是否接受；平台/KY-EMS 如何展示“仅提交”；本地画面把 unknown 显示为“驱动执行失败”的文案需另行修正。EMS `confirmedControlWrite` 要求目标点配置 `verifyAfterWrite`+`verifyByRead`，且回执 `verifyAttempted`/`verifyPassed` 为 true：本机 DO 未配 `verifyIndex` 时，以前锁存回读即被当作确认，现在回执为 `writeback-submitted`，节点会以 -1 故障停止；CAN 不支持 `verifyAfterWrite`，不能作为该节点目标。仓库发货配置未使用该节点（只有测试夹具）。是否让它改为接受 `verifyIndex` 确认需决定。
- AM5SE：映射经现场确认后如何在配置或代码中标记“已确认”。
- RS-485：DE 接 RTS 且设备树已启用时，是保持关闭还是由驱动设置并读回。

## 剩余风险

- AM5SE ASDU44 的 QDS 仍未纳入点值质量。
- 部分驱动按字节截断 UTF-8 消息（本轮 CAN/DIO 已控制在 127 字节内，其他驱动未统一处理）。
- 实验室 MQTT 口令曾出现在仓库文档中，本轮已从文档删除，但仍在 Git 历史里，建议轮换该账号口令。
- 22.11 Linaro 6.3.1 与 Qt 交叉编译已在内网接收阶段通过；目标板运行、Qt 运行及现场/平台联调仍未执行。

## 需要用户提供的材料

- AM5SE：装置型号与固件/程序版本、厂家 IEC103 规约与 FUN/INF 地址表；163 现场抓包（仓库外 `D:/workspace/new/am5-soe-20261029/`）以及按只读步骤记录的变位报文与面板位置对照。
- RS-485：各路串口原理图（收发器型号、DE/RE 接法）、设备树、内核配置与 BSP 串口驱动源码。
- CAN：设备协议中可作为写入确认的状态信号定义。
- DI/DO：DO 与辅助触点接线图。
- MQTT：broker 的 TLS 监听地址、服务端证书 SAN、CA 证书及 ACL 配置（由现场按迁移步骤部署，本仓库不保存）。

## 现场只读验证入口

- AM5SE：[安科瑞 AM5 IEC103 主动 TCP 现场验证](../协议与驱动/安科瑞AM5-IEC103主动TCP现场验证.md)“最小只读验证步骤”。
- RS-485：[RS-485 收发方向逐串口核验](../部署运维/RS-485方向逐串口核验.md)。
- CAN / DIO：[CAN 接入设计](../协议与驱动/CAN接口接入设计.md)“写入结果与能力边界”，[本机 DIDO 接入设计](../协议与驱动/本机DIDO接入设计.md)“输出状态与触点反馈”。
- AGC/AVC：[AGC/AVC 独立模块设计](../需求说明/边端AGC_AVC独立模块设计.md)“命令邮箱授权”。
- MQTT：[MQTT TLS 接入说明](../部署运维/边端MQTT%20TLS接入说明.md) §6、§7。
