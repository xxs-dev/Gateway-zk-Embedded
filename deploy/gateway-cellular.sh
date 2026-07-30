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
: "${CELLULAR_DHCP_SCRIPT:=/etc/udhcpc/default.script}"
: "${CELLULAR_DHCP_PID_FILE:=/run/udhcpc-${CELLULAR_INTERFACE}.pid}"

log_message() {
  local message="$1"
  printf '%s\n' "$message"
  command -v logger >/dev/null 2>&1 && logger -t gateway-cellular -- "$message" || true
}

wait_for_interface() {
  local elapsed=0
  while ! ip link show dev "$CELLULAR_INTERFACE" >/dev/null 2>&1; do
    if [ "$elapsed" -ge "$CELLULAR_INTERFACE_WAIT_SEC" ]; then
      log_message "cellular interface not found after ${CELLULAR_INTERFACE_WAIT_SEC}s: $CELLULAR_INTERFACE"
      return 1
    fi
    sleep 1
    elapsed=$((elapsed + 1))
  done
}

pid_file_is_live() {
  [ -s "$CELLULAR_DHCP_PID_FILE" ] || return 1
  local pid
  pid=$(cat "$CELLULAR_DHCP_PID_FILE" 2>/dev/null || true)
  [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null
}

run_client() {
  wait_for_interface
  ip link set dev "$CELLULAR_INTERFACE" up

  if pid_file_is_live; then
    log_message "DHCP client already running for $CELLULAR_INTERFACE: pid=$(cat "$CELLULAR_DHCP_PID_FILE")"
    return 1
  fi
  rm -f "$CELLULAR_DHCP_PID_FILE"

  command -v udhcpc >/dev/null 2>&1 || {
    log_message "udhcpc is not installed"
    return 1
  }
  [ -x "$CELLULAR_DHCP_SCRIPT" ] || {
    log_message "udhcpc event script is not executable: $CELLULAR_DHCP_SCRIPT"
    return 1
  }

  log_message "starting persistent DHCP client: interface=$CELLULAR_INTERFACE script=$CELLULAR_DHCP_SCRIPT"
  exec udhcpc -f -i "$CELLULAR_INTERFACE" -s "$CELLULAR_DHCP_SCRIPT" \
    -p "$CELLULAR_DHCP_PID_FILE" -S
}

show_status() {
  printf 'interface=%s\n' "$CELLULAR_INTERFACE"
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

case "${1:-run}" in
  run) run_client ;;
  status) show_status ;;
  *)
    echo "Usage: gateway-cellular.sh [run|status]" >&2
    exit 2
    ;;
esac
