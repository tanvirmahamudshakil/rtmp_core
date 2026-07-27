# Phase 7 load-generator tests. These prove the generator is a REAL RTMP
# client by validating the bytes it writes to a real loopback socket against
# the production HandshakeSession / ChunkDecoder / AMF0 decoder / media
# classifier — the same "POSIX socket instead of io_uring" substitution
# tests/integration already uses, since the io_uring transport is Linux-only.
add_executable(rtmp_server_load_tests
    loadgen_protocol_test.cpp
)

target_link_libraries(rtmp_server_load_tests PRIVATE
    rtmp_server_loadgen
    rtmp_server_protocol
    rtmp_server_core
    rtmp_server_warnings
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(rtmp_server_load_tests)
