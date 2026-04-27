#!/usr/bin/env python3
"""Materialize gateway split plan into device/app json outputs."""

from __future__ import annotations

import argparse
import copy
import json
import os
import sys
from pathlib import Path
from typing import Dict, List, Tuple


def parse_args(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate device/app configs from gateway split plan.")
    parser.add_argument("--inventory", required=True, help="inventory json path")
    parser.add_argument("--plan", required=True, help="planner result json path")
    parser.add_argument("--app-template", required=True, help="base mqtt-service/app json path")
    parser.add_argument("--output-dir", required=True, help="output root dir")
    parser.add_argument("--pretty", action="store_true", help="pretty print generated json")
    return parser.parse_args(argv)


def load_json(path: Path) -> Dict[str, object]:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, data: Dict[str, object], pretty: bool) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(data, ensure_ascii=False, indent=2 if pretty else None)
    if not text.endswith("\n"):
        text += "\n"
    path.write_text(text, encoding="utf-8")


def load_inventory_map(inventory: Dict[str, object]) -> Dict[str, Dict[str, object]]:
    result: Dict[str, Dict[str, object]] = {}
    for item in inventory.get("devices", []) if isinstance(inventory.get("devices"), list) else []:
        if isinstance(item, dict) and item.get("meterCode"):
            result[str(item["meterCode"])] = item
    return result


def load_source_configs(inventory_map: Dict[str, Dict[str, object]]) -> Tuple[Dict[str, Dict[str, object]], Dict[str, Tuple[Dict[str, object], Dict[str, object]]]]:
    config_cache: Dict[str, Dict[str, object]] = {}
    meter_map: Dict[str, Tuple[Dict[str, object], Dict[str, object]]] = {}
    for meter_code, item in inventory_map.items():
        config_path = str(item.get("sourceDeviceConfig", ""))
        if not config_path:
            raise ValueError(f"inventory item missing sourceDeviceConfig: {meter_code}")
        if config_path not in config_cache:
            config_cache[config_path] = load_json(Path(config_path))
        source = config_cache[config_path]
        found = False
        for meter in source.get("meters", []) if isinstance(source.get("meters"), list) else []:
            if isinstance(meter, dict) and str(meter.get("meterCode", "")) == meter_code:
                meter_map[meter_code] = (source, meter)
                found = True
                break
        if not found:
            raise ValueError(f"meterCode not found in source config: {meter_code} -> {config_path}")
    return config_cache, meter_map


def sanitize_port_code(port_code: str) -> str:
    return port_code.lower().replace("/", "_")


def choose_template(port: Dict[str, object], meter_map: Dict[str, Tuple[Dict[str, object], Dict[str, object]]]) -> Dict[str, object]:
    meter_codes = port.get("meterCodes", [])
    if not isinstance(meter_codes, list) or not meter_codes:
        raise ValueError("port has no meterCodes")
    first_meter = str(meter_codes[0])
    source_config, _ = meter_map[first_meter]
    return copy.deepcopy(source_config)


def build_device_config(
    machine_code: str,
    port: Dict[str, object],
    meter_map: Dict[str, Tuple[Dict[str, object], Dict[str, object]]],
) -> Dict[str, object]:
    template = choose_template(port, meter_map)
    port_code = str(port["portCode"])
    meter_codes = port.get("meterCodes", [])
    if not isinstance(meter_codes, list):
        meter_codes = []

    meters: List[Dict[str, object]] = []
    for meter_code in meter_codes:
        _, meter = meter_map[str(meter_code)]
        meters.append(copy.deepcopy(meter))

    template["machineCode"] = machine_code
    template["meters"] = meters
    memory_store = template.get("memoryStore")
    if not isinstance(memory_store, dict):
        memory_store = {}
        template["memoryStore"] = memory_store
    memory_store["sharedMemoryName"] = f"gateway_point_store_{machine_code.lower()}_{sanitize_port_code(port_code)}"

    # Keep per-port sqlite split by gateway/port for easier deployment.
    sqlite_name = f"point_samples_{machine_code.lower()}_{sanitize_port_code(port_code)}.db"
    memory_store["sqlitePath"] = f"/opt/modbus-gateway/data/{sqlite_name}"
    return template


