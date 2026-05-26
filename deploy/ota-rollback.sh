#!/bin/sh
set -eu

ARTIFACT_PATH="${1:-}"
VERSION="${2:-}"
JOB_ID="${3:-}"
BACKUP_DIR="${4:-}"
STAGING_DIR="${5:-}"

if [ -z "$ARTIFACT_PATH" ] || [ -z "$VERSION" ] || [ -z "$JOB_ID" ] || [ -z "$BACKUP_DIR" ] || [ -z "$STAGING_DIR" ]; then
  echo "[ota-rollback] usage: ota-rollback.sh <artifactPath> <version> <jobId> <backupDir> <stagingDir>" >&2
  exit 2
fi

require_safe_id() {
  label="$1"
  value="$2"
  case "$value" in
    ""|.|..|*..*|*[!A-Za-z0-9._-]*)
      echo "[ota-rollback] invalid $label: $value" >&2
      exit 2
      ;;
  esac
  if [ "${#value}" -gt 128 ]; then
    echo "[ota-rollback] $label is too long" >&2
    exit 2
  fi
}

require_safe_dir() {
  label="$1"
  value="$2"
  case "$value" in
    /*) ;;
    *)
      echo "[ota-rollback] $label must be an absolute path: $value" >&2
      exit 2
      ;;
  esac
  case "$value" in
    /|/opt|/opt/|/etc|/etc/|*"/../"*|*/..)
      echo "[ota-rollback] unsafe $label: $value" >&2
      exit 2
      ;;
  esac
}

safe_service_name() {
  service="$1"
  case "$service" in
    *[!A-Za-z0-9_.@:-]*|""|*.service.service)
      return 1
      ;;
  esac
  case "$service" in
    gateway-services.service|modbus-rtu@*.service|dlt645-driver@*.service|dio-driver@*.service|can-driver@*.service|compute-engine@*.service|event-engine@*.service|local-display@*.service|local-kiosk@*.service|camera-service@*.service|mqtt-driver@*.service|system-monitor@*.service|mqtt-tls-tunnel@*.service)
      return 0
      ;;
  esac
  return 1
}

require_safe_id "jobId" "$JOB_ID"
require_safe_id "version" "$VERSION"
require_safe_dir "backupDir" "$BACKUP_DIR"
require_safe_dir "stagingDir" "$STAGING_DIR"

TIMESTAMP="$(date '+%Y-%m-%d %H:%M:%S')"
LOG_FILE="$STAGING_DIR/upgrade_history.log"
STATE_FILE="$STAGING_DIR/current_version.txt"
ROLLBACK_MARK="$STAGING_DIR/rollback_${JOB_ID}.txt"
RESTORE_LIST="$STAGING_DIR/rollback_${JOB_ID}_restored.txt"
RESTART_FILE="$STAGING_DIR/rollback_${JOB_ID}_restart_services.txt"
RESTORE_BACKUP_DIR="$BACKUP_DIR"
WORK_DIR="$STAGING_DIR/$JOB_ID"

mkdir -p "$BACKUP_DIR" "$STAGING_DIR"

echo "[$TIMESTAMP] [ota-rollback] start jobId=$JOB_ID version=$VERSION artifact=$ARTIFACT_PATH" | tee -a "$LOG_FILE"

if [ -f "$STATE_FILE" ]; then
  cp "$STATE_FILE" "$ROLLBACK_MARK"
fi

