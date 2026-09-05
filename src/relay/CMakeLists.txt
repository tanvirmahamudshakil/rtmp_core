# Outbound RTMP: pushing this server's own streams to another RTMP server —
# a second origin (the live-stream repeater / relay), or an external ingest
# such as a CDN or social platform (stream targets). Sockets only, no
# io_uring and no codec libraries, so it builds on every platform including
# RTMP_SERVER_CORE_ONLY and is testable against a loopback fake origin.
add_library(rtmp_server_relay STATIC
    rtmp_push_client.cpp
    stream_target.cpp
    stream_target_manager.cpp
    backup_publisher.cpp
    rtmp_tag_builder.cpp
)

target_include_directories(rtmp_server_relay PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(rtmp_server_relay
    PUBLIC
        rtmp_server_core
        rtmp_server_protocol
        rtmp_server_media
        rtmp_server_persistence
        rtmp_server_transcoding
        rtmp_server_warnings
        rtmp_server_sanitizers
)

target_compile_features(rtmp_server_relay PUBLIC cxx_std_23)
