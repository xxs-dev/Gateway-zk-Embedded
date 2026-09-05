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
APPLYING_STALE_SEC="${WATCHDOG_APPLYING_STALE_SEC:-900}"
DISABLE_FLOCK="${WATCHDOG_DISABLE_FLOCK:-0}"

MANUAL_STOP_FILE="$RUN_DIR/manual-stop"
APPLYING_FILE="$RUN_DIR/applying"
LOCK_FILE="$RUN_DIR/recovery.lock"
LOCK_DIR="$RUN_DIR/recovery.lock.d"
REBOOT_HISTORY_FILE="$STATE_DIR/reboot-history"
DESIRED_UNITS_FILE="$RUN_DIR/desired-units.$$"
DESIRED_UNITS_ERROR_FILE="$RUN_DIR/desired-units-error.$$"

MISSING_UNITS=""
CRITICAL_MISSING_UNITS=""
HEALTH_REASON=""
LOCK_STYLE=""

cleanup_runtime_artifacts() {
  rm -f "$DESIRED_UNITS_FILE" "$DESIRED_UNITS_ERROR_FILE" 2>/dev/null || true
  release_lock
}

terminate() {
  cleanup_runtime_artifacts
  trap - EXIT HUP INT TERM
  exit 143
}

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

now_monotonic() {
  if [ -n "${WATCHDOG_NOW_MONOTONIC:-}" ]; then
    printf '%s\n' "$WATCHDOG_NOW_MONOTONIC"
  elif [ -r /proc/uptime ]; then
    sed -n '1{s/\..*//;p;}' /proc/uptime
  else
    now_epoch
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
APPLYING_STALE_SEC=$(positive_or_default "$APPLYING_STALE_SEC" 900)

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
    write_value "$STATE_DIR/boot-started-monotonic" "$(now_monotonic)"
    write_value "$STATE_DIR/consecutive-failures" 0
    write_value "$STATE_DIR/recovery-failures" 0
    write_value "$STATE_DIR/recovery-window-started-at" 0
    write_value "$STATE_DIR/stable-passes" 0
    write_value "$STATE_DIR/cooldown-until" 0
    write_value "$STATE_DIR/cooldown-until-monotonic" 0
  elif [ ! -f "$STATE_DIR/boot-started-at" ]; then
    write_value "$STATE_DIR/boot-started-at" "$(now_epoch)"
    write_value "$STATE_DIR/boot-started-monotonic" "$(now_monotonic)"
  elif [ ! -f "$STATE_DIR/boot-started-monotonic" ]; then
    write_value "$STATE_DIR/boot-started-monotonic" "$(now_monotonic)"
  fi
}

marker_field() {
  file="$1"
  key="$2"
  sed -n "s/^${key}=//p" "$file" 2>/dev/null | sed -n '1p'
}

applying_marker_active() {
  [ -f "$APPLYING_FILE" ] || return 1

  marker_boot=$(marker_field "$APPLYING_FILE" boot_id)
  marker_pid=$(marker_field "$APPLYING_FILE" pid)
  marker_started=$(marker_field "$APPLYING_FILE" created_uptime_sec)
  current_boot=$(boot_id)
  current_uptime=$(now_monotonic)
  stale=0

  if [ -n "$marker_boot" ] && [ "$marker_boot" != "$current_boot" ]; then
    stale=1
  elif is_uint "$marker_pid" && ! kill -0 "$marker_pid" 2>/dev/null; then
    stale=1
  elif is_uint "$marker_started"; then
    if [ "$current_uptime" -lt "$marker_started" ] ||
       [ $((current_uptime - marker_started)) -gt "$APPLYING_STALE_SEC" ]; then
      stale=1
    fi
  else
    marker_mtime=$(stat -c %Y "$APPLYING_FILE" 2>/dev/null || printf '0')
    current_epoch=$(now_epoch)
    if is_uint "$marker_mtime" && [ "$marker_mtime" -gt 0 ] &&
       [ "$current_epoch" -ge "$marker_mtime" ] &&
       [ $((current_epoch - marker_mtime)) -gt "$APPLYING_STALE_SEC" ]; then
      stale=1
    fi
  fi

  if [ "$stale" -eq 1 ]; then
    log "removing stale configuration apply marker"
    rm -f "$APPLYING_FILE"
    return 1
  fi
  return 0
}

