#pragma once

#include <memory>

#include "rtmp_server/dash/segment_store.hpp"
#include "rtmp_server/dash/segmenter.hpp"

namespace rtmp_server::dash {

// One publisher-owned DASH packaging pipeline — the fMP4/DASH counterpart of
// hls::StreamSink, same shape: owns the Segmenter and feeds its output
// straight into the SegmentStore, so the caller only has to hold this one
// object per publisher.
class StreamSink final : public protocol::commands::RecorderSink {
public:
    StreamSink(std::shared_ptr<SegmentStore> store, SegmenterConfig config = {})
        : store_(std::move(store)),
          segmenter_([store = store_](SegmentPtr segment) { store->add_segment(std::move(segment)); },
                    [store = store_](InitSegmentPtr init) { store->set_init_segment(std::move(init)); },
                    std::move(config)) {
        store_->clear();
    }
    ~StreamSink() override { finalize(); }

    void on_metadata(const protocol::chunk::RtmpMessage& message) override {
        if (!finalized_) segmenter_.on_metadata(message);
    }
    void on_audio(const protocol::chunk::RtmpMessage& message) override {
        if (!finalized_) segmenter_.on_audio(message);
    }
    void on_video(const protocol::chunk::RtmpMessage& message) override {
        if (!finalized_) segmenter_.on_video(message);
    }
    void finalize() override {
        if (finalized_) return;
        finalized_ = true;
        segmenter_.finalize();
        store_->mark_ended();
    }

    [[nodiscard]] const SegmenterStats& stats() const noexcept { return segmenter_.stats(); }
    [[nodiscard]] const Segmenter& segmenter() const noexcept { return segmenter_; }

private:
    std::shared_ptr<SegmentStore> store_;
    Segmenter segmenter_;
    bool finalized_ = false;
};

} // namespace rtmp_server::dash
