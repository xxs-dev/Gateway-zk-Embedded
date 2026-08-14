cmake_minimum_required(VERSION 3.20)
project(KyScadaWindows LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

get_filename_component(GATEWAY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

add_executable(KY-SCADA-Windows
    "${GATEWAY_ROOT}/scada_windows_renderer_main.cpp"
    "${GATEWAY_ROOT}/local_display_qt_scada_scene.cpp"
    "${GATEWAY_ROOT}/local_display_qt_access_control.cpp"
    "${GATEWAY_ROOT}/local_display_qt_pcs_power_control.cpp"
    "${GATEWAY_ROOT}/local_display_qt_value_map.cpp"
    "${GATEWAY_ROOT}/src/scada_project_loader.cpp"
)

target_include_directories(KY-SCADA-Windows PRIVATE
    "${GATEWAY_ROOT}"
    "${GATEWAY_ROOT}/include"
)
target_link_libraries(KY-SCADA-Windows PRIVATE Qt6::Widgets)

add_executable(KY-SCADA-Access-Test
    "${GATEWAY_ROOT}/tools/local_display_qt_access_test.cpp"
    "${GATEWAY_ROOT}/local_display_qt_access_control.cpp"
)
target_include_directories(KY-SCADA-Access-Test PRIVATE
    "${GATEWAY_ROOT}"
    "${GATEWAY_ROOT}/include"
)
target_link_libraries(KY-SCADA-Access-Test PRIVATE Qt6::Widgets)

# Compile the edge adapter against the same interface without linking the full edge runtime.
add_library(KY-SCADA-Edge-CompileCheck OBJECT EXCLUDE_FROM_ALL
    "${GATEWAY_ROOT}/local_display_qt_ems_main.cpp"
)
target_include_directories(KY-SCADA-Edge-CompileCheck PRIVATE
    "${GATEWAY_ROOT}"
    "${GATEWAY_ROOT}/include"
)
target_link_libraries(KY-SCADA-Edge-CompileCheck PRIVATE Qt6::Widgets)

if(MINGW)
    target_compile_options(KY-SCADA-Windows PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(KY-SCADA-Windows PRIVATE -static-libgcc -static-libstdc++)
endif()
