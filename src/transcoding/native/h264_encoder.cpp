#include "rtmp_server/transcoding/native/h264_encoder.hpp"

#include <x264.h>

#include <algorithm>
#include <cstring>

#include "rtmp_server/core/error.hpp"

namespace rtmp_server::transcoding::native {

namespace {

core::Error encode_error(std::string message) {
    return core::Error(core::ErrorCode::Unknown, core::ErrorCategory::Internal, std::move(message));
}

const char* profile_for(int profile) {
    switch (profile) {
        case 0: return "baseline";
        case 1: return "main";
        default: return "high";
    }
}

bool is_keyframe(int slice_type) {
    return slice_type == X264_TYPE_IDR || slice_type == X264_TYPE_I;
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
    x264_param_t param{};
    x264_t* encoder = nullptr;

    ~Impl() {
        if (encoder) x264_encoder_close(encoder);
    }
};

H264Encoder::H264Encoder() : impl_(std::make_unique<Impl>()) {}
H264Encoder::~H264Encoder() = default;

core::Result<void> H264Encoder::open(const H264EncoderConfig& config) {
    // "veryfast" + "zerolatency" is the standard realtime-streaming baseline:
    // zerolatency forces rc-lookahead/sync-lookahead to 0 and bframes to 0, so
    // encode() below returns one access unit per input frame with no reorder
    // buffering — matching the zero-latency contract the openh264 backend had.
    if (x264_param_default_preset(&impl_->param, "veryfast", "zerolatency") != 0) {
        return encode_error("failed to apply libx264 veryfast/zerolatency preset");
    }

    x264_param_t& param = impl_->param;
    param.i_width = static_cast<int>(config.width);
    param.i_height = static_cast<int>(config.height);
    param.i_csp = X264_CSP_I420;
    param.i_fps_num = config.fps;
    param.i_fps_den = 1;
    param.i_keyint_max = static_cast<int>(config.gop);
    param.i_keyint_min = static_cast<int>(config.gop); // fixed GOP for segment alignment
    param.b_intra_refresh = 0;
    // Renditions are long-lived streams; repeat SPS/PPS on every IDR so each
    // HLS/TS segment (which starts on a keyframe) is independently decodable
    // by a player joining mid-stream. Mirrors the CONSTANT_ID rationale the
    // openh264 backend used for the same reason.
    param.b_repeat_headers = 1;
    param.b_annexb = 1;
    param.b_vfr_input = 1;
    param.i_timebase_num = 1;
    param.i_timebase_den = 1000; // pacing clock below runs in milliseconds

    // --- Rate control: CRF anchors perceived quality; the preset's bitrate
    // becomes the VBV ceiling rather than a fixed target. Easy scenes spend
    // fewer bits than the strict-CBR openh264 backend did; hard scenes stay
    // under the advertised cap. This is the actual "same quality, lower
    // bitrate" lever — CABAC (enabled below via profile) is the other one.
    param.rc.i_rc_method = X264_RC_CRF;
    param.rc.f_rf_constant = static_cast<float>(config.crf);
    const int kbit = static_cast<int>(std::max<std::uint32_t>(config.bitrate / 1000, 1));
    param.rc.i_vbv_max_bitrate = kbit;
    param.rc.i_vbv_buffer_size = kbit * 2;

    if (config.threads > 1) {
        // Slice-based (not frame-based) multithreading: distributes one frame
        // across cores instead of pipelining multiple frames, so throughput
        // scales with threads without adding reorder/lookahead latency.
        param.i_threads = static_cast<int>(config.threads);
        param.b_sliced_threads = 1;
    } else {
        param.i_threads = 1;
        param.b_sliced_threads = 0;
    }
    param.i_lookahead_threads = 0;
    param.i_log_level = X264_LOG_NONE;

    if (x264_param_apply_profile(&param, profile_for(config.profile)) != 0) {
        return encode_error("failed to apply libx264 profile");
    }

    impl_->encoder = x264_encoder_open(&param);
    if (impl_->encoder == nullptr) return encode_error("failed to open libx264 encoder");

    width_ = config.width;
    height_ = config.height;
    fps_ = config.fps;
    submitted_frames_ = 0;
    pacing_clock_set_ = false;
    last_media_pts_90k_ = 0;
    pacing_timestamp_ms_ = 0;
    return {};
}

core::Result<void> H264Encoder::encode(const YuvFrame& frame,
                                       std::vector<EncodedAccessUnit>& out) {
    if (impl_->encoder == nullptr) return encode_error("encoder not opened");
    if (frame.width != width_ || frame.height != height_) {
        return encode_error("frame geometry does not match encoder configuration");
    }

    x264_picture_t pic_in;
    x264_picture_init(&pic_in);
    pic_in.img.i_csp = X264_CSP_I420;
    pic_in.img.i_plane = 3;
    pic_in.img.plane[0] = const_cast<std::uint8_t*>(frame.y.data());
    pic_in.img.plane[1] = const_cast<std::uint8_t*>(frame.u.data());
    pic_in.img.plane[2] = const_cast<std::uint8_t*>(frame.v.data());
    pic_in.img.i_stride[0] = frame.y_stride;
    pic_in.img.i_stride[1] = frame.u_stride;
    pic_in.img.i_stride[2] = frame.v_stride;

    // x264 uses this timestamp for VBV pacing. Follow the actual media cadence
    // (see the openh264 backend's original rationale, preserved here): assuming
    // every submitted source is exactly `fps_` made a 60 fps input encoded by a
    // 30 fps job run at roughly twice the configured bitrate. Keep the pacing
    // clock monotonic across source discontinuities instead of passing a large
    // or backwards media timestamp directly.
    const auto nominal_step_ms = std::max<std::int64_t>(1, 1000 / fps_);
    if (!pacing_clock_set_) {
        pacing_clock_set_ = true;
        last_media_pts_90k_ = frame.pts_90k;
        pacing_timestamp_ms_ = 0;
    } else {
        const auto delta_90k = frame.pts_90k - last_media_pts_90k_;
        constexpr std::int64_t kMaximumContinuousGap90k = 10 * 90'000;
        const auto step_ms =
            delta_90k > 0 && delta_90k <= kMaximumContinuousGap90k
                ? std::max<std::int64_t>(1, delta_90k / 90)
                : nominal_step_ms;
        pacing_timestamp_ms_ += static_cast<std::uint64_t>(step_ms);
        last_media_pts_90k_ = frame.pts_90k;
    }
    pic_in.i_pts = static_cast<std::int64_t>(pacing_timestamp_ms_);
    pic_in.i_type = X264_TYPE_AUTO;
    ++submitted_frames_;

    x264_nal_t* nals = nullptr;
    int nal_count = 0;
    x264_picture_t pic_out{};
    const int frame_size = x264_encoder_encode(impl_->encoder, &nals, &nal_count, &pic_in, &pic_out);
    if (frame_size < 0) return encode_error("libx264 x264_encoder_encode failed");
    if (frame_size == 0 || nal_count == 0) {
        return {}; // buffered internally, or nothing to emit for this frame
    }

    // NAL payloads for one encode call live in one contiguous buffer; Annex B
    // start codes are already included (b_annexb = 1).
    std::size_t total = 0;
    for (int n = 0; n < nal_count; ++n) total += static_cast<std::size_t>(nals[n].i_payload);
    if (total == 0) return {};

    EncodedAccessUnit au;
    au.pts_90k = frame.pts_90k;
    au.dts_90k = frame.pts_90k; // no B-frames: DTS == PTS
    au.keyframe = is_keyframe(pic_out.i_type);
    au.annexb.resize(total);
    std::size_t offset = 0;
    for (int n = 0; n < nal_count; ++n) {
        const auto bytes = static_cast<std::size_t>(nals[n].i_payload);
        std::memcpy(au.annexb.data() + offset, nals[n].p_payload, bytes);
        offset += bytes;
    }
    out.push_back(std::move(au));
    return {};
}

} // namespace rtmp_server::transcoding::native
