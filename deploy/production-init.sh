#!/bin/sh
set -eu

GATEWAY_HOME="${GATEWAY_HOME:-/opt/modbus-gateway}"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -z "${INIT_PROMPT:-}" ] && [ -n "${prompt:-}" ]; then
  INIT_PROMPT="$prompt"
fi
INIT_PACKAGE="${INIT_PACKAGE:-${FACTORY_PACKAGE:-$SCRIPT_DIR/gateway-factory-defaults.tar.gz}}"
INIT_RUNTIME_PACKAGE="${INIT_RUNTIME_PACKAGE:-}"
if [ -n "${package:-}" ]; then
  INIT_PACKAGE="$package"
fi
INIT_WORK_DIR="${INIT_WORK_DIR:-}"
INIT_WORK_DIR_OWNED=0
INSTALL_SYSTEMD="${INSTALL_SYSTEMD:-1}"
SYSTEMD_UNIT_DIR="${SYSTEMD_UNIT_DIR:-/etc/systemd/system}"
INIT_START_SERVICES="${INIT_START_SERVICES:-1}"
INIT_RUN_SMOKE="${INIT_RUN_SMOKE:-1}"
INIT_RESET_SHM="${INIT_RESET_SHM:-0}"
INIT_KEEP_WORK_DIR="${INIT_KEEP_WORK_DIR:-0}"
INIT_MQTT_CONNECT_TEST="${INIT_MQTT_CONNECT_TEST:-0}"
INIT_PACKAGE_PROFILE="${INIT_PACKAGE_PROFILE:-}"
INIT_EDGE_PACKAGE_MANIFEST="${INIT_EDGE_PACKAGE_MANIFEST:-}"
INIT_DIRECT_MAINTENANCE_ENABLED="${INIT_DIRECT_MAINTENANCE_ENABLED:-1}"
INIT_DIRECT_LISTEN_HOSTS="${INIT_DIRECT_LISTEN_HOSTS:-}"
INIT_DIRECT_ALLOWED_CIDRS="${INIT_DIRECT_ALLOWED_CIDRS:-}"
INIT_LOCAL_QT_DISPLAY="${INIT_LOCAL_QT_DISPLAY:-auto}"

