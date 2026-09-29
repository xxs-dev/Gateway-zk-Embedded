#!/bin/bash
set -Eeuo pipefail

SOURCE_DIR=${SOURCE_DIR:-/tmp/gateway-network-resilience}
GATEWAY_HOME=${GATEWAY_HOME:-/opt/modbus-gateway}
CONFIG_FILE=${CONFIG_FILE:-/etc/default/gateway-network-failover}
EASYTIER_CONFIG=${EASYTIER_CONFIG:-/etc/easytier/et.conf}
ACTIVATE_NETWORK_SERVICES=${ACTIVATE_NETWORK_SERVICES:-0}
NETWORK_RECOVERY_TIMEOUT_SEC=${NETWORK_RECOVERY_TIMEOUT_SEC:-120}
NETWORK_STABLE_PASSES=${NETWORK_STABLE_PASSES:-3}
PROBE_HOST=${PROBE_HOST:-kygate.kyxn.net}
STAMP=$(date +%Y%m%d-%H%M%S)
BACKUP_DIR=${BACKUP_DIR:-$GATEWAY_HOME/data/network-upgrades/$STAMP}
ABSENT_FILE="$BACKUP_DIR/absent-files.txt"
ROLLBACK_NEEDED=0

CELLULAR_TARGET="$GATEWAY_HOME/bin/gateway-cellular.sh"
FAILOVER_TARGET="$GATEWAY_HOME/bin/gateway-network-failover.sh"
SZRL_HELPER_TARGET="$GATEWAY_HOME/bin/gateway-szrl-idempotency.sh"
CELLULAR_UNIT_TARGET=/etc/systemd/system/gateway-cellular.service
FAILOVER_UNIT_TARGET=/etc/systemd/system/gateway-network-failover.service
SZRL_DROPIN_TARGET=/etc/systemd/system/szrl_start.service.d/10-gateway-idempotent.conf

FILES=(
  "gateway-cellular.sh|$CELLULAR_TARGET|0755"
  "gateway-network-failover.sh|$FAILOVER_TARGET|0755"
  "gateway-szrl-idempotency.sh|$SZRL_HELPER_TARGET|0755"
  "gateway-cellular.service|$CELLULAR_UNIT_TARGET|0644"
  "gateway-network-failover.service|$FAILOVER_UNIT_TARGET|0644"
)
BACKUP_TARGETS=(
  "$CELLULAR_TARGET"
  "$FAILOVER_TARGET"
  "$SZRL_HELPER_TARGET"
  "$CELLULAR_UNIT_TARGET"
  "$FAILOVER_UNIT_TARGET"
  "$SZRL_DROPIN_TARGET"
)

fail() {
  echo "network resilience upgrade failed: $*" >&2
  exit 1
}

unit_exists() {
  systemctl cat "$1" >/dev/null 2>&1
}

unit_was_active() {
  systemctl is-active --quiet "$1"
}

unit_was_enabled() {
  systemctl is-enabled --quiet "$1"
}

easytier_process_ready() {
  for comm in /proc/[0-9]*/comm; do
    [ -r "$comm" ] || continue
    grep -qx 'easytier-core' "$comm" && return 0
  done
  return 1
}

easytier_address_ready() {
  local expected_ipv4
  expected_ipv4=$(sed -n 's/^[[:space:]]*ipv4[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' \
    "$EASYTIER_CONFIG" 2>/dev/null | sed -n '1p')
  [ -n "$expected_ipv4" ] || return 0
  printf '%s\n' "$expected_ipv4" | awk -F '[./]' '
    NF == 5 &&
    $1 ~ /^[0-9]+$/ && $1 >= 0 && $1 <= 255 &&
    $2 ~ /^[0-9]+$/ && $2 >= 0 && $2 <= 255 &&
    $3 ~ /^[0-9]+$/ && $3 >= 0 && $3 <= 255 &&
    $4 ~ /^[0-9]+$/ && $4 >= 0 && $4 <= 255 &&
    $5 ~ /^[0-9]+$/ && $5 >= 0 && $5 <= 32 { valid = 1 }
    END { exit valid ? 0 : 1 }
  ' || return 0
  ip -4 -o address show 2>/dev/null | awk -v expected="$expected_ipv4" \
    '$4 == expected { found = 1 } END { exit found ? 0 : 1 }'
}