recovery_suppressed() {
  if [ -f "$MANUAL_STOP_FILE" ]; then
    HEALTH_REASON="manual-stop"
    return 0
  fi
  if applying_marker_active || gateway_transitioning; then
    HEALTH_REASON="configuration-applying"
    return 0
  fi
  return 1
}

write_suspended_status() {
  write_value "$STATE_DIR/consecutive-failures" 0
  MISSING_UNITS=""
  CRITICAL_MISSING_UNITS=""
  case "$HEALTH_REASON" in
    manual-stop) write_status suspended manual-stop ;;
    *) write_status suspended configuration-applying ;;
  esac
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

  list_file="$DESIRED_UNITS_FILE"
  error_file="$DESIRED_UNITS_ERROR_FILE"
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
      gateway-cellular.service|gateway-network-failover.service) continue ;;
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
  if [ "$DISABLE_FLOCK" != "1" ] && command -v flock >/dev/null 2>&1; then
    eval "exec 9>\"$LOCK_FILE\""
    if flock -n 9; then
      LOCK_STYLE="flock"
      return 0
    fi
    return 1
  fi
  if mkdir "$LOCK_DIR" 2>/dev/null; then
    printf '%s\n' "$$" > "$LOCK_DIR/pid"
    LOCK_STYLE="mkdir"
    return 0
  fi
  lock_pid=$(sed -n '1p' "$LOCK_DIR/pid" 2>/dev/null || true)
  if ! is_uint "$lock_pid" || ! kill -0 "$lock_pid" 2>/dev/null; then
    rm -f "$LOCK_DIR/pid" 2>/dev/null || true
    rmdir "$LOCK_DIR" 2>/dev/null || true
    if mkdir "$LOCK_DIR" 2>/dev/null; then
      printf '%s\n' "$$" > "$LOCK_DIR/pid"
      LOCK_STYLE="mkdir"
      return 0
    fi
  fi
  return 1
}

release_lock() {
  if [ "$LOCK_STYLE" = "flock" ]; then
    flock -u 9 >/dev/null 2>&1 || true
  elif [ "$LOCK_STYLE" = "mkdir" ]; then
    rm -f "$LOCK_DIR/pid" >/dev/null 2>&1 || true
    rmdir "$LOCK_DIR" >/dev/null 2>&1 || true
  fi
  LOCK_STYLE=""
}

