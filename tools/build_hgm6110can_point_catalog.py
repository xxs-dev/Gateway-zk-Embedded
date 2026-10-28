"""Build an offline protocol catalog, not an enabled device configuration."""

import argparse
import hashlib
import html
import json
import math
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "config/templates/smartgen_hgm6110can_v1_7"
POINTS = []


def point(fc, address, name, page, dtype="uint16", scale=1, unit="", **extra):
    item = dict(pointCode=f"hgm6110can_fc{fc:02d}_{address:04d}", name=name,
                functionCode=fc, address=address,
                plcAddress=address + (40001 if fc == 3 else 1),
                access="write" if fc == 5 else "read", dataType=dtype,
                length=2 if dtype in ("uint32", "int32") else 1,
                scale=scale, offset=0, unit=unit,
                sourcePages=[page], enabledByDefault=False,
                category="control" if fc == 5 else "status" if fc == 1 else "telemetry")
    if fc == 3:
        item["byteOrder"] = "ABCD" if item["length"] == 2 else "AB"
        item["invalidValueRule"] = "raw32766"
    item.update(extra)
    POINTS.append(item)
    return item


def enum(*names):
    return {str(i): name for i, name in enumerate(names)}


COILS = """公共报警|公共警告报警|公共停机报警|远程模式|远程锁机|保留|市电带负载|发电带负载|紧急停机|超速报警停机|欠速报警停机|速度信号丢失停机|超频报警停机|欠频报警停机|过压报警停机|欠压报警停机|发电过流停机|起动失败|水温高报警停机|油压低报警停机|频率丢失报警|输入口停机报警|燃油位低停机报警|冷却液低停机报警|水温高警告报警|油压低警告报警|发电过流警告报警|停机失败警告报警|油位低警告|充电失败警告|电池电压过低警告报警|电池电压过高警告报警|输入口警告报警|速度信号丢失警告|冷却液低警告|温度传感器开路警告|油压传感器开路警告|维护时间到警告|充电器充电失败警告|过功率警告|系统在测试模式|系统在自动模式|系统在手动模式|系统在停机模式|温度传感器开路停机|油压传感器开路停机|维护时间到停机报警|过功率停机报警|紧急停机输入|可编程输入口1|可编程输入口2|可编程输入口3|可编程输入口4|可编程输入口5|保留|保留|起动继电器输出|燃油继电器输出|可编程输出口1|可编程输出口2|可编程输出口3|可编程输出口4|燃油1输出控制|漏液警告|市电故障|市电正常|市电过压|市电欠压|市电缺相|市电无|保留|保留|发电正常|发电过压|发电欠压|发电过频|发电欠频|发电过流|定时不开机时间指示|ECU警告|ECU停机报警|ECU通信失败报警|高温报警停机（模拟量温度传感器）|低油压报警停机（模拟量油压传感器）|漏液停机|保留|保留|保留|再生中|即将激活严重限制|DPF过载需要服务再生|DPF满载请进行驻车再生|DPF正在再生请不要停机|DPF再生被禁止|DPF需要再生请不要停机|发动机警告故障|行车再生|需进行驻车再生|需服务站进行再生|需进行驻车再生|后处理排温高|再生请勿合闸带载|请在水温>50°C下再生|需进行主动再生|HEST指示|DPF禁止指示|NCD报警|NCD即将限制|NCD初级限制|NCD严重限制|需服务再生|正在驻车再生|MIL|SVS|巡航指示|油中含水|排气制动|尿素液位低|驾驶员报警|DPF|发动机停机故障|保留|保留|保留|保留|保留|保留|保留""".split("|")
assert len(COILS) == 128
for address, name in enumerate(COILS):
    if name != "保留":
        point(1, address, name, 6 if address <= 36 else 7 if address <= 71 else 8 if address <= 111 else 9,
              "bit", valueMap={"0": "未置位", "1": name + "（有效）"})

