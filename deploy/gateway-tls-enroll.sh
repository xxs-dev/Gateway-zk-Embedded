#!/bin/sh
# Device certificates from the Gateway-zk platform trust service. One path for both uses:
#
#   csr       make (or reuse) this device's pending ECDSA P-256 key and print a CSR with CN = machine code
#   install   check the platform's certificate chain against that key and the CA bundle, then install it
#             (--dry-run: all checks, nothing written)
#   rollback  put back what the last install replaced (or remove what it added)
#   show      print the installed certificate's fingerprint, serial and expiry
#
# The device never talks to the platform: the engineering client relays the CSR (signed for a logged-in
# engineer) and brings the certificate back. Private keys are created here and never leave the device.
#
#   gateway-tls-enroll.sh csr      --purpose mqtt|maintenance [--machine-code CODE] [--new-key]
#   gateway-tls-enroll.sh install  --purpose mqtt|maintenance --chain FILE --ca FILE [--machine-code CODE] [--dry-run]
#   gateway-tls-enroll.sh rollback --purpose mqtt|maintenance [--machine-code CODE]
#   gateway-tls-enroll.sh show     --purpose mqtt|maintenance [--machine-code CODE]
#
# Files (GATEWAY_HOME defaults to /opt/modbus-gateway):
#   mqtt         config/runtime/tls/<machine>-client.key, <machine>-client.pem (leaf + device CA), ca.crt
#   maintenance  config/maintenance-v2/server.key, server.crt (leaf + device CA)
# A new key waits as <key>.pending until a certificate for it is installed, so the running service keeps its
# current certificate until then. Services are restarted by the caller after install (and after rollback).
# Exit codes: 2 usage, 3 nothing installed / nothing to roll back, 4 certificate refused.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

BASE_DIR="${GATEWAY_HOME:-/opt/modbus-gateway}"
IDENTITY_FILE="$BASE_DIR/config/runtime/device_identity.json"
TLS_DIR="$BASE_DIR/config/runtime/tls"
MAINTENANCE_DIR="$BASE_DIR/config/maintenance-v2"
COMMAND=""
PURPOSE=""
MACHINE_CODE=""
CHAIN_FILE=""
CA_SOURCE=""
NEW_KEY=0
DRY_RUN=0

usage() {
  sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//' >&2
}

fail() {
  echo "$1" >&2
  exit "${2:-1}"
}

[ "$#" -ge 1 ] || { usage; exit 2; }
COMMAND="$1"
shift
case "$COMMAND" in
  csr|install|rollback|show) ;;
  -h|--help) usage; exit 0 ;;
  *) usage; exit 2 ;;
esac

while [ "$#" -gt 0 ]; do
  case "$1" in
    --purpose) PURPOSE="${2:-}"; shift 2 ;;
    --machine-code) MACHINE_CODE="${2:-}"; shift 2 ;;
    --chain) CHAIN_FILE="${2:-}"; shift 2 ;;
    --ca) CA_SOURCE="${2:-}"; shift 2 ;;
    --new-key) NEW_KEY=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage; exit 2 ;;
  esac
done

command -v openssl >/dev/null 2>&1 || fail "openssl command not found"

if [ -z "$MACHINE_CODE" ]; then
  [ -f "$IDENTITY_FILE" ] || fail "machineCode is required and identity file not found: $IDENTITY_FILE"
  MACHINE_CODE=$(python3 - "$IDENTITY_FILE" <<'PY'
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as fh:
    print(str((json.load(fh) or {}).get("machineCode", "")).strip())
PY
)
fi
# The platform issues CN = machine code exactly; keep to characters that are safe in a subject and a file name.
case "$MACHINE_CODE" in
  ""|*[!A-Za-z0-9._-]*) fail "invalid machineCode: $MACHINE_CODE" 2 ;;
esac

case "$PURPOSE" in
  mqtt)
    KEY_FILE="$TLS_DIR/$MACHINE_CODE-client.key"
    CERT_FILE="$TLS_DIR/$MACHINE_CODE-client.pem"
    CA_FILE="$TLS_DIR/ca.crt"
    VERIFY_PURPOSE="sslclient"
    ;;
  maintenance)
    KEY_FILE="$MAINTENANCE_DIR/server.key"
    CERT_FILE="$MAINTENANCE_DIR/server.crt"
    CA_FILE=""
    VERIFY_PURPOSE="sslserver"
    ;;
  *) fail "--purpose must be mqtt or maintenance" 2 ;;
