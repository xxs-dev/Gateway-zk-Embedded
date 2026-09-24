#!/bin/bash
set -eu
root=$(cd "$(dirname "$0")/../../.." && pwd)
evidence="$root/evidence/ems-transport-bounds-20260924/asan-poll-fix"
runner="$root/evidence/ems-transport-bounds-20260924/isolated.sh"
build=/var/tmp/ems-transport-bounds-kecp2-interface-20260924-build
binary="$build/transport-asan-poll-fix-test"
sources=("$root/tools/ems_cluster_transport_bounds_test.cpp" "$root/src/ems_cluster.cpp" "$root/src/ems_cluster_dispatch.cpp" "$root/src/ems_cluster_transport.cpp")
wrappers=(-Wl,--wrap=accept -Wl,--wrap=recv -Wl,--wrap=recvfrom -Wl,--wrap=send -Wl,--wrap=poll -Wl,--wrap=__recv_chk -Wl,--wrap=__recvfrom_chk -Wl,--wrap=__poll_chk)
g++ --version > "$evidence/compiler.log"
timeout 15s g++ -std=c++17 -O1 -g -fsanitize=address,undefined -I "$root/include" -dM -E "${sources[0]}" | grep -E '^#define (_FORTIFY_SOURCE|__USE_FORTIFY_LEVEL|__GLIBC__|__GLIBC_MINOR__) ' > "$evidence/fortify.log"
set +e
timeout 90s g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -pthread -I "$root/include" "${sources[@]}" "${wrappers[@]}" -o "$binary" > "$evidence/build.log" 2>&1
build_result=$?
printf 'stage\texit\nbuild\t%s\n' "$build_result" > "$evidence/results.tsv"
if [ "$build_result" -eq 0 ]; then
    timeout 25s env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 unshare --mount --net --ipc --pid --fork --mount-proc sh "$runner" "$binary" "$build" writer-deadlines > "$evidence/writer-deadlines.log" 2>&1
    test_result=$?
    printf 'asan-ubsan-writer-deadlines\t%s\n' "$test_result" >> "$evidence/results.tsv"
    sha256sum "$binary" > "$evidence/native-test-sha256.txt"
    objdump -d -C "$binary" | grep -B 10 -A 3 'call.*__wrap___poll_chk' > "$evidence/checked-poll-call.log"
else
    printf 'asan-ubsan-writer-deadlines\tNOT_RUN\n' >> "$evidence/results.tsv"
    test_result=$build_result
fi
set -e
sha256sum "$root/src/ems_cluster_transport.cpp" "$root/include/edge_gateway/ems_cluster_transport.hpp" "$root/tools/ems_cluster_transport_bounds_test.cpp" "$root/CMakeLists.txt" > "$evidence/source-sha256.txt"
cat "$evidence/results.tsv"
exit "$test_result"
