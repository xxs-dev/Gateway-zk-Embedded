#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
DEVICES_FILE="${DEVICES_FILE:-$SCRIPT_DIR/devices.csv}"
PACKAGE="${FACTORY_PACKAGE:-$ROOT_DIR/gateway-factory-defaults.tar.gz}"
REMOTE_DIR="${REMOTE_DIR:-/tmp/gateway-production-init}"
SSH_CONNECT_TIMEOUT="${SSH_CONNECT_TIMEOUT:-10}"
SSH_EXTRA_OPTS="${SSH_EXTRA_OPTS:-}"
DRY_RUN=0

usage() {
  cat >&2 <<'EOF'
Usage: batch-init-devices.sh [options]

Options:
  --devices FILE     CSV device list; defaults to deploy/devices.csv
  --package FILE     gateway-factory-defaults.tar.gz path
  --remote-dir DIR   target temp directory; defaults to /tmp/gateway-production-init
  --dry-run          print actions without copying or executing
  -h, --help         show help

CSV columns:
  host,port,user,password,machine_code,mqtt_broker,mqtt_username,mqtt_password,tls_platform_url,tls_enrollment_token,tls_validity_days,runtime_mode,tls_generate_root_ca,tls_ca_validity_days,tls_ca_subject

Transport:
  Uses scp to upload the package and production-init.sh, then ssh to execute it.
  Password login requires sshpass on the control host. Without sshpass, configure SSH key auth.

Global defaults can be supplied with environment variables:
  EDGE_SSH_USER EDGE_SSH_PASSWORD EDGE_SSH_PORT
  INIT_RUNTIME_MODE
  INIT_MQTT_BROKER INIT_MQTT_USERNAME INIT_MQTT_PASSWORD
  INIT_TLS_PLATFORM_URL INIT_TLS_ENROLLMENT_TOKEN INIT_TLS_VALIDITY_DAYS
  INIT_TLS_GENERATE_ROOT_CA INIT_TLS_CA_VALIDITY_DAYS INIT_TLS_CA_SUBJECT
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --devices)
      DEVICES_FILE="${2:-}"
      shift 2
      ;;
    --package)
      PACKAGE="${2:-}"
      shift 2
      ;;
    --remote-dir)
      REMOTE_DIR="${2:-}"
      shift 2
      ;;
    --dry-run)
      DRY_RUN=1
      shift
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

