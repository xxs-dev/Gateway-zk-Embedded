#!/bin/sh
set -u

GATEWAY_HOME="${GATEWAY_HOME:-/opt/modbus-gateway}"
GATEWAY_SERVICES_SCRIPT="${GATEWAY_SERVICES_SCRIPT:-$GATEWAY_HOME/bin/gateway-services.sh}"
SYSTEMCTL_BIN="${WATCHDOG_SYSTEMCTL_BIN:-systemctl}"
STATE_DIR="${WATCHDOG_STATE_DIR:-/var/lib/modbus-gateway/watchdog}"
RUN_DIR="${WATCHDOG_RUN_DIR:-/run/gateway-health-watchdog}"
STATUS_FILE="${WATCHDOG_STATUS_FILE:-$STATE_DIR/status.json}"

CHECK_INTERVAL_SEC="${WATCHDOG_CHECK_INTERVAL_SEC:-10}"
STARTUP_GRACE_SEC="${WATCHDOG_STARTUP_GRACE_SEC:-60}"
FAILURE_THRESHOLD="${WATCHDOG_FAILURE_THRESHOLD:-3}"
RECOVERY_VERIFY_SEC="${WATCHDOG_RECOVERY_VERIFY_SEC:-30}"
RECOVERY_COOLDOWN_SEC="${WATCHDOG_RECOVERY_COOLDOWN_SEC:-120}"
REBOOT_FAILURE_THRESHOLD="${WATCHDOG_REBOOT_FAILURE_THRESHOLD:-3}"
RECOVERY_WINDOW_SEC="${WATCHDOG_RECOVERY_WINDOW_SEC:-900}"
REBOOT_COOLDOWN_SEC="${WATCHDOG_REBOOT_COOLDOWN_SEC:-1800}"
REBOOT_RATE_WINDOW_SEC="${WATCHDOG_REBOOT_RATE_WINDOW_SEC:-86400}"
MAX_REBOOTS_PER_WINDOW="${WATCHDOG_MAX_REBOOTS_PER_WINDOW:-2}"

MANUAL_STOP_FILE="$RUN_DIR/manual-stop"
APPLYING_FILE="$RUN_DIR/applying"
LOCK_FILE="$RUN_DIR/recovery.lock"
LOCK_DIR="$RUN_DIR/recovery.lock.d"
REBOOT_HISTORY_FILE="$STATE_DIR/reboot-history"

MISSING_UNITS=""
CRITICAL_MISSING_UNITS=""
HEALTH_REASON=""
LOCK_STYLE=""

log() {
  echo "[gateway-health-watchdog] $*"
}

now_epoch() {
  if [ -n "${WATCHDOG_NOW_EPOCH:-}" ]; then
    printf '%s\n' "$WATCHDOG_NOW_EPOCH"
  else
    date +%s
  fi
}

is_uint() {
  case "${1:-}" in
    ''|*[!0-9]*) return 1 ;;
    *) return 0 ;;
  esac
}

positive_or_default() {
  value="$1"
  fallback="$2"
  if is_uint "$value" && [ "$value" -gt 0 ]; then
    printf '%s\n' "$value"
  else
    printf '%s\n' "$fallback"
  fi
}

nonnegative_or_default() {
  value="$1"
  fallback="$2"
  if is_uint "$value"; then
    printf '%s\n' "$value"
  else
    printf '%s\n' "$fallback"
  fi
}

