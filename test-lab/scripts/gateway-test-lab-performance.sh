#!/bin/sh
set -eu

LC_ALL=C
export LC_ALL

ROOT=${GATEWAY_TEST_LAB_ROOT:-/opt/modbus-gateway/test-lab}
LAB_COMMAND=${GATEWAY_TEST_LAB_COMMAND:-$ROOT/bin/gateway-test-lab}
GATEWAY_BIN_DIR=${GATEWAY_BIN_DIR:-$ROOT/bin}

protocol=modbus-tcp
scenario=normal
mode=virtual
host=127.0.0.1
port=
serial_device=auto
can_interface=gatewaytest0
can_driver_udp_port=19011
can_simulator_udp_port=19012
monitor_port=19443
mqtt_broker=
warmup_sec=60
duration_sec=600
sample_interval_sec=1
output_dir=
expected_frame_rate=0
min_protocol_success_pct=99.9
min_schedule_achievement_pct=95
min_quality_good_pct=99.9
max_freshness_p95_ms=1500
max_compute_scan_utilization_pct=70
max_mqtt_scan_utilization_pct=70
min_mqtt_full_achievement_pct=95
max_cpu_p95_pct=70
max_rss_growth_kb=10240
keep_running=0
timing_operation_count=0
timing_registers_per_request=0
timing_retry_count=
timing_device_response_ms=
timing_parse_store_ms=1
timing_network_rtt_ms=2
timing_socket_queue_ms=0
timing_link_mbps=100
timing_turnaround_ms=1
timing_request_bytes=0
timing_response_bytes=0
timing_can_bitrate=500000
timing_can_payload_bytes=8
timing_debounce_ms=0
timing_io_operation_ms=0.05

usage() {
    cat <<'EOF'
Usage: gateway-test-lab performance [options]
  --protocol modbus-tcp|modbus-rtu|dlt645|can|dio|iec104
  --scenario normal|ramp|random|boundary
  --mode virtual|hil
  --host IP --port N --serial-device DEVICE --can-interface NAME
  --warmup-sec N --duration-sec N --sample-interval-sec N
  --output-dir DIR
  --mqtt-broker URL
  --expected-frame-rate N
  --min-protocol-success-pct N
  --min-schedule-achievement-pct N
  --min-quality-good-pct N
  --max-freshness-p95-ms N
  --max-compute-scan-utilization-pct N
  --max-mqtt-scan-utilization-pct N
  --min-mqtt-full-achievement-pct N
  --max-cpu-p95-pct N
  --max-rss-growth-kb N
  --timing-operation-count N
  --timing-registers-per-request N
  --timing-retry-count N
  --timing-device-response-ms N --timing-parse-store-ms N
  --timing-network-rtt-ms N --timing-socket-queue-ms N --timing-link-mbps N
  --timing-turnaround-ms N --timing-request-bytes N --timing-response-bytes N
  --timing-can-bitrate N --timing-can-payload-bytes N
  --timing-debounce-ms N --timing-io-operation-ms N
  --keep-running
EOF
}

require_uint() {
    name=$1
    value=$2
    case "$value" in ''|*[!0-9]*) echo "$name must be a non-negative integer" >&2; exit 2 ;; esac
}

require_nonnegative_number() {
    name=$1
    value=$2
    if ! awk -v value="$value" 'BEGIN { exit !(value ~ /^[0-9]+([.][0-9]+)?$/) }'; then
        echo "$name must be a non-negative number" >&2
        exit 2
    fi
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --protocol) protocol=$2; shift 2 ;;
        --scenario) scenario=$2; shift 2 ;;
        --mode) mode=$2; shift 2 ;;
        --host) host=$2; shift 2 ;;
        --port) port=$2; shift 2 ;;
        --serial-device) serial_device=$2; shift 2 ;;
        --can-interface) can_interface=$2; shift 2 ;;
        --can-driver-udp-port) can_driver_udp_port=$2; shift 2 ;;
        --can-simulator-udp-port) can_simulator_udp_port=$2; shift 2 ;;
        --monitor-port) monitor_port=$2; shift 2 ;;
        --mqtt-broker) mqtt_broker=$2; shift 2 ;;
        --warmup-sec) warmup_sec=$2; shift 2 ;;
        --duration-sec) duration_sec=$2; shift 2 ;;
        --sample-interval-sec) sample_interval_sec=$2; shift 2 ;;
        --output-dir) output_dir=$2; shift 2 ;;
        --expected-frame-rate) expected_frame_rate=$2; shift 2 ;;
        --min-protocol-success-pct) min_protocol_success_pct=$2; shift 2 ;;
        --min-schedule-achievement-pct) min_schedule_achievement_pct=$2; shift 2 ;;
        --min-quality-good-pct) min_quality_good_pct=$2; shift 2 ;;
        --max-freshness-p95-ms) max_freshness_p95_ms=$2; shift 2 ;;
        --max-compute-scan-utilization-pct) max_compute_scan_utilization_pct=$2; shift 2 ;;
        --max-mqtt-scan-utilization-pct) max_mqtt_scan_utilization_pct=$2; shift 2 ;;
        --min-mqtt-full-achievement-pct) min_mqtt_full_achievement_pct=$2; shift 2 ;;
        --max-cpu-p95-pct) max_cpu_p95_pct=$2; shift 2 ;;
        --max-rss-growth-kb) max_rss_growth_kb=$2; shift 2 ;;
        --timing-operation-count) timing_operation_count=$2; shift 2 ;;
        --timing-registers-per-request) timing_registers_per_request=$2; shift 2 ;;
        --timing-retry-count) timing_retry_count=$2; shift 2 ;;
        --timing-device-response-ms) timing_device_response_ms=$2; shift 2 ;;
        --timing-parse-store-ms) timing_parse_store_ms=$2; shift 2 ;;
        --timing-network-rtt-ms) timing_network_rtt_ms=$2; shift 2 ;;
        --timing-socket-queue-ms) timing_socket_queue_ms=$2; shift 2 ;;
        --timing-link-mbps) timing_link_mbps=$2; shift 2 ;;
        --timing-turnaround-ms) timing_turnaround_ms=$2; shift 2 ;;
        --timing-request-bytes) timing_request_bytes=$2; shift 2 ;;
        --timing-response-bytes) timing_response_bytes=$2; shift 2 ;;
        --timing-can-bitrate) timing_can_bitrate=$2; shift 2 ;;
        --timing-can-payload-bytes) timing_can_payload_bytes=$2; shift 2 ;;
        --timing-debounce-ms) timing_debounce_ms=$2; shift 2 ;;
        --timing-io-operation-ms) timing_io_operation_ms=$2; shift 2 ;;
        --keep-running) keep_running=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown performance option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

