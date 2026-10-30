# realese1.0 评审修复记录（2026-09-29）

本文对应评审报告 `realese1.0-代码风险与上线差距评审-20260929.md`，编号与报告一致。

修复原则按确认的方案执行：**默认安全，可配置回退**。凡是会改变与平台端或现场配置契约的项，都新增了配置开关，默认取安全值；平台端或现场完成配套改造之前，可以在本地配置里临时放开。

## 1. 验证情况

| 项目 | 结果 |
|---|---|
| x86_64 Debug 全量构建（GCC 13，`BUILD_TESTING=ON`） | 通过，无新增告警 |
| ctest（103 项） | **99 通过**，4 项失败与基线一致，均为环境原因，与本次改动无关：<br>• `memory_point_store_v11_migration` 需要隔离的备份目录；<br>• 3 个 `gateway_test_lab_*_e2e` 的脚本本次未同步到构建机 |
| 修复前基线（同一环境） | 同样是这 4 项失败 |
| 独立复核 | 另起一轮对抗式审查，专门找本次改动引入的回归，共发现 19 项，已全部修复后再次构建测试 |
| 脚本 | 改动的 `deploy/*.sh` 均通过 `sh -n` 与 `dash -n`（无 `local`、数组、`[[`）；`ota_service_test`、`gateway_health_watchdog_test.sh` 通过；OTA apply 与 rollback 在假 systemctl 沙箱中跑通 |
| **未验证** | • AArch64 交叉编译（须在 192.168.22.11 执行 `tools/build_edge_aarch64.sh`）；<br>• Qt 相关改动（本机没有 Qt，只做了人工逐行检查）：`local_display_qt_*`、`qt_display_bridge_main.cpp`；<br>• 目标板冒烟测试与硬件在环测试 |

`pthread_mutex_clocklock` 做了 glibc ≥ 2.30 的条件编译；若工具链 glibc 更老，会自动退回原来的 `timedlock` 实现。

## 2. 各项修复状态

状态说明：✅ 已修复；◐ 部分修复；✗ 未修复。

### 2.1 MQTT 与控制链路

| 编号 | 状态 | 修复要点 |
|---|---|---|
| M1 | ◐ | MQTT 下发的 `highPriority` 默认拒绝，开关为 `allowRemoteHighPriority`。`priority`/`priorityControl` 两个别名只认 JSON 布尔值，其他取值一律忽略。`source` 仍取自报文，因为 SCADA 和 AGC 桌面工具依赖它；但仅伪造 `source` 拿不到控制权，还需要匹配的 generation。根本解决要靠 broker ACL。 |
| M2 | ✅ | 默认拒绝 retained 命令。`ts` 超过 ±5 分钟时钟偏差即拒绝；支持数字字符串和秒级时间戳。路由层判断租约改用网关受理时间 `acceptedAt`，不再用报文里的时间。 |
| M3 | ✅ | 命令改用严格的 JsonParser：只取根对象字段，重复键拒绝，`index` 必须是整数，`value` 必须是有限数。 |
| M4 | ✅ | 写值先换算成原始值，再按 int16/uint16/int32/uint32/float32 的取值范围检查，越界直接拒绝，不再回绕。 |
| M5 | ✅ | 去重库新增 `created_ms` 列和索引；已完成的回执保留 24 小时后清理，接近容量时淘汰旧回执；未完成的记录永不删除。旧库自动迁移。 |
| M6 | ✅ | 检测 PINGRESP 超时后主动重连，半开连接可以被发现。 |
| M7 | ✅ | 超大报文只读取 topic 和 packetId，其余字节丢弃，照常回 ACK，连接不断开；同一批中已 ACK 的消息不会丢。MQTT5 CONNECT 声明 Maximum Packet Size。 |
| M9 | ✅ | AGC 邮箱写入前校验值有限及 min/max/step；限流独立计算（`mailboxRateLimitPerWindow`）；不做 cmdId 去重，邮箱有自己的序号。 |
| M10 | ✅ | 重连指数退避加抖动（1 s 到 30 s）；"订阅不可用"状态只在状态切换时发一次。 |
| M11 | ✅ | 每个报文有独立的读取截止时间；检查 `SSL_pending`，TLS 已缓冲的数据不会被延迟处理。 |
| M12 | ✅ | SUBACK 逐个 topic 对应检查返回码：命令 topic 被拒时抛错重连，可选 topic 被拒只记日志。 |
| M13 | ✅ | 租约文件通过旁路锁文件加 flock 串行化，release 在锁内重新读取后才删除。 |
| M14 | ✅ | 修正空 payload 越界、MQTT5 属性长度上限、CONNACK 解析位置、ring buffer 长度溢出与 wrap 标记越界。 |
| M15 | ✅ | 该实现未进任何构建目标、没有程序使用，已整体删除（`mosquitto_mqtt_driver_publisher.*`，2026-10-28 驱动审查修复）。 |

