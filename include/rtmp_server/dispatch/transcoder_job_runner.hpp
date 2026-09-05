#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rtmp_server/dispatch/transcoder_job.hpp"
#include "rtmp_server/media/media_handoff_queue.hpp"

namespace rtmp_server::dispatch {

// Runs one dispatched job entirely on this (transcoder) node: pulls
// `source_url` over RTMP, decodes once and re-encodes every rendition (the
// same SourceTranscoder core a local source-transcode job uses), and pushes
// each rung back to the origin over its own RTMP publish. No HLS packaging
// and no HTTP serving happens here at all — this node's only network
// surface toward viewers is none; its only outbound traffic is the pushes
// and, incidentally, the pull.
//
// Pipeline per job (docs/transcoder-dispatch.md):
//   RtmpSourceClient (pull)
//     -> RtmpTagConverter (FLV tag -> Annex B / ADTS)
//     -> SourceTranscoder (decode once, encode per rendition)
//     -> RtmpVideoTagBuilder / RtmpAudioTagBuilder (Annex B / ADTS -> FLV tag)
//     -> one MediaHandoffQueue + RtmpPushClient per rendition
//
// Every stage except the two tag converters already exists and is used
// elsewhere in this codebase (the pull path mirrors HlsSourcePuller's RTMP
// branch; the push path mirrors StreamTargetSink) — this class is the glue,
// not a new pipeline.
class TranscoderJobRunner final : public TranscoderJob {
public:
    explicit TranscoderJobRunner(TranscoderJobAssignment assignment);
    ~TranscoderJobRunner();
    TranscoderJobRunner(const TranscoderJobRunner&) = delete;
    TranscoderJobRunner& operator=(const TranscoderJobRunner&) = delete;

    // Stops the pull and every rendition's push; safe to call more than once.
    void stop() override;

    [[nodiscard]] TranscoderJobRunnerStatus status() const override;

private:
    void run();
    void set_detail(std::string detail);

    TranscoderJobAssignment assignment_;
    std::atomic<bool> running_{true};
    std::atomic<TranscoderJobRunnerState> state_{TranscoderJobRunnerState::Connecting};
    std::atomic<std::uint64_t> bytes_pushed_{0};
    mutable std::mutex detail_mutex_;
    std::string detail_ = "connecting to source";

    // One push queue + pusher thread per rendition, run for the job's whole
    // life; the pull thread (started last, joined first) feeds all of them.
    struct RenditionPusher;
    std::vector<std::unique_ptr<RenditionPusher>> pushers_;
    std::thread pull_thread_;
};

} // namespace rtmp_server::dispatch
