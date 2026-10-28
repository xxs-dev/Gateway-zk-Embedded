#!/usr/bin/env bash
# Run only on the 22.11 compiler host, from an isolated source snapshot.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
output=${1:?Pass a new absolute build output directory}
test ! -e "$output"
case "$output" in /*) ;; *) echo 'Absolute output required' >&2; exit 1 ;; esac
sysroot=/opt/ky-cross/allwinner-sysroot
bin=/home/tronlong/Linux/SZR/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin
cxx=$bin/aarch64-linux-gnu-g++
qt=$sysroot/usr/include/aarch64-linux-gnu/qt5
lib=$sysroot/usr/lib/aarch64-linux-gnu
test -x "$cxx"
test -f "$qt/QtCore/qglobal.h"
mkdir -p "$output"
flags=(--sysroot="$sysroot" -std=gnu++17 -O2 -fPIC -DQT_WIDGETS_LIB -DQT_GUI_LIB -DQT_CORE_LIB
       -I"$root" -I"$root/include" -I"$sysroot/usr/include" -I"$sysroot/usr/include/aarch64-linux-gnu"
       -I"$qt" -I"$qt/QtCore" -I"$qt/QtGui" -I"$qt/QtWidgets")
objects=()
# Deliberately no libedge_gateway.a or point-store objects.
for source in tools/scada_capture_readonly.cpp local_display_qt_scada_scene.cpp local_display_qt_access_control.cpp local_display_qt_pcs_power_control.cpp local_display_qt_value_map.cpp src/scada_project_loader.cpp; do
    object="$output/$(basename "$source").o"
    "$cxx" "${flags[@]}" -c "$root/$source" -o "$object"
    objects+=("$object")
done
"$cxx" --sysroot="$sysroot" -no-pie "${objects[@]}" -L"$lib" -Wl,-rpath-link="$lib" \
    -lQt5Widgets -lQt5Gui -lQt5Core -lGLESv2 -ldl -o "$output/scada_capture_json"
"$bin/aarch64-linux-gnu-nm" -C "$output/scada_capture_json" > "$output/symbols.txt"
if grep -E 'MemoryPointStore|shm_open|pthread_mutex' "$output/symbols.txt"; then
    echo 'Forbidden shared-memory dependency' >&2
    exit 1
fi
"$bin/aarch64-linux-gnu-readelf" -h "$output/scada_capture_json" > "$output/elf-header.txt"
grep -q 'AArch64' "$output/elf-header.txt"
sha256sum "$output/scada_capture_json" | tee "$output/binary.sha256"
