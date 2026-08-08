#include "rtmp_server/transcoding/native/hls_source_puller.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

#include "rtmp_server/observability/logger.hpp"
#include "rtmp_server/transcoding/native/hls_playlist.hpp"

namespace rtmp_server::transcoding::native {

namespace {
using observability::LogLevel;

bool ends_with(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The source's own segment cadence (10-15s) is bursty relative to what we
// publish (~4s per output segment): a single playlist poll can turn up
// several unseen source segments at once, which the decode/transcode loop
// races through in well under a second. Handing each finished output
// segment to the SegmentStore the instant Segmenter produces it would mirror
// that burst straight through to viewers -- several segments appear at once,
// then the live playlist sits static for 8-13s until the next source chunk
// arrives, which is exactly the stall/lag viewers see.
//
// This sits between Segmenter's on_segment callback and the store, and
// releases at most one queued segment every `interval` (matching the
// rendition's own target duration) rather than the instant it's ready. A
// burst of newly-transcoded segments queues up here and drains out at the
// steady rate viewers actually expect; the first segment after an idle
// period is released immediately rather than waiting a full interval, so
// steady-state latency isn't worsened, only bursts are smoothed. Because the
// source only ever exposes a few segments per playlist window, the queue is
// naturally bounded to a handful of entries -- there is no unbounded
// look-ahead to buffer against, only what the source has already published.
class PacedSegmentPublisher {
public:
    PacedSegmentPublisher(std::shared_ptr<hls::SegmentStore> store, std::chrono::milliseconds interval)
        : store_(std::move(store)), interval_(interval), next_publish_(std::chrono::steady_clock::now()) {
        thread_ = std::thread([this] { run(); });
    }

    ~PacedSegmentPublisher() { stop(); }

    void push(hls::SegmentPtr segment) {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(segment));
        wake_.notify_one();
    }

    // Releases everything still queued right now, bypassing the pacing
    // delay. Called when the puller itself is stopping (source ended,
    // shutdown) so the last few buffered seconds aren't stranded behind an
    // interval that will never fire again.
    void flush() {
        std::deque<hls::SegmentPtr> pending;
        {
            std::lock_guard lock(mutex_);
            pending.swap(queue_);
        }
        for (auto& segment : pending) store_->add_segment(std::move(segment));
    }

    void stop() {
        if (stopped_.exchange(true)) return;
        wake_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

    // Diagnostic only: lets the puller loop log how much is backed up here.
    [[nodiscard]] std::size_t queue_size() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

private:
    void run() {
        while (!stopped_.load()) {
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(200), [this] { return stopped_.load(); });
            if (stopped_.load()) break;
            if (queue_.empty()) continue;
            const auto now = std::chrono::steady_clock::now();
            // Only hold a segment back for pacing while the backlog is small
            // (a couple of segments -- exactly the kind of single-poll burst
            // this class exists to smooth). A source that's genuinely
            // publishing faster than we're releasing would otherwise build an
            // ever-growing backlog behind pacing that never repays itself --
            // each publish resets next_publish_ another interval_ into the
            // future regardless of how much is still queued, so a source
            // outrunning that fixed rate falls permanently further behind
            // live with every segment (this is exactly what was reported:
            // playback drifting further from the live edge over the session,
            // not just occasional stalls). Once the backlog passes
            // kMaxBacklog, drain immediately instead of waiting for
            // next_publish_, which caps how far behind live pacing can ever
            // push the stream and self-corrects any drift within a couple of
            // segments.
            if (queue_.size() <= kMaxBacklog && now < next_publish_) continue;
            auto segment = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();
            store_->add_segment(std::move(segment));
            next_publish_ = now + interval_;
        }
    }

    // A source's ~9-12s chunk decodes into roughly that many ~1s output
    // segments in one burst (hls_source_puller.cpp's run() -- 4x the burst
    // size the old 4s target produced), so the backlog allowance scales with
    // it: high enough that a typical single-poll burst still gets smoothed
    // at the steady 1-per-interval pace, low enough to still cap how far a
    // genuinely faster-than-expected source could push the stream behind
    // live before this drains it immediately instead of waiting.
    static constexpr std::size_t kMaxBacklog = 8;

    std::shared_ptr<hls::SegmentStore> store_;
    std::chrono::milliseconds interval_;
    std::chrono::steady_clock::time_point next_publish_;
    std::deque<hls::SegmentPtr> queue_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    std::atomic<bool> stopped_{false};
};

// Runs one dedicated worker thread draining a queue of decoded access units
// into `handler`. Video decode+scale+encode (fanned across its own thread
// pool for multi-rendition jobs, SourceTranscoder::start()) and audio
// decode+encode were both being invoked synchronously from the demuxer's
// callbacks on the single puller thread -- audio had to wait for whatever
// video work was in flight for a given TS packet before its own turn came,
// even though the two are otherwise independent pipelines with no reason to
// serialize against each other. Two of these (one for video work items, one
// for audio) let the demux thread just hand off a copy of each decoded
// access unit and move on immediately, so video's (typically heavier)
// per-frame work never blocks audio's.
//
// Item must be movable and cheap to default-construct. Everything queued
// here is copied out of the demuxer's buffers first (see the video/audio
// handlers below) since those buffers are only valid for the duration of the
// synchronous demux callback, not for whenever this worker gets around to
// the item.
template <typename Item>
class WorkerQueue {
public:
    using Handler = std::function<void(Item&&)>;

