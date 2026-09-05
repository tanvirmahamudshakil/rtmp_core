#include "rtmp_server/transcoding/native/ingest_transcoder.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <sstream>
#include <thread>
#include <utility>

#include "rtmp_server/core/cpu_partition.hpp"
#include "rtmp_server/hls/rendition_feed.hpp"
#include "rtmp_server/hls/segmenter.hpp"
#include "rtmp_server/observability/logger.hpp"
#include "rtmp_server/protocol/media/media_ingest.hpp"
#include "rtmp_server/transcoding/native/codec_tags.hpp"
#include "rtmp_server/transcoding/native/rtmp_tag_converter.hpp"
#include "rtmp_server/transcoding/native/source_job_manager.hpp" // parse_source_job_renditions

namespace rtmp_server::transcoding::native {
namespace {

namespace protocol_media = rtmp_server::protocol::media;

core::Error assignment_error(std::string message) {
    return core::Error(core::ErrorCode::InvalidConfiguration, core::ErrorCategory::Configuration,
                       std::move(message));
}

// BANDWIDTH is a peak, not the encoder's target: MPEG-TS/PES overhead,
// keyframe bursts and rate-control variation all sit above the nominal rate,
// and an ABR client that believes the nominal figure picks a rung it cannot
// sustain. Same 25% reservation the source-job ladder declares.
std::uint64_t peak_hls_bandwidth(std::uint64_t average) {
    constexpr std::uint64_t kPeakPercent = 125;
    if (average > std::numeric_limits<std::uint64_t>::max() / kPeakPercent) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return (average * kPeakPercent + 99) / 100;
}

std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const char c : value) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result.push_back(c); break;
        }
    }
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// IngestTranscodeSink
// ---------------------------------------------------------------------------

IngestTranscodeSink::IngestTranscodeSink(std::vector<IngestRendition> renditions,
                                         IngestTranscodeOptions options)
    : renditions_(std::move(renditions)),
      options_(options),
      queue_(options.queue_limits) {
    options_.fps = std::max<std::uint32_t>(options_.fps, 1);
    set_detail("waiting for the first keyframe");
    worker_ = std::thread([this] { run(); });
}

IngestTranscodeSink::~IngestTranscodeSink() { finalize(); }

void IngestTranscodeSink::set_detail(std::string detail) {
    std::lock_guard<std::mutex> lock(detail_mutex_);
    detail_ = std::move(detail);
}

void IngestTranscodeSink::on_metadata(const protocol::chunk::RtmpMessage&) {}

void IngestTranscodeSink::on_audio(const protocol::chunk::RtmpMessage& message) {
    if (finalized_.load(std::memory_order_acquire)) return;
    media::HandoffMessage queued;
    queued.video = false;
    queued.timestamp = message.timestamp;
    queued.payload.assign(message.payload.begin(), message.payload.end());
    queue_.push(std::move(queued));
}

void IngestTranscodeSink::on_video(const protocol::chunk::RtmpMessage& message) {
    if (finalized_.load(std::memory_order_acquire)) return;
    media::HandoffMessage queued;
    queued.video = true;
    queued.timestamp = message.timestamp;
    // Classified here, on the publisher's thread, because the drop policy in
    // the queue has to know whether this message is a resynchronisation point
    // before it decides what to discard. This is a few byte tests, not a
    // parse: the same call LiveFanout already makes for its GOP cache.
    if (const auto info = protocol_media::classify_video_tag(message.payload)) {
        queued.keyframe = info->frame_type == protocol_media::VideoFrameType::KeyFrame ||
                          info->frame_type == protocol_media::VideoFrameType::GeneratedKeyFrame;
        queued.sequence_header =
            (info->avc_packet_type &&
             *info->avc_packet_type == protocol_media::AvcPacketType::SequenceHeader) ||
            (info->ex_packet_type &&
             *info->ex_packet_type == protocol_media::ExVideoPacketType::SequenceStart);
    }
    queued.payload.assign(message.payload.begin(), message.payload.end());
    queue_.push(std::move(queued));
}

