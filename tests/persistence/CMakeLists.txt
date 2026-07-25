add_executable(rtmp_server_persistence_tests
    sqlite_store_test.cpp
)

target_link_libraries(rtmp_server_persistence_tests PRIVATE
    rtmp_server_persistence
    rtmp_server_warnings
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(rtmp_server_persistence_tests)
