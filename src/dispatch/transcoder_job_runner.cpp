#include "rtmp_server/dispatch/transcoder_job_runner.hpp"

#include <algorithm>
#include <optional>
#include <sstream>
#include <utility>

#include "rtmp_server/observability/logger.hpp"
#include "rtmp_server/relay/rtmp_push_client.hpp"
#include "rtmp_server/relay/rtmp_tag_builder.hpp"
#include "rtmp_server/transcoding/native/rtmp_source_client.hpp"
#include "rtmp_server/transcoding/native/rtmp_tag_converter.hpp"

namespace rtmp_server::dispatch {
namespace {

using media::HandoffMessage;
using media::MediaHandoffQueue;
using relay::RtmpPushClient;
using relay::RtmpAudioTagBuilder;
using relay::RtmpVideoTagBuilder;
using transcoding::native::RtmpSourceClient;
using transcoding::native::RtmpTagConverter;
using transcoding::native::RenditionSpec;
using transcoding::native::SourceTranscoder;
using transcoding::native::SourceVideoCodec;

std::string target_url_for(const TranscoderJobAssignment& assignment,
                          const DispatchedRendition& rendition) {
    std::ostringstream url;
    url << "rtmp://" << assignment.origin_rtmp_host << ':' << assignment.origin_rtmp_port << '/'
        << assignment.target_application << '/' << rendition.output_stream;
    return url.str();
}

} // namespace

// One rendition's push side: a queue fed by the shared decode/encode pipeline
// and its own RtmpPushClient/thread, so a slow or unreachable target for one
// rung never stalls the others or the pull -- the queue's own drop policy
// (media::MediaHandoffQueue) is what a heavy encoder or a stalled target
// costs, exactly as it is on the origin's own stream-target/backup-publisher
// paths.
struct TranscoderJobRunner::RenditionPusher {
    explicit RenditionPusher(std::string url) : target_url(std::move(url)) {}

    std::string target_url;
    MediaHandoffQueue queue;
    RtmpVideoTagBuilder video_builder;
    RtmpAudioTagBuilder audio_builder;
    // Cached so a reconnect can prime the target with the current parameter
    // sets before any picture, exactly as StreamTargetSink's own priming does
    // -- but built once by the encode side, not replayed from a publisher's
    // own tags, since this runner has no publisher, only its own builders.
    std::mutex priming_mutex;
    std::vector<std::byte> video_sequence_header;
    std::vector<std::byte> audio_sequence_header;
    std::thread thread;
    std::atomic<bool> running{true};
    std::atomic<TranscoderJobRunnerState> state{TranscoderJobRunnerState::Connecting};
    std::atomic<std::uint64_t> bytes_pushed{0};

