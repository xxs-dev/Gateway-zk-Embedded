#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
INSTALL=/opt/modbus-gateway
[ "$(id -u)" -eq 0 ] || { echo "requires isolated root namespace" >&2; exit 1; }
[ ! -e "$INSTALL" ] && [ ! -L "$INSTALL" ] || { echo "fixture cleanup test requires empty /opt install path" >&2; exit 1; }
mkdir -p "$INSTALL"
printf 'must-remain\n' > "$INSTALL/sentinel"
for fixture in ota_apply_self_upgrade_test.sh ota_async_health_test.sh; do
  if sh "$ROOT/tools/$fixture" >/tmp/ota-fixture-cleanup.log 2>&1; then
    echo "$fixture unexpectedly accepted an existing installation" >&2
    exit 1
  fi
  grep -q 'installation path must be disposable' /tmp/ota-fixture-cleanup.log
  test "$(cat "$INSTALL/sentinel")" = must-remain
done
printf 'OTA fixture existing-install cleanup test passed\n'
