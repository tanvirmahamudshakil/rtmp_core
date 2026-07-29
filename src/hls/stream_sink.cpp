#include "rtmp_server/hls/stream_sink.hpp"

#include <utility>

namespace rtmp_server::hls {

StreamSink::StreamSink(std::shared_ptr<SegmentStore> store, SegmenterConfig config)
    : store_(std::move(store)),
      segmenter_([store = store_](SegmentPtr segment) { store->add_segment(std::move(segment)); },
                 std::move(config)) {
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
