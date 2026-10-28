#!/bin/sh
set -eu

if [ "$(readlink /proc/self/ns/mnt)" = "$(readlink /proc/1/ns/mnt)" ]; then
  exec unshare --mount --propagation private -- sh "$0" "$@"
fi
mount --make-rprivate /

root=$1
test "$(readlink -f "$root")" = "$root"
label=${2:-enospc}
case "$label" in *[!a-zA-Z0-9_-]*|'') exit 2 ;; esac
mountpoint="$root/$label-mount"
mkdir "$mountpoint"
mount -t tmpfs -o size=32m,nosuid,nodev tmpfs "$mountpoint"
trap 'umount "$mountpoint"' EXIT HUP INT TERM

python3 "$root/sqlite_wal_deep_validation.py" \
  --binary "$root/sqlite_outbox_benchmark" \
  --new-library "$root/libsqlite3-eval.so" \
  --output "$root/$label-results" \
  --enospc-mount "$mountpoint"
