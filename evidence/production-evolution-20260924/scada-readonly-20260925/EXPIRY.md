# Native read-only monitor expiry follow-up

2026-09-25, base 2656d14a2d3dc9e7fcbe97979ced19af827f6021.
Only tools/scada_runtime_test.cpp changed; no product C++ or deploy script change.
The active realese1.0 worktree remains 488c3f78370f0187b93041baea7001812252d920.

Source semantics: src/memory_point_store.cpp:2094 isExpired uses
`expireAt > 0 && nowMs > expireAt`; markStale sets only the stale flag.
The router and ScadaRuntimeMap::readScreen retain the returned sample rather
than dropping expired values or changing quality.

The affected --readonly-monitor fixture now asserts:
- now=1000: index 920000005, value 42, quality 1, stale=false.
- now=31000 (exact expiry): one sample, stale=false.
- now=31001: one sample, stale=true, value 42, quality 1, ts=1000, expireAt=31000.

Executed once using the existing isolated Python test entry:
test_offline_scada_readonly_native_monitor_value. Result: 1 PASS, runner exit 0;
native fixture exit 0. No other test or suite was executed for this follow-up.
The native test was rebuilt against the same pinned cached native library;
no ARM build, device, Windows package or push operation.

Original receipts directory:
`C:/Users/12193/AppData/Local/GatewaySuiteImplementation/GW-20260809-002/scada-readonly-expiry-20260925/`.
native-build.json and invocation.json preserve argv, UTC times and exits;
native-build.stdout/stderr and stdout.txt/stderr.txt preserve original output.

- result.txt SHA256:
  `54f73a22be337a5c815bdc1e8d66a235c3cb70657705ab5e94ce46b582ffe506`
- test_offline_scada_readonly_native_monitor_value.log SHA256:
  `dcc64b2a10c506645b9ce3de350fed90ce5ab197e38698fc60282cf42767835b`
- Native fixture SHA256:
  `9fa08b7b397ba0bb723f308dabe155719e879be1efc708ffacc1acfd4552aa92`

Actual additional native stdout:
`expiry now=31000 stale=0; now=31001 stale=1 value=42 quality=1 ts=1000 expireAt=31000`.
expiry-receipts.json pins every retained file. Prior evidence is unchanged.
This verifies native API expiry propagation, not a rendered Qt stale indicator
or device acceptance. ELF artifacts remain outside Git.
