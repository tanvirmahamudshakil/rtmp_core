#include "rtmp_server/transcoding/native/hls_source_puller.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "rtmp_server/core/cpu_partition.hpp"
#include "rtmp_server/media/aac/adts.hpp"
#include "rtmp_server/media/h264/avc.hpp"
#include "rtmp_server/media/hevc/hevc.hpp"
#include "rtmp_server/observability/logger.hpp"
#include "rtmp_server/protocol/media/media_ingest.hpp"
#include "rtmp_server/transcoding/native/hls_playlist.hpp"
#include "rtmp_server/transcoding/native/paced_segment_publisher.hpp"
#include "rtmp_server/transcoding/native/rtmp_source_client.hpp"

namespace rtmp_server::transcoding::native {

namespace {
using observability::LogLevel;
namespace protocol_media = rtmp_server::protocol::media;

// Many IPTV/CDN panels answer an .m3u8-shaped URL with a redirect straight to
// a continuously-flowing raw MPEG-TS body (no playlist, connection never
// closes on its own) rather than an HLS playlist. Detect that from a small
// peek by checking for the TS sync byte at packet boundaries.
bool looks_like_raw_ts(std::span<const std::byte> data) {
    if (data.empty() || static_cast<unsigned char>(data[0]) != 0x47) return false;
    if (data.size() > 188 && static_cast<unsigned char>(data[188]) != 0x47) return false;
    return true;
}

// How much of a live playlist's trailing media to ingest on the first poll.
// Enough to fill PacedSegmentPublisher's cold-start runway in one go (so the
// output goes live immediately rather than waiting for the source to publish
// another runway's worth in real time), and nothing beyond it -- everything
// older is history no viewer of a live stream wants to sit through. RFC 8216
// 6.3.3's "start three target durations from the end" is the floor here; the
// runway is normally the larger of the two.
constexpr double kInitialLiveEdgeSeconds = 35.0;
constexpr std::size_t kMinInitialLiveEdgeSegments = 3;
// Sequences kept in the "already ingested" set below the current playlist
// window. Enough that a source briefly re-advertising an older segment is
// still recognised, while the set stays bounded on a job that runs for
// weeks (it used to grow by one entry per segment, forever).
constexpr std::uint64_t kSeenSequenceHistory = 64;
// Most media one poll may ingest, after the initial live-edge join, before
// the puller decides it is not keeping up and jumps forward. A healthy poll
// turns up one publish interval's worth; a minute of backlog means the
// pipeline is behind real time (transcode slower than the source publishes,
// or a long stall), and transcoding all of it only puts it further behind
// while every viewer watches history. Expressed in seconds rather than
// segments so it means the same thing for a 2-second and a 15-second source.
constexpr double kMaxCatchUpSeconds = 60.0;

constexpr auto kInputProgressTimeout = std::chrono::seconds(30);
constexpr auto kOutputStartupTimeout = std::chrono::seconds(60);
constexpr auto kOutputProgressTimeout = std::chrono::seconds(30);

bool is_rtmp_source(std::string_view url) { return url.starts_with("rtmp://"); }

// Fetches HLS media segments concurrently, reusing one libcurl handle per
// worker across every request.
//
// The previous shape spawned a std::async thread per segment, each
// constructing its own HttpClient. That paid a fresh DNS lookup, TCP connect
// and TLS handshake for every single segment -- on an HTTPS IPTV source that
// is easily more wall-clock time than the transfer itself, and it created
// (and destroyed) an unbounded number of OS threads per playlist poll on a
// box that may be running many jobs at once. Long-lived handles let libcurl
// keep the connection alive between segments, which is what the source's own
// CDN expects.
class SegmentFetchPool {
public:
    using Body = core::Result<std::vector<std::byte>>;

    explicit SegmentFetchPool(std::size_t workers) {
        workers = std::clamp<std::size_t>(workers, 1, 8);
        clients_.reserve(workers);
        for (std::size_t i = 0; i < workers; ++i) clients_.push_back(std::make_unique<HttpClient>());
        threads_.reserve(workers);
        for (std::size_t i = 0; i < workers; ++i) threads_.emplace_back([this, i] { worker(i); });
    }

    ~SegmentFetchPool() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        work_cv_.notify_all();
        for (auto& thread : threads_) {
            if (thread.joinable()) thread.join();
        }
    }

    SegmentFetchPool(const SegmentFetchPool&) = delete;
    SegmentFetchPool& operator=(const SegmentFetchPool&) = delete;

    // Fetches every URL and returns the bodies in the same order. Blocks
    // until all of them have completed; `should_continue` is polled before
    // each fetch so a stopping job abandons the rest of the batch instead of
    // waiting out every remaining timeout.
    [[nodiscard]] std::vector<Body> fetch_all(const std::vector<std::string>& urls,
                                              const std::function<bool()>& should_continue) {
        std::vector<Body> out;
        if (urls.empty()) return out;
        {
            std::lock_guard lock(mutex_);
            urls_ = &urls;
            should_continue_ = &should_continue;
            results_.assign(urls.size(), std::nullopt);
            next_ = 0;
            remaining_ = urls.size();
        }
        work_cv_.notify_all();

        std::unique_lock lock(mutex_);
        done_cv_.wait(lock, [this] { return remaining_ == 0; });
        urls_ = nullptr;
        should_continue_ = nullptr;
        out.reserve(results_.size());
        for (auto& result : results_) out.push_back(std::move(*result));
        results_.clear();
        return out;
    }

private:
    void worker(std::size_t index) {
        std::unique_lock lock(mutex_);
        for (;;) {
            work_cv_.wait(lock, [this] {
                return stopping_ || (urls_ != nullptr && next_ < urls_->size());
            });
            if (stopping_) return;

            const std::size_t slot = next_++;
            const std::string url = (*urls_)[slot];
            const bool proceed = should_continue_ == nullptr || (*should_continue_)();
            lock.unlock();

            Body body = fetch_one(*clients_[index], url, proceed);

            lock.lock();
            results_[slot] = std::move(body);
            if (--remaining_ == 0) done_cv_.notify_one();
        }
    }

