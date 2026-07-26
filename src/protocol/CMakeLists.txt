# Protocol-layer code (handshake, and later chunk/AMF0/commands) is pure
# state machines over buffers — no sockets, no liburing — per
# docs/architecture.md "Architectural Separation". It only depends on
# rtmp_server_core, so it builds on every platform, including under
# RTMP_SERVER_CORE_ONLY.
add_library(rtmp_server_protocol STATIC
    handshake/handshake_session.cpp
    chunk/chunk_decoder.cpp
    chunk/chunk_encoder.cpp
    amf0/amf0_encoder.cpp
    amf0/amf0_decoder.cpp
    commands/command_session.cpp
    commands/live_fanout.cpp
    media/media_ingest.cpp
    session/rtmp_connection_session.cpp
)

target_include_directories(rtmp_server_protocol PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(rtmp_server_protocol
    PUBLIC
        rtmp_server_core
        rtmp_server_warnings
        rtmp_server_sanitizers
)

target_compile_features(rtmp_server_protocol PUBLIC cxx_std_23)
