#!/bin/sh
set -eu

REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
BINARY="${1:?native ota_final_state_test required}"
SCENARIO="${2:-all}"
ROOT="$(mktemp -d /tmp/ota-lifecycle.XXXXXX)"
INSTALL=/opt/modbus-gateway
cleanup() {
  result=$?
  case "$ROOT" in /tmp/ota-lifecycle.*) rm -rf -- "$ROOT" ;; esac
  if [ "${INSTALL_CREATED:-0}" = 1 ]; then
    [ ! -L "$INSTALL" ] && [ "$(realpath -e -- "$INSTALL")" = /opt/modbus-gateway ] && rm -rf -- "$INSTALL"
  fi
  return "$result"
}
trap cleanup EXIT INT TERM
[ "$(id -u)" = 0 ] || { echo "requires isolated root namespace" >&2; exit 1; }
[ ! -e "$INSTALL" ] && [ ! -L "$INSTALL" ] || { echo "installation path must be disposable" >&2; exit 1; }
mkdir -p "$INSTALL/bin" "$INSTALL/config" "$ROOT/package"
INSTALL_CREATED=1
for name in ota-apply.sh ota-rollback.sh ota-health-result.py runtime-upgrade-guard.py; do
  cp "$REPO_ROOT/deploy/$name" "$INSTALL/bin/$name"
done
printf 'unchanged\n' > "$INSTALL/config/lifecycle-sentinel.txt"
printf '{broken\n' > "$ROOT/package/manifest.json"

run_case() {
  mode="$1"
  case_root="$ROOT/$mode"
  mkdir -p "$case_root/shims" "$case_root/watchdog"
  tar -C "$ROOT/package" -czf "$case_root/source.tar.gz" manifest.json
  if [ "$mode" = scada-marker-kill ]; then
    mkdir -p "$INSTALL/config/runtime"
    printf '{"machineCode":"GW_TEST"}\n' > "$INSTALL/config/runtime/device_identity.json"
    python3 - "$case_root/source.kyscada" <<'PY'
import sys
import zipfile
with zipfile.ZipFile(sys.argv[1], "w") as output:
    output.writestr("project.json", "{}\n")
PY
    cat > "$INSTALL/bin/install-scada-project.sh" <<'SH'
#!/bin/sh
ps -o pgid= -p $$ > "$LIFECYCLE_TEST_ROOT/apply.pgid"
sleep 60
exit 1
SH
    chmod 0755 "$INSTALL/bin/install-scada-project.sh"
  elif [ "$mode" = missing-helper ]; then
    mv "$INSTALL/bin/ota-health-result.py" "$case_root/helper-held.py"
  elif [ "$mode" = missing-python ]; then
    printf '#!/bin/sh\necho "fixture python3 unavailable" >&2\nexit 127\n' > "$case_root/shims/python3"
    chmod 0755 "$case_root/shims/python3"
  elif [ "$mode" = early-kill ]; then
    cat > "$case_root/shims/cp" <<'SH'
#!/bin/sh
ps -o pgid= -p $$ > "$LIFECYCLE_TEST_ROOT/apply.pgid"
sleep 60
exec /bin/cp "$@"
SH
    chmod 0755 "$case_root/shims/cp"
  elif [ "$mode" = registration-kill ] || [ "$mode" = pending-kill ] || [ "$mode" = manifest-marker-kill ]; then
    cat > "$case_root/shims/python3" <<'SH'
#!/bin/sh
if { [ "$LIFECYCLE_TEST_MODE" = registration-kill ] && [ "${7:-}" = prepared ]; } ||
   { [ "$LIFECYCLE_TEST_MODE" = pending-kill ] && [ "${2:-}" = --activate ]; }; then
  ps -o pgid= -p $$ > "$LIFECYCLE_TEST_ROOT/apply.pgid"
  sleep 60
fi
if [ "$LIFECYCLE_TEST_MODE" = manifest-marker-kill ] && [ -n "${OTA_IN_PROGRESS_FILE:-}" ]; then
  exec /usr/bin/python3 "$LIFECYCLE_TEST_ROOT/trace-manifest.py" "$@"
fi
exec /usr/bin/python3 "$@"
SH
    chmod 0755 "$case_root/shims/python3"
    if [ "$mode" = manifest-marker-kill ]; then
      mkdir -p "$case_root/valid-package"
      printf 'new\n' > "$case_root/valid-package/payload.txt"
      python3 - "$case_root/valid-package" <<'PY'
import hashlib
import json
import os
import sys
root = sys.argv[1]
with open(os.path.join(root, "payload.txt"), "rb") as source:
    digest = hashlib.sha256(source.read()).hexdigest()
with open(os.path.join(root, "manifest.json"), "w") as output:
    json.dump(dict(packageType="config", version="2.0.0",
        files=[dict(path="payload.txt", target="/opt/modbus-gateway/config/lifecycle-sentinel.txt", sha256=digest)],
        restart=dict(services=[])), output)
PY
      tar -C "$case_root/valid-package" -czf "$case_root/source.tar.gz" manifest.json payload.txt
      cat > "$case_root/trace-manifest.py" <<'PY'
import os
import sys
import time
code = sys.stdin.read()
sys.argv = sys.argv[1:]
def gate(frame, event, arg):
    if event == "line" and frame.f_code.co_filename == "official-manifest" and os.path.isfile(os.environ["OTA_IN_PROGRESS_FILE"]):
        with open(os.path.join(os.environ["LIFECYCLE_TEST_ROOT"], "apply.pgid"), "w") as output:
            output.write(str(os.getpgrp()) + "\n")
        while True:
            time.sleep(1)
    return gate
sys.settrace(gate)
exec(compile(code, "official-manifest", "exec"), {"__name__": "__main__"})
PY
    fi
  fi
  PATH="$case_root/shims:$PATH" LIFECYCLE_TEST_ROOT="$case_root" LIFECYCLE_TEST_MODE="$mode" WATCHDOG_RUN_DIR="$case_root/watchdog" \
    "$BINARY" --official-early-failure "$case_root" "$mode"
}
case "$SCENARIO" in
  all)
    for mode in missing-python early-kill invalid-manifest missing-helper registration-kill pending-kill scada-marker-kill manifest-marker-kill; do
      run_case "$mode"
    done
    ;;
  missing-python|early-kill|invalid-manifest|missing-helper|registration-kill|pending-kill|scada-marker-kill|manifest-marker-kill) run_case "$SCENARIO" ;;
  *) echo "unknown scenario" >&2; exit 2 ;;
esac
echo 'official lifecycle isolation test passed'
