#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/hls/playlist.hpp"
#include "rtmp_server/hls/segment_store.hpp"
#include "rtmp_server/persistence/store.hpp"
#include "rtmp_server/protocol/chunk/chunk_types.hpp"
#include "rtmp_server/protocol/commands/recorder_sink.hpp"
#include "rtmp_server/media/media_handoff_queue.hpp"
#include "rtmp_server/transcoding/native/source_transcoder.hpp" // RenditionSpec

namespace rtmp_server::transcoding::native {

// One rung of a published stream's ladder: its spec and the segment store its
// transcoded segments are written into. The store is registered with the HLS
// handler exactly like a passthrough publisher's own store, so serving needs
// no new code path.
struct IngestRendition {
    RenditionSpec spec;
    std::shared_ptr<hls::SegmentStore> store;
};

struct IngestTranscodeOptions {
    // Output frame rate for the ladder. The source's own rate is not known
    // until it publishes; SourceTranscoder samples input to this rate.
    std::uint32_t fps = 30;
    // Cores this one publisher's ladder may use for scale+encode (0 = every
    // core). Set per sink by IngestTranscodeManager from the reservation
    // below; a ladder never sizes itself from hardware_concurrency() while
    // other ladders run on the same box.
    std::uint32_t cpu_budget = 0;
    std::vector<unsigned> pinned_cores;
    // Share of the machine reserved for transcoding, as the operator-facing
    // percentage the source-transcode side already uses
    // (ServerConfig::transcode_cpu_reservation_percent). 0 = no reservation:
    // ladders size themselves from every core, the historical behaviour of
    // the pull path. Ladders divide the reserved slice between themselves and
    // are pinned to it, so encoders cannot starve the io_uring workers that
    // carry ingest and delivery.
    std::uint32_t transcode_cpu_reservation_percent = 0;

    // Segment store shape, matching the passthrough ingest path's (6 s
    // segments, 60 s live window) so both surfaces of one publish advertise
    // the same window depth.
    std::uint32_t target_duration_seconds = 6;
    std::uint32_t live_window_segments = 10;
    std::uint32_t retention_grace_segments = 6;
    std::uint64_t max_total_bytes_per_rendition = 256u * 1024u * 1024u;
    std::chrono::milliseconds segment_target_duration{6000};
    std::chrono::milliseconds max_segment_duration{12000};
    // Low-Latency HLS parts, mirroring the passthrough path's config knobs.
    // Zero disables parts.
    std::chrono::milliseconds part_target_duration{0};

    media::HandoffLimits queue_limits;
};

enum class IngestTranscodeState { Starting, Running, Error, Stopped };

struct IngestTranscodeStatus {
    IngestTranscodeState state = IngestTranscodeState::Stopped;
    std::string detail;
    media::HandoffStats queue;
    std::uint64_t frames_in = 0;
    std::uint64_t conversion_errors = 0;
};

// The transcoding half of one publish: RTMP media that arrives on the
// publisher's connection is decoded once and re-encoded into every rung of a
// rendition ladder, each rung packaged as its own HLS stream.
//
// It is a RecorderSink so the existing publish path needs no new concept: the
// composition root hands it to the same factory that already builds the
// passthrough HLS (and DASH) sink, and it is fed and finalized by the same
// publisher lifecycle. Everything expensive happens on this object's own
// worker thread — the publisher's io_uring worker only copies a payload into
// a bounded queue (see media::MediaHandoffQueue for why it drops rather than blocks),
// so a ladder too heavy for the machine costs transcoded quality and never
// ingest or passthrough delivery.
class IngestTranscodeSink final : public protocol::commands::RecorderSink {
public:
    IngestTranscodeSink(std::vector<IngestRendition> renditions, IngestTranscodeOptions options);
    ~IngestTranscodeSink() override;
    IngestTranscodeSink(const IngestTranscodeSink&) = delete;
    IngestTranscodeSink& operator=(const IngestTranscodeSink&) = delete;

    // RTMP metadata carries the publisher's declared geometry/bitrate, none of
    // which survives re-encoding: every rung's real parameters come from its
    // own encoder. Accepted and ignored, so the sink is still a drop-in
    // RecorderSink.
    void on_metadata(const protocol::chunk::RtmpMessage& message) override;
    void on_audio(const protocol::chunk::RtmpMessage& message) override;
    void on_video(const protocol::chunk::RtmpMessage& message) override;
    // Stops the worker, flushes each rung's final segment and closes its
    // playlist. Safe to call more than once (the publisher path may call it
    // on deleteStream and again on disconnect).
    void finalize() override;

    [[nodiscard]] IngestTranscodeStatus status() const;

private:
    void run();
    void set_detail(std::string detail);