trim() {
  printf '%s' "$1" | sed 's/\r$//' | sed 's/^[[:space:]]*//' | sed 's/[[:space:]]*$//'
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

shell_quote() {
  printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

require_file() {
  if [ ! -f "$1" ]; then
    echo "required file not found: $1" >&2
    exit 1
  fi
}

require_file "$DEVICES_FILE"
require_file "$PACKAGE"
require_file "$SCRIPT_DIR/production-init.sh"

if ! command -v ssh >/dev/null 2>&1; then
  echo "ssh command not found" >&2
  exit 1
fi
if ! command -v scp >/dev/null 2>&1; then
  echo "scp command not found" >&2
  exit 1
fi

ssh_base_opts="-o ConnectTimeout=$SSH_CONNECT_TIMEOUT -o StrictHostKeyChecking=accept-new $SSH_EXTRA_OPTS"

run_ssh() {
  host="$1"
  port="$2"
  user="$3"
  password="$4"
  command="$5"
  target="$user@$host"
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "ssh -p $port $target $(shell_quote "$command")"
    return 0
  fi
  if [ -n "$password" ]; then
    if ! command -v sshpass >/dev/null 2>&1; then
      echo "sshpass is required for password login to $target; install sshpass or configure SSH key auth" >&2
      return 127
    fi
    SSHPASS="$password" sshpass -e ssh $ssh_base_opts -p "$port" "$target" "$command"
  else
    ssh $ssh_base_opts -p "$port" "$target" "$command"
  fi
}

run_scp() {
  host="$1"
  port="$2"
  user="$3"
  password="$4"
  src="$5"
  dst="$6"
  target="$user@$host:$dst"
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "scp -P $port $(shell_quote "$src") $(shell_quote "$target")"
    return 0
  fi
  if [ -n "$password" ]; then
    if ! command -v sshpass >/dev/null 2>&1; then
      echo "sshpass is required for password login to $target; install sshpass or configure SSH key auth" >&2
      return 127
    fi
    SSHPASS="$password" sshpass -e scp $ssh_base_opts -P "$port" "$src" "$target"
  else
    scp $ssh_base_opts -P "$port" "$src" "$target"
  fi
}

build_remote_env() {
  machine_code="$1"
  broker="$2"
  mqtt_username="$3"
  mqtt_password="$4"
  tls_platform_url="$5"
  tls_token="$6"
  tls_validity_days="$7"
  runtime_mode="$8"
  tls_generate_root_ca="$9"
  tls_ca_validity_days="${10}"
  tls_ca_subject="${11}"

  remote_env="INIT_MACHINE_CODE=$(shell_quote "$machine_code")"
  if [ -n "$runtime_mode" ]; then
    remote_env="$remote_env INIT_RUNTIME_MODE=$(shell_quote "$runtime_mode")"
  fi
  if [ -n "$broker" ]; then
    remote_env="$remote_env INIT_MQTT_BROKER=$(shell_quote "$broker")"
  fi
  if [ -n "$mqtt_username" ]; then
    remote_env="$remote_env INIT_MQTT_USERNAME=$(shell_quote "$mqtt_username")"
  fi
  if [ -n "$mqtt_password" ]; then
    remote_env="$remote_env INIT_MQTT_PASSWORD=$(shell_quote "$mqtt_password")"
  fi
  if [ -n "$tls_platform_url" ]; then
    remote_env="$remote_env INIT_TLS_PLATFORM_URL=$(shell_quote "$tls_platform_url")"
  fi
  if [ -n "$tls_token" ]; then
    remote_env="$remote_env INIT_TLS_ENROLLMENT_TOKEN=$(shell_quote "$tls_token")"
  fi
  if [ -n "$tls_validity_days" ]; then
    remote_env="$remote_env INIT_TLS_VALIDITY_DAYS=$(shell_quote "$tls_validity_days")"
  fi
  if [ -n "$tls_generate_root_ca" ]; then
    remote_env="$remote_env INIT_TLS_GENERATE_ROOT_CA=$(shell_quote "$tls_generate_root_ca")"
  fi
  if [ -n "$tls_ca_validity_days" ]; then
    remote_env="$remote_env INIT_TLS_CA_VALIDITY_DAYS=$(shell_quote "$tls_ca_validity_days")"
  fi
  if [ -n "$tls_ca_subject" ]; then
    remote_env="$remote_env INIT_TLS_CA_SUBJECT=$(shell_quote "$tls_ca_subject")"
  fi
  remote_env="$remote_env INIT_START_SERVICES=$(shell_quote "${INIT_START_SERVICES:-1}")"
  remote_env="$remote_env INIT_RUN_SMOKE=$(shell_quote "${INIT_RUN_SMOKE:-1}")"
  remote_env="$remote_env INIT_RESET_SHM=$(shell_quote "${INIT_RESET_SHM:-0}")"
  remote_env="$remote_env INIT_MQTT_CONNECT_TEST=$(shell_quote "${INIT_MQTT_CONNECT_TEST:-0}")"
  remote_env="$remote_env INIT_PROMPT=0"
  printf '%s\n' "$remote_env"
}

ok_count=0
fail_count=0

while IFS=, read -r host port user password machine_code mqtt_broker mqtt_username mqtt_password tls_platform_url tls_token tls_validity_days runtime_mode tls_generate_root_ca tls_ca_validity_days tls_ca_subject rest || [ -n "${host:-}" ]; do
  host=$(trim "${host:-}")
  case "$host" in
    ""|\#*) continue ;;
    host) continue ;;
  esac

  port=$(first_nonempty "$(trim "${port:-}")" "${EDGE_SSH_PORT:-}" "22")
  user=$(first_nonempty "$(trim "${user:-}")" "${EDGE_SSH_USER:-}" "root")
  password=$(first_nonempty "$(trim "${password:-}")" "${EDGE_SSH_PASSWORD:-}" "")
  machine_code=$(first_nonempty "$(trim "${machine_code:-}")" "")
  mqtt_broker=$(first_nonempty "$(trim "${mqtt_broker:-}")" "${INIT_MQTT_BROKER:-}" "")
  mqtt_username=$(first_nonempty "$(trim "${mqtt_username:-}")" "${INIT_MQTT_USERNAME:-}" "")
  mqtt_password=$(first_nonempty "$(trim "${mqtt_password:-}")" "${INIT_MQTT_PASSWORD:-}" "")
  tls_platform_url=$(first_nonempty "$(trim "${tls_platform_url:-}")" "${INIT_TLS_PLATFORM_URL:-}" "")
  tls_token=$(first_nonempty "$(trim "${tls_token:-}")" "${INIT_TLS_ENROLLMENT_TOKEN:-}" "")
  tls_validity_days=$(first_nonempty "$(trim "${tls_validity_days:-}")" "${INIT_TLS_VALIDITY_DAYS:-}" "")
  runtime_mode=$(first_nonempty "$(trim "${runtime_mode:-}")" "${INIT_RUNTIME_MODE:-}" "")
  tls_generate_root_ca=$(first_nonempty "$(trim "${tls_generate_root_ca:-}")" "${INIT_TLS_GENERATE_ROOT_CA:-}" "")
  tls_ca_validity_days=$(first_nonempty "$(trim "${tls_ca_validity_days:-}")" "${INIT_TLS_CA_VALIDITY_DAYS:-}" "")
  tls_ca_subject=$(first_nonempty "$(trim "${tls_ca_subject:-}")" "${INIT_TLS_CA_SUBJECT:-}" "")

  if [ -z "$machine_code" ]; then
    echo "skip $host: machine_code is empty" >&2
    fail_count=$((fail_count + 1))
    continue
  fi

  echo "== init $host as $machine_code =="
  if run_ssh "$host" "$port" "$user" "$password" "rm -rf $(shell_quote "$REMOTE_DIR") && mkdir -p $(shell_quote "$REMOTE_DIR")" &&
     run_scp "$host" "$port" "$user" "$password" "$PACKAGE" "$REMOTE_DIR/gateway-factory-defaults.tar.gz" &&
     run_scp "$host" "$port" "$user" "$password" "$SCRIPT_DIR/production-init.sh" "$REMOTE_DIR/production-init.sh"; then
    remote_env=$(build_remote_env "$machine_code" "$mqtt_broker" "$mqtt_username" "$mqtt_password" "$tls_platform_url" "$tls_token" "$tls_validity_days" "$runtime_mode" "$tls_generate_root_ca" "$tls_ca_validity_days" "$tls_ca_subject")
    remote_cmd="cd $(shell_quote "$REMOTE_DIR") && chmod +x production-init.sh && $remote_env INIT_PACKAGE=$(shell_quote "$REMOTE_DIR/gateway-factory-defaults.tar.gz") sh ./production-init.sh"
    if run_ssh "$host" "$port" "$user" "$password" "$remote_cmd"; then
      ok_count=$((ok_count + 1))
      echo "init ok: $host"
    else
      fail_count=$((fail_count + 1))
      echo "init failed: $host" >&2
    fi
  else
    fail_count=$((fail_count + 1))
    echo "upload failed: $host" >&2
  fi
done < "$DEVICES_FILE"

echo "batch init completed: ok=$ok_count failed=$fail_count"
[ "$fail_count" -eq 0 ]
