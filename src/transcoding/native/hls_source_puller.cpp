#include "rtmp_server/transcoding/native/hls_source_puller.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

#include "rtmp_server/media/aac/adts.hpp"
#include "rtmp_server/media/h264/avc.hpp"
#include "rtmp_server/observability/logger.hpp"
#include "rtmp_server/transcoding/native/hls_playlist.hpp"
#include "rtmp_server/transcoding/native/paced_segment_publisher.hpp"
#include "rtmp_server/transcoding/native/rtmp_source_client.hpp"

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

bool is_rtmp_source(std::string_view url) { return url.starts_with("rtmp://"); }

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
        // URL, so retain it for every subsequent poll and parse.
        media_url_out = effective_source_url;
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
    const bool rtmp_source = is_rtmp_source(source_url_);
    if (!rtmp_source && !resolve_source(http, media_url, raw_ts, detail)) {
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
    hls::SegmenterConfig base_segmenter_config;
    base_segmenter_config.target_duration = std::chrono::milliseconds(1000);
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
        publisher_config.startup_buffer = resuming ? std::chrono::seconds(10)
                                                   : std::chrono::seconds(30);
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
    std::optional<core::Error> pipeline_error;
    demux.set_video_handler([&](std::span<const std::byte> annexb, std::uint64_t pts,
                                std::uint64_t dts, bool keyframe) {
        if (pipeline_error) return;
        auto result = transcoder.on_video(annexb, static_cast<std::int64_t>(pts),
                                          static_cast<std::int64_t>(dts), keyframe);
        if (!result) pipeline_error = result.error();
    });
    demux.set_audio_handler([&](std::span<const std::byte> adts, std::uint64_t pts) {
        if (pipeline_error) return;
        auto result = transcoder.on_audio(adts, static_cast<std::int64_t>(pts));
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

    if (rtmp_source) {
        media::h264::AvcDecoderConfig video_config;
        std::optional<media::aac::AudioSpecificConfig> audio_config;
        RtmpTimestampUnwrapper video_clock;
        RtmpTimestampUnwrapper audio_clock;
        RtmpSourceClient client(source_url_);

        auto result = client.run(
            [this] { return running_.load(); },
            [&](const protocol::chunk::RtmpMessage& message) -> core::Result<void> {
                using protocol::chunk::MessageTypeId;
                const auto type = static_cast<MessageTypeId>(message.message_type_id);
                if (type == MessageTypeId::Video) {
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
                        return core::Error(core::ErrorCode::MalformedChunk,
                                           core::ErrorCategory::Protocol,
                                           "unsupported RTMP AVC packet type");
                    }
                    if (!video_config.valid()) {
                        return core::Error(core::ErrorCode::InvalidStateTransition,
                                           core::ErrorCategory::Protocol,
                                           "RTMP video frame arrived before its AVC sequence header");
                    }
                    std::vector<std::byte> annexb;
                    auto converted = media::h264::avcc_to_annexb(
                        tag.value().body, video_config, tag.value().is_keyframe, annexb);
                    if (!converted) return converted.error();
                    const std::uint64_t dts_ms = video_clock.unwrap(message.timestamp);
                    const std::int64_t dts_90k = static_cast<std::int64_t>(dts_ms * 90);
                    const std::int64_t pts_90k =
                        dts_90k + static_cast<std::int64_t>(tag.value().composition_time_ms) * 90;
                    auto transcoded =
                        transcoder.on_video(annexb, pts_90k, dts_90k, tag.value().is_keyframe);
                    if (!transcoded) return transcoded.error();
                    note_input_progress();
                } else if (type == MessageTypeId::Audio) {
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
                    auto transcoded = transcoder.on_audio(adts, pts_90k);
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
        std::string effective_media_url;
        if (auto r = http.get(media_url, playlist_bytes, &effective_media_url); !r) {
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
            auto demuxed = demux.feed(result.value());
            if (!demuxed) pipeline_error = demuxed.error();
            demux.flush(); // each TS segment is a self-contained unit
            if (pipeline_error) {
                set_detail(pipeline_error->message());
                status_.store(PullerStatus::Error);
                break;
            }
            note_input_progress();
        }

        if (status_.load() == PullerStatus::Error) break;

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
