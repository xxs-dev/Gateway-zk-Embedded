#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
UNIT_PATHS="${SZRL_UNIT_PATHS:-/etc/systemd/system/szrl_start.service:/lib/systemd/system/szrl_start.service:/usr/lib/systemd/system/szrl_start.service}"
DROPIN_SOURCE="${SZRL_DROPIN_SOURCE:-$SCRIPT_DIR/10-szrl-start-idempotent.conf}"
DROPIN_PATH="${SZRL_DROPIN_PATH:-/etc/systemd/system/szrl_start.service.d/10-gateway-idempotent.conf}"
START_SCRIPT_PATH="${SZRL_START_SCRIPT_PATH:-/data/szrl/szrl_start.sh}"
EXPECTED_START_SCRIPT_SHA256="${SZRL_START_SCRIPT_SHA256:-0ed716f3fe4f7ec10d6222b59530e764b7acf2102beac724b6c89986abfda12b}"

find_unit() {
  old_ifs=$IFS
  IFS=:
  for candidate in $UNIT_PATHS; do
    if [ -f "$candidate" ]; then
      IFS=$old_ifs
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  IFS=$old_ifs
  return 1
}

unit_is_supported() {
  unit_file="$1"
  command -v sha256sum >/dev/null 2>&1 || return 1
  grep -Eq '^[[:space:]]*ExecStart=/data/szrl/szrl_start\.sh[[:space:]]+-n[[:space:]]+2[[:space:]]*$' "$unit_file" || return 1
  grep -Eq '^[[:space:]]*KillMode=process[[:space:]]*$' "$unit_file" || return 1
  if grep -Eq '^[[:space:]]*Type=' "$unit_file"; then
    grep -Eq '^[[:space:]]*Type=simple[[:space:]]*$' "$unit_file" || return 1
  fi
  [ -f "$START_SCRIPT_PATH" ] || return 1
  actual_sha256=$(sha256sum "$START_SCRIPT_PATH" | awk '{print $1}')
  [ "$actual_sha256" = "$EXPECTED_START_SCRIPT_SHA256" ]
}

remove_managed_dropin() {
  if [ -f "$DROPIN_PATH" ]; then
    rm -f "$DROPIN_PATH"
    rmdir "$(dirname "$DROPIN_PATH")" 2>/dev/null || true
    printf 'removed incompatible Gateway-zk szrl_start drop-in: %s\n' "$DROPIN_PATH"
  fi
}

install_dropin() {
  unit_file=$(find_unit || true)
  if [ -z "$unit_file" ]; then
    remove_managed_dropin
    printf 'szrl_start.service is absent; idempotency drop-in skipped\n'
    return 0
  fi
  if ! unit_is_supported "$unit_file"; then
    remove_managed_dropin
    printf 'unsupported szrl_start.service signature; idempotency drop-in skipped: %s\n' "$unit_file" >&2
    return 0
  fi
  if [ ! -f "$DROPIN_SOURCE" ]; then
    printf 'szrl_start idempotency drop-in source is missing: %s\n' "$DROPIN_SOURCE" >&2
    return 2
  fi
  mkdir -p "$(dirname "$DROPIN_PATH")"
  cp "$DROPIN_SOURCE" "$DROPIN_PATH"
  chmod 0644 "$DROPIN_PATH"
  printf 'installed szrl_start idempotency drop-in for supported unit: %s\n' "$unit_file"
}

case "${1:-install}" in
  check)
    unit_file=$(find_unit || true)
    [ -n "$unit_file" ] && unit_is_supported "$unit_file"
    ;;
  install)
    install_dropin
    ;;
  *)
    printf 'Usage: %s [check|install]\n' "$0" >&2
    exit 2
    ;;
esac
