#!/bin/sh
set -eu

ROOT=${GATEWAY_TEST_LAB_ROOT:-/opt/modbus-gateway/test-lab}
if [ -n "${GATEWAY_BIN_DIR:-}" ]; then
    GATEWAY_BIN_DIR=$GATEWAY_BIN_DIR
elif [ -x "$ROOT/bin/ModbusRtu" ]; then
    GATEWAY_BIN_DIR=$ROOT/bin
else
    GATEWAY_BIN_DIR=/opt/modbus-gateway/bin
fi
SIM_BIN=${GATEWAY_TEST_LAB_SIM_BIN:-$ROOT/bin/gateway-test-lab-sim}
if [ -n "${GATEWAY_TEST_LAB_SYSTEM_MONITOR_BIN:-}" ]; then
    SYSTEM_MONITOR_BIN=$GATEWAY_TEST_LAB_SYSTEM_MONITOR_BIN
elif [ -x "$ROOT/bin/SystemMonitor" ]; then
    SYSTEM_MONITOR_BIN=$ROOT/bin/SystemMonitor
else
    SYSTEM_MONITOR_BIN=$GATEWAY_BIN_DIR/SystemMonitor
fi
CONTROL=$ROOT/run/control.conf
STATUS_FILE=$ROOT/run/simulator-status.json
SIM_PID=$ROOT/run/simulator.pid
DRIVER_PID=$ROOT/run/modbus-driver.pid
MONITOR_PID=$ROOT/run/system-monitor.pid
COMPUTE_PID=$ROOT/run/compute-engine.pid
EVENT_PID=$ROOT/run/event-engine.pid
MQTT_PID=$ROOT/run/mqtt-driver.pid
MODE_FILE=$ROOT/run/mode
PROTOCOL_FILE=$ROOT/run/protocol
SHARED_MEMORY_FILE=$ROOT/run/shared-memory-name
GPIO_ROOT=$ROOT/run/gpio
ROOT_TOKEN=$(basename "$ROOT" | tr '.-' '__')
SYSTEM_MONITOR_SHARED_MEMORY=${GATEWAY_TEST_LAB_SYSTEM_MONITOR_SHARED_MEMORY:-gateway_test_lab_system_monitor_$ROOT_TOKEN}

usage() {
    echo "Usage: gateway-test-lab start [normal|ramp|random|boundary] [--protocol modbus-tcp|modbus-rtu|dlt645|can|dio|iec104] [--mode virtual|hil]"
    echo "       [--host IP] [--port N] [--serial-device DEVICE] [--can-interface NAME] [--can-driver-udp-port N] [--can-simulator-udp-port N]"
    echo "       [--monitor-host IP] [--monitor-port N] [--mqtt-broker URL]"
    echo "       gateway-test-lab stop|status"
    echo "       gateway-test-lab scenario NAME"
    echo "       gateway-test-lab fault modbus.slaveN|dlt645.meterN|can.device1|dio.gpioN MODE"
    echo "       gateway-test-lab recover modbus.slaveN|dlt645.meterN|can.device1|dio.gpioN"
    echo "       gateway-test-lab set-value modbus.slaveN holding|input|coil|discrete ADDRESS VALUE"
    echo "       gateway-test-lab set-value dio.gpioN 0|1"
}

is_running() {
    pid_file=$1
    [ -f "$pid_file" ] || return 1
    pid=$(cat "$pid_file" 2>/dev/null || true)
    [ -n "$pid" ] || return 1
    kill -0 "$pid" 2>/dev/null || return 1
    [ -r "/proc/$pid/cmdline" ] || return 1
    tr '\000' ' ' < "/proc/$pid/cmdline" | grep -F "$ROOT" >/dev/null 2>&1
}

wait_started() {
    pid_file=$1
    name=$2
    count=0
    while [ "$count" -lt 30 ]; do
        if is_running "$pid_file"; then return 0; fi
        sleep 0.1
        count=$((count + 1))
    done
    echo "$name failed to start" >&2
    return 1
}

