# 北向 Modbus 映射服务

本功能用于把边端已采集到共享内存的点位，对外映射为 Modbus TCP slave/server。

命名上区分两条链路：

- 南向采集：边端作为 master/client，采集下游 Modbus RTU/TCP、DLT645、CAN、DIDO 等设备，并写入共享内存。
- 北向服务：边端作为 slave/server，第三方主站读取边端映射出的寄存器/线圈，数据来源是共享内存。

因此配置使用 `northboundServer` 和 `point.northbound`，不使用 `collect_forward`，避免误解成“主站采集主站转发”。

## 设备级服务配置

设备配置根节点：

```json
{
  "northboundServer": {
    "enabled": false,
    "mode": "mapped",
    "protocol": "modbus_tcp",
    "bindHost": "0.0.0.0",
    "port": 1502,
    "requestTimeoutMs": 1000,
    "idleTimeoutMs": 60000,
    "maxClients": 8,
    "writesEnabled": false,
    "maxReadRegisters": 125,
    "maxReadBits": 2000,
    "allowedClientCidrs": ["192.168.22.0/24"],
    "allowUnrestrictedClients": false,
    "bindRetryIntervalMs": 5000
  }
}
```

- `mode=mapped`：按点位 `northbound` 映射共享内存值，是当前唯一实现；`hybrid` 目前与 `mapped` 相同；`passthrough` 预留，配置后北向服务拒绝启动。
- `protocol` 只支持 `modbus_tcp`。
- `allowedClientCidrs`：客户端白名单，元素可以是 IPv4 网段（`192.168.22.0/24`）、单个 IPv4 地址或单个 IPv6 地址。
  监听 `::`（双栈）时 IPv4 客户端以 `::ffff:a.b.c.d` 形式接入，按 IPv4 地址匹配白名单。
- 白名单为空时不限制客户端，但**监听通配地址**（`bindHost` 为空、`0.0.0.0`、`::`、`*`）且白名单为空会拒绝启动，
  除非显式 `allowUnrestrictedClients=true`。因此出厂默认（`0.0.0.0` + 空白名单）启用北向时必须填白名单或具体 `bindHost`。
- `writesEnabled=false` 是安全默认值。北向写控制后续需要结合高优先级控制租约统一放开。
- `bindRetryIntervalMs`：端口被占用等原因监听失败时按此间隔重试（100–600000 ms）。

## 点位级映射配置

点位可配置：

```json
{
  "northbound": {
    "enabled": true,
    "unitId": 1,
    "area": "holding_register",
    "readFunction": 3,
    "address": 100,
    "length": 2,
    "dataType": "float32",
    "scale": 0.1,
    "offset": 0,
    "byteOrder": "ABCD",
    "onInvalid": "sentinel"
  }
}
```

`area` 可选：

- `holding_register` -> 功能码 3
- `input_register` -> 功能码 4
- `coil` -> 功能码 1
- `discrete_input` -> 功能码 2

寄存器区的 `dataType`、`length`、`byteOrder` 必须一致（未写时继承点位 `read` 的同名字段）：

| dataType | length（寄存器数） | byteOrder 字母数 |
| --- | --- | --- |
| `uint16` `int16` `bool` `bit` | 1 | 2（如 `AB`、`BA`） |
| `uint32` `int32` `float32` | 2 | 4（如 `ABCD`、`CDAB`） |
| `uint64` `int64` `float64` | 4 | 8 |

`byteOrder` 是前 N 个字母各出现一次的排列（`A` 为规范大端的最高字节），为空表示大端。线圈/离散输入每个点占 1 位，
值非 0 读为 1。

### 无效值：`onInvalid`

点值缺失、质量坏、过期（stale），或换算后超出 `dataType` 能表示的范围（如负数写入 `uint16`、NaN）时，
只影响这个映射本身，同一读请求里的其他点照常返回：

- `sentinel`（默认）：返回哨兵值。未配置 `sentinel` 时用该类型的“不可用”标记（SunSpec 惯例）：
  `uint16` 0xFFFF、`int16` 0x8000、`uint32` 0xFFFFFFFF、`int32` 0x80000000、`uint64` 全 1、`int64` 最小值、
  `float32`/`float64` NaN；线圈/离散输入为 0。`sentinel` 可配置为该类型的原始值（不经 scale/offset）。
- `hold`：返回本服务启动以来最后一次有效值；还没有有效值时返回哨兵值。

### 读请求规则

- 一个读请求内，每个多寄存器值只从共享内存取一次快照再按 `byteOrder` 展开，不会出现高低字来自两次采集的拼接值。
- 读请求必须覆盖完整的值：从 32/64 位值中间开始、或在其中间结束的请求返回异常 02。
- 请求范围内有未映射的地址返回异常 02；数量为 0 或超过 `maxReadRegisters`/`maxReadBits` 返回异常 03。
- 单元号：请求的单元号有映射时按其应答；单元号 0 或 255 在只配置了一个单元时视为该单元（Modbus TCP 惯例，
  直接寻址网关本身）；配置了多个单元时 0/255 需显式映射，否则返回异常 02。

## 配置校验与故障隔离

加载设备配置时校验北向定义（协议/模式、白名单格式、通配监听无白名单、映射地址范围、类型/长度/字节序一致、
`onInvalid`/`sentinel`、地址重叠）。问题以 `config warning: northbound server will not start: …` 输出，
**不会**拒绝整个设备配置：采集照常运行，只有北向服务拒绝启动，并发状态事件 `northbound-refused`（消息含点名与修正方法）。

运行期监听失败（端口占用、地址不存在）发状态事件 `northbound-bind-failed` 并按 `bindRetryIntervalMs` 重试，
恢复监听后发 `northbound-listening`。这些都不影响采集。

## 当前边界

- 当前实现支持 Modbus TCP 北向 server 的读功能码 1/2/3/4。
- 写功能码 5/6/15/16 一律返回异常（`writesEnabled=false` 时 01，否则 02）；点位 `writeEnabled` 目前不生效。
- RTU slave/server 和透传模式未实现。
