# V1-to-v2 first parser introduction (2026-10-02)

Supplement to `ded3f190d3edf1cfc2f410675a2f9613d881fc2c`. This selects only
the two new private-root tests; the earlier three migration/newboot tests were
not rerun.

Command: `wsl.exe --user root --exec python3 -m unittest -v tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v1_to_v2_introduces_parser_missing_from_legacy_baseline tools.production_runtime_baseline_test.ProductionRuntimeTest.test_v1_to_v2_failed_start_removes_new_parser_on_recovery`

Tool output chunk `317d31`: exit 0, 2 tests OK, 0.305s. The original tool output
is retained in this task; this file is an index, not a raw-log copy.

The private legacy adoption input omits `bin/production_role_policy.py` from
both its live file inventory and its payload. The v2 candidate then supplies
the exact paired parser. Success proves the new live file matches the candidate
pin. The injected startup failure proves official recovery deletes that file,
restores the previous complete file inventory, and retains the old v1 pointer.

As in the prior fixture, trusted unit templates and a digest-named simulator
profile are preinstalled in the private root. This does not prove their formal
delivery, an actual board reboot, or arbitrary configured business roles. A
different sequence, where a previous native newboot proof already exists and
configuration changes afterward, remains untested and is not inferred from
the previously passing candidate-newboot test. No OTA fixture or real `/opt`
operation was run; the previously reported WSL impact remains unknown.
