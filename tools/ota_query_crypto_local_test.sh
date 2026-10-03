#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo 'usage: ota_query_crypto_local_test.sh NATIVE_TEST_BINARY' >&2
    exit 2
fi
test -x "$1"
umask 077
root=$(mktemp -d /tmp/ota-m3-local-query-crypto-XXXXXX)
trap 'rm -rf -- "$root"' EXIT
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$root/platform-private.pem" 2>/dev/null
openssl pkey -in "$root/platform-private.pem" -pubout -out "$root/platform-public.pem" 2>/dev/null
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$root/device-private.pem" 2>/dev/null
nonce=$(printf '%064d' 0 | tr 0 b)
artifact=$(printf '%064d' 0 | tr 0 a)
candidate=$(printf '%064d' 0 | tr 0 c)
printf 'edge-original-job-query-request-v1\njobId=JOB_1\nmachineCode=GW_TEST\nnonce=%s\nversion=2.0.0\nartifactSha256=%s\nartifactSize=13\ncandidateSha256=%s\nissuedAtMs=1700000000000\nexpiresAtMs=1700000060000\n' \
    "$nonce" "$artifact" "$candidate" >"$root/descriptor"
openssl dgst -sha256 -sign "$root/platform-private.pem" "$root/descriptor" \
    | openssl base64 -A >"$root/signature"
"$1" "$root/platform-public.pem" "$root/device-private.pem" "$root/signature"
