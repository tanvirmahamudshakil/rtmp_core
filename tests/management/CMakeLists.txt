add_executable(rtmp_server_management_tests
    url_builder_test.cpp
    token_test.cpp
    stream_manager_test.cpp
    authorization_cache_test.cpp
    stream_manager_persistence_test.cpp
)

target_link_libraries(rtmp_server_management_tests PRIVATE
    rtmp_server_management
    rtmp_server_persistence
    rtmp_server_warnings
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(rtmp_server_management_tests)
