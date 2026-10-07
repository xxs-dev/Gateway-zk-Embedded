# Package-Bound Generic Qt Resource

## Contract

The business factory packager now copies `config/generic-scada-widgets.json`
and binds its SHA256 and byte length in `edge-package-manifest.json.resources`.
The three prototypes are the product Devices screen's first qtLabel, qtValue
and qtButton, projected with the existing generic builder's stripping rules.
The resource records the source path and canonical-LF source SHA256. Site tags,
state rules and actions are removed; business factory points/configs are not.

For selected Qt initialization, the generic package preflight requires the
resource's generic file hash, edge resource binding and usable prototype
structure before the child installer is invoked. A leftover home file cannot
satisfy this check. Qt enablement is exported to that installer, which rechecks
the extracted package and atomically installs its exact bytes. Qt-disabled
preflight remains supported. No default Qt enablement is introduced.

This is a resource contract, not a signature/authentication or race-hardening
claim for hostile replacement of an extracted package. The package's existing
ownership/extraction and approval boundaries remain required.

## Local Evidence

Private capture: `ota-m3-generic-qt-resource-20261007-attempt-01`.

- red-01: old actual candidate, one FAIL because missing resource was accepted.
- green-01: same selector/candidate, one PASS after the preflight change.
- green-02: five PASS; exact product projection, preflight rejection, package
  copy CLI, synthetic 18-point inventory/routes/two screens, duplicate index
  and missing point identity rejection, and Qt-disabled preservation.
- edge-followup-01: two PASS, executing the actual packager's embedded manifest
  program on retained candidate bytes and rejecting a resource changed between
  preflight and install. No ELF is executed by these tests.

The tests use synthetic machine/meter/store identities. They do not establish
physical data acquisition, systemd ownership, renderer behavior or final EMS UI.
PointsN is the existing preparation view, not a completed business engineering
screen. No legacy PcsMon/SysInfo resource is changed.

## Packaging Boundary

Windows Python lacks O_NOFOLLOW and O_NONBLOCK used by the official POSIX
credential-staging helper. The full factory shell packager was not run here;
no helper was mocked, bypassed or weakened. No new actual business TAR/pair is
claimed. New official Linux packaging and normalization of its output remain
required. The Windows normalizer separately supports this one bound resource
in addition to the original 42 business JSON files, without relaxing their
count or changing original member bytes/modes.

The next packaging scope must use this fixed source and the original PRODUCT03
20 ELF bytes, including the separate AGC component. No compilation is needed
for these script/resource changes. Use a new private source/output tree, bind
all inputs and output members, and preserve original failures. Do not replace
formal Windows Assets or allowlists. No old execution receipt may be replayed.

LOCAL_ONLY evidence is not deployment authorization. Linux execution,
Qt environment/renderer qualification, complete business project, three-end
release and two-board acceptance remain OPEN. Production approval remains NO.
