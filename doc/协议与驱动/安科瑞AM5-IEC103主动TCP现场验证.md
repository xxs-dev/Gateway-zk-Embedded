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
- 变位帧（ASDU 1/2）：本次验证的遥信来自总召唤 ASDU44，未验证变位帧。变位帧 DPI 的实际含义仍需厂家确认，或对照原始报文与现场分合状态验证。当前 1→分、2→合、0/3→坏质量 是代码采用的映射，不能视为该固件已验证结论。证据检索结果和只读对照步骤见下文“变位 DPI 证据与只读验证”。
- 无数据应答码：本次未记录装置对空的一级/二级数据请求回的功能码。驱动为此临时接受固定帧 ACK（FC0）作为“无数据”；用通道报文监测确认为 FC9 或 E5 后删除该兼容分支。

## 变位 DPI 证据与只读验证

状态：**现场待验证**。代码映射未改变：ASDU 1/2 的 DPI 八位组 `0x01`→0（分）、`0x02`→1（合）；DPI=0/3 以及第 3..8 位（IEC 60870-5-103 中的备用位）不为 0 的八位组一律坏质量、保留旧值，不转成有效分/合。

### 仓库内证据（2026-10-30 检索）

| 来源 | 型号/固件 | 帧类型 | 能否对应实际分合 |
| --- | --- | --- | --- |
| 本文（2026-09-02，`192.168.22.191:7710`） | 仅记“安科瑞 Feeder AM5”，未记录固件版本 | 总召唤 ASDU44（`FUN=1`、`INF=100` 起），未收到 ASDU 1/2 | 否：遥信 `quality=1` 只说明解码和质量正常，没有逐点对照装置面板位置 |
| [163 实机记录](../releases/am5-soe-163-real-20261029.md) | 同一装置，未记录固件版本 | 启动总召 ASDU44、ASDU23、ASDU8，观察期 381 帧 ASDU10；两份抓包均无 ASDU 1/2 COT1 | 否：抓包存放在仓库外 `D:/workspace/new/am5-soe-20261029/live163-v7-r2/evidence`，仓库内无原始帧 |
| [22.7 模拟记录](../releases/am5-soe-22-7-20261029.md) | 回环模拟器 | 模拟 ASDU 1/2 | 否：模拟数据，不是厂家语义依据 |
| [AM5SE-F原生SOE全流程操作手册](AM5SE-F原生SOE全流程操作手册.md) 第 4 节 | 无 | 教学报文 | 否：文中已注明是教学数据 |
| `tools/iec_codec_test.cpp`、`tools/iec_collector_incremental_test.cpp`、`tools/am5se_*_test.cpp` | 无 | 构造帧 | 否：只验证解码与诊断保留 |
| `config/examples/device_am5se_iec103_*_example.json` | 无 | 点表（`typeId=44`、`FUN=1`） | 否：点名不证明 DPI 极性 |
| 厂家规约、固件说明、地址表 | 仓库内未找到 | — | — |

`evidence/` 下的压缩包和清单只含构建日志和源文件列表，没有 AM5 报文。结论：现有证据**不足以**确认 ASDU 1/2 的 DPI 语义，也不足以确认 ASDU44 状态位与实际分合的对应关系。

### 点值诊断

IEC103 遥信点值（ASDU 1/2 DPI 和 ASDU44 状态位）在采集结果中同时保留：

- `rawHex`：整个 ASDU 原始字节。
- `qualityMsg`：坏质量原因，例如 `IEC103 DPI=3 has no on/off mapping` 或 `IEC103 DPI octet 0x06 has spare bits 3..8 set; not an on/off value`。
- `text`：解释结果后接一段诊断，例如：