void IngestTranscodeSink::finalize() {
    if (finalized_.exchange(true, std::memory_order_acq_rel)) return;
    queue_.close();
    if (worker_.joinable()) worker_.join();
    if (state_.load() != IngestTranscodeState::Error) {
        state_.store(IngestTranscodeState::Stopped);
    }
}

IngestTranscodeStatus IngestTranscodeSink::status() const {
    IngestTranscodeStatus status;
    status.state = state_.load();
    status.queue = queue_.stats();
    status.frames_in = frames_in_.load();
    status.conversion_errors = conversion_errors_.load();
    {
        std::lock_guard<std::mutex> lock(detail_mutex_);
        status.detail = detail_;
    }
    return status;
}

void IngestTranscodeSink::run() {
    if (!options_.pinned_cores.empty()) {
        // Confines this ladder's own thread — and, transitively, the encoder
        // threads x264/fdk-aac start from it — to the reserved core set, so
        // transcoding cannot starve the io_uring workers that serve ingest
        // and delivery.
        core::pin_current_thread_to_cores(options_.pinned_cores);
    }

    // Each rung owns a segmenter that writes straight into its store. Unlike
    // the pull path there is no paced publisher here: a publisher feeds this
    // ladder in real time already, so segments are produced at the rate
    // viewers consume them and pacing would only add latency.
    std::vector<std::unique_ptr<hls::Segmenter>> segmenters;
    std::vector<std::unique_ptr<hls::RenditionFeed>> feeds;
    segmenters.reserve(renditions_.size());
    feeds.reserve(renditions_.size());

    // Segment URLs are immutable at the cache tier. A process restart loses
    // the in-memory store, so restarting at segment 0 would hand active
    // sessions stale cached bytes under a name that now means something else.
    // A wall-clock floor keeps first-start names unique across processes; a
    // retained store continues from whichever value is higher.
    const auto sequence_floor = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    for (auto& rendition : renditions_) {
        hls::SegmenterConfig config;
        config.target_duration = options_.segment_target_duration;
        config.max_segment_duration = options_.max_segment_duration;
        if (options_.part_target_duration.count() > 0) {
            config.part_target_duration = options_.part_target_duration;
        }
        config.initial_sequence = std::max(rendition.store->next_sequence(), sequence_floor);
        auto store = rendition.store;
        auto segmenter = std::make_unique<hls::Segmenter>(
            [store](hls::SegmentPtr segment) { store->add_segment(std::move(segment)); }, config);
        if (options_.part_target_duration.count() > 0) {
            segmenter->set_part_callback([store](hls::PartPtr part) { store->add_part(std::move(part)); });
        }
        feeds.push_back(std::make_unique<hls::RenditionFeed>(*segmenter));
        segmenters.push_back(std::move(segmenter));
    }

    std::vector<RenditionSpec> specs;
    specs.reserve(renditions_.size());
    for (const auto& rendition : renditions_) specs.push_back(rendition.spec);

    // Deferred until the publisher's real codec is known: the decoder is
    // selected once at construction and never switched, so building it before
    // the first video tag would silently assume H.264 for an HEVC publish.
    std::optional<SourceTranscoder> transcoder;
    auto ensure_transcoder = [&](SourceVideoCodec codec) -> core::Result<void> {
        if (transcoder) return {};
        transcoder.emplace(specs, options_.fps, codec, options_.cpu_budget, options_.pinned_cores);
        transcoder->set_video_output([&](std::size_t index, const EncodedAccessUnit& unit) {
            feeds[index]->push_video(unit.annexb, unit.pts_90k, unit.dts_90k, unit.keyframe);
        });
        transcoder->set_audio_output(
            [&](std::size_t index, const EncodedAudioFrame& frame, std::int64_t pts_90k) {
                feeds[index]->push_audio(frame.adts, pts_90k);
            });
        return transcoder->start();
    };

    RtmpTagConverter converter;
    media::HandoffMessage message;
    bool failed = false;

    while (queue_.pop(message)) {
        if (queue_.take_resync()) {
            // Frames were dropped to keep the publisher unblocked. Re-anchor
            // the transcoder's input sampling over the gap; its output clock
            // stays monotonic, so players see a short skip rather than a
            // rewind or a frozen picture.
            if (transcoder) transcoder->mark_discontinuity();
        }

        auto converted = [&]() -> core::Result<void> {
            if (message.video) {
                auto unit = converter.convert_video(message.payload, message.timestamp);
                if (!unit) return unit.error();
                if (!unit.value()) return {}; // sequence header or non-picture tag
                if (auto ready = ensure_transcoder(converter.video_codec()); !ready) {
                    return ready.error();
                }
                const auto& video = *unit.value();
                frames_in_.fetch_add(1, std::memory_order_relaxed);
                return transcoder->on_video(video.annexb, video.pts_90k, video.dts_90k,
                                            video.keyframe);
            }
            auto unit = converter.convert_audio(message.payload, message.timestamp);
            if (!unit) return unit.error();
            if (!unit.value()) return {};
            // Audio can arrive before the first video tag has identified the
            // codec. H.264 is the overwhelmingly common publish and is what
            // the pull path assumes in the same situation.
            if (auto ready = ensure_transcoder(converter.has_video_codec()
                                                   ? converter.video_codec()
                                                   : SourceVideoCodec::H264);
                !ready) {
                return ready.error();
            }
            return transcoder->on_audio(unit.value()->adts, unit.value()->pts_90k);
        }();

        if (!converted) {
            // A malformed or out-of-order tag is the publisher's problem and
            // is survivable — the next keyframe resynchronises the ladder —
            // so it is counted, not fatal. Once the transcoder itself exists
            // and reports a failure, the encoders are gone and continuing
            // would only spin.
            const auto errors = conversion_errors_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (transcoder && converted.error().category() == core::ErrorCategory::Internal) {
                set_detail(converted.error().message());
                state_.store(IngestTranscodeState::Error);
                failed = true;
                break;
            }
            if (errors == 1 || errors % 100 == 0) {
                RTMP_LOG(observability::LogLevel::Warn, "ingest-transcoder", "media tag rejected",
                         {{"errors", std::to_string(errors)},
                          {"error", converted.error().message()}});
            }
            continue;
        }

        if (state_.load() == IngestTranscodeState::Starting && frames_in_.load() > 0) {
            state_.store(IngestTranscodeState::Running);
            set_detail("running");
        }
    }

    for (auto& segmenter : segmenters) segmenter->finalize();
    for (auto& rendition : renditions_) {
        // A failed ladder must not advertise EXT-X-ENDLIST: the publisher may
        // still be live on the passthrough surface, and the stores are reused
        // by the next publish.
        if (!failed) rendition.store->mark_ended();
    }
}

