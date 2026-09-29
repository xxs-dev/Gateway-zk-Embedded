#!/usr/bin/env bash
if [ -z "${BASH_VERSION:-}" ]; then
  exec bash "$0" "$@"
fi
set -euo pipefail

CONFIG_FILE="${CONFIG_FILE:-/etc/default/gateway-network-failover}"
RUNTIME_CONFIG_FILE="${RUNTIME_CONFIG_FILE:-/opt/modbus-gateway/config/runtime/apps/monitor-service.json}"

if [ -f "$CONFIG_FILE" ]; then
  # shellcheck disable=SC1090
  . "$CONFIG_FILE"
fi

load_runtime_interface() {
  command -v python3 >/dev/null 2>&1 || return 0
  [ -f "$RUNTIME_CONFIG_FILE" ] || return 0
  local value
  value=$(python3 - "$RUNTIME_CONFIG_FILE" 2>/dev/null <<'PY'
import json
import sys

try:
    with open(sys.argv[1], "r", encoding="utf-8") as handle:
        root = json.load(handle)
    config = (((root.get("systemMonitor") or {}).get("cellular") or {}).get("routeFailover") or {})
    value = str(config.get("cellularInterface") or "").strip()
    if value:
        print(value)
except Exception:
    pass
PY
  )
  [ -z "$value" ] || CELLULAR_INTERFACE="$value"
}

load_runtime_interface

: "${CELLULAR_INTERFACE:=usb0}"
: "${CELLULAR_INTERFACE_WAIT_SEC:=60}"
: "${CELLULAR_INTERFACE_STABLE_SEC:=10}"
: "${CELLULAR_MONITOR_INTERVAL_SEC:=1}"
: "${CELLULAR_ADDRESS_WAIT_SEC:=30}"
: "${CELLULAR_DHCP_RESTART_DELAY_SEC:=2}"
: "${CELLULAR_DHCP_STOP_TIMEOUT_SEC:=5}"
: "${CELLULAR_DHCP_SCRIPT:=/etc/udhcpc/default.script}"
: "${CELLULAR_DHCP_PID_FILE:=/run/udhcpc-${CELLULAR_INTERFACE}.pid}"
: "${SYS_CLASS_NET_ROOT:=/sys/class/net}"
: "${PROC_ROOT:=/proc}"

DHCP_PID=""
DHCP_START_TIME=""
CELLULAR_INTERFACE_INSTANCE=""

log_message() {
  local message="$1"
  printf '%s\n' "$message"
  command -v logger >/dev/null 2>&1 && logger -t gateway-cellular -- "$message" || true
}

