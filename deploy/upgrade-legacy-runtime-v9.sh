#!/bin/sh
set -eu

GATEWAY_HOME=${GATEWAY_HOME:-/opt/modbus-gateway}
STAGE_DIR=${STAGE_DIR:-/tmp/gateway-runtime-v9}
HEALTH_TIMEOUT_SEC=${HEALTH_TIMEOUT_SEC:-45}
MIN_RECOVERY_PERCENT=${MIN_RECOVERY_PERCENT:-50}
SHM_RELEASE_TIMEOUT_SEC=${SHM_RELEASE_TIMEOUT_SEC:-10}
READY_STABILITY_SEC=${READY_STABILITY_SEC:-10}
PREFLIGHT_ONLY=${PREFLIGHT_ONLY:-0}
EXTERNAL_CONTINUITY=${EXTERNAL_CONTINUITY:-0}
RESET_REALTIME_RING=${RESET_REALTIME_RING:-0}
BIN_DIR="$GATEWAY_HOME/bin"
APP_CONFIG="$GATEWAY_HOME/config/runtime/apps/mqtt-service.json"
FORWARD_HEALTH=${FORWARD_HEALTH:-}
EASYTIER_CONFIG=${EASYTIER_CONFIG:-/etc/easytier/et.conf}
STAMP=$(date +%Y%m%d-%H%M%S)
BACKUP_DIR="$GATEWAY_HOME/data/runtime-backups/v9-$STAMP"
STATE_DIR="$BACKUP_DIR/state"
ACTIVE_UNITS="$STATE_DIR/active-units.txt"
ABSENT_BINARIES="$STATE_DIR/absent-binaries.txt"
BINARIES="ModbusRtu Dlt645Driver DioDriver CanDriver IecDriver MqttDriver MqttForwarder EventEngine ComputeEngine EmsClusterCoordinator AgcAvcController SystemMonitor LocalDisplay QtDisplayBridge CameraService pointctl"
WATCHDOG_RUN_DIR=${WATCHDOG_RUN_DIR:-/run/gateway-health-watchdog}
WATCHDOG_APPLYING_FILE="$WATCHDOG_RUN_DIR/applying"
WATCHDOG_APPLYING_STALE_SEC=${WATCHDOG_APPLYING_STALE_SEC:-900}
SCADA_TARGET=${SCADA_TARGET:-$GATEWAY_HOME/ky-ems/KY-EMS}
BACKUP_READY=0
UPGRADE_STARTED=0
CONTINUITY_STARTED=0
WATCHDOG_MARKER_OWNED=0
SCADA_BACKUP_READY=0
CONTINUITY_DRIVER_PID=""
REALTIME_RING_FILE=""
REALTIME_RING_BACKUP_READY=0

fail() {
    echo "runtime upgrade failed: $*" >&2
    exit 1
}

is_uint() {
    case "${1:-}" in
        ''|*[!0-9]*) return 1 ;;
        *) return 0 ;;
    esac
}

marker_field() {
    sed -n "s/^$2=//p" "$1" 2>/dev/null | sed -n '1p'
}

remove_stale_upgrade_marker() {
    [ -f "$WATCHDOG_APPLYING_FILE" ] || return 0
    marker_boot=$(marker_field "$WATCHDOG_APPLYING_FILE" boot_id)
    marker_pid=$(marker_field "$WATCHDOG_APPLYING_FILE" pid)
    marker_epoch=$(marker_field "$WATCHDOG_APPLYING_FILE" created_epoch_sec)
    marker_started=$(marker_field "$WATCHDOG_APPLYING_FILE" created_uptime_sec)
    current_boot=$(sed -n '1p' /proc/sys/kernel/random/boot_id 2>/dev/null || echo unknown)
    stale=0

    if [ -n "$marker_boot" ] && [ "$marker_boot" != "$current_boot" ]; then
        stale=1
    elif is_uint "$marker_pid"; then
        if ! kill -0 "$marker_pid" 2>/dev/null; then
            stale=1
        fi
    elif is_uint "$marker_epoch"; then
        current_epoch=$(date +%s)
        if [ "$current_epoch" -ge "$marker_epoch" ] &&
           [ $((current_epoch - marker_epoch)) -gt "$WATCHDOG_APPLYING_STALE_SEC" ]; then
            stale=1
        fi
    elif is_uint "$marker_started" && [ -r /proc/uptime ]; then
        current_uptime=$(sed -n '1{s/\..*//;p;}' /proc/uptime)
        if is_uint "$current_uptime" &&
           { [ "$current_uptime" -lt "$marker_started" ] ||
             [ $((current_uptime - marker_started)) -gt "$WATCHDOG_APPLYING_STALE_SEC" ]; }; then
            stale=1
        fi
    fi

    if [ "$stale" -eq 1 ]; then
        echo "removing stale runtime upgrade marker: $WATCHDOG_APPLYING_FILE" >&2
        rm -f "$WATCHDOG_APPLYING_FILE"
    fi
}