// ---------------------------------------------------------------------------
// IngestTranscodeManager
// ---------------------------------------------------------------------------

IngestTranscodeManager::IngestTranscodeManager(Hooks hooks, persistence::Store* store,
                                               IngestTranscodeOptions options,
                                               std::string hls_route_prefix)
    : hooks_(std::move(hooks)),
      store_(store),
      options_(options),
      route_prefix_(std::move(hls_route_prefix)) {}

IngestTranscodeManager::~IngestTranscodeManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [key, state] : streams_) {
        if (auto sink = state.sink.lock()) sink->finalize();
    }
}

std::string IngestTranscodeManager::key_of(std::string_view application, std::string_view stream) {
    return std::string(application) + "/" + std::string(stream);
}

std::string IngestTranscodeManager::master_path(std::string_view application,
                                                std::string_view stream) const {
    return route_prefix_ + "/" + std::string(application) + "/" + std::string(stream) +
           "/master.m3u8";
}

void IngestTranscodeManager::load_from_store() {
    if (store_ == nullptr) return;
    auto rows = store_->load_transcoding_assignments();
    if (!rows) {
        RTMP_LOG(observability::LogLevel::Warn, "ingest-transcoder", "assignment load failed",
                 {{"error", rows.error().message()}});
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& row : rows.value()) {
        auto renditions = parse_source_job_renditions(row.rules);
        if (!renditions) {
            RTMP_LOG(observability::LogLevel::Warn, "ingest-transcoder", "assignment skipped",
                     {{"application", row.application},
                      {"stream", row.source_stream},
                      {"error", renditions.error().message()}});
            continue;
        }
        IngestAssignment assignment;
        assignment.application = row.application;
        assignment.source_stream = row.source_stream;
        assignment.template_name = row.template_name;
        assignment.rules = row.rules;
        assignment.master_hls_path = master_path(row.application, row.source_stream);
        assignment.renditions = std::move(renditions).value();
        assignments_[key_of(row.application, row.source_stream)] = std::move(assignment);
    }
}

