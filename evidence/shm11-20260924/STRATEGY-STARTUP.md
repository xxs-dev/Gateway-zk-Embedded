# Independent station-strategy startup

The four-argument bridge and configured Graph now distinguish a leader's strategy
lease from dispatch write authority. The router exposes the configured authority
store through the existing guard's cached OpenExisting handle. Adding cluster point
routes binds the cluster configuration in Compute and MQTT entry points without
editing worker-owned main files. A missing authority store fails closed.

Configured Graph clusterDispatch consumes one atomic snapshot, then samples the
local monotonic clock and boot ID. Its station gate is independent of dispatch
validity, deadline or diagnostic scalar freshness. Its phase targets come from
the same atomic record. Legacy unconfigured scalar Graph remains diagnostic;
physical-output lineage and router ingress fencing are a subsequent stage.

Regression ems_cluster_strategy_startup_test uses two real EmsClusterNode instances:
election with no target/dispatch, four-argument bridge publication, actual Graph
clusterDispatch plus six switch calculations, bridge sampling, core dispatch,
and Graph readback. It also verifies foreign boot, expired strategy with fresh
wall-clock diagnostics, and publisher restart. No physical commands are emitted.

- red-graph-strategy-startup: runtime startup assertion failed before Graph fix.
- green-graph-strategy-startup: startup, bridge and legacy Graph suites exit 0.
- green-graph-strategy-boundaries: startup including boot/expiry/restart, bridge,
  legacy Graph and authorization boundary suites exit 0.

Local native isolated tests only. No core-worker file edits, ARM build, device
access, deployment or production acceptance claim. Snapshot remains 1240 bytes.
