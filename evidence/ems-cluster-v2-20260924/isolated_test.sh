#!/bin/sh
set -eu
mount --make-rprivate /
mount -t tmpfs -o size=512m,mode=1777 tmpfs /dev/shm
ip link set lo up
cd "$2"
binary="$1"
shift 2
exec "$binary" "$@"