core::Result<std::string> IngestTranscodeManager::upsert(std::string_view application,
                                                         std::string_view source_stream,
                                                         std::string_view template_name,
                                                         std::string_view rules) {
    if (application.empty() || source_stream.empty()) {
        return assignment_error("application and source stream are required");
    }
    auto parsed = parse_source_job_renditions(rules);
    if (!parsed) return parsed.error();
    auto renditions = std::move(parsed).value();
    if (renditions.empty()) return assignment_error("assignment has no renditions");
    if (renditions.size() > kMaxRenditionsPerStream) {
        return assignment_error("too many renditions for one published stream");
    }
    for (std::size_t i = 0; i < renditions.size(); ++i) {
        if (renditions[i].output_stream.empty()) {
            return assignment_error("every rendition needs an outgoing stream name");
        }
        // The publisher's own name already serves the untranscoded
        // passthrough playlist. A rung claiming it would replace that
        // registration and leave the source rendition unreachable.
        if (renditions[i].output_stream == source_stream) {
            return assignment_error("a rendition may not reuse the source stream name");
        }
        for (std::size_t j = i + 1; j < renditions.size(); ++j) {
            if (renditions[i].output_stream == renditions[j].output_stream) {
                return assignment_error("two renditions share one outgoing stream name");
            }
        }
    }

    IngestAssignment assignment;
    assignment.application = std::string(application);
    assignment.source_stream = std::string(source_stream);
    assignment.template_name = std::string(template_name);
    assignment.rules = std::string(rules);
    assignment.master_hls_path = master_path(application, source_stream);
    assignment.renditions = std::move(renditions);

    if (store_ != nullptr) {
        persistence::TranscodingAssignmentRow row;
        row.application = assignment.application;
        row.source_stream = assignment.source_stream;
        row.template_name = assignment.template_name;
        row.rules = assignment.rules;
        if (auto saved = store_->upsert_transcoding_assignment(row); !saved) return saved.error();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto key = key_of(application, source_stream);
        // An assignment change reshapes the ladder, so the ladder currently on
        // air no longer matches it. Stopping the sink here ends the old rungs
        // cleanly; the next publish builds the new ones. Applying it to a
        // publisher already streaming would need the encoders rebuilt
        // mid-GOP, which is a restart either way.
        const auto existing = streams_.find(key);
        if (existing != streams_.end()) {
            if (auto sink = existing->second.sink.lock()) sink->finalize();
            unregister_outputs_locked(assignment.application, existing->second);
            streams_.erase(existing);
        }
        assignments_[key] = assignment;
    }

    std::ostringstream os;
    os << R"({"application":")" << json_escape(assignment.application) << R"(","source_stream":")"
       << json_escape(assignment.source_stream) << R"(","template_name":")"
       << json_escape(assignment.template_name) << R"(","master_hls_path":")"
       << json_escape(assignment.master_hls_path) << R"(","outputs":[)";
    for (std::size_t i = 0; i < assignment.renditions.size(); ++i) {
        const auto& rendition = assignment.renditions[i];
        if (i) os << ',';
        os << R"({"name":")" << json_escape(rendition.name) << R"(","stream":")"
           << json_escape(rendition.output_stream) << R"(","video_codec":"h264","video_bitrate":)"
           << rendition.video_bitrate << R"(,"audio_bitrate":)" << rendition.audio_bitrate
           << R"(,"width":)" << rendition.width << R"(,"height":)" << rendition.height << '}';
    }
    os << "]}";
    return os.str();
}

