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
option(RTMP_SERVER_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer on its own (ASan already includes it)" OFF)
# Phase 8: exploit mitigations for deployable builds. Set by the `production`
# CMake preset; see cmake/Sanitizers.cmake and docs/deployment.md.
option(RTMP_SERVER_ENABLE_HARDENING "Enable production exploit-mitigation compile/link flags" OFF)
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
# Production exploit mitigations ride on the same interface target every
# library already links, so enabling the `production` preset applies them
# everywhere without touching each target's CMakeLists.
rtmp_server_set_hardening(rtmp_server_sanitizers)

add_subdirectory(src/core)
add_subdirectory(src/protocol)
# FLV encoding + recording orchestration (Phase 6): pure byte-format/bookkeeping
# code, builds on every platform including core-only, same as src/protocol.
add_subdirectory(src/media)
# flv_inspector is a plain file reader (no io_uring), so it builds everywhere.
add_subdirectory(apps/flv_inspector)
# load_bench is a synthetic in-process load generator (Phase 9), also pure
# protocol-layer code — builds everywhere.
add_subdirectory(apps/load_bench)
# Phase 7 real load generator: real TCP sockets + real RTMP handshake and
# commands. POSIX sockets only (no io_uring), so it builds everywhere.
add_subdirectory(src/loadgen)
add_subdirectory(apps/rtmp_load_gen)
# TEST-ONLY poll-based server so the load generator has something to drive on
# hosts without io_uring. Never deployed; see its main.cpp header comment.
add_subdirectory(apps/rtmp_test_server)
# SQLite-backed persistence (Phase 9): system libsqlite3, no io_uring, builds
# everywhere.
add_subdirectory(src/persistence)
# Management API domain logic (Phase 8): pure logic over StreamRegistry/
# LiveFanout, no HTTP server/socket dependency, builds everywhere too.
add_subdirectory(src/management)
# Phase 5 authentication glue (StreamManager -> CommandSession hooks +
# per-IP/per-stream resource bounds): same "builds everywhere" posture as
# src/management, no HTTP server/socket dependency.
add_subdirectory(src/authentication)
# Phase 5 management HTTP API: hand-rolled bounded HTTP/1.1 server over
# plain POSIX sockets (see docs/management-api.md for why no vendored
# library was used) plus JSON (de)serialization for the endpoint set.
# Phase 6 HLS packaging (playlists, segmenter, bounded segment store). Must
# come before src/control, which links it for HLS HTTP delivery.
add_subdirectory(src/hls)
# Builds everywhere — POSIX sockets, not io_uring.
add_subdirectory(src/control)

if(NOT RTMP_SERVER_CORE_ONLY AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(src/io/io_uring)
    add_subdirectory(apps/rtmp_server)
endif()

if(RTMP_SERVER_BUILD_TESTS)
    enable_testing()
    # Fuzz harnesses (Phase 9): always buildable as corpus-replay tools;
    # -DRTMP_SERVER_ENABLE_FUZZING=ON additionally links libFuzzer (Clang only).
    add_subdirectory(fuzz)
    add_subdirectory(tests/unit)
    add_subdirectory(tests/protocol)
    add_subdirectory(tests/media)
    add_subdirectory(tests/recording)
    add_subdirectory(tests/hls)
    add_subdirectory(tests/management)
    add_subdirectory(tests/persistence)
    add_subdirectory(tests/integration)
    add_subdirectory(tests/load)
    if(NOT RTMP_SERVER_CORE_ONLY AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_subdirectory(tests/io_uring)
    endif()
    add_subdirectory(tests/authentication)
    add_subdirectory(tests/control)
endif()
