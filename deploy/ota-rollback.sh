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

TIMESTAMP="$(date '+%Y-%m-%d %H:%M:%S')"
LOG_FILE="$STAGING_DIR/upgrade_history.log"
STATE_FILE="$STAGING_DIR/current_version.txt"
ROLLBACK_MARK="$STAGING_DIR/rollback_${JOB_ID}.txt"
RESTORE_LIST="$STAGING_DIR/rollback_${JOB_ID}_restored.txt"
RESTART_FILE="$STAGING_DIR/rollback_${JOB_ID}_restart_services.txt"

mkdir -p "$BACKUP_DIR" "$STAGING_DIR"

echo "[$TIMESTAMP] [ota-rollback] start jobId=$JOB_ID version=$VERSION artifact=$ARTIFACT_PATH" | tee -a "$LOG_FILE"

if [ -f "$STATE_FILE" ]; then
  cp "$STATE_FILE" "$ROLLBACK_MARK"
fi

if [ -f "$BACKUP_DIR/previous_version.txt" ]; then
  cp "$BACKUP_DIR/previous_version.txt" "$STAGING_DIR/applied_version.txt"
fi

if [ -f "$STATE_FILE" ]; then
  WORK_DIR="$(awk -F= '/^workDir=/{print $2}' "$STATE_FILE" | tail -n 1)"
  if [ -n "${WORK_DIR:-}" ] && [ -f "$WORK_DIR/restart_services.txt" ]; then
    cp "$WORK_DIR/restart_services.txt" "$RESTART_FILE"
  fi
fi

if [ -d "$BACKUP_DIR/opt" ] || [ -d "$BACKUP_DIR/etc" ]; then
  python3 - "$BACKUP_DIR" "$RESTORE_LIST" <<'PY'
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
  systemctl daemon-reload || echo "[$TIMESTAMP] [ota-rollback] daemon-reload failed" | tee -a "$LOG_FILE" >&2
  while IFS= read -r service; do
    [ -z "$service" ] && continue
    if [ "$service" = "gateway-services.service" ]; then
      systemctl enable "$service" || echo "[$TIMESTAMP] [ota-rollback] enable failed $service" | tee -a "$LOG_FILE" >&2
    fi
    echo "[$TIMESTAMP] [ota-rollback] restarting $service" | tee -a "$LOG_FILE"
    systemctl restart "$service" || echo "[$TIMESTAMP] [ota-rollback] restart failed $service" | tee -a "$LOG_FILE" >&2
  done < "$RESTART_FILE"
fi

{
  echo "jobId=$JOB_ID"
  echo "rollbackFromVersion=$VERSION"
  echo "artifact=$ARTIFACT_PATH"
  echo "rollbackAt=$TIMESTAMP"
  echo "restoreList=$RESTORE_LIST"
} >> "$ROLLBACK_MARK"

echo "[$TIMESTAMP] [ota-rollback] success jobId=$JOB_ID" | tee -a "$LOG_FILE"
exit 0
