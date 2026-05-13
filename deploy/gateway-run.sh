#!/bin/sh
set -eu

GATEWAY_HOME="${GATEWAY_HOME:-/opt/modbus-gateway}"
BIN_DIR="$GATEWAY_HOME/bin"
APP_CONFIG="${APP_CONFIG:-$GATEWAY_HOME/config/runtime/apps/mqtt-service.json}"

usage() {
  cat <<'EOF'
Usage: gateway-run.sh <command>

Commands:
  list        Show services that should run from current config
  start       Start configured gateway services
  stop        Stop configured gateway services
  restart     Restart configured gateway services
  status      Show systemd service status
  logs        Follow gateway service logs
  snapshot    Print shared-memory point snapshot through pointctl
  pending     Print pending write queue through pointctl
  stats       Print shared-memory capacity and queue stats
  health      Check desired services, shared memory and disk usage
  smoke       Run production smoke checks
  cleanup     Stop services, remove gateway shm and temp stress dirs
EOF
}

cmd="${1:-}"
case "$cmd" in
  list)
    "$BIN_DIR/gateway-services.sh" list
    ;;
  start)
    if command -v systemctl >/dev/null 2>&1; then
      systemctl start gateway-services.service
    else
      "$BIN_DIR/gateway-services.sh" start
    fi
    ;;
  stop)
    if command -v systemctl >/dev/null 2>&1; then
      systemctl stop gateway-services.service
    else
      "$BIN_DIR/gateway-services.sh" stop
    fi
    ;;
  restart)
    if command -v systemctl >/dev/null 2>&1; then
      systemctl restart gateway-services.service
    else
      "$BIN_DIR/gateway-services.sh" restart
    fi
    ;;
  status)
    systemctl status gateway-services.service 'modbus-rtu@*.service' 'dlt645-driver@*.service' 'dio-driver@*.service' 'can-driver@*.service' 'compute-engine@*.service' 'event-engine@*.service' 'local-display@*.service' 'local-kiosk@*.service' 'camera-service@*.service' 'mqtt-driver@*.service' 'system-monitor@*.service' 'mqtt-tls-tunnel@*.service' --no-pager || true
    ;;
  logs)
    journalctl -u gateway-services.service -u 'modbus-rtu@*.service' -u 'dlt645-driver@*.service' -u 'dio-driver@*.service' -u 'can-driver@*.service' -u 'compute-engine@*.service' -u 'event-engine@*.service' -u 'local-display@*.service' -u 'local-kiosk@*.service' -u 'camera-service@*.service' -u 'mqtt-driver@*.service' -u 'system-monitor@*.service' -u 'mqtt-tls-tunnel@*.service' -f
    ;;
  snapshot)
    "$BIN_DIR/pointctl" snapshot --app-config "$APP_CONFIG"
    ;;
  pending)
    "$BIN_DIR/pointctl" pending --app-config "$APP_CONFIG"
    ;;
  stats)
    "$BIN_DIR/pointctl" stats --app-config "$APP_CONFIG"
    ;;
  health)
    rc=0
    if command -v systemctl >/dev/null 2>&1; then
      for unit in $("$BIN_DIR/gateway-services.sh" list); do
        [ -z "$unit" ] && continue
        case "$unit" in \#*) continue ;; esac
        if systemctl is-active --quiet "$unit"; then
          echo "service $unit active"
        else
          echo "service $unit not-active" >&2
          rc=1
        fi
      done
    fi
    "$BIN_DIR/pointctl" stats --app-config "$APP_CONFIG" || rc=1
    df -h "$GATEWAY_HOME" || true
    exit "$rc"
    ;;
  smoke)
    "$BIN_DIR/production-smoke-test.sh"
    ;;
  cleanup)
    if command -v systemctl >/dev/null 2>&1; then
      systemctl stop gateway-services.service || true
      "$BIN_DIR/gateway-services.sh" stop || true
    else
      "$BIN_DIR/gateway-services.sh" stop || true
    fi
    rm -f /dev/shm/gateway_point_store* 2>/dev/null || true
    find /tmp -maxdepth 1 -type d \( -name 'gateway-stress-*' -o -name 'gateway-ci-*' \) -exec rm -rf -- {} + 2>/dev/null || true
    echo "gateway runtime cache cleaned"
    ;;
  -h|--help|help|"")
    usage
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac
