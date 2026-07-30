# Phase 6 HLS packaging: playlist generation, the segmenter state machine and
# the bounded in-memory segment store. Pure CPU/buffer code over the media
# layer's TS muxer and codec helpers — no sockets, no io_uring, no disk — so
# it builds on every platform including RTMP_SERVER_CORE_ONLY. HTTP delivery
# lives in rtmp_server_control (the existing Phase 5 server), not here.
add_library(rtmp_server_hls STATIC
    playlist.cpp
    segmenter.cpp
    segment_store.cpp
    stream_sink.cpp
    rendition_feed.cpp
)

target_include_directories(rtmp_server_hls PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(rtmp_server_hls
    PUBLIC
        rtmp_server_core
        rtmp_server_protocol
        rtmp_server_media
        rtmp_server_warnings
        rtmp_server_sanitizers
)

target_compile_features(rtmp_server_hls PUBLIC cxx_std_23)
