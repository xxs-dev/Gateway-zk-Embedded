cmake_minimum_required(VERSION 3.20)
project(ModbusRtu)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_COLOR_DIAGNOSTICS OFF)

add_library(edge_gateway
    src/alarm_service.cpp
    src/can_driver_service.cpp
    src/can_signal_codec.cpp
    src/collector.cpp
    src/command_executor.cpp
    src/compute_engine_service.cpp
    src/config_loader.cpp
    src/dlt645_client.cpp
    src/dlt645_codec.cpp
    src/dlt645_standard_points_loader.cpp
    src/event_engine_service.cpp
    src/gateway_daemon.cpp
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
    src/modbus_rtu_client.cpp
    src/modbus_tcp_client.cpp
    src/modbus_codec.cpp
    src/posix_serial_port.cpp
    src/point_store_router.cpp
    src/read_task_planner.cpp
    src/sqlite_sample_writer.cpp
    src/sqlite_alarm_writer.cpp
    src/sysfs_gpio_port.cpp
    src/system_monitor_service.cpp
    src/writeback_service.cpp
)

target_include_directories(edge_gateway PUBLIC include)
target_compile_options(edge_gateway PRIVATE -fdiagnostics-color=never)
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

add_executable(SystemMonitor system_monitor_main.cpp)
target_link_libraries(SystemMonitor PRIVATE edge_gateway)
target_compile_options(SystemMonitor PRIVATE -fdiagnostics-color=never)

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

add_executable(legacy_ems_test tools/legacy_ems_test.cpp)
target_link_libraries(legacy_ems_test PRIVATE edge_gateway)
target_compile_options(legacy_ems_test PRIVATE -fdiagnostics-color=never)

add_executable(ota_service_test tools/ota_service_test.cpp)
target_link_libraries(ota_service_test PRIVATE edge_gateway)
target_compile_options(ota_service_test PRIVATE -fdiagnostics-color=never)

add_executable(system_monitor_service_test tools/system_monitor_service_test.cpp)
target_link_libraries(system_monitor_service_test PRIVATE edge_gateway)
target_compile_options(system_monitor_service_test PRIVATE -fdiagnostics-color=never)
