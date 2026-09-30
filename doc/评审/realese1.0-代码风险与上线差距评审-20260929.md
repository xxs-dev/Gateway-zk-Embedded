# realese1.0 边端代码风险与上线差距评审（2026-09-29）

评审对象：`edge/realese1.0`（Gateway-zk 边端运行时，Allwinner AArch64）。
评审方式：静态代码走读，覆盖 `src/`、`include/`、各 `*_main.cpp`、`deploy/`、`tools/` 中的构建与测试脚本。按 6 个方向分组：MQTT/控制链路、协议驱动、共享内存/存储、EMS 控制逻辑、OTA/部署、监测/维护/显示。每条结论都对照代码核实过调用路径，行号以评审时的版本为准。

## 1. 总体结论

**当前版本不具备生产放行条件。** 数据采集和上送的主干设计比较扎实：共享内存 robust mutex、Modbus RTU/TCP 严格帧校验、KECP/2 集群协议、TLS 校验默认开启、OTA 压缩包路径校验等都做得到位。问题集中在四类：

1. **远程控制的授权边界过弱。** MQTT 控制命令的授权取决于 payload 自带的 `highPriority`/`source` 字段；OTA 包不做签名，只校验同一条消息里给出的 sha256；维护口令是固件内置的固定哈希；配置下发可以写入 camera `command`，最终以 root 执行 shell。结论是：拿到 broker 发布权限或维护网段访问权，就等于拿到了设备 root 权限和 PCS 控制权。
2. **失败被当成成功。** IEC104 的否定确认（P/N 位）被掩码掉；陈旧的确认报文会被当成新命令的确认；Modbus int16/uint16 写入值越界时静默回绕（例如 40000 被写成 −25536，充放电方向反转）；CAN 写入时同帧的其他信号被清零。
3. **失去数据或失去授权时，控制链路"保持最后值"，而不是"回到安全值"。** 图形 EMS 中间输出的 TTL 为 10 分钟；SOC 缺失时 SOC 保护被整段跳过；NaN 能穿过 SOC 比较；集群丢失授权后连 0 kW 都写不下去；AGC 对"不可用"的 PCS 不再下发任何值。
4. **长期运行会退化或卡死。** 控制去重库写满 10 万条后永久拒绝写入；北向 Modbus 每个连接泄漏一个线程；共享内存持锁进程被杀后序号回退，导致历史样本和控制命令静默丢失；写命令队列不过期，采集进程重启后会执行几小时前的设定值。

## 2. 严重度汇总

| 方向 | Critical | High | Medium | Low |
|---|---|---|---|---|
| MQTT / 控制链路 | 1 | 7 | 4 | 3 |
| 协议驱动 | 1 | 8 | 6 | 5 |
| 共享内存 / 存储 | 0 | 4 | 5 | 5 |
| EMS 控制逻辑 | 1 | 4 | 5 | 3 |
| OTA / 部署 | 1 | 5 | 6 | 3 |
| 监测 / 维护 / 显示 | 1 | 5 | 6 | 3 |

修复状态见第 9 节。

## 3. MQTT 与远程控制链路

