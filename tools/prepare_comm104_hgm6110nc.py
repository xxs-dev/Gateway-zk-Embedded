"""Prepare a disabled, offline HGM6110NC acquisition candidate. No network I/O."""

import argparse
import copy
import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "config/templates/smartgen_hgm6110can_v1_7/points.json"
METER = "COMM202600104_GENERATOR"
INDEX_BASE = 160000
N_COILS = {
    54: "漏液停机", 62: "过流故障停机输入报警", 63: "超速停机输入报警",
    70: "高温报警停机（模拟量温度传感器）",
    71: "低油压报警停机（模拟量油压传感器）", 79: "漏液警告",
}


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def build():
    catalog = load(CATALOG)
    source = {(p["functionCode"], p["address"]): p for p in catalog["points"]}
    selected = []
    for address in range(80):
        if address in (5, 55):
            continue
        if address in N_COILS:
            p = copy.deepcopy(source[(1, 0)])
            p.update(address=address, name=N_COILS[address], sourcePages=[7 if address < 72 else 8])
            p["valueMap"] = {"0": "未发生", "1": N_COILS[address]}
        else:
            p = copy.deepcopy(source[(1, address)])
        selected.append(p)
    # N-series excludes the CAN-only ECU blocks, including FC03 32/33 and 56..69.
    registers = list(range(32)) + list(range(34, 56)) + list(range(71, 82)) + list(range(92, 98))
    registers += list(range(2500, 2512, 2))
    selected += [copy.deepcopy(source[(3, address)]) for address in registers]
    for address in range(98, 111):
        name = {98: "油位变化百分数（历史记录显示）", 99: "油位变化前起始值",
                100: "油位变化大于5%标志"}.get(address, f"油位循环记录{address - 100}")
        selected.append(dict(functionCode=3, address=address, name=name,
                             dataType="int16" if address == 98 else "uint16", length=1,
                             scale=1, unit="%" if address != 100 else "",
                             category="status" if address == 100 else "telemetry", sourcePages=[12]))
    points = [dict(index=INDEX_BASE, pointCode="gen_online", name="发电机通讯状态",
                   enabled=True, address=0, category="status", isStore=False,
                   read=dict(enable=True, function=0, length=1, dataType="device_online",
                             scale=1, offset=0, intervalMs=1000),
                   write=dict(enable=False), valueMap={"0": "离线", "1": "在线"})]
    metadata = []
    for p in selected:
        fc, address = p["functionCode"], p["address"]
        code = f"gen_fc{fc:02d}_{address:04d}"
        interval = 5000 if fc == 3 and (30 <= address <= 31 or 42 <= address <= 51 or 71 <= address <= 110) else 1000
        point = dict(index=INDEX_BASE + len(points), pointCode=code, name=p["name"],
                     address=address, enabled=True, category=p.get("category", "telemetry"),
                     isStore=False, fullUpload=False, reportOnChange=False,
                     tags=["generator", "hgm6110nc", "offline_candidate"],
                     read=dict(enable=True, function=fc, length=p["length"],
                               dataType=p["dataType"], scale=p["scale"], offset=0,
                               byteOrder=p.get("byteOrder", "AB"),
                               signed=p["dataType"].startswith("int"), unit=p["unit"], intervalMs=interval),
                     write=dict(enable=False), alarms=[])
        if "valueMap" in p:
            point["valueMap"] = p["valueMap"]
        pending = []
        if fc == 3:
            pending.append("raw_32766_quality_filter_required")
        if fc == 3 and address in (6, 13):
            point["name"] += "（原始码，倍率待核）"
            point["read"].update(scale=1, unit="raw")
            pending.append("frequency_table_0.1_vs_example_0.01")
        if fc == 3 and address >= 2500:
            pending.append("verify_32bit_order_and_sentinel_granularity")
        if ((fc == 3 and (address <= 6 or 38 <= address <= 41 or 95 <= address <= 97))
                or (fc == 1 and address in (6, 64, 65, 66, 67, 68, 69))):
            point["enabled"] = False
            pending.append("6110_mains_ats_support_not_verified")
        points.append(point)
        metadata.append(dict(pointCode=code, functionCode=fc, address=address,
                             sourcePages=p["sourcePages"], pending=pending))
    config = dict(templateName="comm104_hgm6110nc_rs485_6_readonly_candidate",
                  schemaVersion="1.1.0", enabled=False,
                  protocol=dict(type="modbus_rtu", slave=1,
                                transport=dict(serialPort="/dev/ttySP6", baudRate=9600, dataBits=8,
                                               stopBits=1, parity="N", timeoutMs=1000,
                                               frameIntervalMs=100, readRetryCount=0)),
                  collect=dict(defaultIntervalMs=1000, batchOptimize=True,
                               maxBatchRegisters=32, maxRequestRegisters=32,
                               offlineFailureThreshold=3, recoverySuccessThreshold=2,
                               slaveFailureBackoffThreshold=3, slaveFailureBackoffMs=30000,
                               taskFailureBackoffThreshold=1, taskFailureBackoffMs=30000,
                               offlineProbeOnly=True, offlineProbeTaskCount=1,
                               adaptiveSplitOnFailure=False, failureGoodValueGraceMs=0,
                               failureBadQualityThreshold=1),
                  memoryStore=dict(enabled=True, backend="memory", keepHistory=10,
                                   defaultTtlMs=15000, sharedMemoryName="gateway_point_store_ttySP6",
                                   maxLatestPoints=1024, maxPendingWrites=16, maxPersistentSamples=1024),
                  meters=[dict(meterCode=METER, deviceName="柴油发电机 HGM6110NC", enabled=False,
                               model="HGM6110NC", slave=1, protocolType="modbus_rtu",
                               sourceCom="COM6", sourceAddress=1, points=points)],
                  northboundServer=dict(enabled=False, writesEnabled=False))
    plan = dict(status="offline-only-not-deployment-ready", machineCode="COMM202600104",
                physicalPort="RS-485 port 6", systemPortCandidate="/dev/ttySP6",
                portMappingVerified=False, serialFormatVerified=False,
                source=catalog["source"], modelBasis="User specified HGM6110NC; N-family mapping, verify nameplate/firmware on site",
                catalogSha256=sha(CATALOG), points=metadata,
                requiredBeforeEnable=["identity_and_port_owner_check", "live_index_and_shm_capacity_check",
                                      "serial_8N1_confirmation", "raw_32766_quality_filter_required",
                                      "firmware_and_register_support_readonly_check"],
                derivedNotInstalled=[
                    dict(name="累计运行小时", expression="R42*10000+R43", unit="h"),
                    dict(name="累计运行时间", expression="(R42*10000+R43)*3600+R44*60+R45", unit="s"),
                    dict(name="累计开机次数", expression="R46*10000+R47", unit="次"),
                    dict(name="累计电能", expression="R48*10000+R49", unit="kWh"),
                    dict(name="维护剩余时间", expression="R30*60+R31", unit="min")],
                displayBindingsNotInstalled={
                    "deviceOnline": "gen_online", "runState": "gen_fc03_0034",
                    "modeBits": [f"gen_fc01_{a:04d}" for a in (40, 41, 42, 43)],
                    "phaseVoltage": [f"gen_fc03_{a:04d}" for a in (7, 8, 9)],
                    "phaseCurrentPrimary": [f"gen_fc03_{a:04d}" for a in (14, 15, 16)],
                    "phaseActivePower": [f"gen_fc03_{a:04d}" for a in (52, 53, 54)],
                    "totalP_Q_S_PF": [f"gen_fc03_{a:04d}" for a in (26, 27, 28, 29)],
                    "frequencyRawPending": "gen_fc03_0013", "speedRpm": "gen_fc03_0023",
                    "batteryVoltage": "gen_fc03_0024", "dPlusVoltage": "gen_fc03_0025",
                    "waterTemperature": "gen_fc03_0017", "oilPressureKPa": "gen_fc03_0019",
                    "fuelLevelPercent": "gen_fc03_0021", "emergencyInput": "gen_fc01_0048",
                    "commonAlarm": "gen_fc01_0000"},
                displayRules=["Read quality/freshness before valueMap; unknown codes remain unknown.",
                              "Run-state code 9 means normal running; never use all nonzero states as running.",
                              "Remote mode FC01/3 is generator mode, NOT the EMS platform-control permission.",
                              "Rename oil-temperature slot to water temperature; N-family has no ECU oil temperature.",
                              "Rename remaining fuel (L) to fuel level (%); tank capacity is unknown.",
                              "D+ voltage is not a proven charger output measurement.",
                              "Do not fabricate per-phase Q/S/PF or accumulated fuel from total values.",
                              "Do not sum 16-bit and 32-bit representations of the same quantity.",
                              "No control buttons or EMS dispatch bindings in this read-only phase."])
    return config, plan


