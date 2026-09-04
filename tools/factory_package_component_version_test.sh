#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP_ROOT="${TMPDIR:-/tmp}/factory-package-version-test.$$"

cleanup() {
  rm -rf "$TMP_ROOT"
}
trap cleanup EXIT INT TERM

mkdir -p "$TMP_ROOT/source/build-aarch64" "$TMP_ROOT/source/ky-ems" \
  "$TMP_ROOT/source/config/factory/runtime/apps" "$TMP_ROOT/source/config/factory/runtime/devices"

for binary in SystemMonitor MqttDriver MqttForwarder pointctl QtDisplayBridge KY-EMS; do
  if [ "$binary" = "KY-EMS" ]; then
    printf 'test\n' > "$TMP_ROOT/source/ky-ems/KY-EMS"
  else
    printf 'test\n' > "$TMP_ROOT/source/build-aarch64/$binary"
  fi
done

cat > "$TMP_ROOT/source/config/factory/runtime/apps/monitor-service.json" <<'JSON'
{"runtimeMode":"gateway","deviceConfigFiles":[]}
JSON
cat > "$TMP_ROOT/source/config/factory/runtime/apps/mqtt-service.json" <<'JSON'
{"runtimeMode":"gateway","deviceConfigFiles":[]}
JSON
cat > "$TMP_ROOT/manifest.json" <<'JSON'
{
  "packageProfile": "project",
  "requiredDrivers": [
    {"binary":"KY-EMS","version":"2.0.9-compact"}
  ]
}
JSON

cp -a "$ROOT_DIR/deploy" "$TMP_ROOT/source/deploy"
(
  cd "$TMP_ROOT/source"
  sh deploy/build-factory-package.sh "$TMP_ROOT/package.tar.gz" \
    --profile project --manifest "$TMP_ROOT/manifest.json"
)

mkdir "$TMP_ROOT/unpacked"
tar -xzf "$TMP_ROOT/package.tar.gz" -C "$TMP_ROOT/unpacked"
python3 - "$TMP_ROOT/unpacked/gateway-factory-defaults/edge-package-manifest.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as fh:
    manifest = json.load(fh)

required = {item["binary"]: item["version"] for item in manifest["requiredDrivers"]}
components = {item["binary"]: item["version"] for item in manifest["components"]}
assert required["KY-EMS"] == "2.0.9-compact", required
assert components["KY-EMS"] == "2.0.9-compact", components
for binary in ("SystemMonitor", "MqttDriver", "MqttForwarder", "pointctl", "QtDisplayBridge"):
    assert required[binary] == "1.0", required
    assert components[binary] == "1.0", components
print("factory package component version test passed")
PY
for watchdog_file in gateway-health-watchdog.sh gateway-health-watchdog.service gateway-health-watchdog.default; do
  [ -f "$TMP_ROOT/unpacked/gateway-factory-defaults/deploy/$watchdog_file" ]
done

GATEWAY_HOME="$TMP_ROOT/gateway" \
INIT_WORK_DIR="$TMP_ROOT/init-work" \
INSTALL_SYSTEMD=0 \
sh "$ROOT_DIR/deploy/production-init.sh" \
  --auto \
  --package "$TMP_ROOT/package.tar.gz" \
  --runtime-mode gateway \
  --package-profile project \
  --machine-code TEST_FACTORY_PACKAGE \
  --no-mqtt-tls \
  --no-start \
  --no-smoke \
  --no-direct-maintenance

for binary in SystemMonitor MqttDriver MqttForwarder pointctl QtDisplayBridge; do
  [ -x "$TMP_ROOT/gateway/bin/$binary" ]
done
[ -x "$TMP_ROOT/gateway/ky-ems/KY-EMS" ]
[ -x "$TMP_ROOT/gateway/bin/gateway-health-watchdog.sh" ]
echo "factory package external production init test passed"
