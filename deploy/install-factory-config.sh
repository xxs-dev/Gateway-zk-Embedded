#!/bin/sh
set -eu

GATEWAY_HOME="${GATEWAY_HOME:-/opt/modbus-gateway}"
DEFAULT_SOURCE_ROOT="${DEFAULT_SOURCE_ROOT:-/home/gateway-factory}"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
BACKUP_DIR="${BACKUP_DIR:-$GATEWAY_HOME/backup}"
START_SERVICES="${START_SERVICES:-1}"
RESET_SHM="${RESET_SHM:-0}"
INSTALL_SYSTEMD="${INSTALL_SYSTEMD:-1}"
SYSTEMD_UNIT_DIR="${SYSTEMD_UNIT_DIR:-/etc/systemd/system}"
SYSTEM_DEFAULT_DIR="${SYSTEM_DEFAULT_DIR:-/etc/default}"
SYSTEMD_SYSTEM_CONF_DIR="${SYSTEMD_SYSTEM_CONF_DIR:-/etc/systemd/system.conf.d}"
FACTORY_PACKAGE_NAME="${FACTORY_PACKAGE_NAME:-gateway-factory-defaults.tar.gz}"
PACKAGE_PROFILE="${PACKAGE_PROFILE:-}"
EDGE_PACKAGE_MANIFEST="${EDGE_PACKAGE_MANIFEST:-}"
INIT_DIRECT_MAINTENANCE_ENABLED="${INIT_DIRECT_MAINTENANCE_ENABLED:-1}"
INIT_DIRECT_LISTEN_HOSTS="${INIT_DIRECT_LISTEN_HOSTS:-}"
INIT_DIRECT_ALLOWED_CIDRS="${INIT_DIRECT_ALLOWED_CIDRS:-}"
FACTORY_EXTRACT_DIR=""

usage() {
  cat >&2 <<'EOF'
Usage: install-factory-config.sh [--profile base|project|full] [--manifest FILE]
                                 [--direct-maintenance|--no-direct-maintenance]
                                 [--direct-listen-host HOST|--direct-listen-hosts HOSTS]
                                 [--direct-allowed-cidr CIDR|--direct-allowed-cidrs CIDRS]

Profiles:
  base     Install only SystemMonitor, MqttDriver and pointctl.
  project  Install base components plus binaries listed in edge-package-manifest.json.
  full     Install all current drivers and tools; default when no package manifest exists.

Direct maintenance is served by SystemMonitor's embedded HTTP API, not a standalone service.
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
    --direct-maintenance)
      INIT_DIRECT_MAINTENANCE_ENABLED=1
      shift
      ;;
    --no-direct-maintenance)
      INIT_DIRECT_MAINTENANCE_ENABLED=0
      shift
      ;;
    --direct-listen-host)
      [ "$#" -ge 2 ] || { echo "--direct-listen-host requires a value" >&2; exit 2; }
      INIT_DIRECT_LISTEN_HOSTS="$2"
      shift 2
      ;;
    --direct-listen-hosts)
      [ "$#" -ge 2 ] || { echo "--direct-listen-hosts requires a value" >&2; exit 2; }
      INIT_DIRECT_LISTEN_HOSTS="$2"
      shift 2
      ;;
    --direct-allowed-cidr)
      [ "$#" -ge 2 ] || { echo "--direct-allowed-cidr requires a value" >&2; exit 2; }
      INIT_DIRECT_ALLOWED_CIDRS="$2"
      shift 2
      ;;
    --direct-allowed-cidrs)
      [ "$#" -ge 2 ] || { echo "--direct-allowed-cidrs requires a value" >&2; exit 2; }
      INIT_DIRECT_ALLOWED_CIDRS="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown option: $1" >&2
      usage
      exit 2
      ;;
  esac
done

cleanup_factory_extract() {
  if [ -n "$FACTORY_EXTRACT_DIR" ] && [ -d "$FACTORY_EXTRACT_DIR" ]; then
    rm -rf "$FACTORY_EXTRACT_DIR"
  fi
}

trap cleanup_factory_extract EXIT INT TERM

path_abs() {
  path="$1"
  dir=$(dirname -- "$path")
  base=$(basename -- "$path")
  if [ -d "$dir" ]; then
    (CDPATH= cd -- "$dir" && printf '%s/%s\n' "$(pwd -P)" "$base")
  else
    printf '%s\n' "$path"
  fi
}

same_path() {
  [ "$(path_abs "$1")" = "$(path_abs "$2")" ]
}

pick_existing_dir() {
  for dir in "$@"; do
    if [ -n "$dir" ] && [ -d "$dir" ]; then
      printf '%s\n' "$dir"
      return 0
    fi
  done
  return 1
}

pick_existing_file() {
  for file in "$@"; do
    if [ -n "$file" ] && [ -f "$file" ]; then
      printf '%s\n' "$file"
      return 0
    fi
  done
  return 1
}

pick_source_root() {
  for dir in "$@"; do
    if [ -n "$dir" ] && { [ -d "$dir/config/factory/runtime" ] || [ -d "$dir/deploy" ] || [ -d "$dir/build-aarch64" ]; }; then
      printf '%s\n' "$dir"
      return 0
    fi
  done
  return 1
}

extract_factory_package() {
  package="$1"
  if [ -z "$package" ] || [ ! -f "$package" ]; then
    return 1
  fi
  if ! command -v tar >/dev/null 2>&1; then
    echo "tar command not found, cannot extract factory package: $package" >&2
    return 1
  fi
  FACTORY_EXTRACT_DIR="/tmp/gateway-factory-defaults.$$"
  rm -rf "$FACTORY_EXTRACT_DIR"
  mkdir -p "$FACTORY_EXTRACT_DIR"
  tar -xzf "$package" -C "$FACTORY_EXTRACT_DIR"
  echo "factory package extracted: $package"
  return 0
}

SOURCE_ROOT="${SOURCE_ROOT:-}"
if [ -z "$SOURCE_ROOT" ]; then
  SOURCE_ROOT=$(pick_source_root "$DEFAULT_SOURCE_ROOT" "$ROOT_DIR" "$SCRIPT_DIR" || true)
fi
DEPLOY_DIR="${DEPLOY_DIR:-$SOURCE_ROOT/deploy}"

FACTORY_PACKAGE="${FACTORY_PACKAGE:-}"
if [ -z "$FACTORY_PACKAGE" ] && [ -z "${SOURCE_ROOT:-}" ]; then
  FACTORY_PACKAGE=$(pick_existing_file \
    "$DEFAULT_SOURCE_ROOT/$FACTORY_PACKAGE_NAME" \
    "/home/$FACTORY_PACKAGE_NAME" \
    "$ROOT_DIR/$FACTORY_PACKAGE_NAME" \
    "$SCRIPT_DIR/$FACTORY_PACKAGE_NAME" \
    "$SCRIPT_DIR/../$FACTORY_PACKAGE_NAME" \
    "$GATEWAY_HOME/$FACTORY_PACKAGE_NAME" || true)
fi

