#!/bin/sh
# Mixed-version check of the shared pending-write queue (SHM v11): runs shm_pending_write_mix built
# from the current tree against the same tool built from earlier releases, on one segment at a time.
#
#   tools/shm_pending_write_mix_check.sh <current-tool> <old-tool> [<old-tool>...]
#
# Building an old tool (example for dabb8fd, outside the working tree, no device access):
#   git worktree add --detach /tmp/edge-dabb8fd dabb8fd
#   cmake -S /tmp/edge-dabb8fd -B /tmp/edge-dabb8fd/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
#   ninja -C /tmp/edge-dabb8fd/build -j1 edge_gateway
#   g++ -std=c++17 -O2 -I/tmp/edge-dabb8fd/include -I/tmp/edge-dabb8fd/src tools/shm_pending_write_mix.cpp \
#       /tmp/edge-dabb8fd/build/libedge_gateway.a -lpthread -ldl -lrt -o /tmp/pwmix-dabb8fd
# Exit status 0 only when every scenario behaves as expected.
set -u

[ $# -ge 2 ] || { echo "usage: $0 <current-tool> <old-tool> [<old-tool>...]" >&2; exit 2; }
NEW=$1
shift
failures=0
scenario=""
shm=""
out=""

now_ms() { date +%s%3N; }

begin() {
  scenario=$1
  shm="pwmix_$$_$(echo "$scenario" | tr -c 'A-Za-z0-9' '_')"
  rm -f "/dev/shm/$shm"
  rm -rf "/tmp/pwmix-$shm"
  out=""
}

run() {
  line=$("$@" 2>&1)
  out="$out
$line"
}

expect() {
  if printf '%s\n' "$out" | grep -q -- "$1"; then return 0; fi
  echo "FAIL [$scenario] missing: $1"
  printf '%s\n' "$out" | sed 's/^/    /'
  failures=$((failures + 1))
  return 1
}

refuse() {
  if printf '%s\n' "$out" | grep -q -- "$1"; then
    echo "FAIL [$scenario] unexpected: $1"
    printf '%s\n' "$out" | sed 's/^/    /'
    failures=$((failures + 1))
    return 1
  fi
  return 0
}

finish() {
  rm -f "/dev/shm/$shm"
  rm -rf "/tmp/pwmix-$shm"
  echo "done [$scenario]"
}

for OLD in "$@"; do
  tag=$(basename "$OLD")

  begin "$tag old-producer new-consumer"
  t=$(now_ms)
  run "$OLD" produce "$shm" OLD1 11 "$t"
  run "$NEW" raw "$shm"
  run "$NEW" consume "$shm" "$t" 10000 11
  run "$NEW" result "$shm" OLD1
  expect "RAW slot=0 cmdId=OLD1 occupied=1"
  refuse "EXEC cmdId=OLD1"
  expect "RESULT cmdId=OLD1 stage=expired success=0"
  printf '%s\n' "$out" | sed -n 's/^RAW .*cmdId=OLD1 \(.*\)$/    observed old-release slot: \1/p'
  finish

  begin "$tag old-producer fresh-looking padding"
  t=$(now_ms)
  run "$OLD" produce "$shm" OLD2 11 "$t"
  mono=$("$NEW" now | sed -n 's/^MONOTONIC //p')
  run "$NEW" raw "$shm" gap "$mono"
  run "$NEW" pending "$shm"
  run "$NEW" consume "$shm" "$t" 10000 11
  run "$NEW" result "$shm" OLD2
  expect "PENDING build=current cmdId=OLD2 index=11 stamp=0"
  refuse "EXEC cmdId=OLD2"
  expect "RESULT cmdId=OLD2 stage=expired success=0"
  finish

  begin "$tag old-producer random padding"
  t=$(now_ms)
  run "$OLD" produce "$shm" OLD3 11 "$t"
  run "$NEW" raw "$shm" gap 0xdeadbeef
  run "$NEW" consume "$shm" "$t" 10000 11
  run "$NEW" result "$shm" OLD3
  refuse "EXEC cmdId=OLD3"
  expect "RESULT cmdId=OLD3 stage=expired success=0"
  finish

  begin "$tag new-producer old-consumer (rollback)"
  t=$(now_ms)
  run "$NEW" produce "$shm" NEW1 11 "$t"
  run "$NEW" produce "$shm" NEW2 11 $((t - 20000))
  run "$NEW" raw "$shm"
  run "$OLD" pending "$shm"
  run "$OLD" consume "$shm" "$t" 10000 11
  run "$OLD" result "$shm" NEW1
  run "$OLD" result "$shm" NEW2
  expect "RAW slot=0 cmdId=NEW1 occupied=2"
  expect "PENDING build=old cmdId=NEW1 index=11"
  expect "EXEC cmdId=NEW1"
  expect "RESULT cmdId=NEW1 stage=writeback-succeeded success=1"
  refuse "EXEC cmdId=NEW2"
  expect "RESULT cmdId=NEW2 stage=expired success=0"
  finish

  begin "$tag queue left over across an upgrade"
  t=$(now_ms)
  run "$OLD" produce "$shm" LEFT_OLD 11 "$t"
  run "$NEW" produce "$shm" LEFT_NEW 11 "$t"
  run "$NEW" consume "$shm" "$t" 10000 11
  run "$NEW" result "$shm" LEFT_OLD
  run "$NEW" result "$shm" LEFT_NEW
  refuse "EXEC cmdId=LEFT_OLD"
  expect "RESULT cmdId=LEFT_OLD stage=expired success=0"
  expect "EXEC cmdId=LEFT_NEW"
  expect "RESULT cmdId=LEFT_NEW stage=writeback-succeeded success=1"
  finish

  begin "$tag queue left over across a rollback, kept by an old driver"
  t=$(now_ms)
  run "$NEW" produce "$shm" KEPT_FRESH 21 "$t"
  run "$NEW" produce "$shm" KEPT_STALE 21 "$t" 20000
  run "$NEW" produce "$shm" OWN_OLD 11 "$t"
  # An old driver owning index 11 drains only its own command and rewrites the others into the ring.
  run "$OLD" consume "$shm" "$t" 10000 11
  run "$NEW" raw "$shm"
  run "$NEW" pending "$shm"
  run "$NEW" consume "$shm" "$t" 10000 21
  run "$NEW" result "$shm" KEPT_FRESH
  run "$NEW" result "$shm" KEPT_STALE
  expect "EXEC cmdId=OWN_OLD"
  refuse "EXEC cmdId=KEPT_STALE"
  expect "RAW slot=0 cmdId=KEPT_FRESH occupied=2"
  expect "RAW slot=1 cmdId=KEPT_STALE occupied=2"
  expect "EXEC cmdId=KEPT_FRESH"
  expect "RESULT cmdId=KEPT_FRESH stage=writeback-succeeded success=1"
  expect "RESULT cmdId=KEPT_STALE stage=expired success=0"
  finish

  begin "$tag receipts written by the current release, read by the old release"
  run "$NEW" putresult "$shm" R_MISMATCH 31 writeback-mismatch 0 0 1
  run "$NEW" putresult "$shm" R_UNCONFIRMED 31 writeback-unconfirmed 0 0 0
  run "$NEW" putresult "$shm" R_TIMEOUT 31 writeback-timeout 0 1 0
  run "$NEW" putresult "$shm" R_FAILED 31 writeback-failed 0 0 0
  run "$NEW" putresult "$shm" R_OK 31 writeback-succeeded 1 0 0
  run "$NEW" rawresult "$shm"
  for receipt in R_MISMATCH R_UNCONFIRMED R_TIMEOUT R_FAILED R_OK; do
    run "$OLD" readresult "$shm" "$receipt" 31
  done
  expect "RAWRESULT cmdId=R_MISMATCH flags=0xc occupied=2"
  for receipt in R_MISMATCH R_UNCONFIRMED R_TIMEOUT; do
    for path in lookup recent; do
      expect "path=$path cmdId=$receipt success=0"
      # Never success, never a definite (retry-safe) failure: 29f5068 says unknown, dabb8fd has no outcome.
      refuse "path=$path cmdId=$receipt .*outcome=failed"
    done
  done
  expect "path=lookup cmdId=R_OK success=1"
  expect "path=lookup cmdId=R_FAILED success=0"
  printf '%s\n' "$out" | sed -n 's/^READ build=old path=lookup /    old reader: /p'
  finish

  begin "$tag receipts written by the old release, read by the current release"
  run "$OLD" putresult "$shm" L_FAILED 32 writeback-failed 0 0 0 "modbus response timeout"
  run "$OLD" putresult "$shm" L_VERIFY 32 writeback-failed 0 0 0 "verify failed: wrote 7, read back 5"
  run "$OLD" putresult "$shm" L_TIMEOUT 32 writeback-timeout 0 1 0
  run "$OLD" putresult "$shm" L_EXPIRED 32 expired 0 0 0
  run "$OLD" putresult "$shm" L_REJECTED 32 control-rejected 0 0 0
  run "$OLD" putresult "$shm" L_OK 32 writeback-succeeded 1 0 0
  run "$NEW" rawresult "$shm"
  for receipt in L_FAILED L_VERIFY L_TIMEOUT L_EXPIRED L_REJECTED L_OK; do
    run "$NEW" readresult "$shm" "$receipt" 32
  done
  expect "RAWRESULT cmdId=L_FAILED flags=0x0 occupied=1"
  for path in lookup recent; do
    # An old writeback-failed may have reached the device (timeout, read-back mismatch): unknown.
    expect "path=$path cmdId=L_FAILED success=0 unknown=1 .*stage=writeback-failed outcome=unknown"
    expect "path=$path cmdId=L_VERIFY success=0 unknown=1 .*outcome=unknown"
    expect "path=$path cmdId=L_TIMEOUT success=0 unknown=1 .*outcome=unknown"
    expect "path=$path cmdId=L_EXPIRED .*outcome=failed"
    expect "path=$path cmdId=L_REJECTED .*outcome=failed"
    expect "path=$path cmdId=L_OK success=1 .*outcome=succeeded"
  done
  finish

  begin "$tag segment created by either release"
  t=$(now_ms)
  run "$OLD" produce "$shm" CREATED_OLD 11 "$t"
  run "$NEW" version "$shm"
  run "$NEW" consume "$shm" "$t" 10000 11
  expect "VERSION 11"
  expect "CONSUMED build=current"
  finish
  begin "$tag segment created by the current release"
  t=$(now_ms)
  run "$NEW" produce "$shm" CREATED_NEW 11 "$t"
  run "$OLD" version "$shm"
  run "$OLD" consume "$shm" "$t" 10000 11
  expect "VERSION 11"
  expect "EXEC cmdId=CREATED_NEW"
  finish
done

if [ "$failures" -ne 0 ]; then
  echo "shm pending-write mixed-version check: $failures failed expectation(s)"
  exit 1
fi
echo "shm pending-write mixed-version check passed"
