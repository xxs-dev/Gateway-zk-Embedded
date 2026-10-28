#!/bin/sh
set -eu

DURATION_SEC=${1:-300}
INTERVAL_SEC=${2:-10}
OUTPUT_DIR=${3:-/tmp/gateway-runtime-usage-$(date +%Y%m%d-%H%M%S)}
GATEWAY_HOME=${GATEWAY_HOME:-/opt/modbus-gateway}
APP_CONFIG=${APP_CONFIG:-$GATEWAY_HOME/config/runtime/apps/mqtt-service.json}

case "$DURATION_SEC:$INTERVAL_SEC" in
    *[!0-9:]*|0:*|*:0) echo "duration and interval must be positive integers" >&2; exit 2 ;;
esac

mkdir -p "$OUTPUT_DIR"
SYSTEM_CSV="$OUTPUT_DIR/system.csv"
PROCESS_CSV="$OUTPUT_DIR/processes.csv"
SERVICE_CSV="$OUTPUT_DIR/services.csv"

printf '%s\n' \
    'timestamp,epochMs,uptimeSec,load1,load5,load15,memTotalKB,memAvailableKB,memUsedKB,rootUsedBytes,rootUsePercent,dataUsedBytes,dataUsePercent,shmCount,shmBytes,eventOutboxBytes,realtimeRingBytes,gatewayCpuPercent,gatewayRssKB,gatewayVszKB,gatewayThreads,forwardHealthy,forwardValueCount,failedUnits' \
    > "$SYSTEM_CSV"
printf '%s\n' 'timestamp,pid,process,cpuPercent,rssKB,vszKB,threads,elapsedSec' > "$PROCESS_CSV"
printf '%s\n' 'timestamp,unit,activeState,subState,mainPid,nRestarts,memoryCurrentBytes,cpuUsageNSec,tasksCurrent' > "$SERVICE_CSV"

HEALTH_FILE=$(python3 - "$APP_CONFIG" <<'PY'
import json
import os
import sys

path = sys.argv[1]
instance = os.path.splitext(os.path.basename(path))[0]
with open(path, encoding="utf-8") as source:
    config = json.load(source)
worker = ((config.get("mqttDriver") or {}).get("fullUploadWorker") or {})
health = str(worker.get("healthFile") or "/run/modbus-gateway/mqtt-primary-full-{instance}-health.json")
print(health.replace("{instance}", instance))
PY
)

gateway_units() {
    systemctl list-units --all --type=service --no-legend --plain 2>/dev/null | awk '
        $1 ~ /^(modbus-rtu|dlt645-driver|dio-driver|can-driver|iec-driver|compute-engine|agc-avc|ems-cluster|event-engine|local-display|local-display-qt|local-kiosk|system-monitor|camera-service|mqtt-driver|mqtt-forwarder)@.*\.service$/ ||
        $1 == "ky-ems.service" || $1 == "qt-display-bridge.service" ||
        $1 == "gateway-health-watchdog.service" || $1 == "gateway-cellular.service" ||
        $1 == "gateway-network-failover.service" || $1 == "easytier.service" {print $1}'
}

json_health_values() {
    python3 - "$HEALTH_FILE" <<'PY'
import json
import sys

try:
    with open(sys.argv[1], encoding="utf-8") as source:
        health = json.load(source)
    print("1" if health.get("healthy") else "0")
    print(int(health.get("valueCount") or 0))
except Exception:
    print("0")
    print("0")
PY
}

