# Same-ID freshness stage

Base: e650082a1dd6c5e695ebcb8b889cc9b3f6e56e98.
Red commit: c9278ba (tests and evidence, before product fix).

## Change

Same-ID traffic from another incarnation no longer directly quarantines a node.
Only Hello addressed to the current process incarnation and echoing a live local
discovery challenge confirms a competing instance. Existing challenge expiry is
exclusive: now >= challenge-send + leaderLeaseMs rejects the reply.

Same-ID Discover with a nonzero challenge only produces a UDP discovery Hello,
echoing that challenge and addressed to the sender incarnation. At most one is
queued per discoveryIntervalMs, with a reserved queue slot to avoid a stale-self
burst exhausting the outgoing budget. It cannot admit a member, refresh authority,
reset replay state, or change term/role. Other self frames are ignored.

No wire changes, self TCP endpoint, transport API changes or downstream edits.
Header change adds only the local reply-rate timestamp; status authority API is
unchanged. The existing discovery=true transport route must deliver authenticated
same-ID Hello broadcasts to receive; actual socket integration remains a coordinator
gate. Challenge confirmation detects a responsive competing process, not duplicate
identities hidden by partitions or indefinitely delayed/dropped replies. Rate
limiting can delay confirmation. Existing trusted-PSK and clock constraints remain.

## Actual verification

Runner: evidence/ems-cluster-v2-20260924/run_fix.py, existing private mount/net/ipc/pid
namespaces, tmpfs /dev/shm and private loopback. Added --core-only mode excludes the
pending three-argument transport loopback fixture and separate Graph suite; no stub
factory or product transport edits. All other ems_cluster_test cases run in green.

Commands (from PowerShell, prefix each with `wsl -d Ubuntu -u root -- python3`):

```text
/mnt/d/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/cluster-lease-repair-worker-20260924/evidence/ems-cluster-v2-20260924/run_fix.py red-core-old-self --core-only old-self
/mnt/d/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/cluster-lease-repair-worker-20260924/evidence/ems-cluster-v2-20260924/run_fix.py red-core-live-self --core-only live-self
/mnt/d/workspace/GatewaySuite-workspaces/GW-20260809-002/edge/cluster-lease-repair-worker-20260924/evidence/ems-cluster-v2-20260924/run_fix.py green-core-self-freshness --core-only
```

- Red old-self: compile exit0 / 2.121s; test exit1 / 0.018s, exact error:
  `delayed old self frame must not quarantine the restarted process`.
- Red live-self: compile exit0 / 1.820s; test exit1 / 0.009s, exact error:
  `unconfirmed same-ID discovery must not quarantine either process`.
- Both reds link the cached fixed-voter library, SHA256
  `7e3859a729c78abe36b32a34880748fb0f06933a2728757c539a5fe26c94a186`,
  identical to green-fixed-voters-final-result.json. No product rebuild for red.
- Green: configure exit0 / 2.873s, edge_gateway library exit0 / 5.380s,
  test compile exit0 / 1.721s, core suite exit0 / 0.317s.
- Green additionally checks unknown challenge, old recipient incarnation, exact
  challenge expiry, 1000 repeated old Discover frames, continued discovery and
  no term/authority/member mutation; two live same-ID nodes exchange UDP-marked
  replies through public APIs and both quarantine only after confirmation.
- `git diff --check`: exit0.

Exact commands, source and binary SHA256 values are in the three result JSON files.
Green library SHA256:
`3aeafc02ba712597e5ac214a6d5f00c7e64d052f117cc817d6f7aea5713119bd`.

Main, actual same-ID sockets, new downstream Graph/driver integration and ARM:
NOT_RUN here, owned by coordinator integration. No production acceptance claim.