    std::vector<IngestRendition> renditions_;
    IngestTranscodeOptions options_;
    media::MediaHandoffQueue queue_;
    std::thread worker_;
    std::atomic<IngestTranscodeState> state_{IngestTranscodeState::Starting};
    std::atomic<std::uint64_t> frames_in_{0};
    std::atomic<std::uint64_t> conversion_errors_{0};
    std::atomic<bool> finalized_{false};
    mutable std::mutex detail_mutex_;
    std::string detail_;
};

// A persisted "transcode what is published to this stream" rule: the same
// opaque PresetCatalogue rules blob a source-transcode job carries, resolved
// into the rendition ladder a publish should produce.
struct IngestAssignment {
    std::string application;
    std::string source_stream;
    std::string template_name;
    std::string rules;
    std::string master_hls_path;
    std::vector<RenditionSpec> renditions;
    // Whether a publisher is currently being transcoded under this
    // assignment. An assignment with no publisher is configuration only.
    bool active = false;
    // The live ladder's own state, meaningful only while `active`. Carried
    // here so the management API can report a ladder that is dropping frames
    // or has failed, rather than only that one exists.
    IngestTranscodeStatus status;
};

// Owns the transcoding assignments and turns a matching publish into a
// ladder.
//
// The manager, not the sink, owns the segment stores: a publisher that drops
// and reconnects (or an encoder restart) must not empty the live window, or
// every viewer's playlist goes empty for a whole startup runway over what may
// have been a two-second reconnect. Stores are therefore retained per stream
// and handed to the next sink, which resumes their media sequence.
class IngestTranscodeManager {
public:
    // Registration of a ladder's output streams with the HLS handler, kept as
    // hooks so this component does not depend on the control layer (the same
    // arrangement SourceJobManager uses).
    struct Hooks {
        std::function<void(const std::string& application, const std::string& master,
                           std::vector<hls::Rendition> renditions)>
            set_renditions;
        std::function<void(const std::string& application, const std::string& stream,
                           std::shared_ptr<hls::SegmentStore> store)>
            register_output;
        std::function<void(const std::string& application, const std::string& stream)>
            unregister_output;
    };

    IngestTranscodeManager(Hooks hooks, persistence::Store* store, IngestTranscodeOptions options,
                           std::string hls_route_prefix = "/hls");
    ~IngestTranscodeManager();

    // Rebuilds the assignments persisted by a previous run. Rows whose rules
    // no longer parse are skipped rather than failing startup: an operator
    // must not lose the whole server to one bad stored blob.
    void load_from_store();

    // Called from the publish path's recorder factory. Returns nullptr when no
    // assignment covers this stream, which is the common case — an
    // unassigned publish keeps exactly the passthrough behaviour it had.
    [[nodiscard]] std::shared_ptr<IngestTranscodeSink> create_sink(std::string_view application,
                                                                   std::string_view stream);

    [[nodiscard]] core::Result<std::string> upsert(std::string_view application,
                                                   std::string_view source_stream,
                                                   std::string_view template_name,
                                                   std::string_view rules);
    [[nodiscard]] core::Result<void> remove(std::string_view application,
                                            std::string_view source_stream);
    // Takes a stream's ladder off the air without touching its assignment:
    // stops the sink and unregisters every rung's playlist. For a stream the
    // operator deleted — the assignment may still be recreated later, but its
    // outputs must not outlive the stream they were derived from.
    void release(std::string_view application, std::string_view stream);
    [[nodiscard]] std::vector<IngestAssignment> list(std::string_view application) const;
    [[nodiscard]] std::optional<IngestAssignment> find(std::string_view application,
                                                       std::string_view source_stream) const;

    // Ladders currently attached to a live publisher, for /metrics and the
    // management API.
    [[nodiscard]] std::size_t active_ladder_count() const;

    // Largest ladder one publish may produce. A ladder is decode-once,
    // encode-per-rung, so the bound is about CPU per publisher rather than
    // memory.
    static constexpr std::size_t kMaxRenditionsPerStream = 8;

private:
    struct StreamState {
        std::vector<IngestRendition> renditions;
        std::weak_ptr<IngestTranscodeSink> sink;
        bool registered = false;
    };

    [[nodiscard]] static std::string key_of(std::string_view application, std::string_view stream);
    [[nodiscard]] std::string master_path(std::string_view application,
                                          std::string_view stream) const;
    void build_renditions_locked(StreamState& state, const std::vector<RenditionSpec>& specs);
    void register_outputs_locked(const std::string& application, const StreamState& state,
                                 const std::string& stream);
    void unregister_outputs_locked(const std::string& application, const StreamState& state);
    // Per-publisher share of the reserved transcode budget, so N concurrent
    // transcoded publishes do not each size themselves from the whole box.
    [[nodiscard]] std::uint32_t cpu_budget_for_locked() const;

    Hooks hooks_;
    persistence::Store* store_ = nullptr;
    IngestTranscodeOptions options_;
    std::string route_prefix_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, IngestAssignment> assignments_;
    std::unordered_map<std::string, StreamState> streams_;
};

} // namespace rtmp_server::transcoding::native
