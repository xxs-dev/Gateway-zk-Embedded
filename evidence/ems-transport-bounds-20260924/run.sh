#!/bin/bash
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
evidence="$root/evidence/ems-transport-bounds-20260924"
build=/tmp/ems-transport-bounds-20260924-build
stage=${1:?red or green evidence label required}
runner=/mnt/d/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/cluster-evidence-20260924/evidence/realese1.0/cluster-20260924/isolated_test.sh
timeout 30s cmake -S "$root" -B "$build" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug > "$evidence/$stage-configure.log" 2>&1
timeout 120s cmake --build "$build" --target ems_cluster_transport_bounds_test -j2 > "$evidence/$stage-build.log" 2>&1
set +e
timeout 25s unshare --mount --net --ipc --pid --fork --mount-proc sh "$runner" "$build/ems_cluster_transport_bounds_test" "$build" > "$evidence/$stage-test.log" 2>&1
result=$?
set -e
cat "$evidence/$stage-test.log"
printf 'test_exit=%s\n' "$result"
exit "$result"
