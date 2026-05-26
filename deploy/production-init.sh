#!/bin/sh
set -eu

GATEWAY_HOME="${GATEWAY_HOME:-/opt/modbus-gateway}"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
INIT_PACKAGE="${INIT_PACKAGE:-${FACTORY_PACKAGE:-$SCRIPT_DIR/gateway-factory-defaults.tar.gz}}"
INIT_WORK_DIR="${INIT_WORK_DIR:-/tmp/gateway-production-init.$$}"
INIT_START_SERVICES="${INIT_START_SERVICES:-1}"
INIT_RUN_SMOKE="${INIT_RUN_SMOKE:-1}"
INIT_RESET_SHM="${INIT_RESET_SHM:-0}"
INIT_KEEP_WORK_DIR="${INIT_KEEP_WORK_DIR:-0}"
INIT_TLS_UPDATE_LOCAL_APP="${INIT_TLS_UPDATE_LOCAL_APP:-1}"
INIT_TLS_FORCE_KEY="${INIT_TLS_FORCE_KEY:-0}"
INIT_TLS_VALIDITY_DAYS="${INIT_TLS_VALIDITY_DAYS:-}"
INIT_MQTT_CONNECT_TEST="${INIT_MQTT_CONNECT_TEST:-0}"
if [ -z "${INIT_PROMPT:-}" ]; then
  if [ -t 0 ]; then
    INIT_PROMPT=1
  else
    INIT_PROMPT=0
  fi
fi

cleanup() {
  if [ "$INIT_KEEP_WORK_DIR" != "1" ] && [ -n "$INIT_WORK_DIR" ] && [ -d "$INIT_WORK_DIR" ]; then
    rm -rf "$INIT_WORK_DIR"
  fi
}

trap cleanup EXIT INT TERM

truthy() {
  case "$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')" in
    1|y|yes|true|on) return 0 ;;
    *) return 1 ;;
  esac
}

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

safe_machine_file_prefix() {
  printf '%s' "$1" | sed 's/[^A-Za-z0-9._-]/_/g'
}

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

rm -rf "$INIT_WORK_DIR"
mkdir -p "$INIT_WORK_DIR"
tar -xzf "$INIT_PACKAGE" -C "$INIT_WORK_DIR"
PACKAGE_ROOT=$(find_package_root) || {
  echo "factory package root not found after extract: $INIT_PACKAGE" >&2
  exit 1
}

require_file "$PACKAGE_ROOT/deploy/install-factory-config.sh" "install script"

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

FACTORY_MACHINE_CODE=$(json_string_value "$PACKAGE_ROOT/config/factory/runtime/device_identity.json" "machineCode" || true)
FACTORY_MQTT_FILE="$PACKAGE_ROOT/config/factory/runtime/apps/mqtt-service.json"
FACTORY_MQTT_BROKER=$(json_string_value "$FACTORY_MQTT_FILE" "broker" || true)
FACTORY_MQTT_USERNAME=$(json_string_value "$FACTORY_MQTT_FILE" "username" || true)
FACTORY_MQTT_PASSWORD=$(json_string_value "$FACTORY_MQTT_FILE" "password" || true)
FACTORY_MQTT_TLS_ENABLED=$(json_tls_bool_value "$FACTORY_MQTT_FILE" "enabled" || true)
FACTORY_MQTT_CA_FILE=$(json_string_value "$FACTORY_MQTT_FILE" "caFile" || true)
FACTORY_MQTT_CERT_FILE=$(json_string_value "$FACTORY_MQTT_FILE" "certFile" || true)
FACTORY_MQTT_KEY_FILE=$(json_string_value "$FACTORY_MQTT_FILE" "keyFile" || true)
FACTORY_MQTT_TLS_INSECURE=$(json_tls_bool_value "$FACTORY_MQTT_FILE" "insecureSkipVerify" || true)

