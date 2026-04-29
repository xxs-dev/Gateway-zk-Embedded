#!/bin/sh
set -eu

GATEWAY_HOME="${GATEWAY_HOME:-/opt/modbus-gateway}"
DEFAULT_SOURCE_ROOT="${DEFAULT_SOURCE_ROOT:-/home/gateway-factory}"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
BACKUP_DIR="${BACKUP_DIR:-$GATEWAY_HOME/backup}"
START_SERVICES="${START_SERVICES:-1}"
RESET_SHM="${RESET_SHM:-0}"

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

pick_source_root() {
  for dir in "$@"; do
    if [ -n "$dir" ] && { [ -d "$dir/config/factory/runtime" ] || [ -d "$dir/deploy" ] || [ -d "$dir/build-aarch64" ]; }; then
      printf '%s\n' "$dir"
      return 0
    fi
  done
  return 1
}

SOURCE_ROOT="${SOURCE_ROOT:-}"
if [ -z "$SOURCE_ROOT" ]; then
  SOURCE_ROOT=$(pick_source_root "$DEFAULT_SOURCE_ROOT" "$ROOT_DIR" "$SCRIPT_DIR" || true)
fi
DEPLOY_DIR="${DEPLOY_DIR:-$SOURCE_ROOT/deploy}"

if [ -z "${FACTORY_DIR:-}" ]; then
  FACTORY_DIR=$(pick_existing_dir "$SOURCE_ROOT/config/factory" "$DEFAULT_SOURCE_ROOT/config/factory" "$ROOT_DIR/config/factory" "$SCRIPT_DIR/config/factory" || true)
fi
if [ -z "${TEMPLATES_DIR:-}" ]; then
  TEMPLATES_DIR=$(pick_existing_dir "$SOURCE_ROOT/config/templates" "$DEFAULT_SOURCE_ROOT/config/templates" "$ROOT_DIR/config/templates" "$SCRIPT_DIR/config/templates" || true)
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

json_string_value() {
  file="$1"
  key="$2"
  if [ -f "$file" ]; then
    sed -n "s/.*\"$key\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$file" | sed -n '1p'
  fi
}

set_json_string_value() {
  file="$1"
  key="$2"
  value="$3"
  if [ ! -f "$file" ]; then
    return 0
  fi
  escaped=$(printf '%s' "$value" | sed 's/[\/&]/\\&/g')
  tmp="$file.tmp.$$"
  if grep -q "\"$key\"" "$file"; then
    sed "s/\"$key\"[[:space:]]*:[[:space:]]*\"[^\"]*\"/\"$key\": \"$escaped\"/" "$file" > "$tmp"
    mv "$tmp" "$file"
  fi
}

mkdir -p "$GATEWAY_HOME/bin" "$GATEWAY_HOME/config" "$GATEWAY_HOME/data" "$GATEWAY_HOME/ota" "$BACKUP_DIR"

if command -v systemctl >/dev/null 2>&1; then
  systemctl stop gateway-services.service 2>/dev/null || true
fi
if [ -x "$GATEWAY_HOME/bin/gateway-services.sh" ]; then
  "$GATEWAY_HOME/bin/gateway-services.sh" stop 2>/dev/null || true
fi

for bin in ModbusRtu Dlt645Driver MqttDriver EventEngine SystemMonitor pointctl stress_runner; do
  install_file_if_exists "$SOURCE_ROOT/build-aarch64/$bin" "$GATEWAY_HOME/bin/$bin"
  install_file_if_exists "$SOURCE_ROOT/bin/$bin" "$GATEWAY_HOME/bin/$bin"
  install_file_if_exists "$SOURCE_ROOT/$bin" "$GATEWAY_HOME/bin/$bin"
done

