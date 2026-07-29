#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
OUT="$ROOT_DIR/gateway-factory-defaults.tar.gz"
PACKAGE_PROFILE="${PACKAGE_PROFILE:-full}"
EDGE_PACKAGE_MANIFEST="${EDGE_PACKAGE_MANIFEST:-}"
COMPONENT_VERSION="${COMPONENT_VERSION:-1.0}"
EDGE_TOOLCHAIN_ID="${EDGE_TOOLCHAIN_ID:-unknown}"
EDGE_PACKAGE_BUILD_DIR="${EDGE_PACKAGE_BUILD_DIR:-}"
ALLOW_DIRTY_SOURCE="${ALLOW_DIRTY_SOURCE:-0}"
TMP_DIR="${TMPDIR:-/tmp}/gateway-factory-defaults.$$"
OUT_TMP=""

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
  [ -z "$OUT_TMP" ] || rm -f "$OUT_TMP"
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

items = []
for key in ("requiredDrivers", "components", "runtimeComponents"):
    value = data.get(key) or []
    if isinstance(value, list):
        items.extend(value)
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

finalize_package_manifest() {
  out="$1"
  profile="$2"
  payload_root="$3"
  required_bins="$4"
  packaged_bins="$5"
  source_commit="$6"
  source_dirty="$7"
  if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 command not found, cannot generate verifiable package manifest" >&2
    exit 2
  fi
  python3 - "$out" "$profile" "$payload_root" "$required_bins" "$packaged_bins" \
    "$source_commit" "$source_dirty" "$COMPONENT_VERSION" "$EDGE_TOOLCHAIN_ID" <<'PY'
import datetime
import hashlib
import json
import os
import sys

(
    output,
    profile,
    payload_root,
    required_raw,
    packaged_raw,
    source_commit,
    source_dirty_raw,
    component_version,
    toolchain_id,
) = sys.argv[1:]

try:
    with open(output, "r", encoding="utf-8") as fh:
        manifest = json.load(fh)
except FileNotFoundError:
    manifest = {}

required = list(dict.fromkeys(required_raw.split()))
packaged = list(dict.fromkeys(packaged_raw.split()))
components = []
for binary in packaged:
    relative = os.path.join("ky-ems", "KY-EMS") if binary == "KY-EMS" else os.path.join("build-aarch64", binary)
    path = os.path.join(payload_root, relative)
    if not os.path.isfile(path):
        raise SystemExit(f"packaged component is missing while finalizing manifest: {relative}")
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    components.append({
        "binary": binary,
        "name": binary,
        "version": component_version,
        "required": binary in required,
        "path": relative.replace(os.sep, "/"),
        "sizeBytes": os.path.getsize(path),
        "sha256": digest.hexdigest(),
    })

manifest.update({
    "schemaVersion": "1.1",
    "packageVersion": component_version,
    "createdAt": datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
    "createdBy": "build-factory-package.sh",
    "packageProfile": profile,
    "sourceCommit": source_commit,
    "sourceDirty": source_dirty_raw == "true",
    "toolchain": toolchain_id,
    "requiredDrivers": [
        {"binary": binary, "name": binary, "version": component_version}
        for binary in required
    ],
    "components": components,
})
tmp = output + ".tmp"
with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
    json.dump(manifest, fh, ensure_ascii=False, indent=2)
    fh.write("\n")
os.replace(tmp, output)
PY
}

factory_requires_ems_runtime() {
  factory_root="$1"
  python3 - "$factory_root" <<'PY'
import json
import os
import sys

apps = os.path.join(sys.argv[1], "runtime", "apps")
required = False
if os.path.isdir(apps):
    for name in os.listdir(apps):
        if not name.endswith(".json"):
            continue
        try:
            with open(os.path.join(apps, name), "r", encoding="utf-8") as fh:
                root = json.load(fh)
        except Exception:
            continue
        compute = root.get("computeEngine")
        if not isinstance(compute, dict) or not compute.get("enabled"):
            continue
        for rule in compute.get("rules") or []:
            script = rule.get("script") if isinstance(rule, dict) else None
            if (isinstance(script, dict) and
                    str(script.get("type", "")).lower() == "graphems" and
                    rule.get("enabled", True)):
                required = True
                break
        if required:
            break
print("true" if required else "false")
PY
}

unique_words() {
  awk 'NF && !seen[$0]++ { print }'
}

