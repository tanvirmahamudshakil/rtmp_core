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

// Pulls an HLS (or plain TS) source over HTTP, demuxes it, transcodes it once
// into every rendition, and writes each rendition's segments into its store —
// the FFmpeg-free source-transcode job, end to end. Runs on its own worker
// thread; the RTMP media path and HTTP serving threads are never touched.
class HlsSourcePuller {
public:
    HlsSourcePuller(std::string source_url, std::vector<PullerRendition> renditions,
                    std::uint32_t fps = 30);
    ~HlsSourcePuller();
    HlsSourcePuller(const HlsSourcePuller&) = delete;
    HlsSourcePuller& operator=(const HlsSourcePuller&) = delete;

    void start();
    void stop();

    [[nodiscard]] PullerStatus status() const noexcept { return status_.load(); }
    [[nodiscard]] std::string detail() const;

private:
    void run();
    [[nodiscard]] std::string resolve_media_url(HttpClient& http, std::string& detail_out);
    void set_detail(std::string detail);

    std::string source_url_;
    std::vector<PullerRendition> renditions_;
    std::uint32_t fps_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<PullerStatus> status_{PullerStatus::Stopped};

    mutable std::mutex detail_mutex_;
    std::string detail_;
};

} // namespace rtmp_server::transcoding::native
