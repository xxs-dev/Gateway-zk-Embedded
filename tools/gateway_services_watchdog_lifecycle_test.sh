#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
SCRIPT="$ROOT_DIR/deploy/gateway-services.sh"
CASE_DIR=$(mktemp -d)
trap 'rm -rf "$CASE_DIR"' EXIT HUP INT TERM

fail() {
  echo "[FAIL] $*" >&2
  exit 1
}

assert_contains() {
  grep -F "$2" "$1" >/dev/null 2>&1 || fail "$1 does not contain: $2"
}

mkdir -p "$CASE_DIR/bin" "$CASE_DIR/config/runtime/apps" "$CASE_DIR/config/runtime/devices" "$CASE_DIR/run"

cat > "$CASE_DIR/config/runtime/devices/device_first.json" <<'EOF'
{"protocol":{"type":"modbus_tcp"}}
EOF
cat > "$CASE_DIR/config/runtime/devices/device_second.json" <<'EOF'
{"protocol":{"type":"modbus_tcp"}}
EOF
cat > "$CASE_DIR/config/runtime/apps/mqtt-service.json" <<'EOF'
{
  "deviceConfigFiles":["../devices/device_first.json","../devices/device_second.json"],
  "mqtt":{"enabled":true},
  "mqttDriver":{"enabled":true}
}
EOF
cat > "$CASE_DIR/config/runtime/apps/monitor-service.json" <<'EOF'
{
  "deviceConfigFiles":["../devices/device_first.json","../devices/device_second.json"],
  "systemMonitor":{"enabled":true}
}
EOF

cat > "$CASE_DIR/bin/systemctl" <<'EOF'
#!/bin/sh
command=${1:-}
shift || true
case "$command" in
  list-units|list-unit-files)
    exit 0
    ;;
  start)
    [ "${1:-}" = "--no-block" ] && shift
    unit=${1:-}
    marker=missing
    [ -f "$WATCHDOG_RUN_DIR/applying" ] && marker=present
    echo "start $unit marker=$marker" >> "$ACTIONS"
    [ "$unit" != "${FAIL_UNIT:-}" ]
    ;;
  stop|disable|reset-failed)
    echo "$command $*" >> "$ACTIONS"
    ;;
  *)
    exit 0
    ;;
esac
EOF
chmod +x "$CASE_DIR/bin/systemctl"

export PATH="$CASE_DIR/bin:$PATH"
export GATEWAY_HOME="$CASE_DIR"
export WATCHDOG_RUN_DIR="$CASE_DIR/run"
export ACTIONS="$CASE_DIR/actions"
: > "$ACTIONS"
: > "$WATCHDOG_RUN_DIR/manual-stop"

FAIL_UNIT=modbus-rtu@device_first.service
export FAIL_UNIT
if sh "$SCRIPT" start; then
  fail "start should report the failed collector"
fi

[ ! -e "$WATCHDOG_RUN_DIR/manual-stop" ] || fail "start did not clear manual-stop marker"
[ ! -e "$WATCHDOG_RUN_DIR/applying" ] || fail "start left applying marker behind"
assert_contains "$ACTIONS" "start system-monitor@monitor-service.service marker=present"
assert_contains "$ACTIONS" "start mqtt-driver@mqtt-service.service marker=present"
assert_contains "$ACTIONS" "start modbus-rtu@device_first.service marker=present"
assert_contains "$ACTIONS" "start modbus-rtu@device_second.service marker=present"

first_collector_line=$(grep -n 'start modbus-rtu@device_first' "$ACTIONS" | cut -d: -f1)
second_collector_line=$(grep -n 'start modbus-rtu@device_second' "$ACTIONS" | cut -d: -f1)
monitor_line=$(grep -n 'start system-monitor@' "$ACTIONS" | cut -d: -f1)
mqtt_line=$(grep -n 'start mqtt-driver@' "$ACTIONS" | cut -d: -f1)
[ "$first_collector_line" -lt "$second_collector_line" ] || fail "configured collector order changed"
[ "$second_collector_line" -lt "$mqtt_line" ] || fail "configured service order changed"
[ "$mqtt_line" -lt "$monitor_line" ] || fail "configured maintenance service order changed"

unset FAIL_UNIT
sh "$SCRIPT" stop
[ -f "$WATCHDOG_RUN_DIR/manual-stop" ] || fail "stop did not create manual-stop marker"

echo "[PASS] gateway service/watchdog lifecycle tests"