PACKAGE_ROOT=""
if [ -n "$FACTORY_PACKAGE" ] && extract_factory_package "$FACTORY_PACKAGE"; then
  PACKAGE_ROOT=$(pick_source_root \
    "$FACTORY_EXTRACT_DIR" \
    "$FACTORY_EXTRACT_DIR/gateway-factory-defaults" \
    "$FACTORY_EXTRACT_DIR/gateway-factory" || true)
fi

if [ -z "${FACTORY_DIR:-}" ]; then
  FACTORY_DIR=$(pick_existing_dir "$PACKAGE_ROOT/config/factory" "$SOURCE_ROOT/config/factory" "$DEFAULT_SOURCE_ROOT/config/factory" "$ROOT_DIR/config/factory" "$SCRIPT_DIR/config/factory" || true)
fi
if [ -z "${TEMPLATES_DIR:-}" ]; then
  TEMPLATES_DIR=$(pick_existing_dir "$PACKAGE_ROOT/config/templates" "$SOURCE_ROOT/config/templates" "$DEFAULT_SOURCE_ROOT/config/templates" "$ROOT_DIR/config/templates" "$SCRIPT_DIR/config/templates" || true)
fi
if [ -z "${EXAMPLES_DIR:-}" ]; then
  EXAMPLES_DIR=$(pick_existing_dir "$PACKAGE_ROOT/config/examples" "$SOURCE_ROOT/config/examples" "$DEFAULT_SOURCE_ROOT/config/examples" "$ROOT_DIR/config/examples" "$SCRIPT_DIR/config/examples" || true)
fi
if [ -z "${DEPLOY_DIR:-}" ] || [ ! -d "$DEPLOY_DIR" ]; then
  DEPLOY_DIR=$(pick_existing_dir "$PACKAGE_ROOT/deploy" "$SOURCE_ROOT/deploy" "$DEFAULT_SOURCE_ROOT/deploy" "$ROOT_DIR/deploy" "$SCRIPT_DIR/deploy" || true)
fi

if [ ! -d "$FACTORY_DIR/runtime" ]; then
  echo "factory runtime not found: $FACTORY_DIR/runtime" >&2
  exit 1
fi

install_file_if_exists() {
  src="$1"
  dst="$2"
  if [ -f "$src" ]; then
    if same_path "$src" "$dst"; then
      return 0
    fi
    mkdir -p "$(dirname "$dst")"
    cp "$src" "$dst"
  fi
}

