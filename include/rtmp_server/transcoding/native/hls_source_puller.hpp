#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "rtmp_server/hls/rendition_feed.hpp"
#include "rtmp_server/hls/segment_store.hpp"
#include "rtmp_server/hls/segmenter.hpp"
#include "rtmp_server/media/ts/ts_demuxer.hpp"
#include "rtmp_server/transcoding/native/http_client.hpp"
#include "rtmp_server/transcoding/native/source_transcoder.hpp"

namespace rtmp_server::transcoding::native {

// One output rendition: its ladder spec and the segment store its transcoded
// segments are written into (owned by the caller and registered with the HLS
// handler for serving).
struct PullerRendition {
    RenditionSpec spec;
    std::shared_ptr<hls::SegmentStore> store;
};

enum class PullerStatus { Starting, Running, Error, Stopped };

// Pulls native RTMP, HLS, or plain HTTP TS and transcodes it once
// into every rendition, and writes each rendition's segments into its store —
// the external-process-free source-transcode job, end to end. It runs on its
// own worker; the inbound RTMP and HTTP serving threads are never touched.
class HlsSourcePuller {
public:
    // `cpu_budget` is the number of cores this job may use for scale+encode
    // (0 = the whole machine). Passed straight to SourceTranscoder; see its
    // constructor for why a per-job share matters once more than one job
    // runs in the same process.
    // `pinned_cores` is forwarded to SourceTranscoder and is also applied to
    // this puller's own worker thread, which is what actually calls into the
    // transcoder (and, for a single-rendition job with no internal render
    // pool, is the thread that opens the encoder directly).
    HlsSourcePuller(std::string source_url, std::vector<PullerRendition> renditions,
                    std::uint32_t fps = 30, std::uint32_t cpu_budget = 0,
                    std::vector<unsigned> pinned_cores = {});
    ~HlsSourcePuller();
    HlsSourcePuller(const HlsSourcePuller&) = delete;
    HlsSourcePuller& operator=(const HlsSourcePuller&) = delete;

    void start();
    void stop();

    [[nodiscard]] PullerStatus status() const noexcept { return status_.load(); }
    [[nodiscard]] std::string detail() const;

private:
    void run();
    // Classifies an HTTP(S) source as a bounded HLS playlist (master or media, resolved
    // to media_url_out) or a continuous raw HTTP-TS live feed (raw_ts_out set,
    // media_url_out == source_url_) — the two shapes this puller can ingest.
    [[nodiscard]] bool resolve_source(HttpClient& http, std::string& media_url_out, bool& raw_ts_out,
                                      std::string& detail_out);
    void set_detail(std::string detail);

    std::string source_url_;
    std::vector<PullerRendition> renditions_;
    std::uint32_t fps_;
    std::uint32_t cpu_budget_ = 0;
    std::vector<unsigned> pinned_cores_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<PullerStatus> status_{PullerStatus::Stopped};

    mutable std::mutex detail_mutex_;
    std::string detail_;
};

} // namespace rtmp_server::transcoding::native
