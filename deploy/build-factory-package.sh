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
FACTORY_BINARY_SOURCE_DIR="${FACTORY_BINARY_SOURCE_DIR:-$ROOT_DIR/build-aarch64}"
FACTORY_KY_EMS_BINARY="${FACTORY_KY_EMS_BINARY:-$ROOT_DIR/ky-ems/KY-EMS}"
SCADA_PROJECT_PACKAGE="${SCADA_PROJECT_PACKAGE:-}"
SCADA_PROJECT_SHA256="${SCADA_PROJECT_SHA256:-}"
SCADA_PROJECT_MACHINE_CODE="${SCADA_PROJECT_MACHINE_CODE:-}"
SCADA_PROJECT_CANONICAL_SHA256="${SCADA_PROJECT_CANONICAL_SHA256:-}"
SCADA_PROJECT_CONTENT_SHA256="${SCADA_PROJECT_CONTENT_SHA256:-}"
ALLOW_DIRTY_SOURCE="${ALLOW_DIRTY_SOURCE:-0}"
TMP_DIR="${TMPDIR:-/tmp}/gateway-factory-defaults.$$"
OUT_TMP=""

usage() {
  cat >&2 <<'EOF'
Usage: build-factory-package.sh [OUT] [--profile base|project|full] [--manifest FILE] [--out OUT]
                                [--scada-package FILE --scada-sha256 SHA256
                                 --scada-machine-code CODE]
                                [--scada-canonical-sha256 SHA256
                                 --scada-content-sha256 SHA256
                                 --scada-machine-code CODE]

Profiles:
  base     Package only SystemMonitor, MqttDriver and pointctl.
  project  Package base components plus drivers listed in edge-package-manifest.json.
  full     Package all current drivers and tools; default for backward compatibility.

All current driver binaries are recorded as version 1.0 in the generated manifest.
Packages containing KY-EMS must either embed a hash-pinned SCADA project or
declare the canonical/content hashes required from an external active-project restore.
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
    --scada-package)
      [ "$#" -ge 2 ] || { echo "--scada-package requires a value" >&2; exit 2; }
      SCADA_PROJECT_PACKAGE="$2"
      shift 2
      ;;
    --scada-sha256)
      [ "$#" -ge 2 ] || { echo "--scada-sha256 requires a value" >&2; exit 2; }
      SCADA_PROJECT_SHA256="$2"
      shift 2
      ;;
    --scada-machine-code)
      [ "$#" -ge 2 ] || { echo "--scada-machine-code requires a value" >&2; exit 2; }
      SCADA_PROJECT_MACHINE_CODE="$2"
      shift 2
      ;;
    --scada-canonical-sha256)
      [ "$#" -ge 2 ] || { echo "--scada-canonical-sha256 requires a value" >&2; exit 2; }
      SCADA_PROJECT_CANONICAL_SHA256="$2"
      shift 2
      ;;
    --scada-content-sha256)
      [ "$#" -ge 2 ] || { echo "--scada-content-sha256 requires a value" >&2; exit 2; }
      SCADA_PROJECT_CONTENT_SHA256="$2"
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
if ! command -v gzip >/dev/null 2>&1; then
  echo "gzip command not found" >&2
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
    if name.lower().replace("-", "").replace("_", "") == "directagent":
        raise SystemExit(
            "retired standalone maintenance agent is not allowed in factory package manifest: " + name
        )
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
  scada_relative="$8"
  scada_sha256="$9"
  scada_machine_code="${10}"
  scada_canonical_sha256="${11}"
  scada_content_sha256="${12}"
  if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 command not found, cannot generate verifiable package manifest" >&2
    exit 2
  fi
  python3 - "$out" "$profile" "$payload_root" "$required_bins" "$packaged_bins" \
    "$source_commit" "$source_dirty" "$COMPONENT_VERSION" "$EDGE_TOOLCHAIN_ID" \
    "$scada_relative" "$scada_sha256" "$scada_machine_code" \
    "$scada_canonical_sha256" "$scada_content_sha256" <<'PY'
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
    scada_relative,
    scada_sha256,
    scada_machine_code,
    scada_canonical_sha256,
    scada_content_sha256,
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

