#!/bin/bash
set -u

CONFIG_FILE="${CONFIG_FILE:-/etc/default/gateway-network-failover}"
if [ -r "$CONFIG_FILE" ]; then
    # shellcheck source=/dev/null
    . "$CONFIG_FILE"
fi

: "${RUNTIME_CONFIG_FILE:=/opt/modbus-gateway/config/runtime/apps/monitor-service.json}"

load_runtime_config() {
    [ -r "$RUNTIME_CONFIG_FILE" ] || return 0
    command -v python3 >/dev/null 2>&1 || return 0

    local name value
    while IFS=$'\t' read -r name value; do
        case "$name" in
            FAILOVER_ENABLED) FAILOVER_ENABLED="$value" ;;
            PREFER_CELLULAR) PREFER_CELLULAR="$value" ;;
            CELLULAR_INTERFACE) CELLULAR_INTERFACE="$value" ;;
            CELLULAR_GATEWAY) CELLULAR_GATEWAY="$value" ;;
            WIRED_INTERFACE) WIRED_INTERFACE="$value" ;;
            WIRED_INTERFACES) WIRED_INTERFACES="$value" ;;
            WIRED_ROUTE_METRIC) WIRED_ROUTE_METRIC="$value" ;;
            WIRED_STANDBY_ROUTE_METRIC) WIRED_STANDBY_ROUTE_METRIC="$value" ;;
            PROBE_HOST) PROBE_HOST="$value" ;;
            PROBE_PORT) PROBE_PORT="$value" ;;
            PROBE_TIMEOUT_SEC) PROBE_TIMEOUT_SEC="$value" ;;
            CHECK_INTERVAL_SEC) CHECK_INTERVAL_SEC="$value" ;;
            FAILURE_THRESHOLD) FAILURE_THRESHOLD="$value" ;;
            RECOVERY_THRESHOLD) RECOVERY_THRESHOLD="$value" ;;
            CELLULAR_ROUTE_METRIC) CELLULAR_ROUTE_METRIC="$value" ;;
            FALLBACK_ROUTE_METRIC) FALLBACK_ROUTE_METRIC="$value" ;;
        esac
    done < <(python3 - "$RUNTIME_CONFIG_FILE" 2>/dev/null <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as handle:
    root = json.load(handle)

config = (((root.get("systemMonitor") or {}).get("cellular") or {}).get("routeFailover") or {})
wired_interfaces = config.get("wiredInterfaces")
if isinstance(wired_interfaces, list):
    values = [str(item).strip() for item in wired_interfaces if str(item).strip()]
    if values:
        print(f"WIRED_INTERFACES\t{' '.join(values)}")
fields = (
    ("FAILOVER_ENABLED", "enabled"),
    ("PREFER_CELLULAR", "preferCellular"),
    ("CELLULAR_INTERFACE", "cellularInterface"),
    ("CELLULAR_GATEWAY", "cellularGateway"),
    ("WIRED_INTERFACE", "wiredInterface"),
    ("WIRED_ROUTE_METRIC", "wiredRouteMetric"),
    ("WIRED_STANDBY_ROUTE_METRIC", "wiredStandbyRouteMetric"),
    ("PROBE_HOST", "probeHost"),
    ("PROBE_PORT", "probePort"),
    ("PROBE_TIMEOUT_SEC", "probeTimeoutSec"),
    ("CHECK_INTERVAL_SEC", "checkIntervalSec"),
    ("FAILURE_THRESHOLD", "failureThreshold"),
    ("RECOVERY_THRESHOLD", "recoveryThreshold"),
    ("CELLULAR_ROUTE_METRIC", "cellularRouteMetric"),
    ("FALLBACK_ROUTE_METRIC", "fallbackRouteMetric"),
)
for output_name, field_name in fields:
    if field_name not in config:
        continue
    value = config[field_name]
    if isinstance(value, bool):
        value = "true" if value else "false"
    print(f"{output_name}\t{value}")
PY
    )
}

