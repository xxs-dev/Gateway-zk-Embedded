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
if grep -F 'start qt-display-bridge.service' "$SYSTEMCTL_LOG" >/dev/null; then
  echo "qt-display-bridge.service must be pulled by the ky-ems systemd dependency, not emitted as a desired unit" >&2
  exit 1
fi

KY_EMS_UNIT="$ROOT_DIR/deploy/ky-ems.service"
QT_BRIDGE_UNIT="$ROOT_DIR/deploy/qt-display-bridge.service"
READINESS_SCRIPT="$ROOT_DIR/deploy/gateway-ky-ems-readiness.sh"
grep -Fx 'Requires=qt-display-bridge.service' "$KY_EMS_UNIT" >/dev/null
grep -Fx 'After=graphical.target qt-display-bridge.service' "$KY_EMS_UNIT" >/dev/null
grep -Fx 'RuntimeDirectory=ky-ems' "$KY_EMS_UNIT" >/dev/null
grep -Fx 'RuntimeDirectoryMode=0700' "$KY_EMS_UNIT" >/dev/null
grep -Fx 'Environment=XDG_RUNTIME_DIR=/run/ky-ems' "$KY_EMS_UNIT" >/dev/null
grep -Fx 'ExecStartPre=/opt/modbus-gateway/bin/gateway-ky-ems-readiness.sh' "$KY_EMS_UNIT" >/dev/null
grep -Fx 'TimeoutStartSec=75' "$KY_EMS_UNIT" >/dev/null
grep -Fx 'PartOf=ky-ems.service' "$QT_BRIDGE_UNIT" >/dev/null
grep -Fx 'Before=ky-ems.service' "$QT_BRIDGE_UNIT" >/dev/null
grep -Fx 'Type=simple' "$QT_BRIDGE_UNIT" >/dev/null

PROJECT_DIR="$TMP_DIR/scada/current"
DISPLAY_SOCKET="$TMP_DIR/X0"
XAUTHORITY_FILE="$TMP_DIR/Xauthority"
RUNTIME_DIR="$TMP_DIR/run/ky-ems"
mkdir -p "$PROJECT_DIR" "$RUNTIME_DIR"
chmod 0700 "$RUNTIME_DIR"
printf '%s\n' test-cookie >"$XAUTHORITY_FILE"
python3 - "$DISPLAY_SOCKET" <<'PY'
import socket
import sys

sock = socket.socket(socket.AF_UNIX)
sock.bind(sys.argv[1])
sock.close()
PY

KY_EMS_PROJECT_DIR="$PROJECT_DIR" \
DISPLAY_SOCKET="$DISPLAY_SOCKET" \
XAUTHORITY="$XAUTHORITY_FILE" \
XDG_RUNTIME_DIR="$RUNTIME_DIR" \
DISPLAY_WAIT_TIMEOUT_SECONDS=0 \
sh "$READINESS_SCRIPT"

if KY_EMS_PROJECT_DIR="$TMP_DIR/scada/missing" \
  DISPLAY_SOCKET="$DISPLAY_SOCKET" \
  XAUTHORITY="$XAUTHORITY_FILE" \
  XDG_RUNTIME_DIR="$RUNTIME_DIR" \
  DISPLAY_WAIT_TIMEOUT_SECONDS=0 \
  sh "$READINESS_SCRIPT" >/dev/null 2>&1; then
  echo "KY-EMS readiness must reject a missing SCADA current project" >&2
  exit 1
fi

if KY_EMS_PROJECT_DIR="$PROJECT_DIR" \
  DISPLAY_SOCKET="$DISPLAY_SOCKET" \
  XAUTHORITY="$TMP_DIR/missing-Xauthority" \
  XDG_RUNTIME_DIR="$RUNTIME_DIR" \
  DISPLAY_WAIT_TIMEOUT_SECONDS=0 \
  sh "$READINESS_SCRIPT" >/dev/null 2>&1; then
  echo "KY-EMS readiness must reject a missing Xauthority file" >&2
  exit 1
fi

echo "gateway_services_boot_test passed"