    static Body fetch_one(HttpClient& client, const std::string& url, bool proceed) {
        if (!proceed) {
            return core::Error(core::ErrorCode::OperationCanceled, core::ErrorCategory::Network,
                               "source job stopping");
        }
        std::vector<std::byte> bytes;
        auto result = client.get(url, bytes);
        if (!result) {
            // A segment is often still available after a transient edge
            // timeout. Retry once (on the same warm handle) before
            // permanently skipping it and introducing an avoidable media
            // discontinuity.
            bytes.clear();
            result = client.get(url, bytes);
        }
        if (!result) return result.error();
        return Body(std::move(bytes));
    }

    std::vector<std::unique_ptr<HttpClient>> clients_;
    std::vector<std::thread> threads_;

    std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable done_cv_;
    bool stopping_ = false;
    const std::vector<std::string>* urls_ = nullptr;
    const std::function<bool()>* should_continue_ = nullptr;
    std::vector<std::optional<Body>> results_;
    std::size_t next_ = 0;
    std::size_t remaining_ = 0;
};

// Expands RTMP's wrapping 32-bit millisecond clock into a monotonic 64-bit
// timeline. A small backwards jump is treated as broken/discontinuous input,
// while the characteristic large jump across UINT32_MAX is a real wrap.
class RtmpTimestampUnwrapper {
public:
    std::uint64_t unwrap(std::uint32_t value) {
        if (last_ && value < *last_ && static_cast<std::uint32_t>(*last_ - value) > 0x80000000u) {
            epoch_ += (std::uint64_t{1} << 32);
        }
        last_ = value;
        return epoch_ + value;
    }

private:
    std::optional<std::uint32_t> last_;
    std::uint64_t epoch_ = 0;
};
} // namespace

HlsSourcePuller::HlsSourcePuller(std::string source_url, std::vector<PullerRendition> renditions,
                                 std::uint32_t fps, std::uint32_t cpu_budget,
                                 std::vector<unsigned> pinned_cores)
    : source_url_(std::move(source_url)),
      renditions_(std::move(renditions)),
      fps_(std::max<std::uint32_t>(fps, 1)),
      cpu_budget_(cpu_budget),
      pinned_cores_(std::move(pinned_cores)) {}

HlsSourcePuller::~HlsSourcePuller() { stop(); }

void HlsSourcePuller::start() {
    if (running_.exchange(true)) return;
    status_.store(PullerStatus::Starting);
    thread_ = std::thread([this] {
        // Pinned before run() touches the transcoder: a single-rendition job
        // has no internal render pool and opens its encoder directly on this
        // thread, so this is the only pinning point that reaches it. A
        // multi-rendition job's own ThreadPool is pinned separately (see
        // SourceTranscoder::start()); both end up confined to the same set.
        core::pin_current_thread_to_cores(pinned_cores_);
        run();
    });
}

void HlsSourcePuller::stop() {
    if (!running_.exchange(false)) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    if (thread_.joinable()) thread_.join();
    status_.store(PullerStatus::Stopped);
}

void HlsSourcePuller::set_detail(std::string detail) {
    std::lock_guard lock(detail_mutex_);
    detail_ = std::move(detail);
}

std::string HlsSourcePuller::detail() const {
    std::lock_guard lock(detail_mutex_);
    return detail_;
}

bool HlsSourcePuller::resolve_source(HttpClient& http, std::string& media_url_out, bool& raw_ts_out,
                                     bool& reresolve_each_poll_out, std::string& detail_out) {
    reresolve_each_poll_out = false;
    // A bounded peek tells us the source's shape without downloading a
    // never-ending raw-TS body in full (that download would simply time out).
    std::vector<std::byte> peeked;
    if (auto r = http.peek(source_url_, 4096, peeked); !r) {
        detail_out = r.error().message();
        return false;
    }
    if (looks_like_raw_ts(peeked)) {
        media_url_out = source_url_;
        raw_ts_out = true;
        return true;
    }

    // Otherwise this is HLS playlist text — small and bounded, safe to fetch whole.
    std::vector<std::byte> body;
    std::string effective_source_url;
    if (auto r = http.get(source_url_, body, &effective_source_url); !r) {
        detail_out = r.error().message();
        return false;
    }
    const std::string_view text(reinterpret_cast<const char*>(body.data()), body.size());
    if (!is_hls_playlist(text)) {
        detail_out = "HTTP source is neither an HLS playlist nor an MPEG-TS stream";
        return false;
    }
    raw_ts_out = false;
    if (!is_master_playlist(text)) {
        // The playlist may have redirected to a different CDN host. Its
        // root-relative and path-relative segment URIs belong to that final
        // URL, so retain it as the parse base for this body.
        media_url_out = effective_source_url;
        // If a redirect was actually followed, the target is very likely a
        // per-request token URL (IPTV panels): reused, it returns 403/429/509
        // within seconds. Re-run source_url_ (and its fresh redirect) on every
        // poll instead. A source that did not redirect keeps polling the same
        // URL as before -- source_url_ == effective_source_url there.
        reresolve_each_poll_out = (effective_source_url != source_url_);
        return true;
    }
    const auto variants = parse_master_playlist(text, effective_source_url);
    const std::string chosen = select_variant(variants, 0);
    if (chosen.empty()) {
        detail_out = "master playlist has no variants";
        return false;
    }
    media_url_out = chosen;
    return true;
}

