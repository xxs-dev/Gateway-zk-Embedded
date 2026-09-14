# Explicit Offline Latest Deduplication

The existing three-argument migration API remains strict. It refuses duplicates.
The options overload accepts `deduplicateLatest` and `checkOnly`, both false by
default. It returns original version, occupied counts, duplicate groups, winner
slot and removed slot numbers (zero-based), never sample values.

Run the same native CLI against ALL stopped segments before any real write:

```sh
memory_point_store_migrate --offline-confirmed --check --deduplicate-latest \
  --shm segment_a --shm segment_b
```

Check accepts repeated `--shm` for distinct names, inspects every requested
segment even if another fails, and returns nonzero if any fail. It opens sources
read-only, creates no backup, and performs no writes. `--backup` with `--check`
is rejected. A check is not a transaction or permission to restart participants;
keep every participant and automatic restart stopped throughout. Recheck the
current complete set, not a historical snapshot.

After all segments pass and the recovery plan is approved, execute per segment:

```sh
memory_point_store_migrate --offline-confirmed --deduplicate-latest \
  --shm segment_a --backup /persistent/unique-segment-a.bak
```

Check and execute use the same validation/planning function. Execute recomputes
the plan from current bytes. For each duplicate index the highest timestamp
wins in its original slot. If multiple slots have that timestamp, value bits,
quality, timestamp, expiry and stale must match. Identical top ties retain the
lowest original slot number. NaN payloads and signed zero use bit comparison;
padding and reserved bytes do not decide equivalence. Lower timestamp samples
need not match. Any top conflict refuses before backup creation or source writes.

All original v8/v9, native size/ABI, pending, mapping, advisory lock, mutex and
owner/claim lease checks remain. Invalid occupancy or a pre-existing latestCount
mismatch still refuses; dedup is not general corruption repair.

Before first source write, a unique persistent full backup is created with
O_EXCL, fsynced along with its directory and compared byte-for-byte. Source
identity, size, mappings and full contents are rechecked after backup. The only
allowed source writes are each loser's one-byte occupied flag to zero, the
four-byte latestCount to the remaining actual occupied count, and finally the
four-byte version to v10. No slot moves; all sample bytes, padding, pending,
history, owners, claims, pointUpdates and mutex bytes are retained. Full final
comparison checks the expected image. No unlink or automatic rollback occurs.

Multiple writes are not crash-atomic. Any IO failure or interruption after the
first source write requires keeping services stopped and inspecting the exact
backup/current segment under the separately approved recovery plan. Re-running
a completed v10 migration refuses. Never restore a pre-runtime snapshot after
runtime has started without a new data-preserving recovery review.

Tests extend the existing migration executable (including sibling CLI checks)
and existing reader-cache regression. Set GATEWAY_MIGRATION_TEST_BACKUP_DIR to
an isolated persistent Linux directory and run as root. The sibling CLI is built
as a CMake dependency; an explicit GATEWAY_MIGRATION_TEST_CLI can select its path.
