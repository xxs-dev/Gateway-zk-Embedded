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
"$1" "$root/platform-public.pem" "$root/device-private.pem" "$root/signature" >"$root/response.base64"
openssl pkey -in "$root/device-private.pem" -pubout -out "$root/device-public.pem" 2>/dev/null
openssl base64 -d -A -in "$root/response.base64" -out "$root/response.sig"
transaction=$(printf '%032d' 0 | tr 0 d)
intent=$(printf '%064d' 0 | tr 0 e)
health=$(printf '%064d' 0 | tr 0 f)
printf 'edge-original-job-query-response-v1\njobId=JOB_1\nmachineCode=GW_TEST\nnonce=%s\noutcome=HEALTHY_APPLIED\noccurrenceId=aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee\nversion=2.0.0\nartifactSha256=%s\nartifactSize=13\ncandidateSha256=%s\ntransaction=OTA_%s\nintentSha256=%s\nhealthSha256=%s\nhealthTs=1700000001000\n' \
    "$nonce" "$artifact" "$candidate" "$transaction" "$intent" "$health" >"$root/response-descriptor"
openssl dgst -sha256 -verify "$root/device-public.pem" -signature "$root/response.sig" "$root/response-descriptor"
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:1024 -out "$root/weak.pem" 2>/dev/null
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$root/ec.pem" 2>/dev/null
openssl genpkey -algorithm RSA-PSS -pkeyopt rsa_keygen_bits:2048 -out "$root/pss.pem" 2>/dev/null
for key in weak ec pss; do
    "$1" --reject-private-key "$root/$key.pem"
done
ln "$root/device-private.pem" "$root/hardlink.pem"
"$1" --reject-private-key "$root/hardlink.pem"
printf 'OTA query challenge, tamper/key policy, and independent response verification PASS\n'