core::Result<void> IngestTranscodeManager::remove(std::string_view application,
                                                  std::string_view source_stream) {
    const auto key = key_of(application, source_stream);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (assignments_.erase(key) == 0) {
            return core::Error(core::ErrorCode::NotFound, core::ErrorCategory::Configuration,
                               "no such transcoding assignment");
        }
        const auto state = streams_.find(key);
        if (state != streams_.end()) {
            if (auto sink = state->second.sink.lock()) sink->finalize();
            unregister_outputs_locked(std::string(application), state->second);
            streams_.erase(state);
        }
    }
    if (store_ != nullptr) {
        if (auto deleted = store_->delete_transcoding_assignment(application, source_stream);
            !deleted) {
            return deleted.error();
        }
    }
    return {};
}

void IngestTranscodeManager::release(std::string_view application, std::string_view stream) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto state = streams_.find(key_of(application, stream));
    if (state == streams_.end()) return;
    if (auto sink = state->second.sink.lock()) sink->finalize();
    unregister_outputs_locked(std::string(application), state->second);
    streams_.erase(state);
}

std::vector<IngestAssignment> IngestTranscodeManager::list(std::string_view application) const {
    std::vector<IngestAssignment> result;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [key, assignment] : assignments_) {
        if (!application.empty() && assignment.application != application) continue;
        auto copy = assignment;
        const auto state = streams_.find(key);
        if (state != streams_.end()) {
            if (auto sink = state->second.sink.lock()) {
                copy.active = true;
                copy.status = sink->status();
            }
        }
        result.push_back(std::move(copy));
    }
    std::ranges::sort(result, [](const IngestAssignment& a, const IngestAssignment& b) {
        return std::tie(a.application, a.source_stream) < std::tie(b.application, b.source_stream);
    });
    return result;
}

std::optional<IngestAssignment> IngestTranscodeManager::find(std::string_view application,
                                                             std::string_view source_stream) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = assignments_.find(key_of(application, source_stream));
    if (it == assignments_.end()) return std::nullopt;
    return it->second;
}

std::size_t IngestTranscodeManager::active_ladder_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& [key, state] : streams_) {
        if (!state.sink.expired()) ++count;
    }
    return count;
}

std::uint32_t IngestTranscodeManager::cpu_budget_for_locked() const {
    // With a reservation configured, ladders divide only the reserved slice,
    // never the whole machine: their combined encoder threads must not spill
    // onto the cores ingest, HTTP and the admin API are confined to.
    //
    // Source-transcode jobs divide the same slice from their own manager, so a
    // box running both kinds of transcode oversubscribes the reservation
    // between them. Sizing them together needs one process-wide budget owner,
    // which neither side has today.
    const auto partition = core::compute_cpu_partition(options_.transcode_cpu_reservation_percent);
    const std::uint32_t cores =
        !partition.transcode_cores.empty()
            ? static_cast<std::uint32_t>(partition.transcode_cores.size())
            : (std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 1);

    std::uint32_t live = 1; // the ladder about to start
    for (const auto& [key, state] : streams_) {
        (void)key;
        if (!state.sink.expired()) ++live;
    }
    // Never hand an encoder a budget of zero cores: it would size itself from
    // the whole machine again, which is what the reservation exists to
    // prevent.
    return std::max<std::uint32_t>(1, cores / live);
}

