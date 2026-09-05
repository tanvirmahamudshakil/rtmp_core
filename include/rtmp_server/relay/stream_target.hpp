#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rtmp_server/media/media_handoff_queue.hpp"
#include "rtmp_server/protocol/chunk/chunk_types.hpp"
#include "rtmp_server/protocol/commands/recorder_sink.hpp"

namespace rtmp_server::relay {

// One outbound destination for a published stream. `url` is a complete
// rtmp://host[:port]/application/streamkey — the form every CDN ingest hands
// out, and the form another StreamForge origin accepts as a publish.
struct StreamTargetConfig {
    std::string application;  // the local application being republished
    std::string stream;       // the local stream being republished
    std::string name;         // operator label, unique within the stream
    std::string url;
    bool enabled = true;
    // Relay targets point at another origin of this same cluster. Only the
    // reporting differs (a relay is capacity, a target is distribution), but an
    // operator needs to tell them apart at a glance.
    bool relay = false;
    std::uint32_t restart_delay_seconds = 5;
};

enum class StreamTargetState { Connecting, Publishing, Error, Stopped };

struct StreamTargetStatus {
    std::string application;
    std::string stream;
    std::string name;
    // Never the raw URL: it carries the target's stream key.
    std::string url_redacted;
    bool relay = false;
    bool enabled = true;
    StreamTargetState state = StreamTargetState::Stopped;
    std::string detail;
    std::uint64_t bytes_sent = 0;
    std::uint64_t frames_dropped = 0;
    std::uint64_t reconnects = 0;
};

// Everything after the last '/' of an RTMP URL is the stream key. Redacted to
// its last four characters so an operator can still recognise which key is
// configured without the API handing the key back out.
[[nodiscard]] std::string redact_rtmp_url(std::string_view url);

// One publish, forwarded to one target.
//
// A RecorderSink, so a target attaches to the existing publish path exactly
// like HLS packaging or the transcode ladder does: media arrives on the
// publisher's io_uring worker, is copied into a bounded queue and leaves on
// this object's own thread. A target that is slow, unreachable or gone
// therefore costs that target's own continuity and never the publish.
//
// The connection is re-established for as long as the publisher is live: an
// ingest that drops a publisher (routine on the large platforms) is retried
// with a bounded exponential backoff, and each attempt starts from cached
// metadata and sequence headers followed by the next keyframe, so the far side
// gets a decodable stream rather than the middle of a GOP.
class StreamTargetSink final : public protocol::commands::RecorderSink {
public:
    StreamTargetSink(StreamTargetConfig config, media::HandoffLimits limits = {});
    ~StreamTargetSink() override;
    StreamTargetSink(const StreamTargetSink&) = delete;
    StreamTargetSink& operator=(const StreamTargetSink&) = delete;

    void on_metadata(const protocol::chunk::RtmpMessage& message) override;
    void on_audio(const protocol::chunk::RtmpMessage& message) override;
    void on_video(const protocol::chunk::RtmpMessage& message) override;
    void finalize() override;

    [[nodiscard]] StreamTargetStatus status() const;

    // Backoff for the n-th consecutive failure, exposed for testing: the
    // configured delay doubled per failure, capped, so a dead target is still
    // retried forever without hammering it.
    [[nodiscard]] static std::chrono::seconds retry_delay_for(const StreamTargetConfig& config,
                                                              std::uint32_t consecutive_failures);

private:
    void run();
    void set_detail(std::string detail);
    [[nodiscard]] std::vector<media::HandoffMessage> priming() const;

    StreamTargetConfig config_;
    media::MediaHandoffQueue queue_;
    std::thread worker_;
    std::atomic<bool> running_{true};
    std::atomic<StreamTargetState> state_{StreamTargetState::Connecting};
    std::atomic<std::uint64_t> bytes_sent_{0};
    std::atomic<std::uint64_t> reconnects_{0};
    std::atomic<bool> finalized_{false};
    // Wakes the retry sleep the moment the publisher goes away, so finalize()
    // never waits out a backoff before joining.
    std::mutex sleep_mutex_;
    std::condition_variable sleep_cv_;

    mutable std::mutex mutex_;
    std::string detail_;
    // The three messages a target needs before any picture makes sense. Kept
    // as raw FLV payloads so they can be replayed onto a new connection
    // verbatim.
    std::vector<std::byte> metadata_;
    std::vector<std::byte> video_sequence_header_;
    std::vector<std::byte> audio_sequence_header_;
};

} // namespace rtmp_server::relay
