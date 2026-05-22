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

require_safe_id() {
  label="$1"
  value="$2"
  case "$value" in
    ""|.|..|*..*|*[!A-Za-z0-9._-]*)
      echo "[ota-apply] invalid $label: $value" >&2
      exit 2
      ;;
  esac
  if [ "${#value}" -gt 128 ]; then
    echo "[ota-apply] $label is too long" >&2
    exit 2
  fi
}

require_safe_dir() {
  label="$1"
  value="$2"
  case "$value" in
    /*) ;;
    *)
      echo "[ota-apply] $label must be an absolute path: $value" >&2
      exit 2
      ;;
  esac
  case "$value" in
    /|/opt|/opt/|/etc|/etc/|*"/../"*|*/..)
      echo "[ota-apply] unsafe $label: $value" >&2
      exit 2
      ;;
  esac
}

validate_archive_entries() {
  archive_path="$1"
  archive_kind="$2"
  python3 - "$archive_path" "$archive_kind" <<'PY'
import os
import posixpath
import stat
import sys
import tarfile
import zipfile

archive_path, archive_kind = sys.argv[1:3]
max_entries = 4096
max_total_bytes = 512 * 1024 * 1024
total_bytes = 0

def fail(message):
    print(f"[ota-apply] unsafe archive: {message}", file=sys.stderr)
    sys.exit(1)

def accept_size(size):
    global total_bytes
    total_bytes += max(0, int(size))
    if total_bytes > max_total_bytes:
        fail("uncompressed content exceeds limit")

def safe_member_name(name):
    if not name or "\x00" in name:
        return False
    normalized = posixpath.normpath(name.replace("\\", "/"))
    return (
        normalized not in ("", ".", "..")
        and not normalized.startswith("../")
        and not normalized.startswith("/")
        and ":" not in normalized
    )

if archive_kind == "tar":
    with tarfile.open(archive_path, "r:*") as archive:
        for index, member in enumerate(archive, start=1):
            if index > max_entries:
                fail("too many archive entries")
            if not safe_member_name(member.name):
                fail(f"path escapes staging directory: {member.name}")
            if member.issym() or member.islnk() or member.isdev():
                fail(f"unsupported archive entry type: {member.name}")
            if member.isfile():
                accept_size(member.size)
elif archive_kind == "zip":
    with zipfile.ZipFile(archive_path) as archive:
        for index, info in enumerate(archive.infolist(), start=1):
            if index > max_entries:
                fail("too many archive entries")
            if not safe_member_name(info.filename):
                fail(f"path escapes staging directory: {info.filename}")
            mode = (info.external_attr >> 16) & 0o170000
            if mode in (stat.S_IFLNK, stat.S_IFCHR, stat.S_IFBLK, stat.S_IFIFO, stat.S_IFSOCK):
                fail(f"unsupported archive entry type: {info.filename}")
            if not info.is_dir():
                accept_size(info.file_size)
else:
    fail(f"unknown archive kind: {archive_kind}")
PY
}

require_safe_id "jobId" "$JOB_ID"
require_safe_id "version" "$VERSION"
require_safe_dir "backupDir" "$BACKUP_DIR"
require_safe_dir "stagingDir" "$STAGING_DIR"

TIMESTAMP="$(date '+%Y-%m-%d %H:%M:%S')"
ARTIFACT_NAME="$(basename "$ARTIFACT_PATH")"
WORK_DIR="$STAGING_DIR/$JOB_ID"
LOG_FILE="$STAGING_DIR/upgrade_history.log"
STATE_FILE="$STAGING_DIR/current_version.txt"
JOB_BACKUP_DIR="$BACKUP_DIR/$JOB_ID"
BACKUP_ARTIFACT="$JOB_BACKUP_DIR/$ARTIFACT_NAME"

case "$ARTIFACT_NAME" in
  ""|.|..|*..*|*[!A-Za-z0-9._-]*)
    echo "[ota-apply] invalid artifact name: $ARTIFACT_NAME" >&2
    exit 4
    ;;
esac

