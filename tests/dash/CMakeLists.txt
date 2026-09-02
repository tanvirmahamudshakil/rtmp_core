# MPEG-DASH tests: pure MPD string generation, the fMP4 segmenter state
# machine and the bounded segment store, plus HTTP delivery -- mirrors
# tests/hls's structure.
add_executable(rtmp_server_dash_tests
    mpd_test.cpp
    segmenter_test.cpp
    segment_store_test.cpp
    dash_http_test.cpp
)

target_include_directories(rtmp_server_dash_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(rtmp_server_dash_tests PRIVATE
    rtmp_server_dash
    rtmp_server_control
    rtmp_server_media
    rtmp_server_warnings
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(rtmp_server_dash_tests)
