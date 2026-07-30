#include "rtmp_server/transcoding/native/audio_transcoder.hpp"

#include "rtmp_server/core/error.hpp"

namespace rtmp_server::transcoding::native {

namespace {

core::Error audio_error(std::string message) {
    return core::Error(core::ErrorCode::Unknown, core::ErrorCategory::Internal, std::move(message));
}

// The MPEG-TS 90 kHz presentation clock.
constexpr std::uint64_t kClockHz = 90000;

std::int64_t ms_to_90k(std::int64_t ms) { return ms * 90; }

} // namespace

NativeAudioTranscoder::NativeAudioTranscoder(Preset preset, AacQualityOptions quality)
    : preset_(std::move(preset)), quality_(quality) {}

core::Result<void> NativeAudioTranscoder::set_sequence_header(std::span<const std::byte> asc) {
    if (auto configured = decoder_.configure(asc); !configured) return configured.error();
    configured_ = true;
    return {};
}

core::Result<void> NativeAudioTranscoder::ensure_encoder(std::uint32_t sample_rate,
                                                         std::uint32_t channels) {
    if (encoder_open_) return {};
    const AacParamSet params = build_aac_param_set(preset_, quality_, sample_rate, channels);
    if (auto opened = encoder_.open(params); !opened) return opened.error();
    sample_rate_ = encoder_.sample_rate();
    encoder_open_ = true;
    return {};
}

void NativeAudioTranscoder::emit(const std::vector<EncodedAudioFrame>& frames,
                                 std::vector<AudioAccessUnit>& out) {
    for (const auto& frame : frames) {
        AudioAccessUnit au;
        au.adts = frame.adts;
        au.pts_90k = base_pts_90k_ +
                     static_cast<std::int64_t>(emitted_samples_ * kClockHz / sample_rate_);
        emitted_samples_ += frame.samples_per_channel;
        out.push_back(std::move(au));
    }
}

core::Result<void> NativeAudioTranscoder::transcode_frame(std::span<const std::byte> aac_frame,
                                                          std::int64_t dts_ms,
                                                          std::vector<AudioAccessUnit>& out) {
    if (!configured_) return audio_error("received an audio frame before the sequence header");
    if (!base_pts_set_) {
        base_pts_90k_ = ms_to_90k(dts_ms);
        base_pts_set_ = true;
    }

    bool produced = false;
    if (auto decoded = decoder_.decode(aac_frame, pcm_, produced); !decoded) return decoded.error();
    if (!produced) return {};

    if (auto ready = ensure_encoder(pcm_.sample_rate, pcm_.channels); !ready) return ready.error();

    std::vector<EncodedAudioFrame> frames;
    if (auto encoded = encoder_.encode(pcm_, frames); !encoded) return encoded.error();
    emit(frames, out);
    return {};
}

core::Result<void> NativeAudioTranscoder::finish(std::vector<AudioAccessUnit>& out) {
    if (!encoder_open_) return {};
    std::vector<EncodedAudioFrame> frames;
    if (auto flushed = encoder_.flush(frames); !flushed) return flushed.error();
    emit(frames, out);
    return {};
}

} // namespace rtmp_server::transcoding::native
