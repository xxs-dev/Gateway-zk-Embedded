# 安科瑞 AM5 IEC103 主动 TCP 现场验证

## 验证对象

- 验证日期：2026-09-02
- 边端：`192.168.22.16`，Allwinner AArch64
- 保护装置：安科瑞 Feeder AM5
- 装置 IP：`192.168.22.191`
- 装置 TCP 模式：`Server`
- TCP 端口：`7710`
- UDP 端口：`1032`，主动 TCP 模式不依赖该端口
- IEC103 链路地址/公共地址：`1/1`
- 设备功能类型：`FUN=1`（馈线保护）

本次只验证读取、录波目录和录波文件拉取，不执行遥控、参数写入或定值修改。

## 配置结论

装置显示 TCP `Server`，边端必须使用：

```json
{
  "protocol": {
    "type": "iec103",
    "tcp": {
      "host": "192.168.22.191",
      "port": 7710,
      "connectTimeoutMs": 1500,
      "timeoutMs": 1000
    },
    "iec": {
      "transportMode": "am5se_active_tcp",
      "linkAddress": 1,
      "commonAddress": 1,
      "deviceFunctionType": 1,
      "pollTimeoutMs": 1000,
      "maxPollFrames": 64
    }
  }
}
```

`am5se_passive_tcp` 只适用于装置主动反连边端的部署方式，不能用于本设备当前网络设置。

## 验证结果

| 项目 | 结果 | 现场数据 |
| --- | --- | --- |
| 网络可达 | 通过 | Ping 无丢包，TCP `7710` 可连接 |
| 链路复位 | 通过 | 装置确认 FT1.2 复位帧 |
| 总召唤 | 通过 | 返回 ASDU44，确认 `FUN=1`、`INF=100` 起始遥信 |
| 一级数据 | 通过 | 45 个遥信/告警点有效 |
| 二级数据 | 通过 | 58 个遥测点有效，包含电流、角度、电度等 |
| 增量质量保持 | 通过 | 遥信与遥测交替返回时共 103 点保持 `quality=1`，无质量抖动 |
| 共享内存 | 通过 | 103 个现场返回点全部可读，坏质量 0、过期 0 |
| SQLite 存盘 | 通过 | 20 秒测试写入 683 条、覆盖 103 个点 |
| 进程稳定性 | 通过 | 20 秒持续运行，RSS 约 22 MiB，CPU 约 1.6% |
| 冷重连 | 通过 | 连续三次重新建链和总召均成功，耗时 422/132/175 ms |
| 录波目录 | 通过 | 返回 `FAN=387..402` 共 16 条记录 |
| 录波拉取 | 通过 | `FAN00402.CFG` 1446 字节，`FAN00402.DAT` 11520 字节 |

现场完整模板共 108 点。装置本次实际返回 103 点；下列 5 个遥测在持续轮询中从未出现，应标记为“当前固件未提供”，不能补零冒充有效数据：

- `INF=198`：`Ia_P`
- `INF=199`：`Ib_P`
- `INF=200`：`Ic_P`
- `INF=201`：`IA_P`
- `INF=202`：`IB_P`

## 发现并修复的问题

旧实现把 TCP 上的 IEC103 当成普通单次总召：发送总召后只读取确认帧，没有继续发送一级/二级数据请求，因此出现“TCP 已连接但没有点值”。现已由 AM5/AM5SE 客户端统一执行链路复位、总召、一级和二级轮询。

IEC103 返回是增量数据。一级响应不一定包含遥测，二级响应也不重复所有遥信。旧采集器会把本轮未出现的点立即置为坏质量，造成遥信和遥测交替失效。现改为：

- 成功轮询时只更新本帧实际出现的点。
- 未出现点保留上次有效值和原时间戳。
- 超过点位 TTL 后再由共享内存统一标记过期。
- 整轮通信失败时才写坏质量并标记设备离线。

录波 DAT 可能超过普通采集允许的 `maxPollFrames`。录波传输现按独立的总超时、15 位包号和
`recordingMaxFileBytes` 约束，不再被普通轮询帧数提前截断。

## 现场拉取录波

22.16 已保存不参与自动启动的诊断配置：

```text
/opt/modbus-gateway/config/diagnostics/device_am5_f_iec103.json
```

先查询装置当前录波目录：

```bash
/opt/modbus-gateway/bin/IecDriver \
  --config /opt/modbus-gateway/config/diagnostics/device_am5_f_iec103.json \
  --app-config /opt/modbus-gateway/config/runtime/apps/mqtt-service.json \
  --list-recordings
```

选择返回结果中的 `fan`，例如拉取 `FAN=409`：

```bash
/opt/modbus-gateway/bin/IecDriver \
  --config /opt/modbus-gateway/config/diagnostics/device_am5_f_iec103.json \
  --app-config /opt/modbus-gateway/config/runtime/apps/mqtt-service.json \
  --pull-recording 409 \
  --recording-output /opt/modbus-gateway/data/iec103-recordings
```

边端会同时生成 `FAN00409.CFG` 和 `FAN00409.DAT`。在 Windows PowerShell 下载：

```powershell
scp root@192.168.22.16:/opt/modbus-gateway/data/iec103-recordings/FAN00409.* `
  "$HOME\Downloads\IEC103\COMM202600999\"
```

同一设备配置已有常驻 `iec-driver@*.service` 时，不应再启动第二个独立驱动进程；应通过
SystemMonitor 的录波 MQTT 任务调用常驻驱动本地 socket。当前 Windows 客户端尚未提供录波
目录和下载页面。

## 尚未纳入本次验收

- IEC103 遥控、参数和定值写入：当前 AM5 专用驱动没有开放，现场测试也未执行。
- 平台 HTTPS 录波分片上传：本次已验证边端录波目录、CFG/DAT 拉取和本地传输服务测试；平台端到端上传需在平台测试任务中单独验收。
- 模板尾部 5 个测量点：需要结合该设备准确型号、固件版本和对应规约表确认是否应删除或采用其他 `FUN/INF`。
