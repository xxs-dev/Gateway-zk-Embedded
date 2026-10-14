#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CASE=$(mktemp -d)
trap 'rm -rf "$CASE"' EXIT HUP INT TERM
mkdir -p "$CASE/bin" "$CASE/config/runtime/apps" "$CASE/config/runtime/devices"
export GATEWAY_HOME="$CASE" WATCHDOG_RUN_DIR="$CASE/run" SYSTEMCTL_LOG="$CASE/actions"
cat > "$CASE/bin/systemctl" <<'SH'
#!/bin/sh
printf '%s\n' "$*" >> "$SYSTEMCTL_LOG"
SH
chmod +x "$CASE/bin/systemctl"
# This fixture tests boot ordering; root-owned projection policy has separate tests.
cat > "$CASE/bin/prepare-generic-runtime.py" <<'PY'
import os
import sys

assert sys.argv[1:] == ["project-kiosk", "--home", os.environ["GATEWAY_HOME"],
                        "--instance", "monitor-service"]
with open(os.environ["SYSTEMCTL_LOG"], "a") as log:
    log.write("project-kiosk monitor-service\n")
sys.exit(1 if os.environ.get("FIXTURE_KIOSK_FAIL") else 0)
PY
export PATH="$CASE/bin:$PATH"
for renderer in nativeQt webkit; do
  printf '{"localDisplay":{"enabled":true,"renderer":"%s","kiosk":{"enabled":true,"requireDisplayConnected":false}}}\n' "$renderer" > "$CASE/config/runtime/apps/monitor-service.json"
  : > "$SYSTEMCTL_LOG"
  sh "$ROOT/deploy/gateway-services.sh" start
  if [ "$renderer" = nativeQt ]; then
    unit=ky-ems.service
  else
    unit=local-kiosk@monitor-service.service
  fi
  grep -Fx "start --no-block $unit" "$SYSTEMCTL_LOG"
  if [ "$renderer" = webkit ]; then
    grep -Fx "project-kiosk monitor-service" "$SYSTEMCTL_LOG"
  fi
  if grep -Fx "start $unit" "$SYSTEMCTL_LOG"; then
    echo "display target start can deadlock the boot transaction" >&2
    exit 1
  fi
done
: > "$SYSTEMCTL_LOG"
if FIXTURE_KIOSK_FAIL=1 sh "$ROOT/deploy/gateway-services.sh" start; then
  echo 'failed kiosk projection must reject startup' >&2
  exit 1
fi
if grep -F 'start --no-block local-kiosk@monitor-service.service' "$SYSTEMCTL_LOG"; then
  echo 'failed kiosk projection must withhold the display target' >&2
  exit 1
fi
echo 'gateway_services_boot_test passed'
