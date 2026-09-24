#!/bin/sh
set -eu

echo "legacy V9 rolling upgrade is retired: SHM11/KECP2 requires offline all-participant migration; no files changed" >&2
exit 2

GATEWAY_HOME=${GATEWAY_HOME:-/opt/modbus-gateway}
STAGE_DIR=${STAGE_DIR:-/tmp/gateway-runtime-v9}
HEALTH_TIMEOUT_SEC=${HEALTH_TIMEOUT_SEC:-45}
MIN_RECOVERY_PERCENT=${MIN_RECOVERY_PERCENT:-50}
PREFLIGHT_ONLY=${PREFLIGHT_ONLY:-0}
EXTERNAL_CONTINUITY=${EXTERNAL_CONTINUITY:-0}
RESET_REALTIME_RING=${RESET_REALTIME_RING:-0}
BIN_DIR="$GATEWAY_HOME/bin"
APP_CONFIG="$GATEWAY_HOME/config/runtime/apps/mqtt-service.json"
FORWARD_HEALTH="$GATEWAY_HOME/run/mqtt-forwarder-health.json"
EASYTIER_CONFIG=${EASYTIER_CONFIG:-/etc/easytier/et.conf}
STAMP=$(date +%Y%m%d-%H%M%S)
BACKUP_DIR="$GATEWAY_HOME/data/runtime-backups/v9-$STAMP"
STATE_DIR="$BACKUP_DIR/state"
ACTIVE_UNITS="$STATE_DIR/active-units.txt"
BINARIES="ModbusRtu Dlt645Driver DioDriver CanDriver IecDriver MqttDriver MqttForwarder EventEngine ComputeEngine SystemMonitor pointctl"
BACKUP_READY=0
UPGRADE_STARTED=0
CONTINUITY_STARTED=0
CONTINUITY_DRIVER_PID=""
REALTIME_RING_FILE=""
REALTIME_RING_BACKUP_READY=0

fail() {
    echo "runtime upgrade failed: $*" >&2
    exit 1
}

json_health_ok() {
    python3 - "$FORWARD_HEALTH" "$BASELINE_FORWARD_COUNT" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as source:
    data = json.load(source)
count = int(data.get("valueCount") or 0)
if not data.get("healthy") or count <= 0:
    raise SystemExit(1)
PY
}

live_point_count() {
    total=0
    for name in $LIVE_SHARED_MEMORY_NAMES; do
        count=$("$BIN_DIR/pointctl" stats --shm "$name" 2>/dev/null | \
            sed -n 's/.* latest=\([0-9][0-9]*\)\/.*/\1/p')
        [ -n "$count" ] || return 1
        total=$((total + count))
    done
    echo "$total"
}

all_units_active() {
    while IFS= read -r unit; do
        [ -z "$unit" ] && continue
        systemctl is-active --quiet "$unit" || return 1
    done < "$ACTIVE_UNITS"
}

all_gateway_shm_v9() {
    found=0
    for segment in /dev/shm/gateway_point_store*; do
        [ -f "$segment" ] || continue
        found=1
        header=$(od -An -tx4 -N8 "$segment" 2>/dev/null | tr -d ' \n')
        [ "$header" = "4d50535400000009" ] || return 1
    done
    [ "$found" -eq 1 ]
}