begin_upgrade_marker() {
    mkdir -p "$WATCHDOG_RUN_DIR"
    remove_stale_upgrade_marker
    [ ! -e "$WATCHDOG_APPLYING_FILE" ] || fail "another configuration or runtime apply is active"
    tmp="$WATCHDOG_APPLYING_FILE.tmp.$$"
    {
        echo "kind=runtime-upgrade"
        echo "pid=$$"
        if [ -r /proc/sys/kernel/random/boot_id ]; then
            echo "boot_id=$(sed -n '1p' /proc/sys/kernel/random/boot_id)"
        else
            echo "boot_id=unknown"
        fi
        echo "created_epoch_sec=$(date +%s)"
        if [ -r /proc/uptime ]; then
            echo "created_uptime_sec=$(sed -n '1{s/\..*//;p;}' /proc/uptime)"
        else
            echo "created_uptime_sec=unknown"
        fi
    } > "$tmp"
    if ! ln "$tmp" "$WATCHDOG_APPLYING_FILE" 2>/dev/null; then
        rm -f "$tmp"
        fail "another configuration or runtime apply acquired the marker"
    fi
    rm -f "$tmp"
    WATCHDOG_MARKER_OWNED=1
}

finish_upgrade_marker() {
    [ "$WATCHDOG_MARKER_OWNED" -eq 1 ] || return 0
    marker_pid=$(sed -n 's/^pid=//p' "$WATCHDOG_APPLYING_FILE" 2>/dev/null | sed -n '1p')
    [ "$marker_pid" != "$$" ] || rm -f "$WATCHDOG_APPLYING_FILE"
    WATCHDOG_MARKER_OWNED=0
}

json_health_sample() {
    python3 - "$FORWARD_HEALTH" <<'PY'
import json
import os
import sys
import time

path = sys.argv[1]
now_ms = int(time.time() * 1000)

def fail(reason):
    print("ok=0 reason=" + reason)
    raise SystemExit(1)

try:
    stat = os.stat(path)
    with open(path, encoding="utf-8") as source:
        data = json.load(source)
except FileNotFoundError:
    fail("missing")
except json.JSONDecodeError:
    fail("invalid_json")
except OSError:
    fail("io_error")

if type(data.get("healthy")) is not bool:
    fail("invalid_healthy_type")
if data["healthy"] is not True:
    fail("unhealthy")

count_value = data.get("valueCount")
if type(count_value) is not int:
    fail("invalid_value_count_type")
count = count_value
if count <= 0:
    fail("empty_value_count")

heartbeat_value = data.get("heartbeatAtMs", data.get("ts"))
if type(heartbeat_value) is not int:
    fail("invalid_heartbeat_type")
heartbeat_ms = heartbeat_value
mtime_ms = int(stat.st_mtime * 1000)
if heartbeat_ms <= 0:
    fail("missing_heartbeat")
if now_ms - heartbeat_ms > 10000 or heartbeat_ms - now_ms > 30000:
    fail("stale_heartbeat")
if now_ms - mtime_ms > 10000 or mtime_ms - now_ms > 30000:
    fail("stale_file")

lease_value = data.get("leaseUntilMs")
if lease_value is not None:
    if type(lease_value) is not int:
        fail("invalid_lease_type")
    lease_ms = lease_value
    if lease_ms < now_ms:
        fail("expired_lease")

print(
    "ok=1 generation={} valueCount={} ageMs={} fileAgeMs={}".format(
        heartbeat_ms, count, max(0, now_ms - heartbeat_ms), max(0, now_ms - mtime_ms)
    )
)
PY
}

