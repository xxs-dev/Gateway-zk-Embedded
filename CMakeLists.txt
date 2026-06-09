cmake_minimum_required(VERSION 3.20)
project(ModbusRtu)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_COLOR_DIAGNOSTICS OFF)

option(GATEWAY_ENABLE_SIZE_OPTIMIZATIONS "Enable function/data section GC for smaller edge binaries" ON)

add_library(edge_gateway
    src/alarm_service.cpp
    src/can_driver_service.cpp
    src/can_signal_codec.cpp
    src/collector.cpp
    src/common/collector_base.cpp
    src/command_executor.cpp
    src/compute_engine_service.cpp
    src/config_loader.cpp
    src/dio_command_executor.cpp
    src/dio_collector.cpp
    src/dlt645_client.cpp
    src/dlt645_collector.cpp
    src/dlt645_codec.cpp
    src/dlt645_standard_points_loader.cpp
    src/event_engine_service.cpp
    src/common/gateway_daemon.cpp
    src/common/gateway_daemon_modbus.cpp
    src/graph_ems_engine.cpp
    src/legacy_ems_engine.cpp
    src/legacy_ems_point_catalog.cpp
    src/memory_point_store.cpp
    src/mock_serial_port.cpp
    src/ota_service.cpp
    src/builtin_mqtt_driver_publisher.cpp
    src/mqtt_event_outbox.cpp
    src/mqtt_driver_service.cpp
    src/mqtt_realtime_ring_buffer.cpp
    src/modbus_northbound_server.cpp
    src/modbus_rtu_client.cpp
    src/modbus_tcp_client.cpp
    src/modbus_codec.cpp
    src/posix_serial_port.cpp
    src/point_store_router.cpp
    src/priority_control_lease.cpp
    src/read_task_planner.cpp
    src/sqlite_sample_writer.cpp
    src/sqlite_alarm_writer.cpp
    src/sysfs_gpio_port.cpp
    src/system_monitor_service.cpp
    src/writeback_service.cpp
)

