cmake_minimum_required(VERSION 3.20)
project(ModbusRtu)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_COLOR_DIAGNOSTICS OFF)

option(GATEWAY_ENABLE_SIZE_OPTIMIZATIONS "Enable function/data section GC for smaller edge binaries" ON)
include(CTest)

add_library(edge_gateway
    src/agc_avc_controller.cpp
    src/agc_avc_command_mailbox.cpp
    src/agc_avc_service.cpp
    src/am5se_iec103_client.cpp
    src/alarm_service.cpp
    src/can_driver_service.cpp
    src/can_signal_codec.cpp
    src/collector.cpp
    src/common/collector_base.cpp
    src/command_executor.cpp
    src/cluster_write_authorization.cpp
    src/compute_engine_service.cpp
    src/config_loader.cpp
    src/control_dedup_store.cpp
    src/dio_command_executor.cpp
    src/dio_collector.cpp
    src/dlt645_client.cpp
    src/dlt645_command_executor.cpp
    src/dlt645_collector.cpp
    src/dlt645_codec.cpp
    src/dlt645_standard_points_loader.cpp
    src/event_engine_service.cpp
    src/event_commit_status.cpp
    src/event_store_stats.cpp
    src/ems_cluster.cpp
    src/ems_cluster_dispatch.cpp
    src/ems_cluster_network.cpp
    src/ems_cluster_points.cpp
    src/ems_cluster_transport.cpp
    src/common/gateway_daemon.cpp
    src/common/gateway_daemon_modbus.cpp
    src/flexible_phase_balance.cpp
    src/graph_ems_engine.cpp
    src/grid_friendly_dispatch.cpp
    src/iec_client.cpp
    src/iec_codec.cpp
    src/iec103_recording_command.cpp
    src/iec103_recording_transfer_service.cpp
    src/iec_command_executor.cpp
    src/iec_collector.cpp
    src/legacy_telemetry_payload.cpp
    src/legacy_ems_engine.cpp
    src/legacy_ems_point_catalog.cpp
    src/memory_point_store.cpp
    src/mock_serial_port.cpp
    src/ota_service.cpp
    src/builtin_mqtt_driver_publisher.cpp
    src/mqtt_event_outbox.cpp
    src/mqtt_event_stats.cpp
    src/mqtt_control_result_store.cpp
    src/mqtt_driver_service.cpp
    src/mqtt_forwarder_service.cpp
    src/mqtt_realtime_ring_buffer.cpp
    src/modbus_northbound_server.cpp
    src/modbus_rtu_client.cpp
    src/modbus_tcp_client.cpp
    src/modbus_codec.cpp
    src/posix_serial_port.cpp
    src/point_store_router.cpp
    src/process_file_lock.cpp
    src/power_control_ownership.cpp
    src/priority_control_lease.cpp
    src/read_task_planner.cpp
    src/scada_project_loader.cpp
    src/scada_runtime_map.cpp
    src/scada_control_lease.cpp
    src/scada_upper_computer_safety.cpp
    src/sqlite_sample_writer.cpp
    src/sqlite_alarm_writer.cpp
    src/storage_budget.cpp
    src/system_monitor_runtime_discovery.cpp
    src/system_monitor_points.cpp
    src/sysfs_gpio_port.cpp
    src/system_monitor_service.cpp
    src/timing_policy.cpp
    src/writeback_service.cpp
)

