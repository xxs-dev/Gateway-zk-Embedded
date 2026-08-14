#!/bin/sh
set -eu

INSTALL_PATH="${INSTALL_PATH:-/opt/modbus-gateway/bin/gateway-cellular.sh}"
SERVICE_PATH="${SERVICE_PATH:-/etc/systemd/system/gateway-cellular.service}"
CONFIG_FILE="${CONFIG_FILE:-/etc/default/gateway-network-failover}"

load_config() {
  if [ -f "$CONFIG_FILE" ]; then
    # shellcheck disable=SC1090
    . "$CONFIG_FILE"
  fi
  CELLULAR_INTERFACE="${CELLULAR_INTERFACE:-usb0}"
  CELLULAR_INTERFACE_WAIT_SEC="${CELLULAR_INTERFACE_WAIT_SEC:-90}"
  CELLULAR_DHCP_PID_FILE="${CELLULAR_DHCP_PID_FILE:-/run/udhcpc-${CELLULAR_INTERFACE}.pid}"
}

log_message() {
  printf '%s\n' "$*"
  command -v logger >/dev/null 2>&1 && logger -t gateway-cellular -- "$*" || true
}

require_root() {
  if [ "$(id -u)" -ne 0 ]; then
    echo "This command must run as root." >&2
    exit 1
  fi
}

find_udhcpc_script() {
  if [ -n "${CELLULAR_DHCP_SCRIPT:-}" ] && [ -x "$CELLULAR_DHCP_SCRIPT" ]; then
    printf '%s\n' "$CELLULAR_DHCP_SCRIPT"
    return 0
  fi

  for candidate in \
    /etc/udhcpc/default.script \
    /usr/share/udhcpc/default.script \
    /usr/lib/udhcpc/default.script \
    /etc/udhcp/default.script; do
    if [ -x "$candidate" ]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

wait_for_interface() {
  elapsed=0
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
  pid=$(cat "$CELLULAR_DHCP_PID_FILE" 2>/dev/null || true)
  [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null
}

run_client() {
  require_root
  load_config
  command -v ip >/dev/null 2>&1 || {
    log_message "ip command is not installed"
    return 1
  }
  command -v udhcpc >/dev/null 2>&1 || {
    log_message "udhcpc is not installed; install BusyBox udhcpc first"
    return 1
  }

  dhcp_script=$(find_udhcpc_script) || {
    log_message "no executable udhcpc event script was found"
    return 1
  }

  wait_for_interface
  ip link set dev "$CELLULAR_INTERFACE" up

  if pid_file_is_live; then
    log_message "DHCP client already running for $CELLULAR_INTERFACE: pid=$pid"
    return 0
  fi
  rm -f "$CELLULAR_DHCP_PID_FILE"

  log_message "starting persistent cellular DHCP: interface=$CELLULAR_INTERFACE script=$dhcp_script"
  exec udhcpc -f -i "$CELLULAR_INTERFACE" -s "$dhcp_script" \
    -p "$CELLULAR_DHCP_PID_FILE" -S
}

show_status() {
  load_config
  printf 'interface=%s\n' "$CELLULAR_INTERFACE"
  ip -br link show dev "$CELLULAR_INTERFACE" 2>/dev/null || true
  ip -br address show dev "$CELLULAR_INTERFACE" 2>/dev/null || true
  ip -4 route show default dev "$CELLULAR_INTERFACE" 2>/dev/null || true
  if pid_file_is_live; then
    printf 'dhcpPid=%s\n' "$pid"
    return 0
  fi
  printf 'dhcpPid=none\n'
  return 1
}

install_service() {
  require_root
  requested_interface="${1:-${CELLULAR_INTERFACE:-usb0}}"

  mkdir -p "$(dirname "$INSTALL_PATH")" "$(dirname "$CONFIG_FILE")"
  source_path=$(readlink -f "$0" 2>/dev/null || printf '%s\n' "$0")
  target_path=$(readlink -f "$INSTALL_PATH" 2>/dev/null || printf '%s\n' "$INSTALL_PATH")
  if [ "$source_path" != "$target_path" ]; then
    install -m 0755 "$0" "$INSTALL_PATH"
  else
    chmod 0755 "$INSTALL_PATH"
  fi

  if [ ! -f "$CONFIG_FILE" ]; then
    {
      printf 'CELLULAR_INTERFACE=%s\n' "$requested_interface"
      printf 'CELLULAR_INTERFACE_WAIT_SEC=90\n'
      printf 'CELLULAR_DHCP_SCRIPT=/etc/udhcpc/default.script\n'
    } >"$CONFIG_FILE"
    chmod 0644 "$CONFIG_FILE"
  fi

  cat >"$SERVICE_PATH" <<EOF
[Unit]
Description=Gateway persistent cellular DHCP bootstrap
After=systemd-udev-settle.service
Wants=systemd-udev-settle.service
StartLimitIntervalSec=0

[Service]
Type=simple
ExecStart=$INSTALL_PATH run
Restart=always
RestartSec=5
User=root

[Install]
WantedBy=multi-user.target
EOF
  chmod 0644 "$SERVICE_PATH"

  command -v systemctl >/dev/null 2>&1 || {
    echo "systemd is not available; run '$INSTALL_PATH run' manually." >&2
    exit 1
  }
  systemctl daemon-reload
  systemctl enable --now gateway-cellular.service
  echo "Installed gateway-cellular.service for interface $requested_interface"
  echo "Status: $INSTALL_PATH status"
  echo "Logs: journalctl -u gateway-cellular.service -f"
}

case "${1:-install}" in
  install)
    install_service "${2:-}"
    ;;
  run)
    run_client
    ;;
  status)
    show_status
    ;;
  *)
    echo "Usage: $0 [install [interface]|run|status]" >&2
    exit 2
    ;;
esac
