#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
WATCHDOG="$ROOT_DIR/deploy/gateway-health-watchdog.sh"
TMP_ROOT=$(mktemp -d)
trap 'rm -rf "$TMP_ROOT"' EXIT HUP INT TERM

fail() {
  echo "[FAIL] $*" >&2
  exit 1
}

assert_contains() {
  file="$1"
  expected="$2"
  grep -F "$expected" "$file" >/dev/null 2>&1 || fail "$file does not contain: $expected"
}

assert_not_contains() {
  file="$1"
  unexpected="$2"
  if grep -F "$unexpected" "$file" >/dev/null 2>&1; then
    fail "$file unexpectedly contains: $unexpected"
  fi
}

setup_case() {
  name="$1"
  CASE_DIR="$TMP_ROOT/$name"
  mkdir -p "$CASE_DIR/bin" "$CASE_DIR/state" "$CASE_DIR/run"
  : > "$CASE_DIR/actions"
  cat > "$CASE_DIR/gateway-services.sh" <<'EOF'
#!/bin/sh
if [ "${MOCK_LIST_FAIL:-0}" = "1" ]; then
  exit 1
fi
[ "${MOCK_LIST_BLOCK_SEC:-0}" -le 0 ] || sleep "$MOCK_LIST_BLOCK_SEC"
printf '%s\n' 'mqtt-driver@mqtt-service.service'
printf '%s\n' 'system-monitor@monitor-service.service'
printf '%s\n' 'modbus-rtu@device-test.service'
[ -z "${MOCK_EXTRA_UNITS:-}" ] || printf '%s\n' $MOCK_EXTRA_UNITS
EOF
  cat > "$CASE_DIR/bin/systemctl" <<'EOF'
#!/bin/sh
echo "$*" >> "$MOCK_ACTIONS"
case "$1" in
  show)
    echo "${MOCK_GATEWAY_STATE:-active}"
    ;;
  is-active)
    shift
    [ "${1:-}" = "--quiet" ] && shift
    unit="${1:-}"
    if [ "${MOCK_HEALTHY:-1}" = "1" ]; then
      exit 0
    fi
    if [ -f "$MOCK_CASE_DIR/recovered" ]; then
      exit 0
    fi
    case " ${MOCK_INACTIVE_UNITS:-} " in
      *" $unit "*) exit 3 ;;
      *) exit 0 ;;
    esac
    ;;
  restart)
    if [ "${MOCK_RECOVER_ON_RESTART:-0}" = "1" ]; then
      : > "$MOCK_CASE_DIR/recovered"
    fi
    [ "${MOCK_RESTART_FAIL:-0}" = "0" ]
    ;;
  start)
    if [ "${MOCK_RECOVER_ON_RESTART:-0}" = "1" ]; then
      : > "$MOCK_CASE_DIR/recovered"
    fi
    [ "${MOCK_RESTART_FAIL:-0}" = "0" ]
    ;;
  reboot)
    exit 0
    ;;
esac
EOF
  chmod +x "$CASE_DIR/gateway-services.sh" "$CASE_DIR/bin/systemctl"
  export PATH="$CASE_DIR/bin:$ORIGINAL_PATH"
  export MOCK_CASE_DIR="$CASE_DIR"
  export MOCK_ACTIONS="$CASE_DIR/actions"
  export GATEWAY_SERVICES_SCRIPT="$CASE_DIR/gateway-services.sh"
  export WATCHDOG_SYSTEMCTL_BIN=systemctl
  export WATCHDOG_STATE_DIR="$CASE_DIR/state"
  export WATCHDOG_RUN_DIR="$CASE_DIR/run"
  export WATCHDOG_STARTUP_GRACE_SEC=0
  export WATCHDOG_RECOVERY_VERIFY_SEC=0
  export WATCHDOG_RECOVERY_COOLDOWN_SEC=0
  export WATCHDOG_NOW_EPOCH=1000
  export WATCHDOG_NOW_MONOTONIC=100
  export WATCHDOG_BOOT_ID=test-boot
  export WATCHDOG_APPLYING_STALE_SEC=30
  export WATCHDOG_DISABLE_FLOCK=0
  export MOCK_GATEWAY_STATE=active
  export MOCK_HEALTHY=1
  export MOCK_INACTIVE_UNITS=""
  export MOCK_RECOVER_ON_RESTART=0
  export MOCK_RESTART_FAIL=0
  export MOCK_LIST_FAIL=0
  export MOCK_LIST_BLOCK_SEC=0
  export MOCK_EXTRA_UNITS=""
}

run_check() {
  sh "$WATCHDOG" check >/dev/null 2>&1 || true
}

ORIGINAL_PATH="$PATH"

