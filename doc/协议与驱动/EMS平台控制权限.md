# EMS 平台控制权限

## 语义

- 远程：允许平台电气控制命令进入原有校验链。不会自动开启点位写权限，也不绕过租约、参数范围或并机校验。
- 就地：拒绝平台电气控制命令；本机画面与直接接入仍执行各自原有的鉴权和安全校验。
- 该开关不是 PCS 运行模式，不发送设备启停或功率命令，不停止已经运行的本机策略。
- 遥测上送、配置管理和升级不是此电气控制开关的拦截对象。MQTT 命令来源按入口识别，不信任载荷里的 `source` 字符串。

## 存储与入口

边端 Linux 默认状态文件为 `/opt/modbus-gateway/data/ems-control-mode.json`。文件不存在时默认远程；已保存选择跨进程重启保留。非法、不可读状态拒绝平台控制，画面显示状态异常。

登录后可在设备控制页切换，并需二次确认。EMU 监测页只显示同一实际状态。确认时再次检查会话权限，取消或过期均不改变权限。

可配置键：设备配置 `memoryStore.emsControlModeFile`、应用配置 `mqttDriver.emsControlModeFile` 和 `mqttForward.control.emsControlModeFile`。所有参与者（包括画面使用的应用配置）必须指向同一状态文件。默认路径的父目录由安装环境提供。

平台入口和物理执行端分别校验。切换就地保存撤销截止时间，再切回远程不会重新放行此前排队的命令。正在下发命令时持有共享文件锁，切换需要独占锁；忙时切换失败，不提前显示成功。已提交给本机策略的目标值不因此自动清除。

## 配套升级

`PendingWriteCommand.platformControl` 是可信入口标记，使用共享内存 v11 pending slot 的保留字节，不改变布局大小。旧驱动不会识别该标记，不能仅替换显示或 MQTT 程序。维护时先暂停入口、确认队列为空，再同步替换实际使用的物理执行程序并恢复服务。不得靠修改 `source` 字符串实现权限区分。

相关回归：`ems_control_mode_test`、`mqtt_driver_service_test`、`mqtt_forwarder_service_test`、`gateway_daemon_realtime_priority_test`、`memory_point_store_shm11_test`、`local_display_qt_access_test`、`local_display_qt_lanhu_test`。现场验证不应发送真实电气控制命令；权限转换与拒绝测试使用隔离共享内存和模拟执行器。

## COMM202600104 消防周期

2026-10-27 厂家确认不支持远程修改频率。禁用 `fire_detector_auto_upload_period` 的启动写入和 `write.enable`，画面标签/路由同步为只读，保留主动上报接收配置。软件回退也不得恢复该不受支持的启动设置。
