# Limited Windows / KECP2 compatibility review

Windows: D:/workspace/GatewayStudio-realese1.0,
0d0012f7add27a7ca475fcabc8ab4c41d1d8390c (clean before/after).
Edge reference: c8a31d64f9eb7da4a4c2ba452fe07bef3c5ee830.
Read-only source inspection only: no product edits, build, UI execution, device
connection, install, OTA execution or release packaging. Commands: git status,
git rev-parse/show, scoped rg, Get-Content. Only this evidence file is authored.

## Result

No concrete field-loss or automatic-voter-bootstrap upgrade defect was found in
the examined paths. **No Windows change is required to preserve the new
emsCluster.controlTargetIndexes or other untouched unknown emsCluster fields.**
This is semantic JSON preservation, not byte-for-byte whitespace/order retention.
It does not promise preservation of values explicitly replaced by a user or an
intentional whole-document template replacement.

## Source evidence (Windows-relative paths)

- src/GatewayDesktop.Next/GatewayDesktop.Next.csproj:32 includes the shared UI
  ViewModels; this review is of the actual GatewayStudio implementation.
- src/GatewayDesktop.UI/ViewModels/EmsStrategyViewModel.cs:3620 parses the full
  existing JsonObject, applies a targeted edit and serializes that same root at
  :3630. ApplyItem :3814 edits compute/rule/script fields, not an EMS app DTO.
  SyncGraphProfileToAppConfigs :2012 also retains the original root.
- src/GatewayDesktop.UI/ViewModels/ServiceConfigViewModel.cs:564 and
  src/GatewayDesktop.UI/ViewModels/MqttConfigViewModel.cs:400 likewise parse the
  existing root and change one JSON path, preserving unrelated unknown fields.
- src/GatewayDesktop.UI/ViewModels/ConfigProductionViewModel.cs:534 reuses the
  existing app root when generating device references; it does not rebuild the
  app through a schema that omits emsCluster.
- src/GatewayDesktop.UI/Infrastructure/ConfigWorkspace.cs:154 stores the complete
  content string; :440 returns it in ConfigFileEntry. ConfigWorkspaceEditSession.cs:
  71 rejects stale page revisions instead of overwriting newer content.
- src/GatewayDesktop.Services/Device/DeviceConfigService.cs:95 preserves pulled
  file content; :140 sends f.Content unchanged in ConfigApplyFile. Direct/MQTT
  transports serialize the outer request, not the embedded app config as a DTO.
- src/GatewayDesktop.UI/ViewModels/ConfigOtaViewModel.cs:246 selects workspace
  files, :325 writes file.Content directly into the package. No EMS-field whitelist.

## Voter / upgrade boundary

No EMS voter-discovery, roster-generation or membership-reset assumption was found
in the scoped Windows EMS editors/OTA UI and service code. OtaService.cs:69 sends
a generic artifact request; it does not generate cluster membership. Bundled
gateway-services.sh:455/:470 selects the coordinator service from runtimeMode and
emsCluster.enabled, without manufacturing a roster or reducing quorum.

Edge c8a31d6 src/ems_cluster.cpp:947 requires a complete pre-provisioned fixed
membership and fails construction if absent/inconsistent. Identical offline roster,
epoch and coordinated KECP2 peer upgrade remain deployment preconditions; Windows
does not validate or provision them automatically. This is not an assertion that a
single-peer live upgrade is supported. Missing final release packaging or a dedicated
roster UI is not reported as a bug. No ARM/release/physical acceptance is implied.
