# EMS 并机第一阶段部署测试

## 1. 用途和限制

本流程只验证多柜通讯、主控选举和自动柜号。当前版本不生成 PCS 写命令，状态文件固定输出 `controlWritesEnabled=false`，不能作为真实并机功率控制验收。

普通服务启动不会自动修改并机网卡，即使旧配置中存在 `autoConfigureAddress=true` 也只记录警告。必须由操作员显式执行 `--prepare-network`，确认地址和默认路由后再启动服务。

首期使用专用实体网口，默认 `ens2`。`ens0` 保留维护，4G、EasyTier、MQTT 继续使用原上行路由。三至五柜使用工业交换机，两柜可网线直连。

## 2. 配置前检查

1. 每台柜的 `machineCode` 必须唯一。
2. `runtimeMode` 必须为 `ems`，`ComputeEngine` 必须正常运行并生成 `/opt/modbus-gateway/run/compute-engine-health.json`。
3. 所有柜的 `clusterId`、PSK、预期柜数和协议参数一致。
4. 不能把仍为 `192.168.3.250/24` 的多个 `ens2` 直接接到同一交换机。
5. 两柜项目接受断链后双方退出并机；否则应增加第三方见证节点后再进入控制阶段。

## 3. 地址准备

先保持并机网线断开，在每台柜本地执行：

```bash
/opt/modbus-gateway/bin/EmsClusterCoordinator \
  --app-config /opt/modbus-gateway/config/runtime/apps/mqtt-service.json \
  --prepare-network
```

`autoLinkLocal` 会根据 `machineCode + MAC` 生成稳定候选，执行三次 ARP DAD，移除该口的出厂地址并应用 `/16` 地址。命令不会添加默认路由和 DNS。缺少 `arping`、地址冲突或网卡不存在时必须停止，不允许跳过。

检查：

```bash
ip -4 address show dev ens2
ip -4 route show default
cat /opt/modbus-gateway/config/runtime/network/ems-cluster-address.json
```

确认默认路由不经过 `ens2` 后，再把各柜接入并机交换机。

## 4. 启动和状态

```bash
systemctl restart gateway-services.service
systemctl status ems-cluster@mqtt-service.service --no-pager
journalctl -u ems-cluster@mqtt-service.service -n 100 --no-pager
cat /opt/modbus-gateway/run/ems-cluster-status.json
```

健康集群应满足：

- 只有一个节点 `role=leader`。
- 其他节点为 `follower`。
- `onlineMembers` 等于现场柜数。
- `quorumValid=true`。
- 所有节点 `membershipEpoch` 一致且 `cabinetNo` 不重复。
- `metricsComplete=true`、`computeHealthy=true`。
- `controlWritesEnabled=false`。

## 5. 故障验证

1. 停止主控协调器，确认租约到期后多数派只产生一个新主控。
2. 三柜隔离原主控，确认原主控退出 `leader`，另外两柜重新选举。
3. 两柜拔线，确认两边都没有有效主控。
4. 重启任一柜，确认 `term` 不回退、柜号不漂移。
5. 临时制造重复 `machineCode`，确认节点进入 `quarantined`，不能投票。
6. 检查 `ip route` 和 `/run/gateway-network-failover/state`，确认并机口未成为公网出口。

## 6. 回退

将 `emsCluster.enabled` 改为 `false`，再执行：

```bash
systemctl restart gateway-services.service
systemctl is-active ems-cluster@mqtt-service.service
```

服务应为 inactive。网络地址恢复必须使用后续 KY-EMS 网络配置页面或经审核的现场网络脚本，不要在远程 SSH 会话中直接清空当前维护口地址。
