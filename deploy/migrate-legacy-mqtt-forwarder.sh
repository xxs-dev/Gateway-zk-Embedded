#!/bin/sh
set -eu

GATEWAY_HOME=${GATEWAY_HOME:-/opt/modbus-gateway}
APP_CONFIG=${APP_CONFIG:-$GATEWAY_HOME/config/runtime/apps/mqtt-service.json}
FORWARDER_BINARY=${FORWARDER_BINARY:-/tmp/MqttForwarder}
FORWARDER_UNIT=${FORWARDER_UNIT:-/tmp/mqtt-forwarder@.service}
INSTANCE=${INSTANCE:-mqtt-service}
HEALTH_FILE=$GATEWAY_HOME/run/mqtt-forwarder-health.json
FORWARDER_SERVICE=mqtt-forwarder@${INSTANCE}.service
MQTT_SERVICE=mqtt-driver@${INSTANCE}.service
BACKUP_DIR=
CUTOVER_DONE=0

fail() {
    echo "migration failed: $*" >&2
    exit 1
}

[ -f "$APP_CONFIG" ] || fail "app config not found: $APP_CONFIG"
[ -x "$FORWARDER_BINARY" ] || fail "forwarder binary is not executable: $FORWARDER_BINARY"
[ -f "$FORWARDER_UNIT" ] || fail "forwarder unit not found: $FORWARDER_UNIT"
command -v python3 >/dev/null 2>&1 || fail "python3 is required"
command -v systemctl >/dev/null 2>&1 || fail "systemctl is required"

rollback() {
    code=$?
    trap - EXIT INT TERM
    if [ "$code" -ne 0 ] && [ -n "$BACKUP_DIR" ]; then
        cp -p "$BACKUP_DIR/mqtt-service.json.before" "$APP_CONFIG"
        if [ "$CUTOVER_DONE" -eq 1 ]; then
            systemctl restart "$MQTT_SERVICE" >/dev/null 2>&1 || true
        fi
        systemctl disable --now "$FORWARDER_SERVICE" >/dev/null 2>&1 || true
        echo "migration rolled back; backup=$BACKUP_DIR" >&2
    fi
    exit "$code"
}
trap rollback EXIT INT TERM

stamp=$(date +%Y%m%d-%H%M%S)
BACKUP_DIR=$GATEWAY_HOME/data/config-backups/mqtt-forwarder-$stamp
mkdir -p "$BACKUP_DIR" "$GATEWAY_HOME/bin" "$GATEWAY_HOME/run"
cp -p "$APP_CONFIG" "$BACKUP_DIR/mqtt-service.json.before"
[ ! -f "$GATEWAY_HOME/bin/MqttForwarder" ] || \
    cp -p "$GATEWAY_HOME/bin/MqttForwarder" "$BACKUP_DIR/MqttForwarder.before"
[ ! -f /etc/systemd/system/mqtt-forwarder@.service ] || \
    cp -p /etc/systemd/system/mqtt-forwarder@.service "$BACKUP_DIR/mqtt-forwarder@.service.before"

install -m 0755 "$FORWARDER_BINARY" "$GATEWAY_HOME/bin/MqttForwarder"
install -m 0644 "$FORWARDER_UNIT" /etc/systemd/system/mqtt-forwarder@.service

python3 - "$APP_CONFIG" <<'PY'
import json
import os
import sys

path = sys.argv[1]
with open(path) as source:
    app = json.load(source)

mqtt = app.get("mqtt") or {}
driver = app.get("mqttDriver") or {}
identity_path = app.get("identityConfigFile") or \
    "/opt/modbus-gateway/config/runtime/device_identity.json"
with open(identity_path) as source:
    machine_code = str(json.load(source).get("machineCode") or "").strip()
if not machine_code:
    raise SystemExit("device machineCode is empty")

legacy_topic = str(mqtt.get("legacyTelemetryTopic") or "").rstrip("/")
suffix = "/" + machine_code
if not legacy_topic.endswith(suffix):
    raise SystemExit(
        "legacyTelemetryTopic must end with /machineCode before migration: " + legacy_topic
    )
base_topic = legacy_topic[:-len(suffix)]
if not base_topic:
    raise SystemExit("legacyTelemetryTopic base is empty")

