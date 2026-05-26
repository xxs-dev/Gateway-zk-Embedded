#!/bin/sh
set -eu

BASE_DIR="${GATEWAY_HOME:-/opt/modbus-gateway}"
IDENTITY_FILE="$BASE_DIR/config/runtime/device_identity.json"
TLS_DIR="$BASE_DIR/config/runtime/tls"
APP_CONFIG="$BASE_DIR/config/runtime/apps/mqtt-service.json"
MACHINE_CODE=""
PLATFORM_URL=""
ENROLLMENT_TOKEN=""
VALIDITY_DAYS=""
FORCE_KEY=0
RESTART_SERVICES=0
UPDATE_LOCAL_APP=0

usage() {
  cat >&2 <<'EOF'
Usage: gateway-tls-enroll.sh [options]

Options:
  --machine-code CODE       Device machineCode; defaults to runtime/device_identity.json
  --platform-url URL        Platform base URL, e.g. https://edge.example.com
  --token TOKEN             TLS enrollment token for the platform sign endpoint
  --tls-dir DIR             TLS output dir; defaults to /opt/modbus-gateway/config/runtime/tls
  --app-config FILE         MQTT app config to update when --update-local-app is set
  --validity-days DAYS      Requested client certificate validity days
  --force-key               Regenerate the private key even when it already exists
  --update-local-app        Update mqtt.tls caFile/certFile/keyFile in local app config
  --restart                 Restart gateway-services.service after successful enrollment
  -h, --help                Show this help

Without --platform-url and --token, the script only creates client.key and client.csr.
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --machine-code)
      MACHINE_CODE="${2:-}"
      shift 2
      ;;
    --platform-url)
      PLATFORM_URL="${2:-}"
      shift 2
      ;;
    --token)
      ENROLLMENT_TOKEN="${2:-}"
      shift 2
      ;;
    --tls-dir)
      TLS_DIR="${2:-}"
      shift 2
      ;;
    --app-config)
      APP_CONFIG="${2:-}"
      shift 2
      ;;
    --validity-days)
      VALIDITY_DAYS="${2:-}"
      shift 2
      ;;
    --force-key)
      FORCE_KEY=1
      shift
      ;;
    --restart)
      RESTART_SERVICES=1
      shift
      ;;
    --update-local-app)
      UPDATE_LOCAL_APP=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown option: $1" >&2
      usage
      exit 2
      ;;
  esac
done

if ! command -v openssl >/dev/null 2>&1; then
  echo "openssl command not found" >&2
  exit 1
fi

if [ -z "$MACHINE_CODE" ]; then
  if [ ! -f "$IDENTITY_FILE" ]; then
    echo "machineCode is required and identity file not found: $IDENTITY_FILE" >&2
    exit 1
  fi
  MACHINE_CODE=$(python3 - "$IDENTITY_FILE" <<'PY'
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as fh:
    print(str((json.load(fh) or {}).get("machineCode", "")).strip())
PY
)
fi