stop_process() {
    pid_file=$1
    name=$2
    if ! is_running "$pid_file"; then
        rm -f -- "$pid_file"
        return 0
    fi
    pid=$(cat "$pid_file")
    kill "$pid" 2>/dev/null || true
    count=0
    while kill -0 "$pid" 2>/dev/null && [ "$count" -lt 50 ]; do
        sleep 0.1
        count=$((count + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f -- "$pid_file"
    echo "$name stopped"
}

validate_token() {
    case "$1" in
        *'|'*|*'&'*|*'\\'*|*'"'*|*"'"*) echo "unsupported character in option: $1" >&2; exit 2 ;;
    esac
}

render_configs() {
    protocol=$1
    endpoint=$2
    simulator_port=$3
    monitor_host=$4
    monitor_port=$5
    mqtt_enabled=$6
    mqtt_broker=$7
    mode=$8
    mkdir -p "$ROOT/config/apps" "$ROOT/config/devices" "$ROOT/run" "$ROOT/logs" "$ROOT/data"
    cp "$ROOT/templates/device_identity.json" "$ROOT/config/device_identity.json"
    case "$protocol" in
        modbus-tcp)
            device_file=$ROOT/config/devices/device_modbus_tcp.json
            shared_memory=gateway_test_lab_modbus
            compute_source_index=910002
            sed -e "s|__SIMULATOR_HOST__|$endpoint|g" \
                -e "s|__SIMULATOR_PORT__|$simulator_port|g" \
                "$ROOT/templates/device_modbus_tcp.json" > "$device_file"
            ;;
        modbus-rtu)
            device_file=$ROOT/config/devices/device_modbus_rtu.json
            shared_memory=gateway_test_lab_modbus_rtu
            compute_source_index=940001
            sed -e "s|__SERIAL_DEVICE__|$endpoint|g" \
                "$ROOT/templates/device_modbus_rtu.json" > "$device_file"
            ;;
        dlt645)
            device_file=$ROOT/config/devices/device_dlt645.json
            shared_memory=gateway_test_lab_dlt645
            compute_source_index=970002
            sed -e "s|__SERIAL_DEVICE__|$endpoint|g" \
                "$ROOT/templates/device_dlt645.json" > "$device_file"
            ;;
        can)
            device_file=$ROOT/config/devices/device_can.json
            shared_memory=gateway_test_lab_can
            compute_source_index=990001
            if [ "$mode" = virtual ]; then can_transport=udp_test; else can_transport=socketcan; fi
            sed -e "s|__CAN_INTERFACE__|$endpoint|g" \
                -e "s|__CAN_TRANSPORT__|$can_transport|g" \
                -e "s|__CAN_UDP_BIND__|$host|g" \
                -e "s|__CAN_UDP_PEER__|$host|g" \
                -e "s|__CAN_DRIVER_UDP_PORT__|$can_driver_udp_port|g" \
                -e "s|__CAN_SIMULATOR_UDP_PORT__|$can_simulator_udp_port|g" \
                "$ROOT/templates/device_can.json" > "$device_file"
            ;;
        dio)
            device_file=$ROOT/config/devices/device_dio.json
            shared_memory=gateway_test_lab_dio
            compute_source_index=991001
            sed -e "s|__GPIO_ROOT__|$endpoint|g" \
                "$ROOT/templates/device_dio.json" > "$device_file"
            ;;
        iec104)
            device_file=$ROOT/config/devices/device_iec104.json
            shared_memory=gateway_test_lab_iec104
            compute_source_index=992002
            sed -e "s|__SIMULATOR_HOST__|$endpoint|g" \
                -e "s|__SIMULATOR_PORT__|$simulator_port|g" \
                "$ROOT/templates/device_iec104.json" > "$device_file"
            ;;
        *) echo "unsupported protocol: $protocol" >&2; exit 2 ;;
    esac
    compute_config_file=$ROOT/config/devices/device_compute.json
    sed -e "s|__SHARED_MEMORY_NAME__|$shared_memory|g" \
        "$ROOT/templates/device_compute.json" > "$compute_config_file"
    sed -e "s|__TEST_LAB_ROOT__|$ROOT|g" \
        -e "s|__MONITOR_HOST__|$monitor_host|g" \
        -e "s|__MONITOR_PORT__|$monitor_port|g" \
        -e "s|__DEVICE_CONFIG_FILE__|$device_file|g" \
        -e "s|__COMPUTE_CONFIG_FILE__|$compute_config_file|g" \
        -e "s|__SHARED_MEMORY_NAME__|$shared_memory|g" \
        -e "s|__COMPUTE_SOURCE_INDEX__|$compute_source_index|g" \
        -e "s|__MQTT_ENABLED__|$mqtt_enabled|g" \
        -e "s|__MQTT_BROKER__|$mqtt_broker|g" \
        "$ROOT/templates/monitor-service-protocol.json" > "$ROOT/config/apps/monitor-service.json"
}

