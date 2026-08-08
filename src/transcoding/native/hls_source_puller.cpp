#include "rtmp_server/transcoding/native/hls_source_puller.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
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

private:
    void run() {
        while (!stopped_.load()) {
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(200), [this] { return stopped_.load(); });
            if (stopped_.load()) break;
            const auto now = std::chrono::steady_clock::now();
            if (queue_.empty() || now < next_publish_) continue;
            auto segment = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();
            store_->add_segment(std::move(segment));
            next_publish_ = now + interval_;
        }
    }

    std::shared_ptr<hls::SegmentStore> store_;
    std::chrono::milliseconds interval_;
    std::chrono::steady_clock::time_point next_publish_;
    std::deque<hls::SegmentPtr> queue_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    std::atomic<bool> stopped_{false};
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
    for (auto& rendition : renditions_) {
        auto store = rendition.store;
        auto publisher = std::make_shared<PacedSegmentPublisher>(store, hls::SegmenterConfig{}.target_duration);
        auto segmenter = std::make_unique<hls::Segmenter>(
            [publisher](hls::SegmentPtr segment) { publisher->push(std::move(segment)); });
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

        for (auto& segmenter : segmenters) segmenter->finalize();
        for (auto& publisher : publishers) publisher->flush();
        for (auto& rendition : renditions_) rendition.store->mark_ended();
        if (status_.load() == PullerStatus::Running) status_.store(PullerStatus::Stopped);
        running_.store(false);
        return;
    }

    std::unordered_set<std::uint64_t> seen; // segment sequence numbers already pulled

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
                continue;
            }
            if (segment->discontinuity) {
                for (auto& feed : feeds) feed->mark_discontinuity();
            }
            static_cast<void>(demux.feed(result.value()));
            demux.flush(); // each TS segment is a self-contained unit
        }

        if (playlist.endlist) {
            set_detail("source ended");
            break; // VOD: nothing more to pull
        }

        // Live: poll at half the target duration (standard HLS client practice).
        // Waiting a full target duration compounds with our own fetch/transcode
        // time, so a segment that becomes available just after we slept can sit
        // unpicked for close to two target durations — output then arrives in
        // bursts (all segments since last poll) followed by an idle gap, which
        // shows up as stall/lag on the viewer side. Polling twice as often keeps
        // that worst case near one target duration without materially raising
        // request load (the playlist itself is a few hundred bytes).
        const auto wait = std::chrono::milliseconds(
            static_cast<long long>(std::max(1.0, playlist.target_duration / 2.0) * 1000));
        std::unique_lock lock(sleep_mutex);
        sleep_cv.wait_for(lock, wait, [&] { return !running_.load(); });
    }

    // Drain encoders and close every rendition's final segment.
    for (auto& segmenter : segmenters) segmenter->finalize();
    for (auto& publisher : publishers) publisher->flush();
    for (auto& rendition : renditions_) rendition.store->mark_ended();
    if (status_.load() == PullerStatus::Running) status_.store(PullerStatus::Stopped);
    running_.store(false);
}

} // namespace rtmp_server::transcoding::native
