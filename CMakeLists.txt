cmake_minimum_required(VERSION 3.25)

project(rtmp_server
    VERSION 0.1.0
    DESCRIPTION "Production-grade C++23 io_uring RTMP streaming server"
    LANGUAGES CXX
)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(WARNING
        "rtmp_server's transport layer requires Linux io_uring. "
        "Configuring on ${CMAKE_SYSTEM_NAME} will fail to build the io_uring target. "
        "Use RTMP_SERVER_CORE_ONLY=ON to configure only platform-independent components.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(RTMP_SERVER_BUILD_TESTS "Build unit and integration tests" ON)
option(RTMP_SERVER_ENABLE_ASAN "Enable AddressSanitizer + UBSan" OFF)
option(RTMP_SERVER_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(RTMP_SERVER_CORE_ONLY "Only build platform-independent core (no io_uring transport)" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(CompilerWarnings)
include(Sanitizers)
include(StaticAnalysis)
include(Dependencies)

add_library(rtmp_server_warnings INTERFACE)
rtmp_server_set_warnings(rtmp_server_warnings)

add_library(rtmp_server_sanitizers INTERFACE)
rtmp_server_set_sanitizers(rtmp_server_sanitizers)

add_subdirectory(src/core)
add_subdirectory(src/protocol)

if(NOT RTMP_SERVER_CORE_ONLY AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(src/io/io_uring)
    add_subdirectory(apps/rtmp_server)
endif()

if(RTMP_SERVER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
    add_subdirectory(tests/protocol)
endif()
