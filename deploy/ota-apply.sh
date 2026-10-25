#!/bin/sh
set -eu

ARTIFACT_PATH="${1:-}"
VERSION="${2:-}"
JOB_ID="${3:-}"
BACKUP_DIR="${4:-}"
STAGING_DIR="${5:-}"
OTA_JOB_TOKEN="${6:-direct-${3:-}}"
OTA_LIFECYCLE_FD="${7:-}"
OTA_RUNTIME_BARRIER_PATH=""
WATCHDOG_RUN_DIR="${WATCHDOG_RUN_DIR:-/run/gateway-health-watchdog}"
WATCHDOG_APPLYING_FILE="$WATCHDOG_RUN_DIR/applying"
KEEP_APPLYING_MARKER=0

watchdog_boot_id() {
  if [ -r /proc/sys/kernel/random/boot_id ]; then
    sed -n '1p' /proc/sys/kernel/random/boot_id
  else
    printf '%s\n' unknown
  fi
}

watchdog_uptime_sec() {
  if [ -r /proc/uptime ]; then
    sed -n '1{s/\..*//;p;}' /proc/uptime
  else
    date +%s
  fi
}

write_applying_marker_for() {
  marker_kind="$1"
  marker_pid="$2"
  mkdir -p "$WATCHDOG_RUN_DIR"
  tmp="$WATCHDOG_APPLYING_FILE.tmp.$$"
  {
    printf 'kind=%s\n' "$marker_kind"
    printf 'pid=%s\n' "$marker_pid"
    printf 'boot_id=%s\n' "$(watchdog_boot_id)"
    printf 'created_uptime_sec=%s\n' "$(watchdog_uptime_sec)"
  } > "$tmp"
  mv -f "$tmp" "$WATCHDOG_APPLYING_FILE"
}

write_applying_marker() {
  write_applying_marker_for ota-apply "$$"
}

cleanup_applying_marker() {
  if [ "$KEEP_APPLYING_MARKER" != "1" ]; then
    rm -f "$WATCHDOG_APPLYING_FILE" 2>/dev/null || true
  fi
}

LIVE_CHANGE_STARTED=0
JOB_DIRS_CREATED=0
ACTIVE_REGISTERED=0

record_unchanged_failure() {
  python3 - "$STAGING_DIR" "$JOB_ID" "$VERSION" "$OTA_JOB_TOKEN" <<'PY'
import fcntl
import json
import os
import sys
import time

staging, job, version, token = sys.argv[1:]
if not all((job, version, token)):
    raise SystemExit("missing OTA job identity")
lock = os.open(os.path.join(staging, "ota_state.lock"), os.O_CREAT | os.O_RDWR | getattr(os, "O_NOFOLLOW", 0), 0o600)
fcntl.flock(lock, fcntl.LOCK_EX)
with open(os.path.join(staging, "ota_active_job.txt"), "rb") as source:
    active = source.read(257)
if active != f"jobId={job}\ntoken={token}\n".encode("ascii"):
    raise SystemExit("OTA active job changed")
with open(os.path.join(staging, f"ota_job_{job}.json"), encoding="utf-8") as source:
    intent = json.load(source)
if (intent.get("format") != "ota-job-v1" or intent.get("jobId") != job or
    intent.get("version") != version or intent.get("token") != token):
    raise SystemExit("OTA intent changed")
if intent.get("lifecycle") == "inherited-flock-v1":
    # Only C++ can decide unchanged after every inherited lease holder exits.
    raise SystemExit(0)
if os.path.exists(os.path.join(staging, "ota-in-progress")):
    raise SystemExit("OTA runtime change may have started")
try:
    with open(os.path.join(staging, "applied_jobs.txt"), encoding="utf-8") as source:
        if job in source.read().splitlines():
            raise SystemExit("OTA jobId already burned")
except FileNotFoundError:
    pass
record = dict(format="ota-health-v1", jobId=job, version=version, token=token,
              status="failed", rollback="unchanged", ts=str(int(time.time() * 1000)))
target = os.path.join(staging, f"health_{job}.txt")
temporary = target + ".tmp." + str(os.getpid())
try:
    with open(temporary, "x", encoding="utf-8") as output:
        output.write(json.dumps(record, separators=(",", ":")) + "\n")
        output.flush()
        os.fsync(output.fileno())
    os.link(temporary, target)
    directory = os.open(staging, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
finally:
    if os.path.exists(temporary):
        os.unlink(temporary)
PY
}

on_exit() {
  status=$?
  # Failed before touching the live runtime: drop disposable job dirs. The
  # service intent still burns this jobId even when applied_jobs.txt does not.
  if [ "$LIVE_CHANGE_STARTED" != "1" ] && [ -n "${IN_PROGRESS_FILE:-}" ] &&
     grep -qx "pid=$$" "$IN_PROGRESS_FILE" 2>/dev/null; then
    LIVE_CHANGE_STARTED=1
  fi
  if [ "$status" -ne 0 ] && [ "$JOB_DIRS_CREATED" = "1" ] && [ "$LIVE_CHANGE_STARTED" != "1" ]; then
    rm -rf -- "${WORK_DIR:?}" "${JOB_BACKUP_DIR:?}" 2>/dev/null || true
  fi
  if [ "$status" -ne 0 ] && [ "$LIVE_CHANGE_STARTED" != "1" ]; then
    record_unchanged_failure || echo "[ota-apply] unchanged result not proven; job remains unresolved" >&2
  fi
  if [ "$status" -ne 0 ] && [ "$ACTIVE_REGISTERED" = "1" ] && [ "$LIVE_CHANGE_STARTED" != "1" ]; then
    python3 "$SCRIPT_DIR/ota-health-result.py" --cancel "$STAGING_DIR" "$JOB_ID" "$OTA_JOB_TOKEN" || true
  fi
  cleanup_applying_marker
}

trap on_exit EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

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
            if member.mode & 0o6000:
                fail(f"setuid/setgid archive entry: {member.name}")
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
            if (info.external_attr >> 16) & 0o6000:
                fail(f"setuid/setgid archive entry: {info.filename}")
            if not info.is_dir():
                accept_size(info.file_size)
else:
    fail(f"unknown archive kind: {archive_kind}")
PY
}

