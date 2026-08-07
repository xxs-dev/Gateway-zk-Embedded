#!/bin/sh
set -eu

REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
INSTALL_ROOT="/opt/modbus-gateway"
TEMP_ROOT="$(mktemp -d /tmp/ota-self-upgrade.XXXXXX)"
RUNTIME_PID=""

cleanup() {
  if [ -n "$RUNTIME_PID" ]; then
    kill "$RUNTIME_PID" 2>/dev/null || true
    wait "$RUNTIME_PID" 2>/dev/null || true
  fi
  if [ "$(realpath -m "$INSTALL_ROOT")" = "/opt/modbus-gateway" ]; then
    rm -rf -- "$INSTALL_ROOT"
  fi
  case "$TEMP_ROOT" in
    /tmp/ota-self-upgrade.*) rm -rf -- "$TEMP_ROOT" ;;
  esac
}
trap cleanup EXIT INT TERM

if [ "$(id -u)" -ne 0 ]; then
  echo "ota_apply_self_upgrade_test must run as root" >&2
  exit 1
fi
if [ -e "$INSTALL_ROOT" ]; then
  echo "$INSTALL_ROOT already exists; refusing to run destructive disposable test" >&2
  exit 1
fi

mkdir -p \
  "$INSTALL_ROOT/bin" \
  "$INSTALL_ROOT/ota/downloads" \
  "$INSTALL_ROOT/ota/backup" \
  "$INSTALL_ROOT/ota/staging" \
  "$TEMP_ROOT/package/deploy"

git -c safe.directory="$REPO_ROOT" -C "$REPO_ROOT" show HEAD:deploy/ota-apply.sh \
  > "$INSTALL_ROOT/bin/ota-apply.sh"
chmod 0755 "$INSTALL_ROOT/bin/ota-apply.sh"
cp "$REPO_ROOT/deploy/ota-apply.sh" "$TEMP_ROOT/package/deploy/ota-apply.sh"
chmod 0755 "$TEMP_ROOT/package/deploy/ota-apply.sh"

new_sha="$(sha256sum "$TEMP_ROOT/package/deploy/ota-apply.sh")"
new_sha="${new_sha%% *}"
python3 - "$TEMP_ROOT/package/manifest.json" "$new_sha" <<'PY'
import json
import sys

path, digest = sys.argv[1:]
with open(path, "w", encoding="utf-8") as stream:
    json.dump(
        {
            "packageType": "config",
            "version": "self-upgrade-test",
            "files": [
                {
                    "path": "deploy/ota-apply.sh",
                    "target": "/opt/modbus-gateway/bin/ota-apply.sh",
                    "sha256": digest,
                }
            ],
            "restart": {"services": []},
        },
        stream,
    )
PY

artifact="$INSTALL_ROOT/ota/downloads/self-upgrade-test.tar.gz"
tar -C "$TEMP_ROOT/package" -czf "$artifact" manifest.json deploy/ota-apply.sh
"$INSTALL_ROOT/bin/ota-apply.sh" \
  "$artifact" \
  self-upgrade-test \
  OTA_SELF_UPGRADE_TEST \
  "$INSTALL_ROOT/ota/backup" \
  "$INSTALL_ROOT/ota/staging"

actual_sha="$(sha256sum "$INSTALL_ROOT/bin/ota-apply.sh")"
actual_sha="${actual_sha%% *}"
test "$actual_sha" = "$new_sha"
test -f "$INSTALL_ROOT/ota/staging/current_version.txt"
test "$(cat "$INSTALL_ROOT/ota/staging/applied_version.txt")" = "self-upgrade-test"

printf 'ota apply self-upgrade passed\nsha256=%s\n' "$actual_sha"
tail -n 8 "$INSTALL_ROOT/ota/staging/upgrade_history.log"

cp /usr/bin/python3 "$INSTALL_ROOT/bin/ModbusRtu"
chmod 0755 "$INSTALL_ROOT/bin/ModbusRtu"
old_binary_sha="$(sha256sum "$INSTALL_ROOT/bin/ModbusRtu")"
old_binary_sha="${old_binary_sha%% *}"
"$INSTALL_ROOT/bin/ModbusRtu" -c 'import time; time.sleep(60)' &
RUNTIME_PID="$!"
sleep 1
kill -0 "$RUNTIME_PID"

mkdir -p "$TEMP_ROOT/running-package/bin"
cp /bin/bash "$TEMP_ROOT/running-package/bin/ModbusRtu"
chmod 0777 "$TEMP_ROOT/running-package/bin/ModbusRtu"
new_binary_sha="$(sha256sum "$TEMP_ROOT/running-package/bin/ModbusRtu")"
new_binary_sha="${new_binary_sha%% *}"
cp /bin/true "$TEMP_ROOT/running-package/bin/EventEngine"
python3 - "$TEMP_ROOT/running-package/manifest.json" "$new_binary_sha" <<'PY'
import json
import sys

