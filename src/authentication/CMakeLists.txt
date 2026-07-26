# Phase 5 authentication: ties management::StreamManager to
# protocol::commands::CommandSession's authorization hooks and enforces
# per-IP/per-stream resource bounds. Pure in-memory logic, no sockets, no
# liburing, no direct database access (StreamManager already isolates
# that) — builds on every platform, including RTMP_SERVER_CORE_ONLY.
add_library(rtmp_server_authentication STATIC
    rtmp_authenticator.cpp
)

target_include_directories(rtmp_server_authentication PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(rtmp_server_authentication
    PUBLIC
        rtmp_server_core
        rtmp_server_protocol
        rtmp_server_management
        rtmp_server_warnings
        rtmp_server_sanitizers
)

target_compile_features(rtmp_server_authentication PUBLIC cxx_std_23)
