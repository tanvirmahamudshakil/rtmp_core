add_executable(rtmp_server_authentication_tests
    rtmp_authenticator_test.cpp
)

target_link_libraries(rtmp_server_authentication_tests PRIVATE
    rtmp_server_authentication
    rtmp_server_management
    rtmp_server_warnings
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(rtmp_server_authentication_tests)