void HlsSourcePuller::run() {
    HttpClient http;

    std::string detail;
    std::string media_url;
    bool raw_ts = false;
    bool reresolve_each_poll = false;
    const bool rtmp_source = is_rtmp_source(source_url_);
    if (!rtmp_source && !resolve_source(http, media_url, raw_ts, reresolve_each_poll, detail)) {
        set_detail(detail.empty() ? "could not resolve source" : detail);
        status_.store(PullerStatus::Error);
        running_.store(false);
        return;
    }
    // The URL polled each iteration. For a token-redirect source this is the
    // post-redirect (tokenised) URL and it is kept as long as it works: the
    // panel load-balances source_url_ across several backends whose media
    // sequence numbers are independent, so re-resolving on every poll makes
    // the sequence jump backends constantly and the ingest treats every jump
    // as a real EXT-X-DISCONTINUITY (the player then re-inits its decoder
    // every few segments -- visible as a permanent stutter). Re-resolution
    // happens only when the current token URL actually fails (below), which
    // costs at most one genuine discontinuity per token lifetime.
    std::string poll_url = media_url;

    // Build the decode-once / encode-per-rendition core and wire each rendition
    // to its own segmenter + segment store via a RenditionFeed.
    std::vector<RenditionSpec> specs;
    specs.reserve(renditions_.size());
    for (const auto& r : renditions_) specs.push_back(r.spec);

    // Construction is deferred until the source's actual video codec is
    // known (TsDemuxer::video_codec() once the PMT is parsed for TS/HLS
    // sources, or the first RTMP video tag's codec id/FourCC for an RTMP
    // source) -- SourceTranscoder's decoder variant is selected once at
    // construction and never switched mid-stream, so constructing it before
    // that is known would silently default to H.264 for an HEVC source. See
    // ensure_transcoder below, invoked lazily from whichever path first
    // learns the codec.
    std::optional<SourceTranscoder> transcoder;
    std::vector<std::unique_ptr<hls::Segmenter>> segmenters;
    std::vector<std::unique_ptr<hls::RenditionFeed>> feeds;
    // One paced publisher per rendition, sitting between its Segmenter and
    // its SegmentStore (see PacedSegmentPublisher above) so a burst of
    // segments produced from a single source poll drains out to viewers at
    // the steady per-segment rate they expect instead of all at once.
    std::vector<std::shared_ptr<PacedSegmentPublisher>> publishers;
    segmenters.reserve(renditions_.size());
    feeds.reserve(renditions_.size());
    publishers.reserve(renditions_.size());
    // Output segment length for pulled/transcoded sources. This must match
    // the SegmentStore's target_duration_seconds (SourceJobManager::Options,
    // wired in apps/rtmp_server/main.cpp) so #EXT-X-TARGETDURATION and the
    // live-window math agree with the pieces actually produced. 6s is chosen
    // for single-box scale: it cuts each viewer's playlist+segment request
    // rate to a third of what 1-2s segments produce (far fewer packets,
    // connections and conntrack entries per viewer) and keeps every fetch in
    // bulk TCP transfer. The cost is added latency, which a rebroadcast
    // audience does not notice. The segmenter still only cuts on a keyframe,
    // so a source with sparse keyframes rounds up toward max_segment_duration.
    hls::SegmenterConfig base_segmenter_config;
    base_segmenter_config.target_duration = std::chrono::milliseconds(6000);
    base_segmenter_config.max_segment_duration = std::chrono::milliseconds(12000);
    // Segment URLs are immutable at the CDN. A whole server process restart
    // loses the in-memory store, so starting again at segment-0.ts would make
    // active sessions receive stale cached bytes. A wall-clock floor keeps
    // first-start names unique across processes; retained stores continue at
    // whichever value is higher.
    const auto sequence_floor = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    for (auto& rendition : renditions_) {
        auto store = rendition.store;
        const auto retained_next_sequence = store->next_sequence();
        const bool resuming = retained_next_sequence > 0;
        const auto next_sequence = std::max(retained_next_sequence, sequence_floor);
        PacedSegmentPublisherConfig publisher_config;
        // A recovered pipeline already has a complete live window available
        // in its retained store. Re-prime with the shorter recovery runway so
        // playback resumes before that window drains.
        publisher_config.startup_buffer = resuming ? std::chrono::seconds(6)
                                                   : std::chrono::seconds(20);
        publisher_config.fallback_interval = base_segmenter_config.target_duration;
        auto publisher = std::make_shared<PacedSegmentPublisher>(store, publisher_config);
        auto segmenter_config = base_segmenter_config;
        segmenter_config.initial_sequence = next_sequence;
        auto segmenter = std::make_unique<hls::Segmenter>(
            [publisher](hls::SegmentPtr segment) { publisher->push(std::move(segment)); }, segmenter_config);
        // The codec and timestamp timeline may change across a rebuilt
        // puller or whole-process restart. This is harmless for a new viewer
        // and tells an existing HLS player to reset its decoder at the first
        // recovered segment while retaining the same session.
        segmenter->mark_media_discontinuity();
        feeds.push_back(std::make_unique<hls::RenditionFeed>(*segmenter));
        segmenters.push_back(std::move(segmenter));
        publishers.push_back(std::move(publisher));
    }

    std::optional<core::Error> pipeline_error;

    // Constructs the transcoder (once) with the given source codec and wires
    // its outputs to the per-rendition feeds exactly as before -- only the
    // timing of this moved, not the wiring itself. Idempotent: a second call
    // (e.g. audio arriving before video has resolved the codec, then video
    // resolving it moments later) is a no-op.
    auto ensure_transcoder = [&](SourceVideoCodec codec) -> core::Result<void> {
        if (transcoder) return {};
        transcoder.emplace(specs, fps_, codec, cpu_budget_, pinned_cores_);
        transcoder->set_video_output([&](std::size_t i, const EncodedAccessUnit& au) {
            feeds[i]->push_video(au.annexb, au.pts_90k, au.dts_90k, au.keyframe);
        });
        transcoder->set_audio_output(
            [&](std::size_t i, const EncodedAudioFrame& frame, std::int64_t pts_90k) {
                feeds[i]->push_audio(frame.adts, pts_90k);
            });
        return transcoder->start();
    };

    media::ts::TsDemuxer demux;
    demux.set_video_handler([&](std::span<const std::byte> annexb, std::uint64_t pts,
                                std::uint64_t dts, bool keyframe) {
        if (pipeline_error) return;
        // The PMT (which TsDemuxer::video_codec() reflects) is always parsed
        // before any PES payload is reassembled off the video/audio PIDs it
        // names, so by the time this handler fires for the first access
        // unit the real codec is already known.
        const auto codec = demux.video_codec() == media::ts::TsVideoCodec::Hevc ? SourceVideoCodec::Hevc
                                                                                : SourceVideoCodec::H264;
        if (auto r = ensure_transcoder(codec); !r) {
            pipeline_error = r.error();
            return;
        }
        auto result = transcoder->on_video(annexb, static_cast<std::int64_t>(pts),
                                           static_cast<std::int64_t>(dts), keyframe);
        if (!result) pipeline_error = result.error();
    });
    demux.set_audio_handler([&](std::span<const std::byte> adts, std::uint64_t pts) {
        if (pipeline_error) return;
        // Audio can in principle arrive before the first video access unit
        // within the same feed() call; the PMT (and so the real codec) is
        // still already known by then, same reasoning as the video handler.
        const auto codec = demux.video_codec() == media::ts::TsVideoCodec::Hevc ? SourceVideoCodec::Hevc
                                                                                : SourceVideoCodec::H264;
        if (auto r = ensure_transcoder(codec); !r) {
            pipeline_error = r.error();
            return;
        }
        auto result = transcoder->on_audio(adts, static_cast<std::int64_t>(pts));
        if (!result) pipeline_error = result.error();
    });

    if (!rtmp_source) {
        status_.store(PullerStatus::Running);
        set_detail("running");
    } else {
        set_detail("connecting to RTMP source");
    }

    // A successful playlist request only proves that the control plane is
    // alive. The source can keep returning the same window forever, or the
    // encoder/publisher can stop producing while the puller still reports
    // Running. Track actual input and published-output progress so the job
    // manager can replace a wedged pipeline automatically.
    const auto pipeline_started_at = std::chrono::steady_clock::now();
    auto last_input_progress = pipeline_started_at;
    auto last_output_progress = pipeline_started_at;
    bool received_input = false;
    bool published_output = false;
    std::uint64_t published_segment_count = 0;
    for (const auto& rendition : renditions_) {
        published_segment_count += rendition.store->stats().real_segments_added;
    }
    published_output = published_segment_count > 0;

    auto note_input_progress = [&] {
        received_input = true;
        last_input_progress = std::chrono::steady_clock::now();
    };
    auto detect_stall = [&]() -> bool {
        const auto now = std::chrono::steady_clock::now();
        std::uint64_t current_output_count = 0;
        for (const auto& rendition : renditions_) {
            current_output_count += rendition.store->stats().real_segments_added;
        }
        if (current_output_count != published_segment_count) {
            published_segment_count = current_output_count;
            published_output = true;
            last_output_progress = now;
        }

        if (now - last_input_progress >= kInputProgressTimeout) {
            set_detail("source stalled: no media segment progress for 30 seconds");
            status_.store(PullerStatus::Error);
            return true;
        }
        if (received_input) {
            const auto output_timeout =
                published_output ? kOutputProgressTimeout : kOutputStartupTimeout;
            if (now - last_output_progress >= output_timeout) {
                set_detail(published_output
                               ? "transcoder stalled: no output segment for 30 seconds"
                               : "transcoder startup stalled: no output segment for 60 seconds");
                status_.store(PullerStatus::Error);
                return true;
            }
        }
        return false;
    };

    auto finish_pipeline = [&] {
        const bool failed = status_.load() == PullerStatus::Error;
        if (failed) {
            // Never publish an accumulated burst from a failed pipeline and
            // never advertise ENDLIST for a live stream. The manager swaps
            // this store for a fresh pipeline after its restart delay.
            for (auto& publisher : publishers) publisher->stop();
            for (auto& segmenter : segmenters) segmenter->finalize();
        } else {
            for (auto& segmenter : segmenters) segmenter->finalize();
            for (auto& publisher : publishers) publisher->flush();
            for (auto& rendition : renditions_) rendition.store->mark_ended();
        }
    };

    std::mutex sleep_mutex;
    std::condition_variable sleep_cv;
    int consecutive_errors = 0;

    if (rtmp_source) {
        media::h264::AvcDecoderConfig video_config;
        std::optional<media::hevc::HevcDecoderConfig> hevc_video_config;
        std::optional<media::aac::AudioSpecificConfig> audio_config;
        RtmpTimestampUnwrapper video_clock;
        RtmpTimestampUnwrapper audio_clock;
        RtmpSourceClient client(source_url_);

        // Handles one RTMP video message once the source's codec is known
        // (classic AVC, legacy CodecID-12 HEVC, or Enhanced RTMP hvc1),
        // covering both the plain-AVC path (unchanged from before this
        // function existed) and the two HEVC forms. Factored into its own
        // lambda so the outer message handler can still reach the shared
        // detect_stall() call below regardless of which branch runs.
        auto handle_video_message = [&](const protocol::chunk::RtmpMessage& message) -> core::Result<void> {
            auto info = protocol_media::classify_video_tag(message.payload);
            if (!info) {
                return core::Error(core::ErrorCode::MalformedChunk, core::ErrorCategory::Protocol,
                                   "empty RTMP video payload");
            }
            const auto source_codec = info->codec == protocol_media::VideoCodec::Hevc
                                          ? SourceVideoCodec::Hevc
                                          : SourceVideoCodec::H264;
            if (auto r = ensure_transcoder(source_codec); !r) return r.error();

            if (info->codec != protocol_media::VideoCodec::Hevc) {
                // Plain classic-AVC tag -- byte-for-byte the same parse/convert
                // calls as before this function gained HEVC support.
                auto tag = media::h264::parse_video_tag(message.payload);
                if (!tag) return tag.error();
                if (tag.value().avc_packet_type == media::h264::kAvcPacketTypeSequenceHeader) {
                    auto config = media::h264::parse_decoder_config(tag.value().body);
                    if (!config) return config.error();
                    video_config = std::move(config).value();
                    return {};
                }
                if (tag.value().avc_packet_type == media::h264::kAvcPacketTypeEndOfSequence) {
                    return {};
                }
                if (tag.value().avc_packet_type != media::h264::kAvcPacketTypeNalu) {
                    return core::Error(core::ErrorCode::MalformedChunk, core::ErrorCategory::Protocol,
                                       "unsupported RTMP AVC packet type");
                }
                if (!video_config.valid()) {
                    return core::Error(core::ErrorCode::InvalidStateTransition,
                                       core::ErrorCategory::Protocol,
                                       "RTMP video frame arrived before its AVC sequence header");
                }
                std::vector<std::byte> annexb;
                auto converted = media::h264::avcc_to_annexb(tag.value().body, video_config,
                                                             tag.value().is_keyframe, annexb);
                if (!converted) return converted.error();
                const std::uint64_t dts_ms = video_clock.unwrap(message.timestamp);
                const std::int64_t dts_90k = static_cast<std::int64_t>(dts_ms * 90);
                const std::int64_t pts_90k =
                    dts_90k + static_cast<std::int64_t>(tag.value().composition_time_ms) * 90;
                auto transcoded =
                    transcoder->on_video(annexb, pts_90k, dts_90k, tag.value().is_keyframe);
                if (!transcoded) return transcoded.error();
                note_input_progress();
                return {};
            }

            // HEVC, either legacy CodecID-12 (info->enhanced == false, same
            // tag layout as AVC's: packet-type byte then 3-byte composition
            // time) or Enhanced RTMP hvc1 (info->enhanced == true, whose
            // ExVideoTagHeader packet types this branch decodes directly --
            // see ExVideoPacketType's comment on confidence for this layout).
            std::span<const std::byte> body;
            std::int32_t composition_time_ms = 0;
            bool is_sequence_start = false;
            bool is_coded_frame = false;

            if (info->enhanced) {
                if (!info->ex_packet_type) {
                    return core::Error(core::ErrorCode::MalformedChunk, core::ErrorCategory::Protocol,
                                       "Enhanced RTMP video tag missing its PacketType");
                }
                switch (*info->ex_packet_type) {
                    case protocol_media::ExVideoPacketType::SequenceStart:
                        if (message.payload.size() < 5) {
                            return core::Error(core::ErrorCode::MalformedChunk,
                                               core::ErrorCategory::Protocol,
                                               "Enhanced RTMP HEVC sequence-start tag too short");
                        }
                        is_sequence_start = true;
                        body = std::span<const std::byte>(message.payload.data() + 5,
                                                          message.payload.size() - 5);
                        break;
                    case protocol_media::ExVideoPacketType::CodedFrames: {
                        if (message.payload.size() < 8) {
                            return core::Error(core::ErrorCode::MalformedChunk,
                                               core::ErrorCategory::Protocol,
                                               "Enhanced RTMP HEVC coded-frame tag too short");
                        }
                        std::int32_t cts = (static_cast<std::int32_t>(message.payload[5]) << 16) |
                                           (static_cast<std::int32_t>(message.payload[6]) << 8) |
                                           static_cast<std::int32_t>(message.payload[7]);
                        if (cts & 0x00800000) cts |= static_cast<std::int32_t>(0xFF000000u);
                        composition_time_ms = cts;
                        is_coded_frame = true;
                        body = std::span<const std::byte>(message.payload.data() + 8,
                                                          message.payload.size() - 8);
                        break;
                    }
                    case protocol_media::ExVideoPacketType::CodedFramesX:
                        if (message.payload.size() < 5) {
                            return core::Error(core::ErrorCode::MalformedChunk,
                                               core::ErrorCategory::Protocol,
                                               "Enhanced RTMP HEVC coded-frame-x tag too short");
                        }
                        is_coded_frame = true;
                        body = std::span<const std::byte>(message.payload.data() + 5,
                                                          message.payload.size() - 5);
                        break;
                    case protocol_media::ExVideoPacketType::SequenceEnd:
                        return {};
                    default:
                        return {}; // Metadata / MPEG2TSSequenceStart: nothing this pipeline needs
                }
            } else {
                // Legacy CodecID-12: identical layout to a classic AVC tag.
                if (message.payload.size() < 5) {
                    return core::Error(core::ErrorCode::MalformedChunk, core::ErrorCategory::Protocol,
                                       "legacy HEVC video tag too short");
                }
                std::int32_t cts = (static_cast<std::int32_t>(message.payload[2]) << 16) |
                                   (static_cast<std::int32_t>(message.payload[3]) << 8) |
                                   static_cast<std::int32_t>(message.payload[4]);
                if (cts & 0x00800000) cts |= static_cast<std::int32_t>(0xFF000000u);
                composition_time_ms = cts;
                body = std::span<const std::byte>(message.payload.data() + 5, message.payload.size() - 5);
                if (!info->avc_packet_type) {
                    return core::Error(core::ErrorCode::MalformedChunk, core::ErrorCategory::Protocol,
                                       "legacy HEVC video tag missing its packet type");
                }
                // classify_video_tag reports the packet type as the scoped
                // protocol_media::AvcPacketType enum, not as the loose
                // media::h264::kAvcPacketType* byte constants the classic-AVC
                // branch above compares against -- a scoped enum has no
                // implicit conversion to std::uint8_t, so comparing the two
                // does not compile.
                if (*info->avc_packet_type == protocol_media::AvcPacketType::SequenceHeader) {
                    is_sequence_start = true;
                } else if (*info->avc_packet_type == protocol_media::AvcPacketType::EndOfSequence) {
                    return {};
                } else if (*info->avc_packet_type == protocol_media::AvcPacketType::Nalu) {
                    is_coded_frame = true;
                } else {
                    return core::Error(core::ErrorCode::MalformedChunk, core::ErrorCategory::Protocol,
                                       "unsupported legacy HEVC packet type");
                }
            }

            if (is_sequence_start) {
                auto config = media::hevc::parse_decoder_config(body);
                if (!config) return config.error();
                hevc_video_config = std::move(config).value();
                return {};
            }
            if (!is_coded_frame) return {};
            if (!hevc_video_config || !hevc_video_config->valid()) {
                return core::Error(core::ErrorCode::InvalidStateTransition, core::ErrorCategory::Protocol,
                                   "RTMP HEVC video frame arrived before its sequence header");
            }
            const bool is_keyframe = info->frame_type == protocol_media::VideoFrameType::KeyFrame ||
                                     info->frame_type == protocol_media::VideoFrameType::GeneratedKeyFrame;
            std::vector<std::byte> annexb;
            auto converted = media::hevc::hvcc_to_annexb(body, *hevc_video_config, is_keyframe, annexb);
            if (!converted) return converted.error();
            const std::uint64_t dts_ms = video_clock.unwrap(message.timestamp);
            const std::int64_t dts_90k = static_cast<std::int64_t>(dts_ms * 90);
            const std::int64_t pts_90k = dts_90k + static_cast<std::int64_t>(composition_time_ms) * 90;
            auto transcoded = transcoder->on_video(annexb, pts_90k, dts_90k, is_keyframe);
            if (!transcoded) return transcoded.error();
            note_input_progress();
            return {};
        };

        auto result = client.run(
            [this] { return running_.load(); },
            [&](const protocol::chunk::RtmpMessage& message) -> core::Result<void> {
                using protocol::chunk::MessageTypeId;
                const auto type = static_cast<MessageTypeId>(message.message_type_id);
                if (type == MessageTypeId::Video) {
                    if (auto r = handle_video_message(message); !r) return r.error();
                } else if (type == MessageTypeId::Audio) {
                    // Codec is unknown from audio alone; H.264 is the
                    // pre-existing default for this path, so falling back to
                    // it here preserves prior behavior when audio precedes
                    // video far enough that no video codec is known yet.
                    if (auto r = ensure_transcoder(SourceVideoCodec::H264); !r) return r.error();
                    auto tag = media::aac::parse_audio_tag(message.payload);
                    if (!tag) return tag.error();
                    if (tag.value().aac_packet_type == media::aac::kAacPacketTypeSequenceHeader) {
                        auto config = media::aac::parse_audio_specific_config(tag.value().body);
                        if (!config) return config.error();
                        audio_config = std::move(config).value();
                        return {};
                    }
                    if (tag.value().aac_packet_type != media::aac::kAacPacketTypeRaw) {
                        return core::Error(core::ErrorCode::MalformedChunk,
                                           core::ErrorCategory::Protocol,
                                           "unsupported RTMP AAC packet type");
                    }
                    if (!audio_config) {
                        return core::Error(core::ErrorCode::InvalidStateTransition,
                                           core::ErrorCategory::Protocol,
                                           "RTMP audio frame arrived before its AAC sequence header");
                    }
                    std::vector<std::byte> adts;
                    adts.reserve(media::aac::kAdtsHeaderSize + tag.value().body.size());
                    media::aac::append_adts_header(adts, *audio_config, tag.value().body.size());
                    adts.insert(adts.end(), tag.value().body.begin(), tag.value().body.end());
                    const auto pts_90k = static_cast<std::int64_t>(audio_clock.unwrap(message.timestamp) * 90);
                    auto transcoded = transcoder->on_audio(adts, pts_90k);
                    if (!transcoded) return transcoded.error();
                    note_input_progress();
                }
                if (detect_stall()) {
                    return core::Error(core::ErrorCode::ConnectionTimedOut,
                                       core::ErrorCategory::Network, this->detail());
                }
                return {};
            },
            [this] {
                status_.store(PullerStatus::Running);
                set_detail("running (native RTMP pull)");
            });

        if (!running_.load()) {
            // Operator-requested stop is clean; never turn it into an
            // auto-restartable source error.
            status_.store(PullerStatus::Stopped);
        } else if (!result) {
            set_detail(result.error().message());
            status_.store(PullerStatus::Error);
        }
        finish_pipeline();
        if (status_.load() == PullerStatus::Running) status_.store(PullerStatus::Stopped);
        running_.store(false);
        return;
    }

    if (raw_ts) {
        // No playlist, no discrete segments: one open GET streams TS packets
        // indefinitely. Feed the demuxer as chunks arrive and reconnect (with
        // backoff) if the connection stalls or drops.
        while (running_.load()) {
            bool received_any = false;
            auto result = http.stream(
                media_url, [this] { return running_.load(); },
                [&](std::span<const std::byte> chunk) -> bool {
                    if (!running_.load()) return false;
                    received_any = true;
                    note_input_progress();
                    set_detail("running");
                    auto demuxed = demux.feed(chunk);
                    if (!demuxed) pipeline_error = demuxed.error();
                    if (pipeline_error) {
                        set_detail(pipeline_error->message());
                        status_.store(PullerStatus::Error);
                        return false;
                    }
                    return !detect_stall();
                });
            if (!running_.load()) break;
            if (status_.load() == PullerStatus::Error) break;

            if (received_any) {
                // Some IPTV/CDN panels close otherwise-healthy raw TS responses
                // every few seconds. Treat that as a normal chunk boundary and
                // reconnect immediately; a fixed sleep here becomes visible
                // playback lag on every reconnect.
                consecutive_errors = 0;
                set_detail("running");
                continue;
            }

            set_detail(result ? "source connection closed without media; reconnecting"
                              : result.error().message());
            consecutive_errors += 1;
            if (consecutive_errors >= 5) {
                status_.store(PullerStatus::Error);
                break;
            }
            std::unique_lock lock(sleep_mutex);
            sleep_cv.wait_for(lock, std::chrono::seconds(2), [&] { return !running_.load(); });
        }

        finish_pipeline();
        if (status_.load() == PullerStatus::Running) status_.store(PullerStatus::Stopped);
        running_.store(false);
        return;
    }

    // A sequence is "seen" only after its bytes have been fetched and fed to
    // the demuxer. Some IPTV origins advertise the live edge several seconds
    // before the corresponding object becomes readable and return a transient
    // 404 in that window. Marking the sequence before the fetch permanently
    // skipped it on every later poll, starving the output whenever this
    // happened repeatedly.
    std::unordered_set<std::uint64_t> seen;
    std::optional<std::uint64_t> last_ingested_sequence;
    bool first_playlist_poll = true;

    // One pool for the whole job: its workers keep their libcurl handles (and
    // therefore their connections to the source's CDN) alive across polls.
    // Four concurrent fetches cover the 2-3 unseen segments a poll typically
    // turns up without turning a many-job box into a download storm. A
    // token-redirect IPTV panel usually enforces a tiny per-account
    // connection cap, so hold it to two there (playlist poll + one segment)
    // to avoid tripping its 403/429/509 limiter.
    SegmentFetchPool segment_fetcher(reresolve_each_poll ? 2 : 4);
    const std::function<bool()> fetch_should_continue = [this] { return running_.load(); };

    while (running_.load()) {
        std::vector<std::byte> playlist_bytes;
        std::string effective_media_url;
        if (auto r = http.get(poll_url, playlist_bytes, &effective_media_url); !r) {
            set_detail(r.error().message());
            // The tokenised URL has most likely expired / hit the panel's
            // per-token connection cap (403/429/509). Re-resolve once from
            // source_url_ to mint a fresh token URL and keep polling that;
            // the sequence discontinuity this may introduce is a real one
            // and rare (once per token lifetime), not once per poll.
            if (reresolve_each_poll) {
                std::string refreshed;
                bool refreshed_raw = false;
                bool refreshed_flag = false;
                std::string ignore_detail;
                if (resolve_source(http, refreshed, refreshed_raw, refreshed_flag, ignore_detail) &&
                    !refreshed_raw && !refreshed.empty()) {
                    poll_url = refreshed;
                }
            }
            // Give a token-redirect source more attempts before declaring the
            // job failed so a transient panel throttle is not fatal.
            const int error_ceiling = reresolve_each_poll ? 20 : 5;
            if (++consecutive_errors >= error_ceiling) {
                status_.store(PullerStatus::Error);
                break;
            }
            if (detect_stall()) break;
            std::unique_lock lock(sleep_mutex);
            sleep_cv.wait_for(lock, std::chrono::seconds(2), [&] { return !running_.load(); });
            continue;
        }
        consecutive_errors = 0;
        const std::string_view text(reinterpret_cast<const char*>(playlist_bytes.data()),
                                    playlist_bytes.size());
        if (!is_hls_playlist(text)) {
            set_detail("HLS endpoint stopped returning a valid #EXTM3U playlist");
            status_.store(PullerStatus::Error);
            break;
        }
        // A media-playlist endpoint can itself redirect on every refresh and
        // change hosts/tokens. Resolve each advertised segment against the
        // URL that produced this exact playlist body.
        const auto playlist = parse_media_playlist(text, effective_media_url);
        if (!playlist.unsupported_feature.empty()) {
            set_detail(playlist.unsupported_feature);
            status_.store(PullerStatus::Error);
            break;
        }

        // Join a live source at its live edge, not at the oldest segment it
        // still advertises. IPTV panels commonly publish a window several
        // minutes deep; ingesting all of it puts that entire backlog through
        // the transcoder and leaves every viewer permanently that far behind
        // the broadcast, for as long as the job runs. RFC 8216 6.3.3 says a
        // live client should start about three target durations from the
        // end, which is also the runway PacedSegmentPublisher primes. A VOD
        // source (EXT-X-ENDLIST) is played from its true beginning, unchanged.
        const bool initial_poll = first_playlist_poll;
        if (first_playlist_poll) {
            first_playlist_poll = false;
            if (!playlist.endlist && playlist.segments.size() > kMinInitialLiveEdgeSegments) {
                // Walk back from the live edge until the runway is covered.
                std::size_t keep = 0;
                double media_seconds = 0;
                for (auto it = playlist.segments.rbegin(); it != playlist.segments.rend(); ++it) {
                    ++keep;
                    media_seconds += it->duration > 0 ? it->duration
                                                      : std::max(playlist.target_duration, 1.0);
                    if (keep >= kMinInitialLiveEdgeSegments && media_seconds >= kInitialLiveEdgeSeconds)
                        break;
                }
                if (keep < playlist.segments.size()) {
                    const std::size_t skip = playlist.segments.size() - keep;
                    for (std::size_t i = 0; i < skip; ++i) seen.insert(playlist.segments[i].sequence);
                    // Nothing has been fed to the demuxer yet, so the first
                    // segment actually ingested must not be reported as a
                    // gap: anchor the continuity check to the last skipped
                    // sequence, which it follows directly.
                    last_ingested_sequence = playlist.segments[skip - 1].sequence;
                }
            }
        }

        // Collect every not-yet-pulled segment from this playlist window
        // first, then fetch them all through the pool (the member `http`
        // above stays reserved for the playlist itself). A playlist poll
        // regularly turns up 2-3 unseen segments at once (the window is
        // wider than the source's own publish interval), and fetching them
        // one at a time serially adds each segment's network round-trip to
        // the next one's. Demuxing must still happen strictly in playlist
        // order (PTS/DTS continuity depends on it), so only the network
        // fetch is parallel; feeding to the demuxer below is unchanged
        // serial code.
        std::vector<const decltype(playlist.segments)::value_type*> pending;
        for (const auto& segment : playlist.segments) {
            if (seen.contains(segment.sequence)) continue;
            pending.push_back(&segment);
        }

        // Bounded catch-up: drop the oldest of a backlog that says we are
        // behind, keeping the newest kMaxCatchUpSeconds. Skipped here (not
        // on the initial poll) because that one deliberately ingests a
        // runway's worth in one go. last_ingested_sequence is deliberately
        // left pointing at the last segment actually fed to the demuxer, so
        // the gap check below sees the jump and emits EXT-X-DISCONTINUITY
        // for it -- the media really is discontinuous at that point.
        if (!initial_poll && !playlist.endlist && !pending.empty()) {
            double backlog_seconds = 0;
            for (const auto* segment : pending) {
                backlog_seconds += segment->duration > 0 ? segment->duration
                                                         : std::max(playlist.target_duration, 1.0);
            }
            std::size_t skip = 0;
            while (skip < pending.size() && backlog_seconds > kMaxCatchUpSeconds) {
                backlog_seconds -= pending[skip]->duration > 0
                                       ? pending[skip]->duration
                                       : std::max(playlist.target_duration, 1.0);
                ++skip;
            }
            if (skip > 0) {
                for (std::size_t i = 0; i < skip; ++i) seen.insert(pending[i]->sequence);
                RTMP_LOG(LogLevel::Warn, "source-transcoder", "skipping ahead to the live edge",
                         {{"url", source_url_}, {"skipped", std::to_string(skip)}});
                pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(skip));
            }
        }

        std::vector<std::string> pending_uris;
        pending_uris.reserve(pending.size());
        for (const auto* segment : pending) pending_uris.push_back(segment->uri);
        auto fetches = segment_fetcher.fetch_all(pending_uris, fetch_should_continue);

        // Preserve playlist order even though downloads run concurrently. If
        // one segment is not available yet, leave it and every later segment
        // unseen so the next playlist poll retries a contiguous suffix. Feeding
        // a later successful download first and then retrying an older one
        // would move the demuxer timeline backwards.
        bool retry_contiguous_suffix = false;
        for (std::size_t i = 0; i < pending.size(); ++i) {
            if (!running_.load()) break;
            const auto* segment = pending[i];
            auto& result = fetches[i];
            if (!result) {
                RTMP_LOG(LogLevel::Warn, "source-transcoder", "segment fetch failed",
                         {{"url", segment->uri}, {"error", result.error().message()}});
                retry_contiguous_suffix = true;
                continue;
            }
            if (retry_contiguous_suffix) continue;

            const bool sequence_gap =
                last_ingested_sequence &&
                segment->sequence != *last_ingested_sequence + 1;
            if (segment->discontinuity) {
                // The source itself advertised EXT-X-DISCONTINUITY: a real
                // content/parameter break the player must reset across.
                // Propagate it fully -- mark the output segment AND
                // re-anchor the transcoder clock (the latter may not exist
                // yet if this precedes the PMT ever being parsed).
                for (auto& feed : feeds) feed->mark_discontinuity();
                if (transcoder) transcoder->mark_discontinuity();
            } else if (sequence_gap) {
                if (transcoder) {
                    // Only the sequence number jumped, with no discontinuity
                    // tag from the source -- almost always a token-redirect
                    // panel load-balancing us onto another backend whose
                    // counter is independent. The broadcast is continuous and
                    // the transcoder re-encodes it onto one timeline with
                    // unchanged SPS/PPS, so re-anchor its clock but do NOT
                    // emit EXT-X-DISCONTINUITY: the tag would force every
                    // player to rebuild its decoder on each backend hop,
                    // which shows up as a constant stutter.
                    transcoder->mark_discontinuity();
                } else {
                    // Passthrough (no re-encode): the gap cannot be smoothed,
                    // so the output segment must carry the marker.
                    for (auto& feed : feeds) feed->mark_discontinuity();
                }
            }
            auto demuxed = demux.feed(result.value());
            if (!demuxed) pipeline_error = demuxed.error();
            demux.flush(); // each TS segment is a self-contained unit
            if (pipeline_error) {
                set_detail(pipeline_error->message());
                status_.store(PullerStatus::Error);
                break;
            }
            seen.insert(segment->sequence);
            last_ingested_sequence = segment->sequence;
            note_input_progress();
        }

        if (status_.load() == PullerStatus::Error) break;

        // Keep the ingested-sequence set bounded. A 24/7 job pulling a
        // source that publishes a segment every few seconds otherwise adds
        // an entry per segment for the life of the process; only sequences
        // at or near the current window can ever be re-offered.
        if (!playlist.segments.empty()) {
            const auto oldest_advertised = playlist.segments.front().sequence;
            const auto floor = oldest_advertised > kSeenSequenceHistory
                                   ? oldest_advertised - kSeenSequenceHistory
                                   : 0;
            if (floor > 0) std::erase_if(seen, [floor](std::uint64_t s) { return s < floor; });
        }

        if (detect_stall()) break;

        if (playlist.endlist) {
            set_detail("source ended");
            break; // VOD: nothing more to pull
        }

        // Live: poll at a fixed short interval rather than a fraction of the
        // source's own EXT-X-TARGETDURATION. Some IPTV/CDN panels advertise a
        // large target duration (10-15s) that doesn't reflect how often a
        // segment actually lands -- even at half that (5-7.5s) a segment
        // published just after we slept could sit unpicked long enough to
        // read as a stall to a viewer. A 2s floor bounds worst-case detection
        // lag to ~2s regardless of what the source claims, at negligible
        // extra cost (the playlist itself is a few hundred bytes). Never
        // waits longer than the old half-target-duration figure either, so a
        // source with a genuinely short target duration doesn't get polled
        // more slowly than before.
        const auto wait = std::chrono::milliseconds(std::min<long long>(
            2000, static_cast<long long>(std::max(1.0, playlist.target_duration / 2.0) * 1000)));
        std::unique_lock lock(sleep_mutex);
        sleep_cv.wait_for(lock, wait, [&] { return !running_.load(); });
    }

    finish_pipeline();
    if (status_.load() == PullerStatus::Running) status_.store(PullerStatus::Stopped);
    running_.store(false);
}

} // namespace rtmp_server::transcoding::native
