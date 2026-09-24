# Transport bounds: integration handoff

Base: c583bb8aad3e98fca9bb1e1815dd5c62560662d5.
Red commit: 965feff0fd7dac3b6e920b1b95d6a663cba06db3.
Branch/worktree: ems-transport-bounds-20260924.
Only product file changed: src/ems_cluster_transport.cpp. Other changes are
the focused test, its CMake registration, and this evidence directory.
No shared header/core/main/point bridge/graph implementation changes.

## Bounds

Let M = maxMembers clamped to the supported resource envelope 2..5.

| Resource/work | Bound |
| --- | --- |
| Retained TCP connections | 2*M, plus one transient accepted fd |
| Inbound unauthenticated admission | reject when M pending connections exist |
| TCP accept calls per poll | M |
| Input storage per connection | one 64 KiB frame, reserved once |
| TCP reads per poll | 16 KiB*M bytes, 16*M recv calls, 4*M decoded frames |
| TCP per-connection service | 16 KiB / 4 frames, rotating fd order |
| UDP per poll | 2*M recvfrom calls, 64 KiB*M received bytes |
| Endpoint/peer identity table | M-1 remote identities |
| Partial frame / unverified stream | absolute 1000 ms, not refreshed by bytes |
| Endpoint/identified stream idle retention | 5000 ms |
| Whole send, including all broadcast recipients | absolute steady-clock 20 ms |
| Connect | absolute 5 ms, also constrained by whole-send deadline |
| Send syscall attempts per peer | 128, also constrained by deadline |
| Cached seed destinations | first M configured seeds, resolved at startup |

TCP buffers request 64 KiB receive / 16 KiB send; Linux socket accounting may
double these values and includes additional kernel overhead. At M=5 the input
vectors reserve at most 640 KiB on the tested libstdc++ implementation; this is
not a total-process RSS guarantee. No deferred outbound frame queue is added.
Failure/expiry/stop resets streams to discard queued bytes. TCP_USER_TIMEOUT,
when available in the sysroot, limits unacknowledged/zero-window queued data to
1000 ms at the kernel's timer granularity. Bytes already delivered to the peer
cannot be retracted: core incarnation/correlation/lease/TTL checks remain required.

Authentication and wire version/magic checks remain in EmsClusterProtocol.
Transport checks cluster/config hashes and nonempty sender identity before
learning endpoints. A stream cannot change its identified/expected sender ID.
Authenticated compatible same-local-ID messages are delivered to core under
the same budgets, never mapped to a self endpoint; their incoming TCP stream
is closed after delivery. Core owns self-multicast suppression and duplicate
incarnation quarantine. No consensus/replay window is duplicated here.

## Results

- Confirmed RED: five failing groups, two passing groups, exit 1; preserved.
- `green-identity-test.log`: all 11 focused socket groups pass.
- `repeat-1.log` through `repeat-5.log`: all 11 groups pass in each run.
- `ems_cluster_test.log`: existing cluster suite passes.
- `graph_ems_cluster_dispatch_test.log`: existing graph suite passes.
- `cxx14-syntax.log`: strict C++14 syntax, -Wall -Wextra -Werror, passes.
- Optimized C++14 full run initially reports only the large-frame group failing.
  Diagnosis (`cxx14-counter-diagnostic.log`): bytes=0 although the connection
  closes correctly. GCC/glibc fortification calls __recv_chk, bypassing the
  original recv-only test counter. The test now wraps the checked entry points
  without disabling fortification. `cxx14-targeted-test.log`: the previously
  failing large-frame/partial-expiry group passes with the corrected counters.
  The earlier readiness-wait hypothesis was insufficient, as retained logs show.
- Per the final instruction, no already-passing groups were rerun after that
  test-only instrumentation correction. Product code is unchanged since the
  11-group repeated passes. Sanitizers were NOT RUN (the verification script
  stopped at the original counter failure before reaching that optional work).

GCC 15.2.0 / CMake 4.2.3, WSL Ubuntu, real local sockets. Test-only wrappers
inject repeated writable/partial-send progress and connect timeout, while
other cases exercise real receive floods and a real non-reading TCP receiver.
Runs use private network/mount/IPC/PID namespaces and loopback only. Final
regressions also mount private /tmp and /dev/shm. No host network changes.
`source-sha256.txt` records exact working-file bytes; `native-test-sha256.txt`
records native test binaries, not release/device artifacts. The optimized
targeted binary includes the final instrumentation; the Debug binary records
the preceding full 11-group run. Builds/tests have explicit timeout/alarm caps.

## Integration And Limits

Cherry-pick RED then this green commit onto the KECP2 candidate. CMake changes
are solely the transport test block; product library source lists are untouched.
Framing reads offsets 6..11, header size 16 and tag size 32, as in the agreed
shared frame envelope. It never checks magic or version. Changing core to
magic 0x4b454332/version 2 needs no transport magic adjustment. These runs used
the specified baseline codec, not the still-changing integrated KECP2 core.
The baseline test fixture uses senderBootId; when integrating new mandatory
incarnation fields, initialize senderIncarnation in messageFor and the duplicate
identity fixture, then run only the necessary combined regressions.

New transport code uses C++11-language facilities, but a strict C++11 translation
unit is blocked by existing compat.hpp (experimental optional/shared_timed_mutex
require C++14); see cxx11-header-limitation.log. No claim of a GCC6.3.1/old-sysroot
or AArch64 build is made. Parent must validate its real target toolchain and
TCP_USER_TIMEOUT support. No cross/remote/device/release build or push occurred.

Bounds limit algorithmic work/storage and cooperative monotonic waits; arbitrary
OS preemption, kernel scheduling and crypto execution do not have a hard CPU or
wall-time guarantee. DNS can still block startup, never send/poll. Exhaustion
drops new identities/connections fail-closed; this is not DoS-proof availability.
Authenticated discovery replay may still refresh/redirect a bounded endpoint;
membership/liveness/authority/replay admission remain core responsibilities.