for base, label in [(0, "市电"), (7, "发电")]:
    for i, phase in enumerate(["UA", "UB", "UC", "UAB", "UBC", "UCA"]):
        point(3, base + i, label + phase, 10, unit="V")
    point(3, base + 6, label + "频率", 10, scale=0.1, unit="Hz",
          verificationRequired=True, notes="表6为0.1Hz；第17页注1示例为0.01Hz，现场核实前不可用于控制或合格判定。")
for i, phase in enumerate("ABC"):
    point(3, 14 + i, phase + "相电流（16位）", 10, unit="A")

def rows(text, page):
    for row in text.strip().splitlines():
        address, name, dtype, scale, unit = row.split("|")
        point(3, int(address), name, page, dtype, float(scale), unit)


rows("""
17|水温温度值|uint16|1|°C
18|水温电阻值|uint16|0.1|Ω
19|油压值|uint16|1|kPa
20|油压电阻值|uint16|0.1|Ω
21|液位值|uint16|1|%
22|液位电阻值|uint16|0.1|Ω
23|转速|uint16|1|RPM
24|电池电压|uint16|0.1|V
25|D+电压|uint16|0.1|V
26|有功功率（16位）|int16|1|kW
27|无功功率（16位）|int16|1|kvar
28|视在功率（16位）|int16|1|kVA
29|功率因数|int16|0.01|
30|维护倒计时小时|uint16|1|h
31|维护倒计时分|uint16|1|min
33|燃气阀开度|int16|0.01|%
""", 10)
point(3, 32, "点火提前角（原始字）", 10, scale=None, rawOnly=True,
      verificationRequired=True, notes="有无符号未注明；ECU51/73倍率1、单位deg，其他ECU倍率0.1、单位CA。确认前仅保留原始字。",
      scaleProfiles=[dict(ecuTypes=[51, 73], scale=1, unit="deg"), dict(ecuTypes="other", scale=0.1, unit="CA")])
point(3, 34, "控制器运行状态", 11, category="status", sourcePages=[11, 18, 19],
      valueMap=enum("待机", "预热", "燃油输出", "起动", "起动间隔", "安全延时", "开机怠速", "高速暖机", "等待带载", "正常运行", "高速散热", "停机怠速", "得电停机", "等待停稳", "停机失败", "过停稳"))
point(3, 36, "自动运行状态", 11, category="status", sourcePages=[11, 19], valueMap=enum("开机延时", "停机延时", "无延时"))
point(3, 38, "ATS运行状态（原始码）", 11, rawOnly=True, notes="协议未提供状态枚举，不猜测含义。")
point(3, 40, "市电状态", 11, category="status", valueMap=enum("正常", "异常", "无延时"))
for address, label in [(35, "控制器运行"), (37, "自动运行"), (39, "ATS运行"), (41, "市电状态")]:
    point(3, address, label + "延时", 11, unit="s", notes="倍率栏留空，按原始秒值保留。")
for address, name, unit in [(42, "累计运行小时", "h"), (46, "累计开机次数", "次"), (48, "累计电能", "kWh")]:
    for i, label in enumerate(["高位", "低位"]):
        point(3, address + i, name + label, 11, unit=unit, rawOnly=True,
              notes=f"组成量；实际总量=R{address}*10000+R{address + 1}，不是二进制uint32。",
              rawRange=[0, 9000 if i == 0 else 9999])
rows("""
44|累计运行分钟|uint16|1|min
45|累计运行秒|uint16|1|s
50|软件版本|uint16|0.1|
51|硬件版本|uint16|0.1|
52|A相有功功率|int16|1|kW
53|B相有功功率|int16|1|kW
54|C相有功功率|int16|1|kW
55|负载输出百分比|uint16|1|%
56|空燃比|int16|0.01|
57|节气门开度|int16|0.1|%
58|冷却液位|int16|1|%
59|机油温度|int16|1|°C
60|冷却液压力|int16|1|kPa
61|燃油压力|int16|1|kPa
62|燃油温度|int16|1|°C
63|进气口温度|int16|1|°C
64|排气口温度|int16|1|°C
65|涡轮压力|int16|1|kPa
66|燃油消耗|int16|0.1|L/h
""", 11)
rows("""
67|进气压力|int16|1|kPa
68|累计燃油消耗|int32|1|L
71|控制器型号（原始码）|uint16|1|
82|点火转速|uint16|1|RPM
83|尿素液位|uint16|0.1|%
84|ECU温度|int16|1|°C
85|环境压力|int16|1|kPa
86|ECU运行时间|int32|0.1|h
88|MTU FC|uint16|1|RPM
89|MTU Droop|uint16|0.1|%
90|MTU Demand Source（原始码）|uint16|1|
91|MTU SSD（原始码）|uint16|1|
""", 12)
for i, (name, low, high) in enumerate([("年", 0, 99), ("月", 1, 12), ("日", 1, 31), ("星期", 0, 6), ("时", 0, 23), ("分", 0, 59), ("秒", 0, 59)]):
    point(3, 72 + i, "控制器时间：" + name, 12, rawRange=[low, high],
          notes="年基准及星期0对应哪天未注明；不擅自转换日历。" if i in (0, 3) else "")