    explicit WorkerQueue(Handler handler) : handler_(std::move(handler)) {
        thread_ = std::thread([this] { run(); });
    }

    ~WorkerQueue() { stop(); }

    void push(Item item) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(item));
        wake_.notify_one();
    }

    // Drains everything already queued before actually stopping, so the
    // last few frames pushed before a shutdown aren't silently dropped.
    void stop() {
        if (stopped_.exchange(true)) return;
        wake_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

private:
    void run() {
        for (;;) {
            Item item;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stopped_.load() || !queue_.empty(); });
                if (queue_.empty()) {
                    if (stopped_.load()) break;
                    continue;
                }
                item = std::move(queue_.front());
                queue_.pop_front();
            }
            handler_(std::move(item));
        }
    }

    Handler handler_;
    std::deque<Item> queue_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    std::atomic<bool> stopped_{false};
};

struct VideoWorkItem {
    std::vector<std::byte> annexb;
    std::int64_t pts_90k = 0;
    std::int64_t dts_90k = 0;
    bool keyframe = false;
};

struct AudioWorkItem {
    std::vector<std::byte> adts;
    std::int64_t pts_90k = 0;
};

// Many IPTV/CDN panels answer an .m3u8-shaped URL with a redirect straight to
// a continuously-flowing raw MPEG-TS body (no playlist, connection never
// closes on its own) rather than an HLS playlist. Detect that from a small
// peek by checking for the TS sync byte at packet boundaries.
bool looks_like_raw_ts(std::span<const std::byte> data) {
    if (data.empty() || static_cast<unsigned char>(data[0]) != 0x47) return false;
    if (data.size() > 188 && static_cast<unsigned char>(data[188]) != 0x47) return false;
    return true;
}
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
        auto publisher = std::make_shared<PacedSegmentPublisher>(store, segmenter_config.target_duration);
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

    // See WorkerQueue's comment: video and audio decode/encode run on their
    // own dedicated threads instead of both blocking the single demux
    // thread. The Segmenter both eventually push into (via transcoder's own
    // output callbacks above) protects itself with its own mutex for exactly
    // this reason (hls/segmenter.hpp), and video_clock_set_ /
    // next_output_video_pts_90k_ (the only SourceTranscoder state on_audio
    // reads that on_video writes) are atomics for the same reason.
    WorkerQueue<VideoWorkItem> video_queue([&](VideoWorkItem&& item) {
        static_cast<void>(
            transcoder.on_video(item.annexb, item.pts_90k, item.dts_90k, item.keyframe));
    });
    WorkerQueue<AudioWorkItem> audio_queue([&](AudioWorkItem&& item) {
        static_cast<void>(transcoder.on_audio(item.adts, item.pts_90k));
    });

    media::ts::TsDemuxer demux;
    demux.set_video_handler([&](std::span<const std::byte> annexb, std::uint64_t pts,
                                std::uint64_t dts, bool keyframe) {
        // Copy out of the demuxer's buffer: annexb is only valid for this
        // callback's duration, but the video worker may not get to it until
        // well after this call returns.
        video_queue.push(VideoWorkItem{std::vector<std::byte>(annexb.begin(), annexb.end()),
                                       static_cast<std::int64_t>(pts),
                                       static_cast<std::int64_t>(dts), keyframe});
    });
    demux.set_audio_handler([&](std::span<const std::byte> adts, std::uint64_t pts) {
        audio_queue.push(
            AudioWorkItem{std::vector<std::byte>(adts.begin(), adts.end()), static_cast<std::int64_t>(pts)});
    });

    status_.store(PullerStatus::Running);
    set_detail("running");

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
                    set_detail("running");
                    static_cast<void>(demux.feed(chunk));
                    return true;
                });
            if (!running_.load()) break;

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

        // Drain whatever's still queued through the video/audio workers
        // before finalizing, so the last frames received aren't lost.
        video_queue.stop();
        audio_queue.stop();
        for (auto& segmenter : segmenters) segmenter->finalize();
        for (auto& publisher : publishers) publisher->flush();
        for (auto& rendition : renditions_) rendition.store->mark_ended();
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
        }

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

    // Drain whatever's still queued through the video/audio workers, then
    // the encoders, then close every rendition's final segment.
    video_queue.stop();
    audio_queue.stop();
    for (auto& segmenter : segmenters) segmenter->finalize();
    for (auto& publisher : publishers) publisher->flush();
    for (auto& rendition : renditions_) rendition.store->mark_ended();
    if (status_.load() == PullerStatus::Running) status_.store(PullerStatus::Stopped);
    running_.store(false);
}

} // namespace rtmp_server::transcoding::native
