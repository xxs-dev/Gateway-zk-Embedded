#!/bin/sh
# gateway-services.sh queue actions (pending-write queue maintenance window): stop-writers,
# stop-drivers and start-queue touch only queue writers and drivers, never network, 4G, EasyTier,
# the MQTT TLS tunnel or read-only services; the watchdog stays suspended until start-queue succeeds.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CASE=$(mktemp -d)
trap 'rm -rf "$CASE"' EXIT HUP INT TERM
mkdir -p "$CASE/bin" "$CASE/config/runtime/apps" "$CASE/config/runtime/devices"
export GATEWAY_HOME="$CASE" WATCHDOG_RUN_DIR="$CASE/run" SYSTEMCTL_LOG="$CASE/actions"
# Units the fake systemd has loaded: queue writers/drivers plus everything that must not be touched.
export FAKE_UNITS="modbus-rtu@device_a.service dlt645-driver@device_b.service \
mqtt-driver@mqtt-service.service mqtt-forwarder@mqtt-service.service compute-engine@mqtt-service.service \
system-monitor@monitor-service.service ky-ems.service qt-display-bridge.service \
event-engine@mqtt-service.service mqtt-tls-tunnel@mqtt-service.service camera-service@camera-service.service \
gateway-cellular.service gateway-network-failover.service easytier.service gateway-health-watchdog.service"
cat > "$CASE/bin/systemctl" <<'SH'
#!/bin/sh
if [ "$1" = list-units ]; then
  shift
  for unit in $FAKE_UNITS; do
    for pattern in "$@"; do
      case "$pattern" in --*) continue ;; esac
      case "$unit" in $pattern) printf '%s loaded active running fake\n' "$unit"; break ;; esac
    done
  done
  exit 0
fi
printf '%s\n' "$*" >> "$SYSTEMCTL_LOG"
SH
chmod +x "$CASE/bin/systemctl"
export PATH="$CASE/bin:$PATH"
cat > "$CASE/config/runtime/apps/mqtt-service.json" <<'JSON'
{"mqtt":{"enabled":true,"broker":"tcp://10.0.0.1:1883"},"mqttDriver":{"enabled":true},
 "computeEngine":{"enabled":true},"eventEngine":{"enabled":true},"mqttForward":{"enabled":true},
 "deviceConfigFiles":["../devices/device_a.json","../devices/device_b.json"]}
JSON
cat > "$CASE/config/runtime/apps/monitor-service.json" <<'JSON'
{"systemMonitor":{"enabled":true,"cellular":{"routeFailover":{"enabled":true}}},
 "localDisplay":{"enabled":true,"renderer":"nativeQt"}}
JSON
printf '{"protocol":{"type":"modbus_rtu"}}\n' > "$CASE/config/runtime/devices/device_a.json"
printf '{"protocol":{"type":"dlt645_2007"}}\n' > "$CASE/config/runtime/devices/device_b.json"

fail() { echo "gateway_services_queue_test: $*" >&2; cat "$SYSTEMCTL_LOG" >&2 2>/dev/null || true; exit 1; }
untouched() {
  for unit in gateway-cellular.service gateway-network-failover.service easytier.service \
      mqtt-tls-tunnel@mqtt-service.service event-engine@mqtt-service.service \
      camera-service@camera-service.service gateway-health-watchdog.service; do
    if grep -F "$unit" "$SYSTEMCTL_LOG" >/dev/null 2>&1; then fail "$1 touched $unit"; fi
  done
}

: > "$SYSTEMCTL_LOG"
sh "$ROOT/deploy/gateway-services.sh" stop-writers
stopped_set() { sed -n 's/^stop //p' "$SYSTEMCTL_LOG" | tr ' ' '\n' | sed '/^$/d' | sort | tr '\n' ' '; }
expected_writers=$(printf '%s\n' mqtt-driver@mqtt-service.service mqtt-forwarder@mqtt-service.service \
  system-monitor@monitor-service.service compute-engine@mqtt-service.service ky-ems.service \
  qt-display-bridge.service | sort | tr '\n' ' ')
[ "$(stopped_set)" = "$expected_writers" ] || fail "stop-writers must stop exactly the queue writers"
grep -F "modbus-rtu@" "$SYSTEMCTL_LOG" >/dev/null && fail "stop-writers must leave the drivers running (they drain the queue)"
untouched stop-writers
[ -f "$CASE/run/manual-stop" ] || fail "stop-writers must suspend the watchdog"
grep -F "disable" "$SYSTEMCTL_LOG" >/dev/null && fail "queue stops must not disable units"

: > "$SYSTEMCTL_LOG"
sh "$ROOT/deploy/gateway-services.sh" stop-drivers
[ "$(stopped_set)" = "$(printf '%s\n' modbus-rtu@device_a.service dlt645-driver@device_b.service | sort | tr '\n' ' ')" ] ||
  fail "stop-drivers must stop exactly the drivers"
untouched stop-drivers

: > "$SYSTEMCTL_LOG"
sh "$ROOT/deploy/gateway-services.sh" start-queue
untouched start-queue
first_writer=$(grep -n "^start mqtt-driver@\|^start compute-engine@\|^start system-monitor@\|ky-ems.service" "$SYSTEMCTL_LOG" | head -1 | cut -d: -f1)
last_driver=$(grep -n "^start modbus-rtu@\|^start dlt645-driver@" "$SYSTEMCTL_LOG" | tail -1 | cut -d: -f1)
[ -n "$first_writer" ] && [ -n "$last_driver" ] && [ "$last_driver" -lt "$first_writer" ] ||
  fail "start-queue must start every driver before any writer"
for unit in modbus-rtu@device_a.service dlt645-driver@device_b.service mqtt-driver@mqtt-service.service \
    mqtt-forwarder@mqtt-service.service compute-engine@mqtt-service.service system-monitor@monitor-service.service; do
  grep -Fx "start $unit" "$SYSTEMCTL_LOG" >/dev/null || fail "start-queue did not start $unit"
done
grep -Fx "start --no-block ky-ems.service" "$SYSTEMCTL_LOG" >/dev/null || fail "start-queue did not start ky-ems.service"
[ -f "$CASE/run/manual-stop" ] && fail "a successful start-queue must resume the watchdog"

sh "$ROOT/deploy/gateway-services.sh" list-queue > "$CASE/list"
grep -Fx "mqtt-driver@mqtt-service.service" "$CASE/list" >/dev/null || fail "list-queue misses a writer"
grep -Fx "modbus-rtu@device_a.service" "$CASE/list" >/dev/null || fail "list-queue misses a driver"
grep -F "cellular" "$CASE/list" >/dev/null && fail "list-queue must not list network units"
echo 'gateway_services_queue_test passed'
