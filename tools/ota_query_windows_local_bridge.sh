#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 6 ]; then
    echo 'usage: ota_query_windows_local_bridge.sh NATIVE_BINARY REQUEST_WINDOWS PUBLIC_WINDOWS PRIVATE_WINDOWS MACHINE OUTPUT_WINDOWS' >&2
    exit 2
fi
binary=$1
test -x "$binary"
root=$(mktemp -d /tmp/ota-m3-local-http-query-XXXXXX)
chmod 700 "$root"
trap 'rm -rf -- "$root"' EXIT
umask 077
cp "$(wslpath -a "$2")" "$root/challenge.json"
cp "$(wslpath -a "$3")" "$root/platform-public.pem"
cp "$(wslpath -a "$4")" "$root/device-private.pem"
chmod 600 "$root/challenge.json" "$root/platform-public.pem" "$root/device-private.pem"
mkdir "$root/staging"
"$binary" --original-query-wire "$root/challenge.json" "$root/platform-public.pem" \
    "$root/device-private.pem" "$root/staging" "$5" >"$(wslpath -a "$6")"