verify_mqtt_outputs() {
    command -v mosquitto_sub >/dev/null 2>&1 || fail "mosquitto_sub is required"
    MQTT_FIELDS_FILE="$STATE_DIR/mqtt-verification-fields"
    python3 - "$APP_CONFIG" "$ACTUAL_MACHINE_CODE" "$MQTT_FIELDS_FILE" <<'PY'
import json
import sys
from urllib.parse import urlparse

config = json.load(open(sys.argv[1], encoding="utf-8"))
machine_code = sys.argv[2]
output_path = sys.argv[3]
mqtt = config.get("mqtt") or {}
forward = config.get("mqttForward") or {}
if not mqtt.get("enabled") or not forward.get("enabled"):
    raise SystemExit("both mqtt and mqttForward must be enabled")
broker = urlparse(str(mqtt.get("broker") or ""))
forward_broker = urlparse(str(forward.get("broker") or ""))
if broker.scheme != "tcp" or forward_broker.scheme != "tcp":
    raise SystemExit("rolling verifier currently requires tcp MQTT brokers")
if (broker.hostname, broker.port) != (forward_broker.hostname, forward_broker.port):
    raise SystemExit("rolling verifier requires both outputs on the same broker")
fields = [
    broker.hostname or "",
    str(broker.port or 1883),
    str(mqtt.get("username") or ""),
    str(mqtt.get("password") or ""),
    f'{str(mqtt.get("telemetryTopic") or "edge/telemetry").rstrip("/")}/full/{machine_code}',
    f'{str(forward.get("fullTelemetryTopic") or "").rstrip("/")}/{machine_code}',
]
if any("\n" in field for field in fields) or not fields[0] or not fields[5]:
    raise SystemExit("invalid MQTT verification fields")
with open(output_path, "w", encoding="utf-8") as output:
    output.write("\n".join(fields) + "\n")
PY
    MQTT_HOST=$(sed -n '1p' "$MQTT_FIELDS_FILE")
    MQTT_PORT=$(sed -n '2p' "$MQTT_FIELDS_FILE")
    MQTT_USERNAME=$(sed -n '3p' "$MQTT_FIELDS_FILE")
    MQTT_PASSWORD=$(sed -n '4p' "$MQTT_FIELDS_FILE")
    MAIN_TOPIC=$(sed -n '5p' "$MQTT_FIELDS_FILE")
    LEGACY_TOPIC=$(sed -n '6p' "$MQTT_FIELDS_FILE")

    verify_dir="$STATE_DIR/mqtt-verification"
    mkdir -p "$verify_dir"
    timeout 18 mosquitto_sub -h "$MQTT_HOST" -p "$MQTT_PORT" -u "$MQTT_USERNAME" -P "$MQTT_PASSWORD" \
        -V mqttv5 -q 1 -t "$MAIN_TOPIC" -C 1 > "$verify_dir/main.json" &
    main_pid=$!
    timeout 18 mosquitto_sub -h "$MQTT_HOST" -p "$MQTT_PORT" -u "$MQTT_USERNAME" -P "$MQTT_PASSWORD" \
        -V mqttv5 -q 1 -t "$LEGACY_TOPIC" -C 1 > "$verify_dir/legacy.json" &
    legacy_pid=$!
    wait "$main_pid" || fail "main MQTT output did not recover: $MAIN_TOPIC"
    wait "$legacy_pid" || fail "legacy MQTT output did not recover: $LEGACY_TOPIC"

    python3 - "$verify_dir/main.json" "$verify_dir/legacy.json" "$ACTUAL_MACHINE_CODE" <<'PY'
import json
import sys

main = json.load(open(sys.argv[1], encoding="utf-8"))
legacy = json.load(open(sys.argv[2], encoding="utf-8"))
machine_code = sys.argv[3]
if main.get("machineCode") != machine_code or not isinstance(main.get("meters"), list) or not main["meters"]:
    raise SystemExit("main MQTT payload structure is invalid")
if not isinstance(legacy.get("data"), list) or not legacy["data"]:
    raise SystemExit("legacy MQTT payload structure is invalid")
PY
}

stop_units() {
    units=$(tr '\n' ' ' < "$ACTIVE_UNITS")
    [ -z "$units" ] || systemctl stop $units
}

