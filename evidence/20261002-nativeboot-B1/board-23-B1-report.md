# Native Creator 23 B1 Actual Result

Executed exactly once under nativeboot-23-B1-execution-scope.md, 192.168.22.23 / COMM202600999, Python 3.8.10 / AArch64. Payload1950402a5d1c97e661d7132eae065508f32ab194; runnerfe24c61489eb276e1ae7e67243c2a06d3b924bef.

`python -B evidence/20261002-nativeboot-B1/execute-native-creator.py --execute-approved`

**Local/remote/namespace-child exit1. B1 FAILED. Zero of six checks completed qualification; one real native command was partially evaluated.** Child1.092s. No retry/new root/24/C/D/product repair after failure.

## Exact Failure

First real pinned pointctl stats --shm gw_native_b1_f9a79bd_cold exited0, no stderr, all queue counts and sequences0. Output:
```text
shm=/gw_native_b1_f9a79bd_cold latest=0/100000 latestCapacity=100000 pendingWrites=0/4096 pendingCapacity=4095 persistent=0/20000 persistentCapacity=19999 pointUpdates=0/65535 writeSeq=0 persistentSeq=0 pointUpdateSeq=0
```

Runner incorrectly expected pendingCapacity4096/persistentCapacity20000/pointUpdates0/65536. Actual effective ring capacities are4095/19999/65535. Read-only current source (selected blob equals sealed artifact source) confirms src/memory_point_store.cpp:2043/2050/2057 use kMaxPendingWriteSlots-1 / kMaxPersistentSlots-1 / kMaxPointUpdateSlots-1. This is observed harness contract mismatch, not unsafe native admission or a product defect. No code fixed under consumed scope.

Cold direct object_info ABI/header/identity snapshot was NOT reached. Reopen, ptrace interruption, original-inode native reentry/reopen, private size refusal, actual-provider map comparison and final provider hash pass were NOT executed. Do not report cold-check PASS or B qualified, nor claim ptrace support.

## Isolation And Preservation Actually Observed

19 dependency inspection commands exit0, eight providers/loader checked (4429720B aggregate), version/dependency policy passed before native invocation. Provider pins and all raw-log sizes/hashes are in receipt/native log;19raw dependency files retained remotely, not separately downloaded.

Parent namespace mnt:[4026531840] / SHMdev16; namespace child mnt:[4026532695] / private SHMdev33. Recursive private propagation proved before child-only bind/tmpfs; business home readonly; empty48MiB private tmpfs before native. Real private-flock capacity reported MemAvailable2286256128B, required98443264B, additional pages31334400B and64MiB policy slack. Not a measured peak/reservation.

Two uploaded/post-input pins match.16business groups and parent/raw mountinfo/SHMdirectory/business-reader namespace/two-name-absence proof unchanged across preflight/before/after/final (four samples). Group528806 empty, namespace child and native command exited, SSH closed. Parent/business namespace and SHM preserved.

Failure source/log/result/work retained under /tmp/gw-nativeboot-f9a79bd-23-B1. No owned-work removal; final baseline is after group cleanup/conditional cleanup phase, **not after work removal**. Child-private tmpfs has no live group member; no parent business SHM unlink/rename. All A1-A4 and unknown WSL47230 untouched.

## Raw Evidence

- board-23-B1-result.json:220753B SHA25620686442532f51ebf95c8cfffecc7dd1abd6d8814b566f0048df5e34ce4bd705; original Windows CRLF bytes preserved, not reserialized.
- board-23-B1-native.log:14291B SHA25697274d56ed362fc948ca65c99301c9ecdc0d64ffbd0b3ab8481be5ba3abe0f50; exact child stdout recovered from receipt and remote log hash verified.
- board-23-B1-summary.json:actual dependencies/capacity/isolation, partial command, unexecuted boundary, pins and baseline/cleanup facts.

Only one real isolated native stats invocation observed, no complete B acceptance, no business/systemd/reboot/signedOTA acceptance. Wait for independent audit and new precise scope; no additional device/source actions.
