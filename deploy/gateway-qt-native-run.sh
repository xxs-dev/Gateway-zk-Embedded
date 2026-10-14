#!/bin/sh
set -eu

# Historical recovery pins gateway-qt-run.sh; keep the native entry separate.
HOME_DIR="${GATEWAY_HOME:-/opt/modbus-gateway}"
BINARY="${GATEWAY_QT_BINARY:-$HOME_DIR/ky-ems/KY-EMS}"
case "$#:${1:-}" in
  0:|1:--check) ;;
  *) echo "unknown native Qt launcher option" >&2; exit 2 ;;
esac
[ -x "$BINARY" ] || { echo "KY-EMS executable missing: $BINARY" >&2; exit 2; }
command -v ldd >/dev/null 2>&1 || { echo "ldd is required for Qt dependency preflight" >&2; exit 2; }
deps=$(ldd "$BINARY") || { echo "cannot inspect KY-EMS dependencies" >&2; exit 2; }
if printf '%s\n' "$deps" | grep -q 'not found'; then
  printf '%s\n' "$deps" >&2
  exit 2
fi
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}"
export DISPLAY="${DISPLAY:-:0}"
if [ "$QT_QPA_PLATFORM" = "xcb" ]; then
  [ -n "${XAUTHORITY:-}" ] && [ -r "$XAUTHORITY" ] || {
    echo "configure target XAUTHORITY in config/runtime/qt-display.env" >&2; exit 2;
  }
  [ -n "${XDG_RUNTIME_DIR:-}" ] && [ -d "$XDG_RUNTIME_DIR" ] || {
    echo "configure target XDG_RUNTIME_DIR in config/runtime/qt-display.env" >&2; exit 2;
  }
fi
[ "${1:-}" != "--check" ] || exit 0
exec "$BINARY" --app-config "$HOME_DIR/config/runtime/apps/monitor-service.json"