stop_continuity_publishers() {
    [ "$CONTINUITY_STARTED" -eq 1 ] || return 0
    for pid in "$CONTINUITY_DRIVER_PID"; do
        [ -n "$pid" ] || continue
        kill -TERM "$pid" >/dev/null 2>&1 || true
    done
    attempt=0
    while [ "$attempt" -lt 6 ]; do
        alive=0
        for pid in "$CONTINUITY_DRIVER_PID"; do
            [ -n "$pid" ] && kill -0 "$pid" >/dev/null 2>&1 && alive=1
        done
        [ "$alive" -eq 0 ] && break
        attempt=$((attempt + 1))
        sleep 0.5
    done
    for pid in "$CONTINUITY_DRIVER_PID"; do
        [ -n "$pid" ] || continue
        kill -KILL "$pid" >/dev/null 2>&1 || true
        wait "$pid" >/dev/null 2>&1 || true
    done
    rm -f /dev/shm/upgrade_hold_v8_* 2>/dev/null || true
    CONTINUITY_STARTED=0
}

start_continuity_publishers() {
    CONTINUITY_CONFIG="$STATE_DIR/mqtt-continuity.json"
    python3 - "$APP_CONFIG" "$CONTINUITY_CONFIG" "$STATE_DIR" <<'PY'
import copy
import json
import os
import shutil
import sys

source_path, output_path, state_dir = sys.argv[1:]
config = json.load(open(source_path, encoding="utf-8"))
driver = config.get("mqttDriver") or {}
names = list(driver.get("sharedMemoryNames") or [])
primary = str(driver.get("sharedMemoryName") or "").strip()
if primary and primary not in names:
    names.insert(0, primary)
if not names:
    raise SystemExit("no MQTT shared memory names configured")

mapping = {name: "upgrade_hold_v8_" + name for name in names}
for old_name, new_name in mapping.items():
    shutil.copyfile("/dev/shm/" + old_name.lstrip("/"), "/dev/shm/" + new_name)
default_hold_name = "upgrade_hold_v8_gateway_point_store"
if os.path.exists("/dev/shm/gateway_point_store"):
    shutil.copyfile("/dev/shm/gateway_point_store", "/dev/shm/" + default_hold_name)

device_dir = os.path.join(state_dir, "continuity-devices")
os.makedirs(device_dir, exist_ok=True)
new_device_files = []
for index, device_path in enumerate(config.get("deviceConfigFiles") or []):
    device = json.load(open(device_path, encoding="utf-8"))
    def replace(value):
        if isinstance(value, dict):
            return {key: replace(item) for key, item in value.items()}
        if isinstance(value, list):
            return [replace(item) for item in value]
        if isinstance(value, str):
            return mapping.get(value, value)
        return value
    device = replace(device)
    target = os.path.join(device_dir, f"device-{index}.json")
    with open(target, "w", encoding="utf-8") as output:
        json.dump(device, output, ensure_ascii=False, indent=2)
        output.write("\n")
    new_device_files.append(target)

config["deviceConfigFiles"] = new_device_files
driver["sharedMemoryName"] = mapping.get(primary, next(iter(mapping.values())))
driver["sharedMemoryNames"] = [mapping[name] for name in names]
config["mqttDriver"] = driver
config.setdefault("mqtt", {})["clientId"] = str(config["mqtt"].get("clientId") or "gateway") + "-upgrade-hold"
config["mqtt"]["legacyTelemetryEnabled"] = True
for key in ("commandRequestTopic", "otaRequestTopic", "systemMonitorRequestTopic", "diagRequestTopic", "configPullRequestTopic"):
    if key in config["mqtt"]:
        config["mqtt"][key] = "upgrade/hold/disabled/" + key
config["mqtt"].setdefault("offlineBuffer", {})["enabled"] = False
config.setdefault("alarmStore", {})["enabled"] = False
config.setdefault("mqttForward", {})["clientId"] = str(config["mqttForward"].get("clientId") or "gateway-forward") + "-upgrade-hold"
config["mqttForward"]["enabled"] = False
camera = config.setdefault("cameraService", {})
camera["enabled"] = False
camera["sharedMemoryName"] = default_hold_name
with open(output_path, "w", encoding="utf-8") as output:
    json.dump(config, output, ensure_ascii=False, indent=2)
    output.write("\n")
print(" ".join(names))
PY
    LIVE_SHARED_MEMORY_NAMES=$(python3 - "$APP_CONFIG" <<'PY'
import json
import sys
config = json.load(open(sys.argv[1], encoding="utf-8"))
driver = config.get("mqttDriver") or {}
names = list(driver.get("sharedMemoryNames") or [])
primary = str(driver.get("sharedMemoryName") or "").strip()
if primary and primary not in names:
    names.insert(0, primary)
print(" ".join(names))
PY
    )
    "$BACKUP_DIR/bin/MqttDriver" --app-config "$CONTINUITY_CONFIG" \
        > "$STATE_DIR/mqtt-continuity-driver.log" 2>&1 &
    CONTINUITY_DRIVER_PID=$!
    CONTINUITY_STARTED=1
    sleep 2
    kill -0 "$CONTINUITY_DRIVER_PID" >/dev/null 2>&1 || fail "MQTT continuity driver failed to start"
}

