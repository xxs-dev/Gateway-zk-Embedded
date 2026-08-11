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
    src/compute_engine_service.cpp
    src/config_loader.cpp
    src/dio_command_executor.cpp
    src/dio_collector.cpp
    src/dlt645_client.cpp
    src/dlt645_command_executor.cpp
    src/dlt645_collector.cpp
    src/dlt645_codec.cpp
    src/dlt645_standard_points_loader.cpp
    src/event_engine_service.cpp
    src/ems_cluster.cpp
    src/ems_cluster_dispatch.cpp
    src/ems_cluster_network.cpp
    src/ems_cluster_points.cpp
    src/ems_cluster_transport.cpp
    src/common/gateway_daemon.cpp
    src/common/gateway_daemon_modbus.cpp
    src/graph_ems_engine.cpp
    src/iec_client.cpp
    src/iec_codec.cpp
    src/iec103_recording_command.cpp
    src/iec103_recording_transfer_service.cpp
    src/iec_command_executor.cpp
    src/iec_collector.cpp
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
    src/power_control_ownership.cpp
    src/priority_control_lease.cpp
    src/read_task_planner.cpp
    src/scada_project_loader.cpp
    src/scada_runtime_map.cpp
    src/scada_control_lease.cpp
    src/scada_upper_computer_safety.cpp
    src/sqlite_sample_writer.cpp
    src/sqlite_alarm_writer.cpp
    src/system_monitor_runtime_discovery.cpp
    src/system_monitor_points.cpp
    src/sysfs_gpio_port.cpp
    src/system_monitor_service.cpp
    src/timing_policy.cpp
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

add_executable(IecDriver iec_driver_main.cpp)
target_link_libraries(IecDriver PRIVATE edge_gateway)
target_compile_options(IecDriver PRIVATE -fdiagnostics-color=never)

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
    target_include_directories(local_display_qt_access_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
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
        target_include_directories(local_display_qt_access_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
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

add_executable(mqtt_driver_service_test tools/mqtt_driver_service_test.cpp)
target_link_libraries(mqtt_driver_service_test PRIVATE edge_gateway)
target_compile_options(mqtt_driver_service_test PRIVATE -fdiagnostics-color=never)

add_executable(event_engine_service_test tools/event_engine_service_test.cpp)
target_link_libraries(event_engine_service_test PRIVATE edge_gateway)
target_compile_options(event_engine_service_test PRIVATE -fdiagnostics-color=never)

add_executable(compute_engine_service_test tools/compute_engine_service_test.cpp)
target_link_libraries(compute_engine_service_test PRIVATE edge_gateway)
target_compile_options(compute_engine_service_test PRIVATE -fdiagnostics-color=never)

add_executable(ems_cluster_test tools/ems_cluster_test.cpp)
target_link_libraries(ems_cluster_test PRIVATE edge_gateway)
target_compile_options(ems_cluster_test PRIVATE -fdiagnostics-color=never)

add_executable(legacy_ems_test tools/legacy_ems_test.cpp)
target_link_libraries(legacy_ems_test PRIVATE edge_gateway)
target_compile_options(legacy_ems_test PRIVATE -fdiagnostics-color=never)

add_executable(graph_ems_voltage_quality_test tools/graph_ems_voltage_quality_test.cpp)
target_link_libraries(graph_ems_voltage_quality_test PRIVATE edge_gateway)
target_compile_options(graph_ems_voltage_quality_test PRIVATE -fdiagnostics-color=never)

add_executable(graph_ems_cluster_dispatch_test tools/graph_ems_cluster_dispatch_test.cpp)
target_link_libraries(graph_ems_cluster_dispatch_test PRIVATE edge_gateway)
target_compile_options(graph_ems_cluster_dispatch_test PRIVATE -fdiagnostics-color=never)

add_executable(graph_ems_v2_loader_test tools/graph_ems_v2_loader_test.cpp)
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
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(
        NAME release_credential_audit
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/test_audit_release_credentials.py"
    )
    add_test(NAME config_loader COMMAND config_loader_test)
    add_test(NAME ota_service COMMAND ota_service_test)
    add_test(NAME point_store_router_retention COMMAND point_store_router_retention_test)
    add_test(NAME scada_runtime COMMAND scada_runtime_test)
    if(TARGET local_display_qt_access_test)
        add_test(NAME local_display_qt_access COMMAND local_display_qt_access_test)
    endif()
    add_test(NAME ems_cluster COMMAND ems_cluster_test)
    add_test(NAME gateway_test_lab_modbus COMMAND gateway_test_lab_modbus_test)
    add_test(NAME gateway_test_lab_serial COMMAND gateway_test_lab_serial_test)
    add_test(NAME gateway_test_lab_iec104 COMMAND gateway_test_lab_iec104_test)
    if(UNIX)
        add_test(
            NAME gateway_test_lab_modbus_e2e
            COMMAND sh
                "${CMAKE_SOURCE_DIR}/test-lab/tests/run_modbus_e2e.sh"
                "${CMAKE_SOURCE_DIR}"
                "${CMAKE_BINARY_DIR}"
        )
        set_tests_properties(gateway_test_lab_modbus_e2e PROPERTIES TIMEOUT 45)
        add_test(
            NAME gateway_test_lab_protocol_e2e
            COMMAND sh
                "${CMAKE_SOURCE_DIR}/test-lab/tests/run_protocol_e2e.sh"
                "${CMAKE_SOURCE_DIR}"
                "${CMAKE_BINARY_DIR}"
        )
        set_tests_properties(gateway_test_lab_protocol_e2e PROPERTIES TIMEOUT 90)
    endif()
    add_test(NAME graph_ems_v2_loader COMMAND graph_ems_v2_loader_test)
    add_test(NAME graph_ems_voltage_quality COMMAND graph_ems_voltage_quality_test)
    add_test(NAME graph_ems_cluster_dispatch COMMAND graph_ems_cluster_dispatch_test)
    add_test(NAME legacy_ems_migration_parity COMMAND legacy_ems_test)
    add_test(NAME system_monitor_direct_maintenance_contract COMMAND system_monitor_direct_maintenance_contract_test)
    if(UNIX)
        add_test(
            NAME gateway_services_boot
            COMMAND sh "${CMAKE_SOURCE_DIR}/tools/gateway_services_boot_test.sh"
        )
        add_test(
            NAME scada_install
            COMMAND sh "${CMAKE_SOURCE_DIR}/tools/scada_install_test.sh"
        )
        add_test(
            NAME factory_scada_runtime
            COMMAND sh "${CMAKE_SOURCE_DIR}/tools/factory_scada_runtime_test.sh"
        )
        set_tests_properties(factory_scada_runtime PROPERTIES TIMEOUT 30)
    endif()
    set_tests_properties(
        graph_ems_v2_loader
        graph_ems_voltage_quality
        graph_ems_cluster_dispatch
        legacy_ems_migration_parity
        point_store_router_retention
        system_monitor_direct_maintenance_contract
        gateway_test_lab_modbus
        PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(
        release_credential_audit
        config_loader
        ota_service
        PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
endif()