DEFAULT_MACHINE_CODE=$(first_nonempty "${INIT_MACHINE_CODE:-}" "$EXISTING_MACHINE_CODE" "$FACTORY_MACHINE_CODE" "GW_FACTORY_001")
DEFAULT_MQTT_BROKER=$(first_nonempty "${INIT_MQTT_BROKER:-}" "$EXISTING_MQTT_BROKER" "$FACTORY_MQTT_BROKER" "tcp://127.0.0.1:1883")
DEFAULT_MQTT_USERNAME=$(first_nonempty "${INIT_MQTT_USERNAME:-}" "$EXISTING_MQTT_USERNAME" "$FACTORY_MQTT_USERNAME")
DEFAULT_MQTT_PASSWORD=$(first_nonempty "${INIT_MQTT_PASSWORD:-}" "$EXISTING_MQTT_PASSWORD" "$FACTORY_MQTT_PASSWORD")
DEFAULT_MQTT_TLS_ENABLED=$(first_nonempty "${INIT_MQTT_TLS_ENABLED:-}" "$EXISTING_MQTT_TLS_ENABLED" "$FACTORY_MQTT_TLS_ENABLED" "$(broker_implies_tls_value "$DEFAULT_MQTT_BROKER")")
DEFAULT_MQTT_TLS_INSECURE=$(first_nonempty "${INIT_MQTT_INSECURE_SKIP_VERIFY:-}" "$EXISTING_MQTT_TLS_INSECURE" "$FACTORY_MQTT_TLS_INSECURE" "false")

if is_interactive_init; then
  echo "Gateway production initialization"
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
    safe_machine=$(safe_machine_file_prefix "$INIT_MACHINE_CODE")
    DEFAULT_MQTT_CA_FILE=$(first_nonempty "${INIT_MQTT_CA_FILE:-}" "$EXISTING_MQTT_CA_FILE" "$FACTORY_MQTT_CA_FILE" "$GATEWAY_HOME/config/runtime/tls/ca.crt")
    DEFAULT_MQTT_CERT_FILE=$(first_nonempty "${INIT_MQTT_CERT_FILE:-}" "$EXISTING_MQTT_CERT_FILE" "$FACTORY_MQTT_CERT_FILE" "$GATEWAY_HOME/config/runtime/tls/$safe_machine-client.pem")
    DEFAULT_MQTT_KEY_FILE=$(first_nonempty "${INIT_MQTT_KEY_FILE:-}" "$EXISTING_MQTT_KEY_FILE" "$FACTORY_MQTT_KEY_FILE" "$GATEWAY_HOME/config/runtime/tls/$safe_machine-client.key")
    INIT_MQTT_CA_FILE=$(prompt_value "MQTT TLS caFile" "$DEFAULT_MQTT_CA_FILE")
    INIT_MQTT_CERT_FILE=$(prompt_value "MQTT TLS certFile" "$DEFAULT_MQTT_CERT_FILE")
    INIT_MQTT_KEY_FILE=$(prompt_value "MQTT TLS keyFile" "$DEFAULT_MQTT_KEY_FILE")
    INIT_TLS_PLATFORM_URL=$(prompt_value "TLS enrollment platform URL" "${INIT_TLS_PLATFORM_URL:-}")
    INIT_TLS_ENROLLMENT_TOKEN=$(prompt_value "TLS enrollment token" "${INIT_TLS_ENROLLMENT_TOKEN:-}" 1)
    INIT_TLS_VALIDITY_DAYS=$(prompt_value "TLS certificate validity days" "$INIT_TLS_VALIDITY_DAYS")
  fi
  INIT_START_SERVICES=$(prompt_bool "Start gateway services after init" "$INIT_START_SERVICES")
  INIT_RUN_SMOKE=$(prompt_bool "Run production smoke test" "$INIT_RUN_SMOKE")
  INIT_RESET_SHM=$(prompt_bool "Reset shared memory before start" "$INIT_RESET_SHM")
  INIT_MQTT_CONNECT_TEST=$(prompt_bool "Run MQTT connect test in smoke test" "$INIT_MQTT_CONNECT_TEST")
fi

export INIT_MACHINE_CODE="${INIT_MACHINE_CODE:-$DEFAULT_MACHINE_CODE}"
export INIT_MQTT_BROKER="${INIT_MQTT_BROKER:-$DEFAULT_MQTT_BROKER}"
export INIT_MQTT_USERNAME="${INIT_MQTT_USERNAME:-$DEFAULT_MQTT_USERNAME}"
export INIT_MQTT_PASSWORD="${INIT_MQTT_PASSWORD:-$DEFAULT_MQTT_PASSWORD}"
export INIT_MQTT_TLS_ENABLED="${INIT_MQTT_TLS_ENABLED:-$DEFAULT_MQTT_TLS_ENABLED}"
export INIT_MQTT_INSECURE_SKIP_VERIFY="${INIT_MQTT_INSECURE_SKIP_VERIFY:-$DEFAULT_MQTT_TLS_INSECURE}"
export INIT_START_SERVICES INIT_RUN_SMOKE INIT_RESET_SHM INIT_MQTT_CONNECT_TEST

