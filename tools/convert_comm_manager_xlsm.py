#!/usr/bin/env python3
"""Convert legacy communication-manager xlsm files to Gateway runtime JSON."""

from __future__ import annotations

import argparse
import copy
import json
import re
import sys
import zipfile
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


NS = {
    "a": "http://schemas.openxmlformats.org/spreadsheetml/2006/main",
    "r": "http://schemas.openxmlformats.org/officeDocument/2006/relationships",
}


@dataclass(frozen=True)
class SerialConfig:
    sheet: str
    serial_port: str
    baud_rate: int
    data_bits: int
    stop_bits: int
    parity: str
    timeout_ms: int
    default_interval_ms: int
    block_interval_ms: int


@dataclass(frozen=True)
class ModbusRow:
    sheet: str
    row: int
    device_name: str
    slave: int
    address: int
    rw_type: str
    function: int
    dtype: str
    scale: float
    source_index: int
    unit: str
    name: str


@dataclass(frozen=True)
class VarRow:
    row: int
    mqtt_device_name: str
    meter_code: str
    point_name: str
    point_code: str
    rw_type: str
    app_index: int
    source_index: int
    start_bit: int
    data_len: int
    dtype: str
    unit: str


class XlsmReader:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.archive = zipfile.ZipFile(path)
        self.shared_strings = self._load_shared_strings()
        self.sheet_paths = self._load_sheet_paths()

    def close(self) -> None:
        self.archive.close()

    def _load_shared_strings(self) -> List[str]:
        if "xl/sharedStrings.xml" not in self.archive.namelist():
            return []
        root = ET.fromstring(self.archive.read("xl/sharedStrings.xml"))
        result: List[str] = []
        for item in root.findall("a:si", NS):
            result.append("".join((text.text or "") for text in item.findall(".//a:t", NS)))
        return result

    def _load_sheet_paths(self) -> Dict[str, str]:
        workbook = ET.fromstring(self.archive.read("xl/workbook.xml"))
        rels = ET.fromstring(self.archive.read("xl/_rels/workbook.xml.rels"))
        rel_map = {rel.attrib["Id"]: rel.attrib["Target"] for rel in rels}
        result: Dict[str, str] = {}
        for sheet in workbook.findall("a:sheets/a:sheet", NS):
            rel_id = sheet.attrib["{http://schemas.openxmlformats.org/officeDocument/2006/relationships}id"]
            target = rel_map[rel_id]
            if not target.startswith("worksheets/"):
                target = "worksheets/" + target
            result[sheet.attrib["name"]] = "xl/" + target
        return result

    @staticmethod
    def _cell_ref_to_col(ref: str) -> int:
        match = re.match(r"([A-Z]+)\d+", ref)
        if not match:
            raise ValueError(f"invalid cell reference: {ref}")
        col = 0
        for char in match.group(1):
            col = col * 26 + ord(char) - ord("A") + 1
        return col

    def _cell_value(self, cell: ET.Element) -> str:
        cell_type = cell.attrib.get("t", "")
        if cell_type == "inlineStr":
            return "".join((text.text or "") for text in cell.findall(".//a:t", NS)).strip()
        value = cell.find("a:v", NS)
        if value is None:
            return ""
        raw = value.text or ""
        if cell_type == "s":
            return self.shared_strings[int(raw)].strip() if raw else ""
        return raw.strip()

    def rows(self, sheet_name: str) -> Iterable[Tuple[int, Dict[int, str]]]:
        if sheet_name not in self.sheet_paths:
            raise ValueError(f"sheet not found: {sheet_name}")
        root = ET.fromstring(self.archive.read(self.sheet_paths[sheet_name]))
        for row in root.findall("a:sheetData/a:row", NS):
            row_no = int(row.attrib["r"])
            values: Dict[int, str] = {}
            for cell in row.findall("a:c", NS):
                values[self._cell_ref_to_col(cell.attrib["r"])] = self._cell_value(cell)
            if values:
                yield row_no, values


