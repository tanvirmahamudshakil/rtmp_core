# FLV container encoding (media/flv) and the recording orchestration
# (recording/) are pure byte-format + bookkeeping code with no sockets and no
# liburing, so they build on every platform including under
# RTMP_SERVER_CORE_ONLY — same as rtmp_server_protocol. The io_uring-backed
# FileSink implementation is the only piece that needs Linux; it lives in the
# io_uring target (src/io/io_uring) behind the Linux gate, not here.
# Phase 6 adds: the codec helpers HLS passthrough needs (h264 AVCC->Annex B,
# aac ADTS framing), the MPEG-TS muxer, the portable async recording sink
# (dedicated writer thread + bounded queue — POSIX only, no io_uring) and
# recording retention. All are platform-independent, so recording and HLS
# packaging are fully available under RTMP_SERVER_CORE_ONLY.
add_library(rtmp_server_media STATIC
    flv/flv_writer.cpp
    h264/avc.cpp
    h264/sps.cpp
    hevc/sps.cpp
    aac/adts.cpp
    mp4/fmp4_muxer.cpp
    ts/ts_muxer.cpp
    ts/ts_demuxer.cpp
    ${CMAKE_SOURCE_DIR}/src/recording/recorder.cpp
    ${CMAKE_SOURCE_DIR}/src/recording/async_file_sink.cpp
    ${CMAKE_SOURCE_DIR}/src/recording/retention.cpp
    ${CMAKE_SOURCE_DIR}/src/recording/recording_path.cpp
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
