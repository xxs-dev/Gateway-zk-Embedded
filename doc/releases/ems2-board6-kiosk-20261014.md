# 22.6 EMS 2.0 专用全屏与开机自启验收

日期：2026-10-14（America/Los_Angeles）。设备日志使用 UTC+08:00，显示为 10 月 15 日。

## 当前结果

- 已从 GNOME 桌面切换为专用 X11 显示会话，仅承载 EMS，不启动桌面、顶栏或任务栏。
- 实际窗口坐标为 `0,0`，尺寸为 `1920x1080`，与显示输出一致，无窗口边框。
- 开机自动登录专用显示会话，等待显示授权就绪后，通过既有服务及完整性校验启动 EMS。
- 屏保、DPMS 自动熄屏及 Xorg 虚拟终端切换已禁用；SSH 运维通道保留。
- 仅运行新版网关。旧程序仍仅作为回退备份保留，没有并行运行。
- 用户实屏确认：“可以正常切换，进入全屏”。全屏和页面切换人工验收通过，未据此宣称完整触摸校准或所有页面视觉验收通过。

## 验证

1. 22.11 隔离目录运行最终脚本测试：13/13 通过。
2. 两轮整机软件重启均自动启动界面；第二轮使用最终配置。未进行拔电测试。
3. 最终 boot ID：`9aee0247-b778-4ecf-b939-86e512a2658f`。
4. 最终网关恢复记录 `phase=FINISHED`，无待执行 systemd 作业。驱动、监控、Qt 和专用会话服务正常。
5. 最终启动中，EMS 进程在系统启动后 109.624 秒启动。重复恢复校验的启动开销仍存在，不属于快速开机验收。
6. 正常退出及维护抑制实测：维护标记存在时不自动拉起；解除后约 26.17 秒恢复。
7. SIGKILL 模拟界面崩溃后约 23.49 秒自动恢复，仅有一个 EMS 进程。
8. 8 个显示配置文件的 SHA256、权限和属主核验通过；原业务基线完整校验通过。
9. 最终启动未发现 GNOME Shell 进程或该进程的本次启动日志；屏保超时为 0，DPMS Disabled。

最终截图：`generated/production-20261014/board6-kiosk-20261014-v1/screen-final-boot-fullscreen.png`。
最终机器证据：同目录 `check-final-verified.log`、`verified-reboot-reconnect.json`、`verified-recovery-*.log`。

## 实现与范围

新增 `deploy/gateway-ems-kiosk.py`、`deploy/install-ems-kiosk.py` 和 `tools/ems_kiosk_test.py`。
GDM 仍作为底层显示管理器存在，但不启动 GNOME 桌面会话。
界面守护只调用 `systemctl start ky-ems.service`，不直接启动可执行文件、不清理启动门禁。
它遵守手动停服、配置应用、待处理升级/恢复事务以及启动器状态。
历史 reprovision `STOP` 由原生命周期保留，其有效门禁由 `production-unit` 判断，不能当作当前维护标记，也没有删除它。

修复并保留了两类中间失败证据：守护服务的 `ProtectHome` 隐藏了就绪文件；以及将历史 `STOP` 误识别为当前维护。
前者改为只读暴露专用用户运行目录，后者补充失败回归测试后修复。

业务程序及既有部署工具仍来自 `ff75a5271e5be4d9a532084ed5ea038e69cf342a`，没有重编译 C++ 或替换业务二进制。
基线 `LOCAL_ACTIVATE_D6B` 的 SHA256 仍为
`1172a9b5018e5d3fc1730cbbe4373dafddfe9c5e5545b26c1c56e64fe3174be6`。

最终显示脚本 SHA256：`b36ad60bc00fcbadcd5a66c67b7371e9f62df44569d3255571838d9346ad5f88`。
最终安装脚本 SHA256：`5d9949ed4b2e9fe67f31110dbb4d2a5a38350c61c388c1f645a02e607ce39276`。
本次改动未提交、未推送；产品工作树 HEAD 为 `163c683ef7520f93b79613318337c200243ef3eb`。
该显示配置已验证的前提为 GDM/X11、UID 1000、`:0` 和当前原生 Qt 服务授权绑定，不宣称已适配任意边端系统。

## 运维与回退

维护前先停止 `gateway-ems-kiosk-watch.service`，再通过原配套生命周期工具停服/升级。
显示配置备份位于设备 `/srv/gateway-board6-kiosk-20261014-v1/previous-display-profile`，已返回本地并核验。
`receipt.json` 保留初始记录，`receipt-current.json` 包含最终配置摘要，保留了中间修订。
经审核需要退回原桌面配置时，可使用本次配套安装脚本的 `restore`，传入该备份目录；它会先验证文件未漂移，只恢复显示配置，不启动旧版业务程序。
本轮未执行真实桌面回退，避免撤销用户要求的专用全屏状态。

当前仍是模拟采集、禁止物理控制、OTA 禁用的受控验证环境。真实设备、HIL、长稳、断电、生产信任配置及完整 UI 一致性仍未验收，`productionReady=false`。
22.7 未修改。用户随后反馈 PCS/BMS 监测页面局部错位，另行修复，不应把全屏通过当成页面布局全部通过。
