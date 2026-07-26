#pragma once

#include <vector>

#include "rtmp_server/protocol/commands/recorder_sink.hpp"

namespace rtmp_server::protocol::commands {

// Fans one CommandSession media hook out to several RecorderSinks.
//
// CommandSession holds a single non-owning RecorderSink* (see
// command_session.hpp set_recorder). Phase 6 adds a second consumer of the
// same media — the HLS Segmenter — alongside the FLV Recorder. Rather than
// widening CommandSession's interface (and touching the hot media path), the
// two are composed here and the tee is what gets injected. CommandSession is
// unchanged.
//
// Non-owning, exactly like set_recorder: the caller owns the sinks and must
// outlive this tee. Order is preserved (recording first, then packaging), so
// a slow or failing HLS sink can never prevent the recording from being
// written.
class TeeRecorderSink final : public RecorderSink {
public:
    TeeRecorderSink() = default;

    // Null sinks are ignored, so callers can add optional components
    // without branching at every call site.
    void add(RecorderSink* sink) {
        if (sink != nullptr) sinks_.push_back(sink);
    }

    [[nodiscard]] std::size_t size() const noexcept { return sinks_.size(); }

    void on_audio(const chunk::RtmpMessage& message) override {
        for (auto* sink : sinks_) sink->on_audio(message);
    }
    void on_video(const chunk::RtmpMessage& message) override {
        for (auto* sink : sinks_) sink->on_video(message);
    }
    void on_metadata(const chunk::RtmpMessage& message) override {
        for (auto* sink : sinks_) sink->on_metadata(message);
    }
    void finalize() override {
        // Every sink is finalized even if an earlier one misbehaves; all
        // implementations are required to be idempotent and non-throwing.
        for (auto* sink : sinks_) sink->finalize();
    }

private:
    std::vector<RecorderSink*> sinks_;
};

} // namespace rtmp_server::protocol::commands
