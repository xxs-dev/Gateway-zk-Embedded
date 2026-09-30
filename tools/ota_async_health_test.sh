#!/bin/sh
set -eu

REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
ROOT="$(mktemp -d /tmp/ota-async-health.XXXXXX)"
INSTALL=/opt/modbus-gateway
INSTALL_CREATED=0
cleanup() {
  result=$?
  if [ "$result" -ne 0 ] && [ -f "$ROOT/staging/upgrade_history.log" ]; then
    tail -n 20 "$ROOT/staging/upgrade_history.log" >&2
  fi
  case "$ROOT" in /tmp/ota-async-health.*) rm -rf -- "$ROOT" ;; esac
  if [ "$INSTALL_CREATED" = 1 ] && [ -d "$INSTALL" ] && [ ! -L "$INSTALL" ] &&
     [ "$(realpath -e -- "$INSTALL")" = /opt/modbus-gateway ]; then
    rm -rf -- "$INSTALL"
  fi
}
trap cleanup EXIT INT TERM
[ "$(id -u)" -eq 0 ] || { echo "requires isolated root namespace" >&2; exit 1; }
[ ! -e "$INSTALL" ] && [ ! -L "$INSTALL" ] || { echo "installation path must be disposable" >&2; exit 1; }
mkdir -p "$INSTALL/config" "$INSTALL/bin" "$ROOT/bin" "$ROOT/package/config" "$ROOT/staging" "$ROOT/backup" "$ROOT/watchdog"
INSTALL_CREATED=1
for name in ota-apply.sh ota-rollback.sh ota-health-result.py runtime-upgrade-guard.py; do
  cp "$REPO_ROOT/deploy/$name" "$INSTALL/bin/$name"
  chmod 0755 "$INSTALL/bin/$name"
done
APPLY="$INSTALL/bin/ota-apply.sh"
ROLLBACK="$INSTALL/bin/ota-rollback.sh"
printf 'old\n' > "$INSTALL/config/ota-health-test.txt"
printf 'jobId=OLD_JOB\nversion=old\n' > "$ROOT/staging/current_version.txt"
printf 'old\n' > "$ROOT/staging/applied_version.txt"
printf 'new\n' > "$ROOT/package/config/ota-health-test.txt"
python3 - "$ROOT/package" <<'PY'
import hashlib, json, os, sys
root = sys.argv[1]
path = os.path.join(root, "config/ota-health-test.txt")
with open(path, "rb") as source:
    digest = hashlib.sha256(source.read()).hexdigest()
with open(os.path.join(root, "manifest.json"), "w", encoding="utf-8") as output:
    json.dump({"packageType": "config", "version": "health-test",
               "files": [{"path": "config/ota-health-test.txt",
                          "target": "/opt/modbus-gateway/config/ota-health-test.txt",
                          "sha256": digest}],
               "restart": {"services": ["mqtt-driver@test.service"]}}, output)
PY
tar -C "$ROOT/package" -czf "$ROOT/health-test.tar.gz" manifest.json config/ota-health-test.txt
cat > "$ROOT/bin/systemctl" <<'SH'
#!/bin/sh
case "$1" in
  restart|reload-or-restart)
    if [ -f "${FAKE_FAIL_MARKER:-/nonexistent}" ]; then
      rm -f "$FAKE_FAIL_MARKER"
      exit 1
    fi ;;
  show)
    case "$3" in
      ActiveState)
        if [ "${FAKE_ACTIVATING:-0}" = 1 ]; then printf 'activating\n'; else printf 'active\n'; fi ;;
      SubState) printf 'running\n' ;;
      MainPID) printf '0\n' ;;
    esac ;;
  *) exit 0 ;;
esac
SH
cat > "$ROOT/bin/systemd-run" <<'SH'
#!/bin/sh
exit 1
SH
chmod +x "$ROOT/bin/systemctl" "$ROOT/bin/systemd-run"
export PATH="$ROOT/bin:$PATH" WATCHDOG_RUN_DIR="$ROOT/watchdog" OTA_HEALTH_VERIFY_SEC=0
export FAKE_FAIL_MARKER="$ROOT/fail-first-restart"
: > "$FAKE_FAIL_MARKER"
mv "$INSTALL/bin/ota-health-result.py" "$ROOT/helper-held.py"
if sh "$APPLY" "$ROOT/health-test.tar.gz" health-test MISSING_HELPER "$ROOT/backup" "$ROOT/staging"; then
  echo "missing helper must reject apply" >&2
  exit 1
