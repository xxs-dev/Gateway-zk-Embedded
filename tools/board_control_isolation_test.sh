#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C
ROOT=${GATEWAY_ISOLATION_ROOT:?Set a root-owned private probe directory containing the probe and shm marker}
LABEL=gateway-ems2-isolation-$$
ANCHOR=$LABEL-anchor.service
test "$(id -u)" = 0
test "$(readlink -f "$ROOT")" = "$ROOT"
test "$(stat -c '%U:%a' "$ROOT")" = root:700
test "$(readlink -f "$ROOT/shm")" = "$ROOT/shm"
test "$(stat -c '%U:%a' "$ROOT/shm")" = root:700
test -f "$ROOT/board_control_isolation_probe.py"
test -f "$ROOT/shm/gateway-ems2-isolation-marker"
test "$(systemctl show "$ANCHOR" --property=LoadState --value)" = not-found
cleanup() {
  code=$?
  trap - EXIT
  if ! systemctl stop "$ANCHOR"; then code=1; fi
  state=$(systemctl show "$ANCHOR" --property=ActiveState --value) || code=1
  if [ "$state" != inactive ] && [ "$state" != failed ]; then code=1; fi
  if [ "$code" = 0 ]; then printf 'FIXTURE_UNITS_STOPPED\n'; fi
  exit "$code"
}
COMMON=(
  --property=PrivateNetwork=yes
  --property=PrivateDevices=yes
  --property=PrivateTmp=yes
  --property=ProtectSystem=strict
  --property=ProtectHome=yes
  --property=ProtectKernelTunables=yes
  --property=ProtectKernelModules=yes
  --property=ProtectKernelLogs=yes
  --property=ProtectControlGroups=yes
  --property=NoNewPrivileges=yes
  '--property=CapabilityBoundingSet=CAP_SYS_ADMIN CAP_SETPCAP'
  --property=AmbientCapabilities=
  --property=RestrictNamespaces=ipc
  --property=RestrictRealtime=yes
  --property=LockPersonality=yes
  --property=SystemCallArchitectures=native
  '--property=RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6 AF_NETLINK'
  '--property=TemporaryFileSystem=/run:rw,nosuid,nodev,noexec,size=16M /dev/pts:rw,nosuid,nodev,noexec,size=1M'
  --property=BindPaths="$ROOT/shm:/dev/shm"
  --property=TimeoutStopSec=5
  --property=RuntimeMaxSec=180
  --property=MemoryMax=128M
  --property=TasksMax=32
)
# systemd 245 has no PrivateIPC; only these fixed tools run with setup caps.
# Probe/business execution must follow setpriv and prove every cap set is zero.
systemd-run --unit="$ANCHOR" --collect "${COMMON[@]}" /usr/bin/unshare --ipc /bin/sleep 180
trap cleanup EXIT
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
for index in 1 2; do
  systemd-run --unit="$LABEL-probe-$index.service" \
    --collect --wait --pipe "${COMMON[@]}" \
    --property=JoinsNamespaceOf="$ANCHOR" --property=After="$ANCHOR" \
    --property=Requires="$ANCHOR" \
    /usr/bin/nsenter --ipc="/proc/$PID/ns/ipc" \
    /usr/bin/setpriv --no-new-privs --bounding-set=-sys_admin,-setpcap \
    --inh-caps=-sys_admin,-setpcap --ambient-caps=-sys_admin,-setpcap \
    /usr/bin/python3 -B "$ROOT/board_control_isolation_probe.py" \
    --host-netns "$HOST_NET" --shared-netns "$SHARED_NET" \
    --host-ipcns "$HOST_IPC" --shared-ipcns "$SHARED_IPC" \
    --shm-marker /dev/shm/gateway-ems2-isolation-marker --marker-sha256 "$MARKER_SHA" \
    --marker-dev "$(stat -c %d "$ROOT/shm/gateway-ems2-isolation-marker")" \
    --marker-ino "$(stat -c %i "$ROOT/shm/gateway-ems2-isolation-marker")"
done
systemctl show "$ANCHOR" --property=ActiveState,MainPID,PrivateNetwork,PrivateDevices,PrivateTmp,CapabilityBoundingSet,AmbientCapabilities,NoNewPrivileges,RestrictAddressFamilies,ProtectControlGroups,ProtectKernelTunables,TemporaryFileSystem,BindPaths
printf 'TWO_SYSTEMD_SERVICE_CONTROL_ISOLATION_PASS\n'