```text
off [iec103 asdu=1 cot=1(spontaneous) ca=1 fun=1 inf=100 dpiOctet=0x01 dpi=1 value=0 quality=1 mapping=iec103-standard-dpi(1=off->0,2=on->1,0/3=bad-hold) verified=false]
1 [iec103 asdu=44 cot=9(gi) ca=1 fun=1 inf=100 block=0 bitIndex=0 status=0x0001 change=0x0000 qds=0x00 bit=1 value=1 quality=1 mapping=asdu44-status-bit(1->1,0->0) verified=false]
```

`asdu=`/`cot=` 区分变位帧（ASDU 1/2，COT 1）、ASDU 1/2 形式的总召应答（COT 9）和 ASDU44 总召快照；坏质量保留旧值时显示 `value=held(旧值)`。`verified=false` 固定输出，表示映射未经厂家资料或现场记录确认。

共享内存只存值、质量和时间，不存上述文字，`IecDriver` 也不经 MQTT 发布采集结果，因此现场的诊断记录来自日志：设置 `GATEWAY_IEC103_DEBUG=1` 后，驱动除每个 ASDU 1/2 变位帧一行原始帧日志（含 `DPI=` 与 `DPIoctet=`、CP32 字段、整帧字节）外，还对匹配到点位的每个 ASDU 1/2 值、以及存储状态或质量发生变化的 ASDU44 值各输出一行 `IEC103 point <pointCode> index=<n> iec103 ...`。默认关闭。

### 最小只读验证步骤

前提：不发送遥控、分合闸、复位、校时或定值/参数命令；不修改装置设置和网关设备配置文件；只利用自然发生的变位，或由现场负责人独立批准、由运行人员在装置侧完成的操作。网关只记录，不触发。

1. 记录装置铭牌型号、装置菜单或厂家工具读出的程序/固件版本、链路地址和公共地址，以及待核对信号的 `FUN/INF` 和厂家地址表含义（有则附上）。
2. 抓原始报文，二选一：
   - 通道报文监测（[接口说明](../配置与消息/通道报文监测接口.txt)）：不重启驱动、不新建设备连接，每个会话最长 300 秒，适合已约定时间的操作；需维护鉴权。
   - `GATEWAY_IEC103_DEBUG=1`：通过 systemd 覆盖给 `iec-driver@<配置文件名>.service` 加环境变量，需要重启驱动（会重新建链、总召唤），只能在获准的维护窗口进行；结束后删除覆盖并再次重启。
3. 变位发生前，记录装置面板或厂家工具显示的该信号当前位置，并保存一次总召后的 ASDU44 状态位（日志中的 `asdu=44 cot=9(gi)` 行，或通道报文）。
4. 变位发生时，记录：网关收到时间、整帧原始字节（`raw=`/通道报文 hex）、ASDU 类型、COT、`FUN/INF`、`DPIoctet`、CP32（时:分:秒.毫秒、IV、SU）；同时记录装置面板、事件记录或厂家工具显示的位置、事件时间和操作人。
5. 下一次总召唤后，记录同一 INF 的 ASDU44 状态位，核对与面板位置是否一致。
6. 同一信号至少各观察一次“分→合”和“合→分”；若出现 DPI=0/3 或备用位非 0 的八位组，原样记录，不按分/合解释。

### 标记“映射已确认”所需记录

只有同时具备以下记录并经审核后，才可把对应信号的映射标为已确认；在此之前诊断一直输出 `verified=false`，也不得把模拟器或构造帧测试当作依据：

- 装置型号、固件/程序版本、链路地址/公共地址、`FUN/INF` 及厂家含义；
- 每个方向至少一条完整事件记录：原始帧字节及网关时间、解出的 ASDU/COT/DPI 八位组/CP32、装置侧显示的实际位置与事件时间、操作来源（自然发生或批准单号）；
- 操作前后 ASDU44 状态位与实际位置的对照；
- 记录人、审核人、日期和证据文件存放位置（含校验值）。

固件版本或装置型号不同，需要重新确认。目前代码没有“映射已确认”配置项；确认后如何在配置或代码中标记，需用户决定。
