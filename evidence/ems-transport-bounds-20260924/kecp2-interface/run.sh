#!/bin/bash
set -eu
root=$(cd "$(dirname "$0")/../../.." && pwd)
evidence="$root/evidence/ems-transport-bounds-20260924/kecp2-interface"
runner="$root/evidence/ems-transport-bounds-20260924/isolated.sh"
build=/var/tmp/ems-transport-bounds-kecp2-interface-20260924-build
g++ --version > "$evidence/compiler.log"
printf 'stage\texit\n' > "$evidence/results.tsv"
failed=0
run_step() {
    local stage=$1
    shift
    local result=0
    "$@" > "$evidence/$stage.log" 2>&1 || result=$?
    printf '%s\t%s\n' "$stage" "$result" >> "$evidence/results.tsv"
    printf '%s exit=%s\n' "$stage" "$result"
    if [ "$result" -ne 0 ]; then failed=1; fi
    return "$result"
}
if run_step configure timeout 30s cmake -S "$root" -B "$build" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug; then
    if run_step native-build timeout 180s cmake --build "$build" --target ems_cluster_transport_bounds_test ems_cluster_test -j2; then
        run_step transport-test timeout 25s unshare --mount --net --ipc --pid --fork --mount-proc sh "$runner" "$build/ems_cluster_transport_bounds_test" "$build" || true
        run_step core-loopback-test timeout 45s unshare --mount --net --ipc --pid --fork --mount-proc sh "$runner" "$build/ems_cluster_test" "$build" || true
    else
        printf 'transport-test\tNOT_RUN\ncore-loopback-test\tNOT_RUN\n' >> "$evidence/results.tsv"
    fi
fi
sources=("$root/tools/ems_cluster_transport_bounds_test.cpp" "$root/src/ems_cluster.cpp" "$root/src/ems_cluster_dispatch.cpp" "$root/src/ems_cluster_transport.cpp")
wrappers=(-Wl,--wrap=accept -Wl,--wrap=recv -Wl,--wrap=recvfrom -Wl,--wrap=send -Wl,--wrap=poll -Wl,--wrap=__recv_chk -Wl,--wrap=__recvfrom_chk)
if run_step cxx14-build timeout 45s g++ -std=c++14 -O1 -pthread -I "$root/include" "${sources[@]}" "${wrappers[@]}" -o "$build/transport-cxx14-test"; then
    run_step cxx14-test timeout 25s unshare --mount --net --ipc --pid --fork --mount-proc sh "$runner" "$build/transport-cxx14-test" "$build" || true
else
    printf 'cxx14-test\tNOT_RUN\n' >> "$evidence/results.tsv"
fi
if run_step asan-ubsan-build timeout 90s g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -pthread -I "$root/include" "${sources[@]}" "${wrappers[@]}" -o "$build/transport-asan-test"; then
    run_step asan-ubsan-test timeout 25s env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 unshare --mount --net --ipc --pid --fork --mount-proc sh "$runner" "$build/transport-asan-test" "$build" || true
else
    printf 'asan-ubsan-test\tNOT_RUN\n' >> "$evidence/results.tsv"
fi
sha256sum "$root/src/ems_cluster.cpp" "$root/src/ems_cluster_dispatch.cpp" "$root/include/edge_gateway/ems_cluster.hpp" "$root/include/edge_gateway/ems_cluster_transport.hpp" "$root/src/ems_cluster_transport.cpp" "$root/tools/ems_cluster_transport_bounds_test.cpp" "$root/CMakeLists.txt" > "$evidence/source-sha256.txt"
for binary in ems_cluster_transport_bounds_test ems_cluster_test transport-cxx14-test transport-asan-test; do
    if [ -f "$build/$binary" ]; then sha256sum "$build/$binary"; fi
done > "$evidence/native-test-sha256.txt"
cat "$evidence/results.tsv"
exit "$failed"
