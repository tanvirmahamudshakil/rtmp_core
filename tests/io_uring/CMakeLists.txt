# Phase 4 (docs/v2_promot.md "Multi-core io_uring worker architecture")
# tests: these link rtmp_server_io_uring (liburing, eventfd), so — like the
# io_uring target itself — they only build on Linux and are excluded from the
# core-only macOS dev configuration (see top-level CMakeLists.txt).
add_executable(rtmp_server_io_uring_tests
    cross_worker_router_test.cpp
    worker_pool_test.cpp
)

target_link_libraries(rtmp_server_io_uring_tests PRIVATE
    rtmp_server_io_uring
    rtmp_server_warnings
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(rtmp_server_io_uring_tests)
