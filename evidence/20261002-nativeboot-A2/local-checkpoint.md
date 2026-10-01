# A2 Local Checkpoint

2026-10-02. Single EDGE agent, LOCAL_ONLY. Parent aee0934.
A1 runner/receipt/source evidence remain unchanged. No connection, WSL launch,
device repair, business build, service operation, push or coordinator edit.

## Five A1 Fixture Corrections

- Scalar show accepts only the exact ActiveState/--value grammar and one owned
  unit. Batch show retains Id/ActiveState/MainPID and uses actual fixture state.
  Unknown units/options/multiple scalar units fail closed. Two portable tests
  execute the actual adapter methods extracted from the existing fixture.
- Pending-job setup binds the approved transaction previous.json instead of asking
  production_context to allow an unresolved installation; its rejection remains
  explicitly asserted. Subsequent original-job tests assert prepare_boot was reached.
- The affected boot-tail test snapshots actual /proc/locks under the official
  mapped private lock and proves its owner before staging. No invented lock rows.
- File-fsync injection binds a regular fd to the exact owned temporary-file
  dev/inode. POSIX additionally asserts parent-directory fsync was passed first.
  Partial archival count/bytes/retirement durability assertions are unchanged.
- Same-boot zero-mutation permits only the four exact read-only inventory calls;
  live units, both SHM identities/size/header, pointer bytes and config stay equal.

## One Product Fix: Dispatch Intent

Only offline-runtime-upgrade.py changed: add DISPATCH_INTENT to the valid phase
set and persist it BEFORE subprocess.run. PREPARED means no dispatcher side effect
was attempted yet; failure before that intent publication may still permit the
first actual dispatch. DISPATCH_INTENT means delivery is UNKNOWN (including zero
calls if intent publication succeeded but reported failure). DISPATCHED records
an accepted bounded response, NOT healthy completion. Gate reentry never sends
again from either state and never clears an intent or declares FINISHED.

Recovery uses the same pinned immutable entry's official production-boot-finish,
original transaction/approval and offline lock after independently establishing
the prior worker is absent and the recovery oneshot has completed. No cleanup,
replacement job or reinstall is authorized by this checkpoint. The Linux fixture
covers three uncertain windows, strict phase/approval rejection, actual substituted
reader startup/health, hash-bound attempt1 and completed reentry without attempt2.
This full finish chain is authored, NOT executed here; it is an A2 acceptance gate.

## Actual Local Results

- Fixture adapter RED: 2 tests, 4 unittest failures (3 are subtests), exit1.
- Frozen e3739ba dispatcher RED: 3 tests/3 failures, exit1 (dispatch-red.log).
- Final focused Windows run: 7 PASS/0 skip/error/failure, exit0, 0.169s.
  These are real fixture methods/dispatcher AST and private files, with Windows
  POSIX trust/dirfsync and manager/authority substitutes, not Linux finish proof.
- Five changed Python files parse with Python3.8 grammar; diff-check exit0.
  This is not execution under Python3.8. No full final Linux pass is claimed.
- Coordinator's separate six dispatch-order windows are not rerun or added to
  these counts. Original WSL47230 result/load point remains unknown.

## A2 Review-Only Execution Proposal

11 Git inputs, 484012 bytes; exact pins and 27 selectors in local-checkpoint.json.
Select only A1's 21 failed/error cases plus 6 new selectors (2 show, 3 dispatch,
1 native official-finish test with 3 subwindows counted once). Do not repeat A1's
23 passing selectors, old eight cases, ARM/MQTT/build or historical full suites.

Reuse the unchanged A1 runner except fixed commit/manifest/pins/selectors/count/
fresh root/receipt. Default dry-run never imports the connector. 23-only root-private
source0444/directories0500, private TMPDIR/cwd; 240s group/subreaper cleanup,
outer300s, CPU180s, AS384MiB, file64MiB, fd128, core0 and original output bounds.
Before/after/final 16-group baseline and 11 posthash assertions remain. Only a full
27PASS/no skip/error plus unchanged baseline permits owned work removal; then take
the final baseline. Failure retains work/evidence, stops, and does not retry/turn24.

Runner and exact new namespace are fixed in the following runner-only commit.
No A2 execution is authorized here. Real pointctl/systemd/OS reboot, complete
configured business, signed HTTPS OTA and D1 pairing remain separate gates.
