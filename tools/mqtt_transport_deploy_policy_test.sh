#!/bin/sh
# Deploy-side MQTT transport policy: install-factory-config.sh refuses plaintext
# and insecureSkipVerify before touching the device, and
# production-smoke-test.sh --mqtt-transport reports what the client will refuse.
# Local files only; no broker is contacted.
set -u
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/mqtt-deploy-policy.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
FAILURES=0

expect() {
  description="$1"; expected_rc="$2"; needle="$3"; actual_rc="$4"; output="$5"
  if [ "$actual_rc" -eq "$expected_rc" ] && { [ -z "$needle" ] || printf '%s' "$output" | grep -F -- "$needle" >/dev/null; }; then
    echo "ok   $description"
  else
    echo "FAIL $description (rc=$actual_rc expected=$expected_rc)"
    printf '%s\n' "$output" | sed 's/^/     /'
    FAILURES=$((FAILURES + 1))
  fi
}

# --- install-factory-config.sh policy helpers, extracted verbatim -------------
awk '/^broker_implies_tls\(\) \{/,/^}/ { print } /^check_mqtt_transport_choice\(\) \{/,/^}/ { print }' \
  "$ROOT/deploy/install-factory-config.sh" > "$WORK/policy.sh"
grep -q '^check_mqtt_transport_choice()' "$WORK/policy.sh" || { echo "FAIL policy helper not found"; exit 1; }

run_policy() {
  ( . "$WORK/policy.sh"; check_mqtt_transport_choice "$@" ) 2>&1
}

out=$(run_policy "tcp://10.0.0.8:1883" false false false); rc=$?
expect "install refuses a plaintext broker by default" 2 "refusing plaintext MQTT broker tcp://10.0.0.8:1883" "$rc" "$out"
out=$(run_policy "10.0.0.8:1883" false false false); rc=$?
expect "install refuses a scheme-less broker by default" 2 "refusing plaintext MQTT broker" "$rc" "$out"
out=$(run_policy "tcp://10.0.0.8:1883" false true false); rc=$?
expect "install accepts plaintext only with the explicit experiment switch, with warning" 0 \
  "WARNING: INSECURE PLAINTEXT MQTT explicitly enabled" "$rc" "$out"
out=$(run_policy "ssl://broker.example.com:8883" false false false); rc=$?
expect "install accepts a TLS broker" 0 "" "$rc" "$out"
out=$(run_policy "tcp://broker.example.com:8883" true false false); rc=$?
expect "install accepts tls.enabled=true" 0 "" "$rc" "$out"
out=$(run_policy "" true false false); rc=$?
expect "install keeps an undeployed broker empty and warns" 0 "MQTT broker not configured" "$rc" "$out"
out=$(run_policy "ssl://broker.example.com:8883" true false true); rc=$?
expect "install refuses insecureSkipVerify=true" 2 "refusing MQTT tls.insecureSkipVerify=true" "$rc" "$out"
if grep -n 'tcp://127.0.0.1:1883' "$ROOT/deploy/install-factory-config.sh" "$ROOT/deploy/production-init.sh"; then
  echo "FAIL init scripts must not invent a default broker"; FAILURES=$((FAILURES + 1))
else
  echo "ok   init scripts do not invent a default broker"
fi

# --- production-smoke-test.sh --mqtt-transport --------------------------------
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout "$WORK/ca.key" \
  -out "$WORK/ca.crt" -days 400 -subj /CN=deploy-policy-test >/dev/null 2>&1 || {
  echo "SKIP smoke checks: openssl CLI unavailable"; [ "$FAILURES" -eq 0 ] && exit 0; exit 1; }
rm -f "$WORK/ca.key"
python3 - "$ROOT/config/factory/runtime/apps/mqtt-service.json" "$WORK" <<'PY'
import copy, json, sys
base = json.load(open(sys.argv[1], encoding="utf-8"))
work = sys.argv[2]
def write(name, **changes):
    data = copy.deepcopy(base)
    data["mqtt"].update({k: v for k, v in changes.items() if k != "tls"})
    data["mqtt"]["tls"].update(changes.get("tls", {}))
    json.dump(data, open(f"{work}/{name}.json", "w", encoding="utf-8"))
write("deployed", broker="ssl://broker.example.com:8883", tls={"caFile": f"{work}/ca.crt"})
write("missing-ca", broker="ssl://broker.example.com:8883", tls={"caFile": f"{work}/absent.crt"})
write("plain", broker="tcp://10.0.0.8:1883", tls={"enabled": False})
write("plain-experiment", broker="tcp://10.0.0.8:1883", allowInsecurePlaintext=True, tls={"enabled": False})
write("skip", broker="ssl://broker.example.com:8883", tls={"caFile": f"{work}/ca.crt", "insecureSkipVerify": True})
PY

smoke() {
  APP_CONFIG="$1" MONITOR_CONFIG="$WORK/none" CAMERA_CONFIG="$WORK/none" GATEWAY_HOME="$WORK/home" \
    sh "$ROOT/deploy/production-smoke-test.sh" --mqtt-transport 2>&1
}
out=$(smoke "$ROOT/config/factory/runtime/apps/mqtt-service.json"); rc=$?
expect "smoke: factory template is refused until a broker is deployed" 1 "broker not configured" "$rc" "$out"
out=$(smoke "$WORK/deployed.json"); rc=$?
expect "smoke: deployed TLS broker with CA passes" 0 "TLS CA file exists" "$rc" "$out"
out=$(smoke "$WORK/missing-ca.json"); rc=$?
expect "smoke: TLS without deployed CA fails" 1 "TLS CA file missing or unreadable" "$rc" "$out"
out=$(smoke "$WORK/plain.json"); rc=$?
expect "smoke: plaintext broker fails" 1 "is refused by the MQTT client" "$rc" "$out"
out=$(smoke "$WORK/plain-experiment.json"); rc=$?
expect "smoke: explicit plaintext experiment is not production-acceptable" 1 "EXPERIMENTAL INSECURE plaintext" "$rc" "$out"
out=$(smoke "$WORK/skip.json"); rc=$?
expect "smoke: insecureSkipVerify fails" 1 "insecureSkipVerify=true is not supported" "$rc" "$out"

if [ "$FAILURES" -eq 0 ]; then
  echo "mqtt_transport_deploy_policy_test passed"
  exit 0
fi
echo "mqtt_transport_deploy_policy_test FAILED failures=$FAILURES"
exit 1
