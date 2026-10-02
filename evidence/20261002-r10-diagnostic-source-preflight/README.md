# R10 Three-Product Diagnostic Source Preflight

Batch: `R10-DIAGNOSTIC-SOURCE-6671a3b-4488c00-e4d8655`  
Status: `SOURCE_ONLY_PREDEPLOYMENT`. No model review has been invoked by this
bundle, and no build, target deployment, live Broker, PostgreSQL migration or
end-to-end three-product acceptance is asserted.

Each `*-source.zip` was made with `git -c core.autocrlf=false archive` from
the exact `after` commit in `manifest.json`. Each included ZIP member's bytes
were hashed as a Git blob and compared to `after:path`; the manifest records
its SHA-256, blob ID, size and source commit. Each `*-diagnostic.diff` is a
`git diff before after -- <selected paths>` from the corresponding repository.
The diff may omit unrelated changes between those commits. The selected
sources include only diagnostic changes and necessary process/handler/build
context, not a whole repository or an active worktree snapshot.

The same ZIPs are expanded under `edge/`, `windows/`, and `platform/` for a
Read/Glob/Grep-only reviewer. All 39 extracted regular files were checked
against manifest member SHA-256 and member counts (12/11/16); no additional
source file was copied into those trees. `request.txt` is a byte-for-byte copy
of the coordinator's scoped CLI request, SHA-256
`c098fd0a10447808ac5c89152bd6743b8cff3eaf08743aafb6b3ca7e4f980283`.
It is an instruction for a later explicitly authorized reviewer, not evidence
that a review has run.

Selection: edge process ownership, direct HTTP and MQTT diagnostic handler,
JSON serialization, CMake target split and focused tests; Windows reply
admission, direct/MQTT transports, service, shared Next ViewModel and tests;
platform request binding, ingress/repository/worker, diagnostic payload limit,
both DB migrations, API test, type/UI and focused tests. Edge factory-helper
changes/evidence and platform raw Maven ZIP/XML are intentionally excluded.
The platform evidence-only SHA correction `6ee978b` is provenance, not a
source delta in this batch.

Safe evidence excerpts (summaries, **not raw logs**):

- Edge `evidence/20261002-r10-direct-http/edge-diagnostic-closure.md` records
  final local direct contract 13.44 s, owner 1.30 s and corrected MQTT binary
  exit 0. Its earlier 246-byte truncation fixture failure and zero-matched
  CTest are not passes. No AArch64 installed service or live Broker proof.
- Windows `docs/r10-diagnostic-local-evidence.md` records 19 targeted local
  tests with a fake socket Broker/mocked service, no packaged installation.
- Platform `evidence/r10-diagnostic-platform/focused-result.md` records the
  final 125 local tests and Vue typecheck after strict worker target checking
  was restored; H2/MockMvc/Paho mock, not PostgreSQL/live Broker proof. The
  corrected raw evidence ZIP SHA is
  `2a3dfc4a41b4122983381b822eb0d09f39ffe6a314b918cfd8fbacc880dca180`;
  that ZIP is not bundled.

No auth headers, credentials, live tokens, private keys or unredacted raw
MockMvc/XML logs were copied as evidence. Source tests deliberately contain
synthetic `FAKE_KEY_MATERIAL_123` and `Bearer durable-token-value` strings to
exercise redaction; those are fixtures, not operational secrets. Reviewers
should keep source/test fixture contents distinct from deployed credentials.
