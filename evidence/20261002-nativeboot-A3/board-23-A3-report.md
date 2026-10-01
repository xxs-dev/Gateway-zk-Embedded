# Native Boot 23 A3 Actual Execution

Executed exactly once under coordinator scope 6f35279, on 192.168.22.23 / COMM202600999. Source 73e2f9f627070ebb4ac03e41c0a50ce6b2032761; runner d4c7b8e11df127679ba04adee92fe7dcde7b18d9. Python 3.8.10 / AArch64.

`python -B evidence/20261002-nativeboot-A3/execute-board-fixture.py --execute-approved`

Local / remote / fixture exit **1**. Fixture 70.004s; unittest 69.204s. **31 tests: 30 PASS, 1 FAIL, 0 ERROR, 0 SKIP.** Original session 19203 completed, SSH closed, process group 524931 absent. No retry, new root, other target, product repair or WSL operation after failure.

## Failure And Limits

Only `NativeBootTest.test_memory_or_tmpfs_insufficient_refuses_before_intent_or_native_call` failed at line 197: its regex `capacity|memory` does not match the real `ValueError: insufficient MemAvailable for activation` with private MemAvailable=0. This is an observed assertion-message mismatch, not unsafe admission. The selector remains failed: its subsequent zero-mutation assertion and tmpfs-zero branch were **not reached**. Do not count that partial selector as passing or infer those checks ran.

Other selected original-job pending/promoted/rollback/pre-intent, no-handoff runtime promoted/adoption rollback, uncertain-dispatch three subwindows and boot attempt/health private finish cases passed. The 11 new quiescence and 2 affected same-boot selectors passed. This is Linux/Python fixture evidence with substituted native constructor/systemd/proc/network, not an actual native creator, OS reboot, systemd ordering, business deployment or signed HTTPS OTA result.

## Preservation

All 11 input and post-input hashes match the fixed Git blob manifest. All 16 protected business baseline groups match across preflight/before/after/final (four samples), including business PIDs/start ticks/ELFs, SHM identities/maps, state/active/config/guard/marker and lock. Raw baseline command output is retained inside the original receipt.

Failure root `/tmp/gw-nativeboot-73e2f9f-23-A3` and its source/log/result/work are retained; work was not removed. Group cleanup completed, then final baseline/posthash ran. This is not a claim of a final baseline after successful work removal. A1/A2 remote work and local evidence remain untouched.

## Raw Evidence

- board-23-A3-result.json: 276064 bytes, SHA256 415eb59556ab17650e408b54c7ab1756da1f1ffdcb2d64bf58f628ad6202fcbd. Original Windows-written CRLF bytes preserved, not reserialized.
- board-23-A3-fixture.log: 56715 bytes, SHA256 6173a1451d82084c5640e27eff84039b8caee1b772c10c2194fc5f48a92f3726. Exact UTF-8 log recovered from the original receipt and verified against its remote log pin.
- board-23-A3-summary.json: structured counts, selector list, posthashes and preservation/untested boundaries.

Raw plan `remoteAuthorized=false` records preparation provenance; the later scope authorized this one command. It has not been rewritten. Wait for independent audit/new scope. No additional device work or source repair under this consumed scope.