deploy_file() {
  name="$1"
  for candidate in \
    "$DEPLOY_DIR/$name" \
    "$PACKAGE_ROOT/deploy/$name" \
    "$SOURCE_ROOT/deploy/$name" \
    "$DEFAULT_SOURCE_ROOT/deploy/$name" \
    "$ROOT_DIR/deploy/$name" \
    "$SCRIPT_DIR/$name" \
    "$SCRIPT_DIR/deploy/$name"; do
    if [ -f "$candidate" ]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

install_deploy_file_if_exists() {
  name="$1"
  dst="$2"
  src=$(deploy_file "$name" || true)
  [ -n "$src" ] || return 0
  install_file_if_exists "$src" "$dst"
}

install_required_deploy_file() {
  name="$1"
  dst="$2"
  src=$(deploy_file "$name" || true)
  if [ -z "$src" ]; then
    echo "required deploy file missing: $name" >&2
    exit 2
  fi
  install_file_if_exists "$src" "$dst"
}

install_deploy_file_with_mode_if_exists() {
  name="$1"
  dst="$2"
  mode="$3"
  src=$(deploy_file "$name" || true)
  [ -n "$src" ] || return 0
  install_file_if_exists "$src" "$dst"
  chmod "$mode" "$dst"
}

install_required_deploy_file_with_mode() {
  name="$1"
  dst="$2"
  mode="$3"
  src=$(deploy_file "$name" || true)
  if [ -z "$src" ]; then
    echo "required deploy file missing: $name" >&2
    exit 2
  fi
  install_file_if_exists "$src" "$dst"
  chmod "$mode" "$dst"
}

find_first_file() {
  for path in "$@"; do
    if [ -f "$path" ]; then
      printf '%s\n' "$path"
      return 0
    fi
  done
  return 1
}

install_required_binary() {
  bin="$1"
  src=$(find_first_file \
    "$PACKAGE_ROOT/build-aarch64/$bin" \
    "$PACKAGE_ROOT/bin/$bin" \
    "$PACKAGE_ROOT/$bin" \
    "$SOURCE_ROOT/build-aarch64/$bin" \
    "$SOURCE_ROOT/bin/$bin" \
    "$SOURCE_ROOT/$bin" \
  ) || {
    echo "required binary missing: $bin" >&2
    exit 2
  }
  install_file_if_exists "$src" "$GATEWAY_HOME/bin/$bin"
}

install_optional_binary() {
  bin="$1"
  src=$(find_first_file \
    "$PACKAGE_ROOT/build-aarch64/$bin" \
    "$PACKAGE_ROOT/bin/$bin" \
    "$PACKAGE_ROOT/$bin" \
    "$SOURCE_ROOT/build-aarch64/$bin" \
    "$SOURCE_ROOT/bin/$bin" \
    "$SOURCE_ROOT/$bin" \
  ) || return 0
  install_file_if_exists "$src" "$GATEWAY_HOME/bin/$bin"
}

install_ky_ems_payload() {
  required="${1:-0}"
  src=""
  for candidate in \
    "$PACKAGE_ROOT/ky-ems" \
    "$SOURCE_ROOT/ky-ems" \
    "$DEFAULT_SOURCE_ROOT/ky-ems" \
    "$ROOT_DIR/ky-ems" \
    "$SCRIPT_DIR/ky-ems" \
    "$SCRIPT_DIR/../ky-ems"; do
    if [ -n "$candidate" ] && [ -d "$candidate" ] && [ -f "$candidate/KY-EMS" ]; then
      src="$candidate"
      break
    fi
  done
  if [ -z "$src" ]; then
    if [ "$required" = "1" ]; then
      echo "required KY-EMS payload missing: ky-ems/KY-EMS" >&2
      exit 2
    fi
    echo "optional KY-EMS payload missing: ky-ems/KY-EMS, skip" >&2
    return 0
  fi
  mkdir -p "$GATEWAY_HOME/ky-ems"
  cp -a "$src"/. "$GATEWAY_HOME/ky-ems"/
  chmod +x "$GATEWAY_HOME/ky-ems/KY-EMS" 2>/dev/null || true
}

manifest_profile() {
  manifest="$1"
  [ -n "$manifest" ] && [ -f "$manifest" ] || return 0
  if ! command -v python3 >/dev/null 2>&1; then
    return 0
  fi
  python3 - "$manifest" <<'PY'
import json
import sys
try:
    with open(sys.argv[1], "r", encoding="utf-8") as fh:
        data = json.load(fh)
    profile = str(data.get("packageProfile") or "").strip().lower()
    if profile in ("base", "project", "full"):
        print(profile)
except Exception:
    pass
PY
}

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

verify_manifest_components() {
  manifest="$1"
  package_root="$2"
  [ -n "$manifest" ] && [ -f "$manifest" ] || return 0
  if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 command not found, cannot verify package component hashes" >&2
    exit 2
  fi
  python3 - "$manifest" "$package_root" <<'PY'
import hashlib
import json
import os
import sys

manifest_path, package_root = sys.argv[1:3]
with open(manifest_path, "r", encoding="utf-8") as fh:
    manifest = json.load(fh)
components = manifest.get("components")
schema = str(manifest.get("schemaVersion") or "")
if not isinstance(components, list) or not components:
    if schema.startswith(("1.1", "1.2")):
        raise SystemExit(f"schema {schema} package manifest must contain hashed components")
    print("warning: legacy package manifest has no component hashes", file=sys.stderr)
    raise SystemExit(0)

root = os.path.realpath(package_root)
packaged_names = set()
for component in components:
    if not isinstance(component, dict):
        raise SystemExit("invalid component entry in package manifest")
    name = str(component.get("binary") or component.get("name") or "").strip()
    relative = str(component.get("path") or "").strip().replace("\\", "/")
    expected = str(component.get("sha256") or "").strip().lower()
    if not name or not relative or len(expected) != 64:
        raise SystemExit(f"incomplete component hash metadata: {name or '<unknown>'}")
    packaged_names.add(name)
    path = os.path.realpath(os.path.join(root, relative))
    if os.path.commonpath((root, path)) != root or not os.path.isfile(path):
        raise SystemExit(f"component path is missing or outside package: {relative}")
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    if digest.hexdigest() != expected:
        raise SystemExit(f"component SHA-256 mismatch: {name}")
    expected_size = component.get("sizeBytes")
    if expected_size is not None and int(expected_size) != os.path.getsize(path):
        raise SystemExit(f"component size mismatch: {name}")

scada = manifest.get("scadaProject")
if "KY-EMS" in packaged_names and not isinstance(scada, dict):
    raise SystemExit("package contains KY-EMS but has no required SCADA project")
if isinstance(scada, dict):
    source_mode = str(scada.get("sourceMode") or "embedded-package").strip()
    machine_code = str(scada.get("machineCode") or "").strip()
    if scada.get("required") is not True:
        raise SystemExit("SCADA project must be marked required")
    if not machine_code:
        raise SystemExit("SCADA project machine code is required")
    if source_mode == "embedded-package":
        relative = str(scada.get("path") or "").strip().replace("\\", "/")
        expected = str(scada.get("sha256") or "").strip().lower()
        if not relative.endswith(".kyscada") or len(expected) != 64:
            raise SystemExit("incomplete embedded SCADA project metadata")
        path = os.path.realpath(os.path.join(root, relative))
        if os.path.commonpath((root, path)) != root or not os.path.isfile(path):
            raise SystemExit(f"SCADA project path is missing or outside package: {relative}")
        digest = hashlib.sha256()
        with open(path, "rb") as fh:
            for chunk in iter(lambda: fh.read(1024 * 1024), b""):
                digest.update(chunk)
        if digest.hexdigest() != expected:
            raise SystemExit("SCADA project SHA-256 mismatch")
        expected_size = scada.get("sizeBytes")
        if expected_size is not None and int(expected_size) != os.path.getsize(path):
            raise SystemExit("SCADA project size mismatch")
    elif source_mode == "external-active-project":
        canonical = str(scada.get("canonicalManifestSha256") or "").strip().lower()
        content = str(scada.get("contentManifestSha256") or "").strip().lower()
        if any(len(value) != 64 or any(ch not in "0123456789abcdef" for ch in value) for value in (canonical, content)):
            raise SystemExit("external SCADA project requires canonical/content SHA256 metadata")
        if scada.get("requiredBeforeServiceStart") is not True:
            raise SystemExit("external SCADA project must be required before service start")
    else:
        raise SystemExit(f"unsupported SCADA project sourceMode: {source_mode}")
print(f"verified package component hashes: {len(components)}; scadaMode={scada.get('sourceMode') if isinstance(scada, dict) else 'none'}")
PY
}

manifest_scada_value() {
  manifest="$1"
  key="$2"
  [ -n "$manifest" ] && [ -f "$manifest" ] || return 0
  python3 - "$manifest" "$key" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as fh:
    project = json.load(fh).get("scadaProject")
if isinstance(project, dict):
    value = project.get(sys.argv[2])
    if isinstance(value, str):
        print(value)
PY
}

unique_words() {
  awk 'NF && !seen[$0]++ { print }'
}

if [ -z "$EDGE_PACKAGE_MANIFEST" ]; then
  EDGE_PACKAGE_MANIFEST=$(pick_existing_file \
    "$PACKAGE_ROOT/edge-package-manifest.json" \
    "$PACKAGE_ROOT/config/runtime/edge-package-manifest.json" \
    "$SOURCE_ROOT/edge-package-manifest.json" \
    "$SOURCE_ROOT/config/runtime/edge-package-manifest.json" \
    "$DEFAULT_SOURCE_ROOT/edge-package-manifest.json" || true)
fi

PACKAGE_CONTENT_ROOT=$(pick_existing_dir \
  "$PACKAGE_ROOT" \
  "$SOURCE_ROOT" \
  "$DEFAULT_SOURCE_ROOT" \
  "$ROOT_DIR" || true)

if [ -z "$PACKAGE_PROFILE" ]; then
  PACKAGE_PROFILE=$(manifest_profile "$EDGE_PACKAGE_MANIFEST" || true)
fi
if [ -z "$PACKAGE_PROFILE" ]; then
  if [ -n "$EDGE_PACKAGE_MANIFEST" ] && [ -f "$EDGE_PACKAGE_MANIFEST" ]; then
    PACKAGE_PROFILE="project"
  else
    PACKAGE_PROFILE="full"
  fi
fi
case "$PACKAGE_PROFILE" in
  base|project|full) ;;
  *) echo "invalid package profile: $PACKAGE_PROFILE" >&2; exit 2 ;;
esac

verify_manifest_components "$EDGE_PACKAGE_MANIFEST" "$PACKAGE_CONTENT_ROOT"
SCADA_PROJECT_RELATIVE=$(manifest_scada_value "$EDGE_PACKAGE_MANIFEST" path)
SCADA_PROJECT_SHA256=$(manifest_scada_value "$EDGE_PACKAGE_MANIFEST" sha256)
SCADA_PROJECT_MACHINE_CODE=$(manifest_scada_value "$EDGE_PACKAGE_MANIFEST" machineCode)
SCADA_PROJECT_CANONICAL_SHA256=$(manifest_scada_value "$EDGE_PACKAGE_MANIFEST" canonicalManifestSha256)
SCADA_PROJECT_CONTENT_SHA256=$(manifest_scada_value "$EDGE_PACKAGE_MANIFEST" contentManifestSha256)
SCADA_PROJECT_PACKAGE=""
if [ -n "$SCADA_PROJECT_RELATIVE" ]; then
  SCADA_PROJECT_PACKAGE="$PACKAGE_CONTENT_ROOT/$SCADA_PROJECT_RELATIVE"
