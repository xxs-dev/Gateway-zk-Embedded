#!/usr/bin/env python3
"""Plan gateway allocation from device workload inventory.

Input JSON example:
{
  "gatewayModel": {
    "machineCodePrefix": "GW",
    "rs485PortsPerGateway": 8
  },
  "limits": {
    "maxPointsPerGateway": 12000,
    "maxSegmentsPerGateway": 320,
    "maxDevicesPerGateway": 200,
    "maxPointsPerPort": 2000,
    "maxSegmentsPerPort": 64,
    "maxDevicesPerPort": 32
  },
  "devices": [
    {
      "meterCode": "SLAVE0001",
      "protocol": "modbus_rtu",
      "pointCount": 350,
      "segmentCount": 8
    }
  ]
}
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple


def require_int(obj: Dict[str, object], key: str, default: Optional[int] = None) -> int:
    value = obj.get(key, default)
    if value is None:
        raise ValueError(f"missing required integer field: {key}")
    if not isinstance(value, int):
        raise ValueError(f"field must be integer: {key}")
    return value


def require_str(obj: Dict[str, object], key: str, default: Optional[str] = None) -> str:
    value = obj.get(key, default)
    if value is None:
        raise ValueError(f"missing required string field: {key}")
    if not isinstance(value, str):
        raise ValueError(f"field must be string: {key}")
    return value


@dataclass
class Device:
    meter_code: str
    protocol: str
    point_count: int
    segment_count: int
    weight: float = 1.0
    preferred_gateway: str = ""
    preferred_port: str = ""
    transport: str = "rs485"

    @property
    def normalized_points(self) -> int:
        return max(0, self.point_count)

    @property
    def normalized_segments(self) -> int:
        return max(0, self.segment_count)


@dataclass
class PortState:
    code: str
    protocol: str = ""
    transport: str = "rs485"
    point_count: int = 0
    segment_count: int = 0
    devices: List[Device] = field(default_factory=list)

    def can_fit(self, device: Device, limits: Dict[str, int]) -> bool:
        if self.transport != device.transport:
            return False
        if self.protocol and self.protocol != device.protocol:
            return False
        if self.point_count + device.normalized_points > limits["maxPointsPerPort"]:
            return False
        if self.segment_count + device.normalized_segments > limits["maxSegmentsPerPort"]:
            return False
        if len(self.devices) + 1 > limits["maxDevicesPerPort"]:
            return False
        return True

    def add(self, device: Device) -> None:
        if not self.protocol:
            self.protocol = device.protocol
        self.point_count += device.normalized_points
        self.segment_count += device.normalized_segments
        self.devices.append(device)


@dataclass
class GatewayState:
    machine_code: str
    rs485_port_capacity: int
    points: int = 0
    segments: int = 0
    ports: List[PortState] = field(default_factory=list)

    @property
    def device_count(self) -> int:
        return sum(len(port.devices) for port in self.ports)

    def can_fit_gateway_level(self, device: Device, limits: Dict[str, int]) -> bool:
        if self.points + device.normalized_points > limits["maxPointsPerGateway"]:
            return False
        if self.segments + device.normalized_segments > limits["maxSegmentsPerGateway"]:
            return False
        if self.device_count + 1 > limits["maxDevicesPerGateway"]:
            return False
        return True

    def find_best_port(self, device: Device, limits: Dict[str, int]) -> Optional[PortState]:
        candidates: List[Tuple[float, PortState]] = []
        for port in self.ports:
            if not port.can_fit(device, limits):
                continue
            after_points = port.point_count + device.normalized_points
            after_segments = port.segment_count + device.normalized_segments
            score = after_points + after_segments * 16 + len(port.devices) * 32
            if port.protocol == device.protocol:
                score -= 128
            candidates.append((score, port))
        if candidates:
            candidates.sort(key=lambda item: item[0])
            return candidates[0][1]
        if len(self.ports) >= self.rs485_port_capacity:
            return None
        new_port = PortState(code=f"RS485_{len(self.ports) + 1}")
        if not new_port.can_fit(device, limits):
            return None
        self.ports.append(new_port)
        return new_port

    def add(self, port: PortState, device: Device) -> None:
        port.add(device)
        self.points += device.normalized_points
        self.segments += device.normalized_segments


def load_inventory(path: Path) -> Tuple[Dict[str, int], Dict[str, object], List[Device]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    gateway_model = data.get("gatewayModel", {})
    limits = data.get("limits", {})
    if not isinstance(gateway_model, dict) or not isinstance(limits, dict):
        raise ValueError("gatewayModel and limits must be objects")

    normalized_limits = {
        "maxPointsPerGateway": require_int(limits, "maxPointsPerGateway"),
        "maxSegmentsPerGateway": require_int(limits, "maxSegmentsPerGateway"),
        "maxDevicesPerGateway": require_int(limits, "maxDevicesPerGateway", 1000000),
        "maxPointsPerPort": require_int(limits, "maxPointsPerPort"),
        "maxSegmentsPerPort": require_int(limits, "maxSegmentsPerPort"),
        "maxDevicesPerPort": require_int(limits, "maxDevicesPerPort", 1000000),
    }

    if "devices" not in data or not isinstance(data["devices"], list):
        raise ValueError("devices must be an array")

    devices: List[Device] = []
    for item in data["devices"]:
        if not isinstance(item, dict):
            raise ValueError("device entry must be object")
        devices.append(
            Device(
                meter_code=require_str(item, "meterCode"),
                protocol=require_str(item, "protocol", "modbus_rtu"),
                point_count=require_int(item, "pointCount"),
                segment_count=require_int(item, "segmentCount"),
                weight=float(item.get("weight", 1.0)),
                preferred_gateway=require_str(item, "preferredGateway", ""),
                preferred_port=require_str(item, "preferredPort", ""),
                transport=require_str(item, "transport", "rs485"),
            )
        )
    return normalized_limits, gateway_model, devices


def sort_devices(devices: List[Device]) -> List[Device]:
    return sorted(
        devices,
        key=lambda item: (
            -(item.segment_count * 1024 + item.point_count * 4),
            item.protocol,
            item.meter_code,
        ),
    )


def device_fits_alone(device: Device, limits: Dict[str, int]) -> bool:
    return (
        device.normalized_points <= limits["maxPointsPerGateway"]
        and device.normalized_segments <= limits["maxSegmentsPerGateway"]
        and device.normalized_points <= limits["maxPointsPerPort"]
        and device.normalized_segments <= limits["maxSegmentsPerPort"]
        and limits["maxDevicesPerGateway"] >= 1
        and limits["maxDevicesPerPort"] >= 1
    )


def make_gateway_code(prefix: str, index: int) -> str:
    return f"{prefix}{index:04d}"


def find_or_create_gateway(
    gateways: List[GatewayState],
    gateway_model: Dict[str, object],
    limits: Dict[str, int],
    device: Device,
) -> GatewayState:
    prefix = require_str(gateway_model, "machineCodePrefix", "GW")
    port_capacity = require_int(gateway_model, "rs485PortsPerGateway", 8)

    candidate_gateways = gateways
    if device.preferred_gateway:
        candidate_gateways = [g for g in gateways if g.machine_code == device.preferred_gateway]

    best: Optional[Tuple[float, GatewayState, PortState]] = None
    for gateway in candidate_gateways:
        if not gateway.can_fit_gateway_level(device, limits):
            continue
        port = gateway.find_best_port(device, limits)
        if port is None:
            continue
        score = (
            (gateway.points + device.normalized_points) / max(1, limits["maxPointsPerGateway"])
            + (gateway.segments + device.normalized_segments) / max(1, limits["maxSegmentsPerGateway"]) * 2
            + len(gateway.ports) * 0.05
        )
        if best is None or score < best[0]:
            best = (score, gateway, port)

    if best is not None:
        return best[1]

    gateway = GatewayState(
        machine_code=device.preferred_gateway or make_gateway_code(prefix, len(gateways) + 1),
        rs485_port_capacity=port_capacity,
    )
    gateways.append(gateway)
    return gateway


def place_devices(
    devices: List[Device],
    gateway_model: Dict[str, object],
    limits: Dict[str, int],
) -> Tuple[List[GatewayState], List[Dict[str, object]]]:
    gateways: List[GatewayState] = []
    rejected: List[Dict[str, object]] = []

    for device in sort_devices(devices):
        if not device_fits_alone(device, limits):
            rejected.append(
                {
                    "meterCode": device.meter_code,
                    "reason": "device exceeds standalone gateway or port capacity",
                    "pointCount": device.point_count,
                    "segmentCount": device.segment_count,
                }
            )
            continue

        gateway = find_or_create_gateway(gateways, gateway_model, limits, device)
        if not gateway.can_fit_gateway_level(device, limits):
            rejected.append(
                {
                    "meterCode": device.meter_code,
                    "reason": "no gateway capacity available",
                    "pointCount": device.point_count,
                    "segmentCount": device.segment_count,
                }
            )
            continue

        port: Optional[PortState] = None
        if device.preferred_port:
            for existing in gateway.ports:
                if existing.code == device.preferred_port:
                    port = existing
                    break
            if port is None and len(gateway.ports) < gateway.rs485_port_capacity:
                port = PortState(code=device.preferred_port)
                gateway.ports.append(port)
            if port is None or not port.can_fit(device, limits):
                rejected.append(
                    {
                        "meterCode": device.meter_code,
                        "reason": "preferred port unavailable",
                        "pointCount": device.point_count,
                        "segmentCount": device.segment_count,
                    }
                )
                continue
        else:
            port = gateway.find_best_port(device, limits)
            if port is None:
                rejected.append(
                    {
                        "meterCode": device.meter_code,
                        "reason": "no port capacity available",
                        "pointCount": device.point_count,
                        "segmentCount": device.segment_count,
                    }
                )
                continue

        gateway.add(port, device)
    return gateways, rejected


def summarize(gateways: List[GatewayState], rejected: List[Dict[str, object]]) -> Dict[str, object]:
    result_gateways = []
    for gateway in gateways:
        ports = []
        for port in gateway.ports:
            ports.append(
                {
                    "portCode": port.code,
                    "protocol": port.protocol,
                    "transport": port.transport,
                    "deviceCount": len(port.devices),
                    "pointCount": port.point_count,
                    "segmentCount": port.segment_count,
                    "meterCodes": [item.meter_code for item in port.devices],
                }
            )
        result_gateways.append(
            {
                "machineCode": gateway.machine_code,
                "deviceCount": gateway.device_count,
                "pointCount": gateway.points,
                "segmentCount": gateway.segments,
                "portCount": len(gateway.ports),
                "ports": ports,
            }
        )
    return {
        "gatewayCount": len(result_gateways),
        "rejectedCount": len(rejected),
        "gateways": result_gateways,
        "rejected": rejected,
    }


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description="Plan gateway split by points and segment counts.")
    parser.add_argument("--input", required=True, help="Inventory json path")
    parser.add_argument("--output", help="Write result json path")
    parser.add_argument("--pretty", action="store_true", help="Pretty print JSON result")
    args = parser.parse_args(argv)

    limits, gateway_model, devices = load_inventory(Path(args.input))
    gateways, rejected = place_devices(devices, gateway_model, limits)
    result = {
        "gatewayModel": gateway_model,
        "limits": limits,
        "summary": summarize(gateways, rejected),
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
