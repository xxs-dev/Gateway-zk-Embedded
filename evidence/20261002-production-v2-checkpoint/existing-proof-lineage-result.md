# Existing new-boot proof / later v2 config transition (local only)

Base: `7f56c063d24852e70eeacc1285c54fa2206fe030`. This is a private Linux
fixture with substituted systemd, proc, SHM paths and native creator. It is not
a board, ARM binary, OTA runner, or deployment result.

The first two new real-entry tests failed before the change at
`production_verify(candidate)` with `ValueError: invalid native boot authority`
(tool chunk `14b312`, exit 1). Both created the ready proof before the approved
config/role change. The failure case did not reach injected endpoint health.

The ready proof is now checked against its original pinned source and actual
boot-local segment identities. A v2 candidate may inherit it only through at
most 128 exact approved previous/candidate transitions. A provisional edge
requires the matching pending transaction; a committed edge requires its
immutable candidate baseline, bound health, state and durable lineage receipt.
The original proof is never rewritten. Failed installation recovers the old
config, pointer and proof through the official `recover` entry.

Final command, executed once against the hashes below (tool chunk `644b7f`,
exit 0):

```text
cmd /d /c "wsl --cd /mnt/d/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/realese1.0 -u root --exec python3 -m unittest -v tools.production_native_boot_test.NativeBootTest.test_existing_boot_proof_config_install_promotes tools.production_native_boot_test.NativeBootTest.test_existing_boot_proof_config_failed_health_recovers tools.production_native_boot_test.NativeBootTest.test_v1_to_v2_migration_newboot_binds_candidate_config_roles tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v1_to_v2_formal_install_promotes_configured_authority > evidence\20261002-production-v2-checkpoint\existing-proof-final.raw.log 2>&1"
```

Raw final stdout/stderr: `existing-proof-final.raw.log`, SHA256
`2121d60d291dd04ded4d2daf61ad8c178417341156ac29bd2f3b81318b4f7c23`.
It includes the WSL startup warning bytes; the unittest result is 4 tests,
4 pass, 0 skip, 0 failure, 1.045 s. An earlier 4-pass run against an earlier
guard draft is separately retained as `existing-proof-focused.raw.log` and
is not the final input claim.

Final pre/post SHA256 were identical:

| Input | SHA256 |
| --- | --- |
| `deploy/runtime-upgrade-guard.py` | `51c745312478f87c30e30860db28c55b3cd3a8699cdde438c37fa0299cb60fd9` |
| `deploy/offline-runtime-upgrade.py` | `be86476247f10aa656ae01b7a886cf2f525c8e361b4540c1ab0a4519596438aa` |
| `tools/production_native_boot_test.py` | `b640416492e5ff4b55fb388496eb4257b1c1662886d5ee9a715b64824d47dd51` |
| `tools/production_runtime_baseline_test.py` | `7de63f6880858dc9f5b96eaf55426693c8f2d4315d9b306f50028599a94bcadd` |
| `evidence/20261002-production-v2-checkpoint/review-fixes-focused-result.md` | `6386d2cabbaa3be3e1767aaae96f25deeb7c38235c4037e7f267ee11a3ba5d074` |

Still open: full multi-transition lineage and crash-window matrix, real native
creator/board/ARM, complete configured-role scope, normal MQTT/direct/SCADA
provision and ordinary logic delete/restore. No existing WSL `/opt` tree was
used by these private fixtures; no OTA isolation runner was invoked.
