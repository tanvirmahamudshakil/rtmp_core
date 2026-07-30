#include "rtmp_server/transcoding/native/h264_encoder.hpp"

#include <wels/codec_api.h>
#include <wels/codec_app_def.h>

#include <algorithm>
#include <cstring>

#include "rtmp_server/core/error.hpp"

namespace rtmp_server::transcoding::native {

namespace {

core::Error encode_error(std::string message) {
    return core::Error(core::ErrorCode::Unknown, core::ErrorCategory::Internal, std::move(message));
}

EProfileIdc profile_for(int profile) {
    switch (profile) {
        case 0: return PRO_BASELINE;
        case 1: return PRO_MAIN;
        default: return PRO_HIGH;
    }
}

} // namespace

H264EncoderConfig build_h264_config(std::uint32_t out_w, std::uint32_t out_h, std::uint32_t fps,
                                    std::uint32_t bitrate, std::uint32_t gop, std::uint32_t threads) {
    H264EncoderConfig config;
    config.width = std::max<std::uint32_t>(out_w, 2);
    config.height = std::max<std::uint32_t>(out_h, 2);
    config.fps = std::max<std::uint32_t>(fps, 1);
    config.bitrate = std::max<std::uint32_t>(bitrate, 100'000);
    config.gop = std::max<std::uint32_t>(gop, 1);
    config.threads = std::clamp<std::uint32_t>(threads, 1, 8);
    return config;
}

struct H264Encoder::Impl {
    ISVCEncoder* encoder = nullptr;

    ~Impl() {
        if (encoder) {
            encoder->Uninitialize();
            WelsDestroySVCEncoder(encoder);
        }
    }
};

H264Encoder::H264Encoder() : impl_(std::make_unique<Impl>()) {}
H264Encoder::~H264Encoder() = default;

core::Result<void> H264Encoder::open(const H264EncoderConfig& config) {
    if (WelsCreateSVCEncoder(&impl_->encoder) != 0 || impl_->encoder == nullptr) {
        return encode_error("failed to create openh264 encoder");
    }

    SEncParamExt param{};
    impl_->encoder->GetDefaultParams(&param);
    param.iUsageType = CAMERA_VIDEO_REAL_TIME;
    param.iPicWidth = static_cast<int>(config.width);
    param.iPicHeight = static_cast<int>(config.height);
    param.iTargetBitrate = static_cast<int>(config.bitrate);
    param.iMaxBitrate = static_cast<int>(config.bitrate);
    param.iRCMode = RC_BITRATE_MODE;
    param.fMaxFrameRate = static_cast<float>(config.fps);
    param.iComplexityMode = LOW_COMPLEXITY;
    param.uiIntraPeriod = config.gop;
    param.bEnableFrameSkip = config.allow_frame_skip;
    param.iMultipleThreadIdc = static_cast<unsigned short>(config.threads);
    param.bEnableDenoise = false;
    param.bEnableBackgroundDetection = true;
    param.bEnableSceneChangeDetect = false; // fixed GOP for segment alignment
    param.iSpatialLayerNum = 1;
    param.iTemporalLayerNum = 1;
    // Renditions are long-lived streams. Keep SPS/PPS identifiers stable
    // across IDRs so every slice continues to reference the parameter sets
    // cached by HLS players. Some openh264 builds default to incrementing the
    // PPS id on every IDR, which makes a stream undecodable if a downstream
    // bridge retains the original decoder configuration.
    param.eSpsPpsIdStrategy = CONSTANT_ID;

    SSpatialLayerConfig& layer = param.sSpatialLayers[0];
    layer.iVideoWidth = static_cast<int>(config.width);
    layer.iVideoHeight = static_cast<int>(config.height);
    layer.fFrameRate = static_cast<float>(config.fps);
    layer.iSpatialBitrate = static_cast<int>(config.bitrate);
    layer.iMaxSpatialBitrate = static_cast<int>(config.bitrate);
    layer.uiProfileIdc = profile_for(config.profile);
    // Single slice per frame: lowest latency, simplest Annex B output.
    layer.sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;

    if (impl_->encoder->InitializeExt(&param) != 0) {
        return encode_error("failed to initialize openh264 encoder");
    }
    int data_format = videoFormatI420;
    impl_->encoder->SetOption(ENCODER_OPTION_DATAFORMAT, &data_format);

    width_ = config.width;
    height_ = config.height;
    fps_ = config.fps;
    submitted_frames_ = 0;
    return {};
}

core::Result<void> H264Encoder::encode(const YuvFrame& frame,
                                       std::vector<EncodedAccessUnit>& out) {
    if (impl_->encoder == nullptr) return encode_error("encoder not opened");
    if (frame.width != width_ || frame.height != height_) {
        return encode_error("frame geometry does not match encoder configuration");
    }

    SSourcePicture pic{};
    pic.iPicWidth = static_cast<int>(width_);
    pic.iPicHeight = static_cast<int>(height_);
    pic.iColorFormat = videoFormatI420;
    pic.iStride[0] = frame.y_stride;
    pic.iStride[1] = frame.u_stride;
    pic.iStride[2] = frame.v_stride;
    pic.pData[0] = const_cast<std::uint8_t*>(frame.y.data());
    pic.pData[1] = const_cast<std::uint8_t*>(frame.u.data());
    pic.pData[2] = const_cast<std::uint8_t*>(frame.v.data());
    // openh264's source-picture timestamp is a millisecond pacing clock used
    // by rate control; it is not the output media PTS. Drive it from the
    // configured frame cadence so a discontinuity/reconnect in the MPEG 90 kHz
    // media timeline cannot look like a multi-minute input gap and provoke a
    // burst of skipped frames. The original PTS is retained on the emitted AU
    // below.
    pic.uiTimeStamp = static_cast<long long>(submitted_frames_ * 1000ULL / fps_);
    ++submitted_frames_;

    SFrameBSInfo info{};
    const int rc = impl_->encoder->EncodeFrame(&pic, &info);
    if (rc != cmResultSuccess) return encode_error("openh264 EncodeFrame failed");
    if (info.eFrameType == videoFrameTypeSkip || info.eFrameType == videoFrameTypeInvalid) {
        return {}; // frame dropped by rate control, or nothing to emit
    }

    // Concatenate every NAL of every layer into one Annex B access unit; the
    // openh264 bitstream is already start-code prefixed.
    std::size_t total = 0;
    for (int l = 0; l < info.iLayerNum; ++l) {
        const SLayerBSInfo& layer = info.sLayerInfo[l];
        for (int n = 0; n < layer.iNalCount; ++n) total += static_cast<std::size_t>(layer.pNalLengthInByte[n]);
    }
    if (total == 0) return {};

    EncodedAccessUnit au;
    au.pts_90k = frame.pts_90k;
    au.dts_90k = frame.pts_90k; // no B-frames: DTS == PTS
    au.keyframe = info.eFrameType == videoFrameTypeIDR || info.eFrameType == videoFrameTypeI;
    au.annexb.resize(total);
    std::size_t offset = 0;
    for (int l = 0; l < info.iLayerNum; ++l) {
        const SLayerBSInfo& layer = info.sLayerInfo[l];
        std::size_t layer_bytes = 0;
        for (int n = 0; n < layer.iNalCount; ++n) layer_bytes += static_cast<std::size_t>(layer.pNalLengthInByte[n]);
        std::memcpy(au.annexb.data() + offset, layer.pBsBuf, layer_bytes);
        offset += layer_bytes;
    }
    out.push_back(std::move(au));
    return {};
}

} // namespace rtmp_server::transcoding::native