load_live_shared_memory_names() {
    LIVE_SHARED_MEMORY_NAMES=$(python3 - "$APP_CONFIG" <<'PY'
import json
import sys
config = json.load(open(sys.argv[1], encoding="utf-8"))
driver = config.get("mqttDriver") or {}
names = list(driver.get("sharedMemoryNames") or [])
primary = str(driver.get("sharedMemoryName") or "").strip()
if primary and primary not in names:
    names.insert(0, primary)
print(" ".join(names))
PY
    )
    [ -n "$LIVE_SHARED_MEMORY_NAMES" ] || fail "no live MQTT shared memory names configured"
}

start_units() {
    units=$(tr '\n' ' ' < "$ACTIVE_UNITS")
    [ -z "$units" ] || systemctl start $units
}

clear_gateway_shm() {
    if command -v fuser >/dev/null 2>&1; then
        for segment in /dev/shm/gateway_point_store*; do
            [ -e "$segment" ] || continue
            ! fuser "$segment" >/dev/null 2>&1 || fail "shared memory still in use: $segment"
        done
    fi
    rm -f /dev/shm/gateway_point_store*
}

restore_binaries() {
    for name in $BINARIES; do
        [ -f "$BACKUP_DIR/bin/$name" ] || continue
        install -m 0755 "$BACKUP_DIR/bin/$name" "$BIN_DIR/$name.restore"
        mv -f "$BIN_DIR/$name.restore" "$BIN_DIR/$name"
    done
}

rollback() {
    code=$?
    trap - EXIT INT TERM
    if [ "$code" -ne 0 ] && [ "$BACKUP_READY" -eq 1 ] && [ "$UPGRADE_STARTED" -eq 1 ]; then
        echo "upgrade validation failed; rolling back from $BACKUP_DIR" >&2
        stop_units >/dev/null 2>&1 || true
        restore_binaries
        if [ "$REALTIME_RING_BACKUP_READY" -eq 1 ]; then
            cp -p "$STATE_DIR/realtime-ring.before" "$REALTIME_RING_FILE"
        fi
        rm -f /dev/shm/gateway_point_store* 2>/dev/null || true
        start_units >/dev/null 2>&1 || true
        sleep 3
        stop_continuity_publishers
        echo "rollback completed; backup=$BACKUP_DIR" >&2
    else
        stop_continuity_publishers
    fi
    exit "$code"
}
trap rollback EXIT INT TERM

[ "$(id -u)" -eq 0 ] || fail "must run as root"
[ -f "$APP_CONFIG" ] || fail "app config missing: $APP_CONFIG"
[ -f "$EASYTIER_CONFIG" ] || fail "EasyTier config missing: $EASYTIER_CONFIG"
command -v python3 >/dev/null 2>&1 || fail "python3 is required"
command -v systemctl >/dev/null 2>&1 || fail "systemctl is required"

