# StorageBudget 接入审计

本阶段只审计干净候选树，不改变 21 项 dirty/untracked 的正式边端树，也不做设备部署。

## 结论

当前没有安全的单一跨进程写入入口可以直接接入 `StorageBudget`。因此本阶段不硬改生产路径，提交可执行审计测试和以下明确边界。

## 已定位入口

| 类别 | 写入/准入入口 | 当前保护 | 本阶段结论 |
| --- | --- | --- | --- |
| EventStore outbox、local journal、可选 history footprint | `src/mqtt_event_outbox.cpp:2852`、`:2900`、`:2965` | `statvfs`、数据库及 sidecar 逻辑长度、`minFreeBytes/maxStoreBytes`；新 append 才拒绝 | 唯一具备事务 append + footprint + 拒收的候选统一入口，但只拥有 EventStore 数据库 |
| 30 天点位/报警历史 | `src/sqlite_sample_writer.cpp:152`、`src/sqlite_alarm_writer.cpp:219` | 各 writer 自己按 `retentionDays` 清理 | 没有统一预算或低水位准入；不能由 EventStore 代替 |
| 控制账本 | `src/control_dedup_store.cpp:179`、`src/mqtt_control_result_store.cpp:225` | 各自 SQLite 连接；控制结果有记录数/天数清理 | 独立写入和生命周期，未接入 StorageBudget |
| OTA 制品、staging、backup、升级日志 | `src/ota_service.cpp:1020`、`:1140`、`:1157` | `maxArtifactBytes` 和 `ensureFreeDiskSpace` 只针对 download 路径；历史日志直接追加 | 没有覆盖 staging/backup/日志的统一预算 |
| MQTT offline 文件 | `src/builtin_mqtt_driver_publisher.cpp:2707` | append 后按 `offlineBufferMaxDiskBytes` 截断 | 没有整机低水位检查；截断语义不能替代关键账本保护 |
| systemd 服务日志 | `deploy/*service`: `StandardOutput=journal` | 由 journald 外部管理 | 应由设备运维配额治理，当前应用无法安全统计或拒收 |

## 为什么本阶段不接入

把 `StorageBudget` 直接放进 EventStore append 只能保护 EventStore 自己可见的数据库字节；它无法原子地预留或拒绝控制账本、OTA backup、offline 文件和 journald 的后续写入。把其他类别的估算值从配置拼进来会产生重复计数、漏计和跨进程竞态，也可能在关键事件确认/恢复路径上改变既有语义。

因此 `5f884ef` 提供的纯策略函数保持为后续各写入 owner 可复用的基础；真正整机保护需要先定义单一 owner、各类别 footprint/预留协议，以及关键事件在低水位下的保留和拒收优先级。

## 测试边界

`tools/storage_budget_entrypoint_audit.py` 验证上述入口仍存在，并确认当前五类独立 owner 尚未声称调用 `checkStorageAdmission`。它是源码契约审计，不是设备满盘测试，也不证明 2GB/T536 点数或字节率准入。
