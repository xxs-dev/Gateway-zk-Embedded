# Transport bounds: red stage

Baseline: c583bb8aad3e98fca9bb1e1815dd5c62560662d5.
Branch/worktree: ems-transport-bounds-20260924.

`red-confirmed-test.log`: five failing groups, two passing groups, exit 1.
The failures reproduce unbounded UDP drains, accept drains, TCP frame drains,
cluster/config identity admission, and repeated partial-write/POLLOUT progress
renewing the send wait indefinitely. Fault writes make one byte of progress
every 2ms; the fixture terminates even against the baseline.

`red-test.log` is the initial fixture result, retained for transparency. The
writer and reverse-recovery setup omitted UDP endpoint discovery; those setup
errors were corrected before the confirmed baseline run. No product changes
are included in this stage.

Run: `wsl -d Ubuntu -u root -- bash <worktree>/evidence/ems-transport-bounds-20260924/run.sh red-confirmed`
(Use the `/mnt/d/...` worktree path inside WSL.) GCC 15.2.0, CMake 4.2.3;
only the focused target and its three implementation sources were built.
Tests run with private mount/network/IPC/PID namespaces and loopback only,
using the existing isolated runner. Build timeout 120s, test timeout 25s,
fixture alarm 18s. No device, remote, release or sealed artifact mutation.

Shared EmsClusterProtocol currently emits header version 2. Transport has no
version check; the test corrupts the emitted version instead of hardcoding a
protocol version. Version, authentication and payload validation stay owned by
EmsClusterProtocol. The proposed framing changes must retain this boundary.

The focused fault test observes algorithmic work/deadline behavior, not a hard
CPU or wall-clock guarantee under arbitrary process preemption.
