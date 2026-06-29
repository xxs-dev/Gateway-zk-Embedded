#!/usr/bin/env bash
if [ -z "${BASH_VERSION:-}" ]; then
  exec bash "$0" "$@"
fi
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-aarch64-cross}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
TOOLCHAIN_FILE="${TOOLCHAIN_FILE:-$ROOT_DIR/toolchains/aarch64-linux-gnu.cmake}"
TOOLCHAIN_BIN="${TOOLCHAIN_BIN:-/home/tronlong/Linux/SZR/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
PUBLISH=0
PACKAGE=0
PACKAGE_PROFILE="${PACKAGE_PROFILE:-full}"
PACKAGE_OUT="${PACKAGE_OUT:-$ROOT_DIR/gateway-factory-defaults.tar.gz}"
MANIFEST=""
CLEAN=0

PRODUCTION_TARGETS=(
  ModbusRtu
  Dlt645Driver
  DioDriver
  CanDriver
  IecDriver
  MqttDriver
  EventEngine
  ComputeEngine
  SystemMonitor
  LocalDisplay
  QtDisplayBridge
  CameraService
  pointctl
  stress_runner
)

usage() {
  cat <<'EOF'
Usage: tools/build_edge_aarch64.sh [options] [target ...]

Build Gateway-zk edge binaries on the 192.168.22.11 cross-compile machine.

Options:
  --build-dir DIR       Build output dir; default: ./build-aarch64-cross
  --build-type TYPE     CMake build type; default: Release
  --jobs N              Parallel build jobs
  --clean               Remove build dir before configuring
  --publish             Copy built production binaries to ./build-aarch64
  --package             Also run deploy/build-factory-package.sh after publish
  --profile PROFILE     Factory package profile: base, project, full
  --manifest FILE       Manifest for project package profile
  --out FILE            Factory package output path
  -h, --help            Show this help

When no target is supplied, production targets are built. The script does not
touch ./build-aarch64 unless --publish or --package is passed.
EOF
}

targets=()
while [ "$#" -gt 0 ]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="${2:-}"
      shift 2
      ;;
    --build-type)
      BUILD_TYPE="${2:-}"
      shift 2
      ;;
    --jobs|-j)
      JOBS="${2:-}"
      shift 2
      ;;
    --clean)
      CLEAN=1
      shift
      ;;
    --publish)
      PUBLISH=1
      shift
      ;;
    --package)
      PACKAGE=1
      PUBLISH=1
      shift
      ;;
    --profile)
      PACKAGE_PROFILE="${2:-}"
      shift 2
      ;;
    --manifest)
      MANIFEST="${2:-}"
      shift 2
      ;;
    --out)
      PACKAGE_OUT="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      while [ "$#" -gt 0 ]; do
        targets+=("$1")
        shift
      done
      ;;
    -*)
      echo "unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *)
      targets+=("$1")
      shift
      ;;
  esac
done

if [ ! -f "$TOOLCHAIN_FILE" ]; then
  echo "toolchain file not found: $TOOLCHAIN_FILE" >&2
  exit 2
fi

if [ -d "$TOOLCHAIN_BIN" ]; then
  export PATH="$TOOLCHAIN_BIN:$PATH"
fi

if ! command -v aarch64-linux-gnu-g++ >/dev/null 2>&1; then
  echo "aarch64-linux-gnu-g++ not found; expected toolchain at: $TOOLCHAIN_BIN" >&2
  exit 2
fi

if [ "${#targets[@]}" -eq 0 ]; then
  targets=("${PRODUCTION_TARGETS[@]}")
fi

if [ "$CLEAN" = "1" ]; then
  rm -rf "$BUILD_DIR"
fi

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCMAKE_COLOR_DIAGNOSTICS=OFF

cmake --build "$BUILD_DIR" --target "${targets[@]}" -- -j"$JOBS"

echo
echo "Built targets:"
for target in "${targets[@]}"; do
  if [ -f "$BUILD_DIR/$target" ]; then
    file "$BUILD_DIR/$target"
    ls -lh "$BUILD_DIR/$target"
  fi
done

if [ "$PUBLISH" = "1" ]; then
  mkdir -p "$ROOT_DIR/build-aarch64"
  for bin in "${PRODUCTION_TARGETS[@]}"; do
    if [ -f "$BUILD_DIR/$bin" ]; then
      cp -f "$BUILD_DIR/$bin" "$ROOT_DIR/build-aarch64/$bin"
      chmod +x "$ROOT_DIR/build-aarch64/$bin"
    fi
  done
  echo "published production binaries to: $ROOT_DIR/build-aarch64"
fi

if [ "$PACKAGE" = "1" ]; then
  args=("$PACKAGE_OUT" "--profile" "$PACKAGE_PROFILE")
  if [ -n "$MANIFEST" ]; then
    args+=("--manifest" "$MANIFEST")
  fi
  sh "$ROOT_DIR/deploy/build-factory-package.sh" "${args[@]}"
fi
