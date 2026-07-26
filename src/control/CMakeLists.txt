# Phase 5 management HTTP API: bounded HTTP/1.1 server (plain POSIX
# sockets — see docs/management-api.md for why no vendored library) plus
# JSON routing over management::StreamManager. POSIX sockets, not
# io_uring, so this builds on every platform including RTMP_SERVER_CORE_ONLY
# (macOS dev/test builds), the same posture as src/persistence's SQLite
# usage.
add_library(rtmp_server_control STATIC
    http_server.cpp
    management_api.cpp
)

target_include_directories(rtmp_server_control PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(rtmp_server_control
    PUBLIC
        rtmp_server_core
        rtmp_server_protocol
        rtmp_server_management
        rtmp_server_persistence
        rtmp_server_warnings
        rtmp_server_sanitizers
)

target_compile_features(rtmp_server_control PUBLIC cxx_std_23)