usage() {
  cat >&2 <<'EOF'
Usage: production-init.sh [options]

Manual mode:
  sh production-init.sh --manual --package /tmp/gateway-factory-defaults.tar.gz
  sh production-init.sh prompt=1 package=/tmp/gateway-factory-defaults.tar.gz

Options:
  --manual, --prompt              Prompt for machineCode, MQTT, TLS and startup settings
  --auto, --no-prompt             Do not prompt; use arguments/env/defaults
  --package FILE                  Factory package path
  --runtime-package FILE          AGC/AVC runtime overlay package; required for agc_avc mode
  --gateway-home DIR              Gateway install directory; defaults to /opt/modbus-gateway
  --runtime-mode MODE             Runtime mode: gateway, ems or agc_avc; defaults to gateway
  --package-profile PROFILE       Driver package profile: base, project or full
  --manifest FILE                 Edge package manifest for project profile
  --local-qt-display              enable generic read-only Qt SCADA (project/full)
  --no-local-qt-display           leave generic Qt disabled; default auto for project KY-EMS
  --machine-code CODE             Device machineCode
  --mqtt-broker URL               MQTT broker, e.g. tls://kygate.kyxn.net:8883
  --mqtt-username USER            MQTT username
  --mqtt-password PASSWORD        MQTT password
  --mqtt-tls, --no-mqtt-tls       Enable or disable MQTT TLS config (certificate files: config/runtime/tls/)
  --ota-pubkey FILE               OTA signature public key (PEM); default: ota/ota.pub from the package
  --start, --no-start             Start services after init; default start
  --smoke, --no-smoke             Run production smoke test; default run
  --reset-shm                     Clear gateway shared memory before start
  --mqtt-connect-test             Enable MQTT connect check in smoke test
  --direct-maintenance            Enable SystemMonitor embedded direct maintenance API; default
  --no-direct-maintenance         Disable SystemMonitor embedded direct maintenance API
  --direct-listen-host HOST       Maintenance API listen host; defaults to factory maintenance address
  --direct-listen-hosts HOSTS     Comma separated maintenance API listen hosts
  --direct-allowed-cidr CIDR      Allowed maintenance client CIDR; empty list prevents API startup
  --direct-allowed-cidrs CIDRS    Comma separated allowed maintenance client CIDRs; empty list prevents API startup
  -h, --help                      Show help

Environment variables with the same meaning are also supported:
  INIT_PROMPT INIT_PACKAGE INIT_RUNTIME_PACKAGE GATEWAY_HOME INIT_RUNTIME_MODE INIT_MACHINE_CODE
  INIT_PACKAGE_PROFILE INIT_EDGE_PACKAGE_MANIFEST
  INIT_MQTT_BROKER INIT_MQTT_USERNAME INIT_MQTT_PASSWORD
  INIT_MQTT_TLS_ENABLED
  INIT_DIRECT_MAINTENANCE_ENABLED INIT_DIRECT_LISTEN_HOSTS INIT_DIRECT_ALLOWED_CIDRS

Device certificates (MQTT client and maintenance channel) are not handled here: after init the engineering
client runs gateway-tls-enroll.sh csr / install on the device and relays the CSR to the platform.
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --manual|--prompt)
      INIT_PROMPT=1
      shift
      ;;
    --auto|--no-prompt)
      INIT_PROMPT=0
      shift
      ;;
    --package)
      [ "$#" -ge 2 ] || { echo "--package requires a value" >&2; exit 2; }
      INIT_PACKAGE="$2"
      shift 2
      ;;
    --runtime-package)
      [ "$#" -ge 2 ] || { echo "--runtime-package requires a value" >&2; exit 2; }
      INIT_RUNTIME_PACKAGE="$2"
      shift 2
      ;;
    --gateway-home)
      [ "$#" -ge 2 ] || { echo "--gateway-home requires a value" >&2; exit 2; }
      GATEWAY_HOME="$2"
      shift 2
      ;;
    --runtime-mode)
      [ "$#" -ge 2 ] || { echo "--runtime-mode requires a value" >&2; exit 2; }
      INIT_RUNTIME_MODE="$2"
      shift 2
      ;;
    --package-profile)
      [ "$#" -ge 2 ] || { echo "--package-profile requires a value" >&2; exit 2; }
      INIT_PACKAGE_PROFILE="$2"
      shift 2
      ;;
    --manifest)
      [ "$#" -ge 2 ] || { echo "--manifest requires a value" >&2; exit 2; }
      INIT_EDGE_PACKAGE_MANIFEST="$2"
      shift 2
      ;;
    --local-qt-display)
      INIT_LOCAL_QT_DISPLAY=1
      shift
      ;;
    --no-local-qt-display)
      INIT_LOCAL_QT_DISPLAY=0
      shift
      ;;
    --machine-code)
      [ "$#" -ge 2 ] || { echo "--machine-code requires a value" >&2; exit 2; }
      INIT_MACHINE_CODE="$2"
      shift 2
      ;;
    --mqtt-broker)
      [ "$#" -ge 2 ] || { echo "--mqtt-broker requires a value" >&2; exit 2; }
      INIT_MQTT_BROKER="$2"
      shift 2
      ;;
    --mqtt-username)
      [ "$#" -ge 2 ] || { echo "--mqtt-username requires a value" >&2; exit 2; }
      INIT_MQTT_USERNAME="$2"
      shift 2
      ;;
    --mqtt-password)
      [ "$#" -ge 2 ] || { echo "--mqtt-password requires a value" >&2; exit 2; }
      INIT_MQTT_PASSWORD="$2"
      shift 2
      ;;
    --mqtt-tls)
      INIT_MQTT_TLS_ENABLED=true
      shift
      ;;
    --no-mqtt-tls)
      INIT_MQTT_TLS_ENABLED=false
      shift
      ;;
    --ota-pubkey)
      [ "$#" -ge 2 ] || { echo "--ota-pubkey requires a value" >&2; exit 2; }
      OTA_SIGNING_PUBKEY="$2"
      export OTA_SIGNING_PUBKEY
      shift 2
      ;;
    --start)
      INIT_START_SERVICES=1
      shift
      ;;
    --no-start)
      INIT_START_SERVICES=0
      shift
      ;;
    --smoke)
      INIT_RUN_SMOKE=1
      shift
      ;;
    --no-smoke)
      INIT_RUN_SMOKE=0
      shift
      ;;
    --reset-shm)
      INIT_RESET_SHM=1
      shift
      ;;
    --mqtt-connect-test)
      INIT_MQTT_CONNECT_TEST=1
      shift
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
    prompt=*|INIT_PROMPT=*)
      INIT_PROMPT="${1#*=}"
      shift
      ;;
    package=*|INIT_PACKAGE=*)
      INIT_PACKAGE="${1#*=}"
      shift
      ;;
    runtime_package=*|INIT_RUNTIME_PACKAGE=*)
      INIT_RUNTIME_PACKAGE="${1#*=}"
      shift
      ;;
    gateway_home=*|GATEWAY_HOME=*)
      GATEWAY_HOME="${1#*=}"
      shift
      ;;
    runtime_mode=*|runtimeMode=*|INIT_RUNTIME_MODE=*)
      INIT_RUNTIME_MODE="${1#*=}"
      shift
      ;;
    package_profile=*|PACKAGE_PROFILE=*|INIT_PACKAGE_PROFILE=*)
      INIT_PACKAGE_PROFILE="${1#*=}"
      shift
      ;;
    manifest=*|EDGE_PACKAGE_MANIFEST=*|INIT_EDGE_PACKAGE_MANIFEST=*)
      INIT_EDGE_PACKAGE_MANIFEST="${1#*=}"
      shift
      ;;
    machine_code=*|machineCode=*|INIT_MACHINE_CODE=*)
      INIT_MACHINE_CODE="${1#*=}"
      shift
      ;;
    mqtt_broker=*|INIT_MQTT_BROKER=*)
      INIT_MQTT_BROKER="${1#*=}"
      shift
      ;;
    mqtt_username=*|INIT_MQTT_USERNAME=*)
      INIT_MQTT_USERNAME="${1#*=}"
      shift
      ;;
    mqtt_password=*|INIT_MQTT_PASSWORD=*)
      INIT_MQTT_PASSWORD="${1#*=}"
      shift
      ;;
    direct_maintenance_enabled=*|directMaintenance=*|INIT_DIRECT_MAINTENANCE_ENABLED=*)
      INIT_DIRECT_MAINTENANCE_ENABLED="${1#*=}"
      shift
      ;;
    direct_listen_host=*|directListenHost=*|direct_listen_hosts=*|directListenHosts=*|INIT_DIRECT_LISTEN_HOSTS=*)
      INIT_DIRECT_LISTEN_HOSTS="${1#*=}"
      shift
      ;;
    direct_allowed_cidr=*|directAllowedCidr=*|direct_allowed_cidrs=*|directAllowedCidrs=*|INIT_DIRECT_ALLOWED_CIDRS=*)
      INIT_DIRECT_ALLOWED_CIDRS="${1#*=}"
      shift
      ;;
    *)
      echo "unknown option: $1" >&2
      usage
      exit 2
      ;;
  esac
