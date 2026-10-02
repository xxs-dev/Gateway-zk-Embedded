# Production configuration authority path (review proposal)

Status: SOURCE_ONLY_PREDEPLOYMENT, not an enabled remote write path. Existing
ordinary MQTT/direct mutations still reject a production journal root before
writing. The v2 journal can install an exact approved config delta locally,
but current client publication does not supply that approval.

## Existing trust boundaries

- Platform `ConfigRevisionController` already separates `config:approve` from
  `config:publish`; the service rejects uploader/submitter self-approval.
  Publish precheck requires a publishable revision, a matching incremental
  base, public HTTPS and signing readiness. `publish` durably creates an
  idempotent task and returns its ID; the worker sends the signed OTA command.
- `edge-ota-v1` signs `jobId`, `machineCode`, `packageType`, `version`, artifact
  SHA256 and size. The digest authenticates the exact artifact bytes, not a v2
  baseline directly. The platform sends the scoped MQTT request non-retained.
  Edge must verify the signature, size and digest before treating enclosed
  metadata as authority. SHA256 alone is not an approval signature.
- Current production runtime handoff admits only a signed, sized `full`
  artifact. `production_ota_owner` then requires a full ABI11 manifest, an
  allowed parent and expiry; for v2 it synthesizes `configWriteSet=[]`. This
  is code-only OTA, not signed configuration publication. Removing only the
  package-type check would not create a safe config path.
- Direct maintenance has a device credential, provisioning, lockout and
  `sensitiveApiEnabled`. This authenticates local maintenance access but does
  not establish `config:approve`/`config:publish` or authorize v2 pins. The
  MQTT publisher classifies scoped topics; SystemMonitor receives payloads.
  A payload `machineCode` proves neither sender identity nor broker ACL.

## Minimal asynchronous path to implement and review

1. An authenticated platform user with `config:publish` submits a particular
   already-approved revision. Keep the existing distinct `config:approve`
   reviewer and the immutable revision/artifact binding; the operator or
   Windows client does not self-approve. Platform creates one durable task for
   the supplied idempotency key and freezes requester, revision, target,
   original task/job ID, artifact SHA/size and previous deployment identity.
2. Platform prepares a reviewed config-capable signed artifact. Its bytes
   must carry the exact old/new v2 baseline pins, `configWriteSet` including
   additions/deletions/restores, role authority and profile digest, candidate
   files and config payload inventory, and the original task ID. Artifact
   build, signing and task persistence must prevent an approved revision from
   mutating between review and dispatch. Publish precheck is advisory; worker
   rechecks approval, base, target and artifact binding immediately before
   signing. This requires a new reviewed package/schema contract, not a
   reinterpretation of today's code-only `full` manifest.
3. Edge verifies the signed descriptor and exact artifact bytes, rejects
   retained MQTT requests, target/type/parent mismatch and stale or duplicate
   job IDs, then persists the original ID and approval digest under the
   existing single OTA owner and `ProductionRuntime` journal before mutation.
   `inputs` independently compares old pins to the current trusted baseline,
   computes the complete delta, verifies the configured board capability and
   role profile, identity, network, TLS, OTA trust, resource/start/health
   policy and approved write set. Unknown roles, paths or protected changes
   fail closed. The caller cannot choose a more permissive capability.
4. Owner handoff and `install/recover/promotion` stay asynchronous. An ACK
   means accepted for processing, not healthy completion. The platform inbox
   correlates replies/status/health to the original task, target, artifact
   digest and attempt; a wrong-candidate or late result cannot replace its
   terminal. Query `GET /api/v2/tasks/{taskId}` and device deployment state
   by original ID after uncertain send, timeout or reconnect. Same-ID retries
   must reproduce identical frozen bytes or return conflict, never execute a
   second changed request. Expiry and durable consumed-job history bound
   replay across process restart; retained delivery is rejected, not used as
   a retry mechanism. Observation timeout remains UNKNOWN until queried or
   officially reconciled, distinct from edge execution timeout/rollback.
5. Windows should obtain/retain the platform task ID and immutable request
   metadata for audit, UI and export, then query task/deployment state.
   Direct maintenance and ordinary config MQTT may submit to this authorized
   path or present the same signed artifact; they must not promote their raw
   JSON or device password to approval. The current direct/MQTT production
   write refusal remains until those client and edge entry changes are tested.

## Authorization and deployment decisions still required

- Whether Windows users have the platform `config:publish` entitlement and
  how direct/offline sites authenticate to that platform; the device password
  alone is insufficient. No new self-approval or remote role is assumed.
- Broker per-machine publisher/subscriber ACL, non-retained delivery and
  historical retained-message removal must be verified as infrastructure,
  not inferred from topic names. This proposal changes no Broker settings.
- Ordinary transactions preserve pinned identity, network, TLS and OTA trust.
  Any rotation of those values requires a separately defined, authorized
  recovery/rollover contract and board-specific capability decision; existing
  `config:approve` is not silently upgraded to arbitrary trust rotation.
- Factory package directory metadata, installed directory remediation,
  platform/Windows paired releases, board builds, two-board install/recovery,
  late-result behavior and production authorization tests remain open.

Required next validation: signed artifact tamper/type/parent/write-set
negatives; strict platform review/publish separation; actual ingress-to-DB-
worker/API orderings; edge duplicate/restart/retained/uncertain handoff; v2
config add/delete/restore and failure recovery; Windows original-ID query and
UNKNOWN display. None of those are established by the local directory tests.