CHECK_INTERVAL_SEC=$(positive_or_default "$CHECK_INTERVAL_SEC" 10)
STARTUP_GRACE_SEC=$(nonnegative_or_default "$STARTUP_GRACE_SEC" 60)
FAILURE_THRESHOLD=$(positive_or_default "$FAILURE_THRESHOLD" 3)
RECOVERY_VERIFY_SEC=$(nonnegative_or_default "$RECOVERY_VERIFY_SEC" 30)
RECOVERY_COOLDOWN_SEC=$(nonnegative_or_default "$RECOVERY_COOLDOWN_SEC" 120)
REBOOT_FAILURE_THRESHOLD=$(positive_or_default "$REBOOT_FAILURE_THRESHOLD" 3)
RECOVERY_WINDOW_SEC=$(positive_or_default "$RECOVERY_WINDOW_SEC" 900)
REBOOT_COOLDOWN_SEC=$(positive_or_default "$REBOOT_COOLDOWN_SEC" 1800)
REBOOT_RATE_WINDOW_SEC=$(positive_or_default "$REBOOT_RATE_WINDOW_SEC" 86400)
MAX_REBOOTS_PER_WINDOW=$(positive_or_default "$MAX_REBOOTS_PER_WINDOW" 2)

ensure_dirs() {
  mkdir -p "$STATE_DIR" "$RUN_DIR"
}

read_uint_file() {
  file="$1"
  fallback="$2"
  value=""
  [ -f "$file" ] && value=$(sed -n '1p' "$file" 2>/dev/null || true)
  if is_uint "$value"; then
    printf '%s\n' "$value"
  else
    printf '%s\n' "$fallback"
  fi
}

write_value() {
  file="$1"
  value="$2"
  tmp="$file.tmp.$$"
  printf '%s\n' "$value" > "$tmp"
  mv -f "$tmp" "$file"
}

boot_id() {
  if [ -n "${WATCHDOG_BOOT_ID:-}" ]; then
    printf '%s\n' "$WATCHDOG_BOOT_ID"
  elif [ -r /proc/sys/kernel/random/boot_id ]; then
    sed -n '1p' /proc/sys/kernel/random/boot_id
  else
    printf '%s\n' unknown
  fi
}

initialize_boot_state() {
  current_boot=$(boot_id)
  saved_boot=""
  [ -f "$STATE_DIR/boot-id" ] && saved_boot=$(sed -n '1p' "$STATE_DIR/boot-id" 2>/dev/null || true)
  if [ "$current_boot" != "$saved_boot" ]; then
    write_value "$STATE_DIR/boot-id" "$current_boot"
    write_value "$STATE_DIR/boot-started-at" "$(now_epoch)"
    write_value "$STATE_DIR/consecutive-failures" 0
    write_value "$STATE_DIR/recovery-failures" 0
    write_value "$STATE_DIR/recovery-window-started-at" 0
    write_value "$STATE_DIR/stable-passes" 0
    write_value "$STATE_DIR/cooldown-until" 0
  elif [ ! -f "$STATE_DIR/boot-started-at" ]; then
    write_value "$STATE_DIR/boot-started-at" "$(now_epoch)"
  fi
}

json_escape() {
  printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

write_status() {
  status="$1"
  action="$2"
  now=$(now_epoch)
  consecutive=$(read_uint_file "$STATE_DIR/consecutive-failures" 0)
  recoveries=$(read_uint_file "$STATE_DIR/recovery-failures" 0)
  cooldown=$(read_uint_file "$STATE_DIR/cooldown-until" 0)
  missing=$(json_escape "$MISSING_UNITS")
  critical=$(json_escape "$CRITICAL_MISSING_UNITS")
  reason=$(json_escape "$HEALTH_REASON")
  tmp="$STATUS_FILE.tmp.$$"
  cat > "$tmp" <<EOF
{"checkedAtEpoch":$now,"status":"$status","lastAction":"$action","reason":"$reason","consecutiveFailures":$consecutive,"recoveryFailures":$recoveries,"cooldownUntilEpoch":$cooldown,"missingUnits":"$missing","criticalMissingUnits":"$critical"}
EOF
  mv -f "$tmp" "$STATUS_FILE"
}

append_word() {
  current="$1"
  word="$2"
  if [ -n "$current" ]; then
    printf '%s %s\n' "$current" "$word"
  else
    printf '%s\n' "$word"
  fi
}

is_critical_unit() {
  case "$1" in
    gateway-services.service|system-monitor@*.service|mqtt-driver@*.service) return 0 ;;
    *) return 1 ;;
  esac
}