for i, name in enumerate(["发布年", "发布月", "发布日"]):
    point(3, 79 + i, name, 12)
for i, name in enumerate(["发电UA", "发电UB", "发电UC", "市电UA", "市电UB", "市电UC"]):
    point(3, 92 + i, name + "相位", 12, "int16", unit="°", rawRange=[0, 360])

for base, group in [(98, "DM1"), (128, "DM2")]:
    for i in range(10):
        address = base + 3 * i
        for offset, label in enumerate(["SPN低16位", "SPN高16位", "OC/FMI组合字"]):
            a = address + offset
            point(3, a, f"{group}报警{i + 1} {label}", 13 if a <= 125 else 14,
                  rawOnly=True, notes=f"SPN=R{address + 1}*65536+R{address}；OC=(R{address + 2}>>8)&255；FMI=R{address + 2}&255。SPN低字在前；故障语义需匹配发动机故障字典。")

rows("""
158|ECU报警停机数量|uint16|1|
159|ECU警告数量|uint16|1|
160|瞬时油耗|uint16|0.1|Km/L
162|尾气氧含量|uint16|0.01|%
163|涡前温度|int16|1|°C
164|燃料阀位置|uint16|0.1|%
165|尿素温度|int16|1|°C
166|尿素温度2|int16|1|°C
167|尿素浓度|uint16|0.1|%
168|尿素消耗|uint16|0.1|L/h
169|燃气温度|int16|1|°C
170|SCR温度|int16|1|°C
171|节气门位置2|int16|0.1|%
172|DPF烟尘量|int16|1|%
173|DPF灰烬量|int16|1|%
174|SCR出口温度|int16|1|°C
176|扭矩|int16|1|%
182|燃料阀位置2|uint16|1|%
183|曲轴箱压力|int16|1|kPa
184|中冷器温度|int16|1|°C
""", 15)
point(3, 161, "燃气压力（原始字）", 15, scale=None, rawOnly=True, verificationRequired=True,
      notes="有无符号未注明；ECU46倍率0.01，其他ECU倍率1，单位kPa。确认前仅保留原始字。",
      scaleProfiles=[dict(ecuTypes=[46], scale=0.01, unit="kPa"), dict(ecuTypes="other", scale=1, unit="kPa")])
for i in range(16):
    point(3, 185 + i, f"排气口温度{i + 1}", 15 if i < 8 else 16, "int16", unit="°C")
point(3, 201, "燃气压力2", 16, "int16", 0.1, "kPa")
for a, name in [(203, "上位机版本高16位"), (204, "上位机版本低16位")]:
    point(3, a, name, 16, rawOnly=True, notes="按4个字节显示版本；0601H/0407H -> 6.1.4.7。")
for base in (206, 2500):
    for i, (name, dtype, unit) in enumerate([("A相电流", "uint32", "A"), ("B相电流", "uint32", "A"), ("C相电流", "uint32", "A"), ("有功功率", "int32", "kW"), ("无功功率", "int32", "kvar"), ("视在功率", "int32", "kVA")]):
        a = base + i * 2
        point(3, a, name + f"（32位，地址{a}）", 17 if a == 2510 else 16, dtype, 0.1, unit,
              notes="与16位/另一扩展区为同一物理量的不同寄存器表示，不重复累计；连续双字必须同帧读取。高字在前假设需现场核对。",
              verificationRequired=True)

