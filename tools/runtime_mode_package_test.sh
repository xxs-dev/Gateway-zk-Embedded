#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
FACTORY_PACKAGE="${1:-$ROOT_DIR/gateway-factory-defaults.tar.gz}"
AGC_AVC_PACKAGE="${2:-$ROOT_DIR/gateway-agc-avc-runtime.tar.gz}"
TEST_ROOT="${TMPDIR:-/tmp}/gateway-runtime-mode-test.$$"
GATEWAY_HOME="$TEST_ROOT/gateway"
AGC_AVC_HOME="$TEST_ROOT/agc-avc"
MOCK_BIN="$TEST_ROOT/mock-bin"
SYSTEMCTL_LOG="$TEST_ROOT/systemctl.log"

cleanup() {
  rm -rf "$TEST_ROOT"
}

trap cleanup EXIT INT TERM

for package in "$FACTORY_PACKAGE" "$AGC_AVC_PACKAGE"; do
  [ -f "$package" ] || {
    echo "package not found: $package" >&2
    exit 2
  }
done

if tar -tzf "$FACTORY_PACKAGE" | grep -Ei 'agc.avc|AgcAvc' >/dev/null; then
  echo "general factory package contains AGC/AVC runtime files" >&2
  exit 1
fi

mkdir -p "$TEST_ROOT" "$MOCK_BIN"
cat >"$MOCK_BIN/systemctl" <<'SH'
#!/bin/sh
printf '%s\n' "$*" >>"$SYSTEMCTL_LOG"
exit 99
SH
chmod +x "$MOCK_BIN/systemctl"
: >"$SYSTEMCTL_LOG"
export SYSTEMCTL_LOG

GATEWAY_HOME="$GATEWAY_HOME" \
INIT_WORK_DIR="$TEST_ROOT/gateway-work" \
TMPDIR="$TEST_ROOT/tmp" \
PATH="$MOCK_BIN:$PATH" \
SYSTEMD_UNIT_DIR="$TEST_ROOT/systemd" \
INSTALL_SYSTEMD=0 \
sh "$ROOT_DIR/deploy/production-init.sh" \
  --auto \
  --package "$FACTORY_PACKAGE" \
  --runtime-mode gateway \
  --package-profile full \
  --machine-code TEST_GATEWAY \
  --no-mqtt-tls \
  --no-start \
  --no-smoke \
  --no-direct-maintenance

[ ! -e "$GATEWAY_HOME/bin/AgcAvcController" ]
[ ! -e "$GATEWAY_HOME/config/runtime/apps/agc-avc-service.json" ]
[ ! -e "$GATEWAY_HOME/config/runtime/devices/device_agc_avc_virtual.json" ]

GATEWAY_HOME="$AGC_AVC_HOME" \
INIT_WORK_DIR="$TEST_ROOT/agc-avc-work" \
TMPDIR="$TEST_ROOT/tmp" \
PATH="$MOCK_BIN:$PATH" \
SYSTEMD_UNIT_DIR="$TEST_ROOT/systemd" \
INSTALL_SYSTEMD=0 \
sh "$ROOT_DIR/deploy/production-init.sh" \
  --auto \
  --package "$FACTORY_PACKAGE" \
  --runtime-package "$AGC_AVC_PACKAGE" \
  --runtime-mode agc_avc \
  --package-profile full \
  --machine-code TEST_AGC \
  --no-mqtt-tls \
  --no-start \
  --no-smoke \
  --no-direct-maintenance

[ -x "$AGC_AVC_HOME/bin/AgcAvcController" ]
[ -f "$AGC_AVC_HOME/config/runtime/apps/agc-avc-service.json" ]
[ -f "$AGC_AVC_HOME/config/runtime/devices/device_agc_avc_virtual.json" ]

python3 - "$AGC_AVC_HOME/config/runtime/apps/agc-avc-service.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as stream:
    app = json.load(stream)

assert app.get("runtimeMode") == "agc_avc"
config = app.get("agcAvc") or {}
assert config.get("enabled") is True
assert config.get("shadowMode") is True
assert config.get("submitWrites") is False
PY

[ ! -s "$SYSTEMCTL_LOG" ] || {
  echo "INSTALL_SYSTEMD=0 invoked systemctl: $(cat "$SYSTEMCTL_LOG")" >&2
  exit 1
}
[ ! -e "$TEST_ROOT/systemd/agc-avc@.service" ] &&
[ ! -e "$TEST_ROOT/systemd/ems-cluster@.service" ] || {
  echo "INSTALL_SYSTEMD=0 wrote a systemd unit" >&2
  exit 1
}

echo "runtime_mode_package_test passed"
