# TcpConnection (src/network) and IoUringEventLoop (src/io/io_uring) call
# into each other concretely (TcpConnection::start_read -> loop.submit_receive,
# event loop -> connection->on_receive_completion), so they are compiled into
# one CMake target to avoid a circular static-library dependency. The header
# split (include/rtmp_server/network vs include/rtmp_server/io/io_uring)
# still enforces that everything above IAsyncTransport only ever includes
# rtmp_server/network/async_transport.hpp, never liburing.h directly.
add_library(rtmp_server_io_uring STATIC
    capabilities.cpp
    context.cpp
    event_loop.cpp
    ${CMAKE_SOURCE_DIR}/src/network/tcp_connection.cpp
)

target_include_directories(rtmp_server_io_uring PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(rtmp_server_io_uring
    PUBLIC
        rtmp_server_core
        rtmp_server_protocol
        rtmp_server_warnings
        rtmp_server_sanitizers
        liburing::liburing
)

target_compile_features(rtmp_server_io_uring PUBLIC cxx_std_23)