def parse_args(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Convert communication-manager xlsm to Gateway runtime device JSON.")
    parser.add_argument("--input", required=True, help="legacy .xlsm file")
    parser.add_argument("--output-dir", default="tmp/converted_comm_manager/runtime/devices", help="device json output directory")
    parser.add_argument("--machine-code", default="", help="optional machineCode written to device JSON; empty means use identity file")
    parser.add_argument("--shared-memory-name", default="gateway_point_store")
    parser.add_argument("--serial-prefix", default="/dev/ttySP", help="COM1 -> /dev/ttySP1 by default")
    parser.add_argument("--sqlite-dir", default="/opt/modbus-gateway/data")
    parser.add_argument("--app-config", action="append", default=[], help="app json to update deviceConfigFiles, repeatable")
    parser.add_argument("--runtime-device-prefix", default="/opt/modbus-gateway/config/runtime/devices")
    parser.add_argument("--update-apps", action="store_true", help="update --app-config files with generated deviceConfigFiles")
    parser.add_argument("--pretty", action="store_true", help="pretty print generated json")
    return parser.parse_args(argv)


def clean_text(value: object, default: str = "") -> str:
    if value is None:
        return default
    text = str(value).strip()
    return text if text else default


def parse_int(value: object, default: int = 0) -> int:
    text = clean_text(value)
    if not text:
        return default
    try:
        return int(float(text))
    except ValueError:
        return default


def parse_float(value: object, default: float = 1.0) -> float:
    text = clean_text(value)
    if not text:
        return default
    try:
        return float(text)
    except ValueError:
        return default


def normalize_unit(value: str) -> str:
    unit = clean_text(value)
    return "" if unit == "-" else unit


def normalize_parity(value: str) -> str:
    parity = clean_text(value, "N").upper()
    if parity in {"NONE", "NO"}:
        return "N"
    if parity.startswith("E"):
        return "E"
    if parity.startswith("O"):
        return "O"
    return parity[:1] if parity else "N"


def serial_port_from_com(com_name: str, serial_prefix: str) -> str:
    match = re.search(r"(\d+)$", clean_text(com_name))
    if not match:
        return com_name
    return f"{serial_prefix}{int(match.group(1))}"


def map_modbus_dtype(dtype: str) -> Tuple[str, int]:
    normalized = clean_text(dtype).upper()
    if normalized in {"BOOL", "BIT"}:
        return "uint16", 1
    if normalized == "INT16":
        return "int16", 1
    if normalized == "UINT16":
        return "uint16", 1
    if normalized == "INT32":
        return "int32", 2
    if normalized == "UINT32":
        return "uint32", 2
    if normalized in {"FLOAT", "FLOAT32"}:
        return "float32", 2
    return "uint16", 1


def build_cache_policy(ttl_ms: int = 600000) -> Dict[str, object]:
    return {
        "storeLatest": True,
        "storeHistory": True,
        "historySize": 100,
        "ttlMs": ttl_ms,
    }


def byte_order_for_length(length: int) -> str:
    return "ABCD" if length >= 2 else "AB"


def build_read_spec(
    source: ModbusRow,
    var: VarRow,
    *,
    data_type: str,
    length: int,
    scale: float,
    unit: str,
    interval_ms: int,
    bit: Optional[int] = None,
) -> Dict[str, object]:
    spec: Dict[str, object] = {
        "enable": True,
        "function": source.function if source.function > 0 else 3,
        "length": length,
        "dataType": data_type,
        "scale": scale,
        "offset": 0,
        "byteOrder": byte_order_for_length(length),
        "signed": data_type.startswith("int"),
        "unit": unit,
        "intervalMs": interval_ms,
        "cachePolicy": build_cache_policy(),
    }
    if bit is not None:
        spec["bit"] = bit
    return spec


def build_write_spec(
    source: ModbusRow,
    *,
    data_type: str,
    length: int,
    scale: float,
    verify_after_write: bool,
) -> Dict[str, object]:
    function = source.function if source.function > 0 else 6
    write_type = data_type
    if function == 5:
        write_type = "bool"
        length = 1
    spec: Dict[str, object] = {
        "enable": True,
        "address": source.address,
        "function": function,
        "length": length,
        "dataType": write_type,
        "scale": scale,
        "offset": 0,
        "byteOrder": byte_order_for_length(length),
        "verifyAfterWrite": verify_after_write,
        "verifyDelayMs": 200,
        "verifyByRead": verify_after_write,
    }
    if write_type in {"uint16", "bool", "bit"}:
        spec["min"] = 0
        spec["max"] = 1 if write_type in {"bool", "bit"} or function == 5 else 65535
        spec["step"] = 1
    return spec


def disabled_write_spec() -> Dict[str, object]:
    return {"enable": False}


def is_online_row(source: ModbusRow, var: VarRow) -> bool:
    return source.rw_type == "COM_ST" or var.rw_type == "COM_ST" or source.name == "在线状态" or var.point_name == "在线状态"


def is_split_bit(var: VarRow, source: ModbusRow, source_ref_count: int) -> bool:
    if is_online_row(source, var):
        return False
    if var.dtype.upper() != "BOOL":
        return False
    if var.data_len == 1:
        return True
    return source_ref_count > 1 and var.start_bit > 0


def make_device_online_point(source: ModbusRow, var: VarRow, interval_ms: int) -> Dict[str, object]:
    return {
        "index": var.app_index,
        "pointCode": var.point_code or "device_online",
        "name": var.point_name or "在线状态",
        "desc": f"{source.device_name} online status",
        "category": "status",
        "address": 0,
        "enabled": True,
        "isStore": False,
        "fullUpload": True,
        "reportOnChange": True,
        "persistIntervalSec": 60,
        "tags": ["status", "online", "change"],
        "read": {
            "enable": True,
            "function": 0,
            "length": 0,
            "dataType": "device_online",
            "scale": 1,
            "offset": 0,
            "byteOrder": "AB",
            "signed": False,
            "unit": "",
            "intervalMs": interval_ms,
            "cachePolicy": build_cache_policy(),
        },
        "write": disabled_write_spec(),
        "alarms": [],
        "valueMap": None,
    }


def point_key(sheet: str, slave: int, meter_code: str, var: VarRow, source: ModbusRow) -> Tuple[object, ...]:
    # Same business app index should become one read-write point. Split bits keep startBit in the key.
    split = var.dtype.upper() == "BOOL" and var.data_len == 1 and var.start_bit > 0
    return (
        sheet,
        slave,
        meter_code,
        var.app_index,
        var.point_code,
        var.start_bit if split else None,
        source.address if split else None,
    )


def merge_point(
    existing: Dict[str, object],
    source: ModbusRow,
    var: VarRow,
    source_ref_count: int,
    interval_ms: int,
) -> None:
    read = existing.get("read")
    write = existing.get("write")
    if var.rw_type == "R" and isinstance(read, dict) and not read.get("enable", False):
        data_type, length = map_modbus_dtype(source.dtype)
        unit = normalize_unit(var.unit or source.unit)
        if is_split_bit(var, source, source_ref_count):
            read.update(build_read_spec(
                source,
                var,
                data_type="bit",
                length=1,
                scale=1.0,
                unit=unit,
                interval_ms=interval_ms,
                bit=var.start_bit,
            ))
        else:
            if var.dtype.upper() == "BOOL":
                data_type = "uint16"
                length = 1
            read.update(build_read_spec(
                source,
                var,
                data_type=data_type,
                length=length,
                scale=source.scale,
                unit=unit,
                interval_ms=interval_ms,
            ))
    if var.rw_type == "W" and isinstance(write, dict) and not write.get("enable", False):
        data_type, length = map_modbus_dtype(source.dtype)
        read = existing.get("read")
        can_verify_by_read = isinstance(read, dict) and bool(read.get("enable", False))
        write.update(build_write_spec(
            source,
            data_type=data_type,
            length=length,
            scale=source.scale,
            verify_after_write=can_verify_by_read,
        ))
        tags = existing.setdefault("tags", [])
        if isinstance(tags, list) and "writable" not in tags:
            tags.append("writable")


def make_point(
    source: ModbusRow,
    var: VarRow,
    source_ref_count: int,
    interval_ms: int,
) -> Dict[str, object]:
    if is_online_row(source, var):
        return make_device_online_point(source, var, interval_ms)

    split_bit = is_split_bit(var, source, source_ref_count)
    unit = normalize_unit(var.unit or source.unit)
    if split_bit:
        read_data_type, read_length, read_scale, bit = "bit", 1, 1.0, var.start_bit
        category = "status"
        tags = ["bit", "status"]
        report_on_change = True
    else:
        read_data_type, read_length = map_modbus_dtype(source.dtype)
        if var.dtype.upper() == "BOOL":
            read_data_type, read_length = "uint16", 1
        read_scale, bit = source.scale, None
        category = "telemetry"
        tags = []
        report_on_change = False

    point: Dict[str, object] = {
        "index": var.app_index,
        "pointCode": var.point_code,
        "name": var.point_name or source.name,
        "desc": f"{source.device_name} {var.point_name or source.name}",
        "category": category,
        "address": source.address,
        "enabled": True,
        "isStore": False,
        "fullUpload": True,
        "reportOnChange": report_on_change,
        "persistIntervalSec": 60,
        "tags": tags,
        "read": {"enable": False},
        "write": disabled_write_spec(),
        "alarms": [],
        "valueMap": None,
    }
    if var.rw_type in {"R", "COM_ST"}:
        point["read"] = build_read_spec(
            source,
            var,
            data_type=read_data_type,
            length=read_length,
            scale=read_scale,
            unit=unit,
            interval_ms=interval_ms,
            bit=bit,
        )
    if var.rw_type == "W":
        write_data_type, write_length = map_modbus_dtype(source.dtype)
        point["write"] = build_write_spec(
            source,
            data_type=write_data_type,
            length=write_length,
            scale=source.scale,
            verify_after_write=False,
        )
        if "writable" not in tags:
            tags.append("writable")
    return point


def read_serial_configs(reader: XlsmReader, serial_prefix: str) -> Dict[str, SerialConfig]:
    result: Dict[str, SerialConfig] = {}
    for row_no, row in reader.rows("串口驱动配置"):
        if row_no < 4:
            continue
        sheet = clean_text(row.get(2))
        if not sheet:
            continue
        com_name = clean_text(row.get(3))
        result[sheet] = SerialConfig(
            sheet=sheet,
            serial_port=serial_port_from_com(com_name, serial_prefix),
            baud_rate=parse_int(row.get(4), 9600),
            data_bits=parse_int(row.get(5), 8),
            stop_bits=parse_int(row.get(6), 1),
            parity=normalize_parity(row.get(7, "N")),
            timeout_ms=parse_int(row.get(8), 1000),
            default_interval_ms=parse_int(row.get(10), 500),
            block_interval_ms=parse_int(row.get(11), 100),
        )
    return result


def read_modbus_rows(reader: XlsmReader) -> Dict[int, ModbusRow]:
    result: Dict[int, ModbusRow] = {}
    for sheet_index in range(1, 9):
        sheet = f"modbusRTU_{sheet_index}"
        if sheet not in reader.sheet_paths:
            continue
        for row_no, row in reader.rows(sheet):
            if row_no < 4 or not clean_text(row.get(10)):
                continue
            source_index = parse_int(row.get(10), 0)
            if source_index <= 0:
                continue
            function = parse_int(row.get(7), 0)
            rw_type = clean_text(row.get(6)).upper()
            if rw_type == "COM_ST" and function <= 0:
                function = 3
            result[source_index] = ModbusRow(
                sheet=sheet,
                row=row_no,
                device_name=clean_text(row.get(2), f"{sheet}_slave_{parse_int(row.get(4), 1)}"),
                slave=parse_int(row.get(4), 1),
                address=parse_int(row.get(5), 0),
                rw_type=rw_type,
                function=function,
                dtype=clean_text(row.get(8), "UINT16").upper(),
                scale=parse_float(row.get(9), 1.0),
                source_index=source_index,
                unit=clean_text(row.get(11)),
                name=clean_text(row.get(12), f"point_{source_index}"),
            )
    return result


def read_var_rows(reader: XlsmReader) -> List[VarRow]:
    result: List[VarRow] = []
    for row_no, row in reader.rows("varlist"):
        if row_no < 4 or not clean_text(row.get(7)):
            continue
        app_index = parse_int(row.get(7), 0)
        source_index = parse_int(row.get(8), 0)
        if app_index <= 0 or source_index <= 0:
            continue
        result.append(VarRow(
            row=row_no,
            mqtt_device_name=clean_text(row.get(2)),
            meter_code=clean_text(row.get(3)),
            point_name=clean_text(row.get(4)),
            point_code=clean_text(row.get(5), f"point_{app_index}"),
            rw_type=clean_text(row.get(6), "R").upper(),
            app_index=app_index,
            source_index=source_index,
            start_bit=parse_int(row.get(9), 0),
            data_len=parse_int(row.get(10), 64),
            dtype=clean_text(row.get(11), "FLOAT").upper(),
            unit=clean_text(row.get(12)),
        ))
    return result


def validate_unique_point_bindings(device: Dict[str, object]) -> None:
    seen_index: Dict[int, str] = {}
    seen_key: Dict[Tuple[str, str], int] = {}
    for meter in device.get("meters", []):
        if not isinstance(meter, dict):
            continue
        meter_code = clean_text(meter.get("meterCode"))
        for point in meter.get("points", []):
            if not isinstance(point, dict):
                continue
            index = parse_int(point.get("index"), 0)
            point_code = clean_text(point.get("pointCode"))
            if index in seen_index:
                raise ValueError(f"duplicate point.index {index}: {point_code} conflicts with {seen_index[index]}")
            seen_index[index] = point_code
            key = (meter_code, point_code)
            if key in seen_key:
                raise ValueError(f"duplicate point binding meterCode={meter_code} pointCode={point_code}")
            seen_key[key] = index


def build_devices(
    serial_configs: Dict[str, SerialConfig],
    modbus_by_index: Dict[int, ModbusRow],
    var_rows: List[VarRow],
    args: argparse.Namespace,
) -> Tuple[List[Tuple[Path, Dict[str, object]]], Dict[str, object]]:
    source_ref_count = Counter(var.source_index for var in var_rows)
    grouped: Dict[str, Dict[Tuple[str, int, str], Dict[str, object]]] = defaultdict(dict)
    point_lookup: Dict[Tuple[object, ...], Dict[str, object]] = {}
    skipped_missing_source: List[int] = []
    adjusted_meter_codes: Dict[Tuple[str, int, str], str] = {}

    # Runtime binding key is machineCode + meterCode + pointCode. A few legacy rows reuse the
    # same MQTT meterCode for multiple physical slaves with identical pointCode sets, so only
    # those conflicting physical groups get a deterministic suffix.
    meter_point_groups: Dict[Tuple[str, str], set[Tuple[str, int, str]]] = defaultdict(set)
    for var in var_rows:
        source = modbus_by_index.get(var.source_index)
        if source is None:
            continue
        meter_point_groups[(var.meter_code, var.point_code)].add((source.sheet, source.slave, var.meter_code))
    conflict_groups: set[Tuple[str, int, str]] = set()
    for groups in meter_point_groups.values():
        if len(groups) > 1:
            conflict_groups.update(groups)
    conflict_order: Dict[str, List[Tuple[str, int, str]]] = defaultdict(list)
    for group in sorted(conflict_groups, key=lambda item: (item[2], item[0], item[1])):
        conflict_order[group[2]].append(group)
    for meter_code, groups in conflict_order.items():
        for offset, group in enumerate(groups):
            if offset == 0:
                adjusted_meter_codes[group] = meter_code
                continue
            sheet_no = int(group[0].rsplit("_", 1)[1])
            adjusted_meter_codes[group] = f"{meter_code}_ttySP{sheet_no}_s{group[1]}"

    for var in var_rows:
        source = modbus_by_index.get(var.source_index)
        if source is None:
            skipped_missing_source.append(var.row)
            continue
        original_group_key = (source.sheet, source.slave, var.meter_code)
        runtime_meter_code = adjusted_meter_codes.get(original_group_key, var.meter_code)
        group_key = (source.sheet, source.slave, runtime_meter_code)
        meter = grouped[source.sheet].setdefault(group_key, {
            "meterCode": runtime_meter_code or f"{source.sheet}_SLAVE{source.slave:04d}",
            "deviceName": var.mqtt_device_name or source.device_name,
            "slave": source.slave,
            "points": [],
        })
        key = point_key(source.sheet, source.slave, runtime_meter_code, var, source)
        existing = point_lookup.get(key)
        if existing is None:
            point = make_point(source, var, source_ref_count[var.source_index], serial_configs[source.sheet].default_interval_ms)
            meter["points"].append(point)
            point_lookup[key] = point
        else:
            merge_point(existing, source, var, source_ref_count[var.source_index], serial_configs[source.sheet].default_interval_ms)

    output_dir = Path(args.output_dir)
    generated: List[Tuple[Path, Dict[str, object]]] = []
    for sheet in sorted(grouped.keys(), key=lambda name: int(name.rsplit("_", 1)[1])):
        serial = serial_configs.get(sheet)
        if serial is None:
            raise ValueError(f"missing serial config for {sheet}")
        sheet_no = int(sheet.rsplit("_", 1)[1])
        meters = list(grouped[sheet].values())
        meters.sort(key=lambda item: (parse_int(item.get("slave"), 0), clean_text(item.get("meterCode"))))
        for meter in meters:
            meter["points"].sort(key=lambda point: parse_int(point.get("index"), 0))
        device = {
            "schemaVersion": "1.0.0",
            "machineCode": args.machine_code,
            "protocol": {
                "type": "modbus_rtu",
                "slave": 1,
                "transport": {
                    "serialPort": serial.serial_port,
                    "baudRate": serial.baud_rate,
                    "dataBits": serial.data_bits,
                    "stopBits": serial.stop_bits,
                    "parity": serial.parity,
                    "timeoutMs": serial.timeout_ms,
                },
            },
            "collect": {
                "defaultIntervalMs": serial.default_interval_ms,
                "batchOptimize": True,
                "maxBatchRegisters": 120,
            },
            "memoryStore": {
                "enabled": True,
                "backend": "memory",
                "keepHistory": 100,
                "defaultTtlMs": 600000,
                "indexBy": ["machineCode", "meterCode", "pointCode"],
                "sharedMemoryName": args.shared_memory_name,
                "maxLatestPoints": 100000,
                "maxPendingWrites": 4096,
                "maxPersistentSamples": 20000,
                "sqlitePath": f"{args.sqlite_dir.rstrip('/')}/point_samples_ttySP{sheet_no}.db",
                "sqliteLibraryPath": "",
                "persistFlushIntervalMs": 60000,
                "writebackIntervalMs": 500,
                "writebackBatchSize": 100,
            },
            "meters": meters,
        }
        validate_unique_point_bindings(device)
        generated.append((output_dir / f"device_slave_ttySP{sheet_no}.json", device))

    summary = {
        "input": str(Path(args.input)),
        "deviceFileCount": len(generated),
        "meterCount": sum(len(device["meters"]) for _, device in generated),
        "pointCount": sum(len(meter["points"]) for _, device in generated for meter in device["meters"]),
        "varRows": len(var_rows),
        "modbusRows": len(modbus_by_index),
        "missingSourceRows": skipped_missing_source,
        "adjustedMeterCodes": [
            {
                "sourceSheet": group[0],
                "slave": group[1],
                "originalMeterCode": group[2],
                "runtimeMeterCode": adjusted,
            }
            for group, adjusted in sorted(adjusted_meter_codes.items(), key=lambda item: (item[0][2], item[0][0], item[0][1]))
            if adjusted != group[2]
        ],
        "bitSplitPointCount": sum(
            1
            for _, device in generated
            for meter in device["meters"]
            for point in meter["points"]
            if isinstance(point.get("read"), dict) and point["read"].get("dataType") == "bit"
        ),
        "readWritePointCount": sum(
            1
            for _, device in generated
            for meter in device["meters"]
            for point in meter["points"]
            if isinstance(point.get("read"), dict)
            and point["read"].get("enable")
            and isinstance(point.get("write"), dict)
            and point["write"].get("enable")
        ),
    }
    return generated, summary


def write_json(path: Path, data: Dict[str, object], pretty: bool) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(data, ensure_ascii=False, indent=2 if pretty else None)
    if not text.endswith("\n"):
        text += "\n"
    path.write_text(text, encoding="utf-8")


def update_app_config(path: Path, generated_files: List[Path], runtime_device_prefix: str, pretty: bool) -> None:
    app = json.loads(path.read_text(encoding="utf-8"))
    app["deviceConfigFiles"] = [f"{runtime_device_prefix.rstrip('/')}/{file.name}" for file in generated_files]
    mqtt_driver = app.get("mqttDriver")
    if isinstance(mqtt_driver, dict):
        mqtt_driver["sharedMemoryName"] = "gateway_point_store"
        mqtt_driver["sharedMemoryNames"] = ["gateway_point_store"]
    write_json(path, app, pretty)


def main(argv: List[str]) -> int:
    args = parse_args(argv)
    reader = XlsmReader(Path(args.input))
    try:
        serial_configs = read_serial_configs(reader, args.serial_prefix)
        modbus_by_index = read_modbus_rows(reader)
        var_rows = read_var_rows(reader)
    finally:
        reader.close()

    generated, summary = build_devices(serial_configs, modbus_by_index, var_rows, args)
    for path, device in generated:
        write_json(path, device, args.pretty)

    manifest = {
        **summary,
        "deviceConfigFiles": [str(path) for path, _ in generated],
    }
    write_json(Path(args.output_dir) / "conversion_manifest.json", manifest, True)

    if args.update_apps:
        for app_config in args.app_config:
            update_app_config(Path(app_config), [path for path, _ in generated], args.runtime_device_prefix, args.pretty)

    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