wait_for_json_health() {
    timeout_sec=$1
    required_consecutive=${2:-3}
    started_ms=$(date +%s%3N)
    deadline_ms=$((started_ms + timeout_sec * 1000))
    consecutive=0
    last_generation=""
    audit_file="${TMPDIR:-/tmp}/gateway-forward-health-wait-$$.log"
    : > "$audit_file"
    while [ "$(date +%s%3N)" -lt "$deadline_ms" ]; do
        now_ms=$(date +%s%3N)
        if sample=$(json_health_sample 2>&1); then
            generation=$(printf '%s\n' "$sample" | sed -n 's/.* generation=\([0-9][0-9]*\).*/\1/p')
            if [ -n "$generation" ] && [ "$generation" != "$last_generation" ]; then
                consecutive=$((consecutive + 1))
                last_generation=$generation
            fi
            printf '%s consecutive=%s %s\n' "$now_ms" "$consecutive" "$sample" >> "$audit_file"
            if [ "$consecutive" -ge "$required_consecutive" ]; then
                echo "forwarder health stable after $((now_ms - started_ms))ms: $sample"
                rm -f "$audit_file"
                return 0
            fi
        else
            consecutive=0
            last_generation=""
            printf '%s consecutive=0 %s\n' "$now_ms" "$sample" >> "$audit_file"
        fi
        sleep 0.5
    done
    echo "forwarder health did not stabilize; recent samples:" >&2
    tail -n 12 "$audit_file" >&2 || true
    rm -f "$audit_file"
    return 1
}

resolve_forward_health_file() {
    instance=${APP_INSTANCE:-$(basename "$APP_CONFIG" .json)}
    python3 - "$APP_CONFIG" "$instance" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    config = json.load(source)
worker = ((config.get("mqttDriver") or {}).get("fullUploadWorker") or {})
path = str(worker.get("healthFile") or "/run/modbus-gateway/mqtt-primary-full-{instance}-health.json")
print(path.replace("{instance}", sys.argv[2]))
PY
}

resolve_event_outbox_path() {
    python3 - "$APP_CONFIG" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as source:
    config = json.load(source)
offline = ((config.get("mqtt") or {}).get("offlineBuffer") or {})
outbox = (offline.get("eventOutbox") or {})
print(str(outbox.get("sqlitePath") or "/opt/modbus-gateway/data/mqtt_event_outbox.db"))
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

unit_ready() {
    unit=$1
    case "$unit" in
        event-engine@*.service) ready_message="event engine started" ;;
        mqtt-driver@*.service) ready_message="mqtt driver started" ;;
        mqtt-forwarder@*.service) ready_message="mqtt forwarder started" ;;
        *) return 0 ;;
    esac
    pid=$(systemctl show "$unit" -p MainPID --value 2>/dev/null)
    is_uint "$pid" && [ "$pid" -gt 0 ] || return 1
    journalctl _SYSTEMD_UNIT="$unit" _PID="$pid" -n 100 --no-pager -o cat 2>/dev/null | \
        grep -Fq "$ready_message"
}

all_units_ready() {
    while IFS= read -r unit; do
        [ -z "$unit" ] && continue
        unit_ready "$unit" || return 1
    done < "$ACTIVE_UNITS"
}

mqtt_outbox_schema_ready() {
    python3 - "$EVENT_OUTBOX_PATH" <<'PY'
import sqlite3
import sys

try:
    connection = sqlite3.connect(
        "file:" + sys.argv[1] + "?mode=ro", uri=True, timeout=0.2
    )
    rows = connection.execute("PRAGMA index_list(mqtt_event_outbox)").fetchall()
    connection.close()
except Exception:
    raise SystemExit(1)
if not any(row[1] == "idx_mqtt_event_outbox_event_target" for row in rows):
    raise SystemExit(1)
PY
}