EXPECTED_MACHINE_CODE=${EXPECTED_MACHINE_CODE:-}
ACTUAL_MACHINE_CODE=$(python3 - "$GATEWAY_HOME/config/runtime/device_identity.json" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as source:
    print(str(json.load(source).get("machineCode") or "").strip())
PY
)
[ -n "$ACTUAL_MACHINE_CODE" ] || fail "machineCode is empty"
[ -z "$EXPECTED_MACHINE_CODE" ] || [ "$ACTUAL_MACHINE_CODE" = "$EXPECTED_MACHINE_CODE" ] || \
    fail "machineCode mismatch: expected $EXPECTED_MACHINE_CODE actual $ACTUAL_MACHINE_CODE"
load_live_shared_memory_names
SINGLE_MQTT_DUAL_TOPIC=$(python3 - "$APP_CONFIG" <<'PY'
import json
import sys
config = json.load(open(sys.argv[1], encoding="utf-8"))
mqtt = config.get("mqtt") or {}
forward = config.get("mqttForward") or {}
print(1 if mqtt.get("legacyTelemetryEnabled") and not forward.get("enabled") else 0)
PY
)

for name in $BINARIES; do
    [ -x "$STAGE_DIR/$name" ] || fail "staged binary missing: $STAGE_DIR/$name"
done
[ -f "$STAGE_DIR/SHA256SUMS" ] || fail "SHA256SUMS missing"
(cd "$STAGE_DIR" && sha256sum -c SHA256SUMS)

BASELINE_FORWARD_COUNT=0
if [ "$SINGLE_MQTT_DUAL_TOPIC" -eq 1 ]; then
    BASELINE_FORWARD_COUNT=$(live_point_count || echo 0)
elif [ -f "$FORWARD_HEALTH" ]; then
    BASELINE_FORWARD_COUNT=$(python3 - "$FORWARD_HEALTH" <<'PY'
import json
import sys
try:
    with open(sys.argv[1], encoding="utf-8") as source:
        print(int(json.load(source).get("valueCount") or 0))
except Exception:
    print(0)
PY
    )
fi
[ "$BASELINE_FORWARD_COUNT" -gt 0 ] || fail "forwarder baseline is unhealthy"

mkdir -p "$BACKUP_DIR/bin" "$STATE_DIR"
systemctl list-units --type=service --state=running --no-legend --plain | \
    awk '$1 ~ /^(modbus-rtu|dlt645-driver|dio-driver|can-driver|iec-driver|compute-engine|agc-avc|ems-cluster|event-engine|system-monitor|camera-service|mqtt-driver|mqtt-forwarder)@.*\.service$/ {print $1}' \
    > "$ACTIVE_UNITS"
grep -q '^mqtt-driver@' "$ACTIVE_UNITS" || fail "main MQTT service is not active"
if [ "$SINGLE_MQTT_DUAL_TOPIC" -eq 0 ]; then
    grep -q '^mqtt-forwarder@' "$ACTIVE_UNITS" || fail "MQTT forwarder service is not active"
fi
for name in $BINARIES; do
    [ ! -f "$BIN_DIR/$name" ] || cp -p "$BIN_DIR/$name" "$BACKUP_DIR/bin/$name"
