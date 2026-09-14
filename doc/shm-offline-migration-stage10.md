# Offline SHM v8/v9 to v10 migration

## Scope

`memory_point_store_migrate` is a Linux-only, explicitly offline tool. It shares
`src/memory_point_store_layout.hpp` with the actual runtime and layout tests.
For the supported native ABI, v8/v9/v10 have the same size and field offsets;
v10 interprets a pending-write reserved byte as the durable-control flag.
The tool refuses ANY queued/occupied pending write and changes only the
four-byte version field. It does not create, unlink, resize, compact or replay
the segment. No broker, device, database or control-ledger access is performed.

Latest values (including offline placeholders), persistent history ring,
point-update ring, completed writebacks, counters, owner/claim records and
mutex bytes are retained exactly. Expired owners/claims are not renewed or
cleared by migration; normal runtime lease expiry still applies after restart.
This is not a conversion between CPU architectures, libc ABIs or arbitrary
historical layout forks. Build and run on the same ABI as the participating
device binaries. The backup is a native binary image, not a portable export.

## Required Preconditions

1. Schedule downtime. Disable all automatic restarts, watchdogs and supervisor
   launch paths, and stop ALL readers and writers of the named segment. This
   includes protocol drivers, MQTT/forwarder, HTTP/API, displays, event/compute
   engines, EMS/AGC and diagnostic utilities, not just the registered owners.
2. Reconcile outstanding device controls before shutdown. The tool never
   drains or replays old commands. A queue that was already drained does not
   prove the physical operation completed. Unknown controls remain unknown;
   migration does not resolve or redispatch them.
3. Run as root in the device's HOST PID and mount namespaces with full readable
   `/proc`. Containers, restricted procfs, hiding other participants, and
   cross-namespace migration are unsupported. Root alone does not prove these
   operational preconditions.
4. Wait for the last owner AND point-claim lease to expire (currently 30 seconds).
   Future heartbeat timestamps are rejected. A live mapping is rejected even
   when it has no owner record or its lease has expired.
5. Use a unique backup filename on verified persistent local storage with space
   for the entire segment. Supported filesystem types are ext-family, XFS and
   Btrfs. tmpfs, ramfs, overlay, network filesystems and other types are refused.
   The operator must still verify that the backing device is durable and healthy.
6. Deploy a mutually compatible v10 runtime set BEFORE resuming any participant.
   Include this stage's `main.cpp` and `iec_driver_main.cpp` startup changes:
   they now reattach directly instead of deleting an ownerless v10 segment.
   Otherwise the old startup cleanup can destroy the newly migrated data.
   Explicit/test cleanup APIs remain unchanged, including Stage 9 legacy protection.

## Invocation

Build the native target using the project's normal target-appropriate toolchain:

```sh
cmake --build BUILD_DIR --target memory_point_store_migrate
sudo ./BUILD_DIR/memory_point_store_migrate \
  --shm gateway_point_store \
  --backup /var/lib/gateway-backups/gateway_point_store-before-v10.bak \
  --offline-confirmed
```

The backup directory must already exist. Migrate each configured segment
explicitly; the tool does not discover or batch-migrate application config.
There is no `--force` or automatic rollback option. Exit 0 means backup and
final byte comparison passed; any refusal/IO error returns 1. Re-running on
v10 is refused, not treated as a new migration.

## Checks and Failure Handling

- Open existing SHM only, validate name, type, exact native size, magic and v8/v9.
- Take an exclusive advisory file lock. This serializes cooperating openers and
  migrations, but does not stop an already running process that ignores it.
- Enumerate `/proc/*/maps` by device and inode, not pathname substring; refuse
  matching mappings or unreadable process maps. Check leases, latest count and
  unique indexes, bounded ring cursors, and all pending-write occupancy slots.