### 2.2 协议驱动

| 编号 | 状态 | 修复要点 |
|---|---|---|
| P1 | ✅ | 保留 COT 中的 P/N 位与 T 位；收到否定确认时，控制命令和参数写都判为失败。 |
| P2 | ✅ | 每次选择或执行前，清掉该地址遗留的确认；确认时校验回显值和 S/E 位；等待改为条件变量。 |
| P3 | ✅ | `send` 加 `MSG_NOSIGNAL`；四个驱动的 main 都忽略 SIGPIPE。 |
| P4 | ✅ | IEC101/103 一次召唤后没有收到任何有效应答（可变帧、固定帧或 E5）即判离线。 |
| P5 | ✅ | 实现 t1 监督（未确认 I 帧、TESTFR 超时即断开）；收到的 N(R) 越界按协议错误断开。 |
| P6 | ✅ | 解析品质描述词，IV/NT 置位或值非有限时 quality=0。 |
| P7 | ✅ | 新增读数据类型 `dlt645_bcd_signed`，以最高字节 bit7 为符号位。原 `dlt645_bcd` 行为不变，现场需要按表计手册把有符号量改成新类型。 |
| P8 | ✅ | 读响应必须来自被寻址的表，控制码必须是 91H/B1H/D1H；发送前清空接收缓冲。 |
| P9 | ✅ | 已结束的客户端线程及时回收；线程创建失败只关闭该连接，不影响整个服务。 |
| P10 | ✅ | I 帧发送串行化，N(S)/N(R) 在发送前统一分配。 |
| P11 | ✅ | 监听通配地址且白名单为空时拒绝启动（开关 `allowUnrestrictedClients`），报 ERROR 日志并发状态事件 `northbound-refused`。空闲超时与请求超时分开，防慢速连接占满。 |
| P12 | ✅ | CAN 按点单独解码，一个信号出错只影响它自己（该点 quality=0）。 |
| P13 | ✅ | 写入值超出位宽直接拒绝，并支持读侧的 `signed` 标志。同帧其他信号的保留改为可选（`protocol.can.preserveFrameSignals`，默认关闭，即沿用原来从零组帧的行为）。2026-10 驱动复审：开关已删除，改为固定按发送帧镜像保留同帧信号（初值 `payloadTemplate`），见 CAN接口接入设计.md。 |
| P14 | ✅ | 只在超时时原样重传同一帧（FCB 不变），3 次仍超时则复位链路。 |
| P15 | ✅ | `rxBuffer_` 只由接收线程修改，其他线程通过标志请求清空。 |
| P16 | ◐ | 先解析波特率再打开串口，配置完成后清空输入缓冲；配置加载阶段未增加波特率校验。 |
| P17 | ✅ | DIO 用 `high`/`low` 一次写入方向和初值，避免上电瞬间的低电平毛刺。 |
| P18 | ✅ | 北向输出值越界时返回 Modbus 异常 0x04，不再回绕。 |
| P19 | ✅ | 新增 `transport.localEcho`，开启后丢弃本机发送的回显字节。 |
| — | ◐ | IEC104 总召唤间隔 `interrogationIntervalMs`，默认 0，保持原行为。 |
| — | ✅ | CAN 解码出 NaN/Inf 时判为无效数据。 |

### 2.3 共享内存与存储

