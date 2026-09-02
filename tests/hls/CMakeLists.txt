# Phase 6 HLS tests. No real player or ffprobe is available on this host
# (see docs/phase-6-report.md), so playlist_test.cpp carries a strict RFC 8216
# structural validator and ts_muxer_test.cpp re-derives the MPEG-TS framing
# from the emitted bytes, including PSI CRC verification.
add_executable(rtmp_server_hls_tests
    codec_test.cpp
    ts_muxer_test.cpp
    ts_demuxer_test.cpp
    rendition_feed_test.cpp
    playlist_test.cpp
    low_latency_test.cpp
    encryption_test.cpp
    segmenter_test.cpp
    segment_store_test.cpp
    hls_http_test.cpp
)

target_include_directories(rtmp_server_hls_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(rtmp_server_hls_tests PRIVATE
    rtmp_server_hls
    rtmp_server_control
    rtmp_server_media
    rtmp_server_management
    rtmp_server_warnings
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(rtmp_server_hls_tests)