ICON = enum("无图标显示", "图标常亮", "图标闪烁")
def profile(types, mapping):
    return dict(ecuTypes=types, valueMap=mapping)


point(3, 175, "DPF积碳量指示", 15, category="status", sourcePages=[15, 21],
      enumProfiles=[dict(ecuTypes="all", ranges=[dict(min=0, max=8, label="无图标显示")]),
                    dict(ecuTypes=[62, 68], ranges=[dict(min=9, max=10, label="图标闪烁：需驻车再生"), dict(min=11, max=65535, label="图标常亮：需服务站再生")]),
                    dict(ecuTypes="other", ranges=[dict(min=9, max=10, label="图标常亮"), dict(min=11, max=65535, label="图标闪烁")])],
      verificationRequired=True, notes="先剔除无效值再匹配范围；这是状态码，不能当作积碳百分比。")
point(3, 177, "DPF再生提醒", 15, category="status", sourcePages=[15, 19], verificationRequired=True,
      enumProfiles=[profile([52], {"0": ICON["0"], "1": ICON["1"], "2": ICON["1"], "4": ICON["2"]}),
                    profile([59, 64, 70], {"0": ICON["0"], "1": ICON["1"], "4": ICON["2"]}),
                    profile([58, 61], enum("无图标显示", "图标常亮", "图标慢闪烁", "图标快闪烁")),
                    profile([66], enum("无图标显示", "图标常亮", "图标快闪烁", "图标更快闪烁（带底纹）")),
                    profile([65], {"0": ICON["0"], "1": ICON["1"], "2": "图标慢闪烁", "4": "图标快闪烁"}),
                    profile("other", {**ICON, "4": ICON["2"]})])
point(3, 178, "DPF再生禁止", 15, category="status", sourcePages=[15, 20], valueMap=enum("无图标显示", "图标常亮"))
point(3, 179, "DPF再生状态", 15, category="status", sourcePages=[15, 20], verificationRequired=True,
      enumProfiles=[profile([58, 61, 59, 64, 65, 66, 70], enum("无图标显示", "图标常亮")), profile("other", ICON)])
point(3, 180, "DPF状态指示", 15, category="status", sourcePages=[15, 20], valueMap=enum("无图标显示", "图标常亮", "图标慢闪烁", "图标快闪烁"))
point(3, 181, "驾驶员报警", 15, category="status", sourcePages=[15, 20], valueMap=enum("无图标显示", "NCD报警（常亮）", "NCD即将限制（常亮）", "NCD初级限制（常亮）", "NCD严重限制（常亮）", "驾驶员报警（常亮）", "驾驶员报警（闪烁）", "存在报警，无图标显示"))
point(3, 202, "SCR指示", 16, category="status", sourcePages=[16, 21], verificationRequired=True,
      enumProfiles=[profile([66], {"0": ICON["0"], "1": ICON["1"], "2": ICON["1"], "3": ICON["2"], "4": ICON["2"], "5": "图标反显闪烁", "6": "图标反显闪烁", "7": ICON["0"]})])
point(3, 218, "空挡状态指示", 16, category="status", sourcePages=[16, 21], verificationRequired=True,
      enumProfiles=[profile([65], enum("无图标显示", "图标常亮"))])
point(3, 219, "DEF液位低报警指示灯", 16, category="status", sourcePages=[16, 21], verificationRequired=True,
      enumProfiles=[profile([75], {"0": ICON["0"], "1": ICON["1"], "4": ICON["2"]})])

for a, name in enumerate(["遥控开机", "遥控停机", "遥控试机", "遥控自动", "遥控手动", "发电分闸", "发电合闸"]):
    point(5, a, name, 18, "bit", writeEnabled=False, verificationRequired=True,
          singleShot=True, automaticRetry=False, automaticReset=False,
          documentedWireValueHex="FF00", conflictingTableValueHex="00FF",
          notes="HGM6110命令；表格00FFH与报文示例FF00冲突，须确认。只发送一次；不可自动补发或写0复位；FC01同地址不是该命令回读。")

