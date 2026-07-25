# Recorder tests use an in-memory FileSink fake (no io_uring, no disk), so the
# recording orchestration — bounded queue, finalize-on-abrupt-disconnect,
# disk-failure non-crash — is verified on every platform including core-only.
add_executable(rtmp_server_recording_tests
    recorder_test.cpp
)

target_link_libraries(rtmp_server_recording_tests PRIVATE
    rtmp_server_media
    rtmp_server_warnings
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(rtmp_server_recording_tests)
