# Shadow Live Helper Verification

Source commit: `a8af1ccf364c07fd33f93fe650c64fa635cee1ef`.
Date: 2026-09-24. Test-only, not a product or deployment authorization.
Runtime/header/ABI changes: none. Usage: `tools/ems_shadow_live_helper.md`.

## Results

- Final isolated native run: `green-shadow-live-helper-pipe-result.json`, all
  configure/build/compile/test steps exit 0. Test includes the actual stdin/stdout
  pipe command loop, timer refresh and EOF termination, plus Session/API tests.
- `green-shadow-live-helper-result.json`: earlier Session/API tests passed.
- `trial-shadow-live-helper-result.json`: initial test exited -11. Investigation
  with ASan/UBSan identified the test dereferencing an absent authority Optional.
  Fixed the test to assert absence, without changing runtime or manufacturing an
  authority record. This trial is failure evidence, not a pass.
- Actual CMake target `ems_shadow_live_helper` built successfully against the
  worker library using `cmake --build ... --target ems_shadow_live_helper`.
- Both helper and test pass native `c++ -std=c++14 -fsyntax-only -Iinclude`.
  Compiler: Ubuntu GCC 15.2.0. This exercises the experimental Optional branch;
  it does not replace Linaro 6.3.1/AArch64 verification.
- Final test also compiled with `-fsanitize=address,undefined
  -fno-omit-frame-pointer -g -O0` and exited 0 in the same private mount/network/
  IPC/PID environment, printing `ems_shadow_live_helper_test passed`. The linked
  existing edge_gateway library was not sanitizer-instrumented.
- Static helper inspection found no submitWrite/drain/publishClusterAuthority,
  CreateOrOpen, mmap, ftruncate, shm_unlink, has_value or filesystem usage.

No full 31-test rerun, remote compilation, SSH, device access or deployment was
performed. Coordinator/implementation own pinning the integrated source and
incremental ARM target build against the existing same-source library.

## Proven Narrow Behaviors

Exact-name rejection; OpenExisting never creates a missing segment; illegal
indexes (including diagnostic/authority/sink indexes), duplicate fields/indexes,
nonfinite/domain-invalid values and mixed invalid batches rejected. Duplicate
helper excluded by Linux OFD lock. Paused targets retain original ts/value and
expire while SOC/enable/capacity continue refreshing. Resume applies staged targets.
Sink nextafter(1,2) round-trips exactly through JSON, retaining time/quality/expiry.
Pending command queue remains empty and no authority is created. External input
changes stop a complete refresh before any writes. Lost SHM name is not recreated.

The helper's checks do not provide an atomic fence against an uncooperative writer
or segment replacement; only the reviewed single-writer, frozen-segment lifecycle
is supported. Old helper values must expire before a new helper claims inputs.
No snapshot claims cross-process transactionality or physical execution evidence.

## Pins

Helper LF SHA256:
`fa230e40360d300d8a7e48d7de1547518c55a853d243094752702a88ca1cfe5b`.

Test LF SHA256:
`c97f8b99babeed17972468c8b1dddda2ee1bf604dfce96d4e775e5cf3387cda5`.

Original final result JSON SHA256:
`a69ac19eeec38b6050c6d9877267609254fd5fcc296b35eacec7013454df99ad`.
