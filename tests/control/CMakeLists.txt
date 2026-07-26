add_executable(rtmp_server_control_tests
    http_server_test.cpp
    management_api_test.cpp
)

target_link_libraries(rtmp_server_control_tests PRIVATE
    rtmp_server_control
    rtmp_server_management
    rtmp_server_persistence
    rtmp_server_warnings
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(rtmp_server_control_tests)
