#!/bin/bash
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
files=(src/command_executor.cpp src/common/gateway_daemon.cpp src/modbus_tcp_client.cpp src/modbus_rtu_client.cpp src/posix_serial_port.cpp src/writeback_service.cpp src/can_driver_service.cpp src/iec_command_executor.cpp src/dio_command_executor.cpp src/dlt645_command_executor.cpp main.cpp can_driver_main.cpp dio_driver_main.cpp dlt645_main.cpp iec_driver_main.cpp)
{
    g++ --version | head -1
    for file in "${files[@]}"; do
        timeout 30 g++ -std=c++14 -Iinclude -fsyntax-only "$file"
        printf 'C++14 syntax %s passed\n' "$file"
    done
    sha256sum tools/cluster_driver_send_guard_test.cpp tools/cluster_driver_rejection_test.cpp src/can_driver_service.cpp
} > evidence/driver-send-20260924/cxx14-syntax.log 2>&1