positive_integer() {
  [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

validate_config() {
  local value
  for value in "$CELLULAR_INTERFACE_WAIT_SEC" "$CELLULAR_INTERFACE_STABLE_SEC" \
    "$CELLULAR_MONITOR_INTERVAL_SEC" "$CELLULAR_ADDRESS_WAIT_SEC" \
    "$CELLULAR_DHCP_RESTART_DELAY_SEC" "$CELLULAR_DHCP_STOP_TIMEOUT_SEC"; do
    if ! positive_integer "$value"; then
      log_message "invalid positive integer in cellular configuration: $value"
      return 2
    fi
  done
  if [ "$CELLULAR_INTERFACE_STABLE_SEC" -gt "$CELLULAR_INTERFACE_WAIT_SEC" ]; then
    log_message "CELLULAR_INTERFACE_STABLE_SEC must not exceed CELLULAR_INTERFACE_WAIT_SEC"
    return 2
  fi
}

interface_instance() {
  local path="$SYS_CLASS_NET_ROOT/$CELLULAR_INTERFACE/ifindex"
  local value
  [ -r "$path" ] || return 1
  read -r value < "$path" || return 1
  [[ "$value" =~ ^[1-9][0-9]*$ ]] || return 1
  printf '%s\n' "$value"
}

interface_ipv4() {
  ip -4 -o address show dev "$CELLULAR_INTERFACE" scope global 2>/dev/null |
    awk 'NR == 1 { split($4, address, "/"); print address[1] }'
}

wait_for_stable_interface() {
  local elapsed=0
  local stable_elapsed=0
  local observed_instance=""
  local current_instance
  while [ "$elapsed" -le "$CELLULAR_INTERFACE_WAIT_SEC" ]; do
    current_instance=$(interface_instance || true)
    if [ -n "$current_instance" ] && ip link show dev "$CELLULAR_INTERFACE" >/dev/null 2>&1; then
      if [ "$current_instance" = "$observed_instance" ]; then
        stable_elapsed=$((stable_elapsed + 1))
      else
        if [ -n "$observed_instance" ]; then
          log_message "cellular interface instance changed while waiting: interface=$CELLULAR_INTERFACE oldIfindex=$observed_instance newIfindex=$current_instance"
        fi
        observed_instance="$current_instance"
        stable_elapsed=0
      fi
      if [ "$stable_elapsed" -ge "$CELLULAR_INTERFACE_STABLE_SEC" ]; then
        CELLULAR_INTERFACE_INSTANCE="$current_instance"
        log_message "cellular interface is stable: interface=$CELLULAR_INTERFACE ifindex=$current_instance stableSec=$stable_elapsed"
        return 0
      fi
    else
      observed_instance=""
      stable_elapsed=0
    fi
    [ "$elapsed" -lt "$CELLULAR_INTERFACE_WAIT_SEC" ] || break
    sleep 1
    elapsed=$((elapsed + 1))
  done
  log_message "cellular interface did not remain stable for ${CELLULAR_INTERFACE_STABLE_SEC}s within ${CELLULAR_INTERFACE_WAIT_SEC}s: $CELLULAR_INTERFACE"
  return 1
}

pid_file_value() {
  local pid
  [ -s "$CELLULAR_DHCP_PID_FILE" ] || return 1
  pid=$(cat "$CELLULAR_DHCP_PID_FILE" 2>/dev/null || true)
  [[ "$pid" =~ ^[1-9][0-9]*$ ]] || return 1
  printf '%s\n' "$pid"
}

pid_file_is_live() {
  local pid
  pid=$(pid_file_value || true)
  [ -n "$pid" ] && capture_cellular_client_start_time "$pid" >/dev/null
}

process_start_time() {
  local pid="$1"
  local stat_line stat_fields
  [ -r "$PROC_ROOT/$pid/stat" ] || return 1
  IFS= read -r stat_line < "$PROC_ROOT/$pid/stat" || return 1
  case "$stat_line" in
    *') '*) stat_fields=${stat_line##*) } ;;
    *) return 1 ;;
  esac
  set -- $stat_fields
  [ "$#" -ge 20 ] || return 1
  [[ "${20}" =~ ^[0-9]+$ ]] || return 1
  printf '%s\n' "${20}"
}

process_state() {
  local pid="$1"
  local stat_line stat_fields
  [ -r "$PROC_ROOT/$pid/stat" ] || return 1
  IFS= read -r stat_line < "$PROC_ROOT/$pid/stat" || return 1
  case "$stat_line" in
    *') '*) stat_fields=${stat_line##*) } ;;
    *) return 1 ;;
  esac
  set -- $stat_fields
  [ "$#" -ge 1 ] || return 1
  printf '%s\n' "$1"
}

pid_matches_cellular_client() {
  local pid="$1"
  local command_line
  [ -r "$PROC_ROOT/$pid/cmdline" ] || return 1
  command_line=$(tr '\0' ' ' < "$PROC_ROOT/$pid/cmdline" 2>/dev/null || true)
  case "$command_line" in
    *udhcpc*) ;;
    *) return 1 ;;
  esac
  case " $command_line " in
    *" -i $CELLULAR_INTERFACE "*) return 0 ;;
    *) return 1 ;;
  esac
}

capture_cellular_client_start_time() {
  local pid="$1"
  local before after
  before=$(process_start_time "$pid" || true)
  [ -n "$before" ] || return 1
  pid_matches_cellular_client "$pid" || return 1
  after=$(process_start_time "$pid" || true)
  [ -n "$after" ] && [ "$before" = "$after" ] || return 1
  printf '%s\n' "$after"
}

cellular_client_identity_matches() {
  local pid="$1"
  local expected_start_time="$2"
  local current_start_time
  current_start_time=$(capture_cellular_client_start_time "$pid" || true)
  [ -n "$current_start_time" ] && [ "$current_start_time" = "$expected_start_time" ]
}

