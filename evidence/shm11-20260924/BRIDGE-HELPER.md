# Bridge and guard stage

Candidate only. No ARM build, device connection, deployment or release claim.

The four-argument point bridge preserves the minimum of the core local monotonic
authority deadline and dispatch deadline. Refreshing an existing dispatch sequence
cannot extend its deadline; revocation retains a tombstone. A publisher restart
clears authority and uses a new random epoch. The three-argument API is diagnostic
only and clears authority.

The independent station-strategy gate uses the leader's lease and local kernel
boot ID, without requiring an existing dispatch or target. It never supplies a
physical-write token. The shared snapshot is now 1240 bytes (native static_assert);
the queue token remains 120 bytes and pending slot remains 264 bytes. This is an
unreleased ABI11 candidate revision; previous candidate mappings of a different
size must be rejected, never resized or reused in place.

Evidence:
- red-strategy-startup: runtime assertion failure with the original four-argument gate.
- green-strategy-startup: bridge, pure authorization boundary and SHM11 tests exit 0.
- red-graph-strategy-startup: expanded bridge test passes; separate Graph startup
  test still fails because legacy Graph depends on dispatch diagnostic points.
- green-bridge-tombstone: bridge tombstone and pure authorization tests exit 0.

Remaining production blockers: Graph atomic consumption/lineage, router ingress
and configuration binding, actual-send guards and full fake-actuator regressions.
The guard API is provided for the send-side worker; it is not yet wired to drivers.
All stage logs/results include source hashes; use LF-normalized hashes across
Windows Git checkouts. Core worker files were not edited in this stage.