- Require a mutex byte representation matching a native freshly initialized or
  normally lock/unlocked process-shared robust mutex. Locked, abandoned,
  nonrecoverable, damaged or unknown representations fail closed. The tool never
  locks or invokes owner-death recovery on the original mutex, since runtime
  recovery would clear the payload. Some other libc representations may require
  separate qualification; refusal is intentional, not permission to reset it.
- Create backup with `O_EXCL|O_NOFOLLOW`, mode 0600. Write all bytes, `fsync` the
  file and parent directory, then read back and compare the complete image.
  An existing backup is never overwritten. A partial/failed backup is retained
  as evidence and must not be treated as a valid recovery image.
- Recheck the named inode/size, mappings and exact source image after backup.
  Only then write version 10, sync, and compare ALL bytes against the original
  with only its version updated.

No scanning scheme here eliminates the race with a newly starting process.
The offline/restart interlock is an explicit operator prerequisite, not a
guarantee inferred from `/proc`. A reboot also discards SHM; do not reboot
between migration and validation expecting the in-memory data to survive.

On failure, keep services stopped and retain both the original segment and
backup evidence. All validation/backup failures occur before any original
write. A failure during/after the final version write can leave an uncertain
version; inspect the full image and verified backup before recovery. Do not
delete/recreate the segment, blindly copy a snapshot over a live mapping, or
restart legacy binaries. Snapshot restoration is an offline engineering
operation, not an automatic command in this tool.

## Regression Evidence

Local Linux x86_64 WSL tests use a private mount/PID/network/IPC namespace,
private tmpfs `/dev/shm` and `/opt`, and ext4 backup storage. No device or
external service is used. Example from this worktree (paths may be adjusted):

```sh
cmake -S . -B /var/tmp/gateway-shm-migration-stage10-build -DBUILD_TESTING=ON
cmake --build /var/tmp/gateway-shm-migration-stage10-build \
  --target memory_point_store_migrate memory_point_store_migration_test \
  memory_point_store_reader_cache_test -j4
sudo unshare --mount --pid --fork --mount-proc --net --ipc bash -c '
  set -eu
  mount --make-rprivate /
  mount -t tmpfs tmpfs /dev/shm
  mount -t tmpfs tmpfs /opt
  mkdir -p /var/tmp/gateway-shm-migration-stage10-backups
  export GATEWAY_MIGRATION_TEST_BACKUP_DIR=/var/tmp/gateway-shm-migration-stage10-backups
  ctest --test-dir /var/tmp/gateway-shm-migration-stage10-build \
    -R "^memory_point_store_(migration|reader_cache)$" --output-on-failure
'
```

Without root and the explicit backup test directory, the migration CTest is
reported SKIPPED (77), not passed. Fixtures are created with the real v8/v9
runtime constructors; raw layout fields seed wrapped history and expired
ownership. Tests compare exact original/backup/migrated bytes and inode,
then reopen via the production config constructor. Refusals include live
mapping, competing flock, bad header/cursors/counts/duplicate index, pending
writes, active/future leases, bad/abandoned robust mutex, existing/symlink
backup, nonpersistent backup and deliberately truncated backup writes.

Stage 10 final local verification (2026-09-14): CMake targets
`memory_point_store_migrate`, `memory_point_store_migration_test`,
`memory_point_store_lifecycle_test`, `memory_point_store_reader_cache_test`,
`control_dedup_integration_test`, `ModbusRtu` and `IecDriver` built successfully.
The three memory-point-store CTests passed (3/3, 1.23 seconds), followed by
`control_dedup_integration` (1/1, 0.49 seconds, including v8/v9 durable rejection
and legacy startup-cleanup preservation). No failures or skips. The migration
implementation and CLI also passed `-Wall -Wextra -Werror` syntax compilation.
Final JUnit evidence is in the local WSL build directory as `migration-final.xml`
and `dedup-final.xml`; generated binaries/evidence are not source commits.

Device-native AArch64 compilation and T536 execution were NOT performed in this
stage. They remain release gates, especially for the native mutex representation.
No main-worktree integration, push or deployment is implied by these local tests.
