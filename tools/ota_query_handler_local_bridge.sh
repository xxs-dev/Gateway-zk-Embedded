#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo 'usage: ota_query_handler_local_bridge.sh NATIVE_TEST_BINARY PRIVATE_OUTPUT_DIR' >&2
    exit 2
fi

binary=$1
output=$2
test -x "$binary"
mkdir -p "$output"
root=$(mktemp -d /tmp/ota-m3-local-query-handler-XXXXXX)
chmod 700 "$root"
trap 'rm -rf -- "$root"' EXIT
mkdir "$root/staging"
umask 077
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$root/platform-private.pem" 2>"$output/platform-keygen.stderr"
openssl pkey -in "$root/platform-private.pem" -pubout -out "$root/platform-public.pem" 2>"$output/platform-pub.stderr"
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$root/device-private.pem" 2>"$output/device-keygen.stderr"
openssl pkey -in "$root/device-private.pem" -pubout -out "$output/device-public.pem" 2>"$output/device-pub.stderr"
issued=$(($(date +%s) * 1000))
expires=$((issued + 60000))
nonce=$(printf '%064d' 0 | tr '0' 'b')
artifact=$(printf '%064d' 0 | tr '0' 'a')
candidate=$(printf '%064d' 0 | tr '0' 'c')
printf 'edge-original-job-query-request-v1\njobId=JOB_1\nmachineCode=GW_TEST\nnonce=%s\nversion=2.0.0\nartifactSha256=%s\nartifactSize=13\ncandidateSha256=%s\nissuedAtMs=%s\nexpiresAtMs=%s\n' \
    "$nonce" "$artifact" "$candidate" "$issued" "$expires" >"$root/challenge.descriptor"
signature=$(openssl dgst -sha256 -sign "$root/platform-private.pem" "$root/challenge.descriptor" | openssl base64 -A)
jq -nc --arg nonce "$nonce" --arg artifact "$artifact" --arg candidate "$candidate" \
    --arg issued "$issued" --arg expires "$expires" --arg signature "$signature" \
    '{jobId:"JOB_1",machineCode:"GW_TEST",nonce:$nonce,version:"2.0.0",artifactSha256:$artifact,
      artifactSize:"13",candidateSha256:$candidate,issuedAtMs:$issued,expiresAtMs:$expires,
      signature:$signature}' >"$root/challenge.json"
expired_issued=$((issued - 120000))
expired_expires=$((issued - 60000))
printf 'edge-original-job-query-request-v1\njobId=JOB_1\nmachineCode=GW_TEST\nnonce=%s\nversion=2.0.0\nartifactSha256=%s\nartifactSize=13\ncandidateSha256=%s\nissuedAtMs=%s\nexpiresAtMs=%s\n' \
    "$nonce" "$artifact" "$candidate" "$expired_issued" "$expired_expires" >"$root/expired.descriptor"
expired_signature=$(openssl dgst -sha256 -sign "$root/platform-private.pem" "$root/expired.descriptor" | openssl base64 -A)
jq -nc --arg nonce "$nonce" --arg artifact "$artifact" --arg candidate "$candidate" \
    --arg issued "$expired_issued" --arg expires "$expired_expires" --arg signature "$expired_signature" \
    '{jobId:"JOB_1",machineCode:"GW_TEST",nonce:$nonce,version:"2.0.0",artifactSha256:$artifact,
      artifactSize:"13",candidateSha256:$candidate,issuedAtMs:$issued,expiresAtMs:$expires,
      signature:$signature}' >"$root/expired.json"
"$binary" --original-query-handler "$root/challenge.json" "$root/expired.json" "$root/platform-public.pem" \
    "$root/device-private.pem" "$root/staging" >"$output/native-handler-response.json" \
    2>"$output/native-handler.stderr"
grep -q 'mqtt OTA query rejected code=AUTH' "$output/native-handler.stderr"
if grep -q "$nonce" "$output/native-handler.stderr"; then
    echo 'query diagnostic leaked nonce' >&2
    exit 1
fi
openssl pkey -pubin -in "$output/device-public.pem" -outform DER 2>"$output/device-der.stderr" \
    | sha256sum | cut -d' ' -f1 >"$output/device-public-der.sha256"
sha256sum "$output/native-handler-response.json" "$output/device-public.pem" >"$output/public-output.sha256"