done
cp -p "$APP_CONFIG" "$STATE_DIR/mqtt-service.json"
cp -p "$EASYTIER_CONFIG" "$STATE_DIR/easytier.conf"
EASYTIER_SHA256_BEFORE=$(sha256sum "$EASYTIER_CONFIG" | awk '{print $1}')
if [ "$RESET_REALTIME_RING" -eq 1 ]; then
    REALTIME_RING_FILE=$(python3 - "$APP_CONFIG" <<'PY'
import json
import sys
config = json.load(open(sys.argv[1], encoding="utf-8"))
print(str(((config.get("mqtt") or {}).get("offlineBuffer") or {}).get("realtimeFile") or ""))
PY
    )
    case "$REALTIME_RING_FILE" in
        "$GATEWAY_HOME"/data/*) ;;
        *) fail "refusing to reset realtime ring outside gateway data directory" ;;
    esac
fi
sha256sum "$BIN_DIR"/* > "$STATE_DIR/bin-sha256.before" 2>/dev/null || true
BACKUP_READY=1

if [ "$EXTERNAL_CONTINUITY" -eq 0 ]; then
    start_continuity_publishers
fi
if [ "$PREFLIGHT_ONLY" -eq 1 ]; then
    stop_continuity_publishers
    echo "runtime upgrade preflight complete"
    echo "machineCode=$ACTUAL_MACHINE_CODE"
    echo "baselineForwardValueCount=$BASELINE_FORWARD_COUNT"
    trap - EXIT INT TERM
    exit 0
fi
UPGRADE_STARTED=1
start_ms=$(date +%s%3N)
stop_units
if [ "$RESET_REALTIME_RING" -eq 1 ]; then
    if [ -f "$REALTIME_RING_FILE" ]; then
        cp -p "$REALTIME_RING_FILE" "$STATE_DIR/realtime-ring.before"
        REALTIME_RING_BACKUP_READY=1
    fi
    rm -f "$REALTIME_RING_FILE"
fi
clear_gateway_shm
for name in $BINARIES; do
    install -m 0755 "$STAGE_DIR/$name" "$BIN_DIR/$name.new"
    mv -f "$BIN_DIR/$name.new" "$BIN_DIR/$name"
done
rm -f "$FORWARD_HEALTH"
start_units

attempt=0
MIN_RECOVERY_COUNT=$((BASELINE_FORWARD_COUNT * MIN_RECOVERY_PERCENT / 100))
[ "$MIN_RECOVERY_COUNT" -gt 0 ] || MIN_RECOVERY_COUNT=1
while [ "$attempt" -lt "$HEALTH_TIMEOUT_SEC" ]; do
    if all_units_active && all_gateway_shm_v9; then
        CURRENT_POINT_COUNT=$(live_point_count || echo 0)
        if [ "$CURRENT_POINT_COUNT" -ge "$MIN_RECOVERY_COUNT" ]; then
            break
        fi
    fi
    attempt=$((attempt + 1))
    sleep 1
done
[ "$attempt" -lt "$HEALTH_TIMEOUT_SEC" ] || fail "services or MQTT health did not recover in ${HEALTH_TIMEOUT_SEC}s"
end_ms=$(date +%s%3N)
[ "$(sha256sum "$EASYTIER_CONFIG" | awk '{print $1}')" = "$EASYTIER_SHA256_BEFORE" ] || \
    fail "EasyTier config changed during runtime upgrade"
stop_continuity_publishers
if [ "$SINGLE_MQTT_DUAL_TOPIC" -eq 0 ]; then
    rm -f "$FORWARD_HEALTH"
    attempt=0
    while [ "$attempt" -lt 35 ]; do
        [ -f "$FORWARD_HEALTH" ] && json_health_ok && break
        attempt=$((attempt + 1))
        sleep 1
    done
    [ "$attempt" -lt 35 ] || fail "new MQTT forwarder health did not recover"
fi

echo "runtime upgrade complete"
echo "machineCode=$ACTUAL_MACHINE_CODE"
echo "mqttMode=$([ "$SINGLE_MQTT_DUAL_TOPIC" -eq 1 ] && echo single-connection-dual-topic || echo isolated-forwarder)"
echo "backup=$BACKUP_DIR"
echo "baselineForwardValueCount=$BASELINE_FORWARD_COUNT"
echo "recoveredPointCount=$CURRENT_POINT_COUNT"
echo "recoveryMs=$((end_ms - start_ms))"
echo "realtimeRingReset=$RESET_REALTIME_RING"
echo "mqttOutputsRequireExternalVerification=true"
if [ "$SINGLE_MQTT_DUAL_TOPIC" -eq 0 ]; then
    cat "$FORWARD_HEALTH"
fi
trap - EXIT INT TERM
