#include "rtmp_server/transcoding/native/hls_source_puller.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

#include "rtmp_server/observability/logger.hpp"
#include "rtmp_server/transcoding/native/hls_playlist.hpp"
#include "rtmp_server/transcoding/native/paced_segment_publisher.hpp"

namespace rtmp_server::transcoding::native {

namespace {
using observability::LogLevel;

// Many IPTV/CDN panels answer an .m3u8-shaped URL with a redirect straight to
// a continuously-flowing raw MPEG-TS body (no playlist, connection never
// closes on its own) rather than an HLS playlist. Detect that from a small
// peek by checking for the TS sync byte at packet boundaries.
bool looks_like_raw_ts(std::span<const std::byte> data) {
    if (data.empty() || static_cast<unsigned char>(data[0]) != 0x47) return false;
    if (data.size() > 188 && static_cast<unsigned char>(data[188]) != 0x47) return false;
    return true;
}

constexpr auto kInputProgressTimeout = std::chrono::seconds(45);
constexpr auto kOutputStartupTimeout = std::chrono::seconds(90);
constexpr auto kOutputProgressTimeout = std::chrono::seconds(45);
} // namespace

HlsSourcePuller::HlsSourcePuller(std::string source_url, std::vector<PullerRendition> renditions,
                                 std::uint32_t fps)
    : source_url_(std::move(source_url)),
      renditions_(std::move(renditions)),
      fps_(std::max<std::uint32_t>(fps, 1)) {}

HlsSourcePuller::~HlsSourcePuller() { stop(); }

void HlsSourcePuller::start() {
    if (running_.exchange(true)) return;
    status_.store(PullerStatus::Starting);
    thread_ = std::thread([this] { run(); });
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
                                     std::string& detail_out) {
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
    if (auto r = http.get(source_url_, body); !r) {
        detail_out = r.error().message();
        return false;
    }
    const std::string_view text(reinterpret_cast<const char*>(body.data()), body.size());
    raw_ts_out = false;
    if (!is_master_playlist(text)) {
        media_url_out = source_url_; // already a media playlist
        return true;
    }
    const auto variants = parse_master_playlist(text, source_url_);
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
    if (!resolve_source(http, media_url, raw_ts, detail)) {
        set_detail(detail.empty() ? "could not resolve source" : detail);
        status_.store(PullerStatus::Error);
        running_.store(false);
        return;
    }

    // Build the decode-once / encode-per-rendition core and wire each rendition
    // to its own segmenter + segment store via a RenditionFeed.
    std::vector<RenditionSpec> specs;
    specs.reserve(renditions_.size());
    for (const auto& r : renditions_) specs.push_back(r.spec);

    SourceTranscoder transcoder(specs, fps_);
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
    // Source-transcode jobs cut much shorter segments than the 4s default:
    // pulling from an upstream that publishes in irregular 9-12s chunks
    // means every second waiting for a *segment* (not just for the source)
    // adds latency and makes a stall/catch-up more visible when it happens.
    // 1s segments (the segmenter can only actually cut on a keyframe, so
    // real durations round up to the source's own keyframe spacing) get
    // smaller pieces of already-decoded video in front of viewers sooner,
    // and give PacedSegmentPublisher finer-grained steps to release instead
    // of a few large ones.
    hls::SegmenterConfig segmenter_config;
    segmenter_config.target_duration = std::chrono::milliseconds(1000);
    for (auto& rendition : renditions_) {
        auto store = rendition.store;
        PacedSegmentPublisherConfig publisher_config;
        publisher_config.startup_buffer = std::chrono::seconds(30);
        publisher_config.fallback_interval = segmenter_config.target_duration;
        auto publisher = std::make_shared<PacedSegmentPublisher>(store, publisher_config);
        auto segmenter = std::make_unique<hls::Segmenter>(
            [publisher](hls::SegmentPtr segment) { publisher->push(std::move(segment)); }, segmenter_config);
        feeds.push_back(std::make_unique<hls::RenditionFeed>(*segmenter));
        segmenters.push_back(std::move(segmenter));
        publishers.push_back(std::move(publisher));
    }

    transcoder.set_video_output([&](std::size_t i, const EncodedAccessUnit& au) {
        feeds[i]->push_video(au.annexb, au.pts_90k, au.dts_90k, au.keyframe);
    });
    transcoder.set_audio_output(
        [&](std::size_t i, const EncodedAudioFrame& frame, std::int64_t pts_90k) {
            feeds[i]->push_audio(frame.adts, pts_90k);
        });
    if (auto r = transcoder.start(); !r) {
        set_detail(r.error().message());
        status_.store(PullerStatus::Error);
        running_.store(false);
        return;
    }

    media::ts::TsDemuxer demux;
    demux.set_video_handler([&](std::span<const std::byte> annexb, std::uint64_t pts,
                                std::uint64_t dts, bool keyframe) {
        static_cast<void>(transcoder.on_video(annexb, static_cast<std::int64_t>(pts),
                                              static_cast<std::int64_t>(dts), keyframe));
    });
    demux.set_audio_handler([&](std::span<const std::byte> adts, std::uint64_t pts) {
        static_cast<void>(transcoder.on_audio(adts, static_cast<std::int64_t>(pts)));
    });

    status_.store(PullerStatus::Running);
    set_detail("running");

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
        published_segment_count += rendition.store->stats().segments_added;
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
            current_output_count += rendition.store->stats().segments_added;
        }
        if (current_output_count != published_segment_count) {
            published_segment_count = current_output_count;
            published_output = true;
            last_output_progress = now;
        }

