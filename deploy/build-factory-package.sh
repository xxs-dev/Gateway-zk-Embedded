#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
OUT="${1:-$ROOT_DIR/gateway-factory-defaults.tar.gz}"
TMP_DIR="${TMPDIR:-/tmp}/gateway-factory-defaults.$$"

cleanup() {
  rm -rf "$TMP_DIR"
}

trap cleanup EXIT INT TERM

if ! command -v tar >/dev/null 2>&1; then
  echo "tar command not found" >&2
  exit 1
fi

rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR/gateway-factory-defaults/config"

cp -a "$ROOT_DIR/config/factory" "$TMP_DIR/gateway-factory-defaults/config/factory"
if [ -d "$ROOT_DIR/config/templates" ]; then
  cp -a "$ROOT_DIR/config/templates" "$TMP_DIR/gateway-factory-defaults/config/templates"
fi
if [ -d "$ROOT_DIR/config/examples" ]; then
  cp -a "$ROOT_DIR/config/examples" "$TMP_DIR/gateway-factory-defaults/config/examples"
fi
if [ -f "$ROOT_DIR/config/README.md" ]; then
  cp "$ROOT_DIR/config/README.md" "$TMP_DIR/gateway-factory-defaults/config/README.md"
fi
if [ -d "$ROOT_DIR/deploy" ]; then
  mkdir -p "$TMP_DIR/gateway-factory-defaults/deploy"
  for file in \
    build-factory-package.sh \
    gateway-run.sh \
    gateway-services.sh \
    install-factory-config.sh \
    local-kiosk.py \
    ota-apply.sh \
    ota-rollback.sh \
    production-smoke-test.sh; do
    [ -f "$ROOT_DIR/deploy/$file" ] && cp "$ROOT_DIR/deploy/$file" "$TMP_DIR/gateway-factory-defaults/deploy/$file"
  done
  for service in "$ROOT_DIR/deploy"/*.service; do
    [ -f "$service" ] && cp "$service" "$TMP_DIR/gateway-factory-defaults/deploy/$(basename "$service")"
  done
fi
if [ -d "$ROOT_DIR/build-aarch64" ]; then
  mkdir -p "$TMP_DIR/gateway-factory-defaults/build-aarch64"
  for bin in ModbusRtu Dlt645Driver DioDriver CanDriver MqttDriver EventEngine ComputeEngine SystemMonitor LocalDisplay CameraService pointctl stress_runner; do
    [ -f "$ROOT_DIR/build-aarch64/$bin" ] && cp "$ROOT_DIR/build-aarch64/$bin" "$TMP_DIR/gateway-factory-defaults/build-aarch64/$bin"
  done
fi

mkdir -p "$(dirname "$OUT")"
tar -C "$TMP_DIR" -czf "$OUT" gateway-factory-defaults

echo "factory package created: $OUT"
