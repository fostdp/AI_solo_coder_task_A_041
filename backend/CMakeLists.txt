cmake_minimum_required(VERSION 3.14)
project(TurbineMonitorMicroservices VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "System: ${CMAKE_SYSTEM_NAME}")

# ============================================================
# Dependencies
# ============================================================
find_package(Threads REQUIRED)

find_path(FFTW3_INCLUDE_DIR fftw3.h)
find_library(FFTW3_LIBRARY NAMES fftw3)

find_path(CLICKHOUSE_INCLUDE_DIR clickhouse/Client.h)
find_library(CLICKHOUSE_LIBRARY NAMES clickhouse-cpp)

find_path(JSON_INCLUDE_DIR nlohmann/json.hpp)

find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
    find_path(SPDLOG_INCLUDE_DIR spdlog/spdlog.h)
    find_library(SPDLOG_LIBRARY NAMES spdlog)
    if(SPDLOG_INCLUDE_DIR AND SPDLOG_LIBRARY)
        message(STATUS "Found spdlog: ${SPDLOG_LIBRARY}")
    else()
        message(WARNING "spdlog not found - building without structured logging")
    endif()
endif()

find_path(PROMETHEUS_INCLUDE_DIR prometheus/registry.h)
find_library(PROMETHEUS_LIBRARY NAMES prometheus-cpp-pull prometheus-cpp-core)

include_directories(
    ${CMAKE_SOURCE_DIR}/common/include
    ${CMAKE_SOURCE_DIR}/include
    ${FFTW3_INCLUDE_DIR}
    ${CLICKHOUSE_INCLUDE_DIR}
    ${JSON_INCLUDE_DIR}
    ${SPDLOG_INCLUDE_DIR}
    ${PROMETHEUS_INCLUDE_DIR}
)

set(THIRD_PARTY_LIBS
    ${FFTW3_LIBRARY}
    ${CLICKHOUSE_LIBRARY}
    ${SPDLOG_LIBRARY}
    ${PROMETHEUS_LIBRARY}
    Threads::Threads
)

if(WIN32)
    list(APPEND THIRD_PARTY_LIBS ws2_32 mswsock)
endif()

# ============================================================
# Common static library (shared IPC + data structures)
# ============================================================
add_library(common STATIC
    common/src/ipc_queue.cpp
    src/data_structures.cpp
    src/config.cpp
    src/clickhouse_client.cpp
    src/logger.cpp
)

target_include_directories(common PUBLIC
    ${CMAKE_SOURCE_DIR}/common/include
    ${CMAKE_SOURCE_DIR}/include
)

# ============================================================
# Signal processing & algorithm library
# ============================================================
add_library(algo STATIC
    src/udp_server.cpp
    src/signal_processor.cpp
    src/feature_extractor.cpp
    src/cavitation_detector.cpp
    src/life_assessor.cpp
    src/alarm_manager.cpp
    src/api_server.cpp
    src/data_pipeline.cpp
)

target_include_directories(algo PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# ============================================================
# Service 1: PXI Collector (高速采集 + 预处理)
# ============================================================
add_executable(pxi_collector
    services/pxi_collector/main.cpp
)
target_link_libraries(pxi_collector PRIVATE common algo ${THIRD_PARTY_LIBS})
set_target_properties(pxi_collector PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/pxi_collector
)

# ============================================================
# Service 2: Cavitation Detector (空化识别 + 阶段判定)
# ============================================================
add_executable(cavitation_detector
    services/cavitation_detector/main.cpp
)
target_link_libraries(cavitation_detector PRIVATE common algo ${THIRD_PARTY_LIBS})
set_target_properties(cavitation_detector PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/cavitation_detector
)

# ============================================================
# Service 3: Fatigue Evaluator (雨流计数 + 寿命评估)
# ============================================================
add_executable(fatigue_evaluator
    services/fatigue_evaluator/main.cpp
)
target_link_libraries(fatigue_evaluator PRIVATE common algo ${THIRD_PARTY_LIBS})
set_target_properties(fatigue_evaluator PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/fatigue_evaluator
)

# ============================================================
# Service 4: Alarm Pusher (告警 + IEC 61850 推送)
# ============================================================
add_executable(alarm_pusher
    services/alarm_pusher/main.cpp
)
target_link_libraries(alarm_pusher PRIVATE common algo ${THIRD_PARTY_LIBS})
set_target_properties(alarm_pusher PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/alarm_pusher
)

# ============================================================
# Service 5: API Gateway (REST聚合层)
# ============================================================
add_executable(api_gateway
    services/api_gateway/main.cpp
)
target_link_libraries(api_gateway PRIVATE common algo ${THIRD_PARTY_LIBS})
set_target_properties(api_gateway PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/api_gateway
)

# ============================================================
# Simulator + Monolith (保留兼容)
# ============================================================
add_executable(turbine_monitor main.cpp)
target_link_libraries(turbine_monitor PRIVATE common algo ${THIRD_PARTY_LIBS})
set_target_properties(turbine_monitor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_executable(pxi_simulator simulators/pxi_simulator.cpp)
target_link_libraries(pxi_simulator PRIVATE common ${THIRD_PARTY_LIBS})
set_target_properties(pxi_simulator PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# ============================================================
# Install / Package
# ============================================================
install(DIRECTORY ../config DESTINATION .)
install(DIRECTORY ../frontend DESTINATION .)
install(DIRECTORY ../tests DESTINATION . OPTIONAL)

# Print summary
message(STATUS "==================================================")
message(STATUS "  Turbine Monitor Microservices Build Targets:")
message(STATUS "    [1/7] pxi_collector         -> UDP采集+FFT/小波")
message(STATUS "    [2/7] cavitation_detector   -> AE/IF+工况归一化")
message(STATUS "    [3/7] fatigue_evaluator     -> 四点法雨流+Miner")
message(STATUS "    [4/7] alarm_pusher          -> 分级告警+IEC61850")
message(STATUS "    [5/7] api_gateway           -> REST API聚合")
message(STATUS "    [6/7] turbine_monitor       -> 单体(兼容)")
message(STATUS "    [7/7] pxi_simulator         -> PXI流量模拟器")
message(STATUS "==================================================")