snapshot_unit_runtime() {
    output=$1
    : > "$output"
    while IFS= read -r unit; do
        [ -z "$unit" ] && continue
        pid=$(systemctl show "$unit" -p MainPID --value 2>/dev/null)
        restarts=$(systemctl show "$unit" -p NRestarts --value 2>/dev/null)
        is_uint "$pid" && is_uint "$restarts" || return 1
        printf '%s,%s,%s\n' "$unit" "$pid" "$restarts" >> "$output"
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
driver_names = list(driver.get("sharedMemoryNames") or [])
primary = str(driver.get("sharedMemoryName") or "").strip()
if primary and primary not in driver_names:
    driver_names.insert(0, primary)
if not driver_names:
    raise SystemExit("no MQTT shared memory names configured")

devices = []
names = list(driver_names)
def collect_shared_memory_names(value):
    if isinstance(value, dict):
        for item in value.values():
            collect_shared_memory_names(item)
    elif isinstance(value, list):
        for item in value:
            collect_shared_memory_names(item)
    elif isinstance(value, str) and value.startswith("gateway_point_store") and value not in names:
        names.append(value)

for device_path in config.get("deviceConfigFiles") or []:
    device = json.load(open(device_path, encoding="utf-8"))
    collect_shared_memory_names(device)
    devices.append(device)

mapping = {name: "upgrade_hold_v8_" + name for name in names}
for old_name, new_name in mapping.items():
    shutil.copyfile("/dev/shm/" + old_name.lstrip("/"), "/dev/shm/" + new_name)
default_hold_name = "upgrade_hold_v8_gateway_point_store"
if os.path.exists("/dev/shm/gateway_point_store"):
    shutil.copyfile("/dev/shm/gateway_point_store", "/dev/shm/" + default_hold_name)

device_dir = os.path.join(state_dir, "continuity-devices")
os.makedirs(device_dir, exist_ok=True)
new_device_files = []
for index, device in enumerate(devices):
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
driver["sharedMemoryNames"] = [mapping[name] for name in driver_names]
config["mqttDriver"] = driver
config.setdefault("mqtt", {})["clientId"] = str(config["mqtt"].get("clientId") or "gateway") + "-upgrade-hold"
config["mqtt"]["legacyTelemetryEnabled"] = True
for key in ("commandRequestTopic", "otaRequestTopic", "systemMonitorRequestTopic", "diagRequestTopic", "configPullRequestTopic"):
    if key in config["mqtt"]:
        config["mqtt"][key] = "upgrade/hold/disabled/" + key
config["mqtt"].setdefault("offlineBuffer", {})["enabled"] = False
config.setdefault("alarmStore", {})["enabled"] = False
forward = config.setdefault("mqttForward", {})
forward["clientId"] = str(forward.get("clientId") or "gateway-forward") + "-upgrade-hold"
forward["enabled"] = False
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
    for segment in /dev/shm/gateway_point_store*; do
        [ -e "$segment" ] || continue
        for pid in $(fuser "$segment" 2>/dev/null || true); do
            [ "$pid" != "$CONTINUITY_DRIVER_PID" ] ||
                fail "MQTT continuity driver opened live shared memory: $segment"
        done
    done
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
    waited=0
    while :; do
        busy_segments=""
        for segment in /dev/shm/gateway_point_store*; do
            [ -e "$segment" ] || continue
            if fuser "$segment" >/dev/null 2>&1; then
                busy_segments="$busy_segments $segment"
            fi
        done
        [ -z "$busy_segments" ] && break
        if [ "$waited" -ge "$SHM_RELEASE_TIMEOUT_SEC" ]; then
            echo "shared-memory release timed out after ${SHM_RELEASE_TIMEOUT_SEC}s" >&2
            for segment in $busy_segments; do
                echo "holders for $segment:" >&2
                fuser -v "$segment" >&2 || true
                for pid in $(fuser "$segment" 2>/dev/null || true); do
                    [ -r "/proc/$pid/comm" ] && printf 'pid=%s comm=%s exe=%s\n' \
                        "$pid" "$(cat "/proc/$pid/comm")" "$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)" >&2
                done
            done
            fail "shared memory still in use:$busy_segments"
        fi
        sleep 1
        waited=$((waited + 1))
    done
    rm -f /dev/shm/gateway_point_store*
}

restore_binaries() {
    for name in $BINARIES; do
        [ -f "$BACKUP_DIR/bin/$name" ] || continue
        install -m 0755 "$BACKUP_DIR/bin/$name" "$BIN_DIR/$name.restore"
        mv -f "$BIN_DIR/$name.restore" "$BIN_DIR/$name"
    done
    if [ -f "$ABSENT_BINARIES" ]; then
        while IFS= read -r name; do
            [ -z "$name" ] || rm -f "$BIN_DIR/$name"
        done < "$ABSENT_BINARIES"
    fi
    if [ "$SCADA_BACKUP_READY" -eq 1 ]; then
        install -m 0755 "$BACKUP_DIR/scada/KY-EMS" "$SCADA_TARGET.restore"
        mv -f "$SCADA_TARGET.restore" "$SCADA_TARGET"
    fi
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
    finish_upgrade_marker
    exit "$code"
}
trap rollback EXIT INT TERM

