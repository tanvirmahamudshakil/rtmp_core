#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rtmp_server/hls/segmenter.hpp"

namespace rtmp_server::hls {

// Bridges freshly-encoded elementary units (Annex B H.264 + ADTS AAC, as the
// native source transcoder produces) into a Segmenter, which only understands
// FLV-form RtmpMessages. It re-wraps each access unit: Annex B -> AVCC sample
// (plus an AVCDecoderConfigurationRecord synthesised from the first keyframe's
// SPS/PPS), and ADTS -> raw AAC (plus an AudioSpecificConfig from the first
// frame's header). No transcoding happens here — it is pure byte reframing —
// so the existing HLS packaging (segments, playlists, discontinuities) is
// reused unchanged for a transcoded rendition.
class RenditionFeed {
public:
    explicit RenditionFeed(Segmenter& segmenter) : segmenter_(segmenter) {}

    // Feeds one encoded H.264 access unit. Timestamps are on the 90 kHz clock.
    // The sequence header is emitted automatically on the first keyframe that
    // carries SPS and PPS.
    void push_video(std::span<const std::byte> annexb, std::int64_t pts_90k, std::int64_t dts_90k,
                    bool keyframe);

    // Feeds one ADTS-framed AAC access unit. The sequence header is emitted
    // automatically from the first frame's ADTS header.
    void push_audio(std::span<const std::byte> adts, std::int64_t pts_90k);

    // Signals a source gap so the next independently-decodable segment
    // carries EXT-X-DISCONTINUITY without shifting the transcoder's already
    // monotonic output timestamps a second time.
    void mark_discontinuity() {
        segmenter_.mark_media_discontinuity();
        video_config_sent_ = false;
        audio_config_sent_ = false;
        video_sps_.clear();
        video_pps_.clear();
    }

private:
    Segmenter& segmenter_;
    bool video_config_sent_ = false;
    bool audio_config_sent_ = false;
    // The encoder may legitimately replace parameter sets after a restart or
    // reconfiguration. Retain the exact values used for the current AVC
    // decoder configuration so a changed keyframe can refresh it before its
    // slices are forwarded.
    std::vector<std::byte> video_sps_;
    std::vector<std::byte> video_pps_;
};

} // namespace rtmp_server::hls