    void run() {
        while (running.load()) {
            RtmpPushClient client(target_url);
            state.store(TranscoderJobRunnerState::Connecting);
            auto result = client.run(
                [this] { return running.load(); }, queue,
                [this]() -> std::vector<HandoffMessage> {
                    std::lock_guard<std::mutex> lock(priming_mutex);
                    std::vector<HandoffMessage> priming;
                    const auto add = [&](const std::vector<std::byte>& payload, bool video) {
                        if (payload.empty()) return;
                        HandoffMessage message;
                        message.video = video;
                        message.sequence_header = true;
                        message.timestamp = 0;
                        message.payload = payload;
                        priming.push_back(std::move(message));
                    };
                    add(video_sequence_header, true);
                    add(audio_sequence_header, false);
                    return priming;
                },
                [this] { state.store(TranscoderJobRunnerState::Running); });
            bytes_pushed.fetch_add(client.bytes_sent(), std::memory_order_relaxed);
            if (!running.load()) break;
            if (!result) {
                state.store(TranscoderJobRunnerState::Error);
                RTMP_LOG(observability::LogLevel::Warn, "transcoder-agent", "rendition push failed",
                         {{"target", target_url}, {"error", result.error().message()}});
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }
        state.store(TranscoderJobRunnerState::Stopped);
    }
};

TranscoderJobRunner::TranscoderJobRunner(TranscoderJobAssignment assignment)
    : assignment_(std::move(assignment)) {
    for (const auto& rendition : assignment_.renditions) {
        auto pusher = std::make_unique<RenditionPusher>(target_url_for(assignment_, rendition));
        pusher->thread = std::thread([p = pusher.get()] { p->run(); });
        pushers_.push_back(std::move(pusher));
    }
    pull_thread_ = std::thread([this] { run(); });
}

TranscoderJobRunner::~TranscoderJobRunner() { stop(); }

void TranscoderJobRunner::set_detail(std::string detail) {
    std::lock_guard<std::mutex> lock(detail_mutex_);
    detail_ = std::move(detail);
}

void TranscoderJobRunner::stop() {
    if (!running_.exchange(false)) return;
    for (auto& pusher : pushers_) {
        pusher->running.store(false);
        pusher->queue.close();
    }
    if (pull_thread_.joinable()) pull_thread_.join();
    for (auto& pusher : pushers_) {
        if (pusher->thread.joinable()) pusher->thread.join();
    }
    state_.store(TranscoderJobRunnerState::Stopped);
}

TranscoderJobRunnerStatus TranscoderJobRunner::status() const {
    TranscoderJobRunnerStatus status;
    status.id = assignment_.id;
    status.state = state_.load();
    std::uint64_t bytes = 0;
    for (const auto& pusher : pushers_) bytes += pusher->bytes_pushed.load();
    status.bytes_pushed = bytes;
    {
        std::lock_guard<std::mutex> lock(detail_mutex_);
        status.detail = detail_;
    }
    return status;
}

void TranscoderJobRunner::run() {
    std::vector<RenditionSpec> specs;
    specs.reserve(assignment_.renditions.size());
    for (const auto& rendition : assignment_.renditions) {
        RenditionSpec spec;
        spec.name = rendition.name;
        spec.output_stream = rendition.output_stream;
        spec.width = rendition.width;
        spec.height = rendition.height;
        spec.video_bitrate = rendition.video_bitrate;
        spec.audio_bitrate = rendition.audio_bitrate;
        specs.push_back(std::move(spec));
    }

    RtmpTagConverter converter;
    std::optional<SourceTranscoder> transcoder;
    std::optional<core::Error> pipeline_error;

    // Deferred exactly as HlsSourcePuller/IngestTranscodeManager defer it:
    // the decoder variant is fixed at construction, so it must wait until the
    // source's real codec is known from the first video tag.
    const auto ensure_transcoder = [&](SourceVideoCodec codec) -> bool {
        if (transcoder) return true;
        transcoder.emplace(specs, std::max<std::uint32_t>(assignment_.fps, 1), codec);
        transcoder->set_video_output(
            [this](std::size_t index, const transcoding::native::EncodedAccessUnit& unit) {
                auto& pusher = *pushers_[index];
                if (unit.keyframe && !pusher.video_builder.has_sequence_header()) {
                    auto header = pusher.video_builder.build_sequence_header(unit.annexb);
                    if (header) {
                        HandoffMessage sequence;
                        sequence.video = true;
                        sequence.sequence_header = true;
                        sequence.timestamp = static_cast<std::uint32_t>(unit.dts_90k / 90);
                        sequence.payload = header.value();
                        {
                            std::lock_guard<std::mutex> lock(pusher.priming_mutex);
                            pusher.video_sequence_header = sequence.payload;
                        }
                        pusher.queue.push(std::move(sequence));
                    }
                }
                if (!pusher.video_builder.has_sequence_header()) return;
                auto frame =
                    pusher.video_builder.build_frame(unit.annexb, unit.pts_90k, unit.dts_90k, unit.keyframe);
                if (!frame) return;
                HandoffMessage message;
                message.video = true;
                message.keyframe = unit.keyframe;
                message.timestamp = static_cast<std::uint32_t>(unit.dts_90k / 90);
                message.payload = std::move(frame).value();
                pusher.queue.push(std::move(message));
            });
        transcoder->set_audio_output([this](std::size_t index,
                                           const transcoding::native::EncodedAudioFrame& frame,
                                           std::int64_t pts_90k) {
            auto& pusher = *pushers_[index];
            if (!pusher.audio_builder.has_sequence_header()) {
                auto header = pusher.audio_builder.build_sequence_header(frame.adts);
                if (header) {
                    HandoffMessage sequence;
                    sequence.video = false;
                    sequence.sequence_header = true;
                    sequence.timestamp = static_cast<std::uint32_t>(pts_90k / 90);
                    sequence.payload = header.value();
                    {
                        std::lock_guard<std::mutex> lock(pusher.priming_mutex);
                        pusher.audio_sequence_header = sequence.payload;
                    }
                    pusher.queue.push(std::move(sequence));
                }
            }
            if (!pusher.audio_builder.has_sequence_header()) return;
            auto tag = pusher.audio_builder.build_frame(frame.adts);
            if (!tag) return;
            HandoffMessage message;
            message.video = false;
            message.timestamp = static_cast<std::uint32_t>(pts_90k / 90);
            message.payload = std::move(tag).value();
            pusher.queue.push(std::move(message));
        });
        if (auto started = transcoder->start(); !started) {
            pipeline_error = started.error();
            return false;
        }
        return true;
    };

    while (running_.load()) {
        RtmpSourceClient client(assignment_.source_url);
        state_.store(TranscoderJobRunnerState::Connecting);
        set_detail("connecting to " + assignment_.source_url);

        auto result = client.run(
            [this] { return running_.load(); },
            [&](const protocol::chunk::RtmpMessage& message) -> core::Result<void> {
                using protocol::chunk::MessageTypeId;
                const auto type = static_cast<MessageTypeId>(message.message_type_id);
                if (type == MessageTypeId::Video) {
                    auto unit = converter.convert_video(message.payload, message.timestamp);
                    if (!unit) return unit.error();
                    if (!unit.value()) return {};
                    if (!ensure_transcoder(converter.video_codec())) {
                        return pipeline_error.value_or(core::Error(
                            core::ErrorCode::Unknown, core::ErrorCategory::Internal,
                            "transcoder failed to start"));
                    }
                    const auto& video = *unit.value();
                    return transcoder->on_video(video.annexb, video.pts_90k, video.dts_90k,
                                                video.keyframe);
                }
                if (type == MessageTypeId::Audio) {
                    auto unit = converter.convert_audio(message.payload, message.timestamp);
                    if (!unit) return unit.error();
                    if (!unit.value()) return {};
                    if (!ensure_transcoder(converter.has_video_codec() ? converter.video_codec()
                                                                       : SourceVideoCodec::H264)) {
                        return pipeline_error.value_or(core::Error(
                            core::ErrorCode::Unknown, core::ErrorCategory::Internal,
                            "transcoder failed to start"));
                    }
                    return transcoder->on_audio(unit.value()->adts, unit.value()->pts_90k);
                }
                return {};
            },
            [this] {
                state_.store(TranscoderJobRunnerState::Running);
                set_detail("running");
            });

        if (!running_.load()) break;
        if (!result) {
            state_.store(TranscoderJobRunnerState::Error);
            set_detail(result.error().message());
            RTMP_LOG(observability::LogLevel::Warn, "transcoder-agent", "source pull failed",
                     {{"id", assignment_.id}, {"error", result.error().message()}});
            if (transcoder) transcoder->mark_discontinuity();
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
    state_.store(TranscoderJobRunnerState::Stopped);
}

} // namespace rtmp_server::dispatch
