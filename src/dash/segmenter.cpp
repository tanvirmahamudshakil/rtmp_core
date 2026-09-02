#include "rtmp_server/dash/segmenter.hpp"

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace rtmp_server::dash {

namespace {

std::span<const std::byte> payload_span(const protocol::chunk::RtmpMessage& message) {
    return std::span<const std::byte>(message.payload.data(), message.payload.size());
}

bool same_bytes(const std::vector<std::vector<std::byte>>& a,
                const std::vector<std::vector<std::byte>>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

} // namespace

Segmenter::Segmenter(SegmentCallback on_segment, InitCallback on_init, SegmenterConfig config)
    : on_segment_(std::move(on_segment)), on_init_(std::move(on_init)), config_(config),
      next_sequence_(config.initial_sequence) {}

std::uint64_t Segmenter::to_90k(std::int64_t milliseconds) const {
    const std::int64_t shifted = milliseconds + timeline_base_ms_;
    const std::int64_t clamped = shifted < 0 ? 0 : shifted;
    return static_cast<std::uint64_t>(clamped) * 90ULL;
}

std::string Segmenter::codecs_attribute() const {
    std::string codecs;
    if (video_config_ && !video_decoder_config_raw_.empty()) {
        codecs = media::mp4::video_codec_string(media::mp4::VideoCodec::H264, video_decoder_config_raw_);
    }
    if (audio_config_) {
        if (!codecs.empty()) codecs += ",";
        codecs += media::mp4::audio_codec_string(*audio_config_);
    }
    return codecs;
}

void Segmenter::rebuild_init_segment() {
    if (!video_config_ && !audio_config_) return;

    media::mp4::Fmp4InitConfig config;
    if (video_config_) {
        config.video_codec = media::mp4::VideoCodec::H264;
        config.video_decoder_config = video_decoder_config_raw_;
        config.video_dimensions = video_dimensions_;
    }
    if (audio_config_) {
        config.has_audio = true;
        config.audio_config = *audio_config_;
        config.audio_specific_config = audio_specific_config_raw_;
    }

    auto built = muxer_.init_segment(config);
    if (!built.ok()) {
        // A malformed but internally-consistent config (e.g. a reserved AAC
        // sampling index) should not be possible past the parsers that
        // populated video_config_/audio_config_, but if it ever happens the
        // safe response is to keep serving the previous init segment rather
        // than publish a broken one no player can initialise from.
        return;
    }

    ++init_epoch_;
    auto init = std::make_shared<InitSegment>();
    init->data = core::SharedBuffer::adopt(std::move(built).value());
    init->epoch = init_epoch_;
    stats_.init_segments_produced += 1;
    if (on_init_) on_init_(init);
}

void Segmenter::rebuild_sample_spans(Track& track) {
    // owned_data may have reallocated (a vector<vector<byte>> push_back can
    // move every inner vector, but each inner vector's own buffer is stable
    // once it exists — the outer vector's storage is what moves). Rebuild
    // every span from the current addresses regardless, since this is only
    // called right after a push and is O(segment sample count), not O(n^2)
    // across the whole segment's life.
    for (std::size_t i = 0; i < track.samples.size(); ++i) {
        track.samples[i].data = std::span<const std::byte>(track.owned_data[i]);
    }
}

void Segmenter::commit_pending(Track& track, std::uint32_t duration_90k) {
    if (!track.pending.has) return;

    if (!track.base_set) {
        track.base_decode_time_90k = to_90k(track.pending.timestamp_ms);
        track.base_set = true;
    }

    media::mp4::Fmp4Sample sample;
    sample.duration = duration_90k;
    sample.composition_offset =
        static_cast<std::int32_t>(track.pending.composition_time_ms) * 90;
    sample.keyframe = track.pending.keyframe;

    track.owned_data.push_back(std::move(track.pending.data));
    track.samples.push_back(sample);
    rebuild_sample_spans(track);

    track.last_duration_90k = duration_90k;
    track.pending = PendingSample{};
}

void Segmenter::close_track_tail(Track& track) {
    if (!track.pending.has) return;
    // No next sample arrived to derive a real duration from. Carry forward
    // the previous inter-sample duration (the common case: a live stream cut
    // mid-cadence), or fall back to the configured target divided evenly —
    // never zero, which would collapse the sample's presentation time into
    // the next one's.
    std::uint32_t duration = track.last_duration_90k;
    if (duration == 0) {
        duration = static_cast<std::uint32_t>(
            std::max<std::int64_t>(1, config_.target_duration.count()) * 90);
    }
    commit_pending(track, duration);
}

void Segmenter::flush_segment() {
    close_track_tail(video_track_);
    close_track_tail(audio_track_);

    if (video_track_.empty() && audio_track_.empty()) {
        segment_open_ = false;
        return;
    }

    media::mp4::Fmp4TrackFragment video_fragment;
    if (!video_track_.empty()) {
        video_fragment.base_decode_time = video_track_.base_decode_time_90k;
        video_fragment.samples = video_track_.samples;
    }
    media::mp4::Fmp4TrackFragment audio_fragment;
    if (!audio_track_.empty()) {
        audio_fragment.base_decode_time = audio_track_.base_decode_time_90k;
        audio_fragment.samples = audio_track_.samples;
    }

    std::vector<std::byte> out;
    media::mp4::Fmp4Muxer::write_styp(out);
    const auto audio_timescale = audio_config_ ? audio_config_->sample_rate() : 0;
    auto result = muxer_.write_fragment(out, video_fragment, audio_fragment, audio_timescale);

    video_track_.clear();
    audio_track_.clear();
    segment_open_ = false;
    segment_byte_estimate_ = 0;

    if (!result.ok()) {
        stats_.dropped_frames += 1;
        return;
    }

    auto segment = std::make_shared<Segment>();
    segment->number = next_sequence_++;
    segment->name = config_.segment_name_prefix + std::to_string(segment->number) +
                    config_.segment_name_suffix;
    const std::int64_t span_ms =
        static_cast<std::int64_t>(last_video_ts_) - static_cast<std::int64_t>(segment_start_ts_);
    segment->duration = std::chrono::milliseconds(span_ms > 0 ? span_ms : 0);
    segment->init_epoch = init_epoch_;
    segment->data = core::SharedBuffer::adopt(std::move(out));

    stats_.segments_produced += 1;
    if (on_segment_) on_segment_(std::move(segment));
}

void Segmenter::mark_publisher_reconnect() {
    flush_segment();
    have_last_video_ts_ = false;
    timeline_base_ms_ += static_cast<std::int64_t>(last_video_ts_) + 1;
    video_config_.reset();
    audio_config_.reset();
    video_track_ = Track{};
    audio_track_ = Track{};
}

void Segmenter::on_metadata(const protocol::chunk::RtmpMessage&) {
    // Same reasoning as hls::Segmenter: the SPS and AudioSpecificConfig are
    // authoritative for the geometry/sample-rate this init segment needs.
}

void Segmenter::on_video(const protocol::chunk::RtmpMessage& message) {
    if (finalized_ || message.payload.empty()) return;

    auto parsed = media::h264::parse_video_tag(payload_span(message));
    if (!parsed.ok()) {
        stats_.dropped_frames += 1;
        return;
    }
    const auto& tag = parsed.value();

    if (tag.avc_packet_type == media::h264::kAvcPacketTypeSequenceHeader) {
        auto config = media::h264::parse_decoder_config(tag.body);
        if (!config.ok()) {
            stats_.dropped_frames += 1;
            return;
        }
        const bool had_config = video_config_.has_value();
        const bool changed = had_config &&
                             (!same_bytes(video_config_->sps, config.value().sps) ||
                              !same_bytes(video_config_->pps, config.value().pps));
        if (changed) flush_segment();

        auto dimensions = media::h264::parse_dimensions(config.value());
        video_config_ = config.value();
        // The raw config bytes ARE the avcC box body: an AVCDecoderConfigurationRecord
        // is byte-identical to the box's payload (ISO/IEC 14496-15 5.2.4.1).
        video_decoder_config_raw_.assign(tag.body.begin(), tag.body.end());
        // Only build a new epoch on the first config or a genuine change: an
        // encoder that repeats its sequence header unchanged (some do, on
        // every keyframe) must not bump the init segment's epoch for no
        // reason -- every rebuild is a new URL body a player has to refetch.
        if (dimensions.ok() && (!had_config || changed)) {
            video_dimensions_ = dimensions.value();
            rebuild_init_segment();
        } else if (dimensions.ok()) {
            video_dimensions_ = dimensions.value();
        }
        // A config that parses but whose geometry the SPS parser rejects
        // (corrupt cropping window etc.) intentionally does not (re)build the
        // init segment: publishing one with a stale or zero size would be
        // worse than continuing to serve the previous epoch, if any.
        return;
    }

    if (tag.avc_packet_type != media::h264::kAvcPacketTypeNalu) return; // end-of-sequence
    if (!video_config_ || video_dimensions_.width == 0) {
        stats_.dropped_frames += 1;
        return;
    }

    const std::int64_t delta =
        have_last_video_ts_
            ? static_cast<std::int64_t>(message.timestamp) - static_cast<std::int64_t>(last_video_ts_)
            : 0;
    if (have_last_video_ts_ && std::llabs(delta) > config_.discontinuity_threshold.count()) {
        flush_segment();
        timeline_base_ms_ += static_cast<std::int64_t>(last_video_ts_) -
                             static_cast<std::int64_t>(message.timestamp) + 1;
    }

    const std::int64_t elapsed =
        static_cast<std::int64_t>(message.timestamp) - static_cast<std::int64_t>(segment_start_ts_);
    const bool over_size = segment_byte_estimate_ >= config_.max_segment_bytes;
    const bool over_time = elapsed >= config_.max_segment_duration.count();

    if (segment_open_) {
        if (tag.is_keyframe && elapsed >= config_.target_duration.count()) {
            flush_segment();
        } else if (over_size || over_time) {
            stats_.forced_cuts += 1;
            flush_segment();
        }
    }

    if (!segment_open_) {
        segment_open_ = true;
        segment_start_ts_ = message.timestamp;
    }

    // The video track's pending sample now has a successor, so its real
    // duration is known; commit it before buffering this one.
    if (video_track_.pending.has) {
        const std::uint32_t duration =
            static_cast<std::uint32_t>(std::max<std::int64_t>(1, delta) * 90);
        commit_pending(video_track_, duration);
    }
    video_track_.pending.data.assign(tag.body.begin(), tag.body.end());
    video_track_.pending.timestamp_ms = message.timestamp;
    video_track_.pending.composition_time_ms = tag.composition_time_ms;
    video_track_.pending.keyframe = tag.is_keyframe;
    video_track_.pending.has = true;

    segment_byte_estimate_ += tag.body.size();
    stats_.video_frames += 1;
    last_video_ts_ = message.timestamp;
    have_last_video_ts_ = true;
}

void Segmenter::on_audio(const protocol::chunk::RtmpMessage& message) {
    if (finalized_ || message.payload.empty()) return;

    auto parsed = media::aac::parse_audio_tag(payload_span(message));
    if (!parsed.ok()) {
        stats_.dropped_frames += 1;
        return;
    }
    const auto& tag = parsed.value();

    if (tag.aac_packet_type == media::aac::kAacPacketTypeSequenceHeader) {
        auto config = media::aac::parse_audio_specific_config(tag.body);
        if (!config.ok()) {
            stats_.dropped_frames += 1;
            return;
        }
        const bool had_config = audio_config_.has_value();
        const bool changed =
            had_config &&
            (audio_config_->sampling_frequency_index != config.value().sampling_frequency_index ||
             audio_config_->channel_configuration != config.value().channel_configuration ||
             audio_config_->object_type != config.value().object_type);
        if (changed) flush_segment();

        audio_config_ = config.value();
        audio_specific_config_raw_.assign(tag.body.begin(), tag.body.end());
        if (!had_config || changed) rebuild_init_segment();
        return;
    }

    if (!audio_config_) {
        stats_.dropped_frames += 1;
        return;
    }
    // Audio never opens a segment on its own (same rule as HLS): a DASH
    // segment must start at a video keyframe so a player can join on it.
    if (!segment_open_) {
        stats_.dropped_frames += 1;
        return;
    }
    if (tag.body.empty()) return;

    if (audio_track_.pending.has) {
        const std::int64_t delta = static_cast<std::int64_t>(message.timestamp) -
                                   static_cast<std::int64_t>(audio_track_.pending.timestamp_ms);
        const auto duration =
            static_cast<std::uint32_t>(std::max<std::int64_t>(1, delta) * 90);
        commit_pending(audio_track_, duration);
    }
    audio_track_.pending.data.assign(tag.body.begin(), tag.body.end());
    audio_track_.pending.timestamp_ms = message.timestamp;
    audio_track_.pending.keyframe = true; // every AAC access unit is independently decodable
    audio_track_.pending.has = true;

    segment_byte_estimate_ += tag.body.size();
    stats_.audio_frames += 1;
}

void Segmenter::finalize() {
    if (finalized_) return;
    finalized_ = true;
    flush_segment();
}

} // namespace rtmp_server::dash
