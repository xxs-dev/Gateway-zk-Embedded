# Authority publication gap: local native evidence

The first section records the earlier local test session retrospectively. The
later one-time fixed-binary recheck has its own directly captured raw files.
Neither is an ARM, device, or S4 acceptance receipt.

## Provenance and receipt limitation

- Red test commit: `49d9534ede2255d7732bd5417ef180b0cf7c8b71`.
- Green product/test commit: `98e87e9da29a6a948ed92e8e0542098dee57540f`.
- The native command stdout/stderr was returned only in the Codex command
  session during the first test stage. No original red or green output file was
  created then. Consequently there is no original path or SHA256 for that
  first-stage output; its observations below are a secondary transcription.
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

## One-time fixed-binary recheck

This is a new receipt, not a recreation of the missing first-stage logs. On
2026-09-24 UTC (2026-09-25 Asia/Shanghai), the two pre-existing binaries were
SHA256-checked against the pins above and executed exactly once each. No test
was recompiled, and the 50-run and four-suite claims above remain historical
agent-reported results. The binary commands executed inside `wsl --exec sh -lc`
after the hash and no-overwrite checks were:

```sh
/var/tmp/ems-cluster-output-authority-s4-red > "$out/red.stdout" 2> "$out/red.stderr"
/var/tmp/ems-cluster-output-authority-s4-green > "$out/green.stdout" 2> "$out/green.stderr"
```

Here `out` was the absolute WSL path to this document's `recheck-once`
subdirectory. Each invocation used `date -u +%Y-%m-%dT%H:%M:%S.%NZ` directly
before and after, then wrote the actual shell exit code to `<case>.exit`.
Red started `2026-09-24T17:38:20.152825484Z`, ended
`2026-09-24T17:38:20.177108032Z`, and exited 1. Green started
`2026-09-24T17:38:29.945708329Z`, ended
`2026-09-24T17:38:29.985295769Z`, and exited 0. Binary stdout/stderr were
redirected inside WSL; an unrelated outer `wsl.exe` startup warning is not
part of these raw binary streams.

All paths below are relative to `recheck-once/`; empty streams are retained as
zero-byte files. SHA256 values were read after the runs, without replay:

| Raw file | Bytes | SHA256 |
| --- | ---: | --- |
| `red.stdout` | 0 | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `red.stderr` | 70 | `a9356dd7fa6532dbef4f92d4a48e1941d6b1cc1c9f208e439f154a83f4b6176b` |
| `red.exit` | 2 | `4355a46b19d348dc2f57c046f8ef63d4538ebb936000f3c9ee954a27460dd865` |
| `red.started-utc` | 31 | `4a58499a3153162b39534fc30c59f00d1fcdb5e80a6e39ac6bd4cd1467c95e8f` |
| `red.ended-utc` | 31 | `c024f50efbca099a665ecc7d8feeb60ba32f08814795767096c6086efce066e9` |
| `green.stdout` | 41 | `d9693d20c3f68fe597483e7f309769ebc115121f921341ea333fd022f21db848` |
| `green.stderr` | 0 | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `green.exit` | 2 | `9a271f2a916b0b6ee6cecb2426f0b3206ef074578be55d9bc94f6f3fe3ab86aa` |
| `green.started-utc` | 31 | `85307d7f04672cd8c88b26ad46bd4d0dc890f615569ffef3dde8b64b6e23b720` |
| `green.ended-utc` | 31 | `a31720d3f493dc57de95bef2e18f961fb6c509c7453a2eddd159980222a103b8` |

## Compatibility and incremental source scope

`350209d713097e1044f01ffeca509a4261d16f4a..98e87e9da29a6a948ed92e8e0542098dee57540f`
changes only `src/ems_cluster_points.cpp` among production C++ translation
units. `CMakeLists.txt` changes only the native authority test's linker-wrap
options; `tools/ems_cluster_output_authority_test.cpp` is test-only. The other
changes are deploy Python/docs/tests, not ARM runtime objects. By contrast,
R5 `9ccfce8822a2504524c29d9505179ab9e94aa775` to B3 `350209d` changed
both `ems_cluster_main.cpp` (P2) and `src/ems_cluster_points.cpp` (P1). R5 to
`98e87e9` therefore contains both production translation units; only the
already-qualified B3 to `98e87e9` increment is points-only.

Thus the changed product object is `ems_cluster_points.cpp.o` in the
`edge_gateway` static library. `EmsClusterCoordinator` calls the bridge and
must be relinked with that object; merely copying an object to a device is not
a release artifact. No change to `ems_cluster_main.cpp`,
`compute_engine_main.cpp`, `graph_ems_engine.cpp`, or `memory_point_store.cpp`
is required by this delta. No ARM compile, link, upload, or deployment was
performed here. The S4 B sample is compatible with this gap but cannot prove
that this was its unique cause rather than actual lease expiry.