load_runtime_config

: "${FAILOVER_ENABLED:=true}"
: "${PREFER_CELLULAR:=true}"
: "${CELLULAR_INTERFACE:=usb0}"
: "${CELLULAR_GATEWAY:=192.168.43.1}"
: "${WIRED_INTERFACE:=ens1}"
: "${WIRED_INTERFACES:=$WIRED_INTERFACE auto}"
: "${WIRED_ROUTE_METRIC:=50}"
: "${WIRED_STANDBY_ROUTE_METRIC:=200}"
: "${PROBE_HOST:=223.5.5.5}"
: "${PROBE_PORT:=53}"
: "${PROBE_TIMEOUT_SEC:=4}"
: "${CHECK_INTERVAL_SEC:=15}"
: "${FAILURE_THRESHOLD:=3}"
: "${RECOVERY_THRESHOLD:=2}"
: "${CELLULAR_ROUTE_METRIC:=10}"
: "${FALLBACK_ROUTE_METRIC:=600}"
: "${STATE_DIR:=/run/gateway-network-failover}"
: "${LOG_FILE:=/opt/modbus-gateway/data/network-failover.log}"
: "${MAX_LOG_SIZE:=1048576}"

STATE_FILE="$STATE_DIR/state"
PROBE_ROUTE_INSTALLED=0
PROBE_ROUTE_INTERFACE=""

mode="unknown"
failures=0
recoveries=0
last_result="unknown"
last_checked=""
last_message="not checked"
selected_wired_interface=""

log_message() {
    local message="$1"
    local timestamp
    timestamp=$(date '+%Y-%m-%d %H:%M:%S')
    mkdir -p "$(dirname "$LOG_FILE")"
    if [ -f "$LOG_FILE" ]; then
        local size
        size=$(stat -c '%s' "$LOG_FILE" 2>/dev/null || printf '0')
        if [ "$size" -ge "$MAX_LOG_SIZE" ]; then
            mv -f "$LOG_FILE" "$LOG_FILE.1"
        fi
    fi
    printf '%s - %s\n' "$timestamp" "$message" >> "$LOG_FILE"
    if command -v logger >/dev/null 2>&1; then
        logger -t gateway-network-failover -- "$message"
    fi
}

positive_integer() {
    [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

truthy() {
    case "${1,,}" in
        1|true|yes|on) return 0 ;;
        *) return 1 ;;
    esac
}

validate_config() {
    if ! [[ "$PROBE_HOST" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]]; then
        printf 'PROBE_HOST must be an IPv4 address: %s\n' "$PROBE_HOST" >&2
        return 2
    fi
    local value
    for value in "$PROBE_PORT" "$PROBE_TIMEOUT_SEC" "$CHECK_INTERVAL_SEC" \
        "$FAILURE_THRESHOLD" "$RECOVERY_THRESHOLD" "$CELLULAR_ROUTE_METRIC" \
        "$FALLBACK_ROUTE_METRIC" "$WIRED_ROUTE_METRIC" "$WIRED_STANDBY_ROUTE_METRIC"; do
        if ! positive_integer "$value"; then
            printf 'invalid positive integer in failover configuration: %s\n' "$value" >&2
            return 2
        fi
    done
}

load_state() {
    if [ -r "$STATE_FILE" ]; then
        # The file is root-owned and generated only by this script.
        # shellcheck source=/dev/null
        . "$STATE_FILE"
    fi
    [[ "$failures" =~ ^[0-9]+$ ]] || failures=0
    [[ "$recoveries" =~ ^[0-9]+$ ]] || recoveries=0
    selected_wired_interface="${selected_wired_interface:-}"
    case "$mode" in
        cellular|cellular-degraded|wired|disabled|unknown) ;;
        *) mode="unknown" ;;
    esac
}

