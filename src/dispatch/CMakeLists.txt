# Origin-side transcoder-tier placement/dispatch: decides which transcoder
# node runs a job and talks to it over a small hand-rolled HTTP client. No
# codec-library dependency and no io_uring, so this builds and is tested on
# every platform including RTMP_SERVER_CORE_ONLY -- a transcoder node needs
# the native codec libraries (apps/transcoder_agent), but the origin
# dispatching to it never does.
add_library(rtmp_server_dispatch STATIC
    agent_http_client.cpp
    transcoder_dispatch_manager.cpp
)

target_include_directories(rtmp_server_dispatch PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(rtmp_server_dispatch
    PUBLIC
        rtmp_server_core
        rtmp_server_cluster
        rtmp_server_hls
        rtmp_server_persistence
        rtmp_server_transcoding
        rtmp_server_warnings
        rtmp_server_sanitizers
)

target_compile_features(rtmp_server_dispatch PUBLIC cxx_std_23)