if [ -f "$STATE_FILE" ]; then
  STATE_BACKUP_DIR="$(awk -F= '/^backupDir=/{print $2}' "$STATE_FILE" | tail -n 1)"
  case "${STATE_BACKUP_DIR:-}" in
    "$BACKUP_DIR"/*) RESTORE_BACKUP_DIR="$STATE_BACKUP_DIR" ;;
  esac
  STATE_WORK_DIR="$(awk -F= '/^workDir=/{print $2}' "$STATE_FILE" | tail -n 1)"
  case "${STATE_WORK_DIR:-}" in
    "$STAGING_DIR"/*) WORK_DIR="$STATE_WORK_DIR" ;;
  esac
fi

case "$WORK_DIR" in
  "$STAGING_DIR"/*) ;;
  *) WORK_DIR="" ;;
esac
if [ -n "${WORK_DIR:-}" ] && [ -f "$WORK_DIR/restart_services.txt" ]; then
  cp "$WORK_DIR/restart_services.txt" "$RESTART_FILE"
fi

if [ "$RESTORE_BACKUP_DIR" = "$BACKUP_DIR" ] && [ -d "$BACKUP_DIR/$JOB_ID" ]; then
  RESTORE_BACKUP_DIR="$BACKUP_DIR/$JOB_ID"
fi

if [ -f "$RESTORE_BACKUP_DIR/previous_version.txt" ]; then
  cp "$RESTORE_BACKUP_DIR/previous_version.txt" "$STAGING_DIR/applied_version.txt"
fi

if [ -d "$RESTORE_BACKUP_DIR/opt" ] || [ -d "$RESTORE_BACKUP_DIR/etc" ]; then
  python3 - "$RESTORE_BACKUP_DIR" "$RESTORE_LIST" <<'PY'
import os
import shutil
import sys

backup_dir, restore_list = sys.argv[1:3]
restored = []
for top in ("opt", "etc"):
    root = os.path.join(backup_dir, top)
    if not os.path.isdir(root):
        continue
    for current_root, _, files in os.walk(root):
        for name in files:
            src = os.path.join(current_root, name)
            rel = os.path.relpath(src, backup_dir)
            dst = os.path.join("/", rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
            restored.append(f"{src} -> {dst}")
with open(restore_list, "w", encoding="utf-8") as fh:
    for item in restored:
        fh.write(item + "\n")
PY
fi

if command -v systemctl >/dev/null 2>&1 && [ -f "$RESTART_FILE" ]; then
  need_gateway_services_restart=0
  if [ -f "$RESTORE_LIST" ] && grep -qE -e '-> /opt/modbus-gateway/config/runtime/tls/|-> /etc/systemd/system/mqtt-tls-tunnel@\.service$' "$RESTORE_LIST"; then
    need_gateway_services_restart=1
  fi
  systemctl daemon-reload || echo "[$TIMESTAMP] [ota-rollback] daemon-reload failed" | tee -a "$LOG_FILE" >&2
  while IFS= read -r service; do
    [ -z "$service" ] && continue
    if ! safe_service_name "$service"; then
      echo "[$TIMESTAMP] [ota-rollback] skip unsafe restart service $service" | tee -a "$LOG_FILE" >&2
      continue
    fi
    if [ "$service" = "gateway-services.service" ]; then
      systemctl enable "$service" || echo "[$TIMESTAMP] [ota-rollback] enable failed $service" | tee -a "$LOG_FILE" >&2
    fi
    echo "[$TIMESTAMP] [ota-rollback] restarting $service" | tee -a "$LOG_FILE"
    systemctl restart "$service" || echo "[$TIMESTAMP] [ota-rollback] restart failed $service" | tee -a "$LOG_FILE" >&2
  done < "$RESTART_FILE"
  if [ "$need_gateway_services_restart" -eq 1 ]; then
    if ! grep -qx 'gateway-services.service' "$RESTART_FILE"; then
      echo "[$TIMESTAMP] [ota-rollback] restarting gateway-services.service for tls restoration" | tee -a "$LOG_FILE"
      systemctl enable gateway-services.service || echo "[$TIMESTAMP] [ota-rollback] enable failed gateway-services.service" | tee -a "$LOG_FILE" >&2
      systemctl restart gateway-services.service || echo "[$TIMESTAMP] [ota-rollback] restart failed gateway-services.service" | tee -a "$LOG_FILE" >&2
    fi
  fi
fi

{
  echo "jobId=$JOB_ID"
  echo "rollbackFromVersion=$VERSION"
  echo "artifact=$ARTIFACT_PATH"
  echo "restoreBackupDir=$RESTORE_BACKUP_DIR"
  echo "rollbackAt=$TIMESTAMP"
  echo "restoreList=$RESTORE_LIST"
} >> "$ROLLBACK_MARK"

echo "[$TIMESTAMP] [ota-rollback] success jobId=$JOB_ID" | tee -a "$LOG_FILE"
exit 0