setup_case healthy
run_check
assert_not_contains "$CASE_DIR/actions" "restart gateway-services.service"
assert_contains "$CASE_DIR/state/status.json" '"status":"healthy"'

setup_case transient
export WATCHDOG_FAILURE_THRESHOLD=3
export MOCK_HEALTHY=0
export MOCK_INACTIVE_UNITS="modbus-rtu@device-test.service"
run_check
assert_not_contains "$CASE_DIR/actions" "restart gateway-services.service"
assert_contains "$CASE_DIR/state/status.json" '"consecutiveFailures":1'

setup_case recover
export WATCHDOG_FAILURE_THRESHOLD=3
export MOCK_HEALTHY=0
export MOCK_INACTIVE_UNITS="mqtt-driver@mqtt-service.service"
export MOCK_RECOVER_ON_RESTART=1
run_check
run_check
run_check
assert_contains "$CASE_DIR/actions" "reset-failed mqtt-driver@mqtt-service.service"
assert_contains "$CASE_DIR/actions" "restart mqtt-driver@mqtt-service.service"
assert_not_contains "$CASE_DIR/actions" "restart gateway-services.service"
assert_contains "$CASE_DIR/state/status.json" '"lastAction":"units-restarted"'

setup_case reboot
export WATCHDOG_FAILURE_THRESHOLD=1
export WATCHDOG_REBOOT_FAILURE_THRESHOLD=2
export MOCK_HEALTHY=0
export MOCK_INACTIVE_UNITS="system-monitor@monitor-service.service"
run_check
export WATCHDOG_NOW_EPOCH=1001
export WATCHDOG_NOW_MONOTONIC=101
run_check
assert_contains "$CASE_DIR/actions" "reboot"
assert_contains "$CASE_DIR/state/status.json" '"status":"rebooting"'
assert_not_contains "$CASE_DIR/actions" "restart gateway-services.service"

setup_case collector_no_reboot
export WATCHDOG_FAILURE_THRESHOLD=1
export WATCHDOG_REBOOT_FAILURE_THRESHOLD=1
export MOCK_HEALTHY=0
export MOCK_INACTIVE_UNITS="modbus-rtu@device-test.service"
run_check
assert_not_contains "$CASE_DIR/actions" "reboot"
assert_not_contains "$CASE_DIR/actions" "restart gateway-services.service"

setup_case collector_recover
export WATCHDOG_FAILURE_THRESHOLD=1
export MOCK_HEALTHY=0
export MOCK_INACTIVE_UNITS="modbus-rtu@device-test.service"
export MOCK_RECOVER_ON_RESTART=1
run_check
assert_contains "$CASE_DIR/actions" "reset-failed modbus-rtu@device-test.service"
assert_contains "$CASE_DIR/actions" "restart modbus-rtu@device-test.service"
assert_not_contains "$CASE_DIR/actions" "restart gateway-services.service"
assert_contains "$CASE_DIR/state/status.json" '"lastAction":"units-restarted"'

setup_case launcher_recover
export WATCHDOG_FAILURE_THRESHOLD=1
export MOCK_HEALTHY=0
export MOCK_INACTIVE_UNITS="gateway-services.service"
export MOCK_RECOVER_ON_RESTART=1
run_check
assert_contains "$CASE_DIR/actions" "reset-failed gateway-services.service"
assert_contains "$CASE_DIR/actions" "start gateway-services.service"
assert_not_contains "$CASE_DIR/actions" "restart gateway-services.service"
assert_contains "$CASE_DIR/state/status.json" '"lastAction":"gateway-started"'

setup_case network_is_not_business_health
export WATCHDOG_FAILURE_THRESHOLD=1
export MOCK_HEALTHY=0
export MOCK_INACTIVE_UNITS="gateway-cellular.service gateway-network-failover.service"
export MOCK_EXTRA_UNITS="gateway-cellular.service gateway-network-failover.service"
run_check
assert_not_contains "$CASE_DIR/actions" "restart gateway-cellular.service"
assert_not_contains "$CASE_DIR/actions" "restart gateway-network-failover.service"
assert_contains "$CASE_DIR/state/status.json" '"status":"healthy"'

setup_case manual_stop
: > "$CASE_DIR/run/manual-stop"
export WATCHDOG_FAILURE_THRESHOLD=1
export MOCK_HEALTHY=0
export MOCK_INACTIVE_UNITS="mqtt-driver@mqtt-service.service"
run_check
assert_not_contains "$CASE_DIR/actions" "restart gateway-services.service"
assert_contains "$CASE_DIR/state/status.json" '"status":"suspended"'