fi

json_string_value() {
  file="$1"
  key="$2"
  if [ -f "$file" ]; then
    sed -n "s/.*\"$key\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$file" | sed -n '1p'
  fi
}

json_tls_bool_value() {
  file="$1"
  key="$2"
  if [ -f "$file" ]; then
    awk -v key="$key" '
      /"tls"[[:space:]]*:/ { in_tls=1 }
      in_tls && $0 ~ "\"" key "\"[[:space:]]*:" {
        if ($0 ~ /true/) { print "true"; exit }
        if ($0 ~ /false/) { print "false"; exit }
      }
      in_tls && /}/ { in_tls=0 }
    ' "$file" | sed -n '1p'
  fi
}

set_json_string_value() {
  file="$1"
  key="$2"
  value="$3"
  if [ ! -f "$file" ]; then
    return 0
  fi
  tmp="$file.tmp.$$"
  if grep -q "\"$key\"" "$file"; then
    awk -v key="$key" -v value="$value" '
      function json_escape(text) {
        gsub(/\\/, "\\\\", text)
        gsub(/"/, "\\\"", text)
        gsub(/&/, "\\\\&", text)
        return text
      }
      {
        pattern = "\"" key "\"[[:space:]]*:[[:space:]]*\"[^\"]*\""
        replacement = "\"" key "\": \"" json_escape(value) "\""
        gsub(pattern, replacement)
        print
      }
    ' "$file" > "$tmp"
    mv "$tmp" "$file"
  fi
}

set_tls_bool_value() {
  file="$1"
  key="$2"
  value="$3"
  if [ ! -f "$file" ]; then
    return 0
  fi
  case "$value" in
    true|false) ;;
    *) return 0 ;;
  esac
  tmp="$file.tmp.$$"
  awk -v key="$key" -v value="$value" '
    /"tls"[[:space:]]*:/ { in_tls=1 }
    in_tls {
      pattern = "\"" key "\"[[:space:]]*:[[:space:]]*(true|false)"
      replacement = "\"" key "\": " value
      sub(pattern, replacement)
    }
    in_tls && /}/ { in_tls=0 }
    { print }
  ' "$file" > "$tmp"
  mv "$tmp" "$file"
}

first_nonempty() {
  for value in "$@"; do
    if [ -n "$value" ]; then
      printf '%s\n' "$value"
      return 0
    fi
  done
}

is_interactive_init() {
  [ "${FACTORY_PROMPT:-1}" = "1" ] && [ -t 0 ]
}

prompt_value() {
  label="$1"
  default_value="$2"
  secret="${3:-0}"
  if ! is_interactive_init; then
    printf '%s\n' "$default_value"
    return 0
  fi
  display="$default_value"
  if [ "$secret" = "1" ] && [ -n "$display" ]; then
    display="******"
  fi
  printf '%s [%s]: ' "$label" "$display" >&2
  if [ "$secret" = "1" ]; then
    stty -echo 2>/dev/null || true
  fi
  IFS= read -r input_value || input_value=""
  if [ "$secret" = "1" ]; then
    stty echo 2>/dev/null || true
    printf '\n' >&2
  fi
  if [ -n "$input_value" ]; then
    printf '%s\n' "$input_value"
  else
    printf '%s\n' "$default_value"
  fi
}

normalize_bool() {
  value=$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')
  case "$value" in
    1|y|yes|true|on) printf 'true\n' ;;
    0|n|no|false|off|"") printf 'false\n' ;;
    *) printf '%s\n' "$2" ;;
  esac
}

prompt_bool() {
  label="$1"
  default_value="$2"
  default_value=$(normalize_bool "$default_value" "false")
  if ! is_interactive_init; then
    printf '%s\n' "$default_value"
    return 0
  fi
  if [ "$default_value" = "true" ]; then
    prompt="$label [Y/n]: "
  else
    prompt="$label [y/N]: "
  fi
  printf '%s' "$prompt" >&2
  IFS= read -r input_value || input_value=""
  if [ -z "$input_value" ]; then
    printf '%s\n' "$default_value"
  else
    normalize_bool "$input_value" "$default_value"
  fi
}

broker_implies_tls() {
  case "$1" in
    ssl://*|tls://*|mqtts://*) printf 'true\n' ;;
    *) printf 'false\n' ;;
  esac
}

normalize_runtime_mode() {
  value=$(printf '%s' "${1:-gateway}" | tr '[:upper:]' '[:lower:]')
  case "$value" in
    ""|gateway) printf 'gateway\n' ;;
    ems) printf 'ems\n' ;;
    agc_avc|agc-avc) printf 'agc_avc\n' ;;
    *)
      echo "invalid runtime mode: $1 (expected gateway, ems or agc_avc)" >&2
      exit 2
      ;;
  esac
}

