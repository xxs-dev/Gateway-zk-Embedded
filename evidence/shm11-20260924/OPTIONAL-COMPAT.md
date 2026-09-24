# SHM11 Probe Optional Compatibility

Date: 2026-09-24. Worker base: `7ce0b0d2731924e8a4619dd7d4265fbd29e1d181`.
Integrated ARM source: `c8a31d64f9eb7da4a4c2ba452fe07bef3c5ee830`.
The affected test was identical in both revisions before this edit.

## One-Line Repair

`tools/memory_point_store_shm11_test.cpp:60`: replace
`received.clusterAuthorization.has_value()` with
`static_cast<bool>(received.clusterAuthorization)`.

`compat.hpp` selects `std::experimental::optional` for C++14; that API lacks
`has_value()`. Explicit bool preserves the presence assertion before dereference
and also works with C++17 `std::optional`. No runtime, headers, ABI, CMake or other
source files changed.

## Verification

The following local command reproduced the exact API failure before the edit
(exit 1), then succeeded after the edit (exit 0, no compiler diagnostics):

```sh
c++ -std=c++14 -fsyntax-only -Iinclude tools/memory_point_store_shm11_test.cpp
```

Executed through WSL Ubuntu with absolute worker paths; local compiler was
Ubuntu GCC 15.2.0. This exercises the experimental-optional branch but is NOT a
Linaro 6.3.1 or AArch64 build. The original ARM failure remains in the
implementation evidence worktree's `ems-shadow-20260924/arm-build` transaction.

The exact 12 probes from its `build-input.json` were searched in the integrated
candidate (all present and unchanged from c8a31d6):

- ems_cluster_test
- ems_cluster_transport_bounds_test
- cluster_driver_send_guard_test
- cluster_driver_rejection_test
- cluster_write_authorization_test
- ems_cluster_output_authority_test
- ems_cluster_strategy_startup_test
- cluster_router_authority_test
- cluster_config_authority_test
- memory_point_store_shm11_test
- memory_point_store_v11_migration_test
- control_dedup_integration_test

Only the repaired line used `has_value()`. No other test changes were needed in
this scope. Unrelated tests outside these 12 were not modified.

Existing isolated native runner evidence: `green-optional-bool-compat-result.json`.
SHM11, v11 migration and dedup integration compiled and passed (3/3, all exit 0).
These runtime regressions use the worker's C++17 native library, not the ARM
integrated candidate. `git diff --check` passed.

Updated test LF-normalized SHA256:
`370670478d9e7092cbccc85f0b238713fe96ad83e4c68689a4effff881f912f2`.

No remote connection, ARM rebuild, deployment or device access was performed.
Coordinator must pin the integrated test-only delta and authorize implementation's
incremental ARM continuation; prior failure logs and the owned build root remain.
