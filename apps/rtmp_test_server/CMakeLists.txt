# TEST-ONLY poll(2)-based RTMP front-end for the Phase 7 load generator.
# See main.cpp's header comment: this is NOT the production server
# (apps/rtmp_server / io_uring, Linux-only) and must not be deployed.
# It exists so real socket-level load measurements are possible on hosts
# where the io_uring transport cannot be built.
add_executable(rtmp_test_server main.cpp)

target_link_libraries(rtmp_test_server PRIVATE
    rtmp_server_protocol
    rtmp_server_core
    rtmp_server_warnings
    rtmp_server_sanitizers
)

target_compile_features(rtmp_test_server PUBLIC cxx_std_23)
