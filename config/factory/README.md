# Edge Gateway Factory Configuration

This directory contains factory default runtime configuration for an edge gateway.

Put the complete factory bundle under `/home/gateway-factory` on the device, then run `/home/gateway-factory/deploy/install-factory-config.sh` or `/opt/modbus-gateway/bin/install-factory-config.sh`. The script copies the bundle into `/opt/modbus-gateway` and keeps the default source separate from the runtime installation.

If `/opt/modbus-gateway/config/runtime/device_identity.json` already exists, initialization preserves its `machineCode` and writes that value back after copying factory defaults. App `clientId` values are also synchronized to the preserved `machineCode`.

Copy `runtime` to `/opt/modbus-gateway/config/runtime` on the device, then adjust:

- `runtime/device_identity.json`: unique `machineCode`, device `imei`, serial number, model and version fields.
- MQTT broker/auth fields in `runtime/apps/mqtt-service.json` and `runtime/apps/monitor-service.json`.
- Serial port files under `runtime/devices/` according to the actual wiring.

Only `ttySP1` and `ttySP2` sample Modbus RTU device files are enabled by default. Add or remove files under `runtime/devices/` to control which protocol drivers are started.

`runtime/apps/monitor-service.json` is part of the factory initialization set. When `gateway-services.service` starts, `gateway-services.sh` will start `system-monitor@monitor-service.service` from this file so the platform can pull configuration, view system resources and run allowed diagnostics after first boot.

MQTT topics in app configs are base topics only. At runtime the gateway publishes and subscribes with a `/<machineCode>` suffix, for example `edge/telemetry/GW0001` and `edge/ota/request/GW0001`.

Do not put `machineCode` in serial/protocol device files. Runtime code injects it from `device_identity.json`.