install_file_if_exists "$DEPLOY_DIR/gateway-services.sh" "$GATEWAY_HOME/bin/gateway-services.sh"
install_file_if_exists "$DEPLOY_DIR/gateway-run.sh" "$GATEWAY_HOME/bin/gateway-run.sh"
install_file_if_exists "$DEPLOY_DIR/install-factory-config.sh" "$GATEWAY_HOME/bin/install-factory-config.sh"
install_file_if_exists "$DEPLOY_DIR/production-smoke-test.sh" "$GATEWAY_HOME/bin/production-smoke-test.sh"
install_file_if_exists "$DEPLOY_DIR/ota-apply.sh" "$GATEWAY_HOME/bin/ota-apply.sh"
install_file_if_exists "$DEPLOY_DIR/ota-rollback.sh" "$GATEWAY_HOME/bin/ota-rollback.sh"
chmod +x "$GATEWAY_HOME/bin/"*.sh 2>/dev/null || true
chmod +x "$GATEWAY_HOME/bin/"* 2>/dev/null || true

EXISTING_MACHINE_CODE=$(json_string_value "$GATEWAY_HOME/config/runtime/device_identity.json" "machineCode" || true)

ts=$(date +%Y%m%d%H%M%S)
if [ -d "$GATEWAY_HOME/config/runtime" ]; then
  mv "$GATEWAY_HOME/config/runtime" "$BACKUP_DIR/runtime-$ts"
  echo "backup old runtime config: $BACKUP_DIR/runtime-$ts"
fi
mkdir -p "$GATEWAY_HOME/config"
cp -a "$FACTORY_DIR/runtime" "$GATEWAY_HOME/config/runtime"

if [ -n "$EXISTING_MACHINE_CODE" ]; then
  set_json_string_value "$GATEWAY_HOME/config/runtime/device_identity.json" "machineCode" "$EXISTING_MACHINE_CODE"
  for app_file in "$GATEWAY_HOME"/config/runtime/apps/*.json; do
    [ -f "$app_file" ] || continue
    set_json_string_value "$app_file" "clientId" "$EXISTING_MACHINE_CODE"
  done
  echo "inherited machineCode: $EXISTING_MACHINE_CODE"
fi

if [ -n "$TEMPLATES_DIR" ] && [ -d "$TEMPLATES_DIR" ]; then
  if ! same_path "$TEMPLATES_DIR" "$GATEWAY_HOME/config/templates"; then
    mkdir -p "$GATEWAY_HOME/config/templates"
    cp -a "$TEMPLATES_DIR"/. "$GATEWAY_HOME/config/templates"/
  fi
fi

if command -v systemctl >/dev/null 2>&1; then
  install_file_if_exists "$DEPLOY_DIR/gateway-services.service" "/etc/systemd/system/gateway-services.service"
  install_file_if_exists "$DEPLOY_DIR/modbus-rtu@.service" "/etc/systemd/system/modbus-rtu@.service"
  install_file_if_exists "$DEPLOY_DIR/dlt645-driver@.service" "/etc/systemd/system/dlt645-driver@.service"
  install_file_if_exists "$DEPLOY_DIR/mqtt-driver@.service" "/etc/systemd/system/mqtt-driver@.service"
  install_file_if_exists "$DEPLOY_DIR/event-engine@.service" "/etc/systemd/system/event-engine@.service"
  install_file_if_exists "$DEPLOY_DIR/system-monitor@.service" "/etc/systemd/system/system-monitor@.service"
  install_file_if_exists "$DEPLOY_DIR/mqtt-tls-tunnel@.service" "/etc/systemd/system/mqtt-tls-tunnel@.service"
  systemctl daemon-reload
  systemctl enable gateway-services.service >/dev/null 2>&1 || true
fi

if [ "$RESET_SHM" = "1" ]; then
  if command -v systemctl >/dev/null 2>&1; then
    "$GATEWAY_HOME/bin/gateway-services.sh" stop || true
  fi
  rm -f /dev/shm/gateway_point_store* 2>/dev/null || true
fi

if [ "$START_SERVICES" = "1" ] && command -v systemctl >/dev/null 2>&1; then
  systemctl restart gateway-services.service
fi

echo "factory source root: $SOURCE_ROOT"
echo "factory config installed to $GATEWAY_HOME/config/runtime"