| 编号 | 状态 | 修复要点 |
|---|---|---|
| S1 | ✅ | EOWNERDEAD 重置时序号不回退；被丢弃的待写命令逐条写失败回执 `owner-died`。 |
| S2 | ✅ | 写命令超过 `pendingWriteMaxAgeMs`（默认 10 s）即判过期，写回执 `expired`，不下发设备。CAN 驱动的写循环未接入这项检查，见第 4 节。 |
| S3 | ✅ | 不同 segment 出现相同 index 时默认拒绝加载（开关 `allowDuplicateIndexAcrossSegments`）。打开开关后，只按 index 下发的写命令遇到有歧义的 index 会被拒绝。 |
| S4 | ✅ | 时间超前 5 s 以上的心跳判为失效；owner 的 pid 已不存在时立即释放占用。 |
| S5 | ✅ | 告警和变化事件跳过 stale、坏质量和非有限值（开关 `evaluateInvalidValues`）。 |
| S6 | ✅ | 历史样本分批写入分批 ack；告警待写队列上限 1 万条，每批最多 256 条。 |
| S7 | ✅ | attach、remap 和每次拿锁后都校验环形游标；无法解析的写命令丢弃并写回执 `malformed-rejected`。 |
| S8 | ◐ | 磁盘预算按实际占用计算，超出时先删最旧的已发送行；写入前检查磁盘剩余空间。journal 模式未改，因为该库由多个进程共用。 |
| S9 | ✗ | 读不存在的 index 仍会全表扫描，属于性能优化，未做。 |
| S10 | ✅ | 锁等待超时改用 `CLOCK_MONOTONIC`，要求 glibc ≥ 2.30，否则退回原实现。 |
| S11 | ✅ | 发现 segment 初始化了一半时自动补做初始化。 |
| S12 | ◐ | 心跳和过期清理路径会检测 segment 是否被替换并重新映射；热路径 `putLatest` 未改。 |
| S13 | ✗ | 保持型虚拟点每次值变化都 fsync 的问题未处理，属于可选优化。 |
| S14 | ✅ | index 绑定改用 `bind_int64`；实例析构时不再 `dlclose`。 |

### 2.4 EMS 控制逻辑

| 编号 | 状态 | 修复要点 |
|---|---|---|
| E1 | ✅ | `pcsPowerSolve` 在以下情况把对应方向的 P/Q 置 0，与 `powerConstraint` 的保护逻辑一致（开关 `failSafeOnMissingLimits`）：<br>• SOC、SOC 上下限、功率上限、BMS 允许功率这些实时遥测点陈旧、坏质量或非有限；<br>• SOC 上下限倒置。<br>永不过期的 EMS 虚拟参数不会触发置 0。置 0 时打日志，并可输出到故障点。<br>interval 规则的图输出 TTL 改为 max(`graphOutputTtlMs`=5 s, 3 个周期)，变化触发的规则沿用 `defaultOutputTtlMs`。<br>重启恢复状态时不再以好质量重发功率设定值。 |
| E2 | ✅ | 不带令牌、值为 0、目标在 `controlTargetIndexes` 内的写入不需要集群授权。 |
| E3 | ✅ | 对已启用但不可用的 PCS 持续下发 0。局限：这台 PCS 原有的份额仍会分给其他机组。 |
| E4 | ✅ | 墙钟回拨后规则调度重新锚定，不再停摆；时间戳超前 1 s 以上视为无效。 |
| E5 | ✅ | 取值和写值时过滤非有限值；`clampAbs(NaN)` 返回 0。 |
| E6 | ✅ | rateLimit 可用于一步的时间间隔封顶（`graphRateLimitMaxGapMs`）；AGC 同样处理。 |
| E7 | ◐ | `clusterDispatch` 新增 `commandSign`（默认 1，保持现状），未显式设置时启动告警。现场需要确认正负号约定。 |
| E8 | ✅ | 负的允许功率按 0 处理，缩放系数限制在 [0,1]，充放电方向不会被翻转。 |
| E9 | ✅ | 图节点出错计入健康状态（开关 `graphNodeErrorsUnhealthy`）。 |
| E10 | ✅ | 状态文件跳过非有限值；fsync 后再 rename，并 fsync 所在目录。 |
| E11–E13 | ✗ | 属于可选项，未做：对端地址学习顺序、设定值周期重申与停机写 0、六相原子写。 |

### 2.5 OTA、部署与运维