gateway_transitioning() {
  state=$($SYSTEMCTL_BIN show gateway-services.service --property=ActiveState --value 2>/dev/null || true)
  case "$state" in
    activating|deactivating|reloading) return 0 ;;
    *) return 1 ;;
  esac
}

check_health() {
  MISSING_UNITS=""
  CRITICAL_MISSING_UNITS=""
  HEALTH_REASON=""

  list_file="$RUN_DIR/desired-units.$$"
  error_file="$RUN_DIR/desired-units-error.$$"
  if ! "$GATEWAY_SERVICES_SCRIPT" list > "$list_file" 2> "$error_file"; then
    HEALTH_REASON="config-list-failed"
    MISSING_UNITS="gateway-services-list"
    rm -f "$list_file" "$error_file"
    return 1
  fi

  desired_count=0
  if ! $SYSTEMCTL_BIN is-active --quiet gateway-services.service >/dev/null 2>&1; then
    MISSING_UNITS=$(append_word "$MISSING_UNITS" gateway-services.service)
    CRITICAL_MISSING_UNITS=$(append_word "$CRITICAL_MISSING_UNITS" gateway-services.service)
  fi

  while IFS= read -r unit; do
    [ -n "$unit" ] || continue
    case "$unit" in
      \#*) continue ;;
      *[!A-Za-z0-9@_.:-]*)
        HEALTH_REASON="invalid-unit-name"
        MISSING_UNITS=$(append_word "$MISSING_UNITS" "$unit")
        continue
        ;;
    esac
    desired_count=$((desired_count + 1))
    if ! $SYSTEMCTL_BIN is-active --quiet "$unit" >/dev/null 2>&1; then
      MISSING_UNITS=$(append_word "$MISSING_UNITS" "$unit")
      if is_critical_unit "$unit"; then
        CRITICAL_MISSING_UNITS=$(append_word "$CRITICAL_MISSING_UNITS" "$unit")
      fi
    fi
  done < "$list_file"
  rm -f "$list_file" "$error_file"

  if [ "$desired_count" -eq 0 ]; then
    HEALTH_REASON="no-desired-units"
    MISSING_UNITS=$(append_word "$MISSING_UNITS" gateway-services-list-empty)
  fi
  [ -n "$MISSING_UNITS" ] || return 0
  [ -n "$HEALTH_REASON" ] || HEALTH_REASON="inactive-units"
  return 1
}

sleep_seconds() {
  seconds="$1"
  [ "$seconds" -le 0 ] || sleep "$seconds"
}

acquire_lock() {
  if command -v flock >/dev/null 2>&1; then
    eval "exec 9>\"$LOCK_FILE\""
    if flock -n 9; then
      LOCK_STYLE="flock"
      return 0
    fi
    return 1
  fi
  if mkdir "$LOCK_DIR" 2>/dev/null; then
    LOCK_STYLE="mkdir"
    return 0
  fi
  return 1
}

release_lock() {
  if [ "$LOCK_STYLE" = "flock" ]; then
    flock -u 9 >/dev/null 2>&1 || true
  elif [ "$LOCK_STYLE" = "mkdir" ]; then
    rmdir "$LOCK_DIR" >/dev/null 2>&1 || true
  fi
  LOCK_STYLE=""
}

prune_reboot_history() {
  now="$1"
  cutoff=$((now - REBOOT_RATE_WINDOW_SEC))
  tmp="$REBOOT_HISTORY_FILE.tmp.$$"
  : > "$tmp"
  if [ -f "$REBOOT_HISTORY_FILE" ]; then
    while IFS= read -r value; do
      if is_uint "$value" && [ "$value" -ge "$cutoff" ]; then
        printf '%s\n' "$value" >> "$tmp"
      fi
    done < "$REBOOT_HISTORY_FILE"
  fi
  mv -f "$tmp" "$REBOOT_HISTORY_FILE"
}

