#!/bin/sh
if [ -z "${BASH_VERSION:-}" ]; then
  exec bash "$0" "$@"
fi
set -euo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
CELLULAR_SCRIPT="$ROOT_DIR/deploy/gateway-cellular.sh"
FAILOVER_SCRIPT="$ROOT_DIR/deploy/gateway-network-failover.sh"
SZRL_HELPER="$ROOT_DIR/deploy/gateway-szrl-idempotency.sh"
TMP_ROOT=$(mktemp -d)
SUPERVISOR_PID=""

cleanup() {
  if [ -n "$SUPERVISOR_PID" ]; then
    kill -TERM "$SUPERVISOR_PID" 2>/dev/null || true
    wait "$SUPERVISOR_PID" 2>/dev/null || true
  fi
  rm -rf "$TMP_ROOT"
}
trap cleanup EXIT HUP INT TERM

fail() {
  echo "[FAIL] $*" >&2
  exit 1
}

assert_contains() {
  grep -F "$2" "$1" >/dev/null 2>&1 || fail "$1 does not contain: $2"
}

assert_not_contains() {
  if grep -F "$2" "$1" >/dev/null 2>&1; then
    fail "$1 unexpectedly contains: $2"
  fi
}

wait_for_count() {
  file="$1"
  pattern="$2"
  expected="$3"
  attempts=0
  while [ "$attempts" -lt 80 ]; do
    count=$(grep -cF "$pattern" "$file" 2>/dev/null || true)
    [ "$count" -ge "$expected" ] && return 0
    sleep 0.1
    attempts=$((attempts + 1))
  done
  fail "timed out waiting for $expected occurrences of '$pattern' in $file"
}

setup_cellular_case() {
  case_dir="$TMP_ROOT/cellular"
  mkdir -p "$case_dir/bin" "$case_dir/sys/usb0"
  printf '10\n' > "$case_dir/sys/usb0/ifindex"
  printf '192.168.43.100\n' > "$case_dir/address"
  : > "$case_dir/actions"
  : > "$case_dir/dhcp-script"
  chmod +x "$case_dir/dhcp-script"

  cat > "$case_dir/bin/id" <<'EOF'
#!/bin/sh
printf '0\n'
EOF
  cat > "$case_dir/bin/logger" <<'EOF'
#!/bin/sh
exit 0
EOF
  cat > "$case_dir/bin/ip" <<'EOF'
#!/bin/sh
echo "ip $*" >> "$MOCK_ACTIONS"
if [ "$1" = "link" ] && [ "$2" = "show" ]; then
  [ -f "$MOCK_SYS_ROOT/usb0/ifindex" ]
elif [ "$1" = "link" ] && [ "$2" = "set" ]; then
  exit 0
elif [ "$1" = "-4" ] && [ "$2" = "-o" ] && [ "$3" = "address" ]; then
  if [ -s "$MOCK_ADDRESS_FILE" ]; then
    printf '7: usb0    inet %s/24 scope global usb0\n' "$(cat "$MOCK_ADDRESS_FILE")"
  fi
fi
EOF
  cat > "$case_dir/bin/udhcpc" <<'EOF'
#!/bin/sh
echo "udhcpc start pid=$$ $*" >> "$MOCK_ACTIONS"
pid_file=""
while [ "$#" -gt 0 ]; do
  if [ "$1" = "-p" ]; then
    shift
    pid_file="$1"
  fi
  shift
done
[ -z "$pid_file" ] || printf '%s\n' "$$" > "$pid_file"
trap 'echo "udhcpc stop pid=$$" >> "$MOCK_ACTIONS"; exit 0' INT TERM
while :; do /bin/sleep 1; done
EOF
  chmod +x "$case_dir/bin/"*

  MOCK_ACTIONS="$case_dir/actions" \
  MOCK_SYS_ROOT="$case_dir/sys" \
  MOCK_ADDRESS_FILE="$case_dir/address" \
  PATH="$case_dir/bin:$PATH" \
  CONFIG_FILE="$case_dir/missing.conf" \
  RUNTIME_CONFIG_FILE="$case_dir/missing.json" \
  SYS_CLASS_NET_ROOT="$case_dir/sys" \
  CELLULAR_INTERFACE_STABLE_SEC=1 \
  CELLULAR_INTERFACE_WAIT_SEC=5 \
  CELLULAR_MONITOR_INTERVAL_SEC=1 \
  CELLULAR_ADDRESS_WAIT_SEC=3 \
  CELLULAR_DHCP_RESTART_DELAY_SEC=1 \
  CELLULAR_DHCP_STOP_TIMEOUT_SEC=2 \
  CELLULAR_DHCP_SCRIPT="$case_dir/dhcp-script" \
  CELLULAR_DHCP_PID_FILE="$case_dir/udhcpc.pid" \
  bash "$CELLULAR_SCRIPT" run > "$case_dir/output" 2>&1 &
  SUPERVISOR_PID=$!

  wait_for_count "$case_dir/actions" "udhcpc start" 1
  printf '11\n' > "$case_dir/sys/usb0/ifindex"
  wait_for_count "$case_dir/actions" "udhcpc start" 2
  assert_contains "$case_dir/output" "cellular interface was recreated (oldIfindex=10 newIfindex=11)"
  assert_contains "$case_dir/actions" "udhcpc stop"

  kill -TERM "$SUPERVISOR_PID"
  wait "$SUPERVISOR_PID" 2>/dev/null || true
  SUPERVISOR_PID=""
}

