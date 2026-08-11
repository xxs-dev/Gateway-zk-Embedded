#!/bin/sh
set -eu

PROJECT_DIR="${KY_EMS_PROJECT_DIR:-/opt/modbus-gateway/scada/current}"
DISPLAY_SOCKET="${DISPLAY_SOCKET:-/tmp/.X11-unix/X0}"
XAUTHORITY_FILE="${XAUTHORITY:-/run/user/1000/gdm/Xauthority}"
RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/ky-ems}"
WAIT_SECONDS="${DISPLAY_WAIT_TIMEOUT_SECONDS:-60}"
GATEWAY_HOME="${GATEWAY_HOME:-/opt/modbus-gateway}"

case "$WAIT_SECONDS" in
  ''|*[!0-9]*)
    echo "invalid DISPLAY_WAIT_TIMEOUT_SECONDS: $WAIT_SECONDS" >&2
    exit 2
    ;;
esac

if [ -f "$GATEWAY_HOME/config/runtime/edge-package-manifest.json" ]; then
  [ -x "$GATEWAY_HOME/bin/gateway-scada-readiness.sh" ] || {
    echo "SCADA readiness verifier is missing" >&2
    exit 1
  }
  "$GATEWAY_HOME/bin/gateway-scada-readiness.sh" strict-required
fi
[ -d "$PROJECT_DIR" ] || {
  echo "SCADA project directory does not exist: $PROJECT_DIR" >&2
  exit 1
}
[ -d "$RUNTIME_DIR" ] || {
  echo "KY-EMS runtime directory does not exist: $RUNTIME_DIR" >&2
  exit 1
}

runtime_uid=$(stat -c '%u' "$RUNTIME_DIR")
runtime_mode=$(stat -c '%a' "$RUNTIME_DIR")
[ "$runtime_uid" = "$(id -u)" ] || {
  echo "KY-EMS runtime directory has wrong ownership: $RUNTIME_DIR" >&2
  exit 1
}
[ "$runtime_mode" = "700" ] || {
  echo "KY-EMS runtime directory must use mode 0700: $RUNTIME_DIR" >&2
  exit 1
}

elapsed=0
while :; do
  if [ -S "$DISPLAY_SOCKET" ] && [ -r "$XAUTHORITY_FILE" ]; then
    exit 0
  fi
  if [ "$elapsed" -ge "$WAIT_SECONDS" ]; then
    break
  fi
  sleep 1
  elapsed=$((elapsed + 1))
done

echo "display session not ready after ${WAIT_SECONDS}s: socket=$DISPLAY_SOCKET xauthority=$XAUTHORITY_FILE" >&2
exit 1