| # | 级别 | 位置 | 问题 | 后果 |
|---|---|---|---|---|
| M1 | Critical | `mqtt_driver_service.cpp:369-378`、`power_control_ownership.cpp:914` | `highPriority` 与 `source` 直接取自命令 JSON；`priority:1`、`"yes"` 也会被当作 true；是否检查 SCADA 租约、AGC 邮箱、集群授权，取决于 `source` 字符串的前缀或子串匹配 | 任何能向命令 topic 发布消息的客户端都能绕过功率控制权仲裁，并抢占 30 s 优先租约 |
| M2 | High | `mqtt_driver_service.cpp:1650`、`point_store_router.cpp:757` | 不拒绝 retained 命令，不校验 `ts` 时钟偏差；路由器把客户端提供的 `ts` 当作"当前时间"来判断租约 | 重连后重放旧命令；伪造未来时间戳即可让租约被判为已过期 |
| M3 | High | `mqtt_driver_service.cpp:176-352` | `FlatJsonReader` 在全文里取第一个同名 key（嵌套对象里的也算）；`100abc` 被解析为 100；`3.9` 被截断为 3 | 实际写入的点位与审计记录不一致 |
| M4 | High | `modbus_codec.cpp:274-281` | 写值只检查可选的 min/max，不检查数据类型的取值范围，转换时静默回绕 | int16 写 40000 实际下发 −25536，充放电方向反转 |
| M5 | High | `control_dedup_store.cpp:200` | 去重库只增不删，达到 10 万条后抛出 capacity exhausted | 1 条/秒的调度约 28 小时后，MQTT 物理写入永久失效 |
| M6 | High | `builtin_mqtt_driver_publisher.cpp:2404` | 发出 PINGREQ 后不检查是否收到 PINGRESP | 4G NAT 老化后形成半开连接，最长约 15 分钟收不到命令，而遥测照常上送 |
| M7 | High | `builtin_mqtt_driver_publisher.cpp:1579,2453` | 超大报文直接断开连接，同一批中已 ACK 的命令被丢弃 | 若是 retained 大消息，每次重连都会失败，形成重连风暴 |
| M8 | High | `ota_service.cpp` | OTA 不做签名校验，允许 http，重定向允许从 https 降级到 http | 见 O1 |
| M9 | Medium | `mqtt_driver_service.cpp:2132`、`point_store_router.cpp:955` | AGC 邮箱分支跳过了限流、去重和数值校验 | 能写入 `inf`，也可被刷爆 |
| M10 | Medium | `builtin_mqtt_driver_publisher.cpp:2941` | 重连没有退避；离线期间状态消息持续灌入离线缓存 | 扫描周期被拖慢；离线队列被状态消息挤满，真正的遥测被挤掉 |
| M11 | Medium | `builtin_mqtt_driver_publisher.cpp:1169,1628` | 订阅连接复用了连接阶段剩余的超时；只检查 socket 是否可读，没有检查 `SSL_pending` | 大报文读到一半超时；命令延迟到下一个 keepalive 才处理 |
| M12 | Medium | `builtin_mqtt_driver_publisher.cpp:1696` | SUBACK 返回码 0x80（订阅被拒）没有检查 | ACL 拒绝订阅后，设备静默地失去控制通道 |
| M13 | Low | `priority_control_lease.cpp:196-257` | acquire 与 release 之间存在竞争 | 可能误删别人的租约 |
| M14 | Low | `mqtt_realtime_ring_buffer.cpp:259`、publisher 1928/1945 | uint32 长度相加可能溢出；空 payload 取 `&v[size]`；MQTT5 属性长度未设上限 | 文件损坏时会申请巨额内存或读到异常数据 |
| M15 | Low | `mosquitto_mqtt_driver_publisher.cpp` | 忽略 TLS 配置，明文发送密码 | 目前没有被引用，属于隐患 |

## 4. 协议驱动