request_reboot_if_allowed() {
  now=$(now_epoch)
  last_reboot=$(read_uint_file "$STATE_DIR/last-reboot-requested-at" 0)
  if [ "$last_reboot" -gt 0 ] && [ $((now - last_reboot)) -lt "$REBOOT_COOLDOWN_SEC" ]; then
    log "whole-device reboot suppressed by cooldown"
    write_status degraded reboot-suppressed-cooldown
    return 1
  fi

  prune_reboot_history "$now"
  reboot_count=$(wc -l < "$REBOOT_HISTORY_FILE" | tr -d ' ')
  reboot_count=$(nonnegative_or_default "$reboot_count" 0)
  if [ "$reboot_count" -ge "$MAX_REBOOTS_PER_WINDOW" ]; then
    log "whole-device reboot suppressed by rate limit"
    write_status degraded reboot-suppressed-rate-limit
    return 1
  fi

  printf '%s\n' "$now" >> "$REBOOT_HISTORY_FILE"
  write_value "$STATE_DIR/last-reboot-requested-at" "$now"
  log "critical maintenance services did not recover; requesting controlled reboot"
  write_status rebooting reboot-requested
  $SYSTEMCTL_BIN reboot
}

record_failed_recovery() {
  allow_reboot="$1"
  now=$(now_epoch)
  window_start=$(read_uint_file "$STATE_DIR/recovery-window-started-at" 0)
  recovery_failures=$(read_uint_file "$STATE_DIR/recovery-failures" 0)
  if [ "$window_start" -eq 0 ] || [ $((now - window_start)) -gt "$RECOVERY_WINDOW_SEC" ]; then
    window_start="$now"
    recovery_failures=0
  fi
  recovery_failures=$((recovery_failures + 1))
  write_value "$STATE_DIR/recovery-window-started-at" "$window_start"
  write_value "$STATE_DIR/recovery-failures" "$recovery_failures"
  if [ "$allow_reboot" = "1" ] &&
     [ "$recovery_failures" -ge "$REBOOT_FAILURE_THRESHOLD" ] &&
     [ -n "$CRITICAL_MISSING_UNITS" ] &&
     [ "$HEALTH_REASON" != "config-list-failed" ] &&
     [ "$HEALTH_REASON" != "no-desired-units" ] &&
     [ "$HEALTH_REASON" != "invalid-unit-name" ]; then
    request_reboot_if_allowed || true
  else
    write_status degraded gateway-restart-failed
  fi
}

wait_for_health() {
  started=$(now_epoch)
  deadline=$((started + RECOVERY_VERIFY_SEC))
  while :; do
    if check_health; then
      return 0
    fi
    now=$(now_epoch)
    [ "$now" -lt "$deadline" ] || return 1
    remaining=$((deadline - now))
    if [ "$remaining" -gt 2 ]; then
      sleep_seconds 2
    else
      sleep_seconds "$remaining"
    fi
  done
}

recover_gateway() {
  if ! acquire_lock; then
    log "another recovery is already running"
    write_status recovering recovery-already-running
    return 0
  fi

  recovery_kind=gateway
  recovery_action=restart-gateway-services
  success_action=gateway-restarted
  restart_ok=1
  if [ -z "$CRITICAL_MISSING_UNITS" ] && [ "$HEALTH_REASON" = "inactive-units" ]; then
    recovery_kind=units
    recovery_action=restart-inactive-units
    success_action=units-restarted
    log "restarting inactive gateway units after repeated failures: $MISSING_UNITS"
    write_status recovering "$recovery_action"
    for unit in $MISSING_UNITS; do
      if ! $SYSTEMCTL_BIN restart "$unit"; then
        restart_ok=0
      fi
    done
  else
    log "restarting gateway services after repeated critical failures: $MISSING_UNITS"
    write_status recovering "$recovery_action"
    if ! $SYSTEMCTL_BIN restart gateway-services.service; then
      restart_ok=0
    fi
  fi

  if [ "$restart_ok" -eq 1 ] && wait_for_health; then
    now=$(now_epoch)
    write_value "$STATE_DIR/consecutive-failures" 0
    write_value "$STATE_DIR/recovery-failures" 0
    write_value "$STATE_DIR/recovery-window-started-at" 0
    write_value "$STATE_DIR/stable-passes" 0
    write_value "$STATE_DIR/cooldown-until" $((now + RECOVERY_COOLDOWN_SEC))
    HEALTH_REASON="recovered"
    log "gateway recovery completed: $success_action"
    write_status healthy "$success_action"
  else
    now=$(now_epoch)
    log "gateway remains unhealthy after $recovery_kind recovery: $MISSING_UNITS"
    write_value "$STATE_DIR/cooldown-until" $((now + RECOVERY_COOLDOWN_SEC))
    if [ "$recovery_kind" = "gateway" ]; then
      record_failed_recovery 1
    else
      record_failed_recovery 0
    fi
  fi
  release_lock
}