scada_project = None
if scada_relative:
    relative = scada_relative.replace("\\", "/")
    expected = scada_sha256.strip().lower()
    if len(expected) != 64 or any(ch not in "0123456789abcdef" for ch in expected):
        raise SystemExit("invalid SCADA project SHA256")
    if not scada_machine_code.strip():
        raise SystemExit("SCADA project machine code is required")
    root = os.path.realpath(payload_root)
    path = os.path.realpath(os.path.join(root, relative))
    if os.path.commonpath((root, path)) != root or not os.path.isfile(path):
        raise SystemExit(f"SCADA project is missing or outside package: {relative}")
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    actual = digest.hexdigest()
    if actual != expected:
        raise SystemExit(f"SCADA project SHA256 mismatch: expected={expected} actual={actual}")
    scada_project = {
        "required": True,
        "sourceMode": "embedded-package",
        "path": relative,
        "sizeBytes": os.path.getsize(path),
        "sha256": actual,
        "machineCode": scada_machine_code.strip(),
        "runtimeRoot": "/opt/modbus-gateway/scada",
        "currentLink": "/opt/modbus-gateway/scada/current",
        "restartServicesDuringInstall": False,
    }
    if scada_canonical_sha256 or scada_content_sha256:
        canonical = scada_canonical_sha256.strip().lower()
        content = scada_content_sha256.strip().lower()
        for label, value in (("canonical", canonical), ("content", content)):
            if len(value) != 64 or any(ch not in "0123456789abcdef" for ch in value):
                raise SystemExit(f"invalid embedded SCADA {label} SHA256")
        scada_project["canonicalManifestSha256"] = canonical
        scada_project["contentManifestSha256"] = content
elif scada_canonical_sha256 or scada_content_sha256:
    canonical = scada_canonical_sha256.strip().lower()
    content = scada_content_sha256.strip().lower()
    for label, value in (("canonical", canonical), ("content", content)):
        if len(value) != 64 or any(ch not in "0123456789abcdef" for ch in value):
            raise SystemExit(f"invalid external SCADA {label} SHA256")
    if not scada_machine_code.strip():
        raise SystemExit("SCADA project machine code is required")
    scada_project = {
        "required": True,
        "sourceMode": "external-active-project",
        "machineCode": scada_machine_code.strip(),
        "canonicalManifestSha256": canonical,
        "contentManifestSha256": content,
        "runtimeRoot": "/opt/modbus-gateway/scada",
        "currentLink": "/opt/modbus-gateway/scada/current",
        "requiredBeforeServiceStart": True,
        "restartServicesDuringInstall": False,
    }