| # | 级别 | 位置 | 问题 | 后果 |
|---|---|---|---|---|
| P1 | Critical | `iec_codec.cpp:1158`、`iec_client.cpp:205,410` | COT 用 `& 0x3F` 掩码，丢掉了 P/N 位 | RTU 拒绝执行（互锁、就地模式）时，仍返回 success |
| P2 | High | `iec_client.cpp:1022` | `controlValues_` 在下发命令前不清空；不比对命令值；不区分 S/E 位 | 上一条超时命令迟到的确认，会被当作新命令的确认 |
| P3 | High | `iec_driver_main.cpp`、`iec_client.cpp:824`、`am5se_iec103_client.cpp:596` | `send` 未加 `MSG_NOSIGNAL`，进程也没有忽略 SIGPIPE | 对端复位连接时，驱动进程被 SIGPIPE 杀掉 |
| P4 | High | `iec_client.cpp:1187,334` | IEC101/103 设备无应答时 poll 仍返回"成功" | 断线的设备保持在线、质量好，最长 10 分钟 |
| P5 | High | `iec_client.cpp:686` | 发出 TESTFR 后不检查 t1 超时 | 半开连接约 15 分钟无法发现 |
| P6 | High | `iec_codec.cpp:342-421` | 质量描述词 IV/NT/SB 未解析 | 设备自己标记为无效的数据被当作有效数据发布 |
| P7 | High | `dlt645_codec.cpp:43` | 有符号 BCD 没有按 bit7 符号位解析 | 反向功率 −0.1234 被解析为 +80.1234 |
| P8 | High | `dlt645_codec.cpp:328`、`dlt645_client.cpp:119` | 读响应不校验表地址，发送前不清空接收缓冲 | 共享 485 总线上，A 表迟到的应答被记到 B 表 |
| P9 | High | `modbus_northbound_server.cpp:511` | 客户端线程只增不 join | 长期运行后创建线程失败，北向服务永久停止 |
| P10 | Medium-High | `iec_client.cpp:318,403` | N(S) 在组帧时分配，发送时才加锁 | 并发时帧序号乱序，RTU 断链 |
| P11 | Medium | `models.hpp:631` | 北向默认监听 0.0.0.0 且无白名单；idle 超时与请求超时共用同一个值 | 正常 SCADA 被频繁断开；局域网内任何主机都能占满连接 |
| P12 | Medium | `can_driver_service.cpp:550` | 单个点解码异常会中断整轮接收并 sleep 1 s | 一帧 DLC 异常就拖垮整个 CAN 解码 |
| P13 | Medium | `can_signal_codec.cpp:234,162` | 编码从全零 payload 开始；不检查位宽范围 | 写一个信号会清零同帧其他信号；超出位宽的值静默回绕 |
| P14 | Medium | `am5se_iec103_client.cpp:702` | 重发时也翻转 FCB | 丢失保护事件 |
| P15 | Medium | `iec_client.cpp:1097` | `rxBuffer_` 跨线程访问，未加锁 | 数据竞争，可能导致堆损坏 |
| P16 | Low | `posix_serial_port.cpp:45` | 波特率非法时 fd 保持打开，termios 未完成配置 | 串口以错误参数运行 |
| P17 | Low | `sysfs_gpio_port.cpp:63` | direction 先写 `out`，会先输出低电平 | 低有效继电器在上电时抖动一下 |
| P18 | Low | `modbus_northbound_server.cpp:228` | 北向值越界时回绕 | 上位机读到错误值 |
| P19 | Low | `modbus_rtu_client.cpp:339` | 本地回显的 FC05/06 与正常应答完全相同 | 设备没收到写命令也被判为成功 |

## 5. 共享内存、路由与存储

| # | 级别 | 位置 | 问题 | 后果 |
|---|---|---|---|---|
| S1 | High | `memory_point_store.cpp:47-109` | EOWNERDEAD 处理时清空整个存储，并把序号清零 | flush 线程随后用旧序号 ack，把新产生的样本全部删掉；待执行的写命令也被静默丢弃 |
| S2 | High | `memory_point_store.cpp:386`、`gateway_daemon.cpp:413` | 写命令不过期 | 采集进程重启后执行几小时前的设定值 |
| S3 | High | `point_store_router.cpp:1175` | 不同 segment 中相同的 index 被静默合并 | 写命令和告警发到错误的设备 |
| S4 | High | `memory_point_store.cpp:149` | 进程租约使用墙钟时间 | 时钟回拨后进程被 systemd 判为 start-limit，永久 failed |
| S5 | Medium | `event_engine_service.cpp:264,294` | 非 IPC 路径不过滤坏质量和 stale 的值 | 产生误告警，或者误判告警恢复 |
| S6 | Medium | `gateway_daemon.cpp:370`、`event_engine_service.cpp:800` | 历史样本整批写入，按每条 4 KiB 估算空间；待持久化的告警队列没有上限 | 历史写入永久被拒；内存持续增长 |
| S7 | Medium | `memory_point_store.cpp:401,503` | 共享内存 header 中的游标不做校验就直接使用 | 越界访问；一个坏槽位就让整个写路径失效 |
| S8 | Medium | `mqtt_event_outbox.cpp:2026` | `maxDiskBytes` 只统计未发送的数据；日志模式为 DELETE + NORMAL | 磁盘被写满；掉电后可能丢数据 |
| S9 | Medium | `memory_point_store.cpp:1419` | 读取不存在的 index 时，持锁扫描全部 10 万个槽位 | 画面轮询拖慢采集进程 |
| S10 | Low | `memory_point_store.cpp:93` | `timedlock` 使用 CLOCK_REALTIME | 时钟跳变时误报超时 |
| S11 | Low | `memory_point_store.cpp:867` | 创建 segment 中途崩溃，留下半初始化的段 | 需要手工 `shm_unlink` 才能恢复 |
| S12 | Low | `memory_point_store.cpp:1343` | 写入方不检测 segment 是否已被替换 | 画面无数据 |
| S13 | Low | `point_store_router.cpp:207` | 保持型虚拟点每次值变化都执行 fsync | eMMC 磨损 |
| S14 | Low | `sqlite_*_writer.cpp` | 使用 `bind_int` 截断 index；dlopen 得到的符号放在全局变量里 | 大 index 被存成负数 |