def lookup(fc, address):
    return next(p for p in POINTS if p["functionCode"] == fc and p["address"] == address)


lookup(3, 55)["notes"] = "倍率栏留空，暂按原始百分数记录，现场核对。"
lookup(3, 55)["verificationRequired"] = True
lookup(3, 160)["notes"] = "单位Km/L按原文保留，不转换成L/h。"
lookup(3, 17)["notes"] = "按协议无符号2Bytes整理；负温表示未说明。"
for a in [71, 90, 91]:
    lookup(3, a)["rawOnly"] = True
    lookup(3, a)["notes"] = "协议未给出码值对照，不臆造枚举。"
lookup(5, 3)["feedbackReference"] = {"functionCode": 1, "address": 41, "expectedValue": 1, "sourcePage": 18}

NOTES = [
    "适用型号：用户确认HGM6110CAN；通信为RS485 Modbus RTU，不是CAN总线点表。",
    "这是离线协议点表，不是可直接下发的网关设备配置。未连接或修改104。所有点默认禁用，控制另有writeEnabled=false。",
    "地址为十进制、从0开始；PLC地址仅用于对照，不能直接作为报文地址。FC01状态区与FC05命令区独立。",
    "工厂通信默认9600/8N1、从站1；现场端口、地址、速率未知，不假定104使用这些默认值。支持2400/4800/9600/19200/115200bps，从站1..254。",
    "协议要求超时>200ms，单次最多120字。接入建议先用1000ms轮询和1000ms超时，非设备刷新率承诺。保持串口独占，不跨保留区合并。",
    "第10页频率行倍率0.1Hz与第17页示例0.01Hz冲突；点表暂记录表格0.1并标记待核，不可直接用于控制。",
    "第18页FC05表格写00FFH，示例实际FF00；按示例保存报文候选值，但未启用写。6110地址5为分闸、6为合闸，不采用6120的切换命令。",
    "第17页规定原始数据32766表示无正常数据，应显示无效/###而非工程数值。现有通用数值解码路径没有本协议专用无效值过滤，接入必须补齐并验证。",
    "32766规则对双字、组合故障码的适用粒度未细说；双字整体值或任一组成字出现0x7FFE时保留原始数据并标记待核，不判正常、不随意置零。",
    "累计小时、累计开机次数、累计电能为高位*10000+低位；分开保留原始字，不能按uint32拼接。派生规则是文档元数据，尚未绑定运行时计算点。",
    "普通4字节值按高字*65536+低字，点表采用ABCD高字在前待现场核对；有符号采用二进制补码。DM1/DM2的SPN明确低字在前，不能套用同一字序。",
    "CAN扩展字段是否有效取决于发动机ECU支持；ECU型号未确认时条件枚举不得落入other分支。未知枚举显示未知(原始码)，不能显示正常。",
    "市电/ATS字段按家族协议保留，6110是否实现须接入确认；不将未支持字段的0值当作有效状态。CAN地址98..157为DM故障码，不混入N版油位历史。",
    "16位、206扩展区、2500扩展区功率/电流为替代表示，不累加；优先核验2500公共扩展区的0.1分辨率。",
    "协议称支持FC06但没有给出可写参数地址表，本点表不生成FC06写点。",
    "接入104前确认串口、从站地址、波特率/校验、固件及ECU型号，分配现场pointCode/index，补齐无效值/派生量/中文状态映射，再做只读对照。电气控制须另行授权验证。",
]

DERIVED = [
    dict(name="累计运行小时", addresses=[42, 43], expression="R42 * 10000 + R43", unit="h", sourcePage=17),
    dict(name="累计运行时间", addresses=[42, 43, 44, 45], expression="(R42 * 10000 + R43) * 3600 + R44 * 60 + R45", unit="s", sourcePage=17),
    dict(name="累计开机次数", addresses=[46, 47], expression="R46 * 10000 + R47", unit="次", sourcePage=17),
    dict(name="累计电能", addresses=[48, 49], expression="R48 * 10000 + R49", unit="kWh", sourcePage=17),
    dict(name="上位机兼容版本", addresses=[203, 204], expression="join('.', [R203 >> 8, R203 & 255, R204 >> 8, R204 & 255])", sourcePage=17),
]


