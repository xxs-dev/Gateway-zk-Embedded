#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
OUT="$ROOT_DIR/gateway-factory-defaults.tar.gz"
PACKAGE_PROFILE="${PACKAGE_PROFILE:-full}"
EDGE_PACKAGE_MANIFEST="${EDGE_PACKAGE_MANIFEST:-}"
COMPONENT_VERSION="1.0"
TMP_DIR="${TMPDIR:-/tmp}/gateway-factory-defaults.$$"

usage() {
  cat >&2 <<'EOF'
Usage: build-factory-package.sh [OUT] [--profile base|project|full] [--manifest FILE] [--out OUT]

Profiles:
  base     Package only SystemMonitor, MqttDriver and pointctl.
  project  Package base components plus drivers listed in edge-package-manifest.json.
  full     Package all current drivers and tools; default for backward compatibility.

All current driver binaries are recorded as version 1.0 in the generated manifest.
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --profile)
      [ "$#" -ge 2 ] || { echo "--profile requires a value" >&2; exit 2; }
      PACKAGE_PROFILE="$2"
      shift 2
      ;;
    --manifest)
      [ "$#" -ge 2 ] || { echo "--manifest requires a value" >&2; exit 2; }
      EDGE_PACKAGE_MANIFEST="$2"
      shift 2
      ;;
    --out)
      [ "$#" -ge 2 ] || { echo "--out requires a value" >&2; exit 2; }
      OUT="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      OUT="$1"
      shift
      ;;
  esac
done

case "$PACKAGE_PROFILE" in
  base|project|full) ;;
  *) echo "invalid profile: $PACKAGE_PROFILE" >&2; usage; exit 2 ;;
esac

cleanup() {
  rm -rf "$TMP_DIR"
}

trap cleanup EXIT INT TERM

if ! command -v tar >/dev/null 2>&1; then
  echo "tar command not found" >&2
  exit 1
fi

manifest_binaries() {
  manifest="$1"
  [ -n "$manifest" ] && [ -f "$manifest" ] || return 0
  if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 command not found, cannot read manifest: $manifest" >&2
    exit 2
  fi
  python3 - "$manifest" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as fh:
    data = json.load(fh)

items = data.get("requiredDrivers") or data.get("components") or []
seen = set()
for item in items:
    if isinstance(item, str):
        name = item
    elif isinstance(item, dict):
        name = item.get("binary") or item.get("name") or item.get("id")
    else:
        continue
    name = str(name or "").strip()
    if not name or name in seen:
        continue
    seen.add(name)
    print(name)
PY
}

json_escape() {
  printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

write_package_manifest() {
  out="$1"
  profile="$2"
  shift 2
  {
    printf '{\n'
    printf '  "schemaVersion": "1.0",\n'
    printf '  "packageVersion": "%s",\n' "$COMPONENT_VERSION"
    printf '  "createdAt": "%s",\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    printf '  "createdBy": "build-factory-package.sh",\n'
    printf '  "packageProfile": "%s",\n' "$profile"
    printf '  "requiredDrivers": [\n'
    first=1
    for bin in "$@"; do
      [ -n "$bin" ] || continue
      if [ "$first" -eq 0 ]; then
        printf ',\n'
      fi
      first=0
      escaped=$(json_escape "$bin")
      printf '    {"binary": "%s", "name": "%s", "version": "%s"}' "$escaped" "$escaped" "$COMPONENT_VERSION"
    done
    printf '\n  ]\n'
    printf '}\n'
  } > "$out"
}

unique_words() {
  awk 'NF && !seen[$0]++ { print }'
}

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
  for file in "$ROOT_DIR/deploy"/*; do
    [ -f "$file" ] && cp "$file" "$TMP_DIR/gateway-factory-defaults/deploy/$(basename "$file")"
  done
fi
if [ -d "$ROOT_DIR/build-aarch64" ]; then
  mkdir -p "$TMP_DIR/gateway-factory-defaults/build-aarch64"
  BASE_BINS="SystemMonitor MqttDriver pointctl"
  ALL_BINS="ModbusRtu Dlt645Driver DioDriver CanDriver IecDriver MqttDriver EventEngine ComputeEngine SystemMonitor pointctl"
  OPTIONAL_BINS="LocalDisplay QtDisplayBridge CameraService stress_runner"
  REQUIRED_BINS="$ALL_BINS"
  if [ "$PACKAGE_PROFILE" = "base" ]; then
    REQUIRED_BINS="$BASE_BINS"
    OPTIONAL_BINS=""
  elif [ "$PACKAGE_PROFILE" = "project" ]; then
    if [ -z "$EDGE_PACKAGE_MANIFEST" ] || [ ! -f "$EDGE_PACKAGE_MANIFEST" ]; then
      echo "project profile requires --manifest edge-package-manifest.json" >&2
      exit 2
    fi
    cp "$EDGE_PACKAGE_MANIFEST" "$TMP_DIR/gateway-factory-defaults/edge-package-manifest.json"
    REQUIRED_BINS=$(printf '%s\n' $BASE_BINS $(manifest_binaries "$EDGE_PACKAGE_MANIFEST") | unique_words | tr '\n' ' ')
    OPTIONAL_BINS=""
  fi
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
  if [ ! -f "$TMP_DIR/gateway-factory-defaults/edge-package-manifest.json" ]; then
    write_package_manifest "$TMP_DIR/gateway-factory-defaults/edge-package-manifest.json" "$PACKAGE_PROFILE" $REQUIRED_BINS $OPTIONAL_BINS
  fi
else
  echo "build-aarch64 directory not found; cross compile before packaging" >&2
  exit 2
fi

mkdir -p "$(dirname "$OUT")"
tar -C "$TMP_DIR" -czf "$OUT" gateway-factory-defaults

echo "factory package created: $OUT"
echo "factory package profile: $PACKAGE_PROFILE"
