# Native Boot Source Checkpoint

2026-10-02. EDGE, single agent, LOCAL_ONLY. Parent 970d7a6.
Intermediate source checkpoint, NOT fully validated, deployed or production ready.
Exact seven changed source pins and eleven proposed fixture input pins are in JSON.
Existing foreign/untracked evidence and the prior eight-case board receipt are untouched.

## Implemented

- Cold /run lock creation uses root-trusted parent, O_CREAT without truncation,
  NOFOLLOW, empty single-link 0600 file, nonblocking flock and named lstat identity.
- Native boot uses only approved logical v11 names and pinned AArch64 ELF pointctl
  stats --shm. Lock, inactive participants, holders, exact tmpfs, capacity, intent,
  constructor output/header/size and boot-local identities precede reader admission.
  v10 names are never created, copied or renamed. Existing v11 pages are not recharged.
  Initial intent/ready publish through existing no-replace atomic publisher.
- Original-job boot preparation covers pending install/rollback, promoted tail,
  RECOVERED tail and pre-intent approval in runtime-payload. It neither fabricates
  transaction files nor replays package installation.
- Recovery gate performs stopped recovery/native preparation only. A fixed-argv
  nonblocking transient oneshot is ordered After=gateway-ota-recover.service.
  Official production-boot-finish retains original job/approval/transaction and
  executes actual reader startup/health before terminal publication. It verifies
  the dispatched helper pair and refuses execution during recovery activation.
  Failed/uncertain dispatch is not automatically replayed. The same official finish
  entry is reentrant after independently confirming the prior worker is gone.
- Each start persists a new attempt under lock. Content-addressed attempt health
  retains old PID evidence; FINISHED requires this attempt's hash-bound proof.
  Startup/health failure refences exact owned gates and verifies stopped state.
  Gate repair rejects other fragments, overrides, inventory or holder drift.
- No-handoff runtime continuation now starts the promoted candidate before proving
  this boot's health; adoption rollback persists its start attempt and proves the
  previous baseline without requiring a nonexistent production current pointer.
  Three reviewed helpers are staged as approval-pinned, root-owned 0400 files in
  a transaction/boot-specific 0700 directory before stopped recovery restores older
  live helpers. The dispatched worker uses that closed-inventory snapshot.
  Unknown interrupted snapshot staging files fail closed, not silently removed.
- Unfinished handoff pointers are bounded independently of completed journals.
  Legacy compatibility reads only the single active-job pointer; 129 completed
  histories do not trigger the old whole-directory cap. No completed evidence is deleted.
- Index and retirement receipts publish via job/token-owned temporary files.
  Partial prefixes are verified and archived, not silently deleted. Existing correct
  finals, mkdir parent, retained archive and pending-unlink directory fsync retries
  are repaired. Retirement receipt durability precedes pending-pointer deletion.

## Actual Local Results

- Windows publication run: 18 PASS, 5 Linux SKIP, exit0, 2.320s (EARLIER snapshot).
- Added archive-directory retry: 1 PASS, exit0, 0.038s (distinct new case).
- Final health attempt retry: 1 PASS, exit0, 0.050s; old PID101 proof retained,
  attempt2/PID102 reference succeeds. POSIX/service boundaries are substitutes.
- Seven Python sources: Python3.8 grammar PASS, exit0. git diff --check exit0.
  Grammar is not actual Python3.8 execution. Coordinator's independent six index
  cases and health-attempt probe are separate snapshot evidence, not extra test totals.

NO complete final-snapshot Linux pass is claimed. The five skipped lock/fork cases,
eighteen new native-boundary cases and one affected existing boot-tail case have not
executed here. The native fixture substitutes constructor, systemd, proc and network;
even a future fixture PASS would not prove native pointctl or OS reboot.
The two added no-handoff cases are promoted pending runtime and adoption rollback.
Adoption uses the existing private legacy-verifier substitute, so it does NOT prove
that an actual historical verifier admits recreated inode identities on a new boot.

WSL session47230 was queried once, returned no stdout/exit. Its source-load point
remains unknown. No duplicate WSL launch, kill, shutdown or remote access occurred.
No real service, SHM, fence, transaction, config, key, Broker or stable ref changed.

## Remaining Gates

See device-validation-proposal.md for a NOT-AUTHORIZED 23-first bounded fixture,
then separately scoped real creator/systemd and business reboot sequence. The old
cb45 pointctl artifact can be reused only after its complete dependency/pin preflight;
it does not substitute for newly built MQTT/Monitor or a full signed factory package.
Reader admission remains an initial safety policy, not final EMS/SCADA/driver support.
Normal native/systemd reboot, full business qualification, signed HTTPS/RSA OTA and
D1 complete paired factory staging remain OPEN. No remote scope is consumed here.
