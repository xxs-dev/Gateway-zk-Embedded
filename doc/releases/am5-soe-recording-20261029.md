# AM5 SOE 录波关联补充验证

日期：2026-10-29。分支：`realese1.0`。基线提交：`02550bdcdddb8339246d5b125f859735573066af`。

## 范围

新增 ASDU2 与 ASDU23 目录的保守关联、同库附表、下载身份复核、SOE 页面录波列及只读详情；补充默认关闭的示例配置和全流程操作说明。原始 SOE 行不修改，没有自动下载、没有新增电气控制命令。未部署 163、22.7、104 或其他现场设备，未连接真实 AM5。

构建快照只叠加本功能文件，不包含工作区原有的 SCADA 布局和蓝湖测试修改，未改动共享编译工作区。

## 构建与证据

- 构建机：`192.168.22.11`，Allwinner GCC 6.3.1，仓库 `build_edge_aarch64.sh`、`build_scada_qt_aarch64.sh`。
- 最终源码快照：`D:/workspace/new/am5-soe-recording-20261029/r3/source.tar.gz`。
- 快照 SHA256：`261c8f805fcaec057d38213119d272de5bf9d945fd1731640a0fd4f8c3879231`。
- 独立远端目录：`/tmp/am5-soe-recording-20261029-r3`。
- 本地产物与重开哈希：`D:/workspace/new/am5-soe-recording-20261029/r3/artifacts/sha256.json`。
- `IecDriver` SHA256：`22985155fbd56b9310ac1fefccfff6f33297945e540e1f527837d38d68157689`。
- `KY-SCADA` SHA256：`c41b3064763f49ac0844df34b3d66d108c596e3e69a04c4e6e330087ccb287b0`。
- 文档在构建后补充，不在该源码构建快照内；代码、测试和配置示例均在快照中。

## 测试结果

以下 7 个测试均在原生 Linux x86 与 ARM/QEMU 两种执行环境通过：

| 测试 | 本轮重点 |
| --- | --- |
| `am5se_soe_test` | 原事件解码、映射、历史与分页回归 |
| `am5se_soe_recording_test` | 关联状态、设备/机器/CA 隔离、FAN 复用、CP56 固定时区/无效日期、旧库迁移、只读、重启持久化、文件缺失 |
| `am5se_iec103_codec_test` | 原协议与录波编码回归 |
| `am5se_iec103_client_integration_test` | 主动/被动连接、SOE 穿插、查询节流、目录不完整/超时、错误链路地址/CA/长度/负确认、下载前后 FAN 复用 |
| `iec_collector_incremental_test` | 增量采集回归 |
| `iec103_recording_command_test` | 既有录波命令解析 |
| `iec103_recording_transfer_service_test` | 录波服务及故意注入的回执发布失败 |

结果文件：`r3/native-test-results.json`、`r3/arm-test-results.json`。执行器先验证独立 mount/net/ipc namespace，再挂私有 `/dev/shm`、`/run`、`/tmp`，只开启隔离回环网络。未打开现场 socket，未在边端运行模拟程序。

ARM/QEMU 首次运行因 sysroot 缺少顶层 `/lib/ld-linux-aarch64.so.1` 失败；测试执行器改用 sysroot 内实际的动态加载器后全部通过。未修改生产代码或系统 sysroot 来规避问题，失败日志保留在 `r3/arm-tests.log`。

`KY-SCADA` 和 `local_display_qt_soe_test` 的 Allwinner 编译通过，新增测试覆盖 8 列状态、只读详情及 FAN 展示。构建机没有原生 Qt Widgets，Allwinner sysroot 又没有离屏平台插件，故本轮未执行 Qt 界面测试、未生成或认证新界面截图。旧页面测试和历史截图不能替代本轮界面运行验证。

## 尚未验证的边界

- 真实 AM5 固件的 RET/CP32/CP56 时间关系、目录状态字位定义以及真实 SOE 与录波一一对应仍未验收。
- ASDU1 无 FAN、时间无效或多候选只报告不确定；`not_found` 仅指当前完整目录没有该 FAN，不代表从未录波。
- `downloaded` 核对文件存在和大小，不提供持续内容哈希认证。目录关联不保证装置当前允许取文件。
- 自动目录查询仅启动及新 ASDU2 后各给出有限检查预算，不能保证发现任意延迟生成的录波；显式查询继续走既有独占实例。
- 下载元数据和文件尚无自动过期删除；监控容量、备份后按独立审批管理。不会为释放空间删除证据。

操作及数据结构见 [全流程操作手册第 17 节](../协议与驱动/AM5SE-F原生SOE全流程操作手册.md#17-soe-录波关联从查询到显示)。上线仍须检查现场 ABI 与驱动配套版本，不能把本产物直接替换到历史 v7 共享内存设备上。
