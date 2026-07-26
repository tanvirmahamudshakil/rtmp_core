# Phase 1 integration tests (docs/v2_promot.md "PHASE 1 — Complete RTMP
# connection and session pipeline"): drive RtmpConnectionSession with real
# RTMP byte sequences, including one real-loopback-socket test. Only depends
# on rtmp_server_protocol (transport-agnostic) plus plain POSIX sockets for
# the socket test, so it builds and runs on every platform — including this
# macOS host where the io_uring transport target itself is unavailable.
add_executable(rtmp_server_integration_tests
    rtmp_connection_session_test.cpp
    rtmp_full_session_socket_test.cpp
)

target_link_libraries(rtmp_server_integration_tests PRIVATE
    rtmp_server_protocol
    rtmp_server_warnings
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(rtmp_server_integration_tests)
