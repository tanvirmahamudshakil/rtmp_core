#include "rtmp_server/relay/stream_target.hpp"

#include <algorithm>
#include <utility>

#include "rtmp_server/media/aac/adts.hpp"
#include "rtmp_server/observability/logger.hpp"
#include "rtmp_server/protocol/media/media_ingest.hpp"
#include "rtmp_server/relay/rtmp_push_client.hpp"

namespace rtmp_server::relay {
namespace {

namespace protocol_media = rtmp_server::protocol::media;

constexpr std::uint32_t kMaxRetryDelaySeconds = 60;

media::HandoffMessage message_from(const protocol::chunk::RtmpMessage& source, bool video,
                                   bool metadata) {
    media::HandoffMessage queued;
    queued.video = video;
    queued.metadata = metadata;
    queued.timestamp = source.timestamp;
    queued.payload.assign(source.payload.begin(), source.payload.end());
    return queued;
}

} // namespace

std::string redact_rtmp_url(std::string_view url) {
    const auto slash = url.rfind('/');
    if (slash == std::string_view::npos || slash + 1 >= url.size()) return std::string(url);
    const auto key = url.substr(slash + 1);
    // A short key would be fully revealed by showing a suffix, so it is masked
    // entirely.
    if (key.size() <= 4) return std::string(url.substr(0, slash + 1)) + "****";
    return std::string(url.substr(0, slash + 1)) + "****" + std::string(key.substr(key.size() - 4));
}

std::chrono::seconds StreamTargetSink::retry_delay_for(const StreamTargetConfig& config,
                                                       std::uint32_t consecutive_failures) {
    const std::uint32_t base = std::max<std::uint32_t>(config.restart_delay_seconds, 1);
    std::uint64_t delay = base;
    for (std::uint32_t i = 0; i < consecutive_failures && delay < kMaxRetryDelaySeconds; ++i) {
        delay *= 2;
    }
    return std::chrono::seconds(std::min<std::uint64_t>(delay, kMaxRetryDelaySeconds));
}

StreamTargetSink::StreamTargetSink(StreamTargetConfig config, media::HandoffLimits limits)
    : config_(std::move(config)), queue_(limits) {
    set_detail("connecting");
    worker_ = std::thread([this] { run(); });
}

StreamTargetSink::~StreamTargetSink() { finalize(); }

void StreamTargetSink::set_detail(std::string detail) {
    std::lock_guard<std::mutex> lock(mutex_);
    detail_ = std::move(detail);
}

void StreamTargetSink::on_metadata(const protocol::chunk::RtmpMessage& message) {
    if (finalized_.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        metadata_.assign(message.payload.begin(), message.payload.end());
    }
    queue_.push(message_from(message, false, true));
}

void StreamTargetSink::on_audio(const protocol::chunk::RtmpMessage& message) {
    if (finalized_.load(std::memory_order_acquire)) return;
    auto queued = message_from(message, false, false);
    if (const auto tag = media::aac::parse_audio_tag(message.payload);
        tag && tag.value().aac_packet_type == media::aac::kAacPacketTypeSequenceHeader) {
        queued.sequence_header = true;
        std::lock_guard<std::mutex> lock(mutex_);
        audio_sequence_header_.assign(message.payload.begin(), message.payload.end());
    }
    queue_.push(std::move(queued));
}

void StreamTargetSink::on_video(const protocol::chunk::RtmpMessage& message) {
    if (finalized_.load(std::memory_order_acquire)) return;
    auto queued = message_from(message, true, false);
    if (const auto info = protocol_media::classify_video_tag(message.payload)) {
        queued.keyframe = info->frame_type == protocol_media::VideoFrameType::KeyFrame ||
                          info->frame_type == protocol_media::VideoFrameType::GeneratedKeyFrame;
        queued.sequence_header =
            (info->avc_packet_type &&
             *info->avc_packet_type == protocol_media::AvcPacketType::SequenceHeader) ||
            (info->ex_packet_type &&
             *info->ex_packet_type == protocol_media::ExVideoPacketType::SequenceStart);
        if (queued.sequence_header) {
            std::lock_guard<std::mutex> lock(mutex_);
            video_sequence_header_.assign(message.payload.begin(), message.payload.end());
        }
    }
    queue_.push(std::move(queued));
}

void StreamTargetSink::finalize() {
    if (finalized_.exchange(true, std::memory_order_acq_rel)) return;
    running_.store(false);
    queue_.close();
    sleep_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    state_.store(StreamTargetState::Stopped);
}

std::vector<media::HandoffMessage> StreamTargetSink::priming() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<media::HandoffMessage> messages;
    const auto add = [&messages](const std::vector<std::byte>& payload, bool video, bool metadata) {
        if (payload.empty()) return;
        media::HandoffMessage message;
        message.video = video;
        message.metadata = metadata;
        message.sequence_header = !metadata;
        // Timestamp zero: these describe the stream rather than sitting in it,
        // and the media that follows re-bases onto the first real frame.
        message.timestamp = 0;
        message.payload = payload;
        messages.push_back(std::move(message));
    };
    add(metadata_, false, true);
    add(video_sequence_header_, true, false);
    add(audio_sequence_header_, false, false);
    return messages;
}

StreamTargetStatus StreamTargetSink::status() const {
    StreamTargetStatus status;
    status.application = config_.application;
    status.stream = config_.stream;
    status.name = config_.name;
    status.url_redacted = redact_rtmp_url(config_.url);
    status.relay = config_.relay;
    status.enabled = config_.enabled;
    status.state = state_.load();
    status.bytes_sent = bytes_sent_.load();
    status.frames_dropped = queue_.stats().dropped;
    status.reconnects = reconnects_.load();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status.detail = detail_;
    }
    return status;
}

void StreamTargetSink::run() {
    std::uint32_t consecutive_failures = 0;

    while (running_.load()) {
        RtmpPushClient client(config_.url);
        const auto attempt_started = std::chrono::steady_clock::now();
        state_.store(StreamTargetState::Connecting);
        set_detail("connecting");

        auto result = client.run(
            [this] { return running_.load(); }, queue_, [this] { return priming(); },
            [this] {
                state_.store(StreamTargetState::Publishing);
                set_detail("publishing");
            });
        bytes_sent_.fetch_add(client.bytes_sent(), std::memory_order_relaxed);

        if (!running_.load()) break;
        if (result) {
            // The queue closed under a still-running target: the publisher went
            // away, which is not a failure.
            break;
        }

        state_.store(StreamTargetState::Error);
        set_detail(result.error().message());
        RTMP_LOG(observability::LogLevel::Warn, "stream-target", "push failed",
                 {{"application", config_.application},
                  {"stream", config_.stream},
                  {"target", config_.name},
                  {"error", result.error().message()}});

        // A target that held a publish for a while and then dropped is not a
        // failing target -- the large ingests cycle a publisher routinely. Only
        // a run that failed quickly counts toward the backoff, so a stream that
        // has been up for hours reconnects at the configured delay rather than
        // at the cap.
        constexpr std::chrono::seconds kHealthyRun{60};
        if (std::chrono::steady_clock::now() - attempt_started >= kHealthyRun) {
            consecutive_failures = 0;
        }
        const auto delay = retry_delay_for(config_, consecutive_failures);
        ++consecutive_failures;
        reconnects_.fetch_add(1, std::memory_order_relaxed);

        std::unique_lock<std::mutex> lock(sleep_mutex_);
        sleep_cv_.wait_for(lock, delay, [this] { return !running_.load(); });
    }

    state_.store(StreamTargetState::Stopped);
}

} // namespace rtmp_server::relay
