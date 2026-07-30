#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/transcoding/native/frame.hpp"

namespace rtmp_server::transcoding::native {

// Wraps an openh264 decoder. Input is Annex B (start-code prefixed) H.264 —
// convert AVCC samples with media::h264::avcc_to_annexb first. Output is I420,
// copied into a caller-owned YuvFrame so the pipeline can reuse buffers.
//
// openh264 is a delay-free CPU H.264 decoder (BSD licensed), which keeps the
// whole transcode path software-only and free of any FFmpeg dependency.
class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();
    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    [[nodiscard]] core::Result<void> initialize();

    // Feeds one Annex B access unit. On success `produced` is true when a full
    // frame was decoded into `out` (some access units — e.g. parameter sets
    // only — decode nothing). `pts_90k` is stamped onto the produced frame.
    [[nodiscard]] core::Result<void> decode(std::span<const std::byte> annexb, std::int64_t pts_90k,
                                            YuvFrame& out, bool& produced);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rtmp_server::transcoding::native