[ "$(id -u)" -eq 0 ] || fail "must run as root"
[ -f "$APP_CONFIG" ] || fail "app config missing: $APP_CONFIG"
[ -f "$EASYTIER_CONFIG" ] || fail "EasyTier config missing: $EASYTIER_CONFIG"
command -v python3 >/dev/null 2>&1 || fail "python3 is required"
command -v systemctl >/dev/null 2>&1 || fail "systemctl is required"
command -v fuser >/dev/null 2>&1 || fail "fuser is required to validate shared-memory release"
command -v journalctl >/dev/null 2>&1 || fail "journalctl is required to validate service readiness"
is_uint "$SHM_RELEASE_TIMEOUT_SEC" && [ "$SHM_RELEASE_TIMEOUT_SEC" -gt 0 ] ||
    fail "SHM_RELEASE_TIMEOUT_SEC must be a positive integer"
is_uint "$READY_STABILITY_SEC" && [ "$READY_STABILITY_SEC" -gt 0 ] ||
    fail "READY_STABILITY_SEC must be a positive integer"
is_uint "$WATCHDOG_APPLYING_STALE_SEC" && [ "$WATCHDOG_APPLYING_STALE_SEC" -gt 0 ] ||
    fail "WATCHDOG_APPLYING_STALE_SEC must be a positive integer"

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
if [ -z "$FORWARD_HEALTH" ]; then
    FORWARD_HEALTH=$(resolve_forward_health_file)
fi
EVENT_OUTBOX_PATH=$(resolve_event_outbox_path)
case "$EVENT_OUTBOX_PATH" in
    /*) ;;
    *) fail "MQTT event outbox path must be absolute" ;;
esac
case "$FORWARD_HEALTH" in
    /*) ;;
    *) fail "MQTT full-upload worker health file must be an absolute path" ;;
esac
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

BASELINE_POINT_COUNT=$(live_point_count || echo 0)
[ "$BASELINE_POINT_COUNT" -gt 0 ] || fail "shared-memory point baseline is empty"
BASELINE_FORWARD_COUNT=0
if [ -f "$FORWARD_HEALTH" ]; then
    wait_for_json_health 30 3 || fail "forwarder health baseline did not remain healthy"
    BASELINE_FORWARD_COUNT=$(python3 - "$FORWARD_HEALTH" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as source:
    print(int(json.load(source).get("valueCount") or 0))
PY
    )
fi

mkdir -p "$BACKUP_DIR/bin" "$STATE_DIR"
systemctl list-units --type=service --state=running --no-legend --plain | \
    awk '$1 ~ /^(modbus-rtu|dlt645-driver|dio-driver|can-driver|iec-driver|compute-engine|agc-avc|ems-cluster|event-engine|local-display|local-display-qt|local-kiosk|system-monitor|camera-service|mqtt-driver|mqtt-forwarder)@.*\.service$/ || $1 == "ky-ems.service" || $1 == "qt-display-bridge.service" {print $1}' \
    > "$ACTIVE_UNITS"
grep -q '^mqtt-driver@' "$ACTIVE_UNITS" || fail "main MQTT service is not active"
if [ "$SINGLE_MQTT_DUAL_TOPIC" -eq 0 ]; then
    grep -q '^mqtt-forwarder@' "$ACTIVE_UNITS" || fail "MQTT forwarder service is not active"
fi
if grep -q '^local-display-qt@' "$ACTIVE_UNITS"; then
    fail "active legacy local-display-qt unit is not supported by this runtime package"
fi
if grep -q '^ky-ems\.service$' "$ACTIVE_UNITS"; then
    exec_path=$(systemctl show ky-ems.service -p ExecStart --value | sed -n 's/^{ path=\([^ ;}]*\).*/\1/p')
    [ -n "$exec_path" ] || fail "cannot resolve active KY-EMS ExecStart path"
    [ "$(readlink -f "$exec_path")" = "$(readlink -f "$SCADA_TARGET")" ] ||
        fail "active KY-EMS ExecStart does not match SCADA_TARGET"
fi
if [ -e "$SCADA_TARGET" ]; then
    [ -x "$SCADA_TARGET" ] || fail "installed KY-EMS binary is not executable: $SCADA_TARGET"
    [ -x "$STAGE_DIR/KY-EMS" ] || fail "staged KY-EMS binary missing: $STAGE_DIR/KY-EMS"
    command -v ldd >/dev/null 2>&1 || fail "ldd is required to validate staged KY-EMS"
    if ldd "$STAGE_DIR/KY-EMS" 2>&1 | grep -q 'not found'; then
        fail "staged KY-EMS has unresolved runtime dependencies"
    fi
