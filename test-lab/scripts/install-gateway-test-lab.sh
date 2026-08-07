#!/bin/sh
set -eu

ROOT=/opt/modbus-gateway/test-lab
SIM_BIN=
SYSTEM_MONITOR_BIN=
GATEWAY_BIN_SOURCE_DIR=
LINK_COMMAND=0

usage() {
    echo "Usage: install-gateway-test-lab.sh --sim-bin FILE [--gateway-bin-dir DIR] [--system-monitor-bin FILE] [--root DIR] [--link-command]"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --sim-bin) SIM_BIN=$2; shift 2 ;;
        --gateway-bin-dir) GATEWAY_BIN_SOURCE_DIR=$2; shift 2 ;;
        --system-monitor-bin) SYSTEM_MONITOR_BIN=$2; shift 2 ;;
        --root) ROOT=$2; shift 2 ;;
        --link-command) LINK_COMMAND=1; shift ;;
        *) usage; exit 2 ;;
    esac
done

[ -n "$SIM_BIN" ] && [ -f "$SIM_BIN" ] || { echo "valid --sim-bin is required" >&2; exit 2; }
SOURCE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if [ -x "$ROOT/bin/gateway-test-lab" ]; then
    GATEWAY_TEST_LAB_ROOT="$ROOT" "$ROOT/bin/gateway-test-lab" stop >/dev/null 2>&1 || true
fi
rm -f -- "$ROOT/run/control.conf" "$ROOT/run/simulator-status.json" \
    "$ROOT/run/simulator.pid" "$ROOT/run/modbus-driver.pid" \
    "$ROOT/run/system-monitor.pid" "$ROOT/run/compute-engine.pid" \
    "$ROOT/run/event-engine.pid" "$ROOT/run/mqtt-driver.pid" \
    "$ROOT/run/mode" "$ROOT/run/protocol" "$ROOT/run/can-interface" \
    "$ROOT/run/can-interface-created"

install -d -m 0755 "$ROOT/bin" "$ROOT/templates" "$ROOT/config" "$ROOT/run" "$ROOT/logs" "$ROOT/data"
install -m 0755 "$SIM_BIN" "$ROOT/bin/gateway-test-lab-sim"
if [ -n "$GATEWAY_BIN_SOURCE_DIR" ]; then
    for binary in ModbusRtu Dlt645Driver CanDriver DioDriver IecDriver \
        ComputeEngine EventEngine MqttDriver SystemMonitor; do
        source_file=$GATEWAY_BIN_SOURCE_DIR/$binary
        [ -f "$source_file" ] || { echo "gateway binary not found: $source_file" >&2; exit 2; }
        install -m 0755 "$source_file" "$ROOT/bin/$binary"
    done
fi
if [ -n "$SYSTEM_MONITOR_BIN" ]; then
    [ -f "$SYSTEM_MONITOR_BIN" ] || { echo "invalid --system-monitor-bin: $SYSTEM_MONITOR_BIN" >&2; exit 2; }
    install -m 0755 "$SYSTEM_MONITOR_BIN" "$ROOT/bin/SystemMonitor"
fi
install -m 0755 "$SOURCE_DIR/scripts/gateway-test-lab.sh" "$ROOT/bin/gateway-test-lab"
install -m 0644 "$SOURCE_DIR/templates/device_identity.json" "$ROOT/templates/device_identity.json"
install -m 0644 "$SOURCE_DIR/templates/device_modbus_tcp.json" "$ROOT/templates/device_modbus_tcp.json"
install -m 0644 "$SOURCE_DIR/templates/device_modbus_rtu.json" "$ROOT/templates/device_modbus_rtu.json"
install -m 0644 "$SOURCE_DIR/templates/device_dlt645.json" "$ROOT/templates/device_dlt645.json"
install -m 0644 "$SOURCE_DIR/templates/device_can.json" "$ROOT/templates/device_can.json"
install -m 0644 "$SOURCE_DIR/templates/device_dio.json" "$ROOT/templates/device_dio.json"
install -m 0644 "$SOURCE_DIR/templates/device_iec104.json" "$ROOT/templates/device_iec104.json"
install -m 0644 "$SOURCE_DIR/templates/device_compute.json" "$ROOT/templates/device_compute.json"
install -m 0644 "$SOURCE_DIR/templates/monitor-service.json" "$ROOT/templates/monitor-service.json"
install -m 0644 "$SOURCE_DIR/templates/monitor-service-protocol.json" "$ROOT/templates/monitor-service-protocol.json"

if [ "$LINK_COMMAND" = "1" ]; then
    ln -sfn "$ROOT/bin/gateway-test-lab" /usr/local/bin/gateway-test-lab
fi

echo "gateway test-lab installed at $ROOT"
echo "it is disabled and has not started any process"
