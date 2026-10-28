# Stage 2: Overlapping Control Leaders Reproduced

Verdict: FAIL for the property "at most one effective control leader".
Source: unchanged 7e392cec6218244b0a230a5f0d58c96888fd51de.
Stage 1 suite PASS remains valid for its narrower assertions.

## Method

The authorized test-only lease_probe.cpp includes the existing cluster test
Simulation. A test-only private/public visibility macro exposes ACK timestamps;
no production implementation is edited. Native GCC links the fixed-source
edge_gateway library. Build commands, compiler, library/binary hashes and true
process exits are in probe-result.json. No executable or library is committed.

The initial /tmp build had disappeared on a later WSL invocation. The same
dependency target was built in a persistent /home cache for this probe; sealed
release binaries were not rebuilt or modified.

Three nodes start together, settle for 5000 simulated ms, then node 1 is isolated
from nodes 2/3. Each node continues receiving fresh simulated capabilities and
the same 18 kW per-phase station target. Parameters are the existing Simulation
defaults: heartbeat 100 ms, lease 400 ms, election 500..800 ms, member timeout
1200 ms, dispatch cycle 100 ms, dispatch/capability TTL 400 ms.

Two identical partition runs and one connected control were observed every
25 simulated ms for 2500 ms. No physical output or device was involved.

## Observed Timeline

Partition begins at simulation time 6000 ms. Both repetitions are identical.

| Relative ms | Old leader N1 | Majority-side N2 | Result |
| ---: | --- | --- | --- |
| 0 | term 1, last peer ACK 6000, dispatch valid | follower | one leader |
| 400 | last peer ACK still 6000, lastQuorumMs becomes 6400 | follower | old lease extended without a new ACK |
| 750 | leader, quorum/control/dispatch valid | leader term 2, quorum valid, no valid dispatch yet | two leader roles |
| 775 | term 1, quorum/control/dispatch valid, Pa=18 | term 2, quorum/control/dispatch valid, Pa=18 | two effective control leaders |
| 800 | still leader, issues dispatch sequence 53 | still effective control leader | overlap continues |
| 825 | discovering, quorum/control/dispatch false, Pa=0 | effective leader | overlap ends |

Role/quorum-leader overlap: samples 750, 775, 800 ms; first-to-last span 50 ms,
or 75 ms through the next simulation tick. Effective control/dispatch-leader
overlap: samples 775 and 800 ms; first-to-last span 25 ms, or 50 ms through the
next tick. These are discrete simulation intervals, NOT measured physical
controller durations. The connected control has zero overlap samples.

Probe execution exit 0 means collection completed. It does not mean the safety
property passed. The JSON records roles, terms, quorumValid, controlActive,
activeDispatch validity/term/sequence/power, lastQuorumMs, lastLeaderSeenMs,
and all peer lastAckMs for every sample (303 rows).

## Source Cause

- src/ems_cluster.cpp:1144-1151 counts ACK timestamps younger than one lease,
  then advances lastQuorumMs to the current tick time, not the ACK quorum time.
- src/ems_cluster.cpp:1152 then allows another lease from lastQuorumMs before
  stepping down. The same retained ACK evidence can therefore extend control
  toward two lease periods after the last actual majority ACK.
- Validation at src/ems_cluster.cpp:942 only requires election minimum greater
  than one lease, not this effective two-lease interval.
- src/ems_cluster_dispatch.cpp:tickDispatch gates on lastQuorumMs and permits
  the isolated old leader to keep accepting its local allocation in that window.
- src/ems_cluster.cpp:1631-1650 exposes quorum and active control from this state.
  The new majority leader independently obtains term 2 and valid local dispatch.

Terms and sequence checks reject old messages after the receiver learns the new
term; they do not instantly fence an isolated old leader's own local outputs.
This is a demonstrated software fencing gap, not proof of a physical incident.

## Disposition

Coordinator and implementation were notified immediately. Physical control
remains prohibited. Do not market Stage 1 PASS as no-split-brain assurance.
No fix, timing workaround, new release, or device experiment is authorized by
this evidence. Any later fix needs a fresh regression for this exact overlap,
then separate timing/fault-injection and physical-output acceptance.