easytier_ready() {
  if unit_exists easytier.service; then
    systemctl is-active --quiet easytier.service || return 1
  else
    easytier_process_ready || return 1
  fi
  easytier_address_ready
}

restore_enabled_state() {
  local unit="$1"
  local enabled="$2"
  if [ "$enabled" = "1" ]; then
    systemctl enable "$unit" >/dev/null 2>&1 || true
  else
    systemctl disable "$unit" >/dev/null 2>&1 || true
  fi
}

backup_target() {
  local target="$1"
  local backup="$BACKUP_DIR/rootfs$target"
  if [ -e "$target" ] || [ -L "$target" ]; then
    mkdir -p "$(dirname "$backup")"
    cp -a "$target" "$backup"
  else
    echo "$target" >> "$ABSENT_FILE"
  fi
}

restore_target() {
  local target="$1"
  local backup="$BACKUP_DIR/rootfs$target"
  if [ -e "$backup" ] || [ -L "$backup" ]; then
    mkdir -p "$(dirname "$target")"
    cp -a "$backup" "$target"
  elif grep -Fxq "$target" "$ABSENT_FILE" 2>/dev/null; then
    rm -f "$target"
  fi
}

cellular_ipv4_ready() {
  ip -4 -o address show dev "$CELLULAR_INTERFACE" scope global 2>/dev/null | grep -q .
}

cellular_default_ready() {
  ip -4 route show default dev "$CELLULAR_INTERFACE" 2>/dev/null | grep -q ' via '
}

wait_for_cellular_network() {
  local elapsed=0
  local stable=0
  while [ "$elapsed" -lt "$NETWORK_RECOVERY_TIMEOUT_SEC" ]; do
    if systemctl is-active --quiet gateway-cellular.service &&
       cellular_ipv4_ready && cellular_default_ready &&
       ping -I "$CELLULAR_INTERFACE" -c 1 -W 2 "$PROBE_HOST" >/dev/null 2>&1; then
      stable=$((stable + 1))
      if [ "$stable" -ge "$NETWORK_STABLE_PASSES" ]; then
        return 0
      fi
    else
      stable=0
    fi
    sleep 1
    elapsed=$((elapsed + 1))
  done
  return 1
}

rollback() {
  local code="$1"
  trap - EXIT INT TERM
  set +e
  echo "network resilience validation failed; rolling back from $BACKUP_DIR" >&2
  systemctl stop gateway-network-failover.service >/dev/null 2>&1 || true
  for target in "${BACKUP_TARGETS[@]}"; do
    restore_target "$target"
  done
  systemctl daemon-reload
  if [ "$CELLULAR_WAS_ACTIVE" = "1" ]; then
    systemctl restart gateway-cellular.service
    wait_for_cellular_network || true
  else
    systemctl stop gateway-cellular.service >/dev/null 2>&1 || true
  fi
  if [ "$FAILOVER_WAS_ACTIVE" = "1" ]; then
    systemctl restart gateway-network-failover.service
  else
    systemctl stop gateway-network-failover.service >/dev/null 2>&1 || true
  fi
  restore_enabled_state gateway-cellular.service "$CELLULAR_WAS_ENABLED"
  restore_enabled_state gateway-network-failover.service "$FAILOVER_WAS_ENABLED"
  echo "network resilience rollback completed" >&2
  exit "$code"
}

on_exit() {
  local code=$?
  if [ "$code" -ne 0 ] && [ "$ROLLBACK_NEEDED" = "1" ]; then
    rollback "$code"
  fi
  exit "$code"
}

on_signal() {
  trap - EXIT INT TERM
  if [ "$ROLLBACK_NEEDED" = "1" ]; then
    rollback 1
  fi
  exit 1
}
trap on_exit EXIT
trap on_signal INT TERM

