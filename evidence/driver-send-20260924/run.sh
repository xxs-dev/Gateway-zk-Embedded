#!/bin/bash
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
build=/tmp/ems-driver-send-build
stage=${1:?stage}
cd "$root"
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Debug > "evidence/driver-send-20260924/$stage-configure.log" 2>&1
timeout 240 cmake --build "$build" --target edge_gateway -j4 > "evidence/driver-send-20260924/$stage-build.log" 2>&1
g++ -std=c++17 -g -O0 -Iinclude tools/cluster_driver_send_guard_test.cpp "$build/libedge_gateway.a" -pthread -ldl -lrt -o "$build/cluster_driver_send_guard_test" > "evidence/driver-send-20260924/$stage-link.log" 2>&1
set +e
timeout 20 unshare --net --mount --ipc --pid --fork --mount-proc sh /mnt/d/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/cluster-evidence-20260924/evidence/realese1.0/cluster-20260924/isolated_test.sh "$build/cluster_driver_send_guard_test" "$build" > "evidence/driver-send-20260924/$stage-test.log" 2>&1
rc=$?
printf 'exit=%s\n' "$rc" >> "evidence/driver-send-20260924/$stage-test.log"
cat "evidence/driver-send-20260924/$stage-test.log"
exit "$rc"
