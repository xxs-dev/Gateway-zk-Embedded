# First Runtime Preparation And Activation

`runtime-bootstrap.py` prepares a first runtime after a stopped whole-image
reprovision. It is **not an activation command**. It does not start or enable
services, replace live configuration/units, remove STOP/fences, promote a
production baseline, or delete/reinitialize existing shared memory.

## Prerequisites

- Run as root on Allwinner AArch64 with the existing offline lifecycle lock.
- Retain the exact `INSTALLED_STOPPED` reprovision receipt, installed image and
  units, previous image, STOP and manager-loaded fences. All participants must
  be stopped with no remaining business process.
- Use one reviewed, root-owned, nonwritable-to-others helper bundle containing
  `runtime-bootstrap.py`, `gateway-reprovision.py`, `offline-runtime-upgrade.py`,
  `runtime-upgrade-guard.py` and `production_role_policy.py`. Recovery requires
  the same helper pins. Never substitute newer helpers into a historical
  recovery bundle.
- The installed `/opt/modbus-gateway/bin/pointctl` must be a pinned AArch64 ELF
  supporting `create-empty --shm <new-name>`. Old binaries are rejected without
  executing an unknown command. Do not patch a reprovisioned image in place:
  deploy a new reviewed package through its own stopped transaction first.
- Select signed OTA trust, or the explicit offline policy below. Signed OTA
  requires the approved public key at `/etc/modbus-gateway/ota.pub` and approved
  HTTPS artifact origins. Fixture keys/origins are never production trust.
- Keep the candidate config outside both `/opt/modbus-gateway` and
  `/opt/gateway-runtime-bootstrap`. Identity, package manifest, network and TLS
  bytes/pins must match the installed configuration.
- Use a reviewed `board-simulator-profile-v1`. Physical outputs are not
  authorized. Native Qt must explicitly bind its protected monitor SHM name.

## Inputs

All JSON is root-owned, single-link, canonical and nonwritable-to-others.

| Input | Required Fields |
| --- | --- |
| Authority | `machineCode`, `identitySha256`, `networkSha256`, `tlsSha256`, `otaPublicKeySha256`, `monitorShmName`; same protected-authority schema as `production_role_policy.py` |
| Profile | `format: board-simulator-profile-v1`, `machineCode`, `ramMiB` (2048 or 4096), `capabilities`, `simulatorBindings`, `allowedUnits`, `segmentNames` |
| Trust | `requireSignature: true`, `checksumRequired: true`, `allowInsecureHttp: false`, `publicKeySha256`, nonempty distinct `httpsOrigins` |

`allowedUnits` must match the resolved role order exactly. `segmentNames` is the
sorted unique set of all resolved role SHM names, with 1-128 entries. Names must
be fresh, never reused from existing host objects. Config may contain the pinned
`edge-package-manifest.json`; it is metadata, not an additional service config.

## Offline Local Runtime

Local startup, boot recovery and supervised operation do not require OTA to be
enabled. A reviewed v2 first-runtime approval may explicitly select:

```json
{
  "otaEnabled": false,
  "requireSignature": true,
  "checksumRequired": true,
  "allowInsecureHttp": false,
  "publicKeySha256": null,
  "httpsOrigins": []
}
```

Its protected authority must have `otaPublicKeySha256: null`. Every selected app
JSON must explicitly set `ota.enabled: false` and `mqtt.otaRequestTopic: ""`,
including Monitor and other app entrypoints. Native defaults enable OTA and its
request subscription, so omission is rejected. No public key is created, read,
removed or substituted in this mode; placing a key later does not enable OTA.

The same file/config/template pins, device identity, role resolution, SHM ABI,
startup fences, separate activation approval, health promotion and reboot proof
remain mandatory. The OTA handoff owner rejects this policy before reading or
staging an artifact. This is not unsigned OTA. Existing signed policies retain
their key requirement and never fall back to offline mode on missing trust.
Legacy v1 baselines cannot select this new mode. Ordinary runtime upgrades must
preserve trust unchanged; enabling OTA requires a separate reviewed trust
provisioning procedure, not editing a live app JSON or baseline in place.

This policy does not authorize physical controls, host-network/device access,
power-loss testing, or visual acceptance. The simulator profile and deployment
isolation must still be reviewed before real services are activated.

## CLI Sequence

The following names are placeholders for reviewed inputs, not device-ready
configuration. Use the absolute path to the paired helper bundle. Review the
generated approval and pin its SHA256 before preparation. Expiry is a Unix
timestamp no more than one day ahead; preparation is bound to the current boot.

