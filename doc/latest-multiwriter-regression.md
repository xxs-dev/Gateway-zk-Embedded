# Latest Multiwriter Recurrence Regression

## Evidence (2026-09-16)

Native WSL Ubuntu x86_64, GCC 15.2.0; current production source:
`5b6b19ae81c6c99bcfe3ecd2c430cfdeff2af301`. Old baseline:
`185db90406bf4117625f314e1007ba64d86b1aec` (`919089a^`). The store implementation
diff between these commits is exactly the `919089a` shared-slot lookup fix;
the store public header and layout header are unchanged.

The same `tools/memory_point_store_multiwriter_test.cpp` is compiled against
both implementations. Each v8/v9/v10 fixture uses three waves of four real
forked writers with independent caches, constructed before a start barrier.
Each writer updates 13 common new indexes 32 times and adds one private index.
All writers exit before physical-slot inspection and store reconstruction.
Assertions cover unique occupied indexes, latestCount, expected identities,
SHM inode/size/version, retained seed value, and restarted writes/readback.
Successive waves must contain exactly 18, 35 and 52 unique occupied slots.

| Run | Result |
| --- | --- |
| Old baseline, plus five repeat runs | Exit 1 every run; all three versions fail in wave 1: `duplicate physical latest index: occupied=57 unique=18` |
| Current CMake test, 20 repetitions | Exit 0, 20/20 pass; all nine version/wave combinations pass per run |
| Current lifecycle / multiwriter / reader-cache CTest | Exit 0, 3/3 pass; reader-cache includes all 20 checks |
| Current ASan + UBSan build/run | Exit 0; all waves and cleanup checks pass, no sanitizer report |
| Test-only `-Wall -Wextra -Wpedantic -Werror -fsyntax-only` | Exit 0 |

The draft's unchecked seed optional is now guarded. Scoped pipes/maps and PID
ownership replace raw cleanup; bounded poll/wait replaces an abrupt alarm.
Injected parent exception, child-exit timeout and early-child barrier EOF each
verify that every spawned PID has already been reaped (`ECHILD`). Reaped PIDs
are removed from cleanup ownership. No `gateway_multiwriter_*` SHM remained.
This does not promise cleanup after external SIGKILL or host termination.

## Reproduce

Run from this worktree. Use Windows Git for the archive because this Windows
worktree's `.git` pointer is not directly usable by Linux Git. No checkout,
other worktree, device, deployment or production source modification is needed.

```powershell
wsl -d Ubuntu -- cmake -S . -B build/latest-recurrence -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
wsl -d Ubuntu -- cmake --build build/latest-recurrence --target memory_point_store_multiwriter_test memory_point_store_reader_cache_test memory_point_store_lifecycle_test -j 4
wsl -d Ubuntu -- ctest --test-dir build/latest-recurrence -R ^memory_point_store_ -E migration --output-on-failure
wsl -d Ubuntu -- ctest --test-dir build/latest-recurrence -R ^memory_point_store_multiwriter$ --repeat until-fail:20 --output-on-failure
wsl -d Ubuntu -- mkdir -p build/latest-recurrence/baseline
git archive --format=tar --output=build/latest-recurrence/baseline.tar 185db90406bf4117625f314e1007ba64d86b1aec src include
wsl -d Ubuntu -- tar -xf build/latest-recurrence/baseline.tar -C build/latest-recurrence/baseline
wsl -d Ubuntu -- g++ -std=c++17 -O0 -g -Ibuild/latest-recurrence/baseline/include tools/memory_point_store_multiwriter_test.cpp build/latest-recurrence/baseline/src/memory_point_store.cpp -pthread -lrt -o build/latest-recurrence/baseline-multiwriter-test
wsl -d Ubuntu -- build/latest-recurrence/baseline-multiwriter-test
```

The last command must return 1 with all three duplicate-slot failures, not a
build, timeout or child failure. Local untracked evidence is under
`build/latest-recurrence/`: `current-multiwriter.log`, `baseline-multiwriter.log`,
`repeat-multiwriter.log`, `targeted-regressions.log`, `sanitized-multiwriter.log`.

The additional migration target compiled, but running it as root with backups
inside this worktree failed its persistent-filesystem guard: WSL's Windows
mount is not ext4/ext-family, XFS or Btrfs. `store-regressions.log` records that
failure (3/4 tests passed). Migration is NOT claimed passed in this run; no
guard was weakened and no backup was redirected outside the allowed worktree.
No AArch64 or device validation was performed.

## Cause And Recovery Boundary

Old `allocateLatestSlot(cache)` treated a process-local cache miss as shared
absence. Even serialized mutex holders could allocate duplicate slots for the
same index. `919089a` searches shared latest on cache miss/invalid cache entry
and backfills the cache. This regression confirms that mechanism, not who
created the 13 duplicate groups in the historical 103 snapshot: that snapshot
does not identify the original creating PIDs.

For the next data-preserving upgrade, stop every participant and automatic
restart, inventory current SHM and persistent outbox/history, and establish
verified, consistent backups on approved persistent storage. Validate the exact
target binary's ABI, layout size, version acceptance, constructor behavior and
startup scripts before any attach. v10 compatibility is NOT permission to roll
back to an arbitrary old binary: old constructor size/version checks or startup
scripts may resize, unlink or reinitialize data. Do not assume binary rollback
is data rollback.

The original 103 v9/outbox snapshots were deleted at the user's request and are
not an available recovery source. Never hand-edit the header or blindly restore
old bytes. Follow [offline dedup preflight](shm-offline-dedup-stage12.md) for all
stopped segments and a separately approved recovery plan; retain current data
and inspect after any partial migration. Once runtime has resumed, restoring a
pre-runtime snapshot requires a new reconciliation/recovery review. This work
does not authorize migration, downgrade, restore or deployment on any device.