dhcp_process_identity_matches() {
  local pid="$1"
  local expected_start_time="$2"
  local owned_child="${3:-0}"
  local current_start_time current_state
  current_start_time=$(process_start_time "$pid" || true)
  [ -n "$current_start_time" ] && [ "$current_start_time" = "$expected_start_time" ] || return 1
  current_state=$(process_state "$pid" || true)
  [ -n "$current_state" ] && [ "$current_state" != "Z" ] || return 1
  [ "$owned_child" = "1" ] || pid_matches_cellular_client "$pid"
}

capture_process_start_time() {
  local pid="$1"
  local attempt=0
  local start_time
  while [ "$attempt" -lt 10 ]; do
    start_time=$(process_start_time "$pid" || true)
    if [ -n "$start_time" ]; then
      printf '%s\n' "$start_time"
      return 0
    fi
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
    attempt=$((attempt + 1))
  done
  return 1
}

remove_pid_file_for() {
  local expected_pid="$1"
  local recorded_pid
  recorded_pid=$(pid_file_value || true)
  if [ -z "$recorded_pid" ] || [ "$recorded_pid" = "$expected_pid" ]; then
    rm -f "$CELLULAR_DHCP_PID_FILE"
  fi
}

stop_dhcp_pid() {
  local pid="$1"
  local expected_start_time="${2:-}"
  local owned_child="${3:-0}"
  local elapsed=0
  if ! kill -0 "$pid" 2>/dev/null; then
    if [ "$owned_child" = "1" ]; then
      wait "$pid" 2>/dev/null || true
    fi
    remove_pid_file_for "$pid"
    return 0
  fi

  if [ "$owned_child" = "1" ] && [ "$(process_state "$pid" || true)" = "Z" ]; then
    wait "$pid" 2>/dev/null || true
    remove_pid_file_for "$pid"
    return 0
  fi

  if [ -z "$expected_start_time" ]; then
    expected_start_time=$(capture_cellular_client_start_time "$pid" || true)
  fi
  if [ -z "$expected_start_time" ] || ! dhcp_process_identity_matches "$pid" "$expected_start_time" "$owned_child"; then
    log_message "DHCP pid identity changed before termination; refusing to signal: pid=$pid"
    return 0
  fi

  kill "$pid" 2>/dev/null || true
  while [ "$elapsed" -lt "$CELLULAR_DHCP_STOP_TIMEOUT_SEC" ]; do
    kill -0 "$pid" 2>/dev/null || break
    if ! dhcp_process_identity_matches "$pid" "$expected_start_time" "$owned_child"; then
      log_message "DHCP pid identity changed after TERM; refusing to send another signal: pid=$pid"
      break
    fi
    sleep 1
    elapsed=$((elapsed + 1))
  done
  if kill -0 "$pid" 2>/dev/null && dhcp_process_identity_matches "$pid" "$expected_start_time" "$owned_child"; then
    log_message "DHCP client did not stop in time; forcing termination: pid=$pid"
    kill -KILL "$pid" 2>/dev/null || true
  fi
  if [ "$owned_child" = "1" ]; then
    wait "$pid" 2>/dev/null || true
  fi
  remove_pid_file_for "$pid"
}

stop_existing_client() {
  local pid
  pid=$(pid_file_value || true)
  if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
    rm -f "$CELLULAR_DHCP_PID_FILE"
    return 0
  fi
  local start_time
  start_time=$(capture_cellular_client_start_time "$pid" || true)
  if [ -n "$start_time" ]; then
    log_message "stopping pre-existing DHCP client before supervision: interface=$CELLULAR_INTERFACE pid=$pid"
    stop_dhcp_pid "$pid" "$start_time" 0
  else
    log_message "ignoring stale DHCP pid file owned by another process: file=$CELLULAR_DHCP_PID_FILE pid=$pid"
    rm -f "$CELLULAR_DHCP_PID_FILE"
  fi
}

cleanup_supervisor() {
  trap - EXIT INT TERM
  if [ -n "$DHCP_PID" ]; then
    stop_dhcp_pid "$DHCP_PID" "$DHCP_START_TIME" 1
  fi
  DHCP_PID=""
  DHCP_START_TIME=""
}

