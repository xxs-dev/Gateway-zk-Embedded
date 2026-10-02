# Factory stage helper admission closure

Base: `daeb1763f70838dfe493c27999bc9cc4779b828b`. Local-only source fix:

- `deploy/ota-apply.sh` adds only `/opt/modbus-gateway/bin/factory-lifecycle-stage.py` to the exact bin target set.
- `deploy/runtime-upgrade-guard.py` adds only `factory-lifecycle-stage.py` to `PRODUCTION_HELPERS`. The existing adoption-only introduced-file gate, complete inventory, payload pin, regular-file/mode, stopped-participant, copy journal, and official recover checks are unchanged.
- `deploy/offline-runtime-upgrade.py`, old baseline/approval/state records, stop fences and service scope are unchanged.

Focused red before the product change: the new production adoption/recovery tests rejected the exact stage helper as `paired inventory expansion is not approved` (4 tests: 1 error, 3 failures); the isolated OTA test rejected its exact target as `unsafe manifest target`. Negative controls for unknown helpers remained rejected. Raw command output was observed locally; these red invocations were not device executions.

Focused green after the change (WSL Linux root, private fixture paths, no device connection):

| Coverage | Result |
| --- | --- |
| New production adoption, health-failure official recover, unknown helper, changed pin, occupied live target, world-writable and symlink stage | 5/5 PASS, exit 0, 0.358 s |
| Existing adoption to same-ABI install and failed-health recovery | 2/2 PASS, exit 0, in a 7-case invocation lasting 0.642 s |
| Exact OTA bin target, unknown/nested targets, bad SHA, installed content and mode | 1/1 PASS, exit 0, 0.981 s |
| Existing installed-bin paired dependency, missing stage rejection, all three factory profiles | 3/3 PASS, exit 0, 2.927 s |
| `sh -n` OTA entry and Python 3.8 AST parse of guard/test | PASS, exit 0 |

The first OTA fixture extension failed because its deliberately bad-checksum case consumed `GUARD_TEST` before the following positive case reused that jobId. The test now assigns a distinct jobId to the positive case. The failed raw output is retained; this is a fixture sequencing issue, not a claim that a consumed OTA job can be replayed. `focused-raw-evidence.zip` preserves 17 exact local output/isolation files, including this failure and the final pass. Every member was read back and SHA-checked against the original. ZIP SHA256: `ff90589cd2ad24e8f2624e5877bae9c7c6f78f8bb03248a716724a84e1e46ef0` (7617 bytes).

Source SHA256 at final test checkpoint:

| File | SHA256 |
| --- | --- |
| `deploy/ota-apply.sh` | `8b1fb3466bc188fe4a5f7f574ebd1355325b8a6dfe80508ac022bdb4c189b577` |
| `deploy/runtime-upgrade-guard.py` | `63d5b8e0ad56b0a72ab3218da1b7740dccb1c988e50bbfcea321041a7b86dff8` |
| `tools/production_runtime_baseline_test.py` | `f126dfa0d53119a50f956a073ad816dc2d622604413e41a25bb4d0c626c7e1ec` |
| `tools/ota_manifest_guard_test.py` | `cb8fb8c967a26a643b4c45be9876a7af82dc0f4549d24a6ffbadaa6d18d0af14` |

Paired release work still required, not performed here: freeze the final edge source and business ELF set; rebuild the factory archive and its `generic-package.json`/edge provenance; copy the complete paired deploy set (including the unchanged `factory-lifecycle-stage.py` and these two changed entries) into Windows DeviceInit assets; regenerate `component-provenance.json`, factory provenance, archive hashes, file counts and outer seal; then validate Windows `InitializationPayload`'s complete `pairedDeployFiles` enumeration against that same snapshot. Existing Windows assets lack the stage helper. The historical candidate assembly tooling contains fixed deploy counts and must not be reused unmodified for a new final release.

These are private local tests only. No ARM build, device install, business start/restart, signed OTA, or full production scope was exercised. Ordinary manifest OTA remains blocked on devices by the persistent runtime stop fence; this test does not establish a legal lifecycle exit. Full business admission/start/health is a separate open slice, not implied by complete file inventory.