done

export GATEWAY_HOME
INIT_WORK_DIR="${INIT_WORK_DIR:-${TMPDIR:-$GATEWAY_HOME/tmp}/gateway-production-init.$$}"

truthy() {
  case "$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')" in
    1|y|yes|true|on) return 0 ;;
    *) return 1 ;;
  esac
}

if [ "$INSTALL_SYSTEMD" = "0" ]; then
  if truthy "$INIT_START_SERVICES"; then echo "INSTALL_SYSTEMD=0 requires --no-start" >&2; exit 2; fi
  if truthy "$INIT_RESET_SHM"; then echo "INSTALL_SYSTEMD=0 forbids --reset-shm" >&2; exit 2; fi
  if truthy "$INIT_RUN_SMOKE"; then echo "INSTALL_SYSTEMD=0 requires --no-smoke" >&2; exit 2; fi
fi

if [ -z "${INIT_PROMPT:-}" ]; then
  if [ -t 0 ]; then
    INIT_PROMPT=1
  else
    INIT_PROMPT=0
  fi
fi
export INIT_PROMPT

cleanup() {
  if [ "$INIT_WORK_DIR_OWNED" = "1" ] && [ "$INIT_KEEP_WORK_DIR" != "1" ] && [ -d "$INIT_WORK_DIR" ]; then
    rm -rf "$INIT_WORK_DIR"
  fi
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

normalize_bool() {
  value=$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')
  case "$value" in
    1|y|yes|true|on) printf 'true\n' ;;
    0|n|no|false|off|"") printf 'false\n' ;;
    *) printf '%s\n' "$2" ;;
  esac
}