def validate():
    keys = [(p["functionCode"], p["address"]) for p in POINTS]
    assert len(keys) == len(set(keys))
    assert len({p["pointCode"] for p in POINTS}) == len(POINTS)
    assert {a for fc, a in keys if fc == 1} == {a for a, name in enumerate(COILS) if name != "保留"}
    covered = []
    for p in POINTS:
        assert not p["enabledByDefault"]
        if p["functionCode"] == 3:
            covered.extend(range(p["address"], p["address"] + p["length"]))
        if p["functionCode"] == 5:
            assert not p["writeEnabled"] and p["singleShot"] and not p["automaticRetry"] and not p["automaticReset"]
    assert len(covered) == len(set(covered))
    assert set(covered) == (set(range(220)) - {70, 205}) | set(range(2500, 2512))
    assert lookup(5, 5)["name"] == "发电分闸" and lookup(5, 6)["name"] == "发电合闸"
    assert lookup(1, 62)["name"] == "燃油1输出控制"
    assert lookup(1, 79)["name"] == "ECU警告"
    assert lookup(3, 98)["name"] == "DM1报警1 SPN低16位"
    assert math.isclose(lookup(3, 24)["scale"] * int("0112", 16), 27.4)
    assert 1 * 10000 + 4 == 10004


def display_details(p):
    parts = []
    if "valueMap" in p:
        parts.append("；".join(f"{v}={label}" for v, label in p["valueMap"].items()))
    for field in ("enumProfiles", "scaleProfiles"):
        if field in p:
            parts.append(json.dumps(p[field], ensure_ascii=False))
    if p.get("rawOnly"):
        parts.append("原始组成量/原始码")
    if p.get("verificationRequired"):
        parts.append("待现场核实")
    if p.get("notes"):
        parts.append(p["notes"])
    return "；".join(parts)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    args = parser.parse_args()
    validate()
    points = sorted(POINTS, key=lambda p: (p["functionCode"], p["address"]))
    counts = dict(Counter(str(p["functionCode"]) for p in points))
    source = dict(fileName=args.source.name, sha256=hashlib.sha256(args.source.read_bytes()).hexdigest(),
                  revision="V1.7", revisionDate="2025-07-01", pageCount=23)
    data = dict(catalogSchemaVersion="1.0", artifactKind="offline-protocol-point-catalog",
                vendor="SmartGen", model="HGM6110CAN", protocol="modbus_rtu", source=source,
                deploymentReady=False, modelConfirmedByUser=True, deviceConnection=None,
                pointCount=len(points), countsByFunctionCode=counts,
                transportReference=dict(defaultSlave=1, defaultBaudRate=9600, dataBits=8, stopBits=1,
                                        defaultParity="none", timeoutMsExclusiveMinimum=200, maxReadRegisters=120),
                reservedAddresses={"1": [a for a, n in enumerate(COILS) if n == "保留"], "3": [70, 205]},
                integrationNotes=NOTES, derivedDefinitions=DERIVED, points=points)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "points.json").write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    summary = f"共{len(points)}个原始点/命令：FC01状态{counts['1']}点，FC03数值及组成字{counts['3']}点，FC05控制{counts['5']}点。保留地址不计入；派生量另列。"
    lines = ["# HGM6110CAN Modbus RTU 点表", "", summary, "", "## 使用边界", ""]
    lines += [f"- {note}" for note in NOTES]
    lines += ["", f"来源：{source['fileName']}，{source['revision']}（{source['revisionDate']}）。", f"SHA256：`{source['sha256']}`", "", "## 派生量（未绑定运行时）", ""]
    lines += [f"- {d['name']}：`{d['expression']}` {d.get('unit', '')}" for d in DERIVED]
    lines += ["", "所有派生量要求同一完整采集帧且源值有效；DM1/DM2的SPN、OC、FMI规则见各点说明。", ""]
    rows_html = []
    for fc, title in [(1, "状态与报警"), (3, "数值与故障码"), (5, "遥控命令（全部禁用）")]:
        lines += [f"## FC{fc:02d} {title}", "", "| 地址(十进制) | PLC地址 | 名称 | 类型/长度 | 倍率 | 单位 | 含义及注意事项 | 来源页 |", "|---:|---:|---|---|---|---|---|---|"]
        for p in points:
            if p["functionCode"] != fc:
                continue
            vals = [str(p["address"]), str(p["plcAddress"]), p["name"], f"{p['dataType']}/{p['length']}",
                    "待定" if p["scale"] is None else f"{p['scale']:g}", p["unit"], display_details(p), ",".join(map(str, p["sourcePages"]))]
            lines.append("| " + " | ".join(v.replace("|", "&#124;").replace("\n", " ") for v in vals) + " |")
            rows_html.append(f'<tr data-fc="{fc}"><td>FC{fc:02d}</td>' + "".join(f"<td>{html.escape(v)}</td>" for v in vals) + "</tr>")
        lines.append("")
    (args.output / "README.md").write_text("\n".join(lines), encoding="utf-8")
    page = '''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>HGM6110CAN 点表</title>
<style>body{margin:24px;color:#20262b;background:#fff;font:14px/1.6 "Microsoft YaHei",sans-serif;letter-spacing:0}h1{font-size:25px}header{border-bottom:3px solid #18816a}input,select{font:inherit;padding:8px;border:1px solid #999;margin:8px 12px 12px 0;max-width:90%}input{width:340px}.table{overflow:auto;max-height:75vh}table{border-collapse:collapse;width:100%;min-width:1100px}th{position:sticky;top:0;background:#e3efec;text-align:left}th,td{padding:9px 12px;border-bottom:1px solid #dce1e3;vertical-align:top}tr:nth-child(even){background:#f7f9fa}td:nth-child(4){min-width:170px}td:nth-child(8){min-width:380px;overflow-wrap:anywhere}details{margin:18px 0}a{color:#006f5b}</style>
<header><h1>HGM6110CAN · Modbus RTU 点表</h1><p>SUMMARY</p><p>离线整理，未部署104；控制全部禁用。</p></header>
<details><summary>协议说明与接入待核项</summary><ul>NOTES</ul></details>
<p><a href="points.json">JSON点表</a> · <a href="README.md">完整中文说明</a></p>
<input id="q" aria-label="搜索点位" placeholder="搜索名称、地址或含义"><select id="fc" aria-label="功能码"><option value="">全部功能码</option><option value="1">FC01 状态报警</option><option value="3">FC03 数值故障码</option><option value="5">FC05 禁用控制</option></select><span id="count"></span>
<div class="table"><table><thead><tr><th>功能码</th><th>地址</th><th>PLC地址</th><th>名称</th><th>类型/长度</th><th>倍率</th><th>单位</th><th>含义及注意事项</th><th>协议页</th></tr></thead><tbody>ROWS</tbody></table></div>
<script>const q=document.getElementById('q'),fc=document.getElementById('fc');function filter(){let n=0;for(const r of document.querySelectorAll('tbody tr')){r.hidden=!!((fc.value&&r.dataset.fc!==fc.value)||!r.textContent.toLowerCase().includes(q.value.toLowerCase()));if(!r.hidden)n++;}document.getElementById('count').textContent=n+' 项';}q.addEventListener('input',filter);fc.addEventListener('change',filter);filter();</script></html>'''
    page = page.replace("SUMMARY", html.escape(summary)).replace("NOTES", "".join(f"<li>{html.escape(n)}</li>" for n in NOTES)).replace("ROWS", "".join(rows_html))
    (args.output / "index.html").write_text(page, encoding="utf-8")
    reopened = json.loads((args.output / "points.json").read_text(encoding="utf-8"))
    assert reopened == data
    print(json.dumps(dict(output=str(args.output), pointCount=len(points), counts=counts, validation="passed"), ensure_ascii=False))


if __name__ == "__main__":
    main()