| 编号 | 状态 | 修复要点 |
|---|---|---|
| O1 | ✅ | OTA 签名规范为 `edge-ota-v1`，默认要求签名（`ota.requireSignature`），详见 `OTA签名说明.md`。默认禁止 http 下载；curl 加 `--proto =https --proto-redir =https --max-filesize`。OTA 包只能收紧安全配置，不能放宽。 |
| O2 | ◐ | 服务重启后做健康检查，不健康就自动回滚，结果写入历史日志并恢复版本标记。但 C++ 侧在健康检查之前就上报了 `completed`，回滚结果不经 MQTT 上报。 |
| O3 | ✅ | 持久化的进行中标记，加开机恢复单元 `gateway-ota-recover.service`；OTA 安装该单元时自动 enable。 |
| O4 | ✅ | jobId 只能使用一次，但在改动运行时之前失败的任务允许重试；不再删除已有备份。 |
| O5 | ✅ | 出厂包排除 `*.csv`、`*.key` 等敏感文件，打包后再检查一遍。 |
| O6 | ✅ | 删除硬编码的组网密钥，改为必须从环境变量或文件读取。 |
| O7 | ✅ | 拒绝带 setuid/setgid 位的文件；按源文件是否有可执行位决定目标权限：可执行 0755，普通文件 0644，私钥 0600。 |
| O8 | ✅ | 服务重启序列改在 `systemd-run` 创建的独立单元中执行。 |
| O9 | ✅ | 下载异常时先终止 curl 子进程。 |
| O10 | ✅ | 剩余空间检查改为 3 倍包大小；staging/backup 目录和 SCADA 历史版本按数量保留。 |
| O11 | ◐ | TLS 注册默认要求 https，返回的 CA 校验后原子写入。设备本地保存根 CA 私钥的逻辑未改。 |
| O12 | ◐ | 11 个 unit 加了 `NoNewPrivileges`。被升级守卫按 SHA256 固定的 unit 未改，也未加 `ProtectSystem`/`WatchdogSec`。 |
| O13 | ✅ | 已退役的滚动升级工具直接拒绝运行。 |
| O14 | ✅ | 临时文件改用 `mktemp`；stop 失败时保留 manual-stop 标记。 |
| O15 | ✅ | 版本标记原子写；私钥用 `install -m 0600` 安装；密码不再出现在远程命令行；`--clean` 限制只能删除仓库内的构建目录。 |

### 2.6 监测、维护与显示

| 编号 | 状态 | 修复要点 |
|---|---|---|
| D1 | ✅ | 维护口令改为每台设备独立凭据（加盐 PBKDF2-HMAC-SHA256，12 万次迭代）。<br>• 固定哈希默认不再接受（`allowLegacyFixedPassword`）；<br>• 未设置凭据时敏感接口返回 503，并要求 `sensitiveApiEnabled`；<br>• 白名单为空时拒绝启动；<br>• 连续失败后锁定该 IP；<br>• 新增命令 `SystemMonitor --set-maintenance-password` 设置口令。 |
| D2 | ◐ | 自定义 camera `command` 默认拒绝，远程下发不能开启该开关；ky-ems 文件不再免校验；配置拉取时密钥脱敏，下发时按 JSON 路径还原。配置下发签名未做。 |
| D3 | ✅ | ConfigLoader 和 SCADA 解析器加深度与节点数上限。 |
| D4 | ◐ | 模态框打开期间不重建界面；PCS 确认框用 QPointer 保护。写入前重新校验登录状态未实现。 |
| D5 | ✅ | 共享内存权限默认 0600（可用 `--shm-mode` 调整），启动时用 IPC_STAT 检查属主和权限。 |
| D6 | ✅ | AT 探测默认关闭；自动排除已配置设备使用的串口；探测时独占端口，结束后恢复原串口参数。 |
| D7 | ◐ | 增加心跳来源限制的开关，但默认保持旧行为，避免上位机未改造时误触发离线安全动作。 |
| D8 | ✅ | 检测墙钟跳变，跳变后重新计宽限期，不会误触发。 |
| D9 | ✅ | 每个请求有总超时；头部上限 16 KB，请求体上限 16 MB。 |
| D10 | ✅ | 租约和分片会话按时间清理，并限制数量与总字节数。 |
| D11 | ✅ | 配置写入 fsync 后原子替换，只保留最近 10 份备份；camera 和 system monitor 的 main 加顶层异常捕获。 |
| D12 | ✅ | HMI 写标志未被消费前，不用回读值覆盖该槽位。 |
| D13 | ◐ | 登录失败锁定，时长逐次翻倍；密码算法未改，否则现有账号记录会失效。 |
| D14 | ✅ | 整数和端口做范围校验；重复的 cameraCode 报错。 |
| D15 | ✅ | 修正状态 JSON 格式；本地显示服务限制连接数并设置超时；camera 子进程独立成进程组，停止时整组发信号。 |