trap cleanup_runtime_artifacts EXIT
trap terminate HUP INT TERM

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
  now=$(now_monotonic)
  window_start=$(read_uint_file "$STATE_DIR/recovery-window-started-monotonic" 0)
  recovery_failures=$(read_uint_file "$STATE_DIR/recovery-failures" 0)
  if [ "$window_start" -eq 0 ] || [ $((now - window_start)) -gt "$RECOVERY_WINDOW_SEC" ]; then
    window_start="$now"
    recovery_failures=0
  fi
  recovery_failures=$((recovery_failures + 1))
  write_value "$STATE_DIR/recovery-window-started-monotonic" "$window_start"
  write_value "$STATE_DIR/recovery-window-started-at" "$(now_epoch)"
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
  started=$(now_monotonic)
  deadline=$((started + RECOVERY_VERIFY_SEC))
  while :; do
    if [ -f "$MANUAL_STOP_FILE" ]; then
      HEALTH_REASON="manual-stop"
      return 2
    fi
    if applying_marker_active || gateway_transitioning; then
      now=$(now_monotonic)
      [ "$now" -lt "$deadline" ] || {
        HEALTH_REASON="configuration-applying"
        return 2
      }
      sleep_seconds 1
      continue
    fi
    if check_health; then
      return 0
    fi
    now=$(now_monotonic)
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

  if recovery_suppressed; then
    write_suspended_status
    release_lock
    return 0
  fi

  recovery_kind=units
  recovery_action=restart-inactive-units
  success_action=units-restarted
  allow_reboot=0
  restart_ok=1
  case " $MISSING_UNITS " in
    *" gateway-services.service "*)
      recovery_kind=gateway
      recovery_action=start-gateway-services
      success_action=gateway-started
      allow_reboot=1
      log "starting the gateway service launcher after repeated failures"
      write_status recovering "$recovery_action"
      $SYSTEMCTL_BIN reset-failed gateway-services.service >/dev/null 2>&1 || true
      if ! $SYSTEMCTL_BIN start gateway-services.service; then
        restart_ok=0
      fi
      ;;
    *)
      if [ "$HEALTH_REASON" != "inactive-units" ]; then
        log "automatic recovery skipped because the desired service set is not trustworthy: $HEALTH_REASON"
        write_status degraded recovery-not-safe
        release_lock
        return 1
      fi
      [ -z "$CRITICAL_MISSING_UNITS" ] || allow_reboot=1
      log "restarting inactive gateway units after repeated failures: $MISSING_UNITS"
      write_status recovering "$recovery_action"
      for unit in $MISSING_UNITS; do
        $SYSTEMCTL_BIN reset-failed "$unit" >/dev/null 2>&1 || true
        if ! $SYSTEMCTL_BIN restart "$unit"; then
          restart_ok=0
        fi
      done
      ;;
  esac

  wait_result=1
  if [ "$restart_ok" -eq 1 ]; then
    wait_for_health && wait_result=0 || wait_result=$?
  fi
  if [ "$restart_ok" -eq 1 ] && [ "$wait_result" -eq 0 ]; then
    now=$(now_epoch)
    now_mono=$(now_monotonic)
    write_value "$STATE_DIR/consecutive-failures" 0
    write_value "$STATE_DIR/recovery-failures" 0
    write_value "$STATE_DIR/recovery-window-started-at" 0
    write_value "$STATE_DIR/recovery-window-started-monotonic" 0
    write_value "$STATE_DIR/stable-passes" 0
    write_value "$STATE_DIR/cooldown-until" $((now + RECOVERY_COOLDOWN_SEC))
    write_value "$STATE_DIR/cooldown-until-monotonic" $((now_mono + RECOVERY_COOLDOWN_SEC))
    HEALTH_REASON="recovered"
    log "gateway recovery completed: $success_action"
    write_status healthy "$success_action"
  elif [ "$wait_result" -eq 2 ]; then
    write_suspended_status
  else
    now=$(now_epoch)
    now_mono=$(now_monotonic)
    log "gateway remains unhealthy after $recovery_kind recovery: $MISSING_UNITS"
    write_value "$STATE_DIR/cooldown-until" $((now + RECOVERY_COOLDOWN_SEC))
    write_value "$STATE_DIR/cooldown-until-monotonic" $((now_mono + RECOVERY_COOLDOWN_SEC))
    record_failed_recovery "$allow_reboot"
  fi
  release_lock
}

check_once() {
  ensure_dirs
  initialize_boot_state
  now=$(now_epoch)
  now_mono=$(now_monotonic)
  boot_started_mono=$(read_uint_file "$STATE_DIR/boot-started-monotonic" "$now_mono")

  if recovery_suppressed; then
    write_suspended_status
    return 0
  fi
  if [ "$now_mono" -ge "$boot_started_mono" ] &&
     [ $((now_mono - boot_started_mono)) -lt "$STARTUP_GRACE_SEC" ]; then
    HEALTH_REASON="startup-grace"
    MISSING_UNITS=""
    CRITICAL_MISSING_UNITS=""
    write_status starting startup-grace
    return 0
  fi

  cooldown_until=$(read_uint_file "$STATE_DIR/cooldown-until-monotonic" 0)
  if check_health; then
    write_value "$STATE_DIR/consecutive-failures" 0
    stable=$(read_uint_file "$STATE_DIR/stable-passes" 0)
    stable=$((stable + 1))
    write_value "$STATE_DIR/stable-passes" "$stable"
    if [ "$stable" -ge 3 ]; then
      write_value "$STATE_DIR/recovery-failures" 0
      write_value "$STATE_DIR/recovery-window-started-at" 0
      write_value "$STATE_DIR/recovery-window-started-monotonic" 0
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

  if [ "$now_mono" -lt "$cooldown_until" ]; then
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
