# COMM202600104 PCS 远方/就地控制联动

## 行为与边界

- 读取 PCS 实际反馈 `MTR20250600657.mb4_s1_1214`，共享内存 `gateway_point_store_ttySP4`，FC02 地址 86：1 远方，0 就地。
- 远方反馈新鲜有效时恢复原按钮颜色，并进入原有登录、确认和驱动回执流程；不表示设备必然执行成功。
- 就地时按钮灰显，但保留点击解释，不入队。覆盖启停、复位、运行模式、并离网设定及六个分相功率修改入口。
- 缺失、坏质量、过期、未来时间戳、非二值或跨设备绑定也会拦截。新鲜度要求为不超过 5000 ms，并同时检查点位 stale/expireAt。
- 提交前再次检查，包括输入框及确认框返回后，防止确认期间切回就地。
- 此条件独立于 EMS 的平台控制权限；不会自动切换 PCS 模式，也不修改驱动的写权限。
- `pcsRemoteTag` 为画面控件的可选属性。104 已绑定；旧项目没有该属性时行为不变。其他型号不得直接套用本 PCS 的地址与语义。

## 修改范围

显示公共判定在 `local_display_qt_pcs_remote_guard.hpp`，普通按钮与三相功率控件共用。
Lanhu 生成器为已确认的 PCS 点位补充属性，现场只修改 `Control-Pcs.json`、包版本和校验清单。点表、账号及其他页面不变。

## 验证与部署

- 编译：192.168.22.11，独立源代码快照，Allwinner AArch64 工具链；复用库前核对整个 src/include 与既有构建一致。
- Qt 模拟测试：104 上独立 mount/net/ipc 命名空间、私有 `/dev/shm`、offscreen 平台、假运行时，不连接生产点库。
- `local_display_qt_access_test`、`local_display_qt_lanhu_test`、`local_display_qt_soe_test` 全部通过。
- 覆盖就地、远方、缺失、过期、坏质量、非二值、跨设备绑定、确认期间模式变化和功率编辑拦截。
- 全部 55 页，4 种尺寸，正常长值/缺失数据共 440 次渲染，0 文字溢出。
- `lanhu-scada.test.ps1`、`lanhu-controls.test.ps1` 通过；`git diff --check` 通过。
- 编译机 QEMU 的 Qt 平台插件不可用，因此不将其启动失败视作测试通过；最终采用上述原生 ARM 隔离验证。
- 仅替换 KY-EMS 并重启 `ky-ems.service`。其他采集/计算/事件/网络服务 PID、配置、驱动文件及默认路由前后核对不变。
- 新二进制 SHA256：`b4f0c3389c23f3644a6e11f8accab970657c2c91774482d2ccfa757b1a46146c`。
- 本次没有发送任何电气命令，没有切换 PCS 远方/就地，没有实机执行启停或功率修改。

## 证据与回退

本地证据：`D:/workspace/new/comm104-pcs-start-20261030/`。
板端记录及旧显示程序备份：`/opt/modbus-gateway/diagnostics/ui-pcs-remote-20261030/`。
新项目：`/opt/modbus-gateway/scada/releases/comm104-lanhu-pcs-remote-20261030`。
旧项目：`/opt/modbus-gateway/scada/releases/comm104-lanhu-generator-controls-20261029`。

维护回退应同时恢复备份 KY-EMS 与旧项目链接，只重启显示服务；不要重启驱动或改写点位。部署脚本在失败时自动按此范围回退。