void IngestTranscodeManager::build_renditions_locked(StreamState& state,
                                                     const std::vector<RenditionSpec>& specs) {
    // Stores outlive the sink that fills them: a publisher that reconnects
    // finds its live window intact and resumes the media sequence, instead of
    // every viewer's playlist going empty for a fresh startup runway over a
    // two-second reconnect.
    if (state.renditions.size() == specs.size()) {
        bool same_outputs = true;
        for (std::size_t i = 0; i < specs.size(); ++i) {
            if (state.renditions[i].store == nullptr ||
                state.renditions[i].spec.output_stream != specs[i].output_stream) {
                same_outputs = false;
                break;
            }
        }
        if (same_outputs) {
            for (std::size_t i = 0; i < specs.size(); ++i) {
                state.renditions[i].spec = specs[i];
                // The previous sink closed the playlist with EXT-X-ENDLIST.
                // Reopen it: the window is about to start moving again.
                state.renditions[i].store->mark_live();
            }
            return;
        }
    }

    state.renditions.clear();
    state.renditions.reserve(specs.size());
    for (const auto& spec : specs) {
        hls::SegmentStoreConfig config;
        config.live_window_segments = options_.live_window_segments;
        config.retention_grace_segments = options_.retention_grace_segments;
        config.max_total_bytes = options_.max_total_bytes_per_rendition;
        config.target_duration_seconds = options_.target_duration_seconds;
        // Every rung is fully re-encoded onto one re-anchored, monotonic
        // output timeline, so a publisher reconnect does not need
        // EXT-X-DISCONTINUITY — which players handle by resetting the
        // decoder, visible as a stall on each occurrence.
        config.seamless_fallback_recovery = true;
        IngestRendition rendition;
        rendition.spec = spec;
        rendition.store = std::make_shared<hls::SegmentStore>(config);
        state.renditions.push_back(std::move(rendition));
    }
}

void IngestTranscodeManager::register_outputs_locked(const std::string& application,
                                                     const StreamState& state,
                                                     const std::string& stream) {
    if (hooks_.register_output) {
        for (const auto& rendition : state.renditions) {
            hooks_.register_output(application, rendition.spec.output_stream, rendition.store);
        }
    }
    if (!hooks_.set_renditions) return;

    std::vector<hls::Rendition> master;
    master.reserve(state.renditions.size());
    const auto fps = std::max<std::uint32_t>(options_.fps, 1);
    for (const auto& rendition : state.renditions) {
        const auto& spec = rendition.spec;
        hls::Rendition entry;
        // The master sits at <app>/<stream>/master.m3u8 and every rung is its
        // own stream directory beside it.
        entry.uri = "../" + spec.output_stream + "/index.m3u8";
        entry.average_bandwidth = spec.video_bitrate + spec.audio_bitrate;
        entry.bandwidth = peak_hls_bandwidth(entry.average_bandwidth);
        entry.codecs = hls_codecs_attribute(spec.width, spec.height, fps, spec.audio_bitrate);
        entry.width = spec.width;
        entry.height = spec.height;
        entry.frame_rate = static_cast<double>(fps);
        entry.name = spec.name;
        master.push_back(std::move(entry));
    }
    hooks_.set_renditions(application, stream, std::move(master));
}

void IngestTranscodeManager::unregister_outputs_locked(const std::string& application,
                                                       const StreamState& state) {
    if (!hooks_.unregister_output) return;
    for (const auto& rendition : state.renditions) {
        hooks_.unregister_output(application, rendition.spec.output_stream);
    }
}

std::shared_ptr<IngestTranscodeSink> IngestTranscodeManager::create_sink(std::string_view application,
                                                                         std::string_view stream) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto key = key_of(application, stream);
    const auto assignment = assignments_.find(key);
    if (assignment == assignments_.end()) return nullptr;

    auto& state = streams_[key];
    // A publisher reconnecting before the old connection's teardown ran would
    // otherwise leave two ladders writing into one set of stores.
    if (auto previous = state.sink.lock()) previous->finalize();

    build_renditions_locked(state, assignment->second.renditions);

    auto options = options_;
    options.cpu_budget = cpu_budget_for_locked();
    options.pinned_cores =
        core::compute_cpu_partition(options_.transcode_cpu_reservation_percent).transcode_cores;
    auto sink = std::make_shared<IngestTranscodeSink>(state.renditions, options);
    state.sink = sink;

    // Registration is idempotent (the handler keys a store by app/stream), and
    // re-publishing the master on every publish is what keeps its labels and
    // bandwidths correct after an assignment change.
    register_outputs_locked(assignment->second.application, state, std::string(stream));
    state.registered = true;
    return sink;
}

} // namespace rtmp_server::transcoding::native
