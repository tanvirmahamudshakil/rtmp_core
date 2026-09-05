add_executable(rtmp_server_dispatch_tests
    agent_http_client_test.cpp
    transcoder_dispatch_manager_test.cpp
    # http_server.cpp/http_message.cpp are compiled directly here rather than
    # via rtmp_server_control: that library also builds hls_http_handler.cpp,
    # which triggers a libc++ std::atomic<shared_ptr<T>> compile failure on
    # non-Linux dev hosts (a pre-existing, unrelated issue) -- the two files a
    # fake agent actually needs have no such dependency on their own.
    ${CMAKE_SOURCE_DIR}/src/control/http_server.cpp
    ${CMAKE_SOURCE_DIR}/src/control/http_message.cpp
)

target_link_libraries(rtmp_server_dispatch_tests PRIVATE
    rtmp_server_dispatch
    rtmp_server_warnings
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(rtmp_server_dispatch_tests)
