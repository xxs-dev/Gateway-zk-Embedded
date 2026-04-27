#!/usr/bin/env python3
"""Build gateway split inventory from existing device config files."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


def parse_args(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build point/segment inventory from device config files.")
    parser.add_argument("--device-config", action="append", required=True, help="device json path, repeatable")
    parser.add_argument("--output", help="write inventory json path")
    parser.add_argument("--pretty", action="store_true", help="pretty print output")
    parser.add_argument("--machine-code-prefix", default="GW", help="planner machineCode prefix")
    parser.add_argument("--rs485-ports-per-gateway", type=int, default=8, help="planner RS485 ports per gateway")
    parser.add_argument("--max-points-per-gateway", type=int, default=12000)
    parser.add_argument("--max-segments-per-gateway", type=int, default=320)
    parser.add_argument("--max-devices-per-gateway", type=int, default=200)
    parser.add_argument("--max-points-per-port", type=int, default=2000)
    parser.add_argument("--max-segments-per-port", type=int, default=64)
    parser.add_argument("--max-devices-per-port", type=int, default=32)
    return parser.parse_args(argv)


def load_json(path: Path) -> Dict[str, object]:
    return json.loads(path.read_text(encoding="utf-8"))


def normalize_interval(point: Dict[str, object], collect_default: int) -> int:
    read = point.get("read", {})
    if isinstance(read, dict):
        value = read.get("intervalMs")
        if isinstance(value, int) and value > 0:
            return value
    return collect_default


def normalize_length(point: Dict[str, object]) -> int:
    read = point.get("read", {})
    if isinstance(read, dict):
        value = read.get("length")
        if isinstance(value, int) and value > 0:
            return value
    return 1


def count_modbus_segments(points: Iterable[Dict[str, object]], collect_default: int) -> int:
    groups: List[Tuple[int, int, int, int]] = []
    for point in points:
        if not point.get("enabled", True):
            continue
        read = point.get("read", {})
        if not isinstance(read, dict) or not read.get("enable", True):
            continue
        function = read.get("function")
        address = point.get("address", read.get("address"))
        if not isinstance(function, int) or not isinstance(address, int):
            continue
        if function <= 0:
            # device_online etc. do not create a Modbus bus transaction
            continue
        interval = normalize_interval(point, collect_default)
        length = normalize_length(point)
        groups.append((function, interval, address, max(1, length)))
    if not groups:
        return 0
    groups.sort(key=lambda item: (item[0], item[1], item[2]))
    segments = 0
    current: Optional[Tuple[int, int, int]] = None
    for function, interval, address, length in groups:
        end = address + length - 1
        if current is None:
            current = (function, interval, end)
            segments += 1
            continue
        current_function, current_interval, current_end = current
        if function == current_function and interval == current_interval and address <= current_end + 1:
            current = (current_function, current_interval, max(current_end, end))
        else:
            segments += 1
            current = (function, interval, end)
    return segments


def count_dlt645_segments(points: Iterable[Dict[str, object]]) -> int:
    segments = 0
    for point in points:
        if not point.get("enabled", True):
            continue
        read = point.get("read", {})
        if not isinstance(read, dict) or not read.get("enable", True):
            continue
        if read.get("dlt645Di"):
            segments += 1
    return segments


def build_device_entry(config_path: Path, config: Dict[str, object]) -> List[Dict[str, object]]:
    protocol = ""
    transport = "rs485"
    collect_default = 1000
    protocol_obj = config.get("protocol", {})
    if isinstance(protocol_obj, dict):
        protocol = str(protocol_obj.get("type", ""))
        transport_obj = protocol_obj.get("transport", {})
        if isinstance(transport_obj, dict) and transport_obj.get("host"):
            transport = "tcp"
    collect = config.get("collect", {})
    if isinstance(collect, dict):
        value = collect.get("defaultIntervalMs")
        if isinstance(value, int) and value > 0:
            collect_default = value

    machine_code = str(config.get("machineCode", ""))
    devices: List[Dict[str, object]] = []
    for meter in config.get("meters", []) if isinstance(config.get("meters"), list) else []:
        if not isinstance(meter, dict):
            continue
        points = meter.get("points", [])
        if not isinstance(points, list):
            points = []
        point_count = sum(1 for point in points if isinstance(point, dict) and point.get("enabled", True))
        if protocol.startswith("dlt645"):
            segment_count = count_dlt645_segments([p for p in points if isinstance(p, dict)])
        else:
            segment_count = count_modbus_segments([p for p in points if isinstance(p, dict)], collect_default)
        devices.append(
            {
                "meterCode": str(meter.get("meterCode", "")),
                "protocol": protocol or "modbus_rtu",
                "transport": transport,
                "pointCount": point_count,
                "segmentCount": segment_count,
                "sourceDeviceConfig": str(config_path),
                "sourceMachineCode": machine_code,
            }
        )
    return devices


def main(argv: List[str]) -> int:
    args = parse_args(argv)
    devices: List[Dict[str, object]] = []
    for path_str in args.device_config:
        path = Path(path_str)
        devices.extend(build_device_entry(path, load_json(path)))

    result = {
        "gatewayModel": {
            "machineCodePrefix": args.machine_code_prefix,
            "rs485PortsPerGateway": args.rs485_ports_per_gateway,
        },
        "limits": {
            "maxPointsPerGateway": args.max_points_per_gateway,
            "maxSegmentsPerGateway": args.max_segments_per_gateway,
            "maxDevicesPerGateway": args.max_devices_per_gateway,
            "maxPointsPerPort": args.max_points_per_port,
            "maxSegmentsPerPort": args.max_segments_per_port,
            "maxDevicesPerPort": args.max_devices_per_port,
        },
        "devices": devices,
    }

    payload = json.dumps(result, ensure_ascii=False, indent=2 if args.pretty or args.output else None)
    if args.output:
        output_path = Path(args.output)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(payload + ("\n" if not payload.endswith("\n") else ""), encoding="utf-8")
    else:
        sys.stdout.write(payload)
        if not payload.endswith("\n"):
            sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