prepare_gpio() {
    rm -rf -- "$GPIO_ROOT"
    mkdir -p "$GPIO_ROOT/gpio101" "$GPIO_ROOT/gpio102" "$GPIO_ROOT/gpio201" "$GPIO_ROOT/gpio202"
    printf 'in\n' > "$GPIO_ROOT/gpio101/direction"
    printf 'in\n' > "$GPIO_ROOT/gpio102/direction"
    printf 'out\n' > "$GPIO_ROOT/gpio201/direction"
    printf 'out\n' > "$GPIO_ROOT/gpio202/direction"
    printf '0\n' > "$GPIO_ROOT/gpio101/value"
    printf '1\n' > "$GPIO_ROOT/gpio102/value"
    printf '0\n' > "$GPIO_ROOT/gpio201/value"
    printf '0\n' > "$GPIO_ROOT/gpio202/value"
}

wait_status_file() {
    count=0
    while [ "$count" -lt 50 ]; do
        [ -s "$STATUS_FILE" ] && return 0
        sleep 0.1
        count=$((count + 1))
    done
    echo "simulator status file was not created" >&2
    return 1
}

start_lab() {
    scenario=normal
    mode=virtual
    protocol=modbus-tcp
    host=127.0.0.1
    port=15020
    serial_device=auto
    can_interface=gatewaytest0
    can_driver_udp_port=19011
    can_simulator_udp_port=19012
    monitor_host=0.0.0.0
    monitor_port=19443
    mqtt_enabled=false
    mqtt_broker=tcp://127.0.0.1:1883
    if [ "$#" -gt 0 ] && [ "${1#--}" = "$1" ]; then scenario=$1; shift; fi
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --mode) mode=$2; shift 2 ;;
            --protocol) protocol=$2; shift 2 ;;
            --host) host=$2; shift 2 ;;
            --port) port=$2; shift 2 ;;
            --serial-device) serial_device=$2; shift 2 ;;
            --can-interface) can_interface=$2; shift 2 ;;
            --can-driver-udp-port) can_driver_udp_port=$2; shift 2 ;;
            --can-simulator-udp-port) can_simulator_udp_port=$2; shift 2 ;;
            --monitor-host) monitor_host=$2; shift 2 ;;
            --monitor-port) monitor_port=$2; shift 2 ;;
            --mqtt-broker) mqtt_enabled=true; mqtt_broker=$2; shift 2 ;;
            *) echo "unknown start option: $1" >&2; exit 2 ;;
        esac
    done
    case "$scenario" in normal|ramp|random|boundary) ;; *) echo "invalid scenario: $scenario" >&2; exit 2 ;; esac
    case "$mode" in virtual|hil) ;; *) echo "invalid mode: $mode" >&2; exit 2 ;; esac
    case "$protocol" in modbus-tcp|modbus-rtu|dlt645|can|dio|iec104) ;; *) echo "invalid protocol: $protocol" >&2; exit 2 ;; esac
    validate_token "$host"
    validate_token "$monitor_host"
    validate_token "$mqtt_broker"
    validate_token "$ROOT"
    if is_running "$DRIVER_PID" || is_running "$MONITOR_PID" || is_running "$SIM_PID" || \
        is_running "$COMPUTE_PID" || is_running "$EVENT_PID" || is_running "$MQTT_PID"; then
        echo "test-lab is already running; stop it before starting a new scenario" >&2
        exit 1
    fi
    mkdir -p "$ROOT/config/apps" "$ROOT/config/devices" "$ROOT/run" "$ROOT/logs" "$ROOT/data"
    [ -x "$SIM_BIN" ] || { echo "simulator binary not found: $SIM_BIN" >&2; exit 1; }
    case "$protocol" in
        dlt645) driver_bin=$GATEWAY_BIN_DIR/Dlt645Driver ;;
        can) driver_bin=$GATEWAY_BIN_DIR/CanDriver ;;
        dio) driver_bin=$GATEWAY_BIN_DIR/DioDriver ;;
        iec104) driver_bin=$GATEWAY_BIN_DIR/IecDriver ;;
        *) driver_bin=$GATEWAY_BIN_DIR/ModbusRtu ;;
    esac
    [ -x "$driver_bin" ] || { echo "driver not found: $driver_bin" >&2; exit 1; }
    [ -x "$SYSTEM_MONITOR_BIN" ] || { echo "SystemMonitor not found: $SYSTEM_MONITOR_BIN" >&2; exit 1; }
    [ -x "$GATEWAY_BIN_DIR/ComputeEngine" ] || { echo "ComputeEngine not found" >&2; exit 1; }
    [ -x "$GATEWAY_BIN_DIR/EventEngine" ] || { echo "EventEngine not found" >&2; exit 1; }
    [ -x "$GATEWAY_BIN_DIR/MqttDriver" ] || { echo "MqttDriver not found" >&2; exit 1; }
    "$SIM_BIN" init --state-file "$CONTROL" --scenario "$scenario" --slaves 1,2,3 >/dev/null
    echo "$mode" > "$MODE_FILE"
    echo "$protocol" > "$PROTOCOL_FILE"

    if [ "$mode" = virtual ]; then
        if [ "$protocol" = modbus-tcp ]; then
            nohup "$SIM_BIN" serve --bind "$host" --port "$port" --state-file "$CONTROL" \
                --status-file "$STATUS_FILE" > "$ROOT/logs/simulator.log" 2>&1 &
        elif [ "$protocol" = modbus-rtu ]; then
            nohup "$SIM_BIN" serve-serial --protocol modbus-rtu --serial-device "$serial_device" \
                --baud-rate 9600 --parity N --state-file "$CONTROL" --status-file "$STATUS_FILE" \
                > "$ROOT/logs/simulator.log" 2>&1 &
        elif [ "$protocol" = dlt645 ]; then
            nohup "$SIM_BIN" serve-serial --protocol dlt645 --serial-device "$serial_device" \
                --baud-rate 2400 --parity E --state-file "$CONTROL" --status-file "$STATUS_FILE" \
                > "$ROOT/logs/simulator.log" 2>&1 &
        elif [ "$protocol" = can ]; then
            nohup "$SIM_BIN" serve-can --transport udp_test \
                --udp-bind "$host" --udp-listen-port "$can_simulator_udp_port" \
                --udp-peer "$host" --udp-peer-port "$can_driver_udp_port" \
                --state-file "$CONTROL" \
                --status-file "$STATUS_FILE" > "$ROOT/logs/simulator.log" 2>&1 &
        elif [ "$protocol" = iec104 ]; then
            nohup "$SIM_BIN" serve-iec104 --bind "$host" --port "$port" --state-file "$CONTROL" \
                --status-file "$STATUS_FILE" > "$ROOT/logs/simulator.log" 2>&1 &
        else
            prepare_gpio
        fi
        if [ "$protocol" != dio ]; then
            echo $! > "$SIM_PID"
            wait_started "$SIM_PID" simulator
            wait_status_file
        fi
        if [ "$protocol" = modbus-tcp ]; then
            endpoint=$host
        elif [ "$protocol" = modbus-rtu ] || [ "$protocol" = dlt645 ]; then
            endpoint=$(sed -n 's/.*"peerDevice":"\([^"]*\)".*/\1/p' "$STATUS_FILE")
            [ -n "$endpoint" ] || { echo "serial peer was not reported" >&2; exit 1; }
        elif [ "$protocol" = can ]; then
            endpoint=$can_interface
        elif [ "$protocol" = iec104 ]; then
            endpoint=$host
        else
            endpoint=$GPIO_ROOT
        fi
    else
        case "$protocol" in
            modbus-tcp) endpoint=$host ;;
            modbus-rtu|dlt645) endpoint=$serial_device ;;
            can) endpoint=$can_interface ;;
            dio) endpoint=/sys/class/gpio ;;
            iec104) endpoint=$host ;;
        esac
    fi
    render_configs "$protocol" "$endpoint" "$port" "$monitor_host" "$monitor_port" "$mqtt_enabled" "$mqtt_broker" "$mode"
    printf '%s\n' "$shared_memory" > "$SHARED_MEMORY_FILE"

    case "$protocol" in
        modbus-tcp) device_config=$ROOT/config/devices/device_modbus_tcp.json ;;
        modbus-rtu) device_config=$ROOT/config/devices/device_modbus_rtu.json ;;
        dlt645) device_config=$ROOT/config/devices/device_dlt645.json ;;
        can) device_config=$ROOT/config/devices/device_can.json ;;
        dio) device_config=$ROOT/config/devices/device_dio.json ;;
        iec104) device_config=$ROOT/config/devices/device_iec104.json ;;
    esac
    nohup "$driver_bin" \
        --config "$device_config" \
        --app-config "$ROOT/config/apps/monitor-service.json" \
        > "$ROOT/logs/modbus-driver.log" 2>&1 &
    echo $! > "$DRIVER_PID"
    wait_started "$DRIVER_PID" driver
    sleep 0.5

    nohup "$GATEWAY_BIN_DIR/ComputeEngine" --app-config "$ROOT/config/apps/monitor-service.json" \
        > "$ROOT/logs/compute-engine.log" 2>&1 &
    echo $! > "$COMPUTE_PID"
    wait_started "$COMPUTE_PID" ComputeEngine

    nohup "$GATEWAY_BIN_DIR/EventEngine" --app-config "$ROOT/config/apps/monitor-service.json" \
        > "$ROOT/logs/event-engine.log" 2>&1 &
    echo $! > "$EVENT_PID"
    wait_started "$EVENT_PID" EventEngine

    nohup "$GATEWAY_BIN_DIR/MqttDriver" --app-config "$ROOT/config/apps/monitor-service.json" \
        > "$ROOT/logs/mqtt-driver.log" 2>&1 &
    echo $! > "$MQTT_PID"
    wait_started "$MQTT_PID" MqttDriver

    nohup "$SYSTEM_MONITOR_BIN" \
        --app-config "$ROOT/config/apps/monitor-service.json" \
        --system-monitor-shared-memory "$SYSTEM_MONITOR_SHARED_MEMORY" \
        > "$ROOT/logs/system-monitor.log" 2>&1 &
    echo $! > "$MONITOR_PID"
    wait_started "$MONITOR_PID" SystemMonitor
    sleep 0.5
    status_lab
}