case "$protocol" in modbus-tcp|modbus-rtu|dlt645|can|dio|iec104) ;; *) echo "invalid protocol: $protocol" >&2; exit 2 ;; esac
case "$scenario" in normal|ramp|random|boundary) ;; *) echo "invalid scenario: $scenario" >&2; exit 2 ;; esac
case "$mode" in virtual|hil) ;; *) echo "invalid mode: $mode" >&2; exit 2 ;; esac
require_uint --warmup-sec "$warmup_sec"
require_uint --duration-sec "$duration_sec"
require_uint --sample-interval-sec "$sample_interval_sec"
require_nonnegative_number --expected-frame-rate "$expected_frame_rate"
require_nonnegative_number --min-protocol-success-pct "$min_protocol_success_pct"
require_nonnegative_number --min-schedule-achievement-pct "$min_schedule_achievement_pct"
require_nonnegative_number --min-quality-good-pct "$min_quality_good_pct"
require_nonnegative_number --max-freshness-p95-ms "$max_freshness_p95_ms"
require_nonnegative_number --max-compute-scan-utilization-pct "$max_compute_scan_utilization_pct"
require_nonnegative_number --max-mqtt-scan-utilization-pct "$max_mqtt_scan_utilization_pct"
require_nonnegative_number --min-mqtt-full-achievement-pct "$min_mqtt_full_achievement_pct"
require_nonnegative_number --max-cpu-p95-pct "$max_cpu_p95_pct"
require_nonnegative_number --max-rss-growth-kb "$max_rss_growth_kb"
require_uint --timing-operation-count "$timing_operation_count"
require_nonnegative_number --timing-registers-per-request "$timing_registers_per_request"
[ -z "$timing_retry_count" ] || require_uint --timing-retry-count "$timing_retry_count"
[ -z "$timing_device_response_ms" ] || require_nonnegative_number --timing-device-response-ms "$timing_device_response_ms"
require_nonnegative_number --timing-parse-store-ms "$timing_parse_store_ms"
require_nonnegative_number --timing-network-rtt-ms "$timing_network_rtt_ms"
require_nonnegative_number --timing-socket-queue-ms "$timing_socket_queue_ms"
require_nonnegative_number --timing-link-mbps "$timing_link_mbps"
require_nonnegative_number --timing-turnaround-ms "$timing_turnaround_ms"
require_uint --timing-request-bytes "$timing_request_bytes"
require_uint --timing-response-bytes "$timing_response_bytes"
require_uint --timing-can-bitrate "$timing_can_bitrate"
require_uint --timing-can-payload-bytes "$timing_can_payload_bytes"
require_nonnegative_number --timing-debounce-ms "$timing_debounce_ms"
require_nonnegative_number --timing-io-operation-ms "$timing_io_operation_ms"
[ "$duration_sec" -gt 0 ] || { echo "--duration-sec must be greater than zero" >&2; exit 2; }
[ "$sample_interval_sec" -gt 0 ] || { echo "--sample-interval-sec must be greater than zero" >&2; exit 2; }
[ "$timing_link_mbps" != "0" ] || { echo "--timing-link-mbps must be greater than zero" >&2; exit 2; }
[ "$timing_can_bitrate" -gt 0 ] || { echo "--timing-can-bitrate must be greater than zero" >&2; exit 2; }
[ "$timing_can_payload_bytes" -le 64 ] || { echo "--timing-can-payload-bytes must be <= 64" >&2; exit 2; }

if [ -z "$port" ]; then
    if [ "$protocol" = "iec104" ]; then port=12404; else port=15020; fi
fi
if [ -z "$output_dir" ]; then
    output_dir=$ROOT/reports/performance-$(date +%Y%m%d-%H%M%S)
fi

POINTCTL=
for candidate in "$ROOT/bin/pointctl" "$GATEWAY_BIN_DIR/pointctl" /opt/modbus-gateway/bin/pointctl; do
    if [ -x "$candidate" ]; then POINTCTL=$candidate; break; fi
done
[ -n "$POINTCTL" ] || { echo "pointctl not found; build or install pointctl before performance testing" >&2; exit 1; }
[ -x "$LAB_COMMAND" ] || { echo "gateway-test-lab command not found: $LAB_COMMAND" >&2; exit 1; }
page_size=$(getconf PAGESIZE 2>/dev/null || printf '4096')
page_kb=$((page_size / 1024))

RAW=$output_dir/raw
mkdir -p "$RAW" "$output_dir/config"
: > "$RAW/resource-samples.tsv"
: > "$RAW/point-observations.tsv"
: > "$RAW/pointctl-errors.log"
: > "$RAW/trace-shared-memory.tsv"
: > "$RAW/trace-mqtt-publisher.tsv"
case "$protocol" in
    modbus-tcp) trace_evidence_index=910099 ;;
    modbus-rtu) trace_evidence_index=940099 ;;
    *) trace_evidence_index=0 ;;
esac
APP_CONFIG=$ROOT/config/apps/monitor-service.json
STATUS_FILE=$ROOT/run/simulator-status.json
COMPUTE_HEALTH=$ROOT/run/compute-engine-health.json
MQTT_HEALTH=$ROOT/run/mqtt-driver-health.json
MQTT_LOG=$ROOT/logs/mqtt-driver.log

started=0
cleanup() {
    result=$?
    trap - EXIT INT TERM
    if [ "$started" -eq 1 ] && [ "$keep_running" -eq 0 ]; then
        "$LAB_COMMAND" stop >/dev/null 2>&1 || true
    fi
    exit "$result"
}
trap cleanup EXIT INT TERM

copy_or_empty_json() {
    source_file=$1
    target_file=$2
    if [ -s "$source_file" ]; then cp "$source_file" "$target_file"; else printf '{}\n' > "$target_file"; fi
}

json_number() {
    file=$1
    key=$2
    fallback=${3:-0}
    value=$(sed -n "s/.*\"$key\":[[:space:]]*\([-+0-9.eE][+0-9.eE-]*\).*/\1/p" "$file" 2>/dev/null | head -n 1)
    if [ -n "$value" ]; then printf '%s\n' "$value"; else printf '%s\n' "$fallback"; fi
}

json_string() {
    file=$1
    key=$2
    fallback=${3:-}
    value=$(sed -n "s/.*\"$key\": *\"\([^\"]*\)\".*/\1/p" "$file" 2>/dev/null | head -n 1)
    if [ -n "$value" ]; then printf '%s\n' "$value"; else printf '%s\n' "$fallback"; fi
}

stat_sum() {
    file=$1
    key=$2
    awk -v key="$key" '
        { for (i = 1; i <= NF; ++i) if (index($i, key "=") == 1) { split($i, a, "="); sum += a[2] } }
        END { printf "%.0f\n", sum + 0 }
    ' "$file" 2>/dev/null || printf '0\n'
}

nonnegative_delta() {
    awk -v start="$1" -v finish="$2" 'BEGIN { value=finish-start; if(value<0)value=0; printf "%.0f\n", value }'
}

ratio_pct() {
    awk -v numerator="$1" -v denominator="$2" 'BEGIN { if(denominator<=0){print "0.00"}else{printf "%.2f\n", numerator*100/denominator} }'
}

float_ge() { awk -v actual="$1" -v expected="$2" 'BEGIN { exit !(actual+0 >= expected+0) }'; }
float_le() { awk -v actual="$1" -v expected="$2" 'BEGIN { exit !(actual+0 <= expected+0) }'; }