def check_baseline(config, directory):
    candidate = config["meters"][0]["points"]
    indexes = {p["index"] for p in candidate}
    codes = {p["pointCode"] for p in candidate}
    files = list(Path(directory).glob("*.json"))
    if not files:
        raise ValueError("No local baseline device files")
    hashes = {}
    for path in files:
        device = load(path)
        points = list(device.get("points", []))
        for meter in device.get("meters", []):
            if meter.get("meterCode") == METER:
                raise ValueError(f"Generator already exists: {path.name}")
            points.extend(meter.get("points", []))
        if indexes.intersection(p.get("index") for p in points) or codes.intersection(p.get("pointCode") for p in points):
            raise ValueError(f"Point collision: {path.name}")
        if device.get("protocol", {}).get("transport", {}).get("serialPort") == "/dev/ttySP6":
            raise ValueError(f"Port already configured: {path.name}")
        if device.get("memoryStore", {}).get("sharedMemoryName") == "gateway_point_store_ttySP6":
            raise ValueError(f"Shared-memory name already configured: {path.name}")
        hashes[path.name] = sha(path)
    return dict(status="local-snapshot-only-not-live", files=hashes)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--baseline-devices", type=Path, required=True)
    parser.add_argument("--protocol-pdf", type=Path, required=True)
    args = parser.parse_args()
    config, plan = build()
    if sha(args.protocol_pdf) != plan["source"]["sha256"]:
        raise ValueError("Protocol source hash mismatch")
    plan["baselineCheck"] = check_baseline(config, args.baseline_devices)
    args.output.mkdir(parents=True, exist_ok=False)
    for name, data in [("device_modbusRTU_6.json", config), ("integration-plan.json", plan)]:
        (args.output / name).write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    manifest = {p.name: sha(p) for p in args.output.iterdir() if p.is_file()}
    (args.output / "sha256.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(dict(output=str(args.output), points=len(config["meters"][0]["points"]),
                         enabled=config["enabled"], files=manifest)))


if __name__ == "__main__":
    main()
