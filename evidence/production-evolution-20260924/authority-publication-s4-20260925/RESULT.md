# Authority publication gap: local native evidence

This is a retrospective record of the already completed local test session. No
test was rerun to produce this file. It is not an ARM, device, or S4 acceptance
receipt.

## Provenance and receipt limitation

- Red test commit: `49d9534ede2255d7732bd5417ef180b0cf7c8b71`.
- Green product/test commit: `98e87e9da29a6a948ed92e8e0542098dee57540f`.
- The native command stdout/stderr was returned only in the Codex command
  session. No original red or green output file was created. Consequently there
  is no original receipt path or SHA256; the observations below are a secondary
  transcription, not a reconstructed original log.
- The original S4 A/B JSONL files were not edited. Their SHA256 values are
  `d2ebd494fcbf57d9c3ec892e64a61d05e60b123bd3ecee00dd48abc6c11d4d82`
  and `1d0b7e2ba6db8eb4b6b62e377c5d2bdb6b66beae8dcd9fc83395f4414af3ff72`.

## Observed local results

- Baseline library plus the GNU linker-wrap barrier test: exit 1, message
  `transient invalid authority during legal refresh would yield reason16`.
  The test reads the real shared-memory authority immediately after each real
  `publishClusterAuthority` call. The baseline library SHA256 was
  `d9c02f29e4fa2c830db033ccefc038ca12315c260318e9b9a213f77115226ad2`.
- Recompiled `src/ems_cluster_points.cpp` linked ahead of that same library:
  `ems_cluster_output_authority_test passed`, exit 0. The final fixture covers
  same-term refresh, new dispatch sequence and old-token rejection, quorum
  loss, term change, unchanged deadline on repeat, and diagnostic-write
  exception revocation. The green binary passed 50 consecutive local runs.
- `ems_cluster_strategy_startup_test`, `graph_ems_cluster_dispatch_test`,
  `cluster_router_authority_test`, and `cluster_write_authorization_test`
  each passed when linked with the replacement bridge object. C++14
  `-fsyntax-only` passed for the bridge and authority test sources.
- WSL `unshare --mount --net --ipc --pid` returned `Operation not permitted`.
  The test instead ran in local WSL with PID-unique SHM names and `shm_unlink`
  cleanup. It did not access a device or ARM builder.

The following are surviving native artifacts, not stdout/stderr receipts;
their `/var/tmp` paths are ephemeral:

| Artifact | SHA256 |
| --- | --- |
| `/var/tmp/ems-cluster-output-authority-s4-red` | `10846910ec91496d7f9090a102c9863e2f3ea515bab4c4493d1ed6519b8b732b` |
| `/var/tmp/ems-cluster-output-authority-s4-green` | `dd1b3f64ecdfb4da69587e55d52e79810b33a46eef64b7f683b378e9aa758fdb` |
| `/var/tmp/ems-cluster-points-s4-green.o` | `e615b068f36b1e53915b35ff28530cca1ef4c29eaf4e958f6d55b9adcfe5e7ee` |

## Compatibility and incremental source scope

`350209d713097e1044f01ffeca509a4261d16f4a..98e87e9da29a6a948ed92e8e0542098dee57540f`
changes only `src/ems_cluster_points.cpp` among production C++ translation
units. `CMakeLists.txt` changes only the native authority test's linker-wrap
options; `tools/ems_cluster_output_authority_test.cpp` is test-only. The other
changes are deploy Python/docs/tests, not ARM runtime objects. The same
production C++ delta holds relative to Compute R5 `9ccfce8822a2504524c29d9505179ab9e94aa775`.

Thus the changed product object is `ems_cluster_points.cpp.o` in the
`edge_gateway` static library. `EmsClusterCoordinator` calls the bridge and
must be relinked with that object; merely copying an object to a device is not
a release artifact. No change to `ems_cluster_main.cpp`,
`compute_engine_main.cpp`, `graph_ems_engine.cpp`, or `memory_point_store.cpp`
is required by this delta. No ARM compile, link, upload, or deployment was
performed here. The S4 B sample is compatible with this gap but cannot prove
that this was its unique cause rather than actual lease expiry.
