# JSON-only SCADA capture

The former direct shared-memory helper is withdrawn. Even OpenExisting reads
could recover an owner-dead robust mutex by resetting production payloads.
The earlier successful capture does not prove absence of side effects.

This replacement takes only an externally supplied JSON file or STDIN. It has
no MemoryPointStore dependency, shared-memory mapping, HTTP client, parameter
restore, or write dispatch. Compile without memory_point_store.cpp. Keep the
existing scene/access/PCS/value-map/project-loader dependencies and Qt Widgets.
Production cross compilation stays on 22.11. No new service is needed.

```
scada_capture_readonly --project /path/to/project --samples /path/to/samples.json --output /new/output
scada_capture_readonly --project /path/to/project --samples - --output /new/output < /path/to/samples.json
```

Input contract: an object containing integer `sampleTimestampMs` (Unix ms) and
`points` array. Each point requires integer `index`, finite numeric `value`,
integer `quality`, integer `ts`/`expireAt` (Unix ms; expiry 0 means unspecified),
and boolean `stale`. Preserve API quality/time/staleness; never synthesize good
quality for missing metadata. The main task exports/normalizes diagnostics API
responses into this format; this helper does not contact that API itself.
Duplicate indices and missing/invalid fields fail closed. Extra valid indices
are ignored; missing project indices are reported and rendered as unavailable.

All pages use one frozen input timestamp. PNGs are offscreen scene renders,
not foreground screenshots, and contain no prior trend history. Exit codes:
0 complete, 2 missing samples or attempted writes, 1 invalid input/render failure.
The report includes sample-byte SHA256 and `liveDataAccess=false`.

Native tests: `run_scada_capture_json_test.sh EXISTING_QT_OBJECT_DIR NEW_OUTPUT PROJECT`.
The fixture is deliberately incomplete synthetic data, not device evidence.
