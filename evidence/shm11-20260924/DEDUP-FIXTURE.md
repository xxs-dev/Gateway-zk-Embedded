# ABI11 Dedup Integration Fixture Repair

Date: 2026-09-24. Base: `6ed863fa6b91935edcb181e331e75df6caa81c87`.
Source scope: `tools/control_dedup_integration_test.cpp` only.

## Diagnosis

The old fixture asked the current runtime to create ABI8/9 stores. ABI11
correctly rejects this before the old durable-command assertion is reached.
`red-dedup-legacy-fixture-result.json` records compilation exit 0 and test exit 1.

Create exclusive legacy POSIX SHM fixtures with the frozen v8-v10 layout instead.
For versions 8, 9 and 10, verify ordinary cleanup preserves the segment, both
runtime attachment modes refuse it, and explicit legacy creation remains refused.
After every operation, compare the segment name, device/inode, size and all bytes,
including latest value 77, offline zero and two history samples. Fixture cleanup
only unlinks the exclusively created test segment.

The first repair run (`green-dedup-legacy-fixture`) was NOT fully green: legacy
checks, Modbus, MQTT and CAN passed, but Direct failed with config=GW versus
identity=GW0001. The other four test programs passed. ConfigLoader discovers
`device_identity.json` before applying an explicit identity; the fixture had
written `identity.json`, allowing discovery to fall back to the repository example.
Rename only the fixture file to `device_identity.json` so discovery is local.
No global identity or product validation was changed.

## Final Verification

Final evidence prefix: `green-dedup-isolated-identity`.
All configuration, library/CLI build, compilation and test steps exited 0.

| Test | Result |
| --- | --- |
| control_dedup_integration_test | PASS |
| memory_point_store_shm11_test | PASS |
| memory_point_store_v11_migration_test | PASS |
| memory_point_store_migration_test | PASS |
| control_dedup_protocol_test | PASS |

Integration output confirms simulated Modbus writes=6, MQTT ingress/replay/spoof/
conflict, two loopback CAN UDP sends, and Direct batch/spoof/replay/conflict/ID checks.
These are local fixtures, not physical-device operations.

Tests ran in private mount/network/IPC/PID namespaces using the existing runner.
No SSH, AArch64/Qt build, deployment, push or physical-device access occurred.
Runtime ABI11 rejection and old-data preservation behavior were not weakened.
Coordinator must rerun the integrated candidate's affected tests after applying
this test-only change; these results do not establish integrated or ARM acceptance.

Final result JSON SHA256:
`5dbf66f8951abdaf825c3d8c7b4884025c87d26dc69960483f0ea70ee8365279`.

Test source LF-normalized SHA256:
`07ceecabc36103cd8ae407882b7a339ac1498a7f96fb1423ff9a3f6914b75e9e`.