```sh
python3 -B /reviewed/bundle/deploy/runtime-bootstrap.py plan \
  --config /reviewed/config \
  --creator /opt/modbus-gateway/bin/pointctl \
  --authority /reviewed/authority.json \
  --profile /reviewed/profile.json \
  --trust /reviewed/trust.json \
  --transaction REVIEWED_TX --expires-at REVIEWED_UNIX_EXPIRY \
  --output /reviewed/prepare-approval.json

python3 -B /reviewed/bundle/deploy/runtime-bootstrap.py prepare \
  --approval /reviewed/prepare-approval.json \
  --approval-sha256 REVIEWED_APPROVAL_SHA256

python3 -B /reviewed/bundle/deploy/runtime-bootstrap.py recover \
  --transaction REVIEWED_TX --approval-sha256 REVIEWED_APPROVAL_SHA256
```

## State And Recovery

The private journal is `/opt/gateway-runtime-bootstrap`, mode 0700. Preparation
records `INTENT`, `CREATING`, then `PREPARED_STOPPED`. Its external transaction
contains the approval, copied config, progress and `ready.json`; the final
`current.json` pins that ready receipt. New approval/pointer/receipt publication
uses fsynced staging plus atomic no-replace rename. Progress replacement is
atomic. Partial staging files are retained, never treated as valid records.

Preparation validates the visible `/dev/shm` mount ID as tmpfs, checks tmpfs and
memory headroom, then calls native exclusive creation. Each completed empty v11
store is bound by name, device, inode and whole-file SHA256. Native zero stats,
ownership, permissions, ABI and absence of mappings/open descriptors are checked.
Origin, helper/trust pins, stopped status, boot, expiry and earlier objects are
rechecked before publishing the receipt. Retained host SHM metadata is unchanged.

The prepared receipt explicitly says `activation: not-authorized`. There is no
command in this module that converts it into a production startup permission.

Recovery validates its paired authority and pointer ownership, records
`ABORTED_STOPPED`, and removes only matching bootstrap pointers. It retains all
config/evidence and all new/partial SHM. Recovery is repeatable and does not
require an unexpired approval or unchanged test trust, but the stopped origin
and paired helper pins must still match.

An interruption after durable approval but before the first state checkpoint
can be aborted only when no bootstrap pointer or target SHM exists. If the
approval itself was never published, there is no recoverable authority: leave
the unused transaction/staging evidence intact and use a new transaction ID.
Never reuse a consumed ID or orphaned SHM name. Do not delete objects manually
to make preparation pass. A preparation abandoned across reboot cannot be
activated using its old boot-bound empty-store evidence.

## Validation Boundary

`tools/runtime_bootstrap_test.py` uses private root-owned fixture paths and a
mock service manager. Ordinary cases substitute the native constructor; its
opt-in case uses the real AArch64 creator in a separately mounted private tmpfs
and exercises the actual kernel mount/capacity checks. The service manager,
lock proof and mapping discovery remain fixture substitutions in that case.
`tools/pointctl_create_empty_test.py` additionally verifies byte-identical refusal
of existing objects and two concurrent creators with exactly one winner.

Exception-injected preparation tests are not SIGKILL or power-loss acceptance.
The separate activation helper below adds first-baseline promotion and stopped
recovery. Installed helper availability depends on the pinned factory release.
Actual canonical startup, reboot, running upgrade/rollback and sustained
operation remain release gates.

## Separate Activation Approval

`runtime-bootstrap-activate.py` consumes a current, same-boot prepared receipt.
It requires a second reviewed approval; preparation alone never opens gates.
This is a first-install path, not legacy adoption: the baseline is genuinely
`runtime-baseline-v2`, generation one, with `parentSha256: null` and no previous
baseline or fabricated legacy activation record.

Additional prerequisites:

- The root-owned external bundle must also contain `runtime-bootstrap-activate.py`
  and `ota-health-result.py`. Its lifecycle, guard, role-policy and health helpers
  must match the installed bytes; the recovery unit must match its paired source.
- `/opt/modbus-gateway/data` and `run` may contain directories only. Existing
  runtime data or a production baseline prohibits this first-install path.
- Config, data, run, the external bootstrap journal and unit directory must
  support same-mount directory switching. No nested mounts or symlink paths.
- The prepared stores must still match their exact empty receipts. The origin,
  config, trust, helpers, stopped services, boot ID and expiry are rechecked.
- Approval expiry must not exceed the preparation expiry. The health policy
  uses the existing restricted loopback HTTP GET and bounded stability policy.
  A simulator profile is still mandatory; no physical output is authorized.

