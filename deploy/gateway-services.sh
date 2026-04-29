#!/bin/sh
set -eu

BASE_DIR="${GATEWAY_HOME:-/opt/modbus-gateway}"
DEVICE_DIR="$BASE_DIR/config/runtime/devices"
APP_DIR="$BASE_DIR/config/runtime/apps"
MQTT_APP_NAME="${MQTT_APP_NAME:-mqtt-service}"
MONITOR_APP_NAME="${MONITOR_APP_NAME:-monitor-service}"

stop_units() {
  if ! command -v systemctl >/dev/null 2>&1; then
    return 0
  fi
  units=$(systemctl list-units --all --plain --no-legend \
    'modbus-rtu@*.service' \
    'dlt645-driver@*.service' \
    'event-engine@*.service' \
    'system-monitor@*.service' \
    'mqtt-driver@*.service' 2>/dev/null |
    awk '{print $1}')
  [ -z "$units" ] && return 0
  # Stop all instances together so OTA/config switching is bounded by the slowest
  # driver, not by the sum of every serial port timeout.
  systemctl stop $units || true
  for unit in $units; do
    [ -z "$unit" ] && continue
    systemctl disable "$unit" >/dev/null 2>&1 || true
    systemctl reset-failed "$unit" >/dev/null 2>&1 || true
  done
}

desired_units() {
  python3 - "$DEVICE_DIR" "$APP_DIR" "$MQTT_APP_NAME" "$MONITOR_APP_NAME" <<'PY'
import json
import os
import sys

device_dir, app_dir, mqtt_app_name, monitor_app_name = sys.argv[1:5]
base_dir = os.path.dirname(os.path.dirname(os.path.dirname(device_dir)))

def read_json(path):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except Exception as exc:
        print(f"# skip invalid config {path}: {exc}", file=sys.stderr)
        return None

def resolve_path(raw, app_path):
    if not raw:
        return ""
    if os.path.isabs(raw):
        return raw
    candidates = [
        os.path.normpath(os.path.join(os.path.dirname(app_path), raw)),
        os.path.normpath(os.path.join(base_dir, raw)),
        os.path.normpath(os.path.join(base_dir, "config", raw)),
    ]
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    return candidates[1]

def app_path(name):
    return os.path.join(app_dir, name + ".json")

def emit_device_unit(path):
    root = read_json(path)
    if root is None:
        return
    protocol = str(root.get("protocol", {}).get("type", "")).lower()
    stem = os.path.splitext(os.path.basename(path))[0]
    if protocol in ("modbus_rtu", "modbus_tcp"):
        print(f"modbus-rtu@{stem}.service")
    elif protocol in ("dlt645_2007", "dlt645"):
        print(f"dlt645-driver@{stem}.service")

def app_device_files(path):
    root = read_json(path)
    if root is None:
        return []
    files = root.get("deviceConfigFiles", [])
    if not isinstance(files, list):
        return []
    return [resolve_path(str(item), path) for item in files if str(item)]

seen_devices = set()
for path in (app_path(mqtt_app_name), app_path(monitor_app_name)):
    if not os.path.isfile(path):
        continue
    for device_path in app_device_files(path):
        if device_path in seen_devices:
            continue
        seen_devices.add(device_path)
        emit_device_unit(device_path)

mqtt_path = app_path(mqtt_app_name)
if os.path.isfile(mqtt_path):
    try:
        with open(mqtt_path, "r", encoding="utf-8") as fh:
            app = json.load(fh)
        event_enabled = bool(app.get("eventEngine", {}).get("enabled", False))
    except Exception:
        event_enabled = False
    if event_enabled:
        print(f"event-engine@{mqtt_app_name}.service")
    print(f"mqtt-driver@{mqtt_app_name}.service")

monitor_path = app_path(monitor_app_name)
if os.path.isfile(monitor_path):
    try:
        with open(monitor_path, "r", encoding="utf-8") as fh:
            app = json.load(fh)
        system_monitor_enabled = bool(app.get("systemMonitor", {}).get("enabled", False))
    except Exception:
        system_monitor_enabled = False
    if system_monitor_enabled:
        print(f"system-monitor@{monitor_app_name}.service")
PY
}

start_units() {
  desired_units | while IFS= read -r unit; do
    [ -z "$unit" ] && continue
    case "$unit" in
      \#*) continue ;;
    esac
    echo "[gateway-services] starting $unit"
    systemctl start "$unit"
  done
}

case "${1:-apply}" in
  apply|restart)
    stop_units
    start_units
    ;;
  start)
    start_units
    ;;
  stop)
    stop_units
    ;;
  list)
    desired_units
    ;;
  *)
    echo "Usage: gateway-services.sh [apply|restart|start|stop|list]" >&2
    exit 2
    ;;
esac
