#!/bin/sh
set -eu

REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
INSTALL_ROOT=/opt/modbus-gateway
TEMP_ROOT="$(mktemp -d /tmp/ota-self-upgrade.XXXXXX)"
export WATCHDOG_RUN_DIR="$TEMP_ROOT/watchdog"
RUNTIME_PID=""
INSTALL_CREATED=0
cleanup() {
  if [ -n "$RUNTIME_PID" ]; then
    kill "$RUNTIME_PID" 2>/dev/null || true
    wait "$RUNTIME_PID" 2>/dev/null || true
  fi
  if [ "$INSTALL_CREATED" = 1 ] && [ -d "$INSTALL_ROOT" ] && [ ! -L "$INSTALL_ROOT" ] &&
     [ "$(realpath -e -- "$INSTALL_ROOT")" = /opt/modbus-gateway ]; then
    rm -rf -- "$INSTALL_ROOT"
  fi
  case "$TEMP_ROOT" in /tmp/ota-self-upgrade.*) rm -rf -- "$TEMP_ROOT" ;; esac
}
trap cleanup EXIT INT TERM

[ "$(id -u)" -eq 0 ] || { echo "requires isolated root namespace" >&2; exit 1; }
[ ! -e "$INSTALL_ROOT" ] && [ ! -L "$INSTALL_ROOT" ] || { echo "installation path must be disposable" >&2; exit 1; }
mkdir -p "$INSTALL_ROOT/bin" "$INSTALL_ROOT/ota/downloads" "$INSTALL_ROOT/ota/backup" \
  "$INSTALL_ROOT/ota/staging" "$TEMP_ROOT/package/deploy"
INSTALL_CREATED=1

# Explicit baseline: a packaged OTA script must have its co-located helper.
for name in ota-apply.sh ota-rollback.sh ota-health-result.py runtime-upgrade-guard.py; do
  cp "$REPO_ROOT/deploy/$name" "$INSTALL_ROOT/bin/$name"
  chmod 0755 "$INSTALL_ROOT/bin/$name"
done
cp "$REPO_ROOT/deploy/ota-apply.sh" "$TEMP_ROOT/package/deploy/ota-apply.sh"
printf '\n# Self-upgrade test payload.\n' >> "$TEMP_ROOT/package/deploy/ota-apply.sh"
chmod 0755 "$TEMP_ROOT/package/deploy/ota-apply.sh"

new_sha="$(sha256sum "$TEMP_ROOT/package/deploy/ota-apply.sh")"
new_sha="${new_sha%% *}"
python3 - "$TEMP_ROOT/package/manifest.json" "$new_sha" <<'PY'
import json
import sys

path, digest = sys.argv[1:]
with open(path, "w", encoding="utf-8") as stream:
    json.dump({"packageType": "config", "version": "self-upgrade-test",
               "files": [{"path": "deploy/ota-apply.sh",
                          "target": "/opt/modbus-gateway/bin/ota-apply.sh",
                          "sha256": digest}],
               "restart": {"services": []}}, stream)
PY

artifact="$INSTALL_ROOT/ota/downloads/self-upgrade-test.tar.gz"
tar -C "$TEMP_ROOT/package" -czf "$artifact" manifest.json deploy/ota-apply.sh
"$INSTALL_ROOT/bin/ota-apply.sh" "$artifact" self-upgrade-test OTA_SELF_UPGRADE_TEST \
  "$INSTALL_ROOT/ota/backup" "$INSTALL_ROOT/ota/staging"

for i in $(seq 1 100); do
  [ -s "$INSTALL_ROOT/ota/staging/health_OTA_SELF_UPGRADE_TEST.txt" ] && break
  sleep 0.05
done
python3 - "$INSTALL_ROOT/ota/staging/health_OTA_SELF_UPGRADE_TEST.txt" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    result = json.load(stream)
assert result["jobId"] == "OTA_SELF_UPGRADE_TEST", result
assert result["status"] == "healthy", result
PY
actual_sha="$(sha256sum "$INSTALL_ROOT/bin/ota-apply.sh")"
actual_sha="${actual_sha%% *}"
test "$actual_sha" = "$new_sha"
test "$(cat "$INSTALL_ROOT/ota/staging/applied_version.txt")" = self-upgrade-test
test "$(sed -n 's/^jobId=//p' "$INSTALL_ROOT/ota/staging/current_version.txt")" = OTA_SELF_UPGRADE_TEST
test "$(sed -n 's/^version=//p' "$INSTALL_ROOT/ota/staging/current_version.txt")" = self-upgrade-test
printf 'ota apply self-upgrade passed\nsha256=%s\n' "$actual_sha"

# The current guard refuses online replacement of a running runtime ELF.
cp /bin/bash "$INSTALL_ROOT/bin/ModbusRtu"
chmod 0755 "$INSTALL_ROOT/bin/ModbusRtu"
old_binary_sha="$(sha256sum "$INSTALL_ROOT/bin/ModbusRtu")"
old_binary_sha="${old_binary_sha%% *}"
"$INSTALL_ROOT/bin/ModbusRtu" -c 'sleep 30' &
RUNTIME_PID="$!"
sleep 0.1
kill -0 "$RUNTIME_PID"
mkdir -p "$TEMP_ROOT/runtime-package/bin"
cp /bin/true "$TEMP_ROOT/runtime-package/bin/ModbusRtu"
new_binary_sha="$(sha256sum "$TEMP_ROOT/runtime-package/bin/ModbusRtu")"
new_binary_sha="${new_binary_sha%% *}"
python3 - "$TEMP_ROOT/runtime-package/manifest.json" "$new_binary_sha" <<'PY'
import json
import sys

path, digest = sys.argv[1:]
with open(path, "w", encoding="utf-8") as stream:
    json.dump({"packageType": "config", "version": "runtime-elf-refusal-test",
               "files": [{"path": "bin/ModbusRtu",
                          "target": "/opt/modbus-gateway/bin/ModbusRtu",
                          "sha256": digest}],
               "restart": {"services": []}}, stream)
PY
runtime_artifact="$INSTALL_ROOT/ota/downloads/runtime-elf-refusal-test.tar.gz"
tar -C "$TEMP_ROOT/runtime-package" -czf "$runtime_artifact" manifest.json bin/ModbusRtu
if "$INSTALL_ROOT/bin/ota-apply.sh" "$runtime_artifact" runtime-elf-refusal-test OTA_RUNTIME_ELF_REFUSAL \
  "$INSTALL_ROOT/ota/backup" "$INSTALL_ROOT/ota/staging"; then
  echo "online runtime ELF replacement must be rejected" >&2
  exit 1
fi
actual_binary_sha="$(sha256sum "$INSTALL_ROOT/bin/ModbusRtu")"
actual_binary_sha="${actual_binary_sha%% *}"
test "$actual_binary_sha" = "$old_binary_sha"
kill -0 "$RUNTIME_PID"
test "$(sed -n 's/^jobId=//p' "$INSTALL_ROOT/ota/staging/current_version.txt")" = OTA_SELF_UPGRADE_TEST
test "$(cat "$INSTALL_ROOT/ota/staging/applied_version.txt")" = self-upgrade-test
printf 'online runtime ELF refusal preserved binary, process, and version\n'
