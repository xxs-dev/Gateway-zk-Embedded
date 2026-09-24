# KECP2 transport interface integration

## Integration inputs

Transport base: 6606d5eb4a699fd2f8fc4ee07d187e3bb6189f56.

- Original core 4afaf81008a890936a89844840d0950b3b317c7b was cherry-picked as
  f42cbb47d15b1c7d7243ef1a4951fb90685e6dd2.
- Original core 1011d73c630fdbc88bb665d145488b6704a67616 was cherry-picked as
  13f797588c2a3d0477633f7786aa4094ef2584ca.
- No later membership/voter-roster commits were integrated. Parent should skip
  these two local core cherry-picks and take only the adaptation commit.

## Adaptation

The fixture now supplies senderIncarnation and a nonzero discoveryChallenge.
The duplicate-local-ID test preserves OS boot identity while changing process
incarnation, and checks that both fields survive the real KECP2 codec.

makeEthernetClusterTransport now accepts a third uint64_t configurationHash,
defaulting to zero. A nonzero value is used verbatim for inbound configuration
validation; zero retains the protocol config-only hash for standalone fixtures.
The same signature is wired through the Linux and unsupported-Windows constructors.
Core owns passing node.configurationHash() from main and its own tests. There
is no transport-side roster/hash derivation and no new magic/version parser.

The new supplied-configuration-hash test uses two real loopback transports with
the same non-base hash. It verifies incompatible UDP discovery is rejected and
cannot learn an endpoint, compatible discovery succeeds, TCP frames travel in
both directions, and a subsequent base-hash TCP frame is rejected with stream
closure. The original default-hash cases still run in the same suite.

Only include/edge_gateway/ems_cluster_transport.hpp,
src/ems_cluster_transport.cpp, tools/ems_cluster_transport_bounds_test.cpp and
this evidence subdirectory are changed by the adaptation commit. No core/main,
core-test, downstream or CMake edits were made after the authorized cherry-picks.

## Actual results

GCC 15.2.0, CMake 4.2.3, WSL Ubuntu. See results.tsv for command exit codes.

| Run | Result |
| --- | --- |
| Debug focused transport suite | PASS, 12/12 groups |
| Existing ems_cluster_test | PASS, including real Ethernet loopback election |
| C++14 -O1 focused build and full suite | PASS, 12/12 groups |
| ASAN+UBSAN -O1 build | PASS |
| ASAN+UBSAN full focused suite | FAIL exit 1, 11/12 groups passed |

The existing core binary has no loopback-only selector, so its existing full
suite was run once without editing that test. No old-combination five-round
repeat or graph/release build was performed.

ASAN/UBSAN ran with detect_leaks=1, halt_on_error=1 and UBSAN stack traces.
The only failure was writer-deadlines: "connect wait exceeds absolute 5ms
budget". There was no sanitizer memory/UB diagnostic in that run. The retained
asan-poll-symbols.log shows waitWritable calling fortified __poll_chk, whereas
the fault-injection fixture wraps poll only. Thus the sanitizer build has an
uncovered polling entry point that bypasses the injected connect-timeout path.
No assertion was relaxed and no rerun was used to replace the failed record.
This run is explicitly NOT a green sanitizer qualification. Repairing that
test interception path is separate from this narrow codec/hash adaptation.

All executions use private network/mount/IPC/PID namespaces, loopback only,
private /tmp and /dev/shm, and bounded command timeouts. Native builds include
only the focused transport target and the library/target needed for the existing
core-loopback regression. Source and native test binary SHA256 manifests are
included. No Linaro/GCC6.3.1, old-sysroot, device or AArch64 claim is made.
The previous strict-C++11 shared-header limitation remains unchanged.

## Reproduction

Run this directory's run.sh under WSL root with its /mnt/d worktree path.
It executes each new-combination suite once, records failed/nonexecuted stages,
and returns nonzero if any stage fails. It does not modify host network settings.
No remote/device action, release artifact modification or push was performed.
