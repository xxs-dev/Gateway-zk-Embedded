# 22.6 并机拓扑默认本机一台验收记录

保存日期：2026-10-20（America/Los_Angeles）。截图沿用设备自身时区。

## 结果与行为

- 已按用户授权部署到 `192.168.22.6`，用户实屏确认：“已显示一台本机储能车”。设备实际截图也已核对。
- 无成员列表或列表为空时，默认显示一台“本机储能车”，身份取当前配置的本机节点 ID。状态显示未知，遥测显示 `--`，不假定在线或已有功率/SOC。
- 收到非空有效成员列表后，按真实列表显示，不额外追加本机；有效空列表会清除旧成员并恢复默认一台。
- 已知成员的快照过期或异常时，保留已有成员、使遥测失效，不误删为一台；原有分页、离线和身份冲突处理保留。
- 仅修改显示逻辑与回归测试，没有修改组网成员协议、共享内存 ABI、点表或控制行为。

## 构建与验证

分支 `realese1.0`，基础提交 `aac6e5e1636f82df3a98b084653ffc977af7f25c`。本次制品包含逐文件 SHA256 固定的未提交改动，没有新增提交或推送，不能作为正式生产发布提交。

- 在 `192.168.22.11` 的隔离目录 `/srv/build/gateway-topology-local-20261020/local1` 同步并核验源码，使用 Allwinner AArch64 工具链构建。
- 在 `192.168.22.7` 隔离运行原生 access、behavior、audit 测试，退出码均为 0。
- 覆盖默认一台、空/异常列表、0/1/2/3/4/5/10 个成员、分页、真实列表替换、过期、重复身份及零控制写入。
- 448 次原生渲染通过；1920×1080 文字扫描发现数为 0。其他分辨率验证渲染和缩放，不宣称已进行相同文字扫描。
- 核对默认一台测试截图，以及用户打开并机拓扑后采集的实际 1920×1080 截图。

## 部署范围

安装事务 `UI_TOPOLOGY_LOCAL_20261020_V1` 已完成，状态 `COMPLETED`。通过现有受保护安装流程，仅替换 `/opt/modbus-gateway/ky-ems/KY-EMS`。

SCADA 包仍为 `2.0.14-lanhu-20261020`，目标仍为 `releases/ems2-lanhu-20261020-v1`。账号、权限、点表、运行配置及 8 个 kiosk 文件保持不变，守护仍启用。没有重启整机，没有旧程序并行运行。

部署后 EMS、监控、模拟采集各仅一个进程，观测时 `NRestarts=0`。短时健康采样最大点年龄 209ms、最大 HTTP 延迟约 24.61ms，不代表长期性能保证。物理控制和 OTA 保持禁用，采集仍为隔离模拟环境。

## 证据

证据目录：`generated/topology-local-default-20261020/`。

| 项目 | SHA256 |
| --- | --- |
| 新 Qt 程序 | `67a04a60d89b0cb25596a4f6560739c8f9f6654816a35fef7761d84bf6088679` |
| 旧 Qt 程序 | `934b0a64ab4112cbf99fc0dbde0247baef29c4651dac1ab7f8d4887cfcabb3b2` |
| 未更改的 SCADA 包 | `f4f0c8b0fb262628c5a3e855296778633652b86ae3f87bc63c53e5197fb60d70` |
| 新运行基线 | `5e6bd0e7c85d7c2e0e52eb9f952a3334438cf13931de7df42b45df50c4bb4c18` |
| 软件验证索引 final-evidence.json | `63a2790e807ed539dded2d4ed15c910c266290a9ec26c3482c0017b8cf93e2e1` |
| 实屏验收索引 physical-acceptance.json | `9450ed20ddef3f35d679059f38649eb8eabe95b975c78ed387ca417fa68b3bf1` |
| 实际并机拓扑截图 screen-1.png | `29a5d804dfdf33d63598821b63439f21ce3d269e0801277cdef8b2c8b90aabc4` |

软件索引生成早于用户确认，其 pending 字段作为历史证据保留。当前验收结论以独立的 `physical-acceptance.json` 为补充，不重写已封存索引。

![22.6 默认一台本机储能车](D:/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/production-ems2-20261014/generated/topology-local-default-20261020/deploy/physical/capture/screen-1.png)

## 回退与限制

- 已核验旧程序和原 SCADA 归档保留于证据目录的 `deploy/preflight/`；设备暂存目录为 `/srv/gateway-topology-local-board6-20261020-v1`。
- 已完成事务不能靠重放 `production-recover` 撤销。回退需使用旧制品创建新的反向安装事务，不能直接覆盖程序或改写完整性记录。
- 本轮完成默认一台的实屏验收，不等于完整 56 页、触摸、95% 视觉一致性或真实多车接入验收。
- 本候选未重新进行整机重启/突然断电测试。长期运行、真实设备/HIL、生产信任配置及此前显示链路无信号根因仍待验证，`productionReady=false`。
