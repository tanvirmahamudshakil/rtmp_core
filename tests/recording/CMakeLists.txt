# Recorder tests use an in-memory FileSink fake (no io_uring, no disk), so the
# recording orchestration — bounded queue, finalize-on-abrupt-disconnect,
# disk-failure non-crash — is verified on every platform including core-only.
# Phase 6 adds async_file_sink_test.cpp (the portable writer-thread sink:
# atomic finalize, bounded queue, disk-space failure policy, and the executed
# proof that no disk I/O happens on the caller/media thread) and
# retention_test.cpp. These use real temporary files, which is portable.
add_executable(rtmp_server_recording_tests
    recorder_test.cpp
    async_file_sink_test.cpp
    retention_test.cpp
    recording_path_test.cpp
)

target_link_libraries(rtmp_server_recording_tests PRIVATE
    rtmp_server_media
    rtmp_server_warnings
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(rtmp_server_recording_tests)
