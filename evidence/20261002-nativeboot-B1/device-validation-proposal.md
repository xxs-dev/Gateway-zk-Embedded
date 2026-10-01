# B1 Native Creator Fixed Preparation

LOCAL_ONLY. Not execution authorization. A1-A4 are sealed; no SSH/device/build/VM/Broker operation, no repeat of their tests.

## Fixed Material And Boundary

Reuse sealed cb45 pointctl: 674504 bytes, SHA f6f4a0a94d55827e874e45b9484daf8e9af74bae60ac048e7416ba433881c813, extracted in memory from the package 0b82b91f93675c61b0d807c39bc711346c8899871ae29b34f5ca8178e9198549. Local pyelftools confirmed ELF64 LE AArch64, interpreter /lib/ld-linux-aarch64.so.1, seven needed libraries, no RPATH/RUNPATH and exact version requirements. 11 selected C++ source blobs still equal the archived audit; not all current dependencies or whole-product equivalence. Binary is not committed/rebuilt, not installed into /opt.

Future upload: only pinned native-child.py and exact ELF (two inputs), root-owned source 0500, script 0444, ELF 0555 under fresh /tmp/gw-nativeboot-f9a79bd-23-B1. Default runner dry-run has no helper import/connection. Precise new scope required.

Before native execution, readelf/ldconfig-p inspect ELF/loader and trusted canonical system providers, exact direct dependencies/closed transitive dependency set and symbol definitions. Root-owned/nonwritable tool and library paths are hash-bound; legitimate /lib aliases resolved. Read-only metadata logs (19 x <=1MiB) retained; aggregate provider hash reads <=64MiB per pass. Missing/ambiguous dependency, utility or kernel support stops, no install/environment fallback.

## Namespace And Ownership

Only namespace child calls CLONE_NEWNS, proves distinct mount namespace, then recursive MS_PRIVATE and absence of propagation tags BEFORE mounting. It gives /opt/modbus-gateway a child-only readonly bind (unsupported subtree mounts refuse), then mounts <=48MiB private tmpfs over /dev/shm with nodev/nosuid/noexec/0700. Verify different SHM device, exact visible tmpfs row, initially empty directory. Parent namespace is never unshared/mounted.

Two exact names: gw_native_b1_f9a79bd_cold / gw_native_b1_f9a79bd_interrupt. Parent sees neither before/after. All commands are the pinned ELF stats --shm <one-name>, no --app-config, write/pending/control or second daemon. Scrubbed environment, no LD_PRELOAD. Hold a newly created private offline flock under work, never the business offline lock.

Parent samples existing 16 protected groups plus namespace/raw mountinfo/SHM-directory proof and each business reader mount namespace, before/after/final after cleanup. Unknown/drift stops. Native tmpfs disappears only when all namespace members exit/reap; no business SHM unlink/rename.

## Six Checks

1. Cold native stats construction: ABI11 exact 15667032 bytes, magic/version, root0600/single link, empty queue counts/sequences and native getStats successful.
2. Reopen same name: native stats again, identical dev/inode/header/size/mode.
3. Interrupt second name: own fork/TRACEME stats executable only; PTRACE_GET_SYSCALL_INFO must prove native AUDIT_ARCH_AARCH64 (0xc00000b7), ftruncate syscall46, length15667032, return0. Resolve tracee FD exact current-private name/dev/inode and same namespace/group. Stop at syscall-exit before returning to user-space, verify kernel-created zero header; SIGKILL/reap only this tracee, never resume beyond this point. Verify actual loaded providers against inspected pins.
4. Reinvoke native stats for that exact zero-header object: native initialization, same inode, ABI11/empty queues. Python never writes a header/mutex/layout.
5. Reopen resumed object: same identity and native empty stats.
6. Explicit private negative: truncate only this second private completed object by one byte, then native stats must exit1 with exact size-mismatch error; full object hash/identity unchanged on refusal. This is deliberate private corruption, NOT an asserted crash-produced size.

Only check3 proves the precise ftruncate-success/pre-user-init interruption window. Other magic/version publication stages, robust mutex owner-death and arbitrary initialization-instruction windows are NOT covered. Unsupported ptrace/syscall info/arch/FD/event/deadline stops without changing ptrace settings or guessing timing.

## Capacity, Bounds And Evidence

Before each native operation, with positive private kernel flock and namespace proof, inspect REAL MemAvailable plus private tmpfs statvfs. Reserve only still-unallocated pages for up to two roles (4096-page rounding =>2x15667200 initially) plus 64MiB serial Python/native/OS policy slack. Existing pages already reduce available memory, no v10/retained-object double debit, no migration CLI heap. This is policy admission, not a measured peak or reservation.

Namespace child wall180s, outer240s, inherited controller AS384MiB/CPU180/file64MiB/fd128/core0. Native CLI each wall15s/CPU15/AS256MiB/file32MiB/fd64/core0, live-capped stdout and stderr each4KiB. Native file limit must exceed layout size so it does not accidentally prevent ftruncate. Each metadata utility10s and log<=1MiB. Parent log<=256KiB, stdout<=1MiB, stderr<=16KiB. Own group/subreaper TERM/KILL/reap reused from A4; no host/full/global stop.

Only all6 checks, empty group, unchanged parent namespace/business baseline and two post-input pins permit inode-verified removal of owned work. Final baseline repeats AFTER removal. Source/ELF/dependency logs/native receipt/parent raw baselines retained. Failure/timeout/unknown retains work/evidence and stops, no retry/new root/24/B/C/D. No execution during preparation.

## Still Needed

B does not establish formal production boot proofs or real systemd/OS reboot. C manager gate/worker ordering and D newer MQTT/Monitor/full configured runtime factory pairing/build/deployment/native-reboot/health and signed HTTPS original-job OTA remain necessary. Full drivers/EMS/SCADA, receiver outage/reconnect/main acquisition continuity, D1 factory boundary, queued R10 diagnostics and cross-end requirements are not replaced by reader-only or sealed cb45 artifacts.