save_state() {
    mkdir -p "$STATE_DIR"
    local temp_file="$STATE_FILE.tmp"
    {
        printf 'mode=%q\n' "$mode"
        printf 'failures=%q\n' "$failures"
        printf 'recoveries=%q\n' "$recoveries"
        printf 'last_result=%q\n' "$last_result"
        printf 'last_checked=%q\n' "$last_checked"
        printf 'last_message=%q\n' "$last_message"
        printf 'selected_wired_interface=%q\n' "$selected_wired_interface"
    } > "$temp_file"
    mv -f "$temp_file" "$STATE_FILE"
}

interface_ipv4() {
    local interface="$1"
    ip -4 -o address show dev "$interface" scope global 2>/dev/null |
        awk 'NR == 1 { split($4, address, "/"); print address[1] }'
}

cellular_ipv4() {
    interface_ipv4 "$CELLULAR_INTERFACE"
}

cellular_gateway() {
    local gateway
    gateway=$(ip -4 route show default dev "$CELLULAR_INTERFACE" 2>/dev/null |
        awk 'NR == 1 { for (i = 1; i <= NF; ++i) if ($i == "via") { print $(i + 1); exit } }')
    if [ -n "$gateway" ]; then
        printf '%s\n' "$gateway"
    else
        printf '%s\n' "$CELLULAR_GATEWAY"
    fi
}

interface_gateway() {
    local interface="$1"
    ip -4 route show default dev "$interface" 2>/dev/null |
        awk 'NR == 1 { for (i = 1; i <= NF; ++i) if ($i == "via") { print $(i + 1); exit } }'
}

interface_default_metric() {
    local interface="$1"
    ip -4 route show default dev "$interface" 2>/dev/null |
        awk 'NR == 1 {
            for (i = 1; i <= NF; ++i) {
                if ($i == "metric") { print $(i + 1); exit }
            }
            print 0
        }'
}

discover_physical_wired_interfaces() {
    local path interface
    for path in /sys/class/net/*; do
        [ -e "$path/device" ] || continue
        interface=${path##*/}
        [ "$interface" = "$CELLULAR_INTERFACE" ] && continue
        case "$interface" in
            en*|eth*) printf '%s\n' "$interface" ;;
        esac
    done
}

wired_candidates() {
    local item
    {
        for item in $WIRED_INTERFACES; do
            if [ "$item" = "auto" ]; then
                discover_physical_wired_interfaces
            elif [ -n "$item" ] && [ "$item" != "$CELLULAR_INTERFACE" ]; then
                printf '%s\n' "$item"
            fi
        done
    } | awk 'NF && !seen[$0]++'
}

ordered_wired_candidates() {
    {
        [ -n "$selected_wired_interface" ] && printf '%s\n' "$selected_wired_interface"
        wired_candidates
    } | awk 'NF && !seen[$0]++'
}

wired_candidates_csv() {
    wired_candidates | awk '
        BEGIN { first = 1 }
        {
            printf "%s%s", first ? "" : ",", $0
            first = 0
        }
        END { print "" }
    '
}

cellular_default_metric() {
    ip -4 route show default dev "$CELLULAR_INTERFACE" 2>/dev/null |
        awk 'NR == 1 {
            for (i = 1; i <= NF; ++i) {
                if ($i == "metric") { print $(i + 1); exit }
            }
            print 0
        }'
}

replace_cellular_default() {
    local metric="$1"
    local gateway
    gateway=$(cellular_gateway)
    if [ -z "$gateway" ]; then
        last_message="cellular gateway is unavailable"
        return 1
    fi
    while ip -4 route del default dev "$CELLULAR_INTERFACE" >/dev/null 2>&1; do
        :
    done
    if ! ip -4 route add default via "$gateway" dev "$CELLULAR_INTERFACE" metric "$metric"; then
        last_message="failed to install cellular default route"
        return 1
    fi
}