## 6. EMS 控制逻辑

| # | 级别 | 位置 | 问题 | 后果 |
|---|---|---|---|---|
| E1 | Critical | `graph_ems_engine.cpp:2999,6646`、`models.hpp:957` | 中间输出 TTL 为 10 分钟；SOC、限值输入缺失时跳过对应保护；`restoreState` 把功率设定值按"好质量"恢复 | BMS 断线后，PCS 仍按原功率充电，最长 10 分钟且没有 SOC 保护 |
| E2 | High | `cluster_write_authorization.cpp:55-101` | 丢失集群授权后，写 0 kW 也被拒绝 | 主节点故障时，从节点保持放电 |
| E3 | High | `agc_avc_service.cpp:704` | 对"不可用"的 PCS 不再下发任何值，其份额分给其他机组 | 这台 PCS 失控，全站超发 |
| E4 | High | `compute_engine_service.cpp:30,755` | 调度周期与数据新鲜度都用墙钟判断 | 时钟回拨 1 小时，控制停摆 1 小时 |
| E5 | High | `graph_ems_engine.cpp:2987,6544`、`can_signal_codec.cpp:208` | NaN 未拦截；`clampAbs(NaN)` 得到 −limit | NaN 让 SOC 保护失效；潜在风险是满功率放电 |
| E6 | Medium | `graph_ems_engine.cpp:6141` | rateLimit 按停顿的总时长计算允许变化量 | 停顿或重启后功率一步到位，爬坡限制失效 |
| E7 | Medium | `ems_cluster_dispatch.cpp:105` | 集群与图形 EMS 的功率正负号约定相反 | 充电请求按放电裕量分配 |
| E8 | Medium | `graph_ems_engine.cpp:6597` | BMS 上报负的允许值时，`bmsPer` 为负 | 充电被翻转成放电 |
| E9 | Medium | `compute_engine_service.cpp:797` | 节点异常不计入健康状态 | 故障节点仍然担任主节点 |
| E10 | Medium | `graph_ems_engine.cpp:3381,3534` | 状态文件可能写入 `nan`；写入时没有 fsync | 下次恢复失败，全部状态丢失 |
| E11 | Low | `ems_cluster_transport.cpp:390` | 在序号校验之前就学习对端地址 | 被重放报文污染对端地址表 |
| E12 | Low | 多处 | 设定值没有周期性重申；停机时不写 0 | PCS 重启后状态不确定 |
| E13 | Low | `graph_ems_engine.cpp:6753` | 六相写入不是原子操作 | 一个扫描周期内三相不平衡 |

## 7. OTA、部署与运维