target_include_directories(edge_gateway PUBLIC include)
target_compile_options(edge_gateway PRIVATE -fdiagnostics-color=never)
if(GATEWAY_ENABLE_SIZE_OPTIMIZATIONS AND NOT MSVC)
    target_compile_options(edge_gateway PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(edge_gateway INTERFACE -Wl,--gc-sections)
endif()
if(NOT WIN32)
    target_link_libraries(edge_gateway PUBLIC dl pthread rt)
endif()

add_executable(ModbusRtu main.cpp)
target_link_libraries(ModbusRtu PRIVATE edge_gateway)
target_compile_options(ModbusRtu PRIVATE -fdiagnostics-color=never)

add_executable(Dlt645Driver dlt645_main.cpp)
target_link_libraries(Dlt645Driver PRIVATE edge_gateway)
target_compile_options(Dlt645Driver PRIVATE -fdiagnostics-color=never)

add_executable(DioDriver dio_driver_main.cpp)
target_link_libraries(DioDriver PRIVATE edge_gateway)
target_compile_options(DioDriver PRIVATE -fdiagnostics-color=never)

add_executable(CanDriver can_driver_main.cpp)
target_link_libraries(CanDriver PRIVATE edge_gateway)
target_compile_options(CanDriver PRIVATE -fdiagnostics-color=never)

add_executable(pointctl tools/pointctl.cpp)
target_link_libraries(pointctl PRIVATE edge_gateway)
target_compile_options(pointctl PRIVATE -fdiagnostics-color=never)

add_executable(MqttDriver mqtt_driver_main.cpp)
target_link_libraries(MqttDriver PRIVATE edge_gateway)
target_compile_options(MqttDriver PRIVATE -fdiagnostics-color=never)

add_executable(EventEngine event_engine_main.cpp)
target_link_libraries(EventEngine PRIVATE edge_gateway)
target_compile_options(EventEngine PRIVATE -fdiagnostics-color=never)

add_executable(ComputeEngine compute_engine_main.cpp)
target_link_libraries(ComputeEngine PRIVATE edge_gateway)
target_compile_options(ComputeEngine PRIVATE -fdiagnostics-color=never)

add_executable(SystemMonitor system_monitor_main.cpp direct_agent_main.cpp)
target_link_libraries(SystemMonitor PRIVATE edge_gateway)
target_compile_definitions(SystemMonitor PRIVATE EDGE_GATEWAY_DIRECT_AGENT_EMBEDDED)
target_compile_options(SystemMonitor PRIVATE -fdiagnostics-color=never)

add_executable(DirectAgent direct_agent_main.cpp)
target_link_libraries(DirectAgent PRIVATE edge_gateway)
target_compile_options(DirectAgent PRIVATE -fdiagnostics-color=never)

add_executable(LocalDisplay local_display_main.cpp)
target_link_libraries(LocalDisplay PRIVATE edge_gateway)
target_compile_options(LocalDisplay PRIVATE -fdiagnostics-color=never)

add_executable(CameraService camera_service_main.cpp)
target_link_libraries(CameraService PRIVATE edge_gateway)
target_compile_options(CameraService PRIVATE -fdiagnostics-color=never)

add_executable(stress_runner tools/stress_runner.cpp)
target_link_libraries(stress_runner PRIVATE edge_gateway)
target_compile_options(stress_runner PRIVATE -fdiagnostics-color=never)

add_executable(dio_smoke_test tools/dio_smoke_test.cpp)
target_link_libraries(dio_smoke_test PRIVATE edge_gateway)
target_compile_options(dio_smoke_test PRIVATE -fdiagnostics-color=never)

add_executable(can_signal_codec_test tools/can_signal_codec_test.cpp)
target_link_libraries(can_signal_codec_test PRIVATE edge_gateway)
target_compile_options(can_signal_codec_test PRIVATE -fdiagnostics-color=never)

add_executable(config_loader_test tools/config_loader_test.cpp)
target_link_libraries(config_loader_test PRIVATE edge_gateway)
target_compile_options(config_loader_test PRIVATE -fdiagnostics-color=never)

add_executable(collector_partial_failure_test tools/collector_partial_failure_test.cpp)
target_link_libraries(collector_partial_failure_test PRIVATE edge_gateway)
target_compile_options(collector_partial_failure_test PRIVATE -fdiagnostics-color=never)

add_executable(gateway_daemon_realtime_priority_test tools/gateway_daemon_realtime_priority_test.cpp)
target_link_libraries(gateway_daemon_realtime_priority_test PRIVATE edge_gateway)
target_compile_options(gateway_daemon_realtime_priority_test PRIVATE -fdiagnostics-color=never)

add_executable(priority_control_lease_test tools/priority_control_lease_test.cpp)
target_link_libraries(priority_control_lease_test PRIVATE edge_gateway)
target_compile_options(priority_control_lease_test PRIVATE -fdiagnostics-color=never)

add_executable(modbus_rtu_client_resync_test tools/modbus_rtu_client_resync_test.cpp)
target_link_libraries(modbus_rtu_client_resync_test PRIVATE edge_gateway)
target_compile_options(modbus_rtu_client_resync_test PRIVATE -fdiagnostics-color=never)

add_executable(dlt645_client_interval_test tools/dlt645_client_interval_test.cpp)
target_link_libraries(dlt645_client_interval_test PRIVATE edge_gateway)
target_compile_options(dlt645_client_interval_test PRIVATE -fdiagnostics-color=never)

add_executable(modbus_tcp_client_limit_test tools/modbus_tcp_client_limit_test.cpp)
target_link_libraries(modbus_tcp_client_limit_test PRIVATE edge_gateway)
target_compile_options(modbus_tcp_client_limit_test PRIVATE -fdiagnostics-color=never)

add_executable(modbus_northbound_server_test tools/modbus_northbound_server_test.cpp)
target_link_libraries(modbus_northbound_server_test PRIVATE edge_gateway)
target_compile_options(modbus_northbound_server_test PRIVATE -fdiagnostics-color=never)

add_executable(memory_point_store_lifecycle_test tools/memory_point_store_lifecycle_test.cpp)
target_link_libraries(memory_point_store_lifecycle_test PRIVATE edge_gateway)
target_compile_options(memory_point_store_lifecycle_test PRIVATE -fdiagnostics-color=never)

add_executable(builtin_mqtt_driver_publisher_test tools/builtin_mqtt_driver_publisher_test.cpp)
target_link_libraries(builtin_mqtt_driver_publisher_test PRIVATE edge_gateway)
target_compile_options(builtin_mqtt_driver_publisher_test PRIVATE -fdiagnostics-color=never)

add_executable(mqtt_driver_service_test tools/mqtt_driver_service_test.cpp)
target_link_libraries(mqtt_driver_service_test PRIVATE edge_gateway)
target_compile_options(mqtt_driver_service_test PRIVATE -fdiagnostics-color=never)

add_executable(legacy_ems_test tools/legacy_ems_test.cpp)
target_link_libraries(legacy_ems_test PRIVATE edge_gateway)
target_compile_options(legacy_ems_test PRIVATE -fdiagnostics-color=never)

add_executable(ota_service_test tools/ota_service_test.cpp)
target_link_libraries(ota_service_test PRIVATE edge_gateway)
target_compile_options(ota_service_test PRIVATE -fdiagnostics-color=never)

add_executable(system_monitor_service_test tools/system_monitor_service_test.cpp)
target_link_libraries(system_monitor_service_test PRIVATE edge_gateway)
target_compile_options(system_monitor_service_test PRIVATE -fdiagnostics-color=never)
