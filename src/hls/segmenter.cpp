#include "rtmp_server/hls/segmenter.hpp"

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace rtmp_server::hls {

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

Segmenter::Segmenter(SegmentCallback on_segment, SegmenterConfig config)
    : on_segment_(std::move(on_segment)), config_(config), next_sequence_(config.initial_sequence) {}

std::uint64_t Segmenter::to_90k(std::int64_t milliseconds) const {
    const std::int64_t shifted = milliseconds + timeline_base_ms_;
    const std::int64_t clamped = shifted < 0 ? 0 : shifted;
    return static_cast<std::uint64_t>(clamped) * 90ULL;
}

std::string Segmenter::codecs_attribute() const {
    std::string codecs;
    if (video_config_ && sps_first_bytes_.size() >= 4) {
        // RFC 6381: avc1.PPCCLL from SPS bytes profile_idc, constraint
        // flags, level_idc (SPS payload begins after the 1-byte NAL header).
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "avc1.%02X%02X%02X",
                      static_cast<unsigned>(sps_first_bytes_[1]),
                      static_cast<unsigned>(sps_first_bytes_[2]),
                      static_cast<unsigned>(sps_first_bytes_[3]));
        codecs = buffer;
    }
    if (audio_config_) {
        if (!codecs.empty()) codecs += ",";
        codecs += "mp4a.40." + std::to_string(audio_config_->object_type);
    }
    return codecs;
}

bool Segmenter::is_discontinuous(std::uint32_t timestamp) const {
    if (!have_last_ts_) return false;
    const std::int64_t delta =
        static_cast<std::int64_t>(timestamp) - static_cast<std::int64_t>(last_ts_);
    // A large jump in either direction is not a real gap: backwards means a
    // 32-bit rollover or an encoder reset, far-forwards means a stalled or
    // spliced source. Both require the player to reset its decoder.
    return std::llabs(delta) > config_.discontinuity_threshold.count();
}

void Segmenter::mark_publisher_reconnect() {
    // Close whatever was open under the old publisher, then force the next
    // segment onto a fresh timeline with a discontinuity marker.
    flush_segment();
    pending_discontinuity_ = true;
    have_last_ts_ = false;
    muxer_.reset_continuity();
    // Advance the timeline past everything seen so far so the new
    // publisher's timestamps (which restart near 0) still produce
    // increasing PTS values.
    timeline_base_ms_ += static_cast<std::int64_t>(last_ts_) + 1;
    // A reconnect may bring different codec parameters; drop the cached
    // configs so the new sequence headers are honoured.
    video_config_.reset();
    audio_config_.reset();
}

void Segmenter::mark_media_discontinuity() {
    flush_segment();
    pending_discontinuity_ = true;
    muxer_.reset_continuity();
    // The source transcoder owns the output clock and keeps it monotonic.
    // Drop configs so packaging resumes only on a self-decodable IDR, but do
    // not alter timeline_base_ms_ or clear the last monotonic timestamp.
    video_config_.reset();
    audio_config_.reset();
}

void Segmenter::start_segment(std::uint32_t timestamp) {
    current_.clear();
    segment_open_ = true;
    segment_start_ts_ = timestamp;
    // Every segment is independently decodable: PAT/PMT at the head.
    muxer_.write_program_tables(current_);

    open_parts_.clear();
    part_start_offset_ = 0;
    part_index_ = 0;
    part_start_ts_ = timestamp;
    // Independence is decided when a keyframe is actually written into an
    // empty part, not assumed here: a segment opened by a forced mid-GOP cut
    // starts with no keyframe at all, and advertising that part as
    // INDEPENDENT=YES would send a joining player into an undecodable slice.
    part_independent_ = false;
    part_has_media_ = false;
    iframe_prefix_bytes_ = 0;
}

