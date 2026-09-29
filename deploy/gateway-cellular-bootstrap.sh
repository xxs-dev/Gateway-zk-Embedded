#!/bin/sh
set -eu

INSTALL_PATH="${INSTALL_PATH:-/opt/modbus-gateway/bin/gateway-cellular.sh}"
SERVICE_PATH="${SERVICE_PATH:-/etc/systemd/system/gateway-cellular.service}"
CONFIG_FILE="${CONFIG_FILE:-/etc/default/gateway-network-failover}"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RUNTIME_SOURCE="${CELLULAR_RUNTIME_SOURCE:-$SCRIPT_DIR/gateway-cellular.sh}"

require_root() {
  if [ "$(id -u)" -ne 0 ]; then
    echo "This command must run as root." >&2
    exit 1
  fi
}

require_runtime_source() {
  if [ ! -f "$RUNTIME_SOURCE" ]; then
    echo "Cellular runtime script not found: $RUNTIME_SOURCE" >&2
    echo "Keep gateway-cellular-bootstrap.sh and gateway-cellular.sh in the same directory." >&2
    exit 2
  fi
}

install_service() {
  require_root
  requested_interface="${1:-${CELLULAR_INTERFACE:-usb0}}"
  require_runtime_source

  mkdir -p "$(dirname "$INSTALL_PATH")" "$(dirname "$CONFIG_FILE")"
  source_path=$(readlink -f "$RUNTIME_SOURCE" 2>/dev/null || printf '%s\n' "$RUNTIME_SOURCE")
  target_path=$(readlink -f "$INSTALL_PATH" 2>/dev/null || printf '%s\n' "$INSTALL_PATH")
  if [ "$source_path" != "$target_path" ]; then
    install -m 0755 "$RUNTIME_SOURCE" "$INSTALL_PATH"
  else
    chmod 0755 "$INSTALL_PATH"
  fi

  if [ ! -f "$CONFIG_FILE" ]; then
    {
      printf 'CELLULAR_INTERFACE=%s\n' "$requested_interface"
      printf 'CELLULAR_INTERFACE_WAIT_SEC=90\n'
      printf 'CELLULAR_INTERFACE_STABLE_SEC=10\n'
      printf 'CELLULAR_MONITOR_INTERVAL_SEC=1\n'
      printf 'CELLULAR_ADDRESS_WAIT_SEC=30\n'
      printf 'CELLULAR_DHCP_RESTART_DELAY_SEC=2\n'
      printf 'CELLULAR_DHCP_STOP_TIMEOUT_SEC=5\n'
      printf 'CELLULAR_DHCP_SCRIPT=/etc/udhcpc/default.script\n'
    } >"$CONFIG_FILE"
    chmod 0644 "$CONFIG_FILE"
  fi

  cat >"$SERVICE_PATH" <<EOF
[Unit]
Description=Gateway persistent cellular DHCP bootstrap
After=systemd-udev-settle.service
Wants=systemd-udev-settle.service
Before=gateway-network-failover.service
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
    require_runtime_source
    exec bash "$RUNTIME_SOURCE" run
    ;;
  status)
    require_runtime_source
    exec bash "$RUNTIME_SOURCE" status
    ;;
  *)
    echo "Usage: $0 [install [interface]|run|status]" >&2
    exit 2
    ;;
esac