require_safe_id "jobId" "$JOB_ID"
require_safe_id "version" "$VERSION"
require_safe_id "health token" "$OTA_JOB_TOKEN"
require_safe_dir "backupDir" "$BACKUP_DIR"
require_safe_dir "stagingDir" "$STAGING_DIR"
runtime_barrier() {
  [ -n "$OTA_LIFECYCLE_FD" ] || return 0
  python3 - "$STAGING_DIR" "$JOB_ID" "$VERSION" "$OTA_JOB_TOKEN" "$OTA_LIFECYCLE_FD" "$1" <<'PY'
import fcntl
import json
import os
import sys

staging, job, version, token, inherited, action = sys.argv[1:]
lease_path = os.path.join(staging, f"ota_job_{job}.lock")
lease = os.fstat(int(inherited))
probe = os.open(lease_path, os.O_RDWR | getattr(os, "O_NOFOLLOW", 0))
identity = os.fstat(probe)
if (lease.st_dev, lease.st_ino) != (identity.st_dev, identity.st_ino):
    raise SystemExit("OTA inherited lifecycle descriptor mismatch")
try:
    fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)
except BlockingIOError:
    pass
else:
    raise SystemExit("OTA lifecycle lease is not held")
finally:
    os.close(probe)
with open(os.path.join(staging, f"ota_job_{job}.json"), encoding="utf-8") as source:
    intent = json.load(source)
if (intent.get("format") != "ota-job-v1" or intent.get("lifecycle") != "inherited-flock-v1" or
    intent.get("jobId") != job or intent.get("version") != version or intent.get("token") != token):
    raise SystemExit("OTA lifecycle intent mismatch")
with open(os.path.join(staging, "ota_active_job.txt"), "rb") as source:
    if source.read(257) != f"jobId={job}\ntoken={token}\n".encode("ascii"):
        raise SystemExit("OTA lifecycle active identity mismatch")
target = os.path.join(staging, f"ota_barrier_{job}.json")
record = dict(format="official-ota-apply-barrier-v1", jobId=job, version=version, token=token, state="prepared")
if action == "prepared" and os.path.lexists(os.path.join(staging, "ota-in-progress")):
    # A legacy interrupted-job recovery can itself change the live runtime.
    # Do not register unchanged eligibility before that recovery branch.
    raise SystemExit("OTA interrupted runtime change must be recovered first")
if action != "prepared":
    with open(target, encoding="utf-8") as source:
        if json.load(source) != record:
            raise SystemExit("OTA runtime barrier is not prepared")
    record["state"] = "runtime-may-change"
temporary = target + ".tmp." + str(os.getpid())
try:
    with open(temporary, "x", encoding="utf-8") as output:
        output.write(json.dumps(record, separators=(",", ":")) + "\n")
        output.flush()
        os.fsync(output.fileno())
    if action == "prepared":
        os.link(temporary, target)
    else:
        os.replace(temporary, target)
    directory = os.open(staging, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
finally:
    if os.path.exists(temporary):
        os.unlink(temporary)
PY
}
runtime_barrier prepared
if [ -n "$OTA_LIFECYCLE_FD" ]; then
  OTA_RUNTIME_BARRIER_PATH="$STAGING_DIR/ota_barrier_$JOB_ID.json"
fi
# The previous job's restart/health-verification sequence must finish first:
# its automatic rollback would otherwise interleave with this apply.
if [ -f "$WATCHDOG_APPLYING_FILE" ]; then
  previous_kind="$(sed -n 's/^kind=//p' "$WATCHDOG_APPLYING_FILE" | sed -n '1p')"
  previous_pid="$(sed -n 's/^pid=//p' "$WATCHDOG_APPLYING_FILE" | sed -n '1p')"
  if [ "$previous_kind" = "ota-restart" ] && [ -n "$previous_pid" ] && kill -0 "$previous_pid" 2>/dev/null; then
    echo "[ota-apply] previous OTA restart/health verification (pid $previous_pid) still running; retry later" >&2
    exit 6
  fi
fi
write_applying_marker

TIMESTAMP="$(date '+%Y-%m-%d %H:%M:%S')"
ARTIFACT_NAME="$(basename "$ARTIFACT_PATH")"
WORK_DIR="$STAGING_DIR/$JOB_ID"
HEALTH_PENDING_FILE="$WORK_DIR/health_pending.txt"
HEALTH_FILE="$STAGING_DIR/health_${JOB_ID}.txt"
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

APPLIED_JOBS_FILE="$STAGING_DIR/applied_jobs.txt"
IN_PROGRESS_FILE="$STAGING_DIR/ota-in-progress"

