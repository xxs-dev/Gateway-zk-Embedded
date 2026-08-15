# Gateway-zk Edge Runtime

Gateway-zk 是 GatewaySuite 的 Linux 边端运行时，负责现场协议采集、共享点表、MQTT 数据上行、实时监测会话、EMS/事件计算、系统监测、OTA 和本地 SCADA。生产目标为 Allwinner AArch64 设备。

## 仓库与协作基线

- 内部 GitLab：[gateway-suite/gateway-zk](http://192.168.22.104/gateway-suite/gateway-zk)
- SSH 克隆：`git clone git@192.168.22.104:gateway-suite/gateway-zk.git`
- 默认分支：`master`
- 跨端契约、兼容矩阵和发布清单：[GatewaySuite 协调仓库](http://192.168.22.104/gateway-suite/gatewaysuite)

`snapshot/current-worktree-20260814` 是迁移时未提交工作树的安全归档，不代表通过测试或允许部署。设备发布只能使用协调仓库中固定的 commit、制品路径和 SHA256。

## 主要能力

- Modbus RTU/TCP、DL/T 645、IEC 101/103/104、CAN 和本机 DIO 驱动。
- 共享内存点表、路由、质量与时间戳管理，以及离线数据补传。
- MQTT realtime、full、告警、状态、诊断、配置快照和 OTA 消息。
- EventEngine、ComputeEngine、AGC/AVC、EMS V1/V2 和控制权仲裁。
- SystemMonitor、维护接口、CameraService 和本地 Qt/SCADA 显示。
- factory/runtime 配置、systemd 服务、安装、升级和回滚工具。

## 实时监测与 full 上传边界

实时监测和周期 full 上传是两个独立数据通道：

- realtime 请求 topic 为 `edge/telemetry/realtime/request/{machineCode}`，数据 topic 为 `edge/telemetry/realtime/{machineCode}`。
- start/subscribe 根据 `sessionId` 写入或续期 realtime session；stop/unsubscribe 只删除同一 `sessionId` 对应的 session。
- stop/unsubscribe 不得修改 `fullUpload`、`fullUploadIndexes` 或 `fullUploadIntervalMs`。
- full 调度继续按 `fullUploadIntervalMs` 发布到 `edge/telemetry/full/{machineCode}`，不受 realtime session 数量影响。
- 验收必须同时证明目标 realtime session 已停推，并跨至少两个 full 周期证明 full 仍持续上传。

实现入口位于 `src/mqtt_driver_service.cpp`，跨端验收口径以 GatewaySuite 工作项和契约为准。

## 项目结构

```text
Gateway-zk/
  include/edge_gateway/    公共模型和服务接口
  src/                     驱动、点表、MQTT、EMS 和系统服务实现
  config/                  factory、runtime 和示例配置
  deploy/                  安装、systemd、升级和回滚脚本
  products/                固定 EMS 产品及项目配置
  tools/                   测试、构建、生成和发布工具
  test-lab/                协议模拟与隔离实验室工具
  ky-ems/                  本地 EMS/SCADA 组件
  doc/                     架构、协议、配置、部署和验收文档
```

## 本地构建与测试

要求 CMake 3.20+、支持 C++17 的编译器，以及目标功能所需的系统依赖。Linux 主机上的基础构建流程：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

只运行与改动相关的测试不能替代发布门禁；涉及共享点表、MQTT、控制或配置加载时，需要执行对应工具测试和端到端回归。

## AArch64 交叉编译

生产制品必须在 `192.168.22.11:/srv/build/Gateway-zk` 使用正式 Allwinner AArch64 工具链构建。不要使用 RK3568 sysroot、Windows MinGW Qt 或未经核验的本地交叉工具链。

1. 通过 Git 或隔离源码快照同步明确的 commit。
2. 核对远端分支、commit 和工作区状态。
3. 在构建机执行 `tools/build_edge_aarch64.sh` 和需要的 `tools/build_scada_qt_aarch64.sh`。
4. 单独取回制品并计算 SHA256；构建完成不等于部署授权。

完整命令见 [192.168.22.11 边端交叉编译教程](doc/交叉编译教程/192.168.22.11边端交叉编译教程.md) 和仓库根目录 [AGENTS.md](AGENTS.md)。

## 配置与部署

- 配置字段：[配置字段说明](doc/配置与消息/配置字段说明.md)
- 运行配置：[运行配置样例](doc/配置与消息/运行配置样例.md)
- MQTT 消息：[消息驱动说明](doc/配置与消息/消息驱动说明.md)
- 部署流程：[边端部署说明](doc/部署运维/边端部署说明.md)
- 上线检查：[生产上线检查清单](doc/部署运维/生产上线检查清单.md)
- 文档总览：[文档索引](doc/文档索引.md)

不得把 Broker 密码、TLS 私钥、隧道密钥或设备凭据提交到仓库。敏感值应由环境变量、受控文件挂载或部署密钥系统注入。

## 发布规则

1. 不从脏工作树或 snapshot 分支直接制作正式制品。
2. 不在共享构建目录执行 `git reset`、`git clean`、强制 checkout 或覆盖未知改动。
3. 构建、上传、安装和启动是独立阶段；没有明确部署授权时只允许构建和校验。
4. 发布清单必须记录三端 commit、制品 SHA256、兼容性结论、测试证据和回滚步骤。