calculate_timing_estimate() {
    device_config=$1
    configured_cycle_ms=$(json_number "$device_config" defaultIntervalMs 1000)
    timing_estimation_source=test-lab-template-defaults
    operation_count=$timing_operation_count
    registers_per_request=$timing_registers_per_request
    retry_count=$timing_retry_count
    device_response_ms=$timing_device_response_ms
    request_bytes=$timing_request_bytes
    response_bytes=$timing_response_bytes
    wakeup_bytes=0
    baud_rate=$(json_number "$device_config" baudRate 9600)
    data_bits=$(json_number "$device_config" dataBits 8)
    stop_bits=$(json_number "$device_config" stopBits 1)
    parity=$(json_string "$device_config" parity N)
    frame_interval_ms=$(json_number "$device_config" frameIntervalMs 0)
    [ -n "$retry_count" ] || retry_count=$(json_number "$device_config" readRetryCount 0)

    case "$protocol" in
        modbus-tcp)
            timing_transport=ethernet
            timing_model=tcp_request_response
            [ "$operation_count" -gt 0 ] || operation_count=10
            if awk -v value="$registers_per_request" 'BEGIN{exit !(value<=0)}'; then registers_per_request=1.3; fi
            [ -n "$device_response_ms" ] || device_response_ms=3
            [ "$request_bytes" -gt 0 ] || request_bytes=12
            if [ "$response_bytes" -eq 0 ]; then
                response_bytes=$(awk -v registers="$registers_per_request" 'BEGIN{printf "%.3f", 9+2*registers}')
            fi
            operation_time_ms=$(awk -v queue="$timing_socket_queue_ms" -v rtt="$timing_network_rtt_ms" \
                -v device="$device_response_ms" -v request="$request_bytes" -v response="$response_bytes" \
                -v mbps="$timing_link_mbps" -v parse="$timing_parse_store_ms" \
                'BEGIN{printf "%.3f", queue+rtt+device+((request+response)*8/(mbps*1000000)*1000)+parse}')
            ;;
        modbus-rtu)
            timing_transport=rs485
            timing_model=serial_request_response
            [ "$operation_count" -gt 0 ] || operation_count=9
            if awk -v value="$registers_per_request" 'BEGIN{exit !(value<=0)}'; then registers_per_request=1; fi
            [ -n "$device_response_ms" ] || device_response_ms=2
            request_bytes=8
            response_bytes=$(awk -v registers="$registers_per_request" 'BEGIN{printf "%.3f", 5+2*registers}')
            ;;
        dlt645)
            timing_transport=rs485
            timing_model=serial_request_response
            [ "$operation_count" -gt 0 ] || operation_count=8
            [ -n "$device_response_ms" ] || device_response_ms=20
            [ "$request_bytes" -gt 0 ] || request_bytes=16
            [ "$response_bytes" -gt 0 ] || response_bytes=24
            wakeup_bytes=$(json_number "$device_config" wakeupBytes 0)
            ;;
        iec104)
            timing_transport=ethernet
            timing_model=tcp_request_response
            [ "$operation_count" -gt 0 ] || operation_count=1
            [ -n "$device_response_ms" ] || device_response_ms=5
            [ "$request_bytes" -gt 0 ] || request_bytes=16
            [ "$response_bytes" -gt 0 ] || response_bytes=64
            operation_time_ms=$(awk -v queue="$timing_socket_queue_ms" -v rtt="$timing_network_rtt_ms" \
                -v device="$device_response_ms" -v request="$request_bytes" -v response="$response_bytes" \
                -v mbps="$timing_link_mbps" -v parse="$timing_parse_store_ms" \
                'BEGIN{printf "%.3f", queue+rtt+device+((request+response)*8/(mbps*1000000)*1000)+parse}')
            ;;
        can)
            if [ "$mode" = virtual ]; then
                timing_transport=udp_test
                timing_model=udp_virtual_can
                [ "$operation_count" -gt 0 ] || operation_count=1
                [ -n "$device_response_ms" ] || device_response_ms=0
                operation_time_ms=$(awk -v rtt="$timing_network_rtt_ms" -v queue="$timing_socket_queue_ms" \
                    -v mbps="$timing_link_mbps" -v parse="$timing_parse_store_ms" \
                    'BEGIN{printf "%.3f", rtt/2+queue+(20*8/(mbps*1000000)*1000)+parse}')
            else
                timing_transport=can
                timing_model=can_bus
                [ "$operation_count" -gt 0 ] || operation_count=1
                [ -n "$device_response_ms" ] || device_response_ms=0
                operation_time_ms=$(awk -v payload="$timing_can_payload_bytes" -v bitrate="$timing_can_bitrate" \
                    'BEGIN{printf "%.3f", ((67+payload*8+3)*1.20/bitrate*1000)}')
            fi
            retry_count=0
            ;;
        dio)
            timing_transport=gpio
            timing_model=gpio_scan
            [ "$operation_count" -gt 0 ] || operation_count=4
            [ -n "$device_response_ms" ] || device_response_ms=0
            operation_time_ms=$timing_io_operation_ms
            retry_count=0
            ;;
    esac

    if [ "$timing_model" = serial_request_response ]; then
        parity_bits=0
        [ "$parity" = N ] || parity_bits=1
        character_ms=$(awk -v baud="$baud_rate" -v data="$data_bits" -v parity="$parity_bits" -v stop="$stop_bits" \
            'BEGIN{printf "%.6f", 1000*(1+data+parity+stop)/baud}')
        protocol_gap_ms=0
        [ "$protocol" != modbus-rtu ] || protocol_gap_ms=$(awk -v char="$character_ms" 'BEGIN{printf "%.6f", 3.5*char}')
        effective_gap_ms=$(awk -v protocol="$protocol_gap_ms" -v configured="$frame_interval_ms" \
            'BEGIN{if(configured>protocol)printf "%.6f", configured;else printf "%.6f", protocol}')
        operation_time_ms=$(awk -v wakeup="$wakeup_bytes" -v request="$request_bytes" -v response="$response_bytes" \
            -v char="$character_ms" -v gap="$effective_gap_ms" -v device="$device_response_ms" \
            -v turnaround="$timing_turnaround_ms" -v parse="$timing_parse_store_ms" \
            'BEGIN{printf "%.3f", (wakeup+request+response)*char+2*gap+device+turnaround+parse}')
    fi

    estimated_minimum_cycle_ms=$(awk -v operation="$operation_time_ms" -v count="$operation_count" \
        'BEGIN{printf "%.3f", operation*count}')
    worst_case_cycle_ms=$(awk -v cycle="$estimated_minimum_cycle_ms" -v retry="$retry_count" \
        -v model="$timing_model" 'BEGIN{if(model=="serial_request_response"||model=="tcp_request_response")printf "%.3f", cycle*(1+retry);else printf "%.3f", cycle}')
    link_utilization_pct=$(ratio_pct "$estimated_minimum_cycle_ms" "$configured_cycle_ms")
    worst_link_utilization_pct=$(ratio_pct "$worst_case_cycle_ms" "$configured_cycle_ms")
    recommended_cycle_ms=$(awk -v cycle="$estimated_minimum_cycle_ms" 'BEGIN{printf "%.0f", cycle/0.70+0.999999}')
    if float_ge "$link_utilization_pct" 90; then
        timing_verdict=fail
        timing_verdict_zh=不可稳定运行
    elif float_ge "$link_utilization_pct" 70 || float_ge "$worst_link_utilization_pct" 100; then
        timing_verdict=risk
        timing_verdict_zh=有风险
    else
        timing_verdict=pass
        timing_verdict_zh=可进入实测
    fi

    if [ "$timing_operation_count" -gt 0 ] || [ "$timing_registers_per_request" != 0 ] || \
        [ -n "$timing_retry_count" ] || [ -n "$timing_device_response_ms" ]; then
        timing_estimation_source=command-line-overrides
    fi
}

now_ms() {
    value=$(date +%s%N)
    case "$value" in
        *N*) printf '%s000\n' "$(date +%s)" ;;
        *) printf '%.13s\n' "$value" ;;
    esac
}

numeric_stats() {
    file=$1
    if [ ! -s "$file" ]; then
        printf '0 0.00 0.00 0.00 0.00 0.00\n'
        return
    fi
    sort -n "$file" | awk '
        { values[NR]=$1; sum+=$1 }
        END {
            if (NR == 0) { print "0 0.00 0.00 0.00 0.00 0.00"; exit }
            p50=int(NR*0.50+0.999999); p95=int(NR*0.95+0.999999); p99=int(NR*0.99+0.999999)
            printf "%d %.2f %.2f %.2f %.2f %.2f\n", NR, sum/NR, values[p50], values[p95], values[p99], values[NR]
        }
    '
}

point_value() {
    file=$1
    target_index=$2
    field=$3
    awk -v target="$target_index" -v field="$field" '
        {
            indexValue=""
            fieldValue=""
            for(i=1;i<=NF;++i){
                split($i,a,"=")
                if(a[1]=="index") indexValue=a[2]
                if(a[1]==field) fieldValue=a[2]
            }
            if(indexValue==target){print fieldValue; exit}
        }
    ' "$file"
}

