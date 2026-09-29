#!/usr/bin/env bash
# Run locally ON the isolated lab host. No SSH, deployment, or legacy test launcher.
set -euo pipefail
mode=${1:---check-only}
if [[ "$mode" != --check-only && "$mode" != --execute && "$mode" != --inspect-options ]]; then
  echo 'Usage: event_store_soak_22_16.sh --check-only|--execute|--inspect-options LAB_DIR BINARY_SHA256 [--purpose soak|tool-selftest] [--suite soak|matrix] [--private-sqlite LIB_SHA256] [runner options]' >&2
  exit 2
fi
[[ $# -ge 3 ]] || { echo 'LAB_DIR and BINARY_SHA256 required' >&2; exit 2; }
lab=$2
binary_sha=$3
shift 3
library_sha=''
purpose=soak
suite=soak
runner_options=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --purpose)
      [[ $# -ge 2 && ( "$2" == soak || "$2" == tool-selftest ) ]] || exit 2
      purpose=$2; shift 2;;
    --suite)
      [[ $# -ge 2 && ( "$2" == soak || "$2" == matrix ) ]] || exit 2
      suite=$2; shift 2;;
    --private-sqlite)
      [[ $# -ge 2 && -z "$library_sha" && "$2" =~ ^[[:xdigit:]]{64}$ ]] || exit 2
      library_sha=${2,,}; shift 2;;
    --binary*|--output*|--purpose*|--suite*|--ephemeral-db*|--expect-sha256*|--sqlite-library*)
      echo "Launcher-owned option forbidden: $1" >&2; exit 2;;
    *) runner_options+=("$1"); shift;;
  esac
done
[[ "$suite" != matrix || ( "$purpose" == tool-selftest && ${#runner_options[@]} == 0 ) ]] || {
  echo 'matrix requires --purpose tool-selftest and no soak runner options' >&2; exit 2;
}
duration=86400
[[ "$purpose" != tool-selftest ]] || duration=60
if [[ "$mode" == --inspect-options ]]; then
  python3 -c 'import json,sys; print(json.dumps(dict(purpose=sys.argv[1],suite=sys.argv[2],defaultDuration=int(sys.argv[3]),runnerOptions=sys.argv[4:],execution=False)))' "$purpose" "$suite" "$duration" "${runner_options[@]}"
  exit 0
fi
lab=$(realpath -e -- "$lab")
[[ "$lab" == /opt/modbus-gateway/test-lab/event-store-soak-* && "$lab" != *$'\n'* ]] || {
  echo 'Refusing non-isolated lab directory' >&2; exit 2;
}
[[ $(id -u) == 0 && $(uname -m) == aarch64 ]] || {
  echo 'This launcher requires root on the aarch64 lab host' >&2; exit 2;
}
for command in unshare mount findmnt python3 sha256sum ip timeout awk; do command -v "$command" >/dev/null; done
for flag in --mount --pid --ipc --net --kill-child --mount-proc; do
  unshare --help | grep -q -- "$flag" || { echo "Missing unshare capability: $flag" >&2; exit 2; }
done
required_inputs=(EventStore event_store_capacity_test event_store_soak.py event_store_soak_journal.py event_store_soak_state.py
  event_store_soak_library.py event_store_soak_report.py event_store_delivery_ipc_test.py
  event_store_soak_22_16.py event_store_soak_22_16.sh event_store_migrate.py event_store_migrate_test.py
  event_store_migrate_history_test.py event_history_query.py event_history_query_test.py
  event_store_migrate_ipc_test mqtt_event_outbox.cpp config-example-mqtt-forward-disabled.json
  config-runtime-mqtt-service.json config-factory-mqtt-service.json)
for file in "${required_inputs[@]}" SHA256SUMS; do
  [[ -f "$lab/$file" && ! -L "$lab/$file" ]] || { echo "Missing regular lab input: $file" >&2; exit 2; }
done
# SHA256SUMS must cover all executable inputs and any selected private SQLite library.
(cd "$lab" && sha256sum -c --strict SHA256SUMS)
[[ $(sha256sum "$lab/EventStore" | cut -d ' ' -f1) == "$binary_sha" ]] || exit 2
for file in "${required_inputs[@]}"; do
  awk -v wanted="$file" '$2 == wanted || $2 == "*" wanted {n++} END {exit n != 1}' "$lab/SHA256SUMS" || exit 2
done
if [[ -n "$library_sha" ]]; then
  python3 -B "$lab/event_store_soak_library.py" "$lab" "$library_sha" >/dev/null
fi
echo 'Inputs verified. This is laboratory IPC only; production acceptance is not implied.'
[[ "$mode" == --execute ]] || exit 0
parent_namespaces=$(python3 -c 'import json,os; print(json.dumps({n:os.readlink("/proc/self/ns/"+n) for n in ("mnt","pid","ipc","net")}))')
# A fresh namespace always owns descendants. There is no non-isolated fallback.
exec unshare --mount --propagation private --pid --fork --kill-child=KILL --mount-proc --ipc --net \
  bash -s -- "$lab" "$binary_sha" "$library_sha" "$purpose" "$suite" "$duration" "$parent_namespaces" "${runner_options[@]}" <<'ISOLATED'
set -euo pipefail
lab=$1
binary_sha=$2
library_sha=$3
purpose=$4
suite=$5
duration=$6
parent_namespaces=$7
shift 7
python3 -c 'import json,os,sys; p=json.loads(sys.argv[1]); assert all(os.readlink("/proc/self/ns/"+n)!=v for n,v in p.items()), "namespace did not change"' "$parent_namespaces"
mount --make-rprivate /
# Preserve the lab as a writable submount before sealing the production tree.
mount --bind "$lab" "$lab"
mount --rbind /opt/modbus-gateway /opt/modbus-gateway
mount -o remount,bind,ro /opt/modbus-gateway
# Seal every production submount too; reject a nested mount inside the writable lab.
while IFS= read -r target; do
  [[ "$target" == "$lab" ]] && continue
  [[ "$target" != "$lab/"* ]] || { echo 'Unexpected nested lab mount' >&2; exit 2; }
  mount -o remount,bind,ro "$target"
done < <(findmnt -Rrn -o TARGET /opt/modbus-gateway)
mount -t tmpfs -o size=256m,nosuid,nodev tmpfs /tmp
mount -t tmpfs -o size=256m,nosuid,nodev tmpfs /var/tmp
mount -t tmpfs -o size=64m,nosuid,nodev tmpfs /dev/shm
ip link set lo up
[[ $(findmnt -n -o VFS-OPTIONS /opt/modbus-gateway) == *ro* ]]
[[ $(findmnt -n -o VFS-OPTIONS "$lab") == *rw* ]]
run="$lab/run-$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir "$run"
cat /proc/self/mountinfo > "$run/mountinfo.txt"
for ns in mnt pid ipc net; do readlink "/proc/self/ns/$ns"; done > "$run/namespaces.txt"
ip -j address > "$run/network.json"
cd "$lab"
library_options=()
if [[ -n "$library_sha" ]]; then
  private_library=$(python3 -B "$lab/event_store_soak_library.py" "$lab" "$library_sha")
  library_options=(--sqlite-library "$private_library")
fi
if [[ "$suite" == matrix ]]; then
  exec python3 -B "$lab/event_store_soak_22_16.py" matrix-local --lab "$lab" --output "$run/result" --parent-namespaces "$parent_namespaces"
fi
# Defaults can be overridden with later options. One storage profile per run.
exec python3 -B "$lab/event_store_soak.py" run --binary "$lab/EventStore" \
  --expect-sha256 "$binary_sha" --output "$run/result" --purpose "$purpose" --mode full \
  --duration "$duration" "${library_options[@]}" "$@"
ISOLATED
