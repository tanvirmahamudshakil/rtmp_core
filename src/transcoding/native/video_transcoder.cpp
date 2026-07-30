#include "rtmp_server/transcoding/native/video_transcoder.hpp"

#include "rtmp_server/core/error.hpp"
#include "rtmp_server/transcoding/native/geometry.hpp"

namespace rtmp_server::transcoding::native {

namespace {

core::Error pipeline_error(std::string message) {
    return core::Error(core::ErrorCode::Unknown, core::ErrorCategory::Internal, std::move(message));
}

// RTMP timestamps are milliseconds; the TS muxer and x265 run on the 90 kHz
// clock. 90000 / 1000 = 90 ticks per millisecond.
std::int64_t ms_to_90k(std::int64_t ms) { return ms * 90; }

} // namespace

NativeVideoTranscoder::NativeVideoTranscoder(VideoTranscoderConfig config)
    : config_(std::move(config)) {}

core::Result<void> NativeVideoTranscoder::set_decoder_config(std::span<const std::byte> record) {
    auto parsed = media::h264::parse_decoder_config(record);
    if (!parsed) return parsed.error();
    decoder_config_ = std::move(parsed).value();
    if (!decoder_config_.valid()) return pipeline_error("AVC decoder config missing SPS/PPS");
    if (!decoder_open_) {
        if (auto init = decoder_.initialize(); !init) return init.error();
        decoder_open_ = true;
    }
    return {};
}

core::Result<void> NativeVideoTranscoder::ensure_encoder(std::uint32_t src_w, std::uint32_t src_h) {
    if (encoder_open_) return {};
    const ScalePlan plan = compute_scale_plan(config_.preset, src_w, src_h);
    const HevcParamSet params = build_hevc_param_set(config_.preset, config_.quality, plan.out_w,
                                                     plan.out_h, config_.fps_num, config_.fps_den);
    if (auto opened = encoder_.open(params); !opened) return opened.error();
    encoder_open_ = true;
    return {};
}

core::Result<void> NativeVideoTranscoder::transcode_sample(std::span<const std::byte> avcc_sample,
                                                           std::int64_t dts_ms, std::int32_t cts_ms,
                                                           bool keyframe,
                                                           std::vector<EncodedAccessUnit>& out) {
    if (!decoder_open_ || !decoder_config_.valid()) {
        return pipeline_error("received a sample before the AVC decoder config");
    }

    // Convert AVCC -> Annex B. Prefixing SPS/PPS on keyframes guarantees the
    // decoder always has parameter sets at each IDR even after a mid-stream join.
    std::vector<std::byte> annexb;
    if (auto conv = media::h264::avcc_to_annexb(avcc_sample, decoder_config_, keyframe, annexb);
        !conv) {
        return conv.error();
    }

    const std::int64_t pts_90k = ms_to_90k(dts_ms + cts_ms);
    bool produced = false;
    if (auto dec = decoder_.decode(annexb, pts_90k, decoded_, produced); !dec) return dec.error();
    if (!produced) return {};

    if (auto ready = ensure_encoder(decoded_.width, decoded_.height); !ready) return ready.error();

    const ScalePlan plan = compute_scale_plan(config_.preset, decoded_.width, decoded_.height);
    if (auto scaled = scaler_.scale(decoded_, plan, scaled_); !scaled) return scaled.error();

    return encoder_.encode(scaled_, out);
}

core::Result<void> NativeVideoTranscoder::finish(std::vector<EncodedAccessUnit>& out) {
    if (!encoder_open_) return {};
    return encoder_.flush(out);
}

} // namespace rtmp_server::transcoding::native