fi
: > "$ABSENT_BINARIES"
for name in $BINARIES; do
    if [ -f "$BIN_DIR/$name" ]; then
        cp -p "$BIN_DIR/$name" "$BACKUP_DIR/bin/$name"
    else
        echo "$name" >> "$ABSENT_BINARIES"
    fi
done
if [ -e "$SCADA_TARGET" ]; then
    mkdir -p "$BACKUP_DIR/scada"
    cp -p "$SCADA_TARGET" "$BACKUP_DIR/scada/KY-EMS"
    SCADA_BACKUP_READY=1
fi
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
    echo "baselinePointCount=$BASELINE_POINT_COUNT"
    echo "baselineForwardValueCount=$BASELINE_FORWARD_COUNT"
    trap - EXIT INT TERM
    exit 0
fi
begin_upgrade_marker
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
if [ "$SCADA_BACKUP_READY" -eq 1 ]; then
    install -m 0755 "$STAGE_DIR/KY-EMS" "$SCADA_TARGET.new"
    mv -f "$SCADA_TARGET.new" "$SCADA_TARGET"
fi
rm -f "$FORWARD_HEALTH"
start_units

attempt=0
MIN_RECOVERY_COUNT=$((BASELINE_POINT_COUNT * MIN_RECOVERY_PERCENT / 100))
[ "$MIN_RECOVERY_COUNT" -gt 0 ] || MIN_RECOVERY_COUNT=1
while [ "$attempt" -lt "$HEALTH_TIMEOUT_SEC" ]; do
    if all_units_active && all_units_ready && mqtt_outbox_schema_ready && all_gateway_shm_v9; then
        CURRENT_POINT_COUNT=$(live_point_count || echo 0)
        if [ "$CURRENT_POINT_COUNT" -ge "$MIN_RECOVERY_COUNT" ]; then
            break
        fi
    fi
    attempt=$((attempt + 1))
    sleep 1
done
[ "$attempt" -lt "$HEALTH_TIMEOUT_SEC" ] || fail "services or MQTT health did not recover in ${HEALTH_TIMEOUT_SEC}s"
READY_RUNTIME_BEFORE="$STATE_DIR/ready-runtime-before.csv"
READY_RUNTIME_AFTER="$STATE_DIR/ready-runtime-after.csv"
snapshot_unit_runtime "$READY_RUNTIME_BEFORE" || fail "cannot record ready service runtime state"
sleep "$READY_STABILITY_SEC"
all_units_active && all_units_ready && mqtt_outbox_schema_ready || \
    fail "a service stopped, lost readiness, or lost the committed outbox schema during the stability window"
snapshot_unit_runtime "$READY_RUNTIME_AFTER" || fail "cannot record stable service runtime state"
cmp -s "$READY_RUNTIME_BEFORE" "$READY_RUNTIME_AFTER" || \
    fail "a service restarted during the ${READY_STABILITY_SEC}s stability window"
end_ms=$(date +%s%3N)
[ "$(sha256sum "$EASYTIER_CONFIG" | awk '{print $1}')" = "$EASYTIER_SHA256_BEFORE" ] || \
    fail "EasyTier config changed during runtime upgrade"
stop_continuity_publishers
if [ "$SINGLE_MQTT_DUAL_TOPIC" -eq 0 ]; then
    rm -f "$FORWARD_HEALTH"
    wait_for_json_health 35 3 || fail "new MQTT forwarder health did not remain healthy"
fi

echo "runtime upgrade complete"
echo "machineCode=$ACTUAL_MACHINE_CODE"
echo "mqttMode=$([ "$SINGLE_MQTT_DUAL_TOPIC" -eq 1 ] && echo single-connection-dual-topic || echo isolated-forwarder)"
echo "backup=$BACKUP_DIR"
echo "baselinePointCount=$BASELINE_POINT_COUNT"
echo "baselineForwardValueCount=$BASELINE_FORWARD_COUNT"
echo "recoveredPointCount=$CURRENT_POINT_COUNT"
echo "recoveryMs=$((end_ms - start_ms))"
echo "readyStabilitySec=$READY_STABILITY_SEC"
echo "realtimeRingReset=$RESET_REALTIME_RING"
echo "mqttOutputsRequireExternalVerification=true"
if [ "$SINGLE_MQTT_DUAL_TOPIC" -eq 0 ]; then
    cat "$FORWARD_HEALTH"
fi
UPGRADE_STARTED=0
finish_upgrade_marker
trap - EXIT INT TERM