[ "$(id -u)" -eq 0 ] || fail "must run as root"
[[ "$ACTIVATE_NETWORK_SERVICES" =~ ^[01]$ ]] || fail "ACTIVATE_NETWORK_SERVICES must be 0 or 1"
[[ "$NETWORK_RECOVERY_TIMEOUT_SEC" =~ ^[1-9][0-9]*$ ]] || fail "invalid recovery timeout"
[[ "$NETWORK_STABLE_PASSES" =~ ^[1-9][0-9]*$ ]] || fail "invalid stable pass count"
command -v systemctl >/dev/null 2>&1 || fail "systemctl is required"
command -v ip >/dev/null 2>&1 || fail "ip is required"
command -v ping >/dev/null 2>&1 || fail "ping is required"

for source in gateway-cellular.sh gateway-network-failover.sh gateway-szrl-idempotency.sh; do
  [ -f "$SOURCE_DIR/$source" ] || fail "missing source file: $source"
  bash -n "$SOURCE_DIR/$source"
done
for source in gateway-cellular.service gateway-network-failover.service 10-szrl-start-idempotent.conf; do
  [ -f "$SOURCE_DIR/$source" ] || fail "missing source file: $source"
done

CELLULAR_INTERFACE=usb0
if [ -f "$CONFIG_FILE" ]; then
  # shellcheck disable=SC1090
  source "$CONFIG_FILE"
fi
CELLULAR_INTERFACE=${CELLULAR_INTERFACE:-usb0}

CELLULAR_WAS_ACTIVE=0
FAILOVER_WAS_ACTIVE=0
CELLULAR_WAS_ENABLED=0
FAILOVER_WAS_ENABLED=0
unit_was_active gateway-cellular.service && CELLULAR_WAS_ACTIVE=1
unit_was_active gateway-network-failover.service && FAILOVER_WAS_ACTIVE=1
unit_was_enabled gateway-cellular.service && CELLULAR_WAS_ENABLED=1
unit_was_enabled gateway-network-failover.service && FAILOVER_WAS_ENABLED=1
if [ "$ACTIVATE_NETWORK_SERVICES" = "1" ]; then
  easytier_ready || fail "EasyTier baseline is not active or its configured IPv4 is absent"
fi

mkdir -p "$BACKUP_DIR"
: > "$ABSENT_FILE"
for target in "${BACKUP_TARGETS[@]}"; do
  backup_target "$target"
done
ROLLBACK_NEEDED=1

for entry in "${FILES[@]}"; do
  IFS='|' read -r source target mode <<< "$entry"
  mkdir -p "$(dirname "$target")"
  install -m "$mode" "$SOURCE_DIR/$source" "$target.new"
  mv -f "$target.new" "$target"
done

SZRL_DROPIN_SOURCE="$SOURCE_DIR/10-szrl-start-idempotent.conf" \
  "$SZRL_HELPER_TARGET" install
systemctl daemon-reload

if [ "$ACTIVATE_NETWORK_SERVICES" = "1" ]; then
  systemctl stop gateway-network-failover.service
  systemctl enable gateway-cellular.service gateway-network-failover.service >/dev/null
  systemctl restart gateway-cellular.service
  wait_for_cellular_network || fail "cellular IPv4/default route did not recover stably"
  systemctl restart gateway-network-failover.service
  systemctl is-active --quiet gateway-network-failover.service || fail "network failover service is not active"
  easytier_ready || fail "EasyTier did not remain active with its configured IPv4"
  systemctl is-active --quiet mqtt-driver@mqtt-service.service || fail "MQTT driver did not remain active"
else
  restore_enabled_state gateway-cellular.service "$CELLULAR_WAS_ENABLED"
  restore_enabled_state gateway-network-failover.service "$FAILOVER_WAS_ENABLED"
fi

ROLLBACK_NEEDED=0
trap - EXIT INT TERM
echo "network resilience upgrade complete"
echo "backup=$BACKUP_DIR"
echo "activateNetworkServices=$ACTIVATE_NETWORK_SERVICES"
echo "cellularInterface=$CELLULAR_INTERFACE"
