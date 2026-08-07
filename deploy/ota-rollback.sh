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
    gateway-services.service|modbus-rtu@*.service|dlt645-driver@*.service|dio-driver@*.service|can-driver@*.service|compute-engine@*.service|ems-cluster@*.service|agc-avc@*.service|event-engine@*.service|local-display@*.service|local-kiosk@*.service|ky-ems.service|camera-service@*.service|mqtt-driver@*.service|system-monitor@*.service|mqtt-tls-tunnel@*.service)
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
REQUEST_WORK_DIR="$WORK_DIR"

mkdir -p "$BACKUP_DIR" "$STAGING_DIR"

echo "[$TIMESTAMP] [ota-rollback] start jobId=$JOB_ID version=$VERSION artifact=$ARTIFACT_PATH" | tee -a "$LOG_FILE"

state_value() {
  key="$1"
  path="$2"
  awk -F= -v wanted="$key" '$1 == wanted { sub(/^[^=]*=/, ""); print; exit }' "$path"
}

rollback_scada() {
  state_path="$1"
  scada_root="$(state_value scadaRoot "$state_path")"
  previous_target="$(state_value scadaPreviousTarget "$state_path")"
  current_target="$(state_value scadaCurrentTarget "$state_path")"
  config_backup="$(state_value scadaConfigBackup "$state_path")"
  app_config="$(state_value scadaAppConfig "$state_path")"
  scada_service="$(state_value scadaService "$state_path")"

  require_safe_dir "scadaRoot" "$scada_root"
  require_safe_dir "scadaPreviousTarget" "$previous_target"
  require_safe_dir "scadaCurrentTarget" "$current_target"
  require_safe_dir "scadaConfigBackup" "$config_backup"
  require_safe_dir "scadaAppConfig" "$app_config"
  case "$previous_target" in "$scada_root"/releases/*) ;; *) echo "[ota-rollback] unsafe previous SCADA target" >&2; return 1 ;; esac
  case "$current_target" in "$scada_root"/releases/*) ;; *) echo "[ota-rollback] unsafe current SCADA target" >&2; return 1 ;; esac
  case "$config_backup" in "$scada_root"/backup/*) ;; *) echo "[ota-rollback] unsafe SCADA config backup" >&2; return 1 ;; esac
  [ -d "$previous_target" ] || { echo "[ota-rollback] previous SCADA release is missing: $previous_target" >&2; return 1; }
  [ -d "$current_target" ] || { echo "[ota-rollback] current SCADA release is missing: $current_target" >&2; return 1; }
  [ -f "$config_backup" ] || { echo "[ota-rollback] SCADA config backup is missing: $config_backup" >&2; return 1; }
  [ -f "$app_config" ] || { echo "[ota-rollback] SCADA app config is missing: $app_config" >&2; return 1; }
  active_target="$(readlink -f "$scada_root/current" 2>/dev/null || true)"
  [ "$active_target" = "$current_target" ] || {
    echo "[ota-rollback] active SCADA release changed; expected $current_target, got $active_target" >&2
    return 1
  }
  if [ -n "$scada_service" ] && ! safe_service_name "$scada_service"; then
    echo "[ota-rollback] unsafe SCADA service: $scada_service" >&2
    return 1
  fi

  config_before="$REQUEST_WORK_DIR/scada-config-before-rollback.json"
  cp -p "$app_config" "$config_before"
  ln -sfn "releases/$(basename "$previous_target")" "$scada_root/current.rollback"
  mv -Tf "$scada_root/current.rollback" "$scada_root/current"
  if ! cp -p "$config_backup" "$app_config"; then
    ln -sfn "releases/$(basename "$current_target")" "$scada_root/current.restore"
    mv -Tf "$scada_root/current.restore" "$scada_root/current"
    cp -p "$config_before" "$app_config" || true
    return 1
  fi

  if [ -n "$scada_service" ] && command -v systemctl >/dev/null 2>&1; then
    if ! systemctl restart "$scada_service" || ! systemctl is-active --quiet "$scada_service"; then
      ln -sfn "releases/$(basename "$current_target")" "$scada_root/current.restore"
      mv -Tf "$scada_root/current.restore" "$scada_root/current"
      cp -p "$config_before" "$app_config" || true
      systemctl restart "$scada_service" >/dev/null 2>&1 || true
      echo "[ota-rollback] SCADA service failed after rollback; restored current release" >&2
      return 1
    fi
  fi

  previous_version="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8")).get("packageVersion", ""))' "$previous_target/manifest.json")"
  require_safe_id "previous SCADA version" "$previous_version"
  printf '%s\n' "$previous_version" > "$STAGING_DIR/applied_version.txt"
  {
    echo "jobId=$JOB_ID"
    echo "version=$previous_version"
    echo "packageType=scada"
    echo "rolledBackFromVersion=$VERSION"
    echo "scadaRoot=$scada_root"
    echo "scadaCurrentTarget=$previous_target"
    echo "scadaPreviousTarget=$current_target"
    echo "scadaAppConfig=$app_config"
    echo "scadaConfigBackup=$config_before"
    echo "scadaService=$scada_service"
  } > "${STATE_FILE}.tmp"
  mv "${STATE_FILE}.tmp" "$STATE_FILE"
  {
    echo "jobId=$JOB_ID"
    echo "rollbackFromVersion=$VERSION"
    echo "activeVersion=$previous_version"
    echo "packageType=scada"
    echo "rollbackAt=$TIMESTAMP"
    echo "scadaPreviousTarget=$previous_target"
  } > "$ROLLBACK_MARK"
  echo "[$TIMESTAMP] [ota-rollback] SCADA success jobId=$JOB_ID activeVersion=$previous_version" | tee -a "$LOG_FILE"
}

if [ -f "$REQUEST_WORK_DIR/package-type.txt" ] && grep -qx 'packageType=scada' "$REQUEST_WORK_DIR/package-type.txt"; then
  SCADA_INSTALL_STATE="$REQUEST_WORK_DIR/scada-install-state.txt"
  if [ ! -f "$SCADA_INSTALL_STATE" ]; then
    {
      echo "jobId=$JOB_ID"
      echo "rollbackFromVersion=$VERSION"
      echo "packageType=scada"
      echo "rollbackAt=$TIMESTAMP"
      echo "message=installer failed before activation state was written; installer rollback already handled"
    } > "$ROLLBACK_MARK"
    echo "[$TIMESTAMP] [ota-rollback] SCADA installer already restored the previous release" | tee -a "$LOG_FILE"
    exit 0
  fi
  rollback_scada "$SCADA_INSTALL_STATE"
  exit 0
fi

if [ -f "$STATE_FILE" ]; then
  STATE_JOB_ID="$(awk -F= '/^jobId=/{print $2}' "$STATE_FILE" | tail -n 1)"
  if [ "${STATE_JOB_ID:-}" = "$JOB_ID" ]; then
    cp "$STATE_FILE" "$ROLLBACK_MARK"
    STATE_BACKUP_DIR="$(awk -F= '/^backupDir=/{print $2}' "$STATE_FILE" | tail -n 1)"
    case "${STATE_BACKUP_DIR:-}" in
      "$BACKUP_DIR"/*) RESTORE_BACKUP_DIR="$STATE_BACKUP_DIR" ;;
    esac
    STATE_WORK_DIR="$(awk -F= '/^workDir=/{print $2}' "$STATE_FILE" | tail -n 1)"
    case "${STATE_WORK_DIR:-}" in
      "$STAGING_DIR"/*) WORK_DIR="$STATE_WORK_DIR" ;;
    esac
  fi
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
import tempfile

backup_dir, restore_list = sys.argv[1:3]
restored = []

def atomic_copy2(src, dst):
    destination_dir = os.path.dirname(dst)
    fd, temporary_path = tempfile.mkstemp(
        prefix=f".{os.path.basename(dst)}.ota-rollback-",
        dir=destination_dir,
    )
    os.close(fd)
    try:
        shutil.copy2(src, temporary_path)
        with open(temporary_path, "rb") as fh:
            os.fsync(fh.fileno())
        os.replace(temporary_path, dst)
    finally:
        if os.path.exists(temporary_path):
            os.unlink(temporary_path)

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
            atomic_copy2(src, dst)
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
