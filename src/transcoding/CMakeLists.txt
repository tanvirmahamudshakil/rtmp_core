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
    native/hls_playlist.cpp
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

# The lib-dependent stages of the native pipelines (openh264 decode, libyuv
# scale, x265 HEVC encode, x264 H.264 encode for source-transcode jobs) are
# compiled only when the dependencies are present. Consumers can test the
# RTMP_NATIVE_TRANSCODE compile definition.
if(RTMP_NATIVE_TRANSCODE_AVAILABLE)
    target_sources(rtmp_server_transcoding PRIVATE
        native/h264_decoder.cpp
        native/h264_encoder.cpp
        native/scaler.cpp
        native/hevc_encoder.cpp
        native/video_transcoder.cpp
        native/aac_decoder.cpp
        native/aac_encoder.cpp
        native/audio_transcoder.cpp
        native/source_transcoder.cpp
        native/http_client.cpp
        native/paced_segment_publisher.cpp
        native/hls_source_puller.cpp
        native/source_job_manager.cpp
    )
    target_link_libraries(rtmp_server_transcoding
        PUBLIC
            rtmp_server_hls
            PkgConfig::X265
            PkgConfig::X264
            PkgConfig::OPENH264
            PkgConfig::FDKAAC
            PkgConfig::LIBCURL
            ${RTMP_LIBYUV_TARGET}
    )
    target_compile_definitions(rtmp_server_transcoding PUBLIC RTMP_NATIVE_TRANSCODE=1)
endif()
