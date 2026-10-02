# R10 L2 Actual Build Result

2026-10-02 (Asia/Shanghai). One authorized build-only invocation completed.
No board/test executable, VM, WSL, Broker or business deployment operation.
Runner/source/product code was not modified after approval. No retry, new root,
extra retrieval connection, installation, clean, package or publish operation.

## Actual Command And Result

Runner ab3ac46d3b3dfe73d79958dfe7112480c69bcc13.
Manifest92ab58ad902f63e1742ddb7d133367b3e3435f011b7bc35a6c339aa453a4de06.
Entry750011726091df3dfb31c5549442a1521f15567d3b7534aa947b6f5054d5314a.

```powershell
python -B evidence/20261002-r10-linux/execute-r10.py --stage build --execute-approved --manifest-sha256 92ab58ad902f63e1742ddb7d133367b3e3435f011b7bc35a6c339aa453a4de06
```

Authoritative local session66878 completed exit0. Only its existing handle was
polled. Preflight exit0, remote build exit0, phaseBUILT, stderr empty,
returnedArtifactHashesVerified=true, sshClosed=true. No primaryError,
postcheckErrors or receiptErrors. All local command cells have completed.
The provenance field executionAuthorized=false remains the original preparation
value; execution was authorized separately by r10-build-L2-execution-scope.md.

Remote root /srv/build/gw-r10-68de70f-build-L2 retained with source/build/logs.
Observed Python3.6.9, /usr/bin/git2.17.1, /usr/bin/cmake3.25.2,
/bin/bash4.4.20, LinaroGCC6.3.1. Boot56c6e908-c036-4305-8f6f-44e49c3bca93.
Preflight MemAvailable6511804416B, disk free108713005056B (point-in-time values).

Two commands completed exit0: existing build_edge_aarch64.sh --jobs2 with exactly
the three approved targets (plus required edge_gateway library), then separate
ownership-probe compilation. Owned launcher/init PID pairs98967/98968 and
99504/99505; each normal-completion cleanup recorded zero namespace survivors.
These observations do NOT test forced parent death, pre-GO failure, setsid
escape, timeout or product supervisor cleanup. No compiled test was executed.

## Source And Shared Proof

Source438507cdb191b5b91c77eca2e672b4e74e06bd1f; product68de70f unchanged.
8140800B archive SHA256
dd8bc90081b89cd52ff7fd7abd2e1633a2c646e2590279a9166a42dfb4d2080f.
All442 post-build source hashes verified. Shared HEAD before/after:
746169f3ffcbe24c631f83eb3c6860622b664de0; path-status SHA before/after:
b9f7824c79edc0e8257623d3f5042e0a75ec45b2ecc5d77b0391dfaa628deeb8.
sharedUnchanged=true covers HEAD, path status and owner/path identity only,
NOT complete shared-tree file contents. Git ran as UID/GID1000 with fixed
read-only vocabulary; no intentional shared source changes.

## Returned Artifacts

All four ELF64 little-endian AArch64, /lib/ld-linux-aarch64.so.1 interpreter;
exact local hashes match remote receipt. ELF metadata is preserved separately.

| Artifact | Bytes | SHA256 |
| --- | ---: | --- |
| diagnostic_process_test | 47584 | 5c106996977066de749c236eada781ecbc52cfbb103a1c00b55bab68008f5d56 |
| system_monitor_diagnostic_test | 3381256 | b1998ee50ae4fddd3f2d6c6ed5289c2ef070597bd3b071febb7f0c7446b9028b |
| system_monitor_service_test | 3504368 | 6b5651c08fe3f1ff4bbf17a92c2f1d5043c43d03f8fcf80ed3bc8542e189e332 |
| ownership-probe | 53960 | eff1af135f84221b7b7401e58647f1607c2df8621214e1620dafdeb15dd3b3b7 |

Original bytes retained in ../build-L2-result.json (local receipt),
../L2-remote-* (remote receipt/two logs/four ELF metadata logs), and
../L2-artifacts/ (four ELF files). raw-evidence.zip preserves those twelve files
without rewriting their original bytes; summary.json inventories all digests.
ZIP2210893B SHAfae73113e5fc1cd061d90fdbbe7ecf2b942b04e5a36696bca5cc5002413f69bf.
Local archival verification exit0:12/12 ZIP members equal originals and their
summary hashes; local/remote receipt JSON equal;4/4 returned ELF headers and
metadata digests match. This verification did not execute any ELF.

Local receipt SHA923ff21f13357bfe92681950d3116da5d927f4d2f35ac485f1afcb8f062e1493.
Remote receipt SHA77aada369db5f7a332c45cf1342a7e82a93150d62050530f1f145eb1e16df911.
Build log SHA7263fb0a3df3af8ef9f1b4f56f8d5481cba741bbea456c16db0a56e3b67587e5.
Ownership log SHA26b7b3f9cca6d91fb852f2041ca21e192ff18603d9fd295c44c2c1a187c84f60.

## Remaining Gates

This proves the authorized cross-compilation/return and normal builder boundary
on this Linux host, not target runtime dependency compatibility or diagnostic
test success. AArch64 suites, FD/parent-death/supervisor-death behavior, timeout
fault cleanup, board isolation, full business deployment, native boot and signed
OTA remain unexecuted by this scope. Board stage has no execution route here.
No further connection or board work without new authorization.
L1/P1 evidence and unknown local changes remain untouched. No push or stable
reference movement. This local archival has not been committed.
