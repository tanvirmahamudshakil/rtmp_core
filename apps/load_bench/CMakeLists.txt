# In-process synthetic load generator for the protocol/fan-out layer
# (Phase 9 "load testing"). Pure protocol-layer code — no sockets, no
# io_uring — so it builds and runs on every platform, same as flv_inspector.
add_executable(load_bench main.cpp)

target_link_libraries(load_bench PRIVATE
    rtmp_server_protocol
    rtmp_server_warnings
    rtmp_server_sanitizers
)

target_compile_features(load_bench PUBLIC cxx_std_23)
