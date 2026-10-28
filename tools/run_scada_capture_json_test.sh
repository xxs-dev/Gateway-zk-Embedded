#!/usr/bin/env bash
# Native test only; production cross compilation belongs on 22.11.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
objects=${1:?Existing native Qt object directory}
output=${2:?New test output directory}
project=${3:?SCADA project directory}
mkdir -p "$output"
read -r -a flags <<< "$(pkg-config --cflags Qt5Widgets)"
read -r -a libs <<< "$(pkg-config --libs Qt5Widgets)"
deps=()
for name in local_display_qt_scada_scene.cpp local_display_qt_access_control.cpp local_display_qt_pcs_power_control.cpp local_display_qt_value_map.cpp scada_project_loader.cpp; do
    deps+=("$objects/$name.o")
done
for name in scada_capture_json_test scada_capture_readonly; do
    g++ -std=c++17 -fPIC -I"$root" -I"$root/include" "${flags[@]}" "$root/tools/$name.cpp" "${deps[@]}" "${libs[@]}" -o "$output/$name"
done
"$output/scada_capture_json_test" "$project"
# No point-store or shared-memory entrypoints may be linked into the helper.
if nm -C "$output/scada_capture_readonly" | grep -E 'MemoryPointStore|shm_open|pthread_mutex'; then
    echo 'Forbidden shared-memory symbol in helper' >&2
    exit 1
fi