case "$MACHINE_CODE" in
  ""|*/*|*\\*|*+*|*#*)
    echo "invalid machineCode: $MACHINE_CODE" >&2
    exit 1
    ;;
esac

SAFE_MACHINE=$(printf '%s' "$MACHINE_CODE" | sed 's/[^A-Za-z0-9._-]/_/g')
mkdir -p "$TLS_DIR"
KEY_FILE="$TLS_DIR/$SAFE_MACHINE-client.key"
CSR_FILE="$TLS_DIR/$SAFE_MACHINE-client.csr"
CERT_FILE="$TLS_DIR/$SAFE_MACHINE-client.pem"
CA_FILE="$TLS_DIR/ca.crt"

if [ "$FORCE_KEY" -eq 1 ] || [ ! -s "$KEY_FILE" ]; then
  umask 077
  openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$KEY_FILE" >/dev/null 2>&1
  chmod 600 "$KEY_FILE" 2>/dev/null || true
fi

openssl req -new \
  -key "$KEY_FILE" \
  -out "$CSR_FILE" \
  -subj "/CN=$MACHINE_CODE/O=KYXN/OU=edge-gateway" >/dev/null 2>&1
chmod 644 "$CSR_FILE" 2>/dev/null || true

echo "private key: $KEY_FILE"
echo "csr: $CSR_FILE"

if [ -z "$PLATFORM_URL" ] && [ -z "$ENROLLMENT_TOKEN" ]; then
  echo "platform enrollment skipped"
  exit 0
fi
if [ -z "$PLATFORM_URL" ] || [ -z "$ENROLLMENT_TOKEN" ]; then
  echo "--platform-url and --token must be provided together" >&2
  exit 2
fi

python3 - "$PLATFORM_URL" "$ENROLLMENT_TOKEN" "$MACHINE_CODE" "$CSR_FILE" "$CERT_FILE" "$CA_FILE" "$KEY_FILE" "$APP_CONFIG" "$VALIDITY_DAYS" "$UPDATE_LOCAL_APP" <<'PY'
import json
import os
import sys
import urllib.error
import urllib.request

platform_url, token, machine_code, csr_file, cert_file, ca_file, key_file, app_config, validity_days, update_local_app = sys.argv[1:11]
with open(csr_file, "r", encoding="utf-8") as fh:
    csr_pem = fh.read()

payload = {
    "machineCode": machine_code,
    "csrPem": csr_pem,
    "certificateFile": "runtime/tls/" + os.path.basename(cert_file),
    "caBundleFile": "runtime/tls/" + os.path.basename(ca_file),
    "keyFile": "runtime/tls/" + os.path.basename(key_file),
    "updateAppConfig": False,
}
if validity_days:
    payload["validityDays"] = int(validity_days)

endpoint = platform_url.rstrip("/") + "/api/config/tls/enrollment/sign"
request = urllib.request.Request(
    endpoint,
    data=json.dumps(payload).encode("utf-8"),
    headers={
        "Content-Type": "application/json",
        "X-Edge-Tls-Enrollment-Token": token,
    },
    method="POST",
)
try:
    with urllib.request.urlopen(request, timeout=30) as response:
        body = response.read().decode("utf-8")
except urllib.error.HTTPError as exc:
    body = exc.read().decode("utf-8", errors="replace")
    raise SystemExit(f"platform sign request failed: HTTP {exc.code}: {body}")

result = json.loads(body)
if not result.get("success"):
    raise SystemExit("platform sign request failed: " + json.dumps(result, ensure_ascii=False))

os.makedirs(os.path.dirname(cert_file), exist_ok=True)
with open(cert_file, "w", encoding="utf-8") as fh:
    fh.write(result.get("certificatePem", ""))
with open(ca_file, "w", encoding="utf-8") as fh:
    fh.write(result.get("caPem", ""))
os.chmod(cert_file, 0o644)
os.chmod(ca_file, 0o644)

if update_local_app == "1":
    with open(app_config, "r", encoding="utf-8") as fh:
        app = json.load(fh)
    mqtt = app.setdefault("mqtt", {})
    tls = mqtt.setdefault("tls", {})
    tls["enabled"] = True
    tls["caFile"] = ca_file
    tls["certFile"] = cert_file
    tls["keyFile"] = key_file
    tls["insecureSkipVerify"] = False
    with open(app_config, "w", encoding="utf-8") as fh:
        json.dump(app, fh, ensure_ascii=False, indent=2)
        fh.write("\n")

print("certificate: " + cert_file)
print("ca: " + ca_file)
PY

if [ "$RESTART_SERVICES" -eq 1 ]; then
  if command -v systemctl >/dev/null 2>&1; then
    systemctl restart gateway-services.service
  elif [ -x "$BASE_DIR/bin/gateway-services.sh" ]; then
    "$BASE_DIR/bin/gateway-services.sh" restart
  fi
fi
