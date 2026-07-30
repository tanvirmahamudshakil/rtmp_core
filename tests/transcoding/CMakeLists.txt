add_executable(rtmp_server_transcoding_tests
    preset_test.cpp
    supervisor_test.cpp
    native_pipeline_test.cpp
    hls_playlist_test.cpp
)

target_link_libraries(rtmp_server_transcoding_tests PRIVATE
    rtmp_server_transcoding
    GTest::gtest_main
    rtmp_server_warnings
    rtmp_server_sanitizers
)

include(GoogleTest)
gtest_discover_tests(rtmp_server_transcoding_tests)
