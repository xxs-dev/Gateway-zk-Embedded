# Edge R10 diagnostic closure (local)

Base: `619ad6cd4ac57547f4a3b8b5c9d7a0b98ce4c2f8`.
No device/build-host/Broker connection and no production installation.

The original direct contract target remains linked to the production library without test hooks. A separate owner fault target compiles `DiagnosticProcess` and `SystemMonitorService` with test hooks; its link map includes neither corresponding production-library object. The leader-signal case appends a self-signal to the owned command shell, not a guessed parent PID. The early `cleanup_unknown` case observes a live poisoned owner, 503 refusal, server-loop reap, and continued refusal.

MQTT same-ID conflicting attempts publish `replyKind=admission_rejection` with `attemptedCmdId` on the diagnostic topic, but no terminal `cmdId`. Admitted results publish `replyKind=terminal` with the original `cmdId`. Tests cover conflict-first and terminal-first plus identical retry; the previous misrouting red light was caused by `publishReply` choosing a topic from `cmdId` presence and was fixed with an explicit diagnostic-topic publish.

Raw targeted CTest summaries:

```text
system_monitor_diagnostic ........ Passed 1.21 sec (1/1, exit 0)
system_monitor_direct_maintenance_contract ... Passed 13.45 sec
system_monitor_direct_maintenance_owner ...... Passed 1.30 sec
100% tests passed, 0 tests failed out of 2
Total Test time (real) = 14.78 sec (exit 0)
```

The isolated owner case is local Linux fixture evidence only. It does not establish ARM kernel capability, the installed direct listener, a real Broker, or Windows/platform consumption. The earlier no-`cmdId` MQTT red light and link-map mixed-layout draft were not accepted as passing evidence. The production-equivalent contract target remains separate.

Follow-up correlation fields: both admitted terminal and non-admitted conflict rejection include normalized `command`, `arg`, and `operator`. `attemptedCmdId` alone must never complete the original request. After this addition the production-linked direct contract, isolated owner test, and actual MQTT service diagnostic test passed together (3/3, 15.99 s, exit 0).

The MQTT diagnostic stdout serializer now escapes every C0 control byte without changing other service JSON paths. A real service reply containing SOH, backspace, form feed, and ESC round-tripped through the repository's strict JSON parser; `system_monitor_diagnostic` passed in 1.24 s (exit 0). This is local serializer evidence, not a Broker or device run.