# Durable (fsynced file + directory) write/append/remove helpers. The
# in-progress marker lives outside /run so an interrupted apply is detected at
# the next boot (gateway-ota-recover.service -> ota-rollback.sh).
durable_file() {
  python3 - "$@" <<'PY'
import os
import sys

action, path = sys.argv[1:3]
directory = os.path.dirname(path) or "."
if action == "remove":
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
else:
    data = sys.argv[3]
    if action == "append":
        with open(path, "a", encoding="utf-8") as fh:
            fh.write(data)
            fh.flush()
            os.fsync(fh.fileno())
    else:
        temporary = path + ".tmp." + str(os.getpid())
        with open(temporary, "w", encoding="utf-8") as fh:
            fh.write(data)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(temporary, path)
fd = os.open(directory, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
try:
    os.fsync(fd)
finally:
    os.close(fd)
PY
}

clear_in_progress_marker() {
  durable_file remove "$IN_PROGRESS_FILE"
}

# Keep only the newest OTA_RETENTION_JOBS job directories in staging/backup.
prune_old_jobs() {
  keep="${OTA_RETENTION_JOBS:-3}"
  case "$keep" in ""|*[!0-9]*) keep=3 ;; esac
  [ "$keep" -ge 1 ] || keep=1
  for prune_root in "$STAGING_DIR" "$BACKUP_DIR"; do
    count=1
    # shellcheck disable=SC2012
    ls -1dt "$prune_root"/*/ 2>/dev/null | while IFS= read -r job_dir; do
      job_dir="${job_dir%/}"
      job_name="$(basename "$job_dir")"
      case "$job_name" in
        ""|.|..|*..*|*[!A-Za-z0-9._-]*) continue ;;
      esac
      [ "$job_name" = "$JOB_ID" ] && continue
      count=$((count + 1))
      if [ "$count" -gt "$keep" ]; then
        rm -rf -- "$job_dir" && echo "[$TIMESTAMP] [ota-apply] pruned old job dir $job_dir" >> "$LOG_FILE"
      fi
    done
  done
}

mkdir -p "$BACKUP_DIR" "$STAGING_DIR"
# A jobId is single-use: re-applying it would overwrite its own rollback backup
# with the already-new files, turning a later rollback into a no-op.
if [ -e "$WORK_DIR" ] || [ -e "$JOB_BACKUP_DIR" ] ||
   { [ -f "$APPLIED_JOBS_FILE" ] && grep -qxF -- "$JOB_ID" "$APPLIED_JOBS_FILE"; }; then
  echo "[$TIMESTAMP] [ota-apply] jobId already processed; refusing to re-apply: $JOB_ID" | tee -a "$LOG_FILE" >&2
  exit 5
fi
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
if [ ! -f "$SCRIPT_DIR/ota-health-result.py" ] || [ ! -r "$SCRIPT_DIR/ota-health-result.py" ]; then
  echo "[ota-apply] required health result helper is missing" >&2
  exit 5
fi
if [ -f "$IN_PROGRESS_FILE" ]; then
  marker_boot="$(sed -n 's/^boot_id=//p' "$IN_PROGRESS_FILE" | sed -n '1p')"
  marker_pid="$(sed -n 's/^pid=//p' "$IN_PROGRESS_FILE" | sed -n '1p')"
  if [ "$marker_boot" = "$(watchdog_boot_id)" ] && [ -n "$marker_pid" ] && kill -0 "$marker_pid" 2>/dev/null; then
    echo "[$TIMESTAMP] [ota-apply] another OTA apply (pid $marker_pid) is still running" | tee -a "$LOG_FILE" >&2
    exit 5
  fi
  # Stale marker (earlier boot or dead apply): recover that job first. Run it
  # in its own transient unit so restarting mqtt-driver@ cannot kill it.
  echo "[$TIMESTAMP] [ota-apply] recovering interrupted OTA before jobId=$JOB_ID" | tee -a "$LOG_FILE" >&2
  RECOVER_UNIT="ota-recover-${JOB_ID}-$$"
  if command -v systemd-run >/dev/null 2>&1 &&
     systemd-run --unit="$RECOVER_UNIT" --wait --collect --quiet \
       --setenv=OTA_RECOVER_CALLER_PID="$$" --setenv=WATCHDOG_RUN_DIR="$WATCHDOG_RUN_DIR" \
       /bin/sh "$SCRIPT_DIR/ota-rollback.sh" --recover-interrupted "$STAGING_DIR" >> "$LOG_FILE" 2>&1; then
    :
  elif [ -f "$IN_PROGRESS_FILE" ]; then
    OTA_RECOVER_CALLER_PID="$$" sh "$SCRIPT_DIR/ota-rollback.sh" --recover-interrupted "$STAGING_DIR" >> "$LOG_FILE" 2>&1 || true
  fi
  if [ -f "$IN_PROGRESS_FILE" ]; then
    echo "[$TIMESTAMP] [ota-apply] interrupted OTA recovery failed; operator action required: $IN_PROGRESS_FILE" | tee -a "$LOG_FILE" >&2
    exit 5
  fi
  # ota-rollback.sh removes the watchdog marker on exit; re-assert ours.
  write_applying_marker
fi
mkdir -p "$WORK_DIR" "$JOB_BACKUP_DIR"
JOB_DIRS_CREATED=1
if [ -e "$HEALTH_FILE" ]; then
  echo "[ota-apply] health result already exists for jobId=$JOB_ID" >&2
  exit 5
fi

# Written immediately before the first change to the live runtime; removed only
# after the state file is complete. ota-rollback.sh clears it after a rollback.
# The jobId is burned (applied_jobs.txt) only from this point on.
in_progress_text() {
  printf 'jobId=%s\nversion=%s\nartifact=%s\nbackupRoot=%s\nstagingDir=%s\npid=%s\nboot_id=%s\n' \
    "$JOB_ID" "$VERSION" "$ARTIFACT_PATH" "$BACKUP_DIR" "$STAGING_DIR" "$$" "$(watchdog_boot_id)"
}

# Snapshot the pre-OTA version markers so ota-rollback.sh can restore them.
snapshot_previous_state() {
  mkdir -p "$JOB_BACKUP_DIR/previous_state"
  [ ! -f "$STATE_FILE" ] || cp -p "$STATE_FILE" "$JOB_BACKUP_DIR/previous_state/current_version.txt"
  [ ! -f "$STAGING_DIR/applied_version.txt" ] || cp -p "$STAGING_DIR/applied_version.txt" "$JOB_BACKUP_DIR/previous_version.txt"
}

write_in_progress_marker() {
  runtime_barrier runtime-may-change
  LIVE_CHANGE_STARTED=1
  snapshot_previous_state
  durable_file append "$APPLIED_JOBS_FILE" "$JOB_ID
"
  durable_file write "$IN_PROGRESS_FILE" "$(in_progress_text)
"
}

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
  *.kyscada)
    validate_archive_entries "$WORK_DIR/$ARTIFACT_NAME" zip
    ;;
  *.bin|*.img)
    :
    ;;
  *)
    echo "[$TIMESTAMP] [ota-apply] unsupported package type: $ARTIFACT_NAME" | tee -a "$LOG_FILE" >&2
    exit 4
    ;;
esac

case "$ARTIFACT_NAME" in
  *.kyscada)
    INSTALLER="/opt/modbus-gateway/bin/install-scada-project.sh"
    IDENTITY_FILE="/opt/modbus-gateway/config/runtime/device_identity.json"
    SCADA_INSTALL_STATE="$WORK_DIR/scada-install-state.txt"
    printf '%s\n' 'packageType=scada' > "$WORK_DIR/package-type.txt"
    [ -x "$INSTALLER" ] || { echo "[$TIMESTAMP] [ota-apply] SCADA installer is missing" >&2; exit 4; }
    [ -f "$IDENTITY_FILE" ] || { echo "[$TIMESTAMP] [ota-apply] device identity is missing" >&2; exit 4; }
    MACHINE_CODE="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8")).get("machineCode", ""))' "$IDENTITY_FILE")"
    [ -n "$MACHINE_CODE" ] || { echo "[$TIMESTAMP] [ota-apply] machineCode is empty" >&2; exit 4; }
    write_in_progress_marker
    "$INSTALLER" \
      --package "$WORK_DIR/$ARTIFACT_NAME" \
      --machine-code "$MACHINE_CODE" \
      --restart \
      --state-file "$SCADA_INSTALL_STATE"
    [ -f "$SCADA_INSTALL_STATE" ] || { echo "[$TIMESTAMP] [ota-apply] SCADA install state is missing" >&2; exit 4; }
    {
      echo "jobId=$JOB_ID"
      echo "version=$VERSION"
      echo "artifact=$ARTIFACT_PATH"
      echo "backupDir=$JOB_BACKUP_DIR"
      echo "backupArtifact=$BACKUP_ARTIFACT"
      echo "workDir=$WORK_DIR"
      echo "appliedAt=$TIMESTAMP"
      echo "packageType=scada"
      cat "$SCADA_INSTALL_STATE"
    } > "$STATE_FILE"
    echo "$VERSION" > "$STAGING_DIR/applied_version.txt"
    clear_in_progress_marker
    prune_old_jobs
    echo "[$TIMESTAMP] [ota-apply] SCADA success jobId=$JOB_ID version=$VERSION" | tee -a "$LOG_FILE"
    exit 0
    ;;
esac

durable_file write "$HEALTH_PENDING_FILE" "jobId=$JOB_ID
version=$VERSION
token=$OTA_JOB_TOKEN
"
python3 "$SCRIPT_DIR/ota-health-result.py" --activate "$STAGING_DIR" "$JOB_ID" "$OTA_JOB_TOKEN"
ACTIVE_REGISTERED=1
MANIFEST_PATH="$WORK_DIR/manifest.json"
if [ ! -f "$MANIFEST_PATH" ]; then
  echo "[ota-apply] root manifest.json is required for non-SCADA packages" >&2
  exit 4
fi

python3 "$(dirname -- "$0")/runtime-upgrade-guard.py" ota "$MANIFEST_PATH"
RESTART_FILE=""
SYSTEMD_RELOAD_FILE=""
CHMOD_FILE=""
if [ -f "$MANIFEST_PATH" ]; then
  RESTART_FILE="$WORK_DIR/restart_services.txt"
  SYSTEMD_RELOAD_FILE="$WORK_DIR/systemd_reload_required"
  CHMOD_FILE="$WORK_DIR/chmod_targets.txt"
  snapshot_previous_state
  # The manifest step records the jobId and writes the in-progress marker itself,
  # after validating every entry and immediately before the first live change.
  OTA_JOB_ID="$JOB_ID" OTA_APPLIED_JOBS_FILE="$APPLIED_JOBS_FILE" OTA_IN_PROGRESS_FILE="$IN_PROGRESS_FILE" \
  OTA_IN_PROGRESS_TEXT="$(in_progress_text)" \
  OTA_RUNTIME_BARRIER_PATH="$OTA_RUNTIME_BARRIER_PATH" OTA_JOB_TOKEN="$OTA_JOB_TOKEN" OTA_JOB_VERSION="$VERSION" \
  python3 - "$MANIFEST_PATH" "$JOB_BACKUP_DIR" "$LOG_FILE" "$RESTART_FILE" "$SYSTEMD_RELOAD_FILE" "$CHMOD_FILE" <<'PY'
import json
import glob
import os
import shutil
import sys
import tempfile

manifest_path, backup_dir, log_file, restart_file, systemd_reload_file, chmod_file = sys.argv[1:7]
root_dir = os.path.dirname(manifest_path)
need_systemd_reload = False
need_gateway_services_restart = False
chmod_targets = []
restart_services = []
restart_service_set = set()

with open(manifest_path, "r", encoding="utf-8") as fh:
    manifest = json.load(fh)

if not isinstance(manifest, dict):
    raise SystemExit("manifest root must be an object")
files = manifest.get("files")
if not isinstance(files, list) or not files:
    raise SystemExit("manifest files must be a non-empty array")
if any(not isinstance(item, dict) for item in files):
    raise SystemExit("manifest files entries must be objects")

allowed_clean_roots = (
    "/opt/modbus-gateway/config/runtime/devices",
    "/opt/modbus-gateway/config/runtime/apps",
    "/opt/modbus-gateway/config/runtime/logic",
    "/opt/modbus-gateway/config/runtime/tls",
    "/opt/modbus-gateway/ky-ems",
)

allowed_bin_targets = {
    "/opt/modbus-gateway/bin/ModbusRtu",
    "/opt/modbus-gateway/bin/Dlt645Driver",
    "/opt/modbus-gateway/bin/DioDriver",
    "/opt/modbus-gateway/bin/CanDriver",
    "/opt/modbus-gateway/bin/IecDriver",
    "/opt/modbus-gateway/bin/MqttDriver",
    "/opt/modbus-gateway/bin/MqttForwarder",
    "/opt/modbus-gateway/bin/EventEngine",
    "/opt/modbus-gateway/bin/ComputeEngine",
    "/opt/modbus-gateway/bin/AgcAvcController",
    "/opt/modbus-gateway/bin/EmsParityCheck",
    "/opt/modbus-gateway/bin/EmsClusterCoordinator",
    "/opt/modbus-gateway/bin/SystemMonitor",
    "/opt/modbus-gateway/bin/LocalDisplay",
    "/opt/modbus-gateway/bin/QtDisplayBridge",
    "/opt/modbus-gateway/bin/CameraService",
    "/opt/modbus-gateway/bin/pointctl",
    "/opt/modbus-gateway/bin/gateway-run.sh",
    "/opt/modbus-gateway/bin/gateway-services.sh",
    "/opt/modbus-gateway/bin/gateway-health-watchdog.sh",
    "/opt/modbus-gateway/bin/install-factory-config.sh",
    "/opt/modbus-gateway/bin/factory-lifecycle-stage.py",
    "/opt/modbus-gateway/bin/local-kiosk.py",
    "/opt/modbus-gateway/bin/production-smoke-test.sh",
    "/opt/modbus-gateway/bin/ota-apply.sh",
    "/opt/modbus-gateway/bin/ota-rollback.sh",
    "/opt/modbus-gateway/bin/ota-health-result.py",
    "/opt/modbus-gateway/bin/runtime-upgrade-guard.py",
    "/opt/modbus-gateway/bin/offline-runtime-upgrade.py",
    "/opt/modbus-gateway/bin/production_role_policy.py",
    "/opt/modbus-gateway/bin/install-scada-project.sh",
}

allowed_systemd_targets = {
    "/etc/default/gateway-health-watchdog",
    "/etc/systemd/system/gateway-services.service",
    "/etc/systemd/system/gateway-health-watchdog.service",
    "/etc/systemd/system/gateway-ota-recover.service",
    "/etc/systemd/system/modbus-rtu@.service",
    "/etc/systemd/system/dlt645-driver@.service",
    "/etc/systemd/system/dio-driver@.service",
    "/etc/systemd/system/can-driver@.service",
    "/etc/systemd/system/compute-engine@.service",
    "/etc/systemd/system/ems-cluster@.service",
    "/etc/systemd/system/agc-avc@.service",
    "/etc/systemd/system/event-engine@.service",
    "/etc/systemd/system/local-display@.service",
    "/etc/systemd/system/local-kiosk@.service",
    "/etc/systemd/system/ky-ems.service",
    "/etc/systemd/system/camera-service@.service",
    "/etc/systemd/system/mqtt-driver@.service",
    "/etc/systemd/system/mqtt-forwarder@.service",
    "/etc/systemd/system/system-monitor@.service",
    "/etc/systemd/system/mqtt-tls-tunnel@.service",
}

# Units that only act at boot must be enabled when an OTA installs them.
ENABLE_ON_INSTALL = {"/etc/systemd/system/gateway-ota-recover.service"}
enable_units = []

allowed_exact_services = (
    "gateway-services.service",
    "gateway-health-watchdog.service",
    "ky-ems.service",
)

allowed_service_prefixes = (
    "modbus-rtu@",
    "dlt645-driver@",
    "dio-driver@",
    "can-driver@",
    "compute-engine@",
    "ems-cluster@",
    "agc-avc@",
    "event-engine@",
    "local-display@",
    "local-kiosk@",
    "camera-service@",
    "mqtt-driver@",
    "mqtt-forwarder@",
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
    if safe_child_path(normalized, "/opt/modbus-gateway/ky-ems"):
        return True
    if safe_child_path(normalized, "/opt/modbus-gateway/bin"):
        return normalized in allowed_bin_targets
    if safe_child_path(normalized, "/etc/systemd/system"):
        return normalized in allowed_systemd_targets
    return False

def is_device_credential(path):
    # Device keys and certificates come only from gateway-tls-enroll.sh on the device (keys never leave it):
    # OTA neither writes nor cleans them. Only stunnel *.conf files and the ticket key list are package files.
    normalized = os.path.abspath(path)
    name = os.path.basename(normalized)
    if safe_child_path(normalized, "/opt/modbus-gateway/config/runtime/tls"):
        return normalized == "/opt/modbus-gateway/config/runtime/tls" or not name.endswith(".conf") \
            or os.path.dirname(normalized) != "/opt/modbus-gateway/config/runtime/tls"
    if safe_child_path(normalized, "/opt/modbus-gateway/config/maintenance-v2"):
        return normalized != "/opt/modbus-gateway/config/maintenance-v2/trusted-ticket-keys.json"
    return False

def needs_gateway_services_restart(dst):
    normalized = os.path.abspath(dst).replace("\\", "/")
    if normalized.startswith("/opt/modbus-gateway/config/runtime/tls/"):
        return True
    if normalized == "/etc/systemd/system/mqtt-tls-tunnel@.service":
        return True
    return False

# Effective defaults mirror OtaConfig in include/edge_gateway/models.hpp.
OTA_FIXED_KEYS = {
    "applyScript": "/opt/modbus-gateway/bin/ota-apply.sh",
    "rollbackScript": "/opt/modbus-gateway/bin/ota-rollback.sh",
    "signaturePublicKeyPath": "/etc/modbus-gateway/ota.pub",
}
OTA_MUST_STAY_ON = {"checksumRequired": True, "requireSignature": True}
OTA_MUST_STAY_OFF = {"allowInsecureHttp": False}

def load_ota_section(path):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            document = json.load(fh)
    except (OSError, ValueError):
        return None
    section = document.get("ota") if isinstance(document, dict) else None
    return section if isinstance(section, dict) else {}

def check_ota_policy(src, dst):
    # A package may tighten OTA security (enable signatures/checksums, disable
    # plain http) but must not weaken it, move the verification key, or point
    # ota.applyScript/rollbackScript at a script it ships itself.
    normalized = os.path.abspath(dst)
    if not (normalized.startswith("/opt/modbus-gateway/config/") and normalized.endswith(".json")):
        return
    new_section = load_ota_section(src)
    if new_section is None:
        return
    old_section = (load_ota_section(dst) if os.path.exists(dst) else {}) or {}
    for key, default in OTA_FIXED_KEYS.items():
        if new_section.get(key, default) != old_section.get(key, default):
            raise SystemExit(f"OTA package may not change ota.{key} in {dst}; use offline provisioning")
    for key, default in OTA_MUST_STAY_ON.items():
        if new_section.get(key, default) is not True and old_section.get(key, default) is True:
            raise SystemExit(f"OTA package may not disable ota.{key} in {dst}; use offline provisioning")
    for key, default in OTA_MUST_STAY_OFF.items():
        if new_section.get(key, default) is not False and old_section.get(key, default) is False:
            raise SystemExit(f"OTA package may not enable ota.{key} in {dst}; use offline provisioning")

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

def target_mode(src, dst):
    # Never inherit setuid/setgid/group-/world-writable bits from the package
    # (Windows/WSL builds are often 0777). Executables (bin/, or an exec bit on
    # the source or the file being replaced, e.g. ky-ems/KY-EMS) get 0755.
    if dst.endswith(".key"):
        return 0o600
    if dst.startswith("/opt/modbus-gateway/bin/"):
        return 0o755
    if os.stat(src).st_mode & 0o111:
        return 0o755
    if os.path.exists(dst) and os.stat(dst).st_mode & 0o111:
        return 0o755
    return 0o644

def atomic_copy2(src, dst, item=None):
    destination_dir = os.path.dirname(dst)
    fd, temporary_path = tempfile.mkstemp(
        prefix=f".{os.path.basename(dst)}.ota-",
        dir=destination_dir,
    )
    os.close(fd)
    try:
        shutil.copyfile(src, temporary_path)
        os.chmod(temporary_path, target_mode(src, dst))
        if item is not None:
            verify_manifest_checksum(item, temporary_path)
        with open(temporary_path, "rb") as fh:
            os.fsync(fh.fileno())
        os.replace(temporary_path, dst)
        try:
            directory_fd = os.open(destination_dir, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
        except OSError:
            pass
    finally:
        if os.path.exists(temporary_path):
            os.unlink(temporary_path)

def is_safe_service_name(value):
    if not value or len(value) > 128 or not value.endswith(".service"):
        return False
    allowed_chars = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.@:-")
    if any(ch not in allowed_chars for ch in value):
        return False
    return value in allowed_exact_services or any(value.startswith(prefix) for prefix in allowed_service_prefixes)

# Validate every entry before the first change to the live runtime.
for item in manifest.get("files", []):
    pre_src = os.path.abspath(os.path.join(root_dir, item["path"]))
    pre_dst = os.path.abspath(item["target"])
    if not safe_child_path(pre_src, root_dir) or not os.path.isfile(pre_src):
        raise SystemExit(f"unsafe or missing manifest source: {item.get('path', '')}")
    if not is_safe_target(pre_dst):
        raise SystemExit(f"unsafe manifest target: {pre_dst}")
    if is_device_credential(pre_dst):
        raise SystemExit(f"OTA package may not install device keys or certificates: {pre_dst}; use gateway-tls-enroll.sh")
    check_ota_policy(pre_src, pre_dst)

def durable_write(path, text, append=False):
    directory = os.path.dirname(path) or "."
    if append:
        with open(path, "a", encoding="utf-8") as fh:
            fh.write(text)
            fh.flush()
            os.fsync(fh.fileno())
    else:
        temporary = path + ".tmp." + str(os.getpid())
        with open(temporary, "w", encoding="utf-8") as fh:
            fh.write(text)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(temporary, path)
    fd = os.open(directory, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(fd)
    finally:
        os.close(fd)

# From here on the live runtime changes: burn the jobId and arm boot recovery.
if os.environ.get("OTA_RUNTIME_BARRIER_PATH"):
    barrier_path = os.environ["OTA_RUNTIME_BARRIER_PATH"]
    expected = dict(format="official-ota-apply-barrier-v1", jobId=os.environ["OTA_JOB_ID"],
                    version=os.environ["OTA_JOB_VERSION"], token=os.environ["OTA_JOB_TOKEN"], state="prepared")
    with open(barrier_path, encoding="utf-8") as source:
        if json.load(source) != expected:
            raise SystemExit("OTA runtime barrier is not prepared")
    expected["state"] = "runtime-may-change"
    durable_write(barrier_path, json.dumps(expected, separators=(",", ":")) + "\n")
durable_write(os.environ["OTA_APPLIED_JOBS_FILE"], os.environ["OTA_JOB_ID"] + "\n", append=True)
durable_write(os.environ["OTA_IN_PROGRESS_FILE"], os.environ["OTA_IN_PROGRESS_TEXT"] + "\n")

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
            if is_device_credential(path):
                log(f"[manifest-clean-skip] device credential {path}")
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
        raise SystemExit(f"unsafe or missing manifest source: {item.get('path', '')}")
    if not is_safe_target(dst) or is_device_credential(dst):
        raise SystemExit(f"unsafe manifest target: {dst}")
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
    # Replacing the inode keeps a running executable alive until its delayed
    # service restart; truncating it in place would fail with ETXTBSY.
    atomic_copy2(src, dst, item)
    if needs_gateway_services_restart(dst):
        need_gateway_services_restart = True
    if dst.startswith("/etc/systemd/system/") and dst.endswith(".service"):
        need_systemd_reload = True
    if dst in ENABLE_ON_INSTALL:
        enable_units.append(os.path.basename(dst))
    if dst.startswith("/opt/modbus-gateway/bin/"):
        chmod_targets.append(dst)
    log(f"[manifest-copy] {src} -> {dst}")

with open(restart_file, "w", encoding="utf-8") as fh:
    for item in manifest.get("restart", {}).get("services", []):
        service = str(item).strip()
        if is_safe_service_name(service):
            if service not in restart_service_set:
                restart_service_set.add(service)
                restart_services.append(service)
        else:
            log(f"[manifest-restart-skip] unsafe service {service}")
if need_gateway_services_restart and "gateway-services.service" not in restart_service_set:
    restart_service_set.add("gateway-services.service")
    restart_services.append("gateway-services.service")
    log("[manifest-restart-add] tls rotation requires gateway-services.service")
with open(restart_file, "w", encoding="utf-8") as fh:
    for service in restart_services:
        fh.write(service + "\n")
if need_systemd_reload:
    with open(systemd_reload_file, "w", encoding="utf-8") as fh:
        fh.write("1\n")
with open(os.path.join(os.path.dirname(restart_file), "enable_units.txt"), "w", encoding="utf-8") as fh:
    for unit in enable_units:
        fh.write(unit + "\n")
with open(chmod_file, "w", encoding="utf-8") as fh:
    for item in chmod_targets:
        fh.write(item + "\n")
PY
fi

ENABLE_UNITS_FILE="$WORK_DIR/enable_units.txt"
if [ -s "$ENABLE_UNITS_FILE" ] && command -v systemctl >/dev/null 2>&1; then
  systemctl daemon-reload >> "$LOG_FILE" 2>&1 || echo "[$TIMESTAMP] [ota-apply] daemon-reload failed" >> "$LOG_FILE"
  while IFS= read -r unit; do
    case "$unit" in
      gateway-ota-recover.service)
        systemctl enable "$unit" >> "$LOG_FILE" 2>&1 || echo "[$TIMESTAMP] [ota-apply] enable failed $unit" >> "$LOG_FILE"
        ;;
    esac
  done < "$ENABLE_UNITS_FILE"
fi

if [ -n "$CHMOD_FILE" ] && [ -f "$CHMOD_FILE" ]; then
  while IFS= read -r target; do
    [ -z "$target" ] && continue
    chmod 0755 "$target" || echo "[$TIMESTAMP] [ota-apply] chmod failed $target" >> "$LOG_FILE"
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
} > "$WORK_DIR/applied_state_pending.txt"
echo "[$TIMESTAMP] [ota-apply] files applied; waiting for restart health jobId=$JOB_ID" | tee -a "$LOG_FILE"

if command -v systemctl >/dev/null 2>&1 && [ -n "$RESTART_FILE" ] && [ -f "$RESTART_FILE" ]; then
  RESTART_LATER="$WORK_DIR/restart_later.sh"
  SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
  ROLLBACK_SCRIPT="$SCRIPT_DIR/ota-rollback.sh"
  GATEWAY_SERVICES_SCRIPT="$SCRIPT_DIR/gateway-services.sh"
  # Post-restart health gate: if restarted units are not active after the
  # verify window, run ota-rollback.sh for this job. OTA_AUTO_ROLLBACK=0 opts out.
  HEALTH_VERIFY_SEC="${OTA_HEALTH_VERIFY_SEC:-30}"
  case "$HEALTH_VERIFY_SEC" in ""|*[!0-9]*) HEALTH_VERIFY_SEC=30 ;; esac
  AUTO_ROLLBACK="${OTA_AUTO_ROLLBACK:-1}"
  [ "$AUTO_ROLLBACK" = "0" ] || AUTO_ROLLBACK=1
  cat > "$RESTART_LATER" <<EOF
#!/bin/sh
WATCHDOG_RUN_DIR="$WATCHDOG_RUN_DIR"
WATCHDOG_APPLYING_FILE="$WATCHDOG_APPLYING_FILE"
cleanup_own_applying_marker() {
  marker_pid=\$(sed -n 's/^pid=//p' "\$WATCHDOG_APPLYING_FILE" 2>/dev/null | sed -n '1p')
  marker_kind=\$(sed -n 's/^kind=//p' "\$WATCHDOG_APPLYING_FILE" 2>/dev/null | sed -n '1p')
  if [ "\$marker_pid" = "\$\$" ] || { [ -z "\$marker_pid" ] && [ "\$marker_kind" = "ota-restart" ]; }; then
    rm -f "\$WATCHDOG_APPLYING_FILE"
  fi
}
restarted=""
restart_failed=0
# At the end of the verification window, a unit still activating has not passed.
unit_ok() {
  unit_state=\$(systemctl show -p ActiveState --value "\$1" 2>/dev/null)
  unit_sub=\$(systemctl show -p SubState --value "\$1" 2>/dev/null)
  case "\$unit_state" in
    active|reloading) return 0 ;;
  esac
  return 1
}
trap cleanup_own_applying_marker EXIT HUP INT TERM
sleep 2
if [ -f "$SYSTEMD_RELOAD_FILE" ]; then
  echo "[$TIMESTAMP] [ota-apply] systemctl daemon-reload" >> "$LOG_FILE"
  systemctl daemon-reload >> "$LOG_FILE" 2>&1 || { echo "[$TIMESTAMP] [ota-apply] daemon-reload failed" >> "$LOG_FILE"; restart_failed=1; }
fi
while IFS= read -r service; do
  [ -z "\$service" ] && continue
  case "\$service" in
    gateway-services.service|gateway-health-watchdog.service|modbus-rtu@*.service|dlt645-driver@*.service|dio-driver@*.service|can-driver@*.service|compute-engine@*.service|ems-cluster@*.service|agc-avc@*.service|event-engine@*.service|local-display@*.service|local-kiosk@*.service|ky-ems.service|camera-service@*.service|mqtt-driver@*.service|mqtt-forwarder@*.service|system-monitor@*.service|mqtt-tls-tunnel@*.service) ;;
    *)
      echo "[$TIMESTAMP] [ota-apply] skip unsafe restart service \$service" >> "$LOG_FILE"
      continue
      ;;
  esac
  if [ "\$service" = "gateway-services.service" ]; then
    systemctl enable "\$service" >> "$LOG_FILE" 2>&1 || echo "[$TIMESTAMP] [ota-apply] enable failed \$service" >> "$LOG_FILE"
    echo "[$TIMESTAMP] [ota-apply] reloading or starting \$service" >> "$LOG_FILE"
    systemctl reload-or-restart "\$service" >> "$LOG_FILE" 2>&1 || { echo "[$TIMESTAMP] [ota-apply] reload-or-restart failed \$service" >> "$LOG_FILE"; restart_failed=1; }
    restarted="\$restarted \$service"
    continue
  fi
  echo "[$TIMESTAMP] [ota-apply] restarting \$service" >> "$LOG_FILE"
  systemctl restart "\$service" >> "$LOG_FILE" 2>&1 || { echo "[$TIMESTAMP] [ota-apply] restart failed \$service" >> "$LOG_FILE"; restart_failed=1; }
  restarted="\$restarted \$service"
done < "$RESTART_FILE"
if [ -n "\$restarted" ]; then
  sleep $HEALTH_VERIFY_SEC
  unhealthy=""
  for service in \$restarted; do
    # Display units are started --no-block and may legitimately wait for X.
    case "\$service" in ky-ems.service|local-kiosk@*.service) continue ;; esac
    unit_ok "\$service" || unhealthy="\$unhealthy \$service"
    if [ "\$service" = "gateway-services.service" ] && [ -x "$GATEWAY_SERVICES_SCRIPT" ]; then
      for unit in \$("$GATEWAY_SERVICES_SCRIPT" list 2>/dev/null); do
        case "\$unit" in ""|\#*|ky-ems.service|local-kiosk@*.service) continue ;; esac
        unit_ok "\$unit" || unhealthy="\$unhealthy \$unit"
      done
    fi
  done
  if [ -n "\$unhealthy" ] || [ "\$restart_failed" = "1" ]; then
    echo "[$TIMESTAMP] [ota-apply] unhealthy after restart:\$unhealthy; rolling back jobId=$JOB_ID" >> "$LOG_FILE"
    if [ "$AUTO_ROLLBACK" = "1" ]; then
      if OTA_ROLLBACK_REASON=unhealthy-after-restart sh "$ROLLBACK_SCRIPT" "$ARTIFACT_PATH" "$VERSION" "$JOB_ID" "$BACKUP_DIR" "$STAGING_DIR" "$OTA_JOB_TOKEN" >> "$LOG_FILE" 2>&1; then
        python3 "$SCRIPT_DIR/ota-health-result.py" "$STAGING_DIR" "$JOB_ID" "$VERSION" failed succeeded
      else
        python3 "$SCRIPT_DIR/ota-health-result.py" "$STAGING_DIR" "$JOB_ID" "$VERSION" failed failed
      fi
    else
      python3 "$SCRIPT_DIR/ota-health-result.py" "$STAGING_DIR" "$JOB_ID" "$VERSION" failed skipped
    fi
  else
    rm -f "$IN_PROGRESS_FILE"
    python3 "$SCRIPT_DIR/ota-health-result.py" "$STAGING_DIR" "$JOB_ID" "$VERSION" healthy none
  fi
else
  rm -f "$IN_PROGRESS_FILE"
  python3 "$SCRIPT_DIR/ota-health-result.py" "$STAGING_DIR" "$JOB_ID" "$VERSION" healthy none
fi
EOF
  chmod +x "$RESTART_LATER"
  RESTART_PID=""
  # Run outside the caller's cgroup (MqttDriver's unit): restarting
  # mqtt-driver@ must not SIGKILL the restart sequence halfway through.
  RESTART_UNIT="ota-restart-${JOB_ID}-$$"
  if command -v systemd-run >/dev/null 2>&1 &&
     systemd-run --unit="$RESTART_UNIT" --collect --quiet /bin/sh "$RESTART_LATER" >/dev/null 2>&1; then
    RESTART_PID="$(systemctl show -p MainPID --value "$RESTART_UNIT.service" 2>/dev/null || true)"
    case "$RESTART_PID" in ""|0|*[!0-9]*) RESTART_PID="" ;; esac
    echo "[$TIMESTAMP] [ota-apply] restart sequence started as $RESTART_UNIT.service" >> "$LOG_FILE"
  else
    nohup sh "$RESTART_LATER" >/dev/null 2>&1 &
    RESTART_PID=$!
  fi
  write_applying_marker_for ota-restart "$RESTART_PID"
  KEEP_APPLYING_MARKER=1
else
  clear_in_progress_marker
  python3 "$SCRIPT_DIR/ota-health-result.py" "$STAGING_DIR" "$JOB_ID" "$VERSION" healthy none
fi

exit 0