SOURCE_COMMIT="unknown"
SOURCE_DIRTY="false"
SOURCE_BUILD_DIR_REL=""
case "$EDGE_PACKAGE_BUILD_DIR" in
  "$ROOT_DIR"/*) SOURCE_BUILD_DIR_REL=${EDGE_PACKAGE_BUILD_DIR#"$ROOT_DIR"/} ;;
esac

source_status() {
  set -- \
    . \
    ':(exclude)build-aarch64/**' \
    ':(exclude)gateway-factory-defaults.tar.gz' \
    ':(exclude)ky-ems/KY-EMS'
  if [ -n "$SOURCE_BUILD_DIR_REL" ]; then
    set -- "$@" \
      ":(exclude)$SOURCE_BUILD_DIR_REL" \
      ":(exclude)$SOURCE_BUILD_DIR_REL/**"
  fi
  git -C "$ROOT_DIR" status --porcelain --untracked-files=normal -- "$@"
}

if command -v git >/dev/null 2>&1 && git -C "$ROOT_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  SOURCE_COMMIT=$(git -C "$ROOT_DIR" rev-parse HEAD)
  SOURCE_CHANGES=$(source_status || true)
  [ -z "$SOURCE_CHANGES" ] || SOURCE_DIRTY="true"
fi
if [ "$SOURCE_DIRTY" = "true" ] && [ "$ALLOW_DIRTY_SOURCE" != "1" ]; then
  echo "source tree contains uncommitted source/config changes; commit them or set ALLOW_DIRTY_SOURCE=1 for a non-production package" >&2
  exit 2
fi

rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR/gateway-factory-defaults/config"

cp -a "$ROOT_DIR/config/factory" "$TMP_DIR/gateway-factory-defaults/config/factory"
rm -f "$TMP_DIR/gateway-factory-defaults/config/factory/runtime/apps/direct-agent.json"
rm -f "$TMP_DIR/gateway-factory-defaults/config/factory/runtime/apps/agc-avc-service.json"
rm -f "$TMP_DIR/gateway-factory-defaults/config/factory/runtime/devices/device_agc_avc_virtual.json"
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
    name=$(basename "$file")
    [ "$name" = "direct-agent@.service" ] && continue
    [ "$name" = "agc-avc@.service" ] && continue
    [ "$name" = "build-agc-avc-runtime-package.sh" ] && continue
    [ -f "$file" ] && cp "$file" "$TMP_DIR/gateway-factory-defaults/deploy/$name"
  done
fi
if [ -d "$ROOT_DIR/build-aarch64" ]; then
  mkdir -p "$TMP_DIR/gateway-factory-defaults/build-aarch64"
  BASE_BINS="SystemMonitor MqttDriver pointctl"
  ALL_BINS="ModbusRtu Dlt645Driver DioDriver CanDriver IecDriver MqttDriver EventEngine ComputeEngine EmsParityCheck SystemMonitor pointctl"
  OPTIONAL_BINS="LocalDisplay QtDisplayBridge KY-EMS CameraService stress_runner"
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
    MANIFEST_BINS=$(manifest_binaries "$EDGE_PACKAGE_MANIFEST")
    REQUIRED_BINS=$(printf '%s\n' $BASE_BINS $MANIFEST_BINS | unique_words | tr '\n' ' ')
    if [ "$(factory_requires_ems_runtime "$TMP_DIR/gateway-factory-defaults/config/factory")" = "true" ]; then
      REQUIRED_BINS=$(printf '%s\n' $REQUIRED_BINS ComputeEngine EmsParityCheck | unique_words | tr '\n' ' ')
    fi
    OPTIONAL_BINS=""
  fi
  case " $REQUIRED_BINS " in
    *" KY-EMS "*)
      REQUIRED_BINS=$(printf '%s\n' $REQUIRED_BINS QtDisplayBridge | unique_words | tr '\n' ' ')
      ;;
  esac
  PACKAGED_BINS="$REQUIRED_BINS"
  for bin in $REQUIRED_BINS; do
    if [ "$bin" = "KY-EMS" ]; then
      [ -d "$ROOT_DIR/ky-ems" ] && [ -f "$ROOT_DIR/ky-ems/KY-EMS" ] || {
        echo "required KY-EMS payload missing: ky-ems/KY-EMS" >&2
        exit 2
      }
      continue
    fi
    if [ ! -f "$ROOT_DIR/build-aarch64/$bin" ]; then
      echo "required factory binary missing: build-aarch64/$bin" >&2
      exit 2
    fi
    cp "$ROOT_DIR/build-aarch64/$bin" "$TMP_DIR/gateway-factory-defaults/build-aarch64/$bin"
  done
  for bin in $OPTIONAL_BINS; do
    [ "$bin" = "KY-EMS" ] && continue
    if [ -f "$ROOT_DIR/build-aarch64/$bin" ]; then
      cp "$ROOT_DIR/build-aarch64/$bin" "$TMP_DIR/gateway-factory-defaults/build-aarch64/$bin"
      PACKAGED_BINS=$(printf '%s\n' $PACKAGED_BINS "$bin" | unique_words | tr '\n' ' ')
    fi
  done
else
  echo "build-aarch64 directory not found; cross compile before packaging" >&2
  exit 2
fi

if [ -d "$ROOT_DIR/ky-ems" ]; then
  mkdir -p "$TMP_DIR/gateway-factory-defaults/ky-ems"
  cp -a "$ROOT_DIR/ky-ems"/. "$TMP_DIR/gateway-factory-defaults/ky-ems"/
  case " $REQUIRED_BINS $OPTIONAL_BINS " in
    *" KY-EMS "*) PACKAGED_BINS=$(printf '%s\n' $PACKAGED_BINS KY-EMS | unique_words | tr '\n' ' ') ;;
  esac
fi

finalize_package_manifest \
  "$TMP_DIR/gateway-factory-defaults/edge-package-manifest.json" \
  "$PACKAGE_PROFILE" \
  "$TMP_DIR/gateway-factory-defaults" \
  "$REQUIRED_BINS" \
  "$PACKAGED_BINS" \
  "$SOURCE_COMMIT" \
  "$SOURCE_DIRTY"

mkdir -p "$(dirname "$OUT")"
PACKAGE_ARCHIVE="$TMP_DIR/gateway-factory-defaults.tar.gz"
tar -C "$TMP_DIR" -czf "$PACKAGE_ARCHIVE" gateway-factory-defaults
tar -tzf "$PACKAGE_ARCHIVE" >/dev/null

OUT_TMP="$OUT.tmp.$$"
cp "$PACKAGE_ARCHIVE" "$OUT_TMP"
tar -tzf "$OUT_TMP" >/dev/null
mv -f "$OUT_TMP" "$OUT"
OUT_TMP=""
tar -tzf "$OUT" >/dev/null

echo "factory package created: $OUT"
echo "factory package profile: $PACKAGE_PROFILE"