created_at = datetime.datetime.fromtimestamp(
    int(os.environ.get("SOURCE_DATE_EPOCH", "0")), datetime.timezone.utc
).replace(microsecond=0).isoformat().replace("+00:00", "Z")
manifest.update({
    "schemaVersion": "1.2",
    "packageVersion": component_version,
    "createdAt": created_at,
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
if scada_project is None:
    manifest.pop("scadaProject", None)
else:
    manifest["scadaProject"] = scada_project
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

assert_no_retired_maintenance_agent() {
  root="$1"
  [ -e "$root" ] || return 0
  found=$(find "$root" -type f -print | grep -Ei '(^|/)direct[-_]?agent([^/]*)$' || true)
  if [ -n "$found" ]; then
    echo "retired standalone maintenance agent artifact is not allowed in factory packages:" >&2
    printf '%s\n' "$found" >&2
    exit 2
  fi
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
if [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
  if [ "$SOURCE_COMMIT" != "unknown" ]; then
    SOURCE_DATE_EPOCH=$(git -C "$ROOT_DIR" show -s --format=%ct "$SOURCE_COMMIT")
  else
    SOURCE_DATE_EPOCH=0
  fi
fi
case "$SOURCE_DATE_EPOCH" in
  ''|*[!0-9]*) echo "SOURCE_DATE_EPOCH must be a non-negative integer" >&2; exit 2 ;;
esac
export SOURCE_DATE_EPOCH

rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR/gateway-factory-defaults/config"

assert_no_retired_maintenance_agent "$ROOT_DIR/config/factory"
assert_no_retired_maintenance_agent "$ROOT_DIR/deploy"

cp -a "$ROOT_DIR/config/factory" "$TMP_DIR/gateway-factory-defaults/config/factory"
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
    [ "$name" = "agc-avc@.service" ] && continue
    [ "$name" = "build-agc-avc-runtime-package.sh" ] && continue
    [ -f "$file" ] && cp "$file" "$TMP_DIR/gateway-factory-defaults/deploy/$name"
  done
fi
if [ -d "$FACTORY_BINARY_SOURCE_DIR" ]; then
  mkdir -p "$TMP_DIR/gateway-factory-defaults/build-aarch64"
  BASE_BINS="SystemMonitor MqttDriver pointctl"
  ALL_BINS="ModbusRtu Dlt645Driver DioDriver CanDriver IecDriver MqttDriver EventEngine ComputeEngine EmsParityCheck EmsClusterCoordinator SystemMonitor pointctl"
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
      REQUIRED_BINS=$(printf '%s\n' $REQUIRED_BINS ComputeEngine EmsParityCheck EmsClusterCoordinator | unique_words | tr '\n' ' ')
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
      [ -d "$ROOT_DIR/ky-ems" ] && [ -f "$FACTORY_KY_EMS_BINARY" ] || {
        echo "required KY-EMS payload missing: $FACTORY_KY_EMS_BINARY" >&2
        exit 2
      }
      continue
    fi
    if [ ! -f "$FACTORY_BINARY_SOURCE_DIR/$bin" ]; then
      echo "required factory binary missing: $FACTORY_BINARY_SOURCE_DIR/$bin" >&2
      exit 2
    fi
    cp "$FACTORY_BINARY_SOURCE_DIR/$bin" "$TMP_DIR/gateway-factory-defaults/build-aarch64/$bin"
  done
  for bin in $OPTIONAL_BINS; do
    [ "$bin" = "KY-EMS" ] && continue
    if [ -f "$FACTORY_BINARY_SOURCE_DIR/$bin" ]; then
      cp "$FACTORY_BINARY_SOURCE_DIR/$bin" "$TMP_DIR/gateway-factory-defaults/build-aarch64/$bin"
      PACKAGED_BINS=$(printf '%s\n' $PACKAGED_BINS "$bin" | unique_words | tr '\n' ' ')
    fi
  done
else
  echo "factory binary source directory not found: $FACTORY_BINARY_SOURCE_DIR" >&2
  exit 2
fi

if [ -d "$ROOT_DIR/ky-ems" ] && [ -f "$FACTORY_KY_EMS_BINARY" ]; then
  mkdir -p "$TMP_DIR/gateway-factory-defaults/ky-ems"
  cp -a "$ROOT_DIR/ky-ems"/. "$TMP_DIR/gateway-factory-defaults/ky-ems"/
  cp "$FACTORY_KY_EMS_BINARY" "$TMP_DIR/gateway-factory-defaults/ky-ems/KY-EMS"
  case " $REQUIRED_BINS $OPTIONAL_BINS " in
    *" KY-EMS "*) PACKAGED_BINS=$(printf '%s\n' $PACKAGED_BINS KY-EMS | unique_words | tr '\n' ' ') ;;
  esac
fi

SCADA_PROJECT_RELATIVE=""
case " $PACKAGED_BINS " in
  *" KY-EMS "*)
    if [ -n "$SCADA_PROJECT_PACKAGE" ] || [ -n "$SCADA_PROJECT_SHA256" ]; then
      if [ -z "$SCADA_PROJECT_PACKAGE" ] || [ -z "$SCADA_PROJECT_SHA256" ] || \
         [ -z "$SCADA_PROJECT_MACHINE_CODE" ] || [ -z "$SCADA_PROJECT_CANONICAL_SHA256" ] || \
         [ -z "$SCADA_PROJECT_CONTENT_SHA256" ]; then
        echo "embedded SCADA requires package, package SHA256, machine code and canonical/content SHA256 values" >&2
        exit 2
      fi
      [ -f "$SCADA_PROJECT_PACKAGE" ] || {
        echo "SCADA project package not found: $SCADA_PROJECT_PACKAGE" >&2
        exit 2
      }
      sh "$ROOT_DIR/deploy/install-scada-project.sh" \
        --package "$SCADA_PROJECT_PACKAGE" \
        --package-sha256 "$SCADA_PROJECT_SHA256" \
        --canonical-manifest-sha256 "$SCADA_PROJECT_CANONICAL_SHA256" \
        --content-manifest-sha256 "$SCADA_PROJECT_CONTENT_SHA256" \
        --machine-code "$SCADA_PROJECT_MACHINE_CODE" \
        --app-config "$ROOT_DIR/config/factory/runtime/apps/monitor-service.json" \
        --scada-root "$TMP_DIR/scada-validation" \
        --require-local-scada \
        --no-restart \
        --dry-run
      SCADA_PROJECT_RELATIVE="scada/active-project.kyscada"
      mkdir -p "$TMP_DIR/gateway-factory-defaults/scada"
      cp "$SCADA_PROJECT_PACKAGE" "$TMP_DIR/gateway-factory-defaults/$SCADA_PROJECT_RELATIVE"
    elif [ -n "$SCADA_PROJECT_CANONICAL_SHA256" ] || [ -n "$SCADA_PROJECT_CONTENT_SHA256" ]; then
      if [ -z "$SCADA_PROJECT_CANONICAL_SHA256" ] || [ -z "$SCADA_PROJECT_CONTENT_SHA256" ] || [ -z "$SCADA_PROJECT_MACHINE_CODE" ]; then
        echo "external SCADA restore requires canonical SHA256, content SHA256 and machine code" >&2
        exit 2
      fi
    else
      echo "packages containing KY-EMS require an embedded SCADA package or fixed external canonical/content SHA256 values" >&2
      exit 2
    fi
    ;;
  *)
    if [ -n "$SCADA_PROJECT_PACKAGE" ] || [ -n "$SCADA_PROJECT_SHA256" ] || \
       [ -n "$SCADA_PROJECT_CANONICAL_SHA256" ] || [ -n "$SCADA_PROJECT_CONTENT_SHA256" ] || \
       [ -n "$SCADA_PROJECT_MACHINE_CODE" ]; then
      echo "SCADA project input requires a packaged KY-EMS runtime" >&2
      exit 2
    fi
    ;;
