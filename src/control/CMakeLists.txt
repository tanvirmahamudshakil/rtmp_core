# Phase 5 management HTTP API: bounded HTTP/1.1 server (plain POSIX
# sockets — see docs/management-api.md for why no vendored library) plus
# JSON routing over management::StreamManager. POSIX sockets, not
# io_uring, so this builds on every platform including RTMP_SERVER_CORE_ONLY
# (macOS dev/test builds), the same posture as src/persistence's SQLite
# usage.
#
# Phase 6 adds hls_http_handler.cpp: HLS playlist/segment delivery served by
# this same bounded HttpServer (chained in front of the management handler),
# deliberately not a second HTTP stack.
#
# edge_viewer_stats.cpp reads the cache edge's per-link viewer accounting
# (deploy/viewer-estimator) so the server's own API can report real HLS
# viewer counts, which the Varnish-collapsed origin cannot observe itself.
add_library(rtmp_server_control STATIC
    http_server.cpp
    management_api.cpp
    hls_http_handler.cpp
    edge_viewer_stats.cpp
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
        rtmp_server_hls
        rtmp_server_warnings
        rtmp_server_sanitizers
)

target_compile_features(rtmp_server_control PUBLIC cxx_std_23)
