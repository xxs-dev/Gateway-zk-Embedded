#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT HUP INT TERM

GATEWAY_HOME="$TMP_DIR/gateway"
MOCK_BIN="$TMP_DIR/bin"
SYSTEMCTL_LOG="$TMP_DIR/systemctl.log"
mkdir -p "$GATEWAY_HOME/config/runtime/apps" \
  "$GATEWAY_HOME/config/runtime/devices" \
  "$MOCK_BIN"

cat >"$GATEWAY_HOME/config/runtime/apps/monitor-service.json" <<'JSON'
{
  "deviceConfigFiles": ["../devices/device_test.json"],
  "systemMonitor": { "enabled": false },
  "localDisplay": { "enabled": true, "renderer": "nativeQt" }
}
JSON

cat >"$GATEWAY_HOME/config/runtime/devices/device_test.json" <<'JSON'
{
  "enabled": true,
  "protocol": { "type": "modbus_rtu" }
}
JSON

cat >"$MOCK_BIN/systemctl" <<'SH'
#!/bin/sh
printf '%s\n' "$*" >>"$SYSTEMCTL_LOG"
exit 0
SH
chmod +x "$MOCK_BIN/systemctl"

export GATEWAY_HOME SYSTEMCTL_LOG
PATH="$MOCK_BIN:$PATH" sh "$ROOT_DIR/deploy/gateway-services.sh" start >/dev/null

grep -Fx 'start modbus-rtu@device_test.service' "$SYSTEMCTL_LOG" >/dev/null
grep -Fx 'start --no-block ky-ems.service' "$SYSTEMCTL_LOG" >/dev/null
if grep -Fx 'start ky-ems.service' "$SYSTEMCTL_LOG" >/dev/null; then
  echo "ky-ems.service must not be started synchronously during boot" >&2
  exit 1
fi

echo "gateway_services_boot_test passed"