status_line() {
    pid_file=$1
    name=$2
    if is_running "$pid_file"; then
        echo "$name=running pid=$(cat "$pid_file")"
    else
        echo "$name=stopped"
    fi
}

status_lab() {
    mode=unknown
    protocol=unknown
    [ -f "$MODE_FILE" ] && mode=$(cat "$MODE_FILE")
    [ -f "$PROTOCOL_FILE" ] && protocol=$(cat "$PROTOCOL_FILE")
    echo "mode=$mode"
    echo "protocol=$protocol"
    status_line "$SIM_PID" simulator
    status_line "$DRIVER_PID" modbus-driver
    status_line "$COMPUTE_PID" compute-engine
    status_line "$EVENT_PID" event-engine
    status_line "$MQTT_PID" mqtt-driver
    status_line "$MONITOR_PID" system-monitor
    [ -f "$CONTROL" ] && "$SIM_BIN" show --state-file "$CONTROL"
    [ -f "$STATUS_FILE" ] && cat "$STATUS_FILE"
    return 0
}

stop_lab() {
    stop_process "$MONITOR_PID" system-monitor
    stop_process "$MQTT_PID" mqtt-driver
    stop_process "$EVENT_PID" event-engine
    stop_process "$COMPUTE_PID" compute-engine
    stop_process "$DRIVER_PID" modbus-driver
    stop_process "$SIM_PID" simulator
    shared_memory=
    [ ! -f "$SHARED_MEMORY_FILE" ] || shared_memory=$(cat "$SHARED_MEMORY_FILE")
    case "$shared_memory" in
        gateway_test_lab_*) rm -f -- "/dev/shm/$shared_memory" ;;
    esac
    rm -f -- "$CONTROL" "$STATUS_FILE" "$MODE_FILE" "$PROTOCOL_FILE" \
        "$SHARED_MEMORY_FILE" "/dev/shm/$SYSTEM_MONITOR_SHARED_MEMORY"
    rm -rf -- "$GPIO_ROOT"
}

