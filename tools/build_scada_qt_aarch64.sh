#!/usr/bin/env bash
if [ -z "${BASH_VERSION:-}" ]; then
  exec bash "$0" "$@"
fi
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-scada-qt-aarch64}"
EDGE_BUILD_DIR="${EDGE_BUILD_DIR:-$ROOT_DIR/build-aarch64-cross}"
SYSROOT="${KY_EMS_SYSROOT:-/opt/ky-cross/allwinner-sysroot}"
TOOLCHAIN_BIN="${TOOLCHAIN_BIN:-/home/tronlong/Linux/SZR/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin}"
CXX="$TOOLCHAIN_BIN/aarch64-linux-gnu-g++"
CLEAN=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --build-dir) BUILD_DIR="${2:-}"; shift 2 ;;
    --edge-build-dir) EDGE_BUILD_DIR="${2:-}"; shift 2 ;;
    --sysroot) SYSROOT="${2:-}"; shift 2 ;;
    --toolchain-bin) TOOLCHAIN_BIN="${2:-}"; CXX="$TOOLCHAIN_BIN/aarch64-linux-gnu-g++"; shift 2 ;;
    --clean) CLEAN=1; shift ;;
    -h|--help)
      echo "Usage: build_scada_qt_aarch64.sh [--clean] [--build-dir DIR] [--edge-build-dir DIR]"
      exit 0
      ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

[ -x "$CXX" ] || { echo "aarch64 compiler not found: $CXX" >&2; exit 2; }
[ -f "$EDGE_BUILD_DIR/libedge_gateway.a" ] || {
  echo "edge runtime library not found: $EDGE_BUILD_DIR/libedge_gateway.a" >&2
  echo "build it first with tools/build_edge_aarch64.sh" >&2
  exit 2
}
[ -f "$SYSROOT/usr/include/aarch64-linux-gnu/qt5/QtCore/qglobal.h" ] || {
  echo "target Qt headers not found in sysroot: $SYSROOT" >&2
  exit 2
}

if [ "$CLEAN" -eq 1 ]; then
  rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR/obj"

include_flags=(
  "-I$ROOT_DIR"
  "-I$ROOT_DIR/include"
  "-I$SYSROOT/usr/include"
  "-I$SYSROOT/usr/include/aarch64-linux-gnu"
  "-I$SYSROOT/usr/include/aarch64-linux-gnu/qt5"
  "-I$SYSROOT/usr/include/aarch64-linux-gnu/qt5/QtCore"
  "-I$SYSROOT/usr/include/aarch64-linux-gnu/qt5/QtGui"
  "-I$SYSROOT/usr/include/aarch64-linux-gnu/qt5/QtWidgets"
)

common_flags=(
  --sysroot="$SYSROOT"
  -std=gnu++17
  -O2
  -fPIC
  -ffunction-sections
  -fdata-sections
  -fdiagnostics-color=never
  -DQT_NO_DEBUG
  -DQT_WIDGETS_LIB
  -DQT_GUI_LIB
  -DQT_CORE_LIB
  "${include_flags[@]}"
)

sources=(
  "$ROOT_DIR/local_display_qt_ems_main.cpp"
  "$ROOT_DIR/local_display_qt_scada_scene.cpp"
  "$ROOT_DIR/local_display_qt_access_control.cpp"
  "$ROOT_DIR/local_display_qt_pcs_power_control.cpp"
  "$ROOT_DIR/local_display_qt_value_map.cpp"
)
objects=()
for source in "${sources[@]}"; do
  object="$BUILD_DIR/obj/$(basename "${source%.cpp}").o"
  "$CXX" "${common_flags[@]}" -c "$source" -o "$object"
  objects+=("$object")
done

lib_dir="$SYSROOT/usr/lib/aarch64-linux-gnu"
"$CXX" --sysroot="$SYSROOT" \
  -no-pie \
  -Wl,--gc-sections \
  -Wl,-rpath-link="$lib_dir" \
  -o "$BUILD_DIR/KY-SCADA" \
  "${objects[@]}" \
  "$EDGE_BUILD_DIR/libedge_gateway.a" \
  -L"$lib_dir" \
  -lQt5Widgets -lQt5Gui -lQt5Core -lGLESv2 \
  -ldl -lpthread -lrt

"$TOOLCHAIN_BIN/aarch64-linux-gnu-strip" --strip-unneeded "$BUILD_DIR/KY-SCADA"
file "$BUILD_DIR/KY-SCADA"
ls -lh "$BUILD_DIR/KY-SCADA"