set_cellular_primary() {
    local metric
    metric=$(cellular_default_metric)
    if [ "$metric" != "$CELLULAR_ROUTE_METRIC" ]; then
        replace_cellular_default "$CELLULAR_ROUTE_METRIC" || return 1
        log_message "4G connectivity recovered; $CELLULAR_INTERFACE is the primary route (metric $CELLULAR_ROUTE_METRIC)"
    fi
    mode="cellular"
    last_message="4G healthy; cellular route is primary"
}

set_wired_fallback() {
    local interface="$1"
    local reason="${2:-4G connectivity failed}"
    local previous_interface="$selected_wired_interface"
    local gateway current_metric
    gateway=$(interface_gateway "$interface")
    [ -n "$gateway" ] || return 1

    current_metric=$(interface_default_metric "$interface")
    if [ "$current_metric" != "$WIRED_ROUTE_METRIC" ]; then
        while ip -4 route del default dev "$interface" >/dev/null 2>&1; do
            :
        done
        if ! ip -4 route add default via "$gateway" dev "$interface" metric "$WIRED_ROUTE_METRIC"; then
            last_message="$reason, but failed to promote wired interface $interface"
            return 1
        fi
    fi

    if [ -n "$previous_interface" ] && [ "$previous_interface" != "$interface" ]; then
        local previous_gateway
        previous_gateway=$(interface_gateway "$previous_interface")
        if [ -n "$previous_gateway" ]; then
            while ip -4 route del default dev "$previous_interface" >/dev/null 2>&1; do
                :
            done
            ip -4 route add default via "$previous_gateway" dev "$previous_interface" metric "$WIRED_STANDBY_ROUTE_METRIC" >/dev/null 2>&1 || true
        fi
    fi

    local metric
    metric=$(cellular_default_metric)
    if [ "$metric" != "$FALLBACK_ROUTE_METRIC" ]; then
        replace_cellular_default "$FALLBACK_ROUTE_METRIC" || return 1
        log_message "$reason; $interface is the primary wired route and $CELLULAR_INTERFACE is standby (metric $FALLBACK_ROUTE_METRIC)"
    fi
    if [ "$previous_interface" != "$interface" ]; then
        log_message "selected wired interface changed from ${previous_interface:-none} to $interface"
    fi
    selected_wired_interface="$interface"
    mode="wired"
    last_message="$reason; wired interface $interface is primary"
}

remove_probe_route() {
    if [ "$PROBE_ROUTE_INSTALLED" -eq 1 ]; then
        ip -4 route del "$PROBE_HOST/32" dev "$PROBE_ROUTE_INTERFACE" >/dev/null 2>&1 || true
        PROBE_ROUTE_INSTALLED=0
        PROBE_ROUTE_INTERFACE=""
    fi
}

probe_interface() {
    local interface="$1"
    local gateway="${2:-}"
    local address
    address=$(interface_ipv4 "$interface")
    [ -n "$gateway" ] || gateway=$(interface_gateway "$interface")
    if [ -z "$address" ] || [ -z "$gateway" ]; then
        return 1
    fi

    remove_probe_route
    if ! ip -4 route replace "$PROBE_HOST/32" via "$gateway" dev "$interface" src "$address" metric 5; then
        return 1
    fi
    PROBE_ROUTE_INSTALLED=1
    PROBE_ROUTE_INTERFACE="$interface"

    local result=1
    if timeout "$PROBE_TIMEOUT_SEC" ping -I "$interface" -c 1 -W "$PROBE_TIMEOUT_SEC" "$PROBE_HOST" >/dev/null 2>&1; then
        result=0
    elif timeout "$PROBE_TIMEOUT_SEC" bash -c "exec 3<>/dev/tcp/$PROBE_HOST/$PROBE_PORT" >/dev/null 2>&1; then
        result=0
    fi
    remove_probe_route
    return "$result"
}

probe_cellular() {
    probe_interface "$CELLULAR_INTERFACE" "$(cellular_gateway)"
}

select_healthy_wired_interface() {
    local interface
    while IFS= read -r interface; do
        [ -n "$interface" ] || continue
        if probe_interface "$interface"; then
            printf '%s\n' "$interface"
            return 0
        fi
    done < <(ordered_wired_candidates)
    return 1
}

