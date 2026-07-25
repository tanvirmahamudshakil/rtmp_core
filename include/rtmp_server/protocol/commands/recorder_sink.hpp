#pragma once

#include "rtmp_server/protocol/chunk/chunk_types.hpp"

namespace rtmp_server::protocol::commands {

// Abstract hook CommandSession routes a publishing stream's media into, so
// the protocol layer stays free of any link dependency on the recording
// layer (rtmp_server::recording depends on the protocol layer for AMF0/chunk
// types, so the reverse edge would be a cycle — see docs/flv-recording.md).
// The concrete implementation is recording::Recorder.
//
// Injected as a non-owning pointer (set_recorder), gated on publish state,
// mirroring how MediaIngest is wired (docs/media-ingest.md "Wiring into
// CommandSession"). Methods return void and never throw: a recorder must
// swallow disk errors internally (docs/rtmp_promot.md Phase 6 "disk failures
// do not crash server"), so the hot RTMP path can ignore recording outcomes.
class RecorderSink {
public:
    virtual ~RecorderSink() = default;

    virtual void on_audio(const chunk::RtmpMessage& message) = 0;
    virtual void on_video(const chunk::RtmpMessage& message) = 0;
    virtual void on_metadata(const chunk::RtmpMessage& message) = 0;

    // Flush pending writes, patch header placeholders, close the file. Called
    // from CommandSession's disconnect/deleteStream path — including on an
    // abrupt publisher disconnect — and must be safe to call more than once.
    virtual void finalize() = 0;
};

} // namespace rtmp_server::protocol::commands
