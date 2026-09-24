# SHM11 queue and offline copy stage

This is an intermediate software foundation, NOT a complete actuator fence or
production acceptance. No device/SSH/AArch64/release build or deployment occurred.
Base: c583bb8aad3e98fca9bb1e1815dd5c62560662d5.
Non-destructive open guard: 3b7f204225a962aa12e802123f46bcd1cbe38d75.

## Changes

- Runtime creates/attaches ABI11 only. Nonempty incompatible segments are never
  resized or cleared. Cleanup checks size before mapping and leaves legacy intact.
- PendingWriteCommand carries an optional, explicitly versioned cluster authority:
  local kernel boot ID, authority epoch, immutable monotonic deadline, dispatch
  sequence and restricted authority store name. MQTT generation is independent.
- Native fixed authorization is 120 bytes; pending slot is 264 rather than 144
  bytes. The 4096-slot region grows by 491520 bytes (480 KiB).
- Push/peek/drain and retained queue slots preserve metadata across processes.
  Unsupported/malformed contexts fail closed. Group submission prevalidates all
  contexts before modifying the queue.
- Frozen v8-v10 layout remains available to the legacy offline converter/tests.
  New copy API converts v10 to a different, exclusively created ABI11 segment.
  Source is read-only and byte-preserved, backup is exclusive/persistent/verified,
  pending commands or active participants cause refusal. Latest, history and
  completed results retain their data/order; mutex is new, ownership is cleared.
- CLI adds `--copy-to-v11 TARGET` with `--shm SOURCE --offline-confirmed --backup
  /persistent/unique.bak`. Configuration switching is deliberately not automatic.

## Actual evidence

`run.py` records argv, exits, timings, source hashes and binary/library hashes.
Each test runs in private mount/net/ipc/pid namespaces with private SHM; native
Linux x86_64 only. No product release executables were built. The offline
migration CLI was built solely for its regression suite.

RED stages: baseline-red-preserve, red-legacy, red-metadata, red-v11-copy.
All compiled and exited 1 with the relevant missing behavior.

Final stage `green-v11-stage`: all compile/build steps and all six suites exit 0:

| Suite | Seconds |
| --- | --- |
| memory_point_store_shm11_test | 0.067 |
| memory_point_store_v11_migration_test | 0.167 |
| memory_point_store_migration_test | 1.721 |
| memory_point_store_lifecycle_test | 0.368 |
| memory_point_store_reader_cache_test | 0.216 |
| config_loader_test | 0.116 |

Earlier harness failures are retained, not overwritten: the config test initially
lacked its relative config fixtures; the copy test correctly refused a temporary
backup filesystem; the legacy migration test initially lacked its sibling CLI.
The runner now provides read-only config fixtures, persistent isolated backup
directories and the sibling CLI. These failures are not claimed as safety REDs.

## Remaining work

Atomic current authority snapshot, PointBridge/Graph propagation, configured
target/store binding and checks at physical-send boundaries are NOT implemented
by this stage. An optional context alone cannot make an old driver safe. All SHM
participants must be rebuilt and stopped for migration; never mix ABI10 and 11.
Native success does not qualify Windows or AArch64 ABI and does not demonstrate
physical PCS timeout behavior. Legacy fixtures are converted before testing v11
runtime behavior; old-format migration assertions have not been erased.
