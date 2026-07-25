# FLV container encoding (media/flv) and the recording orchestration
# (recording/) are pure byte-format + bookkeeping code with no sockets and no
# liburing, so they build on every platform including under
# RTMP_SERVER_CORE_ONLY — same as rtmp_server_protocol. The io_uring-backed
# FileSink implementation is the only piece that needs Linux; it lives in the
# io_uring target (src/io/io_uring) behind the Linux gate, not here.
add_library(rtmp_server_media STATIC
    flv/flv_writer.cpp
    ${CMAKE_SOURCE_DIR}/src/recording/recorder.cpp
)

target_include_directories(rtmp_server_media PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(rtmp_server_media
    PUBLIC
        rtmp_server_core
        rtmp_server_protocol
        rtmp_server_warnings
        rtmp_server_sanitizers
)

target_compile_features(rtmp_server_media PUBLIC cxx_std_23)