mappings = list(mqtt.get("legacyTelemetryPointMappings") or [])
mapped_only = bool(mqtt.get("legacyTelemetryMappedOnly", False))
mapping_indexes = [item.get("index") for item in mappings]
if any(not isinstance(index, int) or index < 0 for index in mapping_indexes):
    raise SystemExit("legacyTelemetryPointMappings contains an invalid index")
if len(mapping_indexes) != len(set(mapping_indexes)):
    raise SystemExit("legacyTelemetryPointMappings contains duplicate indexes")

if mapped_only:
    indexes = mapping_indexes
elif not driver.get("publishAllOnFull", True) and driver.get("fullUploadIndexes"):
    indexes = list(driver["fullUploadIndexes"])
else:
    indexes = []
    seen = set()
    for device_path in app.get("deviceConfigFiles") or []:
        with open(device_path) as source:
            device = json.load(source)
        for meter in device.get("meters") or []:
            for point in meter.get("points") or []:
                index = point.get("index")
                if isinstance(index, int) and index >= 0 and index not in seen:
                    seen.add(index)
                    indexes.append(index)

if not indexes:
    raise SystemExit("derived mqttForward.pointIndexes is empty")
index_set = set(indexes)
if any(index not in index_set for index in mapping_indexes):
    raise SystemExit("legacy mapping index is missing from derived pointIndexes")

app["mqttForward"] = {
    "enabled": True,
    "protocolVersion": mqtt.get("protocolVersion", "mqtt3"),
    "broker": mqtt.get("broker", ""),
    "clientId": machine_code + "-legacy-forward",
    "username": mqtt.get("username", ""),
    "password": mqtt.get("password", ""),
    "fullTelemetryTopic": base_topic,
    "qos": mqtt.get("qos", 1),
    "intervalMs": mqtt.get("legacyTelemetryIntervalMs", 10000),
    "pointIndexes": indexes,
    "payloadFormat": "legacy",
    "legacyTelemetryMappedOnly": mapped_only,
    "legacyTelemetryPointMappings": mappings,
    "tls": mqtt.get("tls") or {},
}

temporary = path + ".mqtt-forwarder.tmp"
with open(temporary, "w") as target:
    json.dump(app, target, ensure_ascii=False, indent=2)
    target.write("\n")
os.chmod(temporary, 0o600)
os.replace(temporary, path)
print(
    "prepared mqttForward machineCode=%s indexes=%d mappings=%d topic=%s" %
    (machine_code, len(indexes), len(mappings), base_topic)
)
PY

rm -f "$HEALTH_FILE"
systemctl daemon-reload
systemctl enable "$FORWARDER_SERVICE"
systemctl restart "$FORWARDER_SERVICE"

attempt=0
while [ "$attempt" -lt 15 ]; do
    if [ -f "$HEALTH_FILE" ] && python3 - "$HEALTH_FILE" <<'PY'
import json
import sys
with open(sys.argv[1]) as source:
    health = json.load(source)
if not health.get("healthy") or int(health.get("valueCount") or 0) <= 0:
    raise SystemExit(1)
PY
    then
        break
    fi
    attempt=$((attempt + 1))
    sleep 1
done
[ "$attempt" -lt 15 ] || fail "forwarder did not become healthy within 15 seconds"
systemctl is-active --quiet "$FORWARDER_SERVICE" || fail "forwarder service is not active"

python3 - "$APP_CONFIG" <<'PY'
import json
import os
import sys
path = sys.argv[1]
with open(path) as source:
    app = json.load(source)
app["mqtt"]["legacyTelemetryEnabled"] = False
temporary = path + ".mqtt-cutover.tmp"
with open(temporary, "w") as target:
    json.dump(app, target, ensure_ascii=False, indent=2)
    target.write("\n")
os.chmod(temporary, 0o600)
os.replace(temporary, path)
PY
CUTOVER_DONE=1

start_ms=$(date +%s%3N)
systemctl restart "$MQTT_SERVICE"
end_ms=$(date +%s%3N)
systemctl is-active --quiet "$MQTT_SERVICE" || fail "main MQTT service did not restart"
systemctl is-active --quiet "$FORWARDER_SERVICE" || fail "forwarder stopped during cutover"

echo "migration complete"
echo "backup=$BACKUP_DIR"
echo "mainMqttRestartMs=$((end_ms - start_ms))"
cat "$HEALTH_FILE"

trap - EXIT INT TERM