is_interactive_init() {
  truthy "$INIT_PROMPT"
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

prompt_bool() {
  label="$1"
  default_value=$(normalize_bool "$2" "false")
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

first_nonempty() {
  for value in "$@"; do
    if [ -n "$value" ]; then
      printf '%s\n' "$value"
      return 0
    fi
  done
  return 0
}

broker_implies_tls() {
  case "$1" in
    ssl://*|tls://*|mqtts://*) return 0 ;;
    *) return 1 ;;
  esac
}

broker_implies_tls_value() {
  if broker_implies_tls "$1"; then
    printf 'true\n'
  else
    printf 'false\n'
  fi
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

json_string_value() {
  file="$1"
  key="$2"
  if [ -f "$file" ]; then
    sed -n "s/.*\"$key\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$file" | sed -n '1p'
  fi
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
    if [ "$INSTALL_SYSTEMD" = "1" ]; then
      rm -f "$SYSTEMD_UNIT_DIR/agc-avc@.service"
    fi
  fi
  if [ "$runtime_mode" != "ems" ]; then
    rm -f "$GATEWAY_HOME/bin/EmsClusterCoordinator"
    if [ "$INSTALL_SYSTEMD" = "1" ]; then
      rm -f "$SYSTEMD_UNIT_DIR/ems-cluster@.service"
    fi
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

find_package_root() {
  for dir in \
    "$INIT_WORK_DIR/gateway-factory-defaults" \
    "$INIT_WORK_DIR/gateway-factory" \
    "$INIT_WORK_DIR"; do
    if [ -d "$dir/deploy" ] && { [ -d "$dir/config/factory/runtime" ] || [ -d "$dir/build-aarch64" ]; }; then
      printf '%s\n' "$dir"
      return 0
    fi
  done
  return 1
}

require_file() {
  if [ ! -f "$1" ]; then
    echo "$2 not found: $1" >&2
    exit 1
  fi
}

if ! command -v tar >/dev/null 2>&1; then
  echo "tar command not found" >&2
  exit 1
fi

require_file "$INIT_PACKAGE" "init package"
if [ -n "$INIT_RUNTIME_PACKAGE" ]; then
  require_file "$INIT_RUNTIME_PACKAGE" "runtime package"
fi

# Never reuse or remove a work directory owned by another invocation.
mkdir -p "$(dirname "$INIT_WORK_DIR")"
mkdir "$INIT_WORK_DIR" || { echo "init work directory must not already exist: $INIT_WORK_DIR" >&2; exit 1; }
INIT_WORK_DIR_OWNED=1
tar -xzf "$INIT_PACKAGE" -C "$INIT_WORK_DIR"
if [ -n "$INIT_RUNTIME_PACKAGE" ]; then
  tar -xzf "$INIT_RUNTIME_PACKAGE" -C "$INIT_WORK_DIR"
fi
PACKAGE_ROOT=$(find_package_root) || {
  echo "factory package root not found after extract: $INIT_PACKAGE" >&2
  exit 1
}

EXISTING_MACHINE_CODE=$(json_string_value "$GATEWAY_HOME/config/runtime/device_identity.json" "machineCode" || true)
EXISTING_MQTT_FILE="$GATEWAY_HOME/config/runtime/apps/mqtt-service.json"
EXISTING_MQTT_BROKER=$(json_string_value "$EXISTING_MQTT_FILE" "broker" || true)
EXISTING_MQTT_USERNAME=$(json_string_value "$EXISTING_MQTT_FILE" "username" || true)
EXISTING_MQTT_PASSWORD=$(json_string_value "$EXISTING_MQTT_FILE" "password" || true)
EXISTING_MQTT_TLS_ENABLED=$(json_tls_bool_value "$EXISTING_MQTT_FILE" "enabled" || true)
EXISTING_MQTT_TLS_INSECURE=$(json_tls_bool_value "$EXISTING_MQTT_FILE" "insecureSkipVerify" || true)

FACTORY_MACHINE_CODE=$(json_string_value "$PACKAGE_ROOT/config/factory/runtime/device_identity.json" "machineCode" || true)
FACTORY_MQTT_FILE="$PACKAGE_ROOT/config/factory/runtime/apps/mqtt-service.json"
FACTORY_MQTT_BROKER=$(json_string_value "$FACTORY_MQTT_FILE" "broker" || true)
FACTORY_MQTT_USERNAME=$(json_string_value "$FACTORY_MQTT_FILE" "username" || true)
FACTORY_MQTT_PASSWORD=$(json_string_value "$FACTORY_MQTT_FILE" "password" || true)
FACTORY_MQTT_TLS_ENABLED=$(json_tls_bool_value "$FACTORY_MQTT_FILE" "enabled" || true)
FACTORY_MQTT_TLS_INSECURE=$(json_tls_bool_value "$FACTORY_MQTT_FILE" "insecureSkipVerify" || true)

DEFAULT_RUNTIME_MODE=$(normalize_runtime_mode "${INIT_RUNTIME_MODE:-gateway}")
DEFAULT_MACHINE_CODE=$(first_nonempty "${INIT_MACHINE_CODE:-}" "$EXISTING_MACHINE_CODE" "$FACTORY_MACHINE_CODE" "GW_FACTORY_001")
DEFAULT_MQTT_BROKER=$(first_nonempty "${INIT_MQTT_BROKER:-}" "$EXISTING_MQTT_BROKER" "$FACTORY_MQTT_BROKER" "tcp://127.0.0.1:1883")
DEFAULT_MQTT_USERNAME=$(first_nonempty "${INIT_MQTT_USERNAME:-}" "$EXISTING_MQTT_USERNAME" "$FACTORY_MQTT_USERNAME")
DEFAULT_MQTT_PASSWORD=$(first_nonempty "${INIT_MQTT_PASSWORD:-}" "$EXISTING_MQTT_PASSWORD" "$FACTORY_MQTT_PASSWORD")
DEFAULT_MQTT_TLS_ENABLED=$(first_nonempty "${INIT_MQTT_TLS_ENABLED:-}" "$EXISTING_MQTT_TLS_ENABLED" "$FACTORY_MQTT_TLS_ENABLED" "$(broker_implies_tls_value "$DEFAULT_MQTT_BROKER")")
DEFAULT_MQTT_TLS_INSECURE=$(first_nonempty "${INIT_MQTT_INSECURE_SKIP_VERIFY:-}" "$EXISTING_MQTT_TLS_INSECURE" "$FACTORY_MQTT_TLS_INSECURE" "false")

if is_interactive_init; then
  echo "Gateway production initialization"
  INIT_RUNTIME_MODE=$(normalize_runtime_mode "$(prompt_value "runtimeMode" "$DEFAULT_RUNTIME_MODE")")
  INIT_PACKAGE_PROFILE=$(prompt_value "packageProfile(base/project/full)" "${INIT_PACKAGE_PROFILE:-project}")
  INIT_MACHINE_CODE=$(prompt_value "machineCode" "$DEFAULT_MACHINE_CODE")
  INIT_MQTT_BROKER=$(prompt_value "MQTT broker" "$DEFAULT_MQTT_BROKER")
  INIT_MQTT_USERNAME=$(prompt_value "MQTT username" "$DEFAULT_MQTT_USERNAME")
  INIT_MQTT_PASSWORD=$(prompt_value "MQTT password" "$DEFAULT_MQTT_PASSWORD" 1)
  if [ -z "${INIT_MQTT_TLS_ENABLED:-}" ] && broker_implies_tls "$INIT_MQTT_BROKER"; then
    DEFAULT_MQTT_TLS_ENABLED="true"
  fi
  INIT_MQTT_TLS_ENABLED=$(prompt_bool "Enable MQTT TLS" "$DEFAULT_MQTT_TLS_ENABLED")
  INIT_MQTT_INSECURE_SKIP_VERIFY=$(prompt_bool "MQTT TLS insecureSkipVerify" "$DEFAULT_MQTT_TLS_INSECURE")
  if truthy "$INIT_MQTT_TLS_ENABLED"; then
    echo "MQTT TLS files: $GATEWAY_HOME/config/runtime/tls/{ca.crt,$INIT_MACHINE_CODE-client.pem,$INIT_MACHINE_CODE-client.key} (installed later by gateway-tls-enroll.sh)" >&2
  fi
  INIT_START_SERVICES=$(prompt_bool "Start gateway services after init" "$INIT_START_SERVICES")
  INIT_RUN_SMOKE=$(prompt_bool "Run production smoke test" "$INIT_RUN_SMOKE")
  INIT_RESET_SHM=$(prompt_bool "Reset shared memory before start" "$INIT_RESET_SHM")
  INIT_MQTT_CONNECT_TEST=$(prompt_bool "Run MQTT connect test in smoke test" "$INIT_MQTT_CONNECT_TEST")
fi

export INIT_RUNTIME_MODE="$(normalize_runtime_mode "${INIT_RUNTIME_MODE:-$DEFAULT_RUNTIME_MODE}")"
if [ "$INIT_RUNTIME_MODE" = "agc_avc" ] && [ -z "$INIT_RUNTIME_PACKAGE" ]; then
  echo "agc_avc runtime mode requires --runtime-package" >&2
  exit 2
fi
export INIT_MACHINE_CODE="${INIT_MACHINE_CODE:-$DEFAULT_MACHINE_CODE}"
# The machine code is the certificate CN and part of file names (same rule as gateway-tls-enroll.sh and the platform).
case "$INIT_MACHINE_CODE" in
  ""|*[!A-Za-z0-9._-]*) echo "invalid machineCode (allowed: A-Z a-z 0-9 . _ -): $INIT_MACHINE_CODE" >&2; exit 2 ;;
esac
export INIT_MQTT_BROKER="${INIT_MQTT_BROKER:-$DEFAULT_MQTT_BROKER}"
export INIT_MQTT_USERNAME="${INIT_MQTT_USERNAME:-$DEFAULT_MQTT_USERNAME}"
export INIT_MQTT_PASSWORD="${INIT_MQTT_PASSWORD:-$DEFAULT_MQTT_PASSWORD}"
export INIT_MQTT_TLS_ENABLED="${INIT_MQTT_TLS_ENABLED:-$DEFAULT_MQTT_TLS_ENABLED}"
export INIT_MQTT_INSECURE_SKIP_VERIFY="${INIT_MQTT_INSECURE_SKIP_VERIFY:-$DEFAULT_MQTT_TLS_INSECURE}"
export INIT_START_SERVICES INIT_RUN_SMOKE INIT_RESET_SHM INIT_MQTT_CONNECT_TEST
export INSTALL_SYSTEMD SYSTEMD_UNIT_DIR
export INIT_DIRECT_MAINTENANCE_ENABLED="$(normalize_bool "${INIT_DIRECT_MAINTENANCE_ENABLED:-1}" "true")"
export INIT_DIRECT_LISTEN_HOSTS="$(first_nonempty "${INIT_DIRECT_LISTEN_HOSTS:-}" "${INIT_DIRECT_LISTEN_HOST:-}" "192.168.1.250")"
export INIT_DIRECT_ALLOWED_CIDRS="$(first_nonempty "${INIT_DIRECT_ALLOWED_CIDRS:-}" "${INIT_DIRECT_ALLOWED_CLIENT_CIDRS:-}")"

tls_requested=0
if truthy "${INIT_MQTT_TLS_ENABLED:-false}" || { [ -n "${INIT_MQTT_BROKER:-}" ] && broker_implies_tls "$INIT_MQTT_BROKER"; }; then
  tls_requested=1
fi

# MQTT TLS always uses the files gateway-tls-enroll.sh installs (one convention, no per-site paths).
if [ "$tls_requested" -eq 1 ]; then
  export INIT_MQTT_TLS_ENABLED="${INIT_MQTT_TLS_ENABLED:-true}"
  export INIT_MQTT_INSECURE_SKIP_VERIFY="${INIT_MQTT_INSECURE_SKIP_VERIFY:-false}"
  export INIT_MQTT_CA_FILE="$GATEWAY_HOME/config/runtime/tls/ca.crt"
  export INIT_MQTT_CERT_FILE="$GATEWAY_HOME/config/runtime/tls/$INIT_MACHINE_CODE-client.pem"
  export INIT_MQTT_KEY_FILE="$GATEWAY_HOME/config/runtime/tls/$INIT_MACHINE_CODE-client.key"
fi

export SOURCE_ROOT="$PACKAGE_ROOT"
if [ -f "$SCRIPT_DIR/install-factory-config.sh" ]; then
  export DEPLOY_DIR="$SCRIPT_DIR"
else
  export DEPLOY_DIR="$PACKAGE_ROOT/deploy"
fi
require_file "$DEPLOY_DIR/install-factory-config.sh" "install script"
if [ -f "$PACKAGE_ROOT/generic-package.json" ]; then
  require_file "$DEPLOY_DIR/prepare-generic-runtime.py" "generic runtime preparation script"
  INIT_LOCAL_QT_DISPLAY_ENABLED=$(python3 "$DEPLOY_DIR/prepare-generic-runtime.py" preflight \
    --package-root "$PACKAGE_ROOT" --deploy-dir "$DEPLOY_DIR" \
    --profile "${INIT_PACKAGE_PROFILE:-full}" --mode "$INIT_RUNTIME_MODE" \
    --manifest "${INIT_EDGE_PACKAGE_MANIFEST:-}" --selection "$INIT_WORK_DIR/generic-selection.json" \
    --machine "$INIT_MACHINE_CODE" --qt "$INIT_LOCAL_QT_DISPLAY" \
    --home "$GATEWAY_HOME" --start "$INIT_START_SERVICES")
  export INIT_LOCAL_QT_DISPLAY_ENABLED
  if [ "${INIT_PACKAGE_PROFILE:-full}" = "project" ]; then
    INIT_EDGE_PACKAGE_MANIFEST="$INIT_WORK_DIR/generic-selection.json"
  fi
elif [ "$INIT_LOCAL_QT_DISPLAY" != "auto" ]; then
  echo "local Qt initialization requires a qualified generic-capable package" >&2
  exit 2
fi
export FACTORY_PROMPT=0
export FACTORY_PACKAGE=""
export FACTORY_DIR="$PACKAGE_ROOT/config/factory"
export TEMPLATES_DIR="$PACKAGE_ROOT/config/templates"
export EXAMPLES_DIR="$PACKAGE_ROOT/config/examples"
export PACKAGE_PROFILE="${INIT_PACKAGE_PROFILE:-}"
export EDGE_PACKAGE_MANIFEST="${INIT_EDGE_PACKAGE_MANIFEST:-}"
export START_SERVICES=0
export RESET_SHM="$INIT_RESET_SHM"
export INIT_TLS_APP_PREPARE=0
# Apps get their MQTT TLS fields at install time; gateway-tls-enroll.sh install later only checks them
# (--require-prepared-app) and writes the certificate files.
if [ "$tls_requested" -eq 1 ]; then
  export INIT_TLS_APP_PREPARE=1
  export INIT_TLS_FINAL_CA_FILE="$INIT_MQTT_CA_FILE"
  export INIT_TLS_FINAL_CERT_FILE="$INIT_MQTT_CERT_FILE"
  export INIT_TLS_FINAL_KEY_FILE="$INIT_MQTT_KEY_FILE"
fi
export INIT_GENERIC_PREPARE=0
export INIT_GENERIC_QT_ENVIRONMENT=""
if [ -f "$PACKAGE_ROOT/generic-package.json" ]; then
  export INIT_GENERIC_PREPARE=1
  export INIT_GENERIC_QT_ENVIRONMENT="$INIT_WORK_DIR/generic-qt-environment.json"
fi

set -- "$DEPLOY_DIR/install-factory-config.sh"
if [ -n "${INIT_PACKAGE_PROFILE:-}" ]; then
  set -- "$@" --profile "$INIT_PACKAGE_PROFILE"
fi
if [ -n "${INIT_EDGE_PACKAGE_MANIFEST:-}" ]; then
  set -- "$@" --manifest "$INIT_EDGE_PACKAGE_MANIFEST"
fi
sh "$@"
# Runtime JSON (including generic/Qt preparation) was installed from the
# validated private stage. Preserve the old non-EMS binary/unit cleanup only.
if [ "$INIT_RUNTIME_MODE" != "ems" ]; then
  rm -f "$GATEWAY_HOME/bin/EmsClusterCoordinator"
  if [ "$INSTALL_SYSTEMD" = "1" ]; then
    rm -f "$SYSTEMD_UNIT_DIR/ems-cluster@.service"
  fi
fi

# Without its certificate the device is not finished: MQTT would only retry and the smoke test would fail.
# Start and smoke run after gateway-tls-enroll.sh install (the engineering client's init wizard does this).
if [ "$tls_requested" -eq 1 ] && { [ ! -f "$INIT_MQTT_CERT_FILE" ] || [ ! -f "$INIT_MQTT_KEY_FILE" ]; }; then
  echo "MQTT TLS configured but the device certificate is not installed yet; services not started, smoke test not run."
  echo "next: gateway-tls-enroll.sh csr --purpose mqtt, relay the CSR to the platform, gateway-tls-enroll.sh install, then start services and run production-smoke-test.sh"
  INIT_START_SERVICES=0
  INIT_RUN_SMOKE=0
fi

if truthy "$INIT_START_SERVICES"; then
  if [ "$INSTALL_SYSTEMD" = "1" ] && command -v systemctl >/dev/null 2>&1; then
    systemctl restart gateway-services.service
    systemctl start gateway-health-watchdog.service
  elif [ "$INSTALL_SYSTEMD" = "1" ] && [ -x "$GATEWAY_HOME/bin/gateway-services.sh" ]; then
    "$GATEWAY_HOME/bin/gateway-services.sh" restart
  fi
fi

if truthy "$INIT_RUN_SMOKE"; then
  require_file "$GATEWAY_HOME/bin/production-smoke-test.sh" "smoke test script"
  MQTT_CONNECT_TEST="$INIT_MQTT_CONNECT_TEST" GATEWAY_HOME="$GATEWAY_HOME" sh "$GATEWAY_HOME/bin/production-smoke-test.sh"
fi

echo "production init completed"