## 3. 新增配置项（默认值即安全行为）

| 配置路径 | 默认值 | 作用 |
|---|---|---|
| `mqttDriver.allowRemoteHighPriority` | false | 是否允许 MQTT 下发高优先级命令 |
| `mqttDriver.rejectRetainedCommands` | true | 拒绝 retained 命令 |
| `mqttDriver.commandMaxSkewMs` | 300000 | 命令时间戳允许的最大偏差，0 表示不检查 |
| `mqttDriver.requireCommandTimestamp` | false | 是否要求命令必须带 ts |
| `mqttDriver.mailboxRateLimitPerWindow` | 1000 | AGC 邮箱限流，0 表示不限 |
| `mqtt.reconnectBackoffMinMs` / `reconnectBackoffMaxMs` | 1000 / 30000 | 重连退避的起始值与上限 |
| `northboundServer.allowUnrestrictedClients` | false | 允许监听通配地址且不设白名单 |
| `northboundServer.idleTimeoutMs` | 60000 | 北向连接空闲超时 |
| `protocol.iec.interrogationIntervalMs` | 0 | IEC104 总召唤间隔，0 保持原行为 |
| `protocol.can.preserveFrameSignals` | 已删除 | 2026-10 驱动复审改为固定按发送帧镜像保留同帧信号 |
| `transport.localEcho` | false | 丢弃本机回显字节 |
| `memoryStore.pendingWriteMaxAgeMs` | 10000 | 待执行写命令的最大时长 |
| `memoryStore.persistFlushBatchSize` | 1000 | 历史样本每批写入条数 |
| `memoryStore.allowDuplicateIndexAcrossSegments` | false | 允许不同 segment 使用相同 index |
| `eventEngine.evaluateInvalidValues` | false | 告警是否评估坏质量值 |
| `offlineBuffer.eventOutbox.minFreeBytes` | 8 MiB | outbox 写入前要求的最小剩余空间 |
| `computeEngine.graphOutputTtlMs` | 5000 | 图输出 TTL（interval 规则） |
| `computeEngine.graphRestoreSetpointOutputs` | false | 重启时是否恢复功率设定值 |
| `computeEngine.inputMaxFutureSkewMs` | 1000 | 输入时间戳允许超前的时长 |
| `computeEngine.graphRateLimitMaxGapMs` | 2000 | rateLimit 单步可用的最大时间间隔 |
| `computeEngine.graphNodeErrorsUnhealthy` | true | 图节点出错计入不健康 |
| `emsCluster.allowZeroWriteWithoutAuthority` | true | 允许无集群授权写 0 kW |
| `agcAvc.zeroUnavailablePcs` / `rateLimitMaxGapMs` / `inputMaxFutureSkewMs` | true / 1000 / 1000 | 不可用 PCS 写 0；斜坡间隔上限；输入时间戳超前上限 |
| 图节点 `pcsPowerSolve.failSafeOnMissingLimits` | true | 限值缺失时 P/Q 置 0 |
| 图节点 `clusterDispatch.commandSign` | 1 | 集群调度正负号约定 |
| `ota.requireSignature` | true | OTA 必须带签名 |
| `ota.signaturePublicKeyPath` | `/etc/modbus-gateway/ota.pub` | OTA 验签公钥路径 |
| `ota.allowInsecureHttp` | false | 允许 http 下载 |
| `systemMonitor.directMaintenance.credentialFile` | `.../monitor-direct-maintenance-credential.json` | 维护凭据文件路径 |
| `systemMonitor.directMaintenance.allowLegacyFixedPassword` | false | 接受旧的固定口令哈希 |
| `systemMonitor.directMaintenance.allowAnyClient` | false | 允许白名单为空 |
| `systemMonitor.directMaintenance.requireSensitiveApiEnabled` | true | 敏感接口要求 `sensitiveApiEnabled` |
| `systemMonitor.directMaintenance.maxAuthFailures` / `authLockoutSec` | 5 / 300 | 失败锁定的次数与时长 |
| `systemMonitor.directMaintenance.maxRequestBytes` / `requestTimeoutMs` | 16 MB / 15000 | 请求体上限与请求超时 |
| `systemMonitor.directMaintenance.realtimeHeartbeatRequiresAuth` | false | 心跳接口要求认证（兼容考虑，默认关闭） |
| `systemMonitor.scadaUpperComputerSafety.heartbeatSessionPrefix` / `clockStepToleranceMs` | "" / 10000 | 心跳来源前缀；墙钟跳变容忍值 |
| `systemMonitor.cellular.modemDevicePatterns` / `excludedDevices` | [] / [] | AT 探测端口；排除端口 |
| `systemMonitor.configPullRedactSecrets` | true | 配置拉取时密钥脱敏 |
| `cameraService.allowCustomCommand` | false | 允许自定义 camera 命令 |

