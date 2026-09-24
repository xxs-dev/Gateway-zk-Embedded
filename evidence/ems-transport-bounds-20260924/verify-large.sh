#!/bin/bash
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
evidence="$root/evidence/ems-transport-bounds-20260924"
build=/var/tmp/ems-transport-bounds-20260924-build
timeout 45s g++ -std=c++14 -O1 -pthread -I "$root/include" "$root/tools/ems_cluster_transport_bounds_test.cpp" "$root/src/ems_cluster.cpp" "$root/src/ems_cluster_dispatch.cpp" "$root/src/ems_cluster_transport.cpp" -Wl,--wrap=accept -Wl,--wrap=recv -Wl,--wrap=recvfrom -Wl,--wrap=send -Wl,--wrap=poll -Wl,--wrap=__recv_chk -Wl,--wrap=__recvfrom_chk -o "$build/transport-cxx14-test" > "$evidence/cxx14-targeted-build.log" 2>&1
timeout 10s unshare --mount --net --ipc --pid --fork --mount-proc sh "$evidence/isolated.sh" "$build/transport-cxx14-test" "$build" large-frame-slow-partial > "$evidence/cxx14-targeted-test.log" 2>&1
cat "$evidence/cxx14-targeted-test.log"
