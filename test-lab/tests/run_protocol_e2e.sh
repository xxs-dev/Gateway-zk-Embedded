#!/bin/sh
set -eu

SOURCE_DIR=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
BUILD_DIR=${2:-$SOURCE_DIR/build-wsl-native}
MONITOR_PORT=${GATEWAY_TEST_LAB_PROTOCOL_E2E_MONITOR_PORT:-29453}
ROOT=$(mktemp -d /tmp/gateway-test-lab-protocol-e2e.XXXXXX)
LAB_SCRIPT=$SOURCE_DIR/test-lab/scripts/gateway-test-lab.sh

print_logs() {
    for log in "$ROOT"/logs/*.log; do
        [ -f "$log" ] || continue
        echo "--- $log" >&2
        tail -n 100 "$log" >&2 || true
    done
}

run_lab() {
    GATEWAY_TEST_LAB_ROOT="$ROOT" \
    GATEWAY_BIN_DIR="$BUILD_DIR" \
    GATEWAY_TEST_LAB_SIM_BIN="$ROOT/bin/gateway-test-lab-sim" \
        "$LAB_SCRIPT" "$@"
}

cleanup() {
    result=$?
    trap - EXIT INT TERM
    run_lab stop >/dev/null 2>&1 || true
    if [ "$result" -ne 0 ]; then print_logs; fi
    if [ "${KEEP_GATEWAY_TEST_LAB_PROTOCOL_E2E:-0}" = "1" ]; then
        echo "test-lab protocol e2e files retained at $ROOT"
    else
        case "$ROOT" in /tmp/gateway-test-lab-protocol-e2e.*) rm -rf -- "$ROOT" ;; esac
    fi
    exit "$result"
}
trap cleanup EXIT INT TERM

mkdir -p "$ROOT/bin"
cp "$BUILD_DIR/gateway-test-lab-sim" "$ROOT/bin/"
cp -R "$SOURCE_DIR/test-lab/templates" "$ROOT/"
chmod +x "$LAB_SCRIPT"

check_points() {
    point_code=$1
    attempt=0
    while [ "$attempt" -lt 40 ]; do
        attempt=$((attempt + 1))
        if curl --noproxy '*' --max-time 2 -fsS \
                "http://127.0.0.1:$MONITOR_PORT/api/v1/health" \
                > "$ROOT/health.json" 2>/dev/null &&
            grep -q '"success":true' "$ROOT/health.json" &&
            curl --noproxy '*' --max-time 2 -fsS \
                "http://127.0.0.1:$MONITOR_PORT/api/v1/realtime/points" \
                > "$ROOT/realtime.json" 2>/dev/null &&
            grep -q "$point_code" "$ROOT/realtime.json" &&
            grep -q '"quality":1' "$ROOT/realtime.json" &&
            grep -q 'SIM_COMPUTE_DOUBLE' "$ROOT/realtime.json" &&
            grep -Eq 'mqtt full .* count=([2-9]|[1-9][0-9]+)' \
                "$ROOT/logs/mqtt-driver.log" 2>/dev/null; then
            return 0
        fi
        sleep 0.25
    done

    echo "test-lab data path did not become ready for $point_code" >&2
    [ ! -f "$ROOT/health.json" ] || cat "$ROOT/health.json" >&2
    [ ! -f "$ROOT/realtime.json" ] || cat "$ROOT/realtime.json" >&2
    [ ! -f "$ROOT/logs/mqtt-driver.log" ] || tail -n 100 "$ROOT/logs/mqtt-driver.log" >&2
    return 1
}

run_lab start normal --protocol modbus-rtu --monitor-host 127.0.0.1 --monitor-port "$MONITOR_PORT" >/dev/null
check_points SIM_RTU02_VOLTAGE
run_lab fault modbus.slave2 exception >/dev/null
sleep 1
run_lab recover modbus.slave2 >/dev/null
sleep 1
run_lab set-value modbus.slave1 holding 10 4321 >/dev/null
sleep 1
curl --noproxy '*' --max-time 5 -fsS "http://127.0.0.1:$MONITOR_PORT/api/v1/realtime/points" |
    grep -q '4321'
run_lab stop >/dev/null

run_lab start normal --protocol dlt645 --monitor-host 127.0.0.1 --monitor-port "$MONITOR_PORT" >/dev/null
check_points SIM_64502_VOLTAGE_A
run_lab fault dlt645.meter2 exception >/dev/null
sleep 1
run_lab recover dlt645.meter2 >/dev/null
sleep 1
run_lab stop >/dev/null

run_lab start normal --protocol dio --monitor-host 127.0.0.1 --monitor-port "$MONITOR_PORT" >/dev/null
check_points SIM_DI_1
run_lab set-value dio.gpio101 1 >/dev/null
sleep 1
curl --noproxy '*' --max-time 5 -fsS "http://127.0.0.1:$MONITOR_PORT/api/v1/realtime/points" \
    > "$ROOT/realtime-dio.json"
grep -q 'SIM_DI_1' "$ROOT/realtime-dio.json"
grep -q '"value":1' "$ROOT/realtime-dio.json"
run_lab stop >/dev/null

run_lab start normal --protocol iec104 --port 22404 --monitor-host 127.0.0.1 --monitor-port "$MONITOR_PORT" >/dev/null
check_points SIM_IEC104_POWER
run_lab fault iec104.station1 timeout >/dev/null
sleep 1
run_lab recover iec104.station1 >/dev/null
sleep 1
run_lab stop >/dev/null

run_lab start normal --protocol can --host 127.0.0.1 \
    --can-driver-udp-port 29011 --can-simulator-udp-port 29012 \
    --monitor-host 127.0.0.1 --monitor-port "$MONITOR_PORT" >/dev/null
check_points SIM_CAN_SPEED
curl --noproxy '*' --max-time 5 -fsS \
    -H 'Content-Type: application/json' \
    -d '{"machineCode":"SIM_COMM202600999","passwordSha256":"5c2358ee05dbd6bc6d52939f51a45c315533ad9191eecf1631fd788ec8ab76b3","requestId":"CAN_E2E_WRITE","commands":[{"cmdId":"CAN_E2E_RELAY","meterCode":"SIM_CAN_01","pointCode":"SIM_CAN_RELAY","index":990004,"value":1,"source":"test-lab-e2e"}]}' \
    "http://127.0.0.1:$MONITOR_PORT/api/v1/control/batch" > "$ROOT/can-write.json"
grep -q '"success":true' "$ROOT/can-write.json"
attempt=0
while [ "$attempt" -lt 20 ]; do
    attempt=$((attempt + 1))
    grep -Eq '"receivedFrames":([1-9]|[1-9][0-9]+)' "$ROOT/run/simulator-status.json" && break
    sleep 0.1
done
grep -Eq '"receivedFrames":([1-9]|[1-9][0-9]+)' "$ROOT/run/simulator-status.json"
run_lab fault can.device1 timeout >/dev/null
sleep 1
run_lab recover can.device1 >/dev/null
sleep 1
run_lab stop >/dev/null

[ ! -e /dev/shm/gateway_test_lab_modbus_rtu ]
[ ! -e /dev/shm/gateway_test_lab_dlt645 ]
[ ! -e /dev/shm/gateway_test_lab_dio ]
[ ! -e /dev/shm/gateway_test_lab_can ]
[ ! -e /dev/shm/gateway_test_lab_iec104 ]
echo "gateway_test_lab_protocol_e2e passed"
