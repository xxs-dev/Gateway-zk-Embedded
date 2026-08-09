#!/bin/sh
set -eu

SOURCE_DIR=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
BUILD_DIR=${2:-$SOURCE_DIR/build-wsl-native}
SIM_PORT=${GATEWAY_TEST_LAB_E2E_SIM_PORT:-25020}
MONITOR_PORT=${GATEWAY_TEST_LAB_E2E_MONITOR_PORT:-29443}
ROOT=$(mktemp -d /tmp/gateway-test-lab-e2e.XXXXXX)
LAB_SCRIPT=$SOURCE_DIR/test-lab/scripts/gateway-test-lab.sh
SYSTEM_MONITOR_SHARED_MEMORY=gateway_test_lab_system_monitor_$(basename "$ROOT" | tr '.-' '__')

print_logs() {
    for log in "$ROOT"/logs/*.log; do
        [ -f "$log" ] || continue
        echo "--- $log" >&2
        tail -n 80 "$log" >&2 || true
    done
}

cleanup() {
    result=$?
    trap - EXIT INT TERM
    if [ -x "$LAB_SCRIPT" ] && [ -d "$ROOT/run" ]; then
        GATEWAY_TEST_LAB_ROOT="$ROOT" \
        GATEWAY_BIN_DIR="$BUILD_DIR" \
        GATEWAY_TEST_LAB_SIM_BIN="$ROOT/bin/gateway-test-lab-sim" \
            "$LAB_SCRIPT" stop >/dev/null 2>&1 || true
    fi
    if [ "$result" -ne 0 ]; then print_logs; fi
    if [ "${KEEP_GATEWAY_TEST_LAB_E2E:-0}" = "1" ]; then
        echo "test-lab e2e files retained at $ROOT"
    else
        case "$ROOT" in /tmp/gateway-test-lab-e2e.*) rm -rf -- "$ROOT" ;; esac
    fi
    exit "$result"
}
trap cleanup EXIT INT TERM

mkdir -p "$ROOT/bin"
cp "$BUILD_DIR/gateway-test-lab-sim" "$ROOT/bin/"
cp -R "$SOURCE_DIR/test-lab/templates" "$ROOT/"
chmod +x "$LAB_SCRIPT"

run_lab() {
    GATEWAY_TEST_LAB_ROOT="$ROOT" \
    GATEWAY_BIN_DIR="$BUILD_DIR" \
    GATEWAY_TEST_LAB_SIM_BIN="$ROOT/bin/gateway-test-lab-sim" \
    GATEWAY_TEST_LAB_SYSTEM_MONITOR_SHARED_MEMORY="$SYSTEM_MONITOR_SHARED_MEMORY" \
        "$LAB_SCRIPT" "$@"
}

run_lab start normal --port "$SIM_PORT" --monitor-host 127.0.0.1 --monitor-port "$MONITOR_PORT" >/dev/null
sleep 2
curl --noproxy '*' --max-time 5 -fsS "http://127.0.0.1:$MONITOR_PORT/api/v1/health" | grep -q '"success":true'
curl --noproxy '*' --max-time 5 -fsS "http://127.0.0.1:$MONITOR_PORT/api/v1/realtime/points" > "$ROOT/realtime-before.json"
grep -q 'SIM_MODBUS_01' "$ROOT/realtime-before.json"
grep -q 'SIM_MODBUS_02' "$ROOT/realtime-before.json"
grep -q 'SIM_MODBUS_03' "$ROOT/realtime-before.json"
curl --noproxy '*' --max-time 5 -fsS \
    -H 'Content-Type: application/json' \
    -d '{"machineCode":"SIM_COMM202600999","passwordSha256":"5c2358ee05dbd6bc6d52939f51a45c315533ad9191eecf1631fd788ec8ab76b3"}' \
    "http://127.0.0.1:$MONITOR_PORT/api/v1/config/snapshot" > "$ROOT/config-snapshot.json"
grep -q '"success":true' "$ROOT/config-snapshot.json"
grep -q 'device_modbus_tcp.json' "$ROOT/config-snapshot.json"
if grep -q '/ky-ems/' "$ROOT/config-snapshot.json"; then
    echo "test-lab snapshot leaked production KY-EMS files" >&2
    exit 1
fi

run_lab fault modbus.slave2 timeout >/dev/null
sleep 3
curl --noproxy '*' --max-time 5 -fsS "http://127.0.0.1:$MONITOR_PORT/api/v1/realtime/points" > "$ROOT/realtime-fault.json"
grep -q '"quality":0' "$ROOT/realtime-fault.json"
grep -q 'SIM_MODBUS_01' "$ROOT/realtime-fault.json"
grep -q 'SIM_MODBUS_03' "$ROOT/realtime-fault.json"

run_lab recover modbus.slave2 >/dev/null
sleep 3
curl --noproxy '*' --max-time 5 -fsS "http://127.0.0.1:$MONITOR_PORT/api/v1/realtime/points" > "$ROOT/realtime-recovered.json"
grep -q 'SIM_MODBUS_02' "$ROOT/realtime-recovered.json"

run_lab set-value modbus.slave1 holding 10 4321 >/dev/null
sleep 1
curl --noproxy '*' --max-time 5 -fsS "http://127.0.0.1:$MONITOR_PORT/api/v1/realtime/points" > "$ROOT/realtime-write.json"
grep -q '4321' "$ROOT/realtime-write.json"

run_lab stop >/dev/null
[ ! -e /dev/shm/gateway_test_lab_modbus ]
[ ! -e "/dev/shm/$SYSTEM_MONITOR_SHARED_MEMORY" ]
echo "gateway_test_lab_modbus_e2e passed"
