add_executable(rtmp_server_relay_tests
    rtmp_push_client_test.cpp
    stream_target_test.cpp
    backup_publisher_test.cpp
)

target_link_libraries(rtmp_server_relay_tests PRIVATE
    rtmp_server_relay
    rtmp_server_warnings
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(rtmp_server_relay_tests)
