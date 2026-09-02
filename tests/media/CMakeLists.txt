# FLV encoder/parser tests — pure byte-format code, no io_uring, so they build
# and run on every platform (including core-only), same as the protocol tests.
add_executable(rtmp_server_media_tests
    flv_writer_test.cpp
    sps_test.cpp
    fmp4_muxer_test.cpp
)

target_link_libraries(rtmp_server_media_tests PRIVATE
    rtmp_server_media
    rtmp_server_warnings
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(rtmp_server_media_tests)
