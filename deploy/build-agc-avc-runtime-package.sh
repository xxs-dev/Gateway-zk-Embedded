#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
OUT="${1:-$ROOT_DIR/gateway-agc-avc-runtime.tar.gz}"
TMP_DIR="${TMPDIR:-/tmp}/gateway-agc-avc-runtime.$$"

cleanup() {
  rm -rf "$TMP_DIR"
}

trap cleanup EXIT INT TERM

require_file() {
  [ -f "$1" ] || {
    echo "required AGC/AVC runtime file missing: $1" >&2
    exit 2
  }
}

require_file "$ROOT_DIR/build-aarch64/AgcAvcController"
require_file "$ROOT_DIR/config/factory/runtime/apps/agc-avc-service.json"
require_file "$ROOT_DIR/config/factory/runtime/devices/device_agc_avc_virtual.json"
require_file "$ROOT_DIR/deploy/agc-avc@.service"

ROOT="$TMP_DIR/gateway-factory-defaults"
mkdir -p "$ROOT/build-aarch64" "$ROOT/config/factory/runtime/apps" \
  "$ROOT/config/factory/runtime/devices" "$ROOT/deploy"

cp "$ROOT_DIR/build-aarch64/AgcAvcController" "$ROOT/build-aarch64/AgcAvcController"
cp "$ROOT_DIR/config/factory/runtime/apps/agc-avc-service.json" \
  "$ROOT/config/factory/runtime/apps/agc-avc-service.json"
cp "$ROOT_DIR/config/factory/runtime/devices/device_agc_avc_virtual.json" \
  "$ROOT/config/factory/runtime/devices/device_agc_avc_virtual.json"
cp "$ROOT_DIR/deploy/agc-avc@.service" "$ROOT/deploy/agc-avc@.service"

cat > "$ROOT/agc-avc-runtime-manifest.json" <<'EOF'
{
  "schemaVersion": "1.0",
  "runtimeMode": "agc_avc",
  "runtimeComponents": [
    {"id": "agcAvc", "binary": "AgcAvcController", "version": "1.0"}
  ]
}
EOF

mkdir -p "$(dirname "$OUT")"
tar -czf "$OUT" -C "$TMP_DIR" gateway-factory-defaults

echo "AGC/AVC runtime package created: $OUT"
