#!/bin/sh
set -u

GATEWAY_HOME="${GATEWAY_HOME:-/opt/modbus-gateway}"
BIN_DIR="${BIN_DIR:-$GATEWAY_HOME/bin}"
APP_CONFIG="${APP_CONFIG:-$GATEWAY_HOME/config/runtime/apps/mqtt-service.json}"
MONITOR_CONFIG="${MONITOR_CONFIG:-$GATEWAY_HOME/config/runtime/apps/monitor-service.json}"
CAMERA_CONFIG="${CAMERA_CONFIG:-$GATEWAY_HOME/config/runtime/apps/camera-service.json}"
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
  for bin in ModbusRtu Dlt645Driver DioDriver CanDriver IecDriver MqttDriver EventEngine SystemMonitor LocalDisplay CameraService pointctl; do
    exec_exists "$BIN_DIR/$bin" "$bin"
  done
  for script in gateway-services.sh gateway-run.sh gateway-tls-enroll.sh production-smoke-test.sh ota-apply.sh ota-rollback.sh; do
    exec_exists "$BIN_DIR/$script" "$script"
  done
}

app_runtime_mode() {
  mode=$(json_string "$1" runtimeMode)
  [ -n "$mode" ] || mode="gateway"
  printf '%s\n' "$mode"
}

app_has_ems_graph_rule() {
  file="$1"
  grep -E '"type"[[:space:]]*:[[:space:]]*"graphEms"|shuntong_ems_graph\.json' "$file" >/dev/null 2>&1
}

check_runtime_mode() {
  echo "== runtime mode =="
  mqtt_mode=$(app_runtime_mode "$APP_CONFIG")
  monitor_mode=$(app_runtime_mode "$MONITOR_CONFIG")
  for item in "$APP_CONFIG:$mqtt_mode" "$MONITOR_CONFIG:$monitor_mode"; do
    cfg=${item%%:*}
    mode=${item#*:}
    case "$mode" in
      gateway|ems) pass "$(basename "$cfg") runtimeMode: $mode" ;;
      *) fail "$(basename "$cfg") runtimeMode must be gateway or ems, actual: $mode" ;;
    esac
  done
  if [ "$mqtt_mode" != "$monitor_mode" ]; then
    fail "mqtt-service and monitor-service runtimeMode mismatch: $mqtt_mode vs $monitor_mode"
    return
  fi
  if [ "$mqtt_mode" = "gateway" ]; then
    for cfg in "$APP_CONFIG" "$MONITOR_CONFIG"; do
      if extract_device_files "$cfg" | grep -q 'device_ems_virtual\.json'; then
        fail "$(basename "$cfg") gateway mode must not reference device_ems_virtual.json"
      else
        pass "$(basename "$cfg") gateway mode has no EMS virtual device reference"
      fi
      if app_has_ems_graph_rule "$cfg"; then
        fail "$(basename "$cfg") gateway mode must not include graphEms rules"
      else
        pass "$(basename "$cfg") gateway mode has no graphEms rules"
      fi
    done
  fi
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
  check_stunnel_brokers
}

report_check_lines() {
  file="$1"
  while IFS= read -r line; do
    level=${line%%:*}
    message=${line#*:}
    case "$level" in
      PASS) pass "$message" ;;
      WARN) warn "$message" ;;
      FAIL) fail "$message" ;;
    esac
  done < "$file"
}

