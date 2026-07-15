# BMS 标准状态枚举与画面绑定

## 目标

画面只识别稳定的 BMS 标准状态，不直接依赖某个厂家的寄存器值。更换 BMS 厂家或型号时，只修改工程 PointMap 中的原始值映射，不修改 Qt 画面配置和代码。

数据链路：

```text
设备原始寄存器值
  -> 采集驱动共享内存
  -> PointMap.semanticValueMap
  -> BMS 标准枚举
  -> KY-EMS activeStates
  -> 状态灯
```

## 固定枚举

### BmsRunState

| 名称 | 标准值 | 含义 |
|---|---:|---|
| `NORMAL` | 0 | 正常/就绪 |
| `CHARGE_DISABLED` | 1 | 禁止充电 |
| `DISCHARGE_DISABLED` | 2 | 禁止放电 |
| `STANDBY` | 3 | 待机 |
| `STOPPED` | 4 | 停机 |
| `FAULT` | 5 | 故障 |

### BmsChargeState

| 名称 | 标准值 | 含义 |
|---|---:|---|
| `IDLE` | 0 | 静置 |
| `DISCHARGING` | 1 | 放电 |
| `CHARGING` | 2 | 充电 |

### BmsPrechargeState

| 名称 | 标准值 | 含义 |
|---|---:|---|
| `OFFLINE` | 0 | 断网/未启动 |
| `STARTING` | 1 | 启动并网 |
| `CONNECTING` | 2 | 并网中 |
| `CONNECTED` | 3 | 并网成功 |
| `FAILED` | 4 | 并网失败 |

### BmsContactorState

| 名称 | 标准值 | 含义 |
|---|---:|---|
| `OFF` | 0 | 主接触器断开 |
| `PRECHARGING` | 1 | 预充过程中 |
| `ON` | 2 | 主正、主负接触器闭合 |
| `ABNORMAL` | 3 | 接触器位组合异常 |

接触器原始点是位图，不能直接用非零判断上电。当前优旦协议按主正、预充、主负、隔离开关四个位的组合显式映射为上述标准状态。

## 厂家映射

PointMap 中的状态点必须声明语义、枚举类型和原始值映射。例如某厂家以 `10/20/30` 表示正常、禁充、禁放：

```json
{
  "index": 1550,
  "semanticRole": "bms.runState",
  "enumType": "BmsRunState",
  "semanticValueMap": {
    "10": "NORMAL",
    "20": "CHARGE_DISABLED",
    "30": "DISCHARGE_DISABLED"
  }
}
```

`semanticValueMap` 中未声明的原始值按无效数据处理，画面显示灰灯，不能把未知值误判成某个状态。

## 画面绑定

画面使用标准状态名，不再使用厂家数值：

```xml
<Bind widget="BatCharge"
      type="image"
      semanticRole="bms.chargeDischargeState"
      activeStates="CHARGING"
      value0Image=":/Pictures/Circle2.png"
      value1Image=":/Pictures/Circle1.png"/>
```

共享内存质量无效、数据过期、语义未命中或枚举映射未命中时，图片统一切换为 `value0Image`，不会保留设计时或上一次的亮灯状态。

## 22.16 当前映射

| 共享内存 index | 采集点 | 标准语义 |
|---:|---|---|
| 1454 | 充放电指示 | `bms.chargeDischargeState` |
| 1462 | 故障位图 | `bms.fault` |
| 1550 | 运行状态 | `bms.runState` |
| 1554 | 预充电阶段 | `bms.prechargeState` |
| 1555 | 接触器状态 | `bms.contactorState` |
| 8197 | 一级报警 | `bms.alarm.level1` |
| 8198 | 二级报警 | `bms.alarm.level2` |
| 8199 | 三级报警 | `bms.alarm.level3` |

当前优旦协议的状态数值与标准值相同，但 PointMap 仍显式配置映射，以保证画面与厂家协议解耦。
