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
  if grep -Fx "start $unit" "$SYSTEMCTL_LOG"; then
    echo "display target start can deadlock the boot transaction" >&2
    exit 1
  fi
done
echo 'gateway_services_boot_test passed'
