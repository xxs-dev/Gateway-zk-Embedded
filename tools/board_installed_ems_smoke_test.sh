#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C
ROOT=${GATEWAY_EMS_SMOKE_ROOT:?Set the private prepared smoke directory}
PIN=${GATEWAY_EMS_SMOKE_APPROVAL_SHA256:?Set the installed approval SHA256}
MODE=${GATEWAY_EMS_SMOKE_DISPLAY_PROBE:-legacy-bridge}
case "$MODE" in legacy-bridge|native-direct-probe) ;; *) exit 2 ;; esac
LABEL=gateway-installed-ems-smoke-$$
ANCHOR=$LABEL-anchor.service
JOB=$LABEL-programs.service
HOME_DIR=/opt/modbus-gateway
test "$(id -u)" = 0
test "$(readlink -f "$ROOT")" = "$ROOT"
test "$(stat -c '%U:%a' "$ROOT")" = root:700
test -f /opt/gateway-reprovision/STOP
test ! -e /opt/gateway-reprovision/pending.json
test "$(systemctl show gateway-services.service --property=MainPID --value)" = 0
test "$(systemctl show ky-ems.service --property=MainPID --value)" = 0
printf '%s  %s\n' "$PIN" "$ROOT/approval.json" | sha256sum -c -
BIND_PATHS="$ROOT/shm:/dev/shm $ROOT/data:$HOME_DIR/data $ROOT/run:$HOME_DIR/run $ROOT/evidence"
WRITE_PATHS="/dev/shm $HOME_DIR/data $HOME_DIR/run $ROOT/evidence"
if [ "$MODE" = legacy-bridge ]; then
  test -f "$ROOT/VarList.xml"
  test ! -L "$ROOT/VarList.xml"
  cmp "$ROOT/VarList.xml" "$HOME_DIR/ky-ems/VarList.xml"
  BIND_PATHS+=" $ROOT/VarList.xml:$HOME_DIR/ky-ems/VarList.xml"
  WRITE_PATHS+=" $HOME_DIR/ky-ems/VarList.xml"
fi
for directory in data run shm evidence; do
  test "$(readlink -f "$ROOT/$directory")" = "$ROOT/$directory"
  test "$(stat -c '%U:%a' "$ROOT/$directory")" = root:700
done
cleanup() {
  code=$?
  trap - EXIT
  for unit in "$JOB" "$ANCHOR"; do
    if [ "$(systemctl show "$unit" --property=LoadState --value)" != not-found ]; then
      if ! systemctl stop "$unit"; then code=1; fi
      if [ "$(systemctl show "$unit" --property=MainPID --value)" != 0 ]; then code=1; fi
    fi
  done
  if [ "${MOUNTED:-0}" = 1 ]; then
    if ! umount "$ROOT/shm"; then code=1; fi
  fi
  if [ "$code" = 0 ]; then printf 'INSTALLED_SMOKE_UNITS_STOPPED\n'; fi
  exit "$code"
}
test -z "$(find "$ROOT/shm" -mindepth 1 -maxdepth 1 ! -name gateway-ems2-isolation-marker -print -quit)"
if mountpoint -q "$ROOT/shm"; then exit 2; fi
# libc shm_open requires tmpfs; never use or clear the host /dev/shm backing.
mount -t tmpfs -o mode=700,nosuid,nodev,noexec,size=256M tmpfs "$ROOT/shm"
MOUNTED=1
trap cleanup EXIT
printf '%s\n' "$PIN" > "$ROOT/shm/gateway-ems2-isolation-marker"
chmod 600 "$ROOT/shm/gateway-ems2-isolation-marker"
COMMON=(
  --property=PrivateNetwork=yes --property=PrivateDevices=yes --property=PrivateTmp=yes
  --property=ProtectSystem=strict --property=ProtectHome=yes
  --property=ProtectKernelTunables=yes --property=ProtectKernelModules=yes
  --property=ProtectKernelLogs=yes --property=ProtectControlGroups=yes
  --property=NoNewPrivileges=yes '--property=CapabilityBoundingSet=CAP_SYS_ADMIN CAP_SETPCAP'
  --property=AmbientCapabilities= --property=RestrictNamespaces=ipc
  --property=RestrictRealtime=yes --property=LockPersonality=yes
  --property=SystemCallArchitectures=native
  '--property=RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6 AF_NETLINK'
  '--property=UnsetEnvironment=LD_PRELOAD LD_LIBRARY_PATH PYTHONHOME PYTHONPATH'
  '--property=TemporaryFileSystem=/run:rw,nosuid,nodev,noexec,size=16M /dev/pts:rw,nosuid,nodev,noexec,size=1M /opt:ro /var:ro'
  "--property=BindReadOnlyPaths=$HOME_DIR $ROOT"
  "--property=BindPaths=$BIND_PATHS"
  "--property=ReadWritePaths=$WRITE_PATHS"
  --property=TimeoutStopSec=30 --property=RuntimeMaxSec=180
  --property=MemoryMax=768M --property=TasksMax=128
)
systemd-run --unit="$ANCHOR" --collect "${COMMON[@]}" /usr/bin/unshare --ipc /bin/sleep 180
PID=$(systemctl show "$ANCHOR" --property=MainPID --value)
test "$PID" -gt 1
HOST_NET=$(readlink /proc/1/ns/net)
HOST_IPC=$(readlink /proc/1/ns/ipc)
for attempt in $(seq 1 50); do
  SHARED_NET=$(readlink "/proc/$PID/ns/net")
  SHARED_IPC=$(readlink "/proc/$PID/ns/ipc")
  if [ "$HOST_NET" != "$SHARED_NET" ] && [ "$HOST_IPC" != "$SHARED_IPC" ]; then break; fi
  sleep .1
done
test "$HOST_NET" != "$SHARED_NET"
test "$HOST_IPC" != "$SHARED_IPC"
MARKER_SHA=$(sha256sum "$ROOT/shm/gateway-ems2-isolation-marker" | cut -d ' ' -f 1)
systemd-run --unit="$JOB" --collect --wait --pipe "${COMMON[@]}" \
  --property=JoinsNamespaceOf="$ANCHOR" --property=After="$ANCHOR" --property=BindsTo="$ANCHOR" \
  /usr/bin/nsenter --ipc="/proc/$PID/ns/ipc" \
  /usr/bin/setpriv --no-new-privs --bounding-set=-sys_admin,-setpcap \
  --inh-caps=-sys_admin,-setpcap --ambient-caps=-sys_admin,-setpcap \
  /usr/bin/python3 -B "$ROOT/board_installed_ems_smoke.py" \
  --host-netns "$HOST_NET" --shared-netns "$SHARED_NET" \
  --host-ipcns "$HOST_IPC" --shared-ipcns "$SHARED_IPC" \
  --shm-marker /dev/shm/gateway-ems2-isolation-marker --marker-sha256 "$MARKER_SHA" \
  --marker-dev "$(stat -c %d "$ROOT/shm/gateway-ems2-isolation-marker")" \
  --marker-ino "$(stat -c %i "$ROOT/shm/gateway-ems2-isolation-marker")" \
  --approval "$ROOT/approval.json" --approval-sha256 "$PIN" --evidence "$ROOT/evidence" \
  --display-probe "$MODE"