check_once() {
  ensure_dirs
  initialize_boot_state
  now=$(now_epoch)
  boot_started=$(read_uint_file "$STATE_DIR/boot-started-at" "$now")

  if [ -f "$MANUAL_STOP_FILE" ]; then
    write_value "$STATE_DIR/consecutive-failures" 0
    HEALTH_REASON="manual-stop"
    MISSING_UNITS=""
    CRITICAL_MISSING_UNITS=""
    write_status suspended manual-stop
    return 0
  fi
  if [ -f "$APPLYING_FILE" ] || gateway_transitioning; then
    write_value "$STATE_DIR/consecutive-failures" 0
    HEALTH_REASON="configuration-applying"
    MISSING_UNITS=""
    CRITICAL_MISSING_UNITS=""
    write_status suspended configuration-applying
    return 0
  fi
  if [ $((now - boot_started)) -lt "$STARTUP_GRACE_SEC" ]; then
    HEALTH_REASON="startup-grace"
    MISSING_UNITS=""
    CRITICAL_MISSING_UNITS=""
    write_status starting startup-grace
    return 0
  fi

  cooldown_until=$(read_uint_file "$STATE_DIR/cooldown-until" 0)
  if check_health; then
    write_value "$STATE_DIR/consecutive-failures" 0
    stable=$(read_uint_file "$STATE_DIR/stable-passes" 0)
    stable=$((stable + 1))
    write_value "$STATE_DIR/stable-passes" "$stable"
    if [ "$stable" -ge 3 ]; then
      write_value "$STATE_DIR/recovery-failures" 0
      write_value "$STATE_DIR/recovery-window-started-at" 0
    fi
    HEALTH_REASON="healthy"
    write_status healthy none
    return 0
  fi

  write_value "$STATE_DIR/stable-passes" 0
  failures=$(read_uint_file "$STATE_DIR/consecutive-failures" 0)
  failures=$((failures + 1))
  write_value "$STATE_DIR/consecutive-failures" "$failures"
  log "health check failed ($failures/$FAILURE_THRESHOLD): $MISSING_UNITS"

  if [ "$now" -lt "$cooldown_until" ]; then
    write_status degraded recovery-cooldown
    return 1
  fi
  if [ "$failures" -lt "$FAILURE_THRESHOLD" ]; then
    write_status degraded waiting-for-threshold
    return 1
  fi
  recover_gateway
}

show_status() {
  if [ -f "$STATUS_FILE" ]; then
    cat "$STATUS_FILE"
  else
    echo '{"status":"unknown","reason":"watchdog-has-not-run"}'
  fi
}

case "${1:-run}" in
  run)
    ensure_dirs
    initialize_boot_state
    write_value "$STATE_DIR/consecutive-failures" 0
    write_value "$STATE_DIR/stable-passes" 0
    log "started; interval=${CHECK_INTERVAL_SEC}s threshold=$FAILURE_THRESHOLD"
    while :; do
      check_once || true
      sleep_seconds "$CHECK_INTERVAL_SEC"
    done
    ;;
  check)
    check_once
    ;;
  status)
    ensure_dirs
    show_status
    ;;
  *)
    echo "Usage: gateway-health-watchdog.sh [run|check|status]" >&2
    exit 2
    ;;
esac