setup_failover_case() {
  case_dir="$TMP_ROOT/failover"
  mkdir -p "$case_dir/bin" "$case_dir/state"
  : > "$case_dir/actions"

  cat > "$case_dir/bin/logger" <<'EOF'
#!/bin/sh
exit 0
EOF
  cat > "$case_dir/bin/resolvectl" <<'EOF'
#!/bin/sh
echo "resolvectl $*" >> "$MOCK_ACTIONS"
exit 0
EOF
  cat > "$case_dir/bin/timeout" <<'EOF'
#!/bin/sh
shift
"$@"
EOF
  cat > "$case_dir/bin/ping" <<'EOF'
#!/bin/sh
[ "$3" = "ens1" ] || [ "$2" = "ens1" ]
EOF
  cat > "$case_dir/bin/ip" <<'EOF'
#!/bin/sh
echo "ip $*" >> "$MOCK_ACTIONS"
case "$*" in
  "-4 -o address show dev ens1 scope global")
    printf '2: ens1    inet 192.168.1.20/24 scope global ens1\n'
    ;;
  "-4 -o address show dev usb0 scope global")
    printf '7: usb0    inet 192.168.43.100/24 scope global usb0\n'
    ;;
  "-4 route show default dev ens1")
    printf 'default via 192.168.1.1 dev ens1 metric 100\n'
    ;;
  "-4 route show default dev usb0")
    ;;
  "link show dev usb0"|"link show dev ens1")
    exit 0
    ;;
  "-4 route replace "*|"-4 route add "*)
    exit 0
    ;;
  "-4 route del "*)
    exit 1
    ;;
esac
exit 0
EOF
  chmod +x "$case_dir/bin/"*

  MOCK_ACTIONS="$case_dir/actions" \
  PATH="$case_dir/bin:$PATH" \
  CONFIG_FILE="$case_dir/missing.conf" \
  RUNTIME_CONFIG_FILE="$case_dir/missing.json" \
  FAILOVER_ENABLED=true \
  PREFER_CELLULAR=true \
  CELLULAR_INTERFACE=usb0 \
  WIRED_INTERFACE=ens1 \
  WIRED_INTERFACES=ens1 \
  FAILURE_THRESHOLD=1 \
  RECOVERY_THRESHOLD=1 \
  PROBE_TIMEOUT_SEC=1 \
  CHECK_INTERVAL_SEC=1 \
  STATE_DIR="$case_dir/state" \
  STATE_FILE="$case_dir/state/state" \
  LOG_FILE="$case_dir/failover.log" \
  bash "$FAILOVER_SCRIPT" once > "$case_dir/output" 2>&1

  assert_contains "$case_dir/actions" "ip -4 route replace default via 192.168.1.1 dev ens1 metric 50"
  assert_not_contains "$case_dir/actions" "ip -4 route del default"
  assert_not_contains "$case_dir/actions" "via 192.168.43.1 dev usb0"
  assert_contains "$case_dir/state/state" "mode=wired"
}

setup_stable_elapsed_case() {
  case_dir="$TMP_ROOT/stable-elapsed"
  mkdir -p "$case_dir/sys/usb0"
  printf '20\n' > "$case_dir/sys/usb0/ifindex"
  (
    export GATEWAY_CELLULAR_SOURCE_ONLY=1
    export CONFIG_FILE="$case_dir/missing.conf"
    export RUNTIME_CONFIG_FILE="$case_dir/missing.json"
    export SYS_CLASS_NET_ROOT="$case_dir/sys"
    export CELLULAR_INTERFACE=usb0
    export CELLULAR_INTERFACE_STABLE_SEC=1
    export CELLULAR_INTERFACE_WAIT_SEC=2
    # shellcheck source=/dev/null
    . "$CELLULAR_SCRIPT"
    sleep_calls=0
    sleep() { sleep_calls=$((sleep_calls + 1)); }
    ip() { return 0; }
    wait_for_stable_interface
    [ "$sleep_calls" -eq 1 ] || fail "stableSec=1 must include one full elapsed second; sleepCalls=$sleep_calls"
  )
}

