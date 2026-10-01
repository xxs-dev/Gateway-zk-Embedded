# R10 Linux Validation Proposal (Not Authorized)

This proposal has no automatic connection or execution runner. LOCAL_ONLY remains
in force. Before authorization, coordinator must fix an exact Linux environment,
source commit/blob SHA256 manifest and owned outer process-cleanup runner.
No dependency installation, shared-tree writes or business build is implicit.

## First Bounded Slice

Transfer only the three fixed Git blobs: diagnostic_process.hpp,
diagnostic_process.cpp and diagnostic_process_test.cpp from this checkpoint.
Verify their SHA256 against Git bytes, not CRLF checkout assumptions. Use one
fresh coordinator-approved private root (0700), never /opt business paths.
The standalone target compiles helper separately with test hooks; production
edge_gateway does not receive those hooks.

Proposed Linux compiler command, from that isolated root:

```sh
g++ -std=c++17 -Wall -Wextra -Werror -pthread \
  -DEDGE_DIAGNOSTIC_PROCESS_TEST_HOOKS -I include \
  src/diagnostic_process.cpp tools/diagnostic_process_test.cpp \
  -o diagnostic-process-test
./diagnostic-process-test
```

Compiler outer timeout60 s; test outer timeout30 s; no retry. Suggested ceilings:
address space512 MiB, output/log8 MiB, FDs256, CPU20 s for test (compiler separately
120 s), no resource-exhaustion fault injection. Confirm compatibility first.

The existing test exercises silent deadline/ticks, continuous output cap, exit1,
signal termination,128 KiB final drain, deterministic exited-owner status race,
modeled live-owner uncertainty/retained-owner CPU backoff, truncated wire, pre-command owner death, held
writer EOF, escaped descendants, destructor and explicit stop.
It does NOT test actual parent death, post-launch supervisor death or inherited
FD closure. Those require separate narrow fixture changes and review first.

The outer runner must be a private subreaper and record exact children before
executing. Descendants deliberately use setsid: killpg(parent) alone is insufficient.
On timeout/error it must retain/reap provably owned descendants (including escaped
groups), preserve logs, and fail cleanup if ownership cannot be established.
Do not claim timeout alone makes the existing executable safe to run.

Capture raw compile/test exit codes, uname/compiler version, stdout/stderr,
process inventory before/after, and exact-root cleanup evidence. Stop on any
compile/test/ownership anomaly; do not automatically run a second host.

## Separate Service Slice

system_monitor_diagnostic_test requires the exact matching edge_gateway library
and linked dependencies, plus the existing system_monitor_service_test regression.
This is not authorized by the standalone compile scope. The new fixture uses a
private fake PATH top, in-memory publisher, empty router and private SCADA files;
no business daemon, real SHM, Broker publish, control target, service or config
change is intended. Its Linux body has not been compiled locally.

Full Allwinner builds use only the authorized22.11 toolchain with isolated exact
source synchronization; this proposal grants neither that build nor a deployment.
Actual transport/deployment and ownership-failure gates remain required.
