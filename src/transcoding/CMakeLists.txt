add_library(rtmp_server_transcoding STATIC
    preset.cpp
    backend.cpp
    supervisor.cpp
    # Pure geometry + x265 parameter mapping. No codec libraries required, so
    # these always compile and stay unit-testable even without the native
    # pipeline enabled.
    native/geometry.cpp
    native/hevc_params.cpp
    native/aac_params.cpp
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

# The lib-dependent stages of the native CPU HEVC pipeline (openh264 decode,
# libyuv scale, x265 encode) are compiled only when the dependencies are
# present. Consumers can test RTMP_NATIVE_TRANSCODE compile definition.
if(RTMP_NATIVE_TRANSCODE_AVAILABLE)
    target_sources(rtmp_server_transcoding PRIVATE
        native/h264_decoder.cpp
        native/scaler.cpp
        native/hevc_encoder.cpp
        native/video_transcoder.cpp
        native/aac_decoder.cpp
        native/aac_encoder.cpp
        native/audio_transcoder.cpp
    )
    target_link_libraries(rtmp_server_transcoding
        PUBLIC
            PkgConfig::X265
            PkgConfig::OPENH264
            PkgConfig::FDKAAC
            ${RTMP_LIBYUV_TARGET}
    )
    target_compile_definitions(rtmp_server_transcoding PUBLIC RTMP_NATIVE_TRANSCODE=1)
endif()