target_slave() {
    target=$1
    case "$target" in
        modbus.slave*) slave=${target#modbus.slave} ;;
        dlt645.meter*) slave=${target#dlt645.meter} ;;
        can.device*) slave=${target#can.device} ;;
        iec104.station*) slave=${target#iec104.station} ;;
        *) echo "target must look like modbus.slave2, dlt645.meter2, or can.device1" >&2; exit 2 ;;
    esac
    case "$slave" in ''|*[!0-9]*) echo "invalid slave target: $target" >&2; exit 2 ;; esac
    [ "$slave" -ge 1 ] && [ "$slave" -le 247 ] || { echo "slave out of range: $slave" >&2; exit 2; }
    echo "$slave"
}

command=${1:-}
[ -n "$command" ] || { usage; exit 2; }
shift || true
case "$command" in
    start) start_lab "$@" ;;
    stop) stop_lab ;;
    status) status_lab ;;
    scenario)
        [ "$#" -eq 1 ] || { usage; exit 2; }
        if [ -f "$PROTOCOL_FILE" ] && [ "$(cat "$PROTOCOL_FILE")" = dio ]; then
            case "$1" in
                normal) printf '0\n' > "$GPIO_ROOT/gpio101/value"; printf '1\n' > "$GPIO_ROOT/gpio102/value" ;;
                ramp|random|boundary) printf '1\n' > "$GPIO_ROOT/gpio101/value"; printf '0\n' > "$GPIO_ROOT/gpio102/value" ;;
                *) echo "invalid scenario: $1" >&2; exit 2 ;;
            esac
        else
            "$SIM_BIN" scenario --state-file "$CONTROL" --name "$1"
        fi
        ;;
    fault)
        [ "$#" -eq 2 ] || { usage; exit 2; }
        case "$1" in
            dio.gpio*)
                gpio=${1#dio.gpio}
                [ -f "$GPIO_ROOT/gpio$gpio/value" ] || { echo "unknown test GPIO: $gpio" >&2; exit 2; }
                : > "$GPIO_ROOT/gpio$gpio/value"
                ;;
            *)
                slave=$(target_slave "$1")
                "$SIM_BIN" fault --state-file "$CONTROL" --slave "$slave" --mode "$2"
                ;;
        esac
        ;;
    recover)
        [ "$#" -eq 1 ] || { usage; exit 2; }
        case "$1" in
            dio.gpio*)
                gpio=${1#dio.gpio}
                [ -d "$GPIO_ROOT/gpio$gpio" ] || { echo "unknown test GPIO: $gpio" >&2; exit 2; }
                printf '0\n' > "$GPIO_ROOT/gpio$gpio/value"
                ;;
            *)
                slave=$(target_slave "$1")
                "$SIM_BIN" recover --state-file "$CONTROL" --slave "$slave"
                ;;
        esac
        ;;
    set-value)
        case "${1:-}" in
            dio.gpio*)
                [ "$#" -eq 2 ] || { usage; exit 2; }
                gpio=${1#dio.gpio}
                case "$2" in 0|1) ;; *) echo "DIO value must be 0 or 1" >&2; exit 2 ;; esac
                [ -f "$GPIO_ROOT/gpio$gpio/value" ] || { echo "unknown test GPIO: $gpio" >&2; exit 2; }
                printf '%s\n' "$2" > "$GPIO_ROOT/gpio$gpio/value"
                ;;
            *)
                [ "$#" -eq 4 ] || { usage; exit 2; }
                slave=$(target_slave "$1")
                "$SIM_BIN" set-value --state-file "$CONTROL" --slave "$slave" --area "$2" --address "$3" --value "$4"
                ;;
        esac
        ;;
    *) usage; exit 2 ;;
esac
