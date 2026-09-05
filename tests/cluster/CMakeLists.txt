add_executable(rtmp_server_cluster_tests
    node_registry_test.cpp
)

target_link_libraries(rtmp_server_cluster_tests PRIVATE
    rtmp_server_cluster
    rtmp_server_warnings
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(rtmp_server_cluster_tests)