        if (now - last_input_progress >= kInputProgressTimeout) {
            set_detail("source stalled: no media segment progress for 45 seconds");
            status_.store(PullerStatus::Error);
            return true;
        }
        if (received_input) {
            const auto output_timeout =
                published_output ? kOutputProgressTimeout : kOutputStartupTimeout;
            if (now - last_output_progress >= output_timeout) {
                set_detail(published_output
                               ? "transcoder stalled: no output segment for 45 seconds"
                               : "transcoder startup stalled: no output segment for 90 seconds");
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
                    static_cast<void>(demux.feed(chunk));
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

    std::unordered_set<std::uint64_t> seen; // segment sequence numbers already pulled
    // Some upstream IPTV/CDN panels list a segment in the playlist slightly
    // before (or after) it's actually available, so a fetch can 404 even
    // though we just saw it advertised. That segment's media is simply gone
    // -- skipping it is correct -- but without flagging it, the next segment
    // we do land splices onto the previous one with a PTS gap the player was
    // never told about, which reads as a much worse stall/seek than a
    // properly marked discontinuity would.
    bool pending_fetch_gap = false;

    while (running_.load()) {
        std::vector<std::byte> playlist_bytes;
        if (auto r = http.get(media_url, playlist_bytes); !r) {
            set_detail(r.error().message());
            if (++consecutive_errors >= 5) {
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
        const auto playlist = parse_media_playlist(text, media_url);

        // Collect every not-yet-pulled segment from this playlist window
        // first, then fetch them all concurrently (one HttpClient per fetch —
        // the member `http` above is single-use, reserved for the playlist
        // itself). A playlist poll regularly turns up 2-3 unseen segments at
        // once (the window is wider than the source's own publish interval),
        // and fetching them one at a time serially adds each segment's
        // network round-trip to the next one's, on a 24-core box that spends
        // the rest of its time idle. Demuxing must still happen strictly in
        // playlist order (PTS/DTS continuity depends on it), so only the
        // network fetch is parallel; feeding to the demuxer below is
        // unchanged serial code.
        std::vector<const decltype(playlist.segments)::value_type*> pending;
        for (const auto& segment : playlist.segments) {
            if (seen.contains(segment.sequence)) continue;
            seen.insert(segment.sequence);
            pending.push_back(&segment);
        }

        std::vector<std::future<core::Result<std::vector<std::byte>>>> fetches;
        fetches.reserve(pending.size());
        for (const auto* segment : pending) {
            fetches.push_back(std::async(std::launch::async, [uri = segment->uri]() {
                HttpClient segment_http;
                std::vector<std::byte> bytes;
                auto result = segment_http.get(uri, bytes);
                if (!result) {
                    // A segment is often still available after a transient
                    // edge timeout. Retry once before permanently skipping
                    // it and introducing an avoidable media discontinuity.
                    HttpClient retry_http;
                    bytes.clear();
                    result = retry_http.get(uri, bytes);
                }
                if (!result) return core::Result<std::vector<std::byte>>(result.error());
                return core::Result<std::vector<std::byte>>(std::move(bytes));
            }));
        }

        for (std::size_t i = 0; i < pending.size(); ++i) {
            if (!running_.load()) break;
            const auto* segment = pending[i];
            auto result = fetches[i].get();
            if (!result) {
                RTMP_LOG(LogLevel::Warn, "source-transcoder", "segment fetch failed",
                         {{"url", segment->uri}, {"error", result.error().message()}});
                pending_fetch_gap = true;
                continue;
            }
            if (segment->discontinuity || pending_fetch_gap) {
                for (auto& feed : feeds) feed->mark_discontinuity();
                // Also reset the transcoder's own video clock gate -- see
                // SourceTranscoder::mark_discontinuity()'s comment for why
                // skipping this left video frozen (while audio kept
                // playing) after exactly this kind of gap.
                transcoder.mark_discontinuity();
                pending_fetch_gap = false;
            }
            static_cast<void>(demux.feed(result.value()));
            demux.flush(); // each TS segment is a self-contained unit
            note_input_progress();
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