path, digest = sys.argv[1:]
with open(path, "w", encoding="utf-8") as stream:
    json.dump(
        {
            "packageType": "config",
            "version": "failed-running-binary-test",
            "files": [
                {
                    "path": "bin/ModbusRtu",
                    "target": "/opt/modbus-gateway/bin/ModbusRtu",
                    "sha256": digest,
                },
                {
                    "path": "bin/EventEngine",
                    "target": "/opt/modbus-gateway/bin/EventEngine",
                    "sha256": "0" * 64,
                },
            ],
            "restart": {"services": []},
        },
        stream,
    )
PY

failed_artifact="$INSTALL_ROOT/ota/downloads/failed-running-binary-test.tar.gz"
tar -C "$TEMP_ROOT/running-package" -czf "$failed_artifact" manifest.json bin/ModbusRtu bin/EventEngine
if "$INSTALL_ROOT/bin/ota-apply.sh" \
  "$failed_artifact" \
  failed-running-binary-test \
  OTA_FAILED_RUNNING_BINARY_TEST \
  "$INSTALL_ROOT/ota/backup" \
  "$INSTALL_ROOT/ota/staging"; then
  echo "expected the second manifest checksum to fail" >&2
  exit 1
fi
test "$(sha256sum "$INSTALL_ROOT/bin/ModbusRtu" | awk '{print $1}')" = "$new_binary_sha"
test "$(awk -F= '/^jobId=/{print $2}' "$INSTALL_ROOT/ota/staging/current_version.txt")" = "OTA_SELF_UPGRADE_TEST"
"$REPO_ROOT/deploy/ota-rollback.sh" \
  "$failed_artifact" \
  failed-running-binary-test \
  OTA_FAILED_RUNNING_BINARY_TEST \
  "$INSTALL_ROOT/ota/backup" \
  "$INSTALL_ROOT/ota/staging"
test "$(sha256sum "$INSTALL_ROOT/bin/ModbusRtu" | awk '{print $1}')" = "$old_binary_sha"
test "$(sha256sum "$INSTALL_ROOT/bin/ota-apply.sh" | awk '{print $1}')" = "$new_sha"
kill -0 "$RUNTIME_PID"

python3 - "$TEMP_ROOT/running-package/manifest.json" "$new_binary_sha" <<'PY'
import json
import sys

path, digest = sys.argv[1:]
with open(path, "w", encoding="utf-8") as stream:
    json.dump(
        {
            "packageType": "config",
            "version": "running-binary-test",
            "files": [
                {
                    "path": "bin/ModbusRtu",
                    "target": "/opt/modbus-gateway/bin/ModbusRtu",
                    "sha256": digest,
                }
            ],
            "restart": {"services": []},
        },
        stream,
    )
PY

running_artifact="$INSTALL_ROOT/ota/downloads/running-binary-test.tar.gz"
tar -C "$TEMP_ROOT/running-package" -czf "$running_artifact" manifest.json bin/ModbusRtu
"$INSTALL_ROOT/bin/ota-apply.sh" \
  "$running_artifact" \
  running-binary-test \
  OTA_RUNNING_BINARY_TEST \
  "$INSTALL_ROOT/ota/backup" \
  "$INSTALL_ROOT/ota/staging"

test "$(sha256sum "$INSTALL_ROOT/bin/ModbusRtu" | awk '{print $1}')" = "$new_binary_sha"
test "$(stat -c '%a' "$INSTALL_ROOT/bin/ModbusRtu")" = "755"
kill -0 "$RUNTIME_PID"
kill "$RUNTIME_PID"
wait "$RUNTIME_PID" 2>/dev/null || true
RUNTIME_PID=""

"$INSTALL_ROOT/bin/ModbusRtu" -c 'sleep 60' &
RUNTIME_PID="$!"
sleep 1
kill -0 "$RUNTIME_PID"
"$REPO_ROOT/deploy/ota-rollback.sh" \
  "$running_artifact" \
  running-binary-test \
  OTA_RUNNING_BINARY_TEST \
  "$INSTALL_ROOT/ota/backup" \
  "$INSTALL_ROOT/ota/staging"

test "$(sha256sum "$INSTALL_ROOT/bin/ModbusRtu" | awk '{print $1}')" = "$old_binary_sha"
kill -0 "$RUNTIME_PID"
printf 'ota running binary apply and rollback passed\n'