check_stunnel_brokers() {
  tmp="/tmp/gateway-smoke-stunnel.$$"
  if ! command -v python3 >/dev/null 2>&1; then
    fail "python3 missing; cannot validate local MQTT stunnel config"
    return
  fi
  if python3 - "$GATEWAY_HOME" "$APP_CONFIG" "$MONITOR_CONFIG" "$CAMERA_CONFIG" > "$tmp" <<'PY'
import glob
import json
import os
import sys
from urllib.parse import urlparse

gateway_home = sys.argv[1]
app_paths = []
for path in sys.argv[2:]:
    if path and path not in app_paths and os.path.isfile(path):
        app_paths.append(path)

def emit(level, message):
    print(f"{level}:{message}")

def read_json(path):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except Exception as exc:
        emit("FAIL", f"invalid app config {path}: {exc}")
        return None

def bool_value(value, default=False):
    if isinstance(value, bool):
        return value
    if value is None:
        return default
    return str(value).strip().lower() in ("1", "true", "yes", "on")

def normalize_host(host):
    text = str(host or "").strip().strip("[]").lower()
    if text in ("localhost", "::1", "0:0:0:0:0:0:0:1"):
        return "127.0.0.1"
    return text

def is_loopback_host(host):
    host = normalize_host(host)
    return host == "127.0.0.1" or host.startswith("127.")

def parse_host_port(raw, default_host=""):
    text = str(raw or "").strip()
    if not text:
        return "", None
    if text.startswith("[") and "]" in text:
        host, _, rest = text[1:].partition("]")
        port_text = rest[1:] if rest.startswith(":") else rest
    elif ":" in text:
        host, port_text = text.rsplit(":", 1)
    else:
        host, port_text = default_host, text
    try:
        port = int(str(port_text).strip())
    except Exception:
        return normalize_host(host), None
    return normalize_host(host), port

def parse_broker_endpoint(broker):
    text = str(broker or "").strip()
    if not text:
        return "", None
    if "://" in text:
        try:
            parsed = urlparse(text)
            return normalize_host(parsed.hostname or ""), parsed.port
        except Exception:
            return "", None
    return parse_host_port(text)

def stunnel_values(path):
    values = {}
    try:
        with open(path, "r", encoding="utf-8") as fh:
            for raw_line in fh:
                line = raw_line.split("#", 1)[0].split(";", 1)[0].strip()
                if "=" not in line:
                    continue
                key, value = line.split("=", 1)
                values[key.strip().lower()] = value.strip()
    except Exception as exc:
        emit("FAIL", f"invalid stunnel config {path}: {exc}")
    return values

def accept_matches(accept_host, broker_host):
    accept_host = normalize_host(accept_host)
    broker_host = normalize_host(broker_host)
    if accept_host in ("", "0.0.0.0", "::", "*"):
        return is_loopback_host(broker_host)
    return accept_host == broker_host

def is_placeholder(value):
    text = str(value or "").strip().lower()
    return "your-" in text or "example.com" in text or text in ("broker.example.com:8883", "placeholder")

def find_matching_stunnel_conf(app_path, broker_host, broker_port):
    tls_dir = os.path.join(gateway_home, "config", "runtime", "tls")
    preferred = os.path.join(tls_dir, os.path.splitext(os.path.basename(app_path))[0] + "-stunnel.conf")
    candidates = []
    for candidate in [preferred] + sorted(glob.glob(os.path.join(tls_dir, "*-stunnel.conf"))):
        if candidate not in candidates:
            candidates.append(candidate)
    for candidate in candidates:
        if not os.path.isfile(candidate):
            continue
        values = stunnel_values(candidate)
        accept_host, accept_port = parse_host_port(values.get("accept", ""), "0.0.0.0")
        if accept_port == broker_port and accept_matches(accept_host, broker_host):
            return candidate, values
    return "", {}

def validate_conf(app_path, broker, broker_host, broker_port, conf, values):
    label = os.path.basename(app_path)
    emit("PASS", f"{label} uses local MQTT stunnel broker: {broker}")
    emit("PASS", f"matching stunnel config found: {conf}")

    connect = values.get("connect", "")
    if not connect:
        emit("FAIL", f"stunnel connect is empty: {conf}")
    elif is_placeholder(connect):
        emit("FAIL", f"stunnel connect still uses template placeholder: {conf}")
    else:
        emit("PASS", f"stunnel connect configured: {connect}")

    check_host = values.get("checkhost", "")
    if check_host:
        if is_placeholder(check_host):
            emit("FAIL", f"stunnel checkHost still uses template placeholder: {conf}")
        else:
            emit("PASS", f"stunnel checkHost configured: {check_host}")
    else:
        emit("WARN", f"stunnel checkHost is empty: {conf}")

    verify_chain = str(values.get("verifychain", "")).strip().lower()
    if verify_chain in ("no", "false", "0"):
        emit("FAIL", f"stunnel verifyChain is disabled: {conf}")
    elif verify_chain:
        emit("PASS", f"stunnel verifyChain configured: {values.get('verifychain')}")
    else:
        emit("WARN", f"stunnel verifyChain not configured: {conf}")

    ca_file = values.get("cafile", "")
    if ca_file:
        if os.path.isfile(ca_file):
            emit("PASS", f"stunnel CAfile exists: {ca_file}")
        else:
            emit("FAIL", f"stunnel CAfile missing: {ca_file}")
    else:
        emit("WARN", f"stunnel CAfile is empty: {conf}")

for app_path in app_paths:
    root = read_json(app_path)
    if root is None:
        continue
    mqtt = root.get("mqtt") or {}
    if not isinstance(mqtt, dict) or bool_value(mqtt.get("enabled"), True) is False:
        continue
    broker = str(mqtt.get("broker") or "").strip()
    broker_host, broker_port = parse_broker_endpoint(broker)
    if not is_loopback_host(broker_host) or broker_port is None:
        continue
    conf, values = find_matching_stunnel_conf(app_path, broker_host, broker_port)
    if conf:
        validate_conf(app_path, broker, broker_host, broker_port, conf, values)
    elif broker_port != 1883:
        emit("FAIL", f"{os.path.basename(app_path)} broker points to local port {broker_port} but no matching stunnel config was found: {broker}")
PY
  then
    report_check_lines "$tmp"
  else
    fail "local MQTT stunnel validation failed"
  fi
  rm -f "$tmp"
}