```sh
python3 -B /reviewed/bundle/deploy/runtime-bootstrap-activate.py plan \
  --prepared-transaction REVIEWED_PREPARE_TX \
  --prepared-sha256 REVIEWED_READY_SHA256 \
  --transaction REVIEWED_ACTIVATION_TX --expires-at REVIEWED_UNIX_EXPIRY \
  --release REVIEWED_RELEASE --health /reviewed/health.json \
  --output /reviewed/activation-approval.json

python3 -B /reviewed/bundle/deploy/runtime-bootstrap-activate.py activate \
  --approval /reviewed/activation-approval.json \
  --approval-sha256 REVIEWED_ACTIVATION_APPROVAL_SHA256

python3 -B /reviewed/bundle/deploy/runtime-bootstrap-activate.py recover \
  --transaction REVIEWED_ACTIVATION_TX \
  --approval-sha256 REVIEWED_ACTIVATION_APPROVAL_SHA256
```

The ready SHA is the digest of the preparation transaction's `ready.json`, not
its approval. Review the activation plan and its separate digest before running
`activate`. Keep the exact external bundle and both approvals for recovery.
Never hot-patch an installed component or an old recovery bundle to make pins
match; install a new reviewed factory candidate through stopped reprovision.

## Activation Commit And Recovery

The activation journal is `/opt/gateway-runtime-bootstrap/activations/<tx>`.
It stages config and fresh data/run directories, retaining the original
directories by rename under `old`. The new data contains the real production
approval, candidate, reviewed profile and startup sentinel. Approved production
gates and an explicit SystemMonitor SHM binding replace the known stop fences.
The original reprovision STOP marker and historical images remain intact.

Provisional service admission requires generation one, same boot, unexpired
approval, the live owner PID/start ticks and exactly one matching kernel flock
on the root-owned 0600 lock. Owner death, lock loss, expiry or a reboot cannot
reuse provisional startup permission. The existing role health and promotion
path publishes the healthy production pointer, which is the commit point.

Before that commit, explicit recovery reinstalls manager-loaded stop fences,
stops participants and restores original config/data/run directory identities.
New runtime files and data are retained under `retired`; added unit overrides
are retained under `retired-units`. No SHM is deleted, rewound or replayed,
including stores changed during a failed start. Recovery is repeatable and does
not require a still-live original owner or unexpired approval, but does require
the exact paired helper authority and ownership of the installation.

After healthy promotion, recovery only completes pending cleanup; it does not
roll back the committed generation. A newer production pointer refuses an old
activation's recovery. Subsequent upgrades use the existing production runtime
lifecycle, not this first-install helper.

Interrupted staging before switching retains a pending external transaction for
explicit recovery. An interruption before a durable initial state has no
recoverable activation authority; retain the unused evidence and use a fresh
ID. Do not manually delete evidence or SHM to recycle a transaction.

An unpromoted interrupted activation fails closed at boot and requires its
paired stopped recovery. A committed first baseline uses native boot evidence
and post-gate startup health, not old empty-store receipts or old health results.
Retain the external journal until any pending external cleanup is reconciled.

`tools/runtime_bootstrap_activation_test.py` uses private files, actual kernel
flock and real guard/lifecycle code. The manager, endpoint health and native ABI
constructor are substituted. Its SIGKILL cases test process interruption, not
power loss or storage durability. Simulated boot-ID/SHM changes are not an OS
reboot, hardware qualification, physical-output test or visual acceptance.

## Isolated Native CLI Test

`tools/runtime_bootstrap_cli_test.py` is an opt-in AArch64 integration fixture.
It runs the unmodified installer, reprovision, preparation, activation and
ordinary upgrade/recovery command lines with factory-packaged pointctl,
SystemMonitor and offscreen Qt. Process identity, executable hashes, kernel
flock, native store construction/mappings and HTTP health are real. Only the
service manager is a test double; this is not canonical systemd acceptance.

The caller provides a new root-owned 0700 `/srv/gateway-bootstrap-cli-<id>`
directory containing `factory.tar.gz`, its paired `tools.tar.gz`,
`ems2.kyscada` and an `inputs.json` SHA256 map, including `entry.py`. Run the
entry with `--root` only inside a bounded transient systemd sandbox: read-only
host filesystem except this directory, private devices, NoNewPrivileges,
restricted socket families and only SYS_ADMIN/SYS_CHROOT/NET_ADMIN/SETPCAP
setup capabilities. The entry creates private mount/network/IPC/PID namespaces
and a chroot, then drops all setup capabilities before any installation or
native execution. It refuses nonzero capabilities, missing seccomp, exposed
manager sockets, host devices or non-loopback network routes.

The fixture uses separate stores for failed and healthy first activation,
checks wrong approval hashes, stopped recovery and post-commit recovery, then
exercises a configuration promotion and a deliberately unreachable health
endpoint followed by rollback. Focused regressions run against the actual
factory-staged helpers, whose CORE test admission remains disabled. The private
installation and evidence are retained. Its generated signing key belongs only
to this chroot and must never become the device's production OTA trust. Inspect
before/after canonical installation proofs as well as the fixture result;
passing this fixture does not accept reboot, power loss, a screen or field I/O.
