#!/bin/sh
set -eu

ARTIFACT_PATH="${1:-}"
VERSION="${2:-}"
JOB_ID="${3:-}"
BACKUP_DIR="${4:-}"
STAGING_DIR="${5:-}"

if [ -z "$ARTIFACT_PATH" ] || [ -z "$VERSION" ] || [ -z "$JOB_ID" ] || [ -z "$BACKUP_DIR" ] || [ -z "$STAGING_DIR" ]; then
  echo "[ota-apply] usage: ota-apply.sh <artifactPath> <version> <jobId> <backupDir> <stagingDir>" >&2
  exit 2
fi

TIMESTAMP="$(date '+%Y-%m-%d %H:%M:%S')"
ARTIFACT_NAME="$(basename "$ARTIFACT_PATH")"
WORK_DIR="$STAGING_DIR/$JOB_ID"
LOG_FILE="$STAGING_DIR/upgrade_history.log"
STATE_FILE="$STAGING_DIR/current_version.txt"
BACKUP_ARTIFACT="$BACKUP_DIR/${JOB_ID}_${ARTIFACT_NAME}"

mkdir -p "$BACKUP_DIR" "$STAGING_DIR" "$WORK_DIR"

echo "[$TIMESTAMP] [ota-apply] start jobId=$JOB_ID version=$VERSION artifact=$ARTIFACT_PATH" | tee -a "$LOG_FILE"

if [ ! -f "$ARTIFACT_PATH" ]; then
  echo "[$TIMESTAMP] [ota-apply] artifact not found: $ARTIFACT_PATH" | tee -a "$LOG_FILE" >&2
  exit 3
fi

cp "$ARTIFACT_PATH" "$BACKUP_ARTIFACT"
cp "$ARTIFACT_PATH" "$WORK_DIR/$ARTIFACT_NAME"

case "$ARTIFACT_NAME" in
  *.tar.gz|*.tgz)
    tar -xzf "$WORK_DIR/$ARTIFACT_NAME" -C "$WORK_DIR"
    ;;
  *.zip)
    unzip -oq "$WORK_DIR/$ARTIFACT_NAME" -d "$WORK_DIR"
    ;;
  *.bin|*.img)
    :
    ;;
  *)
    echo "[$TIMESTAMP] [ota-apply] unsupported package type: $ARTIFACT_NAME" | tee -a "$LOG_FILE" >&2
    exit 4
    ;;
esac

MANIFEST_PATH="$WORK_DIR/manifest.json"
RESTART_FILE=""
SYSTEMD_RELOAD_FILE=""
CHMOD_FILE=""
if [ -f "$MANIFEST_PATH" ]; then
  RESTART_FILE="$WORK_DIR/restart_services.txt"
  SYSTEMD_RELOAD_FILE="$WORK_DIR/systemd_reload_required"
  CHMOD_FILE="$WORK_DIR/chmod_targets.txt"
  python3 - "$MANIFEST_PATH" "$BACKUP_DIR" "$LOG_FILE" "$RESTART_FILE" "$SYSTEMD_RELOAD_FILE" "$CHMOD_FILE" <<'PY'
import json
import glob
import os
import shutil
import sys

manifest_path, backup_dir, log_file, restart_file, systemd_reload_file, chmod_file = sys.argv[1:7]
root_dir = os.path.dirname(manifest_path)
need_systemd_reload = False
chmod_targets = []

with open(manifest_path, "r", encoding="utf-8") as fh:
    manifest = json.load(fh)

allowed_clean_roots = (
    "/opt/modbus-gateway/config/runtime/devices",
    "/opt/modbus-gateway/config/runtime/apps",
    "/opt/modbus-gateway/config/runtime/logic",
)

allowed_bin_targets = {
    "/opt/modbus-gateway/bin/gateway-run.sh",
    "/opt/modbus-gateway/bin/gateway-services.sh",
    "/opt/modbus-gateway/bin/install-factory-config.sh",
    "/opt/modbus-gateway/bin/production-smoke-test.sh",
    "/opt/modbus-gateway/bin/ota-apply.sh",
    "/opt/modbus-gateway/bin/ota-rollback.sh",
}

allowed_exact_services = (
    "gateway-services.service",
)

allowed_service_prefixes = (
    "modbus-rtu@",
    "dlt645-driver@",
    "dio-driver@",
    "can-driver@",
    "compute-engine@",
    "event-engine@",
    "local-display@",
    "local-kiosk@",
    "camera-service@",
    "mqtt-driver@",
    "system-monitor@",
    "mqtt-tls-tunnel@",
)

def log(message):
    with open(log_file, "a", encoding="utf-8") as fh:
        fh.write(message + "\n")

def safe_child_path(path, root):
    normalized = os.path.abspath(path)
    normalized_root = os.path.abspath(root)
    return normalized == normalized_root or normalized.startswith(normalized_root.rstrip("/") + "/")

def is_safe_target(dst):
    normalized = os.path.abspath(dst)
    if safe_child_path(normalized, "/opt/modbus-gateway/config"):
        return True
    if safe_child_path(normalized, "/opt/modbus-gateway/bin"):
        return normalized in allowed_bin_targets
    if safe_child_path(normalized, "/etc/systemd/system"):
        return normalized.endswith(".service")
    return False