run_client() {
  if [ "$(id -u)" -ne 0 ]; then
    log_message "gateway cellular supervisor must run as root"
    return 1
  fi
  validate_config

  command -v ip >/dev/null 2>&1 || {
    log_message "ip command is not installed"
    return 1
  }
  command -v udhcpc >/dev/null 2>&1 || {
    log_message "udhcpc is not installed"
    return 1
  }
  [ -x "$CELLULAR_DHCP_SCRIPT" ] || {
    log_message "udhcpc event script is not executable: $CELLULAR_DHCP_SCRIPT"
    return 1
  }

  stop_existing_client
  trap cleanup_supervisor EXIT
  trap 'exit 0' INT TERM

  while true; do
    wait_for_stable_interface || return 1
    if ! ip link set dev "$CELLULAR_INTERFACE" up; then
      log_message "failed to bring cellular interface up: interface=$CELLULAR_INTERFACE ifindex=$CELLULAR_INTERFACE_INSTANCE"
      sleep "$CELLULAR_DHCP_RESTART_DELAY_SEC"
      continue
    fi

    rm -f "$CELLULAR_DHCP_PID_FILE"
    log_message "starting supervised DHCP client: interface=$CELLULAR_INTERFACE ifindex=$CELLULAR_INTERFACE_INSTANCE script=$CELLULAR_DHCP_SCRIPT"
    udhcpc -f -i "$CELLULAR_INTERFACE" -s "$CELLULAR_DHCP_SCRIPT" \
      -p "$CELLULAR_DHCP_PID_FILE" -S &
    DHCP_PID=$!
    DHCP_START_TIME=$(capture_process_start_time "$DHCP_PID" || true)
    if [ -z "$DHCP_START_TIME" ]; then
      log_message "failed to capture DHCP child identity; leaving cleanup to the service cgroup: pid=$DHCP_PID"
      return 1
    fi

    local restart_reason="DHCP client exited"
    local address_missing_sec=0
    local current_instance
    while kill -0 "$DHCP_PID" 2>/dev/null; do
      sleep "$CELLULAR_MONITOR_INTERVAL_SEC"
      current_instance=$(interface_instance || true)
      if [ -z "$current_instance" ]; then
        restart_reason="cellular interface disappeared"
        break
      fi
      if [ "$current_instance" != "$CELLULAR_INTERFACE_INSTANCE" ]; then
        restart_reason="cellular interface was recreated (oldIfindex=$CELLULAR_INTERFACE_INSTANCE newIfindex=$current_instance)"
        break
      fi
      if [ -n "$(interface_ipv4)" ]; then
        address_missing_sec=0
      else
        address_missing_sec=$((address_missing_sec + CELLULAR_MONITOR_INTERVAL_SEC))
        if [ "$address_missing_sec" -ge "$CELLULAR_ADDRESS_WAIT_SEC" ]; then
          restart_reason="cellular interface has no IPv4 address for ${address_missing_sec}s"
          break
        fi
      fi
    done

    if kill -0 "$DHCP_PID" 2>/dev/null; then
      log_message "$restart_reason; restarting DHCP supervision"
      stop_dhcp_pid "$DHCP_PID" "$DHCP_START_TIME" 1
    else
      if wait "$DHCP_PID"; then
        log_message "DHCP client exited normally; restarting supervision"
      else
        local status=$?
        log_message "DHCP client exited with status $status; restarting supervision"
      fi
      remove_pid_file_for "$DHCP_PID"
    fi
    DHCP_PID=""
    DHCP_START_TIME=""
    CELLULAR_INTERFACE_INSTANCE=""
    sleep "$CELLULAR_DHCP_RESTART_DELAY_SEC"
  done
}

show_status() {
  printf 'interface=%s\n' "$CELLULAR_INTERFACE"
  printf 'ifindex=%s\n' "$(interface_instance || printf 'none')"
  ip -br link show dev "$CELLULAR_INTERFACE" 2>/dev/null || true
  ip -br address show dev "$CELLULAR_INTERFACE" 2>/dev/null || true
  ip -4 route show default dev "$CELLULAR_INTERFACE" 2>/dev/null || true
  if pid_file_is_live; then
    printf 'dhcpPid=%s\n' "$(cat "$CELLULAR_DHCP_PID_FILE")"
    return 0
  fi
  printf 'dhcpPid=none\n'
  return 1
}

if [ "${GATEWAY_CELLULAR_SOURCE_ONLY:-0}" != "1" ]; then
  case "${1:-run}" in
    run) run_client ;;
    status) show_status ;;
    *)
      echo "Usage: gateway-cellular.sh [run|status]" >&2
      exit 2
      ;;
  esac
fi
