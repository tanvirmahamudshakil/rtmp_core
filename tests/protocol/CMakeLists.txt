# Protocol-layer tests (handshake, and later chunk/AMF0/commands) only need
# rtmp_server_protocol + rtmp_server_core — no liburing, no io_uring target
# — so this builds and runs on every platform, including under
# RTMP_SERVER_CORE_ONLY. The socket integration test uses plain POSIX
# sockets directly (not the io_uring transport) for the same reason.
add_executable(rtmp_server_protocol_tests
    handshake/handshake_session_test.cpp
    handshake/handshake_socket_integration_test.cpp
    chunk/chunk_decoder_test.cpp
    chunk/chunk_encoder_test.cpp
    amf0/amf0_codec_test.cpp
    commands/command_session_test.cpp
    commands/live_fanout_test.cpp
    commands/stream_ids_test.cpp
    media/media_ingest_test.cpp
)

target_link_libraries(rtmp_server_protocol_tests PRIVATE
    rtmp_server_protocol
    rtmp_server_warnings
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(rtmp_server_protocol_tests)
