#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_ROOT="${TMPDIR:-/tmp}/gateway-factory-scada-test-$$"
PAYLOAD_ROOT="$TEST_ROOT/payload"
GATEWAY_HOME="$TEST_ROOT/gateway"
SYSTEMD_ROOT="$TEST_ROOT/systemd"
MOCK_BIN="$TEST_ROOT/mock-bin"
SYSTEMCTL_LOG="$TEST_ROOT/systemctl.log"
SCADA_PACKAGE="$TEST_ROOT/site-a.kyscada"
FACTORY_PACKAGE="$TEST_ROOT/gateway-factory-full.tar.gz"
PACKAGE_EXTRACT_ROOT="$TEST_ROOT/package"

cleanup() {
  rm -rf "$TEST_ROOT"
}
trap cleanup EXIT INT TERM

mkdir -p "$PAYLOAD_ROOT/build-aarch64" "$PAYLOAD_ROOT/ky-ems" "$MOCK_BIN"
for bin in ModbusRtu Dlt645Driver DioDriver CanDriver IecDriver MqttDriver EventEngine ComputeEngine \
  EmsParityCheck EmsClusterCoordinator SystemMonitor pointctl LocalDisplay QtDisplayBridge CameraService stress_runner; do
  printf '#!/bin/sh\nexit 0\n' >"$PAYLOAD_ROOT/build-aarch64/$bin"
  chmod +x "$PAYLOAD_ROOT/build-aarch64/$bin"
done
printf '#!/bin/sh\nexit 0\n' >"$PAYLOAD_ROOT/ky-ems/KY-EMS"
chmod +x "$PAYLOAD_ROOT/ky-ems/KY-EMS"

python3 - "$SCADA_PACKAGE" <<'PY'
import hashlib
import json
import sys
import zipfile

documents = {
    "manifest.json": {
        "schemaVersion": "2.0",
        "projectId": "factory-site-a",
        "projectName": "factory test",
        "packageVersion": "1.0.0",
        "entryScreen": "overview",
        "packageRole": "project",
    },
    "topology.json": {
        "mode": "integrated",
        "scadaHost": "edge",
        "emsHost": "edge",
        "dataTransport": "sharedMemory",
        "offlinePolicy": "continueLocal",
    },
    "nodes.json": [{"nodeId": "edge-a", "machineCode": "COMM202600999"}],
    "tags.json": [],
    "runtime-map.json": [],
    "screens/overview.json": {"screenId": "overview", "widgets": []},
}
contents = {
    path: (json.dumps(value, ensure_ascii=True, indent=2) + "\n").encode("utf-8")
    for path, value in documents.items()
}
checksums = {path: hashlib.sha256(content).hexdigest() for path, content in contents.items()}
contents["checksums.json"] = (json.dumps(checksums, indent=2) + "\n").encode("utf-8")
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as archive:
    for path, content in contents.items():
        archive.writestr(path, content)
PY
SCADA_SHA256="$(sha256sum "$SCADA_PACKAGE" | awk '{print $1}')"

if ALLOW_DIRTY_SOURCE=1 \
  FACTORY_BINARY_SOURCE_DIR="$PAYLOAD_ROOT/build-aarch64" \
  FACTORY_KY_EMS_BINARY="$PAYLOAD_ROOT/ky-ems/KY-EMS" \
  sh "$ROOT_DIR/deploy/build-factory-package.sh" --profile full --out "$TEST_ROOT/incomplete.tar.gz" \
    >/dev/null 2>&1; then
  echo "factory package accepted KY-EMS without a fixed SCADA input" >&2
  exit 1
fi

ALLOW_DIRTY_SOURCE=1 \
FACTORY_BINARY_SOURCE_DIR="$PAYLOAD_ROOT/build-aarch64" \
FACTORY_KY_EMS_BINARY="$PAYLOAD_ROOT/ky-ems/KY-EMS" \
COMPONENT_VERSION=1.0.0-test \
EDGE_TOOLCHAIN_ID=test-fixture \
SCADA_PROJECT_PACKAGE="$SCADA_PACKAGE" \
SCADA_PROJECT_SHA256="$SCADA_SHA256" \
SCADA_PROJECT_MACHINE_CODE=COMM202600999 \
sh "$ROOT_DIR/deploy/build-factory-package.sh" --profile full --out "$FACTORY_PACKAGE"

