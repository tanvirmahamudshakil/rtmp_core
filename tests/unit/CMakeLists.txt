add_executable(rtmp_server_unit_tests
    core/byte_reader_writer_test.cpp
    core/config_test.cpp
    core/random_test.cpp
    core/result_test.cpp
    core/hmac_test.cpp
    observability/audit_log_test.cpp
    observability/metrics_test.cpp
    observability/logger_test.cpp
)

target_link_libraries(rtmp_server_unit_tests PRIVATE
    rtmp_server_core
    rtmp_server_warnings
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(rtmp_server_unit_tests)