新增的数据类型和字段：

- 读数据类型 `dlt645_bcd_signed`；
- MQTT OTA 请求的 `signature` 字段；
- QtDisplayBridge 的 `--shm-mode` 参数。

## 4. 升级前必须完成的运维动作

1. **OTA 公钥。** 在签名机离线生成密钥对。升级到本版本之前，先把公钥装到每台设备的 `/etc/modbus-gateway/ota.pub`，可以用出厂包的 `--ota-pubkey` 参数，或 `production-init.sh --ota-pubkey`。平台按 `OTA签名说明.md` 生成 `signature` 字段。否则升级后所有 OTA 都会被拒绝；过渡期可以在本地配置里设 `requireSignature=false`。
2. **维护口令。** 在 `directMaintenance.allowedClientCidrs` 中写入维护网段，然后执行 `echo '<口令>' | SystemMonitor --app-config .../monitor-service.json --set-maintenance-password`。仓库自带的 `monitor-service.json` 白名单为空，升级后维护接口不会启动，这是有意为之。
3. **北向 Modbus。** 如果北向服务监听 `0.0.0.0` 且没有 `allowedClientCidrs`，升级后会拒绝启动，需要补白名单。
4. **轮换泄露的凭据：**
   - EasyTier 组网密钥；
   - 固定维护口令；
   - 历史出厂包中如果带了 `devices.csv`，轮换其中的 SSH 密码、MQTT 密码和 TLS token。
5. **平台端配套改造：**
   - 命令不要设置 retain；
   - 不要使用 `highPriority`；
   - `ts` 使用服务器毫秒时间；
   - OTA 带签名，下载地址改为 https；
   - 配置下发不再携带 camera `command`。
6. **AT 探测。** 使用 4G 的站点需要显式配置 `cellular.modemDevicePatterns`，例如 `["/dev/ttyUSB2"]`。
7. **有符号表计量。** 按表计手册，把 DL/T645 中的有符号量（功率、电流、功率因数等）改为 `dlt645_bcd_signed`，并在现场核对。
8. **集群符号约定。** 现场确认 `clusterDispatch.commandSign` 的正负号约定。
9. **启用 OTA 恢复单元。** 已部署设备需要重新执行 `install-factory-config.sh`，或手动 `systemctl enable gateway-ota-recover.service`。

## 5. 仍未解决、上线前需要评估的项

- **MQTT 与配置下发。** 命令、配置下发没有端到端签名，目前靠 broker ACL；应做到每台设备一个账号，只能访问自己的 topic。
- **OTA。** C++ 侧在健康检查之前就上报 `completed`，需要改成两阶段上报。
- **systemd。** unit 加固（ProtectSystem、WatchdogSec、非 root 运行）要和升级守卫中固定的 SHA256 一起更新。
- **控制闭环：**
  - 设定值没有周期性重申，停机时不写 0（E12）；
  - AGC 对不可用 PCS 的功率份额仍会重新分配（E3 的局限）；
  - CAN 驱动的写循环未接入写命令过期检查（S2 的局限）。
- **必须补的测试：**
  - AArch64 交叉构建与目标板冒烟测试；
  - Qt 画面回归；
  - 硬件在环（HIL）故障注入，包括：BMS/PCS 断线、时钟跳变、kill -9 持锁进程、掉电打断 OTA、4G 半开连接。
