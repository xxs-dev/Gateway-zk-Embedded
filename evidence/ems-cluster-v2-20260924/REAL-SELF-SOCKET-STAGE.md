# Real same-ID UDP challenge integration

Core input: 862f1b5c9094fe1b0a54026df9a7f09bd6e021c6.
Transport dependencies cherry-picked unchanged, in order:

- 965feff -> local 12a15f2
- 6606d5 -> local a3e2f32
- dbc8e8b -> local b913a72

The test-only stage changes tools/ems_cluster_test.cpp and evidence. No transport,
core, downstream, CMake or main implementation edits in this stage.

## Fixture

Uses two real Ethernet transports on private loopback, distinct UDP/TCP ports,
same node ID, different process incarnations and separate state files. Both receive
the full node.configurationHash() via the third factory argument. Reversed roster
JSON order must produce equal hashes, distinct from the config-only codec hash.

Nodes generate and send their own Discover frames through transport. Both sockets
must receive the competing Discover without quarantining. Only then are the queued
incarnation-addressed Hello replies sent via discovery=true UDP. Replies have no
targetNodeId, so no same-ID TCP endpoint is used. Both nodes must quarantine and
report no effective control after socket-delivered challenge confirmation. No
inbound frame is directly injected into the core in this test.

Receive timestamps are resampled after poll. Discovery stage is bounded to 1s and
the complete exchange to 2s; the fixture's challenge lifetime is 4s. This permits
normal local scheduling variance without accepting expired challenges. Existing
old-self replay and exact challenge expiry simulation tests remain in the suite.

## Actual command and results

```powershell
wsl -d Ubuntu -u root -- python3 /mnt/d/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/cluster-lease-repair-worker-20260924/evidence/ems-cluster-v2-20260924/run_fix.py green-real-self-socket
```

Existing runner, no CORE_ONLY define, native GCC 15.2.0. Private mount/net/ipc/pid
namespaces, tmpfs /dev/shm and private loopback; no external/device traffic.

- Configure exit0 / 4.479s; edge_gateway library exit0 / 2.172s.
- ems_cluster_test compile exit0 / 2.021s; full suite exit0 / 4.327s, run once.
- Log: `same-ID real UDP challenge loopback passed`, then `ems cluster tests passed`.
- Includes existing real two-voter election loopback with third-argument full hashes
  and reversed membership JSON order, plus all previous core regressions.
- Existing runner also ran the unchanged local Graph suite: compile exit0 / 1.519s;
  test exit0 / 0.116s. This is NOT the new downstream SHM11/authority integration.
- git diff --check: exit0.

Exact argv, source hashes, binary hashes and timings:
green-real-self-socket-result.json and matching logs.

Test source SHA256:
`7b0bb6470be56c3393c273a21a83ff64d4262fb0b61c2c00a7399e083143e975`.
Transport source SHA256:
`5a0437372e8e95a74b04fdef323b6a622ab3cdac83648c0f3e102b914c2cfe73`.
Library SHA256:
`7c1aea7e25a99baea8a38e1eb2f01010f007e905fbc11287fac021893dbf41e5`.

This closes the earlier core real-socket NOT_RUN gate only. Main remains NOT_BUILT
pending the downstream four-argument publish implementation; no stub was added.
ARM and final downstream/actual-send/physical acceptance remain coordinator gates.