| # | 级别 | 位置 | 问题 | 后果 |
|---|---|---|---|---|
| O1 | Critical | `ota_service.cpp:1656`、`ota-apply.sh` | OTA 包没有签名；允许通过 OTA 覆盖 systemd 模板、bin 脚本和 app 配置 | 拿到 MQTT 发布权即可获得 root |
| O2 | High | `ota-apply.sh:568` | 服务重启之前就上报 completed，没有健康确认，也不会自动回滚 | 坏包被报告为成功，设备随后崩溃循环 |
| O3 | High | `ota-apply.sh:9,491` | 进行中标记放在 `/run`，开机时不做恢复 | 掉电后设备停留在半升级状态 |
| O4 | High | `ota-apply.sh:196` | 重复投递同一个 jobId 时会 `rm -rf` 原有备份 | 回滚无法恢复到升级前版本 |
| O5 | High | `build-factory-package.sh:314` | `deploy/*` 整体打包，而 `devices.csv` 被 gitignore，脏工作区检查发现不了 | 全网设备的凭据被打进出厂包 |
| O6 | High | `install-easytier-bootstrap.sh:110` | 组网密钥以明文硬编码在脚本中（已随代码与出厂包扩散） | 任何人都能加入组网，访问全部网关 |
| O7 | Medium | `ota-apply.sh:213,438` | tar 与 `copy2` 保留了包内的权限位 | 单元文件可能变成全局可写，本地提权 |
| O8 | Medium | `ota-apply.sh:606` | 重启脚本属于 mqtt-driver 的 cgroup | 重启到一半时自己被杀 |
| O9 | Medium | `ota_service.cpp:1635` | 下载超限后 curl 进程未被终止；没有 `--max-filesize` | 磁盘被写满 |
| O10 | Medium | `ota_service.cpp:1202` | staging 与 backup 目录永不清理 | 磁盘逐渐耗尽 |
| O11 | Medium | `gateway-tls-enroll.sh:223` | 注册接口允许 http，返回的 CA 直接作为信任根 | 中间人可替换信任根，进而劫持 MQTT |
| O12 | Medium | `deploy/*.service` | 全部以 root 运行，没有做加固，也没有 WatchdogSec | 进程挂死检测不到；被攻破后影响面大 |
| O13 | Low | `rolling-upgrade-legacy-runtime.py` | 接受任意主机密钥；密码默认为 root；改完配置才失败 | 失败后不回滚 |
| O14 | Low | `gateway-services.sh:545,595` | 临时文件名可预测；`set -e` 下 `result=$?` 永远执行不到 | 失败后 watchdog 保持暂停 |
| O15 | Low | 多处 | 版本标记不是原子写入；私钥先复制再 chmod；密码出现在命令行里 | 小概率导致状态损坏或泄露 |

## 8. 监测、维护、显示与配置

