#!/bin/sh
set -u

GATEWAY_HOME="${GATEWAY_HOME:-/opt/modbus-gateway}"
BIN_DIR="${BIN_DIR:-$GATEWAY_HOME/bin}"
APP_CONFIG="${APP_CONFIG:-$GATEWAY_HOME/config/runtime/apps/mqtt-service.json}"
MONITOR_CONFIG="${MONITOR_CONFIG:-$GATEWAY_HOME/config/runtime/apps/monitor-service.json}"
IDENTITY_CONFIG="${IDENTITY_CONFIG:-$GATEWAY_HOME/config/runtime/device_identity.json}"
MIN_FREE_MB="${MIN_FREE_MB:-512}"
MAX_DISK_USED_PERCENT="${MAX_DISK_USED_PERCENT:-85}"
MQTT_CONNECT_TEST="${MQTT_CONNECT_TEST:-0}"

PASS_COUNT=0
WARN_COUNT=0
FAIL_COUNT=0

pass() {
  PASS_COUNT=$((PASS_COUNT + 1))
  echo "[PASS] $*"
}

warn() {
  WARN_COUNT=$((WARN_COUNT + 1))
  echo "[WARN] $*" >&2
}

fail() {
  FAIL_COUNT=$((FAIL_COUNT + 1))
  echo "[FAIL] $*" >&2
}

json_string() {
  file="$1"
  key="$2"
  sed -n "s/.*\"$key\"[[:space:]]*:[[:space:]]*\"\\([^\"]*\\)\".*/\\1/p" "$file" 2>/dev/null | head -n 1
}

json_number() {
  file="$1"
  key="$2"
  sed -n "s/.*\"$key\"[[:space:]]*:[[:space:]]*\\([0-9][0-9]*\\).*/\\1/p" "$file" 2>/dev/null | head -n 1
}

tls_block() {
  sed -n '/"tls"[[:space:]]*:/,/}/p' "$1" 2>/dev/null
}

tls_string() {
  file="$1"
  key="$2"
  tls_block "$file" | sed -n "s/.*\"$key\"[[:space:]]*:[[:space:]]*\"\\([^\"]*\\)\".*/\\1/p" | head -n 1
}

tls_bool() {
  file="$1"
  key="$2"
  tls_block "$file" | sed -n "s/.*\"$key\"[[:space:]]*:[[:space:]]*\\(true\\|false\\).*/\\1/p" | head -n 1
}

file_exists() {
  path="$1"
  label="$2"
  if [ -f "$path" ]; then
    pass "$label exists: $path"
  else
    fail "$label missing: $path"
  fi
}

exec_exists() {
  path="$1"
  label="$2"
  if [ -x "$path" ]; then
    pass "$label executable: $path"
  elif [ -f "$path" ]; then
    fail "$label exists but not executable: $path"
  else
    fail "$label missing: $path"
  fi
}

has_openssl_runtime() {
  if command -v ldconfig >/dev/null 2>&1; then
    if ldconfig -p 2>/dev/null | grep -q 'libssl' &&
       ldconfig -p 2>/dev/null | grep -q 'libcrypto'; then
      return 0
    fi
  fi
  find /lib /usr/lib -name 'libssl.so*' -print 2>/dev/null | grep -q . || return 1
  find /lib /usr/lib -name 'libcrypto.so*' -print 2>/dev/null | grep -q . || return 1
}

run_timeout() {
  seconds="$1"
  shift
  if command -v timeout >/dev/null 2>&1; then
    timeout "$seconds" "$@"
  else
    "$@"
  fi
}