setup_case applying
cat > "$CASE_DIR/run/applying" <<EOF
pid=$$
boot_id=test-boot
created_uptime_sec=100
EOF
export WATCHDOG_FAILURE_THRESHOLD=1
export MOCK_HEALTHY=0
export MOCK_INACTIVE_UNITS="mqtt-driver@mqtt-service.service"
run_check
assert_not_contains "$CASE_DIR/actions" "restart mqtt-driver@mqtt-service.service"
assert_contains "$CASE_DIR/state/status.json" '"reason":"configuration-applying"'

setup_case stale_applying
cat > "$CASE_DIR/run/applying" <<'EOF'
pid=999999
boot_id=test-boot
created_uptime_sec=1
EOF
export WATCHDOG_FAILURE_THRESHOLD=1
export MOCK_HEALTHY=0
export MOCK_INACTIVE_UNITS="modbus-rtu@device-test.service"
export MOCK_RECOVER_ON_RESTART=1
run_check
[ ! -e "$CASE_DIR/run/applying" ] || fail "stale applying marker was not removed"
assert_contains "$CASE_DIR/actions" "restart modbus-rtu@device-test.service"

setup_case epoch_rollback_does_not_extend_startup_grace
export WATCHDOG_STARTUP_GRACE_SEC=60
export WATCHDOG_FAILURE_THRESHOLD=1
export MOCK_HEALTHY=1
run_check
export WATCHDOG_NOW_EPOCH=10
export WATCHDOG_NOW_MONOTONIC=161
export MOCK_HEALTHY=0
export MOCK_INACTIVE_UNITS="modbus-rtu@device-test.service"
export MOCK_RECOVER_ON_RESTART=1
run_check
assert_contains "$CASE_DIR/actions" "restart modbus-rtu@device-test.service"
assert_not_contains "$CASE_DIR/state/status.json" '"reason":"startup-grace"'

setup_case stale_fallback_lock
export WATCHDOG_DISABLE_FLOCK=1
export WATCHDOG_FAILURE_THRESHOLD=1
export MOCK_HEALTHY=0
export MOCK_INACTIVE_UNITS="modbus-rtu@device-test.service"
export MOCK_RECOVER_ON_RESTART=1
mkdir "$CASE_DIR/run/recovery.lock.d"
printf '%s\n' 999999 > "$CASE_DIR/run/recovery.lock.d/pid"
run_check
assert_contains "$CASE_DIR/actions" "restart modbus-rtu@device-test.service"

setup_case invalid_config
export WATCHDOG_FAILURE_THRESHOLD=1
export WATCHDOG_REBOOT_FAILURE_THRESHOLD=1
export MOCK_LIST_FAIL=1
export MOCK_HEALTHY=0
run_check
assert_not_contains "$CASE_DIR/actions" "reboot"
assert_contains "$CASE_DIR/state/status.json" '"reason":"config-list-failed"'

setup_case interrupted_list_cleanup
export MOCK_LIST_BLOCK_SEC=1
: > "$CASE_DIR/run/recovery.lock"
sh "$WATCHDOG" check >/dev/null 2>&1 &
watchdog_pid=$!
waited=0
while [ ! -e "$CASE_DIR/run/desired-units.$watchdog_pid" ] && [ "$waited" -lt 50 ]; do
  sleep 0.1
  waited=$((waited + 1))
done
[ -e "$CASE_DIR/run/desired-units.$watchdog_pid" ] || fail "watchdog did not create desired-unit temp file"
kill -TERM "$watchdog_pid"
wait "$watchdog_pid" 2>/dev/null || true
[ ! -e "$CASE_DIR/run/desired-units.$watchdog_pid" ] || fail "desired-unit temp file survived SIGTERM"
[ ! -e "$CASE_DIR/run/desired-units-error.$watchdog_pid" ] || fail "desired-unit error file survived SIGTERM"
[ -e "$CASE_DIR/run/recovery.lock" ] || fail "normal flock file was removed during cleanup"

assert_contains "$ROOT_DIR/deploy/mqtt-forwarder@.service" "StartLimitIntervalSec=60"
assert_contains "$ROOT_DIR/deploy/mqtt-forwarder@.service" "StartLimitBurst=10"
assert_contains "$ROOT_DIR/deploy/gateway-health-watchdog.service" "SuccessExitStatus=0 143"

assert_contains "$ROOT_DIR/deploy/ota-apply.sh" '"/opt/modbus-gateway/bin/gateway-health-watchdog.sh"'
assert_contains "$ROOT_DIR/deploy/ota-apply.sh" '"/etc/systemd/system/gateway-health-watchdog.service"'
assert_contains "$ROOT_DIR/deploy/ota-apply.sh" '"/etc/default/gateway-health-watchdog"'

echo "[PASS] gateway health watchdog tests"