def build_app_config(
    machine_code: str,
    app_template: Dict[str, object],
    device_files: List[Tuple[Path, Dict[str, object]]],
) -> Dict[str, object]:
    app = copy.deepcopy(app_template)
    app["deviceConfigFiles"] = [f"/opt/modbus-gateway/config/runtime/devices/{path.name}" for path, _ in device_files]
    app["machineCode"] = machine_code

    mqtt_driver = app.get("mqttDriver")
    if not isinstance(mqtt_driver, dict):
        mqtt_driver = {}
        app["mqttDriver"] = mqtt_driver
    shared_memory_names: List[str] = []
    for _, device_config in device_files:
        memory_store = device_config.get("memoryStore", {})
        if isinstance(memory_store, dict):
            shm = memory_store.get("sharedMemoryName")
            if isinstance(shm, str) and shm:
                shared_memory_names.append(shm)
    if shared_memory_names:
        mqtt_driver["sharedMemoryNames"] = shared_memory_names
        mqtt_driver["sharedMemoryName"] = shared_memory_names[0]

    alarm_store = app.get("alarmStore")
    if isinstance(alarm_store, dict):
        alarm_store["sqlitePath"] = f"/opt/modbus-gateway/data/alarm_events_{machine_code.lower()}.db"

    mqtt = app.get("mqtt")
    if isinstance(mqtt, dict):
        offline_buffer = mqtt.get("offlineBuffer")
        if isinstance(offline_buffer, dict):
            offline_buffer["dir"] = f"/opt/modbus-gateway/data/mqtt-spool/{machine_code.lower()}"
            offline_buffer["realtimeFile"] = f"/opt/modbus-gateway/data/mqtt-spool/{machine_code.lower()}/realtime_ring.dat"
            event_outbox = offline_buffer.get("eventOutbox")
            if isinstance(event_outbox, dict):
                event_outbox["sqlitePath"] = f"/opt/modbus-gateway/data/mqtt_event_outbox_{machine_code.lower()}.db"
        client_id = mqtt.get("clientId")
        if isinstance(client_id, str) and client_id:
            mqtt["clientId"] = f"{client_id}-{machine_code.lower()}"

    ota = app.get("ota")
    if isinstance(ota, dict):
        ota["downloadDir"] = f"/opt/modbus-gateway/ota/{machine_code.lower()}/downloads"
        ota["stagingDir"] = f"/opt/modbus-gateway/ota/{machine_code.lower()}/staging"
        ota["backupDir"] = f"/opt/modbus-gateway/ota/{machine_code.lower()}/backup"
    return app


def main(argv: List[str]) -> int:
    args = parse_args(argv)
    inventory = load_json(Path(args.inventory))
    plan = load_json(Path(args.plan))
    app_template = load_json(Path(args.app_template))
    output_dir = Path(args.output_dir)

    inventory_map = load_inventory_map(inventory)
    _, meter_map = load_source_configs(inventory_map)
    summary = plan.get("summary")
    if not isinstance(summary, dict):
        raise ValueError("plan missing summary")
    gateways = summary.get("gateways", [])
    if not isinstance(gateways, list):
        raise ValueError("plan summary.gateways must be array")

    generated: Dict[str, object] = {"gateways": []}
    for gateway in gateways:
        if not isinstance(gateway, dict):
            continue
        machine_code = str(gateway.get("machineCode", ""))
        ports = gateway.get("ports", [])
        if not machine_code or not isinstance(ports, list):
            continue
        gateway_dir = output_dir / machine_code
        runtime_devices_dir = gateway_dir / "runtime" / "devices"
        runtime_apps_dir = gateway_dir / "runtime" / "apps"

        device_files: List[Tuple[Path, Dict[str, object]]] = []
        for index, port in enumerate(ports, start=1):
            if not isinstance(port, dict):
                continue
            port_code = str(port.get("portCode", f"RS485_{index}"))
            device_config = build_device_config(machine_code, port, meter_map)
            device_path = runtime_devices_dir / f"device_slave_{machine_code.lower()}_{sanitize_port_code(port_code)}.json"
            write_json(device_path, device_config, args.pretty)
            device_files.append((device_path, device_config))

        app_config = build_app_config(machine_code, app_template, device_files)
        app_path = runtime_apps_dir / "mqtt-service.json"
        write_json(app_path, app_config, args.pretty)

        generated["gateways"].append(
            {
                "machineCode": machine_code,
                "deviceConfigFiles": [str(path) for path, _ in device_files],
                "appConfigFile": str(app_path),
            }
        )

    manifest_path = output_dir / "gateway_split_manifest.json"
    write_json(manifest_path, generated, True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