target_include_directories(edge_gateway PUBLIC include)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 8)
    target_link_libraries(edge_gateway PUBLIC stdc++fs)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(event_store_runtime src/event_store_runtime.cpp)
    target_link_libraries(event_store_runtime PUBLIC edge_gateway pthread)
    add_library(event_store_client src/event_store_client.cpp)
    target_link_libraries(event_store_client PUBLIC event_store_runtime)
    add_library(event_store_producer src/event_store_producer.cpp src/event_store_management.cpp)
    target_link_libraries(event_store_producer PUBLIC event_store_client)
    add_library(event_store_sender src/event_store_sender.cpp src/event_store_replay_factory.cpp)
    target_link_libraries(event_store_sender PUBLIC event_store_client)
    add_library(event_stats_reader src/event_stats_reader.cpp src/mqtt_event_stats_factory.cpp)
    target_link_libraries(event_stats_reader PUBLIC event_store_runtime)
    add_executable(EventStore EXCLUDE_FROM_ALL event_store_main.cpp)
    target_link_libraries(EventStore PRIVATE event_store_runtime)
    if(BUILD_TESTING)
        add_executable(event_store_test tools/event_store_test.cpp)
        target_link_libraries(event_store_test PRIVATE event_store_runtime dl)
        add_dependencies(event_store_test sqlite_outbox_failure_fixture)
        add_test(NAME event_store COMMAND event_store_test $<TARGET_FILE:sqlite_outbox_failure_fixture>)
        set_tests_properties(event_store PROPERTIES TIMEOUT 60)
        add_executable(event_store_delivery_test tools/event_store_delivery_test.cpp)
        target_link_libraries(event_store_delivery_test PRIVATE edge_gateway dl)
        add_dependencies(event_store_delivery_test sqlite_outbox_failure_fixture)
        add_test(NAME event_store_delivery COMMAND event_store_delivery_test $<TARGET_FILE:sqlite_outbox_failure_fixture>)
        set_tests_properties(event_store_delivery PROPERTIES TIMEOUT 60)
        add_executable(event_store_client_test tools/event_store_client_test.cpp)
        target_link_libraries(event_store_client_test PRIVATE event_store_client dl)
        add_test(NAME event_store_client COMMAND event_store_client_test)
        set_tests_properties(event_store_client PROPERTIES TIMEOUT 90)
        # Driven by the Python migration suite with isolated database fixtures.
        add_executable(event_store_migrate_ipc_test tools/event_store_migrate_ipc_test.cpp)
        target_link_libraries(event_store_migrate_ipc_test PRIVATE event_store_client dl)
        add_executable(event_store_producer_test tools/event_store_producer_test.cpp)
        target_link_libraries(event_store_producer_test PRIVATE event_store_producer)
        add_test(NAME event_store_producer COMMAND event_store_producer_test)
        set_tests_properties(event_store_producer PROPERTIES TIMEOUT 60)
        add_executable(event_engine_ipc_test tools/event_engine_ipc_test.cpp)
        target_link_libraries(event_engine_ipc_test PRIVATE event_store_producer)
        add_test(NAME event_engine_ipc COMMAND event_engine_ipc_test)
        set_tests_properties(event_engine_ipc PROPERTIES TIMEOUT 60)
        add_executable(event_store_sender_test tools/event_store_sender_test.cpp)
        target_link_libraries(event_store_sender_test PRIVATE event_store_sender dl)
        add_test(NAME event_store_sender COMMAND event_store_sender_test)
        set_tests_properties(event_store_sender PROPERTIES TIMEOUT 90)
        add_executable(event_store_transport_test tools/event_store_transport_test.cpp)
        target_link_libraries(event_store_transport_test PRIVATE event_store_sender event_store_producer)
        add_test(NAME event_store_transport COMMAND event_store_transport_test)
        set_tests_properties(event_store_transport PROPERTIES TIMEOUT 60)
        add_executable(event_store_management_test tools/event_store_management_test.cpp)
        target_link_libraries(event_store_management_test PRIVATE event_store_producer dl)
        add_test(NAME event_store_management COMMAND event_store_management_test)
        set_tests_properties(event_store_management PROPERTIES TIMEOUT 90)
        add_executable(event_history_projection_test tools/event_history_projection_test.cpp)
        target_link_libraries(event_history_projection_test PRIVATE edge_gateway dl)
        add_test(NAME event_history_projection COMMAND event_history_projection_test)
        set_tests_properties(event_history_projection PROPERTIES TIMEOUT 60)
        add_executable(event_store_history_runtime_test tools/event_store_history_runtime_test.cpp)
        target_link_libraries(event_store_history_runtime_test PRIVATE event_store_runtime dl)
        add_test(NAME event_store_history_runtime COMMAND event_store_history_runtime_test)
        set_tests_properties(event_store_history_runtime PROPERTIES TIMEOUT 60)
        add_executable(event_store_cpu_test tools/event_store_cpu_test.cpp)
        target_link_libraries(event_store_cpu_test PRIVATE event_store_runtime dl)
        target_link_options(event_store_cpu_test PRIVATE -Wl,--wrap=realpath)
        add_test(NAME event_store_cpu COMMAND event_store_cpu_test --check)
        set_tests_properties(event_store_cpu PROPERTIES TIMEOUT 60)
        add_executable(event_store_index_init_test tools/event_store_index_init_test.cpp)
        target_link_libraries(event_store_index_init_test PRIVATE event_store_runtime dl)
        add_test(NAME event_store_index_init COMMAND event_store_index_init_test)
        set_tests_properties(event_store_index_init PROPERTIES TIMEOUT 60)
        add_executable(event_store_pipeline_baseline_test tools/event_store_pipeline_baseline_test.cpp)
        target_link_libraries(event_store_pipeline_baseline_test PRIVATE edge_gateway)
        add_test(NAME event_store_pipeline_baseline COMMAND event_store_pipeline_baseline_test)
        set_tests_properties(event_store_pipeline_baseline PROPERTIES TIMEOUT 60)
        add_executable(event_store_capacity_test tools/event_store_capacity_test.cpp)
        target_link_libraries(event_store_capacity_test PRIVATE event_store_runtime dl)
        add_test(NAME event_store_capacity COMMAND event_store_capacity_test)
        set_tests_properties(event_store_capacity PROPERTIES TIMEOUT 60)
        add_executable(event_store_stats_test tools/event_store_stats_test.cpp)
        target_link_libraries(event_store_stats_test PRIVATE event_store_runtime)
        add_test(NAME event_store_stats COMMAND event_store_stats_test)
        set_tests_properties(event_store_stats PROPERTIES TIMEOUT 60)
        add_executable(event_stats_reader_test tools/event_stats_reader_test.cpp)
        target_link_libraries(event_stats_reader_test PRIVATE event_stats_reader)
        add_test(NAME event_stats_reader COMMAND event_stats_reader_test)
        set_tests_properties(event_stats_reader PROPERTIES TIMEOUT 90)
        add_executable(mqtt_event_stats_test tools/mqtt_event_stats_test.cpp)
        target_link_libraries(mqtt_event_stats_test PRIVATE event_stats_reader)
        add_test(NAME mqtt_event_stats COMMAND mqtt_event_stats_test)
        set_tests_properties(mqtt_event_stats PROPERTIES TIMEOUT 60)
    endif()
