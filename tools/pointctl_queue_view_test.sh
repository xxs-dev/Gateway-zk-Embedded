#!/bin/sh
# pointctl pending / pending-peek only look at the shared pending-write queue: a queued command must
# never disappear without a receipt because an operator listed the queue during a maintenance window.
set -eu
POINTCTL="$1"
SHM="pointctl_view_$$"
trap 'rm -f "/dev/shm/$SHM"' EXIT HUP INT TERM
"$POINTCTL" write --index 5 --value 1 --cmd VIEW_1 --shm "$SHM" >/dev/null
first=$("$POINTCTL" pending --shm "$SHM")
second=$("$POINTCTL" pending-peek --shm "$SHM")
printf '%s\n' "$first" | grep -q '^pending_count=1$' || { echo "pending lost the command: $first" >&2; exit 1; }
printf '%s\n' "$second" | grep -q '^pending_count=1$' || { echo "listing removed the command: $second" >&2; exit 1; }
printf '%s\n' "$second" | grep -q 'cmdId=VIEW_1 .* acceptedAt=[0-9]* queuedMs=[0-9]*$' ||
  { echo "queue entry lacks acceptedAt/queuedMs: $second" >&2; exit 1; }
results=$("$POINTCTL" results --index 5 --shm "$SHM")
printf '%s\n' "$results" | grep -q '^result_count=0$' || { echo "unexpected receipts: $results" >&2; exit 1; }
echo 'pointctl_queue_view_test passed'