sample_system() {
    timestamp=$(date '+%Y-%m-%d %H:%M:%S%z')
    epoch_ms=$(date +%s%3N)
    uptime_sec=$(awk '{print int($1)}' /proc/uptime)
    set -- $(awk '{print $1, $2, $3}' /proc/loadavg)
    load1=$1
    load5=$2
    load15=$3
    mem_total=$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)
    mem_available=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
    mem_used=$((mem_total - mem_available))
    set -- $(df -B1 -P / | awk 'NR == 2 {gsub(/%/, "", $5); print $3, $5}')
    root_used=$1
    root_percent=$2
    set -- $(df -B1 -P "$GATEWAY_HOME/data" | awk 'NR == 2 {gsub(/%/, "", $5); print $3, $5}')
    data_used=$1
    data_percent=$2
    set -- $(find /dev/shm -maxdepth 1 -type f -name 'gateway_point_store*' -printf '%s\n' 2>/dev/null | awk '{count += 1; bytes += $1} END {print count + 0, bytes + 0}')
    shm_count=$1
    shm_bytes=$2
    outbox_bytes=$(stat -c %s "$GATEWAY_HOME/data/mqtt_event_outbox.db" 2>/dev/null || echo 0)
    realtime_file=$(python3 - "$APP_CONFIG" <<'PY'
import json
import sys
try:
    with open(sys.argv[1], encoding="utf-8") as source:
        config = json.load(source)
    print(str((((config.get("mqtt") or {}).get("offlineBuffer") or {}).get("realtimeFile")) or ""))
except Exception:
    print("")
PY
    )
    realtime_bytes=$(stat -c %s "$realtime_file" 2>/dev/null || echo 0)
    set -- $(ps -eo comm=,%cpu=,rss=,vsz=,nlwp=,args= | awk '
        index($6, "/opt/modbus-gateway/bin/") == 1 || index($6, "/opt/modbus-gateway/ky-ems/KY-EMS") == 1 {
            cpu += $2; rss += $3; vsz += $4; threads += $5
        }
        END {printf "%.1f %d %d %d\n", cpu + 0, rss + 0, vsz + 0, threads + 0}')
    gateway_cpu=$1
    gateway_rss=$2
    gateway_vsz=$3
    gateway_threads=$4
    set -- $(json_health_values)
    forward_healthy=$1
    forward_values=$2
    failed_units=$(systemctl --failed --type=service --no-legend --plain 2>/dev/null | awk 'END {print NR + 0}')
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$timestamp" "$epoch_ms" "$uptime_sec" "$load1" "$load5" "$load15" \
        "$mem_total" "$mem_available" "$mem_used" "$root_used" "$root_percent" \
        "$data_used" "$data_percent" "$shm_count" "$shm_bytes" "$outbox_bytes" \
        "$realtime_bytes" "$gateway_cpu" "$gateway_rss" "$gateway_vsz" "$gateway_threads" \
        "$forward_healthy" "$forward_values" "$failed_units" >> "$SYSTEM_CSV"
}

sample_processes() {
    timestamp=$(date '+%Y-%m-%d %H:%M:%S%z')
    ps -eo pid=,comm=,%cpu=,rss=,vsz=,nlwp=,etimes=,args= | awk -v ts="$timestamp" '
        index($8, "/opt/modbus-gateway/bin/") == 1 || index($8, "/opt/modbus-gateway/ky-ems/KY-EMS") == 1 {
            printf "%s,%s,%s,%s,%s,%s,%s,%s\n", ts, $1, $2, $3, $4, $5, $6, $7
        }' >> "$PROCESS_CSV"
}

sample_services() {
    timestamp=$(date '+%Y-%m-%d %H:%M:%S%z')
    gateway_units | while IFS= read -r unit; do
        [ -n "$unit" ] || continue
        values=$(systemctl show "$unit" \
            -p ActiveState -p SubState -p MainPID -p NRestarts \
            -p MemoryCurrent -p CPUUsageNSec -p TasksCurrent | awk -F= '
                {value[$1] = $2}
                END {
                    for (key in value) {
                        if (value[key] == "" || value[key] == "[not set]") value[key] = 0
                    }
                    print value["ActiveState"], value["SubState"], value["MainPID"],
                        value["NRestarts"], value["MemoryCurrent"], value["CPUUsageNSec"],
                        value["TasksCurrent"]
                }')
        set -- $values
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "$timestamp" "$unit" "${1:-unknown}" "${2:-unknown}" "${3:-0}" "${4:-0}" \
            "${5:-0}" "${6:-0}" "${7:-0}" >> "$SERVICE_CSV"
    done
}

started=$(date +%s)
deadline=$((started + DURATION_SEC))
while :; do
    sample_system
    sample_processes
    sample_services
    now=$(date +%s)
    [ "$now" -ge "$deadline" ] && break
    remaining=$((deadline - now))
    sleep_for=$INTERVAL_SEC
    [ "$remaining" -ge "$sleep_for" ] || sleep_for=$remaining
    sleep "$sleep_for"
done

echo "runtime usage collection complete"
echo "system=$SYSTEM_CSV"
echo "processes=$PROCESS_CSV"
echo "services=$SERVICE_CSV"
