add_executable(rtmp_server_transcoding_tests
    preset_test.cpp
    native_pipeline_test.cpp
    paced_segment_publisher_test.cpp
    hls_playlist_test.cpp
    rtmp_source_client_test.cpp
    hevc_decoder_test.cpp
    source_job_manager_test.cpp
    codec_tags_test.cpp
)

target_link_libraries(rtmp_server_transcoding_tests PRIVATE
    rtmp_server_transcoding
    GTest::gtest_main
    rtmp_server_warnings
    rtmp_server_sanitizers
)

include(GoogleTest)
gtest_discover_tests(rtmp_server_transcoding_tests)
