# 北向 Modbus 映射服务

本功能用于把边端已采集到共享内存的点位，对外映射为 Modbus TCP slave/server。

命名上区分两条链路：

- 南向采集：边端作为 master/client，采集下游 Modbus RTU/TCP、DLT645、CAN、DIDO 等设备，并写入共享内存。
- 北向服务：边端作为 slave/server，第三方主站读取边端映射出的寄存器/线圈，数据来源是共享内存。

因此配置使用 `northboundServer` 和 `point.northbound`，不使用 `collect_forward`，避免误解成“主站采集主站转发”。

## 设备级服务配置

设备配置根节点新增：

```json
{
  "northboundServer": {
    "enabled": false,
    "mode": "mapped",
    "protocol": "modbus_tcp",
    "bindHost": "0.0.0.0",
    "port": 1502,
    "requestTimeoutMs": 1000,
    "maxClients": 8,
    "writesEnabled": false,
    "maxReadRegisters": 125,
    "maxReadBits": 2000,
    "allowedClientCidrs": []
  }
}
```

- `mode=mapped`：按点位 `northbound` 映射共享内存值，是当前默认实现。
- `mode=hybrid`：预留给后续“映射优先，部分地址透传”。
- `mode=passthrough`：预留，不默认启用。
- `allowedClientCidrs` 为空表示不限制；可填 `192.168.22.0/24` 或单个 IP。
- `writesEnabled=false` 是安全默认值。北向写控制后续需要结合高优先级控制租约统一放开。

## 点位级映射配置

Modbus 点位可新增：

```json
{
  "northbound": {
    "enabled": true,
    "unitId": 1,
    "area": "holding_register",
    "readFunction": 3,
    "address": 100,
    "length": 1,
    "dataType": "uint16",
    "scale": 0.1,
    "offset": 0,
    "byteOrder": "AB",
    "writeEnabled": false,
    "writeFunction": 6,
    "stalePolicy": "exception"
  }
}
```

`area` 可选：

- `holding_register` -> 功能码 3
- `input_register` -> 功能码 4
- `coil` -> 功能码 1
- `discrete_input` -> 功能码 2

`stalePolicy` 可选：

- `exception`：无值、过期或质量坏时返回 Modbus exception code 4。
- `zero`：返回 0。
- `last_value`：有旧值时返回旧值。

## 当前边界

- 当前实现支持 Modbus TCP 北向 server 的读功能码 1/2/3/4。
- 写功能码 5/6/15/16 默认返回异常；即使配置了点位 `writeEnabled`，也需要设备级 `writesEnabled` 和控制链路进一步实现后才会真正下发。
- RTU slave/server 和透传模式仅作为配置方向预留，生产启用前需要单独联调。