check_identity() {
  echo "== identity =="
  file_exists "$IDENTITY_CONFIG" "identity config"
  [ -f "$IDENTITY_CONFIG" ] || return

  machine_code=$(json_string "$IDENTITY_CONFIG" machineCode)
  imei=$(json_string "$IDENTITY_CONFIG" imei)
  if [ -n "$machine_code" ]; then
    pass "machineCode configured: $machine_code"
    case "$machine_code" in
      GW0001|TEST*|test*) warn "machineCode looks like default/test value: $machine_code" ;;
    esac
  else
    fail "machineCode is empty in $IDENTITY_CONFIG"
  fi
  if [ -n "$imei" ]; then
    pass "imei configured"
  else
    warn "imei is empty in $IDENTITY_CONFIG"
  fi
}

check_runtime_files() {
  echo "== runtime files =="
  file_exists "$APP_CONFIG" "mqtt app config"
  file_exists "$MONITOR_CONFIG" "monitor app config"
  for bin in ModbusRtu Dlt645Driver MqttDriver EventEngine SystemMonitor pointctl; do
    exec_exists "$BIN_DIR/$bin" "$bin"
  done
  for script in gateway-services.sh gateway-run.sh production-smoke-test.sh ota-apply.sh ota-rollback.sh; do
    exec_exists "$BIN_DIR/$script" "$script"
  done
}

extract_device_files() {
  file="$1"
  sed -n '/"deviceConfigFiles"[[:space:]]*:/,/\]/p' "$file" 2>/dev/null |
    sed -n 's/.*"\([^"]*\.json\)".*/\1/p'
}

