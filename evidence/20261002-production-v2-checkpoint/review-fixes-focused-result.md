# V2 local review fixes: focused result (2026-10-02)

Base: `f3f1083efb6100d16b74b0448346c0ab71b15e52`. LOCAL_ONLY source and
private-fixture check. No board, build host, Broker, platform, OTA real runner,
or production path was used. The default-WSL `/opt/modbus-gateway` prior-impact
assessment remains UNKNOWN; it was not cleaned or restored.

## Final execution and raw bytes

The exact selected Linux command was `wsl.exe --user root --exec sh -c
'python3 -m unittest -v tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v2_empty_logic_after_completed_deletion_allows_next_app_recovery tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v2_empty_logic_after_failed_addition_allows_next_app_recovery tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v2_strict_umask_interrupted_config_copy_recovers_owned_temp tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v1_to_v2_ordinary_approval_preserves_legacy_tls_pin tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v2_ordinary_approval_rejects_self_consistent_tls_addition tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v2_ordinary_approval_rejects_self_consistent_tls_rotation tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v2_config_payload_private_root_and_unchanged_tls_mode tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v2_envelope_rejects_adoption_and_v1_candidate_before_intent tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v2_config_recovery_refuses_foreign_temp_before_restoring tools.production_runtime_baseline_test.ProductionRuntimeTest.test_failed_health_recovers_code_gates_pointer_not_mutable_data tools.production_v2_config_test.ProductionV2ConfigTest.test_registered_partial_temp_is_recoverable_but_foreign_or_owner_drift_is_not tools.production_v2_config_test.ProductionV2ConfigTest.test_empty_foreign_directory_is_not_approved_mixed_state tools.production_v2_config_test.ProductionV2ConfigTest.test_known_empty_directory_with_writable_permissions_is_not_approved tools.production_v2_config_test.ProductionV2ConfigTest.test_registered_temp_with_extra_permission_is_not_archived tools.production_v2_config_test.ProductionV2ConfigTest.test_config_write_set_rejects_path_order_presence_mode_and_size_drift > /mnt/d/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/realese1.0/evidence/20261002-production-v2-checkpoint/review-fixes-linux.raw.log 2>&1'`.
Tool chunk `4b58bb`: exit 0; child stdout/stderr log: 15 tests OK, 1.733s.
The WSL launcher warning is only in the outer tool output, not the redirected
Python child log.

The exact mock-only command was `cmd /d /c "python -m unittest -v
tools.ota_manifest_guard_test.OtaFixtureIsolationTest >
evidence\20261002-production-v2-checkpoint\review-fixes-ota-mock.raw.log 2>&1"`.
Tool chunk `6955f8`: exit 0; raw log: 5 tests OK, 0.003s. It did not invoke
`OtaManifestTest.setUp`, unshare, mount, or OTA scripts.

SHA256 of both pre/post nine-file input manifests is identical:
`48af3a2510a8eb24bb3cbf3e179507a1a3fb599ccbab647937d42f0395fb47ab`.
The manifests contain each affected product, fixture, and paired unit input
SHA256 and absolute path. A byte-for-byte line comparison of the two manifests
returned no differences. The raw ZIP has four members, individually checked
against the original files:

| Member | Bytes | SHA256 |
|---|---:|---|
| `review-fixes-linux.raw.log` | 16762 | `bccda75225d7c275d629d42fb7e307bde06bc9335748cb858eee0a29989357b2` |
| `review-fixes-ota-mock.raw.log` | 1031 | `3ed7c325aed7fd93b8962e82fed28ec915bf7efddb67e12e30785d0f5bcc80eb` |
| `review-fixes-input-before.sha256` | 1531 | `48af3a2510a8eb24bb3cbf3e179507a1a3fb599ccbab647937d42f0395fb47ab` |
| `review-fixes-input-after.sha256` | 1531 | `48af3a2510a8eb24bb3cbf3e179507a1a3fb599ccbab647937d42f0395fb47ab` |

ZIP SHA256: `e45b75e00c9fe39b7f8ec0f39e0a6995575f34e3d41d6206bbffbba5fc38742c`.
The ZIP preserves raw log bytes; any Git line-ending normalization of standalone
text files must be assessed separately.

## Scope and failures retained

- M-2 red chunk `1d140a` had 2 errors at the old directory predicate. Final
  run covers a completed last-file deletion and a failed new-directory write,
  each followed by an app-only failed transaction and official recovery.
  Unknown or writable directories still fail.
- L-1 red chunk `861c8c` reached the old exact-mode rejection of a registered
  `0600` temp under umask `077`; best-effort refence also exposed completed
  gate mode drift. Final run checks exact completed gate and backup pins,
  recovery of only the owned mode subset, and rejection of extra permissions.
- M-1 ordinary v2 approval now preserves previous network/TLS byte inventory
  and v2 protected pins. The v1 source uses its old config hashes. Self-consistent
  fake-CA introduction/rotation is refused before intent. OTA trust, identity,
  historical lineage, and other existing preserved fields remain unchanged.
  A legitimate TLS rotation needs a separate reviewed authorization binding
  both old/new trust material, endpoint/identity policy, backup, and recovery;
  that path is not yet implemented or silently admitted here.
- L-3/L-4 selected checks cover protected/traversal paths, duplicate/order,
  presence, per-file/total size and mode bounds, version mix/adoption refusal,
  private `0700` config payload root, and unchanged TLS file mode. Nonempty
  config payloads still contain the complete candidate tree, including unchanged
  sensitive files; minimization and paired packaging remain open.
- L-2 mock checks now require exact private tmpfs `/opt`, `/run`, `/etc` with no
  nested mounts, including a redirected HOME. Actual unshare/mount/OTA runner
  usability and the full script write set remain unverified. Do not run the
  real fixture based on these mocks alone.

Earlier local passes before the final `write_new` mode change are not counted
as final-input evidence. The full business role set, old native proof followed
by a configuration change, ordinary MQTT/direct config journal, ARM build,
board behavior, and three-product deployment remain open.
