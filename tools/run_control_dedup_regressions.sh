#!/bin/sh
set -eu
if [ "$(readlink /proc/self/ns/mnt)" = "$(readlink /proc/1/ns/mnt)" ]; then
    echo "Run this test harness in a private mount/network/IPC namespace" >&2
    exit 2
fi
# Run via unshare --user --map-root-user --mount --net --ipc.
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
build="$root/tmp/stage9-control-evidence/build"
opt=$(mktemp -d "$root/tmp/stage9-control-evidence/isolated-opt.XXXXXX")
trap 'umount /opt; rm -rf -- "$opt"' EXIT
mount --make-rprivate /
mount --bind "$opt" /opt
mkdir -p /opt/modbus-gateway/data /opt/modbus-gateway/run
mount -t tmpfs tmpfs /dev/shm
mount -t tmpfs tmpfs /tmp
ip link set lo up
cd "$build"
ctest --output-on-failure -R "${1:-^(control_dedup_.*|gateway_daemon_realtime_priority|can_signal_codec|persistent_flush_pressure_.*|point_history_background_.*|sqlite_writer_failure|sqlite_sample_writer_retention|memory_point_store_reader_cache|memory_point_store_lifecycle|mqtt_driver_service|priority_control_lease|system_monitor_direct_maintenance_contract)$}"