fi
test "$(cat "$INSTALL/config/ota-health-test.txt")" = old
test ! -e "$ROOT/staging/MISSING_HELPER"
mv "$ROOT/helper-held.py" "$INSTALL/bin/ota-health-result.py"
sh "$APPLY" "$ROOT/health-test.tar.gz" health-test FAIL_JOB "$ROOT/backup" "$ROOT/staging"
test "$(cat "$ROOT/staging/applied_version.txt")" = old
test "$(sed -n 's/^version=//p' "$ROOT/staging/current_version.txt")" = old
for i in $(seq 1 100); do
  [ -s "$ROOT/staging/health_FAIL_JOB.txt" ] && break
  sleep 0.05
done
python3 - "$ROOT/staging/health_FAIL_JOB.txt" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as source:
    value = json.load(source)
assert value["status"] == "failed", value
assert value["rollback"] == "succeeded", value
assert value["jobId"] == "FAIL_JOB", value
PY
test "$(cat "$INSTALL/config/ota-health-test.txt")" = old
test "$(cat "$ROOT/staging/applied_version.txt")" = old
test "$(sed -n 's/^version=//p' "$ROOT/staging/current_version.txt")" = old
sh "$APPLY" "$ROOT/health-test.tar.gz" health-test GOOD_JOB "$ROOT/backup" "$ROOT/staging"
test "$(cat "$ROOT/staging/applied_version.txt")" = old
for i in $(seq 1 100); do
  [ -s "$ROOT/staging/health_GOOD_JOB.txt" ] && break
  sleep 0.05
done
python3 - "$ROOT/staging/health_GOOD_JOB.txt" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as source:
    value = json.load(source)
assert value["status"] == "healthy", value
assert value["rollback"] == "none", value
assert value["jobId"] == "GOOD_JOB", value
PY
test "$(cat "$INSTALL/config/ota-health-test.txt")" = new
test "$(cat "$ROOT/staging/applied_version.txt")" = health-test
test "$(sed -n 's/^jobId=//p' "$ROOT/staging/current_version.txt")" = GOOD_JOB
test "$(sed -n 's/^version=//p' "$ROOT/staging/current_version.txt")" = health-test
mkdir -p "$ROOT/staging/LATE_JOB"
printf 'jobId=LATE_JOB\nversion=stale\ntoken=old-token\n' > "$ROOT/staging/LATE_JOB/health_pending.txt"
printf 'jobId=LATE_JOB\nversion=stale\n' > "$ROOT/staging/LATE_JOB/applied_state_pending.txt"
printf 'jobId=NEWER_JOB\ntoken=new-token\n' > "$ROOT/staging/ota_active_job.txt"
if python3 "$INSTALL/bin/ota-health-result.py" "$ROOT/staging" LATE_JOB stale healthy none > "$ROOT/late-result.log" 2>&1; then
  echo "late old health must not overwrite a newer active job" >&2
  exit 1
fi
test ! -e "$ROOT/staging/health_LATE_JOB.txt"
test "$(sed -n 's/^jobId=//p' "$ROOT/staging/current_version.txt")" = GOOD_JOB
test "$(cat "$ROOT/staging/applied_version.txt")" = health-test
rm -f "$ROOT/staging/ota_active_job.txt"
export FAKE_ACTIVATING=1
sh "$APPLY" "$ROOT/health-test.tar.gz" health-test ACTIVATING_JOB "$ROOT/backup" "$ROOT/staging"
for i in $(seq 1 100); do
  [ -s "$ROOT/staging/health_ACTIVATING_JOB.txt" ] && break
  sleep 0.05
done
python3 - "$ROOT/staging/health_ACTIVATING_JOB.txt" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as source:
    value = json.load(source)
assert value["status"] == "failed", value
assert value["rollback"] == "succeeded", value
PY
test "$(cat "$ROOT/staging/applied_version.txt")" = health-test
test "$(sed -n 's/^jobId=//p' "$ROOT/staging/current_version.txt")" = GOOD_JOB
unset FAKE_ACTIVATING
export OTA_HEALTH_VERIFY_SEC=20
sh "$APPLY" "$ROOT/health-test.tar.gz" health-test INTERRUPTED_JOB "$ROOT/backup" "$ROOT/staging"
restart_pid="$(sed -n 's/^pid=//p' "$ROOT/watchdog/applying" | sed -n '1p')"
case "$restart_pid" in ''|0|*[!0-9]*) echo "missing restart PID" >&2; exit 1 ;; esac
kill "$restart_pid"
sleep 0.2
sh "$ROLLBACK" --recover-interrupted "$ROOT/staging"
python3 - "$ROOT/staging/health_INTERRUPTED_JOB.txt" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as source:
    value = json.load(source)
assert value["status"] == "failed", value
assert value["rollback"] == "succeeded", value
assert value["jobId"] == "INTERRUPTED_JOB", value
PY
test "$(cat "$INSTALL/config/ota-health-test.txt")" = new
printf 'ota async health test passed\n'
