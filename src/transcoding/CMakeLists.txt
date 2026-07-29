add_library(rtmp_server_transcoding STATIC
    preset.cpp
    backend.cpp
    supervisor.cpp
)

target_include_directories(rtmp_server_transcoding PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(rtmp_server_transcoding
    PUBLIC
        rtmp_server_core
        rtmp_server_protocol
        rtmp_server_warnings
        rtmp_server_sanitizers
)

target_compile_features(rtmp_server_transcoding PUBLIC cxx_std_23)
