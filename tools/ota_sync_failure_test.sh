#!/bin/sh
set -eu

REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
ROOT="$(mktemp -d /tmp/ota-sync-failure.XXXXXX)"
INSTALL=/opt/modbus-gateway
INSTALL_CREATED=0
cleanup() {
  result=$?
  if [ "$result" -ne 0 ]; then
    find "$ROOT" -name upgrade_history.log -exec tail -n 20 {} \; >&2
  fi
  case "$ROOT" in /tmp/ota-sync-failure.*) rm -rf -- "$ROOT" ;; esac
  if [ "$INSTALL_CREATED" = 1 ] && [ -d "$INSTALL" ] && [ ! -L "$INSTALL" ] &&
     [ "$(realpath -e -- "$INSTALL")" = /opt/modbus-gateway ]; then
    rm -rf -- "$INSTALL"
  fi
}
trap cleanup EXIT INT TERM
[ "$(id -u)" -eq 0 ] || { echo "requires isolated root namespace" >&2; exit 1; }
[ ! -e "$INSTALL" ] && [ ! -L "$INSTALL" ] || { echo "installation path must be disposable" >&2; exit 1; }
mkdir -p "$INSTALL/bin" "$INSTALL/config" "$ROOT/watchdog" "$ROOT/package"
INSTALL_CREATED=1
for name in ota-apply.sh ota-rollback.sh ota-health-result.py runtime-upgrade-guard.py; do
  cp "$REPO_ROOT/deploy/$name" "$INSTALL/bin/$name"
  chmod 0755 "$INSTALL/bin/$name"
done
printf 'unchanged\n' > "$INSTALL/config/sync-failure-test.txt"
printf '{broken\n' > "$ROOT/package/manifest.json"
tar -C "$ROOT/package" -czf "$ROOT/invalid.tar.gz" manifest.json
export WATCHDOG_RUN_DIR="$ROOT/watchdog"

run_rejection() {
  job="$1"
  token="00000000-0000-4000-8000-000000000001"
  staging="$ROOT/$job/staging"
  backup="$ROOT/$job/backup"
  mkdir -p "$staging" "$backup"
  printf 'jobId=%s\ntoken=%s\n' "$job" "$token" > "$staging/ota_active_job.txt"
  printf '{"format":"ota-job-v1","jobId":"%s","machineCode":"GW_TEST","version":"2.0.0","artifactPath":"%s","previousVersion":"1.0.0","token":"%s"}\n' \
    "$job" "$ROOT/invalid.tar.gz" "$token" > "$staging/ota_job_$job.json"
  if sh "$INSTALL/bin/ota-apply.sh" "$ROOT/invalid.tar.gz" 2.0.0 "$job" "$backup" "$staging" "$token"; then
    echo "$job unexpectedly applied" >&2
    exit 1
  fi
  python3 - "$staging/health_$job.txt" "$job" "$token" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as source:
    result = json.load(source)
assert result["jobId"] == sys.argv[2] and result["version"] == "2.0.0"
assert result["token"] == sys.argv[3]
assert result["status"] == "failed" and result["rollback"] == "unchanged"
PY
  test ! -e "$staging/ota-in-progress"
  test ! -e "$staging/applied_jobs.txt"
  test ! -e "$staging/$job"
  test ! -e "$backup/$job"
  test "$(cat "$INSTALL/config/sync-failure-test.txt")" = unchanged
  echo "$job: unchanged proof accepted"
}

run_rejection INVALID_MANIFEST
mv "$INSTALL/bin/ota-health-result.py" "$ROOT/helper-held.py"
run_rejection MISSING_HELPER
mv "$ROOT/helper-held.py" "$INSTALL/bin/ota-health-result.py"
printf 'ota sync-failure script test passed\n'