write_fake_stat() {
  target="$1"
  pid="$2"
  start_time="$3"
  line="$pid (udhcpc) S"
  field=4
  while [ "$field" -lt 22 ]; do
    line="$line 0"
    field=$((field + 1))
  done
  printf '%s %s 0\n' "$line" "$start_time" > "$target"
}

setup_pid_reuse_case() {
  case_dir="$TMP_ROOT/pid-reuse"
  mkdir -p "$case_dir/proc"
  /bin/sleep 30 &
  victim_pid=$!
  trap 'kill "$victim_pid" 2>/dev/null || true; wait "$victim_pid" 2>/dev/null || true' HUP INT TERM
  mkdir -p "$case_dir/proc/$victim_pid"
  write_fake_stat "$case_dir/proc/$victim_pid/stat" "$victim_pid" 222
  printf 'udhcpc\0-f\0-i\0usb0\0' > "$case_dir/proc/$victim_pid/cmdline"
  (
    export GATEWAY_CELLULAR_SOURCE_ONLY=1
    export CONFIG_FILE="$case_dir/missing.conf"
    export RUNTIME_CONFIG_FILE="$case_dir/missing.json"
    export PROC_ROOT="$case_dir/proc"
    export CELLULAR_INTERFACE=usb0
    export CELLULAR_DHCP_PID_FILE="$case_dir/udhcpc.pid"
    printf '%s\n' "$victim_pid" > "$CELLULAR_DHCP_PID_FILE"
    # shellcheck source=/dev/null
    . "$CELLULAR_SCRIPT"
    stop_dhcp_pid "$victim_pid" 111 0
  )
  kill -0 "$victim_pid" 2>/dev/null || fail "identity mismatch signalled an unrelated process"
  kill "$victim_pid" 2>/dev/null || true
  wait "$victim_pid" 2>/dev/null || true
  trap cleanup EXIT HUP INT TERM
}

setup_szrl_signature_cases() {
  case_dir="$TMP_ROOT/szrl"
  mkdir -p "$case_dir"
  printf '#!/bin/sh\nexit 0\n' > "$case_dir/supported-start.sh"
  supported_sha=$(sha256sum "$case_dir/supported-start.sh" | awk '{print $1}')
  cat > "$case_dir/supported.service" <<'EOF'
[Service]
ExecStart=/data/szrl/szrl_start.sh -n 2
KillMode=process
EOF
  SZRL_UNIT_PATHS="$case_dir/supported.service" \
  SZRL_DROPIN_SOURCE="$ROOT_DIR/deploy/10-szrl-start-idempotent.conf" \
  SZRL_DROPIN_PATH="$case_dir/supported.d/10-gateway-idempotent.conf" \
  SZRL_START_SCRIPT_PATH="$case_dir/supported-start.sh" \
  SZRL_START_SCRIPT_SHA256="$supported_sha" \
    sh "$SZRL_HELPER" install > "$case_dir/supported.out"
  assert_contains "$case_dir/supported.d/10-gateway-idempotent.conf" "RemainAfterExit=yes"

  cat > "$case_dir/unsupported.service" <<'EOF'
[Service]
Type=notify
ExecStart=/usr/bin/vendor-network-daemon
KillMode=control-group
EOF
  mkdir -p "$case_dir/unsupported.d"
  printf 'unsafe-old-dropin\n' > "$case_dir/unsupported.d/10-gateway-idempotent.conf"
  SZRL_UNIT_PATHS="$case_dir/unsupported.service" \
  SZRL_DROPIN_SOURCE="$ROOT_DIR/deploy/10-szrl-start-idempotent.conf" \
  SZRL_DROPIN_PATH="$case_dir/unsupported.d/10-gateway-idempotent.conf" \
  SZRL_START_SCRIPT_PATH="$case_dir/supported-start.sh" \
  SZRL_START_SCRIPT_SHA256="$supported_sha" \
    sh "$SZRL_HELPER" install > "$case_dir/unsupported.out" 2>&1
  [ ! -e "$case_dir/unsupported.d/10-gateway-idempotent.conf" ] || fail "unsupported vendor unit received the Gateway-zk drop-in"
  assert_contains "$case_dir/unsupported.out" "unsupported szrl_start.service signature"
}

setup_stable_elapsed_case
setup_pid_reuse_case
setup_cellular_case
setup_failover_case
setup_szrl_signature_cases

assert_contains "$ROOT_DIR/deploy/10-szrl-start-idempotent.conf" "Type=oneshot"
assert_contains "$ROOT_DIR/deploy/10-szrl-start-idempotent.conf" "RemainAfterExit=yes"
assert_contains "$ROOT_DIR/deploy/10-szrl-start-idempotent.conf" "KillMode=control-group"
assert_contains "$ROOT_DIR/deploy/install-factory-config.sh" 'gateway-szrl-idempotency.sh'

echo "[PASS] cellular recreation and failover route tests"
