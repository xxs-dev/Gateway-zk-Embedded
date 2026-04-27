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
  systemctl list-units --all --plain --no-legend \
    'modbus-rtu@*.service' \
    'dlt645-driver@*.service' \
    'event-engine@*.service' \
    'system-monitor@*.service' \
    'mqtt-driver@*.service' 2>/dev/null |
    awk '{print $1}' |
    while IFS= read -r unit; do
      [ -z "$unit" ] && continue
      systemctl stop "$unit" || true
      systemctl disable "$unit" >/dev/null 2>&1 || true
      systemctl reset-failed "$unit" || true
    done
}

desired_units() {
  python3 - "$DEVICE_DIR" "$APP_DIR" "$MQTT_APP_NAME" "$MONITOR_APP_NAME" <<'PY'
import json
import os
import sys

device_dir, app_dir, mqtt_app_name, monitor_app_name = sys.argv[1:5]

if os.path.isdir(device_dir):
    for name in sorted(os.listdir(device_dir)):
        if not name.endswith(".json"):
            continue
        path = os.path.join(device_dir, name)
        try:
            with open(path, "r", encoding="utf-8") as fh:
                root = json.load(fh)
        except Exception as exc:
            print(f"# skip invalid config {name}: {exc}", file=sys.stderr)
            continue

        protocol = str(root.get("protocol", {}).get("type", "")).lower()
        stem = name[:-5]
        if protocol in ("modbus_rtu", "modbus_tcp"):
            print(f"modbus-rtu@{stem}.service")
        elif protocol in ("dlt645_2007", "dlt645"):
            print(f"dlt645-driver@{stem}.service")

mqtt_path = os.path.join(app_dir, mqtt_app_name + ".json")
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

monitor_path = os.path.join(app_dir, monitor_app_name + ".json")
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
