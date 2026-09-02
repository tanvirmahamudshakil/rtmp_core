# MPEG-DASH packaging: MPD generation, the fMP4 segmenter state machine and
# the bounded in-memory segment store. Mirrors src/hls's structure and
# platform posture exactly: pure CPU/buffer code over the media layer's fMP4
# muxer and codec helpers -- no sockets, no io_uring, no disk -- so it builds
# on every platform including RTMP_SERVER_CORE_ONLY. HTTP delivery lives in
# rtmp_server_control, not here.
add_library(rtmp_server_dash STATIC
    mpd.cpp
    segmenter.cpp
    segment_store.cpp
)

target_include_directories(rtmp_server_dash PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(rtmp_server_dash
    PUBLIC
        rtmp_server_core
        rtmp_server_protocol
        rtmp_server_media
        rtmp_server_warnings
        rtmp_server_sanitizers
)

target_compile_features(rtmp_server_dash PUBLIC cxx_std_23)
