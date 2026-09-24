#!/bin/bash
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
build=/tmp/ems-driver-send-build
cd "$root"
evidence="$root/evidence/driver-send-20260924"
tests=(modbus_rtu_client_resync_test posix_serial_port_recovery_test dlt645_command_executor_test control_dedup_protocol_test control_dedup_integration_test can_signal_codec_test)
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Debug > "$evidence/affected-targets-configure.log" 2>&1
timeout 240 cmake --build "$build" --target ModbusRtu CanDriver DioDriver Dlt645Driver IecDriver "${tests[@]}" -j4 > "$evidence/affected-targets-rebuild.log" 2>&1
for test in "${tests[@]}"; do
    timeout 30 unshare --net --mount --ipc --pid --fork --mount-proc sh /mnt/d/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/cluster-evidence-20260924/evidence/realese1.0/cluster-20260924/isolated_test.sh "$build/$test" "$build" > "$evidence/regression-$test.log" 2>&1
    printf '%s exit=0\n' "$test"
done
files=(src/command_executor.cpp src/common/gateway_daemon.cpp src/modbus_tcp_client.cpp src/modbus_rtu_client.cpp src/posix_serial_port.cpp src/writeback_service.cpp src/can_driver_service.cpp src/iec_command_executor.cpp src/dio_command_executor.cpp src/dlt645_command_executor.cpp main.cpp can_driver_main.cpp dio_driver_main.cpp dlt645_main.cpp iec_driver_main.cpp)
for file in "${files[@]}"; do
    timeout 30 g++ -std=c++14 -Iinclude -fsyntax-only "$file"
    printf 'C++14 syntax %s passed\n' "$file"
done > "$evidence/cxx14-syntax.log" 2>&1
git diff --check
git rev-parse HEAD
g++ --version | head -1
