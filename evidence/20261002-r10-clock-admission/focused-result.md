# R10 diagnostic clock admission follow-up

Local source/test checkpoint only. No device clock was changed, no service was
restarted, and no Broker/device/ARM verification was performed.

`diag-request` and `diag-replied` events now use the same complete JSON text
escaping as diagnostic replies. The actual service-loop test submits a JSON
escaped control character in `cmdId`, parses both published status events and
verifies the decoded ID. The test also drives a fresh service instance at the
exact +5 minute boundary, avoiding dependence on milliseconds consumed by a
previous completed request. A stale or future new ID produces a fingerprinted
`admission_rejection` without terminal `cmdId` or cleanup claim. Existing
cached terminals still replay before the age gate. The private WSL
`system_monitor_diagnostic_test` passed (tool call `afed65`, exit 0); the first
boundary draft failed (`615ed6`, exit 1) because it reused an advancing clock
anchor, and was corrected without widening the product window.

Operational precondition: Windows/platform and edge clocks must be within the
configured five-minute past-age and future-skew limits before diagnostic
delivery. The effective-time floor uses a monotonic steady anchor. After a
large wall-clock forward step followed by correction, its projection remains
high and keeps advancing with steady time; simply waiting for ordinary wall
time to catch up is **not** a reliable recovery procedure. Stop new delivery,
review original IDs and any unknown execution, correct time through an
authorized operations procedure, and only then use an explicitly reviewed
restart/recovery plan. Restart alone drops the in-memory terminal cache and
may allow an unconfirmed ID to execute on redelivery. This source behavior is
not evidence of actual board clock handling or cross-restart exactly-once.

Edge producer must be deployed before Windows clients rely on these new reason
tokens. Mixed-version in-flight requests remain UNKNOWN until a trusted
terminal is observed. Platform ingress continues to discard admission-only
messages; a platform observation deadline is not a device timeout. The formal
configured-role/config provision journal and full business acceptance remain
separate, open work.