void Segmenter::maybe_close_part(std::uint32_t timestamp, bool force) {
    if (config_.part_target_duration.count() <= 0) return;
    if (!segment_open_) return;
    if (current_.size() <= part_start_offset_) return; // nothing new to publish

    if (!force) {
        const std::int64_t elapsed =
            static_cast<std::int64_t>(timestamp) - static_cast<std::int64_t>(part_start_ts_);
        if (elapsed < config_.part_target_duration.count()) return;
    }

    auto part = std::make_shared<Part>();
    part->segment_sequence = next_sequence_;
    part->index = part_index_;
    // "segment-42.3.ts": the part index sits between the sequence and the
    // suffix so a part URL is as immutable and as cacheable as a segment URL.
    part->name = config_.segment_name_prefix + std::to_string(next_sequence_) + "." +
                 std::to_string(part_index_) + config_.segment_name_suffix;
    part->data = core::SharedBuffer::copy_from(
        std::span<const std::byte>(current_.data() + part_start_offset_,
                                   current_.size() - part_start_offset_));
    const std::int64_t span_ms =
        static_cast<std::int64_t>(timestamp) - static_cast<std::int64_t>(part_start_ts_);
    part->duration = std::chrono::milliseconds(span_ms > 0 ? span_ms : 0);
    part->independent = part_independent_;

    part_start_offset_ = current_.size();
    part_start_ts_ = timestamp;
    part_index_ += 1;
    // Only a part that begins with a keyframe may be marked independent, and
    // within one segment only the first one does.
    part_independent_ = false;
    part_has_media_ = false;
    stats_.parts_produced += 1;

    open_parts_.push_back(part);
    if (on_part_) on_part_(std::move(part));
}

void Segmenter::flush_segment() {
    if (!segment_open_ || current_.empty()) {
        segment_open_ = false;
        return;
    }

    // Only PAT+PMT and no media is not a segment worth publishing.
    if (current_.size() <= 2 * media::ts::kPacketSize) {
        current_.clear();
        segment_open_ = false;
        return;
    }

    // The tail of the segment is still an unpublished part; close it before
    // the segment itself so a low-latency player's last EXT-X-PART and the
    // segment it belongs to describe exactly the same bytes.
    maybe_close_part(last_ts_, /*force=*/true);

    auto segment = std::make_shared<Segment>();
    segment->sequence = next_sequence_++;
    segment->name = config_.segment_name_prefix + std::to_string(segment->sequence) +
                    config_.segment_name_suffix;
    const std::int64_t span_ms =
        static_cast<std::int64_t>(last_ts_) - static_cast<std::int64_t>(segment_start_ts_);
    segment->duration = std::chrono::milliseconds(span_ms > 0 ? span_ms : 0);
    segment->discontinuity = pending_discontinuity_;
    segment->parts = std::move(open_parts_);
    segment->iframe_prefix_bytes = iframe_prefix_bytes_;
    // Move the accumulated bytes into immutable shared storage: from here on
    // every viewer shares these bytes, never a copy (3.8).
    segment->data = core::SharedBuffer::adopt(std::move(current_));

    current_ = std::vector<std::byte>{};
    open_parts_.clear();
    part_start_offset_ = 0;
    part_index_ = 0;
    part_has_media_ = false;
    iframe_prefix_bytes_ = 0;
    segment_open_ = false;
    if (pending_discontinuity_) {
        stats_.discontinuities += 1;
        pending_discontinuity_ = false;
    }
    stats_.segments_produced += 1;

    // Callback is invoked with no lock held by this class (3.7).
    if (on_segment_) on_segment_(std::move(segment));
}