esac
PENDING_KEY="$KEY_FILE.pending"
KEY_DIR=$(dirname "$KEY_FILE")
# What the last install replaced: copies of the files that existed ("existed") and the names it created ("absent").
PREVIOUS_DIR="$KEY_DIR/.previous-$PURPOSE"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/gz-tls.XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM

# SHA-256 of the DER certificate, uppercase hex with colons (what the platform and the engineering client pin).
fingerprint() {
  openssl x509 -in "$1" -noout -fingerprint -sha256 | sed 's/^[^=]*=//'
}

public_key() {
  openssl pkey -in "$1" -pubout 2>/dev/null
}

is_p256_key() {
  openssl pkey -in "$1" -noout -text 2>/dev/null | grep -q -E 'ASN1 OID: prime256v1|NIST CURVE: P-256'
}

if [ "$COMMAND" = "csr" ]; then
  umask 077
  mkdir -p "$KEY_DIR"
  chmod 700 "$KEY_DIR" 2>/dev/null || true
  if [ "$NEW_KEY" -eq 1 ] || [ ! -s "$PENDING_KEY" ]; then
    openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -pkeyopt ec_param_enc:named_curve \
      -out "$WORK/key.pem" >/dev/null 2>&1 || fail "openssl could not create a P-256 key"
    chmod 600 "$WORK/key.pem"
    mv -f "$WORK/key.pem" "$PENDING_KEY"
  fi
  is_p256_key "$PENDING_KEY" || fail "pending key is not ECDSA P-256: $PENDING_KEY"
  openssl req -new -key "$PENDING_KEY" -subj "/CN=$MACHINE_CODE" -sha256 -out "$WORK/req.pem" >/dev/null 2>&1 \
    || fail "openssl could not create the CSR"
  cat "$WORK/req.pem"
  exit 0
fi

describe() {
  echo "fingerprint=$(fingerprint "$1")"
  echo "serial=$(openssl x509 -in "$1" -noout -serial | sed 's/^serial=//')"
  echo "notAfter=$(openssl x509 -in "$1" -noout -enddate | sed 's/^notAfter=//')"
}

if [ "$COMMAND" = "show" ]; then
  [ -s "$CERT_FILE" ] || fail "no certificate installed: $CERT_FILE" 3
  echo "purpose=$PURPOSE"
  describe "$CERT_FILE"
  echo "certFile=$CERT_FILE"
  [ -s "$PENDING_KEY" ] && echo "pendingKey=1" || echo "pendingKey=0"
  [ -d "$PREVIOUS_DIR" ] && echo "rollbackAvailable=1" || echo "rollbackAvailable=0"
  exit 0
fi

if [ "$COMMAND" = "rollback" ]; then
  [ -f "$PREVIOUS_DIR/existed" ] && [ -f "$PREVIOUS_DIR/absent" ] || fail "nothing to roll back for $PURPOSE" 3
  umask 077
  while IFS= read -r name; do
    [ -n "$name" ] || continue
    cp -p "$PREVIOUS_DIR/$name" "$KEY_DIR/$name.rollback"
    mv -f "$KEY_DIR/$name.rollback" "$KEY_DIR/$name"
  done < "$PREVIOUS_DIR/existed"
  while IFS= read -r name; do
    [ -n "$name" ] || continue
    rm -f "$KEY_DIR/$name"
  done < "$PREVIOUS_DIR/absent"
  rm -rf "$PREVIOUS_DIR"
  echo "purpose=$PURPOSE"
  if [ -s "$CERT_FILE" ]; then
    describe "$CERT_FILE"
  else
    echo "fingerprint="
  fi
  echo "rolledBack=1"
  exit 0
fi

# ---- install ---------------------------------------------------------------------------------------------------
[ -n "$CHAIN_FILE" ] && [ -s "$CHAIN_FILE" ] || fail "--chain FILE (leaf + device CA from the platform) is required" 2
[ -n "$CA_SOURCE" ] && [ -s "$CA_SOURCE" ] || fail "--ca FILE (platform root and service CA) is required" 2