| # | 级别 | 位置 | 问题 | 后果 |
|---|---|---|---|---|
| D1 | Critical | `system_monitor_direct_maintenance.cpp:53,947` | 维护口令是固件内置的固定 SHA-256，客户端直接提交哈希；9443 端口明文 HTTP；CIDR 白名单可以为空 | 从固件里提取出哈希，就能写 PCS、改配置、执行 OTA |
| D2 | High | `system_monitor_service.cpp:2436`、`camera_service_main.cpp:310` | 配置下发只校验 `machineCode`；camera `command` 通过 `sh -c` 以 root 执行 | 一条 MQTT 消息即可获得 root |
| D3 | High | `config_loader.cpp:4040`、`scada_project_loader.cpp:145` | JSON 解析没有深度限制，dryRun 也会触发 | 深层嵌套导致栈溢出，SystemMonitor 崩溃循环 |
| D4 | High | `local_display_qt_scada_scene.cpp:1767` | 模态框打开期间定时器仍然重建界面 | 野指针；操作到一半 HMI 崩溃 |
| D5 | High | `qt_display_bridge_main.cpp:463` | 共享内存权限为 0666，写位直接转成控制命令 | 本机任意用户都能写设备 |
| D6 | High | `system_monitor_service.cpp:1584`、`models.hpp:1264` | 默认遍历 `/dev/ttyUSB*` 发送 AT 命令，并修改 termios | 干扰 USB-485 采集和 PPP 拨号 |
| D7 | Medium | `system_monitor_direct_maintenance.cpp:823` | 未认证的 GET 请求也会刷新上位机心跳 | 离线安全动作被抑制 |
| D8 | Medium | `scada_upper_computer_safety.cpp:161` | 离线判定使用墙钟 | NTP 跳变时误触发 PCS 停机 |
| D9 | Medium | `system_monitor_direct_maintenance.cpp:62,2597` | 单线程处理；96 MB 请求体在认证之前就读入内存；没有整体超时 | 慢速连接堵死服务；被 OOM 杀掉 |
| D10 | Medium | `system_monitor_service.cpp:3028,2143` | 分片缓存和租约表没有上限 | 内存持续增长 |
| D11 | Medium | `system_monitor_service.cpp:450` | 配置写入没有 fsync，备份永不清理，各 main 没有顶层 catch | 配置文件被截断后进程崩溃循环 |
| D12 | Medium | `qt_display_bridge_main.cpp:758` | 取写标志与回写读数之间存在竞争 | 下发旧值或 0 |
| D13 | Low/Medium | `local_display_qt_access_control.cpp:245` | 登录没有失败锁定；口令哈希只做单轮 SHA-256 | 可以暴力破解 |
| D14 | Low | `config_loader.cpp:249` | double 到 int 的转换未检查范围，端口号未校验 | 未定义行为，或端口回绕为 0 |
| D15 | Low | `system_monitor_service.cpp:2278` 等 | 状态 JSON 格式错误；本地显示 CORS 为 `*` | 平台端解析失败 |

## 9. 修复记录

本轮按"默认安全、可配置回退"的原则修改。凡是改变对外契约的项，都新增了配置开关，默认取安全值；平台端完成配套改造之前，可以在配置里临时放开。

修复状态见同目录 `realese1.0-评审修复记录-20260929.md`。

## 10. 距离生产使用还差什么（代码之外）

1. **在目标板上构建并回归。** 本轮修复只在 x86 主机上完成了编译和 ctest。按 AGENTS.md 的要求，必须在 192.168.22.11 用正式 Allwinner 工具链构建，核对 SHA256，并在目标板上执行 `production-smoke-test.sh`。
2. **平台端契约联调**，包括：
   - OTA 签名描述文件：由平台签名，设备内置公钥；
   - MQTT 命令去掉 `highPriority`，使用服务器时间戳，并且不设置 retain；
   - 维护口令改为每台设备独立的口令；
   - 配置下发不再携带 camera `command`。
3. **Broker ACL 按设备隔离。** 每台设备一个账号，只能读写自己的 topic。这是 M1、D2、O1 的根本防线，代码侧的修复只是纵深防御。
4. **轮换凭据。** EasyTier 组网密钥、固定维护口令已经随代码和出厂包扩散，需要全部轮换。排查历史出厂包里是否带有 `devices.csv`。执行 `git log --all -- config/runtime/tls` 确认私钥从未入库。
5. **硬件在环（HIL）与故障注入测试**，覆盖：
   - BMS/PCS 断线，确认功率在 N 秒内归零；
   - 时钟前跳和回跳各 1 小时；
   - kill -9 持锁进程；
   - 掉电打断 OTA；
   - 4G 半开连接；
   - IEC104 否定确认；
   - DL/T645 反向功率。

   目前的测试多为单元级测试，缺少这类端到端证据。
6. **设备侧安全兜底。** 确认 PCS 自身的通信超时保护（心跳或设定值看门狗）已经启用。网关的失效安全只能作为第二道防线。
7. **运维项：**
   - systemd 单元加固：NoNewPrivileges、ProtectSystem、WatchdogSec；
   - OTA 两阶段提交，健康确认失败后自动回滚；
   - 开机时恢复被中断的 OTA；
   - 日志与磁盘容量告警。

   这些在本轮只做了部分，见修复记录。