void Segmenter::on_metadata(const protocol::chunk::RtmpMessage&) {
    // onMetaData carries no data the TS packaging needs: the SPS/PPS and
    // AudioSpecificConfig are authoritative for resolution and sample rate.
    // Deliberately ignored rather than half-used.
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
        const bool changed = video_config_.has_value() &&
                             (!same_bytes(video_config_->sps, config.value().sps) ||
                              !same_bytes(video_config_->pps, config.value().pps));
        if (changed) {
            // A mid-stream codec parameter change is exactly the case
            // EXT-X-DISCONTINUITY exists for: the player must rebuild its
            // decoder. Close the current segment so the new parameters
            // begin a new one.
            stats_.sequence_header_changes += 1;
            flush_segment();
            pending_discontinuity_ = true;
        }
        video_config_ = config.value();
        if (!video_config_->sps.empty()) sps_first_bytes_ = video_config_->sps.front();
        return;
    }

    if (tag.avc_packet_type != media::h264::kAvcPacketTypeNalu) return; // end-of-sequence
    if (!video_config_) {
        // No SPS/PPS yet: a segment starting here would be undecodable.
        stats_.dropped_frames += 1;
        return;
    }

    if (is_discontinuous(message.timestamp)) {
        flush_segment();
        pending_discontinuity_ = true;
        // Keep the output timeline monotonic across the jump.
        timeline_base_ms_ += static_cast<std::int64_t>(last_ts_) -
                             static_cast<std::int64_t>(message.timestamp) + 1;
        muxer_.reset_continuity();
    }

    const std::int64_t elapsed =
        static_cast<std::int64_t>(message.timestamp) - static_cast<std::int64_t>(segment_start_ts_);
    const bool over_size = current_.size() >= config_.max_segment_bytes;
    const bool over_time = elapsed >= config_.max_segment_duration.count();

    if (segment_open_) {
        if (tag.is_keyframe && elapsed >= config_.target_duration.count()) {
            flush_segment();
        } else if (over_size || over_time) {
            // Bounded resources win over clean segment boundaries: cut even
            // though this is not a keyframe. The resulting segment is not
            // independently decodable, so it is marked discontinuous.
            stats_.forced_cuts += 1;
            flush_segment();
            pending_discontinuity_ = true;
        }
    }

    if (!segment_open_) {
        // Only start a segment on a keyframe when we have one; otherwise a
        // forced cut has already left us mid-GOP and we resume immediately.
        start_segment(message.timestamp);
    }

    std::vector<std::byte> annexb;
    annexb.reserve(tag.body.size() + 64);
    // Parameter sets are re-inserted on every keyframe so a player joining
    // at any segment boundary can decode without the initial sequence header.
    auto converted =
        media::h264::avcc_to_annexb(tag.body, *video_config_, /*insert_parameter_sets=*/tag.is_keyframe,
                                    annexb);
    if (!converted.ok()) {
        stats_.dropped_frames += 1;
        return;
    }

    const std::uint64_t dts = to_90k(message.timestamp);
    const std::uint64_t pts = to_90k(static_cast<std::int64_t>(message.timestamp) + tag.composition_time_ms);
    // A part that opens with a keyframe is one a player may start at.
    if (tag.is_keyframe && !part_has_media_) part_independent_ = true;
    (void)muxer_.write_video(current_, annexb, pts, dts, tag.is_keyframe);
    part_has_media_ = true;

    // The first keyframe of a segment, together with the program tables that
    // precede it, is the independently decodable prefix a trick-play playlist
    // points at. Later keyframes in the same segment are not recorded: they
    // are not preceded by their own PAT/PMT, so a byte range starting at one
    // would not decode on its own.
    if (tag.is_keyframe && iframe_prefix_bytes_ == 0) {
        iframe_prefix_bytes_ = current_.size();
    }

    stats_.video_frames += 1;
    last_ts_ = message.timestamp;
    have_last_ts_ = true;
    maybe_close_part(message.timestamp, /*force=*/false);
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
        if (audio_config_ &&
            (audio_config_->sampling_frequency_index != config.value().sampling_frequency_index ||
             audio_config_->channel_configuration != config.value().channel_configuration ||
             audio_config_->object_type != config.value().object_type)) {
            stats_.sequence_header_changes += 1;
            flush_segment();
            pending_discontinuity_ = true;
        }
        audio_config_ = config.value();
        return;
    }

    if (!audio_config_) {
        stats_.dropped_frames += 1;
        return;
    }
    // Audio never opens a segment: HLS segments must start at a video
    // keyframe. Audio arriving before the first keyframe is dropped rather
    // than written into a segment no player could start on.
    if (!segment_open_) {
        stats_.dropped_frames += 1;
        return;
    }
    if (tag.body.empty()) return;

    std::vector<std::byte> adts;
    adts.reserve(tag.body.size() + media::aac::kAdtsHeaderSize);
    media::aac::append_adts_header(adts, *audio_config_, tag.body.size());
    adts.insert(adts.end(), tag.body.begin(), tag.body.end());

    (void)muxer_.write_audio(current_, adts, to_90k(message.timestamp));
    part_has_media_ = true;
    stats_.audio_frames += 1;
    if (!have_last_ts_) {
        last_ts_ = message.timestamp;
        have_last_ts_ = true;
    }
    // An audio-only stretch between video frames must still advance the part
    // cadence, or a low-latency player sees the live edge stall.
    maybe_close_part(message.timestamp, /*force=*/false);
}

void Segmenter::finalize() {
    if (finalized_) return;
    finalized_ = true;
    // Publish whatever partial segment exists so the tail of the stream is
    // not silently lost on an abrupt publisher disconnect.
    flush_segment();
}

} // namespace rtmp_server::hls