endif()
target_compile_options(edge_gateway PRIVATE -fdiagnostics-color=never)
if(GATEWAY_ENABLE_SIZE_OPTIMIZATIONS AND NOT MSVC)
    target_compile_options(edge_gateway PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(edge_gateway INTERFACE -Wl,--gc-sections)
endif()
if(WIN32)
    target_link_libraries(edge_gateway PUBLIC ws2_32)
else()
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

add_executable(IecDriver iec_driver_main.cpp)
target_link_libraries(IecDriver PRIVATE edge_gateway)
target_compile_options(IecDriver PRIVATE -fdiagnostics-color=never)

add_executable(pointctl tools/pointctl.cpp)
target_link_libraries(pointctl PRIVATE edge_gateway)
target_compile_options(pointctl PRIVATE -fdiagnostics-color=never)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Explicit opt-in target for the isolated shadow acceptance, never a product install.
    add_executable(ems_shadow_live_helper EXCLUDE_FROM_ALL tools/ems_shadow_live_helper.cpp)
    target_link_libraries(ems_shadow_live_helper PRIVATE edge_gateway pthread rt)
    add_executable(memory_point_store_migrate
        tools/memory_point_store_migrate.cpp src/memory_point_store_migration.cpp)
    target_link_libraries(memory_point_store_migrate PRIVATE pthread rt)
    option(GATEWAY_BUILD_EMS_SHADOW_HELPER_TESTS "Build isolated shadow helper regression" OFF)
    if(BUILD_TESTING AND GATEWAY_BUILD_EMS_SHADOW_HELPER_TESTS)
        add_executable(ems_shadow_live_helper_test tools/ems_shadow_live_helper_test.cpp)
        target_link_libraries(ems_shadow_live_helper_test PRIVATE edge_gateway pthread rt)
        add_test(NAME ems_shadow_live_helper COMMAND ems_shadow_live_helper_test)
        set_tests_properties(ems_shadow_live_helper PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77)
    endif()
    if(BUILD_TESTING)
        add_executable(memory_point_store_migration_test
            tools/memory_point_store_migration_test.cpp
            src/memory_point_store_migration.cpp src/memory_point_store.cpp)
        target_include_directories(memory_point_store_migration_test PRIVATE include)
        target_link_libraries(memory_point_store_migration_test PRIVATE edge_gateway pthread rt)
        add_dependencies(memory_point_store_migration_test memory_point_store_migrate)
        add_test(NAME memory_point_store_migration COMMAND memory_point_store_migration_test)
        set_tests_properties(memory_point_store_migration PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
        add_executable(memory_point_store_shm11_test tools/memory_point_store_shm11_test.cpp)
        target_link_libraries(memory_point_store_shm11_test PRIVATE edge_gateway)
        add_executable(memory_point_store_v11_migration_test
            tools/memory_point_store_v11_migration_test.cpp src/memory_point_store_migration.cpp)
        target_link_libraries(memory_point_store_v11_migration_test PRIVATE edge_gateway pthread rt)
        add_test(NAME memory_point_store_shm11 COMMAND memory_point_store_shm11_test)
        add_test(NAME memory_point_store_v11_migration COMMAND memory_point_store_v11_migration_test)
        set_tests_properties(memory_point_store_shm11 memory_point_store_v11_migration PROPERTIES TIMEOUT 60)
    endif()
endif()

add_executable(MqttDriver mqtt_driver_main.cpp)
target_link_libraries(MqttDriver PRIVATE edge_gateway)
target_compile_options(MqttDriver PRIVATE -fdiagnostics-color=never)

add_executable(MqttForwarder mqtt_forwarder_main.cpp)
target_link_libraries(MqttForwarder PRIVATE edge_gateway)
target_compile_options(MqttForwarder PRIVATE -fdiagnostics-color=never)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(MqttDriver PRIVATE event_stats_reader)
    target_link_libraries(MqttDriver PRIVATE event_store_producer event_store_sender)
    target_link_libraries(MqttForwarder PRIVATE event_stats_reader)
    target_link_libraries(MqttForwarder PRIVATE event_store_sender)
endif()

add_executable(EventEngine event_engine_main.cpp)
target_link_libraries(EventEngine PRIVATE edge_gateway)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(EventEngine PRIVATE event_store_producer event_stats_reader)
endif()
target_compile_options(EventEngine PRIVATE -fdiagnostics-color=never)

add_executable(ComputeEngine compute_engine_main.cpp)
target_link_libraries(ComputeEngine PRIVATE edge_gateway)
target_compile_options(ComputeEngine PRIVATE -fdiagnostics-color=never)

add_executable(AgcAvcController agc_avc_main.cpp)
target_link_libraries(AgcAvcController PRIVATE edge_gateway)
target_compile_options(AgcAvcController PRIVATE -fdiagnostics-color=never)

add_executable(EmsClusterCoordinator ems_cluster_main.cpp)
target_link_libraries(EmsClusterCoordinator PRIVATE edge_gateway)
target_compile_options(EmsClusterCoordinator PRIVATE -fdiagnostics-color=never)

add_executable(SystemMonitor system_monitor_main.cpp system_monitor_direct_maintenance.cpp)
target_link_libraries(SystemMonitor PRIVATE edge_gateway)
target_compile_options(SystemMonitor PRIVATE -fdiagnostics-color=never)

add_executable(LocalDisplay local_display_main.cpp)
target_link_libraries(LocalDisplay PRIVATE edge_gateway)
target_compile_options(LocalDisplay PRIVATE -fdiagnostics-color=never)

add_executable(QtDisplayBridge qt_display_bridge_main.cpp)
target_link_libraries(QtDisplayBridge PRIVATE edge_gateway)
target_compile_options(QtDisplayBridge PRIVATE -fdiagnostics-color=never)

find_package(Qt6 COMPONENTS Widgets QUIET)
if(Qt6Widgets_FOUND)
    add_executable(LocalDisplayQtEms
        local_display_qt_ems_main.cpp
        local_display_qt_scada_scene.cpp
        local_display_qt_access_control.cpp
        local_display_qt_pcs_power_control.cpp
        local_display_qt_value_map.cpp
    )
    target_link_libraries(LocalDisplayQtEms PRIVATE edge_gateway Qt6::Widgets)
    target_compile_options(LocalDisplayQtEms PRIVATE -fdiagnostics-color=never)
    add_executable(local_display_qt_value_map_test tools/local_display_qt_value_map_test.cpp local_display_qt_value_map.cpp)
    target_include_directories(local_display_qt_value_map_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(local_display_qt_value_map_test PRIVATE Qt6::Core)
    target_compile_options(local_display_qt_value_map_test PRIVATE -fdiagnostics-color=never)
    add_executable(local_display_qt_access_test
        tools/local_display_qt_access_test.cpp
        local_display_qt_access_control.cpp
    )
    target_include_directories(local_display_qt_access_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(local_display_qt_access_test PRIVATE Qt6::Widgets)
    target_compile_options(local_display_qt_access_test PRIVATE -fdiagnostics-color=never)
else()
    find_package(Qt5 COMPONENTS Widgets QUIET)
    if(Qt5Widgets_FOUND)
        add_executable(LocalDisplayQtEms
            local_display_qt_ems_main.cpp
            local_display_qt_scada_scene.cpp
            local_display_qt_access_control.cpp
            local_display_qt_pcs_power_control.cpp
            local_display_qt_value_map.cpp
        )
        target_link_libraries(LocalDisplayQtEms PRIVATE edge_gateway Qt5::Widgets)
        target_compile_options(LocalDisplayQtEms PRIVATE -fdiagnostics-color=never)
        add_executable(local_display_qt_value_map_test tools/local_display_qt_value_map_test.cpp local_display_qt_value_map.cpp)
        target_include_directories(local_display_qt_value_map_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(local_display_qt_value_map_test PRIVATE Qt5::Core)
        target_compile_options(local_display_qt_value_map_test PRIVATE -fdiagnostics-color=never)
        add_executable(local_display_qt_access_test
            tools/local_display_qt_access_test.cpp
            local_display_qt_access_control.cpp
        )
        target_include_directories(local_display_qt_access_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_link_libraries(local_display_qt_access_test PRIVATE Qt5::Widgets)
        target_compile_options(local_display_qt_access_test PRIVATE -fdiagnostics-color=never)
    else()
        message(STATUS "Qt Widgets not found; LocalDisplayQtEms target disabled")
    endif()
endif()

add_executable(CameraService camera_service_main.cpp)
target_link_libraries(CameraService PRIVATE edge_gateway)
target_compile_options(CameraService PRIVATE -fdiagnostics-color=never)

add_executable(stress_runner tools/stress_runner.cpp)
target_link_libraries(stress_runner PRIVATE edge_gateway)
target_compile_options(stress_runner PRIVATE -fdiagnostics-color=never)

add_library(gateway_test_lab_support
    test-lab/src/can_simulator.cpp
    test-lab/src/iec104_simulator.cpp
    test-lab/src/modbus_simulator.cpp
    test-lab/src/serial_simulator.cpp
)
target_include_directories(gateway_test_lab_support PUBLIC test-lab/include include)
target_compile_options(gateway_test_lab_support PRIVATE -fdiagnostics-color=never)
if(WIN32)
    target_link_libraries(gateway_test_lab_support PUBLIC ws2_32)
else()
    target_link_libraries(gateway_test_lab_support PUBLIC pthread)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(gateway_test_lab_support PUBLIC stdc++fs)
endif()

add_executable(GatewayTestLabSim test-lab/src/main.cpp)
set_target_properties(GatewayTestLabSim PROPERTIES OUTPUT_NAME gateway-test-lab-sim)
target_link_libraries(GatewayTestLabSim PRIVATE gateway_test_lab_support)
target_compile_options(GatewayTestLabSim PRIVATE -fdiagnostics-color=never)

add_executable(gateway_test_lab_modbus_test test-lab/tests/modbus_simulator_test.cpp)
target_link_libraries(gateway_test_lab_modbus_test PRIVATE gateway_test_lab_support edge_gateway)
target_compile_options(gateway_test_lab_modbus_test PRIVATE -fdiagnostics-color=never)

add_executable(gateway_test_lab_serial_test test-lab/tests/serial_simulator_test.cpp)
target_link_libraries(gateway_test_lab_serial_test PRIVATE gateway_test_lab_support edge_gateway)
target_compile_options(gateway_test_lab_serial_test PRIVATE -fdiagnostics-color=never)

add_executable(gateway_test_lab_iec104_test test-lab/tests/iec104_simulator_test.cpp)
target_link_libraries(gateway_test_lab_iec104_test PRIVATE gateway_test_lab_support edge_gateway)
target_compile_options(gateway_test_lab_iec104_test PRIVATE -fdiagnostics-color=never)

add_executable(dio_smoke_test tools/dio_smoke_test.cpp)
target_link_libraries(dio_smoke_test PRIVATE edge_gateway)
target_compile_options(dio_smoke_test PRIVATE -fdiagnostics-color=never)

add_executable(can_signal_codec_test tools/can_signal_codec_test.cpp)
target_link_libraries(can_signal_codec_test PRIVATE edge_gateway)
target_compile_options(can_signal_codec_test PRIVATE -fdiagnostics-color=never)

add_executable(iec_codec_test tools/iec_codec_test.cpp)
target_link_libraries(iec_codec_test PRIVATE edge_gateway)
target_compile_options(iec_codec_test PRIVATE -fdiagnostics-color=never)

add_executable(am5se_iec103_codec_test tools/am5se_iec103_codec_test.cpp)
target_link_libraries(am5se_iec103_codec_test PRIVATE edge_gateway)
target_compile_options(am5se_iec103_codec_test PRIVATE -fdiagnostics-color=never)

add_executable(am5se_iec103_client_integration_test tools/am5se_iec103_client_integration_test.cpp)
target_link_libraries(am5se_iec103_client_integration_test PRIVATE edge_gateway)
target_compile_options(am5se_iec103_client_integration_test PRIVATE -fdiagnostics-color=never)

add_executable(iec103_recording_command_test tools/iec103_recording_command_test.cpp)
target_link_libraries(iec103_recording_command_test PRIVATE edge_gateway)
target_compile_options(iec103_recording_command_test PRIVATE -fdiagnostics-color=never)

add_executable(iec103_recording_transfer_service_test tools/iec103_recording_transfer_service_test.cpp)
target_link_libraries(iec103_recording_transfer_service_test PRIVATE edge_gateway)
target_compile_options(iec103_recording_transfer_service_test PRIVATE -fdiagnostics-color=never)

add_executable(iec104_client_integration_test tools/iec104_client_integration_test.cpp)
target_link_libraries(iec104_client_integration_test PRIVATE edge_gateway)
target_compile_options(iec104_client_integration_test PRIVATE -fdiagnostics-color=never)

add_executable(iec_collector_incremental_test tools/iec_collector_incremental_test.cpp)
target_link_libraries(iec_collector_incremental_test PRIVATE edge_gateway)
target_compile_options(iec_collector_incremental_test PRIVATE -fdiagnostics-color=never)

add_executable(config_loader_test tools/config_loader_test.cpp)
target_link_libraries(config_loader_test PRIVATE edge_gateway)
target_compile_options(config_loader_test PRIVATE -fdiagnostics-color=never)

add_executable(timing_policy_test tools/timing_policy_test.cpp)
target_link_libraries(timing_policy_test PRIVATE edge_gateway)
target_compile_options(timing_policy_test PRIVATE -fdiagnostics-color=never)

add_executable(collector_partial_failure_test tools/collector_partial_failure_test.cpp)
target_link_libraries(collector_partial_failure_test PRIVATE edge_gateway)
target_compile_options(collector_partial_failure_test PRIVATE -fdiagnostics-color=never)

add_executable(gateway_daemon_realtime_priority_test tools/gateway_daemon_realtime_priority_test.cpp)
target_link_libraries(gateway_daemon_realtime_priority_test PRIVATE edge_gateway)
target_compile_options(gateway_daemon_realtime_priority_test PRIVATE -fdiagnostics-color=never)

add_executable(priority_control_lease_test tools/priority_control_lease_test.cpp)
target_link_libraries(priority_control_lease_test PRIVATE edge_gateway)
target_compile_options(priority_control_lease_test PRIVATE -fdiagnostics-color=never)

add_executable(agc_avc_controller_test tools/agc_avc_controller_test.cpp)
target_link_libraries(agc_avc_controller_test PRIVATE edge_gateway)
target_compile_options(agc_avc_controller_test PRIVATE -fdiagnostics-color=never)

add_executable(modbus_rtu_client_resync_test tools/modbus_rtu_client_resync_test.cpp)
target_link_libraries(modbus_rtu_client_resync_test PRIVATE edge_gateway)
target_compile_options(modbus_rtu_client_resync_test PRIVATE -fdiagnostics-color=never)

add_executable(modbus_codec_test tools/modbus_codec_test.cpp)
target_link_libraries(modbus_codec_test PRIVATE edge_gateway)
target_compile_options(modbus_codec_test PRIVATE -fdiagnostics-color=never)

add_executable(dlt645_client_interval_test tools/dlt645_client_interval_test.cpp)
target_link_libraries(dlt645_client_interval_test PRIVATE edge_gateway)
target_compile_options(dlt645_client_interval_test PRIVATE -fdiagnostics-color=never)

add_executable(dlt645_command_executor_test tools/dlt645_command_executor_test.cpp)
target_link_libraries(dlt645_command_executor_test PRIVATE edge_gateway)
target_compile_options(dlt645_command_executor_test PRIVATE -fdiagnostics-color=never)

add_executable(modbus_tcp_client_limit_test tools/modbus_tcp_client_limit_test.cpp)
target_link_libraries(modbus_tcp_client_limit_test PRIVATE edge_gateway)
target_compile_options(modbus_tcp_client_limit_test PRIVATE -fdiagnostics-color=never)

add_executable(modbus_northbound_server_test tools/modbus_northbound_server_test.cpp)
target_link_libraries(modbus_northbound_server_test PRIVATE edge_gateway)
target_compile_options(modbus_northbound_server_test PRIVATE -fdiagnostics-color=never)

add_executable(memory_point_store_lifecycle_test tools/memory_point_store_lifecycle_test.cpp)
target_link_libraries(memory_point_store_lifecycle_test PRIVATE edge_gateway)
target_compile_options(memory_point_store_lifecycle_test PRIVATE -fdiagnostics-color=never)

if(UNIX)
    add_executable(filesystem_compat_test tools/filesystem_compat_test.cpp)
    target_include_directories(filesystem_compat_test PRIVATE include)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 8)
        target_link_libraries(filesystem_compat_test PRIVATE stdc++fs)
    endif()
    add_test(NAME filesystem_compat COMMAND filesystem_compat_test)
    add_executable(control_dedup_store_test tools/control_dedup_store_test.cpp)
    target_link_libraries(control_dedup_store_test PRIVATE edge_gateway)
    add_test(NAME control_dedup_store COMMAND control_dedup_store_test)
    add_executable(control_dedup_integration_test tools/control_dedup_integration_test.cpp)
    target_link_libraries(control_dedup_integration_test PRIVATE edge_gateway)
    add_test(NAME control_dedup_integration COMMAND control_dedup_integration_test)
    add_executable(control_dedup_protocol_test tools/control_dedup_protocol_test.cpp)
    target_link_libraries(control_dedup_protocol_test PRIVATE edge_gateway)
    add_test(NAME control_dedup_protocol COMMAND control_dedup_protocol_test)
    add_library(sqlite_failure_fixture SHARED tools/sqlite_failure_fixture.cpp)
    target_compile_options(sqlite_failure_fixture PRIVATE -fdiagnostics-color=never)
    add_executable(sqlite_writer_failure_test tools/sqlite_writer_failure_test.cpp)
    target_link_libraries(sqlite_writer_failure_test PRIVATE edge_gateway dl)
    target_compile_options(sqlite_writer_failure_test PRIVATE -fdiagnostics-color=never)
    add_dependencies(sqlite_writer_failure_test sqlite_failure_fixture)

    add_library(sqlite_outbox_failure_fixture SHARED tools/sqlite_outbox_failure_fixture.cpp)
    target_link_libraries(sqlite_outbox_failure_fixture PRIVATE dl)
    add_executable(sqlite_outbox_failure_test tools/sqlite_outbox_failure_test.cpp)
    target_link_libraries(sqlite_outbox_failure_test PRIVATE edge_gateway dl)
    add_dependencies(sqlite_outbox_failure_test sqlite_outbox_failure_fixture sqlite_failure_fixture)

    add_executable(persistent_flush_pressure_test tools/persistent_flush_pressure_test.cpp)
    target_link_libraries(persistent_flush_pressure_test PRIVATE edge_gateway dl)
    target_compile_options(persistent_flush_pressure_test PRIVATE -fdiagnostics-color=never)
    add_dependencies(persistent_flush_pressure_test sqlite_failure_fixture)

    add_executable(sqlite_sample_writer_retention_test tools/sqlite_sample_writer_retention_test.cpp)
    target_link_libraries(sqlite_sample_writer_retention_test PRIVATE edge_gateway dl)
    target_compile_options(sqlite_sample_writer_retention_test PRIVATE -fdiagnostics-color=never)
    add_executable(sqlite_history_storage_guard_test tools/sqlite_history_storage_guard_test.cpp)
    target_link_libraries(sqlite_history_storage_guard_test PRIVATE edge_gateway dl)
    target_compile_options(sqlite_history_storage_guard_test PRIVATE -fdiagnostics-color=never)
    add_test(NAME sqlite_history_storage_guard COMMAND sqlite_history_storage_guard_test)
    add_executable(storage_retention_test tools/storage_retention_test.cpp)
    target_link_libraries(storage_retention_test PRIVATE edge_gateway dl)
    add_test(NAME storage_retention COMMAND storage_retention_test)
    set_tests_properties(storage_retention PROPERTIES TIMEOUT 30)
    add_executable(storage_budget_test tools/storage_budget_test.cpp)
    target_link_libraries(storage_budget_test PRIVATE edge_gateway)
    add_test(NAME storage_budget COMMAND storage_budget_test)
    add_executable(storage_budget_ndebug_test tools/storage_budget_test.cpp)
    target_compile_definitions(storage_budget_ndebug_test PRIVATE NDEBUG)
    target_link_libraries(storage_budget_ndebug_test PRIVATE edge_gateway)
    add_test(NAME storage_budget_ndebug COMMAND storage_budget_ndebug_test)

    add_executable(posix_serial_port_recovery_test tools/posix_serial_port_recovery_test.cpp)
    target_link_libraries(posix_serial_port_recovery_test PRIVATE edge_gateway)
    target_compile_options(posix_serial_port_recovery_test PRIVATE -fdiagnostics-color=never)
endif()

add_executable(point_store_router_normalize_test tools/point_store_router_normalize_test.cpp)
target_link_libraries(point_store_router_normalize_test PRIVATE edge_gateway)
target_compile_options(point_store_router_normalize_test PRIVATE -fdiagnostics-color=never)

add_executable(point_store_router_retention_test tools/point_store_router_retention_test.cpp)
target_link_libraries(point_store_router_retention_test PRIVATE edge_gateway)
target_compile_options(point_store_router_retention_test PRIVATE -fdiagnostics-color=never)

add_executable(scada_runtime_test tools/scada_runtime_test.cpp)
target_link_libraries(scada_runtime_test PRIVATE edge_gateway)
target_compile_options(scada_runtime_test PRIVATE -fdiagnostics-color=never)

add_executable(scada_upper_computer_safety_test tools/scada_upper_computer_safety_test.cpp)
target_link_libraries(scada_upper_computer_safety_test PRIVATE edge_gateway)
target_compile_options(scada_upper_computer_safety_test PRIVATE -fdiagnostics-color=never)

add_executable(builtin_mqtt_driver_publisher_test tools/builtin_mqtt_driver_publisher_test.cpp)
target_link_libraries(builtin_mqtt_driver_publisher_test PRIVATE edge_gateway)
target_compile_options(builtin_mqtt_driver_publisher_test PRIVATE -fdiagnostics-color=never)

add_executable(mqtt_event_outbox_target_test tools/mqtt_event_outbox_target_test.cpp)
target_link_libraries(mqtt_event_outbox_target_test PRIVATE edge_gateway)
target_compile_options(mqtt_event_outbox_target_test PRIVATE -fdiagnostics-color=never)

if(UNIX)
    add_executable(sqlite_outbox_benchmark tools/sqlite_outbox_benchmark.cpp src/mqtt_event_outbox.cpp src/event_store_stats.cpp)
    target_include_directories(sqlite_outbox_benchmark PRIVATE include)
    target_link_libraries(sqlite_outbox_benchmark PRIVATE dl pthread)
endif()

add_executable(mqtt_driver_service_test tools/mqtt_driver_service_test.cpp)
target_link_libraries(mqtt_driver_service_test PRIVATE edge_gateway)
target_compile_options(mqtt_driver_service_test PRIVATE -fdiagnostics-color=never)

add_executable(mqtt_forwarder_service_test tools/mqtt_forwarder_service_test.cpp)
target_link_libraries(mqtt_forwarder_service_test PRIVATE edge_gateway)
target_compile_options(mqtt_forwarder_service_test PRIVATE -fdiagnostics-color=never)

add_executable(event_engine_service_test tools/event_engine_service_test.cpp)
target_link_libraries(event_engine_service_test PRIVATE edge_gateway)
target_compile_options(event_engine_service_test PRIVATE -fdiagnostics-color=never)

add_executable(compute_engine_service_test tools/compute_engine_service_test.cpp)
target_link_libraries(compute_engine_service_test PRIVATE edge_gateway)
target_compile_options(compute_engine_service_test PRIVATE -fdiagnostics-color=never)

add_executable(ems_cluster_test tools/ems_cluster_test.cpp)
target_link_libraries(ems_cluster_test PRIVATE edge_gateway)
target_compile_options(ems_cluster_test PRIVATE -fdiagnostics-color=never)

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(cluster_driver_send_guard_test tools/cluster_driver_send_guard_test.cpp)
    target_link_libraries(cluster_driver_send_guard_test PRIVATE edge_gateway)
    target_link_options(cluster_driver_send_guard_test PRIVATE
        -Wl,--wrap=send -Wl,--wrap=select -Wl,--wrap=write -Wl,--wrap=nanosleep)
    add_test(NAME cluster_driver_send_guard COMMAND cluster_driver_send_guard_test)
    set_tests_properties(cluster_driver_send_guard PROPERTIES TIMEOUT 30)
    add_executable(cluster_driver_rejection_test tools/cluster_driver_rejection_test.cpp)
    target_link_libraries(cluster_driver_rejection_test PRIVATE edge_gateway)
    add_test(NAME cluster_driver_rejection COMMAND cluster_driver_rejection_test)
    set_tests_properties(cluster_driver_rejection PROPERTIES TIMEOUT 30)
    add_executable(ems_cluster_transport_bounds_test
        tools/ems_cluster_transport_bounds_test.cpp
        src/ems_cluster.cpp src/ems_cluster_dispatch.cpp src/ems_cluster_transport.cpp)
    target_include_directories(ems_cluster_transport_bounds_test PRIVATE include)
    target_link_options(ems_cluster_transport_bounds_test PRIVATE
        -Wl,--wrap=accept -Wl,--wrap=recv -Wl,--wrap=recvfrom
        -Wl,--wrap=send -Wl,--wrap=poll -Wl,--wrap=__recv_chk -Wl,--wrap=__recvfrom_chk
        -Wl,--wrap=__poll_chk)
    add_test(NAME ems_cluster_transport_bounds COMMAND ems_cluster_transport_bounds_test)
    set_tests_properties(ems_cluster_transport_bounds PROPERTIES TIMEOUT 20)
endif()

if(UNIX)
    add_executable(cluster_write_authorization_test tools/cluster_write_authorization_test.cpp)
    target_link_libraries(cluster_write_authorization_test PRIVATE edge_gateway)
    add_executable(ems_cluster_output_authority_test tools/ems_cluster_output_authority_test.cpp)
    target_link_libraries(ems_cluster_output_authority_test PRIVATE edge_gateway)
    add_executable(ems_cluster_strategy_startup_test tools/ems_cluster_strategy_startup_test.cpp)
    target_link_libraries(ems_cluster_strategy_startup_test PRIVATE edge_gateway)
    add_executable(ems_cluster_capability_snapshot_test tools/ems_cluster_capability_snapshot_test.cpp)
    target_link_libraries(ems_cluster_capability_snapshot_test PRIVATE edge_gateway)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(ems_cluster_load_sampler_test tools/ems_cluster_load_sampler_test.cpp)
        target_include_directories(ems_cluster_load_sampler_test PRIVATE include)
        target_compile_options(ems_cluster_load_sampler_test PRIVATE -ffunction-sections -fdata-sections)
        target_link_options(ems_cluster_load_sampler_test PRIVATE
            -Wl,--gc-sections -Wl,--wrap=_ZNSt6chrono3_V212system_clock3nowEv)
        target_link_libraries(ems_cluster_load_sampler_test PRIVATE pthread)
    endif()
    add_executable(cluster_router_authority_test tools/cluster_router_authority_test.cpp)
    target_link_libraries(cluster_router_authority_test PRIVATE edge_gateway)
    add_executable(cluster_config_authority_test tools/cluster_config_authority_test.cpp)
    target_link_libraries(cluster_config_authority_test PRIVATE edge_gateway)
endif()

add_executable(legacy_ems_test tools/legacy_ems_test.cpp)
target_link_libraries(legacy_ems_test PRIVATE edge_gateway)
target_compile_options(legacy_ems_test PRIVATE -fdiagnostics-color=never)

add_executable(graph_ems_voltage_quality_test tools/graph_ems_voltage_quality_test.cpp)
target_link_libraries(graph_ems_voltage_quality_test PRIVATE edge_gateway)
target_compile_options(graph_ems_voltage_quality_test PRIVATE -fdiagnostics-color=never)

add_executable(graph_ems_cluster_dispatch_test tools/graph_ems_cluster_dispatch_test.cpp)
target_link_libraries(graph_ems_cluster_dispatch_test PRIVATE edge_gateway)
target_compile_options(graph_ems_cluster_dispatch_test PRIVATE -fdiagnostics-color=never)

add_executable(flexible_phase_balance_test tools/flexible_phase_balance_test.cpp)
target_link_libraries(flexible_phase_balance_test PRIVATE edge_gateway)
target_compile_options(flexible_phase_balance_test PRIVATE -fdiagnostics-color=never)

add_executable(grid_friendly_dispatch_test tools/grid_friendly_dispatch_test.cpp)
target_link_libraries(grid_friendly_dispatch_test PRIVATE edge_gateway)
target_compile_options(grid_friendly_dispatch_test PRIVATE -fdiagnostics-color=never)

add_executable(graph_ems_v2_loader_test tools/graph_ems_v2_loader_test.cpp)
add_executable(grid_reserve_test tools/grid_reserve_test.cpp)
target_link_libraries(grid_reserve_test PRIVATE edge_gateway)
target_link_libraries(graph_ems_v2_loader_test PRIVATE edge_gateway)
target_compile_options(graph_ems_v2_loader_test PRIVATE -fdiagnostics-color=never)

add_executable(EmsParityCheck tools/ems_parity_check.cpp)
target_link_libraries(EmsParityCheck PRIVATE edge_gateway)
target_compile_options(EmsParityCheck PRIVATE -fdiagnostics-color=never)

add_executable(ota_service_test tools/ota_service_test.cpp)
target_link_libraries(ota_service_test PRIVATE edge_gateway)
target_compile_options(ota_service_test PRIVATE -fdiagnostics-color=never)

add_executable(system_monitor_service_test tools/system_monitor_service_test.cpp)
target_link_libraries(system_monitor_service_test PRIVATE edge_gateway)
target_compile_options(system_monitor_service_test PRIVATE -fdiagnostics-color=never)

add_executable(system_monitor_runtime_discovery_test tools/system_monitor_runtime_discovery_test.cpp)
target_link_libraries(system_monitor_runtime_discovery_test PRIVATE edge_gateway)
target_compile_options(system_monitor_runtime_discovery_test PRIVATE -fdiagnostics-color=never)

add_executable(
    system_monitor_direct_maintenance_contract_test
    tools/system_monitor_direct_maintenance_contract_test.cpp
    system_monitor_direct_maintenance.cpp
)
target_link_libraries(system_monitor_direct_maintenance_contract_test PRIVATE edge_gateway)
target_compile_options(system_monitor_direct_maintenance_contract_test PRIVATE -fdiagnostics-color=never)

if(BUILD_TESTING)
    add_test(NAME agc_avc_controller COMMAND agc_avc_controller_test)
    add_test(NAME mqtt_forwarder_service COMMAND mqtt_forwarder_service_test)
    add_test(NAME point_store_router_retention COMMAND point_store_router_retention_test)
    add_test(NAME scada_runtime COMMAND scada_runtime_test)
    if(TARGET local_display_qt_access_test)
        add_test(NAME local_display_qt_access COMMAND local_display_qt_access_test)
    endif()
    add_test(NAME ems_cluster COMMAND ems_cluster_test)
    if(UNIX)
        add_test(NAME cluster_write_authorization COMMAND cluster_write_authorization_test)
        add_test(NAME ems_cluster_output_authority COMMAND ems_cluster_output_authority_test)
        add_test(NAME ems_cluster_strategy_startup COMMAND ems_cluster_strategy_startup_test)
        add_test(NAME ems_cluster_capability_snapshot COMMAND ems_cluster_capability_snapshot_test)
        if(TARGET ems_cluster_load_sampler_test)
            add_test(NAME ems_cluster_load_sampler COMMAND ems_cluster_load_sampler_test)
        endif()
        add_test(NAME cluster_router_authority COMMAND cluster_router_authority_test)
        add_test(NAME cluster_config_authority COMMAND cluster_config_authority_test)
    endif()
    add_test(NAME gateway_test_lab_modbus COMMAND gateway_test_lab_modbus_test)
    add_test(NAME gateway_test_lab_serial COMMAND gateway_test_lab_serial_test)
    add_test(NAME gateway_test_lab_iec104 COMMAND gateway_test_lab_iec104_test)
    add_test(NAME am5se_iec103_codec COMMAND am5se_iec103_codec_test)
    add_test(NAME am5se_iec103_passive_tcp COMMAND am5se_iec103_client_integration_test)
    add_test(NAME iec103_recording_command COMMAND iec103_recording_command_test)
    add_test(NAME iec103_recording_transfer COMMAND iec103_recording_transfer_service_test)
    add_test(NAME iec104_client_integration COMMAND iec104_client_integration_test)
    add_test(NAME iec_collector_incremental COMMAND iec_collector_incremental_test)
    add_test(NAME can_signal_codec COMMAND can_signal_codec_test)
    add_test(NAME collector_partial_failure COMMAND collector_partial_failure_test)
    add_test(NAME compute_engine_service COMMAND compute_engine_service_test)
    add_test(NAME config_loader COMMAND config_loader_test)
    add_test(NAME dio_smoke COMMAND dio_smoke_test)
    add_test(NAME dlt645_client_interval COMMAND dlt645_client_interval_test)
    add_test(NAME dlt645_command_executor COMMAND dlt645_command_executor_test)
    add_test(NAME event_engine_service COMMAND event_engine_service_test)
    add_test(NAME gateway_daemon_realtime_priority COMMAND gateway_daemon_realtime_priority_test)
    add_test(NAME iec_codec COMMAND iec_codec_test)
    add_test(NAME memory_point_store_lifecycle COMMAND memory_point_store_lifecycle_test)
    set_tests_properties(memory_point_store_lifecycle PROPERTIES TIMEOUT 15)
    if(UNIX)
        add_executable(memory_point_store_reader_cache_test
            tools/memory_point_store_reader_cache_test.cpp src/memory_point_store.cpp)
        target_include_directories(memory_point_store_reader_cache_test PRIVATE include)
        target_link_libraries(memory_point_store_reader_cache_test PRIVATE pthread rt)
        target_compile_options(memory_point_store_reader_cache_test PRIVATE -fdiagnostics-color=never)
        add_test(NAME memory_point_store_reader_cache COMMAND memory_point_store_reader_cache_test)
        set_tests_properties(memory_point_store_reader_cache PROPERTIES TIMEOUT 15)
    endif()
    if(TARGET sqlite_writer_failure_test)
        add_test(NAME sqlite_writer_failure COMMAND sqlite_writer_failure_test $<TARGET_FILE:sqlite_failure_fixture>)
        add_test(NAME sqlite_outbox_failure COMMAND sqlite_outbox_failure_test $<TARGET_FILE:sqlite_outbox_failure_fixture> $<TARGET_FILE:sqlite_failure_fixture>)
        set_tests_properties(sqlite_writer_failure PROPERTIES TIMEOUT 15)
    endif()
    if(TARGET persistent_flush_pressure_test)
        add_test(NAME persistent_flush_pressure_gateway COMMAND persistent_flush_pressure_test gateway $<TARGET_FILE:sqlite_failure_fixture>)
        add_test(NAME persistent_flush_pressure_can COMMAND persistent_flush_pressure_test can $<TARGET_FILE:sqlite_failure_fixture>)
        set_tests_properties(persistent_flush_pressure_gateway persistent_flush_pressure_can PROPERTIES TIMEOUT 60)
        add_test(NAME point_history_background_gateway COMMAND persistent_flush_pressure_test gateway $<TARGET_FILE:sqlite_failure_fixture> retention)
        add_test(NAME point_history_background_can COMMAND persistent_flush_pressure_test can $<TARGET_FILE:sqlite_failure_fixture> retention)
        set_tests_properties(point_history_background_gateway point_history_background_can PROPERTIES TIMEOUT 60)
    endif()
    if(TARGET sqlite_sample_writer_retention_test)
        add_test(NAME sqlite_sample_writer_retention COMMAND sqlite_sample_writer_retention_test)
        set_tests_properties(sqlite_sample_writer_retention PROPERTIES TIMEOUT 30)
    endif()
    if(TARGET posix_serial_port_recovery_test)
        add_test(NAME posix_serial_port_recovery COMMAND posix_serial_port_recovery_test)
        set_tests_properties(posix_serial_port_recovery PROPERTIES TIMEOUT 5)
    endif()
    add_test(NAME modbus_northbound_server COMMAND modbus_northbound_server_test)
    add_test(NAME modbus_codec COMMAND modbus_codec_test)
    add_test(NAME modbus_rtu_client_resync COMMAND modbus_rtu_client_resync_test)
    add_test(NAME modbus_tcp_client_limit COMMAND modbus_tcp_client_limit_test)
    add_test(NAME mqtt_driver_service COMMAND mqtt_driver_service_test)
    add_test(NAME mqtt_event_outbox_target COMMAND mqtt_event_outbox_target_test)
    add_test(NAME ota_service COMMAND ota_service_test)
    if(UNIX)
        add_test(NAME ota_checksum COMMAND ota_service_test --checksum-only)
        set_tests_properties(ota_checksum PROPERTIES TIMEOUT 15)
    endif()
    if(UNIX)
        add_executable(ota_checksum_test tools/ota_checksum_test.cpp src/ota_service.cpp)
        target_include_directories(ota_checksum_test PRIVATE include)
        target_link_libraries(ota_checksum_test PRIVATE pthread)
        target_compile_options(ota_checksum_test PRIVATE -fdiagnostics-color=never)
        add_test(NAME ota_checksum_standalone COMMAND ota_checksum_test)
        set_tests_properties(ota_checksum_standalone PROPERTIES TIMEOUT 15)
    endif()
    add_test(NAME point_store_router_normalize COMMAND point_store_router_normalize_test)
    add_test(NAME priority_control_lease COMMAND priority_control_lease_test)
    add_test(NAME scada_upper_computer_safety COMMAND scada_upper_computer_safety_test)
    add_test(NAME system_monitor_runtime_discovery COMMAND system_monitor_runtime_discovery_test)
    add_test(NAME system_monitor_service COMMAND system_monitor_service_test)
    add_test(NAME timing_policy COMMAND timing_policy_test)
    if(UNIX)
        add_test(NAME mqtt_offline_restart_replay COMMAND builtin_mqtt_driver_publisher_test)
        add_test(NAME gateway_services_boot
            COMMAND sh "${CMAKE_SOURCE_DIR}/tools/gateway_services_boot_test.sh")
        set_tests_properties(gateway_services_boot PROPERTIES TIMEOUT 15)
        add_test(
            NAME gateway_test_lab_modbus_e2e
            COMMAND sh
                "${CMAKE_SOURCE_DIR}/test-lab/tests/run_modbus_e2e.sh"
                "${CMAKE_SOURCE_DIR}"
                "${CMAKE_BINARY_DIR}"
        )
        set_tests_properties(gateway_test_lab_modbus_e2e PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
        add_test(
            NAME gateway_test_lab_protocol_e2e
            COMMAND sh
                "${CMAKE_SOURCE_DIR}/test-lab/tests/run_protocol_e2e.sh"
                "${CMAKE_SOURCE_DIR}"
                "${CMAKE_BINARY_DIR}"
        )
        set_tests_properties(gateway_test_lab_protocol_e2e PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
        add_test(
            NAME gateway_test_lab_performance_report_e2e
            COMMAND sh
                "${CMAKE_SOURCE_DIR}/test-lab/tests/run_performance_report_e2e.sh"
                "${CMAKE_SOURCE_DIR}"
                "${CMAKE_BINARY_DIR}"
        )
        set_tests_properties(gateway_test_lab_performance_report_e2e PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
    endif()
    add_test(NAME graph_ems_v2_loader COMMAND graph_ems_v2_loader_test)
    add_test(NAME grid_reserve COMMAND grid_reserve_test)
    add_test(NAME graph_ems_voltage_quality COMMAND graph_ems_voltage_quality_test)
    add_test(NAME graph_ems_cluster_dispatch COMMAND graph_ems_cluster_dispatch_test)
    add_test(NAME flexible_phase_balance COMMAND flexible_phase_balance_test)
    add_test(NAME grid_friendly_dispatch COMMAND grid_friendly_dispatch_test)
    add_test(NAME legacy_ems_migration_parity COMMAND legacy_ems_test)
    add_test(NAME system_monitor_direct_maintenance_contract COMMAND system_monitor_direct_maintenance_contract_test)
    set_tests_properties(
        graph_ems_v2_loader
        graph_ems_voltage_quality
        graph_ems_cluster_dispatch
        legacy_ems_migration_parity
        point_store_router_retention
        system_monitor_direct_maintenance_contract
        gateway_test_lab_modbus
        am5se_iec103_codec
        am5se_iec103_passive_tcp
        iec103_recording_command
        iec103_recording_transfer
        iec104_client_integration
        iec_collector_incremental
        can_signal_codec
        collector_partial_failure
        compute_engine_service
        config_loader
        dio_smoke
        dlt645_client_interval
        dlt645_command_executor
        event_engine_service
        gateway_daemon_realtime_priority
        iec_codec
        memory_point_store_lifecycle
        modbus_northbound_server
        modbus_rtu_client_resync
        modbus_tcp_client_limit
        mqtt_driver_service
        mqtt_event_outbox_target
        mqtt_forwarder_service
        ota_service
        point_store_router_normalize
        priority_control_lease
        scada_upper_computer_safety
        system_monitor_runtime_discovery
        system_monitor_service
        timing_policy
        PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(system_monitor_service PROPERTIES RUN_SERIAL TRUE)
endif()
