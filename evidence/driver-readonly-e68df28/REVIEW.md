# Read-only driver context and batch review

Requested changes: c1f524a + 7701d1b. During review the driver worker had already
committed e68df2870e335e35b10c8ae886fabf9b4e2f73fe (daemon context/config wiring).
The independent build pins that commit, which contains both requested changes.
CommandExecutor, WritebackService, interface fallback and Modbus TCP implementation
are unchanged between c1f524a and e68df28. No working RTU edits or later test changes
were read into this build. Only own probe/evidence files were created.

## Findings

No new actionable bypass, ordinary MQTT ownership regression or authorization-
failure batch escape was confirmed within the requested scope.

- src/command_executor.cpp:69 and :171: legacy execute/executeByIndex do not carry
  a cluster token/source. Ordinary MQTT/manual calls remain intentionally permitted
  under their existing ownership path, per coordinator clarification. Their mere
  existence is not reported as a cluster bypass. No additional production caller
  stripping graph/cluster context was found beyond the known daemon/main work.
- src/command_executor.cpp:73: executePending retains the full command and installs
  a guard if metadata is present OR the command is graph-ems in protected scope.
  This includes missing-token graph commands; guard failures occur inside the
  private execute try/catch (lines 98-152), returning one failed CommandResult.
- include/edge_gateway/common/command_executor_interface.hpp:17: the fallback
  rejects supplied authorization/callback instead of silently calling the old
  executeByIndex path. Daemon exception boundaries at src/common/gateway_daemon.cpp:
  486 (durable) and :508 (ordinary) keep rejection local to the command.
- src/writeback_service.cpp:86: independent mixed batches confirm rejection does
  not abort or discard subsequent ordinary commands in the examined cases.
- src/common/gateway_daemon.cpp:424: existing ownership authorization still precedes
  driver dispatch. MQTT generation remains independent from cluster metadata.
- src/modbus_tcp_client.cpp:717: per-send callback is retained across TCP send
  continuations; callback failure disconnects before propagation. Existing worker
  continuation tests were reviewed, not re-counted as new review executions.

The earlier daemon metadata loss is already e68df28's subject and is not repeated
as a new finding. GatewayDaemon/main completion and RTU delivery remain Mendel's
scope; this review neither edits them nor labels their unfinished stages defects.

## Independent probe

The probe includes the pinned worker's existing real TCP Peer/Fixture unchanged,
then adds only review cases. It builds a git-archived immutable source snapshot in
a private native cache, not the worker's dirty checkout or shared build directory.
All runs use private mount/net/ipc/pid namespaces, tmpfs /dev/shm and loopback.

```powershell
wsl -d Ubuntu -u root -- python3 /mnt/d/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/cluster-lease-repair-worker-20260924/evidence/driver-readonly-e68df28/run.py
```

Actual results:

- edge_gateway native build exit0 / 31.540s; probe compile exit0 / 1.219s.
- Probe exit0 / 0.166s, eight cases.
- WritebackService and GatewayDaemon.processWritebackOnce: expired token, missing
  protected graph token, changed snapshot epoch, each followed by ordinary manual
  command. First rejected, second accepted; exactly 12 bytes sent per pair, both
  results retained. No exception escapes these tested batch calls.
- WritebackService: missing authority SHM causes guard OpenExisting failure; that
  command fails, following manual command succeeds; exactly 12 bytes total.
- Ordinary MQTT without cluster metadata: active matching generation succeeds;
  wrong generation, manual command during targeted MQTT ownership, and previously
  valid generation after release all reject. Exactly one 12-byte write overall.

Exact argv, library/binary SHA256 and timings are in result.json; per-case outcomes
are in probe.log. This is not a whole-daemon lifecycle/fault-injection proof or
RTU/main/ARM/physical acceptance. No product file or other worker's test was edited.
