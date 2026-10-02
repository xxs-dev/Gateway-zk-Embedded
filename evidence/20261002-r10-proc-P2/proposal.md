# R10 P2 Read-Only Proposal

2026-10-02, LOCAL_ONLY. Preparation only, no new SSH/device authorization.
B1 consumed its single scope with exit1 before GO; namespace closure remains
unconfirmed. Original B1 bytes are archived in ../20261002-r10-board/B1-execution/.
No B2 preparation, product repair, signal, cleanup or rebuild is included here.

## Fixed Input

Target only192.168.22.23/COMM202600999, original boot
87ba3880-1293-4b45-8826-8386e8ffb40f; changed boot refuses.
Three literal namespace references:pid:[4026532696],mnt:[4026532695],net:[4026532698].
Only literal B1 root /tmp/gw-r10-68de70f-23-B1; no new remote directory/upload.

Manifest SHA256e78a0f7b57fbda00ae54230de773017dc0fc521eea3e8b10730a9ae74a4b0fa6.
execute-proc-readonly.py SHA17814edf21d5dccf22e1b6866348647561e963e62f3799f8f6308e684a434073.
proc-readonly.py SHA6b5c5d5f4d1a56ddea33ec5d16bd9f2ebf7c80ef359f85a476dfe0460ffa415b.
proc_readonly_test.py SHA55c7f4894353f5ef98ec6bde1359808d93beff446de99490dfc8abb2f41c884f.
Exact Git-held B2 baseline and existing credential helper/probe remain SHA-pinned.
This target interpreter isPython3.8; the own TID uses threading.get_native_id.

Default local-only review command:
`python -B evidence/20261002-r10-proc-P2/execute-proc-readonly.py`.
Execution flag plus this exact manifest SHA requires a separate new P2 scope.

## Proposed One Read-Only Request

Send fixed code via the existing helper/invoke, not a remote installed script.
Observe kernel release and <=4096B /proc/version; read current main-thread
/proc/self/task/<tid>/children and /proc/thread-self/children independently.
Record raw <=65536B or actual errno/type/message, never infer kernel config from
ENOENT alone or silently substitute another interface for the observation.

Bounded /proc inventory:<=4096 numeric process entries,<=512FD/process,
<=16384FD total,10s scan deadline,<=128 matching records,<=32 read errors.
Inspect pid/pid_for_children/mnt/net membership and namespace FD references,
record PID/startTicks/state. Missing optional pid_for_children is tolerated;
other membership/read errors or disappearing processes mean UNKNOWN.
Already-closed FD entries are counted separately (including enumeration's own
temporary FD). Inventory is non-atomic; NO_REFERENCES_OBSERVED is an observation,
not namespace kernel refcount certification. Reused IDs are not authority to kill.
Any holder/unknown result is only reported, with nonzero exit, no rescue action.

Read root/source/bin/work metadata,<=16 entries per directory, no recursive
traversal or symlink following. Read only the root receipt<=128KiB and require
original B1 SHAacf7d4d7d85a1e61563cb4b094e952ef701a3e0f5760e30044c38d0718d25d2f;
return digest and bounded error/baseline summary, not an unbounded historic tree.
Directory lists and file reads stop at bounds while collecting, not after loading
arbitrary-size content. Snapshot root/receipt before and after; drift refuses.

Reuse unchanged B2 business16-key/parent-namespace sampler before and after:
identity/boot/state/approval/active/guard/config/marker/readers/PID start/maps,
segments and lock; raw sampler JSON/commands preserved. systemctl show only,
no start/stop/restart/control/config/MQTT or real SHM contents/migration.
No root deletion, signal, mount/unshare/chroot/kernel settings/tool installation.

One remote request90s plus existing timeout kill-after20s, stdout<=1MiB,
stderr<=16KiB; local exclusive P2-result.json reserves invocation before connect.
Record original output/exit/SSHclosed, stop on failure/unknown, no retry or extra
retrieval connection. No other host/WSL/VM/Broker or business action.

## Local Verification And Open Items

Final5 private Windows tests PASS0.012s/0skip/exit0: actual missing-file errno,
actual private read bytes, interface overflow refusal, directory collection bound,
AST no mutation primitives/fixed targets. No Linux /proc inventory behavior proved.
Freeze and default dry-run exit0/connects=false; generated codePython3.8 grammar
checked during preparation. Initial4-test draft passed0.013s; not added as separate
coverage. Earlier manifest draft retained locally, excluded from final scope.
No new connection or commit in this preparation.

Local source-only inspection finds src/diagnostic_process.cpp:57 and:114 open
/proc/thread-self/children. That compatibility concern belongs to a later product
phase. No product ELF ran in B1, so there is still no observed product failure.
P2 cannot substitute for six real diagnostic tests, verified native boot, business
deployment or OTA. Await independent code/manifest/scope review before execution.
