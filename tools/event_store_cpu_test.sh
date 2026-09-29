#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 1 || $1 != /* ]]; then
    echo 'usage: bash tools/event_store_cpu_test.sh /absolute/new-build-directory' >&2
    exit 2
fi
build_dir=$1
mkdir "$build_dir"
repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo_dir"
compiler=${CXX:-g++}
flags=(-std=c++17 -O2 -g -pthread -Iinclude)
objects=()
for unit in event_store_runtime mqtt_event_outbox mqtt_event_stats event_store_stats; do
    "$compiler" "${flags[@]}" -c "src/$unit.cpp" -o "$build_dir/$unit.o"
    objects+=("$build_dir/$unit.o")
done
"$compiler" -std=c++17 -O2 -fPIC -shared tools/sqlite_outbox_failure_fixture.cpp -ldl -o "$build_dir/fault.so"
for test in event_store_cpu event_store_index_init event_history_projection event_store_history_runtime event_store event_store_capacity event_store_stats; do
    extra=()
    if [[ $test == event_store_cpu ]]; then extra+=(-Wl,--wrap=realpath); fi
    "$compiler" "${flags[@]}" "tools/${test}_test.cpp" "${objects[@]}" -ldl "${extra[@]}" -o "$build_dir/${test}_test"
done
{
    "$build_dir/event_store_cpu_test" --check
    "$build_dir/event_store_index_init_test"
    "$build_dir/event_history_projection_test"
    "$build_dir/event_store_history_runtime_test"
    "$build_dir/event_store_test" "$build_dir/fault.so"
    "$build_dir/event_store_capacity_test"
    "$build_dir/event_store_stats_test"
} 2>&1 | tee "$build_dir/regression.txt"
