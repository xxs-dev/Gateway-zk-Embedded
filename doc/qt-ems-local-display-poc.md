# Qt EMS Local Display PoC

## Purpose

`LocalDisplayQtEms` is a read-only Qt Widgets proof of concept for migrating the
legacy KY-EMS screen into the current gateway runtime.

It does not use the legacy `QRamRT` / `VarList.xml` shared memory path. Values
are read through the current `PointStoreRouter` and `MemoryPointStore`, so the
display follows the same global point indexes, point metadata, quality flags,
and shared memory segments used by MQTT, realtime monitor, EMS logic, and
writeback services.

## Current Scope

- Native Qt Widgets window.
- Fullscreen by default, `--windowed` for desktop testing.
- Read-only realtime snapshot refresh.
- Tabs:
  - 首页
  - 运行监测
  - PCS
  - BMS
  - 全部点位
- Search by index, device name, meter code, point code, point name, or category.
- Shows value, quality, timestamp, unit, and whether the point is writable.

This PoC intentionally does not submit control commands yet. Later write support
must use `PointStoreRouter::submitWriteCommand()` and the existing writeback
result path, not direct shared memory writes.

## Build

The target is conditionally enabled from `CMakeLists.txt`. If Qt Widgets is not
found, the normal edge build continues and prints:

```text
Qt Widgets not found; LocalDisplayQtEms target disabled
```

When Qt is available:

```bash
cmake --build build-aarch64 --target LocalDisplayQtEms
```

For desktop smoke testing with a local Qt toolchain:

```bash
cmake -S . -B build-local-qt
cmake --build build-local-qt --target LocalDisplayQtEms
```

## Run

Windowed:

```bash
./LocalDisplayQtEms --app-config config/examples/local-display-qt-ems-example.json --windowed
```

Fullscreen:

```bash
./LocalDisplayQtEms --app-config /opt/modbus-gateway/config/runtime/apps/mqtt-service.json
```

Optional arguments:

```text
--refresh-ms 1000
--max-points 500
```

## Migration Notes

Legacy KY-EMS binding:

```text
KY-EMS-Config.xml widget + appDataIndex -> QRamRT::GetItemValue(appDataIndex)
```

PoC binding:

```text
App device configs -> PointDefinition.index -> PointStoreRouter::getLatestByIndex(index)
```

The next migration step is to add a display binding file that maps old
`appDataIndex` values to current global point indexes. Once that adapter exists,
the legacy `.ui` pages can be ported one page at a time while keeping the new
data source.

Recommended order:

1. Keep this PoC as the baseline point browser.
2. Port the KY-EMS homepage layout and bind labels through the new provider.
3. Port run monitor, PCS, BMS, and IO read-only pages.
4. Add historical curve reader for current `point_samples` SQLite storage.
5. Add parameter and IO writes through the existing writeback queue.
