# R3-edge local result (2026-09-30)

Branch: `fix/edge-review-r1-20260930`, after R4 commit `d5e391c`. Local isolated WSL x86_64 only; no real network, target board, or Windows build.

- `cmake --build /var/tmp/edge-review-r1-20260930-build --target ota_service_test system_monitor_direct_maintenance_contract_test -j4`: native exit 0.
- Isolated `ota_service_test`: native exit 0. Fake PATH shows secure HTTPS with only wget fails without invoking wget, fake curl receives `--proto-redir =https`, and explicit `allowInsecureHttp=true` permits wget fallback.
- Isolated `system_monitor_direct_maintenance_contract_test`: native exit 0. Real HTTP handler rejects absent signature and missing signed jobId, accepts a supplied signature with unchanged explicit jobId, then local asynchronous verification fails against the deliberately absent test public key.
- `git diff --check`: exit 0.

Secure OTA HTTPS now requires curl on the device. The HTTP handler test proves field forwarding and request validation, not a valid platform/Windows signature or end-to-end OTA deployment. No private key or external URL was used.
