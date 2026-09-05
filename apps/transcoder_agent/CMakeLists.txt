add_executable(transcoder_agent main.cpp)

target_link_libraries(transcoder_agent PRIVATE
    rtmp_server_core
    rtmp_server_control
    rtmp_server_dispatch
    rtmp_server_media
    rtmp_server_relay
    rtmp_server_transcoding
    rtmp_server_warnings
    rtmp_server_sanitizers
)

target_compile_features(transcoder_agent PUBLIC cxx_std_23)