apply_runtime_mode() {
  runtime_mode=$(normalize_runtime_mode "$1")
  runtime_dir="$GATEWAY_HOME/config/runtime"
  [ -d "$runtime_dir/apps" ] || return 0
  if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 command not found, cannot apply runtime mode" >&2
    exit 1
  fi
  python3 - "$runtime_mode" "$runtime_dir" <<'PY'
import json
import os
import sys

mode, runtime_dir = sys.argv[1:3]
apps_dir = os.path.join(runtime_dir, "apps")
devices_dir = os.path.join(runtime_dir, "devices")
ems_virtual_name = "device_ems_virtual.json"
agc_virtual_name = "device_agc_avc_virtual.json"
agc_app_name = "agc-avc-service.json"


def read_json(path):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except FileNotFoundError:
        return None


def write_json(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(data, fh, ensure_ascii=False, indent=2)
        fh.write("\n")
    os.replace(tmp, path)


def is_ems_virtual_ref(value):
    text = str(value or "").replace("\\", "/").strip()
    return text == ems_virtual_name or text.endswith("/" + ems_virtual_name)


def is_agc_virtual_ref(value):
    text = str(value or "").replace("\\", "/").strip()
    return text == agc_virtual_name or text.endswith("/" + agc_virtual_name)


def is_graph_ems_rule(rule):
    script = (rule or {}).get("script", {})
    if not isinstance(script, dict):
        return False
    graph_file = str(script.get("graphFile", "")).replace("\\", "/")
    return str(script.get("type", "")).lower() == "graphems" or graph_file.endswith("/shuntong_ems_graph.json")


for name in sorted(os.listdir(apps_dir)):
    if not name.endswith(".json"):
        continue
    path = os.path.join(apps_dir, name)
    root = read_json(path)
    if not isinstance(root, dict):
        continue
    root["runtimeMode"] = mode
    if mode != "ems":
        files = root.get("deviceConfigFiles")
        if isinstance(files, list):
            root["deviceConfigFiles"] = [item for item in files if not is_ems_virtual_ref(item)]
        compute = root.get("computeEngine")
        if isinstance(compute, dict) and isinstance(compute.get("rules"), list):
            compute["rules"] = [rule for rule in compute["rules"] if not is_graph_ems_rule(rule)]
    if mode != "agc_avc":
        files = root.get("deviceConfigFiles")
        if isinstance(files, list):
            root["deviceConfigFiles"] = [item for item in files if not is_agc_virtual_ref(item)]
    elif name == agc_app_name:
        agc = root.setdefault("agcAvc", {})
        agc["enabled"] = True
        agc["shadowMode"] = True
        agc["submitWrites"] = False
    write_json(path, root)

if mode != "ems":
    ems_device = os.path.join(devices_dir, ems_virtual_name)
    try:
        os.remove(ems_device)
    except FileNotFoundError:
        pass
if mode != "agc_avc":
    for path in (
        os.path.join(apps_dir, agc_app_name),
        os.path.join(devices_dir, agc_virtual_name),
    ):
        try:
            os.remove(path)
        except FileNotFoundError:
            pass
PY

  if [ "$runtime_mode" != "agc_avc" ]; then
    rm -f "$GATEWAY_HOME/bin/AgcAvcController"
    rm -f /etc/systemd/system/agc-avc@.service
  fi
}

apply_runtime_identity_and_mqtt() {
  machine_code="$1"
  mqtt_broker="$2"
  mqtt_username="$3"
  mqtt_password="$4"
  mqtt_tls_enabled="$5"
  mqtt_ca_file="$6"
  mqtt_cert_file="$7"
  mqtt_key_file="$8"
  mqtt_tls_insecure="$9"

  set_json_string_value "$GATEWAY_HOME/config/runtime/device_identity.json" "machineCode" "$machine_code"
  for app_file in "$GATEWAY_HOME"/config/runtime/apps/*.json; do
    [ -f "$app_file" ] || continue
    set_json_string_value "$app_file" "clientId" "$machine_code"
    set_json_string_value "$app_file" "broker" "$mqtt_broker"
    set_json_string_value "$app_file" "username" "$mqtt_username"
    set_json_string_value "$app_file" "password" "$mqtt_password"
    set_json_string_value "$app_file" "caFile" "$mqtt_ca_file"
    set_json_string_value "$app_file" "certFile" "$mqtt_cert_file"
    set_json_string_value "$app_file" "keyFile" "$mqtt_key_file"
    set_tls_bool_value "$app_file" "enabled" "$mqtt_tls_enabled"
    set_tls_bool_value "$app_file" "insecureSkipVerify" "$mqtt_tls_insecure"
  done
}

apply_direct_maintenance_config() {
  enabled="$1"
  listen_hosts="$2"
  allowed_cidrs="$3"
  direct_config="$GATEWAY_HOME/config/runtime/apps/monitor-service.json"
  [ -f "$direct_config" ] || return 0
  if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 command not found, cannot configure direct maintenance" >&2
    exit 1
  fi
  python3 - "$direct_config" "$enabled" "$listen_hosts" "$allowed_cidrs" "$GATEWAY_HOME" <<'PY'
import json
import os
import sys

path, enabled_raw, listen_raw, cidrs_raw, gateway_home = sys.argv[1:6]


def split_list(value):
    if isinstance(value, list):
        return [str(item).strip() for item in value if str(item).strip()]
    return [item.strip() for item in str(value or "").replace(";", ",").split(",") if item.strip()]


with open(path, "r", encoding="utf-8") as fh:
    root = json.load(fh)

monitor = root.setdefault("systemMonitor", {})
direct = monitor.setdefault("directMaintenance", {})
listen_hosts = split_list(listen_raw) or split_list(direct.get("listenHosts")) or [str(direct.get("listenHost") or "192.168.1.250")]
allowed_cidrs = split_list(cidrs_raw)
direct["enabled"] = str(enabled_raw or "").strip().lower() in ("1", "y", "yes", "true", "on")
direct["listenHost"] = listen_hosts[0]
direct["listenHosts"] = listen_hosts
direct["listenPort"] = int(direct.get("listenPort") or 9443)
direct["allowedClientCidrs"] = allowed_cidrs
direct["identityConfigFile"] = os.path.join(gateway_home, "config/runtime/device_identity.json")
direct["appConfigFile"] = os.path.join(gateway_home, "config/runtime/apps/monitor-service.json")
direct["otaAppConfigFile"] = os.path.join(gateway_home, "config/runtime/apps/mqtt-service.json")
direct["authStateFile"] = os.path.join(gateway_home, "config/runtime/monitor-direct-maintenance-state.json")
direct["otaStatusFile"] = os.path.join(gateway_home, "ota/monitor-direct-maintenance-status.jsonl")
direct["maxRealtimePoints"] = int(direct.get("maxRealtimePoints") or 2000)

tmp = path + ".tmp"
with open(tmp, "w", encoding="utf-8") as fh:
    json.dump(root, fh, ensure_ascii=False, indent=2)
    fh.write("\n")
os.replace(tmp, path)
PY
}

mkdir -p "$GATEWAY_HOME/bin" "$GATEWAY_HOME/config" "$GATEWAY_HOME/data" "$GATEWAY_HOME/ota" "$BACKUP_DIR"

if command -v systemctl >/dev/null 2>&1; then
  systemctl stop gateway-services.service 2>/dev/null || true
fi
if [ -x "$GATEWAY_HOME/bin/gateway-services.sh" ]; then
  "$GATEWAY_HOME/bin/gateway-services.sh" stop 2>/dev/null || true
fi

BASE_BINS="SystemMonitor MqttDriver pointctl"
ALL_BINS="ModbusRtu Dlt645Driver DioDriver CanDriver IecDriver MqttDriver EventEngine ComputeEngine EmsParityCheck EmsClusterCoordinator SystemMonitor pointctl"
OPTIONAL_BINS="LocalDisplay QtDisplayBridge KY-EMS CameraService stress_runner"
EXISTING_RUNTIME_MODE=$(json_string_value "$GATEWAY_HOME/config/runtime/apps/mqtt-service.json" "runtimeMode" || true)
DEFAULT_INSTALL_RUNTIME_MODE=$(first_nonempty "${INIT_RUNTIME_MODE:-}" "$EXISTING_RUNTIME_MODE" "gateway")
INSTALL_RUNTIME_MODE=$(normalize_runtime_mode "$(prompt_value "runtimeMode" "$DEFAULT_INSTALL_RUNTIME_MODE")")
if [ "$INSTALL_RUNTIME_MODE" = "agc_avc" ]; then
  ALL_BINS="$ALL_BINS AgcAvcController"
fi
if [ "$PACKAGE_PROFILE" = "base" ]; then
  if [ "$INSTALL_RUNTIME_MODE" = "ems" ]; then
    echo "base profile cannot initialize EMS mode; use a project or full package containing ComputeEngine, EmsParityCheck and EmsClusterCoordinator" >&2
    exit 2
  fi
  REQUIRED_BINS="$BASE_BINS"
  OPTIONAL_BINS=""
elif [ "$PACKAGE_PROFILE" = "project" ]; then
  if [ -z "$EDGE_PACKAGE_MANIFEST" ] || [ ! -f "$EDGE_PACKAGE_MANIFEST" ]; then
    echo "project profile requires edge-package-manifest.json" >&2
    exit 2
  fi
  REQUIRED_BINS=$(printf '%s\n' $BASE_BINS $(manifest_binaries "$EDGE_PACKAGE_MANIFEST") | unique_words | tr '\n' ' ')
  if [ "$INSTALL_RUNTIME_MODE" = "ems" ]; then
    REQUIRED_BINS=$(printf '%s\n' $REQUIRED_BINS ComputeEngine EmsParityCheck EmsClusterCoordinator | unique_words | tr '\n' ' ')
  fi
  OPTIONAL_BINS=""
else
  REQUIRED_BINS="$ALL_BINS"
fi
if [ "$INSTALL_RUNTIME_MODE" = "agc_avc" ]; then
  REQUIRED_BINS=$(printf '%s\n' $REQUIRED_BINS AgcAvcController | unique_words | tr '\n' ' ')
fi
case " $REQUIRED_BINS " in
  *" KY-EMS "*)
    REQUIRED_BINS=$(printf '%s\n' $REQUIRED_BINS QtDisplayBridge | unique_words | tr '\n' ' ')
    ;;
esac

echo "install package profile: $PACKAGE_PROFILE"
if [ -n "$EDGE_PACKAGE_MANIFEST" ] && [ -f "$EDGE_PACKAGE_MANIFEST" ]; then
  echo "install package manifest: $EDGE_PACKAGE_MANIFEST"
fi

for bin in $REQUIRED_BINS; do
  if [ "$bin" = "KY-EMS" ]; then
    install_ky_ems_payload 1
  else
    install_required_binary "$bin"
  fi
done
for bin in $OPTIONAL_BINS; do
  if [ "$bin" = "KY-EMS" ]; then
    install_ky_ems_payload 0
  else
    install_optional_binary "$bin"
  fi
done

install_required_deploy_file "gateway-services.sh" "$GATEWAY_HOME/bin/gateway-services.sh"
install_required_deploy_file "gateway-run.sh" "$GATEWAY_HOME/bin/gateway-run.sh"
install_required_deploy_file "gateway-tls-enroll.sh" "$GATEWAY_HOME/bin/gateway-tls-enroll.sh"
install_required_deploy_file "install-factory-config.sh" "$GATEWAY_HOME/bin/install-factory-config.sh"
install_required_deploy_file "production-smoke-test.sh" "$GATEWAY_HOME/bin/production-smoke-test.sh"
install_required_deploy_file "ota-apply.sh" "$GATEWAY_HOME/bin/ota-apply.sh"
install_required_deploy_file "ota-rollback.sh" "$GATEWAY_HOME/bin/ota-rollback.sh"
install_required_deploy_file "install-scada-project.sh" "$GATEWAY_HOME/bin/install-scada-project.sh"
install_required_deploy_file "gateway-ky-ems-readiness.sh" "$GATEWAY_HOME/bin/gateway-ky-ems-readiness.sh"
install_deploy_file_if_exists "gateway-network-failover.sh" "$GATEWAY_HOME/bin/gateway-network-failover.sh"
install_deploy_file_if_exists "gateway-cellular.sh" "$GATEWAY_HOME/bin/gateway-cellular.sh"
if [ ! -f "$SYSTEM_DEFAULT_DIR/gateway-network-failover" ]; then
  install_deploy_file_with_mode_if_exists "gateway-network-failover.default" "$SYSTEM_DEFAULT_DIR/gateway-network-failover" 0644
fi
install_deploy_file_if_exists "local-kiosk.py" "$GATEWAY_HOME/bin/local-kiosk.py"
chmod +x "$GATEWAY_HOME/bin/"*.sh 2>/dev/null || true
chmod +x "$GATEWAY_HOME/bin/"* 2>/dev/null || true

EXISTING_MACHINE_CODE=$(json_string_value "$GATEWAY_HOME/config/runtime/device_identity.json" "machineCode" || true)
EXISTING_MQTT_FILE="$GATEWAY_HOME/config/runtime/apps/mqtt-service.json"
EXISTING_MQTT_BROKER=$(json_string_value "$EXISTING_MQTT_FILE" "broker" || true)
EXISTING_MQTT_USERNAME=$(json_string_value "$EXISTING_MQTT_FILE" "username" || true)
EXISTING_MQTT_PASSWORD=$(json_string_value "$EXISTING_MQTT_FILE" "password" || true)
EXISTING_MQTT_TLS_ENABLED=$(json_tls_bool_value "$EXISTING_MQTT_FILE" "enabled" || true)
EXISTING_MQTT_CA_FILE=$(json_string_value "$EXISTING_MQTT_FILE" "caFile" || true)
EXISTING_MQTT_CERT_FILE=$(json_string_value "$EXISTING_MQTT_FILE" "certFile" || true)
EXISTING_MQTT_KEY_FILE=$(json_string_value "$EXISTING_MQTT_FILE" "keyFile" || true)
EXISTING_MQTT_TLS_INSECURE=$(json_tls_bool_value "$EXISTING_MQTT_FILE" "insecureSkipVerify" || true)

ts=$(date +%Y%m%d%H%M%S)
if [ -d "$GATEWAY_HOME/config/runtime" ]; then
  mv "$GATEWAY_HOME/config/runtime" "$BACKUP_DIR/runtime-$ts"
  echo "backup old runtime config: $BACKUP_DIR/runtime-$ts"
fi
mkdir -p "$GATEWAY_HOME/config"
cp -a "$FACTORY_DIR/runtime" "$GATEWAY_HOME/config/runtime"
if [ -n "$EDGE_PACKAGE_MANIFEST" ] && [ -f "$EDGE_PACKAGE_MANIFEST" ]; then
  install_file_if_exists "$EDGE_PACKAGE_MANIFEST" "$GATEWAY_HOME/config/runtime/edge-package-manifest.json"
fi

FACTORY_MACHINE_CODE=$(json_string_value "$GATEWAY_HOME/config/runtime/device_identity.json" "machineCode" || true)
FACTORY_MQTT_FILE="$GATEWAY_HOME/config/runtime/apps/mqtt-service.json"
FACTORY_MQTT_BROKER=$(json_string_value "$FACTORY_MQTT_FILE" "broker" || true)
FACTORY_MQTT_USERNAME=$(json_string_value "$FACTORY_MQTT_FILE" "username" || true)
FACTORY_MQTT_PASSWORD=$(json_string_value "$FACTORY_MQTT_FILE" "password" || true)
FACTORY_MQTT_TLS_ENABLED=$(json_tls_bool_value "$FACTORY_MQTT_FILE" "enabled" || true)
FACTORY_MQTT_CA_FILE=$(json_string_value "$FACTORY_MQTT_FILE" "caFile" || true)
FACTORY_MQTT_CERT_FILE=$(json_string_value "$FACTORY_MQTT_FILE" "certFile" || true)
FACTORY_MQTT_KEY_FILE=$(json_string_value "$FACTORY_MQTT_FILE" "keyFile" || true)
FACTORY_MQTT_TLS_INSECURE=$(json_tls_bool_value "$FACTORY_MQTT_FILE" "insecureSkipVerify" || true)

DEFAULT_MACHINE_CODE=$(first_nonempty "${INIT_MACHINE_CODE:-}" "$EXISTING_MACHINE_CODE" "$SCADA_PROJECT_MACHINE_CODE" "$FACTORY_MACHINE_CODE" "GW_FACTORY_001")
DEFAULT_MQTT_BROKER=$(first_nonempty "${INIT_MQTT_BROKER:-}" "$EXISTING_MQTT_BROKER" "$FACTORY_MQTT_BROKER" "tcp://127.0.0.1:1883")
DEFAULT_MQTT_USERNAME=$(first_nonempty "${INIT_MQTT_USERNAME:-}" "$EXISTING_MQTT_USERNAME" "$FACTORY_MQTT_USERNAME")
DEFAULT_MQTT_PASSWORD=$(first_nonempty "${INIT_MQTT_PASSWORD:-}" "$EXISTING_MQTT_PASSWORD" "$FACTORY_MQTT_PASSWORD")
DEFAULT_MQTT_CA_FILE=$(first_nonempty "${INIT_MQTT_CA_FILE:-}" "$EXISTING_MQTT_CA_FILE" "$FACTORY_MQTT_CA_FILE")
DEFAULT_MQTT_CERT_FILE=$(first_nonempty "${INIT_MQTT_CERT_FILE:-}" "$EXISTING_MQTT_CERT_FILE" "$FACTORY_MQTT_CERT_FILE")
DEFAULT_MQTT_KEY_FILE=$(first_nonempty "${INIT_MQTT_KEY_FILE:-}" "$EXISTING_MQTT_KEY_FILE" "$FACTORY_MQTT_KEY_FILE")
DEFAULT_MQTT_TLS_ENABLED=$(first_nonempty "${INIT_MQTT_TLS_ENABLED:-}" "$EXISTING_MQTT_TLS_ENABLED" "$FACTORY_MQTT_TLS_ENABLED" "$(broker_implies_tls "$DEFAULT_MQTT_BROKER")")
DEFAULT_MQTT_TLS_INSECURE=$(first_nonempty "${INIT_MQTT_INSECURE_SKIP_VERIFY:-}" "$EXISTING_MQTT_TLS_INSECURE" "$FACTORY_MQTT_TLS_INSECURE" "false")
DEFAULT_DIRECT_MAINTENANCE_ENABLED=$(normalize_bool "${INIT_DIRECT_MAINTENANCE_ENABLED:-1}" "true")
DEFAULT_DIRECT_LISTEN_HOSTS=$(first_nonempty "${INIT_DIRECT_LISTEN_HOSTS:-}" "${INIT_DIRECT_LISTEN_HOST:-}" "192.168.1.250")
DEFAULT_DIRECT_ALLOWED_CIDRS=$(first_nonempty "${INIT_DIRECT_ALLOWED_CIDRS:-}" "${INIT_DIRECT_ALLOWED_CLIENT_CIDRS:-}")
DEFAULT_RUNTIME_MODE="$INSTALL_RUNTIME_MODE"

INIT_RUNTIME_MODE_VALUE="$DEFAULT_RUNTIME_MODE"
INIT_MACHINE_CODE_VALUE=$(prompt_value "machineCode" "$DEFAULT_MACHINE_CODE")
INIT_MQTT_BROKER_VALUE=$(prompt_value "MQTT broker" "$DEFAULT_MQTT_BROKER")
INIT_MQTT_USERNAME_VALUE=$(prompt_value "MQTT username" "$DEFAULT_MQTT_USERNAME")
INIT_MQTT_PASSWORD_VALUE=$(prompt_value "MQTT password" "$DEFAULT_MQTT_PASSWORD" 1)
if [ -z "${INIT_MQTT_TLS_ENABLED:-}" ]; then
  if [ "$(broker_implies_tls "$INIT_MQTT_BROKER_VALUE")" = "true" ]; then
    DEFAULT_MQTT_TLS_ENABLED="true"
  else
    DEFAULT_MQTT_TLS_ENABLED=$(first_nonempty "$EXISTING_MQTT_TLS_ENABLED" "$FACTORY_MQTT_TLS_ENABLED" "false")
  fi
fi
INIT_MQTT_TLS_ENABLED_VALUE=$(prompt_bool "Enable MQTT TLS" "$DEFAULT_MQTT_TLS_ENABLED")
INIT_MQTT_CA_FILE_VALUE=$(prompt_value "MQTT TLS caFile" "$DEFAULT_MQTT_CA_FILE")
INIT_MQTT_CERT_FILE_VALUE=$(prompt_value "MQTT TLS certFile" "$DEFAULT_MQTT_CERT_FILE")
INIT_MQTT_KEY_FILE_VALUE=$(prompt_value "MQTT TLS keyFile" "$DEFAULT_MQTT_KEY_FILE")
INIT_MQTT_INSECURE_SKIP_VERIFY_VALUE=$(prompt_bool "MQTT TLS insecureSkipVerify" "$DEFAULT_MQTT_TLS_INSECURE")
INIT_DIRECT_MAINTENANCE_ENABLED_VALUE=$(normalize_bool "$DEFAULT_DIRECT_MAINTENANCE_ENABLED" "true")
INIT_DIRECT_LISTEN_HOSTS_VALUE="$DEFAULT_DIRECT_LISTEN_HOSTS"
INIT_DIRECT_ALLOWED_CIDRS_VALUE="$DEFAULT_DIRECT_ALLOWED_CIDRS"

apply_runtime_identity_and_mqtt \
  "$INIT_MACHINE_CODE_VALUE" \
  "$INIT_MQTT_BROKER_VALUE" \
  "$INIT_MQTT_USERNAME_VALUE" \
  "$INIT_MQTT_PASSWORD_VALUE" \
  "$INIT_MQTT_TLS_ENABLED_VALUE" \
  "$INIT_MQTT_CA_FILE_VALUE" \
  "$INIT_MQTT_CERT_FILE_VALUE" \
  "$INIT_MQTT_KEY_FILE_VALUE" \
  "$INIT_MQTT_INSECURE_SKIP_VERIFY_VALUE"
apply_runtime_mode "$INIT_RUNTIME_MODE_VALUE"
apply_direct_maintenance_config \
  "$INIT_DIRECT_MAINTENANCE_ENABLED_VALUE" \
  "$INIT_DIRECT_LISTEN_HOSTS_VALUE" \
  "$INIT_DIRECT_ALLOWED_CIDRS_VALUE"

if [ -n "$SCADA_PROJECT_PACKAGE" ]; then
  "$GATEWAY_HOME/bin/install-scada-project.sh" \
    --package "$SCADA_PROJECT_PACKAGE" \
    --package-sha256 "$SCADA_PROJECT_SHA256" \
    --machine-code "$INIT_MACHINE_CODE_VALUE" \
    --app-config "$GATEWAY_HOME/config/runtime/apps/monitor-service.json" \
    --scada-root "$GATEWAY_HOME/scada" \
    --require-local-scada \
    --no-restart \
    --state-file "$GATEWAY_HOME/scada/factory-install-state.txt"
fi

echo "initialized runtimeMode: $INIT_RUNTIME_MODE_VALUE"
echo "initialized machineCode: $INIT_MACHINE_CODE_VALUE"
echo "initialized mqtt broker: $INIT_MQTT_BROKER_VALUE"
echo "initialized mqtt username: $INIT_MQTT_USERNAME_VALUE"
echo "initialized mqtt tls: $INIT_MQTT_TLS_ENABLED_VALUE"
echo "initialized direct maintenance: enabled=$INIT_DIRECT_MAINTENANCE_ENABLED_VALUE listen=$INIT_DIRECT_LISTEN_HOSTS_VALUE allowed=$INIT_DIRECT_ALLOWED_CIDRS_VALUE"

if [ -n "$TEMPLATES_DIR" ] && [ -d "$TEMPLATES_DIR" ]; then
  if ! same_path "$TEMPLATES_DIR" "$GATEWAY_HOME/config/templates"; then
    mkdir -p "$GATEWAY_HOME/config/templates"
    cp -a "$TEMPLATES_DIR"/. "$GATEWAY_HOME/config/templates"/
  fi
fi

if [ -n "$EXAMPLES_DIR" ] && [ -d "$EXAMPLES_DIR" ]; then
  if ! same_path "$EXAMPLES_DIR" "$GATEWAY_HOME/config/examples"; then
    mkdir -p "$GATEWAY_HOME/config/examples"
    cp -a "$EXAMPLES_DIR"/. "$GATEWAY_HOME/config/examples"/
  fi
fi

if [ "$INSTALL_SYSTEMD" = "1" ] && command -v systemctl >/dev/null 2>&1; then
  SYSTEMD_MANAGER_REEXEC_REQUIRED=0
  mkdir -p "$SYSTEMD_UNIT_DIR"
  install_required_deploy_file_with_mode "gateway-services.service" "$SYSTEMD_UNIT_DIR/gateway-services.service" 0644
  install_deploy_file_with_mode_if_exists "modbus-rtu@.service" "$SYSTEMD_UNIT_DIR/modbus-rtu@.service" 0644
  install_deploy_file_with_mode_if_exists "dlt645-driver@.service" "$SYSTEMD_UNIT_DIR/dlt645-driver@.service" 0644
  install_deploy_file_with_mode_if_exists "dio-driver@.service" "$SYSTEMD_UNIT_DIR/dio-driver@.service" 0644
  install_deploy_file_with_mode_if_exists "can-driver@.service" "$SYSTEMD_UNIT_DIR/can-driver@.service" 0644
  install_deploy_file_with_mode_if_exists "iec-driver@.service" "$SYSTEMD_UNIT_DIR/iec-driver@.service" 0644
  install_deploy_file_with_mode_if_exists "mqtt-driver@.service" "$SYSTEMD_UNIT_DIR/mqtt-driver@.service" 0644
  install_deploy_file_with_mode_if_exists "event-engine@.service" "$SYSTEMD_UNIT_DIR/event-engine@.service" 0644
  install_deploy_file_with_mode_if_exists "compute-engine@.service" "$SYSTEMD_UNIT_DIR/compute-engine@.service" 0644
  install_deploy_file_with_mode_if_exists "ems-cluster@.service" "$SYSTEMD_UNIT_DIR/ems-cluster@.service" 0644
  if [ "$INIT_RUNTIME_MODE_VALUE" = "agc_avc" ]; then
    install_deploy_file_with_mode_if_exists "agc-avc@.service" "$SYSTEMD_UNIT_DIR/agc-avc@.service" 0644
  fi
  install_deploy_file_with_mode_if_exists "local-display@.service" "$SYSTEMD_UNIT_DIR/local-display@.service" 0644
  install_deploy_file_with_mode_if_exists "local-kiosk@.service" "$SYSTEMD_UNIT_DIR/local-kiosk@.service" 0644
  install_deploy_file_with_mode_if_exists "qt-display-bridge.service" "$SYSTEMD_UNIT_DIR/qt-display-bridge.service" 0644
  install_deploy_file_with_mode_if_exists "ky-ems.service" "$SYSTEMD_UNIT_DIR/ky-ems.service" 0644
  install_deploy_file_with_mode_if_exists "camera-service@.service" "$SYSTEMD_UNIT_DIR/camera-service@.service" 0644
  install_deploy_file_with_mode_if_exists "system-monitor@.service" "$SYSTEMD_UNIT_DIR/system-monitor@.service" 0644
  install_deploy_file_with_mode_if_exists "mqtt-tls-tunnel@.service" "$SYSTEMD_UNIT_DIR/mqtt-tls-tunnel@.service" 0644
  install_deploy_file_with_mode_if_exists "gateway-network-failover.service" "$SYSTEMD_UNIT_DIR/gateway-network-failover.service" 0644
  install_deploy_file_with_mode_if_exists "gateway-cellular.service" "$SYSTEMD_UNIT_DIR/gateway-cellular.service" 0644
  if [ -e /dev/watchdog ] || [ -e /dev/watchdog0 ]; then
    mkdir -p "$SYSTEMD_SYSTEM_CONF_DIR"
    install_deploy_file_with_mode_if_exists "10-gateway-watchdog.conf" "$SYSTEMD_SYSTEM_CONF_DIR/10-gateway-watchdog.conf" 0644
    SYSTEMD_MANAGER_REEXEC_REQUIRED=1
  fi
  systemctl daemon-reload
  if [ "$SYSTEMD_MANAGER_REEXEC_REQUIRED" = "1" ]; then
    systemctl daemon-reexec
  fi
  systemctl enable gateway-services.service >/dev/null 2>&1 || true
fi

if [ "$RESET_SHM" = "1" ]; then
  if command -v systemctl >/dev/null 2>&1; then
    if [ -x "$GATEWAY_HOME/bin/gateway-services.sh" ]; then
      "$GATEWAY_HOME/bin/gateway-services.sh" stop || true
    else
      systemctl stop gateway-services.service 2>/dev/null || true
    fi
  fi
  rm -f /dev/shm/gateway_point_store* 2>/dev/null || true
fi

if [ "$START_SERVICES" = "1" ] && command -v systemctl >/dev/null 2>&1; then
  systemctl restart gateway-services.service
fi

if [ -n "$FACTORY_PACKAGE" ]; then
  echo "factory package: $FACTORY_PACKAGE"
fi
if [ -n "$SCADA_PROJECT_PACKAGE" ]; then
  echo "factory SCADA project SHA256: $SCADA_PROJECT_SHA256"
fi
if [ -n "$SCADA_PROJECT_CANONICAL_SHA256" ]; then
  echo "external SCADA restore required before service start: canonical=$SCADA_PROJECT_CANONICAL_SHA256 content=$SCADA_PROJECT_CONTENT_SHA256"
fi
echo "factory config installed"
