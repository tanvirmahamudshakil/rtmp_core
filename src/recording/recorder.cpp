#include "rtmp_server/recording/recorder.hpp"

#include <array>
#include <span>
#include <string>

#include "rtmp_server/observability/logger.hpp"
#include "rtmp_server/protocol/amf0/amf0_decoder.hpp"
#include "rtmp_server/protocol/amf0/amf0_value.hpp"

namespace rtmp_server::recording {

namespace {

using observability::LogField;
using observability::LogLevel;

std::string u64(std::uint64_t v) { return std::to_string(v); }

// Pulls a numeric metadata field out of an onMetaData Object/ECMA-array value.
void copy_number(const protocol::amf0::Amf0Value& obj, std::string_view key, double& out) {
    if (const auto* v = obj.find(key); v != nullptr && v->is_number()) out = v->as_number();
}

// Best-effort extraction of onMetaData fields from a metadata (type 18)
// payload. RTMP encoders send either `onMetaData` + ECMA-array, or
// `@setDataFrame` + `onMetaData` + ECMA-array. We locate the first
// Object/ECMA-array value and read whatever standard keys are present. A
// malformed payload just leaves the defaults in place (never throws).
void extract_metadata(std::span<const std::byte> payload, media::flv::OnMetaData& meta) {
    auto decoded = protocol::amf0::decode_all(payload);
    if (!decoded.ok()) return;
    for (const auto& value : decoded.value()) {
        if (!value.is_object() && !value.is_ecma_array()) continue;
        copy_number(value, "duration", meta.duration);
        copy_number(value, "width", meta.width);
        copy_number(value, "height", meta.height);
        copy_number(value, "framerate", meta.framerate);
        copy_number(value, "videocodecid", meta.videocodecid);
        copy_number(value, "audiocodecid", meta.audiocodecid);
        copy_number(value, "audiosamplerate", meta.audiosamplerate);
        copy_number(value, "audiosamplesize", meta.audiosamplesize);
        if (const auto* s = value.find("stereo"); s != nullptr && s->is_boolean()) {
            meta.stereo = s->as_boolean();
        }
        return; // first metadata object wins
    }
}

} // namespace

Recorder::Recorder(FileSink& sink, RecorderConfig config) : sink_(sink), config_(config) {}

void Recorder::mark_failed(std::string_view where, const core::Error& error) {
    if (stats_.failed) return;
    stats_.failed = true;
    RTMP_LOG(LogLevel::Error, "recorder", "write_failed",
             {LogField{"where", std::string(where)},
              LogField{"error", error.message()},
              LogField{"bytes_written", u64(stats_.bytes_written)}});
}

void Recorder::ensure_started() {
    if (started_ || stats_.failed) return;
    started_ = true;
    // We always advertise both audio and video present: the FLV TypeFlags are
    // advisory and a recording may carry either or both. Players tolerate a
    // flag set for an absent track; the opposite (a present track with the
    // flag clear) is what breaks some demuxers, so we err on setting both.
    auto header = media::flv::encode_file_header(/*has_audio=*/true, /*has_video=*/true);
    auto res = sink_.append(std::span<const std::byte>(header.data(), header.size()));
    if (!res.ok()) {
        mark_failed("header", res.error());
        return;
    }
    file_offset_ += header.size();
    stats_.bytes_written += header.size();
}

void Recorder::ensure_metadata_written() {
    if (stats_.metadata_written || stats_.failed) return;
    ensure_started();
    if (stats_.failed) return;

    auto tag = media::flv::build_onmetadata_tag(meta_);
    const std::uint64_t base = file_offset_;
    auto res = sink_.append(std::span<const std::byte>(tag.bytes.data(), tag.bytes.size()));
    if (!res.ok()) {
        mark_failed("metadata", res.error());
        return;
    }
    duration_patch_offset_ = base + tag.duration_value_offset;
    filesize_patch_offset_ = base + tag.filesize_value_offset;
    file_offset_ += tag.bytes.size();
    stats_.bytes_written += tag.bytes.size();
    stats_.tags_written += 1;
    stats_.metadata_written = true;
}

void Recorder::write_tag(std::uint8_t tag_type, std::span<const std::byte> payload,
                          std::uint32_t timestamp) {
    if (stats_.failed || finalized_) return;

    // Bounded queue: if the sink has too many bytes still outstanding, drop
    // this frame rather than grow memory without limit under a slow/stuck
    // disk. We drop the newest frame (not the oldest) so everything already
    // committed to the file stays a valid, monotonically-timestamped prefix.
    std::vector<std::byte> framed;
    media::flv::append_tag(framed, tag_type, payload, timestamp);
    if (sink_.pending_bytes() + framed.size() > config_.max_queued_bytes) {
        stats_.dropped_frames += 1;
        stats_.dropped_bytes += framed.size();
        return;
    }

    auto res = sink_.append(std::span<const std::byte>(framed.data(), framed.size()));
    if (!res.ok()) {
        mark_failed("tag", res.error());
        return;
    }
    file_offset_ += framed.size();
    stats_.bytes_written += framed.size();
    stats_.tags_written += 1;
    if (timestamp > stats_.max_timestamp) stats_.max_timestamp = timestamp;
}

void Recorder::on_metadata(const chunk::RtmpMessage& message) {
    if (finalized_ || stats_.failed) return;
    // A metadata message that arrives before any media updates the fields we
    // will write; once the onMetaData tag is already committed we can no
    // longer change it in place (a later phase could rewrite via patch).
    if (!stats_.metadata_written) {
        extract_metadata(std::span<const std::byte>(message.payload.data(), message.payload.size()), meta_);
        ensure_metadata_written();
    }
}

void Recorder::on_audio(const chunk::RtmpMessage& message) {
    if (finalized_ || stats_.failed || message.payload.empty()) return;
    ensure_metadata_written();
    if (stats_.failed) return;
    write_tag(media::flv::kTagTypeAudio,
              std::span<const std::byte>(message.payload.data(), message.payload.size()),
              message.timestamp);
    stats_.audio_tags += 1;
}

void Recorder::on_video(const chunk::RtmpMessage& message) {
    if (finalized_ || stats_.failed || message.payload.empty()) return;
    ensure_metadata_written();
    if (stats_.failed) return;
    write_tag(media::flv::kTagTypeVideo,
              std::span<const std::byte>(message.payload.data(), message.payload.size()),
              message.timestamp);
    stats_.video_tags += 1;
}

void Recorder::finalize() {
    if (finalized_) return;
    finalized_ = true;

    // Even an abrupt disconnect with no media produces a valid (empty) FLV:
    // header + onMetaData. If a prior write already failed, we skip patching
    // but still close the fd cleanly below.
    if (!stats_.failed) {
        ensure_metadata_written();
    }

    if (!stats_.failed && duration_patch_offset_ && filesize_patch_offset_) {
        const double duration_seconds = static_cast<double>(stats_.max_timestamp) / 1000.0;
        auto d = media::flv::encode_double_be(duration_seconds);
        auto f = media::flv::encode_double_be(static_cast<double>(file_offset_));
        if (auto r = sink_.patch(*duration_patch_offset_,
                                 std::span<const std::byte>(d.data(), d.size()));
            !r.ok()) {
            mark_failed("patch_duration", r.error());
        }
        if (!stats_.failed) {
            if (auto r = sink_.patch(*filesize_patch_offset_,
                                     std::span<const std::byte>(f.data(), f.size()));
                !r.ok()) {
                mark_failed("patch_filesize", r.error());
            }
        }
    }

    // Always attempt to flush/close, even on the failure path, so the fd is
    // released and whatever was written is durable.
    if (auto r = sink_.finalize(); !r.ok() && !stats_.failed) {
        mark_failed("finalize", r.error());
    }

    RTMP_LOG(LogLevel::Info, "recorder", "finalized",
             {LogField{"bytes_written", u64(stats_.bytes_written)},
              LogField{"tags_written", u64(stats_.tags_written)},
              LogField{"dropped_frames", u64(stats_.dropped_frames)},
              LogField{"failed", stats_.failed ? "true" : "false"}});
}

} // namespace rtmp_server::recording
