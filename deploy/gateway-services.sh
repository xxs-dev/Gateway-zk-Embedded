#!/bin/sh
set -eu

BASE_DIR="${GATEWAY_HOME:-/opt/modbus-gateway}"
DEVICE_DIR="$BASE_DIR/config/runtime/devices"
APP_DIR="$BASE_DIR/config/runtime/apps"
MQTT_APP_NAME="${MQTT_APP_NAME:-mqtt-service}"
MONITOR_APP_NAME="${MONITOR_APP_NAME:-monitor-service}"
CAMERA_APP_NAME="${CAMERA_APP_NAME:-camera-service}"

stop_units() {
  if ! command -v systemctl >/dev/null 2>&1; then
    return 0
  fi
  units=$(
    {
      systemctl list-units --all --plain --no-legend \
        'modbus-rtu@*.service' \
        'dlt645-driver@*.service' \
        'dio-driver@*.service' \
        'can-driver@*.service' \
        'iec-driver@*.service' \
        'compute-engine@*.service' \
        'event-engine@*.service' \
        'local-display@*.service' \
        'local-display-qt@*.service' \
        'local-kiosk@*.service' \
        'system-monitor@*.service' \
        'camera-service@*.service' \
        'direct-agent@*.service' \
        'mqtt-tls-tunnel@*.service' \
        'mqtt-driver@*.service' 2>/dev/null |
        awk '{print $1}'
      systemctl list-unit-files --plain --no-legend \
        'modbus-rtu@*.service' \
        'dlt645-driver@*.service' \
        'dio-driver@*.service' \
        'can-driver@*.service' \
        'iec-driver@*.service' \
        'compute-engine@*.service' \
        'event-engine@*.service' \
        'local-display@*.service' \
        'local-display-qt@*.service' \
        'local-kiosk@*.service' \
        'system-monitor@*.service' \
        'camera-service@*.service' \
        'direct-agent@*.service' \
        'mqtt-tls-tunnel@*.service' \
        'mqtt-driver@*.service' 2>/dev/null |
        awk '{print $1}'
    } | awk '$0 !~ /@\.service$/ && $0 ~ /@.*\.service$/ && !seen[$0]++'
  )
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
  python3 - "$DEVICE_DIR" "$APP_DIR" "$MQTT_APP_NAME" "$MONITOR_APP_NAME" "$CAMERA_APP_NAME" <<'PY'
import json
import glob
import os
import subprocess
import sys
from urllib.parse import urlparse

device_dir, app_dir, mqtt_app_name, monitor_app_name, camera_app_name = sys.argv[1:6]
base_dir = os.path.dirname(os.path.dirname(os.path.dirname(device_dir)))
emitted_units = set()
stunnel_units_by_endpoint = {}

def emit_unit(unit):
    if not unit or unit in emitted_units:
        return
    emitted_units.add(unit)
    print(unit)

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
    if bool_value(root.get("enabled"), True) is False:
        return
    protocol = str(root.get("protocol", {}).get("type", "")).lower()
    stem = os.path.splitext(os.path.basename(path))[0]
    if protocol in ("modbus_rtu", "modbus_tcp"):
        emit_unit(f"modbus-rtu@{stem}.service")
    elif protocol in ("dlt645_2007", "dlt645"):
        emit_unit(f"dlt645-driver@{stem}.service")
    elif protocol in ("local_dio", "dio", "dido"):
        emit_unit(f"dio-driver@{stem}.service")
    elif protocol in ("can_socketcan", "can"):
        emit_unit(f"can-driver@{stem}.service")
    elif protocol in ("iec104", "iec101", "iec103", "iec103_tcp", "iec103_serial"):
        emit_unit(f"iec-driver@{stem}.service")

def app_device_files(path):
    root = read_json(path)
    if root is None:
        return []
    files = root.get("deviceConfigFiles", [])
    if not isinstance(files, list):
        return []
    return [resolve_path(str(item), path) for item in files if str(item)]

def bool_value(value, default=False):
    if isinstance(value, bool):
        return value
    if value is None:
        return default
    return str(value).strip().lower() in ("1", "true", "yes", "on")

def normalize_host(host):
    text = str(host or "").strip().strip("[]").lower()
    if text in ("localhost", "::1", "0:0:0:0:0:0:0:1"):
        return "127.0.0.1"
    return text

def is_loopback_host(host):
    host = normalize_host(host)
    return host == "127.0.0.1" or host.startswith("127.")

def parse_host_port(raw, default_host=""):
    text = str(raw or "").strip()
    if not text:
        return "", None
    if text.startswith("[") and "]" in text:
        host, _, rest = text[1:].partition("]")
        port_text = rest[1:] if rest.startswith(":") else rest
    elif ":" in text:
        host, port_text = text.rsplit(":", 1)
    else:
        host, port_text = default_host, text
    try:
        port = int(str(port_text).strip())
    except Exception:
        return normalize_host(host), None
    return normalize_host(host), port

def parse_broker_endpoint(broker):
    text = str(broker or "").strip()
    if not text:
        return "", None
    if "://" in text:
        try:
            parsed = urlparse(text)
            return normalize_host(parsed.hostname or ""), parsed.port
        except Exception:
            return "", None
    return parse_host_port(text)

def parse_stunnel_accept(path):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            for raw_line in fh:
                line = raw_line.split("#", 1)[0].split(";", 1)[0].strip()
                if "=" not in line:
                    continue
                key, value = line.split("=", 1)
                if key.strip().lower() == "accept":
                    return parse_host_port(value, "0.0.0.0")
    except Exception as exc:
        print(f"# skip invalid stunnel config {path}: {exc}", file=sys.stderr)
    return "", None

def stunnel_accept_matches(accept_host, broker_host):
    accept_host = normalize_host(accept_host)
    broker_host = normalize_host(broker_host)
    if accept_host in ("", "0.0.0.0", "::", "*"):
        return is_loopback_host(broker_host)
    return accept_host == broker_host

def stunnel_unit_for_app(path):
    root = read_json(path)
    if root is None:
        return ""
    mqtt = root.get("mqtt", {}) or {}
    if not isinstance(mqtt, dict):
        return ""
    if bool_value(mqtt.get("enabled"), True) is False:
        return ""
    broker_host, broker_port = parse_broker_endpoint(mqtt.get("broker"))
    if broker_port is None or not is_loopback_host(broker_host):
        return ""
    broker_endpoint = (normalize_host(broker_host), broker_port)
    if broker_endpoint in stunnel_units_by_endpoint:
        return stunnel_units_by_endpoint[broker_endpoint]

    tls_dir = os.path.join(base_dir, "config", "runtime", "tls")
    preferred = os.path.join(tls_dir, os.path.splitext(os.path.basename(path))[0] + "-stunnel.conf")
    candidates = []
    for candidate in [preferred] + sorted(glob.glob(os.path.join(tls_dir, "*-stunnel.conf"))):
        if candidate not in candidates:
            candidates.append(candidate)
    for candidate in candidates:
        if not os.path.isfile(candidate):
            continue
        accept_host, accept_port = parse_stunnel_accept(candidate)
        if accept_port == broker_port and stunnel_accept_matches(accept_host, broker_host):
            stem = os.path.splitext(os.path.basename(candidate))[0]
            unit = f"mqtt-tls-tunnel@{stem}.service"
            stunnel_units_by_endpoint[broker_endpoint] = unit
            return unit
    return ""

def camera_service_enabled(path):
    root = read_json(path)
    if root is None:
        return False
    service = root.get("cameraService", {}) or {}
    if not bool_value(service.get("enabled"), False):
        return False
    cameras = service.get("cameras", [])
    if not isinstance(cameras, list):
        return False
    return any(bool_value((item or {}).get("enabled"), True) for item in cameras if isinstance(item, dict))

def mqtt_driver_process_needed(app):
    if not isinstance(app, dict):
        return False
    mqtt = app.get("mqtt", {}) or {}
    mqtt_enabled = bool_value(mqtt.get("enabled"), False)
    if not mqtt_enabled:
        return False
    mqtt_driver = app.get("mqttDriver", {}) or {}
    event_engine = app.get("eventEngine", {}) or {}
    ota = app.get("ota", {}) or {}
    realtime = app.get("realtime", {}) or {}
    if bool_value(mqtt_driver.get("enabled"), False):
        return True
    if bool_value(ota.get("enabled"), False):
        return True
    if bool_value(realtime.get("enabled"), False):
        return True
    if bool_value(event_engine.get("enabled"), False) and str(event_engine.get("publishMode", "")).strip() == "mqtt_driver_outbox":
        return True
    for key in (
        "commandRequestTopic",
        "otaRequestTopic",
        "realtimeRequestTopic",
    ):
        if str(mqtt.get(key, "")).strip():
            return True
    return False

def connector_has_edid(path):
    edid = os.path.join(os.path.dirname(path), "edid")
    try:
        return os.path.isfile(edid) and os.path.getsize(edid) > 0
    except Exception:
        return False

def display_connected_by_sysfs(expected_output="", require_edid=True):
    patterns = [
        "/sys/class/drm/card*-HDMI*/status",
        "/sys/class/drm/card*-DP*/status",
        "/sys/class/drm/card*-DPI*/status",
        "/sys/class/drm/card*-DSI*/status",
        "/sys/class/drm/card*-LVDS*/status",
        "/sys/class/drm/card*-VGA*/status",
    ]
    for pattern in patterns:
        for path in glob.glob(pattern):
            name = os.path.basename(os.path.dirname(path))
            if expected_output and not name.endswith("-" + expected_output):
                continue
            try:
                with open(path, "r", encoding="utf-8") as fh:
                    if fh.read().strip().lower() == "connected" and (not require_edid or connector_has_edid(path)):
                        return True
            except Exception:
                pass
    return False

def display_connected_by_xrandr(expected_output="", require_edid=True):
    if require_edid:
        return False
    xrandr = "/usr/bin/xrandr"
    if not os.path.exists(xrandr):
        return False
    env = os.environ.copy()
    env.setdefault("DISPLAY", ":0")
    env.setdefault("XAUTHORITY", "/run/user/1000/gdm/Xauthority")
    env.setdefault("XDG_RUNTIME_DIR", "/run/user/1000")
    try:
        result = subprocess.run(
            [xrandr, "--query"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            universal_newlines=True,
            timeout=2,
            env=env,
        )
    except Exception:
        return False
    if result.returncode != 0:
        return False
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "connected":
            if expected_output:
                return parts[0] == expected_output
            name = parts[0].lower()
            if name.startswith(("hdmi", "dp", "displayport", "dpi", "dsi", "lvds", "vga")):
                return True
    return False

def display_connected(local_display=None):
    kiosk = (local_display or {}).get("kiosk", {}) or {}
    expected_output = str(kiosk.get("displayOutput") or "").strip()
    require_edid = bool_value(kiosk.get("requireEdid"), False if expected_output else True)
    if expected_output:
        return display_connected_by_sysfs(expected_output, require_edid) or display_connected_by_xrandr(expected_output, require_edid)
    return display_connected_by_sysfs("", require_edid) or display_connected_by_xrandr("", require_edid)

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
        compute_enabled = bool_value((app.get("computeEngine", {}) or {}).get("enabled"), False)
        event_enabled = bool_value((app.get("eventEngine", {}) or {}).get("enabled"), False)
        mqtt_driver_needed = mqtt_driver_process_needed(app)
    except Exception:
        compute_enabled = False
        event_enabled = False
        mqtt_driver_needed = False
    if mqtt_driver_needed or event_enabled:
        emit_unit(stunnel_unit_for_app(mqtt_path))
    if compute_enabled:
        emit_unit(f"compute-engine@{mqtt_app_name}.service")
    if event_enabled:
        emit_unit(f"event-engine@{mqtt_app_name}.service")
    if mqtt_driver_needed:
        emit_unit(f"mqtt-driver@{mqtt_app_name}.service")

monitor_path = app_path(monitor_app_name)
if os.path.isfile(monitor_path):
    try:
        with open(monitor_path, "r", encoding="utf-8") as fh:
            app = json.load(fh)
        system_monitor_enabled = bool(app.get("systemMonitor", {}).get("enabled", False))
        local_display = app.get("localDisplay", {}) or {}
        local_display_enabled = bool_value(local_display.get("enabled"), False)
        kiosk = local_display.get("kiosk", {}) or {}
        kiosk_enabled = bool_value(kiosk.get("enabled"), True)
        kiosk_require_display = bool_value(kiosk.get("requireDisplayConnected"), True)
    except Exception:
        system_monitor_enabled = False
        local_display_enabled = False
        kiosk_enabled = False
        kiosk_require_display = True
    if system_monitor_enabled:
        emit_unit(stunnel_unit_for_app(monitor_path))
        emit_unit(f"system-monitor@{monitor_app_name}.service")
    if local_display_enabled:
        emit_unit(f"local-display@{monitor_app_name}.service")
        if kiosk_enabled and (not kiosk_require_display or display_connected(local_display)):
            emit_unit(f"local-kiosk@{monitor_app_name}.service")
        elif kiosk_enabled:
            print("# skip local-kiosk: no connected HDMI/display output", file=sys.stderr)

camera_path = app_path(camera_app_name)
if os.path.isfile(camera_path) and camera_service_enabled(camera_path):
    emit_unit(stunnel_unit_for_app(camera_path))
    emit_unit(f"camera-service@{camera_app_name}.service")

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
