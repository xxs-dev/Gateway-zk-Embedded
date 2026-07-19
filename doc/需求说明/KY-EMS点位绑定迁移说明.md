# KY-EMS 点位绑定迁移说明

## 目标

旧版 KY-EMS 画面直接使用 `VarList.xml` 中的 `appDataIndex`。
当前网关运行时应把 `meterCode + pointCode` 作为稳定业务键，把 `index`
作为运行期共享内存地址。

因此，新绑定建议使用如下格式：

```xml
<Bind widget="EnStorePData"
      meterCode="modbusRTU_1_slave_1"
      pointCode="mb1_s1_1139"
      indexFallback="1139"
      appDataIndex="1139"
      unit="kW"
      decimals="1"/>
```

`appDataIndex` 仅用于兼容老项目。

## 点位映射文件

`KY-EMS-Config.xml` 可以声明：

```xml
<PointMap path="KY-EMS-PointMap.example.json"/>
```

JSON 文件负责将稳定业务键映射到当前运行期 index：

```json
{
  "points": [
    {
      "meterCode": "modbusRTU_1_slave_1",
      "pointCode": "mb1_s1_1139",
      "index": 1139,
      "name": "储能电表总有功功率",
      "unit": "kW"
    }
  ]
}
```

界面取值顺序：

1. 优先通过 `PointMap` 中的 `meterCode + pointCode` 查找。
2. 找不到时使用 `indexFallback`。
3. 再找不到时使用旧版 `appDataIndex`。
4. 最后使用 `defaultValue`。

## 值映射

本地界面可以用文本映射显示数值状态：

```xml
<Bind widget="OverI"
      meterCode="pcs_1"
      pointCode="over_current_alarm"
      valueMap="0=正常;1=报警"
      decimals="0"/>
```

该文本只用于本地 UI 展示。MQTT 全量和实时载荷仍应保留数值和点位元数据，
由客户端根据本地配置自行翻译。

## 多状态指示

状态指示由一个灯控件和一个可选说明文字控件组成。单点枚举和多点组合统一使用 `<StateRules>`：

```xml
<Bind widget="DeviceStateLamp" type="image">
  <StateRules labelWidget="DeviceStateText"
              defaultCode="UNKNOWN" defaultLabel="未知" defaultColor="#AAB3BD">
    <State code="FAULT" label="故障" color="#D64545" priority="100" match="any">
      <Condition meterCode="BMS_1" pointCode="fault" indexFallback="1453"
                 operator="eq" value="1"/>
      <Condition meterCode="PCS_1" pointCode="fault" indexFallback="1301"
                 operator="eq" value="1"/>
    </State>
    <State code="RUNNING" label="运行" color="#2F9D78" priority="50" match="all">
      <Condition meterCode="BMS_1" pointCode="run_state" indexFallback="1454"
                 operator="in" value="1,2"/>
    </State>
  </StateRules>
</Bind>
```

执行规则：

1. `State` 按 `priority` 从高到低求值，第一个命中项生效。
2. `match=all` 表示条件全部满足，`match=any` 表示任一满足。
3. 比较符支持 `eq/ne/gt/ge/lt/le/in/notIn`。
4. 条件优先按 `meterCode + pointCode` 从 PointMap 解析，找不到时使用 `indexFallback`。
5. 命中后同时更新灯色或图片及 `labelWidget` 的状态文字；没有命中时使用默认未知态。
6. KY-EMS 直接读取共享内存，不通过 MQTT，也不修改采集和控制链路。

## Win 工具导出规则

Win 工具导出 EMS 显示工程时，应同时生成：

- `KY-EMS-Config.xml`：界面控件按 `meterCode + pointCode` 绑定。
- `KY-EMS-PointMap.json`：当前项目点位路由表，包含运行期 index。

这样全局重排 index 时，不需要手工修改 Qt 画面 XML。