resolve_config_path() {
  cfg="$1"
  path="$2"
  case "$path" in
    /*) printf '%s\n' "$path" ;;
    *) printf '%s\n' "$(dirname "$cfg")/$path" ;;
  esac
}

check_device_refs() {
  echo "== device config references =="
  refs_found=0
  for cfg in "$APP_CONFIG" "$MONITOR_CONFIG"; do
    [ -f "$cfg" ] || continue
    for device_file in $(extract_device_files "$cfg"); do
      refs_found=1
      resolved_device_file=$(resolve_config_path "$cfg" "$device_file")
      if [ -f "$resolved_device_file" ]; then
        pass "device config referenced by $(basename "$cfg") exists: $resolved_device_file"
      else
        fail "device config referenced by $(basename "$cfg") missing: $resolved_device_file"
      fi
    done
  done
  if [ "$refs_found" -eq 0 ]; then
    fail "no deviceConfigFiles found in app configs"
  fi
}

check_tls_one() {
  cfg="$1"
  [ -f "$cfg" ] || return
  broker=$(json_string "$cfg" broker)
  tls_enabled=$(tls_bool "$cfg" enabled)
  insecure=$(tls_bool "$cfg" insecureSkipVerify)
  ca_file=$(tls_string "$cfg" caFile)
  cert_file=$(tls_string "$cfg" certFile)
  key_file=$(tls_string "$cfg" keyFile)
  needs_tls=0

  case "$broker" in
    ssl://*|tls://*|mqtts://*) needs_tls=1 ;;
  esac
  [ "$tls_enabled" = "true" ] && needs_tls=1

  if [ "$needs_tls" -eq 1 ]; then
    pass "$(basename "$cfg") TLS enabled for broker: $broker"
    if has_openssl_runtime; then
      pass "OpenSSL runtime libraries available"
    else
      fail "OpenSSL runtime libraries missing while TLS is enabled"
    fi
    if [ "$insecure" = "true" ]; then
      fail "tls.insecureSkipVerify=true is not allowed for production: $cfg"
    else
      pass "tls.insecureSkipVerify is not enabled"
    fi
    if [ -n "$ca_file" ]; then
      if [ -f "$ca_file" ]; then
        pass "TLS CA file exists: $ca_file"
      else
        fail "TLS CA file missing: $ca_file"
      fi
    else
      warn "tls.caFile is empty; relying on system CA store for $(basename "$cfg")"
    fi
    if [ -n "$cert_file" ] || [ -n "$key_file" ]; then
      if [ -n "$cert_file" ] && [ -n "$key_file" ]; then
        [ -f "$cert_file" ] && pass "TLS client certificate exists: $cert_file" || fail "TLS client certificate missing: $cert_file"
        [ -f "$key_file" ] && pass "TLS client key exists: $key_file" || fail "TLS client key missing: $key_file"
      else
        fail "tls.certFile and tls.keyFile must be configured together in $cfg"
      fi
    fi
  else
    warn "$(basename "$cfg") uses non-TLS broker: ${broker:-empty}"
  fi
}

check_tls() {
  echo "== mqtt tls =="
  check_tls_one "$APP_CONFIG"
  check_tls_one "$MONITOR_CONFIG"
}

check_services() {
  echo "== services =="
  if [ ! -x "$BIN_DIR/gateway-services.sh" ]; then
    fail "gateway-services.sh is not executable"
    return
  fi
  "$BIN_DIR/gateway-services.sh" list >/tmp/gateway-smoke-units.$$ 2>/tmp/gateway-smoke-list.err || {
    fail "gateway-services.sh list failed"
    cat /tmp/gateway-smoke-list.err >&2
    rm -f /tmp/gateway-smoke-units.$$ /tmp/gateway-smoke-list.err
    return
  }
  if [ ! -s /tmp/gateway-smoke-units.$$ ]; then
    fail "gateway-services.sh list returned no units"
    rm -f /tmp/gateway-smoke-units.$$ /tmp/gateway-smoke-list.err
    return
  fi
  pass "desired service list generated"
  if command -v systemctl >/dev/null 2>&1; then
    while IFS= read -r unit; do
      [ -z "$unit" ] && continue
      case "$unit" in \#*) continue ;; esac
      if systemctl is-active --quiet "$unit"; then
        pass "service active: $unit"
      else
        fail "service not active: $unit"
      fi
    done < /tmp/gateway-smoke-units.$$
    failed_units=$(systemctl --failed --no-legend 2>/dev/null | grep -E 'modbus|dlt645|mqtt|event-engine|system-monitor|gateway-services' || true)
    if [ -n "$failed_units" ]; then
      fail "gateway related failed systemd units detected"
      echo "$failed_units" >&2
    else
      pass "no gateway related failed systemd units"
    fi
  else
    warn "systemctl not found; skipped service active checks"
  fi
  rm -f /tmp/gateway-smoke-units.$$ /tmp/gateway-smoke-list.err
}

check_shared_memory() {
  echo "== shared memory =="
  if [ -x "$BIN_DIR/pointctl" ]; then
    if run_timeout 10 "$BIN_DIR/pointctl" stats --app-config "$APP_CONFIG" >/tmp/gateway-smoke-stats.$$ 2>/tmp/gateway-smoke-stats.err; then
      pass "pointctl stats completed"
      cat /tmp/gateway-smoke-stats.$$
    else
      fail "pointctl stats failed or timed out"
      cat /tmp/gateway-smoke-stats.err >&2
    fi
    if run_timeout 10 "$BIN_DIR/pointctl" pending-peek --app-config "$APP_CONFIG" >/tmp/gateway-smoke-pending.$$ 2>/tmp/gateway-smoke-pending.err; then
      pass "pointctl pending-peek completed"
      cat /tmp/gateway-smoke-pending.$$
    else
      fail "pointctl pending-peek failed or timed out"
      cat /tmp/gateway-smoke-pending.err >&2
    fi
    rm -f /tmp/gateway-smoke-stats.$$ /tmp/gateway-smoke-stats.err /tmp/gateway-smoke-pending.$$ /tmp/gateway-smoke-pending.err
  else
    fail "pointctl not executable"
  fi
}

check_disk_and_permissions() {
  echo "== disk and permissions =="
  df_line=$(df -P "$GATEWAY_HOME" 2>/dev/null | awk 'NR==2 {print $4 " " $5}')
  if [ -n "$df_line" ]; then
    free_kb=$(echo "$df_line" | awk '{print $1}')
    used_percent=$(echo "$df_line" | awk '{gsub("%","",$2); print $2}')
    free_mb=$((free_kb / 1024))
    if [ "$free_mb" -ge "$MIN_FREE_MB" ]; then
      pass "disk free ${free_mb}MB >= ${MIN_FREE_MB}MB"
    else
      fail "disk free ${free_mb}MB < ${MIN_FREE_MB}MB"
    fi
    if [ "$used_percent" -le "$MAX_DISK_USED_PERCENT" ]; then
      pass "disk used ${used_percent}% <= ${MAX_DISK_USED_PERCENT}%"
    else
      fail "disk used ${used_percent}% > ${MAX_DISK_USED_PERCENT}%"
    fi
  else
    warn "failed to read disk usage for $GATEWAY_HOME"
  fi

  for sensitive in "$APP_CONFIG" "$MONITOR_CONFIG" "$IDENTITY_CONFIG"; do
    [ -f "$sensitive" ] || continue
    if find "$sensitive" -perm -004 -print 2>/dev/null | grep -q .; then
      warn "config file is world-readable, consider chmod 600/640: $sensitive"
    else
      pass "config file is not world-readable: $sensitive"
    fi
  done
}

check_ota() {
  echo "== ota =="
  exec_exists "$BIN_DIR/ota-apply.sh" "ota-apply.sh"
  exec_exists "$BIN_DIR/ota-rollback.sh" "ota-rollback.sh"
  retention=$(json_number "$APP_CONFIG" retentionCount)
  if [ -n "$retention" ]; then
    if [ "$retention" -le 3 ]; then
      pass "ota.retentionCount <= 3: $retention"
    else
      fail "ota.retentionCount should be <= 3, actual: $retention"
    fi
  else
    warn "ota.retentionCount not found in $APP_CONFIG"
  fi
  for dir in "$GATEWAY_HOME/ota/downloads" "$GATEWAY_HOME/ota/staging" "$GATEWAY_HOME/ota/backup" "$GATEWAY_HOME/data"; do
    if [ -d "$dir" ]; then
      pass "runtime directory exists: $dir"
    else
      warn "runtime directory missing, service may create it later: $dir"
    fi
  done
}

check_optional_mqtt_connect() {
  echo "== optional mqtt connect =="
  if [ "$MQTT_CONNECT_TEST" != "1" ]; then
    warn "MQTT_CONNECT_TEST=1 not set; skipped active MQTT connection test"
    return
  fi
  if [ ! -x "$BIN_DIR/MqttDriver" ]; then
    fail "MqttDriver not executable"
    return
  fi
  if run_timeout 8 "$BIN_DIR/MqttDriver" --app-config "$APP_CONFIG" --once >/tmp/gateway-smoke-mqtt.$$ 2>/tmp/gateway-smoke-mqtt.err; then
    pass "MqttDriver --once completed"
    cat /tmp/gateway-smoke-mqtt.$$
  else
    fail "MqttDriver --once failed or timed out"
    cat /tmp/gateway-smoke-mqtt.err >&2
  fi
  rm -f /tmp/gateway-smoke-mqtt.$$ /tmp/gateway-smoke-mqtt.err
}

echo "Gateway production smoke test"
echo "GATEWAY_HOME=$GATEWAY_HOME"
echo "APP_CONFIG=$APP_CONFIG"
echo "MONITOR_CONFIG=$MONITOR_CONFIG"
echo

check_identity
check_runtime_files
check_device_refs
check_tls
check_services
check_shared_memory
check_disk_and_permissions
check_ota
check_optional_mqtt_connect

echo
echo "summary: pass=$PASS_COUNT warn=$WARN_COUNT fail=$FAIL_COUNT"

if [ "$FAIL_COUNT" -gt 0 ]; then
  exit 1
fi
exit 0
