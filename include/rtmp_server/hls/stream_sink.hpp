#pragma once

#include <memory>

#include "rtmp_server/hls/segment_store.hpp"
#include "rtmp_server/hls/segmenter.hpp"

namespace rtmp_server::hls {

// One publisher-owned HLS packaging pipeline. RTMP media is repackaged into
// immutable MPEG-TS segments without transcoding; SegmentStore makes those
// bytes concurrently readable by HTTP workers while this sink remains
// confined to the publisher's media thread.
class StreamSink final : public protocol::commands::RecorderSink {
public:
    StreamSink(std::shared_ptr<SegmentStore> store, SegmenterConfig config = {});
    ~StreamSink() override;

    void on_metadata(const protocol::chunk::RtmpMessage& message) override;
    void on_audio(const protocol::chunk::RtmpMessage& message) override;
    void on_video(const protocol::chunk::RtmpMessage& message) override;
    void finalize() override;

    [[nodiscard]] const SegmenterStats& stats() const noexcept { return segmenter_.stats(); }

private:
    std::shared_ptr<SegmentStore> store_;
    Segmenter segmenter_;
    bool finalized_ = false;
};

} // namespace rtmp_server::hls