keep_cellular_as_last_resort() {
    local should_log=0
    [ "$mode" != "cellular-degraded" ] && should_log=1
    if [ "$(cellular_default_metric)" != "$CELLULAR_ROUTE_METRIC" ]; then
        replace_cellular_default "$CELLULAR_ROUTE_METRIC" || return 1
        should_log=1
    fi
    mode="cellular-degraded"
    last_message="4G unhealthy and no healthy wired interface is available; keeping 4G as the last-resort route"
    [ "$should_log" -eq 0 ] || log_message "$last_message"
}

check_once() {
    load_state
    last_checked=$(date -u '+%Y-%m-%dT%H:%M:%SZ')

    if ! truthy "$FAILOVER_ENABLED"; then
        mode="disabled"
        failures=0
        recoveries=0
        last_result="disabled"
        last_message="network failover is disabled by runtime configuration"
        save_state
        return 0
    fi

    if ! truthy "$PREFER_CELLULAR"; then
        local wired_interface
        last_result="not-preferred"
        failures=0
        recoveries=0
        wired_interface=$(select_healthy_wired_interface || true)
        if [ -n "$wired_interface" ]; then
            set_wired_fallback "$wired_interface" "runtime configuration selects the wired route as primary" || true
        else
            last_message="no healthy wired interface is available"
        fi
        save_state
        return 0
    fi

    if probe_cellular; then
        last_result="healthy"
        failures=0
        recoveries=$((recoveries + 1))
        if [ "$recoveries" -ge "$RECOVERY_THRESHOLD" ]; then
            set_cellular_primary || true
        else
            last_message="4G recovery confirmation $recoveries/$RECOVERY_THRESHOLD"
        fi
    else
        last_result="unhealthy"
        recoveries=0
        failures=$((failures + 1))
        if [ "$failures" -ge "$FAILURE_THRESHOLD" ]; then
            local wired_interface
            wired_interface=$(select_healthy_wired_interface || true)
            if [ -n "$wired_interface" ]; then
                set_wired_fallback "$wired_interface" || true
            else
                keep_cellular_as_last_resort || true
            fi
        else
            last_message="4G failure confirmation $failures/$FAILURE_THRESHOLD"
        fi
    fi

    save_state
}

print_status() {
    load_state
    printf 'enabled=%s\n' "$FAILOVER_ENABLED"
    printf 'preferCellular=%s\n' "$PREFER_CELLULAR"
    printf 'runtimeConfig=%s\n' "$RUNTIME_CONFIG_FILE"
    printf 'wiredCandidates=%s\n' "$(wired_candidates_csv)"
    printf 'selectedWiredInterface=%s\n' "$selected_wired_interface"
    printf 'mode=%s\n' "$mode"
    printf 'lastResult=%s\n' "$last_result"
    printf 'failures=%s\n' "$failures"
    printf 'recoveries=%s\n' "$recoveries"
    printf 'lastChecked=%s\n' "$last_checked"
    printf 'message=%s\n' "$last_message"
    ip -4 route show default
}

run_loop() {
    if ! truthy "$FAILOVER_ENABLED"; then
        check_once
        log_message "network failover monitor is disabled by runtime configuration"
        return 0
    fi
    log_message "network failover monitor started: cellular=$CELLULAR_INTERFACE wired=$(wired_candidates_csv) probe=$PROBE_HOST:$PROBE_PORT"
    while true; do
        check_once
        sleep "$CHECK_INTERVAL_SEC"
    done
}

trap remove_probe_route EXIT
trap 'remove_probe_route; exit 0' INT TERM
validate_config || exit $?

case "${1:-run}" in
    run)
        run_loop
        ;;
    once)
        check_once
        print_status
        ;;
    status)
        print_status
        ;;
    *)
        printf 'Usage: %s [run|once|status]\n' "$0" >&2
        exit 2
        ;;
esac
