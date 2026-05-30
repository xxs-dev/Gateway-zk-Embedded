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
    batch-init-devices.sh \
    devices.example.csv \
    gateway-tls-enroll.sh \
    gateway-run.sh \
    gateway-services.sh \
    install-factory-config.sh \
    local-kiosk.py \
    ota-apply.sh \
    ota-rollback.sh \
    production-init.sh \
    production-smoke-test.sh; do
    [ -f "$ROOT_DIR/deploy/$file" ] && cp "$ROOT_DIR/deploy/$file" "$TMP_DIR/gateway-factory-defaults/deploy/$file"
  done
  for service in "$ROOT_DIR/deploy"/*.service; do
    [ -f "$service" ] && cp "$service" "$TMP_DIR/gateway-factory-defaults/deploy/$(basename "$service")"
  done
fi
if [ -d "$ROOT_DIR/build-aarch64" ]; then
  mkdir -p "$TMP_DIR/gateway-factory-defaults/build-aarch64"
  REQUIRED_BINS="ModbusRtu Dlt645Driver DioDriver CanDriver MqttDriver EventEngine ComputeEngine SystemMonitor pointctl"
  OPTIONAL_BINS="LocalDisplay CameraService DirectAgent stress_runner"
  for bin in $REQUIRED_BINS; do
    if [ ! -f "$ROOT_DIR/build-aarch64/$bin" ]; then
      echo "required factory binary missing: build-aarch64/$bin" >&2
      exit 2
    fi
    cp "$ROOT_DIR/build-aarch64/$bin" "$TMP_DIR/gateway-factory-defaults/build-aarch64/$bin"
  done
  for bin in $OPTIONAL_BINS; do
    [ -f "$ROOT_DIR/build-aarch64/$bin" ] && cp "$ROOT_DIR/build-aarch64/$bin" "$TMP_DIR/gateway-factory-defaults/build-aarch64/$bin"
  done
else
  echo "build-aarch64 directory not found; cross compile before packaging" >&2
  exit 2
fi

mkdir -p "$(dirname "$OUT")"
tar -C "$TMP_DIR" -czf "$OUT" gateway-factory-defaults

echo "factory package created: $OUT"