process_is_alive() {
    pid_file=$1
    [ -s "$pid_file" ] || return 1
    pid=$(cat "$pid_file" 2>/dev/null || true)
    [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null
}

sample_resources() {
    timestamp=$(now_ms)
    total_jiffies=$(awk '/^cpu / { total=0; for(i=2;i<=NF;++i) total+=$i; printf "%.0f", total; exit }' /proc/stat)
    process_jiffies=0
    rss_kb=0
    fd_count=0
    thread_count=0
    for pid_file in "$ROOT"/run/simulator.pid "$ROOT"/run/modbus-driver.pid \
        "$ROOT"/run/compute-engine.pid "$ROOT"/run/event-engine.pid \
        "$ROOT"/run/mqtt-driver.pid "$ROOT"/run/system-monitor.pid; do
        process_is_alive "$pid_file" || continue
        pid=$(cat "$pid_file")
        stat_line=$(cat "/proc/$pid/stat" 2>/dev/null || true)
        [ -n "$stat_line" ] || continue
        stat_rest=${stat_line#*) }
        values=$(printf '%s\n' "$stat_rest" | awk '{ printf "%.0f %.0f %.0f\n", $12+$13, $22, $18 }')
        set -- $values
        process_jiffies=$((process_jiffies + $1))
        rss_kb=$((rss_kb + ($2 * page_kb)))
        thread_count=$((thread_count + $3))
        current_fd=$(find "/proc/$pid/fd" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l | tr -d ' ')
        fd_count=$((fd_count + current_fd))
    done
    printf '%s %s %s %s %s %s\n' "$timestamp" "$total_jiffies" "$process_jiffies" \
        "$rss_kb" "$fd_count" "$thread_count" >> "$RAW/resource-samples.tsv"
}

sample_points() {
    current=$RAW/point-snapshot-current.txt
    if "$POINTCTL" snapshot --app-config "$APP_CONFIG" > "$current" 2>> "$RAW/pointctl-errors.log"; then
        observed=$(now_ms)
        awk -v observed="$observed" '
            {
                ts=0; quality=-1; stale=1
                for(i=1;i<=NF;++i){ split($i,a,"="); if(a[1]=="ts")ts=a[2]; if(a[1]=="quality")quality=a[2]; if(a[1]=="stale")stale=a[2] }
                if(ts>0 && observed>=ts) printf "%.0f %d %d\n", observed-ts, quality, stale
            }
        ' "$current" >> "$RAW/point-observations.tsv"
        if [ "$trace_evidence_index" -gt 0 ]; then
            awk -v target="$trace_evidence_index" -v observed="$observed" '
                {
                    indexValue=""; pointValue=""; pointTs=""; quality=""
                    for(i=1;i<=NF;++i){
                        split($i,a,"=")
                        if(a[1]=="index")indexValue=a[2]
                        if(a[1]=="value")pointValue=a[2]
                        if(a[1]=="ts")pointTs=a[2]
                        if(a[1]=="quality")quality=a[2]
                    }
                    if(indexValue==target && quality==1 && pointTs>0){
                        printf "%.0f\t%.0f\t%.0f\n", pointValue, pointTs, observed
                    }
                }
            ' "$current" >> "$RAW/trace-shared-memory.tsv"
        fi
    fi
}

capture_stats() {
    suffix=$1
    copy_or_empty_json "$STATUS_FILE" "$RAW/simulator-$suffix.json"
    "$POINTCTL" stats --app-config "$APP_CONFIG" > "$RAW/point-store-$suffix.txt"
    copy_or_empty_json "$COMPUTE_HEALTH" "$RAW/compute-$suffix.json"
    copy_or_empty_json "$MQTT_HEALTH" "$RAW/mqtt-$suffix.json"
    if [ -f "$MQTT_LOG" ]; then grep -c '^mqtt full ' "$MQTT_LOG" > "$RAW/mqtt-full-$suffix.count" || printf '0\n' > "$RAW/mqtt-full-$suffix.count"; else printf '0\n' > "$RAW/mqtt-full-$suffix.count"; fi
}

if [ -n "$mqtt_broker" ]; then
    "$LAB_COMMAND" start "$scenario" --protocol "$protocol" --mode "$mode" --host "$host" --port "$port" \
        --serial-device "$serial_device" --can-interface "$can_interface" \
        --can-driver-udp-port "$can_driver_udp_port" --can-simulator-udp-port "$can_simulator_udp_port" \
        --monitor-host 127.0.0.1 --monitor-port "$monitor_port" --mqtt-broker "$mqtt_broker" \
        > "$RAW/start.log"
else
    "$LAB_COMMAND" start "$scenario" --protocol "$protocol" --mode "$mode" --host "$host" --port "$port" \
        --serial-device "$serial_device" --can-interface "$can_interface" \
        --can-driver-udp-port "$can_driver_udp_port" --can-simulator-udp-port "$can_simulator_udp_port" \
        --monitor-host 127.0.0.1 --monitor-port "$monitor_port" > "$RAW/start.log"
fi
started=1

case "$protocol" in
    modbus-tcp) timing_device_config=$ROOT/config/devices/device_modbus_tcp.json ;;
    modbus-rtu) timing_device_config=$ROOT/config/devices/device_modbus_rtu.json ;;
    dlt645) timing_device_config=$ROOT/config/devices/device_dlt645.json ;;
    can) timing_device_config=$ROOT/config/devices/device_can.json ;;
    dio) timing_device_config=$ROOT/config/devices/device_dio.json ;;
    iec104) timing_device_config=$ROOT/config/devices/device_iec104.json ;;
esac
calculate_timing_estimate "$timing_device_config"
if [ "$mode" = hil ]; then timing_physical_mode=true; else timing_physical_mode=false; fi
cat > "$RAW/timing-estimate.json" <<EOF
{
  "protocol":"$protocol","transport":"$timing_transport","model":"$timing_model",
  "configuredCycleMs":$configured_cycle_ms,"operationCount":$operation_count,
  "operationTimeMs":$operation_time_ms,"estimatedMinimumCycleMs":$estimated_minimum_cycle_ms,
  "worstCaseCycleMs":$worst_case_cycle_ms,"linkUtilizationPercent":$link_utilization_pct,
  "worstCaseUtilizationPercent":$worst_link_utilization_pct,"recommendedCycleMs":$recommended_cycle_ms,
  "verdict":"$timing_verdict","estimationSource":"$timing_estimation_source","physicalMediaTestMode":$timing_physical_mode
}
EOF