esac

finalize_package_manifest \
  "$TMP_DIR/gateway-factory-defaults/edge-package-manifest.json" \
  "$PACKAGE_PROFILE" \
  "$TMP_DIR/gateway-factory-defaults" \
  "$REQUIRED_BINS" \
  "$PACKAGED_BINS" \
  "$SOURCE_COMMIT" \
  "$SOURCE_DIRTY" \
  "$SCADA_PROJECT_RELATIVE" \
  "$SCADA_PROJECT_SHA256" \
  "$SCADA_PROJECT_MACHINE_CODE" \
  "$SCADA_PROJECT_CANONICAL_SHA256" \
  "$SCADA_PROJECT_CONTENT_SHA256"

assert_no_retired_maintenance_agent "$TMP_DIR/gateway-factory-defaults"

mkdir -p "$(dirname "$OUT")"
PACKAGE_ARCHIVE="$TMP_DIR/gateway-factory-defaults.tar.gz"
PACKAGE_TAR="$TMP_DIR/gateway-factory-defaults.tar"
tar --sort=name --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 --numeric-owner \
  -C "$TMP_DIR" -cf "$PACKAGE_TAR" gateway-factory-defaults
gzip -n -9 <"$PACKAGE_TAR" >"$PACKAGE_ARCHIVE"
rm -f "$PACKAGE_TAR"
tar -tzf "$PACKAGE_ARCHIVE" >/dev/null
if tar -tzf "$PACKAGE_ARCHIVE" | grep -Eiq '(^|/)direct[-_]?agent([^/]*)$'; then
  echo "retired standalone maintenance agent was found in generated factory package" >&2
  exit 2
fi
python3 "$ROOT_DIR/tools/audit_release_credentials.py" "$PACKAGE_ARCHIVE"

OUT_TMP="$OUT.tmp.$$"
cp "$PACKAGE_ARCHIVE" "$OUT_TMP"
tar -tzf "$OUT_TMP" >/dev/null
mv -f "$OUT_TMP" "$OUT"
OUT_TMP=""
tar -tzf "$OUT" >/dev/null

echo "factory package created: $OUT"
echo "factory package profile: $PACKAGE_PROFILE"
