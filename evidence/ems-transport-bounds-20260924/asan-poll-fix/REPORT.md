# Fortified poll instrumentation closure

Input commit: dbc8e8b590b3d2c7666ae60d71612f84de7245b9.
This is a test-instrumentation-only follow-up, not a production transport fix.

## Change

The glibc ABI is `int __poll_chk(struct pollfd*, nfds_t, int, size_t)`.
Added a matching linker wrapper and `--wrap=__poll_chk` to the focused CMake
test target and the existing direct-build reproduction scripts. Both poll
entry points share the same writable/timeout injection and counter logic.
Real checked calls still delegate to glibc's __poll_chk. An undersized buffer
also delegates to that checked entry point before any injection, preserving
fortification rather than replacing its bounds failure. Product files and
all deadline assertions remain unchanged.

## Actual execution

- Native GCC 15.2.0, glibc 2.43; `-std=c++17 -O1 -g`.
- ASAN+UBSAN build completed with exit 0, timeout 90s.
- Only existing selector `writer-deadlines` was executed, once, timeout 25s.
- Result: `PASS writer-deadlines`, exit 0; no sanitizer diagnostic.
- ASAN enabled leak detection and halt-on-error; UBSAN enabled halt-on-error
  and stack traces. No sanitizer checks or timing assertions were relaxed.
- Preprocessor evidence confirms `_FORTIFY_SOURCE=3` and `__USE_FORTIFY_LEVEL=3`.
- Disassembly confirms waitWritable calls __wrap___poll_chk in this binary.
- Network/mount/IPC/PID namespaces, private /tmp and /dev/shm, loopback only.

The previous failing ASAN run, symbol evidence and results.tsv under
`../kecp2-interface/` are unchanged. Its existing 11 passing groups were NOT
rerun; neither were the green Debug/C++14/core suites. This closes the specific
failed sanitizer scenario, not a claim of a newly rerun full sanitizer suite.

## Reproduction and scope

Run this directory's run.sh via WSL root using its /mnt/d worktree path.
It creates a separately named native test executable without replacing the
earlier failing binary. Source and binary SHA256, build output, selected test
output, fortify macros, disassembly and exit codes are retained here.

`git diff --exit-code dbc8e8b -- src include` returned 0. The earlier failing
test log, symbol log and results.tsv also have no diff from dbc8e8b.
No core, product header/source, configuration, device, release or remote changes.
No AArch64/Linaro qualification and no hard wall-time claim under arbitrary
preemption is implied. Ready for the requested read-only exit review.
