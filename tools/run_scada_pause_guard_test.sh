#!/usr/bin/env bash
# Native Linux Qt tests/renderer only. Production AArch64 builds stay on 22.11.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
output=${1:?Pass a new isolated output directory}
mkdir -p "$output"
output=$(cd "$output" && pwd)
qt=Qt5Widgets
if ! pkg-config --exists "$qt"; then qt=Qt6Widgets; fi
read -r -a qt_flags <<< "$(pkg-config --cflags "$qt")"
read -r -a qt_libs <<< "$(pkg-config --libs "$qt")"
objects=()
for source in local_display_qt_scada_scene.cpp local_display_qt_access_control.cpp local_display_qt_pcs_power_control.cpp local_display_qt_value_map.cpp src/scada_project_loader.cpp; do
    object="$output/$(basename "$source").o"
    g++ -std=c++17 -fPIC -O0 -g -I"$root" -I"$root/include" "${qt_flags[@]}" -c "$root/$source" -o "$object"
    objects+=("$object")
done
g++ -std=c++17 -fPIC -I"$root" -I"$root/include" "${qt_flags[@]}" "$root/tools/scada_pause_guard_test.cpp" "${objects[@]}" "${qt_libs[@]}" -o "$output/scada_pause_guard_test"
QT_QPA_PLATFORM=offscreen timeout 60 "$output/scada_pause_guard_test"
g++ -std=c++17 -fPIC -I"$root" -I"$root/include" "${qt_flags[@]}" "$root/scada_windows_renderer_main.cpp" "${objects[@]}" "${qt_libs[@]}" -o "$output/scada_offline_renderer"
printf 'Offline Qt renderer: %s/scada_offline_renderer\n' "$output"