check_services() {
  echo "== services =="
  if [ -e /dev/watchdog ] || [ -e /dev/watchdog0 ]; then
    if command -v systemctl >/dev/null 2>&1; then
      runtime_watchdog=$(systemctl show --property=RuntimeWatchdogUSec --value 2>/dev/null || echo 0)
      case "$runtime_watchdog" in
        0|"") warn "hardware watchdog device exists but systemd RuntimeWatchdogSec is not active" ;;
        *) pass "systemd runtime watchdog active: RuntimeWatchdogUSec=$runtime_watchdog" ;;
      esac
    else
      warn "hardware watchdog device exists; skipped systemd runtime watchdog check"
    fi
  else
    warn "hardware watchdog device not found"
  fi
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
  if grep -E '^mqtt-tls-tunnel@[^[:space:]]+\.service$' /tmp/gateway-smoke-units.$$ >/dev/null; then
    if command -v stunnel >/dev/null 2>&1 || [ -x /usr/bin/stunnel ]; then
      pass "stunnel executable available for mqtt-tls-tunnel"
    else
      fail "stunnel executable missing while mqtt-tls-tunnel is desired"
    fi
    if [ -f /etc/systemd/system/mqtt-tls-tunnel@.service ]; then
      pass "mqtt-tls-tunnel systemd template installed"
    elif command -v systemctl >/dev/null 2>&1; then
      fail "mqtt-tls-tunnel systemd template missing: /etc/systemd/system/mqtt-tls-tunnel@.service"
    else
      warn "systemctl not found; skipped mqtt-tls-tunnel systemd template check"
    fi
    while IFS= read -r unit; do
      case "$unit" in
        mqtt-tls-tunnel@*.service)
          instance=${unit#mqtt-tls-tunnel@}
          instance=${instance%.service}
          conf="$GATEWAY_HOME/config/runtime/tls/$instance.conf"
          if [ -f "$conf" ]; then
            pass "stunnel config exists: $conf"
          else
            fail "stunnel config missing: $conf"
          fi
          ;;
      esac
    done < /tmp/gateway-smoke-units.$$
  fi
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
    active_units_file=/tmp/gateway-smoke-active.$$
    enabled_units_file=/tmp/gateway-smoke-enabled.$$
    systemctl list-units --plain --no-legend --state=active \
      'modbus-rtu@*.service' \
      'dlt645-driver@*.service' \
      'dio-driver@*.service' \
      'can-driver@*.service' \
      'compute-engine@*.service' \
      'event-engine@*.service' \
      'local-display@*.service' \
      'local-display-qt@*.service' \
      'local-kiosk@*.service' \
      'camera-service@*.service' \
      'mqtt-driver@*.service' \
      'system-monitor@*.service' \
      'mqtt-tls-tunnel@*.service' 2>/dev/null |
      awk '{print $1}' > "$active_units_file"
    systemctl list-unit-files --plain --no-legend \
      'modbus-rtu@*.service' \
      'dlt645-driver@*.service' \
      'dio-driver@*.service' \
      'can-driver@*.service' \
      'compute-engine@*.service' \
      'event-engine@*.service' \
      'local-display@*.service' \
      'local-display-qt@*.service' \
      'local-kiosk@*.service' \
      'camera-service@*.service' \
      'mqtt-driver@*.service' \
      'system-monitor@*.service' \
      'mqtt-tls-tunnel@*.service' 2>/dev/null |
      awk '$2 ~ /^enabled/ {print $1}' > "$enabled_units_file"
    unexpected_active=$(awk 'NR==FNR {desired[$1]=1; next} !($1 in desired) {print $1}' /tmp/gateway-smoke-units.$$ "$active_units_file" | tr '\n' ' ')
    unexpected_enabled=$(awk 'NR==FNR {desired[$1]=1; next} !($1 in desired) {print $1}' /tmp/gateway-smoke-units.$$ "$enabled_units_file" | tr '\n' ' ')
    if [ -n "$unexpected_active" ]; then
      fail "unexpected active gateway units: $unexpected_active"
    else
      pass "no unexpected active gateway units"
    fi
    if [ -n "$unexpected_enabled" ]; then
      fail "unexpected enabled gateway units: $unexpected_enabled"
    else
      pass "no unexpected enabled gateway units"
    fi
    rm -f "$active_units_file" "$enabled_units_file"
    failed_units=$(systemctl --failed --no-legend 2>/dev/null | grep -E 'modbus|dlt645|dio|can-driver|mqtt|event-engine|system-monitor|local-display|camera-service|gateway-services' || true)
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
check_runtime_mode
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
