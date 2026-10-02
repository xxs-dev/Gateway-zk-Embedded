# R10 direct HTTP local checkpoint

Base: `36561eb4aa6d1eaa70b558cc59cd4ab6d74c2491`.
Scope: local source/build/test only; no device, build host, Broker, or platform connection.

Changed paths:
- `system_monitor_direct_maintenance.cpp`: owned bounded direct diagnostic execution, identity binding, same-request replay, conflict and capacity rejection, classified bounded result, and continued owner polling in the server loop.
- `src/system_monitor_service.cpp`: structured MQTT `truncated` field.
- `tools/system_monitor_direct_maintenance_contract_test.cpp`: direct handler and local loopback socket coverage.
- `tools/system_monitor_service_test.cpp`: MQTT field assertion.

Latest targeted command:

```text
wsl --exec ctest --test-dir /mnt/d/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/direct-local-build-20261002 -R '^system_monitor_direct_maintenance_contract$' --output-on-failure --timeout 35
Test project /mnt/d/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/direct-local-build-20261002
    Start 108: system_monitor_direct_maintenance_contract
1/1 Test #108: system_monitor_direct_maintenance_contract ...   Passed   13.45 sec
100% tests passed, 0 tests failed out of 1
Total Test time (real) = 13.46 sec
exit 0
```

Before the socket case, the affected MQTT service test passed in 4.99 s and the direct contract passed in 10.96 s. The first signal assertion failed: a shell script killed itself, but its supervised shell exited normally with code 143. The corrected assertion verifies `reason=exit`, `exitCode=143`, `signal=0`; it is not a signal-classification positive test. An initial CTest invocation selected the executable name rather than the registered test name and reported "No tests were found"; it is not counted as a pass.

The loopback test verifies serial behavior: a health request waits behind a two-second diagnostic and returns within eight seconds. This is bounded blocking, not parallel health service. The local socket/handler test does not establish ARM or device timing. Direct leader-signal classification and injected retained-owner eventual-reap behavior remain untested in this slice. Windows 30-second client behavior, ARM runtime, and platform ingest/DB/worker/API response ordering remain open. MQTT same-ID conflict reply can still reach a platform first-write inbox ahead of the original terminal result; this HTTP change does not resolve that cross-product contract.

Next validation requires a newly reviewed fixed AArch64 build and device scope: run the direct handler and loopback HTTP path with identity/auth, duplicate/conflict, truncation, timeout, resource and cleanup checks; exercise an owned leader signal and retained-owner failure/reap path without PID guessing. Compare service/namespace/SHM baselines and preserve raw replies. This checkpoint is not a production or three-end acceptance.
