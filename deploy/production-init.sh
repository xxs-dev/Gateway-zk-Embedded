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

broker_implies_tls() {
  case "$1" in
    ssl://*|tls://*|mqtts://*) return 0 ;;
    *) return 1 ;;
  esac
}

safe_machine_file_prefix() {
  printf '%s' "$1" | sed 's/[^A-Za-z0-9._-]/_/g'
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

tls_requested=0
if [ -n "${INIT_TLS_PLATFORM_URL:-}" ] || [ -n "${INIT_TLS_ENROLLMENT_TOKEN:-}" ]; then
  tls_requested=1
fi
if [ -n "${INIT_MQTT_BROKER:-}" ] && broker_implies_tls "$INIT_MQTT_BROKER"; then
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