def is_safe_service_name(value):
    if not value or len(value) > 128 or not value.endswith(".service"):
        return False
    allowed_chars = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.@:-")
    if any(ch not in allowed_chars for ch in value):
        return False
    return value in allowed_exact_services or any(value.startswith(prefix) for prefix in allowed_service_prefixes)

for rule in manifest.get("cleanBeforeApply", []):
    target = os.path.abspath(rule.get("target", ""))
    if target not in allowed_clean_roots:
        log(f"[manifest-clean-skip] unsafe target {target}")
        continue
    patterns = rule.get("patterns", ["*.json"])
    for pattern in patterns:
        if os.path.basename(pattern) != pattern:
            log(f"[manifest-clean-skip] unsafe pattern {pattern}")
            continue
        for path in glob.glob(os.path.join(target, pattern)):
            if not os.path.isfile(path):
                continue
            backup_path = os.path.join(backup_dir, os.path.relpath(path, "/"))
            os.makedirs(os.path.dirname(backup_path), exist_ok=True)
            shutil.copy2(path, backup_path)
            os.remove(path)
            log(f"[manifest-clean] {path}")

for item in manifest.get("files", []):
    src = os.path.abspath(os.path.join(root_dir, item["path"]))
    dst = os.path.abspath(item["target"])
    if not safe_child_path(src, root_dir) or not os.path.isfile(src):
        log(f"[manifest-copy-skip] unsafe source {item.get('path', '')}")
        continue
    if not is_safe_target(dst):
        log(f"[manifest-copy-skip] unsafe target {dst}")
        continue
    if (
        os.path.abspath(dst) == "/opt/modbus-gateway/config/runtime/device_identity.json"
        and os.path.exists(dst)
        and not manifest.get("updateIdentity", False)
        and not item.get("updateIdentity", False)
    ):
        log(f"[manifest-skip-identity] preserve existing {dst}")
        continue
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.exists(dst):
        backup_path = os.path.join(backup_dir, os.path.relpath(dst, "/"))
        os.makedirs(os.path.dirname(backup_path), exist_ok=True)
        shutil.copy2(dst, backup_path)
    shutil.copy2(src, dst)
    if dst.startswith("/etc/systemd/system/") and dst.endswith(".service"):
        need_systemd_reload = True
    if dst.startswith("/opt/modbus-gateway/bin/") and (dst.endswith(".sh") or os.access(src, os.X_OK)):
        chmod_targets.append(dst)
    log(f"[manifest-copy] {src} -> {dst}")

with open(restart_file, "w", encoding="utf-8") as fh:
    for item in manifest.get("restart", {}).get("services", []):
        service = str(item).strip()
        if is_safe_service_name(service):
            fh.write(service + "\n")
        else:
            log(f"[manifest-restart-skip] unsafe service {service}")
if need_systemd_reload:
    with open(systemd_reload_file, "w", encoding="utf-8") as fh:
        fh.write("1\n")
with open(chmod_file, "w", encoding="utf-8") as fh:
    for item in chmod_targets:
        fh.write(item + "\n")
PY
fi

if [ -n "$CHMOD_FILE" ] && [ -f "$CHMOD_FILE" ]; then
  while IFS= read -r target; do
    [ -z "$target" ] && continue
    chmod +x "$target" || echo "[$TIMESTAMP] [ota-apply] chmod failed $target" >> "$LOG_FILE"
  done < "$CHMOD_FILE"
fi

{
  echo "jobId=$JOB_ID"
  echo "version=$VERSION"
  echo "artifact=$ARTIFACT_PATH"
  echo "backupArtifact=$BACKUP_ARTIFACT"
  echo "workDir=$WORK_DIR"
  echo "appliedAt=$TIMESTAMP"
} > "$STATE_FILE"

echo "$VERSION" > "$STAGING_DIR/applied_version.txt"
echo "[$TIMESTAMP] [ota-apply] success jobId=$JOB_ID version=$VERSION workDir=$WORK_DIR" | tee -a "$LOG_FILE"

if command -v systemctl >/dev/null 2>&1 && [ -n "$RESTART_FILE" ] && [ -f "$RESTART_FILE" ]; then
  RESTART_LATER="$WORK_DIR/restart_later.sh"
  cat > "$RESTART_LATER" <<EOF
#!/bin/sh
sleep 2
if [ -f "$SYSTEMD_RELOAD_FILE" ]; then
  echo "[$TIMESTAMP] [ota-apply] systemctl daemon-reload" >> "$LOG_FILE"
  systemctl daemon-reload >> "$LOG_FILE" 2>&1 || echo "[$TIMESTAMP] [ota-apply] daemon-reload failed" >> "$LOG_FILE"
fi
while IFS= read -r service; do
  [ -z "\$service" ] && continue
  if [ "\$service" = "gateway-services.service" ]; then
    systemctl enable "\$service" >> "$LOG_FILE" 2>&1 || echo "[$TIMESTAMP] [ota-apply] enable failed \$service" >> "$LOG_FILE"
  fi
  echo "[$TIMESTAMP] [ota-apply] restarting \$service" >> "$LOG_FILE"
  systemctl restart "\$service" >> "$LOG_FILE" 2>&1 || echo "[$TIMESTAMP] [ota-apply] restart failed \$service" >> "$LOG_FILE"
done < "$RESTART_FILE"
EOF
  chmod +x "$RESTART_LATER"
  nohup sh "$RESTART_LATER" >/dev/null 2>&1 &
fi

exit 0
