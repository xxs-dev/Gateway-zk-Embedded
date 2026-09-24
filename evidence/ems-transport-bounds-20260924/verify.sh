#!/bin/bash
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
evidence="$root/evidence/ems-transport-bounds-20260924"
build=/var/tmp/ems-transport-bounds-20260924-build
runner="$evidence/isolated.sh"
g++ --version > "$evidence/compiler.log"
set +e
timeout 15s g++ -std=c++11 -pedantic-errors -Wall -Wextra -I "$root/include" -fsyntax-only "$root/src/ems_cluster_transport.cpp" > "$evidence/cxx11-header-limitation.log" 2>&1
printf 'exit=%s\n' "$?" >> "$evidence/cxx11-header-limitation.log"
set -e
timeout 15s g++ -std=c++14 -pedantic-errors -Wall -Wextra -Werror -I "$root/include" -fsyntax-only "$root/src/ems_cluster_transport.cpp" "$root/tools/ems_cluster_transport_bounds_test.cpp" > "$evidence/cxx14-syntax.log" 2>&1
printf 'exit=0\n' >> "$evidence/cxx14-syntax.log"
timeout 30s cmake -S "$root" -B "$build" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug > "$evidence/regression-configure.log" 2>&1
timeout 180s cmake --build "$build" --target ems_cluster_transport_bounds_test ems_cluster_test graph_ems_cluster_dispatch_test -j2 > "$evidence/regression-build.log" 2>&1
for test in ems_cluster_test graph_ems_cluster_dispatch_test; do
    timeout 45s unshare --mount --net --ipc --pid --fork --mount-proc sh "$runner" "$build/$test" "$build" > "$evidence/$test.log" 2>&1
    cat "$evidence/$test.log"
done
for run in 1 2 3 4 5; do
    timeout 25s unshare --mount --net --ipc --pid --fork --mount-proc sh "$runner" "$build/ems_cluster_transport_bounds_test" "$build" > "$evidence/repeat-$run.log" 2>&1
done
sources=("$root/tools/ems_cluster_transport_bounds_test.cpp" "$root/src/ems_cluster.cpp" "$root/src/ems_cluster_dispatch.cpp" "$root/src/ems_cluster_transport.cpp")
wrappers=(-Wl,--wrap=accept -Wl,--wrap=recv -Wl,--wrap=recvfrom -Wl,--wrap=send -Wl,--wrap=poll -Wl,--wrap=__recv_chk -Wl,--wrap=__recvfrom_chk)
timeout 45s g++ -std=c++14 -O1 -pthread -I "$root/include" "${sources[@]}" "${wrappers[@]}" -o "$build/transport-cxx14-test" > "$evidence/cxx14-build.log" 2>&1
timeout 25s unshare --mount --net --ipc --pid --fork --mount-proc sh "$runner" "$build/transport-cxx14-test" "$build" > "$evidence/cxx14-test.log" 2>&1
timeout 60s g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -pthread -I "$root/include" "${sources[@]}" "${wrappers[@]}" -o "$build/transport-sanitizer-test" > "$evidence/sanitizer-build.log" 2>&1
timeout 25s unshare --mount --net --ipc --pid --fork --mount-proc sh "$runner" "$build/transport-sanitizer-test" "$build" > "$evidence/sanitizer-test.log" 2>&1
sha256sum "$build/ems_cluster_transport_bounds_test" "$build/ems_cluster_test" "$build/graph_ems_cluster_dispatch_test" > "$evidence/native-test-sha256.txt"
sha256sum "$root/src/ems_cluster_transport.cpp" "$root/tools/ems_cluster_transport_bounds_test.cpp" "$root/CMakeLists.txt" > "$evidence/source-sha256.txt"
printf 'regression_five_repeats_cxx14_sanitizers=PASS\n'