# Split the chain: first certificate is the leaf, the rest are intermediates.
awk -v dir="$WORK" '
  /-----BEGIN CERTIFICATE-----/ { n++; out = (n == 1) ? dir "/leaf.pem" : dir "/intermediates.pem" }
  out != "" { print >> out }
  /-----END CERTIFICATE-----/ { out = "" }
' "$CHAIN_FILE"
[ -s "$WORK/leaf.pem" ] || fail "chain has no certificate" 2
[ -s "$WORK/intermediates.pem" ] || fail "chain must contain the device CA after the leaf" 2

# Which local key does this certificate belong to: the pending one (normal) or the current one (re-install)?
openssl x509 -in "$WORK/leaf.pem" -noout -pubkey > "$WORK/leaf.pub" 2>/dev/null || fail "leaf is not a certificate" 2
SOURCE_KEY=""
if [ -s "$PENDING_KEY" ] && [ "$(public_key "$PENDING_KEY")" = "$(cat "$WORK/leaf.pub")" ]; then
  SOURCE_KEY="$PENDING_KEY"
elif [ -s "$KEY_FILE" ] && [ "$(public_key "$KEY_FILE")" = "$(cat "$WORK/leaf.pub")" ]; then
  SOURCE_KEY="$KEY_FILE"
else
  fail "certificate does not belong to this device's key (run csr on this device first)" 4
fi
is_p256_key "$SOURCE_KEY" || fail "device key is not ECDSA P-256" 4

SUBJECT_CN=$(openssl x509 -in "$WORK/leaf.pem" -noout -subject -nameopt RFC2253 | sed -n 's/^subject=//; s/.*CN=\([^,]*\).*/\1/p')
[ "$SUBJECT_CN" = "$MACHINE_CODE" ] || fail "certificate is for $SUBJECT_CN, not $MACHINE_CODE" 4

# Chain to the platform root, validity period and key usage for this purpose (clientAuth / serverAuth).
openssl verify -CAfile "$CA_SOURCE" -untrusted "$WORK/intermediates.pem" -purpose "$VERIFY_PURPOSE" "$WORK/leaf.pem" \
  > "$WORK/verify.out" 2>&1 || { cat "$WORK/verify.out" >&2; fail "certificate does not verify against the platform CA bundle" 4; }

echo "purpose=$PURPOSE"
describe "$WORK/leaf.pem"
[ "$SOURCE_KEY" = "$PENDING_KEY" ] && echo "key=pending" || echo "key=current"
if [ -s "$CERT_FILE" ]; then
  echo "replacesFingerprint=$(fingerprint "$CERT_FILE")"
else
  echo "replacesFingerprint="
fi
echo "certFile=$CERT_FILE"
if [ "$DRY_RUN" -eq 1 ]; then
  echo "dryRun=1"
  exit 0
fi

umask 077
mkdir -p "$KEY_DIR"
chmod 700 "$KEY_DIR" 2>/dev/null || true
# Keep what is about to be replaced, for rollback.
rm -rf "$PREVIOUS_DIR"
mkdir -m 700 "$PREVIOUS_DIR"
: > "$PREVIOUS_DIR/existed"
: > "$PREVIOUS_DIR/absent"
for target in "$CERT_FILE" "$KEY_FILE" ${CA_FILE:+"$CA_FILE"}; do
  name=$(basename "$target")
  if [ -e "$target" ]; then
    cp -p "$target" "$PREVIOUS_DIR/$name"
    echo "$name" >> "$PREVIOUS_DIR/existed"
  else
    echo "$name" >> "$PREVIOUS_DIR/absent"
  fi
done

# Stage everything next to its destination, then rename: each file switches atomically.
cat "$WORK/leaf.pem" "$WORK/intermediates.pem" > "$CERT_FILE.new"
chmod 644 "$CERT_FILE.new"
if [ -n "$CA_FILE" ]; then
  cp "$CA_SOURCE" "$CA_FILE.new"
  chmod 644 "$CA_FILE.new"
  mv -f "$CA_FILE.new" "$CA_FILE"
fi
if [ "$SOURCE_KEY" = "$PENDING_KEY" ]; then
  cp "$PENDING_KEY" "$KEY_FILE.new"
  chmod 600 "$KEY_FILE.new"
  mv -f "$KEY_FILE.new" "$KEY_FILE"
  rm -f "$PENDING_KEY"
fi
mv -f "$CERT_FILE.new" "$CERT_FILE"
echo "installed=1"
