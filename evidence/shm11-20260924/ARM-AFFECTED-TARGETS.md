# Point-store ABI11 coordinated rebuild inventory

Source inventory only, not an ARM build/deployment result. Names below are actual
CMake targets in this candidate, including non-EMS readers. The shared static
edge_gateway library and all deployed users must come from one verified revision.

| Target | Point-store involvement / entry point |
| --- | --- |
| ModbusRtu | main.cpp, MemoryPointStore + GatewayDaemon |
| Dlt645Driver | dlt645_main.cpp, MemoryPointStore + GatewayDaemon |
| DioDriver | dio_driver_main.cpp, MemoryPointStore + GatewayDaemon |
| CanDriver | can_driver_main.cpp, MemoryPointStore + CanDriverService |
| IecDriver | iec_driver_main.cpp, MemoryPointStore + GatewayDaemon |
| MqttDriver | mqtt_driver_main.cpp, stores + router |
| MqttForwarder | mqtt_forwarder_main.cpp, OpenExisting stores + router |
| EventEngine | event_engine_main.cpp, point-store readers + router |
| ComputeEngine | compute_engine_main.cpp, stores + router + Graph |
| AgcAvcController | agc_avc_main.cpp, stores + router |
| EmsClusterCoordinator | ems_cluster_main.cpp, EmsClusterPointBridge |
| SystemMonitor | system_monitor_main.cpp, stores + runtime router |
| LocalDisplay | local_display_main.cpp, stores + router |
| QtDisplayBridge | qt_display_bridge_main.cpp, stores + router |
| LocalDisplayQtEms | local_display_qt_ems_main.cpp, stores + router; Qt5/Qt6 conditional target |
| CameraService | camera_service_main.cpp, status point store |
| pointctl | tools/pointctl.cpp, stores + router + direct store operations |
| EmsParityCheck | tools/ems_parity_check.cpp, live stores plus baseline/candidate stores |
| stress_runner | tools/stress_runner.cpp, stores + router; rebuild before using against new ABI |
| memory_point_store_migrate | src/memory_point_store_migration.cpp, directly compiled layout/copy code |

There are 20 named targets, including the migration and diagnostic tools. In
particular EmsParityCheck is not a JSON-only tool: main opens live MemoryPointStore
instances and creates baseline/candidate stores. It must not retain an ABI10 build.

Qt packaging has different artifact names: CMake target LocalDisplayQtEms uses
local_display_qt_ems_main.cpp. tools/build_scada_qt_aarch64.sh compiles that entry
point against libedge_gateway.a but emits KY-SCADA; packaging tests also refer to
ky-ems/KY-EMS. The final release manifest must verify the actually selected Qt
artifact and its SHA256; do not assume renaming or copying old KY-EMS is a rebuild.
local_display_qt_main.cpp exists but is not a target in the current CMakeLists.

tools/build_edge_aarch64.sh PRODUCTION_TARGETS lists 18 of the above. The Qt target
and memory_point_store_migrate need explicit inclusion in the coordinated build
and packaging workflow. Any additional site-built point-store helper must be
inventoried before offline migration. Point-store tests must also be freshly built
when used for target-architecture acceptance; old test binaries are not valid probes.

EventStore has a distinct event IPC ABI. Its runtime links edge_gateway but its
entry/runtime sources do not map MemoryPointStore; do not migrate its event segment
merely because the name contains Store. EventEngine/MQTT point-store participants
above still require rebuilding even when event IPC remains unchanged.

Native static_asserts in src/memory_point_store_layout.hpp currently establish:
authorization record 120 bytes, pending command slot 264 bytes, atomic authority
snapshot 1240 bytes. ARM assertions, sizeof/offsetof receipts and tests are still
required. All incompatible existing nonempty point-store mappings must be rejected
without resizing/clearing. Offline v10 copy creates a new exclusive v11 segment
with verified backup; configuration switching is a separate authorized operation.

Do not deploy only Coordinator and Compute over old ABI10 drivers/readers. Shadow
testing may start those two only in independent namespaces/stores; final production
replacement and offline migration must cover every active point-store participant.
