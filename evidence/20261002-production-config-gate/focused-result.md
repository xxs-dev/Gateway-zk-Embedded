# Production config prewrite gate: local checkpoint

Scope: transitional v1 safety guard, not a v2 provision transaction or a
permanent config-publishing solution. A journal-owned runtime root blocks
ordinary MQTT and direct HTTP apply/delete/restore before any live write.
Read-only snapshot and direct dry-run remain available. The existing identity,
authentication and config content rules remain in place.

The local WSL build compiled `system_monitor_service_test` and
`system_monitor_direct_maintenance_contract_test` from this worktree into
`/home/wmzdxs/gateway-production-config-gate-20261002`. Both final binary
runs returned exit 0. The MQTT fixture first shows an ordinary valid graph
apply, then refuses all three mutating routes when its private production root
exists; refusal retains `dryRun:false` and leaves graph bytes unchanged. The
direct fixture uses two separately created private runtime roots with valid
app, identity and device configs. Ordinary apply/delete/restore succeed and
restore the latest backed-up bytes. The production counterpart contains a
valid, different backup candidate; all three mutating routes refuse with the
device, backup, identity, app, and directory inventory unchanged. Its dry-run
apply returns HTTP 200 with `success:true` and no write; snapshot remains
readable. This is private Linux fixture evidence, not systemd/device behavior.

Final raw host outputs (working-file SHA-256):

| File | SHA-256 | Result |
| --- | --- | --- |
| `mqtt-service.raw.log` | `ecfda9b370f43da2997b4c27d98165ec52ce6691def91d915cdc15e9730abe49` | exit 0, `system_monitor_service_test passed` |
| `direct-contract.raw.log` | `ab9bae6a85168c8c35b6f837530e5e814539c2a1839cf51abab024af84f05d40` | exit 0, `system_monitor_direct_maintenance_contract_test passed` |

Both original members are in `raw-local-outputs.zip`, SHA-256
`99e99acb3d173c5a523db8836be7927f28d4ff44b3730aa3fcc1b842a1be1246`.
The WSL launcher emitted a UTF-16-ish localhost warning into the redirected
host output; it is retained, not presented as test output.

Earlier focused failures are retained as tool records, not represented by the
final ZIP: the initial fixed-root service fixture failed before the new
assertions (exit 1; its precise root cause was not isolated). The next private
fixture run was blocked by an inaccessible host `/opt/modbus-gateway` (exit 1).
The test now uses an
exclusive `mkdtemp` root and the guard checks the runtime identity's home plus
the canonical home when the executable actually runs from `/opt`. A first
direct negative fixture failed before the gate because its app was not
loadable; it was replaced with the valid two-root fixture. An ordinary direct
delete test initially expected one backup, but apply and delete correctly
created two; that fixture expectation was corrected. No historical or
protected directory was removed.

## Open restore defect

The following added ordinary MQTT trigger was run then removed from the active
suite because it exposed an existing delete/restore defect rather than the
prewrite gate. Original tool result `call_XUMVDMbwsknjsoTzHmYXcd0C` at
2026-10-02T10:00:06Z returned exit 1 after the delete. The failing assertion
read the now-missing graph path; source inspection shows
`collectConfigSnapshotFiles` recursively lists only existing logic files, so
`buildConfigFileOperationReply` rejects the deleted path before it consults
`latestBackupPathFor`.

```cpp
MqttIncomingMessage operation;
operation.type = MqttIncomingType::ConfigDeleteRequest;
operation.payload = std::string("{\"requestId\":\"LOGIC_DELETE\",\"machineCode\":\"GW_TEST\",\"dryRun\":false,\"path\":\"") +
    runtimeGraph + "\"}";
logicPublisher->incoming.push_back(operation);
logicService.runOnce(1770000000601LL);
operation.type = MqttIncomingType::ConfigRestoreRequest;
logicPublisher->incoming.push_back(operation);
logicService.runOnce(1770000000602LL);
require(readFile(runtimeGraph).find("updated-graph") != std::string::npos,
        "ordinary MQTT logic restore did not recover the backed-up graph");
```

This remains **OPEN**. The formal provision journal must recover removed files
from its durable approved write set, without scanning arbitrary backups or
relaxing path admission. Ordinary logic-file restore also needs an explicit,
bounded path authority. No Windows/platform config client, actual ARM binary,
device, Broker, reboot or signed OTA was exercised here. V1 historical
validation and protection remain unchanged; full configured-role lifecycle,
capacity, boot, health and v2 config journal are not implemented by this gate.
