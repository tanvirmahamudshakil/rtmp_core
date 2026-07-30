#include "rtmp_server/transcoding/native/hls_source_puller.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>

#include "rtmp_server/observability/logger.hpp"
#include "rtmp_server/transcoding/native/hls_playlist.hpp"

namespace rtmp_server::transcoding::native {

namespace {
using observability::LogLevel;

bool ends_with(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
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

std::string HlsSourcePuller::resolve_media_url(HttpClient& http, std::string& detail_out) {
    std::vector<std::byte> body;
    if (auto r = http.get(source_url_, body); !r) {
        detail_out = r.error().message();
        return {};
    }
    const std::string_view text(reinterpret_cast<const char*>(body.data()), body.size());
    if (!is_master_playlist(text)) return source_url_; // already a media playlist / TS
    const auto variants = parse_master_playlist(text, source_url_);
    const std::string chosen = select_variant(variants, 0);
    if (chosen.empty()) detail_out = "master playlist has no variants";
    return chosen;
}

void HlsSourcePuller::run() {
    HttpClient http;

    std::string detail;
    const std::string media_url = resolve_media_url(http, detail);
    if (media_url.empty()) {
        set_detail(detail.empty() ? "could not resolve source playlist" : detail);
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
    segmenters.reserve(renditions_.size());
    feeds.reserve(renditions_.size());
    for (auto& rendition : renditions_) {
        auto store = rendition.store;
        auto segmenter = std::make_unique<hls::Segmenter>(
            [store](hls::SegmentPtr segment) { store->add_segment(std::move(segment)); });
        feeds.push_back(std::make_unique<hls::RenditionFeed>(*segmenter));
        segmenters.push_back(std::move(segmenter));
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

    std::unordered_set<std::uint64_t> seen; // segment sequence numbers already pulled
    std::mutex sleep_mutex;
    std::condition_variable sleep_cv;
    int consecutive_errors = 0;

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

        for (const auto& segment : playlist.segments) {
            if (!running_.load()) break;
            if (seen.contains(segment.sequence)) continue;
            seen.insert(segment.sequence);

            std::vector<std::byte> ts_bytes;
            if (auto r = http.get(segment.uri, ts_bytes); !r) {
                RTMP_LOG(LogLevel::Warn, "source-transcoder", "segment fetch failed",
                         {{"url", segment.uri}, {"error", r.error().message()}});
                continue;
            }
            if (segment.discontinuity) {
                for (auto& feed : feeds) feed->mark_discontinuity();
            }
            static_cast<void>(demux.feed(ts_bytes));
            demux.flush(); // each TS segment is a self-contained unit
        }

        if (playlist.endlist) {
            set_detail("source ended");
            break; // VOD: nothing more to pull
        }

        // Live: wait roughly one target duration before refreshing the window.
        const auto wait = std::chrono::milliseconds(
            static_cast<long long>(std::max(1.0, playlist.target_duration) * 1000));
        std::unique_lock lock(sleep_mutex);
        sleep_cv.wait_for(lock, wait, [&] { return !running_.load(); });
    }

    // Drain encoders and close every rendition's final segment.
    for (auto& segmenter : segmenters) segmenter->finalize();
    for (auto& rendition : renditions_) rendition.store->mark_ended();
    if (status_.load() == PullerStatus::Running) status_.store(PullerStatus::Stopped);
    running_.store(false);
}

} // namespace rtmp_server::transcoding::native