case "$WORK_DIR" in
  "$STAGING_DIR"/*) ;;
  *)
    echo "[ota-apply] unsafe work directory: $WORK_DIR" >&2
    exit 2
    ;;
esac

case "$JOB_BACKUP_DIR" in
  "$BACKUP_DIR"/*) ;;
  *)
    echo "[ota-apply] unsafe backup directory: $JOB_BACKUP_DIR" >&2
    exit 2
    ;;
esac

mkdir -p "$BACKUP_DIR" "$STAGING_DIR"
rm -rf "$WORK_DIR"
rm -rf "$JOB_BACKUP_DIR"
mkdir -p "$WORK_DIR" "$JOB_BACKUP_DIR"

echo "[$TIMESTAMP] [ota-apply] start jobId=$JOB_ID version=$VERSION artifact=$ARTIFACT_PATH" | tee -a "$LOG_FILE"

if [ ! -f "$ARTIFACT_PATH" ]; then
  echo "[$TIMESTAMP] [ota-apply] artifact not found: $ARTIFACT_PATH" | tee -a "$LOG_FILE" >&2
  exit 3
fi

cp "$ARTIFACT_PATH" "$BACKUP_ARTIFACT"
cp "$ARTIFACT_PATH" "$WORK_DIR/$ARTIFACT_NAME"

case "$ARTIFACT_NAME" in
  *.tar.gz|*.tgz)
    validate_archive_entries "$WORK_DIR/$ARTIFACT_NAME" tar
    tar -xzf "$WORK_DIR/$ARTIFACT_NAME" -C "$WORK_DIR"
    ;;
  *.zip)
    validate_archive_entries "$WORK_DIR/$ARTIFACT_NAME" zip
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
  python3 - "$MANIFEST_PATH" "$JOB_BACKUP_DIR" "$LOG_FILE" "$RESTART_FILE" "$SYSTEMD_RELOAD_FILE" "$CHMOD_FILE" <<'PY'
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
    "/opt/modbus-gateway/config/runtime/tls",
)

allowed_bin_targets = {
    "/opt/modbus-gateway/bin/gateway-run.sh",
    "/opt/modbus-gateway/bin/gateway-services.sh",
    "/opt/modbus-gateway/bin/install-factory-config.sh",
    "/opt/modbus-gateway/bin/local-kiosk.py",
    "/opt/modbus-gateway/bin/production-smoke-test.sh",
    "/opt/modbus-gateway/bin/ota-apply.sh",
    "/opt/modbus-gateway/bin/ota-rollback.sh",
}

allowed_systemd_targets = {
    "/etc/systemd/system/gateway-services.service",
    "/etc/systemd/system/modbus-rtu@.service",
    "/etc/systemd/system/dlt645-driver@.service",
    "/etc/systemd/system/dio-driver@.service",
    "/etc/systemd/system/can-driver@.service",
    "/etc/systemd/system/compute-engine@.service",
    "/etc/systemd/system/event-engine@.service",
    "/etc/systemd/system/local-display@.service",
    "/etc/systemd/system/local-kiosk@.service",
    "/etc/systemd/system/camera-service@.service",
    "/etc/systemd/system/mqtt-driver@.service",
    "/etc/systemd/system/system-monitor@.service",
    "/etc/systemd/system/mqtt-tls-tunnel@.service",
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
        return normalized in allowed_systemd_targets
    return False

def sha256_file(path):
    import hashlib
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

def verify_manifest_checksum(item, src):
    expected = str(item.get("sha256", "")).strip().lower()
    if not expected:
        return
    allowed = set("0123456789abcdef")
    if len(expected) != 64 or any(ch not in allowed for ch in expected):
        raise SystemExit(f"invalid manifest sha256 for {item.get('path', '')}")
    actual = sha256_file(src)
    if actual != expected:
        raise SystemExit(f"manifest checksum mismatch for {item.get('path', '')}")

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
    verify_manifest_checksum(item, src)
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
  echo "backupDir=$JOB_BACKUP_DIR"
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
  case "\$service" in
    gateway-services.service|modbus-rtu@*.service|dlt645-driver@*.service|dio-driver@*.service|can-driver@*.service|compute-engine@*.service|event-engine@*.service|local-display@*.service|local-kiosk@*.service|camera-service@*.service|mqtt-driver@*.service|system-monitor@*.service|mqtt-tls-tunnel@*.service) ;;
    *)
      echo "[$TIMESTAMP] [ota-apply] skip unsafe restart service \$service" >> "$LOG_FILE"
      continue
      ;;
  esac
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
