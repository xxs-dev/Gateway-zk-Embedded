#!/bin/sh
set -eu
mount --make-rprivate /
mount -t tmpfs -o size=512m,mode=1777 tmpfs /dev/shm
mkdir /dev/shm/transport-build
mount --bind "$2" /dev/shm/transport-build
mount -t tmpfs -o size=128m,mode=1777 tmpfs /tmp
ip link set lo up
cd /dev/shm/transport-build
binary=$(basename "$1")
shift 2
exec "./$binary" "$@"
