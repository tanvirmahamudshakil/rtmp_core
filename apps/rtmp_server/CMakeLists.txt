add_executable(rtmp_server main.cpp)

target_link_libraries(rtmp_server PRIVATE
    rtmp_server_core
    rtmp_server_protocol
    rtmp_server_io_uring
    rtmp_server_management
    rtmp_server_authentication
    rtmp_server_persistence
    rtmp_server_control
    rtmp_server_warnings
    rtmp_server_sanitizers
)

target_compile_features(rtmp_server PUBLIC cxx_std_23)
