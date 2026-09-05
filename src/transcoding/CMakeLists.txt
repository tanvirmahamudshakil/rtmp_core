add_library(rtmp_server_transcoding STATIC
    preset.cpp
    # Pure geometry + x265 parameter mapping. No codec libraries required, so
    # these always compile and stay unit-testable even without the native
    # pipeline enabled.
    native/geometry.cpp
    native/hevc_params.cpp
    native/aac_params.cpp
    native/hls_playlist.cpp
    # Pure CPU-budget allocation for a source rendition ladder. Kept outside
    # the codec-library gate so oversubscription policy is unit-tested even on
    # development hosts without libyuv/x264.
    native/source_thread_budget.cpp
    # Pure RFC 6381 CODECS derivation for the master playlist. Depends only
    # on aac_params above, so it compiles and is tested without the codec
    # libraries even though what it describes is produced by them.
    native/codec_tags.cpp
    # FLV/RTMP tag -> Annex B + ADTS conversion. Pure byte reframing over the
    # media layer's codec helpers, so it also builds without the encoders.
    native/rtmp_tag_converter.cpp
    # The native RTMP playback client (DNS/TCP, handshake, connect/
    # createStream/play, chunk/AMF0, ack/ping). Pure sockets + protocol, no
    # codec libraries -- moved out of the codec-gated block below so backup
    # publisher failover (src/relay/backup_publisher.cpp), which needs only
    # this client and no transcoding, works on every host including one
    # without libx264/x265/openh264 installed.
    native/rtmp_source_client.cpp
)

target_include_directories(rtmp_server_transcoding PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(rtmp_server_transcoding
    PUBLIC
        rtmp_server_core
        rtmp_server_protocol
        rtmp_server_hls
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
        native/hevc_decoder.cpp
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
        # Transcoding of streams published to this origin: the RecorderSink
        # that turns one publish into a rendition ladder, and the assignment
        # manager that decides which publishes get one.
        native/ingest_transcoder.cpp
    )
    target_link_libraries(rtmp_server_transcoding
        PUBLIC
            rtmp_server_hls
            PkgConfig::X265
            PkgConfig::X264
            PkgConfig::OPENH264
            PkgConfig::LIBAVCODEC
            PkgConfig::LIBAVUTIL
            PkgConfig::FDKAAC
            PkgConfig::LIBCURL
            PkgConfig::LIBDE265
            ${RTMP_LIBYUV_TARGET}
    )
    target_compile_definitions(rtmp_server_transcoding PUBLIC RTMP_NATIVE_TRANSCODE=1)
endif()
