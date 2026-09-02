#include "rtmp_server/hls/stream_sink.hpp"

#include <utility>

namespace rtmp_server::hls {

StreamSink::StreamSink(std::shared_ptr<SegmentStore> store, SegmenterConfig config)
    : store_(std::move(store)),
      segmenter_([store = store_](SegmentPtr segment) { store->add_segment(std::move(segment)); },
                 config) {
    // Low-Latency HLS: each partial segment reaches the store the moment it
    // is cut, which is what lets a blocked playlist reload be released a part
    // at a time instead of a segment at a time.
    if (config.part_target_duration.count() > 0) {
        segmenter_.set_part_callback(
            [store = store_](PartPtr part) { store->add_part(std::move(part)); });
    }
    // A reconnect starts a fresh media sequence. Existing in-flight HTTP
    // responses retain their shared segment bytes even after this reset.
    store_->clear();
}

StreamSink::~StreamSink() { finalize(); }

void StreamSink::on_metadata(const protocol::chunk::RtmpMessage& message) {
    if (!finalized_) segmenter_.on_metadata(message);
}

void StreamSink::on_audio(const protocol::chunk::RtmpMessage& message) {
    if (!finalized_) segmenter_.on_audio(message);
}

void StreamSink::on_video(const protocol::chunk::RtmpMessage& message) {
    if (!finalized_) segmenter_.on_video(message);
}

void StreamSink::finalize() {
    if (finalized_) return;
    finalized_ = true;
    segmenter_.finalize();
    store_->mark_ended();
}

} // namespace rtmp_server::hls