mkdir -p "$PACKAGE_EXTRACT_ROOT"
tar -xzf "$FACTORY_PACKAGE" -C "$PACKAGE_EXTRACT_ROOT"
PACKAGE_ROOT="$PACKAGE_EXTRACT_ROOT/gateway-factory-defaults"

cat >"$MOCK_BIN/systemctl" <<'SH'
#!/bin/sh
printf '%s\n' "$*" >>"$SYSTEMCTL_LOG"
exit 0
SH
chmod +x "$MOCK_BIN/systemctl"
export SYSTEMCTL_LOG

PATH="$MOCK_BIN:$PATH" \
GATEWAY_HOME="$GATEWAY_HOME" \
BACKUP_DIR="$TEST_ROOT/backup" \
SOURCE_ROOT="$PACKAGE_ROOT" \
FACTORY_DIR="$PACKAGE_ROOT/config/factory" \
DEPLOY_DIR="$PACKAGE_ROOT/deploy" \
FACTORY_PROMPT=0 \
INIT_RUNTIME_MODE=ems \
INIT_MACHINE_CODE=COMM202600999 \
INIT_MQTT_BROKER=tcp://127.0.0.1:1883 \
INIT_MQTT_TLS_ENABLED=false \
START_SERVICES=0 \
RESET_SHM=0 \
INSTALL_SYSTEMD=1 \
SYSTEMD_UNIT_DIR="$SYSTEMD_ROOT/system" \
SYSTEM_DEFAULT_DIR="$SYSTEMD_ROOT/default" \
SYSTEMD_SYSTEM_CONF_DIR="$SYSTEMD_ROOT/system.conf.d" \
sh "$PACKAGE_ROOT/deploy/install-factory-config.sh"

[ -x "$GATEWAY_HOME/bin/gateway-ky-ems-readiness.sh" ]
for unit in "$SYSTEMD_ROOT/system"/*.service; do
  [ "$(stat -c '%a' "$unit")" = "644" ] || {
    echo "systemd unit mode is not 0644: $unit" >&2
    exit 1
  }
done
if grep -E '^(start|restart) ' "$SYSTEMCTL_LOG" >/dev/null; then
  echo "START_SERVICES=0 unexpectedly started a service" >&2
  exit 1
fi

[ -L "$GATEWAY_HOME/scada/current" ]
[ -f "$GATEWAY_HOME/scada/current/manifest.json" ]
[ -f "$GATEWAY_HOME/scada/factory-install-state.txt" ]
python3 - "$GATEWAY_HOME/config/runtime/apps/monitor-service.json" "$GATEWAY_HOME/scada/current" \
  "$GATEWAY_HOME/config/runtime/edge-package-manifest.json" "$SCADA_SHA256" <<'PY'
import hashlib
import json
import sys

config_path, expected_project, manifest_path, expected_scada_sha256 = sys.argv[1:]
with open(config_path, "rb") as stream:
    content = stream.read()
config = json.loads(content)
local_display = config["localDisplay"]
scada = local_display["scada"]
assert local_display["enabled"] is True
assert local_display["renderer"] == "nativeQt"
assert scada["enabled"] is True
assert scada["projectDirectory"] == expected_project
assert scada["packageFile"] == ""
assert scada["nodeId"] == "edge-a"
with open(manifest_path, "r", encoding="utf-8") as stream:
    packaged_scada = json.load(stream)["scadaProject"]
assert packaged_scada["sourceMode"] == "embedded-package"
assert packaged_scada["sha256"] == expected_scada_sha256
print("monitor-service.json SHA256=" + hashlib.sha256(content).hexdigest())
PY

if grep -E '^(start|restart) ' "$SYSTEMCTL_LOG" >/dev/null; then
  echo "no-restart SCADA install unexpectedly started a service" >&2
  exit 1
fi

echo "factory_scada_runtime_test passed"