printf 'performance test warming up: %ss\n' "$warmup_sec"
[ "$warmup_sec" -eq 0 ] || sleep "$warmup_sec"
cp -R "$ROOT/config/." "$output_dir/config/"
uname -a > "$RAW/uname.txt"
cat /proc/cpuinfo > "$RAW/cpuinfo.txt"
cat /proc/meminfo > "$RAW/meminfo-start.txt"
if command -v sha256sum >/dev/null 2>&1; then sha256sum "$ROOT"/bin/* > "$RAW/SHA256SUMS.txt" 2>/dev/null || true; fi
capture_stats start
measurement_start_ms=$(now_ms)

printf 'performance test measuring: %ss\n' "$duration_sec"
elapsed=0
while [ "$elapsed" -lt "$duration_sec" ]; do
    sample_resources
    sample_points
    sleep_for=$sample_interval_sec
    remaining=$((duration_sec - elapsed))
    [ "$sleep_for" -le "$remaining" ] || sleep_for=$remaining
    sleep "$sleep_for"
    elapsed=$((elapsed + sleep_for))
done
sample_resources
sample_points

capture_stats end
measurement_end_ms=$(now_ms)
actual_duration_ms=$((measurement_end_ms - measurement_start_ms))
actual_duration_sec=$(awk -v duration="$actual_duration_ms" 'BEGIN{printf "%.3f", duration/1000}')
cp "$RAW/point-snapshot-current.txt" "$RAW/point-snapshot-measurement-end.txt" 2>/dev/null || true
cp "$RAW/point-snapshot-current.txt" "$RAW/point-snapshot-final.txt" 2>/dev/null || true
cat /proc/meminfo > "$RAW/meminfo-end.txt"
curl --noproxy '*' --max-time 5 -fsS "http://127.0.0.1:$monitor_port/api/v1/health" \
    > "$RAW/system-monitor-health.json" 2> "$RAW/system-monitor-health.error" || printf '{}\n' > "$RAW/system-monitor-health.json"
for log in "$ROOT"/logs/*.log; do [ -f "$log" ] && cp "$log" "$RAW/"; done

if [ "$trace_evidence_index" -gt 0 ] && [ -s "$MQTT_LOG" ]; then
    awk -v target="$trace_evidence_index" '
        /^mqtt full / {
            indexValue=""; pointValue=""; pointTs=""; publishedAt=""; missing=0
            for(i=1;i<=NF;++i){
                split($i,a,"=")
                if(a[1]=="evidenceIndex")indexValue=a[2]
                if(a[1]=="evidenceValue")pointValue=a[2]
                if(a[1]=="evidencePointTs")pointTs=a[2]
                if(a[1]=="publishedAt")publishedAt=a[2]
                if(a[1]=="evidenceMissing")missing=a[2]
            }
            if(indexValue==target && !missing && pointTs>0 && publishedAt>0){
                printf "%.0f\t%.0f\t%.0f\n", pointValue, pointTs, publishedAt
            }
        }
    ' "$MQTT_LOG" > "$RAW/trace-mqtt-publisher.tsv"
fi

awk 'NR>1 { dt=$2-prevTotal; dp=$3-prevProcess; if(dt>0 && dp>=0) printf "%.4f\n", dp*100/dt } { prevTotal=$2; prevProcess=$3 }' \
    "$RAW/resource-samples.tsv" > "$RAW/cpu-normalized-pct.txt"
awk '{print $4}' "$RAW/resource-samples.tsv" > "$RAW/rss-kb.txt"
awk '{print $5}' "$RAW/resource-samples.tsv" > "$RAW/fd-count.txt"
awk '{print $1}' "$RAW/point-observations.tsv" > "$RAW/freshness-ms.txt" 2>/dev/null || true

set -- $(numeric_stats "$RAW/freshness-ms.txt")
fresh_count=$1; fresh_avg=$2; fresh_p50=$3; fresh_p95=$4; fresh_p99=$5; fresh_max=$6
set -- $(numeric_stats "$RAW/cpu-normalized-pct.txt")
cpu_count=$1; cpu_avg=$2; cpu_p50=$3; cpu_p95=$4; cpu_p99=$5; cpu_max=$6
set -- $(numeric_stats "$RAW/rss-kb.txt")
rss_count=$1; rss_avg=$2; rss_p50=$3; rss_p95=$4; rss_p99=$5; rss_max=$6
set -- $(numeric_stats "$RAW/fd-count.txt")
fd_count_samples=$1; fd_avg=$2; fd_p50=$3; fd_p95=$4; fd_p99=$5; fd_max=$6

observations=$(wc -l < "$RAW/point-observations.tsv" 2>/dev/null | tr -d ' ' || printf '0')
good_observations=$(awk '$2==1 && $3==0 {count++} END{print count+0}' "$RAW/point-observations.tsv" 2>/dev/null || printf '0')
stale_observations=$(awk '$3!=0 {count++} END{print count+0}' "$RAW/point-observations.tsv" 2>/dev/null || printf '0')
quality_good_pct=$(ratio_pct "$good_observations" "$observations")

trace_epoch_ms=$(json_number "$RAW/simulator-start.json" traceEpochMs 0)
trace_period_ms=$(json_number "$RAW/simulator-start.json" tracePeriodMs 0)
: > "$RAW/trace-source-to-shared-ms.txt"
: > "$RAW/trace-shared-to-mqtt-ms.txt"
: > "$RAW/trace-source-to-mqtt-ms.txt"
if [ "$trace_epoch_ms" -gt 0 ] && [ "$trace_period_ms" -gt 0 ]; then
    awk -v epoch="$trace_epoch_ms" -v period="$trace_period_ms" '
        { latency=$2-(epoch+$1*period); if(latency>=0) printf "%.0f\n", latency }
    ' "$RAW/trace-shared-memory.tsv" > "$RAW/trace-source-to-shared-ms.txt"
    awk -v epoch="$trace_epoch_ms" -v period="$trace_period_ms" '
        {
            source=epoch+$1*period
            sharedToMqtt=$3-$2
            sourceToMqtt=$3-source
            if(sharedToMqtt>=0) printf "%.0f\n", sharedToMqtt >> sharedFile
            if(sourceToMqtt>=0) printf "%.0f\n", sourceToMqtt >> sourceFile
        }
    ' sharedFile="$RAW/trace-shared-to-mqtt-ms.txt" sourceFile="$RAW/trace-source-to-mqtt-ms.txt" \
        "$RAW/trace-mqtt-publisher.tsv"
fi
set -- $(numeric_stats "$RAW/trace-source-to-shared-ms.txt")
trace_shared_count=$1; trace_shared_avg=$2; trace_shared_p50=$3; trace_shared_p95=$4; trace_shared_p99=$5; trace_shared_max=$6
set -- $(numeric_stats "$RAW/trace-shared-to-mqtt-ms.txt")
trace_mqtt_stage_count=$1; trace_mqtt_stage_avg=$2; trace_mqtt_stage_p50=$3; trace_mqtt_stage_p95=$4; trace_mqtt_stage_p99=$5; trace_mqtt_stage_max=$6
set -- $(numeric_stats "$RAW/trace-source-to-mqtt-ms.txt")
trace_mqtt_count=$1; trace_mqtt_avg=$2; trace_mqtt_p50=$3; trace_mqtt_p95=$4; trace_mqtt_p99=$5; trace_mqtt_max=$6
trace_sequence_available=0
if [ "$trace_shared_count" -gt 0 ] && [ "$trace_mqtt_count" -gt 0 ]; then trace_sequence_available=1; fi

status_start=$RAW/simulator-start.json
status_end=$RAW/simulator-end.json
protocol_errors=$(nonnegative_delta "$(json_number "$status_start" protocolErrors)" "$(json_number "$status_end" protocolErrors)")
requests=0
responses=0
protocol_frames=0
protocol_measured=1
case "$protocol" in
    modbus-tcp|modbus-rtu|dlt645)
        requests=$(nonnegative_delta "$(json_number "$status_start" requests)" "$(json_number "$status_end" requests)")
        responses=$(nonnegative_delta "$(json_number "$status_start" responses)" "$(json_number "$status_end" responses)")
        protocol_frames=$requests
        [ "$requests" -gt 0 ] || protocol_measured=0
        ;;
    can)
        protocol_frames=$(nonnegative_delta "$(json_number "$status_start" transmittedFrames)" "$(json_number "$status_end" transmittedFrames)")
        responses=$protocol_frames
        requests=$protocol_frames
        [ "$protocol_frames" -gt 0 ] || protocol_measured=0
        ;;
    iec104)
        protocol_frames=$(nonnegative_delta "$(json_number "$status_start" transmittedFrames)" "$(json_number "$status_end" transmittedFrames)")
        responses=$protocol_frames
        requests=$protocol_frames
        [ "$protocol_frames" -gt 0 ] || protocol_measured=0
        ;;
    dio) protocol_measured=0 ;;
esac
if [ "$mode" = "hil" ]; then protocol_measured=0; fi
protocol_success_pct=$(ratio_pct "$responses" "$requests")
protocol_fps=$(awk -v count="$protocol_frames" -v seconds="$actual_duration_sec" 'BEGIN{printf "%.2f", count/seconds}')
protocol_actual_display="未自动计数"
protocol_threshold_display="HIL/DIDO 仅依据点位链路、质量和新鲜度判定"
if [ "$protocol_measured" -eq 1 ]; then
    protocol_actual_display="成功率 $protocol_success_pct%，错误 $protocol_errors"
    protocol_threshold_display=">= $min_protocol_success_pct%，错误为 0"
fi

point_seq_start=$(stat_sum "$RAW/point-store-start.txt" pointUpdateSeq)
point_seq_end=$(stat_sum "$RAW/point-store-end.txt" pointUpdateSeq)
point_updates=$(nonnegative_delta "$point_seq_start" "$point_seq_end")
point_updates_per_sec=$(awk -v count="$point_updates" -v seconds="$actual_duration_sec" 'BEGIN{printf "%.2f", count/seconds}')
pending_writes=$(stat_sum "$RAW/point-store-end.txt" pendingWrites)

schedule_evaluated=0
schedule_achievement_json=null
expected_frames_json=null
schedule_display=未配置
if [ "$protocol_measured" -eq 1 ] && float_ge "$expected_frame_rate" 0.000001; then
    schedule_evaluated=1
    expected_frames=$(awk -v rate="$expected_frame_rate" -v seconds="$actual_duration_sec" 'BEGIN{printf "%.2f", rate*seconds}')
    schedule_achievement=$(ratio_pct "$protocol_frames" "$expected_frames")
    schedule_achievement_json=$schedule_achievement
    expected_frames_json=$expected_frames
    schedule_display=$schedule_achievement%
fi

compute_start=$RAW/compute-start.json
compute_end=$RAW/compute-end.json
compute_cycles=$(nonnegative_delta "$(json_number "$compute_start" totalCycles)" "$(json_number "$compute_end" totalCycles)")
compute_deadline_misses=$(nonnegative_delta "$(json_number "$compute_start" deadlineMissCycles)" "$(json_number "$compute_end" deadlineMissCycles)")
compute_failures=$(nonnegative_delta "$(json_number "$compute_start" failedCycles)" "$(json_number "$compute_end" failedCycles)")
compute_rules=$(nonnegative_delta "$(json_number "$compute_start" evaluatedRules)" "$(json_number "$compute_end" evaluatedRules)")
compute_outputs=$(nonnegative_delta "$(json_number "$compute_start" outputsWritten)" "$(json_number "$compute_end" outputsWritten)")
compute_rules_per_sec=$(awk -v count="$compute_rules" -v seconds="$actual_duration_sec" 'BEGIN{printf "%.2f", count/seconds}')
compute_scan_p50=$(json_number "$compute_end" scanP50Ms)
compute_scan_p95=$(json_number "$compute_end" scanP95Ms)
compute_scan_p99=$(json_number "$compute_end" scanP99Ms)
compute_scan_max=$(json_number "$compute_end" scanMaxMs)
compute_scan_utilization=$(json_number "$compute_end" scanUtilizationP95Percent 100)

case "$protocol" in
    modbus-tcp) compute_source_index=910002 ;;
    modbus-rtu) compute_source_index=940001 ;;
    dlt645) compute_source_index=970002 ;;
    can) compute_source_index=990001 ;;
    dio) compute_source_index=991001 ;;
    iec104) compute_source_index=992002 ;;
esac
compute_output_index=999901
compute_correct=0
compute_source_value=
compute_output_value=
compute_output_quality=
compute_correct_attempt=0
: > "$RAW/compute-correctness-attempts.tsv"
while [ "$compute_correct_attempt" -lt 20 ]; do
    compute_correct_attempt=$((compute_correct_attempt + 1))
    if "$POINTCTL" snapshot --app-config "$APP_CONFIG" > "$RAW/point-snapshot-correctness.txt" \
        2>> "$RAW/pointctl-errors.log"; then
        compute_source_value=$(point_value "$RAW/point-snapshot-correctness.txt" "$compute_source_index" value || true)
        compute_output_value=$(point_value "$RAW/point-snapshot-correctness.txt" "$compute_output_index" value || true)
        compute_output_quality=$(point_value "$RAW/point-snapshot-correctness.txt" "$compute_output_index" quality || true)
        printf '%s\t%s\t%s\t%s\n' "$compute_correct_attempt" "${compute_source_value:--}" \
            "${compute_output_value:--}" "${compute_output_quality:--}" >> "$RAW/compute-correctness-attempts.tsv"
        cp "$RAW/point-snapshot-correctness.txt" "$RAW/point-snapshot-final.txt"
        if [ -n "$compute_source_value" ] && [ -n "$compute_output_value" ] && [ "$compute_output_quality" = "1" ] && \
            awk -v source="$compute_source_value" -v output="$compute_output_value" \
                'BEGIN{diff=output-source*2;if(diff<0)diff=-diff;exit !(diff<=0.01)}'; then
            compute_correct=1
            break
        fi
    fi
    sleep 0.05
done

mqtt_start=$RAW/mqtt-start.json
mqtt_end=$RAW/mqtt-end.json
mqtt_cycles=$(nonnegative_delta "$(json_number "$mqtt_start" totalScanCycles)" "$(json_number "$mqtt_end" totalScanCycles)")
mqtt_deadline_misses=$(nonnegative_delta "$(json_number "$mqtt_start" scanDeadlineMissCycles)" "$(json_number "$mqtt_end" scanDeadlineMissCycles)")
mqtt_failures=$(nonnegative_delta "$(json_number "$mqtt_start" scanFailedCycles)" "$(json_number "$mqtt_end" scanFailedCycles)")
mqtt_full=$(nonnegative_delta "$(json_number "$mqtt_start" fullSnapshotsPublished)" "$(json_number "$mqtt_end" fullSnapshotsPublished)")
mqtt_scan_p50=$(json_number "$mqtt_end" scanP50Ms)
mqtt_scan_p95=$(json_number "$mqtt_end" scanP95Ms)
mqtt_scan_p99=$(json_number "$mqtt_end" scanP99Ms)
mqtt_scan_max=$(json_number "$mqtt_end" scanMaxMs)
mqtt_scan_utilization=$(json_number "$mqtt_end" scanUtilizationP95Percent 100)
full_interval_ms=$(sed -n 's/.*"fullUploadIntervalMs":[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$APP_CONFIG" | head -n 1)
[ -n "$full_interval_ms" ] || full_interval_ms=1000
expected_full=$((actual_duration_ms / full_interval_ms))
mqtt_full_achievement=$(awk -v actual="$mqtt_full" -v expected="$expected_full" \
    'BEGIN{if(expected<=0){print "0.00"}else{value=actual*100/expected;if(value>100)value=100;printf "%.2f",value}}')

rss_first=$(head -n 1 "$RAW/rss-kb.txt" 2>/dev/null || printf '0')
rss_last=$(tail -n 1 "$RAW/rss-kb.txt" 2>/dev/null || printf '0')
rss_growth=$(awk -v first="$rss_first" -v last="$rss_last" 'BEGIN{printf "%.0f", last-first}')

services_expected=5
if [ "$mode" = "virtual" ] && [ "$protocol" != "dio" ]; then services_expected=6; fi
services_alive=0
for pid_file in "$ROOT"/run/modbus-driver.pid "$ROOT"/run/compute-engine.pid \
    "$ROOT"/run/event-engine.pid "$ROOT"/run/mqtt-driver.pid "$ROOT"/run/system-monitor.pid; do
    if process_is_alive "$pid_file"; then services_alive=$((services_alive + 1)); fi
done
if [ "$services_expected" -eq 6 ] && process_is_alive "$ROOT/run/simulator.pid"; then services_alive=$((services_alive + 1)); fi

check_services=0; [ "$services_alive" -eq "$services_expected" ] && check_services=1
check_protocol=1
if [ "$protocol_measured" -eq 1 ]; then
    if ! float_ge "$protocol_success_pct" "$min_protocol_success_pct" || [ "$protocol_errors" -ne 0 ]; then check_protocol=0; fi
fi
check_schedule=1
if [ "$schedule_evaluated" -eq 1 ] && ! float_ge "$schedule_achievement" "$min_schedule_achievement_pct"; then check_schedule=0; fi
check_points=0; [ "$point_updates" -gt 0 ] && [ "$pending_writes" -eq 0 ] && check_points=1
check_freshness=0
if [ "$fresh_count" -gt 0 ] && float_le "$fresh_p95" "$max_freshness_p95_ms"; then check_freshness=1; fi
check_quality=0
if [ "$observations" -gt 0 ] && float_ge "$quality_good_pct" "$min_quality_good_pct"; then check_quality=1; fi
check_compute=0
if [ "$compute_cycles" -gt 0 ] && [ "$compute_deadline_misses" -eq 0 ] && [ "$compute_failures" -eq 0 ] && \
    [ "$compute_rules" -gt 0 ] && [ "$compute_correct" -eq 1 ] && \
    float_le "$compute_scan_utilization" "$max_compute_scan_utilization_pct"; then check_compute=1; fi
check_mqtt=0
if [ "$mqtt_cycles" -gt 0 ] && [ "$mqtt_deadline_misses" -eq 0 ] && [ "$mqtt_failures" -eq 0 ] && \
    float_le "$mqtt_scan_utilization" "$max_mqtt_scan_utilization_pct" && \
    float_ge "$mqtt_full_achievement" "$min_mqtt_full_achievement_pct"; then check_mqtt=1; fi
check_resources=0
if [ "$cpu_count" -gt 0 ] && float_le "$cpu_p95" "$max_cpu_p95_pct" && float_le "$rss_growth" "$max_rss_growth_kb"; then check_resources=1; fi

overall_pass=1
for check in "$check_services" "$check_protocol" "$check_schedule" "$check_points" "$check_freshness" \
    "$check_quality" "$check_compute" "$check_mqtt" "$check_resources"; do
    [ "$check" -eq 1 ] || overall_pass=0
done
if [ "$overall_pass" -eq 1 ]; then overall_text=PASS; overall_json=true; else overall_text=FAIL; overall_json=false; fi

bool_json() { if [ "$1" -eq 1 ]; then printf true; else printf false; fi; }

cat > "$output_dir/report.json" <<EOF
{
  "schemaVersion": "1.0",
  "result": "$overall_text",
  "passed": $overall_json,
  "test": {"protocol":"$protocol","scenario":"$scenario","mode":"$mode","warmupSec":$warmup_sec,"configuredDurationSec":$duration_sec,"actualDurationMs":$actual_duration_ms,"sampleIntervalSec":$sample_interval_sec,"expectedFrameRate":$expected_frame_rate},
  "thresholds": {
    "minProtocolSuccessPercent":$min_protocol_success_pct,"minScheduleAchievementPercent":$min_schedule_achievement_pct,
    "minQualityGoodPercent":$min_quality_good_pct,"maxFreshnessP95Ms":$max_freshness_p95_ms,
    "maxComputeScanUtilizationPercent":$max_compute_scan_utilization_pct,"maxMqttScanUtilizationPercent":$max_mqtt_scan_utilization_pct,
    "minMqttFullAchievementPercent":$min_mqtt_full_achievement_pct,"maxNormalizedCpuP95Percent":$max_cpu_p95_pct,
    "maxRssGrowthKb":$max_rss_growth_kb
  },
  "timing": {
    "protocol":"$protocol","transport":"$timing_transport","model":"$timing_model",
    "configuredCycleMs":$configured_cycle_ms,"operationCount":$operation_count,
    "operationTimeMs":$operation_time_ms,"estimatedMinimumCycleMs":$estimated_minimum_cycle_ms,
    "worstCaseCycleMs":$worst_case_cycle_ms,"linkUtilizationPercent":$link_utilization_pct,
    "worstCaseUtilizationPercent":$worst_link_utilization_pct,"recommendedCycleMs":$recommended_cycle_ms,
    "verdict":"$timing_verdict","verdictZh":"$timing_verdict_zh",
    "estimationSource":"$timing_estimation_source","physicalMediaTestMode":$timing_physical_mode,
    "inputs": {
      "registersPerRequest":$registers_per_request,"retryCount":$retry_count,
      "deviceResponseMs":$device_response_ms,"parseStoreMs":$timing_parse_store_ms,
      "baudRate":$baud_rate,"dataBits":$data_bits,"stopBits":$stop_bits,"parity":"$parity",
      "frameIntervalMs":$frame_interval_ms,"turnaroundMs":$timing_turnaround_ms,
      "requestBytes":$request_bytes,"responseBytes":$response_bytes,"wakeupBytes":$wakeup_bytes,
      "networkRttMs":$timing_network_rtt_ms,"socketQueueMs":$timing_socket_queue_ms,"linkMbps":$timing_link_mbps,
      "canBitrate":$timing_can_bitrate,"canPayloadBytes":$timing_can_payload_bytes,
      "debounceMs":$timing_debounce_ms,"ioOperationMs":$timing_io_operation_ms
    }
  },
  "collection": {
    "measured": $(bool_json "$protocol_measured"),
    "requests": $requests,
    "responses": $responses,
    "frames": $protocol_frames,
    "framesPerSecond": $protocol_fps,
    "protocolErrors": $protocol_errors,
    "successPercent": $protocol_success_pct,
    "expectedFrames": $expected_frames_json,
    "scheduleAchievementPercent": $schedule_achievement_json,
    "pointUpdates": $point_updates,
    "pointUpdatesPerSecond": $point_updates_per_sec,
    "freshnessMs": {"samples":$fresh_count,"average":$fresh_avg,"p50":$fresh_p50,"p95":$fresh_p95,"p99":$fresh_p99,"max":$fresh_max},
    "quality": {"observations":$observations,"good":$good_observations,"stale":$stale_observations,"goodPercent":$quality_good_pct}
  },
  "sharedMemory": {"pointUpdateSequenceDelta":$point_updates,"pendingWrites":$pending_writes},
  "compute": {
    "cycles":$compute_cycles,"deadlineMisses":$compute_deadline_misses,"failures":$compute_failures,
    "evaluatedRules":$compute_rules,"rulesPerSecond":$compute_rules_per_sec,"outputsWritten":$compute_outputs,
    "scanMs":{"p50":$compute_scan_p50,"p95":$compute_scan_p95,"p99":$compute_scan_p99,"max":$compute_scan_max},
    "scanUtilizationP95Percent":$compute_scan_utilization,
    "correctness":{"sourceIndex":$compute_source_index,"sourceValue":${compute_source_value:-null},"outputIndex":$compute_output_index,"outputValue":${compute_output_value:-null},"outputQuality":${compute_output_quality:-null},"passed":$(bool_json "$compute_correct")}
  },
  "mqtt": {
    "networkBrokerEnabled": $(if [ -n "$mqtt_broker" ]; then printf true; else printf false; fi),
    "scanCycles":$mqtt_cycles,"deadlineMisses":$mqtt_deadline_misses,"failures":$mqtt_failures,
    "scanMs":{"p50":$mqtt_scan_p50,"p95":$mqtt_scan_p95,"p99":$mqtt_scan_p99,"max":$mqtt_scan_max},
    "scanUtilizationP95Percent":$mqtt_scan_utilization,
    "fullUploadIntervalMs":$full_interval_ms,"expectedFullSnapshots":$expected_full,"publishedFullSnapshots":$mqtt_full,"fullUploadAchievementPercent":$mqtt_full_achievement
  },
  "resources": {
    "normalizedCpuPercent":{"samples":$cpu_count,"average":$cpu_avg,"p50":$cpu_p50,"p95":$cpu_p95,"p99":$cpu_p99,"max":$cpu_max},
    "rssKb":{"samples":$rss_count,"average":$rss_avg,"p50":$rss_p50,"p95":$rss_p95,"p99":$rss_p99,"max":$rss_max,"growth":$rss_growth},
    "fileDescriptors":{"samples":$fd_count_samples,"average":$fd_avg,"p50":$fd_p50,"p95":$fd_p95,"p99":$fd_p99,"max":$fd_max}
  },
  "endToEnd": {
    "observedFreshnessP95Ms":$fresh_p95,
    "traceIdAvailable":false,
    "sequenceEvidenceAvailable":$(bool_json "$trace_sequence_available"),
    "evidenceIndex":$trace_evidence_index,
    "evidenceScope":"$(if [ "$trace_sequence_available" -eq 1 ]; then printf simulator-to-mqtt-publisher; else printf observation-only; fi)",
    "sourceToSharedMemoryMs":{"samples":$trace_shared_count,"average":$trace_shared_avg,"p50":$trace_shared_p50,"p95":$trace_shared_p95,"p99":$trace_shared_p99,"max":$trace_shared_max},
    "sharedMemoryToMqttPublisherMs":{"samples":$trace_mqtt_stage_count,"average":$trace_mqtt_stage_avg,"p50":$trace_mqtt_stage_p50,"p95":$trace_mqtt_stage_p95,"p99":$trace_mqtt_stage_p99,"max":$trace_mqtt_stage_max},
    "sourceToMqttPublisherMs":{"samples":$trace_mqtt_count,"average":$trace_mqtt_avg,"p50":$trace_mqtt_p50,"p95":$trace_mqtt_p95,"p99":$trace_mqtt_p99,"max":$trace_mqtt_max},
    "note":"$(if [ "$trace_sequence_available" -eq 1 ]; then printf '测试序列已贯通模拟设备、真实驱动、共享内存和 MQTT 发布调用；外部 broker 与客户端未纳入，故 traceIdAvailable 仍为 false'; else printf '当前仅测量共享内存点时间戳到测试程序观察时刻；精确 T1-T7 仍需外部 broker 和客户端关联证据'; fi)"
  },
  "services": {"expected":$services_expected,"alive":$services_alive},
  "checks": {
    "services":$(bool_json "$check_services"),"protocol":$(bool_json "$check_protocol"),"schedule":$(bool_json "$check_schedule"),
    "pointFlow":$(bool_json "$check_points"),"freshness":$(bool_json "$check_freshness"),"quality":$(bool_json "$check_quality"),
    "compute":$(bool_json "$check_compute"),"mqtt":$(bool_json "$check_mqtt"),"resources":$(bool_json "$check_resources")
  },
  "artifactsDirectory": "raw"
}
EOF

cat > "$output_dir/report.md" <<EOF
# 边端一键性能测试报告

## 结论

**$overall_text**

| 项目 | 结果 | 实际值 | 门限 |
| --- | --- | --- | --- |
| 服务存活 | $( [ "$check_services" -eq 1 ] && printf 通过 || printf 失败 ) | $services_alive/$services_expected | 全部存活 |
| 协议采集 | $( [ "$check_protocol" -eq 1 ] && printf 通过 || printf 失败 ) | $protocol_actual_display | $protocol_threshold_display |
| 调度达成 | $( [ "$check_schedule" -eq 1 ] && printf 通过 || printf 失败 ) | $schedule_display | 未配置期望速率时不判失败；启用后 >= $min_schedule_achievement_pct% |
| 点位链路 | $( [ "$check_points" -eq 1 ] && printf 通过 || printf 失败 ) | $point_updates 次，$point_updates_per_sec 点/s，待写 $pending_writes | 有更新且无待写积压 |
| 点新鲜度 | $( [ "$check_freshness" -eq 1 ] && printf 通过 || printf 失败 ) | P95 $fresh_p95 ms | <= $max_freshness_p95_ms ms |
| 数据质量 | $( [ "$check_quality" -eq 1 ] && printf 通过 || printf 失败 ) | 好质量 $quality_good_pct% | >= $min_quality_good_pct% |
| 计算引擎 | $( [ "$check_compute" -eq 1 ] && printf 通过 || printf 失败 ) | P95 $compute_scan_p95 ms，利用率 $compute_scan_utilization%，规则 $compute_rules | 无违约/异常，利用率 <= $max_compute_scan_utilization_pct% |
| MQTT | $( [ "$check_mqtt" -eq 1 ] && printf 通过 || printf 失败 ) | P95 $mqtt_scan_p95 ms，全量达成 $mqtt_full_achievement% | 无违约/异常，利用率 <= $max_mqtt_scan_utilization_pct%，全量 >= $min_mqtt_full_achievement_pct% |
| 资源 | $( [ "$check_resources" -eq 1 ] && printf 通过 || printf 失败 ) | CPU P95 $cpu_p95%，RSS 增长 $rss_growth KiB | CPU <= $max_cpu_p95_pct%，RSS 增长 <= $max_rss_growth_kb KiB |

## 测试条件

- 协议：$protocol
- 场景：$scenario
- 模式：$mode
- 预热：${warmup_sec}s
- 正式采样：${duration_sec}s
- 实际计数窗口：${actual_duration_sec}s
- 采样间隔：${sample_interval_sec}s
- MQTT broker：${mqtt_broker:-未启用，仅测边端内部扫描和组包}

## 协议与介质时间估算

| 项目 | 结果 |
| --- | ---: |
| 传输介质 | $timing_transport |
| 配置目标周期 | $configured_cycle_ms ms |
| 每轮协议操作数 | $operation_count |
| 单次请求/帧估算 | $operation_time_ms ms |
| 理论最小轮次 | $estimated_minimum_cycle_ms ms |
| 最坏重试轮次 | $worst_case_cycle_ms ms |
| 正常链路占用 | $link_utilization_pct% |
| 最坏链路占用 | $worst_link_utilization_pct% |
| 建议目标周期 | $recommended_cycle_ms ms |
| 可达性结论 | $timing_verdict_zh |
| 估算输入来源 | $timing_estimation_source |
| 实体介质测试模式 | $( [ "$mode" = hil ] && printf 是 || printf 否 ) |

理论估算用于判断配置是否具备可实现性，不等于实测 P95/P99。hil 只表示本轮按实体接口参数运行，不自动证明接线、电气质量或现场设备兼容性通过。

## 分位数

| 指标 | 样本 | 平均 | P50 | P95 | P99 | 最大 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 点新鲜度 ms | $fresh_count | $fresh_avg | $fresh_p50 | $fresh_p95 | $fresh_p99 | $fresh_max |
| 源事件到共享内存 ms | $trace_shared_count | $trace_shared_avg | $trace_shared_p50 | $trace_shared_p95 | $trace_shared_p99 | $trace_shared_max |
| 共享内存到 MQTT 发布调用 ms | $trace_mqtt_stage_count | $trace_mqtt_stage_avg | $trace_mqtt_stage_p50 | $trace_mqtt_stage_p95 | $trace_mqtt_stage_p99 | $trace_mqtt_stage_max |
| 源事件到 MQTT 发布调用 ms | $trace_mqtt_count | $trace_mqtt_avg | $trace_mqtt_p50 | $trace_mqtt_p95 | $trace_mqtt_p99 | $trace_mqtt_max |
| Compute 扫描 ms | $compute_cycles | - | $compute_scan_p50 | $compute_scan_p95 | $compute_scan_p99 | $compute_scan_max |
| MQTT 扫描 ms | $mqtt_cycles | - | $mqtt_scan_p50 | $mqtt_scan_p95 | $mqtt_scan_p99 | $mqtt_scan_max |
| 整机归一化 CPU % | $cpu_count | $cpu_avg | $cpu_p50 | $cpu_p95 | $cpu_p99 | $cpu_max |
| 服务 RSS KiB | $rss_count | $rss_avg | $rss_p50 | $rss_p95 | $rss_p99 | $rss_max |
| 文件描述符 | $fd_count_samples | $fd_avg | $fd_p50 | $fd_p95 | $fd_p99 | $fd_max |

## 采集与计算

- 协议业务帧：$protocol_frames（$protocol_fps 帧/s）
- 请求/响应：$requests/$responses
- 点更新：$point_updates（$point_updates_per_sec 点/s）
- Compute 执行规则：$compute_rules（$compute_rules_per_sec 规则/s）
- Compute 输出：$compute_outputs
- 二倍值正确性：$( [ "$compute_correct" -eq 1 ] && printf 通过 || printf 失败 )，源值 ${compute_source_value:---}，输出 ${compute_output_value:---}
- MQTT 全量：$mqtt_full/$expected_full，达成率 $mqtt_full_achievement%

## 证据与限制

- 原始证据：\`raw/\`
- 配置快照：\`config/\`
- 机器信息、二进制 SHA256、起止计数、点位快照和服务日志均已保留。
- Modbus TCP/RTU 虚拟测试使用专用单调序列点关联模拟设备、真实驱动、共享内存和 MQTT 发布调用；本轮序列证据：$( [ "$trace_sequence_available" -eq 1 ] && printf 可用 || printf 不可用 )。
- 序列证据不经过外部 broker 和 Windows 客户端，因此只报告边端内部阶段耗时，\`traceIdAvailable\` 保持为 \`false\`，不会伪装成完整 T1-T7。
- 未配置 \`--expected-frame-rate\` 时，调度达成率显示为 \`null\` 且不参与失败判定。
EOF

if [ "$keep_running" -eq 0 ]; then
    "$LAB_COMMAND" stop > "$RAW/stop.log" 2>&1 || true
    started=0
fi

printf '\nreport.json: %s\nreport.md:   %s\n\n' "$output_dir/report.json" "$output_dir/report.md"
cat "$output_dir/report.md"

if [ "$overall_pass" -eq 1 ]; then exit 0; else exit 1; fi