tls_requested=0
if [ -n "${INIT_TLS_PLATFORM_URL:-}" ] || [ -n "${INIT_TLS_ENROLLMENT_TOKEN:-}" ]; then
  tls_requested=1
fi
if truthy "${INIT_MQTT_TLS_ENABLED:-false}" || { [ -n "${INIT_MQTT_BROKER:-}" ] && broker_implies_tls "$INIT_MQTT_BROKER"; }; then
  tls_requested=1
fi

if [ "$tls_requested" -eq 1 ] && [ -n "${INIT_MACHINE_CODE:-}" ]; then
  safe_machine=$(safe_machine_file_prefix "$INIT_MACHINE_CODE")
  export INIT_MQTT_TLS_ENABLED="${INIT_MQTT_TLS_ENABLED:-true}"
  export INIT_MQTT_INSECURE_SKIP_VERIFY="${INIT_MQTT_INSECURE_SKIP_VERIFY:-false}"
  export INIT_MQTT_CA_FILE="${INIT_MQTT_CA_FILE:-$GATEWAY_HOME/config/runtime/tls/ca.crt}"
  export INIT_MQTT_CERT_FILE="${INIT_MQTT_CERT_FILE:-$GATEWAY_HOME/config/runtime/tls/$safe_machine-client.pem}"
  export INIT_MQTT_KEY_FILE="${INIT_MQTT_KEY_FILE:-$GATEWAY_HOME/config/runtime/tls/$safe_machine-client.key}"
fi

export SOURCE_ROOT="$PACKAGE_ROOT"
export DEPLOY_DIR="$PACKAGE_ROOT/deploy"
export FACTORY_PROMPT=0
export FACTORY_PACKAGE=""
export START_SERVICES=0
export RESET_SHM="$INIT_RESET_SHM"

sh "$PACKAGE_ROOT/deploy/install-factory-config.sh"

if [ "$tls_requested" -eq 1 ]; then
  if [ -z "${INIT_TLS_PLATFORM_URL:-}" ] || [ -z "${INIT_TLS_ENROLLMENT_TOKEN:-}" ]; then
    echo "TLS requested but INIT_TLS_PLATFORM_URL or INIT_TLS_ENROLLMENT_TOKEN is empty" >&2
    exit 2
  fi
  if [ -z "${INIT_MACHINE_CODE:-}" ]; then
    echo "INIT_MACHINE_CODE is required for TLS enrollment" >&2
    exit 2
  fi
  require_file "$GATEWAY_HOME/bin/gateway-tls-enroll.sh" "TLS enrollment script"

  set -- \
    --machine-code "$INIT_MACHINE_CODE" \
    --platform-url "$INIT_TLS_PLATFORM_URL" \
    --token "$INIT_TLS_ENROLLMENT_TOKEN"
  if [ -n "$INIT_TLS_VALIDITY_DAYS" ]; then
    set -- "$@" --validity-days "$INIT_TLS_VALIDITY_DAYS"
  fi
  if truthy "$INIT_TLS_UPDATE_LOCAL_APP"; then
    set -- "$@" --update-local-app
  fi
  if truthy "$INIT_TLS_FORCE_KEY"; then
    set -- "$@" --force-key
  fi

  GATEWAY_HOME="$GATEWAY_HOME" sh "$GATEWAY_HOME/bin/gateway-tls-enroll.sh" "$@"
fi

if truthy "$INIT_START_SERVICES"; then
  if command -v systemctl >/dev/null 2>&1; then
    systemctl restart gateway-services.service
  elif [ -x "$GATEWAY_HOME/bin/gateway-services.sh" ]; then
    "$GATEWAY_HOME/bin/gateway-services.sh" restart
  fi
fi

if truthy "$INIT_RUN_SMOKE"; then
  require_file "$GATEWAY_HOME/bin/production-smoke-test.sh" "smoke test script"
  MQTT_CONNECT_TEST="$INIT_MQTT_CONNECT_TEST" GATEWAY_HOME="$GATEWAY_HOME" sh "$GATEWAY_HOME/bin/production-smoke-test.sh"
fi

echo "production init completed"
