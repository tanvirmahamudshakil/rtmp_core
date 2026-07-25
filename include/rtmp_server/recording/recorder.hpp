#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "rtmp_server/media/flv/flv_writer.hpp"
#include "rtmp_server/protocol/chunk/chunk_types.hpp"
#include "rtmp_server/protocol/commands/recorder_sink.hpp"
#include "rtmp_server/recording/file_sink.hpp"

namespace rtmp_server::recording {

namespace chunk = protocol::chunk;

struct RecorderConfig {
    // Upper bound on outstanding (queued-but-not-durably-written) bytes in the
    // sink before the Recorder drops incoming frames rather than growing the
    // queue without limit. Default 16 MiB — a few seconds of high-bitrate
    // video, enough to ride out a brief disk stall without OOMing the server
    // under a permanently slow/stuck disk. See docs/flv-recording.md
    // "Bounded queue policy".
    std::size_t max_queued_bytes = 16u * 1024u * 1024u;
};

struct RecorderStats {
    std::uint64_t tags_written = 0;
    std::uint64_t bytes_written = 0; // total FLV bytes handed to the sink
    std::uint64_t audio_tags = 0;
    std::uint64_t video_tags = 0;
    std::uint64_t dropped_frames = 0; // frames dropped by the bounded-queue policy
    std::uint64_t dropped_bytes = 0;
    std::uint32_t max_timestamp = 0;  // highest media timestamp seen (ms)
    bool metadata_written = false;
    bool failed = false;              // a sink write failed; recording aborted (no crash)
};

// Records one publishing RTMP stream to a single FLV file through a FileSink.
// Consumes the same FLV-format audio/video/metadata payloads MediaIngest
// parses (RTMP already delivers AAC/AVC audio/video message bodies in FLV tag
// body form), frames each as an FLV tag with a running PreviousTagSize, and
// patches the onMetaData duration/filesize placeholders at finalize.
//
// Implements protocol::commands::RecorderSink so CommandSession can route to
// it via an injected pointer without the protocol layer depending on this
// library (docs/flv-recording.md). One Recorder == one recording == one
// output file (typical one-publish-per-connection case, per
// command_session.hpp).
class Recorder final : public protocol::commands::RecorderSink {
public:
    explicit Recorder(FileSink& sink, RecorderConfig config = {});

    // RecorderSink — never throw, never propagate sink errors to the caller.
    void on_metadata(const chunk::RtmpMessage& message) override;
    void on_audio(const chunk::RtmpMessage& message) override;
    void on_video(const chunk::RtmpMessage& message) override;
    void finalize() override;

    [[nodiscard]] const RecorderStats& stats() const noexcept { return stats_; }
    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] bool finalized() const noexcept { return finalized_; }

private:
    // Writes the 13-byte FLV header once, on the first tag/metadata.
    void ensure_started();
    // Writes the onMetaData tag (from a received metadata message, or defaults)
    // before the first media tag, recording the placeholder offsets to patch.
    void ensure_metadata_written();
    // Frames `payload` as an FLV tag and hands it to the sink, applying the
    // bounded-queue drop policy and the disk-failure guard.
    void write_tag(std::uint8_t tag_type, std::span<const std::byte> payload, std::uint32_t timestamp);
    void mark_failed(std::string_view where, const core::Error& error);

    FileSink& sink_;
    RecorderConfig config_;
    RecorderStats stats_;

    bool started_ = false;
    bool finalized_ = false;

    std::uint64_t file_offset_ = 0; // running byte offset (== file size so far)
    media::flv::OnMetaData meta_;
    std::optional<std::uint64_t> duration_patch_offset_;
    std::optional<std::uint64_t> filesize_patch_offset_;
};

} // namespace rtmp_server::recording
